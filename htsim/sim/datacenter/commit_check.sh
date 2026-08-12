#!/usr/bin/env bash

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"
export HTSIM_TRACE_FLOW_COMPLETIONS=1

default_files=(
    "validate_uec_sender.txt"
    "validate_uec_rcv.txt"
    "validate_uec_both.txt"
    "validate_load_balancing_snd.txt"
    "validate_load_balancing_rcv.txt"
    "validate_load_balancing_failed_snd.txt"
    "validate_load_balancing_failed_rcv.txt"
    "validate_uec_connreuse.txt"
)

if (( $# > 0 )); then
    files=("$@")
else
    files=("${default_files[@]}")
fi

for file in "${files[@]}"; do
    echo "Running ${file}"
    python3 validate.py "${file}"
done

echo "Validation gate passed ${#files[@]} plan(s)."
