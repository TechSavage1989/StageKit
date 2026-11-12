# StageKit: read this first

StageKit is a plugin for OBS Studio (C++17, libobs, Qt 6 for the dock). It adds filters
and one source that replace hand-built chains of Move filters, source clones and nested scenes.

Read `DEVELOPMENT_HANDOFF.md` next for the current state, then
[docs/REPOSITORY_STANDARDS.md](docs/REPOSITORY_STANDARDS.md), the shared standard that is identical in
every TechSavage1989 app repository. Change that file only to change the standard everywhere.

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
| `src/shadow/` | Shadow, Glow & Outline filter (id `stagekit_shadow_backdrop`, its first name): shadow or glow in a colour or the picture's own colours, outline, panel, rounded corners |
| `src/zoom/` | Ramp Zoom filter |
| `src/shape/` | Corners & Edges filter (rounds, cuts, notches or scoops corners; trims, softens and outlines edges; never changes the source's size) |
| `src/view/` | Cropped View source (part of another source as a source, so filters follow the crop) |
| `src/placement/` | Scene filters: Place Item, Show/Hide Animation, Visibility Link, Match Item, Layout, Fit Item (keeps one item fitted in a box, filling it or floating centred by shape); and Send To, Place Item's code as a filter on the source it moves |
| `src/dock/` | Two docks (Qt widgets, no `Q_OBJECT`): StageKit follows the item selected in OBS and drives Send To filters on that item's source; StageKit Background holds the start image thumbnail. `arrange-tools.*` is the one set of line-up actions and options behind the dock, the hotkeys and the preview's right-click menu |
| `src/common/` | Shared helpers: easing and `Tween`, `SwitchRamp`, `KawaseBlur`, scene utilities, and the start image (global path + Start Image source) |
| `src/frame/` | Auto Frame filter and the Picture Shape key filter |
| `src/capture/` | Auto Capture filter (aims a Game Capture at the game in front) and Program Trigger filter (shows items and switches filters while a program is open). Both use the Win32 window list in `auto-capture.cpp`; compiled but idle elsewhere |
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
- **A scene item's box is cached and can be a 1x1 placeholder.** An item never moved since it was added
  keeps that placeholder until its scene is drawn, and `obs_sceneitem_force_update_transform` only acts
  on items OBS has flagged. Call `refresh_item()` (`src/common/scene-util.hpp`) once before measuring at
  the start of an operation. Items inside groups use group coordinates: measure with `scene_box()` and
  convert with `item_space()`.
- **The owner's emulators run full-screen at 2560x1440 with black bars**; their old scenes differ only
  by crop (320 px a side for 4:3, 343 for SNES, 150 for GBA) inside one shared 1754.5x1268.5 box at
  30.5, 85.5. Auto Frame's automatic trim exists for this. It reads back a thin copy of the capture
  every 0.4 s, one direction at a time; keep that cost in mind before adding more readbacks.
- **Black bars and black game backgrounds look identical.** Auto Frame's automatic trim only trims more
  when the remaining picture is exactly a known console shape (`is_picture_shape`). Do not loosen that
  to a brightness-only rule, and do not drop the held shape (`held_ratio_`) on a resolution change: the
  owner wants no resize when a window goes full-screen at the same shape. Background: the owner hit
  it with Snes9x (Aladdin's title screen) and the picture resized repeatedly. The owner wants no manual steps when switching games; program rules and Picture
  Shape keys are the fallbacks.
- **Every list the user builds is a `PickList`** (`src/common/pick-list.*`): choose to add, one row per
  entry, each with its own remove button. The owner asked for one consistent control; do not add OBS's
  editable list or a comma-separated text box for a new list.
- **Long control labels push every control to the right** in OBS's filter window, which the owner
  rejected. Keep a label under about 26 characters and put the rest in a tooltip
  (`obs_property_set_long_description`).
- **Built-in emulator shapes are a starting guess, never an order.** The owner's Snes9x shows 8:7; the
  first table said 64:49 and cropped the picture. `kKnownPrograms` only seeds the held shape; what is
  seen on screen corrects it. Only a shape the owner saved, or a Picture Shape key, is obeyed blindly.
- **The dock carries no explanatory paragraphs.** The owner wants it clean: controls and short labels
  only. Put explanations in tooltips and the README.
- **The dock has no picture of the scene.** One was built (drag, wheel, zoom cross) and the owner
  rejected it: they select items in OBS's own preview, and the dock must act on that selection. The
  wheel-zoom they asked for belongs to the start image thumbnail and changes only that thumbnail in
  the dock: never an item, never the background in the scene.
- **The arrange tools measure what is drawn, not the box** (`src/dock/arrange.*`). They draw the
  item's source small and read it back to find the non-transparent part. That happens once per button
  press or drop, never per frame; keep it that way. OBS's own drag-snapping cannot be hooked, so "snap
  when I let go" watches the selected items and corrects after the mouse button is up.
- **The preview's right-click menu is not an OBS API.** `PreviewMenu` in `src/dock/stagekit-dock.cpp`
  watches for a context-menu event on the widget named `preview` and adds to the next menu that opens.
  If OBS renames that widget the menu entry silently disappears; the hotkeys and the dock still work.
- **Arrange options are global** (`arrange_options()`), saved with the scene collection. The dock's
  controls only show and set them. Add a new tool to `ArrangeAction` and `kInfo` and it appears in the
  dock, the menu and the hotkeys at once.
- **Edits made by a person go into OBS's undo list** (`add_transform_undo` in
  `src/dock/arrange-tools.*`): the arrange tools, snap on release, the dock's Send to buttons. Moves
  fired by filters and Stream Deck keys do not; they are part of the show.
- **Dock keys are Send To filters on the item's source**, not Place Item filters on the scene. The
  owner presses them from Stream Deck Filter keys on the camera.
- **Shadow sizes are in screen pixels by default.** The owner's old shadows sat on full-canvas scenes,
  so 20 px was 20 px everywhere; a filter on a scaled item is not. The filter divides its sizes by how
  big the item is drawn in the scene on screen (`find_shown_scale`).
- **Shadow, Glow & Outline moves scene items on purpose.** The shadow's room makes the source bigger; the
  filter shifts each item showing the source (and grows its bounding box) by that room so the picture
  stays put, and remembers what it applied in `adjusted_*` settings. Anything else that changes the
  room around a source must go through `keep_picture_in_place`, not around it.
- **Corners & Edges must never change a source's size.** Everything that makes it safe beside the
  other tools rests on that. It belongs before Shadow, Glow & Outline in the filter list (the shadow reads
  alpha, so it follows the shape); when first added it moves itself above an existing shadow once and
  records that in its `placed` setting. After an Auto Frame it shapes the rectangle from Auto Frame's
  `get_picture`.
- **Trigger filters** (Send To, Place Item, Layout) switch themselves off after running and ignore the enabled
  state restored at load (`ready_`), so they do not fire when OBS starts.
- **Auto Capture's Windows code is only compiled by CI and never run by the tests.** The tests feed it
  stand-in windows through its `set_test_windows` call. The owner wants no list of games to maintain:
  detection is by loaded modules (graphics engine plus controller support, or full-screen), with a
  *Never capture* list for mistakes. Do not make a program list the default again.
- **The owner triggers camera moves from Stream Deck Filter keys on the camera source**, not from the
  dock's buttons. Send To exists for that; keep its controls complete without the dock.
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
- Standing approval, given 2026-11-12, for the scene-collection rebuild only: merge each change once
  its checks pass and publish it as the next pre-release without asking, then tell the owner it is
  live. It does not cover a stable (non-beta) release, changes to repository visibility or access, or
  work unrelated to the rebuild. See the handoff's task history.
- AI coding sessions cannot push tags or create, edit or publish releases here; the platform refuses
  them. Releases are published by `.github/workflows/push.yaml` when a new `docs/releases/<version>.md`
  reaches `main` (steps in the handoff). Never ask the owner to create tags by hand.

## Talking to the owner

Routine progress is plain, short text. Two markers are reserved and go at the top of a message in bold:

- `⚠️ ACTION NEEDED:` when the owner must decide, approve, supply something, or check something on
  their own machine. Say exactly what to do, where, what to report back and what is blocked. One
  request per line. Repeat an unanswered request in the next update.
- `🚀 PUBLISHED:` when a release is live and its assets are verified, followed by the link and a
  one-line summary.

Use them nowhere else. End every message with a short status: what is done, what is still running, and
what the owner needs to do, if anything.
