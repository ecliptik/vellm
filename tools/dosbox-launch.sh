#!/usr/bin/env bash
# dosbox-launch.sh — launch DOSBox-X visible in the X session for manual testing,
# screenshot capture, and xdotool automation.
#
# Unlike dosbox-run.sh (which stages one exe and exits), this launcher mounts
# the repo and vendored CWSDPMI and leaves DOSBox-X running so you can drive it
# by hand or by script.
#
# Usage:
#   tools/dosbox-launch.sh                 # open DOSBox-X at C:\
#   tools/dosbox-launch.sh --kill-first    # kill any running instance first
#   tools/dosbox-launch.sh --exe PATH      # auto-run an .exe on launch
#
# After launch:
#   scrot -u /tmp/dosbox.png                   capture focused window
#   xdotool search --name DOSBox windowactivate --sync  focus the window
#   xdotool type --delay 40 'HELLO.EXE\r'      send keystrokes
#   pkill -x dosbox-x                          stop it
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CONF="$SCRIPT_DIR/dosbox-x.conf"

KILL_FIRST=0
EXE=""

usage() {
  cat <<'USAGE'
Usage: dosbox-launch.sh [--kill-first] [--exe PATH]

  --kill-first, -k   Kill any running dosbox-x process first
  --exe PATH         Path to an .exe to auto-run (relative to repo root)
  -h, --help         Show this help
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --kill-first|-k) KILL_FIRST=1; shift ;;
    --exe)           EXE="$2"; shift 2 ;;
    -h|--help)       usage; exit 0 ;;
    *) echo "dosbox-launch.sh: unknown arg: $1" >&2; usage; exit 2 ;;
  esac
done

if [[ "$KILL_FIRST" == "1" ]] && pgrep -x dosbox-x >/dev/null 2>&1; then
  echo "Stopping running dosbox-x..."
  pkill -x dosbox-x || true
  sleep 1
fi

if pgrep -x dosbox-x >/dev/null 2>&1; then
  echo "dosbox-x is already running. Use --kill-first to restart." >&2
  exit 1
fi

if [[ ! -f "$CONF" ]]; then
  echo "dosbox-launch.sh: conf not found: $CONF" >&2
  exit 2
fi

# Always target the local X session (:0), not any SSH-forwarded DISPLAY the
# caller's shell might have inherited. Override with DOSBOX_DISPLAY=... if
# genuinely needed. Matches the Snow / Basilisk emulator convention.
export DISPLAY="${DOSBOX_DISPLAY:-:0}"

# C: = repo root (browse everything)
# D: = vendor/cwsdpmi  (puts cwsdpmi.exe on PATH so DJGPP binaries find DPMI host)
DBX_ARGS=(-conf "$CONF" -nopromptfolder
          -c "MOUNT C $REPO_ROOT"
          -c "MOUNT D $REPO_ROOT/vendor/cwsdpmi"
          -c 'SET PATH=Z:\;C:\;D:\'
          -c "C:")

if [[ -n "$EXE" ]]; then
  EXE_DOS="$(echo "$EXE" | tr '/' '\\' | tr '[:lower:]' '[:upper:]')"
  DBX_ARGS+=(-c "$EXE_DOS")
fi

echo "Launching DOSBox-X (DISPLAY=$DISPLAY)..."
dosbox-x "${DBX_ARGS[@]}" &
DBX_PID=$!
echo "DOSBox-X running (PID $DBX_PID)."
echo
echo "  screenshot:  scrot -u /tmp/dosbox.png"
echo "  focus:       xdotool search --name DOSBox windowactivate --sync"
echo "  type:        xdotool type --delay 40 'DIR\\r'"
echo "  stop:        pkill -x dosbox-x    (or Ctrl+F9 in the window)"
