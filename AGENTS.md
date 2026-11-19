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

A fifth, at the owner's word (2026-11-18, beta182): "I want all scene filters in the plugin
referred to as scene filters, and likewise for source filters so they are easily distinguishable in
the list", in the form they chose, *Scene Filter: Fit to Space (StageKit)* and *Source Filter: Blur
(StageKit)*. Every StageKit filter's `*.Name` in `data/locale/en-US.ini` says one or the other
(values only; keys and ids unchanged). Scene filters: Move Scene Item, Show/Hide Animation, Match
Item, Link Visibility, Apply Layout, Fit to Space, Automatic Source Switcher, Scene Modes, Scene
Transitions, Scene Fit, Scene Standby, Scene Corners, Scene Shadow, Scene Blur. Source filters:
Blur, Zoom, Shadow, Glow & Outline, Corners & Edges, Auto Frame, Aspect Ratio Preset, Auto Capture,
Program Trigger (a scene is a source too), Automatic Window Switcher, Standby Image, Move Source,
Show in Scenes. Sources (Ambient Background, Cropped View, Standby Image) and transitions (Morph,
Fade and Zoom, Make Room) are not filters and keep their names. A filter already on a source keeps
the name it was given; the dock still names what it adds without the prefix (`add_effect`), and
the Stream Deck plugin knows a kind's own name now and before (`isOwnName`, `OLD_NAMES` in
`streamdeck/src/kinds.ts`). The rest of this file names filters without the prefix.

