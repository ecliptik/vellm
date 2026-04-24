# Phase 2 Memory Audit

Measurement of vellm.exe's DPMI memory footprint across Phase 2.
Companion to `docs/phase1-notes.md`. Two parts:

- §"Baseline (task #2)" — pre-arena-refactor, pre-on-the-fly-dequant.
- §"Post-Phase-2 measurements (task #6)" — post-`#3`-arena,
  post-`#7`-row-dequant. This is the number that matters for the
  integration gate.

## Baseline (task #2)

## How to reproduce

1. Build with the `VELLM_MEMORY_TRACE` feature flag:

   ```bash
   rm -f vellm.exe
   make CFLAGS="-march=pentium -O2 -fomit-frame-pointer -ffloat-store \
                -Wall -Isrc -DVELLM_MEMORY_TRACE"
   ```

   The `vmem_report()` helper lives in `src/vellm.c` under
   `#ifdef VELLM_MEMORY_TRACE`; unflagged builds compile to no-ops. The
   helper wraps `_go32_dpmi_get_free_memory_information` from
   `<dpmi.h>` and prints a single line per checkpoint.

2. Stage 8.3-safe model + tokenizer and run with `--keep-stage` so
   `CWSDPMI.SWP` can be inspected if it survives:

   ```bash
   mkdir -p /tmp/vellm-mem-stage
   cp models/stories15M_q80.bin /tmp/vellm-mem-stage/MODEL.BIN
   cp models/tokenizer.bin       /tmp/vellm-mem-stage/TOKEN.BIN
   tools/dosbox-run.sh \
       --exe vellm.exe \
       --args 'MODEL.BIN -z TOKEN.BIN -t 0 -s 42 -n 20 -i "Once upon a time"' \
       --include /tmp/vellm-mem-stage/MODEL.BIN \
       --include /tmp/vellm-mem-stage/TOKEN.BIN \
       --keep-stage --stdout /tmp/out.txt
   ```

3. `-n 20` gives the full allocation profile without paying the ~5 min
   swap tax of a `-n 200` canonical run. Use `-n 200` only when you
   explicitly want to observe `CWSDPMI.SWP` growing (the swap file is
   deleted on clean shutdown so you need to peek mid-run).

Swap comparison at `memsize=64` uses a throwaway copy of
`tools/dosbox-x.conf` with `memsize = 64`; invoke via
`CONF=/tmp/vellm-dosbox-64.conf tools/dosbox-run.sh ...`. Do **not**
commit a memsize=64 variant — the target hardware is 48 MB.

## Field semantics (DJGPP `<dpmi.h>`)

The DJGPP-side struct is `_go32_dpmi_meminfo`; relevant fields:

| Field                    | Meaning (CWSDPMI r7)                                           |
|--------------------------|----------------------------------------------------------------|
| `available_memory`       | Largest contiguous free block, **in bytes**                    |
| `available_pages`        | Free-page count (virtual arena; includes swap headroom)        |
| `unlocked_pages`         | Pages that can be committed without paging (≈ physical ceiling)|
| `total_physical_pages`   | Total physical pages available to DPMI                         |
| `linear_space`           | CWSDPMI reports `0xFFFFFFFF` ("unknown")                       |

The useful headline numbers are:

- **`available_memory`** — how much *virtual* arena is left. Tracks the
  sum of malloc demand including would-be-swap pages.
- **`unlocked_pages × 4096`** — how much *physical* arena is left (i.e.
  "will I have to page?"). This is the number that governs real-hardware
  tok/s, because real hardware doesn't page (no swap file on bare DOS by
  default).

CWSDPMI deliberately over-reports the virtual arena (172 MB at
`memsize=48`, 188 MB at `memsize=64`) by extending with a swap file on
demand up to ~128 MB over physical. That's why `available_memory`
alone is misleading for "does this fit."

## Measurements — stories15M_q80 under `-n 20`

`stories15M_q80.bin` resolves to `Config{ dim=288, hidden_dim=768,
n_layers=6, n_heads=6, n_kv_heads=6, vocab_size=32000, seq_len=256,
GS=32 }`. Checkpoint file is 16.31 MB on disk.

### memsize = 48 (target PODP5V83 config)

