# Tested hardware

The primary target is documented in `CLAUDE.md`:

- Intel Pentium Overdrive PODP5V83 (83 MHz, Socket 3, P54C, no MMX)
- 48 MB RAM
- Anigma LP4IP1 motherboard
- CF-to-IDE (2 GB MS-DOS 6.22 + WfW 3.11; 4 GB Win95 OSR2.5)
- ATI Mach64 215CT PCI (VGA text mode only)
- CWSDPMI r7

This file will grow into a full matrix of tested configurations
(including the 486DX2/66 stretch benchmark) as Phase 5 approaches. For
now the matrix captures Phase 2's model-size-vs-RAM findings.

## Model size vs. 48 MB target

Captured post-Phase-2 (arena + on-the-fly embedding dequant). All runs
under DOSBox-X `memsize = 48` with CWSDPMI r7 to match the target
PODP5V83. "Peak demand" is the virtual-arena drop from `main-entry` to
`after-build_transformer` as reported by
`_go32_dpmi_get_free_memory_information()`. "Swap peak" is the
`CWSDPMI.SWP` file size sampled mid-run. See
`docs/phase2-memory.md` for the measurement methodology.

| Model              | Disk    | Config (dim/hidden/layers/vocab/seq) | Peak demand | Swap peak | Fit at memsize=48 |
|--------------------|--------:|---------------------------------------|------------:|----------:|-------------------|
| `stories15M_q80`   | 16.31 MB | 288 / 768 / 6 / 32000 / 256           | 19.89 MB    | 0         | **fits, no swap** |
| `stories42M_q80`   | 42.27 MB | 512 / 1376 / 8 / 32000 / 1024         | 74.50 MB    | ~30 MB    | fits, paging      |

Pre-Phase-2 baseline, for reference (see `docs/phase2-memory.md`):

| Model            | Peak demand | Swap peak | Fit at memsize=48       |
|------------------|------------:|----------:|-------------------------|
| `stories15M_q80` | 55.13 MB    | ~10 MB    | fits, paging            |

### What changed

- **Arena refactor (task #3)** consolidated the 15 scattered RunState
  `calloc()` calls into one pre-allocated arena. Small direct peak-memory
  win (fragmentation avoidance), big determinism win.
- **On-the-fly row dequant (task #7)** killed the 35.16 MB fp32 token
  embedding table that `memory_map_weights()` used to allocate up front.
  Individual rows are dequantized per token on the embedding-lookup
  path. Full before/after in `docs/phase2-memory.md` §"Post-Phase-2
  measurements".

### 42M is borderline

`stories42M_q80` runs to completion on 48 MB but pages ~30 MB through
CWSDPMI's swap file. On real DOS hardware without a pre-configured swap
there is no swap file — CWSDPMI's default is `-s` (swap enabled) but
this writes to a swap file the user has to tolerate. Effective
conclusions for Phase 5:

- **15M is the comfortable target** at 48 MB. No paging. Close to
  tok/s ceiling of the hardware.
- **42M is the stretch target** at 48 MB. Requires a writable swap
  location (CF card fine, floppy impractical) and runs at roughly 1/N
  tok/s of its paging ratio. The KV cache alone (2 × 8 × 1024 × 512 ×
  4 = 32 MB) is the dominant new cost vs. 15M — see Phase 3 notes for
  `--seq-len` truncation as a possible mitigation.
- **64 MB+ hardware** runs 42M with headroom and no swap. This will
  be the reference configuration if we ever get a period-correct 64 MB
  system set up. Not the primary target.

Phase 3 candidates to push the ceiling further (see
`docs/optimization-notes.md`):

1. **Shorter effective `seq_len`** — the checkpoint advertises
   `seq_len=1024` but real usage rarely needs that. Truncating the KV
   allocation to an argv-configurable `max_seq_len` (default 256) drops
   42M peak demand by 24 MB and eliminates all paging.
2. **KV cache in int8** — would ~halve the 32 MB cost. Needs a
   correctness diff since quantization is lossy.
3. **Streaming-quantized logits** — 125 KB, much smaller lever; not a
   priority.

## Phase 1 gate — regression fingerprint

The byte-identical gate on `stories15M_q80 -t 0 -s 42 -n 200 -i "Once
upon a time"` passes under all tested memory configs:

| Config            | Pre-Phase-2 | Post-#3 arena | Post-#7 dequant |
|-------------------|:-----------:|:-------------:|:---------------:|
| memsize = 48      | PASS        | PASS          | PASS            |

All three produce the same 192-byte prefix (`3575c4cc…` md5 on the
normalized prefix). Post-#7 now matches the full 200 tokens
byte-identically to upstream `run_host`, per arena's report — the row
dequant path apparently resolved a sub-ULP drift that was flipping a
late-sequence argmax.
