/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Standby Picture: a picture in a capture's place while the capture has
// nothing to show, so its spot in the scene is not an empty hole.
//
// "Nothing to show" means the capture has no size: no game or window is
// attached to it. A capture that is attached and showing a black picture is
// something, and is left alone.
//
// The picture is the start image unless another is chosen. It fills the size
// the capture had when it last showed something (or the size a filter before
// this one gives it, such as an Auto Frame's frame), so nothing in the scene
// moves when the capture arrives; then it fades to the capture.

#include "standby.hpp"

#include "../common/start-image.hpp"
#include "../frame/auto-frame.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>

namespace stagekit {

namespace {

enum class Picture : int {
	StartImage = 0, // saved: append only
	File = 1,
	Source = 2,
};

// Where a filter comes in the order its source applies them, or -1.
int applied_index(obs_source_t *parent, obs_source_t *filter)
{
	struct Find {
		obs_source_t *wanted;
		int at, found;
	} find{filter, 0, -1};
	obs_source_enum_filters(
		parent,
		[](obs_source_t *, obs_source_t *f, void *param) {
			auto *x = static_cast<Find *>(param);
			if (f == x->wanted)
				x->found = x->at;
			x->at++;
		},
		&find);
	return find.found;
}

obs_source_t *filter_of_kind(obs_source_t *parent, const char *id)
{
	struct Find {
		const char *id;
		obs_source_t *found;
	} find{id, nullptr};
	obs_source_enum_filters(
		parent,
		[](obs_source_t *, obs_source_t *f, void *param) {
			auto *x = static_cast<Find *>(param);
			const char *fid = obs_source_get_unversioned_id(f);
			if (!x->found && fid && strcmp(fid, x->id) == 0)
				x->found = f;
		},
		&find);
	return find.found;
}

bool is_video_file(const std::string &path)
{
	const size_t dot = path.rfind('.');
	if (dot == std::string::npos)
		return false;
	std::string ext = path.substr(dot + 1);
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	for (const char *video : {"mp4", "mov", "mkv", "webm", "avi", "m4v", "flv", "ts", "gif"})
		if (ext == video)
			return true;
	return false;
}

// The size of what comes before a filter. For the capture itself that is its
// own size: asking for its width would come back to this filter.
uint32_t before_width(obs_source_t *filter)
{
	obs_source_t *target = obs_filter_get_target(filter), *parent = obs_filter_get_parent(filter);
	if (!target)
		return 0;
	return target == parent ? obs_source_get_base_width(target) : obs_source_get_width(target);
}
uint32_t before_height(obs_source_t *filter)
{
	obs_source_t *target = obs_filter_get_target(filter), *parent = obs_filter_get_parent(filter);
	if (!target)
		return 0;
	return target == parent ? obs_source_get_base_height(target) : obs_source_get_height(target);
}

class Standby {
public:
	Standby(obs_data_t *settings, obs_source_t *source) : source_(source) { update(settings); }

	~Standby()
	{
		let_go();
		obs_enter_graphics();
		gs_texrender_destroy(render_);
		gs_effect_destroy(effect_);
		obs_leave_graphics();
	}

	void update(obs_data_t *settings)
	{
		std::lock_guard lock(mutex_);
		kind_ = static_cast<Picture>(std::clamp((int)obs_data_get_int(settings, "picture"), 0, 2));
		file_ = obs_data_get_string(settings, "file");
		named_ = obs_data_get_string(settings, "source");
		fade_s_ = std::clamp<long long>(obs_data_get_int(settings, "fade_ms"), 0, 5000) / 1000.0;
		last_w_ = (uint32_t)std::max<long long>(0, obs_data_get_int(settings, "last_width"));
		last_h_ = (uint32_t)std::max<long long>(0, obs_data_get_int(settings, "last_height"));
		placed_ = obs_data_get_bool(settings, "placed");
		changed_ = true;
	}

