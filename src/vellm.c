/*
 * vellm — llama2.c runq.c ported to MS-DOS / DJGPP / Pentium Overdrive
 *
 * Copyright (c) 2023 Andrej Karpathy — original runq.c
 * Copyright (c) 2026 Micheal Waltz — DOS port
 *
 * MIT License (see LICENSE). Upstream reference at vendor/llama2.c/
 * (pinned SHA in vendor/llama2.c/UPSTREAM_SHA). All DOS-specific
 * deviations from upstream are annotated with // DOS-PORT:
 */

/* Inference for Llama-2 Transformer model in pure C, int8 quantized forward pass. */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <fcntl.h>
#if defined _WIN32
    #include "win.h"
#else
    #include <unistd.h>
    // DOS-PORT: DJGPP has no <sys/mman.h>; weights are loaded via malloc+fread
    // DOS-PORT: instead of mmap (see read_checkpoint / free_transformer below).
    #if !defined __DJGPP__
    #include <sys/mman.h>
    #endif
#endif
// DOS-PORT: <dpmi.h> for _go32_dpmi_get_free_memory_information() —
// DOS-PORT: used by --benchmark to capture peak DPMI demand (see below).
// DOS-PORT: Same field/struct used in the Phase 2 VELLM_MEMORY_TRACE
// DOS-PORT: instrumentation that produced docs/phase2-memory.md.
#if defined __DJGPP__
#include <dpmi.h>
#include <sys/movedata.h>  // DOS-PORT: dosmemget for BIOS-date peek at F000:FFF5
#endif
// ----------------------------------------------------------------------------
// Globals
int GS = 0; // group size global for quantization of the weights

// Phase 3: GS is always a power of 2 (upstream export.py always writes 32 or
// 64; format.md documents group-size-is-power-of-2 as a format invariant).
// Precompute log2(GS) once in read_checkpoint so matmul() can turn the
// per-group `(in + j) / GS` into a shift. Each such division compiled to an
// `idivl` (~46 cycles, non-pairable on P5); matmul() emits two of them per
// GS block, and a shift is 1-2 cycles. Measured ~22% matmul speedup on
// P54C (see docs/phase3-notes.md §"Task #3 Experiment B").
int GS_SHIFT = 0;

// ----------------------------------------------------------------------------
// Transformer model

typedef struct {
    int dim; // transformer dimension
    int hidden_dim; // for ffn layers
    int n_layers; // number of layers
    int n_heads; // number of query heads
    int n_kv_heads; // number of key/value heads (can be < query heads because of multiquery)
    int vocab_size; // vocabulary size, usually 256 (byte-level)
    int seq_len; // max sequence length
} Config;

typedef struct {
    int8_t* q;    // quantized values
    float* s; // scaling factors
} QuantizedTensor;

typedef struct {
    // token embedding table
    QuantizedTensor *q_tokens; // (vocab_size, dim)
    float* token_embedding_table; // same, but dequantized

    // weights for rmsnorms
    float* rms_att_weight; // (layer, dim) rmsnorm weights
    float* rms_ffn_weight; // (layer, dim)
    // weights for matmuls. note dim == n_heads * head_size
    QuantizedTensor *wq; // (layer, dim, n_heads * head_size)
    QuantizedTensor *wk; // (layer, dim, n_kv_heads * head_size)
    QuantizedTensor *wv; // (layer, dim, n_kv_heads * head_size)
    QuantizedTensor *wo; // (layer, n_heads * head_size, dim)
    // weights for ffn
    QuantizedTensor *w1; // (layer, hidden_dim, dim)
    QuantizedTensor *w2; // (layer, dim, hidden_dim)
    QuantizedTensor *w3; // (layer, hidden_dim, dim)
    // final rmsnorm
    float* rms_final_weight; // (dim,)
    // (optional) classifier weights for the logits, on the last layer
    QuantizedTensor *wcls;
} TransformerWeights;

typedef struct {
    // current wave of activations
    float *x; // activation at current time stamp (dim,)
    float *xb; // same, but inside a residual branch (dim,)
    float *xb2; // an additional buffer just for convenience (dim,)
    float *hb; // buffer for hidden dimension in the ffn (hidden_dim,)
    float *hb2; // buffer for hidden dimension in the ffn (hidden_dim,)
    QuantizedTensor xq; // quantized x (dim,)
    QuantizedTensor hq; // quantized hb (hidden_dim,)
    float *q; // query (dim,)
    float *k; // key (dim,)
    float *v; // value (dim,)
    float *att; // buffer for scores/attention values (n_heads, seq_len)
    float *logits; // output logits
    // DOS-PORT: int8-quantized KV cache with a per-head fp32 scale. Upstream
    // DOS-PORT: stores the fp32 k/v vectors verbatim — on stories42M at its
    // DOS-PORT: baked-in seq_len=1024 that's 32 MB of cache, blowing through
    // DOS-PORT: the 48 MB DPMI ceiling. Per-head scale (one float per pos
    // DOS-PORT: per layer per kv-head) gives ~3.8× reduction with minimal
    // DOS-PORT: quantization error — each attention inner loop already walks
    // DOS-PORT: one head at a time, so the scale is a single fp32 multiply
    // DOS-PORT: outside the dot product. Lossy; validated by the Phase 3
    // DOS-PORT: tolerance gate (docs/phase3-notes.md §"Task #4"). Layout:
    // DOS-PORT:   *_cache_q  (n_layers, seq_len, kv_dim)          int8
    // DOS-PORT:   *_cache_s  (n_layers, seq_len, n_kv_heads)      fp32
    int8_t* key_cache_q;   // (layer, seq_len, kv_dim) quantized keys
    float*  key_cache_s;   // (layer, seq_len, n_kv_heads) per-head scales
    int8_t* value_cache_q; // (layer, seq_len, kv_dim) quantized values
    float*  value_cache_s; // (layer, seq_len, n_kv_heads) per-head scales
} RunState;

// DOS-PORT: single pre-allocated arena replaces the 15 scattered calloc() calls
// DOS-PORT: in malloc_run_state() below — avoids DPMI heap fragmentation and
// DOS-PORT: makes peak RunState memory deterministic on the 48 MB PODP5V83.
// DOS-PORT: 16-byte-aligned bump allocator; zeroed once at init so arena_alloc
// DOS-PORT: returns zero-filled memory (preserves upstream's calloc semantics).
typedef struct {
    char *base;
    size_t size;
    size_t used;
} Arena;

static size_t arena_align16(size_t n) { return (n + 15u) & ~(size_t)15u; }

static void arena_init(Arena *a, size_t bytes) {
    a->base = (char*)malloc(bytes);
    if (!a->base) {
        fprintf(stderr, "arena alloc failed (%lu bytes)\n", (unsigned long)bytes);
        exit(EXIT_FAILURE);
    }
    memset(a->base, 0, bytes);
    a->size = bytes;
    a->used = 0;
}

static void *arena_alloc(Arena *a, size_t bytes) {
    size_t aligned = arena_align16(bytes);
    if (a->used + aligned > a->size) {
        fprintf(stderr,
                "arena exhausted: used=%lu want=%lu size=%lu\n",
                (unsigned long)a->used, (unsigned long)aligned,
                (unsigned long)a->size);
        exit(EXIT_FAILURE);
    }
    void *p = a->base + a->used;
    a->used += aligned;
    return p;
}

static void arena_free(Arena *a) {
    free(a->base);
    a->base = NULL;
    a->size = 0;
    a->used = 0;
}

typedef struct {
    Config config; // the hyperparameters of the architecture (the blueprint)
    TransformerWeights weights; // the weights of the model
    RunState state; // buffers for the "wave" of activations in the forward pass
    // some more state needed to properly clean up the memory mapping (sigh)
    int fd; // file descriptor for memory mapping
    float* data; // memory mapped data pointer
    // DOS-PORT: ssize_t is 32-bit on DJGPP (long). 15M/42M q80 checkpoints
    // DOS-PORT: fit in 32 bits with plenty of headroom; upstream assumed 64-bit host.
    ssize_t file_size; // size of the checkpoint file in bytes
    // DOS-PORT: arena backing every buffer in RunState (see malloc_run_state).
    Arena run_arena;
} Transformer;

// DOS-PORT: size the arena by summing every RunState allocation up front;
// DOS-PORT: mirror any change in malloc_run_state() below in this list too.
static size_t runstate_arena_size(Config *p) {
    int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
    size_t n = 0;
    n += arena_align16((size_t)p->dim * sizeof(float));                             // x
    n += arena_align16((size_t)p->dim * sizeof(float));                             // xb
    n += arena_align16((size_t)p->dim * sizeof(float));                             // xb2
    n += arena_align16((size_t)p->hidden_dim * sizeof(float));                      // hb
    n += arena_align16((size_t)p->hidden_dim * sizeof(float));                      // hb2
    n += arena_align16((size_t)p->dim * sizeof(int8_t));                            // xq.q
    n += arena_align16((size_t)p->dim * sizeof(float));                             // xq.s
    n += arena_align16((size_t)p->hidden_dim * sizeof(int8_t));                     // hq.q
    n += arena_align16((size_t)p->hidden_dim * sizeof(float));                      // hq.s
    n += arena_align16((size_t)p->dim * sizeof(float));                             // q
    n += arena_align16((size_t)kv_dim * sizeof(float));                             // k
    n += arena_align16((size_t)kv_dim * sizeof(float));                             // v
    n += arena_align16((size_t)p->n_heads * p->seq_len * sizeof(float));            // att
    n += arena_align16((size_t)p->vocab_size * sizeof(float));                      // logits
    // DOS-PORT: int8 KV cache (Phase 3 task #4). Quantized values plus a
    // DOS-PORT: per-head fp32 scale — see RunState definition for rationale.
    n += arena_align16((size_t)p->n_layers * p->seq_len * kv_dim * sizeof(int8_t)); // key_cache_q
    n += arena_align16((size_t)p->n_layers * p->seq_len * p->n_kv_heads * sizeof(float)); // key_cache_s
    n += arena_align16((size_t)p->n_layers * p->seq_len * kv_dim * sizeof(int8_t)); // value_cache_q
    n += arena_align16((size_t)p->n_layers * p->seq_len * p->n_kv_heads * sizeof(float)); // value_cache_s
    return n;
}

