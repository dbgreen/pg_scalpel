# How hooking works

`scalpel/hook` and the prelude's `hook-fn` are the one part of the tool worth
understanding beyond its signature. Two layers: the C primitive, and the Janet
wrapper that gives you on-enter / on-leave.

## `scalpel/hook` — the primitive (funchook)

```
(scalpel/hook target-addr detour-ptr) -> <trampoline pointer>
```

It rewrites the first instructions of `target-addr`, in place, in this backend's
memory, with a jump to `detour-ptr`. The instructions it overwrote are copied out
and relocated into a freshly allocated **trampoline** that ends by jumping back
into the rest of the original function. `scalpel/hook` returns the trampoline's
address: call it with the target's own signature and you've run the original.
`scalpel/unhook` writes the saved prologue bytes back.

The patch is copy-on-write and process-local — only this backend sees it, and it
dies with the backend. This is the Detours / Frida technique; capstone does the
length-disassembly so the relocation knows where each instruction ends.

`detour-ptr` has to be a real C function pointer with the target's ABI. That is
what `scalpel/closure` produces from a Janet function. So the raw move is: a
closure wraps your Janet as a C pointer, `hook` points the target at it, and the
trampoline is your way back to the original.

## `hook-fn` — the on-enter / on-leave wrapper

Doing closure + hook + trampoline by hand every time is tedious, so the prelude's
`hook-fn` does it. The whole implementation:

```janet
(defn hook-fn [target ret-type arg-types &named on-enter on-leave]
  (def a (addr-of target))
  (def cell @{:tramp nil})
  (def detour
    (scalpel/closure ret-type arg-types
      (fn [& args]
        (when on-enter (on-enter args))
        (def r (scalpel/call (cell :tramp) ret-type arg-types ;args))
        (cond
          (not (indexed? r)) r
          (= (get r 0) :ok)  (let [ret (get r 1)]
                               (if on-leave (on-leave args ret) ret))
          (error (get r 1))))))
  (put cell :tramp (scalpel/hook a detour))
  {:addr a :detour detour})
```

`cell` is a one-slot mutable table, and it exists for a chicken-and-egg reason:
the closure body needs the trampoline, but the trampoline doesn't exist until
`scalpel/hook` has installed the detour — and `hook` needs the detour, which has
already captured `cell`. So the closure closes over `cell`, and the trampoline is
dropped into it *after* the hook is installed (the `(put cell :tramp ...)` on the
second-to-last line). By the time anything calls the hook, `(cell :tramp)` is
filled in.

The closure body runs on every call to the target:

1. `(when on-enter (on-enter args))` — your on-enter callback, given the
   marshalled argument array. Observation only; its return is ignored.
2. `(scalpel/call (cell :tramp) ret-type arg-types ;args)` — call the **original**
   through the trampoline. `;args` splices the arg array back out into individual
   arguments. `r` is the guard's tagged result, `(:ok value)` or `(:error {...})`.
3. The `cond` decides what the hook hands back to its C caller:
   - not a tuple → pass it through (shouldn't normally happen);
   - `(:ok ret)` → if you gave an `on-leave`, return `(on-leave args ret)` so it
     can **replace** the result; otherwise return `ret` unchanged;
   - otherwise `(:error msg)` → `(error ...)` re-raises in Janet, which the
     closure dispatcher turns back into an `ereport`, so the original's throw
     passes through your hook instead of being swallowed.

So on-enter sees the arguments before the original runs; on-leave sees the
arguments and the return and can rewrite the return.

```janet
# count calls, change nothing
(def n @[0])
(hook-fn "add_path" :void [:pointer :pointer]
         :on-enter (fn [args] (put n 0 (+ 1 (n 0)))))

# observe/replace the return
(hook-fn "some_int_fn" :int [:pointer]
         :on-leave (fn [args ret] (if (< ret 0) 0 ret)))   # clamp negatives
```

## The siblings

- **`replace-fn`** — same shape, but it never calls the trampoline. Your Janet
  *is* the function; the original doesn't run. Use it to stand in rather than
  wrap.
- **`patch-jump` / `patch-return-int` / `patch-return-zero`** — skip funchook
  entirely and overwrite the prologue with a static stub (an absolute jump, or
  `mov eax,N; ret`). No trampoline, no callback, no way back except `unpatch`.

## Caveats

- The signature you give must match the target's real ABI. A wrong one builds a
  wrong call to the original and crashes the backend. Read the real one with
  `scalpel/sig`.
- `hook-fn` *wraps* — the original still runs. Use `replace-fn` to supplant it.
- Hooking a function already on the call stack is racy: the in-flight call
  finishes as the original.
- The patch is backend-global. Every caller in this backend hits your hook, not
  just the query you care about — so don't hook something the backend itself
  leans on (e.g. `palloc`, `strlen`).
