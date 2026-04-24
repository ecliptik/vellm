# Phase 4 Notes — Benchmark harness + CF deployment package

Phase 4 delivered two pieces of infrastructure that don't touch the
compute path:

- **Task #1** (instr): `--benchmark` / `-B` flag on `vellm.exe`. Emits
  a machine-parseable `--- VELLM BENCHMARK ---` block with CPUID
  brand, wall-time, per-phase tok/s, and peak DPMI memory.
- **Tasks #2, #3, #4** (packager): host-side bench harness
  (`bench/run.sh`), DOS-side runners (`BENCH.BAT`, `BENCH42.BAT`),
  `make cf-package` target producing `dist/vellm-cf.tar.gz` and
  `dist/vellm-cf.zip`, and a seeded `bench/results.md` with DOSBox-X
  + host Linux reference rows and a reserved row for real PODP5V83.

No compute-path changes in Phase 4. The Phase 1 primary 192-byte
byte-identical gate still passes. The Phase 3 tolerance-fallback
gate remains in the tree as a safety net; Phase 4 did not exercise
it.

## Benchmark output format

Fixed, grepped against by `bench/run.sh`. Any future change to the
field names must be coordinated with the harness — the parser is
tolerant of missing/extra fields (they become `?`), but the marker
lines and field-name spellings are the contract.

```
--- VELLM BENCHMARK ---
cpu        : <CPUID brand string, or "unknown (no CPUID)">
model      : <checkpoint basename>
ckpt bytes : <file size in bytes>
tokens     : <total = prompt + gen>
prompt tok : <prompt tokens>
gen tok    : <generated tokens>
wall ms    : <generation wall time, ms; excludes model-load I/O>
prompt tok/s: <float>
gen tok/s  : <float>
peak mem   : <high-water DPMI demand, bytes>
--- END ---
```

Fixed by task #1 to use the exact canonical prompt `"The old
computer hummed to life"`, seed 42, temp 0, 200 tokens. `--benchmark`
overrides any conflicting user args so the benchmark is
bit-reproducible across invocations (modulo CPUID-vs-CPUID on
different hardware and DOSBox-X-vs-real-hardware timing).

## Task #2 — `bench/run.sh`

Host-side wrapper around `tools/dosbox-run.sh`. Runs the
`vellm.exe --benchmark` invocation under DOSBox-X for each scenario
in the test matrix, parses the benchmark block, emits rows in
markdown / CSV / TSV.

### Scenarios

| Name | Model | Extra args |
|---|---|---|
| `15m-default` | `stories15M_q80.bin` | (none) |
| `15m-seq256`  | `stories15M_q80.bin` | `--max-seq-len 256` |
| `42m-seq256`  | `stories42M_q80.bin` | `--max-seq-len 256` |

`42m-seq256` is gracefully skipped if `models/stories42M_q80.bin` is
absent; the run still succeeds with 2/3 scenarios. This matches the
CF-package 15M-only fallback so deploys without the 42M model are
consistent end-to-end.

### Parser tolerance

- Missing fields → `?` in the row, not a crash.
- Extra fields not in the known set → silently ignored.
- No `--- VELLM BENCHMARK ---` fencepost → all fields are `?` and a
  warning prints the first 20 lines of captured stdout so the
  operator can diagnose.
- Values containing `:` (e.g., CPU brand strings like
  `Intel(R) Pentium(R) CPU`) split only on the first colon so the
  value stays intact.

### Known `cpu` values encountered

- Under DOSBox-X `cputype = pentium`: `GenuineIntel family 5 model 1
  stepping 7`. The vendor+family+model+stepping fallback, not a brand
  string, because DOSBox-X doesn't implement CPUID leaf 0x80000002-04
  extended-brand for its emulated Pentium.
- Under native Linux (x86_64 host running the non-DJGPP build):
  `unknown (no CPUID)`. The EFLAGS-bit-21 probe is `__i386__`-gated,
  so non-i386 host builds emit a clean compile but the literal
  "unknown" string. If you see this in a DOS context, something is
  very wrong — it means `vellm.exe` was cross-built against a
  non-DJGPP target.
- On real PODP5V83 (expected): `GenuineIntel family 5 model 3
  stepping N`. The PODP5V83 uses family 5 / model 3 (Pentium
  OverDrive with P54CS core); DOSBox-X reports model 1. **This
  difference is a feature, not a bug** — it's the one cheap way to
  tell a DOSBox-X measurement from a real-hardware measurement when
  you're reading `bench/results.md` cold.

## Task #3 — `make cf-package`

Richer deploy bundle than `make dist`:

