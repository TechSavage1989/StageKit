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

#include "../common/loading.hpp"
#include "../common/scene-rev.hpp"
#include "../common/filter-input.hpp"
#include "shadow-backdrop.hpp"
#include "../common/timing.hpp"

#include "../common/kawase-blur.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-util.hpp"
#include "../common/shown-scale.hpp"
#include "../common/tooltip.hpp"
#include "../look/look-paint.hpp"
#include "../look/outline-ambient.hpp"
#include "../look/outline-pattern.hpp"

#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// ==== scene-filters:fit ====
#include <atomic>
namespace stagekit {
// Raised whenever a shadow's room may have changed, so a fit reads it again.
static std::atomic<uint64_t> g_room_rev{1};
uint64_t shadow_room_revision()
{
	return g_room_rev.load();
}
} // namespace stagekit
// ==== end scene-filters:fit ====

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
	int outline_corners = 0; // 0 round the picture's own shape; 1..4 round its box (look-paint.hpp)
	double outline_corner_size = 20.0;
	OutlineDesign design;
	double pattern_shown = 1.0; // a pattern fill's size in the source's pixels, for one on screen
	double intensity = 1.0;
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
	// (Neon's halo of light reaches past the line.)
	const double halo = s.outline && s.outline_fill != Fill::Picture ? s.design.halo_reach(s.outline_width) : 0.0;
	const double around = panel + line + (halo > 0.0 ? std::ceil(halo) + 2.0 : 0.0);
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
	void keep_in_view(obs_source_t *parent);

	obs_source_t *source_;
	std::mutex mutex_;
	Settings settings_;
	// The room around the source that scene items have been adjusted for.
	// Saved with the filter, so loading a collection adjusts nothing twice.
	Padding adjusted_;
	bool adjusted_loaded_ = false;
	bool owner_checked_ = false;
	bool keep_place_ = true;
	// "Keep it on screen": where each item was last seen and since when it
	// has been still (used on the tick only).
	bool in_view_ = false;
	unsigned in_view_turn_ = 0;
	struct Still {
		Box box;
		uint64_t since = 0;
	};
	std::map<obs_sceneitem_t *, Still> still_;
	// How big the source is drawn on screen, for sizes given in screen pixels.
	double shown_sx_ = 1.0, shown_sy_ = 1.0;
	double prev_sx_ = 0.0, prev_sy_ = 0.0; // the scale taken before this one
	// Standing down because of the scene on screen.
	bool off_here_ = false;
	int scale_countdown_ = 0;
	// ==== scene-filters:perf ====
	// What the scale was last looked at with: the scenes' revision, the
	// scene on screen (compared, never held) and the source's size.
	bool scale_check_due(obs_source_t *parent);
	bool view_check_due(obs_source_t *parent);
	bool view_pending_ = true; // "Keep it on screen" has an item to look at again
	std::atomic<bool> view_dirty_{true};
	uint64_t view_rev_ = 0;
	uint32_t view_w_ = 0, view_h_ = 0;
	int view_idle_ = 0;
	std::atomic<bool> scale_dirty_{true};
	uint64_t seen_scene_rev_ = 0;
	const obs_source_t *seen_on_screen_ = nullptr;
	uint32_t seen_w_ = 0, seen_h_ = 0;
	int scale_idle_ = 0;
	bool scale_staggered_ = false;
	// ==== end scene-filters:perf ====

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
	gs_eparam_t *p_outline_corners_ = nullptr;
	gs_eparam_t *p_corner_size_ = nullptr;
	gs_eparam_t *p_intensity_ = nullptr;
	gs_eparam_t *p_out_linear_ = nullptr;
	gs_eparam_t *p_rect_tex_ = nullptr;
	gs_eparam_t *p_rect_known_ = nullptr;
	gs_eparam_t *p_area_tex_ = nullptr;
	std::unique_ptr<VisibleRect> rects_; // what shows of the source, for a line round it
	gs_texture_t *no_rect_ = nullptr;
	gs_texrender_t *tr_body_ = nullptr;
	gs_texrender_t *tr_extended_ = nullptr;
	gs_texrender_t *tr_plain_ = nullptr;
	DesignUniforms design_;
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
	std::unique_ptr<KawaseBlur> halo_blur_; // Neon's halo, made when first wanted
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
		p_outline_corners_ = gs_effect_get_param_by_name(effect_, "outline_corners");
		p_corner_size_ = gs_effect_get_param_by_name(effect_, "corner_size");
		p_intensity_ = gs_effect_get_param_by_name(effect_, "glow_intensity");
		p_out_linear_ = gs_effect_get_param_by_name(effect_, "out_linear");
		design_.find(effect_);
		p_rect_tex_ = gs_effect_get_param_by_name(effect_, "rect_tex");
		p_rect_known_ = gs_effect_get_param_by_name(effect_, "rect_known");
		p_area_tex_ = gs_effect_get_param_by_name(effect_, "area_tex");
		p_extended_ = gs_effect_get_param_by_name(effect_, "extended");
		p_extend_mirror_ = gs_effect_get_param_by_name(effect_, "extend_mirror");
	}
	// (Smooth: sixteen-bit pictures and one more level, so a soft shadow or
	// glow has a round falloff and no steps in its tail.)
	blur_ = std::make_unique<KawaseBlur>(true);
	tr_input_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	tr_card_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	// (Sixteen-bit: OBS lays the picture over what is under it in linear
	// light, where one step of eight-bit alpha near black is some thirteen
	// levels on screen, a visible band at the end of a soft shadow or glow.)
	tr_final_ = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	tr_body_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	tr_extended_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	tr_plain_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	{
		// (A stand-in for rect_tex when no rectangle is known.)
		const float none[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		const uint8_t *data = reinterpret_cast<const uint8_t *>(none);
		no_rect_ = gs_texture_create(1, 1, GS_RGBA32F, 1, &data, 0);
	}
	obs_leave_graphics();
	bfree(path);
	rects_ = std::make_unique<VisibleRect>();

	update(settings);
}

