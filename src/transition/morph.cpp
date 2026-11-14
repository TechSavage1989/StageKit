/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Morph: a scene transition that moves what the two scenes share.
//
// OBS hands a transition its two scenes as finished pictures, which can only
// be mixed. This one draws the scenes' items itself instead, the way a scene
// does: an item showing the same source in both scenes is drawn once, at a
// place, size and crop part-way between the two; items that are in one scene
// only are drawn where they are and cross-faded.
//
// The order from back to front is the new scene's, with the old scene's own
// items slotted in above whatever shared item they lay above. Runs of items
// that are not shared are drawn into two pictures of the whole screen (the
// old scene's, the new scene's) and those are mixed in one pass, so that two
// full-screen backgrounds changing over never add up to less than a whole
// picture part-way.

#include "morph.hpp"
#include "../common/easing.hpp"
#include "../common/scene-util.hpp"
#include "../common/timing.hpp"
#include "../placement/fit-item.hpp"
#include "../placement/place-item.hpp"
#include "../placement/scene-tools.hpp"
#include "../placement/show-in-scenes.hpp"
#include "../placement/visibility-link.hpp"
#include "../capture/active-app.hpp"
#include "../capture/program-trigger.hpp"

#include <obs-module.h>
#include <graphics/matrix4.h>
#include <graphics/vec4.h>
#include <plugin-support.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace stagekit {

namespace {

// How items that are in one scene only come and go. Saved by number: new
// ones go at the end.
enum class Others : int {
	Fade = 0,
	Zoom = 1,  // and grow from, or shrink to, a little smaller
	Slide = 2, // and slide in from, or out to, the nearest edge of the screen
};

// Where a picture is drawn: the square from 0,0 to 1,1 laid onto the screen.
// A point (u, v) of it lands on (u*a + v*c + tx, u*b + v*d + ty).
struct Quad {
	double a = 1.0, b = 0.0, c = 0.0, d = 1.0, tx = 0.0, ty = 0.0;
};

Quad between(const Quad &p, const Quad &q, double t)
{
	auto mix = [t](double x, double y) {
		return x + (y - x) * t;
	};
	return Quad{mix(p.a, q.a), mix(p.b, q.b), mix(p.c, q.c), mix(p.d, q.d), mix(p.tx, q.tx), mix(p.ty, q.ty)};
}

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

bool shown_of(obs_sceneitem_t *item, Shown &out)
{
	out.source = item ? obs_sceneitem_get_source(item) : nullptr;
	if (!out.source)
		return false;
	out.sw = obs_source_get_width(out.source);
	out.sh = obs_source_get_height(out.source);
	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	out.crop = Crop{(double)crop.left, (double)crop.top, (double)crop.right, (double)crop.bottom};
	const double cw = out.sw - out.crop.l - out.crop.r, ch = out.sh - out.crop.t - out.crop.b;
	if (cw < 1.0 || ch < 1.0)
		return false;
	// The scene's own sum for the item: from the pixels of the cropped
	// source to the screen, bounding box, alignment and rotation included.
	matrix4 m;
	obs_sceneitem_get_draw_transform(item, &m);
	out.quad = Quad{cw * m.x.x, cw * m.x.y, ch * m.y.x, ch * m.y.y, m.t.x, m.t.y};
	return true;
}

// The box round a quad, for how much of the screen it covers and which edge
// of the screen is nearest.
struct Bounds {
	double x0, y0, x1, y1;
};

Bounds bounds_of(const Quad &q)
{
	const double xs[4] = {q.tx, q.tx + q.a, q.tx + q.c, q.tx + q.a + q.c};
	const double ys[4] = {q.ty, q.ty + q.b, q.ty + q.d, q.ty + q.b + q.d};
	return Bounds{*std::min_element(xs, xs + 4), *std::min_element(ys, ys + 4), *std::max_element(xs, xs + 4),
		      *std::max_element(ys, ys + 4)};
}

// A scene filter that only moves, shows and hides items, or only handles
// sound: it draws nothing, so a scene with it can still be drawn item by item.
// StageKit's own, and the Move plugin's, which many scenes carry.
bool draws_nothing(obs_source_t *filter)
{
	if (!(obs_source_get_output_flags(filter) & OBS_SOURCE_VIDEO))
		return true;
	const char *id = obs_source_get_unversioned_id(filter);
	for (const char *known :
	     {kFitItemId, kPlaceItemId, kShowHideId, kMatchItemId, kLayoutId, kVisibilityLinkId, kShowInScenesId,
	      kActiveAppId, kProgramTriggerId, "move_source_filter", "move_value_filter", "move_audio_value_filter",
	      "move_action_filter", "move_audio_action_filter", "move_directshow_filter", "move_source_swap_filter",
	      "move_transition_override_filter"})
		if (id && std::strcmp(id, known) == 0)
			return true;
	return false;
}

// Whether a scene can be drawn item by item and look as it does drawn whole:
// it must be a scene (not a group), with no filter that draws. `why` gets
// the name of the filter in the way, if that is what it is.
bool plain_scene(obs_source_t *source, const char **why = nullptr)
{
	if (!source || !obs_scene_from_source(source) || obs_source_is_group(source))
		return false;
	const char *in_the_way = nullptr;
	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			if (obs_source_enabled(filter) && !draws_nothing(filter))
				*static_cast<const char **>(param) = obs_source_get_name(filter);
		},
		&in_the_way);
	if (why)
		*why = in_the_way;
	return !in_the_way;
}

