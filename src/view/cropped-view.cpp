/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Cropped View: shows part of another source as a source of its own.
//
// One camera is often wanted in several shapes: a square in one layout, a
// wide strip in another, the whole picture in a third. An item's crop is
// applied after the source's filters, so a shadow or rounded corners on the
// camera would be cut off by it; the usual way round that is a small scene
// per shape. A Cropped View is that shape as a plain source: it draws the
// chosen part of the camera, and filters added to the view (Shadow &
// Backdrop, Ramp Zoom) follow the crop. The camera itself is captured once.

#include "cropped-view.hpp"
#include "../common/timing.hpp"

#include <obs-module.h>
#include <graphics/vec4.h>
#include <plugin-support.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>

namespace stagekit {

namespace {

enum class CropTo : int {
	Amounts = 0, // the four numbers below
	Square = 1,  // the middle of the picture, in a shape
	R4x3 = 2,
	R16x9 = 3,
	R3x4 = 4,
	R9x16 = 5,
	Drawn = 6, // whatever is drawn: the see-through edges are trimmed off, and it follows changes
};

struct Area {
	uint32_t x = 0, y = 0, w = 0, h = 0;
};

class CroppedView {
public:
	CroppedView(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		obs_enter_graphics();
		render_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		obs_leave_graphics();
		update(settings);
	}

	~CroppedView()
	{
		set_target(nullptr);
		obs_enter_graphics();
		gs_texrender_destroy(render_);
		gs_texrender_destroy(look_render_);
		gs_stagesurface_destroy(look_stage_);
		obs_leave_graphics();
	}

	void update(obs_data_t *settings)
	{
		std::lock_guard lock(mutex_);
		name_ = obs_data_get_string(settings, "source");
		crop_to_ = static_cast<CropTo>(obs_data_get_int(settings, "crop_to"));
		left_ = (uint32_t)std::max<long long>(0, obs_data_get_int(settings, "left"));
		right_ = (uint32_t)std::max<long long>(0, obs_data_get_int(settings, "right"));
		top_ = (uint32_t)std::max<long long>(0, obs_data_get_int(settings, "top"));
		bottom_ = (uint32_t)std::max<long long>(0, obs_data_get_int(settings, "bottom"));
		look_timer_ = 0.0;
		timer_ = 0.0;
	}

	// The source may be created after this one when a collection loads, or
	// be renamed, so it is looked up by name now and then.
	void tick(float seconds)
	{
		std::string name;
		{
			std::lock_guard lock(mutex_);
			look_timer_ -= seconds;
			timer_ -= seconds;
			if (timer_ > 0.0)
				return;
			timer_ = 0.5;
			name = name_;
		}
		obs_source_t *found = name.empty() ? nullptr : obs_get_source_by_name(name.c_str());
		if (found == source_) { // a view of itself would never finish drawing
			obs_source_release(found);
			found = nullptr;
		}
		set_target(found);
		obs_source_release(found);
	}

	obs_source_t *source() const { return source_; }

	obs_source_t *target()
	{
		std::lock_guard lock(mutex_);
		return target_ ? obs_weak_source_get_source(target_) : nullptr;
	}

	// The part of the source that is shown.
	Area area()
	{
		obs_source_t *source = target();
		const uint32_t w = source ? obs_source_get_width(source) : 0;
		const uint32_t h = source ? obs_source_get_height(source) : 0;
		obs_source_release(source);
		Area a;
		if (!w || !h)
			return a;
		std::lock_guard lock(mutex_);
		if (crop_to_ == CropTo::Drawn) {
			// Until something has been seen, the whole source.
			if (!drawn_.w || !drawn_.h)
				return {0, 0, w, h};
			a.x = std::min(drawn_.x, w - 1);
			a.y = std::min(drawn_.y, h - 1);
			a.w = std::min(drawn_.w, w - a.x);
			a.h = std::min(drawn_.h, h - a.y);
			return a;
		}
		if (crop_to_ == CropTo::Amounts) {
			a.x = std::min(left_, w - 1);
			a.y = std::min(top_, h - 1);
			a.w = (uint32_t)std::max<long long>(1, (long long)w - a.x - right_);
			a.h = (uint32_t)std::max<long long>(1, (long long)h - a.y - bottom_);
			return a;
		}
		double shape = 1.0;
		switch (crop_to_) {
		case CropTo::R4x3:
			shape = 4.0 / 3.0;
			break;
		case CropTo::R16x9:
			shape = 16.0 / 9.0;
			break;
		case CropTo::R3x4:
			shape = 3.0 / 4.0;
			break;
		case CropTo::R9x16:
			shape = 9.0 / 16.0;
			break;
		default:
			break;
		}
		// The largest piece of that shape, taken from the middle.
		if ((double)w / (double)h > shape) {
			a.h = h;
			a.w = std::max<uint32_t>(1, (uint32_t)std::lround(h * shape));
		} else {
			a.w = w;
			a.h = std::max<uint32_t>(1, (uint32_t)std::lround(w / shape));
		}
		a.x = (w - a.w) / 2;
		a.y = (h - a.h) / 2;
		return a;
	}

