/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "start-image.hpp"

#include <obs-module.h>
#include <graphics/image-file.h>
#include <plugin-support.h>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace stagekit {

namespace {

std::mutex g_mutex;
std::string g_path;
std::atomic<uint64_t> g_revision{1};
std::atomic<uint64_t> g_choice{0};
AmbientView g_view;

// Shows the start image at its own size. Choosing a file in this source's
// settings sets the start image everywhere.
class StartImageSource {
public:
	StartImageSource(obs_data_t *settings, obs_source_t *source) : source_(source) { update(settings); }

	~StartImageSource()
	{
		obs_enter_graphics();
		if (loaded_)
			gs_image_file4_free(&image_);
		obs_leave_graphics();
	}

	void update(obs_data_t *settings)
	{
		const std::string chosen = obs_data_get_string(settings, "file");
		// Only a deliberate change counts: an empty or unchanged field must
		// not overwrite an image chosen elsewhere.
		if (!chosen.empty() && chosen != last_chosen_)
			choose_start_image(chosen);
		last_chosen_ = chosen;
	}

	void tick()
	{
		const uint64_t revision = start_image_revision();
		if (revision == revision_)
			return;
		revision_ = revision;
		const std::string path = start_image_path();

		obs_enter_graphics();
		if (loaded_)
			gs_image_file4_free(&image_);
		loaded_ = false;
		obs_leave_graphics();
		if (!path.empty()) {
			gs_image_file4_init(&image_, path.c_str(), GS_IMAGE_ALPHA_STRAIGHT);
			obs_enter_graphics();
			gs_image_file4_init_texture(&image_);
			obs_leave_graphics();
			loaded_ = true;
			if (!image_.image3.image2.image.loaded)
				obs_log(LOG_WARNING, "Start Image: could not load '%s'", path.c_str());
		}

		// Keep this source's own file field showing the current image.
		obs_data_t *settings = obs_source_get_settings(source_);
		if (path != obs_data_get_string(settings, "file")) {
			last_chosen_ = path;
			obs_data_set_string(settings, "file", path.c_str());
		}
		obs_data_release(settings);
	}

	gs_texture_t *texture() const { return loaded_ ? image_.image3.image2.image.texture : nullptr; }
	uint32_t width() const { return loaded_ ? image_.image3.image2.image.cx : 0; }
	uint32_t height() const { return loaded_ ? image_.image3.image2.image.cy : 0; }

	void render()
	{
		gs_texture_t *tex = texture();
		if (!tex)
			return;
		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
		const bool linear = gs_get_linear_srgb();
		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(linear);
		if (linear)
			gs_effect_set_texture_srgb(image, tex);
		else
			gs_effect_set_texture(image, tex);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(tex, 0, width(), height());
		gs_enable_framebuffer_srgb(previous);
	}

private:
	obs_source_t *source_;
	std::string last_chosen_;
	uint64_t revision_ = 0;
	gs_image_file4_t image_ = {};
	bool loaded_ = false;
};

} // namespace

std::string start_image_path()
{
	std::lock_guard lock(g_mutex);
	return g_path;
}

void set_start_image_path(const std::string &path)
{
	{
		std::lock_guard lock(g_mutex);
		if (path == g_path)
			return;
		g_path = path;
	}
	g_revision++;
}

void choose_start_image(const std::string &path)
{
	set_start_image_path(path);
	if (!path.empty())
		g_choice++;
}

uint64_t start_image_choice()
{
	return g_choice.load();
}

uint64_t start_image_revision()
{
	return g_revision.load();
}

AmbientView ambient_view()
{
	std::lock_guard lock(g_mutex);
	return g_view;
}

void set_ambient_view(const AmbientView &view)
{
	{
		std::lock_guard lock(g_mutex);
		g_view = view;
		g_view.zoom = std::clamp(view.zoom, 1.0, 8.0);
		g_view.focus_x = std::clamp(view.focus_x, 0.0, 1.0);
		g_view.focus_y = std::clamp(view.focus_y, 0.0, 1.0);
	}
	g_revision++;
}