	void tick(float seconds)
	{
		obs_source_t *parent = obs_filter_get_parent(source_);
		obs_source_t *target = obs_filter_get_target(source_);
		if (!parent || !target)
			return;
		Picture kind;
		std::string file, named;
		bool changed, placed;
		double fade_s;
		{
			std::lock_guard lock(mutex_);
			kind = kind_;
			file = file_;
			named = named_;
			changed = changed_;
			changed_ = false;
			placed = placed_;
			fade_s = fade_s_;
		}
		// Its place among the filters, the first time only: straight after an
		// Auto Frame, so the picture fills the frame, and otherwise first, so
		// corners, shadows and the rest are put on the picture too.
		if (!placed) {
			obs_source_t *frame = filter_of_kind(parent, kAutoFrameId);
			for (int i = 0; i < 32; i++) {
				const int mine = applied_index(parent, source_);
				const int wanted = frame ? applied_index(parent, frame) + 1 : 0;
				if (mine < 0 || mine <= wanted)
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
		if (changed)
			make_picture(kind, file, named);

		// Nothing to show: the capture itself has no size.
		const bool empty = obs_source_get_base_width(parent) == 0 || obs_source_get_base_height(parent) == 0;
		const uint32_t tw = before_width(source_), th = before_height(source_);
		if (!empty && tw && th && (tw != last_w_ || th != last_h_)) {
			// Remembered, for the next time there is nothing.
			obs_data_t *settings = obs_source_get_settings(source_);
			obs_data_set_int(settings, "last_width", tw);
			obs_data_set_int(settings, "last_height", th);
			obs_data_release(settings);
			std::lock_guard lock(mutex_);
			last_w_ = tw;
			last_h_ = th;
		}
		const double want = empty ? 1.0 : 0.0;
		const double step = fade_s > 0.0 ? seconds / fade_s : 1.0;
		double mix;
		{
			std::lock_guard lock(mutex_);
			// It is there at once when there is nothing; it fades away when
			// the capture arrives.
			mix_ = empty ? 1.0 : std::max(want, mix_ - step);
			mix = mix_;
			empty_ = empty;
		}
		// The picture only runs while it is wanted.
		const bool wanted = mix > 0.0 && obs_source_showing(parent);
		if (wanted != holding_ && picture_) {
			if (wanted) {
				obs_source_inc_showing(picture_);
				obs_source_inc_active(picture_);
			} else {
				obs_source_dec_active(picture_);
				obs_source_dec_showing(picture_);
			}
			holding_ = wanted;
		}
	}

	uint32_t width() { return size(true); }
	uint32_t height() { return size(false); }

	void render()
	{
		double mix;
		bool empty;
		{
			std::lock_guard lock(mutex_);
			mix = mix_;
			empty = empty_;
		}
		// The capture, when there is one.
		if (!empty || mix <= 0.0)
			obs_source_skip_video_filter(source_);
		obs_source_t *picture = picture_;
		const uint32_t w = width(), h = height();
		const uint32_t pw = picture ? obs_source_get_width(picture) : 0;
		const uint32_t ph = picture ? obs_source_get_height(picture) : 0;
		if (mix <= 0.0 || !w || !h || !pw || !ph)
			return;
		if (!effect_) {
			char *path = obs_module_file("effects/stagekit-fade.effect");
			char *errors = nullptr;
			effect_ = path ? gs_effect_create_from_file(path, &errors) : nullptr;
			if (!effect_)
				obs_log(LOG_ERROR, "Standby Picture: could not load its effect: %s",
					errors ? errors : "file not found");
			bfree(errors);
			bfree(path);
			if (!effect_)
				return;
		}
		if (!render_)
			render_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		gs_texrender_reset(render_);
		// The picture, filling the size: as large as needed to cover it,
		// centred, with what does not fit cut off.
		const bool was_linear = gs_set_linear_srgb(false);
		const bool was_srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(false);
		bool drawn = false;
		if (gs_texrender_begin(render_, w, h)) {
			vec4 clear;
			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
			const float scale = std::max((float)w / (float)pw, (float)h / (float)ph);
			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
			gs_matrix_push();
			gs_matrix_translate3f(((float)w - (float)pw * scale) / 2.0f,
					      ((float)h - (float)ph * scale) / 2.0f, 0.0f);
			gs_matrix_scale3f(scale, scale, 1.0f);
			obs_source_video_render(picture);
			gs_matrix_pop();
			gs_blend_state_pop();
			gs_texrender_end(render_);
			drawn = true;
		}
		gs_enable_framebuffer_srgb(was_srgb);
		gs_set_linear_srgb(was_linear);
		gs_texture_t *texture = drawn ? gs_texrender_get_texture(render_) : nullptr;
		if (!texture)
			return;
		gs_eparam_t *image = gs_effect_get_param_by_name(effect_, "image");
		gs_effect_set_float(gs_effect_get_param_by_name(effect_, "opacity"), (float)mix);
		const bool linear = gs_get_linear_srgb();
		gs_enable_framebuffer_srgb(linear);
		if (linear)
			gs_effect_set_texture_srgb(image, texture);
		else
			gs_effect_set_texture(image, texture);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		while (gs_effect_loop(effect_, "Draw"))
			gs_draw_sprite(texture, 0, w, h);
		gs_blend_state_pop();
		gs_enable_framebuffer_srgb(was_srgb);
	}

	bool showing()
	{
		std::lock_guard lock(mutex_);
		return empty_;
	}
	obs_source_t *source() const { return source_; }

private:
	// The size the source has through this filter: what comes before it, when
	// that has a size, and otherwise what the capture last had.
	uint32_t size(bool across)
	{
		const uint32_t bw = before_width(source_), bh = before_height(source_);
		const uint32_t now = across ? bw : bh, other = across ? bh : bw;
		if (now && other)
			return now;
		std::lock_guard lock(mutex_);
		if (last_w_ && last_h_)
			return across ? last_w_ : last_h_;
		return across ? 1920 : 1080;
	}

	void let_go()
	{
		if (picture_ && holding_) {
			obs_source_dec_active(picture_);
			obs_source_dec_showing(picture_);
		}
		holding_ = false;
		obs_source_release(picture_);
		picture_ = nullptr;
	}

	// The source that draws the picture: one of our own for the start image
	// or a file, or the one named.
	void make_picture(Picture kind, const std::string &file, const std::string &named)
	{
		let_go();
		if (kind == Picture::Source) {
			obs_source_t *found = named.empty() ? nullptr : obs_get_source_by_name(named.c_str());
			// Not the capture this is on, which would draw itself for ever.
			if (found && found == obs_filter_get_parent(source_)) {
				obs_source_release(found);
				found = nullptr;
			}
			picture_ = found;
			return;
		}
		if (kind == Picture::File && !file.empty()) {
			obs_data_t *settings = obs_data_create();
			const bool video = is_video_file(file);
			obs_data_set_string(settings, video ? "local_file" : "file", file.c_str());
			if (video) {
				obs_data_set_bool(settings, "is_local_file", true);
				obs_data_set_bool(settings, "looping", true);
				obs_data_set_bool(settings, "restart_on_activate", false);
			}
			picture_ = obs_source_create_private(video ? "ffmpeg_source" : "image_source",
							     "Standby Picture", settings);
			obs_data_release(settings);
			if (picture_ && video)
				obs_source_set_muted(picture_, true);
			return;
		}
		picture_ = obs_source_create_private(kStartImageId, "Standby Picture", nullptr);
	}

	obs_source_t *source_;
	std::mutex mutex_;
	Picture kind_ = Picture::StartImage;
	std::string file_, named_;
	double fade_s_ = 0.3;
	uint32_t last_w_ = 0, last_h_ = 0;
	bool placed_ = false;
	bool changed_ = true;
	double mix_ = 0.0;   // how much of the picture shows
	bool empty_ = false; // the capture has nothing to show
	// Used on the graphics thread only.
	obs_source_t *picture_ = nullptr;
	bool holding_ = false;
	gs_texrender_t *render_ = nullptr;
	gs_effect_t *effect_ = nullptr;
};

bool picture_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const auto kind = static_cast<Picture>(obs_data_get_int(settings, "picture"));
	obs_property_set_visible(obs_properties_get(props, "file"), kind == Picture::File);
	obs_property_set_visible(obs_properties_get(props, "source"), kind == Picture::Source);
	return true;
}

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<Standby *>(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "status",
				obs_module_text(self && self->showing() ? "Standby.Status.Showing"
									: "Standby.Status.Waiting"),
				OBS_TEXT_INFO);