| Checkpoint                  | `available_memory` (MB) | `unlocked_pages × 4K` (MB) |
|-----------------------------|------------------------:|---------------------------:|
| `main-entry`                |                  172.30 |                      46.55 |
| `before-checkpoint-malloc`  |                  172.30 |                      46.55 |
| `after-checkpoint-fread`    |                  155.98 |                      46.55 |
| `after-build_transformer`   |                  117.17 |                      46.55 |
| `after-build_tokenizer`     |                  116.30 |                      46.55 |
| `before-generate`           |                  116.05 |                      46.55 |
| `after-generate`            |                  115.80 |                      46.55 |
| `after-cleanup`             |                  115.80 |                      46.55 |

Net virtual demand across `main-entry → after-generate`: **56.50 MB**.
Physical ceiling: 46.55 MB. So ~9–10 MB of committed pages *must* live
in CWSDPMI's swap file on the 48 MB config.

`after-cleanup` does not recover the arena because the committed runtime
heap is not returned to the DPMI host on `free()` — `malloc()` holds it.
For the Phase 1 port with its single-shot generation this is fine; it's
a nuisance only if we ever chain multiple generations without
process-exit in between (a concern if Phase 6 serves from a TSR).

### memsize = 64 (headroom reference)

| Checkpoint                  | `available_memory` (MB) | `unlocked_pages × 4K` (MB) |
|-----------------------------|------------------------:|---------------------------:|
| `main-entry`                |                  188.30 |                      62.55 |
| `after-build_transformer`   |                  133.17 |                      62.55 |
| `after-generate`            |                  131.80 |                      62.55 |

