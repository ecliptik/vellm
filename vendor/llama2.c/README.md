# vendor/llama2.c — pinned upstream snapshot

This directory contains a **pinned snapshot** of
[karpathy/llama2.c](https://github.com/karpathy/llama2.c). The pinned commit
SHA lives in [`UPSTREAM_SHA`](./UPSTREAM_SHA) on a single line.

## What's here

| File                  | Origin (upstream path) | Purpose                                              |
|-----------------------|------------------------|------------------------------------------------------|
| `run.c`               | `run.c`                | fp32 inference reference (Phase 1 correctness gate)  |
| `runq.c`              | `runq.c`               | int8 inference reference (Phase 2 design reference)  |
| `export.py`           | `export.py`            | .pt → .bin checkpoint converter                      |
| `tokenizer.py`        | `tokenizer.py`         | SentencePiece → tokenizer.bin builder                |
| `Makefile.upstream`   | `Makefile`             | Upstream's build (kept for reference; not invoked)   |
| `LICENSE`             | `LICENSE`              | Upstream's MIT license                               |
| `README.upstream.md`  | `README.md`            | Upstream's README (kept for reference)               |
| `UPSTREAM_SHA`        | (this repo)            | Pinned commit SHA                                    |

`tokenizer.c/h` and `sampler.c/h` are **not** separate files in upstream —
they are inlined into `run.c`. The Phase 1 port lifts them out of `run.c`
into our `src/tokenizer.c` and (eventually) `src/sampler.c`.

## Why not a git submodule?

We want the freedom to patch upstream freely in our fork (`src/vellm.c`)
without a floating submodule pointer creating ambiguity about what we are
comparing against. The snapshot under this directory is treated as
read-only reference — all DOS-specific changes live under `/src/`.

## Re-syncing to a newer upstream commit

1. `git clone https://github.com/karpathy/llama2.c /tmp/llama2`
2. `cd /tmp/llama2 && git checkout <new-sha> && git rev-parse HEAD`
3. Copy `run.c runq.c export.py tokenizer.py LICENSE` into this dir.
4. Copy `Makefile` → `Makefile.upstream`, `README.md` → `README.upstream.md`.
5. Write the new SHA to `UPSTREAM_SHA`.
6. Commit. Re-run the golden test; adjust `src/vellm.c` as needed.

**Do not modify any vendored files in place.** They are reference. Our
changes live in `/src/`.

## License

The vendored files are MIT-licensed by Andrej Karpathy (see `LICENSE` in
this directory). vellm as a whole is also MIT — see the top-level `LICENSE`.

## Model data

The matching `stories260K.bin` checkpoint and `tok512.bin` tokenizer are
downloaded into `/models/` (which is `.gitignored` — they are ~1 MB + ~6 KB,
small enough to drop locally but not worth committing). Source:
<https://huggingface.co/karpathy/tinyllamas/tree/main/stories260K>.
