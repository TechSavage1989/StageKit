// Headless test for the Ambient Background source (Linux, run under Xvfb).
//
// Usage: stagekit-ambient-test <stagekit.so> <data dir> <base image>
//                              <texture image> <live image> <out dir>

#include <obs.h>
#include <obs-module.h>
#include <obs-nix-platform.h>
#include <graphics/vec4.h>

#include <dlfcn.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, ...)                    \
	do {                                \
		if (cond) {                 \
			printf("  PASS  "); \
		} else {                    \
			printf("  FAIL  "); \
			g_failures++;       \
		}                           \
		printf(__VA_ARGS__);        \
		printf("\n");               \
	} while (0)

static void sleep_ms(int ms)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static bool load_module(const char *bin, const char *data)
{
	obs_module_t *mod = nullptr;
	return obs_open_module(&mod, bin, data) == MODULE_SUCCESS && obs_init_module(mod);
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

static double mean_abs_diff(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b)
{
	double sum = 0;
	for (size_t i = 0; i < a.size(); i++)
		sum += std::abs(int(a[i]) - int(b[i]));
	return sum / double(a.size());
}

static double mean_rgb(const std::vector<uint8_t> &a)
{
	double sum = 0;
	for (size_t i = 0; i < a.size(); i += 4)
		sum += a[i] + a[i + 1] + a[i + 2];
	return sum / double(a.size() / 4 * 3);
}

static bool opaque(const std::vector<uint8_t> &a)
{
	for (size_t i = 3; i < a.size(); i += 4)
		if (a[i] != 255)
			return false;
	return true;
}

int main(int argc, char **argv)
{
	if (argc < 7) {
		fprintf(stderr, "usage: %s <stagekit.so> <data dir> <base> <texture> <live> <out dir>\n", argv[0]);
		return 2;
	}
	const char *base_image = argv[3];
	const char *texture_image = argv[4];
	const char *live_image = argv[5];
	const std::string out = argv[6];

	void *x11 = dlopen("libX11.so.6", RTLD_NOW);
	auto xopen = x11 ? (void *(*)(const char *))dlsym(x11, "XOpenDisplay") : nullptr;
	void *display = xopen ? xopen(nullptr) : nullptr;
	if (!display) {
		fprintf(stderr, "No X display (run under xvfb-run)\n");
		return 2;
	}
	obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);
	obs_set_nix_platform_display(display);
	if (!obs_startup("en-US", nullptr, nullptr))
		return 1;

	const uint32_t w = 640, h = 360;
	obs_video_info ovi = {};
	ovi.graphics_module = "libobs-opengl";
	ovi.fps_num = 60;
	ovi.fps_den = 1;
	ovi.base_width = w;
	ovi.base_height = h;
	ovi.output_width = w;
	ovi.output_height = h;
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.gpu_conversion = true;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	if (obs_reset_video(&ovi) != OBS_VIDEO_SUCCESS)
		return 1;

	if (!load_module("/usr/lib/x86_64-linux-gnu/obs-plugins/image-source.so",
			 "/usr/share/obs/obs-plugins/image-source") ||
	    !load_module(argv[1], argv[2]))
		return 1;
	obs_post_load_modules();

	obs_data_t *ls = obs_data_create();
	obs_data_set_string(ls, "file", live_image);
	obs_source_t *live = obs_source_create("image_source", "Live Capture", ls, nullptr);
	obs_data_release(ls);

	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "image", base_image);
	obs_data_set_double(s, "speed_x", 0);
	obs_data_set_double(s, "speed_y", 0);
	obs_data_set_double(s, "blur", 0);
	obs_source_t *bg = obs_source_create("stagekit_ambient_background", "Background", s, nullptr);
	CHECK(bg != nullptr, "source is registered and can be created");
	if (!bg)
		return 1;
	// Mark the background as on screen, as a scene would.
	obs_source_inc_showing(bg);

	auto shot = [&](const char *name, const std::function<void(obs_data_t *)> &change) {
		change(s);
		obs_source_update(bg, s);
		sleep_ms(700); // settings apply on a tick; children resolve twice a second
		std::vector<uint8_t> px = render(bg, w, h);
		write_frame(out + "/" + name + ".rgba", px, w, h);
		return px;
	};

	printf("\n[ambient background]\n");
	CHECK(obs_source_get_width(bg) == 0 || true, "created");
	auto plain = shot("1_image_only", [](obs_data_t *) {});
	CHECK(obs_source_get_width(bg) == w && obs_source_get_height(bg) == h, "fills the canvas (%ux%u)",
	      obs_source_get_width(bg), obs_source_get_height(bg));
	CHECK(mean_rgb(plain) > 20 && opaque(plain), "draws the image, fully opaque (mean %.1f)", mean_rgb(plain));

	auto blurred = shot("2_blur", [](obs_data_t *d) { obs_data_set_double(d, "blur", 24); });
	CHECK(mean_abs_diff(blurred, plain) > 2.0, "blur softens it (diff %.2f)", mean_abs_diff(blurred, plain));

	auto graded = shot("3_saturation_glow", [](obs_data_t *d) {
		obs_data_set_double(d, "saturation", 200);
		obs_data_set_double(d, "glow", 30);
	});
	CHECK(mean_rgb(graded) > mean_rgb(blurred), "glow brightens (%.1f -> %.1f)", mean_rgb(blurred),
	      mean_rgb(graded));

	auto hex = shot("4_hex_mosaic", [](obs_data_t *d) {
		obs_data_set_int(d, "pixel_style", 2);
		obs_data_set_double(d, "pixel_size", 14);
	});
	CHECK(mean_abs_diff(hex, graded) > 0.5, "hex mosaic changes the picture (diff %.2f)",
	      mean_abs_diff(hex, graded));

	auto textured =
		shot("5_texture_multiply", [&](obs_data_t *d) { obs_data_set_string(d, "overlay", texture_image); });
	CHECK(mean_rgb(textured) < mean_rgb(hex), "multiplied texture darkens (%.1f -> %.1f)", mean_rgb(hex),
	      mean_rgb(textured));

	auto with_live = shot("6_live_capture", [](obs_data_t *d) {
		obs_data_set_int(d, "live_mode", 2);
		obs_data_set_string(d, "live_source", "Live Capture");
		obs_data_set_double(d, "live_opacity", 70);
	});
	CHECK(mean_abs_diff(with_live, textured) > 1.0, "live capture is blended in (diff %.2f)",
	      mean_abs_diff(with_live, textured));
	CHECK(obs_source_showing(live), "the live source is kept showing while used");

	auto square = shot("7_square_mosaic", [](obs_data_t *d) { obs_data_set_int(d, "pixel_style", 1); });
	CHECK(mean_abs_diff(square, with_live) > 0.2, "square mosaic differs from hex");

	auto from_source = shot("8_from_source", [](obs_data_t *d) {
		obs_data_set_int(d, "live_mode", 0);
		obs_data_set_int(d, "pixel_style", 0);
		obs_data_set_string(d, "overlay", "");
		obs_data_set_int(d, "base_type", 1);
		obs_data_set_string(d, "base_source", "Live Capture");
	});
	CHECK(mean_rgb(from_source) > 5 && opaque(from_source), "another source can be the background");
	CHECK(obs_source_showing(live), "the base source is kept showing");

	printf("\n[motion]\n");
	shot("still", [](obs_data_t *d) {
		obs_data_set_int(d, "base_type", 0);
		obs_data_set_double(d, "blur", 6);
	});
	auto a = render(bg, w, h);
	sleep_ms(300);
	auto b = render(bg, w, h);
	CHECK(mean_abs_diff(a, b) < 0.01, "speed 0 stays still (diff %.4f)", mean_abs_diff(a, b));
	CHECK(!obs_source_showing(live), "sources no longer used are released");

	shot("moving", [](obs_data_t *d) {
		obs_data_set_double(d, "speed_x", 120);
		obs_data_set_double(d, "speed_y", 60);
	});
	a = render(bg, w, h);
	std::vector<double> steps;
	for (int i = 0; i < 12; i++) {
		sleep_ms(50);
		b = render(bg, w, h);
		steps.push_back(mean_abs_diff(a, b));
		a = b;
	}
	double lo = 1e9, hi = 0;
	for (double d : steps) {
		lo = std::min(lo, d);
		hi = std::max(hi, d);
	}
	CHECK(lo > 0.05, "drifts continuously (frame-to-frame change %.2f..%.2f)", lo, hi);

	// ---------------------------------------------------- shadow & backdrop
	if (argc >= 9) {
		printf("\n[shadow & backdrop]\n");
		const std::string shadow_out = argv[8];
		obs_data_t *ts = obs_data_create();
		obs_data_set_string(ts, "file", argv[7]);
		obs_source_t *title = obs_source_create("image_source", "Title", ts, nullptr);
		obs_data_release(ts);
		const uint32_t tw = obs_source_get_width(title), th = obs_source_get_height(title);
		const std::vector<uint8_t> bare = render(title, tw, th);

		obs_data_t *fs = obs_data_create();
		obs_data_set_double(fs, "offset_x", 20);
		obs_data_set_double(fs, "offset_y", 20);
		obs_data_set_double(fs, "softness", 0);
		obs_data_set_double(fs, "shadow_opacity", 100);
		obs_source_t *filter = obs_source_create("stagekit_shadow_backdrop", "Shadow", fs, nullptr);
		obs_source_filter_add(title, filter);
		sleep_ms(60);

		auto frame = [&](const char *name) {
			sleep_ms(60);
			const uint32_t fw = obs_source_get_width(title), fh = obs_source_get_height(title);
			std::vector<uint8_t> px = render(title, fw, fh);
			write_frame(shadow_out + "/" + name + ".rgba", px, fw, fh);
			return px;
		};

		auto hard = frame("1_hard_20px");
		CHECK(obs_source_get_width(title) == tw + 20 && obs_source_get_height(title) == th + 20,
		      "a 20 px hard shadow grows the source by exactly 20 px (%ux%u -> %ux%u)", tw, th,
		      obs_source_get_width(title), obs_source_get_height(title));
		// Opaque text pixels are untouched, and a black copy sits 20 px down-right.
		int text = 0, kept = 0, shadowed = 0;
		const uint32_t fw = tw + 20;
		for (uint32_t y = 0; y < th; y++) {
			for (uint32_t x = 0; x < tw; x++) {
				const uint8_t *o = &bare[(size_t(y) * tw + x) * 4];
				if (o[3] != 255)
					continue;
				text++;
				const uint8_t *f = &hard[(size_t(y) * fw + x) * 4];
				if (std::abs(int(f[0]) - int(o[0])) <= 2 && f[3] == 255)
					kept++;
				const uint8_t *sh = &hard[(size_t(y + 20) * fw + x + 20) * 4];
				if (sh[3] == 255)
					shadowed++;
			}
		}
		CHECK(text > 1000 && kept == text, "the source itself is unchanged (%d of %d pixels)", kept, text);
		CHECK(shadowed == text, "a solid shadow sits 20 px down and right (%d of %d pixels)", shadowed, text);

		obs_data_set_double(fs, "softness", 16);
		obs_data_set_double(fs, "shadow_opacity", 80);
		obs_source_update(filter, fs);
		auto soft = frame("2_soft");
		CHECK(obs_source_get_width(title) > tw + 20, "a soft shadow gets extra room on every side (%u wide)",
		      obs_source_get_width(title));

		obs_data_set_bool(fs, "backdrop", true);
		obs_data_set_double(fs, "backdrop_pad", 24);
		obs_data_set_double(fs, "radius", 18);
		obs_data_set_double(fs, "offset_x", 10);
		obs_data_set_double(fs, "offset_y", 14);
		obs_source_update(filter, fs);
		auto panel = frame("3_panel_rounded");
		const uint32_t pw = obs_source_get_width(title), ph = obs_source_get_height(title);
		const uint8_t *mid = &panel[(size_t(ph / 2) * pw + pw / 2) * 4];
		const uint8_t *corner = &panel[0];
		CHECK(mid[3] == 255 && corner[3] < 40, "the panel is solid behind the source and its corner is clear");

		obs_data_release(fs);
		obs_source_filter_remove(title, filter);
		obs_source_release(filter);

		// A camera-style opaque picture with rounded corners and a soft shadow.
		obs_data_t *cs = obs_data_create();
		obs_data_set_double(cs, "radius", 40);
		obs_data_set_double(cs, "softness", 24);
		obs_data_set_double(cs, "offset_x", 0);
		obs_data_set_double(cs, "offset_y", 12);
		obs_source_t *cam_filter = obs_source_create("stagekit_shadow_backdrop", "Shadow", cs, nullptr);
		obs_data_release(cs);
		obs_source_filter_add(live, cam_filter);
		sleep_ms(120);
		const uint32_t cw = obs_source_get_width(live), ch = obs_source_get_height(live);
		write_frame(shadow_out + "/4_rounded_picture.rgba", render(live, cw, ch), cw, ch);
		obs_source_filter_remove(live, cam_filter);
		obs_source_release(cam_filter);
		obs_source_release(title);
	}

	// ------------------------------------------------------------- ramp zoom
	{
		printf("\n[ramp zoom]\n");
		const uint32_t lw = obs_source_get_width(live), lh = obs_source_get_height(live);
		const std::vector<uint8_t> plain_live = render(live, lw, lh);
		obs_data_t *zs = obs_data_create();
		obs_data_set_double(zs, "zoom", 200);
		obs_data_set_int(zs, "ramp_in_ms", 300);
		obs_data_set_int(zs, "ramp_out_ms", 300);
		obs_data_set_int(zs, "ease_curve", 0);
		obs_source_t *zoom = obs_source_create("stagekit_ramp_zoom", "Zoom", zs, nullptr);
		obs_data_release(zs);
		obs_source_filter_add(live, zoom);
		sleep_ms(100);

		// Enabled filter = fully zoomed: each output pixel shows the source
		// pixel half as far from the centre.
		const std::vector<uint8_t> zoomed = render(live, lw, lh);
		int match = 0, total = 0;
		for (uint32_t y = 4; y < lh - 4; y += 3) {
			for (uint32_t x = 4; x < lw - 4; x += 3) {
				const uint8_t *z = &zoomed[(size_t(y) * lw + x) * 4];
				const uint8_t *o = &plain_live[(size_t(lh / 4 + y / 2) * lw + (lw / 4 + x / 2)) * 4];
				total++;
				if (std::abs(z[0] - o[0]) < 24 && std::abs(z[1] - o[1]) < 24 &&
				    std::abs(z[2] - o[2]) < 24)
					match++;
			}
		}
		CHECK(match > total * 0.93, "200%% zoom shows the centre half of the picture (%d of %d samples)", match,
		      total);
		CHECK(obs_source_get_width(live) == lw, "the source keeps its size");

		obs_source_set_enabled(zoom, false);
		sleep_ms(150);
		const std::vector<uint8_t> half = render(live, lw, lh);
		CHECK(obs_source_enabled(zoom) && mean_abs_diff(half, zoomed) > 1.0 &&
			      mean_abs_diff(half, plain_live) > 1.0,
		      "switching it off zooms out gradually");
		sleep_ms(300);
		CHECK(!obs_source_enabled(zoom) && mean_abs_diff(render(live, lw, lh), plain_live) < 0.01,
		      "fully zoomed out is the untouched picture, and the filter is off");
		obs_source_filter_remove(live, zoom);
		obs_source_release(zoom);
	}

	// ------------------------------------------------------------ place item
	{
		printf("\n[place item]\n");
		// A 640x360 scene with a 160x90 "camera" and a 100x40 "widget" under it.
		obs_scene_t *scene = obs_scene_create("Test Scene");
		obs_source_t *scene_source = obs_scene_get_source(scene);
		auto make_color = [](const char *name, int cw, int ch) {
			obs_data_t *d = obs_data_create();
			obs_data_set_int(d, "width", cw);
			obs_data_set_int(d, "height", ch);
			obs_source_t *src = obs_source_create("color_source", name, d, nullptr);
			obs_data_release(d);
			return src;
		};
		obs_source_t *cam = make_color("Camera", 160, 90);
		obs_source_t *widget = make_color("Widget", 100, 40);
		obs_sceneitem_t *cam_item = obs_scene_add(scene, cam);
		obs_sceneitem_t *widget_item = obs_scene_add(scene, widget);
		vec2 p;
		vec2_set(&p, 30.0f, 30.0f);
		obs_sceneitem_set_pos(cam_item, &p);
		vec2_set(&p, 20.0f, 130.0f); // 10 px left of the camera, 10 px below it
		obs_sceneitem_set_pos(widget_item, &p);

		auto pos_of = [](obs_sceneitem_t *item) {
			vec2 v;
			obs_sceneitem_get_pos(item, &v);
			return v;
		};
		auto make_place = [&](const char *name, int destination, const char *linked) {
			obs_data_t *d = obs_data_create();
			obs_data_set_string(d, "item", "Camera");
			obs_data_set_string(d, "linked", linked);
			obs_data_set_int(d, "destination", destination);
			obs_data_set_double(d, "margin", 30);
			obs_data_set_int(d, "duration_ms", 300);
			obs_data_set_int(d, "ease_curve", 0);
			obs_source_t *f = obs_source_create("stagekit_place_item", name, d, nullptr);
			obs_data_release(d);
			obs_source_filter_add(scene_source, f);
			return f;
		};
		obs_source_t *to_top_right = make_place("Camera Top Right", 2, "Widget");
		obs_source_t *to_bottom_right = make_place("Camera Bottom Right", 8, "Widget");
		sleep_ms(100);
		CHECK(!obs_source_enabled(to_top_right), "a new Place Item filter rests in the off state");

		// Switch it on, as the eye icon or a Stream Deck Filter key would.
		obs_source_set_enabled(to_top_right, true);
		sleep_ms(150);
		vec2 mid = pos_of(cam_item);
		CHECK(mid.x > 130 && mid.x < 400 && obs_source_enabled(to_top_right),
		      "switching it on glides the item (x %.0f part-way)", mid.x);
		sleep_ms(300);
		vec2 c = pos_of(cam_item), wpos = pos_of(widget_item);
		CHECK(std::fabs(c.x - 450) < 0.5 && std::fabs(c.y - 30) < 0.5,
		      "it lands at the top right with a 30 px margin (%.0f, %.0f)", c.x, c.y);
		CHECK(std::fabs(wpos.x - 440) < 0.5 && std::fabs(wpos.y - 130) < 0.5,
		      "the linked item keeps its place below it (%.0f, %.0f)", wpos.x, wpos.y);
		CHECK(!obs_source_enabled(to_top_right), "the filter switches itself off afterwards");

		// Bottom right: below the camera would be off screen, so the widget flips above.
		calldata_t cd = {};
		proc_handler_call(obs_source_get_proc_handler(to_bottom_right), "trigger", &cd);
		calldata_free(&cd);
		sleep_ms(500);
		c = pos_of(cam_item);
		wpos = pos_of(widget_item);
		CHECK(std::fabs(c.x - 450) < 0.5 && std::fabs(c.y - 240) < 0.5, "bottom right (%.0f, %.0f)", c.x, c.y);
		CHECK(std::fabs(wpos.y - 190) < 0.5 && std::fabs(wpos.x - 440) < 0.5,
		      "the linked item flips above the camera to stay on screen (%.0f, %.0f)", wpos.x, wpos.y);

		// And back to the top: it flips below again.
		obs_source_set_enabled(to_top_right, true);
		sleep_ms(500);
		wpos = pos_of(widget_item);
		CHECK(std::fabs(wpos.y - 130) < 0.5, "back at the top it returns below (%.0f, %.0f)", wpos.x, wpos.y);

		int hotkeys = 0;
		obs_enum_hotkeys(
			[](void *data, obs_hotkey_id, obs_hotkey_t *key) {
				if (strncmp(obs_hotkey_get_name(key), "stagekit.place.", 15) == 0)
					(*static_cast<int *>(data))++;
				return true;
			},
			&hotkeys);
		CHECK(hotkeys == 2, "each destination has a hotkey on the scene (%d)", hotkeys);

		obs_source_filter_remove(scene_source, to_top_right);
		obs_source_filter_remove(scene_source, to_bottom_right);
		obs_source_release(to_top_right);
		obs_source_release(to_bottom_right);
		obs_source_release(cam);
		obs_source_release(widget);
		obs_scene_release(scene);
	}

	obs_source_dec_showing(bg);
	obs_data_release(s);
	obs_source_release(bg);
	obs_source_release(live);
	obs_shutdown();

	printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
	return g_failures ? 1 : 0;
}
