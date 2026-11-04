# scalpel-lib.janet — pure-Janet helpers over the pg_scalpel primitives.
# This is the SOURCE of the extension's auto-loaded prelude: tools/embed-prelude.janet
# bakes it into ext/pgs_prelude.h at build time, and pgs_vm_ensure evaluates it at
# VM init, so make-struct/pack-struct/read-struct are present in every REPL with no
# manual load. (After editing this file you must rebuild; to hot-reload without a
# rebuild during dev, (eval-string (slurp ".../janet-lib/scalpel-lib.janet")).)
#
# Turns a (scalpel/struct ...) layout into a ready struct-by-pointer buffer (and back).
# Scalar fields only; pointer/cstring/datum fields take an integer address
# (0 = NULL). Janet numbers are f64, so values/addresses above 2^53 lose precision.

(defn- enc-field
  "Encode one scalar field value to a little-endian buffer per its :call keyword."
  [call value]
  (def b (buffer/new 8))
  (case call
    :int     (buffer/push-uint32 b :le (if (< value 0) (+ value 0x100000000) value))
    :uint    (buffer/push-uint32 b :le value)
    :int64   (buffer/push-uint64 b :le value)
    :uint64  (buffer/push-uint64 b :le value)
    :datum   (buffer/push-uint64 b :le value)
    :pointer (buffer/push-uint64 b :le value)
    :cstring (buffer/push-uint64 b :le value)
    :bool    (buffer/push-byte   b (if value 1 0))
    :double  (buffer/push-float64 b :le value)
    :float   (buffer/push-float32 b :le value)
    (errorf "pack-struct: unhandled field call-type %p (set this field manually)" call))
  b)

(defn pack-struct
  ``Build a byte buffer for a struct LAYOUT (as returned by scalpel/struct), writing
  each field named in VALUES (name -> value) at its :offset. Unset fields stay
  zero. Returns a buffer you can pass to scalpel/call as a :bytes argument.``
  [layout values]
  (def buf (buffer/new-filled (layout :size)))
  (each f (layout :fields)
    (def v (get values (f :name)))
    (when (not= v nil)
      (buffer/blit buf (enc-field (f :call) v) (f :offset))))
  buf)

(defn make-struct
  "Resolve struct NAME via scalpel/struct and pack VALUES into a fresh buffer."
  [name values]
  (def layout (scalpel/struct name))
  (unless layout (errorf "make-struct: unknown struct %s" name))
  (pack-struct layout values))

(defn- dec-uint [buf off n]
  # multiply/add (not bit-shift): Janet bit ops are 32-bit; this stays in the
  # float domain (exact up to 2^53, so 4-byte and pointer-sized reads are exact)
  (var acc 0)
  (var mul 1)
  (for i 0 n
    (+= acc (* (in buf (+ off i)) mul))
    (set mul (* mul 256)))
  acc)

(defn read-struct
  ``Read integer/pointer/bool fields out of BUF per LAYOUT into a name->value
  table (e.g. to inspect an output struct a call filled in). float/double and
  aggregate fields are skipped.``
  [layout buf]
  (def out @{})
  (each f (layout :fields)
    (def o (f :offset))
    (def v
      (case (f :call)
        :int     (let [u (dec-uint buf o 4)]
                   (if (>= u 0x80000000) (- u 0x100000000) u))
        :uint    (dec-uint buf o 4)
        :int64   (dec-uint buf o 8)
        :uint64  (dec-uint buf o 8)
        :datum   (dec-uint buf o 8)
        :pointer (dec-uint buf o 8)
        :cstring (dec-uint buf o 8)
        :bool    (not= 0 (in buf o))
        nil))
    (when (not= v nil) (put out (f :name) v)))
  out)

# ---- script loader -------------------------------------------------------
# scalpel-load resolves script paths relative to this root (default:
# $HOME/pg_scalpel) so you can pull in example/instrument scripts by short name
# after connecting. Override by redefining scalpel-root if your checkout is elsewhere.
(def scalpel-root (string (os/getenv "HOME") "/pg_scalpel"))

(defn scalpel-load
  ``Load a Janet script by path relative to scalpel-root ('.janet' optional) and
  return its final value. E.g. (scalpel-load "janet-lib/fuzz") loads the fuzz engine;
  (def t (scalpel-load "fuzz/targets/numeric")) loads a target and captures its table.``
  [relpath]
  (def p (if (string/has-suffix? ".janet" relpath) relpath (string relpath ".janet")))
  (eval-string (slurp (string scalpel-root "/" p))))

:scalpel-lib-loaded
