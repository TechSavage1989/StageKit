# StageKit: read this first

StageKit is a plugin for OBS Studio (C++17, libobs, Qt 6 for the dock). It adds filters
and one source that replace hand-built chains of Move filters, source clones and nested scenes.

Read `DEVELOPMENT_HANDOFF.md` next for the current state, then `docs/REPOSITORY_STANDARDS.md` for the
shared organisation standard. That standards file is not in the repository yet: the owner has to supply
the canonical copy (see the handoff). Do not write one from memory.

## What StageKit is not

It is an OBS plugin, not a standalone desktop app. It has no window of its own, no executable, no
installer and no updater, and it cannot restart OBS. The parts of the shared standard about in-app
update checks, update banners, download bars and install dialogs do not apply unless the owner asks
for an update notice inside OBS. This exception is recorded in the handoff.

## Folder map

| Path | What lives there |
| --- | --- |
| `src/plugin-main.cpp` | Module entry: registers every filter and source, global hotkeys, the obs-websocket vendor |
| `src/blur/` | Ramp Blur filter (owns its own multi-type blur pipeline) |
| `src/ambient/` | Ambient Background source |
| `src/shadow/` | Shadow & Backdrop filter |
| `src/zoom/` | Ramp Zoom filter |
| `src/placement/` | Scene filters: Place Item, Show/Hide Animation, Match Item, Layout |
| `src/dock/` | The StageKit dock (Qt widget, no `Q_OBJECT`; it drives Place Item filters) |
| `src/common/` | Shared helpers: easing and `Tween`, `SwitchRamp`, `KawaseBlur`, scene utilities |
| `src/api/` | obs-websocket vendor requests (`StageKit` vendor) |
| `src/third-party/` | Vendored `obs-websocket-api.h`, kept byte-for-byte (excluded from clang-format) |
| `data/effects/` | Shaders in OBS `.effect` format |
| `data/locale/en-US.ini` | Every user-visible string |
| `tests/` | Headless libobs tests, test-image generators, `run-tests.sh` |
| `scripts/` | Repository checks (`check-root.sh`, `root-allowlist.txt`) |
| `docs/images/` | README imagery, produced by the tests |
| `cmake/`, `build-aux/`, `.github/` | From the official obs-plugintemplate; change only when needed |

Root files are limited to `scripts/root-allowlist.txt`. `buildspec.json`, `CMakeLists.txt`,
`CMakePresets.json`, `.clang-format` and `.gersemirc` must stay at the root for the template's tooling.

## Build and test

Linux (needs `libobs-dev`, `qt6-base-dev`, `obs-studio`, `xvfb`, Python Pillow):

```bash
tests/run-tests.sh
```

That configures `build-tests/` with warnings as errors, builds the plugin and both test programs, runs
them under Xvfb and writes contact sheets. Every check must print `ALL PASSED`.

Windows and macOS are built only by GitHub Actions (`.github/workflows/build-project.yaml`); the
artifacts are on each run. `buildspec.json` pins the OBS version they build against (31.1.1).

Before pushing, the template's CI requires:

```bash
clang-format --dry-run --Werror $(git ls-files 'src/*.cpp' 'src/*.hpp' 'tests/*.cpp')
```

```bash
gersemi --check CMakeLists.txt tests/CMakeLists.txt
```

```bash
bash scripts/check-root.sh
```

Use clang-format 19 (`pip install clang-format==19.1.7`) and `pip install gersemi`.

## Things that will bite you

- **Shaders are only proven on OpenGL.** The tests run libobs with OpenGL. Windows uses Direct3D, which
  compiles `.effect` files when OBS loads the plugin. A build passing CI says nothing about that. Keep
  shader code to what both accept: `float(i)` casts, constant loop bounds with `break`, `SampleLevel`
  inside loops, no `inout` parameters.
- **OBS applies source settings on the next video tick.** Tests must wait a few frames after
  `obs_source_update` before rendering.
- **A disabled filter is not drawn.** Effects that animate out when switched off use `SwitchRamp`
  (`src/common/switch-ramp.*`): it re-enables the filter until the ramp ends. Reuse it; do not write a
  second copy. Ramp Blur still has its own older copy of this logic.
- **OBS finishes hiding a scene item after its `item_visible` signal returns.** Re-showing inside the
  handler leaves the item undrawn. Show/Hide Animation re-shows on the next tick instead.
- **Trigger filters** (Place Item, Layout) switch themselves off after running and ignore the enabled
  state restored at load (`ready_`), so they do not fire when OBS starts.
- **Deprecated libobs calls fail CI** (warnings are errors). Avoid `obs_sceneitem_get_info`; use the
  individual position, scale, rotation and bounds calls.
- **`.gitignore` is an allowlist.** A new top-level file or folder is ignored until it is added there
  and to `scripts/root-allowlist.txt`.
- Colour is handled as raw (non-linear) values inside multi-pass pipelines; the final draw follows
  `gs_get_linear_srgb()`. Ramp Blur instead blurs in linear light through sRGB views.

## Conventions

- Persisted enum values in settings are append-only.
- Every user-visible string goes in `data/locale/en-US.ini`; wording is plain and describes what the
  user sees, not the mechanism.
- New features get a headless test section and, where they draw, a contact sheet for the README.
- Update `README.md` in the same pull request when behaviour or settings change, and regenerate the
  affected images in `docs/images/`.

## Branches and pull requests

- Never commit to `main`. One focused change per pull request, on a branch named `feat/`, `fix/`,
  `docs/`, `chore/` or `release/`.
- `.github/workflows/delete-merged-branches.yml` removes merged branches on every push to `main`
  (and on manual runs with a dry-run option). It deletes a branch only when its pull request was merged
  at the branch's current commit, or when `main` contains all of its commits and its last commit is over
  a day old. It never touches `main`, protected branches, or branches with an open pull request, and it
  lists kept branches with unique commits in the run summary.
- `.github/workflows/repo-checks.yaml` runs `scripts/check-root.sh`.
- Merging, marking a pull request ready, tagging and releasing each need the owner's explicit approval
  for that task.

## Talking to the owner

Routine progress is plain, short text. Two markers are reserved and go at the top of a message in bold:

- `⚠️ ACTION NEEDED:` when the owner must decide, approve, supply something, or check something on
  their own machine. Say exactly what to do, where, what to report back and what is blocked. One
  request per line. Repeat an unanswered request in the next update.
- `🚀 PUBLISHED:` when a release is live and its assets are verified, followed by the link and a
  one-line summary.

Use them nowhere else. End every message with a short status: what is done, what is still running, and
what the owner needs to do, if anything.
