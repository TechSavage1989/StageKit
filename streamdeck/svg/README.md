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
| `marks/` | The on mark (green dot) and the off mark (dimmed, red slash) that keys showing a state carry. |

Each key picture has its own layers:

- **Background (the pack's key):** the key's frame and pattern from the icon pack. This one is a
  picture, not shapes: the icon pack came to this project as PNG files only.
- **Drawing:** the drawing itself, as lines, boxes and circles in the palette's colour. Edit, recolour
  or replace freely.
- **Off look**, **Mark: on**, **Mark: off:** hidden; show them for the switched-off picture and for
  the marked copies a key shows while it is on or off.

The pictures of the original icon pack itself (blur types, shadows, corners, the camera positions,
scenes and so on) are not here: they exist in this repository only as PNGs. Their editable source is
with the program that drew the pack.

Pictures are 144 x 144 units, the size of a Stream Deck key at 2x.
