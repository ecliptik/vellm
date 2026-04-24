# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What This Is

`vellm` is a port of karpathy/llama2.c to run under MS-DOS 6.22 on vintage DOS hardware. int8-quantized TinyStories inference, DPMI via CWSDPMI, cross-compiled on Linux via DJGPP. The primary deliverable is a statically-linked `vellm.exe` that loads a checkpoint and generates text on period hardware. v0.1 shipping with real-HW measurements in `bench/results.md`.

See `PLAN.md` for the phased implementation roadmap.

## Target Hardware

Tested configuration:

- **CPU:** Intel Pentium Overdrive PODP5V83 (83 MHz, Socket 3, P54C core, 32 KB L1, **no MMX**)
- **RAM:** 48 megs
- **Board:** Anigma LP4IP1
- **Storage:** CF-to-IDE (2 GB card with MS-DOS 6.22 + WfW 3.11; 4 GB card with Win95 OSR2.5)
- **Video:** ATI Mach64 215CT PCI (VGA text mode used; no graphics needed)
- **DPMI:** CWSDPMI r7

**Minimum plausible hardware:** Intel 486DX or later x86 with a hardware FPU, ≥24 megs of RAM for 15M (≥48 megs for 42M), MS-DOS 6.22 + HIMEM.SYS. DJGPP targets i386+, but the x87 code path needs a real FPU (486SX without a coprocessor won't work). 486DX2/66 is a plausible benchmark-comparison target; untested but should run.

## Toolchain

| Component | Location | Install |
|---|---|---|
| DJGPP cross-compiler | `~/emulators/tools/djgpp/` (override via `DJGPP_PREFIX`) | `tools/build-djgpp.sh` (wraps `andrewwutw/build-djgpp`) |
| DOSBox-X (pre-hardware test) | system | `sudo apt install dosbox-x` |
| CWSDPMI | `vendor/cwsdpmi/cwsdpmi.exe` | vendored |
| Python (host quantizer) | 3.11+ | `vendor/llama2.c/export.py --version 2` (produces Q8_0 checkpoints from fp32 `.pt`) |

Toolchain lives under `~/emulators/tools/` alongside the Mac retro pipeline's `retro68-build/`. The `~/emulators/` hub is the canonical home for cross-project retro infrastructure — see `~/emulators/CLAUDE.md` and `~/emulators/docs/DJGPP.md` for the full convention.

The Makefile sets `PATH` to find `i586-pc-msdosdjgpp-gcc`; no shell activation needed.

## Development Workflow

```
edit on Linux → make → cross-compile vellm.exe
             → tests/run-golden.sh   # headless DOSBox-X + diff vs. reference
             → make install CF=/mnt/cf  # user mounts CF, we copy
             → boot real hardware
```

## Key Commands

```bash
# First-time toolchain install
./tools/build-djgpp.sh             # installs DJGPP into ~/emulators/tools/djgpp

# Build
make                               # cross-DJGPP → vellm.exe
make -f Makefile.host              # native Linux reference build (run_host)

# Test
tests/run-golden.sh                # headless DOSBox-X, diff vs. golden

# Deploy
make cf-package                    # preferred: produces dist/vellm-cf.{zip,tar.gz}
                                   # with binary, CWSDPMI, tokenizer, model(s),
                                   # RUN.BAT / BENCH.BAT / BENCH42.BAT, LICENSE, README
make install CF=/mnt/cf            # direct copy onto a mounted CF (minimal: 15M only)
make dist                          # legacy: zips minimal install payload

# Interactive DOSBox-X session (staged, one-shot)
tools/dosbox-run.sh --interactive --exe build/vellm.exe

# Visible DOSBox-X launcher (for manual testing + screenshot/xdotool)
tools/dosbox-launch.sh                      # repo mounted as C:
tools/dosbox-launch.sh --exe hello.exe      # auto-run on launch
tools/dosbox-launch.sh --kill-first         # restart cleanly
```

## DOSBox-X Interaction (parity with Snow / Basilisk workflow)

`tools/dosbox-launch.sh` brings DOSBox-X up on the local X session (`DISPLAY=:0`) so Claude Code — or a human — can screenshot it and drive it with xdotool, matching how `~/emulators/` runs Snow and Basilisk.

```bash
tools/dosbox-launch.sh --exe hello.exe      # visible launch, C: = repo root,
                                            # D: = vendor/cwsdpmi on DOS PATH

DISPLAY=:0 scrot -u /tmp/dosbox.png         # capture focused window
DISPLAY=:0 xdotool search --name DOSBox windowactivate --sync
DISPLAY=:0 xdotool type --delay 40 'HELLO.EXE'
DISPLAY=:0 xdotool key Return

pkill -x dosbox-x                           # stop it (or Ctrl+F9 in window)
```

**Rules:**

- Use `scrot -u` for screenshots. The Snow-era rule against ImageMagick `import` (it grabs the X pointer and breaks emulator mouse input) generalizes; don't use it here either.
- Always target `DISPLAY=:0` explicitly for scrot/xdotool — shells invoked by Claude Code may inherit an SSH-forwarded `$DISPLAY` that isn't the user's visible desktop. `dosbox-launch.sh` forces `:0` itself; override with `DOSBOX_DISPLAY=...` if actually needed.
- Do not run multiple DOSBox-X instances simultaneously — they contend for the audio device and the window title makes xdotool's `search --name DOSBox` ambiguous. The launcher refuses to start a second one; use `--kill-first` to restart cleanly.
- `xdotool type` uses literal strings. Use `xdotool key Return` for Enter, `xdotool key ctrl+c` for control chords. Use `--delay 40` on `type` — DOSBox-X occasionally drops keys with zero delay.
- `tools/dosbox-run.sh` (the headless runner) and `tools/dosbox-launch.sh` (this visible launcher) are separate tools with different jobs. Don't collapse them.
- `tools/dosbox-x.conf` sets `quit warning = false`. DOSBox-X's default `auto` opens a modal "program still running, really quit?" dialog on SIGTERM, which blocks any scripted shutdown and silently wedges automated runners. If a test agent ever appears stuck with no output, check for this dialog via `DISPLAY=:0 scrot` before digging deeper.

## Critical Rules

- **Always `fopen(path, "rb")`.** DJGPP defaults to text mode; CRLF translation will corrupt checkpoints silently.
- **No `mmap`.** Use `malloc` + `fread`. 48 megs on-device is plenty; upstream already supports the non-mmap path.
- **`size_t` is 32-bit on DJGPP.** Audit every `ftell`/`ssize_t` coming from upstream — llama2.c assumes 64-bit on modern hosts.
- **`-ffast-math` is enabled in the shipping build** (since Phase 3). Phase 1's byte-identical gate was relaxed to a 192-byte prefix once it became clear that full-output identity was infeasible across toolchains — see `docs/phase1-notes.md`. When touching fp-sensitive code (matmul, softmax, RoPE), make sure `tests/run-golden.sh` still passes; the tolerance fallback in the gate is a safety net, not a free pass.
- **Stubedit stack.** Always `stubedit vellm.exe minstack=2048k` as a post-link step; default 256 KB is too small.
- **`setvbuf(stdout, NULL, _IOFBF, 4096)`** at the top of `main` — unbuffered stdout is painfully slow on DOS.
- **No MMX / SSE / SIMD.** P54C predates MMX.
- **No C99 VLAs in hot paths.** Stack under DPMI is constrained.
- **No `long double`.** DJGPP printf formatting for 80-bit has been historically buggy.
- **CWSDPMI must ship alongside `vellm.exe`.** Include it in every CF deploy and every dist zip. Document this in the README.
- **Annotate every DOS-specific deviation from upstream with `// DOS-PORT:`.** vellm is a deliberately-minimal fork of `runq.c`; the goal is that `grep 'DOS-PORT:' src/vellm.c` enumerates the entire port diff. Use this for fopen "rb" changes, `size_t`/`ssize_t` fixes, `setvbuf`, and any other DOS-required code change. Do NOT use it for optimizations or refactors — only for changes required to make upstream compile and run correctly under DJGPP.

## Correctness Gate

The canonical correctness test is a byte-level diff of the **first 192 bytes** (≈ 30 tokens, the first paragraph) of

```
vellm.exe stories15M_q80.bin -t 0 -s 42 -n 200 -i "Once upon a time"
```

against `tests/golden/once_upon_a_time.txt` (first 192 bytes). This is the primary gate run by `tests/run-golden.sh`. Anything that perturbs that prefix — optimizer changes, matmul refactors, missing DOS-PORT — is rejected.

**Why 192 bytes and not 200 tokens:** full-output byte-identity is infeasible across independent fp toolchains — DJGPP's older libm transcendentals diverge sub-ULP from host glibc, and x87 vs SSE2 rounding compounds across ~180 compute ops per token. After ~30 tokens the divergence is enough to flip a softmax argmax, and both sides continue producing grammatically valid but different TinyStories text. The first 192 bytes are stable across every tested fp configuration (native SSE2, native x87 ± `-ffloat-store`, DJGPP x87 ± `-ffloat-store`, DJGPP + `-ffast-math`), which makes them a strong cross-toolchain correctness fingerprint. Full diagnosis in `docs/phase1-notes.md`.

**Tolerance fallback** (Phase 3): if the 192-byte primary gate ever fails, `run-golden.sh` falls through to a ≥97% whitespace-word positional-agreement check. The primary gate has survived `-ffast-math`, IDIV→SAR, and int8 KV cache in the shipping build — the fallback hasn't had to trigger in practice, but exists for future optimizations that do move the prefix.

vellm ports `runq.c` (int8-quantized) not `run.c` (fp32). See `PLAN.md` § "Upstream baseline".

## Do Not

- Do not replace `fopen`/`fread` with `mmap` or any fancier I/O path.
- Do not enable SIMD (MMX/SSE/SSE2) flags. The tested Pentium OverDrive predates all of them, and the 486 stretch target definitely predates them.
- Do not vendor upstream llama2.c as a git submodule — keep a pinned snapshot so we can patch freely. The upstream SHA lives in `vendor/llama2.c/UPSTREAM_SHA`.
- Do not modify toolchain files under `~/emulators/tools/djgpp/`; that directory is shared retro-build infrastructure. Install-only.
- Do not commit checkpoints. `models/` is `.gitignore`d.

## Documentation

- `PLAN.md` — phased implementation plan, decision log
- `BUILDING.md` — prerequisites, DJGPP install, build + test + CF packaging, common errors
- `THIRD-PARTY.md` — full attribution matrix for vendored / runtime-shipped components
- `docs/hardware.md` — tested configurations, DOSBox-X vs real-HW calibration, per-phase memory history
- `docs/format.md` — Q8_0 file format (upstream runq.c's native format)
- `docs/optimization-notes.md` — Phase 3 speed tuning: what won, what didn't, why
- `docs/fine-tune.md` — Phase 6 recipe: training a domain-specific model
- `docs/phase1-notes.md` — Phase 1 integration findings (FP divergence, 8.3 filenames, CWSDPMI paging)
- `docs/phase2-memory.md` — memory audit methodology + per-alloc tables
- `docs/phase3-notes.md` — per-experiment speed + memory deltas with asm-level rationale
- `docs/phase4-notes.md` — `--benchmark` harness + CF package post-mortems
- `bench/results.md` — reproducible benchmark numbers (DOSBox-X, host Linux, real PODP5V83)
- `vendor/llama2.c/UPSTREAM_SHA` — pinned upstream reference
