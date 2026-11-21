#include <obs.h>
#include <obs-module.h>
#include <obs-nix-platform.h>
#include <graphics/vec4.h>
#include <QApplication>
#include <dlfcn.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

static int failures = 0;
static void check(bool pass, const std::string &label)
{
	printf("%s %s\n", pass ? "PASS" : "FAIL", label.c_str());
	if (!pass)
		++failures;
}
static bool load_module(const char *bin, const char *data)
{
	obs_module_t *mod = nullptr;
	int r = obs_open_module(&mod, bin, data);
	if (r != MODULE_SUCCESS) {
		printf("failed to open module %s (%d)\n", bin, r);
		return false;
	}
	if (!obs_init_module(mod)) {
		printf("failed to init module %s\n", bin);
		return false;
	}
	return true;
}

static void sleep_ms(int ms)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static std::vector<uint8_t> render(obs_source_t *src, uint32_t w, uint32_t h)
{
	std::vector<uint8_t> pixels(size_t(w) * h * 4);
	obs_enter_graphics();
	gs_texrender_t *tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_stagesurf_t *stage = gs_stagesurface_create(w, h, GS_RGBA);
	if (gs_texrender_begin(tr, w, h)) {
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
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

static void write_frame(const std::string &path, const std::vector<uint8_t> &px, uint32_t w, uint32_t h)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f)
		return;
	uint32_t header[2] = {w, h};
	fwrite(header, sizeof(header), 1, f);
	fwrite(px.data(), 1, px.size(), f);
	fclose(f);
}

// Difference as a viewer would see it: both frames composited over a dark
// background, so colour hidden in fully transparent pixels does not count.
static double mean_abs_diff(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b)
{
	const double bg = 30.0;
	double sum = 0;
	for (size_t i = 0; i < a.size(); i += 4) {
		const double aa = a[i + 3] / 255.0, ba = b[i + 3] / 255.0;
		for (int c = 0; c < 3; c++)
			sum += std::fabs((a[i + c] * aa + bg * (1 - aa)) - (b[i + c] * ba + bg * (1 - ba)));
	}
	return sum / double(a.size() / 4 * 3);
}