ShadowBackdrop::~ShadowBackdrop()
{
	rects_.reset();
	obs_enter_graphics();
	blur_.reset();
	halo_blur_.reset();
	gs_texture_destroy(no_rect_);
	gs_effect_destroy(effect_);
	gs_texrender_destroy(tr_input_);
	gs_texrender_destroy(tr_card_);
	gs_texrender_destroy(tr_final_);
	gs_texrender_destroy(tr_body_);
	gs_texrender_destroy(tr_extended_);
	gs_texrender_destroy(tr_plain_);
	obs_leave_graphics();
}

void ShadowBackdrop::update(obs_data_t *settings)
{
	// ==== scene-filters:fit ====
	g_room_rev++;
	// ==== end scene-filters:fit ====
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
	s.outline_corners = (int)std::clamp<long long>(obs_data_get_int(settings, "outline_corners"), 0, 5);
	s.outline_corner_size = std::clamp(obs_data_get_double(settings, "outline_corner_size"), 0.0, 500.0);
	s.intensity = std::clamp(obs_data_get_double(settings, "glow_intensity") / 100.0, 0.25, 4.0);
	s.design = read_outline_design(settings);
	// A glow sits evenly all round.
	if (s.shadow_type == ShadowType::Glow)
		s.offset_x = s.offset_y = 0.0;
	if (s.outline_width < 0.5)
		s.outline = false;
	s.screen_px = obs_data_get_bool(settings, "screen_px");
	in_view_ = obs_data_get_bool(settings, "in_view");
	// ==== scene-filters:perf ====
	scale_dirty_ = true; // "Off in these scenes" may have changed
	view_dirty_ = true;
	// ==== end scene-filters:perf ====

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
			// (Written down as well: the other tools read the room from
			// here, and took such a shadow for one that takes none.)
			obs_data_set_int(settings, "adjusted_left", adjusted_.left);
			obs_data_set_int(settings, "adjusted_top", adjusted_.top);
			obs_data_set_int(settings, "adjusted_right", adjusted_.right);
			obs_data_set_int(settings, "adjusted_bottom", adjusted_.bottom);
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
	// ==== scene-filters:fit ====
	g_room_rev++;
	// ==== end scene-filters:fit ====
}

