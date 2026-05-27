#!/usr/bin/env bash
# Safely merges new upstream OrcaSlicer commits into your local branch.
# Protects the Bambu cloud bridge files from being overwritten.
# Never force-pushes or deletes anything.

set -e
cd "$(dirname "$(readlink -f "$0")")"

UPSTREAM_BRANCH="upstream/release/v2.4"
LOCAL_BRANCH="local/v2.4"

# Files/dirs that belong to your Bambu cloud code — never overwrite these.
PROTECTED=(
    "src/slic3r/Utils/PJarczakLinuxBridge"
)

# ── Sanity checks ────────────────────────────────────────────────────────────
CURRENT=$(git branch --show-current)
if [[ "$CURRENT" != "local/v2.4" ]]; then
    echo "ERROR: You are on branch '$CURRENT', expected 'local/v2.4'."
    echo "Switch first:  git checkout local/v2.4"
    exit 1
fi

if ! git diff --quiet || ! git diff --cached --quiet; then
    echo "ERROR: You have uncommitted changes. Please stash or commit them first:"
    echo "  git stash"
    exit 1
fi

# ── Fetch ────────────────────────────────────────────────────────────────────
echo "=== Fetching upstream ==="
git fetch upstream --no-tags -q
echo "Done."
echo ""

NEW=$(git log --oneline "${LOCAL_BRANCH}..${UPSTREAM_BRANCH}" 2>/dev/null)
if [[ -z "$NEW" ]]; then
    echo "Already up to date. Nothing to merge."
    exit 0
fi

COUNT=$(echo "$NEW" | wc -l)
echo "=== $COUNT new upstream commit(s) to merge ==="
echo "$NEW"
echo ""

# ── Merge ────────────────────────────────────────────────────────────────────
echo "=== Merging upstream changes ==="
if git merge --no-edit "$UPSTREAM_BRANCH"; then
    echo ""
    echo "Merge succeeded with no conflicts."
else
    echo ""
    echo "=== Conflicts detected — checking protected files ==="

    CONFLICTS=$(git diff --name-only --diff-filter=U)
    BRIDGE_CONFLICTS=""
    OTHER_CONFLICTS=""

    while IFS= read -r f; do
        IS_PROTECTED=0
        for P in "${PROTECTED[@]}"; do
            if [[ "$f" == "$P"* ]]; then
                IS_PROTECTED=1
                break
            fi
        done
        if [[ $IS_PROTECTED -eq 1 ]]; then
            BRIDGE_CONFLICTS+="  $f"$'\n'
        else
            OTHER_CONFLICTS+="  $f"$'\n'
        fi
    done <<< "$CONFLICTS"

    # Keep our version of protected files automatically
    if [[ -n "$BRIDGE_CONFLICTS" ]]; then
        echo ""
        echo "Keeping YOUR version of Bambu cloud files (never overwriting these):"
        echo "$BRIDGE_CONFLICTS"
        while IFS= read -r f; do
            [[ -z "$f" ]] && continue
            f="${f#  }"
            git checkout --ours "$f"
            git add "$f"
        done <<< "$BRIDGE_CONFLICTS"
    fi

    # Report any other conflicts for manual resolution
    if [[ -n "$OTHER_CONFLICTS" ]]; then
        echo ""
        echo "=== ATTENTION: Conflicts in non-bridge files need your review ==="
        echo "$OTHER_CONFLICTS"
        echo ""
        echo "These are files changed by both upstream and your branch."
        echo "Open each file, look for <<<<<<< markers, decide which to keep,"
        echo "then run:  git add <file> && git merge --continue"
        echo ""
        echo "If you want to bail out completely:  git merge --abort"
        exit 1
    fi

    # All conflicts were in protected files — finish the merge
    git merge --continue --no-edit
    echo ""
    echo "Merge complete. All conflicts were in Bambu bridge files (kept yours)."
fi

echo ""
echo "=== Done. Your branch is now up to date with upstream. ==="
echo ""
echo "To rebuild OrcaSlicer with the new changes, run:"
echo "  ./build_linux.sh -s -i -j 3"
echo ""
echo "To push your updated branch to GitHub:"
echo "  git push origin local/v2.4:release/v2.4"
