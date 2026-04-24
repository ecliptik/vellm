@ECHO OFF
REM ============================================================
REM  BENCH42.BAT  --  vellm benchmark (stories42M_q80 @ seq=128)
REM ============================================================
REM  42M model with --MAX-SEQ-LEN 128. This cap serves two purposes:
REM    1. Shrinks the KV cache by ~1.1 megs vs seq=256, dropping peak
REM       memory to ~43.6 megs — clear of real DOS's ~43-45 megs ceiling
REM       so CWSDPMI doesn't page.
REM    2. --benchmark respects --max-seq-len and clamps the canonical
REM       200-token generation down to 128 when the cap is below 200.
REM       The output block's `tokens:` field reports the actual count.
REM
REM  Tok/s results remain directly comparable to the 15M benchmark
REM  (it's a rate, not a total); only the total wall time differs.
REM
REM  If seq=128 still pages on your specific DOS config, drop to
REM  --max-seq-len 96 or 64.  42M's physical floor on 48 megs real DOS
REM  is machine-dependent (drivers, TSRs, HIMEM config).
REM
REM  CWSDPMI.EXE must be in the same directory as VELLM.EXE.
REM ============================================================
SET DPMI=CWSDPMI.EXE
ECHO.
ECHO Running vellm benchmark (stories42M_q80, seq=128, 128 tokens)...
ECHO This takes ~20 minutes on a Pentium/83 (measured, 0.11 tok/s).
ECHO.
VELLM.EXE STORY42.BIN -z TOKEN.BIN --benchmark --max-seq-len 128
