# StageKit: read this first

StageKit is a plugin for OBS Studio (C++17, libobs, Qt 6 for the dock). It adds filters
and one source that replace hand-built chains of Move filters, source clones and nested scenes.

Read `DEVELOPMENT_HANDOFF.md` next for the current state, then
[docs/REPOSITORY_STANDARDS.md](docs/REPOSITORY_STANDARDS.md), the shared standard that is identical in
every TechSavage1989 app repository. Change that file only to change the standard everywhere.

## Names

The owner asked for names "to make more sense" and approved a table (2026-11-14, beta87). What a
user sees changed; ids, settings, folders and code names did not. Old name, then the name shown:
Ramp Blur: Blur. Ramp Zoom: Zoom. Send To: Move Source. Place Item: Move Scene Item. Layout: Apply
Layout. Fit Item: Fit to Space. Visibility Link: Link Visibility.
Picture Shape key: Aspect Ratio Preset. Stream Deck keys: Picture shape: Set Aspect Ratio. Hook
capture: Capture Front Window. Window switch: Next Manual Capture. Dock switch: Dock Option. Slide
to next edge: Slide to Edge. Blur everything: Blur All. A key's press is *On press*: *Toggle*,
*Switch on*, *Switch off*. The handoff's history and the code still use the old names; a filter
already on a source keeps the name it was given.

A second table, in OBS's own terms, was put to the owner and taken whole (2026-11-15, beta119: "use
all the recommended names you proposed"). **The rest of this file still uses the names from before
it.** Then and now: Capture App in Use: Automatic Window Switcher. Show Source in Use (first Follow
Active App): Automatic Source Switcher. Apps to capture: Windows to switch between. Follow the app
in use: Switch to the active window. Apps you switch to / Captures you switch to: Manual windows /
Manual sources. Switch to the next of these: Switch to next manual window. Back to following the
apps: Return to automatic. Keep the others ready: Keep other windows captured (Keep other sources
ready in the scene filter). Change with / Lasting, in every filter: Transition / Duration; Show
with / Hide with: Show transition / Hide transition. Hiding and showing it: Show/Hide transition.
Stream Deck keys: Next Manual Capture: Manual Window / Source. Capture Front Window: Capture Active
Window.

A third table, for control labels, was approved on 2026-11-15 (beta124) with two changes of the
owner's: *picture* becomes *image* and *item* becomes *source*, "to align with more
professional/technically accurate verbiage for OBS", and "the verbiage should be concise". Then
and now: Items never in the way: Ignored sources. Fit it in: Fit area. The free space around it:
Free space around source. Gap to edges and items: Gap. Glide to a new space over: Move duration.
Resize it to fit: Resize to fit. Keep it towards: Align in the space. Same place in these scenes:
Sync position to scenes. Tolerance: Size tolerance. Bring along: Linked source. Send it to:
Destination. Travel time: Duration. Comes in from / Time to come in / Time to leave: Direction /
Show duration / Hide duration. Ramp style / Ramp in time / Ramp out time: Blur transition /
Blur-in duration / Blur-out duration. Zoom in time / Zoom out time: Zoom-in duration / Zoom-out
duration. Blend in live capture: Live capture. Live capture amount: Live capture opacity. Bloom
from brightness: Bloom threshold. Picture shape: Aspect ratio. Saved shapes: Saved aspect
ratios. Hold a shape for: Keep aspect ratio for. Made from: Colour source (its *The picture
itself*: The image itself). Sizes as they look on screen: Sizes in screen pixels. Standby
Picture's Show or hide the picture now: Cover the capture now; To the capture / Back to the
picture: Duration: image to capture / capture to image; Picture keeps its shape: Keep image's
aspect ratio; Size glides too: Animate the size change. Window switcher's Hide to the Standby
Picture: Hide shows Standby Picture; Hide or show it now: Hide/show capture now. Dock: Send to:
Move to; When I let go of an item: On drop. Names of filters and keys were not part of it.

A fourth, at the owner's word (2026-11-16, beta135): "I would like Start image change throughout
the plugin and settings as Standby Image", and then "rename everything with the word picture to
image as previously requested". So *Start image* is shown as *Standby image*, and the word
*picture* is *image* in every string a user reads: `data/locale/en-US.ini` (values, never keys),
the README, and the Stream Deck plugin's wording (the *Image* list on every key, the *Standby
Image* key). That includes filter names: Standby Picture is shown as *Standby Image (StageKit)*.
Ids (`stagekit_standby`, `stagekit_start_image`, `SetPictureShape`), settings, pack file names and
the code are unchanged, and **the rest of this file still says picture and start image**. Write
*image* in any new string.

## What StageKit is not

