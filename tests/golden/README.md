# tests/golden — Phase 1 byte-identical reference outputs

Each `*.txt` in this directory is the exact stdout captured from a native
build of upstream `vendor/llama2.c/runq.c` for a specific invocation.
`tests/run-golden.sh` builds `vellm.exe` under DJGPP, runs the same
invocation headless under DOSBox-X, captures stdout, and diffs it against
the matching golden file. An empty diff is the Phase 1 pass condition.

Do not hand-edit these files. Regenerate them with the recipes below after
intentional upstream changes (e.g. after bumping
`vendor/llama2.c/UPSTREAM_SHA`) and commit the result.

## once_upon_a_time.txt

The canonical Phase 1 correctness gate (PLAN.md § Phase 1 step 5).

```
make -f Makefile.host
./run_host models/stories15M_q80.bin \
    -z models/tokenizer.bin \
    -t 0 -s 42 -n 200 \
    -i "Once upon a time" \
    > tests/golden/once_upon_a_time.txt
```

Flag notes (upstream `runq.c` argv contract, lines 1042–1054):

- `-t 0` — greedy sampling (argmax). Deterministic.
- `-s 42` — fixed seed. Ignored under greedy but documents intent.
- `-n 200` — cap at 200 tokens.
- `-z models/tokenizer.bin` — explicit tokenizer path (upstream default is
  `./tokenizer.bin`, which would miss the `models/` subdirectory).
- `-i "Once upon a time"` — prompt.

Model inputs (both `.gitignored`, see top-level `.gitignore`):

- `models/stories15M_q80.bin` — produced from upstream
  `stories15M.pt` via `python vendor/llama2.c/export.py <out> --version 2
  --checkpoint stories15M.pt`. Source of the `.pt`:
  https://huggingface.co/karpathy/tinyllamas/resolve/main/stories15M.pt
- `models/tokenizer.bin` — 32K-vocab Llama tokenizer from
  https://github.com/karpathy/llama2.c/raw/master/tokenizer.bin

Upstream SHA pinned in `vendor/llama2.c/UPSTREAM_SHA`.
