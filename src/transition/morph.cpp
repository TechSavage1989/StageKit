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
//
// Make Room (`Others::MakeRoom`, and the Make Room transition, which is this
// class with that way fixed): the items in one scene only leave by an edge of
// the screen, away from what morphs in. The middle of what morphs is the
// middle of the paired items' places, each counted by its area: in the new
// scene for an item going, in the old scene for an item coming. A pair that
// neither moves nor resizes is not morphing and does not count, nor does a
// background. Of the edges that lie the way the line from that middle to the
// item points, the item takes the one it has least far to go to be off the
// screen (`room_edge`); with nothing morphing, the nearest edge.

#include "morph.hpp"
#include "item-morph.hpp"
#include "morph-paint.hpp"
#include "../common/easing.hpp"
#include "../common/item-transition.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-filter-ids.hpp"
#include "../common/scene-util.hpp"
#include "../common/timing.hpp"
#include "../look/scene-shadow.hpp"
#include "../placement/fit-item.hpp"
#include "../placement/scene-corners.hpp"
#include "../placement/place-item.hpp"
#include "../placement/scene-tools.hpp"
#include "../placement/show-in-scenes.hpp"
#include "../placement/visibility-link.hpp"
#include "../capture/active-app.hpp"
#include "../capture/program-trigger.hpp"
#include "../common/tooltip.hpp"

#include <obs-module.h>
#include <graphics/matrix4.h>
#include <graphics/vec4.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace stagekit {