// "Keep it on screen": an item whose picture is on the canvas but whose
// shadow, glow or outline runs off an edge is made smaller, by just enough
// for the effect to show whole. It stays against the edge it was at.
//
// Only an item that has been still for a moment is touched, so one being
// dragged, or gliding in from off screen, is left alone; so is one whose
// picture itself is off the canvas, which is somebody's intention.
void ShadowBackdrop::keep_in_view(obs_source_t *parent)
{
	Padding room;
	bool fixed;
	{
		std::lock_guard lock(mutex_);
		room = adjusted_;
		fixed = settings_.screen_px;
	}
	const uint64_t now = os_gettime_ns();
	std::map<obs_sceneitem_t *, Still> next;
	view_pending_ = false;
	for (obs_sceneitem_t *item : items_showing(parent)) {
		struct Release {
			obs_sceneitem_t *item;
			~Release() { obs_sceneitem_release(item); }
		} release{item};
		obs_scene_t *scene = obs_sceneitem_get_scene(item);
		obs_source_t *scene_source = scene ? obs_scene_get_source(scene) : nullptr;
		// Plain items straight in a scene: an item fitted by a bounding box
		// is sized by whoever set the box (Fit Item does this itself), and
		// an item in a group is placed in the group's own space.
		if (!scene_source || obs_scene_is_group(scene) || !obs_sceneitem_visible(item) ||
		    obs_sceneitem_locked(item) || obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE ||
		    std::fabs(obs_sceneitem_get_rot(item)) > 0.01f)
			continue;
		const double cw = obs_source_get_width(scene_source), ch = obs_source_get_height(scene_source);
		const Box box = item_box(item);
		if (cw < 2.0 || ch < 2.0 || box.w < 2.0 || box.h < 2.0)
			continue;
		Still seen;
		const auto found = still_.find(item);
		if (found != still_.end() && std::fabs(found->second.box.x - box.x) < 0.25 &&
		    std::fabs(found->second.box.y - box.y) < 0.25 && std::fabs(found->second.box.w - box.w) < 0.25 &&
		    std::fabs(found->second.box.h - box.h) < 0.25) {
			seen = found->second;
		} else {
			seen.box = box;
			seen.since = now;
		}
		next[item] = seen;
		if (now - seen.since < 400000000ULL) {
			view_pending_ = true; // not still long enough yet: looked at again
			continue;
		}

		vec2 scale, pos;
		obs_sceneitem_get_scale(item, &scale);
		obs_sceneitem_get_pos(item, &pos);
		obs_sceneitem_crop crop;
		obs_sceneitem_get_crop(item, &crop);
		// The effect's room on each side, on the canvas.
		const double rl = std::max(0.0, (double)room.left - crop.left) * std::fabs(scale.x);
		const double rr = std::max(0.0, (double)room.right - crop.right) * std::fabs(scale.x);
		const double rt = std::max(0.0, (double)room.top - crop.top) * std::fabs(scale.y);
		const double rb = std::max(0.0, (double)room.bottom - crop.bottom) * std::fabs(scale.y);
		const double over_l = -box.x, over_r = box.x + box.w - cw;
		const double over_t = -box.y, over_b = box.y + box.h - ch;
		// The picture itself off the canvas: not for this to change.
		if (over_l > rl + 1.0 || over_r > rr + 1.0 || over_t > rt + 1.0 || over_b > rb + 1.0)
			continue;
		const bool cut_l = over_l > 0.5, cut_r = over_r > 0.5, cut_t = over_t > 0.5, cut_b = over_b > 0.5;
		if (!cut_l && !cut_r && !cut_t && !cut_b)
			continue;
		// The size the whole item may have, and how much smaller the picture
		// must be for that. A room in screen pixels keeps its size.
		const double may_w = box.w - std::max(0.0, over_l) - std::max(0.0, over_r);
		const double may_h = box.h - std::max(0.0, over_t) - std::max(0.0, over_b);
		const double pic_w = box.w - rl - rr, pic_h = box.h - rt - rb;
		double k;
		if (fixed && pic_w > 2.0 && pic_h > 2.0)
			k = std::min((may_w - rl - rr) / pic_w, (may_h - rt - rb) / pic_h);
		else
			k = std::min(may_w / box.w, may_h / box.h);
		if (!(k > 0.05) || k >= 1.0)
			continue;
		const double new_w = fixed ? pic_w * k + rl + rr : box.w * k;
		const double new_h = fixed ? pic_h * k + rt + rb : box.h * k;
		// Against the edge it ran over; centred where it ran over both, and
		// about its own centre where it ran over neither.
		const double new_x = cut_l && cut_r ? (cw - new_w) / 2.0
				     : cut_l        ? 0.0
				     : cut_r        ? cw - new_w
						    : box.x + (box.w - new_w) / 2.0;
		const double new_y = cut_t && cut_b ? (ch - new_h) / 2.0
				     : cut_t        ? 0.0
				     : cut_b        ? ch - new_h
						    : box.y + (box.h - new_h) / 2.0;
		// The item's position is somewhere on its box, by its alignment.
		const double ax = (pos.x - box.x) / box.w, ay = (pos.y - box.y) / box.h;
		obs_sceneitem_defer_update_begin(item);
		vec2_set(&scale, (float)(scale.x * k), (float)(scale.y * k));
		obs_sceneitem_set_scale(item, &scale);
		vec2_set(&pos, (float)(new_x + ax * new_w), (float)(new_y + ay * new_h));
		obs_sceneitem_set_pos(item, &pos);
		obs_sceneitem_defer_update_end(item);
		// What it did does not count as the item being moved.
		next[item].box = Box{new_x, new_y, new_w, new_h};
	}
	still_.swap(next);
}

