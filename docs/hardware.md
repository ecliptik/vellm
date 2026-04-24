# Tested hardware

The primary target is documented in `CLAUDE.md`:

- Intel Pentium Overdrive PODP5V83 (83 MHz, Socket 3, P54C, no MMX)
- 48 MB RAM
- Anigma LP4IP1 motherboard
- CF-to-IDE (2 GB MS-DOS 6.22 + WfW 3.11; 4 GB Win95 OSR2.5)
- ATI Mach64 215CT PCI (VGA text mode only)
- CWSDPMI r7

This file will grow into a full matrix of tested configurations
(including the 486DX2/66 stretch benchmark) as Phase 5 approaches. It
currently captures Phase 2's memory findings and Phase 3's speed +
memory integration numbers.

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

| Model + flags                              | Disk     | Config (dim/hidden/layers/vocab/seq) | Peak demand    | Swap peak | Fit at memsize=48     | Wall time (n=200) |
|---------------------------------------------|---------:|---------------------------------------|---------------:|----------:|-----------------------|------------------:|
| `stories15M_q80`                            | 16.31 MB | 288 / 768 / 6 / 32000 / 256           | **17.44 MB**   | 0         | **fits, no swap**     | **2m59s**         |
| `stories42M_q80` (default)                  | 42.27 MB | 512 / 1376 / 8 / 32000 / 1024         | 51.06 MB       | ~4.5 MB   | fits, paging          | —                 |
| `stories42M_q80 --max-seq-len 256`          | 42.27 MB | 512 / 1376 / 8 / 32000 / **256 (cap)** | **44.69 MB**   | **0**     | **fits, no swap**     | —                 |

Physical ceiling at `memsize=48`: **46.55 MB**. The headline is the last
row — `stories42M_q80 + --max-seq-len 256 + int8 KV` is the first
configuration that fits 42M onto the 48 MB target with zero paging.

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
