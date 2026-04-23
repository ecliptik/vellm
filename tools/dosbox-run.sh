#!/usr/bin/env bash
# dosbox-run.sh — run a DOS executable under DOSBox-X and capture its stdout.
#
# Typical headless usage:
#   tools/dosbox-run.sh --exe build/vellm.exe \
#     --args "stories260K.bin -t 0 -s 42 -i Once upon a time" \
#     --stdout /tmp/vellm.out
#
# Interactive (opens the DOSBox-X window):
#   tools/dosbox-run.sh --exe build/hello.exe --interactive
#
# The script:
#   1. Stages a temp dir containing the exe plus every --include file (cwsdpmi.exe,
#      model checkpoints, etc.), drops a RUN.BAT that invokes the exe and redirects
#      stdout to STDOUT.TXT, then launches DOSBox-X with the vellm config.
#   2. On exit, copies STDOUT.TXT to the path given by --stdout (default: print to
#      the real host stdout).
#
# Headless mode relies on `dosbox-x -silent -exit`, which runs AUTOEXEC.BAT and
# quits. We drive AUTOEXEC.BAT via repeated `-c` flags so the config file stays
# invocation-agnostic.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONF="${CONF:-$SCRIPT_DIR/dosbox-x.conf}"

# Defaults -------------------------------------------------------------------
EXE=""
ARGS=""
STDOUT_PATH=""
INTERACTIVE=0
INCLUDES=()
KEEP_STAGE=0
CWSDPMI="${CWSDPMI:-$SCRIPT_DIR/../vendor/cwsdpmi/cwsdpmi.exe}"

usage() {
  cat <<'USAGE'
Usage: dosbox-run.sh --exe PATH [--args "..."] [--stdout PATH] [--include PATH]...
                     [--interactive] [--keep-stage]

  --exe PATH         DOS executable to run (required). Basename is placed at C:\.
  --args "..."       Arguments passed to the exe (quoted as a single string).
  --stdout PATH      Where to copy captured stdout. Default: stream to host stdout.
  --include PATH     Additional file to stage into C:\ (repeatable). If CWSDPMI
                     is present at vendor/cwsdpmi/cwsdpmi.exe it is auto-included.
  --interactive      Open the DOSBox-X window instead of running headless.
  --keep-stage       Leave the staging temp dir on exit (useful for debugging).
  -h, --help         Show this help.
USAGE
}

# Parse ---------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
  case "$1" in
    --exe)         EXE="$2"; shift 2 ;;
    --args)        ARGS="$2"; shift 2 ;;
    --stdout)      STDOUT_PATH="$2"; shift 2 ;;
    --include)     INCLUDES+=("$2"); shift 2 ;;
    --interactive) INTERACTIVE=1; shift ;;
    --keep-stage)  KEEP_STAGE=1; shift ;;
    -h|--help)     usage; exit 0 ;;
    *) echo "dosbox-run.sh: unknown arg: $1" >&2; usage; exit 2 ;;
  esac
done

if [[ -z "$EXE" ]]; then
  echo "dosbox-run.sh: --exe is required" >&2
  usage; exit 2
fi
if [[ ! -f "$EXE" ]]; then
  echo "dosbox-run.sh: exe not found: $EXE" >&2
  exit 2
fi
if [[ ! -f "$CONF" ]]; then
  echo "dosbox-run.sh: conf not found: $CONF" >&2
  exit 2
fi

# Stage ---------------------------------------------------------------------
STAGE="$(mktemp -d -t vellm-dosbox.XXXXXX)"
cleanup() {
  if [[ "$KEEP_STAGE" == "1" ]]; then
    echo "dosbox-run.sh: stage kept at $STAGE" >&2
  else
    rm -rf "$STAGE"
  fi
}
trap cleanup EXIT

cp "$EXE" "$STAGE/"
EXE_BASENAME="$(basename "$EXE")"
EXE_DOSNAME="${EXE_BASENAME^^}"

# Auto-include cwsdpmi.exe if vendored (noisy-fail is fine — CWSDPMI is only
# needed once a real DJGPP-built exe runs; the hello-world stage doesn't need it).
if [[ -f "$CWSDPMI" && ! " ${INCLUDES[*]:-} " =~ [[:space:]]"$CWSDPMI"[[:space:]] ]]; then
  INCLUDES+=("$CWSDPMI")
fi

for f in "${INCLUDES[@]:-}"; do
  [[ -z "$f" ]] && continue
  if [[ ! -f "$f" ]]; then
    echo "dosbox-run.sh: --include not found: $f" >&2
    exit 2
  fi
  cp "$f" "$STAGE/"
done

# RUN.BAT — invokes the exe and captures stdout to STDOUT.TXT.
# DOS CRLF is harmless; writing LF is also fine for COMMAND.COM.
#
# Note: COMMAND.COM does not honor stdout redirection for CALLed batch files
# in DOSBox-X (known DOS 6.22 behavior). For .BAT targets we spawn a sub-shell
# via `COMMAND /C` — that one DOES capture stdout. .EXE/.COM can redirect
# directly.
case "${EXE_DOSNAME##*.}" in
  BAT|bat)
    INVOKE_LINE="$(printf 'COMMAND /C %s %s > STDOUT.TXT\r\n' "$EXE_DOSNAME" "$ARGS")"
    ;;
  *)
    INVOKE_LINE="$(printf '%s %s > STDOUT.TXT\r\n' "$EXE_DOSNAME" "$ARGS")"
    ;;
esac
{
  printf '@ECHO OFF\r\n'
  printf '%s' "$INVOKE_LINE"
} > "$STAGE/RUN.BAT"

# Invoke DOSBox-X -----------------------------------------------------------
# Drive AUTOEXEC via -c flags so the conf stays generic.
DBX_ARGS=(-conf "$CONF" -nopromptfolder
          -c "MOUNT C $STAGE"
          -c "C:"
          -c "CALL RUN.BAT")

if [[ "$INTERACTIVE" == "1" ]]; then
  dosbox-x "${DBX_ARGS[@]}"
else
  DBX_ARGS+=(-c "EXIT" -silent -exit -nogui -nomenu)
  # -silent suppresses startup banner and enforces exit after AUTOEXEC.
  dosbox-x "${DBX_ARGS[@]}" >/dev/null 2>&1 || {
    rc=$?
    echo "dosbox-run.sh: dosbox-x exited non-zero ($rc)" >&2
    exit $rc
  }
fi

# Deliver captured stdout ---------------------------------------------------
OUT="$STAGE/STDOUT.TXT"
if [[ -f "$OUT" ]]; then
  if [[ -n "$STDOUT_PATH" ]]; then
    cp "$OUT" "$STDOUT_PATH"
  else
    cat "$OUT"
  fi
else
  echo "dosbox-run.sh: no STDOUT.TXT produced (exe may have crashed under DPMI)" >&2
  exit 3
fi
