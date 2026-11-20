/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Scene Shadow: one shadow, glow or outline for every source of a scene, as
// a filter on the scene.
//
// The scene draws itself as it always does (every item transition, crop and
// blending mode is OBS's own). The filter then draws the scene's items again
// as two silhouettes: the sources that cast (not background, not listed under
// "Ignored sources", without a Shadow, Glow & Outline of their own), and every
// source that is not background. The first, softened and offset, is laid over
// the finished picture as the shadow, and kept off the second, so the shadow
// falls on the background and never on a source. Nothing is ever moved or
// resized: the sources keep their size, so Fit to Space, the arrange tools and
// Morph have nothing to allow for.
//
// Where it differs from a shadow drawn under each source: overlapping sources
// make one shape and do not shadow each other, and at a see-through edge of a
// source the background under it is darkened very slightly differently. The
// silhouette also leaves out a crop to the bounding box (as Morph does).
//
// *Shadow from: Each source by itself* (`shadow_from` 1) is the shadow drawn
// under each source: a source's shadow falls on whatever lies under it, other
// sources too, and the sources above it cover it. The sources are drawn in
// layers (`shadow_layers`): a run of sources, bottom first, whose shadows
// cannot reach one another is one layer, so sources side by side cost what
// one shape costs and only sources that overlap (shadows included) take a
// layer more. Layer by layer from the bottom, its casting sources' shadow is
// laid over the picture so far, kept off every source of that layer and
// above (`LookPaint::compose_layers`).

#include "scene-shadow.hpp"
#include "outline-ambient.hpp"

#include "../common/easing.hpp"
#include "../common/filter-input.hpp"
#include "../common/filter-order.hpp"
#include "../common/loading.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-filter-ids.hpp"
#include "../common/scene-rev.hpp"
#include "../common/scene-util.hpp"
#include "../common/stand-in.hpp"
#include "../common/switch-ramp.hpp"
#include "../common/timing.hpp"
#include "../shadow/shadow-backdrop.hpp"
#include "../transition/morph.hpp"
#include "scene-blur.hpp"
#include "../common/tooltip.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace stagekit {

namespace {

// A source counts as covering the screen from nine tenths of it (Morph's rule).
constexpr double kFills = 0.9;
// How often the scene's items are looked at again while it is showing.
constexpr double kLookEvery = 0.25;
// How long after it was last drawn (or asked for by Morph) the filter keeps
// looking, though its scene is not showing.
constexpr uint64_t kWantedNs = 1000000000ULL;

// Sources covered. Saved by number: append only. A filter saved before
// there was a choice has no number and reads as All sources, which is what
// it did then.
enum class Which : int { AllSources = 0, Chosen = 1 };

// Shadow from. Saved by number: append only. 0 is what every filter saved
// before it did.
enum class From : int { Scene = 0, Each = 1 };

bool listed(const std::vector<std::string> &list, const char *name)
{
	return name && std::find(list.begin(), list.end(), name) != list.end();
}

// The ids of a scene's own items (a group as one), from the back.
std::vector<int64_t> item_ids(obs_source_t *scene_source)
{
	std::vector<int64_t> ids;
	if (obs_scene_t *scene = obs_scene_from_source(scene_source))
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
				static_cast<std::vector<int64_t> *>(param)->push_back(obs_sceneitem_get_id(item));
				return true;
			},
			&ids);
	return ids;
}

// What each item of the scene does, by its id. See ShadowFlag.
// `chosen`, when not null: only these cast (the rest are still kept clear).
void classify(obs_source_t *scene_source, const std::vector<std::string> &background,
	      const std::vector<std::string> &skip, const std::vector<std::string> *chosen, bool auto_background,
	      ShadowFlags &out)
{
	out.clear();
	obs_scene_t *scene = obs_scene_from_source(scene_source);
	if (!scene)
		return;
	std::vector<obs_sceneitem_t *> items;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			obs_sceneitem_addref(item);
			static_cast<std::vector<obs_sceneitem_t *> *>(param)->push_back(item);
			return true;
		},
		&items);
	const double cw = obs_source_get_width(scene_source), ch = obs_source_get_height(scene_source);
	bool at_bottom = true; // still in the unbroken run of full-screen sources at the bottom
	std::vector<std::pair<int64_t, int64_t>> twins;
	for (obs_sceneitem_t *item : items) {
		const int64_t id = obs_sceneitem_get_id(item);
		obs_source_t *source = obs_sceneitem_get_source(item);
		const char *name = obs_source_get_name(source);
		if (const int64_t owner = stand_in_for_item(item)) {
			twins.emplace_back(id, owner);
			continue;
		}
		// The layer Morph draws two items changing into each other with:
		// the size of the scene, never background, and it casts.
		if (is_morph_layer(item)) {
			out[id] = kShadowCasts | kShadowMasked;
			continue;
		}
		bool back = listed(background, name);
		if (!back && auto_background && at_bottom && !obs_sceneitem_is_group(item) && cw > 0.0 && ch > 0.0) {
			const Box box = item_box(item);
			const double x0 = std::max(0.0, box.x), y0 = std::max(0.0, box.y);
			const double x1 = std::min(cw, box.x + box.w), y1 = std::min(ch, box.y + box.h);
			back = x1 > x0 && y1 > y0 && (x1 - x0) * (y1 - y0) >= kFills * cw * ch;
		}
		if (!back)
			at_bottom = false;
		uint8_t flags = back ? kShadowBackground : kShadowMasked;
		// A source with a shadow of its own keeps that one: no shadow twice.
		if (!back && !listed(skip, name) && (!chosen || listed(*chosen, name)) &&
		    !first_filter(source, "stagekit_shadow_backdrop"))
			flags |= kShadowCasts;
		out[id] = flags;
	}
	// A Standby stand-in does what the source it stands in for does.
	for (const auto &[twin, owner] : twins) {
		const auto found = out.find(owner);
		out[twin] = found != out.end() ? found->second : (uint8_t)kShadowMasked;
	}
	for (obs_sceneitem_t *item : items)
		obs_sceneitem_release(item);
}

