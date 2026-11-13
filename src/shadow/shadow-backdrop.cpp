/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Shadow & Backdrop: a filter that gives any source a drop shadow, an
// optional panel behind it and rounded corners.
//
// A filter normally cannot draw outside its source, which is why shadows
// usually mean wrapping the source in an oversized scene. This filter grows
// the source by exactly the room the shadow and panel need, so no wrapper
// scene is required.

#include "shadow-backdrop.hpp"

#include "../common/kawase-blur.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-util.hpp"
#include "../common/shown-scale.hpp"

#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <plugin-support.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <cstring>
#include <string>
#include <vector>

namespace stagekit {

namespace {

// Saved in settings: append only.
enum class ShadowType { Drop = 0, Glow = 1 }; // to one side, or all round
enum class Fill { Color = 0, Picture = 1 };   // a chosen colour, or the picture's own colours

enum class Edge { Stretch = 0, Mirror = 1 }; // how the picture carries on past its edge

struct Settings {
	bool shadow = true;
	uint32_t shadow_color = 0xFF000000;
	double shadow_opacity = 0.6;
	double offset_x = 12.0;
	double offset_y = 12.0;
	double softness = 20.0;
	double spread = 0.0;
	bool backdrop = false;
	uint32_t backdrop_color = 0xFF3A3A3A;
	double backdrop_opacity = 1.0;
	double backdrop_pad = 12.0;
	double radius = 0.0;
	bool hide_source = false;
	ShadowType shadow_type = ShadowType::Drop;
	Fill shadow_fill = Fill::Color;
	double shadow_boost = 0.3;
	bool outline = false;
	double outline_width = 6.0;
	uint32_t outline_color = 0xFFFFFFFF;
	double outline_opacity = 1.0;
	Fill outline_fill = Fill::Color;
	Edge outline_edge = Edge::Stretch;
	double outline_blur = 20.0;
	// Sizes are meant as they look on screen: an item drawn at half size
	// gets twice the numbers, so 20 px of shadow is 20 px in the picture.
	bool screen_px = true;
};

struct Padding {
	uint32_t left = 0, top = 0, right = 0, bottom = 0;
};

// Room needed around the source for the panel, the outline and the shadow.
Padding padding_for(const Settings &s)
{
	const double panel = s.backdrop ? s.backdrop_pad : 0.0;
	const double line = s.outline ? std::ceil(s.outline_width) + 1.0 : 0.0;
	const double soft = s.shadow && s.softness > 0.0 ? std::ceil(s.softness * 1.5) + 2.0 : 0.0;
	const double ox = s.shadow ? s.offset_x : 0.0;
	const double oy = s.shadow ? s.offset_y : 0.0;
	const double shadow = s.shadow ? 1.0 : 0.0;
	const double around = panel + line;
	Padding p;
	p.left = (uint32_t)std::ceil(around + shadow * (std::max(0.0, -ox) + soft));
	p.right = (uint32_t)std::ceil(around + shadow * (std::max(0.0, ox) + soft));
	p.top = (uint32_t)std::ceil(around + shadow * (std::max(0.0, -oy) + soft));
	p.bottom = (uint32_t)std::ceil(around + shadow * (std::max(0.0, oy) + soft));
	return p;
}

// --- Keeping the picture where it is ---------------------------------------
//
// The shadow needs room around the source, so the source gets bigger and the
// picture sits further in. Left alone, that moves or shrinks the picture in
// every scene whenever the softness or offset changes. Instead, each scene
// item showing the source is adjusted by exactly the room added, so the
// picture itself stays the size and in the place it was.

// One direction (across or down) of a scene item.
struct ItemAxis {
	double pos = 0, anchor = 0;             // position, and where on the item it refers to (0, 0.5 or 1)
	double scale = 1;                       // used when the item has no bounding box
	double bounds = 0, bounds_anchor = 0.5; // the bounding box, and where the source sits in it
};

double anchor_of(uint32_t alignment, uint32_t low, uint32_t high)
{
	return (alignment & low) ? 0.0 : (alignment & high) ? 1.0 : 0.5;
}

// Adjusts one scene item for the room around the source changing from
// `before` to `after`. inner_w and inner_h are the source without any room.
void keep_picture_in_place(obs_sceneitem_t *item, double inner_w, double inner_h, const Padding &before,
			   const Padding &after)
{
	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	const double w0 = std::max(1.0, inner_w + before.left + before.right - crop.left - crop.right);
	const double h0 = std::max(1.0, inner_h + before.top + before.bottom - crop.top - crop.bottom);
	const double w1 = std::max(1.0, inner_w + after.left + after.right - crop.left - crop.right);
	const double h1 = std::max(1.0, inner_h + after.top + after.bottom - crop.top - crop.bottom);

	vec2 pos, scale, bounds;
	obs_sceneitem_get_pos(item, &pos);
	obs_sceneitem_get_scale(item, &scale);
	obs_sceneitem_get_bounds(item, &bounds);
	const obs_bounds_type type = obs_sceneitem_get_bounds_type(item);
	const uint32_t align = obs_sceneitem_get_alignment(item);
	const uint32_t bounds_align = obs_sceneitem_get_bounds_alignment(item);
	const bool boxed = type != OBS_BOUNDS_NONE && bounds.x > 0.0f && bounds.y > 0.0f;

	ItemAxis x{pos.x, anchor_of(align, OBS_ALIGN_LEFT, OBS_ALIGN_RIGHT), scale.x, bounds.x,
		   anchor_of(bounds_align, OBS_ALIGN_LEFT, OBS_ALIGN_RIGHT)};
	ItemAxis y{pos.y, anchor_of(align, OBS_ALIGN_TOP, OBS_ALIGN_BOTTOM), scale.y, bounds.y,
		   anchor_of(bounds_align, OBS_ALIGN_TOP, OBS_ALIGN_BOTTOM)};

	// Where the picture's corner is now, measured from the item's position
	// along the item's own (unrotated) directions, and its scale.
	double sx0 = x.scale, sy0 = y.scale;
	if (boxed)
		bounds_scale(type, x.bounds, y.bounds, w0, h0, sx0, sy0);
	auto corner = [boxed](const ItemAxis &a, double size, double s, double room) {
		const double outer = boxed ? -a.anchor * a.bounds + a.bounds_anchor * (a.bounds - size * s)
					   : -a.anchor * size * s;
		return outer + room * s;
	};
	const double pic_x = corner(x, w0, sx0, (double)before.left - crop.left);
	const double pic_y = corner(y, h0, sy0, (double)before.top - crop.top);

	// Grow (or shrink) the bounding box by the room added, at the scale the
	// picture has, so that scale does not change.
	double sx1 = sx0, sy1 = sy0;
	if (boxed) {
		x.bounds += (w1 - w0) * sx0;
		y.bounds += (h1 - h0) * sy0;
		bounds_scale(type, x.bounds, y.bounds, w1, h1, sx1, sy1);
		vec2_set(&bounds, (float)x.bounds, (float)y.bounds);
		obs_sceneitem_set_bounds(item, &bounds);
	}
	const double new_x = corner(x, w1, sx1, (double)after.left - crop.left);
	const double new_y = corner(y, h1, sy1, (double)after.top - crop.top);

	// Shift the item by however far the picture's corner would have moved.
	double dx = pic_x - new_x, dy = pic_y - new_y;
	const double turn = obs_sceneitem_get_rot(item) * 3.14159265358979323846 / 180.0;
	if (turn != 0.0) {
		const double rx = dx * std::cos(turn) - dy * std::sin(turn);
		dy = dx * std::sin(turn) + dy * std::cos(turn);
		dx = rx;
	}
	pos.x += (float)dx;
	pos.y += (float)dy;
	obs_sceneitem_set_pos(item, &pos);
}

// Every scene item, in every scene and group, that shows a source. Each is
// held with a reference, for the caller to release: the items are used after
// the scenes are let go of, when OBS's window could be deleting one.
std::vector<obs_sceneitem_t *> items_showing(obs_source_t *wanted)
{
	struct Find {
		obs_source_t *wanted;
		std::vector<obs_sceneitem_t *> *out;
	};
	std::vector<obs_sceneitem_t *> out;
	Find find{wanted, &out};
	static const auto visit = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		auto *f = static_cast<Find *>(param);
		if (obs_sceneitem_get_source(item) == f->wanted) {
			obs_sceneitem_addref(item);
			f->out->push_back(item);
		}
		return true;
	};
	obs_enum_scenes(
		[](void *param, obs_source_t *scene_source) {
			if (obs_scene_t *scene = obs_scene_from_source(scene_source)) {
				obs_scene_enum_items(
					scene,
					[](obs_scene_t *s, obs_sceneitem_t *item, void *p) {
						visit(s, item, p);
						if (obs_sceneitem_is_group(item))
							obs_sceneitem_group_enum_items(item, visit, p);
						return true;
					},
					param);
			}
			return true;
		},
		&find);
	return out;
}

// --- Scenes it is left off in ----------------------------------------------
//
// One source can sit in several scenes and want its shadow in only some. A
// filter cannot tell which scene is drawing it, so the rule goes by the scene
// on screen: when every scene there that holds the source is ticked, the
// filter stands down and gives its room back.

bool holds_directly(obs_scene_t *scene, obs_source_t *wanted)
{
	struct Find {
		obs_source_t *wanted;
		bool found;
	} find{wanted, false};
	static const auto visit = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		auto *f = static_cast<Find *>(param);
		if (obs_sceneitem_get_source(item) == f->wanted)
			f->found = true;
		else if (obs_sceneitem_is_group(item))
			obs_sceneitem_group_enum_items(
				item,
				[](obs_scene_t *, obs_sceneitem_t *inner, void *p) {
					auto *g = static_cast<Find *>(p);
					if (obs_sceneitem_get_source(inner) == g->wanted)
						g->found = true;
					return !g->found;
				},
				param);
		return !f->found;
	};
	obs_scene_enum_items(scene, visit, &find);
	return find.found;
}

