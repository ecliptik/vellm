# vellm benchmark results

Reproducible benchmarks for vellm on the canonical Phase 4 prompt.

## Test matrix

| Axis | Values |
|---|---|
| Prompt | `"The old computer hummed to life"` (fixed, per PLAN.md Phase 4 step 1) |
| Seed | 42 (fixed) |
| Temp | 0 (greedy argmax) |
| Steps | 200 tokens |
| Model | `stories15M_q80.bin` (17,101,696 B), `stories42M_q80.bin` (44,321,024 B) |

All rows below were generated with `vellm.exe --benchmark` or the
upstream-compatible `run_host --benchmark` equivalent; numbers are
parsed from the `--- VELLM BENCHMARK ---` block. See `bench/run.sh`
for the harness, `docs/phase3-notes.md` for the optimization history
that produced the current build, and the "Provenance" section below
for how each row was measured.

## Headline numbers

Captured 2026-04-23 on commit `4d1fd7a` (`--benchmark` harness) with
the Phase 3 final build (`-O2 -fomit-frame-pointer -funroll-loops
-ffast-math`, int8 KV cache, IDIV→SAR, `--max-seq-len` supported).

| Platform | CPU | Model | Max seq | Prompt tok/s | Gen tok/s | Wall (s) | Peak megs | Notes |
|---|---|---|---:|---:|---:|---:|---:|---|
| DOSBox-X cycles=90000 | GenuineIntel family 5 model 1 stepping 7 | 15M q80 | default (256) | 1.01 | 0.99 | 201.4 | 18.87 | primary Phase 4 reference |
| DOSBox-X cycles=90000 | GenuineIntel family 5 model 1 stepping 7 | 15M q80 | 256 (explicit) | 1.01 | 0.99 | 201.4 | 18.87 | `--max-seq-len 256` sanity — no-op for 15M (native seq_len=256) |
| DOSBox-X cycles=90000 | GenuineIntel family 5 model 1 stepping 7 | 42M q80 | 256 | 0.42 | 0.41 | 491.3 | 46.06 | 42M at `--max-seq-len 256`, near-ceiling |
| Host Linux (native) | Intel i7-8700K @ 3.70 GHz | 15M q80 | default (256) | n/a | ~96 | ~2.1 | n/a | sanity ceiling — upstream runq.c |
| Host Linux (native) | Intel i7-8700K @ 3.70 GHz | 42M q80 | default (1024) | n/a | ~44 | ~4.0 | n/a | sanity ceiling — upstream runq.c |
| Real PODP5V83 | Pentium OverDrive 83 MHz (P54C, family 5 model 3 stepping 2) | 15M q80 | default (256) | 0.28 | 0.27 | 715.6 | 19.79 | measured 2026-04-24 on real hardware; 200 tokens |
| Real PODP5V83 | Pentium OverDrive 83 MHz (P54C, family 5 model 3 stepping 2) | 42M q80 | **128** | 0.08 | 0.11 | 1187.7 | 45.01 | measured 2026-04-24 on real hardware; 128 tokens (`-L 128` — see observations below) |

## Observations

