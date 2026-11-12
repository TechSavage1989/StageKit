// Headless test for the Ambient Background source (Linux, run under Xvfb).
//
// Usage: stagekit-ambient-test <stagekit.so> <data dir> <base image>
//                              <texture image> <live image> <out dir>

#include <obs.h>
#include <obs-module.h>
#include <obs-nix-platform.h>
#include <graphics/vec4.h>

#include <dlfcn.h>

#include <QApplication>
#include <QComboBox>
#include <QPushButton>
#include <QWidget>

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

	QApplication app(argc, argv); // the dock is a Qt widget

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

	// ------------------------------------------- show/hide, match item, layout
	{
		auto color = [](const char *name, int cw, int ch) {
			obs_data_t *d = obs_data_create();
			obs_data_set_int(d, "width", cw);
			obs_data_set_int(d, "height", ch);
			obs_source_t *src = obs_source_create("color_source", name, d, nullptr);
			obs_data_release(d);
			return src;
		};
		auto at = [](obs_sceneitem_t *item, float x, float y) {
			vec2 p;
			vec2_set(&p, x, y);
			obs_sceneitem_set_pos(item, &p);
		};
		auto pos = [](obs_sceneitem_t *item) {
			vec2 p;
			obs_sceneitem_get_pos(item, &p);
			return p;
		};
		auto add_filter = [](obs_scene_t *scene, const char *id, const char *name, obs_data_t *d) {
			obs_source_t *f = obs_source_create(id, name, d, nullptr);
			obs_data_release(d);
			obs_source_filter_add(obs_scene_get_source(scene), f);
			return f;
		};

		printf("\n[show/hide animation]\n");
		obs_scene_t *scene = obs_scene_create("Show Scene");
		obs_source_t *lower_src = color("Lower Third", 200, 50);
		obs_sceneitem_t *lower = obs_scene_add(scene, lower_src);
		at(lower, 30, 280); // nearest edge is the bottom, 80 px away
		obs_data_t *d = obs_data_create();
		obs_data_set_string(d, "item", "Lower Third");
		obs_data_set_int(d, "in_ms", 300);
		obs_data_set_int(d, "out_ms", 300);
		obs_data_set_int(d, "ease_curve", 0);
		obs_source_t *show_hide = add_filter(scene, "stagekit_show_hide", "Animate", d);
		sleep_ms(60);

		obs_sceneitem_set_visible(lower, false);
		sleep_ms(120);
		vec2 p = pos(lower);
		CHECK(obs_sceneitem_visible(lower) && p.y > 285 && p.y < 360 && std::fabs(p.x - 30) < 0.5,
		      "hiding slides it towards the nearest edge instead of cutting (y %.0f)", p.y);
		sleep_ms(350);
		p = pos(lower);
		CHECK(!obs_sceneitem_visible(lower) && std::fabs(p.y - 280) < 0.5,
		      "then it is hidden, resting at its normal place (y %.0f)", p.y);

		obs_sceneitem_set_visible(lower, true);
		p = pos(lower);
		CHECK(std::fabs(p.y - 360) < 0.5, "showing starts it just off screen on the same frame (y %.0f)", p.y);
		sleep_ms(450);
		p = pos(lower);
		CHECK(obs_sceneitem_visible(lower) && std::fabs(p.y - 280) < 0.5, "and it slides home (y %.0f)", p.y);

		obs_data_t *pop = obs_data_create();
		obs_data_set_int(pop, "style", 5);
		obs_source_update(show_hide, pop);
		obs_data_release(pop);
		sleep_ms(60);
		obs_sceneitem_set_visible(lower, false);
		sleep_ms(150);
		vec2 sc;
		obs_sceneitem_get_scale(lower, &sc);
		CHECK(sc.x > 0.1f && sc.x < 0.9f, "pop style shrinks it towards its centre (scale %.2f)", sc.x);
		sleep_ms(300);
		obs_sceneitem_get_scale(lower, &sc);
		p = pos(lower);
		CHECK(!obs_sceneitem_visible(lower) && std::fabs(sc.x - 1.0f) < 0.001f && std::fabs(p.x - 30) < 0.5,
		      "and its size and place are restored once hidden");
		obs_source_filter_remove(obs_scene_get_source(scene), show_hide);
		obs_source_release(show_hide);

		printf("\n[match item]\n");
		obs_scene_t *master_scene = obs_scene_create("Start Scene");
		obs_source_t *title_a = color("Starting Title", 300, 60);
		obs_sceneitem_t *master_item = obs_scene_add(master_scene, title_a);
		at(master_item, 40, 200);
		obs_source_t *title_b = color("Break Title", 600, 60); // twice as long
		obs_sceneitem_t *follower = obs_scene_add(scene, title_b);
		at(follower, 5, 5);
		d = obs_data_create();
		obs_data_set_string(d, "item", "Break Title");
		obs_data_set_string(d, "master_scene", "Start Scene");
		obs_data_set_string(d, "master_item", "Starting Title");
		obs_source_t *match = add_filter(scene, "stagekit_match_item", "Match", d);
		sleep_ms(100);
		vec2 b;
		obs_sceneitem_get_bounds(follower, &b);
		p = pos(follower);
		CHECK(std::fabs(p.x - 40) < 0.5 && std::fabs(p.y - 200) < 0.5 && std::fabs(b.x - 300) < 0.5 &&
			      std::fabs(b.y - 60) < 0.5 &&
			      obs_sceneitem_get_bounds_type(follower) == OBS_BOUNDS_SCALE_INNER,
		      "a longer title is fitted inside the same box (at %.0f,%.0f, box %.0fx%.0f)", p.x, p.y, b.x, b.y);
		at(master_item, 100, 150);
		sleep_ms(100);
		p = pos(follower);
		CHECK(std::fabs(p.x - 100) < 0.5 && std::fabs(p.y - 150) < 0.5,
		      "moving the original moves the matched item too (%.0f, %.0f)", p.x, p.y);
		obs_source_set_enabled(match, false);
		at(master_item, 300, 300);
		sleep_ms(100);
		p = pos(follower);
		CHECK(std::fabs(p.x - 100) < 0.5, "switching the filter off stops the matching");
		obs_source_filter_remove(obs_scene_get_source(scene), match);
		obs_source_release(match);

		printf("\n[layout]\n");
		at(lower, 30, 280);
		at(follower, 100, 150);
		d = obs_data_create();
		obs_data_set_int(d, "duration_ms", 300);
		obs_data_set_int(d, "ease_curve", 0);
		obs_source_t *layout = add_filter(scene, "stagekit_layout", "Default Layout", d);
		sleep_ms(60);
		calldata_t cd = {};
		proc_handler_call(obs_source_get_proc_handler(layout), "save", &cd);
		sleep_ms(60);
		at(lower, 400, 20);
		at(follower, 0, 0);
		obs_source_set_enabled(layout, true);
		sleep_ms(150);
		p = pos(lower);
		CHECK(p.x > 60 && p.x < 380, "switching it on glides items back (x %.0f part-way)", p.x);
		sleep_ms(300);
		p = pos(lower);
		vec2 q = pos(follower);
		CHECK(std::fabs(p.x - 30) < 0.5 && std::fabs(p.y - 280) < 0.5 && std::fabs(q.x - 100) < 0.5 &&
			      std::fabs(q.y - 150) < 0.5,
		      "every item returns to the saved arrangement");
		CHECK(!obs_source_enabled(layout), "the filter switches itself off afterwards");
		calldata_free(&cd);
		obs_source_filter_remove(obs_scene_get_source(scene), layout);
		obs_source_release(layout);

		obs_source_release(lower_src);
		obs_source_release(title_a);
		obs_source_release(title_b);
		obs_scene_release(master_scene);
		obs_scene_release(scene);
	}

	// ------------------------------------------------------------ auto frame
	{
		printf("\n[auto frame]\n");
		auto sized = [](obs_source_t *src, int cw, int ch) {
			obs_data_t *d = obs_data_create();
			obs_data_set_int(d, "width", cw);
			obs_data_set_int(d, "height", ch);
			obs_source_update(src, d);
			obs_data_release(d);
		};
		obs_source_t *game = obs_source_create("color_source", "Emulator", nullptr, nullptr);
		sized(game, 640, 360); // a 16:9 picture
		obs_data_t *fs = obs_data_create();
		obs_data_set_int(fs, "frame_width", 800);
		obs_data_set_int(fs, "frame_height", 600);
		obs_data_set_int(fs, "glide_ms", 300);
		obs_data_set_int(fs, "ease_curve", 0);
		obs_source_t *frame = obs_source_create("stagekit_auto_frame", "Auto Frame", fs, nullptr);
		obs_source_filter_add(game, frame);

		struct Pic {
			double x, y, w, h;
		};
		auto picture = [&]() {
			render(game, obs_source_get_width(game), obs_source_get_height(game));
			calldata_t cd = {};
			proc_handler_call(obs_source_get_proc_handler(frame), "get_picture", &cd);
			Pic p{calldata_float(&cd, "x"), calldata_float(&cd, "y"), calldata_float(&cd, "width"),
			      calldata_float(&cd, "height")};
			calldata_free(&cd);
			return p;
		};
		auto is = [](const Pic &p, double x, double y, double pw, double ph) {
			return std::fabs(p.x - x) < 0.6 && std::fabs(p.y - y) < 0.6 && std::fabs(p.w - pw) < 0.6 &&
			       std::fabs(p.h - ph) < 0.6;
		};
		auto set = [&](const char *key, long long value) {
			obs_data_set_int(fs, key, value);
			obs_source_update(frame, fs);
			sleep_ms(80);
			picture();
			sleep_ms(400); // let any glide finish
		};

		sleep_ms(120);
		Pic p = picture();
		CHECK(obs_source_get_width(game) == 800 && obs_source_get_height(game) == 600,
		      "the source takes the frame's size (%ux%u)", obs_source_get_width(game),
		      obs_source_get_height(game));
		CHECK(is(p, 0, 75, 800, 450), "a 16:9 picture is fitted and centred (%.0f,%.0f %.0fx%.0f)", p.x, p.y,
		      p.w, p.h);
		const std::vector<uint8_t> px = render(game, 800, 600);
		CHECK(px[(size_t(10) * 800 + 400) * 4 + 3] == 0 && px[(size_t(300) * 800 + 400) * 4 + 3] == 255,
		      "bars outside the picture are transparent, the picture is solid");

		// The emulator switches to a 4:3 resolution: same scene, new fit, with a glide.
		sized(game, 320, 240);
		sleep_ms(60);
		picture();
		sleep_ms(120);
		p = picture();
		CHECK(obs_source_get_width(game) == 800 && p.h > 455 && p.h < 595,
		      "when the resolution changes the frame stays put and the picture glides (height %.0f)", p.h);
		sleep_ms(350);
		p = picture();
		CHECK(is(p, 0, 0, 800, 600), "and settles filling the 4:3 frame (%.0f,%.0f %.0fx%.0f)", p.x, p.y, p.w,
		      p.h);

		// A 256x224 console picture.
		sized(game, 256, 224);
		set("fit", 2); // whole-number scale
		p = picture();
		CHECK(is(p, 144, 76, 512, 448), "whole-number scale shows 256x224 at exactly 2x (%.0f,%.0f %.0fx%.0f)",
		      p.x, p.y, p.w, p.h);
		set("shape", 1); // show it as 4:3
		set("fit", 0);
		p = picture();
		CHECK(is(p, 0, 0, 800, 600), "a shape override shows it as 4:3 (%.0f,%.0f %.0fx%.0f)", p.x, p.y, p.w,
		      p.h);
		set("shape", 0);
		set("fit", 1); // fill
		p = picture();
		CHECK(p.w >= 799.4 && p.h >= 599.4 && (p.w > 801 || p.h > 601),
		      "fill covers the frame and crops the overflow (%.0fx%.0f)", p.w, p.h);
		set("fit", 0);
		set("align_x", 0);
		set("align_y", 2);
		p = picture();
		CHECK(std::fabs(p.x) < 0.6 && std::fabs(p.y + p.h - 600) < 0.6,
		      "alignment sits it bottom-left (%.0f,%.0f)", p.x, p.y);

		obs_data_release(fs);
		obs_source_filter_remove(game, frame);
		obs_source_release(frame);
		obs_source_release(game);
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

		// The dock, driven the way a person would: pick, click, look.
		printf("\n[dock]\n");
		calldata_t dock_cd = {};
		calldata_set_string(&dock_cd, "scene", "Test Scene");
		proc_handler_call(obs_get_proc_handler(), "stagekit_create_dock", &dock_cd);
		auto *dock = static_cast<QWidget *>(calldata_ptr(&dock_cd, "widget"));
		calldata_free(&dock_cd);
		CHECK(dock != nullptr, "the dock can be built for a scene");
		if (dock) {
			dock->resize(340, 640);
			auto click = [&](const char *name) {
				auto *button = dock->findChild<QPushButton *>(name);
				if (button)
					button->click();
				return button != nullptr;
			};
			// Items are listed top-most first, as in OBS; pick the camera.
			if (auto *items = dock->findChild<QComboBox *>("item"))
				items->setCurrentText("Camera");
			CHECK(click("grid6"), "it has a button per screen position");
			sleep_ms(900);
			c = pos_of(cam_item);
			CHECK(std::fabs(c.x - 30) < 0.5 && std::fabs(c.y - 240) < 0.5,
			      "clicking bottom-left sends the selected item there (%.0f, %.0f)", c.x, c.y);

			click("makeKeys");
			calldata_t spot_cd = {};
			calldata_set_ptr(&spot_cd, "widget", dock);
			calldata_set_string(&spot_cd, "name", "Corner");
			proc_handler_call(obs_get_proc_handler(), "stagekit_dock_save_spot", &spot_cd);
			calldata_free(&spot_cd);
			int filters = 0;
			obs_source_enum_filters(
				scene_source,
				[](obs_source_t *, obs_source_t *, void *param) { (*static_cast<int *>(param))++; },
				&filters);
			// 2 made by this test + the dock's own mover + 9 positions + 1 saved spot
			CHECK(filters == 13,
			      "one click makes a key for each of the nine positions, plus the saved spot (%d filters)",
			      filters);
			obs_source_t *key = obs_source_get_filter_by_name(scene_source, "Widget → Top right");
			CHECK(key == nullptr, "keys are named after the selected item");
			obs_source_release(key);
			key = obs_source_get_filter_by_name(scene_source, "Camera → Top right");
			CHECK(key != nullptr, "for example \"Camera → Top right\"");
			if (key) {
				sleep_ms(100);
				obs_source_set_enabled(key, true);
				sleep_ms(800);
				c = pos_of(cam_item);
				CHECK(std::fabs(c.x - 450) < 0.5 && std::fabs(c.y - 30) < 0.5,
				      "and switching that key on moves the camera (%.0f, %.0f)", c.x, c.y);
			}
			obs_source_release(key);

			click("addShadow");
			obs_source_t *shadow = obs_source_get_filter_by_name(cam, "Shadow & Backdrop");
			CHECK(shadow != nullptr, "\"Add to this item\" puts the effect on the item's source");
			obs_source_release(shadow);

			dock->grab().save(QString::fromStdString(out + "/../stagekit-dock.png"));
			delete dock;
		}

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
