/*
 * quant.c — int8 quant loader (stub, Phase 2).
 *
 * See docs/format.md for the `.q8` format description once it lands.
 * Real implementation comes in Phase 2; for now q8_load returns -1 so
 * the rest of the codebase can link against a stable symbol.
 */

#include "quant.h"

int q8_load(const char *path) {
    (void)path;
    return -1; /* not implemented */
}
