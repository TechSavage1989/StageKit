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
		{"motion", 5, 40, 30}, {"motion", 5, 90, 90},  {"hexagon", 6, 8, 0},   {"hexagon", 6, 30, 0},
		{"hexagon", 6, 90, 0},
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

	// ------------------------------------------------ 1a. mosaic shapes
	printf("\n[render] mosaic shapes\n");
	{
		obs_data_t *now = obs_source_get_settings(filter);
		CHECK(obs_data_get_int(now, "blur_type") == 3 && obs_data_get_int(now, "pixel_shape") == 1,
		      "the old 'Hexagons' blur type is read as Pixelate with hexagon cells (type %lld, shape %lld)",
		      obs_data_get_int(now, "blur_type"), obs_data_get_int(now, "pixel_shape"));
		obs_data_release(now);
		auto mosaic = [&](int shape, double smooth, double rotation, double origin_x) {
			obs_data_t *fs = obs_data_create();
			obs_data_set_int(fs, "blur_type", 3);
			obs_data_set_double(fs, "strength", 30);
			obs_data_set_int(fs, "pixel_shape", shape);
			obs_data_set_double(fs, "pixel_smooth", smooth);
			obs_data_set_double(fs, "rotation", rotation);
			obs_data_set_double(fs, "origin_x", origin_x);
			obs_source_update(filter, fs);
			obs_data_release(fs);
			sleep_ms(60);
			return render(img, w, h);
		};
		const std::vector<uint8_t> squares = mosaic(0, 0, 0, 50);
		const std::vector<uint8_t> hexagons = mosaic(1, 0, 0, 50);
		const char *names[] = {"squares", "hexagons",  "circles", "triangles",
				       "voronoi", "rhomboids", "triakis"};
		std::vector<std::vector<uint8_t>> shapes{squares, hexagons};
		for (int shape = 2; shape <= 6; shape++) {
			shapes.push_back(mosaic(shape, 0, 0, 50));
			write_frame(out + "/" + names[shape] + "_030.rgba", shapes.back(), w, h);
		}
		// Each is a picture of its own, and none leaves holes in an opaque picture.
		for (int a = 2; a <= 6; a++) {
			double nearest = 1e9;
			for (int b = 0; b < a; b++)
				nearest = std::min(nearest, mean_abs_diff(shapes[a], shapes[b]));
			double see_through = 0;
			for (size_t i = 3; i < shapes[a].size(); i += 4)
				see_through += shapes[a][i] < original[i] ? 1 : 0;
			CHECK(mean_abs_diff(shapes[a], original) > 0.5 && nearest > 0.3 &&
				      see_through < shapes[a].size() / 4 * 0.001,
			      "%-9s cells change the picture, differ from the other shapes and leave no holes (diff %.2f, nearest other shape %.2f)",
			      names[a], mean_abs_diff(shapes[a], original), nearest);
		}
		const std::vector<uint8_t> smoothed = mosaic(1, 100, 0, 50);
		write_frame(out + "/smoothed_030.rgba", smoothed, w, h);
		CHECK(mean_abs_diff(smoothed, hexagons) > 0.2 &&
			      mean_abs_diff(smoothed, hexagons) < mean_abs_diff(hexagons, original),
		      "smoothing softens the edges between cells (diff from hard edges %.2f)",
		      mean_abs_diff(smoothed, hexagons));
		const std::vector<uint8_t> turned = mosaic(0, 0, 30, 50);
		const std::vector<uint8_t> shifted = mosaic(0, 0, 0, 52);
		CHECK(mean_abs_diff(turned, squares) > 0.3 && mean_abs_diff(shifted, squares) > 0.3,
		      "the pattern can be turned and laid out from another point (diff %.2f turned, %.2f moved)",
		      mean_abs_diff(turned, squares), mean_abs_diff(shifted, squares));
		const std::vector<uint8_t> again = mosaic(0, 0, 0, 50);
		CHECK(mean_abs_diff(again, squares) < 0.01, "with the settings back, squares are as they were");
	}

	// ------------------------------------------------ 1a2. temporal
	printf("\n[render] temporal\n");
	{
		obs_data_t *cs = obs_data_create();
		obs_data_set_int(cs, "color", 0xFF0000FF); // red (ABGR)
		obs_data_set_int(cs, "width", 64);
		obs_data_set_int(cs, "height", 64);
		obs_source_t *card = obs_source_create("color_source", "Trail Card", cs, nullptr);
		obs_data_t *ts = obs_data_create();
		obs_data_set_int(ts, "blur_type", 7);
		obs_data_set_double(ts, "strength", 40); // things stay about 0.4 s
		obs_data_set_int(ts, "ramp_in_ms", 0);
		obs_source_t *trail = obs_source_create("stagekit_ramp_blur", "Trail", ts, nullptr);
		obs_data_release(ts);
		obs_source_filter_add(card, trail);
		std::vector<uint8_t> px;
		for (int i = 0; i < 20; i++) {
			sleep_ms(30);
			px = render(card, 64, 64);
		}
		const int red0 = px[0], blue0 = px[2];
		obs_data_set_int(cs, "color", 0xFFFF0000); // blue
		obs_source_update(card, cs);
		sleep_ms(100);
		px = render(card, 64, 64);
		const int red1 = px[0], blue1 = px[2];
		for (int i = 0; i < 80; i++) {
			sleep_ms(30);
			px = render(card, 64, 64);
		}
		const int red2 = px[0], blue2 = px[2];
		CHECK(red0 > 250 && blue0 < 5, "a still picture is left as it is by Temporal (red %d, blue %d)", red0,
		      blue0);
		CHECK(red1 > 20 && red1 < 250 && blue1 > 5 && blue1 < 250,
		      "just after a change the old picture still shows through: a trail (red %d, blue %d)", red1,
		      blue1);
		CHECK(red2 < 5 && blue2 > 250, "and it has faded all the way out a little later (red %d, blue %d)",
		      red2, blue2);
		obs_source_filter_remove(card, trail);
		obs_source_release(trail);
		obs_source_release(card);
		obs_data_release(cs);
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

		// The filter's own on/off switch is the blur state. This is what the
		// eye icon and the Stream Deck OBS plugin's Filter key flip.
		bool active = true;
		sleep_ms(60);
		CHECK(get_level(filter, &active) == 1.0 && active, "a newly added (enabled) filter starts blurred");
		obs_source_set_enabled(filter, false);
		sleep_ms(100);
		double during = get_level(filter, &active);
		CHECK(!active && during > 0.3 && during < 0.9 && obs_source_enabled(filter),
		      "switching the filter off ramps out instead of cutting (level %.2f, still drawn)", during);
		sleep_ms(350);
		CHECK(get_level(filter) == 0.0 && !obs_source_enabled(filter),
		      "after the ramp-out the filter really is off");
		obs_source_set_enabled(filter, true);
		sleep_ms(300);
		during = get_level(filter, &active);
		CHECK(active && during > 0.3 && during < 0.7, "switching the filter on ramps in (level %.2f)", during);
		sleep_ms(400);
		obs_source_set_enabled(filter, false);
		sleep_ms(100);
		obs_source_set_enabled(filter, false); // second press while ramping out
		sleep_ms(450);
		CHECK(get_level(filter, &active) == 1.0 && active && obs_source_enabled(filter),
		      "pressing again during the ramp-out brings the blur back");
		obs_source_set_enabled(filter, false);
		sleep_ms(450);
		CHECK(get_level(filter, &active) == 0.0 && !active, "off again");

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
		// A frame may tick between the two readings: one frame of the 300 ms
		// ramp-out is 0.056, so allow two. A real jump would be far larger.
		CHECK(!active && std::fabs(right_after - mid) < 0.12,
		      "reversing mid-ramp continues from current level (%.3f -> %.3f)", mid, right_after);
		sleep_ms(400);
		CHECK(get_level(filter) == 0.0 && !obs_source_enabled(filter),
		      "ramp-out finished within its remaining time and switched the filter off");

		call_proc(filter, "blur_on", &cd);
		sleep_ms(750);
		CHECK(get_level(filter) == 1.0 && obs_source_enabled(filter),
		      "full ramp-in reaches 1.0 and the filter shows as on");
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

		// The dock's switches and slides, as a Stream Deck key asks for them.
		auto [ok_opts, opts_json] = vendor_call("GetOptions", nullptr);
		CHECK(ok_opts && opts_json.find("\"snap\":false") != std::string::npos &&
			      opts_json.find("\"guides\":false") != std::string::npos,
		      "GetOptions gives the dock's switches");
		obs_data_t *opt = obs_data_create();
		obs_data_set_string(opt, "option", "snap");
		auto [ok_set, set_json] = vendor_call("SetOption", opt);
		CHECK(ok_set && set_json.find("\"on\":true") != std::string::npos, "SetOption switches one over");
		obs_data_set_string(opt, "mode", "on");
		auto [ok_on, on_json] = vendor_call("SetOption", opt);
		CHECK(ok_on && on_json.find("\"on\":true") != std::string::npos, "SetOption 'on' leaves it on");
		obs_data_set_string(opt, "mode", "off");
		auto [ok_soff, soff_json] = vendor_call("SetOption", opt);
		CHECK(ok_soff && soff_json.find("\"on\":false") != std::string::npos,
		      "SetOption 'off' switches it off");
		obs_data_set_string(opt, "option", "nothing");
		auto [ok_none, none_json] = vendor_call("SetOption", opt);
		CHECK(!ok_none, "an unknown option is refused");
		obs_data_release(opt);
		obs_data_t *slide = obs_data_create();
		obs_data_set_string(slide, "direction", "sideways");
		auto [ok_side, side_json] = vendor_call("Slide", slide);
		CHECK(!ok_side && side_json.find("direction must be") != std::string::npos,
		      "an unknown direction is refused");
		obs_data_set_string(slide, "direction", "left");
		auto [ok_left, left_json] = vendor_call("Slide", slide);
		CHECK(!ok_left && left_json.find("dock") != std::string::npos, "a slide without the dock says so");
		obs_data_release(slide);
	}

	obs_source_filter_remove(img, filter);
	obs_source_release(filter);
	obs_source_release(img);
	obs_shutdown();

	printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
	return g_failures ? 1 : 0;
}
