# vellm

vellm runs karpathy's [llama2.c](https://github.com/karpathy/llama2.c) (specifically `runq.c`, the int8-quantized variant) on MS-DOS 6.22. It targets 1990s Pentium-class hardware: TinyStories 15M and 42M checkpoints generate text at fractional tokens per second on an 83 MHz Intel Pentium Overdrive with 48 MB of RAM, using CWSDPMI for 32-bit protected mode and [DJGPP](https://www.delorie.com/djgpp/) as the cross-compiler. The name is pronounced *vellum*.

[TinyStories](https://arxiv.org/abs/2305.07759) is a synthetic dataset of very short, simple stories — roughly the vocabulary and sentence complexity of a children's picture book — created by Microsoft Research in 2023 to study how small a language model can be and still produce coherent English. Karpathy's [stories15M and stories42M](https://huggingface.co/karpathy/tinyllamas) are tiny Llama-architecture models (15 and 42 million parameters, vs. the 7+ billion in a typical modern LLM) trained on that dataset. They produce grammatical, on-topic prose in the children's-story domain — *"Once upon a time there was a little girl named Lily. She loved to play outside..."* — and are small enough to quantize to Q8_0 (~17 and 42 MB on disk) and fit in a 48 MB DOS machine's memory. That's what makes this project possible at all.

This project was 100% built agentically using [Claude Code](https://docs.anthropic.com/en/docs/claude-code).

<p align="center">
<a href="#requirements">Requirements</a> · <a href="#features">Features</a> · <a href="#usage">Usage</a> · <a href="#benchmarks">Benchmarks</a> · <a href="#building">Building</a> · <a href="#deployment">Deployment</a> · <a href="#testing">Testing</a> · <a href="#acknowledgments">Acknowledgments</a> · <a href="#license">License</a>
</p>

<p align="center">
<a href="docs/vellm-real-hw.jpeg"><img src="docs/vellm-real-hw.jpeg" width="540" alt="vellm running stories42M_q80 on a real Intel Pentium Overdrive 83 MHz under MS-DOS 6.22"></a>
<br>
<strong>vellm generating tokens with stories42M_q80 on a real Intel Pentium OverDrive 83 MHz</strong>
</p>

---

## Requirements

**Target hardware**
- Intel Pentium-class CPU (P5, P54C, or OverDrive)
- 48 MB RAM for `stories42M_q80`; 24 MB is enough for `stories15M_q80`
- MS-DOS 6.22 or compatible (HIMEM.SYS required; EMM386 not recommended)
- IDE / CF storage with ~50 MB free
- CWSDPMI r7 (shipped in the CF package)

**Build host**
- Linux (Debian 13 tested; any modern distro should work)
- `apt install dosbox-x build-essential bison flex texinfo libgmp-dev libmpfr-dev libmpc-dev wget curl unzip zlib1g-dev patch`
- See [BUILDING.md](./BUILDING.md) for the full walk-through

## Features

**Inference**
- [TinyStories 15M and 42M](https://huggingface.co/karpathy/tinyllamas) quantized to Q8_0 via upstream `export.py --version 2`
- On-the-fly row dequantization for the token-embedding table (saves 35 MB on 15M)
- int8 KV cache with per-head fp32 scales (halves the KV footprint on 42M)
- Single-arena runtime allocator — one `malloc` at startup, no per-token allocations, deterministic across runs
- `--max-seq-len` cap clamps the KV cache below the checkpoint's native `seq_len` for memory-constrained configs

**DOS integration**
- Pure DJGPP build — cross-compiled on Linux, runs in 32-bit protected mode via CWSDPMI
- Works with plain DOS 6.22 + HIMEM.SYS — no EMM386, no MSCDEX, no third-party driver required
- 8.3 filenames throughout so DOS FAT sees everything natively (no LFN support on DOS 6.22)
- `make cf-package` produces a self-contained `vellm-cf.tar.gz` / `.zip` bundle

**Hardware detection**
- Startup banner reports actual CPU brand (via CPUID family/model friendly-name lookup), measured MHz (rdtsc over BIOS tick window, ~0.1 MHz precision), physical RAM breakdown (INT 15h E801h), and DOS + BIOS versions

**Benchmarking**
- `--benchmark` mode: fixed canonical prompt, seed, temp; emits a machine-parseable `--- VELLM BENCHMARK ---` block with `tokens`, `wall ms`, `prompt tok/s`, `gen tok/s`, `peak mem`
- `bench/run.sh` harness drives DOSBox-X and parses results into [`bench/results.md`](./bench/results.md)
- `BENCH.BAT` / `BENCH42.BAT` for direct on-hardware benchmarks, with a spinner + progress counter

**Correctness**
- First 192 bytes of output byte-identical to upstream `runq.c` on the canonical prompt — a cross-toolchain fingerprint that proves tokenizer, attention, FFN, RMSNorm, RoPE, softmax, and argmax are all functioning identically
- Tolerance fallback: ≥97% whitespace-word positional agreement (for optimizations that trade bit-identity for speed)

## Usage

After [deploying to DOS](#deployment), at a `C:\VELLM>` prompt:

**Canonical demo**
```
C:\VELLM>RUN.BAT
```
Generates the same 200-token *"Once upon a time, there was a little girl named Lily..."* output as the pinned correctness fingerprint. Takes ~12 min on a Pentium/83.

**Benchmarks**
```
C:\VELLM>BENCH.BAT           Canonical 200-token 15M benchmark, ~12 min
C:\VELLM>BENCH42.BAT         42M at --max-seq-len 128, 128 tokens, ~20 min
```

Redirect to file to capture the `--- VELLM BENCHMARK ---` block for later transfer:
```
C:\VELLM>BENCH.BAT > BENCH15.TXT
C:\VELLM>BENCH42.BAT > BENCH42.TXT
```

**Custom prompts**
```
REM deterministic (seed 42, greedy) - same output every run
VELLM.EXE STORY15.BIN -z TOKEN.BIN -t 0 -s 42 -i "Once upon a time"

REM random sampling - varied output, time-seeded if -s omitted
VELLM.EXE STORY15.BIN -z TOKEN.BIN -t 0.8 -i "In the old computer"

REM longer generation (default is 256 tokens)
VELLM.EXE STORY15.BIN -z TOKEN.BIN -t 0.8 -n 400 -i "A DOS prompt"

REM 42M for better story quality - must cap seq_len to fit 48 MB
VELLM.EXE STORY42.BIN -z TOKEN.BIN -L 128 -t 0.8 -i "The floppy drive"
```

**Flag reference**

| Flag | Meaning | Typical |
|---|---|---|
| `-T N` | Temperature | `0` = greedy/deterministic; `0.8–1.0` = creative |
| `-P N` | Top-p (nucleus) sampling | default `0.9` |
| `-S N` | Random seed | omit for time-based; fix for reproducibility |
| `-N N` | Max tokens to generate | default `256` |
| `-I "..."` | Prompt string | quote it |
| `-L N` | KV cache cap (`--max-seq-len`) | required for 42M on 48 MB: use `-L 128` |
| `-Z PATH` | Tokenizer path | always `TOKEN.BIN` for stock models |
| `--BENCHMARK` / `-B` | Machine-parseable benchmark mode | fixed seed/prompt/temp |

TinyStories checkpoints are narrow-domain — prompts phrased as short children's-book openings produce the best output. Outside that domain the model still generates grammatical text but it won't be on-topic.

## Benchmarks

Canonical 200-token run, seed 42, temp 0. Full matrix in [`bench/results.md`](./bench/results.md).

| Platform | Model | Tokens | Gen tok/s | Wall | Peak MB |
|---|---|---:|---:|---:|---:|
| **Real PODP5V83 (83 MHz)** | [15M q80](https://huggingface.co/karpathy/tinyllamas) | 200 | **0.27** | **11m 56s** | 19.8 |
| **Real PODP5V83 (83 MHz)** | [42M q80](https://huggingface.co/karpathy/tinyllamas), `-L 128` | 128 | **0.11** | **19m 48s** | **45.0** |
| DOSBox-X (cycles=fixed 90000) | [15M q80](https://huggingface.co/karpathy/tinyllamas) | 200 | 0.99 | 2m 59s | 19.9 |
| DOSBox-X (cycles=fixed 90000) | [42M q80](https://huggingface.co/karpathy/tinyllamas), `-L 256` | 200 | 0.41 | 8m 11s | 46.1 |
| Host Linux (i7-8700K, upstream runq.c) | [15M q80](https://huggingface.co/karpathy/tinyllamas) | 200 | ~96 | ~2.1s | — |

42M on real hardware uses `--max-seq-len 128` to stay under CWSDPMI's ~45 MB physical ceiling on a 48 MB box; `--benchmark` clamps the canonical 200-token target to the cap. [`docs/hardware.md`](./docs/hardware.md) documents the DOSBox-X ↔ real-hardware calibration — DOSBox-X at `cycles=fixed 90000` runs ~3.6× faster than real silicon for this workload.

Sample `--- VELLM BENCHMARK ---` block (real PODP5V83 15M run):

```
cpu        : Intel Pentium OverDrive
cpu mhz    : 83.0
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

The banner printed to stderr at every startup shows the actual hardware — friendly CPU name, measured MHz, RAM breakdown, DOS + BIOS versions. Visible in the screenshot above.

## Building

Requires the DJGPP cross-compiler. One-time install (~30–60 min):

```bash
./tools/build-djgpp.sh
```

This wraps Andrew Wu's [build-djgpp](https://github.com/andrewwutw/build-djgpp) to install DJGPP 12.2.0 into `$HOME/emulators/tools/djgpp/`. Override with `DJGPP_PREFIX=/path/to/djgpp`.

Then build vellm:

```bash
make                        # cross-builds vellm.exe
make -f Makefile.host       # native Linux reference build (run_host)
tests/run-golden.sh         # correctness gate: first 192 bytes vs. golden
```

See [BUILDING.md](./BUILDING.md) for prerequisites, the full dependency list, and a common-errors section.

## Deployment

```bash
make cf-package             # produces dist/vellm-cf.{zip,tar.gz}
```

Contents: `VELLM.EXE`, `CWSDPMI.EXE` + license, tokenizer + model(s), batch-file runners (`RUN.BAT`, `BENCH.BAT`, `BENCH42.BAT`), DOS-formatted `README.TXT`. If `models/stories42M_q80.bin` is present at build time, the 42M model and its benchmark are included.

Mount a FAT-formatted CF card and extract:

```bash
tar xzf dist/vellm-cf.tar.gz -C /mnt/cf
```

Then boot your DOS machine and run `RUN.BAT` or one of the benchmark scripts.

## Testing

- **Pre-hardware**: [DOSBox-X](https://dosbox-x.com/) 2025.02.01 with `tools/dosbox-x.conf` (Pentium, 48 MB, `cycles=fixed 90000`). `tests/run-golden.sh` runs the correctness gate headless; `bench/run.sh` drives the benchmark matrix.
- **Real hardware**: Intel Pentium Overdrive PODP5V83 on an Anigma LP4IP1 board, 48 MB RAM, MS-DOS 6.22, CF-to-IDE storage. The CF package drops straight onto the card.

## Acknowledgments

- **[Claude Code](https://claude.ai/code)** by [Anthropic](https://www.anthropic.com/)
- **[llama2.c](https://github.com/karpathy/llama2.c)** by Andrej Karpathy — vellm ports `runq.c` with a minimal annotated diff. Upstream SHA pinned at [`vendor/llama2.c/UPSTREAM_SHA`](./vendor/llama2.c/UPSTREAM_SHA). MIT license.
- **[TinyStories](https://arxiv.org/abs/2305.07759)** — Ronen Eldan and Yuanzhi Li. Model checkpoints trained on the TinyStories dataset by Karpathy.
- **[DJGPP](https://www.delorie.com/djgpp/)** by DJ Delorie and many contributors — the 32-bit DOS GCC port used as the cross-compiler.
- **[CWSDPMI](http://sandmann.dotster.com/cwsdpmi/)** by Charles W. Sandmann — the DPMI host required at DOS runtime. Redistributed per its license (see [`vendor/cwsdpmi/`](./vendor/cwsdpmi/)).
- **[DOSBox-X](https://dosbox-x.com/)** — pre-hardware testing environment.
- **[build-djgpp](https://github.com/andrewwutw/build-djgpp)** by Andrew Wu — the cross-toolchain installer `tools/build-djgpp.sh` wraps.

Full attribution matrix: [THIRD-PARTY.md](./THIRD-PARTY.md).

## License

MIT. See [LICENSE](./LICENSE) for vellm itself, [`vendor/cwsdpmi/`](./vendor/cwsdpmi/) for CWSDPMI's redistribution terms, [`vendor/llama2.c/LICENSE`](./vendor/llama2.c/LICENSE) for upstream llama2.c, and [THIRD-PARTY.md](./THIRD-PARTY.md) for the complete attribution matrix.