// DOS-PORT: bump-alloc every buffer out of the pre-sized arena. Signature
// DOS-PORT: gains an Arena* vs. upstream; build_transformer() owns the arena.
void malloc_run_state(RunState* s, Config* p, Arena *a) {
    int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
    s->x           = (float*) arena_alloc(a, (size_t)p->dim * sizeof(float));
    s->xb          = (float*) arena_alloc(a, (size_t)p->dim * sizeof(float));
    s->xb2         = (float*) arena_alloc(a, (size_t)p->dim * sizeof(float));
    s->hb          = (float*) arena_alloc(a, (size_t)p->hidden_dim * sizeof(float));
    s->hb2         = (float*) arena_alloc(a, (size_t)p->hidden_dim * sizeof(float));
    s->xq.q        = (int8_t*)arena_alloc(a, (size_t)p->dim * sizeof(int8_t));
    s->xq.s        = (float*) arena_alloc(a, (size_t)p->dim * sizeof(float));
    s->hq.q        = (int8_t*)arena_alloc(a, (size_t)p->hidden_dim * sizeof(int8_t));
    s->hq.s        = (float*) arena_alloc(a, (size_t)p->hidden_dim * sizeof(float));
    s->q           = (float*) arena_alloc(a, (size_t)p->dim * sizeof(float));
    s->k           = (float*) arena_alloc(a, (size_t)kv_dim * sizeof(float));
    s->v           = (float*) arena_alloc(a, (size_t)kv_dim * sizeof(float));
    s->att         = (float*) arena_alloc(a, (size_t)p->n_heads * p->seq_len * sizeof(float));
    s->logits      = (float*) arena_alloc(a, (size_t)p->vocab_size * sizeof(float));
    // DOS-PORT: int8 KV cache (task #4). See RunState + runstate_arena_size above.
    s->key_cache_q   = (int8_t*)arena_alloc(a, (size_t)p->n_layers * p->seq_len * kv_dim * sizeof(int8_t));
    s->key_cache_s   = (float*) arena_alloc(a, (size_t)p->n_layers * p->seq_len * p->n_kv_heads * sizeof(float));
    s->value_cache_q = (int8_t*)arena_alloc(a, (size_t)p->n_layers * p->seq_len * kv_dim * sizeof(int8_t));
    s->value_cache_s = (float*) arena_alloc(a, (size_t)p->n_layers * p->seq_len * p->n_kv_heads * sizeof(float));
}

// DOS-PORT: no-op — buffers live in Transformer.run_arena, freed by
// DOS-PORT: arena_free() in free_transformer. Stub kept for call-site parity.
void free_run_state(RunState* s) {
    (void)s;
}

// ----------------------------------------------------------------------------
// Quantization functions

void dequantize(QuantizedTensor *qx, float* x, int n) {
    for (int i = 0; i < n; i++) {
        x[i] = qx->q[i] * qx->s[i / GS];
    }
}

void quantize(QuantizedTensor *qx, float* x, int n) {
    int num_groups = n / GS;
    float Q_MAX = 127.0f;

    for (int group = 0; group < num_groups; group++) {

        // find the max absolute value in the current group
        float wmax = 0.0;
        for (int i = 0; i < GS; i++) {
            float val = fabs(x[group * GS + i]);
            if (val > wmax) {
                wmax = val;
            }
        }

        // calculate and write the scaling factor
        float scale = wmax / Q_MAX;
        qx->s[group] = scale;

        // calculate and write the quantized values
        for (int i = 0; i < GS; i++) {
            float quant_value = x[group * GS + i] / scale; // scale
            int8_t quantized = (int8_t) round(quant_value); // round and clamp
            qx->q[group * GS + i] = quantized;
        }
    }
}

/* initialize `n` x quantized tensor (with `size_each` elements), starting from memory pointed at *ptr */
QuantizedTensor *init_quantized_tensors(void **ptr, int n, int size_each) {
    void *p = *ptr;
    QuantizedTensor *res = malloc(n * sizeof(QuantizedTensor));
    for(int i=0; i<n; i++) {
        /* map quantized int8 values*/
        res[i].q = (int8_t*)p;
        p = (int8_t*)p + size_each;
        /* map scale factors */
        res[i].s = (float*)p;
        p = (float*)p + size_each / GS;
    }
    *ptr = p; // advance ptr to current position
    return res;
}

void memory_map_weights(TransformerWeights *w, Config* p, void* ptr, uint8_t shared_classifier) {
    int head_size = p->dim / p->n_heads;
    // first are the parameters that are kept in fp32 (the rmsnorm (1D) weights)
    float* fptr = (float*) ptr; // cast our pointer to float*
    w->rms_att_weight = fptr;
    fptr += p->n_layers * p->dim;
    w->rms_ffn_weight = fptr;
    fptr += p->n_layers * p->dim;
    w->rms_final_weight = fptr;
    fptr += p->dim;

    // now read all the quantized weights
    ptr = (void*)fptr; // now cast the pointer back to void*
    w->q_tokens = init_quantized_tensors(&ptr, 1, p->vocab_size * p->dim);
    // DOS-PORT: skip upstream's full vocab_size × dim fp32 dequant of the token
    // DOS-PORT: embedding table — that table is 35.16 MB on stories15M and is what
    // DOS-PORT: drives CWSDPMI.SWP growth on the 48 MB target (see docs/phase2-memory.md).
    // DOS-PORT: Instead we dequantize one row on demand at lookup in forward(),
    // DOS-PORT: keeping only the Q8_0 form (~8.25 MB) resident. Numerically
    // DOS-PORT: identical: same qx->q[i] * qx->s[i/GS] formula, just lazy.
    w->token_embedding_table = NULL;

    w->wq = init_quantized_tensors(&ptr, p->n_layers, p->dim * (p->n_heads * head_size));
    w->wk = init_quantized_tensors(&ptr, p->n_layers, p->dim * (p->n_kv_heads * head_size));
    w->wv = init_quantized_tensors(&ptr, p->n_layers, p->dim * (p->n_kv_heads * head_size));
    w->wo = init_quantized_tensors(&ptr, p->n_layers, (p->n_heads * head_size) * p->dim);

    w->w1 = init_quantized_tensors(&ptr, p->n_layers, p->dim * p->hidden_dim);
    w->w2 = init_quantized_tensors(&ptr, p->n_layers, p->hidden_dim * p->dim);
    w->w3 = init_quantized_tensors(&ptr, p->n_layers, p->dim * p->hidden_dim);

    w->wcls = shared_classifier ? w->q_tokens : init_quantized_tensors(&ptr, 1, p->dim * p->vocab_size);
}

void read_checkpoint(char* checkpoint, Config* config, TransformerWeights* weights,
                     int* fd, float** data, ssize_t* file_size) {
    // DOS-PORT: DJGPP default is text mode; "rb" is mandatory — CRLF translation corrupts binary checkpoints.
    FILE *file = fopen(checkpoint, "rb");
    if (!file) { fprintf(stderr, "Couldn't open file %s\n", checkpoint); exit(EXIT_FAILURE); }
    // read in magic number (uint32), has to be 0x616b3432, i.e. "ak42" in ASCII
    uint32_t magic_number;
    if (fread(&magic_number, sizeof(uint32_t), 1, file) != 1) { exit(EXIT_FAILURE); }
    if (magic_number != 0x616b3432) { fprintf(stderr, "Bad magic number\n"); exit(EXIT_FAILURE); }
    // read in the version number (uint32), has to be 2
    int version;
    if (fread(&version, sizeof(int), 1, file) != 1) { exit(EXIT_FAILURE); }
    if (version != 2) { fprintf(stderr, "Bad version %d, need version 2\n", version); exit(EXIT_FAILURE); }
    int header_size = 256; // the header size for version 2 in bytes
    // read in the Config
    if (fread(config, sizeof(Config), 1, file) != 1) { exit(EXIT_FAILURE); }
    // read in flags
    uint8_t shared_classifier; // a byte to indicate if the classifier is shared
    if (fread(&shared_classifier, sizeof(uint8_t), 1, file) != 1) { exit(EXIT_FAILURE); }
    int group_size; // the group size used in quantization
    if (fread(&group_size, sizeof(int), 1, file) != 1) { exit(EXIT_FAILURE); }
    GS = group_size; // set as global, as it will be used in many places
    // Phase 3: compute log2(GS) for matmul() IDIV→shift (see globals block).
    // GS must be a power of 2; assert and derive the shift count once here.
    if (GS <= 0 || (GS & (GS - 1)) != 0) {
        fprintf(stderr, "bad group_size %d (must be a power of 2)\n", GS);
        exit(EXIT_FAILURE);
    }
    GS_SHIFT = 0;
    { int g = GS; while (g > 1) { g >>= 1; GS_SHIFT++; } }
    // figure out the file size
    fseek(file, 0, SEEK_END); // move file pointer to end of file
    // DOS-PORT: ftell returns long (32-bit on DJGPP); 15M/42M checkpoints fit comfortably.
    *file_size = ftell(file); // get the file size, in bytes
    // DOS-PORT: no mmap on DOS — slurp the whole checkpoint into a malloc'd arena via fread.
    // DOS-PORT: 48 MB DPMI is plenty for 15M/42M q80. File stays resident until free_transformer.
    rewind(file);
    *data = malloc(*file_size);
    if (*data == NULL) {
        fprintf(stderr, "malloc failed for checkpoint (%ld bytes)\n", (long)*file_size);
        exit(EXIT_FAILURE);
    }
    if (fread(*data, 1, (size_t)*file_size, file) != (size_t)*file_size) {
        fprintf(stderr, "short read on checkpoint\n");
        exit(EXIT_FAILURE);
    }
    fclose(file);
    // DOS-PORT: no mmap fd on DOS; field retained for struct parity with upstream.
    *fd = -1;
    void* weights_ptr = ((char*)*data) + header_size; // skip header bytes. char is 1 byte
    memory_map_weights(weights, config, weights_ptr, shared_classifier);
}

void build_transformer(Transformer *t, char* checkpoint_path, int max_seq_len) {
    // read in the Config and the Weights from the checkpoint
    read_checkpoint(checkpoint_path, &t->config, &t->weights, &t->fd, &t->data, &t->file_size);
    // DOS-PORT: optional --max-seq-len cap on KV cache allocation; checkpoint
    // DOS-PORT: seq_len is an upper bound, not a hard requirement. On stories42M
    // DOS-PORT: (seq_len=1024) the KV cache dominates peak memory — capping at
    // DOS-PORT: 256 reclaims ~24 MB so 42M fits in 48 MB DPMI without swap.
    // DOS-PORT: Cap happens BEFORE runstate_arena_size so the arena is sized to
    // DOS-PORT: the reduced seq_len. forward()'s use of p->seq_len as the kv
    // DOS-PORT: stride then Just Works — no forward-path changes needed.
    if (max_seq_len > 0 && max_seq_len < t->config.seq_len) {
        t->config.seq_len = max_seq_len;
    }
    // DOS-PORT: one malloc for the whole RunState, sized from Config up front.
    arena_init(&t->run_arena, runstate_arena_size(&t->config));
    // allocate the RunState buffers
    malloc_run_state(&t->state, &t->config, &t->run_arena);
}

void free_transformer(Transformer* t) {
    // free QuantizedTensors
    free(t->weights.q_tokens);
    free(t->weights.token_embedding_table);
    free(t->weights.wq);
    free(t->weights.wk);
    free(t->weights.wv);
    free(t->weights.wo);
    free(t->weights.w1);
    free(t->weights.w2);
    free(t->weights.w3);
    if(t->weights.wcls != t->weights.q_tokens) { free(t->weights.wcls); }
    // DOS-PORT: weights loaded via malloc+fread; release the arena, no munmap/close.
    if (t->data != NULL) { free(t->data); t->data = NULL; }
    // free the RunState buffers
    free_run_state(&t->state);
    // DOS-PORT: release the single arena that backed every RunState buffer.
    arena_free(&t->run_arena);
}

// ----------------------------------------------------------------------------
// neural net blocks; the dynamics of the Transformer