struct Settings {
	EaseCurve curve = EaseCurve::Cubic;
	EaseMode mode = EaseMode::InOut;
	Others others = Others::Fade;
	bool pair_main = true; // the largest item left over in each scene becomes one that changes into the other
};

// One thing to draw, from the back: an item of the old scene, of the new
// scene, or of both.
struct Part {
	obs_sceneitem_t *from = nullptr; // held
	obs_sceneitem_t *to = nullptr;   // held
};

class Morph {
public:
	Morph(obs_data_t *settings, obs_source_t *source);
	~Morph();

	void update(obs_data_t *settings);
	void render();
	// A transition has begun or ended: what was worked out for the last one
	// is let go, on the drawing thread, before anything is drawn again.
	void changed() { stale_ = true; }
	obs_source_t *source() const { return source_; }

private:
	void forget();
	void plan(obs_source_t *from, obs_source_t *to);
	void draw_morph(double t, uint32_t cx, uint32_t cy);
	void draw_run(const std::vector<const Part *> &run, double t, uint32_t cx, uint32_t cy);
	void draw_side(gs_texrender_t *into, const std::vector<const Part *> &run, bool leaving, double t, uint32_t cx,
		       uint32_t cy);
	void draw_item(const Shown &shown, const Quad &quad, const Crop &crop);
	gs_texture_t *cropped(const Shown &shown, const Crop &crop);
	void draw_texture(gs_texture_t *first, gs_texture_t *second, const Quad &quad, float amount);
	gs_texrender_t *spare();
	static void mix_cb(void *data, gs_texture_t *a, gs_texture_t *b, float t, uint32_t cx, uint32_t cy);

	obs_source_t *source_;
	std::mutex mutex_;
	Settings settings_;

	gs_effect_t *effect_ = nullptr;
	gs_eparam_t *p_image_ = nullptr;
	gs_eparam_t *p_image2_ = nullptr;
	gs_eparam_t *p_opacity_ = nullptr;
	gs_eparam_t *p_amount_ = nullptr;

	// Used on the drawing thread only.
	std::vector<Part> parts_;
	obs_source_t *planned_from_ = nullptr; // compared, never used
	obs_source_t *planned_to_ = nullptr;
	gs_texrender_t *tr_from_ = nullptr; // a run of the old scene's own items
	gs_texrender_t *tr_to_ = nullptr;   // and of the new scene's
	std::vector<gs_texrender_t *> spares_;
	size_t spare_ = 0;
	float eased_ = 0.0f; // for the plain cross-fade
	std::atomic<bool> stale_{false};
	obs_source_t *told_about_ = nullptr; // the scene whose filter was last written to the log
};