std::string ambient_picture_path()
{
	std::lock_guard lock(g_mutex);
	return g_view.image.empty() ? g_path : g_view.image;
}

void start_image_save(obs_data_t *save_data)
{
	obs_data_set_string(save_data, "stagekit.start_image", start_image_path().c_str());
	const AmbientView view = ambient_view();
	obs_data_set_string(save_data, "stagekit.ambient_image", view.image.c_str());
	obs_data_set_double(save_data, "stagekit.ambient_zoom", view.zoom);
	obs_data_set_double(save_data, "stagekit.ambient_focus_x", view.focus_x);
	obs_data_set_double(save_data, "stagekit.ambient_focus_y", view.focus_y);
}

void start_image_load(obs_data_t *save_data)
{
	// A collection saved before this feature has no entry: keep whatever a
	// Start Image source in it may already have set.
	if (obs_data_has_user_value(save_data, "stagekit.start_image"))
		set_start_image_path(obs_data_get_string(save_data, "stagekit.start_image"));
	AmbientView view;
	if (obs_data_has_user_value(save_data, "stagekit.ambient_zoom")) {
		view.image = obs_data_get_string(save_data, "stagekit.ambient_image");
		view.zoom = obs_data_get_double(save_data, "stagekit.ambient_zoom");
		view.focus_x = obs_data_get_double(save_data, "stagekit.ambient_focus_x");
		view.focus_y = obs_data_get_double(save_data, "stagekit.ambient_focus_y");
	}
	set_ambient_view(view);
}

void register_start_image()
{
	obs_source_info info = {};
	info.id = kStartImageId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	info.icon_type = OBS_ICON_TYPE_IMAGE;
	info.get_name = [](void *) {
		return obs_module_text("StartImage.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new StartImageSource(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<StartImageSource *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<StartImageSource *>(data)->update(settings);
	};
	info.get_properties = [](void *) {
		obs_properties_t *props = obs_properties_create();
		obs_properties_add_path(
			props, "file", obs_module_text("StartImage.File"), OBS_PATH_FILE,
			"Images (*.bmp *.tga *.png *.jpeg *.jpg *.jxr *.gif *.psd *.webp *.avif);;All files (*.*)",
			nullptr);
		obs_property_t *help =
			obs_properties_add_text(props, "help", obs_module_text("StartImage.Help"), OBS_TEXT_INFO);
		obs_property_text_set_info_word_wrap(help, true);
		return props;
	};
	info.get_width = [](void *data) {
		return static_cast<StartImageSource *>(data)->width();
	};
	info.get_height = [](void *data) {
		return static_cast<StartImageSource *>(data)->height();
	};
	info.video_tick = [](void *data, float) {
		static_cast<StartImageSource *>(data)->tick();
	};
	info.video_render = [](void *data, gs_effect_t *) {
		static_cast<StartImageSource *>(data)->render();
	};
	obs_register_source(&info);

	proc_handler_add(
		obs_get_proc_handler(), "void stagekit_set_start_image(in string path)",
		[](void *, calldata_t *cd) {
			const char *path = calldata_string(cd, "path");
			choose_start_image(path ? path : "");
		},
		nullptr);
	proc_handler_add(
		obs_get_proc_handler(),
		"void stagekit_set_ambient_view(in string image, in float zoom, in float focus_x, in float focus_y)",
		[](void *, calldata_t *cd) {
			AmbientView view;
			const char *image = calldata_string(cd, "image");
			view.image = image ? image : "";
			view.zoom = calldata_float(cd, "zoom");
			view.focus_x = calldata_float(cd, "focus_x");
			view.focus_y = calldata_float(cd, "focus_y");
			set_ambient_view(view);
		},
		nullptr);
}

} // namespace stagekit
