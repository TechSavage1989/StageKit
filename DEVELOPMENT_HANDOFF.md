# StageKit development handoff

Read [AGENTS.md](AGENTS.md) first. This file is the current state.

## Where things stand

| | |
| --- | --- |
| Version | 0.1.0 (in `buildspec.json`). First pre-release is to be tagged `0.1.0-beta1` on `main` by the owner |
| Repository | https://github.com/TechSavage1989/StageKit (public; visibility verified, not changed) |
| Default branch | `main`, which holds all work so far (pull request #1, merged 2026-11-12) |
| Working branch | None open. Start new work on a `feat/`, `fix/`, `docs/` or `chore/` branch |
| Releases / update feed | GitHub releases on this repository. The plugin has no updater |
| Built against | OBS 31.1.1 (`buildspec.json`). The owner runs OBS 32.2.2 on Windows |

## What is built

All on `main`, each with headless tests on Linux:

- Ramp Blur filter, with Blur All hotkeys and obs-websocket vendor requests
- Ambient Background source
- Shadow & Backdrop filter
- Ramp Zoom filter
- Place Item, Show/Hide Animation, Match Item and Layout scene filters
- StageKit dock (Qt): position grid, one-click keys, saved spots, add-an-effect buttons

## Not verified

- **Nothing has been loaded in OBS on Windows or macOS.** CI compiles there; the shaders are only
  exercised on OpenGL. Direct3D compiles `.effect` files at load time, so the first run on Windows is
  the real test.
- The Windows artifact's folder layout has not been opened and checked.
- Ambient Background's *Automatic* live capture needs the OBS front end and is not covered by tests.
- The dock is tested by building the widget and clicking its buttons without an OBS window. It has not
  been seen docked inside OBS, in OBS's theme, or following scene changes.
- No test uses a real Stream Deck. Filter and Source keys are simulated by flipping the filter's enabled
  state and the item's visibility.
- The README has not been viewed rendered on GitHub.

## Known limitations

- Place Item needs one filter per destination; the dock creates the nine grid ones in one click.
- Scene filters do not list items inside groups.
- Linked visibility (hiding a linked item with its leader) is not built.
- Ramp Zoom has no follow-the-mouse mode.
- Ambient Background cuts, rather than cross-fades, when *Automatic* picks a different capture.
- Show/Hide Animation can show a one-frame gap if a hide lands between a frame's tick and its render.
- Ramp Blur carries its own copy of the on/off-switch logic that `SwitchRamp` now provides, and Ambient
  Background its own copy of the Kawase blur that `KawaseBlur` now provides.

## Still to do

1. A Stream Deck plugin with live key state and dial support. The owner asked for this last.
2. Fold the two duplicated helpers above into the shared ones.
3. A mini map in the dock for dragging items (OBS's own preview already allows dragging).

## Commands

```bash
tests/run-tests.sh
```

```bash
bash scripts/check-root.sh
```

Formatting checks are listed in AGENTS.md. Settings live in the user's OBS scene collection; the plugin
writes no files of its own.

## Exceptions to the shared standard

- **`docs/REPOSITORY_STANDARDS.md` is missing.** The owner described the standard in chat but the
  canonical file, which must be identical across their repositories, was not supplied. It must be added
  from that source, not rewritten here.
- **Update checks, the update notice, the download bar and the install dialog do not apply.** StageKit
  is an OBS plugin with no window, executable or updater of its own. Revisit only if the owner asks for
  an update notice inside OBS.
- **No hero image or logo.** The README uses frames rendered by the tests instead of window
  screenshots, because OBS has not been run with a display.
- **`ramp-blur` predates the branch-naming rule** (`feat/`, `fix/`, ...). It is kept because pull
  request #1 is open on it. Use the prefixes for new branches.

## Releasing

1. Add `docs/releases/<version>.md` and merge it.
2. Create the tag on `main`. **An AI coding session cannot do this**: pushing tags and creating or
   editing releases are refused for these sessions. The owner creates it on GitHub: *Releases → Draft a
   new release → Choose a tag*, type the version (for example `0.1.0-beta1`), target `main`, tick *Set
   as a pre-release* for `-beta` / `-rc` versions, and publish.
3. The tag starts `.github/workflows/push.yaml`, which builds every platform, then attaches the files to
   the release for that tag, with the notes from `docs/releases/` and SHA-256 checksums as its text. The
   template sets the release back to **draft** at that point, so open it and press *Publish* again.

Tags must look like `1.2.3`, `1.2.3-beta1` or `1.2.3-rc1`; anything else builds but creates no release.

## Branches

- No unmerged branches. `ramp-blur` was merged and removed by the branch-cleanup workflow on its first
  run (1 deleted, 0 kept). That run reported failure because of a printing bug with an empty list, fixed
  in the following pull request.

## Task history

- 2026-11-12: Owner supplied the repository and asked for the features above to be built and pushed to
  it. Source was pushed to `ramp-blur` and draft pull request #1 was opened. No approval was given for
  marking the pull request ready, merging, tagging or releasing, so none of those were done. This entry
  is history, not standing permission.
- 2026-11-12: Owner approved merging into `main` and publishing a release. Pull request #1 was marked
  ready and merged after all checks passed. Tagging and publishing could not be done from the session
  (refused by the platform), so they were handed to the owner. History, not standing permission.
