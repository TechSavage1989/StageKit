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
| `src/capture/` | Auto Capture filter (aims a Game Capture at the game in front), Program Trigger filter (shows items and switches filters while a program is open) and Active App scene filter (shows the one of several captures whose app is in use). All use the Win32 window list in `auto-capture.cpp`; compiled but idle elsewhere. Also Standby Picture (`standby.*`), a filter that shows a picture in a capture's place while it has nothing; it works everywhere |
| `src/api/` | obs-websocket vendor requests (`StageKit` vendor) |
| `src/third-party/` | Vendored `obs-websocket-api.h`, kept byte-for-byte (excluded from clang-format) |
| `data/effects/` | Shaders in OBS `.effect` format |
| `data/locale/en-US.ini` | Every user-visible string |
| `tests/` | Headless libobs tests, test-image generators, `run-tests.sh` |
| `scripts/` | Repository checks (`check-root.sh`, `root-allowlist.txt`); `rebuild-collection.py` writes a StageKit collection from an old one and `upgrade-collection.py` brings a StageKit collection up to date. Both read one file and write another; the owner's collections are private and never committed |
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
  when the picture is seen to end in a straight line, the same distance in from both sides, and what is
  left is exactly a known console shape (`is_picture_shape`). Do not loosen that to a brightness-only
  or shape-only rule, and do not drop the held shape (`held_ratio_`) on a resolution change: the
  owner wants no resize when a window goes full-screen at the same shape. A shape is held only once
  the picture has been seen to fit it (`Bars::confirmed`); one taken from a black screen put a square
  crop on a widescreen game. A first sight may also be believed without straight edges when the
  picture *fits the capture* (`fitted`: both ends reached one way, the same distance in from both ends
  the other way, a real shape): the owner's dark modern games gave no straight edge for a long time.
  A shape a program has held is remembered (`learned`) and is where that program starts next time; it
  is a starting point like the built-in ones, never an order. Background: the owner hit
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
- **Guides are child windows inside OBS's preview.** `src/dock/preview-guides.*` puts one thin,
  opaque native child window (`LineWindow`, `WA_NativeWindow`, parent: the widget named `preview`)
  per line, and repeats OBS's own sums (`preview_map`: a 10 px edge, then the canvas as large as fits)
  to find the canvas in it. That only holds for *Scale to Window*. Every design with windows *over*
  the preview failed on the owner's Windows machine: one see-through window (beta46 to beta49), then
  one separate tool window per line (beta50 to beta54), which the dock reported as on and the owner
  never saw. Do not go back to top-level windows. The mouse button is also read from Windows itself.
  Guides are off by default and lines are only shown while one is up; switching guides on flashes a
  frame round the canvas. Items are measured once when a drag starts, never while it goes on. The
  owner had OBS crash while moving items with guides and snap on, and not with both off (beta53).
  OBS names four widgets `preview` (the main one, and one each in the Filters, Properties and
  Interact windows); the main one is picked by its class, `OBSBasicPreview`. Child windows inside the
  preview attached to the wrong one were not seen (beta55); attached to the right one the owner saw
  them (beta56). `guides_report()` says what the guides did, for a log line or a message if they
  stop showing.
- **The order items are selected in comes from OBS's own `item_select` signals**
  (`track_selection_order` in `src/dock/arrange-tools.cpp`), on every scene and group. It used to be
  learned by polling one scene, which forgot it whenever the dock's *Scene* list showed another scene,
  so "last selected" became the top-most item. The dock's Arrange tools act on the scene being
  edited (`arrange_scene()`), not on the dock's *Scene* list, unless nothing is selected there.
- **Ignore shadows & outlines measures the source's own pixels** (`measure_item`): the source drawn
  with none of its filters (`render_bare`), placed where the Shadow, Glow & Outline room puts it, and
  then only where something is also drawn with the filters on, so what a filter takes away (a mask,
  trimmed edges) does not count. The owner asked for that: anything a filter draws (a Corners & Edges
  border, say) is left out. Draw bare with `render_bare`, never `obs_source_default_render`: that one
  is only right for plain sources, not for one that draws itself (a browser, a nested scene). A
  source that plays video frames (`OBS_SOURCE_ASYNC`: a camera, a video) is never drawn bare: the
  owner had crashes in exactly the versions that did (beta53 to beta55), cause unproven. When the
  filters change the size by more than that room (an Auto Frame) it falls back to what the first
  shadow draws from (`shadow_free`), then to cutting the room off the edges. Never cut the room off
  the edges first: a widget's picture often stops short of its own edges. Unticked, a shadow sized in
  screen pixels keeps its size while the item is resized, and `match_size` allows for that.
- **Measuring draws on OBS's drawing thread and never waits long for it** (`on_drawing_thread` in
  `src/dock/arrange.cpp`). Up to beta53 it drew sources from the window's thread while OBS drew them
  for the picture. Beta54 to beta56 handed the work to the drawing thread and waited without limit;
  the owner's OBS then froze for good while dragging with guides on (no crash file: a deadlock). Now
  the wait is `kPatienceMs`; after that the caller goes by boxes (a slide says *OBS was busy*) and the
  work, which holds its own references (`Held`), finishes later by itself. Never wait without a limit
  on another OBS thread from the window's thread, and never capture a caller's locals by reference
  in work handed over this way.
- **Everything that runs on a timer stops when OBS closes.** `register_dock` listens for
  `OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN` (sent before OBS clears its scenes) and `EXIT`, calls
  `measuring_stop()`, deletes the guides and removes the preview menu's event filter; the docks'
  timers check `measuring_stopped()`. Before beta58 they all kept asking OBS about scenes and
  measuring while OBS took the scenes apart, and the owner's OBS often froze on close. Anything new
  that polls or listens for the whole session must stop there too.
