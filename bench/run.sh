#!/usr/bin/env bash
#
# bench/run.sh — host-side benchmark runner for vellm.exe.
#
# Drives `vellm.exe --benchmark` across a standard test matrix under
# DOSBox-X via tools/dosbox-run.sh, parses the machine-readable
# `--- VELLM BENCHMARK ---` block emitted by the exe, and prints one
# row per scenario to stdout or appends to a markdown/CSV file.
#
# Typical usage:
#
#   bench/run.sh                          # run full matrix, print rows
#   bench/run.sh --scenario 15m-default   # just one scenario
#   bench/run.sh --output bench/results.md --append
#   bench/run.sh --dry-run                # show invocations, don't run
#
# Scenarios (name / model / extra args):
#
#   15m-default    stories15M_q80.bin   (upstream default)
#   15m-seq256     stories15M_q80.bin   --max-seq-len 256
#   42m-seq128     stories42M_q80.bin   --max-seq-len 128
#
# The 42M scenario is skipped (with a clear note) when
# models/stories42M_q80.bin is not present.
#
# Output format (the field names match the exe's --- VELLM BENCHMARK ---
# block exactly). Parser is tolerant: missing fields become `?`, extra
# fields are ignored.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$REPO_ROOT"

# --- Defaults ----------------------------------------------------------------

SELECTED_SCENARIO=""
OUTPUT_PATH=""
OUTPUT_MODE="replace"     # replace | append
DRY_RUN=0
FORMAT="md"               # md | csv | tsv — inferred from --output suffix

EXE="vellm.exe"
TOKENIZER="models/tokenizer.bin"
CWSDPMI="vendor/cwsdpmi/cwsdpmi.exe"

# --- Arg parse ---------------------------------------------------------------

usage() {
  cat <<'USAGE'
Usage: bench/run.sh [options]

  --scenario NAME    Run just this scenario (one of: 15m-default, 15m-seq256,
                     42m-seq128). Default: run all available scenarios.
  --output PATH      Append/write rows to PATH. Format inferred from suffix
                     (.md / .csv / .tsv). Default: write to stdout as markdown.
  --append           With --output: append rows; do not overwrite the file.
  --dry-run          Print the DOSBox-X invocations, don't run anything.
  --format FMT       Override output format (md|csv|tsv). Default: inferred.
  -h, --help         This help.

Reads the `--- VELLM BENCHMARK ---` block from vellm.exe's stdout. Missing
fields are reported as `?`; extra fields are ignored.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --scenario) SELECTED_SCENARIO="$2"; shift 2 ;;
    --output)   OUTPUT_PATH="$2"; shift 2 ;;
    --append)   OUTPUT_MODE="append"; shift ;;
    --dry-run)  DRY_RUN=1; shift ;;
    --format)   FORMAT="$2"; shift 2 ;;
    -h|--help)  usage; exit 0 ;;
    *) echo "bench/run.sh: unknown arg: $1" >&2; usage; exit 2 ;;
  esac
done

# Infer format from --output suffix if not explicitly set.
if [[ -n "$OUTPUT_PATH" && "$FORMAT" == "md" ]]; then
  case "$OUTPUT_PATH" in
    *.csv) FORMAT="csv" ;;
    *.tsv) FORMAT="tsv" ;;
    *.md)  FORMAT="md"  ;;
  esac
fi

# --- Preflight ---------------------------------------------------------------

if [[ "$DRY_RUN" -eq 0 ]]; then
  if [[ ! -f "$EXE" ]]; then
    echo "bench/run.sh: $EXE missing — run 'make' first" >&2
    exit 1
  fi
  for f in "$TOKENIZER" "$CWSDPMI"; do
    if [[ ! -f "$f" ]]; then
      echo "bench/run.sh: required file missing: $f" >&2
      exit 1
    fi
  done
fi

# --- Scenario table ----------------------------------------------------------
#
# Each scenario is: NAME | MODEL_PATH | EXTRA_ARGS
# EXTRA_ARGS is appended after the model+tokenizer; `--benchmark` is
# always added by the runner.

SCENARIOS=(
  "15m-default|models/stories15M_q80.bin|"
  "15m-seq256|models/stories15M_q80.bin|--max-seq-len 256"
  "42m-seq128|models/stories42M_q80.bin|--max-seq-len 128"
)

