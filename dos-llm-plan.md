# Project: `dosllm` — TinyStories Inference on DOS 6.22 / Pentium Overdrive

## Target hardware

- **CPU:** Intel Pentium OverDrive PODP5V83 (83 MHz, Socket 3, P54C core w/ 32KB L1)
- **Board:** Anigma LP4IP1, 48 MB RAM
- **Storage:** Transcend 410S NVMe via custom INT 13h driver, plus CF-to-IDE as fallback
- **OS:** MS-DOS 6.22 (pure real-mode boot, no Windows shell)
- **Display:** VGA text mode, no graphics required

## Goal

Port Karpathy's `llama2.c` to run under DJGPP on DOS 6.22, with int8-quantized TinyStories checkpoints, on a 48 MB Pentium OverDrive system. Deliverables:

1. A single statically-linked `.EXE` that loads a checkpoint + tokenizer and generates text from a prompt.
2. An int8 weight quantizer (host-side Python tool) that converts stock llama2.c `.bin` checkpoints into a DOS-friendly format.
3. A deterministic benchmark mode (fixed seed, fixed prompt, greedy sampling) reporting tokens/second and total wall time.
4. A README with build instructions, hardware requirements, and benchmark results.

## Non-goals

- Training. This is inference only.
- GPU/SIMD. No MMX (the PODP5V83 predates it), no SSE, no nothing.
- Windows compatibility. Pure DOS, real-mode boot, DPMI via CWSDPMI.
- Model sizes above TinyStories 42M. 110M and llama2-7B are out of scope.

## Toolchain

- **Host:** Linux or macOS with cross-DJGPP (i586-pc-msdosdjgpp-gcc). Alternatively native DJGPP on DOSBox-X or a real DOS machine for final builds.
- **Compiler:** GCC 12.x via DJGPP port, `-march=pentium -O2 -ffast-math -fomit-frame-pointer`
- **DPMI host:** CWSDPMI r7 (ships with DJGPP, public domain)
- **Python 3.11+ on host** for the quantizer and checkpoint conversion scripts.
- **Reference implementation:** https://github.com/karpathy/llama2.c — treat as upstream, keep a clean diff.

## Phases

### Phase 1 — Baseline port, fp32, small model

Goal: get *something* generating tokens on the target hardware, even if slow.

1. Fork `llama2.c` into a new repo `dosllm`.
2. Set up a cross-DJGPP build in a Makefile. Produce `dosllm.exe`.
3. Verify it runs on DOSBox-X configured as Pentium/83MHz/48MB as a proxy before hitting real hardware.
4. Use the TinyStories **260K** checkpoint (`stories260K.bin`, ~1 MB) as first target — this eliminates memory pressure as a variable.
5. Get deterministic output matching upstream `run.c` on the same seed and prompt. This is the correctness gate.
6. Port to real hardware. Expect pain: file I/O quirks, stdout buffering, DPMI allocator weirdness. Document each.

**Exit criteria:** `dosllm.exe stories260K.bin -t 0 -s 42 -i "Once upon a time"` produces identical output on DOS and on host Linux build of upstream `run.c`.

### Phase 2 — int8 weight quantization

Goal: shrink the 15M model to fit comfortably in RAM and reduce memory bandwidth, which is the actual bottleneck on Pentium.

