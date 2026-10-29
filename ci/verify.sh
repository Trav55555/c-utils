#!/usr/bin/env bash
# Trusted Gitea runs only (ci/README.md), against the pinned toolchain, on the
# repository-scoped host runner that owns the checker profile.
#   ci/verify.sh gate   a push: dcheck gate
#   ci/verify.sh deep   the nightly run: make deep, judged by ci/findings.py,
#                       which keeps the "Nightly findings" issue
set -euo pipefail
ulimit -c 0
tier="${1:-}"
case "$tier" in gate|deep) ;; *) echo 'usage: ci/verify.sh gate|deep' >&2; exit 2 ;; esac
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python3 -I -m unittest discover -s "$root/tests" -p 'test_ci*.py'
VERIFY_REQUIRE_SCOPE=1 python3 -I -m unittest discover -s "$root/tests" -p 'test_verify*.py'
[[ "$(uname -s)" == Linux && "$(uname -m)" == x86_64 ]] || { echo 'CI requires Linux x86_64' >&2; exit 1; }
: "${XDG_RUNTIME_DIR:?user systemd runtime required}"
: "${RUNNER_TEMP:?runner scratch required}"
if [[ "$tier" == deep ]]; then
    : "${CUTILS_CI_API_LOGIN:?API login required for the findings issue}"
    : "${GITHUB_REPOSITORY:?repository required for the findings issue}"
fi
command -v flock >/dev/null
command -v systemd-run >/dev/null
command -v tea >/dev/null
systemctl --user show-environment >/dev/null
# One checker run at a time on this host, the findings issue included.
exec 9>"$XDG_RUNTIME_DIR/c-utils-ci.lock"
flock 9
work="$(mktemp -d "$RUNNER_TEMP/c-utils-ci.XXXXXX")"
printf 'CI scratch: %s\n' "$work"
# The deep run's verdict and findings issue, from exit status $1. The verdict
# needs regions among the passes, so regions cannot pass by being skipped.
# Gitea gets 20 minutes, inside the job's timeout-minutes.
findings() {
    python3 -I "$root/ci/findings.py" --result "$work/result.json" --root "$work/c-utils" \
        --require regions --log "$root/ci-artifacts/deep.log" --exit-status "$1" --rev "$(git -C "$root" rev-parse HEAD)" \
        --run-url "${GITHUB_SERVER_URL:-}/$GITHUB_REPOSITORY/actions/runs/${GITHUB_RUN_ID:-}" \
        --login "$CUTILS_CI_API_LOGIN" --repo "$GITHUB_REPOSITORY" --within 1200
}
# A deep run that stops before make deep still keeps the findings issue.
setup_failed() {
    local rc=$?
    findings "$rc" || true
    exit "$rc"
}
if [[ "$tier" == deep ]]; then trap setup_failed EXIT; fi
cp -a "$root" "$work/c-utils"
python3 -I "$root/ci/fetch_dependencies.py" "$root/ci/dependencies.json" "$work"
toolchain="$work/dependable-c-agent-toolchain"
"$toolchain/bin/dcheck-tools" check
# A fresh toolchain has no regions binary, and dcheck skips regions without
# one; build it so the regions check runs.
make -C "$toolchain" regions/regions
# Logs land in the runner's checkout, where the workflow uploads them.
mkdir -p "$root/ci-artifacts"
keep_logs() {
    if [[ -d "$work/c-utils/.dcheck/logs" ]]; then
        cp -a "$work/c-utils/.dcheck/logs" "$root/ci-artifacts/logs"
        # Where the time went, in the job log, which outlives the artifacts.
        python3 -I "$root/ci/timings.py" "$work/c-utils/.dcheck/logs"
    fi
}

status=0
if [[ "$tier" == gate ]]; then
    # The same 3 GiB scope runner as make deep; the gate takes minutes.
    (cd "$work/c-utils" && "$toolchain/bin/dcheck-tools" exec nice -n 15 \
        python3 -I tools/verify_deep.py --seconds 900 -- "$toolchain/bin/dcheck" gate) \
        2>&1 | tee "$root/ci-artifacts/gate.log" || status=$?
    keep_logs
    [[ "$status" -eq 0 ]] || { printf 'CI gate: FAIL\n'; exit 1; }
    grep -Fx 'PASS regions' "$root/ci-artifacts/gate.log"
    grep -q '^dcheck gate: PASS' "$root/ci-artifacts/gate.log"
    printf 'CI gate: PASS\n'
    exit 0
fi

"$toolchain/bin/dcheck-tools" exec nice -n 15 \
    make -j1 -C "$work/c-utils" deep DEEP_SECONDS=2400 DCHECK="$toolchain/bin/dcheck" \
    DCHECK_JSON="$work/result.json" 2>&1 | tee "$root/ci-artifacts/deep.log" || status=$?
keep_logs
cp "$work/result.json" "$root/ci-artifacts/" 2>/dev/null || true
trap - EXIT
# The exit status is the verdict; a failed issue update is only logged.
verdict=0
findings "$status" || verdict=$?
if [[ "$status" -ne 0 || "$verdict" -ne 0 ]]; then
    printf 'CI deep: FAIL\n'
    exit 1
fi
printf 'CI deep: PASS\n'
