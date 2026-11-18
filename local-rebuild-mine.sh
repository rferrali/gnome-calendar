#!/usr/bin/env bash
# Rebuild "mine" = <base> + a --no-ff merge of every topic branch.
#
# <base> is `git config fork.base` (default upstream/main), e.g. a stable
# branch such as upstream/gnome-51. The local branch fork-base is set to it,
# and new topic branches start from it.
#
# Topic branches: anything under feature/, fix/ or local/ (local/ is merged
# last). Each is rebased onto the fresh base first; commits upstream already
# took are dropped by the rebase, and a branch left empty is skipped.
#
# A branch can build on another topic branch instead of the base:
#   git config branch.<branch>.forkParent <parent>
# It is then rebased onto its rebased parent, and merged after it.
#
# Nothing is pushed. The script prints the push command at the end.
# If a rebase conflicts, resolve it (git rebase --continue) and rerun.

set -euo pipefail

# This file is tracked on local/fork only, so it leaves the work tree while
# the other branches are checked out: run from a temporary copy.
if [ -z "${REBUILD_MINE_COPY:-}" ]; then
    copy="$(mktemp)"
    cp "$0" "$copy"
    REBUILD_MINE_COPY="$copy" exec bash "$copy" "$@"
fi
trap 'rm -f "$REBUILD_MINE_COPY"' EXIT

cd "$(git rev-parse --show-toplevel)"

if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "Working tree has uncommitted changes; aborting." >&2
    exit 1
fi

base="$(git config fork.base || echo upstream/main)"

git fetch upstream
git fetch origin
git branch -f main upstream/main
git branch -f fork-base "$base"
echo "Base: $base ($(git log -1 --format='%h %s' fork-base))"

mapfile -t found < <(
    git for-each-ref --format='%(refname:short)' refs/heads/feature refs/heads/fix
    git for-each-ref --format='%(refname:short)' refs/heads/local
)

# Order parents before the branches built on them, keeping local/ last
declare -A parent old_tip
for b in "${found[@]}"; do
    parent[$b]="$(git config "branch.$b.forkParent" || true)"
    old_tip[$b]="$(git rev-parse "$b")"
    if [ -n "${parent[$b]}" ] && ! git show-ref --quiet --verify "refs/heads/${parent[$b]}"; then
        echo "$b: forkParent ${parent[$b]} is not a local branch; aborting." >&2
        exit 1
    fi
done

branches=()
declare -A placed
while [ "${#branches[@]}" -lt "${#found[@]}" ]; do
    progress=0
    for b in "${found[@]}"; do
        p="${parent[$b]}"
        if [ -z "${placed[$b]:-}" ] && { [ -z "$p" ] || [ -n "${placed[$p]:-}" ]; }; then
            branches+=("$b")
            placed[$b]=1
            progress=1
        fi
    done
    if [ "$progress" -eq 0 ]; then
        echo "forkParent settings form a cycle; aborting." >&2
        exit 1
    fi
done

merged=()
for b in "${branches[@]}"; do
    p="${parent[$b]}"
    if [ -n "$p" ]; then
        # Replay only this branch's own commits onto the rebased parent. The
        # parent's reflog finds where they start even after the parent was
        # amended or rebased by hand; its tip from before this run otherwise.
        start="$(git merge-base --fork-point "$p" "$b" || echo "${old_tip[$p]}")"
        git rebase --quiet --onto "$p" "$start" "$b"
        onto="$p"
    else
        git rebase --quiet fork-base "$b"
        onto=fork-base
    fi
    if [ "$(git rev-list --count "$onto".."$b")" -eq 0 ]; then
        echo "== $b: fully upstream now, skipping (delete with: git branch -d $b; git push origin :$b)"
    else
        echo "== $b: $(git rev-list --count "$onto".."$b") commit(s)${p:+ on $p}"
        merged+=("$b")
    fi
done

backup="mine-backup-$(date +%F-%H%M%S)"
git tag "$backup" mine
git switch --quiet mine
git reset --quiet --hard fork-base
for b in "${merged[@]}"; do
    git merge --quiet --no-ff --no-edit -m "Merge branch '$b'" "$b"
done

echo
git log --oneline --first-parent fork-base..mine
echo
echo "Old mine saved as tag $backup. When happy:"
echo "  git push --force-with-lease origin fork-base mine ${merged[*]}"
