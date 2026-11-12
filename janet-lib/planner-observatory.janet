# planner-observatory.janet: live report of the planner's path candidates.
#
# Hooks add_path() (every candidate Path the optimizer costs and offers for a
# rel) and get_relation_info() (to name base rels). Decodes each Path's pathtype
# and costs straight out of backend memory. The field offsets it reads are pulled
# from the running binary's DWARF via scalpel/struct at arm time -- not hardcoded
# -- so the decode follows the struct layout across builds and PG versions. No
# EXPLAIN parsing. For a join candidate it also records the outer and inner paths
# it was built from, so the report shows each join's inputs as well as its total.
#
#   (def obs (scalpel-load "janet-lib/planner-observatory"))
#   (obs :arm)                # resolve the layouts, then install the hooks
#   ... run/EXPLAIN a query in THIS session (planner fires) ...
#   (obs :dump)               # grouped, cost-sorted candidate report (string)
#   (obs :disarm)             # remove hooks
#
# Join inputs need readable node names (SCALPEL_NODETAGS, below): the join, index,
# material and memoize paths are recognized by their node tag.
#
# Process-local: only this backend is instrumented, and it dies with the session.

(def PO-state @{:log @[] :relmap @{} :nodetags @{} :handles @[] :off @{}})

# The node structs whose field offsets we read. All are plain structs (or
# typedefs to one), so scalpel/struct resolves each and gives us its members.
(def PO-structs
  [:Path :RelOptInfo :Bitmapset :List
   :IndexPath :IndexOptInfo :JoinPath :MergePath :MaterialPath :MemoizePath])

# Two widths scalpel/struct can't hand us, and that are fixed 64-bit PG ABI
# anyway. scalpel/struct introspects STRUCTs; a ListCell is a union and a
# bitmapword a scalar typedef, so neither exposes member offsets to read. Both
# are stable infrastructure, not the node layouts that actually churn.
(def PO-CELL 8)   # sizeof(ListCell): union { void* ; int ; Oid ; ... } -> pointer-sized
(def PO-WORD 8)   # sizeof(bitmapword) on a 64-bit build

# --- offset lookup + typed reads at (ptr + offset) ----------------------------
# po-o returns the DWARF offset of a field, resolved at arm (see po-load-offsets).
# po-u32 / po-ptr / po-f64 read one field straight from live memory at base+off.
# A Path's rows/startup_cost/total_cost are C doubles; read-f64 (from the prelude,
# scalpel-lib's dec-f64) reconstructs those -- we don't reimplement the decode.
# Integers and pointers we decode here (multiply/add, since Janet bit ops are 32-bit).
(defn- po-dec-uint [buf off n]
  (var acc 0) (var mul 1)
  (for i 0 n (+= acc (* (in buf (+ off i)) mul)) (set mul (* mul 256)))
  acc)

(defn- po-o [s f]
  (or (get-in PO-state [:off s f])
      (errorf "planner-observatory: offset for %p.%p missing from DWARF layout" s f)))

