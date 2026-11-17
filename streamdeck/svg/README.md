# StageKit Stream Deck icons as SVG

Editable SVG copies of every picture drawn in this repository, made by
`tools/make-svgs.py` from the same code that draws the PNGs the plugin uses,
so the two always match. Open them in Inkscape, Illustrator, Affinity Designer
or any SVG editor.

| Folder | What is in it |
| --- | --- |
| `action-icons/` | The small icon of each key in Stream Deck's list of actions: white lines on nothing, drawn at 160 units and meant for 20 and 40 px. |
| `keys/<palette>/devices/` | Devices and sources for any key: internal capture card, external capture box, HDMI, console, webcam, camcorder, DSLR, microphone, phone, display, laptop, browser, speaker. |
| `keys/<palette>/options/` | The pictures for each key's options (alignments, aspect ratios, dock options, the Switch Scene key and the rest). |
| `badges/<palette>/` | The corner badges a key's image carries when it has both a type and a function (its step, tool, slide, ratio, option, capture kind, blur style, destination, a move's extra option), written by `tools/make-badges.py`. |
| `marks/` | The on mark (green dot) and the off mark (dimmed, red slash) that keys showing a state carry. |
| `pack/<palette>/<group>/` | The original icon pack itself, all six palettes: blur types, shadows, corners and edges, camera positions, scenes, show/hide, tools and the rest, as the pack's own program wrote them. Pure shapes in named layers (`background`, `layout`, `main`, `cam`, `chat`, `effect`, `label`); the `label` layer holds the words, which the plugin's key pictures leave out. 288 x 288 px. |

Each key picture has its own layers:

- **Background (the pack's key):** the key's frame and pattern from the icon pack. In these files it is a
  picture, not shapes; the pack's own shapes are under `pack/`.
- **Drawing:** the drawing itself, as lines, boxes and circles in the palette's colour. Edit, recolour
  or replace freely.
- **Off look**, **Mark: on**, **Mark: off:** hidden; show them for the switched-off picture and for
  the marked copies a key shows while it is on or off.

The pictures drawn here are 144 x 144 units, the size of a Stream Deck key at 2x.