class SceneShadow {
public:
	SceneShadow(obs_data_t *settings, obs_source_t *source);
	~SceneShadow();

	obs_source_t *filter() const { return source_; }
	void update(obs_data_t *settings);
	void tick(float seconds);
	void render();
	bool now(SceneShadowNow &out);
	gs_texture_t *lay_over(gs_texture_t *picture, uint32_t cx, uint32_t cy);
	void attached() { ramp_.attached(); }
	int draws() const { return draws_.load(); }
	int layers() const { return layers_.load(); }

private:
	void look_again(obs_source_t *parent);
	bool render_layers(const std::vector<obs_sceneitem_t *> &items,
			   const std::function<uint8_t(obs_sceneitem_t *)> &flags_of,
			   const std::function<void(obs_sceneitem_t *)> &draw, const ShadowLook &look, uint32_t cx,
			   uint32_t cy);
	void finish(const ShadowLook &look, uint64_t frame, uint32_t cx, uint32_t cy, bool linear, bool each);
	bool has_new_items(obs_source_t *parent);

	obs_source_t *source_;
	SwitchRamp ramp_;

	std::mutex mutex_;
	ShadowLook look_;
	std::vector<std::string> background_, skip_, chosen_;
	Which which_ = Which::AllSources;
	From from_ = From::Scene;
	bool auto_background_ = true;
	ShadowFlags flags_;
	bool classified_ = false;
	bool blocked_ = false; // a filter that draws comes before it
	bool wide_ = false;    // the canvas is in a wide colour space
	bool on_group_ = false;

	// On the tick only.
	double countdown_ = -1.0;
	bool was_showing_ = false;
	std::string told_blocked_;
	uint64_t seen_rev_ = 0; // scene_revision() when its items were last checked for new ones
	std::atomic<uint64_t> wanted_ns_{0};
	std::atomic<bool> settings_changed_{true};

	// On the drawing thread only.
	std::unique_ptr<LookPaint> paint_;
	gs_texrender_t *tr_scene_ = nullptr;
	gs_texrender_t *tr_over_ = nullptr; // lay_over's picture
	// The frame's canvases (casts, masks, lines, presences), taken in turn.
	std::vector<gs_texrender_t *> pool_;
	size_t pool_used_ = 0;
	gs_texrender_t *canvas()
	{
		if (pool_used_ >= pool_.size())
			pool_.push_back(gs_texrender_create(GS_RGBA, GS_ZS_NONE));
		return pool_[pool_used_++];
	}
	// What the last frame's shadow was laid in: one stage for the sources
	// that are whole, and one for each that is fading in or out (in each
	// layer, with Each source by itself).
	std::vector<LookPaint::Stage> stages_;
	void add_stages(const std::vector<obs_sceneitem_t *> &items,
			const std::function<uint8_t(obs_sceneitem_t *)> &flags_of,
			const std::function<void(obs_sceneitem_t *)> &draw, const std::vector<LayerBox> *boxes,
			size_t lo, size_t hi, gs_texture_t *mask, const ShadowLook &look, uint32_t cx, uint32_t cy);
	bool last_each_ = false;
	ShadowLook last_look_;
	uint64_t last_frame_ = 0;
	uint32_t last_cx_ = 0, last_cy_ = 0;
	bool last_linear_ = false;
	bool have_out_ = false;
	std::atomic<int> draws_{0};
	std::atomic<int> layers_{0}; // the layers the last frame was drawn in
};

SceneShadow::SceneShadow(obs_data_t *settings, obs_source_t *source) : source_(source), ramp_(source)
{
	paint_ = std::make_unique<LookPaint>();
	obs_enter_graphics();
	tr_scene_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	tr_over_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();
	// For the tests: how many times it has drawn the shadow (once a frame
	// at most, however often the scene is drawn).
	proc_handler_add(
		obs_source_get_proc_handler(source), "void draws(out int count)",
		[](void *data, calldata_t *cd) {
			calldata_set_int(cd, "count", static_cast<SceneShadow *>(data)->draws());
		},
		this);
	// And in how many layers the last one was drawn (1 for The scene as drawn).
	proc_handler_add(
		obs_source_get_proc_handler(source), "void layers(out int count)",
		[](void *data, calldata_t *cd) {
			calldata_set_int(cd, "count", static_cast<SceneShadow *>(data)->layers());
		},
		this);
	update(settings);
}

SceneShadow::~SceneShadow()
{
	paint_.reset();
	obs_enter_graphics();
	gs_texrender_destroy(tr_scene_);
	gs_texrender_destroy(tr_over_);
	for (gs_texrender_t *tr : pool_)
		gs_texrender_destroy(tr);
	obs_leave_graphics();
}

