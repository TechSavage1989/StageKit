/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Scene Blur: a blur for a scene's sources, as a filter on the scene. The
// owner: "The blur filter should be able to blur all sources, live and
// capture sources only, specific sources, and also never blur or ignore
// specific sources."
//
// It blurs in this scene only. A Blur filter on each source would not do:
// a source's filters go with it into every scene. So, as Scene Shadow does,
// the scene draws itself as it always does and the filter works on the
// finished picture:
//
//  - a mask of where a covered source shows: the scene's items drawn again,
//    each by way of a picture of its own, from the top down, a covered one
//    counted and any other held clear, so that a source above a blurred one
//    that is not covered stays sharp;
//  - the picture to blur: the finished scene, or, when a source that stays
//    sharp lies above a covered one, the scene drawn again without such
//    sources (so a sharp camera over a blurred game does not smear into it);
//  - that picture blurred by the Blur filter's own passes (blur-pipeline.*)
//    and laid over the scene only where the mask says.
//
// Nothing is moved or resized and no item, filter or source is added, so
// the same source in another scene stays sharp and Fit to Space, the arrange
// tools and the copy tools have nothing to allow for. Off, the filter is
// disabled (SwitchRamp) and OBS does not draw it: it costs nothing. On, it
// reads nothing back.
//
// Where it differs from a Blur filter on each source: the blur stays inside
// the shape of what shows of a covered source (a per-source blur spreads
// past a see-through edge), and a group or nested scene counts as one
// source.

#include "scene-blur.hpp"

#include "look-paint.hpp"

#include "../api/vendor-api.hpp"
#include "../blur/blur-pipeline.hpp"
#include "../blur/ramp-blur.hpp"
#include "../common/filter-input.hpp"
#include "../common/loading.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-filter-ids.hpp"
#include "../common/scene-util.hpp"
#include "../common/stand-in.hpp"
#include "../common/switch-ramp.hpp"
#include "../common/timing.hpp"
#include "../transition/morph.hpp"
#include "scene-shadow.hpp"

#include <obs-module.h>
#include <graphics/matrix4.h>
#include <graphics/vec4.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace stagekit {

namespace {

// Which sources are blurred. Kept by number: new ones go at the end.
enum class Cover : int {
	All = 0,
	Live = 1,   // live and capture sources
	Chosen = 2, // the sources listed
};

// How often the scene's items are looked at again while it is blurred and
// showing.
constexpr double kLookEvery = 0.25;
// How long after it was last drawn the filter keeps looking, though its
// scene is not showing (a projector, the multiview).
constexpr uint64_t kWantedNs = 1000000000ULL;

bool listed(const std::vector<std::string> &list, const char *name)
{
	return name && std::find(list.begin(), list.end(), name) != list.end();
}

// A video or media file playing: live too, as the owner counts it.
bool is_media(obs_source_t *source)
{
	const char *id = obs_source_get_unversioned_id(source);
	return id && (std::strcmp(id, "ffmpeg_source") == 0 || std::strcmp(id, "vlc_source") == 0) &&
	       obs_source_get_width(source) > 0;
}

// Whether a source counts as live: Blur's rule for "And live captures on
// screen" (a capture with an image, one by way of a Cropped View, one a
// group or nested scene shows), less the ones never to blur, and media.
bool is_live(obs_source_t *source, const std::vector<std::string> &never)
{
	if (is_media(source))
		return true;
	std::vector<obs_source_t *> found;
	live_captures_in(source, found);
	for (obs_source_t *capture : found)
		if (!listed(never, obs_source_get_name(capture)))
			return true;
	return false;
}

// Whether a source is listed under "Never blur these", or is a Cropped View
// of one that is (a camera listed by its crop keeps the view clear too, as
// in the Blur filter).
bool never_blurred(obs_source_t *source, const std::vector<std::string> &never)
{
	if (listed(never, obs_source_get_name(source)))
		return true;
	const char *id = obs_source_get_unversioned_id(source);
	if (!id || std::strcmp(id, "stagekit_cropped_view") != 0)
		return false;
	obs_data_t *view = obs_source_get_settings(source);
	const bool shows_listed = listed(never, obs_data_get_string(view, "source"));
	obs_data_release(view);
	return shows_listed;
}

using Covered = std::unordered_map<int64_t, bool>;

std::vector<obs_sceneitem_t *> held_items(obs_scene_t *scene)
{
	std::vector<obs_sceneitem_t *> items;
	if (!scene)
		return items;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			obs_sceneitem_addref(item);
			static_cast<std::vector<obs_sceneitem_t *> *>(param)->push_back(item);
			return true;
		},
		&items);
	return items;
}

