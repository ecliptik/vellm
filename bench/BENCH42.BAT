@ECHO OFF
REM ============================================================
REM  BENCH42.BAT  --  vellm benchmark (stories42M_q80 @ seq=200)
REM ============================================================
REM  42M model with --MAX-SEQ-LEN 200. This is the tightest legal
REM  KV cache cap for the canonical benchmark: --benchmark mode
REM  hardcodes -n 200 tokens, so -L must be at least 200 (the
REM  earlier -L 128 failed with "-n 200 exceeds --max-seq-len").
REM
REM  Projected peak ~44.2 MB — slightly below seq=256's 44.7 MB.
REM  On real DOS 6.22 with CWSDPMI this sits right against the
REM  physical ceiling (~43-45 MB usable depending on config),
REM  so expect either clean runs or light paging. If CWSDPMI.SWP
REM  grows substantially during the run, 42M simply doesn't fit
REM  in 48 MB without swap and that's the documented outcome.
REM
REM  CWSDPMI.EXE must be in the same directory as VELLM.EXE.
REM ============================================================
SET DPMI=CWSDPMI.EXE
ECHO.
ECHO Running vellm benchmark (stories42M_q80, seq=200, 200 tokens)...
ECHO This takes ~30 minutes on a Pentium/83 (no-swap estimate).
ECHO.
VELLM.EXE STORY42.BIN -z TOKEN.BIN --benchmark --max-seq-len 200