void ShadowBackdrop::tick()
{
	obs_source_t *parent = obs_filter_get_parent(source_);
	if (!parent)
		return;
	bool keep, ambient;
	uint32_t ambient_choice;
	{
		std::lock_guard lock(mutex_);
		keep = keep_place_;
		ambient = settings_.outline && settings_.design.ambient_fill();
		ambient_choice = settings_.design.ambient;
	}
	// An Ambient Background fill: its background looked up here, not while
	// drawing (outline-ambient.hpp).
	if (ambient)
		outline_ambient_warm(ambient_choice, nullptr);
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
	// Nothing is adjusted while OBS loads its scenes: only some of them have
	// their items yet, and how big the source is drawn is not known, so the
	// room was given back to the items there were (as if drawn full size) and
	// then taken from all of them. An item in a scene loaded later got the
	// room a second time at every start: the owner's camera, 11 px larger
	// after each restart in two scenes of three.
	if (!scenes_loaded())
		return;
	// A filter that is switched off, or told not to, takes no room.
	// Now and then, see how big the source is drawn where it is on screen.
	// ==== scene-filters:perf ====
	if (scale_check_due(parent)) {
		// ==== end scene-filters:perf ====
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
		const double nx = found ? std::max(0.05, sx) : 1.0, ny = found ? std::max(0.05, sy) : 1.0;
		// ==== scene-filters:perf ====
		// In a bounding box the scale read depends on the room this
		// filter takes (the source is that much larger), and the room on
		// the scale: rounded to whole pixels, the two could take turns
		// between two values for good, moving every item showing the
		// source by a pixel and back three times a second. A scale that
		// goes straight back to the one before, by under 2 %, is that
		// and is not taken.
		const auto near = [](double a, double b, double share) {
			return std::fabs(a - b) <= share * std::max(a, b);
		};
		const bool back = near(nx, prev_sx_, 1e-4) && near(ny, prev_sy_, 1e-4) && near(nx, shown_sx_, 0.02) &&
				  near(ny, shown_sy_, 0.02) && !(nx == shown_sx_ && ny == shown_sy_);
		if (!back && !(nx == shown_sx_ && ny == shown_sy_)) {
			prev_sx_ = shown_sx_;
			prev_sy_ = shown_sy_;
			shown_sx_ = nx;
			shown_sy_ = ny;
		}
		// ==== end scene-filters:perf ====
	}
	const bool active = keep && effect_ && obs_source_enabled(source_);
	settle_items(parent, active ? padding_for(settings()) : Padding());
	bool in_view;
	{
		std::lock_guard lock(mutex_);
		in_view = in_view_;
	}
	// Looked at a few times a second: it only acts on an item that has been
	// still for a while, and going through every scene's items is not free.
	if (!(in_view && active)) {
		still_.clear();
		view_pending_ = true;
	} else if (++in_view_turn_ % 8 == 0 && view_check_due(parent)) {
		keep_in_view(parent);
	}
}

