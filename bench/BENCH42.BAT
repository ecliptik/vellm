@ECHO OFF
REM ============================================================
REM  BENCH42.BAT  --  vellm benchmark (stories42M_q80 @ seq=256)
REM ============================================================
REM  42M model with --MAX-SEQ-LEN 256 so the KV cache fits under
REM  the 48 MB / CWSDPMI physical ceiling. Expected peak demand
REM  ~44.7 MB; should run with zero CWSDPMI.SWP growth.
REM
REM  CWSDPMI.EXE must be in the same directory as VELLM.EXE.
REM ============================================================
SET DPMI=CWSDPMI.EXE
ECHO.
ECHO Running vellm benchmark (stories42M_q80, seq=256, 200 tokens)...
ECHO This takes ~8 minutes on a Pentium/83.
ECHO.
VELLM.EXE STORY42.BIN -z TOKEN.BIN --benchmark --max-seq-len 256