void release_items(std::vector<obs_sceneitem_t *> &items)
{
	for (obs_sceneitem_t *item : items)
		obs_sceneitem_release(item);
	items.clear();
}

// Which of the scene's items are blurred, by id; and whether any of them is
// showing.
void classify(obs_source_t *scene_source, Cover cover, const std::vector<std::string> &chosen,
	      const std::vector<std::string> &never, Covered &out, bool &any)
{
	out.clear();
	any = false;
	std::vector<obs_sceneitem_t *> items = held_items(obs_scene_from_source(scene_source));
	std::vector<std::pair<int64_t, int64_t>> twins;
	std::vector<int64_t> shown;
	for (obs_sceneitem_t *item : items) {
		const int64_t id = obs_sceneitem_get_id(item);
		if (const int64_t owner = stand_in_for_item(item)) {
			twins.emplace_back(id, owner);
			if (obs_sceneitem_visible(item))
				shown.push_back(id);
			continue;
		}
		obs_source_t *source = obs_sceneitem_get_source(item);
		bool covered;
		if (is_morph_layer(item))
			// (Two items changing into each other: blurred with all sources.)
			covered = cover == Cover::All;
		else if (!source || never_blurred(source, never))
			covered = false;
		else if (cover == Cover::All)
			covered = true;
		else if (cover == Cover::Live)
			covered = is_live(source, never);
		else
			covered = listed(chosen, obs_source_get_name(source));
		out[id] = covered;
		if (covered && obs_sceneitem_visible(item))
			any = true;
	}
	// A Standby stand-in is blurred as the source it stands in for.
	for (const auto &[twin, owner] : twins) {
		const auto found = out.find(owner);
		const bool covered = found != out.end() && found->second;
		out[twin] = covered;
		if (covered && std::find(shown.begin(), shown.end(), twin) != shown.end())
			any = true;
	}
	release_items(items);
}

std::string replace_all(std::string text, const std::string &from, const std::string &to)
{
	for (size_t pos = text.find(from); pos != std::string::npos; pos = text.find(from, pos + to.size()))
		text.replace(pos, from.size(), to);
	return text;
}

} // namespace

class SceneBlur {
public:
	SceneBlur(obs_data_t *settings, obs_source_t *source);
	~SceneBlur();

	obs_source_t *filter() const { return source_; }
	void update(obs_data_t *settings);
	void tick(float seconds);
	void render();
	void attach(obs_source_t *parent);
	void detach();

	bool set_active(bool active) { return ramp_.set_active(active); }
	bool active() const { return ramp_.active(); }
	double level() const { return ramp_.level(); }
	int draws() const { return draws_.load(); }
	std::string type_name() const
	{
		std::lock_guard lock(mutex_);
		return blur_type_name(look_.type, look_.pixel_shape);
	}
	// Whether it blurs nothing at all now: Morph may then still take the
	// scene apart.
	bool blurs_nothing() const { return ramp_.level() <= 0.0005 || !covers_any_.load(); }

private:
	void look_again(obs_source_t *parent);
	void register_hotkeys(obs_source_t *parent);
	void unregister_hotkeys();
	void on_rename();
	static void hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);
	static void rename_cb(void *data, calldata_t *cd);
	void draw_out(uint32_t cx, uint32_t cy);

	obs_source_t *source_;
	SwitchRamp ramp_;

	mutable std::mutex mutex_;
	BlurLook look_;
	RampStyle style_ = RampStyle::Grow;
	Cover cover_ = Cover::All;
	std::vector<std::string> chosen_, never_;
	Covered covered_;
	bool classified_ = false;
	bool blocked_ = false; // a filter that draws comes before it
	bool wide_ = false;    // the canvas is in a wide colour space
	bool on_group_ = false;
	std::atomic<bool> covers_any_{false};

	// On the tick only.
	double countdown_ = 0.0;
	bool looked_ = false; // since it was last switched on
	bool was_showing_ = false;
	std::string told_blocked_;
	bool said_valid_ = false;
	bool said_ = false;
	std::atomic<uint64_t> wanted_ns_{0};
	std::atomic<bool> settings_changed_{true};

	// Hotkeys, on the scene.
	obs_source_t *parent_ = nullptr;
	obs_hotkey_id hk_on_ = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hk_off_ = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hk_toggle_ = OBS_INVALID_HOTKEY_ID;

	// On the drawing thread only.
	std::unique_ptr<BlurPipeline> blur_;
	std::unique_ptr<LookPaint> paint_;
	gs_texrender_t *tr_scene_ = nullptr;
	gs_texrender_t *tr_under_ = nullptr;
	gs_texrender_t *tr_mask_ = nullptr;
	gs_texrender_t *tr_out_ = nullptr;
	uint64_t last_frame_ = 0;
	uint32_t last_cx_ = 0, last_cy_ = 0;
	bool last_linear_ = false;
	bool have_out_ = false;
	std::atomic<int> draws_{0};
};