// ==== scene-filters:perf ====
// Every 20 ticks, as before, but only when something it depends on changed;
// otherwise every 120 ticks. The first look comes at once; after it, the
// filters' 20-tick rounds are spread by their uuid.
bool ShadowBackdrop::scale_check_due(obs_source_t *parent)
{
	if (--scale_countdown_ > 0)
		return false;
	scale_countdown_ = 20;
	if (!scale_staggered_) {
		scale_staggered_ = true;
		scale_countdown_ = 1 + stagger_phase(source_, 20);
		scale_idle_ = stagger_phase(source_, 120);
	}
	const uint64_t rev = scene_revision();
	scale_idle_ += 20;
	// The cheap tests first: a scene changed, or its settings.
	bool changed = scale_dirty_.exchange(false) || rev != seen_scene_rev_ || scale_idle_ >= 120;
	// Then another scene on screen, or another size of the source (which
	// a scene not drawn does not signal).
	obs_source_t *on_screen = scene_on_screen();
	obs_source_release(on_screen);
	changed = changed || on_screen != seen_on_screen_;
	uint32_t w = seen_w_, h = seen_h_;
	if (!changed) {
		w = obs_source_get_width(parent);
		h = obs_source_get_height(parent);
		changed = w != seen_w_ || h != seen_h_;
	}
	if (!changed)
		return false;
	if (w == seen_w_ && h == seen_h_) {
		w = obs_source_get_width(parent);
		h = obs_source_get_height(parent);
	}
	seen_scene_rev_ = rev;
	seen_on_screen_ = on_screen;
	seen_w_ = w;
	seen_h_ = h;
	scale_idle_ = 0;
	count_look(Look::ShadowScale);
	return true;
}

