/*
 * matmul.h — isolated matmul interface.
 *
 * The implementation in matmul.c is deliberately naive and exists only so
 * that the rest of the codebase can link against a stable signature from
 * Phase 0 onward. Phase 3 replaces the body with blocked / U-V-pipe-tuned
 * variants and (optionally) a fixed-point branch; the signature stays the
 * same so callers do not have to change.
 */
#ifndef VELLM_MATMUL_H
#define VELLM_MATMUL_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * xout = W * x
 *   x    : length n
 *   W    : row-major d x n
 *   xout : length d
 */
void matmul_fp32(float *xout, const float *x, const float *w, int n, int d);

#ifdef __cplusplus
}
#endif

#endif /* VELLM_MATMUL_H */