- **A smoothed edge's faint last row of pixels is not picture** (`measure_drawn`): where every pixel
  in the outermost row or column is under half the one inside it, the picture ends one pixel further
  in. Counting it left a line of background along the screen's edge at an edge distance of 0.
- **Groups are items like any other.** A selected group is the dock's item and is moved, slid and
  copied by *Copy transform to all scenes* as one thing; only when the group itself is not selected do the
  tools look at what is selected inside it. Do not skip groups in a new tool.
- **Every tool that places things measures by the Arrange options.** The owner: "i need the tools to
  respect all the settings". Send To and Place Item's nine screen positions keep their edge distance
  to what `arrange_options().how` counts as the item (`as_arranged`, on by default; off goes by the
  box). A new tool that places or sizes items must do the same.
- **A brought-along item flips round the leader's picture** (`plan()` in `src/placement/place-item.cpp`),
  measured without shadows, not round the leader's box: round the box, the camera's shadow added its
  width to the gap after a flip (the owner's screenshots, beta51).
- **Copy transform to all scenes** (first called *Same in every scene*, which the owner found
  ambiguous; `copy_to_every_scene` in `src/dock/arrange.*`, action `same_everywhere`) copies an item's transform
  to every copy of its source in the other scenes, converting position, scale and bounds through each
  copy's group (`item_space`), skipping locked copies and groups listed as scenes.
  *Copy transform to chosen scenes* (`same_chosen`) does the same for the scenes named in
  `ArrangeOptions::copy_scenes` only, a list built in the dock the PickList way (choose to add, a
  row each with a remove button) and saved with the collection. Name a tool for what it does, verb
  first; the owner asked for labels that say what happens.
- **Arrange options are global** (`arrange_options()`), saved with the scene collection. The dock's
  controls only show and set them. Add a new tool to `ArrangeAction` and `kInfo` and it appears in the
  dock, the menu and the hotkeys at once.
- **Edits made by a person go into OBS's undo list** (`add_transform_undo` in
  `src/dock/arrange-tools.*`): the arrange tools, snap on release, the dock's Send to buttons. Moves
  fired by filters and Stream Deck keys do not; they are part of the show.
- **Slide to the next edge** (`slide_offset` in `src/dock/arrange.*`) stops the leading edge `gap`
  short of the next edge ahead: the near side of an item in the path, then (next press) its far side,
  then the screen's edge, never past it. The owner asked for exactly that: stop at the first edge,
  pass through on the next press. A press made while the item is still gliding starts from where the
  last slide ends. The dock sends the result as a Send To to *a spot on the screen*
  (`Destination::Spot`, the box's top-left in canvas pixels), so with *In every scene* the item lands
  on the same spot of the screen everywhere, grouped or not. Do not send a slide as a saved spot: that
  is the item's own position, which means something else inside a group.
- **Send To's "every scene" skips groups in `obs_enum_scenes`**: OBS lists each group as a scene too,
  and reaching a grouped item that way moved it a second time as if the group were the screen.
- **Dock keys are Send To filters on the item's source**, not Place Item filters on the scene. The
  owner presses them from Stream Deck Filter keys on the camera.
- **Active App follows the app in use, and nothing else.** The owner's old macros ran while a listed
  program had the focus: the capture follows from one listed app to another, stays on the last one
  while anything else is in front, and goes back to an earlier capture when an app closes. It acts on
  events (an app opens, closes or is clicked into), never continuously, so a capture shown by hand is
  kept. The change is OBS's own show and hide transition on each item, which the filter sets; do not
  build a second fade.
- **Active App's captures switched to by hand** (`by_hand`) are how the owner holds a window that is
  not one of the automatic apps: showing one (any way OBS shows an item: a Stream Deck source key, the
  item's own hotkeys, the eye) hides the automatic captures and stops the following until it is
  hidden. No new key type was added on purpose; the two hotkeys on the scene are extras. Before this
  the owner aimed a capture at the foreground window with a Stream Deck key and lost it as soon as a
  listed app was clicked into.
- **Standby Picture goes by the capture's own size** (`obs_source_get_base_width(parent)` is 0 when
  nothing is attached), never by how the picture looks: the owner agreed a black loading screen is
  not "nothing". For what comes before it in the filter list it must ask through `before_width`:
  asking the capture for its width comes straight back to this filter (it crashed the first build).
  The start image is its default picture, as the owner asked. It is switched off like any filter;
  no separate enable setting.
- **Send To moves the source in every scene it is in, by default** (`apply_in`). The owner asked for
  that: the camera sent to a spot is there in every scene. The older `where` setting is not read.
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
- **Fit Item gives OBS the space, not the size.** Its bounding box is the space the picture may take
  (plus a shadow's room at the fitted scale), with scale-to-inner-bounds and centre alignment, so OBS
  fits the picture every frame and a window being resized never lags. Do not go back to setting the
  item's scale or a box of the picture's exact size: both are one tick late.
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
- **Every filter and source that draws is flagged `OBS_SOURCE_SRGB`.** Without it OBS forces
  `gs_get_linear_srgb()` off for that filter, and where a window has a wide-colour (16-bit float)
  surface the filter writes ordinary values into linear light: the picture is pale. The owner saw
  that in the Filters window, on a standard display with HDR off, for Shadow, Glow & Outline and
  Auto Frame, the two that lacked the flag. The tests draw each filter onto such a surface
  (`render_shown`); add a new filter to that list.

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