namespace {

// How items that are in one scene only come and go. Saved by number: new
// ones go at the end.
enum class Others : int {
	Fade = 0,
	Zoom = 1,     // and grow from, or shrink to, a little smaller
	Slide = 2,    // and slide in from, or out to, the nearest edge of the screen
	MakeRoom = 3, // off the nearest edge that lies away from what morphs in
};
constexpr long long kLastOthers = 3;

// Make Room's way of choosing an edge (`room_way`). Saved by number.
enum class RoomWay : int {
	Push = 0,    // the nearest edge away from the morph
	Nearest = 1, // the nearest edge, whatever morphs
};

// How an item with nothing to change into comes in or goes out, when Morph
// is an item's own transition (*Unpaired sources*, `unpaired`): one of
// these, a cut (`kTransitionCut`), or any other transition by its id, with
// that transition's settings under "unpaired|<id>|". Saved as text.
constexpr const char *kUnpairedFade = "morph_fade";
constexpr const char *kUnpairedZoom = "morph_zoom";
constexpr const char *kUnpairedSlide = "morph_slide";
// (and Make Room, `kUnpairedRoom` in item-morph.hpp)

// A scene filter that only moves, shows and hides items, or only handles
// sound: it draws nothing, so a scene with it can still be drawn item by item.
// StageKit's own, and the Move plugin's, which many scenes carry.
bool draws_nothing(obs_source_t *filter)
{
	if (!(obs_source_get_output_flags(filter) & OBS_SOURCE_VIDEO))
		return true;
	const char *id = obs_source_get_unversioned_id(filter);
	// Scene Corners draws only when it shapes the whole scene.
	if (id && std::strcmp(id, kSceneCornersId) == 0)
		return !scene_corners_draws(filter);
	// The scene filters that draw nothing (scene-filter-ids.hpp).
	for (const char *known : {kSceneModesId, kSceneTransitionsId, kSceneFitId, kSceneStandbyId})
		if (id && std::strcmp(id, known) == 0)
			return true;
	for (const char *known :
	     {kFitItemId, kPlaceItemId, kShowHideId, kMatchItemId, kLayoutId, kVisibilityLinkId, kShowInScenesId,
	      kActiveAppId, kProgramTriggerId, "move_source_filter", "move_value_filter", "move_audio_value_filter",
	      "move_action_filter", "move_audio_action_filter", "move_directshow_filter", "move_source_swap_filter",
	      "move_transition_override_filter"})
		if (id && std::strcmp(id, known) == 0)
			return true;
	return false;
}

// The drawing filters Morph still takes a scene apart with, each asked in
// turn (Scene Shadow, which Morph carries through the change; Scene Blur,
// while it blurs nothing). Set once each, at registration.
constexpr size_t kMaxAccepts = 8;
std::array<std::atomic<bool (*)(obs_source_t *)>, kMaxAccepts> g_accepts{};

bool accepted(obs_source_t *filter)
{
	for (const auto &slot : g_accepts) {
		auto accepts = slot.load();
		if (accepts && accepts(filter))
			return true;
	}
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
	// Make Room: away from the morph, or the nearest edge; fading as it goes.
	bool room_push = true;
	bool room_fade = false;
	// An item's own Morph, with nothing to change into: how it comes or goes,
	// and, for another transition, that transition's settings as JSON.
	std::string unpaired = kUnpairedFade;
	std::string unpaired_own;
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
	// `room`: the Make Room transition, Morph with Make Room for whatever
	// has no partner, and whose items, as their own transition, never pair.
	Morph(obs_data_t *settings, obs_source_t *source, bool room = false);
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
	void draw_part(const Shown &shown, const Quad &quad, const Crop &crop, float strength);
	// Pass::Ring: the line round a part's box, with the look's corners.
	// (`shown`, `crop`: the source, so the line goes round what shows of it;
	// nullptr: round the whole quad.)
	// (`b`, `cb`, `k`: a second source changing into the first, `k` of the
	// way; the line's kind, box or following, is the nearer one's.)
	void draw_ring(const Quad &quad, float strength, const Shown *shown = nullptr, const Crop *crop = nullptr,
		       const Shown *b = nullptr, const Crop *cb = nullptr, double k = 0.0);
	void draw_shadowed(double t, uint32_t cx, uint32_t cy, const SceneShadowNow *from, const SceneShadowNow *to);
	bool draw_layers(double t, uint32_t cx, uint32_t cy, gs_texture_t *picture, const ShadowLook &shade);
	bool in_layer(const Part &part) const;
	bool draw_rounded(double t, uint32_t cx, uint32_t cy, const CornerShape *from, const CornerShape *to,
			  const std::function<void()> &inner);
	uint8_t flags_of(obs_sceneitem_t *item, bool old_side) const;
	double weight(const Part &part, double t) const;
	void draw_run(const std::vector<const Part *> &run, double t, uint32_t cx, uint32_t cy);
	// Which of a run's items to draw: all, or (Make Room without a fade)
	// the backgrounds, which fade, apart from the rest, which do not.
	enum class Only { All, Fills, Movers };
	void draw_side(gs_texrender_t *into, const std::vector<const Part *> &run, bool leaving, double t, uint32_t cx,
		       uint32_t cy, Only only);
	void draw_side_items(const std::vector<const Part *> &run, bool leaving, double t, uint32_t cx, uint32_t cy,
			     Only only);
	void find_room_centres(uint32_t cx, uint32_t cy);
	static void mix_cb(void *data, gs_texture_t *a, gs_texture_t *b, float t, uint32_t cx, uint32_t cy);
	static void alone_cb(void *data, gs_texture_t *a, gs_texture_t *b, float t, uint32_t cx, uint32_t cy);
	void start_alone(const Settings &s, obs_source_t *from, obs_source_t *to, uint32_t cx, uint32_t cy);
	void draw_alone(const Settings &s, double way, uint32_t cx, uint32_t cy, bool was_linear);
	void let_go_fallback();

	obs_source_t *source_;
	const bool room_;
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
	// Make Room: the middle of what morphs, where it goes (for the items
	// going) and where it comes from (for the items coming), each frame.
	RoomCentre room_to_, room_from_;
	// As one item's way of appearing, with nothing to change into or from:
	// the clock's time when it began to fade by itself, and how it goes.
	float alone_from_ = -1.0f;
	bool alone_zoom_ = false;
	bool alone_in_ = false;   // the item is coming in (shown), not going
	bool alone_fade_ = false; // a slide that fades as well (Make Room's, in its own frame)
	// *Slide off the nearest edge*: the way it moves, as a step of the
	// item's own width and height (one of them 1 or -1, the other 0).
	int slide_x_ = 0, slide_y_ = 0;
	// Another transition as the way it comes or goes: a private one of that
	// kind, driven by hand (`OBS_TRANSITION_MODE_MANUAL`) from the change's
	// own clock, with the item as its one side. Made when first needed.
	obs_source_t *fallback_ = nullptr;
	std::string fallback_kind_;
	bool fallback_on_ = false; // it holds the item of the change under way
	std::atomic<bool> stale_{false};
	obs_source_t *told_about_ = nullptr; // the scene whose filter was last written to the log

	// A Scene Shadow on either scene is carried through the change: the
	// morph is drawn three times (the picture, the casting parts and the
	// parts the shadow is kept off), and the shadow, part-way between the
	// two scenes', is laid over the picture as the filter lays it.
	// (Ring: the lines round the casting parts' boxes, when the line's
	// corners are chosen.)
	enum class Pass { Picture, Cast, Mask, Ring };
	ShadowLook ring_look_; // the look the Ring pass draws with
	Pass pass_ = Pass::Picture;
	ShadowFlags flags_from_, flags_to_; // what each item does, this frame
	ShadowFlags plain_from_, plain_to_; // for a scene with none, worked out with the plan
	std::unique_ptr<LookPaint> look_;
	gs_texrender_t *tr_pic_ = nullptr;
	gs_texrender_t *tr_cast_ = nullptr;
	gs_texrender_t *tr_mask_ = nullptr;
	gs_texrender_t *tr_ring_ = nullptr;
	// *Shadow from: Each source by itself*: the casting parts and the mask
	// a layer at a time (the parts from layer_lo_ up to layer_hi_).
	size_t layer_lo_ = 0, layer_hi_ = SIZE_MAX;
	std::vector<gs_texrender_t *> tr_layer_cast_, tr_layer_mask_, tr_layer_ring_;
	obs_source_t *told_mixed_ = nullptr; // the scene last logged for two ways of shadowing
	// A Scene Corners shaping a whole scene: the morph drawn into a picture
	// and cut to the shape, part-way between the two scenes'.
	std::unique_ptr<CornersPaint> round_;
	gs_texrender_t *tr_round_ = nullptr;
};

Morph::Morph(obs_data_t *settings, obs_source_t *source, bool room) : source_(source), room_(room)
{
	obs_enter_graphics();
	tr_from_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	tr_to_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	tr_pic_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	tr_cast_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	tr_mask_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	tr_ring_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();
	update(settings);
}

Morph::~Morph()
{
	item_morph_gone(source_);
	forget();
	let_go_fallback();
	obs_source_release(fallback_);
	obs_enter_graphics();
	gs_texrender_destroy(tr_from_);
	gs_texrender_destroy(tr_to_);
	gs_texrender_destroy(tr_pic_);
	gs_texrender_destroy(tr_cast_);
	gs_texrender_destroy(tr_mask_);
	gs_texrender_destroy(tr_ring_);
	gs_texrender_destroy(tr_round_);
	for (auto *list : {&tr_layer_cast_, &tr_layer_mask_, &tr_layer_ring_})
		for (gs_texrender_t *tr : *list)
			gs_texrender_destroy(tr);
	obs_leave_graphics();
	look_.reset();
	round_.reset();
}

void Morph::update(obs_data_t *settings)
{
	Settings s;
	s.curve = static_cast<EaseCurve>(obs_data_get_int(settings, "ease_curve"));
	s.mode = static_cast<EaseMode>(obs_data_get_int(settings, "ease_mode"));
	s.others = static_cast<Others>(std::clamp<long long>(obs_data_get_int(settings, "others"), 0, kLastOthers));
	s.room_push = obs_data_get_int(settings, "room_way") != (int)RoomWay::Nearest;
	s.room_fade = obs_data_get_bool(settings, "room_fade");
	s.pair_main = obs_data_get_bool(settings, "pair_main");
	s.seen = obs_data_get_bool(settings, "visible_pixels");
	s.no_shadows = obs_data_get_bool(settings, "no_shadows");
	// Unpaired sources. Made before the choice: as "Other items" said, which
	// an item by itself followed for a fade and zoom only (a slide was a fade).
	s.unpaired = obs_data_get_string(settings, "unpaired");
	if (!obs_data_has_user_value(settings, "unpaired") || s.unpaired.empty()) {
		s.unpaired = s.others == Others::Zoom ? kUnpairedZoom : kUnpairedFade;
		obs_data_set_default_string(settings, "unpaired", s.unpaired.c_str());
	}
	if (room_) {
		// (Make Room's way is fixed, for a change of scene and for an item.)
		s.others = Others::MakeRoom;
		s.unpaired = kUnpairedRoom;
	}
	if (item_transition_is_fallback_kind(s.unpaired.c_str()))
		s.unpaired_own = item_transition_own_json(settings, "unpaired|" + s.unpaired + "|");
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
		rule.others = static_cast<Others>(std::clamp<long long>(number("others"), 0, kLastOthers));
		rule.pair_main = obs_data_get_bool(settings, (key + "pair_main").c_str());
		s.rules.push_back(rule);
	}
	// (For a Morph that is one item's way of appearing: who it may pair with.)
	MorphSaid said{s.links, s.pair_main, s.seen, s.no_shadows, s.curve, s.mode};
	said.room = s.unpaired == kUnpairedRoom;
	said.room_push = s.room_push;
	said.room_fade = s.room_fade;
	said.pairs = !room_;
	item_morph_said(source_, said);
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
	// (For a scene without a Scene Shadow, what its items would do under
	// one: the shadow fades out over its background, not over everything.)
	scene_shadow_default_flags(from, plain_from_);
	scene_shadow_default_flags(to, plain_to_);
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
		      uint32_t cy, Only only)
{
	gs_texrender_reset(into);
	if (!gs_texrender_begin_with_color_space(into, cx, cy, GS_CS_SRGB))
		return;
	vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
	draw_side_items(run, leaving, t, cx, cy, only);
	gs_texrender_end(into);
}

