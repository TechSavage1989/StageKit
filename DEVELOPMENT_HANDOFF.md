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
group. 0.1.0-beta17 adds the Program Trigger the owner approved. In the owner's 48 macros, program and window
conditions mostly chose a scene per picture shape and capture type, which Auto Capture and Auto Frame
replace. The trigger is for what was left: showing the window captures for ZBrush, Blender and Fusion
360; the Dolphin GBA link layout (window titles "GBA1", "GBA2"); and a shadow filter switched per
program. Not covered: hiding the music widget when Spotify is silent, which needs an audio condition.
The trigger is edge-triggered on purpose, so a manual change survives until the program comes or goes.

0.1.0-beta18, from the owner's reports: automatic trim did nothing for DS emulators because 2:3 was
not in `is_picture_shape`; the list now has two-screen, other handheld, upright and ultra-wide shapes.
*Keep this trim for this program* saves the current crop's shape as a rule for the captured program
and returns to automatic. The shape gate itself was not loosened.

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

## Outline made from the picture, redone (0.1.0-beta33)

The owner ran beta31 on Windows (so the Outline and Compose shaders compile on Direct3D) and showed a
47 px picture outline that looked "stacked or multiplied multiple times", and asked for blur options.
The colour came from averaging the 48 dilation samples, which are shifted copies. Now an `Extend` pass
draws the source carried on past its edges over the whole padded area (`outline_edge`: 0 stretch,
1 mirror), `KawaseBlur` softens it by `outline_blur`, and the Outline pass shows it through the line.
The dilation mask is 24 directions by 4 rings. **The Extend pass is not verified on Direct3D.**

### Copied filters (same release)