void rmsnorm(float* o, float* x, float* weight, int size) {
    // calculate sum of squares
    float ss = 0.0f;
    for (int j = 0; j < size; j++) {
        ss += x[j] * x[j];
    }
    ss /= size;
    ss += 1e-5f;
    ss = 1.0f / sqrtf(ss);
    // normalize and scale
    for (int j = 0; j < size; j++) {
        o[j] = weight[j] * (ss * x[j]);
    }
}

void softmax(float* x, int size) {
    // find max value (for numerical stability)
    float max_val = x[0];
    for (int i = 1; i < size; i++) {
        if (x[i] > max_val) {
            max_val = x[i];
        }
    }
    // exp and sum
    float sum = 0.0f;
    for (int i = 0; i < size; i++) {
        x[i] = expf(x[i] - max_val);
        sum += x[i];
    }
    // normalize
    for (int i = 0; i < size; i++) {
        x[i] /= sum;
    }
}

void matmul(float* xout, QuantizedTensor *x, QuantizedTensor *w, int n, int d) {
    // W (d,n) @ x (n,) -> xout (d,)
    // by far the most amount of time is spent inside this little function
    // inputs to this function are both quantized

    // Phase 3 Experiment B: the per-group scale index `(in + j) / GS` and
    // `j / GS` compile to `idivl %esi` on P5 (~46 cycles, non-pairable)
    // because GS is a global variable the compiler can't see as a constant.
    // We know GS is a power of 2 (checked in read_checkpoint), and the
    // addends are both multiples of GS (since n must be too, or upstream's
    // own runq.c would already be mis-indexing). So:
    //   (in + j) / GS  ==  (in >> GS_SHIFT) + (j >> GS_SHIFT)
    // Precompute `in_g = in >> GS_SHIFT` once per i, `j_g` once per j-block,
    // replacing the two IDIVs with one add. Measured ~22% matmul speedup
    // on P54C, orthogonal to -funroll-loops. No accuracy cost — the
    // division was exact in the original formulation.
    int shift = GS_SHIFT;
    int i;
    #pragma omp parallel for private(i)
    for (i = 0; i < d; i++) {

        float val = 0.0f;
        int32_t ival = 0;
        int in = i * n;
        int in_g = in >> shift;          // == (i * n) / GS, exact

        // do the matmul in groups of GS
        int j;
        for (j = 0; j <= n - GS; j += GS) {
            int j_g = j >> shift;        // == j / GS, exact (j is a multiple of GS)
            float wx_s = w->s[in_g + j_g] * x->s[j_g];  // per-group scale product
            for (int k = 0; k < GS; k++) {
                ival += ((int32_t) x->q[j + k]) * ((int32_t) w->q[in + j + k]);
            }
            val += ((float) ival) * wx_s;
            ival = 0;
        }

        xout[i] = val;
    }
}

float* forward(Transformer* transformer, int token, int pos) {

    // a few convenience variables
    Config* p = &transformer->config;
    TransformerWeights* w = &transformer->weights;
    RunState* s = &transformer->state;
    float *x = s->x;
    int dim = p->dim;
    int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
    int kv_mul = p->n_heads / p->n_kv_heads; // integer multiplier of the kv sharing in multiquery
    int hidden_dim =  p->hidden_dim;
    int head_size = dim / p->n_heads;

    // DOS-PORT: on-the-fly row dequant — upstream memcpy's from a pre-dequantized
    // DOS-PORT: fp32 table (35 MB on stories15M). See memory_map_weights(). The
    // DOS-PORT: arithmetic below matches upstream's dequantize() byte-for-byte
    // DOS-PORT: (same q[i] * s[i/GS] form), just computed per-row instead of once
    // DOS-PORT: at load. Cost: ~dim multiplies per token, negligible vs. matmul.
    {
        int row = token * dim;
        for (int j = 0; j < dim; j++) {
            x[j] = (float)w->q_tokens->q[row + j] * w->q_tokens->s[(row + j) / GS];
        }
    }

    // forward all the layers
    for(int l = 0; l < p->n_layers; l++) {

        // attention rmsnorm
        rmsnorm(s->xb, x, w->rms_att_weight + l*dim, dim);

        // qkv matmuls for this position
        quantize(&s->xq, s->xb, dim);
        matmul(s->q, &s->xq, w->wq + l, dim, dim);
        matmul(s->k, &s->xq, w->wk + l, dim, kv_dim);
        matmul(s->v, &s->xq, w->wv + l, dim, kv_dim);

        // RoPE relative positional encoding: complex-valued rotate q and k in each head
        for (int i = 0; i < dim; i+=2) {
            int head_dim = i % head_size;
            float freq = 1.0f / powf(10000.0f, head_dim / (float)head_size);
            float val = pos * freq;
            float fcr = cosf(val);
            float fci = sinf(val);
            int rotn = i < kv_dim ? 2 : 1; // how many vectors? 2 = q & k, 1 = q only
            for (int v = 0; v < rotn; v++) {
                float* vec = v == 0 ? s->q : s->k; // the vector to rotate (query or key)
                float v0 = vec[i];
                float v1 = vec[i+1];
                vec[i]   = v0 * fcr - v1 * fci;
                vec[i+1] = v0 * fci + v1 * fcr;
            }
        }

        // DOS-PORT: save k, v at this timestep into the int8 KV cache (task #4).
        // DOS-PORT: Per-head scale: one fp32 per (layer, pos, kv_head). The
        // DOS-PORT: attention inner loops below already walk one head at a time,
        // DOS-PORT: so the scale is a single multiply outside each dot product.
        int loff   = l * p->seq_len * kv_dim;           // byte offset into *_cache_q
        int loff_s = l * p->seq_len * p->n_kv_heads;    // scale offset into *_cache_s
        {
            int8_t *kq_row = s->key_cache_q   + loff   + pos * kv_dim;
            int8_t *vq_row = s->value_cache_q + loff   + pos * kv_dim;
            float  *ks_row = s->key_cache_s   + loff_s + pos * p->n_kv_heads;
            float  *vs_row = s->value_cache_s + loff_s + pos * p->n_kv_heads;
            for (int kh = 0; kh < p->n_kv_heads; kh++) {
                int base = kh * head_size;
                // scan this head's sub-vector for max |.| → scale
                float kmax = 0.0f, vmax = 0.0f;
                for (int i = 0; i < head_size; i++) {
                    float ak = fabsf(s->k[base + i]);
                    float av = fabsf(s->v[base + i]);
                    if (ak > kmax) kmax = ak;
                    if (av > vmax) vmax = av;
                }
                float ks = kmax / 127.0f;
                float vs = vmax / 127.0f;
                ks_row[kh] = ks;
                vs_row[kh] = vs;
                // quantize. Guard against a zero scale (all-zero sub-vector).
                float kinv = (ks > 0.0f) ? (1.0f / ks) : 0.0f;
                float vinv = (vs > 0.0f) ? (1.0f / vs) : 0.0f;
                for (int i = 0; i < head_size; i++) {
                    float kv_k = s->k[base + i] * kinv;
                    float kv_v = s->v[base + i] * vinv;
                    // clamp + round (matches upstream quantize() behavior)
                    int qk = (int)(kv_k < 0.0f ? kv_k - 0.5f : kv_k + 0.5f);
                    int qv = (int)(kv_v < 0.0f ? kv_v - 0.5f : kv_v + 0.5f);
                    if (qk >  127) qk =  127;
                    if (qk < -128) qk = -128;
                    if (qv >  127) qv =  127;
                    if (qv < -128) qv = -128;
                    kq_row[base + i] = (int8_t)qk;
                    vq_row[base + i] = (int8_t)qv;
                }
            }
        }

        // multihead attention. iterate over all heads
        int h;
        #pragma omp parallel for private(h)
        for (h = 0; h < p->n_heads; h++) {
            int kv_h = h / kv_mul;                    // which kv-head feeds this query head
            // get the query vector for this head
            float* q = s->q + h * head_size;
            // attention scores for this head
            float* att = s->att + h * p->seq_len;
            // iterate over all timesteps, including the current one
            for (int t = 0; t <= pos; t++) {
                // DOS-PORT: dequant key for this head/timestep on the fly.
                // DOS-PORT: int8 dot + single scale multiply outside the inner
                // DOS-PORT: loop replaces a memcpy + fp32 dot in upstream.
                const int8_t *kq = s->key_cache_q + loff + t * kv_dim + kv_h * head_size;
                float ks = s->key_cache_s[loff_s + t * p->n_kv_heads + kv_h];
                float dot = 0.0f;
                for (int i = 0; i < head_size; i++) {
                    dot += q[i] * (float)kq[i];
                }
                att[t] = (dot * ks) / sqrtf(head_size);
            }

            // softmax the scores to get attention weights, from 0..pos inclusively
            softmax(att, pos + 1);

            // weighted sum of the values, store back into xb
            float* xb = s->xb + h * head_size;
            memset(xb, 0, head_size * sizeof(float));
            for (int t = 0; t <= pos; t++) {
                // DOS-PORT: dequant value for this head/timestep; fold the
                // DOS-PORT: per-head scale into the attention weight so the
                // DOS-PORT: inner loop is still a single fmadd chain.
                const int8_t *vq = s->value_cache_q + loff + t * kv_dim + kv_h * head_size;
                float vs = s->value_cache_s[loff_s + t * p->n_kv_heads + kv_h];
                float a = att[t] * vs;
                for (int i = 0; i < head_size; i++) {
                    xb[i] += a * (float)vq[i];
                }
            }
        }

        // final matmul to get the output of the attention
        quantize(&s->xq, s->xb, dim);
        matmul(s->xb2, &s->xq, w->wo + l, dim, dim);

        // residual connection back into x
        for (int i = 0; i < dim; i++) {
            x[i] += s->xb2[i];
        }

        // ffn rmsnorm
        rmsnorm(s->xb, x, w->rms_ffn_weight + l*dim, dim);

        // Now for FFN in PyTorch we have: self.w2(F.silu(self.w1(x)) * self.w3(x))
        // first calculate self.w1(x) and self.w3(x)
        quantize(&s->xq, s->xb, dim);
        matmul(s->hb, &s->xq, w->w1 + l, dim, hidden_dim);
        matmul(s->hb2, &s->xq, w->w3 + l, dim, hidden_dim);

        // SwiGLU non-linearity
        for (int i = 0; i < hidden_dim; i++) {
            float val = s->hb[i];
            // silu(x)=x*σ(x), where σ(x) is the logistic sigmoid
            val *= (1.0f / (1.0f + expf(-val)));
            // elementwise multiply with w3(x)
            val *= s->hb2[i];
            s->hb[i] = val;
        }

        // final matmul to get the output of the ffn
        quantize(&s->hq, s->hb, hidden_dim);
        matmul(s->xb, &s->hq, w->w2 + l, hidden_dim, dim);

        // residual connection
        for (int i = 0; i < dim; i++) {
            x[i] += s->xb[i];
        }
    }

    // final rmsnorm
    rmsnorm(x, x, w->rms_final_weight, dim);

    // classifier into logits
    quantize(&s->xq, x, dim);
    matmul(s->logits, &s->xq, w->wcls, dim, p->vocab_size);
    return s->logits;
}

// ----------------------------------------------------------------------------
// The Byte Pair Encoding (BPE) Tokenizer that translates strings <-> tokens

