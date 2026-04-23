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

CFLAGS   := -march=pentium -O2 -fomit-frame-pointer -Wall -Isrc
LDFLAGS  :=

# Phase 1 post-link step: bump the DPMI min stack from 256K to 2048K. The
# default is way too small for llama2-scale activations on DPMI.
MINSTACK := 2048k

# --- Sources ------------------------------------------------------------------

SRC_COMMON := \
	src/matmul.c \
	src/quant.c  \
	src/timing.c \
	src/tokenizer.c

SRC_MAIN  := src/vellm.c
OBJ_MAIN  := $(SRC_MAIN:.c=.o) $(SRC_COMMON:.c=.o)

# --- Targets ------------------------------------------------------------------

# Phase 0: the default target is the smoke-test `hello.exe` until Phase 1
# fills in vellm.c. Once Phase 1 lands, flip `all` to depend on vellm.exe.
.PHONY: all
all: hello.exe

.PHONY: vellm
vellm: vellm.exe

vellm.exe: $(OBJ_MAIN)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lm
	$(STUBEDIT) $@ minstack=$(MINSTACK)

# Pattern rule for .c -> .o under DJGPP.
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# --- Phase 0 smoke test -------------------------------------------------------

.PHONY: hello
hello: hello.exe

hello.exe: tests/smoketest/hello.c
	$(CC) $(CFLAGS) -o $@ $<
	$(STUBEDIT) $@ minstack=256k

# --- Install / dist -----------------------------------------------------------

# `make install CF=/mnt/cf` copies the minimum runtime payload onto a
# mounted CF card. We do NOT expect this Makefile to mount anything; that's
# the operator's job.
CF ?=

.PHONY: install
install:
ifeq ($(strip $(CF)),)
	@echo "error: set CF=/path/to/cf/mount (e.g. make install CF=/mnt/cf)" >&2; exit 1
else
	@test -d "$(CF)" || (echo "error: CF=$(CF) is not a directory" >&2; exit 1)
	install -m 0644 vellm.exe               "$(CF)/VELLM.EXE"
	install -m 0644 vendor/cwsdpmi/cwsdpmi.exe "$(CF)/CWSDPMI.EXE"
	@if ls models/*.bin >/dev/null 2>&1; then \
		for f in models/*.bin; do \
			install -m 0644 "$$f" "$(CF)/$$(basename $$f | tr a-z A-Z)"; \
		done; \
	else \
		echo "warning: no models/*.bin to install"; \
	fi
	@echo '@echo off'            >  "$(CF)/RUN.BAT"
	@echo 'set DPMI=CWSDPMI.EXE' >> "$(CF)/RUN.BAT"
	@echo 'VELLM.EXE %*'         >> "$(CF)/RUN.BAT"
	@echo "installed vellm payload to $(CF)"
endif

.PHONY: dist
dist: vellm.exe
	@mkdir -p dist
	@rm -f dist/vellm.zip
	@tmp=$$(mktemp -d) && \
		cp vellm.exe                "$$tmp/VELLM.EXE" && \
		cp vendor/cwsdpmi/cwsdpmi.exe "$$tmp/CWSDPMI.EXE" && \
		printf '@echo off\nset DPMI=CWSDPMI.EXE\nVELLM.EXE %%*\n' > "$$tmp/RUN.BAT" && \
		(cd "$$tmp" && zip -q -r "$(CURDIR)/dist/vellm.zip" .) && \
		rm -rf "$$tmp" && \
		echo "dist/vellm.zip"

# --- Housekeeping -------------------------------------------------------------

.PHONY: clean
clean:
	rm -f src/*.o tests/smoketest/*.o
	rm -f vellm.exe hello.exe
	rm -rf build dist