void SceneShadow::update(obs_data_t *settings)
{
	const ShadowLook look = read_shadow_look(settings);
	const double ramp = std::max(0.0, (double)obs_data_get_int(settings, "ramp_ms") / 1000.0);
	ramp_.configure(ramp, ramp, static_cast<EaseCurve>(obs_data_get_int(settings, "ramp_ease_curve")),
			static_cast<EaseMode>(obs_data_get_int(settings, "ramp_ease_mode")));
	std::lock_guard lock(mutex_);
	look_ = look;
	background_ = pick_list_read(settings, "background", false);
	skip_ = pick_list_read(settings, "shadow_skip", false);
	which_ = static_cast<Which>(std::clamp((int)obs_data_get_int(settings, "which"), 0, 1));
	from_ = static_cast<From>(std::clamp((int)obs_data_get_int(settings, "shadow_from"), 0, 1));
	chosen_ = pick_list_read(settings, "shadow_sources", false);
	auto_background_ = obs_data_get_bool(settings, "auto_background");
	settings_changed_ = true;
}

// Which items cast, are background or are kept clear, and whether the filter
// can draw at all where it is.
void SceneShadow::look_again(obs_source_t *parent)
{
	// A filter that draws before this one: the scene's picture is no longer
	// where its items are, and the shadow would not fit it.
	std::string in_the_way;
	struct Find {
		obs_source_t *self;
		bool reached = false;
		std::string *in_the_way;
	} find{source_, false, &in_the_way};
	obs_source_enum_filters(
		parent,
		[](obs_source_t *, obs_source_t *f, void *param) {
			auto *x = static_cast<Find *>(param);
			if (f == x->self) {
				x->reached = true;
				return;
			}
			// (Filters come in the order they are applied: those before this one first.
			// A Scene Blur, and a Scene Corners on the whole scene, move
			// themselves after this one within half a second.)
			const char *id = obs_source_get_unversioned_id(f);
			if (!x->reached && x->in_the_way->empty() && obs_source_enabled(f) && !morph_draws_nothing(f) &&
			    !(id && (std::strcmp(id, kSceneBlurId) == 0 || std::strcmp(id, kSceneCornersId) == 0)))
				*x->in_the_way = obs_source_get_name(f) ? obs_source_get_name(f) : "?";
		},
		&find);
	if (!in_the_way.empty() && in_the_way != told_blocked_)
		obs_log(LOG_INFO, "Scene Shadow '%s': the filter '%s' draws before it, so it draws nothing",
			obs_source_get_name(source_), in_the_way.c_str());
	told_blocked_ = in_the_way;
	obs_video_info ovi = {};
	const bool wide = obs_get_video_info(&ovi) &&
			  (ovi.colorspace == VIDEO_CS_2100_PQ || ovi.colorspace == VIDEO_CS_2100_HLG);
	std::vector<std::string> background, skip, chosen;
	bool auto_background, only_chosen;
	{
		std::lock_guard lock(mutex_);
		background = background_;
		skip = skip_;
		chosen = chosen_;
		only_chosen = which_ == Which::Chosen;
		auto_background = auto_background_;
	}
	ShadowFlags flags;
	classify(parent, background, skip, only_chosen ? &chosen : nullptr, auto_background, flags);
	std::lock_guard lock(mutex_);
	flags_.swap(flags);
	classified_ = true;
	blocked_ = !in_the_way.empty();
	wide_ = wide;
}

// Whether the scene has an item the last look did not see: one added since
// (the item Morph's layer, a Standby stand-in, a source put in the scene).
bool SceneShadow::has_new_items(obs_source_t *parent)
{
	const std::vector<int64_t> ids = item_ids(parent);
	std::lock_guard lock(mutex_);
	for (const int64_t id : ids)
		if (!flags_.count(id))
			return true;
	return false;
}

void SceneShadow::tick(float seconds)
{
	ramp_.tick(seconds);
	obs_source_t *parent = obs_filter_get_parent(source_);
	if (!parent)
		return;
	if (obs_source_is_group(parent)) {
		std::lock_guard lock(mutex_);
		on_group_ = true;
		return;
	}
	// Nothing is looked at while OBS loads a collection: only some scenes
	// have their items yet.
	if (!scenes_loaded())
		return;
	{
		// An Ambient Background fill: its background looked up here, not
		// while drawing (outline-ambient.hpp).
		bool ambient;
		uint32_t choice;
		{
			std::lock_guard lock(mutex_);
			ambient = look_.outline && look_.design.ambient_fill();
			choice = look_.design.ambient;
		}
		if (ambient)
			outline_ambient_warm(choice, parent);
	}
	if (countdown_ < 0.0) {
		// TODO(stagger): use stagger_phase() once it is in. Until then the
		// filters spread their looks over the quarter second by their id.
		const char *uuid = obs_source_get_uuid(source_);
		countdown_ = (double)(std::hash<std::string>{}(uuid ? uuid : "") % 250) / 1000.0;
		look_again(parent);
		return;
	}
	const uint64_t now = os_gettime_ns();
	// (Taken from the clock on the drawing thread, maybe after `now`: never
	// subtracted the wrong way round.)
	const uint64_t wanted = wanted_ns_.load();
	const bool showing = obs_source_showing(parent) || (wanted && (wanted >= now || now - wanted < kWantedNs));
	countdown_ -= seconds;
	// An item added while it is seen is looked at in this very tick, before
	// the frame is drawn: up to a quarter second "not looked at yet" was a
	// blink of the shadow. The item Morph's layer is put in on OBS's own
	// thread and starts drawing from the item Morph's next tick callback,
	// which OBS calls before it ticks the sources; and while the layer was
	// not looked at the two items it draws cast nothing (the owner: "shadows
	// flicker when ... i switch/show or hide sources inside a scene"). Only
	// when something in a scene changed, and then only the ids are compared.
	bool new_items = false;
	if (showing) {
		const uint64_t rev = scene_revision();
		if (rev != seen_rev_) {
			seen_rev_ = rev;
			new_items = has_new_items(parent);
		}
	}
	// At once when it comes into view, its settings change or it has a new
	// item; then a few times a second while it is seen. Off screen it does
	// nothing.
	const bool due = (showing && !was_showing_) || settings_changed_.exchange(false) || new_items ||
			 (showing && countdown_ <= 0.0);
	was_showing_ = showing;
	if (!due)
		return;
	countdown_ = kLookEvery;
	look_again(parent);
}