void Morph::draw_side_items(const std::vector<const Part *> &run, bool leaving, double t, uint32_t cx, uint32_t cy,
			    Only only)
{
	const Settings s = active_;
	// How far gone: 0 in place, 1 all the way out.
	const double gone = std::clamp(leaving ? t : 1.0 - t, 0.0, 1.0);
	const uint8_t wanted = pass_ == Pass::Cast || pass_ == Pass::Ring ? kShadowCasts : kShadowMasked;
	for (const Part *part : run) {
		Shown shown;
		obs_sceneitem_t *item = leaving ? part->from : part->to;
		if (!item || (pass_ != Pass::Picture && (!(flags_of(item, leaving) & wanted) || !in_layer(*part))))
			continue;
		if (!look(item, shown))
			continue;
		Quad quad = shown.quad;
		const Bounds b = bounds_of(quad);
		// (Something that fills the screen is a background: it only fades.
		// Slid or shrunk, it would leave the screen bare behind it.)
		const bool fills = (b.x1 - b.x0) * (b.y1 - b.y0) >= 0.9 * (double)cx * (double)cy;
		if ((only == Only::Fills && !fills) || (only == Only::Movers && fills))
			continue;
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
			quad = room_moved(quad, b, room_edge(b, (double)cx, (double)cy, nullptr), gone, (double)cx,
					  (double)cy);
		} else if (s.others == Others::MakeRoom) {
			// Off the nearest edge that lies away from what morphs in (or
			// in from the one that lies away from what morphs out).
			const RoomCentre &centre = leaving ? room_to_ : room_from_;
			const bool push = s.room_push && centre.any();
			quad = room_moved(quad, b, room_edge(b, (double)cx, (double)cy, push ? &centre : nullptr), gone,
					  (double)cx, (double)cy);
		}
		if (pass_ == Pass::Ring)
			draw_ring(quad, 1.0f, &shown, &shown.crop);
		else
			draw_item(shown, quad, shown.crop);
	}
}

// Make Room: the middle of what morphs, in the new scene and in the old.
// Pairs that stay as they are, and backgrounds, do not count.
void Morph::find_room_centres(uint32_t cx, uint32_t cy)
{
	room_to_ = RoomCentre();
	room_from_ = RoomCentre();
	if (active_.others != Others::MakeRoom)
		return;
	const double screen = (double)cx * (double)cy;
	for (const Part &part : parts_) {
		Shown was, will;
		if (!part.from || !part.to || !look(part.from, was) || !look(part.to, will))
			continue;
		const Bounds a = bounds_of(was.quad), b = bounds_of(will.quad);
		const bool still = std::abs(a.x0 - b.x0) < 1.0 && std::abs(a.y0 - b.y0) < 1.0 &&
				   std::abs(a.x1 - b.x1) < 1.0 && std::abs(a.y1 - b.y1) < 1.0;
		auto background = [screen](const Bounds &x) {
			return (x.x1 - x.x0) * (x.y1 - x.y0) >= 0.9 * screen;
		};
		if (still)
			continue;
		if (!background(b))
			room_to_.add(b);
		if (!background(a))
			room_from_.add(a);
	}
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
	// (Make Room without a fade: what moves is drawn whole on its way, after
	// the backgrounds have been cross-faded.)
	const bool solid = active_.others == Others::MakeRoom && !active_.room_fade;
	const Only only = solid ? Only::Fills : Only::All;
	if (any_from)
		draw_side(tr_from_, run, true, t, cx, cy, only);
	if (any_to)
		draw_side(tr_to_, run, false, t, cx, cy, only);
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
		gs_texture_t *only_one = from ? from : to;
		if (linear)
			gs_effect_set_texture_srgb(p_image_, only_one);
		else
			gs_effect_set_texture(p_image_, only_one);
		while (gs_effect_loop(effect_, "Draw"))
			gs_draw_sprite(only_one, 0, cx, cy);
		gs_enable_framebuffer_srgb(was_srgb);
	}
	if (solid) {
		if (any_from)
			draw_side_items(run, true, t, cx, cy, Only::Movers);
		if (any_to)
			draw_side_items(run, false, t, cx, cy, Only::Movers);
	}
}

