# vellm

**vellm runs a 15-million-parameter language model on an 83 MHz
Intel Pentium OverDrive under MS-DOS 6.22 — emitting coherent
TinyStories-domain text at 0.27 tokens per second on a 1995-era
machine, with 48 MB of RAM and no SIMD.**

<!-- docs/vellm-real-hw.jpeg lands after the task-team closes -->
![vellm running on PODP5V83](docs/vellm-real-hw.jpeg)

A port of [karpathy/llama2.c](https://github.com/karpathy/llama2.c)
(specifically `runq.c`, the int8-quantized variant) to MS-DOS 6.22,
targeting a Pentium Overdrive 83 MHz system (PODP5V83, Socket 3,
P54C, 48 MB RAM, CF-to-IDE, CWSDPMI r7 for DPMI). The name is
pronounced *vellum*.

The deliverable is a statically-linked `vellm.exe` that loads an
int8-quantized TinyStories checkpoint and generates text on period
hardware.

## Headline numbers

Canonical 200-token run, `stories15M_q80.bin`, seed 42, temp 0.
Full matrix in [`bench/results.md`](./bench/results.md).

| Platform | Model | Gen tok/s | Wall | Peak MB |
|---|---|---:|---:|---:|
| **Real PODP5V83 (83 MHz)** | 15M q80 | **0.27** | **11m 56s** | 19.8 |
| DOSBox-X (cycles=fixed 90000) | 15M q80 | 0.99 | 2m 59s | 19.9 |
| DOSBox-X (cycles=fixed 90000) | 42M q80, `--max-seq-len 256` | 0.41 | 8m 11s | 46.1 |
| Host Linux (i7-8700K, upstream runq.c) | 15M q80 | ~96 | ~2.1s | — |

Real PODP5V83 42M numbers are pending measurement.

## What this is

A working int8 Llama-architecture inference engine for real-mode
MS-DOS 6.22 on Pentium-class hardware. Everything runs in 32-bit
protected mode via CWSDPMI. The checkpoint format is upstream's
Q8_0 (`runq.c`'s "version 2" export) — we don't invent a new one.
Output is byte-identical to upstream on the first ~30 tokens
(Phase 1 correctness gate: 192-byte prefix diff).

## What this isn't

Not a demo of how far compiler optimization will push a 1995 CPU —
we deliberately leave MMX/SSE off the table (the PODP5V83 predates
MMX). Not a general-purpose LLM runtime; the model is a
15M-parameter TinyStories checkpoint that produces children's
stories in the style of the training corpus. Not a
benchmark-shootout project; real-HW wall-clock is what it is.

## Build

```bash
./tools/build-djgpp.sh      # one-time: installs DJGPP cross-compiler
make                        # cross-builds vellm.exe (+ stubedit fixup)
make -f Makefile.host       # native Linux reference build → run_host
tests/run-golden.sh         # correctness gate: first 192 bytes vs. golden
```

See [`BUILDING.md`](./BUILDING.md) for the full setup walk-through,
prerequisites, and a common-errors section.

## Quick run (DOSBox-X)

```bash
tests/run-golden.sh    # canonical prompt, diffs first 192 bytes vs golden
bench/run.sh           # full benchmark matrix → bench/results.md
```

Both scripts stage inputs to 8.3-safe names internally
(`MODEL.BIN`, `TOKEN.BIN`) — DOS 6.22 has no LFN support.

Expected first paragraph: `Once upon a time, there was a little
girl named Lily…` — the first 192 bytes match the pinned golden,
regardless of whether you're on DOSBox-X or real PODP5V83.

## Benchmarks

Phase 4 delivered a reproducible harness. Numbers land in
[`bench/results.md`](./bench/results.md), produced by
`bench/run.sh` (host-side, drives DOSBox-X) or directly by the
shipping `BENCH.BAT` / `BENCH42.BAT` on real hardware.

```bash
bench/run.sh                             # run full matrix under DOSBox-X
bench/run.sh --scenario 15m-default      # run one scenario
bench/run.sh --output bench/results.md   # write rows to file
```

Sample `--- VELLM BENCHMARK ---` block, from the real PODP5V83 run:

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

[`docs/hardware.md`](./docs/hardware.md) documents the DOSBox-X vs
real-hardware calibration — DOSBox-X at `cycles=fixed 90000` runs
~3.56× faster than the real PODP5V83 for this workload. Memory
footprint matches the emulator projection to the byte.

## Deployment

`make cf-package` produces a CF-card-ready bundle in two formats:

- `dist/vellm-cf.zip` (Windows-mount-and-copy workflow)
- `dist/vellm-cf.tar.gz` (Linux scp workflow)

Both contain `VELLM.EXE`, `CWSDPMI.EXE` + license, tokenizer +
model, and the batch-file runners (`RUN.BAT` for the demo,
`BENCH.BAT` / `BENCH42.BAT` for benchmarks), plus a DOS-formatted
`README.TXT`. If `models/stories42M_q80.bin` is present at build
time, the 42M model and its benchmark are included.

## Status

Phases 0–5 complete; v0.1 ships with 15M confirmed on real
hardware. 42M real-hardware numbers pending; see
[`PLAN.md`](./PLAN.md) for the roadmap.

## Layout

```
src/               vellm.c (forked from runq.c with DOS-PORT deltas)
tools/             build-djgpp.sh, dosbox-run.sh, dosbox-launch.sh,
                   dosbox-x.conf (Pentium/48 MB profile)
tests/             run-golden.sh, golden/once_upon_a_time.txt
bench/             run.sh (host harness), BENCH.BAT / BENCH42.BAT
                   (DOS-side), results.md (reproducible numbers)
docs/              phase0…phase5 notes, hardware.md, format.md,
                   fine-tune.md, optimization-notes.md
vendor/            cwsdpmi/ (vendored DPMI host), llama2.c/ (pinned
                   upstream snapshot with UPSTREAM_SHA)
models/            gitignored; drop stories15M_q80.bin,
                   stories42M_q80.bin, tokenizer.bin here
Makefile           cross-DJGPP build
Makefile.host      native Linux reference build
```

## Credits

Built on Andrej Karpathy's
[llama2.c](https://github.com/karpathy/llama2.c) — `vellm`
is a port of `runq.c` (the int8 variant) to DJGPP +
CWSDPMI. Upstream SHA pinned in
[`vendor/llama2.c/UPSTREAM_SHA`](./vendor/llama2.c/UPSTREAM_SHA);
full attribution matrix in
[`THIRD-PARTY.md`](./THIRD-PARTY.md).

## License

MIT. See [`LICENSE`](./LICENSE) for vellm itself, `vendor/cwsdpmi/`
for CWSDPMI terms (Sandmann's distribution license), and
[`THIRD-PARTY.md`](./THIRD-PARTY.md) for the complete upstream
attribution matrix.