bool SceneShadow::now(SceneShadowNow &out)
{
	wanted_ns_ = os_gettime_ns();
	if (!scenes_loaded() || !paint_->ready())
		return false;
	{
		std::lock_guard lock(mutex_);
		if (!classified_ || blocked_ || wide_ || on_group_)
			return false;
		out.look = scaled_look(look_, ramp_.level());
		out.flags = flags_;
		out.each = from_ == From::Each;
	}
	// (As in render: this scene's own background first. Nothing locked.)
	if (out.look.design.ambient_fill() && out.look.outline)
		out.look.design.ambient_id =
			outline_ambient_resolve(out.look.design.ambient, obs_filter_get_parent(source_));
	return true;
}

void SceneShadow::render()
{
	obs_source_t *target = obs_filter_get_target(source_);
	const uint32_t cx = target ? obs_source_get_base_width(target) : 0;
	const uint32_t cy = target ? obs_source_get_base_height(target) : 0;
	wanted_ns_ = os_gettime_ns();
	ShadowLook look;
	bool usable, each;
	{
		std::lock_guard lock(mutex_);
		look = look_;
		usable = classified_ && !blocked_ && !wide_ && !on_group_;
		each = from_ == From::Each;
	}
	look = scaled_look(look, ramp_.level());
	// An Ambient Background fill: the background in this scene before any
	// other (kept by the lookup; Morph and Scene Blur get the same one).
	if (look.design.ambient_fill() && look.outline)
		look.design.ambient_id = outline_ambient_resolve(look.design.ambient, obs_filter_get_parent(source_));

	if (!usable || !paint_->ready() || !scenes_loaded() || !look.any() || !cx || !cy) {
		skip_filter_input(source_);
		have_out_ = false;
		return;
	}
	// Once a frame. The multiview, a projector and Studio Mode's preview
	// draw a scene again in the same frame, with its items as they were;
	// those draws take the picture made the first time. (This is safe for a
	// scene, unlike for a source a Standby stand-in draws two ways in one
	// frame: the stand-in draws its source, never the scene.)
	const uint64_t frame = obs_get_video_frame_time();
	const bool linear = gs_get_linear_srgb();
	if (have_out_ && frame == last_frame_ && cx == last_cx_ && cy == last_cy_ && linear == last_linear_) {
		paint_->draw_out(cx, cy);
		return;
	}
	have_out_ = false;
	if (!draw_filter_input(source_, tr_scene_, cx, cy)) {
		skip_filter_input(source_);
		return;
	}
	ShadowFlags flags;
	{
		std::lock_guard lock(mutex_);
		flags = flags_;
	}
	// The scene's items, from the back, each held for this call only.
	std::vector<obs_sceneitem_t *> items;
	obs_scene_enum_items(
		obs_scene_from_source(obs_filter_get_parent(source_)),
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			obs_sceneitem_addref(item);
			static_cast<std::vector<obs_sceneitem_t *> *>(param)->push_back(item);
			return true;
		},
		&items);
	auto flags_of = [&](obs_sceneitem_t *item) -> uint8_t {
		const auto found = flags.find(obs_sceneitem_get_id(item));
		if (found != flags.end())
			return found->second;
		// (An item added since the last look is kept clear, and casts
		// nothing until it has been looked at; the tick looks at once. The
		// item Morph's layer always casts: it draws two sources that do.)
		return is_morph_layer(item) ? (uint8_t)(kShadowCasts | kShadowMasked) : (uint8_t)kShadowMasked;
	};
	// A Scene Blur after this one that blurs each source by itself: such a
	// source's shape is its blurred image's (a mosaic's whole cells, say), so
	// that is what casts and is kept clear.
	struct FindBlur {
		obs_source_t *blur = nullptr; // a reference
	} find_blur;
	obs_source_enum_filters(
		obs_filter_get_parent(source_),
		[](obs_source_t *, obs_source_t *f, void *param) {
			auto *x = static_cast<FindBlur *>(param);
			if (!x->blur && obs_source_enabled(f) && is_scene_blur(f))
				x->blur = obs_source_get_ref(f);
		},
		&find_blur);
	auto draw = [&](obs_sceneitem_t *item) {
		matrix4 m;
		uint32_t w = 0, h = 0;
		gs_texture_t *blurred = find_blur.blur ? scene_blur_item_picture(find_blur.blur, item, m, w, h)
						       : nullptr;
		if (blurred)
			paint_->draw_picture(blurred, m, w, h);
		else
			paint_->draw_item(item);
	};
	paint_->begin_frame();
	pool_used_ = 0;
	stages_.clear();
	if (each) {
		const bool drawn = render_layers(items, flags_of, draw, look, cx, cy);
		obs_source_release(find_blur.blur);
		for (obs_sceneitem_t *item : items)
			obs_sceneitem_release(item);
		if (!drawn) {
			skip_filter_input(source_);
			return;
		}
		finish(look, frame, cx, cy, linear, true);
		return;
	}
	layers_ = 1;
	// A source on its way in or out (its own show or hide transition, or the
	// item Morph's layer) is see-through as a whole, and its line and shadow
	// are laid by themselves (add_stages): then the mask is every source
	// kept clear, as it is drawn. With none, the mask is the casting sources
	// and whatever else is kept clear.
	bool masked_only = false, fading = false;
	for (obs_sceneitem_t *item : items) {
		const uint8_t f = flags_of(item);
		masked_only = masked_only || ((f & kShadowMasked) && !(f & kShadowCasts));
		fading = fading || ((f & kShadowCasts) && LookPaint::may_fade(item));
	}
	gs_texture_t *mask = nullptr;
	if (fading) {
		gs_texrender_t *tr = canvas();
		if (paint_->begin_canvas(tr, cx, cy)) {
			for (obs_sceneitem_t *item : items)
				if (flags_of(item) & kShadowMasked)
					draw(item);
			paint_->end_canvas(tr);
			mask = gs_texrender_get_texture(tr);
		}
	}
	add_stages(items, flags_of, draw, nullptr, 0, items.size(), mask, look, cx, cy);
	if (!fading && masked_only && !stages_.empty()) {
		gs_texrender_t *tr = canvas();
		if (paint_->begin_canvas(tr, cx, cy)) {
			paint_->copy_onto(stages_[0].cast, cx, cy);
			for (obs_sceneitem_t *item : items) {
				const uint8_t f = flags_of(item);
				if ((f & kShadowMasked) && !(f & kShadowCasts))
					draw(item);
			}
			paint_->end_canvas(tr);
			stages_[0].mask = gs_texrender_get_texture(tr);
		}
	}
	obs_source_release(find_blur.blur);
	for (obs_sceneitem_t *item : items)
		obs_sceneitem_release(item);
	if (!paint_->compose_stages(gs_texrender_get_texture(tr_scene_), stages_, look, cx, cy)) {
		skip_filter_input(source_);
		return;
	}
	finish(look, frame, cx, cy, linear, false);
}