// The scenes holding the source that are showing now: the scene on screen
// and the visible scenes nested in it. Each is a new reference.
struct Showing {
	obs_source_t *wanted;
	std::vector<obs_source_t *> *holders;
	int depth;
};
void showing_holders(obs_source_t *scene_source, Showing &look)
{
	obs_scene_t *scene = obs_scene_from_source(scene_source);
	if (!scene || look.depth > 8)
		return;
	if (holds_directly(scene, look.wanted) &&
	    std::find(look.holders->begin(), look.holders->end(), scene_source) == look.holders->end())
		look.holders->push_back(obs_source_get_ref(scene_source));
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *l = static_cast<Showing *>(param);
			obs_source_t *source = obs_sceneitem_get_source(item);
			if (obs_sceneitem_visible(item) && source && obs_scene_from_source(source) &&
			    !obs_source_is_group(source)) {
				Showing deeper{l->wanted, l->holders, l->depth + 1};
				showing_holders(source, deeper);
			}
			return true;
		},
		&look);
}

class ShadowBackdrop {
public:
	obs_source_t *filter() const { return source_; }
	ShadowBackdrop(obs_data_t *settings, obs_source_t *source);
	~ShadowBackdrop();

	void update(obs_data_t *settings);
	void tick();
	void leave(obs_source_t *parent);
	void render();
	uint32_t width();
	uint32_t height();