Morph::Morph(obs_data_t *settings, obs_source_t *source) : source_(source)
{
	char *path = obs_module_file("effects/stagekit-morph.effect");
	obs_enter_graphics();
	char *errors = nullptr;
	effect_ = path ? gs_effect_create_from_file(path, &errors) : nullptr;
	if (!effect_)
		obs_log(LOG_ERROR, "Morph: failed to load effect '%s': %s. It will cut from scene to scene.",
			path ? path : "(missing)", errors ? errors : "unknown error");
	bfree(errors);
	if (effect_) {
		p_image_ = gs_effect_get_param_by_name(effect_, "image");
		p_image2_ = gs_effect_get_param_by_name(effect_, "image2");
		p_opacity_ = gs_effect_get_param_by_name(effect_, "opacity");
		p_amount_ = gs_effect_get_param_by_name(effect_, "amount");
	}
	tr_from_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	tr_to_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();
	bfree(path);
	update(settings);
}

Morph::~Morph()
{
	forget();
	obs_enter_graphics();
	gs_effect_destroy(effect_);
	gs_texrender_destroy(tr_from_);
	gs_texrender_destroy(tr_to_);
	for (gs_texrender_t *tr : spares_)
		gs_texrender_destroy(tr);
	obs_leave_graphics();
}

void Morph::update(obs_data_t *settings)
{
	Settings s;
	s.curve = static_cast<EaseCurve>(obs_data_get_int(settings, "ease_curve"));
	s.mode = static_cast<EaseMode>(obs_data_get_int(settings, "ease_mode"));
	s.others = static_cast<Others>(std::clamp<long long>(obs_data_get_int(settings, "others"), 0, 2));
	s.pair_main = obs_data_get_bool(settings, "pair_main");
	std::lock_guard lock(mutex_);
	settings_ = s;
}

void Morph::forget()
{
	for (Part &part : parts_) {
		obs_sceneitem_release(part.from);
		obs_sceneitem_release(part.to);
	}
	parts_.clear();
	planned_from_ = planned_to_ = nullptr;
}

// Which items go together, and in what order everything is drawn.
void Morph::plan(obs_source_t *from, obs_source_t *to)
{
	forget();
	planned_from_ = from;
	planned_to_ = to;
	Settings s;
	{
		std::lock_guard lock(mutex_);
		s = settings_;
	}
	// The items each scene shows, from the back, each held.
	auto gather = [](obs_source_t *scene, std::vector<obs_sceneitem_t *> &out) {
		obs_scene_enum_items(
			obs_scene_from_source(scene),
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
				if (obs_sceneitem_visible(item)) {
					obs_sceneitem_addref(item);
					// (So that its place is worked out even in a scene
					// that has never been drawn.)
					refresh_item(item);
					static_cast<std::vector<obs_sceneitem_t *> *>(param)->push_back(item);
				}
				return true;
			},
			&out);
	};
	std::vector<obs_sceneitem_t *> olds, news;
	gather(from, olds);
	gather(to, news);
	// The same source in both: the first of the one with the first of the other.
	std::vector<int> partner_of_new(news.size(), -1), partner_of_old(olds.size(), -1);
	for (size_t n = 0; n < news.size(); n++) {
		for (size_t o = 0; o < olds.size(); o++) {
			if (partner_of_old[o] < 0 &&
			    obs_sceneitem_get_source(olds[o]) == obs_sceneitem_get_source(news[n])) {
				partner_of_old[o] = (int)n;
				partner_of_new[n] = (int)o;
				break;
			}
		}
	}
	// The main picture: the largest item left over in each scene, when each
	// covers a good part of the screen (a game in one, a window in the other).
	if (s.pair_main) {
		const double screen = (double)obs_source_get_width(to) * (double)obs_source_get_height(to);
		auto largest = [screen](const std::vector<obs_sceneitem_t *> &items, const std::vector<int> &partner) {
			int best = -1;
			double best_area = screen * 0.15;
			for (size_t i = 0; i < items.size(); i++) {
				Shown shown;
				if (partner[i] >= 0 || !shown_of(items[i], shown))
					continue;
				const Bounds b = bounds_of(shown.quad);
				const double area = (b.x1 - b.x0) * (b.y1 - b.y0);
				// (Not something that fills the screen: that is a
				// background, and a background is cross-faded.)
				if (area > best_area && area < screen * 0.98) {
					best = (int)i;
					best_area = area;
				}
			}
			return best;
		};
		const int o = largest(olds, partner_of_old), n = largest(news, partner_of_new);
		if (o >= 0 && n >= 0) {
			partner_of_old[o] = n;
			partner_of_new[n] = o;
		}
	}
	// From the back: the new scene's order, with each of the old scene's own
	// items straight above the shared item it lay above there.
	std::vector<int> below(olds.size(), -1); // for an old-only item: the shared old item under it
	int last_shared = -1;
	for (size_t o = 0; o < olds.size(); o++) {
		if (partner_of_old[o] >= 0)
			last_shared = (int)o;
		else
			below[o] = last_shared;
	}
	auto leaving_above = [&](int shared_old) {
		for (size_t o = 0; o < olds.size(); o++)
			if (partner_of_old[o] < 0 && below[o] == shared_old)
				parts_.push_back(Part{olds[o], nullptr});
	};
	leaving_above(-1);
	for (size_t n = 0; n < news.size(); n++) {
		if (partner_of_new[n] < 0) {
			parts_.push_back(Part{nullptr, news[n]});
		} else {
			parts_.push_back(Part{olds[partner_of_new[n]], news[n]});
			leaving_above(partner_of_new[n]);
		}
	}
}