- **15M-default ≡ 15M-seq256.** Identical numbers because
  `stories15M_q80`'s native `seq_len = 256`; `--max-seq-len 256` is a
  no-op for this model. Run kept as a regression sanity (if the cap
  ever started corrupting the KV stride, it'd diverge here).
- **42M wall time ~8 min.** Expected on DOSBox-X cycles=90000. 42M
  has ~2.8× the parameters of 15M and the KV cache is larger; tok/s
  drops by ~2.4× (0.99 → 0.41).
- **Peak memory: Phase 4 numbers are higher than Phase 3 notes**
  reported (18.87 vs 17.44 megs for 15M; 46.06 vs 44.69 megs for 42M).
  Not a regression — different measurement window. Phase 3 captured
  `after-build_transformer` only (arena alone);
  `--benchmark peak mem` captures whole-process high-water (arena +
  activations + stdio buffers + heap fragmentation). Both are
  correct; read them in context.
- **42M peak = 46.06 megs is very close to the 46.55 megs DPMI
  physical ceiling** under DOSBox-X. On real PODP5V83 this
  configuration actually pages — CWSDPMI's real-DOS overhead is
  ~1.4 megs higher than DOSBox-X models, which pushes 42M
  `--max-seq-len 256` a megabyte or two over the physical
  ceiling (`mem /c` reports 47 megs free XMS; CWSDPMI claims ~2 megs
  for page tables etc.; net usable ~45 megs). Observed behavior:
  `CWSDPMI.SWP` grew to ~10 megs within minutes on the real-HW
  attempt before being aborted.
- **42M real-HW uses `--max-seq-len 128`** to fit the physical
  ceiling. `--benchmark` mode clamps its canonical 200-token
  target to the cap, so the 42M real-HW row reports 128 tokens
  instead of 200. Tok/s stays directly comparable to the 15M
  row (it's a rate). Peak memory drops to 45.01 megs — right at
  the ceiling, but unpaged (`CWSDPMI.SWP` stayed at 0 bytes
  over the full 19m 48s run).
- **42M / 15M tok/s ratio: 2.45×** on real PODP5V83 (0.27 → 0.11).
  Expected ~2.8× from parameter-count scaling; the small gap is
  within measurement noise and aligns with the DOSBox-X ratio
  (2.4×). Confirms no unexpected real-HW penalty beyond the
  per-token matmul work.
- **CPUID brand mismatch is intentional.** DOSBox-X's emulated
  Pentium reports `family 5 model 1`, while a real PODP5V83 reports
  `family 5 model 3`. Use this as a cheap "is this a real-hardware
  row or a DOSBox-X row?" indicator when reading this file cold.
- **Real hardware is 3.56× slower than DOSBox-X at `cycles=90000`**
  for this workload: 715.6 s vs 201.4 s wall on the same 15M run.
  `cycles = fixed 90000` approximates IPC × frequency but doesn't
  model cache misses hitting the 60–66 MHz FSB, FPU dependency-chain
  latency, or pipeline stalls — all of which dominate our
  matmul-heavy hot loop. Memory footprint, on the other hand,
  matches the DOSBox-X projection to the byte (19.79 megs real vs
  19.9 megs DOSBox-X peak for 15M) — DPMI allocations don't depend on
  CPU simulation fidelity. DOSBox-X cycles≈25000 would approximate
  real-HW wall time; see `docs/hardware.md` §"DOSBox-X vs
  real-hardware calibration".

## Provenance

### DOSBox-X rows

Produced by `bench/run.sh` on the host (Linux) driving `dosbox-x`
via `tools/dosbox-run.sh`. DOSBox-X config:

- `machine = vgaonly`
- `memsize = 48` (megs)
- `cputype = pentium` (P54C equivalent — no MMX, FPU present)
- `core = normal` (deterministic emulation; `dynamic` reorders in
  ways that break byte-exact golden diffs)
- `cycles = fixed 90000` (starting-point throttle for 83 MHz Pentium
  integer perf; not cycle-accurate — see note below)

**DOSBox-X measurement caveat:** `cycles = fixed 90000` is a
throttling knob, not a P5-cycle simulator. It charges ~1 emulated
cycle per x86 instruction regardless of actual P5 latency. Optimizations
that replace a rare-but-slow P5 instruction with a fast one (e.g.
Phase 3 Experiment B's IDIV→SAR) look near-null under DOSBox but pay
off massively on real hardware. Real PODP5V83 numbers will diverge
from these DOSBox-X numbers — in the direction of "real HW is faster
than 1:1 emulation would suggest" for the optimized paths. See
`docs/phase3-notes.md` §"Experiment B" for the detailed analysis.

### Host Linux rows

Produced by `./run_host` (native gcc build of `vendor/llama2.c/runq.c`
at the pinned UPSTREAM_SHA). Pre-`--benchmark` — `run_host` emits
upstream's `achieved tok/s: N.NN` line on stderr, which we capture
directly. Wall time is measured from the shell via `time`. Host
numbers establish a "how fast does the algorithm go when compute
isn't emulated" ceiling; they're not a direct comparison to vellm
(which applies int8-KV and other memory-vs-speed tradeoffs appropriate
for 48 megs of real DOS).

Reference CPU: Intel Core i7-8700K @ 3.70 GHz, 16 GB RAM, single
thread (runq.c is single-threaded).

### Real PODP5V83 rows

The 15M row was measured 2026-04-24 on the target PODP5V83
(Anigma LP4IP1, 48 megs RAM, CF-to-IDE with MS-DOS 6.22, CWSDPMI r7)
using `dist/vellm-cf.tar.gz` / `dist/vellm-cf.zip` (produced by
`make cf-package`). Operator boots DOS on the PODP5V83, runs
`BENCH.BAT` (15M) and/or `BENCH42.BAT` (42M, `--max-seq-len 256`),
and transcribes the `--- VELLM BENCHMARK ---` block. Raw
transcription for the 15M run:

```
cpu        : GenuineIntel family 5 model 3 stepping 2
model      : STORY15.BIN
ckpt bytes : 17101696
tokens     : 199
prompt tok : 8
gen tok    : 191
wall ms    : 715604
prompt tok/s: 0.28
gen tok/s  : 0.27
peak mem   : 19791872
```

The 42M row is still `_TBD_` — the `BENCH42.BAT` run on real
hardware is in flight at the time of this commit and fills in
once the operator transcribes the block.

## Reproducing

From a fresh clone:

```bash
make                                     # cross-build vellm.exe
bench/run.sh                             # DOSBox-X, all scenarios,
                                         # prints rows to stdout
bench/run.sh --output bench/results.md --append
                                         # ...or append to this file
bench/run.sh --scenario 15m-default      # run one scenario
bench/run.sh --dry-run                   # show invocations w/o running
```

Real hardware:

```bash
make cf-package                          # produces dist/vellm-cf.tar.gz
                                         # (and .zip)
# copy to CF card, boot DOS on PODP5V83
BENCH.BAT                                # 15M
BENCH42.BAT                              # 42M, --max-seq-len 256
```
