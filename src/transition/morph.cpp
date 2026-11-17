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
#include "item-morph.hpp"
#include "morph-paint.hpp"
#include "../common/easing.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-filter-ids.hpp"
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
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
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

// A scene filter that only moves, shows and hides items, or only handles
// sound: it draws nothing, so a scene with it can still be drawn item by item.
// StageKit's own, and the Move plugin's, which many scenes carry.
bool draws_nothing(obs_source_t *filter)
{
	if (!(obs_source_get_output_flags(filter) & OBS_SOURCE_VIDEO))
		return true;
	const char *id = obs_source_get_unversioned_id(filter);
	// The scene filters that draw nothing (scene-filter-ids.hpp).
	for (const char *known : {kSceneModesId, kSceneTransitionsId, kSceneFitId, kSceneStandbyId, kSceneCornersId})
		if (id && std::strcmp(id, known) == 0)
			return true;
	for (const char *known :
	     {kFitItemId, kPlaceItemId, kShowHideId, kMatchItemId, kLayoutId, kVisibilityLinkId, kShowInScenesId,
	      kActiveAppId, kProgramTriggerId, "move_source_filter", "move_value_filter", "move_audio_value_filter",
	      "move_action_filter", "move_audio_action_filter", "move_directshow_filter", "move_source_swap_filter",
	      "move_transition_override_filter"})
		if (id && std::strcmp(id, known) == 0)
			return true;
	// The scene filters that only show, hide, fit and add filters.
	for (const char *known : {kSceneModesId, kSceneTransitionsId, kSceneFitId, kSceneStandbyId, kSceneCornersId})
		if (id && std::strcmp(id, known) == 0)
			return true;
	return false;
}

// A drawing filter Morph still takes a scene apart with (Scene Shadow, once
// it says so through morph_accept_drawing_filter). Empty until then.
std::atomic<bool (*)(obs_source_t *)> g_accepts{nullptr};

bool accepted(obs_source_t *filter)
{
	auto accepts = g_accepts.load();
	return accepts && accepts(filter);
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
			if (obs_source_enabled(filter) && !draws_nothing(filter) && !accepted(filter))
				*static_cast<const char **>(param) = obs_source_get_name(filter);
		},
		&in_the_way);
	if (why)
		*why = in_the_way;
	return !in_the_way;
}

// How a change of scene is made: the morph, or, by a rule for two scenes,
// a plain cross-fade or a cut. Kept by number.
enum class Style { Morph = 0, CrossFade = 1, Cut = 2 };

// A rule of its own for the change from one scene to another. The owner:
// "specific overrides for transitions for specific scenes transitioning
// between specific scenes".
struct Rule {
	std::string from, to; // a scene's name; empty for any scene
	bool both = false;    // the other way round as well
	Style style = Style::Morph;
	EaseCurve curve = EaseCurve::Cubic;
	EaseMode mode = EaseMode::InOut;
	Others others = Others::Fade;
	bool pair_main = true;
};

struct Settings {
	Style style = Style::Morph;
	EaseCurve curve = EaseCurve::Cubic;
	EaseMode mode = EaseMode::InOut;
	Others others = Others::Fade;
	bool pair_main = true;  // the largest item left over in each scene becomes one that changes into the other
	bool seen = true;       // two sources change into each other by what shows of them, not their boxes
	bool no_shadows = true; // and by their pictures, without shadows, glows and outlines
	// Sources set to count as one thing: the one changes into the other.
	MorphLinks links;
	std::vector<Rule> rules;
};

// Between the scenes of a rule's line in the list, and before each setting
// of a rule in the settings.
constexpr const char *kRuleMark = " \xE2\x86\x92 ";
constexpr const char *kRuleKey = "rule|";

