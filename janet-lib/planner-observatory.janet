# planner-observatory.janet: live report of the planner's path candidates.
#
# Hooks add_path() (every candidate Path the optimizer considers for a rel) and
# get_relation_info() (to name base rels). Decodes each Path's pathtype and costs
# from backend memory with the pg_scalpel primitives. No EXPLAIN parsing.
# For a join candidate it also records the outer and inner paths it was built
# from, so the report shows each join's inputs as well as its total.
#
#   (def obs (scalpel-load "janet-lib/planner-observatory"))
#   (obs :arm)                # install the hooks
#   ... run/EXPLAIN a query in THIS session (planner fires) ...
#   (obs :dump)               # grouped, cost-sorted candidate report (string)
#   (obs :disarm)             # remove hooks
#
# Join inputs need readable node names (SCALPEL_NODETAGS, below): the join, index,
# material and memoize paths are recognized by their node tag.
#
# Process-local: only this backend is instrumented, and it dies with the session.

(def PO-state @{:log @[] :relmap @{} :nodetags @{} :handles @[]})

# --- IEEE-754 f64 decode (Cost/Cardinality are f64, which read-struct skips) ---
(defn- po-dec-uint [buf off n]
  (var acc 0) (var mul 1)
  (for i 0 n (+= acc (* (in buf (+ off i)) mul)) (set mul (* mul 256)))
  acc)

(defn- po-dec-f64 [buf off]
  (def b (fn [i] (in buf (+ off i))))
  (def b7 (b 7)) (def b6 (b 6))
  (def sign (if (>= b7 128) -1 1))
  (def exp (+ (* (band b7 0x7f) 16) (brshift b6 4)))
  (def mant (+ (* (band b6 0x0f) 281474976710656) (* (b 5) 1099511627776)
               (* (b 4) 4294967296) (* (b 3) 16777216) (* (b 2) 65536) (* (b 1) 256) (b 0)))
  (cond
    (= exp 0)    (* sign mant (math/pow 2 -1074))
    (= exp 2047) (if (= mant 0) (* sign math/inf) math/nan)
    (* sign (+ 1 (/ mant 4503599627370496)) (math/pow 2 (- exp 1023)))))

# --- NodeTag int -> name, from the build's generated header -------------------
(defn- po-load-nodetags []
  # NodeTag int -> name, parsed from the build's generated nodetags.h. Set
  # SCALPEL_NODETAGS to its path, e.g. "$(pg_config --includedir-server)/nodes/nodetags.h",
  # for readable node names. Without it, paths show as "Tag#N".
  (def path (os/getenv "SCALPEL_NODETAGS"))
  (def m @{})
  (when path
    (when-let [src (try (slurp path) ([_] nil))]
      (each line (string/split "\n" src)
        (when-let [c (peg/match ~(* (any :s) "T_" (<- (some (+ :w "_"))) (any :s) "=" (any :s) (<- (some :d))) line)]
          (put m (scan-number (get c 1)) (get c 0))))))
  m)

(defn- po-type-name [n] (or (get (PO-state :nodetags) n) (string "Tag#" n)))

# --- decode a RelOptInfo's relids Bitmapset into a set of RTE indexes ----------
(defn- po-relids [reloptptr]
  (def rbuf (peek-bytes reloptptr 16))          # RelOptInfo.relids @8 -> Bitmapset*
  (def bms (po-dec-uint rbuf 8 8))
  (def out @[])
  (when (not= bms 0)
    (def nwords (po-dec-uint (peek-bytes (+ bms 4) 4) 0 4))   # Bitmapset.nwords @4
    (def wbuf (peek-bytes (+ bms 8) (* nwords 8)))            # words @8, bytewise (avoids f64 loss)
    (for w 0 nwords
      (for byte 0 8
        (def bv (in wbuf (+ (* w 8) byte)))
        (for bit 0 8
          (when (not= 0 (band bv (blshift 1 bit)))
            (array/push out (+ (* w 64) (* byte 8) bit)))))))
  out)

# Base rels are "name{relid}" so a self-join's two sides stay apart (t{1}, t{2});
# join rels are just their relid set.
(defn- po-rel-label [reloptptr]
  (def ids (string "{" (string/join (map string (po-relids reloptptr)) ",") "}"))
  (if-let [nm (get (PO-state :relmap) reloptptr)] (string nm ids) ids))

# --- describe a path's inputs ---------------------------------------------------
# Children are decoded when the join is offered to add_path: the planner frees
# dominated paths later, so the pointers are only good now.
(defn- po-rel-name [oid]
  (or (get (PO-state :names) oid)
      (let [r (scalpel/call (scalpel/resolve "get_rel_name") :cstring [:uint] oid)
            nm (if (and (indexed? r) (= (get r 0) :ok) (string? (get r 1))) (get r 1) (string "oid " oid))]
        (put (PO-state :names) oid nm)
        nm)))

