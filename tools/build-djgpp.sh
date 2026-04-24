#!/usr/bin/env bash
# build-djgpp.sh — install the DJGPP cross-toolchain vellm needs to produce
# DOS executables from Linux/macOS.
#
# Uses Andrew Wu's build-djgpp scripts (github.com/andrewwutw/build-djgpp)
# to compile GCC + binutils + DJGPP libc into $HOME/emulators/tools/djgpp/
# (override with DJGPP_PREFIX). The Makefile expects the toolchain at that
# default location; if you install elsewhere, edit the Makefile's PATH
# export or symlink into the default path.
#
# Usage: tools/build-djgpp.sh            # build default GCC version (12.2.0)
#        tools/build-djgpp.sh 10.3.0     # build a specific GCC version
#        FORCE=1 tools/build-djgpp.sh    # rebuild even if already installed
#
# Build takes ~30-60 minutes. Requires network access. On Debian/Ubuntu,
# install prerequisites first:
#   sudo apt-get install -y build-essential bison flex texinfo \
#       libgmp-dev libmpfr-dev libmpc-dev wget curl unzip zlib1g-dev patch

set -euo pipefail

GCC_VERSION="${1:-12.2.0}"
PREFIX="${DJGPP_PREFIX:-$HOME/emulators/tools/djgpp}"
GCC_BIN="$PREFIX/bin/i586-pc-msdosdjgpp-gcc"
SCRATCH="${SCRATCH:-/tmp/build-djgpp}"
REPO_URL="https://github.com/andrewwutw/build-djgpp"

log() { printf '[build-djgpp] %s\n' "$*" >&2; }

if [[ -x "$GCC_BIN" && "${FORCE:-0}" != "1" ]]; then
  log "Found existing install: $GCC_BIN"
  log "Version: $("$GCC_BIN" --version | head -n1)"
  log "Skipping rebuild. Set FORCE=1 to force."
  exit 0
fi

log "Installing DJGPP with GCC $GCC_VERSION into $PREFIX"
log "This will take roughly 30-60 minutes."

for cmd in gcc g++ make bison flex curl unzip patch makeinfo; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    log "WARNING: '$cmd' not found in PATH; build may fail. See script header for apt command."
  fi
done

if [[ ! -d "$SCRATCH" ]]; then
  log "Cloning $REPO_URL -> $SCRATCH"
  git clone --depth 1 "$REPO_URL" "$SCRATCH"
else
  log "Reusing existing clone at $SCRATCH (git pull)"
  (cd "$SCRATCH" && git pull --ff-only || true)
fi

mkdir -p "$PREFIX"

log "Running build-djgpp.sh $GCC_VERSION (DJGPP_PREFIX=$PREFIX)"
(cd "$SCRATCH" && DJGPP_PREFIX="$PREFIX" ./build-djgpp.sh "$GCC_VERSION")

if [[ ! -x "$GCC_BIN" ]]; then
  log "ERROR: build finished but $GCC_BIN is missing."
  exit 1
fi

log "Install complete."
log "Version: $("$GCC_BIN" --version | head -n1)"
log "Put $PREFIX/bin on PATH to use 'i586-pc-msdosdjgpp-gcc' directly,"
log "or source $PREFIX/setenv to use the short 'gcc' name."
