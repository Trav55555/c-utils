#!/usr/bin/env bash
# Trusted Gitea push only: make deep against the pinned toolchain, on the
# repository-scoped host runner that owns the checker profile.
set -euo pipefail
ulimit -c 0
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ "$(uname -s)" == Linux && "$(uname -m)" == x86_64 ]] || { echo 'CI requires Linux x86_64' >&2; exit 1; }
: "${XDG_RUNTIME_DIR:?user systemd runtime required}"
: "${RUNNER_TEMP:?runner scratch required}"
command -v flock >/dev/null
command -v systemd-run >/dev/null
command -v tea >/dev/null
systemctl --user show-environment >/dev/null
# One deep run at a time, even if this repository gains another runner.
exec 9>"$XDG_RUNTIME_DIR/c-utils-ci.lock"
flock 9
work="$(mktemp -d "$RUNNER_TEMP/c-utils-ci.XXXXXX")"
printf 'CI scratch: %s\n' "$work"
cp -a "$root" "$work/c-utils"
python3 -I "$root/ci/fetch_dependencies.py" "$root/ci/dependencies.json" "$work"
toolchain="$work/dependable-c-agent-toolchain"
"$toolchain/bin/dcheck-tools" check
# A fresh toolchain has no regions binary, and dcheck skips regions without
# one; build it so the gate's regions check runs.
make -C "$toolchain" regions/regions
# Logs land in the runner's checkout, where the workflow uploads them.
mkdir -p "$root/ci-artifacts"
"$toolchain/bin/dcheck-tools" exec systemd-run --user --scope -q \
    -p MemoryMax=3G -p MemorySwapMax=0 -p RuntimeMaxSec=2400 nice -n 15 \
    make -j1 -C "$work/c-utils" deep DCHECK="$toolchain/bin/dcheck" \
    2>&1 | tee "$root/ci-artifacts/deep.log"
cp -a "$work/c-utils/.dcheck/logs" "$root/ci-artifacts/logs"
grep -Fx 'PASS regions' "$root/ci-artifacts/deep.log"
grep -q '^dcheck deep: PASS' "$root/ci-artifacts/deep.log"
printf 'CI deep: PASS\n'
