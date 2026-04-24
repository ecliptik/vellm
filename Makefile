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

# --- cf-package (Phase 4) -----------------------------------------------------
#
# `make cf-package` produces a richer deploy bundle than `make dist` —
# same core files plus DOS-formatted LICENSE.TXT + README.TXT and the
# Phase 4 benchmark runners (BENCH.BAT, BENCH42.BAT). Ships BOTH a
# .zip (Windows-mount-and-copy workflow) and a .tar.gz (Linux scp
# workflow) with identical contents.
#
# If models/stories42M_q80.bin is present in the tree at package-build
# time, the 42M model and BENCH42.BAT are included as well. If absent,
# the package still builds (15M only) and README.TXT notes the omission.

MODEL_42M_BIN := models/stories42M_q80.bin

BENCH_BAT      := bench/BENCH.BAT
BENCH42_BAT    := bench/BENCH42.BAT

CF_STAGE       := dist/vellm-cf
CF_ZIP         := dist/vellm-cf.zip
CF_TARGZ       := dist/vellm-cf.tar.gz

# README.TXT template. Rendered with CRLF line endings, under 80 cols,
# 7-bit ASCII only (no em-dashes, no smart quotes). The two `@42M@`
# markers are substituted at package time:
#   - `@42M@`             → either the 42M block or the empty string
#   - `@REPO_URL@`        → from `git remote get-url origin`
#
# Keeping this in the Makefile (rather than a separate file) so the
# package invariant (readme text ↔ package contents) stays in one place.
define CF_README_TEMPLATE
vellm - TinyStories inference on MS-DOS 6.22
============================================

vellm is a port of karpathy/llama2.c's int8-quantized inference
(runq.c) to MS-DOS 6.22 + DJGPP + CWSDPMI, targeting a Pentium
Overdrive 83 MHz (Socket 3, P54C core, no MMX) with 48 MB of RAM.
It runs the TinyStories 15M and 42M Q8_0 checkpoints from upstream.

HOW TO RUN
----------

 1. Boot DOS (MS-DOS 6.22 or compatible).
 2. Change to the directory holding VELLM.EXE:
        C:\>CD \VELLM
 3. Run one of the batch files:

        RUN.BAT         Canonical demo. Seed 42, temp 0, 200 tokens,
                        prompt "Once upon a time".
        BENCH.BAT       Reproducible benchmark on stories15M_q80.
                        Prints a parseable "--- VELLM BENCHMARK ---"
                        block at the end. ~3 min on a Pentium/83.
@42M_RUN@
WHAT YOU SHOULD SEE
-------------------

The canonical demo opens with:

    Once upon a time, there was a little girl named Lily.

