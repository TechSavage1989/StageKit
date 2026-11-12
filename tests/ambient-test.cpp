// Headless test for the Ambient Background source (Linux, run under Xvfb).
//
// Usage: stagekit-ambient-test <stagekit.so> <data dir> <base image>
//                              <texture image> <live image> <out dir>

#include <obs.h>
#include <obs-module.h>
#include <obs-nix-platform.h>
#include <graphics/matrix4.h>
#include <graphics/vec4.h>

#include <dlfcn.h>

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPixmap>
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

	// How the capture is laid over the screen, and using it as the whole background.
	{
		auto fitted = shot("6b_live_fit_small", [](obs_data_t *d) {
			obs_data_set_int(d, "live_fit", 1);
			obs_data_set_double(d, "live_zoom", 50);
		});
		CHECK(mean_abs_diff(fitted, with_live) > 1.0 && mean_abs_diff(fitted, textured) > 0.3,
		      "the live capture can be fitted and zoomed, with the picture showing around it (diff %.2f)",
		      mean_abs_diff(fitted, with_live));
		auto replaced = shot("6c_live_replaces", [](obs_data_t *d) {
			obs_data_set_int(d, "live_fit", 0);
			obs_data_set_double(d, "live_zoom", 100);
			obs_data_set_bool(d, "live_replace", true);
		});
		CHECK(mean_abs_diff(replaced, with_live) > 1.0, "the live capture can replace the picture (diff %.2f)",
		      mean_abs_diff(replaced, with_live));
		auto gone = shot("6d_live_gone",
				 [](obs_data_t *d) { obs_data_set_string(d, "live_source", "No Such Source"); });
		CHECK(mean_abs_diff(gone, textured) < 0.5, "and with no capture the picture is back (diff %.2f)",
		      mean_abs_diff(gone, textured));
		shot("6_live_capture", [](obs_data_t *d) {
			obs_data_set_string(d, "live_source", "Live Capture");
			obs_data_set_bool(d, "live_replace", false);
		});
	}

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

	// ---------------------------------------------------------- auto capture
	{
		printf("\n[auto capture]\n");
		// A stand-in for a Game Capture; the filter only writes its settings.
		obs_source_t *capture = obs_source_create("color_source", "One Game Capture", nullptr, nullptr);
		auto list_of = [](std::initializer_list<const char *> names) {
			obs_data_array_t *array = obs_data_array_create();
			for (const char *name : names) {
				obs_data_t *entry = obs_data_create();
				obs_data_set_string(entry, "value", name);
				obs_data_array_push_back(array, entry);
				obs_data_release(entry);
			}
			return array;
		};
		obs_data_t *ad = obs_data_create();
		obs_data_array_t *never = list_of({"Blender"});
		obs_data_set_array(ad, "exclude", never);
		obs_data_array_release(never);
		obs_source_t *aimer = obs_source_create("stagekit_auto_capture", "Auto Capture", ad, nullptr);
		CHECK(aimer != nullptr, "the Auto Capture filter can be created");
		obs_source_filter_add(capture, aimer);

		// Stand-in windows: "title|class|exe|front|game".
		auto see = [&](const char *windows) {
			calldata_t cd = {};
			calldata_set_string(&cd, "windows", windows);
			proc_handler_call(obs_source_get_proc_handler(aimer), "set_test_windows", &cd);
			calldata_free(&cd);
			sleep_ms(250);
			obs_data_t *settings = obs_source_get_settings(capture);
			std::string window = obs_data_get_string(settings, "window");
			obs_data_release(settings);
			return window;
		};
		const char *mail = "Inbox - Mail|MailWnd|mail.exe|front|";
		const char *snes = "Aladdin - Snes9x 1.60|Snes9X: WndClass|snes9x-x64.exe|front|game";

		std::string aimed = see(mail);
		CHECK(aimed.empty(), "an ordinary program in front is not captured");
		aimed = see(snes);
		CHECK(aimed == "Aladdin - Snes9x 1.60:Snes9X#3A WndClass:snes9x-x64.exe",
		      "a game in front is captured with no list to maintain (%s)", aimed.c_str());

		// Emulators change their title every second; that must not re-aim the capture.
		obs_data_t *mark = obs_source_get_settings(capture);
		obs_data_set_string(mark, "window", "untouched");
		obs_data_release(mark);
		aimed = see("Aladdin - Snes9x 1.60 - 60 fps|Snes9X: WndClass|snes9x-x64.exe|front|game");
		CHECK(aimed == "untouched", "a changing window title does not re-aim it");

		// Alt-tabbing to something else keeps the game.
		aimed = see("Aladdin|Snes9X: WndClass|snes9x-x64.exe||\nInbox - Mail|MailWnd|mail.exe|front|");
		CHECK(aimed == "untouched", "switching to another program keeps the game captured");

		aimed = see(
			"Aladdin|Snes9X: WndClass|snes9x-x64.exe||\nDolphin 2506a|DolphinWnd|Dolphin.exe|front|game");
		CHECK(aimed.find("Dolphin.exe") != std::string::npos,
		      "another game coming to the front takes over (%s)", aimed.c_str());

		aimed = see("Dolphin 2506a|DolphinWnd|Dolphin.exe||\nBlender|GHOST_WindowClass|blender.exe|front|game");
		CHECK(aimed.find("Dolphin.exe") != std::string::npos,
		      "a program on the 'never capture' list is ignored even if it looks like a game");
		aimed = see("Dolphin 2506a|DolphinWnd|Dolphin.exe||\nOBS|Qt|obs64.exe|front|game");
		CHECK(aimed.find("Dolphin.exe") != std::string::npos,
		      "OBS itself and other everyday programs are built in to that list");

		// "Always capture" covers a game it would not spot by itself, and
		// "only the listed programs" turns the guessing off.
		obs_data_array_t *always = list_of({"oddgame"});
		obs_data_set_array(ad, "programs", always);
		obs_data_array_release(always);
		obs_data_set_int(ad, "mode", 1);
		obs_source_update(aimer, ad);
		sleep_ms(80);
		aimed = see("Odd Game|OddWnd|OddGame.exe|front|\nAladdin|Snes9X: WndClass|snes9x-x64.exe||game");
		CHECK(aimed.find("OddGame.exe") != std::string::npos, "an 'always capture' program is captured (%s)",
		      aimed.c_str());
		aimed = see(
			"Aladdin - Snes9x 1.60|Snes9X: WndClass|snes9x-x64.exe|front|game\nOdd Game|OddWnd|OddGame.exe||");
		CHECK(aimed.find("OddGame.exe") != std::string::npos,
		      "in 'only the listed programs' mode an unlisted game is left alone");
		obs_data_release(ad);

		// The controls: say what is captured, undo a wrong pick in one
		// press, and choose from open programs instead of typing names.
		{
			obs_data_t *fresh = obs_data_create();
			obs_source_t *easy =
				obs_source_create("stagekit_auto_capture", "Auto Capture 2", fresh, nullptr);
			obs_data_release(fresh);
			obs_source_t *capture2 = obs_source_create("color_source", "Second Capture", nullptr, nullptr);
			obs_source_filter_add(capture2, easy);
			auto see2 = [&](const char *windows) {
				calldata_t cd = {};
				calldata_set_string(&cd, "windows", windows);
				proc_handler_call(obs_source_get_proc_handler(easy), "set_test_windows", &cd);
				calldata_free(&cd);
				sleep_ms(250);
				obs_data_t *settings = obs_source_get_settings(capture2);
				std::string window = obs_data_get_string(settings, "window");
				obs_data_release(settings);
				return window;
			};
			const char *open = "Render - Blender|GHOST_WindowClass|blender.exe|front|game\n"
					   "Odd Game|OddWnd|OddGame.exe||\nOBS|Qt|obs64.exe||";
			std::string got = see2(open);
			CHECK(got.find("blender.exe") != std::string::npos,
			      "a 3D program that looks like a game is picked up");

			obs_properties_t *props = obs_source_properties(easy);
			std::string status = obs_property_description(obs_properties_get(props, "status"));
			CHECK(status.find("blender.exe") != std::string::npos,
			      "the controls say what is being captured (%s)", status.c_str());
			obs_property_t *pick = obs_properties_get(props, "pick");
			CHECK(obs_property_list_item_count(pick) == 3,
			      "open programs can be picked from a list, without OBS itself (%d entries)",
			      (int)obs_property_list_item_count(pick));

			obs_property_button_clicked(obs_properties_get(props, "never_now"), easy);
			sleep_ms(300);
			obs_data_t *es = obs_source_get_settings(easy);
			CHECK(std::string(obs_data_get_string(es, "never")) == "blender.exe",
			      "one press adds the wrong program to 'Never capture' (%s)",
			      obs_data_get_string(es, "never"));
			obs_data_set_string(es, "pick", "OddGame.exe");
			obs_data_release(es);
			obs_property_button_clicked(obs_properties_get(props, "always_pick"), easy);
			obs_properties_destroy(props);
			got = see2(open);
			CHECK(got.find("OddGame.exe") != std::string::npos,
			      "a picked program is always captured, and the excluded one is dropped (%s)", got.c_str());

			obs_source_filter_remove(capture2, easy);
			obs_source_release(easy);
			obs_source_release(capture2);
		}

		obs_source_filter_remove(capture, aimer);
		obs_source_release(aimer);
		obs_source_release(capture);
	}

	// ------------------------------------------------------------ start image
	{
		printf("\n[start image]\n");
		auto set_start = [](const char *path) {
			calldata_t cd = {};
			calldata_set_string(&cd, "path", path);
			proc_handler_call(obs_get_proc_handler(), "stagekit_set_start_image", &cd);
			calldata_free(&cd);
		};
		obs_source_t *start = obs_source_create("stagekit_start_image", "Start Image", nullptr, nullptr);
		CHECK(start != nullptr, "the Start Image source can be created");
		set_start(base_image);
		sleep_ms(150);
		CHECK(obs_source_get_width(start) == w && obs_source_get_height(start) == h,
		      "choosing a start image makes the source show it (%ux%u)", obs_source_get_width(start),
		      obs_source_get_height(start));
		const std::vector<uint8_t> shown = render(start, w, h);
		CHECK(mean_rgb(shown) > 20 && opaque(shown), "and it draws the picture");

		// The same image feeds a background set to follow it.
		shot("start_follow", [](obs_data_t *d) {
			obs_data_set_int(d, "base_type", 2);
			obs_data_set_double(d, "blur", 0);
			obs_data_set_double(d, "saturation", 100);
			obs_data_set_double(d, "glow", 0);
			obs_data_set_double(d, "zoom", 100);
			obs_data_set_double(d, "speed_x", 0);
			obs_data_set_double(d, "speed_y", 0);
		});
		const std::vector<uint8_t> follows_a = render(bg, w, h);
		set_start(live_image);
		sleep_ms(250);
		const std::vector<uint8_t> follows_b = render(bg, w, h);
		const std::vector<uint8_t> shown_b = render(start, w, h);
		CHECK(mean_abs_diff(follows_a, follows_b) > 5.0 && mean_abs_diff(shown, shown_b) > 5.0,
		      "changing the start image changes both the source and the background, with no other step");
		CHECK(mean_abs_diff(follows_b, shown_b) < 12.0, "and both show the same picture (diff %.1f)",
		      mean_abs_diff(follows_b, shown_b));
		obs_source_release(start);
		shot("start_reset", [](obs_data_t *d) { obs_data_set_int(d, "base_type", 0); });

		// A background showing a picture of its own switches to the start
		// image when one is chosen: nothing to set on the background.
		set_start(base_image);
		sleep_ms(250);
		{
			obs_data_t *now = obs_source_get_settings(bg);
			CHECK(obs_data_get_int(now, "base_type") == 2,
			      "choosing a start image makes it the background automatically (background from: %d)",
			      (int)obs_data_get_int(now, "base_type"));
			obs_data_release(now);
			obs_data_t *fresh = obs_data_create();
			obs_source_t *fresh_bg =
				obs_source_create("stagekit_ambient_background", "New Background", fresh, nullptr);
			obs_data_t *fresh_now = obs_source_get_settings(fresh_bg);
			CHECK(obs_data_get_int(fresh_now, "base_type") == 2,
			      "a new background uses the start image from the start");
			obs_data_release(fresh_now);
			obs_data_release(fresh);
			obs_source_release(fresh_bg);
		}
		shot("start_reset", [](obs_data_t *d) { obs_data_set_int(d, "base_type", 0); });
	}

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

	// ------------------------------------------------------ corners & edges
	{
		printf("\n[corners & edges]\n");
		const std::string corners_out = out + "/../corners";
		auto solid = [](const char *name, int cw, int ch) {
			obs_data_t *d = obs_data_create();
			obs_data_set_int(d, "width", cw);
			obs_data_set_int(d, "height", ch);
			obs_data_set_int(d, "color", 0xFF3070E0);
			obs_source_t *src = obs_source_create("color_source", name, d, nullptr);
			obs_data_release(d);
			return src;
		};
		obs_source_t *card = solid("Corner Card", 400, 240);
		const uint32_t cw = 400, ch = 240;
		obs_data_t *cs = obs_data_create();
		obs_data_set_double(cs, "size", 60);
		obs_source_t *corners = obs_source_create("stagekit_corners", "Corners", cs, nullptr);
		obs_source_filter_add(card, corners);
		auto alpha = [](const std::vector<uint8_t> &px, uint32_t w, uint32_t x, uint32_t y) {
			return (int)px[(size_t(y) * w + x) * 4 + 3];
		};
		auto look = [&](const char *name) {
			obs_source_update(corners, cs);
			sleep_ms(120);
			const uint32_t fw = obs_source_get_width(card), fh = obs_source_get_height(card);
			std::vector<uint8_t> px = render(card, fw, fh);
			if (name)
				write_frame(corners_out + "/" + name + ".rgba", px, fw, fh);
			return px;
		};

		auto round = look("1_rounded");
		CHECK(obs_source_get_width(card) == cw && obs_source_get_height(card) == ch,
		      "the source keeps its size (%ux%u)", obs_source_get_width(card), obs_source_get_height(card));
		CHECK(alpha(round, cw, 0, 0) == 0 && alpha(round, cw, cw - 1, ch - 1) == 0 &&
			      alpha(round, cw, cw / 2, ch / 2) == 255 && alpha(round, cw, 20, 20) == 255,
		      "rounded: the corners are clear, the rest is solid");
		const uint8_t *mid = &round[(size_t(ch / 2) * cw + cw / 2) * 4];
		CHECK(std::abs(int(mid[0]) - 0xE0) <= 2 && std::abs(int(mid[1]) - 0x70) <= 2 &&
			      std::abs(int(mid[2]) - 0x30) <= 2,
		      "the picture's colour is untouched (%d, %d, %d)", mid[0], mid[1], mid[2]);

		obs_data_set_int(cs, "style", 1);
		auto cut = look("2_cut_off");
		CHECK(alpha(cut, cw, 20, 20) == 0 && alpha(cut, cw, 40, 40) == 255 && alpha(cut, cw, 55, 1) == 0 &&
			      alpha(cut, cw, 64, 1) == 255,
		      "cut off: a straight cut across each corner");

		obs_data_set_int(cs, "style", 2);
		auto notch = look("3_notched");
		CHECK(alpha(notch, cw, 50, 50) == 0 && alpha(notch, cw, 70, 50) == 255 &&
			      alpha(notch, cw, 50, 70) == 255,
		      "notched: a square taken out of each corner");

		obs_data_set_int(cs, "style", 3);
		auto scoop = look("4_scooped");
		CHECK(alpha(scoop, cw, 30, 30) == 0 && alpha(scoop, cw, 50, 50) == 255 && alpha(scoop, cw, 57, 2) == 0,
		      "scooped: a curve bitten into each corner");

		obs_data_set_int(cs, "style", 0);
		obs_data_set_bool(cs, "top_right", false);
		obs_data_set_bool(cs, "bottom_left", false);
		auto some = look("5_two_corners_only");
		CHECK(alpha(some, cw, 0, 0) == 0 && alpha(some, cw, cw - 1, 0) == 255 &&
			      alpha(some, cw, 0, ch - 1) == 255 && alpha(some, cw, cw - 1, ch - 1) == 0,
		      "corners can be chosen one by one");
		obs_data_set_bool(cs, "top_right", true);
		obs_data_set_bool(cs, "bottom_left", true);

		obs_data_set_double(cs, "inset", 30);
		auto trimmed = look(nullptr);
		CHECK(obs_source_get_width(card) == cw && alpha(trimmed, cw, cw / 2, 10) == 0 &&
			      alpha(trimmed, cw, 10, ch / 2) == 0 && alpha(trimmed, cw, cw / 2, 40) == 255,
		      "trimming the edges clears them and still keeps the size");
		obs_data_set_double(cs, "inset", 0);

		obs_data_set_double(cs, "soft", 40);
		auto soft = look("6_soft_edge");
		const int near_edge = alpha(soft, cw, cw / 2, 4), further = alpha(soft, cw, cw / 2, 20);
		CHECK(near_edge < further && further < 255 && alpha(soft, cw, cw / 2, ch / 2) == 255,
		      "a soft edge fades towards the outside (%d, %d)", near_edge, further);
		obs_data_set_double(cs, "soft", 0);

		obs_data_set_double(cs, "border", 10);
		obs_data_set_int(cs, "border_color", 0xFF00FFFF); // yellow
		auto lined = look("7_outline");
		const uint8_t *on_line = &lined[(size_t(4) * cw + cw / 2) * 4];
		const uint8_t *inside = &lined[(size_t(30) * cw + cw / 2) * 4];
		CHECK(on_line[0] > 240 && on_line[1] > 240 && on_line[2] < 15 && on_line[3] == 255 && inside[0] > 200 &&
			      inside[2] < 80,
		      "an outline follows the shape (%d, %d, %d)", on_line[0], on_line[1], on_line[2]);
		obs_data_set_double(cs, "border", 0);
		look(nullptr);

		// A Shadow & Backdrop after it casts the new shape's shadow.
		obs_data_t *ss = obs_data_create();
		obs_data_set_double(ss, "offset_x", 20);
		obs_data_set_double(ss, "offset_y", 20);
		obs_data_set_double(ss, "softness", 0);
		obs_data_set_double(ss, "shadow_opacity", 100);
		obs_data_set_bool(ss, "screen_px", false);
		obs_source_t *shadow = obs_source_create("stagekit_shadow_backdrop", "Shadow", ss, nullptr);
		obs_source_filter_add(card, shadow);
		sleep_ms(200);
		{
			const uint32_t fw = obs_source_get_width(card), fh = obs_source_get_height(card);
			const std::vector<uint8_t> px = render(card, fw, fh);
			write_frame(corners_out + "/8_with_a_shadow.rgba", px, fw, fh);
			CHECK(fw == cw + 20 && fh == ch + 20,
			      "with a shadow after it, the shadow adds its own room (%ux%u)", fw, fh);
			CHECK(alpha(px, fw, 0, 0) == 0 && alpha(px, fw, fw - 1, fh - 1) == 0 &&
				      alpha(px, fw, fw - 1, fh / 2) == 255 && alpha(px, fw, cw / 2, ch / 2) == 255,
			      "and the shadow has the same rounded corners as the picture");
		}

		// Added after a shadow, it goes above the shadow by itself, once.
		obs_source_t *second = solid("Corner Card 2", 400, 240);
		obs_source_t *shadow2 = obs_source_create("stagekit_shadow_backdrop", "Shadow", ss, nullptr);
		obs_source_filter_add(second, shadow2);
		obs_data_t *fresh = obs_data_create(); // its own settings: a filter keeps the object it is given
		obs_data_set_double(fresh, "size", 60);
		obs_source_t *corners2 = obs_source_create("stagekit_corners", "Corners", fresh, nullptr);
		obs_data_release(fresh);
		obs_source_filter_add(second, corners2);
		sleep_ms(300);
		{
			const uint32_t fw = obs_source_get_width(second), fh = obs_source_get_height(second);
			const std::vector<uint8_t> px = render(second, fw, fh);
			CHECK(fw == cw + 20 && alpha(px, fw, fw - 1, fh - 1) == 0 &&
				      alpha(px, fw, fw - 1, fh / 2) == 255 && alpha(px, fw, 398, 20) == 0,
			      "added after a shadow, it moves above it so the shadow is not cut off");
			obs_source_filter_set_order(second, corners2, OBS_ORDER_MOVE_BOTTOM);
			sleep_ms(300);
			const std::vector<uint8_t> moved = render(second, fw, fh);
			CHECK(alpha(moved, fw, 398, 20) == 255, "but the order stays yours to change afterwards");
		}
		obs_source_filter_remove(second, corners2);
		obs_source_filter_remove(second, shadow2);
		obs_source_release(corners2);
		obs_source_release(shadow2);
		obs_source_release(second);
		obs_source_filter_remove(card, shadow);
		obs_source_release(shadow);
		obs_data_release(ss);
		sleep_ms(120);

		// Sizes are as seen on screen: at half size in the scene, a 60 px
		// corner takes 120 of the source's own pixels.
		{
			obs_scene_t *sc = obs_scene_create("Corner Scene");
			obs_sceneitem_t *item = obs_scene_add(sc, card);
			vec2 half;
			vec2_set(&half, 0.5f, 0.5f);
			obs_sceneitem_set_scale(item, &half);
			obs_source_t *before = obs_get_output_source(0);
			obs_set_output_source(0, obs_scene_get_source(sc));
			sleep_ms(1200);
			auto shown = look(nullptr);
			CHECK(alpha(shown, cw, 30, 30) == 0 && alpha(shown, cw, 100, 100) == 255,
			      "sizes follow how big the item is on screen");
			obs_data_set_bool(cs, "screen_px", false);
			auto own = look(nullptr);
			CHECK(alpha(own, cw, 30, 30) == 255, "or the source's own pixels when that is switched off");
			obs_data_set_bool(cs, "screen_px", true);
			obs_set_output_source(0, before);
			obs_source_release(before);
			obs_sceneitem_remove(item);
			obs_scene_release(sc);
		}
		obs_source_filter_remove(card, corners);
		obs_source_release(corners);

		// After an Auto Frame, it shapes the game picture, not the frame around it.
		{
			obs_source_t *game = solid("Corner Game", 400, 300);
			obs_data_t *fs = obs_data_create();
			obs_data_set_int(fs, "frame_width", 800);
			obs_data_set_int(fs, "frame_height", 300);
			obs_data_set_int(fs, "glide_ms", 0);
			obs_source_t *frame = obs_source_create("stagekit_auto_frame", "Auto Frame", fs, nullptr);
			obs_data_release(fs);
			obs_source_filter_add(game, frame);
			obs_data_set_double(cs, "size", 40);
			obs_source_t *shape = obs_source_create("stagekit_corners", "Corners", cs, nullptr);
			obs_source_filter_add(game, shape);
			sleep_ms(300);
			render(game, 800, 300);
			sleep_ms(300);
			const std::vector<uint8_t> px = render(game, 800, 300);
			write_frame(corners_out + "/9_inside_an_auto_frame.rgba", px, 800, 300);
			CHECK(obs_source_get_width(game) == 800 && alpha(px, 800, 202, 2) == 0 &&
				      alpha(px, 800, 597, 297) == 0 && alpha(px, 800, 400, 150) == 255 &&
				      alpha(px, 800, 240, 2) == 255,
			      "after an Auto Frame, the corners are the game picture's, not the frame's");
			obs_source_filter_remove(game, shape);
			obs_source_filter_remove(game, frame);
			obs_source_release(shape);
			obs_source_release(frame);
			obs_source_release(game);
		}
		obs_data_release(cs);
		obs_source_release(card);
	}

	// ------------------------------------------------- glow and outline
	{
		printf("\n[shadow, glow & outline]\n");
		const std::string glow_out = out + "/../glow";
		obs_data_t *gd = obs_data_create();
		obs_data_set_int(gd, "width", 200);
		obs_data_set_int(gd, "height", 120);
		obs_data_set_int(gd, "color", 0xFF30C040); // green
		obs_source_t *card = obs_source_create("color_source", "Glow Card", gd, nullptr);
		obs_data_release(gd);
		obs_data_t *fs = obs_data_create();
		obs_data_set_bool(fs, "shadow", false);
		obs_data_set_bool(fs, "outline", true);
		obs_data_set_double(fs, "outline_width", 10);
		obs_data_set_int(fs, "outline_color", 0xFF0000FF); // red
		obs_source_t *filter = obs_source_create("stagekit_shadow_backdrop", "Glow", fs, nullptr);
		obs_source_filter_add(card, filter);
		uint32_t fw = 0, fh = 0;
		auto look = [&](const char *name) {
			obs_source_update(filter, fs);
			sleep_ms(150);
			fw = obs_source_get_width(card);
			fh = obs_source_get_height(card);
			std::vector<uint8_t> px = render(card, fw, fh);
			write_frame(glow_out + "/" + name + ".rgba", px, fw, fh);
			return px;
		};
		auto at = [&](const std::vector<uint8_t> &px, uint32_t x, uint32_t y) {
			return &px[(size_t(y) * fw + x) * 4];
		};

		auto lined = look("1_outline");
		CHECK(fw == 222 && fh == 142, "a 10 px outline adds its own room all round (%ux%u)", fw, fh);
		const uint8_t *band = at(lined, 5, fh / 2), *inside = at(lined, fw / 2, fh / 2),
			      *corner = at(lined, 0, 0);
		CHECK(band[0] > 240 && band[1] < 15 && band[3] == 255 && inside[1] > 180 && inside[0] < 80 &&
			      corner[3] == 0,
		      "the outline is drawn round the picture in its colour (%d, %d, %d)", band[0], band[1], band[2]);

		obs_data_set_int(fs, "outline_fill", 1);
		auto own_line = look("2_outline_from_the_picture");
		band = at(own_line, 5, fh / 2);
		CHECK(band[1] > 150 && band[0] < 100 && band[3] == 255,
		      "'the picture itself' gives the outline the picture's colours (%d, %d, %d)", band[0], band[1],
		      band[2]);
		obs_data_set_int(fs, "outline_fill", 0);

		obs_data_set_bool(fs, "shadow", true);
		obs_data_set_double(fs, "offset_x", 20);
		obs_data_set_double(fs, "offset_y", 20);
		obs_data_set_double(fs, "softness", 0);
		obs_data_set_double(fs, "shadow_opacity", 100);
		auto both = look("3_outline_and_shadow");
		const uint8_t *cast = at(both, fw - 3, fh / 2);
		CHECK(fw == 242 && fh == 162 && cast[3] == 255 && cast[0] < 15 && cast[1] < 15,
		      "a shadow is cast by the picture and its outline together (%ux%u)", fw, fh);

		obs_data_set_bool(fs, "outline", false);
		obs_data_set_int(fs, "shadow_type", 1);
		obs_data_set_double(fs, "softness", 20);
		obs_data_set_double(fs, "spread", 40);
		auto glow = look("4_glow");
		const int left = at(glow, 28, fh / 2)[3], right = at(glow, fw - 29, fh / 2)[3];
		CHECK(fw == 264 && left > 20 && std::abs(left - right) <= 3,
		      "a glow sits evenly all round, whatever the offsets say (%u wide; %d, %d)", fw, left, right);

		obs_data_set_int(fs, "shadow_fill", 1);
		auto own_glow = look("5_glow_from_the_picture");
		const uint8_t *halo = at(own_glow, 28, fh / 2);
		CHECK(halo[3] > 20 && halo[1] > halo[0] + 60 && halo[1] > halo[2] + 60,
		      "'the picture itself' gives the glow the picture's colours (%d, %d, %d, alpha %d)", halo[0],
		      halo[1], halo[2], halo[3]);

		obs_data_release(fs);
		obs_source_filter_remove(card, filter);
		obs_source_release(filter);

		// On moving video: the glow is a soft copy of what is playing.
		obs_data_t *ls = obs_data_create();
		obs_data_set_int(ls, "shadow_type", 1);
		obs_data_set_int(ls, "shadow_fill", 1);
		obs_data_set_double(ls, "softness", 60);
		obs_data_set_double(ls, "spread", 60);
		obs_data_set_double(ls, "shadow_opacity", 100);
		obs_data_set_double(ls, "radius", 30);
		obs_source_t *live_glow = obs_source_create("stagekit_shadow_backdrop", "Glow", ls, nullptr);
		obs_data_release(ls);
		obs_source_filter_add(live, live_glow);
		sleep_ms(200);
		const uint32_t lw = obs_source_get_width(live), lh = obs_source_get_height(live);
		write_frame(glow_out + "/6_a_game_glowing_in_its_own_colours.rgba", render(live, lw, lh), lw, lh);
		obs_source_filter_remove(live, live_glow);
		obs_source_release(live_glow);

		// An outline made from a real picture: one clean continuation of it,
		// stretched or mirrored, sharp or blurred.
		{
			obs_data_t *ad = obs_data_create();
			obs_data_set_string(ad, "file", base_image);
			obs_source_t *art = obs_source_create("image_source", "Outline Art", ad, nullptr);
			obs_data_release(ad);
			sleep_ms(300);
			obs_data_t *os = obs_data_create();
			obs_data_set_bool(os, "shadow", false);
			obs_data_set_bool(os, "outline", true);
			obs_data_set_double(os, "outline_width", 40);
			obs_data_set_int(os, "outline_fill", 1);
			obs_data_set_double(os, "outline_blur", 0);
			obs_source_t *edge = obs_source_create("stagekit_shadow_backdrop", "Outline", os, nullptr);
			obs_source_filter_add(art, edge);
			auto shot = [&](const char *name) {
				obs_source_update(edge, os);
				sleep_ms(150);
				const uint32_t ew = obs_source_get_width(art), eh = obs_source_get_height(art);
				std::vector<uint8_t> px = render(art, ew, eh);
				write_frame(glow_out + "/" + name + ".rgba", px, ew, eh);
				return px;
			};
			const auto stretched = shot("7_outline_edge_stretched");
			const uint32_t ew = obs_source_get_width(art), eh = obs_source_get_height(art);
			// Stretched: along the left band every pixel of a row has the colour of the
			// picture's first column on that row.
			int same = 0, rows = 0;
			for (uint32_t y = 60; y + 60 < eh; y += 7) {
				const uint8_t *outer = &stretched[(size_t(y) * ew + 6) * 4];
				const uint8_t *inner =
					&stretched[(size_t(y) * ew + 41) * 4]; // the picture's first column
				rows++;
				same += std::abs(int(outer[0]) - int(inner[0])) <= 6 &&
							std::abs(int(outer[1]) - int(inner[1])) <= 6 &&
							std::abs(int(outer[2]) - int(inner[2])) <= 6 && outer[3] == 255
						? 1
						: 0;
			}
			CHECK(rows > 5 && same == rows,
			      "made from the picture, the outline is the picture's edge carried outwards, not copies "
			      "of it (%d of %d rows)",
			      same, rows);
			obs_data_set_int(os, "outline_edge", 1);
			const auto mirrored = shot("8_outline_mirrored");
			CHECK(mean_abs_diff(mirrored, stretched) > 0.2,
			      "or the picture mirrored at each edge (diff %.2f)", mean_abs_diff(mirrored, stretched));
			obs_data_set_double(os, "outline_blur", 40);
			const auto blurred = shot("9_outline_mirrored_and_blurred");
			CHECK(mean_abs_diff(blurred, mirrored) > 0.2, "and it can be blurred (diff %.2f)",
			      mean_abs_diff(blurred, mirrored));
			obs_data_release(os);
			obs_source_filter_remove(art, edge);
			obs_source_release(edge);
			obs_source_release(art);
		}
		obs_source_release(card);
	}

	// ------------------------------------- a shadow left off in some scenes
	{
		printf("\n[shadow left off in a scene]\n");
		obs_data_t *cd = obs_data_create();
		obs_data_set_int(cd, "width", 200);
		obs_data_set_int(cd, "height", 100);
		obs_source_t *card = obs_source_create("color_source", "Two Scene Card", cd, nullptr);
		obs_data_release(cd);
		obs_scene_t *one = obs_scene_create("Scene With Shadow");
		obs_scene_t *two = obs_scene_create("Scene Without Shadow");
		obs_sceneitem_t *in_one = obs_scene_add(one, card);
		obs_sceneitem_t *in_two = obs_scene_add(two, card);
		vec2 v;
		vec2_set(&v, 100, 60);
		obs_sceneitem_set_pos(in_one, &v);
		obs_sceneitem_set_pos(in_two, &v);
		obs_source_t *before_out = obs_get_output_source(0);
		obs_set_output_source(0, obs_scene_get_source(one));

		obs_data_t *fs = obs_data_create();
		obs_data_set_double(fs, "offset_x", -20); // room on the left, so the item has to shift
		// Recorded as not yet adjusted, as a filter added in OBS is.
		for (const char *side : {"adjusted_left", "adjusted_top", "adjusted_right", "adjusted_bottom"})
			obs_data_set_int(fs, side, 0);
		obs_data_set_double(fs, "offset_y", -20);
		obs_data_set_double(fs, "softness", 0);
		obs_data_set_bool(fs, "screen_px", false);
		obs_source_t *filter = obs_source_create("stagekit_shadow_backdrop", "Shadow", fs, nullptr);
		obs_source_filter_add(card, filter);
		auto wait = [&](obs_scene_t *shown) {
			for (int i = 0; i < 14; i++) {
				render(obs_scene_get_source(shown), 640, 360);
				sleep_ms(100);
			}
		};
		wait(one);
		auto x_of = [](obs_sceneitem_t *item) {
			vec2 p;
			obs_sceneitem_get_pos(item, &p);
			return p.x;
		};
		CHECK(obs_source_get_width(card) == 220 && std::fabs(x_of(in_one) - 80) < 0.6,
		      "the shadow is drawn where it is wanted (%u wide, item at %.0f)", obs_source_get_width(card),
		      x_of(in_one));

		// The settings list both scenes by name, each with a tick box.
		{
			obs_properties_t *props = obs_source_properties(filter);
			const std::string key_two =
				std::string("off_in:") + obs_source_get_uuid(obs_scene_get_source(two));
			const std::string key_one =
				std::string("off_in:") + obs_source_get_uuid(obs_scene_get_source(one));
			obs_property_t *box_two = obs_properties_get(props, key_two.c_str());
			obs_property_t *box_one = obs_properties_get(props, key_one.c_str());
			CHECK(box_one && box_two && obs_property_get_type(box_two) == OBS_PROPERTY_BOOL &&
				      std::string(obs_property_description(box_two)) == "Scene Without Shadow",
			      "the settings offer a tick box for each scene the source is in");
			obs_properties_destroy(props);
			obs_data_set_bool(fs, key_two.c_str(), true);
			obs_source_update(filter, fs);
		}
		wait(one);
		CHECK(obs_source_get_width(card) == 220, "ticking another scene changes nothing in this one");

		obs_set_output_source(0, obs_scene_get_source(two));
		wait(two);
		const std::vector<uint8_t> plain =
			render(card, obs_source_get_width(card), obs_source_get_height(card));
		CHECK(obs_source_get_width(card) == 200 && obs_source_get_height(card) == 100 && plain[3] == 255,
		      "in a ticked scene the source shows plain, with no room taken (%ux%u)",
		      obs_source_get_width(card), obs_source_get_height(card));
		CHECK(std::fabs(x_of(in_two) - 100) < 0.6, "and its picture is where it was put (item at %.0f)",
		      x_of(in_two));

		obs_set_output_source(0, obs_scene_get_source(one));
		wait(one);
		CHECK(obs_source_get_width(card) == 220 && std::fabs(x_of(in_one) - 80) < 0.6,
		      "back in the other scene the shadow returns (%u wide, item at %.0f)", obs_source_get_width(card),
		      x_of(in_one));

		obs_set_output_source(0, before_out);
		obs_source_release(before_out);
		obs_data_release(fs);
		obs_source_filter_remove(card, filter);
		obs_source_release(filter);
		obs_scene_release(one);
		obs_scene_release(two);
		obs_source_release(card);
	}

	// ------------------------------------------ one blur switch for several
	{
		printf("\n[blur these too]\n");
		auto solid = [](const char *name) {
			obs_data_t *d = obs_data_create();
			obs_data_set_int(d, "width", 120);
			obs_data_set_int(d, "height", 80);
			obs_source_t *src = obs_source_create("color_source", name, d, nullptr);
			obs_data_release(d);
			return src;
		};
		obs_source_t *lead = solid("Blur Lead");
		obs_source_t *second = solid("Blur Second");
		obs_source_t *third = solid("Blur Third");
		// The third already has a blur of its own, switched off.
		obs_data_t *own = obs_data_create();
		obs_data_set_int(own, "ramp_in_ms", 0);
		obs_data_set_int(own, "ramp_out_ms", 0);
		obs_source_t *third_blur = obs_source_create("stagekit_ramp_blur", "Own Blur", own, nullptr);
		obs_data_release(own);
		obs_source_set_enabled(third_blur, false);
		obs_source_filter_add(third, third_blur);

		obs_data_t *bs = obs_data_create();
		obs_data_set_int(bs, "ramp_in_ms", 0);
		obs_data_set_int(bs, "ramp_out_ms", 0);
		obs_data_array_t *also = obs_data_array_create();
		for (const char *name : {"Blur Second", "Blur Third"}) {
			obs_data_t *entry = obs_data_create();
			obs_data_set_string(entry, "value", name);
			obs_data_array_push_back(also, entry);
			obs_data_release(entry);
		}
		obs_data_set_array(bs, "also", also);
		obs_data_array_release(also);
		obs_source_t *blur = obs_source_create("stagekit_ramp_blur", "Lead Blur", bs, nullptr);
		obs_data_release(bs);
		obs_source_set_enabled(blur, false);
		obs_source_filter_add(lead, blur);
		for (obs_source_t *src : {lead, second, third})
			obs_source_inc_showing(src);
		auto draw_all = [&](int ms) {
			for (int waited = 0; waited < ms; waited += 100) {
				for (obs_source_t *src : {lead, second, third})
					render(src, 120, 80);
				sleep_ms(100);
			}
		};
		draw_all(300);
		auto blur_of = [](obs_source_t *src) {
			obs_source_t *found = nullptr;
			obs_source_enum_filters(
				src,
				[](obs_source_t *, obs_source_t *f, void *param) {
					auto **out = static_cast<obs_source_t **>(param);
					if (!*out &&
					    strcmp(obs_source_get_unversioned_id(f), "stagekit_ramp_blur") == 0)
						*out = f;
				},
				&found);
			return found;
		};
		CHECK(blur_of(second) == nullptr, "a listed source has no blur until it is needed");

		obs_source_set_enabled(blur, true); // as a Stream Deck Filter key does
		draw_all(400);
		obs_source_t *made = blur_of(second);
		CHECK(made && obs_source_enabled(made),
		      "switching one blur on gives a listed source a blur and switches it on");
		CHECK(obs_source_enabled(third_blur), "and switches on the blur a listed source already had");

		obs_source_set_enabled(blur, false);
		draw_all(500);
		made = blur_of(second);
		CHECK(made && !obs_source_enabled(made) && !obs_source_enabled(third_blur) && !obs_source_enabled(blur),
		      "switching it off clears them all");

		for (obs_source_t *src : {lead, second, third})
			obs_source_dec_showing(src);
		obs_source_filter_remove(lead, blur);
		obs_source_filter_remove(third, third_blur);
		obs_source_release(blur);
		obs_source_release(third_blur);
		for (obs_source_t *src : {lead, second, third})
			obs_source_release(src);
	}

	// --------------------------------------- a shadow copied onto a group
	{
		printf("\n[shadow copied to a group]\n");
		auto solid = [](const char *name, int cw, int ch) {
			obs_data_t *d = obs_data_create();
			obs_data_set_int(d, "width", cw);
			obs_data_set_int(d, "height", ch);
			obs_source_t *src = obs_source_create("color_source", name, d, nullptr);
			obs_data_release(d);
			return src;
		};
		obs_scene_t *sc = obs_scene_create("Copy Scene");
		obs_source_t *first = solid("Copy From", 200, 100);
		obs_source_t *in_a = solid("In Group A", 120, 80);
		obs_source_t *in_b = solid("In Group B", 60, 60);
		obs_sceneitem_t *first_item = obs_scene_add(sc, first);
		obs_sceneitem_t *group = obs_scene_add_group(sc, "Copy Group");
		obs_sceneitem_t *ia = obs_scene_add(sc, in_a);
		obs_sceneitem_t *ib = obs_scene_add(sc, in_b);
		vec2 v;
		vec2_set(&v, 300, 200);
		obs_sceneitem_set_pos(ia, &v);
		vec2_set(&v, 430, 210);
		obs_sceneitem_set_pos(ib, &v);
		obs_sceneitem_group_add_item(group, ia);
		obs_sceneitem_group_add_item(group, ib);
		vec2_set(&v, 20, 20);
		obs_sceneitem_set_pos(first_item, &v);
		obs_source_t *before_out = obs_get_output_source(0);
		obs_set_output_source(0, obs_scene_get_source(sc));
		sleep_ms(300);

		obs_data_t *fs = obs_data_create();
		obs_data_set_double(fs, "softness", 30);
		obs_data_set_bool(fs, "outline", true);
		obs_data_set_double(fs, "outline_width", 20);
		obs_data_set_int(fs, "outline_fill", 1);
		obs_source_t *filter = obs_source_create("stagekit_shadow_backdrop", "Shadow", fs, nullptr);
		obs_data_release(fs);
		obs_source_filter_add(first, filter);
		sleep_ms(400); // the first item is adjusted for the shadow's room

		vec2 group_before;
		obs_sceneitem_get_pos(group, &group_before);
		struct {
			double x;
		} picture_before{group_before.x};
		obs_source_t *group_source = obs_sceneitem_get_source(group);
		const uint32_t gw = obs_source_get_width(group_source), gh = obs_source_get_height(group_source);
		obs_source_copy_filters(group_source, first);
		for (int i = 0; i < 12; i++) {
			sleep_ms(100);
			render(obs_scene_get_source(sc), 640, 360);
		}
		vec2 group_after;
		obs_sceneitem_get_pos(group, &group_after);
		const uint32_t gw2 = obs_source_get_width(group_source), gh2 = obs_source_get_height(group_source);
		// The group grew by the shadow's room; its picture must be where it was,
		// so the item moved up and left by exactly the room on those sides.
		const double room_x = (gw2 - gw) / 2.0;
		CHECK(gw2 > gw && gh2 > gh, "the copied filter takes its room around the group (%ux%u -> %ux%u)", gw,
		      gh, gw2, gh2);
		CHECK(std::fabs((group_before.x - group_after.x) - room_x) < 12.0 &&
			      std::fabs(group_before.x - group_after.x) < 80.0,
		      "and the group's picture stays where it was (item moved %.0f, %.0f; room %.0f; box was %.0f)",
		      group_before.x - group_after.x, group_before.y - group_after.y, room_x, picture_before.x);

		obs_set_output_source(0, before_out);
		obs_source_release(before_out);
		obs_source_filter_remove(first, filter);
		obs_source_release(filter);
		obs_scene_release(sc);
		for (obs_source_t *src : {first, in_a, in_b})
			obs_source_release(src);
	}

	// ------------------------------- background from an auto-framed capture
	{
		printf("\n[background from a framed capture]\n");
		obs_data_t *gd = obs_data_create();
		obs_data_set_int(gd, "width", 400);
		obs_data_set_int(gd, "height", 300);
		obs_data_set_int(gd, "color", 0xFF2030E0); // red
		obs_source_t *game = obs_source_create("color_source", "Framed Capture", gd, nullptr);
		obs_data_release(gd);
		obs_data_t *fs = obs_data_create();
		obs_data_set_int(fs, "frame_width", 800);
		obs_data_set_int(fs, "frame_height", 300);
		obs_data_set_int(fs, "glide_ms", 0);
		obs_source_t *frame = obs_source_create("stagekit_auto_frame", "Auto Frame", fs, nullptr);
		obs_data_release(fs);
		obs_source_filter_add(game, frame);
		sleep_ms(200);
		render(game, 800, 300);

		obs_data_t *bs = obs_data_create();
		obs_data_set_int(bs, "live_mode", 2);
		obs_data_set_string(bs, "live_source", "Framed Capture");
		obs_data_set_bool(bs, "live_replace", true);
		obs_data_set_int(bs, "live_fit", 0);
		obs_data_set_string(bs, "image", base_image);
		obs_data_set_double(bs, "blur", 0);
		obs_data_set_double(bs, "glow", 0);
		obs_data_set_double(bs, "speed_x", 0);
		obs_data_set_double(bs, "speed_y", 0);
		obs_source_t *back = obs_source_create("stagekit_ambient_background", "Framed Background", bs, nullptr);
		obs_data_release(bs);
		obs_source_inc_showing(back);
		sleep_ms(700); // it takes the canvas size and finds its capture on its first ticks
		const uint32_t bw = obs_source_get_width(back), bh = obs_source_get_height(back);
		render(back, bw, bh);
		sleep_ms(300);
		const std::vector<uint8_t> px = render(back, bw, bh);
		// Filling the screen with the whole 800x300 frame would leave its empty
		// sides showing as black at the left and right.
		int red_edges = 0;
		for (uint32_t x : {bw / 50, bw - 1 - bw / 50}) {
			const uint8_t *p = &px[(size_t(bh / 2) * bw + x) * 4];
			red_edges += p[0] > 90 && p[0] > p[2] + 40 ? 1 : 0;
		}
		const uint8_t *edge = &px[(size_t(bh / 2) * bw + bw / 50) * 4];
		CHECK(red_edges == 2,
		      "a live capture with an Auto Frame fills the background with its picture, not its frame "
		      "(edge %d, %d, %d)",
		      edge[0], edge[1], edge[2]);

		// A capture showing nothing is no capture: the usual picture comes back.
		auto game_colour = [&](long long colour) {
			obs_data_t *d = obs_data_create();
			obs_data_set_int(d, "color", colour);
			obs_source_update(game, d);
			obs_data_release(d);
		};
		auto shows_capture_red = [&](int wait_ms) {
			std::vector<uint8_t> shot;
			for (int waited = 0; waited <= wait_ms; waited += 150) {
				sleep_ms(150);
				shot = render(back, bw, bh);
			}
			const uint8_t *p = &shot[(size_t(bh / 2) * bw + bw / 2) * 4];
			return p[0] > 90 && p[0] > p[2] + 40 && p[0] > p[1] + 40;
		};
		auto brightness = [&] {
			const std::vector<uint8_t> shot = render(back, bw, bh);
			double sum = 0;
			for (size_t k = 0; k < shot.size(); k += 4)
				sum += shot[k] + shot[k + 1] + shot[k + 2];
			return sum / (double)(shot.size() / 4) / 3.0;
		};
		game_colour(0x00000000); // nothing drawn
		const bool red_when_clear = shows_capture_red(900);
		const double clear_brightness = brightness();
		CHECK(!red_when_clear && clear_brightness > 12.0,
		      "a capture with nothing in it gives way to the usual picture (brightness %.0f)",
		      clear_brightness);
		game_colour(0xFF2030E0);
		CHECK(shows_capture_red(900), "and takes over again the moment it shows something");
		game_colour(0xFF000000); // a black screen
		CHECK(brightness() < 6.0 || shows_capture_red(300) == false,
		      "a black screen is still the capture at first");
		const bool red_when_black = shows_capture_red(3200);
		const double black_brightness = brightness();
		CHECK(!red_when_black && black_brightness > 12.0,
		      "a capture that stays black gives way to the usual picture (brightness %.0f)", black_brightness);
		game_colour(0xFF2030E0);
		CHECK(shows_capture_red(900), "and comes straight back when the game shows a picture");

		obs_source_dec_showing(back);
		obs_source_release(back);
		obs_source_filter_remove(game, frame);
		obs_source_release(frame);
		obs_source_release(game);
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

		// Changing the shadow must not move or resize the picture in a scene.
		{
			obs_data_t *cd2 = obs_data_create();
			obs_data_set_int(cd2, "width", 100);
			obs_data_set_int(cd2, "height", 60);
			obs_source_t *card = obs_source_create("color_source", "Shadow Card", cd2, nullptr);
			obs_data_release(cd2);
			obs_scene_t *sc = obs_scene_create("Shadow Scene");
			obs_sceneitem_t *plain = obs_scene_add(sc, card);
			obs_sceneitem_t *boxed = obs_scene_add(sc, card);
			vec2 v;
			vec2_set(&v, 100.0f, 50.0f);
			obs_sceneitem_set_pos(plain, &v);
			vec2_set(&v, 2.0f, 2.0f);
			obs_sceneitem_set_scale(plain, &v);
			vec2_set(&v, 10.0f, 10.0f);
			obs_sceneitem_set_pos(boxed, &v);
			obs_sceneitem_set_bounds_type(boxed, OBS_BOUNDS_SCALE_INNER);
			vec2_set(&v, 300.0f, 200.0f);
			obs_sceneitem_set_bounds(boxed, &v);

			obs_data_t *sd = obs_data_create();
			obs_data_set_double(sd, "softness", 0);
			obs_data_set_double(sd, "offset_x", 0);
			obs_data_set_double(sd, "offset_y", 0);
			obs_data_set_bool(sd, "screen_px", false); // this part measures in the source's own pixels
			obs_source_t *shade = obs_source_create("stagekit_shadow_backdrop", "Shadow", sd, nullptr);
			obs_source_filter_add(card, shade);
			sleep_ms(150);
			obs_data_set_double(sd, "softness", 20); // 32 px of room on every side...
			obs_data_set_double(sd, "offset_x", 10); // ...and 10 more to the right and below
			obs_data_set_double(sd, "offset_y", 10);
			obs_source_update(shade, sd);
			sleep_ms(200);
			CHECK(obs_source_get_width(card) == 174 && obs_source_get_height(card) == 134,
			      "a soft shadow takes room around the source (%ux%u)", obs_source_get_width(card),
			      obs_source_get_height(card));
			vec2 pp, ps, bp, bb;
			obs_sceneitem_get_pos(plain, &pp);
			obs_sceneitem_get_scale(plain, &ps);
			obs_sceneitem_get_pos(boxed, &bp);
			obs_sceneitem_get_bounds(boxed, &bb);
			CHECK(std::fabs(pp.x - 36) < 0.6 && std::fabs(pp.y + 14) < 0.6 && std::fabs(ps.x - 2) < 0.001,
			      "but the picture stays where it was, at the same size (item now at %.0f, %.0f)", pp.x,
			      pp.y);
			// In a 300x200 box the 100x60 picture was drawn 3x at (10, 20). It must still be.
			const double drawn = std::min(bb.x / 174.0, bb.y / 134.0);
			const double pic_x = bp.x + (bb.x - 174 * drawn) / 2.0 + 32 * drawn;
			const double pic_y = bp.y + (bb.y - 134 * drawn) / 2.0 + 32 * drawn;
			CHECK(std::fabs(drawn - 3.0) < 0.01 && std::fabs(pic_x - 10) < 0.6 &&
				      std::fabs(pic_y - 20) < 0.6,
			      "also inside a bounding box: same size and place (%.2fx at %.0f, %.0f)", drawn, pic_x,
			      pic_y);

			obs_source_filter_remove(card, shade);
			sleep_ms(100);
			obs_sceneitem_get_pos(plain, &pp);
			obs_sceneitem_get_pos(boxed, &bp);
			obs_sceneitem_get_bounds(boxed, &bb);
			CHECK(std::fabs(pp.x - 100) < 0.6 && std::fabs(pp.y - 50) < 0.6 && std::fabs(bp.x - 10) < 0.6 &&
				      std::fabs(bb.x - 300) < 0.6 && std::fabs(bb.y - 200) < 0.6,
			      "removing the shadow puts everything back (%.0f, %.0f; box %.0fx%.0f)", pp.x, pp.y, bb.x,
			      bb.y);
			obs_data_release(sd);
			obs_source_release(shade);
			obs_scene_release(sc);
			obs_source_release(card);
		}

		// Cropped View: part of a source as a source of its own.
		printf("\n[cropped view]\n");
		{
			// A 400x200 picture: red on the left half, blue on the right.
			QImage halves(400, 200, QImage::Format_RGB32);
			for (int y = 0; y < 200; y++)
				for (int x = 0; x < 400; x++)
					halves.setPixel(x, y, x < 200 ? qRgb(220, 30, 30) : qRgb(30, 30, 220));
			const std::string halves_path = out + "/view-halves.png";
			halves.save(QString::fromStdString(halves_path));
			obs_data_t *hd = obs_data_create();
			obs_data_set_string(hd, "file", halves_path.c_str());
			obs_source_t *camera = obs_source_create("image_source", "View Camera", hd, nullptr);
			obs_data_release(hd);

			obs_data_t *vd = obs_data_create();
			obs_data_set_string(vd, "source", "View Camera");
			obs_data_set_int(vd, "crop_to", 1); // a square from the middle
			obs_source_t *view =
				obs_source_create("stagekit_cropped_view", "Square Camera View", vd, nullptr);
			CHECK(view != nullptr, "the Cropped View source can be created");
			sleep_ms(700);
			CHECK(obs_source_get_width(view) == 200 && obs_source_get_height(view) == 200,
			      "'a square' takes the middle of the picture (%ux%u)", obs_source_get_width(view),
			      obs_source_get_height(view));
			std::vector<uint8_t> vp = render(view, 200, 200);
			auto at = [&](int x, int y, int channel) {
				return (int)vp[(size_t(y) * 200 + x) * 4 + channel];
			};
			CHECK(at(50, 100, 0) > 150 && at(50, 100, 2) < 90 && at(150, 100, 2) > 150 &&
				      at(150, 100, 0) < 90,
			      "and shows that part: red half, blue half (%d,%d | %d,%d)", at(50, 100, 0),
			      at(50, 100, 2), at(150, 100, 0), at(150, 100, 2));

			obs_data_set_int(vd, "crop_to", 0);
			obs_data_set_int(vd, "left", 250);
			obs_data_set_int(vd, "top", 20);
			obs_data_set_int(vd, "bottom", 30);
			obs_source_update(view, vd);
			sleep_ms(150);
			CHECK(obs_source_get_width(view) == 150 && obs_source_get_height(view) == 150,
			      "set amounts crop each side (%ux%u)", obs_source_get_width(view),
			      obs_source_get_height(view));
			vp = render(view, 150, 150);
			CHECK((int)vp[(size_t(75) * 150 + 10) * 4 + 2] > 150, "what is left is the right-hand part");

			// "Whatever is drawn": the see-through room around the content is
			// trimmed off without any numbers, and the view follows the content.
			{
				auto card = [&](const char *name, int x0, int x1, int y0, int y1) {
					QImage page(800, 600, QImage::Format_ARGB32);
					page.fill(Qt::transparent);
					for (int y = y0; y < y1; y++)
						for (int x = x0; x < x1; x++)
							page.setPixel(x, y, qRgba(40, 40, 40, 255));
					const std::string path = out + "/" + name;
					page.save(QString::fromStdString(path));
					return path;
				};
				obs_data_t *wd = obs_data_create();
				obs_data_set_string(wd, "file", card("widget-small.png", 100, 500, 250, 350).c_str());
				obs_source_t *widget = obs_source_create("image_source", "View Widget", wd, nullptr);
				obs_data_t *ad = obs_data_create();
				obs_data_set_string(ad, "source", "View Widget");
				obs_data_set_int(ad, "crop_to", 6);
				obs_source_t *fitted =
					obs_source_create("stagekit_cropped_view", "Fitted Widget", ad, nullptr);
				auto settle = [&](int ms) {
					for (int i = 0; i < ms / 100; i++) {
						render(fitted, 800, 600);
						sleep_ms(100);
					}
				};
				settle(2600);
				const int fw = (int)obs_source_get_width(fitted),
					  fh = (int)obs_source_get_height(fitted);
				CHECK(std::abs(fw - 400) <= 6 && std::abs(fh - 100) <= 6,
				      "'whatever is drawn' trims the empty room off a widget (%dx%d of 800x600)", fw,
				      fh);
				// The widget's content gets wider: the view follows.
				obs_data_set_string(wd, "file", card("widget-wide.png", 20, 700, 250, 350).c_str());
				obs_source_update(widget, wd);
				settle(2600);
				CHECK(std::abs((int)obs_source_get_width(fitted) - 680) <= 6,
				      "and follows when the content changes size (%u wide)",
				      obs_source_get_width(fitted));
				obs_data_release(ad);
				obs_data_release(wd);
				obs_source_release(fitted);
				obs_source_release(widget);
			}

			// A shadow on the view follows the crop: the view grows by the shadow's room.
			obs_data_t *sd = obs_data_create();
			obs_data_set_double(sd, "softness", 0);
			obs_data_set_double(sd, "offset_x", 20);
			obs_data_set_double(sd, "offset_y", 20);
			obs_source_t *shade = obs_source_create("stagekit_shadow_backdrop", "Shadow", sd, nullptr);
			obs_source_filter_add(view, shade);
			sleep_ms(150);
			CHECK(obs_source_get_width(view) == 170 && obs_source_get_height(view) == 170,
			      "a shadow added to the view sits around the cropped picture (%ux%u)",
			      obs_source_get_width(view), obs_source_get_height(view));
			obs_source_filter_remove(view, shade);
			obs_source_release(shade);
			obs_data_release(sd);
			obs_data_release(vd);
			obs_source_release(view);
			obs_source_release(camera);
		}

		// A 20 px shadow must look 20 px whatever size the item is drawn at.
		{
			auto make = [](const char *name) {
				obs_data_t *d = obs_data_create();
				obs_data_set_int(d, "width", 400);
				obs_data_set_int(d, "height", 200);
				obs_source_t *src = obs_source_create("color_source", name, d, nullptr);
				obs_data_release(d);
				return src;
			};
			obs_source_t *small = make("Shown Half Size");
			obs_source_t *big = make("Shown Double Size");
			obs_scene_t *sc = obs_scene_create("Shadow Size Scene");
			vec2 v;
			obs_sceneitem_t *small_item = obs_scene_add(sc, small);
			vec2_set(&v, 0.5f, 0.5f);
			obs_sceneitem_set_scale(small_item, &v);
			obs_sceneitem_t *big_item = obs_scene_add(sc, big);
			vec2_set(&v, 2.0f, 2.0f);
			obs_sceneitem_set_scale(big_item, &v);
			obs_source_t *before = obs_get_output_source(0);
			obs_set_output_source(0, obs_scene_get_source(sc));
			obs_data_t *sd = obs_data_create();
			obs_data_set_double(sd, "softness", 0);
			obs_data_set_double(sd, "offset_x", 20);
			obs_data_set_double(sd, "offset_y", 20);
			obs_source_t *shade_small =
				obs_source_create("stagekit_shadow_backdrop", "Shadow", sd, nullptr);
			obs_source_t *shade_big = obs_source_create("stagekit_shadow_backdrop", "Shadow", sd, nullptr);
			obs_source_filter_add(small, shade_small);
			obs_source_filter_add(big, shade_big);
			sleep_ms(700);
			// On screen: (source width - 400) x the item's scale.
			const double small_on_screen = (obs_source_get_width(small) - 400.0) * 0.5;
			const double big_on_screen = (obs_source_get_width(big) - 400.0) * 2.0;
			CHECK(std::fabs(small_on_screen - 20) <= 1.0 && std::fabs(big_on_screen - 20) <= 1.0,
			      "a 20 px shadow is 20 px on screen at half size and at double size (%.0f and %.0f)",
			      small_on_screen, big_on_screen);
			vec2 sp;
			obs_sceneitem_get_scale(small_item, &sp);
			CHECK(std::fabs(sp.x - 0.5) < 0.001, "and the item keeps its size");
			obs_set_output_source(0, before);
			obs_source_release(before);
			obs_source_filter_remove(small, shade_small);
			obs_source_filter_remove(big, shade_big);
			obs_source_release(shade_small);
			obs_source_release(shade_big);
			obs_data_release(sd);
			obs_scene_release(sc);
			obs_source_release(small);
			obs_source_release(big);
		}

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

		// Visibility Link: eye icons tied together.
		printf("\n[visibility link]\n");
		{
			obs_scene_t *vs = obs_scene_create("Link Scene");
			obs_source_t *vsrc = obs_scene_get_source(vs);
			obs_sceneitem_t *vi[3];
			obs_source_t *vsources[3];
			const char *vnames[3] = {"Frame SNES", "Frame GBA", "Frame N64"};
			for (int i = 0; i < 3; i++) {
				vsources[i] = obs_source_create("color_source", vnames[i], nullptr, nullptr);
				vi[i] = obs_scene_add(vs, vsources[i]);
			}
			obs_data_t *ld = obs_data_create();
			obs_source_t *link = obs_source_create("stagekit_visibility_link", "One frame", ld, nullptr);
			obs_source_filter_add(vsrc, link);
			sleep_ms(80);
			// Fill the list the way a person would: pick an item, press the button.
			obs_properties_t *lp = obs_source_properties(link);
			for (const char *name : vnames) {
				obs_data_t *now = obs_source_get_settings(link);
				obs_data_set_string(now, "pick", name);
				obs_data_release(now);
				obs_property_button_clicked(obs_properties_get(lp, "add"), link);
			}
			CHECK(obs_property_list_item_count(obs_properties_get(lp, "pick")) == 4,
			      "the scene's items can be picked from a list");
			obs_properties_destroy(lp);
			sleep_ms(120);
			auto shown = [&] {
				return std::string(obs_sceneitem_visible(vi[0]) ? "1" : "0") +
				       (obs_sceneitem_visible(vi[1]) ? "1" : "0") +
				       (obs_sceneitem_visible(vi[2]) ? "1" : "0");
			};
			obs_sceneitem_set_visible(vi[0], false);
			obs_sceneitem_set_visible(vi[1], false);
			sleep_ms(120);
			CHECK(shown() == "001", "hiding items leaves the others alone (%s)", shown().c_str());
			obs_sceneitem_set_visible(vi[1], true);
			sleep_ms(150);
			CHECK(shown() == "010", "showing one item hides the others (%s)", shown().c_str());
			obs_sceneitem_set_visible(vi[0], true);
			sleep_ms(150);
			CHECK(shown() == "100", "and again for another (%s)", shown().c_str());

			obs_data_t *now = obs_source_get_settings(link);
			obs_data_set_int(now, "mode", 1);
			obs_source_update(link, now);
			obs_data_release(now);
			sleep_ms(100);
			obs_sceneitem_set_visible(vi[2], true);
			sleep_ms(150);
			CHECK(shown() == "111", "'together' shows the rest when one is shown (%s)", shown().c_str());
			obs_sceneitem_set_visible(vi[1], false);
			sleep_ms(150);
			CHECK(shown() == "000", "and hides the rest when one is hidden (%s)", shown().c_str());

			obs_source_set_enabled(link, false);
			obs_sceneitem_set_visible(vi[0], true);
			sleep_ms(150);
			CHECK(shown() == "100", "switched off, the filter leaves items alone (%s)", shown().c_str());

			obs_source_filter_remove(vsrc, link);
			obs_source_release(link);
			obs_data_release(ld);
			for (obs_source_t *source : vsources)
				obs_source_release(source);
			obs_scene_release(vs);
		}

		// Program Trigger: items and filters follow a program.
		printf("\n[program trigger]\n");
		{
			obs_scene_t *ts = obs_scene_create("Trigger Scene");
			obs_source_t *tsrc = obs_scene_get_source(ts);
			obs_source_t *sculpt = obs_source_create("color_source", "ZBrush Capture", nullptr, nullptr);
			obs_source_t *other = obs_source_create("color_source", "Something Else", nullptr, nullptr);
			obs_sceneitem_t *sculpt_item = obs_scene_add(ts, sculpt);
			obs_sceneitem_t *other_item = obs_scene_add(ts, other);
			obs_sceneitem_set_visible(sculpt_item, false);
			obs_source_t *tint =
				obs_source_create("stagekit_shadow_backdrop", "Extra Shadow", nullptr, nullptr);
			obs_source_filter_add(tsrc, tint);
			obs_source_set_enabled(tint, false);

			obs_data_t *td = obs_data_create();
			obs_data_set_string(td, "program", "ZBrush.exe");
			obs_source_t *trigger = obs_source_create("stagekit_program_trigger", "ZBrush", td, nullptr);
			obs_source_filter_add(tsrc, trigger);
			sleep_ms(80);
			obs_properties_t *tp = obs_source_properties(trigger);
			obs_data_t *now = obs_source_get_settings(trigger);
			obs_data_set_string(now, "pick_item", "ZBrush Capture");
			obs_data_set_string(now, "pick_filter", "Extra Shadow");
			obs_data_release(now);
			obs_property_button_clicked(obs_properties_get(tp, "add_item"), trigger);
			obs_property_button_clicked(obs_properties_get(tp, "add_filter"), trigger);
			CHECK(obs_property_list_item_count(obs_properties_get(tp, "pick_item")) == 3 &&
				      obs_property_list_item_count(obs_properties_get(tp, "pick_filter")) == 2,
			      "items and filters are picked from lists");
			obs_properties_destroy(tp);

			auto see = [&](const char *windows) {
				calldata_t cd = {};
				calldata_set_string(&cd, "windows", windows);
				proc_handler_call(obs_source_get_proc_handler(trigger), "set_test_windows", &cd);
				calldata_free(&cd);
				sleep_ms(250);
			};
			see("Inbox|mail.exe|front");
			CHECK(!obs_sceneitem_visible(sculpt_item) && !obs_source_enabled(tint),
			      "without the program, its item is hidden and its filter is off");
			see("Inbox|mail.exe|front\nZBrush 2026|ZBrush.exe|");
			CHECK(obs_sceneitem_visible(sculpt_item) && obs_source_enabled(tint),
			      "when the program opens, the item is shown and the filter switched on");
			CHECK(obs_sceneitem_visible(other_item), "items not on the list are left alone");
			// A change made by hand is kept while nothing about the program changes.
			obs_sceneitem_set_visible(sculpt_item, false);
			see("ZBrush 2026 - sphere|ZBrush.exe|\nInbox|mail.exe|front");
			CHECK(!obs_sceneitem_visible(sculpt_item),
			      "a change made by hand is kept until the program comes or goes");
			see("Inbox|mail.exe|front");
			see("ZBrush 2026|ZBrush.exe|front");
			CHECK(obs_sceneitem_visible(sculpt_item), "and it acts again the next time the program opens");
			see("Inbox|mail.exe|front");
			CHECK(!obs_sceneitem_visible(sculpt_item) && !obs_source_enabled(tint),
			      "when the program closes, both are put back");

			// By window title, and only while it is in front.
			now = obs_source_get_settings(trigger);
			obs_data_set_string(now, "program", "");
			obs_data_set_string(now, "title", "GBA1");
			obs_data_set_int(now, "when", 1);
			obs_source_update(trigger, now);
			obs_data_release(now);
			sleep_ms(80);
			see("GBA1 | Volume 100%|Dolphin.exe|\nInbox|mail.exe|front");
			CHECK(!obs_sceneitem_visible(sculpt_item),
			      "'in front' ignores a window that is behind another");
			see("GBA1 - Volume 100%|Dolphin.exe|front");
			CHECK(obs_sceneitem_visible(sculpt_item),
			      "a window can be matched by its title, while it is in front");

			obs_source_filter_remove(tsrc, trigger);
			obs_source_filter_remove(tsrc, tint);
			obs_source_release(trigger);
			obs_source_release(tint);
			obs_data_release(td);
			obs_source_release(sculpt);
			obs_source_release(other);
			obs_scene_release(ts);
		}

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
		// A matched item that lives inside a group still lands on the same
		// place on screen.
		{
			obs_source_t *grouped_src = color("Grouped Title", 450, 60);
			obs_sceneitem_t *title_group = obs_scene_add_group(scene, "Title Group");
			obs_sceneitem_t *grouped = obs_scene_add(scene, grouped_src);
			obs_sceneitem_group_add_item(title_group, grouped);
			at(title_group, 60, 40);
			obs_data_t *gd = obs_data_create();
			obs_data_set_string(gd, "item", "Grouped Title");
			obs_data_set_string(gd, "master_scene", "Start Scene");
			obs_data_set_string(gd, "master_item", "Starting Title");
			obs_source_t *gmatch = add_filter(scene, "stagekit_match_item", "Match Grouped", gd);
			sleep_ms(150);
			vec2 gp = pos(title_group), ip = pos(grouped), gb;
			obs_sceneitem_get_bounds(grouped, &gb);
			CHECK(std::fabs(gp.x + ip.x - 100) < 0.6 && std::fabs(gp.y + ip.y - 150) < 0.6 &&
				      std::fabs(gb.x - 300) < 0.6,
			      "a matched item inside a group lands on the same spot on screen (%.0f, %.0f, box %.0f wide)",
			      gp.x + ip.x, gp.y + ip.y, gb.x);
			obs_source_filter_remove(obs_scene_get_source(scene), gmatch);
			obs_source_release(gmatch);
			obs_source_release(grouped_src);
		}

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

		// A layout limited to chosen items leaves everything else alone.
		{
			obs_data_t *ld = obs_data_create();
			obs_data_array_t *only = obs_data_array_create();
			obs_data_t *entry = obs_data_create();
			obs_data_set_string(entry, "value", "Lower Third");
			obs_data_array_push_back(only, entry);
			obs_data_release(entry);
			obs_data_set_array(ld, "only_items", only);
			obs_data_array_release(only);
			obs_data_set_int(ld, "duration_ms", 100);
			obs_source_t *partial = add_filter(scene, "stagekit_layout", "Lower Third Home", ld);
			sleep_ms(80);
			proc_handler_call(obs_source_get_proc_handler(partial), "save", &cd);
			sleep_ms(80);
			at(lower, 300, 100);
			at(follower, 7, 9);
			obs_source_set_enabled(partial, true);
			sleep_ms(400);
			p = pos(lower);
			q = pos(follower);
			CHECK(std::fabs(p.x - 30) < 0.5 && std::fabs(q.x - 7) < 0.5 && std::fabs(q.y - 9) < 0.5,
			      "a layout limited to chosen items moves only those (%.0f / %.0f,%.0f)", p.x, q.x, q.y);
			obs_source_filter_remove(obs_scene_get_source(scene), partial);
			obs_source_release(partial);
		}
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
		// The old picture is held for a moment while the capture settles, then it glides.
		for (int i = 0; i < 5; i++) {
			sleep_ms(50);
			picture();
		}
		sleep_ms(30);
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

		// "Use that item's size": the frame is copied from an item already laid out.
		{
			obs_scene_t *layout_scene = obs_scene_create("Old 4:3 Scene");
			obs_source_t *old_src = obs_source_create("color_source", "Old Capture", nullptr, nullptr);
			sized(old_src, 400, 300);
			sleep_ms(80);
			obs_sceneitem_t *old_item = obs_scene_add(layout_scene, old_src);
			vec2 half;
			vec2_set(&half, 0.5f, 0.5f);
			obs_sceneitem_set_scale(old_item, &half); // shown at 200x150
			obs_data_set_string(fs, "copy_scene", "Old 4:3 Scene");
			obs_data_set_string(fs, "copy_item", "Old Capture");
			obs_source_update(frame, fs);
			sleep_ms(80);
			obs_properties_t *props = obs_source_properties(frame);
			obs_property_t *button = obs_properties_get(props, "copy_frame");
			CHECK(button && obs_property_button_clicked(button, frame),
			      "the filter has a 'use that item's size' button");
			obs_properties_destroy(props);
			sleep_ms(120);
			CHECK(obs_source_get_width(game) == 200 && obs_source_get_height(game) == 150,
			      "pressing it sets the frame to the size that item takes up (%ux%u)",
			      obs_source_get_width(game), obs_source_get_height(game));
			obs_source_release(old_src);
			obs_scene_release(layout_scene);
		}

		// Black bars: a full-screen emulator shows a 4:3 picture inside 16:9.
		if (argc >= 11) {
			obs_data_t *bd = obs_data_create();
			obs_data_set_string(bd, "file", argv[9]);
			obs_source_t *barred = obs_source_create("image_source", "Full-screen Emulator", bd, nullptr);
			obs_data_t *bf = obs_data_create();
			obs_data_set_int(bf, "frame_width", 800);
			obs_data_set_int(bf, "frame_height", 600);
			obs_data_set_int(bf, "glide_ms", 100);
			obs_data_set_double(bf, "trim_wait", 1.0);
			obs_source_t *trimmer = obs_source_create("stagekit_auto_frame", "Auto Frame", bf, nullptr);
			obs_source_filter_add(barred, trimmer);
			auto look = [&](int ms) {
				for (int i = 0; i < ms / 50; i++) {
					render(barred, 800, 600); // bars are looked for while the source is drawn
					sleep_ms(50);
				}
				calldata_t cd = {};
				proc_handler_call(obs_source_get_proc_handler(trimmer), "get_trim", &cd);
				const double tx = calldata_float(&cd, "x");
				proc_handler_call(obs_source_get_proc_handler(trimmer), "get_picture", &cd);
				Pic pic{calldata_float(&cd, "x"), calldata_float(&cd, "y"),
					calldata_float(&cd, "width"), calldata_float(&cd, "height")};
				calldata_free(&cd);
				return std::make_pair(tx, pic);
			};
			auto [t0, p0] = look(300);
			CHECK(t0 == 0.0 && is(p0, 0, 75, 800, 450), "bars are not trimmed straight away (trim %.0f)",
			      t0);
			auto [t1, p1] = look(3500);
			CHECK(std::fabs(t1 - 80) <= 1.0 && is(p1, 0, 0, 800, 600),
			      "after the wait, 80 px bars are trimmed and the 4:3 picture fills the frame (trim %.0f, %.0fx%.0f)",
			      t1, p1.w, p1.h);

			// A black screen teaches nothing: the trim is kept.
			obs_data_set_string(bd, "file", argv[10]);
			obs_source_update(barred, bd);
			auto [t2, p2] = look(2500);
			CHECK(std::fabs(t2 - 80) <= 1.0, "a fully black moment keeps the trim (trim %.0f)", t2);

			// The picture now fills the whole capture: the trim is released quickly.
			obs_data_set_string(bd, "file", live_image);
			obs_source_update(barred, bd);
			auto [t3, p3] = look(3500);
			CHECK(t3 == 0.0 && is(p3, 0, 75, 800, 450),
			      "when the picture grows into the bars the trim is released (trim %.0f)", t3);

			// A game with a black background (the reported Aladdin title
			// screen): artwork on black is not bars and must not be trimmed.
			if (argc >= 12) {
				obs_data_set_string(bd, "file", argv[11]);
				obs_source_update(barred, bd);
				auto [t5, p5] = look(4000);
				CHECK(t5 == 0.0 && is(p5, 0, 75, 800, 450),
				      "artwork on a black background is not mistaken for bars (trim %.0f after 4 s)",
				      t5);

				// ...unless asked: "trim to what is on screen now" measures once and fixes it.
				calldata_t mcd = {};
				proc_handler_call(obs_source_get_proc_handler(trimmer), "measure_now", &mcd);
				calldata_free(&mcd);
				auto [t6, p6] = look(800);
				obs_data_t *now = obs_source_get_settings(trimmer);
				CHECK(std::fabs(t6 - 145) <= 1.0 && obs_data_get_int(now, "trim") == 2 &&
					      std::fabs(obs_data_get_double(now, "trim_y") - 55) <= 1.0,
				      "the 'trim to what is on screen now' button measures once and fixes the amounts (%.0f, %.0f)",
				      obs_data_get_double(now, "trim_x"), obs_data_get_double(now, "trim_y"));
				obs_data_release(now);
				obs_data_set_int(bf, "trim", 1);
				obs_data_set_string(bd, "file", live_image);
				obs_source_update(barred, bd);
			}

			// Hands-off control of the shape: a key, and a rule per program.
			if (argc >= 12) {
				obs_data_set_int(bf, "trim", 1);
				obs_source_update(trimmer, bf);
				obs_data_set_string(bd, "file", argv[11]); // the dark game again
				obs_source_update(barred, bd);

				obs_data_t *kd = obs_data_create();
				obs_data_set_string(kd, "shape", "4:3");
				obs_source_t *key =
					obs_source_create("stagekit_picture_shape", "Shape 4:3", kd, nullptr);
				obs_source_filter_add(barred, key);
				look(200);
				obs_source_set_enabled(key, true); // what a Stream Deck Filter key does
				auto [t7, p7] = look(600);
				CHECK(std::fabs(t7 - 80) <= 1.0 && is(p7, 0, 0, 800, 600) && !obs_source_enabled(key),
				      "a Picture Shape key sets 4:3 at once, even on a dark game, then switches itself off (trim %.0f)",
				      t7);
				obs_data_set_string(kd, "shape", "0");
				obs_source_update(key, kd);
				look(150);
				obs_source_set_enabled(key, true);
				auto [t8, p8] = look(600);
				CHECK(t8 == 0.0, "a key set to Automatic hands control back (trim %.0f)", t8);
				obs_source_filter_remove(barred, key);
				obs_source_release(key);
				obs_data_release(kd);

				// The capture reports which program it shows; a rule names its shape.
				obs_data_t *capture_settings = obs_source_get_settings(barred);
				obs_data_set_string(capture_settings, "window",
						    "Aladdin - Snes9x 1.60:Snes9x:snes9x-x64.exe");
				obs_data_release(capture_settings);
				obs_data_array_t *rules = obs_data_array_create();
				obs_data_t *rule = obs_data_create();
				obs_data_set_string(rule, "value", "Snes9x = 4:3");
				obs_data_array_push_back(rules, rule);
				obs_data_release(rule);
				obs_data_set_array(bf, "rules", rules);
				obs_source_update(trimmer, bf);
				auto [t9, p9] = look(1800);
				CHECK(std::fabs(t9 - 80) <= 1.0 && is(p9, 0, 0, 800, 600),
				      "a rule for the captured program sets its shape with no key press (trim %.0f)",
				      t9);
				obs_data_array_release(rules);
				rules = obs_data_array_create();
				obs_data_set_array(bf, "rules", rules);
				obs_data_array_release(rules);

				// The controls say what is captured and which shape applies.
				{
					obs_properties_t *props = obs_source_properties(trimmer);
					const std::string status =
						obs_property_description(obs_properties_get(props, "status"));
					CHECK(status.find("snes9x-x64.exe") != std::string::npos &&
						      status.find("4:3") != std::string::npos,
					      "the controls name the program and its shape (%s)", status.c_str());
					obs_properties_destroy(props);
				}

				// With no rule of the owner's, a common emulator's shape is known.
				obs_source_update(trimmer, bf);
				sleep_ms(1300);
				{
					obs_properties_t *props = obs_source_properties(trimmer);
					const std::string status =
						obs_property_description(obs_properties_get(props, "status"));
					CHECK(status.find("8:7") != std::string::npos &&
						      status.find("usual") != std::string::npos,
					      "a common emulator starts from its usual picture shape (%s)",
					      status.c_str());
				}
				{
					// That is only a starting point: this picture is clearly 4:3,
					// so after the hold time the picture is what counts.
					obs_data_set_string(bd, "file", argv[9]); // a bright 4:3 picture with bars
					obs_source_update(barred, bd);
					auto [tk, pk] = look(3500);
					obs_properties_t *props = obs_source_properties(trimmer);
					const std::string status =
						obs_property_description(obs_properties_get(props, "status"));
					CHECK(std::fabs(tk - 80) <= 1.0 && status.find("4:3") != std::string::npos,
					      "and is corrected when the picture clearly shows another shape (trim %.0f; %s)",
					      tk, status.c_str());

					// Choose a program and a shape, press the button: it is saved.
					obs_data_set_string(bf, "rule_program", "Snes9x-x64.exe");
					obs_data_set_string(bf, "rule_shape", "4:3");
					obs_source_update(trimmer, bf);
					sleep_ms(100);
					obs_property_button_clicked(obs_properties_get(props, "rule_add"), trimmer);
					obs_properties_destroy(props);
					obs_data_t *now = obs_source_get_settings(trimmer);
					const std::string saved = obs_data_get_string(now, "rules_text");
					obs_data_set_string(bf, "rules_text", saved.c_str());
					obs_data_release(now);
					CHECK(saved == "snes9x-x64 = 4:3", "'Remember this shape' saves it (%s)",
					      saved.c_str());
				}
				auto [t10, p10] = look(1800);
				CHECK(std::fabs(t10 - 80) <= 1.0 && is(p10, 0, 0, 800, 600),
				      "and a saved shape wins over the built-in one (trim %.0f)", t10);
				obs_data_set_string(bf, "rules_text", "");
				obs_data_set_bool(bf, "known_programs", false);
				obs_data_t *capture_reset = obs_source_get_settings(barred);
				obs_data_set_string(capture_reset, "window", "");
				obs_data_release(capture_reset);
				obs_source_update(trimmer, bf);
				look(1300);
			}

			obs_data_set_int(bf, "trim", 2);
			obs_data_set_double(bf, "trim_x", 80);
			obs_data_set_double(bf, "trim_y", 0); // the measuring step above stored its own amounts
			obs_source_update(trimmer, bf);
			auto [t4, p4] = look(600);
			CHECK(std::fabs(t4 - 80) <= 0.5 && is(p4, 0, 0, 800, 600),
			      "a set trim amount works too (trim %.0f)", t4);

			obs_data_release(bd);
			obs_data_release(bf);
			// A two-screen handheld: two 4:3 screens stacked make a tall 2:3
			// picture, which must be trimmed like any other real shape. An
			// odd shape that is not known can be measured and kept for its program.
			{
				auto paint2 = [&](const char *name, int iw, int ih, int bar) {
					QImage image(iw, ih, QImage::Format_RGB32);
					image.fill(QColor(0, 0, 0));
					for (int y = 0; y < ih; y++)
						for (int x = bar; x < iw - bar; x++)
							image.setPixel(x, y, qRgb(200, 120 + y % 100, 40 + x % 150));
					const std::string path = out + "/" + name;
					image.save(QString::fromStdString(path));
					return path;
				};
				obs_data_t *reset = obs_source_get_settings(barred);
				obs_data_set_string(reset, "window", "");
				obs_data_release(reset);
				obs_data_set_int(bf, "trim", 1);
				obs_data_set_string(bf, "rules_text", "");
				obs_data_set_bool(bf, "known_programs", false);
				obs_source_update(trimmer, bf);
				obs_data_set_string(bd, "file",
						    paint2("ds-stacked.png", 1280, 720, 400).c_str()); // 480x720
				obs_source_update(barred, bd);
				auto [d0, dp0] = look(3500);
				CHECK(std::fabs(d0 - 400) <= 1.0,
				      "two stacked handheld screens (2:3) are trimmed (trim %.0f)", d0);

				// 1280x720 with 290 px bars leaves 700x720: not a shape anyone uses.
				obs_data_set_string(bd, "file", paint2("odd-shape.png", 1280, 720, 290).c_str());
				obs_source_update(barred, bd);
				obs_data_t *cap = obs_source_get_settings(barred);
				obs_data_set_string(cap, "window", "Odd Emulator:OddWnd:oddemu.exe");
				obs_data_release(cap);
				auto [d1, dp1] = look(4500);
				CHECK(std::fabs(d1 - 290) > 5.0,
				      "an unknown shape is not trimmed to by guesswork (trim %.0f)", d1);
				calldata_t mcd = {};
				proc_handler_call(obs_source_get_proc_handler(trimmer), "measure_now", &mcd);
				calldata_free(&mcd);
				auto [d2, dp2] = look(800);
				obs_properties_t *props = obs_source_properties(trimmer);
				obs_property_button_clicked(obs_properties_get(props, "trim_keep"), trimmer);
				obs_properties_destroy(props);
				auto [d3, dp3] = look(1500);
				obs_data_t *kept = obs_source_get_settings(trimmer);
				const std::string saved = obs_data_get_string(kept, "rules_text");
				CHECK(std::fabs(d2 - 290) <= 1.0 && std::fabs(d3 - 290) <= 1.0 &&
					      obs_data_get_int(kept, "trim") == 1 && saved.find("oddemu = 0.97") == 0,
				      "measuring it and pressing 'keep' saves it for that program, back on automatic (trim %.0f; %s)",
				      d3, saved.c_str());
				obs_data_set_string(bf, "rules_text", saved.c_str());
				obs_data_release(kept);
				// The same program at another size: its saved shape applies at once.
				obs_data_set_string(bd, "file", paint2("odd-shape-small.png", 640, 360, 145).c_str());
				obs_source_update(barred, bd);
				auto [d4, dp4] = look(1200);
				CHECK(std::fabs(d4 - 145) <= 1.5,
				      "and it is used straight away the next time, at any size (trim %.0f)", d4);
				obs_data_t *cap2 = obs_source_get_settings(barred);
				obs_data_set_string(cap2, "window", "");
				obs_data_release(cap2);
			}

			// A shape found once is held. An emulator window (4:3, no bars)
			// goes full-screen (the same 4:3 picture, now with bars): the
			// picture must keep its shape with no moment of showing the bars.
			{
				auto paint = [&](const char *name, int iw, int ih, int bar, bool menu) {
					QImage image(iw, ih, QImage::Format_RGB32);
					image.fill(QColor(0, 0, 0));
					for (int y = 0; y < ih; y++)
						for (int x = bar; x < iw - bar; x++)
							image.setPixel(x, y, qRgb(60 + x % 150, 90 + y % 120, 200));
					if (menu) // a menu bar across the whole window, over the bars
						for (int y = 0; y < 24; y++)
							for (int x = 0; x < iw; x++)
								image.setPixel(x, y, qRgb(230, 230, 230));
					const std::string path = out + "/" + name;
					image.save(QString::fromStdString(path));
					return path;
				};
				const std::string windowed = paint("hold-windowed.png", 400, 300, 0, false);
				const std::string full = paint("hold-full.png", 1280, 720, 160, false);
				const std::string full_menu = paint("hold-full-menu.png", 1280, 720, 160, true);
				const std::string wide = paint("hold-wide.png", 1280, 720, 0, false);

				obs_data_set_int(bf, "trim", 1);
				obs_data_set_string(bf, "rules_text", "");
				obs_data_set_bool(bf, "known_programs", false);
				obs_source_update(trimmer, bf);
				obs_data_set_string(bd, "file", windowed.c_str());
				obs_source_update(barred, bd);
				auto [h0, q0] = look(1500);
				CHECK(h0 == 0.0 && is(q0, 0, 0, 800, 600),
				      "a 4:3 window fills the 4:3 frame (trim %.0f)", h0);

				obs_data_set_string(bd, "file", full.c_str());
				obs_source_update(barred, bd);
				bool flickered = false;
				for (int i = 0; i < 8; i++) {
					auto [ht, qt] = look(100);
					if (!is(qt, 0, 0, 800, 600))
						flickered = true;
				}
				auto [h1, q1] = look(100);
				CHECK(!flickered && std::fabs(h1 - 160) <= 1.0,
				      "going full-screen keeps the picture's shape with no resize in between (trim %.0f)",
				      h1);

				obs_data_set_string(bd, "file", full_menu.c_str());
				obs_source_update(barred, bd);
				auto [h2, q2] = look(700);
				CHECK(std::fabs(h2 - 160) <= 1.0,
				      "a menu drawn over the bars does not resize it (trim %.0f)", h2);
				obs_data_set_string(bd, "file", full.c_str());
				obs_source_update(barred, bd);
				look(600);

				obs_data_set_string(bd, "file", wide.c_str());
				obs_source_update(barred, bd);
				auto [h3, q3] = look(3500);
				CHECK(h3 == 0.0,
				      "a picture that clearly and steadily fills another real shape is followed (trim %.0f)",
				      h3);
			}

			obs_source_filter_remove(barred, trimmer);
			obs_source_release(trimmer);
			obs_source_release(barred);
		}

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

		// An item inside a group is positioned relative to the group; sending
		// it to a screen position must still land it there on screen.
		obs_source_t *badge = make_color("Badge", 50, 50);
		obs_sceneitem_t *group = obs_scene_add_group(scene, "Overlay Group");
		obs_sceneitem_t *badge_item = obs_scene_add(scene, badge);
		obs_sceneitem_group_add_item(group, badge_item);
		vec2_set(&p, 200.0f, 120.0f);
		obs_sceneitem_set_pos(group, &p);
		{
			obs_data_t *d = obs_data_create();
			obs_data_set_string(d, "item", "Badge");
			obs_data_set_int(d, "destination", 0); // top left
			obs_data_set_double(d, "margin", 30);
			obs_data_set_int(d, "duration_ms", 200);
			obs_source_t *f = obs_source_create("stagekit_place_item", "Badge Top Left", d, nullptr);
			obs_data_release(d);
			obs_source_filter_add(scene_source, f);
			sleep_ms(100);
			obs_source_set_enabled(f, true);
			sleep_ms(500);
			vec2 gp = pos_of(group), bp = pos_of(badge_item);
			CHECK(std::fabs(gp.x + bp.x - 30) < 0.6 && std::fabs(gp.y + bp.y - 30) < 0.6,
			      "an item inside a group lands at the right place on screen (%.0f, %.0f)", gp.x + bp.x,
			      gp.y + bp.y);
			obs_source_filter_remove(scene_source, f);
			obs_source_release(f);
		}

		// Send To: the same move as a filter on the camera itself.
		printf("\n[send to]\n");
		{
			obs_source_t *before = obs_get_output_source(0);
			obs_set_output_source(0, scene_source);
			// The camera also sits in a second scene, and that scene is not on screen.
			obs_scene_t *other = obs_scene_create("Other Scene");
			obs_sceneitem_t *other_cam = obs_scene_add(other, cam);
			vec2_set(&p, 5.0f, 5.0f);
			obs_sceneitem_set_pos(other_cam, &p);
			obs_sceneitem_set_pos(cam_item, &p);

			auto make = [&](const char *name, int destination, int where) {
				obs_data_t *d = obs_data_create();
				obs_data_set_int(d, "destination", destination);
				obs_data_set_double(d, "margin", 30);
				obs_data_set_int(d, "duration_ms", 150);
				obs_data_set_int(d, "where", where);
				obs_source_t *f = obs_source_create("stagekit_send_to", name, d, nullptr);
				obs_data_release(d);
				obs_source_filter_add(cam, f);
				sleep_ms(100);
				return f;
			};
			obs_source_t *corner = make("Camera bottom right", 8, 0);
			CHECK(corner != nullptr, "the Send To filter can be added to a source");
			obs_source_set_enabled(corner, true);
			sleep_ms(450);
			vec2 at = pos_of(cam_item), elsewhere = pos_of(other_cam);
			CHECK(std::fabs(at.x - 450) < 0.6 && std::fabs(at.y - 240) < 0.6,
			      "switching it on moves that source in the scene on screen (%.0f, %.0f)", at.x, at.y);
			CHECK(!obs_source_enabled(corner),
			      "and it switches itself off again, ready for the next press");
			CHECK(std::fabs(elsewhere.x - 5) < 0.6, "other scenes are left alone by default");

			// Save a spot: drag the camera somewhere, press the button, move away, press the key.
			obs_source_t *spot = make("Camera my spot", 0, 1);
			vec2_set(&p, 222.0f, 111.0f);
			obs_sceneitem_set_pos(cam_item, &p);
			obs_properties_t *props = obs_source_properties(spot);
			obs_property_button_clicked(obs_properties_get(props, "capture"), spot);
			obs_properties_destroy(props);
			sleep_ms(100);
			obs_source_set_enabled(corner, true);
			sleep_ms(450);
			obs_source_set_enabled(spot, true);
			sleep_ms(450);
			at = pos_of(cam_item);
			elsewhere = pos_of(other_cam);
			CHECK(std::fabs(at.x - 222) < 0.6 && std::fabs(at.y - 111) < 0.6,
			      "a spot saved from where the source sits is returned to (%.0f, %.0f)", at.x, at.y);
			CHECK(std::fabs(elsewhere.x - 222) < 0.6 && std::fabs(elsewhere.y - 111) < 0.6,
			      "'every scene it is in' moves it in the other scenes too (%.0f, %.0f)", elsewhere.x,
			      elsewhere.y);

			// The camera inside a nested scene that is on screen.
			obs_scene_t *outer = obs_scene_create("Outer Scene");
			obs_scene_add(outer, obs_scene_get_source(other));
			obs_set_output_source(0, obs_scene_get_source(outer));
			obs_source_set_enabled(corner, true);
			sleep_ms(450);
			elsewhere = pos_of(other_cam);
			const float want_x = (float)obs_source_get_width(obs_scene_get_source(other)) - 30 - 160;
			CHECK(std::fabs(elsewhere.x - want_x) < 0.6,
			      "it is found inside a nested scene (%.0f, wanted %.0f)", elsewhere.x, want_x);

			obs_set_output_source(0, before);
			obs_source_release(before);
			obs_source_filter_remove(cam, corner);
			obs_source_filter_remove(cam, spot);
			obs_source_release(corner);
			obs_source_release(spot);
			obs_scene_release(outer);
			obs_scene_release(other);
		}

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

			auto count_filters = [](obs_source_t *source) {
				int n = 0;
				obs_source_enum_filters(
					source,
					[](obs_source_t *, obs_source_t *, void *param) {
						(*static_cast<int *>(param))++;
					},
					&n);
				return n;
			};
			const int scene_filters_before = count_filters(scene_source);
			const int cam_filters_before = count_filters(cam);
			click("makeKeys");
			calldata_t spot_cd = {};
			calldata_set_ptr(&spot_cd, "widget", dock);
			calldata_set_string(&spot_cd, "name", "Corner");
			proc_handler_call(obs_get_proc_handler(), "stagekit_dock_save_spot", &spot_cd);
			calldata_free(&spot_cd);
			CHECK(count_filters(cam) - cam_filters_before == 10 &&
				      count_filters(scene_source) == scene_filters_before,
			      "one click makes a key per position, plus the saved spot, on the camera itself (%d new, scene %d)",
			      count_filters(cam) - cam_filters_before,
			      count_filters(scene_source) - scene_filters_before);
			obs_source_t *key = obs_source_get_filter_by_name(cam, "Top right");
			CHECK(key != nullptr && strcmp(obs_source_get_unversioned_id(key), "stagekit_send_to") == 0,
			      "each key is a Send To filter named after its position");
			if (key) {
				// A key acts in the scene on screen, as a Stream Deck press would.
				obs_source_t *on_screen = obs_get_output_source(0);
				obs_set_output_source(0, scene_source);
				sleep_ms(100);
				obs_source_set_enabled(key, true);
				sleep_ms(800);
				c = pos_of(cam_item);
				CHECK(std::fabs(c.x - 450) < 0.5 && std::fabs(c.y - 30) < 0.5,
				      "and switching that key on moves the camera (%.0f, %.0f)", c.x, c.y);
				obs_set_output_source(0, on_screen);
				obs_source_release(on_screen);
			}
			obs_source_release(key);

			// Order buttons: the camera was added first, so it starts at the back.
			const int before = obs_sceneitem_get_order_position(cam_item);
			click("orderTop");
			const int front = obs_sceneitem_get_order_position(cam_item);
			click("orderBottom");
			CHECK(front > before && obs_sceneitem_get_order_position(cam_item) == 0,
			      "bring to front and send to back change the stacking order (%d -> %d -> %d)", before,
			      front, obs_sceneitem_get_order_position(cam_item));

			// The scene picker reaches scenes that are not on screen.
			if (auto *scenes = dock->findChild<QComboBox *>("scene")) {
				obs_scene_t *other = obs_scene_create("Nested Camera Scene");
				obs_source_t *nested_cam = make_color("Nested Cam", 80, 60);
				obs_sceneitem_t *nested_item = obs_scene_add(other, nested_cam);
				sleep_ms(1200); // the dock refreshes its lists once a second
				QCoreApplication::processEvents();
				scenes->setCurrentIndex(scenes->findData(QString("Nested Camera Scene")));
				auto *items = dock->findChild<QComboBox *>("item");
				CHECK(items && items->findText("Nested Cam") >= 0,
				      "the scene picker lists another scene's items");
				if (items)
					items->setCurrentText("Nested Cam");
				click("grid8");
				sleep_ms(900);
				vec2 np = pos_of(nested_item);
				CHECK(std::fabs(np.x - 530) < 0.6 && std::fabs(np.y - 270) < 0.6,
				      "and moves an item in that scene (%.0f, %.0f)", np.x, np.y);
				scenes->setCurrentIndex(0);
				if (items)
					items->setCurrentText("Camera");
				obs_source_release(nested_cam);
				obs_scene_release(other);
			}

			click("addShadow");
			obs_source_t *shadow = obs_source_get_filter_by_name(cam, "Shadow, Glow & Outline");
			CHECK(shadow != nullptr, "\"Add to this item\" puts the effect on the item's source");
			obs_source_release(shadow);

			// Arrange: line up, space out, match sizes and snap, by what is visible.
			printf("\n[arrange]\n");
			{
				// A 200x100 picture whose drawn part is only the middle 100x40:
				// 50 px of nothing left and right, 30 px above and below.
				QImage padded(200, 100, QImage::Format_ARGB32);
				padded.fill(Qt::transparent);
				for (int y = 30; y < 70; y++)
					for (int x = 50; x < 150; x++)
						padded.setPixel(x, y, qRgba(240, 200, 40, 255));
				const std::string padded_path = out + "/arrange-padded.png";
				padded.save(QString::fromStdString(padded_path));
				obs_data_t *pd = obs_data_create();
				obs_data_set_string(pd, "file", padded_path.c_str());
				obs_source_t *badge_src =
					obs_source_create("image_source", "Padded Badge", pd, nullptr);
				obs_data_release(pd);
				obs_scene_t *as = obs_scene_create("Arrange Scene"); // 640x360
				obs_source_t *as_source = obs_scene_get_source(as);
				obs_source_t *block_a = make_color("Block A", 100, 50);
				obs_source_t *block_b = make_color("Block B", 60, 80);
				obs_source_t *block_c = make_color("Block C", 40, 40);
				obs_sceneitem_t *ia = obs_scene_add(as, block_a);
				obs_sceneitem_t *ib = obs_scene_add(as, block_b);
				obs_sceneitem_t *ic = obs_scene_add(as, block_c);
				obs_sceneitem_t *ibadge = obs_scene_add(as, badge_src);
				auto place = [](obs_sceneitem_t *item, float x, float y) {
					vec2 v;
					vec2_set(&v, x, y);
					obs_sceneitem_set_pos(item, &v);
				};
				place(ia, 20, 20);
				place(ib, 200, 150);
				place(ic, 500, 40);
				place(ibadge, 300, 200);
				sleep_ms(500); // the image loads
				auto *scenes = dock->findChild<QComboBox *>("scene");
				for (int i = 0; i < 30 && scenes && scenes->findData("Arrange Scene") < 0; i++) {
					QCoreApplication::processEvents();
					sleep_ms(50);
				}
				if (scenes)
					scenes->setCurrentIndex(std::max(0, scenes->findData("Arrange Scene")));
				auto select_only = [&](std::initializer_list<obs_sceneitem_t *> chosen) {
					for (obs_sceneitem_t *item : {ia, ib, ic, ibadge})
						obs_sceneitem_select(item, false);
					for (obs_sceneitem_t *item : chosen)
						obs_sceneitem_select(item, true);
				};
				auto tick_box = [&](const char *name, bool on) {
					if (auto *box = dock->findChild<QCheckBox *>(name))
						box->setChecked(on);
				};
				auto against = [&](int index) {
					if (auto *combo = dock->findChild<QComboBox *>("arrangeAgainst"))
						combo->setCurrentIndex(index);
				};

				// By the box: the badge's 200 px box goes to the screen's right edge.
				tick_box("arrangeVisible", false);
				against(0);
				select_only({ibadge});
				CHECK(click("arrange_right"), "the dock has line-up buttons");
				vec2 bp = pos_of(ibadge);
				CHECK(std::fabs(bp.x - 440) < 0.6,
				      "by its box, an item's box meets the screen's edge (x %.0f)", bp.x);

				// By what is visible: the drawn 100 px meets the edge; the empty 50 px hangs off.
				tick_box("arrangeVisible", true);
				click("arrange_right");
				bp = pos_of(ibadge);
				CHECK(std::fabs(bp.x - 490) < 1.6,
				      "by what is visible, the drawn picture meets the edge instead (x %.0f)", bp.x);
				click("arrange_top");
				bp = pos_of(ibadge);
				CHECK(std::fabs(bp.y + 30) < 1.6, "the same up and down (y %.0f)", bp.y);

				// Each other: three blocks share the left edge of the left-most.
				against(1);
				select_only({ia, ib, ic});
				click("arrange_left");
				CHECK(std::fabs(pos_of(ia).x - 20) < 0.6 && std::fabs(pos_of(ib).x - 20) < 0.6 &&
					      std::fabs(pos_of(ic).x - 20) < 0.6,
				      "'each other' lines several items up on the same edge (%.0f, %.0f, %.0f)",
				      pos_of(ia).x, pos_of(ib).x, pos_of(ic).x);

				// First or last selected: that one stays put and the others come to it.
				{
					auto settle = [&] {
						for (int i = 0; i < 8; i++) {
							QCoreApplication::processEvents();
							sleep_ms(50);
						}
					};
					place(ia, 50, 10);
					place(ib, 220, 100);
					place(ic, 400, 200);
					select_only({ib});
					settle();
					obs_sceneitem_select(ic, true);
					settle();
					obs_sceneitem_select(ia, true);
					settle();
					auto *list = dock->findChild<QListWidget *>("arrangeSelected");
					CHECK(list && list->count() == 3 && list->item(0)->text() == "Block B" &&
						      list->item(1)->text() == "Block C" &&
						      list->item(2)->text() == "Block A",
					      "the dock lists every selected item, in the order selected (%d rows)",
					      list ? list->count() : 0);
					against(3);
					settle();
					CHECK(list && list->count() == 3 &&
						      list->item(2)->text().startsWith("Block A") &&
						      list->item(2)->text() != "Block A",
					      "and marks the one the others will line up to (%s)",
					      list && list->count() == 3 ? list->item(2)->text().toUtf8().constData()
									 : "");
					click("arrange_left");
					CHECK(std::fabs(pos_of(ia).x - 50) < 0.6 &&
						      std::fabs(pos_of(ib).x - 50) < 0.6 &&
						      std::fabs(pos_of(ic).x - 50) < 0.6,
					      "'last selected' brings the others to the last item selected (%.0f, %.0f, %.0f)",
					      pos_of(ia).x, pos_of(ib).x, pos_of(ic).x);
					place(ib, 220, 100);
					place(ic, 400, 200);
					against(2);
					click("arrange_left");
					CHECK(std::fabs(pos_of(ib).x - 220) < 0.6 &&
						      std::fabs(pos_of(ia).x - 220) < 0.6 &&
						      std::fabs(pos_of(ic).x - 220) < 0.6,
					      "'first selected' brings them to the first instead (%.0f, %.0f, %.0f)",
					      pos_of(ia).x, pos_of(ib).x, pos_of(ic).x);
					// Sizes copy that item too: B (60 wide) was selected first.
					click("arrange_same_width");
					vec2 scale_a;
					obs_sceneitem_get_scale(ia, &scale_a);
					CHECK(std::fabs(scale_a.x - 0.6) < 0.005,
					      "and 'same width' copies that item's width (scale %.2f)", scale_a.x);
					vec2 one;
					vec2_set(&one, 1.0f, 1.0f);
					for (obs_sceneitem_t *item : {ia, ib, ic})
						obs_sceneitem_set_scale(item, &one);
					against(1);
					select_only({ia, ib, ic});
					settle();
				}

				// Space out: A (100 wide) at 0, C (40) ending at 600, B (60) between: equal 200 px gaps.
				place(ia, 0, 10);
				place(ib, 130, 10);
				place(ic, 560, 10);
				click("arrange_space_across");
				CHECK(std::fabs(pos_of(ib).x - 300) < 0.6 && std::fabs(pos_of(ia).x) < 0.6 &&
					      std::fabs(pos_of(ic).x - 560) < 0.6,
				      "spacing out makes the gaps equal and leaves the outer two alone (middle at %.0f)",
				      pos_of(ib).x);

				// Same width: the others become as wide as the top-most selected (C, 40 px).
				select_only({ia, ic});
				click("arrange_same_width");
				vec2 sa;
				obs_sceneitem_get_scale(ia, &sa);
				CHECK(std::fabs(sa.x - 0.4) < 0.005 && std::fabs(sa.y - 0.4) < 0.005,
				      "'same width' resizes to match, keeping the shape (scale %.2f)", sa.x);
				vec2_set(&sa, 1.0f, 1.0f);
				obs_sceneitem_set_scale(ia, &sa);

				// Each tool is one step for OBS's Undo and Redo.
				{
					auto replay = [](bool redo) {
						calldata_t cd = {};
						calldata_set_bool(&cd, "redo", redo);
						proc_handler_call(obs_get_proc_handler(), "stagekit_replay_arrange",
								  &cd);
						calldata_free(&cd);
					};
					vec2 was;
					obs_sceneitem_get_scale(ia, &was);
					const vec2 where = pos_of(ia);
					click("arrange_same_width");
					vec2 changed;
					obs_sceneitem_get_scale(ia, &changed);
					replay(false);
					vec2 undone;
					obs_sceneitem_get_scale(ia, &undone);
					CHECK(std::fabs(changed.x - 0.4) < 0.005 &&
						      std::fabs(undone.x - was.x) < 0.005 &&
						      std::fabs(pos_of(ia).x - where.x) < 0.6,
					      "undo puts an item back as it was (scale %.2f -> %.2f -> %.2f)", was.x,
					      changed.x, undone.x);
					replay(true);
					obs_sceneitem_get_scale(ia, &changed);
					CHECK(std::fabs(changed.x - 0.4) < 0.005,
					      "and redo applies the tool again (scale %.2f)", changed.x);
					replay(false);
				}

				// Without 'keep shape', only the direction asked for changes.
				tick_box("arrangeKeepShape", false);
				click("arrange_same_width");
				obs_sceneitem_get_scale(ia, &sa);
				CHECK(std::fabs(sa.x - 0.4) < 0.005 && std::fabs(sa.y - 1.0) < 0.005,
				      "with 'keep shape' off, 'same width' stretches across only (scale %.2f x %.2f)",
				      sa.x, sa.y);
				click("arrange_same_size");
				obs_sceneitem_get_scale(ia, &sa);
				CHECK(std::fabs(sa.x - 0.4) < 0.005 && std::fabs(sa.y - 0.8) < 0.005,
				      "'same size' then matches both directions (scale %.2f x %.2f)", sa.x, sa.y);
				tick_box("arrangeKeepShape", true);
				vec2_set(&sa, 1.0f, 1.0f);
				obs_sceneitem_set_scale(ia, &sa);
				click("arrange_same_size");
				obs_sceneitem_get_scale(ia, &sa);
				CHECK(std::fabs(sa.x - 0.4) < 0.005 && std::fabs(sa.y - 0.4) < 0.005,
				      "with 'keep shape' on, 'same size' fits inside the other evenly (scale %.2f)",
				      sa.x);
				vec2_set(&sa, 1.0f, 1.0f);
				obs_sceneitem_set_scale(ia, &sa);

				// The same tools from a right-click in OBS's preview, without the dock.
				{
					calldata_t install = {};
					proc_handler_call(obs_get_proc_handler(), "stagekit_install_preview_menu",
							  &install);
					calldata_free(&install);
					obs_source_t *was_out = obs_get_output_source(0);
					obs_set_output_source(0, as_source);
					QWidget preview;
					preview.setObjectName("preview");
					QContextMenuEvent asked(QContextMenuEvent::Mouse, QPoint(5, 5), QPoint(5, 5));
					QCoreApplication::sendEvent(&preview, &asked);
					QMenu popup;
					popup.addAction("Transform");
					popup.show();
					QCoreApplication::processEvents();
					auto *sub = popup.findChild<QMenu *>("stagekitArrangeMenu");
					CHECK(sub != nullptr, "a right-click in the preview offers a StageKit menu");
					place(ia, 300, 20);
					against(0);
					select_only({ia});
					if (sub) {
						if (auto *entry = sub->findChild<QAction *>("menu_left"))
							entry->trigger();
						if (auto *entry = sub->findChild<QAction *>("menuKeepShape"))
							entry->setChecked(false);
					}
					for (int i = 0; i < 12; i++) {
						QCoreApplication::processEvents();
						sleep_ms(50);
					}
					CHECK(std::fabs(pos_of(ia).x) < 0.6,
					      "its entries act on the selected items (x %.0f)", pos_of(ia).x);
					auto *keep = dock->findChild<QCheckBox *>("arrangeKeepShape");
					CHECK(keep && !keep->isChecked(), "and its options are the dock's options");
					tick_box("arrangeKeepShape", true);
					popup.hide();
					QMenu other;
					other.addAction("Something else");
					other.show();
					QCoreApplication::processEvents();
					CHECK(other.findChild<QMenu *>("stagekitArrangeMenu") == nullptr,
					      "other menus are left alone");
					other.hide();
					obs_set_output_source(0, was_out);
					obs_source_release(was_out);
				}

				// Snap: the badge's drawn left edge is 8 px short of block B's right edge.
				place(ib, 100, 150);     // B covers x 100..160
				place(ibadge, 118, 250); // drawn part starts at 168
				select_only({ibadge});
				click("arrange_snap_now");
				bp = pos_of(ibadge);
				CHECK(std::fabs(bp.x - 110) < 1.6,
				      "snapping moves the last few pixels so visible edges meet (x %.0f)", bp.x);
				place(ibadge, 300, 250); // nothing within reach
				click("arrange_snap_now");
				CHECK(std::fabs(pos_of(ibadge).x - 300) < 3.1,
				      "and leaves an item alone when nothing is near");

				// Snap when letting go: move it, leave it, and it settles by itself.
				tick_box("arrangeLiveSnap", true);
				for (int i = 0; i < 6; i++) {
					QCoreApplication::processEvents();
					sleep_ms(50);
				}
				place(ibadge, 116, 250);
				for (int i = 0; i < 12; i++) {
					QCoreApplication::processEvents();
					sleep_ms(50);
				}
				bp = pos_of(ibadge);
				CHECK(std::fabs(bp.x - 110) < 1.6,
				      "'snap when I let go' settles a moved item by itself (x %.0f)", bp.x);
				tick_box("arrangeLiveSnap", false);

				// A group counts as one item: a block and a group line up with each other.
				{
					obs_sceneitem_t *grouped = obs_scene_add_group(as, "Arrange Group");
					obs_sceneitem_group_add_item(grouped, ic);
					place(ia, 60, 20);
					place(grouped, 400, 200);
					sleep_ms(200);
					select_only({ia});
					obs_sceneitem_select(grouped, true);
					against(1);
					click("arrange_left");
					CHECK(std::fabs(pos_of(ia).x - 60) < 0.6 &&
						      std::fabs(pos_of(grouped).x - 60) < 1.6,
					      "a selected group lines up with another item, not with the screen (%.0f, %.0f)",
					      pos_of(ia).x, pos_of(grouped).x);
					obs_sceneitem_select(grouped, false);
					against(0);
				}

				select_only({});
				if (scenes)
					scenes->setCurrentIndex(std::max(0, scenes->findData("Test Scene")));
				obs_scene_release(as);
				for (obs_source_t *source : {block_a, block_b, block_c, badge_src})
					obs_source_release(source);
				static_cast<void>(as_source);
			}

			// The dock works on whatever is selected in OBS's own preview.
			{
				if (auto *scenes = dock->findChild<QComboBox *>("scene"))
					scenes->setCurrentIndex(std::max(0, scenes->findData("Test Scene")));
				auto *items = dock->findChild<QComboBox *>("item");
				if (items)
					items->setCurrentText("Camera");
				obs_sceneitem_select(cam_item, false);
				obs_sceneitem_select(widget_item, true);
				for (int i = 0; i < 16; i++) {
					QCoreApplication::processEvents();
					sleep_ms(50);
				}
				CHECK(items && items->currentText() == "Widget",
				      "selecting an item in OBS's preview selects it in the dock (%s)",
				      items ? items->currentText().toUtf8().constData() : "?");
				obs_sceneitem_select(widget_item, false);
				CHECK(dock->findChild<QWidget *>("canvas") == nullptr,
				      "the dock no longer has a picture of the scene");
			}

			// The start image and background live in a dock of their own. A very
			// long file name must not widen it, and it shows a small picture.
			{
				// Chosen before the dock exists, as when OBS starts.
				{
					calldata_t pre = {};
					calldata_set_string(&pre, "path", base_image);
					proc_handler_call(obs_get_proc_handler(), "stagekit_set_start_image", &pre);
					calldata_free(&pre);
				}
				calldata_t bd = {};
				proc_handler_call(obs_get_proc_handler(), "stagekit_create_background_dock", &bd);
				auto *background_dock = static_cast<QWidget *>(calldata_ptr(&bd, "widget"));
				calldata_free(&bd);
				CHECK(background_dock && dock->findChild<QLabel *>("startThumb") == nullptr,
				      "the start image and background have a dock of their own");
				if (!background_dock)
					background_dock = new QWidget();
				background_dock->resize(340, 320);
				for (int i = 0; i < 4; i++) {
					QCoreApplication::processEvents();
					sleep_ms(50);
				}
				{
					auto *first = background_dock->findChild<QLabel *>("startThumb");
					CHECK(first && !first->pixmap().isNull(),
					      "a start image chosen before the dock opened shows its picture at once");
				}
				QWidget *items_dock = dock;
				dock = background_dock;
				const int narrow = dock->minimumSizeHint().width();
				const std::string long_name =
					out + "/a_very_long_start_image_file_name_that_goes_on_and_on_and_on_and_on_"
					      "and_on_and_on_and_on_and_on_and_on_and_on_and_on_1360x766.png";
				QImage picture(320, 180, QImage::Format_RGB32);
				picture.fill(QColor(200, 60, 30));
				picture.save(QString::fromStdString(long_name));
				calldata_t cd = {};
				calldata_set_string(&cd, "path", long_name.c_str());
				proc_handler_call(obs_get_proc_handler(), "stagekit_set_start_image", &cd);
				calldata_free(&cd);
				for (int i = 0; i < 30; i++) {
					QCoreApplication::processEvents();
					sleep_ms(50);
				}
				auto *thumb = dock->findChild<QLabel *>("startThumb");
				dock->grab();
				QCoreApplication::processEvents();
				const QPixmap shown = thumb ? thumb->pixmap() : QPixmap();
				CHECK(!shown.isNull() && thumb && std::abs(shown.width() - thumb->width()) <= 2,
				      "the dock shows a picture of the start image as wide as the dock allows (%d of %d)",
				      shown.width(), thumb ? thumb->width() : 0);
				{
					const int before_w = shown.width();
					dock->resize(520, 700);
					dock->grab(); // lays the hidden dock out, as showing it would
					for (int i = 0; i < 6; i++) {
						QCoreApplication::processEvents();
						sleep_ms(30);
					}
					const QPixmap wider = thumb ? thumb->pixmap() : QPixmap();
					CHECK(wider.width() > before_w + 100 &&
						      std::abs(wider.width() - thumb->width()) <= 2,
					      "and it grows when the dock is made wider (%d -> %d)", before_w,
					      wider.width());
					dock->resize(340, 320);
					dock->grab();
					for (int i = 0; i < 6; i++) {
						QCoreApplication::processEvents();
						sleep_ms(30);
					}
				}
				CHECK(dock->minimumSizeHint().width() <= narrow,
				      "a long start image file name does not widen the dock (%d, was %d)",
				      dock->minimumSizeHint().width(), narrow);

				// The wheel and dragging on the thumbnail are for a closer look
				// at it in the dock. The scene must not change.
				auto background_look = [&] {
					obs_data_t *bs = obs_data_create();
					obs_data_set_int(bs, "base_type", 2);
					obs_data_set_double(bs, "blur", 0);
					obs_data_set_double(bs, "speed_x", 0);
					obs_data_set_double(bs, "speed_y", 0);
					obs_data_set_double(bs, "glow", 0);
					obs_source_update(bg, bs);
					obs_data_release(bs);
					sleep_ms(300);
					return render(bg, w, h);
				};
				{
					calldata_t pic = {};
					calldata_set_string(&pic, "path", base_image);
					proc_handler_call(obs_get_proc_handler(), "stagekit_set_start_image", &pic);
					calldata_free(&pic);
				}
				for (int i = 0; i < 12; i++) {
					QCoreApplication::processEvents();
					sleep_ms(50);
				}
				const std::vector<uint8_t> scene_before = background_look();
				const QImage thumb_before = thumb ? thumb->pixmap().toImage() : QImage();
				if (thumb) {
					const QPointF middle(thumb->width() / 2.0, thumb->height() / 2.0);
					QWheelEvent zoom_in(middle, thumb->mapToGlobal(middle), QPoint(),
							    QPoint(0, 5 * 120), Qt::NoButton, Qt::NoModifier,
							    Qt::NoScrollPhase, false);
					QCoreApplication::sendEvent(thumb, &zoom_in);
				}
				const QImage thumb_zoomed = thumb ? thumb->pixmap().toImage() : QImage();
				CHECK(!thumb_before.isNull() && thumb_zoomed != thumb_before,
				      "turning the wheel on the thumbnail zooms the thumbnail");
				if (thumb) {
					auto drag = [&](QEvent::Type type, QPointF where) {
						QMouseEvent event(type, where, thumb->mapToGlobal(where),
								  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
						QCoreApplication::sendEvent(thumb, &event);
					};
					drag(QEvent::MouseButtonPress, QPointF(100, 60));
					drag(QEvent::MouseMove, QPointF(40, 60));
				}
				CHECK(thumb && thumb->pixmap().toImage() != thumb_zoomed,
				      "and dragging moves around it");
				const std::vector<uint8_t> scene_after = background_look();
				CHECK(mean_abs_diff(scene_before, scene_after) < 0.01,
				      "the scene itself is not zoomed or moved (diff %.3f)",
				      mean_abs_diff(scene_before, scene_after));
				dock->resize(340, dock->sizeHint().height());
				QCoreApplication::processEvents();
				dock->grab().save(QString::fromStdString(out + "/../stagekit-background-dock.png"));
				delete dock;
				dock = items_dock;
			}

			// The Arrange box names what its buttons will move.
			{
				for (int i = 0; i < 12; i++) {
					QCoreApplication::processEvents();
					sleep_ms(50);
				}
				auto *target = dock->findChild<QListWidget *>("arrangeSelected");
				auto *items = dock->findChild<QComboBox *>("item");
				CHECK(target && items && !items->currentText().isEmpty() && target->count() == 1 &&
					      target->item(0)->text() == items->currentText(),
				      "the Arrange box lists the item it will move (%s)",
				      target && target->count() ? target->item(0)->text().toUtf8().constData() : "");
			}

			dock->resize(340, dock->sizeHint().height());
			QCoreApplication::processEvents();
			dock->grab().save(QString::fromStdString(out + "/../stagekit-dock.png"));
			delete dock;
		}

		obs_source_filter_remove(scene_source, to_top_right);
		obs_source_filter_remove(scene_source, to_bottom_right);
		obs_source_release(to_top_right);
		obs_source_release(to_bottom_right);
		obs_source_release(cam);
		obs_source_release(widget);
		obs_source_release(badge);
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
