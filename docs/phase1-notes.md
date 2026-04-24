# Phase 1 Notes — Integration Test Findings

Captured during the Phase 1 integration gate (task #7). These are the
landmines we hit running the byte-identical correctness gate end-to-end.
Companion to `docs/phase0-notes.md`.

## Phase 1 gate status

**Result: FAIL on full `-n 200` byte-identical, PASS on short `-n 20` smoke.**

| Run      | Tokens | vellm.exe bytes | golden bytes | Match                  |
|----------|-------:|-----------------|--------------|------------------------|
| `-n 20`  |     20 | 83              | 83           | byte-identical (md5 match) |
| `-n 200` |    200 | 677 (post-CR)   | 692          | diverges at byte 193   |

First ~192 bytes of `-n 200` output are identical to the golden; divergence
begins in the second paragraph. Both continuations are grammatically valid
TinyStories passages — neither is garbage. This is fp accumulation drift,
not a porting bug.

See "FP precision mismatch" below for the root cause and candidate fixes.

## Q8_0 model fetch (for future Phase 6 `fetch-model.sh`)

Upstream hosts `stories15M.bin` / `stories15M.pt` on HuggingFace
(`karpathy/tinyllamas`) but does **not** host a pre-quantized
`stories15M_q80.bin` — despite CLAUDE.md/PLAN.md reading as if such a file
is downloadable. The q80 variant is a host-side derivative produced by
`vendor/llama2.c/export.py --version 2`.

Working pipeline used during task #1:

```bash
# Install PyTorch CPU build on the host (one-time, ~200 megs)
python3 -m pip install --break-system-packages torch --index-url \
    https://download.pytorch.org/whl/cpu

# Fetch the fp32 checkpoint + the matching model.py from the pinned
# upstream SHA (model.py is not vendored under vendor/llama2.c/ — only
# run.c/runq.c/export.py/tokenizer.py are).
UPSTREAM_SHA=$(cat vendor/llama2.c/UPSTREAM_SHA)
mkdir -p /tmp/q80 && cd /tmp/q80
curl -sSL -o model.py \
    "https://raw.githubusercontent.com/karpathy/llama2.c/$UPSTREAM_SHA/model.py"
curl -sSL -o stories15M.pt \
    "https://huggingface.co/karpathy/tinyllamas/resolve/main/stories15M.pt"
cp /path/to/vellm/vendor/llama2.c/export.py .

# Run the quantizer. Output is ~17 megs.
python3 export.py stories15M_q80.bin --version 2 --checkpoint stories15M.pt
```

For Phase 6 (`docs/fine-tune.md`, `tools/fetch-model.sh`): bake this flow
into a script. Don't document the file as "downloadable" — document it as
"generated, here is the one-time-build recipe."

## DOS 8.3 filename mangling

DOSBox-X's default DOS-shell view of the C: mount exposes only 8.3-short
names, even though the host filesystem has long names. Anything named
longer than 8 chars base + 3 chars extension (e.g. `stories15M_q80.bin`,
`tokenizer.bin`) gets aliased to `STORIE~N.BIN`, `TOKENI~N.BIN`, etc. The
`~N` suffix ordering depends on directory population order — **fragile**.

Symptom we hit: `run-golden.sh` initially passed the long names verbatim
in the RUN.BAT:

```
VELLM.EXE STORIES15M_Q80.BIN -z TOKENIZER.BIN ...
```

COMMAND.COM doesn't see these names → `fopen` fails → vellm prints to
stderr (which was not captured) → exits → empty STDOUT.TXT → the test
"failed" with a trivial 0-byte capture that looked nothing like a real
correctness failure.

Fix: `run-golden.sh` stages the model + tokenizer into an intermediate
tempdir under 8.3-safe names `MODEL.BIN` / `TOKEN.BIN` before handing them
to `dosbox-run.sh --include`. RUN.BAT invokes with those names. This keeps
the test independent of `~N` allocation order.

Alternative not taken: setting DOSBox-X `lfn = true`. Rejected because
real DOS 6.22 on the target hardware does not have LFN support, so the
test would drift from the deployment environment.

**Follow-up resolved (Phase 4 post-landing):** the `make install`,
`make dist`, and `make cf-package` targets were updated to stage
everything under 8.3-safe names (`STORY15.BIN`, `STORY42.BIN`,
`TOKEN.BIN`) so the on-CF path matches the test path exactly. Before
the fix, a real-DOS run of the shipped `RUN.BAT` / `BENCH.BAT` failed
with "Couldn't open file STORIES15M_Q80.BIN" because `tar xzf` on a
Linux VFAT driver wrote long names whose `~N` aliases weren't stable
under pure DOS 6.22.

## CWSDPMI paging under DOSBox-X (signal for Phase 2)

`-n 200` takes **~5 min 16 s** of DOSBox-X wall time at `cycles = fixed
90000`. Most of that is disk activity, not compute.

Observation from a kept staging dir mid-run:

- `CWSDPMI.SWP` grew to ~10 megs while `STDOUT.TXT` was still 0 bytes.
- `MODEL.BIN` is 17 megs resident. vellm allocates another ~3 megs of
  activations / KV cache / runstate. CWSDPMI overhead on top.
- Config: `memsize = 48` (megs), which matches the target PODP5V83. DOSBox-X
  still pages because CWSDPMI's DPMI arena is smaller than the real
  physical — it reserves memory for the extender, the stub, and
  conventional DOS, so effective allocatable is well under 48 megs.
- Consequence: stdout grows slowly at first (several seconds with zero
  output while the model streams through the DPMI arena once per token
  generation) then picks up. This looked like a wedge the first time we
  saw it; it isn't.

**Debug tip for "is it hung vs. just slow?"**  Run dosbox-run.sh with
`--keep-stage` and check both in the staging dir:

- `STDOUT.TXT` — should be growing (tokens trickling out)
- `CWSDPMI.SWP` — should be growing (CWSDPMI actively paging)

Both flat = truly stuck. Either one moving = alive.

**Phase 2 input:** this confirms the allocation audit + pre-allocated
arena approach in PLAN.md § Phase 2 isn't optional — it's the difference
between 5 min and (hopefully) ~30 s of DOSBox-X wall time per 200-token
run. It also means the ~1.25 tok/s we measured is an under-estimate for
real hardware performance, because a real PODP5V83 doesn't page when it
fits in 48 megs of real DRAM.

## FP precision mismatch — the Phase 1 gate failure

The byte-identical gate as specified in PLAN.md § Phase 1 assumes the
native `run_host` reference and the DJGPP-built `vellm.exe` produce
identical fp accumulation. They don't, for subtle and unavoidable
reasons.

### What we observed

- Golden (native gcc x86-64, `-O2 -Wall`, SSE2 fp by default) and
  `vellm.exe` (DJGPP 12.2 `i586-pc-msdosdjgpp`, `-march=pentium -O2
  -fomit-frame-pointer`, x87 fp) agree for the first 192 bytes then
  diverge. The first diverging byte is at offset 193, second paragraph,
  fifth word. From that point both continuations are coherent but
  different TinyStories passages. The divergence point is reproducible
  across runs.
- Shorter generations stay in the matching prefix: `-n 20` (83 bytes)
  is byte-identical to `run_host -n 20` on the same seed. So the gate
  only fails once the divergence point is past.
- Verified this is not a `-ffast-math` leak: neither `Makefile` nor
  `Makefile.host` enables it, and `grep -R 'ffast-math\|Ofast' Makefile*`
  returns nothing.

### Why

Native x86-64 gcc defaults to SSE2 for scalar fp (`-mfpmath=sse -msse2`),
which evaluates float arithmetic in strict IEEE-754 32-bit. DJGPP targets
i586 and defaults to x87, which carries intermediates in 80-bit extended
precision and rounds to 32-bit only on store. Ordering-identical code
therefore produces **numerically** slightly different results. In a
deeply-chained operation like the transformer forward pass — hundreds of
matmuls, per-layer residuals, softmax, argmax — a sub-ULP difference in
any intermediate propagates and eventually flips a top-1 argmax, at which
point the generation diverges permanently.

### Candidate fix — `-ffloat-store` on the reference build

Confirmed experiment during task #7:

```bash
cc -O2 -Wall -m32 -march=pentium -mfpmath=387 -ffloat-store \
   -o run_host_strict vendor/llama2.c/runq.c -lm
./run_host_strict <same args> > /tmp/strict.txt
md5sum /tmp/strict.txt tests/golden/once_upon_a_time.txt
# 3575c4cc2dad115479bc78069aedd789  /tmp/strict.txt  ← MATCHES
# 3575c4cc2dad115479bc78069aedd789  tests/golden/once_upon_a_time.txt
```

So `-m32 -march=pentium -mfpmath=387 -ffloat-store` happens to produce
byte-identical output to the x86-64 SSE2 default build. That's useful
evidence — it means forcing strict 32-bit rounding on every store
reconciles x87 and SSE2 for this workload.

**The corresponding DJGPP experiment was not completed in task #7.** We
rebuilt `vellm.exe` with `CFLAGS=... -ffloat-store`, started a `-n 200`
run, and the test took long enough that team-lead killed the run for
unrelated reasons (CWSDPMI quit-warning modal, since fixed). Given how
close the native strict-x87 build comes to matching, it is very likely
that DJGPP `-ffloat-store` will also match — but that's a claim, not a
measurement yet.

### Proposed resolution (for team-lead triage)

1. **First: re-test DJGPP `-ffloat-store`.** One-line Makefile change.
   Cheap to verify. If it passes, done.
2. **If (1) fails:** consider either tightening the reference build
   (`Makefile.host` gets `-m32 -mfpmath=387 -ffloat-store` and we
   regenerate the golden), or redefining the Phase 1 gate to
   "first-K-tokens byte-identical + full-sequence cosine similarity
   within 1e-5" (which is what real inference-kernel verification
   projects do; PLAN.md already defines a tolerance-based Phase 3 gate,
   and we could fold it forward).