SceneBlur::SceneBlur(obs_data_t *settings, obs_source_t *source) : source_(source), ramp_(source)
{
	blur_ = std::make_unique<BlurPipeline>();
	paint_ = std::make_unique<LookPaint>();
	obs_enter_graphics();
	tr_scene_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	tr_under_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	tr_mask_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	tr_out_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();
	signal_handler_connect(obs_source_get_signal_handler(source_), "rename", rename_cb, this);
	proc_handler_t *ph = obs_source_get_proc_handler(source);
	// For the tests: how many times it has drawn the blur (once a frame at
	// most), and its level.
	proc_handler_add(
		ph, "void draws(out int count, out float level)",
		[](void *data, calldata_t *cd) {
			auto *self = static_cast<SceneBlur *>(data);
			calldata_set_int(cd, "count", self->draws());
			calldata_set_float(cd, "level", self->level());
		},
		this);
	update(settings);
}

SceneBlur::~SceneBlur()
{
	signal_handler_disconnect(obs_source_get_signal_handler(source_), "rename", rename_cb, this);
	unregister_hotkeys();
	blur_.reset();
	paint_.reset();
	obs_enter_graphics();
	gs_texrender_destroy(tr_scene_);
	gs_texrender_destroy(tr_under_);
	gs_texrender_destroy(tr_mask_);
	gs_texrender_destroy(tr_out_);
	obs_leave_graphics();
}

void SceneBlur::update(obs_data_t *settings)
{
	const BlurLook look = read_blur_look(settings);
	const BlurRamp ramp = read_blur_ramp(settings);
	ramp_.configure(ramp.in_s, ramp.out_s, ramp.curve, ramp.mode);
	std::lock_guard lock(mutex_);
	look_ = look;
	style_ = ramp.style;
	cover_ = static_cast<Cover>(std::clamp<long long>(obs_data_get_int(settings, "cover"), 0, 2));
	chosen_ = pick_list_read(settings, "chosen", false);
	never_ = pick_list_read(settings, "never", false);
	settings_changed_ = true;
}

// --------------------------------------------------------------- hotkeys

void SceneBlur::hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	auto *self = static_cast<SceneBlur *>(data);
	if (id == self->hk_on_)
		self->set_active(true);
	else if (id == self->hk_off_)
		self->set_active(false);
	else if (id == self->hk_toggle_)
		self->set_active(!self->active());
}

void SceneBlur::register_hotkeys(obs_source_t *parent)
{
	unregister_hotkeys();
	if (!parent)
		return;
	parent_ = parent;
	const std::string name = obs_source_get_name(source_) ? obs_source_get_name(source_) : "";
	auto reg = [&](const char *key, const char *text_key) {
		const std::string id = std::string("stagekit.sceneblur.") + key + "." + name;
		const std::string desc = replace_all(obs_module_text(text_key), "%1", name);
		return obs_hotkey_register_source(parent, id.c_str(), desc.c_str(), hotkey_cb, this);
	};
	hk_toggle_ = reg("toggle", "Hotkey.SceneBlurToggle");
	hk_on_ = reg("on", "Hotkey.SceneBlurOn");
	hk_off_ = reg("off", "Hotkey.SceneBlurOff");
}

