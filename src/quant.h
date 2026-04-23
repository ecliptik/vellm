/*
 * quant.h — int8 quantization interfaces (stubs for Phase 2).
 *
 * The `.q8` file format lives in docs/format.md. This header declares the
 * public surface that src/vellm.c will call once Phase 2 lands; the bodies
 * in quant.c are stubs returning -1 for now so the rest of the codebase
 * can compile against stable symbols from day 1.
 */
#ifndef VELLM_QUANT_H
#define VELLM_QUANT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define Q8_MAGIC0  'Q'
#define Q8_MAGIC1  '8'
#define Q8_MAGIC2  0x00
#define Q8_MAGIC3  0x01
#define Q8_VERSION 1u

struct q8_header {
    uint8_t  magic[4];     /* 'Q', '8', 0x00, 0x01 */
    uint32_t version;      /* Q8_VERSION */
    uint32_t tensor_count; /* number of tensors that follow */
    uint8_t  reserved[8];  /* zero-padded, reserved for future use */
};

/*
 * q8_load — Phase 2 will mmap-ish load a .q8 file into caller-provided
 * tensor descriptors. For now this is a stub that returns -1 (not
 * implemented). The full API lives in docs/format.md once written.
 */
int q8_load(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* VELLM_QUANT_H */