	// The room it takes on each side right now, in the source's pixels.
	Padding padding_now() { return padding_for(settings()); }
	bool in_screen_pixels()
	{
		std::lock_guard lock(mutex_);
		return settings_.screen_px;
	}

private:
	Settings settings();
	bool capture_input(uint32_t w, uint32_t h);

	void settle_items(obs_source_t *parent, const Padding &wanted);

	obs_source_t *source_;
	std::mutex mutex_;
	Settings settings_;
	// The room around the source that scene items have been adjusted for.
	// Saved with the filter, so loading a collection adjusts nothing twice.
	Padding adjusted_;
	bool adjusted_loaded_ = false;
	bool owner_checked_ = false;
	bool keep_place_ = true;
	// How big the source is drawn on screen, for sizes given in screen pixels.
	double shown_sx_ = 1.0, shown_sy_ = 1.0;
	// Standing down because of the scene on screen.
	bool off_here_ = false;
	int scale_countdown_ = 0;

	gs_effect_t *effect_ = nullptr;
	gs_eparam_t *p_image_ = nullptr;
	gs_eparam_t *p_shadow_ = nullptr;
	gs_eparam_t *p_out_size_ = nullptr;
	gs_eparam_t *p_src_size_ = nullptr;
	gs_eparam_t *p_pad_ = nullptr;
	gs_eparam_t *p_radius_ = nullptr;
	gs_eparam_t *p_shadow_picture_ = nullptr;
	gs_eparam_t *p_shadow_boost_ = nullptr;
	gs_eparam_t *p_outline_width_ = nullptr;
	gs_eparam_t *p_outline_color_ = nullptr;
	gs_eparam_t *p_outline_picture_ = nullptr;
	gs_texrender_t *tr_body_ = nullptr;
	gs_texrender_t *tr_extended_ = nullptr;
	gs_eparam_t *p_extended_ = nullptr;
	gs_eparam_t *p_extend_mirror_ = nullptr;
	gs_eparam_t *p_backdrop_on_ = nullptr;
	gs_eparam_t *p_backdrop_pad_ = nullptr;
	gs_eparam_t *p_backdrop_color_ = nullptr;
	gs_eparam_t *p_shadow_offset_ = nullptr;
	gs_eparam_t *p_shadow_color_ = nullptr;
	gs_eparam_t *p_shadow_gain_ = nullptr;
	gs_eparam_t *p_hide_source_ = nullptr;

	std::unique_ptr<KawaseBlur> blur_;
	gs_texrender_t *tr_input_ = nullptr;
	gs_texrender_t *tr_card_ = nullptr;
	gs_texrender_t *tr_final_ = nullptr;
};

ShadowBackdrop::ShadowBackdrop(obs_data_t *settings, obs_source_t *source) : source_(source)
{
	char *path = obs_module_file("effects/stagekit-shadow.effect");
	obs_enter_graphics();
	char *errors = nullptr;
	effect_ = path ? gs_effect_create_from_file(path, &errors) : nullptr;
	if (!effect_)
		obs_log(LOG_ERROR, "Shadow & Backdrop: failed to load effect: %s", errors ? errors : "not found");
	bfree(errors);
	if (effect_) {
		p_image_ = gs_effect_get_param_by_name(effect_, "image");
		p_shadow_ = gs_effect_get_param_by_name(effect_, "shadow");
		p_out_size_ = gs_effect_get_param_by_name(effect_, "out_size");
		p_src_size_ = gs_effect_get_param_by_name(effect_, "src_size");
		p_pad_ = gs_effect_get_param_by_name(effect_, "pad");
		p_radius_ = gs_effect_get_param_by_name(effect_, "radius");
		p_backdrop_on_ = gs_effect_get_param_by_name(effect_, "backdrop_on");
		p_backdrop_pad_ = gs_effect_get_param_by_name(effect_, "backdrop_pad");
		p_backdrop_color_ = gs_effect_get_param_by_name(effect_, "backdrop_color");
		p_shadow_offset_ = gs_effect_get_param_by_name(effect_, "shadow_offset");
		p_shadow_color_ = gs_effect_get_param_by_name(effect_, "shadow_color");
		p_shadow_gain_ = gs_effect_get_param_by_name(effect_, "shadow_gain");
		p_hide_source_ = gs_effect_get_param_by_name(effect_, "hide_source");
		p_shadow_picture_ = gs_effect_get_param_by_name(effect_, "shadow_picture");
		p_shadow_boost_ = gs_effect_get_param_by_name(effect_, "shadow_boost");
		p_outline_width_ = gs_effect_get_param_by_name(effect_, "outline_width");
		p_outline_color_ = gs_effect_get_param_by_name(effect_, "outline_color");
		p_outline_picture_ = gs_effect_get_param_by_name(effect_, "outline_picture");
		p_extended_ = gs_effect_get_param_by_name(effect_, "extended");
		p_extend_mirror_ = gs_effect_get_param_by_name(effect_, "extend_mirror");
	}
	blur_ = std::make_unique<KawaseBlur>();
	tr_input_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	tr_card_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	tr_final_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	tr_body_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	tr_extended_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();
	bfree(path);

	update(settings);
}

ShadowBackdrop::~ShadowBackdrop()
{
	obs_enter_graphics();
	blur_.reset();
	gs_effect_destroy(effect_);
	gs_texrender_destroy(tr_input_);
	gs_texrender_destroy(tr_card_);
	gs_texrender_destroy(tr_final_);
	gs_texrender_destroy(tr_body_);
	gs_texrender_destroy(tr_extended_);
	obs_leave_graphics();
}