# --- Benchmark block parser --------------------------------------------------
#
# Reads captured stdout; locates `--- VELLM BENCHMARK ---` ... `--- END ---`;
# extracts known fields. Unknown / missing fields default to `?`. Shell
# vars are written via the caller's namespace — function takes the stdout
# file path and a prefix.
#
# Fields (must match instr's task #1 spec, in spec order):
#   cpu, model, ckpt_bytes, tokens, prompt_tok, gen_tok,
#   wall_ms, prompt_tps, gen_tps, peak_mem

parse_benchmark_block() {
  local capfile="$1"
  # Default every field to ? so a missing block still produces a full row.
  BM_cpu="?"
  BM_model="?"
  BM_ckpt_bytes="?"
  BM_tokens="?"
  BM_prompt_tok="?"
  BM_gen_tok="?"
  BM_wall_ms="?"
  BM_prompt_tps="?"
  BM_gen_tps="?"
  BM_peak_mem="?"

  # Strip CR (DJGPP writes CRLF on DOS).
  local scratch
  scratch=$(mktemp -t vellm-bench-parse.XXXXXX)
  tr -d '\r' < "$capfile" > "$scratch"

  # awk extracts the lines between the fenceposts and emits KEY=VALUE
  # pairs to eval. Tolerant: no fencepost → prints nothing → all fields
  # stay `?`. Extra fields we don't recognize are silently ignored.
  #
  # We split on ":" only once (field name is everything before the first
  # colon, trimmed) so a colon in the CPU brand string doesn't confuse us.
  eval "$(awk '
    BEGIN { in_block = 0 }
    /^--- VELLM BENCHMARK ---/ { in_block = 1; next }
    /^--- END ---/             { in_block = 0; next }
    !in_block { next }
    {
      # Find first colon.
      ci = index($0, ":")
      if (ci == 0) next
      key = substr($0, 1, ci - 1)
      val = substr($0, ci + 1)
      # trim leading/trailing whitespace on both
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", key)
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", val)
      # shell-escape val via single quotes (replace embedded single quote)
      gsub(/'\''/, "'\''\\'\'\''", val)

      # Map spec field names -> shell-safe variable names.
      if      (key == "cpu")          print "BM_cpu='\''" val "'\''"
      else if (key == "model")        print "BM_model='\''" val "'\''"
      else if (key == "ckpt bytes")   print "BM_ckpt_bytes='\''" val "'\''"
      else if (key == "tokens")       print "BM_tokens='\''" val "'\''"
      else if (key == "prompt tok")   print "BM_prompt_tok='\''" val "'\''"
      else if (key == "gen tok")      print "BM_gen_tok='\''" val "'\''"
      else if (key == "wall ms")      print "BM_wall_ms='\''" val "'\''"
      else if (key == "prompt tok/s") print "BM_prompt_tps='\''" val "'\''"
      else if (key == "gen tok/s")    print "BM_gen_tps='\''" val "'\''"
      else if (key == "peak mem")     print "BM_peak_mem='\''" val "'\''"
    }
  ' "$scratch")"

  rm -f "$scratch"
}

# --- Row emitter -------------------------------------------------------------

emit_header() {
  case "$FORMAT" in
    md)
      cat <<'HDR'
| scenario | cpu | model | ckpt bytes | tokens | prompt tok | gen tok | wall ms | prompt tok/s | gen tok/s | peak mem |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
HDR
      ;;
    csv)
      echo "scenario,cpu,model,ckpt_bytes,tokens,prompt_tok,gen_tok,wall_ms,prompt_tps,gen_tps,peak_mem"
      ;;
    tsv)
      printf 'scenario\tcpu\tmodel\tckpt_bytes\ttokens\tprompt_tok\tgen_tok\twall_ms\tprompt_tps\tgen_tps\tpeak_mem\n'
      ;;
  esac
}

emit_row() {
  local scenario="$1"
  case "$FORMAT" in
    md)
      printf '| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n' \
        "$scenario" "$BM_cpu" "$BM_model" \
        "$BM_ckpt_bytes" "$BM_tokens" "$BM_prompt_tok" "$BM_gen_tok" \
        "$BM_wall_ms" "$BM_prompt_tps" "$BM_gen_tps" "$BM_peak_mem"
      ;;
    csv)
      printf '%s,"%s","%s",%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$scenario" "$BM_cpu" "$BM_model" \
        "$BM_ckpt_bytes" "$BM_tokens" "$BM_prompt_tok" "$BM_gen_tok" \
        "$BM_wall_ms" "$BM_prompt_tps" "$BM_gen_tps" "$BM_peak_mem"
      ;;
    tsv)
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$scenario" "$BM_cpu" "$BM_model" \
        "$BM_ckpt_bytes" "$BM_tokens" "$BM_prompt_tok" "$BM_gen_tok" \
        "$BM_wall_ms" "$BM_prompt_tps" "$BM_gen_tps" "$BM_peak_mem"
      ;;
  esac
}