void Morph::draw_morph(double t, uint32_t cx, uint32_t cy)
{
	spare_ = 0;
	find_room_centres(cx, cy);
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
		// (In a silhouette, how much of it counts: part-way between what
		// it does in the one scene and in the other.)
		const float strength = (float)weight(part, t);
		if (strength <= 0.001f)
			continue;
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
		if (pass_ == Pass::Ring) {
			// (Two sources: round the quad, which is already what shows of
			// each with Use visible pixels.)
			if (two) {
				draw_ring(quad, strength, &was, &from_part, &will, &to_part, std::clamp(t, 0.0, 1.0));
			} else {
				const Shown *one = had && has ? &will : had ? &was : &will;
				draw_ring(quad, strength, one, &one->crop);
			}
		} else if (had && has && was.source == will.source) {
			// The same source: once, its crop part-way as well. (A curve
			// that overshoots moves past its place, but cuts no further.)
			const double k = std::clamp(t, 0.0, 1.0);
			const Crop crop{was.crop.l + (will.crop.l - was.crop.l) * k,
					was.crop.t + (will.crop.t - was.crop.t) * k,
					was.crop.r + (will.crop.r - was.crop.r) * k,
					was.crop.b + (will.crop.b - was.crop.b) * k};
			draw_part(will, quad, crop, strength);
		} else if (had && has) {
			// Two sources that change into each other: both, mixed.
			gs_texture_t *first = cropped(was, from_part);
			gs_texture_t *second = cropped(will, to_part);
			// What casts and throws the line is the two shapes together, each
			// whole: mixed as the picture is, a shape the other lacks counted
			// half half-way, and its line and shadow went see-through with it
			// (the owner's titles, "STREAM STARTING", changing into another).
			// Scaled by the larger share, the two are each whole at their own
			// end and never less than whole on the way.
			const double k = std::clamp(t, 0.0, 1.0);
			const float whole = pass_ == Pass::Cast ? (float)(1.0 / std::max(k, 1.0 - k)) : 1.0f;
			if (first && second)
				draw_texture(first, second, quad, (float)k, strength * whole);
		} else {
			draw_part(had ? was : will, quad, had ? was.crop : will.crop, strength);
		}
	}
	end_run();
	gs_blend_state_pop();
}

// An item's source on a quad, at a strength: as a scene draws it when whole.
void Morph::draw_part(const Shown &shown, const Quad &quad, const Crop &crop, float strength)
{
	if (strength >= 0.999f) {
		draw_item(shown, quad, crop);
		return;
	}
	if (gs_texture_t *texture = cropped(shown, crop))
		draw_texture(texture, nullptr, quad, 0.0f, strength);
}

void Morph::draw_ring(const Quad &quad, float strength, const Shown *shown, const Crop *crop, const Shown *b,
		      const Crop *cb, double k)
{
	if (!look_ || !shown || !crop)
		return;
	matrix4 m;
	vec4_set(&m.x, (float)quad.a, (float)quad.b, 0.0f, 0.0f);
	vec4_set(&m.y, (float)quad.c, (float)quad.d, 0.0f, 0.0f);
	vec4_set(&m.z, 0.0f, 0.0f, 1.0f, 0.0f);
	vec4_set(&m.t, (float)quad.tx, (float)quad.ty, 0.0f, 1.0f);
	const bool turned = box_turned(m);
	gs_texture_t *first = cropped(*shown, *crop);
	gs_texture_t *second = b && cb ? cropped(*b, *cb) : nullptr;
	if (!first)
		return;
	auto size = [](gs_texture_t *t, double &w, double &h) {
		w = t ? (double)gs_texture_get_width(t) : 0.0;
		h = t ? (double)gs_texture_get_height(t) : 0.0;
	};
	double w1, h1, w2, h2;
	size(first, w1, h1);
	size(second, w2, h2);
	// The nearer side says whether the line is a box or follows.
	const bool near_second = second && k >= 0.5;
	gs_texture_t *near = near_second ? second : first;
	const double nw = near_second ? w2 : w1, nh = near_second ? h2 : h1;
	const Seen seen = look_->visible_rect(near, (uint32_t)nw, (uint32_t)nh);
	look_->draw_box_ring(m, ring_look_, strength, seen, nw, nh, turned);
	if (!second) {
		look_->draw_follow(first, m, w1, h1, seen, turned, strength);
		return;
	}
	// Two sources changing into each other: both shapes, each whole at its
	// end and never less than whole on the way (as the Cast pass draws them).
	const double whole = 1.0 / std::max(k, 1.0 - k);
	look_->draw_follow(first, m, nw, nh, seen, turned, (float)(strength * (1.0 - k) * whole));
	look_->draw_follow(second, m, nw, nh, seen, turned, (float)(strength * k * whole));
}

// What an item does under its scene's Scene Shadow (or under one as it
// comes, for a scene with none); one not looked at yet is kept clear.
uint8_t Morph::flags_of(obs_sceneitem_t *item, bool old_side) const
{
	const ShadowFlags &flags = old_side ? flags_from_ : flags_to_;
	const auto found = item ? flags.find(obs_sceneitem_get_id(item)) : flags.end();
	return found != flags.end() ? found->second : (uint8_t)kShadowMasked;
}

// How much of a part counts in the silhouette being drawn: all of it in the
// picture; else what it does in the old scene and in the new, mixed by how
// far the change has gone.
double Morph::weight(const Part &part, double t) const
{
	if (pass_ == Pass::Picture)
		return 1.0;
	if (!in_layer(part))
		return 0.0;
	const uint8_t wanted = pass_ == Pass::Cast || pass_ == Pass::Ring ? kShadowCasts : kShadowMasked;
	const double from = part.from && (flags_of(part.from, true) & wanted) ? 1.0 : 0.0;
	const double to = part.to && (flags_of(part.to, false) & wanted) ? 1.0 : 0.0;
	if (!part.from)
		return to;
	if (!part.to)
		return from;
	const double k = std::clamp(t, 0.0, 1.0);
	return from + (to - from) * k;
}