void ShadowBackdrop::update(obs_data_t *settings)
{
	// 0.1.0-beta38 kept this list as a tick box per scene, by the scene's id.
	{
		std::vector<std::string> old_keys;
		std::vector<std::string> listed = pick_list_read(settings, "off_scenes", false);
		for (obs_data_item_t *item = obs_data_first(settings); item; obs_data_item_next(&item)) {
			const std::string key = obs_data_item_get_name(item);
			if (key.rfind("off_in:", 0) != 0)
				continue;
			old_keys.push_back(key);
			if (!obs_data_item_get_bool(item))
				continue;
			if (obs_source_t *scene = obs_get_source_by_uuid(key.c_str() + 7)) {
				const std::string name = obs_source_get_name(scene);
				if (std::find(listed.begin(), listed.end(), name) == listed.end())
					listed.push_back(name);
				obs_source_release(scene);
			}
		}
		if (!old_keys.empty()) {
			for (const std::string &key : old_keys)
				obs_data_erase(settings, key.c_str());
			pick_list_write(settings, "off_scenes", false, listed);
		}
	}
	Settings s;
	s.shadow = obs_data_get_bool(settings, "shadow");
	s.shadow_color = (uint32_t)obs_data_get_int(settings, "shadow_color");
	s.shadow_opacity = std::clamp(obs_data_get_double(settings, "shadow_opacity") / 100.0, 0.0, 1.0);
	s.offset_x = std::clamp(obs_data_get_double(settings, "offset_x"), -500.0, 500.0);
	s.offset_y = std::clamp(obs_data_get_double(settings, "offset_y"), -500.0, 500.0);
	s.softness = std::clamp(obs_data_get_double(settings, "softness"), 0.0, 200.0);
	s.spread = std::clamp(obs_data_get_double(settings, "spread") / 100.0, 0.0, 1.0);
	s.backdrop = obs_data_get_bool(settings, "backdrop");
	s.backdrop_color = (uint32_t)obs_data_get_int(settings, "backdrop_color");
	s.backdrop_opacity = std::clamp(obs_data_get_double(settings, "backdrop_opacity") / 100.0, 0.0, 1.0);
	s.backdrop_pad = std::clamp(obs_data_get_double(settings, "backdrop_pad"), 0.0, 500.0);
	s.radius = std::clamp(obs_data_get_double(settings, "radius"), 0.0, 2000.0);
	s.hide_source = obs_data_get_bool(settings, "hide_source");
	s.shadow_type = obs_data_get_int(settings, "shadow_type") == 1 ? ShadowType::Glow : ShadowType::Drop;
	s.shadow_fill = obs_data_get_int(settings, "shadow_fill") == 1 ? Fill::Picture : Fill::Color;
	s.shadow_boost = std::clamp(obs_data_get_double(settings, "shadow_boost") / 100.0, 0.0, 1.0);
	s.outline = obs_data_get_bool(settings, "outline");
	s.outline_width = std::clamp(obs_data_get_double(settings, "outline_width"), 0.0, 200.0);
	s.outline_color = (uint32_t)obs_data_get_int(settings, "outline_color");
	s.outline_opacity = std::clamp(obs_data_get_double(settings, "outline_opacity") / 100.0, 0.0, 1.0);
	s.outline_fill = obs_data_get_int(settings, "outline_fill") == 1 ? Fill::Picture : Fill::Color;
	s.outline_edge = obs_data_get_int(settings, "outline_edge") == 1 ? Edge::Mirror : Edge::Stretch;
	s.outline_blur = std::clamp(obs_data_get_double(settings, "outline_blur"), 0.0, 200.0);
	// A glow sits evenly all round.
	if (s.shadow_type == ShadowType::Glow)
		s.offset_x = s.offset_y = 0.0;
	if (s.outline_width < 0.5)
		s.outline = false;
	s.screen_px = obs_data_get_bool(settings, "screen_px");

	std::lock_guard lock(mutex_);
	settings_ = s;
	keep_place_ = obs_data_get_bool(settings, "keep_place");
	if (!adjusted_loaded_) {
		adjusted_loaded_ = true;
		// A shadow set up before this was kept track of: its scene items
		// were already arranged around the room it takes, so leave them.
		obs_data_item_t *first = obs_data_first(settings);
		const bool set_up_before = first != nullptr && !obs_data_has_user_value(settings, "adjusted_left");
		obs_data_item_release(&first);
		if (set_up_before && keep_place_) {
			adjusted_ = padding_for(s);
			return;
		}
		adjusted_.left = (uint32_t)obs_data_get_int(settings, "adjusted_left");
		adjusted_.top = (uint32_t)obs_data_get_int(settings, "adjusted_top");
		adjusted_.right = (uint32_t)obs_data_get_int(settings, "adjusted_right");
		adjusted_.bottom = (uint32_t)obs_data_get_int(settings, "adjusted_bottom");
	}
}

// Brings every scene item showing the source in line with the room the
// shadow takes now.
void ShadowBackdrop::settle_items(obs_source_t *parent, const Padding &wanted)
{
	Padding before;
	{
		std::lock_guard lock(mutex_);
		before = adjusted_;
	}
	if (before.left == wanted.left && before.top == wanted.top && before.right == wanted.right &&
	    before.bottom == wanted.bottom)
		return;
	obs_source_t *target = obs_filter_get_target(source_);
	const uint32_t w = target ? obs_source_get_base_width(target) : 0;
	const uint32_t h = target ? obs_source_get_base_height(target) : 0;
	if (!w || !h)
		return; // nothing to measure against yet; try again next frame
	for (obs_sceneitem_t *item : items_showing(parent)) {
		keep_picture_in_place(item, w, h, before, wanted);
		obs_sceneitem_release(item);
	}
	{
		std::lock_guard lock(mutex_);
		adjusted_ = wanted;
	}
	obs_data_t *settings = obs_source_get_settings(source_);
	obs_data_set_int(settings, "adjusted_left", wanted.left);
	obs_data_set_int(settings, "adjusted_top", wanted.top);
	obs_data_set_int(settings, "adjusted_right", wanted.right);
	obs_data_set_int(settings, "adjusted_bottom", wanted.bottom);
	obs_data_release(settings);
}

