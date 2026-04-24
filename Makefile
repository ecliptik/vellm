# vellm — cross-DJGPP build for MS-DOS 6.22 / Pentium Overdrive 83.
#
# This is the primary Makefile. It builds `vellm.exe` (and the Phase 0
# `hello.exe` smoke test) using the DJGPP cross-compiler installed under
# ~/emulators/tools/djgpp/ by tools/build-djgpp.sh.
#
# See Makefile.host for the native-Linux reference build.

# --- Toolchain ----------------------------------------------------------------

# Prepend the DJGPP cross toolchain to PATH so that `make` works regardless
# of shell state. This mirrors the retro68-build pattern from our Mac retro
# pipeline.
#
# Two dirs are needed: the main bin/ (cross-gcc + friends) and
# i586-pc-msdosdjgpp/bin/ (target-side utilities like stubedit, stubify, exe2coff).
DJGPP_ROOT := $(HOME)/emulators/tools/djgpp
export PATH := $(DJGPP_ROOT)/bin:$(DJGPP_ROOT)/i586-pc-msdosdjgpp/bin:$(PATH)

CC       := i586-pc-msdosdjgpp-gcc
STUBEDIT := stubedit

# Phase 3 flags — aggressive optimizations enabled behind the tolerance
# gate. Measured winners (stories15M_q80 -n 200, DOSBox-X cycles=fixed
# 90000, memsize=48); see docs/phase3-notes.md §"Task #3 Experiment A":
#
#   pre-Phase-3 (-O2 -ffloat-store):              5m45s
#   + -funroll-loops:                              3m55s  (-32%)
#   + -ffast-math:                                 3m49s
#   + drop -ffloat-store (this configuration):     2m43s  (-53% / 2.11x)
#
# All three still pass the Phase 1 primary byte-identical gate (first
# 192 bytes of -n 200 match tests/golden/once_upon_a_time.txt exactly).
# The Phase 3 tolerance gate is the safety net, not the fallback.
#
# Why dropping -ffloat-store is safe here: -ffloat-store forced every
# x87 intermediate to round-trip through memory as 32-bit float (added
# in Phase 1 to reconcile x87 with the SSE2-generated golden). With
# -ffast-math on, gcc is already free to contract/reassoc fp chains in
# ways that diverge sub-ULP from strict SSE2; keeping -ffloat-store on
# top of that was belt-and-suspenders and the belt cost ~1m of wall
# time per -n 200 run (the spill/reload on every fp op is enormous on
# the short P5 pipeline). Empirically, -ffast-math WITHOUT -ffloat-store
# still produces a 192-byte-identical prefix on the canonical prompt —
# the first ~30 tokens remain the stable cross-toolchain fingerprint,
# per docs/phase1-notes.md §"FP precision mismatch".
#
# -funroll-loops unrolls the matmul inner loop (src/vellm.c matmul()
# GS-sized block). On the P5 this is the single largest lever — it
# collapses the tight `for (k=0; k<GS; k++)` integer-mac chain into a
# straight sequence of pair-issuable MOV/IMUL/ADD triples.
#
# -O3 adds nothing on top of -O2 -funroll-loops on this workload and
# regresses slightly when -fschedule-insns{,2} get pulled in with it.
# Keeping -O2 as the optimization baseline.
CFLAGS   := -march=pentium -O2 -fomit-frame-pointer -funroll-loops -ffast-math -Wall -Isrc
LDFLAGS  :=
LDLIBS   := -lm

# Phase 1 post-link step: bump the DPMI min stack from 256K to 2048K. The
# default is way too small for llama2-scale activations on DPMI.
MINSTACK := 2048k

# --- Sources ------------------------------------------------------------------

# vellm.c is a self-contained fork of vendor/llama2.c/runq.c with minimum
# DOS-required deltas. It does NOT link against the Phase-0 scaffolding
# (src/matmul.c, src/timing.c, src/tokenizer.c) — those get subsumed/replaced
# in later phases; the Phase 1 port stays single-TU so the
# `grep 'DOS-PORT:' src/vellm.c` audit is complete.
# (src/quant.{c,h} were deleted in Phase 2 — runq.c's Q8_0 format is native;
# vellm never needs its own quant path.)
SRC_MAIN  := src/vellm.c

# --- Targets ------------------------------------------------------------------

# Default target: the real Phase 1 artifact. `make hello` is still wired up
# for the Phase 0 smoke test.
.PHONY: all
all: vellm.exe

.PHONY: vellm
vellm: vellm.exe

vellm.exe: $(SRC_MAIN)
	@test -s $< || (echo "error: $< is empty — see task #4 (fork runq.c)" >&2; exit 1)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)
	$(STUBEDIT) $@ minstack=$(MINSTACK)

# Pattern rule for .c -> .o under DJGPP (kept for future multi-TU work).
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# --- Phase 0 smoke test -------------------------------------------------------

.PHONY: hello
hello: hello.exe

hello.exe: tests/smoketest/hello.c
	$(CC) $(CFLAGS) -o $@ $<
	$(STUBEDIT) $@ minstack=256k