// The morph with the scenes' shadow: the shadow's settings part-way between
// the two scenes' (a scene with none counts as one at no strength), and each
// part throwing it as far as it does in each scene, so a source that only
// casts in the old scene stops casting on the way.
void Morph::draw_shadowed(double t, uint32_t cx, uint32_t cy, const SceneShadowNow *from, const SceneShadowNow *to)
{
	flags_from_ = from ? from->flags : plain_from_;
	flags_to_ = to ? to->flags : plain_to_;
	const ShadowLook look = lerp_look(from ? &from->look : nullptr, to ? &to->look : nullptr, t);
	// The mask is only drawn when it differs from the casting parts.
	bool needs_mask = false;
	for (const ShadowFlags *flags : {&flags_from_, &flags_to_})
		for (const auto &[id, f] : *flags)
			needs_mask = needs_mask || ((f & kShadowMasked) && !(f & kShadowCasts));
	// (Made the first time a change of scene has a shadow: a Morph is also
	// every item transition set to it, and most never need one.)
	if (!look_)
		look_ = std::make_unique<LookPaint>();
	look_->begin_frame();
	auto into = [&](gs_texrender_t *tr, Pass pass) -> gs_texture_t * {
		pass_ = pass;
		gs_texrender_reset(tr);
		if (!gs_texrender_begin_with_color_space(tr, cx, cy, GS_CS_SRGB))
			return nullptr;
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
		draw_morph(t, cx, cy);
		gs_texrender_end(tr);
		return gs_texrender_get_texture(tr);
	};
	gs_texture_t *picture = look.any() && look_->ready() ? into(tr_pic_, Pass::Picture) : nullptr;
	if (picture && ((from && from->each) || (to && to->each))) {
		pass_ = Pass::Picture;
		if (!draw_layers(t, cx, cy, picture, look)) {
			draw_morph(t, cx, cy);
			return;
		}
		gs_blend_state_push();
		gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		look_->draw_out(cx, cy);
		gs_blend_state_pop();
		return;
	}
	gs_texture_t *cast = picture ? into(tr_cast_, Pass::Cast) : nullptr;
	gs_texture_t *mask = cast && needs_mask ? into(tr_mask_, Pass::Mask) : nullptr;
	ring_look_ = look;
	gs_texture_t *ring = cast && look.box_line() ? into(tr_ring_, Pass::Ring) : nullptr;
	pass_ = Pass::Picture;
	if (!cast || !look_->compose(picture, cast, mask, look, cx, cy, nullptr, false, ring)) {
		draw_morph(t, cx, cy);
		return;
	}
	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	look_->draw_out(cx, cy);
	gs_blend_state_pop();
}

// Whether a part counts in the layer being drawn (every part outside the
// layered passes).
bool Morph::in_layer(const Part &part) const
{
	const size_t at = (size_t)(&part - parts_.data());
	return at >= layer_lo_ && at < layer_hi_;
}

// The morph's shadow with *Each source by itself*: the parts, from the back,
// in layers whose shadows cannot reach one another (each part's place in
// both scenes counted, so it holds all the way), and each layer's shadow laid
// over the picture so far, kept off that layer and every one above it, as
// the Scene Shadow does it. Into look_'s picture; false when nothing could be
// drawn.
bool Morph::draw_layers(double t, uint32_t cx, uint32_t cy, gs_texture_t *picture, const ShadowLook &shade)
{
	std::vector<LayerBox> boxes(parts_.size());
	for (size_t i = 0; i < parts_.size(); i++) {
		const Part &part = parts_[i];
		const bool stacked = (part.from && (flags_of(part.from, true) & kShadowMasked)) ||
				     (part.to && (flags_of(part.to, false) & kShadowMasked));
		if (!stacked)
			continue;
		LayerBox &b = boxes[i];
		for (int side = 0; side < 2; side++) {
			obs_sceneitem_t *item = side ? part.to : part.from;
			Shown shown;
			if (!item || !look(item, shown))
				continue;
			const Bounds q = bounds_of(shown.quad);
			b.x0 = b.present ? std::min(b.x0, q.x0) : q.x0;
			b.y0 = b.present ? std::min(b.y0, q.y0) : q.y0;
			b.x1 = b.present ? std::max(b.x1, q.x1) : q.x1;
			b.y1 = b.present ? std::max(b.y1, q.y1) : q.y1;
			b.present = true;
		}
	}
	// (A curve that overshoots carries a part a little past its two places.)
	const double reach = shadow_reach(shade) + 0.05 * std::max(cx, cy);
	const std::vector<size_t> starts = shadow_layers(boxes, reach, kShadowMostLayers);
	const size_t n = starts.size();
	if (!n)
		return false;
	while (tr_layer_cast_.size() < n) {
		tr_layer_cast_.push_back(gs_texrender_create(GS_RGBA, GS_ZS_NONE));
		tr_layer_mask_.push_back(gs_texrender_create(GS_RGBA, GS_ZS_NONE));
		tr_layer_ring_.push_back(gs_texrender_create(GS_RGBA, GS_ZS_NONE));
	}
	auto end_of = [&](size_t l) {
		return l + 1 < n ? starts[l + 1] : parts_.size();
	};
	// One pass of the morph into a canvas picture, for the layer set now,
	// with `above` (the masks above) laid over it if there is one: the mask's
	// colours are the sources' part of a pixel, so what is on top stays so.
	auto into = [&](gs_texrender_t *tr, Pass pass, gs_texture_t *above) -> gs_texture_t * {
		pass_ = pass;
		gs_texrender_reset(tr);
		if (!gs_texrender_begin_with_color_space(tr, cx, cy, GS_CS_SRGB))
			return nullptr;
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
		draw_morph(t, cx, cy);
		if (above)
			look_->copy_onto(above, cx, cy, true);
		gs_texrender_end(tr);
		return gs_texrender_get_texture(tr);
	};
	std::vector<gs_texture_t *> casts(n, nullptr), masks(n, nullptr), rings(n, nullptr);
	ring_look_ = shade;
	for (size_t l = 0; l < n; l++) {
		bool casts_any = false;
		for (size_t i = starts[l]; i < end_of(l); i++) {
			const Part &part = parts_[i];
			casts_any = casts_any || (part.from && (flags_of(part.from, true) & kShadowCasts)) ||
				    (part.to && (flags_of(part.to, false) & kShadowCasts));
		}
		if (!casts_any)
			continue;
		layer_lo_ = starts[l];
		layer_hi_ = end_of(l);
		casts[l] = into(tr_layer_cast_[l], Pass::Cast, nullptr);
		if (shade.box_line())
			rings[l] = into(tr_layer_ring_[l], Pass::Ring, nullptr);
	}
	for (size_t k = n; k-- > 0;) {
		layer_lo_ = starts[k];
		layer_hi_ = end_of(k);
		masks[k] = into(tr_layer_mask_[k], Pass::Mask, k + 1 < n ? masks[k + 1] : nullptr);
	}
	layer_lo_ = 0;
	layer_hi_ = SIZE_MAX;
	pass_ = Pass::Picture;
	return look_->compose_layers(picture, casts, masks, shade, cx, cy, nullptr, &rings);
}

