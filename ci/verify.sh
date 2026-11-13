#!/usr/bin/env bash
# Trusted Gitea runs only (ci/README.md), against the pinned toolchain, on the
# repository-scoped host runner that owns the checker profile.
#   ci/verify.sh gate   a push: dcheck gate, in the root and in each subproject
#   ci/verify.sh deep   the nightly run: make deep, judged by ci/findings.py,
#                       which keeps the "Nightly findings" issue
# Sourced, it only defines the check lists and functions above its run, for
# the offline tests.
set -euo pipefail

# The checks each tier must PASS. dcheck reports a check it cannot run as SKIP
# and still passes the tier, so only a check's PASS line shows that it ran.
gate_required=(subset lint matrix analyzer sanitize msan tsan release wasm i386 coverage regions)
deep_required=("${gate_required[@]}" semantics valgrind fuzz cbmc)
# The only checks any project may skip. authority checks capabilities against
# a ward.toml, and no project here declares one, so it always skips.
allowed_skips=(authority)
# Further checks a subproject's gate may skip, by its path, space-separated.
# Add one only after the runner shows the subproject cannot run it, and say why.
declare -A subproject_skips=()

# The tracked subprojects with their own dcheck.conf, root excluded, one per line.
subprojects() {
    git -C "$1" ls-files -- '*/dcheck.conf' | sed 's|/dcheck\.conf$||'
}

# gate_verdict LOG [ALLOWED_SKIP...]: whether the dcheck gate output in LOG
# passes: every required check not allowed to skip among its passes, and no
# other skip. Prints why it does not.
gate_verdict() {
    local log="$1" name line verdict=0
    shift
    local allowed=" $* "
    for name in "${gate_required[@]}"; do
        [[ "$allowed" == *" $name "* ]] && continue
        line="$(grep -E "^(PASS|FAIL|SKIP) $name( |\$)" "$log" | tail -n 1 || true)"
        case "$line" in
            "PASS "*) ;;
            "") printf 'required check %s did not run\n' "$name"; verdict=1 ;;
            *) printf 'required check %s did not pass: %s\n' "$name" "$line"; verdict=1 ;;
        esac
    done
    while read -r name; do
        [[ "$allowed" == *" $name "* || " ${gate_required[*]} " == *" $name "* ]] && continue
        printf 'check %s skipped and not allowed to\n' "$name"
        verdict=1
    done < <(sed -n 's/^SKIP \([^ ]*\).*/\1/p' "$log")
    grep -q '^dcheck gate: PASS' "$log" || { printf 'dcheck gate did not pass\n'; verdict=1; }
    return "$verdict"
}

# The deep run's verdict and findings issue, from exit status $1, with root
# and work as set below. The verdict needs every deep check among the passes,
# so none can pass by being skipped. Gitea gets 20 minutes, inside the job's
# timeout-minutes.
findings() {
    local checks=() name
    for name in "${deep_required[@]}"; do checks+=(--require "$name"); done
    for name in "${allowed_skips[@]}"; do checks+=(--allow-skip "$name"); done
    python3 -I "$root/ci/findings.py" --result "$work/result.json" --root "$work/c-utils" \
        "${checks[@]}" --log "$root/ci-artifacts/deep.log" --exit-status "$1" --rev "${GITHUB_SHA:-$(git -C "$root" rev-parse HEAD)}" \
        --run-url "${GITHUB_SERVER_URL:-}/$GITHUB_REPOSITORY/actions/runs/${GITHUB_RUN_ID:-}" \
        --login "$CUTILS_CI_API_LOGIN" --repo "$GITHUB_REPOSITORY" --within 1200
}

# return works only when sourced.
if (return 0 2>/dev/null); then return 0; fi

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
# keep_logs DIR OUT: the dcheck logs of the project in DIR, kept in OUT.
keep_logs() {
    if [[ -d "$1/.dcheck/logs" ]]; then
        cp -a "$1/.dcheck/logs" "$2/logs"
        # Where the time went, in the job log, which outlives the artifacts.
        python3 -I "$root/ci/timings.py" "$1/.dcheck/logs"
    fi
}

status=0
if [[ "$tier" == gate ]]; then
    # gate_project DIR OUT [ALLOWED_SKIP...]: dcheck gate in the project in
    # DIR, under the same 3 GiB scope runner as make deep, its logs kept in OUT.
    gate_project() {
        local dir="$1" out="$2" rc=0
        shift 2
        mkdir -p "$out"
        (cd "$dir" && "$toolchain/bin/dcheck-tools" exec nice -n 15 \
            python3 -I "$work/c-utils/tools/verify_deep.py" --seconds 900 -- "$toolchain/bin/dcheck" gate) \
            2>&1 | tee "$out/gate.log" || rc=$?
        keep_logs "$dir" "$out"
        [[ "$rc" -eq 0 ]] || { printf 'dcheck gate exited %d\n' "$rc"; return 1; }
        gate_verdict "$out/gate.log" "$@"
    }
    # Listed before any gate runs, so a failed listing stops the tier.
    listed="$(subprojects "$root")"
    subs=()
    [[ -z "$listed" ]] || mapfile -t subs <<<"$listed"
    summary=() failed=0
    if gate_project "$work/c-utils" "$root/ci-artifacts" "${allowed_skips[@]}"; then
        summary+=("PASS .")
    else
        summary+=("FAIL ."); failed=1
    fi
    for sub in ${subs[@]+"${subs[@]}"}; do
        printf 'CI gate: %s\n' "$sub"
        read -ra extra <<<"${subproject_skips[$sub]:-}"
        if gate_project "$work/c-utils/$sub" "$root/ci-artifacts/${sub//\//_}" \
                "${allowed_skips[@]}" ${extra[@]+"${extra[@]}"}; then
            summary+=("PASS $sub")
        else
            summary+=("FAIL $sub"); failed=1
        fi
    done
    printf 'CI gate projects:\n'
    printf '  %s\n' "${summary[@]}"
    [[ "$failed" -eq 0 ]] || { printf 'CI gate: FAIL\n'; exit 1; }
    printf 'CI gate: PASS\n'
    exit 0
fi

printf 'CI deep: the root project only; the gate tier checks the subprojects (%s)\n' \
    "$(subprojects "$root" | paste -sd ' ' -)"
# Two semantics, Valgrind or CBMC files at a time: a Cerberus run peaks at
# 0.7 to 1.6 GiB, so two fit the scope's 3 GiB and a third may not.
"$toolchain/bin/dcheck-tools" exec env DCHECK_JOBS=2 nice -n 15 \
    make -j1 -C "$work/c-utils" deep DEEP_SECONDS=2400 DCHECK="$toolchain/bin/dcheck" \
    DCHECK_JSON="$work/result.json" 2>&1 | tee "$root/ci-artifacts/deep.log" || status=$?
keep_logs "$work/c-utils" "$root/ci-artifacts"
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
