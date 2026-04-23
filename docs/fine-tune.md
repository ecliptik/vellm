# Domain Fine-Tuning — Training a Small Model for vellm

**Status:** planned for Phase 6, post-v0.1. This document is a forward recipe, not a built feature. Do not attempt until Phase 2 (quantized `stories15M` working end-to-end on real hardware) is complete.

## Goal

Produce a model in the 20–40M parameter range, trained on a narrow domain corpus (initially: DOS / vintage computing knowledge), that runs on the Pentium Overdrive via `vellm.exe` and answers period-appropriate questions more usefully than `stories*` models can.

## Why from scratch (not fine-tune)

The `stories260K`/`stories15M`/`stories42M` checkpoints are trained exclusively on TinyStories — children's fiction at ~3-year-old reading level. They do not encode general knowledge. Fine-tuning them on a DOS corpus produces confused stories about Lily and Timmy debugging `CONFIG.SYS`, not a working help system. Larger general-purpose bases (GPT-2 small, Pythia-70M) are too big to run on 48 MB once we account for KV cache.

The practical path: **train from scratch** using llama2.c's own training code (`vendor/llama2.c/train.py`), on a domain corpus sized to a 20–40M-parameter model. At this scale, data quality matters vastly more than compute efficiency.

## Hardware

### Host training box

| | |
|---|---|
| CPU | Intel i7-8700K (6C/12T, 4.7 GHz boost, AVX2) |
| RAM | 64 GB |
| GPU | Intel UHD 630 (iGPU, not practically usable for PyTorch training) |

The 8700K is sufficient for **all non-training steps** (data collection, cleanup, tokenizer training, tokenization, export, quantization) with plenty of headroom. 64 GB is ample.

The bottleneck is training itself. The UHD 630 is technically supported by Intel's oneAPI PyTorch XPU backend but performance and op coverage make it not worth the setup effort — treat it as unavailable for training.

### Training on CPU — realistic time budget

Rough throughput for a 30M-parameter model on the 8700K with `intel-extension-for-pytorch` (IPEX) and `OMP_NUM_THREADS=12`:

| Tokens trained | Wall time |
|---|---|
| 100M tokens (small smoke run, ~1 epoch of a lean corpus) | ~1–2 days |
| 500M tokens (realistic domain model) | ~5–10 days |
| 1B tokens (data-rich run) | ~2–3 weeks |

These are order-of-magnitude estimates. Real throughput depends on sequence length, batch size, and whether IPEX's oneDNN kernels cover every op the model uses. **Measure with a short pilot run before committing to a full training job.**

### Cloud GPU as an escape hatch

For any iteration loop faster than "kick it off and check in a week," rent a GPU. A single RTX 4090 or A100 finishes a 500M-token training run on a 30M model in 2–6 hours at $0.50–$2/hour on Vast.ai, RunPod, or Lambda. Total cost per attempt: $5–20.

**Recommendation:** use the 8700K for everything except the training step. For training, do a small (few-hundred-K-token) pilot run locally to validate the pipeline end-to-end, then rent a GPU for the real run. Saves weeks of wall time; costs less than a takeout dinner.

## Pipeline

```
raw corpus → clean/dedupe → format as Q&A → train tokenizer
                                         ↓
                                   tokenize to .bin shards
                                         ↓
                              train model (PyTorch, CPU or GPU)
                                         ↓
                              export.py → model.bin (fp32)
                                         ↓
                          tools/quantize.py → model.q8
                                         ↓
                              make install CF=/mnt/cf MODEL=model.q8
                                         ↓
                              VELLM.EXE MODEL.Q8 -I "..."
```

## Step 1: Data collection

At 30M parameters, data quality dominates. Expect to spend 60–70% of your time here.

### Primary sources (DOS / vintage computing domain)

| Source | What it is | Why it matters |
|---|---|---|
| **Ralf Brown's Interrupt List (RBIL)** | Canonical DOS/BIOS interrupt reference. ~8 MB structured plain text. | Gold standard technical reference. Dense, accurate. |
| **USENET archives** — `comp.os.msdos.*`, `comp.sys.ibm.pc.*` | Millions of Q&A threads, 1985–2000s. Available on archive.org. | Real questions + real answers = ideal assistant training format. |
| **FreeDOS documentation** | Man pages, kernel docs, command reference. Pristine text. | Accurate command-line semantics. |
| **textfiles.com** | Massive archive of BBS-era docs, FAQs, philes, shareware READMEs. | On-aesthetic, surprisingly technical in places. |
| **Period books** (text form) | *Undocumented DOS*, *PC System Programming*, Abrash's *Zen of Assembly*, DOS programmer's refs. Archive.org has scanned text for many. | Dense, well-edited prose on the domain. |
| **Source code** | FreeDOS, MS-DOS 2.0 leak, PC-DOS utilities, shareware with source. | Teaches code patterns and idioms. |
| **Kernel / driver source comments** | Same sources, extract just doc-comments. | High signal-to-noise. |

