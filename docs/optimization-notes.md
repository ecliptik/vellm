# Optimization notes — Phase 3 winners and non-winners

Written after Phase 3 wrapped (commits `74460cf`, `4b4b969`, `fd1480b`).
Captures what worked, what didn't, and — just as importantly —
what we considered and deliberately skipped, so a future reader knows
the search was thorough and the skipped path was skipped for a reason.

Full per-experiment log is in `docs/phase3-notes.md`. This doc is the
distilled conclusions and the Pentium-specific notes that travel with
the project.

## Headline

`stories15M_q80 -n 200 -s 42 -i "Once upon a time"` under DOSBox-X
`memsize=48, cycles=fixed 90000`:

- **Phase 2 baseline:** 5m45s
- **Phase 3 final:** 2m59s (**1.93×** faster, **−48.4%** wall time)
- **Correctness:** primary 192-byte byte-identical gate still passes —
  no tolerance fallback needed despite `-ffast-math` + int8 KV cache.

Memory on `stories42M_q80` at `memsize=48`:

- **Phase 2 baseline:** 74.50 MB peak, ~30 MB of CWSDPMI swap.
- **Phase 3 with `--max-seq-len 256 + int8 KV`:** 44.69 MB peak, **no
  swap**. First configuration where 42M runs unpaged on the 48 MB
  target.

## What worked

### 1. `-funroll-loops` alone (Experiment A, the whole story)

Dropping this flag into CFLAGS cut ~32% off wall time by itself. The
matmul inner loop (`src/vellm.c` `matmul()`, GS-sized integer-MAC
chain) is a tight fixed-iteration loop — exactly the shape gcc's
unroller was designed for. P5 has zero branch prediction for the
small-iteration loop-back, so unrolling also eliminates the
mispredict tax.

`-O3` on top of `-O2 -funroll-loops` gave nothing measurable and
regressed slightly when `-fschedule-insns{,2}` pulled in with it. The
P5 I-cache is only 8 KB; `-O3`'s vectorization and predictive-commoning
passes enlarge the footprint without corresponding SIMD wins (P54C
predates MMX, so nothing vectorizes).

Kept: `-O2 -fomit-frame-pointer -funroll-loops -ffast-math -Wall`.

### 2. Dropping `-ffloat-store` (Experiment A continued)

Phase 1 added `-ffloat-store` to reconcile x87 with the SSE2-generated
golden — it forced every fp intermediate to round-trip through memory
as 32-bit float. On the P5's short pipeline this is enormous: two
memory ops per fp instruction, FPU blocked on the AGU every op.

Empirical result: with `-ffast-math` on, dropping `-ffloat-store`
saved another ~1m of wall time per `-n 200` run (3m49s → 2m43s) **and
still clears the 192-byte primary gate** on the canonical prompt.
The first 30 tokens are the cross-toolchain-stable manifold; the
pipeline behind them can run at full x87 80-bit extended precision
without tripping the fingerprint.

Kept. This is the single biggest lever in Phase 3.

### 3. `-ffast-math` (Experiment A continued)

About +1.6% on its own, but the important thing it does is unlock (2)
— gcc contracts/reassociates fp chains and deduplicates reciprocals
in ways that `-ffloat-store` used to block.

Kept.

### 4. IDIV → SAR for the per-group scale lookup (Experiment B)

`matmul()`'s per-group epilog does `w->s[(in + j) / GS] * x->s[j / GS]`.
`GS` is read from the checkpoint header (global, not compile-time
const), so gcc can't prove it's a power of 2 and falls back to signed
integer division.

P54C cycle counts per Agner Fog (see references below):

- `IDIV r32`: **46 cycles, non-pipelined, non-pairable**
- `IMUL r32,r32`: 10 latency, 1/cycle throughput (pipelined)
- `SAR r32, imm`: 1 cycle, U+V pairable

Per GS=32 group: 32 pipelined IMULs ≈ 32 cycles, plus **92 cycles of
IDIV** — the division was ~49% of per-group time on real hardware.

`read_checkpoint()` now asserts `GS` is a power of 2 (upstream
`export.py` always writes 32 or 64; documented in `docs/format.md` as
a format invariant) and derives `GS_SHIFT = log2(GS)` once at load.
`matmul()` uses `>> GS_SHIFT` in place of `/ GS`.

Kept with caveat. **DOSBox-X does not model P5 instruction latencies**
— its dynamic-translation core charges ~1 emulated cycle per x86
instruction regardless of whether real hardware takes 1 or 46. The
measured DOSBox delta was ~1.5% at `-n 200`. Real PODP5V83 wall-clock
numbers captured in Phase 5 will be the meaningful validation; this
row of the benchmark table is expected to look very different there.

### 5. int8 KV cache with per-head fp32 scale (Task #4)

KV cache was the largest single allocation on 42M at default
`seq_len=1024` — 32 MB, enough by itself to blow past the 46.55 MB
physical ceiling. int8 quantization with a **per-head** scale
(n_kv_heads floats per (layer, pos)) gives a 3.8× reduction with
minimal quantization error: each attention inner loop already walks
one head at a time, so the scale multiply is one fp op outside the
dot product.

Lossy in principle, but the canonical stories15M run still clears the
**primary 192-byte gate** — the 30-token fingerprint absorbs the
sub-ULP drift. The tolerance gate is there as the safety net, not as
the expected pass path.