typedef struct {
    char *str;
    int id;
} TokenIndex;

typedef struct {
    char** vocab;
    float* vocab_scores;
    TokenIndex *sorted_vocab;
    int vocab_size;
    unsigned int max_token_length;
    unsigned char byte_pieces[512]; // stores all single-byte strings
} Tokenizer;

int compare_tokens(const void *a, const void *b) {
    return strcmp(((TokenIndex*)a)->str, ((TokenIndex*)b)->str);
}

void build_tokenizer(Tokenizer* t, char* tokenizer_path, int vocab_size) {
    // i should have written the vocab_size into the tokenizer file... sigh
    t->vocab_size = vocab_size;
    // malloc space to hold the scores and the strings
    t->vocab = (char**)malloc(vocab_size * sizeof(char*));
    t->vocab_scores = (float*)malloc(vocab_size * sizeof(float));
    t->sorted_vocab = NULL; // initialized lazily
    for (int i = 0; i < 256; i++) {
        t->byte_pieces[i * 2] = (unsigned char)i;
        t->byte_pieces[i * 2 + 1] = '\0';
    }
    // read in the file
    // DOS-PORT: "rb" is mandatory on DJGPP; text-mode default would corrupt the binary vocab blob.
    FILE *file = fopen(tokenizer_path, "rb");
    if (!file) { fprintf(stderr, "couldn't load %s\n", tokenizer_path); exit(EXIT_FAILURE); }
    if (fread(&t->max_token_length, sizeof(int), 1, file) != 1) { fprintf(stderr, "failed read\n"); exit(EXIT_FAILURE); }
    int len;
    for (int i = 0; i < vocab_size; i++) {
        if (fread(t->vocab_scores + i, sizeof(float), 1, file) != 1) { fprintf(stderr, "failed read\n"); exit(EXIT_FAILURE);}
        if (fread(&len, sizeof(int), 1, file) != 1) { fprintf(stderr, "failed read\n"); exit(EXIT_FAILURE); }
        t->vocab[i] = (char *)malloc(len + 1);
        if (fread(t->vocab[i], len, 1, file) != 1) { fprintf(stderr, "failed read\n"); exit(EXIT_FAILURE); }
        t->vocab[i][len] = '\0'; // add the string terminating token
    }
    fclose(file);
}

void free_tokenizer(Tokenizer* t) {
    for (int i = 0; i < t->vocab_size; i++) { free(t->vocab[i]); }
    free(t->vocab);
    free(t->vocab_scores);
    free(t->sorted_vocab);
}

char* decode(Tokenizer* t, int prev_token, int token) {
    char *piece = t->vocab[token];
    // following BOS (1) token, sentencepiece decoder strips any leading whitespace (see PR #89)
    if (prev_token == 1 && piece[0] == ' ') { piece++; }
    // careful, some tokens designate raw bytes, and look like e.g. '<0x01>'
    // parse this and convert and return the actual byte
    unsigned char byte_val;
    if (sscanf(piece, "<0x%02hhX>", &byte_val) == 1) {
        piece = (char*)t->byte_pieces + byte_val * 2;
    }
    return piece;
}

void safe_printf(char *piece) {
    // piece might be a raw byte token, and we only want to print printable chars or whitespace
    // because some of the other bytes can be various control codes, backspace, etc.
    if (piece == NULL) { return; }
    if (piece[0] == '\0') { return; }
    if (piece[1] == '\0') {
        unsigned char byte_val = piece[0];
        if (!(isprint(byte_val) || isspace(byte_val))) {
            return; // bad byte, don't print it
        }
    }
    printf("%s", piece);
}

int str_lookup(char *str, TokenIndex *sorted_vocab, int vocab_size) {
    // efficiently find the perfect match for str in vocab, return its index or -1 if not found
    TokenIndex tok = { .str = str }; // acts as the key to search for
    TokenIndex *res = bsearch(&tok, sorted_vocab, vocab_size, sizeof(TokenIndex), compare_tokens);
    return res != NULL ? res->id : -1;
}

void encode(Tokenizer* t, char *text, int8_t bos, int8_t eos, int *tokens, int *n_tokens) {
    // encode the string text (input) into an upper-bound preallocated tokens[] array
    // bos != 0 means prepend the BOS token (=1), eos != 0 means append the EOS token (=2)
    if (text == NULL) { fprintf(stderr, "cannot encode NULL text\n"); exit(EXIT_FAILURE); }

    if (t->sorted_vocab == NULL) {
        // lazily malloc and sort the vocabulary
        t->sorted_vocab = malloc(t->vocab_size * sizeof(TokenIndex));
        for (int i = 0; i < t->vocab_size; i++) {
            t->sorted_vocab[i].str = t->vocab[i];
            t->sorted_vocab[i].id = i;
        }
        qsort(t->sorted_vocab, t->vocab_size, sizeof(TokenIndex), compare_tokens);
    }

    // create a temporary buffer that will store merge candidates of always two consecutive tokens
    // *2 for concat, +1 for null terminator +2 for UTF8 (in case max_token_length is 1)
    char* str_buffer = malloc((t->max_token_length*2 +1 +2) * sizeof(char));
    size_t str_len = 0;

    // start at 0 tokens
    *n_tokens = 0;

    // add optional BOS (=1) token, if desired
    if (bos) tokens[(*n_tokens)++] = 1;

    // add_dummy_prefix is true by default
    // so prepend a dummy prefix token to the input string, but only if text != ""
    // TODO: pretty sure this isn't correct in the general case but I don't have the
    // energy to read more of the sentencepiece code to figure out what it's doing
    if (text[0] != '\0') {
        int dummy_prefix = str_lookup(" ", t->sorted_vocab, t->vocab_size);
        tokens[(*n_tokens)++] = dummy_prefix;
    }

    // Okay UTF-8 time. This will get messy. Here is the reference from Wikipedia:
    // Code point ↔ UTF-8 conversion
    // First code point	Last code point	Byte 1	Byte 2	Byte 3	Byte 4
    // U+0000	U+007F	    0xxxxxxx
    // U+0080	U+07FF	    110xxxxx	10xxxxxx
    // U+0800	U+FFFF	    1110xxxx	10xxxxxx	10xxxxxx
    // U+10000	U+10FFFF    11110xxx	10xxxxxx	10xxxxxx	10xxxxxx

    // process the raw (UTF-8) byte sequence of the input string
    for (char *c = text; *c != '\0'; c++) {

        // reset buffer if the current byte is ASCII or a leading byte
        // 0xC0 is 11000000, so (*c & 0xC0) keeps the first 2 bits and zeros the rest
        // 0x80 is 10000000
        // in UTF-8, all continuation bytes start with "10" in first two bits
        // so in English this is: "if this byte is not a continuation byte"
        if ((*c & 0xC0) != 0x80) {
            // this byte must be either a leading byte (11...) or an ASCII char (0x...)
            // => reset our location, as we're starting a new UTF-8 codepoint
            str_len = 0;
        }

        // append the current byte to the buffer
        str_buffer[str_len++] = *c; // ++ is post-increment, incremented after this line
        str_buffer[str_len] = '\0';

        // while the next character is a continuation byte, continue appending
        // but if there are too many of them, just stop to avoid overruning str_buffer size.
        if ((*(c+1) & 0xC0) == 0x80 && str_len < 4) {
            continue;
        }

        // ok c+1 is not a continuation byte, so we've read in a full codepoint
        int id = str_lookup(str_buffer, t->sorted_vocab, t->vocab_size);

        if (id != -1) {
            // we found this codepoint in vocab, add it as a token
            tokens[(*n_tokens)++] = id;
        } else {
            // byte_fallback encoding: just encode each byte as a token
            // +3 is here because the first 3 vocab elements are <unk>, <s>, </s>
            // so the individual bytes only start at index 3
            for (int i=0; i < str_len; i++) {
                tokens[(*n_tokens)++] = (unsigned char)str_buffer[i] + 3;
            }
        }
        str_len = 0; // protect against a sequence of stray UTF8 continuation bytes
    }

    // merge the best consecutive pair each iteration, according the scores in vocab_scores
    while (1) {
        float best_score = -1e10;
        int best_id = -1;
        int best_idx = -1;

        for (int i=0; i < (*n_tokens-1); i++) {
            // check if we can merge the pair (tokens[i], tokens[i+1])
            sprintf(str_buffer, "%s%s", t->vocab[tokens[i]], t->vocab[tokens[i+1]]);
            int id = str_lookup(str_buffer, t->sorted_vocab, t->vocab_size);
            if (id != -1 && t->vocab_scores[id] > best_score) {
                // this merge pair exists in vocab! record its score and position
                best_score = t->vocab_scores[id];
                best_id = id;
                best_idx = i;
            }
        }

        if (best_idx == -1) {
            break; // we couldn't find any more pairs to merge, so we're done
        }

        // merge the consecutive pair (best_idx, best_idx+1) into new token best_id
        tokens[best_idx] = best_id;
        // delete token at position best_idx+1, shift the entire sequence back 1
        for (int i = best_idx+1; i < (*n_tokens-1); i++) {
            tokens[i] = tokens[i+1];
        }
        (*n_tokens)--; // token length decreased
    }

    // add optional EOS (=2) token, if desired
    if (eos) tokens[(*n_tokens)++] = 2;

    free(str_buffer);
}

// ----------------------------------------------------------------------------
// The Sampler, which takes logits and returns a sampled token
// sampling can be done in a few ways: greedy argmax, sampling, top-p sampling

typedef struct {
    float prob;
    int index;
} ProbIndex; // struct used when sorting probabilities during top-p sampling

typedef struct {
    int vocab_size;
    ProbIndex* probindex; // buffer used in top-p sampling
    float temperature;
    float topp;
    unsigned long long rng_state;
} Sampler;

int sample_argmax(float* probabilities, int n) {
    // return the index that has the highest probability
    int max_i = 0;
    float max_p = probabilities[0];
    for (int i = 1; i < n; i++) {
        if (probabilities[i] > max_p) {
            max_i = i;
            max_p = probabilities[i];
        }
    }
    return max_i;
}

int sample_mult(float* probabilities, int n, float coin) {
    // sample index from probabilities (they must sum to 1!)
    // coin is a random number in [0, 1), usually from random_f32()
    float cdf = 0.0f;
    for (int i = 0; i < n; i++) {
        cdf += probabilities[i];
        if (coin < cdf) {
            return i;
        }
    }
    return n - 1; // in case of rounding errors
}

int compare(const void* a, const void* b) {
    ProbIndex* a_ = (ProbIndex*) a;
    ProbIndex* b_ = (ProbIndex*) b;
    if (a_->prob > b_->prob) return -1;
    if (a_->prob < b_->prob) return 1;
    return 0;
}