# --- Install / dist -----------------------------------------------------------
#
# `make install CF=/mnt/cf` copies the Phase 1 runtime payload onto a
# mounted CF card. We do NOT expect this Makefile to mount anything; that's
# the operator's job. The payload is:
#
#   VELLM.EXE        — the DJGPP-built inference binary
#   CWSDPMI.EXE      — required DPMI host; must ship alongside vellm.exe
#   CWSDPMI.DOC      — cwsdpmi license/readme (THIRD-PARTY.md requirement)
#   STORIES15M_Q80.BIN — TinyStories Q8_0 checkpoint (~15 MB, from karpathy/llama2.c release)
#   TOKENIZER.BIN    — 32K-vocab SentencePiece tokenizer
#   RUN.BAT          — canned invocation with the Phase 1 golden-test args

CF ?=

MODEL_BIN := models/stories15M_q80.bin
TOKENIZER_BIN := models/tokenizer.bin
CWSDPMI_EXE := vendor/cwsdpmi/cwsdpmi.exe
CWSDPMI_DOC := vendor/cwsdpmi/cwsdpmi.doc

# Canonical Phase 1 invocation. Flag names track runq.c argv verbatim
# (lowercase: -z tokenizer, -t temp, -s seed, -n steps, -i prompt).
# DOS COMMAND.COM passes argv through literally so lowercase works.
RUN_BAT_CMD := VELLM.EXE STORIES15M_Q80.BIN -z TOKENIZER.BIN -t 0 -s 42 -n 200 -i "Once upon a time"

.PHONY: install
install: vellm.exe
ifeq ($(strip $(CF)),)
	@echo "error: set CF=/path/to/cf/mount (e.g. make install CF=/mnt/cf)" >&2; exit 1
else
	@test -d "$(CF)" || (echo "error: CF=$(CF) is not a directory" >&2; exit 1)
	@test -f "$(CWSDPMI_EXE)" || (echo "error: $(CWSDPMI_EXE) missing" >&2; exit 1)
	@test -f "$(CWSDPMI_DOC)" || (echo "error: $(CWSDPMI_DOC) missing" >&2; exit 1)
	@test -f "$(MODEL_BIN)"   || (echo "error: $(MODEL_BIN) missing — download from karpathy/llama2.c release" >&2; exit 1)
	@test -f "$(TOKENIZER_BIN)" || (echo "error: $(TOKENIZER_BIN) missing — download from karpathy/llama2.c release" >&2; exit 1)
	install -m 0644 vellm.exe        "$(CF)/VELLM.EXE"
	install -m 0644 $(CWSDPMI_EXE)   "$(CF)/CWSDPMI.EXE"
	install -m 0644 $(CWSDPMI_DOC)   "$(CF)/CWSDPMI.DOC"
	install -m 0644 $(MODEL_BIN)     "$(CF)/STORIES15M_Q80.BIN"
	install -m 0644 $(TOKENIZER_BIN) "$(CF)/TOKENIZER.BIN"
	@printf '@echo off\r\nset DPMI=CWSDPMI.EXE\r\n%s\r\n' '$(RUN_BAT_CMD)' > "$(CF)/RUN.BAT"
	@echo "installed vellm payload to $(CF)"
endif

.PHONY: dist
dist: vellm.exe
	@test -f "$(CWSDPMI_EXE)" || (echo "error: $(CWSDPMI_EXE) missing" >&2; exit 1)
	@test -f "$(CWSDPMI_DOC)" || (echo "error: $(CWSDPMI_DOC) missing" >&2; exit 1)
	@test -f "$(MODEL_BIN)"   || (echo "error: $(MODEL_BIN) missing — download from karpathy/llama2.c release" >&2; exit 1)
	@test -f "$(TOKENIZER_BIN)" || (echo "error: $(TOKENIZER_BIN) missing — download from karpathy/llama2.c release" >&2; exit 1)
	@mkdir -p dist
	@rm -f dist/vellm.zip
	@tmp=$$(mktemp -d) && \
		cp vellm.exe            "$$tmp/VELLM.EXE" && \
		cp $(CWSDPMI_EXE)       "$$tmp/CWSDPMI.EXE" && \
		cp $(CWSDPMI_DOC)       "$$tmp/CWSDPMI.DOC" && \
		cp $(MODEL_BIN)         "$$tmp/STORIES15M_Q80.BIN" && \
		cp $(TOKENIZER_BIN)     "$$tmp/TOKENIZER.BIN" && \
		printf '@echo off\r\nset DPMI=CWSDPMI.EXE\r\n%s\r\n' '$(RUN_BAT_CMD)' > "$$tmp/RUN.BAT" && \
		(cd "$$tmp" && zip -q -r "$(CURDIR)/dist/vellm.zip" .) && \
		rm -rf "$$tmp" && \
		echo "dist/vellm.zip"

# --- Housekeeping -------------------------------------------------------------

.PHONY: clean
clean:
	rm -f src/*.o tests/smoketest/*.o
	rm -f vellm.exe hello.exe
	rm -f run_host
	rm -rf build dist
