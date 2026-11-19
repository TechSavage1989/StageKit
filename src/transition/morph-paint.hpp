/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <obs-module.h>
#include <graphics/matrix4.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// What Morph draws with, shared by the change of scene (`morph.cpp`), the
// change from one item to another inside a scene (`item-morph.cpp`) and Fade
// and Zoom (`fade-zoom.cpp`).

namespace stagekit {

// Where a picture is drawn: the square from 0,0 to 1,1 laid onto the screen.
// A point (u, v) of it lands on (u*a + v*c + tx, u*b + v*d + ty).
struct Quad {
	double a = 1.0, b = 0.0, c = 0.0, d = 1.0, tx = 0.0, ty = 0.0;
};

Quad between(const Quad &p, const Quad &q, double t);

// What an item cuts off its source, in the source's pixels.
struct Crop {
	double l = 0.0, t = 0.0, r = 0.0, b = 0.0;
	bool any() const { return l > 0.01 || t > 0.01 || r > 0.01 || b > 0.01; }
};

// An item as its scene draws it.
struct Shown {
	obs_source_t *source = nullptr;
	double sw = 0.0, sh = 0.0; // the source's size
	Crop crop;
	Quad quad; // where what is left of it goes
};

// False for an item whose source has no picture to speak of.
bool shown_of(obs_sceneitem_t *item, Shown &out);

// The box round a quad, for how much of the screen it covers and which edge
// of the screen is nearest.
struct Bounds {
	double x0, y0, x1, y1;
};

Bounds bounds_of(const Quad &q);

// Where the part of an item left by `part` goes, when the item's own crop
// goes to its quad: `part` cuts at least as much as the item's crop.
Quad part_quad(const Shown &shown, const Crop &part);

// `part` without the room the item's Shadow, Glow & Outline filters take
// round its picture: what is matched when shadows and outlines are left out.
Crop without_room(const Shown &shown, const Crop &part);

// Two items that change into each other: `match_a` and `match_b` are the
// parts that are to land on each other, `whole_a` and `whole_b` all that is
// to be drawn of each (the matched part and what lies round it: a shadow).
// Gives the part of each to draw, `draw_a` and `draw_b`: the matched part and
// the same share of it round both, as much as the larger of the two needs, so
// the two can be mixed on one quad with the matched parts on each other. (A
// part may then reach past its source; nothing is drawn there.)
void match_parts(const Shown &a, const Crop &match_a, const Crop &whole_a, const Shown &b, const Crop &match_b,
		 const Crop &whole_b, Crop &draw_a, Crop &draw_b);

void set_quad(const Quad &q);

// Make Room: a source with nothing to change into leaves by an edge of the
// screen, away from what morphs in. The owner: "sources move off screen, on
// the closest screen edge but away based on the incoming morph".
enum class RoomEdge { Left = 0, Right = 1, Up = 2, Down = 3 };

// The middle of the changes a source makes room for: each box counts by its
// area. Empty until a box is added.
struct RoomCentre {
	double x = 0.0, y = 0.0, weight = 0.0;
	void add(const Bounds &b);
	bool any() const { return weight > 0.0; }
	double cx() const { return x / weight; }
	double cy() const { return y / weight; }
};

// The edge a source with the box `b` leaves by (or comes in from), on a
// screen of `w` by `h`: of the edges that lie the way the line from `from`'s
// middle to the source's middle points (each by more than a hair), the one
// it has least far to go to be all off the screen; with no `from`, a line
// too short to point anywhere, or no such edge, simply that nearest edge.
RoomEdge room_edge(const Bounds &b, double w, double h, const RoomCentre *from);

// `q` moved `gone` (0 in place, 1 all off the screen) of the way off `edge`.
Quad room_moved(const Quad &q, const Bounds &b, RoomEdge edge, double gone, double w, double h);

// Sources set to count as one thing: the one changes into the other. A link
// is kept as one line of a list, the two names with `kLinkMark` between them.
using MorphLinks = std::vector<std::pair<std::string, std::string>>;
inline constexpr const char *kLinkMark = " \xE2\x86\x94 ";

// The links in a list kept under `setting`.
MorphLinks morph_links_read(obs_data_t *settings, const char *setting);

// Whether two sources are linked, straight or by way of others (A with B and
// B with C links A with C).
bool morph_linked(const MorphLinks &links, const char *a, const char *b);

// The two boxes, the button and the list a link is made with, for a settings
// window. The settings are `prefix` + "link_a", "link_b" and "links".
void add_morph_link_properties(obs_properties_t *props, obs_source_t *owner, const char *prefix);

// The effect and the pictures to draw into. On the drawing thread only,
// apart from making and unmaking it.
class MorphPaint {
public:
	MorphPaint();
	~MorphPaint();
	MorphPaint(const MorphPaint &) = delete;
	MorphPaint &operator=(const MorphPaint &) = delete;

protected:
	// An item as it is to be drawn now, its place worked out afresh when it
	// is first met and whenever its source changes size. `work_out` false:
	// the place is taken as OBS last worked it out and OBS is asked for
	// nothing, for an item of a scene that is being drawn at this moment
	// (the item Morph's layer, which is one of that scene's items: the scene
	// works every item's place out before it draws any). Working it out says
	// so to every listener of the scene, which must not happen in the middle
	// of that scene's drawing: see `refresh_item`.
	bool look(obs_sceneitem_t *item, Shown &out, bool work_out = true);
	// What `look` remembers is let go; `what` names the change in the log.
	void forget_sizes();
	gs_texrender_t *spare();
	// The source with `crop` cut off it, as a picture of its own.
	gs_texture_t *cropped(const Shown &shown, const Crop &crop);
	// The item's crop drawn in to what shows of its picture: the smallest
	// part holding everything that is not see-through. Two different sources
	// change into each other by these parts, not by their boxes: the owner,
	// "the morph transition effect should morph objects based on visible
	// pixels, not bounding box". Looked at once for an item (a small copy,
	// read back) and kept until `forget_seen`; the crop as it is for an item
	// that shows nothing.
	Crop seen_crop(obs_sceneitem_t *item, const Shown &shown);
	void forget_seen();
	// One picture, or two mixed (`amount` of the second), laid onto a quad,
	// at a strength (`opacity`, for a silhouette a part counts in only partly).
	void draw_texture(gs_texture_t *first, gs_texture_t *second, const Quad &quad, float amount,
			  float opacity = 1.0f);
	// An item's source on a quad, with a crop.
	void draw_item(const Shown &shown, const Quad &quad, const Crop &crop);
	// A picture of cx by cy at a strength, `zoom` times its size round its
	// middle, for a transition's own drawing.
	void draw_faded(gs_texture_t *texture, uint32_t cx, uint32_t cy, float opacity, double zoom);

	gs_effect_t *effect_ = nullptr;
	gs_eparam_t *p_image_ = nullptr;
	gs_eparam_t *p_image2_ = nullptr;
	gs_eparam_t *p_opacity_ = nullptr;
	gs_eparam_t *p_amount_ = nullptr;
	std::vector<gs_texrender_t *> spares_;
	size_t spare_ = 0;

private:
	// The size each item's source had when the item's place was last worked
	// out, and whether it has had a picture yet.
	struct Sized {
		uint32_t w = 0, h = 0;
		bool had = false;
	};
	std::unordered_map<obs_sceneitem_t *, Sized> sized_;
	std::unordered_map<obs_sceneitem_t *, Crop> seen_;
	uint64_t since_ = 0;
};

} // namespace stagekit