// The stages for the casting sources among items lo up to hi (`boxes`: only
// those present), all kept off `mask` (nullptr: off themselves). The ones
// that are whole are one stage. Each one fading in or out is laid by itself
// (those whose lines cannot meet, together), with a presence: the mask with
// that source whole on it, so its line and shadow are as faded as it is and
// never lie under it. Laid by itself because a line is kept off the whole of
// the presence: another source's line under this one must still show through
// it as far as it is see-through.
void SceneShadow::add_stages(const std::vector<obs_sceneitem_t *> &items,
			     const std::function<uint8_t(obs_sceneitem_t *)> &flags_of,
			     const std::function<void(obs_sceneitem_t *)> &draw, const std::vector<LayerBox> *boxes,
			     size_t lo, size_t hi, gs_texture_t *mask, const ShadowLook &look, uint32_t cx, uint32_t cy)
{
	constexpr size_t kMostUnits = 6;
	std::vector<obs_sceneitem_t *> whole, fading;
	for (size_t i = lo; i < hi && i < items.size(); i++) {
		if ((boxes && !(*boxes)[i].present) || !(flags_of(items[i]) & kShadowCasts))
			continue;
		// (Without a mask of all the sources there is no presence to make.)
		(mask && LookPaint::may_fade(items[i]) ? fading : whole).push_back(items[i]);
	}
	auto stage = [&](const std::vector<obs_sceneitem_t *> &group, bool with_presence) {
		LookPaint::Stage out;
		out.mask = mask;
		gs_texrender_t *cast = canvas();
		if (!paint_->begin_canvas(cast, cx, cy))
			return;
		for (obs_sceneitem_t *item : group)
			draw(item);
		paint_->end_canvas(cast);
		out.cast = gs_texrender_get_texture(cast);
		if (!out.cast)
			return;
		if (look.box_line()) {
			gs_texrender_t *ring = canvas();
			if (paint_->begin_canvas(ring, cx, cy)) {
				// (An inner line lies under every other source kept clear
				// that is above it: those of this stretch, and the layers
				// above.)
				for (size_t i = lo; i < items.size(); i++) {
					if (boxes && !(*boxes)[i].present)
						continue;
					if (i < hi && std::find(group.begin(), group.end(), items[i]) != group.end())
						paint_->draw_item_ring(items[i], look);
					else if (look.inner_line() && (flags_of(items[i]) & kShadowMasked))
						paint_->cover_ring(items[i]);
				}
				paint_->end_canvas(ring);
				out.ring = gs_texrender_get_texture(ring);
			}
		}
		if (with_presence) {
			gs_texrender_t *presence = canvas();
			if (paint_->begin_canvas(presence, cx, cy)) {
				paint_->copy_onto(mask, cx, cy);
				for (obs_sceneitem_t *item : group)
					paint_->draw_presence(item);
				paint_->end_canvas(presence);
				out.presence = gs_texrender_get_texture(presence);
			}
		}
		stages_.push_back(out);
	};
	if (!whole.empty())
		stage(whole, false);
	if (fading.empty())
		return;
	std::vector<LayerBox> places(fading.size());
	for (size_t k = 0; k < fading.size(); k++)
		item_drawn_box(fading[k], places[k]);
	std::vector<size_t> starts = shadow_layers(places, shadow_reach(look), kMostUnits);
	if (starts.empty())
		starts.push_back(0);
	for (size_t u = 0; u < starts.size(); u++) {
		const size_t end = u + 1 < starts.size() ? starts[u + 1] : fading.size();
		stage(std::vector<obs_sceneitem_t *>(fading.begin() + (long)starts[u], fading.begin() + (long)end),
		      true);
	}
}

