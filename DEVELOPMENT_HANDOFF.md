# StageKit development handoff

Read [AGENTS.md](AGENTS.md) first. This file is the current state.

## Where things stand

| | |
| --- | --- |
| Version | 0.1.0 (in `buildspec.json`). First pre-release is `0.1.0-beta1` |
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
- Auto Frame filter (fixed frame, fits any capture resolution inside it)
- Ramp Zoom filter
- Place Item, Show/Hide Animation, Match Item and Layout scene filters
- StageKit dock (Qt): position grid, one-click keys, saved spots, add-an-effect buttons

## Owner's goal

Rebuild the owner's whole OBS scene collection so StageKit tools do what the nested scenes, source
clones, Move filter chains and Advanced Scene Switcher macros do today. Two reasons: the old setup is
hard to manage and heavy on resources. Judge each new feature by which scenes or macros it removes, and
keep idle cost near zero. The owner confirmed on Windows (OBS 32.2.2, 0.1.0-beta2) that Ramp Blur,
Ambient Background, Shadow & Backdrop, Ramp Zoom, Show/Hide Animation and Layout work.

Auto Frame (0.1.0-beta5) trims black bars and copies its size from an existing item, both asked for by
the owner; neither has been tried on Windows or with a real emulator. The owner wants settings to live
in the filter's own controls where possible, not only in the dock.

0.1.0-beta6 fixes the automatic trim chasing black game backgrounds (owner's report, Snes9x on
Windows) and adds program rules and the Picture Shape key. Whether Game Capture and Window Capture
answer the `get_hooked` call that program rules use is unverified on Windows.

0.1.0-beta7 adds the start image the owner asked for: chosen once in the dock, shown by the Start Image
source and followed by Ambient Background. It is stored in the scene collection's save data under
`stagekit.start_image`. Not yet tried on Windows.

0.1.0-beta6 was never published as its own release: its notes reached `main` moments before
0.1.0-beta7's, and the release job only publishes the newest notes file. Everything in it is in
0.1.0-beta7. Wait for a version's tag to exist (check `git/matching-refs/tags/<version>`, not a call
that prints an error body) before merging the next notes file.

0.1.0-beta8 adds Auto Capture and Send To. Auto Capture: the owner rejected a required program list
("I want it to detect games regardless of windowed or not regardless of the game") and asked for
exclusions. Its Win32 code has never run; expect the detection rule to need tuning from the owner's
reports. Send To: the owner said the point of "send it to" was Stream Deck keys for camera spots, set
up as a filter on the camera itself, and that they will not use the dock's buttons for it.

The owner confirmed on Windows (0.1.0-beta8) that Auto Capture loads and captured Snes9x. They found
its controls and Auto Frame's unintuitive: long labels cut off and tall list boxes. 0.1.0-beta10 and
beta11 redo both: a status line, short labels, numbered groups, picking programs from what is open or
by browsing, and one-line text lists (`rules_text`, `never`, `always`) in place of editable list boxes;
the old array settings are still read. Auto Frame also has a built-in table of emulator shapes
(`kKnownPrograms`); the owner's saved shapes win. The status lines are read when the filter window
opens. Keep labels short enough to fit OBS's narrow filter window.

0.1.0-beta12, from the owner's reports on Windows: Auto Frame holds a found shape across resolution
changes and only gives it up for another real shape that has held for the *Hold a shape for* time
(`trim_wait`); labels were shortened in every tool because long ones pushed the controls right; and a
chosen start image becomes every Ambient Background's picture without a setting (the owner: "that's
what I wanted"). None of it has been run on Windows.

0.1.0-beta13: the dock's scene picture (drag to move, wheel to resize, drag the Ramp Zoom cross;
tested by sending mouse events to the widget, never used by hand); Snes9x's built-in shape corrected to
8:7 after the owner's screenshots, and built-in shapes demoted to a starting guess; and a hold of the
last good frame for 0.2 s after a resolution change (1 s if the capture drops out), because the owner
still saw a split-second flicker when switching between a window and full-screen. Whether that hold is
long enough for Game Capture's re-hook on Windows is unknown.

0.1.0-beta14: Shadow & Backdrop keeps the picture's size and place when its settings change (owner's
report: raising the softness shrank and moved the picture inside its bounding box). A shadow filter
saved by an earlier version is assumed to be laid out already and is left alone on first load.