Kept. Paid ~18.5s of wall-time regression on 15M (2m40.5s → 2m59s)
for the memory headroom that makes 42M usable without swap — the
correct trade for a memory-constrained target.

## What didn't work (and why, so it stays skipped)

### `-O3` over `-O2`

Regresses slightly on this workload. `-ftree-loop-vectorize` and
`-fpredictive-commoning` bloat the code footprint in the 8 KB P5
I-cache without SIMD wins to compensate. Kept at `-O2`.

### `-fschedule-insns{,2}`

Neutral or harmful alongside `-funroll-loops`. gcc's Pentium
scheduler already does the useful reordering at `-O2`; explicit
scheduling fights unrolling's register pressure and sometimes
re-inserts the pipeline stalls `-funroll-loops` had removed. Not kept.

### Hand-written U/V pipe pairing (Experiment B originally-planned)

Inspected the `-funroll-loops -ffast-math` asm output for `matmul()`
and found the P5 bottleneck is **IMUL throughput** (1/cycle when
pipelined — the irreducible floor for an int-MAC loop) plus the two
non-pairable IDIVs per group. Individual pairable ops (MOV, ADD, SAR)
are already reasonably paired by gcc; hand-scheduling would buy
fractions of a percent.

Experiment B pivoted from "U/V pairing" to "remove the IDIV", which
was the higher-leverage version of the same insight: attack the
non-pairable instruction directly instead of trying to pair around it.

Agner Fog's U/V pairing rules are still worth keeping in the back
pocket for the **attention-score dot product** (fp32, no IMUL) if it
ever becomes the hot path — but on 15M/42M it isn't, and on real
P5 hardware we haven't measured yet.

### Blocked / tiled matmul (Experiment C)

Considered and rejected on algorithmic grounds. The matmul here is
matrix-**vector** (W[d,n] × x[n] → xout[d]), not matrix-matrix:

- `x[n]` is reused across all d iterations of `i` — already in L1
  naturally (n=288 for 15M → 288 bytes, trivial for 8 KB L1d).
- `w[d,n]` is streamed once per call and never revisited. No
  tile-local reuse to capture.

Tiling wins when the inner operand has reuse across an outer
dimension — classic GEMM pattern. matvec has no such reuse. Tiling
here would just add loop nests with no cache benefit. Skipped.

### Fixed-point matmul (Experiment D)

Considered and rejected. `matmul()` already uses int32 accumulation
for the entire dot product; the only fp work per group is the final
scale multiply (`ival × w->s × x->s`) and one fadd. ~12K fp ops per
matmul call for 15M's wq vs ~83K IMULs — fp is already <15% of the
hot loop. Converting the per-group scale to fixed-point would save a
fraction of that fraction while adding quantization error to the only
remaining fp precision in the pipeline. Not a good trade.

### Int-MAC on the attention inner loops (post-Phase-3 candidate)

int8 KV cache pushed int8→float conversion into the attention fmadd
chain. A fully-int MAC (quantize `q` and `att` too, do int32-
accumulated dot products) would eliminate the conversion cost and
potentially flip the 18.5s Phase-3 #4 regression into a net win.

Non-trivial integer-quantization design, more moving parts to keep
numerically stable. Deferred — filed in `docs/phase3-notes.md`
§"Task #4 Possible follow-up".

## Agner Fog references

Agner Fog's "The microarchitecture of Intel, AMD, and VIA CPUs" and
"Instruction tables" are the canonical P54C latency data. Chapters
that directly informed Phase 3:

- **P5 chapter, U/V pairing rules** — which instructions pair in the
  V-pipe, which stall on write-after-write, AGI stalls, FP
  instruction exchange register file. Used to confirm that IMUL
  throughput (not pairability) is the bottleneck in unrolled matmul
  and that individual pair-scheduling has no remaining headroom.
- **P5 chapter, IDIV timing** — 46 cycles non-pipelined is the number
  that made Experiment B worth pursuing.
- **P5 chapter, FP stack management** — justification for why
  `-ffloat-store` was so expensive: every `fstp`/`fld` pair is two
  memory ops and blocks the FPU on the AGU.

All three are in the `P5` (original Pentium) chapter; the P6 / P-Pro
chapter starts after them and doesn't apply to P54C.

## Pentium-targeting summary (for future phases)

Rules of thumb learned during Phase 3:

1. **`-funroll-loops` is free money** on tight fixed-iteration int
   loops. Always.
2. **`-ffloat-store` is extremely expensive** on P5. Only enable it
   if you have a measured correctness reason (Phase 1 did; Phase 3
   doesn't).
3. **`-O3` over `-O2`** is usually a regression on pre-SIMD hardware.
4. **Division is the devil.** Always check `grep idivl` on the hot
   function's asm output. If a divisor is a load-time constant and a
   power of 2, replace with a shift.
5. **Don't tile matvec.** It's a streaming-read over weights with a
   cache-hot x vector. Tiles don't help.
6. **Measure on real hardware before you trust DOSBox-X's wall time
   for microarchitectural optimizations.** DOSBox throttles
   work-per-wall-ms but doesn't model instruction latencies; it
   systematically under-represents wins that replace slow
   non-pairable instructions with fast ones.

## Phase 4 carryover

- `--benchmark` CLI flag (stub parsed from Phase 0) not yet wired
  to emit tok/s with cycle breakdowns. That's Phase 4 proper.
- Real PODP5V83 wall-clock for the full optimization matrix is the
  Phase 5 exit deliverable.
- Int-MAC attention inner loops (see §"What didn't work" above).
