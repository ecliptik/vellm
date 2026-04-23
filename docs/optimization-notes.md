# Optimization notes

TBD (Phase 3).

This document will collect Pentium U/V-pipe tuning observations, blocked
matmul experiments, fixed-point matmul results, and cache-residency notes
from the `rdtsc`-backed harness in `src/timing.c`.

Until Phase 3 lands, `-ffast-math` is off and the matmul is a trivial
nested loop — see `src/matmul.c`.
