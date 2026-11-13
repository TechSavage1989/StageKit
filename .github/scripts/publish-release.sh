#!/usr/bin/env bash
# Publishes a release in stages, from inside the push workflow's run.
#
# The owner tests on Windows and asked not to wait for the other builds: the
# release is created as soon as the Windows files exist, and the macOS and
# Linux files are added to the same release when their builds finish.
#
# Usage: publish-release.sh <notes file> <release title>
# Needs: GH_TOKEN, VERSION, PRERELEASE, GITHUB_REPOSITORY, GITHUB_SHA,
#        GITHUB_RUN_ID. WAIT_MINUTES (default 90) and POLL_SECONDS (default 30)
#        are optional.
#
# It is safe to run again (a re-run of failed jobs): files already on the
# release are left alone and only the missing ones are added.
set -euo pipefail
shopt -s extglob nullglob

notes_file="${1:?notes file}"
title="${2:?release title}"
: "${VERSION:?}" "${PRERELEASE:?}" "${GITHUB_REPOSITORY:?}" "${GITHUB_SHA:?}" "${GITHUB_RUN_ID:?}"

repo="${GITHUB_REPOSITORY}"
run="${GITHUB_RUN_ID}"
hash="${GITHUB_SHA:0:9}"
deadline=$(($(date +%s) + ${WAIT_MINUTES:-90} * 60))
poll="${POLL_SECONDS:-30}"

# Each stage: the build artifacts it needs (the end of their names, before the
# commit hash) and the file types taken from each.
stages=(windows macos linux)
declare -A label=([windows]='Windows' [macos]='macOS' [linux]='Linux')
declare -A needs=([windows]='windows-x64' [macos]='macos-universal' [linux]='ubuntu-24.04-x86_64 sources')
declare -A types=(
  [windows-x64]='zip|exe'
  [macos-universal]='tar.xz|pkg'
  [ubuntu-24.04-x86_64]='tar.xz|deb|ddeb'
  [sources]='tar.xz'
)

# A release of this version made from another commit is not ours to add to.
target="$(gh release view "${VERSION}" --repo "${repo}" --json targetCommitish --jq .targetCommitish 2> /dev/null || true)"
if [[ -n "${target}" && "${target}" != "${GITHUB_SHA}" ]]; then
  echo "${VERSION} was already published from ${target}; nothing to do."
  exit 0
fi

mkdir -p upload artifacts
pending=("${stages[@]}")

artifact_for() { # <variant>: prints the artifact's name, if it has been uploaded
  grep -E -- "-${1}-${hash}\$" <<< "${artifacts_now}" | head -n 1 || true
}

write_notes() {
  tail -n +2 "${notes_file}" > notes.md
  if ((${#pending[@]})); then
    local waiting="" other
    for other in "${pending[@]}"; do waiting+="${waiting:+ and }${label[${other}]}"; done
    {
      echo ""
      echo "_The ${waiting} files are still being built and are added here as soon as they are ready._"
    } >> notes.md
  fi
  {
    echo ""
    echo "### Checksums (SHA-256)"
  } >> notes.md
  local file
  for file in upload/*; do
    echo "    ${file##*/}: $(sha256sum "${file}" | cut -d ' ' -f 1)" >> notes.md
  done
}

publish_stage() { # <stage>
  local stage="$1" variant name file other files=() left=()
  for variant in ${needs[${stage}]}; do
    name="$(artifact_for "${variant}")"
    rm -rf "artifacts/${name}"
    gh run download "${run}" --repo "${repo}" --name "${name}" --dir "artifacts/${name}"
    for file in "artifacts/${name}"/*.@(${types[${variant}]}); do
      mv "${file}" upload/
      files+=("upload/${file##*/}")
    done
  done
  if ((${#files[@]} == 0)); then
    echo "::error::The ${label[${stage}]} build produced no packaged files."
    exit 1
  fi

  for other in "${pending[@]}"; do [[ "${other}" == "${stage}" ]] || left+=("${other}"); done
  pending=("${left[@]}")
  write_notes

  local existing
  if existing="$(gh release view "${VERSION}" --repo "${repo}" --json assets --jq '.assets[].name' 2> /dev/null)"; then
    for file in "${files[@]}"; do
      if grep -qxF -- "${file##*/}" <<< "${existing}"; then
        echo "${file##*/} is already on the release."
      else
        gh release upload "${VERSION}" "${file}" --repo "${repo}"
      fi
    done
    gh release edit "${VERSION}" --repo "${repo}" --notes-file notes.md
  else
    local flags=()
    if [[ "${PRERELEASE}" == "true" ]]; then flags+=(--prerelease); fi
    gh release create "${VERSION}" "${files[@]}" \
      --repo "${repo}" \
      --target "${GITHUB_SHA}" \
      --title "${title}" \
      --notes-file notes.md "${flags[@]}"
  fi
  echo "Published the ${label[${stage}]} files of ${VERSION}."
}

builds_over=0
while ((${#pending[@]})); do
  # The state of the builds is read before the artifact list, so a build seen
  # as finished here has its artifacts in the list read after it.
  builds="$(gh api "repos/${repo}/actions/runs/${run}/jobs?per_page=100" \
    --jq '[.jobs[] | select(.name | startswith("Build Project"))] | "\(length) \(map(select(.status != "completed")) | length)"' ||
    echo '0 1')"
  artifacts_now="$(gh api "repos/${repo}/actions/runs/${run}/artifacts?per_page=100" \
    --jq '.artifacts[] | select(.expired | not) | .name' || true)"

  for stage in "${pending[@]}"; do
    ready=1
    for variant in ${needs[${stage}]}; do
      [[ -n "$(artifact_for "${variant}")" ]] || ready=0
    done
    if ((ready)); then publish_stage "${stage}"; fi
  done
  ((${#pending[@]})) || break

  # Every build has ended (seen twice running) and files are still missing:
  # a build failed. Re-running the failed jobs adds what is missing.
  read -r total running <<< "${builds}"
  if ((total >= 2 && running == 0)); then builds_over=$((builds_over + 1)); else builds_over=0; fi
  if ((builds_over >= 2)); then
    for stage in "${pending[@]}"; do
      echo "::error::The ${label[${stage}]} build did not produce its files; ${VERSION} is missing them."
    done
    exit 1
  fi
  if (($(date +%s) > deadline)); then
    echo "::error::Gave up waiting for the builds; ${VERSION} is missing: ${pending[*]}."
    exit 1
  fi
  sleep "${poll}"
done

echo "${VERSION} is complete."