# --- Scenario runner ---------------------------------------------------------
#
# Stages MODEL.BIN + TOKEN.BIN under 8.3 names (matching tests/run-golden.sh)
# so DOSBox-X's LFN mangling doesn't bite. Invokes vellm.exe via
# tools/dosbox-run.sh, captures stdout, parses the benchmark block.

run_scenario() {
  local name="$1"
  local model_path="$2"
  local extra_args="$3"

  if [[ ! -f "$model_path" ]]; then
    echo "bench/run.sh: scenario $name skipped — $model_path not present" >&2
    return 2
  fi

  # 8.3-safe staging (same trick as tests/run-golden.sh).
  local stage rawout
  stage=$(mktemp -d -t vellm-bench-stage.XXXXXX)
  rawout=$(mktemp -t vellm-bench-raw.XXXXXX)
  # shellcheck disable=SC2064
  trap "rm -f '$rawout'; rm -rf '$stage'" RETURN

  cp "$model_path" "$stage/MODEL.BIN"
  cp "$TOKENIZER"  "$stage/TOKEN.BIN"

  local args_line
  args_line="MODEL.BIN -z TOKEN.BIN --benchmark"
  if [[ -n "$extra_args" ]]; then
    args_line="$args_line $extra_args"
  fi

  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "DRY: tools/dosbox-run.sh --exe $EXE --args '$args_line'" \
         "--include $stage/MODEL.BIN --include $stage/TOKEN.BIN" >&2
    # Leave a sentinel BM_* record so the dry-run still emits a row.
    BM_cpu="(dry-run)"
    BM_model="(dry-run)"
    BM_ckpt_bytes="?"; BM_tokens="?"; BM_prompt_tok="?"; BM_gen_tok="?"
    BM_wall_ms="?"; BM_prompt_tps="?"; BM_gen_tps="?"; BM_peak_mem="?"
    return 0
  fi

  echo "bench/run.sh: running scenario '$name' ($model_path $extra_args)..." >&2

  tools/dosbox-run.sh \
    --exe "$EXE" \
    --args "$args_line" \
    --include "$stage/MODEL.BIN" \
    --include "$stage/TOKEN.BIN" \
    --stdout "$rawout"

  parse_benchmark_block "$rawout"

  # If the exe did emit a block, we expect to see a real cpu value.
  if [[ "$BM_cpu" == "?" ]]; then
    echo "bench/run.sh: warning: no '--- VELLM BENCHMARK ---' block found in scenario '$name' stdout" >&2
    echo "             (first 20 lines of captured output):" >&2
    head -n 20 "$rawout" >&2 || true
  fi
}

# --- Main driver -------------------------------------------------------------

# Build the list of scenarios to run.
PLAN=()
if [[ -n "$SELECTED_SCENARIO" ]]; then
  for row in "${SCENARIOS[@]}"; do
    name="${row%%|*}"
    if [[ "$name" == "$SELECTED_SCENARIO" ]]; then
      PLAN+=("$row")
      break
    fi
  done
  if [[ ${#PLAN[@]} -eq 0 ]]; then
    echo "bench/run.sh: unknown scenario: $SELECTED_SCENARIO" >&2
    echo "available:" >&2
    for row in "${SCENARIOS[@]}"; do
      echo "  ${row%%|*}" >&2
    done
    exit 2
  fi
else
  PLAN=("${SCENARIOS[@]}")
fi

# Destination sink — either a file or stdout.
if [[ -n "$OUTPUT_PATH" ]]; then
  # Ensure parent dir exists.
  mkdir -p "$(dirname "$OUTPUT_PATH")"
  if [[ "$OUTPUT_MODE" == "replace" || ! -s "$OUTPUT_PATH" ]]; then
    : > "$OUTPUT_PATH"
    emit_header >> "$OUTPUT_PATH"
  fi
  SINK=">> $OUTPUT_PATH"
else
  emit_header
  SINK=""
fi

for row in "${PLAN[@]}"; do
  IFS='|' read -r name model_path extra_args <<< "$row"

  if run_scenario "$name" "$model_path" "$extra_args"; then
    if [[ -n "$OUTPUT_PATH" ]]; then
      emit_row "$name" >> "$OUTPUT_PATH"
    else
      emit_row "$name"
    fi
  fi
done

if [[ -n "$OUTPUT_PATH" ]]; then
  echo "bench/run.sh: wrote results to $OUTPUT_PATH" >&2
fi
