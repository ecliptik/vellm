# vellm — Implementation Plan

TinyStories inference on MS-DOS 6.22, targeting a Pentium Overdrive 83 MHz system.

This document is the phased implementation roadmap. See `CLAUDE.md` for the day-to-day operating guide.

## Decisions locked in

- **Target CPU:** Pentium Overdrive PODP5V83 (83 MHz, Socket 3, P54C core, no MMX). 486DX2/66 is a stretch benchmark target, not a correctness target.
- **Target OS:** MS-DOS 6.22, real-mode boot. DPMI via CWSDPMI r7.
- **Storage:** CF-to-IDE adapter (2 GB / 4 GB cards). No NVMe driver work.
- **Repo name:** `vellm` (pronounced vellum).
- **Upstream baseline:** `runq.c`, **not** `run.c` (see section below).
- **First-light model:** `stories15M_q80.bin` (~15 megs, upstream Q8_0 format) for the correctness gate. Larger domain-tuned checkpoints land in Phase 6.
- **Correctness floating point:** no `-ffast-math` in Phase 1 — Phase 1 exit criteria is byte-identical stdout vs. upstream Linux `runq.c` on the same seed/prompt. `-ffast-math` re-enables in Phase 3 with tolerance-based diffing.
- **Build delivery:** user mounts CF card on Linux, runs `make install CF=/mnt/…`.
- **Toolchain location:** DJGPP cross-compiler at `~/emulators/tools/djgpp/`, matching the existing `retro68-build` convention in the Macintosh retro pipeline.

## Upstream baseline: `runq.c`, not `run.c`

vellm ports Karpathy's `llama2.c/runq.c` — the int8-quantized inference variant — rather than the fp32 `run.c`. Rationale:

