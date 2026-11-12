# patch.janet: live function hot-patching via inline detour.
#   (scalpel-load "janet-lib/patch")
#
# Built on scalpel/resolve + scalpel/call + scalpel/page-protect + scalpel/flush-icache.
# Overwrites a target function's prologue with a 14-byte absolute jump to a
# replacement, so every caller (fmgr dispatch or a direct C call) is
# rerouted. The change is process-local and copy-on-write. It only affects this
# backend and goes away when the backend exits.
#
#   (def P (scalpel-load "janet-lib/patch"))
#   (def h (patch-jump "int4pl" "int4mi"))   # reroute int4pl -> int4mi
#   ...exercise it from SQL in the SAME session...
#   (unpatch h)                              # restore the original bytes
#
# WARNING: superuser-only. Patching a function that is currently on
# the call stack is racy (the in-flight call finishes as the original).
# Inlined/optimized-away functions have no single body to patch.

(defn ptr->int
  "Numeric address of a pointer abstract (resolve returns these, not numbers)."
  [p]
  (let [s (string/format "%q" p)
        i (string/find "0x" s)
        e (string/find ">" s)]
    (when (and i e) (scan-number (string/slice s i e)))))

(defn- addr-of [x]
  # accept a symbol name (string), a raw address (number), or a pointer abstract
  (cond
    (string? x) (or (ptr->int (scalpel/resolve x)) (errorf "patch: cannot resolve %s" x))
    (number? x) x
    (or (ptr->int x) (errorf "patch: not an address: %q" x))))

(defn peek-bytes
  "Read N bytes from address ADDR into a fresh buffer (via memcpy)."
  [addr n]
  (def buf (buffer/new-filled n))
  (scalpel/call-raw (scalpel/resolve "memcpy") :pointer [:bytes :pointer :uint64] buf (addr-of addr) n)
  buf)

(defn read-f64
  "Read an IEEE-754 double (8 bytes) from memory at ADDR, as a Janet number (dec-f64)."
  [addr] (dec-f64 (peek-bytes addr 8) 0))

(defn read-f32
  "Read an IEEE-754 single (4 bytes) from memory at ADDR, as a Janet number (dec-f32)."
  [addr] (dec-f32 (peek-bytes addr 4) 0))

(defn poke-bytes
  ``Write BYTES into address ADDR: make the page writable+executable, copy,
  flush the instruction cache, then restore read+execute protection.
  page-protect raises an error if mprotect fails, so we never write to a
  read-only page (the guard catches the PG ereport).``
  [addr bytes]
  (def a (addr-of addr))
  (def n (length bytes))
  (scalpel/page-protect a n true)            # RWX (page-aligned in C)
  (scalpel/call-raw (scalpel/resolve "memcpy") :pointer [:pointer :bytes :uint64] a bytes n)
  (scalpel/flush-icache a n)
  (scalpel/page-protect a n false)           # restore R-X
  a)

(defn abs-jmp-bytes
  "14-byte position-independent absolute jump to DEST: FF 25 00000000 <abs64 LE>."
  [dest]
  (def s (buffer/new 14))
  (buffer/push-byte s 0xFF)
  (buffer/push-byte s 0x25)
  (buffer/push-uint32 s :le 0)
  (buffer/push-uint64 s :le dest)
  s)

(defn install
  ``Core primitive: overwrite TARGET (name/address/pointer) with STUB bytes,
  saving exactly (length STUB) original bytes. Returns a handle for unpatch.
  STUB replaces the function outright and must be self-contained. The original
  is never run.

  The overwrite is not atomic (a plain memcpy of the prologue). That is only
  safe because a Postgres backend is a single thread, so nothing can see a
  half-written prologue, and copy-on-write keeps other backends from seeing the
  write at all. Patching a function that is on the call stack is still racy.
  The in-flight call finishes as the original.``
  [target stub]
  (def a (addr-of target))
  (def n (length stub))
  (def orig (peek-bytes a n))            # save before we clobber
  (poke-bytes a stub)
  {:addr a :len n :orig orig})

(defn patch-jump
  ``Reroute TARGET to DEST via an absolute jump (each a name/address/pointer).
  Use when DEST is a real function with a matching ABI (e.g. int4pl->int4mi).``
  [target dest]
  (def d (addr-of dest))
  (merge (install target (abs-jmp-bytes d)) {:to d}))

# ---- Way 1: Janet-generated replacement stubs (no bytes supplied by hand) ----

(defn ret-int-bytes
  "mov eax, N; ret (B8 <imm32 LE> C3). Returns the int4/Datum constant N in RAX."
  [n]
  (def v (if (< n 0) (+ n 0x100000000) n))
  (def s (buffer/new 6))
  (buffer/push-byte s 0xB8)
  (buffer/push-uint32 s :le v)
  (buffer/push-byte s 0xC3)
  s)

(defn patch-return-int
  ``Force TARGET (an fmgr/int4-returning C function) to ignore its arguments and
  always return the int4 constant N. Useful for fault injection. The machine
  code is built from N, so no hand-written bytes. Returns a handle.``
  [target n]
  (merge (install target (ret-int-bytes n)) {:returns n}))

