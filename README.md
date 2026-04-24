# vellm

vellm (pronounced *[vellum](https://en.wikipedia.org/wiki/Vellum)*, after the medieval parchment) is a port of [karpathy/llama2.c](https://github.com/karpathy/llama2.c) to MS-DOS 6.22. It runs [TinyStories](https://huggingface.co/karpathy/tinyllamas) 15M and 42M checkpoints — tiny transformer models trained on children's-story text — on 1990s Pentium-class hardware via [DJGPP](https://www.delorie.com/djgpp/) and [CWSDPMI](https://en.wikipedia.org/wiki/DOS_Protected_Mode_Interface).

This project was 100% built agentically using [Claude Code](https://docs.anthropic.com/en/docs/claude-code).

<p align="center">
<a href="#minimum-requirements">Minimum Requirements</a> · <a href="#features">Features</a> · <a href="#usage">Usage</a> · <a href="#benchmarks">Benchmarks</a> · <a href="#building">Building</a> · <a href="#acknowledgments">Acknowledgments</a> · <a href="#license">License</a>
</p>

<p align="center">
<a href="docs/vellm-real-hw.jpeg"><img src="docs/vellm-real-hw.jpeg" width="540" alt="vellm running stories42M_q80 on a real Intel Pentium Overdrive 83 MHz under MS-DOS 6.22"></a>
<br>
<strong>vellm generating tokens on a <a href="https://en.wikipedia.org/wiki/Pentium_OverDrive">Pentium OverDrive</a> 83MHz</strong>
</p>

---

## Minimum Requirements

- Intel Pentium-class CPU (P5, P54C, or OverDrive)
- 48 megs RAM for `stories42M_q80`; 24 megs is enough for `stories15M_q80`
- MS-DOS 6.22 or compatible (HIMEM.SYS required; EMM386 not recommended)
- IDE / CF storage with ~50 megs free
- CWSDPMI r7 (shipped in the CF package)

## Features

**Inference**
- [TinyStories 15M and 42M](https://huggingface.co/karpathy/tinyllamas) quantized to Q8_0 via upstream `export.py --version 2`
- On-the-fly row dequantization for the token-embedding table (saves 35 megs on 15M)
- int8 KV cache with per-head fp32 scales (halves the KV footprint on 42M)
- Single-arena runtime allocator — one `malloc` at startup, no per-token allocations, deterministic across runs
- `--max-seq-len` cap clamps the KV cache below the checkpoint's native `seq_len` for memory-constrained configs

**Correctness**
- First 192 bytes of output byte-identical to upstream `runq.c` on the canonical prompt — covers tokenizer, attention, FFN, RMSNorm, RoPE, softmax, argmax
- Tolerance fallback: ≥97% whitespace-word positional agreement (for optimizations that trade bit-identity for speed)

**DOS integration**
- Runs in 32-bit protected mode via CWSDPMI
- Works on DOS 6.22 + HIMEM.SYS

## Usage

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

Redirect to file to capture the `--- VELLM BENCHMARK ---` block:
```
C:\VELLM>BENCH.BAT > BENCH15.TXT
C:\VELLM>BENCH42.BAT > BENCH42.TXT
```

**Custom prompts**

TinyStories checkpoints are narrow-domain. Prompts phrased as short children's-book openings produce the best output.

```
REM deterministic (seed 42, greedy) - same output every run
VELLM.EXE STORY15.BIN -z TOKEN.BIN -t 0 -s 42 -i "Once upon a time"

REM random sampling - varied output, time-seeded if -s omitted
VELLM.EXE STORY15.BIN -z TOKEN.BIN -t 0.8 -i "In the old computer"

REM longer generation (default is 256 tokens)
VELLM.EXE STORY15.BIN -z TOKEN.BIN -t 0.8 -n 400 -i "A DOS prompt"

REM 42M for better story quality - must cap seq_len to fit 48 megs
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
| `-L N` | KV cache cap (`--max-seq-len`) | required for 42M on 48 megs: use `-L 128` |
| `-Z PATH` | Tokenizer path | always `TOKEN.BIN` for stock models |
| `--BENCHMARK` / `-B` | Machine-parseable benchmark mode | fixed seed/prompt/temp |

## Benchmarks

Canonical 200-token run, seed 42, temp 0. Full matrix in [`bench/results.md`](./bench/results.md).

| Platform | Model | Tokens | Gen tok/s | Wall | Peak megs |
|---|---|---:|---:|---:|---:|
| **Real PODP5V83 (83 MHz)** | [15M q80](https://huggingface.co/karpathy/tinyllamas) | 200 | **0.27** | **11m 56s** | 19.8 |
| **Real PODP5V83 (83 MHz)** | [42M q80](https://huggingface.co/karpathy/tinyllamas), `-L 128` | 128 | **0.11** | **19m 48s** | **45.0** |
| DOSBox-X (cycles=fixed 90000) | [15M q80](https://huggingface.co/karpathy/tinyllamas) | 200 | 0.99 | 2m 59s | 19.9 |
| DOSBox-X (cycles=fixed 90000) | [42M q80](https://huggingface.co/karpathy/tinyllamas), `-L 256` | 200 | 0.41 | 8m 11s | 46.1 |
| Host Linux (i7-8700K, upstream runq.c) | [15M q80](https://huggingface.co/karpathy/tinyllamas) | 200 | ~96 | ~2.1s | — |

42M uses `--max-seq-len 128` on real hardware to stay under CWSDPMI's ~45 megs ceiling on a 48 megs box; `--benchmark` clamps the 200-token target to the cap. [`docs/hardware.md`](./docs/hardware.md) documents the DOSBox-X calibration — `cycles=fixed 90000` runs ~3.6× faster than the Pentium for this workload.

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

## Building

See [BUILDING.md](./BUILDING.md) for prerequisites, DJGPP cross-compiler install, build commands, testing, CF packaging, and common errors.

## Acknowledgments

- **[Claude Code](https://claude.ai/code)** by [Anthropic](https://www.anthropic.com/)
- **[llama2.c](https://github.com/karpathy/llama2.c)** by Andrej Karpathy — `runq.c` is the base that vellm ports. Upstream SHA pinned at [`vendor/llama2.c/UPSTREAM_SHA`](./vendor/llama2.c/UPSTREAM_SHA). MIT.
- **[TinyStories](https://arxiv.org/abs/2305.07759)** by Ronen Eldan and Yuanzhi Li; checkpoints trained by Karpathy.
- **[DJGPP](https://www.delorie.com/djgpp/)** by DJ Delorie — the 32-bit DOS GCC port.
- **[CWSDPMI](https://www.delorie.com/pub/djgpp/current/v2misc/)** by Charles W. Sandmann — DOS DPMI host. Redistributed per its license (see [`vendor/cwsdpmi/`](./vendor/cwsdpmi/)).
- **[DOSBox-X](https://dosbox-x.com/)** — DOS emulator for pre-hardware testing.
- **[build-djgpp](https://github.com/andrewwutw/build-djgpp)** by Andrew Wu — installer that `tools/build-djgpp.sh` wraps.

Full attribution matrix: [THIRD-PARTY.md](./THIRD-PARTY.md).

## License

MIT. See [LICENSE](./LICENSE) for vellm itself, [`vendor/cwsdpmi/`](./vendor/cwsdpmi/) for CWSDPMI's redistribution terms, and [`vendor/llama2.c/LICENSE`](./vendor/llama2.c/LICENSE) for upstream llama2.c.
