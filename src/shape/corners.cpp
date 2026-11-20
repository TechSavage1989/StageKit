/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Corners & Edges: cuts a source's picture to a shape (rounded, cut, notched
// or scooped corners, trimmed and feathered edges, an outline) without
// changing its size, so everything after it in the filter list, and every
// scene item showing it, carries on as before.

#include "../common/filter-input.hpp"
#include "corners.hpp"
#include "../common/timing.hpp"

#include "../common/filter-order.hpp"
#include "../common/managed-filter.hpp"
#include "../common/scene-filter-ids.hpp"
#include "../common/shown-scale.hpp"
#include "../frame/auto-frame.hpp"
#include "../shadow/shadow-backdrop.hpp"
#include "../common/tooltip.hpp"

#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <plugin-support.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace stagekit {

namespace {

// Saved in settings: append only.
enum class Style { Round = 0, Cut = 1, Notch = 2, Scoop = 3 };

struct Settings {
	CornerShape shape;
	bool screen_px = true;
};

class Corners {
public:
	Corners(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		paint_ = std::make_unique<CornersPaint>();
		obs_enter_graphics();
		tr_input_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		obs_leave_graphics();
		update(settings);
	}

	~Corners()
	{
		paint_.reset();
		obs_enter_graphics();
		gs_texrender_destroy(tr_input_);
		obs_leave_graphics();
	}

	void update(obs_data_t *settings)
	{
		Settings s;
		s.shape = read_corner_shape(settings);
		s.screen_px = obs_data_get_bool(settings, "screen_px");
		std::lock_guard lock(mutex_);
		settings_ = s;
		placed_ = obs_data_get_bool(settings, "placed");
		managed_ = obs_data_get_bool(settings, "managed");
		managed_by_ = obs_data_get_string(settings, "managed_by");
	}

	// Made by a Scene Corners: the scene it names. False for one made by hand.
	bool managed(std::string &by)
	{
		std::lock_guard lock(mutex_);
		by = managed_by_;
		return managed_;
	}
	obs_source_t *source() const { return source_; }

	void tick()
	{
		obs_source_t *parent = obs_filter_get_parent(source_);
		if (!parent)
			return;
		bool placed;
		{
			std::lock_guard lock(mutex_);
			placed = placed_;
		}
		// Added after a Shadow & Backdrop, it would cut the shadow off. The
		// first time only, it moves itself above the shadow; after that the
		// order is the user's to choose.
		if (!placed) {
			obs_source_t *shadow = first_filter(parent, kShadowBackdropId);
			for (int i = 0; shadow && i < 32; i++) {
				const int mine = applied_index(parent, source_), theirs = applied_index(parent, shadow);
				if (mine < 0 || theirs < 0 || mine < theirs)
					break;
				obs_source_filter_set_order(parent, source_, OBS_ORDER_MOVE_UP);
				if (applied_index(parent, source_) == mine)
					break;
			}
			obs_data_t *settings = obs_source_get_settings(source_);
			obs_data_set_bool(settings, "placed", true);
			obs_data_release(settings);
			std::lock_guard lock(mutex_);
			placed_ = true;
		}
		// Now and then, see how big the source is drawn where it is on screen.
		if (--scale_countdown_ <= 0) {
			scale_countdown_ = 20;
			double sx = 0.0, sy = 0.0;
			const bool found = shown_scale(parent, sx, sy);
			std::lock_guard lock(mutex_);
			shown_sx_ = found ? std::max(0.05, sx) : 1.0;
			shown_sy_ = found ? std::max(0.05, sy) : 1.0;
		}
	}