// A Scene Corners on either scene shaping the whole scene: the change drawn
// (by `inner`) into a picture of the screen and cut to the shape part-way
// between the two scenes' (a scene with none counts as one with no
// corners). False when that could not be done; nothing is drawn then.
bool Morph::draw_rounded(double t, uint32_t cx, uint32_t cy, const CornerShape *from, const CornerShape *to,
			 const std::function<void()> &inner)
{
	if (!round_) {
		round_ = std::make_unique<CornersPaint>();
		tr_round_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	}
	if (!round_->ready())
		return false;
	gs_texrender_reset(tr_round_);
	if (!gs_texrender_begin_with_color_space(tr_round_, cx, cy, GS_CS_SRGB))
		return false;
	vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
	inner();
	gs_texrender_end(tr_round_);
	const CornerShape shape = lerp_corner_shape(from, to, t);
	if (!round_->shape(gs_texrender_get_texture(tr_round_), cx, cy, 0.0, 0.0, cx, cy, 1.0, 1.0, shape))
		return false;
	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	round_->draw_out(cx, cy);
	gs_blend_state_pop();
	return true;
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
	// (OBS hands a transition a see-through placeholder for the side that
	// has no source, never nothing: which side is the item's is known from
	// the transition's sources. Taken as `a ? a : b`, an item being shown
	// drew the placeholder all the way and appeared at once at the end.)
	gs_texture_t *only = self->alone_in_ ? b : a;
	if (!only)
		return;
	const float there = self->alone_in_ ? self->eased_ : 1.0f - self->eased_; // how much of it is there
	if (!self->slide_x_ && !self->slide_y_) {
		self->draw_faded(only, cx, cy, there, self->alone_zoom_ ? 0.8 + 0.2 * (double)there : 1.0);
		return;
	}
	// Slid towards (or in from) the nearest edge, inside the item's own
	// frame as every item transition is: the part still in the frame is
	// drawn where it has got to.
	const double gone = 1.0 - (double)there;
	const int ox = (int)std::lround(self->slide_x_ * gone * (double)cx);
	const int oy = (int)std::lround(self->slide_y_ * gone * (double)cy);
	const int vw = (int)cx - std::abs(ox), vh = (int)cy - std::abs(oy);
	if (vw <= 0 || vh <= 0 || !self->effect_)
		return;
	const bool was_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_effect_set_texture_srgb(self->p_image_, only);
	gs_effect_set_float(self->p_opacity_, self->alone_fade_ ? there : 1.0f);
	gs_matrix_push();
	gs_matrix_translate3f((float)std::max(ox, 0), (float)std::max(oy, 0), 0.0f);
	while (gs_effect_loop(self->effect_, "Draw"))
		gs_draw_sprite_subregion(only, 0, (uint32_t)std::max(-ox, 0), (uint32_t)std::max(-oy, 0), (uint32_t)vw,
					 (uint32_t)vh);
	gs_matrix_pop();
	gs_enable_framebuffer_srgb(was_srgb);
}

// The change for an item by itself begins: which way it slides, and, for
// another transition, that transition set up with the item as its one side
// (in from nothing when shown, out to nothing when hidden).
void Morph::start_alone(const Settings &s, obs_source_t *from, obs_source_t *to, uint32_t cx, uint32_t cy)
{
	alone_in_ = to != nullptr;
	alone_zoom_ = s.unpaired == kUnpairedZoom;
	slide_x_ = slide_y_ = 0;
	// (Make Room by itself here: its scene is not on screen, or it is in a
	// group, where no layer is put. It slides off its nearest edge inside
	// its own frame, as Slide does.)
	const bool slide = s.unpaired == kUnpairedSlide || s.unpaired == kUnpairedRoom;
	alone_fade_ = s.unpaired == kUnpairedRoom && s.room_fade;
	const bool other = item_transition_is_fallback_kind(s.unpaired.c_str());
	// Off the nearest edge: of the item's own scene, as the item lies in it.
	const char *edge = slide || other ? item_morph_edge(source_) : nullptr;
	if (slide) {
		const std::string way = edge ? edge : (alone_in_ ? "right" : "left");
		// (Shown, it comes in moving that way: from the other side.)
		const int sign = alone_in_ ? -1 : 1;
		slide_x_ = way == "left" ? -sign : way == "right" ? sign : 0;
		slide_y_ = way == "up" ? -sign : way == "down" ? sign : 0;
	}
	if (!other)
		return;
	if (!fallback_ || fallback_kind_ != s.unpaired) {
		let_go_fallback();
		obs_source_release(fallback_);
		fallback_ = obs_source_create_private(s.unpaired.c_str(), "Morph unpaired", nullptr);
		fallback_kind_ = s.unpaired;
	}
	if (!fallback_)
		return; // (that kind is not installed any more)
	obs_data_t *own = s.unpaired_own.empty() ? obs_data_create()
						 : obs_data_create_from_json(s.unpaired_own.c_str());
	if (!own)
		own = obs_data_create();
	// A slide's or a swipe's "Off the nearest edge", made a direction.
	if (strcmp(obs_data_get_string(own, "direction"), "nearest") == 0) {
		obs_data_set_string(own, "direction", edge ? edge : (alone_in_ ? "right" : "left"));
		if (s.unpaired == "swipe_transition")
			obs_data_set_bool(own, "swipe_in", alone_in_);
	}
	obs_source_reset_settings(fallback_, own);
	obs_data_release(own);
	obs_transition_set_size(fallback_, cx, cy);
	obs_transition_set_alignment(fallback_, OBS_ALIGN_CENTER);
	obs_transition_set_scale_type(fallback_, OBS_TRANSITION_SCALE_ASPECT);
	obs_transition_set_manual_torque(fallback_, 0.0f, 0.0f);
	// As a scene starts an item's own transition.
	if (alone_in_) {
		obs_transition_set(fallback_, nullptr);
		obs_transition_start(fallback_, OBS_TRANSITION_MODE_MANUAL, 300, to);
	} else {
		obs_transition_set(fallback_, from);
		obs_transition_start(fallback_, OBS_TRANSITION_MODE_MANUAL, 300, nullptr);
	}
	obs_transition_set_manual_time(fallback_, 0.0f);
	fallback_on_ = true;
}

