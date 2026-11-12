# StageKit for OBS Studio

Quality-of-life tools for OBS that replace long chains of Move filters with
single, easy-to-use features. Every feature can be triggered by an OBS hotkey,
the StageKit Stream Deck plugin, or obs-websocket.

| Feature | Status |
| --- | --- |
| **Ramp Blur**: one filter that smoothly blurs a source in and out, six blur types | Working (v0.1) |
| **Placements**: send items to preset or saved spots, drag on a mini map, animated moves | Planned |
| **Shared layout slots**: one position shared by items across scenes, each with its own text/media | Planned |
| **Stream Deck plugin**: keys and dials that show live state | Planned |

## Ramp Blur

Add **Ramp Blur (StageKit)** as a filter on any source or scene. The filter's own on/off switch is the
blur: turning it on ramps the blur in, turning it off ramps it out and then switches the filter off, so
it costs nothing while off.

![Every blur type at three strengths](docs/images/blur-types-game.png)

| Setting | What it does |
| --- | --- |
| Blur type | Gaussian (smoothest), Dual Kawase (fastest, best for strong blur), Box, Pixelate, Zoom (radial), Motion (directional) |
| Strength | 0–250. Gaussian, Kawase and Box are calibrated to look alike at the same number |
| Ramp style | *Grow blur strength* (sharp → blurred) or *Fade blurred image in* (cross-fade) |
| Ramp in / out time | Separate durations, 0–5000 ms. 0 = instant |
| Easing | Linear, Sine, Quad, Cubic, Quart, Quint, Expo, Circular, Back, Elastic, Bounce × in / out / in-and-out |
| Include in 'Blur All' | Whether the global Blur All hotkeys affect this filter |

Triggering it:

- **Stream Deck, with Elgato's OBS Studio plugin**: add a *Filter* key and pick the source and the Ramp
  Blur filter. The key ramps the blur and its light follows the state. No extra plugin is needed.
- **The eye icon** next to the filter in OBS does the same.
- **Hotkeys**: *Settings → Hotkeys*, under the source: `StageKit: Blur Toggle / On / Off (<filter name>)`.
  Renaming the filter keeps your bindings. A Stream Deck *Hotkey* key can press these (e.g. F13–F24).
- **Blur All**: `StageKit: Blur All Toggle / On / Off` in the top section of *Settings → Hotkeys* drives
  every Ramp Blur at once (a privacy key).
- **obs-websocket**: `CallVendorRequest` with vendor `StageKit` (see below).
- **Scripts**: the filter has `blur_on`, `blur_off`, `blur_toggle` and `get_state` procs.

Turning the blur on or off part-way through a ramp reverses smoothly from the current level. Alpha is
handled properly: text and cut-out sources blur without dark fringes
([title example](docs/images/blur-types-title.png)).

### Replacing a Move-based blur chain

A typical Move setup uses six filters per source: Composite Blur, *Move Action Blur On*,
*Move Value Blur Up*, *Move Action Blur Off*, *Move Value Blur Down* and *Move Action Composite Blur Off*.
To replace it:

1. Add **Ramp Blur (StageKit)** to the source and pick the type and strength. For Composite Blur's
   Pixelate at radius 5, use *Pixelate* at strength 4.
2. Bind your existing keys (e.g. F19 / F21) to `StageKit: Blur On` / `Blur Off` for that source.
3. Delete the old Composite Blur and Move filters.

## Ambient Background

Add **Ambient Background (StageKit)** as a source (it fills the canvas). One source produces a soft,
slowly drifting backdrop, replacing a stack of source clones, nested scenes and blur / scroll / colour /
shader filters.

![Ambient Background stages](docs/images/ambient-background.png)

| Group | Settings |
| --- | --- |
| Picture | An image file, or any existing source or scene. Optionally blend in a live capture: a specific source, or *Automatic*, which uses the largest visible capture (game, window, display or capture card) in the current scene |
| Motion | Drift speed right/left and down/up, zoom, and seamless mirroring at the edges |
| Look | Blur, mosaic (hexagons or squares) and its size, saturation, glow, brightness, vignette |
| Texture overlay | An image multiplied or screened over the result, optionally in black and white |

The picture is built at half resolution before the final pass, since it is blurred anyway. Put the
source in one shared scene and nest that scene wherever you need the background.

## Shadow & Backdrop

Add **Shadow & Backdrop (StageKit)** as a filter on any source, group or scene. It replaces the usual
workaround of wrapping the source in an oversized scene so a shadow has room to draw.

![Shadow & Backdrop examples](docs/images/shadow-backdrop.png)

| Setting | What it does |
| --- | --- |
| Drop shadow | Colour, opacity, offset right/left and down/up, softness (0 = hard edge), spread, and *show the shadow only* |
| Panel behind the source | A filled panel with its own colour, opacity and margin |
| Rounded corners | Rounds the source (and the panel) |

The source grows by exactly the room the shadow and panel need. Room added on the left or top shifts the
picture by that amount inside the item; a hard shadow pointing down and right adds none.

## obs-websocket API

All requests go through `CallVendorRequest` with `vendorName: "StageKit"`. Blur requests take
`sourceName` and an optional `filterName`. Without `filterName`, the first Ramp Blur on the source is
used. `durationMs` optionally overrides the ramp time for that one call.

| requestType | requestData | Response |
| --- | --- | --- |
| `GetVersion` | – | `version`, `features` |
| `ListBlurFilters` | – | `filters: [{sourceName, filterName, active, level, blurType}]` |
| `BlurOn` / `BlurOff` / `BlurToggle` | `sourceName`, `filterName?`, `durationMs?` | the filter's new state |
| `SetBlur` | `sourceName`, `filterName?`, `active`, `durationMs?` | the filter's new state |
| `BlurAll` | `mode?`: `on`, `off` or `toggle` (default) | `active` |

Every response has `success` and, on failure, `error`. A `BlurChanged` vendor event
(`sourceName`, `filterName`, `active`) is emitted whenever a blur turns on or off, so a Stream Deck key
can show the current state.

## Roadmap

**Placements**: moving items without setting up a filter per position.
- A 3×3 grid of anchor presets (corners, edges, centre) with an adjustable safe margin. These work on any
  item with no setup.
- Named custom spots captured from wherever you dragged the item ("Save current spot").
- Linked items that move together (e.g. a camera and its frame), replacing paired "simultaneous moves".
- A dock with a mini map of the scene: click a grid cell or drag the item, and nudge with arrow buttons.
- Animated moves with the same easing options, plus hotkeys and websocket requests per preset.

**Shared layout slots**: for titles such as STARTING / BRB / INTERMISSION / ENDING.
- Define a slot (position, size, alignment) once and assign items in any scene to it.
- Moving or resizing the slot updates every assigned item. The text and media stay per-scene.

**Stream Deck plugin** (TypeScript, Elgato SDK): Blur toggle / on / off keys that light up when blurred,
Move-to-preset keys, Stream Deck + dials for strength or nudging, and dropdowns filled live from OBS.

## Building

Windows and macOS builds are produced by GitHub Actions (`.github/workflows`, from the official
[obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate)). They target OBS 31+.

On Linux:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

### Tests

`tests/run-tests.sh` builds the plugin and runs a headless libobs render test under Xvfb. It renders
every blur type, checks ramp timing and reversal, checks that the strength sweep has no visible jumps,
checks hotkey registration and rename handling, and drives the blur end-to-end through obs-websocket.
It needs `libobs-dev`, `qt6-base-dev`, `obs-studio`, `xvfb` and Python Pillow.

## License

GPL-2.0-or-later, like OBS Studio.