void ShadowBackdrop::tick()
{
	obs_source_t *parent = obs_filter_get_parent(source_);
	if (!parent)
		return;
	bool keep;
	{
		std::lock_guard lock(mutex_);
		keep = keep_place_;
	}
	// The room recorded as already given belongs to the source it was given
	// on. A filter copied or pasted onto something else arrives with the
	// other source's record: there, nothing has been adjusted yet.
	if (!owner_checked_) {
		owner_checked_ = true;
		const char *uuid = obs_source_get_uuid(parent);
		obs_data_t *settings = obs_source_get_settings(source_);
		const std::string recorded = obs_data_get_string(settings, "adjusted_for");
		if (uuid && !recorded.empty() && recorded != uuid) {
			std::lock_guard lock(mutex_);
			adjusted_ = Padding();
			for (const char *side : {"adjusted_left", "adjusted_top", "adjusted_right", "adjusted_bottom"})
				obs_data_set_int(settings, side, 0);
		}
		if (uuid)
			obs_data_set_string(settings, "adjusted_for", uuid);
		obs_data_release(settings);
	}
	// A filter that is switched off, or told not to, takes no room.
	// Now and then, see how big the source is drawn where it is on screen.
	if (--scale_countdown_ <= 0) {
		scale_countdown_ = 20;
		// And whether the scene on screen is one it is left off in.
		bool off = false;
		if (obs_source_t *on_screen = scene_on_screen()) {
			std::vector<obs_source_t *> holders;
			Showing look{parent, &holders, 0};
			showing_holders(on_screen, look);
			obs_data_t *settings = obs_source_get_settings(source_);
			const std::vector<std::string> listed = pick_list_read(settings, "off_scenes", false);
			auto is_listed = [&](obs_source_t *scene) {
				const char *name = scene ? obs_source_get_name(scene) : nullptr;
				return name && std::find(listed.begin(), listed.end(), name) != listed.end();
			};
			// Off when the scene on screen is listed, or when every scene
			// showing the source there is.
			off = !holders.empty();
			for (obs_source_t *scene : holders) {
				off = off && is_listed(scene);
				obs_source_release(scene);
			}
			off = off || (!listed.empty() && is_listed(on_screen));
			obs_data_release(settings);
			obs_source_release(on_screen);
		}
		{
			std::lock_guard lock(mutex_);
			off_here_ = off;
		}
		double sx = 0.0, sy = 0.0;
		const bool found = shown_scale(parent, sx, sy);
		std::lock_guard lock(mutex_);
		// Guard against a source drawn almost invisibly small.
		shown_sx_ = found ? std::max(0.05, sx) : 1.0;
		shown_sy_ = found ? std::max(0.05, sy) : 1.0;
	}
	const bool active = keep && effect_ && obs_source_enabled(source_);
	settle_items(parent, active ? padding_for(settings()) : Padding());
}

// The filter is being taken off its source: give the room back.
void ShadowBackdrop::leave(obs_source_t *parent)
{
	if (parent)
		settle_items(parent, Padding());
}

// The settings in the source's own pixels, which is what is drawn.
Settings ShadowBackdrop::settings()
{
	std::lock_guard lock(mutex_);
	Settings s = settings_;
	if (off_here_) {
		// Nothing drawn and no room taken.
		s.shadow = s.outline = s.backdrop = false;
		s.radius = 0.0;
	}
	if (s.screen_px && shown_sx_ > 0.0 && shown_sy_ > 0.0) {
		const double both = std::sqrt(shown_sx_ * shown_sy_);
		s.offset_x /= shown_sx_;
		s.offset_y /= shown_sy_;
		s.softness = std::min(200.0, s.softness / both);
		s.backdrop_pad /= both;
		s.outline_width = std::min(200.0, s.outline_width / both);
		s.outline_blur = std::min(200.0, s.outline_blur / both);
		s.radius /= both;
	}
	return s;
}

uint32_t ShadowBackdrop::width()
{
	obs_source_t *target = obs_filter_get_target(source_);
	const uint32_t w = target ? obs_source_get_base_width(target) : 0;
	if (!w)
		return 0;
	const Padding p = padding_for(settings());
	return w + p.left + p.right;
}

uint32_t ShadowBackdrop::height()
{
	obs_source_t *target = obs_filter_get_target(source_);
	const uint32_t h = target ? obs_source_get_base_height(target) : 0;
	if (!h)
		return 0;
	const Padding p = padding_for(settings());
	return h + p.top + p.bottom;
}