void SceneShadow::finish(const ShadowLook &look, uint64_t frame, uint32_t cx, uint32_t cy, bool linear, bool each)
{
	paint_->draw_out(cx, cy);
	draws_++;
	have_out_ = true;
	last_look_ = look;
	last_each_ = each;
	last_frame_ = frame;
	last_cx_ = cx;
	last_cy_ = cy;
	last_linear_ = linear;
}

// Each source by itself: the items in layers whose shadows cannot reach one
// another, each layer's shadow laid over the picture from the bottom up and
// kept off that layer and every one above it.
bool SceneShadow::render_layers(const std::vector<obs_sceneitem_t *> &items,
				const std::function<uint8_t(obs_sceneitem_t *)> &flags_of,
				const std::function<void(obs_sceneitem_t *)> &draw, const ShadowLook &look, uint32_t cx,
				uint32_t cy)
{
	// Only the sources that are not background are in the stack: the
	// background lies under every one of them and takes every shadow.
	std::vector<LayerBox> boxes(items.size());
	for (size_t i = 0; i < items.size(); i++)
		if (flags_of(items[i]) & kShadowMasked)
			item_drawn_box(items[i], boxes[i]);
	const std::vector<size_t> starts = shadow_layers(boxes, shadow_reach(look), kShadowMostLayers);
	const size_t n = starts.size();
	layers_ = (int)n;
	if (!n)
		return false;
	auto end_of = [&](size_t l) {
		return l + 1 < n ? starts[l + 1] : items.size();
	};
	// The masks from the top down: each is its own layer's sources with the
	// one above it laid over them (in that order, so its colours are the
	// sources' as they are drawn: the shader takes them for the sources'
	// part of a pixel).
	std::vector<gs_texture_t *> masks(n, nullptr);
	for (size_t k = n; k-- > 0;) {
		gs_texrender_t *tr = canvas();
		if (!paint_->begin_canvas(tr, cx, cy))
			return false;
		for (size_t i = starts[k]; i < end_of(k); i++)
			if (boxes[i].present)
				draw(items[i]);
		if (k + 1 < n)
			paint_->copy_onto(masks[k + 1], cx, cy, true);
		paint_->end_canvas(tr);
		masks[k] = gs_texrender_get_texture(tr);
		if (!masks[k])
			return false;
	}
	// Each layer's casting sources from the bottom up (a source fading in
	// or out by itself: add_stages).
	for (size_t l = 0; l < n; l++)
		add_stages(items, flags_of, draw, &boxes, starts[l], end_of(l), masks[l], look, cx, cy);
	return paint_->compose_stages(gs_texrender_get_texture(tr_scene_), stages_, look, cx, cy);
}

// The shadow drawn this frame, from the same shapes, over another picture of
// the scene.
gs_texture_t *SceneShadow::lay_over(gs_texture_t *picture, uint32_t cx, uint32_t cy)
{
	// (Asked from inside the same drawing as the shadow's own, just after it.
	// A drawing outside OBS's video thread can see the video frame's time
	// move on between the two: up to two frames on is taken for the same.)
	const uint64_t now = obs_get_video_frame_time();
	const uint64_t two_frames = 2 * obs_get_frame_interval_ns();
	if (!picture || !have_out_ || !obs_source_enabled(source_) || now < last_frame_ ||
	    now - last_frame_ > two_frames || cx != last_cx_ || cy != last_cy_)
		return nullptr;
	return paint_->compose_stages(picture, stages_, last_look_, cx, cy, tr_over_)
		       ? gs_texrender_get_texture(tr_over_)
		       : nullptr;
}

// --------------------------------------------------------------- properties

bool look_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool glow = obs_data_get_int(settings, "shadow_type") == 1;
	const bool picture = obs_data_get_int(settings, "shadow_fill") == 1;
	obs_property_set_visible(obs_properties_get(props, "offset_x"), !glow);
	obs_property_set_visible(obs_properties_get(props, "offset_y"), !glow);
	obs_property_set_visible(obs_properties_get(props, "shadow_color"), !picture);
	obs_property_set_visible(obs_properties_get(props, "shadow_boost"), picture);
	return true;
}

obs_property_t *slider(obs_properties_t *props, const char *name, const char *text, double min, double max,
		       const char *suffix)
{
	obs_property_t *p = obs_properties_add_float_slider(props, name, obs_module_text(text), min, max, 1.0);
	obs_property_float_set_suffix(p, suffix);
	return p;
}

bool which_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool chosen = obs_data_get_int(settings, "which") == (int)Which::Chosen;
	obs_property_set_visible(obs_properties_get(props, "shadow_sources|alist"), chosen);
	// The sources listed are all there is to cover: nothing more to leave out.
	obs_property_set_visible(obs_properties_get(props, "shadow_skip|alist"), !chosen);
	return true;
}