	obs_property_t *picture = obs_properties_add_list(props, "picture", obs_module_text("Standby.Picture"),
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(picture, obs_module_text("Standby.Picture.Start"), (int)Picture::StartImage);
	obs_property_list_add_int(picture, obs_module_text("Standby.Picture.File"), (int)Picture::File);
	obs_property_list_add_int(picture, obs_module_text("Standby.Picture.Source"), (int)Picture::Source);
	obs_property_set_long_description(picture, obs_module_text("Standby.Picture.Tip"));
	obs_property_set_modified_callback(picture, picture_modified);

	obs_properties_add_path(props, "file", obs_module_text("Standby.File"), OBS_PATH_FILE,
				obs_module_text("Standby.File.Filter"), nullptr);
	obs_property_t *source = obs_properties_add_list(props, "source", obs_module_text("Standby.Source"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	struct Fill {
		obs_property_t *list;
		obs_source_t *self;
	} fill{source, self ? obs_filter_get_parent(self->source()) : nullptr};
	obs_property_list_add_string(source, obs_module_text("Standby.Source.None"), "");
	obs_enum_sources(
		[](void *param, obs_source_t *candidate) {
			auto *f = static_cast<Fill *>(param);
			const char *name = obs_source_get_name(candidate);
			if (candidate != f->self && name && *name &&
			    (obs_source_get_output_flags(candidate) & OBS_SOURCE_VIDEO))
				obs_property_list_add_string(f->list, name, name);
			return true;
		},
		&fill);

	obs_property_t *fade =
		obs_properties_add_int_slider(props, "fade_ms", obs_module_text("Standby.Fade"), 0, 2000, 50);
	obs_property_int_set_suffix(fade, " ms");
	obs_property_set_long_description(fade, obs_module_text("Standby.Fade.Tip"));

	obs_property_t *help = obs_properties_add_text(props, "help", obs_module_text("Standby.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(help, true);
	return props;
}

} // namespace

void register_standby()
{
	obs_source_info info = {};
	info.id = kStandbyId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = [](void *) {
		return obs_module_text("Standby.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new Standby(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<Standby *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<Standby *>(data)->update(settings);
	};
	info.get_defaults = [](obs_data_t *settings) {
		obs_data_set_default_int(settings, "picture", (int)Picture::StartImage);
		obs_data_set_default_int(settings, "fade_ms", 300);
		obs_data_set_default_bool(settings, "placed", false);
	};
	info.get_properties = get_properties;
	info.video_tick = [](void *data, float seconds) {
		static_cast<Standby *>(data)->tick(seconds);
	};
	info.video_render = [](void *data, gs_effect_t *) {
		static_cast<Standby *>(data)->render();
	};
	info.get_width = [](void *data) {
		return static_cast<Standby *>(data)->width();
	};
	info.get_height = [](void *data) {
		return static_cast<Standby *>(data)->height();
	};
	obs_register_source(&info);
}

} // namespace stagekit
