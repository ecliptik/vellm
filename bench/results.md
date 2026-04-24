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

| Platform | CPU | Model | Max seq | Prompt tok/s | Gen tok/s | Wall (s) | Peak MB | Notes |
|---|---|---|---:|---:|---:|---:|---:|---|
| DOSBox-X cycles=90000 | GenuineIntel family 5 model 1 stepping 7 | 15M q80 | default (256) | 1.01 | 0.99 | 201.4 | 18.87 | primary Phase 4 reference |
| DOSBox-X cycles=90000 | GenuineIntel family 5 model 1 stepping 7 | 15M q80 | 256 (explicit) | 1.01 | 0.99 | 201.4 | 18.87 | `--max-seq-len 256` sanity — no-op for 15M (native seq_len=256) |
| DOSBox-X cycles=90000 | GenuineIntel family 5 model 1 stepping 7 | 42M q80 | 256 | 0.42 | 0.41 | 491.3 | 46.06 | 42M at `--max-seq-len 256`, near-ceiling |
| Host Linux (native) | Intel i7-8700K @ 3.70 GHz | 15M q80 | default (256) | n/a | ~96 | ~2.1 | n/a | sanity ceiling — upstream runq.c |
| Host Linux (native) | Intel i7-8700K @ 3.70 GHz | 42M q80 | default (1024) | n/a | ~44 | ~4.0 | n/a | sanity ceiling — upstream runq.c |
| Real PODP5V83 | Pentium OverDrive 83 MHz (P54C, family 5 model 3) | 15M q80 | 256 | _TBD_ | _TBD_ | _TBD_ | _TBD_ | **pending real-HW run** |
| Real PODP5V83 | Pentium OverDrive 83 MHz (P54C, family 5 model 3) | 42M q80 | 256 | _TBD_ | _TBD_ | _TBD_ | _TBD_ | **pending real-HW run** |

## Observations

- **15M-default ≡ 15M-seq256.** Identical numbers because
  `stories15M_q80`'s native `seq_len = 256`; `--max-seq-len 256` is a
  no-op for this model. Run kept as a regression sanity (if the cap
  ever started corrupting the KV stride, it'd diverge here).
- **42M wall time ~8 min.** Expected on DOSBox-X cycles=90000. 42M
  has ~2.8× the parameters of 15M and the KV cache is larger; tok/s
  drops by ~2.4× (0.99 → 0.41).
- **Peak memory: Phase 4 numbers are higher than Phase 3 notes**
  reported (18.87 vs 17.44 MB for 15M; 46.06 vs 44.69 MB for 42M).
  Not a regression — different measurement window. Phase 3 captured
  `after-build_transformer` only (arena alone);
  `--benchmark peak mem` captures whole-process high-water (arena +
  activations + stdio buffers + heap fragmentation). Both are
  correct; read them in context.
- **42M peak = 46.06 MB is very close to the 46.55 MB DPMI
  physical ceiling.** The docs/phase3-notes.md §"Integration
  measurements" claim of "0 swap" was measured on the arena; real
  total demand is 0.5 MB under the ceiling. 42M + `--max-seq-len 256`
  still fits with negligible margin, but "no margin" is closer to
  the truth than "2 MB headroom". Real PODP5V83 numbers will
  confirm whether CWSDPMI.SWP grows at all.
- **CPUID brand mismatch is intentional.** DOSBox-X's emulated
  Pentium reports `family 5 model 1`, while a real PODP5V83 reports
  `family 5 model 3`. Use this as a cheap "is this a real-hardware
  row or a DOSBox-X row?" indicator when reading this file cold.

## Provenance

### DOSBox-X rows

Produced by `bench/run.sh` on the host (Linux) driving `dosbox-x`
via `tools/dosbox-run.sh`. DOSBox-X config:

- `machine = vgaonly`
- `memsize = 48` (MB)
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
for 48 MB of real DOS).

Reference CPU: Intel Core i7-8700K @ 3.70 GHz, 16 GB RAM, single
thread (runq.c is single-threaded).

### Real PODP5V83 row

**Reserved.** Not fabricated. The `_TBD_` entries are placeholders
for the real-hardware benchmark run — `dist/vellm-cf.tar.gz`
(produced by `make cf-package`) is the deploy artifact intended for
this run. The operator boots DOS on the PODP5V83, runs
`BENCH.BAT` (15M) and/or `BENCH42.BAT` (42M), and pastes the
`--- VELLM BENCHMARK ---` block into this file.

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