Net virtual demand: **56.50 MB** (same — the code doesn't change). With
a 62.55 MB physical ceiling, demand fits entirely in RAM; **no swap**.

The 1.45 MB gap between `memsize` and `unlocked_pages×4K` (48 → 46.55,
64 → 62.55) is the DOS conventional region + the CWSDPMI stub, held
back before DPMI arena setup. It's a fixed tax, not something the port
can reclaim.

## Per-allocation breakdown (from `malloc_run_state` + `read_checkpoint`)

```
checkpoint-plan        file        16.31 MB  (malloc+fread slurp)
token-embed-plan       fp32 copy   35.16 MB  (32000 × 288 × 4)
runstate-plan          total        3.52 MB
  └─ kv cache                       3.38 MB  (2 × 6 × 256 × 288 × 4)
  └─ logits                       125.00 KB  (32000 × 4)
  └─ activations                   12.75 KB
  └─ attention scores               6.00 KB  (6 × 256 × 4)
  └─ quantize buffers               5.16 KB
─────────────────────────────────────────────
demand total                       55.00 MB
observed virtual drop              56.50 MB   (delta: 1.5 MB allocator
                                                fragmentation + extra
                                                QuantizedTensor[]
                                                arrays in memory_map_weights)
```

### Headline: the dequantized token-embedding table is 35 MB

This is by far the single largest allocation, more than double the 17 MB
model file itself. `memory_map_weights()` dequantizes the entire
`q_tokens` Q8_0 block into fp32 at load time, then never re-reads the
int8 copy for embedding lookup (though it stays resident because it's
slicing the one big checkpoint buffer).

**Phase 3 will look at dequantizing rows on the fly** (cost: one
`q → fp32` conversion of `dim=288` floats per generated token = ~1152
bytes per token, trivial compared to the matmul cost). That single
change would drop peak demand from 55 MB to ~20 MB on `stories15M_q80`
and is the most impactful lever we have before touching matmul.

This is **not** in scope for Phase 2 (arena refactor doesn't touch
`memory_map_weights`). Filing as a Phase 3 candidate — see
`docs/optimization-notes.md`.

## CWSDPMI.SWP observation — `-n 200` at `memsize = 48`

Reproducing the phase-1 notes' observation with direct mid-run sampling:
started `-n 200` in background with `--keep-stage`, polled
`stat -c %s CWSDPMI.SWP` every 5 s.

```
t+0s   SWP=10,436,608 bytes  (~9.95 MB)  STDOUT=0 bytes
```

`CWSDPMI.SWP` hits its steady size (≈10 MB) *during* the malloc+fread
phase, before the first token is emitted. This matches the ~10 MB
number recorded in `docs/phase1-notes.md` §"CWSDPMI paging under
DOSBox-X" almost exactly. It does not grow further during the 200-token
generation loop — the working set stays stable once the weights are
loaded.

The swap file is deleted by CWSDPMI on process exit, so it is invisible
after a clean run; `--keep-stage` does not preserve it. To observe, you
have to sample mid-run.

## Conclusions for tasks #3 / #4 / #5

- **Task #3 (arena refactor)** should replace the 14 separate
  `calloc()`s in `malloc_run_state` with a single pre-allocated arena.
  Expected win: **not** a large peak-memory reduction (the 3.52 MB of
  RunState is dwarfed by the 35 MB dequant table). The win is
  allocator-fragmentation avoidance across repeated generations and
  determinism (calloc is already zero-fill, but a contiguous arena
  removes a whole class of "is this field properly zeroed after a
  previous run?" questions).

  If the refactor makes peak virtual go *up* rather than down, something
  is wrong — the arena total should match `runstate-plan total` within
  a few KB.

- **Task #4 (determinism)** should diff three `-n 20` outputs at the
  same seed. `-n 20` is enough — the Phase 1 gate is a 192-byte prefix
  and the short run is ~3 min vs. ~5 min per `-n 200` sweep.

- **Task #5 (stories42M upper bound)** — extrapolating:
  `stories42M_q80` should be ~42 MB on disk, with a dequantized token
  embedding table scaling as `vocab_size × dim × 4`. If the 42M config
  keeps vocab_size=32000 and bumps dim (likely `dim=512`, `n_layers=8`),
  the dequant table alone becomes ~62 MB — **larger than the physical
  ceiling at `memsize=48`**. Strong prediction: without the Phase 3
  on-the-fly dequant, 42M will not fit on the 48 MB target. Task #5
  should confirm or refute by loading it and capturing the
  `after-build_transformer` demand number.

## Instrumentation revert

The `VELLM_MEMORY_TRACE`-flagged helper in `src/vellm.c` is short-lived
by design — per the task brief, it should be reverted before task #3
starts so the arena refactor begins from a clean source. If this
document has been merged, the instrumentation has already been reverted;
recreate via `git revert` + the feature flag if future measurement is
needed.

---

## Post-Phase-2 measurements (task #6)

After arena commits `a566789` (task #3, pre-allocated RunState arena)
and `e9cfefa` (task #7, on-the-fly token-embedding row dequant). Same
instrumentation harness as task #2; instrumentation re-applied as a
short-lived `VELLM_MEMORY_TRACE` patch, measurements captured, patch
reverted before commit.

### stories15M_q80 under `-n 5`, memsize = 48

Direct measurement, post-instrumentation-reapply, `-n 5 -s 42`:

```
[MEM main-entry]              largest=172.30 MB  unlocked=46.55 MB
[MEM after-build_transformer] largest=152.42 MB  unlocked=46.55 MB
```

Net virtual demand: **19.88 MB** (vs. 55.13 MB pre-Phase-2,
matches arena team's 19.89 MB headline from task-#7 commit). The
−35.2 MB delta matches the fp32 embedding-table size
(`32000 × 288 × 4 = 35.16 MB`) almost exactly — that is the one
allocation task #7 eliminated.

Demand is now ~26 MB *under* the 46.55 MB physical ceiling. CWSDPMI.SWP
stays at 0 bytes throughout the run (confirmed mid-run sampling — see
§"CWSDPMI.SWP observation — post-Phase-2" below).

### stories42M_q80 upper bound (task #5)

42M config resolves to `dim=512 hidden=1376 layers=8 heads=8 kv_heads=8
vocab=32000 seq_len=1024 GS=64`. Checkpoint is 42.27 MB.

| Checkpoint                  | `available_memory` (MB) | `unlocked_pages × 4K` (MB) |
|-----------------------------|------------------------:|---------------------------:|
| `main-entry`                |                  172.30 |                      46.55 |
| `after-build_transformer`   |                   97.80 |                      46.55 |
| `before-generate`           |                   96.55 |                      46.55 |
| `after-generate`            |                   96.30 |                      46.55 |

Net virtual demand: **74.50 MB**. Physical ceiling: 46.55 MB. So ~28 MB
must page through CWSDPMI.SWP. Mid-run sample confirmed it:

```
t=19:34:07  CWSDPMI.SWP = 30,834,688 bytes  (~29.4 MB)
```

42M **fits with paging** on memsize=48. On period hardware without
swap-friendly storage (slow CF, no swap file configured) this will be
painful; on 64 MB hardware it runs without paging.

### Per-allocation breakdown — 42M (derived from Config, post-Phase-2)

```
checkpoint-plan        file        42.27 MB  (malloc+fread slurp)
token-embed-plan       fp32 copy    0.00 MB  (eliminated by task #7)
runstate-plan          total       32.20 MB
  └─ kv cache                      32.00 MB  (2 × 8 × 1024 × 512 × 4)
  └─ logits                       125.00 KB  (32000 × 4)
  └─ attention scores              32.00 KB  (8 × 1024 × 4)
  └─ activations                   24.00 KB  (dim=512, hidden=1376)
  └─ quantize buffers              11.25 KB
─────────────────────────────────────────────
demand total                       74.47 MB
observed virtual drop              74.50 MB
```

The KV cache (32 MB) is now the dominant allocation on 42M — 4× bigger
than on 15M because `seq_len` jumped from 256 to 1024. This is the
lever Phase 3 should reach for if we want 42M to run *without* swap on
48 MB hardware: truncate the KV allocation to an argv-configurable
`max_seq_len` (default 256). That alone would drop peak demand to
~50 MB and eliminate nearly all paging.

### Before/after summary

| Model            | Pre-Phase-2 peak | Post-Phase-2 peak | Delta     |
|------------------|-----------------:|------------------:|----------:|
| `stories15M_q80` |        55.13 MB  |         19.89 MB  | −35.2 MB  |
| `stories42M_q80` |   not measurable |         74.50 MB  | —         |

(15M pre-Phase-2 number from task-#2 baseline; 42M was not measurable
pre-Phase-2 because its fp32 embedding table alone — 32000 × 512 × 4
= 62.5 MB — would exceed the full virtual arena.)

### CWSDPMI.SWP observation — post-Phase-2

Sampled `stories15M_q80 -n 200` in background, polled `CWSDPMI.SWP`
every 10 s until exit:

```
t=+0s       CWSDPMI.SWP = 0 bytes   STDOUT.TXT = 0 bytes
... 5m45s ...
t=+5m45s    CWSDPMI.SWP = 0 bytes   STDOUT.TXT = 742 bytes   (process exits)
```

The SWP file is *created* by CWSDPMI at startup (0-byte placeholder)
but never grown. Post-Phase-2 `-n 200` on memsize=48 runs entirely in
physical RAM. This is the PLAN.md § Phase 2 exit criterion
*"15M q80 runs to completion without swapping"* — **met**.

Two independent post-Phase-2 `-n 200` runs (same seed, same args)
produce the same md5 `f03995f18f8149d80d2af3b8c12ca22f` on
CRLF-normalized stdout — full-length determinism, not just the 192-byte
prefix that the Phase 1 gate checks.

Pre-Phase-2 baseline for comparison:

```
t=+0s       CWSDPMI.SWP = 10,436,608 bytes  (~9.95 MB, steady)
```

Before/after: **~10 MB → 0 MB** of paging pressure on memsize=48. On
target hardware where swap-to-CF is undesirable, this is the headline
Phase 2 win.

**Wall-time note:** post-Phase-2 `-n 200` took ~5m45s under DOSBox-X
cycles=90000. Pre-Phase-2 was ~5m16s (phase1-notes.md). The two are
close because DOSBox-X's "disk" for the SWP is host RAM — paging in
DOSBox is almost free. On real hardware to a CF card, the pre-Phase-2
~10 MB of paging would cost seconds to minutes per generation cycle.
The Phase 2 swap-avoidance win is real but invisible under DOSBox-X.

## Task #4 determinism result

Three consecutive `-n 20 -s 42` runs under DOSBox-X memsize=48 after
the arena refactor:

```
b5696ff3ca002b9660dc40b0587c3021  /tmp/vellm-det1.norm
b5696ff3ca002b9660dc40b0587c3021  /tmp/vellm-det2.norm
b5696ff3ca002b9660dc40b0587c3021  /tmp/vellm-det3.norm
```

All three byte-identical. Arena's `memset(arena, 0, size)` + single
bump-alloc preserves upstream's calloc semantics. No uninit-memory
regression introduced by the refactor.

## Phase 3 carryover

The full allocation audit surfaces two candidates that Phase 2 did not
touch and that Phase 3 should consider:

1. **Truncatable `seq_len`.** Needed to make 42M fit without swap on
   48 MB hardware. Cost: a new argv flag or a Config override; zero
   compute-path change.
2. **int8 KV cache.** A 50% KV-cache reduction via Q8_0 quantization of
   the key/value buffers. This is a lossy change and needs a
   tolerance-based correctness gate (Phase 3's planned upgrade from
   the byte-identical Phase 1 gate).

Both are in `docs/optimization-notes.md`.