int main(int argc, char **argv)
{
	if (argc < 5) {
		fprintf(stderr, "usage: %s <stagekit.so> <data dir> <image> <out dir>\n", argv[0]);
		return 2;
	}
	const char *plugin_bin = argv[1];
	const char *plugin_data = argv[2];
	const char *image = argv[3];
	const std::string out = argv[4];

	// obs-websocket builds Qt widgets when it loads, so a QApplication must exist.
	QApplication app(argc, argv);

	void *x11 = dlopen("libX11.so.6", RTLD_NOW);
	auto xopen = x11 ? (void *(*)(const char *))dlsym(x11, "XOpenDisplay") : nullptr;
	void *display = xopen ? xopen(nullptr) : nullptr;
	if (!display) {
		fprintf(stderr, "No X display (run under xvfb-run)\n");
		return 2;
	}
	obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);
	obs_set_nix_platform_display(display);

	if (!obs_startup("en-US", nullptr, nullptr)) {
		fprintf(stderr, "obs_startup failed\n");
		return 1;
	}

	obs_video_info ovi = {};
	ovi.graphics_module = "libobs-opengl";
	ovi.fps_num = 60;
	ovi.fps_den = 1;
	ovi.base_width = 640;
	ovi.base_height = 360;
	ovi.output_width = 640;
	ovi.output_height = 360;
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.adapter = 0;
	ovi.gpu_conversion = true;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	int vr = obs_reset_video(&ovi);
	if (vr != OBS_VIDEO_SUCCESS) {
		fprintf(stderr, "obs_reset_video failed: %d\n", vr);
		return 1;
	}
	obs_enter_graphics();
	printf("renderer: %s\n", gs_get_device_name());
	obs_leave_graphics();

	const char *sys_bin = "/usr/lib/x86_64-linux-gnu/obs-plugins";
	const char *sys_data = "/usr/share/obs/obs-plugins";
	if (!load_module((std::string(sys_bin) + "/image-source.so").c_str(),
			 (std::string(sys_data) + "/image-source").c_str()))
		return 1;
	if (!load_module(plugin_bin, plugin_data))
		return 1;

	obs_post_load_modules();
	const auto has_label = [](const char *id, const char *prefix) {
		const char *name = obs_source_get_display_name(id);
		return name && std::string(name).find(prefix) == 0 && std::string(name).find("(StageKit)") != std::string::npos;
	};
	const char *source_types[] = {"3d_effect_filter", "advanced_masks_filter", "obs_composite_blur", "obs_retro_effects_filter",
		"obs_noise_displace_filter", "pixel_art_plugin", "freeze_filter", "dynamic_delay_filter", "recursion_effect_filter",
		"time_warp_scan_filter", "dir_watch_media", "source_defaults_video", "source_defaults_audio", "move_action_filter",
		"move_value_filter", "move_transition_override_filter", "shader_filter"};
	for (const auto *id : source_types) {
		const std::string type = "stagekit_port_" + std::string(id);
		check(has_label(type.c_str(), "Source Filter:"), type);
	}
	for (const auto *id : {"stagekit_port_move_source_filter", "stagekit_port_move_source_swap_filter"})
		check(has_label(id, "Scene Filter:"), id);
	for (const auto *id : {"stagekit_effect_file", "stagekit_drop_shadow", "stagekit_edge_detection", "stagekit_gaussian_blur", "stagekit_toon", "stagekit_fix_alpha"})
		check(has_label(id, "Source Filter:"), id);
	check(has_label("stagekit_port_source-clone", "Source Clone"), "clone input registered");
	check(has_label("stagekit_port_move_transition", "Move"), "Move transition registered");

	obs_data_t *image_settings = obs_data_create();
	obs_data_set_string(image_settings, "file", image);
	auto *image_source = obs_source_create("image_source", "Test art", image_settings, nullptr);
	obs_data_release(image_settings);
	if (!image_source)
		return 1;
	obs_source_inc_showing(image_source);
	sleep_ms(100);
	const uint32_t w = obs_source_get_width(image_source), h = obs_source_get_height(image_source);
	if (!w || !h)
		return 1;
	const auto base_pixels = render(image_source, w, h);
	write_frame(out + "/original.rgba", base_pixels, w, h);
	struct Example { const char *id; const char *name; };
	const Example examples[] = {{"stagekit_effect_file", "custom"}, {"stagekit_drop_shadow", "shadow"},
		{"stagekit_edge_detection", "edges"}, {"stagekit_gaussian_blur", "gaussian"}, {"stagekit_toon", "toon"},
		{"stagekit_port_obs_composite_blur", "composite"}, {"stagekit_port_shader_filter", "saved-shader"}};
	for (const auto &example : examples) {
		const std::string name(example.name);
		auto *settings = obs_data_create();
		if (name == "custom")
			obs_data_set_string(settings, "effect_path", (out + "/invert.effect").c_str());
		if (name == "shadow") {
			obs_data_set_int(settings, "offset_x", -9);
			obs_data_set_int(settings, "offset_y", 12);
			obs_data_set_int(settings, "shadow_color", 0xffdd9933);
		}
		if (name == "gaussian") {
			obs_data_set_int(settings, "kernel_size", 21);
			obs_data_set_double(settings, "sigma", 5.0);
		}
		if (name == "composite")
			obs_data_set_double(settings, "radius", 12.0);
		if (name == "saved-shader") {
			obs_data_set_bool(settings, "from_file", true);
			const std::string file = std::string(plugin_data) + "/components/shader/examples/drop_shadow.shader";
			obs_data_set_string(settings, "shader_file_name", file.c_str());
			obs_data_set_int(settings, "shadow_offset_x", 9);
			obs_data_set_int(settings, "shadow_offset_y", 12);
			obs_data_set_int(settings, "shadow_blur_size", 4);
		}
		auto *filter = obs_source_create(example.id, example.name, settings, nullptr);
		obs_data_release(settings);
		if (!filter) { check(false, name + " could not be created"); continue; }
		obs_source_filter_add(image_source, filter);
		sleep_ms(130);
		auto fw = obs_source_get_width(image_source), fh = obs_source_get_height(image_source);
		check(fw && fh, name + " reports nonzero dimensions");
		if (fw && fh) {
			(void)render(image_source, fw, fh);
			sleep_ms(50);
			auto frame = render(image_source, fw, fh);
			write_frame(out + "/" + name + ".rgba", frame, fw, fh);
			if (name == "shadow") {
				check(fw == w + 9 && fh == h + 12, "shadow extends the frame in both directions");
				bool colour = false;
				for (size_t i = 0; i < frame.size(); i += 4)
					if (frame[i+3] > 16 && frame[i+2] > frame[i]+60) { colour = true; break; }
				check(colour, "shadow paint is visible");
			} else if (fw == w && fh == h) {
				const double diff = mean_abs_diff(frame, base_pixels);
				printf("mean difference for %s: %.3f\n", example.name, diff);
				check(diff > 0.12, name + " changes visible pixels");
				if (name == "gaussian") {
					auto *mask = obs_data_create();
					obs_data_set_bool(mask, "use_mask", true);
					obs_data_set_string(mask, "mask_image", (out + "/mask.png").c_str());
					obs_source_update(filter, mask);
					sleep_ms(100);
					const auto masked = render(image_source, fw, fh);
					obs_data_set_bool(mask, "invert_mask", true);
					obs_source_update(filter, mask);
					sleep_ms(100);
					const auto inverted = render(image_source, fw, fh);
					check(mean_abs_diff(masked, inverted) > 0.12, "inverting the Gaussian mask moves the blur to the other half");
					obs_data_release(mask);
				} else if (name == "custom") {
					auto *bad = obs_data_create();
					obs_data_set_string(bad, "effect_path", (out + "/missing.effect").c_str());
					obs_source_update(filter, bad);
					obs_data_release(bad);
					sleep_ms(100);
					check(mean_abs_diff(frame, render(image_source, fw, fh)) < 0.01, "failed reload retains the working custom effect");
				}
			} else {
				check(false, name + " unexpectedly changed the source size");
			}
		}
		obs_source_filter_remove(image_source, filter);
		obs_source_release(filter);
		sleep_ms(40);
	}
	obs_source_dec_showing(image_source);
	obs_source_release(image_source);
	obs_shutdown();
	return failures ? 1 : 0;
}