gs_texrender_t *Morph::spare()
{
	if (spare_ >= spares_.size())
		spares_.push_back(gs_texrender_create(GS_RGBA, GS_ZS_NONE));
	return spares_[spare_++];
}

// The source with `crop` cut off it, as a picture of its own.
gs_texture_t *Morph::cropped(const Shown &shown, const Crop &crop)
{
	const double cw = shown.sw - crop.l - crop.r, ch = shown.sh - crop.t - crop.b;
	if (cw < 1.0 || ch < 1.0)
		return nullptr;
	gs_texrender_t *into = spare();
	gs_texrender_reset(into);
	if (!gs_texrender_begin_with_color_space(into, (uint32_t)std::lround(cw), (uint32_t)std::lround(ch),
						 GS_CS_SRGB))
		return nullptr;
	vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho((float)crop.l, (float)(crop.l + cw), (float)crop.t, (float)(crop.t + ch), -100.0f, 100.0f);
	obs_source_video_render(shown.source);
	gs_texrender_end(into);
	return gs_texrender_get_texture(into);
}

void set_quad(const Quad &q)
{
	matrix4 m;
	vec4_set(&m.x, (float)q.a, (float)q.b, 0.0f, 0.0f);
	vec4_set(&m.y, (float)q.c, (float)q.d, 0.0f, 0.0f);
	vec4_set(&m.z, 0.0f, 0.0f, 1.0f, 0.0f);
	vec4_set(&m.t, (float)q.tx, (float)q.ty, 0.0f, 1.0f);
	gs_matrix_mul(&m);
}

// One picture, or two mixed (`amount` of the second), laid onto a quad.
void Morph::draw_texture(gs_texture_t *first, gs_texture_t *second, const Quad &quad, float amount)
{
	if (!effect_ || !first)
		return;
	const bool linear = gs_get_linear_srgb();
	const bool was_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(linear);
	auto set = [linear](gs_eparam_t *param, gs_texture_t *texture) {
		if (linear)
			gs_effect_set_texture_srgb(param, texture);
		else
			gs_effect_set_texture(param, texture);
	};
	set(p_image_, first);
	if (second)
		set(p_image2_, second);
	gs_effect_set_float(p_opacity_, 1.0f);
	gs_effect_set_float(p_amount_, amount);
	gs_matrix_push();
	set_quad(quad);
	while (gs_effect_loop(effect_, second ? "Mix" : "Draw"))
		gs_draw_sprite(nullptr, 0, 1, 1);
	gs_matrix_pop();
	gs_enable_framebuffer_srgb(was_srgb);
}

