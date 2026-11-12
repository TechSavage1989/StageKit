// Headless test for StageKit on Linux.
//
// Starts libobs with the OpenGL renderer (run under Xvfb), loads the
// StageKit module next to OBS's image source, then:
//   1. renders every blur type at several strengths and writes raw RGBA
//      frames for visual inspection (tests/make-contact-sheet.py),
//   2. checks the ramp timing, reversal and hotkey registration,
//   3. if obs-websocket is present, drives the blur through the
//      "StageKit" vendor requests exactly as the Stream Deck plugin will.
//
// Usage: stagekit-render-test <stagekit.so> <stagekit data dir> <image> <out dir>

#include <obs.h>
#include <obs-module.h>
#include <obs-nix-platform.h>
#include <graphics/vec4.h>

#include "../src/third-party/obs-websocket-api.h"

#include <dlfcn.h>

#include <QApplication>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, ...)                                    \
	do {                                                \
		if (cond) {                                 \
			printf("  PASS  ");                 \
		} else {                                    \
			printf("  FAIL  ");                 \
			g_failures++;                       \
		}                                           \
		printf(__VA_ARGS__);                        \
		printf("\n");                               \
	} while (0)

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

static bool call_proc(obs_source_t *filter, const char *name, calldata_t *cd)
{
	return proc_handler_call(obs_source_get_proc_handler(filter), name, cd);
}

static double get_level(obs_source_t *filter, bool *active = nullptr)
{
	calldata_t cd = {};
	call_proc(filter, "get_state", &cd);
	double level = calldata_float(&cd, "level");
	if (active)
		*active = calldata_bool(&cd, "active");
	calldata_free(&cd);
	return level;
}

