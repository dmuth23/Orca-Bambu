#!/usr/bin/env bash
# Fetches the latest from upstream OrcaSlicer and reports new commits.
# Safe: read-only, never modifies your branch.

set -e
cd "$(dirname "$(readlink -f "$0")")"

UPSTREAM_BRANCH="upstream/release/v2.4"
LOCAL_BRANCH="local/v2.4"

echo "=== Fetching upstream OrcaSlicer ==="
git fetch upstream --no-tags -q
echo "Done."
echo ""

NEW=$(git log --oneline "${LOCAL_BRANCH}..${UPSTREAM_BRANCH}" 2>/dev/null)

if [[ -z "$NEW" ]]; then
    echo "You are up to date with upstream. Nothing new."
else
    COUNT=$(echo "$NEW" | wc -l)
    echo "=== $COUNT new commit(s) on upstream you don't have yet ==="
    echo ""
    echo "$NEW"
    echo ""
    echo "To pull these in safely, run:  ./safe-update.sh"
fi

echo ""
echo "=== Commits unique to YOUR branch (your customizations) ==="
OURS=$(git log --oneline "${UPSTREAM_BRANCH}..${LOCAL_BRANCH}" 2>/dev/null)
if [[ -z "$OURS" ]]; then
    echo "(none — your branch is a straight copy of upstream)"
else
    echo "$OURS"
fi
