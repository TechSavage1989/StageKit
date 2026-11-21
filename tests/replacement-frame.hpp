#pragma once

#include <obs.h>
#include <graphics/vec4.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace replacement_test {

inline std::vector<uint8_t> render(obs_source_t *src, uint32_t w, uint32_t h, uint32_t source_w = 0,
				   uint32_t source_h = 0)
{
	std::vector<uint8_t> pixels(size_t(w) * h * 4);
	obs_enter_graphics();
	gs_texrender_t *tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_stagesurf_t *stage = gs_stagesurface_create(w, h, GS_RGBA);
	if (gs_texrender_begin(tr, w, h)) {
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)(source_w ? source_w : w), 0.0f, (float)(source_h ? source_h : h), -100.0f,
			 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		obs_source_video_render(src);
		gs_blend_state_pop();
		gs_texrender_end(tr);
	}
	gs_stage_texture(stage, gs_texrender_get_texture(tr));
	uint8_t *data = nullptr;
	uint32_t linesize = 0;
	if (gs_stagesurface_map(stage, &data, &linesize)) {
		for (uint32_t y = 0; y < h; y++)
			memcpy(&pixels[size_t(y) * w * 4], data + size_t(y) * linesize, size_t(w) * 4);
		gs_stagesurface_unmap(stage);
	}
	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(tr);
	obs_leave_graphics();
	return pixels;
}

// The fixture contains opaque, strongly colored regions and a band of narrow vertical stripes.
inline bool retains_artwork(const std::vector<uint8_t> &frame)
{
	size_t colored = 0;
	for (size_t i = 0; i + 3 < frame.size(); i += 4) {
		const auto range = std::minmax({frame[i], frame[i + 1], frame[i + 2]});
		if (frame[i + 3] > 48 && range.second - range.first > 24)
			++colored;
	}
	return !frame.empty() && colored > frame.size() / 4 / 8;
}

inline double stripe_contrast(const std::vector<uint8_t> &frame, uint32_t width)
{
	double variation = 0;
	for (uint32_t y = 58; y < 95; ++y)
		for (uint32_t x = 61; x < 153; ++x) {
			const size_t i = (size_t(y) * width + x) * 4;
			for (size_t c = 0; c < 3; ++c)
				variation += std::abs(int(frame[i + c]) - int(frame[i + c - 4]));
		}
	return variation;
}

inline void configure_blur(obs_data_t *settings, const std::string &name)
{
	obs_data_set_double(settings, "radius", 12.0);
	if (name == "composite-kawase") {
		obs_data_set_int(settings, "blur_algorithm", 3);
		obs_data_set_int(settings, "blur_type", 1);
		obs_data_set_double(settings, "kawase_passes", 20.0);
	} else if (name == "composite-hex") {
		obs_data_set_int(settings, "blur_algorithm", 4);
		obs_data_set_int(settings, "blur_type", 1);
		obs_data_set_int(settings, "pixelate_type", 1);
		obs_data_set_double(settings, "radius", 10.0);
	}
}

} // namespace replacement_test
