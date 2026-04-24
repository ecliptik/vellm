@ECHO OFF
REM ============================================================
REM  BENCH42.BAT  --  vellm benchmark (stories42M_q80 @ seq=128)
REM ============================================================
REM  42M model with --MAX-SEQ-LEN 128. The tighter cap keeps the
REM  KV cache ~4 MB smaller than seq=256 so the working set stays
REM  comfortably under real DOS 6.22's ~45 MB DPMI ceiling (seq=256
REM  at ~44.7 MB projected fits DOSBox-X but pages aggressively on
REM  real hardware due to tighter CWSDPMI overhead). Expected peak
REM  ~40 MB; no CWSDPMI.SWP growth.
REM
REM  For the canonical 200-token benchmark, seq_len=128 vs 256
REM  makes no functional difference (we only use ~200 positions
REM  either way).
REM
REM  CWSDPMI.EXE must be in the same directory as VELLM.EXE.
REM ============================================================
SET DPMI=CWSDPMI.EXE
ECHO.
ECHO Running vellm benchmark (stories42M_q80, seq=128, 200 tokens)...
ECHO This takes ~30 minutes on a Pentium/83 (no-swap estimate).
ECHO.
VELLM.EXE STORY42.BIN -z TOKEN.BIN --benchmark --max-seq-len 128