(defn patch-return-zero
  "xor eax,eax; ret (31 C0 C3). Forces TARGET to return 0 / false / NULL datum."
  [target]
  (def s (buffer/new 3))
  (buffer/push-byte s 0x31) (buffer/push-byte s 0xC0) (buffer/push-byte s 0xC3)
  (install target s))

(defn unpatch
  "Restore the original bytes saved in a patch handle (jump or stub)."
  [h]
  (poke-bytes (h :addr) (h :orig))
  (h :addr))

# ---- inline hooking with a trampoline (call the original) ----
# Backed by scalpel/hook (funchook). TARGET may be a name/address/pointer; DETOUR is
# a code pointer (e.g. from scalpel/closure). Returns the trampoline pointer. Call
# it with TARGET's signature to run the original function.

(defn hook
  ``Detour TARGET to DETOUR; returns the trampoline pointer to reach the original.``
  [target detour]
  (scalpel/hook (addr-of target) detour))

(defn unhook
  "Remove a hook installed on TARGET (name/address/pointer)."
  [target]
  (scalpel/unhook (addr-of target)))

# ---- pure replacement: original is never called ----
# replace-fn detours TARGET to a Janet function with the matching ABI. While the
# hook is installed the original body never runs. Use it to write the whole body
# in Janet instead of wrapping the existing one. RET-TYPE + ARG-TYPES describe
# the signature the same way as scalpel/closure. For fmgr functions, arg-types is [:pointer] (fcinfo) and the
# body reads args via (scalpel/getarg fcinfo n type).
#
#   (replace-fn "int4pl" :int [:pointer]
#     (fn [fcinfo]
#       (def a (scalpel/getarg fcinfo 0 :int))
#       (def b (scalpel/getarg fcinfo 1 :int))
#       (- a b)))                       # int4pl now subtracts

(defn replace-fn
  ``Replace TARGET entirely with FN (a Janet function). The original is never
  called, since the detour does not go through the trampoline. Returns a
  handle ({:addr :detour}); remove with (unhook target).``
  [target ret-type arg-types fn]
  (def a (addr-of target))
  (def detour (scalpel/closure ret-type arg-types fn))
  (scalpel/hook a detour)                  # trampoline returned but discarded
  {:addr a :detour detour})

# ---- onEnter/onLeave wrapping (like Frida's Interceptor.attach) ----
# hook-fn wraps TARGET so it still runs (via the trampoline) but you observe its
# args on entry and observe/replace its return on leave. RET-TYPE + ARG-TYPES
# describe the signature the same way as scalpel/closure and scalpel/call.
#   :on-enter (fn [args])        observe the arg array (marshalled Janet values)
#   :on-leave (fn [args ret])    the value it returns replaces the result.
#                                Return ret unchanged to pass through.
# The original is called raw, not through scalpel/call's subtransaction. A
# subtransaction per call breaks anything in the transaction machinery
# (hooking CommitTransaction crashed the backend), and an error raised by the
# original should propagate unchanged. on-leave doesn't run for a call that errors.

(defn hook-fn
  ``Wrap TARGET with onEnter/onLeave around a real call to the original.
  Returns a handle ({:addr :detour}); remove with (unhook target).``
  [target ret-type arg-types &named on-enter on-leave]
  (def a (addr-of target))
  (def cell @{:tramp nil})
  (def detour
    (scalpel/closure ret-type arg-types
      (fn [& args]
        (when on-enter (on-enter args))
        (def ret (get (scalpel/call-raw (cell :tramp) ret-type arg-types ;args) 1))
        (if on-leave (on-leave args ret) ret))))
  (put cell :tramp (scalpel/hook a detour))
  {:addr a :detour detour})

# ---- trace (frida-trace): hook a family of same-signature functions ----
# trace-log accumulates @{:fn :args :ret} records. You must pass the signature
# (ret-type arg-types). Every matched function is called through it, so it must
# be correct for all of them (e.g. fmgr functions are all :datum [:pointer]).

(def trace-log @[])
(defn trace-clear [] (array/clear trace-log))

(defn trace
  ``Hook every symbol matching PATTERN (scalpel/syms wildcard) with the given
  signature and log each call to trace-log. :stack true also records a
  backtrace per call. Returns a handle for untrace. The signature is applied
  to every match, so only trace functions that actually share it.``
  [pattern ret-type arg-types &named limit stack]
  (default limit 50)
  (def hooked @[])
  (each s (scalpel/syms pattern limit)
    (def nm (s :name))
    (when (try
            (do (hook-fn nm ret-type arg-types
                  :on-leave (fn [args ret]
                              (array/push trace-log
                                @{:fn nm :args args :ret ret
                                  :stack (if stack (scalpel/backtrace 12) nil)})
                              ret))
                true)
            ([_] false))
      (array/push hooked nm)))
  {:pattern pattern :hooked hooked})

(defn untrace
  "Remove all hooks installed by a trace handle."
  [h]
  (each nm (h :hooked) (try (scalpel/unhook (addr-of nm)) ([_] nil)))
  (length (h :hooked)))

:patch-lib-loaded