(defn- po-node-str [p depth]
  (def buf (peek-bytes p 80))
  (def tag (po-type-name (po-dec-uint buf 0 4)))         # path node tag @0 (IndexPath, HashPath, ...)
  (def head
    (string/format "%s %s%s  rows=%.0f cost=%.2f..%.2f%s"
      (po-type-name (po-dec-uint buf 4 4))
      (if (= tag "IndexPath")                            # IndexPath.indexinfo @80 -> IndexOptInfo.indexoid @4
        (string (po-rel-name (po-dec-uint (peek-bytes (po-dec-uint (peek-bytes p 88) 80 8) 8) 4 4)) " on ")
        "")
      (po-rel-label (po-dec-uint buf 8 8))               # parent @8
      (po-dec-f64 buf 40) (po-dec-f64 buf 56) (po-dec-f64 buf 64)
      (if (not= 0 (po-dec-uint buf 24 8)) "  param" "")))
  (if (and (< depth 2) (or (= tag "MaterialPath") (= tag "MemoizePath")))
    (string head "\n" (string/repeat " " (* 2 (+ depth 1))) "<- "
            (po-node-str (po-dec-uint (peek-bytes p 88) 80 8) (+ depth 1)))   # subpath @80
    head))

(defn- po-join-inputs [p tag]                   # JoinPath: outerjoinpath @88, innerjoinpath @96
  (def jbuf (peek-bytes p 144))
  (def sorted? (fn [off] (and (= tag "MergePath") (not= 0 (po-dec-uint jbuf off 8)))))
  @{:outer (string (po-node-str (po-dec-uint jbuf 88 8) 0) (if (sorted? 120) "  +sort" ""))   # outersortkeys @120
    :inner (string (po-node-str (po-dec-uint jbuf 96 8) 0) (if (sorted? 128) "  +sort" ""))}) # innersortkeys @128

# --- hook callbacks -----------------------------------------------------------
(defn- po-on-get-relation-info [args]           # (root, Oid, inhparent, rel)
  (def oid (get args 1))
  (def rel (ptr->int (get args 3)))
  (when (and oid rel (> oid 0))
    (def r (scalpel/call (scalpel/resolve "get_rel_name") :cstring [:uint] oid))
    (when (and (indexed? r) (= (get r 0) :ok) (string? (get r 1)))
      (put (PO-state :relmap) rel (get r 1)))))

(defn- po-on-add-path [args]                    # (parent_rel, new_path)
  (def rel (ptr->int (get args 0)))
  (def p   (ptr->int (get args 1)))
  (when (and rel p)
    (def buf (peek-bytes p 80))                 # Path is 80 bytes through total_cost/pathkeys
    (def tag (po-type-name (po-dec-uint buf 0 4)))
    (array/push (PO-state :log)
      @{:inputs   (if (index-of tag ["NestPath" "MergePath" "HashPath"]) (po-join-inputs p tag))
        :rel      (po-rel-label rel)
        :relptr   rel
        :ptr      p
        :type     (po-type-name (po-dec-uint buf 4 4))   # pathtype @4
        :rows     (po-dec-f64 buf 40)                    # rows @40
        :startup  (po-dec-f64 buf 56)                    # startup_cost @56
        :total    (po-dec-f64 buf 64)                    # total_cost @64
        :disabled (po-dec-uint buf 48 4)                 # disabled_nodes @48
        :param    (not= 0 (po-dec-uint buf 24 8))        # param_info @24 (non-NULL => parameterized)
        :pworkers (po-dec-uint buf 36 4)})))             # parallel_workers @36

# set_cheapest(rel) runs after every candidate is in. By then rel->pathlist holds
# only the paths add_path kept, and cheapest_total_path is the winner. Capture
# both, keyed by the RelOptInfo pointer.
(defn- po-list-ptrs [listptr]           # read a pg List of pointers -> array of ints
  (def out @[])
  (when (not= listptr 0)
    (def hdr (peek-bytes listptr 24))                    # length @4, elements @16
    (def len (po-dec-uint hdr 4 4))
    (def elems (po-dec-uint hdr 16 8))
    (when (and (> len 0) (not= elems 0))
      (def cbuf (peek-bytes elems (* len 8)))            # ListCell[] , ptr_value @0, 8 bytes each
      (for i 0 len (array/push out (po-dec-uint cbuf (* i 8) 8)))))
  out)

# Classify by a (type|startup|total) signature read from the live pathlist, not by
# raw pointer. The planner pfrees dominated paths and reuses their addresses, so
# pointers collide across a run. Every path in pathlist is still live at
# set_cheapest, so there is no reuse there.
(defn- po-path-sig-at [pbuf]
  (string (po-type-name (po-dec-uint pbuf 4 4)) "|" (po-dec-f64 pbuf 56) "|" (po-dec-f64 pbuf 64)))