	void render()
	{
		obs_source_t *target = obs_filter_get_target(source_);
		obs_source_t *parent = obs_filter_get_parent(source_);
		const uint32_t w = target ? obs_source_get_base_width(target) : 0;
		const uint32_t h = target ? obs_source_get_base_height(target) : 0;
		if (!paint_->ready() || !parent || !w || !h) {
			skip_filter_input(source_);
			return;
		}
		Settings s;
		double kx, ky;
		{
			std::lock_guard lock(mutex_);
			s = settings_;
			kx = s.screen_px ? shown_sx_ : 1.0;
			ky = s.screen_px ? shown_sy_ : 1.0;
		}

		// The part to shape: the whole picture, or the game picture inside an
		// Auto Frame's frame when one comes before this filter.
		double rx = 0.0, ry = 0.0, rw = w, rh = h;
		if (obs_source_t *frame = first_filter(parent, kAutoFrameId)) {
			if (applied_index(parent, frame) < applied_index(parent, source_)) {
				calldata_t cd = {};
				if (proc_handler_call(obs_source_get_proc_handler(frame), "get_picture", &cd)) {
					const double px = calldata_float(&cd, "x"), py = calldata_float(&cd, "y");
					const double pw = calldata_float(&cd, "width"),
						     ph = calldata_float(&cd, "height");
					if (pw >= 1.0 && ph >= 1.0) {
						rx = std::clamp(px, 0.0, (double)w);
						ry = std::clamp(py, 0.0, (double)h);
						rw = std::clamp(pw, 0.0, w - rx);
						rh = std::clamp(ph, 0.0, h - ry);
					}
				}
				calldata_free(&cd);
			}
		}

		// (Afresh at each drawing: see filter-input.hpp.)
		if (!draw_filter_input(source_, tr_input_, w, h))
			return;
		gs_texture_t *input = gs_texrender_get_texture(tr_input_);
		if (!input)
			return;

		// Sizes are in pixels as they look on screen, so the shape is worked
		// out in a space scaled like the item is drawn.
		if (paint_->shape(input, w, h, rx, ry, rw, rh, kx, ky, s.shape))
			paint_->draw_out(w, h);
	}

private:
	obs_source_t *source_;
	std::mutex mutex_;
	Settings settings_;
	bool placed_ = false;
	bool managed_ = false; // made by a Scene Corners, which pushes its look
	std::string managed_by_;
	double shown_sx_ = 1.0, shown_sy_ = 1.0;
	int scale_countdown_ = 0;

	std::unique_ptr<CornersPaint> paint_;
	gs_texrender_t *tr_input_ = nullptr;
};

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<Corners *>(data);
	obs_properties_t *props = obs_properties_create();
	// Made by a Scene Corners: it says so, and the look is set there.
	// Without one any more it works as one made by hand.
	std::string by;
	if (self && self->managed(by)) {
		obs_source_t *parent = obs_filter_get_parent(self->source());
		const ManagedState state = managed_state(kSceneCornersId, parent, by.c_str());
		obs_property_t *status = obs_properties_add_text(
			props, "status",
			state.owner_found ? text_with("Corners.Status.Managed", state.owner_scene).c_str()
					  : obs_module_text("Corners.Status.Orphan"),
			OBS_TEXT_INFO);
		obs_property_text_set_info_word_wrap(status, true);
		if (state.owner_found)
			return props;
	}
	corners_add_look_properties(props);
	add_help_line(props, obs_module_text("Corners.Help"));
	return props;
}

} // namespace