// The settings for one change of scene: the usual ones, or those of the rule
// that names it most exactly (both scenes before one, one before none).
Settings for_change(const Settings &usual, const char *from, const char *to)
{
	const std::string a = from ? from : "", b = to ? to : "";
	const Rule *best = nullptr;
	int best_score = -1;
	for (const Rule &rule : usual.rules) {
		auto fits = [&](const std::string &x, const std::string &y) {
			return (rule.from.empty() || rule.from == x) && (rule.to.empty() || rule.to == y);
		};
		if (!fits(a, b) && !(rule.both && fits(b, a)))
			continue;
		const int score = (rule.from.empty() ? 0 : 1) + (rule.to.empty() ? 0 : 1);
		if (score > best_score) {
			best = &rule;
			best_score = score;
		}
	}
	Settings s = usual;
	if (best) {
		s.style = best->style;
		s.curve = best->curve;
		s.mode = best->mode;
		s.others = best->others;
		s.pair_main = best->pair_main;
	}
	return s;
}

// One thing to draw, from the back: an item of the old scene, of the new
// scene, or of both.
struct Part {
	obs_sceneitem_t *from = nullptr; // held
	obs_sceneitem_t *to = nullptr;   // held
};

class Morph : public MorphPaint {
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
	static void mix_cb(void *data, gs_texture_t *a, gs_texture_t *b, float t, uint32_t cx, uint32_t cy);
	static void alone_cb(void *data, gs_texture_t *a, gs_texture_t *b, float t, uint32_t cx, uint32_t cy);

	obs_source_t *source_;
	std::mutex mutex_;
	Settings settings_;

	// Used on the drawing thread only.
	Settings active_; // the settings for the change of scene going on, a rule's or the usual ones
	std::vector<Part> parts_;
	obs_source_t *planned_from_ = nullptr; // compared, never used
	obs_source_t *planned_to_ = nullptr;
	gs_texrender_t *tr_from_ = nullptr; // a run of the old scene's own items
	gs_texrender_t *tr_to_ = nullptr;   // and of the new scene's
	float eased_ = 0.0f;                // for the plain cross-fade
	// As one item's way of appearing, with nothing to change into or from:
	// the clock's time when it began to fade by itself, and how it goes.
	float alone_from_ = -1.0f;
	bool alone_zoom_ = false;
	std::atomic<bool> stale_{false};
	obs_source_t *told_about_ = nullptr; // the scene whose filter was last written to the log
};

Morph::Morph(obs_data_t *settings, obs_source_t *source) : source_(source)
{
	obs_enter_graphics();
	tr_from_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	tr_to_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();
	update(settings);
}

Morph::~Morph()
{
	item_morph_gone(source_);
	forget();
	obs_enter_graphics();
	gs_texrender_destroy(tr_from_);
	gs_texrender_destroy(tr_to_);
	obs_leave_graphics();
}

void Morph::update(obs_data_t *settings)
{
	Settings s;
	s.curve = static_cast<EaseCurve>(obs_data_get_int(settings, "ease_curve"));
	s.mode = static_cast<EaseMode>(obs_data_get_int(settings, "ease_mode"));
	s.others = static_cast<Others>(std::clamp<long long>(obs_data_get_int(settings, "others"), 0, 2));
	s.pair_main = obs_data_get_bool(settings, "pair_main");
	s.seen = obs_data_get_bool(settings, "visible_pixels");
	s.no_shadows = obs_data_get_bool(settings, "no_shadows");
	s.links = morph_links_read(settings, "links");
	for (const std::string &line : pick_list_read(settings, "rules", false)) {
		const size_t mark = line.find(kRuleMark);
		if (mark == std::string::npos)
			continue;
		const std::string any = obs_module_text("Morph.AnyScene");
		Rule rule;
		rule.from = line.substr(0, mark);
		rule.to = line.substr(mark + strlen(kRuleMark));
		if (rule.from == any)
			rule.from.clear();
		if (rule.to == any)
			rule.to.clear();
		const std::string key = std::string(kRuleKey) + line + "|";
		auto number = [&](const char *name) {
			return obs_data_get_int(settings, (key + name).c_str());
		};
		rule.both = obs_data_get_bool(settings, (key + "both").c_str());
		rule.style = static_cast<Style>(std::clamp<long long>(number("style"), 0, 2));
		rule.curve = static_cast<EaseCurve>(number("ease_curve"));
		rule.mode = static_cast<EaseMode>(number("ease_mode"));
		rule.others = static_cast<Others>(std::clamp<long long>(number("others"), 0, 2));
		rule.pair_main = obs_data_get_bool(settings, (key + "pair_main").c_str());
		s.rules.push_back(rule);
	}
	// (For a Morph that is one item's way of appearing: who it may pair with.)
	item_morph_said(source_, MorphSaid{s.links, s.pair_main, s.seen, s.no_shadows, s.curve, s.mode});
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
	forget_sizes();
	planned_from_ = planned_to_ = nullptr;
}

