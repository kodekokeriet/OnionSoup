#!/bin/sh
# Shrink the local OnionSoup repo without changing anything on GitHub:
# drop stale worktrees, deleted remote branches, unpacked/unreachable objects
# and checked-out submodules (CI builds the release, they are not needed locally).
# Usage: sh tools/git_cleanup.sh [repo-folder]   (default: ~/dev/onion-soup)
set -eu
cd "${1:-$HOME/dev/onion-soup}"
echo "Before: $(du -sh . | cut -f1)"
git worktree prune
git fetch -q --prune origin
git submodule deinit -q -f --all 2> /dev/null || true
rm -rf .git/modules
git reflog expire --expire=now --all
git gc -q --prune=now
echo "After:  $(du -sh . | cut -f1)"
