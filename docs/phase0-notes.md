# Phase 0 Notes — Toolchain & Smoke Test

Captured while wiring up the DJGPP cross-compiler + DOSBox-X harness. These
are landmines we hit; recording them so the next person does not.

## Toolchain

- **DJGPP install location:** `~/emulators/tools/djgpp/` (mirrors the
  `~/emulators/tools/retro68-build/` convention from the Mac retro pipeline).
  `tools/build-djgpp.sh` wraps the upstream andrewwutw/build-djgpp script with
  an idempotency check so re-runs on a warm system are a no-op.
- **GCC version:** 12.2.0 (latest supported by build-djgpp.sh as of this
  checkout). Build takes ~45 minutes on a modern Linux host.
- **Two PATH entries are required.** DJGPP splits the cross toolchain across
  two bin dirs:
    - `djgpp/bin/` — cross-gcc, cross-binutils (`i586-pc-msdosdjgpp-*`)
    - `djgpp/i586-pc-msdosdjgpp/bin/` — target-side utilities (`stubedit`,
      `stubify`, `exe2coff`, `dxe3gen`)
  The Makefile prepends both. Missing the second dir is the most likely way
  `make` fails with "stubedit: No such file or directory".

## DOSBox-X Headless Harness

- **Version installed via apt:** `dosbox-x 2025.02.01 SDL2`.
- **Headless invocation:** `dosbox-x -silent -exit -nogui -nomenu`. `-silent`
  both suppresses the startup banner and (critically) forces exit after
  AUTOEXEC.BAT completes without waiting on user input.
- **AUTOEXEC.BAT vs. `-c` flags.** `tools/dosbox-x.conf` leaves `[autoexec]`
  empty; `tools/dosbox-run.sh` injects `MOUNT C`, `C:`, `CALL RUN.BAT`, and
  `EXIT` via repeated `-c` flags. Keeps the conf invocation-agnostic.
- **COMMAND.COM batch-redirection quirk.** Inside a batch script, `CALL
  FOO.BAT > OUT.TXT` does NOT capture FOO.BAT's stdout — redirection of a
  called batch is a known DOS 6.22 / COMMAND.COM limitation, and DOSBox-X
  reproduces it faithfully. Direct redirection of an `.EXE` works fine:
  `HELLO.EXE > STDOUT.TXT` captures correctly. Workaround for `.BAT` targets:
  spawn a sub-shell via `COMMAND /C FOO.BAT > OUT.TXT`. `dosbox-run.sh`
  does this automatically when `--exe` points at a `.BAT` (smoke-test path).
- **Pentium profile.** `cputype=pentium` (not `pentium_mmx` — the PODP5V83 has
  no MMX). `core=normal` keeps emulation deterministic so byte-exact golden
  diffs stay stable. `cycles=fixed 90000` is a starting point for 83 MHz
  P54C integer perf; Phase 5 will re-calibrate against a reference benchmark.

## hello.exe Results

- Size: 149,684 bytes (COFF + go32 v2.05 stub + libc). For reference when
  Phase 1 adds real code.
- `file hello.exe` → `MS-DOS executable, COFF for MS-DOS, DJGPP go32 v2.05 DOS
  extender (stub), autoload "CWSDPMI.EXE"` — confirms the stubedit step ran
  and CWSDPMI is declared as the DPMI host.
- Runs cleanly under DOSBox-X with CWSDPMI auto-staged by `dosbox-run.sh`.
- Captured stdout: `hello from vellm`.
- Note: the stub's "autoload CWSDPMI.EXE" line means CWSDPMI must sit next to
  the exe on disk. `dosbox-run.sh` auto-copies `vendor/cwsdpmi/cwsdpmi.exe`
  into the stage dir if present, so smoke tests and real runs behave the
  same way.

## Known Quirks to Watch For Later

- **Build-djgpp.sh is noisy.** It runs binutils → djcrx → djlsr → GCC → GMP
  → MPFR → MPC in sequence and dumps raw compiler output. The log is fine
  to archive but don't try to grep it for status — the final "Installing..."
  and "DJGPP install complete" lines from the upstream script are the only
  signal worth watching.
- **stubedit exit status.** `stubedit` silently succeeds with exit 0 even
  when the COFF section it expects is malformed. If future stubedit failures
  start showing up as "works but behavior is wrong at runtime", dump the exe
  with `objdump` rather than trusting the exit code.
