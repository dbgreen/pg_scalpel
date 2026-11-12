# scalpel-lib.janet: pure-Janet helpers over the pg_scalpel primitives.
# This is the source of the extension's auto-loaded prelude. tools/embed-prelude.janet
# bakes it into pgs_prelude.h at build time, and pgs_vm_ensure evaluates it at
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
    :char    (buffer/push-byte   b (if (< value 0) (+ value 0x100) value))
    :uchar   (buffer/push-byte   b value)
    :short   (buffer/push-uint16 b :le (if (< value 0) (+ value 0x10000) value))
    :ushort  (buffer/push-uint16 b :le value)
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
  # Multiply/add instead of bit shifts, since Janet bit ops are 32-bit. Doubles
  # are exact up to 2^53, so 4-byte and pointer-sized reads are exact.
  (var acc 0)
  (var mul 1)
  (for i 0 n
    (+= acc (* (in buf (+ off i)) mul))
    (set mul (* mul 256)))
  acc)

(defn dec-f64
  ``Reconstruct an IEEE-754 double from the 8 little-endian bytes of BUF at OFF.
  This Janet has a native float writer (buffer/push-float64) but no reader, and a
  Janet number IS an f64, so the value round-trips exactly. Handles zero and
  subnormals, +/-inf and NaN.``
  [buf off]
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

(defn dec-f32
  ``Reconstruct an IEEE-754 single from the 4 little-endian bytes of BUF at OFF,
  widened to a Janet f64 (exactly). Handles zero and subnormals, +/-inf and NaN.``
  [buf off]
  (def b (fn [i] (in buf (+ off i))))
  (def b3 (b 3)) (def b2 (b 2))
  (def sign (if (>= b3 128) -1 1))
  (def exp (+ (* (band b3 0x7f) 2) (brshift b2 7)))
  (def mant (+ (* (band b2 0x7f) 65536) (* (b 1) 256) (b 0)))
  (cond
    (= exp 0)   (* sign mant (math/pow 2 -149))
    (= exp 255) (if (= mant 0) (* sign math/inf) math/nan)
    (* sign (+ 1 (/ mant 8388608)) (math/pow 2 (- exp 127)))))

(defn read-struct
  ``Read scalar fields out of BUF per LAYOUT into a name->value table (e.g. to
  inspect an output struct a call filled in). Decodes integer/pointer/bool and
  float/double fields. long double, int128, bitfields, and aggregate
  (struct/union/array) fields have no single Janet-number value and are skipped.``
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
        :char    (let [u (in buf o)] (if (>= u 0x80) (- u 0x100) u))
        :uchar   (in buf o)
        :short   (let [u (dec-uint buf o 2)] (if (>= u 0x8000) (- u 0x10000) u))
        :ushort  (dec-uint buf o 2)
        :double  (dec-f64 buf o)
        :float   (dec-f32 buf o)
        nil))
    (when (not= v nil) (put out (f :name) v)))
  out)

# ---- script loader -------------------------------------------------------
# scalpel-load resolves script paths relative to this root (default:
# $HOME/pg_scalpel) so you can pull in example scripts by short name after
# connecting. If your checkout is elsewhere, (set scalpel-root "/path/to/it").
# It's a var because scalpel-load would keep a def's original value even if
# you redefined it.
(var scalpel-root (string (os/getenv "HOME") "/pg_scalpel"))

(defn scalpel-load
  ``Load a Janet script by path relative to scalpel-root ('.janet' optional) and
  return its final value. E.g. (def obs (scalpel-load "janet-lib/planner-observatory"))
  loads the planner observatory and captures its dispatch function.``
  [relpath]
  (def p (if (string/has-suffix? ".janet" relpath) relpath (string relpath ".janet")))
  (eval-string (slurp (string scalpel-root "/" p))))

:scalpel-lib-loaded