// An item's source on a quad, with a crop: straight onto what is being drawn
// when nothing is cut off, as a scene draws it, and by way of a picture of
// its own when something is.
void Morph::draw_item(const Shown &shown, const Quad &quad, const Crop &crop)
{
	if (!crop.any()) {
		gs_matrix_push();
		set_quad(quad);
		gs_matrix_scale3f((float)(1.0 / shown.sw), (float)(1.0 / shown.sh), 1.0f);
		obs_source_video_render(shown.source);
		gs_matrix_pop();
		return;
	}
	if (gs_texture_t *texture = cropped(shown, crop))
		draw_texture(texture, nullptr, quad, 0.0f);
}

// The old scene's own items of a run (or the new scene's), drawn as the scene
// draws them into a picture of the whole screen. With a style other than
// Fade they are also on their way in or out.
void Morph::draw_side(gs_texrender_t *into, const std::vector<const Part *> &run, bool leaving, double t, uint32_t cx,
		      uint32_t cy)
{
	Settings s;
	{
		std::lock_guard lock(mutex_);
		s = settings_;
	}
	gs_texrender_reset(into);
	if (!gs_texrender_begin_with_color_space(into, cx, cy, GS_CS_SRGB))
		return;
	vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
	// How far gone: 0 in place, 1 all the way out.
	const double gone = std::clamp(leaving ? t : 1.0 - t, 0.0, 1.0);
	for (const Part *part : run) {
		Shown shown;
		if (!shown_of(leaving ? part->from : part->to, shown))
			continue;
		Quad quad = shown.quad;
		const Bounds b = bounds_of(quad);
		// (Something that fills the screen is a background: it only fades.
		// Slid or shrunk, it would leave the screen bare behind it.)
		const bool fills = (b.x1 - b.x0) * (b.y1 - b.y0) >= 0.9 * (double)cx * (double)cy;
		if (fills) {
			// as it is
		} else if (s.others == Others::Zoom) {
			// Round its own middle, down to four fifths.
			const double k = 1.0 - 0.2 * gone;
			const double mx = (b.x0 + b.x1) / 2.0, my = (b.y0 + b.y1) / 2.0;
			quad.a *= k;
			quad.b *= k;
			quad.c *= k;
			quad.d *= k;
			quad.tx = mx + (quad.tx - mx) * k;
			quad.ty = my + (quad.ty - my) * k;
		} else if (s.others == Others::Slide) {
			// Off the nearest edge of the screen, all of it.
			const double left = b.x1, right = (double)cx - b.x0, up = b.y1, down = (double)cy - b.y0;
			const double least = std::min({left, right, up, down});
			if (least == left)
				quad.tx -= left * gone;
			else if (least == right)
				quad.tx += right * gone;
			else if (least == up)
				quad.ty -= up * gone;
			else
				quad.ty += down * gone;
		}
		draw_item(shown, quad, shown.crop);
	}
	gs_texrender_end(into);
}

