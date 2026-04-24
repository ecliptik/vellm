# Tested hardware

The primary target is documented in `CLAUDE.md`:

- Intel Pentium Overdrive PODP5V83 (83 MHz, Socket 3, P54C, no MMX)
- 48 MB RAM
- Anigma LP4IP1 motherboard
- CF-to-IDE (2 GB MS-DOS 6.22 + WfW 3.11; 4 GB Win95 OSR2.5)
- ATI Mach64 215CT PCI (VGA text mode only)
- CWSDPMI r7

This file captures Phase 2's memory findings, Phase 3's speed +
memory integration numbers, and the Phase 5 real-hardware
measurements from the target PODP5V83. It will grow further (including
the 486DX2/66 stretch benchmark) as more hardware is tested.

## Real PODP5V83 (measured)

First real-hardware run landed 2026-04-24 on commit
[`13c7d34`](../README.md) (Phase 5 benchmark binary).

| Component | Value |
|---|---|
| CPU | Intel Pentium OverDrive PODP5V83, 83 MHz, Socket 3, P54C core, no MMX |
| CPUID | `GenuineIntel family 5 model 3 stepping 2` (Overdrive variant — note `model 3` vs DOSBox-X's `model 1`) |
| L1 cache | 32 KB |
| RAM | 48 MB |
| Motherboard | Anigma LP4IP1 |
| BIOS date | _TBD — to be read off the POST screen_ |
| Storage | CF-to-IDE, 2 GB card (MS-DOS 6.22 + Windows for Workgroups 3.11) |
| Video | ATI Mach64 215CT PCI (VGA text mode) |
| DPMI | CWSDPMI r7 |

### Benchmark results on real hardware

| Model | Max seq | Prompt tok/s | Gen tok/s | Wall | Peak MB |
|---|---:|---:|---:|---:|---:|
| `stories15M_q80` | default (256) | 0.28 | 0.27 | 11m 56s (715.6 s) | 19.79 |
| `stories42M_q80 --max-seq-len 256` | 256 | _pending_ | _pending_ | _pending_ | _pending_ |

The 42M row is pending — the operator's `BENCH42.BAT` run is still
in flight at the time of writing. Peak memory is expected near
45 MB from the DOSBox-X projection; the real-HW number lands when
the run completes.

## DOSBox-X vs real-hardware calibration

At `cycles = fixed 90000`, DOSBox-X runs the canonical
`stories15M_q80` benchmark in 201 s (see
[`bench/results.md`](../bench/results.md)). The same binary on the
real PODP5V83 takes 716 s. Ratio: **real HW is 3.56× slower than
DOSBox-X at `cycles = fixed 90000`**.

Why the gap is large:

- **Cache misses.** Our matmul iterates whole tensor rows off-chip,
  hitting the 60–66 MHz FSB repeatedly. DOSBox-X charges emulated
  cycles per x86 instruction without modeling FSB bandwidth, so a
  real-HW cache miss that costs ~40 FSB cycles looks free to the
  emulator.
- **FPU dependency chains.** Real P54C FADD/FMUL throughput is 3
  cycles but latency is higher, and our attention/matmul inner
  loops keep the same x87 register live across several
  dependent ops. DOSBox-X doesn't model the dependency-chain
  stalls.
- **Pipeline stalls.** P54C is a dual-issue in-order pipeline;
  memory-access pairing rules are nontrivial. DOSBox-X doesn't
  model pairing at all — it just charges one cycle per op.

What this means for DOSBox-X users:

- **Projecting real-HW wall time:** `cycles = fixed 25000`
  (~90000 / 3.56) is the rough equivalent for this workload. Treat
  this as a per-workload approximation rather than a universal
  constant; the multiplier shifts with cache/FPU behavior.
- **CI / gate runs:** `tests/run-golden.sh` stays at the default
  DOSBox-X config (cycles=90000, 2–3 min) — that's the right
  tradeoff for a correctness gate, which only needs the first 192
  bytes to match. Real-HW wall time is irrelevant to the gate.
- **Memory footprint is accurate across both.** Peak memory on
  real hardware (19.79 MB) matches the DOSBox-X projection to the
  byte (19.9 MB from `bench/results.md`). DPMI allocation sizes
  don't depend on CPU simulation fidelity — `memsize = 48` +
  CWSDPMI r7 in DOSBox-X is a faithful model of the 48 MB real
  target for memory planning.

## Model size vs. 48 MB target (Phase 3 final)

Captured post-Phase-3 on commits `74460cf` (flags) `4b4b969` (IDIV→shift)
`fd1480b` (int8 KV cache). All runs under DOSBox-X `memsize = 48,
cycles = fixed 90000` with CWSDPMI r7 to match the target PODP5V83.
"Peak demand" is the virtual-arena drop from `main-entry` to
`after-build_transformer` as reported by
`_go32_dpmi_get_free_memory_information()`. "Swap peak" is the
`CWSDPMI.SWP` file size sampled mid-run. Measurement methodology:
`docs/phase2-memory.md`. Per-experiment wall-time and memory deltas:
`docs/phase3-notes.md`.

| Model + flags                              | Disk     | Config (dim/hidden/layers/vocab/seq) | DOSBox-X peak  | Real PODP5V83 peak | Swap peak | Fit at memsize=48     | DOSBox-X wall (n=200) |
|---------------------------------------------|---------:|---------------------------------------|---------------:|-------------------:|----------:|-----------------------|---------------------:|
| `stories15M_q80`                            | 16.31 MB | 288 / 768 / 6 / 32000 / 256           | **17.44 MB**   | **19.79 MB**       | 0         | **fits, no swap**     | **2m59s**            |
| `stories42M_q80` (default)                  | 42.27 MB | 512 / 1376 / 8 / 32000 / 1024         | 51.06 MB       | _pending_          | ~4.5 MB   | fits, paging          | —                    |
| `stories42M_q80 --max-seq-len 256`          | 42.27 MB | 512 / 1376 / 8 / 32000 / **256 (cap)** | **44.69 MB**   | _pending_          | **0**     | **fits, no swap**     | —                    |

Physical ceiling at `memsize=48`: **46.55 MB**. The headline is the last
row — `stories42M_q80 + --max-seq-len 256 + int8 KV` is the first
configuration that fits 42M onto the 48 MB target with zero paging.

The Phase 3 "Peak demand" column captured the virtual-arena drop
from `main-entry` to `after-build_transformer` (arena alone). The
"Real PODP5V83 peak" column captures whole-process high-water
(arena + activations + stdio buffers + heap fragmentation) as
reported by `--benchmark`'s `peak mem` line. Both are correct in
their own frame — see [`bench/results.md`](../bench/results.md)
§"Observations" for the full reconciliation. The 15M real-HW peak
(19.79 MB) matches the DOSBox-X `--benchmark` measurement
(19.9 MB) to the byte.

### Per-phase memory history — `stories15M_q80`

| Phase            | Peak demand | Swap peak | Wall time (n=200) | Gate    |
|------------------|------------:|----------:|------------------:|---------|
| Phase 1 port     | 55.13 MB    | ~10 MB    | ~5m16s            | primary |
| Phase 2 (arena + dequant) | 19.88 MB | 0    | 5m45s             | primary |
| Phase 3 (#3 flags)| 19.88 MB   | 0         | 2m43s             | primary |
| Phase 3 (#3 A+B)  | 19.88 MB   | 0         | 2m40.5s           | primary |
| Phase 3 (#3+#4)   | **17.44 MB** | 0       | **2m59s**         | primary |

- `stories15M` clears the Phase 1 primary 192-byte gate at every Phase 3
  stage — no tolerance fallback needed despite `-ffast-math` and int8 KV.
- Wall-time speedup Phase 2 → Phase 3 final: **1.93×** (5m45s → 2m59s).
- Phase-3 #4 (int8 KV) traded ~18.5s of the Phase-3 #3 speed win for
  2.44 MB of memory headroom. 15M didn't need the memory, but the
  trade-off is good insurance for tighter configs and necessary for
  42M to fit.

### Per-phase memory history — `stories42M_q80`

| Phase            | Config            | Peak demand | Swap peak |
|------------------|-------------------|------------:|----------:|
| Phase 2 (arena + dequant) | seq_len=1024 | 74.50 MB   | ~30 MB    |
| Phase 3 task #1  | `--max-seq-len 256` only | ~50.5 MB | ~4 MB    |
| Phase 3 task #4  | seq_len=1024, int8 KV | 51.06 MB | ~4.5 MB  |
| Phase 3 **final** | `--max-seq-len 256 + int8 KV` | **44.69 MB** | **0** |

Neither task #1 nor task #4 alone is enough to take 42M under the
46.55 MB physical ceiling. Together they are.

### 42M now runs unpaged on 48 MB hardware

`stories42M_q80` previously paged ~30 MB through CWSDPMI's swap file
on the 48 MB target. Phase 3 resolves this. Conclusions for Phase 5:

- **15M is the comfortable target** at 48 MB, no paging, ~2m59s for
  200 tokens under DOSBox-X.
- **42M is the usable stretch target** at 48 MB with
  `--max-seq-len 256`. No paging, full int8-KV-cache advantage. The
  wall-clock number on 42M under DOSBox-X isn't captured here because
  task #5's benchmark is the 15M canonical; capture in Phase 5 on
  real hardware.
- **64 MB+ hardware** runs 42M at full seq_len=1024 with fp32 KV if
  correctness is paramount and CPU is free — reference configuration,
  not primary target.

### Phase 4 / post-v0.1 candidates

- **Int-MAC attention inner loops.** Would likely recoup the ~18.5s
  task-#4 regression. See `docs/phase3-notes.md` §"Task #4 Possible
  follow-up".
- **`--benchmark` CLI harness** — Phase 4 proper. Currently the gate
  script times wall clock; Phase 4 adds per-phase cycle breakdowns.
- **Real PODP5V83 numbers** — Phase 5 exit deliverable. DOSBox-X under-
  represents the IDIV→SAR win (see `docs/optimization-notes.md`
  §"What worked item 4"), so real-hardware wall time is expected to
  look meaningfully different than the DOSBox numbers here.

## Reproducing the benchmark

Phase 4 shipped a reproducible benchmark harness:

- **Host side:** `bench/run.sh` drives `dosbox-x` via
  `tools/dosbox-run.sh`, captures the `--- VELLM BENCHMARK ---` block
  emitted by `vellm.exe --benchmark`, and emits one markdown / CSV /
  TSV row per scenario to stdout or a file.

  ```bash
  make                                    # build vellm.exe
  bench/run.sh                            # full matrix, print to stdout
  bench/run.sh --scenario 15m-default     # one scenario
  bench/run.sh --output bench/results.md --append
                                          # ...or append to results.md
  ```

  Scenarios: `15m-default`, `15m-seq256`, `42m-seq256` (the last one
  is skipped when `models/stories42M_q80.bin` is absent).

- **DOS side:** `BENCH.BAT` (15M) and `BENCH42.BAT`
  (`stories42M_q80 --max-seq-len 256`) are shipped in
  `dist/vellm-cf.tar.gz` / `dist/vellm-cf.zip` produced by
  `make cf-package`. Boot DOS, cd to the deploy dir, run the BAT
  file, capture the benchmark block on paper / camera / serial
  capture.

Numbers land in [`bench/results.md`](../bench/results.md), including
a reserved row for the real PODP5V83 that gets filled in from a
hardware run. See that file's "Provenance" section for DOSBox-X
config details and the host-Linux sanity ceiling.

## Phase 1 gate — regression fingerprint

The byte-identical gate on `stories15M_q80 -t 0 -s 42 -n 200 -i "Once
upon a time"` still passes every stage through Phase 3:

| Config       | Pre-Phase-2 | Post-#3 arena | Post-#7 dequant | Phase 3 (flags) | Phase 3 (A+B+#4 int8 KV) |
|--------------|:-----------:|:-------------:|:---------------:|:---------------:|:------------------------:|
| memsize = 48 | PASS        | PASS          | PASS            | PASS            | PASS                     |

All configurations produce the same 192-byte prefix (`3575c4cc…` md5
on the normalized prefix). That means the Phase 1 primary gate
continues to be the gate-of-record — Phase 3's tolerance gate is a
safety net, not the expected pass path. Even with `-ffast-math`,
dropped `-ffloat-store`, and per-head-quantized int8 KV cache, the
30-token cross-toolchain fingerprint holds.