If the first sentence matches, the model is loading correctly and
the DPMI host is working. Divergence after ~30 tokens between DOS
and a Linux reference run is expected (floating-point rounding
differences between DJGPP's libm and modern glibc); the first
paragraph is the cross-toolchain correctness fingerprint.

CWSDPMI
-------

CWSDPMI.EXE is the DPMI host that DJGPP-built programs need under
plain DOS. It MUST be in the same directory as VELLM.EXE the first
time you run it; after that CWSDPMI self-installs into XMS. If you
see "No DPMI host available" at startup, check that CWSDPMI.EXE is
alongside VELLM.EXE. License is in CWSDPMI.DOC.

MEMORY
------

Requires approximately 20 MB of free DPMI memory for the 15M model
and 45 MB for 42M with --MAX-SEQ-LEN 256. The 48 MB target machine
has enough headroom for both configurations with no CWSDPMI.SWP
growth. See docs/phase3-notes.md in the repo for the full matrix.

UPDATES
-------

Source and full documentation:
    @REPO_URL@

This package contains:
    VELLM.EXE           inference binary (DJGPP cross-built)
    CWSDPMI.EXE         DPMI host
    CWSDPMI.DOC         CWSDPMI license / readme
    LICENSE.TXT         vellm license (MIT)
    README.TXT          this file
    RUN.BAT             canonical demo runner
    BENCH.BAT           15M benchmark runner
    STORIES15M_Q80.BIN  TinyStories 15M Q8_0 checkpoint
    TOKENIZER.BIN       Llama 32K-vocab tokenizer@42M_LIST@
endef
export CF_README_TEMPLATE

# Snippet spliced at @42M_RUN@ when 42M is included. Leading blank
# line separates it from BENCH.BAT; trailing blank line keeps the
# following section header from running into it.
define CF_README_42M_RUN

        BENCH42.BAT     Same benchmark on stories42M_q80 with
                        --MAX-SEQ-LEN 256 (42M fits under the 48 MB
                        ceiling with no swap in this configuration).
                        ~8 min on a Pentium/83.

endef
export CF_README_42M_RUN

# Snippet spliced at @42M_LIST@ when 42M is included. No leading
# newline (the template splice point is immediately after the
# tokenizer listing line).
define CF_README_42M_LIST

    STORIES42M_Q80.BIN  TinyStories 42M Q8_0 checkpoint (optional)
    BENCH42.BAT         42M benchmark runner (optional)
endef
export CF_README_42M_LIST

# crlf = apply Unix-LF -> DOS-CRLF normalization to stdin, emit to stdout.
# Used for LICENSE.TXT and README.TXT generation. awk is available
# everywhere; `unix2dos` is not.
#
# We strip any existing CR first (idempotent) then add one at EOL.
CRLF := awk 'BEGIN{ORS="\r\n"} {sub(/\r$$/, ""); print}'

.PHONY: cf-package
cf-package: vellm.exe
	@test -f "$(CWSDPMI_EXE)"      || (echo "error: $(CWSDPMI_EXE) missing" >&2; exit 1)
	@test -f "$(CWSDPMI_DOC)"      || (echo "error: $(CWSDPMI_DOC) missing" >&2; exit 1)
	@test -f "$(MODEL_BIN)"        || (echo "error: $(MODEL_BIN) missing — download from karpathy/llama2.c release" >&2; exit 1)
	@test -f "$(TOKENIZER_BIN)"    || (echo "error: $(TOKENIZER_BIN) missing — download from karpathy/llama2.c release" >&2; exit 1)
	@test -f "$(BENCH_BAT)"        || (echo "error: $(BENCH_BAT) missing — Phase 4 task #2 not applied?" >&2; exit 1)
	@test -f "$(BENCH42_BAT)"      || (echo "error: $(BENCH42_BAT) missing — Phase 4 task #2 not applied?" >&2; exit 1)
	@test -f LICENSE               || (echo "error: LICENSE missing" >&2; exit 1)
	@rm -rf "$(CF_STAGE)"
	@rm -f  "$(CF_ZIP)" "$(CF_TARGZ)"
	@mkdir -p "$(CF_STAGE)"
	@install -m 0644 vellm.exe            "$(CF_STAGE)/VELLM.EXE"
	@install -m 0644 $(CWSDPMI_EXE)       "$(CF_STAGE)/CWSDPMI.EXE"
	@install -m 0644 $(CWSDPMI_DOC)       "$(CF_STAGE)/CWSDPMI.DOC"
	@install -m 0644 $(MODEL_BIN)         "$(CF_STAGE)/STORIES15M_Q80.BIN"
	@install -m 0644 $(TOKENIZER_BIN)     "$(CF_STAGE)/TOKENIZER.BIN"
	@$(CRLF) < $(BENCH_BAT) > "$(CF_STAGE)/BENCH.BAT"
	@printf '@echo off\r\nset DPMI=CWSDPMI.EXE\r\n%s\r\n' '$(RUN_BAT_CMD)' > "$(CF_STAGE)/RUN.BAT"
	@$(CRLF) < LICENSE > "$(CF_STAGE)/LICENSE.TXT"
	@# Conditionally include 42M: if the checkpoint is present, copy
	@# it and BENCH42.BAT, and splice the 42M blocks into README.TXT.
	@# When absent, the two markers are replaced with empty strings.
	@url='$(shell git remote get-url origin 2>/dev/null || echo unknown)'; \
	if [ -f "$(MODEL_42M_BIN)" ]; then \
	    install -m 0644 $(MODEL_42M_BIN)   "$(CF_STAGE)/STORIES42M_Q80.BIN"; \
	    $(CRLF) < $(BENCH42_BAT)          > "$(CF_STAGE)/BENCH42.BAT"; \
	    run_blk="$$CF_README_42M_RUN"; \
	    list_blk="$$CF_README_42M_LIST"; \
	    echo "cf-package: 42M included (STORIES42M_Q80.BIN + BENCH42.BAT)"; \
	else \
	    run_blk=""; \
	    list_blk=""; \
	    echo "cf-package: 42M not found at $(MODEL_42M_BIN) — 15M-only package"; \
	fi; \
	printf '%s\n' "$$CF_README_TEMPLATE" | \
	    awk -v blk="$$run_blk"  'BEGIN{RS="@42M_RUN@"}  NR==1{printf "%s", $$0; next} {printf "%s%s", blk, $$0}' | \
	    awk -v blk="$$list_blk" 'BEGIN{RS="@42M_LIST@"} NR==1{printf "%s", $$0; next} {printf "%s%s", blk, $$0}' | \
	    awk -v url="$$url"      '{gsub(/@REPO_URL@/, url); print}' | \
	    $(CRLF) > "$(CF_STAGE)/README.TXT"
	@# Produce both archives from the staged directory. `-C` on tar so
	@# the archive roots at "./" rather than "dist/vellm-cf/" — the
	@# operator extracts into their own named dir.
	@(cd "$(CF_STAGE)" && zip -q -r "$(CURDIR)/$(CF_ZIP)" .)
	@tar -C "$(CF_STAGE)" -czf "$(CF_TARGZ)" .
	@echo "cf-package: built $(CF_ZIP) ($$(stat -c '%s' $(CF_ZIP)) bytes)"
	@echo "cf-package: built $(CF_TARGZ) ($$(stat -c '%s' $(CF_TARGZ)) bytes)"
	@# Size-budget sanity checks. 42M present: 50 MB cap. 15M only: 12 MB cap.
	@if [ -f "$(MODEL_42M_BIN)" ]; then BUDGET=52428800; else BUDGET=12582912; fi; \
	    for a in $(CF_ZIP) $(CF_TARGZ); do \
	        sz=$$(stat -c '%s' "$$a"); \
	        if [ "$$sz" -gt "$$BUDGET" ]; then \
	            echo "cf-package: WARNING $$a is $$sz bytes, over budget $$BUDGET" >&2; \
	        fi; \
	    done

# --- Housekeeping -------------------------------------------------------------

.PHONY: clean
clean:
	rm -f src/*.o tests/smoketest/*.o
	rm -f vellm.exe hello.exe
	rm -f run_host
	rm -rf build dist
