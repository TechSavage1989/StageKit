#include "frame.hpp"
#include "enable-keys.hpp"

#include <graphics/image-file.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>

namespace stagekit::native {
namespace {
struct Scan {
	obs_source_t *source;
	EnableKeys keys;
	Effect shader{"effects/stagekit-scan.effect"};
	Frame buffers[2];
	gs_image_file_t background{};
	std::string path;
	int previous = 0;
	Size size{};
	std::atomic<bool> restart{true};
	bool ready = false, show_live = false, follow_alpha = false;
	uint64_t start = 0, rendered = 0, duration = 10000000000ULL;
	float cursor = 0, angle = 0, line = 3, opacity = 1;
	vec4 color{};

	Scan(obs_data_t *s, obs_source_t *context) : source(context), keys(context)
	{
		update(s);
		signal_handler_connect(obs_source_get_signal_handler(source), "enable", enabled, this);
	}
	~Scan()
	{
		signal_handler_disconnect(obs_source_get_signal_handler(source), "enable", enabled, this);
		obs_enter_graphics();
		gs_image_file_free(&background);
		obs_leave_graphics();
	}
	static void enabled(void *p, calldata_t *) { static_cast<Scan *>(p)->restart.store(true); }
	static void defaults(obs_data_t *s)
	{
		obs_data_set_default_int(s, "scan_duration", 10000);
		obs_data_set_default_int(s, "line_width", 3);
		obs_data_set_default_int(s, "line_color", 0xffffff55);
		obs_data_set_default_double(s, "line_opacity", 100);
	}
	void update(obs_data_t *s)
	{
		duration = uint64_t(std::clamp<long long>(obs_data_get_int(s, "scan_duration"), 0, 100000)) * 1000000ULL;
		line = float(std::clamp<long long>(obs_data_get_int(s, "line_width"), 0, 100));
		vec4_from_rgba(&color, uint32_t(obs_data_get_int(s, "line_color")) | 0xff000000U);
		follow_alpha = obs_data_get_bool(s, "dst_line_opacity");
		opacity = float(std::clamp(obs_data_get_double(s, "line_opacity") / 100.0, 0., 1.));
		show_live = obs_data_get_bool(s, "transparent");
		const float rotation = float(obs_data_get_double(s, "rotation") * .017453292519943295);
		if (rotation != angle)
			restart.store(true);
		angle = rotation;
		const std::string file = obs_data_get_string(s, "image");
		if (file != path) {
			obs_enter_graphics();
			gs_image_file_free(&background);
			background = {};
			obs_leave_graphics();
			path = file;
			if (!file.empty()) {
				gs_image_file_init(&background, file.c_str());
				obs_enter_graphics();
				if (background.loaded)
					gs_image_file_init_texture(&background);
				obs_leave_graphics();
			}
		}
	}
	static obs_properties_t *properties(void *)
	{
		auto *p = obs_properties_create();
		obs_property_int_set_suffix(obs_properties_add_int(p, "scan_duration", text("Native.ScanTime"), 0, 100000, 10), " ms");
		obs_property_int_set_suffix(obs_properties_add_int(p, "line_width", text("Native.ScanWidth"), 0, 100, 1), " px");
		obs_properties_add_color(p, "line_color", text("Native.ScanColor"));
		obs_properties_add_bool(p, "dst_line_opacity", text("Native.ScanAlpha"));
		obs_properties_add_float_slider(p, "line_opacity", text("Native.ScanOpacity"), 0, 100, 1);
		obs_properties_add_float_slider(p, "rotation", text("Native.Rotation"), 0, 360, 1);
		obs_properties_add_bool(p, "transparent", text("Native.ScanLive"));
		obs_properties_add_path(p, "image", text("Native.ScanBackground"), OBS_PATH_FILE, "Images (*.png *.jpg *.jpeg *.bmp *.webp *.gif);;All files (*)", nullptr);
		return p;
	}
	void render()
	{
		const Size current = size_of_target(source);
		if (!current || !shader.value) {
			obs_source_skip_video_filter(source);
			return;
		}
		const auto now = obs_get_video_frame_time();
		if (restart.exchange(false) || current.width != size.width || current.height != size.height) {
			start = now;
			size = current;
			ready = false;
			cursor = 0;
		}
		const float dx = std::sin(angle), dy = std::cos(angle);
		const float span = std::max(1.0f, std::abs(dx) * size.width + std::abs(dy) * size.height);
		shader.pair("sweep", dx * size.width / span, dy * size.height / span);
		auto *frozen = buffers[previous].texture();
		if (!ready || (cursor < 1 && rendered != now)) {
			const int next = 1 - previous;
			const float position = duration ? float(std::min(1.0, double(now - start) / double(duration))) : 1.0f;
			if (!buffers[next].begin(size)) {
				obs_source_skip_video_filter(source);
				return;
			}
			const bool ok = obs_source_process_filter_begin(source, GS_RGBA, OBS_NO_DIRECT_RENDERING);
			if (ok) {
				shader.pair("progress", ready ? cursor : -1.f, position);
				gs_effect_set_texture(shader.parameter("stored"), ready ? frozen : nullptr);
				obs_source_process_filter_tech_end(source, shader.value, size.width, size.height, "Accumulate");
			}
			buffers[next].end();
			if (!ok) {
				obs_source_skip_video_filter(source);
				return;
			}
			rendered = now;
			ready = true;
			cursor = position;
			previous = next;
		}
		if (!obs_source_process_filter_begin(source, GS_RGBA, OBS_NO_DIRECT_RENDERING))
			return;
		shader.pair("progress", cursor, cursor);
		shader.four("line_style", line / span, opacity, follow_alpha ? 1.f : 0.f, show_live ? 1.f : 0.f);
		gs_effect_set_vec4(shader.parameter("ink"), &color);
		gs_effect_set_texture(shader.parameter("stored"), buffers[previous].texture());
		gs_effect_set_texture(shader.parameter("backdrop"), background.texture);
		gs_effect_set_bool(shader.parameter("has_image"), background.texture != nullptr);
		obs_source_process_filter_tech_end(source, shader.value, size.width, size.height, "Display");
	}
};
void load()
{
	auto info = video_filter<Scan>("stagekit_port_time_warp_scan_filter", "Ports.TimeWarpScan");
	info.filter_add = [](void *p, obs_source_t *parent) {
		static_cast<Scan *>(p)->keys.bind(parent, "tws.Enable", "tws.Disable", "Native.ScanOn", "Native.ScanOff");
	};
	info.filter_remove = [](void *p, obs_source_t *) { static_cast<Scan *>(p)->keys.clear(); };
	obs_register_source(&info);
}
} // namespace
const Component component{load};
} // namespace stagekit::native