// "Keep it on screen" goes through every scene's items, eight ticks apart,
// but only while an item may yet need it: something in a scene changed, the
// source changed size, an item has not been still for long enough yet, or
// its settings changed. Otherwise every 120 ticks.
bool ShadowBackdrop::view_check_due(obs_source_t *parent)
{
	const uint64_t rev = scene_revision();
	const uint32_t w = obs_source_get_width(parent), h = obs_source_get_height(parent);
	view_idle_ += 8;
	if (!view_pending_ && !view_dirty_.exchange(false) && rev == view_rev_ && w == view_w_ && h == view_h_ &&
	    view_idle_ < 120)
		return false;
	view_rev_ = rev;
	view_w_ = w;
	view_h_ = h;
	view_idle_ = 0;
	return true;
}
// ==== end scene-filters:perf ====

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
		s.outline_corner_size /= both;
		s.design.dash = std::min(400.0, s.design.dash / both);
		s.design.gap = std::min(400.0, s.design.gap / both);
		s.pattern_shown = 1.0 / both;
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
	// (Afresh at each drawing: see filter-input.hpp.)
	return draw_filter_input(source_, tr_input_, w, h);
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
		skip_filter_input(source_);
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
	gs_texture_t *plain = nullptr; // the card with its line whole, when the line has a design
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
		// A line round the box goes round what shows of the source (a
		// widget's or a capture's see-through margins left out), or round
		// its panel when it has one.
		// (And a source that fills under nine tenths of it, text or a
		// shape, keeps the line that follows it.)
		Seen seen;
		if (s.outline_corners > 0 && rects_) {
			rects_->begin_frame();
			seen = rects_->measure(input, w, h);
		}
		const bool known = seen.known() && !s.backdrop;
		// (Every value the pass reads, each time it runs: an effect's
		// values last for one draw.)
		// A pattern fill: the pattern over the source and its room, in the
		// source's own space (where it is on screen is not known here),
		// sized as it would be on the canvas.
		gs_texture_t *pattern = nullptr;
		if (s.design.pattern() && s.outline_fill != Fill::Picture) {
			obs_video_info ovi;
			const double canvas = obs_get_video_info(&ovi) ? (double)ovi.base_height : 1080.0;
			pattern = outline_pattern_texture(s.design, s.outline_color, ow, oh,
							  canvas / (double)oh * s.pattern_shown);
		} else if (s.design.ambient_fill() && s.outline_fill != Fill::Picture) {
			// An Ambient Background fill: the background at the size the
			// canvas shows it, its middle on the source's middle.
			obs_video_info ovi;
			AmbientCover cover;
			if (obs_get_video_info(&ovi) && ovi.base_width && ovi.base_height) {
				cover.x = (double)ow / (s.pattern_shown * (double)ovi.base_width);
				cover.y = (double)oh / (s.pattern_shown * (double)ovi.base_height);
			}
			pattern = outline_ambient_texture(s.design, ow, oh, cover);
		}
		auto line_values = [&](const OutlineDesign &design) {
			vec4 outline_color;
			vec4_from_rgba(&outline_color, s.outline_color);
			outline_color.w = (float)s.outline_opacity;
			gs_effect_set_vec2(p_out_size_, &out_size);
			gs_effect_set_texture(p_extended_, extended);
			gs_effect_set_float(p_outline_width_, (float)s.outline_width);
			gs_effect_set_vec4(p_outline_color_, &outline_color);
			gs_effect_set_float(p_outline_picture_, s.outline_fill == Fill::Picture ? 1.0f : 0.0f);
			// (Square: a rounded corner of no size.)
			const bool square = s.outline_corners == 5;
			gs_effect_set_float(p_outline_corners_, square ? 1.0f : (float)s.outline_corners);
			gs_effect_set_float(p_corner_size_, square ? 0.0f : (float)s.outline_corner_size);
			// (The box: given again, an effect's values last for one draw.)
			gs_effect_set_vec2(p_src_size_, &src_size);
			gs_effect_set_vec2(p_pad_, &pad);
			gs_effect_set_float(p_backdrop_on_, s.backdrop ? 1.0f : 0.0f);
			gs_effect_set_float(p_backdrop_pad_, (float)s.backdrop_pad);
			gs_effect_set_texture(p_rect_tex_, known ? seen.rect : no_rect_);
			gs_effect_set_texture(p_area_tex_, known ? seen.area : no_rect_);
			gs_effect_set_float(p_rect_known_, known ? 1.0f : 0.0f);
			design_.set(design, design.pattern() || design.ambient_fill() ? pattern : nullptr, card);
		};
		// The shadow is thrown by the whole line: a design's gaps are not
		// holes in it. (An inner line changes no silhouette.)
		if (s.design.kind != OutlineDesign::Solid && !s.design.inner() && s.shadow) {
			line_values(OutlineDesign());
			draw(tr_plain_, card, "Outline");
			plain = gs_texrender_get_texture(tr_plain_);
		}
		line_values(s.design);
		draw(tr_body_, card, "Outline");
		if (gs_texture_t *body = gs_texrender_get_texture(tr_body_))
			card = body;
	}

	// 3. Its silhouette, softened, becomes the shadow.
	gs_texture_t *silhouette = plain ? plain : card;
	// (And, softened by itself, a Neon line's halo of light: one more blur,
	// only for a Neon line.)
	gs_texture_t *halo = nullptr;
	const double halo_reach = s.outline && s.outline_fill != Fill::Picture ? s.design.halo_reach(s.outline_width)
									       : 0.0;
	if (card && halo_reach > 0.0) {
		if (!halo_blur_)
			halo_blur_ = std::make_unique<KawaseBlur>(true);
		halo = halo_blur_->blur(silhouette, ow, oh, halo_reach * kHaloBlur);
		if (halo == silhouette)
			halo = nullptr;
	}
	if (card && s.shadow && s.softness > 0.0)
		silhouette = blur_->blur(silhouette, ow, oh, s.softness);

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
		gs_effect_set_float(p_intensity_, (float)s.intensity);
		gs_effect_set_float(p_out_linear_, gs_get_linear_srgb() ? 1.0f : 0.0f);
		design_.set_halo(s.design, s.outline_color, s.outline_opacity, halo, card);
		draw(tr_final_, card, "Compose");
	}

	gs_enable_framebuffer_srgb(previous_srgb);

	if (gs_texture_t *final_tex = card ? gs_texrender_get_texture(tr_final_) : nullptr) {
		gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *image = gs_effect_get_param_by_name(def, "image");
		// (It holds linear light already where OBS draws in it: a
		// sixteen-bit picture has no sRGB view to read it through.)
		const bool linear = gs_get_linear_srgb();
		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(linear);
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
		set_tip(fill, obs_module_text("Shadow.Fill.Tip"));
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
	set_tip(boost, obs_module_text("Shadow.Boost.Tip"));
	slider(shadow, "shadow_opacity", "Shadow.Opacity", 0.0, 100.0, "%");
	slider(shadow, "offset_x", "Shadow.OffsetX", -200.0, 200.0, " px");
	slider(shadow, "offset_y", "Shadow.OffsetY", -200.0, 200.0, " px");
	slider(shadow, "softness", "Shadow.Softness", 0.0, 200.0, " px");
	slider(shadow, "spread", "Shadow.Spread", 0.0, 100.0, "%");
	add_intensity_property(shadow);
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
	set_tip(edge, obs_module_text("Shadow.OutlineEdge.Tip"));
	obs_property_t *line_blur = slider(outline, "outline_blur", "Shadow.OutlineBlur", 0.0, 200.0, " px");
	set_tip(line_blur, obs_module_text("Shadow.OutlineBlur.Tip"));
	slider(outline, "outline_opacity", "Shadow.OutlineOpacity", 0.0, 100.0, "%");
	add_outline_corner_properties(outline);
	add_outline_design_properties(outline);
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
	set_tip(screen, obs_module_text("Shadow.ScreenPx.Tip"));
	obs_property_t *in_view = obs_properties_add_bool(props, "in_view", obs_module_text("Shadow.InView"));
	set_tip(in_view, obs_module_text("Shadow.InView.Tip"));
	obs_property_t *keep = obs_properties_add_bool(props, "keep_place", obs_module_text("Shadow.Keep"));
	set_tip(keep, obs_module_text("Shadow.Keep.Tip"));
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
	outline_glow_defaults(settings);
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
		double left = 0.0, top = 0.0, right = 0.0, bottom = 0.0;
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
				f->right += p.right;
				f->bottom += p.bottom;
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
	out.right = find.right;
	out.bottom = find.bottom;
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

bool shadow_kept_in_view(obs_source_t *source)
{
	bool kept = false;
	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			const char *id = obs_source_get_unversioned_id(filter);
			if (!id || strcmp(id, kShadowBackdropId) != 0 || !obs_source_enabled(filter))
				return;
			obs_data_t *settings = obs_source_get_settings(filter);
			if (obs_data_get_bool(settings, "in_view"))
				*static_cast<bool *>(param) = true;
			obs_data_release(settings);
		},
		&kept);
	return kept;
}

void register_shadow_backdrop()
{
	obs_source_info info = {};
	info.id = kShadowBackdropId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	// Colour-aware: OBS then tells the filter when it is drawing in linear
	// light (a window with a wide-colour surface) and the last draw follows
	// that. Without it the picture came out pale in the Filters window.
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
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
	register_timed(&info);
}

} // namespace stagekit