int sample_topp(float* probabilities, int n, float topp, ProbIndex* probindex, float coin) {
    // top-p sampling (or "nucleus sampling") samples from the smallest set of
    // tokens that exceed probability topp. This way we never sample tokens that
    // have very low probabilities and are less likely to go "off the rails".
    // coin is a random number in [0, 1), usually from random_f32()

    int n0 = 0;
    // quicksort indices in descending order of probabilities
    // values smaller than (1 - topp) / (n - 1) cannot be part of the result
    // so for efficiency we crop these out as candidates before sorting
    const float cutoff = (1.0f - topp) / (n - 1);
    for (int i = 0; i < n; i++) {
        if (probabilities[i] >= cutoff) {
            probindex[n0].index = i;
            probindex[n0].prob = probabilities[i];
            n0++;
        }
    }
    qsort(probindex, n0, sizeof(ProbIndex), compare);

    // truncate the list where cumulative probability exceeds topp
    float cumulative_prob = 0.0f;
    int last_idx = n0 - 1; // in case of rounding errors consider all elements
    for (int i = 0; i < n0; i++) {
        cumulative_prob += probindex[i].prob;
        if (cumulative_prob > topp) {
            last_idx = i;
            break; // we've exceeded topp by including last_idx
        }
    }

    // sample from the truncated list
    float r = coin * cumulative_prob;
    float cdf = 0.0f;
    for (int i = 0; i <= last_idx; i++) {
        cdf += probindex[i].prob;
        if (r < cdf) {
            return probindex[i].index;
        }
    }
    return probindex[last_idx].index; // in case of rounding errors
}

void build_sampler(Sampler* sampler, int vocab_size, float temperature, float topp, unsigned long long rng_seed) {
    sampler->vocab_size = vocab_size;
    sampler->temperature = temperature;
    sampler->topp = topp;
    sampler->rng_state = rng_seed;
    // buffer only used with nucleus sampling; may not need but it's ~small
    sampler->probindex = malloc(sampler->vocab_size * sizeof(ProbIndex));
}

void free_sampler(Sampler* sampler) {
    free(sampler->probindex);
}

unsigned int random_u32(unsigned long long *state) {
    // xorshift rng: https://en.wikipedia.org/wiki/Xorshift#xorshift.2A
    *state ^= *state >> 12;
    *state ^= *state << 25;
    *state ^= *state >> 27;
    return (*state * 0x2545F4914F6CDD1Dull) >> 32;
}
float random_f32(unsigned long long *state) { // random float32 in [0,1)
    return (random_u32(state) >> 8) / 16777216.0f;
}

int sample(Sampler* sampler, float* logits) {
    // sample the token given the logits and some hyperparameters
    int next;
    if (sampler->temperature == 0.0f) {
        // greedy argmax sampling: take the token with the highest probability
        next = sample_argmax(logits, sampler->vocab_size);
    } else {
        // apply the temperature to the logits
        for (int q=0; q<sampler->vocab_size; q++) { logits[q] /= sampler->temperature; }
        // apply softmax to the logits to get the probabilities for next token
        softmax(logits, sampler->vocab_size);
        // flip a (float) coin (this is our source of entropy for sampling)
        float coin = random_f32(&sampler->rng_state);
        // we sample from this distribution to get the next token
        if (sampler->topp <= 0 || sampler->topp >= 1) {
            // simply sample from the predicted probability distribution
            next = sample_mult(logits, sampler->vocab_size, coin);
        } else {
            // top-p (nucleus) sampling, clamping the least likely tokens to zero
            next = sample_topp(logits, sampler->vocab_size, sampler->topp, sampler->probindex, coin);
        }
    }
    return next;
}

// ----------------------------------------------------------------------------
// utilities: time

long time_in_ms() {
    // return time in milliseconds, for benchmarking the model speed
#ifdef __DJGPP__
    // DOS-PORT: DJGPP's <time.h> has no clock_gettime / CLOCK_REALTIME; fall back
    // DOS-PORT: to clock(). PIT granularity is ~55ms (18.2 Hz) — fine for tok/s reporting.
    return (long)((long long)clock() * 1000 / CLOCKS_PER_SEC);
#else
    struct timespec time;
    clock_gettime(CLOCK_REALTIME, &time);
    return time.tv_sec * 1000 + time.tv_nsec / 1000000;
#endif
}

// ----------------------------------------------------------------------------
// generation loop

void generate(Transformer *transformer, Tokenizer *tokenizer, Sampler *sampler, char *prompt, int steps) {
    char *empty_prompt = "";
    if (prompt == NULL) { prompt = empty_prompt; }

    // encode the (string) prompt into tokens sequence
    int num_prompt_tokens = 0;
    int* prompt_tokens = (int*)malloc((strlen(prompt)+3) * sizeof(int)); // +3 for '\0', ?BOS, ?EOS
    encode(tokenizer, prompt, 1, 0, prompt_tokens, &num_prompt_tokens);
    if (num_prompt_tokens < 1) {
        fprintf(stderr, "something is wrong, expected at least 1 prompt token\n");
        exit(EXIT_FAILURE);
    }

    // start the main loop
    long start = 0;  // used to time our code, only initialized after first iteration
    int next;        // will store the next token in the sequence
    int token = prompt_tokens[0]; // kick off with the first token in the prompt
    int pos = 0;     // position in the sequence
    while (pos < steps) {

        // forward the transformer to get logits for the next token
        float* logits = forward(transformer, token, pos);

        // advance the state state machine
        if (pos < num_prompt_tokens - 1) {
            // if we are still processing the input prompt, force the next prompt token
            next = prompt_tokens[pos + 1];
        } else {
            // otherwise sample the next token from the logits
            next = sample(sampler, logits);
        }
        pos++;

        // data-dependent terminating condition: the BOS (=1) token delimits sequences
        if (next == 1) { break; }

        // print the token as string, decode it with the Tokenizer object
        char* piece = decode(tokenizer, token, next);
        safe_printf(piece); // same as printf("%s", piece), but skips "unsafe" bytes
        fflush(stdout);
        token = next;

        // init the timer here because the first iteration can be slower
        if (start == 0) { start = time_in_ms(); }
    }
    printf("\n");

    // report achieved tok/s (pos-1 because the timer starts after first iteration)
    if (pos > 1) {
        long end = time_in_ms();
        fprintf(stderr, "achieved tok/s: %f\n", (pos-1) / (double)(end-start)*1000);
    }

    free(prompt_tokens);
}

void read_stdin(const char* guide, char* buffer, size_t bufsize) {
    // read a line from stdin, up to but not including \n
    printf("%s", guide);
    if (fgets(buffer, bufsize, stdin) != NULL) {
        size_t len = strlen(buffer);
        if (len > 0 && buffer[len - 1] == '\n') {
            buffer[len - 1] = '\0'; // strip newline
        }
    }
}

// ----------------------------------------------------------------------------
// chat loop
// I manually inspected the tokens for a few chat conversations compared to
// python reference and that seemed ok, but this was not thoroughly tested and
// is not safely implemented, it's more a proof of concept atm.

void chat(Transformer *transformer, Tokenizer *tokenizer, Sampler *sampler,
          char *cli_user_prompt, char *cli_system_prompt, int steps) {

    // buffers for reading the system prompt and user prompt from stdin
    // you'll notice they are soomewhat haphazardly and unsafely set atm
    char system_prompt[512];
    char user_prompt[512];
    char rendered_prompt[1152];
    int num_prompt_tokens = 0;
    int* prompt_tokens = (int*)malloc(1152 * sizeof(int));
    int user_idx;

    // start the main loop
    int8_t user_turn = 1; // user starts
    int next;        // will store the next token in the sequence
    int token;       // stores the current token to feed into the transformer
    int prev_token;
    int pos = 0;     // position in the sequence
    while (pos < steps) {

        // when it is the user's turn to contribute tokens to the dialog...
        if (user_turn) {
            // get the (optional) system prompt at position 0
            if (pos == 0) {
                // at position 0, the user can also contribute a system prompt
                if (cli_system_prompt == NULL) {
                    // system prompt was not passed in, attempt to get it from stdin
                    read_stdin("Enter system prompt (optional): ", system_prompt, sizeof(system_prompt));
                } else {
                    // system prompt was passed in, use it
                    strcpy(system_prompt, cli_system_prompt);
                }
            }
            // get the user prompt
            if (pos == 0 && cli_user_prompt != NULL) {
                // user prompt for position 0 was passed in, use it
                strcpy(user_prompt, cli_user_prompt);
            } else {
                // otherwise get user prompt from stdin
                read_stdin("User: ", user_prompt, sizeof(user_prompt));
            }
            // render user/system prompts into the Llama 2 Chat schema
            if (pos == 0 && system_prompt[0] != '\0') {
                char system_template[] = "[INST] <<SYS>>\n%s\n<</SYS>>\n\n%s [/INST]";
                sprintf(rendered_prompt, system_template, system_prompt, user_prompt);
            } else {
                char user_template[] = "[INST] %s [/INST]";
                sprintf(rendered_prompt, user_template, user_prompt);
            }
            // encode the rendered prompt into tokens
            encode(tokenizer, rendered_prompt, 1, 0, prompt_tokens, &num_prompt_tokens);
            user_idx = 0; // reset the user index
            user_turn = 0;
            printf("Assistant: ");
        }

        // determine the token to pass into the transformer next
        if (user_idx < num_prompt_tokens) {
            // if we are still processing the input prompt, force the next prompt token
            token = prompt_tokens[user_idx++];
        } else {
            // otherwise use the next token sampled from previous turn
            token = next;
        }
        // EOS (=2) token ends the Assistant turn
        if (token == 2) { user_turn = 1; }

        // forward the transformer to get logits for the next token
        float* logits = forward(transformer, token, pos);
        next = sample(sampler, logits);
        pos++;

        if (user_idx >= num_prompt_tokens && next != 2) {
            // the Assistant is responding, so print its output
            char* piece = decode(tokenizer, token, next);
            safe_printf(piece); // same as printf("%s", piece), but skips "unsafe" bytes
            fflush(stdout);
        }
        if (next == 2) { printf("\n"); }
    }
    printf("\n");
    free(prompt_tokens);
}


// ----------------------------------------------------------------------------
// DOS-PORT: --benchmark machinery (Phase 4). Three pieces:
// DOS-PORT:   1. CPUID-based CPU identification with a 486 fallback.
// DOS-PORT:      CPUID is a Pentium-era instruction (introduced on later
// DOS-PORT:      i486SX2/DX4 steppings). EFLAGS bit 21 (the ID flag) is
// DOS-PORT:      writable iff CPUID is supported, so we probe via
// DOS-PORT:      pushfl/popfl before issuing any cpuid instruction —
// DOS-PORT:      otherwise an early-486 host would #UD.
// DOS-PORT:   2. Peak-memory snapshot via _go32_dpmi_get_free_memory_information().
// DOS-PORT:   3. Fixed-args generation loop with separated prompt/gen
// DOS-PORT:      timing, so packager's bench/run.sh can grep tok/s.

// DOS-PORT: Probe whether CPUID is supported.  Returns 1 if so, 0 if the
// DOS-PORT: CPU is a pre-CPUID 486.  Pure inline asm — must come before
// DOS-PORT: any direct cpuid invocation; otherwise we'd #UD on those hosts.
// DOS-PORT: Only the i386 asm path is real; the host (x86_64) reference
// DOS-PORT: build returns 0 so bench_cpu_brand falls through to "unknown".
static int bench_has_cpuid(void) {
#if defined __i386__
    unsigned int after, before;
    __asm__ volatile (
        /* Save EFLAGS into `after`, copy to `before` as the original. */
        "pushfl                  \n\t"
        "popl   %0               \n\t"
        "movl   %0, %1           \n\t"
        /* Flip bit 21 (the ID flag) and try to load it back. */
        "xorl   $0x00200000, %0  \n\t"
        "pushl  %0               \n\t"
        "popfl                   \n\t"
        /* Read EFLAGS again — bit 21 stays flipped iff CPUID is supported. */
        "pushfl                  \n\t"
        "popl   %0               \n\t"
        /* Restore the original EFLAGS so we don't perturb DPMI state. */
        "pushl  %1               \n\t"
        "popfl                   \n\t"
        : "=&r" (after), "=&r" (before)
        :
        : "cc"
    );
    return ((after ^ before) & 0x00200000u) != 0;
#else
    /* Non-i386 host build: report "no CPUID" rather than emit broken asm. */
    return 0;
#endif
}