3. **Don't** relax the gate to "first line matches" — that would hide
   future real bugs.

## DOS-PORT audit (task #7 step 3)

`grep -n 'DOS-PORT:' src/vellm.c` yields 15 matches — 1 header reference
at line 9 plus 14 per-change annotations. Summary of the 14 code changes:

| Line | Change                                                              |
|-----:|---------------------------------------------------------------------|
|   26 | Skip `<sys/mman.h>` under `__DJGPP__` (no mmap)                     |
|   27 | Cross-reference to the malloc+fread path                            |
|  103 | `ssize_t` documented as 32-bit on DJGPP (long)                      |
|  104 | Confirmation that 15M/42M q80 fits in 32 bits                       |
|  238 | `fopen(checkpoint, "rb")` — mandatory to avoid CRLF corruption      |
|  260 | `ftell` returns 32-bit long on DJGPP; fine for these checkpoints    |
|  262 | Slurp checkpoint via `malloc + fread` (no mmap)                     |
|  263 | 48 megs DPMI is plenty for 15M/42M q80                                |
|  275 | `fd = -1` — struct parity with upstream; no real fd on DOS          |
|  300 | `free(data)` in place of `munmap` on teardown                       |
|  543 | `fopen(tokenizer, "rb")` — same CRLF reasoning as checkpoint        |
|  869 | `clock()` fallback; `clock_gettime`/`CLOCK_REALTIME` absent on DJGPP |
|  870 | PIT 55ms granularity is fine for tok/s reporting                    |
| 1061 | `setvbuf(stdout, NULL, _IOFBF, 4096)` — unbuffered stdout is slow   |

All 14 changes are required DOS-compatibility deltas. None are
optimizations or refactors. Matches the CLAUDE.md "DOS-PORT: is only for
required changes" rule.