void corners_add_look_properties(obs_properties_t *props)
{
	obs_property_t *style = obs_properties_add_list(props, "style", obs_module_text("Corners.Style"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(style, obs_module_text("Corners.Style.Round"), (int)Style::Round);
	obs_property_list_add_int(style, obs_module_text("Corners.Style.Cut"), (int)Style::Cut);
	obs_property_list_add_int(style, obs_module_text("Corners.Style.Notch"), (int)Style::Notch);
	obs_property_list_add_int(style, obs_module_text("Corners.Style.Scoop"), (int)Style::Scoop);
	obs_property_t *size =
		obs_properties_add_float_slider(props, "size", obs_module_text("Corners.Size"), 0.0, 500.0, 1.0);
	obs_property_float_set_suffix(size, " px");

	obs_properties_t *which = obs_properties_create();
	obs_properties_add_bool(which, "top_left", obs_module_text("Corners.TopLeft"));
	obs_properties_add_bool(which, "top_right", obs_module_text("Corners.TopRight"));
	obs_properties_add_bool(which, "bottom_left", obs_module_text("Corners.BottomLeft"));
	obs_properties_add_bool(which, "bottom_right", obs_module_text("Corners.BottomRight"));
	// (Not "which": Scene Corners has a list of that name, and OBS refuses a
	// second property of one name, dropping the four tick boxes with it.)
	obs_properties_add_group(props, "corners_which", obs_module_text("Corners.Which"), OBS_GROUP_NORMAL, which);

	obs_properties_t *edges = obs_properties_create();
	obs_property_t *inset =
		obs_properties_add_float_slider(edges, "inset", obs_module_text("Corners.Inset"), 0.0, 500.0, 1.0);
	obs_property_float_set_suffix(inset, " px");
	set_tip(inset, obs_module_text("Corners.Inset.Tip"));
	obs_property_t *soft =
		obs_properties_add_float_slider(edges, "soft", obs_module_text("Corners.Soft"), 0.0, 200.0, 1.0);
	obs_property_float_set_suffix(soft, " px");
	set_tip(soft, obs_module_text("Corners.Soft.Tip"));
	obs_properties_add_group(props, "edges", obs_module_text("Corners.Edges"), OBS_GROUP_NORMAL, edges);

	obs_properties_t *line = obs_properties_create();
	obs_property_t *border =
		obs_properties_add_float_slider(line, "border", obs_module_text("Corners.Border"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(border, " px");
	obs_properties_add_color(line, "border_color", obs_module_text("Corners.BorderColor"));
	obs_property_t *opacity = obs_properties_add_float_slider(
		line, "border_opacity", obs_module_text("Corners.BorderOpacity"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(opacity, "%");
	obs_properties_add_group(props, "line", obs_module_text("Corners.Line"), OBS_GROUP_NORMAL, line);

	obs_property_t *screen = obs_properties_add_bool(props, "screen_px", obs_module_text("Corners.ScreenPx"));
	set_tip(screen, obs_module_text("Corners.ScreenPx.Tip"));
	set_tips_where_missing(props, {{"style", obs_module_text("Corners.Style.Tip")},
				       {"size", obs_module_text("Corners.Size.Tip")},
				       {"top_left", obs_module_text("Corners.Which.Tip")},
				       {"top_right", obs_module_text("Corners.Which.Tip")},
				       {"bottom_left", obs_module_text("Corners.Which.Tip")},
				       {"bottom_right", obs_module_text("Corners.Which.Tip")},
				       {"border", obs_module_text("Corners.Border.Tip")},
				       {"border_color", obs_module_text("Corners.BorderLook.Tip")},
				       {"border_opacity", obs_module_text("Corners.BorderLook.Tip")}});
}

void corners_look_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "style", (int)Style::Round);
	obs_data_set_default_double(settings, "size", 40.0);
	obs_data_set_default_bool(settings, "top_left", true);
	obs_data_set_default_bool(settings, "top_right", true);
	obs_data_set_default_bool(settings, "bottom_left", true);
	obs_data_set_default_bool(settings, "bottom_right", true);
	obs_data_set_default_double(settings, "inset", 0.0);
	obs_data_set_default_double(settings, "soft", 0.0);
	obs_data_set_default_double(settings, "border", 0.0);
	obs_data_set_default_int(settings, "border_color", 0xFFFFFFFF);
	obs_data_set_default_double(settings, "border_opacity", 100.0);
	obs_data_set_default_bool(settings, "screen_px", true);
}

CornerShape read_corner_shape(obs_data_t *settings)
{
	CornerShape s;
	s.style = std::clamp((int)obs_data_get_int(settings, "style"), 0, 3);
	const double size = std::clamp(obs_data_get_double(settings, "size"), 0.0, 2000.0);
	const char *keys[4] = {"top_left", "top_right", "bottom_right", "bottom_left"};
	for (int i = 0; i < 4; i++)
		s.radii[i] = obs_data_get_bool(settings, keys[i]) ? size : 0.0;
	s.inset = std::clamp(obs_data_get_double(settings, "inset"), 0.0, 2000.0);
	s.soft = std::clamp(obs_data_get_double(settings, "soft"), 0.0, 500.0);
	s.border = std::clamp(obs_data_get_double(settings, "border"), 0.0, 500.0);
	s.border_color = (uint32_t)obs_data_get_int(settings, "border_color");
	s.border_opacity = std::clamp(obs_data_get_double(settings, "border_opacity") / 100.0, 0.0, 1.0);
	return s;
}

CornerShape lerp_corner_shape(const CornerShape *a, const CornerShape *b, double t)
{
	t = std::clamp(t, 0.0, 1.0);
	if (!a && !b)
		return CornerShape();
	// A missing side: the other side's shape at no size.
	auto none = [](CornerShape x) {
		for (double &r : x.radii)
			r = 0.0;
		x.inset = x.soft = x.border = 0.0;
		return x;
	};
	const CornerShape from = a ? *a : none(*b), to = b ? *b : none(*a);
	auto mix = [t](double x, double y) {
		return x + (y - x) * t;
	};
	CornerShape out;
	out.style = !a ? to.style : !b ? from.style : t < 0.5 ? from.style : to.style;
	for (int i = 0; i < 4; i++)
		out.radii[i] = mix(from.radii[i], to.radii[i]);
	out.inset = mix(from.inset, to.inset);
	out.soft = mix(from.soft, to.soft);
	out.border = mix(from.border, to.border);
	// (A line that is not there on one side keeps the other's colour.)
	const uint32_t ca = from.border > 0.0 || !(to.border > 0.0) ? from.border_color : to.border_color;
	const uint32_t cb = to.border > 0.0 || !(from.border > 0.0) ? to.border_color : from.border_color;
	out.border_color = 0;
	for (int shift = 0; shift < 32; shift += 8) {
		const double x = (double)((ca >> shift) & 0xFF), y = (double)((cb >> shift) & 0xFF);
		out.border_color |= (uint32_t)std::lround(std::clamp(mix(x, y), 0.0, 255.0)) << shift;
	}
	out.border_opacity = mix(from.border_opacity, to.border_opacity);
	return out;
}

CornersPaint::CornersPaint()
{
	char *path = obs_module_file("effects/stagekit-corners.effect");
	obs_enter_graphics();
	char *errors = nullptr;
	effect_ = path ? gs_effect_create_from_file(path, &errors) : nullptr;
	if (!effect_)
		obs_log(LOG_ERROR, "Corners & Edges: failed to load effect: %s", errors ? errors : "not found");
	bfree(errors);
	if (effect_) {
		p_image_ = gs_effect_get_param_by_name(effect_, "image");
		p_size_ = gs_effect_get_param_by_name(effect_, "size");
		p_rect_ = gs_effect_get_param_by_name(effect_, "rect");
		p_radii_ = gs_effect_get_param_by_name(effect_, "radii");
		p_style_ = gs_effect_get_param_by_name(effect_, "style");
		p_soft_ = gs_effect_get_param_by_name(effect_, "soft");
		p_border_ = gs_effect_get_param_by_name(effect_, "border");
		p_border_color_ = gs_effect_get_param_by_name(effect_, "border_color");
	}
	out_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();
	bfree(path);
}

CornersPaint::~CornersPaint()
{
	obs_enter_graphics();
	gs_effect_destroy(effect_);
	gs_texrender_destroy(out_);
	obs_leave_graphics();
}

bool CornersPaint::shape(gs_texture_t *input, uint32_t w, uint32_t h, double rx, double ry, double rw, double rh,
			 double kx, double ky, const CornerShape &s)
{
	drawn_ = false;
	if (!effect_ || !input || !w || !h)
		return false;
	vec2 size;
	vec2_set(&size, (float)(w * kx), (float)(h * ky));
	const double inset = std::max(0.0, std::min({s.inset, rw * kx / 2.0 - 0.5, rh * ky / 2.0 - 0.5}));
	vec4 rect;
	vec4_set(&rect, (float)(rx * kx + inset), (float)(ry * ky + inset), (float)(rw * kx - 2.0 * inset),
		 (float)(rh * ky - 2.0 * inset));
	vec4 radii;
	vec4_set(&radii, (float)s.radii[0], (float)s.radii[1], (float)s.radii[2], (float)s.radii[3]);
	vec4 border_color;
	vec4_from_rgba(&border_color, s.border_color);
	border_color.w = (float)s.border_opacity;

	// The shaping pass works on raw colour values.
	const bool previous_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_texrender_reset(out_);
	if (gs_texrender_begin(out_, w, h)) {
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		gs_effect_set_texture(p_image_, input);
		gs_effect_set_vec2(p_size_, &size);
		gs_effect_set_vec4(p_rect_, &rect);
		gs_effect_set_vec4(p_radii_, &radii);
		gs_effect_set_float(p_style_, (float)s.style);
		gs_effect_set_float(p_soft_, (float)s.soft);
		gs_effect_set_float(p_border_, (float)s.border);
		gs_effect_set_vec4(p_border_color_, &border_color);
		while (gs_effect_loop(effect_, "Shape"))
			gs_draw_sprite(input, 0, w, h);
		gs_blend_state_pop();
		gs_texrender_end(out_);
		drawn_ = true;
	}
	gs_enable_framebuffer_srgb(previous_srgb);
	return drawn_;
}

void CornersPaint::draw_out(uint32_t w, uint32_t h)
{
	gs_texture_t *out = drawn_ ? gs_texrender_get_texture(out_) : nullptr;
	if (!out)
		return;
	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(def, "image");
	const bool linear = gs_get_linear_srgb();
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(linear);
	if (linear)
		gs_effect_set_texture_srgb(image, out);
	else
		gs_effect_set_texture(image, out);
	while (gs_effect_loop(def, "Draw"))
		gs_draw_sprite(out, 0, w, h);
	gs_enable_framebuffer_srgb(previous);
}

namespace {

void get_defaults(obs_data_t *settings)
{
	corners_look_defaults(settings);
	obs_data_set_default_bool(settings, "placed", false);
	obs_data_set_default_bool(settings, "managed", false);
	obs_data_set_default_string(settings, "managed_by", "");
}

} // namespace

void register_corners()
{
	obs_source_info info = {};
	info.id = kCornersId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = [](void *) {
		return obs_module_text("Corners.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new Corners(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<Corners *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<Corners *>(data)->update(settings);
	};
	info.get_defaults = get_defaults;
	info.get_properties = get_properties;
	info.video_tick = [](void *data, float) {
		static_cast<Corners *>(data)->tick();
	};
	info.video_render = [](void *data, gs_effect_t *) {
		static_cast<Corners *>(data)->render();
	};
	register_timed(&info);
}

} // namespace stagekit
