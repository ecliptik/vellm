/*
 * matmul.c — trivial fp32 matmul.
 *
 * This is a correctness-first, nested-loop implementation. It exists for
 * Phase 1 (byte-identical to upstream run.c on the golden prompt). Phase 3
 * replaces the body with blocked / Pentium-U/V-pipe variants, without
 * changing the public signature in matmul.h.
 *
 * Constraints (see CLAUDE.md):
 *   - No MMX/SSE (P54C predates MMX).
 *   - No -ffast-math in Phase 1.
 *   - No long double; no C99 VLAs in hot paths.
 */

#include "matmul.h"

void matmul_fp32(float *xout, const float *x, const float *w, int n, int d) {
    int i, j;
    for (i = 0; i < d; i++) {
        float sum = 0.0f;
        const float *wi = w + (long)i * n;
        for (j = 0; j < n; j++) {
            sum += wi[j] * x[j];
        }
        xout[i] = sum;
    }
}