	void render()
	{
		if (rendering_)
			return; // the source being viewed contains this view
		obs_source_t *source = target();
		const Area a = area();
		if (!source || !a.w || !a.h) {
			obs_source_release(source);
			return;
		}
		rendering_ = true;
		if (look_due())
			look_for_drawn_part(source);
		gs_texrender_reset(render_);
		if (gs_texrender_begin(render_, a.w, a.h)) {
			vec4 clear;
			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			// Only the chosen part of the source lands on the texture.
			gs_ortho((float)a.x, (float)(a.x + a.w), (float)a.y, (float)(a.y + a.h), -100.0f, 100.0f);
			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
			obs_source_video_render(source);
			gs_blend_state_pop();
			gs_texrender_end(render_);
		}
		rendering_ = false;
		obs_source_release(source);

		gs_texture_t *texture = gs_texrender_get_texture(render_);
		if (!texture)
			return;
		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
		const bool linear = gs_get_linear_srgb();
		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(linear);
		if (linear)
			gs_effect_set_texture_srgb(image, texture);
		else
			gs_effect_set_texture(image, texture);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA); // the texture is premultiplied
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(texture, 0, a.w, a.h);
		gs_blend_state_pop();
		gs_enable_framebuffer_srgb(previous);
	}

private:
	bool look_due()
	{
		std::lock_guard lock(mutex_);
		if (crop_to_ != CropTo::Drawn || look_timer_ > 0.0)
			return false;
		look_timer_ = kLookEvery;
		return true;
	}

	// Finds the part of the source with something drawn in it. The source is
	// drawn small and read back, about once a second and only in this mode.
	// The view grows to take in new content at once and shrinks only when the
	// smaller picture has been seen twice, so an animation does not make it
	// flutter. A source showing nothing keeps the size it had.
	void look_for_drawn_part(obs_source_t *source)
	{
		const uint32_t sw = obs_source_get_width(source), sh = obs_source_get_height(source);
		if (!sw || !sh)
			return;
		const double shrink = std::min(1.0, 320.0 / std::max(sw, sh));
		const uint32_t w = std::max<uint32_t>(1, (uint32_t)std::lround(sw * shrink));
		const uint32_t h = std::max<uint32_t>(1, (uint32_t)std::lround(sh * shrink));
		if (!look_render_)
			look_render_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		if (!look_stage_ || look_w_ != w || look_h_ != h) {
			gs_stagesurface_destroy(look_stage_);
			look_stage_ = gs_stagesurface_create(w, h, GS_RGBA);
			look_w_ = w;
			look_h_ = h;
		}
		gs_texrender_reset(look_render_);
		if (!gs_texrender_begin(look_render_, w, h))
			return;
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		obs_source_video_render(source);
		gs_blend_state_pop();
		gs_texrender_end(look_render_);
		gs_stage_texture(look_stage_, gs_texrender_get_texture(look_render_));
		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		if (!gs_stagesurface_map(look_stage_, &data, &linesize))
			return;
		uint32_t min_x = w, min_y = h, max_x = 0, max_y = 0;
		for (uint32_t y = 0; y < h; y++) {
			const uint8_t *row = data + (size_t)y * linesize;
			for (uint32_t x = 0; x < w; x++) {
				if (row[x * 4 + 3] <= 12)
					continue;
				min_x = std::min(min_x, x);
				max_x = std::max(max_x, x);
				min_y = std::min(min_y, y);
				max_y = std::max(max_y, y);
			}
		}
		gs_stagesurface_unmap(look_stage_);
		if (max_x < min_x || max_y < min_y)
			return; // nothing showing: keep the last size

		// Back to the source's pixels, one small-picture pixel generous.
		Area seen;
		seen.x = (uint32_t)std::floor(min_x / shrink);
		seen.y = (uint32_t)std::floor(min_y / shrink);
		seen.w = std::min(sw, (uint32_t)std::ceil((max_x + 1) / shrink)) - seen.x;
		seen.h = std::min(sh, (uint32_t)std::ceil((max_y + 1) / shrink)) - seen.y;

		std::lock_guard lock(mutex_);
		const bool first = !drawn_.w || !drawn_.h;
		const uint32_t l = std::min(drawn_.x, seen.x), t = std::min(drawn_.y, seen.y);
		const uint32_t r = std::max(drawn_.x + drawn_.w, seen.x + seen.w);
		const uint32_t b = std::max(drawn_.y + drawn_.h, seen.y + seen.h);
		const bool same_as_last = seen.x == last_seen_.x && seen.y == last_seen_.y && seen.w == last_seen_.w &&
					  seen.h == last_seen_.h;
		if (first || same_as_last)
			drawn_ = seen; // settled: take it as it is, smaller or not
		else
			drawn_ = {l, t, r - l, b - t}; // still changing: only ever grow
		last_seen_ = seen;
	}