void SceneBlur::unregister_hotkeys()
{
	for (obs_hotkey_id *id : {&hk_on_, &hk_off_, &hk_toggle_}) {
		if (*id != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(*id);
		*id = OBS_INVALID_HOTKEY_ID;
	}
	parent_ = nullptr;
}

void SceneBlur::attach(obs_source_t *parent)
{
	ramp_.attached();
	register_hotkeys(parent);
}

void SceneBlur::detach()
{
	unregister_hotkeys();
}

void SceneBlur::rename_cb(void *data, calldata_t *)
{
	static_cast<SceneBlur *>(data)->on_rename();
}

// The hotkeys' names hold the filter's name: made again, keeping their keys.
void SceneBlur::on_rename()
{
	if (!parent_)
		return;
	obs_data_array_t *on = obs_hotkey_save(hk_on_);
	obs_data_array_t *off = obs_hotkey_save(hk_off_);
	obs_data_array_t *toggle = obs_hotkey_save(hk_toggle_);
	register_hotkeys(parent_);
	obs_hotkey_load(hk_on_, on);
	obs_hotkey_load(hk_off_, off);
	obs_hotkey_load(hk_toggle_, toggle);
	obs_data_array_release(on);
	obs_data_array_release(off);
	obs_data_array_release(toggle);
}

// ------------------------------------------------------------------ tick

// Which items are blurred, and whether the filter can draw at all where it is.
void SceneBlur::look_again(obs_source_t *parent)
{
	// A filter that draws before this one: the scene's picture is no longer
	// where its items are. A Scene Shadow is the one exception: it moves
	// nothing, so its shadows are blurred with what they fall on.
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
			if (!x->reached && x->in_the_way->empty() && obs_source_enabled(f) && !morph_draws_nothing(f) &&
			    !is_scene_shadow(f))
				*x->in_the_way = obs_source_get_name(f) ? obs_source_get_name(f) : "?";
		},
		&find);
	if (!in_the_way.empty() && in_the_way != told_blocked_)
		obs_log(LOG_INFO, "Scene Blur '%s': the filter '%s' draws before it, so it draws nothing",
			obs_source_get_name(source_), in_the_way.c_str());
	told_blocked_ = in_the_way;
	obs_video_info ovi = {};
	const bool wide = obs_get_video_info(&ovi) &&
			  (ovi.colorspace == VIDEO_CS_2100_PQ || ovi.colorspace == VIDEO_CS_2100_HLG);
	Cover cover;
	std::vector<std::string> chosen, never;
	{
		std::lock_guard lock(mutex_);
		cover = cover_;
		chosen = chosen_;
		never = never_;
	}
	Covered covered;
	bool any = false;
	classify(parent, cover, chosen, never, covered, any);
	covers_any_ = any;
	std::lock_guard lock(mutex_);
	covered_.swap(covered);
	classified_ = true;
	blocked_ = !in_the_way.empty();
	wide_ = wide;
}

void SceneBlur::tick(float seconds)
{
	ramp_.tick(seconds);
	blur_->frame_passed(seconds);
	// A key showing this filter follows it: said when it changes, not at
	// the first look (the state restored at load).
	const bool on = ramp_.active();
	if (!said_valid_) {
		said_valid_ = true;
		said_ = on;
	} else if (on != said_) {
		said_ = on;
		vendor_emit_scene_blur_changed(source_, on);
	}
	obs_source_t *parent = obs_filter_get_parent(source_);
	if (!parent)
		return;
	if (obs_source_is_group(parent)) {
		std::lock_guard lock(mutex_);
		on_group_ = true;
		return;
	}
	// Nothing is looked at while it is off, or while OBS loads a
	// collection (only some scenes have their items yet).
	if (ramp_.level() <= 0.0 && !on) {
		looked_ = was_showing_ = false;
		return;
	}
	if (!scenes_loaded())
		return;
	const uint64_t now = os_gettime_ns();
	// (Taken from the clock on the drawing thread, maybe after `now`: never
	// subtracted the wrong way round.)
	const uint64_t wanted = wanted_ns_.load();
	const bool showing = obs_source_showing(parent) || (wanted && (wanted >= now || now - wanted < kWantedNs));
	countdown_ -= seconds;
	// At once when it is switched on, comes into view or its settings
	// change; then a few times a second while it is seen.
	const bool due = !looked_ || (showing && !was_showing_) || settings_changed_.exchange(false) ||
			 (showing && countdown_ <= 0.0);
	was_showing_ = showing;
	if (!due)
		return;
	looked_ = true;
	countdown_ = kLookEvery;
	look_again(parent);
}

// ------------------------------------------------------------------ render