// A run of items that are each in one scene only: the old scene's cross-fade
// into the new scene's.
void Morph::draw_run(const std::vector<const Part *> &run, double t, uint32_t cx, uint32_t cy)
{
	bool any_from = false, any_to = false;
	for (const Part *part : run) {
		any_from = any_from || part->from;
		any_to = any_to || part->to;
	}
	if (any_from)
		draw_side(tr_from_, run, true, t, cx, cy);
	if (any_to)
		draw_side(tr_to_, run, false, t, cx, cy);
	gs_texture_t *from = any_from ? gs_texrender_get_texture(tr_from_) : nullptr;
	gs_texture_t *to = any_to ? gs_texrender_get_texture(tr_to_) : nullptr;
	const Quad screen{(double)cx, 0.0, 0.0, (double)cy, 0.0, 0.0};
	const float amount = (float)std::clamp(t, 0.0, 1.0);
	if (from && to) {
		draw_texture(from, to, screen, amount);
	} else if (from || to) {
		// One side only: faded out, or in, over what is under it.
		gs_effect_set_float(p_opacity_, from ? 1.0f - amount : amount);
		const bool linear = gs_get_linear_srgb();
		const bool was_srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(linear);
		gs_texture_t *only = from ? from : to;
		if (linear)
			gs_effect_set_texture_srgb(p_image_, only);
		else
			gs_effect_set_texture(p_image_, only);
		while (gs_effect_loop(effect_, "Draw"))
			gs_draw_sprite(only, 0, cx, cy);
		gs_enable_framebuffer_srgb(was_srgb);
	}
}

void Morph::draw_morph(double t, uint32_t cx, uint32_t cy)
{
	spare_ = 0;
	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	std::vector<const Part *> run;
	auto end_run = [&] {
		if (!run.empty())
			draw_run(run, t, cx, cy);
		run.clear();
	};
	for (const Part &part : parts_) {
		if (!part.from || !part.to) {
			run.push_back(&part);
			continue;
		}
		end_run();
		Shown was, will;
		const bool had = shown_of(part.from, was), has = shown_of(part.to, will);
		if (!had && !has)
			continue;
		// (A source with nothing in it on one side: the other side's place.)
		const Quad quad = had && has ? between(was.quad, will.quad, t) : had ? was.quad : will.quad;
		if (had && has && was.source == will.source) {
			// The same source: once, its crop part-way as well. (A curve
			// that overshoots moves past its place, but cuts no further.)
			const double k = std::clamp(t, 0.0, 1.0);
			const Crop crop{was.crop.l + (will.crop.l - was.crop.l) * k,
					was.crop.t + (will.crop.t - was.crop.t) * k,
					was.crop.r + (will.crop.r - was.crop.r) * k,
					was.crop.b + (will.crop.b - was.crop.b) * k};
			draw_item(will, quad, crop);
		} else if (had && has) {
			// Two sources that change into each other: both, mixed.
			gs_texture_t *first = cropped(was, was.crop);
			gs_texture_t *second = cropped(will, will.crop);
			if (first && second)
				draw_texture(first, second, quad, (float)std::clamp(t, 0.0, 1.0));
		} else {
			draw_item(had ? was : will, quad, had ? was.crop : will.crop);
		}
	}
	end_run();
	gs_blend_state_pop();
}

// Two scenes that cannot be drawn item by item: a plain cross-fade.
void Morph::mix_cb(void *data, gs_texture_t *a, gs_texture_t *b, float, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<Morph *>(data);
	if (!self->effect_ || (!a && !b))
		return;
	const Quad screen{(double)cx, 0.0, 0.0, (double)cy, 0.0, 0.0};
	if (a && b) {
		self->draw_texture(a, b, screen, self->eased_);
		return;
	}
	gs_effect_set_float(self->p_opacity_, a ? 1.0f - self->eased_ : self->eased_);
	const bool was_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_effect_set_texture_srgb(self->p_image_, a ? a : b);
	while (gs_effect_loop(self->effect_, "Draw"))
		gs_draw_sprite(a ? a : b, 0, cx, cy);
	gs_enable_framebuffer_srgb(was_srgb);
}

