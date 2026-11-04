# Build-time codegen: bake the always-present Janet helpers into pgs_prelude.h,
# a C string the extension evaluates at VM init. Concatenates (in order):
#   scalpel-lib.janet  — struct packing + the pgj-load script loader
#   patch.janet    — hot-patching helpers (patch-jump/patch-return-int/hook/...)
# so all of them are available in every instrument REPL without a manual load.
# (fuzz.janet stays load-on-demand via pgj-load — heavier and situational.)
# Run with no args (paths are fixed for this project).

# Paths are relative to the repo root; run this from there:
#   janet/build/janet tools/embed-prelude.janet
(def lib   "janet-lib/scalpel-lib.janet")
(def patch "janet-lib/patch.janet")
(def out   "pgs_prelude.h")

(def src (string (slurp lib) "\n" (slurp patch)))
(def b @"")
(buffer/push-string b
  "/* Generated from janet-lib/scalpel-lib.janet by tools/embed-prelude.janet. */\n")
(buffer/push-string b "/* Do not edit; edit the .janet source and rebuild.   */\n")
(buffer/push-string b "static const char pgs_prelude[] =\n")
(each line (string/split "\n" src)
  (def esc (->> line
                (string/replace-all "\\" "\\\\")
                (string/replace-all "\"" "\\\"")))
  (buffer/push-string b "\t\"")
  (buffer/push-string b esc)
  (buffer/push-string b "\\n\"\n"))
(buffer/push-string b "\t;\n")
(spit out b)
(printf "wrote %s (%d bytes from %d-byte source)" out (length b) (length src))