0.1.0-beta15: Ambient Background's live capture gets a fit mode, a zoom and *Live capture replaces the
picture* (owner's request). "No capture" means the live source has no size or cannot be found; a Game
Capture that is hooked but showing black still counts as a capture.

0.1.0-beta16 adds Visibility Link (show only one / show and hide together), which the owner asked for
to replace the show/hide macros. It only hears changes to items directly in its scene, not inside a
group. Still proposed, not built: a program trigger ("when this program is captured, switch this filter
on"). In the owner's 48 macros, program and window conditions mostly chose a scene per picture shape and
capture type, which Auto Capture and Auto Frame replace. What is left for a trigger: showing the window
captures for ZBrush, Blender and Fusion 360; the Dolphin GBA link layout (windows "GBA1", "GBA2"); a
shadow filter switched per program; and hiding the music widget when Spotify is silent (that one also
needs an audio condition).

Reported problems still open: a Match Item item "dropping behind others" has not been reproduced.
Addressed in 0.1.0-beta4, awaiting the owner's check on Windows: the dock reaching nested scenes and
groups, Match Item inside groups, order buttons, Layout for chosen items.

## Not verified

- Tools added after 0.1.0-beta2 (Auto Frame so far) have not been run in OBS on Windows. Nothing has
  been run on macOS.
- The Windows zip and installer have not been opened or run on a real machine. The installer is built
  by Inno Setup on the CI runner and cannot be built or run on Linux.
- Ambient Background's *Automatic* live capture needs the OBS front end and is not covered by tests.
- The dock is tested by building the widget and clicking its buttons without an OBS window. It has not
  been seen docked inside OBS, in OBS's theme, or following scene changes.
- No test uses a real Stream Deck. Filter and Source keys are simulated by flipping the filter's enabled
  state and the item's visibility.
- The README has not been viewed rendered on GitHub.

## Known limitations

- Place Item needs one filter per destination; the dock creates the nine grid ones in one click.
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

## Windows installer

`.github/scripts/Package-Windows.ps1` builds `stagekit-<version>-windows-x64-Installer.exe` with Inno
Setup 6 from `cmake/windows/resources/installer-Windows.iss`, next to the zip, on every Windows build.
It installs to `C:\ProgramData\obs-studio\plugins\stagekit`, needs administrator rights, leaves out
`.pdb` files and is not code-signed. Keep the `AppId` in the script unchanged so upgrades and the
uninstaller keep working.

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

- **Update checks, the update notice, the download bar and the install dialog do not apply.** StageKit
  is an OBS plugin with no window, executable or updater of its own. Revisit only if the owner asks for
  an update notice inside OBS.
- **No hero image or logo.** The README uses frames rendered by the tests instead of window
  screenshots, because OBS has not been run with a display.
- **`ramp-blur` predates the branch-naming rule** (`feat/`, `fix/`, ...). It is kept because pull
  request #1 is open on it. Use the prefixes for new branches.

## Releasing

Releases publish themselves; nobody pushes a tag by hand.

1. On a branch, add `docs/releases/<version>.md` (for example `0.2.0.md` or `0.2.0-beta1.md`). Bump
   `version` in `buildspec.json` when the base version changes. Merge the pull request.
2. On that push to `main`, `.github/workflows/push.yaml` builds every platform. Its *Find Unpublished
   Release* job takes the newest notes file by version; if no tag of that name exists, *Publish Release*
   creates the tag on that commit and a release with the built files, the notes and SHA-256 checksums.
   Names ending in `-beta` or `-rc` (with an optional number) are published as pre-releases.
3. Check the release page: files present, checksums listed, pre-release flag correct.

An existing tag is never reused or overwritten; to publish again, add a new notes file with a new
version. AI coding sessions cannot push tags or create releases themselves (the platform refuses both),
which is why the repository's own workflow does it. Pushing a version tag by hand still works and goes
through the template's older *Create Release* job, which leaves a draft.

Files released from `main` are built in the `RelWithDebInfo` configuration (optimised, with debug
information), which is what the template uses for pushes to `main`.

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
  ready and merged after all checks passed. Tagging and publishing are refused for sessions by the platform,
  so an automatic release job was added to the push workflow and the release was published by it. History, not standing permission.
- 2026-11-12: `docs/REPOSITORY_STANDARDS.md` was copied byte-for-byte from the owner's other app
  repositories (nine of them held an identical copy).
- 2026-11-12: Owner said "yes merge and publish each one without asking" for the test releases of
  the scene-collection rebuild. Scope: merging rebuild changes after green checks and publishing them
  as pre-releases. Not covered: a stable release, visibility or access changes, unrelated work.