bool ShadowBackdrop::capture_input(uint32_t w, uint32_t h)
{
	gs_texrender_reset(tr_input_);
	if (!obs_source_process_filter_begin(source_, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
		return false;
	if (!gs_texrender_begin(tr_input_, w, h))
		return false;
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	obs_source_process_filter_end(source_, obs_get_base_effect(OBS_EFFECT_DEFAULT), w, h);
	gs_blend_state_pop();
	gs_texrender_end(tr_input_);
	return true;
}

void ShadowBackdrop::render()
{
	obs_source_t *target = obs_filter_get_target(source_);
	obs_source_t *parent = obs_filter_get_parent(source_);
	const uint32_t w = target ? obs_source_get_base_width(target) : 0;
	const uint32_t h = target ? obs_source_get_base_height(target) : 0;
	bool off_here;
	{
		std::lock_guard lock(mutex_);
		off_here = off_here_;
	}
	if (!effect_ || !parent || !w || !h || off_here) {
		obs_source_skip_video_filter(source_);
		return;
	}

	const Settings s = settings();
	const Padding p = padding_for(s);
	const uint32_t ow = w + p.left + p.right;
	const uint32_t oh = h + p.top + p.bottom;

	if (!capture_input(w, h))
		return;
	gs_texture_t *input = gs_texrender_get_texture(tr_input_);
	if (!input)
		return;

	// The intermediate passes work on raw colour values.
	const bool previous_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);

	vec2 out_size, src_size, pad;
	vec2_set(&out_size, (float)ow, (float)oh);
	vec2_set(&src_size, (float)w, (float)h);
	vec2_set(&pad, (float)p.left, (float)p.top);

	auto draw = [&](gs_texrender_t *dst, gs_texture_t *image, const char *technique) {
		gs_texrender_reset(dst);
		if (!gs_texrender_begin(dst, ow, oh))
			return;
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		gs_ortho(0.0f, (float)ow, 0.0f, (float)oh, -100.0f, 100.0f);
		gs_effect_set_texture(p_image_, image);
		while (gs_effect_loop(effect_, technique))
			gs_draw_sprite(image, 0, ow, oh);
		gs_blend_state_pop();
		gs_texrender_end(dst);
	};

	// 1. The source on its panel, with rounded corners, inside the padding.
	vec4 backdrop_color;
	vec4_from_rgba(&backdrop_color, s.backdrop_color);
	backdrop_color.w = (float)s.backdrop_opacity;
	gs_effect_set_vec2(p_out_size_, &out_size);
	gs_effect_set_vec2(p_src_size_, &src_size);
	gs_effect_set_vec2(p_pad_, &pad);
	gs_effect_set_float(p_radius_, (float)s.radius);
	gs_effect_set_float(p_backdrop_on_, s.backdrop ? 1.0f : 0.0f);
	gs_effect_set_float(p_backdrop_pad_, (float)s.backdrop_pad);
	gs_effect_set_vec4(p_backdrop_color_, &backdrop_color);
	draw(tr_card_, input, "Card");
	gs_texture_t *card = gs_texrender_get_texture(tr_card_);

	// 2. A line around it, in a colour or in the picture's own colours.
	if (card && s.outline) {
		// Made from the picture: the picture itself carried on past its
		// edges, softened as asked, shows through the line.
		gs_texture_t *extended = card;
		if (s.outline_fill == Fill::Picture) {
			gs_effect_set_float(p_extend_mirror_, s.outline_edge == Edge::Mirror ? 1.0f : 0.0f);
			// An effect's values last for one draw: give the sizes again.
			gs_effect_set_vec2(p_out_size_, &out_size);
			gs_effect_set_vec2(p_src_size_, &src_size);
			gs_effect_set_vec2(p_pad_, &pad);
			draw(tr_extended_, input, "Extend");
			if (gs_texture_t *whole = gs_texrender_get_texture(tr_extended_))
				extended = blur_->blur(whole, ow, oh, s.outline_blur);
		}
		vec4 outline_color;
		vec4_from_rgba(&outline_color, s.outline_color);
		outline_color.w = (float)s.outline_opacity;
		gs_effect_set_vec2(p_out_size_, &out_size);
		gs_effect_set_texture(p_extended_, extended);
		gs_effect_set_float(p_outline_width_, (float)s.outline_width);
		gs_effect_set_vec4(p_outline_color_, &outline_color);
		gs_effect_set_float(p_outline_picture_, s.outline_fill == Fill::Picture ? 1.0f : 0.0f);
		draw(tr_body_, card, "Outline");
		if (gs_texture_t *body = gs_texrender_get_texture(tr_body_))
			card = body;
	}

	// 3. Its silhouette, softened, becomes the shadow.
	gs_texture_t *silhouette = card;
	if (card && s.shadow && s.softness > 0.0)
		silhouette = blur_->blur(card, ow, oh, s.softness);

	// 4. Shadow underneath the card.
	if (card) {
		vec2 offset;
		vec2_set(&offset, (float)s.offset_x, (float)s.offset_y);
		vec4 shadow_color;
		vec4_from_rgba(&shadow_color, s.shadow_color);
		shadow_color.w = s.shadow ? (float)s.shadow_opacity : 0.0f;
		gs_effect_set_vec2(p_out_size_, &out_size);
		gs_effect_set_texture(p_shadow_, silhouette);
		gs_effect_set_vec2(p_shadow_offset_, &offset);
		gs_effect_set_vec4(p_shadow_color_, &shadow_color);
		gs_effect_set_float(p_shadow_gain_, (float)(1.0 + s.spread * 6.0));
		gs_effect_set_float(p_hide_source_, s.hide_source ? 1.0f : 0.0f);
		gs_effect_set_float(p_shadow_picture_, s.shadow_fill == Fill::Picture ? 1.0f : 0.0f);
		gs_effect_set_float(p_shadow_boost_, (float)s.shadow_boost);
		draw(tr_final_, card, "Compose");
	}

	gs_enable_framebuffer_srgb(previous_srgb);

	if (gs_texture_t *final_tex = card ? gs_texrender_get_texture(tr_final_) : nullptr) {
		gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *image = gs_effect_get_param_by_name(def, "image");
		const bool linear = gs_get_linear_srgb();
		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(linear);
		if (linear)
			gs_effect_set_texture_srgb(image, final_tex);
		else
			gs_effect_set_texture(image, final_tex);
		while (gs_effect_loop(def, "Draw"))
			gs_draw_sprite(final_tex, 0, ow, oh);
		gs_enable_framebuffer_srgb(previous);
	}
}

// --------------------------------------------------------------- properties

bool visibility_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	// Only what applies to the choices made is shown.
	const bool glow = obs_data_get_int(settings, "shadow_type") == 1;
	const bool shadow_picture = obs_data_get_int(settings, "shadow_fill") == 1;
	const bool outline_picture = obs_data_get_int(settings, "outline_fill") == 1;
	obs_property_set_visible(obs_properties_get(props, "offset_x"), !glow);
	obs_property_set_visible(obs_properties_get(props, "offset_y"), !glow);
	obs_property_set_visible(obs_properties_get(props, "shadow_color"), !shadow_picture);
	obs_property_set_visible(obs_properties_get(props, "shadow_boost"), shadow_picture);
	obs_property_set_visible(obs_properties_get(props, "outline_color"), !outline_picture);
	obs_property_set_visible(obs_properties_get(props, "outline_edge"), outline_picture);
	obs_property_set_visible(obs_properties_get(props, "outline_blur"), outline_picture);
	return true;
}