// One frame of an item by itself, `way` along its change (0 to 1, by the
// clock, not eased: another transition eases itself).
void Morph::draw_alone(const Settings &s, double way, uint32_t cx, uint32_t cy, bool was_linear)
{
	if (s.unpaired == kTransitionCut) {
		// At once: shown whole, or gone.
		if (alone_in_)
			obs_transition_video_render_direct(source_, OBS_TRANSITION_SOURCE_B);
		return;
	}
	if (fallback_on_ && fallback_) {
		// (Never all the way there: at its end the transition would draw
		// its first side again, the item it hid. The end is drawn here.)
		if (way >= 0.999) {
			if (alone_in_)
				obs_transition_video_render_direct(source_, OBS_TRANSITION_SOURCE_B);
			return;
		}
		obs_transition_set_size(fallback_, cx, cy);
		obs_transition_set_manual_time(fallback_, (float)way);
		// (Drawn as OBS would draw it in the item's place: in the state the
		// scene drew this transition in.)
		const bool linear = gs_set_linear_srgb(was_linear);
		obs_source_video_render(fallback_);
		gs_set_linear_srgb(linear);
		return;
	}
	obs_transition_video_render(source_, alone_cb);
}

// The item the fallback transition held is let go (its change is over).
void Morph::let_go_fallback()
{
	if (fallback_ && fallback_on_)
		obs_transition_set(fallback_, nullptr);
	fallback_on_ = false;
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
		let_go_fallback();
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
		if (alone_from_ < 0.0f) {
			alone_from_ = std::min(t, 0.95f);
			start_alone(s, from, to, cx, cy);
		}
		const double way = std::clamp((double)(t - alone_from_) / (double)(1.0f - alone_from_), 0.0, 1.0);
		eased_ = (float)std::clamp(ease(s.curve, s.mode, way), 0.0, 1.0);
		draw_alone(s, way, cx, cy, was_linear);
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
		SceneShadowNow shadow_from, shadow_to;
		const bool from_shadowed = scene_shadow_now(from, shadow_from);
		const bool to_shadowed = scene_shadow_now(to, shadow_to);
		CornerShape round_from, round_to;
		const bool from_round = scene_corners_now(from, round_from);
		const bool to_round = scene_corners_now(to, round_to);
		auto inner = [&] {
			if (from_shadowed || to_shadowed)
				draw_shadowed(eased, cx, cy, from_shadowed ? &shadow_from : nullptr,
					      to_shadowed ? &shadow_to : nullptr);
			else
				draw_morph(eased, cx, cy);
		};
		if (from_shadowed && to_shadowed && shadow_from.each != shadow_to.each && shadow_from.look.any() &&
		    shadow_to.look.any()) {
			// The two scenes shadow two different ways: either way all along
			// would jump at one end, so the two scenes are cross-faded, each
			// with its own shadow.
			if (to != told_mixed_) {
				told_mixed_ = to;
				obs_log(LOG_INFO,
					"Morph: '%s' and '%s' have Scene Shadows with a different Shadow from; this change of scene is a cross-fade",
					obs_source_get_name(from), obs_source_get_name(to));
			}
			obs_transition_video_render(source_, mix_cb);
		} else if (!(from_round || to_round) || !draw_rounded(eased, cx, cy, from_round ? &round_from : nullptr,
								      to_round ? &round_to : nullptr, inner)) {
			inner();
		}
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

// The settings of the transition chosen under Unpaired sources, in their
// own group, made again whenever another is chosen.
bool unpaired_own_cb(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	obs_property_t *group = obs_properties_get(props, "unpaired|g");
	obs_properties_t *content = group ? obs_property_group_content(group) : nullptr;
	if (!content || !settings)
		return false;
	std::vector<std::string> old;
	for (obs_property_t *p = obs_properties_first(content); p; obs_property_next(&p))
		old.push_back(obs_property_name(p));
	for (const std::string &name : old)
		obs_properties_remove_by_name(content, name.c_str());
	const std::string kind = obs_data_get_string(settings, "unpaired");
	if (item_transition_is_fallback_kind(kind.c_str()))
		item_transition_add_own(content, settings, "unpaired|" + kind + "|", kind);
	obs_property_set_visible(group, obs_properties_first(content) != nullptr);
	return true;
}

// Make Room's own two settings: which edge, and whether it fades.
void add_room_properties(obs_properties_t *props)
{
	obs_property_t *way = obs_properties_add_list(props, "room_way", obs_module_text("MakeRoom.Way"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(way, obs_module_text("MakeRoom.Way.Push"), (int)RoomWay::Push);
	obs_property_list_add_int(way, obs_module_text("MakeRoom.Way.Nearest"), (int)RoomWay::Nearest);
	set_tip(way, obs_module_text("MakeRoom.Way.Tip"));
	obs_property_t *fade = obs_properties_add_bool(props, "room_fade", obs_module_text("MakeRoom.Fade"));
	set_tip(fade, obs_module_text("MakeRoom.Fade.Tip"));
}

// The Make Room transition's window: Morph's pairing and links, its easing,
// and Make Room's own two settings. (Its way for what has no partner is
// fixed, so neither Other items nor Unpaired sources is offered, nor rules.)
obs_properties_t *get_room_properties(void *data)
{
	auto *self = static_cast<Morph *>(data);
	obs_properties_t *props = obs_properties_create();
	add_room_properties(props);
	obs_property_t *pair_main = obs_properties_add_bool(props, "pair_main", obs_module_text("Morph.PairMain"));
	set_tip(pair_main, obs_module_text("Morph.PairMain.Tip"));
	obs_property_t *seen = obs_properties_add_bool(props, "visible_pixels", obs_module_text("Morph.Seen"));
	set_tip(seen, obs_module_text("Morph.Seen.Tip"));
	obs_property_t *no_shadows = obs_properties_add_bool(props, "no_shadows", obs_module_text("Measure.NoShadows"));
	set_tip(no_shadows, obs_module_text("Morph.NoShadows.Tip"));
	add_easing_properties(props, "");
	add_morph_link_properties(props, self ? self->source() : nullptr, "");
	return props;
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
	obs_property_list_add_int(others, obs_module_text("MakeRoom.Option"), (int)Others::MakeRoom);
	set_tip(others, obs_module_text("Morph.Others.Tip"));
	// Unpaired sources: when this is an item's own transition, how an item
	// with nothing to change into comes and goes. Morph's own ways, a cut, or
	// any other transition, whose own settings follow (`unpaired|g`).
	obs_property_t *unpaired = obs_properties_add_list(props, "unpaired", obs_module_text("Morph.Alone"),
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(unpaired, obs_module_text("Morph.Others.Fade"), kUnpairedFade);
	obs_property_list_add_string(unpaired, obs_module_text("Morph.Others.Zoom"), kUnpairedZoom);
	obs_property_list_add_string(unpaired, obs_module_text("Morph.Others.Slide"), kUnpairedSlide);
	obs_property_list_add_string(unpaired, obs_module_text("MakeRoom.Option"), kUnpairedRoom);
	obs_property_list_add_string(unpaired, obs_module_text("Change.Cut"), kTransitionCut);
	const char *kind = nullptr;
	for (size_t i = 0; obs_enum_transition_types(i, &kind); i++) {
		if (!item_transition_is_fallback_kind(kind))
			continue;
		const char *shown_as = obs_source_get_display_name(kind);
		obs_property_list_add_string(unpaired, shown_as ? shown_as : kind, kind);
	}
	set_tip(unpaired, obs_module_text("Morph.Alone.Tip"));
	if (self) {
		obs_properties_add_group(props, "unpaired|g", obs_module_text("Morph.AloneOwn"), OBS_GROUP_NORMAL,
					 obs_properties_create());
		obs_data_t *settings = obs_source_get_settings(self->source());
		unpaired_own_cb(props, nullptr, settings);
		obs_data_release(settings);
		obs_property_set_modified_callback(unpaired, unpaired_own_cb);
	}
	add_room_properties(props);
	obs_property_t *pair_main = obs_properties_add_bool(props, "pair_main", obs_module_text("Morph.PairMain"));
	set_tip(pair_main, obs_module_text("Morph.PairMain.Tip"));
	obs_property_t *seen = obs_properties_add_bool(props, "visible_pixels", obs_module_text("Morph.Seen"));
	set_tip(seen, obs_module_text("Morph.Seen.Tip"));
	obs_property_t *no_shadows = obs_properties_add_bool(props, "no_shadows", obs_module_text("Measure.NoShadows"));
	set_tip(no_shadows, obs_module_text("Morph.NoShadows.Tip"));
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
		set_tip(scene, obs_module_text("Morph.Rules.Tip"));
		// Two scenes that have a rule already: its settings are shown.
		obs_property_set_modified_callback(scene, rule_chosen);
	}
	obs_property_t *both = obs_properties_add_bool(rule, "rule_both", obs_module_text("Morph.RuleBoth"));
	set_tip(both, obs_module_text("Morph.RuleBoth.Tip"));
	obs_property_t *style = obs_properties_add_list(rule, "rule_style", obs_module_text("Morph.RuleStyle"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(style, obs_module_text("Morph.RuleStyle.Morph"), (int)Style::Morph);
	obs_property_list_add_int(style, obs_module_text("Morph.RuleStyle.Fade"), (int)Style::CrossFade);
	obs_property_list_add_int(style, obs_module_text("Morph.RuleStyle.Cut"), (int)Style::Cut);
	set_tip(style, obs_module_text("Morph.RuleStyle.Tip"));
	obs_property_t *rule_others = obs_properties_add_list(rule, "rule_others", obs_module_text("Morph.Others"),
							      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(rule_others, obs_module_text("Morph.Others.Fade"), (int)Others::Fade);
	obs_property_list_add_int(rule_others, obs_module_text("Morph.Others.Zoom"), (int)Others::Zoom);
	obs_property_list_add_int(rule_others, obs_module_text("Morph.Others.Slide"), (int)Others::Slide);
	obs_property_list_add_int(rule_others, obs_module_text("MakeRoom.Option"), (int)Others::MakeRoom);
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
	obs_data_set_default_string(settings, "unpaired", kUnpairedFade);
	obs_data_set_default_bool(settings, "pair_main", true);
	obs_data_set_default_bool(settings, "visible_pixels", true);
	obs_data_set_default_bool(settings, "no_shadows", true);
	obs_data_set_default_int(settings, "ease_curve", (int)EaseCurve::Cubic);
	obs_data_set_default_int(settings, "ease_mode", (int)EaseMode::InOut);
	obs_data_set_default_int(settings, "room_way", (int)RoomWay::Push);
	obs_data_set_default_bool(settings, "room_fade", false);
	obs_data_set_default_int(settings, "rule_style", (int)Style::Morph);
	obs_data_set_default_int(settings, "rule_others", (int)Others::Fade);
	obs_data_set_default_bool(settings, "rule_pair_main", true);
	obs_data_set_default_int(settings, "rule_ease_curve", (int)EaseCurve::Cubic);
	obs_data_set_default_int(settings, "rule_ease_mode", (int)EaseMode::InOut);
}

void get_room_defaults(obs_data_t *settings)
{
	get_defaults(settings);
	obs_data_set_default_int(settings, "others", (int)Others::MakeRoom);
	obs_data_set_default_string(settings, "unpaired", kUnpairedRoom);
}

} // namespace

bool morph_draws_nothing(obs_source_t *filter)
{
	return filter && draws_nothing(filter);
}

void morph_accept_drawing_filter(bool (*accepts)(obs_source_t *filter))
{
	if (!accepts) {
		for (auto &slot : g_accepts)
			slot.store(nullptr);
		return;
	}
	for (auto &slot : g_accepts) {
		if (slot.load() == accepts)
			return;
		bool (*empty)(obs_source_t *) = nullptr;
		if (slot.compare_exchange_strong(empty, accepts))
			return;
	}
	obs_log(LOG_WARNING, "Morph: no room for another drawing filter to accept");
}

void register_morph()
{
	// Morph, and Make Room: the same transition with Make Room for whatever
	// has no partner (`room`).
	for (const bool room : {false, true}) {
		obs_source_info info = {};
		info.id = room ? kMakeRoomId : kMorphId;
		info.type = OBS_SOURCE_TYPE_TRANSITION;
		if (room) {
			info.get_name = [](void *) {
				return obs_module_text("MakeRoom.Name");
			};
			info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
				return new Morph(settings, source, true);
			};
		} else {
			info.get_name = [](void *) {
				return obs_module_text("Morph.Name");
			};
			info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
				return new Morph(settings, source, false);
			};
		}
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
		info.get_properties = room ? get_room_properties : get_properties;
		info.get_defaults = room ? get_room_defaults : get_defaults;
		register_timed(&info);
	}
}

} // namespace stagekit