	// Tells OBS which source this one shows, so a camera or capture is
	// switched on while the view is on screen and released afterwards.
	void set_target(obs_source_t *source)
	{
		obs_weak_source_t *weak = source ? obs_source_get_weak_source(source) : nullptr;
		obs_weak_source_t *old;
		{
			std::lock_guard lock(mutex_);
			old = target_;
			target_ = weak;
		}
		if (!obs_weak_source_references_source(old, source)) {
			if (obs_source_t *old_source = old ? obs_weak_source_get_source(old) : nullptr) {
				obs_source_remove_active_child(source_, old_source);
				obs_source_release(old_source);
			}
			if (source)
				obs_source_add_active_child(source_, source);
		}
		obs_weak_source_release(old);
	}

	obs_source_t *source_;
	std::mutex mutex_;
	std::string name_;
	CropTo crop_to_ = CropTo::Amounts;
	uint32_t left_ = 0, right_ = 0, top_ = 0, bottom_ = 0;
	double timer_ = 0.0;
	obs_weak_source_t *target_ = nullptr;
	gs_texrender_t *render_ = nullptr;
	bool rendering_ = false;
	// "Whatever is drawn": the part found, and what the last look saw.
	static constexpr double kLookEvery = 1.0;
	Area drawn_, last_seen_;
	double look_timer_ = 0.0;
	gs_texrender_t *look_render_ = nullptr;
	gs_stagesurf_t *look_stage_ = nullptr;
	uint32_t look_w_ = 0, look_h_ = 0;
};

bool crop_to_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool amounts = static_cast<CropTo>(obs_data_get_int(settings, "crop_to")) == CropTo::Amounts;
	for (const char *name : {"left", "right", "top", "bottom"})
		obs_property_set_visible(obs_properties_get(props, name), amounts);
	return true;
}

obs_properties_t *get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *source = obs_properties_add_list(props, "source", obs_module_text("View.Source"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(source, obs_module_text("View.Source.None"), "");
	struct Fill {
		obs_property_t *list;
		void *self;
	} fill{source, data};
	const auto add = [](void *param, obs_source_t *candidate) {
		auto *f = static_cast<Fill *>(param);
		const char *name = obs_source_get_name(candidate);
		const bool video = (obs_source_get_output_flags(candidate) & OBS_SOURCE_VIDEO) != 0;
		if (name && *name && video && obs_obj_get_data(candidate) != f->self)
			obs_property_list_add_string(f->list, name, name);
		return true;
	};
	obs_enum_sources(add, &fill);
	obs_enum_scenes(add, &fill);

	obs_property_t *crop_to = obs_properties_add_list(props, "crop_to", obs_module_text("View.CropTo"),
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(crop_to, obs_module_text("View.CropTo.Amounts"), (int)CropTo::Amounts);
	obs_property_list_add_int(crop_to, obs_module_text("View.CropTo.Drawn"), (int)CropTo::Drawn);
	obs_property_list_add_int(crop_to, obs_module_text("View.CropTo.Square"), (int)CropTo::Square);
	obs_property_list_add_int(crop_to, "4:3", (int)CropTo::R4x3);
	obs_property_list_add_int(crop_to, "16:9", (int)CropTo::R16x9);
	obs_property_list_add_int(crop_to, "3:4", (int)CropTo::R3x4);
	obs_property_list_add_int(crop_to, "9:16", (int)CropTo::R9x16);
	obs_property_set_modified_callback(crop_to, crop_to_modified);
	for (const char *name : {"left", "right", "top", "bottom"}) {
		const std::string key = std::string("View.") + name;
		obs_property_t *amount = obs_properties_add_int(props, name, obs_module_text(key.c_str()), 0, 16384, 1);
		obs_property_int_set_suffix(amount, " px");
	}
	obs_property_t *help = obs_properties_add_text(props, "help", obs_module_text("View.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(help, true);
	return props;
}

} // namespace

void register_cropped_view()
{
	obs_source_info info = {};
	info.id = kCroppedViewId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	info.icon_type = OBS_ICON_TYPE_CAMERA;
	info.get_name = [](void *) {
		return obs_module_text("View.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new CroppedView(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<CroppedView *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<CroppedView *>(data)->update(settings);
	};
	info.get_defaults = [](obs_data_t *settings) {
		obs_data_set_default_int(settings, "crop_to", (int)CropTo::Amounts);
	};
	info.get_properties = get_properties;
	info.get_width = [](void *data) {
		return static_cast<CroppedView *>(data)->area().w;
	};
	info.get_height = [](void *data) {
		return static_cast<CroppedView *>(data)->area().h;
	};
	info.video_tick = [](void *data, float seconds) {
		static_cast<CroppedView *>(data)->tick(seconds);
	};
	info.video_render = [](void *data, gs_effect_t *) {
		static_cast<CroppedView *>(data)->render();
	};
	info.enum_active_sources = [](void *data, obs_source_enum_proc_t callback, void *param) {
		auto *self = static_cast<CroppedView *>(data);
		if (obs_source_t *source = self->target()) {
			callback(self->source(), source, param);
			obs_source_release(source);
		}
	};
	register_timed(&info);
}

} // namespace stagekit
