#pragma once

#include "replacement-frame.hpp"

#include <obs-frontend-api.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QStringList>
#include <QThread>

namespace studio_preview_test {

// OBS draws Preview directly and Program from its composited texture. Windows
// presents 10-bit SDR monitors through a linear floating-point swap chain.
inline std::vector<uint8_t> display(obs_source_t *preview, gs_color_space space, uint32_t w, uint32_t h)
{
	return replacement_test::render_frame(96, 54, 96, 54, [&] {
		auto *panel = gs_texrender_create(space == GS_CS_SRGB ? GS_RGBA : GS_RGBA16F, GS_ZS_NONE);
		if (gs_texrender_begin_with_color_space(panel, 96, 54, space)) {
			vec4 black;
			vec4_zero(&black);
			black.w = 1;
			gs_clear(GS_CLEAR_COLOR, &black, 0, 0);
			gs_ortho(0, (float)w, 0, (float)h, -100, 100);
			const bool was_linear = gs_set_linear_srgb(false);
			if (preview)
				obs_source_video_render(preview);
			else
				obs_render_main_texture_src_color_only();
			gs_set_linear_srgb(was_linear);
			gs_texrender_end(panel);

			// Read both kinds of display in the same 8-bit sRGB units.
			auto *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			auto *texture = gs_texrender_get_texture(panel);
			const bool previous = gs_framebuffer_srgb_enabled();
			gs_enable_framebuffer_srgb(true);
			gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), texture);
			while (gs_effect_loop(effect, "Draw"))
				gs_draw_sprite(texture, 0, 96, 54);
			gs_enable_framebuffer_srgb(previous);
		}
		gs_texrender_destroy(panel);
	});
}

inline void settle()
{
	QElapsedTimer time;
	time.start();
	while (time.elapsed() < 550) {
		QCoreApplication::processEvents();
		QThread::msleep(15);
	}
}

inline void check(QStringList &issues)
{
	const bool previously_studio = obs_frontend_preview_program_mode_active();
	auto *old_program = obs_frontend_get_current_scene();
	auto *old_preview = previously_studio ? obs_frontend_get_current_preview_scene() : nullptr;
	obs_frontend_set_preview_program_mode(false);
	auto *scene = obs_scene_create("Studio color chart");
	auto *source = obs_scene_get_source(scene);
	const uint32_t w = obs_source_get_width(source), h = obs_source_get_height(source);
	const uint32_t colors[] = {0xff202020, 0xff808080, 0xffc07030};
	for (unsigned i = 0; i < 3; ++i) {
		auto *settings = obs_data_create();
		obs_data_set_int(settings, "width", (w + 2) / 3);
		obs_data_set_int(settings, "height", h);
		obs_data_set_int(settings, "color", colors[i]);
		auto *bar = obs_source_create_private("color_source_v3", "Studio chart patch", settings);
		auto *item = obs_scene_add(scene, bar);
		vec2 pos = {};
		pos.x = (float)(i * w / 3);
		obs_sceneitem_set_pos(item, &pos);
		obs_source_release(bar);
		obs_data_release(settings);
	}
	auto *settings = obs_data_create_from_json(R"({"shadow":false,"outline":true,"shadow_from":1})");
	auto *outline = obs_source_create_private("stagekit_scene_shadow", "Studio chart outline", settings);
	obs_source_filter_add(source, outline);
	obs_data_release(settings);
	obs_frontend_set_current_scene(source);
	settle();
	obs_frontend_set_preview_program_mode(true);
	settle();
	auto *preview = obs_frontend_get_current_preview_scene();
	if (preview != source)
		issues.append("Studio color chart did not reach Preview");
	for (auto space : {GS_CS_SRGB, GS_CS_SRGB_16F}) {
		const auto left = display(preview, space, w, h);
		const auto right = display(nullptr, space, w, h);
		for (unsigned i = 0; i < 3; ++i) {
			const size_t pixel = (27 * 96 + 16 + i * 32) * 4;
			blog(LOG_INFO, "[studio preview test] %s patch %u: Preview %u/%u/%u Program %u/%u/%u",
			     space == GS_CS_SRGB ? "8-bit SDR" : "10-bit SDR", i, left[pixel], left[pixel + 1],
			     left[pixel + 2], right[pixel], right[pixel + 1], right[pixel + 2]);
			for (unsigned channel = 0; channel < 3; ++channel) {
				const int expected = (colors[i] >> (channel * 8)) & 255;
				if (std::abs(left[pixel + channel] - expected) > 2 ||
				    std::abs(right[pixel + channel] - expected) > 2)
					issues.append(QString("Studio color chart %1 patch %2 channel %3: %4 / %5, expected %6")
							      .arg(space).arg(i).arg(channel).arg(left[pixel + channel])
							      .arg(right[pixel + channel]).arg(expected));
			}
		}
	}
	obs_source_release(preview);
	obs_frontend_set_preview_program_mode(false);
	if (old_program)
		obs_frontend_set_current_scene(old_program);
	if (previously_studio) {
		obs_frontend_set_preview_program_mode(true);
		if (old_preview)
			obs_frontend_set_current_preview_scene(old_preview);
	}
	obs_source_release(old_preview);
	obs_source_release(old_program);
	obs_source_release(outline);
	obs_source_remove(source);
	obs_scene_release(scene);
}

} // namespace studio_preview_test