// DOS-PORT: Issue one cpuid leaf. ebx is clobbered by the instruction; we
// DOS-PORT: list it as an output so gcc handles the register correctly under
// DOS-PORT: every code model. DJGPP builds non-PIC by default, but the same
// DOS-PORT: form is safe under -fPIC too. Only called when bench_has_cpuid()
// DOS-PORT: returned 1, so the non-i386 branch should never execute it.
static void bench_cpuid(unsigned int leaf,
                        unsigned int *eax, unsigned int *ebx,
                        unsigned int *ecx, unsigned int *edx) {
#if defined __i386__
    __asm__ volatile (
        "cpuid"
        : "=a" (*eax), "=b" (*ebx), "=c" (*ecx), "=d" (*edx)
        : "a" (leaf)
    );
#else
    (void)leaf;
    *eax = *ebx = *ecx = *edx = 0;
#endif
}

// DOS-PORT: Fill `out` (must be >= 64 bytes) with a CPU identification string.
// DOS-PORT: Three tiers, in order:
// DOS-PORT:   - extended-leaf brand string (Pentium 4 / Athlon era and later)
// DOS-PORT:   - vendor + family/model/stepping from leaves 0/1 (P5/P6 era,
// DOS-PORT:     including the target PODP5V83 — reports family 5 model 3)
// DOS-PORT:   - "unknown (no CPUID)" for pre-CPUID 486s.
static void bench_cpu_brand(char *out, size_t outsz) {
    unsigned int a, b, c, d;
    if (outsz == 0) return;
    out[0] = '\0';
    if (!bench_has_cpuid()) {
        snprintf(out, outsz, "unknown (no CPUID)");
        return;
    }
    /* How many extended leaves does this CPU support? */
    bench_cpuid(0x80000000u, &a, &b, &c, &d);
    if (a >= 0x80000004u) {
        /* Brand string: 48 chars across three 16-byte register quadruples. */
        unsigned int regs[12];
        char buf[49];
        char *p;
        bench_cpuid(0x80000002u, &regs[0],  &regs[1],  &regs[2],  &regs[3]);
        bench_cpuid(0x80000003u, &regs[4],  &regs[5],  &regs[6],  &regs[7]);
        bench_cpuid(0x80000004u, &regs[8],  &regs[9],  &regs[10], &regs[11]);
        memcpy(buf, regs, 48);
        buf[48] = '\0';
        /* Intel's brand strings are right-justified; trim leading spaces. */
        p = buf;
        while (*p == ' ') p++;
        snprintf(out, outsz, "%s", p);
    } else {
        /* Pre-extended-leaf path. Vendor is EBX|EDX|ECX of leaf 0. */
        unsigned int v0, v1, v2, v3;
        char vendor[13];
        unsigned int family, model, stepping;
        bench_cpuid(0x0u, &v0, &v1, &v2, &v3);
        memcpy(vendor + 0, &v1, 4);  /* EBX */
        memcpy(vendor + 4, &v3, 4);  /* EDX */
        memcpy(vendor + 8, &v2, 4);  /* ECX */
        vendor[12] = '\0';
        bench_cpuid(0x1u, &a, &b, &c, &d);
        /* Pre-Pentium-Pro encoding — extended fields are zero on P5,
         * so plain (a >> N) & 0xf is correct for the target hardware. */
        family   = (a >> 8) & 0xfu;
        model    = (a >> 4) & 0xfu;
        stepping = a & 0xfu;

        /* DOS-PORT: map GenuineIntel family+model to a friendly name for
         * era-relevant CPUs (486 + all P5 variants, including the Pentium
         * OverDrive at family 5 model 3 — the primary vellm target).
         * Unknown combinations fall through to the raw format. */
        const char *friendly = NULL;
        if (memcmp(vendor, "GenuineIntel", 12) == 0) {
            if (family == 4) {
                switch (model) {
                    case 0: case 1: friendly = "Intel 80486 DX"; break;
                    case 2: friendly = "Intel 80486 SX"; break;
                    case 3: friendly = "Intel 80486 DX2"; break;
                    case 4: friendly = "Intel 80486 SL"; break;
                    case 5: friendly = "Intel 80486 SX2"; break;
                    case 7: friendly = "Intel 80486 DX2 (write-back)"; break;
                    case 8: friendly = "Intel 80486 DX4"; break;
                    case 9: friendly = "Intel 80486 DX4 (write-back)"; break;
                }
            } else if (family == 5) {
                switch (model) {
                    case 0: friendly = "Intel Pentium (P5)"; break;
                    case 1: case 2: case 7: friendly = "Intel Pentium (P54C)"; break;
                    case 3: friendly = "Intel Pentium OverDrive"; break;
                    case 4: friendly = "Intel Pentium MMX (P55C)"; break;
                    case 8: friendly = "Intel Pentium MMX (Tillamook)"; break;
                }
            }
        }
        if (friendly) {
            snprintf(out, outsz, "%s", friendly);
        } else {
            snprintf(out, outsz, "%s family %u model %u stepping %u",
                     vendor, family, model, stepping);
        }
    }
}

