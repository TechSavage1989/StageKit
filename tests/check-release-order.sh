#!/usr/bin/env bash
# Exercise the real publisher with Stream Deck ready before Windows and Linux ready last.
set -euo pipefail
publisher="$(cd "$(dirname "$0")/.." && pwd)/.github/scripts/publish-release.sh"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
export GITHUB_SHA=0123456789abcdef GITHUB_RUN_ID=123 GITHUB_REPOSITORY=fixture/stagekit
export VERSION=0.1.0-beta999 PRERELEASE=true POLL_SECONDS=0

gh() {
  case "$1 $2" in
    api*jobs*) echo '4 1' ;;
    api*artifacts*)
      local poll
      poll=$(cat poll)
      echo 'fixture-streamdeck-012345678'
      if ((poll >= 1)); then echo 'fixture-windows-x64-012345678'; fi
      if ((poll >= 2)); then
        printf '%s\n' fixture-macos-universal-012345678 fixture-ubuntu-24.04-x86_64-012345678 fixture-sources-012345678
      fi ;;
    'run download')
      mkdir -p "$9"
      case "$7" in
        *windows*)
          touch "$9/fixture-windows-x64.zip"
          if [[ "$WINDOWS_INSTALLER" == yes ]]; then touch "$9/fixture-windows-x64-Installer.exe"; fi ;;
        *streamdeck*) touch "$9/fixture.streamDeckPlugin" ;;
        *macos*) touch "$9/fixture.pkg" ;;
        *sources*) touch "$9/fixture-source.tar.xz" ;;
        *ubuntu*) touch "$9/fixture.deb" ;;
      esac ;;
    'release view')
      [[ -f published ]] || return 1
      if [[ "$*" == *targetCommitish* ]]; then echo "$GITHUB_SHA"; else cat published; fi ;;
    'release create')
      # Fail if an early non-Windows build creates a placeholder or late platforms delay Windows.
      [[ "$(cat poll)" == 1 ]] || return 1
      [[ " $* " == *' upload/fixture-windows-x64-Installer.exe '* && " $* " == *' upload/fixture-windows-x64.zip '* ]] || return 1
      grep -q 'fixture-windows-x64-Installer.exe:' notes.md && grep -q 'fixture-windows-x64.zip:' notes.md || return 1
      printf '%s\n' fixture-windows-x64-Installer.exe fixture-windows-x64.zip > published ;;
    'release upload') basename "$4" >> published ;;
    'release edit') : ;;
    *) echo "Unexpected test invocation of gh" >&2; return 1 ;;
  esac
}
sleep() { echo "$(( $(cat poll) + 1 ))" > poll; }
export -f gh sleep

for WINDOWS_INSTALLER in yes no; do
  export WINDOWS_INSTALLER
  mkdir "$work/$WINDOWS_INSTALLER"
  cd "$work/$WINDOWS_INSTALLER"
  printf '# Release\n\nReady.\n' > release.md
  echo 0 > poll
  if [[ "$WINDOWS_INSTALLER" == yes ]]; then
    bash "$publisher" release.md 'StageKit fixture'
    test "$(wc -l < published)" -eq 6
    grep -Fxq 'fixture.streamDeckPlugin' published
  else
    if bash "$publisher" release.md 'StageKit fixture'; then exit 1; fi
    test ! -f published
  fi
done
echo 'PASS: Windows installer and ZIP are public first.'