// Which items go together, and in what order everything is drawn.
void Morph::plan(obs_source_t *from, obs_source_t *to)
{
	forget();
	planned_from_ = from;
	planned_to_ = to;
	const Settings s = active_;
	// The items each scene shows, from the back, each held.
	auto gather = [](obs_source_t *scene, std::vector<obs_sceneitem_t *> &out) {
		obs_scene_enum_items(
			obs_scene_from_source(scene),
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
				if (obs_sceneitem_visible(item) && !is_morph_layer(item)) {
					obs_sceneitem_addref(item);
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
	// Then sources linked in the settings: one camera in this scene and
	// another in that, set to count as the same thing.
	for (size_t n = 0; n < news.size() && !s.links.empty(); n++) {
		if (partner_of_new[n] >= 0)
			continue;
		const char *new_name = obs_source_get_name(obs_sceneitem_get_source(news[n]));
		for (size_t o = 0; o < olds.size(); o++) {
			if (partner_of_old[o] < 0 &&
			    morph_linked(s.links, obs_source_get_name(obs_sceneitem_get_source(olds[o])), new_name)) {
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
		auto largest = [this, screen](const std::vector<obs_sceneitem_t *> &items,
					      const std::vector<int> &partner) {
			int best = -1;
			double best_area = screen * 0.15;
			for (size_t i = 0; i < items.size(); i++) {
				Shown shown;
				if (partner[i] >= 0 || !look(items[i], shown))
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

// The old scene's own items of a run (or the new scene's), drawn as the scene
// draws them into a picture of the whole screen. With a style other than
// Fade they are also on their way in or out.
void Morph::draw_side(gs_texrender_t *into, const std::vector<const Part *> &run, bool leaving, double t, uint32_t cx,
		      uint32_t cy)
{
	const Settings s = active_;
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
		if (!look(leaving ? part->from : part->to, shown))
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
		const bool had = look(part.from, was), has = look(part.to, will);
		if (!had && !has)
			continue;
		// (A source with nothing in it on one side: the other side's place.)
		const bool two = had && has && was.source != will.source;
		// Two sources: by what shows of each, looked at once as the change
		// begins.
		Crop from_part = was.crop, to_part = will.crop;
		if (two) {
			const Crop from_whole = active_.seen ? seen_crop(part.from, was) : was.crop;
			const Crop to_whole = active_.seen ? seen_crop(part.to, will) : will.crop;
			match_parts(was, active_.no_shadows ? without_room(was, from_whole) : from_whole, from_whole,
				    will, active_.no_shadows ? without_room(will, to_whole) : to_whole, to_whole,
				    from_part, to_part);
		}
		const Quad quad = two          ? between(part_quad(was, from_part), part_quad(will, to_part), t)
				  : had && has ? between(was.quad, will.quad, t)
				  : had        ? was.quad
					       : will.quad;
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
			gs_texture_t *first = cropped(was, from_part);
			gs_texture_t *second = cropped(will, to_part);
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

// One item by itself: faded in or out where it is, a little smaller on the
// way with "Fade and zoom".
void Morph::alone_cb(void *data, gs_texture_t *a, gs_texture_t *b, float, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<Morph *>(data);
	gs_texture_t *only = a ? a : b;
	if (!only)
		return;
	const float there = a ? 1.0f - self->eased_ : self->eased_; // how much of it is there
	self->draw_faded(only, cx, cy, there, self->alone_zoom_ ? 0.8 + 0.2 * (double)there : 1.0);
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
		s = for_change(settings_, from ? obs_source_get_name(from) : nullptr,
			       to ? obs_source_get_name(to) : nullptr);
	}
	// (A rule for these two scenes may have other settings than the last
	// change had: the plan is made again.)
	if (s.pair_main != active_.pair_main || s.others != active_.others)
		planned_from_ = planned_to_ = nullptr;
	active_ = s;
	// (Curves that overshoot would fade past all or nothing: kept between.)
	const double eased = t <= 0.0f ? 0.0 : t >= 1.0f ? 1.0 : ease(s.curve, s.mode, t);
	eased_ = (float)std::clamp(eased, 0.0, 1.0);
	// The size to draw at. OBS's own window never gives a transition one,
	// and then `obs_transition_get_size` says 0 by 0: the transition is as
	// large as its scenes, which is what its width and height say. (Going
	// by the first alone, the morph was skipped for want of a size and the
	// owner saw a plain cut.)
	uint32_t cx = 0, cy = 0;
	obs_transition_get_size(source_, &cx, &cy);
	if (!cx || !cy) {
		cx = obs_source_get_width(source_);
		cy = obs_source_get_height(source_);
	}
	if ((!cx || !cy) && to) {
		cx = obs_source_get_width(to);
		cy = obs_source_get_height(to);
	}
	if (stale_.exchange(false)) {
		forget();
		alone_from_ = -1.0f;
	}
	// As one item's own show or hide transition (one side is nothing), what
	// is drawn depends on whether another item is changing into it.
	const ItemMorph as_item = (from && to) || t >= 1.0f || t <= 0.0f ? ItemMorph::None : item_morph_now(source_);
	if (as_item == ItemMorph::Paired) {
		// The layer above the two items draws the change.
		obs_source_release(from);
		obs_source_release(to);
		gs_set_linear_srgb(was_linear);
		return;
	}
	if (as_item == ItemMorph::Alone && effect_) {
		if (alone_from_ < 0.0f)
			alone_from_ = std::min(t, 0.95f);
		const double way = std::clamp((double)(t - alone_from_) / (double)(1.0f - alone_from_), 0.0, 1.0);
		eased_ = (float)std::clamp(ease(s.curve, s.mode, way), 0.0, 1.0);
		alone_zoom_ = s.others == Others::Zoom;
		obs_transition_video_render(source_, alone_cb);
		obs_source_release(from);
		obs_source_release(to);
		gs_set_linear_srgb(was_linear);
		return;
	}
	const bool between_scenes = effect_ && from && to && t > 0.0f && t < 1.0f && cx && cy;
	const char *filter_from = nullptr, *filter_to = nullptr;
	if (between_scenes && s.style == Style::Cut) {
		// By a rule for these two scenes: the new scene at once.
		obs_transition_video_render_direct(source_, OBS_TRANSITION_SOURCE_B);
	} else if (between_scenes && s.style == Style::CrossFade) {
		obs_transition_video_render(source_, mix_cb);
	} else if (between_scenes && plain_scene(from, &filter_from) && plain_scene(to, &filter_to)) {
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

// What a rule keeps, each under "rule|<its line>|<name>" and, while it is
// being made, under "rule_<name>".
const char *const kRuleNumbers[] = {"style", "others", "ease_curve", "ease_mode"};
const char *const kRuleSwitches[] = {"both", "pair_main"};

std::string rule_line(obs_data_t *settings)
{
	const std::string from = obs_data_get_string(settings, "rule_from"),
			  to = obs_data_get_string(settings, "rule_to");
	return from.empty() || to.empty() ? std::string() : from + kRuleMark + to;
}

// The two scenes chosen have a rule already: its settings are shown, to be
// looked at or changed.
bool rule_chosen(obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	const std::string line = rule_line(settings);
	const std::vector<std::string> lines = pick_list_read(settings, "rules", false);
	if (line.empty() || std::find(lines.begin(), lines.end(), line) == lines.end())
		return false;
	const std::string key = std::string(kRuleKey) + line + "|";
	for (const char *name : kRuleNumbers)
		obs_data_set_int(settings, (std::string("rule_") + name).c_str(),
				 obs_data_get_int(settings, (key + name).c_str()));
	for (const char *name : kRuleSwitches)
		obs_data_set_bool(settings, (std::string("rule_") + name).c_str(),
				  obs_data_get_bool(settings, (key + name).c_str()));
	return true;
}

// "Save this rule": what is set becomes the rule for the two scenes chosen,
// a new line of the list or an old one changed.
bool add_rule(obs_properties_t *props, obs_property_t *, void *data)
{
	auto *owner = static_cast<obs_source_t *>(data);
	if (!owner)
		return false;
	obs_data_t *settings = obs_source_get_settings(owner);
	const std::string line = rule_line(settings);
	if (line.empty()) {
		obs_data_release(settings);
		return false;
	}
	const std::string key = std::string(kRuleKey) + line + "|";
	for (const char *name : kRuleNumbers)
		obs_data_set_int(settings, (key + name).c_str(),
				 obs_data_get_int(settings, (std::string("rule_") + name).c_str()));
	for (const char *name : kRuleSwitches)
		obs_data_set_bool(settings, (key + name).c_str(),
				  obs_data_get_bool(settings, (std::string("rule_") + name).c_str()));
	std::vector<std::string> lines = pick_list_read(settings, "rules", false);
	if (std::find(lines.begin(), lines.end(), line) == lines.end()) {
		lines.push_back(line);
		pick_list_write(settings, "rules", false, lines);
	}
	obs_source_update(owner, settings);
	obs_data_release(settings);
	pick_list_refresh(props, owner, "rules", false);
	return true;
}

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<Morph *>(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *others = obs_properties_add_list(props, "others", obs_module_text("Morph.Others"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(others, obs_module_text("Morph.Others.Fade"), (int)Others::Fade);
	obs_property_list_add_int(others, obs_module_text("Morph.Others.Zoom"), (int)Others::Zoom);
	obs_property_list_add_int(others, obs_module_text("Morph.Others.Slide"), (int)Others::Slide);
	obs_property_set_long_description(others, obs_module_text("Morph.Others.Tip"));
	obs_property_t *pair_main = obs_properties_add_bool(props, "pair_main", obs_module_text("Morph.PairMain"));
	obs_property_set_long_description(pair_main, obs_module_text("Morph.PairMain.Tip"));
	obs_property_t *seen = obs_properties_add_bool(props, "visible_pixels", obs_module_text("Morph.Seen"));
	obs_property_set_long_description(seen, obs_module_text("Morph.Seen.Tip"));
	obs_property_t *no_shadows = obs_properties_add_bool(props, "no_shadows", obs_module_text("Measure.NoShadows"));
	obs_property_set_long_description(no_shadows, obs_module_text("Morph.NoShadows.Tip"));
	add_easing_properties(props, "");

	// Links: two different sources that are to count as the same thing.
	add_morph_link_properties(props, self ? self->source() : nullptr, "");

	// Rules: a change between two named scenes made another way.
	obs_source_t *owner = self ? self->source() : nullptr;
	obs_properties_t *rule = obs_properties_create();
	for (const char *name : {"rule_from", "rule_to"}) {
		obs_property_t *scene = obs_properties_add_list(
			rule, name, obs_module_text(strcmp(name, "rule_from") == 0 ? "Morph.RuleFrom" : "Morph.RuleTo"),
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(scene, obs_module_text("List.Choose"), "");
		obs_property_list_add_string(scene, obs_module_text("Morph.AnyScene"),
					     obs_module_text("Morph.AnyScene"));
		obs_enum_scenes(
			[](void *param, obs_source_t *source) {
				const char *scene_name = obs_source_get_name(source);
				if (scene_name && *scene_name && !obs_source_is_group(source))
					obs_property_list_add_string(static_cast<obs_property_t *>(param), scene_name,
								     scene_name);
				return true;
			},
			scene);
		obs_property_set_long_description(scene, obs_module_text("Morph.Rules.Tip"));
		// Two scenes that have a rule already: its settings are shown.
		obs_property_set_modified_callback(scene, rule_chosen);
	}
	obs_property_t *both = obs_properties_add_bool(rule, "rule_both", obs_module_text("Morph.RuleBoth"));
	obs_property_set_long_description(both, obs_module_text("Morph.RuleBoth.Tip"));
	obs_property_t *style = obs_properties_add_list(rule, "rule_style", obs_module_text("Morph.RuleStyle"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(style, obs_module_text("Morph.RuleStyle.Morph"), (int)Style::Morph);
	obs_property_list_add_int(style, obs_module_text("Morph.RuleStyle.Fade"), (int)Style::CrossFade);
	obs_property_list_add_int(style, obs_module_text("Morph.RuleStyle.Cut"), (int)Style::Cut);
	obs_property_set_long_description(style, obs_module_text("Morph.RuleStyle.Tip"));
	obs_property_t *rule_others = obs_properties_add_list(rule, "rule_others", obs_module_text("Morph.Others"),
							      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(rule_others, obs_module_text("Morph.Others.Fade"), (int)Others::Fade);
	obs_property_list_add_int(rule_others, obs_module_text("Morph.Others.Zoom"), (int)Others::Zoom);
	obs_property_list_add_int(rule_others, obs_module_text("Morph.Others.Slide"), (int)Others::Slide);
	obs_properties_add_bool(rule, "rule_pair_main", obs_module_text("Morph.PairMain"));
	add_easing_properties(rule, "rule_");
	obs_properties_add_button2(rule, "rule_add", obs_module_text("Morph.RuleAdd"), add_rule, owner);
	obs_properties_add_group(props, "rule_group", obs_module_text("Morph.RuleGroup"), OBS_GROUP_NORMAL, rule);
	PickList rules;
	rules.setting = "rules";
	rules.title = "Morph.Rules";
	rules.tip = "Morph.Rules.Tip";
	pick_list_add(props, owner, rules);
	return props;
}

void get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "others", (int)Others::Fade);
	obs_data_set_default_bool(settings, "pair_main", true);
	obs_data_set_default_bool(settings, "visible_pixels", true);
	obs_data_set_default_bool(settings, "no_shadows", true);
	obs_data_set_default_int(settings, "ease_curve", (int)EaseCurve::Cubic);
	obs_data_set_default_int(settings, "ease_mode", (int)EaseMode::InOut);
	obs_data_set_default_int(settings, "rule_style", (int)Style::Morph);
	obs_data_set_default_int(settings, "rule_others", (int)Others::Fade);
	obs_data_set_default_bool(settings, "rule_pair_main", true);
	obs_data_set_default_int(settings, "rule_ease_curve", (int)EaseCurve::Cubic);
	obs_data_set_default_int(settings, "rule_ease_mode", (int)EaseMode::InOut);
}

} // namespace

void morph_accept_drawing_filter(bool (*accepts)(obs_source_t *filter))
{
	g_accepts.store(accepts);
}

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
		item_morph_started(static_cast<Morph *>(data)->source());
	};
	info.transition_stop = [](void *data) {
		static_cast<Morph *>(data)->changed();
		item_morph_stopped(static_cast<Morph *>(data)->source());
	};
	info.get_properties = get_properties;
	info.get_defaults = get_defaults;
	register_timed(&info);
}

} // namespace stagekit