void SceneBlur::draw_out(uint32_t cx, uint32_t cy)
{
	gs_texture_t *texture = gs_texrender_get_texture(tr_out_);
	if (!texture)
		return;
	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(def, "image");
	const bool linear = gs_get_linear_srgb();
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(linear);
	if (linear)
		gs_effect_set_texture_srgb(image, texture);
	else
		gs_effect_set_texture(image, texture);
	while (gs_effect_loop(def, "Draw"))
		gs_draw_sprite(texture, 0, cx, cy);
	gs_enable_framebuffer_srgb(previous);
}

void SceneBlur::render()
{
	obs_source_t *target = obs_filter_get_target(source_);
	obs_source_t *parent = obs_filter_get_parent(source_);
	const uint32_t cx = target ? obs_source_get_base_width(target) : 0;
	const uint32_t cy = target ? obs_source_get_base_height(target) : 0;
	wanted_ns_ = os_gettime_ns();
	BlurLook look;
	RampStyle style;
	bool usable;
	Covered covered;
	{
		std::lock_guard lock(mutex_);
		look = look_;
		style = style_;
		usable = classified_ && !blocked_ && !wide_ && !on_group_;
		if (usable)
			covered = covered_;
	}
	double strength = 0.0, mix = 0.0;
	const double level = ramp_.level();
	blur_at_level(look, style, level, strength, mix);
	if (!usable || !parent || !paint_->ready() || !blur_->ready() || !blur_->has_looks() || !scenes_loaded() ||
	    level <= 0.0005 || strength <= 0.01 || mix <= 0.0005 || !cx || !cy) {
		blur_->forget_trail(); // a trail starts afresh when the blur comes back
		obs_source_skip_video_filter(source_);
		have_out_ = false;
		return;
	}
	// Once a frame. The multiview, a projector and Studio Mode's preview
	// draw a scene again in the same frame; those draws take the picture
	// made the first time (and a trail moves on once a frame).
	const uint64_t frame = obs_get_video_frame_time();
	const bool linear = gs_get_linear_srgb();
	if (have_out_ && frame == last_frame_ && cx == last_cx_ && cy == last_cy_ && linear == last_linear_) {
		draw_out(cx, cy);
		return;
	}
	have_out_ = false;

	// The scene's items, from the back, each held for this call only.
	std::vector<obs_sceneitem_t *> items = held_items(obs_scene_from_source(parent));
	auto covered_of = [&](obs_sceneitem_t *item) {
		// (An item added since the last look stays sharp until it is looked at.)
		const auto found = covered.find(obs_sceneitem_get_id(item));
		return found != covered.end() && found->second;
	};
	// The lowest covered item; nothing under it has any say in the mask.
	size_t lowest = items.size();
	for (size_t i = 0; i < items.size(); i++)
		if (covered_of(items[i])) {
			lowest = i;
			break;
		}
	if (lowest == items.size()) {
		release_items(items);
		obs_source_skip_video_filter(source_);
		return;
	}
	// A sharp source above a covered one: the picture to blur is drawn
	// without it.
	bool sharp_above = false;
	for (size_t i = lowest + 1; i < items.size(); i++)
		sharp_above = sharp_above || !covered_of(items[i]);

	if (!draw_filter_input(source_, tr_scene_, cx, cy)) {
		release_items(items);
		obs_source_skip_video_filter(source_);
		return;
	}
	gs_texture_t *scene = gs_texrender_get_texture(tr_scene_);

	paint_->begin_frame();
	// The mask: from the top down, each item's coverage laid under what is
	// above it, counted where it is blurred.
	gs_texture_t *mask = nullptr;
	if (paint_->begin_canvas(tr_mask_, cx, cy)) {
		for (size_t i = items.size(); i-- > lowest;) {
			matrix4 m;
			uint32_t iw = 0, ih = 0;
			gs_texture_t *piece = paint_->item_picture(items[i], m, iw, ih);
			if (!piece)
				continue;
			gs_blend_function(GS_BLEND_INVDSTALPHA, GS_BLEND_ONE);
			gs_matrix_push();
			gs_matrix_mul(&m);
			blur_->draw_mask_piece(piece, iw, ih, covered_of(items[i]));
			gs_matrix_pop();
		}
		paint_->end_canvas(tr_mask_);
		mask = gs_texrender_get_texture(tr_mask_);
	}
	// What is blurred: the scene, or the scene without the sharp sources
	// above the blurred ones.
	gs_texture_t *under = scene;
	if (sharp_above && paint_->begin_canvas(tr_under_, cx, cy)) {
		for (size_t i = 0; i < items.size(); i++)
			if (i < lowest || covered_of(items[i]))
				paint_->draw_item(items[i]);
		paint_->end_canvas(tr_under_);
		under = gs_texrender_get_texture(tr_under_);
	}
	release_items(items);
	if (!scene || !mask || !under) {
		obs_source_skip_video_filter(source_);
		return;
	}
	const BlurPipeline::Result blurred = blur_->run(look, under, cx, cy, strength, mix);
	bool drawn = false;
	gs_texrender_reset(tr_out_);
	if (gs_texrender_begin_with_color_space(tr_out_, cx, cy, GS_CS_SRGB)) {
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		drawn = blur_->composite_masked(scene, blurred, mask, cx, cy, mix);
		gs_blend_state_pop();
		gs_texrender_end(tr_out_);
	}
	if (!drawn) {
		obs_source_skip_video_filter(source_);
		return;
	}
	draw_out(cx, cy);
	draws_++;
	have_out_ = true;
	last_frame_ = frame;
	last_cx_ = cx;
	last_cy_ = cy;
	last_linear_ = linear;
}

