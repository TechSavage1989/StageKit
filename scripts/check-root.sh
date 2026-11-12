#!/usr/bin/env bash
# Fails when the repository root contains anything that is not on
# scripts/root-allowlist.txt.
#
# Inside a Git checkout it checks tracked files. In an extracted source
# archive (no .git) it checks what is on disk, ignoring the generated
# folders listed below.
#
# Usage: scripts/check-root.sh [repository root]
set -euo pipefail

root="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
allowlist="$root/scripts/root-allowlist.txt"
# Generated locally by builds and tools; never tracked.
generated='^(\.git|\.deps|build|build-tests|build_.*|release|\.vscode|\.idea)$'

if [ ! -f "$allowlist" ]; then
  echo "check-root: missing $allowlist" >&2
  exit 2
fi

if git -C "$root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  entries="$(git -C "$root" ls-files | cut -d/ -f1 | sort -u)"
  mode="tracked files"
else
  entries="$(ls -A "$root" | grep -Ev "$generated" | sort -u || true)"
  mode="files on disk (no Git metadata)"
fi

allowed="$(grep -Ev '^\s*(#|$)' "$allowlist" | sort -u)"
unexpected="$(comm -23 <(printf '%s\n' "$entries") <(printf '%s\n' "$allowed") || true)"

if [ -n "$unexpected" ]; then
  echo "check-root: unexpected entries at the repository root ($mode):" >&2
  printf '  %s\n' $unexpected >&2
  echo "Move them into the right folder, or add them to scripts/root-allowlist.txt with a reason." >&2
  exit 1
fi
echo "check-root: root is clean ($mode)."
