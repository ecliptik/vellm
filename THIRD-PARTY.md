# Third-Party Components

vellm incorporates, bundles, or depends on several third-party components. This document lists each, its license, how it is used, and how we comply with its terms. Upstream license texts are preserved in their respective vendored locations.

vellm itself is licensed under the MIT License (see `LICENSE`).

## Summary

| Component | Version | License | Shipped in release | Vendored in repo |
|---|---|---|---|---|
| karpathy/llama2.c | pinned SHA `350e04fe` | MIT | Derived code in `vellm.exe` | `vendor/llama2.c/` |
| CWSDPMI | r7 | Sandmann redistribution terms | Yes (`cwsdpmi.exe`) | `vendor/cwsdpmi/` |
| DJGPP libc + stub | toolchain-provided | permissive (see below) | Static-linked / embedded in `vellm.exe` | — (external toolchain) |
| GCC runtime | 12.x | GPL + Runtime Library Exception | Compiler-inserted code in `vellm.exe` | — |
| TinyStories weights | upstream release | MIT via llama2.c | No — user fetches on install | — |
| DOSBox-X | system | GPL | No (test-only) | — |

## Per-component detail

### karpathy/llama2.c

- **License:** MIT
- **Upstream:** <https://github.com/karpathy/llama2.c>
- **Pinned SHA:** `350e04fe35433e6d2941dce5a1f53308f87058eb` (recorded in `vendor/llama2.c/UPSTREAM_SHA`)
- **Use:** `src/vellm.c` is a derivative work of upstream `runq.c`. `src/tokenizer.c` derives from upstream's tokenizer code. `tools/export.py` (Phase 6) is a copy of upstream's `export.py`.
- **Preserved:** `vendor/llama2.c/LICENSE` ships unchanged.
- **Attribution:** `src/vellm.c` file header carries both upstream copyright (Andrej Karpathy) and our modifications copyright (Micheal Waltz).
- **Port audit trail:** Every DOS-specific deviation from upstream `runq.c` is annotated with `// DOS-PORT:` in `src/vellm.c`. The full port diff is enumerable via `grep 'DOS-PORT:' src/vellm.c`.

### CWSDPMI r7

- **License:** Charles W Sandmann's permissive redistribution terms (see `vendor/cwsdpmi/cwsdpmi.doc`)
- **Upstream:** distributed via the DJGPP archives, e.g. <http://www.delorie.com/pub/djgpp/current/v2misc/csdpmi7b.zip>
- **Author:** Charles W Sandmann
- **Use:** Shipped as `cwsdpmi.exe` alongside every `vellm.exe` release. Provides the DPMI host required to run DJGPP-compiled programs on real-mode DOS 6.22.
- **Compliance:** Binary is redistributed unmodified per Sandmann's terms. `cwsdpmi.doc` (which contains the license and readme) ships in every release zip and in every CF install alongside the binary.
- **Preserved:** `vendor/cwsdpmi/cwsdpmi.doc` (upstream readme/license), `vendor/cwsdpmi/README.md` (our provenance note).

### DJGPP toolchain (libc and binary stub)

Compiled `vellm.exe` contains code drawn from:

- The **DJGPP libc**, statically linked. It is a collection of permissively-licensed contributions maintained by DJ Delorie and the DJGPP community. The DJGPP distribution is treated as free for use and redistribution without copyleft obligations on compiled binaries. See <https://www.delorie.com/djgpp/> for maintainer license statements.
- The **DJGPP stub**, a small real-mode DOS executable that DJGPP prepends to every compiled binary to bootstrap CWSDPMI. Permissive terms set by DJ Delorie.

Neither imposes copyleft on the compiled binary.

### GCC runtime library

Compiler-inserted support code in `vellm.exe` is covered by the **GCC Runtime Library Exception** (<https://www.gnu.org/licenses/gcc-exception-3.1.html>), which explicitly permits the resulting binary to be distributed under any license compatible with the rest of the program's terms, including MIT.

### TinyStories model weights (`stories15M_q80.bin`, `tokenizer.bin`)

- **License:** MIT, inherited from the karpathy/llama2.c repository which distributes the weights as release assets
- **Upstream:** <https://github.com/karpathy/llama2.c> releases; or <https://huggingface.co/karpathy/tinyllamas>
- **Use:** Required to run inference. Fetched by the end-user via `tools/fetch-model.sh`. **Not bundled in vellm release zips.**
- **Compliance:** User fetches directly from upstream, preserving provenance. vellm does not re-host or repackage the weights.
- **Training provenance (informational):** Weights were trained by Andrej Karpathy on the TinyStories dataset (Eldan & Li, 2023, Microsoft Research; CDLA-Sharing-1.0). vellm does not redistribute the dataset or the training pipeline; a domain-training pipeline of our own is planned as Phase 6.

### DOSBox-X (build/test dependency only)

- **License:** GPL
- **Use:** Host-side emulator invoked by `tests/run-golden.sh` for pre-hardware correctness testing. Not bundled, not shipped, not linked. No impact on vellm's license.

### Phase 6 dependencies (host-side training, not bundled)

The Phase 6 domain-fine-tuning pipeline (`docs/fine-tune.md`) uses host-side tools that are never shipped as part of vellm:

| Tool | License | Role |
|---|---|---|
| PyTorch | BSD | Training backend |
| intel-extension-for-pytorch (IPEX) | Apache 2.0 | CPU training optimization |
| SentencePiece | Apache 2.0 | BPE tokenizer training |

**Training corpora for Phase 6** carry their own considerations and are evaluated per-title when Phase 6 begins. In broad strokes:

- **Ralf Brown's Interrupt List (RBIL)** — freely redistributable for non-commercial use; safe to train on with attribution, do not repackage.
- **FreeDOS documentation** — GPL-compatible; safe.
- **textfiles.com archives** — mixed; review per-file permission notes.
- **USENET archives** — individual author copyrights. Training sits in the same fair-use posture as larger foundation-model training pipelines; mentioned in the per-model NOTICE if released.
- **Period books in text form** — copyright status verified per title before inclusion; prefer out-of-copyright or explicitly-released works.

Any Phase 6 model released publicly will carry its own NOTICE documenting the exact training corpus and its license.

## Release contents and compliance

A vellm release zip contains:

```
vellm.exe                  # our MIT
cwsdpmi.exe                # Sandmann CWSDPMI, redistributed per his terms
cwsdpmi.doc                # Sandmann license/readme (required alongside exe)
LICENSE                    # vellm MIT
THIRD-PARTY.md             # this file
README.md                  # vellm readme
RUN.BAT                    # our MIT
fetch-model.sh             # our MIT; fetches weights from upstream on install
vendor/llama2.c/LICENSE    # upstream MIT, preserved for reference
```

Model weights are **not** bundled. `fetch-model.sh` downloads `stories15M_q80.bin` and `tokenizer.bin` directly from upstream on first install, preserving provenance and keeping the release artifact small.

## Questions

License questions or attribution concerns: open an issue at the vellm repository.