// DOS-PORT: Strip directory components (both '/' and '\\') from a path.
// DOS-PORT: Used to normalize the `model` field in the benchmark report so
// DOS-PORT: it is identical on Linux and DOS regardless of how the user
// DOS-PORT: invoked vellm.exe (e.g. C:\MODELS\STORIES15M_Q80.BIN).
static const char *bench_basename(const char *path) {
    const char *p = path;
    const char *base = path;
    for (; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}

// DOS-PORT: Result struct for the benchmark generation loop.
typedef struct {
    int  prompt_tokens;   /* number of tokens encoded from the prompt */
    int  gen_tokens;      /* number of tokens generated post-prompt */
    long prompt_ms;       /* wall-time spent forwarding the prompt */
    long gen_ms;          /* wall-time spent generating new tokens */
    long total_ms;        /* prompt_ms + gen_ms */
} BenchResult;

// DOS-PORT: Parallel to generate(), but with separated prompt/gen timing
// DOS-PORT: and no token decoding — the canonical benchmark workload.
// DOS-PORT: Model-load time is excluded; clock starts at the first forward()
// DOS-PORT: call. PIT granularity is ~55ms, fine for the multi-second
// DOS-PORT: phases this measures.
// DOS-PORT: Progress throbber for long --benchmark runs. stderr-only (\r
// DOS-PORT: to overwrite in place) so the machine-parseable stdout block
// DOS-PORT: stays intact. At 0.27 tok/s on real PODP5V83 a 15M benchmark
// DOS-PORT: is a ~12-minute wall, and 42M is ~30 — watching a blank
// DOS-PORT: screen for that long is bad UX. Classic 4-char spinner
// DOS-PORT: (|/-\) cycles on each token; position counter shows progress.
static void bench_throbber(int pos, int steps) {
    static const char spin[] = "|/-\\";
    fprintf(stderr, "\r  %c  generating: %d/%d tokens ",
            spin[pos & 3], pos, steps);
    fflush(stderr);
}
static void bench_throbber_done(int pos, int steps) {
    /* Leave a permanent line before the stdout block prints — useful on
     * screenshots and when stderr+stdout interleave on the DOS console. */
    fprintf(stderr, "\r  done: generated %d of %d tokens.          \n",
            pos, steps);
    fflush(stderr);
}

static void generate_benchmark(Transformer *transformer, Tokenizer *tokenizer,
                               Sampler *sampler, char *prompt, int steps,
                               BenchResult *result) {
    int num_prompt_tokens = 0;
    int *prompt_tokens;
    long t_start, t_prompt_end, t_end;
    int next, token, pos;

    if (prompt == NULL) prompt = "";
    prompt_tokens = (int*)malloc((strlen(prompt) + 3) * sizeof(int));
    encode(tokenizer, prompt, 1, 0, prompt_tokens, &num_prompt_tokens);
    if (num_prompt_tokens < 1) {
        fprintf(stderr, "benchmark: empty prompt encoding\n");
        exit(EXIT_FAILURE);
    }

    t_start = time_in_ms();
    t_prompt_end = t_start;
    token = prompt_tokens[0];
    pos = 0;
    next = 0;

    while (pos < steps) {
        float *logits = forward(transformer, token, pos);
        if (pos < num_prompt_tokens - 1) {
            next = prompt_tokens[pos + 1];
        } else {
            next = sample(sampler, logits);
        }
        pos++;
        // DOS-PORT: throbber update every token. Cheap stderr print,
        // DOS-PORT: negligible vs. matmul; preserves progress visibility
        // DOS-PORT: on the 12-min / 30-min real-HW runs.
        bench_throbber(pos, steps);
        if (pos == num_prompt_tokens) {
            /* Boundary between prompt forwarding and free-running generation. */
            t_prompt_end = time_in_ms();
        }
        if (next == 1) break; /* BOS terminator from upstream */
        token = next;
    }
    t_end = time_in_ms();
    // DOS-PORT: finalize the throbber line so the stdout block starts clean.
    bench_throbber_done(pos, steps);

    if (pos < num_prompt_tokens) {
        /* `steps` cut us off before generation began; charge it all to prompt. */
        t_prompt_end = t_end;
    }

    result->prompt_tokens = num_prompt_tokens;
    result->gen_tokens    = pos - num_prompt_tokens;
    if (result->gen_tokens < 0) result->gen_tokens = 0;
    result->prompt_ms     = t_prompt_end - t_start;
    result->gen_ms        = t_end - t_prompt_end;
    result->total_ms      = t_end - t_start;

    free(prompt_tokens);
}

// DOS-PORT: Read the largest free DPMI block in bytes (a proxy for "how
// DOS-PORT: much memory has the process committed so far"). On non-DJGPP
// DOS-PORT: hosts (the Linux reference build via Makefile.host) returns 0
// DOS-PORT: so the report still emits valid numbers, just without a
// DOS-PORT: meaningful peak-mem field.
static unsigned long bench_dpmi_free_bytes(void) {
#if defined __DJGPP__
    __dpmi_free_mem_info info;
    memset(&info, 0, sizeof(info));
    if (_go32_dpmi_get_free_memory_information(&info) != 0) {
        return 0;
    }
    /* The "largest contiguous free block" field — same one used in
     * docs/phase2-memory.md. CWSDPMI populates it consistently. */
    return info.largest_available_free_block_in_bytes;
#else
    return 0;
#endif
}

// DOS-PORT: Measure CPU clock rate by reading rdtsc across a known wall-time
// DOS-PORT: interval. We use the BIOS tick counter at 0040:006C directly —
// DOS-PORT: libc's clock() on DJGPP advances in 10 ms units nominally but is
// DOS-PORT: actually driven by the 18.2 Hz BIOS interrupt (≈55 ms per tick),
// DOS-PORT: so waiting on clock() introduces a 0–55 ms jitter between "time
// DOS-PORT: of c0 read" and "time c0 reflects." The BIOS tick counter jumps
// DOS-PORT: atomically at each interrupt, so we can sync-to-edge and then
// DOS-PORT: measure N ticks of exact 65536/1193181.8 s each (= 54.9254 ms).
// DOS-PORT: N_TICKS=8 gives a ~440 ms calibration window — long enough to
// DOS-PORT: capture ~36M rdtsc cycles on an 83 MHz P5, short enough not to
// DOS-PORT: noticeably delay startup.
// DOS-PORT: On an 83 MHz PODP5V83 this reads ~83.0; on DOSBox-X with
// DOS-PORT: cycles=fixed N it reflects the emulated rate. Returns 0.0 on
// DOS-PORT: host native builds and on pre-Pentium CPUs that lack rdtsc.
static double bench_measure_mhz(void) {
#if defined __DJGPP__
    if (!bench_has_cpuid()) return 0.0;  // rdtsc is Pentium-era alongside CPUID
    enum { N_TICKS = 8 };  // ~440 ms window
    unsigned long t_now, t_sync, t_end;
    uint64_t r0, r1;

    /* Sync to the next BIOS tick edge so the measurement window starts
     * aligned — eliminates the 0-to-55 ms bias of "c0 read mid-tick". */
    dosmemget(0x0046Cu, 4, &t_now);
    do { dosmemget(0x0046Cu, 4, &t_sync); } while (t_sync == t_now);

    __asm__ volatile("rdtsc" : "=A"(r0));
    do { dosmemget(0x0046Cu, 4, &t_end); } while (t_end - t_sync < N_TICKS);
    __asm__ volatile("rdtsc" : "=A"(r1));

    unsigned long n_ticks = t_end - t_sync;
    /* BIOS timer period: 65536 counts of the 1.193182 MHz 8254 divider. */
    double elapsed_s = (double)n_ticks * 65536.0 / 1193181.8181818;
    if (elapsed_s <= 0.0) return 0.0;
    return ((double)(r1 - r0)) / (elapsed_s * 1.0e6);
#else
    return 0.0;
#endif
}

// DOS-PORT: Cache the MHz measurement so banner + benchmark report share one
// DOS-PORT: 165 ms calibration instead of paying it twice.
static double bench_mhz_cached(void) {
    static double cached = -1.0;
    if (cached < 0.0) cached = bench_measure_mhz();
    return cached;
}

// DOS-PORT: Conventional DOS memory in KB via INT 12h (real-mode BIOS call
// DOS-PORT: bridged through DPMI). Returns 0 on host native. Nearly always
// DOS-PORT: 640 KB on modern DOS; low value is a signal of a config issue.
static unsigned int bench_conv_kb(void) {
#if defined __DJGPP__
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    __dpmi_int(0x12, &r);
    return r.x.ax;
#else
    return 0;
#endif
}

// DOS-PORT: Physical extended memory in KB via INT 15h. Reports actual RAM
// DOS-PORT: above 1 MB, not DPMI virtual address space. Uses E801h (post-1994
// DOS-PORT: standard, unambiguous above 64 MB) and falls back to AH=88h (16-bit
// DOS-PORT: AX = KB extended, capped at ~64 MB) for older BIOSes. This is the
// DOS-PORT: "your machine has N MB of RAM" figure the banner should show —
// DOS-PORT: CWSDPMI's _go32_dpmi_get_free_memory_information() returns the
// DOS-PORT: virtual address space including paging-file potential, which on
// DOS-PORT: a 48 MB box routinely shows 150+ MB and misleads the reader.
static unsigned long bench_ext_kb(void) {
#if defined __DJGPP__
    __dpmi_regs r;

    /* E801h: ES:DI untouched, CF clear on success. Returns
     *   AX/CX = extended memory 1-16 MB in KB (both registers same value)
     *   BX/DX = extended memory above 16 MB in 64-KB blocks
     * Some BIOSes zero AX/BX and return only in CX/DX; handle both. */
    memset(&r, 0, sizeof r);
    r.x.ax = 0xE801;
    __dpmi_int(0x15, &r);
    if (!(r.x.flags & 0x0001u)) {  /* CF clear = success */
        unsigned int below16 = r.x.ax ? r.x.ax : r.x.cx;
        unsigned int above16 = r.x.bx ? r.x.bx : r.x.dx;
        return (unsigned long)below16 + (unsigned long)above16 * 64u;
    }

    /* Fall back to AH=88h: AX = KB of extended memory. Capped at 64 MB - 1 KB
     * because AX is 16 bits. On a 48 MB target this is sufficient. */
    memset(&r, 0, sizeof r);
    r.h.ah = 0x88;
    __dpmi_int(0x15, &r);
    return (unsigned long)r.x.ax;
#else
    return 0;
#endif
}

// DOS-PORT: CPU feature-flag summary from CPUID leaf 1 EDX. The flags we care
// DOS-PORT: about for vellm's correctness/shape: FPU (bit 0), TSC (bit 4),
// DOS-PORT: CMOV (bit 15), MMX (bit 23), SSE (bit 25). P54C has FPU + TSC,
// DOS-PORT: NO CMOV/MMX/SSE — printing those absences is the real proof this
// DOS-PORT: is running on pre-Pentium-II silicon. Shows present flags plus
// DOS-PORT: "no MMX/SSE" when those are absent (the two most diagnostic).
static void bench_cpu_features(char *out, size_t outsz) {
#if defined __DJGPP__
    if (!bench_has_cpuid()) { snprintf(out, outsz, "no CPUID"); return; }
    unsigned int a, b, c, d;
    bench_cpuid(0x1u, &a, &b, &c, &d);
    int fpu  = (d >> 0)  & 1;
    int tsc  = (d >> 4)  & 1;
    int cmov = (d >> 15) & 1;
    int mmx  = (d >> 23) & 1;
    int sse  = (d >> 25) & 1;
    /* Show only present flags. Omit ever-present ones implied by
     * the era (e.g. FPU on every Pentium-class chip — still show it,
     * as it's a legit CPUID bit; just don't call out absences). */
    snprintf(out, outsz, "%s%s%s%s%s",
             fpu  ? "FPU "  : "",
             tsc  ? "TSC "  : "",
             cmov ? "CMOV " : "",
             mmx  ? "MMX "  : "",
             sse  ? "SSE "  : "");
    /* Trim trailing space. */
    size_t n = strlen(out);
    while (n > 0 && out[n-1] == ' ') out[--n] = 0;
#else
    snprintf(out, outsz, "native");
#endif
}

// DOS-PORT: DOS version via INT 21h AH=30h. AL = major, AH = minor. On
// DOS-PORT: MS-DOS 6.22 this returns 6.22. DR-DOS / PC-DOS are also valid.
static void bench_dos_version(char *out, size_t outsz) {
#if defined __DJGPP__
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.h.ah = 0x30;
    __dpmi_int(0x21, &r);
    snprintf(out, outsz, "%u.%02u", r.h.al, r.h.ah);
#else
    snprintf(out, outsz, "n/a");
#endif
}

// DOS-PORT: BIOS date string at physical F000:FFF5 — 8 ASCII bytes "MM/DD/YY"
// DOS-PORT: present on every PC BIOS since the original IBM PC. Reading via
// DOS-PORT: DJGPP's dosmemget() (linear address 0xFFFF5). Validates that the
// DOS-PORT: bytes are printable before reporting; some BIOSes zero or pad the
// DOS-PORT: region. Returns "unknown" on any anomaly.
static void bench_bios_date(char *out, size_t outsz) {
#if defined __DJGPP__
    char buf[9];
    dosmemget(0xFFFF5u, 8, buf);
    buf[8] = 0;
    for (int i = 0; i < 8; i++) {
        if (buf[i] < 0x20 || buf[i] > 0x7E) {
            snprintf(out, outsz, "unknown");
            return;
        }
    }
    snprintf(out, outsz, "%s", buf);
#else
    snprintf(out, outsz, "n/a");
#endif
}

// DOS-PORT: Print a short hardware banner to stderr at main() entry.
// DOS-PORT: Goes to stderr so stdout redirects (RUN.BAT > OUT.TXT) still
// DOS-PORT: capture only inference output, but the banner remains on screen
// DOS-PORT: for screenshot-as-proof-of-hardware. Four lines, 7-bit ASCII.
static void bench_hw_banner(FILE *out) {
    char brand[64], features[64], dosver[16], biosdate[16];
    bench_cpu_brand(brand, sizeof(brand));
    bench_cpu_features(features, sizeof(features));
    bench_dos_version(dosver, sizeof(dosver));
    bench_bios_date(biosdate, sizeof(biosdate));
    double mhz = bench_mhz_cached();
    unsigned int conv_kb = bench_conv_kb();
    unsigned long ext_kb = bench_ext_kb();

    fprintf(out, "vellm (%s)\n",
#if defined __DJGPP__
            "MS-DOS / DJGPP"
#else
            "host native"
#endif
    );
    if (mhz > 0.0) {
        fprintf(out, "CPU: %s @ %.1f MHz [%s]\n", brand, mhz, features);
    } else {
        fprintf(out, "CPU: %s [%s]\n", brand, features);
    }
    if (ext_kb > 0) {
        /* Report physical RAM: conventional (640 KB on any sane config) plus
         * extended-memory detection via INT 15h. Round-to-nearest-MB on the
         * total so a 48 MB machine reads "48 MB" even if HIMEM shaved a few
         * KB of XMS handles from the reported extended figure. */
        unsigned long total_kb = (unsigned long)conv_kb + ext_kb;
        unsigned long total_mb = (total_kb + 512) / 1024;
        fprintf(out, "MEM: %lu MB RAM (%u KB conv + %.1f MB ext)\n",
                total_mb, conv_kb, (double)ext_kb / 1024.0);
    } else if (conv_kb > 0) {
        fprintf(out, "MEM: %u KB conv\n", conv_kb);
    }
    fprintf(out, "DOS: %s   BIOS: %s\n\n", dosver, biosdate);
    fflush(out);
}

// ----------------------------------------------------------------------------
// CLI, include only if not testing
#ifndef TESTING

void error_usage() {
    // DOS-PORT: upstream named its binary `run`; ours is `vellm.exe`. Also
    // DOS-PORT: show a realistic DOS invocation with 8.3 filenames matching
    // DOS-PORT: what the shipped CF package uses (STORY15.BIN / TOKEN.BIN).
    fprintf(stderr, "Usage:   vellm.exe <checkpoint> [options]\n");
    fprintf(stderr, "Example: vellm.exe STORY15.BIN -z TOKEN.BIN -t 0 -s 42 -i \"Once upon a time\"\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -t <float>  temperature in [0,inf], default 1.0\n");
    fprintf(stderr, "  -p <float>  p value in top-p (nucleus) sampling in [0,1] default 0.9\n");
    fprintf(stderr, "  -s <int>    random seed, default time(NULL)\n");
    fprintf(stderr, "  -n <int>    number of steps to run for, default 256. 0 = max_seq_len\n");
    fprintf(stderr, "  -i <string> input prompt\n");
    fprintf(stderr, "  -z <string> optional path to custom tokenizer\n");
    fprintf(stderr, "  -m <string> mode: generate|chat, default: generate\n");
    fprintf(stderr, "  -y <string> (optional) system prompt in chat mode\n");
    // DOS-PORT: --max-seq-len / -L cap KV cache allocation below the checkpoint's
    // DOS-PORT: baked-in seq_len (see build_transformer).
    fprintf(stderr, "  -L <int>    cap KV cache at N tokens (also: --max-seq-len N)\n");
    // DOS-PORT: --benchmark / -B reproducible perf report (Phase 4); see
    // DOS-PORT: generate_benchmark() above for the canonical workload.
    fprintf(stderr, "  -B          benchmark mode (also: --benchmark);\n");
    fprintf(stderr, "              ignores -t/-p/-s/-n/-i and runs the canonical workload\n");
    fprintf(stderr, "              (-t 0 -s 42 -n 200 -i \"The old computer hummed to life\"),\n");
    fprintf(stderr, "              prints a machine-readable report between\n");
    fprintf(stderr, "              --- VELLM BENCHMARK --- and --- END --- markers.\n");
    fprintf(stderr, "              --max-seq-len / -L is honored and clamps the\n");
    fprintf(stderr, "              canonical 200 tokens down to L when L<200\n");
    fprintf(stderr, "              (useful for 42M on memory-constrained hosts).\n");
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {

    // DOS-PORT: unbuffered stdout is painfully slow on DOS; force 4KB full buffering.
    setvbuf(stdout, NULL, _IOFBF, 4096);

    // DOS-PORT: capture the baseline DPMI free-block size before any
    // DOS-PORT: malloc()s in this process. Used by --benchmark to compute
    // DOS-PORT: peak demand at report time. Cheap (one DPMI int call), no
    // DOS-PORT: harm to non-benchmark runs.
    unsigned long bench_baseline_free = bench_dpmi_free_bytes();

    // DOS-PORT: hardware banner to stderr — visible in screenshots as proof of
    // DOS-PORT: actual CPU + MHz + DPMI on the host running this. Doesn't
    // DOS-PORT: pollute stdout, so RUN.BAT > OUT.TXT captures only inference.
    bench_hw_banner(stderr);

    // default parameters
    char *checkpoint_path = NULL;  // e.g. STORY15.BIN or STORY42.BIN on DOS
    // DOS-PORT: default tokenizer path is `TOKEN.BIN` (8.3, as the CF
    // DOS-PORT: package ships it), not upstream's long-name `tokenizer.bin`
    // DOS-PORT: which DOS 6.22 can't see without LFN support. Users who
    // DOS-PORT: keep the long-name file can still point at it via `-z`.
    char *tokenizer_path = "TOKEN.BIN";
    float temperature = 1.0f;   // 0.0 = greedy deterministic. 1.0 = original. don't set higher
    float topp = 0.9f;          // top-p in nucleus sampling. 1.0 = off. 0.9 works well, but slower
    int steps = 256;            // number of steps to run for
    char *prompt = NULL;        // prompt string
    unsigned long long rng_seed = 0; // seed rng with time by default
    char *mode = "generate";    // generate|chat
    char *system_prompt = NULL; // the (optional) system prompt to use in chat mode
    // DOS-PORT: -1 sentinel means "use checkpoint's Config.seq_len as-is";
    // DOS-PORT: any positive value caps the KV cache allocation below it.
    int max_seq_len = -1;
    // DOS-PORT: --benchmark / -B mode flag. Overrides user -t/-p/-s/-n/-i
    // DOS-PORT: with the canonical workload after parsing completes.
    int benchmark_mode = 0;

    // poor man's C argparse so we can override the defaults above from the command line
    if (argc >= 2) { checkpoint_path = argv[1]; } else { error_usage(); }
    // DOS-PORT: switched from `for (i = 2; ; i+=2)` to a manual-step loop so
    // DOS-PORT: --benchmark / -B can consume zero values without skipping the
    // DOS-PORT: next real flag.
    {
        int i = 2;
        while (i < argc) {
            if (argv[i][0] != '-') { error_usage(); } // must start with dash
            // Boolean (no-value) flags first.
            if (strcmp(argv[i], "--benchmark") == 0 || strcmp(argv[i], "-B") == 0) {
                benchmark_mode = 1;
                i += 1;
                continue;
            }
            // Value-taking flags from here on; require an argument to follow.
            if (i + 1 >= argc) { error_usage(); }
            // DOS-PORT: long-form --max-seq-len accepted alongside short -L (below).
            // DOS-PORT: handled before the two-letter-flag check so --max-seq-len
            // DOS-PORT: doesn't trip the strlen(argv[i]) != 2 reject.
            if (strcmp(argv[i], "--max-seq-len") == 0) {
                max_seq_len = atoi(argv[i + 1]);
                i += 2;
                continue;
            }
            if (strlen(argv[i]) != 2) { error_usage(); } // must be -x (one dash, one letter)
            // read in the args
            if (argv[i][1] == 't') { temperature = atof(argv[i + 1]); }
            else if (argv[i][1] == 'p') { topp = atof(argv[i + 1]); }
            else if (argv[i][1] == 's') { rng_seed = atoi(argv[i + 1]); }
            else if (argv[i][1] == 'n') { steps = atoi(argv[i + 1]); }
            else if (argv[i][1] == 'i') { prompt = argv[i + 1]; }
            else if (argv[i][1] == 'z') { tokenizer_path = argv[i + 1]; }
            else if (argv[i][1] == 'm') { mode = argv[i + 1]; }
            else if (argv[i][1] == 'y') { system_prompt = argv[i + 1]; }
            // DOS-PORT: -L short alias for --max-seq-len (L = "length cap").
            else if (argv[i][1] == 'L') { max_seq_len = atoi(argv[i + 1]); }
            else { error_usage(); }
            i += 2;
        }
    }

    // DOS-PORT: in --benchmark mode, override user-supplied perf-affecting
    // DOS-PORT: flags with the canonical Phase 3 workload. Keeps benchmark
    // DOS-PORT: results comparable across builds, machines, and operators.
    if (benchmark_mode) {
        temperature = 0.0f;
        topp        = 0.9f;
        rng_seed    = 42;
        steps       = 200;
        /* Locked Phase 4 prompt (per PLAN.md / team-lead spec). Do NOT change
         * — bench/run.sh's reference numbers are pinned to this exact string. */
        prompt      = "The old computer hummed to life";
        mode        = "generate";
        /* DOS-PORT: if the operator capped --max-seq-len below 200, clamp the
         * canonical 200-token target to fit the KV cache. Preserves benchmark
         * utility for memory-constrained configurations (e.g. stories42M_q80
         * on 48 MB real DOS, where even -L 200 still pages). The benchmark
         * output's `tokens` field reports the actual count, so parsers see
         * what happened; tok/s figures remain directly comparable across
         * scenarios because they're rates, not totals. */
        if (max_seq_len > 0 && max_seq_len < steps) steps = max_seq_len;
    }

    // DOS-PORT: validate --max-seq-len against -n before build_transformer so
    // DOS-PORT: we don't silently truncate generation.
    if (max_seq_len > 0 && steps > max_seq_len) {
        fprintf(stderr,
                "error: -n %d exceeds --max-seq-len %d (KV cache cap); "
                "either raise --max-seq-len or lower -n\n",
                steps, max_seq_len);
        exit(EXIT_FAILURE);
    }

    // parameter validation/overrides
    if (rng_seed <= 0) rng_seed = (unsigned int)time(NULL);
    if (temperature < 0.0) temperature = 0.0;
    if (topp < 0.0 || 1.0 < topp) topp = 0.9;
    if (steps < 0) steps = 0;

    // build the Transformer via the model .bin file
    Transformer transformer;
    build_transformer(&transformer, checkpoint_path, max_seq_len);
    if (steps == 0 || steps > transformer.config.seq_len) steps = transformer.config.seq_len; // override to ~max length

    // build the Tokenizer via the tokenizer .bin file
    Tokenizer tokenizer;
    build_tokenizer(&tokenizer, tokenizer_path, transformer.config.vocab_size);

    // build the Sampler
    Sampler sampler;
    build_sampler(&sampler, transformer.config.vocab_size, temperature, topp, rng_seed);

    // run!
    // DOS-PORT: --benchmark short-circuits the normal generate/chat dispatch
    // DOS-PORT: with a fixed workload, separated prompt/gen timing, and a
    // DOS-PORT: machine-readable report. No decoded tokens are printed so
    // DOS-PORT: stdout stays cleanly grep-able by packager's bench/run.sh.
    if (benchmark_mode) {
        BenchResult br;
        char cpu_brand[96];
        unsigned long bench_end_free;
        unsigned long peak_mem;
        double prompt_tps, gen_tps;

        generate_benchmark(&transformer, &tokenizer, &sampler, prompt, steps, &br);

        bench_end_free = bench_dpmi_free_bytes();
        /* baseline >= end normally; clamp to 0 if the host returned 0
         * (non-DJGPP build) or if measurement noise inverted the order. */
        peak_mem = (bench_baseline_free > bench_end_free)
                   ? (bench_baseline_free - bench_end_free) : 0;

        bench_cpu_brand(cpu_brand, sizeof(cpu_brand));

        prompt_tps = (br.prompt_ms > 0)
                     ? ((double)br.prompt_tokens * 1000.0 / (double)br.prompt_ms) : 0.0;
        gen_tps    = (br.gen_ms > 0)
                     ? ((double)br.gen_tokens    * 1000.0 / (double)br.gen_ms)    : 0.0;

        printf("--- VELLM BENCHMARK ---\n");
        printf("cpu        : %s\n", cpu_brand);
        // DOS-PORT: measured MHz — cached from the startup banner's
        // DOS-PORT: calibration; matches the stderr banner value.
        printf("cpu mhz    : %.1f\n", bench_mhz_cached());
        printf("model      : %s\n", bench_basename(checkpoint_path));
        printf("ckpt bytes : %ld\n", (long)transformer.file_size);
        printf("tokens     : %d\n", br.prompt_tokens + br.gen_tokens);
        printf("prompt tok : %d\n", br.prompt_tokens);
        printf("gen tok    : %d\n", br.gen_tokens);
        printf("wall ms    : %ld\n", br.total_ms);
        printf("prompt tok/s: %.2f\n", prompt_tps);
        printf("gen tok/s  : %.2f\n", gen_tps);
        printf("peak mem   : %lu\n", peak_mem);
        printf("--- END ---\n");
        fflush(stdout);
    } else if (strcmp(mode, "generate") == 0) {
        generate(&transformer, &tokenizer, &sampler, prompt, steps);
    } else if (strcmp(mode, "chat") == 0) {
        chat(&transformer, &tokenizer, &sampler, prompt, system_prompt, steps);
    } else {
        fprintf(stderr, "unknown mode: %s\n", mode);
        error_usage();
    }

    // memory and file handles cleanup
    free_sampler(&sampler);
    free_tokenizer(&tokenizer);
    free_transformer(&transformer);
    return 0;
}
#endif
