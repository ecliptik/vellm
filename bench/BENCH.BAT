@ECHO OFF
REM ============================================================
REM  BENCH.BAT  --  vellm benchmark (stories15M_q80)
REM ============================================================
REM  Runs VELLM.EXE in --benchmark mode. Fixed canonical prompt,
REM  seed, temp, and 200 tokens. Prints the parseable
REM  "--- VELLM BENCHMARK ---" block to stdout.
REM
REM  CWSDPMI.EXE must be in the same directory as VELLM.EXE.
REM ============================================================
SET DPMI=CWSDPMI.EXE
ECHO.
ECHO Running vellm benchmark (stories15M_q80, 200 tokens)...
ECHO This takes ~3 minutes on a Pentium/83.
ECHO.
VELLM.EXE STORIES15M_Q80.BIN -z TOKENIZER.BIN --benchmark
