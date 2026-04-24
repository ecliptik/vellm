# vellm

A port of [karpathy/llama2.c](https://github.com/karpathy/llama2.c) to MS-DOS
6.22, targeting a Pentium Overdrive 83 MHz system (PODP5V83, 48 MB RAM,
CF-to-IDE, CWSDPMI r7 for DPMI). The name is pronounced *vellum*.

The primary deliverable is a statically-linked `vellm.exe` that loads an
int8-quantized TinyStories checkpoint and generates text on period hardware.

**Status: Phases 0–4 complete.** See [`PLAN.md`](./PLAN.md) for the
phased roadmap and [`CLAUDE.md`](./CLAUDE.md) for the day-to-day operating
guide.

## Quick build

```bash
./tools/build-djgpp.sh      # one-time: installs DJGPP cross-compiler
make                        # cross-builds vellm.exe (+ stubedit fixup)
make -f Makefile.host       # native Linux reference build → run_host
tests/run-golden.sh         # correctness gate: first 192 bytes vs. golden
```

## Quick run (DOSBox-X)

```bash
tools/dosbox-run.sh --exe vellm.exe \
    --args 'STORIES15M_Q80.BIN -z TOKENIZER.BIN -t 0 -s 42 -n 200 -i "Once upon a time"' \
    --include models/stories15M_q80.bin --include models/tokenizer.bin
```

Expected first paragraph: `Once upon a time, there was a little girl
named Lily…` — the first 192 bytes match the pinned golden, regardless
of whether you're on DOSBox-X or real PODP5V83.

## Benchmarks

Phase 4 delivers a reproducible benchmark harness. Numbers land in
[`bench/results.md`](./bench/results.md), produced by
`bench/run.sh` (host-side, drives DOSBox-X) or directly by the
shipping `BENCH.BAT` / `BENCH42.BAT` on real hardware.

```bash
make                                     # build vellm.exe
bench/run.sh                             # run full matrix under DOSBox-X
bench/run.sh --scenario 15m-default      # run one scenario
bench/run.sh --output bench/results.md   # write rows to file
```

Sample `--- VELLM BENCHMARK ---` block:

```
cpu        : GenuineIntel family 5 model 1 stepping 7
model      : MODEL.BIN
ckpt bytes : 17101696
tokens     : 199
prompt tok : 8
gen tok    : 191
wall ms    : 201428
prompt tok/s: 1.01
gen tok/s  : 0.99
peak mem   : 19791872
```

Phase 4 headline: **~1.0 gen tok/s** on the canonical 15M run under
DOSBox-X (Pentium / 48 MB / cycles=fixed 90000), consistent with the
Phase 3 2m59s wall-time baseline. Real-hardware PODP5V83 numbers are a
row reserved in `bench/results.md`, filled in after deploying
`dist/vellm-cf.tar.gz`.

## Deployment

`make cf-package` produces a CF-card-ready bundle in two formats:

- `dist/vellm-cf.zip` (Windows-mount-and-copy workflow)
- `dist/vellm-cf.tar.gz` (Linux scp workflow)

Both contain `VELLM.EXE`, `CWSDPMI.EXE` + license, tokenizer + model,
and the batch-file runners (`RUN.BAT` for the demo, `BENCH.BAT` /
`BENCH42.BAT` for benchmarks), plus a DOS-formatted `README.TXT` with
instructions. If `models/stories42M_q80.bin` is present at build time
the 42M model and its benchmark are included.

See [`docs/hardware.md`](./docs/hardware.md) for the tested-hardware
matrix and [`docs/phase3-notes.md`](./docs/phase3-notes.md) for the
optimization history that produced the current build.

## Layout

```
src/               vellm.c (forked from runq.c with DOS-PORT deltas)
tools/             build-djgpp.sh, dosbox-run.sh, dosbox-launch.sh,
                   dosbox-x.conf (Pentium/48 MB profile)
tests/             run-golden.sh, golden/once_upon_a_time.txt
bench/             run.sh (host harness), BENCH.BAT / BENCH42.BAT
                   (DOS-side), results.md (reproducible numbers)
docs/              phase0…phase4 notes, hardware.md, format.md,
                   fine-tune.md, optimization-notes.md
vendor/            cwsdpmi/ (vendored DPMI host), llama2.c/ (pinned
                   upstream snapshot with UPSTREAM_SHA)
models/            gitignored; drop stories15M_q80.bin,
                   stories42M_q80.bin, tokenizer.bin here
Makefile           cross-DJGPP build
Makefile.host      native Linux reference build
```

## License

MIT. See [`LICENSE`](./LICENSE) for vellm itself, `vendor/cwsdpmi/`
for CWSDPMI terms (Sandmann's distribution license), and
[`THIRD-PARTY.md`](./THIRD-PARTY.md) for the complete upstream
attribution matrix.