obs_property_t *slider(obs_properties_t *props, const char *name, const char *text, double min, double max,
		       const char *suffix)
{
	obs_property_t *p = obs_properties_add_float_slider(props, name, obs_module_text(text), min, max, 1.0);
	obs_property_float_set_suffix(p, suffix);
	return p;
}

obs_properties_t *get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	auto made_from = [](obs_properties_t *group, const char *name) {
		obs_property_t *fill = obs_properties_add_list(group, name, obs_module_text("Shadow.Fill"),
							       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(fill, obs_module_text("Shadow.Fill.Color"), (int)Fill::Color);
		obs_property_list_add_int(fill, obs_module_text("Shadow.Fill.Picture"), (int)Fill::Picture);
		obs_property_set_long_description(fill, obs_module_text("Shadow.Fill.Tip"));
		obs_property_set_modified_callback(fill, visibility_modified);
	};

	// Each part has a tick box of its own in its heading.
	obs_properties_t *shadow = obs_properties_create();
	obs_property_t *type = obs_properties_add_list(shadow, "shadow_type", obs_module_text("Shadow.Type"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(type, obs_module_text("Shadow.Type.Drop"), (int)ShadowType::Drop);
	obs_property_list_add_int(type, obs_module_text("Shadow.Type.Glow"), (int)ShadowType::Glow);
	obs_property_set_modified_callback(type, visibility_modified);
	made_from(shadow, "shadow_fill");
	obs_properties_add_color(shadow, "shadow_color", obs_module_text("Shadow.Color"));
	obs_property_t *boost = slider(shadow, "shadow_boost", "Shadow.Boost", 0.0, 100.0, "%");
	obs_property_set_long_description(boost, obs_module_text("Shadow.Boost.Tip"));
	slider(shadow, "shadow_opacity", "Shadow.Opacity", 0.0, 100.0, "%");
	slider(shadow, "offset_x", "Shadow.OffsetX", -200.0, 200.0, " px");
	slider(shadow, "offset_y", "Shadow.OffsetY", -200.0, 200.0, " px");
	slider(shadow, "softness", "Shadow.Softness", 0.0, 200.0, " px");
	slider(shadow, "spread", "Shadow.Spread", 0.0, 100.0, "%");
	obs_properties_add_bool(shadow, "hide_source", obs_module_text("Shadow.HideSource"));
	obs_properties_add_group(props, "shadow", obs_module_text("Shadow.Shadow"), OBS_GROUP_CHECKABLE, shadow);

	obs_properties_t *outline = obs_properties_create();
	slider(outline, "outline_width", "Shadow.OutlineWidth", 0.0, 100.0, " px");
	made_from(outline, "outline_fill");
	obs_properties_add_color(outline, "outline_color", obs_module_text("Shadow.OutlineColor"));
	obs_property_t *edge = obs_properties_add_list(outline, "outline_edge", obs_module_text("Shadow.OutlineEdge"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(edge, obs_module_text("Shadow.OutlineEdge.Stretch"), (int)Edge::Stretch);
	obs_property_list_add_int(edge, obs_module_text("Shadow.OutlineEdge.Mirror"), (int)Edge::Mirror);
	obs_property_set_long_description(edge, obs_module_text("Shadow.OutlineEdge.Tip"));
	obs_property_t *line_blur = slider(outline, "outline_blur", "Shadow.OutlineBlur", 0.0, 200.0, " px");
	obs_property_set_long_description(line_blur, obs_module_text("Shadow.OutlineBlur.Tip"));
	slider(outline, "outline_opacity", "Shadow.OutlineOpacity", 0.0, 100.0, "%");
	obs_properties_add_group(props, "outline", obs_module_text("Shadow.Outline"), OBS_GROUP_CHECKABLE, outline);

	obs_properties_t *backdrop = obs_properties_create();
	obs_properties_add_color(backdrop, "backdrop_color", obs_module_text("Shadow.BackdropColor"));
	slider(backdrop, "backdrop_opacity", "Shadow.BackdropOpacity", 0.0, 100.0, "%");
	slider(backdrop, "backdrop_pad", "Shadow.BackdropPad", 0.0, 200.0, " px");
	obs_properties_add_group(props, "backdrop", obs_module_text("Shadow.Backdrop"), OBS_GROUP_CHECKABLE, backdrop);

	slider(props, "radius", "Shadow.Radius", 0.0, 500.0, " px");

	// Scenes it is left off in: chosen from the scenes that exist.
	{
		obs_source_t *filter = data ? static_cast<ShadowBackdrop *>(data)->filter() : nullptr;
		PickList off;
		off.setting = "off_scenes";
		off.title = "Shadow.OffIn";
		off.add = "List.AddScene";
		off.tip = "Shadow.OffIn.Tip";
		off.choices = [](std::vector<std::string> &names) {
			obs_enum_scenes(
				[](void *param, obs_source_t *scene) {
					const char *name = obs_source_get_name(scene);
					if (name && *name && !obs_source_is_group(scene))
						static_cast<std::vector<std::string> *>(param)->push_back(name);
					return true;
				},
				&names);
		};
		pick_list_add(props, filter, off);
	}
	obs_property_t *screen = obs_properties_add_bool(props, "screen_px", obs_module_text("Shadow.ScreenPx"));
	obs_property_set_long_description(screen, obs_module_text("Shadow.ScreenPx.Tip"));
	obs_property_t *keep = obs_properties_add_bool(props, "keep_place", obs_module_text("Shadow.Keep"));
	obs_property_set_long_description(keep, obs_module_text("Shadow.Keep.Tip"));
	return props;
}

void get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "shadow", true);
	obs_data_set_default_int(settings, "shadow_color", 0xFF000000);
	obs_data_set_default_double(settings, "shadow_opacity", 60.0);
	obs_data_set_default_double(settings, "offset_x", 12.0);
	obs_data_set_default_double(settings, "offset_y", 12.0);
	obs_data_set_default_double(settings, "softness", 20.0);
	obs_data_set_default_double(settings, "spread", 0.0);
	obs_data_set_default_bool(settings, "hide_source", false);
	obs_data_set_default_int(settings, "shadow_type", (int)ShadowType::Drop);
	obs_data_set_default_int(settings, "shadow_fill", (int)Fill::Color);
	obs_data_set_default_double(settings, "shadow_boost", 30.0);
	obs_data_set_default_bool(settings, "outline", false);
	obs_data_set_default_double(settings, "outline_width", 6.0);
	obs_data_set_default_int(settings, "outline_fill", (int)Fill::Color);
	obs_data_set_default_int(settings, "outline_edge", (int)Edge::Stretch);
	obs_data_set_default_double(settings, "outline_blur", 20.0);
	obs_data_set_default_int(settings, "outline_color", 0xFFFFFFFF);
	obs_data_set_default_double(settings, "outline_opacity", 100.0);
	obs_data_set_default_bool(settings, "backdrop", false);
	obs_data_set_default_int(settings, "backdrop_color", 0xFF3A3A3A);
	obs_data_set_default_double(settings, "backdrop_opacity", 100.0);
	obs_data_set_default_double(settings, "backdrop_pad", 12.0);
	obs_data_set_default_double(settings, "radius", 0.0);
	obs_data_set_default_bool(settings, "keep_place", true);
	obs_data_set_default_bool(settings, "screen_px", true);
}

} // namespace

bool shadow_free(obs_source_t *source, ShadowFree &out)
{
	out = ShadowFree();
	// Every shadow filter switched on, and which one is applied first: OBS
	// keeps a source's filters with the one applied first at the end.
	struct Find {
		obs_source_t *source;
		obs_source_t *first = nullptr;
		int first_index = -1;
		double left = 0.0, top = 0.0;
		bool on_screen = false;
	} find{source};
	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			const char *id = obs_source_get_unversioned_id(filter);
			if (!id || strcmp(id, kShadowBackdropId) != 0 || !obs_source_enabled(filter))
				return;
			auto *f = static_cast<Find *>(param);
			auto *shadow = static_cast<ShadowBackdrop *>(obs_obj_get_data(filter));
			if (shadow) {
				// Where the room it takes now puts its picture.
				const Padding p = shadow->padding_now();
				f->left += p.left;
				f->top += p.top;
				f->on_screen = f->on_screen || shadow->in_screen_pixels();
			}
			const int index = obs_source_filter_get_index(f->source, filter);
			if (index > f->first_index) {
				f->first_index = index;
				f->first = filter;
			}
		},
		&find);
	if (!find.first)
		return false;
	obs_source_t *under = obs_filter_get_target(find.first);
	if (!under)
		return false;
	out.draw = obs_source_get_ref(under);
	if (!out.draw)
		return false;
	out.bare = under == source;
	out.left = find.left;
	out.top = find.top;
	out.fixed_on_screen = find.on_screen;
	return true;
}