obs_properties_t *get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_source_t *filter = data ? static_cast<SceneShadow *>(data)->filter() : nullptr;
	obs_source_t *parent = filter ? obs_filter_get_parent(filter) : nullptr;
	if (parent && obs_source_is_group(parent)) {
		add_on_group_warning(props, obs_module_text("SceneShadow.OnGroup"));
	}

	// What it applies to and the lists first, then the look, then the timing:
	// the order of every scene filter's window.
	// The scene's own sources (a group as one), for the three lists.
	auto sources = [filter](std::vector<std::string> &names) {
		obs_source_t *scene = filter ? obs_filter_get_parent(filter) : nullptr;
		obs_scene_t *s = scene ? obs_scene_from_source(scene) : nullptr;
		if (!s)
			return;
		obs_scene_enum_items(
			s,
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
				obs_source_t *source = obs_sceneitem_get_source(item);
				const char *name = obs_source_get_name(source);
				auto *out = static_cast<std::vector<std::string> *>(param);
				if (name && *name && !is_stand_in(source) && !is_morph_layer(item) &&
				    std::find(out->begin(), out->end(), name) == out->end())
					out->push_back(name);
				return true;
			},
			&names);
	};
	obs_property_t *from = obs_properties_add_list(props, "shadow_from", obs_module_text("SceneShadow.From"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	// Each source first, as in Scene Corners and Scene Blur; the numbers stay as saved.
	obs_property_list_add_int(from, obs_module_text("SceneShadow.From.Each"), (int)From::Each);
	obs_property_list_add_int(from, obs_module_text("SceneShadow.From.Scene"), (int)From::Scene);
	set_tip(from, obs_module_text("SceneShadow.From.Tip"));
	obs_property_t *which = obs_properties_add_list(props, "which", obs_module_text("SceneShadow.Which"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(which, obs_module_text("SceneShadow.Which.All"), (int)Which::AllSources);
	obs_property_list_add_int(which, obs_module_text("SceneShadow.Which.Chosen"), (int)Which::Chosen);
	set_tip(which, obs_module_text("SceneShadow.Which.Tip"));
	obs_property_set_modified_callback(which, which_modified);
	PickList chosen;
	chosen.setting = "shadow_sources";
	chosen.title = "SceneShadow.Sources";
	chosen.add = "SceneShadow.AddSource";
	chosen.tip = "SceneShadow.Sources.Tip";
	chosen.choices = sources;
	pick_list_add(props, filter, chosen);
	PickList skip;
	skip.setting = "shadow_skip";
	skip.title = "SceneShadow.Skip";
	skip.add = "SceneShadow.AddSource";
	skip.tip = "SceneShadow.Skip.Tip";
	skip.choices = sources;
	pick_list_add(props, filter, skip);
	PickList background;
	background.setting = "background";
	background.title = "SceneShadow.Background";
	background.add = "SceneShadow.AddSource";
	background.tip = "SceneShadow.Background.Tip";
	background.choices = sources;
	pick_list_add(props, filter, background);
	obs_property_t *autob =
		obs_properties_add_bool(props, "auto_background", obs_module_text("SceneShadow.AutoBackground"));
	set_tip(autob, obs_module_text("SceneShadow.AutoBackground.Tip"));

	obs_properties_t *shadow = obs_properties_create();
	obs_property_t *type = obs_properties_add_list(shadow, "shadow_type", obs_module_text("Shadow.Type"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(type, obs_module_text("Shadow.Type.Drop"), 0);
	obs_property_list_add_int(type, obs_module_text("Shadow.Type.Glow"), 1);
	set_tip(type, obs_module_text("Shadow.Type.Tip"));
	obs_property_set_modified_callback(type, look_modified);
	obs_property_t *fill = obs_properties_add_list(shadow, "shadow_fill", obs_module_text("Shadow.Fill"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(fill, obs_module_text("Shadow.Fill.Color"), 0);
	obs_property_list_add_int(fill, obs_module_text("Shadow.Fill.Picture"), 1);
	set_tip(fill, obs_module_text("Shadow.Fill.Tip"));
	obs_property_set_modified_callback(fill, look_modified);
	obs_properties_add_color(shadow, "shadow_color", obs_module_text("Shadow.Color"));
	obs_property_t *boost = slider(shadow, "shadow_boost", "Shadow.Boost", 0.0, 100.0, "%");
	set_tip(boost, obs_module_text("Shadow.Boost.Tip"));
	slider(shadow, "shadow_opacity", "Shadow.Opacity", 0.0, 100.0, "%");
	slider(shadow, "offset_x", "Shadow.OffsetX", -200.0, 200.0, " px");
	slider(shadow, "offset_y", "Shadow.OffsetY", -200.0, 200.0, " px");
	set_tip(slider(shadow, "softness", "Shadow.Softness", 0.0, 200.0, " px"),
		obs_module_text("Shadow.Softness.Tip"));
	set_tip(slider(shadow, "spread", "Shadow.Spread", 0.0, 100.0, "%"), obs_module_text("Shadow.Spread.Tip"));
	add_intensity_property(shadow);
	obs_properties_add_group(props, "shadow", obs_module_text("Shadow.Shadow"), OBS_GROUP_CHECKABLE, shadow);

	obs_properties_t *outline = obs_properties_create();
	slider(outline, "outline_width", "Shadow.OutlineWidth", 0.0, 100.0, " px");
	obs_properties_add_color(outline, "outline_color", obs_module_text("Shadow.OutlineColor"));
	slider(outline, "outline_opacity", "Shadow.OutlineOpacity", 0.0, 100.0, "%");
	add_outline_corner_properties(outline);
	add_outline_design_properties(outline);
	obs_property_t *ring = obs_properties_add_group(props, "outline", obs_module_text("Shadow.Outline"),
							OBS_GROUP_CHECKABLE, outline);
	set_tip(ring, obs_module_text("SceneShadow.Outline.Tip"));

	obs_property_t *ramp =
		obs_properties_add_int_slider(props, "ramp_ms", obs_module_text("SceneShadow.Ramp"), 0, 5000, 50);
	obs_property_int_set_suffix(ramp, " ms");
	set_tip(ramp, obs_module_text("SceneShadow.Ramp.Tip"));
	add_easing_properties(props, "ramp_");
	set_tips_where_missing(props, {{"shadow_color", obs_module_text("Shadow.Color.Tip")},
				       {"shadow_opacity", obs_module_text("Shadow.Opacity.Tip")},
				       {"offset_x", obs_module_text("Shadow.Offset.Tip")},
				       {"offset_y", obs_module_text("Shadow.Offset.Tip")},
				       {"outline_width", obs_module_text("Shadow.OutlineWidth.Tip")},
				       {"outline_color", obs_module_text("Shadow.OutlineLook.Tip")},
				       {"outline_opacity", obs_module_text("Shadow.OutlineLook.Tip")},
				       {"backdrop_color", obs_module_text("Shadow.BackdropLook.Tip")},
				       {"backdrop_opacity", obs_module_text("Shadow.BackdropLook.Tip")}});
	add_help_line(props, obs_module_text("SceneShadow.Help"));
	return props;
}

void get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "shadow", true);
	obs_data_set_default_int(settings, "shadow_type", 0);
	obs_data_set_default_int(settings, "shadow_fill", 0);
	obs_data_set_default_int(settings, "shadow_color", 0xFF000000);
	obs_data_set_default_double(settings, "shadow_opacity", 60.0);
	obs_data_set_default_double(settings, "offset_x", 12.0);
	obs_data_set_default_double(settings, "offset_y", 12.0);
	obs_data_set_default_double(settings, "softness", 20.0);
	obs_data_set_default_double(settings, "spread", 0.0);
	obs_data_set_default_double(settings, "shadow_boost", 30.0);
	obs_data_set_default_bool(settings, "outline", false);
	obs_data_set_default_double(settings, "outline_width", 6.0);
	obs_data_set_default_int(settings, "outline_color", 0xFFFFFFFF);
	obs_data_set_default_double(settings, "outline_opacity", 100.0);
	outline_glow_defaults(settings);
	obs_data_set_default_bool(settings, "auto_background", true);
	obs_data_set_default_int(settings, "which", (int)Which::AllSources);
	obs_data_set_default_int(settings, "shadow_from", (int)From::Scene);
	obs_data_set_default_int(settings, "ramp_ms", 300);
	obs_data_set_default_int(settings, "ramp_ease_curve", (int)EaseCurve::Cubic);
	obs_data_set_default_int(settings, "ramp_ease_mode", (int)EaseMode::InOut);
}

} // namespace

bool is_scene_shadow(obs_source_t *filter)
{
	const char *id = filter ? obs_source_get_unversioned_id(filter) : nullptr;
	return id && std::strcmp(id, kSceneShadowId) == 0;
}

bool scene_shadow_now(obs_source_t *scene, SceneShadowNow &out)
{
	// (A filter in the list is held by its scene for as long as it is
	// enumerated, so its data can be asked here.)
	struct Find {
		SceneShadowNow *out;
		bool found = false;
	} find{&out};
	obs_source_enum_filters(
		scene,
		[](obs_source_t *, obs_source_t *f, void *param) {
			auto *x = static_cast<Find *>(param);
			if (x->found || !obs_source_enabled(f) || !is_scene_shadow(f))
				return;
			if (auto *self = static_cast<SceneShadow *>(obs_obj_get_data(f)))
				x->found = self->now(*x->out);
		},
		&find);
	return find.found;
}

gs_texture_t *scene_shadow_lay_over(obs_source_t *filter, gs_texture_t *picture, uint32_t cx, uint32_t cy)
{
	if (!is_scene_shadow(filter))
		return nullptr;
	auto *self = static_cast<SceneShadow *>(obs_obj_get_data(filter));
	return self ? self->lay_over(picture, cx, cy) : nullptr;
}

void scene_shadow_default_flags(obs_source_t *scene, ShadowFlags &out)
{
	classify(scene, {}, {}, nullptr, true, out);
}

void register_scene_shadow()
{
	obs_source_info info = {};
	info.id = kSceneShadowId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	// Colour-aware, like every StageKit filter that draws.
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = [](void *) {
		return obs_module_text("SceneShadow.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new SceneShadow(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<SceneShadow *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<SceneShadow *>(data)->update(settings);
	};
	info.get_defaults = get_defaults;
	info.get_properties = get_properties;
	info.video_render = [](void *data, gs_effect_t *) {
		static_cast<SceneShadow *>(data)->render();
	};
	info.video_tick = [](void *data, float seconds) {
		static_cast<SceneShadow *>(data)->tick(seconds);
	};
	info.filter_add = [](void *data, obs_source_t *) {
		static_cast<SceneShadow *>(data)->attached();
	};
	register_timed(&info);
	// A scene whose only filter that draws is a Scene Shadow is still morphed
	// item by item; Morph carries the shadow through the change.
	morph_accept_drawing_filter(is_scene_shadow);
}

} // namespace stagekit
