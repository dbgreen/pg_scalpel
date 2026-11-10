# How hooking works

There are two layers: the C primitive `scalpel/hook`, and `hook-fn`, the Janet
wrapper in the prelude that gives you `on-enter` and `on-leave`.

## scalpel/hook

```
(scalpel/hook target-addr detour-ptr) -> <trampoline pointer>
```

`scalpel/hook` uses funchook. It overwrites the first few instructions of
`target-addr`, in this backend's memory, with a jump to `detour-ptr`. The
instructions it overwrote get copied into a new block of code called a
trampoline, fixed up so they still work at the new address, and followed by a
jump back into the rest of the original function. `scalpel/hook` returns the
trampoline's address. Call it with the target's signature and the original
function runs. `scalpel/unhook` writes the saved bytes back.

The patch only affects this backend. Other backends don't see it, and it goes
away when the backend exits. It's the same technique Detours and Frida use.
capstone is what tells funchook where each instruction ends so it knows how much
to copy.

`detour-ptr` has to be a real C function pointer with the same calling
convention as the target. That's what `scalpel/closure` makes out of a Janet
function. So doing it by hand looks like this: make a closure from your Janet
function, point the target at it with `hook`, and use the trampoline when you
want to call the original.

## hook-fn

Doing closure, hook, and trampoline yourself every time gets tedious, so the
prelude's `hook-fn` does it for you. This is all of it:

```janet
(defn hook-fn [target ret-type arg-types &named on-enter on-leave]
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
```

`cell` is there because of an ordering problem. The closure needs the trampoline
to call the original. But the trampoline doesn't exist until `scalpel/hook` runs,
and `scalpel/hook` needs the closure. So the closure holds on to `cell`, an empty
table, and the trampoline gets put into `cell` after the hook is installed. By the
time anything calls the hooked function, `(cell :tramp)` is set.

Every call to the target runs the closure:

1. `(when on-enter (on-enter args))` calls your `on-enter` with the arguments.
   Its return value is ignored.
2. `(scalpel/call-raw (cell :tramp) ret-type arg-types ;args)` calls the original
   through the trampoline. `;args` splices the argument array back into separate
   arguments. `call-raw` returns `(:ok value)` and `get ... 1` pulls out the
   value.

   It uses `call-raw` and not `scalpel/call` on purpose. `scalpel/call` would
   start a subtransaction on every call to the hooked function, and that breaks
   anything that touches the transaction machinery. With `call-raw`, if the
   original raises an error, that error goes straight through your hook to
   whoever called the function.
3. If you passed `on-leave`, the hook returns `(on-leave args ret)`, so you can
   change the result. Otherwise it returns `ret`. If the original raised an
   error, this step never runs, so `on-leave` only sees calls that returned.

```janet
# count calls, change nothing
(def n @[0])
(hook-fn "add_path" :void [:pointer :pointer]
         :on-enter (fn [args] (put n 0 (+ 1 (n 0)))))

# change the return value
(hook-fn "some_int_fn" :int [:pointer]
         :on-leave (fn [args ret] (if (< ret 0) 0 ret)))   # clamp negatives
```

## Related functions

- `replace-fn` works the same way but never calls the trampoline. Your function
  replaces the original, which doesn't run at all.
- `patch-jump`, `patch-return-int`, and `patch-return-zero` don't use funchook.
  They overwrite the start of the function with a fixed stub: a jump, or
  `mov eax,N; ret`. There's no trampoline and no callback. `unpatch` undoes them.

## Things to watch out for

- The signature you pass has to match the target's real one. If it doesn't, the
  call to the original is built wrong and the backend crashes. Get the real
  signature with `scalpel/sig`.
- `hook-fn` wraps the original, so it still runs. Use `replace-fn` if you want to
  replace it.
- Hooking a function that's already on the call stack is a race. The call that's
  in progress finishes in the original.
- The hook applies to the whole backend. Everything in the backend that calls the
  function goes through your hook, not just the query you're looking at. Don't
  hook things the backend uses all the time, like `palloc` or `strlen`.