void Morph::render()
{
	const bool was_linear = gs_set_linear_srgb(true);
	const float t = obs_transition_get_time(source_);
	obs_source_t *from = obs_transition_get_source(source_, OBS_TRANSITION_SOURCE_A);
	obs_source_t *to = obs_transition_get_source(source_, OBS_TRANSITION_SOURCE_B);
	Settings s;
	{
		std::lock_guard lock(mutex_);
		s = settings_;
	}
	// (Curves that overshoot would fade past all or nothing: kept between.)
	const double eased = t <= 0.0f ? 0.0 : t >= 1.0f ? 1.0 : ease(s.curve, s.mode, t);
	eased_ = (float)std::clamp(eased, 0.0, 1.0);
	uint32_t cx = 0, cy = 0;
	obs_transition_get_size(source_, &cx, &cy);
	if (stale_.exchange(false))
		forget();
	const bool between_scenes = effect_ && from && to && t > 0.0f && t < 1.0f && cx && cy;
	const char *filter_from = nullptr, *filter_to = nullptr;
	if (between_scenes && plain_scene(from, &filter_from) && plain_scene(to, &filter_to)) {
		if (from != planned_from_ || to != planned_to_)
			plan(from, to);
		draw_morph(eased, cx, cy);
	} else if (between_scenes) {
		// A scene with a filter that draws cannot be taken apart: said once
		// in the log, so that a plain fade where a morph was expected can
		// be explained.
		obs_source_t *filtered = filter_from ? from : filter_to ? to : nullptr;
		if (filtered && filtered != told_about_) {
			told_about_ = filtered;
			obs_log(LOG_INFO,
				"Morph: '%s' has the filter '%s', which draws; this change of scene is a cross-fade",
				obs_source_get_name(filtered), filter_from ? filter_from : filter_to);
		}
		obs_transition_video_render(source_, mix_cb);
	} else {
		// Before, after, or with one scene only: OBS draws what is showing,
		// and ends the transition when its time is up.
		obs_transition_video_render_direct(source_,
						   t >= 1.0f ? OBS_TRANSITION_SOURCE_B : OBS_TRANSITION_SOURCE_A);
	}
	obs_source_release(from);
	obs_source_release(to);
	gs_set_linear_srgb(was_linear);
}

float mix_out(void *, float t)
{
	return 1.0f - t;
}

float mix_in(void *, float t)
{
	return t;
}

obs_properties_t *get_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *others = obs_properties_add_list(props, "others", obs_module_text("Morph.Others"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(others, obs_module_text("Morph.Others.Fade"), (int)Others::Fade);
	obs_property_list_add_int(others, obs_module_text("Morph.Others.Zoom"), (int)Others::Zoom);
	obs_property_list_add_int(others, obs_module_text("Morph.Others.Slide"), (int)Others::Slide);
	obs_property_set_long_description(others, obs_module_text("Morph.Others.Tip"));
	obs_property_t *pair_main = obs_properties_add_bool(props, "pair_main", obs_module_text("Morph.PairMain"));
	obs_property_set_long_description(pair_main, obs_module_text("Morph.PairMain.Tip"));
	add_easing_properties(props, "");
	return props;
}

void get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "others", (int)Others::Fade);
	obs_data_set_default_bool(settings, "pair_main", true);
	obs_data_set_default_int(settings, "ease_curve", (int)EaseCurve::Cubic);
	obs_data_set_default_int(settings, "ease_mode", (int)EaseMode::InOut);
}

} // namespace

void register_morph()
{
	obs_source_info info = {};
	info.id = kMorphId;
	info.type = OBS_SOURCE_TYPE_TRANSITION;
	info.get_name = [](void *) {
		return obs_module_text("Morph.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new Morph(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<Morph *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<Morph *>(data)->update(settings);
	};
	info.video_render = [](void *data, gs_effect_t *) {
		static_cast<Morph *>(data)->render();
	};
	info.audio_render = [](void *data, uint64_t *ts_out, obs_source_audio_mix *audio, uint32_t mixers,
			       size_t channels, size_t sample_rate) {
		return obs_transition_audio_render(static_cast<Morph *>(data)->source(), ts_out, audio, mixers,
						   channels, sample_rate, mix_out, mix_in);
	};
	info.transition_start = [](void *data) {
		static_cast<Morph *>(data)->changed();
	};
	info.transition_stop = [](void *data) {
		static_cast<Morph *>(data)->changed();
	};
	info.get_properties = get_properties;
	info.get_defaults = get_defaults;
	register_timed(&info);
}

} // namespace stagekit