It is an OBS plugin, not a standalone desktop app: it has no window or executable of its own. It
does have an updater, because the owner asked for one (2026-11-14: "can you build an automatic
updater? ... or build into the obs plugin an updater feature?"): a notice in the dock and a one-press
install, in `src/update/`. The shared standard's download bar and install dialog are not built; the
notice and the confirmation are what there is. This is recorded in the handoff.

## Folder map

| Path | What lives there |
| --- | --- |
| `src/plugin-main.cpp` | Module entry: registers every filter and source, global hotkeys, the obs-websocket vendor |
| `src/blur/` | Blur filter (owns its own multi-type blur pipeline: soft blurs, a mosaic in seven cell shapes, zoom, motion and a trail over time) |
| `src/ambient/` | Ambient Background source |
| `src/shadow/` | Shadow, Glow & Outline filter (id `stagekit_shadow_backdrop`, its first name): shadow or glow in a colour or the picture's own colours, outline, panel, rounded corners |
| `src/zoom/` | Zoom filter |
| `src/shape/` | Corners & Edges filter (rounds, cuts, notches or scoops corners; trims, softens and outlines edges; never changes the source's size) |
| `src/view/` | Cropped View source (part of another source as a source, so filters follow the crop) |
| `src/placement/` | Scene filters: Move Scene Item, Show/Hide Animation, Link Visibility, Match Item, Layout, Fit to Space (keeps one item fitted in a box, filling it or floating centred by shape); Move Source, Move Scene Item's code as a filter on the source it moves; and Show in Scenes (`show-in-scenes.*`), a filter on any source that is a switch for showing and hiding sources across scenes |
| `src/dock/` | `window-layout.*` keeps OBS's window layout. Two docks (Qt widgets, no `Q_OBJECT`): StageKit follows the item selected in OBS and drives Move Source filters on that item's source; StageKit Background holds the start image thumbnail. `arrange-tools.*` is the one set of line-up actions and options behind the dock, the hotkeys and the preview's right-click menu |
| `src/common/` | Shared helpers: `timing` (registers every kind and logs what is slow), easing and `Tween`, `SwitchRamp`, `KawaseBlur`, scene utilities, `item-transition` (the *Change with* control for filters that show and hide items), and the start image (global path + Start Image source) |
| `src/frame/` | Auto Frame filter and the Aspect Ratio Preset filter |
| `src/capture/` | Auto Capture filter (aims a Game Capture at the game in front), Program Trigger filter (shows items and switches filters while a program is open) and Show Source in Use scene filter (shows the one of several captures whose app is in use). Capture App in Use (`app-capture.*`), a filter on one Window Capture that aims it at whichever of several listed apps is in use. All use the Win32 window list in `auto-capture.cpp`; compiled but idle elsewhere. Also Standby Picture (`standby.*`), a filter that shows a picture in a capture's place while it has nothing; it works everywhere |
| `src/api/` | obs-websocket vendor requests (`StageKit` vendor) |
| `src/transition/` | Morph, a scene transition: items showing the same source in both scenes glide from one place to the other, the rest cross-fades (`morph.*`); the same between two items of one scene, one hidden as the other is shown (`item-morph.*`); Fade and Zoom, a transition for scenes and single items (`fade-zoom.*`); what the three draw with (`morph-paint.*`) |
| `src/update/` | The updater: reads the releases from GitHub, tells the dock about a newer one, and on Windows installs it through a PowerShell helper that outlives OBS |
| `src/third-party/` | Vendored `obs-websocket-api.h`, kept byte-for-byte (excluded from clang-format) |
| `data/effects/` | Shaders in OBS `.effect` format. `stagekit-looks.effect` holds the newer looks (mosaic shapes, trail, bloom) apart from the older files |
| `data/locale/en-US.ini` | Every user-visible string |
| `tests/` | Headless libobs tests, test-image generators, `run-tests.sh` |
| `streamdeck/` | The Stream Deck plugin (TypeScript, Elgato's SDK 3, Node): `src/` (the link to OBS over obs-websocket, the key types), `com.techsavage1989.stagekit.sdPlugin/` (manifest, settings pages, icons), `tests/`, `tools/make-icons.py`. It has its own `package.json`; nothing in the OBS plugin depends on it |
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
  second copy. Blur still has its own older copy of this logic.
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
- **A program can be named by part of its window's title wherever it is named by file.** The owner
  runs Dolphin (4:3) and PrimeHack, a build of it with the same `Dolphin.exe` (16:9): "some
  applications i have are the same exe name but they are different versions and need different
  rules". Auto Frame's saved shapes are looked for in the file name and the title, and the longest
  name that fits wins (`said`); its *Program or window* list offers the open windows' titles.
  Show Source in Use gives each of several captures of one program the window whose title starts
  most like the title the capture was aimed at (`is_for`). Auto Capture's two lists match the title
  too. Program Trigger had a title box already. What Auto Frame *learns* by itself is still kept by
  file name only; a saved shape for the title is how the two are told apart.
- **Built-in emulator shapes are a starting guess, never an order.** The owner's Snes9x shows 8:7; the
  first table said 64:49 and cropped the picture. `kKnownPrograms` only seeds the held shape; what is
  seen on screen corrects it. Only a shape the owner saved, or an Aspect Ratio Preset, is obeyed blindly.
- **The StageKit dock is four tabs** (`stagekitTabs`: Place, Arrange, Copy, Effects) under the
  *Scene* and *Item* rows, with the status line below them. It was one column 1,460 px tall; the
  owner called it "ugly and messy" and asked for it "organized, and made visually appealing".
  A new control goes on the tab its job belongs to, not in a new box at the bottom. The line-up and
  order tools are drawings alone (`icon_only`, name and use in the tooltip; the line-up tools a block three across and four down since beta132, with *How the tools measure* beside it, as the owner laid it out: "align left to align right on top, then align top to align bottom under that row ... then how tools measure can fit to the right") and the
  Place arrows a block of 40 px squares with the tab's tick boxes beside them: the owner asked for
  "just icons with tooltips, making space for more functions" and found the three-dot update menu
  unreadable, so that became the word *Updates*; since beta132 it is not in the dock at all: the
  update notice (`updateBar`) is the dock's first row, shown only while a newer version is there,
  and the menu (`updateMenu`: version, check now, the two settings, *Update to … now*) hangs under
  *StageKit* in OBS's Tools menu (`obs_frontend_add_tools_menu_qaction`). The owner: "I need the
  updater to appear at the top, and only when there is an available update. otherwise it should be
  contained in the tools menu". A check asked for there answers in a message box when there is
  nothing newer. The four order tools are a block two by two (the two upward ones on top), each with a drawing of its own since beta137: an arrow to a line for all the way, an arrow past one sheet for one step; as two overlapping sheets the owner found them "too similar. they need to be completely distinguishable". A test fails if two of them are alike. The copy tools keep their words. The
  drawings (`tool_icon`) are in the theme's own text colour; no stylesheet is
  set, so OBS's themes keep styling it. Widgets on a tab that is not showing have no geometry: a
  test that measures positions must switch to that tab first.
- **StageKit keeps OBS's window layout** (`src/dock/window-layout.*`, beta138): `QMainWindow`'s
  `saveState` as base64 in `window-layout.json` in the module's config folder, with *Restore it
  when OBS starts* (`at_start`, on) and *Save it when OBS closes* (`at_exit`, off), all in the
  Tools menu's StageKit entry. It is brought back `kAfterLoadingMs` (1.5 s) after
  `OBS_FRONTEND_EVENT_FINISHED_LOADING`, and saved at `SCRIPTING_SHUTDOWN`, before OBS takes its
  window apart. The owner had dock-layout plugins and still lost the layout at every start: "I'd
  like stagekit to also have a tool to store my OBS dock and layout, and also auto load it at
  startup". **Why OBS lost their layout was not found**; this restores it afterwards whatever the
  cause. Only the docks' state is kept, not the main window's size and place, which OBS restores
  itself.
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
- **Each filter has its own measuring options; the dock's are the dock's.** First every placing
  tool followed the dock's Arrange options (`as_arranged`; the owner: "i need the tools to respect
  all the settings"). Then, on beta70: "those options should only apply to the actively edited items
  ... the filters needs their own separate toggles". So Move Source, Move Scene Item and Fit to Space each have
  *Use visible pixels* and *Ignore shadows & outlines* (`visible_pixels`, `no_shadows`, both on by
  default; an old `as_arranged` unticked reads as boxes). The dock writes its own two options into
  the Move Source filters it drives. A new filter that places or sizes items gets the same two tick
  boxes; a new dock tool uses `arrange_options().how`.
- **A brought-along item flips round the leader's picture** (`plan()` in `src/placement/place-item.cpp`),
  measured without shadows, not round the leader's box: round the box, the camera's shadow added its
  width to the gap after a flip (the owner's screenshots, beta51).
- **Copy transform to all scenes** (first called *Same in every scene*, which the owner found
  ambiguous; `copy_to_every_scene` in `src/dock/arrange.*`, action `same_everywhere`) copies an item's transform
  to every copy of its source in the other scenes, converting position, scale and bounds through each
  copy's group (`item_space`), skipping locked copies and groups listed as scenes.
  *Copy transform to chosen scenes* (`same_chosen`) does the same for the scenes named in
  `ArrangeOptions::copy_scenes` only, a list built in the dock the PickList way (choose to add, a
  row each with a remove button) and saved with the collection. Both go to the sources named in
  `ArrangeOptions::copy_sources` instead when that list is not empty (the owner: "when not
  specifically set, it only applies to the same source in other scenes"); the transform is copied as
  it is, like OBS's Paste Transform, so a source of another size ends up another size. Name a tool for what it does, verb
  first; the owner asked for labels that say what happens.
- **The copy tools hand a copy over to its Fit to Space** (`fit_item_placed` in
  `src/placement/fit-item.*`, called from `copy_to_every_scene`): the filter takes the item where
  it was put (its space is looked for there, and a kept size is read from it) and does not put it
  back. Before, a copy in a scene with a Fit to Space was reverted at once, above all in scenes not
  on screen, where the filter only repeats its last placing; the owner found the copy tools worked
  only on copies "unlocked and selected manually in each affected scene". Anything else that places
  an item on purpose across scenes should call it too. *Also change locked copies* (`copy_locked`,
  off as it comes) lets both copy tools change locked copies, which stay locked.
- **Snap on release and the resize limit glide into place** (`step_glides` in
  `src/dock/stagekit-dock.cpp`): the correction is worked out at once (and that is what Undo holds),
  the item is put back where it was let go and travels from there over the dock's glide time and
  curve. Taking hold of an item again ends its glide where it is. The owner: "snap and resize should
  glide if able".
- **Stop resizing at edges acts on release** (`limit_resize` in `src/dock/arrange.*`, called from the
  dock's `watch_for_drop`). OBS's own resize drag cannot be hooked, and correcting an item while the
  mouse still holds it fights OBS's next mouse move, so the item is pulled back when the button is
  let go, like snap on release. Only sides that were pulled outward are limited, only by things that
  were ahead of them when the drag began, and an item that was only moved is left to snapping. The
  owner asked for the limit to hold *while* dragging; this is the nearest that works, and they were
  told so.
- **With "Use visible pixels", an item that draws nothing is not there** (`measure_item_now`'s
  `nothing`). It is no obstacle to a slide or a resize and gives no line to snap to or guide to
  show. The owner's music widget shows nothing between songs and still stopped a resized window
  455 px short. Its box is still what a tool gets when asked to move that very item.
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
  last slide ends. The dock sends the result as a Move Source to *a spot on the screen*
  (`Destination::Spot`, the box's top-left in canvas pixels), so with *In every scene* the item lands
  on the same spot of the screen everywhere, grouped or not. Do not send a slide as a saved spot: that
  is the item's own position, which means something else inside a group.
- **Move Source's "every scene" skips groups in `obs_enum_scenes`**: OBS lists each group as a scene too,
  and reaching a grouped item that way moved it a second time as if the group were the screen.
- **Dock keys are Move Source filters on the item's source**, not Move Scene Item filters on the scene. The
  owner presses them from Stream Deck Filter keys on the camera.
- **Show Source in Use follows the app in use, and nothing else.** The owner's old macros ran while a listed
  program had the focus: the capture follows from one listed app to another, stays on the last one
  while anything else is in front, and goes back to an earlier capture when an app closes. It acts on
  events (an app opens, closes or is clicked into), never continuously, so a capture shown by hand is
  kept. The change is OBS's own show and hide transition on each item, which the filter sets; do not
  build a second fade.
- **Show Source in Use's captures switched to by hand** (`by_hand`) are how the owner holds a window that is
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
  It owns its size: while waiting, the picture's own shape (`own_shape`); with a capture, the size
  of what comes before it; and it always glides between sizes itself (`cur_w_`, `glide_`), never
  follows what comes before it directly. The first build filled the capture's last size and followed
  the filter before it: the owner's start image was cropped, jumped shape the moment an emulator was
  recognised, cut to the game, and stayed the game's shape afterwards.
  It must come after an Auto Frame and checks that twice a second (`order_check_`), not only when
  first added: before it, Auto Frame trimmed the picture as if it were a game (the same four
  complaints again, beta67). While it stands in, Auto Frame's `get_picture` reports no picture
  (`get_standing`), so Corners & Edges and the rest shape the whole picture. The capture is always
  drawn by way of a copy (`render_live_`), kept when the capture ends so the change back starts from
  the last frame: a capture that has ended draws nothing.
- **Standby Picture can stand in for a hidden source** (`when_hidden`, *Show when source is
  hidden*, off as it comes; beta139). The owner: "I need a way for the standby image to be displayed
  in place of any source I choose when it's hidden". A hidden scene item draws nothing, so each
  item showing the source, in every scene (not in groups yet), gets a twin item straight under it
  (`sync_stand_ins`, from the tick four times a second): locked, placed like the item while the
  item shows (`place_like`), visible exactly while the item is hidden, with the filter's item
  transition. The twin's source is private (`stagekit_stand_in`, `OBS_SOURCE_CAP_DISABLED`): it
  draws the source itself with `t_stand_in` up, and the Standby filter then draws its picture
  alone, so the picture has the source's size and every later filter. `get_standing` says
  standing while that flag is up, so Auto Frame's `get_picture` and Corners & Edges treat it as
  the whole picture. The twin knows its item by the item's id in its private settings
  (`stagekit_stand_in_for`); twins are taken out when the option or the filter goes, and a private
  source's items are not loaded back by OBS, so they are made again at each start. Anything that
  lists a scene's items will see them, as "<source> (standby)".
- **Nothing goes through every scene's items while OBS loads them** (`scenes_loaded()` in
  `src/common/loading.*`: true from two seconds after `FINISHED_LOADING` or
  `SCENE_COLLECTION_CHANGED` until `SCENE_COLLECTION_CHANGING` or shutdown; always true in the
  tests, which have no OBS window). While a collection loads, filters are already ticked but
  only some scenes have their items. Two faults came of acting then:
  - Stand-in items, made in a Standby filter's first tick (up to beta142), were in the scenes
    when another plugin went through them in its load step (`obs-quick-access-utility.dll`,
    `LinkSceneItem`), and it crashed on an item showing a private source: the owner's OBS would
    not start. Since beta143 it starts with that plugin on (the owner: "obs loads with quick
    access enabled"). Whether OBS crashed depended on how fast it loaded.
  - Shadow, Glow & Outline, not yet knowing how big its source is drawn, gave its room back to
    the items there were (as if drawn full size) and then took it again from all of them
    (`settle_items`). An item in a scene loaded later got the room twice: the owner's camera
    (*DSLR Component*) was 11 px larger after every restart in two scenes of three, exactly the
    room at that item's scale. The third scene hid it, because its Fit to Space put the item
    right each time; in the other two that filter's *Source* was empty. Fixed in beta144: the
    filter's tick returns before anything is adjusted until `scenes_loaded()`.
  A new filter that adds, removes or adjusts scene items from its tick asks `scenes_loaded()`
  first.
- **Fit to Space follows a renamed source** (`on_source_rename` in `src/placement/fit-item.cpp`,
  beta144: `item`, `keep_item`, `orig_item`). The owner's camera source had been renamed and
  two of its three filters still named the old one, so they did nothing. No other filter
  follows a rename yet; give one the same when it is asked for.
- **The change between a source and its stand-in is one transition, the Standby filter's**
  (beta141). The owner: "the dslr standby works, but I want it to morph or perform transition
  that I choose. currently it seems to just slide": the slide was the item's own hide transition,
  set by another filter, and the stand-in only faded, up to a quarter second late. Now the filter
  has the fuller *Show/Hide transition* (`add_item_change_properties`, no source list) and sets
  it on the item and, the two ways swapped, on its twin (`apply_item_change_as_owner`), ahead of
  time. With Morph both transitions get a link "<source> ↔ <source> (standby)" on top of the
  links made in the filter, so the item Morph pairs them. The twin's visibility follows its item
  every tick (`pairs_`, held from the last look), inside Morph's 0.1 s. And while it is ticked
  nothing else sets those items' transitions: `item_transitions_owner` (`g_stood_in`) makes
  `set_item_transitions` and `apply_item_change` pass them over. A new filter that sets item
  transitions must go through those two calls. Seen in the tests with a fade and by the
  transitions set; a Morph between the two on screen has not been looked at on Windows.
- **Every StageKit filter takes its input afresh at each drawing** (Shadow, Glow & Outline and
  Corners & Edges since beta142; Blur, Zoom and Auto Frame since beta146, at the owner's word:
  "yes, make blur, zoom and auto frame safe too"; `draw_filter_input` in
  `src/common/filter-input.hpp`), not through
  `obs_source_process_filter_begin`. OBS's way fills a texture of the filter's that is only
  cleared in the filter's tick, so a source drawn twice in one frame comes out the same both
  times. A source and its stand-in are both on screen for the whole of a transition between
  them, and with a shadow or corners after the Standby filter both showed whichever was drawn
  first: the owner's "most of the transitions don't work. they just instantly switch in place
  ... the standby image flash then change when showing". Beta141's tests had no later filter.
  Two other ways were tried and cannot work: a copy of the stand-in's look made in the filter's
  tick or in OBS's tick callback either keeps the source's picture from the frame before or
  leaves its own in the later filters, and nothing a plugin can call clears that texture
  (`obs_source_video_tick` is not exported). So: **a new StageKit filter takes its input with
  `draw_filter_input`, never `obs_source_process_filter_begin`** (only that helper calls it,
  for a picture in a wide colour space). A filter of OBS's own or of another plugin
  after the Standby filter still has the fault; the README says to put those before it. While
  building this, three test runs in a row tested an old build, because the build had failed
  and its errors were filtered away: **check the build's exit code before a run.** The
  first of the two transition lists is called *No-signal transition* since then (the owner: "no
  signal transition is ok").
- **Show in Scenes can take bring-along items with it** (`with_linked`, off by default): the
  `linked` item of every Move Source filter on a listed source, and of Move Scene Item filters that move it.
  The owner asked for it.
- **Link Visibility reaches into other scenes** (`elsewhere`, *Also in other scenes*): the sources
  listed are shown and hidden with the scene's linked items wherever they are, and a change to one
  of them in any scene comes back. For that the filter listens to every scene's `item_visible`
  (`watch_others`, found again every two seconds, each held weakly and let go in `unwatch`). The
  owner: "The link visibility filter only works with sources in the same scene ... one streamdeck
  button to affect them together". Show in Scenes does the same job as a switch on a key; this one
  follows whatever shows or hides an item.
- **Show in Scenes is a switch, not a trigger.** Its on and off state is the shown and hidden
  state, so one Stream Deck Filter key toggles and shows which way it is. It acts on the `enable`
  signal only (never each tick, so a change by hand is kept) and ignores the state restored at load
  (`ready_`). The owner had a multi-action with a Source Visibility step per scene for this.
- **Anything that changes over time lets the user choose how.** The owner: "Any filter or
  feature/effect that would do a transition or transform should have those ... be customizable, and
  not only offer fade. fade should be the default". A filter that shows and hides scene items offers
  *Change with* and *Lasting* through `src/common/item-transition.*` (any OBS transition, a cut, or
  keep; default fade, 300 ms): Show Source in Use, Show in Scenes, Program Trigger, Link Visibility. A filter
  that animates its own drawing offers a style list, its timings and `add_easing_properties`. Do not
  ship a fixed fade or a fixed curve in anything new.
- **Show in Scenes has the fuller *Change with*** (`add_item_change_properties`, `ItemChange` in
  `src/common/item-transition.*`): *Show with* and *Hide with* apart (hiding is *The same as
  showing* as it comes), the chosen transition's own settings copied into the filter's window from
  that transition's own (`add_own`: tick boxes, numbers, lists and colours; kept as
  `show|<kind>|<its setting>`), and *Settings for*, which switches the controls between the usual
  change and each listed source's own (`for|<source>|...`, *As for all sources* until changed).
  The owner: "must allow you to select the specific settings for the transition type in the filter
  setting, like left right up or down for slide ... each one to have its specific setting set
  independently". The controls are rebuilt in place when a kind or the *Settings for* box changes,
  and when the source list does (`pick_list_on_change`). A slide's and a swipe's direction list
  has one entry of StageKit's own, *Off the nearest edge* (`nearest`, turned into left, right, up
  or down for each item when it is applied, `off_nearest_edge`; a swipe is also told whether it
  is coming in): the owner asked for it by Morph's name for the same thing. OBS's item
  transitions move the picture inside the item's own frame, not across the screen; the owner was
  told so, and that Show/Hide Animation is the one that travels. Show Source in Use, Program Trigger and
  Link Visibility still have the plain two controls; give them this one when it is asked for,
  not a third kind.
- **Move Source moves the source in every scene it is in, by default** (`apply_in`). The owner asked for
  that: the camera sent to a spot is there in every scene. The older `where` setting is not read.
- **Shadow sizes are in screen pixels by default.** The owner's old shadows sat on full-canvas scenes,
  so 20 px was 20 px everywhere; a filter on a scaled item is not. The filter divides its sizes by how
  big the item is drawn in the scene on screen (`find_shown_scale`).
- **A hidden item still says how big its source is drawn** (`shown_scale` in
  `src/common/shown-scale.cpp`, used by Shadow, Glow & Outline and Corners & Edges): the scene on
  screen before the others, and in each a showing item before a hidden one. Hidden items were
  passed over up to beta113, so a hidden item's effect was sized for a copy in another scene, or
  for no scaling at all, and put right up to a third of a second after it was shown (the scale is
  looked up every 20 ticks). The owner, of a slide in Show in Scenes: the shadow and outline
  "start out thinner ... then grow or instantly snap to the correct pixel thickness towards the
  end of the transition, which cause the fit to space filter items to adjust unsmoothly".
- **Shadow, Glow & Outline moves scene items on purpose.** The shadow's room makes the source bigger; the
  filter shifts each item showing the source (and grows its bounding box) by that room so the picture
  stays put, and remembers what it applied in `adjusted_*` settings. Anything else that changes the
  room around a source must go through `keep_picture_in_place`, not around it.
- **Corners & Edges must never change a source's size.** Everything that makes it safe beside the
  other tools rests on that. It belongs before Shadow, Glow & Outline in the filter list (the shadow reads
  alpha, so it follows the shape); when first added it moves itself above an existing shadow once and
  records that in its `placed` setting. After an Auto Frame it shapes the rectangle from Auto Frame's
  `get_picture`.
- **Fit to Space's free space draws an item to look at it as seldom as can be** (`free_box` in
  `src/placement/fit-item.cpp`). *The free space around it* (the owner: a margin of 30 "should
  remain exactly 30 pixel within the edges of the screen, and within 30 pixels of any objects")
  needs to know what shows in every other item. Each is drawn and read back once when first met;
  after that an item that moves or changes size is followed by its box (what shows keeps its place
  in the box), which costs nothing. A widget that starts or stops drawing is noticed by *glances*
  (`glance_step`): one item at a time is drawn 128 px small, handed to the graphics card and read
  back two frames later, which does not stall; a proper look follows only when a glance shows
  another part of the item drawn than at its last look (`looked_`). Until beta106 one item a second
  was looked at properly, in turn, by every filter: the owner's log showed Fit to Space ticks of
  8 to 45 ms, the preview was choppy, and the chat box took seconds to make room for the music
  widget. A glance is not free either: it draws its item once more, filters and all. At fifteen
  a second over every item (beta107) the owner saw the frame time go up, so only an item that
  can come and go is glanced at often (`comes_and_goes`: a browser widget, text, a group, a
  nested scene, and anything once seen to change; twice a second each), and the rest about every
  ten seconds, one at a time. An item's first glance after a look is not trusted as that look's
  measure (they can be seconds apart): the item is looked at once more. What was seen of a
  scene's items is forgotten when a filter joins or leaves the scene (`glance_forget`): a deleted
  scene's entries were taken for those of a new scene made at the same address, and its widget
  went unnoticed (found by a test that failed one run in three). The first build (beta70) read one
  item back every quarter second and all of them after any move; the owner then found camera moves
  "very choppy". Never add a read-back that is read in the same frame on a timer, and none that
  runs while things animate. It only looks while its scene
  is being shown (`obs_source_showing`): every scene's filters tick all the time, shown or not. The space is the largest
  clear one holding the item's place (`anchor_x/y`, saved; retaken when the item is moved by
  anything else, which it lets finish first). Whatever the place lies inside is not in the way. It
  measures by its own two tick boxes and glides to a new space.
- **Shadow, Glow & Outline's "Keep it on screen" only touches an item that is still**
  (`keep_in_view`): a plain item (no bounding box, not in a group, not rotated or locked) whose
  picture is on the canvas but whose effect runs off an edge is scaled down, keeping its shape,
  after 0.4 s without moving. An item being dragged or gliding in, or whose picture is itself off
  the canvas, is left alone. It is one-way: the item does not grow back. Items with a bounding box
  are whoever set the box's business; Fit to Space asks `shadow_kept_in_view` and fits the shadow too.
- **Fit to Space has no "size it by hand".** One was built (beta71: a resize in the preview became the
  box's width and height) and the owner had it removed: "i don't know if the size by hand really
  works. just remove it". An old `by_hand` setting is ignored. Do not bring it back unasked.
- **Ambient Background does not blend in a standby picture** (`live_skip_standby`, on by default):
  `standby_waiting()` says the capture is only showing its Standby Picture. The owner's background
  turned into the still start image.
- **Every filter and source is registered through `register_timed`** (`src/common/timing.*`), not
  `obs_register_source`. It keeps time of each kind's tick and drawing and, twice a minute, writes a
  `timing, last 30 s:` line to OBS's log for any kind with a call over 3 ms or over 2% of the time;
  nothing while all is quick. A filter's drawing time includes what it draws from. It measures the
  processor's side only: a slow shader shows up as OBS's own rendering lag, not here. Ask the owner
  for the log when frames drop; do not guess.
- **Fit to Space's free space takes the roomiest of the spaces that are as good as equal**
  (`free_space`: pictures within the filter's *Tolerance*, 8 px as it comes; the owner asked for it to be a setting). The owner's music widget, set to the same width as the
  column above it, was read a few pixels wider (what shows is measured on a small copy), and the
  space stopped 95 px above the screen's edge to keep those pixels of width.
- **The dock's two measuring options show on the Place tab as well as the Arrange tab**
  (`placeVisible`, `placeNoShadow`, the same options, each following the other), and the Arrange tab
  has its own *In every scene* (`arrange_every_scene`, off as it comes: a tool's result is copied to
  the other scenes with `copy_to_every_scene`). Both asked for by the owner on beta87.
- **Fit to Space can leave the size alone** (`resize`, *Resize it to fit*, ticked as it comes).
  Unticked, the picture keeps the size it has (`keep_w_`, read from the item once and again after a
  resize by hand) and is centred in its space. The owner: "move the box without changing the shape
  or size to fill the gap ... a safety option as well informing the filter to not resize".
  A new filter starts ticked and fits the item the moment one is chosen, so the size an item has
  before the filter first touches it is saved (`orig_item`, `orig_w`, `orig_h`) and is what
  unticking goes back to: the owner's chat box had been shrunk to its space before the box could
  be unticked ("it shrinks").
- **Fit to Space's kept size is read from the item on three occasions only**: when the filter is
  given another item, when *Resize it to fit* is unticked (then the size from before the filter),
  and after a resize by hand, which counts only on an item that is selected, showing, and whose
  source has kept its size for a second. Any other change of settings leaves it. The owner's
  camera, with *Resize it to fit* unticked, got smaller each time it was hidden and shown
  (beta98/99). **The cause was not reproduced here**: two tests built to match (a shadow in screen
  pixels, a camera drawn at a quarter of its source) passed without the fix. The filter now logs
  `the size kept for ... went from ... to ...` with the reason whenever it re-reads the size; ask
  for the log if it happens again.
- **Fit to Space never puts an item past the screen's edge, and does nothing on a group.** With
  *Resize it to fit* unticked, a space narrower than the item used to centre it past the edge (the
  owner's chat box, pushed off the canvas by the game growing beside it); it is now held the gap
  inside the screen (`inside`). The owner had also added the filter to the *group* holding the chat
  box, where the "scene" is the group itself: the filter now stays idle there and says so in its
  settings (`Fit.OnGroup`). It belongs on the scene, with the group as its item.
- **Fit to Space's "Use visible pixels" counts for the fitted item too** (`own_cut_` in
  `src/placement/fit-item.cpp`): the part of the item's picture that shows is what is fitted, kept
  and held clear, not its box. The owner, of a group wider than its grey panel: "only the visible
  pixels in the group, or any other item should be counted. otherwise, what's the point of that
  setting?" It is looked at when first met, half a second after its source last changed size, and
  every three seconds while it sits where it was put; only while it is wholly on the screen; and a
  cut under a fiftieth of the picture is taken for measuring error. Sizes kept (`keep_k_`,
  `orig_k`) are scales of the source, so they hold whatever part shows.
- **Two Fit to Space filters in one scene go by each other's destinations** (`Claim`, `claim_set`,
  `claim_of` in `src/placement/fit-item.cpp`): each says where its item will be once it has got
  there and whether it resizes it, and the other takes that for the obstacle, a claimed group as
  one thing. A kept-size item with something that resizes on one side of it and something firm on
  the other goes against the firm side at once (`kept_in`, `soft_l` and so on); centring between
  them never came to rest, because the other made room each time. The owner's game and chat box
  shifted "a number of times" after a scene switch or a camera being hidden. Which side gives way
  is judged by which side of the space's middle it lies on, never by whether it touches the space:
  judged by touching, the two filters chased each other without end.
- **Fit to Space does nothing to a hidden item** (`hidden_` in `src/placement/fit-item.cpp`): no
  placing, no claim, anchor and kept size untouched, so the other filters take its room. Shown
  again it first only repeats its last claim for `kComingBack` (0.1 s), so whatever took its room
  makes way before it looks for its own space: looking at once, it found its place *inside* the
  other item and took that for something it sits on. The owner's camera, fitted while hidden, was
  put in the middle of the column the chat box had taken, came back on top of it, and neither
  moved again. Do not fit, measure or claim for a hidden item.
- **Fit to Space answers a change made in one step at once** (`stepped` in `free_box`): an item
  shown or hidden, or another filter's claim changing. Only something on its way (`moved`: a box
  that is not a claim and has changed) is waited for, 0.15 s after it stops. And a scene that has
  only just come into view is put right without a glide (`kArriving`, 0.25 s). The owner: "make
  the detection faster and not jumpy so the objects don't continue to adjust multiple times before
  settling". Waiting 0.15 s for every change, two filters answered each other a moment apart.
- **Fit to Space's *Keep it towards*** (`toward`, saved by number, append-only: middle, top,
  bottom, left, right, then the four corners): where an item lies in its space when it does not
  fill it. A kept-size item goes against that side (`kept_in`), a resized picture is aligned in
  OBS's bounding box (`obs_sceneitem_set_bounds_alignment`). A side where a resizing filter's item
  makes room (`soft_*`) still wins over it, or the two would close in for good. The owner asked
  for "a directional bias or preferred direction ... prevent it from going in an undesirable
  direction".
- **Fit to Space's *Same place in these scenes*** (`share_scenes`, a PickList, empty as it comes; `share_place` in
  `src/dock/arrange.*`, `fit_item_shared` in `src/placement/fit-item.*`): each time the filter's
  item has come to rest in a scene that is showing, the same source's copies in the scenes that
  are *not* showing get its transform, locked ones too. A filter of its own in such a scene leaves
  the item there until its scene comes into view, re-reads the kept size, and takes the sharing
  filter's *home* point (anchor) for its own, not the spot the item happens to be at: taken from
  the spot, a chat box that had moved up into a hidden camera's place would be "at home" inside
  the camera in the next scene. Scenes on screen are never written to, or two scenes showing at
  once would push each other for good. The owner: "links the same object between scenes ... so
  it's already there when scene switches and it's not jarring". It began (beta105) as a tick box
  for every other scene; the owner: "it should not be all scenes. because i have the same source
  on other scenes in which their position is supposed to be different". Now only the scenes
  listed get it, plus any scene whose own filter for the item lists this one (`share_partners`),
  so one list links two scenes both ways. The old `share` tick box is not read. Never share with
  a scene that is not named.
- **Fit to Space's kept size is saved** (`keep_item`, `keep_k`, written whenever the size is read)
  and is what a filter starts from when OBS loads it (`first_update_`). Before, a filter created
  with *Resize it to fit* unticked looked like one that had just been unticked and went back to
  the size from before the filter was first added (`orig_k`): the owner's chat box, set by hand to
  the camera's width, was "slightly larger" after every restart. A size is also only read off an
  item whose source has kept its size for half a second: while OBS loads, a group or a shadowed
  source is not yet the size it will be.
- **Morph draws the scenes' items itself** (`src/transition/morph.cpp`). OBS gives a transition
  its two scenes as finished pictures; Morph instead draws each item the way a scene does
  (`obs_sceneitem_get_draw_transform`, the source drawn straight on, or through a picture of its own
  when it is cropped), at a place part-way between the two scenes for an item whose source is in
  both. The owner asked whether a morph between scenes was possible and then to "work on the morph
  transitions". What that means for anything touching it:
  - Before the first frame, after the last and with only one scene it must call
    `obs_transition_video_render_direct`: that call is also what ends a transition. Drawing by
    itself all the way, the transition never stops.
  - Its size is its scenes': OBS's own window never calls `obs_transition_set_size`, and
    `obs_transition_get_size` then says 0 by 0. The first build (beta108) went by that alone,
    skipped the morph for want of a size, and the owner saw "instantly switches scenes with no
    animation". The tests had given the transition a size by hand; they no longer do, and one
    runs it by the clock as OBS does. Test a transition the way OBS's window drives it.
  - A scene with an enabled filter that draws cannot be taken apart (`plain_scene`); the change is
    then a cross-fade and the log says which filter. Filters that draw nothing are listed in
    `draws_nothing`: add a new StageKit scene filter there, or scenes carrying it lose the morph.
  - Runs of items that are in one scene only are drawn into two full-screen pictures and mixed in
    one pass (`Mix`): faded over each other, two backgrounds add up to less than a whole picture
    half-way, a dark dip.
  - The plan (which items go together, and the order) holds scene items by reference and is let go
    through `changed()` on the drawing thread, never from the thread a transition is started on.
  - *Linked sources* (`links`, lines of "A ↔ B" in a PickList, made with two boxes and a button)
    are matched after sources that are the same and before the main picture; a link holds by way
    of other links too (`linked`). The owner: "link my different camera sources to each other
    across scenes ... multiple links should be able to be configured".
  - Like every shader here, `stagekit-morph.effect` has only run on OpenGL; without it the
    transition cuts.
  - **Rules for two scenes** (`rules`, a PickList of "From → To" lines, either side *(any scene)*;
    each rule's settings under `rule|<line>|style`, `others`, `pair_main`, `ease_curve`,
    `ease_mode`, `both`; made in the `rule_*` controls and saved with a button, like the links):
    `for_change` gives the settings for the change going on (`active_`), the most exact rule
    first. Style 1 is a cross-fade, 2 a cut. The owner: "specific overrides for transitions for
    specific scenes transitioning between specific scenes". The duration is OBS's and is not
    overridden. `pick_list_on_change` has one slot for the whole plugin and is Show in Scenes':
    do not take it for a second rebuilt-in-place control.
  - An item's place is worked out by Morph, not by OBS, while it draws (`look`): OBS does it when
    a scene is drawn, from the size the source has then, and neither scene is drawn during a
    morph. `look` does it when an item is first met and again whenever its source changes size.
    Worked out once at the start (up to beta112), a display capture in a bounding box, which has
    no size until it has started again, had no place for the whole change: the owner's "capture
    source doesn't appear until the transition animations completes, and it's abrupt". Reach an
    item through `look`, never `shown_of` alone. The log says `had no picture for the first ...
    ms` for a source that started late.
  - **Two different sources change into each other by what shows of them** (`visible_pixels`,
    *Use visible pixels*, on as it comes; `seen_crop`, `part_quad` in `morph-paint.*`, beta145),
    in the scene Morph and the item Morph alike. The owner: "the morph transition effect should
    morph objects based on visible pixels, not bounding box". Each item's crop is drawn in to
    the smallest part holding all that is not see-through, both parts are drawn as pictures of
    their own and mixed on one quad that goes from the one part's place to the other's. It is
    found from a 256 px copy read back at once, **once per item per change** (kept in `seen_`,
    let go by `forget_sizes` and at each new item change): never per frame. The same source in
    both scenes is not looked at; its box is the same part of it either way.
    *Ignore shadows & outlines* (`no_shadows`, on as it comes; the owner: "add ignore shadows,
    strokes/glow, etc") matches the pictures without the room their Shadow, Glow & Outline
    filters take (`without_room`, from `shadow_room`: no drawing bare, so cameras are safe).
    The effects are still drawn: `match_parts` gives each side the matched part and the same
    share of it all round, as much as the larger effect needs, so both can still be mixed on one
    quad (a part may reach past its source; `cropped` draws nothing there). Only StageKit's own
    filter is known by its room; another plugin's glow counts as picture.
    A shadow filter that had never changed its room since it was made (default sizes, or one
    from before the room was kept track of) had no `adjusted_*` in its settings, and
    `shadow_room` took it for none: it is written down in that case too since beta145.
  - Not followed during the change: an item's blending mode, its show and hide transition, a crop
    to the bounding box, and a shadow's size in screen pixels (`shown_scale` goes by one scene at
    a time, so it may jump at the start). None has been asked for.
- **Morph between two items of one scene is drawn by a layer, not by the items' transitions**
  (`src/transition/item-morph.cpp`). The owner: "id like a similar morph effect when showing and
  hiding objects ... morph should be a transition in show in scene, and follow the same rules as
  the global morph source links, or specify source links in the filter". What that rests on:
  - OBS draws an item's show and hide transition inside the item's own frame, often by way of a
    picture the size of that frame (any crop, scale filter or blending mode does it), so nothing
    drawn there can cross the screen. A private source of the scene's size (`stagekit_morph_layer`)
    is added to the scene straight above the two items, locked, for as long as the change lasts,
    and taken out after. It draws the pair as the Morph transition does.
  - The two items are hidden and shown for OBS and everything else from the first moment. Their
    own transitions are the Morph transition as an item's (`ItemMorph`): *Paired* draws nothing,
    *Alone* fades (or fades and zooms, `others`), *Waiting* is a hiding item left as it is and a
    showing one not drawn yet.
  - Pairs are found whoever hid and showed the items. Every Morph says when it starts
    (`item_morph_started`); on the next tick the ones that are an item's are looked up in the
    scenes (`find_items`), and a hide and a show in one scene within `kGraceNs` (0.1 s) pair up
    when their sources are linked in *any* Morph's settings (`g_said`), or are the largest of each
    at 15 % of the screen or more (`pair_main`). Only in a scene that is showing and for items
    straight in it: a group would grow to the layer's size.
  - The change goes by the shown item's transition's clock, and the tick ends it at the very frame
    that transition draws the item itself: OBS's video time is the same in a frame's tick and its
    drawing. Do not time it any other way, or there is a frame with both or with neither.
  - Scene items are added and removed from a tick callback, never from inside a signal or while
    drawing. Everything stops at `item_morph_stop()` (OBS closing, unload).
  - Anything that goes through a scene's items to fit round them, line up with them or snap to
    them must pass the layer over (`is_morph_layer` in `src/common/scene-util.hpp`): Fit to Space,
    the arrange tools' obstacles and the Morph transition's own plan do.
  - A filter with the fuller *Change with* (Show in Scenes) has its own links (`morph_links`, made
    with `add_morph_link_properties`), handed to its items' Morph transitions as their `links`
    setting, shown only while Morph is chosen. The other three filters go by the links made
    elsewhere and by the main picture.
  - A comparison of two times taken from the clock at different moments must not be made by
    subtracting unsigned numbers: the first build ended every change in the tick that began it.
- **Fade and Zoom** (`stagekit_fade_zoom`, `src/transition/fade-zoom.cpp`; the owner: "also add
  fade and zoom as a transition") is a transition like OBS's own, so it is in every list of
  transitions by itself. One side (an item): it fades as it grows or shrinks. Two sides (a change
  of scene): the old scene stays underneath and the new one fades in over it as it grows, so the
  picture is whole all the way. *Smallest size* stays at 100 % or under: larger would be cut off
  at the item's frame.
- **Blur goes to 1000** (`kMaxStrength`; it was 250 and the owner asked for more). A Gaussian or box
  blur set above `kTapLimit` (250) is drawn the Dual Kawase way for its whole ramp, so there is no
  change of look part-way through a ramp. Zoom's pull is still worked out from 250. Ambient
  Background's own blur goes to 1000 as well (`kBlurLevels` 8), asked for in the same breath.
- **Blur's "And live captures on screen" has a list it leaves alone** (`never`, *Never blur
  these*, a PickList of captures; `lead_others` in `src/blur/ramp-blur.cpp`). The owner: "there
  should be an exclusion option ... so cameras for example can be intentionally excluded from
  blur". It only holds back that tick box: a source named under *Blur these too* is still
  blurred, and so is one with a Blur of its own under the *Blur All* hotkeys.
- **The mosaic is one shader for the Blur filter and Ambient Background** (`Mosaic` in
  `data/effects/stagekit-looks.effect`): seven cell shapes by
  number (`PixelShape`: square, hexagon, circle, triangle, Voronoi, rhomboid, triakis; append-only),
  smoothing, an origin and a rotation. The owner showed Composite Blur's lists and asked for
  "similar blur settings added to the blur filters and ambient background". The old *Hexagons* blur
  type (6) is read as Pixelate with hexagon cells and rewritten so in the settings. Hexagon and
  circle cells keep the old hexagon size; the other shapes are scaled to cover about the same area.
  Like every shader here it has only run on OpenGL: it keeps to constant loop bounds, `float(i)`
  casts, no `inout` and no `atan2`.
  *Glass* (`pixel_glass`, the `glass` uniform, beta134): each cell lit from the upper left by
  where the pixel lies in it (one more `cell_centre`, no more reads of the picture). The owner:
  "make the mosaic glassy ... with no or extremely low impact on performance". Two styles
  (`pixel_glass_style`, `glass_style`, by number, append-only): 0 *Dome*, 1 *Flat plate*, in the
  owner's words "a hexagon that has flat sharp edges because it's a flat plate of glass, and ...
  a curved dome glass". The plate's bevel has straight facets for squares and hexagons (the
  hexagon's three axes are those of `hex_centre`: change them together) and is round for the rest.
- **New shader code goes in `stagekit-looks.effect`, not in the older files.** The mosaic, the
  trail and bloom live there so that, should Direct3D refuse any of it, `stagekit-blur.effect` and
  `stagekit-ambient.effect` still load: both features then log one error and fall back (squares and
  hexagons by the old techniques, no Temporal, no Bloom). The tests ran that way once, with the
  file moved aside. One effect file that fails to compile takes every technique in it with it; do
  not add untried code to a file an existing feature needs.
- **Temporal blur keeps what it drew last** (`tr_past_`, 16-bit float so a long trail fades all
  the way out) and moves it on once per frame however often the filter is drawn (`new_frame_`),
  by the time since it last did (`frame_seconds_`), so the trail's length does not depend on the
  frame rate. It starts afresh when the blur has been off or the size changes. The same in
  Ambient Background (`trail`).
- **Ambient Background's Bloom** (`bloom`, 0 is off) takes the bright parts of the picture as it
  is *before* the blur (`BloomBright`), spreads them with a `KawaseBlur` of their own and lays them
  over the blurred picture the way light adds up (`BloomAdd`), before the mosaic. The owner: "Add
  Bloom as a function in ambient background". Effect masks and a background source to blend over,
  which Composite Blur also has, were not asked for by name and are not built.
- **The Copy tab locks and unlocks across scenes** (`set_items_locked` in `src/dock/arrange.*`): the
  selected items' sources, the listed sources or every source; in all scenes, the listed scenes or
  this one. The owner asked for it so a locked copy can be freed for the copy tools, and as a
  "global lock sources toggle". It is not in OBS's undo list.
- **A filter is changed everywhere by copying one filter's settings** (`copy_filter_settings` in
  `src/dock/arrange.*`, *Copy a filter's settings* on the Copy tab), not by a second editor: the
  owner asked for "selecting a filter and changing its settings" for all sources, chosen sources or
  the sources of chosen scenes. OBS's own Filters window stays the editor; the dock pushes the
  result. `kOwnSettings` lists what each filter keeps for its own source (`item`, `adjusted_*`,
  `learned`, anchors and the like) and is never copied: add a filter's new self-written setting
  there. A filter belongs to a source, so "in a scene" means the sources shown in that scene.
- **Fit to Space's free space waits for a move to end.** It takes its new space in one glide once the
  other items have been still for 0.15 s. Beta74 had it chase a moving item, resizing the picture
  (and its shadow) on every frame of the move; the owner still saw dropped frames during moves.
- **Fit to Space gives OBS the space, not the size.** Its bounding box is the space the picture may take
  (plus a shadow's room at the fitted scale), with scale-to-inner-bounds and centre alignment, so OBS
  fits the picture every frame and a window being resized never lags. Do not go back to setting the
  item's scale or a box of the picture's exact size: both are one tick late.
- **Trigger filters** (Move Source, Move Scene Item, Layout) switch themselves off after running and ignore the enabled
  state restored at load (`ready_`), so they do not fire when OBS starts.
- **The list of open windows is kept by a thread of its own** (`WindowWatch` in
  `src/capture/auto-capture.cpp`); a filter's tick only takes a copy (`open_windows()`,
  `g_window_watch.latest()`). Looking at every window, and inside the program in front, takes from a
  few milliseconds to a third of a second; done on OBS's drawing thread four times a second it
  dropped frames all the time (the owner's log, beta76: Auto Capture ticks of 20 to 70 ms, Active
  App up to 361 ms). Never call `list_windows()` from a tick. The thread is stopped in
  `obs_module_unload`.
- **A capture can be hooked by hand** (`hook_front_window` in `src/capture/auto-capture.cpp`, the
  `HookFront` vendor request, the Stream Deck plugin's *Capture Front Window* key). The owner: "i want to be
  able to press a button to hook a window for capture or a game manually. the built in obs plugin
  does not have that". It aims the capture at the window in front (from the window watch) and
  switches an Auto Capture on that source off, or that would aim it elsewhere a quarter second
  later; switching the Auto Capture on again is how it is handed back. OBS itself in front is
  refused. Its Windows side has not run in the tests (they give it a stand-in window).
- **A picture shape set by hand lasts for one game when asked to** (`force_until_gone` in Auto
  Frame, `SetPictureShape`, the *Set Aspect Ratio* key). The owner: "it should maintain that ratio for
  the active or hooked game until its over". It is dropped once the capture has had a picture since
  the shape was set and then nothing for `kNewSession`; set before a game starts, it waits for it.
  The Aspect Ratio Preset filter still sets a shape that stays.
- **The hook can change over with a transition** (`hook_front_window`'s `transition`): the items
  showing the capture are hidden with it, the capture is aimed while out of sight, and they are
  shown again, on a thread that holds what it uses and is joined at unload. The owner asked for
  "configurable transition options" on the hook key.
- **Capture App in Use is Show Source in Use with one capture** (`src/capture/app-capture.cpp`,
  id `stagekit_app_capture`). The owner: "I'd like to have a single Window capture, and a new
  filter added that can have multiple apps associated with the Window capture and effectively
  emulate my current process and visibility functionality ... but with only a single window
  capture source." Keep its rules the same as the scene filter's: an app that opens is taken,
  the app clicked into is taken with *Follow the app in use*, anything else in front changes
  nothing, and the one used before comes back when the captured app closes. It acts on those
  events only. What is its own:
  - One capture cannot show two windows, so the filter draws the change: it copies the capture's
    picture every frame (`last_`), keeps it while the capture has none (`Phase::Wait` after
    aiming, `Phase::Lost` when the picture goes without warning) and brings the new one in over
    it (`Phase::Mix`), in the style chosen. It reports the kept size meanwhile. A capture goes on
    showing its old window for a moment after being aimed: the new picture is believed once the
    capture has been seen empty, or after `kSettle`.
  - Standby Picture goes by the capture's own size, so it is told (`app_capture_holding`), or it
    would stand in for that moment. A new filter that stands in for a capture must ask too.
  - It keeps itself before a Standby Picture and an Auto Frame, looked at twice a second
    (`keep_first`). Moving to the top once when added (beta116) did not hold on the owner's
    machine: their screenshot had it third, after the Standby Picture. Cause not found.
  - **The other open apps are captured in the background** (`ready`, *Keep the others ready*, on
    as it comes; `keep_ready`): a private capture like the parent for each other open entry of
    the two lists, kept showing while the parent is. On a switch the filter draws that capture at
    once (`bridge_`) while the parent is being aimed, and goes back to the parent's own picture
    once it has the window. The owner, of beta116, which waited for the one capture every time:
    "switches between apps much slower than the show source in use filter ... a long pause". The
    parent is still aimed, because Auto Frame and others read the program from its settings.
  - *Apps you switch to* (`by_hand`) are held by hand (`held_`): the *next* command takes the next
    one that is open, and after the last, or with *back*, the apps are followed again. Nothing is
    followed while one is held. The owner: "manually switch to and from a specific app, like a
    browser page". *Hide* hides and shows the capture's items in every scene with the item
    transition chosen. The three commands are hotkeys on the capture named
    `stagekit.active.next.` / `.back.` / `.hide.` + the filter's name, the same beginning as Show
    Source in Use's, so the Stream Deck *Next Manual Capture* key serves both.
  - The reported size glides from the last picture's to the new one's for every style but a cut:
    taking the new size at once (beta117, all but Morph) made the item jump on screen before the
    picture changed. *Morph* (`Style::Morph`) stretches both pictures over the gliding frame; the
    others fit each inside it. It is not the item Morph, which needs two items.
  - A ready capture that is being drawn (`holding()`) is never stopped. Up to beta117 the capture
    of the app switched to was dropped in the same tick as the switch, being "the current one"
    now; the owner's log had `Window Capture (<app>)` re-made at every switch and they saw the
    picture drop out and come back.
  - **The Capture Front Window key goes through the filter** (`app_capture_hold`, called first in
    `hook_front_window`): the window is held (`held_`), its program is added to `by_hand`, and
    the filter stays on. Switched off instead (beta116, beta117), the picture vanished at once and
    there was no way back to that window. From following the apps, *next* goes to the app held
    last (`last_held_`), so with one hand-picked window the key is a toggle.
  - Entries are a program's file name or part of a window's title (`answers`); a title entry aims
    the capture by title (`priority` 1), a program entry by executable.
    A title entry is aimed again when its window's title has changed **and the capture has no
    picture** (the ready capture in `keep_ready` after 0.3 s, the capture itself in `ready_for`,
    beta136): OBS finds a window aimed by title by its whole title only. Never aim a capture that
    still has its picture because a title changed: it holds its window, and a title with a frame
    rate in it changes every second. The owner's Dolphin GBA windows are "GBA1 | Volume 100%" or
    "GBA1 | Muted"; their old scene had two captures for each window. *Ignored windows* (`ignore`,
    a PickList; checked first in `match`) are never taken, by program or part of a title: the
    owner asked "can exclusions be added?". The lists offer the first
    part of a title with " | " or " - " in it. **One capture per window** is the answer to "two
    windows in one scene": the owner asked whether one capture could show two, was told no, and
    the scene they showed places the two windows apart.
  - The Capture Front Window key switches it off, as it does an Auto Capture; switching it on
    hands the capture back (it takes the app in front or the last one used).
  - **Nothing is started, aimed or stopped at a switch** (beta119). Every open entry of the two
    lists has its ready capture from the moment its app opens, the one being shown too, and a
    switch only changes which is drawn (`bridge_`); the capture itself is left where it was
    (`own_ok_` false: its own picture is not shown, or the wrong window would flash up when the
    ready capture goes). Up to beta118 each switch started one capture, aimed the parent and
    stopped a third. The owner: "a significant increase in frame times and FPS drop when
    transitioning"; OBS's profiler in their log had `tick_sources` at up to 135 ms with drawing
    under 10 ms and no StageKit tick over 7 ms: it was the captures starting. The parent is aimed
    only when there is no ready capture with a picture yet.
  - *Captured window* (`picked`, by number, append-only: 0 *Wait until switched to*, as it
    comes; 1 *Switch at selection*): what the Capture Active Window key does through the filter.
    Waiting, the window's program joins `by_hand` and becomes `last_held_`, so it is captured in
    the background and the toggle goes to it, but `held_` is not set and nothing is aimed. The
    owner: "select a browser or other windows for capture in the background while using my
    autoswitch apps, but then use the switch to manually captured window when i want".
  - **A picked window is that window** (`pinned_name`: "program — title", `pins_`: entry to
    window handle, `OpenWindow::handle`). The owner, with two browser profiles: "i close one, it
    defaults to the other one and may show private info on stream ... this needs to have a global
    solution that works with any app". Two causes: entries were programs, and OBS's Window
    Capture, aimed by executable, takes another window of the program by itself when its window
    closes (`window_rating` in OBS's window-helpers.c: priority 2 matches any window of the
    executable, priority 1 only an exact title). So a pinned entry is matched by the window's
    handle once seen (by program and whole title before that, and only when one window answers),
    aimed by title (ready captures too, re-aimed when the title has changed), and taken off the
    list when its window closes. And the window shown is watched (`shown_handle_`): when it
    closes, the capture's own picture and bridge are not believed (`unsafe_`, `own_ok_` false)
    until `aim` is called for a window chosen here; with nowhere to go the Standby Picture is
    shown (`guard_cover_`). Program entries are still aimed by executable: aimed by title, a
    window whose title changes all the time (an emulator's frame rate) would never be found.
    Windows side not run here: the tests give stand-in windows ids.
    Picked windows are cleared (`clear` command, hotkey `stagekit.active.clear.` + name, the
    Stream Deck step `clear`; `clear_picked`, on as it comes, drops them when the filter is
    first loaded, `loaded_`): the owner did not want them "retained in the next session".
  - *Auto-switch manual windows* (`hand_follow`, on as it comes): while one is held, the same
    rules as automatic mode among `by_hand` only (clicked into, just opened via
    `hand_was_open_`, the one before via `hand_recent_` when the held one closes); without it a
    held window stays until let go. The owner: "when in manual window mode, it follow the same
    behavior as the auto mode, but only for the manually selected windows ... it should be able
    to be toggled with a switch in the settings".
  - *Toggle manual / automatic* (`toggle`, hotkey `stagekit.active.toggle.` + name, also on the
    scene filter): back from a held window, else on to the one held last. The owner: "the
    capture front window can be used to select the manual capture, and then the toggle can be
    used to switch".
  - The capture drawn from (`bridge_`) and the change to it (`phase_`) are set under one lock in
    `aim`, and coming back from a manual window never takes what the capture itself is aimed at
    for what is showing (`released`). Either way the new window's picture was up at a stroke
    before its size had begun to change: the owner's "the image of the next capture appears to
    instantly appear, then the morphing and window size changes happen".
  - None of the Windows side has run in the tests: they feed it stand-in windows
    (`set_test_windows`) and a colour source for the capture. How a real Window Capture behaves
    while it is re-aimed (how long it is empty, whether it flashes the old window) is unknown here.
- **Auto Capture leaves a program it cannot hook alone for a while, never for good** (`rest_`,
  `unhooked_s_`, `give_up_s_` 10 s, in `src/capture/auto-capture.cpp`). Up to beta134 such a
  program went on the saved *Never capture* list. The owner's Dolphin, open with no game running,
  gives a Game Capture no picture either: their log had it put on the list three times in one
  session, each time after they had taken it off ("Dolphin keeps getting added to the never
  capture"). Now it is rested in memory only: tried again when its window's title changes (a game
  was started; three such tries at most), when it is clicked into again (`away`), or after a wait
  of 20 s that grows by half each time to a minute at most, for the full time each try; a picture
  or the program closing ends it. (Beta135 and 136 tried for half the time, 5 s, with waits up to
  5 minutes: the owner's Ryujinx, whose title never changes and whose first picture took 7 s, was
  given up on three times running and they reported "auto capture is not capturing a game". Its
  log also showed OBS hooking it the moment a game drew: an emulator's menu gives no picture.) The
  status says *Aimed at ..., which shows no image yet* (`waiting_for_picture`) instead of *Now
  capturing* over an empty capture. **Do not write to the user's
  lists from the tick again.** What follows is why the giving up exists at all: aimed at a
  program for that long with no picture, the program was given up on and the log says so. OBS's Game Capture tries to hook about
  five times a second for as long as it is aimed, each try some 20 ms on the drawing thread. The
  owner's 3D modelling program had been taken for a game (it loads a graphics engine): 3,404
  tries in twelve minutes, `tick_sources` late in 7 % of frames in every log from beta114 on, and
  they saw "choppiness during the animations". It was first put down to the window switcher's
  captures (beta119); the tell was a 19-second session with no switch at all and the same 7 %.
  **Read OBS's own profiler lines and count repeated lines in a log before blaming new code.**
  Programs an Automatic Window Switcher lists are never candidates (`app_capture_lists`). A
  program on *Always capture* is never given up on.
- **A game that OBS will not hook is not always Auto Capture's doing.** The owner's Ryujinx, on
  *Always capture* with a game running, was tried 254 times before a picture came, and their
  Dolphin's Vulkan capture was freed and started 109 times in one session. Their NVIDIA driver's
  *Vulkan/OpenGL present method* was *Prefer layered on DXGI Swapchain*: OBS started a Vulkan and a
  Direct3D 12 capture of the same game at once. Set to *Prefer native*, "the capture works
  immediately now" (the owner, 2026-11-16). In a log, look for `attempting to hook process` /
  `capture stopped` in a tight loop, or `vulkan capture freed` many times a second, before
  changing Auto Capture; the README's Troubleshooting has the fix.
- **Ambient Background follows a change of capture with a transition** (`live_change`,
  `live_change_ms`, `live_delay_ms`, `live_ease_*`): when the capture blended in changes (another
  source, none, or `app_capture_changes` says the switcher took another window) the scene as last
  shown is kept (`tr_was_`) and laid over the new one, fading or sliding away after the delay. It
  is laid down `as_is` (not coloured a second time). The owner: "the ambient background changes
  to it instantly. there should be a configurable delay ... and the transition for that blend in
  should include the same transition options that others do". The tests use a cut for the
  pictures they take and check the fade separately.
- **Ambient Background's bloom light is spread at least as far as the blur** (`spread`, the larger
  of `bloom_size` and `blur`). It is taken from the picture before the blur; spread less, it laid
  the sharp picture back over the blurred one. The owner: "blur does not appear to work correctly
  when bloom is enabled". And a slider has no modified callback that shows or hides other
  controls: OBS rebuilds the window at each step and the slider "cannot be dragged smoothly".
- **Fit to Space's items never in the way can count after all** (`skip_fill_on`, *Overlap only when it fills*, and `skip_fill`, *Fill needed to
  overlap*; first one slider called *Avoid them under a fill of*, which the owner found "unclear
  and ambiguous" and asked to have "a toggle on or off and have a streamdeck button": the
  *Fit to Space Overlap* key sets `skip_fill_on` with obs-websocket's `SetSourceFilterSettings`): the space is worked out without them first, and when the picture would fill
  less than that share of it (width share times height share), again with them. Both come from
  the picture's shape, never from where it is now, or the choice would flip once it had moved.
  The owner: a camera overlapping a window capture, but beside it when the window "doesn't fit
  the whole screen".
- **Fit to Space's overlap rule has two measures, each with a tick box** (`skip_use_pct`, *Use fill
  percentage*, on as it comes, with `skip_fill`; `skip_use_px`, *Use edge distances*, with
  `skip_px_top`, `_bottom`, `_left`, `_right`, 50 px as they come): the second overlaps when the
  empty room the picture would leave at every edge of its space (lying where *Align in the space*
  keeps it) is within that edge's pixels. Either ticked rule is enough to overlap; with neither,
  the item always goes beside. `skip_fill_on` is still the one switch, and the Stream Deck key's.
  The amounts are sliders and are called *Top / Bottom / Left / Right gap up to* under *Use edge
  gaps (pixels)* since beta131: as plain number boxes called *Most empty room: top* the owner
  found them "not clear" and asked for "sliders with digit inputs".
  The owner: "individual configurations for specific pixel amounts from the top, bottom, left and
  right edges ... a tick box to use percentage, and a tick box to use the specific pixel values".
- **The window switcher keeps the shown window's ready capture while its scene is away**
  (`ready_for` in `src/capture/app-capture.cpp`). Every ready capture used to be dropped the moment
  the capture was in no scene on show, the one being drawn from too (`bridge_`), with the capture
  itself not believed (`own_ok_` false): back in its scene there was no picture until another
  window was switched to. The owner, beta130: "the capture is gone and does not come back unless
  I specifically pick a certain one of the auto capture windows". Now only the others are dropped,
  and a lost bridge is taken up again from the ready capture of what is shown (the log says
  `draws ... again`). Never leave `bridge_` empty with `own_ok_` false.
- **Fit to Space goes with a window switcher's change** (`app_capture_change`, `AppCaptureChange`;
  `with_switch`, `synced_`, `sync_s_` in `free_box`): while the switcher brings a new window's
  picture in (`Phase::Mix`) it says the size it is going to, how far along it is and its easing;
  the fit takes that shape at once (scaled by the same ratio, so a shadow after the switcher is
  carried along) and glides to its space over the switcher's remaining time and easing, not its
  own. Left to notice the shape, it waited half a second after the shape stopped changing and
  then set off on its own glide. The owner: "the window switches, but then it's like the auto
  fit has to catch up". Only the free space depends on the shape; the other two fit areas are
  fitted by OBS every frame and never lagged.
- **Standby Picture does not glide after a window switcher** (`led`, `app_capture_present`): with
  an enabled Automatic Window Switcher on the capture, and the capture live (`!empty`, `mix_` 0),
  it takes the size before it as it is. Its own size glide (800 ms as it comes) set off after
  each step of the switcher's (250 ms on the owner's capture) and arrived long after the switch;
  beta128 had synchronised Fit to Space with the switcher and left this in between, and the owner
  still saw the fit trail ("fit still trails the source window switch", with logs). Found from
  the filter order in their log and the times in their collection, not reproduced on Windows.
  The rule above it, never following what comes before it directly, still holds for everything
  else (an Auto Frame settling on a shape).
- **A Switch Scene key's picture is the pack's own for that scene** (`scenePictureFor` in
  `streamdeck/src/pictures.ts`): the pack's `scenes` group was drawn from the owner's layouts, and
  the owner asked that "the scene keys should be based off the original scene key designs". A
  picture fits when all the words of its label are in the scene's name; no scene names are in the
  code.
- **A Standby Picture can be shown over a running capture** (`cover_`, the `cover` proc,
  `standby_cover_toggle`, hotkey `stagekit.standby.cover.` + filter name). The Automatic Window
  Switcher's hide does that when the capture has an enabled Standby Picture (`hide_standby`, on
  as it comes) instead of hiding the items. The owner: "I'd like the standby picture filter to
  somehow work when the capture show/hide option is used". Not saved: OBS starts uncovered. While the picture comes in
  over a running capture the capture is still drawn live under it (`under`). The owner found the
  change "instantly goes on or off" on beta121; here it fades (a test looks part-way), so the
  cause on their machine is **not known**: the log line `Standby Picture '...': shown over the
  capture, over ... ms (transition n)` is there to tell. `Style::Morph` (11) stretches both
  pictures over the gliding frame, as the window switcher's Morph does.
- **Auto Capture can show the capture's Standby Picture** (`no_game_standby`, *No game: Standby
  Picture*, off as it comes; `hide_standby`, *Hide shows Standby Picture*, on; hotkey
  `stagekit.active.hide.` + filter name, the window switchers' beginning, so the Stream Deck
  *Manual Window / Source* key's *hide* serves it). It goes through `standby_cover_set`, which
  lends the Standby Picture Auto Capture's own transition (`standby_style`, by the Standby
  Picture's numbers; `standby_ms`; `standby_ease_*`) for that change in and the one back
  (`lent_`, `own_` in `src/capture/standby.cpp`); then the filter's own settings apply again. The
  owner: "add an option for no game: show standby image, with transition options similar to
  other filters". Without a Standby Picture, hiding hides the capture's items in every scene.
- **Ambient Background's Bloom has two styles** (`bloom_style`): 0, the bright parts shine
  (threshold, screen-blended); 1, *Whole picture, amplified*: all of the picture's light added
  back (`BloomAmplify`), which is what the bloom in the owner's old collection did
  (obs-shaderfilter's `bloom.shader`, Angle_Steps 1, Radius_Steps 20, ampFactor 4 to 10; 100 %
  here is its ampFactor of 1). The owner: "the bloom effect ... is not the effect i intended. I
  like the bloom effect from my old scene collection".
- **Auto Capture keeps its capture shown** (`keep_ready`, on by default, `obs_source_inc_showing` on
  the capture). OBS stops a Game Capture about 0.3 s after the last scene showing it is left and
  hooks it again on the way back; the owner's log showed that at every scene switch.
- **A Move Source press looks at each source once** (`seen_part` in `src/placement/place-item.cpp`), not
  once per scene: what shows of a source is the same part of its box everywhere. A press that moved
  the camera in twenty scenes took 40 to 70 ms before a single frame of the move. An item straight
  in its scene that only moves is stepped from where it began (`Move::plain`), with nothing looked
  up on the way.
- **Stream Deck key art has no words, and every option has its own picture and title.** The owner:
  "there should be icons specific to each option, and this goes for every stream deck key",
  "remove the embedded text names of the keys in the art, but ... make sure the Title for the key
  is set accordingly based on the options selected". `streamdeck/tools/make-option-icons.py` takes
  the words out of every pack picture (run it after `import-icons.py`) and draws the pictures the
  pack has none of into `imgs/pack/<palette>/options/` (the pack's own little screen with a
  drawing in it), listing them in `src/pack.json`. `src/titles.ts` is what each key calls itself
  (`setTitle` in every action's `show`); a title typed in Stream Deck wins. A new option gets a
  drawing in `DRAWINGS`, a line in `src/pictures.ts` and a title, or it shows the plain key.
  Each key offered also has its own small icon in Stream Deck's list of actions
  (`tools/make-action-icons.py`, `ICONS`, white lines, 20 and 40 px, into
  `imgs/actions/<key>/icon`); the owner, of a list where all keys showed one of two drawings:
  "I'd like all of the stream deck buttons to get specific icons". A new key gets a drawing there;
  `tests/features.test.ts` fails if two keys share one.
- **A key that switches something shows which way it is with a mark, not by brightness.** The
  owner: "the options that have an on and an off or toggle must represent that current status on
  the key with the image ... right now, the stagekit blur button just flashed". The pack's own on
  and off pictures differ only in brightness; `tools/make-state-icons.py` writes marked copies
  (`imgs/pack/<palette>/status/on|off/`, a green dot on, dimmed with a red slash off, 256
  colours) and `stateFile`/`showOnOff` (`src/actions/shared.ts`) set both states' pictures and
  the state. Every two-state action has `DisableAutomaticStates` in the manifest
  (`make-plugin-files.py`): Stream Deck flipped the key on each press before the plugin put it
  back, the flash. *Manual Window / Source* has two states and asks OBS with the
  `GetSwitcherState` vendor request, following the `SwitcherChanged` event
  (`vendor_switch_state`, reported by the two switchers and Auto Capture): *Toggle* is on while
  a manual window is shown, *Hide* while the capture is shown (`shown_nowhere`). A new key with
  two states goes through `showOnOff`.
- **A Stream Deck key keeps what OBS last told it** (`remembered` in `streamdeck/src/seen.ts`: the
  filter's kind, on or off, and the few settings an image or a name depends on, in the key's own
  settings under `seen`, with what it was for). Without an answer from OBS (closed, the connection
  lost, or just started: obs-websocket connects before the scenes are loaded) the filter, move and
  Capture Active Window keys showed the plain image and *Switch*, *Move* or *Capture Game*, and
  nothing made them look again. The owner: "Some keys on streamdeck default back to the wrong
  image, and do not take on the correct name". `obs.ts` also says `changed` 2, 6 and 15 s after
  connecting and, once per half second at most, when inputs come and go or the collection
  changes. **Raise `VERSION` in `tools/make-plugin-files.py` whenever anything under
  `streamdeck/` changes**: with the same number Stream Deck may keep the copy it has (beta135
  changed its wording without a new number, and the owner's keys still said *Picture* afterwards).
- **The Switch Scene key** (`src/actions/scene.ts`, UUID `.scene`) uses obs-websocket's own scene
  requests, nothing of the OBS plugin: `SetCurrentProgramScene`, or `SetCurrentPreviewScene` in
  Studio Mode; a transition named in the key is set first with `SetCurrentSceneTransition`, which
  makes it OBS's current one (obs-websocket has no transition for one switch). Lit while its scene
  is on (`CurrentProgramSceneChanged` and the preview's). The owner asked for "streamdeck key ...
  for scene switching".
- **Device pictures and SVGs.** `DEVICES` in `tools/make-option-icons.py` (group `devices`; the
  owner: "icons available for things like generic capture cards, external capture cards"), and
  `tools/make-svgs.py` writes `streamdeck/svg/`: every picture drawn here as editable SVG, through
  `tools/svgdraw.py`, a stand-in for Pillow's `ImageDraw` that writes shapes, so the SVG and PNG
  come from the same code. Run it after changing a drawing. The original pack's own SVGs, sent by
  the owner ("pack the svgs in the repository/ all of them is fine"), are under
  `streamdeck/svg/pack/<palette>/<group>/` (1,572 files, 42 MB): copied as they came, written by
  no tool here, and not what the plugin's PNGs are made from.
  The word-stripper in `make-option-icons.py` skips `options/` and `status/`: run over the marked
  copies, it took their dot and slash for words.
- **Blur's *Never blur these* offers every source with a picture**, not only the capture kinds,
  and a Cropped View listed there keeps the source it shows clear too (the blur sits on that
  source). The owner could not find their camera: "this list should show all sources".
  And *And live captures on screen* finds a capture that is on screen only through a Cropped View
  (`find_live`, beta133; into a scene or group the view shows as well): the owner's *Square
  Camera* "is actually not blurring at all even with it not included in the exclusion list".
  The blur goes on the capture, which the view draws with its filters.
- **The Stream Deck plugin has never run in Stream Deck from here.** This session has no Stream Deck
  and cannot reach docs.elgato.com; it was written from the SDK's npm packages (`@elgato/streamdeck`,
  the project template in `@elgato/cli`) and their type definitions. Proven here: it type-checks,
  builds, passes `streamdeck validate`, packs, and its link to OBS works against a stand-in
  WebSocket server (`streamdeck/tests/obs.test.ts`). Not proven: the settings pages (the lists come
  from the plugin through sdpi-components' `datasource`; the filter list is refreshed when the
  source changes; the connection fields use `global`), and how the keys look. Its keys act on OBS
  through plain obs-websocket requests (`SetSourceFilterEnabled`), so they need nothing new in the
  OBS plugin. Its key pictures are the owner's icon pack, published with the owner's say-so
  ("its fine for the icon pack to be publishes", mono as the default colour): 144 px copies under
  `imgs/pack/<colour>/`, written with `src/pack.json` by `tools/import-icons.py` from the pack's
  folder, which is not in the repository (nor is the program that draws it; the session that
  made the pack has both, and added the "hook game" and "hook window" pictures to it for beta81). A key left on
  *Automatic* gets its picture from the filter's kind and settings (`automatic()` in
  `src/pictures.ts`); keep that by kind and setting, never by the owner's source names. Since
  beta134 a filter key first takes the pack's own picture of the source it is on, where the words
  of a picture's label are all in the source's name (`sourcePictureFor`: the pack's *show hide*,
  *shadow by source*, *standby by source* and *blur <type>* groups), and a Show in Scenes key then
  the picture of what the source is (`inputPicture`, by OBS input kind; `obs.inputKind` follows a
  Cropped View to what it shows), before the filter's general picture (`chooseFor`). The owner:
  "the show in scenes key set to square camera should either set to the correct icon relative to
  a camera source, or at least have another option". Matching is by words; still no source names
  in the code. The
  Stream Deck shows one key per feature (`src/features.json`: the owner asked for "a key for each
  of the major functions", not "one filter key with a ton of filters"), each choosing from one
  list (`target`, source and filter in one setting). The manifest, the settings pages and the
  default key pictures are written by `tools/make-plugin-files.py` from that table: add a feature
  there and run it, do not edit `manifest.json` or `ui/*.html` by hand. The first two catch-all
  keys (`.switch`, `.move`) stay registered and hidden so keys made with them keep working. The
  palette is each key's own setting (`palette`): the first build had one colour for all keys and
  the owner asked for it per key and for the word *Palette*. A *Dock Option* key and a *Slide to Edge* key go
  through the `SetOption`, `GetOptions` and `Slide` vendor requests; the slide is the dock's own
  (`set_slide_handler`), run on the window's thread, so it needs the dock. An *Arrange tool* key presses the
  tool's OBS hotkey by name (`stagekit.arrange.<tool>`, the names in `kInfo`): renaming one there
  breaks the key. *Blur All* uses the `BlurAll` and `ListBlurFilters` vendor requests.
  `com.techsavage1989.stagekit` and the action UUIDs under it must never change once
  the owner has keys on their decks: Stream Deck ties saved keys to them.
- **The updater installs only what it has checked.** `newest_release` takes a release only if it
  has a Windows installer *and* the notes carry that file's SHA-256 (the release workflow writes
  them); `install_script` downloads, compares the hash, and only then closes OBS. Keep that order
  and never offer a file without a checksum. Every page of releases is read (`kReleasesPath`,
  a hundred to a page) and the highest version taken: GitHub does not list them newest first as it
  says, and a build that read only the first 20 (beta83 to beta100) told the owner beta99 was the
  newest when beta100 was 33rd in the list. A release appears minutes before its Windows files
  (they are published in stages); such a release is passed over until they are up. The build
  knows which release it is from the newest notes file at configure time (`STAGEKIT_RELEASE`): the
  plugin's version number alone is the same for every pre-release. The download and install are a
  PowerShell script because the DLL cannot be replaced while OBS runs. The helper is started with
  `CREATE_NO_WINDOW` alone: with `DETACHED_PROCESS` (beta83 to beta85) the owner pressed *Update*
  and nothing followed. It writes its steps to `%TEMP%\StageKitUpdate\update.log`, and the dock says
  so when the helper ends while OBS is still open (`update_state`). None of the Windows side
  (WinHTTP, the script, the installer's silent run) has run in the tests; they cover the choosing,
  the ordering and the script's text.
- **Auto Capture's Windows code is only compiled by CI and never run by the tests.** The tests feed it
  stand-in windows through its `set_test_windows` call. The owner wants no list of games to maintain:
  detection is by loaded modules (graphics engine plus controller support, or full-screen), with a
  *Never capture* list for mistakes. Do not make a program list the default again.
- **The owner triggers camera moves from Stream Deck Filter keys on the camera source**, not from the
  dock's buttons. Move Source exists for that; keep its controls complete without the dock.
- **Deprecated libobs calls fail CI** (warnings are errors). Avoid `obs_sceneitem_get_info`; use the
  individual position, scale, rotation and bounds calls.
- **`.gitignore` is an allowlist.** A new top-level file or folder is ignored until it is added there
  and to `scripts/root-allowlist.txt`.
- Colour is handled as raw (non-linear) values inside multi-pass pipelines; the final draw follows
  `gs_get_linear_srgb()`. Blur instead blurs in linear light through sRGB views.
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
  reaches `main` (steps in the handoff). The release appears as soon as the Windows files are built
  and the macOS and Linux files are added after; tell the owner at that first moment. Never ask the
  owner to create tags by hand.

## Talking to the owner

Routine progress is plain, short text. Two markers are reserved and go at the top of a message in bold:

- `⚠️ ACTION NEEDED:` when the owner must decide, approve, supply something, or check something on
  their own machine. Say exactly what to do, where, what to report back and what is blocked. One
  request per line. Repeat an unanswered request in the next update.
- `🚀 PUBLISHED:` when a release is live and its assets are verified, followed by the link and a
  one-line summary.

Use them nowhere else. End every message with a short status: what is done, what is still running, and
what the owner needs to do, if anything.