struct Case {
	const char *label;
	int type;
	double strength;
	double angle;
};

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
	const bool have_websocket = load_module((std::string(sys_bin) + "/obs-websocket.so").c_str(),
						(std::string(sys_data) + "/obs-websocket").c_str());
	obs_post_load_modules();

	obs_data_t *img_settings = obs_data_create();
	obs_data_set_string(img_settings, "file", image);
	obs_source_t *img = obs_source_create("image_source", "Test Image", img_settings, nullptr);
	obs_data_release(img_settings);
	const uint32_t w = obs_source_get_width(img);
	const uint32_t h = obs_source_get_height(img);
	printf("image: %ux%u\n", w, h);
	if (!w || !h)
		return 1;

	const std::vector<uint8_t> original = render(img, w, h);
	write_frame(out + "/original.rgba", original, w, h);

	// ------------------------------------------------ 1. every blur type
	printf("\n[render] blur types\n");
	const Case cases[] = {
		{"gaussian", 0, 8, 0}, {"gaussian", 0, 30, 0}, {"gaussian", 0, 90, 0}, {"kawase", 2, 8, 0},
		{"kawase", 2, 30, 0},  {"kawase", 2, 90, 0},   {"box", 1, 8, 0},       {"box", 1, 30, 0},
		{"box", 1, 90, 0},     {"pixelate", 3, 8, 0},  {"pixelate", 3, 30, 0}, {"pixelate", 3, 90, 0},
		{"zoom", 4, 30, 0},    {"zoom", 4, 90, 0},     {"zoom", 4, 200, 0},    {"motion", 5, 15, 0},
		{"motion", 5, 40, 30}, {"motion", 5, 90, 90},
	};
	obs_source_t *filter = nullptr;
	for (const Case &c : cases) {
		obs_data_t *fs = obs_data_create();
		obs_data_set_int(fs, "blur_type", c.type);
		obs_data_set_double(fs, "strength", c.strength);
		obs_data_set_double(fs, "angle", c.angle);
		obs_data_set_int(fs, "start_state", 1); // start blurred
		if (!filter) {
			filter = obs_source_create("stagekit_ramp_blur", "Ramp Blur", fs, nullptr);
			obs_source_filter_add(img, filter);
		} else {
			obs_source_update(filter, fs);
		}
		obs_data_release(fs);
		// OBS applies settings on the next video tick; wait a few frames.
		sleep_ms(60);

		const std::vector<uint8_t> px = render(img, w, h);
		const double diff = mean_abs_diff(px, original);
		char name[128];
		snprintf(name, sizeof(name), "%s_%03d.rgba", c.label, (int)c.strength);
		write_frame(out + "/" + name, px, w, h);
		CHECK(diff > 0.5, "%-8s strength %5.1f changes the image (mean diff %.2f)", c.label, c.strength, diff);
	}

	// ------------------------------------------------ 1b. ramp smoothness
	// Sweep strength in small steps. A smooth ramp changes the picture by a
	// similar amount every step; a "pop" shows up as a spike.
	printf("\n[smoothness] frame-to-frame change while strength sweeps 0 -> 80\n");
	for (int type : {0, 2, 1, 3}) {
		const char *names[] = {"gaussian", "box", "kawase", "pixelate"};
		std::vector<double> steps;
		std::vector<uint8_t> prev;
		for (double st = 0.0; st <= 80.0; st += 2.0) {
			obs_data_t *fs = obs_data_create();
			obs_data_set_int(fs, "blur_type", type);
			obs_data_set_double(fs, "strength", st);
			obs_source_update(filter, fs);
			obs_data_release(fs);
			sleep_ms(40);
			std::vector<uint8_t> px = render(img, w, h);
			if (!prev.empty())
				steps.push_back(mean_abs_diff(px, prev));
			prev = std::move(px);
		}
		double worst = 0.0;
		int worst_at = 0;
		for (size_t i = 1; i + 1 < steps.size(); i++) {
			const double neighbours = std::max((steps[i - 1] + steps[i + 1]) / 2.0, 0.05);
			if (steps[i] / neighbours > worst) {
				worst = steps[i] / neighbours;
				worst_at = int(i + 1) * 2;
			}
		}
		printf("        %-8s", names[type]);
		for (size_t i = 0; i < steps.size(); i += 4)
			printf(" %5.2f", steps[i]);
		printf("\n");
		if (type == 3)
			printf("        (pixelate is stepped by nature: block size changes in whole pixels)\n");
		else
			CHECK(worst < 2.5,
			      "%-8s no visible jump while ramping (worst step %.2fx its neighbours at strength %d)",
			      names[type], worst, worst_at);
	}

	// ------------------------------------------------ 2. ramp behaviour
	printf("\n[ramp]\n");
	{
		obs_source_filter_remove(img, filter);
		obs_source_release(filter);

		obs_data_t *fs = obs_data_create();
		obs_data_set_int(fs, "blur_type", 0);
		obs_data_set_double(fs, "strength", 30);
		obs_data_set_int(fs, "ramp_in_ms", 600);
		obs_data_set_int(fs, "ramp_out_ms", 300);
		obs_data_set_int(fs, "ease_curve", 0); // linear, so timing is easy to check
		filter = obs_source_create("stagekit_ramp_blur", "Ramp Blur", fs, nullptr);
		obs_data_release(fs);
		obs_source_filter_add(img, filter);

		bool active = true;
		CHECK(get_level(filter, &active) == 0.0 && !active, "starts off (start_state default)");
		const std::vector<uint8_t> off_px = render(img, w, h);
		CHECK(mean_abs_diff(off_px, original) < 0.01, "off = pass-through (diff %.4f)",
		      mean_abs_diff(off_px, original));

		calldata_t cd = {};
		call_proc(filter, "blur_on", &cd);
		sleep_ms(300);
		double mid = get_level(filter, &active);
		CHECK(active && mid > 0.3 && mid < 0.7, "halfway through 600 ms ramp-in level is ~0.5 (got %.3f)", mid);
		write_frame(out + "/ramp_mid.rgba", render(img, w, h), w, h);

		// Reverse part-way: should head back down without jumping.
		mid = get_level(filter);
		call_proc(filter, "blur_toggle", &cd);
		double right_after = get_level(filter, &active);
		CHECK(!active && std::fabs(right_after - mid) < 0.05,
		      "reversing mid-ramp continues from current level (%.3f -> %.3f)", mid, right_after);
		sleep_ms(400);
		CHECK(get_level(filter) == 0.0, "ramp-out finished within its remaining time");

		call_proc(filter, "blur_on", &cd);
		sleep_ms(750);
		CHECK(get_level(filter) == 1.0, "full ramp-in reaches 1.0");
		calldata_free(&cd);
	}

	// ------------------------------------------------ 3. hotkeys
	printf("\n[hotkeys]\n");
	{
		struct Found {
			int count = 0;
		} found;
		obs_enum_hotkeys(
			[](void *data, obs_hotkey_id, obs_hotkey_t *key) {
				const char *name = obs_hotkey_get_name(key);
				if (name && strncmp(name, "stagekit.blur.", 14) == 0) {
					static_cast<Found *>(data)->count++;
					printf("        %s  ->  \"%s\"\n", name, obs_hotkey_get_description(key));
				}
				return true;
			},
			&found);
		CHECK(found.count == 3, "toggle/on/off hotkeys registered on the parent source (%d)", found.count);

		obs_source_set_name(filter, "Pause Blur");
		found.count = 0;
		bool renamed = false;
		obs_enum_hotkeys(
			[](void *data, obs_hotkey_id, obs_hotkey_t *key) {
				const char *name = obs_hotkey_get_name(key);
				if (name && strcmp(name, "stagekit.blur.toggle.Pause Blur") == 0)
					*static_cast<bool *>(data) = true;
				return true;
			},
			&renamed);
		CHECK(renamed, "hotkeys follow a filter rename");
	}

	// ------------------------------------------------ 4. obs-websocket vendor
	printf("\n[obs-websocket vendor]\n");
	if (!have_websocket || obs_websocket_get_api_version() == 0) {
		printf("  SKIP  obs-websocket not available\n");
	} else {
		auto vendor_call = [](const char *type, obs_data_t *inner) {
			obs_data_t *req = obs_data_create();
			obs_data_set_string(req, "vendorName", "StageKit");
			obs_data_set_string(req, "requestType", type);
			if (inner)
				obs_data_set_obj(req, "requestData", inner);
			auto *res = obs_websocket_call_request("CallVendorRequest", req);
			obs_data_release(req);
			std::string json;
			bool ok = false;
			if (res) {
				json = res->response_data ? res->response_data : "";
				obs_data_t *rd = obs_data_create_from_json(json.c_str());
				obs_data_t *inner_res = rd ? obs_data_get_obj(rd, "responseData") : nullptr;
				ok = inner_res && obs_data_get_bool(inner_res, "success");
				obs_data_release(inner_res);
				obs_data_release(rd);
				obs_websocket_request_response_free(res);
			}
			return std::make_pair(ok, json);
		};

		auto [ok_list, list_json] = vendor_call("ListBlurFilters", nullptr);
		CHECK(ok_list && list_json.find("Pause Blur") != std::string::npos, "ListBlurFilters finds the filter");
		printf("        %s\n", list_json.c_str());

		obs_data_t *args = obs_data_create();
		obs_data_set_string(args, "sourceName", "Test Image");
		obs_data_set_double(args, "durationMs", 0);
		auto [ok_off, off_json] = vendor_call("BlurOff", args);
		CHECK(ok_off && get_level(filter) == 0.0, "BlurOff with durationMs 0 is instant");
		auto [ok_tog, tog_json] = vendor_call("BlurToggle", args);
		CHECK(ok_tog && get_level(filter) == 1.0, "BlurToggle turns it back on");
		obs_data_release(args);

		obs_data_t *bad = obs_data_create();
		obs_data_set_string(bad, "sourceName", "Nope");
		auto [ok_bad, bad_json] = vendor_call("BlurOn", bad);
		CHECK(!ok_bad && bad_json.find("No source") != std::string::npos, "unknown source returns an error");
		obs_data_release(bad);
	}

	obs_source_filter_remove(img, filter);
	obs_source_release(filter);
	obs_source_release(img);
	obs_shutdown();

	printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
	return g_failures ? 1 : 0;
}