### Realistic corpus size budget

- **Under 100 MB of cleaned text:** model overfits, repeats training data verbatim.
- **200–800 MB cleaned text:** sweet spot for a 30M model.
- **Over 1 GB:** diminishing returns at this scale.

### Cleanup steps

1. Strip USENET headers, signature blocks (`--\n` delimited), and quoted-reply lines (`> ...`). Keep the question and keep the top-voted or marked answer; drop the flamewar.
2. Dedupe aggressively — near-duplicate detection via minhash / `datasketch`. USENET has heavy cross-posting.
3. OCR artifacts from scanned books: common glitches are `1/l/I` confusion, `rn→m`, broken hyphenation at line wraps. Write a small cleaner.
4. Drop anything with a huge character-level entropy anomaly (tables, ASCII art, binary-encoded attachments) — a 30M model won't learn useful patterns from those and they'll pollute next-token prediction.
5. Normalize whitespace but preserve code-block indentation.

### Format as Q&A pairs

This is the single biggest lever for making the model feel like an assistant. Structure the training data as:

```
<|user|>
How do I load HIMEM.SYS if QEMM is already loaded?
<|assistant|>
You don't need HIMEM with QEMM — QEMM provides its own XMS manager.
Remove any HIMEM.SYS line from CONFIG.SYS, and QEMM will handle extended
memory instead. If you must run both for a specific app, load HIMEM first
with DEVICE= and then load QEMM with DEVICE= after it, but this is
unusual and the usual answer is "just use QEMM."
```

USENET `comp.os.msdos.*` threads are already in this format — a Subject line + body (question), followed by replies (answers). Mine them for pairs.

For reference material (RBIL, FreeDOS docs), synthesize Q&A pairs programmatically: for each documented command/interrupt, generate questions like "What does INT 21h function 3Dh do?" paired with the doc text as the answer. A larger LLM (via API) can do this transformation cheaply and scalably.

## Step 2: Train the tokenizer

```bash
pip install sentencepiece
```

Train a BPE tokenizer on your corpus with a vocab size of 2048–4096:

```bash
python -c "
import sentencepiece as spm
spm.SentencePieceTrainer.train(
    input='corpus.txt',
    model_prefix='vellm_tok',
    vocab_size=2048,
    model_type='bpe',
    character_coverage=1.0,
    user_defined_symbols=['<|user|>', '<|assistant|>', '<|eos|>'],
)
"
```

Domain-specific tokens matter: `HIMEM.SYS`, `CONFIG.SYS`, `INT 21H`, `AX=4C00H` will each become single tokens, making the model dramatically more efficient at its actual job. A stock 32K general-purpose tokenizer would waste ~90% of its vocab on tokens you'll never see.

Output: `vellm_tok.model` (for tokenization) and `vellm_tok.vocab` (human-readable).

Port this to llama2.c's binary tokenizer format — upstream's `tokenizer.py` has an exporter. Our Phase 1 code already loads this format.

## Step 3: Model architecture

Configure `train.py` for a size that lands ~30M params after accounting for our small vocab:

```python
# config (in train.py)
vocab_size = 2048
dim        = 384
n_layers   = 8
n_heads    = 8
max_seq_len= 512
```

Parameter count is roughly `vocab_size * dim + n_layers * (12 * dim^2)` — this config ≈ 16M params. For 30M, bump `dim` to 512 and `n_layers` to 10.

Why these knobs:

- **`vocab_size=2048`** — matches the tokenizer trained above. A smaller vocab saves parameters and is fine for a narrow domain.
- **`dim=384–512`** — sets the model's "width." Too small and it can't represent the domain; too big and you can't fit it in 48 MB at int8.
- **`n_layers=8–10`** — depth matters for multi-step reasoning. More layers cost more inference time on the Pentium.
- **`max_seq_len=512`** — DOS answers are longer than children's stories. 1024 is fine too; it just bloats the KV cache.

## Step 4: Train

`vendor/llama2.c/train.py` is llama2.c's training loop, a thin wrapper over a nanoGPT-style trainer. It handles data loading, optimizer, logging, checkpointing.

### Tokenize the corpus

```bash
python vendor/llama2.c/tinystories.py pretokenize \
    --vocab_size 2048 \
    --tokenizer_model vellm_tok.model \
    --data_dir data/corpus/
```