// ------------------------------------------------------------ properties

namespace {

bool cover_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const auto cover = static_cast<Cover>(obs_data_get_int(settings, "cover"));
	obs_property_set_visible(obs_properties_get(props, "chosen|alist"), cover == Cover::Chosen);
	return true;
}

obs_properties_t *get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_source_t *filter = data ? static_cast<SceneBlur *>(data)->filter() : nullptr;
	obs_source_t *parent = filter ? obs_filter_get_parent(filter) : nullptr;
	if (parent && obs_source_is_group(parent)) {
		obs_property_t *text =
			obs_properties_add_text(props, "on_group", obs_module_text("SceneBlur.OnGroup"), OBS_TEXT_INFO);
		obs_property_text_set_info_type(text, OBS_TEXT_INFO_WARNING);
	}

	obs_property_t *cover = obs_properties_add_list(props, "cover", obs_module_text("SceneBlur.Cover"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(cover, obs_module_text("SceneBlur.Cover.All"), (int)Cover::All);
	obs_property_list_add_int(cover, obs_module_text("SceneBlur.Cover.Live"), (int)Cover::Live);
	obs_property_list_add_int(cover, obs_module_text("SceneBlur.Cover.Chosen"), (int)Cover::Chosen);
	obs_property_set_long_description(cover, obs_module_text("SceneBlur.Cover.Tip"));
	obs_property_set_modified_callback(cover, cover_modified);

	// The scene's own sources (a group as one), for the two lists.
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
	PickList chosen;
	chosen.setting = "chosen";
	chosen.title = "SceneBlur.Chosen";
	chosen.add = "SceneBlur.AddSource";
	chosen.tip = "SceneBlur.Chosen.Tip";
	chosen.choices = sources;
	pick_list_add(props, filter, chosen);
	PickList never;
	never.setting = "never";
	never.title = "SceneBlur.Never";
	never.add = "SceneBlur.AddSource";
	never.tip = "SceneBlur.Never.Tip";
	never.choices = sources;
	pick_list_add(props, filter, never);

	add_blur_look_properties(props);
	add_blur_ramp_properties(props);
	obs_properties_add_button2(
		props, "toggle_now", obs_module_text("SceneBlur.ToggleNow"),
		[](obs_properties_t *, obs_property_t *, void *d) {
			auto *self = static_cast<SceneBlur *>(d);
			self->set_active(!self->active());
			return false;
		},
		data);
	return props;
}

void get_defaults(obs_data_t *settings)
{
	blur_look_defaults(settings);
	obs_data_set_default_int(settings, "cover", (int)Cover::All);
}

// Every Scene Blur that `want` takes, held, with its scene's name.
struct Found {
	obs_source_t *filter; // a reference
	std::string scene;
};

std::vector<Found> find_blurs(const std::string &scene, const std::string &name)
{
	struct Search {
		const std::string *scene, *name;
		std::vector<Found> out;
	} search{&scene, &name, {}};
	obs_enum_scenes(
		[](void *param, obs_source_t *source) {
			auto *s = static_cast<Search *>(param);
			// (OBS lists each group as a scene too.)
			if (obs_source_is_group(source))
				return true;
			const char *scene_name = obs_source_get_name(source);
			if (!scene_name || (!s->scene->empty() && *s->scene != scene_name))
				return true;
			struct Inner {
				Search *s;
				const char *scene_name;
			} inner{s, scene_name};
			obs_source_enum_filters(
				source,
				[](obs_source_t *, obs_source_t *f, void *param) {
					auto *in = static_cast<Inner *>(param);
					const char *filter_name = obs_source_get_name(f);
					if (!is_scene_blur(f) || !filter_name ||
					    (!in->s->name->empty() && *in->s->name != filter_name))
						return;
					if (obs_source_t *ref = obs_source_get_ref(f))
						in->s->out.push_back({ref, in->scene_name});
				},
				&inner);
			return true;
		},
		&search);
	return std::move(search.out);
}

SceneBlurState state_of(const Found &found)
{
	SceneBlurState state;
	state.scene = found.scene;
	state.filter = obs_source_get_name(found.filter) ? obs_source_get_name(found.filter) : "";
	state.enabled = obs_source_enabled(found.filter);
	if (auto *self = static_cast<SceneBlur *>(obs_obj_get_data(found.filter))) {
		state.active = self->active();
		state.type = self->type_name();
	}
	return state;
}

bool accepts_for_morph(obs_source_t *filter)
{
	if (!is_scene_blur(filter))
		return false;
	auto *self = static_cast<SceneBlur *>(obs_obj_get_data(filter));
	return self && self->blurs_nothing();
}

} // namespace

