# Building vellm

Practical build, test, and deploy instructions for contributors. See
[`PLAN.md`](./PLAN.md) for the phased roadmap and
[`CLAUDE.md`](./CLAUDE.md) for the day-to-day operating guide.

## Prerequisites

Linux host. Tested on Debian 13; any modern glibc distro should work.

System packages (Debian/Ubuntu):

```bash
sudo apt install dosbox-x build-essential bison flex texinfo \
                 libgmp-dev libmpfr-dev libmpc-dev \
                 wget curl unzip zlib1g-dev patch
```

`dosbox-x` is required for `tests/run-golden.sh` and `bench/run.sh`.
The rest are the GCC cross-build toolchain's prerequisites.

Python 3.11+ is only needed to convert your own fp32 `.pt`
checkpoints to Q8_0 via `vendor/llama2.c/export.py --version 2`. The
base build doesn't require Python.

## One-time toolchain install

```bash
./tools/build-djgpp.sh
```

This is a symlink to `~/emulators/scripts/update-djgpp.sh`, the
canonical DJGPP installer shared across retro-build projects. It
clones `andrewwutw/build-djgpp`, builds GCC 12.2.0 + binutils +
newlib for the `i586-pc-msdosdjgpp` target, and installs to
`~/emulators/tools/djgpp/`.

Expect 30–60 minutes and a working network connection. When it
finishes:

```bash
~/emulators/tools/djgpp/bin/i586-pc-msdosdjgpp-gcc --version
```

should print a version banner. The `Makefile` prepends the DJGPP
`bin/` dirs to `PATH` automatically; you don't need to source
anything.

## Build

```bash
make                        # → vellm.exe (cross-DJGPP, stubedited)
make -f Makefile.host       # → run_host (native Linux reference)
make hello                  # Phase 0 smoke test (hello.exe)
make clean
```

`make` ends with a `stubedit vellm.exe minstack=2048k` step; don't
skip it. The DJGPP default 256 KB stack is too small for our
matmul.

## Test

```bash
tests/run-golden.sh         # correctness gate: first 192 bytes vs golden
bench/run.sh                # full benchmark matrix under DOSBox-X
```

`run-golden.sh` is the Phase 1 correctness gate. It runs
`stories15M_q80 -t 0 -s 42 -n 200 -i "Once upon a time"` under
DOSBox-X and diffs the first 192 bytes of stdout against
`tests/golden/once_upon_a_time.txt`. A passing gate produces no
output. Any deviation (optimizer change, fast-math, matmul refactor,
missing DOS-PORT) shows up as a diff here.

`bench/run.sh` drives `dosbox-x` through `tools/dosbox-run.sh`,
parses the `--- VELLM BENCHMARK ---` block, and emits one
markdown/CSV/TSV row per scenario. See
[`bench/results.md`](./bench/results.md) for the current numbers
and [`docs/hardware.md`](./docs/hardware.md) for the DOSBox-X vs
real-HW calibration note.

## Package for real hardware

```bash
make cf-package             # → dist/vellm-cf.zip + dist/vellm-cf.tar.gz
```

Requires `models/stories15M_q80.bin` and `models/tokenizer.bin` in
place. Checkpoints are gitignored; fetch them from the
[karpathy/llama2.c](https://github.com/karpathy/llama2.c) release
page or produce them yourself by running
`vendor/llama2.c/export.py --version 2` on an fp32 `.pt`
checkpoint.

The bundle contains `VELLM.EXE`, `CWSDPMI.EXE` + license,
tokenizer + model (stored with 8.3-safe names like `STORY15.BIN`,
`TOKEN.BIN`), and batch runners (`RUN.BAT` for the demo,
`BENCH.BAT` / `BENCH42.BAT` for benchmarks), plus a DOS-formatted
`README.TXT`. Extract onto a FAT-formatted CF card, boot DOS, run
`RUN.BAT` or `BENCH.BAT`.

## Common errors

- **`i586-pc-msdosdjgpp-gcc: command not found`** — toolchain
  not on PATH. The Makefile exports it automatically; if you're
  invoking the compiler by hand, prepend
  `~/emulators/tools/djgpp/bin:~/emulators/tools/djgpp/i586-pc-msdosdjgpp/bin`.

- **`stubedit: command not found`** — lives in the second PATH
  dir above (`i586-pc-msdosdjgpp/bin`), not the main `bin/` dir.

- **`tests/run-golden.sh: dosbox-x not found`** — install it:
  `sudo apt install dosbox-x`.

- **`make cf-package` error about missing `models/*.bin`** —
  models are gitignored. Fetch from karpathy/llama2.c releases or
  run `vendor/llama2.c/export.py --version 2` on an fp32
  checkpoint.

- **DOSBox-X agent silent for > 30 s during tests** — check for
  a modal "are you sure you want to quit" dialog:
  `DISPLAY=:0 scrot /tmp/dosbox.png`. Phase 4 set `quit warning =
  false` in `tools/dosbox-x.conf` to stop this; any test config
  that overrides that value can reintroduce the wedge.

- **Long filename fails when copying to real CF card** —
  `make cf-package` emits 8.3-safe names (`STORY15.BIN`,
  `STORY42.BIN`, `TOKEN.BIN`) per Phase 4 followup, so this
  shouldn't happen unless files are renamed outside the package
  workflow. DOS 6.22 has no LFN support — `stories15M_q80.bin`
  won't open under DJGPP.

- **x87 vs SSE2 fp divergence on the first 192 bytes** —
  documented in [`docs/phase1-notes.md`](./docs/phase1-notes.md).
  Fix is `-ffloat-store`, which is already in the cross-DJGPP
  CFLAGS. If you've modified flags and the golden gate fails, put
  it back.

## Working on the code

- `src/vellm.c` is a forked-and-annotated port of upstream
  `runq.c`. Every DOS-specific change is tagged `// DOS-PORT:` so
  the full port diff is enumerable:

  ```bash
  grep 'DOS-PORT:' src/vellm.c
  ```

  Use this tag only for changes required to make upstream compile
  and run correctly under DJGPP (`fopen(..., "rb")`, `size_t` vs
  `ssize_t`, `setvbuf`). Do **not** use it for optimizations or
  refactors.

- `vendor/llama2.c/` is a pinned snapshot at the SHA recorded in
  [`vendor/llama2.c/UPSTREAM_SHA`](./vendor/llama2.c/UPSTREAM_SHA).
  Don't modify — it's the correctness reference for the golden
  gate.

- Primary correctness gate: `tests/run-golden.sh` diffs the first
  192 bytes of stdout against the committed golden. It must keep
  passing on any change. If you're touching floating-point math,
  matmul ordering, quantization, or compiler flags, expect to have
  to justify the gate result.