void shadow_room(obs_source_t *source, double room[4])
{
	room[0] = room[1] = room[2] = room[3] = 0.0;
	struct Find {
		double *room;
	} find{room};
	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			const char *id = obs_source_get_unversioned_id(filter);
			if (!id || strcmp(id, kShadowBackdropId) != 0 || !obs_source_enabled(filter))
				return;
			auto *f = static_cast<Find *>(param);
			obs_data_t *settings = obs_source_get_settings(filter);
			f->room[0] += (double)obs_data_get_int(settings, "adjusted_left");
			f->room[1] += (double)obs_data_get_int(settings, "adjusted_top");
			f->room[2] += (double)obs_data_get_int(settings, "adjusted_right");
			f->room[3] += (double)obs_data_get_int(settings, "adjusted_bottom");
			obs_data_release(settings);
		},
		&find);
}

void register_shadow_backdrop()
{
	obs_source_info info = {};
	info.id = kShadowBackdropId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) {
		return obs_module_text("Shadow.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new ShadowBackdrop(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<ShadowBackdrop *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<ShadowBackdrop *>(data)->update(settings);
	};
	info.get_defaults = get_defaults;
	info.get_properties = get_properties;
	info.get_width = [](void *data) {
		return static_cast<ShadowBackdrop *>(data)->width();
	};
	info.get_height = [](void *data) {
		return static_cast<ShadowBackdrop *>(data)->height();
	};
	info.video_render = [](void *data, gs_effect_t *) {
		static_cast<ShadowBackdrop *>(data)->render();
	};
	info.video_tick = [](void *data, float) {
		static_cast<ShadowBackdrop *>(data)->tick();
	};
	info.filter_remove = [](void *data, obs_source_t *parent) {
		static_cast<ShadowBackdrop *>(data)->leave(parent);
	};
	obs_register_source(&info);
}

} // namespace stagekit
