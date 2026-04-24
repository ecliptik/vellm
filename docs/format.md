# Checkpoint format

vellm's checkpoint format is upstream llama2.c's **Q8_0** — the
"version 2" export from `runq.c`. We don't invent anything. Phase 2
considered a bespoke `.q8` format (see the stub this doc replaces)
and rejected it: `runq.c` already implements per-group symmetric
int8 quantization with fp32 scales, which is exactly what the 48 megs
Pentium target needs. See [`PLAN.md`](../PLAN.md) §"Upstream
baseline: `runq.c`, not `run.c`".

## Layout

Produced by `vendor/llama2.c/export.py --version 2`. Consumed by
`vendor/llama2.c/runq.c` (upstream) and `src/vellm.c` (our port).

- **Header (256 bytes):** magic `"ak42"`, `uint32` version, seven
  `int32` Config fields (`dim`, `hidden_dim`, `n_layers`,
  `n_heads`, `n_kv_heads`, `vocab_size`, `max_seq_len`), a
  `shared_classifier` flag byte, and a `group_size` int (64 in
  every shipped checkpoint). Remainder of the 256 bytes is
  zero-padding.
- **Tensor data:** each quantized tensor is a block of `int8`
  values followed by `float32` per-group scales (group_size = 64).
  fp32-only tensors (`rms_att_weight`, `rms_ffn_weight`,
  `rms_final_weight`) follow unquantized. Tensor order is fixed by
  `runq.c`'s `memory_map_weights()`.

The canonical layout reference is the code itself — read
`vendor/llama2.c/runq.c` at the SHA pinned in
[`vendor/llama2.c/UPSTREAM_SHA`](../vendor/llama2.c/UPSTREAM_SHA).

## Why it works for DOS

48 megs of DPMI RAM holds a `stories15M_q80.bin` (16.3 megs) or a
`stories42M_q80.bin` (42.3 megs) with headroom for activations and
KV cache. On-the-fly row dequantization in the matmul inner loop
keeps the working-set cache-friendly on the P54C's 32 KB L1. See
[`docs/phase2-memory.md`](./phase2-memory.md) for the arena +
dequant strategy, and [`docs/phase3-notes.md`](./phase3-notes.md)
for the int8 KV-cache extension that brings 42M under the 48 megs
ceiling without paging.

## How to produce

Use upstream's exporter on any llama2.c-compatible `.pt`
checkpoint:

```bash
python vendor/llama2.c/export.py models/stories15M_q80.bin \
       --version 2 --checkpoint stories15M.pt
```

This is the only supported checkpoint path for vellm —
there's no `run.c`-compatible fp32 loader.