(defn- po-on-set-cheapest [args ret]    # on-leave: cheapest_total_path is set by now
  (def rel (ptr->int (get args 0)))
  (when rel
    (def rbuf (peek-bytes rel 88))                       # pathlist @48, cheapest_total_path @80
    (def kept @{})
    (each pp (po-list-ptrs (po-dec-uint rbuf 48 8))
      (when (not= pp 0) (put kept (po-path-sig-at (peek-bytes pp 80)) true)))
    (def wptr (po-dec-uint rbuf 80 8))
    (put (PO-state :rels) rel
      @{:kept kept :winner (if (= wptr 0) nil (po-path-sig-at (peek-bytes wptr 80)))}))
  ret)

# --- control ------------------------------------------------------------------
(defn- po-arm []
  (put PO-state :nodetags (po-load-nodetags))
  (put PO-state :log @[])
  (put PO-state :relmap @{})
  (put PO-state :names @{})
  (put PO-state :rels @{})
  (put PO-state :handles @[])
  (array/push (PO-state :handles)
    (hook-fn "get_relation_info" :void [:pointer :uint :bool :pointer] :on-enter po-on-get-relation-info))
  (array/push (PO-state :handles)
    (hook-fn "add_path" :void [:pointer :pointer] :on-enter po-on-add-path))
  (array/push (PO-state :handles)
    (hook-fn "set_cheapest" :void [:pointer] :on-leave po-on-set-cheapest))
  (string "armed: " (length (PO-state :handles)) " hooks; nodetags=" (length (PO-state :nodetags))))

(defn- po-disarm []
  (each nm ["add_path" "get_relation_info" "set_cheapest"]
    (try (scalpel/unhook (scalpel/resolve nm)) ([_] nil)))
  (array/clear (PO-state :handles))
  "disarmed")

(defn- po-dump []
  (def groups @{})
  (each r (PO-state :log)
    (put groups (r :rel) (array/push (or (get groups (r :rel)) @[]) r)))
  (def out @"")
  (buffer/push out (string "planner candidates: " (length (PO-state :log))
                           " paths over " (length groups) " rel(s)\n"))
  (buffer/push out (string "  legend:  * winner (cheapest_total_path)   + survived (kept in pathlist)"
                           "   x rejected (dominated/pruned)   param = per-execution cost\n"))
  (each rel (sort (keys groups))
    (def cands (get groups rel))
    (def relinfo (get (PO-state :rels) ((first cands) :relptr)))
    (def kept-sigs (and relinfo (relinfo :kept)))
    (defn sig-of [r] (string (r :type) "|" (r :startup) "|" (r :total)))
    (defn kept? [r] (if relinfo (truthy? (get kept-sigs (sig-of r))) true))
    # Only mark a winner when set_cheapest reported one (base and join rels).
    # Upper/final rels pick their path elsewhere, so we don't guess.
    (def wsig (and relinfo (relinfo :winner)))
    (def kept (sort-by (fn [r] (r :total)) (filter kept? cands)))
    (def rej  (sort-by (fn [r] (r :total)) (filter (fn [r] (not (kept? r))) cands)))
    (def rows (array/concat @[] kept rej))
    (buffer/push out
      (string "\n  " rel "  (" (length cands) " considered"
              (if relinfo (string ", " (length kept) " kept, " (length rej) " rejected") ", no set_cheapest")
              ")\n"))
    (var winner-shown false)
    (each r rows
      (def mark (cond (and wsig (not winner-shown) (= (sig-of r) wsig)) (do (set winner-shown true) "*")
                      (kept? r) "+" "x"))
      (buffer/push out
        (string/format "    %s %-16s rows=%-9.0f cost=%9.2f..%-9.2f%s%s%s\n"
          mark (r :type) (r :rows) (r :startup) (r :total)
          (if (r :param) "  param" "")
          (if (> (r :disabled) 0) (string "  disabled=" (r :disabled)) "")
          (if (> (r :pworkers) 0) (string "  parallel/" (r :pworkers)) "")))
      (when-let [ins (r :inputs)]
        (each side [:outer :inner]
          (buffer/push out (string/format "          %-5s %s\n" (string side)
                                          (string/replace-all "\n" "\n            " (ins side))))))))
  (string out))

# dispatch table (scalpel-load returns this; call as (obs :arm) / (obs :dump) / ...)
(fn planner-observatory [cmd & _]
  (case cmd
    :arm    (po-arm)
    :disarm (po-disarm)
    :dump   (po-dump)
    :clear  (do (put PO-state :log @[]) (put PO-state :relmap @{}) (put PO-state :rels @{}) "cleared")
    :log    (PO-state :log)
    (errorf "planner-observatory: unknown cmd %p" cmd)))
