#!/usr/bin/env bash
#
# tests/run-golden.sh — byte-identical output check (Phase 1).
#
# Phase 1 implementation (not yet wired up — this is a stub):
#
#   1. Build the DJGPP cross-target vellm.exe via `make`.
#   2. Build the native reference run_host via `make -f Makefile.host`.
#   3. Run run_host with the canonical prompt to produce a reference file:
#        ./run_host models/stories260K.bin -t 0 -s 42 -i "Once upon a time"
#        > tests/golden/once_upon_a_time.txt
#      (The reference file is committed to the repo; this script only
#      regenerates it if --regen-golden is passed.)
#   4. Run vellm.exe headless under DOSBox-X (tools/dosbox-run.sh) with
#      stdout captured to a file on a mounted host directory.
#   5. diff the two files. Exit nonzero on any difference.
#
# Phase 0 exit criteria only requires hello.exe to run under DOSBox-X and
# print "hello from vellm" — see tests/smoketest/. The golden test proper
# lands with the Phase 1 port.

set -euo pipefail

echo "tests/run-golden.sh: not implemented yet (see PLAN.md Phase 1)"
echo "  Phase 0 smoke test lives in tests/smoketest/ + Makefile 'hello' target."
exit 0
