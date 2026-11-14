#!/usr/bin/env bash
# Every check that reads the sources instead of running them, behind one entry point.
#
#   ./Tools/rfab/run-static-checks.sh
#
# A new check is added to the list below, not to .github/workflows: the GitHub App used for
# development cannot change workflow files. All checks run even when one fails, so a single CI run
# reports everything that is wrong.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${REPO_ROOT}"

failed=()

run() {
    local name="$1"
    shift
    echo "==> ${name}"
    if ! "$@"; then
        echo "!! ${name} failed: $*"
        failed+=("${name}")
    fi
}

# The tools first prove that each of their rules still catches the mistake it exists for.
run "ci diagnostics self-test"    python3 Tools/rfab/ci_diagnostics.py --self-test
run "hook discipline self-test"   python3 Tools/rfab/check_hook_discipline.py --self-test
run "wire contract self-test"     python3 Tools/rfab/check_wire_contract.py --self-test

# Engine threads only read snapshots and post work (hooks, script sinks, CEF handlers).
run "hook discipline"             python3 Tools/rfab/check_hook_discipline.py
# Every message and event kind has a sender and a receiver in shipped code.
run "wire contract"               python3 Tools/rfab/check_wire_contract.py
# VERSION agrees everywhere, submodules are pinned, no build output in git, manifests parse.
run "release invariants"          python3 Tools/rfab/check_release_invariants.py
# Address Library ids and struct layouts match Skyrim SE 1.5.97.
run "SE 1.5.97 port audit"        python3 Tools/rfab/verify_se1597_port.py

if [[ ${#failed[@]} -gt 0 ]]; then
    echo "!! ${#failed[@]} static check(s) failed: ${failed[*]}"
    exit 1
fi
echo "==> all static checks passed"