Produces `.bin` shards of uint16 token IDs.

### CPU training (pilot run first)

```bash
# activate IPEX optimization
pip install intel-extension-for-pytorch
export OMP_NUM_THREADS=12
export KMP_AFFINITY=granularity=fine,compact,1,0

python vendor/llama2.c/train.py \
    --device=cpu \
    --dtype=float32 \
    --compile=False \
    --batch_size=16 \
    --max_seq_len=512 \
    --vocab_size=2048 \
    --dim=384 --n_layers=8 --n_heads=8 \
    --max_iters=2000 \
    --eval_interval=500 \
    --out_dir=out/pilot
```

Do a small pilot (2000 iters, maybe 2–4 hours) to confirm the pipeline works before committing to a multi-day run. Watch validation loss — if it's not decreasing by iter 500, something is wrong with your data or tokenizer, not your hyperparameters.

### Full training (GPU rental recommended)

Same flags, `--device=cuda`, larger `--max_iters`, bigger batch size. A 4090 should run 1 epoch of 500M tokens in a couple of hours.

### Monitor

- Validation loss (held out 5%)
- Eyeball generations every 500 steps — if garbage at step 5000, your data is the problem
- Perplexity on a held-out set of Q&A pairs from the training domain

## Step 5: Export to llama2.c format

```bash
python vendor/llama2.c/export.py out/final/model.pt out/final/model.bin --version 0
```

Produces a `.bin` file in the exact format `vellm.exe` loads.

## Step 6: Quantize to .q8 for vellm

Once Phase 2 lands:

```bash
python tools/quantize.py out/final/model.bin models/domain.q8
```

## Step 7: Deploy

```bash
make install CF=/mnt/cf MODEL=models/domain.q8
```

On the Pentium:

```
C:\>VELLM.EXE DOMAIN.Q8 -I "HOW DO I LOAD HIMEM.SYS WITH QEMM?"
```

Tokens stream out. Depending on model size and Phase 3 optimization, expect fractions of a token per second to a few tokens per second on real hardware.

## Gotchas

- **Data cleanup is the whole game.** A small model is pitilessly honest about bad data. 70% of your effort belongs here; if you skimp, the model will mirror your corpus's sloppiness.
- **A 30M model will hallucinate.** It will invent plausible-sounding DOS commands and BIOS interrupts that don't exist. Mitigate with temperature=0 and a tight domain, but do not oversell its reliability.
- **Prompt format must match training exactly.** If you train with `<|user|>...<|assistant|>...`, you must prompt with that structure at inference. Document whichever format you choose in the model card.
- **Don't skip the tokenizer retrain.** Using the stock llama2.c 32K tokenizer wastes parameter budget on tokens you'll never use.
- **Don't forget `<|eos|>` handling.** Without a proper end-of-text token, the model will ramble until it hits `max_seq_len`. Train the tokenizer with it as a reserved token and make sure it appears at the end of every training Q&A pair.
- **Check for training/inference divergence.** The vellm inference code is a port of `run.c`; if you diverge from llama2.c's architecture defaults during training (RoPE base, rmsnorm eps, attention implementation), you'll get silent quality loss on deploy. Keep training architecture and vellm's inference code in lockstep.

## Variants worth considering (post-initial domain model)

- **BBS assistant** — train on BBS-era tech support transcripts. Deploy behind a terminal program on COM2 as a dial-up chatbot.
- **Interactive fiction engine** — train on IF transcripts (Zork, Infocom releases in text form). Pair with a minimal game-state skeleton for on-demand room description.
- **Code-completion TSR** — train on DOS-era source (Turbo Pascal, QuickBASIC, DOS C). Hook INT 09h in an editor for keystroke-triggered completion.
- **Period-style tech writer** — train on PC Magazine / Byte / Dr. Dobbs archives. Generate period-appropriate prose about any topic.

Each of these reuses the same training infrastructure; only the corpus differs.

## References

- Karpathy, *llama2.c*: <https://github.com/karpathy/llama2.c>
- Karpathy, *TinyStories* training write-up: repo's `README.md` and `doc/` directory
- Eldan & Li, *TinyStories: How Small Can Language Models Be and Still Speak Coherent English?* (2023)
- SentencePiece: <https://github.com/google/sentencepiece>
- Intel Extension for PyTorch: <https://github.com/intel/intel-extension-for-pytorch>
- Ralf Brown's Interrupt List: <https://www.cs.cmu.edu/~ralf/files.html>
- Archive.org USENET: search `comp.os.msdos.*` collections
- textfiles.com: <https://textfiles.com>