1. Write `tools/quantize.py` (host-side) that reads a llama2.c `.bin` checkpoint and emits a `.q8` format:
   - Per-tensor symmetric int8 quantization with fp32 scale stored alongside each tensor.
   - Preserve embedding and final classifier in fp32 (they're small and quality-sensitive).
2. Define the `.q8` file format explicitly in `docs/format.md`. Magic bytes, header, tensor table, data blocks. Keep it simple — no protobuf, no flatbuffers, just a fixed C struct written raw.
3. In `dosllm.c`, add a loader branch for `.q8`. Dequantize on-the-fly inside the matmul inner loop: load int8, multiply by scale, accumulate in fp32.
4. Validate: quantized 15M output should be coherent (not identical to fp32, but grammatical and on-topic).

**Exit criteria:** TinyStories 15M quantized fits in <20 MB resident, produces coherent stories, and runs on the Pentium OverDrive without swapping.

### Phase 3 — Pentium-specific optimization

Goal: squeeze the FPU. This is where the project earns its keep as a benchmark.

1. Profile with a cycle-accurate approach: use `rdtsc` via inline asm wrapped in a portable `now_cycles()` helper. Report per-stage timing (matmul, attention, softmax, tokenizer).
2. Hand-tune the matmul inner loop for Pentium U/V pipe pairing. Target: keep both integer pipes fed while the FPU churns on fmul/fadd. Reference: Agner Fog's Pentium optimization manual (pre-MMX edition).
3. Evaluate blocked matmul (e.g., 4x4 or 8x8 tiles) to improve L1 cache reuse. The P54C has 8KB data L1 — tune tile size to fit.
4. Experiment with fixed-point fallback for matmul. Might be faster than fp32 on some paths, might not. Benchmark both.
5. Ensure the allocator isn't fragmenting across inference calls. If it is, pre-allocate one big arena at startup.

**Exit criteria:** Measurable speedup over Phase 2 baseline on the same hardware. Document every optimization with before/after numbers.

### Phase 4 — Benchmark harness

Goal: make this reproducible and postable.

1. Add `--benchmark` mode: fixed seed, fixed prompt ("The old computer hummed to life"), greedy sampling, generate exactly 200 tokens, report:
   - Total wall time
   - Tokens/second (prompt + generation, separately)
   - Peak resident memory
   - CPU model string (via CPUID)
2. Write `bench/run.sh` (host) and `bench/RUN.BAT` (DOS) that execute a standard test matrix.
3. Capture results in `bench/results.md` with columns: hardware, OS, model, quant, tok/s, wall time, notes.
4. Seed the results with: DOSBox-X emulated Pentium/83, real PODP5V83, and a host Linux reference build for sanity.

**Exit criteria:** Anyone with the repo and a DOS machine can reproduce the headline number.

### Phase 5 — Packaging and docs

1. `README.md`: what it is, why it exists, how to build, how to run, headline benchmark, screenshot of it actually running on the real machine.
2. `BUILDING.md`: cross-DJGPP setup on Linux/macOS, native DJGPP setup on DOS, common errors.
3. `docs/format.md`: the `.q8` file format, versioned.
4. `docs/hardware.md`: tested configurations, known-working and known-broken setups.
5. Release: tagged v0.1, pre-built `dosllm.exe` + `stories15M.q8` as a zip under 10 MB so it fits on a floppy if someone's feeling dramatic.

## Repository layout

```
dosllm/
├── src/
│   ├── dosllm.c          # main inference, ported from run.c
│   ├── matmul.c          # isolated matmul, Pentium-tuned
│   ├── matmul.h
│   ├── quant.c           # int8 dequant helpers
│   ├── quant.h
│   ├── tokenizer.c       # BPE, from upstream
│   ├── tokenizer.h
│   └── timing.c          # rdtsc wrapper, benchmark harness
├── tools/
│   ├── quantize.py       # host-side fp32 -> int8 converter
│   └── export.py         # upstream checkpoint export (mirror of llama2.c's)
├── bench/
│   ├── RUN.BAT
│   ├── run.sh
│   └── results.md
├── docs/
│   ├── format.md
│   ├── hardware.md
│   └── optimization-notes.md
├── Makefile              # cross-DJGPP build
├── MAKEFILE.DOS          # native DJGPP build (8.3 filename)
├── README.md
├── BUILDING.md
└── LICENSE               # MIT, matching upstream llama2.c
```

## Specific things to flag to Claude Code

- **Don't silently replace `fopen` with anything fancy.** DOS needs binary mode (`"rb"`), and CRLF translation will corrupt checkpoints if you forget.
- **Avoid `mmap`.** DJGPP has it, sort of, but just `malloc` + `fread` the whole checkpoint. 48 MB is plenty and it's simpler.
- **Watch `size_t` width.** DJGPP is 32-bit, so `size_t` is 32 bits. Upstream llama2.c uses `ssize_t` and `ftell` in ways that assume 64-bit on modern systems — audit every file-size computation.
- **No C99 VLAs in hot paths.** DJGPP supports them but stack is limited under DPMI by default (256 KB unless you link with `stubedit` to raise it).
- **Default stack is too small.** Add a `stubedit dosllm.exe minstack=2048k` step to the Makefile.
- **`printf` is slow on DOS** because stdout is unbuffered by default in many configs. `setvbuf(stdout, NULL, _IOFBF, 4096)` early in `main`.
- **CWSDPMI must be in PATH or same dir as `.exe`** on the target machine. Document this in README.
- **Do not use `long double`.** 80-bit floats are fine on Pentium FPU but DJGPP's printf formatting for them has historically been buggy.

## Stretch goals (post-v0.1)

- A `dosllm.sys`-style TSR that exposes inference over INT 2Fh so other DOS programs can query the model. Entirely silly. Do it.
- Port to Floppinux and get it running off a single 1.44 MB floppy with a smaller checkpoint. The Floppinux work already solved the hard parts.
- Compare PODP5V83 vs. stock 486DX2/66 vs. emulated Pentium across the same benchmark. Publish the graph.
- A minimal curses-style TUI for interactive prompting, because watching tokens stream out on a CRT is the entire point.

## First prompt to Claude Code

> Read `PLAN.md`. Start Phase 1: fork llama2.c into this repo, add a cross-DJGPP Makefile targeting i586-pc-msdosdjgpp, and get `dosllm.exe` building. Use `-march=pentium -O2 -ffast-math -fomit-frame-pointer`. Do not modify inference logic yet — I want a clean baseline port that produces identical output to upstream `run.c` on the same seed. Set up a test that runs both builds with `stories260K.bin -t 0 -s 42 -i "Once upon a time"` and diffs the output.

Keep each phase in its own branch. Squash-merge after the exit criteria check. Tag v0.1 after Phase 5.
