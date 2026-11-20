# StageKit development handoff

Read [AGENTS.md](AGENTS.md) first. This file is the current state.

## Where things stand

| | |
| --- | --- |
| Version | 0.1.0 (in `buildspec.json`). First pre-release is `0.1.0-beta1` |
| Repository | https://github.com/TechSavage1989/StageKit (public; visibility verified, not changed) |
| Default branch | `main`, which holds all work so far (pull request #1, merged 2026-11-12) |
| Working branch | `chore/review-stage6`. Release notes in `docs/releases/0.1.0-beta231.md`. |
| Releases / update feed | GitHub releases on this repository, which the plugin's updater reads (`src/update/`) |
| Built against | OBS 31.1.1 (`buildspec.json`). The owner runs OBS 32.2.2 on Windows |
| Gate | Checks passed |
| --- | --- |
| `MORPH_UI_TASK` (new; five runs) | 27 of 27 each time |
| `MORPH` (*[morph between items]*, the new sections in the full flow, and what comes before) | 246 |
| `MORPH_HOLDS`, `MAKE_ROOM`, `POP_IN` | 5, 15, 4 |
| `LINK_FOLLOW`, `LINK_TRANS`, `SWITCH_COMMAND` | 51, 14, 3 |
| `CAPTURE_HIDE`, `CAPTURE_CHAIN`, `RESTORE_HIDDEN`, `STANDBY_COVER` | 5, 11, 5, 7 |
| `LAYOUT`, `MODES_HIDE`, `SCENE_TRANS_OVERRIDE` | 40, 29, 9 |
| `SHADOW_FLICKER`, `SHADOW_EACH`, `TOOLTIPS` | 32, 13, 12 |
| The other nineteen gates (the test program's waiting changed for all of them) | all passed |

## Scene controls and collection compatibility (0.1.0-beta231)

Stage 6 covers the new and historical switch defaults, Scene Corners coverage, the full show and hide
controls on the capture tools, source and scene renames, Morph rule editing, Scene Shadow and manual
source commands, and Apply Layout framing. Move Scene Item remains registered for saved filters and keys.
The Fit defaults and behavior in stage 7 await the owner's separate Fit work.

The review, restore-hidden, mode placement, switch command, automatic frame and source switching,
capture hide, scene transition override, layout and Morph tests passed on Linux. Memcheck reported
zero invalid memory accesses when the new controls rebuilt their settings.

## Guide arranged by job (0.1.0-beta230)

The guide begins with six comparison tables. Source and scene filters are grouped by job.
The holding scene recipe starts with Scene Modes and retains the Link Visibility version
produced by scripts/merge-holding-scenes.py. The repository checks compare the first column
of the guide's Setting tables with the English labels. All 54 rows passed, and all 29
links in the comparison tables resolve to guide headings.

## Stream Deck review (0.1.0-beta229)

The Stream Deck plugin is version 0.34. Its 38 property inspectors share an Image group.
Every existing UUID, target separator, persisted control ID and saved picture remains readable.
Match Item and Scene Transitions have their own keys. A manual switcher hotkey is registered on the
parent in OBS, so the key includes that exact source or scene as the WebSocket context.
The optional scene-transition restore is tested headless and awaits a visual check in OBS.
All 93 Stream Deck tests passed. Elgato's validator accepted the plugin and the package built.

## Twelve material and data patterns (0.1.0-beta228)

Ambient pattern values 25 through 36 follow the owner's remaining references in order. The
separate shader file protects earlier looks when a renderer cannot load it. Nine textures
have outline designs 35 through 43. The Ambient Background fill keeps its value 34.
Code wall, Network web and HUD data wall use the full background to show their small details.
The solder-pad circuit board is a separate option so a saved circuit board keeps its look.
An isolated Windows OBS 32.2.2 Direct3D 11 renderer checked both palettes, motion and the
240-second loop endpoints for all twelve backgrounds. The Ubuntu OpenGL tests passed for
HEX_PATTERNS, OUTLINE_AMBIENT, OUTLINE_PATTERNS and PATTERN_TRANSFORM. The two animated
preview sheets in the guide show frames captured from the Windows renderer.

## Dock and filter controls (0.1.0-beta227)

The approved third review stage covers the dock decisions D16 to D22, source hotkey renames,
capture choices, timing, ratio presets and source-specific status. The dock's ordered selection,
copy destination, placement duration and position keys preserve existing collections. The Stream
Deck's matching ratio list belongs to stage 4. Fit behavior and migration belong to stage 7.

- Scene Transitions lists scene sources in *Settings for*. A saved per-source transition applies
  in All sources even when its name is absent from the Chosen sources list. Link Visibility and
  Show in Scenes continue to use their configured lists. The Scene Transitions test exercises
  an unlisted source and its unaffected neighbour.
- Scene Fit reports the same decisions its existing source selection makes. Apply Layout compares
  saved source names with the active list. Scene Shadow and Scene Blur report the blocking filter or HDR.
- Preserve integer IDs for ratio and visual transition choices. Auto Capture's default scene-item
  hide keeps the item's own transition. A saved Scene Shadow with only one time uses that for both
  ramp directions. A saved Morph wildcard keeps the literal value it had in English.
- Targeted render checks cover capture hiding, Standby, dock bounds, tooltips, layouts,
  Auto Frame and Scene Transitions. All seven targeted groups passed on Linux OpenGL. The Scene
  Transitions property rebuild completed under Valgrind with zero memory errors.

## Outlines filled with an Ambient Background (0.1.0-beta226)

The owner: "id also like there to be a border/shadow/outline option to follow the ambient
background setting for pattern, but also background image or live, including mosaic and blur
configurations".

- **Outline design** 34, *Ambient Background*, in Shadow, Glow & Outline and Scene Shadow. New
  settings `outline_ambient` (*Background*: "" Automatic, or a name), `outline_ambient_bright`
  (*Fill brightness*, 130 %), `outline_ambient_sat` (*Fill saturation*, 120 %); the pattern
  fills' Scale, Position X/Y and Rotation lay it out. Filters saved before are unchanged
  (tested).
- **The picture** is the background's own last finished frame (`src/ambient/ambient-frame.*`, a
  registry by number, weak references, published at the end of `AmbientBackground::render`);
  a background not drawn in two frames is drawn once into a scratch texrender. Laid out and
  lifted by `data/effects/stagekit-outline-ambient.effect` (mirror addressing; all uniforms set
  every run); without it the line is its outline colour and the log says so once.
- **Mapping**: Scene Shadow canvas-aligned; Shadow, Glow & Outline the background's middle on the
  source's middle at the canvas's scale (the filter does not know the item's place on screen).
- **Lookup only in ticks.** The first build looked the background up while drawing and hung the
  test program: `scene_on_screen()` without the frontend asks `obs_get_output_source`, whose lock
  the main view holds while drawing. `look_tick` and `outline_ambient_warm` (each filter's tick)
  do it now; drawing only reads the answer kept.
- **Not done**: the drop shadow and glow in the background's colours (a shadow in the source's
  own colours exists; this would be another texture through both shadow passes).
- Verified on Ubuntu with OBS 30.0.2 and software OpenGL: `OUTLINE_AMBIENT`, `OUTLINE_BLEED`
  (including the one-column and two-column controls), `OUTLINE_GLOW`, `OUTLINE_PATTERNS`,
  `OUTLINE_DESIGNS` and `HEX_PATTERNS` all passed. The rendered preview is
  [outline-ambient.png](docs/images/outline-ambient.png). The full suite was not run.
- The OpenGL neon gate reproduced a texture binding failure with the first integrated build.
  Ring and Follow had separate shader uniforms for the same texture. Both now use `ring_shape`,
  leaving room for the caster sample that protects the canvas edge. The beta224 comparison
  and the corrected build both passed the neon gate on the same runner image.
- The new fill effect compiled in OBS 32.2.2 on Windows with Direct3D 11. All six beta221
  patterns also produced distinct nonflat frames and animated on that renderer.

## A one-column Scene Shadow outline (0.1.0-beta225)

A Windows report showed a 6 px Nebula outline with Scene Shadow set to *Each source by itself*.
The supplied DSLR transform has a 3665.33 × 2062.67 image in 2560 × 1440 Fit bounds, with zero crop
and rotation. Fitting that ratio to the height leaves about 1.14 px of horizontal space in total.
The previous edge test skipped uncovered pixel centres. It now checks a one-column gap and a
positive two-column control, including Nebula and both Scene Shadow stacking modes.

The shader looks at the caster one pixel inward only at the canvas perimeter. It preserves the
scene underneath and only gates the outer ring there. Source sizing and fitted bounds use their
existing paths.

An isolated Windows OBS 32.2.2 Direct3D 11 render reproduced the report with a synthetic source,
zero crop, 4/3 scale, 2560 × 1440 Fit bounds and the 6 px Nebula outline. On beta224 the last column
contained the outline. With the updated shader it matched the black background, and widening the gap
to two columns restored the outline. This test did not load the owner's scene collection.

## An outline at the screen's edge (0.1.0-beta224)

Branch `fix/outline-edge-bleed`, from `main` at beta220. Not pushed. The owner (Windows, OBS 32,
2560x1440): "the border seems to bleed a pixel or two on the right and/or left sides of some
sources", a capture card filling the screen with a line showing along the right edge.

- **Cause, reproduced**: Shadow, Glow & Outline's last draw used OBS's default `Draw`, which
  samples the filter's picture (source plus room, line in the room) bilinearly. Drawn larger
  (480x270 fitted to 640x360 by Fit to Space or Scene Fit, as 1920x1080 on 2560x1440), the
  screen's outermost pixel sampled 1/8 of the line's texel: (99, 240, 0) instead of green, on all
  four edges, for every width 1 to 12, with or without shadow; with odd sizes (481x271, 853x480,
  333x187) 128 to 245 of 255 and unequal between sides, as the owner saw. OBS's bounding box alone
  never put the picture at the edge (the room is inside the box).
- **Fix**: technique `Show` (`PSShow`) in `stagekit-shadow.effect`: inside the source's (or
  panel's) box, the read is kept half a pixel inside the box; elsewhere as before. At scale 1 the
  same pixels.
- **Scene Shadow**: no fault. A line shows only in a pixel row or column whose middle the item
  does not cover (0.6 px short); the item 0.4 px off, or 1.5 px past, shows none. Plain, square,
  rounded, pattern, both *Shadow from*.
- **Not covered**: an item OBS draws through its own texture (crop, scale filter, blending mode)
  samples the same way, outside the filter's reach. Not seen on Windows / Direct3D.
- Gates: `OUTLINE_BLEED` (new), `OUTLINE_GLOW`, `OUTLINE_DESIGNS`, `OUTLINE_PATTERNS`,
  `PATTERN_TRANSFORM`, `SHADOW_EACH`, `SHADOW_FLICKER`, `BLUR_EDGES`. Not the full suite.

## Review stage 2: tidier windows (0.1.0-beta223)

Branch `chore/review-stage2`, on `main` at beta222. Committed locally, not pushed. Stage 2 of the
approved whole-plugin review: the order, grouping and visibility of controls in the windows, plus
decision D11 (one line of help per filter window). No setting key, id, default or saved number
changed; groups added hold no setting. Mechanics are in AGENTS.md's *Names* (the stage 2 paragraph).

**Done:** C10 (StageKit's own transition lists in one order: Standby Image, Scene Standby, Auto
Capture, the Window Switcher; Ambient's capture change already was), C12 (`add_on_group_warning`),
C14, C18, C20, C21, C23, C24, C27, C28, C31, C32, C36, C39, C40 (with the open windows' titles),
C41, C42, C44, C48, C49 (window part only), C51, C54, C61, C62, C63, C64's *Unpaired transition*
line, C66, C68, C69's blur-type order, C72, C75, C76, C78, and D11. Also: tooltips for the
controls of Auto Frame, Auto Capture, Program Trigger, Shadow, Glow & Outline (and Scene Shadow)
and Corners & Edges (and Scene Corners) that had none; *Make room if not filling*'s tooltip (Fit
to Space and Scene Fit) says the filling rules are hidden while it is unticked; Link Visibility's
*Lock hidden sources* moved above the transition group; the unused `Modes.AddTemplate` removed.

**Left for later stages:** C5's *Move duration* / *5. Timing* and C6's easing tooltips (stage 3);
every remaining control without a tooltip outside the windows regrouped here (Blur's look, Layout,
Cropped View's four sides, the easing lists: C13's rest); Morph used by an item is told by looking
through the scenes' items (`used_by_item`), and a Morph not yet set on any item shows everything.
Nothing of Fit to Space or Scene Fit behaviour changed (stage 7); C48/C49 there are the window only.

**Tested** (gates, after rebasing onto beta222): `TOOLTIPS` 12 (34 kinds, 457 tooltips),
`SCENE_BLUR_CONTROLS` 3, `BLUR_EDGES` 19, `LINK_TRANS` 14, `LINK_FOLLOW` 51, `SCENE_TRANS_OVERRIDE`
9, `SHADOW_EACH` 13, `CORNERS_WHOLE` 11, `FIT_GROUPS` 15, `FIT_PRIORITY` 41, `FIT_WHOLE_OVERLAP`
36, `FIT_VISIBLE` 16, `MODES_HIDE` 29, `CAPTURE_HIDE` 5, `CLEAR_PICKED` 7, `STANDBY_COVER` 7,
`RESTORE_HIDDEN` 5, `SWITCH_COMMAND` 3, `MAKE_ROOM` 15, `MORPH_UI_TASK` 27, `DOCK_BOUNDS` 23,
`AMBIENT_DARKEST` 16: all passed. `FIT_TOOL` failed once (*with shadows counted (by boxes) ...
stay inside the gap*, while other builds shared the machine) and passed all 78 on a second run.
The Scene Modes section that checks the one *Add mode* row and the dock's Standby & Ambient tab run
only in the full suite, which was not run (the job asked for gates only). Not seen in OBS on
Windows.

## Make room only when the image would overlap (0.1.0-beta222)

Branch `feat/make-room-only-overlap`, on `main` at beta220. Committed locally, not pushed. The
owner: "for the make room if not filling, add an option to only reposition/make room if the
visible pixels would actually overlap the Make room for source otherwise."

- New setting `whole_room_only_hit` (*Only if it would overlap*, off as it comes, so a saved
  filter fits as in beta218), under *Make room if not filling* in Fit to Space, in each Scene Fit
  entry and in the shared set (`kFitKeys`); shown only while that box is shown and ticked.
- Decided in `space_among`'s `in.whole` branch, after `fills`: the picture as the fit area would
  place it (`would`) against each box room is made for, plain overlap of more than half a pixel
  each way, no gap. The boxes are the scan's obstacles as already measured (visible pixels and
  shadows by the fit's own two options), so there is no new drawing or read-back. A box the
  area's middle lies inside is passed over, as `free_space` passes it over. No overlap: it is
  `covering`, exactly as when it fills. See the AGENTS.md entry.
- Tests: `[fit whole screen: making room only when the image would overlap]`
  (`fit_room_only_hit_test`, under `STAGEKIT_TEST_ONLY_FIT_WHOLE_OVERLAP`, Scene Fit entry and
  Fit to Space): saved without the key, camera in the bar ticked and unticked, camera under the
  image's edge, 3 s without a move or a glide in both places, nothing listed with a background,
  the window. 36 checks with beta218's 22. Gates passed: FIT_WHOLE_OVERLAP, FIT_ALIGN,
  FIT_ENABLE, FIT_GROUPS, FIT_KEEP_SIZE, FIT_OWNER, FIT_PRIORITY, FIT_RENAME, FIT_TOOL,
  FIT_VISIBLE (one run, no fault at shutdown), TOOLTIPS. The full suite was not run. Not seen
  on Windows.
- Not tested: a *Make room for* source that is see-through in part (the overlap goes by the
  box of what shows, not pixel by pixel), a box set as the fit area, a source kept at its size.

## Six hexagon and dot patterns (0.1.0-beta221)

The owner's "pattern batch 2", part 1, from reference looks (style only).

- **Ambient Background** `pattern` 19 to 24: Hex columns, Mesh, Halftone hexagons, Dot grid, Hex
  wire glow, Hex mosaic. All in a new file, `data/effects/stagekit-patterns-hex.effect`
  (`pattern_file` 3); the three older files are untouched. Same uniforms, all set at every run by
  both users.
- **Outline designs** 29 to 33 (Shadow, Glow & Outline and Scene Shadow): Mesh, Halftone, Dot
  grid, Hex wire glow, Hex mosaic, through `outline-pattern.cpp`'s fourth file. Hex columns is
  not offered there (a view, not a texture).
- **Colour 1 is the ground in all six.** Two references described Colour 1 as a light's colour
  (Mesh: "colour 1 to colour 2 to dark"; Dot grid: "colour 1 one side, colour 3 the other"); here
  the light is Colour 2 to Colour 3 so that a light Colour 1 works as a light ground, as in the
  circuit patterns. Say so if the owner asks for the reference's order.
- Tested in `STAGEKIT_TEST_ONLY_HEX_PATTERNS` (79 checks; its own run in `run-tests.sh`, sheet
  `docs/images/hex-patterns.png`). Also run: `AMBIENT_PATTERNS`, `OUTLINE_PATTERNS`,
  `OUTLINE_DESIGNS`, `PATTERN_TRANSFORM`, `TOOLTIPS` (the two outline tests now expect 34
  designs). **Not the full suite.** The original release was tested on OpenGL. A subsequent check on Windows OBS 32.2.2 rendered and animated all six on Direct3D 11.
- Cost on llvmpipe at 1280x720 (drawn at half size), a frame: Mesh 20 ms, Hex mosaic 23, Halftone
  and Hex wire glow 25, Hex columns 34 (six cells looked at per pixel), Dot grid 34 to 40
  (fourteen dots looked at per pixel for the stars). Not measured on a graphics card.
- Not done: no Stream Deck wording was touched; `docs/images/ambient-patterns.png` still shows
  the first nineteen (the README has the new sheet beside it).

## Position X, Position Y and Rotation (0.1.0-beta220)

The owner: "the ambient background image/animations and shadow outlines/animations should have
similar transform controls as the blur/mosaic feature with x, y, and rotation controls."

- **Ambient Background** (Image group, every base): `base_pos_x`, `base_pos_y` (% of the screen,
  -100 to 100), `base_rotation` (degrees, -180 to 180). Image, standby image, source: turned and
  moved in `PSLayer` before everything else; the wrapping (mirror or repeat) fills the corners;
  drift stays across the screen. Pattern: the pattern's own frame is moved and turned in the
  patterns' vertex shader (`src/common/pattern-frame.hpp`).
- **Outlines** (Shadow, Glow & Outline and Scene Shadow, one code path): `outline_pos_x`,
  `outline_pos_y`, `outline_rotation`. All three for the pattern fills (designs 12 to 28);
  *Rotation* alone for Dashed, Marching, Gradient, Travelling, Shimmer and Rainbow (where round
  the border they begin); none for Solid, Double, Dotted, Neon, Inner, Pulse. Morph glides them.
- Defaults 0; at 0 a render is identical to one with the keys absent (tested). The two outline
  effect files changed by two expressions each (`design_phase` and `design_turn` are no longer
  gated by design in the shader; the CPU already sent 0 for the designs that do not move).
- Not done: Dotted has no offset (its dots are laid out without a phase); the lerp in Morph is
  not exercised by a test (the test program cannot call into the plugin's `lerp_design`).
- Found on the way, not touched: `Shadow.OutlineDesign.Tip` is in `data/locale/en-US.ini` twice
  on `main` (lines 233 and 237), which `tests/run-tests.sh` stops on.
- Gates run: `PATTERN_TRANSFORM` (31 checks), `AMBIENT_PATTERNS`, `OUTLINE_PATTERNS`,
  `OUTLINE_DESIGNS`, `AMBIENT_SEAMS`, `BLUR_OFF_DRAW`, `TOOLTIPS`. Not the full suite. Only OpenGL.

## Neon is a tube with a halo (0.1.0-beta219)

Branch `feat/neon-glow`, rebased on `main` at beta217. Not pushed. The owner on beta212's Neon:
"neon just looks white", and said "go" to a proper tube. Now the line is whole, in the outline
colour and brighter along its middle; a halo of light spreads past it (*Glow size*,
`outline_neon`, now per cent of the width beyond the line, 150 as it comes, to 400); a very dark
colour has no halo unless the *Second colour* (shown for Neon now) is brighter. One more blur,
only for Neon; every other design is unchanged (designs 0 to 4 and 6 to 11 at speed 0 compared
file by file with the build before, in both filters: the same). How it is built is in AGENTS.md
under *Outline designs*. Gates on the rebased tree: `OUTLINE_DESIGNS` (new checks: halo lit and
falling off within its reach, tube brighter and still cyan, a black line a dim rim, the second
colour's halo, the image unmoved from Solid to Neon with 40 px more room), `OUTLINE_GLOW`,
`OUTLINE_PATTERNS`, `SHADOW_EACH`, `MORPH_HOLDS`, `TOOLTIPS`. The full suite was not run. Not
tested: Neon through a Morph (it goes through the same `compose`), text, the pattern-filled
designs bit for bit (their pictures move with the clock). Two stale expectations were put
right on the way (29 designs, not 27, since beta216). Only OpenGL. Contact sheet
`docs/images/outline-designs.png` regenerated (Neon on dark). Release notes in
`docs/releases/0.1.0-beta219.md`, to be numbered just before pushing.

## The whole screen makes room when the image does not fill (0.1.0-beta218)

Branch `feat/whole-screen-overlap`, on `main` at beta215. Not pushed. The owner (a 4:3 standby
image on *The whole screen*, centred, over a sliver of the square camera in the corner): "id like
those settings to exist for whole screen mode too, that way if a standby image or other odd
resolution item doesn't fill the screen to a certain amount, it doesn't awkwardly cover a sliver
of the camera square."

- New per-source setting `whole_make_room` (*Make room if not filling*, off as it comes), shown
  for *The whole screen* and *A box I set* in Fit to Space and in each Scene Fit entry (and the
  shared set of *Same for all sources*; it is one of `kFitKeys`). It needs *Overlap only when it
  fills* (`skip_fill_on`; a Scene Fit's is filter-wide) to do anything: the owner already has that
  ticked with entries on the whole screen, so the switch alone must not change a saved layout.
- Ticked, the fit runs the free space's path (glide, claims, visible pixels, tolerance); whether
  it takes the whole screen or the room beside the other sources is decided in `space_among`
  (`in.whole`) from the shape and the settings only. See the AGENTS.md entry for the rules.
- The list (`skip`) is shown as *Make room for* there: the sources room is made for, all of them
  when it is empty. Chosen over "never in the way": a camera listed from habit (as under the free
  space, where listed sources are the ones overlapped only while it fills) then works, and so
  does an empty list.
- The dock's *Fit to space* on such a source: a resized one is refitted by its own fit
  (`fit_item_refit`, unchanged, which now glides it to where the rule puts it); one kept at its
  size is still left to its fit, as for any whole-screen fit. Nothing was changed in the dock.
- Scene Fit's *Overlap only when it fills* tooltip now says where each source's rules are and
  for which Fit areas they show.
- Differences from a plain whole-screen fit while the box is ticked: it is not fitted while its
  scene is out of view (put right without a glide on coming into view), and a new shape is taken
  after the usual half second, with a glide.
- Tests: `STAGEKIT_TEST_ONLY_FIT_WHOLE_OVERLAP` (22 checks). Gates passed: FIT_ENABLE,
  FIT_KEEP_SIZE, FIT_PRIORITY, FIT_GROUPS, FIT_TOOL, FIT_ALIGN, FIT_VISIBLE, DOCK_BOUNDS, LAYOUT,
  TOOLTIPS. FIT_VISIBLE passed its 16 checks in every run, but one run of five ended in a
  segmentation fault at shutdown, after the last scene was destroyed; **cause not looked for**
  (the machine was shared with another build at the time), so whether it is new is not known.
  The full suite was not run (the owner asked for efficiency). Not seen on Windows.

## Review stage 1: words only (0.1.0-beta217)

Stage 1 of the approved whole-plugin review (the proposal and the owner's decisions, 2026-11-19:
"make it so with all your recommendations"). Wording only: values in `data/locale/en-US.ini`, the
README, and a few lines of C++ that give a control a tooltip or a label a key of its own. No setting
key, id, enum number, default or control order changed. The then-and-now list is the sixth table in
AGENTS.md's *Names*.

**Done:** C1 (item: source, with Match Item's labels from C54 and Show in Scenes' *Move's linked
sources too* from C39, both pure renames), C2 (hotkey descriptions), C4 (list wording: values only),
C7, C8, C9, C11, C13 (in part, below), C15, C22, C25, C29, C33, C34, C35, C37, C38, C43, C46, C50, C53,
C56, C57, C58, C59, C60, C64, C65, C69 (names only), C70, C77, C79, C80 (menu wording), C81 (wording),
C97, C98 (the labels that changed here, and the hotkey counts), C101, and decisions D5 (A), D8, D9
and D12 b.

**C++ touched, and why:** new locale keys for the fuller *Show/Hide transition*'s two durations
(`Change.ShowTime`, `Change.HideTime`, `Change.Duration.Tip`) and the Window Switcher's
`AppCapture.ChangeTime`; Morph's own entries in the unpaired list (`Morph.Alone.Fade/Zoom/Slide`);
the transition tooltips put *As the scene sets* first; tooltips set by control name at the end of
the Zoom, Cropped View, Show/Hide Animation and Move windows, on Blur's *Strength*, Auto Frame's
*Display shape* and the dock's five Effects buttons (`<key>.Tip`); "px" on the saved position; "%"
without a space in Fade and Zoom and Corners & Edges; and Scene Fit's shared settings use Fit to
Space's list titles (`shared` in `fit_add_controls`: titles only).

**Skipped as moot:** *Capture.Always/Never* "separated by commas" (they are lists now: the tooltips
were corrected instead); C101's `Capture.Lists` is in use and stays; of C101's 27 keys, 24 are deleted
and `Trigger.ActGroup`, `Frame.Rules.Tip` and `Arrange.CopyBox` are kept for stages 2 and 3.

**Left for later stages:**
- *Stream Deck (stage 4):* its share of C1, C37, C38 and C77 (the look name *Outlined edges*, the
  Position labels, tool names). Nothing under `streamdeck/` was touched, so its version is unchanged.
- *C4:* one shared `List.AddSource` key in place of the per-filter ones, and "(choose a …)" against
  "(none)" in every list (needs each call site looked at). *C102:* shared `Common.*` keys. Neither
  is visible.
- *C13:* only the worst tooltips were cut (Outline design, Outline corners, Link Visibility's mode,
  *Show only what it hid*, *Show when source is hidden*, Darkest level, the dock's slide). Still
  over 550 characters, among others: `Fit.Priority.Tip`, `Fit.Toward.Tip`, `SceneFit.Settings.Tip`,
  `SceneFit.TakeOver.Tip`, `SceneBlur.Area.Tip`, `Morph.Alone.Tip`, `SceneTrans.Cover.Tip`. Controls
  still without a tooltip in Auto Frame, Auto Capture, Program Trigger, Shadow and Corners & Edges
  go with their windows' regrouping in stage 2 (C18, C27, C36, C40).
- *C64:* the "Unpaired transition" line before the copied settings in a filter's window (a new
  control: stage 2).
- *C69:* the order of Ambient Background's blur types (order is stage 2). Its mosaic list keeps the
  name *Mosaic*, because its first entry is *None*; the slider is *Cell size*.
- *C80:* *Release page* on the update button on macOS and Linux, and `Update.Check.Title` (code).
  *C81:* the undo name "StageKit: Snap on release" (a new key in code).
- *C98, C99:* the README was brought in line with every label renamed here; the wider pass over
  its quoted labels, the corrected troubleshooting and scene-filter introduction, the Features list
  by job and the two standby sections go with the guide's reorganising (stage 5).
- Anything about how Fit to Space or Scene Fit behave: stage 7.

**Tested:** the tooltips gate (`STAGEKIT_TEST_ONLY_TOOLTIPS`: every kind's window, 34 kinds), the
Link Visibility sections that read labels and build the transition group, and the Scene Fit groups
section. The full suite was not run (the owner asked for a short job); the dock section's check of
the Tools menu text was updated to the new wording and has not run.

## The circuit patterns (0.1.0-beta216)

Branch `fix/circuit-board`, on `main` at beta215. Not pushed. The owner, of beta215: "i don't
really like the circuit board effect animation. it needs major improvement", then reference
pictures. Circuit board (`pattern` 10, outline design 21) is drawn anew: ribbons of parallel
traces bending together at 45°, ring pads where they end, thick traces, a dimmer board under,
lights along the ribbons, chips and soldered pads at a higher Detail. Two new patterns, appended:
17 Circuit traces and 18 Data lines (outline designs 27 and 28). All three are in a new file,
`data/effects/stagekit-patterns-circuit.effect`; Circuit board was taken out of
`stagekit-patterns-more.effect`. Details in AGENTS.md (*Ambient Background's patterns*).

- **Only run on OpenGL.** If the three show the standby image on Windows, the log line names the
  circuit file; the other sixteen patterns are in the two older files and unaffected.
- Previews for the owner (not committed): `tests/make-pattern-preview.sh <out.gif> "<pattern>
  <frames> <fps> <detail> <size> <colour1> <colour2> <colour3>" [still.png]`.
- Cost on llvmpipe against Nebula (a whole frame at 1280x720, the least of three runs): Circuit
  board 1.6 times, Circuit traces 1.35, Data lines 1.2.
- Not built from the references: serpentine (length-matching) runs, traces that grow, small
  rectangular loops, "∞" marks. Data lines in an outline is nearly the plain outline colour.
- A second batch of patterns from further references (Hex columns, Code wall, Mesh, Halftone
  hexagons) is another session's, in a file of its own.
- Gates run: `AMBIENT_PATTERNS`, `OUTLINE_PATTERNS`, `AMBIENT_SEAMS`, `TOOLTIPS`; the full suite
  was not run (the owner is short on usage).

## Outline designs filled with a pattern (0.1.0-beta215)

Branch `feat/outline-patterns`, on `main` at beta214. Not pushed. The owner: "can any of the new
background animations have counterparts for the outlines/borders?", then "maybe also add some
more procedural animations for the background and borders. I'd like at least one of them to be
like a circuitry/circuitboard animation style." Seven new Ambient Background patterns (10 to 16:
Circuit board, Hex grid, Matrix rain, Tunnel, Ripples, Embers, Sun grid) and fifteen new *Outline
design* entries (12 to 26) in Shadow, Glow & Outline and Scene Shadow fill the line with one of Ambient
Background's patterns; *Third colour* and *Pattern size* are the two new controls. Details in
AGENTS.md (*Outline designs filled with a pattern*).

- The pattern is drawn by the patterns' own shader files into a small picture
  (`src/look/outline-pattern.*`); the outline shaders only read it (two new uniforms, one line
  each). **Only run on OpenGL.** If a pattern outline is a plain solid line on Windows, ask for
  the log line `Outline patterns are not available (...)`. If *every* outline is gone on Windows
  after this version, the two new uniforms are the first thing to look at.
- Scene Shadow: the pattern lies over the canvas. Shadow, Glow & Outline: over the source (it
  does not know where the source is on screen).
- Grid pulse and Sun grid were left out of the outlines. Aurora, Starfield, Rays, Ripples and
  Embers are mostly ground colour in a thin line; whether they are worth keeping is the owner's
  to say. Whether Circuit board reads as one on their screen too.
- The seven new patterns are in a file of their own, `data/effects/stagekit-patterns-more.effect`,
  so a Direct3D refusal of one of them leaves the first ten (and their outlines) working: the
  background then shows the standby image for the new seven only, with its log line naming the
  file.
- Not done: Detail as a control (fixed at 50 %), a test of a Morph between two pattern lines
  (`lerp_design` carries the kind as for the other designs).
- Gates run: see the table in the commit's report; the full suite was not run (the owner is short
  on usage).

## Ambient Background patterns (0.1.0-beta214)

The owner: "id like to have some animated ambient background choices added too. some procedural
ones that would be appealing." *Background from: A pattern* (`base_type` 3) with ten patterns, a
three-colour palette, Speed, Size and Detail. Details in AGENTS.md (*Ambient Background's
patterns*).

- The shaders are `data/effects/stagekit-patterns.effect`, a new file loaded only when a pattern is
  chosen. **It has only run on OpenGL.** If Direct3D refuses it, the background shows the image and
  the log has one line, `Ambient Background: the patterns are not available (...)`: ask the owner
  for that line if a pattern shows the standby image instead.
- A pattern takes the image's place in the pipeline; nothing after it changed. Motion (drift, zoom,
  mirror) is not applied to a pattern and its group is hidden.
- Not built: *Colours from capture* (no cheap source of the capture's colours), a hex lattice for
  Grid pulse (it is a square grid).
- Tests run for this change: AMBIENT_PATTERNS (new), AMBIENT_SEAMS, AMBIENT_DARKEST, BLUR_OFF_DRAW,
  STANDBY_GIF, TOOLTIPS. The full suite was not run (the owner is short on usage).
- The default blur (60) turns every pattern into soft colour; the README and the tooltip say to
  turn it down. Whether the defaults look right on the owner's screen is theirs to say.

## Fading lines again, and a full-screen main image (0.1.0-beta213)

Branch `fix/outline-fade-2`, rebased on `main` at beta212. Not pushed. The owner on beta211:
"outline issue is worse now" (a screenshot mid change of scene: the band of each line showed the
other scene, a hole). Measured with two scenes that share nothing (blue and orange backgrounds, a
white box and a two-bar "title" in a group, 10 px black lines, both with Scene Shadow), half-way,
*Other items* Slide: the box's line was 120, 81 (red, green) in beta210 and in beta211, 164, 104
wanted (half black over the new scene) and got now. Fade: where both scenes have a line the pixel
was 120 in both, 0 wanted and got; beta210 had 77,585 values over 6 levels off the half-and-half
mix, beta211 53,548, now none (worst 0). In one scene, a source fading out as another fades in
over its line, Each source by itself: 164 in beta211 (the hole), 109 in beta210, 126 wanted, 130
now. Cause: the line was laid on the mixed picture of both scenes (beta210: see-through; beta211's
single presence also removed it wherever any other fading source stood). Fix: see AGENTS.md,
Scene Shadow, *A fading source ...* (Morph draws a scene's side at a time and mixes; in a scene
each fading source is a stage of its own). Morph now costs two sets of passes while unpaired
parts fade or move.

**Not reproduced: Make room movers half see-through.** The owner's saved Morph is Make room with
no fade. With colour sources, a shared full-screen background, a see-through full-screen overlay
on top and a game filling 91 % of the other scene, the movers and their lines are whole at 0.25,
0.5 and 0.75 in beta210, beta211 and now (test 3d). An earlier save had *Other items* Slide, which
fades what it slides and matches the screenshot as described; if movers still look see-through
with Make room, ask for the log and whether *Fade while moving* is on. Stand-in twins, real text
sources and browser sources were not in the test.

Also: *Morph the main image* takes a full-screen source that has a background under it (AGENTS.md,
Morph, *The main image may fill the screen ...*); the owner asked why Standby to Live did not
morph. Tested in OUTLINE_GLOW (3c, 3d, 3e; 129 checks before the rebase). Only OpenGL: `MixSides`
is new shader code.

## Outline designs (0.1.0-beta212)

Branch `feat/outline-designs`, on the beta211 branch. Not pushed. The owner asked for more outline
types, some animated or procedural: an *Outline design* list (Solid, Double line, Dashed, Dotted,
Gradient, Neon, Inner line, Marching dashes, Travelling gradient, Pulse, Shimmer, Rainbow) in
Shadow, Glow & Outline and Scene Shadow, with *Second colour*, *Dash length*, *Gap length*,
*Speed* and *Glow size*. See AGENTS.md, *Outline designs*. Choices made here: a line that follows
a shape is patterned round the source's middle in the per-source filter and along the canvas's
diagonal in Scene Shadow; Neon lies inside the outline's width and is laid over, not added;
Pulse changes the line's opacity, not its width. Gates passed: `OUTLINE_DESIGNS` (new, 36),
`OUTLINE_GLOW` (71), `SHADOW_EACH` (13), `MORPH_HOLDS` (5), `TOOLTIPS` (12); the full suite was
not run. Not seen on Direct3D or Windows. Release notes in `docs/releases/0.1.0-beta212.md`,
to be numbered just before pushing.

## Auto Capture's two gated tests: no plugin fault (0.1.0-beta211)

Branch `fix/auto-capture-tests`, on `main` at beta210. Not pushed. Tests only.
`STAGEKIT_TEST_ONLY_AUTO_WINCLOSE` and `STAGEKIT_TEST_ONLY_AUTO_KEEP_AIM` had been reported as
failing two checks each since beta202/203. Neither is a plugin bug and neither expectation was
stale: both write their red standby image to `<out dir>/../<name>-standby.png`, and the runs that
failed gave an output folder that did not exist (`G`, relative to the worktree, where the files are
in `out/G`). `QImage::save` then failed without a word, the Standby filter had no image (the log's
`failed to load texture 'G/../winclose-standby.png'`), and exactly the checks that look for the
red image failed (centre 0, 0); every aiming check passed. Run as `tests/run-tests.sh` runs them
(an existing folder) both pass on unchanged beta210: 17 and 25 checks. No expectation was
changed. The two sections now make the output folder and check that the image was written
(18 and 26 checks), so a wrong folder says so in its own line; passed with a folder that did not
exist too. Also run, all passed: `AUTO_REST` (18), `HOOK_BACK` (6), `CAPTURE_HIDE` (5).
**When running a gate by hand, give argv 6 and 8 a folder that exists** (`out/tooltips` as
run-tests.sh does). Other sections that write to `<out>/../` have the same weakness and were not
touched.

## A fading source and its outline are one flat picture (0.1.0-beta211)

Branch `fix/outline-fade-flat`, on `main` at beta210. Not pushed. The owner, after beta210: "the
directly morphing outlines are fine, but fading ones using the secondary animation still have the
multiply/transparent issue when transitioning." Cause, the same in all three paths (the scene
Morph's unpaired parts, an item's own show or hide transition under a Scene Shadow, the item
Morph's layer): the mask held the fading source at its faded alpha, so the shader took the share
it let through for free room and laid the source's own line (and shadow) there, under the source:
at half-way a quarter of the line over the whole of the picture. A red source (176, 32, 32) on a
208 grey background, green half-way: 113 before, 154 now (154 is half the source over the
background in linear light); over another source 79 before, 107 now; the line itself was right
already (152, half black). beta207's test used a white source, where a clamp hid it. Fix: a
*presence* picture beside the mask (see AGENTS.md, Scene Shadow, *A fading source ...*). Costs one
more pass of the morph, or one more canvas in a scene, only while something fades. Limits: a
soft shadow of another source falling under a fading one is thinned there by the fading one's
alpha (it cannot be told from its own); a source that is see-through all over while its transition
runs counts as whole. Tested in OUTLINE_GLOW (section 3b, ten checks: Morph half-way alone and
over another source, both *Shadow from* ways; Make room without a fade solid; item show
transitions: OBS's fade, Morph unpaired fade, Make room fading through the layer). Gates passed:
OUTLINE_GLOW (71), SHADOW_EACH (13), MORPH_HOLDS (5), MAKE_ROOM (15), TOOLTIPS (12); the full
suite was not run. Only OpenGL: the two new techniques (`Presence`, `TopFirst`/`TopReduce` in
`stagekit-looks.effect`) and the new `presence` uniform (set at every `compose`) have not run on
Direct3D.

## Square lines on Windows, and text keeps a following line (0.1.0-beta210)

Branch `fix/outline-square`. Not pushed. The owner's Standby scene (beta209, Square and then Scoop)
showed lines off their pictures and missing bottom sides. Cause: the visible-rectangle passes
picked with `lerp` from a 1e9 "empty" value; Direct3D's lerp (a + t(b - a)) lost up to some 32
picture pixels there, OpenGL here none, so the tests passed. Now an exact pick and a 65536 / -1
empty value. And every box mode keeps a following line for a source filling under nine tenths of
its visible rectangle (text, logos, groups with gaps) or turned. Tested: bigger-than-picture
bounding boxes aligned top, top-left, centre and bottom-right (colour and image, both filters), a
moved and scaled group, a nested scene, 2560 by 1440 at 0.05, overlapping, hidden, off screen,
turned, a gappy group. Not tested: an item mid show or hide transition (its rectangle counts
alpha from a tenth, and a fading source fills under nine tenths, so it follows and fades). Gates:
OUTLINE_GLOW, SHADOW_EACH, TOOLTIPS. Not seen on Direct3D.

## Outline round what shows, and Square corners (0.1.0-beta209)

Branch `fix/outline-shape-box`. Not pushed. The owner saw a line of varying width in Scene Shadow
with *Rounded*, size 0, round a capture fitted in a bounding box and round a small widget. The box
lines went round the item's box; a box with see-through margins (a widget's, a window capture's)
stood the line off the picture on those sides. Bounding boxes (scale to inner, aligned left or
centre), crops and scale were measured right already (10.0 px on every side). The line now goes
round the rectangle the visible pixels cover, found on the graphics card (`VisibleRect`), in both
filters and in Morph; *Square* was added (5). The per-source filter's line is still cut by an item's
crop (OBS crops the filter's image) and, in a box that draws it larger than its source, is
resampled (soft, within half a pixel). Gates: OUTLINE_GLOW, SHADOW_EACH, TOOLTIPS.

## A rename right after a fit filter goes (0.1.0-beta208)

Branch `fix/fit-rename-uaf`. Not pushed. Valgrind found an invalid read in `engine_owners()` when
a source was renamed right after a Fit to Space was removed: `obs_source_release` frees a source's
weak control at once and OBS calls the filter's destroy later on its destruction thread, so the
engine was still in `g_filters` and `obs_source_get_ref(source_)` read the freed control. Fit
engines, Scene Fit, Scene Modes and Scene Transitions now hold an `OwnerRef`
(`src/common/owner-ref.hpp`) and their registries take references through it. Scene Modes'
destructor disconnects `source_rename` outside `g_modes_mutex` (a rename in progress holds the
signal's lock and waits for that mutex). The 300 ms wait in the `MODE_PLACE` test before its
rename is gone. Gates `FIT_RENAME` (also under valgrind), `FIT_ENABLE`, `FIT_KEEP_SIZE`,
`MODE_PLACE` passed; the full suite was not run.

## Outline corners, matching outlines, solid through Morph, a smoother glow (0.1.0-beta207)

Branch `fix/outline-glow`. Not pushed. Four of the owner's items and one from the coordinator, in
Shadow, Glow & Outline and Scene Shadow (see AGENTS.md, *Outline corners, matching widths ...*):
*Outline corners* and *Corner size*; Scene Shadow's drop shadow thrown by the outlined shape
(the cause of "thicker" and of the square shadow corners: widths themselves measured equal);
Morph's silhouette of two different sources changing into each other made whole (the only
see-through case found; shared and one-scene sources were already right); a smoother, unbanded
glow with *Intensity*. Gates passed: OUTLINE_GLOW (new), SHADOW_EACH, CORNERS_WHOLE, MORPH_HOLDS,
BLUR_EDGES, SHADOW_FLICKER, TOOLTIPS. Not seen on Direct3D or Windows. To free disk space for the
build, the old scratch folders `perf2` and `linktrans` and `morphholds-vg.log` (from 2026-11-17)
were deleted from the session scratchpad.

## Scene Modes: places per mode, and Settings for (0.1.0-beta206)

Branch `feat/mode-transforms`. Not pushed. Each mode can keep saved transforms for chosen sources
(*Placed in this mode*, *Save their places now*, *Move duration* and easing) and glides them there
when it comes on; the window shows one mode's group at a time (*Settings for*, `mode_for`). See
AGENTS.md's Scene Modes entry. Gates `MODE_PLACE`, `MODES_HIDE`, `TOOLTIPS` passed; the full
suite was not run. Not seen on Windows.

## Ambient Background: no bars between repeats (0.1.0-beta205)

Branch `fix/ambient-seams`. Not pushed. The owner's background image had fully transparent left
and right columns (about 3 % a side); *Mirror at the edges* and the drift repeated them as black
strips. An image file's empty edges (see-through, or black all the way along; at most a quarter a
side) are now found once at load from the decoded pixels and left out of the shape and the
sampling (`picture_crop`, `uv_crop`). A source as the base and live captures are unchanged (no
cheap measure exists for them). The drift and mirror maths were checked and had no seam. Gates
passed: AMBIENT_SEAMS (new), BLUR_OFF_DRAW, AMBIENT_DARKEST. Not seen on Direct3D.

## An animated standby image plays (0.1.0-beta205)

Branch `fix/standby-gif`, on `main`. Not pushed. The owner's standby image is an animated GIF
(`start.gif`) and stood still. `tick_animated_image` in `src/common/start-image.*` ticks the
`gs_image_file4_t` as OBS's Image source does; the Start Image source and Ambient Background call it
from their tick, which covers every Standby filter (its picture is a private Start Image source),
Scene Standby's managed filters and the stand-ins. The dock's thumbnail plays a `QMovie`. A *File*
picture in a Standby filter was OBS's own Image source already and played. New gate
`STANDBY_GIF` (its own run in `run-tests.sh`); the full suite was not run. Not seen on Windows.

## A hook by hand hands the capture back (0.1.0-beta204)

Branch `fix/hand-hook-hands-back`, on beta203. Not pushed. The owner hooked a Game Capture by
hand on an emulator days before; the hook switched its Auto Capture off, OBS saved it off, and the
capture stayed aimed at the closed program. Now `hook_front_window` writes `hand_hooked` (the
program) into the Auto Capture it switches off, and the filter's tick (called while off) switches
it on again once that program has no window (`hand_watch`; 5 s grace after loading for a saved
record). User-switched-off filters are never switched on. Gate `HOOK_BACK` (see AGENTS.md's hand-hook
entry). Not seen on Windows.

## Window switchers share their ready captures (0.1.0-beta203)

Branch `fix/ready-capture-share`, on beta202. Not pushed; release notes written by another session.
The owner's logs: `tick_sources` median 3 to 3.9 ms while their Dolphin GBA windows (BitBlt Window
Captures) were open. Each Automatic Window Switcher kept a private ready capture of every open
listed window, the one its own capture was already on included, so a window was captured two or
three times. Now the ready captures are one per window, aiming and capture settings for the whole
plugin (`g_pool` in `src/capture/app-capture.cpp`, weak references and a count of users), and no
ready capture is kept of the window the switcher's own capture is on; switching to that window
mixes in the capture's own picture at once. See AGENTS.md, *Ready captures are shared*. Not done:
leaving out a window another ordinary capture source already shows. The ungated window switcher
section of the full run had two counts changed (3 to 2 kept, the capture's own window having none)
and was not run. Gates passed: READY_SHARE (new, 4 checks), CLEAR_PICKED, GAME_AREA,
AUTO_FRAME_SWITCHER, CAPTURE_HIDE, STANDBY_COVER. AUTO_WINCLOSE and AUTO_KEEP_AIM (Auto Capture,
no switcher) failed two checks each here; AUTO_WINCLOSE failed the same with beta202's
`app-capture.cpp` (AUTO_KEEP_AIM not tried so). (Not a fault of the plugin: the output folder
given did not exist; see *Auto Capture's two gated tests* at the top.) Not seen on
Windows: what a real Window Capture costs, and whether a switch to the capture's own window is
clean there.

## Fit alignment rule B and the alignment grid (0.1.0-beta203)

Branch `fix/fit-rule-b-grid`, on beta202. Notes: `docs/releases/0.1.0-beta203.md` (to be numbered
just before pushing; another session adds the window switchers' shared ready captures to the same
release). Not pushed. The owner chose rule B: "b is the correct logic. these features need to work
with any layout format in the future, and not be specific to mine", and asked for "similar
Alignment design as the built in bounds alignment for the transform menu".

- *Align in the space* alone places a kept-size source (`kept_in_room`); `soft_sides_of`,
  `lean_y_of`, `trim_for_kept_in` and the `grows_*`/`leans_*` on claims are gone. What keeps two
  fits from chasing: a resizing fit goes by what a kept-size fit of its standing holds at its
  home (`Claim::least`, `Obstacle::holds`), never by where it lies. Built from an earlier partial
  attempt (beta199-era), merged onto beta202 by hand; the dock's checks (`spaces_kept`) carry the
  held box (`MovedBox::keeps`, `held`).
- The filters' *Align in the space* list is in grid order with arrows (values unchanged; a real
  grid is not possible in OBS's properties window). The dock has a real 3x3 grid beside Fit to
  space; a source with no fit uses `ArrangeOptions::place_toward`, saved as
  `stagekit.place.toward`.
- Tests: new `[fit align: ...]` (`STAGEKIT_TEST_ONLY_FIT_ALIGN`). `fit_enable_test`'s *a game made
  60 px smaller by hand fills its space again* expected the column to stay to the pixel; under
  rule B the game fills its whole space (451 against the hand layout's 447, within the Size
  tolerance) and the column's resized sources move up to 5 px by their alignment, the gap kept:
  the check now allows 6 px and checks the gap. `fit_visible_test` expected the beta194 packing
  (a kept-size chat box at *The middle* packed the gap under the camera; a column at *The middle*
  against the right edge): its cases now set *The top* (chat box, and the camera in the banner
  case) and *The right* (column), and the game is centred in the room round the column's home.
  `layout_scenes_test`'s camera and chat box are kept towards the right (4): at *The middle* they
  moved 26.65 px with each capture of another shape, which is rule B. **The owner's own layout
  scenes have their column at *The middle* unless they set it**; the release notes say so.
  Gates passed: FIT_ALIGN, FIT_ENABLE, FIT_KEEP_SIZE, FIT_PRIORITY, FIT_GROUPS, FIT_TOOL,
  DOCK_BOUNDS, FIT_VISIBLE, LAYOUT, TOOLTIPS.

## Scene Fit rests at rest (0.1.0-beta202)

Branch `fix/scene-fit-idle`, on beta201. Notes: `docs/releases/0.1.0-beta202.md` (to be numbered
just before pushing). Not pushed. The owner's OBS 32 log: StageKit's ticks 0.27 to 0.36 ms a frame,
`stagekit_scene_fit: tick 5400 calls, 0.040–0.059 ms each` for three Scene Fits, one per layout
scene. Scene Fit already had the off-screen rule (`kIdleEvery`), but it counted *All sources* and
set claims first; and on screen it did a full solve every tick at rest. Now (`SceneFit::tick`): the
skip is decided first; on screen, after 30 quiet full ticks with the same `RestKey` (scene, claims,
glances, shadow room revisions), a full tick every 30th only, glances kept going in between
(`glance_at_rest`). New: `claims_revision()`, `glance_revision()`, `FitEngine::settling()`, a
`scene_revision_bump()` where a window switcher starts its mix, and `SCENE_FIT=1` /
`--scene-fit` for the perf harness. See AGENTS.md, *Fit to Space is idle off screen*.

Measured (`SCENE_FIT=1 FRAMES=600 WARMUP=6 tests/run-perf.sh`, µs a frame of Scene Fit, without
ticks of 1 ms or more, before → after): Live 78.0 → 28.2, Chat Live 109.1 → 31.8, Standby 22.0 →
12.9, nothing on screen 15.0 → 15.8 (noise; the off-screen full tick every fifteenth is most of it,
not cut further). StageKit's ticks in all, without those of 1 ms or more: Live 171.9 → 114.0, Chat
Live 207.4 → 119.1. Without the glances (an experiment, not shipped) Scene Fit on Chat Live was
about 20: the rest is the glances' drawing, which the full tick at rest did every frame before too.
Gates passed: FIT_GROUPS, FIT_PRIORITY, FIT_ENABLE, FIT_KEEP_SIZE, FIT_OWNER, FIT_TOOL,
FIT_VISIBLE, DOCK_BOUNDS, LAYOUT. The full run (its ungated `[scene fit]` section) was not run.

## Dock: Bounds list and Auto-fit switch (0.1.0-beta201)

Branch `feat/dock-bounds-autofit`, on the beta200 branch. Notes: `docs/releases/0.1.0-beta201.md`
(to be numbered just before pushing). Not pushed. The owner asked for a *Bounds* list like OBS 32's
Edit Transform and an *Auto-fit* on/off for the scene's fit filters; then for a level: *Selected
only* acts on the selected sources' own Fit to Space and their Scene Fit entries, which needed a
per-entry switch (`fit|<source>|enabled`, *Fit this source* in the Scene Fit window). Both are on
the Place tab under the arrows. Hotkey `stagekit.arrange.auto_fit` (*StageKit: Auto-fit on/off*);
no vendor request or Stream Deck key. Tested by the new gate `DOCK_BOUNDS` (23 checks) and
`TOOLTIPS`; the full suite was not run. Not seen in OBS on Windows.

## Scene Fit: alignment always obeyed, the dock button fits at once, no extra frame cost (0.1.0-beta200)

Branch `fix/fit-followup`, on `main` after beta198 (pull request 216, another assistant's fix for
the same fit problems, merged and released as beta198). Notes: `docs/releases/0.1.0-beta200.md`.
No Stream Deck file changed. Not pushed, not released. This finishes what beta198 left of the
owner's reports of 2026-11-19 and takes back three things beta198 broke; a second fix written
here on beta197 the same night (one local commit, never pushed) was the source for the ideas and
tests, but beta198's code is the base: nothing of it was replaced wholesale.

- **The owner (2026-11-19):** "align in the space is not being respected. chat box is not
  centering"; "I had sized the GBA screens to fit the space, and they've shrunk even when they are
  set to not resize"; and, of beta198 with the Scene Fit on and the game resized small by hand: "if
  I manually resize a source then click the fit to space button, it doesn't seem to work when scene
  fit is on, but it does eventually fit automatically. generally the scene fit takes a while to
  respond but it does eventually. the button doesn't seem to work while scene fit is on".
- **Align in the space at home** (`space_from`, `claim_box`, `apply`): a home is the space and the
  size given by hand, never a place. Kept size: by its alignment, as away from home. Resized: the
  home size by its alignment in the space it has now while the fill is within the Size tolerance,
  else it fills. Switching the filter on can therefore move a source; it glides there.
- **Glides start from where the item is** (`rebase`, `part_on_screen`): at switch-on, after a tool,
  after a move or resize by hand, after what shows of it is measured anew. Replaces "never glide
  from home to home" and "no glide from where it was let go".
- **The dock's Fit to space with the fit on** (`fit_item_refit`, `FitEngine::refit`): the source's
  own switched-on *Resize to fit* fit is told to fit now; status *Fitted by <filter>.*. Undo does
  not cover it (the fit moves it, and would fit an undone size again).
- **After a hand resize** the fit acts 0.3 s after the mouse is let go (was 0.3 s, then 0.4 s if
  the place changed too, then 0.5 s of home settling: 0.8 to 1.2 s). The mouse button is read on
  Windows (`mouse_held_any_thread`); the tests say it with `stagekit_test_mouse_down`.
- **Frame cost** (`FitScan::obstacles` / `look`, the own look in `prepare`): no look because a
  size changed; another fit's item by its claim without a look; by boxes nothing is drawn; a kept
  entry no longer looks at itself on every tick. Counted by `stagekit_test_looks` (`fit_obstacle`,
  `fit_own`).
- **Room kept** for a showing fitted neighbour that draws nothing (the beta147 rule, lost in
  beta198 for any source with a size).
- **Kept size** (`KeptSize`): `keep_w/h` plus the sizes in its source (`keep_sw/sh/pw/ph`) and
  `keep_k` for older versions; the scale stays while the source keeps its size and shows another
  part; only a scale while no size can be read yet (older collections, a source not steady);
  read while a Standby Image stands in, what is on the screen is kept as a rectangle (and the size
  noted before the filter likewise), never the image's scale, which turned into a size with the
  capture's made a 480 px window some 118 px wide;
  not re-read when the item is drawn at the size kept; the log line is back. Nothing blocks on a
  conversion any more.
- **`take_home_`** is used up for an entry hidden or without an image at switch-on.
- **First placing** waits for what shows of the item, 1.5 s at the most.
- **The dock tool**: the space is looked for round the rest of the item when its middle is
  covered (`best_space`); a press that found OBS busy leaves its looks for the next one (5 s); a
  kept-size source whose space is too small, and one kept by a fit in an area of its own, are named
  in the status line.
- **Left as beta198 has it, on purpose:** no Standby stand-in is in the way of any fit (announced in
  beta198's notes).
- **Open, for the owner:** a kept-size source set to *The middle* above or below a resized image
  with no use for more height is still packed against that image (the beta194 rule from "the chat
  box is more than 30px from the visible border of the camera box"). Their chat box under the
  camera therefore packs under it; it does not sit in the middle of the rest of the column. The
  release notes' *Good to know* asks.
- **Not seen on Windows:** all of it. The mouse-button reading in particular has only run through
  the tests' own switch.
- **Tests** (each gate run alone under Xvfb on the final build; **the full `tests/run-tests.sh`
  was NOT run**: the machine was shared that night, so the ungated fit sections of the suite have
  only been read against the change, not run):
  `FIT_TOOL` 79 passed (cases 16 to 19 new: the owner's layout with the Scene Fit off and on, a
  neighbour's spare room, the dock press with the fit on, a resize and a move by hand with the
  mouse held and let go: it sets off 327 to 345 ms after letting go, 10 ms after a press),
  `FIT_ENABLE` 80 (re-laid: the layout it is switched on over is made with the gaps by another
  fit; the kept-size section runs at its end), `FIT_KEEP_SIZE` 28 (new gate), `FIT_OWNER` 15,
  `FIT_VISIBLE` 16, `FIT_PRIORITY` 41, `FIT_GROUPS` 15, `LAYOUT` 40, `MAKE_ROOM` 15, `TOOLTIPS`
  12; and, for what the engine changes could touch, `POP_IN` 4, `CAPTURE_CHAIN` 11,
  `CAPTURE_HIDE` 5, `LINK_TRANS` 14, `GAME_AREA` 8, `AUTO_FRAME_SWITCHER` 10, `STANDBY_COVER` 7,
  `RESTORE_HIDDEN` 5, `MODES_HIDE` 29, `LINK_FOLLOW` 51, `SHADOW_FLICKER` 32, `FILTER_REMOVE` 9.
  No failures. Against beta198's plugin the new tests fail where they should: 17 of the 28
  kept-size checks (alignment at home, the entry hidden at switch-on, the widget's room taken, 83
  px, a kept scale turned into the Standby Image's 158 px instead of 120, a kept-size box that
  shows four times as much of itself drawn at an eighth of its size instead of half) and 13 of
  `FIT_TOOL`'s.
  beta198's own tests on beta198's plugin: all gates passed here but `FIT_TOOL`, where *with
  shadows counted (by what shows), image and shadow stay inside the gap* failed in one run of two
  (it passed in every run of the new build; not looked into).
  clang-format 19 and `scripts/check-root.sh` clean.


## Morph between sources: the layer on OBS's own thread (0.1.0-beta199)

The owner (2026-11-19): OBS "probably froze. it's happened a bunch of times while pressing
streamdeck buttons, and the average time to render has been a lot higher". No crash file, so a
hang. Four freezes in their logs (beta179, beta195, beta196) each ended within 0.2 s of the item
Morph adding or removing its *Morph (StageKit, in progress)* layer from its tick callback. AGENTS.md
(*The layer is put into its scene and taken out on OBS's own thread*) has the locks involved. Which
two locks met on the owner's machine is **not proven**: there is no memory dump. The release notes
ask for one if OBS freezes again.

What changed:

- **Who does what.** The item Morph's tick callback (OBS's drawing thread, with OBS's lock round
  all tick callbacks held) now only looks and decides. A change is claimed there (`Job`,
  `Entry::claims`); its layer is put into the scene, and later taken out, by `ui_work` on OBS's own
  thread (`src/common/ui-task.*`, `obs_queue_task(OBS_TASK_UI, ..., false)`, never waited for; one
  task on its way at most). The tick after the layer is in (`take_up_layers`) the layer starts
  drawing and the two items' own transitions stop, in one frame. Until then the hidden item stays
  whole and the shown one is not drawn, so a late window thread shows as a later start, never as
  a blink. A change that is over first gets no layer.
- **Lifetimes.** A `Job` holds its two items, their sources and the clock's transition from the
  tick that pairs them until `ui_work` lets go of them (`Job::let_go`); nothing of OBS's is let go
  of on the drawing thread any more, also not what a tick took hold of for its own look (`Bin`).
  The scene is still only held weakly. `item_morph_let_go()` and `item_morph_stop()` (the frontend's
  events, on OBS's own thread) end everything and take every layer out there and then; a task
  that comes after finds nothing to do. **A layer is only taken out of a scene that can still be
  held** (`take_out`: a strong reference from the job's weak one, for that call). A scene whose
  last reference is gone takes its own items out on OBS's thread for unmaking things and is freed
  straight after; `obs_sceneitem_remove` reads the item's scene without a lock and could lock a
  scene being freed. Such a layer is left to its scene (found in review; the race was there
  before, from the tick).
- **The tick never waits for a scene's locks with OBS's list of sources locked.** `find_items`
  lists the scenes first (`obs_enum_scenes` holds the canvas's `sources_mutex` for all of its
  callbacks), holds each, and goes through each scene's items after. OBS's own thread now takes a
  layer out with its scene locked and every `item_remove` listener called meanwhile; a listener
  that lists scenes or looks a source up there would wait for the tick, and the tick for the scene.
- **No `refresh_item` in the tick or in the layer's drawing.** It has OBS signal `item_transform`
  to every listener. The tick and the layer go by the place OBS last worked out
  (`item_box(item, true)`, `MorphPaint::look(..., false)`).
- **Show in Scenes and Link Visibility** find their items first and change them after the walk
  (`each_item`, `follow_elsewhere`), not with a scene's locks held.
- **Timing.** The tick callback is a kind of its own in the 30 s lines, `stagekit_item_morph`
  (`timed_tick_callback`).

One long-lived layer per scene was looked at and **not built**: its place in the stack depends on
the pair (so most presses would still reorder it, with the same signal and the same save by OBS),
two changes at once need two layers, and an item kept in every scene would be in OBS's Sources
list, in every saved scene and in every list a Stream Deck or websocket client reads. That is the
owner's call to make. So OBS still saves the collection once when a layer goes in and once when it
comes out; what one tick asks for is done in one task.

Not done, and known:

- **Not seen on Windows.** The freeze never happened in the tests either (they have no window);
  what is tested is that nothing is added, removed, made or let go of on the drawing thread, and
  that nothing blinks or is left behind whenever the task comes.
- `MorphPaint::seen_crop` reads a small copy back in the frame it is drawn in, once per item at the
  start of a change between two different sources: a stall of its own. Left as it is (noted in the
  code and in AGENTS.md).
- Link Visibility's `watch_others` still connects signal handlers inside `obs_enum_scenes`. The
  Standby stand-ins are still added and removed, and items shown and hidden by every filter, from
  a source's own tick (OBS's drawing thread, but with no lock of OBS's held, unlike a tick
  callback). If a freeze comes back, the memory dump will say whether one of these is in it.
- **Every other walk still goes through a scene inside `obs_enum_scenes`** (Show in Scenes'
  `each_item`, Link Visibility's `follow_elsewhere`, and most filters that look for a source in
  every scene): they wait for a scene's locks with OBS's list of sources locked, from a source's
  tick. Only the item Morph's `find_items` was changed. Such a walk can only hang against
  something that holds a scene's locks and then asks OBS for its list of sources, as a listener
  of `item_remove` or `item_visible` might. Whether any listener on the owner's machine does (the
  Elgato Stream Deck plugin's, say) is not known. Worth one sweep with a shared helper (list the
  scenes, hold them, walk after), as a change of its own.
- **OBS selects every item that is added to a scene, and deselects the rest of that scene**
  (`OBSBasic::AddSceneItem`, `select_one`, OBS 32.2.2 `OBSBasic_Scenes.cpp`). So each layer takes
  the selection in its scene and leaves nothing selected when it goes: a Morph in the scene being
  edited loses what was selected there. That is as old as the item Morph (it came as a queued call
  before and now runs inside `obs_scene_add`); read from OBS's source, not seen, and never
  reported by the owner. Not changed here: putting the selection back would say `item_select`
  again for each item, in an order the arrange tools would take for the order they were selected
  in (`track_selection_order`). A change of its own, with the owner's word on it.
- What a tick takes hold of for its look (a scene, its items) is held until OBS's own thread lets
  go of it: at once in practice, longer while that thread is busy.

Tests (Linux libobs 30.0.2, Xvfb, each gate run alone): see the results at the end of this
section. The test program now is OBS's own thread for the whole run: `ui_thread_start` sets a
handler for OBS's UI tasks, `sleep_ms` runs them, and a test can make each wait a number of frames
(`g_ui_delay`). The other test programs set no handler; there StageKit does that work as its tick
ends, as before.

Results, each gate alone with `STAGEKIT_TEST_ONLY_<name>`, the final build (after the review's
changes below), no failure in any:


`tests/run-tests.sh` (the full run) was **not** run here: other work was using the machine. Run it
before the merge. Under valgrind (the final build again), `MORPH_UI_TASK` runs to its end with no
memory error: its seven frame-by-frame checks fail there, as they must (a frame takes far longer),
and every check of what is held, left behind or let go of passes. `MORPH_HOLDS` under valgrind: no
memory error in three runs, all five checks passed in two of them; in the third the camera and the
capture of one case were not paired (the two changes came more than Morph's 0.1 s apart under
valgrind, so there was no layer to look at), and nothing was held there either.

**Reviewed twice before it was pushed** (threads and lifetimes; tests and docs), by reading, against
OBS 32.2.2's source. What came of it:

- *Changed:* `take_out` and a scene being unmade (above: low, and older than this change; the
  tick had the same race a frame later). The gate has a case for it, a scene let go of with its
  layer in it and the change half-way, and StageKit counts the layers it left to their scene
  (`left` in `stagekit_test_item_morph_layers`): one there. (The build before asked OBS to take
  the layer out whatever had become of the scene.)
- *Changed:* the case of a change that is over before its layer comes went by how fast the machine
  draws (150 frames against 4.9 s of waiting: about 31 frames a second). A test that has seen what
  it wanted of a late task now lets it come (`ui_tasks_release`, which `tasks_done` calls).
- *Changed:* AGENTS.md's Scene Shadow entry and a comment in `src/look/scene-shadow.cpp` still
  said the layer is added from a tick callback. What they claim still holds: `item_add` raises
  `scene_revision()` before `Job::placed` is said, so Scene Shadow has looked at the layer before
  the frame that first draws it.
- *Offered, and done:* `find_items` lists the scenes first (above).
- *Offered, and not done:* putting the selection back after a layer is added (the last point under
  *Not done, and known*).
- Nothing else: no new lock taken in two orders, no use after free and no gap in the hand-over
  from the items' own transitions to the layer was found.

What `MORPH_UI_TASK` looks at, frame by frame as OBS draws them: a pair and a Make Room item with
the layer 0, 1 and 5 frames late (never both pictures, never neither; the hidden item whole until
the layer is in); a change that is over before its layer comes (no layer, nothing held after); a
second key half a second into a change that hides the item the first one shows; the collection
changing and a scene removed with the task still on its way; a scene let go of with the task on
its way, and with its layer in it; and that every layer was put in and
taken out on OBS's own thread, by StageKit's own count and by the thread each scene signal came
on. Its second section has a listener that needs the scene from another thread while Show in
Scenes or Link Visibility hides a source. The same test program with the beta198 plugin fails
both: 138 scene signals came from the drawing thread, and the listener could not get at the
scene.

## Dock and Scene Fit in a split game layout (0.1.0-beta198)

The dock reads the selected source's fit settings even when the filter is disabled. Measurement runs
on the graphics thread. A timeout ends the attempt and is reported in the dock. Fitted neighbors
claim their image rectangles. A briefly hidden fit can reserve its last image for two seconds,
unless a mode holds it hidden. Neighbor prediction compares image position, size, growth and lean.

Resize-on entries keep a hand layout within their size tolerance and fill an undersized home.
Resize-off saves the reference image in scene pixels, including during conversion from an older
collection. Automatic capture switching preserves a visible display capture's reserved slot while the input
starts at 0x0. This keeps the camera and chat column in place through the first frame.

The LAYOUT, FIT_TOOL and FIT_ENABLE tests passed under Linux libobs and Xvfb. They cover dock
placement, a filled hand layout, capture switching, signal loss and recovery, and conversion of a
legacy saved scale. The same run executes the full suite with `tests/run-tests.sh`.
[Headless validation](https://github.com/TechSavage1989/StageKit/actions/runs/44462893755).

## Auto Capture stays aimed at a game while its picture comes back (0.1.0-beta197)

Branch `fix/auto-capture-keeps-aim`, on `main` after beta193. Notes:
`docs/releases/0.1.0-beta197.md`. No Stream Deck file changed. The owner (2026-11-19), with a
screenshot of a windowed Switch emulator running a Vulkan game and OBS not showing it: "auto
capture still takes a while to grab some windowed games".

- **Cause (from their log, read with scripts kept out of the repository; no window titles
  quoted).** The emulator was on *Always capture*. Each time OBS freed its hook (`vulkan capture
  freed`; twice `capture window no longer exists`), Auto Capture said `back to any full-screen
  application` 0.35 to 0.56 s later and `now follows` the emulator again 3.5, 45.3, 5.9 and over
  37 s after (the last until the log ends); OBS's first frame came 3.4 to 4.2 s after each
  re-aim. On two of the three returns no new hook was injected, so the same process had run
  all along (the 45 s one was a new process): no window of it passed the window watch's filter
  (visible, not minimised, not owned, not a tool window, a title) in those gaps, and nothing
  was chosen. Frees OBS handled with the aim left alone
  took 0.14 to 0.16 s (once 1.6 s). Every change of the capture's mode or window restarts OBS's capture
  (`capture_needs_reset`), so each drop cost two restarts. Re-following came within one tick
  and one look of the window reappearing. The 32 s to the very first frame was OBS hooking a
  game still booting. The other log (Dolphin) showed rests and quick pickup behaving.
- **Fix** (`src/capture/auto-capture.cpp`): the window watch also gives every program with a
  top-level window of any kind (`running`); the capture stays aimed at the program it is aimed
  at while that program runs (another of its windows, `same_program`, or, with none listed, a
  stand-in `kept` that writes nothing), and for `kKeepGrace` (3 s) after it was last seen
  running; only a candidate in front takes over meanwhile, an open one after the grace.
  Another class with no picture is aimed at in the same tick, with the picture there it is
  taken as it is (no restart). Logged `kept on <program> while its picture comes back`, once a
  drop, and once more when no window of it can be seen. Rests unchanged (a remembered game kept
  with no picture rests after 10 s; *Always capture* never). `rest_`, `tried_` and `spent_`
  count a program closed by `running`. Outside the tests the tick acts on each new look of the
  watch, with its clock stepping by real time. OBS's own windows' titles are read with
  `InternalGetWindowText` (a possible hang between the watch's thread and the UI thread, found
  reading the code). Details in AGENTS.md (*Auto Capture stays aimed at a game ...*).
- **Review fixes** (two reviews of the first build, in the same commit):
  - Keeping the aim had brought back beta190's last frame for a Dolphin-like program whose menu
    is minimised when its game window closes (none of its windows listed, so the watch reset as
    if it had closed). Now, with none listed, the watch resets only when the program is out of
    `running`, the capture has no picture, or OBS's Game Capture has sent `hooked`/`unhooked`
    since the game's window was last there (`hook_events_`: OBS can hook again between two
    looks); otherwise a game window still there unlisted keeps it and with all gone it lets go
    after 0.5 s, the minimised menu counted as spent. The reviewer's simpler rule (picture there
    means let go) would have let go of the owner's emulator whenever OBS hooked it again within
    one look, or its window was only hidden.
  - Switched off (a capture hooked by hand) and on again, Auto Capture now aims afresh
    (`aimed_once_` and `applied_key_` cleared): it had left the hand-hooked window while its
    old game was still open (older than this branch; `kept` made it likelier).
  - `kept` now ranks above an open listed or remembered game during the grace too (the task's
    rule); the test's "closed with another open" waits about 3 s. The test's `is_emu` also
    needs window mode: any full-screen keeps the window setting, so "opened again" had passed at
    once without a re-aim.
  - Docs: OBS's own hook after a re-aim was 3.4 to 4.2 s, frees left aimed 0.14 to 0.16 s (once
    1.6 s); the Troubleshooting note says the `kept on` line comes once the game has shown an
    image.
- **Tests:** `[auto capture: stays aimed at a game while its picture comes back]`, alone with
  `STAGEKIT_TEST_ONLY_AUTO_KEEP_AIM` (25 checks, all passed), and in the full run after
  `auto_window_closed_test`. Each of the three parts of the new no-window rule was taken out
  once and its check failed. Also run: `AUTO_REST`, `AUTO_WINCLOSE`, `CAPTURE_HIDE`,
  `TOOLTIPS`, and the full run's `[auto capture]` block under a temporary gate (not committed):
  all passed. The full run itself was not made here (machine limits).
- **Not proven:** none of the Windows side has run (`running`, `InternalGetWindowText`, acting on
  the watch's looks, the Game Capture's hook signals). Which of the emulator's windows the filter
  missed is a guess (its game-list window minimised, with *Single Window Mode* off): worth asking
  the owner. A game window gone while OBS captures through another window of the program, with
  the last frame up for over half a second, is still let go of (AGENTS.md). The fix removes the
  hop to any full-screen and its two restarts; it does not shorten OBS's own 3 to 4 s from a
  game starting to its first frame.

## Window switcher: Game area only (0.1.0-beta193)


## Auto Frame goes by the window the switcher shows (0.1.0-beta197)

Branch `fix/auto-frame-follows-switcher`, on `main` after beta194. Notes:
`docs/releases/0.1.0-beta197.md`. No Stream Deck file changed. The owner (2026-11-19), with a
screenshot of a Window Capture's filters (Automatic Window Switcher, Auto Frame, Show in Scenes,
Standby Picture) showing a web browser while Auto Frame's top line said *Capturing Dolphin.exe.
Aspect ratio: 4:3, from your saved shapes.*: "says capturing dolphin, but it's a web browser".

- **Cause** (reproduced: the new test, with the fix switched off, gives the owner's very line
  and cuts the browser's 800x450 to 600x450). Since beta119 a switch only changes which ready
  capture is drawn (`bridge_`, `own_ok_` false); the Window Capture itself stays aimed at an
  earlier window. Auto Frame took the program from the capture's `get_hooked` and `window`
  setting, so it kept Dolphin's saved 4:3 on whatever was shown. The AGENTS.md line "The parent
  is still aimed, because Auto Frame and others read the program from its settings" was
  beta116's and stopped being true at beta119.
- **Fix.** `app_capture_shown_window` (`src/capture/app-capture.*`, `AppCaptureShown`): the
  program and title of the window the enabled switcher shows (`shown_entry_`, `shown_exe_`,
  `shown_title_`, set in `aim` and at the first look's adoption, the title refreshed by window
  handle at each look); nothing while it shows nothing (`unsafe_`, or nothing chosen with the
  capture's own image not believed); false, so the capture's own settings count, when it has no
  say (switched off, or no window chosen since OBS started or it was switched on, with the
  capture's own image passed on: lists nothing, not looked yet, not on Windows; lists emptied
  while it runs leave the last window shown and named). Under the filter's lock,
  through `obs_source_enum_filters` like `app_capture_change`; nothing held after it. Auto
  Frame's `captured` (one call in place of `captured_program` and `captured_executable`) asks it
  first; its saved shapes (by program or title), learned shapes, held shape, status line and the
  first entry of its *Program or window* list go by it. A change of `app_capture_changes` makes
  it look before the next image (`switches_`), as a new size does, so windows of the same size
  change program at once too.
- **Two cases found in review, fixed.** (1) Switched off and on again: the off branch of
  `AppCapture::tick` drops the ready captures and the bridge and passes the capture's own image
  on (`own_ok_` true), but kept `current_`; on again, with no listed window in front, the first
  look found `current_` still listed and neither adopted the capture's aim nor aimed anything,
  so the capture's own window (the emulator) was drawn while the browser was named (and its
  status line said so too). The off branch now forgets `current_`, `shown_handle_` and the
  `shown_*` fields, so the first look after it adopts where the capture is aimed or aims afresh,
  as at start. (`shown_handle_` too: kept, a window closed while off would have set `unsafe_`
  at that look, which the adoption does not clear.) (2) The window shown closes with nothing else
  open: `phase_` is `Lost` and its last image is held for `kLostFor` (0.8 s) before the Standby
  Image covers, but `current_` is cleared and `unsafe_` set at once, so Auto Frame named nothing
  and dropped the shape off the held image. `shown_window` now names `shown_exe_`/`shown_title_`
  (not cleared with `current_`) while `Lost` with a kept image after a close, as beta194 did
  when the closed window was the capture's own.
- **Readers of the capture's own `window` left as they are**: the switcher's own (first look's
  adoption, the title re-aim in `ready_for`, `settle_aim`, its status line when switched off: all
  about where the capture itself is aimed), Auto Capture's (its own Game Capture, which it aims
  itself), and Show Source in Use's `program_of`/`title_of` (which app each listed capture is
  *for*, set up by aiming it; a capture with a switcher is not meant to be in that list, and
  going by what it shows would change which item an existing scene filter shows). Standby, Fit
  to Space and Ambient Background never read a program; they use the switcher's own calls.
- **Not changed**: a shape set by hand for this game only (*Set Aspect Ratio*, `force_until_gone`)
  is still dropped only when the capture has had nothing for 4 s, so it stays on through a
  switch to another program, as it did when a re-aimed capture found the next window quickly.
  Not asked for; worth asking the owner.
- **Tested** (`[auto frame: goes by the window the window switcher shows]`,
  `auto_frame_switcher_test`, alone with `STAGEKIT_TEST_ONLY_AUTO_FRAME_SWITCHER`, also in the
  full run after the clear-picked section; stand-in kind `stagekit_test_af_window`): emu_43.exe
  (640x360, `emu_43 = 4:3` saved) trimmed 80 px a side; switched to browser.exe (800x450, nothing
  saved) while the capture stays aimed at emu_43: *Capturing browser.exe*, trim 0 on every one of
  39 frames from the first frame of the switch; the browser's title changed to one with a saved
  `arcade night = 3:2`: 62.5 px; back to emu_43: 80 px and *from your saved shapes*; switched
  off and on again with the browser drawn and nothing in front: the window named is the one
  drawn (emu_43, 640 wide, 80 px); the window shown closes with nothing listed open: 4:3 kept on
  every held frame (24, 14 of them 0.35 s on, after Auto Frame's next look), then *No program
  detected*; switcher off: the capture's own emu_43 again. With the fix switched off 4 of the
  first 8 checks fail; without the two review fixes the two new checks fail (*Capturing
  browser.exe* over the 640-wide image with no trim; the held image's trim 0). Also run here,
  all passed: `GAME_AREA`, `CLEAR_PICKED`, `CAPTURE_HIDE`, `STANDBY_COVER`, `TOOLTIPS`; the new
  section three more times. The full suite (with the `[auto frame]` and the main window
  switcher sections, whose off-and-on step starts with nothing shown and so is not changed) was
  not run here. Not seen on Windows with a real Window Capture.


## Ambient Background: Darkest level is a soft floor, under the blur (0.1.0-beta196)

Branch `fix/darkest-level-soft`, on `main` after beta193. Notes: `docs/releases/0.1.0-beta196.md`.
No Stream Deck file changed. The owner (2026-11-19), with a screenshot (a mostly black live
capture, hexagons, Darkest level 1 % toward a bright magenta image, the black parts the image at
full strength with a hard edge against a dark circle): "Even at 1%, Darkest level seems to bright
a lot. it's also not smoothly blended. the darkest level should be effected by the blur settings".

- **Cause** (beta192's `PSLift`): `c = lerp(rgb, fill, under * lift_image)` with
  `under = 1 - smoothstep(0, 1.5 x level, luma)` put the whole image wherever the picture was
  darker than 1.5 x the level, whatever the level (it only set the threshold), and the pass ran
  last, after blur, mosaic, bloom, brightness and vignette: a band a pixel or two wide next to the
  blurred black, never blurred or cut into cells.
- **Formula** (`PSLift` in `data/effects/stagekit-looks.effect`): the floor `F` is the image times
  the level, or the colour scaled to the level's luminance (grey making up what a saturated
  colour cannot give). With `l` the part's luminance, `Lf` the floor's and a knee `k`: `h =
  saturate(1 - |l - Lf| / k)`, `m = max(l, Lf) + k h^2 / 4` (a quadratic smooth maximum),
  `out = rgb + F (m - l) / Lf`. `k` is `Lf` (as wide as the floor is bright: from `2 Lf` up
  nothing changes), except toward the image, where it is `min(Lf, Li - Lf)`, `Li` the image's
  own luminance there. Black becomes `F` exactly (1 %: 1 % of the image), the luminance rises
  smoothly and never falls (in the part's brightness and in the level), and only the floor's
  colour is added (no hue shift for the picture's own bright parts). The narrower knee toward
  the image was found in review: as wide as `Lf` it reached the image's own brightness above
  50 % and made the image itself brighter, by `(level - 0.5)^2 / level` (a quarter at 100 %),
  so the image round a capture was brighter than in its black parts and dimmed when a dark
  capture came in. Now no part toward the image ends brighter than itself or the image: up to
  50 % nothing differs, the image alone is left as it is at every level, and 100 % is a plain
  `max(l, Li)` (`k` 0; a kink in the curve there, which no monotonic curve that keeps both black
  to the image and the image as it is can avoid; the blur after smooths it). Chosen over a cap
  of the added luminance at `max(l, Li)` (the other reviewer's way): the same up to 50 %, but
  that one has a kink above 50 % where the cap starts, this one is smooth below 100 %. Chosen over
  `rgb + F w(l)` with a smoothstep weight falling to 0 at 2 to 2.5 x the level: that adds half to
  two thirds of the floor to a part as bright as it, this a quarter, and nothing from `2 Lf` up.
- **Where** (`render` in `src/ambient/ambient-background.cpp`): straight after `tr_scene_` is put
  together (image, capture, the kept picture of a change), into `tr_lift_` at half size; the
  blur or trail, bloom and the mosaic take the lifted picture, `Final` (overlay, vignette,
  brightness) comes after. `tr_scene_` stays unlifted (`tr_was_` copies it). The image fill is
  `tr_scene_` while it holds the image alone, else `draw_base` into `tr_fill_` at the same size and
  place, no longer blurred (`fill_blur_` removed); `lift_brightness` and `lift_vignette` removed
  from the effect and the code. Level 0: no pass, the picture to the bit as before.
- **Settings** unchanged (`darkest_level`, `darkest_fill`, `darkest_color`, same ranges and
  defaults); a saved level looks much darker toward the image now, which is the fix. Tooltips of
  *Darkest level* and *Lift toward* say what the level now means.
- **Tests:** `[ambient background: darkest level]`, alone with
  `STAGEKIT_TEST_ONLY_AMBIENT_DARKEST`, 15 checks, all passed: toward the image (mean 119.7 of 255
  in the black part) the black is 1.26 at 1 %, 12.07 at 10 %, 36.00 at 30 %, the image itself at
  100 % (0.00 off); 0 exactly as off; the largest neighbour step under a Gaussian of 80 is 4.93 /
  4.21 / 3.93 at 1 / 10 / 30 % against 5.00 with no lift; 96 square mosaic cells each one colour
  (0 apart). The same new checks against beta192's code: 6 failures (119.65 at 1 %, a step of
  82.28, cells 201 apart). Also run: `BLUR_OFF_DRAW`, `TOOLTIPS`, all passed. The full run was not
  made here (machine limits). Contact sheet `docs/images/ambient-darkest.png` regenerated (eight
  frames). After review: 16 checks, all passed; 75 % added (89.77 against 89.74) and the image
  alone lifted toward itself at 30, 75 and 100 % (0.000 off; with the knee as wide as `Lf`,
  7.67 at 75 % and 21.53 at 100 %, a failure). The eight frames of the sheet are byte for byte
  as before (frame 6, 100 %, is the capture's black part, which was already the image), so the
  sheet was not made again. Final now comes after the lift, so a Brightness under 100 % or a
  vignette of 2/3 or more takes the colour floor below the level (beta192 guaranteed it after
  them): the tooltip and the notes say so.
- **Not proven:** Direct3D (the shader keeps to the rules: no loops, no `inout`, no `atan2`, a
  ternary on a scalar condition as beta192 had, `saturate`, `abs`, `min`, `max`, `lerp`); not
  seen on the owner's machine.

## Window switcher: Game area only (0.1.0-beta193)


## Dock: Fit to space and Edit transform (0.1.0-beta196, plugin 0.33.0.0)

Branch `feat/dock-fit-tool`, rebased on `main` after beta194. Notes: `docs/releases/0.1.0-beta196.md`.
The owner (2026-11-18), with screenshots of a layout of a game and a column of two handheld screens
and a capture card: "i'd like a new arrange or place option to "fit" an object to the available
space respecting existing rules like margins and gaps and use visible pixels, etc. I'd also like a
shortcut button in the relevant tab to edit transform of the selected source."

- **Fit to space** (`ArrangeAction::FitSpace`, `kInfo` name `fit_space`: so also the hotkey
  `stagekit.arrange.fit_space`, the preview menu's entry and the Stream Deck Arrange tool's option)
  runs `fit_to_space` in `src/dock/arrange.*`: per item, top-most first, the obstacles are a
  `fit::FitScan` (one for the press, so each other item is looked at once and followed by its box
  after that, and claims of the scene's fits count where they take their items), the space is
  `fit::free_space` (Tolerance 8, as Fit to Space's default) round the centre of what shows, with
  every obstacle marked firm except one the item lies wholly on (a background): an item it overlaps
  is stepped off, never covered. The item is scaled (or its bounding box) by the one factor that
  fits, and shifted so what shows is centred. A shadow sized in screen pixels that counts (*Ignore
  shadows* off) is allowed for as in `match_size`, with one more measure for the place. The gap is
  the Place tab's *Edge distance*, which is now an arrange option (`place_margin`, saved as
  `stagekit.place.margin`), so the hotkey and the key have it without the dock. Each changed item
  goes to `fit_item_placed` once it is where it is going: `run_arrange` gives the items' old
  transforms to the dock's glide handler (`set_arrange_glide`, `ArrangeStart`, owned by one dock),
  which puts them back and glides them over the *Duration* and *Motion* (`glide_back`, the same
  as snap on release, which now uses it too) and hands each over when its glide ends (at once
  without a glide or a dock). The copy tools' hand-over is now per copy's scene, so *In every
  scene* does not hand the gliding item over early. An item with a screen-pixel shadow does not
  glide (its filter's shift depends on the size when it looks); each glide step adds what moved
  the item since the last step (`Glide::last`). A screen-pixel shadow that
  counts is allowed for by the picture's place, also when measuring by boxes; a *maximum size
  only* box has its scale scaled too.
- **It never makes another fit give up room** (the owner, after trying it: "the fit to space
  option/tool cannot force other objects to resize"). Built as: every fit of the free space in the
  scene (Fit to Space or a Scene Fit entry, enabled, its item shown or hidden) is known by
  `fit::held_spaces` (from `FitEngine::held`, written in `solve` each time the goal is worked out,
  kept while the item is hidden, cleared when the fit gets another item or publishes no claim;
  the item looked up by name, in groups too, and its id checked). Its item is an obstacle by the
  whole space it holds, firm, unless that space reaches what shows of the item (that fit then
  ignores the item or lies on it, and its claim counts as before: a camera on a capture that
  ignores it still grows over it); a hidden item's space counts the same. The result is then
  checked against every such fit (`spaces_kept`): the fit's last inputs (`fit::SpaceInputs`),
  with the moved item first followed from where it was when the fit looked (`at`) to where it is
  now, then to where it would be, both worked out by `fit::space_from`, the code `solve` itself
  calls (moved out of `solve`, no copy), must give the same space within 0.5 px. A shadow in
  screen pixels keeps its size in that mapping (`keeps_outside`). An item that fit did not see
  (hidden when it last worked its space out: `visible`) makes the check unknown, and the item is
  left as it is. Failing the check, the largest of twenty smaller sizes (down to its size now) in
  ten places in the space (centred, part of the way, four sides, four corners; the full size
  against sides and corners too) that changes nobody; none, and the item is left as it is. The
  status line names the source given way to, also when fitted spaces leave no room at all. An
  item whose own fit resizes it is left to that fit (status: "its own fit filter sizes ..."); one
  whose fit keeps its size goes where that fit will put it once handed over (`fit::claim_from`,
  what `publish_claim` now calls), and the others are checked against that claim. An item in a
  group a fit looks after is left as it is. Items changed earlier in the press are in the way
  where they now are. *In every scene*: a scene where the copy would change a fit (its own fit
  taking it where it would) is skipped (the copy is put back as it was; `copy_keeps_fits`,
  `copy_to_every_scene`'s `allow`), and named; chosen over shrinking there so the copies stay the
  same transform as the item. A scene with a shown fit not worked out yet (not shown since OBS
  started) is skipped and named too. Tested in `fit_tool_test`: case 11, the column's bottom item
  fitted by a Fit to Space (*Resize to fit* on, then off) and by a Scene Fit entry, its space
  ending 30 px under the small item: after the press its position, scale and bounds move at most
  0.5 px over 3 s of ticks (0.00 measured), the item grows to the gap above it only; with the
  dock's edge distance 10 against the filter's 30 it is kept smaller, against the top (0.12 px),
  and the status line says "Kept smaller so FT Column Bottom keeps its space."; with *In every
  scene* a second scene whose Fit to Space would lose room is not copied to and named, and that
  fitted source, shown for 2 s after, moves 0.00 px; a third scene never shown is not copied to
  and named. Case 12: the bottom item hidden, its space kept for it (not "kept smaller"), and
  shown again it moves 0.00 px. Case 13: an item its own resizing Fit to Space looks after is
  left to it. Case 14: an item in a fitted group is left as it is, the group named; a camera on a
  fitted capture that ignores it (hidden and shown once first) grows and the capture moves 0.00
  px. Case 15 (after the rebase on beta194): the bottom item laid out by hand with its fit off and
  then switched on (kept size, resized, Scene Fit entry) stays at its home before and for 3 s
  after the press (0.00 px), and `stagekit_test_fit_predicts` says the one fit's claim worked out
  again from `held_spaces` is the claim it makes (415, 248, 180 x 80), at home. The earlier cases
  (no fit filters, and an item a Fit to Space keeps the size of) are unchanged. Not done: an item a fit outranks or ignores is never in its way whatever happens, so
  that fit is not asked; item ids inside groups can repeat a scene's own ids, which the check
  does not tell apart.
- **Rebased on beta194** (homes, `switched_on`, fair column sharing, priority never overlaps, the
  gap from what shows of a letterboxed source). `FitEngine::solve` keeps beta194's working out
  exactly, now in `fit::space_from` with everything it reads in `SpaceInputs` (as before, plus the
  picture's size, `grows`, `leans`, the other fits' `homes`, and the fit's own home: `home_box`,
  `home_settling_`, `home_space_`) and what it decides in `SpaceOut` (goal, room before the cut,
  `avoiding`, `at_home`, the home's space when still being found, `space_grows_*`). beta194's
  `space_among` lambda, `trim_for_kept` and `trim_to_picture` became free functions
  (`space_among`, `trim_for_kept_in`, `trim_to_picture_in`), and `soft_sides`, `lean_axes` and
  `kept_in` wrap `soft_sides_of`, `lean_y_of` and `kept_in_room`; `kHomeSlack` is `fit::kHomeSlack`.
  `publish_claim` calls `fit::claim_box` (the kept size placed and clamped unless at home, or the
  picture in the room while reshaping), which `claim_from` calls with `reshaping` worked out from
  the inputs; `least`, `home`, `grows_*` and `leans_y` are set as in beta194. `held_spaces`' `space`
  is the room (before the cut), `goal` the goal. The dock's check (`spaces_kept`,
  `same_outcome`) compares goal, room, claim and at-home, each fit's inputs taken as settled
  (`settled`: a home still being found is worked out as it will be); an item's own fit with a
  home takes its home where the item is put (`handed_over`), also one whose home does not match
  the item now (`SpaceInputs::takes_home`, the engine's `has_home_`: `home_box` is false then, but
  `prepare` takes a new home on a placing), and in the others' inputs the moved item's home is
  that home where it is put (`MovedBox::home`, `home_after` in `with_moved`), not its claim,
  which is elsewhere when it does not keep the gap there. No beta194 test changed.
- **Edit transform** (`editTransform`) triggers the main window's `actionEditTransform` 50 ms after
  selecting the dock's item alone when it is in the scene being edited, switching the action on
  for the press (OBS switches it off when its Edit menu opened with nothing selected). Missing
  action: the button is off, its tooltip says so, and the log says so once. The real OBS path has not
  run here.
- **Why the Place tab**: both act on the one item the Place tab moves, and Fit to space uses its
  *Edge distance* and *Duration*; the Arrange tab's block is a full three by four. They are two
  drawings (`tool_icon` `fit_space`, `edit_transform`) under the Place tab's tick boxes.
- **Stream Deck**: `fit_space` in `src/tools.ts`, `titles.ts`, `pictures.ts`; drawing `fit-space`
  in `make-option-icons.py`, its badge in `make-badges.py`; files regenerated
  (make-option-icons, make-state-icons, make-live-icons, make-badges, make-svgs, make-plugin-files).
  Plugin 0.33.0.0 (main has 0.30.0.0; 0.32.0.0 is a queued branch's).
- **Test**: `[dock: fit to space and edit transform]` (`fit_tool_test`, alone:
  `STAGEKIT_TEST_ONLY_FIT_TOOL`; in the full run after the filter-removal test, before any sound):
  by visible pixels to the gap with exactly the edge distance, centred, shape kept, gliding, Undo
  and Redo, a second press changes nothing, by box, a bounding box, a shadow ignored, In every
  scene, locked, the hotkey, an overlapped item stepped off clear of all three others, two selected
  sharing a gap the edge distance apart, a shadow counted by what shows and by boxes, a glide time
  with a screen-pixel shadow, an item a Fit to Space (*Resize to fit* off) keeps the size it
  glided to; the Edit transform button off with its tooltip. Not run on Windows.
- **Done elsewhere**: the owner's other report in the same message (a layout set by hand changes
  size when its Fit to Space filters are switched on) was beta194's (homes); this branch now goes by
  those homes.
- **Tests run here after the rebase** (gated sections only, machine limits; the full run was not
  run): `FIT_TOOL` 49 pass (40 before the rebase, 9 new in case 15), `FIT_ENABLE` 17, `FIT_OWNER`
  15, `FIT_VISIBLE` 16, `FIT_PRIORITY` 41, `FIT_GROUPS` 15, `LAYOUT` 40, `MAKE_ROOM` 15, `TOOLTIPS`
  12; Stream Deck `npm test` and `npx tsc --noEmit`. Run `tests/run-tests.sh` before merging.

## Scene Fit: the gap from what shows of a letterboxed source (0.1.0-beta194)


## Every Stream Deck key says what it works with (0.1.0-beta195, plugin 0.32.0.0)

Branch `feat/streamdeck-works-with`, on `main` after beta193. Notes:
`docs/releases/0.1.0-beta195.md`. Stream Deck plugin only; no C++ changed. The owner
(2026-11-18): "I want the stream deck keys to show the filter or feature they work with somewhere
outside its [list]. somewhere in the configuration area at the top for all keys."

- **The row**: *Works with*, first on all 36 pages (the two hidden older keys too), written by
  `make-plugin-files.py` (`works_row`, `WORKS_SCRIPT`) from one table, `streamdeck/src/works-with.ts`
  (`WORKS_WITH`); see AGENTS, *Every key's page starts with Works with*. Line one: the filter's kind
  exactly as OBS names it, or the feature. Line two: what the key is set to, answered by the plugin
  (`getWorksWith` in `answerLists`, `worksWithNow`), with the chosen filter's own kind on line one
  and "(not found in OBS)" while OBS can be asked.
- **The beta189 line under the list is gone**: it said the kind a second time, now on line one; the
  list keeps its tooltip. `SCENE_NAMES` and `obs_name` in the generator are gone too (the kinds come
  from `shownName` through the table).
- **Every action passes its UUID** to `answerLists` (a fourth argument) so the answer is the key's own.
- VERSION 0.32.0.0 (0.31.0.0 is taken by an unreleased branch).
- **Tests**: `streamdeck/tests/works-with.test.ts` (7) and one more in `tests/obs.test.ts`; the
  whole Stream Deck suite 89 of 89, `tsc --noEmit` clean, `streamdeck validate` successful (offline:
  Elgato's remote schemas could not be loaded). No C++ build or OBS test was needed.
- **Not proven**: a real Stream Deck. The page script was checked against sdpi-components 4.0.1's
  own script (`window.SDPIComponents.streamDeckClient`, `send`, `sendToPropertyInspector.subscribe`,
  the `valuechange` event) and run in a `vm` with a stand-in client, not in Stream Deck's browser.
  How the two lines wrap in the 241 px content column is not seen: Manual Window / Source's first
  line names three kinds until one is chosen with OBS connected.

## Window switcher: Game area only (0.1.0-beta193)

Branch `fix/scene-fit-visible-pixels`, rebased on `main` after beta191. Notes:
`docs/releases/0.1.0-beta194.md`. The owner (2026-11-18, Component-Capture-Scene, a Scene Fit on
*Each source its own*, DSLR Component above Chat Box Group): "the chat box is more than 30px from
the visible border of the camera box, but the bounding box of the camera box is exactly 30 from
each edge". With Scene Fit off the dock's tools placed it right.

- **Cause** (reproduced, `fit_visible_test`): not *Use visible pixels*. Both measuring options are
  on by default for Scene Fit entries (`fit_entry_defaults`) and Fit to Space, and a Cropped View
  of an async camera is measured by what shows. The chat box keeps its size; the camera resizes, so
  `kept_in` sent the chat box to the far side (the canvas's bottom), and the camera's space reached
  down to it: a picture held by the column's width, centred in a space taller than it, 22 px empty
  above and 24 below (owner's: the same, at their size). Same with two Fit to Space filters.
- **Fix** (`src/placement/fit-engine.*`): a resizing claim carries `grows_x`/`grows_y` (would more
  width / height make the picture larger); `soft_sides` counts a side only along an axis the
  picture grows in (above and below only; side by side it is as before, whatever `grows_x` says);
  where the picture would not grow in height, `lean_axes` puts the kept-size item at the top of
  its space (only with *Align in the space* in the middle there, and no side that gives way
  deciding), says so in its claim (`leans_y`; `leans_x` is never set), and the resized fit cuts its space to
  its picture at the top (`trim_for_kept`). Packed towards the top whichever is above, so it
  settles in one move. No setting or default changed; layouts where the resized picture grows
  towards the kept item are as before.
- **Numbers** (test canvas 640x360, gap 20): chat box 44.0 px below the camera's picture before,
  20.0 after; camera picture 42 px from the top before, 20 after; 0 moves in 5 s at rest in all 14
  cases. **Horizontal packing taken out after review**: the first build also packed side by side,
  which moved a 4:3 game beside a kept-size chat column (column 73 px off the right edge on the
  test canvas, game against the left). Now that case is as at beta181 and is tested (column 20.0
  from the right edge, game 46/47 px either side, 0 moves, Scene Fit and Fit to Space).
  `STAGEKIT_TEST_ONLY_FIT_VISIBLE` 16 pass, `FIT_PRIORITY` 41, `FIT_GROUPS` 15, `MAKE_ROOM`
  15. The full run was not run here (machine limits); its fit block has the kept-size pair tests
  (*Fit As A Pair*, *Fit As A Trio*), whose resized game grows towards the kept items, so they
  should be unchanged: run `tests/run-tests.sh` before merging.
- **Not reproduced:** the chat box "constantly flashes back and forth" earlier in the same scene.
- **Left as it is:** with both resized, a picture that does not fill its space sideways still sits
  in the middle of it (the camera 40 px from the right edge with a gap of 20); that is *Align in
  the space*, and no one else's gap depends on it.

**Round 2 (same branch, second commit): the owner's five open Scene Fit problems in
Component-Capture-Scene** (2026-11-18, from their collection and three logs; kept outside the
repository). All reproduced in `fit_owner_test` (`STAGEKIT_TEST_ONLY_FIT_OWNER`, also in the full
run's fit block): a quarter-scale stand-in (640x360, gap 7.5 for their 30), their entries'
resize, alignment, tolerance, anchors, kept and original sizes, the GBA items starting in their
saved slivers, modes switched as cuts.

- **G1, a resized letterboxed camera's box took the column** (cause: the goal was the roomiest
  of the near-equal spaces, `free_space_round`, and the box was the goal; the claim was always
  the picture). Fix: `trim_to_picture` cuts the goal to the picture along an axis it does not
  fill when the claim of another fit that resizes its item lies beyond it on that axis (a
  kept-size item there does not count: against the camera and chat column a layout scene's 4:3
  capture lost 53 px of the capture's space, five layout-scene checks in both orders; the
  section runs alone with `STAGEKIT_TEST_ONLY_LAYOUT`). Camera box 120.0 high for an 88 px
  picture (27 % empty) before; 88.4 after. The owner's sliver state was not reproduced (it
  recovered within a tick here); a source with no image yet holds its saved bounding box
  (`holds_room`), which is now the picture rather than the whole space, which may have been it.
  Review fix: the cut box is used only while the picture keeps the shape it was cut for; another
  shape is given the uncut space at once (`reshaping`), or a capture changing shape beside the
  column was squeezed into the old box for 0.5 s and a glide. Tested: 120 px high four frames
  after turning taller, 88 without the fix.
- **G2, unticking Resize to fit went back to `orig_k`** (cause: `back_to_original_` read only
  `orig_k`). Fix: the saved `keep_k` first, `orig_k` only if none. 0.4765 set by hand came back as
  0.4899; now 0.4765. Review fix: the saved size is read at every update, so a rename of the
  source in between (Fit to Space rewrites `keep_item`) keeps it.
- **G3, Takes space first covered the camera and chat** (cause: `FitScan::obstacles` skipped an
  outranked claim, and a kept-size item cannot shrink to make room). Fix: an outranked kept-size
  claim is in the way by `Claim::least` (its kept size round its home), independent of how it
  reacts, so the dependence stays one-way; a resizing one is still not in the way (it shrinks).
  Tried and removed: a resizing lower's home as a point (two higher fits sharing a column then
  crept, `[scene fit groups]` cases 3 and 4), and passing over a home inside the higher's claim
  (hysteresis). Game over camera 576 px², over chat 1278 px² before; 0 and 0 after, in both modes,
  0 moves in 5 s.
- **G4, "did not settle in 4 passes"** (causes: goals chasing space edges, fixed by G1; and the
  camera, between two windows, listed before one of them, waited a pass for it each time). Fix:
  passes alternate direction, `kPasses` 8. From the slivers: the log line on entering GBA mode
  before; settles at the sixth pass after, no line anywhere in the section, 1 glide per change of
  mode, 0 moves at rest in every scenario.
- **G5, a hand resize read every frame** (cause: the detection compared the bounds with the last
  ones set on every tick, waiting only for the source's size). Fix: `hand_on_`, counted once the
  box has kept still 0.3 s, item left alone meanwhile, followed to the end if deselected. A 36-frame
  drag and the dock's 27-frame glide: 53 reads before, 1 after; the dock's final size is kept.
  Review fix: a change of the source's size during the 0.3 s starts the wait again, so the size
  is never read off a source still settling (a shadow's room catching up).
- **Changed behaviour, documented:** a *Takes space first* fit now stops short of a lower fit
  that keeps its size and moves when such an item is hidden or shown (its home comes and goes).
  No existing test expectation changed. On the owner's 2560 canvas a 16:9 game on *Takes space
  first* in GBA mode still leaves the resized column a sliver (they make room, nothing overlaps):
  that is what the setting means for sources that resize.
- **Tests run here:** `STAGEKIT_TEST_ONLY_FIT_OWNER` 15 pass, `FIT_VISIBLE` 16, `FIT_PRIORITY`
  41, `FIT_GROUPS` 15, `MAKE_ROOM` 15, no existing expectation changed. The full run was not run (machine limits): run
  `tests/run-tests.sh` before merging. Not seen on Windows.

**Round 3 (branch `fix/scene-fit-enable-keeps-layout`, third commit on top of the two above,
released with them): switching a fit on keeps a hand-made layout.** The owner (2026-11-18,
screenshots of Component-Capture-Scene's GBA column): "when I set up some layouts as desired ...
manually positioning and sizing things with the fit to filter disabled, then enable the filter,
even with resize to fit enabled, everything changes size". The second handheld window (*Resize to
fit* off, bottom right) shrank to a small box in the corner, the capture card shrank and moved.
Their saved collection predates the hand layout (the column saved as slivers), so it gave the
settings only; the size changes were reproduced headless (probe kept outside the repository).

- **Causes, by step:** `prepare`: unticking *Resize to fit* while off set `back_to_original_`, and
  the first tick on brought back the saved `keep_k` / `orig_k` from before the hand layout (round
  2's G2); a size kept before switching off stayed (a hand resize is only read while ticking); a
  filter loaded off read its saved `keep_k`. `kept_in` then put it against its bottom-right side.
  `motion`: anchors from the old session (a filter loaded off used its saved anchor at once).
  `solve`: a resized item always took the largest picture its space holds (the game grew 7 px,
  the camera re-centred), and once the kept item shrank the others grew into its room. Not causes:
  the fair sharing (`trim_to_picture`), Space priority (all *Shares space*), the visible-pixel cut,
  the settle passes (only the "did not settle" line when margins pass the screen's edge).
- **Fix** (`src/placement/fit-engine.*`, `scene-fit.cpp`): an off-to-on edge (`was_off_`,
  `switched_on`) takes the item as it is (anchor, kept size, no glide, stale claims dropped) and a
  **home**: the whole picture's box now and, over 0.5 s on screen, the space round it with the
  other fits' items at their homes. At home again (that space within 3 px, the gap kept within
  3 px) the goal is the home box exactly; otherwise the old fit, back home when undone. See the
  AGENTS.md entry. Saved as own settings (`home_*`), so a filter loaded on keeps it. A filter never
  switched off has no home: unchanged behaviour. Also, what shows of the fitted item is measured
  even when its box hangs past the screen's edge (`on_screen` dropped in `prepare`).
- **Decision on growth:** a resized item rests at its hand size while the room round it is as it
  was; it grows past it only when the room changes (a neighbour hidden), as before, and returns.
  (Capping at the hand size for good was the alternative: it would stop the column taking a
  hidden camera's room.)
- **Numbers** (640x360, gap 7.5; a 4:3 game with a shadow, a column of a letterboxed window,
  a 3:2 camera view and a kept-size letterboxed window, margins larger than the gap; the fit run
  first on another layout, switched off, the bottom unticked, laid out by hand): before, up to
  45 px of change (the investigation's probe); after, 0.0 px at +0.5 s and +3 s, middle hidden
  (the top window grows into its room) and shown again 0.0 px, loaded on from the saved settings
  0.0 px, for a Scene Fit and for four Fit to Space; a new filter never off still fills (game 450
  wide against 347 by hand).
- **Review fixes (same commit):** the reviewers' eleven findings (one reported twice) were all
  real; each code fault but the window switcher's was first shown failing by a new test case, then
  fixed. (1) Switched on with nothing measured yet (OBS started with the filter off, or a source
  resized while off), the space round each home was fixed before what shows of the items had been
  looked at: every item left home, up to 71 px. The settle now waits for the item's own look and
  its shape counted, and starts again while another fit's home changes. (2) A *Takes space first*
  fit was taken at its home by the lower fits: grown out of it, it came within 1 px of the column.
  (3) The shadow filter's own shift of an item when its room changes (sizes in screen pixels, at
  every resize) was taken for a move by hand: an item grown into a hidden neighbour's room made
  that its home, 81 px off afterwards; also what showed of a letterboxed window was forgotten at
  every such room change (21 to 42 px off), now only when the picture itself changes size. (4) A
  kept-size item at home within the 3 px slack of the gap was pulled the full gap in (1 px). (5)
  *All sources*: a source added while off joined half a second after switch-on with no home and was
  fitted (it moved 172 px and grew). (6) A window switcher's change was judged against the old
  window's shape, so the new window could stay squeezed into the old home box (not reproduced here:
  the switcher's Windows side does not run headless; fixed by going by the shape it changes to
  while it changes). (7) At home, a picture of another shape was given only its home box for half a
  second; now the space round it, as away from home, but not when only what shows of it is measured
  again (handing the space then made a letterboxed window jump at its first look, seen in the new
  test). Also found while testing: the home read from the bounding box was up to a pixel off what
  OBS draws (OBS places a picture in its box by whole pixels); it is now read off OBS's drawing
  transform. And the merge script's `FIT_OWN` (`scripts/merge-capture-scenes.py`) lists the
  `home_*` keys, and AGENTS.md's visible-pixels entry no longer says "wholly on the screen". Not
  taken: a general "at home but its home no longer fits: solve again at once" (it would send a
  window that changed shape off before its new shape is known, the half-second wait round 2 relies
  on).
- **Tests run here:** `STAGEKIT_TEST_ONLY_FIT_ENABLE` 17 pass (new, also in the full run's fit
  block after `fit_owner_test`; 8 before the review fixes, not 10 as first written), `FIT_OWNER`
  15, `FIT_VISIBLE` 16, `FIT_PRIORITY` 41, `FIT_GROUPS` 15, `LAYOUT` 40, `MAKE_ROOM` 15; no
  existing expectation changed. The full run was not run (machine limits): run
  `tests/run-tests.sh` before merging. Not seen on Windows: the owner should switch the Scene Fit
  off, lay the GBA column out, and switch it on again (a filter that has never been off has no
  home).

## Auto Capture picks a game up within seconds while it rests (0.1.0-beta191)

Branch `fix/auto-capture-quick-pickup`, on `main` after beta185. Notes:
`docs/releases/0.1.0-beta191.md`. No Stream Deck file changed. The owner (2026-11-18, after
beta184): "game capture is taking quite a while to hook windowed games and emulators, but i don't
want the overhead processing of OBS to go up".

- **Cause.** Beta184 made a rest real (the capture aimed at no window), but a rest was 20 to 60 s
  of nothing, title changes did not count in its first 10 s and ended three rests at most, so a game
  started in an emulator's menu (no image until a game draws) waited for the rest to run out.
- **Looks in** (`src/capture/auto-capture.cpp`, `Rest::probe_at`, `gap`, `probes`, `probing`):
  while a program rests the capture is aimed at it for `kProbeFor` (1 s) every `kProbeEvery` (5 s
  after the last look ended); after `kQuickProbes` (12) empty looks the gap grows by half each time
  to `kProbeEveryMost` (15 s). An empty look goes back to no window in the same tick (`rested_now`,
  no extra quarter second). A picture during a look ends the rest. Looks are not logged; the rest's
  two lines are logged once a rest (`rest_said_`). A look is not a game for *No game: Standby
  Image*. A look that comes while another game is chosen is put off a gap.
- **Ends at once:** a title the program never showed while the capture was aimed at it (`Tried`,
  per program while open, `title_shape`, at most `kTriedMost` 64 kept; `kTitleTries` 8 rests ended
  so while it is open), or a click back in (`away`), at any moment of the rest. That try gets the
  whole patience (10 s), as the first does.
- **Measured** (`[auto capture: a program rests for real ...]`, `STAGEKIT_TEST_ONLY_AUTO_REST`,
  about two minutes, all passed): aimed at the resting program 9.6 s in a minute, 9 looks (beta183:
  60 s; beta184: 0); a look 1.05 to 1.1 s; a game started 1.7 s into a rest picked up after 3.7 s,
  just after a look after 5.4 s (the stand-in capture's `needs`: a picture only while aimed at the
  program); a new title 0.3 s into a rest tried at once and for the whole patience; a title tried
  before held the rest 5.3 s (to the next look); a stream of new titles ended eight rests in all,
  then held; the Ryujinx case (a picture at 0.7 of the patience) kept on the first try; nothing
  written to *Never capture*; *Never capture* ends a rest. Only that section was run here, not the
  full suite; the `[auto capture]` section's own rest check (a new title tried at once) was not run.
- **Not known:** how many hook tries OBS's Game Capture makes in a 1 s look at the owner's hook
  rate (their log showed one every 0.42 s); not tried on Windows.

## The Update button lists what changed since your version (0.1.0-beta185)

Branch `fix/auto-capture-window-closed`, on `main` after beta186. Notes:
`docs/releases/0.1.0-beta190.md`. No Stream Deck file changed. The owner (2026-11-18): "if the
captured game closes but the emulator stays open in the menu/game list, the last captured frame
of the game remains in the OBS capture, and it doesn't morph into the standby as intended".
`fix/auto-capture-quick-pickup` changes the rest logic in the same file at the same time; this
change keeps to its own lines (a method of its own, `watch_captured`, the candidate lines, the
first line of `resting`), so the two should merge with only the notes and this file to sort out.

- **Cause:** OBS's Game Capture hooks the program; when Dolphin's game window closes and the menu
  stays, the hook keeps the last texture, the capture keeps its size, and the Standby Picture,
  which goes by the capture's own size, never stands in.
- **Fix:** Auto Capture remembers the game's windows (handles): those of the program opened
  since the capture was aimed at it (else the one in front or aimed at by title). All gone from
  the window watch's list for 0.5 s with the program open (and on Windows not merely minimised),
  the capture is aimed at no window and the program's windows seen meanwhile are no game until
  it closes; a window opened later is aimed at as usual. A window that opens within that time
  is followed, with no re-aim. See AGENTS.md's entry.
- **Review fixes (same commit):** only windows seen while the game was open are spent (a game
  restarted within the wait was shut out); the game's windows are found by what opened since the
  aim, not by focus (the menu was watched when OBS was in front; a GBA window closing let go);
  the watch is forgotten while the filter is off; a minimised spent emulator stays spent; its
  own log line; the README's fall-back line.
- **Tests:** `[auto capture: the game's window closes, its program stays open]`
  (`STAGEKIT_TEST_ONLY_AUTO_WINCLOSE`, also run after the rest section in the full run), passed
  twice; `STAGEKIT_TEST_ONLY_AUTO_REST` and `STAGEKIT_TEST_ONLY_CAPTURE_HIDE` still pass. The
  full `tests/run-tests.sh` was not run (machine limits).
- **Not proven:** that OBS frees the hook's last frame when the capture is aimed at no window
  (it does stop the capture on a change of window); nothing here has run on Windows.

## Scene Shadow: no flicker when items change, no gap at smoothed edges (0.1.0-beta186)

## Keys name their filter; a Standby Image over any source (0.1.0-beta189, plugin 0.30.0.0)

Branch `feat/key-filter-names-standby-cover`, on `main` after beta182. Notes:
`docs/releases/0.1.0-beta189.md`. The owner: "'Set its source to Square Camera and its filter to
Show in Scenes-Camera.' - i can't set a filter for show in scenes. there is no option. The stream
deck keys don't really say what filters they are associated with ... also, The Hide shows standby
image I think should have its own filter, or be separate from that automatic window switcher
somehow". Then: "ONE Show in Scenes filter where each Stream Deck key chooses".

- **Keys name their filter**: `targetItems` (`streamdeck/src/kinds.ts`) always "<source> —
  <filter>", the kind in brackets for a renamed filter; `modeFilterItems` (the Scene Mode key's
  *Scene and filter*, beta181's filter list) always names the Scene Modes filter. Lists renamed *Source and filter* / *Scene and filter* / *Capture and filter* / *Switcher
  filter*, each with a tooltip and a line naming the kind (`target_list` in `make-plugin-files.py`).
  Values unchanged.
- **OBS side**: `StandbyCover`, `GetStandbyCover`, `ShowInScenesPress` vendor requests and the
  `StandbyCoverChanged` event; Show in Scenes' *Hide shows Standby Image* (`hide_standby`); see
  AGENTS, *A Standby Image covers any source*.
- **Stream Deck**: Standby Image key *On press* + *Cover: toggle* / *Cover with image* / *Uncover*
  (lit while covered, `standbycover` event); Show in Scenes key *When hiding* (`hideAs`). VERSION
  0.30.0.0.
- Tests: `[standby image over any source: ...]` (`STAGEKIT_TEST_ONLY_STANDBY_COVER`, 7 checks;
  also run in the full run after the switch-command section); `streamdeck/tests` (kinds, titles,
  a Standby Image key and a Show in Scenes key against the stand-in server). The full
  `tests/run-tests.sh` was not run in this session (machine limit); `STAGEKIT_TEST_ONLY_SWITCH_COMMAND`
  was run as a neighbour.
- **Show only what it hid** (second commit, after the owner's camera switch showed the hidden DSLR
  and the Only one link then hid Game Capture): `restore_hidden`, `hidden_record` (in
  `kOwnSettings`); see AGENTS, *Show in Scenes' Show only what it hid*. Test
  `STAGEKIT_TEST_ONLY_RESTORE_HIDDEN`, 5 checks, also in the full run after the standby-cover
  section.
- **Not proven**: Windows; a real Stream Deck (the line under the list is plain HTML in the
  settings page, not seen in Stream Deck); a real camera (the test's camera is a colour source).

## Filter names, tooltips, Mode types (0.1.0-beta182)

Branch `fix/scene-shadow-flicker`, rebased on `main` after beta176. Notes: `docs/releases/0.1.0-beta186.md`.
No Stream Deck file changed. The owner: "shadows flicker when transitions happen" ("scene
shadow. none of the sources have their own shadow"; "when i switch/show or hide sources inside a
scene"), and "text with the scene shadow has a noticeable separation from the shadow".

- **Flicker, cause:** the item Morph draws a pair through a layer item it adds to the scene
  (`start_job` in `src/transition/item-morph.cpp`), and the two items' own transitions draw
  nothing while *Paired*. Scene Shadow looked at the scene's items only four times a second
  (`SceneShadow::tick`, `kLookEvery`), and an item it had not looked at was "masked, casts
  nothing" (`flags_of` in `SceneShadow::render`). So for up to a quarter second at the start of
  every item Morph the pair cast no shadow. Measured headless, drawing the scene through the
  change: 362.4 (sum of the shadow's darkening) before, 155.8 (only the chat column's) for
  100 to 200 ms from the layer's first frame, back to 362 after the next look; three tries
  of three failed or blinked, with and without a glide beside it.
- **Flicker, fix:** `has_new_items` in the tick: whenever `scene_revision()` has changed while
  the scene is in view, the scene's item ids are compared with the flags, and a new one makes
  the look due in that tick. OBS runs tick callbacks before source ticks, so the layer (and
  any item added from a tick callback or between frames) is looked at before the first frame
  that draws it. `flags_of` also gives an unlooked-at Morph layer *casts* outright. Cost: an
  id list of the scene per scene change while in view (every frame while something glides),
  no `classify` unless an id is new.
- **Not flickering headless** (before and after): Fade and Slide item transitions (worst 0.97
  of the frames round it), a Fit to Space-like glide during them, a change of scene with Morph
  between two shadowed scenes (1.00), and a scene coming into view with a source added while
  out of view (classified at once on coming into view, as before).
- **Gap, cause:** the shader (`data/effects/stagekit-scene-shadow.effect`) shadowed the whole
  finished pixel by `sa * (1 - mask.a)`. At a smoothed edge with coverage a, what is under the
  source is (1 - a) of the pixel and should be shadowed by `sa`; it got `sa * (1 - a)`, so
  `bg * a * (1 - a) * sa` too much background stayed: a 1 px bright line. Test: an
  antialiased dark grey disc on orange, hard black shadow (8, 8): red 139 at edge pixels whose
  shadow comes from the disc's inside, against at most 66 (the disc 60, the shadow beyond);
  73 of 73 edge pixels too bright.
- **Gap, fix:** `split_at` splits each pixel into the sources' part (M as drawn,
  premultiplied, to linear light) and what is under them (the scene less that, held to the
  uncovered share); shadow and outline go on the second only (`laid_under`). After: 0 of 73;
  the shadow-free side of the edge and the inside unchanged (0 off). Morph's carried shadow
  and Scene Blur's `lay_over` go through the same `compose`, and so does beta176's *Each
  source by itself* (`compose_layers` calls `compose` per layer; a later layer's picture is
  straight alpha, which `scene_linear` already reads through `image_straight`).
- **Each source by itself, the masks' colours** (done in the rebase onto beta176): a layer's
  mask was the mask above it copied first and the layer's own sources drawn over it, so where a
  lower source showed through a see-through edge of a higher one the mask's colours were the
  lower one's. Alpha was all that counted before; `split_at` reads the colours too. Now the
  layer's sources are drawn first and the mask above laid over them
  (`LookPaint::copy_onto(..., over = true)`), in `SceneShadow::render_layers` and Morph's
  `draw_layers`. The flicker fix needs nothing more there: `render_layers` uses the same
  `flags_of`, and the tick's look is the same for both modes.
- Tests: `[scene shadow: no flicker through transitions]` (`shadow_flicker_test`; Morph, Fade,
  Slide, Morph and Fade with a glide, three tries each; a source added from a tick callback;
  a change of scene with Morph both ways twice; a scene coming into view) and `[scene shadow:
  no gap at a smoothed edge]` (`shadow_edge_test`, each check for *The scene as drawn* and for
  *Each source by itself*, the disc a layer above a small block's), alone with
  `STAGEKIT_TEST_ONLY_SHADOW_FLICKER`, and last in a full run (they need sound).
- **The flicker test goes by OBS's video thread** (after the rebase onto beta181, where the
  release queue's full run failed once: *Fade, the chat column gliding (try 2)*, 245.3
  against 362.4 for one frame at 168 ms, before the change was even made). Not a fault of the
  filter: the test drew the scene from its own thread, by the wall clock, while OBS's video
  thread ticked and drew. Three ways that showed what OBS never shows: the chat column was
  moved from the test's thread, and OBS 30 works an item's matrix out in place step by step
  (`update_item_transform` from `obs_sceneitem_set_pos`, no lock), so a draw could read it
  half-made; a draw made while the sources ticked found an item transition's textures locked
  by `obs_transition_tick` (`trylock_textures` fails, a hiding item is then drawn as nothing);
  and a draw waiting for the graphics behind a lagging video thread carried a stale time. On
  top, with few draws a frame (a busy machine) the twelve draws each one was compared with
  reached so far that the shadow's own fall and rise read as a blink: 24 gliding tries
  failed in six runs of the old test with two busy loops (of 36), all of that kind. Now the changes
  are made from a tick callback (as Fit to Space and the item Morph make them), every draw
  measured is made in a main-rendered callback (twice a frame, the second from the frame's
  cache), times are video times, and a draw is compared with those within 100 ms each way
  (at least the next frame each way); each case also needs three frames drawn while the change
  goes on. Still catches the bug it was written for: with `has_new_items` and the layer's
  *casts* taken out, Morph fails at 155.8 against 362.4 as before.
- **A crash found on the way:** with the gate run alone 30-odd times, 4 runs ended in a
  segfault after the last check (none seen in the queue's full run). gdb: the video thread in
  `SceneShadow::render` calling `obs_source_skip_video_filter` after `obs_source_filter_remove`
  had cleared the filter's parent from the test's thread mid-draw (the log's
  `obs_source_enum_filters: Null 'source' parameter` just before); OBS's skip reads
  `parent->info` unchecked. The same can happen in OBS when a Scene Shadow is removed in the
  Filters window while its scene is drawn. Scene Shadow now gives up through
  `skip_filter_input` (`src/common/filter-input.hpp`), which skips only while the filter still
  has a parent and target. Other filters still call OBS's skip directly; most do so at once,
  Scene Blur after a long draw as Scene Shadow did.
- **Not proven:** Windows and Direct3D (the effect now has a struct returned from a function,
  as `VSDefault` already does); the owner's own scenes; text from a real text source (the
  test's disc is drawn by Qt with antialiasing).

## Scene Shadow from each source, Scene Corners on the whole scene (0.1.0-beta176)

Branch `fix/auto-capture-hook-loop`, on `main` after beta180. Notes:
`docs/releases/0.1.0-beta184.md`. No Stream Deck file changed.

- **Evidence (the owner's logs, 2026-11-18):** Auto Capture aimed a Game Capture at Dolphin.exe;
  OBS printed `attempting to hook process: Dolphin.exe` / `capture stopped` about every 0.42 s
  (78 times in 90 s, hundreds a session). The rest logged *got no picture ... leaving it alone*
  and, 0.27 s later, *trying Dolphin.exe again (its window's title changed)*.
- **Causes, `src/capture/auto-capture.cpp`:** with nothing else chosen the capture was left aimed
  at the resting program (or at *any full-screen application*, which hooks a full-screen emulator
  too); and the rest ended as soon as any window of the program had a title other than the one
  given up on (a frame rate, a GBA window's volume, the GBA windows themselves).
- **Fix:** while a program rests and nothing else is chosen, the Game Capture is aimed at no
  window (`kNoWindow`, `capture_mode` window, a class and executable nothing has) and aimed back
  when the rest ends. A rest ignores titles seen at its start and in its first `give_up_s_`
  (`titles`, `title_shape`: digits out); a new title after that ends it, three times at most while
  the program stays open. Clicking back in counts only when no window of the program was in front,
  and not in that first stretch. A program put on *Never capture* while it rests is no longer
  rested (`rest_.erase`), so the full-screen fallback comes back (a review finding). Nothing is
  written to the user's lists; *Always capture* programs are never rested.
- **Tests:** `[auto capture: a program rests for real, and is aimed at again after]`
  (`auto_rest_test`, alone with `STAGEKIT_TEST_ONLY_AUTO_REST`, also run inside the auto capture
  section; the older check of that section now expects no window, not any full-screen, during a
  rest). `register_stand_in_capture` registers a kind once only.
- **Not seen on Windows:** that OBS's Game Capture, aimed at a window that does not exist, only
  looks for it and logs nothing (believed from OBS's `try_hook`, which returns when no window is
  found).

## A capture left in use after a cut part-way through a change (0.1.0-beta180)

Branch `fix/stale-transition-flake`, on `main` after beta174. Notes:
`docs/releases/0.1.0-beta180.md`. No Stream Deck file changed. The check *a scene left with a cut
part-way through a change: back on it, the capture whose change was cut off is not in use* (capture
scenes section, the chain of two scenes) failed in about one full run in twenty, only under load.

- **Cause (plugin side), `set_item_visible` in `src/common/item-transition.cpp`:** the second
  capture (Chain B) had a hide transition in Chain Full left under way, still holding the capture,
  from the threads loop just before (no item transition ends in the tests before they turn sound
  on). OBS stops an item's other-way transition at a change but only signals it, so the transition
  kept the capture. Shown and then hidden again out of view, the item carried that transition as a
  hide transition under way on a hidden item. OBS counts that in when the scene comes on, and
  `end_stale_transition` takes it for a change being made now. Link Visibility's half-second look
  (`stale_timer_`) ended it only if it fell in the roughly 480 ms the capture was shown in between.
  Evidence: the failing log lacks `ended a transition left under way on 'Chain B'` in Chain Full,
  which every passing log has (40 of 40). The diagnosis showed the hide transition `active 1` holding
  `Chain B`. With the press 60 ms after the scene is left, 8 runs in 10 failed, and the 2 passes
  were the 2 runs whose look fell in the window.
- **Fix:** after every change `set_item_visible` makes (in view and out of view, not for an owned
  item), the other way's transition is ended if it still holds a source (`end_held_change`, the old
  tail of `end_stale_transition`, which now calls it). This way's transition is *not* ended while
  it is off the item out of view. That was tried first, and OBS's counts then went wrong for good.
- **libobs 30.0.2 (the test machine) miscounts by itself.** Its `scene_enum_sources` is
  `if / else if / else if`: while an item's transition runs, the item's own source is left out when
  the scene is switched on or off, but `set_visibility` and `add/remove_active_child` count it in and
  out. A scene left part-way through a change leaves its source counted in use for good, and in the
  old runs that was cancelled by whichever leftover transitions Link Visibility happened to end. 31.1.1
  (`buildspec.json`) and later enumerate both. So the test now checks what the plugin leaves behind
  (no item transition of the capture counted in and holding it) on every version. It checks
  `in_use` only from libobs 31 (`obs_get_version()`). The press now comes 60 ms after the scene is
  left (it was 400 ms), so the old fault fails 8 runs in 10, no longer 1 in 20.
- **Test:** the chain is a function (`capture_chain_test`), run where it was and alone with
  `STAGEKIT_TEST_ONLY_CAPTURE_CHAIN`. Under load (two busy loops on four cores): old plugin with the
  old test 19 of 20 passed; old plugin with the new test 2 of 10; fixed plugin with the new test
  20 of 20. The sections `STAGEKIT_TEST_ONLY_MORPH_HOLDS`, `_LINK_TRANS`, `_LINK_FOLLOW`,
  `_SWITCH_COMMAND`, `_POP_IN` and `_MORPH` pass. The full run was not made here (the release queue
  was using the machine): the rest of the capture scenes section and the layout section have no gate.
- Link Visibility's log line now names the scene: `ended a transition left under way on '<source>'
  in '<scene>'`.
- Not seen on Windows.

## Show in Scenes: a switch is a command, not a hold (0.1.0-beta174)

Branch `feat/switch-is-command`, on `main` after beta169. Notes: `docs/releases/0.1.0-beta174.md`.
No Stream Deck file changed. The owner's DSLR has a Show in Scenes capture switch (`Show in
Scenes-Capture`, every scene, *Follow what is shown* on) that reads off whenever another capture is
the main one. In Holding-Scene a Link Visibility in *Follow sources* (lead Intermission Text,
follower DSLR) would not show DSLR, because `show_in_scenes_holds_off` said the switch held it off.
The owner: "show in scenes should not prevent any other filter from turning on or off a source
later, at least thats how my logic works. for me the show in scenes was more of a control to turn a
source on or off, not lock it down and prevent anything else from doing it".

- **Removed:** `show_in_scenes_holds_off` (`src/placement/show-in-scenes.*`), with the registry of
  switches it read (`g_switches`, `g_switches_mutex`, `SwitchLook`, `ShowInScenes::look`), and
  its two callers: Link Visibility's *Follow sources* show (`visibility-link.cpp`, a follower is
  now always shown when the leads come on) and Scene Modes' `held_elsewhere` (a mode's show).
  Standby's `stands_in` never asked it. Nothing else did.
- **Kept:** a press shows or hides its sources in the scenes it covers; *Follow what is shown*;
  Show in Scenes passing over an item `mode_holds_hidden` holds, and waiting on `mode_settling`.
  That is a mode holding a source hidden, not a switch holding it, and the owner's rule is about
  the switch; a press still never shows a camera view in a mode not showing.
- **Consequence:** a camera switched off with its switch comes back when a mode or a link shows it.
  To keep it off there it must be taken out of that mode or link, or the switch pressed off again.
- Tests: `[scene modes: camera switch]` (both orders) and `[modes in one scene: follow the first
  source, and a camera switch]` now expect the mode to show the camera with the switch off (the
  owner quoted beside each); new `[show in scenes: a switch is a command, not a hold]`
  (`switch_is_command_test`, alone: `STAGEKIT_TEST_ONLY_SWITCH_COMMAND`; also run at the end of
  `STAGEKIT_TEST_ONLY_LINK_FOLLOW`): the owner's case, then the switch pressed on and off as a
  command, then the lead hidden with the follower hidden again when shown by hand.
- **Not proven:** Windows; the owner's own collection.

## Scene Fit groups: Same for all sources (0.1.0-beta173)

Branch `feat/scene-fit-groups`, on `main` after beta166. Notes: `docs/releases/0.1.0-beta173.md`.
No Stream Deck file changed. The owner: "I'd like the Fit to scene filter also have the option to
select one or more sources and specify the priority for all the selected sources together. that
way i can have multiple fit to filters with similar settings or priorities instead of multiple for
each source."

- **Scene Fit's *Settings*** (`settings_mode`: 0 *Each source its own*, the default and every saved
  filter; 1 *Same for all sources*). In 1 the window shows one group, *Settings for all sources*
  (`group|...`, lists `group_skip` / `group_share_scenes`, the shared `fit_add_controls`), and
  hides *Settings for*; every entry reads how it fits from that set (`FitEngine`'s new
  `look_prefix` / `set_look`), and what it writes for itself (home, kept size) stays under
  `fit|<source>|`. AGENTS, Scene Fit entry.
- **Switching moves nothing**: to 1 the shared set is the first entry's; back to 0 every entry gets
  the shared set (`settle_mode`, once per change, marked by `settings_mode_was`; in the window's
  callback or in `update`, never as OBS loads the filter).
- **Groups** are several Scene Fits in one scene, each on 1. Nothing new in the claims: each
  entry's claim already carried its own `Rank`, so a group on *Takes space first* outranks a group
  on *Makes room* one-way, and *Priority by list order* on both goes by filter position; entries of
  one group share a rank and solve together in the multi-pass as before. Checked `set_claims_firm`,
  `above_` and `kPasses`: no change needed.
- **Take over Fit to Space**: into an empty Scene Fit on 0, two or more Fit to Space filters with
  the same settings (defaults included, `same_look`) make it 1 (logged); different ones leave it 0.
  Into a Scene Fit on 1 the sources take its shared set. Chosen over offering a choice: OBS's
  window has no question box, and the result is the same either way (each entry keeps its own copy).
- A rename follows into `group_skip` too.
- Tests: `[scene fit groups: the same settings for all sources]` (`scene_fit_groups_test`, alone:
  `STAGEKIT_TEST_ONLY_FIT_GROUPS`): three sources and one shared change; switching in the window
  and from outside; two groups, by level both ways and both filter orders, and by list order both
  orders; a Scene Fit saved before the setting; Take over, alike and different.
- **Seen, not new**: "its sources did not settle in 4 passes" is logged for three resized sources
  sharing space in one Scene Fit with *Each source its own* too (the test's first layout); they
  still settle a moment later. Within a group sharing, where two sharing sources come to rest after
  the other group was hidden and shown can differ from before (they go by who looks first), as with
  two Fit to Space filters that share.
- **Not proven**: OBS's own window on Windows (the group shown and hidden, the lists' rows rebuilt
  when the mode changes).

## Ambient Background's mosaic on Direct3D (0.1.0-beta172)

Branch `fix/scene-blur-off-draw`, from `main` at beta170. Notes: `docs/releases/0.1.0-beta172.md`.
No Stream Deck file changed. The owner, with screenshots of Full-Capture-Scene and
Component-Capture-Scene (black background, no shadows): "ambient background does not show unless
i turn on scene blur. it remains visible after turning blur off"; then, Scene Blur removed and
Scene Shadow off, still black after a restart; then "turning off mosaic brought it back".

- **Cause**: `AmbientBackground::render` (`src/ambient/ambient-background.cpp`, the `Mosaic`
  pass) set every uniform of `stagekit-looks.effect`'s Mosaic technique but `whole`,
  `open_edge` and `fallback`, added for Scene Blur in beta165 (`whole`) and beta167 (the other
  two). Direct3D refuses a draw while a uniform of its shader has never had a value ("Not all
  shader parameters were set", in OBS's log as `device_draw (D3D11)`), so the mosaic drew
  nothing. OBS loads the effect file once for everyone and keeps the last value given to each
  shader uniform; once a Blur or Scene Blur had drawn a mosaic, the values were there and the
  background drew from then on, with whatever values the blur had left. Scene Shadow was not at
  fault: its shadows fall on the background and do not show on black. Not seen on Windows
  here; the reasoning comes from libobs's effect code and the owner's reports, and their log
  should hold the `Not all shader parameters were set` lines.
- **Fix**: `src/common/mosaic-uniforms.hpp` (`MosaicLook`, `MosaicUniforms`) sets every uniform
  the Mosaic technique reads; `BlurPipeline::run` and Ambient Background both go through it.
- **Test**: `[Ambient Background's mosaic sets every uniform of the shared effect]`
  (`STAGEKIT_TEST_ONLY_BLUR_OFF_DRAW`, also in the full run): the shared effect is drawn with
  other values first (whole cells, a mask, open sides), then the background must look as it did.
  Before the fix the hexagon case was 0.061 off, after 0.000. The square case passed both ways
  (its cells meet the edges evenly). OpenGL starts uniforms at zero, so the black itself cannot
  be shown headless.
- **Not done**: the row of dots along the game's bottom edge in the owner's Scene Blur
  screenshot (*The scene as drawn*, hexagons) is a separate matter, not looked into here.


## Scene Transitions: no write while a scene's sources load (0.1.0-beta171)

The check *a scene loaded again with its transitions saved is written nothing* failed now and then
(one write; seen in the frame-time work and on `feat/scene-fit-groups`). Scene Transitions acted on
`item_add` and on a scene first met at its next tick, while OBS was still setting the saved
transitions of the items it had just added. Both now wait `kAddedNs` (0.1 s). During OBS's own
start `scenes_loaded()` already held it off; it mattered for a scene loaded by itself.

## The item Morph lets go of scenes and sources (0.1.0-beta170)

Branch `fix/item-morph-holds`, from `main` at beta167. Notes: `docs/releases/0.1.0-beta170.md`.
No Stream Deck file changed. Found by a helper writing a Link Visibility test: with Morph on the
captures, the section's scenes and sources were still there at `obs_shutdown` ("source(s) were
remaining", "Double destroy"), and not with a fade. The owner uses Morph as the show and hide
transition of their captures.

- **Cause**: `Entry` in `src/transition/item-morph.cpp` held its scene and item (strong refs),
  released only when the transition said it stopped, started again or was destroyed, or after
  `kTooLongNs` (20 s). OBS says a transition stopped only when it is drawn to its end with its
  sound ended, which a scene out of view never is (and the tests have no sound there). The held
  scene kept its items, they their sources and their Morph transitions, and the transition's
  destruction is what would have ended the entry: a cycle broken only by the 20 s. At a change of
  collection or exit within that time, OBS found them held; `obs_free_data` then destroyed them by
  force and destroyed them again as the scene let go of its items ("Double destroy").
  `item_morph_item` handed the held item to the transition as it drew.
- **Fix**: entries keep a weak scene and the item's id; the tick finds each item afresh outside
  `g_mutex` and lets go of it at the end of the tick (`Held`, `reach`). The nearest edge is worked
  out when the entry is made (`item_morph_edge` replaces `item_morph_item`). A `Job` keeps a weak
  scene (its items and sources are held only while the change is drawn), ends when its scene is
  removed, and `end_job` clears the layer's hold on it. `let_go_all` ends every change and drops
  every entry: at `SCENE_COLLECTION_CHANGING` and `SCENE_COLLECTION_CLEANUP`
  (`item_morph_let_go`), in a tick while `!scenes_loaded()`, and in `item_morph_stop()`.
- **Counts** (the new section alone): before, every case failed, with each case's scene, its
  three sources and six transitions still held after the test let go, "12" and "27 source(s)
  were remaining" and 39 "Double destroy" at shutdown; `STAGEKIT_TEST_ONLY_LINK_TRANS` left its
  Morph scene and three sources ("2 source(s) were remaining" and 2 "Double destroy"; 3 + 8 and 11
  in the first run). After: nothing held in any case within 1.5 s, nothing remaining, no "Double
  destroy" in either.
- **Tests**: `[the item Morph lets go of scenes and sources]` (`STAGEKIT_TEST_ONLY_MORPH_HOLDS`,
  also run after the Link Visibility section in the full run): a change seen to its end, cut off
  part-way, left part-way out of view, its transitions replaced part-way, and a collection change
  part-way (`stagekit_test_loaded`); the last checks that the layer is gone and nothing is held
  while the collection changes. The Link Visibility section now checks the same at its end.
- **Not proven**: the frontend path (`SCENE_COLLECTION_CHANGING`/`CLEANUP` calling
  `item_morph_let_go`) has not run; the tests stand in with `stagekit_test_loaded`. Not tried on
  Windows. The Morph transition's own `fallback_` (another transition driving an unpaired item)
  still holds that item's source until the transition next starts or is destroyed; it is held by
  the item, so it goes with the scene, and the tests found nothing left from it.

## Link Visibility: Follow sources (0.1.0-beta169)

Branch `feat/link-follow-sources`, on `main` after beta168. Notes: `docs/releases/0.1.0-beta169.md`.
No Stream Deck file changed. The owner, with a screenshot of Component-Capture-Scene's filters (five
Link Visibility filters in *Follow the first source*, -ComponentGame, -ComponentWindow,
-ComponentCapture, -ComponentDesktop, -ComponentTablet, each a capture followed by *Column: Chat*,
Morph): "I have all these filters effectively just to make the Column chat follow all my main live
sources. I think the follow first source option should be changed to a follow source option, where
one or more sources can be added, then the linked sources can be added."

- Mode 2 is shown as **Follow sources** (still saved as 2). New settings: `leads` (PickList, *Lead
  sources*, empty as it comes) and `lead_rule` (0 *Any lead is shown*, default; 1 *All leads are
  shown*; append-only). In mode 2 `items` is shown as *Following sources*. The lead controls show in
  mode 2 only. AGENTS, *Link Visibility's Follow sources*, has the rules.
- **Old filters:** `leads` empty means the first entry of `items` leads the rest, as before; nothing
  is rewritten. *Make the first a lead* (`take_first`, shown only for such a filter) moves it on a
  press.
- `link_visibility_holds_hidden` / `link_visibility_settling` go by `leads_say_shown`: a follower is
  held hidden when every link listing it as a follower has its leads saying hidden.
- **For the owner, five filters to one**: add a Link Visibility to Component-Capture-Scene, *Follow
  sources*, the five captures under *Lead sources*, *Column: Chat* under *Following sources*,
  Morph as before; switch the five old filters off, try every capture key, then remove them. The
  release notes give the steps in plain words. The merge scripts still write the one-filter-per-mode
  form, which keeps working.
- Tests: `[link visibility: follow sources, several leads and their followers]` (alone:
  `STAGEKIT_TEST_ONLY_LINK_FOLLOW`), last in the full run because it turns sound on: the window,
  then both filter orders on and off screen (five leads and a chat follower, three capture changes
  by Show in Scenes key with *Show only one* and Morph, two changes in one frame both ways, every
  lead hidden, a Show in Scenes press passed over, *All leads are shown*), and a filter saved
  without leads, with *Make the first a lead*. valgrind clean on the section.
- **Seen in the tests, not changed:** with Morph as the captures' transition, scenes and sources of
  the section are still held when OBS shuts down (18 and 36 remaining, "Double destroy" lines);
  with a fade nothing is left. The same shows on `STAGEKIT_TEST_ONLY_LINK_TRANS`. It points at the
  item Morph holding its scenes; not looked into further.
- **Not proven:** Windows; the owner's own collection.

## Stream and Record keys (0.1.0-beta168, plugin 0.24.0.0)

Branch `feat/sd-stream-record`, on `main` after beta166. Stream Deck only; no OBS plugin file
changed. The owner: "Id like animated stream Start buttons and stream Record buttons. both should
be able to be set to toggle, start, pause or stop, and have animations that are relevant, and
match the existing color themes", and then "the new keys also need a long press setting".

- **Two keys** (`.stream`, `.record`; `src/actions/output.ts`), on obs-websocket's own requests
  and events, as the Switch Scene key. Stream: Toggle / Start / Stop. Record: Toggle / Start /
  Stop / Pause / resume / Pause / Resume. **A stream cannot be paused**: OBS and obs-websocket
  have no such thing, so the Stream key has no Pause (tooltip, README). Tell the owner.
- **Animated** by cycling frames with `setImage` (not a GIF: the SDK documents PNG, JPEG and SVG
  for `setImage`, GIF only for the manifest). 70 frames a palette by `tools/make-live-icons.py`
  (stream: idle 1, starting 8, live 8, reconnecting 8, stopping 8; record: idle 1, starting 8,
  recording 8, paused 12, stopping 8), 8 a second, one shared timer, at most 12 keys a tick,
  only while the key is on the deck and only in moving states; *Animate* Off for still images.
  The record dot is red in every palette; the rest follows the palette.
- **Long press** (`src/hold.ts`, reusable): *Long press* Nothing (as it comes, acts on key down)
  or any *On press* choice; *Hold time* 800 ms as it comes. A bar in the palette's accent fills
  while held; the title says *Hold: Stop*.
- Titles follow the press and the state, with the running time (*Show time*). A Scene type list
  (with its Layout list), as every key has: with a type, the type with a still state badge.
- Tests: `npm test` 73 of 73 (9 new in `tests/output.test.ts`: the requests, the events, the
  frames in every palette, the animator, the long press, the titles), `npm run validate`.
- **Not proven**: on a real Stream Deck (frame rate, how the bar and the animation look, whether
  a hand-set image wins over the frames) and against a real OBS (the exact order of its state
  events; the stand-in sends STARTING then STARTED).

## Scene Blur: the screen's edge, Mosaic edge's mode, no gap (0.1.0-beta167)

Branch `fix/blur-edges-2`, from `main` after beta166. Notes: `docs/releases/0.1.0-beta167.md`.
No Stream Deck file changed. The owner, of beta165, with screenshots of their
Component-Capture-Scene (a full-screen Ambient Background, a game capture with a Shadow, Glow &
Outline, a camera, a chat panel, a Scene Shadow; Scene Blur on hexagons, all or live sources):
"Whole cells blur also blurs the entire scene border. If whole cells is selected then the mode is
switched back to whole scene as drawn, the whole scene edge retains the border/blur. scene as
draw, causes a visual gap between the shadow, outline/glow effect."

- **Open sides**: with *Each source by itself*, a side of a source at or past the screen's edge is
  no edge for whole cells (`BlurLook::open_edges`, `open_edge` uniform, set in `blurred_item` from
  the item's draw transform). A full-screen background keeps all its cells; a source running off
  the left keeps its cells there (252 of 252 edge pixels) and its top edge inside the screen is
  still whole cells (11 rows of spread). Background sources were not left out of whole cells
  altogether: open sides cover them and also a source that runs off one side only.
  The Blur filter on a source cannot know where it is on screen and still cuts there (README).
- **Mosaic edge only with Each source by itself**: `update` clears `whole_cells` otherwise.
  Tested: *The scene as drawn* with *Whole cells* left on is identical to *Cut at the edge*
  (0.00 off), nothing see-through.
- **The scene as drawn is kept to the mask**: the picture blurred has its alpha times the mask
  (`keep_to_mask`), every pass averages colour by alpha, and the composite takes the blur's
  colour with the scene's own alpha (`keep_alpha`). A mosaic cell that only just reaches in looks
  on a ring, then at the pixel itself. Gaussian along the top edge: 25.57 off the Blur filter on
  the source before, 0.23 now; hexagon edge cells: most blue 255 before, 194 now (the source's
  own at most 200). The existing `[scene blur]` expectations did not rely on the bleed.
- **Not changed, and why**: with *All sources* the background is covered, so cells along a
  source's edge still mix it with the background (as drawn). Keeping each source apart within the
  covered area would need a per-pixel source id in every pass (not separable for the soft blurs);
  that is what *Each source by itself* is for. Said in the README and the notes.
- **Not proven**: Direct3D for all three shader changes; the owner's scene itself (not
  reproduced here: the test scene stands in for it).

## Layouts on the Stream Deck keys (0.1.0-beta166, plugin 0.23.1.0)

The owner: "every icon should have a standalone version, live/no chat version, and a
component/chat version", then "perhaps have a selector to choose the layout and a second selector
for the specific type". Every key with a Source type or Scene type list has a *Layout* list just
before it (setting `layout`: `auto` Automatic, `standalone`, `live` Live (no chat), `chat` Live +
chat). Automatic is what every key showed before, so no key changes on update. The two layouts are
the pack's game capture and game component scenes with their gamepad taken out; each type's drawing
goes in the panel (`tools/make-option-icons.py`, `layout_pictures`): 100 drawings, 202 pictures a
palette with the two frames, in all six palettes, marked on and off, SVGs too. An image with no
drawing of its own is laid smaller into the panel when the key is shown (`src/compose.ts`).

After the owner saw the first contact sheet ("they should all be consistent ... any app type should
not have a specific letter ... make them all centered. fix the icons universally"): one template
per layout for every type (game, window and capture card no longer use the pack's 02/03, 05/06,
07/08, whose camera is elsewhere); the 3D, sculpting and CAD app types are drawings, not the pack's
lettered app pictures (their show, shadow and blur art dropped with them); and every drawing the
tools make is centred by its ink, on keys, in layouts and on badges (before, every key drawing sat
7.5 px left of the frame's centre). This changes how every drawn key looks, not only the layouts. AGENTS, *Layout*, under *Every key chooses
its image from type lists*. Stream Deck plugin `VERSION` 0.23.1.0. Tests: `npm test` 64 of 64 (9 new
in `tests/layouts.test.ts`), `npm run validate`. Not tried on a Stream Deck.

## Scene Blur's blur area and whole mosaic cells (0.1.0-beta165)

Branch `feat/blur-edges`, on `main` after beta164. Notes: `docs/releases/0.1.0-beta165.md`. No
Stream Deck file changed. The owner, with screenshots of a window capture as a hexagon mosaic under
a Blur filter and under Scene Blur: "I'd like if there was a configuration to have the shadows or
other applied strokes, glows or filters in general take on the visible pixel shape, so when a
screen is blurred it can take on the pixel shape for example. also, i want a configuration option
to keep scene filters within the bounds of a source, or overlay the whole source ... the scene
blur filter doesn't seem to go all the way to the edges".

- **Scene Blur's *Blur area*** (`blur_area`: 0 *The scene as drawn*, as before and the default;
  1 *Each source by itself*). In 1 each covered source is blurred from its own picture with the
  Blur filter's own passes and drawn in its place in a redraw of the scene, laid over the scene
  inside the covered sources' boxes (AGENTS, Scene Blur, *Blur area*). Tested equal to a Blur
  filter on the source (0.00 off, edges included, hexagons, Gaussian and whole cells); the scene
  as drawn is 40.7 off along the top edge, the light-blue band of the owner's screenshot.
- **Mosaic edge** (`pixel_edge`: 0 *Cut at the edge*, default; 1 *Whole cells*), in the shared
  blur look: cells the picture's edge cuts are left out, never drawn past it (AGENTS, the mosaic
  entry). Every shape, a turned pattern too; Voronoi cells are judged by their neighbours.
- **Shadows follow**: a Shadow, Glow & Outline after the Blur filter reads alpha and follows the
  cells by itself (tested). Scene Shadow draws a source a Scene Blur blurs by itself from that
  blurred picture (`scene_blur_item_picture`, made once a frame for both filters), and Scene Blur
  lays the frame's shadow over its redraw (`scene_shadow_lay_over`), so the outline follows the
  cells inside the source's box too (tested).
- The mosaic's nine taps are a loop now (same result; a shorter shader for Direct3D to compile).
- Tests: `[blur edges: ...]` (alone: `STAGEKIT_TEST_ONLY_BLUR_EDGES`), `scene_blur_controls_test`
  extended; contact sheet `docs/images/blur-edges.png`.
- **Not proven**: Direct3D (the whole-cells code, the Voronoi neighbour test and the loop), the
  cost on the owner's scenes with many covered sources, and a source with a crop or a scale filter
  in *Each source by itself* (the redraw does not follow a crop to bounds, scale filters or
  blending modes, as Scene Blur's redraw before it).

**Seen once:** the first full run on `main` after beta164 failed `a scene left with a cut part-way
through a change: back on it, the capture whose change was cut off is not in use` (the capture still
in use 1.5 s after Link Visibility logged ending its transition, an item Morph between the same
captures under way), while another helper's image build loaded the machine. The next full run
passed. Nothing in this branch touches that path; if it comes back, look at the item Morph's layer
holding the source while Link Visibility ends the stale transition.

## Any image on any key (0.1.0-beta164)

The owner, with a screenshot of their holding-screen keys: "I need all my original Scene type icons
to exist in the types lists. for example, i can set my holding scenes to icons that match my
original icons layouts, but i can't set my original icon sets to any of the show in scenes or
others", then "I want to be able to set any combination". Every key's Source type list now offers
every source type and every scene type (`scene:<id>`), and every Scene type list every source type
(`source:<id>`), the key's own types first (AGENTS, *Any combination*). Corners & Edges got a Source
type list. Stream Deck plugin `VERSION` 0.22.0.0. Tests: `npm test` 55 of 55, `npm run validate`.
Not tried on a Stream Deck.

## Frame time, and a settings-window crash (0.1.0-beta163)

**On the owner's PC (2026-11-17, beta163/164):** "frametimes are better. averaging .8ms when idle with a capture" (about 1.1 ms before).

The owner: OBS's *Average time to render frame* about 1.1 ms with StageKit, under 1 ms before; their
logs had `tick_sources` median 0.25 to 0.45 ms with `render_video` unchanged (0.33 ms). Started again
from `release/beta162`; from the stopped `feat/frame-time` (beta155) the timing work (µs, the per-frame
line, `stagekit_timing_take`, `stagger_phase`, its test) and the harness were taken; the shadow's
scale check was taken with its own scene watch replaced by the shared `scene_revision()`; the WIP
`run-perf.sh` / `perf-compare.py` (old/trial/new collections side by side) were rewritten for the
synthetic collection. AGENTS' *Frame time* entry has the rules.

**Measured** (`tests/run-perf.sh`, synthetic collection at a quarter canvas, 900 frames a scene,
headless llvmpipe; StageKit's ticks in µs a frame without ticks of 1 ms or more, which are Fit to
Space's read-backs waiting for llvmpipe; before = `release/beta162` plus the timing only):

| Scene | before | after | change | OBS `tick_sources` median (ms) |
| --- | --- | --- | --- | --- |
| Live | 169.5 | 98.6 | −42 % | 0.230 → 0.194 |
| Chat Live | 163.3 | 106.8 | −35 % | 0.231 → 0.202 |
| Standby | 137.3 | 98.4 | −28 % | 0.202 → 0.195 |
| nothing on screen | 138.4 | 77.0 | −44 % | 0.186 → 0.158 |

Per kind, µs a tick (mean, Live): Link Visibility 2.87 → 0.72, Shadow, Glow & Outline 2.24 → 0.93,
Ambient Background 13.0 → 3.5, Show in Scenes 0.61 → 0.31, Standby 3.15 → 2.76; Fit to Space (not
changed) 0.97 → 0.9 without its read-backs, now the largest share (about a third). Per-frame p95 with
nothing on screen 623 → 127 µs (the staggering). These are llvmpipe's numbers on this machine: what
the owner's log shows is the measure that counts; ask for the `timing` lines and OBS's
`obs_graphics_thread` / `tick_sources` lines after they update.

**Cut, and why it is safe:** see AGENTS (*Frame time*): `scene_revision()` (every scene signal),
looks skipped when it has not moved, each with a slow look anyway; `scene_revision_bump()` from a
Link Visibility taking another state or switched off and from a Show in Scenes switch turned;
`refresh_item` no longer counts as a change unless the box changed; `shown_scale` measures only what
it needs; Link Visibility's settings shared, not copied each tick; Standby's strings copied only when
the image is made; `stagger_phase` for the timers touched.

**Found and fixed on the way (a behaviour change, called out in the notes):** a source with a shadow
in a bounding box (the synthetic captures in Live; most likely the owner's too) had its shadow's room
alternate between two sizes every scale check (34 and 35 px), moving every item showing it by a
pixel and back three times a second, in `release/beta162` as well (checked with that build). The
scale is no longer taken when it goes straight back to the one before by under 2 %.

**Tests:** `[frame time: looks only when something changed]` in the `scene-filters:perf` block (the
four looks counted through `stagekit_test_looks`, and that hiding, showing, a press and a move are
still followed at once); `[timing: µs and the per-frame line]` from the old branch.

**The crash was beta162's, not the cuts'.** Two more full runs here crashed again, and three
worker runs under gdb crashed twice: each time in `obs_source_properties` →
`obs_properties_apply_settings`, once in `[link visibility: ...]` (the Show in Scenes window with
Morph's *Unpaired sources* on a transition), and the first ones at the nearest-edge flips in `[show
in scenes]`, which open the same window. valgrind (`STAGEKIT_TEST_ONLY_LINK_TRANS=1`) showed it on
every run: `build` in `src/common/item-transition.cpp` took the window's controls off from inside
OBS's first round of modified callbacks, which then read the freed controls. The mark of what was
made (`change|sig|...`) was taken before the controls were made, and making them sets defaults
(Morph's own, an old Morph's *Unpaired sources*), so the mark did not match at the first callback.
The mark is now taken after everything is made. valgrind is clean on that section, on
`STAGEKIT_TEST_ONLY_FIT_PRIORITY` (Scene Fit's rebuild) and on `STAGEKIT_TEST_ONLY_MORPH`. In OBS
it could crash on opening the settings of a Show in Scenes or Link Visibility using Morph (more
likely with *Unpaired sources* set). Not seen on Windows; no report from the owner.
**A window that rebuilds its controls in place: run its section under valgrind.** A use-after-free
there crashes in one run of four or five.

**Not done:** Fit to Space (its idle and glance rules stand; at rest its off-screen tick is 0.2 µs and
one full tick in fifteen), Standby's 4 Hz stand-in look (a Scene Standby's list changes without a scene
signal), the window switchers, the new collection's own measurement (the generator does not exist
yet: `run-perf.sh` takes `label=file.json` for it). Not run on Windows.

## All sources, Space priority, Link Visibility's transitions (0.1.0-beta162)

Branch `release/beta162`, made from `feat/scene-blur` (beta161) with the three finished branches
below merged in this order, each a signed merge commit. Notes: `docs/releases/0.1.0-beta162.md`.
No Stream Deck file changed (plugin stays 0.21.0.0).

### How the three were put together

- **All sources and Space priority share the fit engine.** In `FitEngine::solve` All sources'
  `set_claims_firm` steps the place a space is looked for round out of another fit's claim; Space
  priority marks the claims of fits that take their space before this one (`above_`) and lets
  `free_space` look off each side of those (`firm`), keeping the biggest picture. Together: the
  firm-claims step passes over a claim that is `above_` and leaves it to `free_space`; a claim of a
  fit this one outranks is not in `boxes_` at all (`FitScan::obstacles` drops it), so neither step
  sees it. Between fits of the same standing All sources works exactly as it did on its branch.
- **An All sources entry has a Space priority.** The scene's sources that are not listed are
  entries like the listed ones (`fit|<source>|`), so each has its own *Space priority* under
  *Settings for* (default *Shares space*). With *Priority by list order* they come after the listed
  ones, from the top of the scene down (`unlisted_sources`, `set_entry_index`).
- Scene Fit's *Priority by list order* box uses the self-aware callback All sources gave the
  overlap switch (`obs_property_set_modified_callback2(..., chosen_modified, self)`), so the
  window shows the unlisted entries' groups too.
- Morph: Scene Blur's `morph_accept_drawing_filter` and the Unpaired sources' `start_alone` /
  `draw_alone` are apart in `morph.cpp` and merged without a conflict. The fallback is drawn only
  for an item `item-morph.cpp` marks `ItemMorph::Alone`, which is set after the linked, main-image
  and beta158/159 joining rules (`kJoinShare`) have all had their turn, so a left-over item that
  joins a change is still morphed, not faded.
- Test added: `[fit priority]` ends with a Scene Fit on All sources (nothing listed) whose game
  entry takes its space first: the game is as tall as the screen and the camera beside it, both
  entries have a *Space priority* control, both sharing behave as before, and a source put down
  inside the game's space is fitted round it while the game does not move.
- Conflicts were in the handoff, `fit-engine.cpp` (`solve`), `scene-fit.cpp` (properties) and two
  new test functions written at one place in `tests/ambient-test.cpp` (both kept).
- **Not tried on Windows**: any of it. In particular Scene Fit's window with many unlisted entries
  and a *Space priority* in each, another plugin's transition as Morph's unpaired fallback, and how
  the owner's real layout scenes settle with All sources.

### All sources in the scene filters (branch `feat/scene-all-sources`)

The owner: "the scene filters must have an all sources option." Each scene filter that chooses sources
now has one **Sources covered** list with **All sources** first (AGENTS has the numbers and the rules):

| Filter | Sources covered offers | A new filter starts on | A filter saved before reads as |
| --- | --- | --- | --- |
| Scene Transitions | All sources, Chosen sources (was *All sources in the scene* / *Only listed sources*; *Leave these alone* is *Ignored sources*) | All sources | unchanged |
| Scene Fit | All sources, Chosen sources (its *Fitted sources*), plus *Ignored sources* | Chosen sources | Chosen sources |
| Scene Standby | All sources, Every capture, Chosen sources | Every capture | Every capture |
| Scene Corners | All sources, All but the background, Chosen sources | All but the background | All but the background |
| Scene Shadow | All sources, Chosen sources (*No shadow for* is *Ignored sources*) | All sources | All sources |

No default changed: OBS saves no value left on its default, so changing one would have changed every
filter saved with it. Scene Modes has no All sources, on purpose (AGENTS says why). `screen_share` moved
from Scene Corners to `src/common/scene-util.hpp`. Scene Fit's unlisted sources: a source put down inside a fitted
source's space waits until that one has made room, and with All sources a place inside another fit's claim is
moved clear of it before the space is looked for (`set_claims_firm`, `FitEngine::solve`; without both, a source
added later and the chat box took the same space for good, found by the test). Fit to Space and a Scene Fit on
Chosen sources solve as before. Tests: `[all sources: scene corners]`, `[... scene
standby]`, `[... scene shadow]`, `[... scene fit]` in their own `scene-filters:all-sources` block. Not
proven on Windows: Scene Fit's window with many unlisted entries (one group each under *Settings for*),
and how All sources settles in the owner's real scenes (in the test four sources settle and stay still).

### Space priority for Fit to Space and Scene Fit (branch `feat/fit-priority`)

The owner, with a screenshot of a scene with many Fit to Space filters: "fit to filter priority of
the source to override another source size or placement should be based on filter position ...
if I want my primary capture or live sources to always grab space before and override the camera,
it would go higher in the list". Then: "make it configurable. maybe a selector to set source as
dominant, equal, and less", and "a tickbox that would disable the selector and make list order the
priority".

- **What the user sees.** *Space priority* on every Fit to Space and on every Scene Fit entry
  (*Takes space first*, *Shares space*, *Makes room*), and *Priority by list order* on every Fit to
  Space and once on Scene Fit. A source that takes space first fits as if the sources that share or
  make room were not there and never moves for them; they fit around it. One that makes room fits
  around all the others. The same level shares, as every fit did before. With the box ticked the
  selector is greyed out, and among the ticked fits of a scene the one higher in the filter list
  wins (a Scene Fit at its place, its entries in their order). A ticked fit counts as *Shares
  space* towards an unticked one. Only the free space takes part.
- **Default: *Shares space*, box unticked, for saved and new filters alike.** Saved filters must
  not change by themselves: the owner's layout scenes were built on sharing (the Live scene's game
  fitted beside the camera and chat column), and a dominant capture there would take the whole
  height and leave the camera and chat box over it. For new filters *Shares space* too, not list
  order: a new filter is most often added to a scene that already has sharing filters, and it then
  behaves like them; with list order on by default, the order filters happened to be added in would
  quietly decide who wins, and the selector would be greyed out for no reason the user chose. The
  owner ticks the box, or picks a level, where they want it.
- **How the claims changed.** A claim carries its fit's rank (`Rank`), looked up at every tick. A
  fit does not see the claim of one it outranks at all; a claim that outranks it is a firm side,
  never one that gives way. Between fits of different standing the dependence is therefore one-way
  and nothing can chase. The same standing keeps today's two-way claims and their anti-chasing rules
  untouched. A higher fit that covers a lower one's home point no longer lets the lower one sit on
  top of it: the space is looked for just off the higher one's sides (`free_space`'s `firm`), the
  home point is kept. A filter moved up or down is followed at the next tick, with a glide.
- **Tests:** `[fit priority]` (`fit_priority_test`; run alone with
  `STAGEKIT_TEST_ONLY_FIT_PRIORITY=1`): every level against every other in both filter orders,
  ties compared with two that share, a filter saved before the setting, the camera and the game
  each hidden and shown in both orders (the dominant game never moves; the camera makes way in one
  move), list order with a filter moved up (glides) and back, ticked against unticked, the
  selector greyed out, Scene Fit entries in order and swapped, Scene Fit by levels, and a Scene Fit
  and a Fit to Space ranked by their places in one scene. The existing fit tests are unchanged:
  their filters read as *Shares space*.
- **Not done or not proven.** Not seen on Windows. Kept-size items in a priority pair are not in
  the new tests (the old pair tests still cover them sharing). A lower fit with no room at all
  keeps its last claim and its item stays where it was, under or over the higher one. A fit outside
  the free space (whole scene, an area) has no rank and is an ordinary item to the others. The
  merge scripts write no `space_priority`, so the generated collections share as before.

### Link Visibility's show and hide transitions; Morph's Unpaired sources (branch `feat/link-visibility-transitions`)

Branch `feat/link-visibility-transitions`. The owner, with a screenshot of a Link Visibility
(*Follow the first source*, Game Capture and Column: Chat, *Also in other scenes*, Morph, 500 ms,
*Lock hidden sources*): "Link visibility scene filter should have additional transition options
including a fallback or other items transition for items that don't have partners".

- `src/placement/visibility-link.cpp`: the fuller *Show/Hide transition*, the one Show in Scenes
  has (`add_item_change_properties`, `item_change_read`, `apply_item_change`), not a third kind.
  *Settings for* lists the sources of both lists (the list setting is now `"items,elsewhere"`;
  `build` in `src/common/item-transition.cpp` reads comma-separated lists). Old filters keep the
  same `transition` / `transition_ms` keys and `hide_transition` defaults to *The same as
  showing*, so they read as before (tested: the kinds and durations set on the items, and *What is
  set on each item* left alone).
- `src/transition/morph.cpp`: *Unpaired sources* (`unpaired`, text; `morph_fade` as it comes):
  Fade, Fade and zoom, Slide off the nearest edge (new for an item by itself: it was a fade), a cut,
  or any other transition with its own settings (`unpaired|<kind>|...`). Another transition is a
  private one (`fallback_`) set up in `start_alone` as a scene starts an item's own and driven by
  hand from the change's clock; never to its end (it would draw the hidden item again), Morph
  draws the last frames. Shown in Morph's own window (with the chosen kind's settings in a group
  `unpaired|g`, rebuilt by `unpaired_own_cb`) and, copied, under Morph in every fuller *Change
  with* in place of *With nothing to change into* (`others` is not copied: it only means something
  in a change of scene). Old settings: `others` Fade and zoom reads as Fade and zoom, anything else
  a fade, as before.
- `build` in `item-transition.cpp` now makes nothing again when the controls are already made for
  what is chosen (a hidden mark, `change|sig|...`). OBS calls every control's callback once when it
  builds a window, from a list taken beforehand; one more callback that rebuilt (Unpaired sources)
  made a callback run on a control already taken off, and a test window crashed in
  `obs_properties_apply_settings`. The same was true before for the two kind lists and went unseen.
- `item_morph_item` (`item-morph.*`) gives the item a Morph is the transition of, for the nearest
  edge.
- Test `[link visibility: show and hide transitions, and Morph's unpaired sources]` (run alone with
  `STAGEKIT_TEST_ONLY_LINK_TRANS=1`): old settings, separate show and hide with a transition's own
  settings, a source's own settings, the window; Morph's and Show in Scenes' controls and old
  `others`; and a camera hidden as a capture is shown with a small source shown by the Link
  Visibility, for each fallback (Morph's fade, zoom and slide, Fade and Zoom (StageKit), OBS's Slide
  off the nearest edge, a cut): part-way in and out (or at once for the cut), whole or gone after,
  with the camera and capture still morphing by one layer. `[show in scenes]` now looks for
  `show|stagekit_morph|unpaired` where it looked for `...|others`.
- Not seen on Windows; another plugin's transition used as the fallback has only been tried with
  OBS's Slide and StageKit's Fade and Zoom.

## Scene Blur and its Stream Deck key (0.1.0-beta161, plugin 0.21.0.0)

The owner: "also make another scene filter. The filter is a blur filter. The blur filter should be able to
blur all sources, live and capture sources only, specific sources, and also never blur or ignore specific
sources. Also, there should be a companion stream deck key. the stream deck key must be able to activate the
blur for a specific scene, or all scenes with the blur filter of the same name."

- **Scene Blur** (`stagekit_scene_blur`, `src/look/scene-blur.*`): *Sources to blur* (All sources / Live and
  capture sources / Chosen sources with *Blur these*), *Never blur these*, then the Blur filter's own *Blur*
  and *Timing* groups. Per scene by drawing over the finished scene with a mask of the covered sources (see
  AGENTS.md, *Scene Blur*); nothing is added to any source, so the same source in another scene stays sharp.
  Off it is disabled and costs nothing; on, per frame: the scene once into a texture, each item from the
  lowest covered one up drawn again into its own picture for the mask, the scene drawn once more without
  the sharp sources above covered ones only when there are some, the blur passes, one composite. No
  read-backs. Classification four times a second while on and showing.
- **Blur's code moved** into `src/blur/blur-pipeline.*` (look, ramp, controls, passes); Blur and Scene Blur
  both use it. Blur's render-test images were byte-identical to `main`'s before the move was committed.
- **With Scene Shadow**: Scene Blur keeps itself after every Scene Shadow (twice a second), and Scene Shadow
  passes over a Scene Blur before it meanwhile; both add orders are tested.
- **Morph**: `morph_accept_drawing_filter` keeps up to eight predicates now. A scene whose Scene Blur blurs
  changes by cross-fade (blur kept on screen); off, or blurring nothing, it is morphed as before.
- **Requests**: `ListSceneBlurs`, `SceneBlur` {filter, scene or "", how on|off|toggle or none}, event
  `SceneBlurChanged` {scene, filter, active}; `style` in the answers is the keys' Blur style entry. Hotkeys
  `stagekit.sceneblur.toggle|on|off.<filter name>` on the scene.
- **Stream Deck key** *Scene Blur* (`com.techsavage1989.stagekit.sceneblur`, new; no existing UUID changed):
  *Blur filter*, *Scene* (*All scenes with this filter* first), *On press*, *Blur style* (the Blur key's
  fourteen) and *Scene type* with the style as a corner badge (`sceneBlurShown`, as Blur All's
  `blurAllShown`), *Palette*. Through features.json's new `scenes` list and make-plugin-files.py; VERSION
  0.21.0.0; action icon drawn (`make-action-icons.py`, SVG written).
- Tests: `[scene blur]` in `tests/ambient-test.cpp` (33 checks), contact sheet `docs/images/scene-blur.png`
  (`tests/run-tests.sh` makes it), a stand-in-server test in `streamdeck/tests/obs.test.ts` and the Blur
  style list check in `key-types.test.ts`.
- Shared text outside the blocks, kept small: README's scene-filter intro (*seven*), the Remote control
  table, the Stream Deck key tables; AGENTS.md's folder map rows for `src/blur/` and the scene filters;
  one condition in Scene Shadow's `look_again`.
- **Not proven**: Direct3D (the two new techniques in `stagekit-looks.effect` follow the rules but have only
  run on OpenGL); Windows; the key on a real Stream Deck (the *Scene* list with an empty value for *All
  scenes*, and its refresh when the filter changes).

## Stream Deck: a type list on every key, its function as a corner badge (0.1.0-beta160, plugin 0.20.1.0)

Branch `feat/streamdeck-every-key`. The owner, of the Manual Window / Source key: "no types
selection for manual window/source? all keys must have the options"; and of Move Source: "Move
source needs plenty of relevant custom icons for all the potential options".

- Every key a user can add has a type list now (tested against the manifest). New: *Source type*
  on Manual Window / Source, Arrange tool, Dock Option, Slide to Edge, Set Aspect Ratio, Capture
  Active Window (beside Capture type), Move Source and Move Scene Item (beside Position); *Scene
  type* on Blur All (beside Blur style). Lists in `SOURCES` / `sceneKeys()` of
  `streamdeck/src/key-types.ts`.
- With a type chosen, the image is the type's picture with what the key does as a badge in the top
  right corner; Move keys carry a second badge top left (linked source, flipped or not; this scene
  or a chosen scene only). 78 badges per palette by `tools/make-badges.py` (PNG + SVG); laid
  together at run time by `src/compose.ts` (PNG data URL, plain Node, no native modules). See
  AGENTS.md, *A type and a function on one image*.
- Position covers every Destination of `place-item.cpp`: nine places, *Saved spot* (9) and the new
  *Spot on the screen* (10, drawing `options/screen-spot`). There is no slide, flip or swap
  destination in the filter; *flip* is the linked source's `flip_linked`, shown as a badge.
  `moveTitle` says *Screen Spot* for 10 (it said *Saved Spot*).
- Automatic is unchanged: no type, no badge, the same file path (tested for every function and
  setting). Keys given a Source type in beta155 (Capture Active Window, Manual Window / Source) now
  show that type with their capture kind or step as a badge; hook keys no longer show the
  `hook-type-*` pictures.
- Tests: `streamdeck/tests/compose.test.ts` (every badge drawn in every palette; every composed
  image a valid 144 px PNG equal to the base outside the badges and to the badge inside; on/off
  mark kept; PNG reader/writer round trip on 256-colour and RGBA files; Automatic and old
  settings unchanged) and the updated `key-types.test.ts` / `source-types.test.ts`. `npm run build`,
  `npm test` (52 pass), `npm run validate` pass.
- Not run on a Stream Deck: whether `setImage` with a PNG data URL looks as the file does (it is
  documented to take base64 images), and how legible a 54 x 36 badge is on a 72 px key.

## The chat box no longer joins the camera's morph (0.1.0-beta159)

The owner, on beta158: "the camera is morphing from the chat box. how do i change that", with their
current collection (2026-11-17). By then Component-Capture-Scene (Chat Live) held DSLR full-screen
too, Link Visibility-ComponentChat (*Show only one*: Column: Chat, DSLR) hid the chat column when
DSLR was shown, every item there had Morph, and the Morph links list was empty. Showing DSLR hid
Window Capture and Chat Box Group (686x853 of 2560x1440, 16 %): DSLR paired with Window Capture
(main image), and beta158's join rule, at the main image's 15 %, let the chat box join DSLR's
change as well. Going back, the chat box came out of the camera.

- `kJoinShare` (a third of the screen) for joining a change as the main image, on both the item
  left over and the one it joins; joining by a link is unchanged. The owner's original case
  (captures and DSLR fill most of the screen) still joins.
- Each pairing is logged: `Morph: 'A' changes into 'B' in '<scene>' (<rule>)`.
- Test: `[morph between items]` gains the owner's case (a 21 % panel hidden or shown with the
  capture as the camera goes or comes): 2 layers on beta158, 1 now. The beta158 blocks there grew
  from 26 % to 56 % of the screen to stay over the new bar, as the owner's camera and captures are.
- There is no setting for it: a panel is only pulled into another item's change through a link.
## A capture comes out of the DSLR (0.1.0-beta158)

The owner, with their collection and four logs: "when I switch to window or game source from DSLR
by hiding DSLR, it doesn't smoothly morph and transition into the new source like all the others
do". In their Full-Capture-Scene a capture's key shows the capture; Link Visibility-Captures
(*Show only one*, Morph) hides DSLR; the capture's *Follow the first* link (Link Visibility-Camera
Window, Game, ...) shows Square Camera, hidden while DSLR fills the screen. Their Morph links
include "DSLR ↔ Square Camera", and links are paired first, so DSLR went into Square Camera and the
capture, with nothing left to change into, faded in by itself. Their log shows one morph layer per
switch (03:47:43 and 03:47:44). Between two captures only one pair changes, so those morphed.

- `src/transition/item-morph.cpp`: an item left over changes into (or out of) one already paired,
  in this tick or within `kGraceNs` before, by the same rules (linked, then the largest). So the
  capture comes out of DSLR while DSLR goes into Square Camera, and back both go into DSLR. The
  order the three changes arrive in no longer matters.
- Test: `[morph between items]` gains the owner's case with three blocks (both ways, the capture a
  moment late, and a small unlinked item that still fades by itself). The new checks fail on the
  old code (no picture of camera and capture half-way, one layer) and pass now.
- Not seen on Windows. A workaround for the old build was to take "DSLR ↔ Square Camera" off the
  Morph (StageKit) links (then DSLR and the capture pair as main images, and Square Camera fades).
- With beta156 (pull request #173, `Morph::alone_cb`), a source left with nothing to change from
  fades in during the change instead of popping in at its end; the owner confirmed that on Windows
  (below). This goes a step further, as asked here: the capture is no longer left over, it morphs
  out of DSLR. The first case of #173's `[a capture shown in the camera's place ...]` test now
  expects that (two layers at once, the capture's own transition drawing nothing); its second case
  (the camera linked to the capture, the square view fading in) is unchanged.
- Released on its own at the owner's word ("Publish now"). It was put to them as beta155; #171,
  #173 and #174 were merged as beta155, beta156 and beta157 while it was being tested, so it is
  beta158. Full suite on the tree before the last merge: 984 checks, ALL PASSED.

## A source shown with Morph and nothing to change from (0.1.0-beta156)

One item hidden and two shown at once: the hidden one morphs into one of them, and the other went by
Morph's *Alone* path, which drew OBS's see-through placeholder for the empty side (`a ? a : b`) and
popped in at the end. `alone_in_` picks the side from the transition's own sources; Fade and Zoom
had the mirror fault on hiding. **Confirmed on the owner's Windows machine (2026-11-17)**: the
DSLR to capture switch now fades the capture in while the DSLR morphs into the square camera, and
the beta155 scene filters "are working well" so far.

## Stream Deck: type lists in place of the Image list (0.1.0-beta157, plugin 0.20.0.0)

The owner: "instead of having an image list on all the streamdeck buttons, have a similar source
type, scene type, etc style list relative to the button function ... every single stream deck
button must be intuitive with customizable types"; then "I don't want any of the buttons, or the
options specific to my stream scene collection, but I do want the specific icons I had"; then
"Dolphin isn't handheld. maybe something like retro or classic ... make as many scene and source
types as possible and provide the proper respective icons".

- `streamdeck/src/key-types.json` / `key-types.ts`: 49 source types in five groups, 32 scene types in
  five groups, and the looks (Blur style, Corner style, Position, Capture type); see the second round below. Every key but the
  two old hidden ones has a type list, or its own function list as its type. The Image list is gone
  from every page; an old Image choice is kept (or set as the type that shows it) so no key changes
  look. See AGENTS.md, *Every key chooses its image from type lists*.
- 18 source-type and 14 scene-type drawings added to `make-option-icons.py` (every palette, on/off,
  marked; SVGs written). The rest are the owner's pack art.
- Second round, after the owner saw the sheets ("Retro icon should be specific to GC+GBA. create as
  many device, status, style, activity icons as possible for every key that it's relevant to"): the
  console-and-handheld scene is *Console + handheld link*, its source pictures *Linked handheld
  screen* (1 and 2); *Retro gaming* (scene) has a pixel-sprite drawing; *Second retro game* and
  *Retro column* are gone (*Handheld column* in their place, under *Layout markers*). 51 more
  drawings: 13 devices, 7 status, 20 styles (7 blur and mosaic, 3 edges, 3 shadows, 7
  transitions), 11 activities and retro, 2 linked-handheld, plus a new Voice chat drawing (the old
  one is *Headset*). Now 86 source types in 8 groups, 47 scene types in 7 groups; looks: Blur
  style 14, Corner style 7, Shadow style 7 (new list on the Shadow key), Transition 7 (new, on
  Show/Hide Animation, Link Visibility and both switchers). `npm test` 48 pass. This branch was
  built on by `feat/scene-blur` from 470c394; these are later commits on top.
- Tests: `streamdeck/tests/key-types.test.ts` (every entry is what the key shows, files exist in every
  palette, no repeats in a list, generic names only, pages have the lists and no Image list, old
  images kept). `npm run build`, `npm test`, `npm run validate` pass.
- Changed look on Automatic (deliberate): a Show in Scenes key that went by its source's name or OBS
  kind to a type now shows the owner's show-key art for it (a camera: the camera with the eye), and
  a Scene Mode key's layout types the owner's scene layouts. Keys with a type chosen in beta155 show
  that type's art for their function.
- Not done: the OBS side's Scene Modes *Mode type* list still has the first twenty types only; not
  run on a Stream Deck (whether `<optgroup>` inside `sdpi-select` renders, and whether a user's own
  image beats `setImage`, are from documentation and memory, not seen).

## Scene filters and the Scene Mode key (0.1.0-beta155)

The owner asked for filters on a scene that look after its sources together, and then: "if you can release
faster in multiple releases then so be it". So beta155 ships the six scene filters and the Stream Deck side;
the frame-time work and a new scene collection built with them follow in beta156.

Built as a groundwork commit and six branches, merged on `feat/scene-filters` in this order:
`feat/scene-transitions`, `feat/scene-standby-corners`, `feat/scene-modes`, `feat/scene-fit`,
`feat/scene-shadow` (both phases), `feat/streamdeck-scene`. Each scene filter keeps a block of its own between
`==== scene-filters:<unit> ====` lines; the only file two units both edited outside their blocks was this
handoff. The Morph fix (the five non-drawing scene filters in `draws_nothing`, which the groundwork commit
had meant to include) came on two branches as the same change and is in once. Added at integration:
Standby's `sync_stand_ins` resolves *As the scene sets* before swapping a change for its twin, and the two
tests that needed Scene Modes (Standby's test 11, Transitions' test 11 reduced). The first whole run of the
merged tree found two faults, fixed here:
- Scene Transitions' scene signals reached a freed filter when its scene was destroyed (the weak reference
  no longer reached the scene to disconnect it): OBS hung in `obs_shutdown` in the tests, and could have
  crashed or hung on a change of collection. `detach(parent)` from `filter_remove`, and a `live` check in
  every handler.
- A Standby filter taken off its source left that source's address in its list of owned items
  (`g_stood_in`, now per scene and source), so a source created later at the same address had its hides
  passed over by `set_item_visible`: hidden captures stayed in use in the layout scenes' out-of-view test.
  `filter_remove` forgets them (Scene Modes' a30c71b had only worked round it in its tests). Scene
  Standby's test 9 (a covered source let go of with its scene) failed in the same run and passed after.
Seen while testing and not changed: in a scene that was on screen, a source whose item transition finished
there can stay counted as in use after the scene goes off (OBS's own counting, as AGENTS says under
`set_item_visible`); Transitions' Scene Modes test therefore changes the mode out of view first.
The review before release fixed four small things: the Automatic Source Switcher (`active-app.cpp`) still
set its own default of a fade, so it did not start on *As the scene sets* as the notes say (Transitions'
test 5 now checks the default of every filter named there); Scene Shadow's "asked for lately" check
subtracted two clock readings taken on different threads the unsigned way; `scene_transitions_stop()` is
also called at unload; and Morph's `draws_nothing` listed the scene filters twice. The notes and README no
longer name Auto Capture's hide among the filters with the choice: it sets no transition at all and uses
the one the source carries.

**Not in beta155, next (beta156):** the frame-time work (`feat/frame-time`: per-frame timing in µs,
`stagekit_timing_take`, the shadow scale check, the measurement harness) and the new collection
(`scripts/make-scene-filter-collection.py`, with `--no-scene-fit` / `--no-scene-shadow`). Deferred perf cuts:
Show in Scenes *Follow what is shown* by signal, Link Visibility's copies, Scene Fit's `kComingBack`.

**Not proven on Windows:** `stagekit-scene-shadow.effect` (Direct3D compiles it at load; an error leaves
the scene without its shadow), every scene filter's window in OBS's own Filters window (Scene Fit's per-entry
lists rebuilt in place are the least proven: cut to one filter-wide list if they misbehave), Morph between
two captures as a Scene Modes change, the Stream Deck settings pages and the Scene Mode key against the real
filter. Scene Shadow differs from a true layer only at see-through edges and does not follow a crop to
bounds. *As the scene sets* is the new default in every filter that shows and hides sources: it changes
nothing in a scene without a Scene Transitions (a fade of the filter's own length, as before); Transitions'
test 5 is the guard.

### Groundwork

The shared pieces the six scene filters build on, with no change of behaviour: their ids
(`src/common/scene-filter-ids.hpp`) and Morph's `draws_nothing` for the five that draw nothing,
`morph_accept_drawing_filter`; `kOwnSettings` by the last `|` part, plus `mode_now`, `mode_before`,
`managed`, `managed_by`; PickList `title_text` and `pick_list_on_change_for` (called after the plugin-wide
callback); `item_transitions_owner` asked per item; `holds-hidden.*` (`mode_holds_hidden`,
`mode_settling`, Link Visibility registered); `go-to-scene.*` moved out of Show in Scenes with `stay_in`;
`filter-order.hpp`, `stand-in.hpp`, `is_screen_capture`; the `==== scene-filters:<unit> ====` blocks in
plugin-main, CMake, the locale file, the tests, the README and AGENTS; `tests/run-tests.sh` stops on a
repeated locale key and on a failed build. Whole suite: ALL PASSED.

### Scene Transitions

Scene Transitions (`src/placement/scene-transitions.*`): one filter on a scene that writes every source's
OBS item transitions from its tick, on events, only where they differ; *As the scene sets*
(`kTransitionScene`) is the new default of every filter's transition, resolved only in
`resolve_item_change` (fade of the filter's own duration where the scene sets nothing). Owned (Standby),
private, *Leave these alone*, Show/Hide Animation and grouped items (unless `in_groups`) are passed over; the
scene's transitions are put back after another filter's own change. Standby's `resolve_item_change` call and
test 11 with Scene Modes (reduced: the scene's way in view, a cut out of view, nothing hidden left in use; the
item Morph pairing of two captures is not in it) were added at integration. Whole suite: ALL PASSED.

### Scene Standby and Scene Corners

Two scene filters that draw nothing and keep a filter on each source they cover, through one helper
(`src/common/managed-filter.*`, `ManagedSet`): Scene Standby (`stagekit_scene_standby`) a Standby Image
filter on each capture (or chosen source), Scene Corners (`stagekit_scene_corners`) a Corners & Edges on
each source but the full-screen background. Managed filters carry `managed` / `managed_by`, follow only the
scene filter they name (taken over by the next one when it is gone), are taken off 3 s after no scene
filter covers them, only from a scene filter's tick, never from `destroy`; a filter made by hand is left
alone. A managed Standby filter with no covering Scene Standby goes idle (draws straight through); a
Scene Corners switches its filters off and on with itself. Stand-ins are now per scene (the filter's own
*Show when source is hidden* in every scene, a Scene Standby's *Show when hidden* in its own scene with its
transition), and the transitions owner is per (scene, source). `Standby::update` makes the image again only
when `picture`, `file` or `source` change. Since the integration, `sync_stand_ins` resolves *As the scene sets* with
`resolve_item_change` before swapping the change for the twin. Test 11 (a camera held hidden by a Scene Modes mode gets no
standby image, both filter orders) was added at integration. The collection's adoption of existing filters
comes with the new collection (beta156).

### Scene Modes

Scene Modes (StageKit), a scene filter: modes made by hand or from a template (Starting, Game,
Display / Screen, Window, Camera, Chat, Intermission, Be right back, Ending), each known by an id
(`mode|<id>|name/type/go_to/sources`, `mode_now`, `mode_before`); one on at a time, hides before
shows, a source in both not touched; *Allow no mode*, *Same modes in scenes*, *Lock hidden sources*,
the fuller Show/Hide transition; hotkeys per mode, next and none; vendor requests `ListSceneModes`,
`GetSceneMode`, `SetSceneMode` and the `SceneModeChanged` event; registered with `holds-hidden`.
Also: PickList's `parse` copes with a setting name that has
bars in it. Tests: the `[scene modes: ...]` sections (16 of the design's, the out-of-view one in the
sound section).

### Scene Fit and Fit to Space off screen

Three commits, the whole suite passing after each. (1) Fit to Space's engine moved as it was into
`src/placement/fit-engine.*`. (2) The engine's tick cut into steps (`prepare`, `motion`, `observe`,
`solve`, `publish_claim`, `advance`, `apply`) with what is in the way in a `FitScan` of its own, Fit to Space
calling them in the old order; off screen with nothing asked of it a fit's tick is skipped but four times a
second (perf cut 1); the shadow room is read again only when `shadow_room_revision()` changes (perf cut 2,
three anchored lines in `shadow-backdrop.cpp`). (3) Scene Fit (`stagekit_scene_fit`): one entry per source
with Fit to Space's settings under `fit|<source>|` (lists under `fit_<hex>_`), all worked out in one tick in
list order with up to four passes, one glance a tick, *Take over Fit to Space*, renames followed.
Not done (next work): `kComingBack` inside Scene Fit, one glide time for every entry during a window
switcher's change, the both-orders Link Visibility test, the window-switcher sync test, and the cost test
against seven Fit to Space filters, which waits for `stagekit_timing_take` (the frame-time work, beta156). The new collection
script's `fit_for` must write the keys *Take over* writes (effective values, Scene Fit's own gap default is
30, the lists under `fit_list_prefix`).

### Scene Shadow

Scene Shadow (`stagekit_scene_shadow`, `src/look/`): one shadow, glow or outline for a scene, laid over the
scene's own picture and kept off every source that is not background (silhouettes drawn from the items with
`LookPaint`), in linear light so it matches Shadow, Glow & Outline (off by at most 1/255 on an opaque square).
Nothing is moved or resized. Tested in `[scene shadow]`: unchanged where there is no shadow, the per-source
match, background, *No shadow for*, own shadows, sizes, once a frame, a hide transition still ends and its
shadow fades, the switch fades, nothing before loading, linear light, the outline off the mask. Shader proven
on OpenGL only. Whole suite: ALL PASSED.

Scene Shadow phase 2 (Morph): a scene whose only drawing filter is a Scene Shadow is still morphed
item by item (`morph_accept_drawing_filter`), and Morph carries the shadow: the morph is drawn as a picture and two
silhouettes (each part weighted between its two scenes' flags by eased t) and composed with the look part-way
between the scenes' (`lerp_look`). Tested: no cross-fade logged, the square half-way, the shadow at about half
strength half-way to a plain scene (111 against 111 expected), run by the clock, and the item Morph's layer
casting. Not on Windows yet.

### Stream Deck: Source type and the Scene Mode key

Stream Deck plugin 0.18.0.0. A *Source type* list (Automatic and twenty types, ids shared with the
Scene Modes filter's *Mode type*) on every key that acts on a source and on the new *Scene Mode* key
(`com.techsavage1989.stagekit.mode`, vendor requests `ListSceneModes`, `GetSceneMode`,
`SetSceneMode`, event `SceneModeChanged`); keys for Scene Fit, Scene Shadow, Scene Corners and
Scene Standby; *Fit to Space Overlap* takes a Scene Fit. On Automatic a Show in Scenes key goes by
the source's name before its OBS input kind (not the other way, as first asked: that undid
beta154's holding-mode key images). `npm test`, `npm run build`, `npm run validate` and `npm run
pack` pass. Not run on a Stream Deck, and the Scene Mode key not run against the real filter.

## Live, Chat Live and Standby; Switch to scene (0.1.0-beta154)

The owner: "Full capture should be called Live / Holding should be called standby / Component
should be called Chat Live / there should be a configurable option that Pressing the show button
for any source that belongs to a specific scene automatically switches to that scene ... it must
be universal to work with future scene collection designs. also the standby scene, live scene and
Chat Live scenes should have icons that are representative of their function as should each of
the show in scene buttons. of course allow manual icon changes still."

- Show in Scenes has *Switch to scene* (`go_to`, a scene name, *(none)* as it comes): a press that
  shows the sources (turns the switch on, or off with *On means* hidden) also changes OBS to that scene (the preview in Studio Mode) after the sources
  are shown, unless the scene on screen already holds one of the sources or is that scene. Never on
  a press that hides them, a *Follow what is shown* correction, the state restored at load, or *Do it now*
  (a choice made here, not asked for: one line to change if the owner wants *Do it now* to switch
  too). A test section `[Show in Scenes: switch to scene]` with a test seam for the frontend; the
  real `obs_frontend_set_current_scene` path and Studio Mode have not run.
- The merge scripts write *Live* (was Full-Capture-Scene), *Chat Live* (was
  Component-Capture-Scene) and *Standby* (was Holding-Scene), and set `go_to` on every switch they
  make: captures to Live, titles to Standby, column markers to Chat Live. They refuse a collection
  with the old names (merge again from the file before). The collection's suffix is *+ Standby*.
  The trial file "StageKit Live, Chat Live and Standby (trial)" is the earlier "Five Scenes" file
  with only the names and `go_to` changed (compared), and loads in libobs with 151 StageKit sources
  and filters, none unknown. It is made from the owner's private collection and never committed.
- Stream Deck plugin 0.17.4.0: nine new key images (on and off, every palette): scenes *live*,
  *chat live*, *standby*; show keys *starting*, *brb*, *intermission*, *ending*, *column chat*,
  *column gba*. Matching unchanged (most words wins, a hand-picked image first). Not seen on a deck.
- To tell the owner: a capture key pressed in a game scene other than Live now goes to Live; set a
  switch's *Switch to scene* to *(none)* or another scene to change that. A title source must be
  named with the label's words (*BRB*, not *Be Right Back*) for its own image.
- Carried over: the Windows-only open points of beta147 and beta153 below (Morph between captures,
  fades between modes, GBA windows in Chat Live, the two free-space fits in Standby side by side;
  Alerts place, widget order, Pureref, one key per look, Studio Mode).

## One holding scene (trial, 2026-11-17)

The owner asked to combine Stream Start, Intermission, Be Right Back and Stream Ending.
`scripts/merge-holding-scenes.py` adds `Standby` (`Holding-Scene` until beta154) with the first, third and fourth as modes
(keys: Show in Scenes-Mode on Stream Starting Group, BRB Text, Ending Text, *Switch on*);
Intermission stays a scene because of the camera switch. The trial file sent to the owner has the
layout scenes and the holding scene beside all fifteen old scenes. Not seen on Windows: the fades
between modes, the text and browser sources in them, Be Right Back's own transition (a Transition
Table rule and a scene override) which Standby does not have.

## Five scenes: Intermission and GBA join (0.1.0-beta153)

The owner: "can the intermission screen be merged with the other hold scenes? also the GBA scene?"
Link Visibility gained *Follow the first source* (mode 2) and Show in Scenes and Link Visibility
now respect each other: a source shows only when its mode says so and no switch holds it off.
Holding-Scene (now Standby) has four modes (Intermission added); Component-Capture-Scene (now
Chat Live) has two columns (Chat,
GBA) switched by transparent marker sources. The trial file "StageKit Five Scenes (trial)" keeps
all old scenes beside the new ones. A first build put DSLR's standby image over every holding mode
but Intermission (its stand-in); fixed before release. The beta152 tab is renamed *Standby &
Ambient* at the owner's word. Not seen on Windows: the fades between modes, the GBA windows in the
Component scene, the two free-space fits in Holding-Scene side by side.

## Background dock becomes a tab (0.1.0-beta152)

The owner: "id like the stagekit Background dock to be added to the primary stagekit dock as a new
tab". `BackgroundDock` is now the dock's fifth tab (`pageBackground`); the separate dock
`stagekit-background-dock` is no longer added, so a saved window layout simply has nothing to put
there. The owner confirmed beta151's off keys: "the off keys are much easier to tell apart now".

## Off keys show what they control (0.1.0-beta151)

The owner, with a photo of their capture keys all off: "these all look virtually identical. how
would i tell them apart by picture alone?" The pack's "hidden" picture of a source is an empty
screen. Off images are now drawn from the key's own on picture (greyed, red sign), except the
blur types' shared off pictures. Stream Deck plugin 0.17.3.0.

## Stream Deck keys: legible off images, one style (0.1.0-beta150)

The owner, with a photo of their deck: "the current 'off' keys are very dark and virtually
impossible to recognize ... these need to be legible even when off", then "all keys for the
plugin must be of consistent design language and styling". Off images are now bright with a red
"no" sign in the corner (`make-state-icons.py`), and the keys that took pictures from the pack's
older groups are drawn in the framed-screen style (`make-option-icons.py`). Stream Deck plugin
0.17.2.0. Not seen on a deck yet.

## Window layout after a change of collection (0.1.0-beta149)

The owner confirmed beta148 ("collection switching works now, no error") and reported that the
saved window layout did not come back after switching collection. It was only brought back after
OBS started; now also after every change of scene collection. The menu's tick box is now *Restore
it when a collection loads*. On the way: the window layout's menu and Apply Layout shared the
`Layout.Save` string; the menu's strings are `WindowLayout.*` now.

## OBS closed itself on a change of scene collection (0.1.0-beta148)

The owner imported the trial collection and got OBS's "Source Cleanup Error ... some sources
could not be unloaded ... OBS Studio will now exit". Every log they sent, back to one from before
beta147, said `Not all sources were cleared when clearing scene data: - DSLR`. The Standby filter
on DSLR (*Show when source is hidden*) held DSLR's own scene items (`pairs_`), each holding DSLR:
a cycle that was only broken at the filter's next look, which does not come while a collection
changes. It now knows them by id. A test clears a collection the way OBS does and fails without
the fix. The beta147 trial itself (capture keys, Morph between captures) has not been reported on
yet.

## One scene per layout (0.1.0-beta147)

The owner asked whether their ten capture scenes could be one scene that changes between layouts.
A study (inventory, three designs, three sceptical reviews) found literal one-scene possible only
with new, unproven work (a Morph from a private copy of the scene to the scene, shown in a headless
test only), and offered one scene per layout instead. The owner: "having two primary scenes for
both component and full layouts that contain all the live sources is fine".

- `scripts/merge-capture-scenes.py` writes the trial collection: all fifteen scenes kept, plus
  Full-Capture-Scene and Component-Capture-Scene (Live and Chat Live since beta154), each with the five captures (Game Capture shown,
  the rest hidden and locked), a Link Visibility each, a Show in Scenes-Capture on each capture.
  Its fidelity table: every old scene is matched by the new one with that capture shown, apart from
  the options (Alerts centred in Full, widget under the camera, Pureref hidden) and one copy taken
  for every capture (Discord at 30,30; the chat box's y differs by 0.04 px). The trial file and its
  report are made from the owner's private collection and never committed.
- Plugin: *Lock hidden sources* on Link Visibility, *Follow what is shown* on Show in Scenes, a cut
  for visibility changes in scenes out of view, Show in Scenes' presses acted on in its tick, Link
  Visibility no longer losing a press from another thread, and three Fit to Space fixes (a hidden
  item's room on record, a showing item with no image holds its room, the own cut from the side it
  is kept towards).
- The generated collection was driven headless with the new build (a program kept in the
  scratchpad, not committed): every capture switch in both scenes, the eye, the camera and chat
  switches, and that no hidden capture stays in use. It found the transition left under way when a
  scene is left with a cut, now ended by Link Visibility.
- Not seen on Windows: the Morph between two captures as items, a real display capture's first
  frames, the HD60S when Component-Capture-Scene comes on. Open questions put to the owner: Alerts
  place, widget order, Pureref, one key per look, Studio Mode.

## Blur, Zoom and Auto Frame after a Standby filter (0.1.0-beta146)

The owner asked why other filters had to come before Standby Image, was told, and said: "yes,
make blur, zoom and auto frame safe too". All five StageKit filters that work on a source's
picture now take it with `draw_filter_input`. Betas 141 to 145 are confirmed by the owner (the
chosen transition, slide and fade with a shadow, the start-up crash, the camera's size, Morph by
visible pixels). OBS's own and other plugins' filters after Standby Image still cut.

## Morph by visible pixels (0.1.0-beta145)

The owner: "I think the morph transition effect should morph objects based on visible pixels,
not bounding box". Morph (between scenes and between two items) has *Use visible pixels*, on as
it comes: two different sources are matched by the part of each that shows. One small read-back
per item at the start of a change; if the owner sees a hitch as a Morph begins, that is where to
look. Not seen on Windows.
*Ignore shadows & outlines* (asked for the same day) matches the images without the room of
their Shadow, Glow & Outline filters; the effects are still drawn.

## A shadowed item grew at every start (0.1.0-beta144)

The owner: "even after i make them the same size, if i restart obs, the camera in a couple of my
scenes does not match". Their collection showed *DSLR Component*'s box 11 px larger in the two
scenes whose Fit to Space had no source chosen (the camera source had been renamed). Cause:
Shadow, Glow & Outline settled its room while the collection was still loading (see AGENTS.md).
Fixed with `scenes_loaded()`; Fit to Space now follows a rename. The beta143 crash fix is
confirmed by the owner. **For the owner to do once**: choose *DSLR Component* as the source in
*Fit to Space-ComponentCamera* in the HD60S and Window component scenes, or size the camera
there once more by hand.

## Stand-ins wait for OBS to finish loading (0.1.0-beta143)

The owner's OBS crashed at start three times after beta142, in `obs-quick-access-utility.dll`
(`QuickAccessUtility::LinkSceneItem`, from its load callback). Stand-in items, whose source is
private, were being added during loading. They are now made two seconds after OBS has finished
loading. Not confirmed on the owner's machine yet; if it still crashes, ask for the new crash
file and whether it starts with *Show when source is hidden* unticked.

## Standby when hidden: transitions with later filters (0.1.0-beta142)

The owner, of beta141: "most of the transitions don't work. they just instantly switch in place.
also some of them make the standby image flash then change when showing". Reproduced in the
tests by adding a Corners & Edges after the Standby filter: OBS draws a filter's input once a
frame, so source and stand-in showed the same picture during a change. Shadow, Glow & Outline
and Corners & Edges now take their input afresh (`draw_filter_input`); Blur, Zoom and Auto
Frame do not yet, and other plugins' filters after the Standby filter still have the fault.
*Transition* at the top of the filter is now
*No-signal transition*, as agreed. To check with the owner: the DSLR with its shadow and corners,
hidden and shown with a slide, a fade and Morph.

## Standby when hidden: the transition is chosen (0.1.0-beta141)

The owner: "the dslr standby works, but I want it to morph or perform transition that I choose.
currently it seems to just slide". The Standby filter's *Show when source is hidden* now has the
fuller *Show/Hide transition* and sets it on the source's items and on their stand-ins (ways
swapped); Morph links the two; the stand-in follows its item every tick; other StageKit filters
pass those items over (`item_transitions_owner`). Also the Stream Deck plugin's description fix
(0.17.1.0). Not seen on Windows: how the Morph between a camera and its standby image looks.
To check with the owner: the DSLR hidden and shown with Morph and with one other transition.

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

- **The update notice is the dock's line and a confirmation; there is no download bar or install
  dialog.** The owner asked for an updater on 2026-11-14 and it was built into the plugin
  (`src/update/`, since 0.1.0-beta83) rather than as a separate application.
- **No hero image or logo.** The README uses frames rendered by the tests instead of window
  screenshots, because OBS has not been run with a display.
- **`ramp-blur` predates the branch-naming rule** (`feat/`, `fix/`, ...). It is kept because pull
  request #1 is open on it. Use the prefixes for new branches.

## Releasing

Releases publish themselves; nobody pushes a tag by hand.

1. On a branch, add `docs/releases/<version>.md` (for example `0.2.0.md` or `0.2.0-beta1.md`). Bump
   `version` in `buildspec.json` when the base version changes. Merge the pull request. Line 3 of
   the notes must be a one-line summary and each bullet of *New*, *Changed* and *Fixed* must start
   with a short bold lead: the dock's Update button shows exactly those (see AGENTS.md).
2. On that push to `main`, `.github/workflows/push.yaml` builds every platform. Its *Find Unpublished
   Release* job takes the newest notes file by version; if no tag of that name exists, *Publish Release*
   (`.github/scripts/publish-release.sh`) creates the tag on that commit and the release **as soon as the
   Windows files are built** (about three minutes), with the notes and SHA-256 checksums, then adds the
   macOS and Linux files to the same release as each build finishes. Until then the notes end with a
   line saying which files are still being built. The owner asked for this on 2026-11-13: they test on
   Windows and a slow or stuck macOS build had held releases back (beta67 waited 50 minutes).
   Names ending in `-beta` or `-rc` (with an optional number) are published as pre-releases.
3. Tell the owner when the Windows installer is on the release page; do not wait for the rest. Then
   check the finished release: six files, checksums listed, no "still being built" line, pre-release
   flag correct. If a build fails the job fails and the release stays without those files: re-run the
   failed jobs and the missing files are added (files already there are left alone).

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

## Stream Deck keys keep their image and name (0.1.0-beta140)

The owner's screenshot: a Capture Active Window key set to a Window Capture showing *Capture
Game*, and rows of keys named *Move* and *Switch*; the plugin's lists still said *Picture* and
*Standby Picture*, so Stream Deck had not taken beta135's copy. **The cause was reasoned from the
code, not seen**: every one of those looks is what a key shows when OBS gives no answer. Keys now
remember the last answer and look again after OBS has loaded; the plugin is 0.17.0.0. Whether
the installer's hand-over of the `.streamDeckPlugin` file updates an installed plugin by itself is
still not known: ask the owner which version Stream Deck shows.
## The standby image in a hidden source's place (0.1.0-beta139)

Asked for by the owner in those words; built into the Standby filter as a tick box (AGENTS.md).
The owner confirmed beta138: "layout restore worked great". Tested with colour sources in one
scene: the stand-in appears in the source's place and size, follows a move, and is taken out when
unticked. **Not tried here**: a source with a shadow or corners after the filter, a source in
several scenes on screen at once, a camera that stops when hidden, and how the twin items look in
OBS's Sources list. Items in groups are left out.

## StageKit keeps OBS's window layout (0.1.0-beta138)

The owner: their dock-layout plugins save the layout, "but whenever I start OBS the layout is lost
and I have to manually reload", and then asked for StageKit to store the docks and layout and load
it at start-up. Built as four entries in the Tools menu (AGENTS.md). Tested with a stand-in
QMainWindow and two docks; **not run in OBS**, where the timing after loading (1.5 s) is a guess
to be confirmed by the owner. Their log has OBS Dock Layout Manager, OBS Auto Dock Resizer and
jrdockie loaded, the first two keeping their profiles in `C:/Program Files/obs-studio/bin/64bit/`.

## Auto Capture: retries long enough and often enough (0.1.0-beta137)

The owner: "auto capture is not capturing a game even when i've added it manually to the always
capture list", with screenshots (aimed at Ryujinx, *Now capturing: Ryujinx.exe*, the Standby image
showing) and a log. The log: before it was on the list, Ryujinx was given up on after 10 s and
then tried for 5 s at 30, 60 and 120 s apart, each without a picture; aimed again at 01:17:40 it
gave one after 7 s (Vulkan); on the list, OBS tried 254 times from 01:17:55 and got the picture at
01:19:43, nine seconds before OBS was closed. **Whether a game was running all that time is not
known**: OBS hooks a program when it draws, and an emulator's menu does not. Changed: full
patience at each try, waits of 20 s to 60 s, a retry on coming to the front, and a status that
says the capture is aimed but empty. Asked the owner whether the game shows once it is running.

The owner then said a game *was* running, and that the capture came after clicking out of the game
and back in. The same log has OBS starting a Vulkan and a Direct3D 12 capture of Ryujinx in one
instant, and for Dolphin the Vulkan capture freed and started again 109 times; their card is an
RTX 4090 and their NVIDIA *Vulkan/OpenGL present method* was *Prefer layered on DXGI Swapchain*
(their screenshot). They were asked to set *Prefer native* and report. **Not proven**: read from
the log's pattern, nothing StageKit can change. Ask for the result before touching Auto Capture
again for this.

**Confirmed by the owner**: "set to prefer native and the capture works immediately now". It is in
the README's Troubleshooting and in AGENTS.md.

## The order tools: two by two, four drawings (0.1.0-beta137)

The owner asked for the dock's four order buttons stacked "into two columns and two rows" and for
drawings that are "completely distinguishable". Done in `stagekit-dock.cpp` (`orders`,
`tool_icon`); the README's dock picture is regenerated.

## A window named by part of its title is followed through title changes (0.1.0-beta136)

The owner confirmed beta135 ("everything seems correct and functional") and asked whether one
Window Capture could show two windows at once, for Dolphin's two GBA link windows beside the game.
No; and their layout places the two apart, so it is two captures. What stood in the way was in
their collection: the windows' titles change with the volume ("GBA1 | Volume 100%", "GBA1 |
Muted"), so the old scene had four captures. A switcher's title entry is now re-aimed when the
title changes, and the lists offer a title's first part. Windows side not run here.

Before it was published the owner asked whether it works for other apps, whether it breaks
things, and for exclusions. The first version re-aimed at every title change, which restarts a
capture; it now does so only for a capture with no picture. *Ignored windows* was added. One run
of the suite failed in Ambient Background's timing check (`when the capture changes the background
stays for the delay`) while a commit and push ran beside it; it passed in the two runs after.

## Auto Capture no longer puts programs on Never capture by itself (0.1.0-beta135)

The owner: "Dolphin keeps getting added to the never capture of auto capture, even after i remove
it", with two logs. The log of 21:01 has `could not hook Dolphin.exe in 10 s; it is now on the
Never capture list` at 21:24, 21:50 and 21:55, each while Dolphin was open without a game (the
last one 12 s after `vulkan capture freed`: the game had just been closed), and Dolphin hooked
at once in between when a game ran. The give-up is now a rest kept in memory (AGENTS.md). Entries
the old code added stay on the owner's list: they were told to take `dolphin.exe` off once more.

## Start image is Standby image; picture is image everywhere (0.1.0-beta135)

Two instructions from the owner (see AGENTS.md, Names, the fourth). Done by script over the
values of `data/locale/en-US.ini`, the README outside code spans and links, and the Stream Deck
plugin's `features.json`, `kinds.ts`, the *Image* label in `make-plugin-files.py` and one label in
`pack.json` (which `import-icons.py` would write back as "standby picture" from the file name if
the pack is imported again: change it again then). Ids, settings and code names are unchanged.

## Morph rules for two scenes; a glass look for the mosaic (0.1.0-beta134)

Two asks from the owner in one message. Morph: rules by pair of scenes (see AGENTS.md, under
Morph), with a cut, a cross-fade or the morph with other settings; the transition's duration
cannot be set by a rule and the owner was told so. Mosaic: a *Glass* slider in the Blur filter and
Ambient Background, done in the existing `Mosaic` technique of `stagekit-looks.effect`. That file
is the one that may fail to compile on Direct3D without taking the older effects with it; **the
new lines have only run on OpenGL**, so ask the owner whether the mosaic still shows on beta134.

Also in beta134 (Stream Deck 0.16.0.0): a filter key left on *Automatic* fits its source (the
pack's per-source picture by name, then the source's kind for Show in Scenes). One run of the
Stream Deck tests failed in `obs.test.ts` while the OBS tests were running at the same time and
passed on the next two runs; not looked into further.

## Blur reaches a capture shown through a Cropped View (0.1.0-beta133)

The owner confirmed beta131 and beta132 (scene changes keep the capture, the arrange block, the
update notice and the Tools menu) and reported that *Square Camera*, a Cropped View of the camera,
was not blurred at all. `find_live` only took capture kinds that were scene items themselves; it
now also takes the capture a Cropped View shows. Tested with stand-in captures.

## The updater: notice at the top of the dock, the rest in the Tools menu (0.1.0-beta132)

The owner's screenshot had the status line and the *Updates* button below the dock's visible area,
behind a scroll bar. Asked: the notice at the top and only with an update; everything else in
OBS's Tools menu. Done as asked (see AGENTS.md, the dock's bullet). The Tools menu entry is only
made when OBS has a main window, so the tests see the menu as a child of the dock; how it looks in
OBS's own menu has not been seen here.

In the same release the Arrange tab's twelve line-up tools became a block three across (the
order of `ArrangeAction` gives the rows the owner asked for) with the measure options beside it.

## The capture gone after a scene change; edge gaps as sliders; the pack's SVGs (0.1.0-beta131)

The owner, on beta130: "When switching to a non window capture containing scene then to window
capture scene, the capture is gone and does not come back unless I specifically pick a certain one
of the auto capture windows." Cause, read from the code (a test now covers it; it was not run against the old code): `ready_for`
dropped every ready capture while the parent was in no scene on show, the bridge too, with
`own_ok_` false, and nothing took a bridge up again but `aim`. Now the shown entry's ready capture
is kept while away and a lost bridge is restored. Not run on Windows. Also: the edge amounts are
sliders named *Top / Bottom / Left / Right gap up to* under *Use edge gaps (pixels)* (the owner:
"should be sliders with digit inputs", "it's not clear how it works"); the owner's icon pack SVGs
are committed under `streamdeck/svg/pack/` with their say-so. The owner confirmed "key icons work".
Still unanswered: whether *Square Camera* stays clear under Blur with *Never blur these*.

## Fit to Space: overlap by edge distances (0.1.0-beta130)

The owner asked for the *Fill needed to overlap* rule to have, beside the percentage, pixel amounts
for the top, bottom, left and right edges, each measure with its own tick box. `skip_use_pct` (on)
and `skip_use_px` (off) under the existing `skip_fill_on` switch; four `skip_px_*` amounts. The
edge rule overlaps when the empty room at all four edges is within its amount; either ticked rule
is enough; neither ticked means always beside. Tested in the free-space test (edge rule, both,
neither). Also in this session: the owner's icon pack arrived with SVGs for every picture (the same
pictures the plugin has); a zip of the pack's SVGs with StageKit's own was sent to the owner, and
nothing of it is committed (about 42 MB; the owner was asked whether they want it in the
repository). The owner confirmed beta129: "the fit to now follows the windows size transition
perfectly".

## The fit still trailed: Standby Picture's own glide; scene keys use the pack's scenes (0.1.0-beta129)

On beta128 the owner still saw the fit trail the switch and sent two logs. The Standby Picture
after the switcher glided the size a second time (AGENTS.md). Also: Switch Scene keys take the
pack's scene pictures by name (Stream Deck 0.15.0.0), and the owner confirmed "keys show dot and
slash correctly". They said the original icon pack was attached; **no such file arrived**, only
the two logs, and they were told.

## Fit to Space goes with the window switcher's change (0.1.0-beta128)

The owner: the fit of the window capture should "line up with and coordinate with the auto switch
transition ... right now the window switches, but then it's like the auto fit has to catch up".
See AGENTS.md. Tested with stand-in windows; not seen on the owner's machine yet.

## Switch Scene key, device pictures, editable SVGs (0.1.0-beta127)

The owner asked for a Stream Deck key "for scene switching", icons "for things like generic capture
cards, external capture cards, etc", and "the full icon set with editable SVGs". All in AGENTS.md;
Stream Deck plugin 0.14.0.0. The SVGs cover everything drawn here; the original pack's pictures
are PNG only in this repository, and the owner was asked whether they have its source (the session
that made the pack had the program that draws it). Not run on a Stream Deck here.

## Keys show on or off; Never blur these lists every source (0.1.0-beta126)

The owner: the Blur key "just flashed when pressed, but does not otherwise signify if blur is on or
off ... this logic must apply to all keys", and *Never blur these* did not offer their camera. Both
are in AGENTS.md. Stream Deck plugin 0.13.0.0. **Not seen on a Stream Deck here**: the marked
pictures, `DisableAutomaticStates` and the per-state images are from the SDK's documentation and
pass `streamdeck validate`.

## Confirmed by the owner on beta125 (2026-11-15)

"everything seems to be working as intended": the work of beta121 to beta125 on their machine
(the window switcher's pinned windows, clearing, *Captured window*, *Auto-switch manual windows*,
Auto Capture's Standby Picture options, the overlap switch and key, the new names and the renamed
collection). Nothing is open from that work.

## A picked window is that window, never another of its program (0.1.0-beta125)

The owner runs several browser profiles: closing the picked one, the capture moved to another
profile's window, which could put private information on stream. Windows picked by hand are now
kept as that window (by its handle), and the window shown is watched so that OBS's own fallback
to another window of the program is never drawn. See AGENTS.md. Not run on Windows here.

## The approved control names (0.1.0-beta124)

The owner took the name table proposed with beta122, "mostly good", with *picture* as *image*,
*item* as *source*, and *Items it may overlap* as *Ignored sources*. 37 labels changed; the full
list is in AGENTS.md (Names). Only what the user sees changed: setting names, ids and saved values
did not, so nothing in a scene collection moves. Filter and key names were not in the table.

Also in beta124, asked for in the same session: the Automatic Window Switcher's *Captured
window* (`picked`), a list with *Wait until switched to* as the default (the owner asked for a
list and for that default) and *Switch at selection* (their wording). The default changes what
the Capture Active Window key did before: it no longer switches at once.

Also the switcher's *Auto-switch manual windows* (`hand_follow`), asked for in the same session.

And Auto Capture's *No game: Standby Picture* and *Hide shows Standby Picture* with their own
transition, asked for from a screenshot of Auto Capture's lists. See AGENTS.md.

## Stream Deck: an icon of its own for every key in the list (0.1.0-beta123)

The owner sent a screenshot of Stream Deck's list of StageKit actions, where every key showed the
same small drawing, and asked for "all of the stream deck buttons to get specific icons". Each
of the 25 keys offered now has its own (`tools/make-action-icons.py`); Stream Deck plugin
0.11.0.0. Not seen in Stream Deck here. The name table proposed with beta122 is still unanswered
and not applied.

## The overlap rule gets a switch, a clearer name and a Stream Deck key (0.1.0-beta122)

The owner on beta121: the fill rule at 98 % "seemed to work as intended" and "all the ambient
background changes work"; *Avoid them* was "unclear and ambiguous" and should have "a toggle on or
off and have a streamdeck button". Now *Overlap only when it fills* (`skip_fill_on`) and *Fill
needed to overlap* (`skip_fill`); a share set in beta121 reads as switched on. Stream Deck 0.10.0.0
adds the *Fit to Space Overlap* key, which sets the tick box through obs-websocket
(`SetSourceFilterSettings`, overlay) and reads it back from `GetSourceFilter`; never run in Stream
Deck from here.

Mid-task the owner added: the Standby Picture's show and hide "just instantly goes on or off" and
needs transitions "including morph and fade and zoom". It has both now (Morph is new, *Fade and
zoom* was there as *Zoom and fade*), and the capture stays live under the picture while it comes
in. The instant change was **not reproduced**; see AGENTS.md for the log line to ask for.

The owner also asked for "the verbiage of the whole plugin and feature set" to be reviewed. A third
table of names was put to them in chat on 2026-11-15 and is **not applied**: apply only what they
approve, as with the first two (AGENTS.md, Names).

## Bloom and blur, the fill rule for Fit to Space, hiding to the Standby Picture (0.1.0-beta121)

The owner on beta120: "frametimes are much better and all the transitions seem much smoother and
ambient background transition is better". Then four requests, all in this release:

- Blur looked wrong with Bloom on: the bloom's light came from the unblurred picture and was spread
  by *Bloom size* alone. It is now spread at least as far as the blur. **Not seen on the owner's
  machine**; it is the cause that fits the code, and a test measures the detail with and without.
- The Bloom slider would not drag: it had a modified callback that showed and hid the other bloom
  controls, so OBS rebuilt the window at each step. The callback is gone and the controls stay.
- Fit to Space: *Avoid them under a fill of* (`skip_fill`). See AGENTS.md.
- The switcher's hide brings in the Standby Picture (`hide_standby`, `standby_cover_toggle`), and
  Standby Picture has a button and hotkey of its own for it.

Still unconfirmed by the owner: Stream Deck 0.9.0.0 (wordless art, titles, toggle key), Morph in
Automatic Source Switcher, Fade and Zoom, *Never blur these*, the amplified bloom's look.

## The real cause of the late frames; background transition; the old bloom (0.1.0-beta120)

- The owner on beta119, with two logs: "the transition between manual and automatic is a bit
  jarring, since the image of the next capture appears to instantly appear, then the morphing and
  windows size changes happen"; "before the capture image appears, the ambient background changes
  to it instantly. there should be a configurable delay ... and the transition ... should include
  the same transition options that others do"; "the bloom effect in the ambient background doesn't
  appear to work correctly and is not the effect i intended. I like the bloom effect from my old
  scene collection"; "There is choppiness during the animations of morph transitions and resizing
  moves".
- **The choppiness was Auto Capture, not the window switcher.** Every log from beta114 on has the
  Game Capture trying to hook the owner's 3D modelling program about five times a second (5,338,
  3,215 and 3,404 tries in three sessions) and 7 % of frames late in `tick_sources`; the beta106
  log has one try and 0.2 %. beta119's change to the switcher was worth making (it did start
  captures at every switch) but was not this. Auto Capture now gives up; see AGENTS.md.
- The other three are fixed as AGENTS.md describes. The old bloom was found in the old collection
  the owner sent on 2026-11-11 (private, not in the repository).
- **Not confirmed by the owner yet.** If frames are still late, the profiler lines at the end of
  OBS's log (`tick_sources`, `render_video`) and a count of repeated lines are where to look first.

## Frame time at a switch, the toggle, the new names, Stream Deck art and titles (0.1.0-beta119)

- The owner on beta118: "the transitions work smoothly now, but there is a significant increase in
  frame times and FPS drop when transitioning. Capture front window does come in smoothly", with
  two logs. Then, one after another: "the next manual capture button should have an option to
  toggle between manual capture and automatic"; "there should be icons specific to each option,
  and this goes for every stream deck key"; "for all of the key art, remove the embedded text
  names of the keys in the art, but for the stream deck keys themselves, make sure the Title for
  the key is set accordingly based on the options selected"; "make all the changes for this
  release"; "use all the recommended names you proposed in the release".
- The logs: OBS's profiler had `tick_sources` up to 135 ms and 152 ms, drawing under 10 ms, no
  StageKit tick over 7 ms. 106 switches in the longer session. Fixed by never starting, aiming or
  stopping a capture at a switch; see AGENTS.md. **Not confirmed by the owner yet.**
- All of the proposed names are in: the table is in AGENTS.md under *Names*. Ids, settings and
  hotkey names are unchanged. The rest of AGENTS.md and this file keep the older names.
- Stream Deck plugin 0.9.0.0: no words in any key picture, a title from the key's options, a
  picture for every option (drawn here, not from the owner's pack: plain line drawings on the
  pack's little screen), *Toggle manual / automatic*. None of it seen on a real deck.
- An earlier step of this work was interrupted by the owner and a half-finished state was pushed
  to the branch as a WIP commit; the branch was finished afterwards.

## Capture App in Use: the flicker, the hook key, names (0.1.0-beta118)

- The owner on beta117, with a log: "There is still a flicker where it looks like the source sort
  of jumps immediately after switching to the other app, then the other app instantly appears,
  then it morphs. This occurs when I have any 'change with' setting other than Morph"; the *Capture
  Front Window* key "immediately makes the previously automatically captured source vanish
  instantly then a part of a second later the manually captured window morphs in"; "once i want
  to switch back to the Apps to capture, I don't know how to do so while also keeping the
  previously manually selected window available"; and "I'd also like the verbiage and filter and
  feature names to be reviewed and changed to make more sense based on OBS terminology".
- Fixed: see AGENTS.md (the ready capture dropped mid-change, the size jump, the hook through the
  filter). **Not confirmed by the owner yet.**
- **Names: proposed, not changed.** A table of new names in OBS's terms was put to the owner in
  chat (the last rename, Follow Active App to Show Source in Use, was followed by "where is
  follow active app?"). Change nothing until they say which.

## Capture App in Use: fast switching, apps by hand, Morph, hiding (0.1.0-beta117)

- The owner after beta116, with a screenshot of the filter on their capture: "Capture app in use
  should have the additionally added transitions, like morph ... my Show source in use filter on
  the scene includes a 'Captures you switch to' ... I need a similar functionality and morph
  transition capability ... also still be able to hide the window capture entirely if desired.
  these capabilities should be included in the obs plugin as well as the stream deck plugin", and
  then: "capture app in use switches between apps much slower than the show source in use filter
  ... it should be much faster and none of this should slow down OBS or increase frametime
  significantly". And: "where is follow active app?" (told: renamed, offered to change it back).
- Built: see AGENTS.md. Stream Deck plugin 0.8.0.0: the *Next Manual Capture* key also takes a
  Capture App in Use, with *Hide or show the capture*.
- The screenshot also showed the filter third in the list and a status line saying nothing was
  captured while a listed app plainly was. The order is now kept (`keep_first`); the status line
  now reads the capture's own setting. Why beta116's move to the top did not hold is not known.
- **Not confirmed by the owner yet; none of it has run on Windows.** *Keep the others ready* runs
  one more capture per other open app, as Show Source in Use's option of the same name does on
  the owner's present scene; ask for the log's `timing` lines if the frame time goes up.
- Not built: switching by hand to one named app out of several (the key steps through the list,
  which is the same thing with one app on it); the item Morph between this capture and another
  item works as for any item, through *Hiding and showing it*.

## Capture App in Use; Follow Active App renamed (0.1.0-beta116)

- The owner (2026-11-15), with a log: "I'd like a new filter for window capture. Currently I have
  multiple window captures set up to capture specific apps, and they have to be hidden and shown
  individually, per the existing show app in use filter (which should probably be called show
  source in use, or similar), or manually. I'd like to have a single Window capture, and a new
  filter added that can have multiple apps associated with the Window capture and effectively
  emulate my current process and visibility functionality based on the open and captured apps,
  but with only a single window capture source. Understand?"
- Built as **Capture App in Use**, a filter on the Window Capture. See AGENTS.md for how it draws
  the change between two windows. Follow Active App is now shown as **Show Source in Use**; its
  id and settings are unchanged, and a filter already in a scene keeps the name it was given.
- The Stream Deck plugin (0.7.0.0) has a *Capture App in Use* key and the renamed *Show Source in
  Use* key. Its action for the renamed key is the same one, so keys already on a deck keep working.
- **Not confirmed by the owner yet. None of it has run on Windows.** The log sent with the request
  showed the scene it is meant to replace (a capture for each app, one capture switched to by
  hand, a Fit to Space each) and nothing wrong. Things to ask for a log about if the change
  between apps is not smooth: the line `Capture App in Use: '...' now captures ...` gives the
  moment; whether the old window flashes back, or the picture is empty, says which of `kSettle`
  and `kWaitFor` to look at.
- Not built: hiding the capture's items while none of the apps is open (the capture has no picture
  then, and a Standby Picture stands in); moving the owner's scene over to one capture, which is
  theirs to do in OBS (add the filter to *Window Capture*, list the apps, remove the others).

## Morph between items; Fade and Zoom; Blur exclusions (0.1.0-beta115)

- Three requests in a row (2026-11-15), released together:
  - "id like a similar morph effect when showing and hiding objects. for example for my auto hide
    and show between my 3d modeling apps, and regular window capture. morph should be a transition
    in show in scene, and follow the same rules as the global morph source links, or specify
    source links in the filter."
  - "also add fade and zoom as a transition"
  - "for ramp blur, there should be an exclusion option for And live capture on screen, so cameras
    for example can be intentionally excluded from blur"
- How the first is built, and why a layer in the scene: AGENTS.md. Tested with stand-in colour
  blocks: a linked pair, an item by itself, the main-picture pair, the layer coming and going.
- **Not confirmed by the owner yet**, and not run in a real OBS window. Things to listen for:
  - the layer item flashing by in the Sources list, and a `User added source` line in OBS's log
    for each change (OBS's window writes it for any item added);
  - Follow Active App's captures in a group, or a Morph asked for in a scene that is not showing:
    those fade (said in the README);
  - a capture that is hidden keeps running for the length of the change (`obs_source_inc_showing`).
- Not built: links of their own in Follow Active App, Program Trigger and Link Visibility (they
  have the plain *Change with*; the links made in a Morph transition or a Show in Scenes count for
  them, and so does the main picture). Give them the link boxes if asked.

## Shadow and outline sizes while an item is hidden (0.1.0-beta114)

- The owner: with a slide off the nearest edge in Show in Scenes, an item's shadow and outline
  "start out thinner, or less pixels as they enter into the canvas from the edge of screen, then
  grow or instantly snap to the correct pixel thickness towards the end of the transition", and
  the Fit to Space items round it "adjust unsmoothly".
- Found: sizes in screen pixels go by how big the item is drawn (`shown_scale`), which skipped
  hidden items. Fixed there; see AGENTS.md. A test hides an item and looks at its corners; it
  fails on beta113's code and passes now.
- The owner after installing it (2026-11-15): "much better". Not said: whether the thickness is
  now the same all the way, or whether the fitted items move once. Take it as improved, not as
  proven perfect; do not repeat the two checks as open requests.
- Still so: the scale is looked up every 20 ticks, so an item that is a different size in the
  scene switched to gets its new thickness up to a third of a second into that scene (README,
  Morph's *Things to know*). Not what was reported here.

## Morph: a capture that starts during the change (0.1.0-beta113)

- The owner: "when i switch to my main display scene, the capture source doesn't appear until the
  transition animations completes, and it's abrupt and jarring", with Morph as the transition
  (their collection, sent with it, is private and not in the repository).
- Found: the display capture's item fits a bounding box. OBS stops a display capture while it is
  not showing, so at the start of the change its source is 0 by 0, and OBS's sum for a bounding
  box then gives a scale that is not a number. Morph worked places out once, at the start, so the
  item was nowhere until OBS drew the new scene itself at the end. A test with a stand-in that
  starts part-way failed on beta112's code and passes now (`Morph::look`). See AGENTS.md.
- **Confirmed by the owner (2026-11-15): "113 works and the frametimes are a bit lower too on
  average as are the cpu usage"**, with OBS's Stats window showing 60 fps, an average of 0.8 ms to
  render a frame and 2 frames in 5,093 missed. That also answers the frame-time question left
  open after beta109.
- Not built: keeping display captures running while their scene is off screen, which would take
  away the capture's own start-up moment as well. It costs capturing every display all the time
  (OBS stops them on purpose) and was not asked for. If the owner still sees the display come in
  late, ask for the log: `Morph: '...' had no picture for the first ... ms of the change` says how
  long the start-up was, and that is the figure to decide by.
- An item with no picture yet at the start is also not taken for the *main picture* (it has no
  size to go by). Not asked about.

## The owner's word after beta112 (2026-11-15)

- "everything is working and does what it should for now." Said after beta112 was published,
  without naming a version or a feature. Take it as covering what the sections below mark *not
  confirmed by the owner yet* (beta103 to beta112: Fit to Space's hidden items, kept size and
  shared place, the blur additions, the glances, Morph and its links, Show in Scenes' transitions),
  as far as the owner had tried them.
- Not answered one by one (the frame time has been since, see beta113): the
  Stream Deck plugin's version, the `primehack` rule in Auto Frame. Ask again only if one of them
  comes up; do not repeat them as open requests.

## Slide off the nearest edge in Show in Scenes (0.1.0-beta112)

- The owner: "for slide show in scenes filter, slide off the nearest edge should be an option".
  Added as a direction in the Slide's and the Swipe's own list. See AGENTS.md.
- The owner also asked whether Morph's links carry through one another. They do (`linked`), and
  were told so.

## Transitions with their own settings; Morph links (0.1.0-beta111)

- Two requests, released together because the owner said "release everything i've requested in
  the next release": Show in Scenes' *Show with* / *Hide with* with each transition's own settings
  and per-source choices, and *Linked sources* in Morph. See AGENTS.md for both.
- **Not confirmed by the owner yet**: either of them, nor a first real morph (beta110).

## Morph did nothing in OBS's own window (0.1.0-beta110)

- The owner on beta109: "it doesn't morph, it literally just instantly switches scenes". The
  transition took its size from `obs_transition_get_size`, which is 0 by 0 unless something sets
  one, and OBS's window does not; with no size it skipped its own drawing and cut at the end.
  The tests had set a size. Fixed, and the tests now drive it as OBS does. See AGENTS.md.
- **Still not seen by the owner**: an actual morph on their scenes.

## Glances only where they are needed (0.1.0-beta109)

- The owner on beta107: detection and speed "much better", but "it does seem like my frametime
  has gone up". Each glance draws its item once more, filters and all, and beta107 did that
  fifteen times a second over every item, the game capture and the background included. Now only
  items that can come and go are glanced at often; see AGENTS.md.
- In the tests a widget of such a kind is made room for in under half a second; an item of any
  other kind is first noticed within about ten seconds and quickly from then on.
- **Not measured**: the frame time itself, which only the owner's machine shows (OBS's Stats
  window, *Average time to render frame*). Ask for the before and after if it is still up.

## The Morph transition (0.1.0-beta108)

- Offered to the owner on 2026-11-14 as plausible; asked for the same day ("i would like to work
  on the morph transitions as well"). Built as a scene transition, `stagekit_morph`: see AGENTS.md
  and the README's *Morph transition*.
- Proven in the tests: an item in both scenes is half-way between its places at half time, two
  backgrounds cross-fade with no dip, the largest leftover items change into each other, two
  scenes that are alike give the scene's own picture mid-way (colours, crop, a scaled image), and
  the transition ends on the new scene.
- **Not proven**: anything on the owner's machine. Their scenes carry Fit to Space filters (fine)
  and may carry others; shadows sized in screen pixels may change size at the first frame; a
  2560x1440 canvas means several full-screen passes per frame for the length of the transition.
  Ask how it looks and for a log if it cross-fades where a morph was expected.
- Not built: pairing two different sources by hand (only the automatic main picture), a morph of
  items inside different groups, rotation taken the short way round.

## Glances: Fit to Space stops stalling the picture (0.1.0-beta107)

- The owner's two logs from beta106 (private, not in the repository): `stagekit_fit_item` with
  23 filters, single ticks of 8 to 45 ms; the owner saw a choppy preview and a chat box slow to
  react to the music widget appearing. Both came from the once-a-second look at each item.
  Replaced by glances, see AGENTS.md. In the test a widget is made room for in about 0.1 s.
- The same logs show `stagekit-looks.effect` loading without error on the owner's Windows
  machine: the new mosaic, trail and bloom shader code compiles on Direct3D.
- The owner confirmed on beta106: "Everything seems to work very well", and scene switches look
  right without *Same place in these scenes* being filled in.
- The owner on beta107: "THE DETECTION AND SPEED IS MUCH BETTER". Whether the preview's
  choppiness is gone as well was not said; if it comes up again, ask for a log: the
  `timing, last 30 s` lines say which kind is slow.

## Shared place for chosen scenes only; kept size survives a restart (0.1.0-beta106)

- The owner, an hour after beta105: the link "should not be all scenes", because the same source
  sits elsewhere on purpose in some scenes. The tick box became a list of scenes, linked both ways.
- The owner's chat box was a few pixels larger after every restart. Found in the code: a filter
  loaded with *Resize it to fit* unticked went back to the size from before it was first added.
  The kept size is now saved and used at load. See AGENTS.md. **Not confirmed by the owner yet.**

## A fitted item's place shared with the other scenes (0.1.0-beta105)

- Asked for by the owner after beta103: a link so that what Fit to Space does in the scene on
  screen is already done in the scene switched to. Built as a tick box on the filter (*Same place
  in other scenes*), see AGENTS.md. **Not confirmed by the owner yet.**
- The owner also asked whether filter order decides priority between Fit to Space filters. It
  does not; they were told so and that a priority setting can be added if two items still want
  the same room.

## More blur: cell shapes, Temporal, Bloom (0.1.0-beta104)

- The owner showed Composite Blur's *Blur Algorithm* and *Pixelate Type* lists and asked for
  similar settings in the Blur filter and Ambient Background, and for Bloom in Ambient Background.
  Built: the cell shapes with smoothing, origin and rotation, a Temporal type, Gaussian, Box and
  Temporal for the background, and Bloom. See the three entries in AGENTS.md.
- Not built, and said so to the owner: Composite Blur's effect masks and its background source for
  compositing; its tilt-shift and area/directional variants of Gaussian and Box.
- **Not proven**: the new shader code on Direct3D (Windows). It compiles and draws on OpenGL in the
  tests. It is in a file of its own (`stagekit-looks.effect`), so if Direct3D refuses it the older
  looks still work and only the new ones are missing; the OBS log then holds `failed to load`
  with the compiler's message. Ask for the log if the owner says a new shape, Temporal or Bloom
  does nothing.

## Fit to Space: hidden items, one move, a side to keep towards (0.1.0-beta103)

- The owner's screenshots: camera above chat box, each with its own Fit to Space. Camera hidden,
  the chat box moved to the middle of the column; camera shown, it came back in the middle on top
  of the chat box and both stayed. Read from the code, not reproduced on the owner's machine: the
  filter kept fitting the hidden camera, whose place by then lay just inside the chat box. A test
  now covers three fitted items with one hidden and shown again.
- Also asked for in the same messages: faster and "not jumpy", and a preferred direction. See the
  three Fit to Space entries in AGENTS.md (*hidden item*, *one step*, *Keep it towards*).
- The copy tools no longer touch an item inside a group that is shown in several scenes (it is the
  same item in each). The owner confirmed beta102 copies a group's transform.
- **Not confirmed by the owner yet**: the hide and show on their scenes, and *Keep it towards*.

## New names (0.1.0-beta87), and the first run of the updater (0.1.0-beta86)

- beta86: the owner pressed *Update* on beta83 and nothing followed; no `StageKitUpdate` folder was
  made, so the helper never ran. It is now started with `CREATE_NO_WINDOW` alone and logs its steps.
  **Unproven until the owner updates from beta86 or later to a newer release.** Also the dock's
  height by tab, and the Stream Deck settings pages' font.
- beta87: the names in the table in AGENTS.md (*Names*), approved by the owner with "go". Sections
  below this one use the old names.
- Still to be answered by the owner: whether to build the scene morph.

## Keys for the dock's switches and slides (0.1.0-beta85)

The rest of "I need every feature that makes sense as a key": a *Dock switch* key (snap on release,
guides, stop resizing, slide, in every scene, keep shape; it follows the dock through the
`OptionsChanged` event) and a *Slide to next edge* key. Still no key: Auto Frame's *Frame it now*
and a dial for a blur's amount; nobody has asked for those by name. Not run on Windows or a device.

## A key per feature, shapes by hand, a hook with a transition (0.1.0-beta84)

Asked for by the owner, all in one go:

- "have a key for each of the major functions ... don't just have one filter key with a ton of
  filters": 13 switch keys and 3 move keys from `streamdeck/src/features.json`, one list each.
- "ratio setting buttons ... maintain that ratio for the active or hooked game until its over":
  the *Picture shape* key and `SetPictureShape`.
- "the hook button should have configurable transition options": *Change with* and *Lasting*.
- "I need every feature that makes sense as a key": also *Window switch* (Active App's next and
  back). **Not yet keys:** the dock's Snap on release, Guides and Stop resizing switches, and the
  slides (the pack has pictures for all of them); they need requests in `src/api/` first.
- **Asked, to be answered, not started:** a morph between scenes like the Standby Picture's: "detecting
  object and doing a morph". Plausible as an OBS transition that matches items showing the same
  source in both scenes and glides them from one place and size to the other while the rest
  cross-fades. Told to the owner; build only on their say-so.
- None of this has run on Windows or on a device.

## An updater inside the plugin (0.1.0-beta83)

The owner: "can you build an automatic updater? an application i can install that will be able to
download updates without me going to github every time", then "or build into the obs plugin an
updater feature?". Built into the plugin: nothing extra to install or keep running.

- A look at GitHub's release list ten seconds after OBS starts (and on *Check for updates now*), a
  line in the dock when there is something newer, *Update* to install it. See the README and
  AGENTS.md for what it does and the rules it keeps.
- **None of the Windows side has run.** First real test is the owner updating from beta83 to the
  release after it. If it fails: the helper script is `%TEMP%\stagekit-update.ps1` and can be read;
  OBS's log has an `Updates:` line for each look.
- Beta82's first build failed on Windows only (`setAlphaF(0.45)`: double to float is an error
  there). Fixed and published. Watch number types in Qt calls; the Linux build does not complain.
- Asked for next, not started: transition options for the Hook key; Stream Deck keys for setting a
  game's picture shape by hand, kept until that game ends; "every feature that makes sense as a key".

## The dock in four tabs (0.1.0-beta82)

The owner: "the stagekit dock is ugly and messy. id like it organized, and made visually appealing
and a polish UX/UI".

- Place / Arrange / Copy / Effects tabs; *Bring along* moved next to the move settings; curve and
  direction on one row; the tick boxes split into "How the tools measure" and "When I let go of an
  item"; drawn icons on the arrange and order buttons; the status line stays under the tabs.
- Every control kept its object name and behaviour; the tests only needed to open the Arrange tab
  before measuring where the order buttons sit.
- Not seen in OBS's own themes yet (the test picture is Qt's plain style). Ask the owner for a
  screenshot.
- The Background dock was left as it is.

## Hooking a capture by hand, with its own key and pictures (0.1.0-beta81)

- The owner on beta80: "the plugin seems to work fully", then: still "a bit clunky and not very
  aesthetically or visually appealing"; asked for icons for keys that hook a window or a game, and
  for the key itself; asked whether a key can switch Auto Capture on and off (yes: *Filter switch*).
- Done: `HookFront` in the OBS plugin, a *Hook capture* key in the Stream Deck plugin, and "hook
  game" / "hook window" pictures in all six palettes.
- **Next, asked for and not started:** "the stagekit dock is ugly and messy. id like it organized,
  and made visually appealing and a polish UX/UI". Also still open: what exactly feels clunky in
  the Stream Deck plugin (asked; no answer yet).

## Stream Deck plugin: first device test, palette per key, two more keys (0.1.0-beta80)

The owner on beta79, **on their own Stream Deck**: "plugin installed. i see two icons. they connect
to websocket. move works", and a Show in Scenes switch with the camera worked. So the settings
pages (the lists from the plugin, the shared connection fields) work on a device.

- Asked for and done: *Colour* is now *Palette* and is each key's own.
- "polish the app, make it more aesthetically appealing and simple. also add any other feature or
  QoL items": plain keys in each palette for filters the pack has no picture of (no more grey
  placeholder beside pack pictures); a tick on a Move key when OBS took it; the settings pages share
  one layout; two new keys, *Arrange tool* and *Blur everything*.
- Not confirmed yet: switch keys following a change made in OBS, the *Picture* list, the new keys.
- Ideas still open: keys that need no filter set up first (send the camera to a corner), a dial for
  blur amount, titles filled in automatically.

## The Stream Deck plugin's keys use the icon pack (0.1.0-beta79)

The owner asked whether the plugin's icons were the ones made earlier (they were not), allowed the
pack to be published, and chose mono as the default colour.

- 187 pictures in six colours are in the plugin (6.7 MB of 144 px files; the packed plugin is
  4.4 MB). *Picture* (Automatic or any of the pack) per key, *Colour* for all keys at once.
- Automatic covers Ramp Blur (by type), Corners & Edges (by style), Shadow, Glow & Outline (by
  look), Send To and Place Item (by place), Auto Capture, Auto Frame, Show in Scenes and Standby
  Picture. The other kinds show the plain switch until the pack has pictures for them.
- The blur-type pictures had no "off" look; `import-icons.py` makes a greyed one.
- Still untested on a device, like the rest of the plugin.

## A Stream Deck plugin, first step (0.1.0-beta78)

The owner: "Can you start working on a stream deck plugin?" with a link to Elgato's getting-started
page (not reachable from the session; see AGENTS.md for what it was built from instead).

- `streamdeck/`: two key types, *Filter switch* (two states, follows OBS) and *Move* (runs a trigger
  filter). One connection to OBS for all keys, set in any key's settings, kept in Stream Deck's
  global settings.
- CI builds it (`streamdeck-build` in `build-project.yaml`) and the release carries
  `stagekit-<version>-streamdeck.streamDeckPlugin` (a stage in `publish-release.sh`).
- **Untested on a device.** Ask the owner what they see: whether it installs, whether the lists
  fill, whether a key switches a filter and follows it.
- Ideas not started, for the owner to choose from: keys that need no filter set up first (send the
  camera to a corner, arrange tools, blur everything), which need new requests in `src/api/`; key
  pictures from the owner's icon pack; a dial for blur amount on a Stream Deck +.
- The beta77 fixes were confirmed by the owner's closed-session log: 99.2% of frames on schedule
  (97.6% on beta76), 99.83% drawn inside their time, and no capture stop from a scene switch.

## What the timing log found (0.1.0-beta77)

The owner sent three logs (private, not in the repository); the beta76 one had 96 timing lines.

- **Window scans on the drawing thread.** `stagekit_auto_capture` ticks up to 22–73 ms and
  `stagekit_active_app` up to 87–361 ms, in almost every 30 s window. Now on a thread of its own.
- **Send To.** Up to 17% of the time while moving, longest tick 40–72 ms (the start of a move,
  measuring the camera in every scene). Now one look per source per press, and cheap steps.
- **The game capture stopped and re-hooked at every scene switch** (28 times in the session). Auto
  Capture now keeps it shown (*Keep capturing in other scenes*).
- **Not StageKit:** the capture card source logs `Video configuration failed` each time a scene
  with it is shown (18 times); its device was not connected. Told to the owner.
- Fine in the log: Fit Item (17 filters, 0.01 ms a tick), Shadow (27 filters, 0.05 ms a draw),
  Standby, Auto Frame, Ambient Background (0.1 ms a draw).
- The owner on beta76, before this: "smooth but still frame drops".
- Confirmed by the owner: *Bring-along items too* works.

## A timing log, and free space waits for moves to end (0.1.0-beta76)

- The owner on beta74: "definitely smoother overall, but switching scenes or making moves does cause
  frame drops at least during the animations".
- No longer guessing: `register_timed` writes `timing, last 30 s:` lines to the log for whatever is
  slow. **Next step is the owner's log** from a session with scene switches and moves.
- Fit Item's free space no longer chases a moving item (see AGENTS.md).
- Still unproven suspects for drops during a scene switch: OBS draws both scenes during a
  transition, so every StageKit filter in both is drawn twice; and a shadow sized in screen pixels
  rebuilds its textures whenever its source is shown at another size.

## Lag: free space only for scenes on screen; Show in Scenes takes bring-along items (0.1.0-beta75)

- The owner, before trying beta74: "live feed and animations are both very laggy. framerate is
  dropping a lot", with OBS's Stats showing 53 fps and 2.1% of frames missed to rendering lag, and
  "OBS also appears to be constantly writing to disk".
- Found by reading: a Fit Item on *The free space around it* measured its scene's items whether or
  not the scene was shown anywhere. It now stands still unless its scene is being shown.
- **Not explained:** the disk writing. In the owner's Task Manager picture the megabytes a second
  are a browser page's (a browser source), not OBS's own process, which wrote about 9 KB/s. StageKit
  writes to the log only on events. Asked for the session's log to see whether anything repeats.
- If beta75 is still laggy: ask for the log and for Stats with the StageKit filters switched off one
  kind at a time; suspects in order are Standby Picture's copy of every capture, Auto Frame's
  read-back every 0.4 s, and the dock's 0.4 s refresh.
- New: *Bring-along items too* in Show in Scenes, as asked.

## Corrections glide; free space no longer makes moves choppy (0.1.0-beta74)

- The owner: "snap and resize should glide if able". Done in the dock, over its glide time and curve.
- The owner: "animations when the camera moves witht he buttons, and some others are very choppy".
  **Cause not proven.** The suspect was Fit Item's free-space measuring (new in beta70, which the
  owner had just switched on): a read-back every quarter second and a full round after every move.
  It now follows moving items by their boxes and reads back one item a second only while nothing
  moves. *Keep it on screen* also looks only a few times a second now. If moves are still choppy on
  beta74, ask which moves and whether it goes away with Fit Item set back to *The whole screen*; the
  next suspects are the dock's 0.4 s refresh (it now walks every scene's items for *Sources to copy
  to*) and Standby Picture drawing every capture through a copy.
- The owner did not remember Active App's *Captures you switch to* (beta60); it was explained again.

## "Size it by hand" removed (0.1.0-beta73)

The owner asked for Fit Item's *Size it by hand* (beta71) to be removed. Confirmed by the owner on
their machine the same day, nothing further to check for these:

- beta72: both tick boxes in the filters work.
- beta71: *Keep it on screen* works; the Ambient Background shows its moving picture again.
- beta70: *The free space around it* "works great".
- beta69: copy transform to other sources works.
- beta67: the resize past the empty widget works.
- beta68: the standby picture is right at start, on opening an emulator and into a game.

The crash on closing with the X: no crash file was written (the newest was a day old); the owner
said to ignore it for now. If it comes back, ask for that session's log.

Still not reported: Active App's captures switched to by hand (beta60); whether Shadow/Corners
switching and snap/resize-limit moves should animate; the real streaming test.

## Filters measure by their own options, not the dock's (0.1.0-beta72)

The owner, on beta70's *Measure by Arrange options*: the dock's options are for what is being edited
with them; a filter needs its own toggles. Fit Item, Place Item and Send To now have *Use visible
pixels* and *Ignore shadows & outlines* each. See AGENTS.md. No new test: the dock tests that tick
the dock's boxes and then send still pass, because the dock passes its options along.

## Shadows kept on screen, a box sized by hand, standby ignored by the background (0.1.0-beta71)

Three requests from the owner in one release:

- Shadow, Glow & Outline: *Keep it on screen* (off by default). See AGENTS.md for its limits.
- Fit Item: *Size it by hand* for *A box I set*.
- Ambient Background: *Ignore a standby picture* (on by default).
- The owner on beta70's *The whole screen*: 16:9 games fill the screen and other shapes keep the
  margin, "not sure why ... but I'm not complaining". Told: that is the design.
- **Open:** the owner had OBS crash on closing with the window's X "after making a lot of changes".
  Version not stated; no crash file yet. Asked for it. Nothing found by reading the code: filters
  are removed from a source before it is destroyed, so Fit Item's signal disconnect is safe.

## Fit Item: the free space around it (0.1.0-beta70)

The owner asked for the fitting and resizing filters to follow *Use visible pixels*, *Ignore shadows*
and the edge limit, and for a "fill space" choice in Fit Item's *Fit it in* that keeps the margin
from the screen and from other objects.

- Done for Fit Item: *The free space around it*, *Measure by Arrange options*, *Items never in the
  way*, a glide with a curve. See AGENTS.md for how it measures.
- **Not done, told to the owner:** Match Item and Layout still go by boxes. The gap is exact on the
  sides that limit the picture; a picture of another shape than the space is centred in it.
- Not run in OBS. Watch for: the item fighting a drag (it waits 0.4 s after the last move), and
  cost with many items (one small draw and read-back every 0.25 s).
- Beta68 confirmed by the owner: standby picture right at start, unchanged when an emulator opens,
  smooth to the game, also with a full-screen game.

## Copy transform to other sources (0.1.0-beta69)

The owner: "copy transform to should also allow selecting specific other sources to apply the
transform to, not just other scenes. assume when not specifically set, it only applies to the same
source in other scenes."

- *Sources to copy to* in the dock's *Copy transform* box, the same kind of list as the scenes.
- My reading, told to the owner: listed sources take the transform *instead of* the same source
  (add the item's own source to have both), and in the current scene too. The transform is literal
  (scale, not size on screen). Change either if the owner says so.

## Standby Picture after Auto Frame, and a real fade back (0.1.0-beta68)

The owner, on beta67: the standby picture started cropped before any game, took an emulator's shape
when the emulator opened, shook and cut to the game, came back at once and kept the game's shape.

- Not seen on their machine; deduced: their Standby Picture is before Auto Frame in the list (the
  placing ran once and only moved it up), so Auto Frame trimmed the picture by the held shape. The
  filter now moves itself after an Auto Frame whenever it finds itself before one.
- Reproduced in a test: Corners & Edges after an Auto Frame cut the standby picture to the last
  game's rectangle. Auto Frame's `get_picture` now gives no rectangle while the picture stands in.
- The last frame of the capture is kept and fades into the picture. The capture is therefore always
  drawn through an 8-bit copy, also when settled; watch for any change in how a game looks.
- **Not confirmed by the owner.** If the shape still goes wrong, ask for the OBS log of that session:
  Auto Frame's "is now WxH" lines say what it was looking at.

## An item that draws nothing is not in the way (0.1.0-beta67)

The owner, on beta66, with three screenshots (a window capture resized towards the right stopped
455 px from the screen's edge; the empty music widget's box sat there): "use visible pixels seems to
be ignored since items that are not currently visible, like the Amuse widget, seem to be affecting
the edge resize detection".

- `measure_item` returned an item's whole box when nothing was drawn in it. That is right for the
  item a tool is moving, wrong for an obstacle. `others_in_now` and `snap_lines_now` now leave such
  an item out. So this also changes slides, snap on release and the guides, not only the resize
  limit.
- **So *Stop resizing at edges* works on Windows**: it limited the resize, only against the wrong
  thing.

**Confirmed by the owner on Windows (2026-11-14)**: the Filters window is no longer pale (beta61;
so the wide-colour surface was the cause); OBS closes without freezing "for now" (beta58); *Copy
transform to chosen scenes* "seems to work" (beta59); *Show in Scenes* "works as intended" (beta64);
the transition choices "partially tested and so far works" (beta66). The icon pack is finished.

**Not yet reported on**: the manual window toggle in Active App (beta60), the Standby Picture's
morph with an emulator game (beta63), and whether to animate Shadow and Corners switching, snap and
the resize limit.

## Transitions you can choose, everywhere (0.1.0-beta66)

The owner: "Any filter or feature/effect that would do a transition or transform should have those
transforms and transitions be customizable, and not only offer fade. fade should be the default but
there should be other options as well as timings and such."

- **Audit.** Already had a choice of curve and timings: Ramp Blur, Ramp Zoom, Send To and Place Item,
  Show/Hide Animation, Layout, Match Item, Auto Frame; Active App already offered any OBS transition.
- **Added.**
  - `src/common/item-transition.*`: one *Change with* / *Lasting* pair. Active App now uses it; Show
    in Scenes, Program Trigger and Visibility Link gained it (default fade, 300 ms). Note that with
    the default these three now put a fade on the items they show and hide, where before they left
    the items' own transitions alone; *What is set on each item* restores that.
  - Standby Picture: *Change with* (fade, cut, slide and wipe in four directions, zoom and fade),
    separate times to the capture and back, the easing curve and direction, and *Size glides too*.
    The picture now also animates back in when the capture ends instead of appearing at once.
  - The dock's Send to: *Motion* (curve and direction), saved with the collection
    (`ArrangeOptions::send_curve`, `send_mode`) and written into the keys the dock makes.
- **Still fixed, on purpose or not yet done**: Shadow, Glow & Outline and Corners & Edges switch on
  and off at once (no ramp); snap on release and the resize limit jump to place; Fit Item has no
  timing because OBS fits it every frame; Copy transform is instant. Offer these to the owner.
- Stream Deck icons were also made this session (79 per colour choice, SVG and PNG, six colours),
  from the owner's scene layouts; they live outside the repository and are not committed.

## Stop resizing at edges (0.1.0-beta65)

The owner: "I'd like to be able to resize objects the way i usually would, like dragging an edge to
extend it, or scale, etc, but I'd like to have a border/edge limiter set up that would prevent the
object from being extended or scaled ... to a point that it would collide with another object, or
come within the specified pixel range. for example if the padding is set to 30px, dragging a box edge
... down to the bottom of the screen would prevent it from ... extending past ... once it is 30px
from the edge, or from another object. it should follow the same visible pixels and other rules."

- New Arrange option *Stop resizing at edges* with *Resize limit* (`ArrangeOptions::limit_resize`,
  `limit_px`, default off and 30). In the dock and the preview menu.
- It works on release, not during the drag (see AGENTS.md for why). Told to the owner plainly; if
  they want it to hold during the drag, that needs a different approach and may flicker.
- `limit_resize()` compares the item's box with its box when the drag began, limits each side pulled
  outward to the limit short of the screen's edge and of items that were ahead of it and level with
  it, scales evenly when the drag was even, and makes two passes because what is visible is not
  always the same share of the box at another size. Bounding-box items have their bounds scaled.
- The dock's watcher now tracks each selected item's box as well as its position, so a resize that
  does not move the item's position is seen. Snap on release now skips an item that was resized.
- Not covered: rotated items (skipped), items in groups (uses `scene_box`, untested), cropping with
  Alt-drag (changes the box like a resize and would be "limited" by scaling; untested).

## Show in Scenes (0.1.0-beta64)

The owner, with a screenshot of a Stream Deck multi-action holding eight Source Visibility steps:
"I'd like a filter that can be applied to a source and be configured to a stream deck key that allow
adding a source, or multiple sources, and a scene, multiple scenes, or all scenes that contain that
source, and allow to hide or unhide that source accordingly. it should follow the intuitive design as
other tools built."

- New filter `stagekit_show_in_scenes` (`src/placement/show-in-scenes.*`) for any source. Lists:
  *Sources to show and hide* (empty means the source it is on) and *Scenes* (shown only when *In* is
  *The scenes listed below*). *When this is on*: shown or hidden; off does the opposite.
- It walks every scene (skipping groups listed as scenes, looking inside groups) and sets item
  visibility on the `enable` signal. A *Do it now* button applies the current state.
- Known gap: the key's state and the items can disagree after a change by hand; the next press sets
  them all the same way, which can take two presses to get what is wanted.
- Still **asked, not built**: the limit while resizing by hand (see the beta63 section).

## Standby Picture keeps its shape and morphs (0.1.0-beta63)

The owner, on beta62, with a screenshot (the start image cropped to a near-square box in the game's
place): "when I launched an emulator game, the start image placeholder instantly changed ratio then
after a while switch directly to the emulator capture without a smooth transition. I'd like the start
image or whatever placeholder is selected to maintain its ratio, and smoothly morph or transition to
the ratio of the live capture ... Also ending the game did not revert the placeholder back to its
original ratio."

- So the filter works on Windows: a Game Capture does report no size when nothing is hooked.
- While waiting the size is now the picture's own shape at the capture's last height (`own_shape`,
  default on). The size is the filter's own and always glides (smoothstep over `fade_ms`, now 800 by
  default and labelled *Change over*) to wherever it should be, so a filter before it changing shape
  cannot make it jump. During a glide the capture and the picture are each drawn to cover the size
  of the moment into their own texrender and cross-faded.
- Not seen on Windows yet. With an Auto Frame before it, the capture's size keeps changing for a
  while after a game starts (trimming, gliding); each change restarts the glide from where it is.
  Whether that reads as one smooth morph on the owner's machine is the thing to ask.
- **Asked, not built yet**: a limit while resizing by hand. "I'd like to be able to resize objects
  the way i usually would, like dragging an edge ... but I'd like to have a border/edge limiter ...
  that would prevent the object from being extended or scaled ... to a point that it would collide
  with another object, or come within the specified pixel range ... it should follow the same
  visible pixels and other rules."

## Standby Picture (0.1.0-beta62)

The owner: "is there a possibility of adding a new feature/functionality/filter that allows the start
image, or other media to take the place of game or window capture, or other live feed when it's not
active, nothing is captured, etc? as a place holder so there isn't just a blank area"; then "you can
build a placeholder, though it should be configurable to be enabled or disabled. Start image should
be the default for the placeholder."

- New filter `stagekit_standby` (`src/capture/standby.*`, shader `data/effects/stagekit-fade.effect`).
  While the capture's own size is zero it draws a picture, covering the size the capture last had
  (saved in `last_width`/`last_height`; 1920x1080 before it has ever shown anything) or the size the
  filter before it gives. It is there at once when the capture goes and fades out (`fade_ms`, 300)
  when it comes.
- Picture: the start image (a private Start Image source, so it follows the dock's choice), a file
  (image or looping muted video, by its extension), or another source by name.
- "Enabled or disabled" is the filter's own eye; no second switch was added.
- When first added it moves itself to just after an Auto Frame, else to be applied first (`placed`).
- Not yet seen on Windows. The shader is new and only proven on OpenGL. Whether a Game Capture
  and a Window Capture really report no size when nothing is attached on the owner's machine is
  assumed from OBS's behaviour, not tested. No contact sheet was made for the README.
- Not covered: Auto Frame before it keeps its frame's size when the capture is empty, which is what
  makes the picture fill the frame; that pairing is not tested.

## Pale pictures in the Filters window (0.1.0-beta61)

The owner: "all the sources with the new filters have a whiteness added to them, compared to the
actual canvas viewer"; then "turning off the filter makes the whiteness go away. the whiteness occurs
with multiple different filters. no HDR enabled."

- **Cause, reproduced in the tests**: Shadow, Glow & Outline and Auto Frame (and the Ambient
  Background source) were not flagged `OBS_SOURCE_SRGB`. OBS then draws them with linear-light
  handling switched off. On an 8-bit surface that makes no difference, which is why the canvas and
  the earlier tests were right; on a 16-bit float surface holding linear light, ordinary values land
  unconverted and look pale. `render_shown` in the tests draws onto such a surface: both filters
  were off by about 46 levels per channel before the flag and 0 after.
- **Assumed, not seen**: that the owner's OBS gives its Filters window such a surface. HDR is off;
  their Windows is a recent preview build, where colour management for standard displays may be on.
  If beta61 does not cure it, that assumption is wrong and the next thing to ask for is a screenshot
  of the same source's Filters window with a StageKit filter that already had the flag (Corners &
  Edges alone).
- **Agreed, not built yet**: the placeholder. The owner: "you can build a placeholder, though it
  should be configurable to be enabled or disabled. Start image should be the default for the
  placeholder. What's 'nothing to show' referring to?" (answered in chat: a capture with no game or
  window hooked, as opposed to one showing a black picture).

## Active App: captures switched to by hand (0.1.0-beta60)

The owner: automatic switching between configured apps works, but a window captured by hand (the
foreground window, with a third-party Stream Deck key) is taken over as soon as a configured app is
clicked into. "I'd like a feature that lets me pick a window to capture, such as a web browser
window, and manually toggle to and from it, even when automatic switching is set up for other apps
... it would stay active until I toggle back. Then automatic switching ... would pick up again ...
configurable options for switching and toggling between multiple captured windows ... assignable to
Stream Deck keys or hotkeys."

- New list on Active App, *Captures you switch to* (`by_hand`, a PickList of the scene's items).
  While one of them is visible it is held: the automatic captures are hidden and nothing follows the
  apps. Hidden again, the filter works out afresh which app's capture to show. One newly shown hides
  the others on the list.
- Toggled by ordinary item visibility, so a Stream Deck source key or OBS's item hotkeys work with
  nothing new to learn. Two hotkeys per filter on the scene: step through them then back to the apps
  (`stagekit.active.next.<filter>`), and straight back (`stagekit.active.back.<filter>`).
- Tests drive the two actions through procs on the filter (`step_by_hand`, `back_to_apps`); the
  headless tests cannot press a hotkey.
- The whiteness reported the same day is explained and fixed in beta61 (above).
- **Asked, not built**: a placeholder (the start image or other media) where a capture has nothing
  to show.

## Copy transform to chosen scenes; clearer names (0.1.0-beta59)

The owner: "did we already develop a tool that can send existing settings or transforms to specific
copies of a source in other specific scenes?"; then "can you build that variant functionality, but
also clearly label the option or buttons as to what they do. Same in every scene is a bit ambiguous".

- *Same in every scene* is now **Copy transform to all scenes** (action name and hotkey id unchanged,
  so bound hotkeys keep working). New: **Copy transform to chosen scenes** (`ArrangeAction::SameChosen`,
  `same_chosen`), in the dock, the preview menu and the hotkeys.
- Both sit in a box of their own in the dock, *Copy to other scenes*, with the list *Scenes to copy
  to* under them. The list is `ArrangeOptions::copy_scenes`, saved as `stagekit.copy.scenes`. The
  owner did not answer whether it should be remembered; it is.
- `store_arrange_options` now starts from the current options, so options without a control in the
  Arrange box are not reset when a tick box changes.
- Asked but not built yet: a placeholder (the start image or other media) shown where a capture has
  nothing to show. See the next message to the owner for the proposed design.

## Quiet on close (0.1.0-beta58)

The owner, on beta57: "obs is crashing on close often, but it's just freezing up and not offering to
save the error log."

- **Cause not proven; no log of a frozen close received.** What was wrong on reading: nothing of
  StageKit's stopped when OBS closed. The guides' timer, both docks' timers, the snap watcher and the
  preview menu's event filter all kept running while OBS cleared its scenes (which pumps window
  events), asking for scenes and measuring items. They now stop at `SCRIPTING_SHUTDOWN`.
- **Other suspects on the owner's machine, outside StageKit**: a media source reading a network
  stream that keeps disconnecting (OBS waits for such a source's reader when it closes), and the
  third-party plugin whose callback crashed at startup in the one crash file they had.
- If closes still freeze on beta58: ask for the log of that session (second-newest in *Help → Log
  Files*). Its last lines show how far the shutdown got; StageKit now writes `OBS is closing: the
  docks, guides and measuring have stopped` when its part is done. A freeze after that line, with
  that line present, points away from the dock code.

## Confirmed by the owner on Windows (beta57, 2026-11-13)

"doesn't seem to be crashing ... either way it's much better", then, after being asked to tick *Snap
on release* with *Guides in the preview*, to watch for freezes, and to try a group with a slide and
*Same in every scene*: "everything seems to be working."

- No freeze on beta57 with guides and snap ticked, so the limited wait in `on_drawing_thread` was
  the fix. What the drawing thread was waiting for was never found.
- Guides show; snap on release works; groups work with slides and *Same in every scene*.
- The owner had taken the guides to snap by themselves. They only draw lines; *Snap on release*
  moves the item. If that confusion comes back, it is a wording or design question, not a bug.
- Not StageKit's: OBS sometimes dies at startup inside another plugin's frontend callback (the one
  crash file the owner had). Nothing to do here.

## The freeze: measuring no longer waits without limit (0.1.0-beta57)

The owner, on beta56: "I saw a blue outline in the preview. the guides seem to appear now, but it
crash after a few seconds of moving an object"; the newest file in OBS's crash folder was an older
startup crash inside another plugin, not StageKit, so the "crashes" leave no crash file; then: "obs
just freezes and becomes completely unresponsive".

- **Guides show on Windows since beta56** (the main preview picked by class). The report in the
  dock's message was removed again.
- **The "crashes" are freezes**: a deadlock, which is why there is never a crash file. Every one
  happened with guides ticked. From beta54 the window's thread waited without limit for the drawing
  thread to measure; what the drawing thread was waiting for is not known (no log from a frozen
  session was received; the log sent was the running session's). The wait is now limited to 400 ms,
  which ends any deadlock of that shape whatever its other half is. Beta53's freezes (drawing from
  the window's thread) were a different shape of the same thing.
- If freezes go on with beta57, the wait in `on_drawing_thread` is ruled out: get the log of the
  frozen session (second-newest in *Help → Log Files*) and look for `measuring: OBS's drawing thread
  did not answer`.
- Still unconfirmed by the owner: groups with slides and *Same in every scene* (beta54).

## Guides report in the dock; cameras not drawn bare (0.1.0-beta56)

The owner, on beta55: "i saw no frame flash. the app did crash after moving an object and click off
the canvas" (guides ticked, snap unticked); "the slide edge appears good now but it crashed again
while doing a slide move"; "games don't appear to crash". Earlier the same day: "bring along seem
more accurate now, same width works, stream deck keys work".

- **Confirmed on Windows by the owner**: the edge pixel fix (beta54), Bring-along flips (beta52),
  *Same width* following the last selected (beta50), Stream Deck keys after beta53, games no longer
  crashing with OBS open.
- **Crashes: cause still unknown, no crash report received.** They happen in a slide and in a drag
  with guides on, so the measuring both share is the suspect, not the guide windows. Checked against
  OBS 32's source: `obs_queue_task(OBS_TASK_GRAPHICS, …, wait)` does wait and runs after the frame's
  displays are drawn; `obs_source_skip_video_filter` on the filter applied first is what OBS itself
  does. What changed in beta53, the first version with crashes, is that sources playing video frames
  (the camera) are drawn bare and that every item is drawn twice. The first is undone here as a
  precaution. If crashes go on, the next step is the crash report, not another guess.
- **Guides**: neither windows over the preview nor child windows inside it show on their machine,
  though the dock reports them on. The dock now appends a report (widget class, size, position, how
  many widgets are named `preview`, where the canvas was found, how many lines were made, shown, and
  visible to Windows). Ask for a screenshot of that message. The main preview is now picked by class.
- The dock's *Ignore shadows & outlines* tick box lost its `&` (Qt took it for a shortcut mark).

## Guides inside the preview (0.1.0-beta55)

The owner, on beta53, with a screenshot of the dock saying *Guides on: a frame should flash round the
canvas in the preview*: "no crashing with guides and snap turned off".

- So the preview widget is found and the line windows are created and shown, yet nothing is seen:
  top-level windows over the preview do not work on their machine, for a reason not known (no log).
  The lines are now native child windows of the preview widget, in its own coordinates. This is the
  fourth design; it is untested on Windows like the others.
- The crashes come from the guides or snap path: either the measuring at the start and end of a drag
  (moved to the drawing thread in beta54) or the line windows. Ask whether beta55 still crashes with
  both ticked, and for the crash report if so.

## Groups; a pixel at the edge; measuring on the drawing thread (0.1.0-beta54)

The owner, on beta53: "seems better, but it may be leaving a pixel at screen edges" (two screenshots:
the widget at the top-left with edge distance 0, its disc a pixel short of the left edge); "obs keeps
crashing when moving and editing objects. also, still no guides"; "slide to next edge does not work
with groups"; "same in every scene also does not work on groups. make sure all the tools work the same
on groups".

- **So the beta53 slide fix worked on Windows** for the browser widget ("seems better").
- **The pixel**: not reproduced with their widget. A smoothed edge's outermost row of faint pixels
  was counted as picture; it is now left out when every pixel in it is under half its inner
  neighbour. Tested with an image built that way. Whether that was their pixel is not known.
- **The crash**: no crash report received, cause not known. The measuring drew sources from the
  window's thread (every item, at each drag start with guides or snap on, and at each button press);
  beta53 added drawing cameras and captures bare that way. All measuring now runs on OBS's drawing
  thread (`on_drawing_thread`). If crashes go on, the report is needed; ask whether they stop with
  *Guides in the preview* and *Snap on release* unticked.
- **Groups**: the dock's *Item* list never followed a selected group (`follow_selection` skipped
  groups), so the arrows acted on whatever was shown before; *Same in every scene* skipped groups
  outright. Both fixed and tested (a group slid; the same group in a second scene).
- **Guides**: still not seen by the owner on beta53, and they have not said what the dock shows
  after ticking the box (*Guides on…* or *could not find OBS's preview*), nor sent a log. Nothing
  changed this time; do not try a fourth design without that answer.

## Tools follow the Arrange options; guides, third try (0.1.0-beta53)

The owner: "the slide to next edge is not respecting the use visible pixels setting" (three
screenshots: a browser widget with a Shadow, Glow & Outline, stopped with its box at the edge
distance, both options ticked); "i need the tools to respect all the settings"; "i don't think guides
in preview are working yet. i've never seen them with the box checked"; "the transform same in every
scene works" (beta52, confirmed on Windows).

- **Not reproduced here.** The tests have no browser source. Two things were wrong on reading and are
  fixed; whether either was the owner's case is not known until they try it.
  - The bare picture was drawn with `obs_source_default_render`, which wraps the draw in OBS's default
    effect whatever the source is. OBS itself only does that for plain sources; one that draws itself
    (a browser) is drawn directly. `render_bare` now uses `obs_source_skip_video_filter` on the filter
    applied first, which is OBS's own way of drawing what is under the filters.
  - When the bare picture came back empty, the item's whole box was used. It now falls back to
    everything drawn, less the shadows' room.
- The bare picture is now also cut to what is drawn with the filters on, so trimmed edges and masks
  are not counted.
- **Send To and Place Item** kept their edge distance to the item's box whatever the options said.
  They now measure by the Arrange options (`as_arranged`, default on). This moves where existing keys
  land for an item with a shadow or see-through room: the picture, not the box, is the edge distance
  from the edge. The owner asked for the tools to follow the settings; the tick box undoes it.
- **A slide writes a line to OBS's log** (`[stagekit] Slide: '…' counts as …`): what it measured and
  the item's box. Ask for it if a slide stops wrong again.
- **Guides**: line windows no longer use `Qt::WindowTransparentForInput` (a layered window on
  Windows, like the see-through overlay that showed nothing); they do not take focus instead. Ticking
  the box flashes a frame round the canvas for 1.5 s and the dock says *Guides are on* or that it
  cannot find OBS's preview (`guides_preview_state`). Still not seen on Windows by anyone.
- Tests: a Cropped View (a source that draws itself) of the padded badge with a shadow is slid, sent
  to a corner under each combination of the two options, and sent with its edges trimmed.

## Same in every scene; own pixels; flips round the picture (0.1.0-beta52)

The owner: "id like a feature to paste transforms to all of the same source in all scenes ... similar
to the copy and paste transforms but to all scenes for the selected source/sources"; "i don't think
slide to next edge is respecting ignore shadows of other objects. also the ignore shadows should ignore
any of the outlines or shadows etc added by filters and force the detection to be based on the
source's actual pixels"; then four screenshots as "proof".

- **Same in every scene**: `ArrangeAction::SameEverywhere` (`same_everywhere`), so it is in the dock,
  the preview menu and the hotkeys. `copy_to_every_scene` sets alignment, bounds type, bounds
  alignment, bounds, scale, rotation, crop and position on every copy of each selected item's source
  in the other scenes. Position, scale and bounds go through `item_space` of both sides, so a copy in
  a group lands on the same canvas spot. Locked copies are skipped; one item per source (the first
  selected); groups listed by `obs_enum_scenes` are skipped. Undo snapshots every scene. The dock's
  13th Arrange button spans its row. Bounds crop (OBS 30.1+) is not copied: the headers the tests build
  against do not have it.
- **The screenshots** showed the slide itself stopping right (the camera's picture 30 px from the
  edges). What was wrong was *Bring along*: after a flip, the widget sat a shadow's width further from
  the camera than before. `plan()` mirrored the follower round the leader's box (shadow room
  included); it now mirrors round the leader's picture (`Measure{true, true}`), scaled to the leader's
  target size. A test with a 20 px shadow on the leader fails without the fix (gap 30, not 10).
- **Own pixels**: see the AGENTS.md entry. The label is now *Ignore shadows & outlines*. Test: a
  Corners & Edges border round the badge's whole source is counted when unticked and left out when
  ticked. The owner's collection has no other shadow or outline filters (only Color Correction and
  Render Delay besides StageKit's).
- A slip while adding the action (its `case` put before *Same size* so *Same width* and *Same height*
  fell into it) was caught by the existing size tests before anything was pushed.

**Not verified:** on Windows.

## Slide to the next edge (0.1.0-beta51)

The owner: the Send to arrows should also have "a second function where any one of those directions
moves the object in the chosen direction until it either would collide with the edge of another
object, or would collide with the edge of the scene, but instead of colliding, respect the edge
distance setting", "make sure the same settings like use visible pixels and all the others still work
with it"; then: "must have the object stop at the first edge it meets, but if pressed again can pass
through the object edge until the next edge it meets". They asked for it with "ultracode"; it was
written directly and checked by a short review workflow.

- Dock tick box *Slide to the next edge* (`sendSlide`, saved as `stagekit.send.slide` with the
  arrange options). The arrows' tooltips change with it.
- `slide_offset(scene, item, dx, dy, gap, how, leave_out)`: the item and every other visible item are
  measured with the Arrange options (`how`). Along each direction the stops for the leading edge are
  the screen's edge less `gap`, and for each item overlapping the path (by more than half a pixel
  across it) its near side less `gap` and its far side less `gap`; the nearest stop more than half a
  pixel ahead wins, none means no move. Diagonals do across, then down/up from the new place. The
  centre button centres between the nearest items on either side (or the screen's edges), both ways.
- Items in groups count on their own; a group in `leave_out` (or hidden) takes its items with it. The
  dock leaves out the item chosen in *Bring along*.
- The dock sends the result as a Send To with the new destination `Spot` (10): `spot_x`/`spot_y` are
  where the item's box starts on the canvas, applied per scene in that scene's coordinates.

**Review** (a workflow of three read-only reviewers and one skeptic per finding; 10 confirmed, 1
refuted, 2 not verified because of the cap). Fixed, each with a test that fails without the fix:
- a second press during the glide went back to the same stop: the dock remembers the last slide's end
  (measured box and item box) for the travel time plus 150 ms and starts from there;
- stops past the screen's edge (items partly off the canvas, oversized backgrounds) took the item off
  screen: stops beyond the screen stop are dropped;
- an obstacle thinner than the edge distance cost a press: its far-side stop is only added when it
  lies past its near side;
- *In every scene* applied a group-local saved position in other scenes: now `Destination::Spot`;
- `places_of(every scene)` also walked groups as scenes (OBS lists them in `obs_enum_scenes`), so a
  grouped item was moved twice, once as if the group were the screen. This affected the plain arrows
  and Stream Deck keys with *Every scene it is in* as well. Groups are skipped now;
- Studio Mode with *In every scene* unticked: the dock measured the preview scene and Send To moved
  the program scene. `send_with` now names the scene it worked on;
- presses during a glide made several Undo steps from mid-glide positions: they now make one step
  (`UndoChain`, grouped by time, so a late timer cannot mix two runs);
- the diagonal test could not tell "from the new place" from "from the old place"; test comments; the
  dock screenshot.
Refuted: that *In every scene* should keep each scene's own arrangement (it is documented as the same
place everywhere).

**Not verified:** on Windows.

## Same width, Ignore shadows, guides on Windows (0.1.0-beta50)

The owner, with screenshots of the dock and the preview: "Same width doesn't appear to work. I expect
that following the rules for align to, it would set the visible pixel width to match the last selected
object"; "Guides in the preview don't seem to work at all"; "ignore shadows feels like it has the
opposite effect"; and the Order buttons should be front/back in one column and forward/back one in the
other.

- **Selection order.** It was learned by the dock polling `selection_in_order(dock's scene)` every
  120 ms, which kept one remembered scene and cleared the order when asked about another. The dock's
  *Scene* list and the scene being edited differ whenever the owner picked a scene in the dock, so the
  order was cleared on every poll and "last selected" became the top-most item in the Sources list
  (the Amuse widget, not the camera). Now `track_selection_order()` connects to `item_select` and
  `item_deselect` on every scene and group (`source_create` plus `obs_enum_scenes` at load) and
  `selection_in_order` sorts by when each was selected.
- **Which scene.** The dock's Arrange buttons, its selection list and snap-on-release used the dock's
  *Scene* list; hotkeys and the preview menu used the scene being edited. All now use
  `arrange_scene()`: the scene being edited when anything is selected there, else the dock's.
- **Ignore shadows.** `measure_item` cut the shadow's room off the source's edges. A browser widget's
  picture stops short of its own edges, so its shadow was inside what was kept and counted as picture:
  the widget plus its shadow was made as wide as the camera's picture. Confirmed by a test that fails
  with the old measuring (scale 0.667 where 1.000 is right). Now `shadow_free` (in
  `src/shadow/shadow-backdrop.*`) gives what the first enabled Shadow, Glow & Outline draws from (a
  filter, or the source drawn with `obs_source_default_render`) and where its picture sits; an async
  source shadowed directly falls back to the old cut. Unticked, a screen-pixel shadow keeps its size
  while the item is resized, so `match_size` resizes only the picture to make up the difference.
- **Guides.** Nothing showed on Windows; no log came with the report, so the cause is not known. Two
  suspects were replaced: the single see-through (layered) window over the whole preview is now one
  thin opaque window per line, and the mouse button is also read with `GetAsyncKeyState` (Windows only,
  compiled by CI). OBS's log now says when guides are switched on and how big the preview is, when a
  drag is first seen, and where the first line was put, once each.
- OBS 32's preview was checked in its source (sparse clone of obsproject/obs-studio, not committed):
  the widget is still `OBSBasicPreview` named `preview`, a native window (`WA_NativeWindow`,
  `WA_PaintOnScreen`), and `ResizePreview` uses `PREVIEW_EDGE_SIZE` and `GetScaleAndCenterPos` as
  `preview_map` assumes.
- The dock's Order box: *Bring to front* above *Send to back*, *Forward one* above *Back one*.

**Not verified:** all of it on Windows, and above all the guides.

## First sight in the dark; shapes remembered (0.1.0-beta49)

The owner, after beta48: "most things are working as expected. Game capture still takes a while to
start capturing windowed games, and it still tends to take a long time to register the ratio of a
windowed app, especially one that is more modern. setting up the measurements and saving them in the
filter manually helps but i wish the automated feature worked better." No log was sent.

**Auto Frame.** beta47 made first sight safe by asking for straight, lit edges on both sides (15% of
strips). A dark modern game in a window does not give that for a long time. Four changes:

- **What counts as lit** is now relative to the capture's own ends: the darkest level along the two
  end lines (capped at 16) plus `kLitStep` 4. Bars are flat, nearly always pure black; a night scene is
  not. It was a fixed 16.
- **Fitted** (`fitted` in `look_for_bars`): a first sight is also clear when the picture has been seen
  to reach both ends the other way (`across_full`), to stop the same distance in from both ends this
  way (`even`), and that leaves a real shape with nothing trimmed the other way. It uses
  `Bars::outer_lo/hi`, the furthest out the picture has been seen from each end (remembered for
  `kOuterMemory` 5 s, moved at once by anything seen further out). A few lit pixels per side do.
  Artwork or a logo on black never reaches the ends. **Only for a direction not yet confirmed**: once
  confirmed, a change still needs straight edges and the hold time, so cut-scenes do not resize.
- The ends are robust: the `across / 50` furthest-out strips on each side are left out (one of 90, three
  of 160), so a pointer or a speck neither blocks nor fakes an edge. Fewer lit strips than that is black.
- **Eager looks**: after a new size, a look every 0.1 s (0.2 s on black) until both directions are
  confirmed or `kEagerLooks` 80 are used up (a black look costs a quarter). It was 0.4 s. Each look is
  still one small readback; the budget is what keeps the cost bounded.
- A direction with nothing trimmed is confirmed by the picture reaching both ends, no straight edge needed.

**Remembered shapes.** Setting `learned` ("game.exe = 16:9, multi.exe = varies"), written by the filter
itself. A shape that has stayed confirmed for `kLearnAfter` 15 s (hidden setting `learn_after`, used by
the tests) is kept for the program; the next time that program appears it is `usual_ratio_`, ahead of
`kKnownPrograms`, so the trim is in place from the first frame, black or not. It is unconfirmed like
any starting point. A program later seen with another shape, or filling a window of no shape, becomes
`varies` and starts from nothing. Under the same option as the built-in shapes (`known_programs`,
relabelled). *Forget shapes seen before* clears it; its tooltip lists it. Once per appearance of a
program (`learned_this_time_`).

**Auto Capture** (Windows-only code, compiled by CI, never run here): a windowed program now also
counts as a game when it has a graphics engine plus a game store library, Unity's player, FMOD, Bink
or DLSS, or `windows.gaming.input.dll`, or when its window class is a game engine's. The scan is
every 0.25 s. **Whether detection was the owner's delay is not known.** Their emulators are on the
*Always capture* list, which needs none of this; for those the wait may be OBS's own hook or the
emulator's black start. The log now has the times; it has been asked for twice.

**Owner, on Windows with beta49 (2026-11-13): "capture speed and ratio seems better."** Before
that, on beta48: "most things are working as expected". No log was ever sent, so which of the changes
shortened the capture's start is not known (detection, the hook, or only the shape registering
sooner), and the remembered shapes have not been reported on separately.

**Not verified in detail:** the tests paint pictures: a dim one, one with a few lit specks per side,
one whose specks stop at different distances (must not trim). Nobody has reported on a program that
shows more than one shape.

## Active App: the capture of the app in use (0.1.0-beta48)

The owner, about the two window scenes: after Blender, ZBrush or Fusion closes "the original app
capture never comes back and stays hidden"; with several open "the capture follows" the active window
in their old setup, and that "should be configurable"; switching should "transition like fade or other
transitions that are configurable"; and, spelled out: listed apps are followed while active, a window
selected for capture or captured before is switched to, and a listed app that is no longer in front
stays captured until another listed app is.

What the old collection did (read from it and from the Advanced Scene Switcher export): one macro per
program with a *process has focus* condition, showing that program's item and hiding the others, and a
250 ms fade as each item's own show and hide transition (350/300 on the plain capture). Nothing brought
the plain capture back. The rebuild had replaced this with a "one at a time" Visibility Link plus three
"while it is open" Program Triggers, which neither followed the focus nor came back, and had no fades.

- `src/capture/active-app.*`, id `stagekit_active_app`, a scene filter. Settings: `items` (PickList),
  `follow` (default on), `transition` (an OBS transition id, `none` for a cut, `keep` to leave the
  items alone; default `fade_transition`), `transition_ms` (250), `ready` (default on).
- Each item's program is the executable in its source's `window` setting. No program names are typed.
- It acts on events. Of these, a later one wins: an app that opened (when following, only if it is
  the one in front); a capture re-aimed by hand; the app clicked into (when following). If none of
  those happened and the shown app has closed, it goes back: the most recently shown that is open,
  else the first open in list order, else the first item. Otherwise nothing changes, including when
  a program not on the list is in front.
- Transitions are OBS's item transitions, set with `obs_sceneitem_set_transition`; OBS starts them
  itself inside `obs_sceneitem_set_visible`, so the two items cross-fade. Confirmed by the tests.
- `ready`: `obs_source_inc_showing` on the sources of listed items whose app is open, only while the
  parent scene is showing; balanced in `keep_ready`, released on detach, disable and destroy.
- `open_windows()` is now cached for 0.2 s (was 0.5 s) so following feels immediate.
- `scripts/upgrade-collection.py` replaces the Visibility Link and its Program Triggers with one
  Active App in such scenes; `scripts/rebuild-collection.py` writes Active App from the start.
- The owner's current collection, upgraded: `StageKit current + app switch.json` (not committed).
- **Not verified:** all of it in OBS on Windows. In particular whether a Window Capture that is kept
  showing really keeps capturing while its item is hidden, and how quickly a hidden capture has a
  picture when it is not kept ready. An earlier draft of the rebuild that carried item fades crashed
  OBS on import (cause never found); these are made by OBS's own calls at run time, not written by hand.
- A test artefact worth knowing: in a scene that is never drawn, an item hidden with a hide transition
  keeps its source active, so the kept-ready test uses a scene of its own with cuts.

## Shape seen before it is held; Send To in every scene (0.1.0-beta47)

The owner, with screenshots: a widescreen game in a window (1796x1147 surface, 16:9 picture inside
with bars above and below) "opening as limited ratio. took a long time to eventually register
correctly"; then "taking a long time to capture windowed games"; then Send To should move the source
"on every scene it's in" by default, configurable.

**Auto Frame.** Measured from the screenshot: the crop was exactly square (1147x1147) with the
window's own bars still inside it. Which path put the square there is not known (no log); three were
open and all are closed:

- `held_ratio_` was set every frame from the current crop, before any picture had been seen: from a
  black screen, or from a transient first size. It is now set only when both directions are
  `confirmed` (the picture seen to end exactly at the trim, both sides), and cleared when a confirmed
  picture fills a size that is no picture shape.
- More is trimmed only on a **clear** sight: both edges straight (`kEdgeShare` 15% of strips within
  `kEdge` 2 px), the candidate held for two looks, and the result a picture shape. Shape alone is no
  longer enough, so a logo whose width leaves 4:3 is left alone. `look_for_bars` now scans every strip
  (`strip_lo_`, `strip_hi_`) instead of whole lines; same single readback per look.
- An unconfirmed direction (nothing yet, a program's usual shape, a shape carried to a new size) acts
  on a clear sight at once and gives up after `kGiveUp` 1 s of the picture showing inside it. A
  confirmed one waits `trim_wait`, or `kInsideFor` 4 s of the picture showing inside.
- "Inside" needs `kInsideShare` 30% of strips, so a pointer, pop-up or menu strip is ignored. The old
  rule released after six seconds of anything steady there.
- `alone`: when the other direction's trim is unconfirmed and this direction explains the picture as
  a real shape on its own, this one is applied and the other dropped. In the screenshot's state the
  bars above and below could never be trimmed because 1147x1011 is no shape.
- Stability is timed with a clock (`clock_`), not counted per look: quick looks used to count 0.8 s each.
  Quick looks are now used up by black frames too, so a long loading screen is not read back every frame.
- The program is checked four times a second, and at once when the size changes.
- Decisions are written to OBS's log (`Auto Frame: <program> left/right trim a -> b px at WxH (why)`).

**Do not** go back to trimming on shape alone, or to holding a shape that was not seen. Still true from
before: no brightness-only rule, and the held shape survives a resolution change.

**Windowed games.** The owner's collection already has the Game Capture's hook rate on fastest, and
Auto Capture picks up a remembered program within half a second, so the likely delay was Auto Frame's
(a window is where a picture has bars the capture must find). Unproven. `loaded_engines` now looks a
second apart for the first five looks (twelve at most). If it is still slow, the log has the times.

**Send To.** New setting `apply_in` (`ApplyIn`: 0 every scene, 1 the scene on screen, 2 one chosen
scene named in `apply_scene`), default every scene. The old `where` (0 on screen, 1 every) is **not
read any more**: the dock wrote 0 into every key it made, so honouring it would have left the owner's
nine keys on the old behaviour. `in_scene` still wins when set (the dock's own move filter, for the
scene being worked on). The dock's tick box is `ArrangeOptions::send_every_scene`
(`stagekit.send.every_scene`); undo for an every-scene move uses `transform_snapshot_all`.

**Not verified:** all of it on Windows. The Auto Frame tests use painted pictures, not a capture.

## Guides in the preview (0.1.0-beta46)

The owner: "I'd like visual indicators in the viewer for alignment if possible". They also confirmed
that the preview's right-click *StageKit* menu works on Windows, which is the same kind of unsupported
hook.

- `src/dock/preview-guides.*`: `GuideOverlay`, a frameless `Qt::Tool` window (translucent, transparent
  for input, never focused) parented to OBS's main window and laid over the `preview` widget. A 33 ms
  timer shows the line a line-up tool just used (`last_arrange_guide`, 0.9 s) and, while the left
  button is down and the selected items are moving, the nearest line each is within reach of.
- `snap_item` is now `snap_lines` (gather once) plus `meet_lines` (pure), shared with the overlay.
  `align_items` reports the line it used.
- Option `guides` in `ArrangeOptions` (`stagekit.arrange.guides`), default **off**: nothing is created
  on screen unless the owner ticks it.
- **Unverified in OBS**: whether `preview_map` matches where OBS 32 really draws the canvas (it is
  written from memory of `OBSBasic::ResizePreview`: `PREVIEW_EDGE_SIZE` 10 and `GetScaleAndCenterPos`),
  whether a layered tool window composites over the preview's Direct3D surface, and high-DPI screens.
  The tests cover the sums, the overlay's geometry over a stand-in widget, and when lines appear and go.

## Fit Item in real time; the owner's current collection (0.1.0-beta45)

The owner wants window captures to behave like the game capture in their window scenes and, as in
their old collection, to "resize smoothly in realtime and grow to fit the space I set". They then sent
their current collection (a StageKit rebuild they have since edited in OBS).

- **Fit Item**: the bounds are now the available space grown by the shadow room at the fitted scale
  (`aw + (l+r)k`, `ah + (t+b)k`) and the position is the space's corner less `l*k`, `t*k`. With
  scale-to-inner-bounds and centre alignment, OBS then lands the picture in the space on every frame;
  the box only changes when the picture crosses between "fills" and "floats" or the room changes.
  beta41 set a box of the picture's exact size, which was right one tick late. The tests now render
  the scene and look for the picture, instead of reading the item's numbers.
- **`scripts/upgrade-collection.py`** brings an existing StageKit collection up to date without
  rebuilding it: Fit Item for window captures that sit in a centred scale-inner box (whole screen with
  the collection's own margin, or the box they had), Auto Frame's picture height to the canvas, and
  `flip_linked` on for filters that bring an item along. Everything else in the file is left alone;
  a diff of the owner's file showed exactly those changes and nothing else.
- What the owner's file showed: Fit Item already on the three game scenes, none on the two window
  scenes; the seven generated *Camera ...* Send To filters with `flip_linked` false while the nine
  dock-made keys flip; and **Snap on release switched on**, which is what shifted items after a Send To
  move before beta40. The upgraded copy was sent to them as "StageKit current + window fit".
- Judgment call, told to the owner: the full-screen window scene got the game scene's 90 px margin.
  Their old collection fitted windows edge to edge there; margin 0 gives that back.

## Hexagon blur (0.1.0-beta44)

Asked for by the owner. `BlurType::Hexagon = 6` (appended), technique `Hexagon` in
`data/effects/stagekit-blur.effect`: the hexagon cell maths from the Ambient Background's mosaic,
anchored on the image centre and averaged over four taps like `Pixelate`. Reported to obs-websocket
as `hexagon`. **Not verified on Direct3D.**

## One list control everywhere (0.1.0-beta43)

The owner: "make any list that apps or sources or scenes or items of any kind get added to ... have a
vertical building list with individual controls for deletion", consistent everywhere; and the
shadow's scene list must be a selection from the user's scenes, "not hard coded scenes. this app will
not only be used by me" (it never was hard-coded, but a row of tick boxes read that way).

- `src/common/pick-list.*`: `pick_list_add` builds a group with a choose-to-add box and one button
  row per entry (label "x  name"); `pick_list_read`/`_write` handle both storage forms (array of
  `{value}` and comma-separated text), so no saved setting changed shape.
- Property names carry the list: `<setting>|a...` or `<setting>|t...` plus `list`, `pick`, `row|<n>`,
  `none`. The callbacks have only the owner source as context and read the rest from the name.
- Rows are rebuilt inside the callbacks by editing the live `obs_properties_t` and returning true.
  **Not seen in OBS's properties window**; if rows do not refresh there, that is the place to look.
- Shadow: `off_scenes` (scene names) replaces the `off_in:<uuid>` tick boxes; `update` migrates them.
  Rule: off when the scene on screen is listed, or every showing scene that holds the source is.
- Use it for every new list. Do not add another editable list or comma text box.

## Background capture kept ready (0.1.0-beta42)

The owner: switching from a scene without the live background to one with it has "a slight loading
time for the live background feed to kick in". In Automatic mode the background drops its live child
when the scene on screen has no capture, OBS deactivates the capture, and a Game Capture then has to
hook again. `live_ready` (default on) keeps the last capture, or the largest in any scene, as an active
child with `live_shown_` false so it is not drawn. Children are resolved every 0.1 s, through
`scene_on_screen()` now. **Untested: no capture source can be created headless.**

## Steady capture (0.1.0-beta41)

The owner, with Auto Frame as *Just the picture* and Fit Item: going between a window and full-screen
"causes it to adjust and move and flicker"; windowed games are sometimes slow to be detected; and
window captures should resize smoothly in real time as their old scenes did.

- Auto Frame's picture mode now outputs `frame_height` tall and `height x ratio` wide, not the
  capture's own pixels, so only a change of shape changes the source's size.
- Fit Item sets the item's bounds (SCALE_INNER, centred) to what is drawn at the fitted scale, and
  OBS does the fitting per frame. Before, it set the scale one tick after the source changed size.
- Auto Capture: `known_games_` (saved as `known_games`) remembers programs once seen to be games; the
  scan runs every 0.5 s.
- **Games crashing** (owner's report, no details yet). `loaded_engines` used
  `CreateToolhelp32Snapshot(TH32CS_SNAPMODULE)` on the foreground process every scan; for another
  process that call injects a thread. It now uses `EnumProcessModulesEx` and caches per process id
  (six looks at most, 2.5 s apart). **Windows-only code, compiled by CI and never run by the tests.**
  If crashes continue, the next suspect is OBS's own Game Capture hook being re-aimed.
- **Stale shape**: after Snes9x (8:7) the owner's Switch emulator stayed cropped. The held shape is
  dropped when the executable changes; that evidently did not happen here, reason unknown. Added:
  `kNewSession`, the held shape is also dropped when the capture was gone more than 4 s. Untested.
- The owner confirmed on Windows that the preview's right-click *StageKit* menu works.

## Bring-along flip and the shift after a move (0.1.0-beta40)

The owner: a Send To that brings the Amuse widget along with the camera did not flip the widget below
the camera at the top of the screen, and "when it's done moving, either the item or the bring along
shifts a few pixels".

- The generator wrote the camera's seven Send To filters with `flip_linked` false. It now writes true.
  The owner's imported filters still have it off; the release notes tell them to tick it.
- `PlaceItem::plan` judges the flip with `measure_item` (visible pixels) instead of the follower's box.
- The shift: not reproduced. Two plausible causes were removed. The dock's snap-on-release watcher
  counted any movement of a selected item as a drag (now only while a mouse button is down;
  `stagekit_test_mouse_down` stands in for the mouse in tests), and `tick` applied the final step
  twice. If the owner still sees it, ask whether *Snap on release* is ticked and whether the camera
  has a shadow with *Sizes as they look on screen*.

### Crash reports (same release)

The owner said OBS "is crashing sometimes when working with the new filters". No crash report has
been received (three requests are open: the draft 4 import, the copy-to-group crash, and this). Gaps
closed on suspicion only: `items_showing` and `showing_holders` now return references, Fit Item holds
its item for the tick, and Ramp Blur's `led_` has a mutex. Still unverified and worth suspecting if
crashes continue: `PreviewMenu` adding to OBS's menu in its Show event, and `measure_item` rendering a
source from the UI thread or from inside a tick.

## Rebuild draft 5: one game capture with Fit Item

The owner asked for the collection regenerated with Fit Item set up. `scripts/rebuild-collection.py`
now makes a single game source, *Game Capture Framed* (Auto Frame as *Just the picture*, with the
shadow and the switched-off Ramp Blur), and gives every game scene a *Fit the game* Fit Item filter:
the whole screen with a 90 px margin where the old capture covered the canvas, and the old game box
(1754x1268 at 30, 86) with no margin elsewhere. The 2560x1440 "Game Capture" source is gone.
Draft 5 is generated from the owner's original export, so nothing they changed by hand in OBS after
importing draft 4b is in it; they were told. It loads in `tests/collection-loadcheck.cpp`. Show/hide
fades are still left out.

## One capture for every scene: Fit Item (0.1.0-beta39)

The owner wants the same "Game Capture Framed" in the full-screen Game-Capture-Scene and in the
component scene: a normal game fills the full-screen scene, an emulator's legacy shape floats centred
with a margin (they showed a hand-placed example, about 88% of the canvas height with a shadow). Their
original collection had no such thing to copy: its full-screen scene simply showed the capture at
2560x1440 with its bars.

- Auto Frame `frame_mode` 1, *Just the picture*: the frame takes the trimmed picture's size
  (`out_w_`, `out_h_`), nothing is fitted or glided.
- `src/placement/fit-item.*`, a scene filter: fits the picture (source minus `shadow_room`, minus the
  item's crop) in the whole scene or a custom box; same shape within 1% fills, otherwise it is centred
  inside the margin. It owns the item's transform: no bounds, top-left alignment, no rotation.
- `shadow_room` moved from `src/dock/arrange.cpp` to `src/shadow/shadow-backdrop.*`.
- The rebuilt collection has not been regenerated with this; the release notes give the owner the
  three steps instead.

## Shadow left off in chosen scenes (0.1.0-beta38)

Asked for by the owner on 2026-11-12: exclude the effect in selected scenes when the source is in
several, chosen from the existing scenes in a clear list rather than a one-line entry.

- The properties add one tick box per scene that holds the source (`scenes_holding`), keyed
  `off_in:<scene uuid>`.
- A filter cannot know which scene is drawing it. Every 20 ticks it finds the scenes holding the
  source that are showing (`showing_holders`: the scene on screen and visible nested scenes); when
  all of them are ticked it sets `off_here_`, `settings()` then reports nothing to draw, the render is
  skipped and `settle_items` gives the room back.
- Consequences to remember: Preview in Studio Mode follows the Program scene; the change lags a scene
  switch by up to 20 ticks, so a cut shows a frame or two of the old state. The owner has not seen
  this yet.

## Ramp Blur leads other sources (0.1.0-beta37)

Asked for by the owner on 2026-11-12: choose inside the filter which sources it applies to, so one
Stream Deck key blurs them all, with an option to pick up live game and window captures.

- Settings `also` (editable list of source names, filled by `also_pick` + `also_add`) and
  `also_captures`. `RampBlur::lead_others` runs wherever the blur's on/off state changes (`announce`)
  and calls `set_active` on the first Ramp Blur of each target, creating one from this filter's
  settings (named *Ramp Blur (follows another)*) when a target has none.
- Captures are found at switch-on in `scene_on_screen()`; weak references are kept so they are cleared
  at switch-off even if they have left the screen. **Not tested**: no capture source exists headless.
- `is_capture_source` moved to `src/common/scene-util.hpp`; Ambient Background uses it.
- Properties are four groups; `btn_on` and `btn_off` were removed.

## Align to the first or last selected; selection list (0.1.0-beta36)

Asked for by the owner on 2026-11-12. `Against` gains `First` and `Last` (saved values 2 and 3).
OBS keeps no selection order, so `selection_in_order()` (`src/dock/arrange-tools.*`) notes it by being
called often; the dock calls it every 120 ms. Items selected together fall back to Sources-list order.
The size tools copy the key item with First/Last and the top-most item otherwise. The dock's
*Selected* list (`arrangeSelected`) replaces the one-line "Moves: ..." label.

## Undo and redo (0.1.0-beta35)

The owner: "transforms in stage kit don't respond to undo or redo". `run_arrange` now takes
`obs_scene_save_transform_states` before and after and hands both to
`obs_frontend_add_undo_redo_action`; both callbacks are `obs_scene_load_transform_states`. The dock's
*Send to* buttons add their step once the glide has ended; *Snap on release* adds one after the drag's.
Send To / Place Item / Layout filters fired by keys do not add steps, on purpose. The tests replay the
last step through `stagekit_replay_arrange` because there is no undo list without OBS's window.
**Not verified in OBS.**

## Thumbnail fits the dock (0.1.0-beta34)

The owner: "stagekit background in dock should scale to fit the dock size". `Thumb` reports width
changes; `show_thumb` scales the picture to the label's width and sets the label's height to match,
capped so the buttons stay visible in a short dock (`kBelowThumb`). Their screenshot also confirmed
the AVIF thumbnail fix from beta27 on Windows.

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
