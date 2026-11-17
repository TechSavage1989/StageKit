# StageKit development handoff

Read [AGENTS.md](AGENTS.md) first. This file is the current state.

## Where things stand

| | |
| --- | --- |
| Version | 0.1.0 (in `buildspec.json`). First pre-release is `0.1.0-beta1` |
| Repository | https://github.com/TechSavage1989/StageKit (public; visibility verified, not changed) |
| Default branch | `main`, which holds all work so far (pull request #1, merged 2026-11-12) |
| Working branch | `feat/sd-scene-art` (any image on any key, beta164), on `main` after beta163 (#179). Next: `feat/blur-edges`, then the new collection |
| Releases / update feed | GitHub releases on this repository, which the plugin's updater reads (`src/update/`) |
| Built against | OBS 31.1.1 (`buildspec.json`). The owner runs OBS 32.2.2 on Windows |

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
   `version` in `buildspec.json` when the base version changes. Merge the pull request.
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