(defn- po-u32 [base off] (po-dec-uint (peek-bytes (+ base off) 4) 0 4))
(defn- po-ptr [base off] (po-dec-uint (peek-bytes (+ base off) 8) 0 8))
(defn- po-f64 [base off] (read-f64 (+ base off)))

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
  (def bms (po-ptr reloptptr (po-o :RelOptInfo :relids)))      # relids -> Bitmapset*
  (def out @[])
  (when (not= bms 0)
    (def nwords (po-u32 bms (po-o :Bitmapset :nwords)))
    (def wbuf (peek-bytes (+ bms (po-o :Bitmapset :words)) (* nwords PO-WORD)))  # bytewise (avoids f64 loss)
    (for w 0 nwords
      (for byte 0 PO-WORD
        (def bv (in wbuf (+ (* w PO-WORD) byte)))
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
  (def tag (po-type-name (po-u32 p (po-o :Path :type))))         # Path.type: the node's own tag
  (def head
    (string/format "%s %s%s  rows=%.0f cost=%.2f..%.2f%s"
      (po-type-name (po-u32 p (po-o :Path :pathtype)))           # Path.pathtype: scan/join kind
      (if (= tag "IndexPath")
        (let [ii (po-ptr p (po-o :IndexPath :indexinfo))]        # IndexPath.indexinfo -> IndexOptInfo*
          (string (po-rel-name (po-u32 ii (po-o :IndexOptInfo :indexoid))) " on "))
        "")
      (po-rel-label (po-ptr p (po-o :Path :parent)))             # Path.parent -> RelOptInfo*
      (po-f64 p (po-o :Path :rows))
      (po-f64 p (po-o :Path :startup_cost))
      (po-f64 p (po-o :Path :total_cost))
      (if (not= 0 (po-ptr p (po-o :Path :param_info))) "  param" "")))
  (if (and (< depth 2) (or (= tag "MaterialPath") (= tag "MemoizePath")))
    (string head "\n" (string/repeat " " (* 2 (+ depth 1))) "<- "
            (po-node-str (po-ptr p (po-o (if (= tag "MemoizePath") :MemoizePath :MaterialPath) :subpath)) (+ depth 1)))
    head))

(defn- po-join-inputs [p tag]                   # JoinPath base: outerjoinpath / innerjoinpath
  (def sorted? (fn [field] (and (= tag "MergePath") (not= 0 (po-ptr p (po-o :MergePath field))))))
  @{:outer (string (po-node-str (po-ptr p (po-o :JoinPath :outerjoinpath)) 0) (if (sorted? :outersortkeys) "  +sort" ""))
    :inner (string (po-node-str (po-ptr p (po-o :JoinPath :innerjoinpath)) 0) (if (sorted? :innersortkeys) "  +sort" ""))})

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
    (def tag (po-type-name (po-u32 p (po-o :Path :type))))       # the Path node's own tag
    (array/push (PO-state :log)
      @{:inputs   (if (index-of tag ["NestPath" "MergePath" "HashPath"]) (po-join-inputs p tag))
        :rel      (po-rel-label rel)
        :relptr   rel
        :ptr      p
        :type     (po-type-name (po-u32 p (po-o :Path :pathtype)))
        :rows     (po-f64 p (po-o :Path :rows))
        :startup  (po-f64 p (po-o :Path :startup_cost))
        :total    (po-f64 p (po-o :Path :total_cost))
        :disabled (po-u32 p (po-o :Path :disabled_nodes))
        :param    (not= 0 (po-ptr p (po-o :Path :param_info)))   # param_info non-NULL => parameterized
        :pworkers (po-u32 p (po-o :Path :parallel_workers))})))

# set_cheapest(rel) runs after every candidate is in. By then rel->pathlist holds
# only the paths add_path kept, and cheapest_total_path is the winner. Capture
# both, keyed by the RelOptInfo pointer.
(defn- po-list-ptrs [listptr]           # read a pg List of pointers -> array of ints
  (def out @[])
  (when (not= listptr 0)
    (def len   (po-u32 listptr (po-o :List :length)))
    (def elems (po-ptr listptr (po-o :List :elements)))
    (when (and (> len 0) (not= elems 0))
      (def cbuf (peek-bytes elems (* len PO-CELL)))             # ListCell[]: ptr_value @0 of each cell
      (for i 0 len (array/push out (po-dec-uint cbuf (* i PO-CELL) 8)))))
  out)

# Classify by a (type|startup|total) signature read from the live pathlist, not by
# raw pointer. The planner pfrees dominated paths and reuses their addresses, so
# pointers collide across a run. Every path in pathlist is still live at
# set_cheapest, so there is no reuse there.
(defn- po-path-sig-at [p]
  (string (po-type-name (po-u32 p (po-o :Path :pathtype))) "|"
          (po-f64 p (po-o :Path :startup_cost)) "|"
          (po-f64 p (po-o :Path :total_cost))))

(defn- po-on-set-cheapest [args ret]    # on-leave: cheapest_total_path is set by now
  (def rel (ptr->int (get args 0)))
  (when rel
    (def kept @{})
    (each pp (po-list-ptrs (po-ptr rel (po-o :RelOptInfo :pathlist)))
      (when (not= pp 0) (put kept (po-path-sig-at pp) true)))
    (def wptr (po-ptr rel (po-o :RelOptInfo :cheapest_total_path)))
    (put (PO-state :rels) rel
      @{:kept kept :winner (if (= wptr 0) nil (po-path-sig-at wptr))}))
  ret)

# --- control ------------------------------------------------------------------
# Resolve every struct's field offsets once, from the running binary's DWARF.
# If debug info is missing scalpel/struct returns nil and we fail loudly here
# rather than decode against guessed offsets.
(defn- po-load-offsets []
  (def off @{})
  (each s PO-structs
    (def l (scalpel/struct (string s)))
    (unless l (errorf "planner-observatory: no DWARF layout for %s (debug info missing?)" s))
    (def by @{})
    (each f (l :fields) (put by (keyword (f :name)) (f :offset)))
    (put off s by))
  off)

(defn- po-arm []
  (put PO-state :nodetags (po-load-nodetags))
  (put PO-state :off (po-load-offsets))
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
  (string "armed: " (length (PO-state :handles)) " hooks; nodetags=" (length (PO-state :nodetags))
          "; structs=" (length (PO-state :off))))

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