- Produces **both** `dist/vellm-cf.zip` (Windows-mount-and-copy) and
  `dist/vellm-cf.tar.gz` (Linux scp) with identical contents.
- Adds DOS-formatted `LICENSE.TXT` (the MIT license from repo root,
  CRLF'd) and `README.TXT` (generated from an inline template, CRLF,
  ≤80 cols, 7-bit ASCII only).
- Adds the Phase 4 benchmark runners: `BENCH.BAT` (15M) and, when
  `models/stories42M_q80.bin` is present, `BENCH42.BAT` (42M at
  `--max-seq-len 256`) + the 42M checkpoint itself.
- Size-budget checks at the end: warns (doesn't fail) if zip or
  tar.gz exceeds 12 MB (15M-only) or 50 MB (15M+42M).

### Measured sizes

| Config | `.zip` | `.tar.gz` |
|---|---:|---:|
| 15M only | 11,494,917 B (10.96 MiB) | 11,496,232 B (10.96 MiB) |
| 15M + 42M | 44,833,332 B (42.76 MiB) | 44,836,267 B (42.76 MiB) |

Both well under budget; tar.gz is marginally larger than zip because
tar adds a `.` root entry and the 42M model doesn't compress further
than the Q8_0 quantized bytes already are. Neither format offers
much compression on the model data (~95% random-looking int8), so
the two archive sizes track file-size closely.

### README.TXT template

Inline in the `Makefile` as a `define` block — keeps the package
invariant (README text ↔ package contents) in one place. Two splice
markers (`@42M_RUN@`, `@42M_LIST@`) conditionally inject the 42M
how-to and listing lines when 42M is present; `@REPO_URL@` is
substituted from `git remote get-url origin`.

### Failure modes deliberately surfaced

The target fails fast with a specific error message if any of:

- `vellm.exe` missing (run `make` first)
- `vendor/cwsdpmi/cwsdpmi.exe` or `cwsdpmi.doc` missing
- `models/stories15M_q80.bin` missing
- `models/tokenizer.bin` missing
- `bench/BENCH.BAT` or `bench/BENCH42.BAT` missing (task #2 not
  applied?)
- `LICENSE` file missing from repo root

## Task #4 — `bench/results.md` seeding

Three kinds of rows:

- **DOSBox-X rows** — measured by running `bench/run.sh` after
  `make`. Reproducible, noise-free, one-shot.
- **Host Linux rows** — measured by running `./run_host` (native
  build of upstream `vendor/llama2.c/runq.c` at the pinned
  UPSTREAM_SHA). Upstream emits `achieved tok/s: N.NN` on stderr;
  that's the sole number captured. Pre-`--benchmark` format, so
  wall time is measured externally. Host numbers are a "compute
  ceiling" anchor, not a direct comparison to vellm.
- **Real PODP5V83 row** — **reserved**, not fabricated. Filled in
  after the operator deploys `dist/vellm-cf.tar.gz` to the target
  and runs `BENCH.BAT` / `BENCH42.BAT`.

### DOSBox-X measurement caveat (inherited from Phase 3)

`cycles = fixed 90000` is a throttling setting, not a cycle-accurate
P5 simulator. Optimizations that replace a rare-but-slow P5
instruction with a fast one (e.g. Phase 3 Experiment B's IDIV→SAR)
look near-null under DOSBox-X but pay off massively on real
hardware. Real PODP5V83 tok/s is expected to **exceed** DOSBox-X
tok/s for the optimized paths, not fall short. See
`docs/phase3-notes.md` §"Experiment B" for the detailed analysis.

## Phase 4 carryover

- **Real PODP5V83 measurements.** The single most-interesting row in
  `bench/results.md` is still a TBD. Once the operator runs the
  deploy artifact, paste the `--- VELLM BENCHMARK ---` block into
  that row and note the CWSDPMI.SWP filesize sampled mid-run.
- **486DX2/66 stretch benchmark.** PLAN.md Phase 5 stretch goal.
  Probably needs a smaller `stories260K.bin` to fit the weaker
  hardware's memory and to produce a number in reasonable wall-clock
  time.
- **Int-MAC attention inner loops.** Deferred from Phase 3, not
  attempted in Phase 4. Would likely recoup the ~18.5s task-#4
  int8-KV-cache regression documented in `docs/phase3-notes.md`.
- **v0.1 release tag.** Phase 4 closes the pre-v0.1 punch list from
  `PLAN.md` § Phase 5. Tag `v0.1` once real-HW numbers land in
  `bench/results.md` and there's a confirmed boots-and-runs photo /
  screenshot of PODP5V83.