bool is_scene_blur(obs_source_t *filter)
{
	const char *id = filter ? obs_source_get_unversioned_id(filter) : nullptr;
	return id && std::strcmp(id, kSceneBlurId) == 0;
}

std::vector<SceneBlurState> scene_blur_list(const std::string &scene, const std::string &filter)
{
	std::vector<SceneBlurState> out;
	for (Found &found : find_blurs(scene, filter)) {
		out.push_back(state_of(found));
		obs_source_release(found.filter);
	}
	return out;
}

bool scene_blur_press(const std::string &scene, const std::string &filter, const std::string &how,
		      std::vector<SceneBlurState> &out, bool &active, std::string &why)
{
	out.clear();
	active = false;
	if (filter.empty()) {
		why = "filter is required";
		return false;
	}
	if (!how.empty() && how != "on" && how != "off" && how != "toggle") {
		why = "how must be on, off or toggle";
		return false;
	}
	std::vector<Found> found = find_blurs(scene, filter);
	if (found.empty()) {
		why = scene.empty() ? "No scene has a Scene Blur with that name"
				    : "That scene has no Scene Blur with that name";
		return false;
	}
	bool any_on = false;
	for (const Found &f : found)
		if (auto *self = static_cast<SceneBlur *>(obs_obj_get_data(f.filter)))
			any_on = any_on || self->active();
	if (how.empty()) {
		active = any_on;
	} else {
		active = how == "on" || (how == "toggle" && !any_on);
		for (const Found &f : found)
			if (auto *self = static_cast<SceneBlur *>(obs_obj_get_data(f.filter)))
				self->set_active(active);
	}
	for (Found &f : found) {
		out.push_back(state_of(f));
		obs_source_release(f.filter);
	}
	return true;
}

void register_scene_blur()
{
	obs_source_info info = {};
	info.id = kSceneBlurId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	// Colour-aware, like every StageKit filter that draws.
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = [](void *) {
		return obs_module_text("SceneBlur.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new SceneBlur(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<SceneBlur *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<SceneBlur *>(data)->update(settings);
	};
	info.get_defaults = get_defaults;
	info.get_properties = get_properties;
	info.video_render = [](void *data, gs_effect_t *) {
		static_cast<SceneBlur *>(data)->render();
	};
	info.video_tick = [](void *data, float seconds) {
		static_cast<SceneBlur *>(data)->tick(seconds);
	};
	info.filter_add = [](void *data, obs_source_t *parent) {
		static_cast<SceneBlur *>(data)->attach(parent);
	};
	info.filter_remove = [](void *data, obs_source_t *) {
		static_cast<SceneBlur *>(data)->detach();
	};
	register_timed(&info);
	// A scene whose Scene Blur blurs nothing at the moment is still morphed
	// item by item. While it blurs, the change is a cross-fade, which keeps
	// the blur on screen all the way (see AGENTS.md, Scene Blur).
	morph_accept_drawing_filter(accepts_for_morph);
}

} // namespace stagekit