The trial scenes were renamed at the owner's word (2026-11-17, beta154: "Full capture should be
called Live / Holding should be called standby / Component should be called Chat Live"):
Full-Capture-Scene is *Live*, Component-Capture-Scene *Chat Live*, Holding-Scene *Standby*. Those
are scene names the three merge scripts write, not ids; the scripts refuse a collection that still
has the old names.

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
| `src/blur/` | Blur filter, and its multi-type blur pipeline (`blur-pipeline.*`, shared with Scene Blur: soft blurs, a mosaic in seven cell shapes, zoom, motion and a trail over time) |
| `src/ambient/` | Ambient Background source |
| `src/shadow/` | Shadow, Glow & Outline filter (id `stagekit_shadow_backdrop`, its first name): shadow or glow in a colour or the picture's own colours, outline, panel, rounded corners |
| `src/zoom/` | Zoom filter |
| `src/shape/` | Corners & Edges filter (rounds, cuts, notches or scoops corners; trims, softens and outlines edges; never changes the source's size) |
| `src/view/` | Cropped View source (part of another source as a source, so filters follow the crop) |
| `src/placement/` | Scene filters: Move Scene Item, Show/Hide Animation, Link Visibility, Match Item, Layout, Fit to Space (keeps one item fitted in a box, filling it or floating centred by shape); Move Source, Move Scene Item's code as a filter on the source it moves; and Show in Scenes (`show-in-scenes.*`), a filter on any source that is a switch for showing and hiding sources across scenes |
| `src/placement/scene-*`, `src/look/` | The scene filters: Scene Modes, Scene Transitions, Scene Fit, Scene Standby, Scene Corners, Scene Shadow and Scene Blur, each a filter on a scene that looks after its sources together. Each one's files are listed in its entry at the end of *Things that will bite you* |
| `src/dock/` | `window-layout.*` keeps OBS's window layout. One dock (Qt widgets, no `Q_OBJECT`): StageKit follows the item selected in OBS and drives Move Source filters on that item's source; its Standby & Ambient tab (`BackgroundDock`, `pageBackground`; a dock of its own up to beta151, called Background in beta152) holds the start image thumbnail. `arrange-tools.*` is the one set of line-up actions and options behind the dock, the hotkeys and the preview's right-click menu |
| `src/common/` | Shared helpers: `timing` (registers every kind and logs what is slow), easing and `Tween`, `SwitchRamp`, `KawaseBlur`, scene utilities, `item-transition` (the *Change with* control for filters that show and hide items), and the start image (global path + Start Image source). For the scene filters: `scene-filter-ids.hpp` (their ids), `holds-hidden` (is an item held hidden by a mode not showing), `go-to-scene` (*Switch to scene*), `filter-order.hpp` (`applied_index`, `first_filter`, `first_filter_of_kind`), `stand-in.hpp` (a Standby twin and the item it stands for) |
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
| `scripts/` | Repository checks (`check-root.sh`, `root-allowlist.txt`); `rebuild-collection.py` writes a StageKit collection from an old one and `upgrade-collection.py` brings a StageKit collection up to date; `merge-capture-scenes.py` adds the two layout scenes (Live, Chat Live) and `merge-holding-scenes.py` the holding scene (Standby) to a StageKit collection for a side-by-side trial. All four read one file and write another; the owner's collections are private and never committed |
| `docs/images/` | README imagery, produced by the tests |
| `.claude/settings.json` | Claude Code's project settings: allows the Workflow tool without a prompt (the owner, 2026-11-18). Only this file is tracked; `settings.local.json` stays ignored |
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
  (`set_tip`, below).
- **Every tooltip goes through `set_tip`** (`src/common/tooltip.hpp`, beta182), never
  `obs_property_set_long_description` itself: OBS shows a long description as one line as wide as
  its text, and the owner found "all tooltips in the plugin are hard to read because they are just
  one really long line of text". `wrap_tip` breaks it at spaces to `kTipWidth` (64) characters,
  counted in UTF-8 characters, keeping the breaks already in it and never splitting a word. Tested
  in `[tooltips: ...]` (`tooltips_test`, run by `tests/run-tests.sh` as a run of its own with `STAGEKIT_TEST_ONLY_TOOLTIPS`), which also opens
  every StageKit kind's window (filters, sources, transitions, with their modified callbacks run
  once) and fails on a line over the limit. Stream Deck pages are left alone: a browser wraps a
  `title` tooltip itself.
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
- **The StageKit dock is five tabs** (`stagekitTabs`: Place, Arrange, Copy, Effects, and since
  beta152 a fifth tab (*Standby & Ambient* since beta153, the owner's choice of name; *Background* in beta152), which was the separate StageKit Background dock; the owner: "id like the
  stagekit Background dock to be added to the primary stagekit dock as a new tab") under the
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
  `OBS_FRONTEND_EVENT_FINISHED_LOADING` and after every `SCENE_COLLECTION_CHANGED`
  (`window_layout_restores_after`, beta149: OBS puts a collection's docks up again, and the owner
  found the layout "doesn't automatically restore after saving and switching to another
  collection"), and saved at `SCRIPTING_SHUTDOWN`, before OBS takes its
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
- **What shows of the fitted item is worked out from the side it is kept towards** (beta147,
  `obs_sceneitem_get_bounds_alignment` in Fit to Space's own-cut look). Taken for centred, a squarer
  window kept towards the left was seen as cut off on its right and was fitted to a space of
  another size. This also changes existing filters with *Align in the space* on a side and *Use
  visible pixels* (the owner's Window-Component filter is one).
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
  every tick (`pairs_`, from the last look), inside Morph's 0.1 s. `pairs_` knows each item
  and twin by its id in its scene (`Held`: a weak scene and two ids), **never by holding the
  item**: an item holds its source, so a filter that holds the items of the source it sits on
  keeps that source alive for good. Up to beta147 it did, the hold was only let go at the next
  look, and no look comes once a collection starts changing (`scenes_loaded()`): OBS logged "Not
  all sources were cleared ... - DSLR" and closed itself at every change of scene collection
  ("Source Cleanup Error"). A filter must never keep a reference to an item of its own source
  past one call. The tests can say a collection is changing (`stagekit_test_loaded`, a global
  proc). And while it is ticked
  nothing else sets those items' transitions: `item_transitions_owner` (`g_stood_in`; asked per
  item since the scene filters' groundwork, so an owner can own an item in one scene only) makes
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
- **A filter skips with `skip_filter_input`, never `obs_source_skip_video_filter`**
  (`src/common/filter-input.hpp`, 2026-11-18). A filter taken off its source (the Filters window,
  on the window's thread) while the video thread draws it loses its parent part-way through the
  drawing, and libobs 30's skip reads `parent->info.output_flags` without looking: OBS crashed
  in Scene Shadow's and Scene Blur's render after they had already drawn and then gave up. The
  helper skips only while the filter still has a parent and a target, and otherwise draws
  nothing for that frame. It narrows the window to the moment between its check and OBS's read;
  it cannot close it (the parent is cleared without a lock a plugin can take). Tested in
  `[filters removed while they are drawn]` (`filter_remove_while_drawn_test`, alone:
  `STAGEKIT_TEST_ONLY_FILTER_REMOVE`, rounds per kind `STAGEKIT_TEST_REMOVE_ROUNDS`, 60 as it
  comes). `render_bare` in `src/dock/arrange.cpp` skips the same way.
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
- **Show in Scenes is a switch, not a trigger, and a command, not a hold** (beta174). Its on and
  off state is the shown and hidden state, so one Stream Deck Filter key toggles and shows which
  way it is. It acts when pressed and holds nothing afterwards: a switch that is off never stops
  Link Visibility (*Follow sources*) or Scene Modes from showing its source (a Standby stand-in
  never asked the switch; it still goes by `mode_holds_hidden` only).
  The owner: "show in scenes should not prevent any other filter from turning on or off a source
  later, at least thats how my logic works. for me the show in scenes was more of a control to
  turn a source on or off, not lock it down and prevent anything else from doing it". So a camera
  switched off with its switch comes back when a mode or a link shows it; to keep it off, hide it
  in that mode or take it out of the link. The other way round is unchanged: a press passes over
  an item a mode holds hidden (`mode_holds_hidden`), and *Follow what is shown* leaves such items
  out. Tested in `[show in scenes: a switch is a command, not a hold]`
  (`switch_is_command_test`, alone: `STAGEKIT_TEST_ONLY_SWITCH_COMMAND`). It acts on the `enable`
  signal only (never each tick, so a change by hand is kept) and ignores the state restored at load
  (`ready_`). The owner had a multi-action with a Source Visibility step per scene for this.
  Since beta147 a press is acted on in the filter's next tick (`press_`, the last of a frame), not
  on the thread that pressed: a Link Visibility acts on the same items from its tick, and two
  threads setting one item's transitions at once can each let go of the other's.
- **Show in Scenes' *Switch to scene*** (`go_to`, a scene name, empty as it comes; beta154). The
  owner: "Pressing the show button for any source that belongs to a specific scene automatically
  switches to that scene ... it must be universal to work with future scene collection designs".
  So it is a setting on any switch, naming any scene, not a rule about the trial scenes. It fires
  only on a press that shows the sources (turns the switch on, or off with *On means* hidden), in
  the tick after `apply()`, once `scenes_loaded()`; never on a press that hides them, a *Follow
  what is shown* correction (`correcting_`), the state restored at load (`ready_`), or *Do it now*
  (`kPressStay` in `press_`). It is skipped when
  the scene is gone, is a group, is already on screen, or when the scene on screen already holds
  one of the listed sources (or the filter's own source), in a group or hidden. The change is
  `obs_frontend_set_current_scene` (the preview in Studio Mode) from an `OBS_TASK_UI` task, which
  checks again there; the log says `switched to scene`. Test seam: the global procs
  `stagekit_test_scene_switch` (test mode, stands in for the scene on screen) and
  `stagekit_test_scene_switched` (the scenes asked for since the last call, `|`-joined). The real
  frontend path and Studio Mode have not run. The merge scripts set it: captures to Live, holding
  titles to Standby, column markers to Chat Live.
- **Show in Scenes' *Follow what is shown*** (`follow`, off as it comes; beta147): four times a
  second the switch is put the way its items are (`each_item`, the same items `apply` acts on,
  without bring-along items): on when all are shown, with `on_means` respected, left alone when
  there are none. The correction is `obs_source_set_enabled` from the tick with the thread noted in
  `correcting_`; `enabled_cb` passes over a signal from that thread, so correcting never shows or
  hides anything and a press from any other thread still acts. Do not make the guard a plain flag.
  It is for switches like the layout scenes' capture switches; on a switch whose source is hidden
  on purpose in some covered scene it reads off for good.
- **Link Visibility's *Lock hidden sources*** (`lock_hidden`, off as it comes; beta147): a listed
  item of the filter's own scene is locked while hidden and unlocked when shown, in the tick after
  the change, whoever made it; the hidden ones are locked when the option or the filter is switched
  on and once after loading (`sweep_due_`), never before `scenes_loaded()`. OBS's preview selects
  a hidden item on a click. Its own changes are known by the thread they are made on
  (`applying_`): a plain flag dropped a show made from another thread meanwhile, which left two
  captures shown and the new one locked.
- **A filter that shows or hides an item in a scene that may be out of view calls
  `set_item_visible`** (`src/common/item-transition.*`, beta147), after `set_item_transitions` or
  `apply_item_change`, never `obs_sceneitem_set_visible` itself. OBS starts the item's transition
  whether or not its scene is drawn, and only drawing ends it (`obs_transition_force_stop` alone
  does not clear it); left running, it makes the hidden source count as in use the moment that
  scene comes on, and a capture card set to let go of its device opens it and closes it again.
  Out of view the helper takes the item's transition off for that one change and puts it back
  after, so OBS starts none and the change is a cut. (Started and then stopped from outside, the
  first way, a Morph could not end the next change made in view.) And a change under way when its
  scene is left with a cut is never ended either: Link Visibility ends one left on a listed item
  twice a second (`end_stale_transition`: a show transition on a hidden item, or a hide on a shown
  one; only one that still holds a source: OBS can count a finished one as in use for good, and beta147 to beta153 ended that one twice a second all session). **Every change `set_item_visible` makes also ends the other way's
  transition if it still holds the source** (`end_held_change`, beta180): OBS stops it at a
  change but only says so. Shown and hidden again while away, an item carried a hide transition
  cut off earlier as one under way on a hidden item, which the scene counts in when it comes on
  and `end_stale_transition` takes for a change being made now. Link Visibility's half-second
  look caught it only while the item was shown in between (the chain test failed one run in
  twenty under load). This way's transition is never ended while it is off the item out of
  view: done so, OBS's counts went wrong for good. **libobs 30.0.2, on the test machine,
  miscounts by itself**: its `scene_enum_sources` leaves an item's own source out (`else if`)
  while the item's transition runs, but shows and hides count it, so a scene left part-way through
  a change leaves the source counted in use for good. 31.1.1 enumerates both. A test of "in use"
  after such a cut checks what the plugin leaves behind (an item transition counted in and holding
  the source) and `in_use` only from libobs 31 (`[capture scenes ...]`'s chain,
  `capture_chain_test`, alone with `STAGEKIT_TEST_ONLY_CAPTURE_CHAIN`). Found by driving the generated collection headless with the new build; the tests had
  waited for every change to end before the next step. Items a Standby filter owns are passed over. Link Visibility, Show
  in Scenes, Scene Modes and the capture hide (`hid_items_press`) go through it; Program Trigger,
  the two switchers' following, Show/Hide Animation and the stand-ins do not yet (the stand-ins through it broke the Standby
  transition tests, which draw a scene that is not on the output: to it, out of view). A Standby filter forgets the
  items it owns when it is taken off its source (`filter_remove`): kept by address, they made a
  source created later at the same address "owned", so its hides were never cuts. A source with sound is finished hiding in the scene's
  audio mix, so it may still be in use for one audio tick when the scene comes on; not measured.
  The tests had no sound until the layout section (`obs_reset_audio`), so no item transition ended
  in them: test that a fade is used by the source still being in use part-way.
- **One scene per layout** (Live and Chat Live; Full-Capture-Scene and Component-Capture-Scene
  until beta154) (beta147; the owner, 2026-11-16: "having two primary scenes for both
  component and full layouts that contain all the live sources is fine"). Each layout scene holds
  every capture, stacked, one shown, each with its own Fit to Space; a Link Visibility (only one,
  `lock_hidden`, Morph) per scene and a Show in Scenes (`follow`, the layout scenes listed, Morph)
  per capture. A layout change is still a change of scene with the Morph transition, untouched.
  Literally one scene was looked at and put aside: OBS will not transition a scene to itself, an
  item has one eye per scene (a layout change would bring back a camera switched off), and the
  owner's Morph would become several animations. A Morph from a private copy of the scene to the
  scene itself was shown to work in a headless test (not committed), not on Windows.
  `scripts/merge-capture-scenes.py` writes the trial collection (all fifteen old scenes kept, two
  added); removing the old scenes is a later step. Its plan (which old scene each item is copied
  from) is a table for this owner's collection, not a general tool. The changing of a capture in
  a layout scene is tested in both filter orders (`[layout scenes: changing the capture leaves the
  rest where it is]`, last in the test file because it turns sound on): camera and chat box move
  at most 0.2 px. A new test that changes visibility between frames should try both orders.
  **One holding scene** the same way (2026-11-17; the owner: "The start screen, Intermission,
  BRB, and Stream ending are all affectively the same design. Can this also be combine into a
  single scene"): `Standby` (Holding-Scene until beta154) holds Stream Start, Be Right Back, Stream Ending and (since beta153)
  Intermission as modes, each a title group plus extras; Link Visibility-Titles (only one) over
  the titles, one Link Visibility *Follow sources* (then called *Follow the first source*) per mode (first a *Together*, which a
  camera switch showing one member would flip), a Show in Scenes-Mode (`follow`) on each title.
  One Link Visibility reacts to another's change because each passes over only what it does on
  its own thread while acting (`applying_`). Written by `scripts/merge-holding-scenes.py`; the GBA
  layout is a second column mode of Chat Live, written by
  `scripts/merge-columns.py`, switched by two transparent marker sources (*Column: Chat*,
  *Column: GBA*) because every visible item of a column is worked by the camera or chat switch.
  Both driven headless with the plugin (every mode and column, camera and chat off and on, two
  keys at once, off screen, nothing hidden left in use).
- **Link Visibility's *Follow sources*** (mode 2, `LinkMode::Follow`, saved as 2; first *Follow the
  first source*, 2026-11-17, the owner: "can the intermission screen be merged with the other hold
  scenes? also the GBA scene?"; several leads since beta169, the owner, of five such filters on
  Component-Capture-Scene, one per capture, each followed by *Column: Chat*: "I have all these
  filters effectively just to make the Column chat follow all my main live sources. I think the
  follow first source option should be changed to a follow source option, where one or more
  sources can be added, then the linked sources can be added"). **Lead sources** (`leads`, a
  PickList) lead; `items` (shown as *Following sources* in this mode) follow, any lead among them
  left out. **With `leads` empty, the first entry of `items` is the one lead** (`split_follow`):
  every filter saved before keeps its meaning, and nothing rewrites its settings; the window's
  *Make the first a lead* (`take_first`, shown only then) moves it on a press. What the leads say
  is `leads_say_shown`: *Any lead is shown* (`lead_rule` 0, the default; append-only) is shown
  while one is, hidden once all are, so going from one capture to another, in either order within
  a frame, never hides the followers in between; *All leads are shown* (1) needs every lead found
  in the scene. Leads not in the scene are passed over; none there leads nothing. When that state
  changes (the tick compares it with the state last followed, `acted_lead_`; the state found at
  the first look after start, enable, loading or a change of the lists or the rule is taken as it
  is) the followers are shown, whatever a Show in Scenes switch on them says (a switch is a
  command, not a hold; see *Show in Scenes is a switch*), or hidden. While the leads say
  hidden, every tick, a follower shown by anything else is hidden again. A change to a follower
  moves nothing; changes from *Also in other scenes* are not followed in this mode, only the
  leads' state is sent there. Every listed source, leads too, is watched, locked by *Lock hidden
  sources* and offered under *Settings for*. Show in Scenes asks `mode_holds_hidden`
  (`src/common/holds-hidden.*`, which asks every registered kind; this one registers
  `link_visibility_holds_hidden`: every
  enabled mode-2 link of that scene that has the source among its followers has its leads saying
  hidden) and passes over such an item when showing, never when hiding, and leaves it out of
  *Follow what is shown*; `mode_settling` (here `link_visibility_settling`: a leads' state not yet followed) makes that look
  wait, or a camera switch read off for the tick between a mode change and its extras. Link
  Visibility keeps a registry of live instances (`g_links`) and copies what it needs out under
  it: no lock is held while another filter is asked. This is what lets the camera switch (every
  scene) live with modes holding camera views: the switch never shows a camera view a mode holds
  hidden. Tested
  in `[modes in one scene: follow the first source, and a camera switch]` (filters saved the old
  way), both filter orders, off screen too, and in `[link visibility: follow sources, several leads
  and their followers]` (alone: `STAGEKIT_TEST_ONLY_LINK_FOLLOW`; five leads, a chat follower, a
  capture changed by its Show in Scenes key with *Show only one* and Morph, two changes in one
  frame both ways through a tick callback, *All leads are shown*, a filter saved without leads, the
  window; both orders, on and off screen, with sound as in OBS, so it runs last). Up to beta169
  a follower a Show in Scenes switch had off (`show_in_scenes_holds_off`) was left hidden when
  the leads came on; the owner's DSLR, whose capture switch reads off while another capture is the
  main one, never came back with Intermission Text. Removed at the owner's word (see *Show in
  Scenes is a switch*). A Standby stand-in is not shown for an item a mode holds
  hidden (`stands_in` in `standby.cpp`): the owner's DSLR, in the Intermission mode, had its
  standby image over every other mode in the first build. A link that changes mode looks afresh
  (`acted_valid_`).
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
  told so, and that Show/Hide Animation is the one that travels. **Link Visibility has it too**
  (the owner: "Link visibility scene filter should have additional transition options including
  a fallback or other items transition for items that don't have partners"): *Settings for*
  lists the sources of both its lists (`add_item_change_properties(..., "items,elsewhere")`; the
  list setting may name several, comma-separated). A filter saved with the plain two controls
  reads as before: they are the same `transition` and `transition_ms`, and `hide_transition`
  is *The same as showing* as it comes. Under Morph the filter's window offers Morph's
  *Unpaired sources* (`unpaired`) in place of *Other items* (`others`, which only means
  something in a change of scene and is not copied), and, for another transition there, that
  transition's own settings, kept as `show|stagekit_morph|unpaired|<kind>|<its setting>` and
  handed to the item's Morph inside its own settings like `links`. An old filter's `others` of
  Fade and zoom reads as *Unpaired sources* Fade and zoom. Show Source in Use and Program
  Trigger still have the plain two controls; give them this one when it is asked for, not a
  third kind. The controls are rebuilt only when their mark (`change|sig|...`: the source chosen,
  the kinds, Morph's *Unpaired sources*) is missing, and the mark is added **after** the controls
  are made (beta163): making them sets defaults, and a mark taken before did not match at OBS's
  first round of callbacks, which then took the controls off while OBS went through them (a
  use-after-free that crashed one run in four or five; valgrind shows it every time). A window
  that rebuilds its controls from a modified callback must match at that first round; run its
  test section under valgrind.
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
  unticking goes back to when no size was ever kept: the owner's chat box had been shrunk to its
  space before the box could be unticked ("it shrinks"). **A size kept before (`keep_k`, saved;
  set by hand or read when first kept) wins over it** (beta194, 2026-11-18): the owner's camera,
  set by hand to 0.4765 of its source, came back at 0.4899 (`orig_k`) after *Resize to fit* was
  ticked and unticked. `keep_k` is only ever written while unticked, so it is never a fitted size. It is read again
  at every update (`saved_keep_item_`), so a rename of the source (which rewrites `keep_item`)
  keeps it.
- **Fit to Space's kept size is read from the item on three occasions only**: when the filter is
  given another item, when *Resize it to fit* is unticked (then the size last kept, else the size
  from before the filter), and after a resize by hand, which starts only on an item that is
  selected, showing, and whose source has kept its size for a second, and **counts once the box
  has kept still for `kHandStill` (0.3 s)**, its source's size not changing meanwhile (a change
  starts the wait again) (`hand_on_`, beta194); the item is left alone
  meanwhile and the resize is followed to its end even if the item is no longer selected. Read on
  every tick (up to beta194), a drag and the dock's Stop resizing at edges glide after it
  (`step_glides`: the allowed size for one frame, back to where it was let go, then a glide there)
  were read once a frame, 53 times in the test; the owner's log had "went from 0.9021 to 0.5563
  ... (resized in the preview)" and back within two frames, then a run of small steps, each one
  making the whole column be worked out again. Any other change of settings leaves it. The owner's
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
  setting?" It is looked at when first met, half a second after its picture last changed size, and
  every three seconds while it sits where it was put; also while its box hangs past the screen's
  edge (beta194: the source is drawn by itself to be measured); and a cut under a fiftieth of the
  picture is taken for measuring error. The picture is the source less its crop and, with *Ignore
  shadows & outlines*, the shadow's room (`own_sw_`), so a change of a shadow's room alone keeps
  what was seen (beta194: sized in screen pixels, the room changes at every resize, and for half
  a second each time a letterboxed window was fitted with its empty margins). Sizes kept
  (`keep_k_`, `orig_k`) are scales of the source, so they hold whatever part shows.
- **Two Fit to Space filters in one scene go by each other's destinations** (`Claim`, `claim_set`,
  `claim_of` in `src/placement/fit-item.cpp`): each says where its item will be once it has got
  there and whether it resizes it, and the other takes that for the obstacle, a claimed group as
  one thing. A kept-size item with something that resizes on one side of it and something firm on
  the other goes against the firm side at once (`kept_in`, `soft_l` and so on); centring between
  them never came to rest, because the other made room each time. The owner's game and chat box
  shifted "a number of times" after a scene switch or a camera being hidden. Which side gives way
  is judged by which side of the space's middle it lies on, never by whether it touches the space:
  judged by touching, the two filters chased each other without end.
- **A resized picture that has no use for more height is packed with a kept-size item above or
  below it, top first** (`grows_x`/`grows_y` and `leans_x`/`leans_y` on `Claim`, `lean_axes`, `trim_for_kept` in
  `src/placement/fit-engine.cpp`; beta194, 2026-11-18). The owner, of a Scene Fit with their camera
  (resized, held by the column's width) above the chat box (keeping its size): "I don't think use
  visible pixels is working as intended. the chat box is more than 30px from the visible border of
  the camera box, but the bounding box of the camera box is exactly 30 from each edge". Measuring
  was right (a claim is the picture, not the box); the chat box went to the far side (the canvas's
  bottom) because the camera resizes, and the camera's space then reached down to it, its picture
  centred with 22 and 24 px empty above and below (test canvas). Now a resizing fit's claim says
  whether more width or more height would make its picture larger (it leaves room the other way);
  only along an axis it would grow in is it a side that gives way (`soft_sides`). Along an axis it
  would not grow in, a kept-size item lies at the top of its space and says so in its
  claim, and the resized fit cuts its space down to its picture at the top: whichever of
  the two is above stays against its edge and the other comes up the gap from it, in one move, and
  nothing answers anything after (5 s watched). Packing towards the item's side instead crept, one
  glide at a time. Only with *Align in the space* in the middle along that axis on both; a side
  that does give way still decides as before. Same in Scene Fit and Fit to Space, with *Use visible
  pixels* or without. Tested in `[fit visible pixels: ...]` (`fit_visible_test`, alone:
  `STAGEKIT_TEST_ONLY_FIT_VISIBLE`; a Cropped View of an async camera). The owner's flashing chat
  box in the same scene was not reproduced. **Vertically only**: side by side nothing leans and
  nothing is trimmed (`leans_x` is never set, `soft_sides`' `beside` ignores `grows_x`), so a 4:3
  game held by its height beside a kept-size chat column still counts as giving way, the column
  stays against the screen's edge and the game centred in the rest, exactly as at beta181. The
  first build packed side by side too and moved that layout (column 73 px off the right edge on
  the test canvas); tested since in the same section. Horizontal packing would be a new layout
  rule: only at the owner's word.
- **A resized picture's box is its picture where another resized fit lies beyond it** (`trim_to_picture`
  in `src/placement/fit-engine.cpp`, beta194, 2026-11-18). The owner's GBA mode: two 3:2 handheld
  windows and a 16:9 camera view share a column, all resized, all held by its width; the camera's
  box took about 1250 px of the column's height while its image used a third of that, and GBA 1
  looked squeezed to a sliver. The claim was always the picture; the box was the whole roomiest
  space (`free_space`'s tolerance). Now, along an axis the picture does not fill, with the claim
  of another fit that **resizes** its item beyond it on that axis (above or below, or beside;
  `gives_`), the goal is cut to the picture
  where it lies by *Align in the space*: nothing moves on screen, claims are the same, the box
  shows what the item takes, and the saved bounding box (what a source with no image yet holds,
  `holds_room`) is the picture too. `grows_x`/`grows_y` are judged on the space before the cut
  (`space_grows_*_`). Alone in its space nothing is cut, and **a kept-size item beyond it does
  not count**: it has no use for the room, and the space is the item's own. The first build cut
  against any claim, and in a layout scene a 4:3 capture beside the kept-size camera and chat
  column got a 427 px box in the 480 px capture's space (`[layout scenes: changing the capture
  leaves the rest where it is]`, five checks in both filter orders: the space written a pixel
  larger was not the same one, and a squarer capture shown again or switched on out of view was
  53.33 px off its space). That section runs alone with `STAGEKIT_TEST_ONLY_LAYOUT`
  (`layout_scenes_test`; it turns sound on itself). **The cut box holds only while the
  picture is the shape it was cut for** (`reshaping`, `room_goal_`/`room_cur_`, glided with the
  goal): a picture of another shape (a window dragged wider, a capture changing resolution, an
  Auto Frame settling) is given the space before the cut in `apply`, and claims from it, until the
  new shape has held 0.5 s and the next solve cuts again from there. In the cut box OBS could only
  shrink it, for that half second and the glide after: the rule that Fit to Space gives OBS the
  space so a changing picture never lags still holds. A window switcher's change is excluded (it
  takes the new shape at once). Tested in `fit_owner_test`: the camera's view turned from 16:9 to
  taller is 120 px high four frames later (88 squeezed into the cut box without it). A goal that is the picture changes only when the picture does, which
  is most of G4 below. Test canvas: camera box 120.0 high for an 88 px picture before, 88.4 after.
- **"Its sources did not settle in 4 passes"** (the owner's logs, within 10 s of start and after
  mode changes; beta194). Two causes, both fixed: the goals of a column's resized entries were
  their spaces and chased each other's edges geometrically (cut to the picture, above), and an
  entry whose space ends at one listed after it (the camera between the two windows) waited a pass
  for that one each time, so from the owner's saved slivers a column of three needed six passes.
  Passes now go down the list and back up it in turn, eight at most. Not to be raised further
  without a test that needs it: a tick that does not settle is kept as it is and finished in the
  next. Tested in `[fit: the owner's layout scene (G1 to G5)]` (`fit_owner_test`, alone:
  `STAGEKIT_TEST_ONLY_FIT_OWNER`, a quarter-scale stand-in of their scene, generic names): both
  modes, six switches, Takes space first in both modes, Resize to fit ticked and unticked, a resize
  by hand and the dock's glide, each with 5 s at rest (nothing moves, nothing logged).
- **Space priority decides which fit makes room for which** (`space_priority`, `SpacePriority`,
  saved by number, append-only: 0 *Shares space*, 1 *Takes space first*, 2 *Makes room*; and
  `list_priority`, *Priority by list order*, a Fit to Space's own and a Scene Fit's for all its
  entries; `Rank`, `outranks` in `src/placement/fit-engine.hpp`). The owner: "fit to higher in the
  list would have higher priority", then "a selector to set source as dominant, equal, and less",
  then "a tickbox that would disable the selector and make list order the priority". Each claim
  carries its fit's `Rank`, found afresh at every `prepare` (the filter's `applied_index`, a Scene
  Fit entry's place in *Fitted sources*), so moving a filter up or down counts at the next tick and
  is followed with a glide (`observe` sees the obstacles change; nothing listens to
  `reorder_filters`). `FitScan::obstacles` with `mine`: a claim `mine` outranks is in the way
  **only by what that fit holds whatever happens** (`Claim::least`, beta194): for a fit that
  keeps its size, that size round its home (its anchor), on the screen; a resizing fit holds
  nothing and is not in the way, as before (it shrinks to make room, so it is never covered).
  Never by where it is going (`box`), which follows the higher fit's claim. A claim that outranks
  `mine` is firm (`gives` false, `above`). Two ranks neither outranking the other go by each
  other's claims exactly as before. So between two fits of different standing the dependence is
  still one-way and there is nothing to chase: the higher goes by the lower's home and kept size
  (which change only when it is moved by hand, resized by hand, shared, shown or hidden), the
  lower by the higher's claim. Up to beta194 a kept-size lower was not in the way at all either,
  and the owner's game on *Takes space first* covered the camera and the chat box beside it
  (test: 576 and 1278 px² of overlap), which cannot shrink. The owner: "they aren't supposed to
  ever cross borders of other sources unless specifically allowed with the overlap setting"
  (2026-11-18). Priority decides who gives way, never overlap. Two variants were built and taken
  out: a resizing lower's home as a point in the way (it changed the shape of the higher fits'
  spaces, and two higher ones sharing a column then crept for good, half a pixel a tick, in
  `[scene fit groups]`), and passing over a home lying inside the higher's own claim (hysteresis:
  the higher grew over it while that item was hidden and never gave it back). Consequence: a
  higher fit now moves when a kept-size lower item is hidden or shown (its home comes and goes),
  as a fit that shares does, and comes back exactly. Rules: both
  `list_priority`: the higher in the filter list (then the entry order) wins; otherwise a ticked
  one counts as *Shares space* and the levels decide. An outranking fit that covers a lower fit's
  anchor would otherwise be ignored by it (a box holding the anchor is not in the way):
  `free_space`'s `firm` list makes such a box count and looks for the space just off each of its
  sides, for that answer only; the anchor itself is not moved, so the lower item goes home when
  the higher is hidden. "Honouring a lower fit when it costs nothing" was tried on paper and left
  out: a higher fit that looks at a lower one's claim at all depends on it again, and a centred
  picture then crept towards the wall one glide at a time. The default is *Shares space*, list
  order unticked, for new and saved filters alike, so no layout changes by itself. Only the free
  space claims; a fit in the whole scene or an area takes no part. Tested in `[fit priority]`
  (`fit_priority_test`, run alone with `STAGEKIT_TEST_ONLY_FIT_PRIORITY`).
  **With All sources** (beta162) each unlisted entry of a Scene Fit has its own *Space priority*
  (`fit|<source>|space_priority`) and, by list order, comes after the listed ones, from the top of
  the scene down. `set_claims_firm`'s step out of a claim passes over an `above_` claim and leaves
  it to `free_space`'s `firm` sides; an outranked claim is in `boxes_` only as its `least`. Tested
  at the end of `[fit priority]`: All sources, the game taking its space first, a source put down
  inside it (resized: unchanged by beta194's `least`, which only kept-size fits have).
- **Fit to Space does nothing to a hidden item** (`hidden_` in `src/placement/fit-item.cpp`): no
  placing, anchor and kept size untouched, so the other filters take its room. Its room stays on
  record (`holds_room`, beta147): nothing goes by it while the item is hidden (claims are only asked
  for items that show), but it counts from the moment the item is shown, not from its filter's next
  tick. Cleared before, a capture just shown in a layout scene was taken for something firm for one
  frame when the Link Visibility and the camera's filter ticked before the capture's, and the camera
  and chat box jumped 27 px (test canvas). A showing item with no image yet holds that room too (or
  its saved bounding box if never fitted; `box_asked_`), so a display capture before its first
  frame does not send the column beside it to the middle of the screen; the room is forgotten when
  the filter is given another item or the item is placed by a tool. A generated collection must
  give each capture of a layout scene a bounding box the size of its space. Shown
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
- **A fit switched on takes the layout as it is: its home** (`FitEngine::switched_on`, `home_`,
  `home_space_`, `home_box`, `home_from_item`, `at_home_`; beta194). The owner, 2026-11-18:
  "when I set up some layouts as desired, for example my Dolphin-GBA Link layout, manually
  positioning and sizing things with the fit to filter disabled, then enable the filter, even with
  resize to fit enabled, everything changes size." Causes: the first tick on went on from the
  last session (`have_keep_`, `keep_k_` from before, the round-2 untick restoring a stale `keep_k`
  or `orig_k` when *Resize to fit* was unticked while off, anchors and claims from the old
  layout; a filter loaded off read its saved `keep_k` and anchor), and a resized item was always
  given the largest picture its space holds, never the size it had. Now an off-to-on edge seen by
  the tick (`was_off_`, Fit to Space in `FitEngine::tick`, Scene Fit for all entries in its
  tick; never `!scenes_loaded()`) drops all that: anchor from the item with no wait, the kept size
  read off the item, no glide, and a **home**: the box the whole picture has now (before
  *Use visible pixels* cuts it, so a shadow's room or a later look changes nothing) and, found
  over the next 0.5 s on screen (`home_settling_`), the free space round its place with every
  other fit's item at *its* home (`Claim::home`, `Obstacle::home`). In `solve`, while that space
  is the same again (3 px, `kHomeSlack`) and the picture at home keeps the gap (same slack) to
  what counts and the screen's edges, the goal is the home box exactly, no trims, home to home
  without a glide; otherwise it fits as it always did, and comes back when the change is undone.
  Comparing against the others' homes, not where they are now, is what makes a column return:
  each entry decides alone and the departed ones do not hold each other off. A resized item
  grows beyond its hand size only while the space round it differs (a neighbour hidden), as
  before. Retaken on a move by something else, a resize by hand (now watched for resized items
  with a home too) and a copy tool's placing; dropped for another item; unticking *Resize to
  fit* with a home keeps the home's size. Saved as own settings (`home_item`, `home_x/y/w/h`,
  `home_space_*`, `home_cut_*`, in `kOwnSettings` and Scene Fit's `kFitKeys`, renamed with the
  source), so a filter loaded on goes by them; `home_cut_*` stands for what shows until the item
  is looked at (else a window's empty margins were its home for half a second and the column
  left home). A filter that was never off has no home and fits as before. Also: what shows of
  the fitted item is now measured when its box hangs past the screen's edge (the source is drawn
  by itself; it was never looked at, and a window letterboxed beyond the gap kept its margins).
  The review of it found more, fixed before release: the space round a home is found only once
  what shows of the item has been looked at and its shape counted, and again from the start while
  another fit's home changes (`home_settle_`, `ready` in `motion`, `homes_changed` in `observe`:
  switched on with nothing measured yet, after OBS started with the filter off, every home space
  came out a window's margins too small and every item left home); a fit that takes its space
  first counts where it is, not at its home (`above_`: else it grew over the lower fits at their
  homes); the home is read off OBS's own drawing transform (`home_from_item`: OBS places a picture
  in its bounding box by whole pixels, and the item moved by up to a pixel the first time its box
  became the picture's); a kept-size item at home is not pulled the full gap in (`apply`,
  `publish_claim`: a home within the 3 px slack of the edge moved); during a window switcher's
  change `home_box` goes by the shape it is changing to (`switching_`: the new window was squeezed
  into the old one's home box); at home a picture of another shape is handed the space round it as
  away from home (`found`, `reshaping`: only when it no longer fits its home, so what shows of it
  being measured again changes nothing on screen); the shadow filter's own move of an item when its
  room changes is not taken for a move by hand (`set_k_`: the shift and box growth are added to
  `set_x_`, `set_bw_` and the rest, else an item grown into a hidden neighbour's room took the
  grown place for its home); and a Scene Fit with *All sources* counts its sources at the first
  tick on (`count_now_` while off), so a source added meanwhile gets a home with the rest. Tested
  in `[fit: switching it on keeps a hand-made layout]` (`fit_enable_test`, alone with
  `STAGEKIT_TEST_ONLY_FIT_ENABLE`): also a filter made switched off (never ticked on), a shadow in
  screen pixels on a column window, a bottom window a pixel inside the gap, a game set to *Takes
  space first* growing as a neighbour is hidden, and *All sources* with a source added while off.
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
    overridden. `pick_list_on_change` is a chain, not one slot: the fuller *Change with*
    installs its rebuild there each time a window is built, and a kind that needs a control
    rebuilt in place registers `pick_list_on_change_for(<its id>, ...)`, which runs after it.
    Never replace the first: *Settings for* stops rebuilding.
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
    *Alone* goes by *Unpaired sources* (below), *Waiting* is a hiding item left as it is and a
    showing one not drawn yet.
  - **Unpaired sources** (`unpaired`, a string, `morph_fade` as it comes; in Morph's own window
    and, copied, under Morph in every fuller *Change with*): how an *Alone* item comes or goes.
    `morph_fade`, `morph_zoom` and `morph_slide` are drawn by `alone_cb` (the slide off the
    nearest edge of the item's scene, `item_morph_item` + `item_off_nearest_edge`, inside the
    item's own frame: `gs_draw_sprite_subregion`); `none` is a cut (shown whole, or gone, once
    *Alone*); any other transition id (`item_transition_is_fallback_kind`: not Morph, not a
    stinger) is a private transition of that kind (`fallback_`) with its settings from
    `unpaired|<kind>|...` (a slide's or swipe's *Off the nearest edge* made a direction), set up
    in `start_alone` as a scene starts an item's own (nothing to the item when shown, the item to
    nothing when hidden) and driven in `OBS_TRANSITION_MODE_MANUAL` by the change's own clock,
    not eased (it eases itself). It is never driven to 1: at its end OBS's transition draws its
    first side again, which for a hide is the item, so the last frames are drawn by Morph itself;
    it lets go of the item at the next start or stop (`let_go_fallback`). A Morph saved before
    reads `others` (Fade and zoom stays so; Slide, then a fade for an item, stays a fade). Which
    side is the item's is still read from the transition's sources (`alone_in_`). Tested in
    `[link visibility: show and hide transitions, and Morph's unpaired sources]`, each way in and
    out while a pair morphs beside it; not seen on Windows.
  - Pairs are found whoever hid and showed the items. Every Morph says when it starts
    (`item_morph_started`); on the next tick the ones that are an item's are looked up in the
    scenes (`find_items`), and a hide and a show in one scene within `kGraceNs` (0.1 s) pair up
    when their sources are linked in *any* Morph's settings (`g_said`), or are the largest of each
    at 15 % of the screen or more (`pair_main`). Only in a scene that is showing and for items
    straight in it: a group would grow to the layer's size.
  - **One item can be in two changes** (beta158). An item left over after the pairing changes into
    (or out of) one already paired, in this tick or within `kGraceNs` before (a `Paired` entry is
    offered again with `taken` set, while its item is still hidden or shown as it was), by the same
    two rules: linked, then the largest left over with the largest of the other kind. Each such pair
    is a job and a layer of its own. The owner's Live scene: showing a capture hides the full-screen
    DSLR (Link Visibility, *Show only one*) and brings back Square Camera (a *Follow the first* link
    per capture), and their Morph links say DSLR ↔ Square Camera. The camera took the corner view,
    as the owner intends, and the capture, left over, popped in at the end (faded in by itself in
    beta156 and beta157, `alone_in_` below): "when I switch to window or game source from DSLR by
    hiding DSLR, it doesn't smoothly morph and transition into the new source like all the others
    do". Now the capture comes out of the DSLR as well, and whichever order the three changes arrive
    in, the result is the same. Tested in `[morph between items]` (both ways, the capture a moment
    late, a small unlinked item still fading by itself) and in the first case of `[a capture shown
    in the camera's place ...]` (two layers at once; the capture's own transition draws nothing);
    not yet seen on Windows. **Joining as the main image needs a third of the screen on both
    sides** (`kJoinShare`, beta159), not the main image's own 15 % (`kMainShare`): at 15 % the
    owner's chat box (Chat Box Group, 16 %) joined the full-screen DSLR's change in Chat Live, where
    showing DSLR hides Window Capture and the chat column (the owner: "the camera is morphing from
    the chat box"). Their collection by then had no Morph links at all, and DSLR in Chat Live.
    Joining by a link has no size rule. Every pairing is logged once (`start_job`'s `why`:
    `Morph: 'A' changes into 'B' in '<scene>' (linked | main image | ..., joining a change under
    way)`): ask for the log before guessing which rule paired two items.
  - The change goes by the shown item's transition's clock, and the tick ends it at the very frame
    that transition draws the item itself: OBS's video time is the same in a frame's tick and its
    drawing. Do not time it any other way, or there is a frame with both or with neither.
  - Scene items are added and removed from a tick callback, never from inside a signal or while
    drawing. Everything stops at `item_morph_stop()` (OBS closing, unload).
  - **Nothing here holds a scene, and an item only while its change is drawn** (beta170). An
    item's Morph transition is known (`Entry`) from its start until OBS says it stopped, and OBS
    says that only when the transition is drawn to its end with its sound ended: never for a
    scene out of view, never in the tests before they turn sound on. Up to beta167 each entry
    held its scene and item, so a scene held its items, they their sources and transitions, and
    each transition kept the entry that held the scene: all of it stayed until the entry was
    given up on, 20 s later. A change of collection or exit in that time found them held (OBS's
    "Not all sources were cleared"; in the tests "N source(s) were remaining" and "Double
    destroy" at shutdown, 39 lines in the new section). Now an entry keeps a weak scene and the
    item's id, and each tick finds the item afresh and lets go of it as the tick ends (`Held`,
    `reach`, outside `g_mutex`: OBS calls in here holding a scene's lock). The nearest edge a
    transition needs is worked out in the tick when the entry is made (`item_morph_edge`); the
    item is never handed out. A change (`Job`) holds its two items and sources while it is drawn,
    and a weak scene; it ends when its scene is removed, and the layer lets go of it in
    `end_job`, not when OBS destroys the layer. Everything is let go of (`let_go_all`) at
    `SCENE_COLLECTION_CHANGING` and `CLEANUP` (`item_morph_let_go`, from the frontend callback in
    `plugin-main.cpp`: OBS clears the collection at once, no tick comes between), in a tick while
    `!scenes_loaded()`, and at `item_morph_stop()`. Tested in `[the item Morph lets go of scenes
    and sources]` (`STAGEKIT_TEST_ONLY_MORPH_HOLDS`; `stagekit_test_item_morph_held`, a global
    proc, gives the entries and changes held) and at the end of the Link Visibility section.
  - Anything that goes through a scene's items to fit round them, line up with them or snap to
    them must pass the layer over (`is_morph_layer` in `src/common/scene-util.hpp`): Fit to Space,
    the arrange tools' obstacles and the Morph transition's own plan do.
  - A filter with the fuller *Change with* (Show in Scenes) has its own links (`morph_links`, made
    with `add_morph_link_properties`), handed to its items' Morph transitions as their `links`
    setting, shown only while Morph is chosen. The other three filters go by the links made
    elsewhere and by the main picture.
  - A comparison of two times taken from the clock at different moments must not be made by
    subtracting unsigned numbers: the first build ended every change in the tick that began it.
  - **An item shown with nothing to change from fades in** (`alone_in_`). OBS hands a
    transition's drawing callback a see-through placeholder for the side with no source, never
    nothing, so which side is the item's is read from `obs_transition_get_source`, never from
    which texture is there. Up to beta154 `a ? a : b` drew the placeholder: a shown *Alone* item
    drew nothing for the whole change and appeared at once at its end (a hidden one faded, which
    is all the tests looked at). The owner saw it when one hide met two shows: "when I switch
    from DSLR to windows ... the DSLR morphs into the square camera as I intend ... the source
    just instantly appears", and with the camera linked to a capture instead, "now the square
    camera just pops in". Their settings were not at fault. Tested in `[a capture shown in the
    camera's place ...]`, last in the test file (it needs sound, as the layout section does).
- **Fade and Zoom** (`stagekit_fade_zoom`, `src/transition/fade-zoom.cpp`; the owner: "also add
  fade and zoom as a transition") is a transition like OBS's own, so it is in every list of
  transitions by itself. One side (an item): it fades as it grows or shrinks. Two sides (a change
  of scene): the old scene stays underneath and the new one fades in over it as it grows, so the
  picture is whole all the way. *Smallest size* stays at 100 % or under: larger would be cut off
  at the item's frame. Which case it is goes by the transition's sources (`has_a_`, `has_b_`), not
  by the textures it is handed (a placeholder stands in for a missing side): up to beta154 an item
  hidden with it was drawn as a change of scene into nothing and stayed whole until it went.
- **Make Room** (beta175; the owner: "a transition in which sources move off screen on the closed
  side of the screen based on other objects morphing into the scene? can it be its own transition,
  but also an "other" transition to be used for objects with no link set up in morph? ... the
  sources would move off screen, on the closest screen edge but away based on the incoming morph").
  Three ways in, one rule:
  - Morph's *Other items* `Others::MakeRoom` (3) and *Unpaired sources* `kUnpairedRoom`
    (`morph_make_room`, in `item-morph.hpp`), both appended; settings `room_way` (`RoomWay`: 0 away
    from the morph, 1 nearest edge only, append-only) and `room_fade` (off as it comes). Rules
    offer it too (`kLastOthers`).
  - The transition `stagekit_make_room` (`kMakeRoomId`, in `morph.hpp`): **the Morph class with
    `room_` set**, registered in the same loop of `register_morph`, so it is in every transition
    list by itself and in `add_kinds`. `update` fixes `others` and `unpaired` to Make Room; its
    window (`get_room_properties`) has no *Other items*, *Unpaired sources* or rules. Do not fork
    Morph's logic for it. As an item's transition it **never pairs** (`MorphSaid::pairs` false:
    `can` and `largest` pass it over), and `add_own` leaves out its pairing and link controls in a
    filter's window. It is not a fallback kind (`item_transition_is_fallback_kind`): Morph's own
    `morph_make_room` is.
  - **The direction rule** (`room_edge` in `morph-paint.*`): the middle of what morphs in
    (`RoomCentre`, boxes weighted by area), a line from it to the source's middle; the candidate
    edges are those the line points to by more than 2 % of its length (left if dx < 0, and so
    on); of those, the one with the least travel to be all off the screen (the same travel the
    plain Slide uses); a line under 1 % of the screen's diagonal, or no middle, is the plain
    nearest edge. In the scene Morph (`find_room_centres`, every frame from the plan's parts) the
    middle is the paired parts' boxes in the new scene for a leaving item and in the old scene for
    a coming one, leaving out pairs that neither move nor resize (1 px) and backgrounds (90 %).
    Without *Fade while moving* the moving items are drawn whole straight into the target after
    the run's backgrounds are cross-faded (`Only::Fills` / `Only::Movers` in `draw_side_items`).
  - **As an item's change** (a Morph's *Alone* item with Make Room, and every Make Room item): the
    tick, as it sets an entry *Alone* in a scene on screen and not in a group, starts a job with
    one side (`Job::solo`, `start_solo`) with the item Morph's layer, so the item crosses the
    screen; its own transition is then `Paired` and draws nothing. Every edge of a tick is worked
    out before any job starts (`room_centre`): for an item going, the destinations of the jobs in
    its scene, the items of the other kind that started near it, and any item of the scene whose
    own show transition is running (`item_transition_running`, any kind: a Fade and Zoom shown
    beside it counts); for one coming, the reverse. A solo job ends when its clock runs out, its
    item's visibility flips back, it leaves the scene or the scene is removed; like a pair's job
    it keeps its scene weakly and holds its one item only while the change lasts (beta170's rule). Out of view or in a group there is no
    layer: `start_alone` takes Make Room for a Slide inside the item's frame (fading with
    `room_fade`). The log has one line per item: `Morph: 'X' makes room in 'S', by the right
    edge (away from the change under way)`.
  - Tested in `[make room: ...]` (`make_room_test`, alone: `STAGEKIT_TEST_ONLY_MAKE_ROOM`), the
    frames in `docs/images/make-room.png`. Not seen on Windows; the item version waits the item
    Morph's 0.1 s before it moves, like Morph's own *Alone*.
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
  *Mosaic edge* (`pixel_edge`, `MosaicEdge`, by number, append-only: 0 *Cut at the edge*, as it
  comes, 1 *Whole cells*; `BlurLook::whole_cells`, the `whole` uniform; a control of the shared
  look, so the Blur filter and Scene Blur both have it, Scene Blur only with *Each source by
  itself*). The owner: "I'd like if there was a configuration to have the shadows or other applied
  strokes, glows or filters in general take on the visible pixel shape, so when a screen is
  blurred it can take on the pixel shape for example". With whole cells a tap whose cell the
  picture's edge cuts gives nothing (`cell_whole` in `mosaic_tap`), so the outline is whole cells
  and the alpha follows it; with smoothing it fades into the cells left out. The picture cannot
  grow, so cells are only left out, never drawn past the edge. Each shape is judged by its own
  corners (`inside_by` on the cell's corners turned back to the picture, half a pixel of slack),
  a circle by its radius (the gap cell between three circles by 0.29 of a cell), a Voronoi cell by
  its 24 neighbours (`voronoi_whole`: whether the cell reaches the line of any picture edge,
  worked out as a stretch of that line, only for cells within a square's diagonal of an edge).
  Anything after the Blur filter that reads alpha (Shadow, Glow & Outline, Corners & Edges)
  follows the cells by itself; a test checks that an outline after it is jagged. The nine taps are
  a loop since then (`PSMosaic`): written out nine times, the Voronoi test made a very long shader,
  and Direct3D compiles every technique of the file when OBS loads the plugin. Not run on
  Direct3D. **Open sides** (`BlurLook::open_edges`, the `open_edge` uniform, beta167): a side of
  the picture that is no edge, because the screen cuts the source there anyway; `inside_by` and
  `voronoi_whole` never reach it. Only Scene Blur's *Each source by itself* knows where a source
  is on screen and sets them (`blurred_item`: a side whose two corners, through the item's draw
  transform, are both at or past one screen edge, half a pixel of slack). The owner, of beta165:
  "Whole cells blur also blurs the entire scene border" (the full-screen Ambient Background lost a
  row of cells all round, a black notched border). The Blur filter on a source cannot know it
  (one source, many scenes and places) and still cuts there: the README says to use Scene Blur.
  **Every uniform of the mosaic is set at every run, by every user, through one helper**
  (`MosaicUniforms` / `MosaicLook` in `src/common/mosaic-uniforms.hpp`, used by `BlurPipeline`
  and Ambient Background, after beta170). OBS shares one copy of an effect file between all who
  load it, empties an effect's values at the end of each technique and passes over a uniform with no
  value when it hands values to the shader, so a uniform a user leaves out keeps whatever the
  shader was last given by anyone; and **Direct3D draws nothing while a uniform has never been
  given a value** ("Not all shader parameters were set"; OpenGL takes zeros, so the tests never
  saw it). Ambient Background set none of `whole`, `open_edge` and `fallback` (added for Scene
  Blur in beta165 and beta167): on Windows its mosaic drew nothing, a black background, until a
  Blur or Scene Blur had run once in the session, and kept working after. The owner: "ambient
  background does not show unless i turn on scene blur. it remains visible after turning blur
  off", then "turning off mosaic brought it back". With a black background Scene Shadow's
  shadows did not show either; nothing was wrong with it. A new uniform of the mosaic goes in
  `MosaicLook` and `MosaicUniforms::set`, never in one caller; a new uniform of any shared
  effect file must be set by every user of every technique that reads it. Tested in
  `[Ambient Background's mosaic sets every uniform of the shared effect]`
  (`STAGEKIT_TEST_ONLY_BLUR_OFF_DRAW`): the shader is given other values first, as another user
  would, and the background must look the same (only the hexagon case tells the two apart).
  **Kept to a mask** (`fallback` uniform, `run`'s `kept_to_mask`): Scene Blur's *The scene as
  drawn* hands the mosaic a picture whose alpha is the mask, so its taps (weighted by alpha)
  take in only covered pixels; a cell whose nine taps near its centre all miss looks on a ring at
  0.42 of a cell, and failing that takes the pixel's own colour, never one from outside.
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
  - **Game area only** (`game_area`, off as it comes; beta193). The owner, of emulators
    (Ryujinx, eden, nxemu, Dolphin) in a Window Capture with *Client area* ticked: "I don't want
    it to grab the window toolbars and such of the game window", "i'm not manually cropping
    anything". The emulator's own menu, tool and status bars are inside the client area. **How
    it is found** (`find_game_area` in `auto-capture.cpp`, on the window watch's thread, only
    for handles asked for in the last 5 s with `game_area_want`, which the switcher does for
    every window of its two lists at each look): the largest visible child window
    (`EnumChildWindows`, all depths) of at least two fifths of the client area that leaves
    some of it out (one that fills it is a frame), skipping controls (`control_class`: the
    common controls' classes, and any class with statusbar, toolbar, menubar, rebar, tooltip,
    richedit or scrollbar in it, and Chromium's page window, `Chrome_RenderWidgetHostHWND` and
    `Intermediate D3D Window`, so a browser or an Electron app keeps its tabs and bars). Measured with `GetWindowRect` mapped into the client with
    `MapWindowPoints`, both in OBS's own DPI terms. It is published in the snapshot as
    `OpenWindow::game_area` (`GameArea`, client pixels with the client size; `looked` says it
    was looked for). Qt render widgets (eden and yuzu forks, Dolphin), Avalonia's native host
    (Ryujinx) are expected to be such children; **none of it has run on Windows or a real
    emulator**: the tests give stand-in windows an area (`set_test_windows`' fifth field,
    `x,y,w,h,cw,ch` or `none`). A single-surface app gives none and is shown whole (the log
    says `no game area found in <program>: shown whole`; found: `game area of <program>:
    child <class>, WxH at x,y in a WxH client`, once per program). Looking at the image
    instead was left out on purpose. **How it is applied**: the switcher draws its live
    picture through copies already; `copy_live` draws only the cut part (`LiveBox`, from
    `live_box`) and the cut size is the filter's size, so Standby, Auto Frame and Fit to Space
    see the game's shape. Nothing is written to the item or the capture. The cut is kept as
    shares of the client (`Cut`), and used only when the picture has the client area's shape;
    a picture of another shape keeps the cut for up to a second (`kMismatchNs`,
    `mismatch_since_`: a window resized or maximised has its new image before the window watch
    has its new client size), and only after that is shown whole (*Client area* unticked),
    logged once per window switched to. A switch takes the new window's
    cut at once (`cut_at_once_locked` in `aim`; the change between the two pictures glides the
    size); within one window a new cut is taken once it has stayed 0.3 s (`kAreaSteadyNs`) and
    is more than 3 px off (`near_cut`), and glided to over the switcher's duration and easing
    (`cut_t_`); that glide is reported to Fit to Space through `app_capture_change` like a
    change of window (`change()`, its id `changes_ | cut_seq_ << 40`, so Ambient Background,
    which plays its change at each `changes_`, plays none). Unticking glides back to the whole
    picture the same way. While the cut glides, `copy_live` makes its copy's texture again each
    frame (its size changes): a fraction of a second, only when the area changes, accepted.
    Tested in `[window switcher: game area only]` (`game_area_test`, alone with
    `STAGEKIT_TEST_ONLY_GAME_AREA`; a stand-in kind `stagekit_test_window` drawing red menus
    round a green game, by the program it is aimed at).
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
    **Clearing goes through both lists** (`clear_picked_in`: `apps` and `by_hand`, all four
    paths; beta188), and a picked automatic entry being shown is let go of as a closed window
    is (`drop_shown`: no last frame kept, the next automatic window or the Standby Image).
    `is_pinned` needs a program's file name (`.exe`) before the dash: Firefox's titles have the
    same " — " in them ("Page — Profile — Mozilla Firefox"), and up to beta186 such a title
    chosen from a list was taken for a picked window (it answered nothing, so an ignored
    Firefox title ignored nothing, and Clear would have removed it).
  - **Nothing unlisted is drawn, and the capture is never left aimed where OBS could wander**
    (beta188). The owner, 2026-11-18: "window capture is capturing a window i've not manually
    selected and i can't clear it": their lists held two closed apps and an ignored Firefox
    title, and the Window Capture, last aimed by executable at firefox.exe (a window of another
    profile had been picked), took the Work profile's window by itself at the next start; the
    switcher drew it. Now, whenever nothing is shown (`current` empty after a look), the
    capture's own picture and bridge are not believed (`unsafe_`, `own_ok_` false, Standby
    cover); and after every look `settle_aim` aims the capture at no window
    (`aim_capture_at_nothing`, Auto Capture's `kNoWindow`) when its own aim is by program at a
    program not listed by name, by title at a title no entry answers (a picked entry: its
    program), or at an ignored window. A window of a listed program is aimed as before. The
    status line says *Capturing nothing* (`AppCapture.Status.Nothing`) and goes by what the
    filter shows, not where the capture is aimed. With both lists empty the filter still does
    nothing at all. Tested in `[window switcher: picked windows cleared from both lists, nothing
    unlisted drawn]` (`clear_picked_test`, alone: `STAGEKIT_TEST_ONLY_CLEAR_PICKED`), the
    owner's chain at load and within a session. A capture parked at no window while a ready
    capture is drawn reads `stagekit-no-window.exe` as its program (Auto Frame goes by that).
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
  of 20 s that grows by half each time to a minute at most, for the full time each try (until
  beta191; now looks, below); a picture
  or the program closing ends it. (Beta135 and 136 tried for half the time, 5 s, with waits up to
  5 minutes: the owner's Ryujinx, whose title never changes and whose first picture took 7 s, was
  given up on three times running and they reported "auto capture is not capturing a game". Its
  log also showed OBS hooking it the moment a game drew: an emulator's menu gives no picture.) The
  status says *Aimed at ..., which shows no image yet* (`waiting_for_picture`) instead of *Now
  capturing* over an empty capture. **A rest is a rest** (2026-11-18, beta184; the owner's logs:
  `attempting to hook process: Dolphin.exe` / `capture stopped` about every 0.42 s, 78 times in
  90 s, and every rest of Dolphin ended 0.27 s after it began, "its window's title changed"). Two
  faults: the capture stayed aimed at the resting program whenever nothing else was chosen (with
  *any full-screen application* as the fallback it would hook a full-screen emulator too), and any
  window of the program with another title ended the rest at once (Dolphin's frame rate, a GBA
  window's "Volume 100%" / "Muted", the GBA windows' titles against the main one's). Now, with
  nothing else to capture while a program rests, a Game Capture is aimed at no window
  (`kNoWindow`, window mode, key `kRestKey`; logged once a rest), and aimed at the program again
  when the rest ends. In beta184 a rest kept the titles of all the program's windows (`titles`, by
  `title_shape`: lower case, digits out) and every title seen in its first `give_up_s_`; only a
  title not among them after that ended it, 3 times while the program stayed open
  (`title_tries`; replaced in beta191, below). Clicking back in counts only when no
  window of the program was in front (`rest_in_front`; in beta184 also not in that first
  stretch). Tested in
  `[auto capture: a program rests for real ...]` (`auto_rest_test`, alone with
  `STAGEKIT_TEST_ONLY_AUTO_REST`; the Ryujinx case, a picture at 0.7 of the patience on the first
  try, and a game started during a rest picked up by a look). `set_test_windows` looks at once, and each look moves the filter's clock on 0.25 s: a test
  that sets windows often passes `keep_clock`. A program put on *Never capture* while it rests
  is no longer rested (`rest_.erase`), so the full-screen fallback comes back. The Windows side
  (whether OBS's Game Capture really logs nothing for a window that is not there) is believed
  from OBS's source, not seen. **A rest looks in on the program** (beta191; the owner, the same
  day: "game capture is taking quite a while to hook windowed games and emulators, but i don't
  want the overhead processing of OBS to go up"). Beta184's rests were 20 to 60 s of nothing, and
  a game started in an emulator's menu waited for the end of one. Now a rest aims the capture at
  the program for `kProbeFor` (1 s, two or three of OBS's hook tries at the 0.42 s the owner's log
  showed) every `kProbeEvery` (5 s from the end of the last look; `probe_at`, `gap`; after
  `kQuickProbes` (12) empty looks the gap grows by half each time to `kProbeEveryMost`, 15 s), then
  back to no window in the same tick. A picture during a look ends the rest and the capture stays
  (logged `has a picture ... looked in on while it rested`); looks are not logged, and *aimed at no
  window* is logged once a rest (`rest_said_`). A look is not a game for *No game: Standby Image*
  (`probing`), or the image would come and go every few seconds. A title change or a click back
  in ends a rest at once, at any moment of it (beta184's quiet first `give_up_s_` is gone), and that
  try gets the whole patience, so the Ryujinx case (a picture 7 s after aiming) still holds on a
  first try; a look's 1 s does not. A title counts only once (`Tried`, per program while it is
  open, a picture or not): every `title_shape` the program showed while the capture was aimed at
  it, so Dolphin's frame rate and its GBA window's *Volume* / *Muted* end a rest once at most, and
  `kTitleTries` (8) rests ended by a title while it stays open is the backstop. Measured in
  `auto_rest_test`: aimed at the resting program 9.6 s in a minute (beta183: all of it; beta184:
  none), a game started 1.7 s into a rest picked up after 3.7 s, just after a look after 5.4 s.
  How many hook tries OBS really makes in a 1 s look, at the owner's hook rate, is not seen.
  **Do not write to the user's lists from the tick again.** What follows is why the giving up exists at all: aimed at a
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
- **Auto Capture lets go when the game's window closes and its program stays open**
  (`watch_captured`, `captured_`, `spent_` in `src/capture/auto-capture.cpp`, beta190). The
  owner (2026-11-18): "when using game capture with some emulators like dolphin where the dolphin
  menu and game screen are separate windows, if the captured game closes but the emulator stays
  open in the menu/game list, the last captured frame of the game remains in the OBS capture, and
  it doesn't morph into the standby as intended". OBS's Game Capture hooks the program, not the
  window, so the hook keeps the last frame and the capture its size, and a Standby Picture (which
  goes by the capture's own size) never sees nothing. So the game's windows are remembered by
  their handles (`OpenWindow`'s `Window::handle`, from the window watch's copy): the program's
  windows that were not open when the capture was first aimed at it (`before_`, the menu was;
  Dolphin's GBA windows count as the game's), and only when there are none (the game was open
  already), the window in front or the one aimed at by title, `kPickAfter` (0.5 s) after the
  picture came, since the list can lag. Picking by focus alone watched the menu whenever the
  capture had been aimed at the menu and OBS was in front. When every one has been out of the
  list for `kLetGoAfter` (0.5 s, two looks) while the program is open, the capture is aimed at no
  window (`kNoWindow`, as for a rest; logged as `aimed at no window while <program> shows no game
  window`), the log says `the window it captured in <program> has closed; letting go`, and the
  program's windows seen while the game was open (`seen`: the menu) are no game (`is_spent`)
  until it closes; a window opened later is chosen as usual. One of several closing (a GBA
  window) is not the game closing. Any window not seen before that appears within the wait is
  followed (one of the same title shape first: a switch to full screen makes the window again; a
  game started at once makes a new one), nothing re-aimed, and while the old one may be coming
  back the capture stays aimed where it was (`current` ignores `is_spent` while `gone_since` is
  set). A minimised window is left out of the window watch's list but is still there (`IsWindow`
  and `IsWindowVisible` on Windows, in the tick: no list of windows is made there); a spent
  program minimised whole stays spent while `IsWindow` finds one of its windows. Switched off
  (a capture hooked by hand) the filter forgets what it watched. Always capture programs go the
  same way. A window with no handle (the older tests' stand-ins) is never watched. Tested in
  `[auto capture: the game's window closes, its program stays open]`
  (`auto_window_closed_test`, alone: `STAGEKIT_TEST_ONLY_AUTO_WINCLOSE`): let go of in 0.6 to
  0.8 s, the Standby Picture's fade part-way after it and whole later, the menu not aimed at, a
  new game window aimed at, a window made again (at once, or after a quarter second) not let go
  of, the menu aimed at first and the game watched all the same, a GBA window closing not let
  go of, a game under another title opened within the wait followed. The stand-in windows have a sixth field, an id for the handle. Whether OBS's Game Capture
  really drops the hook's last frame when aimed at no window is not seen on Windows.
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
  code. Since beta154 `make-option-icons.py` adds scene pictures labelled *live*, *chat live* and
  *standby* (group `scenes`) and show-key pictures for the holding modes and columns, *starting*,
  *brb*, *intermission*, *ending*, *column chat*, *column gba* (group `show hide`, found by
  `sourcePictureFor`; `GROUPED` gives each its group and label). The owner: "the standby scene,
  live scene and Chat Live scenes should have icons that are representative of their function as
  should each of the show in scene buttons. of course allow manual icon changes still". No
  matching changed: of several labels that fit, the one with the most words wins (so *Chat Live*
  is *chat live*, not *live*), and a picture chosen by hand is still looked at first. Labels match
  whole words: a source must be called *BRB*, not *Be Right Back*, for the brb picture.
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
- **The capture hide shows only what it hid** (`hid_items_press` in `src/common/hid-items.*`;
  Auto Capture and the Automatic Window Switcher, beta183). Without a Standby Picture to cover
  with, the hide hides the capture's shown items in every scene and group and writes down which
  (scene, containing scene or group, item id; `hid_items` in the filter's own settings, in
  `kOwnSettings`, never an item held); the show shows only those still hidden, passing over what
  `mode_holds_hidden` holds, and clears the record; with no record and nothing shown it does
  nothing. Before, the show showed the capture everywhere: the owner's DSLR, with a leftover Auto
  Capture, came up full-screen over the capture a Link Visibility had on. A pending record reads
  hidden to the Stream Deck key. Not an option: a show of an item the command never hid is the
  bug. Tested in `[capture hide: ...]` (`STAGEKIT_TEST_ONLY_CAPTURE_HIDE`).
- **A Standby Image covers any source on purpose, and keys can ask for it** (beta189). The owner:
  "The Hide shows standby image I think should have its own filter, or be separate from that
  automatic window switcher somehow. Automatic window switcher doesn't even apply to capture sources
  other than window". So: the vendor requests `StandbyCover` (`how` on, off, toggle; none asks) and
  `GetStandbyCover` (`standby_cover_press` in `standby.cpp`: the filter named, or the first enabled
  one; a press refused while it is switched off; its own transition, `style` -1), and the event
  `StandbyCoverChanged` from every change of `cover_` (`set_cover` / `toggle_cover`, so the hotkey,
  the button, the switchers' and Show in Scenes' hides all tell; `vendor_emit_standby_cover_changed`;
  the `covering` proc answers for it). Show in Scenes' *Hide shows Standby Image* (`hide_standby`,
  off as it comes): a hide covers each listed source with an enabled Standby Image instead
  (`standby_present`), its items left shown; a show uncovers them and whatever this filter covered
  before (`covered_`), whichever way that hide was made; *Follow what is shown* counts a covered
  source as hidden (`standby_covered`). The owner then preferred "ONE Show in Scenes filter where
  each Stream Deck key chooses": `ShowInScenesPress` (`show_in_scenes_press`, `hideAs` filter, hide,
  cover) switches the filter on the request's thread with the way noted for that thread
  (`press_as`, `pressing_thread_`, `pressing_as_`, read in `enabled_cb` into `press_`'s
  `kPressHide` / `kPressCover`); a key on *As the filter says* still uses
  `SetSourceFilterEnabled`. The cover is the source's: every scene and every Cropped View of it
  shows the image (to cover one view, the filter goes on the view). Tested in `[standby image over
  any source: ...]` (`standby_cover_test`, alone: `STAGEKIT_TEST_ONLY_STANDBY_COVER`, also after
  the switch-command section) with a colour source as the camera. Stream Deck: the Standby Image
  key's *On press* gains `cover`, `coveron`, `coveroff` (append-only, `coverHow`), lit while
  covered; the Show in Scenes key's *When hiding* (`hideAs`, `hideAsOf`). Not run on Windows or a
  Stream Deck.
- **Show in Scenes' *Show only what it hid*** (`restore_hidden`, off as it comes; beta189). The
  owner's camera switch lists DSLR Component, DSLR GBA Link, Square Camera and DSLR (full-screen,
  hidden in the component scene); with *Follow what is shown* the switch read off, a press showed
  DSLR, and the captures' Only one link hid Game Capture and DSLR Component. Ticked: a hide press
  notes each item it actually hid as (scene name, name of the scene or group holding it, item id;
  `Noted`, never a reference) and each source it covered (`Record::covers`, only one not covered
  before), added to what earlier hides noted and is still held, written to the filter's own
  settings (`hidden_record`, in `kOwnSettings`; lines `i\tscene\towner\tid`, `c\tsource`). A show
  press (`restore_record`) brings back only those still hidden and not held hidden by a mode, then
  forgets all; with nothing noted it does nothing and logs it. **The rule**: the switch means "has
  hidden things": `follow()` with it ticked puts the switch on (with *When this is on* shown) while
  nothing noted is still held (`prune_record`, which forgets an item something else has shown or
  that is gone, and a cover taken away), so a toggle key always hides and then restores. Works with
  *Hide shows Standby Image* and `ShowInScenesPress`. Tested in `[show in scenes: show only what it
  hid]` (`restore_hidden_test`, alone: `STAGEKIT_TEST_ONLY_RESTORE_HIDDEN`; the owner's scene with
  both links, the press and its restore, a show with nothing noted, an item shown and hidden by
  hand meanwhile, a filter made again from its settings, a key set to Cover).
- **Every key's list names its filter** (beta189; the owner: "The stream deck keys don't really
  say what filters they are associated with which makes it difficult for me to find the associated
  filters"). `targetItems` labels every entry "<source> — <filter>" (`filterLabel`: the kind in
  brackets where the name is not the kind's own, `isOwnName`, which knows the old names too); up to beta175 a source with one such filter was listed
  by its name alone, so the owner's *Square Camera* hid its *Show in Scenes-Camera*. The Scene
  Mode key's *Scene and filter* entries (`modeFilterItems`, beta181's filter list; the mode is
  chosen under it) are "<scene> — <filter>" always. Values are unchanged, so saved keys
  keep working. The list is called *Source and filter* / *Scene and filter* (*Capture and filter*,
  *Switcher filter*), and `target_list` in `make-plugin-files.py` gives it a tooltip and a line
  under it naming the kind as OBS lists it ("Works the filter named after the dash, a “Source Filter:
  Show in Scenes (StageKit)”", by `obs_name`: Scene Filter or Source Filter as beta182 names them). A new key that picks a filter uses `target_list`.
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
- **Ambient Background's Darkest level** (`darkest_level`, 0 to 100 %, 0 as it comes: off, and
  no pass is drawn; `darkest_fill`, by number, append-only: 0 *The background image*, 1 *A
  colour*; `darkest_color`, a mid grey, shown only for a colour; beta192). The owner: "is there
  a way I can prevent the background from ever becoming completely black, even when the ambient
  background is capturing a live feed that has completely black content?", then "add darkest
  level as a configurable option". The last pass (`Lift` in `stagekit-looks.effect`, full size,
  raw values like `Final`, before the hand-off that follows `gs_get_linear_srgb()`): toward the
  image a part becomes the fill as far as it is dark (`1 - smoothstep(0, 1.5 x level, luma)`);
  then, with either fill, the floor `luma + level x (1 - smoothstep(0, 1.5 x level, luma))`,
  which never falls under the level, is made up by adding the fill's colour scaled to unit
  luminance (grey for what a saturated colour cannot give). From 1.5 x the level up nothing
  changes. The image fill is the picture before `Final` itself while the scene holds the image
  alone (no capture, no change under way: no cost), else the image drawn alone into `tr_fill_`
  and blurred by a `KawaseBlur` of its own (`fill_blur_`) at *Blur* (not for Temporal), with no
  mosaic or bloom; `Lift` gives it `Final`'s brightness and vignette. No image under *Background
  from*: the colour. A black capture counts as no capture after two seconds
  (`live_shows_something`), so this is for a capture that is dark but not empty. Every uniform
  of `Lift` (`lift_*`, `image`, `image2`) is set at each run; the test gives the shader other
  values first for each fill, a new colour each time (a colour left in the shader by the first
  would hide an unset `lift_color` in the second). Tested in `[ambient background:
  darkest level]` (`ambient_darkest_test`, alone `STAGEKIT_TEST_ONLY_AMBIENT_DARKEST`; sheet
  `docs/images/ambient-darkest.png` from `$out/darkest`). Not run on Direct3D.
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
  **Every key's picture is the pack's framed screen with a drawing in it** (the owner: "all keys
  for the plugin must be of consistent design language and styling"): since beta150 the arrange
  tools, slides, camera places, hook keys and plain keys are drawn there too (`copy-all`,
  `slide-*`, `camera-*`, `hook-*`, `plain-*`), not taken from the pack's older `tools`,
  `camera`, `capture` and `plain` groups, which stay listed only so a picture a key was given by
  hand still loads.
  Each key offered also has its own small icon in Stream Deck's list of actions
  (`tools/make-action-icons.py`, `ICONS`, white lines, 20 and 40 px, into
  `imgs/actions/<key>/icon`); the owner, of a list where all keys showed one of two drawings:
  "I'd like all of the stream deck buttons to get specific icons". A new key gets a drawing there;
  `tests/features.test.ts` fails if two keys share one.
- **Every key chooses its image from type lists, not an Image list** (beta156,
  `streamdeck/src/key-types.ts` and `key-types.json`). The owner: "instead of having an image list
  on all the streamdeck buttons, have a similar source type, scene type, etc style list relative to
  the button function"; then "I don't want any of the buttons, or the options specific to my
  stream scene collection, but I do want the specific icons I had"; and "Dolphin isn't handheld ...
  make as many scene and source types as possible and provide the proper respective icons". So:
  - **Any combination** (beta164; the owner: "I need all my original Scene type icons to exist in
    the types lists ... I want to be able to set any combination"). A Source type list offers its
    key's types first, then every other source type (`More: <group>`) and every scene type as
    `scene:<id>` (`Scenes: <group>`); a Scene type list offers every source type as
    `source:<id>` (`Sources: <group>`) after the scene types. `sourceTypePicture` and
    `sceneTypePicture` follow the prefix, so every resolver gets it; `distinct` drops a picture
    already offered higher in the list and marks a repeated name (*Console (scene)*). The
    Corners & Edges key, which had a look list only, has a Source type list too. The `only`
    lists are now what comes first, not all there is. Tested: every key offers every scene
    picture and every source type.
  - **Generic types, the owner's art.** Every type is a general one (Game, Retro game, Capture
    card, Facecam, Gameplay + chat, Holding screen ...; no product names either: "Stream
    controller", not the brand); a test fails on an owner's scene, source,
    app or device name in any list (`owners` in `tests/key-types.test.ts`). Each type's picture is
    the owner's pack art for that type *and the key's function* where there is one (`art` in the
    JSON, by family: `show` the *show hide* pictures, `shadow` *shadow by source*, `blur` the
    *blur <style>* pictures in the filter's style, `mode` the *scenes* layouts), otherwise the
    type's own picture (`base`: a drawing of `make-option-icons.py`'s `TYPES`/`MORE_TYPES`, a
    `devices` picture; never the pack's `filters/app-*`, see *Layout*). Scene types are the owner's *scenes*
    pictures under generic names (*dolphin gba* is **Console + handheld link**, and its source
    pictures *gba link 1/2* are *Linked handheld screen* and *Second linked handheld screen*: the
    owner, "Retro icon should be specific to GC+GBA"; *Retro gaming*, *Classic gaming* and *Retro
    game* have drawings of their own; *hd60s* is *Capture card*, the
    beta154 Live / Chat Live / Standby pictures *On air*, *On air + chat*, *Holding screen*) and
    `options/scene-type-<id>` drawings (`SCENE_TYPES` in `make-option-icons.py`) for the rest.
    Four families of drawings were added at the owner's word ("create as many device, status,
    style, activity icons as possible for every key that it's relevant to"): devices
    (`MORE_TYPES`, as source types in *Devices & audio* and *Cameras*), status (source types in
    *Status & holding screens*, reused as scene types), styles (`STYLES`, `options/style-*`: blur
    and mosaic looks, edges, shadow looks, transitions) and activities (`SCENE_TYPES`, also source
    types in *Activities*, offered only by the Scene Mode key). Show in Scenes offers every source
    type but the activities; each other key only what fits its job (`SOURCES`, `LOOK_KEYS`).
    A Blur style the pack has no blurred pictures of draws a source in the nearest one
    (`packBlurStyle`). The pack's *standby by source* pictures are not offered: without the words they carried all
    nine are the same mountain. Within one list no value, name, picture id or picture file repeats
    (tested).
  - **Settings.** `sourceType` (Source type, or *Mode type* on the Scene Mode key; ids append-only,
    the Scene Modes filter's *Mode type* in OBS offers every source type of the Scene Mode key's
    list, the same ids in the same order since beta182: `tests/mode-types.test.ts` reads
    `kTypes` in `scene-modes.cpp` and fails when the two differ), `sceneType` (Scene type), `look` (Blur style, Corner style, Shadow style,
    Transition, Position, Capture type). Each list starts with *Automatic*, value `auto`; unset or empty reads the same. Since beta160
    every key a user can add has one (tested from the manifest): the keys whose own list is what
    they do (Arrange tool, Dock Option, Slide to Edge, Set Aspect Ratio, Manual Window / Source:
    `OWN_LISTS`) have a Source type too, Capture Active Window and Move Source / Move Scene Item a
    Source type beside their look, Blur All a Scene type beside its Blur style (the owner: "no
    types selection for manual window/source? all keys must have the options"). `make-plugin-files.py` writes the lists into
    the pages from `typeLists()` (it runs node on `key-types.ts`), as static `<optgroup>`s with
    `placeholder` and the tip as the item's `title`.
  - **Resolution order:** a type chosen in a list; else the image kept from the old Image list;
    else Automatic as before (a Show in Scenes key: the pack's picture of the source by name, then
    the type its name speaks of, `typeOfName`, then its OBS input kind, both drawn as show-key art).
    Each entry of every list is checked to be what the key itself shows for it.
  - **Old keys keep their look.** A key's `picture` (the old Image list) is put aside once
    (`settle` in `actions/shared.ts`, `adoptOld`) as `oldImage` with `oldImageWhile`, the type
    settings at that moment; `keptPicture` shows it while they are unchanged. Where a type of the
    key's lists shows that very picture (`typeForKept`, tried with the key's own resolver), the
    type is set instead so the page says what it is. Choosing anything in a list, Automatic
    included, drops it. Hook and Manual Window / Source keys' old `sourceType` (beta155) is the
    value of their new Source type list (since beta160 drawn as the type with a badge, no longer as
    `hook-type-*`).
  - **A type and a function on one image** (beta160; `*Shown` in `src/pictures.ts`,
    `src/compose.ts`, `tools/make-badges.py`). With a type chosen on a key that also does
    something of its own (step, tool, option, slide, ratio, capture kind, blur style, destination),
    the image is the type's picture with that function as a badge, 54 x 36 px, top right
    (`BADGE_AT`); a Move Source / Move Scene Item key has a second badge top left for its filter's
    `linked` (flipped or not) or `apply_in` (this scene, a chosen scene), which `seen` keeps. The
    on/off mark stays top left on two-state keys: the badge goes over the marked file
    (`layersOf`). Badges are drawn beforehand in every palette (`imgs/pack/<palette>/badges/`,
    listed in `src/badges.json`, SVGs in `svg/badges/`), from make-option-icons.py's drawings or
    their own (`OWN`); the combinations (types x functions x palettes x on/off) are too many to
    draw, so the plugin lays them together when a key is shown: plain Node (`node:zlib`) PNG
    reading and writing, a `data:image/png;base64` URL to `setImage`, cached per combination. A
    PNG, not an SVG with the PNG inside: Stream Deck takes PNG data URLs as it takes files, and
    whether its SVG renderer draws an embedded raster was not something to find out on the
    owner's deck. **Automatic is unchanged**: no type, no badge, the same file path as before
    (tested for every function and setting). A new key with a function gets a `*Shown` function,
    a badge per function and a Source type in `SOURCES`; run `make-badges.py` after
    `make-option-icons.py`. Not seen on a Stream Deck.
  - **Layout** (beta166; `layout`, `layoutList` and `LAYOUTS` in `src/key-types.ts`, `laidOut` in
    `src/pictures.ts`, `src/layouts.json`). The owner: "every icon should have a standalone version,
    live/no chat version, and a component/chat version", then "perhaps have a selector to choose the
    layout and a second selector for the specific type". Every key with a Source type or Scene type
    list has a *Layout* list just before it (`make-plugin-files.py` puts it there): `auto`
    *Automatic*, `standalone`, `live` *Live (no chat)*, `chat` *Live + chat* (append-only). It is
    **not** one of `typeLists` (so `typeForKept` never sets it) and not in `snapshot` (choosing a
    layout keeps an image from the old Image list, laid out). Automatic is unchanged: `laidOut`
    returns the very `Shown` it was given (tested). The layouts are the pack's game capture and
    game component scenes (02/03): `make-option-icons.py`'s `layout_pictures` takes the gamepad out
    of them (`GLYPH`, the frames `layouts/live-frame`, `layouts/chat-frame`) and draws each type's
    drawing in the panel (`PANEL`, `LAYOUT_SIZE` 1.1) as `layouts/<live|chat>-<drawing>`, for every
    drawing a source type's `base` or a scene type's `picture` is, and those of `GLYPH_OF` (the
    pack's scene pictures by the drawing of what they show): 100 drawings, 202 pictures a palette
    with the frames, status copies by `make-state-icons.py`, SVGs in `svg/keys/<palette>/layouts/`.
    **One template per layout, for every type** (beta166's second pass; the owner, of the first
    contact sheet: "they should all be consistent. for the live the square camera image should be
    in the same position for all, but the window icon appears to have it moved to the bottom.
    also, App 3D, or any app type should not have a specific letter or anything. it should only be
    images relevant to the source, capture, type etc. also, all the standalone icons appear to
    have the images offset to the left. make them all centered. fix the icons universally"). So:
    game, window and capture card are drawn in the template too (the pack's 05 to 08 put the
    camera elsewhere or show a bar; the pack's scenes stay as scene types); the three app types
    that were the pack's lettered `filters/app-blender`, `app-zbrush`, `app-fusion-360` (and
    their show, shadow and blur art) are drawings now (`type-app-3d` a wireframe cube,
    `type-sculpting` a bust and a loop tool, `type-cad` a set square and a ruler; no type picture
    is a program's logo or letter, tested; an old key's hand-chosen app picture still loads); and
    **every drawing is centred** (`centred` in `make-option-icons.py`, by the ink's bounds,
    `bounds`, applied to every entry of `DRAWINGS` and to the badges' `OWN`): `SCREEN` is now a
    box round the key frame's centre (`FRAME` 14..129 x 17..81, centre 71.5, 49), `ERASE` what is
    cleared of the pack's own glyph. Up to beta164 every key drawing sat 7.5 px left of the
    frame's centre. The pack's small camera in the screen's bottom right corner is the pack's own
    (its app pictures have it) and stays. Tested: every options picture, layout picture and badge
    has its ink's middle within 2 px of its frame's, panel's or badge's middle.
    `laidOut` goes by the picture the key would show: a Source type's function art (eye, shadow,
    blur, mode) is taken back to the type's `base` (`typeBehind`), then `byPicture`, then
    `glyphOf`; a picture with neither (the standby image's, a key on Automatic, a kept image) is laid,
    smaller, into the frame's panel when the key is shown (`inset`; `laidIn`/`shrink` in
    `src/compose.ts`, the key's whole picture, `crop`), and the on/off mark is put on that whole
    (`imgs/marks/on|off.png`, the off look as `make-state-icons.py` makes it). *Standalone* changes
    only the six pack scenes that are layouts themselves (`standalone`): their drawing alone.
    Every action goes through `imageOf`/`showOnOff` with the key's settings (not its palette
    alone), so there is no per-action code. Badges and marks are laid as before. Not seen on a
    Stream Deck.
  - **Stream Deck's own image menu**: titles are documented (setTitle "can only be set by the
    plugin when the user has not specified a custom title"); for images the SDK's types say only
    that state images "can be overridden by the user", nothing of setImage against a user's image.
    It is believed, not proven, that a user's image wins. Not tried on a Stream Deck.
- **The Scene Mode key** (`src/actions/mode.ts`, UUID `.mode`, beta155; tied to one filter since
  beta178) is for the Scene Modes filter through its vendor requests: `ListSceneModes`,
  `GetSceneMode` and `SetSceneMode` with `how` `on`, `toggle`, `next` or `none`; the
  `SceneModeChanged` event (`obs.on("mode")`) makes every Scene Mode key look again. The owner,
  of beta174: "the scene mode button when set to toggle needs to actually have icons that change
  to the respective mode icon. Also how does toggle work with more than two scene modes?", then
  "allow for the mode key to have a layout/mode type selection for each associated mode. Also,
  the key should connect to the selected scene mode filter, not individual modes in all filters,
  and it should allow configuration of the specific modes as desired, whether it's on, off,
  toggle, or next". So (`src/modes.ts`, all pure):
  - **Settings**: `modeFilter` (`scene␟filter`, the *Scene Modes* list, `getModeFilters`),
    `press` (`on` as it comes, `off`, `toggle`, `next`), `mode` (a mode id, for on and toggle;
    the *Switch to mode* list (*Mode* up to beta179), `getFilterModes`, refreshed by the page's script when `modeFilter` changes),
    `longPress` (`none` as it comes, or the same four), `longMode` (unset: `mode`), `holdMs`
    (`HoldPress` in `src/hold.ts` and `withHoldBar`, as the Stream and Record keys), and
    `perMode` (`{ <id>: { type?, layout?, skip? } }`). A key saved before (`target`
    `scene␟filter␟mode id`) is moved once by `migrateModeKey` (its press stays). Ids are a
    filter's own (`m1` in two filters are two modes): choosing another filter clears `perMode`,
    `mode`, `longMode` and `modeFor` (`modeFilterWas`).
  - **The page: a mode, then its settings** (2026-11-18; the owner: "the scene modes key is very
    long in the configuration when the different modes are stacked. allow the mode to configure
    to be selected from another selector under the scene modes selector, and have it's
    configurations configurable"). In order: *Scene Modes*; *Mode settings* (`modeFor`, the
    filter's modes; *Settings for* up to beta179) and right under it that mode's *Use in Next*
    (`modeUse`), *Layout* (`modeLayout`) and *Image* (`modeType`); then an open *Key* fold-out
    with *On press*, *Switch to mode* (`mode`; *Mode* before), *Long press*, *Long press mode*,
    *Hold time* and *Palette*. The key-wide *Layout* (`layout`) and *Mode type* (`sourceType`)
    rows are gone from the page (`page(..., lists=False, palette=False)` in
    `make-plugin-files.py`; the tests know `mode` as the key whose type lists are per mode) but
    are still read for keys saved with them, with exactly the old precedence: `layout` behind
    every mode without its own Layout, `sourceType` behind the key's own mode only (extended to
    every mode it would change what Toggle and Next mode keys show for the other modes). The
    own mode's *Image* list loads `sourceType` so the page shows what the key shows; an edit
    there takes its place (`sourceType` set to `auto`). Which mode it was loaded for is kept
    (`modeTypeKeyWide`), so changing *Switch to mode* reloads the lists and is never read as an
    edit of the Image (it neither writes the old mode an image nor erases `sourceType`; tested).
    *Mode settings* is chosen for the user
    when empty: the key's `mode`, else the filter's first mode (`editModeSettings`' `firstMode`,
    read in `upToDate`). Tested: an old key with `layout`/`sourceType` and no `perMode` shows
    the same image, title and state after its page is brought up to date, for its own mode, another
    mode and none (`tests/obs.test.ts`); the page's order and labels (`tests/key-types.test.ts`).
    Plugin 0.28.0.0.
  - **Each mode's settings on a generated page**: one *Mode settings* list (`modeFor`, the
    filter's modes) and, under it, *Use in Next* (`modeUse`), *Layout* (`modeLayout`) and *Image*
    (`modeType`): `modePictureLists()` in `src/key-types.ts`, the key's own Layout and Mode type
    lists under those settings, kept out of `typeLists` so `typeForKept` never sets them.
    `editModeSettings` (on willAppear and every didReceiveSettings) loads the chosen mode's entry
    into the three lists when `modeFor` changes (`modeForWas`) and stores them back when they
    change. Chosen over rows built by a page script (sdpi-components' API would have had to be
    driven by hand, untried) or a fixed number of slots; Scene Fit's *Settings for* in OBS works
    the same way. That the page shows the values the plugin writes back (`setSettings` →
    didReceiveSettings in the page) is believed, not seen on a Stream Deck.
  - **Next mode** sends the filter's own `next` while every mode is used (`How::Next` in
    `scene-modes.cpp`: the mode after the one on, in order, round again; from none, the first;
    never to none). With a mode left out (`skip`) the plugin reads `GetSceneMode` and sends `on`
    with the next used mode after the one on (`modeRequest`): two presses inside one round trip
    can then land on the same mode. *Toggle* with more than two modes goes only between its mode
    and the one before (`How::Toggle`); the README says to use Next mode to go through them.
  - **Image and title** (`shownMode`, `followedModePicture` in `src/pictures.ts`,
    `followedModeTitle`): *Switch on* shows its own mode, lit while on, titled with its name, as
    before. The others show the mode that is on, titled with its name, or the plain mode key and
    *No mode*; lit: Toggle while its own mode is on, Switch off and Next mode while any is. A
    mode's picture: `perMode[id].type`; for the key's own mode the key's Mode type, then the
    image kept from the old Image list; then the mode's *Mode type* in OBS, the type its name
    speaks of (*GBA* is a handheld), then `options/mode`; drawn as a mode (`art.mode`). Its
    Layout: `perMode[id].layout`, else the key's. `remembered` keeps, for the three that follow,
    the mode that was on (`Seen.id`, under `for` = the target + `|follows`); a Switch on key keeps
    what it kept before.
  - Tested against a stand-in server in `tests/obs.test.ts` (an old key, Next with every mode
    and with one left out, Switch off, Toggle, per-mode pictures and layouts, Automatic, a long
    press, OBS gone) and `tests/key-types.test.ts` (the per-mode lists), not against the real
    filter or on a Stream Deck.
- **A key that switches something shows which way it is with a mark, not by brightness.** The
  owner: "the options that have an on and an off or toggle must represent that current status on
  the key with the image ... right now, the stagekit blur button just flashed". The pack's own on
  and off pictures differ only in brightness; `tools/make-state-icons.py` writes marked copies
  (`imgs/pack/<palette>/status/on|off/`, a green dot on, a red "no" sign in the same corner
  off, 256 colours; the off picture is brought to most of the on picture's brightness,
  `OFF_BRIGHTNESS`, and partly greyed: dimmed with a red slash across it, up to beta149, the
  owner found the off keys "very dark and virtually impossible to recognize"). Since beta151 an
  off picture used by one key picture only is drawn from that key's *on* picture: the pack's own
  off picture of a hidden source is an empty screen, and the owner's capture keys "all look
  virtually identical ... how can i know which is which to turn on". Only an off picture several
  on pictures share (the blur types') is kept as drawn) and `stateFile`/`showOnOff` (`src/actions/shared.ts`) set both states' pictures and
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
- **The Stream and Record keys** (`src/actions/output.ts`, one `OutputKey` class, UUIDs `.stream`
  and `.record`, beta168; `src/live.ts`, `src/animate.ts`, `src/hold.ts`). The owner: "Id like
  animated stream Start buttons and stream Record buttons. both should be able to be set to
  toggle, start, pause or stop, and have animations that are relevant, and match the existing
  color themes", then "the new keys also need a long press setting". Like the Switch Scene key
  they use obs-websocket's own requests and events, nothing of the OBS plugin: `ToggleStream`,
  `StartStream`, `StopStream`; `ToggleRecord`, `StartRecord`, `StopRecord`, `TogglePauseRecord`,
  `PauseRecord`, `ResumeRecord` (`requestFor`); state from `GetStreamStatus` / `GetRecordStatus`
  and `StreamStateChanged` / `RecordStateChanged` (`stateOfEvent`: starting, live or recording,
  reconnecting, paused, stopping, idle). **A stream cannot be paused** (OBS has no such thing and
  obs-websocket no request): the Stream key offers no Pause, and its tooltip and the README say
  why. Do not fake one. Settings: `press`, `longPress` (`none` as it comes, and unset reads the
  same), `holdMs` (800, 300 to 5000), `animate`, `showTime`, `sceneType` (with its Layout list),
  `palette`. On (second state, green mark) while the output runs (`isOn`: not idle or starting).
  `seen.type` keeps the last state; with OBS gone the key shows it still.
  - **Art**: `tools/make-live-icons.py` draws every frame (`FRAMES`: stream idle 1, starting 8,
    live 8, reconnecting 8, stopping 8; record idle 1, starting 8, recording 8, paused 12,
    stopping 8; 70 frames) in the pack's framed screen, in every palette, as
    `imgs/pack/<palette>/live/<key>-<state>-<n>.png` with marked copies under `status/on|off/live/`
    (its own call of make-state-icons.py's `marked`), lists them in `src/pack.json` (group `live`)
    and writes `src/live.json` (the frame counts and each palette's accent, which the TypeScript
    reads: one source). The record dot is red (`RED`) in every palette; waves, the ring of light
    and pause bars are the accent (the strongest colour of the pack's key frame; mono's ink).
    make-option-icons.py's word-stripper skips `live/`. SVGs: make-svgs.py's `live_pictures`.
    Run it after make-option-icons.py and make-state-icons.py (a later make-state-icons.py
    re-marks the frames from the 256-colour plain ones: harmless). A Scene type shows the type
    with a still badge (`stream-on|off`, `record-on|off|paused` in make-badges.py's `OWN`).
  - **Animation is frames by `setImage`**, not an animated GIF: the SDK's `setImage` documents
    file paths, base64 data with a mime type "(e.g. PNG, JPEG, etc.)" and SVG; GIF is only named
    for the manifest's state images, so a GIF through `setImage` is not known to animate. Frames
    are file paths (a Layout's inset composes data URLs once, cached). One `Animator` timer for
    every key, a frame each `FRAME_MS` (125 ms), at most 12 keys a tick taken in turn; a key plays
    only between willAppear and willDisappear and only in a moving state with *Animate* on; a
    resting key (held for a long press) holds still.
  - **Long press** (`HoldPress` in `src/hold.ts`, any key may use it; only these two do). With
    *Long press* on Nothing the press acts on key down at once, as before; set, the choice is made
    on key up (short) or when the hold time is reached (long, once, and the key up after does
    nothing). While held a bar fills under the picture (`withHoldBar` in `src/compose.ts`,
    `HOLD_BAR`, the palette's accent, ten steps, cached) and the animation rests.
  - Titles (`outputTitle`): the press's name, the state or the time (counted on locally each
    second and re-read from OBS every ten), and *Hold: Stop* when a long press is set; three
    lines at most, a two-line name shortened. Tested in `tests/output.test.ts` against a
    stand-in server that streams and records. Not run on a Stream Deck, nor against a real OBS.
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
  the picture of what the source is (its name's source type, then `inputPicture`, by OBS input
  kind; `obs.inputKind` follows a Cropped View to what it shows), before the filter's general
  picture (`chooseFor`). A key's type lists, when set, come before all of these (see *Every key
  chooses its image from type lists*). The owner:
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
- **The Update button lists what changed** (`notes_of`, `read_changes`, `order_changes` in
  `src/update/updater.*`; `show_changes` in the dock; beta185). The owner: "id like future
  updaters to show the recent changes clearly at a high level when pressing the update button".
  The release bodies come with the list of releases the updater already reads (every page, no
  request per release); every release after `STAGEKIT_RELEASE` up to the one offered, drafts
  left out, pre-releases in whatever the betas setting, newest first, at most `kMostChanges`
  (15) and then "and N earlier". Each shows the body's first paragraph (the notes' **line 3
  summary**) and the bold leads of the bullets under *New*, *Changed* and *Fixed*, at most
  `kMostPoints` (4), a bullet without a bold lead by its first sentence; never *Good to know*,
  nested bullets or anything from `### Checksums` (or the *still being built* line) on. Markdown
  marks are taken out. *What's new* opens the same list with a Close button; the release page is
  a button in both. So **a release's notes must start with a one-line summary on line 3, and each
  bullet of New, Changed and Fixed with a short bold lead that reads alone**: that is what users
  see before installing. Tested in `[updates: what changed since your version]`
  (`STAGEKIT_TEST_ONLY_UPDATES`), which also prints the list made from the real beta179 to 181.
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
- **A key of `data/locale/en-US.ini` is used once.** The window layout's menu and Apply Layout
  both used `Layout.Save` (up to beta148), and OBS keeps one of the two texts for both. The
  window layout's strings are `WindowLayout.*` since beta149. Check for a repeated key when adding
  one.
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
- **The scene filters share a groundwork; use it, do not copy it.** Their ids are in
  `src/common/scene-filter-ids.hpp`, so Morph (`draws_nothing`, which lists the five that draw
  nothing) and the copy tools never wait for a filter's own files. A drawing scene filter Morph
  should still take apart says so with `morph_accept_drawing_filter`. `kOwnSettings` goes by the
  last `|`-separated part of a setting's name (`fit|Cam|anchor_x` is `anchor_x`), and lists
  `mode_now`, `mode_before`, `managed` and `managed_by`. A PickList's heading can be made at run
  time (`title_text`). `pick_list_on_change` stays the fuller *Change with*'s, rebuilt into it
  each time a window is built; a kind's own callback is `pick_list_on_change_for`, called after
  it. `item_transitions_owner` is asked per item. Whether a mode not showing holds an item hidden
  is `mode_holds_hidden` / `mode_settling` (`holds_hidden_register` adds a kind's answers; Link
  Visibility's and Scene Modes' are there); Show in Scenes, Standby and Scene Modes ask it. *Switch to scene* is
  `go_to_scene` (`src/common/go-to-scene.*`, with `stay_in`; Show in Scenes passes none), and its
  test procs are added by `go_to_scene_add_test_calls`. Each scene filter has a block of its own,
  between `==== scene-filters:<unit> ====` and `==== end scene-filters:<unit> ====` lines, in
  `src/plugin-main.cpp` (includes and registration), `CMakeLists.txt`, `data/locale/en-US.ini`,
  `tests/ambient-test.cpp`, `README.md` and here; write only inside your own. `tests/run-tests.sh`
  stops on a locale key used twice and on a failed build.
<!-- ==== scene-filters:all-sources ==== -->
- **Every scene filter's *Sources covered* starts with *All sources*** (2026-11-17; the owner: "the
  scene filters must have an all sources option"). The saved numbers are append-only and the list
  shows *All sources* first whatever its number: Scene Transitions `cover` (0 All sources, 1 Chosen
  sources), Scene Fit `which` (0 Chosen sources, its *Fitted sources*; 1 All sources), Scene Standby
  `which` (0 Every capture, 1 Chosen sources, 2 All sources), Scene Corners `which` (0 All but the
  background, 1 Chosen sources, 2 All sources), Scene Shadow `which` (0 All sources, 1 Chosen
  sources, listed in `shadow_sources`). **No default was changed**: OBS saves no value left on its
  default, so a filter saved before reads the default, and a new default would change it. A new
  filter therefore starts where an old one was: All sources for Transitions and Shadow, Every
  capture, All but the background, and Chosen sources for Fit (fitting every source the moment the
  filter is added would move and resize the whole scene). *All sources* is the items straight in
  the scene, a group counting as one (Standby opens groups instead and covers no group or nested
  scene: a scene is never empty), never the Morph layer, a stand-in or a private source, less
  *Ignored sources* (`skip`; Transitions' `except`, Shadow's `shadow_skip`), counted afresh at each
  look so a source added later joins. Scene Fit's unlisted entries (`unlisted_sources`, the window
  and the tick both use it; counted every `kLookForSources`, 0.5 s) come after the listed ones,
  top of the scene first, with `fit_entry_defaults` and a group under *Settings for* like a listed
  one. Left out there: `skip`, a source an enabled Fit to Space or another enabled Scene Fit lists,
  everything when an earlier enabled Scene Fit of the scene has All sources (two would fight), a
  source with no size yet (until it has one), and one covering nine tenths of the canvas
  (`screen_share`) unless `fit|<name>|orig_item` names it: fitted once, it stays (the key is saved,
  so it holds over a restart; a game fitted to the screen less a 30 px gap covers 0.91 of it and
  would otherwise drop out at the next load). A source not fitted yet whose box centre lies inside
  a fitted entry's box (`pos` + bounds) waits too: its place inside that space would make the
  engine take the same space for it (what the place lies inside is not in the way) and the two
  overlapped for good; meanwhile it is an obstacle, the other makes room, and it joins once its
  place is clear. And with All sources every entry is `set_claims_firm`: in `FitEngine::solve` a
  place lying inside another fit's claim (`said_`) is first moved to the nearest point clear of
  it (the gap beyond its edge, on the screen) before `free_space` is asked. Without it a newcomer
  and the chat box in the test, each resized, ended in one shared space for good, each place
  inside the other's claim. Only an All-sources Scene Fit sets it, so Fit to Space and a Scene
  Fit on Chosen sources solve as before. A claim of a fit ranked above this one (Space priority,
  `above_`) is passed over by that step and left to `free_space`'s `firm` sides. A full-screen source is no obstacle anyway
  (`free_space` passes over what the place lies inside). A rename moves an unlisted source's
  `fit|<name>|` keys too. **Scene Modes has no All sources, on purpose**: a source in no mode is
  never touched, and a mode of every source (or of every source in no other mode) would hide the
  background, the overlays and every source added later whenever another mode came on.
<!-- ==== end scene-filters:all-sources ==== -->
<!-- ==== scene-filters:modes ==== -->
- **Scene Modes** (`stagekit_scene_modes`, `src/placement/scene-modes.*`; beta155). The owner:
  "scene modes should not have specific names built in. instead scene modes should be able to be
  made manually and customized", with templates to start from. A mode is known by an **id**
  (`m1`, `m2`, ..., from `mode_seq`; never reused, never renamed): settings `modes` (the ids in
  order), `mode|<id>|name`, `mode|<id>|type` (a string id, the Stream Deck Source type's: up to
  beta176 `game`, `window`, `art_app`, `code_editor`, `browser`, `camera`, `capture_card`,
  `display`, `phone`, `tablet`, `chat`, `music`, `alerts`, `title`, `image`, `video`, `starting`,
  `brb`, `intermission`, `ending`; since beta182 every source type of the Scene Mode key's list,
  `handheld` and the rest, in the key's order (the owner: "handheld is not an option"); ""
  Automatic; ids never change or go), `mode|<id>|go_to`, `mode|<id>|sources`;
  `mode_now` and `mode_before` (ids, written by the filter under its lock, in `kOwnSettings`).
  Hotkeys `stagekit.mode.<filter>.<id>`, `.next`, `.none` (shown with the mode's name), and the
  vendor requests `ListSceneModes`, `GetSceneMode`, `SetSceneMode` (an id, or a name) and the event
  `SceneModeChanged`, so a rename breaks no key. The rules: hides before shows in one tick; a
  source in the old and the new mode is not touched; a source in no mode never; a press is queued
  and acted on in the tick; nothing is polled (it listens to `item_visible` on its scene and on
  *Same modes in scenes*, and hides again a member of a mode not on that something else showed);
  a show is passed over when `link_visibility_holds_hidden` or another Scene Modes covering that
  scene holds it (never for a Show in Scenes switch that is off: a switch is a command); registered with `holds_hidden_register`, so Show in
  Scenes and Standby's `stands_in` ask it. At the first look after `scenes_loaded()` a scene that
  disagrees with `mode_now` is put right with a cut (and logged); presses made before are kept.
  "Mode now" in the window is a press that never changes scene; *Switch to scene* goes through
  `go_to_scene` with `stay_in` = its own scene and *Same modes in scenes*. Items inside groups are
  shown and hidden, but a group's own `item_visible` is not listened to (an inner item shown by
  hand is not put back). The window's mode groups and everything below them are taken off and
  added again in place (`rebuild`) on *Add mode*, *Add from template*, *Move up* and *Remove
  mode*; a rename only changes the heading and the *Mode now* list. A mode's PickList is nested in
  its group, and its setting has bars in it: `pick-list.cpp`'s `parse` takes the last bar
  followed by `a`/`t` and a part (`list`, `pick`, `none`, `row|n`), not the first bar. The tests
  reach the vendor requests through the `stagekit_vendor_call` and `stagekit_vendor_events` procs
  (`vendor_api_add_test_calls`); obs-websocket is not there.
  **Hide when on and Leave when shown** (beta177; the owner's two transparent markers *Column:
  Chat* and *Column: GBA* existed only for a Link Visibility *Show only one* between the
  full-screen DSLR and the GBA column; offered a per-mode rule: "option 2 please", then "showing
  DSLR switch back to the Chat mode"). `mode|<id>|hide` (a PickList): when the mode comes on (any
  press, and only on a change of mode, never at the look after loading), these sources are hidden
  in the covered scenes in `plan`, with the mode's own hides (`Work::extra`: through
  `apply_item_change` + `set_item_visible`, never locked by *Lock hidden sources*). They are not
  members: never shown again, never put back, nothing else. A mode's own sources win over its
  hide list. `mode|<id>|leave_on` (a PickList) and `mode|<id>|leave_to` ("" the mode before, as it
  comes; `none` (`kLeaveNone`), only with `allow_none`; or a mode id): `visible_cb` (not while
  loading, never for the filter's own changes, `applying_`) queues a `How::Leave` press carrying
  the mode on (`from`) and the source (`spare`) when a source of the current mode's `leave_on` is
  shown, and does not queue it for `put_back`. `act` drops it unless `from` is still the mode on;
  the change never hides `spare` (not by the new mode's hide list, not as a member of a mode not
  on) and logs that it was spared. A leave changes no scene. Both lists follow a rename; *Remove
  mode* points a `leave_to` naming it back at the mode before. Tested in `[scene modes: hide when
  on, leave when shown]` (`modes_hide_test`, at the end of the Scene Modes section; alone with
  `STAGEKIT_TEST_ONLY_MODES_HIDE`): on and off screen, a Link Visibility before and after, by
  hand, the loop case, a filter saved without the keys, the window (rebuild, rename, *The mode
  before*).
<!-- ==== end scene-filters:modes ==== -->
<!-- ==== scene-filters:transitions ==== -->
- **Scene Transitions** (`src/placement/scene-transitions.*`, `stagekit_scene_transitions`; the owner
  was promised "one place to set how every source in the scene shows and hides, instead of a
  transition setting inside each filter") writes OBS's own item transitions, **only from its tick,
  only on events** (created, switched on, settings changed, `item_add`, a covered item's
  `item_visible`, a nearest-edge item still for 0.25 s after `item_transform`), only once
  `scenes_loaded()`, and only where `item_change_matches` says the item differs: a collection
  loaded again writes nothing (a test saves and loads a scene and counts writes through the
  `stagekit_scene_transitions_writes` proc). A source just added, and a scene just met, are looked
  at only `kAddedNs` (0.1 s) later (beta: the next after 169): OBS adds a scene's items and sets each
  item's saved transitions after, so acting at once now and then wrote one it was about to load (the
  test *a scene loaded again ... is written nothing* failed about one run in five). It passes over owned items
  (`item_transitions_owned`), private sources (stand-ins, the item Morph's layer), *Ignored
  sources* (`except`; *Leave these alone* before), items an enabled Show/Hide Animation names and, unless `in_groups`, items in groups.
  Items are known by id in their scene or group (`Key`), never held. The first enabled one in a
  scene's filter list is the one used. **`kTransitionScene` ("As the scene sets") is resolved
  only in `resolve_item_change`** (`src/common/item-transition.*`), through the lookup the filter
  registers (`item_transitions_scene`); `set_item_transitions`, `apply_item_change` and
  `apply_item_change_as_owner` all go through it, so a filter that sets item transitions through
  them gets the choice for free, with a fade of the way's own duration where the scene sets
  nothing. It is the default of `item_transition_defaults` and `item_change_defaults` since
  beta155: settings left on the old default (fade) read it, which changes nothing in a scene
  without a Scene Transitions. After another filter's own choice the scene's transitions are put
  back once that change's duration + 100 ms has passed (from `item_visible`), never part-way; a
  full pass skips an item with such a wait pending. A Standby must resolve *before* it swaps a
  change for its twin (the twin, a private source, is never covered and would get the fallback).
  Never write item transitions into a generated collection: this filter writes them at the first
  load. A mode change of Scene Modes is tested here on *As the scene sets* (the scene's way in
  view, a cut out of view); pairing two captures in the item Morph on a mode change is not tested.
  **A scene being destroyed cannot be reached through a weak reference**, yet it signals every item
  it removes after its filters are gone: `filter_remove` lets go by the pointer it was given
  (`detach(parent)`), and each signal handler checks under `g_mutex` that its filter is still in
  `g_all` (`live`). Before the integration a removed scene's `item_remove` reached a freed filter
  and OBS hung at shutdown (seen in the tests). A new filter that listens to its scene's or groups'
  signals must do the same.
  **Override other filters** (`override_others`, a bool, off as it comes; beta179). The owner, with
  Link Visibility and Show in Scenes set to Morph of their own: "none of the other transitions seem
  to do anything. only morph works at all". Those filters' own choice wins for their change, and
  the scene's is only put back after; they can be set to *As the scene sets*, or this is ticked.
  Then the scene's choice (the usual one, or a listed source's own) replaces whatever was asked,
  in the one place every filter goes through: `resolve_item_change` (and so `apply_item_change`,
  `item_change_matches`) and `set_item_transitions` (`overridden` in
  `src/common/item-transition.cpp`). The lookup (`SceneLookup`) says `overrides` beside its answer;
  `covers` already leaves out private sources, the item Morph's layer, ignored and animated
  sources and, without *Sources in groups*, grouped ones; an owned item (Standby,
  `item_transitions_owner`) is never overridden. While no filter has it ticked
  (`item_transitions_scene_overrides`, a count) a change with no *As the scene sets* in it is not
  looked up at all, as before. The put-back finds the item as it should be and writes nothing.
  Scene Modes' put-right after loading is a cut through `cut_item_transitions`, which is never
  overridden. **The list's heading follows *Sources covered*** (`title_text`, `cover_changed`):
  *Sources with own settings* with All sources, *Chosen sources* with Chosen sources, each with
  its own tip; the owner, of the one tip for both: "this tooltip makes no sense." **Morph's own
  choices** (*Unpaired sources* and its transition's settings, *Morph the main image*, *Use
  visible pixels*, *Ignore shadows & outlines*, easing, *Linked sources*) were already in its
  window through the fuller *Change with*; a test now shows them there and their values on the
  items' Morph transitions. Tested in `[scene transitions: override other filters, the list's
  wording, Morph's choices]` (`scene_trans_override_test`, alone:
  `STAGEKIT_TEST_ONLY_SCENE_TRANS_OVERRIDE`).
<!-- ==== end scene-filters:transitions ==== -->
<!-- ==== scene-filters:fit ==== -->
- **Scene Fit and Fit to Space share one engine** (`src/placement/fit-engine.*`, beta155). A Fit
  to Space is one `FitEngine` and one `FitScan` (`fit-item.cpp` keeps only its window and
  registration); a Scene Fit (`stagekit_scene_fit`, `src/placement/scene-fit.*`) is one
  `FitEngine` per entry and one `FitScan` per way of measuring. The engine's tick is cut into
  steps, `prepare` (hidden, coming back, kept size, own visible-pixel cut), `motion` (off
  screen, dragged by hand, anchor), `observe` + `solve` (what is in the way, the goal),
  `publish_claim`, `advance` (the glide) and `apply`, and `FitEngine::tick` calls them in the
  order the old `FitItem::tick` did: change them there, never in one host only. Scene Fit
  works its entries out in the order of *Fitted sources*, top first, then bottom first, and so on
  in turn (up to eight passes, `kPasses`; four, all top first, up to beta194) while one entry's
  new claim moves another's goal, before any glide
  starts: each entry sets off once per change, all in one tick. The list's order is a priority
  only with the Scene Fit's *Priority by list order* (`set_entry_index`; see *Space priority*);
  *Take over Fit to Space* copies `space_priority` per entry and the first filter's
  `list_priority`. An entry's settings are
  `fit|<source>|<Fit to Space's key>` (the self-written ones too: anchors, `keep_k`, `orig_k`;
  `kOwnSettings` matches on the last `|` part), but its two lists are `fit_<the name in hex>_skip`
  and `..._share_scenes` (`fit_list_prefix`): a PickList's setting cannot hold a bar and a
  source's name may. The overlap switch `skip_fill_on` is one for the whole Scene Fit, so the
  *Fit to Space Overlap* key works on it. Every entry always carries its own settings (seeded by
  `fit_entry_defaults`, gap 30); with *All sources* the unlisted sources are entries of the same
  kind (see the all-sources entry above). The window has a group per entry
  (`fit|<source>|g`, controls from the shared `fit_add_controls`), only the one chosen in
  *Settings for* (`fit_for`) shown, kept in step with the list by `pick_list_on_change_for`.
  *Take over Fit to Space* (`take_over`) copies each enabled Fit to Space's effective settings
  (defaults included: Fit to Space's gap is 80, Scene Fit's 30) and switches it off. A
  stand-in twin of an entry is not in the way of the others (its item's claim holds that room).
  **Settings: Same for all sources** (`settings_mode`, by number, append-only: 0 *Each source its
  own*, as it comes and for every filter saved before; 1 *Same for all sources*; beta173). The
  owner: "I'd like the Fit to scene filter also have the option to select one or more sources and
  specify the priority for all the selected sources together. that way i can have multiple fit to
  filters with similar settings or priorities instead of multiple for each source." In 1 every
  entry (unlisted ones of All sources too) reads *how* it fits, Space priority included, from one
  set, `group|<Fit to Space's key>` with the lists `group_skip` and `group_share_scenes`
  (`FitEngine`'s `look_prefix`, `set_look`); what an entry writes for itself (`kFitKeys`' `own`:
  anchors, `orig_*`, `keep_*`) stays under `fit|<source>|` either way, so each keeps its home and
  kept size. The window shows one group, `group|g`, and hides *Settings for* and the entries'
  groups. A change of mode is *settled* once (`settle_mode`, against `settings_mode_was`, which
  is not in `kOwnSettings` so a copied filter is not settled again): to 1 the shared set is copied
  from the first entry (the defaults with none), back to 0 it is copied into every entry, so
  nothing moves. It is settled in the window's callback (so the window shows the values at once)
  or, from outside it, in `update`; never as OBS makes or loads the filter (`first_update_`: a
  mismatch is only written down), so a hand-made or saved filter is read as it is. *Take over*
  into an empty Scene Fit on 0 sets 1 when two or more filters fit alike (`same_look`, defaults
  included); into one on 1, the sources take its set (their own copies are kept for 0). Several
  Scene Fits are the owner's *groups*: their entries' claims carry their own `Rank`, so a group on
  *Takes space first* outranks one on *Makes room* entry by entry, one-way, whatever the filter
  order, and with *Priority by list order* on both the higher filter wins; inside one group all
  entries share one rank and go by each other's claims as before (`kPasses` unchanged).
  `set_claims_firm` and `above_` needed no change. Tested in `[scene fit groups: ...]`
  (`scene_fit_groups_test`, alone with `STAGEKIT_TEST_ONLY_FIT_GROUPS`).
  Not done in Scene Fit (each its own change later): dropping `kComingBack`, one glide time for
  all entries during a window switcher's change, a Link Visibility test in both orders, the
  window-switcher sync test, and the cost test against seven Fit to Space filters (it waits for
  `stagekit_timing_take`).
- **Fit to Space is idle off screen** (`FitEngine::idle`, perf cut 1): with its scene not
  showing and nothing asked of it (`dirty_`, `placed_`, `shared_in_`, `new_item_`) its tick is
  skipped but every fifteenth, each engine on a frame of its own, and the time skipped is added
  to the next full tick (`idle_s_`), so a kept size waiting to be read and a hidden item's room
  carry on as before. The tick the scene comes on is a full one (`kArriving`: no glide). The
  room a shadow takes is read again only when `shadow_room_revision()` changes (perf cut 2),
  raised in `ShadowBackdrop::update` and `settle_items`: anything else that changes the room
  must raise it too. `stagekit_test_fit_counts` (a global proc) gives a fit filter's full ticks
  and glides, for the tests.
<!-- ==== end scene-filters:fit ==== -->
<!-- ==== scene-filters:standby-corners ==== -->
- **Scene Standby and Scene Corners keep a filter on each source; neither draws** (Scene Corners
  except with *Corners on: The whole scene*, below)
  (`src/placement/scene-standby.*`, `src/placement/scene-corners.*`, both through
  `src/common/managed-filter.*`, `ManagedSet`). Each adds a `stagekit_standby` or `stagekit_corners`
  to each source it covers, with `managed` and `managed_by` (the scene's name) in its settings, and
  pushes only the look keys that differ, from its own tick (once a second on screen, three off it, at
  once after a change; never before `scenes_loaded()`). A filter takes its look only from the scene
  filter in the scene it names; when that one is gone the first other one covering the source (by
  scene name) rewrites `managed_by` and takes it over: no last-edited-wins. A filter of the kind the
  user made (no `managed`) is left alone and no second one is added. One is taken off only from a
  scene filter's tick, after `kManagedGraceS` (3 s) covered by none, **never from `destroy`**, which
  also runs at a change of collection and at shutdown. Scene Corners is the switch for its filters
  (`mirror_enabled`, on an edge of its own state); Scene Standby never switches its filters on again.
  The registry (`g_entries`, and Scene Standby's `g_hidden`) holds no item and no strong reference:
  copied out under its lock, nothing held while OBS is asked.
- **A managed Standby filter goes idle, never away by itself**: not covered for 2 s by an enabled
  Scene Standby with *Show when no image* (`check_managed`, `idle_`), it draws straight through and
  takes the size of what comes before it. `Standby::update` makes the image again only when
  `picture`, `file` or `source` change (`made_`, the `test_state` proc): a pushed duration made it flash.
- **Stand-ins are per scene since Scene Standby** (`sync_stand_ins`): a scene has them when the
  filter's own `when_hidden` is ticked (every scene) or its Scene Standby lists the source under
  *Show when hidden* (`scene_standby_hidden`, that scene's change, per source). `g_stood_in` is
  (scene, source) pairs, so the transitions owner is per item: in a scene without stand-ins the item
  is free for Show in Scenes and Scene Transitions. An item in a group was owned before and is not
  now (it never had a stand-in). The change given to each scene is remembered as text (`applied_`) so
  a change made in the Scene Standby is applied again. `sync_stand_ins` resolves *As the scene sets*
  (`resolve_item_change`, in the item's scene) before the change is swapped for the twin.
- **Scene Corners leaves the background out by size alone** (`screen_share` in
  `src/common/scene-util.hpp`, which Scene Fit uses too: bounds, or source size
  times scale less crop; no `refresh_item`, which would mark the scene changed every second): items
  from the bottom while each covers 90 % of the canvas, with *All but the background* only (*All
  sources* covers the background too). Corners & Edges' look controls and defaults are
  shared (`corners_add_look_properties`, `corners_look_defaults`), so the keys are the same.
- **Scene Corners' *Corners on*** (`corners_area`, by number, append-only: 0 *Each source*, as it
  comes and as every filter saved before reads; 1 *The whole scene*; 2026-11-17). Proposed to the
  owner with Scene Shadow's *Shadow from* ("a whole-scene option ... the scene's picture itself gets
  Corners & Edges' look at the canvas edges, as one shape"); the owner: "yes to both". With 1 the
  scene filter **draws**: the scene (`draw_filter_input`) cut by `CornersPaint` (moved out of
  `src/shape/corners.cpp`, which Corners & Edges now draws with too; `CornerShape`,
  `read_corner_shape`) over the whole canvas, sizes in canvas pixels (`screen_px` hidden), once a
  frame (`last_frame_`, as Scene Shadow). It publishes no source, so the managed filters go by the
  usual grace and come back on 0. It keeps itself after every Scene Shadow and Scene Blur
  (`keep_last`, twice a second once `scenes_loaded()`, logged), so the shape is cut last; Scene
  Shadow passes over a Scene Corners before it, as it does a Scene Blur; Scene Blur (its file not
  touched here) may say once in the log that a filter draws before it, for the half second until
  it moves. `draws_nothing` in Morph asks `scene_corners_draws` for Scene Corners (no longer by id
  alone), and `morph_accept_drawing_filter(scene_corners_draws)`: **Morph carries the shape**
  (`scene_corners_now`, `Morph::draw_rounded`): the change (morph, shadow and all) is drawn into a
  canvas picture and cut to `lerp_corner_shape` of the two scenes' shapes (a scene without one is
  the other's at no size; the style changes half-way). Tested in `[scene corners: the whole scene]`
  (`scene_corners_whole_test`, alone with `STAGEKIT_TEST_ONLY_CORNERS_WHOLE`; contact sheet
  `docs/images/scene-corners.png` from `$out/scene-corners`): corners cut, edges and inside
  untouched (0 off), filters off after the grace and back, outline, morphed half-way with corners
  at half size, moved after a Scene Shadow and both drawing. Only OpenGL.
  Corners & Edges' *Which corners* group is named `corners_which` (it was `which`, which Scene
  Corners' list of sources already had: OBS refused the group, leaked it, and Scene Corners' window
  had no corner tick boxes until then). A plain group holds no setting, so nothing saved changes.
<!-- ==== end scene-filters:standby-corners ==== -->
<!-- ==== scene-filters:shadow ==== -->
- **Scene Shadow** (`src/look/scene-shadow.*`, `src/look/look-paint.*`,
  `data/effects/stagekit-scene-shadow.effect`; beta155): a scene filter that draws, over
  OBS's own picture of the scene. A filter that drew every item itself (the first design, with
  corners in it) would have had to repeat OBS's item drawing with public calls only (crop to
  bounds, scale filters, blending, groups, nested scenes, item transitions to their last frame):
  each a way to break the owner's main scenes. So corners are Scene Corners' Corners & Edges per
  source, and only a mask is drawn here, where a mistake shows as a slightly wrong shadow edge. The scene draws itself (`draw_filter_input`); the filter then draws the scene's items
  again with `LookPaint::draw_item` as two silhouettes, S (the casting sources) and M (every
  source not background; skipped when it would equal S), and lays the Kawase-blurred S over the
  picture **off M** (`MaskedShadow`, `Ring` for the outline). That pass works **in linear light**
  (`to_linear`/`to_srgb` in the effect): OBS lays a source over what is under it in linear
  light, and done on raw values the shadow came out darker than the per-source filter's (52
  against 84 on grey 128). **Only the part of a pixel no source covers is shadowed** (`split_at`
  in the effect, beta186): M holds the sources as drawn (premultiplied), so a pixel splits into
  their part and what is under them, and the shadow and the outline go on the second only
  (`laid_under`). Shadowing the whole pixel by the uncovered share, as before, left most of what
  is under a smoothed edge unshadowed: the owner's "text with the scene shadow has a noticeable
  separation from the shadow", a 1 px bright line of the orange background between each glyph
  and its hard shadow (red 139 against at most 66 in the test). Where M's colours differ from
  the scene's (an overlap, a background listed above a source) what is under is held to the
  uncovered share, so a source's inside is never darkened. It never moves or resizes anything. An item whose show or hide
  transition runs is drawn through the transition (`item_transition_running`: a show's second
  side, a hide's first side, is the item's source), so its shadow fades with it; drawing it a
  second time after the scene has drawn it does not keep it from ending (tested by the
  transition's `transition_video_stop`: the tests have no sound until the layout section, so the
  sound half never ends there). Flags by item id (`ShadowFlag`: background, casts, masked) are
  worked out in the tick (`classify`), four times a second while the scene shows or was drawn or
  asked for by Morph in the last second, at once on coming into view or a settings change, never
  before `scenes_loaded()`; **and in the very tick an item is added to a scene in view**
  (`has_new_items`, beta186: when `scene_revision()` has changed, the scene's item ids are
  compared with the flags', and a new one makes the look due at once). OBS calls tick callbacks
  before it ticks sources, so the item Morph's layer, added from its tick callback, is looked at
  before the frame that first draws it. Up to beta174 an item not yet looked at was masked and
  cast nothing for up to a quarter second, and the layer is what draws both items of an item
  Morph (their own transitions draw nothing while *Paired*): the shadow of the pair went for
  that time at the start of every item Morph, the owner's "shadows flicker when ... i
  switch/show or hide sources inside a scene" (shadow 155.8 against 362.4 in the test, at the
  layer's first frames). An item still not looked at when drawn (none should be) is masked and
  casts nothing, but the Morph layer always casts. Fade and Slide never flickered (worst 0.97 of
  the frames round it), nor did a change of scene with Morph or a scene coming into view.
  Tested in `[scene shadow: no flicker through transitions]` and `[scene shadow: no gap at a
  smoothed edge]` (alone: `STAGEKIT_TEST_ONLY_SHADOW_FLICKER`; with sound, last in the file).
  **A frame-by-frame test draws on OBS's video thread**, from a main-rendered callback, and
  makes its changes from a tick callback, by the video clock: drawn from the test's own thread
  while OBS ticked, a scene showed blinks OBS cannot show (an item moved from another thread is
  seen with its matrix half-made, an item transition ticking has its textures locked and draws
  a hiding item as nothing), and on a busy machine the queue's full run failed on them.
  A filter taken off while it is drawn (from another thread) has no parent any more, and OBS's
  `obs_source_skip_video_filter` then crashes: Scene Shadow gives up through
  `skip_filter_input` (`src/common/filter-input.hpp`), as every filter now does (see *A filter
  skips with `skip_filter_input`*).
  Background:
  listed, or the unbroken run of bottom items covering nine tenths of the canvas (groups only if
  listed); the Morph layer casts and is never background; a stand-in twin takes its owner's
  flags; a source with an enabled Shadow, Glow & Outline casts nothing. Drawn **once a frame**
  (`last_frame_`, with size and linear-light state): later draws in that frame reuse the picture.
  Skips itself with a drawing filter before it (`morph_draws_nothing`, logged once), on a group,
  on an HDR canvas, before loading, and with nothing to draw. Its sizes are canvas pixels; it
  has no `shown_scale` and no room.
  **Morph carries it** (without it every change between Live and Chat Live would be a
  cross-fade; `morph_accept_drawing_filter(is_scene_shadow)` in `register_scene_shadow`, so
  `plain_scene` takes such a scene apart): when either scene has one
  (`scene_shadow_now`, a copy of the look faded by its switch and the flags, taken each frame),
  `Morph::draw_shadowed` draws `draw_morph` three times into canvas pictures, `pass_` Picture,
  Cast and Mask (only when some item is masked but does not cast), each part weighted by what
  it does in the old and the new scene mixed by eased t (`weight`, `flags_of`; a part drawn
  part-way goes through `draw_part`/`draw_texture`'s new `opacity`), and composes them with
  `LookPaint` and `lerp_look` (a missing side, or a part switched off, is the other side's at
  opacity 0). A scene with no Scene Shadow gets the flags one would give it as it comes
  (`scene_shadow_default_flags`, worked out with the plan), so the shadow fades over its
  background. The item Morph needs nothing: its layer item casts.
- **Scene Shadow's *Shadow from*** (`shadow_from`, by number, append-only: 0 *The scene as drawn*,
  as it comes and as every filter saved before reads; 1 *Each source by itself*; 2026-11-17). Put to
  the owner with Scene Corners' *Corners on* ("every casting source gets its own shadow ... falls
  onto whatever lies under that source, other sources included, and is covered by the sources
  above it, the way separate per-source Shadow, Glow & Outline filters would look"); the owner:
  "yes to both". Drawn in **layers**, not a pass per item: `shadow_layers` (in `look-paint.*`)
  cuts the stacked items (not background, drawn: `item_drawn_box`, from the draw transform on the
  drawing thread, never `item_box`) bottom first into runs whose boxes grown by `shadow_reach`
  (offset, twice the softness, the line) do not meet; a run is one layer, at most
  `kShadowMostLayers` (12, the last takes the rest). Per layer a cast (its casting items) and a
  mask (its items and every layer above, made top-down by `copy_onto` the mask above);
  `LookPaint::compose_layers` lays layer by layer from the bottom over the picture so far
  (`between_` ping-pong; the effect's `image_straight` says the input is compose's own straight
  alpha). Exact for opaque sources: a pixel takes the shadows of the layers above its topmost
  source. Sources apart are one layer, the same cost and picture as mode 0 (tested 0 off). Once a
  frame as before; `lay_over` (Scene Blur's *Each source by itself*) chains the same layers over
  its redraw, and each layer draws Scene Blur's blurred pictures as before. `lay_over` now takes
  a shadow up to two frames old and only from an enabled filter: a drawing from outside OBS's
  video thread (the tests' `render`) could see the frame's time move on between the shadow's
  drawing and Scene Blur's, and lost the shadow on the blurred source (seen once in a full run). The `layers` proc
  gives the last frame's count. **Morph** (`draw_layers`): when either side is *Each*, the parts
  are layered the same way by the union of their quads in both scenes plus 5 % of the canvas for
  an overshooting curve, `in_layer` (parts `layer_lo_`..`layer_hi_`) limits the Cast and Mask
  passes (`weight`, `draw_side`), and `compose_layers` composes. Both sides shadowed and drawing,
  with different *Shadow from*: a cross-fade (`mix_cb`), logged once per scene (either way all
  along would jump at one end). Tested in `[scene shadow: each source by itself]`
  (`scene_shadow_each_test`, alone with `STAGEKIT_TEST_ONLY_SHADOW_EACH`; frames 5 to 8 of
  `docs/images/scene-shadow.png`): against two per-source Shadow, Glow & Outline filters at most
  1 level off (0.036 mean), 2 layers and 1 apart, outlines, with Scene Blur on the lower source,
  morphed half-way with the shadow falling, the mixed cross-fade. Only OpenGL.
  Each layer goes through `compose`, so `split_at` (only the uncovered part of a pixel is
  shadowed) holds here too, and the tick's look for a new item is the same in both modes. Since
  that reads the mask's colours, a layer's mask is its own sources drawn first and the mask above
  laid **over** them (`copy_onto(..., over = true)`, in `render_layers` and Morph's
  `draw_layers`), so what is on top is on top in the mask; copied first, as in beta176, a lower
  source's colours stood where it showed through a higher one's edge. `[scene shadow: no gap at
  a smoothed edge]` checks both modes, the disc a layer above a small block's.
<!-- ==== end scene-filters:shadow ==== -->
<!-- ==== scene-filters:blur ==== -->
- **Scene Blur** (`stagekit_scene_blur`, `src/look/scene-blur.*`; the shared blur in
  `src/blur/blur-pipeline.*`; two techniques, `MaskPiece` and `MaskedComposite`, in
  `stagekit-looks.effect`; 2026-11-17, the owner: "also make another scene filter. The filter is
  a blur filter ... blur all sources, live and capture sources only, specific sources, and also
  never blur or ignore specific sources. Also, there should be a companion stream deck key").
  - **Per scene by drawing over the scene**, as Scene Shadow does, never by Blur filters on the
    sources: a source's filters go with it into every scene, and the owner asked for the blur in
    one scene only. The scene draws itself (`draw_filter_input`); then a **mask** is drawn from
    the top item down, each item by way of a picture of its own (`LookPaint::item_picture`, new:
    its source, crop and running show or hide transition at the source's size, and the draw
    transform), with "under" blending (`GS_BLEND_INVDSTALPHA, GS_BLEND_ONE`) so its coverage
    counts only where nothing above it covers, red where it is blurred and nothing where it is
    kept clear. Blending a source over the mask could not be trusted: nested scenes and groups
    set their own blend state, which is why each item goes through its own picture. Items under
    the lowest covered one are skipped. The **picture blurred** is the scene, or, when a sharp
    item lies above a covered one (`sharp_above`), the scene drawn again without such items
    (`tr_under_`), or the sharp camera's colours smeared into the blurred game round it (the test
    *the sharp badge is not smeared* fails without it, by 57 levels). Then `BlurPipeline::run`
    and `composite_masked` into `tr_out_`, drawn out following `gs_get_linear_srgb()`. Once a
    frame (`last_frame_`), as Scene Shadow.
  - **Coverage** (`cover`, by number, append-only: 0 *All sources*, 1 *Live and capture
    sources*, 2 *Chosen sources*), worked out in the tick four times a second while on and
    showing (`classify`, per item id, `covered_`); nothing while off. Live is Blur's rule,
    shared as `live_captures_in` (`src/blur/ramp-blur.*`: `find_live` split into
    `collect_live`), plus media (`ffmpeg_source`, `vlc_source`), which the owner named.
    *Never blur these* (`never`) wins over all three, also for a Cropped View of a listed source;
    a group or nested scene counts as live only by a live capture in it that is not listed. A group or
    nested scene is one item: blurred or sharp whole. A Standby stand-in follows its item; the
    item Morph's layer is covered only with *All sources*. An item added since the last look
    stays sharp until looked at.
  - **The Blur filter's code was moved, not copied**: `BlurLook`, `BlurRamp`, the enums, the
    controls (`add_blur_look_properties`, `add_blur_ramp_properties`, same names and numbers),
    `blur_at_level` and every pass are in `blur-pipeline.*`; `RampBlur` holds a `BlurPipeline`.
    Its log line on a missing `stagekit-blur.effect` still says *Ramp Blur*. Scene Blur needs
    `stagekit-looks.effect` (the mask): without it it draws nothing.
  - **SwitchRamp** for on and off (`ramp_in_ms`, `ramp_out_ms`, `ease_*`); *Blur transition*
    (`ramp_style`) chooses grow or fade as in Blur. A state restored at load is taken as it is
    (SwitchRamp's first tick). Hotkeys on the scene: `stagekit.sceneblur.toggle.` / `.on.` /
    `.off.` + the filter's name, made again on a rename keeping their keys.
  - **Morph**: `morph_accept_drawing_filter` now keeps up to eight predicates (it held one,
    Scene Shadow's). Scene Blur is accepted only while it blurs nothing (`blurs_nothing`: level
    0, or no covered item showing at the last look); while it blurs, `plain_scene` refuses the
    scene and the change is Morph's cross-fade, which keeps the blur on screen all the way.
    Carrying the blur through a morph (masks per pass, as Morph carries Scene Shadow) was
    looked at and not built: not asked for, and the picture blurred would have to be the
    morph's own. Tested: cross-fade logged while blurring, morphed when off or blurring nothing.
  - **Filter order**: anything enabled before it that draws, other than a Scene Shadow, makes it
    stand aside (logged once). It keeps itself after every Scene Shadow (`keep_after_shadow`,
    from the tick twice a second, on or off, once `scenes_loaded()`, as Standby keeps itself after
    an Auto Frame), so either add order works; and Scene Shadow passes over a Scene Blur before
    it (`kSceneBlurId` in its `look_again`) for that half second instead of standing aside.
    Tested both ways (`... first: Scene Blur is kept after Scene Shadow and both draw`).
  - **Vendor**: `ListSceneBlurs` and `SceneBlur` (`scene_blur_list`, `scene_blur_press`; a toggle
    over several is all off when any is on, else all on), and `SceneBlurChanged` from the tick
    when `ramp_.active()` changes (not at the first tick), through `emit_event` so the tests see
    it. Both requests are in `stagekit_vendor_call` for the tests.
  - **Stream Deck key** *Scene Blur* (`.sceneblur`, `src/actions/scene-blur.ts`,
    `src/scene-blurs.ts`): features.json's new `scenes` list (make-plugin-files.py writes its
    page from it: *Blur filter* `getSceneBlurs`, *Scene* `getSceneBlurScenes` refreshed when the
    filter changes, *On press*), *Blur style* and *Scene type* lists as Blur All (`LOOK_KEYS`,
    `sceneKeys`; `sceneBlurShown`: the scene type with the blur style's badge, else `sceneBlurPicture`),
    `sceneBlurTitle`, `showOnOff`, `remembered` with the filter's look as `type`. Plugin 0.21.0.0
    (beta161). The OBS side reports `style` (`blur_style_name`), an entry of key-types.json's
    Blur style list (fourteen: the blurs, `trail`, the mosaic shapes, `glass` for any mosaic with
    glass), which is what the key shows on *Automatic*; keep the two in step.
  - **Blur area** (`blur_area`, `Area`, by number, append-only: 0 *The scene as drawn*, as it
    comes and as every filter saved before reads, 1 *Each source by itself*). The owner: "i want a
    configuration option to keep scene filters within the bounds of a source, or overlay the
    whole source. see the difference between the local source blur filter and the scene blur
    filter with the same settings. the scene blur filter doesn't seem to go all the way to the
    edges". Blurring the finished scene, the cells along a source's edge took in the background
    round it, and the mosaic's grid is the canvas's. With *Each source by itself*
    (`render_each`): each covered item's picture (`LookPaint::item_picture`: its source, crop and
    running show or hide transition, at the source's size) is blurred by itself with
    `BlurPipeline::blur_into` (the Blur filter's own run and composite, so origin, rotation, zoom
    centre and strength are the source's, as on the source) once a frame (`cache_` by item id,
    `blurred_item`; a trail of its own per item, `BlurTrail`, moved on by the video frame's time;
    let go 2 s after last use, and all of them when the blur is off or draws the scene as a whole).
    Then the covered items' boxes are drawn (`tr_boxes_`), the scene is drawn again from its items
    (`tr_redrawn_`: a covered item as its blurred picture, `LookPaint::draw_picture`, the rest
    with `draw_item`), the frame's Scene Shadow is laid over that again
    (`scene_shadow_lay_over`: the same cast and mask, `compose` into its own `tr_over_`), and that
    goes over the scene inside the boxes only (`composite_masked`, mix 1). Outside the boxes the
    scene is OBS's own; inside, the redraw does not follow a crop to bounds, scale filters or
    blending modes (as `tr_under_` before it). Tested equal to a Blur filter on the source: 0.00
    off in the box and along the edge for hexagons, Gaussian and whole cells. A crop is the edge:
    the per-source filter blurs the whole source and OBS crops it after, so at a crop the two
    differ. Whole cells keep the screen's edge (open sides, see the mosaic entry).
  - **Mosaic edge only with Each source by itself** (beta167): `update` clears
    `look_.whole_cells` for *The scene as drawn*, where the control is hidden. The owner: "If
    whole cells is selected then the mode is switched back to whole scene as drawn, the whole
    scene edge retains the border/blur": the setting was still read and cut the cells along
    the screen's edge.
  - **The scene as drawn is kept to the mask** (beta167). The owner: "scene as draw, causes a
    visual gap between the shadow, outline/glow effect": a mosaic cell or blur tap along a
    covered source's edge averaged in the background round it (40.7 off along the top edge in
    the test), a light band between the source and its outline. Now the picture blurred is
    `keep_to_mask` (`KeepToMask`: its alpha times the mask) and every pass averages colour by
    alpha, so only covered pixels count (exactly, through the separable Gaussian and Box too);
    `composite_masked` with `keep_alpha` takes the blur's colour and the scene's own alpha. A
    mosaic cell that only just reaches in looks further in (the mosaic entry). Tested: a Gaussian
    along the edge 0.23 off the Blur filter on the source (25.6 before); the hexagon mosaic's edge cells have no
    background blue in them (most blue 194, the source's own at most 200; 255 before). With *All
    sources* the background is covered too and nothing changes there: separating sources inside
    the covered area is *Each source by itself*'s job. No existing scene-blur expectation relied
    on the bleed.
  - **Scene Shadow takes the blurred shape** of a source blurred by itself: in its render it looks
    for an enabled Scene Blur on the scene and draws each casting or masked item through
    `scene_blur_item_picture` (the same picture, made once a frame by whichever asks first;
    Scene Shadow draws first, inside Scene Blur's `draw_filter_input`), else as before. So the
    shadow, glow and outline follow whole cells, and a soft blur's soft edge, and stay sharp
    over the blurred source (Scene Blur lays the frame's shadow over its redraw). With *The scene
    as drawn* nothing changes: shadows are blurred with what they fall on. Morph never carries
    this: it cross-fades while a Scene Blur blurs.
  - Tests: `[blur edges: each source by itself, and whole cells]` (`blur_edges_test`, a
    stand-in kind `stagekit_test_photo`; alone with `STAGEKIT_TEST_ONLY_BLUR_EDGES`, which also
    runs the controls test; contact sheet `docs/images/blur-edges.png` from `$out/blur-edges`):
    the match above, nothing of the background in the top cells, drawn twice alike, whole cells
    for every shape and a turned pattern, an outline after a Blur filter and a Scene Shadow's
    outline jagged with whole cells (inside the box too) and straight without, Temporal and
    Kawase each source by itself; and since beta167 the scene as drawn kept to the source (mosaic
    and Gaussian), the screen's edge kept with whole cells (a full-screen background and a source
    running off the left) and a Mosaic edge left on Whole cells ignored by the scene as drawn.
    `scene_blur_controls_test` checks *Blur area* and *Mosaic edge*'s visibility and defaults.
  - Tests: `[scene blur]` in `tests/ambient-test.cpp` (stand-in captures of kind `xshm_input`
    drawing a checkerboard, sharpness measured as the mean step between neighbouring pixels),
    `streamdeck/tests/obs.test.ts` (stand-in server). Not run on Windows or Direct3D; the key not
    on a real Stream Deck.
<!-- ==== end scene-filters:blur ==== -->
<!-- ==== scene-filters:perf ==== -->
- **Frame time** (`src/common/timing.*`, `src/common/scene-rev.*`; the harness `tests/collection-perf.cpp`,
  `tests/make-perf-collection.py`, `tests/run-perf.sh`, `tests/perf-compare.py`). The owner's
  *Average time to render frame* went from under 1 ms to 1.1 ms: all of it `tick_sources` (0.25 to
  0.45 ms median), none of it drawing. What was established:
  - **Measure before claiming a saving, with the harness, and give its numbers.** `run-perf.sh
    <build dir>` writes the synthetic collection (18 scenes, 36 Fit to Space, 23 shadows, 14 Show in
    Scenes, 10 Link Visibility, 4 Standby: the shape of the owner's, nobody's sources) **on a quarter
    canvas** (`--scale 0.25`), shows Live, Chat Live, Standby and then nothing (`(off)`), and prints
    per kind: instances, µs a frame, µs a frame without ticks of 1 ms or more, and per call mean,
    p50, p95 and max. At 2560x1440 llvmpipe draws a frame in 0.3 s, and every timer (4 Hz, 10 Hz)
    then fires at every tick: the first baseline said Ambient Background cost 40 µs a tick, which
    was its 10 Hz look running every frame. Fit to Space's read-backs wait for llvmpipe (the ticks
    of 1 ms or more); compare the "without" column. `PLUGIN=` measures another build the same way.
    The frontend's "Tried to call ... with no callbacks" lines are dropped before formatting, or
    every `scene_on_screen()` cost a formatted log line OBS does not have.
  - **`stagekit_timing_take(out string json)`** gives per kind the calls, µs, p50 and p95 per call
    (`Spread`: eight buckets per doubling), the longest, the ticks of 1 ms or more apart
    (`tick_long_us`), and StageKit's ticks per frame (p50, p95, max: `frame_add`, keyed by
    `obs_get_video_frame_time`). The 30 s log lines are ms with three decimals, plus one line for all
    of StageKit's ticks a frame when over 0.1 ms. `stagekit_test_looks` counts the periodic looks
    below that were not skipped, and every scene signal by name (what keeps changing at rest).
  - **`scene_revision()`** goes up on every `item_add`, `item_remove`, `item_visible`,
    `item_transform`, `reorder` and `refresh` of every scene and group (connected at the first call
    and at `source_create`; the signal only counts). A periodic look that depends only on what is in
    the scenes compares it and is skipped when nothing changed, with a slow look anyway:
    Show in Scenes' *Follow what is shown* (still on its quarter second, skipped unless the revision,
    a press, its settings or loading changed, or the last look found a mode settling; a look every
    2 s anyway), Link Visibility's *Follow sources* (every tick as before when one of its
    items or leads changed or no state is followed yet, else every fifteenth tick), a shadow's scale check
    (every 20 ticks only when the revision, the scene on screen, the source's size or its settings
    changed, else every 120) and its *Keep it on screen* (also while an item has not been still for
    0.4 s yet), and Ambient Background's sources (10 Hz only on a change of revision, scene on
    screen or settings, else 2 Hz). **Anything that changes what those go by without a scene signal
    must call `scene_revision_bump()`**: a Link Visibility that takes another state or is switched
    off, a Show in Scenes switch turned (`enabled_cb`) do. A size change of a source in a scene that
    is not drawn sends no signal (OBS signals it in `scene_video_render`), so a look that depends on
    sizes compares the size itself.
  - **`refresh_item` is quiet** (`scene_signals_quiet`): setting an item's position to itself made
    OBS signal `item_transform` at every measure, twice a second in a shown layout scene, and every
    look above ran as if something had moved. It counts a change only when the box did change.
  - **A shadow's scale is not taken when it goes straight back** (`prev_sx_`): in a bounding box the
    scale read depends on the room the shadow takes, and the room on the scale. Rounded to pixels
    the two took turns between two values for good (34 and 35 px on the synthetic captures; also in
    beta162), moving every item showing the source by a pixel and back three times a second. A
    scale that returns to the one before, by under 2 %, is that cycle and is kept as it is.
  - `shown_scale` measures only the item wanted and groups and nested scenes on the way: a source's
    size goes through all its filters, and measuring every item cost 26 µs a look.
  - Settings a tick needs are kept behind a `shared_ptr<const ...>` made in `update` (Link
    Visibility's `Config`), never copied each tick; Standby copies its image's strings only when it
    makes the image again.
  - **Periodic jobs start at `stagger_phase(source, period)`** (the source's uuid), so a dozen
    filters with one period do not all work in one frame (p95). Not done: Fit to Space's own ticks
    (its idle and glance rules above stand), Standby's 4 Hz stand-in look (a Scene Standby's list is
    not a scene signal), the window switchers.
  - Before and after on the synthetic collection (µs a frame of StageKit's ticks, without those of
    1 ms or more): Live 169.5 to 98.6, Chat Live 163.3 to 106.8, Standby 137.3 to 98.4, nothing on
    screen 138.4 to 77.0. Link Visibility 2.9 to 0.7 µs a tick, Shadow 2.2 to 0.9, Ambient
    Background 13 to 3.5, Show in Scenes 0.6 to 0.3. Fit to Space, untouched, is now the largest.
<!-- ==== end scene-filters:perf ==== -->

## Conventions

- Release notes (`docs/releases/<version>.md`): line 1 `# StageKit <version>`, line 3 **a one-line
  summary** (required: the dock's Update list shows it), then `## New`, `## Changed`, `## Fixed`,
  `## Good to know`, each bullet starting `- **Short bold lead.**`. The updater lists the bold leads
  of the first three.
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
- **Several AI sessions may work and release at the same time** (the owner, 2026-11-17: "i ... don't
  mind having multiple working in tandem if it can allow faster work and not cause issues"). Name
  release notes `0.1.0-betaNEXT.md` while working; number them only just before pushing, from
  `git fetch origin` and the highest `docs/releases/` on `origin/main` plus one, and check again
  right before the merge. If another session took the number, rebase, renumber and keep both
  handoff sections. Never merge a branch whose notes are not the newest number on `main`, and
  never force-push or rewrite another session's branch.

## Talking to the owner

Routine progress is plain, short text. Two markers are reserved and go at the top of a message in bold:

- `⚠️ ACTION NEEDED:` when the owner must decide, approve, supply something, or check something on
  their own machine. Say exactly what to do, where, what to report back and what is blocked. One
  request per line. Repeat an unanswered request in the next update.
- `🚀 PUBLISHED:` when a release is live and its assets are verified, followed by the link and a
  one-line summary.

Use them nowhere else. End every message with a short status: what is done, what is still running, and
what the owner needs to do, if anything.