- `runq.c` already implements per-group symmetric int8 quantization (group size 64) with fp32 scales, which is what this project needs on the Pentium Overdrive anyway.
- Starting from `runq.c` eliminates the bulk of the originally-planned Phase 2 work: we don't invent a new `.q8` format, we don't write a quantizer, we don't port a dequant-in-matmul path.
- The `runq.c` file format (`.bin` with upstream's header + Q8_0 tensors) is the native format for vellm. Documented in `docs/format.md` as "llama2.c Q8_0 format, unchanged."
- Upstream's `export.py` produces quantized checkpoints on the host. Upstream's released `stories15M_q80.bin` and `stories42M_q80.bin` are the target checkpoints for Phases 1 and beyond.

`run.c` stays around at `vendor/llama2.c/run.c` as a reference for the fp32 correctness intuition, but we do not port it.

## Phases

### Phase 0 — Toolchain + scaffolding

1. Cross-DJGPP via `andrewwutw/build-djgpp` installed to `~/emulators/tools/djgpp/`. Smoke-test with `hello.c`.
2. DOSBox-X (apt). Config at `tools/dosbox-x.conf`: Pentium CPU, 48 megs, cycles tuned for Pentium/83.
3. CWSDPMI vendored at `vendor/cwsdpmi/cwsdpmi.exe`, license alongside.
4. Repo skeleton per layout below.
5. Git remote verified.

**Exit:** a stub `hello.c` cross-compiles to `hello.exe`, runs headless under DOSBox-X, prints hello, test script diffs captured stdout against expected output.

### Phase 1 — Baseline port of `runq.c` (int8)

Goal: get `runq.c` cross-compiling under DJGPP and producing byte-identical output to upstream on the Pentium.

**Prereq (download before starting):** place upstream's `stories15M_q80.bin` (~15 megs) and `tokenizer.bin` (32K vocab) in `models/`. Source: the release assets on the `karpathy/llama2.c` repo.

1. Fork `vendor/llama2.c/runq.c` to `src/vellm.c`. Keep `vendor/llama2.c/runq.c` pristine as the correctness reference.
2. Minimum DOS diff — the only code changes vs. upstream are:
   - `setvbuf(stdout, NULL, _IOFBF, 4096)` at top of `main`.
   - Explicit `"rb"` on every `fopen` for binary files.
   - Any `size_t` / `ssize_t` / `ftell` fixes for DJGPP's 32-bit `long`.
   - No `mmap`; upstream already supports the `malloc` + `fread` path.
   - **Every deviation is annotated with a `// DOS-PORT:` comment** so the full port diff is one grep away.
3. `Makefile` cross-build: `-march=pentium -O2 -fomit-frame-pointer -Wall -Isrc`. **No `-ffast-math`** — it perturbs fp accumulation and breaks the byte-identical gate. Post-link: `stubedit vellm.exe minstack=2048k`.
4. `Makefile.host`: native gcc build of `vendor/llama2.c/runq.c` → `run_host`. Used only to generate the reference output.
5. `tests/run-golden.sh`: builds both, runs both with `stories15M_q80.bin -t 0 -s 42 -n 200 -i "Once upon a time"`, runs the DJGPP binary headless under DOSBox-X with stdout captured to a mounted host dir, diffs captured output against the native host output. Empty diff = pass.
6. `make install CF=/mnt/…` copies `vellm.exe`, `cwsdpmi.exe`, `stories15M_q80.bin`, `tokenizer.bin`, `RUN.BAT` to the mount.

**Exit:** `vellm.exe stories15M_q80.bin -t 0 -s 42 -n 200 -i "Once upon a time"` produces output whose **first 192 bytes (≈ 30 tokens / first paragraph) are byte-identical** to `tests/golden/once_upon_a_time.txt`, both under DOSBox-X and on real hardware (PODP5V83). Divergence after that prefix is expected and acceptable: it reflects sub-ULP differences between DJGPP's libm transcendentals (older glibc era) and modern host glibc, plus x87/SSE2 rounding semantics — documented in `docs/phase1-notes.md`. 30 matching tokens across independent FP implementations is a strong correctness fingerprint: it proves tokenizer, embedding lookup, attention, FFN, RMSNorm, RoPE, softmax, and argmax are all functioning identically. Phase 3 upgrades this gate to a tolerance-based logit-trace diff (cosine similarity over the full 200-token trace) when `-ffast-math` is re-enabled.

### Phase 2 — Memory hardening and DOS-specific robustness

Goal: make it robust on 48 megs of real DOS, not just technically-working. Original Phase 2 (quantizer + custom format) is **eliminated** — upstream `runq.c`'s Q8_0 format is already our format.

1. Allocation audit. Confirm `stories15M_q80.bin` fits resident alongside tokenizer, KV cache, and stdio buffers. Document peak resident memory (via `_go32_dpmi_get_free_memory_information` or equivalent).
2. Pre-allocate one arena at startup for all model tensors and KV cache. No per-token allocations in the hot path, no DJGPP-allocator fragmentation across generations.
3. **On-the-fly token-embedding dequant.** Upstream `runq.c`'s `memory_map_weights()` dequantizes the full `vocab_size × dim` token-embedding table to fp32 at load (35 megs for 15M on a 32K vocab — more than 2× the 17 megs checkpoint, and the single largest resident allocation). For a 48 megs target this is untenable. Change: keep the embedding in Q8_0 form, dequantize one row on demand at each embedding lookup. Speed cost on an 83 MHz Pentium is negligible (~130 µs/token) vs. matmul time; memory goes from 35 megs to ~1 KB transient. This is a memory-vs-speed tradeoff upstream resolved in favor of speed; for vellm's constraints, memory wins.
4. Determinism check: run the Phase 1 command three times with the same seed, verify byte-identical output across runs (catches uninitialized-memory bugs that DJGPP's allocator can otherwise mask).
5. Push the upper bound: try `stories42M_q80.bin` — if it fits (now plausible with on-the-fly dequant), document it as an additional target; if it doesn't, document the RAM boundary in `docs/hardware.md`.
6. The Phase 0 `src/quant.{c,h}` stubs can be deleted — we never need our own quant code.

**Exit:** 15M q80 runs to completion without swapping, deterministic across runs, peak-memory number captured. Optional upper-bound test with 42M documented.

### Deferred (after POC)

- **Phase 3** — Pentium U/V pipe tuning, blocked matmul, fixed-point matmul experiments. `now_cycles()` via `rdtsc` available from day 1. `-ffast-math` enabled here behind a tolerance-based diff against the Phase 1 byte-identical gate.
- **Phase 4** — Full benchmark harness. `--benchmark` CLI flag parsed from day 1, emits basic tok/s now; full harness later.
- **Phase 5** — README, BUILDING, hardware matrix, floppy-sized release zip. Ships v0.1 with TinyStories.

### Phase 6 — Domain fine-tuning (post-v0.1)

Train a ~30M-parameter model from scratch on a narrow domain corpus (starting with DOS / vintage computing knowledge) so `vellm.exe` answers period-appropriate questions, not just generates children's stories. Ship as v0.2.

1. **Data collection** — Ralf Brown's Interrupt List, USENET `comp.os.msdos.*` archives, FreeDOS docs, textfiles.com, period books in text form. Cleanup + dedupe. Target 200–800 megs cleaned text.
2. **Format as Q&A pairs** — structure training data with `<|user|>` / `<|assistant|>` delimiters. USENET threads map naturally; synthesize pairs from reference material programmatically.
3. **Train a custom BPE tokenizer** — SentencePiece, vocab 2048–4096, domain tokens (`HIMEM.SYS`, `INT 21H`, etc.) become single tokens.
4. **Train the model** — `vendor/llama2.c/train.py`. Pilot run on the host i7-8700K (few hundred K tokens) to validate pipeline, then full training run on a rented GPU (RTX 4090 or A100 — a few hours, $5–20 per attempt on Vast.ai/RunPod).
5. **Export + quantize + deploy** — reuse Phase 2 tooling. Final artifact: `domain.q8` on a CF card.

Prerequisite: Phases 1 and 2 complete. This is a host-side Python pipeline; no DOS-binary changes required. Details in `docs/fine-tune.md`.

**Exit:** a 30M-param domain-tuned model answers a held-out Q&A test set more usefully than a stock `stories*` model, and runs on real hardware at an acceptable (if slow) tok/s.

### Stretch (post-v0.2)

- 486DX2/66 comparison benchmark run.
- Serial debug wrapper (direct 2F8h/IRQ3 port writes). `timing.c` stubs this from day 1, behind `-DVELLM_SERIAL_DEBUG`.
- TSR inference service over INT 2Fh. Loaded once, queryable by any DOS program.
- Additional domain models reusing the Phase 6 pipeline: BBS-assistant (serial/COM2 chatbot), interactive-fiction engine, period-style tech-writer, DOS-era code-completion TSR.
- Floppinux port.
- Interactive TUI.

## Repository layout

```
vellm/
├── src/
│   ├── vellm.c              # main inference, forked from upstream runq.c
│   ├── matmul.{c,h}         # isolated matmul, swappable impl (Phase 3)
│   ├── quant.{c,h}          # scaffolding stubs; deleted in Phase 2 (runq.c format is native)
│   ├── tokenizer.{c,h}      # BPE, from upstream
│   └── timing.{c,h}         # rdtsc + serial-debug stubs
├── tools/
│   ├── build-djgpp.sh       # installs andrewwutw/build-djgpp to ~/emulators/tools/djgpp
│   ├── dosbox-x.conf        # Pentium/48MB profile
│   ├── dosbox-run.sh        # headless run + stdout capture
│   └── quantize.py          # host-side fp32 → int8 (Phase 2)
├── tests/
│   ├── run-golden.sh
│   └── golden/
│       └── once_upon_a_time.txt
├── vendor/
│   ├── cwsdpmi/             # cwsdpmi.exe + license
│   └── llama2.c/            # pinned upstream snapshot + UPSTREAM_SHA
├── models/                  # .gitignored, user drops stories260K.bin here
├── Makefile                 # cross-DJGPP build
├── Makefile.host            # native reference build
├── .gitignore
├── LICENSE                  # MIT
├── CLAUDE.md
├── PLAN.md                  # this doc
└── README.md                # stub, filled in Phase 5
```

## Critical build notes

- Always `fopen(path, "rb")` — DJGPP default is text mode, will mangle checkpoints.
- No `mmap`. Just `malloc` + `fread`. 48 megs on-device is plenty.
- Audit every `ftell`/`ssize_t` from upstream — DJGPP is 32-bit; upstream assumes 64-bit on modern hosts.
- `setvbuf(stdout, NULL, _IOFBF, 4096)` at top of main — unbuffered stdout is slow on DOS.
- Default stack under DPMI is 256 KB. Post-link: `stubedit minstack=2048k`.
- CWSDPMI must be in PATH or same dir as `.exe` on target.
- No `long double` (DJGPP printf for 80-bit is historically buggy).
- No C99 VLAs in hot paths.
- No MMX/SSE — P54C predates MMX.
