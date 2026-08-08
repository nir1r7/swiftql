# Seam audit — storage — pass 4

Branch `claude/phase5-week26-qomtkb`, HEAD `b2bc70e` (code identical to the gated
`9da0494`; `b2bc70e` is docs-only — verified with `git show --stat`).
Written incrementally; sections appear in the order they were established.

Prior passes: `seam-storage-pass-1.md` (S-0 headline), `-2.md` (refuted S-0; 0/0/0/2),
`-3.md` (0 BLOCKER / 0 HIGH / 1 MEDIUM S-9 / 3 LOW).

**Binaries.** All measurements below use a **Release** build (`-O3 -DNDEBUG`) configured
out of the session scratchpad from the HEAD tree, built under
`flock -w 1800 /tmp/swiftql-build.lock`; `build/`, `build-seamfix/` and
`build-seamfix-rel/` in the working tree were **not touched**. `build-seamfix-rel/swiftql`
predates HEAD (05:54 vs 09:03) and was deliberately not reused. Correctness runs that
name a Debug binary say so explicitly.

The three real cells, restated so the tables below are unambiguous:

    A  --storage row      --execution volcano
    B  --storage columnar --execution volcano
    C  --storage columnar --execution vectorized
    (+ D = C with --no-optimize, an optimizer flag rather than a fourth cell)

---

## Part A.1 — S-9 re-measured on the current tree: **still open, unchanged, both halves**

(section written first; measurements below)