The owner copied the filter from a source to a group: the group moved and OBS crashed. The move is
explained and fixed: copied settings include `adjusted_*`, so the copy believed its new parent's items
were already shifted. The filter now stores `adjusted_for` (the parent's UUID) and clears the record
on its first tick when the parent differs. A side effect: a source made with OBS's *Paste (Duplicate)*
gets its copied items shifted by the room once. **The crash was not reproduced** on Linux (test
`[shadow copied to a group]`); ask for the crash report.

Effect values last for one draw: the `Extend` pass first ran without `out_size`, `src_size` and `pad`
re-set and libobs logged "set to invalid size 0". Set every value a technique reads before each draw.

## Background ignores an empty capture (0.1.0-beta32)

The owner: with *Live capture replaces the picture* on and Game Capture capturing nothing, the
background was black; "if the live capture is completely black, the start image must be shown".
An auto-framed capture always has the frame's size, so it never looked absent.
`AmbientBackground::live_shows_something` reads back a 64x64 copy of the live picture every 0.5 s:
all transparent is empty at once, all black is empty after 2 s (so a game's cut to black does not flash
the start image), anything visible ends it immediately. The 2 s is a constant, `kBlackFor`; the owner
has not been asked whether they want it adjustable.

## Shadow, Glow & Outline (0.1.0-beta31)

Asked for by the owner on 2026-11-12: a stroke/border, a shadow or border "made of the object itself"
for live captures, more types, and a name that fits; simple and clean.

- Display name is now *Shadow, Glow & Outline (StageKit)*. The id stays `stagekit_shadow_backdrop` and
  the files stay `src/shadow/shadow-backdrop.*`; every setting key is unchanged.
- New settings: `shadow_type` (0 drop, 1 glow: offsets ignored), `shadow_fill` and `outline_fill`
  (0 colour, 1 the picture itself), `shadow_boost`, `outline`, `outline_width`, `outline_color`,
  `outline_opacity`.
- Shader: new `Outline` technique (48 samples in a 16-step loop, `SampleLevel`), run between Card and
  Compose; Compose can colour the shadow from the blurred body. **Not verified on Direct3D.**
- Controls are three checkable groups whose names are the existing `shadow` and `backdrop` settings
  plus `outline`.

## Background live capture follows Auto Frame (0.1.0-beta30)

The owner saw black bars in Ambient Background's live capture whatever the fit setting. The background
drew the capture's whole output, and an auto-framed capture is a frame with empty sides.
`auto_frame_picture()` (`src/frame/auto-frame.*`) gives the picture's rectangle, allowing for a
shadow's room after the frame; `render_child(..., picture_only)` renders just that rectangle.

## Corners & Edges (0.1.0-beta29)

Asked for by the owner on 2026-11-12: "a filter that can round, chamfer, or otherwise modify objects
corners and edges and it should work with the drop shadow and other tools without breaking them".

- `src/shape/corners.*`, `data/effects/stagekit-corners.effect` (one pass, signed-distance shapes).
- Size never changes. Sizes are screen pixels by default (`shown_scale`).
- Shadow & Backdrop keeps its own *radius* setting; with both, put the shape here and leave the
  shadow's radius at 0.
- **Not verified on Direct3D.** The shader uses only `lerp`, `step`, `saturate`, `length`, no loops.

## Groups in the arrange tools (0.1.0-beta28)

The owner selected the camera and "Chat Box Group" with *Each other* and *Left*; the camera went to
the screen's edge. `selected_items` skipped groups, leaving one item, which fell back to the screen.
A selected group is now one item (its children are not added as well); in a group that is not
selected, selected children still count.

## Arrange anywhere, background dock (0.1.0-beta27)

Asked for by the owner on 2026-11-12: they did not understand the Arrange tools, wanted them usable
without the dock ("right click inside the viewport"), on several selected items, by visible pixels or
by box, with resizing that either keeps the shape or stretches; and wanted the start image and
background in a dock of their own. They also reported the thumbnail missing at start-up.

- `src/dock/arrange-tools.*`: `ArrangeAction`, shared `ArrangeOptions`, `run_arrange`, frontend hotkeys
  (`stagekit.arrange.*`), saved in the collection's module data.
- `PreviewMenu`: adds a *StageKit* submenu to OBS's preview context menu by watching for it. **Not
  verified on Windows**; nothing in OBS's API supports it.
- `match_size` takes width, height and `keep_shape`. Stretching an item that has a shape-keeping
  bounding box turns the box into a stretch box of what is drawn now.
- `BackgroundDock` (id `stagekit-background-dock`). The thumbnail falls back to
  `gs_create_texture_file_data` when Qt cannot read the file, retries a few times, and logs why it
  failed. The cause of the owner's missing thumbnail was not reproduced; ask for the OBS log if it
  still happens (look for "could not read the picture").
- `bounds_scale`, `item_draw_scale` and the on-screen scale search moved to `src/common/shown-scale.*`.

## Dock wording (0.1.0-beta26)

The owner: "no need for all the informations verbiage in the dock. need to keep the ui clean and
simple". The three explanatory paragraphs became tooltips and the labels were shortened. Keep the dock
to controls and short labels; explanations go in tooltips and the README.

## Cropped View: whatever is drawn (0.1.0-beta25)

The owner's music widget (Amuse Main, a web page) was cropped by hand years ago; the widget's layout
has since changed and the crop cut the album art off. They did not know how it had been configured and
asked for the new collection to "emulate that with the new tools". Cropped View gained *Whatever is
drawn*, which trims to the non-transparent part and follows it (one small readback a second, only in
that mode). The rebuild uses it for cropped web widgets, pinned by their bottom-left corner at the
place the old crop put them.

The owner then said they did not want a cropped widget at all: "i just need the component amuse scene
placed where the old cropped main was on all the scenes relevant". The generator's `SWAP_WIDGET` now
puts the Amuse Component widget, whole, where the cropped Amuse Main sat, centred on the middle of the
old strip (draft 4). *Whatever is drawn* stays in the plugin but the rebuild no longer uses it.

Draft 4 crashed the owner's OBS on import. Draft 4b, identical but without the 31 show/hide fades
copied from the original items, loaded. So the generator leaves fades off by default. The cause is not
known: it does not reproduce in the headless load check on Linux even with obs-transitions loaded, and
no crash report has been seen. Suspects: the copied transition entries themselves, or a StageKit filter
on a source whose item has a transition. Ask for the crash report before putting fades back.

## Arrange tools (0.1.0-beta24)

The owner asked for "intuitive transform and object editing tools" for alignment and snapping,
including "based on visible pixels, and not just the bounding box". The dock gained a box that lines
up, spaces out, matches sizes and snaps the items selected in OBS, measured by the drawn picture by
default. Not done: snapping while an item is being resized by its handles, rotation-aware edges, and
guides drawn in the preview (a plugin cannot draw into OBS's preview).

## Installer folder page (0.1.0-beta22)

The owner asked for the installer to "show and allow customizing the install directory". The folder
page is now always shown (`DisableDirPage=no`), with wording that OBS only loads plugins from its own
plugins folder. The script cannot be compiled or run on Linux: CI building the installer is the only
check, and nobody has seen the new page yet.

## Shadow sizes (0.1.0-beta21)

The owner imported rebuild draft 2 with beta19 and it loaded (Cropped Views included). First report:
a 20 px shadow looked much smaller on the camera than on the chat box. Cause: shadow sizes were in the
source's pixels, and the camera view is drawn at about 0.48 while the chat box is drawn at 1.29.
Shadow & Backdrop now sizes by the scale the item has in the scene on screen.

## Dock rework (0.1.0-beta20)

The owner said the dock's live scene picture was not what they meant. What they asked for, in their
words: a thumbnail for the start image; the start image as the ambient background by default; the
background "globally configurable on its own if desired"; the zoom "was to zoom in on the thumbnail,
not zoom a selected item"; and to "select an object in the main scene viewer, and have the stagekit
options apply to the selected item". The arrow buttons were for placing the camera, which they wanted
as a filter on the camera.

So: `dock-canvas.*` is deleted; the dock follows `obs_sceneitem_selected`; its keys are Send To filters
on the item's source; the thumbnail's wheel and drag set a collection-wide `AmbientView` (zoom, focus
point, optional picture of the background's own), saved under `stagekit.ambient_*`.

That reading of "zoom in on the thumbnail" was wrong. The owner: "zooming into the start image isn't
supposed to zoom into it in the actual scene viewport, just in the dock." From 0.1.0-beta23 the wheel
and drag change only the dock's thumbnail; `AmbientView` keeps only the optional picture, and its zoom
and focus are pinned to neutral so a value saved by beta20 or beta21 has no effect.

## Scene collection rebuild

The owner asked for their collection to be rebuilt on StageKit (129 scenes, 187 sources, 242 filters).
It is generated, not hand-edited, so it can be regenerated as the tools change:

```bash
python3 -I scripts/rebuild-collection.py <original.json> <rebuilt.json> <report.txt>
```

- The original export is the owner's private file. It is not in this repository and must not be
  committed; ask the owner for it. The script never writes to it.
- Each production scene is flattened: nested "Overlay" and "Parts" scenes, source clones and Move
  chains go, and the real sources are placed directly in the scene with StageKit filters. Hidden
  per-shape variants are dropped in favour of one game capture with Auto Capture and Auto Frame. The
  hand-built blurred backgrounds become one Ambient Background source.
- A source that was cropped and shadowed becomes a Cropped View (StageKit) source, because an item's
  crop is applied after its filters. Draft 1 used small wrapper scenes for this and the owner objected
  ("square camera is still embedded in another scene"); nothing in the rebuild may nest a scene.
- Draft 2 (2026-11-12): 57 sources, 15 scenes (only the production scenes), 65 filters. It needs
  0.1.0-beta19 for the Cropped View source.
- `tests/collection-loadcheck.cpp` loads a collection in headless libobs with StageKit and shows each
  scene once. It is not part of `run-tests.sh`. Build and run it by hand:

```bash
g++ -std=c++17 tests/collection-loadcheck.cpp -o /tmp/loadcheck $(pkg-config --cflags --libs libobs) -ldl
```

```bash
xvfb-run -a /tmp/loadcheck build-tests/stagekit.so data <rebuilt.json> "<scene to print>"
```

  On Linux the capture, browser and text sources are missing plugins, so it proves the file loads and
  the StageKit filters are created, not that the picture is right.
