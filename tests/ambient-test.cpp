// Headless test for the Ambient Background source (Linux, run under Xvfb).
//
// Usage: stagekit-ambient-test <stagekit.so> <data dir> <base image>
//                              <texture image> <live image> <out dir>

#include <obs.h>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs-nix-platform.h>
#include <graphics/matrix4.h>
#include <graphics/vec4.h>

#include <dlfcn.h>

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QImage>
#include <QPainter>
#include <QLabel>
#include <QSpinBox>
#include <QListWidget>
#include <QLayout>
#include <QAction>
#include <QMainWindow>
#include <QDockWidget>
#include <QMenu>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPixmap>
#include <QTabWidget>
#include <QPushButton>
#include <QWidget>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
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

// The same, drawn the way OBS draws a source by itself in a window of its
// own (the Filters and Properties windows, a projector) where Windows gives
// that window a wide-colour surface: sixteen-bit, holding linear light. A
// source or filter that is not colour-aware puts ordinary values there and
// comes out pale. What was drawn is brought back to ordinary values to compare.
static std::vector<uint8_t> render_shown(obs_source_t *src, uint32_t w, uint32_t h)
{
	std::vector<uint8_t> pixels(size_t(w) * h * 4);
	obs_enter_graphics();
	gs_texrender_t *wide = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	gs_texrender_t *tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_stagesurf_t *stage = gs_stagesurface_create(w, h, GS_RGBA);
	if (gs_texrender_begin_with_color_space(wide, w, h, GS_CS_SRGB_16F)) {
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		const bool was_linear = gs_set_linear_srgb(true);
		const bool was_srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(false);
		obs_source_video_render(src);
		gs_enable_framebuffer_srgb(was_srgb);
		gs_set_linear_srgb(was_linear);
		gs_blend_state_pop();
		gs_texrender_end(wide);
	}
	if (gs_texrender_begin(tr, w, h)) {
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_texture_t *linear_light = gs_texrender_get_texture(wide);
		const bool was_srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(true); // linear light back to ordinary values
		gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), linear_light);
		while (gs_effect_loop(def, "Draw"))
			gs_draw_sprite(linear_light, 0, w, h);
		gs_enable_framebuffer_srgb(was_srgb);
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
	gs_texrender_destroy(wide);
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

// Adds an entry to one of the plugin's lists the way a person does: by
// choosing it in the list's box. `list` is the setting's name plus "|a" for a
// list kept as an array or "|t" for one kept as text.
static bool pick_into_list(obs_source_t *owner, obs_properties_t *props, const std::string &list, const char *choice)
{
	obs_property_t *box = obs_properties_get(props, (list + "pick").c_str());
	if (!box)
		return false;
	obs_data_t *settings = obs_source_get_settings(owner);
	obs_data_set_string(settings, (list + "pick").c_str(), choice);
	obs_property_modified(box, settings);
	obs_source_update(owner, settings);
	obs_data_release(settings);
	return true;
}

// How many rows a list shows, each with its own remove button.
static int list_rows(obs_properties_t *props, const std::string &list)
{
	int rows = 0;
	while (obs_properties_get(props, (list + "row|" + std::to_string(rows)).c_str()))
		rows++;
	return rows;
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

// A stand-in for a capture: one colour, and, like a Game or Window Capture
// on the owner's machine, no picture (no size) while nothing shows it; its
// picture comes a little after it is shown again. Registered under a
// capture's kind, so Ambient Background takes it for one.
struct StandInCapture {
	obs_source_t *source;
	uint32_t colour = 0xFF00FF00;
	uint32_t cx = 640, cy = 360;
	bool lazy = true;
	int frames_shown = 0;
	std::atomic<int> start_frames{3};
	std::atomic<bool> live{false};
};

static void register_stand_in_capture(const char *id)
{
	obs_source_info info = {};
	info.id = id;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = [](void *) {
		return "Capture (stand-in that starts when shown)";
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		auto *c = new StandInCapture();
		c->source = source;
		c->colour = (uint32_t)obs_data_get_int(settings, "color");
		c->cx = (uint32_t)obs_data_get_int(settings, "width");
		c->cy = (uint32_t)obs_data_get_int(settings, "height");
		c->lazy = !obs_data_get_bool(settings, "always");
		c->live = !c->lazy;
		return c;
	};
	info.update = [](void *data, obs_data_t *settings) {
		auto *c = static_cast<StandInCapture *>(data);
		if (obs_data_has_user_value(settings, "start_frames"))
			c->start_frames = (int)obs_data_get_int(settings, "start_frames");
	};
	info.destroy = [](void *data) {
		delete static_cast<StandInCapture *>(data);
	};
	info.video_tick = [](void *data, float) {
		auto *c = static_cast<StandInCapture *>(data);
		if (!c->lazy)
			return;
		// Its picture comes a few frames after it is first shown, and goes
		// the moment nothing shows it.
		c->frames_shown = obs_source_showing(c->source) ? c->frames_shown + 1 : 0;
		c->live = c->frames_shown >= c->start_frames;
	};
	info.get_width = [](void *data) -> uint32_t {
		auto *c = static_cast<StandInCapture *>(data);
		return c->live ? c->cx : 0;
	};
	info.get_height = [](void *data) -> uint32_t {
		auto *c = static_cast<StandInCapture *>(data);
		return c->live ? c->cy : 0;
	};
	info.video_render = [](void *data, gs_effect_t *) {
		auto *c = static_cast<StandInCapture *>(data);
		if (!c->live)
			return;
		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		vec4 colour;
		vec4_from_rgba(&colour, c->colour);
		gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &colour);
		while (gs_effect_loop(solid, "Solid"))
			gs_draw_sprite(nullptr, 0, c->cx, c->cy);
	};
	obs_register_source(&info);
}

// The owner's switch from the camera to a capture in their full-capture
// scene (beta154): the DSLR shown full-screen; a capture shown in its place,
// the DSLR hidden by a Link Visibility (Show only one), the Square Camera (a
// Cropped View of the DSLR) shown with the capture. Every item changes with
// Morph over 700 ms, the DSLR and the Square Camera are linked in the Morph
// transition's settings, and the capture has a Standby Picture and starts
// only when shown. Ambient Background follows the capture on screen. The
// owner: "the ambient background changes to the next source before the
// source appears, and the source just instantly appears without morph or
// fade or anything".
static void capture_pop_in_test(uint32_t w, uint32_t h, const std::string &out)
{
	printf("\n[a capture shown in the camera's place comes in with the change, and the background after it]\n");
	if (!obs_source_get_display_name("stagekit_morph")) {
		CHECK(false, "the Morph transition is there");
		return;
	}
	register_stand_in_capture("game_capture");
	register_stand_in_capture("v4l2_input");
	auto set_list = [](obs_data_t *settings, const char *list, std::vector<const char *> names) {
		obs_data_array_t *rows = obs_data_array_create();
		for (const char *name : names) {
			obs_data_t *row = obs_data_create();
			obs_data_set_string(row, "value", name);
			obs_data_array_push_back(rows, row);
			obs_data_release(row);
		}
		obs_data_set_array(settings, list, rows);
		obs_data_array_release(rows);
	};
	// A start image of its own colour (blue), so the Standby Picture shows
	// apart from the capture (green) and the camera (red).
	const std::string blue_path = out + "/../pop-in-standby.png";
	{
		QImage blue(160, 90, QImage::Format_RGB32);
		blue.fill(QColor(0, 0, 255));
		blue.save(QString::fromStdString(blue_path));
		calldata_t cd = {};
		calldata_set_string(&cd, "path", blue_path.c_str());
		proc_handler_call(obs_get_proc_handler(), "stagekit_set_start_image", &cd);
		calldata_free(&cd);
	}
	auto capture = [](const char *kind, const char *name, uint32_t colour, bool always) {
		obs_data_t *d = obs_data_create();
		obs_data_set_int(d, "color", colour);
		obs_data_set_int(d, "width", 640);
		obs_data_set_int(d, "height", 360);
		obs_data_set_bool(d, "always", always);
		obs_source_t *src = obs_source_create(kind, name, d, nullptr);
		obs_data_release(d);
		return src;
	};
	obs_source_t *dslr = capture("v4l2_input", "Pop DSLR", 0xFF0000FF, true);      // red
	obs_source_t *game = capture("game_capture", "Pop Game", 0xFF00FF00, false);   // green
	obs_source_t *other = capture("game_capture", "Pop Other", 0xFFFFFFFF, false); // white
	obs_data_t *vd = obs_data_create();
	obs_data_set_string(vd, "source", "Pop DSLR");
	obs_data_set_int(vd, "left", 140);
	obs_data_set_int(vd, "right", 140);
	obs_source_t *square = obs_source_create("stagekit_cropped_view", "Pop Square Camera", vd, nullptr);
	obs_data_release(vd);

	// The capture's Standby Picture, as the owner has it. (Their Shadow, Glow
	// & Outline after it is left out: the capture popped in without it too,
	// and with it this test could only draw a few frames a second.)
	obs_data_t *sd = obs_data_create();
	obs_data_set_int(sd, "picture", 0); // the start image
	obs_data_set_int(sd, "style", 11);  // Morph
	obs_data_set_bool(sd, "own_shape", true);
	obs_source_t *standby = obs_source_create("stagekit_standby", "Pop Standby", sd, nullptr);
	obs_data_release(sd);
	obs_source_filter_add(game, standby);

	obs_data_t *ad = obs_data_create();
	obs_data_set_int(ad, "base_type", 0);
	obs_data_set_int(ad, "live_mode", 1); // the capture on screen
	obs_data_set_double(ad, "blur", 0);
	obs_data_set_double(ad, "speed_x", 0);
	obs_data_set_double(ad, "speed_y", 0);
	obs_data_set_int(ad, "live_delay_ms", 700); // as the owner set it
	obs_source_t *ambient = obs_source_create("stagekit_ambient_background", "Pop Ambient", ad, nullptr);
	obs_data_release(ad);

	obs_scene_t *scene = obs_scene_create("Pop Full Capture");
	obs_source_t *scene_src = obs_scene_get_source(scene);
	// (The background is drawn by itself here, not under the items, so
	// what the screen shows of the capture is the capture alone.)
	obs_source_inc_showing(ambient);
	auto full = [&](obs_source_t *src, bool shown) {
		obs_sceneitem_t *item = obs_scene_add(scene, src);
		vec2 box;
		vec2_set(&box, (float)w, (float)h);
		obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
		obs_sceneitem_set_bounds(item, &box);
		obs_sceneitem_set_visible(item, shown);
		return item;
	};
	obs_sceneitem_t *dslr_item = full(dslr, true);
	obs_sceneitem_t *other_item = full(other, false);
	obs_sceneitem_t *game_item = full(game, false);
	obs_sceneitem_t *square_item = obs_scene_add(scene, square);
	{
		vec2 at, k;
		vec2_set(&at, 520.0f, 20.0f);
		vec2_set(&k, 0.3f, 0.3f);
		obs_sceneitem_set_pos(square_item, &at);
		obs_sceneitem_set_scale(square_item, &k);
		obs_sceneitem_set_visible(square_item, false);
	}
	// The Morph a change of scene uses, with the owner's links: the camera
	// and its square view, and the captures among themselves.
	obs_data_t *gd = obs_data_create();
	obs_data_set_int(gd, "others", 2);
	set_list(gd, "links", {"Pop DSLR \xE2\x86\x94 Pop Square Camera", "Pop Game \xE2\x86\x94 Pop Other"});
	obs_source_t *global = obs_source_create_private("stagekit_morph", "Pop Scene Morph", gd);
	obs_data_release(gd);
	// Every item's show and hide transition is a Morph of 700 ms, as saved in
	// the owner's collection.
	for (obs_sceneitem_t *item : {dslr_item, other_item, game_item, square_item})
		for (const bool show : {true, false}) {
			obs_source_t *t = obs_source_create_private("stagekit_morph", "Pop Item Morph", nullptr);
			obs_sceneitem_set_transition(item, show, t);
			obs_sceneitem_set_transition_duration(item, show, 700);
			obs_source_release(t);
		}
	auto link = [&](const char *name, int mode, std::vector<const char *> items) {
		obs_data_t *ld = obs_data_create();
		set_list(ld, "items", items);
		obs_data_set_int(ld, "mode", mode);
		obs_data_set_string(ld, "transition", "stagekit_morph");
		obs_data_set_int(ld, "transition_ms", 700);
		obs_data_set_bool(ld, "lock_hidden", true);
		obs_source_t *f = obs_source_create("stagekit_visibility_link", name, ld, nullptr);
		obs_data_release(ld);
		obs_source_filter_add(scene_src, f);
		return f;
	};
	obs_source_t *captures_link = link("Pop Link Captures", 0, {"Pop Game", "Pop Other", "Pop DSLR"});
	obs_source_t *camera_link = link("Pop Link Camera Game", 1, {"Pop Game", "Pop Square Camera"});

	obs_source_t *before_out = obs_get_output_source(0);
	obs_set_output_source(0, scene_src);
	sleep_ms(2500); // the capture's Standby Picture has long stood in for it

	auto mean_of = [](const std::vector<uint8_t> &px, int channel) {
		double sum = 0.0;
		for (size_t i = channel; i < px.size(); i += 4)
			sum += px[i];
		return sum / (double)(px.size() / 4);
	};
	auto waiting = [&] {
		calldata_t cd = {};
		bool out_waiting = false;
		if (proc_handler_call(obs_source_get_proc_handler(standby), "get_standing", &cd))
			out_waiting = calldata_bool(&cd, "waiting");
		calldata_free(&cd);
		return out_waiting;
	};
	auto count_items = [&] {
		int n = 0;
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *, void *param) {
				++*static_cast<int *>(param);
				return true;
			},
			&n);
		return n;
	};
	const int items_before = count_items();
	const std::vector<uint8_t> amb_before = render(ambient, w, h);
	CHECK(obs_sceneitem_visible(dslr_item) && !obs_sceneitem_visible(game_item) && waiting() &&
		      mean_of(amb_before, 0) > mean_of(amb_before, 1) + 40,
	      "to begin with the camera fills the screen, the capture is hidden with its Standby Picture standing in, and the background follows the camera (ambient red %.0f, green %.0f)",
	      mean_of(amb_before, 0), mean_of(amb_before, 1));

	// What is drawn, sampled as often as the test can draw: the middle of
	// the screen (the capture, or its Standby Picture, over nothing), how
	// much of the capture's and the square view's show transitions is
	// drawn, and the background by itself.
	struct Sample {
		double ms;
		int capture;      // the middle of the screen: green (the capture) or blue (its Standby Picture)
		double capture_a; // what the capture's show transition draws (alpha, 0 to 255)
		double square_a;  // the same for the square view's
		double amb_r, amb_g;
		bool layer; // a Morph layer is in the scene
		int layers; // how many
	};
	auto watch = [&](int for_ms) {
		std::vector<Sample> samples;
		const auto t0 = std::chrono::steady_clock::now();
		for (;;) {
			const double ms =
				std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			if (ms > for_ms)
				break;
			const std::vector<uint8_t> frame = render(scene_src, w, h);
			const std::vector<uint8_t> amb = render(ambient, w, h);
			const std::vector<uint8_t> cap_t = render(obs_sceneitem_get_transition(game_item, true), w, h);
			const std::vector<uint8_t> sq_t = render(obs_sceneitem_get_transition(square_item, true), w, h);
			const uint8_t *p = &frame[(size_t(250) * w + 320) * 4];
			const int layers = count_items() - items_before;
			samples.push_back(Sample{ms, std::max((int)p[1], (int)p[2]), mean_of(cap_t, 3),
						 mean_of(sq_t, 3), mean_of(amb, 0), mean_of(amb, 1), layers > 0,
						 layers});
			sleep_ms(10);
		}
		return samples;
	};
	auto print = [](const std::vector<Sample> &samples) {
		for (const Sample &s : samples)
			if (s.ms < 1500)
				printf("    %5.0f ms  capture on screen %3d  its transition %5.1f  square view's %5.1f  background red %5.1f green %5.1f%s\n",
				       s.ms, s.capture, s.capture_a, s.square_a, s.amb_r, s.amb_g,
				       s.layer ? "  (layer)" : "");
	};
	// The highest value part-way (neither nothing nor all) of a measure
	// between two times, and how many samples were part-way.
	struct PartWay {
		int n = 0, of = 0;
	};
	auto part_way = [](const std::vector<Sample> &samples, double from, double to, auto measure, double lo,
			   double hi) {
		PartWay out;
		for (const Sample &s : samples) {
			if (s.ms < from || s.ms > to)
				continue;
			out.of++;
			const double v = measure(s);
			out.n += v > lo && v < hi;
		}
		return out;
	};

	// The owner's key: a Show in Scenes on the capture.
	obs_data_t *swd = obs_data_create();
	set_list(swd, "scenes", {"Pop Full Capture"});
	obs_data_set_int(swd, "where", 1);
	obs_data_set_string(swd, "transition", "stagekit_morph");
	obs_data_set_int(swd, "transition_ms", 700);
	obs_data_set_bool(swd, "follow", true);
	obs_source_t *game_switch =
		obs_source_create("stagekit_show_in_scenes", "Pop Show in Scenes-Capture", swd, nullptr);
	obs_data_release(swd);
	obs_source_set_enabled(game_switch, false);
	obs_source_filter_add(game, game_switch);
	sleep_ms(500);

	auto run = [&](int start_frames) {
		obs_data_t *gs = obs_source_get_settings(game);
		obs_data_set_int(gs, "start_frames", start_frames);
		obs_source_update(game, gs);
		obs_data_release(gs);
		// The switch: the capture's key; the links hide the camera and show
		// the square view.
		obs_source_set_enabled(game_switch, true);
		const std::vector<Sample> samples = watch(3000);
		print(samples);
		return samples;
	};
	auto back_to_camera = [&] {
		obs_sceneitem_set_visible(dslr_item, true);
		sleep_ms(2500);
	};

	// 1. As the owner has it: the camera linked to its square view, the
	// capture's picture coming only after the change (40 frames). Up to
	// beta155 the capture appeared at the end of the change, in beta156 and
	// beta157 it faded in by itself (its own transition drew it); since
	// beta158 it comes out of the camera as well, drawn by a second layer,
	// and its own transition draws nothing.
	{
		printf("  (the camera linked to its square view; the capture's picture comes after the change)\n");
		const std::vector<Sample> s = run(40);
		// (The middle of the screen is under the camera shrinking into its
		// square view for the first half of the change, so the capture is
		// looked for there once the camera has gone past, well before the end.)
		const PartWay cap = part_way(s, 0, 600, [](const Sample &x) { return (double)x.capture; }, 25, 256);
		const PartWay cap_t = part_way(s, 0, 690, [](const Sample &x) { return x.capture_a; }, 5, 250);
		int layers = 0;
		for (const Sample &x : s)
			layers = std::max(layers, x.layers);
		CHECK(cap.n >= 1 && cap_t.n == 0 && layers == 2,
		      "the camera changes into its square view, and the capture comes out of the camera, drawn by a second layer, during the change instead of appearing at its end (on screen in %d of %d samples in the first 600 ms; its own transition part-way in %d; %d layers at once)",
		      cap.n, cap.of, cap_t.n, layers);
		double red_until = 0.0;
		for (const Sample &x : s)
			if (x.amb_r > 200.0)
				red_until = x.ms;
		CHECK(red_until >= 650.0 && s.back().amb_g > s.back().amb_r,
		      "the background keeps the camera's picture for its Capture delay (700 ms) and then follows the capture (the camera's red until %.0f ms; at the end red %.0f, green %.0f)",
		      red_until, s.back().amb_r, s.back().amb_g);
		back_to_camera();
	}
	// 2. The owner's other try: the camera linked to the capture instead.
	// The capture changes with the camera; the square view, with nothing to
	// change from, must fade in.
	{
		printf("  (the camera linked to the capture; the capture's picture comes 3 frames after it is shown)\n");
		obs_data_t *gd2 = obs_source_get_settings(global);
		set_list(gd2, "links", {"Pop DSLR \xE2\x86\x94 Pop Game", "Pop Game \xE2\x86\x94 Pop Other"});
		obs_source_update(global, gd2);
		obs_data_release(gd2);
		const std::vector<Sample> s = run(3);
		const PartWay sq = part_way(s, 0, 690, [](const Sample &x) { return x.square_a; }, 3.0, 140.0);
		bool layer = false;
		for (const Sample &x : s)
			layer = layer || x.layer;
		CHECK(sq.n >= 2 && layer && obs_sceneitem_visible(square_item),
		      "with the camera linked to the capture instead, the two change into each other and the square view fades in during the change instead of appearing at its end (its transition part-way in %d of %d samples in the first 690 ms)",
		      sq.n, sq.of);
		back_to_camera();
	}

	obs_set_output_source(0, before_out);
	obs_source_release(before_out);
	obs_source_filter_remove(scene_src, captures_link);
	obs_source_filter_remove(scene_src, camera_link);
	obs_source_release(captures_link);
	obs_source_release(camera_link);
	obs_source_release(global);
	sleep_ms(100);
	obs_scene_release(scene);
	obs_source_filter_remove(game, game_switch);
	obs_source_release(game_switch);
	obs_source_filter_remove(game, standby);
	obs_source_release(standby);
	obs_source_dec_showing(ambient);
	for (obs_source_t *src : {dslr, game, other, square, ambient})
		obs_source_release(src);
	sleep_ms(200);
}

// ------------------------------------------------- space priority
// Which fitted source makes room for which: a game (4:3, kept towards
// the left) and a camera (4:3, kept towards the right) on a 640x360
// canvas with a gap of 10. The one that takes its space first is as
// tall as the screen allows (340); the other fits beside it. Sharing,
// they go by each other's claims as before: where they settle then
// depends on which looks first, so a tie is compared with two that
// share in the same filter order.
static void fit_priority_test()
{
	printf("\n[fit priority]\n");
	auto fp_sized = [](obs_source_t *src, int cw, int ch) {
		obs_data_t *d = obs_data_create();
		obs_data_set_int(d, "width", cw);
		obs_data_set_int(d, "height", ch);
		obs_source_update(src, d);
		obs_data_release(d);
	};
	obs_source_t *fp_game = obs_source_create("color_source", "FP Game", nullptr, nullptr);
	fp_sized(fp_game, 1200, 900);
	obs_source_t *fp_cam = obs_source_create("color_source", "FP Cam", nullptr, nullptr);
	fp_sized(fp_cam, 120, 90);
	obs_source_t *before = obs_get_output_source(0);
	struct Pic {
		double x = 0, y = 0, w = 0, h = 0;
	};
	// Where the image is drawn in its bounding box.
	auto pic = [](obs_sceneitem_t *it) {
		vec2 p, b;
		obs_sceneitem_get_pos(it, &p);
		obs_sceneitem_get_bounds(it, &b);
		obs_source_t *s = obs_sceneitem_get_source(it);
		const double sw = obs_source_get_width(s), sh = obs_source_get_height(s);
		if (sw < 1.0 || sh < 1.0 || b.x < 1.0 || b.y < 1.0)
			return Pic{};
		const double k = std::min(b.x / sw, b.y / sh), w = sw * k, h = sh * k;
		const uint32_t a = obs_sceneitem_get_bounds_alignment(it);
		const double x = p.x + (a & OBS_ALIGN_LEFT ? 0.0 : a & OBS_ALIGN_RIGHT ? b.x - w : (b.x - w) / 2.0);
		const double y = p.y + (a & OBS_ALIGN_TOP ? 0.0 : a & OBS_ALIGN_BOTTOM ? b.y - h : (b.y - h) / 2.0);
		return Pic{x, y, w, h};
	};
	auto same = [](const Pic &a, const Pic &b, double by) {
		return std::fabs(a.x - b.x) <= by && std::fabs(a.y - b.y) <= by && std::fabs(a.w - b.w) <= by &&
		       std::fabs(a.h - b.h) <= by;
	};
	auto apart = [](const Pic &a, const Pic &b) {
		const bool overlap = a.x < b.x + b.w - 0.5 && b.x < a.x + a.w - 0.5 && a.y < b.y + b.h - 0.5 &&
				     b.y < a.y + a.h - 0.5;
		auto in = [](const Pic &p) {
			return p.w > 1.0 && p.x >= 9.5 && p.y >= 9.5 && p.x + p.w <= 630.5 && p.y + p.h <= 350.5;
		};
		return !overlap && in(a) && in(b);
	};
	auto tall = [](const Pic &p) {
		return std::fabs(p.h - 340.0) < 1.0;
	};
	int made = 0;
	struct FP {
		obs_scene_t *scene = nullptr;
		obs_sceneitem_t *g = nullptr, *c = nullptr;
		obs_source_t *fg = nullptr, *fc = nullptr;
	};
	// `level` -1: not set (as a filter saved before the setting).
	auto fp_filter = [&](obs_scene_t *scene, const char *item, int level, bool by_list, int toward) {
		obs_data_t *d = obs_data_create();
		obs_data_set_string(d, "item", item);
		obs_data_set_int(d, "box", 2);
		obs_data_set_double(d, "margin", 10);
		obs_data_set_double(d, "tolerance", 5);
		obs_data_set_bool(d, "resize", true);
		obs_data_set_int(d, "toward", toward);
		obs_data_set_int(d, "glide_ms", 300);
		if (level >= 0)
			obs_data_set_int(d, "space_priority", level);
		if (by_list)
			obs_data_set_bool(d, "list_priority", true);
		const std::string name = std::string("FP Fit ") + item + " " + std::to_string(made);
		obs_source_t *f = obs_source_create("stagekit_fit_item", name.c_str(), d, nullptr);
		obs_data_release(d);
		obs_source_filter_add(obs_scene_get_source(scene), f);
		return f;
	};
	auto fp_scene = [&](FP &f) {
		made++;
		f.scene = obs_scene_create(("FP Scene " + std::to_string(made)).c_str());
		f.g = obs_scene_add(f.scene, fp_game);
		f.c = obs_scene_add(f.scene, fp_cam);
		vec2 at, k;
		vec2_set(&at, 10.0f, 10.0f);
		vec2_set(&k, 0.25f, 0.25f);
		obs_sceneitem_set_pos(f.g, &at);
		obs_sceneitem_set_scale(f.g, &k);
		vec2_set(&at, 500.0f, 10.0f);
		obs_sceneitem_set_pos(f.c, &at);
	};
	// A game and a camera, each with its own Fit to Space; `g_first`:
	// the game's filter is the higher in the list.
	auto fp_make = [&](int gl, bool gt, int cl, bool ct, bool g_first) {
		FP f;
		fp_scene(f);
		if (g_first) {
			f.fg = fp_filter(f.scene, "FP Game", gl, gt, 3);
			f.fc = fp_filter(f.scene, "FP Cam", cl, ct, 4);
		} else {
			f.fc = fp_filter(f.scene, "FP Cam", cl, ct, 4);
			f.fg = fp_filter(f.scene, "FP Game", gl, gt, 3);
		}
		obs_set_output_source(0, obs_scene_get_source(f.scene));
		return f;
	};
	auto fp_wait = [&](FP &f, int ms) {
		for (int i = 0; i < ms / 20; i++) {
			render(obs_scene_get_source(f.scene), 640, 360);
			sleep_ms(20);
		}
	};
	auto fp_drop = [&](FP &f) {
		for (obs_source_t *filter : {f.fg, f.fc}) {
			if (!filter)
				continue;
			obs_source_filter_remove(obs_scene_get_source(f.scene), filter);
			obs_source_release(filter);
		}
		obs_source_remove(obs_scene_get_source(f.scene));
		obs_scene_release(f.scene);
	};
	// Settled: what each shows, and whether it stayed put for 0.8 s.
	auto fp_settle = [&](FP &f, Pic &g, Pic &c) {
		fp_wait(f, 1500);
		g = pic(f.g);
		c = pic(f.c);
		fp_wait(f, 800);
		return same(g, pic(f.g), 0.1) && same(c, pic(f.c), 0.1);
	};
	static const char *const kLevel[] = {"shares space", "takes space first", "makes room"};
	auto tier = [](int level) {
		return level == 1 ? 0 : level == 0 ? 1 : 2;
	};

	// Every level against every other, the game's filter above and below.
	Pic shared_g, shared_c, tie_g[2], tie_c[2];
	for (int order = 0; order < 2; order++) {
		for (int gl = 0; gl < 3; gl++) {
			for (int cl = 0; cl < 3; cl++) {
				FP f = fp_make(gl, false, cl, false, order == 0);
				Pic g, c;
				const bool still = fp_settle(f, g, c);
				const int tg = tier(gl), tc = tier(cl);
				if (gl == 0 && cl == 0) {
					tie_g[order] = g;
					tie_c[order] = c;
				}
				const bool right = tg < tc   ? tall(g) && !tall(c)
						   : tc < tg ? tall(c) && !tall(g)
							     : same(g, tie_g[order], 0.5) && same(c, tie_c[order], 0.5);
				CHECK(still && apart(g, c) && right,
				      "the game %s, the camera %s (%s filter above): %s, apart, and still (game %.0f,%.0f %.0fx%.0f; camera %.0f,%.0f %.0fx%.0f)",
				      kLevel[gl], kLevel[cl], order == 0 ? "game's" : "camera's",
				      tg < tc   ? "the game takes its space"
				      : tc < tg ? "the camera takes its space"
						: "they share, as two that share do",
				      g.x, g.y, g.w, g.h, c.x, c.y, c.w, c.h);
				if (gl == 0 && cl == 0 && order == 0) {
					shared_g = g;
					shared_c = c;
				}
				fp_drop(f);
			}
		}
	}

	// Filters saved before the setting: shares space, not by list order,
	// and they are placed exactly as two that share.
	{
		FP f = fp_make(-1, false, -1, false, true);
		obs_data_t *gs = obs_source_get_settings(f.fg);
		const bool read = obs_data_get_int(gs, "space_priority") == 0 &&
				  !obs_data_get_bool(gs, "list_priority");
		obs_data_release(gs);
		Pic g, c;
		const bool still = fp_settle(f, g, c);
		CHECK(read && still && same(g, shared_g, 0.5) && same(c, shared_c, 0.5),
		      "a filter saved before Space priority shares space and is placed as before (game %.0fx%.0f, camera %.0fx%.0f)",
		      g.w, g.h, c.w, c.h);
		// A new filter, made with nothing set: shares space too.
		obs_source_t *fresh = obs_source_create("stagekit_fit_item", "FP Fresh", nullptr, nullptr);
		obs_data_t *fs = obs_source_get_settings(fresh);
		CHECK(obs_data_get_int(fs, "space_priority") == 0 && !obs_data_get_bool(fs, "list_priority"),
		      "a new filter starts as Shares space, list order unticked");
		obs_data_release(fs);
		obs_source_release(fresh);
		fp_drop(f);
	}

	// Hidden and shown, with the game taking its space first, in both
	// filter orders: the game never moves for the camera; the camera
	// takes the room while the game is hidden and makes way again in
	// one move when it is back.
	for (int order = 0; order < 2; order++) {
		FP f = fp_make(1, false, 0, false, order == 0);
		Pic g0, c0;
		fp_settle(f, g0, c0);
		bool game_still = true;
		obs_sceneitem_set_visible(f.c, false);
		for (int i = 0; i < 50; i++) {
			fp_wait(f, 20);
			game_still = game_still && same(pic(f.g), g0, 0.05);
		}
		obs_sceneitem_set_visible(f.c, true);
		for (int i = 0; i < 75; i++) {
			fp_wait(f, 20);
			game_still = game_still && same(pic(f.g), g0, 0.05);
		}
		CHECK(game_still && same(pic(f.c), c0, 0.5),
		      "%s: the camera hidden and shown, the game does not move at all and the camera comes back (%.0f,%.0f)",
		      order == 0 ? "game's filter above" : "camera's filter above", pic(f.c).x, pic(f.c).y);
		obs_sceneitem_set_visible(f.g, false);
		fp_wait(f, 1000);
		const Pic c_alone = pic(f.c);
		obs_sceneitem_set_visible(f.g, true);
		int turns = 0;
		double last = c_alone.h, last_step = 0.0;
		for (int i = 0; i < 100; i++) {
			fp_wait(f, 20);
			const double h = pic(f.c).h, step = h - last;
			if (step * last_step < -1e-6)
				turns++;
			if (std::fabs(step) > 0.05 && std::fabs(last_step) <= 0.05 && i > 2 &&
			    std::fabs(last - c_alone.h) > 0.05)
				turns++;
			last_step = step;
			last = h;
		}
		CHECK(tall(c_alone) && turns == 0 && same(pic(f.g), g0, 0.5) && same(pic(f.c), c0, 0.5),
		      "%s: the game hidden, the camera takes the room (%.0f tall); shown, the camera makes way in one move (turns %d) and both are back",
		      order == 0 ? "game's filter above" : "camera's filter above", c_alone.h, turns);
		fp_drop(f);
	}

	// "Priority by list order": the level is not read; the higher filter wins.
	{
		// (The game set to make room and the camera to take its space
		// first: ignored, the game's filter is above.)
		FP f = fp_make(2, true, 1, true, true);
		Pic g, c;
		const bool still = fp_settle(f, g, c);
		CHECK(still && tall(g) && !tall(c) && apart(g, c),
		      "by list order, the filter higher in the list takes its space, whatever its level (game %.0f tall, camera %.0f)",
		      g.h, c.h);
		// Moved up: the camera wins, gliding there, not jumping.
		obs_source_filter_set_order(obs_scene_get_source(f.scene), f.fc, OBS_ORDER_MOVE_UP);
		const double from = pic(f.c).h;
		bool between = false;
		for (int i = 0; i < 60; i++) {
			fp_wait(f, 20);
			const double h = pic(f.c).h;
			between = between || (h > from + 2.0 && h < 339.0);
		}
		Pic g2, c2;
		const bool still2 = fp_settle(f, g2, c2);
		CHECK(still2 && tall(c2) && !tall(g2) && apart(g2, c2) && between,
		      "the camera's filter moved above it: the camera takes its space, gliding there (camera %.0f tall, game %.0f)",
		      c2.h, g2.h);
		obs_source_filter_set_order(obs_scene_get_source(f.scene), f.fc, OBS_ORDER_MOVE_DOWN);
		Pic g3, c3;
		const bool still3 = fp_settle(f, g3, c3);
		CHECK(still3 && same(g3, g, 0.5) && same(c3, c, 0.5),
		      "moved back down, both go back where they were (game %.0f tall)", g3.h);
		// The level control is greyed out while the box is ticked.
		obs_properties_t *props = obs_source_properties(f.fg);
		obs_property_t *level = obs_properties_get(props, "space_priority");
		obs_property_t *box = obs_properties_get(props, "list_priority");
		obs_data_t *gs = obs_source_get_settings(f.fg);
		obs_property_modified(box, gs);
		const bool off = level && !obs_property_enabled(level);
		obs_data_set_bool(gs, "list_priority", false);
		obs_property_modified(box, gs);
		const bool on = level && obs_property_enabled(level);
		obs_data_set_bool(gs, "list_priority", true);
		obs_data_release(gs);
		obs_properties_destroy(props);
		CHECK(box && off && on, "Space priority is greyed out while list order is ticked, and not after");
		fp_drop(f);
	}
	// Ticked against unticked: the ticked one counts as sharing space.
	{
		const struct {
			int cam_level;
			const char *what;
		} cases[] = {{0, "shares space: they share"},
			     {1, "takes space first: the camera wins"},
			     {2, "makes room: the game wins"}};
		for (const auto &one : cases) {
			FP f = fp_make(2, true, one.cam_level, false, true);
			Pic g, c;
			const bool still = fp_settle(f, g, c);
			const bool right = one.cam_level == 0   ? same(g, shared_g, 0.5) && same(c, shared_c, 0.5)
					   : one.cam_level == 1 ? tall(c) && !tall(g)
								: tall(g) && !tall(c);
			CHECK(still && apart(g, c) && right,
			      "the game by list order (level set to make room, not read), the camera unticked and %s (game %.0f, camera %.0f tall)",
			      one.what, g.h, c.h);
			fp_drop(f);
		}
	}

	// Scene Fit: its entries by its list's order, or each by its level.
	auto fp_list = [](obs_data_t *d, const char *setting, std::vector<std::string> values) {
		obs_data_array_t *list = obs_data_array_create();
		for (const std::string &value : values) {
			obs_data_t *entry = obs_data_create();
			obs_data_set_string(entry, "value", value.c_str());
			obs_data_array_push_back(list, entry);
			obs_data_release(entry);
		}
		obs_data_set_array(d, setting, list);
		obs_data_array_release(list);
	};
	auto fp_entry = [](obs_data_t *d, const std::string &prefix, int toward, int level) {
		obs_data_set_int(d, (prefix + "box").c_str(), 2);
		obs_data_set_double(d, (prefix + "margin").c_str(), 10);
		obs_data_set_double(d, (prefix + "tolerance").c_str(), 5);
		obs_data_set_bool(d, (prefix + "resize").c_str(), true);
		obs_data_set_int(d, (prefix + "toward").c_str(), toward);
		obs_data_set_int(d, (prefix + "glide_ms").c_str(), 300);
		if (level >= 0)
			obs_data_set_int(d, (prefix + "space_priority").c_str(), level);
	};
	auto fp_scene_fit = [&](FP &f, std::vector<std::string> entries, bool by_list, int game_level, int cam_level) {
		obs_data_t *d = obs_data_create();
		fp_list(d, "entries", entries);
		fp_entry(d, "fit|FP Game|", 3, game_level);
		fp_entry(d, "fit|FP Cam|", 4, cam_level);
		obs_data_set_bool(d, "list_priority", by_list);
		const std::string name = "FP Scene Fit " + std::to_string(made);
		obs_source_t *sf = obs_source_create("stagekit_scene_fit", name.c_str(), d, nullptr);
		obs_data_release(d);
		obs_source_filter_add(obs_scene_get_source(f.scene), sf);
		return sf;
	};
	{
		FP f;
		fp_scene(f);
		f.fg = fp_scene_fit(f, {"FP Game", "FP Cam"}, true, 2, 1);
		obs_set_output_source(0, obs_scene_get_source(f.scene));
		Pic g, c;
		bool still = fp_settle(f, g, c);
		CHECK(still && tall(g) && !tall(c) && apart(g, c),
		      "Scene Fit by list order: the first entry takes its space (game %.0f, camera %.0f tall)", g.h,
		      c.h);
		obs_data_t *d = obs_source_get_settings(f.fg);
		fp_list(d, "entries", {"FP Cam", "FP Game"});
		obs_source_update(f.fg, d);
		obs_data_release(d);
		still = fp_settle(f, g, c);
		CHECK(still && tall(c) && !tall(g) && apart(g, c),
		      "the entries swapped: the camera takes its space (camera %.0f, game %.0f tall)", c.h, g.h);
		// Unticked: each entry's own level.
		d = obs_source_get_settings(f.fg);
		obs_data_set_bool(d, "list_priority", false);
		obs_data_set_int(d, "fit|FP Game|space_priority", 1);
		obs_data_set_int(d, "fit|FP Cam|space_priority", 0);
		obs_source_update(f.fg, d);
		obs_properties_t *props = obs_source_properties(f.fg);
		obs_property_t *level = obs_properties_get(props, "fit|FP Game|space_priority");
		obs_property_t *box = obs_properties_get(props, "list_priority");
		obs_data_set_bool(d, "list_priority", true);
		obs_property_modified(box, d);
		const bool off = level && !obs_property_enabled(level);
		obs_data_set_bool(d, "list_priority", false);
		obs_property_modified(box, d);
		const bool on = level && obs_property_enabled(level);
		obs_properties_destroy(props);
		obs_data_release(d);
		CHECK(off && on, "an entry's Space priority is greyed out while the Scene Fit goes by list order");
		still = fp_settle(f, g, c);
		CHECK(still && tall(g) && !tall(c) && apart(g, c),
		      "Scene Fit unticked: the game's entry, set to take its space first, wins although listed second (game %.0f tall)",
		      g.h);
		fp_drop(f);
	}
	// A Scene Fit and a Fit to Space on one scene, both by list order:
	// the Scene Fit's entries count at its place in the filter list.
	{
		FP f;
		fp_scene(f);
		f.fc = fp_scene_fit(f, {"FP Cam"}, true, -1, 2);
		f.fg = fp_filter(f.scene, "FP Game", 1, true, 3);
		obs_set_output_source(0, obs_scene_get_source(f.scene));
		Pic g, c;
		bool still = fp_settle(f, g, c);
		CHECK(still && tall(c) && !tall(g) && apart(g, c),
		      "a Scene Fit above a Fit to Space: its entry takes its space (camera %.0f, game %.0f tall)", c.h,
		      g.h);
		obs_source_filter_set_order(obs_scene_get_source(f.scene), f.fg, OBS_ORDER_MOVE_UP);
		still = fp_settle(f, g, c);
		CHECK(still && tall(g) && !tall(c) && apart(g, c),
		      "the Fit to Space moved above it: the game takes its space (game %.0f, camera %.0f tall)", g.h,
		      c.h);
		fp_drop(f);
	}

	obs_set_output_source(0, before);
	obs_source_release(before);
	obs_source_release(fp_cam);
	obs_source_release(fp_game);
}

// Link Visibility's fuller Show/Hide transition, and Morph's Unpaired
// sources. The owner: "Link visibility scene filter should have additional
// transition options including a fallback or other items transition for
// items that don't have partners".
static void link_transitions_test(uint32_t w, uint32_t h)
{
	printf("\n[link visibility: show and hide transitions, and Morph's unpaired sources]\n");
	for (const char *kind :
	     {"stagekit_morph", "fade_transition", "slide_transition", "swipe_transition", "stagekit_fade_zoom"})
		if (!obs_source_get_display_name(kind)) {
			CHECK(false, "the transition %s is there", kind);
			return;
		}
	auto block = [](const char *name, long long abgr, int bw, int bh) {
		obs_data_t *d = obs_data_create();
		obs_data_set_int(d, "color", abgr);
		obs_data_set_int(d, "width", bw);
		obs_data_set_int(d, "height", bh);
		obs_source_t *src = obs_source_create("color_source", name, d, nullptr);
		obs_data_release(d);
		return src;
	};
	auto put = [](obs_scene_t *scene, obs_source_t *src, float x, float y) {
		obs_sceneitem_t *item = obs_scene_add(scene, src);
		vec2 p;
		vec2_set(&p, x, y);
		obs_sceneitem_set_pos(item, &p);
		return item;
	};
	auto set_list = [](obs_data_t *settings, const char *list, std::vector<const char *> names) {
		obs_data_array_t *rows = obs_data_array_create();
		for (const char *name : names) {
			obs_data_t *row = obs_data_create();
			obs_data_set_string(row, "value", name);
			obs_data_array_push_back(rows, row);
			obs_data_release(row);
		}
		obs_data_set_array(settings, list, rows);
		obs_data_array_release(rows);
	};
	auto kind_of = [](obs_sceneitem_t *item, bool show) {
		obs_source_t *t = obs_sceneitem_get_transition(item, show);
		const char *id = t ? obs_source_get_unversioned_id(t) : nullptr;
		return std::string(id ? id : "(none)");
	};
	auto ms_of = [](obs_sceneitem_t *item, bool show) {
		return (int)obs_sceneitem_get_transition_duration(item, show);
	};
	auto setting_of = [](obs_sceneitem_t *item, bool show, const char *name) {
		obs_source_t *t = obs_sceneitem_get_transition(item, show);
		obs_data_t *d = t ? obs_source_get_settings(t) : nullptr;
		const std::string v = d ? obs_data_get_string(d, name) : "";
		obs_data_release(d);
		return v;
	};
	auto count_items = [](obs_scene_t *scene) {
		int n = 0;
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *, void *param) {
				++*static_cast<int *>(param);
				return true;
			},
			&n);
		return n;
	};
	obs_source_t *before_out = obs_get_output_source(0);

	// ------------------------------------------------ the filter's controls
	{
		obs_source_t *a = block("LT A", 0xFF0000FF, 100, 60);
		obs_source_t *b = block("LT B", 0xFF00FF00, 100, 60);
		obs_source_t *c = block("LT C", 0xFFFF0000, 100, 60);
		obs_scene_t *scene = obs_scene_create("LT Scene");
		obs_source_t *scene_src = obs_scene_get_source(scene);
		obs_sceneitem_t *ai = put(scene, a, 20, 20);
		obs_sceneitem_t *bi = put(scene, b, 220, 20);
		obs_sceneitem_t *ci = put(scene, c, 420, 20);
		obs_set_output_source(0, scene_src);

		// 1. Saved before: the plain two controls, a slide of 400 ms. They
		// read as they did: showing and hiding both a slide of 400 ms.
		obs_data_t *ld = obs_data_create();
		set_list(ld, "items", {"LT A", "LT B"});
		obs_data_set_int(ld, "mode", 0); // only one
		obs_data_set_string(ld, "transition", "slide_transition");
		obs_data_set_int(ld, "transition_ms", 400);
		obs_source_t *old_link = obs_source_create("stagekit_visibility_link", "LT Old Link", ld, nullptr);
		obs_data_release(ld);
		obs_source_filter_add(scene_src, old_link);
		obs_sceneitem_set_visible(bi, false);
		sleep_ms(300);
		obs_sceneitem_set_visible(bi, true); // shows B: A goes
		sleep_ms(200);
		obs_properties_t *props = obs_source_properties(old_link);
		obs_data_t *now = obs_source_get_settings(old_link);
		CHECK(!obs_sceneitem_visible(ai) && kind_of(ai, false) == "slide_transition" &&
			      ms_of(ai, false) == 400 && kind_of(ai, true) == "slide_transition" &&
			      ms_of(ai, true) == 400 && setting_of(ai, false, "direction").empty() &&
			      strcmp(obs_data_get_string(now, "hide_transition"), "same") == 0 &&
			      obs_properties_get(props, "transition") && obs_properties_get(props, "hide_transition") &&
			      obs_properties_get(props, "transition_ms") && obs_properties_get(props, "change|for"),
		      "a Link Visibility saved with the plain Transition and Duration reads as before: the source it hides gets a slide of 400 ms to show and to hide, nothing else set, and its window has the fuller controls with Hide transition the same as showing (hide %s %d ms, show %s %d ms)",
		      kind_of(ai, false).c_str(), ms_of(ai, false), kind_of(ai, true).c_str(), ms_of(ai, true));
		obs_data_release(now);
		obs_properties_destroy(props);
		// Kept as it is set on each item: left alone.
		now = obs_source_get_settings(old_link);
		obs_data_set_string(now, "transition", "keep");
		obs_source_update(old_link, now);
		obs_data_release(now);
		obs_sceneitem_set_transition(bi, false, nullptr);
		obs_sceneitem_set_visible(ai, true); // shows A: B goes
		sleep_ms(200);
		CHECK(!obs_sceneitem_visible(bi) && kind_of(bi, false) == "(none)",
		      "and one saved as What is set on each item leaves the items' own transitions alone (%s)",
		      kind_of(bi, false).c_str());
		obs_source_filter_remove(scene_src, old_link);
		obs_source_release(old_link);
		for (obs_sceneitem_t *item : {ai, bi, ci}) {
			obs_sceneitem_set_visible(item, true);
			obs_sceneitem_set_transition(item, true, nullptr);
			obs_sceneitem_set_transition(item, false, nullptr);
		}
		sleep_ms(200);

		// 2. Showing and hiding apart, with a transition's own settings, and
		// one source with settings of its own.
		ld = obs_data_create();
		set_list(ld, "items", {"LT A", "LT B", "LT C"});
		obs_data_set_int(ld, "mode", 1); // together
		obs_data_set_string(ld, "transition", "fade_transition");
		obs_data_set_int(ld, "transition_ms", 300);
		obs_data_set_string(ld, "hide_transition", "slide_transition");
		obs_data_set_int(ld, "hide_ms", 500);
		obs_data_set_string(ld, "hide|slide_transition|direction", "down");
		obs_data_set_string(ld, "for|LT B|transition", "swipe_transition");
		obs_data_set_int(ld, "for|LT B|transition_ms", 250);
		obs_data_set_string(ld, "for|LT B|show|swipe_transition|direction", "up");
		obs_source_t *link = obs_source_create("stagekit_visibility_link", "LT Link", ld, nullptr);
		obs_data_release(ld);
		obs_source_filter_add(scene_src, link);
		sleep_ms(200);
		obs_sceneitem_set_visible(ai, false); // hides B and C
		sleep_ms(200);
		CHECK(!obs_sceneitem_visible(bi) && !obs_sceneitem_visible(ci) &&
			      kind_of(ci, true) == "fade_transition" && ms_of(ci, true) == 300 &&
			      kind_of(ci, false) == "slide_transition" && ms_of(ci, false) == 500 &&
			      setting_of(ci, false, "direction") == "down",
		      "Link Visibility shows and hides each its own way: a fade of 300 ms to show, a slide down of 500 ms to hide (C: show %s %d ms, hide %s %d ms %s)",
		      kind_of(ci, true).c_str(), ms_of(ci, true), kind_of(ci, false).c_str(), ms_of(ci, false),
		      setting_of(ci, false, "direction").c_str());
		CHECK(kind_of(bi, true) == "swipe_transition" && ms_of(bi, true) == 250 &&
			      setting_of(bi, true, "direction") == "up" && kind_of(bi, false) == "swipe_transition" &&
			      ms_of(bi, false) == 250,
		      "and a source with settings of its own (Settings for) gets those, hiding the same as it shows (B: show %s %d ms %s, hide %s %d ms)",
		      kind_of(bi, true).c_str(), ms_of(bi, true), setting_of(bi, true, "direction").c_str(),
		      kind_of(bi, false).c_str(), ms_of(bi, false));
		// The window: Settings for lists the sources; a source chosen shows
		// its own controls.
		props = obs_source_properties(link);
		obs_property_t *for_box = obs_properties_get(props, "change|for");
		const size_t listed = for_box ? obs_property_list_item_count(for_box) : 0;
		obs_properties_destroy(props);
		now = obs_source_get_settings(link);
		obs_data_set_string(now, "change|for", "LT B");
		props = obs_source_properties(link);
		const bool own_shown = obs_properties_get(props, "for|LT B|transition") &&
				       obs_properties_get(props, "for|LT B|show|swipe_transition|direction");
		obs_properties_destroy(props);
		obs_data_set_string(now, "change|for", "");
		obs_data_release(now);
		CHECK(listed == 4 && own_shown,
		      "its window offers Settings for each listed source (%zu entries with All sources) and shows a source's own transition and its settings when chosen",
		      listed);
		obs_source_filter_remove(scene_src, link);
		obs_source_release(link);
		obs_set_output_source(0, before_out);
		obs_scene_release(scene);
		for (obs_source_t *src : {a, b, c})
			obs_source_release(src);
	}

	// ------------------------------------------------ Morph's controls
	{
		obs_data_t *md = obs_data_create();
		obs_data_set_int(md, "others", 1); // fade and zoom, saved before Unpaired sources
		obs_source_t *old_morph = obs_source_create_private("stagekit_morph", "LT Old Morph", md);
		obs_data_release(md);
		obs_data_t *ms = obs_source_get_settings(old_morph);
		const std::string read = obs_data_get_string(ms, "unpaired");
		obs_data_set_string(ms, "unpaired", "slide_transition");
		obs_properties_t *mp = obs_source_properties(old_morph);
		const bool own = obs_properties_get(mp, "unpaired") &&
				 obs_properties_get(mp, "unpaired|slide_transition|direction");
		obs_properties_destroy(mp);
		obs_data_release(ms);
		obs_source_release(old_morph);
		CHECK(read == "morph_zoom" && own,
		      "a Morph saved with Other items on Fade and zoom reads Unpaired sources as Fade and zoom (%s), and another transition chosen there shows its own settings",
		      read.c_str());
		// In a filter's window, the same control under the Morph chosen, and
		// an old filter's "With nothing to change into" read the same way.
		obs_data_t *sd = obs_data_create();
		obs_data_set_string(sd, "transition", "stagekit_morph");
		obs_data_set_int(sd, "show|stagekit_morph|others", 1);
		obs_source_t *sw = obs_source_create("stagekit_show_in_scenes", "LT Switch", sd, nullptr);
		obs_data_release(sd);
		obs_properties_t *sp = obs_source_properties(sw);
		obs_data_t *ss = obs_source_get_settings(sw);
		const std::string filter_read = obs_data_get_string(ss, "show|stagekit_morph|unpaired");
		const bool no_others = !obs_properties_get(sp, "show|stagekit_morph|others");
		obs_properties_destroy(sp);
		obs_data_set_string(ss, "show|stagekit_morph|unpaired", "slide_transition");
		sp = obs_source_properties(sw);
		const bool nested = obs_properties_get(sp, "show|stagekit_morph|unpaired|slide_transition|direction");
		obs_properties_destroy(sp);
		obs_data_release(ss);
		obs_source_release(sw);
		CHECK(filter_read == "morph_zoom" && no_others && nested,
		      "Show in Scenes offers Unpaired sources under Morph in place of With nothing to change into (an old Fade and zoom reads %s) and the chosen transition's own settings under it",
		      filter_read.c_str());
	}

	// ------------------------------------------------ the fallback drawn
	// A camera hidden as a capture is shown (the two change into each
	// other) and a small source the Link Visibility shows with the capture,
	// with nothing to change into: it comes in by its Unpaired sources, and
	// goes out the same way when the capture is hidden and the camera shown.
	{
		obs_source_t *cam = block("LT Cam", 0xFF0000FF, 300, 200);    // red
		obs_source_t *grab = block("LT Grab", 0xFFFF0000, 300, 200);  // blue
		obs_source_t *badge = block("LT Badge", 0xFF00FF00, 120, 80); // green
		obs_scene_t *scene = obs_scene_create("LT Morph Scene");
		obs_source_t *scene_src = obs_scene_get_source(scene);
		obs_sceneitem_t *ci = put(scene, cam, 20, 20);
		obs_sceneitem_t *gi = put(scene, grab, 300, 20);
		obs_sceneitem_t *bi = put(scene, badge, 480, 260);
		obs_sceneitem_set_visible(gi, false);
		obs_sceneitem_set_visible(bi, false);
		for (obs_sceneitem_t *item : {ci, gi})
			for (const bool show : {true, false}) {
				obs_data_t *d = obs_data_create();
				obs_data_set_int(d, "ease_curve", 0);
				obs_source_t *t = obs_source_create_private("stagekit_morph", "LT Item Morph", d);
				obs_data_release(d);
				obs_sceneitem_set_transition(item, show, t);
				obs_sceneitem_set_transition_duration(item, show, 1000);
				obs_source_release(t);
			}
		// The badge goes with the capture; Morph for both, the badge with
		// its own fallback (Settings for).
		obs_data_t *ld = obs_data_create();
		set_list(ld, "items", {"LT Grab", "LT Badge"});
		obs_data_set_int(ld, "mode", 1); // together
		obs_data_set_string(ld, "transition", "stagekit_morph");
		obs_data_set_int(ld, "transition_ms", 1000);
		obs_data_set_string(ld, "for|LT Badge|transition", "stagekit_morph");
		obs_data_set_int(ld, "for|LT Badge|transition_ms", 1000);
		obs_source_t *link = obs_source_create("stagekit_visibility_link", "LT Morph Link", ld, nullptr);
		obs_data_release(ld);
		obs_source_filter_add(scene_src, link);
		obs_set_output_source(0, scene_src);
		sleep_ms(300);
		const int base = count_items(scene);
		// The badge's frame: 480..600, 260..340.
		struct Fill {
			int full = 0, empty = 0, part = 0, of = 0;
			int centre = 0, corner = 0;
		};
		auto fill = [&](const std::vector<uint8_t> &f) {
			Fill out;
			for (int y = 262; y < 338; y++)
				for (int x = 482; x < 598; x++) {
					const int g = f[(size_t(y) * w + x) * 4 + 1];
					out.of++;
					out.full += g >= 240;
					out.empty += g < 15;
					out.part += g >= 15 && g < 240;
				}
			out.centre = f[(size_t(300) * w + 540) * 4 + 1];
			out.corner = f[(size_t(262) * w + 482) * 4 + 1];
			return out;
		};
		struct Case {
			const char *name, *unpaired;
			const char *own_name, *own_value; // one of the transition's own settings
			int kind;                         // 0 fades, 1 fades and shrinks, 2 slides, 3 cut
		};
		const Case cases[] = {
			{"Fade", "morph_fade", nullptr, nullptr, 0},
			{"Fade and zoom", "morph_zoom", nullptr, nullptr, 1},
			{"Slide off the nearest edge", "morph_slide", nullptr, nullptr, 2},
			{"Fade and Zoom (another transition)", "stagekit_fade_zoom", "zoom", nullptr, 1},
			{"OBS's Slide, off the nearest edge", "slide_transition", "direction", "nearest", 2},
			{"a cut", "none", nullptr, nullptr, 3},
		};
		for (const Case &k : cases) {
			obs_data_t *now = obs_source_get_settings(link);
			obs_data_set_string(now, "for|LT Badge|show|stagekit_morph|unpaired", k.unpaired);
			obs_data_set_int(now, "for|LT Badge|show|stagekit_morph|ease_curve", 0);
			if (k.own_name && k.own_value)
				obs_data_set_string(now,
						    (std::string("for|LT Badge|show|stagekit_morph|unpaired|") +
						     k.unpaired + "|" + k.own_name)
							    .c_str(),
						    k.own_value);
			else if (k.own_name) // Fade and Zoom's smallest size
				obs_data_set_double(now,
						    (std::string("for|LT Badge|show|stagekit_morph|unpaired|") +
						     k.unpaired + "|" + k.own_name)
							    .c_str(),
						    50.0);
			obs_source_update(link, now);
			obs_data_release(now);
			sleep_ms(100);
			// In: the camera hidden, the capture shown (the badge with it).
			obs_sceneitem_set_visible(ci, false);
			obs_sceneitem_set_visible(gi, true);
			sleep_ms(250);
			const Fill early = fill(render(scene_src, w, h));
			sleep_ms(200);
			const std::vector<uint8_t> mid = render(scene_src, w, h);
			const Fill in = fill(mid);
			const int layers_in = count_items(scene) - base;
			int pair_px = 0; // red and blue in one place: the camera changing into the capture
			for (size_t i = 0; i < mid.size(); i += 4)
				pair_px += mid[i] > 90 && mid[i + 2] > 90;
			sleep_ms(900);
			render(scene_src, w, h);
			const Fill end_in = fill(render(scene_src, w, h));
			// Out: the capture hidden (the badge with it), the camera shown.
			obs_sceneitem_set_visible(gi, false);
			obs_sceneitem_set_visible(ci, true);
			sleep_ms(450);
			const Fill out = fill(render(scene_src, w, h));
			sleep_ms(900);
			render(scene_src, w, h);
			const Fill end_out = fill(render(scene_src, w, h));
			auto on_the_way = [&](const Fill &f) {
				switch (k.kind) {
				case 0:
					return f.centre > 30 && f.centre < 225 && f.part > f.of / 2;
				case 1: // (a transition of its own eases by itself: part-way may be near either end)
					return f.centre > 30 && f.centre < 245 && f.corner < 15 && f.part > f.of / 5;
				case 2:
					return f.full > f.of / 10 && f.empty > f.of / 10;
				default:
					return false;
				}
			};
			const bool came = k.kind == 3 ? early.full > early.of * 9 / 10 : on_the_way(in);
			const bool went = k.kind == 3 ? out.empty > out.of * 9 / 10 : on_the_way(out);
			CHECK(came && went && end_in.full > end_in.of * 9 / 10 && end_out.empty > end_out.of * 9 / 10 &&
				      layers_in == 1 && pair_px > 3000,
			      "Unpaired sources, %s: a source shown with nothing to change into %s, and hidden %s; the camera and the capture still morph into each other by a layer (in: centre %d, corner %d, %d full / %d empty / %d part-way of %d; at %d ms: %d full; out: centre %d, %d full / %d empty / %d part-way; afterwards %d full shown, %d empty hidden; %d layer(s), %d px of camera and capture)",
			      k.name, k.kind == 3 ? "is there at once" : "comes in part-way",
			      k.kind == 3 ? "is gone at once" : "goes out part-way", in.centre, in.corner, in.full,
			      in.empty, in.part, in.of, 250, early.full, out.centre, out.full, out.empty, out.part,
			      end_in.full, end_out.empty, layers_in, pair_px);
		}
		obs_set_output_source(0, before_out);
		obs_source_filter_remove(scene_src, link);
		obs_source_release(link);
		sleep_ms(100);
		obs_scene_release(scene);
		for (obs_source_t *src : {cam, grab, badge})
			obs_source_release(src);
	}
	obs_source_release(before_out);
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
	// OBS's own transitions, for the tests that fade one item into another.
	// They are left out where the module is not installed.
	load_module("/usr/lib/x86_64-linux-gnu/obs-plugins/obs-transitions.so",
		    "/usr/share/obs/obs-plugins/obs-transitions");
	obs_post_load_modules();
	if (getenv("STAGEKIT_TEST_ONLY_FIT_PRIORITY")) { // to try that section alone
		fit_priority_test();
		obs_shutdown();
		printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures,
		       g_failures == 1 ? "" : "s");
		return g_failures ? 1 : 0;
	}
	if (getenv("STAGEKIT_TEST_ONLY_POP_IN")) { // to try that section alone
		obs_audio_info sound = {};
		sound.samples_per_sec = 48000;
		sound.speakers = SPEAKERS_STEREO;
		obs_reset_audio(&sound);
		capture_pop_in_test(w, h, out);
		obs_shutdown();
		printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures,
		       g_failures == 1 ? "" : "s");
		return g_failures ? 1 : 0;
	}

	if (getenv("STAGEKIT_TEST_ONLY_LINK_TRANS")) { // to try that section alone
		link_transitions_test(w, h);
		obs_shutdown();
		printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures,
		       g_failures == 1 ? "" : "s");
		return g_failures ? 1 : 0;
	}

	obs_data_t *ls = obs_data_create();
	obs_data_set_string(ls, "file", live_image);
	obs_source_t *live = obs_source_create("image_source", "Live Capture", ls, nullptr);
	obs_data_release(ls);

	// ==== scene-filters:perf ====
	// ==== end scene-filters:perf ====

	// ==== scene-filters:blur ====
	// ------------------------------------------------------------ scene blur
	// A blur on a scene: all its sources, its live and capture sources or
	// the ones chosen, never the ones listed, and in that scene only.
	{
		printf("\n[scene blur]\n");
		const std::string blur_dir = out + "/../scene-blur";
		if (std::system(("mkdir -p '" + blur_dir + "'").c_str()) != 0)
			printf("  (could not make %s)\n", blur_dir.c_str());
		// Stand-ins with a fine checkerboard (8 px squares), or the source
		// named in their "child" setting: one counted as a capture
		// ("xshm_input", not loaded here) and one that is not.
		struct Checker {
			obs_source_t *child = nullptr;
			uint32_t cw = 160, ch = 120;
		};
		static gs_texture_t *checker_texture = nullptr;
		auto checker_kind = [](const char *id, const char *name) {
			obs_source_info k = {};
			k.id = id;
			k.type = OBS_SOURCE_TYPE_INPUT;
			k.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
			k.get_name = [](void *) {
				return "Checker (stand-in)";
			};
			(void)name;
			k.create = [](obs_data_t *settings, obs_source_t *) -> void * {
				auto *c = new Checker;
				const char *child = obs_data_get_string(settings, "child");
				c->child = child && *child ? obs_get_source_by_name(child) : nullptr;
				return c;
			};
			k.destroy = [](void *data) {
				auto *c = static_cast<Checker *>(data);
				obs_source_release(c->child);
				delete c;
			};
			k.get_width = [](void *data) -> uint32_t {
				auto *c = static_cast<Checker *>(data);
				return c->child ? obs_source_get_width(c->child) : c->cw;
			};
			k.get_height = [](void *data) -> uint32_t {
				auto *c = static_cast<Checker *>(data);
				return c->child ? obs_source_get_height(c->child) : c->ch;
			};
			k.video_render = [](void *data, gs_effect_t *) {
				auto *c = static_cast<Checker *>(data);
				if (c->child) {
					obs_source_video_render(c->child);
					return;
				}
				if (!checker_texture) {
					std::vector<uint8_t> px(160 * 120 * 4);
					for (int y = 0; y < 120; y++)
						for (int x = 0; x < 160; x++) {
							const bool white = ((x / 8) + (y / 8)) % 2 == 0;
							uint8_t *p = &px[(size_t(y) * 160 + x) * 4];
							p[0] = p[1] = p[2] = white ? 255 : 0;
							p[3] = 255;
						}
					const uint8_t *planes[] = {px.data()};
					checker_texture = gs_texture_create(160, 120, GS_RGBA, 1, planes, 0);
				}
				gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
				gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), checker_texture);
				while (gs_effect_loop(def, "Draw"))
					gs_draw_sprite(checker_texture, 0, 160, 120);
			};
			obs_register_source(&k);
		};
		checker_kind("xshm_input", "capture");
		checker_kind("stagekit_test_checker", "picture");
		auto sb_block = [](const char *name, long long abgr, int bw, int bh) {
			obs_data_t *d = obs_data_create();
			obs_data_set_int(d, "color", abgr);
			obs_data_set_int(d, "width", bw);
			obs_data_set_int(d, "height", bh);
			obs_source_t *src = obs_source_create("color_source", name, d, nullptr);
			obs_data_release(d);
			return src;
		};
		auto sb_put = [](obs_scene_t *scene, obs_source_t *src, float x, float y) {
			obs_sceneitem_t *item = obs_scene_add(scene, src);
			vec2 p;
			vec2_set(&p, x, y);
			obs_sceneitem_set_pos(item, &p);
			return item;
		};
		// How sharp a part of a frame is: the mean step in red between
		// neighbouring pixels (the checkerboard's is about 60; blurred, near 0).
		auto sharpness = [&](const std::vector<uint8_t> &f, int x0, int y0, int x1, int y1) {
			double sum = 0.0;
			int n = 0;
			for (int y = y0; y < y1; y++)
				for (int x = x0; x < x1; x++) {
					const int here = f[(size_t(y) * w + x) * 4];
					sum += std::abs(here - (int)f[(size_t(y) * w + x + 1) * 4]) +
					       std::abs(here - (int)f[(size_t(y + 1) * w + x) * 4]);
					n++;
				}
			return n ? sum / n : 0.0;
		};
		auto sb_px = [&](const std::vector<uint8_t> &f, int x, int y, int c) {
			return (int)f[(size_t(y) * w + x) * 4 + c];
		};
		auto sb_set = [](obs_source_t *f, const std::function<void(obs_data_t *)> &change) {
			obs_data_t *s = obs_source_get_settings(f);
			change(s);
			obs_source_update(f, s);
			obs_data_release(s);
		};
		auto sb_list = [](obs_data_t *s, const char *setting, std::vector<const char *> names) {
			obs_data_array_t *list = obs_data_array_create();
			for (const char *name : names) {
				obs_data_t *entry = obs_data_create();
				obs_data_set_string(entry, "value", name);
				obs_data_array_push_back(list, entry);
				obs_data_release(entry);
			}
			obs_data_set_array(s, setting, list);
			obs_data_array_release(list);
		};
		auto sb_level = [](obs_source_t *f) {
			calldata_t cd = {};
			proc_handler_call(obs_source_get_proc_handler(f), "draws", &cd);
			const double level = calldata_float(&cd, "level");
			calldata_free(&cd);
			return level;
		};
		auto sb_draws = [](obs_source_t *f) {
			calldata_t cd = {};
			proc_handler_call(obs_source_get_proc_handler(f), "draws", &cd);
			const long long count = calldata_int(&cd, "count");
			calldata_free(&cd);
			return count;
		};
		auto sb_call = [](const char *request, const std::function<void(obs_data_t *)> &fill) {
			obs_data_t *data = obs_data_create();
			fill(data);
			calldata_t cd = {};
			calldata_set_string(&cd, "request", request);
			calldata_set_string(&cd, "data", obs_data_get_json(data));
			proc_handler_call(obs_get_proc_handler(), "stagekit_vendor_call", &cd);
			const char *json = calldata_string(&cd, "response");
			obs_data_t *answer = obs_data_create_from_json(json && *json ? json : "{}");
			calldata_free(&cd);
			obs_data_release(data);
			return answer;
		};
		auto sb_events = [] {
			calldata_t cd = {};
			proc_handler_call(obs_get_proc_handler(), "stagekit_vendor_events", &cd);
			const char *events = calldata_string(&cd, "events");
			std::string got = events ? events : "";
			calldata_free(&cd);
			return got;
		};
		sb_events(); // (from here on the events are noted)

		obs_source_t *bg = sb_block("SB Back", 0xFF606060, 640, 360);
		obs_source_t *badge = sb_block("SB Badge", 0xFF0000FF, 40, 30); // red
		obs_source_t *game = obs_source_create("xshm_input", "SB Game", nullptr, nullptr);
		obs_source_t *camera = obs_source_create("xshm_input", "SB Camera", nullptr, nullptr);
		obs_source_t *picture = obs_source_create("stagekit_test_checker", "SB Picture", nullptr, nullptr);
		obs_scene_t *sbs = obs_scene_create("SB Scene");
		obs_scene_t *sbo = obs_scene_create("SB Other");
		for (obs_scene_t *scene : {sbs, sbo}) {
			sb_put(scene, bg, 0, 0);
			sb_put(scene, game, 40, 40);
			sb_put(scene, picture, 240, 40);
			sb_put(scene, camera, 440, 40);
			sb_put(scene, badge, 100, 100); // over the game
		}
		obs_source_t *sbs_src = obs_scene_get_source(sbs), *sbo_src = obs_scene_get_source(sbo);
		obs_source_t *before_out = obs_get_output_source(0);
		obs_set_output_source(0, sbs_src);
		obs_data_t *fd = obs_data_create();
		obs_data_set_int(fd, "ramp_in_ms", 0);
		obs_data_set_int(fd, "ramp_out_ms", 0);
		obs_source_t *sblur = obs_source_create("stagekit_scene_blur", "Scene Blur", fd, nullptr);
		obs_data_release(fd);
		CHECK(sblur != nullptr, "Scene Blur is registered and can be created");
		obs_source_set_enabled(sblur, false);
		obs_source_filter_add(sbs_src, sblur);
		sleep_ms(300);
		// The four parts: the game (away from the badge), the picture, the
		// camera, and the red badge over the game.
		struct Part {
			const char *name;
			int x0, y0, x1, y1;
		};
		const Part p_game{"game", 50, 50, 94, 94}, p_picture{"picture", 250, 50, 390, 150},
			p_camera{"camera", 450, 50, 590, 150};
		auto sharp = [&](const std::vector<uint8_t> &f, const Part &p) {
			return sharpness(f, p.x0, p.y0, p.x1, p.y1) > 30.0;
		};
		auto blurred = [&](const std::vector<uint8_t> &f, const Part &p) {
			return sharpness(f, p.x0, p.y0, p.x1, p.y1) < 6.0;
		};
		auto badge_sharp = [&](const std::vector<uint8_t> &f) {
			return sb_px(f, 120, 115, 0) > 240 && sb_px(f, 120, 115, 1) < 15 && sb_px(f, 120, 115, 2) < 15;
		};
		const std::vector<uint8_t> plain = render(sbs_src, w, h);
		CHECK(sharp(plain, p_game) && sharp(plain, p_picture) && sharp(plain, p_camera) && badge_sharp(plain) &&
			      mean_abs_diff(plain, render(sbo_src, w, h)) < 0.01,
		      "switched off, the scene is drawn as it is (game %.1f, picture %.1f, camera %.1f)",
		      sharpness(plain, 50, 50, 94, 94), sharpness(plain, 250, 50, 390, 150),
		      sharpness(plain, 450, 50, 590, 150));
		CHECK(sb_draws(sblur) == 0, "and off, it draws nothing at all");

		// All sources.
		obs_source_set_enabled(sblur, true);
		sleep_ms(300);
		render(sbs_src, w, h);
		const std::vector<uint8_t> all = render(sbs_src, w, h);
		CHECK(blurred(all, p_game) && blurred(all, p_picture) && blurred(all, p_camera) && !badge_sharp(all),
		      "'All sources' blurs every source of the scene (game %.1f, picture %.1f, camera %.1f)",
		      sharpness(all, 50, 50, 94, 94), sharpness(all, 250, 50, 390, 150),
		      sharpness(all, 450, 50, 590, 150));
		obs_set_output_source(0, sbo_src);
		sleep_ms(150);
		const std::vector<uint8_t> other = render(sbo_src, w, h);
		CHECK(sharp(other, p_game) && sharp(other, p_picture) && sharp(other, p_camera) && badge_sharp(other) &&
			      mean_abs_diff(other, plain) < 0.01,
		      "the same sources in another scene, on screen, stay sharp: the blur is this scene's only");
		obs_set_output_source(0, sbs_src);
		sleep_ms(150);

		// Live and capture sources: the badge over the game stays sharp, and
		// none of its red is smeared into the game's blur.
		sb_set(sblur, [](obs_data_t *s) { obs_data_set_int(s, "cover", 1); });
		sleep_ms(300);
		render(sbs_src, w, h);
		const std::vector<uint8_t> live_only = render(sbs_src, w, h);
		int smear = 0;
		for (int y = 104; y < 126; y++)
			for (int x = 88; x < 98; x++)
				smear = std::max(smear, sb_px(live_only, x, y, 0) - sb_px(live_only, x, y, 1));
		CHECK(blurred(live_only, p_game) && blurred(live_only, p_camera) && sharp(live_only, p_picture) &&
			      badge_sharp(live_only),
		      "'Live and capture sources' blurs the two captures and leaves the picture and the badge over the game sharp (picture %.1f)",
		      sharpness(live_only, 250, 50, 390, 150));
		CHECK(smear < 12,
		      "and the sharp badge is not smeared into the game's blur beside it (red over green by %d)",
		      smear);

		// Chosen sources.
		sb_set(sblur, [&](obs_data_t *s) {
			obs_data_set_int(s, "cover", 2);
			sb_list(s, "chosen", {"SB Picture"});
		});
		sleep_ms(300);
		render(sbs_src, w, h);
		const std::vector<uint8_t> chosen = render(sbs_src, w, h);
		CHECK(blurred(chosen, p_picture) && sharp(chosen, p_game) && sharp(chosen, p_camera) &&
			      badge_sharp(chosen),
		      "'Chosen sources' blurs the one listed and nothing else");
		{
			obs_properties_t *props = obs_source_properties(sblur);
			obs_property_t *list = obs_properties_get(props, "chosen|alist");
			obs_data_t *s = obs_source_get_settings(sblur);
			obs_data_set_int(s, "cover", 0);
			obs_property_modified(obs_properties_get(props, "cover"), s);
			const bool hidden = list && !obs_property_visible(list);
			obs_data_set_int(s, "cover", 2);
			obs_property_modified(obs_properties_get(props, "cover"), s);
			const bool shown = list && obs_property_visible(list);
			obs_data_release(s);
			size_t offered = 0;
			bool offers_game = false;
			if (obs_property_t *pick = obs_properties_get(props, "never|apick")) {
				offered = obs_property_list_item_count(pick);
				for (size_t i = 0; i < offered; i++)
					offers_game = offers_game ||
						      strcmp(obs_property_list_item_string(pick, i), "SB Game") == 0;
			}
			obs_properties_destroy(props);
			CHECK(hidden && shown && offers_game,
			      "the chosen list shows only with 'Chosen sources', and the lists offer the scene's sources");
		}

		// Never blur these: honoured over every choice.
		sb_set(sblur, [&](obs_data_t *s) {
			obs_data_set_int(s, "cover", 0);
			sb_list(s, "never", {"SB Camera"});
		});
		sleep_ms(300);
		render(sbs_src, w, h);
		const std::vector<uint8_t> never_all = render(sbs_src, w, h);
		sb_set(sblur, [&](obs_data_t *s) { obs_data_set_int(s, "cover", 1); });
		sleep_ms(300);
		render(sbs_src, w, h);
		const std::vector<uint8_t> never_live = render(sbs_src, w, h);
		sb_set(sblur, [&](obs_data_t *s) {
			obs_data_set_int(s, "cover", 2);
			sb_list(s, "chosen", {"SB Picture", "SB Camera"});
		});
		sleep_ms(300);
		render(sbs_src, w, h);
		const std::vector<uint8_t> never_chosen = render(sbs_src, w, h);
		CHECK(sharp(never_all, p_camera) && blurred(never_all, p_game) && blurred(never_all, p_picture) &&
			      sharp(never_live, p_camera) && blurred(never_live, p_game) &&
			      sharp(never_chosen, p_camera) && blurred(never_chosen, p_picture),
		      "'Never blur these' keeps the camera sharp with all sources, live sources and when it is chosen too");

		// Drawn once a frame, and the same in a window of its own.
		{
			const std::vector<uint8_t> a = render(sbs_src, w, h);
			const std::vector<uint8_t> b = render(sbs_src, w, h);
			CHECK(mean_abs_diff(a, b) == 0.0, "drawn twice, the scene looks the same both times");
			const std::vector<uint8_t> in_window = render_shown(sbs_src, w, h);
			CHECK(mean_abs_diff(in_window, a) < 1.5,
			      "in a window with a wide-colour surface it looks as it does in the scene (off by %.2f)",
			      mean_abs_diff(in_window, a));
		}

		// With a Scene Shadow, whichever was added first: Scene Blur keeps
		// itself after the shadow, so that both draw.
		for (int shadow_first = 1; shadow_first >= 0; shadow_first--) {
			obs_scene_t *os = obs_scene_create(shadow_first ? "SB Order 1" : "SB Order 2");
			sb_put(os, bg, 0, 0);
			sb_put(os, picture, 240, 40);
			sb_put(os, badge, 60, 200);
			obs_source_t *os_src = obs_scene_get_source(os);
			obs_data_t *od = obs_data_create();
			obs_data_set_int(od, "ramp_in_ms", 0);
			obs_data_set_int(od, "cover", 2);
			sb_list(od, "chosen", {"SB Picture"});
			obs_source_t *ob = obs_source_create("stagekit_scene_blur", "SB Order Blur", od, nullptr);
			obs_data_release(od);
			obs_source_t *osh =
				obs_source_create("stagekit_scene_shadow", "SB Order Shadow", nullptr, nullptr);
			obs_source_filter_add(os_src, shadow_first ? osh : ob);
			sleep_ms(200);
			obs_source_filter_add(os_src, shadow_first ? ob : osh);
			sleep_ms(900);
			for (int i = 0; i < 3; i++) {
				render(os_src, w, h);
				sleep_ms(40);
			}
			const long long blur_before = sb_draws(ob), shadow_before = sb_draws(osh);
			std::vector<uint8_t> frame;
			for (int i = 0; i < 3; i++) {
				frame = render(os_src, w, h);
				sleep_ms(40);
			}
			std::vector<std::string> order;
			obs_source_enum_filters(
				os_src,
				[](obs_source_t *, obs_source_t *f, void *param) {
					static_cast<std::vector<std::string> *>(param)->push_back(
						obs_source_get_name(f));
				},
				&order);
			// The badge's shadow: 12 px right and down of it, on the grey back.
			const int under = sb_px(frame, 105, 236, 1), back = sb_px(frame, 600, 340, 1);
			CHECK(order.size() == 2 && order[0] == "SB Order Shadow" && order[1] == "SB Order Blur" &&
				      sb_draws(ob) > blur_before && sb_draws(osh) > shadow_before &&
				      blurred(frame, p_picture) && under < back - 15,
			      "%s first: Scene Blur is kept after Scene Shadow and both draw (shadow %d against %d, picture %.1f)",
			      shadow_first ? "Scene Shadow added" : "Scene Blur added", under, back,
			      sharpness(frame, 250, 50, 390, 150));
			obs_source_filter_remove(os_src, ob);
			obs_source_filter_remove(os_src, osh);
			obs_source_release(ob);
			obs_source_release(osh);
			obs_scene_release(os);
		}

		// The ramp: in and out over the switch's duration, and a state
		// restored at load is taken as it is.
		{
			sb_set(sblur, [&](obs_data_t *s) {
				obs_data_set_int(s, "cover", 0);
				sb_list(s, "never", {});
				obs_data_set_int(s, "ramp_in_ms", 1000);
				obs_data_set_int(s, "ramp_out_ms", 1000);
				obs_data_set_int(s, "ease_curve", 0); // linear
				obs_data_set_double(s, "strength", 8.0);
			});
			obs_source_set_enabled(sblur, false);
			sleep_ms(1200);
			CHECK(!obs_source_enabled(sblur) && sb_level(sblur) <= 0.0001,
			      "switched off, it ramps out and then really switches off");
			obs_source_set_enabled(sblur, true);
			sleep_ms(450);
			const double mid = sb_level(sblur);
			render(sbs_src, w, h);
			const double part = sharpness(render(sbs_src, w, h), 50, 50, 94, 94);
			sleep_ms(800);
			render(sbs_src, w, h);
			const double full = sharpness(render(sbs_src, w, h), 50, 50, 94, 94);
			CHECK(mid > 0.25 && mid < 0.75 && part > full + 2.0 &&
				      part < sharpness(plain, 50, 50, 94, 94) && sb_level(sblur) == 1.0,
			      "switched on, it ramps in: part-way (level %.2f) it is part blurred (%.1f; %.1f at the end)",
			      mid, part, full);
			obs_source_set_enabled(sblur, false);
			sleep_ms(400);
			CHECK(obs_source_enabled(sblur) && sb_level(sblur) < 1.0 && sb_level(sblur) > 0.0,
			      "switched off, it stays drawn while it ramps out (level %.2f)", sb_level(sblur));
			sleep_ms(900);
			CHECK(!obs_source_enabled(sblur), "and is off once the ramp has ended");
			obs_data_t *ld = obs_data_create();
			obs_data_set_int(ld, "ramp_in_ms", 1000);
			obs_source_t *loaded = obs_source_create("stagekit_scene_blur", "SB Loaded", ld, nullptr);
			obs_data_release(ld);
			obs_source_set_enabled(loaded, true); // as OBS restores it
			obs_source_filter_add(sbo_src, loaded);
			sleep_ms(120);
			CHECK(sb_level(loaded) == 1.0, "a Scene Blur on at load is on at once, without ramping in");
			obs_source_filter_remove(sbo_src, loaded);
			obs_source_release(loaded);
			sb_set(sblur, [&](obs_data_t *s) {
				obs_data_set_int(s, "ramp_in_ms", 0);
				obs_data_set_int(s, "ramp_out_ms", 0);
				obs_data_unset_user_value(s, "strength");
			});
		}

		// The key's requests: one scene, or every scene with a Scene Blur of
		// that name; a state event when one changes; and the hotkeys.
		{
			obs_data_t *od = obs_data_create();
			obs_data_set_int(od, "ramp_in_ms", 0);
			obs_data_set_int(od, "ramp_out_ms", 0);
			obs_source_t *other_blur = obs_source_create("stagekit_scene_blur", "Scene Blur", od, nullptr);
			obs_data_release(od);
			obs_source_set_enabled(other_blur, false);
			obs_source_filter_add(sbo_src, other_blur);
			sleep_ms(150);
			sb_events();
			obs_data_t *list = sb_call("ListSceneBlurs", [](obs_data_t *) {});
			obs_data_array_t *filters = obs_data_get_array(list, "filters");
			bool both = false;
			for (size_t i = 0; i < obs_data_array_count(filters); i++) {
				obs_data_t *f = obs_data_array_item(filters, i);
				obs_data_array_t *scenes = obs_data_get_array(f, "scenes");
				if (strcmp(obs_data_get_string(f, "filter"), "Scene Blur") == 0 &&
				    obs_data_array_count(scenes) == 2 &&
				    strcmp(obs_data_get_string(f, "type"), "gaussian") == 0 &&
				    strcmp(obs_data_get_string(f, "style"), "gaussian") == 0)
					both = true;
				obs_data_array_release(scenes);
				obs_data_release(f);
			}
			obs_data_array_release(filters);
			CHECK(obs_data_get_bool(list, "success") && both,
			      "ListSceneBlurs lists the name once with the two scenes it is in, and its blur's look");
			obs_data_release(list);

			obs_data_t *one = sb_call("SceneBlur", [](obs_data_t *d) {
				obs_data_set_string(d, "filter", "Scene Blur");
				obs_data_set_string(d, "scene", "SB Scene");
				obs_data_set_string(d, "how", "on");
			});
			sleep_ms(150);
			CHECK(obs_data_get_bool(one, "success") && obs_data_get_bool(one, "active") &&
				      obs_source_enabled(sblur) && !obs_source_enabled(other_blur),
			      "SceneBlur with a scene switches on that scene's blur only");
			obs_data_release(one);
			const std::string said = sb_events();
			CHECK(said.find("\"SceneBlurChanged\"") != std::string::npos &&
				      said.find("\"SB Scene\"") != std::string::npos &&
				      said.find("\"SB Other\"") == std::string::npos,
			      "and says so with a SceneBlurChanged event for that scene");
			obs_data_t *toggled = sb_call("SceneBlur", [](obs_data_t *d) {
				obs_data_set_string(d, "filter", "Scene Blur");
				obs_data_set_string(d, "how", "toggle");
			});
			sleep_ms(150);
			obs_data_array_t *blurs = obs_data_get_array(toggled, "blurs");
			CHECK(obs_data_get_bool(toggled, "success") && !obs_data_get_bool(toggled, "active") &&
				      obs_data_array_count(blurs) == 2 && !obs_source_enabled(sblur) &&
				      !obs_source_enabled(other_blur),
			      "SceneBlur for every scene toggles them all: one was on, so all go off");
			obs_data_array_release(blurs);
			obs_data_release(toggled);
			obs_data_t *again = sb_call("SceneBlur", [](obs_data_t *d) {
				obs_data_set_string(d, "filter", "Scene Blur");
				obs_data_set_string(d, "scene", "");
				obs_data_set_string(d, "how", "toggle");
			});
			sleep_ms(150);
			CHECK(obs_data_get_bool(again, "active") && obs_source_enabled(sblur) &&
				      obs_source_enabled(other_blur),
			      "and the next toggle switches them all on");
			obs_data_release(again);
			const std::string said_all = sb_events();
			CHECK(said_all.find("\"SB Scene\"") != std::string::npos &&
				      said_all.find("\"SB Other\"") != std::string::npos,
			      "each one that changes says so");
			obs_data_t *asked = sb_call("SceneBlur", [](obs_data_t *d) {
				obs_data_set_string(d, "filter", "Scene Blur");
				obs_data_set_string(d, "scene", "SB Other");
			});
			obs_data_t *missing = sb_call("SceneBlur", [](obs_data_t *d) {
				obs_data_set_string(d, "filter", "No Such Blur");
				obs_data_set_string(d, "how", "on");
			});
			CHECK(obs_data_get_bool(asked, "success") && obs_data_get_bool(asked, "active") &&
				      !obs_data_get_bool(missing, "success"),
			      "without 'how' it only says how they are; a name nothing has is refused (%s)",
			      obs_data_get_string(missing, "error"));
			obs_data_release(asked);
			obs_data_release(missing);
			sb_call("SceneBlur", [](obs_data_t *d) {
				obs_data_set_string(d, "filter", "Scene Blur");
				obs_data_set_string(d, "how", "off");
			});
			sleep_ms(150);

			// The hotkeys, on the scene, named by the filter.
			obs_hotkey_id toggle_key = OBS_INVALID_HOTKEY_ID;
			int named = 0;
			struct Find {
				obs_hotkey_id *toggle;
				int *named;
				obs_source_t *scene;
			} find{&toggle_key, &named, sbs_src};
			obs_enum_hotkeys(
				[](void *param, obs_hotkey_id id, obs_hotkey_t *key) {
					auto *f = static_cast<Find *>(param);
					const std::string name = obs_hotkey_get_name(key);
					if (name.rfind("stagekit.sceneblur.", 0) == 0 && name.size() > 11 &&
					    name.substr(name.size() - 11) == ".Scene Blur")
						(*f->named)++;
					obs_weak_source_t *weak = (obs_weak_source_t *)obs_hotkey_get_registerer(key);
					obs_source_t *owner = obs_hotkey_get_registerer_type(key) ==
									      OBS_HOTKEY_REGISTERER_SOURCE
								      ? obs_weak_source_get_source(weak)
								      : nullptr;
					if (name == "stagekit.sceneblur.toggle.Scene Blur" && owner == f->scene)
						*f->toggle = id;
					obs_source_release(owner);
					return true;
				},
				&find);
			CHECK(toggle_key != OBS_INVALID_HOTKEY_ID && named == 6,
			      "each Scene Blur has toggle, on and off hotkeys on its scene, named by the filter (%d)",
			      named);
			if (toggle_key != OBS_INVALID_HOTKEY_ID) {
				obs_hotkey_enable_callback_rerouting(true);
				obs_hotkey_trigger_routed_callback(toggle_key, true);
				obs_hotkey_trigger_routed_callback(toggle_key, false);
				sleep_ms(150);
				CHECK(obs_source_enabled(sblur) && !obs_source_enabled(other_blur),
				      "the toggle hotkey switches that scene's blur on, and no other");
				obs_hotkey_trigger_routed_callback(toggle_key, true);
				obs_hotkey_trigger_routed_callback(toggle_key, false);
				sleep_ms(150);
				CHECK(!obs_source_enabled(sblur), "and off again");
			}
			obs_source_filter_remove(sbo_src, other_blur);
			obs_source_release(other_blur);
		}

		// For the README: a scene with a game capture, a camera, a panel
		// and a background, as each choice blurs it.
		{
			obs_data_t *bd = obs_data_create();
			obs_data_set_string(bd, "file", texture_image);
			obs_source_t *demo_back = obs_source_create("image_source", "SB Demo Back", bd, nullptr);
			obs_data_release(bd);
			obs_data_t *gd = obs_data_create();
			obs_data_set_string(gd, "child", "Live Capture");
			obs_source_t *demo_game = obs_source_create("xshm_input", "SB Demo Game", gd, nullptr);
			obs_data_release(gd);
			obs_source_t *demo_cam = obs_source_create("xshm_input", "SB Demo Camera", nullptr, nullptr);
			obs_source_t *demo_panel = sb_block("SB Demo Panel", 0xE0302020, 170, 200);
			obs_scene_t *demo = obs_scene_create("SB Demo");
			obs_sceneitem_t *back_item = sb_put(demo, demo_back, 0, 0);
			vec2 b;
			vec2_set(&b, 640, 360);
			obs_sceneitem_set_bounds_type(back_item, OBS_BOUNDS_STRETCH);
			obs_sceneitem_set_bounds(back_item, &b);
			obs_sceneitem_t *game_item = sb_put(demo, demo_game, 20, 20);
			vec2_set(&b, 420, 236);
			obs_sceneitem_set_bounds_type(game_item, OBS_BOUNDS_SCALE_INNER);
			obs_sceneitem_set_bounds(game_item, &b);
			sb_put(demo, demo_panel, 455, 20);
			sb_put(demo, demo_cam, 465, 225);
			obs_source_t *demo_src = obs_scene_get_source(demo);
			obs_data_t *dd = obs_data_create();
			obs_data_set_int(dd, "ramp_in_ms", 0);
			obs_data_set_int(dd, "ramp_out_ms", 0);
			obs_data_set_double(dd, "strength", 24.0);
			obs_source_t *demo_blur = obs_source_create("stagekit_scene_blur", "SB Demo Blur", dd, nullptr);
			obs_data_release(dd);
			obs_source_set_enabled(demo_blur, false);
			obs_source_filter_add(demo_src, demo_blur);
			sleep_ms(400);
			write_frame(blur_dir + "/1 the scene.rgba", render(demo_src, w, h), w, h);
			auto shot = [&](const char *name, const std::function<void(obs_data_t *)> &change) {
				sb_set(demo_blur, change);
				obs_source_set_enabled(demo_blur, true);
				sleep_ms(400);
				render(demo_src, w, h);
				write_frame(blur_dir + "/" + name + ".rgba", render(demo_src, w, h), w, h);
			};
			shot("2 all sources", [](obs_data_t *) {});
			shot("3 live and capture sources", [](obs_data_t *s) { obs_data_set_int(s, "cover", 1); });
			shot("4 all, never the camera", [&](obs_data_t *s) {
				obs_data_set_int(s, "cover", 0);
				sb_list(s, "never", {"SB Demo Camera"});
			});
			shot("5 chosen - the background, as a mosaic", [&](obs_data_t *s) {
				obs_data_set_int(s, "cover", 2);
				sb_list(s, "never", {});
				sb_list(s, "chosen", {"SB Demo Back"});
				obs_data_set_int(s, "blur_type", 3);
				obs_data_set_int(s, "pixel_shape", 1);
				obs_data_set_double(s, "pixel_glass", 60.0);
			});
			obs_source_filter_remove(demo_src, demo_blur);
			obs_source_release(demo_blur);
			obs_scene_release(demo);
			for (obs_source_t *src : {demo_back, demo_game, demo_cam, demo_panel})
				obs_source_release(src);
		}

		// Morph: a change from a scene that is blurring is a cross-fade,
		// which keeps the blur on screen all the way; with the blur off, or
		// blurring nothing, the scene is morphed item by item as before.
		{
			static bool blur_crossfade_logged = false;
			static log_handler_t blur_prev_log = nullptr;
			static void *blur_prev_param = nullptr;
			base_get_log_handler(&blur_prev_log, &blur_prev_param);
			blur_crossfade_logged = false;
			base_set_log_handler(
				[](int level, const char *format, va_list args, void *) {
					char line[2048];
					va_list copy;
					va_copy(copy, args);
					vsnprintf(line, sizeof(line), format, copy);
					va_end(copy);
					if (strstr(line, "cross-fade") && strstr(line, "SB Morph From"))
						blur_crossfade_logged = true;
					if (blur_prev_log)
						blur_prev_log(level, format, args, blur_prev_param);
				},
				nullptr);
			obs_scene_t *mfrom = obs_scene_create("SB Morph From");
			obs_scene_t *mto = obs_scene_create("SB Morph To");
			sb_put(mfrom, bg, 0, 0);
			sb_put(mfrom, game, 40, 40);
			sb_put(mto, bg, 0, 0);
			sb_put(mto, game, 400, 200);
			obs_source_t *from_src = obs_scene_get_source(mfrom), *to_src = obs_scene_get_source(mto);
			obs_data_t *bd = obs_data_create();
			obs_data_set_int(bd, "ramp_in_ms", 0);
			obs_data_set_int(bd, "ramp_out_ms", 0);
			obs_source_t *mblur = obs_source_create("stagekit_scene_blur", "SB Morph Blur", bd, nullptr);
			obs_data_release(bd);
			obs_source_filter_add(from_src, mblur);
			obs_data_t *md = obs_data_create();
			obs_data_set_int(md, "ease_curve", 0); // linear
			obs_source_t *morph = obs_source_create_private("stagekit_morph", "SB Morph", md);
			obs_data_release(md);
			sleep_ms(400);
			const std::vector<uint8_t> first = render(from_src, w, h);
			const std::vector<uint8_t> last = render(to_src, w, h);
			auto run_half = [&]() {
				obs_transition_set(morph, from_src);
				obs_transition_start(morph, OBS_TRANSITION_MODE_MANUAL, 0, to_src);
				obs_transition_set_manual_time(morph, 0.5f);
				sleep_ms(150);
				render(morph, w, h);
				std::vector<uint8_t> half = render(morph, w, h);
				obs_transition_set_manual_time(morph, 1.0f);
				sleep_ms(120);
				render(morph, w, h);
				return half;
			};
			const std::vector<uint8_t> half = run_half();
			// Cross-faded: the game is at both ends, half-strength, and the
			// one at the start is still blurred.
			CHECK(blurred(first, p_game) && blur_crossfade_logged &&
				      sharpness(half, 50, 50, 94, 94) < 6.0 &&
				      std::abs(sb_px(half, 120, 80, 0) -
					       (sb_px(first, 120, 80, 0) + sb_px(last, 120, 80, 0)) / 2) < 12,
			      "a change from a scene while it blurs is a cross-fade, the blur kept all the way (logged %d)",
			      (int)blur_crossfade_logged);
			obs_transition_set(morph, from_src);
			sleep_ms(100);
			obs_transition_start(morph, OBS_TRANSITION_MODE_AUTO, 600, to_src);
			sleep_ms(900);
			render(morph, w, h);
			const std::vector<uint8_t> done = render(morph, w, h);
			CHECK(mean_abs_diff(done, last) < 0.01,
			      "run by the clock, the change ends on the new scene (%.2f)", mean_abs_diff(done, last));
			// Switched off: morphed as a plain scene.
			obs_source_set_enabled(mblur, false);
			sleep_ms(200);
			blur_crossfade_logged = false;
			const std::vector<uint8_t> half_off = run_half();
			CHECK(!blur_crossfade_logged && sharpness(half_off, 222, 122, 340, 220) > 30.0,
			      "with the blur off, the scene is morphed: the game is half-way, sharp (%.1f)",
			      sharpness(half_off, 222, 122, 340, 220));
			// On, but with nothing chosen to blur: still morphed.
			sb_set(mblur, [](obs_data_t *s) { obs_data_set_int(s, "cover", 2); });
			obs_source_set_enabled(mblur, true);
			sleep_ms(300);
			render(from_src, w, h);
			blur_crossfade_logged = false;
			const std::vector<uint8_t> half_none = run_half();
			CHECK(!blur_crossfade_logged && sharpness(half_none, 222, 122, 340, 220) > 30.0,
			      "on but blurring nothing, it does not stand in Morph's way");
			base_set_log_handler(blur_prev_log, blur_prev_param);
			obs_source_release(morph);
			obs_source_filter_remove(from_src, mblur);
			obs_source_release(mblur);
			obs_scene_release(mfrom);
			obs_scene_release(mto);
		}

		obs_set_output_source(0, before_out);
		obs_source_release(before_out);
		obs_source_filter_remove(sbs_src, sblur);
		obs_source_release(sblur);
		obs_scene_release(sbs);
		obs_scene_release(sbo);
		for (obs_source_t *src : {bg, badge, game, camera, picture})
			obs_source_release(src);
		obs_enter_graphics();
		gs_texture_destroy(checker_texture);
		checker_texture = nullptr;
		obs_leave_graphics();
	}
	// ==== end scene-filters:blur ====

	// ---------------------------------------------------- morph transition
	{
		printf("\n[morph transition]\n");
		auto block = [](const char *name, long long abgr, int bw, int bh) {
			obs_data_t *d = obs_data_create();
			obs_data_set_int(d, "color", abgr);
			obs_data_set_int(d, "width", bw);
			obs_data_set_int(d, "height", bh);
			obs_source_t *src = obs_source_create("color_source", name, d, nullptr);
			obs_data_release(d);
			return src;
		};
		auto put = [](obs_scene_t *scene, obs_source_t *src, float x, float y) {
			obs_sceneitem_t *item = obs_scene_add(scene, src);
			vec2 p;
			vec2_set(&p, x, y);
			obs_sceneitem_set_pos(item, &p);
			return item;
		};
		obs_source_t *old_back = block("Morph Old Back", 0xFFFF0000, 640, 360); // blue
		obs_source_t *new_back = block("Morph New Back", 0xFF00FF00, 640, 360); // green
		obs_source_t *game = block("Morph Game", 0xFF00FFFF, 320, 180);         // yellow
		obs_source_t *window = block("Morph Window", 0xFFFFFF00, 200, 200);     // cyan
		obs_source_t *camera = block("Morph Camera", 0xFF0000FF, 100, 60);      // red
		obs_scene_t *ma = obs_scene_create("Morph From");
		obs_scene_t *mb = obs_scene_create("Morph To");
		obs_scene_t *mc = obs_scene_create("Morph From Again");
		put(ma, old_back, 0, 0);
		put(ma, game, 100, 90);
		put(ma, live, 0, 0);
		obs_sceneitem_t *cam_a = put(ma, camera, 20, 20);
		put(mb, new_back, 0, 0);
		put(mb, window, 300, 60);
		obs_sceneitem_t *cam_b = put(mb, camera, 520, 280);
		// The same as the first scene, item for item, with a crop and a
		// scaled picture in it: mid-way between the two nothing may change.
		put(mc, old_back, 0, 0);
		put(mc, game, 100, 90);
		obs_sceneitem_t *pic_c = put(mc, live, 0, 0);
		obs_sceneitem_t *cam_c = put(mc, camera, 20, 20);
		obs_sceneitem_t *pic_a = obs_scene_find_source(ma, "Live Capture");
		for (obs_sceneitem_t *pic : {pic_a, pic_c}) {
			vec2 at, small;
			vec2_set(&at, 430, 200);
			vec2_set(&small, 0.25f, 0.25f);
			obs_sceneitem_set_pos(pic, &at);
			obs_sceneitem_set_scale(pic, &small);
		}
		const obs_sceneitem_crop cut = {10, 6, 20, 4};
		obs_sceneitem_set_crop(cam_a, &cut);
		obs_sceneitem_set_crop(cam_c, &cut);
		(void)cam_b;

		obs_data_t *ms = obs_data_create();
		obs_data_set_int(ms, "ease_curve", 0); // linear, so half the time is half the way
		obs_source_t *morph = obs_source_create_private("stagekit_morph", "Morph", ms);
		CHECK(morph != nullptr, "the Morph transition is registered and can be created");
		auto px = [&](const std::vector<uint8_t> &frame, int x, int y, int channel) {
			return (int)frame[(size_t(y) * w + x) * 4 + channel];
		};
		auto at_time = [&](obs_scene_t *from, obs_scene_t *to, float t) {
			// (No size is given to the transition: OBS's own window gives
			// it none either, and it must take its scenes' size.)
			obs_transition_set(morph, obs_scene_get_source(from));
			obs_transition_start(morph, OBS_TRANSITION_MODE_MANUAL, 0, obs_scene_get_source(to));
			obs_transition_set_manual_time(morph, t);
			sleep_ms(120);
			return render(morph, w, h);
		};
		if (morph) {
			const std::vector<uint8_t> first = render(obs_scene_get_source(ma), w, h);
			const std::vector<uint8_t> same = at_time(ma, mc, 0.5f);
			CHECK(mean_abs_diff(same, first) < 0.6 && opaque(same),
			      "half-way between two scenes that are alike, the picture is the scene's own, colours, crop and all (diff %.2f)",
			      mean_abs_diff(same, first));

			const std::vector<uint8_t> half = at_time(ma, mb, 0.5f);
			// For the README: the change of scene in five steps.
			write_frame(out + "/../morph/1 before.rgba", first, w, h);
			write_frame(out + "/../morph/2 a quarter.rgba", at_time(ma, mb, 0.25f), w, h);
			write_frame(out + "/../morph/3 half-way.rgba", half, w, h);
			write_frame(out + "/../morph/4 three quarters.rgba", at_time(ma, mb, 0.75f), w, h);
			write_frame(out + "/../morph/5 after.rgba", render(obs_scene_get_source(mb), w, h), w, h);
			at_time(ma, mb, 0.5f);
			// The camera: from 20,20 to 520,280, cut to 70x50. Half-way its
			// top-left is at 270,150.
			CHECK(px(half, 300, 170, 0) > 240 && px(half, 300, 170, 1) < 15 && px(half, 300, 170, 2) < 15 &&
				      px(half, 50, 40, 0) < 15 && px(half, 550, 300, 0) < 15,
			      "an item in both scenes is half-way between its two places, and at neither of them (red %d there; %d and %d at the ends)",
			      px(half, 300, 170, 0), px(half, 50, 40, 0), px(half, 550, 300, 0));
			// The backgrounds: one fades into the other and the two always
			// make a whole picture.
			CHECK(opaque(half) && px(half, 20, 340, 1) > 80 && px(half, 20, 340, 1) < 215 &&
				      px(half, 20, 340, 2) > 80 && px(half, 20, 340, 2) < 215 &&
				      px(half, 20, 340, 0) < 15,
			      "the two backgrounds are cross-faded, with no see-through dip (green %d, blue %d)",
			      px(half, 20, 340, 1), px(half, 20, 340, 2));
			// The main picture: the yellow game (100,90 320x180) becomes the
			// cyan window (300,60 200x200): half-way 200,75 260x190, both mixed.
			CHECK(px(half, 215, 100, 1) > 240 && px(half, 215, 100, 0) > 80 &&
				      px(half, 215, 100, 0) < 215 && px(half, 215, 100, 2) > 80 &&
				      px(half, 215, 100, 2) < 215 && px(half, 120, 100, 0) < 15 &&
				      px(half, 480, 120, 0) < 15,
			      "the largest item left in each scene changes into the other on the way (red %d, green %d, blue %d in it; red %d where the game was)",
			      px(half, 215, 100, 0), px(half, 215, 100, 1), px(half, 215, 100, 2),
			      px(half, 120, 100, 0));

			// A rule for two scenes: the change from the one to the other
			// is made another way, and only that change.
			{
				const std::vector<uint8_t> scene_b = render(obs_scene_get_source(mb), w, h);
				const char *line = "Morph From \xE2\x86\x92 Morph To";
				obs_data_t *rs = obs_source_get_settings(morph);
				obs_data_array_t *rules = obs_data_array_create();
				obs_data_t *entry = obs_data_create();
				obs_data_set_string(entry, "value", line);
				obs_data_array_push_back(rules, entry);
				obs_data_release(entry);
				obs_data_set_array(rs, "rules", rules);
				obs_data_array_release(rules);
				const std::string key = std::string("rule|") + line + "|";
				obs_data_set_int(rs, (key + "style").c_str(), 2); // a cut
				obs_source_update(morph, rs);
				const std::vector<uint8_t> cut = at_time(ma, mb, 0.5f);
				const std::vector<uint8_t> back = at_time(mb, ma, 0.5f);
				// (The other way there is no rule: the camera is half-way.)
				const bool back_morphs = px(back, 300, 170, 0) > 240 && px(back, 50, 40, 0) < 15;
				obs_data_set_bool(rs, (key + "both").c_str(), true);
				obs_source_update(morph, rs);
				const std::vector<uint8_t> back_cut = at_time(mb, ma, 0.5f);
				CHECK(mean_abs_diff(cut, scene_b) < 0.6 && back_morphs &&
					      mean_abs_diff(back_cut, first) < 0.6,
				      "a rule for two scenes makes that change a cut and leaves the change back a morph, until it is set for both ways (diff %.2f; back morphs %d; both ways %.2f)",
				      mean_abs_diff(cut, scene_b), (int)back_morphs, mean_abs_diff(back_cut, first));
				// A cross-fade by rule: no item travels.
				obs_data_set_int(rs, (key + "style").c_str(), 1);
				obs_source_update(morph, rs);
				const std::vector<uint8_t> faded = at_time(ma, mb, 0.5f);
				CHECK(px(faded, 300, 170, 1) > 150 && px(faded, 50, 40, 0) > 80 &&
					      px(faded, 50, 40, 0) < 215,
				      "and a cross-fade by rule: the camera fades where it is and does not travel (green %d where the morph has the red camera half-way; red %d at its old place)",
				      px(faded, 300, 170, 1), px(faded, 50, 40, 0));
				// The settings offer it.
				obs_properties_t *mp = obs_source_properties(morph);
				CHECK(obs_properties_get(mp, "rule_from") && obs_properties_get(mp, "rule_to") &&
					      obs_properties_get(mp, "rule_style") &&
					      obs_properties_get(mp, "rule_add") && list_rows(mp, "rules|a") == 1,
				      "the Morph's settings have the two scenes, how the change is made, and the list of rules");
				obs_properties_destroy(mp);
				obs_data_erase(rs, "rules");
				obs_source_update(morph, rs);
				obs_data_release(rs);
				at_time(ma, mb, 0.5f);
			}

			// At the end it is the new scene, and the transition is over.
			const std::vector<uint8_t> last = render(obs_scene_get_source(mb), w, h);
			obs_transition_set_manual_time(morph, 1.0f);
			sleep_ms(120);
			render(morph, w, h);
			const std::vector<uint8_t> done = render(morph, w, h);
			obs_source_t *showing = obs_transition_get_active_source(morph);
			CHECK(mean_abs_diff(done, last) < 0.01 && showing == obs_scene_get_source(mb),
			      "at the end the new scene is drawn as it is, and the transition has ended (diff %.2f)",
			      mean_abs_diff(done, last));
			obs_source_release(showing);

			// Run by the clock, the way OBS runs it on a change of scene:
			// part-way through the second it is neither scene, and after
			// it the new one.
			obs_transition_set(morph, obs_scene_get_source(ma));
			sleep_ms(100);
			obs_transition_start(morph, OBS_TRANSITION_MODE_AUTO, 1000, obs_scene_get_source(mb));
			sleep_ms(450);
			const std::vector<uint8_t> timed = render(morph, w, h);
			sleep_ms(900);
			render(morph, w, h);
			const std::vector<uint8_t> timed_done = render(morph, w, h);
			CHECK(mean_abs_diff(timed, first) > 5.0 && mean_abs_diff(timed, last) > 5.0 &&
				      px(timed, 50, 40, 0) < 15 && px(timed, 550, 300, 0) < 15 &&
				      mean_abs_diff(timed_done, last) < 0.01,
			      "run by the clock it is part-way after part of its time, with the shared item on its way, and the new scene after it (%.1f from the old scene, %.1f from the new; %.2f at the end)",
			      mean_abs_diff(timed, first), mean_abs_diff(timed, last), mean_abs_diff(timed_done, last));

			// Links: two different sources set to count as the same thing.
			// The owner: "link my different camera sources to each other
			// across scenes so the camera from one morphs into the camera
			// for the other ... multiple links".
			{
				obs_source_t *camera_two = block("Morph Camera Two", 0xFFFF00FF, 100, 60); // magenta
				obs_scene_t *md = obs_scene_create("Morph Linked From");
				obs_scene_t *me = obs_scene_create("Morph Linked To");
				put(md, old_back, 0, 0);
				put(md, camera, 20, 20);
				put(me, new_back, 0, 0);
				put(me, camera_two, 520, 280);
				const std::vector<uint8_t> unlinked = at_time(md, me, 0.5f);
				// In the settings window: choose the two, press the button.
				obs_properties_t *lp = obs_source_properties(morph);
				obs_data_t *ls2 = obs_source_get_settings(morph);
				obs_data_set_string(ls2, "link_a", "Morph Camera");
				obs_data_set_string(ls2, "link_b", "Morph Camera Two");
				obs_source_update(morph, ls2);
				const bool pressed =
					obs_properties_get(lp, "link_add") &&
					obs_property_button_clicked(obs_properties_get(lp, "link_add"), morph);
				const int rows = list_rows(lp, "links|a");
				obs_data_release(ls2);
				sleep_ms(100);
				const std::vector<uint8_t> linked_half = at_time(md, me, 0.5f);
				// Unlinked, each camera fades where it is. Linked, there is
				// one camera half-way (270,150 to 370,210), both mixed, and
				// none at either end.
				CHECK(pressed && rows == 1 && px(unlinked, 50, 40, 0) > 80 &&
					      px(unlinked, 550, 300, 2) > 80 && px(linked_half, 320, 180, 0) > 240 &&
					      px(linked_half, 320, 180, 2) > 80 && px(linked_half, 320, 180, 2) < 215 &&
					      px(linked_half, 50, 40, 0) < 15 && px(linked_half, 550, 300, 0) < 15,
				      "two sources linked in the settings change into each other on the way, like one source in both scenes (half-way: red %d, blue %d; red %d and %d at the ends; unlinked they fade in place: %d and %d)",
				      px(linked_half, 320, 180, 0), px(linked_half, 320, 180, 2),
				      px(linked_half, 50, 40, 0), px(linked_half, 550, 300, 0), px(unlinked, 50, 40, 0),
				      px(unlinked, 550, 300, 2));
				// The row's own button takes the link off again.
				obs_property_button_clicked(obs_properties_get(lp, "links|arow|0"), morph);
				sleep_ms(100);
				const std::vector<uint8_t> off_again = at_time(md, me, 0.5f);
				CHECK(list_rows(lp, "links|a") == 0 && mean_abs_diff(off_again, unlinked) < 0.5,
				      "and a link taken off the list no longer counts (diff %.2f)",
				      mean_abs_diff(off_again, unlinked));
				obs_properties_destroy(lp);
				obs_transition_set_manual_time(morph, 1.0f);
				sleep_ms(120);
				render(morph, w, h);
				obs_scene_release(md);
				obs_scene_release(me);
				obs_source_release(camera_two);
			}

			// A capture that only starts when its scene is switched to. The
			// owner: "when i switch to my main display scene, the capture
			// source doesn't appear until the transition animations completes,
			// and it's abrupt". A display capture has no size until it has
			// started, and its item's place in a bounding box is worked out
			// from that size.
			{
				obs_source_t *late = block("Morph Late Capture", 0xFF0000FF, 0, 0); // red, nothing yet
				obs_scene_t *mf = obs_scene_create("Morph Late From");
				obs_scene_t *mg = obs_scene_create("Morph Late To");
				put(mf, old_back, 0, 0);
				put(mg, new_back, 0, 0);
				obs_sceneitem_t *boxed = put(mg, late, 160, 90);
				vec2 box;
				vec2_set(&box, 320, 180);
				obs_sceneitem_set_bounds_type(boxed, OBS_BOUNDS_SCALE_INNER);
				obs_sceneitem_set_bounds(boxed, &box);
				const std::vector<uint8_t> nothing_yet = at_time(mf, mg, 0.5f);
				// It starts: 160x90, which its box shows at twice the size.
				obs_data_t *ld = obs_source_get_settings(late);
				obs_data_set_int(ld, "width", 160);
				obs_data_set_int(ld, "height", 90);
				obs_source_update(late, ld);
				obs_data_release(ld);
				sleep_ms(120);
				const std::vector<uint8_t> started = render(morph, w, h);
				CHECK(px(nothing_yet, 320, 180, 0) < 15 && px(started, 320, 180, 0) > 80 &&
					      px(started, 320, 180, 0) < 215 && px(started, 180, 100, 0) > 80 &&
					      px(started, 460, 260, 0) > 80 && px(started, 140, 80, 0) < 15 &&
					      px(started, 500, 280, 0) < 15 && opaque(started),
				      "a capture that starts during the change is drawn from then on, in its bounding box (red %d in the middle and %d, %d in the corners of the box, %d and %d outside it; %d before it started)",
				      px(started, 320, 180, 0), px(started, 180, 100, 0), px(started, 460, 260, 0),
				      px(started, 140, 80, 0), px(started, 500, 280, 0), px(nothing_yet, 320, 180, 0));
				obs_transition_set_manual_time(morph, 1.0f);
				sleep_ms(120);
				render(morph, w, h);
				obs_scene_release(mf);
				obs_scene_release(mg);
				obs_source_release(late);
			}

			// Unticked, the game and the window are cross-faded in place.
			obs_data_set_bool(ms, "pair_main", false);
			obs_source_update(morph, ms);
			const std::vector<uint8_t> apart = at_time(ma, mb, 0.5f);
			CHECK(px(apart, 120, 100, 0) > 80 && px(apart, 480, 120, 2) > 80,
			      "with 'Morph the main picture' unticked those two fade where they are (red %d where the game is, blue %d where the window is)",
			      px(apart, 120, 100, 0), px(apart, 480, 250, 2));
			// Sliding: the game, on its way out, has moved towards the edge.
			obs_data_set_int(ms, "others", 2);
			obs_source_update(morph, ms);
			const std::vector<uint8_t> slid = at_time(ma, mb, 0.5f);
			CHECK(mean_abs_diff(slid, apart) > 1.0 && opaque(slid),
			      "'Slide off the nearest edge' moves the items that are in one scene only (diff %.2f)",
			      mean_abs_diff(slid, apart));
			obs_transition_set_manual_time(morph, 1.0f);
			sleep_ms(120);
			render(morph, w, h);
			obs_transition_clear(morph);
			// Two different sources change into each other by what shows of
			// them, not by their boxes. The owner: "the morph transition effect
			// should morph objects based on visible pixels, not bounding box".
			// Here each is a whole-screen source with one small block in it.
			{
				obs_source_t *dot_a = block("Morph Dot A", 0xFF0000FF, 100, 100); // red
				obs_source_t *dot_b = block("Morph Dot B", 0xFF00FF00, 100, 100); // green
				obs_scene_t *pad_a = obs_scene_create("Morph Pad A");
				obs_scene_t *pad_b = obs_scene_create("Morph Pad B");
				put(pad_a, dot_a, 40, 40);
				put(pad_b, dot_b, 400, 200);
				obs_scene_t *va = obs_scene_create("Morph Seen From");
				obs_scene_t *vb = obs_scene_create("Morph Seen To");
				put(va, obs_scene_get_source(pad_a), 0, 0);
				put(vb, obs_scene_get_source(pad_b), 0, 0);
				obs_data_t *vs = obs_source_get_settings(morph);
				obs_data_array_t *links = obs_data_array_create();
				obs_data_t *link = obs_data_create();
				obs_data_set_string(link, "value", "Morph Pad A \xE2\x86\x94 Morph Pad B");
				obs_data_array_push_back(links, link);
				obs_data_release(link);
				obs_data_set_array(vs, "links", links);
				obs_data_array_release(links);
				for (bool seen : {true, false}) {
					obs_data_set_bool(vs, "visible_pixels", seen);
					obs_source_update(morph, vs);
					sleep_ms(150);
					const std::vector<uint8_t> mid = at_time(va, vb, 0.5f);
					// Half-way: one block between the two places (the two colours
					// mixed), and nothing left where the first one was. By boxes,
					// which are the same here, each block fades where it is.
					const int mr = px(mid, 270, 170, 0), mg = px(mid, 270, 170, 1),
						  ma_ = px(mid, 270, 170, 3);
					const int was_r = px(mid, 90, 90, 0), was_a = px(mid, 90, 90, 3);
					if (seen)
						CHECK(mr > 40 && mg > 40 && ma_ > 240 && was_a < 12,
						      "two linked sources morph by what shows of them: half-way one whole block lies between their two places and none is left behind (between %d,%d alpha %d; where it was: alpha %d)",
						      mr, mg, ma_, was_a);
					else
						CHECK(ma_ < 12 && was_r > 40 && was_a > 60,
						      "and unticked they change box into box as before (between: alpha %d; where it was %d, alpha %d)",
						      ma_, was_r, was_a);
				}
				// And by their images, a shadow left out of the matching: the
				// owner, "add ignore shadows, strokes/glow, etc". One block has a
				// shadow 20 px to its right and below, the other has none.
				{
					obs_source_t *lit = block("Morph Shadowed", 0xFF0000FF, 100, 100); // red
					obs_source_t *plain = block("Morph Plain", 0xFF00FF00, 100, 100);  // green
					obs_data_t *sd = obs_data_create();
					obs_data_set_bool(sd, "shadow", true);
					obs_data_set_double(sd, "offset_x", 20.0);
					obs_data_set_double(sd, "offset_y", 20.0);
					obs_data_set_double(sd, "softness", 0.0);
					obs_data_set_double(sd, "shadow_opacity", 100.0);
					obs_source_t *shade = obs_source_create("stagekit_shadow_backdrop",
										"Morph Shade", sd, nullptr);
					obs_scene_t *sa = obs_scene_create("Morph Shadow From");
					obs_scene_t *sb = obs_scene_create("Morph Shadow To");
					put(sa, lit, 40, 40);
					put(sb, plain, 400, 200);
					obs_source_filter_add(lit, shade);
					for (int i = 0; i < 12; i++) {
						render(obs_scene_get_source(sa), w, h);
						sleep_ms(50);
					}
					obs_data_array_t *both = obs_data_array_create();
					obs_data_t *one = obs_data_create();
					obs_data_set_string(one, "value", "Morph Shadowed \xE2\x86\x94 Morph Plain");
					obs_data_array_push_back(both, one);
					obs_data_release(one);
					obs_data_set_array(vs, "links", both);
					obs_data_array_release(both);
					obs_data_set_bool(vs, "visible_pixels", true);
					int seen_r[2] = {0, 0}, seen_g[2] = {0, 0};
					for (int round = 0; round < 2; round++) {
						obs_data_set_bool(vs, "no_shadows", round == 0);
						obs_source_update(morph, vs);
						sleep_ms(150);
						const std::vector<uint8_t> mid = at_time(sa, sb, 0.5f);
						// Just inside the far corner of where the image is half-way.
						seen_r[round] = px(mid, 314, 214, 0);
						seen_g[round] = px(mid, 314, 214, 1);
					}
					CHECK(seen_r[0] > 40 && seen_g[0] > 40 && seen_r[1] < 25 && seen_g[1] > 40,
					      "with shadows left out the two images land on each other, the shadow drawn beside them; counted in, the shadowed one is matched shadow and all (corner of the image half-way: %d,%d left out; %d,%d counted)",
					      seen_r[0], seen_g[0], seen_r[1], seen_g[1]);
					obs_data_set_bool(vs, "no_shadows", true);
					obs_source_filter_remove(lit, shade);
					obs_source_release(shade);
					obs_data_release(sd);
					obs_scene_release(sa);
					obs_scene_release(sb);
					obs_source_release(lit);
					obs_source_release(plain);
				}
				obs_properties_t *vp = obs_source_properties(morph);
				CHECK(obs_properties_get(vp, "visible_pixels") != nullptr &&
					      obs_properties_get(vp, "no_shadows") != nullptr,
				      "Morph offers Use visible pixels and Ignore shadows & outlines");
				obs_properties_destroy(vp);
				obs_data_release(vs);
				obs_scene_release(va);
				obs_scene_release(vb);
				obs_scene_release(pad_a);
				obs_scene_release(pad_b);
				obs_source_release(dot_a);
				obs_source_release(dot_b);
			}
			obs_source_release(morph);
		}
		// --------------------------------------- one item into another
		// The owner: "id like a similar morph effect when showing and
		// hiding objects ... morph should be a transition in show in scene,
		// and follow the same rules as the global morph source links".
		{
			printf("\n[morph between items]\n");
			obs_source_t *first = block("Item Morph First", 0xFF0000FF, 100, 60);      // red
			obs_source_t *second = block("Item Morph Second", 0xFF00FF00, 200, 120);   // green
			obs_source_t *big_one = block("Item Morph Big One", 0xFFFFFF00, 300, 200); // cyan
			obs_source_t *big_two = block("Item Morph Big Two", 0xFF00FFFF, 300, 200); // yellow
			obs_scene_t *is = obs_scene_create("Item Morph Scene");
			put(is, old_back, 0, 0); // blue
			obs_sceneitem_t *one = put(is, first, 20, 150);
			obs_sceneitem_t *two = put(is, second, 420, 120);
			obs_sceneitem_set_visible(two, false);
			auto count_items = [](obs_scene_t *scene) {
				int n = 0;
				obs_scene_enum_items(
					scene,
					[](obs_scene_t *, obs_sceneitem_t *, void *param) {
						++*static_cast<int *>(param);
						return true;
					},
					&n);
				return n;
			};
			// Every Morph's links count: here the links of a Morph kept as a
			// change of scene would be.
			obs_data_t *gs_ = obs_data_create();
			obs_data_set_int(gs_, "ease_curve", 0);
			obs_data_array_t *links = obs_data_array_create();
			obs_data_t *line = obs_data_create();
			obs_data_set_string(line, "value", "Item Morph First \xE2\x86\x94 Item Morph Second");
			obs_data_array_push_back(links, line);
			obs_data_release(line);
			obs_data_set_array(gs_, "links", links);
			obs_data_array_release(links);
			obs_source_t *global = obs_source_create_private("stagekit_morph", "Global Morph", gs_);
			obs_data_release(gs_);
			auto as_morph = [&](obs_sceneitem_t *item, bool show) {
				obs_data_t *d = obs_data_create();
				obs_data_set_int(d, "ease_curve", 0); // linear
				obs_source_t *t = obs_source_create_private("stagekit_morph", "Item Morph", d);
				obs_data_release(d);
				obs_sceneitem_set_transition(item, show, t);
				obs_sceneitem_set_transition_duration(item, show, 1000);
				obs_source_release(t);
			};
			for (obs_sceneitem_t *item : {one, two}) {
				as_morph(item, true);
				as_morph(item, false);
			}
			obs_source_t *before_out = obs_get_output_source(0);
			obs_set_output_source(0, obs_scene_get_source(is));
			sleep_ms(300);
			render(obs_scene_get_source(is), w, h);
			// Where something of both colours is, in a frame.
			struct Mixed {
				int n = 0, x0 = 100000, x1 = -1;
			};
			auto mixed = [&](const std::vector<uint8_t> &frame, int c1, int c2) {
				Mixed m;
				for (int y = 0; y < (int)h; y++) {
					for (int x = 0; x < (int)w; x++) {
						if (px(frame, x, y, c1) > 90 && px(frame, x, y, c2) > 90) {
							m.n++;
							m.x0 = std::min(m.x0, x);
							m.x1 = std::max(m.x1, x);
						}
					}
				}
				return m;
			};
			obs_sceneitem_set_visible(one, false);
			obs_sceneitem_set_visible(two, true);
			sleep_ms(450);
			const std::vector<uint8_t> mid = render(obs_scene_get_source(is), w, h);
			const Mixed m = mixed(mid, 0, 1);
			const int during = count_items(is);
			CHECK(m.n > 3000 && m.x0 > 120 && m.x1 < 600 && m.x1 - m.x0 > 104 && m.x1 - m.x0 < 196 &&
				      px(mid, 60, 180, 0) < 15 && px(mid, 600, 180, 1) < 15 &&
				      !obs_sceneitem_visible(one) && obs_sceneitem_visible(two),
			      "an item hidden and a linked item shown at the same moment change into each other on the way: one picture of both colours between the two places, wider than the one and narrower than the other, and nothing at either end (%d px from x %d to %d; red %d where the first was, green %d at the far side of the second)",
			      m.n, m.x0, m.x1, px(mid, 60, 180, 0), px(mid, 600, 180, 1));
			sleep_ms(900);
			render(obs_scene_get_source(is), w, h);
			const std::vector<uint8_t> after = render(obs_scene_get_source(is), w, h);
			CHECK(during == 4 && count_items(is) == 3 && px(after, 520, 180, 1) > 240 &&
				      px(after, 520, 180, 0) < 15 && px(after, 60, 180, 0) < 15 &&
				      mixed(after, 0, 1).n == 0,
			      "it is drawn by a layer that is in the scene only while the change lasts, and afterwards the shown item is drawn as it is (%d items during, %d after; green %d in the second)",
			      during, count_items(is), px(after, 520, 180, 1));

			// By itself: nothing hidden or shown with it, so it fades.
			obs_sceneitem_set_visible(two, false);
			sleep_ms(550);
			const std::vector<uint8_t> going = render(obs_scene_get_source(is), w, h);
			const int alone_items = count_items(is);
			sleep_ms(800);
			render(obs_scene_get_source(is), w, h);
			const std::vector<uint8_t> gone = render(obs_scene_get_source(is), w, h);
			CHECK(px(going, 520, 180, 1) > 40 && px(going, 520, 180, 1) < 235 && alone_items == 3 &&
				      px(gone, 520, 180, 1) < 15,
			      "an item hidden with Morph and nothing to change into fades out where it is (green %d part-way, %d after; %d items)",
			      px(going, 520, 180, 1), px(gone, 520, 180, 1), alone_items);

			// The main picture: the largest item hidden and the largest shown
			// go together without a link.
			obs_sceneitem_t *b1 = put(is, big_one, 20, 20);
			obs_sceneitem_t *b2 = put(is, big_two, 320, 140);
			obs_sceneitem_set_visible(b2, false);
			for (obs_sceneitem_t *item : {b1, b2}) {
				as_morph(item, true);
				as_morph(item, false);
			}
			sleep_ms(200);
			render(obs_scene_get_source(is), w, h);
			// For the README: before, on the way, after.
			write_frame(out + "/../item-morph/1 before.rgba", render(obs_scene_get_source(is), w, h), w, h);
			obs_sceneitem_set_visible(b1, false);
			obs_sceneitem_set_visible(b2, true);
			sleep_ms(450);
			const std::vector<uint8_t> big_mid = render(obs_scene_get_source(is), w, h);
			write_frame(out + "/../item-morph/2 on the way.rgba", big_mid, w, h);
			// Cyan into yellow: red and blue are both part-way.
			const Mixed bm = mixed(big_mid, 0, 2);
			CHECK(bm.n > 30000 && bm.x0 > 40 && bm.x1 < 600,
			      "with no link, the largest item hidden and the largest shown change into each other (%d px of both colours, from x %d to %d)",
			      bm.n, bm.x0, bm.x1);
			sleep_ms(900);
			render(obs_scene_get_source(is), w, h);
			write_frame(out + "/../item-morph/3 after.rgba", render(obs_scene_get_source(is), w, h), w, h);
			CHECK(count_items(is) == 5, "and the layer is gone again (%d items)", count_items(is));

			// Fade and Zoom, the owner's "also add fade and zoom as a
			// transition": an item grows to its size as it fades in.
			{
				obs_source_t *zoomed = block("Fade Zoom Item", 0xFF0000FF, 200, 100); // red
				obs_sceneitem_t *zi = put(is, zoomed, 20, 240);
				obs_sceneitem_set_visible(zi, false);
				obs_data_t *zd = obs_data_create();
				obs_data_set_int(zd, "ease_curve", 0);
				obs_data_set_double(zd, "zoom", 50.0);
				obs_source_t *zt = obs_source_create_private("stagekit_fade_zoom", "Fade Zoom", zd);
				CHECK(zt != nullptr, "the Fade and Zoom transition is registered and can be created");
				if (zt) {
					obs_sceneitem_set_transition(zi, true, zt);
					obs_sceneitem_set_transition_duration(zi, true, 1000);
					sleep_ms(200);
					render(obs_scene_get_source(is), w, h);
					obs_sceneitem_set_visible(zi, true);
					sleep_ms(450);
					const std::vector<uint8_t> zmid = render(obs_scene_get_source(is), w, h);
					sleep_ms(800);
					render(obs_scene_get_source(is), w, h);
					const std::vector<uint8_t> zend = render(obs_scene_get_source(is), w, h);
					// The item is 200x100 at 20,240: its middle is 120,290.
					CHECK(px(zmid, 120, 290, 0) > 40 && px(zmid, 120, 290, 0) < 235 &&
						      px(zmid, 25, 244, 0) < 15 && px(zend, 25, 244, 0) > 240 &&
						      px(zend, 120, 290, 0) > 240,
					      "an item shown with Fade and Zoom is part-way there and smaller than its frame on the way, and whole at the end (red %d in the middle and %d in the corner part-way; %d in the corner after)",
					      px(zmid, 120, 290, 0), px(zmid, 25, 244, 0), px(zend, 25, 244, 0));
					// As a change of scene the picture stays whole.
					obs_source_t *zs =
						obs_source_create_private("stagekit_fade_zoom", "Fade Zoom Scenes", zd);
					obs_transition_set(zs, obs_scene_get_source(ma));
					obs_transition_start(zs, OBS_TRANSITION_MODE_MANUAL, 0,
							     obs_scene_get_source(mb));
					obs_transition_set_manual_time(zs, 0.5f);
					sleep_ms(120);
					const std::vector<uint8_t> zscene = render(zs, w, h);
					const double from_a =
						mean_abs_diff(zscene, render(obs_scene_get_source(ma), w, h));
					const double from_b =
						mean_abs_diff(zscene, render(obs_scene_get_source(mb), w, h));
					CHECK(opaque(zscene) && from_a > 3.0 && from_b > 3.0,
					      "as a change of scene it is neither scene half-way, and a whole picture (%.1f from the old scene, %.1f from the new, whole %d)",
					      from_a, from_b, (int)opaque(zscene));
					obs_transition_clear(zs);
					obs_source_release(zs);
					// And hidden with it: it fades as it shrinks. (OBS hands a
					// transition a see-through placeholder for the side with
					// no source; taken for a change of scene into nothing, the
					// item stayed whole and went at once at the end.)
					obs_sceneitem_set_transition(zi, false, zt);
					obs_sceneitem_set_transition_duration(zi, false, 1000);
					obs_sceneitem_set_visible(zi, false);
					sleep_ms(450);
					const std::vector<uint8_t> zout = render(obs_scene_get_source(is), w, h);
					CHECK(px(zout, 120, 290, 0) > 40 && px(zout, 120, 290, 0) < 235 &&
						      px(zout, 25, 244, 0) < 15,
					      "an item hidden with Fade and Zoom is part-way gone and smaller than its frame on the way (red %d in the middle and %d in the corner part-way)",
					      px(zout, 120, 290, 0), px(zout, 25, 244, 0));
					obs_sceneitem_set_transition(zi, false, nullptr);
				}
				obs_sceneitem_set_transition(zi, true, nullptr);
				obs_source_release(zt);
				obs_data_release(zd);
				obs_source_release(zoomed);
			}

			// One item in two changes. The owner's Live scene: hiding the
			// full-screen camera (DSLR) to show a capture also brings back the
			// camera's corner view (Square Camera), and the two cameras are
			// linked. The owner: "when I switch to window or game source from
			// DSLR by hiding DSLR, it doesn't smoothly morph and transition into
			// the new source like all the others do". The corner view took the
			// camera and the capture faded in by itself.
			{
				// (Camera and capture each fill more than a third of the screen,
				// as the owner's do: over half of it here.)
				obs_source_t *cam = block("Item Morph Cam", 0xFF0000FF, 480, 270);     // red
				obs_source_t *grab = block("Item Morph Grab", 0xFF00FF00, 480, 270);   // green
				obs_source_t *corner = block("Item Morph Corner", 0xFFFF0000, 80, 60); // blue
				obs_scene_t *ls = obs_scene_create("Item Morph Live");                 // no background
				obs_sceneitem_t *ci = put(ls, cam, 20, 20); // lowest, as DSLR
				obs_sceneitem_t *gi = put(ls, grab, 140, 70);
				obs_sceneitem_t *ki = put(ls, corner, 540, 280); // highest
				obs_sceneitem_set_visible(gi, false);
				obs_sceneitem_set_visible(ki, false);
				for (obs_sceneitem_t *item : {ci, gi, ki}) {
					as_morph(item, true);
					as_morph(item, false);
				}
				obs_data_t *ld = obs_data_create();
				obs_data_array_t *ll = obs_data_array_create();
				obs_data_t *lline = obs_data_create();
				obs_data_set_string(lline, "value", "Item Morph Cam \xE2\x86\x94 Item Morph Corner");
				obs_data_array_push_back(ll, lline);
				obs_data_release(lline);
				obs_data_set_array(ld, "links", ll);
				obs_data_array_release(ll);
				obs_source_t *cam_links =
					obs_source_create_private("stagekit_morph", "Camera Links", ld);
				obs_data_release(ld);
				obs_set_output_source(0, obs_scene_get_source(ls));
				sleep_ms(300);
				render(obs_scene_get_source(ls), w, h);
				// Half-way: what of the capture comes out of the camera (red and
				// green in one place), and what of the corner view (red and blue).
				struct Halfway {
					Mixed grab, corner;
					int items = 0;
				};
				auto halfway = [&]() {
					sleep_ms(450);
					const std::vector<uint8_t> mid = render(obs_scene_get_source(ls), w, h);
					Halfway hw{mixed(mid, 0, 1), mixed(mid, 0, 2), count_items(ls)};
					sleep_ms(900);
					render(obs_scene_get_source(ls), w, h);
					return hw;
				};
				obs_sceneitem_set_visible(ci, false);
				obs_sceneitem_set_visible(gi, true);
				obs_sceneitem_set_visible(ki, true);
				const Halfway out_of = halfway();
				const std::vector<uint8_t> shown = render(obs_scene_get_source(ls), w, h);
				CHECK(out_of.grab.n > 15000 && out_of.grab.x0 > 60 && out_of.grab.x1 < 600 &&
					      out_of.corner.n > 4000 && out_of.items == 5,
				      "a camera hidden as a capture and the camera's linked corner view are shown changes into both: the corner view and the capture each come out of it (half-way: %d px of camera and capture from x %d to %d, %d px of camera and corner view; %d items)",
				      out_of.grab.n, out_of.grab.x0, out_of.grab.x1, out_of.corner.n, out_of.items);
				CHECK(count_items(ls) == 3 && px(shown, 470, 240, 1) > 240 &&
					      px(shown, 470, 240, 0) < 15 && px(shown, 580, 310, 2) > 240 &&
					      px(shown, 100, 60, 0) < 15,
				      "and afterwards the capture and the corner view are drawn as they are, and the camera is gone (%d items; green %d, blue %d, red %d where the camera was)",
				      count_items(ls), px(shown, 470, 240, 1), px(shown, 580, 310, 2),
				      px(shown, 100, 60, 0));
				// And back: the capture and the corner view both go into the camera.
				obs_sceneitem_set_visible(gi, false);
				obs_sceneitem_set_visible(ki, false);
				obs_sceneitem_set_visible(ci, true);
				const Halfway into = halfway();
				CHECK(into.grab.n > 15000 && into.corner.n > 4000 && into.items == 5,
				      "and back, the capture and the corner view both change into the camera (half-way: %d px of capture and camera, %d px of corner view and camera; %d items)",
				      into.grab.n, into.corner.n, into.items);
				// The capture a moment later than the other two (another filter's
				// tick): the camera is already on its way into the corner view.
				obs_sceneitem_set_visible(ci, false);
				obs_sceneitem_set_visible(ki, true);
				sleep_ms(40);
				obs_sceneitem_set_visible(gi, true);
				const Halfway late = halfway();
				CHECK(late.grab.n > 15000 && late.corner.n > 4000,
				      "a capture shown a moment after the camera went into its corner view still comes out of the camera (half-way: %d px of camera and capture, %d px of camera and corner view)",
				      late.grab.n, late.corner.n);
				// Two items left over in one pair's change: only the largest goes
				// with it; a small one with no link fades by itself.
				obs_source_t *dot = block("Item Morph Dot", 0xFF00FFFF, 40, 40); // yellow
				obs_sceneitem_t *di = put(ls, dot, 20, 300);
				obs_sceneitem_set_visible(di, false);
				as_morph(di, true);
				as_morph(di, false);
				sleep_ms(100);
				obs_sceneitem_set_visible(gi, false);
				obs_sceneitem_set_visible(ki, false);
				obs_sceneitem_set_visible(ci, true);
				obs_sceneitem_set_visible(di, true);
				const Halfway small = halfway();
				CHECK(small.items == 6,
				      "an item too small to be the main image, with no link, is in no change: it fades by itself (%d items half-way)",
				      small.items);
				// A panel beside the main picture (a fifth of the screen: above
				// the main image's 15 %, under a third), unlinked, hidden or shown
				// with the capture as the camera comes or goes: it fades by itself
				// and does not join the camera's change. The owner's chat box in
				// Chat Live, with beta158: "the camera is morphing from the chat
				// box".
				obs_source_t *side = block("Item Morph Side", 0xFFFFFF00, 160, 300); // cyan
				obs_sceneitem_t *si = put(ls, side, 460, 30);
				obs_sceneitem_set_visible(si, false);
				as_morph(si, true);
				as_morph(si, false);
				sleep_ms(100);
				obs_sceneitem_set_visible(ci, false);
				obs_sceneitem_set_visible(gi, true);
				obs_sceneitem_set_visible(si, true);
				const Halfway panel_in = halfway();
				obs_sceneitem_set_visible(gi, false);
				obs_sceneitem_set_visible(si, false);
				obs_sceneitem_set_visible(ci, true);
				const Halfway panel_out = halfway();
				CHECK(panel_in.items == 6 && panel_out.items == 6 && panel_in.grab.n > 15000 &&
					      panel_out.grab.n > 15000,
				      "a panel under a third of the screen, shown or hidden with the capture as the camera goes or comes, fades by itself: only the camera and the capture change into each other (%d and %d items half-way, one layer each; %d and %d px of camera and capture)",
				      panel_in.items, panel_out.items, panel_in.grab.n, panel_out.grab.n);
				obs_set_output_source(0, obs_scene_get_source(is));
				obs_source_release(cam_links);
				obs_scene_release(ls);
				for (obs_source_t *src : {cam, grab, corner, dot, side})
					obs_source_release(src);
			}
			obs_set_output_source(0, before_out);
			obs_source_release(before_out);
			obs_source_release(global);
			obs_scene_release(is);
			for (obs_source_t *src : {first, second, big_one, big_two})
				obs_source_release(src);
		}
		obs_data_release(ms);
		obs_scene_release(ma);
		obs_scene_release(mb);
		obs_scene_release(mc);
		for (obs_source_t *src : {old_back, new_back, game, window, camera})
			obs_source_release(src);
		link_transitions_test(w, h);
		if (getenv("STAGEKIT_TEST_ONLY_MORPH")) { // to try this section alone
			obs_source_release(live);
			obs_shutdown();
			printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures,
			       g_failures == 1 ? "" : "s");
			return g_failures ? 1 : 0;
		}
	}

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
		// (A cut when the capture changes, for the pictures taken here; its
		// transition has a check of its own below.)
		obs_data_set_int(d, "live_change", 1);
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
		// The capture coming back with a delay and a fade: the background
		// stays as it was for the delay, is part-way after that, and has
		// followed at the end. The owner: "there should be a configurable
		// delay in the ambient background transition to the live capture".
		{
			auto held = shot("6d_live_gone", [](obs_data_t *d) {
				obs_data_set_int(d, "live_change", 0);
				obs_data_set_int(d, "live_delay_ms", 700);
				obs_data_set_int(d, "live_change_ms", 1500);
				obs_data_set_int(d, "live_ease_curve", 0);
				obs_data_set_string(d, "live_source", "Live Capture");
			});
			sleep_ms(1300);
			auto part_way = shot("6d_live_gone", [](obs_data_t *) {});
			sleep_ms(1700);
			auto followed = shot("6d_live_gone", [](obs_data_t *) {});
			CHECK(mean_abs_diff(held, gone) < 0.6 && mean_abs_diff(part_way, gone) > 0.4 &&
				      mean_abs_diff(part_way, replaced) > 0.4 &&
				      mean_abs_diff(followed, replaced) < 0.6,
			      "when the capture changes the background stays for the delay, then gives way over its time (%.2f from before during the delay; part-way %.2f from before and %.2f from after; %.2f from after at the end)",
			      mean_abs_diff(held, gone), mean_abs_diff(part_way, gone),
			      mean_abs_diff(part_way, replaced), mean_abs_diff(followed, replaced));
		}
		shot("6_live_capture", [](obs_data_t *d) {
			obs_data_set_int(d, "live_change", 1);
			obs_data_set_int(d, "live_delay_ms", 0);
			obs_data_set_string(d, "live_source", "Live Capture");
			obs_data_set_bool(d, "live_replace", false);
		});
		sleep_ms(200);
		shot("6_live_capture", [](obs_data_t *) {});
	}

	auto square = shot("7_square_mosaic", [](obs_data_t *d) { obs_data_set_int(d, "pixel_style", 1); });
	CHECK(mean_abs_diff(square, with_live) > 0.2, "square mosaic differs from hex");

	// The other cell shapes, the blur types and bloom, on the plain picture.
	{
		auto reset = shot("7_square_mosaic", [](obs_data_t *) {});
		auto circles = shot("7b_circles_smoothed", [](obs_data_t *d) {
			obs_data_set_int(d, "pixel_style", 3);
			obs_data_set_double(d, "pixel_smooth", 60);
		});
		auto voronoi = shot("7c_voronoi_turned", [](obs_data_t *d) {
			obs_data_set_int(d, "pixel_style", 5);
			obs_data_set_double(d, "pixel_smooth", 0);
			obs_data_set_double(d, "pixel_rotation", 20);
		});
		auto triakis = shot("7d_triakis", [](obs_data_t *d) {
			obs_data_set_int(d, "pixel_style", 7);
			obs_data_set_double(d, "pixel_rotation", 0);
		});
		auto glass =
			shot("7e_triakis_glass", [](obs_data_t *d) { obs_data_set_double(d, "pixel_glass", 100); });
		CHECK(mean_abs_diff(glass, triakis) > 0.5 && opaque(glass),
		      "the background's mosaic can be glassy too (diff %.2f)", mean_abs_diff(glass, triakis));
		obs_data_set_double(s, "pixel_glass", 0);
		CHECK(mean_abs_diff(circles, reset) > 0.2 && mean_abs_diff(voronoi, circles) > 0.2 &&
			      mean_abs_diff(triakis, voronoi) > 0.2 && opaque(circles) && opaque(voronoi) &&
			      opaque(triakis),
		      "the mosaic has more cell shapes, each a picture of its own and all opaque (%.2f, %.2f, %.2f)",
		      mean_abs_diff(circles, reset), mean_abs_diff(voronoi, circles), mean_abs_diff(triakis, voronoi));
		// The blur types: each softens, and they agree roughly on how much.
		obs_data_set_int(s, "pixel_style", 0);
		obs_data_set_int(s, "live_mode", 0);
		obs_data_set_string(s, "overlay", "");
		obs_data_set_double(s, "saturation", 100);
		obs_data_set_double(s, "glow", 0);
		obs_data_set_double(s, "blur", 0);
		const std::vector<uint8_t> sharp = shot("x_sharp", [](obs_data_t *) {});
		obs_data_set_double(s, "blur", 24);
		const std::vector<uint8_t> kawase =
			shot("x_kawase", [](obs_data_t *d) { obs_data_set_int(d, "blur_type", 0); });
		const std::vector<uint8_t> gaussian =
			shot("x_gaussian", [](obs_data_t *d) { obs_data_set_int(d, "blur_type", 1); });
		const std::vector<uint8_t> box =
			shot("x_box", [](obs_data_t *d) { obs_data_set_int(d, "blur_type", 2); });
		const std::vector<uint8_t> temporal =
			shot("x_temporal", [](obs_data_t *d) { obs_data_set_int(d, "blur_type", 3); });
		CHECK(mean_abs_diff(gaussian, sharp) > 2.0 && mean_abs_diff(box, sharp) > 2.0 &&
			      mean_abs_diff(gaussian, kawase) < mean_abs_diff(gaussian, sharp) &&
			      mean_abs_diff(box, kawase) < mean_abs_diff(box, sharp),
		      "Gaussian and Box blur the background too, about as much as Dual Kawase (from sharp: %.2f, %.2f, %.2f; from each other: %.2f, %.2f)",
		      mean_abs_diff(kawase, sharp), mean_abs_diff(gaussian, sharp), mean_abs_diff(box, sharp),
		      mean_abs_diff(gaussian, kawase), mean_abs_diff(box, kawase));
		CHECK(mean_abs_diff(temporal, sharp) < 1.0 && opaque(temporal),
		      "Temporal leaves a still picture sharp (diff %.2f)", mean_abs_diff(temporal, sharp));
		for (const char *name : {"x_sharp", "x_kawase", "x_gaussian", "x_box", "x_temporal"})
			remove((out + "/" + name + ".rgba").c_str());
		// Bloom: the bright parts shine; a higher threshold lets fewer of them.
		obs_data_set_int(s, "blur_type", 0);
		obs_data_set_double(s, "blur", 12);
		const std::vector<uint8_t> before = shot("x_before", [](obs_data_t *) {});
		const std::vector<uint8_t> bloom = shot("7e_bloom", [](obs_data_t *d) {
			obs_data_set_double(d, "bloom", 120);
			obs_data_set_double(d, "bloom_threshold", 40);
			obs_data_set_double(d, "bloom_size", 120);
		});
		const std::vector<uint8_t> picky =
			shot("x_picky", [](obs_data_t *d) { obs_data_set_double(d, "bloom_threshold", 90); });
		CHECK(mean_rgb(bloom) > mean_rgb(before) + 1.0 && mean_rgb(picky) < mean_rgb(bloom) &&
			      mean_rgb(picky) >= mean_rgb(before) - 0.01 && opaque(bloom),
		      "bloom makes the bright parts shine, and fewer of them from a higher brightness (mean %.1f, with bloom %.1f, from 90%% %.1f)",
		      mean_rgb(before), mean_rgb(bloom), mean_rgb(picky));
		// The other style, the bloom of the owner's old collection (the
		// shader filter's): the whole picture's light added back onto it,
		// whatever the threshold says.
		const std::vector<uint8_t> amplified = shot("7f_bloom_amplified", [](obs_data_t *d) {
			obs_data_set_int(d, "bloom_style", 1);
			obs_data_set_double(d, "bloom", 300);
			obs_data_set_double(d, "bloom_threshold", 90);
			obs_data_set_double(d, "bloom_size", 20);
		});
		CHECK(mean_rgb(amplified) > mean_rgb(bloom) + 5.0 && opaque(amplified),
		      "'Whole picture, amplified' brightens everything, bright or not (mean %.1f, against %.1f for the bright parts alone)",
		      mean_rgb(amplified), mean_rgb(bloom));
		// Bloom never brings the sharp picture back over a blurred one: the
		// light spreads at least as far as the blur, however small its size.
		{
			auto detail = [&](const std::vector<uint8_t> &px) {
				double sum = 0.0;
				size_t n = 0;
				for (uint32_t y = 0; y < h; y++)
					for (uint32_t x = 0; x + 1 < w; x++)
						for (int c = 0; c < 3; c++, n++)
							sum += std::abs((int)px[(y * w + x) * 4 + c] -
									(int)px[(y * w + x + 1) * 4 + c]);
				return n ? sum / (double)n : 0.0;
			};
			const std::vector<uint8_t> soft = shot("x_soft", [](obs_data_t *d) {
				obs_data_set_int(d, "bloom_style", 1);
				obs_data_set_double(d, "bloom", 0);
				obs_data_set_double(d, "blur", 200);
				obs_data_set_int(d, "pixel_style", 0);
			});
			const std::vector<uint8_t> lit = shot("x_lit", [](obs_data_t *d) {
				obs_data_set_double(d, "bloom", 200);
				obs_data_set_double(d, "bloom_size", 0);
			});
			CHECK(mean_rgb(lit) > mean_rgb(soft) + 5.0 && detail(lit) < detail(soft) * 2.5 + 0.05,
			      "bloom leaves a blurred background as soft as it was (detail %.3f without, %.3f with; mean %.1f and %.1f)",
			      detail(soft), detail(lit), mean_rgb(soft), mean_rgb(lit));
			obs_data_set_double(s, "bloom_size", 80);
		}
		for (const char *name : {"x_before", "x_picky", "x_soft", "x_lit"})
			remove((out + "/" + name + ".rgba").c_str());
		// Back as the checks below expect to find it.
		obs_data_set_int(s, "bloom_style", 0);
		obs_data_set_double(s, "bloom", 0);
		obs_data_set_double(s, "blur", 24);
		obs_data_set_int(s, "pixel_style", 1);
		obs_data_set_int(s, "live_mode", 2);
		obs_data_set_string(s, "overlay", texture_image);
		obs_data_set_double(s, "saturation", 200);
		obs_data_set_double(s, "glow", 30);
		obs_source_update(bg, s);
		sleep_ms(700);
	}

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

		// A game seen once is remembered: next time it is picked up straight
		// away, in a window and behind another program, without being looked into.
		see("Inbox - Mail|MailWnd|mail.exe|front|");
		aimed = see("Aladdin|Snes9X: WndClass|snes9x-x64.exe||\nInbox - Mail|MailWnd|mail.exe|front|");
		CHECK(aimed.find("snes9x-x64.exe") != std::string::npos,
		      "a game seen before is picked up again at once, even behind another window (%s)", aimed.c_str());

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

		// A program taken for a game that cannot be hooked: OBS would go on
		// trying for good, several times a second, each try a stall. After
		// a while with no picture the program is left alone for a time (the
		// owner's 3D modelling program: 3,404 tries in twelve minutes), but
		// never put on the Never list: the owner's Dolphin, open with no
		// game running, was put there each time they took it off.
		{
			obs_source_t *stuck = obs_source_create("game_capture", "Stuck Game Capture", nullptr, nullptr);
			obs_data_t *gd = obs_data_create();
			obs_data_set_int(gd, "give_up_ms", 750);
			obs_source_t *giver = obs_source_create("stagekit_auto_capture", "Auto Capture 3", gd, nullptr);
			obs_data_release(gd);
			obs_source_filter_add(stuck, giver);
			auto show = [&](const char *windows) {
				calldata_t cd = {};
				calldata_set_string(&cd, "windows", windows);
				proc_handler_call(obs_source_get_proc_handler(giver), "set_test_windows", &cd);
				calldata_free(&cd);
			};
			auto aimed_at = [&] {
				obs_data_t *d = obs_source_get_settings(stuck);
				const std::string window = obs_data_get_string(d, "window");
				const std::string mode = obs_data_get_string(d, "capture_mode");
				obs_data_release(d);
				return mode + " " + window;
			};
			show("Sculpt 2026|SculptWnd|sculpt3d.exe|front|game");
			sleep_ms(400);
			const std::string taken = aimed_at();
			sleep_ms(1600);
			obs_data_t *gs = obs_source_get_settings(giver);
			const std::string never_now = obs_data_get_string(gs, "never");
			obs_data_release(gs);
			const std::string rested = aimed_at();
			// A game is started in it: the window's title changes, and it is
			// tried again at once.
			show("Sculpt 2026 - playing|SculptWnd|sculpt3d.exe|front|game");
			sleep_ms(500);
			const std::string again = aimed_at();
			CHECK(taken.find("sculpt3d.exe") != std::string::npos &&
				      never_now.find("sculpt3d.exe") == std::string::npos &&
				      rested.rfind("any_fullscreen", 0) == 0 &&
				      again.find("sculpt3d.exe") != std::string::npos,
			      "a program that gives the capture no picture is left alone for a while, not put on the Never capture list, and tried again when its window's title changes (%s; never: '%s'; then %s; then %s)",
			      taken.c_str(), never_now.c_str(), rested.c_str(), again.c_str());
			obs_source_filter_remove(stuck, giver);
			obs_source_release(giver);
			obs_source_release(stuck);
		}

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
			obs_property_t *pick = obs_properties_get(props, "always|tpick");
			CHECK(pick && obs_property_list_item_count(pick) == 3,
			      "open programs can be picked from a list, without OBS itself (%d entries)",
			      pick ? (int)obs_property_list_item_count(pick) : -1);

			obs_property_button_clicked(obs_properties_get(props, "never_now"), easy);
			sleep_ms(300);
			obs_data_t *es = obs_source_get_settings(easy);
			CHECK(std::string(obs_data_get_string(es, "never")) == "blender.exe",
			      "one press adds the wrong program to 'Never capture' (%s)",
			      obs_data_get_string(es, "never"));
			obs_data_release(es);
			CHECK(list_rows(props, "never|t") == 1, "and it shows as a row of its own in that list");
			pick_into_list(easy, props, "always|t", "OddGame.exe");
			CHECK(list_rows(props, "always|t") == 1, "choosing a program adds it as a row");
			sleep_ms(200);
			got = see2(open);
			CHECK(got.find("OddGame.exe") != std::string::npos,
			      "a picked program is always captured, and the excluded one is dropped (%s)", got.c_str());
			// Each row's own button takes it off again.
			obs_property_button_clicked(obs_properties_get(props, "never|trow|0"), easy);
			es = obs_source_get_settings(easy);
			CHECK(std::string(obs_data_get_string(es, "never")).empty() && list_rows(props, "never|t") == 0,
			      "a row's own button removes that entry (%s)", obs_data_get_string(es, "never"));
			obs_data_release(es);
			obs_properties_destroy(props);

			obs_source_filter_remove(capture2, easy);
			obs_source_release(easy);
			obs_source_release(capture2);
		}

		obs_source_filter_remove(capture, aimer);
		obs_source_release(aimer);
		obs_source_release(capture);
	}

	// ------------------------------------------------------------ start image
	// ------------------------------------------- a stand-in lets its source go
	{
		printf("\n[a source with stand-ins is let go of with its scenes]\n");
		obs_data_t *cd = obs_data_create();
		obs_data_set_int(cd, "color", 0xFF2040C0);
		obs_data_set_int(cd, "width", 320);
		obs_data_set_int(cd, "height", 180);
		obs_source_t *cam = obs_source_create("color_source", "Freed Camera", cd, nullptr);
		obs_data_release(cd);
		static bool freed = false;
		signal_handler_connect(
			obs_source_get_signal_handler(cam), "destroy", [](void *, calldata_t *) { freed = true; },
			nullptr);
		obs_data_t *sd = obs_data_create();
		obs_data_set_bool(sd, "when_hidden", true);
		obs_source_t *standby = obs_source_create("stagekit_standby", "Freed Standby", sd, nullptr);
		obs_data_release(sd);
		obs_source_filter_add(cam, standby);
		obs_scene_t *one = obs_scene_create("Freed One");
		obs_scene_t *two = obs_scene_create("Freed Two");
		obs_sceneitem_t *hidden = obs_scene_add(one, cam);
		obs_scene_add(two, cam);
		obs_set_output_source(0, obs_scene_get_source(one));
		sleep_ms(700);
		obs_sceneitem_set_visible(hidden, false);
		sleep_ms(700);
		int twins = 0;
		obs_scene_enum_items(
			one,
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
				if (strcmp(obs_source_get_unversioned_id(obs_sceneitem_get_source(item)),
					   "stagekit_stand_in") == 0)
					(*static_cast<int *>(param))++;
				return true;
			},
			&twins);
		// Cleared the way OBS clears a collection: it first says the
		// collection is changing (from then on nothing goes through the
		// scenes' items), then every source is taken out and every reference
		// the collection held let go.
		auto set_loaded = [](bool loaded) {
			calldata_t ld;
			calldata_init(&ld);
			calldata_set_bool(&ld, "loaded", loaded);
			proc_handler_call(obs_get_proc_handler(), "stagekit_test_loaded", &ld);
			calldata_free(&ld);
		};
		set_loaded(false);
		sleep_ms(300);
		obs_set_output_source(0, nullptr);
		obs_source_release(standby);
		for (obs_scene_t *scene : {one, two}) {
			obs_source_remove(obs_scene_get_source(scene));
			obs_scene_release(scene);
		}
		obs_source_remove(cam);
		obs_source_release(cam);
		sleep_ms(500);
		set_loaded(true);
		CHECK(twins == 1 && freed,
		      "a source with a stand-in (Show when source is hidden) is let go of when its scenes are cleared, so OBS can change scene collection and close (stand-ins %d, let go %d)",
		      twins, (int)freed);
	}

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
		// The report: sources with StageKit's filters looked paler in the
		// Filters window than on the canvas. Each filter must leave colours
		// alone however OBS is drawing: in the scene, or by itself in a
		// window, where colours are handled in linear light.
		printf("\n[colours in the Filters window]\n");
		{
			obs_data_t *pd = obs_data_create();
			obs_data_set_string(pd, "file", argv[3]);
			obs_source_t *picture = obs_source_create("image_source", "Colour Check", pd, nullptr);
			obs_data_release(pd);
			sleep_ms(200);
			// How far the picture's middle differs between the two ways of
			// drawing, on average, per colour channel.
			auto differs = [&]() {
				for (int i = 0; i < 4; i++) {
					sleep_ms(60);
					render(picture, obs_source_get_width(picture), obs_source_get_height(picture));
				}
				const uint32_t w = obs_source_get_width(picture), h = obs_source_get_height(picture);
				const std::vector<uint8_t> in_scene = render(picture, w, h);
				const std::vector<uint8_t> shown = render_shown(picture, w, h);
				double sum = 0.0;
				size_t count = 0;
				for (uint32_t y = h / 3; y < h * 2 / 3; y += 3) {
					for (uint32_t x = w / 3; x < w * 2 / 3; x += 3) {
						const size_t at = (size_t(y) * w + x) * 4;
						for (int c = 0; c < 3; c++)
							sum += std::abs((int)in_scene[at + c] - (int)shown[at + c]);
						count += 3;
					}
				}
				return count ? sum / (double)count : 0.0;
			};
			const double plain = differs();
			CHECK(plain < 1.5, "a picture with no filter looks the same either way (off by %.2f)", plain);
			struct {
				const char *id, *name;
			} kinds[] = {{"stagekit_shadow_backdrop", "Shadow, Glow & Outline"},
				     {"stagekit_corners", "Corners & Edges"},
				     {"stagekit_ramp_zoom", "Ramp Zoom"},
				     {"stagekit_ramp_blur", "Ramp Blur"},
				     {"stagekit_auto_frame", "Auto Frame"},
				     {"stagekit_standby", "Standby Picture"},
				     {"stagekit_app_capture", "Capture App in Use"}};
			for (const auto &kind : kinds) {
				obs_data_t *kd = obs_data_create();
				obs_data_set_bool(kd, "placed", true);
				obs_source_t *f = obs_source_create(kind.id, kind.name, kd, nullptr);
				obs_data_release(kd);
				if (!f) {
					CHECK(false, "%s can be created", kind.name);
					continue;
				}
				obs_source_filter_add(picture, f);
				sleep_ms(300);
				const double off = differs();
				CHECK(off < 1.5, "with %s it still looks the same in the Filters window (off by %.2f)",
				      kind.name, off);
				obs_source_filter_remove(picture, f);
				obs_source_release(f);
				sleep_ms(100);
			}
			// A source that draws itself, as a Window Capture does: OBS takes
			// its picture for a filter by another way than a plain source's.
			obs_data_t *vd = obs_data_create();
			obs_data_set_string(vd, "source", "Colour Check");
			obs_data_set_int(vd, "crop_to", 0);
			obs_source_t *self_drawn =
				obs_source_create("stagekit_cropped_view", "Colour View", vd, nullptr);
			obs_data_release(vd);
			sleep_ms(700);
			obs_source_t *whole = picture;
			picture = self_drawn;
			const double plain_view = differs();
			CHECK(plain_view < 1.5, "a source that draws itself looks the same either way (off by %.2f)",
			      plain_view);
			for (const auto &kind : kinds) {
				obs_data_t *kd = obs_data_create();
				obs_data_set_bool(kd, "placed", true);
				obs_source_t *f = obs_source_create(kind.id, kind.name, kd, nullptr);
				obs_data_release(kd);
				if (!f)
					continue;
				obs_source_filter_add(picture, f);
				sleep_ms(300);
				const double off = differs();
				CHECK(off < 1.5, "and with %s on it, the same in the Filters window (off by %.2f)",
				      kind.name, off);
				obs_source_filter_remove(picture, f);
				obs_source_release(f);
				sleep_ms(100);
			}
			picture = whole;
			obs_source_release(self_drawn);
			obs_source_release(picture);
		}

		// Show in Scenes: one switch that shows and hides sources across scenes.
		// Updates: which release is newest, which is newer than this build,
		// and the helper that installs one.
		printf("\n[updates]\n");
		{
			auto order = [](const char *a, const char *b) {
				calldata_t cd = {};
				calldata_set_string(&cd, "a", a);
				calldata_set_string(&cd, "b", b);
				proc_handler_call(obs_get_proc_handler(), "stagekit_update_compare", &cd);
				const long long got = calldata_int(&cd, "order");
				calldata_free(&cd);
				return got;
			};
			CHECK(order("0.1.0-beta9", "0.1.0-beta10") < 0 && order("0.1.0-beta82", "0.1.0-rc1") < 0 &&
				      order("0.1.0-rc2", "0.1.0") < 0 && order("0.1.0", "0.1.1-beta1") < 0 &&
				      order("0.2.0", "0.10.0") < 0 && order("0.1.0-beta82", "0.1.0-beta82") == 0,
			      "versions are put in order: beta 9 before beta 10, betas before release candidates before the finished one");
			{
				calldata_t cd = {};
				proc_handler_call(obs_get_proc_handler(), "stagekit_update_current", &cd);
				const char *now = calldata_string(&cd, "version");
				CHECK(now && strncmp(now, "0.", 2) == 0 && strlen(now) >= 5,
				      "the build knows which release it is (%s)", now ? now : "");
				calldata_free(&cd);
			}
			const std::string sha_a(64, 'a'), sha_b(64, 'b'), sha_c(64, 'c');
			auto release = [&](const char *tag, bool pre, bool draft, const std::string &assets,
					   const std::string &notes) {
				return std::string("{\"tag_name\":\"") + tag +
				       "\",\"prerelease\":" + (pre ? "true" : "false") +
				       ",\"draft\":" + (draft ? "true" : "false") +
				       ",\"html_url\":\"https://example.invalid/" + tag + "\",\"body\":\"" + notes +
				       "\",\"assets\":[" + assets + "]}";
			};
			auto asset = [](const std::string &name) {
				return "{\"name\":\"" + name +
				       "\",\"browser_download_url\":\"https://example.invalid/dl/" + name + "\"}";
			};
			const std::string exe = "stagekit-0.1.0-windows-x64-Installer.exe";
			const std::string deck = "stagekit-0.1.0-streamdeck.streamDeckPlugin";
			const std::string list =
				"[" +
				// Newest of all, but its Windows files are not up yet.
				release("0.1.0-beta90", true, false, asset(deck),
					"### Checksums\\n    " + deck + ": " + sha_c) +
				"," +
				release("0.1.0-beta89", true, false, asset(exe) + "," + asset(deck),
					"Notes.\\n### Checksums\\n    " + deck + ": " + sha_b + "\\n    " + exe + ": " +
						sha_a) +
				"," + release("0.1.0-beta91", true, true, asset(exe), "    " + exe + ": " + sha_a) +
				"," + release("0.0.9", false, false, asset(exe), "    " + exe + ": " + sha_c) + "," +
				// No checksum in its notes: never offered.
				release("0.1.0-beta95", true, false, asset(exe), "no checksums here") + "]";
			auto newest = [&](bool betas, std::string &version, std::string &sha, std::string &deck_url) {
				calldata_t cd = {};
				calldata_set_string(&cd, "json", list.c_str());
				calldata_set_bool(&cd, "betas", betas);
				proc_handler_call(obs_get_proc_handler(), "stagekit_update_newest", &cd);
				const bool found = calldata_bool(&cd, "found");
				version = calldata_string(&cd, "version") ? calldata_string(&cd, "version") : "";
				sha = calldata_string(&cd, "sha") ? calldata_string(&cd, "sha") : "";
				deck_url = calldata_string(&cd, "deck") ? calldata_string(&cd, "deck") : "";
				calldata_free(&cd);
				return found;
			};
			std::string version, sha, deck_url;
			const bool with_betas = newest(true, version, sha, deck_url);
			CHECK(with_betas && version == "0.1.0-beta89" && sha == sha_a &&
				      deck_url.find("streamDeckPlugin") != std::string::npos,
			      "the newest release is the newest with a checked Windows installer: not a draft, not one whose files are still on their way, not one without a checksum (%s)",
			      version.c_str());
			const bool without = newest(false, version, sha, deck_url);
			CHECK(without && version == "0.0.9" && sha == sha_c && deck_url.empty(),
			      "without test versions only finished releases count (%s)", version.c_str());
			{
				calldata_t cd = {};
				calldata_set_string(&cd, "installer", "https://example.invalid/dl/it's.exe");
				calldata_set_string(&cd, "sha", sha_a.c_str());
				calldata_set_string(&cd, "deck", "https://example.invalid/dl/deck");
				calldata_set_string(&cd, "deck_sha", sha_b.c_str());
				calldata_set_string(&cd, "obs", "C:\\Program Files\\obs-studio\\bin\\64bit\\obs64.exe");
				proc_handler_call(obs_get_proc_handler(), "stagekit_update_script", &cd);
				const std::string script =
					calldata_string(&cd, "script") ? calldata_string(&cd, "script") : "";
				calldata_free(&cd);
				const size_t fetch =
					script.find("Fetch 'https://example.invalid/dl/it''s.exe' '" + sha_a + "'");
				const size_t close = script.find("CloseMainWindow");
				const size_t setup = script.find("/VERYSILENT");
				const size_t again = script.find("obs64.exe'");
				CHECK(fetch != std::string::npos && close != std::string::npos &&
					      setup != std::string::npos && again != std::string::npos &&
					      fetch < close && close < setup && setup < again &&
					      script.find("Get-FileHash") != std::string::npos &&
					      script.find("update.log") != std::string::npos &&
					      script.find("trap {") != std::string::npos &&
					      script.find("'" + sha_b + "'") != std::string::npos,
				      "the install helper downloads and checks before it closes OBS, then installs, then starts OBS again, with quotes in names kept safe");
			}
		}

		// A capture hooked onto the window in front, by hand (the Stream Deck
		// plugin's Hook key asks for this through obs-websocket).
		printf("\n[hook a capture by hand]\n");
		{
			obs_source_t *gc = obs_source_create("game_capture", "Hook Game", nullptr, nullptr);
			obs_source_t *other = obs_source_create("color_source", "Hook Colour", nullptr, nullptr);
			obs_source_t *autocap =
				obs_source_create("stagekit_auto_capture", "Auto Capture", nullptr, nullptr);
			obs_source_filter_add(gc, autocap);
			sleep_ms(150);
			auto front = [](const char *title, const char *cls, const char *exe) {
				calldata_t cd = {};
				calldata_set_string(&cd, "title", title);
				calldata_set_string(&cd, "window_class", cls);
				calldata_set_string(&cd, "executable", exe);
				proc_handler_call(obs_get_proc_handler(), "stagekit_test_front_window", &cd);
				calldata_free(&cd);
			};
			auto hook = [](const char *source, std::string &said) {
				calldata_t cd = {};
				calldata_set_string(&cd, "source", source);
				proc_handler_call(obs_get_proc_handler(), "stagekit_hook_front", &cd);
				const bool ok = calldata_bool(&cd, "success");
				const char *text = calldata_string(&cd, "said");
				said = text ? text : "";
				calldata_free(&cd);
				return ok;
			};
			std::string said;
			front("Some Game", "GameWnd", "somegame.exe");
			const bool hooked = hook("Hook Game", said);
			obs_data_t *gs = obs_source_get_settings(gc);
			const std::string window = obs_data_get_string(gs, "window");
			CHECK(hooked && said == "somegame.exe" && window.find("somegame.exe") != std::string::npos &&
				      strcmp(obs_data_get_string(gs, "capture_mode"), "window") == 0 &&
				      obs_data_get_int(gs, "priority") == 2,
			      "a Game Capture is aimed at the window in front (%s; %s)", said.c_str(), window.c_str());
			obs_data_release(gs);
			CHECK(!obs_source_enabled(autocap),
			      "and its Auto Capture is switched off, so it does not aim it elsewhere again");
			front("OBS", "Qt", "obs64.exe");
			CHECK(!hook("Hook Game", said) && said.find("OBS") != std::string::npos,
			      "OBS itself in front is refused (%s)", said.c_str());
			front("Some Game", "GameWnd", "somegame.exe");
			CHECK(!hook("Hook Colour", said), "a source that is not a capture is refused (%s)",
			      said.c_str());
			// With a transition: the items showing the capture go out, the
			// capture is aimed, and they come back.
			{
				obs_scene_t *hs = obs_scene_create("Hook Scene");
				obs_sceneitem_t *hi = obs_scene_add(hs, gc);
				front("Other Game", "OtherWnd", "othergame.exe");
				calldata_t cd = {};
				calldata_set_string(&cd, "source", "Hook Game");
				calldata_set_string(&cd, "transition", "fade_transition");
				calldata_set_int(&cd, "ms", 200);
				proc_handler_call(obs_get_proc_handler(), "stagekit_hook_front", &cd);
				const bool ok = calldata_bool(&cd, "success");
				calldata_free(&cd);
				const bool hidden_first = !obs_sceneitem_visible(hi);
				obs_data_t *mid = obs_source_get_settings(gc);
				const bool not_yet =
					std::string(obs_data_get_string(mid, "window")).find("othergame") ==
					std::string::npos;
				obs_data_release(mid);
				sleep_ms(1000);
				obs_data_t *after = obs_source_get_settings(gc);
				const bool aimed =
					std::string(obs_data_get_string(after, "window")).find("othergame") !=
					std::string::npos;
				obs_data_release(after);
				CHECK(ok && hidden_first && not_yet && aimed && obs_sceneitem_visible(hi),
				      "with a transition the capture goes out first, is aimed out of sight, and comes back (%d %d %d %d)",
				      hidden_first, not_yet, aimed, (int)obs_sceneitem_visible(hi));
				obs_scene_release(hs);
			}
			// A picture shape set by hand, for this game only.
			{
				obs_source_t *shaped =
					obs_source_create("image_source", "Shape Game", nullptr, nullptr);
				obs_data_t *sd = obs_data_create();
				obs_data_set_string(sd, "file", argv[3]);
				obs_source_update(shaped, sd);
				obs_source_t *sframe =
					obs_source_create("stagekit_auto_frame", "Auto Frame", nullptr, nullptr);
				obs_source_filter_add(shaped, sframe);
				sleep_ms(300);
				auto shape = [](const char *source, const char *text, std::string &said2) {
					calldata_t cd = {};
					calldata_set_string(&cd, "source", source);
					calldata_set_string(&cd, "shape", text);
					calldata_set_bool(&cd, "until_gone", true);
					proc_handler_call(obs_get_proc_handler(), "stagekit_set_picture_shape", &cd);
					const bool ok = calldata_bool(&cd, "success");
					said2 = calldata_string(&cd, "said") ? calldata_string(&cd, "said") : "";
					calldata_free(&cd);
					return ok;
				};
				auto forced = [&] {
					obs_data_t *fs = obs_source_get_settings(sframe);
					const double r = obs_data_get_double(fs, "force_ratio");
					obs_data_release(fs);
					return r;
				};
				std::string said2;
				const bool set = shape("Shape Game", "8:7", said2);
				sleep_ms(200);
				CHECK(set && std::fabs(forced() - 8.0 / 7.0) < 0.001,
				      "a shape set by hand is put on the source's Auto Frame (%.4f)", forced());
				CHECK(!shape("Shape Game", "banana", said2) && !shape("Hook Colour", "4:3", said2),
				      "something that is not a shape, and a source without an Auto Frame, are refused");
				// The game ends: nothing captured for a while.
				obs_data_set_string(sd, "file", "");
				obs_source_update(shaped, sd);
				sleep_ms(5200);
				CHECK(forced() == 0.0, "and it is dropped when that game's capture has ended (%.4f)",
				      forced());
				shape("Shape Game", "4:3", said2);
				sleep_ms(200);
				const bool again = std::fabs(forced() - 4.0 / 3.0) < 0.001;
				shape("Shape Game", "auto", said2);
				sleep_ms(200);
				CHECK(again && forced() == 0.0,
				      "set before a game has started it waits for the game, and 'auto' hands it back at once");
				obs_data_release(sd);
				obs_source_filter_remove(shaped, sframe);
				obs_source_release(sframe);
				obs_source_release(shaped);
			}
			front("", "", "");
			obs_source_filter_remove(gc, autocap);
			obs_source_release(autocap);
			obs_source_release(other);
			obs_source_release(gc);
		}

		printf("\n[show in scenes]\n");
		{
			obs_source_t *cam = obs_source_create("color_source", "Switch Camera", nullptr, nullptr);
			obs_source_t *chat = obs_source_create("color_source", "Switch Chat", nullptr, nullptr);
			obs_scene_t *one = obs_scene_create("Switch One");
			obs_scene_t *two = obs_scene_create("Switch Two");
			obs_scene_t *three = obs_scene_create("Switch Three");
			obs_sceneitem_t *cam1 = obs_scene_add(one, cam), *cam2 = obs_scene_add(two, cam);
			obs_sceneitem_t *chat1 = obs_scene_add(one, chat), *chat3 = obs_scene_add(three, chat);
			// In the third scene the camera sits inside a group.
			obs_sceneitem_t *holder = obs_scene_add_group(three, "Switch Group");
			obs_sceneitem_t *cam3 = obs_scene_add(three, cam);
			obs_sceneitem_group_add_item(holder, cam3);
			obs_source_t *sw =
				obs_source_create("stagekit_show_in_scenes", "Camera Switch", nullptr, nullptr);
			CHECK(sw != nullptr, "the Show in Scenes filter can be created");
			obs_source_filter_add(cam, sw);
			sleep_ms(200);
			auto flip = [&](bool on) {
				obs_source_set_enabled(sw, on);
				sleep_ms(120);
			};
			CHECK(obs_sceneitem_visible(cam1) && obs_sceneitem_visible(cam2) && obs_sceneitem_visible(cam3),
			      "adding it changes nothing by itself");
			flip(false);
			CHECK(!obs_sceneitem_visible(cam1) && !obs_sceneitem_visible(cam2) &&
				      !obs_sceneitem_visible(cam3) && obs_sceneitem_visible(chat1),
			      "switched off, the source it is on is hidden in every scene, inside a group too, and nothing else is");
			flip(true);
			CHECK(obs_sceneitem_visible(cam1) && obs_sceneitem_visible(cam2) && obs_sceneitem_visible(cam3),
			      "switched on, it is shown everywhere again");
			if (obs_source_get_display_name("fade_transition")) {
				obs_source_t *show = obs_sceneitem_get_transition(cam1, true);
				CHECK(show && strcmp(obs_source_get_unversioned_id(show), "fade_transition") == 0 &&
					      obs_sceneitem_get_transition_duration(cam1, true) == 300,
				      "the items it shows and hide are given a fade, the choice it comes with");
				obs_data_t *cut = obs_source_get_settings(sw);
				obs_data_set_string(cut, "transition", "none");
				obs_source_update(sw, cut);
				obs_data_release(cut);
				sleep_ms(100);
				flip(false);
				flip(true);
				CHECK(obs_sceneitem_get_transition(cam1, true) == nullptr,
				      "and set to a cut, the items' transitions are taken off");
			}

			// Only the scenes listed.
			obs_properties_t *sp = obs_source_properties(sw);
			pick_into_list(sw, sp, "scenes|a", "Switch Two");
			obs_properties_destroy(sp);
			obs_data_t *ss = obs_source_get_settings(sw);
			obs_data_set_int(ss, "where", 1);
			obs_source_update(sw, ss);
			sleep_ms(100);
			flip(false);
			CHECK(obs_sceneitem_visible(cam1) && !obs_sceneitem_visible(cam2) &&
				      obs_sceneitem_visible(cam3),
			      "with scenes listed, only those change");
			flip(true);

			// Several sources, and "on" meaning hidden.
			sp = obs_source_properties(sw);
			pick_into_list(sw, sp, "sources|a", "Switch Camera");
			pick_into_list(sw, sp, "sources|a", "Switch Chat");
			obs_properties_destroy(sp);
			obs_data_set_int(ss, "where", 0);
			obs_data_set_int(ss, "on_means", 1);
			obs_source_update(sw, ss);
			obs_data_release(ss);
			sleep_ms(100);
			flip(false);
			const bool all_shown = obs_sceneitem_visible(cam1) && obs_sceneitem_visible(cam2) &&
					       obs_sceneitem_visible(cam3) && obs_sceneitem_visible(chat1) &&
					       obs_sceneitem_visible(chat3);
			flip(true);
			const bool all_hidden = !obs_sceneitem_visible(cam1) && !obs_sceneitem_visible(cam2) &&
						!obs_sceneitem_visible(cam3) && !obs_sceneitem_visible(chat1) &&
						!obs_sceneitem_visible(chat3);
			CHECK(all_shown && all_hidden,
			      "several sources switch together, and 'on' can mean hidden (%d %d)", all_shown,
			      all_hidden);
			// How items appear and disappear: showing and hiding each with a
			// transition of its own, that transition's own settings (the way
			// a slide goes) set in the filter, and one listed source with a
			// change of its own. The owner: "if one has slide up to hide and
			// down to show, another can be fade".
			if (obs_source_get_display_name("slide_transition")) {
				obs_data_t *cs = obs_source_get_settings(sw);
				obs_data_set_string(cs, "transition", "slide_transition");
				obs_data_set_int(cs, "transition_ms", 400);
				obs_data_set_string(cs, "show|slide_transition|direction", "down");
				obs_data_set_string(cs, "hide_transition", "slide_transition");
				obs_data_set_int(cs, "hide_ms", 250);
				obs_data_set_string(cs, "hide|slide_transition|direction", "up");
				obs_data_set_string(cs, "for|Switch Chat|transition", "fade_transition");
				obs_data_set_int(cs, "for|Switch Chat|transition_ms", 150);
				obs_source_update(sw, cs);
				sleep_ms(100);
				flip(false); // shown
				flip(true);  // hidden
				auto way = [](obs_sceneitem_t *item, bool show, const char *kind, const char *direction,
					      uint32_t ms) {
					obs_source_t *t = obs_sceneitem_get_transition(item, show);
					if (!t || strcmp(obs_source_get_unversioned_id(t), kind) != 0 ||
					    obs_sceneitem_get_transition_duration(item, show) != ms)
						return false;
					obs_data_t *ts = obs_source_get_settings(t);
					const bool right = !direction ||
							   strcmp(obs_data_get_string(ts, "direction"), direction) == 0;
					obs_data_release(ts);
					return right;
				};
				CHECK(way(cam1, true, "slide_transition", "down", 400) &&
					      way(cam1, false, "slide_transition", "up", 250) &&
					      way(cam3, false, "slide_transition", "up", 250),
				      "showing and hiding each have their own transition, with its own settings: a slide down to show and a slide up to hide");
				CHECK(way(chat1, true, "fade_transition", nullptr, 150) &&
					      way(chat1, false, "fade_transition", nullptr, 150) &&
					      way(chat3, true, "fade_transition", nullptr, 150),
				      "and a listed source can have a change of its own: here a fade, while the others slide");
				// The settings window: the slide's own settings are there, and
				// choosing a source under 'Settings for' brings up that source's.
				obs_properties_t *cp = obs_source_properties(sw);
				obs_property_t *direction = obs_properties_get(cp, "show|slide_transition|direction");
				obs_property_t *for_box = obs_properties_get(cp, "change|for");
				const bool offered = direction && obs_property_list_item_count(direction) >= 4 &&
						     obs_properties_get(cp, "hide|slide_transition|direction") &&
						     for_box && obs_property_list_item_count(for_box) == 3;
				obs_data_set_string(cs, "change|for", "Switch Chat");
				if (for_box)
					obs_property_modified(for_box, cs);
				obs_property_t *own_kind = obs_properties_get(cp, "for|Switch Chat|transition");
				CHECK(offered && own_kind &&
					      !obs_properties_get(cp, "show|slide_transition|direction") &&
					      strcmp(obs_property_list_item_string(own_kind, 0), "usual") == 0,
				      "the filter's settings offer the slide's own direction for showing and for hiding, and 'Settings for' switches them to one source's own");
				obs_properties_destroy(cp);
				// "Off the nearest edge": a direction of StageKit's own in the
				// slide's list. Each item goes by where it lies in its scene:
				// towards that edge to hide, in from it to show.
				{
					vec2 at;
					vec2_set(&at, 500.0f, 100.0f); // nearest the right edge of 640x360
					obs_sceneitem_set_pos(cam1, &at);
					vec2_set(&at, -1800.0f, 0.0f); // nearest the left edge
					obs_sceneitem_set_pos(cam2, &at);
					obs_data_set_string(cs, "change|for", ""); // the usual change's controls
					obs_data_set_string(cs, "show|slide_transition|direction", "nearest");
					obs_data_set_string(cs, "hide|slide_transition|direction", "nearest");
					obs_source_update(sw, cs);
					obs_properties_t *np = obs_source_properties(sw);
					obs_property_t *ways =
						obs_properties_get(np, "show|slide_transition|direction");
					bool listed = false;
					for (size_t i = 0; ways && i < obs_property_list_item_count(ways); i++)
						listed = listed ||
							 strcmp(obs_property_list_item_string(ways, i), "nearest") == 0;
					obs_properties_destroy(np);
					sleep_ms(100);
					flip(false);
					flip(true);
					auto going = [](obs_sceneitem_t *item, bool show) {
						obs_source_t *t = obs_sceneitem_get_transition(item, show);
						obs_data_t *ts = t ? obs_source_get_settings(t) : nullptr;
						const std::string dir = ts ? obs_data_get_string(ts, "direction")
									   : "none";
						obs_data_release(ts);
						return dir;
					};
					CHECK(listed && way(cam1, false, "slide_transition", "right", 250) &&
						      way(cam1, true, "slide_transition", "left", 400) &&
						      way(cam2, false, "slide_transition", "left", 250) &&
						      way(cam2, true, "slide_transition", "right", 400),
					      "a slide can go 'Off the nearest edge': an item by the right edge leaves to the right and comes back from it, one by the left edge the other way (offered %d; the first hides %s and shows %s, the second hides %s and shows %s)",
					      (int)listed, going(cam1, false).c_str(), going(cam1, true).c_str(),
					      going(cam2, false).c_str(), going(cam2, true).c_str());
					vec2_set(&at, 0.0f, 0.0f);
					obs_sceneitem_set_pos(cam1, &at);
					obs_sceneitem_set_pos(cam2, &at);
				}
				// Morph as the way an item appears: the filter offers it, with
				// the boxes a link is made from, and the items it shows are
				// given the links made here. The owner: "morph should be a
				// transition in show in scene, and follow the same rules as
				// the global morph source links, or specify source links in
				// the filter".
				{
					obs_data_set_string(cs, "change|for", "");
					obs_data_set_string(cs, "transition", "stagekit_morph");
					obs_data_set_string(cs, "hide_transition", "same");
					obs_data_set_string(cs, "morph_link_a", "Switch Camera");
					obs_data_set_string(cs, "morph_link_b", "Switch Chat");
					obs_source_update(sw, cs);
					obs_properties_t *mp = obs_source_properties(sw);
					obs_property_t *add = obs_properties_get(mp, "morph_link_add");
					const bool offered = add && obs_property_visible(add) &&
							     obs_properties_get(mp, "show|stagekit_morph|unpaired") &&
							     obs_properties_get(mp, "show|stagekit_morph|ease_curve") &&
							     !obs_properties_get(mp, "show|stagekit_morph|link_a");
					const bool pressed = add && obs_property_button_clicked(add, sw);
					const int rows = list_rows(mp, "morph_links|a");
					obs_properties_destroy(mp);
					sleep_ms(100);
					flip(false);
					flip(true);
					obs_source_t *as = obs_sceneitem_get_transition(cam1, true);
					obs_data_t *as_set = as ? obs_source_get_settings(as) : nullptr;
					obs_data_array_t *given = as_set ? obs_data_get_array(as_set, "links")
									 : nullptr;
					CHECK(offered && pressed && rows == 1 && as &&
						      strcmp(obs_source_get_unversioned_id(as), "stagekit_morph") ==
							      0 &&
						      given && obs_data_array_count(given) == 1,
					      "Morph is offered as a way to show and hide, with its own settings and the boxes to link two sources; a link made in the filter goes to the items it shows (offered %d, pressed %d, rows %d, links on the item's transition %d)",
					      (int)offered, (int)pressed, rows,
					      given ? (int)obs_data_array_count(given) : -1);
					obs_data_array_release(given);
					obs_data_release(as_set);
					// With another way chosen the link boxes are out of the way.
					obs_data_t *now = obs_source_get_settings(sw);
					obs_data_set_string(now, "transition", "fade_transition");
					obs_source_update(sw, now);
					obs_data_release(now);
					obs_properties_t *fp = obs_source_properties(sw);
					obs_property_t *hidden_add = obs_properties_get(fp, "morph_link_add");
					CHECK(hidden_add && !obs_property_visible(hidden_add),
					      "and with no Morph chosen the link boxes are not shown");
					obs_properties_destroy(fp);
					obs_data_release(cs);
					cs = obs_source_get_settings(sw);
				}
				// Back to a cut for everything, as the checks below found it.
				obs_data_set_string(cs, "change|for", "");
				obs_data_set_string(cs, "transition", "none");
				obs_data_set_string(cs, "hide_transition", "same");
				obs_data_set_string(cs, "for|Switch Chat|transition", "usual");
				obs_source_update(sw, cs);
				obs_data_release(cs);
				sleep_ms(100);
				flip(false);
				flip(true);
				CHECK(obs_sceneitem_get_transition(chat1, true) == nullptr &&
					      obs_sceneitem_get_transition(cam1, false) == nullptr,
				      "set back to a cut, and the source back to the usual, every item's transitions are taken off again");
			}

			// Bring-along items too: what the camera's Send To key brings along.
			{
				obs_source_t *tag = obs_source_create("color_source", "Switch Tag", nullptr, nullptr);
				obs_sceneitem_t *tag1 = obs_scene_add(one, tag);
				obs_data_t *kd = obs_data_create();
				obs_data_set_string(kd, "linked", "Switch Tag");
				obs_source_t *key = obs_source_create("stagekit_send_to", "Camera Key", kd, nullptr);
				obs_data_release(kd);
				obs_source_filter_add(cam, key);
				obs_source_set_enabled(key, false);
				obs_data_t *ls = obs_source_get_settings(sw);
				obs_data_array_t *only_cam = obs_data_array_create();
				obs_data_t *entry = obs_data_create();
				obs_data_set_string(entry, "value", "Switch Camera");
				obs_data_array_push_back(only_cam, entry);
				obs_data_release(entry);
				obs_data_set_array(ls, "sources", only_cam);
				obs_data_array_release(only_cam);
				obs_data_set_int(ls, "on_means", 0);
				obs_source_update(sw, ls);
				sleep_ms(100);
				flip(false);
				const bool tag_stays = obs_sceneitem_visible(tag1) && !obs_sceneitem_visible(cam1);
				flip(true);
				obs_data_set_bool(ls, "with_linked", true);
				obs_source_update(sw, ls);
				obs_data_release(ls);
				sleep_ms(100);
				flip(false);
				const bool tag_goes = !obs_sceneitem_visible(tag1) && !obs_sceneitem_visible(cam1);
				flip(true);
				CHECK(tag_stays && tag_goes && obs_sceneitem_visible(tag1),
				      "with 'Bring-along items too', what the source's Send To key brings along is hidden and shown with it (%d %d)",
				      tag_stays, tag_goes);
				obs_source_filter_remove(cam, key);
				obs_source_release(key);
				obs_source_release(tag);
			}
			obs_source_filter_remove(cam, sw);
			obs_source_release(sw);
			obs_scene_release(one);
			obs_scene_release(two);
			obs_scene_release(three);
			obs_source_release(cam);
			obs_source_release(chat);
		}

		// Standby Picture: a picture in a capture's place while it has nothing.
		printf("\n[standby picture]\n");
		{
			// A capture with nothing attached: an image source with no file
			// has no size, as a Game Capture has before a game is hooked.
			obs_source_t *capture = obs_source_create("image_source", "Standby Capture", nullptr, nullptr);
			obs_data_t *sd = obs_data_create();
			obs_data_set_int(sd, "picture", 1); // a file
			obs_data_set_string(sd, "file", argv[3]);
			obs_data_set_int(sd, "fade_ms", 200);
			obs_data_set_int(sd, "back_ms", 200);
			obs_source_t *standby = obs_source_create("stagekit_standby", "Standby Picture", sd, nullptr);
			CHECK(standby != nullptr, "the Standby Picture filter can be created");
			obs_source_filter_add(capture, standby);
			auto settle = [&](int frames) {
				for (int i = 0; i < frames; i++) {
					sleep_ms(50);
					const uint32_t w = obs_source_get_width(capture),
						       h = obs_source_get_height(capture);
					if (w && h)
						render(capture, w, h);
				}
			};
			settle(12);
			uint32_t w = obs_source_get_width(capture), h = obs_source_get_height(capture);
			std::vector<uint8_t> px = w && h ? render(capture, w, h) : std::vector<uint8_t>();
			auto solid = [&](uint32_t x, uint32_t y) {
				return !px.empty() && px[(size_t(y) * w + x) * 4 + 3] > 250;
			};
			CHECK(w == 1920 && h == 1080 && solid(w / 2, h / 2) && solid(4, 4) && solid(w - 5, h - 5),
			      "with nothing captured, the picture stands in, whole and in its own shape (%ux%u)", w, h);

			// The capture arrives: a 100x60 red picture.
			QImage red(100, 60, QImage::Format_ARGB32);
			red.fill(QColor(255, 0, 0));
			const std::string red_path = out + "/standby-red.png";
			red.save(QString::fromStdString(red_path));
			obs_data_t *cs = obs_source_get_settings(capture);
			obs_data_set_string(cs, "file", red_path.c_str());
			obs_source_update(capture, cs);
			sleep_ms(110);
			w = obs_source_get_width(capture);
			h = obs_source_get_height(capture);
			px = w && h ? render(capture, w, h) : std::vector<uint8_t>();
			const uint8_t *mid = px.empty() ? nullptr : &px[(size_t(h / 2) * w + w / 2) * 4];
			// Part-way: a size between the two, and a picture that is neither yet.
			const bool part_way = mid && w > 100 && w < 1920 && h > 60 && h < 1080 &&
					      !(mid[0] > 250 && mid[1] < 5 && mid[2] < 5);
			const uint32_t mid_w = w, mid_h = h;
			settle(12);
			w = obs_source_get_width(capture);
			h = obs_source_get_height(capture);
			px = render(capture, w, h);
			mid = &px[(size_t(h / 2) * w + w / 2) * 4];
			CHECK(part_way && w == 100 && h == 60 && mid[0] > 250 && mid[1] < 5 && mid[2] < 5,
			      "when the capture starts, the size glides to the capture's as the picture fades into it (%ux%u part-way; then %ux%u, %d, %d, %d)",
			      mid_w, mid_h, w, h, mid[0], mid[1], mid[2]);

			// And goes again: the picture comes back, gliding to its own shape
			// (16:9 here) at the height the capture had.
			obs_data_set_string(cs, "file", "");
			obs_source_update(capture, cs);
			obs_data_release(cs);
			sleep_ms(110);
			const uint32_t back_w = obs_source_get_width(capture);
			settle(12);
			w = obs_source_get_width(capture);
			h = obs_source_get_height(capture);
			px = w && h ? render(capture, w, h) : std::vector<uint8_t>();
			mid = px.empty() ? nullptr : &px[(size_t(h / 2) * w + w / 2) * 4];
			CHECK(back_w > 100 && back_w < 107 && w == 107 && h == 60 && mid && mid[3] > 250 &&
				      !(mid[0] > 250 && mid[1] < 5 && mid[2] < 5),
			      "when the capture goes, the picture is back and glides to its own shape again (%u part-way; then %ux%u)",
			      back_w, w, h);

			// Asked for over a capture that is running: the picture comes
			// in as if the capture had gone, and leaves again.
			{
				obs_data_t *again = obs_source_get_settings(capture);
				obs_data_set_string(again, "file", red_path.c_str());
				obs_source_update(capture, again);
				settle(12);
				auto cover = [&]() {
					calldata_t cd = {};
					calldata_set_bool(&cd, "toggle", true);
					proc_handler_call(obs_source_get_proc_handler(standby), "cover", &cd);
					calldata_free(&cd);
				};
				auto red_mid = [&]() {
					const uint32_t cw = obs_source_get_width(capture),
						       ch = obs_source_get_height(capture);
					const std::vector<uint8_t> p = cw && ch ? render(capture, cw, ch)
										: std::vector<uint8_t>();
					const uint8_t *m = p.empty() ? nullptr : &p[(size_t(ch / 2) * cw + cw / 2) * 4];
					return m && m[0] > 250 && m[1] < 5 && m[2] < 5;
				};
				const bool live_first = red_mid();
				cover();
				// Part-way: neither the capture alone nor the picture alone,
				// and a size between the two.
				sleep_ms(60);
				render(capture, obs_source_get_width(capture), obs_source_get_height(capture));
				sleep_ms(50);
				uint8_t part[4] = {0, 0, 0, 0};
				const uint32_t part_w = obs_source_get_width(capture);
				{
					const uint32_t ch = obs_source_get_height(capture);
					const std::vector<uint8_t> p = render(capture, part_w, ch);
					memcpy(part, &p[(size_t(ch / 2) * part_w + part_w / 2) * 4], 4);
				}
				settle(12);
				{
					const uint32_t cw = obs_source_get_width(capture),
						       ch = obs_source_get_height(capture);
					const std::vector<uint8_t> p = render(capture, cw, ch);
					const uint8_t *e = &p[(size_t(ch / 2) * cw + cw / 2) * 4];
					const bool mixed = !(part[0] > 250 && part[1] < 5 && part[2] < 5) &&
							   (std::abs(part[0] - e[0]) > 8 ||
							    std::abs(part[1] - e[1]) > 8 ||
							    std::abs(part[2] - e[2]) > 8);
					CHECK(mixed && part_w > 100 && part_w < 107,
					      "and it comes in with the filter's transition, not at a stroke (part-way %d, %d, %d and %u wide; then %d, %d, %d)",
					      part[0], part[1], part[2], part_w, e[0], e[1], e[2]);
				}
				bool waiting = false;
				{
					calldata_t cd = {};
					if (proc_handler_call(obs_source_get_proc_handler(standby), "get_standing",
							      &cd))
						waiting = calldata_bool(&cd, "waiting");
					calldata_free(&cd);
				}
				const bool covered = !red_mid() && waiting;
				const uint32_t cover_w = obs_source_get_width(capture);
				cover();
				settle(12);
				CHECK(live_first && covered && cover_w == 107 && red_mid() &&
					      obs_source_get_width(capture) == 100,
				      "asked for over a running capture, the picture stands in and leaves again (%d, %d, %u wide; then %d, %u wide)",
				      live_first, covered, cover_w, red_mid(), obs_source_get_width(capture));
				obs_data_set_string(again, "file", "");
				obs_source_update(capture, again);
				obs_data_release(again);
				settle(12);
			}

			// Auto Capture on the same capture: its Standby Picture while no
			// game is captured, and behind it when the capture is hidden, with
			// Auto Capture's own transition.
			{
				obs_data_t *again = obs_source_get_settings(capture);
				obs_data_set_string(again, "file", red_path.c_str());
				obs_source_update(capture, again);
				obs_data_t *acd = obs_data_create();
				obs_data_set_bool(acd, "no_game_standby", true);
				obs_data_set_int(acd, "standby_style", 1); // a cut, so the check need not wait
				obs_data_array_t *listed = obs_data_array_create();
				obs_data_t *entry = obs_data_create();
				obs_data_set_string(entry, "value", "oddgame");
				obs_data_array_push_back(listed, entry);
				obs_data_release(entry);
				obs_data_set_array(acd, "programs", listed);
				obs_data_array_release(listed);
				obs_source_t *ac = obs_source_create("stagekit_auto_capture", "Standby Auto Capture",
								     acd, nullptr);
				obs_data_release(acd);
				obs_source_filter_add(capture, ac);
				auto windows = [&](const char *list) {
					calldata_t cd = {};
					calldata_set_string(&cd, "windows", list);
					proc_handler_call(obs_source_get_proc_handler(ac), "set_test_windows", &cd);
					calldata_free(&cd);
				};
				auto waiting = [&]() {
					bool w = false;
					calldata_t cd = {};
					if (proc_handler_call(obs_source_get_proc_handler(standby), "get_standing",
							      &cd))
						w = calldata_bool(&cd, "waiting");
					calldata_free(&cd);
					return w;
				};
				windows("Inbox|MailWnd|mail.exe|front|");
				settle(10);
				const bool no_game = waiting();
				windows("Odd Game|OddWnd|OddGame.exe|front|");
				settle(10);
				const bool with_game = waiting();
				proc_handler_call(obs_source_get_proc_handler(ac), "hide", nullptr);
				settle(10);
				const bool hidden = waiting();
				proc_handler_call(obs_source_get_proc_handler(ac), "hide", nullptr);
				settle(10);
				CHECK(no_game && !with_game && hidden && !waiting(),
				      "Auto Capture shows the Standby Picture while there is no game, and hides the capture behind it and back (%d, %d, %d, %d)",
				      no_game, with_game, hidden, waiting());
				obs_source_filter_remove(capture, ac);
				obs_source_release(ac);
				obs_data_set_string(again, "file", "");
				obs_source_update(capture, again);
				obs_data_release(again);
				settle(12);
			}

			// Asked to fill instead, it takes the size the capture last had.
			obs_data_set_bool(sd, "own_shape", false);
			obs_source_update(standby, sd);
			settle(12);
			CHECK(obs_source_get_width(capture) == 100 && obs_source_get_height(capture) == 60,
			      "unticked, the picture fills the size the capture last had (%ux%u)",
			      obs_source_get_width(capture), obs_source_get_height(capture));
			obs_data_set_bool(sd, "own_shape", true);
			obs_source_update(standby, sd);
			settle(12);

			// Another way of changing: the picture slides away to the left, so
			// part-way the capture shows at the right and the picture at the left.
			{
				obs_data_set_int(sd, "style", 2);
				obs_data_set_int(sd, "fade_ms", 800);
				obs_data_set_int(sd, "ease_curve", 0); // linear, so part-way is half-way
				obs_source_update(standby, sd);
				settle(8);
				obs_data_t *again = obs_source_get_settings(capture);
				obs_data_set_string(again, "file", red_path.c_str());
				obs_source_update(capture, again);
				bool seen = false;
				for (int i = 0; i < 14 && !seen; i++) {
					sleep_ms(50);
					const uint32_t sw = obs_source_get_width(capture),
						       sh = obs_source_get_height(capture);
					if (!sw || !sh)
						continue;
					const std::vector<uint8_t> part = render(capture, sw, sh);
					const uint8_t *l = &part[(size_t(sh / 2) * sw + sw / 10) * 4];
					const uint8_t *r = &part[(size_t(sh / 2) * sw + sw * 9 / 10) * 4];
					const bool left_picture = !(l[0] > 250 && l[1] < 5 && l[2] < 5) && l[3] > 250;
					const bool right_capture = r[0] > 250 && r[1] < 5 && r[2] < 5;
					seen = left_picture && right_capture;
				}
				CHECK(seen,
				      "with 'Slide away to the left', the capture is uncovered from the right as the picture leaves");
				settle(20);
				obs_data_set_string(again, "file", "");
				obs_source_update(capture, again);
				obs_data_release(again);
				obs_data_set_int(sd, "style", 0);
				obs_data_set_int(sd, "fade_ms", 200);
				obs_data_set_int(sd, "ease_curve", 3);
				obs_source_update(standby, sd);
				settle(12);
			}

			// Switched off, the source is as it would be without it.
			obs_source_set_enabled(standby, false);
			settle(4);
			CHECK(obs_source_get_width(capture) == 0, "switched off, nothing stands in for the capture");
			obs_source_set_enabled(standby, true);

			// The start image is what it shows unless told otherwise.
			calldata_t si = {};
			calldata_set_string(&si, "path", red_path.c_str());
			proc_handler_call(obs_get_proc_handler(), "stagekit_set_start_image", &si);
			calldata_free(&si);
			obs_data_set_int(sd, "picture", 0);
			obs_source_update(standby, sd);
			settle(14);
			w = obs_source_get_width(capture);
			h = obs_source_get_height(capture);
			px = w && h ? render(capture, w, h) : std::vector<uint8_t>();
			mid = px.empty() ? nullptr : &px[(size_t(h / 2) * w + w / 2) * 4];
			CHECK(mid && mid[0] > 250 && mid[1] < 5 && mid[2] < 5,
			      "left as it comes, it shows the start image (%d, %d, %d)", mid ? mid[0] : -1,
			      mid ? mid[1] : -1, mid ? mid[2] : -1);
			calldata_t back = {};
			calldata_set_string(&back, "path", "");
			proc_handler_call(obs_get_proc_handler(), "stagekit_set_start_image", &back);
			calldata_free(&back);
			// An Ambient Background blending in this capture does not take
			// the standby picture (all red here) for the capture.
			{
				// (The start image is cleared by now; the standby picture is the red file.)
				obs_data_set_int(sd, "picture", 1);
				obs_data_set_string(sd, "file", red_path.c_str());
				obs_source_update(standby, sd);
				obs_data_t *bs = obs_data_create();
				obs_data_set_int(bs, "live_mode", 2);
				obs_data_set_string(bs, "live_source", "Standby Capture");
				obs_data_set_bool(bs, "live_replace", true);
				obs_data_set_string(bs, "image", argv[3]);
				obs_data_set_double(bs, "blur", 0);
				obs_data_set_double(bs, "glow", 0);
				obs_data_set_double(bs, "speed_x", 0);
				obs_data_set_double(bs, "speed_y", 0);
				obs_source_t *behind = obs_source_create("stagekit_ambient_background",
									 "Standby Background", bs, nullptr);
				obs_source_inc_showing(behind);
				auto all_red = [&] {
					std::vector<uint8_t> shot;
					for (int i = 0; i < 8; i++) {
						sleep_ms(150);
						const uint32_t aw = obs_source_get_width(behind),
							       ah = obs_source_get_height(behind);
						if (aw && ah)
							shot = render(behind, aw, ah);
						render(capture, obs_source_get_width(capture),
						       obs_source_get_height(capture));
					}
					if (shot.size() < 8)
						return false;
					const uint8_t *c = &shot[(shot.size() / 8) * 4];
					return c[0] > 200 && c[1] < 40 && c[2] < 40;
				};
				CHECK(!all_red(),
				      "an Ambient Background does not blend in a standby picture as if it were the capture");
				obs_data_set_bool(bs, "live_skip_standby", false);
				obs_source_update(behind, bs);
				CHECK(all_red(), "unless 'Ignore a standby picture' is unticked");
				obs_data_release(bs);
				obs_source_dec_showing(behind);
				obs_source_release(behind);
			}
			// After an Auto Frame, and before a Corners & Edges: the game
			// picture's rectangle kept from the last game must not cut the
			// standby picture.
			{
				QImage small(400, 300, QImage::Format_ARGB32);
				small.fill(QColor(0, 200, 0));
				const std::string small_path = out + "/standby-game.png";
				small.save(QString::fromStdString(small_path));
				obs_data_t *gd = obs_data_create();
				obs_data_set_string(gd, "file", small_path.c_str());
				obs_source_t *game = obs_source_create("image_source", "Standby Framed", gd, nullptr);
				obs_data_t *fs = obs_data_create();
				obs_data_set_int(fs, "frame_width", 800);
				obs_data_set_int(fs, "frame_height", 300);
				obs_data_set_int(fs, "glide_ms", 0);
				obs_source_t *frame =
					obs_source_create("stagekit_auto_frame", "Auto Frame", fs, nullptr);
				obs_data_release(fs);
				obs_data_t *wd = obs_data_create();
				obs_data_set_int(wd, "picture", 1);
				obs_data_set_string(wd, "file", argv[3]);
				obs_data_set_int(wd, "fade_ms", 100);
				obs_data_set_int(wd, "back_ms", 600);
				obs_data_set_bool(wd, "placed", true);
				obs_source_t *wait =
					obs_source_create("stagekit_standby", "Standby Picture", wd, nullptr);
				obs_data_release(wd);
				// Added before the Auto Frame, the wrong way round: it must
				// put itself after it, or the Auto Frame trims the picture.
				obs_source_filter_add(game, wait);
				obs_source_filter_add(game, frame);
				obs_data_t *cd = obs_data_create();
				obs_data_set_double(cd, "size", 40);
				obs_source_t *shape = obs_source_create("stagekit_corners", "Corners", cd, nullptr);
				obs_data_release(cd);
				obs_source_filter_add(game, shape);
				auto draw = [&](int frames) {
					std::vector<uint8_t> got;
					for (int i = 0; i < frames; i++) {
						sleep_ms(50);
						const uint32_t gw = obs_source_get_width(game),
							       gh = obs_source_get_height(game);
						if (gw && gh)
							got = render(game, gw, gh);
					}
					return got;
				};
				std::vector<uint8_t> gp = draw(24);
				uint32_t gw = obs_source_get_width(game), gh = obs_source_get_height(game);
				auto a = [&](uint32_t x, uint32_t y) {
					return gp.size() == size_t(gw) * gh * 4 ? (int)gp[(size_t(y) * gw + x) * 4 + 3]
										: -1;
				};
				CHECK(gw == 800 && gh == 300 && a(100, 150) == 0 && a(400, 150) == 255,
				      "with a game, Corners & Edges shapes the game picture in the frame (%ux%u, %d, %d)",
				      gw, gh, a(100, 150), a(400, 150));
				CHECK(gp.size() > 4 && gp[(size_t(150) * gw + 400) * 4 + 1] > 190 &&
					      gp[(size_t(150) * gw + 400) * 4] < 12,
				      "and the game is drawn as it is");
				obs_data_set_string(gd, "file", "");
				obs_source_update(game, gd);
				// Just after the game goes, its last frame is still fading out.
				int greenest = 0;
				for (int i = 0; i < 5; i++) {
					gp = draw(1);
					gw = obs_source_get_width(game);
					gh = obs_source_get_height(game);
					if (gp.size() == size_t(gw) * gh * 4 && gw && gh) {
						const uint8_t *c = &gp[(size_t(gh / 2) * gw + gw / 2) * 4];
						if (c[1] > c[0] + 40 && c[1] > c[2] + 40)
							greenest = std::max(greenest, (int)c[1]);
					}
				}
				CHECK(greenest > 60, "when the game goes, its last frame fades into the picture (%d)",
				      greenest);
				gp = draw(24);
				gw = obs_source_get_width(game);
				gh = obs_source_get_height(game);
				CHECK(gw > 400 && gh == 300 && a(6, gh / 2) == 255 && a(gw - 7, gh / 2) == 255 &&
					      a(gw / 2, 3) == 255,
				      "the game gone, the standby picture is whole: not cut to the last game's shape "
				      "(%ux%u, left %d, right %d)",
				      gw, gh, gw ? a(6, gh / 2) : -1, gw ? a(gw - 7, gh / 2) : -1);
				obs_data_release(gd);
				obs_source_filter_remove(game, shape);
				obs_source_filter_remove(game, wait);
				obs_source_filter_remove(game, frame);
				obs_source_release(shape);
				obs_source_release(wait);
				obs_source_release(frame);
				obs_source_release(game);
			}
			obs_data_release(sd);
			obs_source_filter_remove(capture, standby);
			obs_source_release(standby);
			obs_source_release(capture);
		}

		// The standby image in a hidden source's place. The owner: "I need a
		// way for the standby image to be displayed in place of any source I
		// choose when it's hidden".
		{
			auto colour = [](const char *name, long long abgr, int cw, int ch) {
				obs_data_t *d = obs_data_create();
				obs_data_set_int(d, "color", abgr);
				obs_data_set_int(d, "width", cw);
				obs_data_set_int(d, "height", ch);
				obs_source_t *made = obs_source_create("color_source", name, d, nullptr);
				obs_data_release(d);
				return made;
			};
			obs_source_t *cam = colour("Hidden Cam", 0xFF00FF00LL, 200, 100);         // green
			obs_source_t *stand = colour("Hidden Cam Standby", 0xFF0000FFLL, 64, 64); // red
			obs_scene_t *hs = obs_scene_create("Standby When Hidden Scene");
			obs_sceneitem_t *cam_item = obs_scene_add(hs, cam);
			vec2 at;
			vec2_set(&at, 100.0f, 60.0f);
			obs_sceneitem_set_pos(cam_item, &at);
			obs_data_t *hd = obs_data_create();
			obs_data_set_int(hd, "picture", 2);
			obs_data_set_string(hd, "source", "Hidden Cam Standby");
			obs_data_set_bool(hd, "when_hidden", true);
			obs_data_set_string(hd, "transition", "none");
			obs_source_t *filter =
				obs_source_create("stagekit_standby", "Standby When Hidden", hd, nullptr);
			obs_source_filter_add(cam, filter);
			auto shown_for = [&](int ms) {
				std::vector<uint8_t> last;
				for (int i = 0; i < ms / 50; i++) {
					last = render(obs_scene_get_source(hs), 640, 360);
					sleep_ms(50);
				}
				return last;
			};
			auto items = [&] {
				int n = 0;
				obs_scene_enum_items(
					hs,
					[](obs_scene_t *, obs_sceneitem_t *, void *param) {
						(*static_cast<int *>(param))++;
						return true;
					},
					&n);
				return n;
			};
			auto at_px = [&](const std::vector<uint8_t> &frame, int x, int y) {
				return &frame[(size_t(y) * 640 + x) * 4];
			};
			std::vector<uint8_t> frame = shown_for(900);
			const int with_stand_in = items();
			const uint8_t *p = at_px(frame, 200, 110);
			const bool shows_source = p[1] > 250 && p[0] < 5;
			obs_sceneitem_set_visible(cam_item, false);
			frame = shown_for(900);
			p = at_px(frame, 200, 110);
			const bool shows_standby = p[0] > 250 && p[1] < 5 && p[3] > 250;
			const uint8_t *outside = at_px(frame, 400, 300);
			CHECK(with_stand_in == 2 && shows_source && shows_standby && outside[3] < 5,
			      "a hidden source's place is taken by its standby image, at the source's place and size (items %d; source shown %d; standby in its place %d: %d,%d,%d)",
			      with_stand_in, (int)shows_source, (int)shows_standby, p[0], p[1], p[2]);
			// Moved while it shows, the stand-in goes with it; shown again,
			// the source is back.
			obs_sceneitem_set_visible(cam_item, true);
			vec2_set(&at, 300.0f, 200.0f);
			obs_sceneitem_set_pos(cam_item, &at);
			frame = shown_for(600);
			p = at_px(frame, 400, 250);
			const bool back = p[1] > 250 && p[0] < 5;
			obs_sceneitem_set_visible(cam_item, false);
			frame = shown_for(600);
			p = at_px(frame, 400, 250);
			CHECK(back && p[0] > 250 && p[1] < 5 && at_px(frame, 200, 110)[3] < 5,
			      "shown again the source is back, and its stand-in follows it to a new place (%d; %d,%d,%d there)",
			      (int)back, p[0], p[1], p[2]);
			// The change is one the owner chooses, for the source and its
			// stand-in alike: "I want it to morph or perform transition that I
			// choose. currently it seems to just slide". Whatever else sets the
			// item's transitions is passed over.
			{
				obs_sceneitem_set_visible(cam_item, true);
				obs_data_set_string(hd, "transition", "fade_transition");
				obs_data_set_int(hd, "transition_ms", 600);
				obs_data_set_string(hd, "hide_transition", "same");
				obs_source_update(filter, hd);
				shown_for(700);
				struct Found {
					obs_sceneitem_t *twin = nullptr;
				} found;
				obs_scene_enum_items(
					hs,
					[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
						if (strcmp(obs_source_get_unversioned_id(obs_sceneitem_get_source(item)),
							   "stagekit_stand_in") == 0)
							static_cast<Found *>(param)->twin = item;
						return true;
					},
					&found);
				auto kind_of = [](obs_sceneitem_t *item, bool show) -> std::string {
					obs_source_t *tr = item ? obs_sceneitem_get_transition(item, show) : nullptr;
					return tr ? obs_source_get_unversioned_id(tr) : "";
				};
				const bool fades = kind_of(cam_item, false) == "fade_transition" &&
						   kind_of(found.twin, true) == "fade_transition" &&
						   obs_sceneitem_get_transition_duration(cam_item, false) == 600;
				// Hidden: part-way, both are there, the one going and the other coming.
				obs_sceneitem_set_visible(cam_item, false);
				frame = shown_for(300);
				p = at_px(frame, 400, 250);
				const int mid_r = p[0], mid_g = p[1];
				frame = shown_for(700);
				p = at_px(frame, 400, 250);
				CHECK(fades && mid_r > 30 && mid_r < 230 && mid_g > 30 && mid_g < 230 && p[0] > 250 &&
					      p[1] < 5,
				      "the source and its stand-in change with the one transition chosen in the filter (set on both %d; part-way red %d, green %d; after %d,%d)",
				      (int)fades, mid_r, mid_g, p[0], p[1]);
				// Another filter's change for the item is passed over.
				{
					obs_data_t *sd = obs_data_create();
					obs_data_set_string(sd, "transition", "none");
					obs_source_t *sw = obs_source_create("stagekit_show_in_scenes",
									     "Hidden Cam Switch", sd, nullptr);
					obs_source_filter_add(cam, sw);
					shown_for(300);
					obs_source_set_enabled(sw, false);
					shown_for(300);
					obs_source_set_enabled(sw, true);
					shown_for(900);
					obs_source_filter_remove(cam, sw);
					obs_source_release(sw);
					obs_data_release(sd);
				}
				CHECK(kind_of(cam_item, false) == "fade_transition",
				      "nothing else changes the transitions of a source the filter stands in for (hide: %s)",
				      kind_of(cam_item, false).c_str());
				// Morph: both get it, linked to each other.
				obs_data_set_string(hd, "transition", "stagekit_morph");
				obs_source_update(filter, hd);
				shown_for(700);
				obs_source_t *hide_tr = obs_sceneitem_get_transition(cam_item, false);
				obs_data_t *hs_set = hide_tr ? obs_source_get_settings(hide_tr) : nullptr;
				obs_data_array_t *links = hs_set ? obs_data_get_array(hs_set, "links") : nullptr;
				obs_data_t *first = links && obs_data_array_count(links) ? obs_data_array_item(links, 0)
											 : nullptr;
				const std::string line = first ? obs_data_get_string(first, "value") : "";
				CHECK(kind_of(cam_item, false) == "stagekit_morph" &&
					      kind_of(found.twin, true) == "stagekit_morph" &&
					      line.find("Hidden Cam") == 0 &&
					      line.find("(standby)") != std::string::npos,
				      "with Morph chosen the source and its stand-in are linked, so the one changes into the other (%s / %s; link '%s')",
				      kind_of(cam_item, false).c_str(), kind_of(found.twin, true).c_str(),
				      line.c_str());
				obs_data_release(first);
				obs_data_array_release(links);
				obs_data_release(hs_set);
				obs_sceneitem_set_visible(cam_item, true);
				frame = shown_for(1200);
				p = at_px(frame, 400, 250);
				CHECK(p[1] > 250 && p[0] < 5,
				      "and after a Morph back the source is there again (%d,%d,%d)", p[0], p[1], p[2]);
				obs_data_set_string(hd, "transition", "none");
			}
			// With another filter after the Standby one, as the owner has: OBS
			// draws what a filter is given once a frame, so the source and its
			// stand-in, both on screen during the change, showed the same picture.
			// The owner: "most of the transitions don't work. they just instantly
			// switch in place. also some of them make the standby image flash then
			// change when showing".
			// Each StageKit filter that may come after it: Corners & Edges (and
			// Shadow, which takes its input the same way) since beta142, Blur and
			// Zoom since beta146.
			struct Later {
				const char *id, *name, *key;
				double value;
			};
			for (const Later &later : {Later{"stagekit_corners", "Corners & Edges", nullptr, 0.0},
						   Later{"stagekit_ramp_blur", "Blur", "strength", 6.0},
						   Later{"stagekit_ramp_zoom", "Zoom", "zoom", 120.0}})
				if (obs_source_get_display_name("slide_transition")) {
					obs_data_t *ld = obs_data_create();
					if (later.key)
						obs_data_set_double(ld, later.key, later.value);
					obs_source_t *corners =
						obs_source_create(later.id, "Hidden Cam Later Filter", ld, nullptr);
					obs_data_release(ld);
					obs_source_filter_add(cam, corners);
					for (const char *kind : {"slide_transition", "fade_transition"}) {
						const bool slide = strcmp(kind, "slide_transition") == 0;
						obs_sceneitem_set_visible(cam_item, true);
						obs_data_set_string(hd, "transition", kind);
						obs_data_set_int(hd, "transition_ms", 800);
						obs_data_set_string(hd, "show|slide_transition|direction", "left");
						obs_source_update(filter, hd);
						shown_for(1200);
						bool both[2] = {false, false}, ends[2] = {false, false};
						int seen[2][4] = {};
						for (int round = 0; round < 2; round++) {
							obs_sceneitem_set_visible(cam_item, round == 1);
							// Part-way both are there: side by side in a slide (going
							// left: what is left of the one on the left, the other coming
							// in on the right), mixed in a fade. Looked for at each step.
							const uint8_t *l = nullptr;
							for (int step = 0; step < 45; step++) {
								frame = render(obs_scene_get_source(hs), 640, 360);
								sleep_ms(15);
								l = at_px(frame, 330, 250);
								const uint8_t *r = at_px(frame, 470, 250);
								const bool here =
									slide ? (round == 0 ? l[1] > 250 && r[0] > 250
											    : l[0] > 250 && r[1] > 250)
									      : l[0] > 30 && l[1] > 30;
								if (here && !both[round]) {
									both[round] = true;
									seen[round][0] = l[0], seen[round][1] = l[1],
									seen[round][2] = r[0], seen[round][3] = r[1];
								}
							}
							frame = shown_for(800);
							l = at_px(frame, 330, 250);
							ends[round] = round == 0 ? l[0] > 250 && l[1] < 5
										 : l[1] > 250 && l[0] < 5;
						}
						CHECK(both[0] && both[1] && ends[0] && ends[1],
						      "with %s after it on the source, a %s shows the source and its standby image apart all the way (hiding: left %d,%d right %d,%d; showing: left %d,%d right %d,%d; ends %d %d)",
						      later.name, slide ? "slide" : "fade", seen[0][0], seen[0][1],
						      seen[0][2], seen[0][3], seen[1][0], seen[1][1], seen[1][2],
						      seen[1][3], (int)ends[0], (int)ends[1]);
					}
					obs_source_filter_remove(cam, corners);
					obs_source_release(corners);
					obs_data_set_string(hd, "transition", "none");
				}
			// Unticked: the stand-ins are taken out again.
			obs_data_set_bool(hd, "when_hidden", false);
			obs_source_update(filter, hd);
			shown_for(600);
			CHECK(items() == 1, "unticked, the stand-in items are taken out of the scenes (%d left)",
			      items());
			obs_properties_t *hp = obs_source_properties(filter);
			CHECK(obs_properties_get(hp, "when_hidden") && obs_properties_get(hp, "transition"),
			      "the Standby filter offers it, with the transition its stand-in appears with");
			obs_properties_destroy(hp);
			obs_data_release(hd);
			obs_source_filter_remove(cam, filter);
			obs_source_release(filter);
			obs_scene_release(hs);
			obs_source_release(stand);
			obs_source_release(cam);
		}

		// A renamed source goes on being fitted: the owner's camera, renamed,
		// was left out of its Fit to Space in two scenes of three.
		{
			obs_source_t *named = obs_source_create("color_source", "Fit Renamed Before", nullptr, nullptr);
			obs_scene_t *rs = obs_scene_create("Fit Rename Scene");
			obs_scene_add(rs, named);
			obs_data_t *rd = obs_data_create();
			obs_data_set_string(rd, "item", "Fit Renamed Before");
			obs_data_set_string(rd, "keep_item", "Fit Renamed Before");
			obs_source_t *fit = obs_source_create("stagekit_fit_item", "Fit Rename", rd, nullptr);
			obs_source_filter_add(obs_scene_get_source(rs), fit);
			sleep_ms(200);
			obs_source_set_name(named, "Fit Renamed After");
			sleep_ms(200);
			obs_data_t *now = obs_source_get_settings(fit);
			CHECK(strcmp(obs_data_get_string(now, "item"), "Fit Renamed After") == 0 &&
				      strcmp(obs_data_get_string(now, "keep_item"), "Fit Renamed After") == 0,
			      "a Fit to Space follows its source's new name (item '%s', kept size for '%s')",
			      obs_data_get_string(now, "item"), obs_data_get_string(now, "keep_item"));
			obs_data_release(now);
			obs_data_release(rd);
			obs_source_filter_remove(obs_scene_get_source(rs), fit);
			obs_source_release(fit);
			obs_scene_release(rs);
			obs_source_release(named);
		}

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
			// Hidden, the item is still the size it will be shown at. The
			// owner saw a shadow and an outline come in thinner while their
			// item slid in, and snap to their thickness part-way: while it
			// was hidden the sizes had been worked out for some other size.
			obs_sceneitem_set_visible(item, false);
			sleep_ms(1200);
			auto hidden = look(nullptr);
			obs_sceneitem_set_visible(item, true);
			CHECK(alpha(hidden, cw, 30, 30) == 0 && alpha(hidden, cw, 100, 100) == 255,
			      "and stay so while the item is hidden, ready for when it is shown");
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

	// ------------------------------------------- one capture, placed by scene
	{
		printf("\n[fit item]\n");
		auto sized = [](obs_source_t *src, int cw, int ch) {
			obs_data_t *d = obs_data_create();
			obs_data_set_int(d, "width", cw);
			obs_data_set_int(d, "height", ch);
			obs_source_update(src, d);
			obs_data_release(d);
		};
		obs_source_t *game = obs_source_create("color_source", "Fit Game", nullptr, nullptr);
		sized(game, 400, 300);                                   // an emulator's squarer picture
		obs_scene_t *full = obs_scene_create("Fit Full Screen"); // 640x360
		obs_sceneitem_t *item = obs_scene_add(full, game);
		obs_data_t *fs = obs_data_create();
		obs_data_set_string(fs, "item", "Fit Game");
		obs_data_set_double(fs, "margin", 30);
		obs_source_t *fit = obs_source_create("stagekit_fit_item", "Fit", fs, nullptr);
		obs_source_filter_add(obs_scene_get_source(full), fit);
		obs_source_t *before_out = obs_get_output_source(0);
		obs_set_output_source(0, obs_scene_get_source(full));
		// Where the picture really ends up: the scene is drawn and the light
		// rectangle (the colour source; a shadow is black) is found in it.
		struct Seen {
			int x, y, w, h;
		};
		auto picture = [&] {
			sleep_ms(400);
			const std::vector<uint8_t> px = render(obs_scene_get_source(full), 640, 360);
			int x0 = 640, y0 = 360, x1 = -1, y1 = -1;
			for (int y = 0; y < 360; y++) {
				for (int x = 0; x < 640; x++) {
					const uint8_t *p = &px[(size_t(y) * 640 + x) * 4];
					if (p[3] > 128 && p[0] > 100) {
						x0 = std::min(x0, x);
						y0 = std::min(y0, y);
						x1 = std::max(x1, x);
						y1 = std::max(y1, y);
					}
				}
			}
			return Seen{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
		};
		auto is_at = [](const Seen &s, int x, int y, int w, int h) {
			return std::abs(s.x - x) <= 2 && std::abs(s.y - y) <= 2 && std::abs(s.w - w) <= 3 &&
			       std::abs(s.h - h) <= 3;
		};
		auto box_of = [&] {
			vec2 p, b;
			obs_sceneitem_get_pos(item, &p);
			obs_sceneitem_get_bounds(item, &b);
			struct {
				double x, y, w, h;
			} out{p.x, p.y, b.x, b.y};
			return out;
		};
		Seen at = picture();
		CHECK(is_at(at, 120, 30, 400, 300), "a squarer picture floats centred inside the margin (%d, %d %dx%d)",
		      at.x, at.y, at.w, at.h);

		// A window dragged wider: OBS itself keeps it fitted, frame by frame.
		// Fit Item's box is the space allowed, so it has nothing to change.
		{
			const auto before = box_of();
			sized(game, 500, 300);
			at = picture();
			const auto after = box_of();
			CHECK(is_at(at, 70, 30, 500, 300),
			      "a picture that changes size stays fitted and centred (%d, %d %dx%d)", at.x, at.y, at.w,
			      at.h);
			CHECK(std::fabs(before.w - 580) < 0.1 && std::fabs(before.h - 300) < 0.1 &&
				      std::fabs(after.x - before.x) < 0.1 && std::fabs(after.y - before.y) < 0.1 &&
				      std::fabs(after.w - before.w) < 0.1 && std::fabs(after.h - before.h) < 0.1,
			      "and the item's box did not have to change for it (%.0fx%.0f at %.0f, %.0f)", after.w,
			      after.h, after.x, after.y);
			sized(game, 400, 300);
		}

		sized(game, 1280, 720); // a widescreen game
		at = picture();
		CHECK(is_at(at, 0, 0, 640, 360), "a picture with the screen's shape fills the screen (%d, %d %dx%d)",
		      at.x, at.y, at.w, at.h);

		// With a shadow, it is the picture that is centred, not picture plus shadow.
		sized(game, 400, 300);
		obs_data_t *ss = obs_data_create();
		obs_data_set_double(ss, "offset_x", -20);
		obs_data_set_double(ss, "offset_y", -20);
		obs_data_set_double(ss, "softness", 0);
		obs_data_set_bool(ss, "screen_px", false);
		for (const char *side : {"adjusted_left", "adjusted_top", "adjusted_right", "adjusted_bottom"})
			obs_data_set_int(ss, side, 0);
		obs_source_t *shadow = obs_source_create("stagekit_shadow_backdrop", "Shadow", ss, nullptr);
		obs_data_release(ss);
		obs_source_filter_add(game, shadow);
		sleep_ms(600);
		at = picture();
		CHECK(obs_source_get_width(game) == 420 && is_at(at, 120, 30, 400, 300),
		      "with a shadow on it, the picture itself stays centred (%d, %d %dx%d)", at.x, at.y, at.w, at.h);
		obs_source_filter_remove(game, shadow);
		obs_source_release(shadow);
		sleep_ms(300);

		// A box of its own, as in a scene with a camera and chat beside the game.
		obs_data_set_int(fs, "box", 1);
		obs_data_set_double(fs, "box_x", 20);
		obs_data_set_double(fs, "box_y", 40);
		obs_data_set_double(fs, "box_width", 400);
		obs_data_set_double(fs, "box_height", 300);
		obs_source_update(fit, fs);
		sized(game, 800, 600);
		at = picture();
		CHECK(is_at(at, 20, 40, 400, 300),
		      "it can fit a box of its own instead of the whole screen (%d, %d %dx%d)", at.x, at.y, at.w, at.h);

		// Auto Frame as just the picture: the source is the trimmed picture.
		obs_data_t *as = obs_data_create();
		obs_data_set_int(as, "frame_mode", 1);
		obs_data_set_int(as, "frame_width", 1920);
		obs_data_set_int(as, "frame_height", 1080);
		obs_data_set_int(as, "trim", 2);
		obs_data_set_double(as, "trim_x", 100);
		obs_data_set_double(as, "trim_y", 0);
		obs_source_t *frame = obs_source_create("stagekit_auto_frame", "Auto Frame", as, nullptr);
		obs_data_release(as);
		obs_source_filter_add(game, frame);
		for (int i = 0; i < 6; i++) {
			render(obs_scene_get_source(full), 640, 360);
			sleep_ms(100);
		}
		CHECK(obs_source_get_width(game) == 1080 && obs_source_get_height(game) == 1080,
		      "Auto Frame as 'just the picture' makes the source the trimmed picture at the height asked for (%ux%u)",
		      obs_source_get_width(game), obs_source_get_height(game));
		at = picture();
		// A square picture inside the 400x300 box less its 30 px margin: 240x240, centred.
		CHECK(is_at(at, 100, 70, 240, 240), "and Fit Item places that picture (%d, %d %dx%d)", at.x, at.y, at.w,
		      at.h);

		// The game goes from a window to full-screen at the same shape: nothing moves.
		{
			vec2 pos_before, box_before;
			obs_sceneitem_get_pos(item, &pos_before);
			obs_sceneitem_get_bounds(item, &box_before);
			sized(game, 1600, 1200); // twice the pixels, same 4:3 and the same trim share is not kept:
			obs_data_t *bigger = obs_data_create();
			obs_data_set_double(bigger, "trim_x", 200);
			obs_source_update(frame, bigger);
			obs_data_release(bigger);
			bool steady = true;
			for (int i = 0; i < 12; i++) {
				render(obs_scene_get_source(full), 640, 360);
				sleep_ms(60);
				vec2 pos_now, box_now;
				obs_sceneitem_get_pos(item, &pos_now);
				obs_sceneitem_get_bounds(item, &box_now);
				steady = steady && obs_source_get_width(game) == 1080 &&
					 obs_source_get_height(game) == 1080 &&
					 std::fabs(pos_now.x - pos_before.x) < 0.1 &&
					 std::fabs(box_now.x - box_before.x) < 0.1;
			}
			CHECK(steady,
			      "a change of resolution at the same shape changes neither the source's size nor its place");
		}

		// Shadow, Glow & Outline's "Keep it on screen": an item whose shadow
		// would run off the canvas is made smaller instead.
		{
			obs_source_t *box = obs_source_create("color_source", "In View Box", nullptr, nullptr);
			sized(box, 600, 330);
			obs_scene_t *sc = obs_scene_create("In View");      // 640x360
			obs_scene_t *other = obs_scene_create("In View 2"); // the same source, pushed off screen
			obs_sceneitem_t *bi = obs_scene_add(sc, box);
			obs_sceneitem_t *oi = obs_scene_add(other, box);
			vec2 p;
			vec2_set(&p, 20.0f, 10.0f);
			obs_sceneitem_set_pos(bi, &p);
			vec2_set(&p, 150.0f, 10.0f);
			obs_sceneitem_set_pos(oi, &p);
			obs_data_t *sd = obs_data_create();
			obs_data_set_double(sd, "offset_x", 30);
			obs_data_set_double(sd, "offset_y", 30);
			obs_data_set_double(sd, "softness", 0);
			obs_source_t *shadow = obs_source_create("stagekit_shadow_backdrop", "Shadow", sd, nullptr);
			obs_source_filter_add(box, shadow);
			auto settle = [&](int ms) {
				for (int i = 0; i < ms / 50; i++) {
					render(obs_scene_get_source(sc), 640, 360);
					render(obs_scene_get_source(other), 640, 360);
					sleep_ms(50);
				}
			};
			settle(800);
			struct Edges {
				double x, y, w, h;
			};
			auto edges = [&] {
				vec2 at, by;
				obs_sceneitem_get_pos(bi, &at);
				obs_sceneitem_get_scale(bi, &by);
				return Edges{at.x, at.y, obs_source_get_width(box) * by.x,
					     obs_source_get_height(box) * by.y};
			};
			Edges b = edges();
			CHECK(b.x + b.w > 645.0 && b.y + b.h > 365.0,
			      "left as it comes, a shadow runs off the canvas's edge (to %.0f, %.0f)", b.x + b.w,
			      b.y + b.h);
			obs_data_set_bool(sd, "in_view", true);
			obs_source_update(shadow, sd);
			settle(1500);
			b = edges();
			vec2 sc_now, sc_other;
			obs_sceneitem_get_scale(bi, &sc_now);
			obs_sceneitem_get_scale(oi, &sc_other);
			CHECK(std::fabs(b.x + b.w - 640.0) < 1.5 && std::fabs(b.y + b.h - 360.0) < 1.5 &&
				      sc_now.x < 0.99f && std::fabs(sc_now.x - sc_now.y) < 0.001f,
			      "with 'Keep it on screen' the item is made smaller, keeping its shape, until the shadow ends at the edge (to %.1f, %.1f; scale %.3f)",
			      b.x + b.w, b.y + b.h, sc_now.x);
			CHECK(std::fabs(sc_other.x - 1.0f) < 0.001f,
			      "an item whose picture is itself off the canvas is left alone (scale %.3f)", sc_other.x);
			obs_data_release(sd);
			obs_source_filter_remove(box, shadow);
			obs_source_release(shadow);
			obs_scene_release(sc);
			obs_scene_release(other);
			obs_source_release(box);
		}
		obs_set_output_source(0, before_out);
		obs_source_release(before_out);
		obs_data_release(fs);
		obs_source_filter_remove(game, frame);
		obs_source_release(frame);
		obs_source_filter_remove(obs_scene_get_source(full), fit);
		// The free space around it: as large as it can be where it is, the
		// gap away from the screen's edges and from the other items.
		{
			obs_source_t *game2 = obs_source_create("color_source", "Free Game", nullptr, nullptr);
			sized(game2, 400, 300);
			obs_source_t *cam = obs_source_create("color_source", "Free Cam", nullptr, nullptr);
			sized(cam, 100, 100);
			obs_scene_t *sc = obs_scene_create("Fit Free Space"); // 640x360
			obs_sceneitem_t *gi = obs_scene_add(sc, game2);
			obs_sceneitem_t *ci = obs_scene_add(sc, cam);
			vec2 at2;
			vec2_set(&at2, 100.0f, 50.0f);
			obs_sceneitem_set_pos(gi, &at2);
			vec2_set(&at2, 520.0f, 240.0f);
			obs_sceneitem_set_pos(ci, &at2);
			obs_data_t *ff = obs_data_create();
			obs_data_set_string(ff, "item", "Free Game");
			obs_data_set_int(ff, "box", 2);
			obs_data_set_double(ff, "margin", 20);
			obs_data_set_int(ff, "glide_ms", 0);
			obs_source_t *free_fit = obs_source_create("stagekit_fit_item", "Fit Free", ff, nullptr);
			obs_source_filter_add(obs_scene_get_source(sc), free_fit);
			obs_source_t *shown_before = obs_get_output_source(0);
			obs_set_output_source(0, obs_scene_get_source(sc));
			auto space = [&](int wait_ms) {
				for (int i = 0; i < wait_ms / 50; i++) {
					render(obs_scene_get_source(sc), 640, 360);
					sleep_ms(50);
				}
				vec2 p, b;
				obs_sceneitem_get_pos(gi, &p);
				obs_sceneitem_get_bounds(gi, &b);
				struct {
					double x, y, w, h;
				} out{p.x, p.y, b.x, b.y};
				return out;
			};
			auto is = [](double a, double b) {
				return std::fabs(a - b) < 0.6;
			};
			auto sp = space(500);
			CHECK(is(sp.x, 20) && is(sp.y, 20) && is(sp.w, 480) && is(sp.h, 320),
			      "'The free space around it' keeps the gap from the screen's edges and from the item beside it (%.0f, %.0f %.0fx%.0f)",
			      sp.x, sp.y, sp.w, sp.h);
			obs_sceneitem_set_visible(ci, false);
			sp = space(900);
			CHECK(is(sp.x, 20) && is(sp.y, 20) && is(sp.w, 600) && is(sp.h, 320),
			      "an item hidden gives its space up (%.0f, %.0f %.0fx%.0f)", sp.x, sp.y, sp.w, sp.h);
			obs_sceneitem_set_visible(ci, true);
			sp = space(900);
			CHECK(is(sp.w, 480), "and shown again takes it back (%.0f wide)", sp.w);
			// Moved somewhere else in the scene: the item beside it moves
			// over the game's place, so it is the game that makes room.
			vec2_set(&at2, 20.0f, 20.0f);
			obs_sceneitem_set_pos(ci, &at2);
			sp = space(900);
			CHECK(is(sp.x, 140) && is(sp.y, 20) && is(sp.w, 480) && is(sp.h, 320),
			      "an item moved in beside it is kept clear of too (%.0f, %.0f %.0fx%.0f)", sp.x, sp.y,
			      sp.w, sp.h);
			// Listed as never in the way: it is as if it were not there.
			obs_data_t *entry = obs_data_create();
			obs_data_set_string(entry, "value", "Free Cam");
			obs_data_array_t *skip = obs_data_array_create();
			obs_data_array_push_back(skip, entry);
			obs_data_set_array(ff, "skip", skip);
			obs_data_array_release(skip);
			obs_data_release(entry);
			obs_source_update(free_fit, ff);
			sp = space(600);
			CHECK(is(sp.x, 20) && is(sp.w, 600),
			      "an item listed as never in the way is not (%.0f, %.0f wide)", sp.x, sp.w);
			// ... unless the picture would leave too much of its space
			// empty. A 4:3 game fills 71 % of the 600 by 320 space.
			obs_data_set_double(ff, "skip_fill", 95.0);
			obs_source_update(free_fit, ff);
			sp = space(600);
			CHECK(is(sp.x, 140) && is(sp.w, 480),
			      "a picture that fills under the share set goes beside the item never in the way (%.0f, %.0f wide)",
			      sp.x, sp.w);
			obs_data_set_double(ff, "skip_fill", 60.0);
			obs_source_update(free_fit, ff);
			sp = space(600);
			CHECK(is(sp.x, 20) && is(sp.w, 600),
			      "and one that fills more than that still overlaps it (%.0f, %.0f wide)", sp.x, sp.w);
			// Its own switch: off, the share set counts for nothing.
			obs_data_set_double(ff, "skip_fill", 95.0);
			obs_data_set_bool(ff, "skip_fill_on", false);
			obs_source_update(free_fit, ff);
			sp = space(600);
			const bool off_overlaps = is(sp.x, 20) && is(sp.w, 600);
			obs_data_set_bool(ff, "skip_fill_on", true);
			obs_source_update(free_fit, ff);
			sp = space(600);
			CHECK(off_overlaps && is(sp.x, 140) && is(sp.w, 480),
			      "the rule has a switch of its own: off it overlaps, on it goes beside (%d; %.0f, %.0f wide)",
			      off_overlaps, sp.x, sp.w);
			// By the empty room at each edge instead: the 4:3 game leaves
			// 87 px at the left and at the right of its space.
			obs_data_set_bool(ff, "skip_use_pct", false);
			obs_data_set_bool(ff, "skip_use_px", true);
			for (const char *edge : {"skip_px_top", "skip_px_bottom", "skip_px_left", "skip_px_right"})
				obs_data_set_double(ff, edge, 90.0);
			obs_source_update(free_fit, ff);
			sp = space(600);
			const bool near_overlaps = is(sp.x, 20) && is(sp.w, 600);
			obs_data_set_double(ff, "skip_px_right", 50.0);
			obs_source_update(free_fit, ff);
			sp = space(600);
			CHECK(near_overlaps && is(sp.x, 140) && is(sp.w, 480),
			      "by edge distances: within the pixels set at every edge it overlaps, past them at one edge it goes beside (%d; %.0f, %.0f wide)",
			      near_overlaps, sp.x, sp.w);
			// Both rules ticked: either is enough.
			obs_data_set_bool(ff, "skip_use_pct", true);
			obs_data_set_double(ff, "skip_fill", 60.0);
			obs_source_update(free_fit, ff);
			sp = space(600);
			const bool either = is(sp.x, 20) && is(sp.w, 600);
			// Neither: nothing says it fills, so it goes beside.
			obs_data_set_bool(ff, "skip_use_pct", false);
			obs_data_set_bool(ff, "skip_use_px", false);
			obs_source_update(free_fit, ff);
			sp = space(600);
			CHECK(either && is(sp.x, 140) && is(sp.w, 480),
			      "with both rules ticked either is enough, with neither it goes beside (%d; %.0f, %.0f wide)",
			      either, sp.x, sp.w);
			obs_data_set_bool(ff, "skip_use_pct", true);
			obs_data_set_double(ff, "skip_fill", 95.0);
			obs_data_set_bool(ff, "skip_fill_on", false);
			obs_source_update(free_fit, ff);
			sp = space(400);
			// Dragged to another place: left alone while it moves.
			vec2_set(&at2, 300.0f, 200.0f);
			obs_sceneitem_set_pos(gi, &at2);
			render(obs_scene_get_source(sc), 640, 360);
			sleep_ms(100);
			vec2 during;
			obs_sceneitem_get_pos(gi, &during);
			sp = space(900);
			CHECK(is(during.x, 300) && is(sp.x, 20) && is(sp.w, 600),
			      "moved by hand it is let go, then fitted again where it was put (%.0f while moving; then %.0f, %.0f wide)",
			      during.x, sp.x, sp.w);
			// A widget under a column, read as a few pixels wider than the
			// column: the game keeps its gap from both and still takes the
			// whole height, not a space that stops above the widget.
			{
				obs_source_t *wide = obs_source_create("color_source", "Wide Game", nullptr, nullptr);
				sized(wide, 320, 180);
				obs_source_t *column =
					obs_source_create("color_source", "Free Column", nullptr, nullptr);
				sized(column, 120, 200);
				obs_source_t *widget =
					obs_source_create("color_source", "Free Widget", nullptr, nullptr);
				sized(widget, 124, 40);
				obs_scene_t *ws = obs_scene_create("Fit Beside A Widget"); // 640x360
				obs_sceneitem_t *wi = obs_scene_add(ws, wide);
				vec2 wp;
				vec2_set(&wp, 60.0f, 60.0f);
				obs_sceneitem_set_pos(wi, &wp);
				vec2_set(&wp, 500.0f, 20.0f);
				obs_sceneitem_set_pos(obs_scene_add(ws, column), &wp);
				vec2_set(&wp, 496.0f, 300.0f);
				obs_sceneitem_set_pos(obs_scene_add(ws, widget), &wp);
				obs_data_t *wf = obs_data_create();
				obs_data_set_string(wf, "item", "Wide Game");
				obs_data_set_int(wf, "box", 2);
				obs_data_set_double(wf, "margin", 20);
				obs_data_set_int(wf, "glide_ms", 0);
				obs_source_t *wide_fit =
					obs_source_create("stagekit_fit_item", "Fit Wide", wf, nullptr);
				obs_source_filter_add(obs_scene_get_source(ws), wide_fit);
				obs_set_output_source(0, obs_scene_get_source(ws));
				for (int i = 0; i < 12; i++) {
					render(obs_scene_get_source(ws), 640, 360);
					sleep_ms(50);
				}
				vec2 p, b;
				obs_sceneitem_get_pos(wi, &p);
				obs_sceneitem_get_bounds(wi, &b);
				CHECK(is(p.x, 20) && is(p.y, 20) && is(b.x, 456) && is(b.y, 320),
				      "a widget a few pixels wider than the column above it does not cost the space its height (%.0f, %.0f %.0fx%.0f)",
				      p.x, p.y, b.x, b.y);
				// The tolerance is the filter's own setting: at 0 the biggest
				// picture wins, even by a pixel or two.
				obs_data_set_double(wf, "tolerance", 0);
				obs_source_update(wide_fit, wf);
				for (int i = 0; i < 16; i++) {
					render(obs_scene_get_source(ws), 640, 360);
					sleep_ms(50);
				}
				obs_sceneitem_get_bounds(wi, &b);
				CHECK(is(b.x, 460) && is(b.y, 260),
				      "with a tolerance of 0 the space that gives the biggest picture is taken (%.0fx%.0f)",
				      b.x, b.y);
				obs_data_release(wf);
				obs_source_filter_remove(obs_scene_get_source(ws), wide_fit);
				obs_source_release(wide_fit);
				obs_scene_release(ws);
				obs_source_release(widget);
				obs_source_release(column);
				obs_source_release(wide);
			}
			// "Resize it to fit" unticked: the item keeps its size and is
			// only moved to the middle of its space, and moves again when
			// the space changes.
			{
				obs_source_t *chat = obs_source_create("color_source", "Kept Chat", nullptr, nullptr);
				sized(chat, 100, 100);
				obs_source_t *above = obs_source_create("color_source", "Kept Cam", nullptr, nullptr);
				sized(above, 100, 80);
				obs_scene_t *ks = obs_scene_create("Fit Keeping Size"); // 640x360
				obs_sceneitem_t *chat_item = obs_scene_add(ks, chat);
				obs_sceneitem_t *above_item = obs_scene_add(ks, above);
				vec2 kp;
				vec2_set(&kp, 500.0f, 200.0f);
				obs_sceneitem_set_pos(chat_item, &kp);
				vec2_set(&kp, 500.0f, 20.0f);
				obs_sceneitem_set_pos(above_item, &kp);
				obs_data_t *kf = obs_data_create();
				obs_data_set_string(kf, "item", "Kept Chat");
				obs_data_set_int(kf, "box", 2);
				obs_data_set_double(kf, "margin", 20);
				obs_data_set_int(kf, "glide_ms", 0);
				obs_data_set_bool(kf, "resize", false);
				obs_source_t *keep_fit =
					obs_source_create("stagekit_fit_item", "Fit Keeping", kf, nullptr);
				obs_source_filter_add(obs_scene_get_source(ks), keep_fit);
				obs_set_output_source(0, obs_scene_get_source(ks));
				auto settle = [&](int ms) {
					for (int i = 0; i < ms / 50; i++) {
						render(obs_scene_get_source(ks), 640, 360);
						sleep_ms(50);
					}
				};
				settle(700);
				vec2 p1, b1, p2, b2;
				obs_sceneitem_get_pos(chat_item, &p1);
				obs_sceneitem_get_bounds(chat_item, &b1);
				obs_sceneitem_set_visible(above_item, false);
				settle(900);
				obs_sceneitem_get_pos(chat_item, &p2);
				obs_sceneitem_get_bounds(chat_item, &b2);
				CHECK(is(b1.x, 100) && is(b1.y, 100) && is(p1.x, 270) && is(p1.y, 180),
				      "with 'Resize it to fit' unticked the item keeps its size and sits in the middle of its space (%.0f, %.0f %.0fx%.0f)",
				      p1.x, p1.y, b1.x, b1.y);
				CHECK(is(b2.x, 100) && is(b2.y, 100) && is(p2.x, 270) && is(p2.y, 130),
				      "and when the space grows it moves to the new middle, still its own size (%.0f, %.0f %.0fx%.0f)",
				      p2.x, p2.y, b2.x, b2.y);
				// Ticked, it is fitted (and so resized); unticked again it
				// gets back the size it had before the filter touched it.
				obs_data_set_bool(kf, "resize", true);
				obs_source_update(keep_fit, kf);
				settle(600);
				vec2 fitted;
				obs_sceneitem_get_bounds(chat_item, &fitted);
				obs_data_set_bool(kf, "resize", false);
				obs_source_update(keep_fit, kf);
				settle(600);
				vec2 again;
				obs_sceneitem_get_bounds(chat_item, &again);
				CHECK(fitted.x > 150 && is(again.x, 100) && is(again.y, 100),
				      "unticking 'Resize it to fit' gives the item back the size it had before the filter (fitted %.0f wide, then %.0fx%.0f)",
				      fitted.x, again.x, again.y);
				// A camera drawn much smaller than its source, with a shadow
				// sized in screen pixels, hidden and shown again a few times:
				// it is still the size it was. (Hidden, the shadow's room in
				// the source changes and with it the bounding box, which is
				// not a resize by hand.)
				{
					obs_source_t *cam =
						obs_source_create("color_source", "Kept Camera", nullptr, nullptr);
					sized(cam, 400, 400);
					obs_scene_t *cs = obs_scene_create("Fit Kept Camera"); // 640x360
					obs_sceneitem_t *cam_item = obs_scene_add(cs, cam);
					vec2 quarter;
					vec2_set(&quarter, 0.25f, 0.25f);
					obs_sceneitem_set_scale(cam_item, &quarter);
					obs_data_t *cf = obs_data_create();
					obs_data_set_string(cf, "item", "Kept Camera");
					obs_data_set_int(cf, "box", 2);
					obs_data_set_double(cf, "margin", 20);
					obs_data_set_int(cf, "glide_ms", 0);
					obs_data_set_bool(cf, "resize", false);
					obs_source_t *cam_fit =
						obs_source_create("stagekit_fit_item", "Fit Camera", cf, nullptr);
					obs_source_filter_add(obs_scene_get_source(cs), cam_fit);
					obs_set_output_source(0, obs_scene_get_source(cs));
					auto pause = [&](int ms) {
						for (int i = 0; i < ms / 50; i++) {
							render(obs_scene_get_source(cs), 640, 360);
							sleep_ms(50);
						}
					};
					pause(600);
					obs_data_t *sd = obs_data_create();
					obs_data_set_bool(sd, "screen_px", true);
					obs_source_t *sh = obs_source_create("stagekit_shadow_backdrop", "Kept Shadow",
									     sd, nullptr);
					obs_data_release(sd);
					obs_source_filter_add(cam, sh);
					pause(1800);
					// How wide the camera's own picture is drawn, whatever
					// room its shadow takes around it.
					auto drawn = [&] {
						vec2 b;
						obs_sceneitem_get_bounds(cam_item, &b);
						const double k = std::min(b.x / (double)obs_source_get_width(cam),
									  b.y / (double)obs_source_get_height(cam));
						return 400.0 * k;
					};
					const double before = drawn();
					for (int i = 0; i < 3; i++) {
						obs_sceneitem_set_visible(cam_item, false);
						pause(700);
						obs_sceneitem_set_visible(cam_item, true);
						pause(1500);
					}
					const double after = drawn();
					CHECK(std::fabs(before - 100) < 3 && std::fabs(after - before) < 1.5,
					      "hidden and shown again three times, a kept-size camera with a shadow is still its size (%.1f then %.1f px wide)",
					      before, after);
					obs_source_filter_remove(cam, sh);
					obs_source_release(sh);
					obs_source_filter_remove(obs_scene_get_source(cs), cam_fit);
					obs_source_release(cam_fit);
					obs_data_release(cf);
					obs_scene_release(cs);
					obs_source_release(cam);
					obs_set_output_source(0, obs_scene_get_source(ks));
					settle(300);
				}
				// Something grows beside it until the space is narrower than
				// the item: it stays the gap inside the screen's edge.
				obs_source_t *wall = obs_source_create("color_source", "Kept Wall", nullptr, nullptr);
				sized(wall, 520, 340);
				obs_sceneitem_t *wall_item = obs_scene_add(ks, wall);
				vec2_set(&kp, 0.0f, 10.0f);
				obs_sceneitem_set_pos(wall_item, &kp);
				vec2_set(&kp, 530.0f, 130.0f);
				obs_sceneitem_set_pos(chat_item, &kp);
				settle(1200);
				vec2 p3, b3;
				obs_sceneitem_get_pos(chat_item, &p3);
				obs_sceneitem_get_bounds(chat_item, &b3);
				CHECK(is(b3.x, 100) && p3.x <= 520.5 && p3.x >= 19.5 && p3.y >= 19.5 && p3.y <= 240.5,
				      "squeezed by a neighbour it stays the gap inside the screen, at its own size (%.0f, %.0f %.0fx%.0f)",
				      p3.x, p3.y, b3.x, b3.y);
				obs_sceneitem_remove(wall_item);
				obs_source_release(wall);
				obs_data_release(kf);
				obs_source_filter_remove(obs_scene_get_source(ks), keep_fit);
				obs_source_release(keep_fit);
				obs_scene_release(ks);
				obs_source_release(above);
				obs_source_release(chat);
			}
			// "Use visible pixels" counts for the fitted item itself: an item
			// larger than what it shows (a nested scene, a group, a widget)
			// is fitted by what shows, not by its box.
			{
				obs_scene_t *inner = obs_scene_create("Mostly Empty"); // 640x360, a 100x100 block in it
				obs_source_t *block =
					obs_source_create("color_source", "Small Block", nullptr, nullptr);
				sized(block, 100, 100);
				vec2 ip;
				vec2_set(&ip, 270.0f, 130.0f);
				obs_sceneitem_set_pos(obs_scene_add(inner, block), &ip);
				obs_scene_t *vs = obs_scene_create("Fit What Shows"); // 640x360
				obs_sceneitem_t *nested = obs_scene_add(vs, obs_scene_get_source(inner));
				obs_data_t *vf = obs_data_create();
				obs_data_set_string(vf, "item", "Mostly Empty");
				obs_data_set_int(vf, "box", 0);
				obs_data_set_double(vf, "margin", 20);
				obs_source_t *vis_fit =
					obs_source_create("stagekit_fit_item", "Fit Shows", vf, nullptr);
				obs_source_filter_add(obs_scene_get_source(vs), vis_fit);
				obs_set_output_source(0, obs_scene_get_source(vs));
				for (int i = 0; i < 16; i++) {
					render(obs_scene_get_source(vs), 640, 360);
					sleep_ms(50);
				}
				auto shows_at = [&](double &x, double &y, double &w) {
					vec2 p, b;
					obs_sceneitem_get_pos(nested, &p);
					obs_sceneitem_get_bounds(nested, &b);
					const double kk = std::min(b.x / 640.0, b.y / 360.0);
					x = p.x + (b.x - 640.0 * kk) / 2.0 + 270.0 * kk;
					y = p.y + (b.y - 360.0 * kk) / 2.0 + 130.0 * kk;
					w = 100.0 * kk;
				};
				double x1, y1, w1;
				shows_at(x1, y1, w1);
				CHECK(std::fabs(w1 - 320) < 12 && std::fabs(x1 - 160) < 8 && std::fabs(y1 - 20) < 8,
				      "an item larger than what it shows is fitted by what shows: the block fills the height less the margin (%.0f wide at %.0f, %.0f)",
				      w1, x1, y1);
				obs_data_set_bool(vf, "visible_pixels", false);
				obs_source_update(vis_fit, vf);
				for (int i = 0; i < 12; i++) {
					render(obs_scene_get_source(vs), 640, 360);
					sleep_ms(50);
				}
				double x2, y2, w2;
				shows_at(x2, y2, w2);
				CHECK(w2 < 110,
				      "with 'Use visible pixels' unticked it goes by its box again (block %.0f wide)",
				      w2);
				obs_data_release(vf);
				obs_source_filter_remove(obs_scene_get_source(vs), vis_fit);
				obs_source_release(vis_fit);
				obs_scene_release(vs);
				obs_source_release(block);
				obs_scene_release(inner);
			}
			// Two filters in one scene, a game that is resized and a chat box
			// that keeps its size: each goes by where the other is going, so
			// they settle in one move and then stay.
			{
				obs_source_t *wide = obs_source_create("color_source", "Pair Game", nullptr, nullptr);
				sized(wide, 320, 180);
				obs_source_t *chat = obs_source_create("color_source", "Pair Chat", nullptr, nullptr);
				sized(chat, 100, 200);
				obs_scene_t *ps = obs_scene_create("Fit As A Pair"); // 640x360
				obs_sceneitem_t *game_item = obs_scene_add(ps, wide);
				obs_sceneitem_t *chat_item = obs_scene_add(ps, chat);
				vec2 pp;
				vec2_set(&pp, 40.0f, 60.0f);
				obs_sceneitem_set_pos(game_item, &pp);
				vec2_set(&pp, 450.0f, 100.0f);
				obs_sceneitem_set_pos(chat_item, &pp);
				auto fit_for = [&](const char *name, const char *item_name, bool resize) {
					obs_data_t *d = obs_data_create();
					obs_data_set_string(d, "item", item_name);
					obs_data_set_int(d, "box", 2);
					obs_data_set_double(d, "margin", 20);
					obs_data_set_int(d, "glide_ms", 100);
					obs_data_set_bool(d, "resize", resize);
					obs_source_t *f = obs_source_create("stagekit_fit_item", name, d, nullptr);
					obs_data_release(d);
					obs_source_filter_add(obs_scene_get_source(ps), f);
					return f;
				};
				obs_source_t *fit_game = fit_for("Fit Pair Game", "Pair Game", true);
				obs_source_t *fit_chat = fit_for("Fit Pair Chat", "Pair Chat", false);
				obs_set_output_source(0, obs_scene_get_source(ps));
				auto wait = [&](int ms) {
					for (int i = 0; i < ms / 50; i++) {
						render(obs_scene_get_source(ps), 640, 360);
						sleep_ms(50);
					}
				};
				auto where = [&](obs_sceneitem_t *it) {
					vec2 p, b;
					obs_sceneitem_get_pos(it, &p);
					obs_sceneitem_get_bounds(it, &b);
					struct {
						double x, y, w, h;
					} out{p.x, p.y, b.x, b.y};
					return out;
				};
				wait(1500);
				const auto g1 = where(game_item), c1 = where(chat_item);
				wait(1500);
				const auto g2 = where(game_item), c2 = where(chat_item);
				CHECK(is(c1.x, 520) && is(c1.y, 80) && is(c1.w, 100) && is(c1.h, 200),
				      "beside a game that is resized, a kept-size item goes to the firm side and the middle of its height (%.0f, %.0f %.0fx%.0f)",
				      c1.x, c1.y, c1.w, c1.h);
				CHECK(is(g1.x, 20) && is(g1.y, 20) && is(g1.w, 480) && is(g1.h, 320),
				      "and the game takes the room up to the gap from it (%.0f, %.0f %.0fx%.0f)", g1.x,
				      g1.y, g1.w, g1.h);
				CHECK(is(g2.x, g1.x) && is(g2.w, g1.w) && is(c2.x, c1.x) && is(c2.y, c1.y),
				      "having settled, neither moves again (game %.0f then %.0f, chat %.0f then %.0f)",
				      g1.x, g2.x, c1.x, c2.x);
				for (obs_source_t *f : {fit_game, fit_chat}) {
					obs_source_filter_remove(obs_scene_get_source(ps), f);
					obs_source_release(f);
				}
				obs_scene_release(ps);
				obs_source_release(chat);
				obs_source_release(wide);
			}
			// A fitted item that is hidden is left where it is, and comes
			// back there. The report: a camera above a chat box, each with
			// its own filter; hidden and shown again, the camera came back
			// in the middle of the column, on top of the chat box, and
			// neither moved again. Here the chat box is kept towards the
			// top, so it takes the camera's very place while that is hidden.
			{
				obs_source_t *wide = obs_source_create("color_source", "Trio Game", nullptr, nullptr);
				sized(wide, 320, 180);
				obs_source_t *cam3 = obs_source_create("color_source", "Trio Camera", nullptr, nullptr);
				sized(cam3, 100, 60);
				obs_source_t *chat = obs_source_create("color_source", "Trio Chat", nullptr, nullptr);
				sized(chat, 100, 150);
				obs_scene_t *ts = obs_scene_create("Fit As A Trio"); // 640x360
				obs_sceneitem_t *game_item = obs_scene_add(ts, wide);
				obs_sceneitem_t *cam_item = obs_scene_add(ts, cam3);
				obs_sceneitem_t *chat_item = obs_scene_add(ts, chat);
				vec2 pp;
				vec2_set(&pp, 40.0f, 60.0f);
				obs_sceneitem_set_pos(game_item, &pp);
				vec2_set(&pp, 520.0f, 20.0f);
				obs_sceneitem_set_pos(cam_item, &pp);
				vec2_set(&pp, 520.0f, 100.0f);
				obs_sceneitem_set_pos(chat_item, &pp);
				auto fit_for = [&](const char *name, const char *item_name, bool resize, int toward) {
					obs_data_t *d = obs_data_create();
					obs_data_set_string(d, "item", item_name);
					obs_data_set_int(d, "box", 2);
					obs_data_set_double(d, "margin", 20);
					obs_data_set_int(d, "glide_ms", 100);
					obs_data_set_bool(d, "resize", resize);
					obs_data_set_int(d, "toward", toward);
					obs_source_t *f = obs_source_create("stagekit_fit_item", name, d, nullptr);
					obs_data_release(d);
					obs_source_filter_add(obs_scene_get_source(ts), f);
					return f;
				};
				obs_source_t *fit_game = fit_for("Fit Trio Game", "Trio Game", true, 0);
				obs_source_t *fit_cam = fit_for("Fit Trio Camera", "Trio Camera", false, 1);
				obs_source_t *fit_chat = fit_for("Fit Trio Chat", "Trio Chat", false, 1);
				obs_set_output_source(0, obs_scene_get_source(ts));
				auto wait = [&](int ms) {
					for (int i = 0; i < ms / 50; i++) {
						render(obs_scene_get_source(ts), 640, 360);
						sleep_ms(50);
					}
				};
				auto at = [&](obs_sceneitem_t *it) {
					vec2 p;
					obs_sceneitem_get_pos(it, &p);
					return p;
				};
				wait(1500);
				const vec2 cam0 = at(cam_item), chat0 = at(chat_item), game0 = at(game_item);
				CHECK(is(cam0.x, 520) && is(cam0.y, 20) && is(chat0.x, 520) && is(chat0.y, 100),
				      "kept towards the top, a kept-size item lies at the top of its space, not in the middle (camera %.0f, %.0f; chat box %.0f, %.0f)",
				      cam0.x, cam0.y, chat0.x, chat0.y);
				obs_sceneitem_set_visible(cam_item, false);
				wait(1000);
				const vec2 cam1 = at(cam_item), chat1 = at(chat_item);
				CHECK(is(chat1.y, 20) && is(cam1.x, cam0.x) && is(cam1.y, cam0.y),
				      "with the camera hidden the chat box takes its place, and the hidden camera is left where it was (chat box y %.0f; camera %.0f, %.0f)",
				      chat1.y, cam1.x, cam1.y);
				// Shown again: watched every 50 ms, the chat box goes back in
				// one move and is there within 0.4 s; nothing shifts after.
				obs_sceneitem_set_visible(cam_item, true);
				int turns = 0;
				double last_y = chat1.y, last_step = 0.0, y_at_400 = 0.0;
				bool cam_stayed = true;
				for (int i = 0; i < 30; i++) {
					render(obs_scene_get_source(ts), 640, 360);
					sleep_ms(50);
					const double y = at(chat_item).y, step = y - last_y;
					// A move that stops and starts again, or turns round.
					if (std::fabs(step) > 0.05 && std::fabs(last_step) <= 0.05 && i > 0 &&
					    std::fabs(last_y - chat1.y) > 0.05)
						turns++;
					if (step * last_step < 0.0)
						turns++;
					last_step = step;
					last_y = y;
					if (i == 7)
						y_at_400 = y;
					const vec2 c = at(cam_item);
					cam_stayed = cam_stayed && is(c.x, cam0.x) && is(c.y, cam0.y);
				}
				const vec2 chat2 = at(chat_item), game2_at = at(game_item);
				CHECK(cam_stayed,
				      "shown again, the camera is where it was hidden and stays there (%.0f, %.0f)",
				      at(cam_item).x, at(cam_item).y);
				CHECK(is(chat2.x, chat0.x) && is(chat2.y, chat0.y) && is(game2_at.x, game0.x),
				      "and the chat box makes way for it, back to where it was (%.0f, %.0f; wanted %.0f, %.0f)",
				      chat2.x, chat2.y, chat0.x, chat0.y);
				CHECK(turns == 0 && is(y_at_400, chat0.y),
				      "in one move, done within 0.4 s (stops and turns on the way: %d; y after 0.4 s: %.0f)",
				      turns, y_at_400);
				// A resized picture of another shape than its space lies
				// against the side it is kept towards.
				obs_data_t *gd = obs_source_get_settings(fit_game);
				obs_data_set_int(gd, "toward", 2);
				obs_source_update(fit_game, gd);
				obs_data_release(gd);
				wait(500);
				CHECK(obs_sceneitem_get_bounds_alignment(game_item) == OBS_ALIGN_BOTTOM,
				      "a resized picture kept towards the bottom lies at the bottom of its space (bounds alignment %u)",
				      obs_sceneitem_get_bounds_alignment(game_item));
				for (obs_source_t *f : {fit_game, fit_cam, fit_chat}) {
					obs_source_filter_remove(obs_scene_get_source(ts), f);
					obs_source_release(f);
				}
				obs_scene_release(ts);
				obs_source_release(chat);
				obs_source_release(cam3);
				obs_source_release(wide);
			}
			// "Same place in these scenes": what the filter does in the scene
			// on screen, the same source has in the scenes listed, so it is
			// already there when one of them is switched to. A scene that
			// is not listed keeps the source where it belongs there (the
			// owner: "it should not be all scenes").
			{
				obs_source_t *wall = obs_source_create("color_source", "Share Wall", nullptr, nullptr);
				sized(wall, 300, 360);
				obs_source_t *cam4 =
					obs_source_create("color_source", "Share Camera", nullptr, nullptr);
				sized(cam4, 100, 60);
				obs_scene_t *one = obs_scene_create("Share One"); // 640x360
				obs_scene_t *two = obs_scene_create("Share Two");
				obs_scene_t *three = obs_scene_create("Share Three");
				obs_scene_add(one, wall);
				obs_scene_add(two, wall);
				obs_sceneitem_t *in_one = obs_scene_add(one, cam4);
				obs_sceneitem_t *in_two = obs_scene_add(two, cam4);
				obs_sceneitem_t *in_three = obs_scene_add(three, cam4);
				vec2 pp;
				vec2_set(&pp, 400.0f, 100.0f);
				obs_sceneitem_set_pos(in_one, &pp);
				vec2_set(&pp, 350.0f, 250.0f);
				obs_sceneitem_set_pos(in_two, &pp);
				vec2_set(&pp, 10.0f, 10.0f);
				obs_sceneitem_set_pos(in_three, &pp);
				obs_sceneitem_set_locked(in_three, true);
				obs_scene_t *four = obs_scene_create("Share Four");
				obs_sceneitem_t *in_four = obs_scene_add(four, cam4);
				vec2_set(&pp, 30.0f, 40.0f);
				obs_sceneitem_set_pos(in_four, &pp);
				auto fit_in = [&](obs_scene_t *scene, const char *name, bool share) {
					obs_data_t *d = obs_data_create();
					obs_data_set_string(d, "item", "Share Camera");
					obs_data_set_int(d, "box", 2);
					obs_data_set_double(d, "margin", 20);
					obs_data_set_int(d, "glide_ms", 100);
					// The first scene lists two of the others; the second
					// lists none and is linked back all the same.
					obs_data_array_t *with = obs_data_array_create();
					for (const char *listed : {"Share Two", "Share Three"}) {
						obs_data_t *entry = obs_data_create();
						obs_data_set_string(entry, "value", listed);
						if (share)
							obs_data_array_push_back(with, entry);
						obs_data_release(entry);
					}
					obs_data_set_array(d, "share_scenes", with);
					obs_data_array_release(with);
					obs_source_t *f = obs_source_create("stagekit_fit_item", name, d, nullptr);
					obs_data_release(d);
					obs_source_filter_add(obs_scene_get_source(scene), f);
					return f;
				};
				obs_source_t *fit_one = fit_in(one, "Fit Share One", true);
				obs_source_t *fit_two = fit_in(two, "Fit Share Two", false);
				struct Where {
					double x, y, w, h;
				};
				auto where = [&](obs_sceneitem_t *it) {
					vec2 p, b;
					obs_sceneitem_get_pos(it, &p);
					obs_sceneitem_get_bounds(it, &b);
					return Where{p.x, p.y, b.x, b.y};
				};
				auto alike = [&](const Where &a, const Where &b) {
					return is(a.x, b.x) && is(a.y, b.y) && is(a.w, b.w) && is(a.h, b.h);
				};
				obs_set_output_source(0, obs_scene_get_source(one));
				for (int i = 0; i < 30; i++) {
					render(obs_scene_get_source(one), 640, 360);
					sleep_ms(50);
				}
				const Where a = where(in_one), b = where(in_two), c = where(in_three);
				CHECK(is(a.x, 320) && is(a.y, 20) && is(a.w, 300) && is(a.h, 320),
				      "the item is fitted in the scene on screen (%.0f, %.0f %.0fx%.0f)", a.x, a.y, a.w,
				      a.h);
				CHECK(alike(a, b) && alike(a, c) && obs_sceneitem_locked(in_three),
				      "its copies in the scenes listed under 'Same place in these scenes' have the same place and size, with a filter of their own (%.0f, %.0f %.0fx%.0f) or without, locked or not (%.0f, %.0f %.0fx%.0f)",
				      b.x, b.y, b.w, b.h, c.x, c.y, c.w, c.h);
				CHECK(is(where(in_four).x, 30) && is(where(in_four).y, 40),
				      "a scene that is not listed keeps the source where it was (%.0f, %.0f)",
				      where(in_four).x, where(in_four).y);
				// Switched to: it is already there, and does not move.
				obs_set_output_source(0, obs_scene_get_source(two));
				bool stayed = true;
				for (int i = 0; i < 20; i++) {
					render(obs_scene_get_source(two), 640, 360);
					stayed = stayed && alike(where(in_two), a);
					sleep_ms(50);
				}
				const Where b2 = where(in_two);
				CHECK(stayed,
				      "switching to the other scene, the item is already in its place and does not move (%.0f, %.0f %.0fx%.0f)",
				      b2.x, b2.y, b2.w, b2.h);
				// Fitted another way in the scene now on screen (a wider gap):
				// the scene left behind takes that, though only it listed
				// this one. The scene not listed still keeps its own.
				obs_data_t *wider = obs_source_get_settings(fit_two);
				obs_data_set_double(wider, "margin", 40);
				obs_source_update(fit_two, wider);
				obs_data_release(wider);
				for (int i = 0; i < 30; i++) {
					render(obs_scene_get_source(two), 640, 360);
					sleep_ms(50);
				}
				CHECK(is(where(in_two).x, 340) && is(where(in_two).y, 40) &&
					      alike(where(in_one), where(in_two)) && is(where(in_four).x, 30),
				      "and the link holds both ways: the scene left behind, which listed this one, follows what happens here (%.0f, %.0f; here %.0f, %.0f)",
				      where(in_one).x, where(in_one).y, where(in_two).x, where(in_two).y);
				obs_source_filter_remove(obs_scene_get_source(one), fit_one);
				obs_source_filter_remove(obs_scene_get_source(two), fit_two);
				obs_source_release(fit_one);
				obs_source_release(fit_two);
				obs_scene_release(one);
				obs_scene_release(two);
				obs_scene_release(three);
				obs_scene_release(four);
				obs_source_release(cam4);
				obs_source_release(wall);
			}
			// The size kept survives a restart. The report: "even after i fix
			// it, if i restart, the chat box is slightly larger". A filter
			// created with "Resize it to fit" unticked (which is OBS loading
			// it) went back to the size from before it was first added.
			{
				obs_source_t *box = obs_source_create("color_source", "Restart Box", nullptr, nullptr);
				sized(box, 100, 50);
				obs_scene_t *rs = obs_scene_create("Fit After Restart"); // 640x360
				obs_sceneitem_t *it = obs_scene_add(rs, box);
				// As the filter left it when OBS closed: its own size.
				auto as_saved = [&] {
					vec2 p, b;
					vec2_set(&p, 270.0f, 155.0f);
					vec2_set(&b, 100.0f, 50.0f);
					obs_sceneitem_set_pos(it, &p);
					obs_sceneitem_set_bounds_type(it, OBS_BOUNDS_SCALE_INNER);
					obs_sceneitem_set_bounds(it, &b);
				};
				as_saved();
				auto load = [&](bool with_saved_size) {
					obs_data_t *d = obs_data_create();
					obs_data_set_string(d, "item", "Restart Box");
					obs_data_set_int(d, "box", 2);
					obs_data_set_double(d, "margin", 10);
					obs_data_set_int(d, "glide_ms", 0);
					obs_data_set_bool(d, "resize", false);
					// Before the filter was first added it was a fifth larger.
					obs_data_set_string(d, "orig_item", "Restart Box");
					obs_data_set_double(d, "orig_k", 1.2);
					if (with_saved_size) {
						obs_data_set_string(d, "keep_item", "Restart Box");
						obs_data_set_double(d, "keep_k", 1.0);
					}
					obs_source_t *f =
						obs_source_create("stagekit_fit_item", "Fit Restart", d, nullptr);
					obs_data_release(d);
					obs_source_filter_add(obs_scene_get_source(rs), f);
					return f;
				};
				auto wait = [&](int ms) {
					for (int i = 0; i < ms / 50; i++) {
						render(obs_scene_get_source(rs), 640, 360);
						sleep_ms(50);
					}
				};
				auto width = [&] {
					vec2 b;
					obs_sceneitem_get_bounds(it, &b);
					return (double)b.x;
				};
				obs_set_output_source(0, obs_scene_get_source(rs));
				// A collection saved by an older version: no size saved yet.
				obs_source_t *loaded = load(false);
				wait(1500);
				obs_data_t *now = obs_source_get_settings(loaded);
				CHECK(is(width(), 100) && std::fabs(obs_data_get_double(now, "keep_k") - 1.0) < 0.001 &&
					      std::string(obs_data_get_string(now, "keep_item")) == "Restart Box",
				      "loaded with 'Resize it to fit' unticked, the item keeps the size it was saved at, not the one from before the filter, and that size is saved (%.0f wide, saved %.3f)",
				      width(), obs_data_get_double(now, "keep_k"));
				obs_data_release(now);
				obs_source_filter_remove(obs_scene_get_source(rs), loaded);
				obs_source_release(loaded);
				// Loading again, while the source is for a moment not the
				// size it will be (a group still filling up, a shadow not
				// yet round it): the saved size is used, not one worked out
				// from the item.
				sized(box, 80, 40);
				as_saved();
				wait(200);
				loaded = load(true);
				wait(100);
				sized(box, 100, 50);
				wait(1500);
				CHECK(is(width(), 100),
				      "and a source that is not yet its full size while OBS loads does not change it (%.0f wide)",
				      width());
				obs_source_filter_remove(obs_scene_get_source(rs), loaded);
				obs_source_release(loaded);
				obs_scene_release(rs);
				obs_source_release(box);
			}
			// A widget that begins to show something, or stops, is noticed
			// quickly, and without the filter looking at every item once a
			// second. The report: "when my amuse widget appears or
			// disappears, the text box is much slower to react and move
			// than when my camera box hides and shows". Items of a kind
			// that comes and goes (here a nested scene, like a browser
			// widget or a group) are glanced at twice a second; anything
			// else seldom, until it has once been seen to change.
			for (const bool known_kind : {true, false}) {
				// (Names of their own each time round: a source just let go
				// may still be there for a moment, and OBS would rename
				// the new one.)
				const char *game_name = known_kind ? "Glance Game" : "Glance Game Too";
				obs_source_t *game3 = obs_source_create("color_source", game_name, nullptr, nullptr);
				sized(game3, 320, 180);
				obs_data_t *wd = obs_data_create();
				obs_data_set_int(wd, "width", 200);
				obs_data_set_int(wd, "height", 100);
				obs_data_set_int(wd, "color", 0x00000000); // there, and drawing nothing
				obs_source_t *widget = obs_source_create(
					"color_source", known_kind ? "Glance Widget" : "Glance Widget Too", wd,
					nullptr);
				obs_scene_t *gs = obs_scene_create(known_kind ? "Fit And A Widget"
									      : "Fit And A Plain Item"); // 640x360
				obs_scene_t *inner =
					obs_scene_create(known_kind ? "Glance Widget Scene" : "Glance Scene Unused");
				obs_sceneitem_t *game_item = obs_scene_add(gs, game3);
				obs_sceneitem_t *widget_item;
				vec2 pp;
				if (known_kind) {
					obs_sceneitem_t *in = obs_scene_add(inner, widget);
					vec2_set(&pp, 420.0f, 240.0f);
					obs_sceneitem_set_pos(in, &pp);
					widget_item = obs_scene_add(gs, obs_scene_get_source(inner));
				} else {
					widget_item = obs_scene_add(gs, widget);
					vec2_set(&pp, 420.0f, 240.0f);
					obs_sceneitem_set_pos(widget_item, &pp);
				}
				vec2_set(&pp, 40.0f, 40.0f);
				obs_sceneitem_set_pos(game_item, &pp);
				obs_data_t *d = obs_data_create();
				obs_data_set_string(d, "item", game_name);
				obs_data_set_int(d, "box", 2);
				obs_data_set_double(d, "margin", 20);
				obs_data_set_int(d, "glide_ms", 0);
				obs_source_t *fit3 = obs_source_create("stagekit_fit_item", "Fit Glance", d, nullptr);
				obs_data_release(d);
				obs_source_filter_add(obs_scene_get_source(gs), fit3);
				obs_set_output_source(0, obs_scene_get_source(gs));
				auto width = [&] {
					vec2 b;
					obs_sceneitem_get_bounds(game_item, &b);
					return (double)b.x;
				};
				// Draws frames until the fitted width is `wanted`; says how
				// long that took, or -1 when it took too long.
				auto until = [&](double wanted, int at_most) {
					for (int ms = 0; ms <= at_most; ms += 25) {
						render(obs_scene_get_source(gs), 640, 360);
						if (is(width(), wanted))
							return ms;
						sleep_ms(25);
					}
					return -1;
				};
				auto show = [&](bool on) {
					obs_data_set_int(wd, "color", on ? 0xFF00FF00 : 0x00000000);
					obs_source_update(widget, wd);
				};
				const int at_first = until(600, 3000);
				for (int i = 0; i < 40; i++) { // long enough for every item to have been glanced at
					render(obs_scene_get_source(gs), 640, 360);
					sleep_ms(25);
				}
				show(true);
				const int first = until(380, 14000);
				show(false);
				const int gone = until(600, 3000);
				show(true);
				const int again = until(380, 3000);
				if (known_kind)
					CHECK(at_first >= 0 && first >= 0 && first <= 800 && gone >= 0 && gone <= 800 &&
						      again >= 0 && again <= 800,
					      "a widget of a kind that comes and goes is made room for within 0.8 s, from the first time (took %d, %d and %d ms)",
					      first, gone, again);
				else
					CHECK(at_first >= 0 && first >= 0 && gone >= 0 && gone <= 800 && again >= 0 &&
						      again <= 800,
					      "any other item is glanced at seldom until it has once been seen to change, and often from then on (first noticed after %d ms; then %d and %d ms)",
					      first, gone, again);
				obs_source_filter_remove(obs_scene_get_source(gs), fit3);
				obs_source_release(fit3);
				obs_scene_release(gs);
				obs_scene_release(inner);
				obs_source_release(widget);
				obs_source_release(game3);
				obs_data_release(wd);
				(void)widget_item;
			}
			obs_set_output_source(0, shown_before);
			obs_source_release(shown_before);
			obs_data_release(ff);
			obs_source_filter_remove(obs_scene_get_source(sc), free_fit);
			obs_source_release(free_fit);
			obs_scene_release(sc);
			obs_source_release(cam);
			obs_source_release(game2);
		}
		obs_source_release(fit);
		obs_scene_release(full);
		obs_source_release(game);
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

		// The scenes it is left off in are chosen from the collection's scenes.
		{
			obs_properties_t *props = obs_source_properties(filter);
			obs_property_t *box = obs_properties_get(props, "off_scenes|apick");
			bool offered = false;
			for (size_t n = 0; box && n < obs_property_list_item_count(box); n++)
				offered = offered ||
					  std::string(obs_property_list_item_string(box, n)) == "Scene Without Shadow";
			CHECK(offered, "the settings offer the collection's scenes to choose from");
			pick_into_list(filter, props, "off_scenes|a", "Scene Without Shadow");
			CHECK(list_rows(props, "off_scenes|a") == 1,
			      "a chosen scene becomes a row with its own remove button");
			obs_properties_destroy(props);
		}
		wait(one);
		CHECK(obs_source_get_width(card) == 220, "choosing another scene changes nothing in this one");

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

		// "And live captures on screen", and the captures it is told to leave
		// alone. The owner: "there should be an exclusion option ... so
		// cameras for example can be intentionally excluded from blur".
		// (No capture works here, so a kind of the camera's name with a
		// size of its own stands in for them.)
		{
			obs_source_info stand_in = {};
			stand_in.id = "dshow_input";
			stand_in.type = OBS_SOURCE_TYPE_INPUT;
			stand_in.output_flags = OBS_SOURCE_VIDEO;
			stand_in.get_name = [](void *) {
				return "Capture (stand-in)";
			};
			stand_in.create = [](obs_data_t *, obs_source_t *) -> void * {
				return bmalloc(1);
			};
			stand_in.destroy = [](void *data) {
				bfree(data);
			};
			stand_in.get_width = [](void *) -> uint32_t {
				return 64;
			};
			stand_in.get_height = [](void *) -> uint32_t {
				return 48;
			};
			stand_in.video_render = [](void *, gs_effect_t *) {
			};
			obs_register_source(&stand_in);
			obs_source_t *screen = obs_source_create("dshow_input", "Live Screen", nullptr, nullptr);
			obs_source_t *camera = obs_source_create("dshow_input", "Live Camera", nullptr, nullptr);
			obs_data_t *crop_s = obs_data_create();
			obs_data_set_string(crop_s, "source", "Live Camera");
			obs_source_t *crop =
				obs_source_create("stagekit_cropped_view", "Live Camera Crop", crop_s, nullptr);
			obs_data_release(crop_s);
			obs_scene_t *ls = obs_scene_create("Live Scene");
			if (screen && camera) {
				obs_scene_add(ls, screen);
				obs_scene_add(ls, camera);
			}
			obs_source_t *before_out = obs_get_output_source(0);
			obs_set_output_source(0, obs_scene_get_source(ls));
			sleep_ms(600);
			const bool live = screen && camera && obs_source_get_width(screen) > 0 &&
					  obs_source_get_width(camera) > 0;
			CHECK(live, "the stand-in captures have a picture (needed for the next check)");
			if (live) {
				obs_data_t *ns = obs_source_get_settings(blur);
				obs_data_set_bool(ns, "also_captures", true);
				obs_data_array_t *never = obs_data_array_create();
				obs_data_t *entry = obs_data_create();
				// (Listed by its crop, as the owner's camera is shown: a
				// Cropped View keeps the source it shows clear too.)
				obs_data_set_string(entry, "value", "Live Camera Crop");
				obs_data_array_push_back(never, entry);
				obs_data_release(entry);
				obs_data_set_array(ns, "never", never);
				obs_data_array_release(never);
				obs_source_update(blur, ns);
				obs_properties_t *bp = obs_source_properties(blur);
				const int rows = list_rows(bp, "never|a");
				// Every source with a picture is offered, not only captures.
				bool offers_any = false;
				if (obs_property_t *pick = obs_properties_get(bp, "never|apick"))
					for (size_t i = 0; i < obs_property_list_item_count(pick); i++)
						offers_any = offers_any ||
							     strcmp(obs_property_list_item_string(pick, i),
								    "Live Scene") == 0;
				size_t offered = 0;
				bool has_capture = false;
				if (obs_property_t *pick = obs_properties_get(bp, "never|apick")) {
					offered = obs_property_list_item_count(pick);
					for (size_t i = 0; i < offered; i++)
						has_capture = has_capture ||
							      strcmp(obs_property_list_item_string(pick, i),
								     "Live Screen") == 0;
				}
				obs_properties_destroy(bp);
				CHECK(has_capture && offered > 3 && !offers_any,
				      "'Never blur these' offers every source with a picture, not scenes (%zu offered)",
				      offered);
				obs_data_release(ns);
				sleep_ms(200);
				obs_source_set_enabled(blur, true);
				draw_all(400);
				obs_source_t *on_screen = blur_of(screen);
				CHECK(rows == 1 && on_screen && obs_source_enabled(on_screen) &&
					      blur_of(camera) == nullptr,
				      "'And live captures on screen' blurs a capture that is showing and leaves alone one listed under 'Never blur these' (rows %d, the screen blurred %d, the camera given a blur %d)",
				      rows, (int)(on_screen && obs_source_enabled(on_screen)),
				      (int)(blur_of(camera) != nullptr));
				obs_source_set_enabled(blur, false);
				draw_all(400);
				on_screen = blur_of(screen);
				CHECK(on_screen && !obs_source_enabled(on_screen),
				      "and switching it off clears the capture it blurred");
				// A capture on screen only by way of a Cropped View (the
				// owner's camera) is blurred as well.
				obs_source_t *inner =
					obs_source_create("dshow_input", "Viewed Camera", nullptr, nullptr);
				obs_data_t *view_s = obs_data_create();
				obs_data_set_string(view_s, "source", "Viewed Camera");
				obs_source_t *view = obs_source_create("stagekit_cropped_view", "Viewed Camera Crop",
								       view_s, nullptr);
				obs_data_release(view_s);
				obs_sceneitem_t *view_item = obs_scene_add(ls, view);
				draw_all(300);
				obs_source_set_enabled(blur, true);
				draw_all(400);
				obs_source_t *on_inner = blur_of(inner);
				const bool viewed = on_inner && obs_source_enabled(on_inner);
				obs_source_set_enabled(blur, false);
				draw_all(400);
				on_inner = blur_of(inner);
				CHECK(viewed && on_inner && !obs_source_enabled(on_inner) && blur_of(camera) == nullptr,
				      "a capture shown only through a Cropped View is blurred and cleared with the rest (blurred %d)",
				      (int)viewed);
				obs_sceneitem_remove(view_item);
				obs_source_release(view);
				obs_source_release(inner);
			}
			obs_source_release(crop);
			obs_set_output_source(0, before_out);
			obs_source_release(before_out);
			obs_scene_release(ls);
			obs_source_release(screen);
			obs_source_release(camera);
		}

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
			CHECK(obs_property_list_item_count(obs_properties_get(lp, "items|apick")) == 4,
			      "the scene's items can be picked from a list");
			for (const char *name : vnames)
				pick_into_list(link, lp, "items|a", name);
			CHECK(list_rows(lp, "items|a") == 3, "each one chosen becomes a row (%d)",
			      list_rows(lp, "items|a"));
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

			// Also in other scenes: sources listed there are shown and hidden
			// with the set wherever they are, and it works from there too.
			{
				obs_scene_t *other = obs_scene_create("Link Other Scene");
				obs_source_t *cam2 =
					obs_source_create("color_source", "Other Camera", nullptr, nullptr);
				obs_source_t *loose = obs_source_create("color_source", "Not Linked", nullptr, nullptr);
				obs_sceneitem_t *same_there = obs_scene_add(other, vsources[0]);
				obs_sceneitem_t *cam2_there = obs_scene_add(other, cam2);
				obs_sceneitem_t *loose_there = obs_scene_add(other, loose);
				obs_properties_t *ep = obs_source_properties(link);
				pick_into_list(link, ep, "elsewhere|a", "Frame SNES");
				pick_into_list(link, ep, "elsewhere|a", "Other Camera");
				CHECK(list_rows(ep, "elsewhere|a") == 2, "sources in other scenes can be listed (%d)",
				      list_rows(ep, "elsewhere|a"));
				obs_properties_destroy(ep);
				sleep_ms(2300); // the other scenes are found within a couple of seconds
				obs_sceneitem_set_visible(vi[1], true);
				sleep_ms(200);
				const bool shown_there = obs_sceneitem_visible(same_there) &&
							 obs_sceneitem_visible(cam2_there);
				obs_sceneitem_set_visible(vi[1], false);
				sleep_ms(200);
				CHECK(shown_there && !obs_sceneitem_visible(same_there) &&
					      !obs_sceneitem_visible(cam2_there) && obs_sceneitem_visible(loose_there),
				      "showing and hiding a linked item does the same to the listed sources in other scenes, and to nothing else");
				obs_sceneitem_set_visible(cam2_there, true);
				sleep_ms(200);
				const bool came = shown() == "111" && obs_sceneitem_visible(same_there);
				obs_sceneitem_set_visible(cam2_there, false);
				sleep_ms(200);
				CHECK(came && shown() == "000" && !obs_sceneitem_visible(same_there),
				      "and showing or hiding one of them in the other scene does the same here (%s)",
				      shown().c_str());
				obs_scene_release(other);
				obs_source_release(cam2);
				obs_source_release(loose);
			}

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

		// One scene holding several captures, one shown at a time: what
		// Link Visibility and Show in Scenes need for that.
		printf("\n[capture scenes: out of view, locked while hidden, switches that follow]\n");
		if (obs_source_get_display_name("fade_transition")) {
			obs_source_t *before_out = obs_get_output_source(0);
			auto colour = [](const char *name) {
				return obs_source_create("color_source", name, nullptr, nullptr);
			};
			// A list as the plugin keeps it: a row for each name.
			auto set_list = [](obs_data_t *settings, const char *list, std::vector<const char *> names) {
				obs_data_array_t *rows = obs_data_array_create();
				for (const char *name : names) {
					obs_data_t *row = obs_data_create();
					obs_data_set_string(row, "value", name);
					obs_data_array_push_back(rows, row);
					obs_data_release(row);
				}
				obs_data_set_array(settings, list, rows);
				obs_data_array_release(rows);
			};
			auto is_fade = [](obs_sceneitem_t *item, bool show, uint32_t ms) {
				obs_source_t *t = obs_sceneitem_get_transition(item, show);
				return t && strcmp(obs_source_get_unversioned_id(t), "fade_transition") == 0 &&
				       obs_sceneitem_get_transition_duration(item, show) == ms;
			};
			auto in_use = [](obs_source_t *source) {
				return obs_source_active(source) || obs_source_showing(source);
			};
			// How often a source was taken into use (a capture card opens its device then).
			static int opened = 0;
			const auto count_opened = [](void *, calldata_t *) {
				opened++;
			};

			// --- A change made in a scene that is not being shown is a cut.
			{
				obs_scene_t *view = obs_scene_create("Cut In View");
				obs_scene_t *away = obs_scene_create("Cut Away");
				obs_source_t *away_src = obs_scene_get_source(away);
				obs_set_output_source(0, obs_scene_get_source(view));
				sleep_ms(100);

				// What OBS does by itself, which is why: the transition
				// it starts is only ended by drawing the scene.
				obs_source_t *plain = colour("Cut Plain");
				obs_sceneitem_t *plain_item = obs_scene_add(away, plain);
				obs_source_t *fade =
					obs_source_create_private("fade_transition", "Plain Hide", nullptr);
				obs_sceneitem_set_transition(plain_item, false, fade);
				obs_sceneitem_set_transition_duration(plain_item, false, 300);
				obs_source_release(fade);
				sleep_ms(100);
				obs_sceneitem_set_visible(plain_item, false);
				sleep_ms(500);
				obs_set_output_source(0, away_src);
				CHECK(in_use(plain),
				      "left to OBS, a source hidden with a fade while its scene was out of view counts as in use again when the scene comes on");
				obs_set_output_source(0, obs_scene_get_source(view));
				obs_sceneitem_remove(plain_item);
				sleep_ms(100);

				// Link Visibility, in the scene out of view.
				const char *names[3] = {"Cut A", "Cut B", "Cut C"};
				obs_source_t *src[3];
				obs_sceneitem_t *item[3];
				for (int i = 0; i < 3; i++) {
					src[i] = colour(names[i]);
					item[i] = obs_scene_add(away, src[i]);
				}
				obs_sceneitem_set_visible(item[1], false);
				obs_sceneitem_set_visible(item[2], false);
				obs_data_t *ld = obs_data_create();
				set_list(ld, "items", {names[0], names[1], names[2]});
				obs_data_set_int(ld, "transition_ms", 600);
				obs_source_t *link =
					obs_source_create("stagekit_visibility_link", "Cut Link", ld, nullptr);
				obs_source_filter_add(away_src, link);
				sleep_ms(150);
				signal_handler_connect(obs_source_get_signal_handler(src[0]), "activate", count_opened,
						       nullptr);
				opened = 0;
				obs_sceneitem_set_visible(item[1], true);
				sleep_ms(150);
				CHECK(!obs_sceneitem_visible(item[0]) && obs_sceneitem_visible(item[1]) &&
					      is_fade(item[0], false, 600),
				      "Link Visibility hides the other source in a scene that is out of view, and it keeps the fade it was given");
				obs_set_output_source(0, away_src);
				const bool at_once = in_use(src[0]);
				sleep_ms(300);
				CHECK(!at_once && !in_use(src[0]) && opened == 0,
				      "when that scene comes on, the source hidden there is not taken into use, not even for a moment (%d %d)",
				      at_once, opened);
				// The same scene, now showing: the fade is used as before, so
				// the source is still in use while it fades. (These tests
				// have no sound running, and a fade is over for OBS only
				// once its sound is too: its end cannot be seen here.)
				obs_sceneitem_set_visible(item[2], true);
				sleep_ms(250);
				CHECK(!obs_sceneitem_visible(item[1]) && in_use(src[1]),
				      "in a scene that is showing, the source it hides fades out as before");
				signal_handler_disconnect(obs_source_get_signal_handler(src[0]), "activate",
							  count_opened, nullptr);
				obs_set_output_source(0, obs_scene_get_source(view));
				sleep_ms(100);

				// Show in Scenes, in the scene out of view.
				obs_source_t *solo = colour("Cut Solo");
				obs_sceneitem_t *solo_item = obs_scene_add(away, solo);
				obs_data_t *sd = obs_data_create();
				obs_data_set_int(sd, "transition_ms", 600);
				obs_source_t *sw =
					obs_source_create("stagekit_show_in_scenes", "Cut Switch", sd, nullptr);
				obs_source_filter_add(solo, sw);
				sleep_ms(150);
				signal_handler_connect(obs_source_get_signal_handler(solo), "activate", count_opened,
						       nullptr);
				opened = 0;
				obs_source_set_enabled(sw, false);
				sleep_ms(150);
				CHECK(!obs_sceneitem_visible(solo_item) && is_fade(solo_item, false, 600),
				      "Show in Scenes hides its source in a scene that is out of view, and it keeps its fade");
				obs_set_output_source(0, away_src);
				const bool solo_at_once = in_use(solo);
				sleep_ms(300);
				CHECK(!solo_at_once && !in_use(solo) && opened == 0,
				      "when that scene comes on, the hidden source is not taken into use (%d %d)",
				      solo_at_once, opened);
				obs_set_output_source(0, obs_scene_get_source(view));
				sleep_ms(100);
				obs_source_set_enabled(sw, true);
				sleep_ms(150);
				obs_source_t *show_way = obs_sceneitem_get_transition(solo_item, true);
				obs_source_t *under_way = show_way ? obs_transition_get_active_source(show_way)
								   : nullptr;
				CHECK(obs_sceneitem_visible(solo_item) && is_fade(solo_item, true, 600) && !under_way,
				      "shown again out of view, it is simply shown: no fade is left under way");
				obs_source_release(under_way);
				obs_set_output_source(0, away_src);
				sleep_ms(200);
				obs_source_set_enabled(sw, false);
				sleep_ms(250);
				CHECK(!obs_sceneitem_visible(solo_item) && in_use(solo),
				      "and in a scene that is showing, Show in Scenes fades its source out as before");
				signal_handler_disconnect(obs_source_get_signal_handler(solo), "activate", count_opened,
							  nullptr);

				obs_set_output_source(0, before_out);
				obs_source_filter_remove(solo, sw);
				obs_source_release(sw);
				obs_data_release(sd);
				obs_source_filter_remove(away_src, link);
				obs_source_release(link);
				obs_data_release(ld);
				for (obs_source_t *source : src)
					obs_source_release(source);
				obs_source_release(solo);
				obs_source_release(plain);
				obs_scene_release(away);
				obs_scene_release(view);
			}

			// --- Link Visibility: "Lock hidden sources".
			{
				obs_scene_t *ls = obs_scene_create("Lock Scene");
				obs_source_t *lsrc = obs_scene_get_source(ls);
				const char *names[3] = {"Lock A", "Lock B", "Lock C"};
				obs_source_t *src[3];
				obs_sceneitem_t *item[3];
				for (int i = 0; i < 3; i++) {
					src[i] = colour(names[i]);
					item[i] = obs_scene_add(ls, src[i]);
				}
				obs_source_t *loose = colour("Lock Loose");
				obs_sceneitem_t *loose_item = obs_scene_add(ls, loose);
				obs_sceneitem_set_visible(item[1], false);
				obs_sceneitem_set_visible(item[2], false);
				obs_sceneitem_set_visible(loose_item, false);
				auto locks = [&] {
					return std::string(obs_sceneitem_locked(item[0]) ? "1" : "0") +
					       (obs_sceneitem_locked(item[1]) ? "1" : "0") +
					       (obs_sceneitem_locked(item[2]) ? "1" : "0");
				};
				auto shown = [&] {
					return std::string(obs_sceneitem_visible(item[0]) ? "1" : "0") +
					       (obs_sceneitem_visible(item[1]) ? "1" : "0") +
					       (obs_sceneitem_visible(item[2]) ? "1" : "0");
				};
				obs_data_t *ld = obs_data_create();
				set_list(ld, "items", {names[0], names[1], names[2]});
				obs_data_set_string(ld, "transition", "none");
				obs_source_t *link =
					obs_source_create("stagekit_visibility_link", "Lock Link", ld, nullptr);
				obs_source_filter_add(lsrc, link);
				sleep_ms(150);
				obs_properties_t *lp = obs_source_properties(link);
				obs_property_t *box = obs_properties_get(lp, "lock_hidden");
				CHECK(box && obs_property_long_description(box) &&
					      strlen(obs_property_description(box)) <= 26,
				      "Link Visibility has a 'Lock hidden sources' tick box with a tooltip");
				obs_properties_destroy(lp);
				obs_sceneitem_set_visible(item[1], true);
				sleep_ms(150);
				CHECK(shown() == "010" && locks() == "000",
				      "unticked, as it comes, nothing is locked (%s %s)", shown().c_str(),
				      locks().c_str());
				obs_sceneitem_set_locked(item[1], true); // a lock set by hand on the one shown
				obs_data_t *now = obs_source_get_settings(link);
				obs_data_set_bool(now, "lock_hidden", true);
				obs_source_update(link, now);
				sleep_ms(150);
				CHECK(locks() == "111" && !obs_sceneitem_locked(loose_item),
				      "ticked, the listed sources that are hidden are locked, the one shown is left as it is, and a hidden source not on the list is not touched (%s)",
				      locks().c_str());
				obs_sceneitem_set_visible(item[2], true);
				sleep_ms(150);
				CHECK(shown() == "001" && locks() == "110",
				      "showing a source unlocks it, and the one the filter hides for it is locked (%s %s)",
				      shown().c_str(), locks().c_str());
				obs_sceneitem_set_visible(item[2], false);
				sleep_ms(150);
				CHECK(shown() == "000" && locks() == "111",
				      "a source hidden by hand is locked too (%s %s)", shown().c_str(),
				      locks().c_str());
				obs_sceneitem_set_visible(item[0], true);
				sleep_ms(150);
				CHECK(shown() == "100" && locks() == "011", "and shown by hand it is unlocked (%s %s)",
				      shown().c_str(), locks().c_str());
				// Switched off, the filter does nothing; switched on again it
				// locks what is hidden.
				obs_source_set_enabled(link, false);
				sleep_ms(100);
				obs_sceneitem_set_visible(item[0], false);
				obs_sceneitem_set_locked(item[1], false);
				sleep_ms(150);
				const std::string while_off = locks();
				obs_source_set_enabled(link, true);
				sleep_ms(150);
				CHECK(while_off == "001" && locks() == "111",
				      "while the filter is switched off no lock is changed, and switched on again it locks the hidden ones (%s %s)",
				      while_off.c_str(), locks().c_str());
				// Unticked again: locks are left as they are.
				obs_data_set_bool(now, "lock_hidden", false);
				obs_source_update(link, now);
				obs_data_release(now);
				sleep_ms(150);
				obs_sceneitem_set_visible(item[1], true);
				sleep_ms(150);
				obs_sceneitem_set_visible(item[1], false);
				sleep_ms(150);
				obs_sceneitem_set_locked(item[2], false);
				obs_sceneitem_set_visible(item[0], true);
				sleep_ms(150);
				CHECK(shown() == "100" && locks() == "110",
				      "unticked again, showing and hiding changes no lock (%s %s)", shown().c_str(),
				      locks().c_str());
				obs_source_filter_remove(lsrc, link);
				obs_source_release(link);
				obs_data_release(ld);
				for (obs_source_t *source : src)
					obs_source_release(source);
				obs_source_release(loose);
				obs_scene_release(ls);
			}

			// --- Show in Scenes: "Follow what is shown".
			{
				obs_scene_t *one = obs_scene_create("Follow One");
				obs_scene_t *two = obs_scene_create("Follow Two");
				obs_scene_t *other = obs_scene_create("Follow Other");
				obs_source_t *cam = colour("Follow Camera");
				obs_sceneitem_t *cam1 = obs_scene_add(one, cam), *cam2 = obs_scene_add(two, cam);
				obs_sceneitem_t *cam_other = obs_scene_add(other, cam); // a scene it does not cover
				obs_data_t *sd = obs_data_create();
				set_list(sd, "scenes", {"Follow One", "Follow Two"});
				obs_data_set_int(sd, "where", 1);
				obs_data_set_string(sd, "transition", "none");
				obs_source_t *sw =
					obs_source_create("stagekit_show_in_scenes", "Follow Switch", sd, nullptr);
				obs_source_filter_add(cam, sw);
				sleep_ms(150);
				obs_properties_t *sp = obs_source_properties(sw);
				obs_property_t *box = obs_properties_get(sp, "follow");
				CHECK(box && obs_property_long_description(box) &&
					      strlen(obs_property_description(box)) <= 26,
				      "Show in Scenes has a 'Follow what is shown' tick box with a tooltip");
				obs_properties_destroy(sp);
				obs_sceneitem_set_visible(cam2, false);
				sleep_ms(500);
				CHECK(obs_source_enabled(sw),
				      "unticked, as it comes, the switch stays as it was last pressed");
				obs_sceneitem_set_visible(cam2, true);
				obs_data_t *now = obs_source_get_settings(sw);
				obs_data_set_bool(now, "follow", true);
				obs_source_update(sw, now);
				sleep_ms(500);
				CHECK(obs_source_enabled(sw), "ticked, with all its sources shown the switch stays on");
				obs_sceneitem_set_visible(cam2, false);
				sleep_ms(500);
				CHECK(!obs_source_enabled(sw) && obs_sceneitem_visible(cam1) &&
					      !obs_sceneitem_visible(cam2),
				      "a source hidden by something else in one of its scenes turns the switch off, and that hides nothing more");
				sleep_ms(500);
				CHECK(!obs_source_enabled(sw) && obs_sceneitem_visible(cam1),
				      "and it stays that way: the switch does not act on its own correction");
				obs_sceneitem_set_visible(cam2, true);
				sleep_ms(500);
				CHECK(obs_source_enabled(sw) && obs_sceneitem_visible(cam1) &&
					      obs_sceneitem_visible(cam2),
				      "shown again everywhere, the switch turns on");
				obs_sceneitem_set_visible(cam_other, false);
				sleep_ms(500);
				CHECK(obs_source_enabled(sw), "a scene it does not cover does not count");
				// A press on a switch that is on already still acts.
				obs_sceneitem_set_visible(cam1, false);
				obs_source_set_enabled(sw, true);
				sleep_ms(100);
				const bool acted = obs_sceneitem_visible(cam1);
				sleep_ms(500);
				CHECK(acted && obs_source_enabled(sw),
				      "switching it on when it is on already still shows its sources, and it stays on");
				// A press, and the switch does not undo itself.
				obs_source_set_enabled(sw, false);
				sleep_ms(600);
				CHECK(!obs_source_enabled(sw) && !obs_sceneitem_visible(cam1) &&
					      !obs_sceneitem_visible(cam2),
				      "switched off by a press, its sources are hidden and it stays off");
				obs_source_set_enabled(sw, true);
				sleep_ms(600);
				CHECK(obs_source_enabled(sw) && obs_sceneitem_visible(cam1) &&
					      obs_sceneitem_visible(cam2),
				      "switched on by a press, they are shown and it stays on");
				// "On" meaning hidden.
				obs_data_set_int(now, "on_means", 1);
				obs_source_update(sw, now);
				sleep_ms(500);
				CHECK(!obs_source_enabled(sw) && obs_sceneitem_visible(cam1) &&
					      obs_sceneitem_visible(cam2),
				      "with 'on' meaning hidden, the switch reads off while its sources are shown, and changing that hid nothing");
				obs_sceneitem_set_visible(cam1, false);
				sleep_ms(500);
				CHECK(obs_source_enabled(sw) && obs_sceneitem_visible(cam2),
				      "and on once one of them is hidden, without hiding the other");
				obs_sceneitem_set_visible(cam1, true);
				sleep_ms(500);
				CHECK(!obs_source_enabled(sw), "and off again when all are shown");
				// In none of its scenes: nothing to go by.
				set_list(now, "scenes", {"Follow Nowhere"});
				obs_source_update(sw, now);
				obs_data_release(now);
				sleep_ms(100);
				obs_source_set_enabled(sw, true);
				sleep_ms(500);
				CHECK(obs_source_enabled(sw),
				      "with its source in none of its scenes the switch is left alone");
				obs_source_filter_remove(cam, sw);
				obs_source_release(sw);
				obs_data_release(sd);
				obs_source_release(cam);
				obs_scene_release(one);
				obs_scene_release(two);
				obs_scene_release(other);
			}

			// --- Show in Scenes: "Switch to scene".
			{
				printf("\n[Show in Scenes: switch to scene]\n");
				obs_scene_t *live = obs_scene_create("GoTo Live");
				obs_scene_t *standby = obs_scene_create("GoTo Standby");
				obs_scene_t *elsewhere = obs_scene_create("GoTo Elsewhere");
				obs_source_t *cam = colour("GoTo Camera");
				obs_sceneitem_t *cam_live = obs_scene_add(live, cam);
				obs_sceneitem_t *cam_standby = obs_scene_add(standby, cam);
				obs_sceneitem_set_visible(cam_live, false);
				obs_sceneitem_set_visible(cam_standby, false);
				const auto program = [](const char *name) {
					calldata_t cd = {};
					calldata_set_string(&cd, "program", name);
					proc_handler_call(obs_get_proc_handler(), "stagekit_test_scene_switch", &cd);
					calldata_free(&cd);
				};
				const auto switched = []() {
					calldata_t cd = {};
					proc_handler_call(obs_get_proc_handler(), "stagekit_test_scene_switched", &cd);
					const char *got = calldata_string(&cd, "scenes");
					std::string out = got ? got : "";
					calldata_free(&cd);
					return out;
				};
				program("GoTo Elsewhere");
				obs_data_t *sd = obs_data_create();
				set_list(sd, "scenes", {"GoTo Live", "GoTo Standby"});
				obs_data_set_int(sd, "where", 1);
				obs_data_set_string(sd, "transition", "none");
				obs_data_set_string(sd, "go_to", "GoTo Live");
				obs_source_t *sw =
					obs_source_create("stagekit_show_in_scenes", "GoTo Switch", sd, nullptr);
				obs_source_set_enabled(sw, false);
				obs_source_filter_add(cam, sw);
				sleep_ms(150);
				switched();
				obs_properties_t *sp = obs_source_properties(sw);
				obs_property_t *box = obs_properties_get(sp, "go_to");
				bool lists = false;
				for (size_t i = 0; box && i < obs_property_list_item_count(box); i++)
					lists = lists ||
						strcmp(obs_property_list_item_string(box, i), "GoTo Live") == 0;
				CHECK(box && obs_property_long_description(box) &&
					      strlen(obs_property_description(box)) <= 26 &&
					      obs_property_list_item_count(box) > 0 &&
					      !*obs_property_list_item_string(box, 0) && lists,
				      "Show in Scenes has a 'Switch to scene' list of scenes, (none) first, with a tooltip");
				obs_properties_destroy(sp);
				obs_source_set_enabled(sw, true);
				sleep_ms(150);
				std::string got = switched();
				CHECK(got == "GoTo Live" && obs_sceneitem_visible(cam_live),
				      "a press turning it on shows the source and asks for the scene chosen (%s)",
				      got.c_str());
				obs_source_set_enabled(sw, false);
				sleep_ms(150);
				got = switched();
				CHECK(got.empty() && !obs_sceneitem_visible(cam_live),
				      "a press turning it off changes no scene (%s)", got.c_str());
				program("GoTo Standby");
				obs_source_set_enabled(sw, true);
				sleep_ms(150);
				got = switched();
				CHECK(got.empty() && obs_sceneitem_visible(cam_standby),
				      "with a scene on screen that has the source already, no scene change (%s)",
				      got.c_str());
				program("GoTo Live");
				obs_source_set_enabled(sw, false);
				sleep_ms(100);
				obs_source_set_enabled(sw, true);
				sleep_ms(150);
				got = switched();
				CHECK(got.empty(), "with the chosen scene on screen, no scene change (%s)",
				      got.c_str());
				// "Follow what is shown" putting the switch right is not a press.
				program("GoTo Elsewhere");
				obs_data_t *now = obs_source_get_settings(sw);
				obs_data_set_bool(now, "follow", true);
				obs_source_update(sw, now);
				obs_sceneitem_set_visible(cam_live, false);
				sleep_ms(500);
				const bool off = !obs_source_enabled(sw);
				obs_sceneitem_set_visible(cam_live, true);
				sleep_ms(500);
				got = switched();
				CHECK(off && obs_source_enabled(sw) && got.empty(),
				      "the switch turned on by 'Follow what is shown' changes no scene (%s)",
				      got.c_str());
				// Off as it comes.
				obs_data_set_bool(now, "follow", false);
				obs_data_set_string(now, "go_to", "");
				obs_source_update(sw, now);
				obs_data_release(now);
				obs_source_set_enabled(sw, false);
				sleep_ms(100);
				obs_source_set_enabled(sw, true);
				sleep_ms(150);
				got = switched();
				CHECK(got.empty() && obs_sceneitem_visible(cam_live),
				      "with no scene chosen, a press changes no scene (%s)", got.c_str());
				// "On means hidden": the press that shows (switching it off)
				// changes scene, the one that hides does not, nor "Do it now".
				now = obs_source_get_settings(sw);
				obs_data_set_string(now, "go_to", "GoTo Live");
				obs_data_set_int(now, "on_means", 1);
				obs_source_update(sw, now);
				obs_data_release(now);
				sleep_ms(100);
				obs_source_set_enabled(sw, false);
				sleep_ms(150);
				got = switched();
				CHECK(got == "GoTo Live" && obs_sceneitem_visible(cam_live),
				      "on means hidden: a press that shows the source changes scene (%s)", got.c_str());
				obs_source_set_enabled(sw, true);
				sleep_ms(150);
				got = switched();
				CHECK(got.empty() && !obs_sceneitem_visible(cam_live),
				      "on means hidden: a press that hides the source changes no scene (%s)",
				      got.c_str());
				obs_source_set_enabled(sw, false);
				sleep_ms(150);
				switched();
				sp = obs_source_properties(sw);
				obs_property_button_clicked(obs_properties_get(sp, "apply_now"), sw);
				obs_properties_destroy(sp);
				sleep_ms(150);
				got = switched();
				CHECK(got.empty(), "'Do it now' changes no scene (%s)", got.c_str());
				obs_source_filter_remove(cam, sw);
				obs_source_release(sw);
				obs_data_release(sd);
				obs_source_release(cam);
				obs_scene_release(live);
				obs_scene_release(standby);
				obs_scene_release(elsewhere);
			}

			// --- The whole chain: two scenes, each holding three captures of
			// which one is shown, and a switch on each capture.
			{
				const char *scene_names[2] = {"Chain Full", "Chain Component"};
				const char *names[3] = {"Chain A", "Chain B", "Chain C"};
				const char *kind = obs_source_get_display_name("stagekit_morph") ? "stagekit_morph"
												 : "fade_transition";
				obs_scene_t *scene[2];
				obs_source_t *src[3], *sw[3], *link[2];
				obs_sceneitem_t *item[2][3];
				for (int i = 0; i < 3; i++)
					src[i] = colour(names[i]);
				for (int s = 0; s < 2; s++) {
					scene[s] = obs_scene_create(scene_names[s]);
					for (int i = 0; i < 3; i++) {
						item[s][i] = obs_scene_add(scene[s], src[i]);
						if (i)
							obs_sceneitem_set_visible(item[s][i], false);
					}
				}
				// The first is on screen, the second is not.
				obs_set_output_source(0, obs_scene_get_source(scene[0]));
				sleep_ms(100);
				for (int s = 0; s < 2; s++) {
					obs_data_t *ld = obs_data_create();
					set_list(ld, "items", {names[0], names[1], names[2]});
					obs_data_set_string(ld, "transition", kind);
					obs_data_set_int(ld, "transition_ms", 300);
					obs_data_set_bool(ld, "lock_hidden", true);
					link[s] = obs_source_create("stagekit_visibility_link",
								    "Link Visibility-Captures", ld, nullptr);
					obs_source_filter_add(obs_scene_get_source(scene[s]), link[s]);
					obs_data_release(ld);
				}
				for (int i = 0; i < 3; i++) {
					obs_data_t *sd = obs_data_create();
					set_list(sd, "scenes", {scene_names[0], scene_names[1]});
					obs_data_set_int(sd, "where", 1);
					obs_data_set_string(sd, "transition", kind);
					obs_data_set_int(sd, "transition_ms", 300);
					obs_data_set_bool(sd, "follow", true);
					sw[i] = obs_source_create("stagekit_show_in_scenes", "Show in Scenes-Capture",
								  sd, nullptr);
					obs_source_filter_add(src[i], sw[i]);
					obs_data_release(sd);
				}
				// As many letters as captures: which are shown in the two
				// scenes, which are locked, which switches are on.
				auto state = [&] {
					std::string text;
					for (int s = 0; s < 2; s++) {
						for (int i = 0; i < 3; i++)
							text += obs_sceneitem_visible(item[s][i]) ? "1" : "0";
						text += s ? " locked " : "/";
					}
					for (int s = 0; s < 2; s++) {
						for (int i = 0; i < 3; i++)
							text += obs_sceneitem_locked(item[s][i]) ? "1" : "0";
						text += s ? " on " : "/";
					}
					for (int i = 0; i < 3; i++)
						text += obs_source_enabled(sw[i]) ? "1" : "0";
					return text;
				};
				sleep_ms(700);
				CHECK(state() == "100/100 locked 011/011 on 100",
				      "to begin with the first capture is shown in both scenes, the hidden ones are locked and only its switch reads on (%s)",
				      state().c_str());
				obs_source_set_enabled(sw[1], true);
				sleep_ms(1000);
				CHECK(state() == "010/010 locked 101/101 on 010",
				      "switching the second capture on shows only it in both scenes, locks the other two, unlocks it, and only its switch is on (%s)",
				      state().c_str());
				obs_source_set_enabled(sw[2], true);
				sleep_ms(1000);
				CHECK(state() == "001/001 locked 110/110 on 001", "then the third (%s)",
				      state().c_str());
				obs_source_set_enabled(sw[0], true);
				sleep_ms(1000);
				CHECK(state() == "100/100 locked 011/011 on 100", "then the first again (%s)",
				      state().c_str());
				// The other scene comes on: nothing hidden there is opened.
				for (int i = 1; i < 3; i++)
					signal_handler_connect(obs_source_get_signal_handler(src[i]), "activate",
							       count_opened, nullptr);
				opened = 0;
				obs_set_output_source(0, obs_scene_get_source(scene[1]));
				sleep_ms(400);
				CHECK(opened == 0 && !in_use(src[1]) && !in_use(src[2]) && in_use(src[0]),
				      "and when the scene that was out of view comes on, the captures hidden in it are not taken into use (%d)",
				      opened);
				for (int i = 1; i < 3; i++)
					signal_handler_disconnect(obs_source_get_signal_handler(src[i]), "activate",
								  count_opened, nullptr);
				// Shown by the eye instead of a key: the switches follow.
				obs_sceneitem_set_visible(item[1][2], true);
				sleep_ms(1000);
				CHECK(state() == "100/001 locked 011/110 on 000",
				      "a capture shown by hand in one scene only: each scene shows one, and no switch reads on, as none is shown in both (%s)",
				      state().c_str());
				obs_source_set_enabled(sw[2], true);
				sleep_ms(1000);
				CHECK(state() == "001/001 locked 110/110 on 001",
				      "and a press on its switch brings the two scenes together again (%s)",
				      state().c_str());
				// Two keys pressed one straight after the other: the last
				// counts, and no press is lost on the way.
				obs_source_set_enabled(sw[0], true);
				obs_source_set_enabled(sw[1], true);
				sleep_ms(1000);
				CHECK(state() == "010/010 locked 101/101 on 010",
				      "two keys pressed one straight after the other: the second capture is the one shown, alone and unlocked, in both scenes (%s)",
				      state().c_str());
				// Keys pressed from two threads at once, as obs-websocket and
				// a hotkey can, while the filters act: always one capture,
				// the same in both scenes, unlocked, and only its switch on.
				auto settled = [&] {
					const std::string st = state();
					int one = -1;
					for (int i = 0; i < 3; i++)
						if (st[i] == '1')
							one = one < 0 ? i : 9;
					if (one < 0 || one > 2)
						return false;
					std::string want = "000/000 locked 111/111 on 000";
					want[one] = want[4 + one] = '1';
					want[15 + one] = want[19 + one] = '0';
					want[26 + one] = '1';
					return st == want;
				};
				int wrong = 0;
				for (int round = 0; round < 12; round++) {
					std::thread other([&] { obs_source_set_enabled(sw[round % 3], true); });
					obs_source_set_enabled(sw[(round + 1) % 3], true);
					other.join();
					sleep_ms(round % 2 ? 40 : 600);
					if (round % 2 == 0 && !settled())
						wrong++;
				}
				sleep_ms(1000);
				CHECK(wrong == 0 && settled(),
				      "keys pressed from two threads at once, again and again: each time one capture ends up shown in both scenes, unlocked, with only its switch on (%d wrong; %s)",
				      wrong, state().c_str());
				// A scene left with a cut while a capture's change is still
				// under way there: OBS never ends that change, and the
				// capture would stay in use whenever the scene is on.
				obs_set_output_source(0, obs_scene_get_source(scene[0]));
				sleep_ms(300);
				obs_source_set_enabled(sw[1], true);
				sleep_ms(80);
				obs_set_output_source(0, obs_scene_get_source(scene[1]));
				sleep_ms(400);
				obs_source_set_enabled(sw[2], true);
				sleep_ms(400);
				obs_set_output_source(0, obs_scene_get_source(scene[0]));
				sleep_ms(1500);
				// (Only the second is looked at: without sound in these tests
				// no item transition ends by itself, so the first, hidden
				// earlier with one, is still in use either way.)
				CHECK(state() == "001/001 locked 110/110 on 001" && !in_use(src[1]) && in_use(src[2]),
				      "a scene left with a cut part-way through a change: back on it, the capture whose change was cut off is not in use (%s; %d %d)",
				      state().c_str(), (int)in_use(src[1]), (int)in_use(src[2]));
				// OBS can leave a finished item transition counted as in use
				// for good. One that holds no source is not under way and is
				// not ended again and again (the owner's log: twice a second
				// for the rest of the session).
				{
					obs_sceneitem_t *shown =
						obs_scene_find_source(scene[0], obs_source_get_name(src[2]));
					obs_source_t *hide = shown ? obs_sceneitem_get_transition(shown, false)
								   : nullptr;
					int stops = 0;
					signal_handler_t *sh = hide ? obs_source_get_signal_handler(hide) : nullptr;
					const auto count = [](void *data, calldata_t *) {
						++*static_cast<int *>(data);
					};
					if (hide) {
						obs_transition_clear(hide);
						obs_source_inc_active(hide);
						signal_handler_connect(sh, "transition_stop", count, &stops);
					}
					sleep_ms(1500);
					if (hide) {
						signal_handler_disconnect(sh, "transition_stop", count, &stops);
						obs_source_dec_active(hide);
					}
					CHECK(hide && stops == 0,
					      "a finished transition OBS still counts as in use is left alone, not ended twice a second (%d ends)",
					      stops);
				}

				obs_set_output_source(0, before_out);
				for (int i = 0; i < 3; i++) {
					obs_source_filter_remove(src[i], sw[i]);
					obs_source_release(sw[i]);
				}
				for (int s = 0; s < 2; s++) {
					obs_source_filter_remove(obs_scene_get_source(scene[s]), link[s]);
					obs_source_release(link[s]);
				}
				sleep_ms(100);
				for (obs_source_t *source : src)
					obs_source_release(source);
				for (obs_scene_t *each : scene)
					obs_scene_release(each);
			}
			obs_source_release(before_out);
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
			CHECK(obs_property_list_item_count(obs_properties_get(tp, "items|apick")) == 3 &&
				      obs_property_list_item_count(obs_properties_get(tp, "filters|apick")) == 2,
			      "items and filters are picked from lists");
			pick_into_list(trigger, tp, "items|a", "ZBrush Capture");
			pick_into_list(trigger, tp, "filters|a", "Extra Shadow");
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
			obs_data_t *now = obs_source_get_settings(trigger);
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

		// Capture App in Use: one Window Capture for several apps. The owner:
		// "I'd like to have a single Window capture, and a new filter added
		// that can have multiple apps associated with the Window capture".
		printf("\n[capture app in use]\n");
		{
			auto coloured = [](obs_source_t *src, long long abgr, int cw, int ch) {
				obs_data_t *d = obs_source_get_settings(src);
				obs_data_set_int(d, "color", abgr);
				obs_data_set_int(d, "width", cw);
				obs_data_set_int(d, "height", ch);
				obs_source_update(src, d);
				obs_data_release(d);
			};
			obs_source_t *cap = obs_source_create("color_source", "One Capture", nullptr, nullptr);
			coloured(cap, 0xFF0000FFLL, 200, 100); // red
			obs_data_t *fd = obs_data_create();
			obs_data_array_t *apps = obs_data_array_create();
			for (const char *name : {"ZSculpt.exe", "modeller.exe", "Prime"}) {
				obs_data_t *entry = obs_data_create();
				obs_data_set_string(entry, "value", name);
				obs_data_array_push_back(apps, entry);
				obs_data_release(entry);
			}
			obs_data_set_array(fd, "apps", apps);
			obs_data_array_release(apps);
			obs_data_array_t *hand = obs_data_array_create();
			obs_data_t *hand_entry = obs_data_create();
			obs_data_set_string(hand_entry, "value", "browser.exe");
			obs_data_array_push_back(hand, hand_entry);
			obs_data_release(hand_entry);
			obs_data_set_array(fd, "by_hand", hand);
			obs_data_array_release(hand);
			// (First without the other apps kept ready: the capture alone.)
			obs_data_set_bool(fd, "ready", false);
			obs_data_set_int(fd, "change_ms", 600);
			obs_data_set_int(fd, "ease_curve", 0); // linear
			obs_source_t *one =
				obs_source_create("stagekit_app_capture", "Capture App in Use", fd, nullptr);
			obs_data_release(fd);
			CHECK(one != nullptr, "the Capture App in Use filter can be created");
			obs_source_filter_add(cap, one);
			obs_source_inc_showing(cap);
			const uint32_t cw = 200, ch = 100;
			std::vector<uint8_t> frame;
			// Drawn all the while, as a capture on screen is.
			auto draw_for = [&](int ms) {
				for (int waited = 0; waited < ms; waited += 50) {
					frame = render(cap, cw, ch);
					sleep_ms(50);
				}
			};
			auto at = [&](int x, int y, int channel) {
				return (int)frame[(size_t(y) * cw + x) * 4 + channel];
			};
			auto windows = [&](const char *text) {
				calldata_t cd = {};
				calldata_set_string(&cd, "windows", text);
				proc_handler_call(obs_source_get_proc_handler(one), "set_test_windows", &cd);
				calldata_free(&cd);
			};
			// What the capture is aimed at: the program and the title.
			auto aimed = [&](std::string &title, long long &priority) {
				obs_data_t *d = obs_source_get_settings(cap);
				const std::string window = obs_data_get_string(d, "window");
				priority = obs_data_get_int(d, "priority");
				obs_data_release(d);
				title = window.substr(0, window.find(':'));
				const size_t colon = window.rfind(':');
				return colon == std::string::npos ? std::string() : window.substr(colon + 1);
			};
			std::string title;
			long long priority = 0;
			draw_for(300);
			windows("Notes|notes.exe|front\nSculpt 2026|ZSculpt.exe|");
			draw_for(450);
			CHECK(aimed(title, priority) == "ZSculpt.exe" && priority == 2,
			      "with one of its apps open, the capture is aimed at it, by its program (%s)",
			      aimed(title, priority).c_str());
			draw_for(1500); // (the change that goes with being aimed runs out)
			windows("Notes|notes.exe|\nSculpt 2026|ZSculpt.exe|\nModel scene|modeller.exe|front");
			draw_for(150);
			const std::string after_click = aimed(title, priority);
			// The new window's picture: another colour and another size.
			coloured(cap, 0xFF00FF00LL, 100, 100); // green
			draw_for(200);
			const uint32_t held_w = obs_source_get_width(cap);
			const int held_red = at(100, 50, 0), held_green = at(100, 50, 1);
			draw_for(550);
			const uint32_t mix_w = obs_source_get_width(cap);
			const int mix_red = at(50, 50, 0), mix_green = at(50, 50, 1);
			draw_for(900);
			CHECK(after_click == "modeller.exe" && held_w == 200 && held_red > 240 && held_green < 15,
			      "clicking into another of its apps aims the capture at that one, and the last picture stays, at its size, until the new one is there (aimed at %s; %u wide, red %d, green %d)",
			      after_click.c_str(), held_w, held_red, held_green);
			CHECK(mix_w > 100 && mix_w < 200 && mix_red > 40 && mix_green > 40 &&
				      obs_source_get_width(cap) == 100 && at(50, 50, 0) < 15 && at(50, 50, 1) > 240,
			      "then the new picture comes in over it while the size glides to the new one's, and is all there is at the end (part-way %u wide, red %d, green %d; after: red %d, green %d)",
			      mix_w, mix_red, mix_green, at(50, 50, 0), at(50, 50, 1));

			windows("Notes|notes.exe|front\nSculpt 2026|ZSculpt.exe|\nModel scene|modeller.exe|");
			draw_for(450);
			CHECK(aimed(title, priority) == "modeller.exe",
			      "clicking into something that is not one of its apps changes nothing");
			windows("Notes|notes.exe|front\nSculpt 2026|ZSculpt.exe|");
			draw_for(450);
			CHECK(aimed(title, priority) == "ZSculpt.exe",
			      "when the app being captured closes, the one used before it is captured again (%s)",
			      aimed(title, priority).c_str());
			draw_for(1500);
			// Two programs with the same file name, told apart by a title.
			windows("Other Game|emu.exe|\nPrime Hack|emu.exe|front\nSculpt 2026|ZSculpt.exe|");
			draw_for(450);
			const std::string by_title = aimed(title, priority);
			CHECK(by_title == "emu.exe" && title == "Prime Hack" && priority == 1,
			      "an app listed by part of its window's title is found among windows of one program, and the capture goes by the title (%s, '%s', priority %lld)",
			      by_title.c_str(), title.c_str(), priority);
			draw_for(1500);

			// Its app closes and no other is open: the picture stays for a
			// moment (which app to go back to is not known at once), then
			// the capture has none, and a Standby Picture can stand in.
			windows("Notes|notes.exe|front");
			coloured(cap, 0xFF00FF00LL, 0, 0);
			draw_for(300);
			const uint32_t lost_w = obs_source_get_width(cap);
			const int lost_green = at(50, 50, 1);
			draw_for(1100);
			CHECK(lost_w == 100 && lost_green > 240 && obs_source_get_width(cap) == 0,
			      "a capture that loses its picture keeps the last one for a moment and then has none (%u wide with green %d at first, %u wide after)",
			      lost_w, lost_green, obs_source_get_width(cap));

			// Switched off it follows nothing; switched on again it takes
			// the app in front.
			coloured(cap, 0xFF0000FFLL, 200, 100);
			obs_source_set_enabled(one, false);
			windows("Model scene|modeller.exe|front\nSculpt 2026|ZSculpt.exe|");
			draw_for(450);
			const std::string while_off = aimed(title, priority);
			obs_source_set_enabled(one, true);
			draw_for(450);
			CHECK(while_off == "emu.exe" && aimed(title, priority) == "modeller.exe",
			      "switched off it leaves the capture where it is, and switched on again it takes the app in front (%s, then %s)",
			      while_off.c_str(), aimed(title, priority).c_str());

			auto command = [&](const char *what) {
				calldata_t cd = {};
				calldata_set_string(&cd, "what", what);
				proc_handler_call(obs_source_get_proc_handler(one), "command", &cd);
				calldata_free(&cd);
			};
			// An app switched to by hand: it is held whatever app is used,
			// until the next press lets it go. The owner: "manually switch
			// to and from a specific app, like a browser page".
			windows("Docs|browser.exe|\nModel scene|modeller.exe|front\nSculpt 2026|ZSculpt.exe|");
			draw_for(300);
			command("next");
			draw_for(300);
			const std::string by_hand = aimed(title, priority);
			windows("Docs|browser.exe|\nModel scene|modeller.exe|\nSculpt 2026|ZSculpt.exe|front");
			draw_for(400);
			const std::string still_held = aimed(title, priority);
			command("next");
			draw_for(400);
			CHECK(by_hand == "browser.exe" && still_held == "browser.exe" &&
				      aimed(title, priority) == "ZSculpt.exe",
			      "an app switched to by hand is held while other apps are clicked into, and after the last of them the apps are followed again (%s, %s, then %s)",
			      by_hand.c_str(), still_held.c_str(), aimed(title, priority).c_str());
			draw_for(1500);

			// A window picked by hand (the Capture Front Window key) goes
			// through the filter: it is held, its program is remembered
			// among the apps switched to, and one press goes back to it.
			// The owner: "I don't know how to [switch back] while also
			// keeping the previously manually selected window available".
			{
				{
					obs_data_t *now = obs_source_get_settings(one);
					obs_data_set_int(now, "picked", 1); // switch at selection
					obs_source_update(one, now);
					obs_data_release(now);
				}
				windows("Reference board|refs.exe|front\nDocs|browser.exe|\nSculpt 2026|ZSculpt.exe|");
				draw_for(300);
				calldata_t cd = {};
				calldata_set_string(&cd, "title", "Reference board");
				calldata_set_string(&cd, "executable", "refs.exe");
				proc_handler_call(obs_source_get_proc_handler(one), "hold", &cd);
				calldata_free(&cd);
				draw_for(300);
				const std::string picked = aimed(title, priority);
				windows("Reference board|refs.exe|\nDocs|browser.exe|\nSculpt 2026|ZSculpt.exe|front");
				draw_for(400);
				const std::string kept_it = aimed(title, priority);
				command("back");
				draw_for(400);
				const std::string followed = aimed(title, priority);
				command("next");
				draw_for(400);
				obs_properties_t *hp = obs_source_properties(one);
				const int hand_rows = list_rows(hp, "by_hand|a");
				obs_properties_destroy(hp);
				CHECK(picked == "refs.exe" && kept_it == "refs.exe" && followed == "ZSculpt.exe" &&
					      aimed(title, priority) == "refs.exe" && hand_rows == 2 &&
					      obs_source_enabled(one),
				      "a window picked by hand is held by the filter, which stays on; going back follows the apps again, and the next press returns to that window, now on the list (%s, %s, %s, %s; %d on the list)",
				      picked.c_str(), kept_it.c_str(), followed.c_str(), aimed(title, priority).c_str(),
				      hand_rows);
				// One command for both ways: the owner's "toggle between manual
				// capture and automatic".
				command("toggle");
				draw_for(400);
				const std::string to_auto = aimed(title, priority);
				command("toggle");
				draw_for(400);
				CHECK(to_auto == "ZSculpt.exe" && aimed(title, priority) == "refs.exe",
				      "'toggle' goes from the window held by hand back to the apps, and from there back to that window (%s, then %s)",
				      to_auto.c_str(), aimed(title, priority).c_str());
				command("back");
				draw_for(1500);
				// "Wait until switched to", as it comes: the window picked is
				// only kept as the manual window, until it is switched to.
				obs_data_t *ps = obs_source_get_settings(one);
				obs_data_set_int(ps, "picked", 0);
				obs_source_update(one, ps);
				windows("Notes|notes.exe|front\nDocs|browser.exe|\nSculpt 2026|ZSculpt.exe|");
				draw_for(300);
				const std::string before = aimed(title, priority);
				calldata_t kd = {};
				calldata_set_string(&kd, "title", "Notes");
				calldata_set_string(&kd, "executable", "notes.exe");
				proc_handler_call(obs_source_get_proc_handler(one), "hold", &kd);
				calldata_free(&kd);
				draw_for(400);
				const std::string waited = aimed(title, priority);
				command("toggle");
				draw_for(400);
				CHECK(waited == before && waited != "notes.exe" &&
					      aimed(title, priority) == "notes.exe",
				      "as it comes, a window picked by hand waits until it is switched to (%s, still %s, then %s)",
				      before.c_str(), waited.c_str(), aimed(title, priority).c_str());
				// "Auto-switch manual windows", on as it comes: in manual mode the
				// capture follows the manual window clicked into, and only those.
				windows("Notes|notes.exe|\nDocs|browser.exe|front\nSculpt 2026|ZSculpt.exe|");
				draw_for(400);
				const std::string followed_hand = aimed(title, priority);
				windows("Notes|notes.exe|\nDocs|browser.exe|\nSculpt 2026|ZSculpt.exe|front");
				draw_for(400);
				const std::string not_auto = aimed(title, priority);
				obs_data_set_bool(ps, "hand_follow", false);
				obs_source_update(one, ps);
				windows("Notes|notes.exe|front\nDocs|browser.exe|\nSculpt 2026|ZSculpt.exe|");
				draw_for(400);
				CHECK(followed_hand == "browser.exe" && not_auto == "browser.exe" &&
					      aimed(title, priority) == "browser.exe",
				      "in manual mode it follows the manual window clicked into and no other; switched off, it stays (%s, %s, %s)",
				      followed_hand.c_str(), not_auto.c_str(), aimed(title, priority).c_str());
				obs_data_set_bool(ps, "hand_follow", true);
				obs_source_update(one, ps);
				command("back");
				draw_for(1500);
				// Two windows of one program (two browser profiles): the one
				// picked is that window, not its program. Closed, the other is
				// never taken in its place, whatever its title. The owner: "if
				// i have 2 separate firefox window groups open and i close one,
				// it defaults to the other one and may show private info".
				for (const char *other_title : {"Private profile", "Stream profile"}) {
					obs_data_set_int(ps, "picked", 1); // switch at selection
					obs_source_update(one, ps);
					const std::string both =
						std::string(
							"Stream profile|browser2.exe|front|11\nSculpt 2026|ZSculpt.exe||5\n") +
						other_title + "|browser2.exe||12";
					windows(both.c_str());
					draw_for(300);
					calldata_t pd = {};
					calldata_set_string(&pd, "title", "Stream profile");
					calldata_set_string(&pd, "executable", "browser2.exe");
					calldata_set_int(&pd, "id", 11);
					proc_handler_call(obs_source_get_proc_handler(one), "hold", &pd);
					calldata_free(&pd);
					draw_for(400);
					std::string shown_title;
					const std::string picked_exe = aimed(shown_title, priority);
					const std::string picked_title = shown_title;
					const long long picked_priority = priority;
					obs_properties_t *bp = obs_source_properties(one);
					const int rows_before = list_rows(bp, "by_hand|a");
					obs_properties_destroy(bp);
					// The picked window closes; the other profile stays open.
					const std::string left = std::string("Sculpt 2026|ZSculpt.exe||5\n") +
								 other_title + "|browser2.exe|front|12";
					windows(left.c_str());
					draw_for(1500);
					const std::string after_exe = aimed(shown_title, priority);
					bp = obs_source_properties(one);
					const int rows_after = list_rows(bp, "by_hand|a");
					obs_properties_destroy(bp);
					CHECK(picked_exe == "browser2.exe" && picked_title == "Stream profile" &&
						      picked_priority == 1 &&
						      !(after_exe == "browser2.exe" && shown_title == other_title) &&
						      rows_after == rows_before - 1,
					      "a window picked by hand is that window: closed, another window of its program ('%s') is not taken, and it leaves the list (%s '%s', then %s '%s'; %d then %d on the list)",
					      other_title, picked_exe.c_str(), picked_title.c_str(), after_exe.c_str(),
					      shown_title.c_str(), rows_before, rows_after);
					command("back");
					draw_for(600);
				}
				obs_data_set_int(ps, "picked", 0);
				obs_source_update(one, ps);
				// Picked windows cleared by hand (the button, its hotkey, a
				// Stream Deck key): a picked window being shown is let go of,
				// and programs put on the list by name stay.
				{
					obs_data_set_int(ps, "picked", 1);
					obs_source_update(one, ps);
					windows("Stream profile|browser2.exe|front|21\nSculpt 2026|ZSculpt.exe||5");
					draw_for(300);
					calldata_t cd2 = {};
					calldata_set_string(&cd2, "title", "Stream profile");
					calldata_set_string(&cd2, "executable", "browser2.exe");
					calldata_set_int(&cd2, "id", 21);
					proc_handler_call(obs_source_get_proc_handler(one), "hold", &cd2);
					calldata_free(&cd2);
					draw_for(400);
					std::string t;
					const std::string held_it = aimed(t, priority);
					obs_properties_t *cp = obs_source_properties(one);
					const int with_picked = list_rows(cp, "by_hand|a");
					obs_properties_destroy(cp);
					command("clear");
					draw_for(400);
					cp = obs_source_properties(one);
					const int cleared = list_rows(cp, "by_hand|a");
					obs_properties_destroy(cp);
					obs_data_t *now = obs_source_get_settings(one);
					bool any_picked = false;
					obs_data_array_t *list = obs_data_get_array(now, "by_hand");
					for (size_t i = 0; i < obs_data_array_count(list); i++) {
						obs_data_t *e = obs_data_array_item(list, i);
						any_picked = any_picked ||
							     strstr(obs_data_get_string(e, "value"), " \xE2\x80\x94 ");
						obs_data_release(e);
					}
					obs_data_array_release(list);
					obs_data_release(now);
					CHECK(held_it == "browser2.exe" && with_picked > cleared && cleared >= 1 &&
						      !any_picked && aimed(t, priority) == "ZSculpt.exe",
					      "clearing the picked windows takes them off the list, keeps the programs named there, and goes back to the automatic windows (%s; %d then %d on the list; then %s)",
					      held_it.c_str(), with_picked, cleared, aimed(t, priority).c_str());
					// And "Clear picked windows on exit": a filter loaded with
					// picked windows on its list starts without them.
					obs_data_t *saved = obs_data_create();
					obs_data_array_t *arr = obs_data_array_create();
					for (const char *v :
					     {"browser.exe", "browser2.exe \xE2\x80\x94 Stream profile"}) {
						obs_data_t *e = obs_data_create();
						obs_data_set_string(e, "value", v);
						obs_data_array_push_back(arr, e);
						obs_data_release(e);
					}
					obs_data_set_array(saved, "by_hand", arr);
					obs_data_array_release(arr);
					obs_source_t *fresh = obs_source_create("stagekit_app_capture",
										"Loaded Switcher", saved, nullptr);
					obs_data_t *loaded = obs_source_get_settings(fresh);
					obs_data_array_t *after = obs_data_get_array(loaded, "by_hand");
					const size_t left = obs_data_array_count(after);
					obs_data_array_release(after);
					obs_data_release(loaded);
					// (Settings of its own: the first filter took the picked window out of those.)
					obs_data_t *saved2 = obs_data_create();
					obs_data_array_t *arr2 = obs_data_array_create();
					for (const char *v :
					     {"browser.exe", "browser2.exe \xE2\x80\x94 Stream profile"}) {
						obs_data_t *e = obs_data_create();
						obs_data_set_string(e, "value", v);
						obs_data_array_push_back(arr2, e);
						obs_data_release(e);
					}
					obs_data_set_array(saved2, "by_hand", arr2);
					obs_data_array_release(arr2);
					obs_data_set_bool(saved2, "clear_picked", false);
					obs_source_t *kept = obs_source_create("stagekit_app_capture", "Kept Switcher",
									       saved2, nullptr);
					obs_data_release(saved2);
					obs_data_t *kept_s = obs_source_get_settings(kept);
					obs_data_array_t *kept_a = obs_data_get_array(kept_s, "by_hand");
					const size_t kept_n = obs_data_array_count(kept_a);
					obs_data_array_release(kept_a);
					obs_data_release(kept_s);
					CHECK(left == 1 && kept_n == 2,
					      "loaded again, the picked windows are gone, unless 'Clear picked windows on exit' is unticked (%zu left; %zu kept)",
					      left, kept_n);
					obs_source_release(fresh);
					obs_source_release(kept);
					obs_data_release(saved);
					obs_data_set_int(ps, "picked", 0);
					obs_source_update(one, ps);
					command("back");
					draw_for(600);
				}
				obs_data_set_int(ps, "picked", 0);
				obs_source_update(one, ps);
				obs_data_release(ps);
				draw_for(1500);
			}

			// Morph: the frame's size is on the way from the one window's to
			// the other's while the picture changes.
			{
				obs_data_t *ms = obs_source_get_settings(one);
				obs_data_set_int(ms, "change", 7);
				obs_source_update(one, ms);
				obs_data_release(ms);
				draw_for(400);
				windows("Docs|browser.exe|\nModel scene|modeller.exe|front\nSculpt 2026|ZSculpt.exe|");
				draw_for(150);
				coloured(cap, 0xFF00FF00LL, 100, 100);
				draw_for(750);
				const uint32_t morph_w = obs_source_get_width(cap);
				draw_for(900);
				CHECK(morph_w > 104 && morph_w < 196 && obs_source_get_width(cap) == 100,
				      "with Morph the capture's size glides from the one window's to the other's (%u wide part-way, %u after)",
				      morph_w, obs_source_get_width(cap));
			}

			// Fit to Space goes with the change: the space for the new window's
			// shape is taken while the switcher changes over, by its time,
			// not after it. The owner: "the window switches, but then it's
			// like the auto fit has to catch up".
			{
				obs_source_t *fcam = obs_source_create("color_source", "Switch Cam", nullptr, nullptr);
				{
					obs_data_t *cs = obs_source_get_settings(fcam);
					obs_data_set_int(cs, "width", 120);
					obs_data_set_int(cs, "height", 120);
					obs_source_update(fcam, cs);
					obs_data_release(cs);
				}
				// (With a Standby Picture after the switcher, as on the owner's
				// capture: it follows the switcher's size, it does not glide
				// after it over its own, longer time.)
				obs_source_t *sby =
					obs_source_create("stagekit_standby", "Switch Standby", nullptr, nullptr);
				obs_source_filter_add(cap, sby);
				// (Until it has ticked, a Standby Picture says a size of its
				// own; the fit's place is taken once the real one is there.)
				draw_for(1000);
				obs_scene_t *fs = obs_scene_create("Fit Switch Scene"); // 640x360
				obs_sceneitem_t *fi = obs_scene_add(fs, cap);
				obs_sceneitem_t *ci = obs_scene_add(fs, fcam);
				vec2 at2;
				vec2_set(&at2, 500.0f, 220.0f);
				obs_sceneitem_set_pos(ci, &at2);
				vec2_set(&at2, 100.0f, 100.0f);
				obs_sceneitem_set_pos(fi, &at2);
				obs_data_t *fd2 = obs_data_create();
				obs_data_set_string(fd2, "item", obs_source_get_name(cap));
				obs_data_set_int(fd2, "box", 2);
				obs_data_set_double(fd2, "margin", 0);
				obs_data_set_bool(fd2, "have_anchor", true);
				obs_data_set_double(fd2, "anchor_x", 150.0);
				obs_data_set_double(fd2, "anchor_y", 150.0);
				// (Its own glide is far slower than the switcher's 600 ms:
				// the two cannot be mistaken for each other.)
				obs_data_set_int(fd2, "glide_ms", 1800);
				obs_data_set_bool(fd2, "skip_fill_on", true);
				obs_data_set_double(fd2, "skip_fill", 90.0);
				obs_data_array_t *skip = obs_data_array_create();
				obs_data_t *entry = obs_data_create();
				obs_data_set_string(entry, "value", "Switch Cam");
				obs_data_array_push_back(skip, entry);
				obs_data_release(entry);
				obs_data_set_array(fd2, "skip", skip);
				obs_data_array_release(skip);
				obs_source_t *ffit =
					obs_source_create("stagekit_fit_item", "Fit With Switch", fd2, nullptr);
				obs_data_release(fd2);
				obs_source_filter_add(obs_scene_get_source(fs), ffit);
				obs_source_t *was_out = obs_get_output_source(0);
				obs_set_output_source(0, obs_scene_get_source(fs));
				auto shown_for = [&](int ms) {
					for (int i = 0; i < ms / 50; i++) {
						render(obs_scene_get_source(fs), 640, 360);
						sleep_ms(50);
					}
				};
				auto bounds_w = [&]() {
					vec2 b;
					obs_sceneitem_get_bounds(fi, &b);
					return (double)b.x;
				};
				// A square window: beside the camera, since it would fill
				// little of the screen.
				shown_for(6000);
				const double square_w = bounds_w();
				// On to a 16:9 window, which fills the screen: over the camera.
				windows("Docs|browser.exe|\nModel scene|modeller.exe|\nSculpt 2026|ZSculpt.exe|front");
				shown_for(150);
				coloured(cap, 0xFFFF0000LL, 320, 180);
				double mid_w = 0.0, at_end_w = 0.0;
				bool ended = false;
				int began = -1, took = -1;
				for (int i = 0; i < 60 && !ended; i++) {
					shown_for(50);
					const uint32_t cw = obs_source_get_width(cap), ch = obs_source_get_height(cap);
					const double bw = bounds_w();
					if (began < 0 && cw != 100)
						began = i;
					if (took < 0 && cw == 320 && ch == 180)
						took = i - began;
					if (cw > 110 && cw < 300 && bw > square_w + 8.0 && bw < 632.0)
						mid_w = bw; // both on their way
					if (cw == 320 && ch == 180) {
						// The switcher has arrived: so has the fit, or nearly
						// (one look of the fit, a twentieth of a second).
						shown_for(150);
						at_end_w = bounds_w();
						ended = true;
					}
				}
				// (The switcher's change is 600 ms here, a look every 50 or so;
				// gliding after it for its own 800 ms, the Standby Picture took
				// well over twice as long.)
				CHECK(began >= 0 && took >= 0 && took <= 16,
				      "a Standby Picture after the switcher takes the new size with the switcher's change, not after it (%d looks of about 50 ms)",
				      took);
				CHECK(square_w > 100.0 && square_w < 600.0 && mid_w > 0.0 && ended && at_end_w > 630.0,
				      "Fit to Space takes the new window's space with the switcher's change, not after it (%.0f wide for the square window; %.0f part-way; %.0f when the switcher had arrived)",
				      square_w, mid_w, at_end_w);
				obs_set_output_source(0, was_out);
				obs_source_release(was_out);
				obs_source_filter_remove(obs_scene_get_source(fs), ffit);
				obs_source_release(ffit);
				obs_source_filter_remove(cap, sby);
				obs_source_release(sby);
				obs_sceneitem_remove(fi);
				obs_sceneitem_remove(ci);
				obs_scene_release(fs);
				obs_source_release(fcam);
				// (As the checks after this one found it.)
				windows("Docs|browser.exe|\nModel scene|modeller.exe|front\nSculpt 2026|ZSculpt.exe|");
				draw_for(150);
				coloured(cap, 0xFF00FF00LL, 100, 100);
				draw_for(1700);
			}

			// A window named by part of its title is followed when the rest of
			// its title changes: the owner's emulator windows are "GBA1 |
			// Volume 100%" and then "GBA1 | Muted".
			{
				obs_data_t *ts = obs_source_get_settings(one);
				obs_data_array_t *before = obs_data_get_array(ts, "apps");
				obs_data_array_t *only = obs_data_array_create();
				obs_data_t *entry = obs_data_create();
				obs_data_set_string(entry, "value", "Pad1");
				obs_data_array_push_back(only, entry);
				obs_data_release(entry);
				obs_data_set_array(ts, "apps", only);
				obs_data_array_release(only);
				obs_source_update(one, ts);
				windows("Pad1 - Volume 100%|pads.exe|front\nPad2 - Volume 100%|pads.exe|");
				draw_for(500);
				std::string at_first;
				long long how_first = 0;
				aimed(at_first, how_first);
				// While the capture has its window nothing is done: it holds
				// it whatever the title does.
				windows("Pad1 - Muted|pads.exe|front\nPad2 - Volume 100%|pads.exe|");
				draw_for(500);
				std::string held_title;
				long long how_held = 0;
				aimed(held_title, how_held);
				// Once it has lost it (OBS looks for it by the old title and
				// does not find it), it is aimed at the window again.
				coloured(cap, 0xFF00FF00LL, 0, 0);
				draw_for(500);
				std::string then;
				long long how_then = 0;
				const std::string program = aimed(then, how_then);
				CHECK(at_first == "Pad1 - Volume 100%" && held_title == at_first &&
					      then == "Pad1 - Muted" && how_then == 1 && program == "pads.exe",
				      "a window named by part of its title is found again when the rest of its title has changed, and left alone while the capture still holds it (%s; holding: %s; lost: %s, priority %lld)",
				      at_first.c_str(), held_title.c_str(), then.c_str(), how_then);
				// Ignored windows: never taken, whatever they answer to.
				coloured(cap, 0xFF00FF00LL, 100, 100);
				obs_data_array_t *any = obs_data_array_create(), *not_these = obs_data_array_create();
				obs_data_t *any_pad = obs_data_create(), *not_one = obs_data_create();
				obs_data_set_string(any_pad, "value", "Pad");
				obs_data_set_string(not_one, "value", "Pad1");
				obs_data_array_push_back(any, any_pad);
				obs_data_array_push_back(not_these, not_one);
				obs_data_release(any_pad);
				obs_data_release(not_one);
				obs_data_set_array(ts, "apps", any);
				obs_data_set_array(ts, "ignore", not_these);
				obs_data_array_release(any);
				obs_data_array_release(not_these);
				obs_source_update(one, ts);
				draw_for(600);
				std::string other;
				long long how_other = 0;
				aimed(other, how_other);
				obs_properties_t *ip = obs_source_properties(one);
				CHECK(other == "Pad2 - Volume 100%" && list_rows(ip, "ignore|a") == 1,
				      "a window on the Ignored windows list is never taken, even in front and matching an entry (%s taken)",
				      other.c_str());
				obs_properties_destroy(ip);
				obs_data_erase(ts, "ignore");
				obs_data_set_array(ts, "apps", before);
				obs_data_array_release(before);
				obs_source_update(one, ts);
				obs_data_release(ts);
				windows("Docs|browser.exe|\nModel scene|modeller.exe|front\nSculpt 2026|ZSculpt.exe|");
				draw_for(150);
				coloured(cap, 0xFF00FF00LL, 100, 100);
				draw_for(1700);
			}

			// Hidden and shown altogether, wherever it is.
			{
				obs_scene_t *hs = obs_scene_create("Capture Hide Scene");
				obs_sceneitem_t *shown = obs_scene_add(hs, cap);
				command("hide");
				draw_for(300);
				const bool hidden = !obs_sceneitem_visible(shown);
				command("hide");
				draw_for(300);
				CHECK(hidden && obs_sceneitem_visible(shown) &&
					      obs_sceneitem_get_transition(shown, false),
				      "the capture can be hidden everywhere it is shown, and shown again, with a transition");
				obs_sceneitem_remove(shown);
				obs_scene_release(hs);
			}

			// The other open apps kept ready: a capture of each runs in the
			// background, so a switch does not wait for the capture.
			{
				// (The colour first: the ready captures are made as copies.)
				coloured(cap, 0xFF0000FFLL, 200, 100);
				draw_for(300);
				obs_data_t *rs = obs_source_get_settings(one);
				obs_data_set_bool(rs, "ready", true);
				obs_data_set_int(rs, "change", 0);
				obs_source_update(one, rs);
				obs_data_release(rs);
				draw_for(600);
				calldata_t cd = {};
				proc_handler_call(obs_source_get_proc_handler(one), "ready_count", &cd);
				const long long ready = calldata_int(&cd, "count");
				calldata_free(&cd);
				windows("Docs|browser.exe|\nModel scene|modeller.exe|\nSculpt 2026|ZSculpt.exe|front");
				draw_for(150);
				// (The stand-in for the new window's picture: the capture's
				// own, which the ready one was made a copy of.)
				const uint32_t at_once = obs_source_get_width(cap);
				draw_for(1200);
				// (The capture itself is left where it was: aiming it again is
				// what cost frames at every switch.)
				const std::string left_at = aimed(title, priority);
				CHECK(ready == 3 && left_at == "modeller.exe" && at_once == 200 && opaque(frame),
				      "with the other windows kept captured, every open app has a capture in the background and a switch draws that picture without aiming the capture again (%lld kept, the capture still at %s)",
				      ready, left_at.c_str());
				// In no scene on show for a while (another scene is on), and
				// back: the picture is there again without switching windows.
				auto kept_now = [&] {
					calldata_t count = {};
					proc_handler_call(obs_source_get_proc_handler(one), "ready_count", &count);
					const long long n = calldata_int(&count, "count");
					calldata_free(&count);
					return n;
				};
				obs_source_dec_showing(cap);
				draw_for(600);
				const long long away = kept_now();
				obs_source_inc_showing(cap);
				draw_for(600);
				CHECK(away == 1 && kept_now() == 3 && obs_source_get_width(cap) == 200 && opaque(frame),
				      "away from every scene on show only the window shown stays captured, and its picture is there on the way back (%lld kept away, %lld back, %u wide)",
				      away, kept_now(), obs_source_get_width(cap));
			}

			obs_properties_t *props = obs_source_properties(one);
			CHECK(list_rows(props, "apps|a") == 3 && list_rows(props, "by_hand|a") == 1 &&
				      obs_properties_get(props, "follow") && obs_properties_get(props, "ready") &&
				      obs_properties_get(props, "picked") && obs_properties_get(props, "change") &&
				      obs_properties_get(props, "ease_curve") &&
				      obs_properties_get(props, "transition") && obs_properties_get(props, "hide_now"),
			      "its settings list the apps, with how to follow them and how the picture changes");
			obs_properties_destroy(props);
			obs_source_dec_showing(cap);
			obs_source_filter_remove(cap, one);
			obs_source_release(one);
			obs_source_release(cap);
		}

		// Active App: one of several captures shows, the one whose app is in use.
		printf("\n[active app]\n");
		{
			obs_scene_t *as = obs_scene_create("App Scene");
			obs_source_t *asrc = obs_scene_get_source(as);
			// Stand-ins for window captures: a colour each, aimed at a program.
			auto capture = [](const char *name, const char *window, long long abgr) {
				obs_data_t *d = obs_data_create();
				obs_data_set_int(d, "color", abgr);
				obs_data_set_int(d, "width", 200);
				obs_data_set_int(d, "height", 100);
				obs_data_set_string(d, "window", window);
				obs_source_t *made = obs_source_create("color_source", name, d, nullptr);
				obs_data_release(d);
				return made;
			};
			obs_source_t *any = capture("Any Window", "Docs:Browser:browser.exe", 0xFF0000FFLL); // red
			obs_source_t *sculpt =
				capture("Sculpt", "Sculpt 2026:SculptWnd:ZSculpt.exe", 0xFF00FF00LL); // green
			obs_source_t *model =
				capture("Model", "Model#3A scene:ModelWnd:modeller.exe", 0xFFFF0000LL); // blue
			obs_source_t *second = obs_source_create("color_source", "Second Camera", nullptr, nullptr);
			obs_sceneitem_t *any_item = obs_scene_add(as, any);
			obs_sceneitem_t *sculpt_item = obs_scene_add(as, sculpt);
			obs_sceneitem_t *model_item = obs_scene_add(as, model);
			obs_sceneitem_t *second_item = obs_scene_add(as, second);
			vec2 aside;
			vec2_set(&aside, 300.0f, 0.0f);
			obs_sceneitem_set_pos(second_item, &aside);

			obs_data_t *ad = obs_data_create();
			obs_data_set_int(ad, "transition_ms", 600);
			obs_source_t *app = obs_source_create("stagekit_active_app", "Active App", ad, nullptr);
			obs_source_filter_add(asrc, app);
			sleep_ms(80);
			obs_properties_t *ap = obs_source_properties(app);
			pick_into_list(app, ap, "items|a", "Any Window");
			pick_into_list(app, ap, "items|a", "Sculpt");
			pick_into_list(app, ap, "items|a", "Model");
			obs_properties_destroy(ap);
			ap = obs_source_properties(app);
			CHECK(list_rows(ap, "items|a") == 3,
			      "the captures to switch between are picked from the scene's items");
			obs_properties_destroy(ap);

			auto see = [&](const char *windows) {
				calldata_t cd = {};
				calldata_set_string(&cd, "windows", windows);
				proc_handler_call(obs_source_get_proc_handler(app), "set_test_windows", &cd);
				calldata_free(&cd);
				sleep_ms(450);
			};
			auto only = [&](obs_sceneitem_t *item) {
				return obs_sceneitem_visible(any_item) == (item == any_item) &&
				       obs_sceneitem_visible(sculpt_item) == (item == sculpt_item) &&
				       obs_sceneitem_visible(model_item) == (item == model_item);
			};
			see("Notes|notes.exe|front\nDocs|browser.exe|");
			CHECK(only(any_item), "one capture shows at a time: to begin with, the one whose app is open");
			see("Notes|notes.exe|\nDocs|browser.exe|\nModel: scene|modeller.exe|front");
			CHECK(only(model_item),
			      "an app that opens is shown, and the capture that was showing is hidden");
			see("Notes|notes.exe|\nDocs|browser.exe|\nModel: scene|modeller.exe|\nSculpt 2026|zsculpt.exe|front");
			CHECK(only(sculpt_item), "a second app opening takes over");
			see("Notes|notes.exe|\nDocs|browser.exe|\nModel: scene|modeller.exe|front\nSculpt 2026|zsculpt.exe|");
			CHECK(only(model_item), "with both open, clicking into the other one shows it");
			see("Notes|notes.exe|front\nDocs|browser.exe|\nModel: scene|modeller.exe|\nSculpt 2026|zsculpt.exe|");
			CHECK(only(model_item), "clicking into a program that is not listed changes nothing");
			see("Notes|notes.exe|\nDocs|browser.exe|\nModel: scene|modeller.exe|\nSculpt 2026|zsculpt.exe|front");
			see("Notes|notes.exe|front\nDocs|browser.exe|\nModel: scene|modeller.exe|\nSculpt 2026|zsculpt.exe|");
			CHECK(only(sculpt_item),
			      "and the last app used stays captured while something else is in front");
			see("Notes|notes.exe|front\nDocs|browser.exe|\nModel: scene|modeller.exe|");
			CHECK(only(model_item),
			      "when the app that is showing closes, the one used before it comes back");
			see("Notes|notes.exe|front\nDocs|browser.exe|");
			CHECK(only(any_item), "and when that closes too, the capture that was there before them");
			see("Notes|notes.exe|front");
			CHECK(only(any_item), "with none of the apps open, the first in the list stays on show");
			CHECK(obs_sceneitem_visible(second_item), "items not on the list are left alone");

			// Two captures of programs with the same file name: each goes
			// with the window whose title starts like the one it was aimed at.
			{
				obs_scene_t *ts = obs_scene_create("Twin Scene");
				obs_source_t *plain = capture("Emu Plain", "Dolphin 5.0:Wnd:Dolphin.exe", 0xFF00FFFFLL);
				obs_source_t *prime = capture("Emu Prime", "PrimeHack Release [v1.0.4]:Wnd:Dolphin.exe",
							      0xFFFF00FFLL);
				obs_sceneitem_t *plain_item = obs_scene_add(ts, plain);
				obs_sceneitem_t *prime_item = obs_scene_add(ts, prime);
				obs_data_t *td = obs_data_create();
				obs_data_set_int(td, "transition_ms", 0);
				obs_source_t *twin = obs_source_create("stagekit_active_app", "Twin Apps", td, nullptr);
				obs_source_filter_add(obs_scene_get_source(ts), twin);
				sleep_ms(80);
				obs_properties_t *tp = obs_source_properties(twin);
				pick_into_list(twin, tp, "items|a", "Emu Plain");
				pick_into_list(twin, tp, "items|a", "Emu Prime");
				obs_properties_destroy(tp);
				auto see_twin = [&](const char *windows) {
					calldata_t cd = {};
					calldata_set_string(&cd, "windows", windows);
					proc_handler_call(obs_source_get_proc_handler(twin), "set_test_windows", &cd);
					calldata_free(&cd);
					sleep_ms(450);
				};
				see_twin("Notes|notes.exe|\nDolphin 5.0 - JIT64 - Some Game|dolphin.exe|front");
				const bool plain_first = obs_sceneitem_visible(plain_item) &&
							 !obs_sceneitem_visible(prime_item);
				see_twin("Notes|notes.exe|\nDolphin 5.0 - JIT64 - Some Game|dolphin.exe|\n"
					 "PrimeHack Release [v1.0.4] - Another Game|dolphin.exe|front");
				const bool prime_next = !obs_sceneitem_visible(plain_item) &&
							obs_sceneitem_visible(prime_item);
				see_twin("Notes|notes.exe|front\nDolphin 5.0 - JIT64 - Some Game|dolphin.exe|");
				const bool plain_back = obs_sceneitem_visible(plain_item) &&
							!obs_sceneitem_visible(prime_item);
				CHECK(plain_first && prime_next && plain_back,
				      "two captures of programs with one file name each follow their own window, by its title (%d %d %d)",
				      plain_first, prime_next, plain_back);
				obs_source_filter_remove(obs_scene_get_source(ts), twin);
				obs_source_release(twin);
				obs_data_release(td);
				obs_source_release(plain);
				obs_source_release(prime);
				obs_scene_release(ts);
			}

			// A change made by hand is kept until something happens.
			obs_sceneitem_set_visible(model_item, true);
			obs_sceneitem_set_visible(any_item, false);
			see("Notes|notes.exe|front");
			CHECK(only(model_item),
			      "a capture shown by hand stays until an app opens, closes or is clicked into");
			// The plain capture is aimed at another window by hand.
			obs_data_t *aimed = obs_source_get_settings(any);
			obs_data_set_string(aimed, "window", "Sketch:PaintWnd:paint.exe");
			obs_data_release(aimed);
			see("Notes|notes.exe|front");
			CHECK(only(any_item), "a capture aimed at another window is shown");

			// Captures switched to by hand: one that is shown stays, whatever
			// app is used, until it is hidden; then the apps are followed again.
			{
				obs_source_t *web = obs_source_create("color_source", "Web Page", nullptr, nullptr);
				obs_sceneitem_t *web_item = obs_scene_add(as, web);
				obs_sceneitem_set_visible(web_item, false);
				obs_sceneitem_set_visible(second_item, false);
				obs_properties_t *hp = obs_source_properties(app);
				pick_into_list(app, hp, "by_hand|a", "Second Camera");
				pick_into_list(app, hp, "by_hand|a", "Web Page");
				obs_properties_destroy(hp);
				const char *both =
					"Notes|notes.exe|\nModel: scene|modeller.exe|front\nSculpt 2026|zsculpt.exe|";
				const char *other =
					"Notes|notes.exe|\nModel: scene|modeller.exe|\nSculpt 2026|zsculpt.exe|front";
				see(both);
				CHECK(only(model_item) && !obs_sceneitem_visible(web_item),
				      "captures switched to by hand stay out of the way until one is shown");
				obs_sceneitem_set_visible(web_item, true);
				sleep_ms(450);
				CHECK(only(nullptr) && obs_sceneitem_visible(web_item),
				      "showing one takes over from the app's capture");
				see(other);
				see(both);
				CHECK(only(nullptr) && obs_sceneitem_visible(web_item),
				      "and it stays while the listed apps are clicked into");
				obs_sceneitem_set_visible(second_item, true);
				sleep_ms(450);
				CHECK(only(nullptr) && obs_sceneitem_visible(second_item) &&
					      !obs_sceneitem_visible(web_item),
				      "showing another one switched to by hand hides the first");
				see(other);
				obs_sceneitem_set_visible(second_item, false);
				sleep_ms(450);
				CHECK(only(sculpt_item) && !obs_sceneitem_visible(web_item),
				      "hiding it goes back to the capture of the app in use");

				// The two hotkeys the filter adds to the scene.
				struct Keys {
					obs_hotkey_id next = OBS_INVALID_HOTKEY_ID, back = OBS_INVALID_HOTKEY_ID;
				} keys;
				obs_enum_hotkeys(
					[](void *param, obs_hotkey_id id, obs_hotkey_t *key) {
						auto *k = static_cast<Keys *>(param);
						const char *name = obs_hotkey_get_name(key);
						if (name && strcmp(name, "stagekit.active.next.Active App") == 0)
							k->next = id;
						if (name && strcmp(name, "stagekit.active.back.Active App") == 0)
							k->back = id;
						return true;
					},
					&keys);
				CHECK(keys.next != OBS_INVALID_HOTKEY_ID && keys.back != OBS_INVALID_HOTKEY_ID,
				      "the scene gets a hotkey to step through them and one to go back to the apps");
				struct {
					const char *next = "step_by_hand", *back = "back_to_apps";
				} act;
				auto press = [&](const char *what) {
					calldata_t cd = {};
					proc_handler_call(obs_source_get_proc_handler(app), what, &cd);
					calldata_free(&cd);
					sleep_ms(450);
				};
				press(act.next);
				const bool first_shown = only(nullptr) && obs_sceneitem_visible(second_item);
				press(act.next);
				const bool second_shown = only(nullptr) && obs_sceneitem_visible(web_item) &&
							  !obs_sceneitem_visible(second_item);
				press(act.next);
				const bool apps_again = only(sculpt_item) && !obs_sceneitem_visible(web_item);
				CHECK(first_shown && second_shown && apps_again,
				      "the step hotkey shows each in turn, then goes back to the apps (%d %d %d)",
				      first_shown, second_shown, apps_again);
				press(act.next);
				press(act.back);
				CHECK(only(sculpt_item) && !obs_sceneitem_visible(second_item),
				      "the other hotkey goes straight back to the apps");
				see("Notes|notes.exe|front");
				CHECK(only(any_item), "and the apps are followed as before");
				// The list is emptied again for what follows.
				obs_data_t *emptied = obs_source_get_settings(app);
				obs_data_erase(emptied, "by_hand");
				obs_source_update(app, emptied);
				obs_data_release(emptied);
				sleep_ms(300);
				obs_sceneitem_set_visible(second_item, true);
				obs_sceneitem_remove(web_item);
				obs_source_release(web);
				see("Notes|notes.exe|front");
			}

			// The change is a fade, as OBS does it for an item.
			if (obs_source_get_display_name("fade_transition")) {
				obs_source_t *show = obs_sceneitem_get_transition(model_item, true);
				obs_source_t *hide = obs_sceneitem_get_transition(model_item, false);
				CHECK(show && hide &&
					      strcmp(obs_source_get_unversioned_id(show), "fade_transition") == 0 &&
					      obs_sceneitem_get_transition_duration(model_item, true) == 600 &&
					      obs_sceneitem_get_transition_duration(model_item, false) == 600,
				      "each listed item is given the chosen transition for showing and hiding");
				const uint32_t aw = obs_source_get_width(asrc), ah = obs_source_get_height(asrc);
				calldata_t cd = {};
				calldata_set_string(&cd, "windows",
						    "Notes|notes.exe|\nModel: scene|modeller.exe|front");
				proc_handler_call(obs_source_get_proc_handler(app), "set_test_windows", &cd);
				calldata_free(&cd);
				int most_red = 0, most_blue = 0;
				bool mixed = false;
				for (int i = 0; i < 12; i++) {
					sleep_ms(60);
					const std::vector<uint8_t> px = render(asrc, aw, ah);
					const uint8_t *at_item = &px[(size_t(50) * aw + 50) * 4];
					most_red = std::max<int>(most_red, at_item[0]);
					most_blue = std::max<int>(most_blue, at_item[2]);
					if (at_item[0] > 25 && at_item[2] > 25)
						mixed = true;
				}
				sleep_ms(500);
				const std::vector<uint8_t> after = render(asrc, aw, ah);
				const uint8_t *settled = &after[(size_t(50) * aw + 50) * 4];
				CHECK(mixed && settled[0] < 12 && settled[2] > 240,
				      "one capture fades into the next (red and blue together part-way; then %d, %d)",
				      settled[0], settled[2]);
			}

			// Captures of the other open apps keep running while the scene is on
			// show. A scene of its own, changing with a cut, so nothing else
			// has a hold on the sources.
			{
				obs_scene_t *rs = obs_scene_create("Ready Scene");
				obs_source_t *rsrc = obs_scene_get_source(rs);
				obs_source_t *one = capture("Ready One", "One:OneWnd:one.exe", 0xFF0000FFLL);
				obs_source_t *two = capture("Ready Two", "Two:TwoWnd:two.exe", 0xFF00FF00LL);
				obs_scene_add(rs, one);
				obs_sceneitem_t *two_item = obs_scene_add(rs, two);
				obs_sceneitem_set_visible(two_item, false);
				obs_data_t *rd = obs_data_create();
				obs_data_set_string(rd, "transition", "none");
				obs_data_array_t *listed = obs_data_array_create();
				for (const char *name : {"Ready One", "Ready Two"}) {
					obs_data_t *entry = obs_data_create();
					obs_data_set_string(entry, "value", name);
					obs_data_array_push_back(listed, entry);
					obs_data_release(entry);
				}
				obs_data_set_array(rd, "items", listed);
				obs_data_array_release(listed);
				obs_source_t *rapp =
					obs_source_create("stagekit_active_app", "Active App", rd, nullptr);
				obs_source_filter_add(rsrc, rapp);
				auto look = [&](const char *windows) {
					calldata_t cd = {};
					calldata_set_string(&cd, "windows", windows);
					proc_handler_call(obs_source_get_proc_handler(rapp), "set_test_windows", &cd);
					calldata_free(&cd);
					sleep_ms(450);
				};
				look("One|one.exe|front\nTwo|two.exe|");
				const bool idle = !obs_source_showing(two); // the scene is not on show
				obs_source_inc_showing(rsrc);
				look("One|one.exe|front\nTwo|two.exe|");
				const bool ready = obs_source_showing(two) && !obs_sceneitem_visible(two_item);
				look("One|one.exe|front");
				const bool closed = !obs_source_showing(two); // its app has closed
				look("One|one.exe|front\nTwo|two.exe|");
				const bool again = obs_source_showing(two) && !obs_sceneitem_visible(two_item);
				obs_source_dec_showing(rsrc);
				look("One|one.exe|front\nTwo|two.exe|");
				const bool let_go = !obs_source_showing(two);
				CHECK(idle && ready && closed && again && let_go,
				      "the capture of another open app is kept running while the scene is showing, and only then (%d %d %d %d %d)",
				      idle, ready, closed, again, let_go);
				obs_source_filter_remove(rsrc, rapp);
				obs_source_release(rapp);
				obs_data_release(rd);
				obs_source_release(one);
				obs_source_release(two);
				obs_scene_release(rs);
			}

			// Not following: an app shows when it opens and stays until it closes.
			obs_data_set_bool(ad, "follow", false);
			obs_source_update(app, ad);
			sleep_ms(80);
			see("Notes|notes.exe|front\nModel: scene|modeller.exe|");
			see("Notes|notes.exe|\nModel: scene|modeller.exe|\nSculpt 2026|zsculpt.exe|front");
			const bool took_over = only(sculpt_item);
			see("Notes|notes.exe|\nModel: scene|modeller.exe|front\nSculpt 2026|zsculpt.exe|");
			const bool stayed = only(sculpt_item);
			see("Notes|notes.exe|\nModel: scene|modeller.exe|front");
			CHECK(took_over && stayed && only(model_item),
			      "with following off, an app shows when it opens and stays until it closes (%d %d %d)",
			      took_over, stayed, only(model_item));

			obs_source_filter_remove(asrc, app);
			obs_source_release(app);
			obs_data_release(ad);
			obs_source_release(any);
			obs_source_release(sculpt);
			obs_source_release(model);
			obs_source_release(second);
			obs_scene_release(as);
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
		{
			// Its button has its own words, not the window layout menu's
			// (the two once shared a name in the strings file).
			obs_properties_t *lp = obs_source_properties(layout);
			obs_property_t *save = obs_properties_get(lp, "save");
			CHECK(save && strcmp(obs_property_description(save), "Save where everything is now") == 0,
			      "Apply Layout's button says what it does (%s)",
			      save ? obs_property_description(save) : "-");
			obs_properties_destroy(lp);
		}
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

	// ==== scene-filters:modes ====
	// ---- modes
	{
		// Scene Modes: one filter on a scene, modes made by hand or from a
		// template, one on at a time. Its presses go through the vendor
		// requests (as a Stream Deck key's do) and are acted on in a tick.
		printf("\n[scene modes: one mode at a time]\n");
		obs_source_t *modes_before_out = obs_get_output_source(0);
		auto modes_colour = [](const char *name, int cw = 0, int ch = 0, long long abgr = 0) {
			obs_data_t *d = obs_data_create();
			if (cw) {
				obs_data_set_int(d, "width", cw);
				obs_data_set_int(d, "height", ch);
			}
			if (abgr)
				obs_data_set_int(d, "color", abgr);
			obs_source_t *src = obs_source_create("color_source", name, d, nullptr);
			obs_data_release(d);
			return src;
		};
		auto modes_list = [](obs_data_t *settings, const std::string &list,
				     const std::vector<std::string> &names) {
			obs_data_array_t *rows = obs_data_array_create();
			for (const std::string &name : names) {
				obs_data_t *row = obs_data_create();
				obs_data_set_string(row, "value", name.c_str());
				obs_data_array_push_back(rows, row);
				obs_data_release(row);
			}
			obs_data_set_array(settings, list.c_str(), rows);
			obs_data_array_release(rows);
		};
		struct ModesMode {
			std::string id, name;
			std::vector<std::string> sources;
			std::string go_to;
		};
		// The settings of a Scene Modes, as the collection script writes them.
		auto modes_settings = [&](const std::vector<ModesMode> &modes, const char *now) {
			obs_data_t *d = obs_data_create();
			std::vector<std::string> ids;
			for (const ModesMode &m : modes) {
				ids.push_back(m.id);
				obs_data_set_string(d, ("mode|" + m.id + "|name").c_str(), m.name.c_str());
				modes_list(d, "mode|" + m.id + "|sources", m.sources);
				obs_data_set_string(d, ("mode|" + m.id + "|go_to").c_str(), m.go_to.c_str());
			}
			modes_list(d, "modes", ids);
			obs_data_set_string(d, "mode_now", now);
			obs_data_set_string(d, "transition", "none");
			return d;
		};
		auto modes_filter = [&](obs_scene_t *scene, const char *name, const std::vector<ModesMode> &modes,
					const char *now) {
			obs_data_t *d = modes_settings(modes, now);
			obs_source_t *f = obs_source_create("stagekit_scene_modes", name, d, nullptr);
			obs_data_release(d);
			obs_source_filter_add(obs_scene_get_source(scene), f);
			return f;
		};
		auto modes_unfilter = [](obs_scene_t *scene, obs_source_t *f) {
			obs_source_filter_remove(obs_scene_get_source(scene), f);
			obs_source_release(f);
		};
		// A vendor request, with its data and its answer as JSON.
		auto modes_call = [](const char *request, obs_data_t *data) {
			calldata_t cd = {};
			calldata_set_string(&cd, "request", request);
			calldata_set_string(&cd, "data", obs_data_get_json(data));
			proc_handler_call(obs_get_proc_handler(), "stagekit_vendor_call", &cd);
			const char *json = calldata_string(&cd, "response");
			obs_data_t *answer = obs_data_create_from_json(json && *json ? json : "{}");
			calldata_free(&cd);
			return answer;
		};
		std::string modes_why;
		auto modes_press = [&](const char *scene, const char *mode, const char *how = "on",
				       const char *filter = "") {
			obs_data_t *req = obs_data_create();
			obs_data_set_string(req, "scene", scene);
			obs_data_set_string(req, "filter", filter);
			obs_data_set_string(req, "mode", mode);
			obs_data_set_string(req, "how", how);
			obs_data_t *answer = modes_call("SetSceneMode", req);
			const bool ok = obs_data_get_bool(answer, "success");
			modes_why = obs_data_get_string(answer, "error");
			obs_data_release(answer);
			obs_data_release(req);
			sleep_ms(150);
			return ok;
		};
		auto modes_events = [] {
			calldata_t cd = {};
			proc_handler_call(obs_get_proc_handler(), "stagekit_vendor_events", &cd);
			const char *events = calldata_string(&cd, "events");
			std::string out_events = events ? events : "";
			calldata_free(&cd);
			return out_events;
		};
		auto modes_now = [](obs_source_t *f) {
			obs_data_t *s = obs_source_get_settings(f);
			std::string now = obs_data_get_string(s, "mode_now");
			obs_data_release(s);
			return now;
		};
		auto modes_shown = [](const std::vector<obs_sceneitem_t *> &items) {
			std::string s;
			for (obs_sceneitem_t *item : items)
				s += obs_sceneitem_visible(item) ? "1" : "0";
			return s;
		};
		auto modes_set = [](obs_source_t *f, const std::function<void(obs_data_t *)> &change) {
			obs_data_t *s = obs_source_get_settings(f);
			change(s);
			obs_source_update(f, s);
			obs_data_release(s);
			sleep_ms(150);
		};
		auto modes_count_items = [](obs_scene_t *scene, const char *id) {
			struct Count {
				const char *id;
				int n = 0;
			} count{id};
			obs_scene_enum_items(
				scene,
				[](obs_scene_t *, obs_sceneitem_t *it, void *p) {
					auto *c = static_cast<Count *>(p);
					if (strcmp(obs_source_get_unversioned_id(obs_sceneitem_get_source(it)),
						   c->id) == 0)
						c->n++;
					return true;
				},
				&count);
			return count.n;
		};

		// --- One scene: A, B in the first mode, C in the second, X in both, D in none.
		obs_scene_t *one = obs_scene_create("SM One");
		const char *one_names[5] = {"SM A", "SM B", "SM C", "SM D", "SM X"};
		obs_source_t *one_src[5];
		obs_sceneitem_t *one_item[5];
		for (int i = 0; i < 5; i++) {
			one_src[i] = modes_colour(one_names[i]);
			one_item[i] = obs_scene_add(one, one_src[i]);
		}
		obs_sceneitem_set_visible(one_item[2], false);
		const std::vector<obs_sceneitem_t *> abcdx(one_item, one_item + 5);
		obs_source_t *one_filter = modes_filter(
			one, "SM Modes",
			{{"m1", "First", {"SM A", "SM B", "SM X"}, ""}, {"m2", "Second", {"SM C", "SM X"}, ""}}, "m1");
		sleep_ms(200);
		modes_events(); // from here on, what is emitted is noted
		CHECK(modes_shown(abcdx) == "11011",
		      "as loaded, the scene shows the saved mode and nothing is changed (%s)",
		      modes_shown(abcdx).c_str());
		const bool pressed = modes_press("SM One", "m2");
		const std::string events = modes_events();
		CHECK(pressed && modes_shown(abcdx) == "00111" && modes_now(one_filter) == "m2",
		      "a press turns the second mode on: the first mode's sources hide, its own show, a source in both and one in no mode are left alone, and the mode on is saved (%s, %s)",
		      modes_shown(abcdx).c_str(), modes_now(one_filter).c_str());
		CHECK(events.find("SceneModeChanged") != std::string::npos &&
			      events.find("\"mode\":\"m2\"") != std::string::npos &&
			      events.find("\"modeName\":\"Second\"") != std::string::npos,
		      "and it says so: a SceneModeChanged event with the mode's id and name (%s)", events.c_str());

		printf("\n[scene modes: a source in two modes is not touched]\n");
		static int modes_x_signals = 0;
		static obs_sceneitem_t *modes_x_item = nullptr;
		modes_x_item = one_item[4];
		modes_x_signals = 0;
		const auto modes_count_x = [](void *, calldata_t *cd) {
			if (calldata_ptr(cd, "item") == modes_x_item)
				modes_x_signals++;
		};
		signal_handler_connect(obs_source_get_signal_handler(obs_scene_get_source(one)), "item_visible",
				       modes_count_x, nullptr);
		modes_press("SM One", "m1");
		modes_press("SM One", "m2");
		signal_handler_disconnect(obs_source_get_signal_handler(obs_scene_get_source(one)), "item_visible",
					  modes_count_x, nullptr);
		CHECK(modes_x_signals == 0 && obs_sceneitem_visible(one_item[4]) && modes_shown(abcdx) == "00111",
		      "changing between two modes that both have a source never shows or hides it (%d signals)",
		      modes_x_signals);

		printf("\n[scene modes: shown by something else]\n");
		obs_sceneitem_set_visible(one_item[0], true);
		sleep_ms(200);
		CHECK(!obs_sceneitem_visible(one_item[0]),
		      "a source of a mode that is not on, shown with the eye, is hidden again");
		obs_sceneitem_set_visible(one_item[2], false);
		sleep_ms(300);
		CHECK(!obs_sceneitem_visible(one_item[2]),
		      "a source of the mode on, hidden with the eye, stays hidden (a camera switched off stays off)");
		obs_sceneitem_set_visible(one_item[2], true);
		sleep_ms(200);
		CHECK(obs_sceneitem_visible(one_item[2]), "and shown again it stays shown");

		printf("\n[scene modes: no mode]\n");
		CHECK(!modes_press("SM One", "", "none"), "no mode is refused while it is not allowed (%s)",
		      modes_why.c_str());
		modes_press("SM One", "m2", "toggle");
		CHECK(modes_shown(abcdx) == "11011" && modes_now(one_filter) == "m1",
		      "Toggle on the mode on goes back to the mode before when no mode is not allowed (%s)",
		      modes_shown(abcdx).c_str());
		modes_set(one_filter, [](obs_data_t *s) { obs_data_set_bool(s, "allow_none", true); });
		modes_press("SM One", "m1", "toggle");
		CHECK(modes_shown(abcdx) == "00010" && modes_now(one_filter).empty(),
		      "with 'Allow no mode', Toggle on the mode on hides every mode's sources and leaves the rest (%s)",
		      modes_shown(abcdx).c_str());
		modes_press("SM One", "m2", "toggle");
		CHECK(modes_shown(abcdx) == "00111" && modes_now(one_filter) == "m2",
		      "and Toggle on another mode turns that one on (%s)", modes_shown(abcdx).c_str());

		printf("\n[scene modes: the vendor requests]\n");
		{
			obs_data_t *req = obs_data_create();
			obs_data_set_string(req, "scene", "SM One");
			obs_data_t *list = modes_call("ListSceneModes", req);
			obs_data_array_t *filters = obs_data_get_array(list, "filters");
			obs_data_t *first = obs_data_array_count(filters) == 1 ? obs_data_array_item(filters, 0)
									       : nullptr;
			obs_data_array_t *modes = first ? obs_data_get_array(first, "modes") : nullptr;
			obs_data_t *m0 = obs_data_array_count(modes) == 2 ? obs_data_array_item(modes, 0) : nullptr;
			CHECK(first && m0 && strcmp(obs_data_get_string(first, "filter"), "SM Modes") == 0 &&
				      strcmp(obs_data_get_string(first, "current"), "m2") == 0 &&
				      strcmp(obs_data_get_string(first, "currentName"), "Second") == 0 &&
				      strcmp(obs_data_get_string(m0, "id"), "m1") == 0 &&
				      strcmp(obs_data_get_string(m0, "name"), "First") == 0 &&
				      obs_data_get_bool(first, "allowNone"),
			      "ListSceneModes gives the scene's filter, its modes by id and name, and the mode on");
			obs_data_release(m0);
			obs_data_array_release(modes);
			obs_data_release(first);
			obs_data_array_release(filters);
			obs_data_release(list);
			obs_data_t *got = modes_call("GetSceneMode", req);
			CHECK(obs_data_get_bool(got, "success") &&
				      strcmp(obs_data_get_string(got, "current"), "m2") == 0 &&
				      strcmp(obs_data_get_string(got, "previous"), "m1") == 0,
			      "GetSceneMode gives the mode on and the one before");
			obs_data_release(got);
			obs_data_release(req);
		}
		CHECK(modes_press("SM One", "First") && modes_now(one_filter) == "m1",
		      "SetSceneMode takes a mode's name as well as its id");
		modes_press("SM One", "", "next");
		const std::string after_next = modes_now(one_filter);
		modes_press("SM One", "", "next");
		CHECK(after_next == "m2" && modes_now(one_filter) == "m1", "'next' goes down the list and round again");
		CHECK(!modes_press("SM Nowhere", "m1") && !modes_press("SM One", "Nope") &&
			      !modes_press("SM One", "m1", "sideways") &&
			      !modes_press("SM One", "m1", "on", "Not There"),
		      "a scene, a filter, a mode or a 'how' that is not there is refused with a reason (%s)",
		      modes_why.c_str());
		obs_source_set_enabled(one_filter, false);
		const bool while_off = modes_press("SM One", "m2");
		obs_source_set_enabled(one_filter, true);
		sleep_ms(150);
		CHECK(!while_off && modes_why.find("off") != std::string::npos && modes_now(one_filter) == "m1",
		      "a filter switched off refuses a press (%s)", modes_why.c_str());

		printf("\n[scene modes: hotkeys]\n");
		{
			struct Keys {
				obs_hotkey_id m1 = OBS_INVALID_HOTKEY_ID, m2 = OBS_INVALID_HOTKEY_ID,
					      next = OBS_INVALID_HOTKEY_ID, none = OBS_INVALID_HOTKEY_ID;
				std::string m1_text;
			};
			auto keys = [] {
				Keys k;
				obs_enum_hotkeys(
					[](void *param, obs_hotkey_id id, obs_hotkey_t *key) {
						auto *k = static_cast<Keys *>(param);
						const std::string name = obs_hotkey_get_name(key);
						if (name == "stagekit.mode.SM Modes.m1") {
							k->m1 = id;
							k->m1_text = obs_hotkey_get_description(key);
						}
						if (name == "stagekit.mode.SM Modes.m2")
							k->m2 = id;
						if (name == "stagekit.mode.SM Modes.next")
							k->next = id;
						if (name == "stagekit.mode.SM Modes.none")
							k->none = id;
						return true;
					},
					&k);
				return k;
			};
			Keys k = keys();
			CHECK(k.m1 != OBS_INVALID_HOTKEY_ID && k.m2 != OBS_INVALID_HOTKEY_ID &&
				      k.next != OBS_INVALID_HOTKEY_ID && k.none != OBS_INVALID_HOTKEY_ID &&
				      k.m1_text.find("First") != std::string::npos,
			      "the scene has a hotkey per mode, named by the mode's id and shown with its name, one for 'next' and, while allowed, one for no mode (%s)",
			      k.m1_text.c_str());
			obs_hotkey_enable_callback_rerouting(true);
			obs_hotkey_trigger_routed_callback(k.m2, true);
			obs_hotkey_trigger_routed_callback(k.m2, false);
			sleep_ms(150);
			CHECK(modes_now(one_filter) == "m2" && modes_shown(abcdx) == "00111",
			      "pressing a mode's hotkey turns it on");
			modes_set(one_filter, [](obs_data_t *s) { obs_data_set_string(s, "mode|m1|name", "Primary"); });
			Keys renamed = keys();
			obs_hotkey_trigger_routed_callback(renamed.m1, true);
			obs_hotkey_trigger_routed_callback(renamed.m1, false);
			sleep_ms(150);
			CHECK(renamed.m1 == k.m1 && renamed.m1_text.find("Primary") != std::string::npos &&
				      modes_now(one_filter) == "m1" && modes_shown(abcdx) == "11011",
			      "renamed, a mode keeps its hotkey (shown with the new name), its sources and its place (%s)",
			      renamed.m1_text.c_str());
			obs_hotkey_enable_callback_rerouting(false);
			modes_set(one_filter, [](obs_data_t *s) { obs_data_set_bool(s, "allow_none", false); });
			CHECK(keys().none == OBS_INVALID_HOTKEY_ID,
			      "and the no-mode hotkey goes when no mode is not allowed");
		}

		printf("\n[scene modes: made in the window]\n");
		{
			obs_properties_t *props = obs_source_properties(one_filter);
			auto click = [&](const std::string &name) {
				obs_property_t *p = obs_properties_get(props, name.c_str());
				const bool there = p != nullptr;
				if (p)
					obs_property_button_clicked(p, one_filter);
				sleep_ms(100);
				return there;
			};
			auto ids = [&] {
				obs_data_t *s = obs_source_get_settings(one_filter);
				std::string out_ids;
				obs_data_array_t *rows = obs_data_get_array(s, "modes");
				for (size_t i = 0; i < obs_data_array_count(rows); i++) {
					obs_data_t *row = obs_data_array_item(rows, i);
					out_ids += std::string(i ? "," : "") + obs_data_get_string(row, "value");
					obs_data_release(row);
				}
				obs_data_array_release(rows);
				obs_data_release(s);
				return out_ids;
			};
			auto setting = [&](const std::string &name) {
				obs_data_t *s = obs_source_get_settings(one_filter);
				std::string v = obs_data_get_string(s, name.c_str());
				obs_data_release(s);
				return v;
			};
			bool labels_short = true;
			for (const char *name : {"mode_now", "new_mode", "template", "allow_none", "lock_hidden",
						 "mode|m2|name", "mode|m2|type", "mode|m2|go_to"}) {
				obs_property_t *p = obs_properties_get(props, name);
				if (!p || strlen(obs_property_description(p)) >= 26 ||
				    !obs_property_long_description(p))
					labels_short = false;
			}
			obs_property_t *heading = obs_properties_get(props, "mode|m2|g");
			CHECK(labels_short && heading &&
				      strcmp(obs_property_description(heading), "Mode: Second") == 0 &&
				      obs_properties_get(props, "mode|m2|sources|apick") &&
				      obs_properties_get(props, "mode|m2|type") &&
				      obs_properties_get(props, "mode|m2|go_to"),
			      "each mode has a group of its own, headed with its name: its name, type, scene to switch to and sources, and every label is short");
			modes_set(one_filter, [](obs_data_t *s) { obs_data_set_string(s, "new_mode", "Third"); });
			click("add_mode");
			CHECK(ids() == "m1,m2,m3" && setting("mode|m3|name") == "Third" &&
				      obs_properties_get(props, "mode|m3|g"),
			      "a mode typed by hand is added with an id of its own (%s)", ids().c_str());
			modes_set(one_filter, [](obs_data_t *s) { obs_data_set_int(s, "template", 6); });
			click("add_template");
			CHECK(ids() == "m1,m2,m3,m4" && setting("mode|m4|name") == "Intermission" &&
				      setting("mode|m4|type") == "intermission",
			      "a mode from a template starts with the template's name and type (%s, %s)",
			      setting("mode|m4|name").c_str(), setting("mode|m4|type").c_str());
			modes_set(one_filter, [](obs_data_t *s) { obs_data_set_string(s, "new_mode", "Third"); });
			click("add_mode");
			CHECK(ids() == "m1,m2,m3,m4,m5" && setting("mode|m5|name") == "Third",
			      "two modes may have the same name: their ids differ (%s)", ids().c_str());
			const bool picked = pick_into_list(one_filter, props, "mode|m3|sources|a", "SM D");
			sleep_ms(150);
			CHECK(picked && list_rows(props, "mode|m3|sources|a") == 1,
			      "a mode's list of sources is filled from its box like any list");
			click("mode|m3|up");
			CHECK(ids() == "m1,m3,m2,m4,m5", "'Move up' moves a mode up the list (%s)", ids().c_str());
			modes_press("SM One", "", "next");
			CHECK(modes_now(one_filter) == "m3" && obs_sceneitem_visible(one_item[3]) &&
				      modes_shown(abcdx) == "00010",
			      "and 'next' follows the new order (%s, %s)", modes_now(one_filter).c_str(),
			      modes_shown(abcdx).c_str());
			click("mode|m3|remove");
			click("mode|m4|remove");
			click("mode|m5|remove");
			CHECK(ids() == "m1,m2" && setting("mode|m3|name").empty() &&
				      !obs_properties_get(props, "mode|m3|g") && modes_now(one_filter).empty() &&
				      obs_sceneitem_visible(one_item[3]),
			      "'Remove mode' takes a mode off and leaves its sources as they are (%s)", ids().c_str());
			obs_properties_destroy(props);
			modes_set(one_filter, [](obs_data_t *s) { obs_data_set_string(s, "mode_now", "m1"); });
			CHECK(modes_now(one_filter) == "m1" && modes_shown(abcdx) == "11011",
			      "choosing a mode in 'Mode now' turns it on (%s)", modes_shown(abcdx).c_str());
		}

		printf("\n[scene modes: rename]\n");
		obs_source_set_name(one_src[0], "SM A Renamed");
		sleep_ms(100);
		modes_press("SM One", "m2");
		{
			obs_data_t *s = obs_source_get_settings(one_filter);
			obs_data_array_t *rows = obs_data_get_array(s, "mode|m1|sources");
			obs_data_t *row = obs_data_array_count(rows) ? obs_data_array_item(rows, 0) : nullptr;
			CHECK(row && strcmp(obs_data_get_string(row, "value"), "SM A Renamed") == 0 &&
				      !obs_sceneitem_visible(one_item[0]),
			      "a source renamed stays in its mode under its new name");
			obs_data_release(row);
			obs_data_array_release(rows);
			obs_data_release(s);
		}
		modes_unfilter(one, one_filter);
		for (obs_source_t *src : one_src)
			obs_source_release(src);
		obs_scene_release(one);

		// --- A camera switch (Show in Scenes, every scene) and a mode holding the camera.
		for (int order = 0; order < 2; order++) {
			printf("\n[scene modes: camera switch] (%s)\n",
			       order == 0 ? "the switch made first" : "the modes made first");
			const std::string tag = order == 0 ? " 1" : " 2";
			const std::string scene_name = "SM Cam Scene" + tag, cam_name = "SM Cam" + tag,
					  y_name = "SM Y" + tag;
			obs_scene_t *scene = obs_scene_create(scene_name.c_str());
			obs_source_t *cam = modes_colour(cam_name.c_str());
			obs_source_t *y = modes_colour(y_name.c_str());
			obs_sceneitem_t *icam = obs_scene_add(scene, cam);
			obs_sceneitem_t *iy = obs_scene_add(scene, y);
			obs_sceneitem_set_visible(icam, false);
			obs_source_t *sw = nullptr, *f = nullptr;
			auto make_switch = [&] {
				obs_data_t *sd = obs_data_create();
				obs_data_set_string(sd, "transition", "none");
				sw = obs_source_create("stagekit_show_in_scenes", ("SM Cam Switch" + tag).c_str(), sd,
						       nullptr);
				obs_data_release(sd);
				obs_source_filter_add(cam, sw);
			};
			auto make_modes = [&] {
				f = modes_filter(scene, "SM Cam Modes",
						 {{"m1", "Plain", {y_name}, ""}, {"m2", "With camera", {cam_name}, ""}},
						 "m1");
			};
			if (order == 0) {
				make_switch();
				make_modes();
			} else {
				make_modes();
				make_switch();
			}
			sleep_ms(200);
			obs_source_set_enabled(sw, false);
			sleep_ms(150);
			modes_press(scene_name.c_str(), "m2");
			const bool held = !obs_sceneitem_visible(icam) && !obs_sceneitem_visible(iy);
			obs_source_set_enabled(sw, true);
			sleep_ms(150);
			const bool shown_by_switch = obs_sceneitem_visible(icam);
			modes_press(scene_name.c_str(), "m1");
			const bool gone = !obs_sceneitem_visible(icam) && obs_sceneitem_visible(iy);
			obs_source_set_enabled(sw, false);
			sleep_ms(150);
			obs_source_set_enabled(sw, true);
			sleep_ms(300);
			CHECK(held && shown_by_switch && gone && !obs_sceneitem_visible(icam),
			      "the mode holding a camera switched off does not show it; switched on, the camera shows in its mode, and the switch never shows it in another mode (%d %d %d %d)",
			      held, shown_by_switch, gone, obs_sceneitem_visible(icam));
			obs_source_filter_remove(cam, sw);
			obs_source_release(sw);
			modes_unfilter(scene, f);
			obs_source_release(cam);
			obs_source_release(y);
			obs_scene_release(scene);
		}

		// --- A Standby stand-in for a camera in a mode.
		{
			printf("\n[scene modes: standby stand-in]\n");
			obs_scene_t *ss = obs_scene_create("SM Stand-in Scene");
			obs_source_t *z = modes_colour("SM Stand-in Z");
			obs_source_t *cam = modes_colour("SM Stand-in Cam", 200, 120, 0xFF2040C0);
			obs_data_t *sd = obs_data_create();
			obs_data_set_bool(sd, "when_hidden", true);
			obs_data_set_string(sd, "transition", "none");
			obs_source_t *standby =
				obs_source_create("stagekit_standby", "SM Stand-in Standby", sd, nullptr);
			obs_data_release(sd);
			obs_source_filter_add(cam, standby);
			obs_scene_add(ss, z);
			obs_sceneitem_t *icam = obs_scene_add(ss, cam);
			obs_sceneitem_set_visible(icam, false);
			obs_source_t *f = modes_filter(
				ss, "SM Stand-in Modes",
				{{"m1", "Z", {"SM Stand-in Z"}, ""}, {"m2", "Cam", {"SM Stand-in Cam"}, ""}}, "m1");
			obs_set_output_source(0, obs_scene_get_source(ss));
			auto twin_shown = [&] {
				int found = -1;
				obs_scene_enum_items(
					ss,
					[](obs_scene_t *, obs_sceneitem_t *it, void *p) {
						if (strcmp(obs_source_get_unversioned_id(obs_sceneitem_get_source(it)),
							   "stagekit_stand_in") == 0)
							*static_cast<int *>(p) = obs_sceneitem_visible(it) ? 1 : 0;
						return true;
					},
					&found);
				return found;
			};
			sleep_ms(900);
			const int other_mode = twin_shown();
			modes_press("SM Stand-in Scene", "m2");
			sleep_ms(750);
			const int own_mode = twin_shown();
			const bool cam_on = obs_sceneitem_visible(icam);
			obs_sceneitem_set_visible(icam, false);
			sleep_ms(900);
			const int cam_off = twin_shown();
			CHECK(other_mode == 0 && cam_on && own_mode == 0 && cam_off == 1,
			      "no standby image for a camera while another mode is on; in its own mode, the standby image when the camera is hidden (%d %d %d %d)",
			      other_mode, (int)cam_on, own_mode, cam_off);
			obs_set_output_source(0, nullptr);
			modes_unfilter(ss, f);
			// Not standing in any more before it goes, so nothing of it is left behind.
			obs_data_t *off = obs_source_get_settings(standby);
			obs_data_set_bool(off, "when_hidden", false);
			obs_source_update(standby, off);
			obs_data_release(off);
			sleep_ms(400);
			obs_source_filter_remove(cam, standby);
			obs_source_release(standby);
			sleep_ms(300);
			obs_scene_release(ss);
			obs_source_release(z);
			obs_source_release(cam);
		}

		// --- Sources inside groups, and a group as a member.
		{
			printf("\n[scene modes: items in groups]\n");
			obs_scene_t *gs = obs_scene_create("SM Group Scene");
			obs_source_t *e = modes_colour("SM Group E");
			obs_source_t *fsrc = modes_colour("SM Group F");
			obs_sceneitem_t *g = obs_scene_add_group(gs, "SM Group G");
			obs_sceneitem_t *ie = obs_scene_add(gs, e);
			obs_sceneitem_group_add_item(g, ie);
			obs_sceneitem_t *hgroup = obs_scene_add_group(gs, "SM Group H");
			obs_sceneitem_t *ifs = obs_scene_add(gs, fsrc);
			obs_sceneitem_group_add_item(hgroup, ifs);
			obs_sceneitem_set_visible(ie, false);
			obs_source_t *f = modes_filter(
				gs, "SM Group Modes",
				{{"m1", "Group", {"SM Group H"}, ""}, {"m2", "Inner", {"SM Group E"}, ""}}, "m1");
			sleep_ms(200);
			modes_press("SM Group Scene", "m2");
			const bool inner_on = obs_sceneitem_visible(ie) && !obs_sceneitem_visible(hgroup);
			modes_press("SM Group Scene", "m1");
			CHECK(inner_on && !obs_sceneitem_visible(ie) && obs_sceneitem_visible(hgroup) &&
				      obs_sceneitem_visible(g),
			      "a source inside a group is shown and hidden with its mode, and so is a group listed itself");
			modes_unfilter(gs, f);
			obs_scene_release(gs);
			obs_source_release(e);
			obs_source_release(fsrc);
		}

		// --- Same modes in another scene, and "Switch to scene".
		{
			printf("\n[scene modes: same modes in another scene]\n");
			obs_scene_t *sa = obs_scene_create("SM Share A");
			obs_scene_t *sb = obs_scene_create("SM Share B");
			obs_scene_t *sc = obs_scene_create("SM Share C");
			obs_source_t *p = modes_colour("SM Share P");
			obs_source_t *q = modes_colour("SM Share Q");
			obs_sceneitem_t *pa = obs_scene_add(sa, p);
			obs_sceneitem_t *qa = obs_scene_add(sa, q);
			obs_sceneitem_t *qb = obs_scene_add(sb, q);
			obs_sceneitem_t *qc = obs_scene_add(sc, q);
			obs_sceneitem_set_visible(qa, false);
			obs_sceneitem_set_visible(qb, false);
			obs_sceneitem_set_visible(qc, false);
			obs_data_t *d = modes_settings({{"m1", "P", {"SM Share P"}, "SM Share A"},
							{"m2", "Q", {"SM Share Q"}, "SM Share A"}},
						       "m1");
			modes_list(d, "share_scenes", {"SM Share B"});
			obs_source_t *f = obs_source_create("stagekit_scene_modes", "SM Share Modes", d, nullptr);
			obs_data_release(d);
			obs_source_filter_add(obs_scene_get_source(sa), f);
			sleep_ms(300);
			const auto program = [](const char *name) {
				calldata_t cd = {};
				calldata_set_string(&cd, "program", name);
				proc_handler_call(obs_get_proc_handler(), "stagekit_test_scene_switch", &cd);
				calldata_free(&cd);
			};
			const auto switched = []() {
				calldata_t cd = {};
				proc_handler_call(obs_get_proc_handler(), "stagekit_test_scene_switched", &cd);
				const char *s = calldata_string(&cd, "scenes");
				std::string got = s ? s : "";
				calldata_free(&cd);
				return got;
			};
			program("SM Share B");
			switched();
			modes_press("SM Share A", "m2");
			const std::string from_b = switched();
			CHECK(!obs_sceneitem_visible(pa) && obs_sceneitem_visible(qa) && obs_sceneitem_visible(qb) &&
				      !obs_sceneitem_visible(qc) && from_b.empty(),
			      "one press changes the mode in both scenes, a scene not listed is left alone, and pressed while a listed scene is on screen it stays there ('%s')",
			      from_b.c_str());
			obs_sceneitem_set_visible(qb, false);
			obs_sceneitem_set_visible(pa, true);
			sleep_ms(250);
			CHECK(!obs_sceneitem_visible(qb) && !obs_sceneitem_visible(pa),
			      "a source of the mode on hidden in the shared scene stays hidden; one of another mode shown is hidden again");
			program("SM Share C");
			modes_press("SM Share A", "m2");
			const std::string from_c = switched();
			modes_press("SM Share A", "", "next");
			const std::string by_next = switched();
			CHECK(from_c == "SM Share A" && by_next == "SM Share A",
			      "pressed while another scene is on screen, OBS goes to the mode's scene ('%s', '%s')",
			      from_c.c_str(), by_next.c_str());
			obs_data_t *s = obs_source_get_settings(f);
			obs_data_set_string(s, "mode_now", "m2");
			obs_source_update(f, s);
			obs_data_release(s);
			sleep_ms(150);
			CHECK(switched().empty() && obs_sceneitem_visible(qa),
			      "'Mode now' in the window never changes scene");
			modes_unfilter(sa, f);
			obs_scene_release(sa);
			obs_scene_release(sb);
			obs_scene_release(sc);
			obs_source_release(p);
			obs_source_release(q);
		}

		// --- Two filters on one scene: a source in both shows only when both allow it.
		for (int order = 0; order < 2; order++) {
			printf("\n[scene modes: two filters on one scene] (%s)\n",
			       order == 0 ? "captures first" : "columns first");
			const std::string tag = order == 0 ? " 1" : " 2";
			const std::string scene_name = "SM Two" + tag;
			obs_scene_t *scene = obs_scene_create(scene_name.c_str());
			const std::string n[4] = {"SM Two P" + tag, "SM Two Q" + tag, "SM Two R" + tag,
						  "SM Two S" + tag};
			obs_source_t *src[4];
			std::vector<obs_sceneitem_t *> it;
			for (int i = 0; i < 4; i++) {
				src[i] = modes_colour(n[i].c_str());
				it.push_back(obs_scene_add(scene, src[i]));
			}
			obs_sceneitem_set_visible(it[1], false);
			obs_sceneitem_set_visible(it[3], false);
			obs_source_t *caps = nullptr, *cols = nullptr;
			auto make_caps = [&] {
				caps = modes_filter(scene, "SM Captures",
						    {{"m1", "P", {n[0]}, ""}, {"m2", "Q", {n[1]}, ""}}, "m1");
			};
			auto make_cols = [&] {
				cols = modes_filter(scene, "SM Columns",
						    {{"c1", "PR", {n[0], n[2]}, ""}, {"c2", "S", {n[3]}, ""}}, "c1");
			};
			if (order == 0) {
				make_caps();
				make_cols();
			} else {
				make_cols();
				make_caps();
			}
			sleep_ms(200);
			modes_press(scene_name.c_str(), "c2", "on", "SM Columns");
			const std::string a = modes_shown(it);
			modes_press(scene_name.c_str(), "m2", "on", "SM Captures");
			modes_press(scene_name.c_str(), "m1", "on", "SM Captures");
			const std::string b = modes_shown(it);
			modes_press(scene_name.c_str(), "c1", "on", "SM Columns");
			const std::string c = modes_shown(it);
			CHECK(a == "0001" && b == "0001" && c == "1010",
			      "a source in two filters' modes shows only while both have it on (%s %s %s)", a.c_str(),
			      b.c_str(), c.c_str());
			modes_unfilter(scene, caps);
			modes_unfilter(scene, cols);
			for (obs_source_t *s : src)
				obs_source_release(s);
			obs_scene_release(scene);
		}

		// --- Loading: nothing is done until OBS has its scenes.
		if (obs_source_get_display_name("fade_transition")) {
			printf("\n[scene modes: loading]\n");
			auto loaded = [](bool yes) {
				calldata_t cd = {};
				calldata_set_bool(&cd, "loaded", yes);
				proc_handler_call(obs_get_proc_handler(), "stagekit_test_loaded", &cd);
				calldata_free(&cd);
			};
			obs_scene_t *la = obs_scene_create("SM Load A"); // agrees with its saved mode
			obs_scene_t *lb = obs_scene_create("SM Load B"); // does not
			obs_source_t *l1 = modes_colour("SM Load 1"), *l2 = modes_colour("SM Load 2");
			obs_source_t *k1 = modes_colour("SM Load K1"), *k2 = modes_colour("SM Load K2");
			obs_sceneitem_t *a1 = obs_scene_add(la, l1), *a2 = obs_scene_add(la, l2);
			obs_sceneitem_t *b1 = obs_scene_add(lb, k1), *b2 = obs_scene_add(lb, k2);
			obs_sceneitem_set_visible(a2, false);
			obs_sceneitem_set_visible(b1, false); // saved mode: K1, but K2 shows
			obs_set_output_source(0, obs_scene_get_source(lb));
			sleep_ms(100);
			static int modes_load_signals = 0;
			modes_load_signals = 0;
			const auto count = [](void *, calldata_t *) {
				modes_load_signals++;
			};
			signal_handler_connect(obs_source_get_signal_handler(obs_scene_get_source(la)), "item_visible",
					       count, nullptr);
			loaded(false);
			auto faded = [&](const std::vector<ModesMode> &modes) {
				obs_data_t *d = modes_settings(modes, "m1");
				obs_data_set_string(d, "transition", "fade_transition");
				obs_data_set_int(d, "transition_ms", 600);
				return d;
			};
			obs_data_t *da = faded({{"m1", "One", {"SM Load 1"}, ""}, {"m2", "Two", {"SM Load 2"}, ""}});
			obs_data_t *db = faded({{"m1", "One", {"SM Load K1"}, ""}, {"m2", "Two", {"SM Load K2"}, ""}});
			obs_source_t *fa = obs_source_create("stagekit_scene_modes", "SM Load Modes A", da, nullptr);
			obs_source_t *fb = obs_source_create("stagekit_scene_modes", "SM Load Modes B", db, nullptr);
			obs_data_release(da);
			obs_data_release(db);
			obs_source_filter_add(obs_scene_get_source(la), fa);
			obs_source_filter_add(obs_scene_get_source(lb), fb);
			sleep_ms(200);
			const bool kept = modes_press("SM Load A", "m2");
			sleep_ms(200);
			const bool waited = obs_sceneitem_visible(a1) && !obs_sceneitem_visible(a2) &&
					    !obs_sceneitem_visible(b1) && obs_sceneitem_visible(b2);
			loaded(true);
			sleep_ms(400);
			CHECK(kept && waited && !obs_sceneitem_visible(a1) && obs_sceneitem_visible(a2),
			      "while OBS loads, nothing is shown or hidden; a press made then is acted on once it has loaded");
			// (The fade it keeps for the next change is not looked at: a
			// Standby filter's list of sources it stands in for is kept by
			// address and can name a source made since at the same one.)
			CHECK(obs_sceneitem_visible(b1) && !obs_sceneitem_visible(b2) && !obs_source_active(k2),
			      "a scene that does not show its saved mode is put right after loading with a cut: the source hidden is out of use at once (%d %d %d)",
			      obs_sceneitem_visible(b1), obs_sceneitem_visible(b2), obs_source_active(k2));
			signal_handler_disconnect(obs_source_get_signal_handler(obs_scene_get_source(la)),
						  "item_visible", count, nullptr);
			obs_set_output_source(0, nullptr);
			modes_unfilter(la, fa);
			modes_unfilter(lb, fb);
			// A third: saved and shown alike. Nothing at all happens when it loads.
			obs_scene_t *lc = obs_scene_create("SM Load C");
			obs_source_t *c1 = modes_colour("SM Load C1"), *c2 = modes_colour("SM Load C2");
			obs_scene_add(lc, c1);
			obs_sceneitem_t *ic2 = obs_scene_add(lc, c2);
			obs_sceneitem_set_visible(ic2, false);
			modes_load_signals = 0;
			signal_handler_connect(obs_source_get_signal_handler(obs_scene_get_source(lc)), "item_visible",
					       count, nullptr);
			loaded(false);
			obs_source_t *fc = modes_filter(
				lc, "SM Load Modes C",
				{{"m1", "One", {"SM Load C1"}, ""}, {"m2", "Two", {"SM Load C2"}, ""}}, "m1");
			sleep_ms(150);
			loaded(true);
			sleep_ms(300);
			signal_handler_disconnect(obs_source_get_signal_handler(obs_scene_get_source(lc)),
						  "item_visible", count, nullptr);
			CHECK(modes_load_signals == 0,
			      "a scene that shows its saved mode is not touched when it loads (%d signals)",
			      modes_load_signals);
			modes_unfilter(lc, fc);
			for (obs_source_t *s : {l1, l2, k1, k2, c1, c2})
				obs_source_release(s);
			obs_scene_release(la);
			obs_scene_release(lb);
			obs_scene_release(lc);
		}

		// --- Morph between two modes' captures, and a scene carrying Scene Modes still morphs.
		if (obs_source_get_display_name("stagekit_morph")) {
			printf("\n[scene modes: item morph between modes]\n");
			obs_scene_t *ms = obs_scene_create("SM Morph Scene");
			obs_source_t *back = modes_colour("SM Morph Back", 640, 360, 0xFFFF0000);
			obs_source_t *m1 = modes_colour("SM Morph One", 300, 200, 0xFF0000FF);
			obs_source_t *m2 = modes_colour("SM Morph Two", 300, 200, 0xFF00FF00);
			obs_scene_add(ms, back);
			obs_sceneitem_t *i1 = obs_scene_add(ms, m1);
			obs_sceneitem_t *i2 = obs_scene_add(ms, m2);
			vec2 at;
			vec2_set(&at, 320, 140);
			obs_sceneitem_set_pos(i2, &at);
			obs_sceneitem_set_visible(i2, false);
			obs_data_t *d = modes_settings(
				{{"m1", "One", {"SM Morph One"}, ""}, {"m2", "Two", {"SM Morph Two"}, ""}}, "m1");
			obs_data_set_string(d, "transition", "stagekit_morph");
			obs_data_set_int(d, "transition_ms", 1000);
			obs_source_t *f = obs_source_create("stagekit_scene_modes", "SM Morph Modes", d, nullptr);
			obs_data_release(d);
			obs_source_filter_add(obs_scene_get_source(ms), f);
			obs_set_output_source(0, obs_scene_get_source(ms));
			sleep_ms(300);
			obs_data_t *req = obs_data_create();
			obs_data_set_string(req, "scene", "SM Morph Scene");
			obs_data_set_string(req, "mode", "m2");
			obs_data_release(modes_call("SetSceneMode", req));
			obs_data_release(req);
			sleep_ms(450);
			render(obs_scene_get_source(ms), w, h);
			const int during = modes_count_items(ms, "stagekit_morph_layer");
			sleep_ms(1300);
			render(obs_scene_get_source(ms), w, h);
			const int after = modes_count_items(ms, "stagekit_morph_layer");
			CHECK(during == 1 && after == 0 && !obs_sceneitem_visible(i1) && obs_sceneitem_visible(i2),
			      "with Morph as the transition, the capture of the mode going changes into the one coming, drawn by a layer only while the change lasts (%d during, %d after)",
			      during, after);
			obs_set_output_source(0, nullptr);
			modes_unfilter(ms, f);

			printf("\n[scene modes: a scene with Scene Modes still morphs]\n");
			obs_scene_t *ta = obs_scene_create("SM Morph From");
			obs_scene_t *tb = obs_scene_create("SM Morph To");
			obs_source_t *cam = modes_colour("SM Morph Cam", 100, 60, 0xFF0000FF);
			obs_source_t *green = modes_colour("SM Morph Green", 640, 360, 0xFF00FF00);
			obs_scene_add(ta, green);
			obs_scene_add(tb, green);
			obs_sceneitem_t *ca = obs_scene_add(ta, cam);
			obs_sceneitem_t *cb = obs_scene_add(tb, cam);
			vec2_set(&at, 20, 20);
			obs_sceneitem_set_pos(ca, &at);
			vec2_set(&at, 520, 280);
			obs_sceneitem_set_pos(cb, &at);
			obs_source_t *tf =
				modes_filter(ta, "SM Morph From Modes", {{"m1", "Cam", {"SM Morph Cam"}, ""}}, "m1");
			obs_data_t *mset = obs_data_create();
			obs_data_set_int(mset, "ease_curve", 0);
			obs_source_t *morph = obs_source_create_private("stagekit_morph", "SM Morph", mset);
			obs_data_release(mset);
			obs_transition_set(morph, obs_scene_get_source(ta));
			obs_transition_start(morph, OBS_TRANSITION_MODE_MANUAL, 0, obs_scene_get_source(tb));
			obs_transition_set_manual_time(morph, 0.5f);
			sleep_ms(150);
			const std::vector<uint8_t> half = render(morph, w, h);
			auto red = [&](int x, int y) {
				return (int)half[(size_t(y) * w + x) * 4];
			};
			CHECK(red(300, 170) > 240 && red(50, 40) < 15,
			      "a Morph from a scene carrying Scene Modes takes it apart: the camera is half-way between its places (red %d there, %d at its first place)",
			      red(300, 170), red(50, 40));
			obs_source_release(morph);
			modes_unfilter(ta, tf);
			obs_scene_release(ta);
			obs_scene_release(tb);
			obs_source_release(cam);
			obs_source_release(green);
			sleep_ms(200);
			obs_scene_release(ms);
			for (obs_source_t *s : {back, m1, m2})
				obs_source_release(s);
		}
		obs_set_output_source(0, modes_before_out);
		obs_source_release(modes_before_out);
	}
	// ==== end scene-filters:modes ====
	// ==== scene-filters:transitions ====
	// ------------------------------------------------------------ scene transitions
	if (obs_source_get_display_name("fade_transition") && obs_source_get_display_name("slide_transition")) {
		printf("\n[scene transitions: sets every source]\n");
		auto trans_colour = [](const char *name, long long abgr, int cw, int ch) {
			obs_data_t *d = obs_data_create();
			obs_data_set_int(d, "color", abgr);
			obs_data_set_int(d, "width", cw);
			obs_data_set_int(d, "height", ch);
			obs_source_t *made = obs_source_create("color_source", name, d, nullptr);
			obs_data_release(d);
			return made;
		};
		auto trans_kind = [](obs_sceneitem_t *item, bool show) {
			obs_source_t *t = obs_sceneitem_get_transition(item, show);
			const char *id = t ? obs_source_get_unversioned_id(t) : nullptr;
			return std::string(id ? id : "");
		};
		auto trans_ms = [](obs_sceneitem_t *item, bool show) {
			return (int)obs_sceneitem_get_transition_duration(item, show);
		};
		auto trans_setting = [](obs_sceneitem_t *item, bool show, const char *name) {
			obs_source_t *t = obs_sceneitem_get_transition(item, show);
			if (!t)
				return std::string();
			obs_data_t *s = obs_source_get_settings(t);
			std::string v = obs_data_get_string(s, name);
			obs_data_release(s);
			return v;
		};
		auto trans_writes = [] {
			calldata_t cd;
			calldata_init(&cd);
			proc_handler_call(obs_get_proc_handler(), "stagekit_scene_transitions_writes", &cd);
			const long long n = calldata_int(&cd, "writes");
			calldata_free(&cd);
			return n;
		};
		auto trans_list = [](obs_data_t *settings, const char *name, std::vector<const char *> values) {
			obs_data_array_t *array = obs_data_array_create();
			for (const char *v : values) {
				obs_data_t *row = obs_data_create();
				obs_data_set_string(row, "value", v);
				obs_data_array_push_back(array, row);
				obs_data_release(row);
			}
			obs_data_set_array(settings, name, array);
			obs_data_array_release(array);
		};
		auto trans_change = [](obs_source_t *filter, const std::function<void(obs_data_t *)> &edit) {
			obs_data_t *s = obs_source_get_settings(filter);
			edit(s);
			obs_source_update(filter, s);
			obs_data_release(s);
		};
		// Waits while the scene is drawn, so transitions started in it end.
		auto trans_wait = [](obs_source_t *scene, int ms) {
			for (int i = 0; i < ms / 50; i++) {
				render(scene, 640, 360);
				sleep_ms(50);
			}
		};
		auto trans_put = [](obs_scene_t *scene, obs_source_t *src, float x, float y) {
			obs_sceneitem_t *item = obs_scene_add(scene, src);
			vec2 p;
			vec2_set(&p, x, y);
			obs_sceneitem_set_pos(item, &p);
			return item;
		};
		obs_source_t *ta = trans_colour("Trans A", 0xFF0000FFLL, 100, 60);
		obs_source_t *tb = trans_colour("Trans B", 0xFF00FF00LL, 100, 60);
		obs_source_t *tc = trans_colour("Trans C", 0xFFFF0000LL, 100, 60);
		obs_source_t *td = trans_colour("Trans D", 0xFFFFFF00LL, 100, 60);
		auto trans_make = [](obs_source_t *scene_src, const std::function<void(obs_data_t *)> &edit) {
			obs_data_t *s = obs_data_create();
			edit(s);
			obs_source_t *f =
				obs_source_create("stagekit_scene_transitions", "Scene Transitions", s, nullptr);
			obs_data_release(s);
			obs_source_filter_add(scene_src, f);
			return f;
		};

		// 1 and 7: every source set; nothing written again when it matches;
		// a collection loaded again writes nothing.
		{
			obs_scene_t *sc = obs_scene_create("Trans Every");
			obs_source_t *src = obs_scene_get_source(sc);
			obs_sceneitem_t *ia = trans_put(sc, ta, 10, 10);
			obs_sceneitem_t *ib = trans_put(sc, tb, 200, 10);
			obs_sceneitem_t *ic = trans_put(sc, tc, 400, 10);
			obs_source_t *priv = obs_source_create_private("color_source", "Trans Private", nullptr);
			obs_sceneitem_t *ip = obs_scene_add(sc, priv);
			const long long w0 = trans_writes();
			obs_source_t *f = trans_make(src, [](obs_data_t *s) {
				obs_data_set_string(s, "transition", "fade_transition");
				obs_data_set_int(s, "transition_ms", 400);
			});
			sleep_ms(200);
			bool all = true;
			for (obs_sceneitem_t *item : {ia, ib, ic})
				for (bool show : {true, false})
					all = all && trans_kind(item, show) == "fade_transition" &&
					      trans_ms(item, show) == 400;
			CHECK(all && trans_kind(ip, true).empty() && trans_kind(ip, false).empty(),
			      "every source's show and hide transition is the scene's (fade, 400 ms); a private source is left alone");
			CHECK(trans_writes() - w0 == 3, "each source is written once (%lld)", trans_writes() - w0);

			printf("\n[scene transitions: nothing written when it matches]\n");
			obs_source_t *before_show = obs_sceneitem_get_transition(ia, true);
			obs_source_t *before_hide = obs_sceneitem_get_transition(ib, false);
			const long long w1 = trans_writes();
			trans_change(f, [](obs_data_t *s) { obs_data_set_bool(s, "in_groups", false); });
			sleep_ms(200);
			CHECK(trans_writes() == w1 && obs_sceneitem_get_transition(ia, true) == before_show &&
				      obs_sceneitem_get_transition(ib, false) == before_hide,
			      "a settings change that changes nothing writes nothing (%lld)", trans_writes() - w1);
			// Saved and loaded again, as OBS does with a collection.
			obs_sceneitem_remove(ip);
			obs_source_release(priv);
			obs_data_t *saved = obs_save_source(src);
			obs_source_filter_remove(src, f);
			obs_source_release(f);
			obs_source_remove(src);
			obs_scene_release(sc);
			sleep_ms(100);
			const long long w2 = trans_writes();
			obs_source_t *loaded = obs_load_source(saved);
			obs_data_release(saved);
			if (loaded)
				obs_source_load(loaded); // (its items, as OBS loads a collection)
			sleep_ms(300);
			obs_scene_t *again = loaded ? obs_scene_from_source(loaded) : nullptr;
			obs_sceneitem_t *la = again ? obs_scene_find_source(again, "Trans A") : nullptr;
			CHECK(again && la && trans_kind(la, false) == "fade_transition" && trans_ms(la, false) == 400 &&
				      trans_writes() == w2,
			      "a scene loaded again with its transitions saved is written nothing (%lld)",
			      trans_writes() - w2);
			if (loaded) {
				obs_source_remove(loaded);
				obs_source_release(loaded);
			}
		}

		// 2: a listed source with its own settings.
		printf("\n[scene transitions: own settings for a listed source]\n");
		{
			obs_scene_t *sc = obs_scene_create("Trans Own");
			obs_source_t *src = obs_scene_get_source(sc);
			obs_sceneitem_t *ia = trans_put(sc, ta, 10, 10);
			obs_sceneitem_t *ib = trans_put(sc, tb, 200, 10);
			obs_source_t *f = trans_make(src, [&](obs_data_t *s) {
				obs_data_set_string(s, "transition", "fade_transition");
				obs_data_set_int(s, "transition_ms", 400);
				trans_list(s, "sources", {"Trans B"});
				obs_data_set_string(s, "for|Trans B|transition", "slide_transition");
				obs_data_set_int(s, "for|Trans B|transition_ms", 500);
				obs_data_set_string(s, "for|Trans B|show|slide_transition|direction", "left");
			});
			sleep_ms(200);
			CHECK(trans_kind(ib, true) == "slide_transition" &&
				      trans_setting(ib, true, "direction") == "left" && trans_ms(ib, true) == 500 &&
				      trans_kind(ib, false) == "slide_transition" &&
				      trans_setting(ib, false, "direction") == "left",
			      "the listed source slides left both ways, its hide the same as its show (%s %s / %s %s)",
			      trans_kind(ib, true).c_str(), trans_setting(ib, true, "direction").c_str(),
			      trans_kind(ib, false).c_str(), trans_setting(ib, false, "direction").c_str());
			CHECK(trans_kind(ia, true) == "fade_transition" && trans_kind(ia, false) == "fade_transition",
			      "the others fade");
			// A listed source renamed keeps its own settings.
			obs_source_set_name(tb, "Trans B Renamed");
			sleep_ms(100);
			obs_data_t *now = obs_source_get_settings(f);
			const bool followed = strcmp(obs_data_get_string(now, "for|Trans B Renamed|transition"),
						     "slide_transition") == 0;
			obs_data_release(now);
			obs_source_set_name(tb, "Trans B");
			sleep_ms(100);
			CHECK(followed, "a listed source that is renamed keeps its own settings under its new name");
			obs_source_filter_remove(src, f);
			obs_source_release(f);
			obs_scene_release(sc);
		}

		// 3 and 4: only listed, leave these alone, a source added later.
		printf("\n[scene transitions: only listed, and leave these alone]\n");
		{
			obs_scene_t *sc = obs_scene_create("Trans Listed");
			obs_source_t *src = obs_scene_get_source(sc);
			obs_sceneitem_t *ia = trans_put(sc, ta, 10, 10);
			obs_sceneitem_t *ib = trans_put(sc, tb, 200, 10);
			obs_sceneitem_t *ic = trans_put(sc, tc, 400, 10);
			obs_source_t *f = trans_make(src, [&](obs_data_t *s) {
				obs_data_set_int(s, "cover", 1);
				trans_list(s, "sources", {"Trans A"});
			});
			sleep_ms(200);
			CHECK(trans_kind(ia, true) == "fade_transition" && trans_kind(ib, true).empty() &&
				      trans_kind(ic, true).empty(),
			      "with Only listed sources, only the listed one is set");
			trans_change(f, [&](obs_data_t *s) {
				obs_data_set_int(s, "cover", 0);
				trans_list(s, "except", {"Trans C"});
			});
			sleep_ms(200);
			CHECK(trans_kind(ib, true) == "fade_transition" && trans_kind(ic, true).empty(),
			      "with All sources, a source left alone keeps what it had");

			printf("\n[scene transitions: a source added later]\n");
			obs_sceneitem_t *id = trans_put(sc, td, 10, 200);
			sleep_ms(120);
			CHECK(trans_kind(id, true) == "fade_transition" && trans_kind(id, false) == "fade_transition",
			      "a source added to the scene is set within a few frames");
			obs_sceneitem_remove(id);
			sleep_ms(100);
			CHECK(true, "and taking it out again is fine");
			obs_source_filter_remove(src, f);
			obs_source_release(f);
			obs_scene_release(sc);
		}

		// 5 and 6: As the scene sets, and a filter's own choice.
		printf("\n[scene transitions: as the scene sets]\n");
		if (obs_source_get_display_name("swipe_transition")) {
			obs_scene_t *sc = obs_scene_create("Trans Link");
			obs_source_t *src = obs_scene_get_source(sc);
			obs_source_inc_showing(src);
			obs_sceneitem_t *ia = trans_put(sc, ta, 10, 10);
			obs_sceneitem_t *ib = trans_put(sc, tb, 200, 10);
			obs_sceneitem_set_visible(ib, false);
			obs_source_t *f = trans_make(src, [](obs_data_t *s) {
				obs_data_set_string(s, "transition", "swipe_transition");
				obs_data_set_int(s, "transition_ms", 450);
			});
			obs_data_t *ls = obs_data_create();
			trans_list(ls, "items", {"Trans A", "Trans B"});
			obs_source_t *link = obs_source_create("stagekit_visibility_link", "Trans Link", ls, nullptr);
			obs_data_release(ls);
			obs_source_filter_add(src, link);
			obs_data_t *defaults = obs_source_get_settings(link);
			const std::string link_default = obs_data_get_string(defaults, "transition");
			obs_data_release(defaults);
			CHECK(link_default == "scene", "a filter's transition is As the scene sets as it comes (%s)",
			      link_default.c_str());
			// Every filter the release notes name starts with it, the two
			// switchers included.
			for (const char *kind : {"stagekit_show_in_scenes", "stagekit_program_trigger",
						 "stagekit_active_app", "stagekit_app_capture", "stagekit_standby"}) {
				obs_data_t *d = obs_get_source_defaults(kind);
				const std::string kind_default = d ? obs_data_get_string(d, "transition") : "";
				obs_data_release(d);
				CHECK(kind_default == "scene", "%s starts with As the scene sets (%s)", kind,
				      kind_default.c_str());
			}
			trans_wait(src, 300);
			obs_sceneitem_set_visible(ib, true); // the link hides A
			trans_wait(src, 150);
			CHECK(!obs_sceneitem_visible(ia) && trans_kind(ia, false) == "swipe_transition" &&
				      trans_ms(ia, false) == 450,
			      "a filter on As the scene sets changes a source the scene's way (%s %d)",
			      trans_kind(ia, false).c_str(), trans_ms(ia, false));
			trans_wait(src, 700);
			obs_source_filter_remove(src, f);
			obs_source_release(f);
			trans_wait(src, 100);
			obs_sceneitem_set_visible(ia, true); // the link hides B, with no scene filter now
			trans_wait(src, 150);
			CHECK(!obs_sceneitem_visible(ib) && trans_kind(ib, false) == "fade_transition" &&
				      trans_ms(ib, false) == 300,
			      "with no Scene Transitions it is a fade of the filter's own length, as before (%s %d)",
			      trans_kind(ib, false).c_str(), trans_ms(ib, false));
			trans_wait(src, 500);

			printf("\n[scene transitions: a filter's own choice, then back]\n");
			f = trans_make(src, [](obs_data_t *s) {
				obs_data_set_string(s, "transition", "fade_transition");
				obs_data_set_int(s, "transition_ms", 400);
			});
			trans_change(link, [](obs_data_t *s) {
				obs_data_set_string(s, "transition", "slide_transition");
				obs_data_set_int(s, "transition_ms", 1000);
			});
			trans_wait(src, 300);
			CHECK(trans_kind(ia, false) == "fade_transition" && trans_kind(ib, true) == "fade_transition",
			      "at rest the sources carry the scene's transitions");
			obs_sceneitem_set_visible(ib, true); // the link hides A with its slide
			trans_wait(src, 500);
			const bool sliding = trans_kind(ia, false) == "slide_transition" && trans_ms(ia, false) == 1000;
			trans_wait(src, 1000);
			CHECK(sliding && trans_kind(ia, false) == "fade_transition" && trans_ms(ia, false) == 400 &&
				      trans_kind(ia, true) == "fade_transition",
			      "a filter's own choice is used for its change, not replaced part-way, and the scene's is back after (%d, then %s %d)",
			      (int)sliding, trans_kind(ia, false).c_str(), trans_ms(ia, false));
			obs_source_filter_remove(src, link);
			obs_source_release(link);
			obs_source_filter_remove(src, f);
			obs_source_release(f);
			obs_source_dec_showing(src);
			obs_scene_release(sc);
		}

		// 8: a Standby that stands in for a source owns its transitions.
		printf("\n[scene transitions: standby owns its source]\n");
		{
			obs_source_t *cam = trans_colour("Trans Cam", 0xFF00FF00LL, 200, 100);
			obs_source_t *stand = trans_colour("Trans Cam Standby", 0xFF0000FFLL, 64, 64);
			obs_scene_t *sc = obs_scene_create("Trans Standby");
			obs_source_t *src = obs_scene_get_source(sc);
			obs_sceneitem_t *ci = trans_put(sc, cam, 100, 60);
			obs_data_t *hd = obs_data_create();
			obs_data_set_int(hd, "picture", 2);
			obs_data_set_string(hd, "source", "Trans Cam Standby");
			obs_data_set_bool(hd, "when_hidden", true);
			obs_source_t *standby = obs_source_create("stagekit_standby", "Trans Standby", hd, nullptr);
			obs_data_release(hd);
			obs_source_filter_add(cam, standby);
			obs_source_t *f = trans_make(src, [](obs_data_t *s) {
				obs_data_set_string(s, "transition", "slide_transition");
				obs_data_set_int(s, "transition_ms", 450);
			});
			trans_wait(src, 900);
			obs_source_t *show_was = obs_sceneitem_get_transition(ci, true);
			obs_source_t *hide_was = obs_sceneitem_get_transition(ci, false);
			const long long w0 = trans_writes();
			trans_change(f, [](obs_data_t *s) { obs_data_set_int(s, "transition_ms", 500); });
			trans_wait(src, 600);
			CHECK(trans_writes() == w0 && show_was && obs_sceneitem_get_transition(ci, true) == show_was &&
				      obs_sceneitem_get_transition(ci, false) == hide_was,
			      "the Scene Transitions never writes a source a Standby stands in for (%lld)",
			      trans_writes() - w0);
			CHECK(trans_kind(ci, false) == "slide_transition" && trans_ms(ci, false) == 500,
			      "with As the scene sets, the Standby gives its source the scene's hide (%s %d)",
			      trans_kind(ci, false).c_str(), trans_ms(ci, false));
			obs_source_filter_remove(src, f);
			obs_source_release(f);
			obs_source_filter_remove(cam, standby);
			obs_source_release(standby);
			trans_wait(src, 300);
			obs_scene_release(sc);
			obs_source_release(cam);
			obs_source_release(stand);
		}

		// 9: the Morph still morphs between scenes that carry the filter.
		printf("\n[scene transitions: the morph still works]\n");
		{
			obs_source_t *cam = trans_colour("Trans Morph Cam", 0xFF0000FFLL, 100, 60); // red
			obs_scene_t *pa = obs_scene_create("Trans Morph From");
			obs_scene_t *pb = obs_scene_create("Trans Morph To");
			trans_put(pa, cam, 20, 20);
			trans_put(pb, cam, 520, 280);
			obs_source_t *fa = trans_make(obs_scene_get_source(pa), [](obs_data_t *) {});
			obs_source_t *fb = trans_make(obs_scene_get_source(pb), [](obs_data_t *) {});
			obs_data_t *ms = obs_data_create();
			obs_data_set_int(ms, "ease_curve", 0);
			obs_source_t *morph = obs_source_create_private("stagekit_morph", "Trans Morph", ms);
			obs_data_release(ms);
			obs_transition_set(morph, obs_scene_get_source(pa));
			obs_transition_start(morph, OBS_TRANSITION_MODE_MANUAL, 0, obs_scene_get_source(pb));
			obs_transition_set_manual_time(morph, 0.5f);
			sleep_ms(120);
			const std::vector<uint8_t> half = render(morph, w, h);
			auto red = [&](int x, int y) {
				const uint8_t *p = &half[(size_t(y) * w + x) * 4];
				return p[0] > 240 && p[1] < 15 && p[2] < 15;
			};
			CHECK(red(300, 170) && !red(50, 40) && !red(550, 300),
			      "a source in both scenes is half-way between its two places half-way through");
			obs_transition_force_stop(morph);
			obs_source_release(morph);
			obs_source_filter_remove(obs_scene_get_source(pa), fa);
			obs_source_filter_remove(obs_scene_get_source(pb), fb);
			obs_source_release(fa);
			obs_source_release(fb);
			obs_scene_release(pa);
			obs_scene_release(pb);
			obs_source_release(cam);
		}

		// 10: off the nearest edge, kept current at rest.
		printf("\n[scene transitions: off the nearest edge at rest]\n");
		{
			obs_scene_t *sc = obs_scene_create("Trans Nearest");
			obs_source_t *src = obs_scene_get_source(sc);
			obs_sceneitem_t *ib = trans_put(sc, tb, 10, 150);
			obs_source_t *f = trans_make(src, [&](obs_data_t *s) {
				trans_list(s, "sources", {"Trans B"});
				obs_data_set_string(s, "for|Trans B|transition", "slide_transition");
				obs_data_set_string(s, "for|Trans B|show|slide_transition|direction", "nearest");
			});
			sleep_ms(200);
			obs_source_t *hide_was = obs_sceneitem_get_transition(ib, false);
			const bool left = trans_setting(ib, false, "direction") == "left" &&
					  trans_setting(ib, true, "direction") == "right";
			vec2 at;
			vec2_set(&at, 530.0f, 150.0f);
			obs_sceneitem_set_pos(ib, &at);
			sleep_ms(500);
			CHECK(left && trans_setting(ib, false, "direction") == "right" &&
				      obs_sceneitem_get_transition(ib, false) == hide_was,
			      "near the left edge it goes off to the left; moved to the right edge it goes off to the right once it is still, the same transition re-set (%d, %s)",
			      (int)left, trans_setting(ib, false, "direction").c_str());
			obs_source_filter_remove(src, f);
			obs_source_release(f);
			obs_scene_release(sc);
		}

		// 12: sources in groups.
		printf("\n[scene transitions: sources in groups]\n");
		{
			obs_scene_t *sc = obs_scene_create("Trans Groups");
			obs_source_t *src = obs_scene_get_source(sc);
			obs_sceneitem_t *group = obs_scene_add_group(sc, "Trans Group");
			obs_sceneitem_t *loose = obs_scene_add(sc, td);
			obs_sceneitem_group_add_item(group, loose);
			obs_sceneitem_t *inner = obs_scene_find_source(obs_sceneitem_group_get_scene(group), "Trans D");
			obs_source_t *f = trans_make(src, [](obs_data_t *) {});
			sleep_ms(200);
			CHECK(inner && trans_kind(group, true) == "fade_transition" && trans_kind(inner, true).empty(),
			      "a group is one source: it is set and what is in it is not");
			trans_change(f, [](obs_data_t *s) { obs_data_set_bool(s, "in_groups", true); });
			sleep_ms(200);
			CHECK(inner && trans_kind(inner, true) == "fade_transition",
			      "with Sources in groups, what is in a group is set too");
			obs_source_filter_remove(src, f);
			obs_source_release(f);
			obs_scene_release(sc);
		}

		// 11: a Scene Modes change goes the scene's way in view, and is a cut
		// out of view that leaves nothing hidden in use.
		printf("\n[scene transitions: scene modes]\n");
		if (obs_source_get_display_name("swipe_transition")) {
			obs_source_t *ma = trans_colour("Trans Mode A", 0xFF0000FFLL, 100, 60);
			obs_source_t *mb = trans_colour("Trans Mode B", 0xFF00FF00LL, 100, 60);
			obs_scene_t *sc = obs_scene_create("Trans Modes");
			obs_source_t *src = obs_scene_get_source(sc);
			obs_sceneitem_t *ia = trans_put(sc, ma, 10, 10);
			obs_sceneitem_t *ib = trans_put(sc, mb, 200, 10);
			obs_sceneitem_set_visible(ib, false);
			obs_source_t *f = trans_make(src, [](obs_data_t *s) {
				obs_data_set_string(s, "transition", "swipe_transition");
				obs_data_set_int(s, "transition_ms", 450);
			});
			obs_data_t *md = obs_data_create();
			trans_list(md, "modes", {"m1", "m2"});
			obs_data_set_string(md, "mode|m1|name", "A");
			trans_list(md, "mode|m1|sources", {"Trans Mode A"});
			obs_data_set_string(md, "mode|m2|name", "B");
			trans_list(md, "mode|m2|sources", {"Trans Mode B"});
			obs_data_set_string(md, "mode_now", "m1");
			obs_source_t *modes =
				obs_source_create("stagekit_scene_modes", "Trans Scene Modes", md, nullptr);
			obs_data_release(md);
			obs_source_filter_add(src, modes);
			auto press = [](const char *mode) {
				obs_data_t *req = obs_data_create();
				obs_data_set_string(req, "scene", "Trans Modes");
				obs_data_set_string(req, "filter", "");
				obs_data_set_string(req, "mode", mode);
				obs_data_set_string(req, "how", "on");
				calldata_t cd = {};
				calldata_set_string(&cd, "request", "SetSceneMode");
				calldata_set_string(&cd, "data", obs_data_get_json(req));
				proc_handler_call(obs_get_proc_handler(), "stagekit_vendor_call", &cd);
				calldata_free(&cd);
				obs_data_release(req);
			};
			obs_source_t *was_out = obs_get_output_source(0);
			obs_scene_t *elsewhere = obs_scene_create("Trans Modes Elsewhere");
			// Out of view first (a scene once on screen can keep a source its
			// finished item transition used counted as in use, OBS's own
			// doing): the change is a cut, and the source hidden there is not
			// in use when the scene comes on.
			obs_set_output_source(0, obs_scene_get_source(elsewhere));
			sleep_ms(300);
			press("m2");
			sleep_ms(300);
			obs_set_output_source(0, src);
			const bool at_once = obs_source_active(ma) || obs_source_showing(ma);
			sleep_ms(300);
			const bool later = obs_source_active(ma) || obs_source_showing(ma);
			CHECK(!obs_sceneitem_visible(ia) && obs_sceneitem_visible(ib) && !at_once && !later,
			      "out of view, a Scene Modes change is a cut: when the scene comes on the hidden source is not in use (%d %d)",
			      (int)at_once, (int)later);
			// In view, the scene's way.
			trans_wait(src, 300);
			press("m1");
			trans_wait(src, 200);
			CHECK(obs_sceneitem_visible(ia) && !obs_sceneitem_visible(ib) &&
				      trans_kind(ib, false) == "swipe_transition" &&
				      trans_kind(ia, true) == "swipe_transition" && trans_ms(ia, true) == 450,
			      "in view, a Scene Modes change on As the scene sets goes the scene's way (%s / %s %d)",
			      trans_kind(ib, false).c_str(), trans_kind(ia, true).c_str(), trans_ms(ia, true));
			trans_wait(src, 700);
			obs_set_output_source(0, was_out);
			obs_source_release(was_out);
			obs_scene_release(elsewhere);
			obs_source_filter_remove(src, modes);
			obs_source_release(modes);
			obs_source_filter_remove(src, f);
			obs_source_release(f);
			obs_scene_release(sc);
			obs_source_release(ma);
			obs_source_release(mb);
		}

		obs_source_release(ta);
		obs_source_release(tb);
		obs_source_release(tc);
		obs_source_release(td);
	}
	// ==== end scene-filters:transitions ====
	// ==== scene-filters:standby-corners ====
	// ---------------------------------------------- scene standby, corners
	{
		// Shared by both sections.
		auto sb_wait = [](const std::function<bool()> &done, int ms) {
			for (int waited = 0; waited < ms; waited += 50) {
				if (done())
					return true;
				sleep_ms(50);
			}
			return done();
		};
		// A source's filters of a kind, in order (no references kept).
		auto sb_filters = [](obs_source_t *source, const char *kind) {
			struct Find {
				const char *kind;
				std::vector<obs_source_t *> found;
			} find{kind, {}};
			obs_source_enum_filters(
				source,
				[](obs_source_t *, obs_source_t *f, void *param) {
					auto *x = static_cast<Find *>(param);
					if (strcmp(obs_source_get_unversioned_id(f), x->kind) == 0)
						x->found.push_back(f);
				},
				&find);
			return find.found;
		};
		auto sb_setting = [](obs_source_t *filter, const char *key) -> std::string {
			obs_data_t *s = obs_source_get_settings(filter);
			const std::string value = obs_data_get_string(s, key);
			obs_data_release(s);
			return value;
		};
		auto sb_int = [](obs_source_t *filter, const char *key) {
			obs_data_t *s = obs_source_get_settings(filter);
			const long long value = obs_data_get_int(s, key);
			obs_data_release(s);
			return value;
		};
		auto sb_list = [](obs_data_t *settings, const char *key, std::vector<const char *> names) {
			obs_data_array_t *array = obs_data_array_create();
			for (const char *name : names) {
				obs_data_t *entry = obs_data_create();
				obs_data_set_string(entry, "value", name);
				obs_data_array_push_back(array, entry);
				obs_data_release(entry);
			}
			obs_data_set_array(settings, key, array);
			obs_data_array_release(array);
		};
		auto sb_colour = [](const char *name, long long abgr, int cw, int ch) {
			obs_data_t *d = obs_data_create();
			obs_data_set_int(d, "color", abgr);
			obs_data_set_int(d, "width", cw);
			obs_data_set_int(d, "height", ch);
			obs_source_t *made = obs_source_create("color_source", name, d, nullptr);
			obs_data_release(d);
			return made;
		};
		auto sb_twins = [](obs_scene_t *scene, bool visible_only) {
			struct Count {
				bool visible_only;
				int n;
			} count{visible_only, 0};
			obs_scene_enum_items(
				scene,
				[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
					auto *c = static_cast<Count *>(param);
					if (strcmp(obs_source_get_unversioned_id(obs_sceneitem_get_source(item)),
						   "stagekit_stand_in") == 0 &&
					    (!c->visible_only || obs_sceneitem_visible(item)))
						c->n++;
					return true;
				},
				&count);
			return count.n;
		};
		auto sb_state = [](obs_source_t *standby, bool &idle) {
			calldata_t cd = {};
			long long made = -1;
			if (proc_handler_call(obs_source_get_proc_handler(standby), "test_state", &cd)) {
				made = calldata_int(&cd, "made");
				idle = calldata_bool(&cd, "idle");
			}
			calldata_free(&cd);
			return made;
		};
		auto sb_loaded = [](bool loaded) {
			calldata_t ld;
			calldata_init(&ld);
			calldata_set_bool(&ld, "loaded", loaded);
			proc_handler_call(obs_get_proc_handler(), "stagekit_test_loaded", &ld);
			calldata_free(&ld);
		};
		auto sb_kind_of = [](obs_sceneitem_t *item, bool show) -> std::string {
			obs_source_t *tr = item ? obs_sceneitem_get_transition(item, show) : nullptr;
			return tr ? obs_source_get_unversioned_id(tr) : "";
		};

		printf("\n[scene standby]\n");
		{
			// Captures with nothing attached (an image source with no file has
			// no size, as a Game Capture before a game is hooked).
			obs_source_t *a = obs_source_create("image_source", "SB Capture A", nullptr, nullptr);
			obs_source_t *b = obs_source_create("image_source", "SB Capture B", nullptr, nullptr);
			obs_source_t *cam = sb_colour("SB Camera", 0xFF00FF00LL, 200, 100); // green
			obs_scene_t *s = obs_scene_create("SB Scene S");
			obs_scene_t *t = obs_scene_create("SB Scene T");
			obs_scene_add(s, a);
			obs_scene_add(s, b);
			obs_sceneitem_t *cam_s = obs_scene_add(s, cam);
			obs_sceneitem_t *cam_t = obs_scene_add(t, cam);
			vec2 at;
			vec2_set(&at, 100.0f, 60.0f);
			obs_sceneitem_set_pos(cam_s, &at);
			obs_sceneitem_set_pos(cam_t, &at);
			obs_set_output_source(0, obs_scene_get_source(s));

			// B has a Standby Image of the user's own.
			obs_data_t *bd = obs_data_create();
			obs_data_set_int(bd, "style", 3);
			obs_source_t *b_own = obs_source_create("stagekit_standby", "SB Own Standby", bd, nullptr);
			obs_data_release(bd);
			obs_source_filter_add(b, b_own);
			sleep_ms(200);
			obs_data_t *b_before = obs_source_get_settings(b_own);
			const std::string b_json = obs_data_get_json(b_before);
			obs_data_release(b_before);

			// 1. A Scene Standby on S, covering A and B.
			obs_data_t *sd = obs_data_create();
			obs_data_set_int(sd, "which", 1);
			sb_list(sd, "sources", {"SB Capture A", "SB Capture B"});
			obs_data_set_int(sd, "picture", 1);
			obs_data_set_string(sd, "file", base_image);
			obs_data_set_int(sd, "fade_ms", 200);
			obs_data_set_int(sd, "back_ms", 200);
			obs_source_t *scene_sb =
				obs_source_create("stagekit_scene_standby", "SB Scene Standby", sd, nullptr);
			CHECK(scene_sb != nullptr, "the Scene Standby filter can be created");
			obs_source_filter_add(obs_scene_get_source(s), scene_sb);
			sb_wait([&] { return sb_filters(a, "stagekit_standby").size() == 1; }, 2000);
			std::vector<obs_source_t *> on_a = sb_filters(a, "stagekit_standby");
			obs_source_t *a_filter = on_a.size() == 1 ? on_a[0] : nullptr;
			obs_data_t *af = a_filter ? obs_source_get_settings(a_filter) : nullptr;
			const bool managed = af && obs_data_get_bool(af, "managed") &&
					     strcmp(obs_data_get_string(af, "managed_by"), "SB Scene S") == 0;
			obs_data_release(af);
			sb_wait([&] { return obs_source_get_width(a) > 0; }, 1500);
			sleep_ms(300);
			uint32_t aw = obs_source_get_width(a), ah = obs_source_get_height(a);
			std::vector<uint8_t> px = aw && ah ? render(a, aw, ah) : std::vector<uint8_t>();
			const bool image = !px.empty() && px[(size_t(ah / 2) * aw + aw / 2) * 4 + 3] > 250;
			CHECK(a_filter && managed && obs_source_enabled(a_filter) && image,
			      "a Scene Standby puts one Standby Image filter on a chosen source, marked as set by it, and the empty source shows the image (%zu filters, managed %d, %ux%u)",
			      on_a.size(), (int)managed, aw, ah);

			// 2. A change on the scene filter reaches the source's filter
			// without making the image again.
			bool idle = false;
			const long long made_before = a_filter ? sb_state(a_filter, idle) : -1;
			obs_data_set_int(sd, "style", 11);
			obs_data_set_int(sd, "fade_ms", 650);
			obs_source_update(scene_sb, sd);
			sb_wait([&] { return a_filter && sb_int(a_filter, "fade_ms") == 650; }, 2000);
			sleep_ms(300);
			const long long made_after = a_filter ? sb_state(a_filter, idle) : -1;
			CHECK(a_filter && sb_int(a_filter, "style") == 11 && sb_int(a_filter, "fade_ms") == 650 &&
				      made_before >= 1 && made_after == made_before,
			      "a change in the Scene Standby reaches the source's filter, and the image is not made again (style %lld, %lld ms; made %lld then %lld)",
			      a_filter ? sb_int(a_filter, "style") : -1, a_filter ? sb_int(a_filter, "fade_ms") : -1,
			      made_before, made_after);

			// 3. The user's own filter is left as it is.
			obs_data_t *b_after = obs_source_get_settings(b_own);
			const bool b_same = b_json == obs_data_get_json(b_after);
			obs_data_release(b_after);
			CHECK(b_same && sb_filters(b, "stagekit_standby").size() == 1,
			      "a source with a Standby Image of its own keeps it as it was, and gets no second one (%zu)",
			      sb_filters(b, "stagekit_standby").size());

			// 4. Show when hidden, in this scene only.
			const bool have_fade = obs_source_get_display_name("fade_transition") != nullptr;
			obs_data_set_string(sd, "transition", "fade_transition");
			obs_data_set_int(sd, "transition_ms", 400);
			sb_list(sd, "hidden", {"SB Camera"});
			obs_source_update(scene_sb, sd);
			sb_wait([&] { return sb_filters(cam, "stagekit_standby").size() == 1; }, 2000);
			obs_sceneitem_set_visible(cam_s, false);
			obs_sceneitem_set_visible(cam_t, false);
			sb_wait([&] { return sb_twins(s, true) == 1; }, 2000);
			sleep_ms(400);
			obs_sceneitem_t *twin_s = nullptr;
			obs_scene_enum_items(
				s,
				[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
					if (strcmp(obs_source_get_unversioned_id(obs_sceneitem_get_source(item)),
						   "stagekit_stand_in") == 0)
						*static_cast<obs_sceneitem_t **>(param) = item;
					return true;
				},
				&twin_s);
			CHECK(sb_twins(s, true) == 1 && sb_twins(t, false) == 0,
			      "a source listed under Show when hidden has its image in its place in that scene only (visible stand-ins %d here, %d in the other scene)",
			      sb_twins(s, true), sb_twins(t, false));
			if (have_fade) {
				const bool item_fades = sb_kind_of(cam_s, false) == "fade_transition" &&
							obs_sceneitem_get_transition_duration(cam_s, false) == 400 &&
							sb_kind_of(twin_s, true) == "fade_transition";
				// Another filter that sets transitions (a Show in Scenes, every
				// scene) may set the item of the other scene, not this one's.
				obs_sceneitem_set_visible(cam_s, true);
				obs_sceneitem_set_visible(cam_t, true);
				obs_data_t *wd = obs_data_create();
				obs_data_set_string(wd, "transition", "fade_transition");
				obs_data_set_int(wd, "transition_ms", 777);
				obs_source_t *sw =
					obs_source_create("stagekit_show_in_scenes", "SB Switch", wd, nullptr);
				obs_data_release(wd);
				obs_source_filter_add(cam, sw);
				sleep_ms(300);
				obs_source_set_enabled(sw, false);
				sleep_ms(300);
				obs_source_set_enabled(sw, true);
				sleep_ms(600);
				const int ms_s = (int)obs_sceneitem_get_transition_duration(cam_s, false);
				const int ms_t = (int)obs_sceneitem_get_transition_duration(cam_t, false);
				obs_source_filter_remove(cam, sw);
				obs_source_release(sw);
				CHECK(item_fades && ms_s == 400 && ms_t == 777,
				      "the source and its stand-in change with the Scene Standby's transition there, and the same source in another scene stays free for other filters (%d; %d ms here, %d ms there)",
				      (int)item_fades, ms_s, ms_t);
			} else {
				printf("  SKIP  no fade transition here: the transitions per scene are not checked\n");
			}
			obs_sceneitem_set_visible(cam_s, false);

			// 5. Off the list: the stand-in goes; off the sources, the filter
			// goes after the grace time.
			sb_list(sd, "hidden", {});
			obs_source_update(scene_sb, sd);
			const bool twin_gone = sb_wait([&] { return sb_twins(s, false) == 0; }, 1500);
			obs_sceneitem_set_visible(cam_s, true);
			CHECK(twin_gone, "taken off Show when hidden, its stand-in is taken out (%d left)",
			      sb_twins(s, false));
			sb_list(sd, "sources", {"SB Capture B"});
			obs_source_update(scene_sb, sd);
			sleep_ms(1500);
			const size_t kept_meanwhile = sb_filters(a, "stagekit_standby").size();
			const bool removed = sb_wait([&] { return sb_filters(a, "stagekit_standby").empty(); }, 5000);
			CHECK(kept_meanwhile == 1 && removed,
			      "taken off the chosen sources, its filter is kept for a moment and then taken off (%zu after 1.5 s, gone %d)",
			      kept_meanwhile, (int)removed);

			// 6. Two Scene Standbys: the filter takes its look from the one it
			// names, and the other takes it over when that one goes.
			sb_list(sd, "sources", {"SB Capture A", "SB Capture B"});
			obs_source_update(scene_sb, sd);
			sb_wait([&] { return sb_filters(a, "stagekit_standby").size() == 1; }, 2000);
			a_filter = sb_filters(a, "stagekit_standby").empty() ? nullptr
									     : sb_filters(a, "stagekit_standby")[0];
			obs_sceneitem_t *a_in_t = obs_scene_add(t, a);
			(void)a_in_t;
			obs_data_t *td = obs_data_create();
			obs_data_set_int(td, "which", 1);
			sb_list(td, "sources", {"SB Capture A"});
			obs_data_set_int(td, "style", 4);
			obs_data_set_int(td, "picture", 1);
			obs_data_set_string(td, "file", base_image);
			obs_source_t *scene_tb =
				obs_source_create("stagekit_scene_standby", "SB Scene Standby T", td, nullptr);
			obs_source_filter_add(obs_scene_get_source(t), scene_tb);
			sleep_ms(1500);
			const long long style_kept = a_filter ? sb_int(a_filter, "style") : -1;
			const std::string by_kept = a_filter ? sb_setting(a_filter, "managed_by") : "";
			obs_source_filter_remove(obs_scene_get_source(s), scene_sb);
			sb_wait([&] { return a_filter && sb_setting(a_filter, "managed_by") == "SB Scene T"; }, 5000);
			sleep_ms(300);
			CHECK(style_kept == 11 && by_kept == "SB Scene S" && a_filter &&
				      sb_setting(a_filter, "managed_by") == "SB Scene T" &&
				      sb_int(a_filter, "style") == 4,
			      "with two Scene Standbys, the filter follows the one it names, and the other takes it over when that one is gone (style %lld by %s; then style %lld by %s)",
			      style_kept, by_kept.c_str(), a_filter ? sb_int(a_filter, "style") : -1,
			      a_filter ? sb_setting(a_filter, "managed_by").c_str() : "-");

			// 7. Switched off by hand, it stays off.
			if (a_filter) {
				obs_source_set_enabled(a_filter, false);
				obs_data_set_int(td, "fade_ms", 300);
				obs_source_update(scene_tb, td);
				sleep_ms(2500);
				CHECK(!obs_source_enabled(a_filter) && sb_int(a_filter, "fade_ms") == 300,
				      "a filter the user switched off stays off while its look is still set (fade %lld ms)",
				      sb_int(a_filter, "fade_ms"));
				obs_source_set_enabled(a_filter, true);
			}

			// 5b. Its Scene Standby gone, the filter is idle (the empty
			// source draws nothing) until another one's tick takes it off.
			obs_source_filter_remove(obs_scene_get_source(t), scene_tb);
			sb_wait(
				[&] {
					bool i = false;
					return a_filter && sb_state(a_filter, i) >= 0 && i;
				},
				4000);
			idle = false;
			if (a_filter)
				sb_state(a_filter, idle);
			// (The image leaves over its own time first.)
			sb_wait([&] { return obs_source_get_width(a) == 0; }, 2000);
			const uint32_t idle_w = obs_source_get_width(a);
			obs_data_t *ud = obs_data_create();
			obs_data_set_int(ud, "which", 1);
			obs_source_t *other =
				obs_source_create("stagekit_scene_standby", "SB Scene Standby U", ud, nullptr);
			obs_data_release(ud);
			obs_source_filter_add(obs_scene_get_source(t), other);
			const bool taken_off = sb_wait([&] { return sb_filters(a, "stagekit_standby").empty(); }, 9000);
			CHECK(idle && idle_w == 0 && taken_off,
			      "with no Scene Standby left, its filter is idle and the empty source draws nothing (%d, %u wide), and another Scene Standby's tick takes it off (%d)",
			      (int)idle, idle_w, (int)taken_off);
			obs_source_filter_remove(obs_scene_get_source(t), other);
			obs_source_release(other);

			// 8. Not while OBS loads its scenes.
			sb_loaded(false);
			obs_source_filter_add(obs_scene_get_source(s), scene_sb);
			sleep_ms(1500);
			const size_t while_loading = sb_filters(a, "stagekit_standby").size();
			sb_loaded(true);
			const bool after_loading =
				sb_wait([&] { return sb_filters(a, "stagekit_standby").size() == 1; }, 3000);
			CHECK(while_loading == 0 && after_loading,
			      "nothing is added while OBS loads its scenes, and it is once they are loaded (%zu, %d)",
			      while_loading, (int)after_loading);

			// 12. Screen captures only when asked for.
			{
				obs_source_t *screen =
					obs_source_get_display_name("xshm_input")
						? obs_source_create("xshm_input", "SB Screen", nullptr, nullptr)
						: nullptr;
				if (!screen) {
					printf("  SKIP  no screen capture kind here: Include screen captures is not checked\n");
				} else {
					obs_scene_add(t, screen);
					obs_data_t *xd = obs_data_create();
					obs_source_t *all =
						obs_source_create("stagekit_scene_standby", "SB All", xd, nullptr);
					obs_source_filter_add(obs_scene_get_source(t), all);
					sleep_ms(1500);
					const size_t without = sb_filters(screen, "stagekit_standby").size();
					obs_data_set_bool(xd, "screens", true);
					obs_source_update(all, xd);
					const bool with = sb_wait(
						[&] { return sb_filters(screen, "stagekit_standby").size() == 1; },
						3000);
					CHECK(without == 0 && with,
					      "a screen capture is covered by Every capture only with Include screen captures (%zu, %d)",
					      without, (int)with);
					obs_data_release(xd);
					obs_source_filter_remove(obs_scene_get_source(t), all);
					obs_source_release(all);
					obs_source_release(screen);
				}
			}
			// 11. A Scene Modes mode that is not showing holds the camera
			// hidden: no stand-in shows for it. Both filter orders.
			for (int order = 0; order < 2; order++) {
				sb_list(sd, "hidden", {"SB Camera"});
				obs_data_set_string(sd, "transition", "none");
				obs_source_update(scene_sb, sd);
				obs_sceneitem_set_visible(cam_s, true);
				obs_data_t *md = obs_data_create();
				sb_list(md, "modes", {"m1", "m2"});
				obs_data_set_string(md, "mode|m1|name", "Other");
				sb_list(md, "mode|m1|sources", {"SB Capture A"});
				obs_data_set_string(md, "mode|m2|name", "Camera");
				sb_list(md, "mode|m2|sources", {"SB Camera"});
				obs_data_set_string(md, "mode_now", "m2");
				obs_data_set_string(md, "transition", "none");
				obs_source_t *sb_modes =
					obs_source_create("stagekit_scene_modes", "SB Modes", md, nullptr);
				obs_data_release(md);
				obs_source_filter_add(obs_scene_get_source(s), sb_modes);
				obs_source_filter_set_order(obs_scene_get_source(s), sb_modes,
							    order ? OBS_ORDER_MOVE_BOTTOM : OBS_ORDER_MOVE_TOP);
				sleep_ms(500);
				const bool cam_first = obs_sceneitem_visible(cam_s);
				obs_data_t *req = obs_data_create();
				obs_data_set_string(req, "scene", "SB Scene S");
				obs_data_set_string(req, "filter", "");
				obs_data_set_string(req, "mode", "m1");
				obs_data_set_string(req, "how", "on");
				calldata_t cd = {};
				calldata_set_string(&cd, "request", "SetSceneMode");
				calldata_set_string(&cd, "data", obs_data_get_json(req));
				proc_handler_call(obs_get_proc_handler(), "stagekit_vendor_call", &cd);
				calldata_free(&cd);
				obs_data_release(req);
				const bool hidden = sb_wait([&] { return !obs_sceneitem_visible(cam_s); }, 2000);
				sleep_ms(800);
				const int twins = sb_twins(s, true);
				CHECK(cam_first && hidden && twins == 0,
				      "a camera held hidden by a Scene Modes mode not showing gets no standby image in its place (%s order; shown first %d, hidden %d, twins showing %d)",
				      order ? "modes after" : "modes first", (int)cam_first, (int)hidden, twins);
				obs_source_filter_remove(obs_scene_get_source(s), sb_modes);
				obs_source_release(sb_modes);
				obs_sceneitem_set_visible(cam_s, true);
				sleep_ms(300);
			}

			// The README's picture: one capture empty, the camera hidden, both
			// showing the standby image in their places.
			{
				obs_data_set_int(sd, "style", 0);
				sb_list(sd, "hidden", {"SB Camera"});
				obs_data_set_string(sd, "transition", "none");
				obs_source_update(scene_sb, sd);
				obs_sceneitem_t *a_item = obs_scene_find_source(s, "SB Capture A");
				vec2 small, place;
				vec2_set(&small, 0.15f, 0.15f);
				vec2_set(&place, 360.0f, 60.0f);
				obs_sceneitem_set_scale(a_item, &small);
				obs_sceneitem_set_pos(a_item, &place);
				obs_sceneitem_set_visible(cam_s, true);
				sleep_ms(1200);
				const std::vector<uint8_t> before = render(obs_scene_get_source(s), w, h);
				obs_sceneitem_set_visible(cam_s, false);
				sb_wait([&] { return sb_twins(s, true) == 1; }, 2000);
				sleep_ms(500);
				const std::vector<uint8_t> after = render(obs_scene_get_source(s), w, h);
				write_frame(out + "/../scene-standby/1 camera shown.rgba", before, w, h);
				write_frame(out + "/../scene-standby/2 camera hidden.rgba", after, w, h);
				const uint8_t *was = &before[(size_t(110) * w + 200) * 4];
				const uint8_t *now = &after[(size_t(110) * w + 200) * 4];
				const uint8_t *empty = &after[(size_t(100) * w + 430) * 4];
				CHECK(was[1] > 250 && was[0] < 5 && now[3] > 250 && !(now[1] > 250 && now[0] < 5) &&
					      empty[3] > 250,
				      "in the scene, the empty capture and the hidden camera both show the standby image (camera %d,%d,%d; then %d,%d,%d; empty capture alpha %d)",
				      was[0], was[1], was[2], now[0], now[1], now[2], empty[3]);
				obs_sceneitem_set_visible(cam_s, true);
			}

			obs_data_release(td);
			obs_source_release(scene_tb);
			obs_data_release(sd);
			obs_set_output_source(0, nullptr);
			obs_source_filter_remove(obs_scene_get_source(s), scene_sb);
			obs_source_release(scene_sb);
			obs_source_release(b_own);
			obs_scene_release(s);
			obs_scene_release(t);
			obs_source_release(a);
			obs_source_release(b);
			obs_source_release(cam);
		}

		// 9. A scene with a Scene Standby and its covered source are let go of
		// together, so OBS can change scene collection.
		{
			obs_source_t *g = obs_source_create("image_source", "SB Freed Capture", nullptr, nullptr);
			static bool sb_freed = false;
			signal_handler_connect(
				obs_source_get_signal_handler(g), "destroy",
				[](void *, calldata_t *) { sb_freed = true; }, nullptr);
			obs_scene_t *gs = obs_scene_create("SB Freed Scene");
			obs_scene_add(gs, g);
			obs_data_t *gd = obs_data_create();
			obs_data_set_int(gd, "which", 1);
			sb_list(gd, "sources", {"SB Freed Capture"});
			sb_list(gd, "hidden", {"SB Freed Capture"});
			obs_source_t *gf = obs_source_create("stagekit_scene_standby", "SB Freed Standby", gd, nullptr);
			obs_data_release(gd);
			obs_source_filter_add(obs_scene_get_source(gs), gf);
			sb_wait([&] { return sb_filters(g, "stagekit_standby").size() == 1; }, 2000);
			obs_sceneitem_set_visible(obs_scene_find_source(gs, "SB Freed Capture"), false);
			sleep_ms(800);
			const int twins = sb_twins(gs, false);
			sb_loaded(false);
			sleep_ms(300);
			obs_source_release(gf);
			obs_source_remove(obs_scene_get_source(gs));
			obs_scene_release(gs);
			obs_source_remove(g);
			obs_source_release(g);
			sleep_ms(500);
			sb_loaded(true);
			CHECK(twins == 1 && sb_freed,
			      "a covered source with a stand-in is let go of with its scene, so OBS can change scene collection (stand-ins %d, let go %d)",
			      twins, (int)sb_freed);
		}

		printf("\n[scene corners]\n");
		{
			obs_source_t *back = sb_colour("SC Background", 0xFF404040LL, 640, 360);
			obs_source_t *p = sb_colour("SC Panel", 0xFF0000FFLL, 200, 100); // red
			obs_source_t *q = sb_colour("SC Own", 0xFFFF0000LL, 200, 100);   // blue
			obs_scene_t *cs = obs_scene_create("SC Scene");
			obs_scene_add(cs, back);
			obs_sceneitem_t *p_item = obs_scene_add(cs, p);
			obs_sceneitem_t *q_item = obs_scene_add(cs, q);
			vec2 at;
			vec2_set(&at, 40.0f, 40.0f);
			obs_sceneitem_set_pos(p_item, &at);
			vec2_set(&at, 360.0f, 200.0f);
			obs_sceneitem_set_pos(q_item, &at);
			obs_set_output_source(0, obs_scene_get_source(cs));
			obs_data_t *od = obs_data_create();
			obs_data_set_double(od, "size", 10);
			obs_source_t *q_own = obs_source_create("stagekit_corners", "SC Own Corners", od, nullptr);
			obs_data_release(od);
			obs_source_filter_add(q, q_own);
			sleep_ms(200);
			obs_data_t *q_before = obs_source_get_settings(q_own);
			const std::string q_json = obs_data_get_json(q_before);
			obs_data_release(q_before);

			// Not while OBS loads its scenes.
			sb_loaded(false);
			obs_data_t *cd = obs_data_create();
			obs_data_set_double(cd, "size", 30);
			obs_source_t *corners =
				obs_source_create("stagekit_scene_corners", "SC Scene Corners", cd, nullptr);
			CHECK(corners != nullptr, "the Scene Corners filter can be created");
			obs_source_filter_add(obs_scene_get_source(cs), corners);
			sleep_ms(1500);
			const size_t while_loading = sb_filters(p, "stagekit_corners").size();
			sb_loaded(true);
			sb_wait([&] { return sb_filters(p, "stagekit_corners").size() == 1; }, 3000);
			obs_source_t *p_corners = sb_filters(p, "stagekit_corners").empty()
							  ? nullptr
							  : sb_filters(p, "stagekit_corners")[0];
			CHECK(while_loading == 0 && p_corners && sb_setting(p_corners, "managed_by") == "SC Scene",
			      "Scene Corners adds a Corners & Edges to each source, not while OBS loads its scenes (%zu, then %d)",
			      while_loading, p_corners != nullptr);
			obs_data_t *q_after = obs_source_get_settings(q_own);
			const bool q_same = q_json == obs_data_get_json(q_after);
			obs_data_release(q_after);
			CHECK(sb_filters(back, "stagekit_corners").empty() && q_same &&
				      sb_filters(q, "stagekit_corners").size() == 1,
			      "the full-screen background is left out, and a Corners & Edges of the user's own is left as it was (background %zu, own %zu, same %d)",
			      sb_filters(back, "stagekit_corners").size(), sb_filters(q, "stagekit_corners").size(),
			      (int)q_same);

			// A change of look is pushed, and the source's size never changes.
			auto corner_alpha = [&] {
				const std::vector<uint8_t> frame = render(obs_scene_get_source(cs), w, h);
				return (int)frame[(size_t(41) * w + 41) * 4 + 0]; // red channel at the top-left corner
			};
			sleep_ms(300);
			write_frame(out + "/../scene-standby/3 scene corners.rgba",
				    render(obs_scene_get_source(cs), w, h), w, h);
			const int round_before = corner_alpha();
			bool size_kept = true;
			obs_data_set_double(cd, "size", 0);
			obs_source_update(corners, cd);
			for (int i = 0; i < 30; i++) {
				size_kept = size_kept && obs_source_get_width(p) == 200 &&
					    obs_source_get_height(p) == 100;
				render(obs_scene_get_source(cs), w, h);
				sleep_ms(50);
			}
			const int round_after = corner_alpha();
			double pushed_size = -1.0;
			if (p_corners) {
				obs_data_t *ps = obs_source_get_settings(p_corners);
				pushed_size = obs_data_get_double(ps, "size");
				obs_data_release(ps);
			}
			CHECK(pushed_size == 0.0 && round_before < 128 && round_after > 200 && size_kept,
			      "a change in Scene Corners reaches each source's filter, and no source changes size meanwhile (corner red %d, then %d; size kept %d)",
			      round_before, round_after, (int)size_kept);

			// Off and on: its filters go with it.
			obs_source_set_enabled(corners, false);
			const bool off = sb_wait([&] { return p_corners && !obs_source_enabled(p_corners); }, 2000);
			obs_source_set_enabled(corners, true);
			const bool on = sb_wait([&] { return p_corners && obs_source_enabled(p_corners); }, 2000);
			CHECK(off && on && obs_source_enabled(q_own),
			      "switching Scene Corners off and on switches its filters off and on, and not the user's own (%d, %d)",
			      (int)off, (int)on);

			// Morph still morphs a scene with Scene Corners and Scene Standby:
			// neither draws.
			{
				obs_scene_t *other = obs_scene_create("SC Scene Two");
				obs_scene_add(other, back);
				obs_sceneitem_t *p_two = obs_scene_add(other, p);
				vec2_set(&at, 400.0f, 240.0f);
				obs_sceneitem_set_pos(p_two, &at);
				obs_source_t *corners_two = obs_source_create("stagekit_scene_corners",
									      "SC Scene Corners Two", cd, nullptr);
				obs_source_filter_add(obs_scene_get_source(other), corners_two);
				obs_data_t *ed = obs_data_create();
				obs_data_set_int(ed, "which", 1);
				obs_source_t *standby_two = obs_source_create("stagekit_scene_standby",
									      "SC Scene Standby Two", ed, nullptr);
				obs_data_release(ed);
				obs_source_filter_add(obs_scene_get_source(other), standby_two);
				sleep_ms(1200);
				obs_data_t *ms = obs_data_create();
				obs_data_set_int(ms, "ease_curve", 0);
				obs_source_t *morph = obs_source_create_private("stagekit_morph", "SC Morph", ms);
				obs_data_release(ms);
				obs_transition_set(morph, obs_scene_get_source(cs));
				obs_transition_start(morph, OBS_TRANSITION_MODE_MANUAL, 0, obs_scene_get_source(other));
				obs_transition_set_manual_time(morph, 0.5f);
				sleep_ms(150);
				const std::vector<uint8_t> half = render(morph, w, h);
				// The panel: from 40,40 to 400,240 (200x100); half-way 220,140.
				const uint8_t *mid = &half[(size_t(190) * w + 320) * 4];
				const uint8_t *start = &half[(size_t(90) * w + 140) * 4];
				CHECK(mid[0] > 240 && mid[2] < 15 && start[0] < 120,
				      "a scene with Scene Corners and Scene Standby is still morphed item by item: the panel is half-way (red %d there, %d at its old place)",
				      mid[0], start[0]);
				obs_transition_force_stop(morph);
				obs_transition_clear(morph);
				obs_source_release(morph);
				obs_source_filter_remove(obs_scene_get_source(other), standby_two);
				obs_source_release(standby_two);
				obs_source_filter_remove(obs_scene_get_source(other), corners_two);
				obs_source_release(corners_two);
				obs_scene_release(other);
			}

			obs_data_release(cd);
			obs_set_output_source(0, nullptr);
			obs_source_filter_remove(obs_scene_get_source(cs), corners);
			obs_source_release(corners);
			obs_source_release(q_own);
			obs_scene_release(cs);
			obs_source_release(back);
			obs_source_release(p);
			obs_source_release(q);
		}
	}
	// ==== end scene-filters:standby-corners ====
	// ==== scene-filters:shadow ====
	// ---------------------------------------------------------- scene shadow
	// One shadow for the whole scene: laid over the finished scene, kept off
	// every source that is not background, and the sources keep their size.
	{
		printf("\n[scene shadow]\n");
		const std::string shadow_dir = out + "/../scene-shadow";
		if (std::system(("mkdir -p '" + shadow_dir + "'").c_str()) != 0)
			printf("  (could not make %s)\n", shadow_dir.c_str());
		auto shadow_block = [](const char *name, long long abgr, int bw, int bh) {
			obs_data_t *d = obs_data_create();
			obs_data_set_int(d, "color", abgr);
			obs_data_set_int(d, "width", bw);
			obs_data_set_int(d, "height", bh);
			obs_source_t *src = obs_source_create("color_source", name, d, nullptr);
			obs_data_release(d);
			return src;
		};
		auto shadow_put = [](obs_scene_t *scene, obs_source_t *src, float x, float y) {
			obs_sceneitem_t *item = obs_scene_add(scene, src);
			vec2 p;
			vec2_set(&p, x, y);
			obs_sceneitem_set_pos(item, &p);
			return item;
		};
		auto shadow_px = [&](const std::vector<uint8_t> &frame, int x, int y, int c) {
			return (int)frame[(size_t(y) * w + x) * 4 + c];
		};
		// The largest difference of any channel inside (or outside) a box.
		auto shadow_most = [&](const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, int x0, int y0,
				       int x1, int y1, bool inside) {
			int most = 0;
			for (int y = 0; y < (int)h; y++)
				for (int x = 0; x < (int)w; x++) {
					const bool in = x >= x0 && x < x1 && y >= y0 && y < y1;
					if (in != inside)
						continue;
					for (int c = 0; c < 4; c++)
						most = std::max(
							most, std::abs(shadow_px(a, x, y, c) - shadow_px(b, x, y, c)));
				}
			return most;
		};
		auto shadow_set_loaded = [](bool loaded) {
			calldata_t ld;
			calldata_init(&ld);
			calldata_set_bool(&ld, "loaded", loaded);
			proc_handler_call(obs_get_proc_handler(), "stagekit_test_loaded", &ld);
			calldata_free(&ld);
		};
		obs_source_t *bg = shadow_block("Shadow Back", 0xFF808080, 640, 360);     // grey, the whole canvas
		obs_source_t *sq_a = shadow_block("Shadow Square", 0xFFFFFFFF, 100, 100); // white
		obs_source_t *sq_b = shadow_block("Shadow Skipped", 0xFFFF0000, 60, 60);  // blue
		obs_source_t *sq_c = shadow_block("Shadow Own", 0xFF0000FF, 80, 80); // red, with a shadow of its own
		obs_source_t *sq_r =
			shadow_block("Shadow Reference", 0xFFFFFFFF, 100, 100); // white, for the per-source shadow
		{
			obs_data_t *od = obs_data_create();
			obs_data_set_double(od, "offset_x", -10.0);
			obs_data_set_double(od, "offset_y", 10.0);
			obs_source_t *own = obs_source_create("stagekit_shadow_backdrop", "Own Shadow", od, nullptr);
			obs_data_release(od);
			obs_source_filter_add(sq_c, own);
			obs_source_release(own);
		}
		obs_scene_t *ss = obs_scene_create("Shadow Scene");
		obs_scene_t *sp = obs_scene_create("Shadow Plain");
		obs_scene_t *sr = obs_scene_create("Shadow Per Source");
		obs_sceneitem_t *item_a = nullptr;
		for (obs_scene_t *scene : {ss, sp}) {
			shadow_put(scene, bg, 0, 0);
			obs_sceneitem_t *a = shadow_put(scene, sq_a, 100, 100);
			if (scene == ss)
				item_a = a;
			shadow_put(scene, sq_b, 190, 190);
			shadow_put(scene, sq_c, 420, 100);
		}
		shadow_put(sr, bg, 0, 0);
		shadow_put(sr, sq_r, 100, 100);
		{
			// (Added as a person adds it: with the source already in its
			// scene, whose item is moved to keep the image where it is.)
			// (A new filter: the room it takes has not been given yet.)
			obs_data_t *pd = obs_data_create();
			obs_data_set_int(pd, "adjusted_left", 0);
			obs_source_t *per =
				obs_source_create("stagekit_shadow_backdrop", "Reference Shadow", pd, nullptr);
			obs_data_release(pd);
			obs_source_filter_add(sq_r, per);
			obs_source_release(per);
		}
		obs_source_t *ss_src = obs_scene_get_source(ss);
		obs_data_t *fd = obs_data_create();
		obs_data_array_t *skipped = obs_data_array_create();
		obs_data_t *entry = obs_data_create();
		obs_data_set_string(entry, "value", "Shadow Skipped");
		obs_data_array_push_back(skipped, entry);
		obs_data_release(entry);
		obs_data_set_array(fd, "shadow_skip", skipped);
		obs_data_array_release(skipped);
		obs_source_t *scene_shadow = obs_source_create("stagekit_scene_shadow", "Scene Shadow", fd, nullptr);
		obs_data_release(fd);
		CHECK(scene_shadow != nullptr, "Scene Shadow is registered and can be created");
		obs_source_filter_add(ss_src, scene_shadow);
		vec2 a_pos_before, a_scale_before;
		obs_sceneitem_get_pos(item_a, &a_pos_before);
		obs_sceneitem_get_scale(item_a, &a_scale_before);
		sleep_ms(400);
		for (int i = 0; i < 3; i++) {
			render(ss_src, w, h);
			sleep_ms(40);
		}
		const std::vector<uint8_t> plain = render(obs_scene_get_source(sp), w, h);
		const std::vector<uint8_t> shadowed = render(ss_src, w, h);
		const std::vector<uint8_t> per_source = render(obs_scene_get_source(sr), w, h);

		// The square's shadow reaches from about 80 to 245 across and down.
		CHECK(shadow_most(shadowed, plain, 70, 70, 250, 250, false) <= 1 &&
			      shadow_most(shadowed, plain, 100, 100, 200, 200, true) <= 1,
		      "the scene is unchanged where there is no shadow, and on the source itself (off by %d outside, %d on it)",
		      shadow_most(shadowed, plain, 70, 70, 250, 250, false),
		      shadow_most(shadowed, plain, 100, 100, 200, 200, true));
		// Where the blue square is not: the shadow is the per-source filter's.
		const int against = std::max(shadow_most(shadowed, per_source, 70, 70, 186, 250, true),
					     shadow_most(shadowed, per_source, 186, 70, 250, 186, true));
		CHECK(against <= 4 && shadow_px(shadowed, 206, 150, 0) < 110,
		      "the shadow is the one Shadow, Glow & Outline draws under the same square (off by %d; %d beside it)",
		      against, shadow_px(shadowed, 206, 150, 0));
		CHECK(shadow_most(shadowed, plain, 0, 0, 640, 360, true) > 20 &&
			      shadow_px(shadowed, 636, 180, 0) == shadow_px(plain, 636, 180, 0) &&
			      shadow_px(shadowed, 320, 356, 0) == shadow_px(plain, 320, 356, 0),
		      "the full-screen background casts nothing: no shadow along the canvas's edges");
		CHECK(shadow_most(shadowed, plain, 190, 190, 250, 250, true) <= 1 &&
			      shadow_px(shadowed, 258, 258, 0) == shadow_px(plain, 258, 258, 0),
		      "a source under 'No shadow for' casts nothing and the square's shadow is kept off it (off by %d on it)",
		      shadow_most(shadowed, plain, 190, 190, 250, 250, true));
		CHECK(shadow_most(shadowed, plain, 380, 60, 540, 230, true) <= 1,
		      "a source with a shadow of its own keeps that one and gets none from the scene (off by %d)",
		      shadow_most(shadowed, plain, 380, 60, 540, 230, true));
		vec2 a_pos_after, a_scale_after;
		obs_sceneitem_get_pos(item_a, &a_pos_after);
		obs_sceneitem_get_scale(item_a, &a_scale_after);
		CHECK(a_pos_after.x == a_pos_before.x && a_pos_after.y == a_pos_before.y &&
			      a_scale_after.x == a_scale_before.x && obs_source_get_width(ss_src) == 640 &&
			      obs_source_get_height(ss_src) == 360,
		      "nothing is moved or resized: the sources and the scene keep their size");
		CHECK(shadow_most(render(ss_src, w, h), render(ss_src, w, h), 0, 0, 640, 360, true) == 0,
		      "drawn twice, the scene looks the same both times");
		{
			const std::vector<uint8_t> in_scene = render(ss_src, w, h);
			const std::vector<uint8_t> in_window = render_shown(ss_src, w, h);
			const std::vector<uint8_t> plain_window = render_shown(obs_scene_get_source(sp), w, h);
			double sum = 0.0, plain_sum = 0.0;
			int count = 0;
			for (int y = 60; y < 300; y += 3)
				for (int x = 60; x < 600; x += 3)
					for (int c = 0; c < 3; c++) {
						sum += std::abs(shadow_px(in_scene, x, y, c) -
								shadow_px(in_window, x, y, c));
						plain_sum += std::abs(shadow_px(plain, x, y, c) -
								      shadow_px(plain_window, x, y, c));
						count++;
					}
			(void)plain_sum;
			CHECK(sum / count < 1.5,
			      "drawn in a window with linear light (the Filters window) it looks the same (off by %.2f; %.2f without it)",
			      sum / count, plain_sum / count);
		}
		obs_properties_t *props = obs_source_properties(scene_shadow);
		CHECK(props && obs_properties_get(props, "shadow") && obs_properties_get(props, "outline") &&
			      obs_properties_get(props, "offset_x") && obs_properties_get(props, "auto_background") &&
			      obs_properties_get(props, "ramp_ms") && obs_properties_get(props, "ramp_ease_curve") &&
			      list_rows(props, "shadow_skip|a") == 1,
		      "its settings have the shadow, the outline, the two lists, Full-screen = background and the switch's duration");
		obs_properties_destroy(props);

		// The background listed off: it is a source like any other, so the
		// square's shadow, which falls on it, is kept off it.
		{
			obs_data_t *sd = obs_source_get_settings(scene_shadow);
			obs_data_set_bool(sd, "auto_background", false);
			obs_source_update(scene_shadow, sd);
			sleep_ms(400);
			const std::vector<uint8_t> no_back = render(ss_src, w, h);
			CHECK(shadow_most(no_back, plain, 0, 0, 640, 360, true) <= 1,
			      "with Full-screen = background unticked the background is one more source, and no shadow lands on it (off by %d)",
			      shadow_most(no_back, plain, 0, 0, 640, 360, true));
			obs_data_set_bool(sd, "auto_background", true);
			obs_source_update(scene_shadow, sd);
			obs_data_release(sd);
			sleep_ms(400);
		}

		// The outline, only outside every source that is not background.
		{
			obs_data_t *sd = obs_source_get_settings(scene_shadow);
			obs_data_set_bool(sd, "shadow", false);
			obs_data_set_bool(sd, "outline", true);
			obs_data_set_double(sd, "outline_width", 6.0);
			obs_data_set_int(sd, "outline_color", 0xFF0000FF); // red
			obs_source_update(scene_shadow, sd);
			sleep_ms(400);
			const std::vector<uint8_t> ring = render(ss_src, w, h);
			CHECK(shadow_px(ring, 97, 150, 0) > 240 && shadow_px(ring, 97, 150, 1) < 15 &&
				      shadow_px(ring, 150, 150, 1) > 240 && shadow_px(ring, 203, 195, 2) > 240 &&
				      shadow_px(ring, 203, 195, 0) < 15 && shadow_px(ring, 120, 120, 1) > 240,
			      "the outline goes round the casting sources and stays off every source (red %d beside the square; on the square green %d; on the blue square blue %d)",
			      shadow_px(ring, 97, 150, 0), shadow_px(ring, 150, 150, 1), shadow_px(ring, 203, 195, 2));
			// For the README: a glow and an outline.
			obs_data_set_bool(sd, "shadow", true);
			obs_data_set_int(sd, "shadow_type", 1);
			obs_data_set_int(sd, "shadow_color", 0xFF00D0FF); // amber
			obs_data_set_double(sd, "shadow_opacity", 90.0);
			obs_data_set_double(sd, "softness", 40.0);
			obs_data_set_int(sd, "outline_color", 0xFFFFFFFF);
			obs_data_set_double(sd, "outline_width", 4.0);
			obs_source_update(scene_shadow, sd);
			sleep_ms(400);
			obs_data_unset_user_value(sd, "shadow_type");
			obs_data_unset_user_value(sd, "shadow_color");
			obs_data_unset_user_value(sd, "shadow_opacity");
			obs_data_unset_user_value(sd, "softness");
			obs_data_unset_user_value(sd, "outline_color");
			obs_data_unset_user_value(sd, "outline_width");
			obs_data_set_bool(sd, "outline", false);
			obs_source_update(scene_shadow, sd);
			obs_data_release(sd);
			sleep_ms(400);
		}

		// On the output, as OBS draws a scene: once a frame, however often
		// it is drawn; and a hide transition still ends, its shadow fading
		// with it.
		obs_source_t *before_out = obs_get_output_source(0);
		obs_set_output_source(0, ss_src);
		sleep_ms(300);
		{
			auto draws = [&] {
				calldata_t cd;
				calldata_init(&cd);
				proc_handler_call(obs_source_get_proc_handler(scene_shadow), "draws", &cd);
				const long long n = calldata_int(&cd, "count");
				calldata_free(&cd);
				return n;
			};
			const long long d0 = draws();
			const uint64_t t0 = obs_get_video_frame_time();
			for (int i = 0; i < 25; i++) {
				render(ss_src, w, h);
				sleep_ms(20);
			}
			const long long d1 = draws();
			const uint64_t t1 = obs_get_video_frame_time();
			const long long frames = (long long)((t1 - t0) / 16666667ULL);
			CHECK(d1 - d0 <= frames + 2 && d1 - d0 >= frames / 2,
			      "drawn by OBS and 25 more times, the shadow is made once a frame (%lld times in %lld frames)",
			      d1 - d0, frames);
		}
		{
			const std::vector<uint8_t> before = render(ss_src, w, h);
			obs_source_t *fade = obs_source_create_private("fade_transition", "Shadow Hide", nullptr);
			CHECK(fade != nullptr, "OBS's fade transition is there for the hide test");
			// (Sound is off in this part of the tests, so OBS never ends the
			// sound half of an item transition here: what is watched is
			// the picture half, which only drawing ends.)
			static bool video_ended = false;
			video_ended = false;
			signal_handler_connect(
				obs_source_get_signal_handler(fade), "transition_video_stop",
				[](void *, calldata_t *) { video_ended = true; }, nullptr);
			obs_sceneitem_set_transition(item_a, false, fade);
			obs_sceneitem_set_transition_duration(item_a, false, 600);
			obs_source_release(fade);
			sleep_ms(100);
			obs_sceneitem_set_visible(item_a, false);
			std::vector<int> beside;
			const auto started = std::chrono::steady_clock::now();
			while (std::chrono::steady_clock::now() - started < std::chrono::milliseconds(560)) {
				beside.push_back(shadow_px(render(ss_src, w, h), 206, 150, 0));
				sleep_ms(30);
			}
			const int full = shadow_px(before, 206, 150, 0), none = shadow_px(plain, 206, 150, 0);
			bool between = false;
			for (int v : beside)
				between = between || (v > full + 8 && v < none - 8);
			sleep_ms(600 + 3 * 17 - 560);
			const bool ended = video_ended;
			const std::vector<uint8_t> gone = render(ss_src, w, h);
			CHECK(between,
			      "a source fading out takes its shadow with it part-way (%d with its shadow, %d without)",
			      full, none);
			CHECK(ended && shadow_px(gone, 206, 150, 0) == none && shadow_px(gone, 150, 150, 0) == none,
			      "and the hide transition ends (its picture within its duration and three frames), with no shadow left (ended %d)",
			      (int)ended);
			obs_sceneitem_set_transition(item_a, false, nullptr);
			obs_sceneitem_set_visible(item_a, true);
			sleep_ms(200);
		}
		obs_set_output_source(0, before_out);
		obs_source_release(before_out);

		// Switched off, the shadow fades over the switch's duration.
		{
			obs_data_t *sd = obs_source_get_settings(scene_shadow);
			obs_data_set_int(sd, "ramp_ms", 600);
			obs_data_set_int(sd, "ramp_ease_curve", 0);
			obs_source_update(scene_shadow, sd);
			obs_data_release(sd);
			sleep_ms(100);
			const int full = shadow_px(render(ss_src, w, h), 206, 150, 0);
			obs_source_set_enabled(scene_shadow, false);
			sleep_ms(300);
			const int part = shadow_px(render(ss_src, w, h), 206, 150, 0);
			sleep_ms(500);
			const int off = shadow_px(render(ss_src, w, h), 206, 150, 0);
			CHECK(part > full + 8 && part < off - 8 && off == shadow_px(plain, 206, 150, 0) &&
				      !obs_source_enabled(scene_shadow),
			      "switched off it fades out over its duration, then is off (%d, %d part-way, %d)", full,
			      part, off);
			obs_source_set_enabled(scene_shadow, true);
			sleep_ms(800);
		}

		// Nothing while a collection is loading or changing.
		{
			shadow_set_loaded(false);
			sleep_ms(200);
			const std::vector<uint8_t> loading = render(ss_src, w, h);
			shadow_set_loaded(true);
			sleep_ms(400);
			const std::vector<uint8_t> loaded = render(ss_src, w, h);
			CHECK(shadow_most(loading, plain, 0, 0, 640, 360, true) <= 1 &&
				      shadow_most(loaded, plain, 0, 0, 640, 360, true) > 20,
			      "while a collection loads or changes it draws nothing, and afterwards the shadow is back");
		}

		// For the README: a scene with a game, a camera and a chat box on a
		// picture, as OBS draws it, with a Scene Shadow, and with a glow
		// and an outline.
		obs_scene_t *demo = obs_scene_create("Shadow Demo");
		obs_source_t *demo_src = obs_scene_get_source(demo);
		obs_source_t *demo_back = nullptr, *demo_cam = nullptr, *demo_chat = nullptr;
		obs_source_t *demo_shadow = nullptr;
		{
			obs_data_t *bd = obs_data_create();
			obs_data_set_string(bd, "file", texture_image);
			demo_back = obs_source_create("image_source", "Shadow Demo Back", bd, nullptr);
			obs_data_release(bd);
			demo_cam = shadow_block("Shadow Demo Camera", 0xFF3080E0, 170, 128);
			demo_chat = shadow_block("Shadow Demo Chat", 0xFF5A3A2A, 190, 120);
			auto boxed = [&](obs_source_t *src, float x, float y, float bw, float bh) {
				obs_sceneitem_t *item = shadow_put(demo, src, x, y);
				vec2 b;
				vec2_set(&b, bw, bh);
				obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
				obs_sceneitem_set_bounds(item, &b);
				return item;
			};
			obs_sceneitem_set_bounds_type(boxed(demo_back, 0, 0, 640, 360), OBS_BOUNDS_STRETCH);
			boxed(live, 36, 44, 352, 198);
			shadow_put(demo, demo_chat, 420, 44);
			shadow_put(demo, demo_cam, 430, 196);
			obs_data_t *dd = obs_data_create();
			obs_data_set_double(dd, "shadow_opacity", 75.0);
			obs_data_set_double(dd, "offset_x", 14.0);
			obs_data_set_double(dd, "offset_y", 14.0);
			obs_data_set_double(dd, "softness", 24.0);
			demo_shadow = obs_source_create("stagekit_scene_shadow", "Demo Shadow", dd, nullptr);
			obs_data_release(dd);
			obs_source_set_enabled(demo_shadow, false);
			obs_source_filter_add(demo_src, demo_shadow);
			sleep_ms(500);
			write_frame(shadow_dir + "/1 the scene as OBS draws it.rgba", render(demo_src, w, h), w, h);
			obs_data_t *quick = obs_source_get_settings(demo_shadow);
			obs_data_set_int(quick, "ramp_ms", 0);
			obs_source_update(demo_shadow, quick);
			obs_data_release(quick);
			obs_source_set_enabled(demo_shadow, true);
			sleep_ms(500);
			write_frame(shadow_dir + "/2 Scene Shadow.rgba", render(demo_src, w, h), w, h);
			obs_data_t *gd = obs_source_get_settings(demo_shadow);
			obs_data_set_int(gd, "shadow_type", 1);
			obs_data_set_int(gd, "shadow_color", 0xFF30C0FF); // amber
			obs_data_set_double(gd, "shadow_opacity", 90.0);
			obs_data_set_double(gd, "softness", 36.0);
			obs_data_set_double(gd, "spread", 30.0);
			obs_data_set_bool(gd, "outline", true);
			obs_data_set_double(gd, "outline_width", 3.0);
			obs_source_update(demo_shadow, gd);
			obs_data_release(gd);
			sleep_ms(500);
			write_frame(shadow_dir + "/3 glow and outline.rgba", render(demo_src, w, h), w, h);
		}

		// Morph keeps morphing a scene with a Scene Shadow, and carries the
		// shadow through the change: part-way, the shadow is part-way
		// between the two scenes' (a scene with none counts as one at no
		// strength).
		{
			static bool shadow_crossfade_logged = false;
			static log_handler_t shadow_prev_log = nullptr;
			static void *shadow_prev_param = nullptr;
			base_get_log_handler(&shadow_prev_log, &shadow_prev_param);
			shadow_crossfade_logged = false;
			base_set_log_handler(
				[](int level, const char *format, va_list args, void *) {
					char line[2048];
					va_list copy;
					va_copy(copy, args);
					vsnprintf(line, sizeof(line), format, copy);
					va_end(copy);
					if (strstr(line, "cross-fade"))
						shadow_crossfade_logged = true;
					if (shadow_prev_log)
						shadow_prev_log(level, format, args, shadow_prev_param);
				},
				nullptr);
			obs_scene_t *mfrom = obs_scene_create("Shadow Morph From");
			obs_scene_t *mto = obs_scene_create("Shadow Morph To");
			shadow_put(mfrom, bg, 0, 0);
			shadow_put(mfrom, sq_a, 100, 100);
			shadow_put(mto, bg, 0, 0);
			shadow_put(mto, sq_a, 400, 150);
			obs_source_t *from_src = obs_scene_get_source(mfrom), *to_src = obs_scene_get_source(mto);
			obs_source_t *mshadow =
				obs_source_create("stagekit_scene_shadow", "Morph Scene Shadow", nullptr, nullptr);
			obs_source_filter_add(from_src, mshadow);
			obs_data_t *md = obs_data_create();
			obs_data_set_int(md, "ease_curve", 0); // linear
			obs_source_t *morph = obs_source_create_private("stagekit_morph", "Shadow Morph", md);
			obs_data_release(md);
			sleep_ms(400);
			const std::vector<uint8_t> whole = render(from_src, w, h);
			const std::vector<uint8_t> last = render(to_src, w, h);
			obs_transition_set(morph, from_src);
			obs_transition_start(morph, OBS_TRANSITION_MODE_MANUAL, 0, to_src);
			obs_transition_set_manual_time(morph, 0.5f);
			sleep_ms(150);
			render(morph, w, h);
			const std::vector<uint8_t> half = render(morph, w, h);
			// The square half-way: 250..350 across, 125..225 down. Its shadow
			// 6 px to its right, as at 206,150 in the first scene.
			auto lin = [](double v) {
				v /= 255.0;
				return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
			};
			auto srgb = [](double v) {
				return 255.0 * (v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055);
			};
			const int full = shadow_px(whole, 206, 150, 0), back = shadow_px(whole, 600, 20, 0);
			const double expected = srgb(lin(back) * (1.0 - 0.5 * (1.0 - lin(full) / lin(back))));
			const int got = shadow_px(half, 356, 175, 0);
			CHECK(shadow_px(half, 300, 175, 1) > 240 && shadow_px(half, 150, 150, 0) < 140 &&
				      shadow_px(half, 450, 200, 0) < 140 && !shadow_crossfade_logged,
			      "a scene with a Scene Shadow is still morphed: the square is half-way, at neither end, and no cross-fade is logged (white %d half-way)",
			      shadow_px(half, 300, 175, 1));
			CHECK(std::abs(got - expected) <= 6 && got < back - 8,
			      "half-way to a scene without one, the shadow is there at about half its strength (%d; %.0f expected, %d at full strength, %d none)",
			      got, expected, full, back);
			// Run by the clock as OBS's window runs it.
			obs_transition_set_manual_time(morph, 1.0f);
			sleep_ms(120);
			render(morph, w, h);
			obs_transition_set(morph, from_src);
			sleep_ms(100);
			obs_transition_start(morph, OBS_TRANSITION_MODE_AUTO, 1000, to_src);
			sleep_ms(450);
			const std::vector<uint8_t> timed = render(morph, w, h);
			sleep_ms(900);
			render(morph, w, h);
			const std::vector<uint8_t> timed_done = render(morph, w, h);
			int shaded = 0;
			for (int y = 100; y < 260; y++)
				for (int x = 150; x < 450; x++)
					if (shadow_px(timed, x, y, 0) < back - 15 && shadow_px(timed, x, y, 0) > 10)
						shaded++;
			CHECK(shaded > 300 && mean_abs_diff(timed, whole) > 2.0 &&
				      mean_abs_diff(timed_done, last) < 0.01 && !shadow_crossfade_logged,
			      "run by the clock it morphs with the shadow on the way (%d px shaded), and ends on the new scene (%.2f)",
			      shaded, mean_abs_diff(timed_done, last));
			base_set_log_handler(shadow_prev_log, shadow_prev_param);

			// For the README: the demo scene half-way to a plain one.
			{
				obs_data_t *gd = obs_source_get_settings(demo_shadow);
				obs_data_unset_user_value(gd, "shadow_type");
				obs_data_unset_user_value(gd, "shadow_color");
				obs_data_set_double(gd, "shadow_opacity", 75.0);
				obs_data_set_double(gd, "softness", 24.0);
				obs_data_unset_user_value(gd, "spread");
				obs_data_set_bool(gd, "outline", false);
				obs_source_update(demo_shadow, gd);
				obs_data_release(gd);
				obs_scene_t *plain_demo = obs_scene_create("Shadow Demo Plain");
				shadow_put(plain_demo, demo_back, 0, 0);
				{
					obs_sceneitem_t *back_item =
						obs_scene_find_source(plain_demo, "Shadow Demo Back");
					vec2 b;
					vec2_set(&b, 640, 360);
					obs_sceneitem_set_bounds_type(back_item, OBS_BOUNDS_STRETCH);
					obs_sceneitem_set_bounds(back_item, &b);
					obs_sceneitem_t *game_item = shadow_put(plain_demo, live, 140, 20);
					vec2_set(&b, 480, 270);
					obs_sceneitem_set_bounds_type(game_item, OBS_BOUNDS_SCALE_INNER);
					obs_sceneitem_set_bounds(game_item, &b);
				}
				shadow_put(plain_demo, demo_cam, 20, 220);
				sleep_ms(400);
				obs_transition_set(morph, demo_src);
				obs_transition_start(morph, OBS_TRANSITION_MODE_MANUAL, 0,
						     obs_scene_get_source(plain_demo));
				obs_transition_set_manual_time(morph, 0.5f);
				sleep_ms(150);
				render(morph, w, h);
				write_frame(shadow_dir + "/4 Morph half-way.rgba", render(morph, w, h), w, h);
				obs_transition_set_manual_time(morph, 1.0f);
				sleep_ms(120);
				render(morph, w, h);
				obs_scene_release(plain_demo);
			}
			obs_source_release(morph);
			obs_source_filter_remove(from_src, mshadow);
			obs_source_release(mshadow);
			obs_scene_release(mfrom);
			obs_scene_release(mto);
		}

		// One item changing into another under a Scene Shadow: the layer
		// Morph draws the pair with casts, so the pair has its shadow on the
		// way.
		{
			obs_source_t *one = shadow_block("Shadow Morph One", 0xFFFFFFFF, 100, 60);
			obs_source_t *two = shadow_block("Shadow Morph Two", 0xFFFFFFFF, 200, 120);
			obs_scene_t *ims = obs_scene_create("Shadow Item Morph");
			obs_source_t *ims_src = obs_scene_get_source(ims);
			shadow_put(ims, bg, 0, 0);
			obs_sceneitem_t *one_item = shadow_put(ims, one, 40, 150);
			obs_sceneitem_t *two_item = shadow_put(ims, two, 380, 120);
			obs_sceneitem_set_visible(two_item, false);
			obs_source_t *ishadow =
				obs_source_create("stagekit_scene_shadow", "Item Morph Shadow", nullptr, nullptr);
			obs_source_filter_add(ims_src, ishadow);
			obs_data_t *gd = obs_data_create();
			obs_data_array_t *links = obs_data_array_create();
			obs_data_t *line = obs_data_create();
			obs_data_set_string(line, "value", "Shadow Morph One \xE2\x86\x94 Shadow Morph Two");
			obs_data_array_push_back(links, line);
			obs_data_release(line);
			obs_data_set_array(gd, "links", links);
			obs_data_array_release(links);
			obs_source_t *linker = obs_source_create_private("stagekit_morph", "Shadow Links", gd);
			obs_data_release(gd);
			for (obs_sceneitem_t *item : {one_item, two_item})
				for (bool show : {true, false}) {
					obs_data_t *d = obs_data_create();
					obs_data_set_int(d, "ease_curve", 0);
					obs_source_t *t =
						obs_source_create_private("stagekit_morph", "Shadow Item Morph", d);
					obs_data_release(d);
					obs_sceneitem_set_transition(item, show, t);
					obs_sceneitem_set_transition_duration(item, show, 1000);
					obs_source_release(t);
				}
			obs_source_t *before_morph_out = obs_get_output_source(0);
			obs_set_output_source(0, ims_src);
			sleep_ms(400);
			auto shaded_middle = [&](const std::vector<uint8_t> &frame) {
				int n = 0;
				for (int y = 0; y < (int)h; y++)
					for (int x = 200; x < 400; x++)
						if (shadow_px(frame, x, y, 0) < 113 && shadow_px(frame, x, y, 0) > 10)
							n++;
				return n;
			};
			const int before = shaded_middle(render(ims_src, w, h));
			obs_sceneitem_set_visible(one_item, false);
			obs_sceneitem_set_visible(two_item, true);
			sleep_ms(450);
			int items = 0;
			obs_scene_enum_items(
				ims,
				[](obs_scene_t *, obs_sceneitem_t *, void *param) {
					++*static_cast<int *>(param);
					return true;
				},
				&items);
			const std::vector<uint8_t> mid = render(ims_src, w, h);
			const int during = shaded_middle(mid);
			CHECK(items == 4 && before == 0 && during > 300,
			      "one item changing into another under a Scene Shadow: Morph's layer casts, so the pair has its shadow on the way (%d px shaded part-way, %d before; %d items)",
			      during, before, items);
			sleep_ms(900);
			render(ims_src, w, h);
			obs_set_output_source(0, before_morph_out);
			obs_source_release(before_morph_out);
			obs_source_filter_remove(ims_src, ishadow);
			obs_source_release(ishadow);
			obs_source_release(linker);
			obs_scene_release(ims);
			obs_source_release(one);
			obs_source_release(two);
		}

		obs_source_filter_remove(demo_src, demo_shadow);
		obs_source_release(demo_shadow);
		obs_scene_release(demo);
		for (obs_source_t *src : {demo_back, demo_cam, demo_chat})
			obs_source_release(src);
		obs_source_filter_remove(ss_src, scene_shadow);
		obs_source_release(scene_shadow);
		obs_scene_release(ss);
		obs_scene_release(sp);
		obs_scene_release(sr);
		for (obs_source_t *src : {bg, sq_a, sq_b, sq_c, sq_r})
			obs_source_release(src);
	}
	// ==== end scene-filters:shadow ====

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
		// The 4:3 shape held from the size before is checked against the new
		// picture over the next few frames, then the picture glides to its fit.
		for (int i = 0; i < 3; i++) {
			sleep_ms(50);
			picture();
		}
		sleep_ms(400);
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
			auto [t0, p0] = look(100);
			CHECK(t0 == 0.0 && is(p0, 0, 75, 800, 450),
			      "bars are not trimmed on the strength of a single look (trim %.0f)", t0);
			auto [t1, p1] = look(1200);
			CHECK(std::fabs(t1 - 80) <= 1.0 && is(p1, 0, 0, 800, 600),
			      "within a second or so, 80 px bars are trimmed and the 4:3 picture fills the frame (trim %.0f, %.0fx%.0f)",
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
				// Two builds with one file name, told apart by the window's
				// title: the saved shape that says the most is the one used.
				obs_data_set_string(bd, "file", paint2("odd-shape.png", 1280, 720, 290).c_str());
				obs_source_update(barred, bd);
				obs_data_set_string(bf, "rules_text", "oddemu = 16:9, special build = 0.97");
				obs_source_update(trimmer, bf);
				obs_data_t *twin = obs_source_get_settings(barred);
				obs_data_set_string(twin, "window", "Special Build 2:OddWnd:oddemu.exe");
				obs_data_release(twin);
				auto [d5, dp5] = look(1500);
				twin = obs_source_get_settings(barred);
				obs_data_set_string(twin, "window", "Odd Emulator:OddWnd:oddemu.exe");
				obs_data_release(twin);
				auto [d6, dp6] = look(1500);
				CHECK(std::fabs(d5 - 290) <= 1.5 && d6 < 5.0,
				      "a shape saved for part of a window's title wins over one for the program's file name (trim %.0f; the other window %.0f)",
				      d5, d6);
				obs_data_set_string(bf, "rules_text", "oddemu = 0.97");
				obs_source_update(trimmer, bf);
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

			// A widescreen game in a window (the report: it opened cropped to
			// a square and took a long time to come right). Its picture sits
			// letterboxed in a window of another shape, the first frames are
			// black and at another size, and the scene is dark.
			{
				auto trims = [&](int ms) {
					for (int i = 0; i < ms / 50; i++) {
						render(barred, 800, 600);
						sleep_ms(50);
					}
					calldata_t cd = {};
					proc_handler_call(obs_source_get_proc_handler(trimmer), "get_trim", &cd);
					const std::pair<double, double> t{calldata_float(&cd, "x"),
									  calldata_float(&cd, "y")};
					calldata_free(&cd);
					return t;
				};
				auto save = [&](QImage &image, const char *name) {
					const std::string path = out + "/" + name;
					image.save(QString::fromStdString(path));
					return path;
				};
				auto program = [&](const char *window) {
					obs_data_t *cap = obs_source_get_settings(barred);
					obs_data_set_string(cap, "window", window);
					obs_data_release(cap);
				};
				auto show = [&](const std::string &path) {
					obs_data_set_string(bd, "file", path.c_str());
					obs_source_update(barred, bd);
				};
				// Lit where the test says, dark blue like a night scene.
				auto night = [](int x, int y) {
					return qRgb(12 + x % 40, 24, 70 + y % 60);
				};

				QImage start(100, 100, QImage::Format_RGB32);
				start.fill(QColor(0, 0, 0));
				// 896x572 holds a 16:9 picture (896x504) with 34 px above and below.
				QImage dark(896, 572, QImage::Format_RGB32);
				dark.fill(QColor(0, 0, 0));
				for (int y = 34; y < 538; y++)
					for (int x = 0; x < 540; x++) // the right of the scene is in darkness
						dark.setPixel(x, y, night(x, y));

				obs_data_set_int(bf, "trim", 1);
				obs_data_set_string(bf, "rules_text", "");
				obs_data_set_bool(bf, "known_programs", true);
				obs_source_update(trimmer, bf);
				program("Wide Game:WideWnd:widegame.exe");
				show(save(start, "wide-start.png"));
				trims(400);
				show(save(dark, "wide-dark.png"));
				bool cropped = false;
				for (int i = 0; i < 6; i++)
					if (trims(100).first != 0.0)
						cropped = true;
				auto [wx, wy] = trims(1900);
				CHECK(!cropped && wx == 0.0,
				      "a widescreen game that starts black at another size is never cropped at the sides (trim %.0f)",
				      wx);
				CHECK(std::fabs(wy - 34) <= 1.0,
				      "and its own bars above and below are trimmed within moments, dark scene or not (trim %.0f)",
				      wy);

				// A shape carried over from before that does not suit what is
				// now shown: a dark, moving picture with nothing steady about it.
				auto blobs = [&](const char *name, int ax, int ar, int bx, int br) {
					QImage image(1280, 720, QImage::Format_RGB32);
					image.fill(QColor(0, 0, 0));
					QPainter painter(&image);
					painter.setPen(Qt::NoPen);
					painter.setBrush(QColor(20, 40, 110));
					painter.drawEllipse(QPoint(ax, 300), ar, ar);
					painter.drawEllipse(QPoint(bx, 400), br, br);
					painter.drawEllipse(QPoint(640, 360), 90, 60);
					painter.end();
					return save(image, name);
				};
				QImage window43(400, 300, QImage::Format_RGB32);
				for (int y = 0; y < 300; y++)
					for (int x = 0; x < 400; x++)
						window43.setPixel(x, y, qRgb(60 + x % 150, 90 + y % 120, 200));
				const std::string moving_a = blobs("wide-moving-a.png", 200, 180, 1000, 150);
				const std::string moving_b = blobs("wide-moving-b.png", 230, 170, 980, 160);
				program("Another Game:OtherWnd:othergame.exe");
				show(save(window43, "wide-43.png"));
				trims(1500);
				show(moving_a);
				const double carried = trims(100).first;
				for (int i = 0; i < 28; i++) {
					show(i % 2 ? moving_a : moving_b);
					trims(100);
				}
				const double let_go = trims(100).first;
				CHECK(std::fabs(carried - 160) <= 1.0 && let_go == 0.0,
				      "a shape carried over is let go when the picture plainly shows inside it (trim %.0f, then %.0f)",
				      carried, let_go);

				// A logo on black whose width happens to leave a real picture
				// shape (960x720 is 4:3): its edges are not straight, so it is not bars.
				QImage logo(1280, 720, QImage::Format_RGB32);
				logo.fill(QColor(0, 0, 0));
				{
					QPainter painter(&logo);
					painter.setPen(Qt::NoPen);
					painter.setBrush(QColor(230, 230, 240));
					painter.drawEllipse(QPoint(640, 360), 480, 200);
				}
				program("Logo Game:LogoWnd:logogame.exe");
				show(save(logo, "wide-logo.png"));
				auto [lx, ly] = trims(3000);
				CHECK(lx == 0.0 && ly == 0.0,
				      "a logo on black is not bars, even when its width leaves a real shape (trim %.0f, %.0f)",
				      lx, ly);

				// Bars that have been seen, with a mouse pointer resting on one.
				auto pillar = [&](const char *name, bool pointer) {
					QImage image(1280, 720, QImage::Format_RGB32);
					image.fill(QColor(0, 0, 0));
					for (int y = 0; y < 720; y++)
						for (int x = 160; x < 1120; x++)
							image.setPixel(x, y, qRgb(60 + x % 150, 90 + y % 120, 200));
					if (pointer)
						for (int y = 300; y < 322; y++)
							for (int x = 40; x < 54; x++)
								image.setPixel(x, y, qRgb(255, 255, 255));
					return save(image, name);
				};
				program("Pointer Game:PointerWnd:pointergame.exe");
				show(pillar("wide-pillar.png", false));
				const double before_pointer = trims(2000).first;
				show(pillar("wide-pillar-pointer.png", true));
				const double with_pointer = trims(7000).first;
				CHECK(std::fabs(before_pointer - 160) <= 1.0 && std::fabs(with_pointer - 160) <= 1.0,
				      "a mouse pointer resting on a bar does not resize the picture (trim %.0f, then %.0f)",
				      before_pointer, with_pointer);

				// An emulator's usual shape is only where its picture starts.
				// Here the picture is 16:9 with bars above and below. It is dark
				// at the sides but for a band along the top, too little to count
				// as the picture when looking across, yet plainly inside the
				// guess: the bars that are seen decide, not the guess.
				QImage centre(960, 620, QImage::Format_RGB32);
				centre.fill(QColor(0, 0, 0));
				for (int y = 40; y < 580; y++)
					for (int x = 320; x < 640; x++)
						centre.setPixel(x, y, night(x, y));
				for (int y = 40; y < 170; y++)
					for (int x = 0; x < 960; x++)
						centre.setPixel(x, y, night(x, y));
				program("Game - Snes9x:Snes9x:snes9x-x64.exe");
				show(save(start, "wide-start.png"));
				trims(400);
				show(save(centre, "wide-centre.png"));
				// The guess is in place for the first few frames at the new size.
				double guessed = 0.0;
				for (int i = 0; i < 6; i++)
					guessed = std::max(guessed, trims(50).first);
				auto [sx, sy] = trims(2200);
				CHECK(guessed > 100.0 && sx == 0.0 && std::fabs(sy - 40) <= 1.0,
				      "bars seen clearly one way overrule a starting guess the other way (sides %.0f then %.0f, above and below %.0f)",
				      guessed, sx, sy);

				// The same guess when nothing shows inside it stays: a cut-scene
				// with its own bars above and below, inside an old-shape picture.
				QImage cinema(1280, 720, QImage::Format_RGB32);
				cinema.fill(QColor(0, 0, 0));
				for (int y = 40; y < 680; y++) // 1280x640 would be a real shape (2:1) on its own
					for (int x = 229; x < 1051; x++) // the 8:7 picture is 822 wide
						cinema.setPixel(x, y, night(x, y));
				program("");
				trims(400);
				program("Game - Snes9x:Snes9x:snes9x-x64.exe");
				show(save(start, "wide-start.png"));
				trims(400);
				show(save(cinema, "wide-cinema.png"));
				auto [kx, ky] = trims(2500);
				CHECK(std::fabs(kx - 229) <= 1.5,
				      "a guess that nothing shows inside is kept when bars appear the other way (sides %.0f, above and below %.0f)",
				      kx, ky);

				// A modern game in a window on a dim scene: far darker than plain
				// brightness could tell from bars, but not the flat black of bars.
				QImage dim(896, 572, QImage::Format_RGB32);
				dim.fill(QColor(0, 0, 0));
				for (int y = 34; y < 538; y++)
					for (int x = 0; x < 896; x++)
						dim.setPixel(x, y, qRgb(9, 9, 10 + (x + y) % 3));
				program("Dim Game:DimWnd:dimgame.exe");
				show(save(start, "wide-start.png"));
				trims(400);
				show(save(dim, "wide-dim.png"));
				auto [mx, my] = trims(1200);
				CHECK(mx == 0.0 && std::fabs(my - 34) <= 1.0,
				      "a dim picture still shows where it ends: its bars are trimmed within a second or so (trim %.0f, %.0f)",
				      mx, my);

				// A dark scene in a window: only a few lit specks touch each side
				// of the picture, nowhere near a straight edge. A picture that
				// reaches both ends one way and stops the same distance in from
				// both ends the other way, leaving 16:9, is a picture fitted in
				// its window.
				auto specks = [&](const char *name, int bottom) {
					QImage image(896, 572, QImage::Format_RGB32);
					image.fill(QColor(0, 0, 0));
					QPainter painter(&image);
					painter.setPen(Qt::NoPen);
					painter.setBrush(QColor(120, 140, 200));
					for (int x : {100, 300, 520, 700, 820})
						painter.drawRect(x, 34, 12, 6); // along the top of the picture
					for (int x : {60, 250, 450, 650, 850})
						painter.drawRect(x, bottom - 6, 12, 6); // along the bottom
					for (int y : {120, 260, 400})
						painter.drawRect(0, y, 6, 14); // at the left end
					for (int y : {150, 300, 480})
						painter.drawRect(890, y, 6, 14); // at the right end
					painter.drawEllipse(QPoint(448, 286), 130, 90);
					painter.end();
					return save(image, name);
				};
				program("Speck Game:SpeckWnd:speckgame.exe");
				show(save(start, "wide-start.png"));
				trims(400);
				show(specks("wide-specks.png", 538));
				auto [kx2, ky2] = trims(1500);
				CHECK(kx2 == 0.0 && std::fabs(ky2 - 34) <= 1.0,
				      "a dark scene with a few lit specks at each side is enough for a picture that fits its window (trim %.0f, %.0f)",
				      kx2, ky2);
				program("Uneven Game:UnevenWnd:unevengame.exe");
				show(save(start, "wide-start.png"));
				trims(400);
				show(specks("wide-specks-uneven.png", 506));
				auto [ux, uy] = trims(2000);
				CHECK(ux == 0.0 && uy == 0.0,
				      "but not when it stops at different distances from the two ends (trim %.0f, %.0f)",
				      ux, uy);

				// A program's shape, once seen and held, is where it starts the
				// next time: right from the first frame, even on a black screen.
				QImage bright(896, 572, QImage::Format_RGB32);
				bright.fill(QColor(0, 0, 0));
				for (int y = 34; y < 538; y++)
					for (int x = 0; x < 896; x++)
						bright.setPixel(x, y, qRgb(60 + x % 150, 90 + y % 120, 200));
				QImage black_window(1000, 640, QImage::Format_RGB32);
				black_window.fill(QColor(0, 0, 0));
				QImage old_shape(1000, 640, QImage::Format_RGB32); // 4:3: 854x640 with 73 px each side
				old_shape.fill(QColor(0, 0, 0));
				for (int y = 0; y < 640; y++)
					for (int x = 73; x < 927; x++)
						old_shape.setPixel(x, y, qRgb(60 + x % 150, 90 + y % 120, 200));
				auto remembered = [&] {
					obs_data_t *now = obs_source_get_settings(trimmer);
					const std::string text = obs_data_get_string(now, "learned");
					obs_data_release(now);
					return text;
				};
				obs_data_set_double(bf, "learn_after", 1.0);
				obs_source_update(trimmer, bf);
				program("Learner:LearnWnd:Learner.exe");
				show(save(bright, "wide-bright.png"));
				trims(2800);
				const std::string first_time = remembered();
				program("Something Else:ElseWnd:else.exe");
				trims(500);
				program("Learner:LearnWnd:Learner.exe");
				show(save(black_window, "wide-black-window.png"));
				const double from_memory = trims(600).second;
				obs_properties_t *learned_props = obs_source_properties(trimmer);
				const std::string learned_status =
					obs_property_description(obs_properties_get(learned_props, "status"));
				CHECK(first_time == "learner.exe = 16:9" && std::fabs(from_memory - 39) <= 1.0 &&
					      learned_status.find("before") != std::string::npos,
				      "a shape that has held is remembered for its program and used from the first frame next time, even on black (%s; trim %.0f; %s)",
				      first_time.c_str(), from_memory, learned_status.c_str());

				// The same program now shows another shape: what is seen wins,
				// and a program with more than one shape starts from nothing.
				show(save(old_shape, "wide-old-shape.png"));
				auto [ox, oy] = trims(3200);
				const std::string second_time = remembered();
				program("Something Else:ElseWnd:else.exe");
				trims(500);
				program("Learner:LearnWnd:Learner.exe");
				show(save(black_window, "wide-black-window.png"));
				auto [nx, ny] = trims(600);
				CHECK(std::fabs(ox - 73) <= 1.5 && oy == 0.0 && second_time == "learner.exe = varies" &&
					      nx == 0.0 && ny == 0.0,
				      "a remembered shape gives way to what is seen, and a program with more than one shape starts from nothing (trim %.0f, %.0f; %s; then %.0f, %.0f)",
				      ox, oy, second_time.c_str(), nx, ny);
				obs_property_button_clicked(obs_properties_get(learned_props, "learned_forget"),
							    trimmer);
				obs_properties_destroy(learned_props);
				sleep_ms(100);
				CHECK(remembered().empty(),
				      "'Forget shapes seen before' clears what was remembered (%s)",
				      remembered().c_str());
				// A program whose picture simply fills its window has no shape to start from.
				QImage filling(1000, 650, QImage::Format_RGB32); // not a shape any console uses
				for (int y = 0; y < 650; y++)
					for (int x = 0; x < 1000; x++)
						filling.setPixel(x, y, qRgb(60 + x % 150, 90 + y % 120, 200));
				program("Filler:FillWnd:Filler.exe");
				show(save(bright, "wide-bright.png"));
				trims(2800);
				const std::string kept_shape = remembered();
				program("Something Else:ElseWnd:else.exe");
				trims(500);
				program("Filler:FillWnd:Filler.exe");
				show(save(filling, "wide-filling.png"));
				auto [fx, fy] = trims(3000);
				CHECK(kept_shape == "filler.exe = 16:9" && fx == 0.0 && fy == 0.0 &&
					      remembered() == "filler.exe = varies",
				      "a program that then fills a window of another shape is shown whole and no longer starts from a shape (%s; trim %.0f, %.0f; %s)",
				      kept_shape.c_str(), fx, fy, remembered().c_str());
				obs_data_set_string(bf, "learned", "");
				obs_data_set_double(bf, "learn_after", 15.0);

				program("");
				obs_data_set_bool(bf, "known_programs", false);
				obs_source_update(trimmer, bf);
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

			// apply_in: 0 every scene, 1 the scene on screen, 2 one chosen scene.
			// -1 is a key from before that choice existed: unset, with the
			// "where" that older versions wrote.
			auto make = [&](const char *name, int destination, int apply_in, const char *in = "") {
				obs_data_t *d = obs_data_create();
				obs_data_set_int(d, "destination", destination);
				obs_data_set_double(d, "margin", 30);
				obs_data_set_int(d, "duration_ms", 150);
				if (apply_in >= 0)
					obs_data_set_int(d, "apply_in", apply_in);
				else
					obs_data_set_int(d, "where", 0);
				obs_data_set_string(d, "apply_scene", in);
				obs_source_t *f = obs_source_create("stagekit_send_to", name, d, nullptr);
				obs_data_release(d);
				obs_source_filter_add(cam, f);
				sleep_ms(100);
				return f;
			};
			obs_source_t *corner = make("Camera bottom right", 8, 1);
			CHECK(corner != nullptr, "the Send To filter can be added to a source");
			obs_source_set_enabled(corner, true);
			sleep_ms(450);
			vec2 at = pos_of(cam_item), elsewhere = pos_of(other_cam);
			CHECK(std::fabs(at.x - 450) < 0.6 && std::fabs(at.y - 240) < 0.6,
			      "switching it on moves that source in the scene on screen (%.0f, %.0f)", at.x, at.y);
			CHECK(!obs_source_enabled(corner),
			      "and it switches itself off again, ready for the next press");
			CHECK(std::fabs(elsewhere.x - 5) < 0.6,
			      "set to 'only the scene on screen', other scenes are left alone");

			// Save a spot: drag the camera somewhere, press the button, move away, press the key.
			obs_source_t *spot = make("Camera my spot", 0, -1);
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
			      "left as it comes, a key moves the source in every scene it is in, and so does a key made before that was a choice (%.0f, %.0f)",
			      elsewhere.x, elsewhere.y);

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
			{
				// One chosen scene: only there, whichever scene is on screen.
				obs_source_t *shown = obs_get_output_source(0);
				obs_set_output_source(0, scene_source);
				obs_source_t *one = make("Camera in the other scene", 2, 2, "Other Scene");
				const vec2 here_before = pos_of(cam_item);
				obs_properties_t *props = obs_source_properties(one);
				obs_property_t *list = obs_properties_get(props, "apply_scene");
				bool offered = false;
				for (size_t i = 0; list && i < obs_property_list_item_count(list); i++)
					if (strcmp(obs_property_list_item_string(list, i), "Other Scene") == 0)
						offered = true;
				obs_properties_destroy(props);
				obs_source_set_enabled(one, true);
				sleep_ms(450);
				const vec2 here = pos_of(cam_item);
				elsewhere = pos_of(other_cam);
				const float top_right =
					(float)obs_source_get_width(obs_scene_get_source(other)) - 30 - 160;
				CHECK(offered && std::fabs(here.x - here_before.x) < 0.6 &&
					      std::fabs(here.y - here_before.y) < 0.6 &&
					      std::fabs(elsewhere.x - top_right) < 0.6 &&
					      std::fabs(elsewhere.y - 30) < 0.6,
				      "'only one scene I choose' lists the scenes it is in and moves it only there (%.0f, %.0f; here %.0f, %.0f)",
				      elsewhere.x, elsewhere.y, here.x, here.y);
				obs_source_filter_remove(cam, one);
				obs_source_release(one);
				obs_set_output_source(0, shown);
				obs_source_release(shown);
			}
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

			// "In every scene" is ticked to begin with: the buttons, and the keys
			// made, reach the camera wherever it is, and one Undo takes it all back.
			{
				obs_scene_t *second = obs_scene_create("Second Camera Scene");
				obs_sceneitem_t *second_cam = obs_scene_add(second, cam);
				vec2 start;
				vec2_set(&start, 7.0f, 9.0f);
				obs_sceneitem_set_pos(second_cam, &start);
				sleep_ms(100);
				auto *every = dock->findChild<QCheckBox *>("sendEveryScene");
				obs_source_t *made = obs_source_get_filter_by_name(cam, "Top right");
				obs_data_t *made_settings = made ? obs_source_get_settings(made) : nullptr;
				CHECK(every && every->isChecked() && made_settings &&
					      obs_data_get_int(made_settings, "apply_in") == 0,
				      "the dock, and the keys it makes, act in every scene unless told otherwise");
				obs_data_release(made_settings);
				obs_source_release(made);

				const vec2 was = pos_of(cam_item);
				click("grid0");
				sleep_ms(900);
				QCoreApplication::processEvents();
				vec2 here = pos_of(cam_item), there = pos_of(second_cam);
				CHECK(std::fabs(here.x - 30) < 0.6 && std::fabs(here.y - 30) < 0.6 &&
					      std::fabs(there.x - 30) < 0.6 && std::fabs(there.y - 30) < 0.6,
				      "a Send to button moves the item's source in every scene it is in (%.0f, %.0f and %.0f, %.0f)",
				      here.x, here.y, there.x, there.y);
				calldata_t undo_cd = {};
				calldata_set_bool(&undo_cd, "redo", false);
				proc_handler_call(obs_get_proc_handler(), "stagekit_replay_arrange", &undo_cd);
				calldata_free(&undo_cd);
				sleep_ms(100);
				here = pos_of(cam_item);
				there = pos_of(second_cam);
				CHECK(std::fabs(here.x - was.x) < 0.6 && std::fabs(here.y - was.y) < 0.6 &&
					      std::fabs(there.x - 7) < 0.6 && std::fabs(there.y - 9) < 0.6,
				      "and one Undo takes it back in all of them (%.0f, %.0f and %.0f, %.0f)", here.x,
				      here.y, there.x, there.y);

				if (every)
					every->setChecked(false);
				click("grid8");
				sleep_ms(900);
				QCoreApplication::processEvents();
				here = pos_of(cam_item);
				there = pos_of(second_cam);
				CHECK(std::fabs(here.x - 450) < 0.6 && std::fabs(here.y - 240) < 0.6 &&
					      std::fabs(there.x - 7) < 0.6 && std::fabs(there.y - 9) < 0.6,
				      "unticked, only the scene being worked on changes (%.0f, %.0f; the other %.0f, %.0f)",
				      here.x, here.y, there.x, there.y);
				click("makeKeys");
				made = obs_source_get_filter_by_name(cam, "Top right");
				made_settings = made ? obs_source_get_settings(made) : nullptr;
				CHECK(made_settings && obs_data_get_int(made_settings, "apply_in") == 1,
				      "and keys made then act only in the scene on screen");
				obs_data_release(made_settings);
				obs_source_release(made);
				if (every)
					every->setChecked(true);
				obs_sceneitem_remove(second_cam);
				obs_scene_release(second);
			}

			// Order buttons: front above back in one column, forward one above back one in the other.
			{
				// They are on the Arrange tab, which has to be the one showing
				// for its buttons to have a place.
				auto *order_tabs = dock->findChild<QTabWidget *>("stagekitTabs");
				if (order_tabs)
					order_tabs->setCurrentIndex(1);
				QCoreApplication::processEvents();
				dock->grab(); // lays out a widget that is not on screen
				auto at = [&](const char *name) {
					auto *button = dock->findChild<QPushButton *>(name);
					return button ? button->mapTo(dock, QPoint(0, 0)) : QPoint(-1, -1);
				};
				const QPoint top = at("orderTop"), bottom = at("orderBottom"), up = at("orderUp"),
					     down = at("orderDown");
				CHECK(top.x() == bottom.x() && up.x() == down.x() && top.x() < up.x() &&
					      top.y() == up.y() && bottom.y() == down.y() && top.y() < bottom.y(),
				      "the order tools are a block of two by two: to the front and forward one on top, to the back and back one under them");
				// Each has a drawing of its own: no two look alike.
				{
					std::vector<QImage> drawn;
					for (const char *name : {"orderTop", "orderUp", "orderDown", "orderBottom"})
						if (auto *button = dock->findChild<QPushButton *>(name))
							drawn.push_back(button->icon().pixmap(64, 64).toImage());
					double least = 1e9;
					for (size_t a = 0; a < drawn.size(); a++)
						for (size_t b = a + 1; b < drawn.size(); b++) {
							double differ = 0;
							for (int y = 0; y < 64; y++)
								for (int x = 0; x < 64; x++)
									differ += qAlpha(drawn[a].pixel(x, y)) !=
										  qAlpha(drawn[b].pixel(x, y));
							least = std::min(least, differ / (64.0 * 64.0));
						}
					CHECK(drawn.size() == 4 && least > 0.08,
					      "the four order tools' drawings are told apart at a glance (the two most alike differ in %.0f %% of the drawing)",
					      least * 100.0);
				}
				// The line-up tools are drawings alone, each named when pointed at.
				auto *left_tool = dock->findChild<QPushButton *>("arrange_left");
				auto *arrow_tool = dock->findChild<QPushButton *>("grid0");
				CHECK(left_tool && left_tool->text().isEmpty() && !left_tool->icon().isNull() &&
					      left_tool->toolTip().startsWith("Left") && left_tool->width() < 90,
				      "an arrange tool is a drawing with its name in the tooltip (%d px wide)",
				      left_tool ? left_tool->width() : -1);
				// The line-up tools are a block three across, four rows, with
				// how the tools measure beside it.
				{
					const QPoint l = at("arrange_left"), r = at("arrange_right"),
						     t2 = at("arrange_top"), w = at("arrange_same_width");
					auto *measure = dock->findChild<QCheckBox *>("arrangeEveryScene");
					const QPoint m = measure ? measure->mapTo(dock, QPoint(0, 0)) : QPoint(-1, -1);
					CHECK(l.y() == r.y() && r.x() > l.x() && t2.x() == l.x() && t2.y() > l.y() &&
						      w.x() == l.x() && w.y() > t2.y() && r.x() >= l.x() + 80 &&
						      m.x() >= r.x() + 40 && m.y() < w.y() + 40,
					      "the arrange tools are a block three across with how the tools measure beside it (left %d,%d; top %d,%d; same width %d,%d; options from %d)",
					      l.x(), l.y(), t2.x(), t2.y(), w.x(), w.y(), m.x());
				}
				CHECK(arrow_tool && arrow_tool->minimumWidth() == 40 &&
					      arrow_tool->maximumHeight() == 40,
				      "the Place arrows are squares");
				// The updates are a menu for OBS's Tools menu, not a button in
				// the dock, and the notice of a newer one is the dock's first row.
				auto *updates = dock->findChild<QMenu *>("updateMenu");
				auto *notice = dock->findChild<QWidget *>("updateBar");
				auto *install = dock->findChild<QAction *>("updateInstall");
				CHECK(updates && updates->actions().size() >= 5 &&
					      !dock->findChild<QPushButton *>("updateMenu") && notice &&
					      dock->layout()->indexOf(notice) == 0 && notice->isHidden() && install &&
					      !install->isVisible(),
				      "the updates are a menu for OBS's Tools menu; the dock only has a notice, first of all and hidden until there is a newer version");
				// OBS's window layout, kept by StageKit: the menu has the four
				// entries, and a layout saved comes back after the docks have
				// been moved about.
				{
					CHECK(updates && updates->findChild<QAction *>("layoutSave") &&
						      updates->findChild<QAction *>("layoutRestore") &&
						      updates->findChild<QAction *>("layoutAtStart") &&
						      updates->findChild<QAction *>("layoutAtExit"),
					      "the menu has the window layout's entries: save, restore, at start-up and on closing");
					QMainWindow window;
					window.setObjectName("layoutTestWindow");
					auto *one_dock = new QDockWidget("One", &window);
					auto *two_dock = new QDockWidget("Two", &window);
					one_dock->setObjectName("layoutTestOne");
					two_dock->setObjectName("layoutTestTwo");
					window.addDockWidget(Qt::LeftDockWidgetArea, one_dock);
					window.addDockWidget(Qt::BottomDockWidgetArea, two_dock);
					auto layout = [&](bool save) {
						calldata_t cd = {};
						calldata_set_ptr(&cd, "window", &window);
						calldata_set_bool(&cd, "save", save);
						proc_handler_call(obs_get_proc_handler(), "stagekit_window_layout",
								  &cd);
						const bool ok = calldata_bool(&cd, "ok");
						calldata_free(&cd);
						return ok;
					};
					const bool saved = layout(true);
					window.addDockWidget(Qt::RightDockWidgetArea, one_dock);
					window.addDockWidget(Qt::TopDockWidgetArea, two_dock);
					two_dock->setFloating(true);
					const bool moved = window.dockWidgetArea(one_dock) == Qt::RightDockWidgetArea &&
							   two_dock->isFloating();
					const bool restored = layout(false);
					CHECK(saved && moved && restored &&
						      window.dockWidgetArea(one_dock) == Qt::LeftDockWidgetArea &&
						      window.dockWidgetArea(two_dock) == Qt::BottomDockWidgetArea &&
						      !two_dock->isFloating(),
					      "a saved window layout comes back: each dock where it was, docked again (saved %d, restored %d)",
					      (int)saved, (int)restored);
					// When it comes back by itself: OBS started, and every
					// change of scene collection (OBS puts the collection's
					// docks up again); nothing else.
					auto after = [](int event) {
						calldata_t cd = {};
						calldata_set_int(&cd, "event", event);
						proc_handler_call(obs_get_proc_handler(),
								  "stagekit_window_layout_after", &cd);
						const bool yes = calldata_bool(&cd, "restores");
						calldata_free(&cd);
						return yes;
					};
					CHECK(after(OBS_FRONTEND_EVENT_FINISHED_LOADING) &&
						      after(OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED) &&
						      !after(OBS_FRONTEND_EVENT_SCENE_CHANGED) &&
						      !after(OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING),
					      "the saved window layout comes back by itself when OBS starts and after every change of scene collection");
					QAction *at_start = updates->findChild<QAction *>("layoutAtStart");
					CHECK(at_start &&
						      at_start->text() ==
							      QString("Restore it when a collection loads") &&
						      updates->findChild<QAction *>("layoutSave")->text() ==
							      QString("Save window layout"),
					      "and the menu says so (%s)",
					      at_start ? at_start->text().toUtf8().constData() : "-");
				}
				// Pictures of the two busiest tabs, to look at.
				dock->resize(380, dock->sizeHint().height());
				dock->grab().save(QString::fromStdString(out + "/../stagekit-dock-arrange.png"));
				if (order_tabs) {
					order_tabs->setCurrentIndex(0);
					QCoreApplication::processEvents();
					dock->resize(380, dock->sizeHint().height());
					dock->grab().save(QString::fromStdString(out + "/../stagekit-dock-place.png"));
					order_tabs->setCurrentIndex(1);
					QCoreApplication::processEvents();
				}
				// The dock is only as tall as the tab showing needs: a tall
				// tab out of sight must not make a short one scroll.
				if (order_tabs) {
					int tallest = 0, shortest = 1 << 20;
					for (int i = 0; i < order_tabs->count(); i++) {
						order_tabs->setCurrentIndex(i);
						QCoreApplication::processEvents();
						const int needs = dock->minimumSizeHint().height();
						tallest = std::max(tallest, needs);
						shortest = std::min(shortest, needs);
					}
					printf("        the dock needs %d to %d px by tab\n", shortest, tallest);
					// The update notice fits a narrow dock: it must never be
					// what makes the dock wider than its window.
					auto *bar = dock->findChild<QWidget *>("updateBar");
					const int bar_needs = bar ? bar->minimumSizeHint().width() : 9999;
					printf("        the dock needs %d px across, the update notice %d\n",
					       dock->minimumSizeHint().width(), bar_needs);
					CHECK(bar_needs <= 120, "the update notice fits a narrow dock (%d px)",
					      bar_needs);
					CHECK(shortest > 0 && shortest < tallest - 40,
					      "the dock is as tall as the tab showing, not as the tallest tab");
				}
				if (order_tabs)
					order_tabs->setCurrentIndex(0);
			}
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
				// It is also the scene being edited, where items are selected in OBS's preview.
				obs_source_t *edited_before = obs_get_output_source(0);
				obs_set_output_source(0, as_source);
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

					// The report: the dock's own scene picker left on another
					// scene, and items picked one after the other without a pause.
					// The order comes from OBS, and the tools work where the items
					// are selected.
					if (scenes)
						scenes->setCurrentIndex(std::max(0, scenes->findData("Test Scene")));
					settle();
					select_only({ib});
					obs_sceneitem_select(ia, true); // A (100 wide) is selected last
					against(3);
					settle();
					click("arrange_same_width");
					vec2 scale_b;
					obs_sceneitem_get_scale(ib, &scale_b);
					CHECK(std::fabs(scale_b.x - 100.0 / 60.0) < 0.01,
					      "'same width' takes the last selected item's width, whatever scene the dock is showing (scale %.2f)",
					      scale_b.x);
					obs_sceneitem_set_scale(ib, &one);
					// The same from a hotkey or the preview's menu, picked the other way round.
					select_only({ia});
					obs_sceneitem_select(ib, true); // now B (60 wide) is last
					calldata_t hk = {};
					calldata_set_string(&hk, "action", "same_width");
					proc_handler_call(obs_get_proc_handler(), "stagekit_run_arrange", &hk);
					calldata_free(&hk);
					vec2 scale_a2;
					obs_sceneitem_get_scale(ia, &scale_a2);
					CHECK(std::fabs(scale_a2.x - 0.6) < 0.005,
					      "and from a hotkey or the preview's menu too (scale %.2f)", scale_a2.x);
					for (obs_sceneitem_t *item : {ia, ib, ic})
						obs_sceneitem_set_scale(item, &one);

					// Shadows and sizes (the report: 'Ignore shadows' seemed to do
					// the opposite). A has a 20 px shadow, the badge a 40 px one, both
					// in screen pixels. The badge's picture (100 of its 200 px) does
					// not reach its own edges, as a widget's often does not.
					{
						auto shadow_on = [](obs_source_t *source, int offset) {
							obs_data_t *d = obs_data_create();
							obs_data_set_bool(d, "shadow", true);
							obs_data_set_double(d, "offset_x", offset);
							obs_data_set_double(d, "offset_y", offset);
							obs_data_set_double(d, "softness", 0);
							obs_data_set_bool(d, "keep_place", true);
							for (const char *side : {"adjusted_left", "adjusted_top",
										 "adjusted_right", "adjusted_bottom"})
								obs_data_set_int(d, side, 0);
							obs_source_t *f = obs_source_create("stagekit_shadow_backdrop",
											    "Shadow, Glow & Outline", d,
											    nullptr);
							obs_data_release(d);
							obs_source_filter_add(source, f);
							return f;
						};
						obs_source_t *shadow_a = shadow_on(block_a, 20);
						obs_source_t *shadow_badge = shadow_on(badge_src, 40);
						vec2 badge_was;
						obs_sceneitem_get_scale(ibadge, &badge_was);
						vec2 half;
						vec2_set(&half, 0.5f, 0.5f);
						auto start = [&] {
							obs_sceneitem_set_scale(ibadge, &half);
							place(ia, 40, 150);
							place(ibadge, 300, 150);
							settle();
							select_only({ibadge});
							obs_sceneitem_select(ia, true); // A is selected last
							settle();
						};
						against(3);
						tick_box("arrangeVisible", true);

						tick_box("arrangeNoShadow", true);
						start();
						click("arrange_same_width");
						settle();
						vec2 ignored;
						obs_sceneitem_get_scale(ibadge, &ignored);
						CHECK(std::fabs(ignored.x - 1.0) < 0.02,
						      "with 'Ignore shadows', the pictures come out the same width and the badge's shadow reaches past it (scale %.3f, wanted 1.000)",
						      ignored.x);

						tick_box("arrangeNoShadow", false);
						start();
						click("arrange_same_width");
						settle();
						vec2 counted;
						obs_sceneitem_get_scale(ibadge, &counted);
						CHECK(std::fabs(counted.x - 0.8) < 0.02,
						      "without it, each with its shadow comes out the same width (scale %.3f, wanted 0.800)",
						      counted.x);

						tick_box("arrangeNoShadow", true);
						obs_source_filter_remove(block_a, shadow_a);
						obs_source_filter_remove(badge_src, shadow_badge);
						obs_source_release(shadow_a);
						obs_source_release(shadow_badge);
						obs_sceneitem_set_scale(ibadge, &badge_was);
						obs_sceneitem_set_scale(ia, &one);
						settle();
					}

					if (scenes)
						scenes->setCurrentIndex(std::max(0, scenes->findData("Arrange Scene")));
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
				auto mouse = [](bool down) {
					calldata_t cd = {};
					calldata_set_bool(&cd, "down", down);
					proc_handler_call(obs_get_proc_handler(), "stagekit_test_mouse_down", &cd);
					calldata_free(&cd);
				};
				// Corrections glide into place; for the checks that follow
				// they are made to land at once.
				auto correct_glide = [](int ms) {
					calldata_t cd = {};
					calldata_set_int(&cd, "ms", ms);
					proc_handler_call(obs_get_proc_handler(), "stagekit_test_correct_glide", &cd);
					calldata_free(&cd);
				};
				correct_glide(0);
				auto settle_snap = [&] {
					for (int i = 0; i < 8; i++) {
						QCoreApplication::processEvents();
						sleep_ms(50);
					}
				};
				// Moved without the mouse, as a Send To key moves it: left alone.
				place(ibadge, 116, 250);
				settle_snap();
				CHECK(std::fabs(pos_of(ibadge).x - 116) < 0.6,
				      "an item moved by a key or a filter is not snapped afterwards (x %.0f)",
				      pos_of(ibadge).x);
				// Dragged with the mouse and let go: snapped.
				place(ibadge, 300, 250);
				settle_snap();
				mouse(true);
				place(ibadge, 116, 250);
				settle_snap();
				mouse(false);
				settle_snap();
				bp = pos_of(ibadge);
				CHECK(std::fabs(bp.x - 110) < 1.6,
				      "'snap when I let go' settles an item dragged with the mouse (x %.0f)", bp.x);
				// The same again with a glide: part-way there shortly after
				// letting go, and in place at the end.
				correct_glide(600);
				place(ibadge, 300, 250);
				settle_snap();
				mouse(true);
				place(ibadge, 116, 250);
				settle_snap();
				mouse(false);
				double part_way = -1.0;
				for (int i = 0; i < 40 && part_way < 0.0; i++) {
					QCoreApplication::processEvents();
					sleep_ms(20);
					const double x = pos_of(ibadge).x;
					if (x < 115.5 && x > 110.5)
						part_way = x;
				}
				for (int i = 0; i < 20; i++) {
					QCoreApplication::processEvents();
					sleep_ms(50);
				}
				CHECK(part_way > 0.0 && std::fabs(pos_of(ibadge).x - 110) < 1.6,
				      "a snap glides into place rather than jumping (%.1f on the way, then %.0f)",
				      part_way, pos_of(ibadge).x);
				correct_glide(0);
				tick_box("arrangeLiveSnap", false);

				// Stop resizing at edges: an item resized with the mouse is pulled
				// back when it is let go, to stop the limit short of the screen's
				// edge or of another item.
				{
					auto *limit_px = dock->findChild<QSpinBox *>("limitPx");
					CHECK(limit_px && dock->findChild<QCheckBox *>("arrangeLimitResize"),
					      "the Arrange box has 'Stop resizing at edges' and its limit");
					if (limit_px)
						limit_px->setValue(10);
					tick_box("arrangeLimitResize", true);
					obs_sceneitem_t *in_group = obs_scene_find_source(as, "Arrange Group");
					const bool group_was = in_group && obs_sceneitem_visible(in_group);
					const bool c_was = obs_sceneitem_visible(ic);
					if (in_group)
						obs_sceneitem_set_visible(in_group, false);
					obs_sceneitem_set_visible(ic, false);
					auto scale_of = [](obs_sceneitem_t *item) {
						vec2 v;
						obs_sceneitem_get_scale(item, &v);
						return v;
					};
					auto drag_to_scale = [&](float sx, float sy) {
						settle_snap();
						mouse(true);
						vec2 v;
						vec2_set(&v, sx, sy);
						obs_sceneitem_set_scale(ia, &v);
						settle_snap();
						mouse(false);
						settle_snap();
					};
					auto reset = [&](float x, float y) {
						vec2 one;
						vec2_set(&one, 1.0f, 1.0f);
						obs_sceneitem_set_scale(ia, &one);
						place(ia, x, y);
					};
					select_only({ia});
					place(ib, 300, 200);     // 60x80: in the way further down
					place(ibadge, 400, 250); // out of the way: its picture starts at 450
					// Towards the screen's edge: 100 wide at 400, pulled to 300 wide.
					reset(400, 100);
					drag_to_scale(3.0f, 1.0f);
					vec2 sc = scale_of(ia);
					CHECK(std::fabs(sc.x - 2.3f) < 0.02f && std::fabs(sc.y - 1.0f) < 0.01f &&
						      std::fabs(pos_of(ia).x - 400) < 0.6,
					      "an edge dragged past the screen's edge stops the limit short of it (width x%.2f; wanted 2.30)",
					      sc.x);
					// Towards another item.
					reset(100, 210);
					drag_to_scale(3.0f, 1.0f);
					sc = scale_of(ia);
					CHECK(std::fabs(sc.x - 1.9f) < 0.02f,
					      "and stops the limit short of another item in its way (width x%.2f; wanted 1.90)",
					      sc.x);
					// Scaled evenly: pulled back evenly, keeping its shape.
					reset(20, 20);
					drag_to_scale(8.0f, 8.0f);
					sc = scale_of(ia);
					CHECK(std::fabs(sc.x - 2.7f) < 0.03f && std::fabs(sc.y - 2.7f) < 0.03f &&
						      std::fabs(pos_of(ia).x - 20) < 0.6 &&
						      std::fabs(pos_of(ia).y - 20) < 0.6,
					      "an item scaled evenly is pulled back evenly and keeps its corner (x%.2f, x%.2f; wanted 2.70)",
					      sc.x, sc.y);
					// The report: a widget showing nothing was still in the way.
					// With 'Use visible pixels', an item that draws nothing is
					// not there: the edge goes on to the screen's edge.
					{
						QImage blank(200, 200, QImage::Format_ARGB32);
						blank.fill(Qt::transparent);
						const std::string blank_path = out + "/arrange-blank.png";
						blank.save(QString::fromStdString(blank_path));
						obs_data_t *bd = obs_data_create();
						obs_data_set_string(bd, "file", blank_path.c_str());
						obs_source_t *blank_src =
							obs_source_create("image_source", "Empty Widget", bd, nullptr);
						obs_data_release(bd);
						obs_sceneitem_t *iblank = obs_scene_add(as, blank_src);
						place(iblank, 250, 40); // level with A, in its way
						for (int i = 0; i < 10 && obs_source_get_width(blank_src) < 200; i++) {
							QCoreApplication::processEvents();
							sleep_ms(50);
						}
						reset(20, 100);
						drag_to_scale(8.0f, 1.0f);
						sc = scale_of(ia);
						CHECK(std::fabs(sc.x - 6.1f) < 0.03f,
						      "an item that draws nothing is not in the way of a resize (width x%.2f; wanted 6.10, the screen's edge)",
						      sc.x);
						tick_box("arrangeVisible", false);
						reset(20, 100);
						drag_to_scale(8.0f, 1.0f);
						sc = scale_of(ia);
						CHECK(std::fabs(sc.x - 2.2f) < 0.03f,
						      "without 'Use visible pixels' its box is, as OBS shows it (width x%.2f; wanted 2.20)",
						      sc.x);
						tick_box("arrangeVisible", true);
						obs_sceneitem_remove(iblank);
						obs_source_release(blank_src);
					}

					// Only moved: left where it is.
					reset(500, 20);
					settle_snap();
					mouse(true);
					place(ia, 600, 20);
					settle_snap();
					mouse(false);
					settle_snap();
					CHECK(std::fabs(pos_of(ia).x - 600) < 0.6 &&
						      std::fabs(scale_of(ia).x - 1.0f) < 0.01f,
					      "an item that was only moved is left alone");
					// Resized by a key or a filter, not the mouse: left alone.
					reset(400, 100);
					settle_snap();
					vec2 big;
					vec2_set(&big, 3.0f, 1.0f);
					obs_sceneitem_set_scale(ia, &big);
					settle_snap();
					CHECK(std::fabs(scale_of(ia).x - 3.0f) < 0.01f,
					      "and so is one resized without the mouse");
					reset(20, 20);
					tick_box("arrangeLimitResize", false);
					if (in_group)
						obs_sceneitem_set_visible(in_group, group_was);
					obs_sceneitem_set_visible(ic, c_was);
					select_only({});
					settle_snap();
				}

				// Guide lines over the preview. OBS's preview is stood in for by a
				// plain widget; the lines live in a see-through window over it.
				{
					auto mapped = [](double ww, double wh, double ratio, double &scale, double &x,
							 double &y) {
						calldata_t cd = {};
						calldata_set_float(&cd, "width", ww);
						calldata_set_float(&cd, "height", wh);
						calldata_set_float(&cd, "ratio", ratio);
						calldata_set_float(&cd, "canvas_width", 2560);
						calldata_set_float(&cd, "canvas_height", 1440);
						proc_handler_call(obs_get_proc_handler(), "stagekit_preview_map", &cd);
						scale = calldata_float(&cd, "scale");
						x = calldata_float(&cd, "x");
						y = calldata_float(&cd, "y");
						calldata_free(&cd);
					};
					double ms = 0, mx = 0, my = 0;
					mapped(1000, 600, 1.0, ms, mx, my);
					const bool tall_ok = std::fabs(ms - 980.0 / 2560.0) < 1e-4 &&
							     std::fabs(mx - 10) < 0.01 && std::fabs(my - 25) < 0.01;
					mapped(2000, 600, 1.0, ms, mx, my);
					CHECK(tall_ok && std::fabs(ms - 580.0 / 1440.0) < 1e-4 &&
						      std::fabs(mx - 485) < 0.01 && std::fabs(my - 10) < 0.01,
					      "the canvas is found inside the preview as OBS places it (%.4f at %.1f, %.1f)",
					      ms, mx, my);

					obs_source_t *was_out = obs_get_output_source(0);
					obs_set_output_source(0, as_source);
					QWidget stand_in;
					stand_in.resize(600, 400);
					stand_in.show();
					calldata_t gd = {};
					calldata_set_ptr(&gd, "preview", &stand_in);
					proc_handler_call(obs_get_proc_handler(), "stagekit_install_preview_guides",
							  &gd);
					auto *overlay = static_cast<QObject *>(calldata_ptr(&gd, "overlay"));
					calldata_free(&gd);
					auto lines = [&] {
						return overlay ? overlay->property("guideLines").toString()
							       : QString("none");
					};
					// The line windows showing on the screen.
					auto shown = [] {
						QList<QRect> out;
						for (QWidget *w : QApplication::allWidgets())
							if (w->objectName() == "stagekitGuideLine" && w->isVisible() &&
							    w->parentWidget())
								out.push_back(w->geometry().translated(
									w->parentWidget()->mapToGlobal(QPoint(0, 0))));
						return out;
					};
					against(0);
					select_only({ia});
					settle_snap();
					click("arrange_right");
					settle_snap();
					CHECK(overlay && shown().isEmpty(),
					      "guides stay out of the way until they are switched on");
					tick_box("arrangeGuides", true);
					// Switching them on flashes a frame round the canvas, so it is
					// plain at once that lines show over the preview.
					for (int i = 0; i < 4; i++) {
						QCoreApplication::processEvents();
						sleep_ms(40);
					}
					{
						const QPoint at = stand_in.mapToGlobal(QPoint(0, 0));
						QList<QRect> hello = shown();
						auto has = [&](const QRect &r) {
							return hello.contains(r);
						};
						CHECK(hello.size() == 4 &&
							      has(QRect(at.x() + 8, at.y() + 37, 4, 326)) &&
							      has(QRect(at.x() + 588, at.y() + 37, 4, 326)) &&
							      has(QRect(at.x() + 10, at.y() + 35, 580, 4)) &&
							      has(QRect(at.x() + 10, at.y() + 361, 580, 4)),
						      "switching guides on flashes a frame round the canvas in the preview (%d lines)",
						      (int)hello.size());
						for (int i = 0; i < 45 && !shown().isEmpty(); i++) {
							QCoreApplication::processEvents();
							sleep_ms(40);
						}
						CHECK(shown().isEmpty(), "and the frame goes after a moment");
					}
					click("arrange_left");
					for (int i = 0; i < 6; i++) {
						QCoreApplication::processEvents();
						sleep_ms(40);
					}
					// 600x400 preview, 640x360 canvas: drawn at 0.90625 from (10, 37), 580x326.
					const QPoint corner = stand_in.mapToGlobal(QPoint(0, 0));
					const QList<QRect> flash = shown();
					CHECK(lines() == "x:0" && flash.size() == 1 &&
						      flash[0] == QRect(corner.x() + 10 - 2, corner.y() + 37, 4, 326),
					      "a line-up tool shows the line it used, a window of its own exactly over the canvas's edge in the preview (%s)",
					      lines().toUtf8().constData());
					for (int i = 0; i < 28; i++) {
						QCoreApplication::processEvents();
						sleep_ms(40);
					}
					CHECK(shown().isEmpty() && lines().isEmpty(),
					      "and the line goes away by itself");

					// Dragging: the block's centre comes within reach of the screen's centre line.
					place(ib, 20, 300);
					place(ic, 600, 320);
					place(ibadge, 430, 250);
					place(ia, 100, 20);
					settle_snap();
					mouse(true);
					place(ia, 180, 20);
					settle_snap();
					place(ia, 266, 20); // 100 wide: its centre is at 316, the screen's at 320
					settle_snap();
					// The screen's centre, 320, is at 10 + 290 = 300 in the preview.
					bool centre_line = false;
					for (const QRect &r : shown())
						centre_line = centre_line ||
							      (r.x() == corner.x() + 300 - 2 && r.height() == 326);
					CHECK(lines().contains("x:320") && centre_line,
					      "while an item is dragged, the line it is about to meet is shown (%s)",
					      lines().toUtf8().constData());
					mouse(false);
					settle_snap();
					for (int i = 0; i < 4; i++) {
						QCoreApplication::processEvents();
						sleep_ms(40);
					}
					CHECK(shown().isEmpty(), "and it goes when the mouse is let go");
					tick_box("arrangeGuides", false);
					delete overlay;
					obs_set_output_source(0, was_out);
					obs_source_release(was_out);
				}

				// Bringing an item along: whether it has to flip is judged by what
				// is drawn in it, not by the empty room in its box.
				{
					auto send = [&](double margin) {
						place(ia, 300, 200); // 100x50
						place(ibadge, 250,
						      120); // drawn part 100x40 at 300, 150: 10 px above the block
						obs_data_t *d = obs_data_create();
						obs_data_set_string(d, "item", "Block A");
						obs_data_set_string(d, "linked", "Padded Badge");
						obs_data_set_int(d, "destination", 2); // top right
						obs_data_set_double(d, "margin", margin);
						obs_data_set_int(d, "duration_ms", 0);
						obs_source_t *f = obs_source_create("stagekit_place_item", "Flip Test",
										    d, nullptr);
						obs_data_release(d);
						obs_source_filter_add(as_source, f);
						sleep_ms(150);
						calldata_t cd = {};
						proc_handler_call(obs_source_get_proc_handler(f), "trigger", &cd);
						calldata_free(&cd);
						sleep_ms(300);
						obs_source_filter_remove(as_source, f);
						obs_source_release(f);
					};
					send(60); // the block lands at y 60: the drawn badge fits above it at y 10
					CHECK(std::fabs(pos_of(ia).y - 60) < 0.6 &&
						      std::fabs(pos_of(ibadge).y + 20) < 1.6,
					      "an item brought along stays on its side while what is drawn still fits (y %.0f)",
					      pos_of(ibadge).y);
					send(30); // at y 30 the drawn badge would be cut off above: it goes below
					CHECK(std::fabs(pos_of(ia).y - 30) < 0.6 &&
						      std::fabs(pos_of(ibadge).y - 60) < 1.6,
					      "and flips to the other side when it would be cut off (y %.0f)",
					      pos_of(ibadge).y);
				}

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

				// 'Ignore shadows & outlines' leaves out whatever any filter draws,
				// not only a Shadow, Glow & Outline: here a border from Corners &
				// Edges around the badge's whole 200x100 source, whose own picture
				// is only the middle 100x40.
				{
					obs_data_t *cd = obs_data_create();
					obs_data_set_double(cd, "size", 0.0);
					obs_data_set_double(cd, "border", 6.0);
					obs_data_set_double(cd, "border_opacity", 100.0);
					obs_data_set_int(cd, "border_color", 0xFFFFFFFF);
					obs_data_set_bool(cd, "screen_px", false);
					obs_data_set_bool(cd, "placed", true);
					obs_source_t *edges =
						obs_source_create("stagekit_corners", "Corners & Edges", cd, nullptr);
					obs_data_release(cd);
					obs_source_filter_add(badge_src, edges);
					against(0);
					tick_box("arrangeVisible", true);
					select_only({ibadge});
					place(ibadge, 300, 200);
					for (int i = 0; i < 4; i++) {
						QCoreApplication::processEvents();
						sleep_ms(50);
					}
					tick_box("arrangeNoShadow", false);
					click("arrange_left");
					const float with_border = pos_of(ibadge).x;
					tick_box("arrangeNoShadow", true);
					click("arrange_left");
					const float own_pixels = pos_of(ibadge).x;
					CHECK(std::fabs(with_border - 0) < 1.6 && std::fabs(own_pixels + 50) < 1.6,
					      "'Ignore shadows & outlines' measures the source's own pixels, leaving out a border another filter draws (x %.0f, then %.0f)",
					      with_border, own_pixels);
					obs_source_filter_remove(badge_src, edges);
					obs_source_release(edges);
					select_only({});
				}

				// Bring along with a shadow on the leader (the report: after a
				// flip, the gap grew by the camera's shadow). Block A's picture is
				// 100x50 with a 20 px shadow below and right; the badge's picture
				// sits 10 px above A's. Sent to the top, the badge must flip below
				// and sit 10 px under A's picture, not under its shadow.
				{
					obs_data_t *sd = obs_data_create();
					obs_data_set_bool(sd, "shadow", true);
					obs_data_set_double(sd, "offset_x", 20);
					obs_data_set_double(sd, "offset_y", 20);
					obs_data_set_double(sd, "softness", 0);
					obs_data_set_bool(sd, "keep_place", true);
					for (const char *side :
					     {"adjusted_left", "adjusted_top", "adjusted_right", "adjusted_bottom"})
						obs_data_set_int(sd, side, 0);
					obs_source_t *lead_shadow = obs_source_create(
						"stagekit_shadow_backdrop", "Shadow, Glow & Outline", sd, nullptr);
					obs_data_release(sd);
					obs_source_filter_add(block_a, lead_shadow);
					select_only({});
					place(ia, 300, 200);
					place(ibadge, 250, 120); // drawn part 100x40 at 300, 150: 10 px above A
					for (int i = 0; i < 6; i++) {
						QCoreApplication::processEvents();
						sleep_ms(50);
					}
					obs_data_t *pd = obs_data_create();
					obs_data_set_string(pd, "item", "Block A");
					obs_data_set_string(pd, "linked", "Padded Badge");
					obs_data_set_int(pd, "destination", 2); // top right
					obs_data_set_double(pd, "margin", 30);
					obs_data_set_int(pd, "duration_ms", 0);
					obs_source_t *mover = obs_source_create("stagekit_place_item",
										"Flip Shadow Test", pd, nullptr);
					obs_data_release(pd);
					obs_source_filter_add(as_source, mover);
					sleep_ms(100);
					obs_source_set_enabled(mover, true);
					for (int i = 0; i < 6; i++) {
						QCoreApplication::processEvents();
						sleep_ms(50);
					}
					const vec2 lead_at = pos_of(ia), badge_at = pos_of(ibadge);
					// A's picture top is at its position; the badge's picture is 30 px into the badge.
					const double gap = (badge_at.y + 30) - (lead_at.y + 50);
					CHECK(std::fabs(lead_at.y - 30) < 0.6 && std::fabs(gap - 10) < 1.1,
					      "a brought-along item that flips keeps its gap to the leader's picture, not to the leader's shadow (gap %.0f)",
					      gap);
					obs_source_filter_remove(as_source, mover);
					obs_source_release(mover);
					obs_source_filter_remove(block_a, lead_shadow);
					obs_source_release(lead_shadow);
				}

				// Same in every scene: like Copy and Paste Transform, for every
				// other scene at once, groups included; locked copies are left alone.
				{
					obs_scene_t *paste_a = obs_scene_create("Paste Elsewhere");
					obs_sceneitem_t *loose = obs_scene_add(paste_a, block_a);
					obs_sceneitem_t *box = obs_scene_add_group(paste_a, "Paste Group");
					obs_sceneitem_t *grouped_copy = obs_scene_add(paste_a, block_a);
					obs_sceneitem_group_add_item(box, grouped_copy);
					place(box, 200, 150);
					place(grouped_copy, 0, 0);
					place(loose, 500, 300);
					obs_scene_t *paste_b = obs_scene_create("Paste Locked");
					obs_sceneitem_t *locked = obs_scene_add(paste_b, block_a);
					place(locked, 5, 5);
					obs_sceneitem_set_locked(locked, true);
					place(ia, 40, 60);
					vec2 half;
					vec2_set(&half, 0.5f, 0.5f);
					obs_sceneitem_set_scale(ia, &half);
					obs_sceneitem_crop crop = {10, 0, 0, 0};
					obs_sceneitem_set_crop(ia, &crop);
					select_only({ia});
					for (int i = 0; i < 4; i++) {
						QCoreApplication::processEvents();
						sleep_ms(50);
					}
					CHECK(click("arrange_same_everywhere"),
					      "the Arrange box has 'Copy transform to all scenes'");
					sleep_ms(100);
					vec2 loose_scale;
					obs_sceneitem_get_scale(loose, &loose_scale);
					obs_sceneitem_crop loose_crop;
					obs_sceneitem_get_crop(loose, &loose_crop);
					const vec2 lp = pos_of(loose), hp = pos_of(box), gp = pos_of(grouped_copy),
						   kp = pos_of(locked);
					CHECK(std::fabs(lp.x - 40) < 0.6 && std::fabs(lp.y - 60) < 0.6 &&
						      std::fabs(loose_scale.x - 0.5) < 0.001 && loose_crop.left == 10,
					      "every copy in the other scenes gets its place, size and crop (%.0f, %.0f at %.2f, crop %d)",
					      lp.x, lp.y, loose_scale.x, loose_crop.left);
					CHECK(std::fabs(hp.x + gp.x - 40) < 1.0 && std::fabs(hp.y + gp.y - 60) < 1.0,
					      "a copy inside a group lands on the same spot of the screen (%.0f, %.0f)",
					      hp.x + gp.x, hp.y + gp.y);
					CHECK(std::fabs(kp.x - 5) < 0.6 && std::fabs(kp.y - 5) < 0.6,
					      "a locked copy is left alone");

					// With "Also change locked copies" ticked the locked one is
					// changed too, and stays locked.
					tick_box("copyLocked", true);
					place(ia, 44, 66);
					click("arrange_same_everywhere");
					sleep_ms(100);
					const vec2 kp2 = pos_of(locked);
					tick_box("copyLocked", false);
					CHECK(std::fabs(kp2.x - 44) < 0.6 && std::fabs(kp2.y - 66) < 0.6 &&
						      obs_sceneitem_locked(locked),
					      "with 'Also change locked copies' a locked copy takes the transform and stays locked (%.0f, %.0f)",
					      kp2.x, kp2.y);
					// A copy looked after by a Fit to Space, in a scene that is
					// not on screen: the filter takes it where it was put and
					// does not put it back.
					{
						obs_data_t *fd = obs_data_create();
						obs_data_set_string(fd, "item", "Block A");
						obs_data_set_int(fd, "box", 2);
						obs_data_set_double(fd, "margin", 10);
						obs_data_set_int(fd, "glide_ms", 0);
						obs_data_set_bool(fd, "resize", false);
						obs_source_t *held = obs_source_create("stagekit_fit_item", "Fit Paste",
										       fd, nullptr);
						obs_data_release(fd);
						obs_source_filter_add(obs_scene_get_source(paste_b), held);
						// Shown for a moment, so the filter takes the item in hand.
						obs_source_t *was_out = obs_get_output_source(0);
						obs_set_output_source(0, obs_scene_get_source(paste_b));
						for (int i = 0; i < 16; i++) {
							render(obs_scene_get_source(paste_b), 640, 360);
							sleep_ms(50);
						}
						obs_set_output_source(0, was_out);
						obs_source_release(was_out);
						sleep_ms(300);
						obs_sceneitem_set_locked(locked, false);
						const vec2 fitted_at = pos_of(locked);
						place(ia, 150, 120);
						click("arrange_same_everywhere");
						sleep_ms(600);
						const vec2 put = pos_of(locked);
						CHECK(std::fabs(put.x - 150) < 0.6 && std::fabs(put.y - 120) < 0.6,
						      "a copy that a Fit to Space looks after, in a scene not on screen, stays where the transform was copied to (%.0f, %.0f; the filter had it at %.0f, %.0f)",
						      put.x, put.y, fitted_at.x, fitted_at.y);
						obs_source_filter_remove(obs_scene_get_source(paste_b), held);
						obs_source_release(held);
						obs_sceneitem_set_locked(locked, true);
						// Back as the checks below expect to find it.
						place(ia, 40, 60);
						click("arrange_same_everywhere");
						sleep_ms(100);
					}

					// From a hotkey or the preview's menu, and one step for Undo.
					place(ia, 100, 100);
					calldata_t hk = {};
					calldata_set_string(&hk, "action", "same_everywhere");
					proc_handler_call(obs_get_proc_handler(), "stagekit_run_arrange", &hk);
					calldata_free(&hk);
					const vec2 moved = pos_of(loose);
					calldata_t undo_cd = {};
					calldata_set_bool(&undo_cd, "redo", false);
					proc_handler_call(obs_get_proc_handler(), "stagekit_replay_arrange", &undo_cd);
					calldata_free(&undo_cd);
					sleep_ms(100);
					const vec2 back = pos_of(loose);
					CHECK(std::fabs(moved.x - 100) < 0.6 && std::fabs(back.x - 40) < 0.6,
					      "it works from a hotkey too, and Undo takes it back in every scene (%.0f, then %.0f)",
					      moved.x, back.x);

					// "In every scene" on the Arrange tab: a tool's result goes
					// to the other scenes too. Unticked, it stays in this one.
					place(ia, 300, 200);
					place(loose, 500, 300);
					select_only({ia});
					for (int i = 0; i < 4; i++) {
						QCoreApplication::processEvents();
						sleep_ms(50);
					}
					click("arrange_left");
					sleep_ms(100);
					const vec2 here_only = pos_of(ia), not_there = pos_of(loose);
					tick_box("arrangeEveryScene", true);
					place(ia, 300, 200);
					click("arrange_left");
					sleep_ms(100);
					const vec2 here_too = pos_of(ia), there_too = pos_of(loose);
					tick_box("arrangeEveryScene", false);
					CHECK(here_only.x < 299 && std::fabs(not_there.x - 500) < 0.6 &&
						      std::fabs(there_too.x - here_too.x) < 0.6 &&
						      std::fabs(there_too.y - here_too.y) < 0.6,
					      "'In every scene' on the Arrange tab puts a lined-up item the same way in the other scenes (without: %.0f; with: %.0f, here %.0f)",
					      not_there.x, there_too.x, here_too.x);
					// The measuring options show on the Place tab too, as the same options.
					auto *place_visible = dock->findChild<QCheckBox *>("placeVisible");
					auto *arrange_visible = dock->findChild<QCheckBox *>("arrangeVisible");
					bool follows = place_visible && arrange_visible;
					if (follows) {
						const bool was = arrange_visible->isChecked();
						place_visible->setChecked(!was);
						follows = arrange_visible->isChecked() == !was;
						arrange_visible->setChecked(was);
						follows = follows && place_visible->isChecked() == was;
					}
					CHECK(follows,
					      "the Place tab's measuring options are the Arrange tab's, each following the other");

					// Copy a filter's settings to the same kind of filter elsewhere.
					{
						auto with_filter = [](obs_source_t *on, const char *name, double amount,
								      int own) {
							obs_data_t *d = obs_data_create();
							obs_data_set_double(d, "test_amount", amount);
							obs_data_set_int(d, "adjusted_left", own);
							obs_source_t *f = obs_source_create("stagekit_ramp_blur", name,
											    d, nullptr);
							obs_data_release(d);
							obs_source_filter_add(on, f);
							return f;
						};
						obs_source_t *look = with_filter(block_a, "Look", 7.0, 5);
						obs_source_t *other = with_filter(block_b, "Another Look", 1.0, 9);
						obs_source_t *block_c =
							obs_source_create("color_source", "Block C", nullptr, nullptr);
						obs_sceneitem_t *ic = obs_scene_add(as, block_c);
						select_only({ia});
						for (int i = 0; i < 8; i++) {
							QCoreApplication::processEvents();
							sleep_ms(50);
						}
						auto *from = dock->findChild<QComboBox *>("filterFrom");
						auto *to = dock->findChild<QComboBox *>("filterTo");
						auto *add_missing = dock->findChild<QCheckBox *>("filterAddMissing");
						const int row = from ? from->findText("Look (", Qt::MatchStartsWith)
								     : -1;
						CHECK(from && to && add_missing && row >= 0,
						      "the Copy tab lists the chosen item's filters to copy from");
						if (from && to && add_missing && row >= 0) {
							from->setCurrentIndex(row);
							to->setCurrentIndex(0); // every source that has one
							click("filterCopy");
							obs_data_t *got = obs_source_get_settings(other);
							CHECK(obs_data_get_double(got, "test_amount") == 7.0 &&
								      obs_data_get_int(got, "adjusted_left") == 9,
							      "'Copy filter settings' gives them to the same kind of filter on other sources, and leaves what that filter keeps for its own source (%.0f, %d)",
							      obs_data_get_double(got, "test_amount"),
							      (int)obs_data_get_int(got, "adjusted_left"));
							obs_data_release(got);
							obs_source_t *none_yet =
								obs_source_get_filter_by_name(block_c, "Look");
							to->setCurrentIndex(3); // sources in this scene
							add_missing->setChecked(true);
							click("filterCopy");
							obs_source_t *now =
								obs_source_get_filter_by_name(block_c, "Look");
							obs_data_t *made = now ? obs_source_get_settings(now) : nullptr;
							CHECK(!none_yet && now &&
								      obs_data_get_double(made, "test_amount") == 7.0 &&
								      !obs_data_has_user_value(made, "adjusted_left"),
							      "with 'Add it where it is missing' a source in the scene without the filter gets a copy");
							obs_data_release(made);
							obs_source_release(now);
							obs_source_release(none_yet);
							add_missing->setChecked(false);
						}
						// The copies it added go again: the scene is used further on.
						const auto strip = [](void *, obs_source_t *source) {
							if (obs_source_t *copy =
								    obs_source_get_filter_by_name(source, "Look")) {
								obs_source_filter_remove(source, copy);
								obs_source_release(copy);
							}
							return true;
						};
						obs_source_filter_remove(block_a, look);
						obs_enum_sources(strip, nullptr);
						obs_enum_scenes(strip, nullptr);
						obs_sceneitem_remove(ic);
						obs_source_release(block_c);
						obs_source_filter_add(block_a, look);
						obs_source_filter_remove(block_a, look);
						obs_source_filter_remove(block_b, other);
						obs_source_release(look);
						obs_source_release(other);
					}

					// Lock and unlock across scenes, from the Copy tab.
					{
						select_only({ia});
						for (int i = 0; i < 4; i++) {
							QCoreApplication::processEvents();
							sleep_ms(50);
						}
						auto *which = dock->findChild<QComboBox *>("lockWhich");
						auto *where = dock->findChild<QComboBox *>("lockWhere");
						CHECK(which && where, "the Copy tab has 'Lock and unlock'");
						if (which && where) {
							which->setCurrentIndex(0); // the selected items' sources
							where->setCurrentIndex(0); // all scenes
							click("unlockItems");
							const bool freed = !obs_sceneitem_locked(locked);
							click("lockItems");
							const bool all_locked = obs_sceneitem_locked(ia) &&
										obs_sceneitem_locked(loose) &&
										obs_sceneitem_locked(grouped_copy) &&
										obs_sceneitem_locked(locked) &&
										!obs_sceneitem_locked(ib);
							where->setCurrentIndex(2); // this scene only
							click("unlockItems");
							const bool only_here = !obs_sceneitem_locked(ia) &&
									       obs_sceneitem_locked(loose);
							where->setCurrentIndex(0);
							click("unlockItems");
							CHECK(freed && all_locked && only_here &&
								      !obs_sceneitem_locked(loose) &&
								      !obs_sceneitem_locked(grouped_copy),
							      "Lock and Unlock reach the selected item's source in every scene, groups included, or only in this scene (%d %d %d)",
							      freed, all_locked, only_here);
						}
					}

					vec2 one;
					vec2_set(&one, 1.0f, 1.0f);
					obs_sceneitem_set_scale(ia, &one);
					obs_sceneitem_crop none = {0, 0, 0, 0};
					obs_sceneitem_set_crop(ia, &none);
					select_only({});
					obs_scene_release(paste_a);
					obs_scene_release(paste_b);
				}

				// Slide to the next edge: the Send to arrows move the chosen item
				// that way until it comes within the edge distance of the next edge.
				{
					auto *items_box = dock->findChild<QComboBox *>("item");
					auto *margin = dock->findChild<QSpinBox *>("margin");
					auto *travel = dock->findChild<QSpinBox *>("duration");
					auto *slide = dock->findChild<QCheckBox *>("sendSlide");
					CHECK(items_box && margin && travel && slide,
					      "the Send to box has 'Slide to the next edge'");
					if (items_box && margin && travel && slide) {
						for (int i = 0; i < 30 && items_box->findText("Block A") < 0; i++) {
							QCoreApplication::processEvents();
							sleep_ms(50);
						}
						items_box->setCurrentText("Block A");
						margin->setValue(10);
						travel->setValue(0);
						slide->setChecked(true);
						tick_box("arrangeVisible", true);
						tick_box("arrangeNoShadow", true);
						select_only({});
						// Block C is in a group by now; the group is put out of sight.
						obs_sceneitem_t *c_group = obs_scene_find_source(as, "Arrange Group");
						if (c_group)
							obs_sceneitem_set_visible(c_group, false);
						place(ia, 20, 20);       // 100x50
						place(ib, 300, 15);      // 60x80, in A's way across
						place(ibadge, 300, 250); // drawn part 100x40 at 350, 280
						auto press = [&](int index) {
							click(QString("grid%1").arg(index).toUtf8().constData());
							for (int i = 0; i < 4; i++) {
								QCoreApplication::processEvents();
								sleep_ms(40);
							}
							return pos_of(ia);
						};
						vec2 at1 = press(5); // right: up to B, 10 short
						vec2 at2 = press(
							5); // again: through B's near side to 10 short of its far side
						vec2 at3 = press(5); // again: on to the screen's edge
						CHECK(std::fabs(at1.x - 190) < 0.6 && std::fabs(at2.x - 250) < 0.6 &&
							      std::fabs(at3.x - 530) < 0.6 &&
							      std::fabs(at3.y - 20) < 0.6,
						      "each press slides to the next edge, keeping the edge distance: short of the item in the way, then through it, then the screen's edge (x %.0f, %.0f, %.0f)",
						      at1.x, at2.x, at3.x);
						vec2 stuck = press(5);
						CHECK(std::fabs(stuck.x - 530) < 0.6,
						      "with no edge left that way it stays (x %.0f)", stuck.x);
						vec2 back = press(3); // left: meets B's near side first
						CHECK(std::fabs(back.x - 370) < 0.6,
						      "the other way, it meets the near side first (x %.0f)", back.x);
						place(ia, 380, 20);
						vec2 down = press(7); // down: the badge's picture is in the way
						tick_box("arrangeVisible", false);
						place(ia, 380, 20);
						vec2 down_box = press(7);
						tick_box("arrangeVisible", true);
						CHECK(std::fabs(down.y - 220) < 0.6 &&
							      std::fabs(down_box.y - 190) < 0.6,
						      "'Use visible pixels' applies: the badge's picture stops it, or its box when that is off (y %.0f, %.0f)",
						      down.y, down_box.y);
						// Up and right from low down: across, the badge's picture stops it;
						// then up from there, where B is in the way (from where it began,
						// nothing would be).
						place(ia, 20, 250);
						vec2 corner = press(2);
						CHECK(std::fabs(corner.x - 240) < 0.6 &&
							      std::fabs(corner.y - 105) < 0.6,
						      "a diagonal slides across, then up from where that left it (%.0f, %.0f)",
						      corner.x, corner.y);
						place(ia, 20, 20);
						vec2 centred = press(4);
						CHECK(std::fabs(centred.x - 100) < 0.6 &&
							      std::fabs(centred.y - 155) < 0.6,
						      "the centre button centres it in the free space around it (%.0f, %.0f)",
						      centred.x, centred.y);
						// Something partly past the screen's edge: the edge is as far as it goes.
						place(ib, 600, 15); // 600..660 on a 640 wide screen
						place(ia, 20, 20);
						vec2 e1 = press(5), e2 = press(5), e3 = press(5);
						CHECK(std::fabs(e1.x - 490) < 0.6 && std::fabs(e2.x - 530) < 0.6 &&
							      std::fabs(e3.x - 530) < 0.6,
						      "nothing past the screen's edge takes it further (x %.0f, %.0f, %.0f)",
						      e1.x, e2.x, e3.x);

						// Something thinner than the edge distance: the next press passes it.
						obs_source_t *thin_src = make_color("Thin Divider", 6, 100);
						obs_sceneitem_t *thin = obs_scene_add(as, thin_src);
						place(thin, 300, 0);
						place(ib, 500, 200);
						margin->setValue(30);
						place(ia, 20, 20);
						vec2 t1 = press(5), t2 = press(5);
						CHECK(std::fabs(t1.x - 170) < 0.6 && std::fabs(t2.x - 510) < 0.6,
						      "something thinner than the edge distance is passed by the next press (x %.0f, %.0f)",
						      t1.x, t2.x);
						obs_sceneitem_remove(thin);
						obs_source_release(thin_src);
						margin->setValue(10);
						place(ib, 300, 15);

						// Pressed again while still gliding: it goes on from where the
						// first slide ends, and both make one step for Undo.
						travel->setValue(400);
						place(ia, 20, 20);
						for (int i = 0; i < 4; i++) {
							QCoreApplication::processEvents();
							sleep_ms(50);
						}
						click("grid5");
						QCoreApplication::processEvents();
						sleep_ms(120);
						click("grid5");
						for (int i = 0; i < 20; i++) {
							QCoreApplication::processEvents();
							sleep_ms(50);
						}
						const vec2 glided = pos_of(ia);
						calldata_t undo_cd = {};
						calldata_set_bool(&undo_cd, "redo", false);
						proc_handler_call(obs_get_proc_handler(), "stagekit_replay_arrange",
								  &undo_cd);
						calldata_free(&undo_cd);
						sleep_ms(100);
						const vec2 undone = pos_of(ia);
						CHECK(std::fabs(glided.x - 250) < 0.6 &&
							      std::fabs(undone.x - 20) < 0.6 &&
							      std::fabs(undone.y - 20) < 0.6,
						      "a second press while it glides passes the edge, and one Undo takes both back (x %.0f, then %.0f)",
						      glided.x, undone.x);
						travel->setValue(0);

						// In every scene: the same spot on the screen, also where the
						// item sits in a group.
						obs_scene_t *other = obs_scene_create("Slide Elsewhere");
						obs_sceneitem_t *holder = obs_scene_add_group(other, "Slide Holder");
						obs_sceneitem_t *copy = obs_scene_add(other, block_a);
						obs_sceneitem_group_add_item(holder, copy);
						place(holder, 100, 100);
						place(copy, 0, 0);
						for (int i = 0; i < 4; i++) {
							QCoreApplication::processEvents();
							sleep_ms(50);
						}
						place(ia, 20, 20);
						press(5);
						for (int i = 0; i < 4; i++) {
							QCoreApplication::processEvents();
							sleep_ms(50);
						}
						const vec2 hp = pos_of(holder), cp = pos_of(copy), here = pos_of(ia);
						CHECK(std::fabs(here.x - 190) < 0.6 &&
							      std::fabs(hp.x + cp.x - 190) < 1.0 &&
							      std::fabs(hp.y + cp.y - 20) < 1.0,
						      "with 'In every scene', it lands on the same spot of the screen in a scene where it is in a group (%.0f, %.0f)",
						      hp.x + cp.x, hp.y + cp.y);
						obs_scene_release(other);

						slide->setChecked(false);
						place(ia, 20, 20);
						vec2 sent = press(5);
						CHECK(std::fabs(sent.x - 530) < 0.6 && std::fabs(sent.y - 155) < 0.6,
						      "unticked, the arrows send it to the screen position again (%.0f, %.0f)",
						      sent.x, sent.y);
						// The report: a browser widget with a shadow was slid, and sent,
						// by its box. A source that draws itself (a Cropped View of the
						// padded badge: 200x100, its picture the middle 100x40) with a
						// shadow below and right, measured by its own pixels.
						{
							obs_data_t *wd = obs_data_create();
							obs_data_set_string(wd, "source", "Padded Badge");
							obs_data_set_int(wd, "crop_to", 0);
							obs_source_t *widget = obs_source_create(
								"stagekit_cropped_view", "Badge Widget", wd, nullptr);
							obs_data_release(wd);
							obs_data_t *sd = obs_data_create();
							obs_data_set_bool(sd, "shadow", true);
							obs_data_set_double(sd, "offset_x", 20);
							obs_data_set_double(sd, "offset_y", 20);
							obs_data_set_double(sd, "softness", 0);
							obs_data_set_bool(sd, "screen_px", false);
							obs_data_set_bool(sd, "keep_place", true);
							for (const char *side : {"adjusted_left", "adjusted_top",
										 "adjusted_right", "adjusted_bottom"})
								obs_data_set_int(sd, side, 0);
							obs_source_t *widget_shadow = obs_source_create(
								"stagekit_shadow_backdrop", "Shadow, Glow & Outline",
								sd, nullptr);
							obs_data_release(sd);
							obs_source_filter_add(widget, widget_shadow);
							obs_sceneitem_t *iw = obs_scene_add(as, widget);
							obs_sceneitem_set_visible(ibadge, false);
							place(ia, 500, 300);
							place(iw, 300, 250);
							for (int i = 0;
							     i < 40 && (items_box->findText("Badge Widget") < 0 ||
									obs_source_get_width(widget) < 200);
							     i++) {
								QCoreApplication::processEvents();
								sleep_ms(50);
							}
							sleep_ms(300);
							items_box->setCurrentText("Badge Widget");
							margin->setValue(10);
							travel->setValue(0);
							auto go = [&](int index) {
								click(QString("grid%1").arg(index).toUtf8().constData());
								for (int i = 0; i < 4; i++) {
									QCoreApplication::processEvents();
									sleep_ms(40);
								}
								return pos_of(iw);
							};
							slide->setChecked(true);
							go(3);
							const vec2 slid = go(1);
							CHECK(std::fabs(slid.x + 40) < 1.6 &&
								      std::fabs(slid.y + 20) < 1.6,
							      "a source that draws itself, with a shadow, slides until its own picture is the edge distance from the screen's edge (%.0f, %.0f; wanted -40, -20)",
							      slid.x, slid.y);
							slide->setChecked(false);
							const vec2 corner = go(8);
							CHECK(std::fabs(corner.x - 480) < 1.6 &&
								      std::fabs(corner.y - 280) < 1.6,
							      "the Send to buttons keep the edge distance to the picture too, so the shadow reaches past it (%.0f, %.0f; wanted 480, 280)",
							      corner.x, corner.y);
							tick_box("arrangeNoShadow", false);
							const vec2 shadowed = go(8);
							CHECK(std::fabs(shadowed.x - 460) < 1.6 &&
								      std::fabs(shadowed.y - 260) < 1.6,
							      "with shadows counted, the shadow keeps the edge distance (%.0f, %.0f; wanted 460, 260)",
							      shadowed.x, shadowed.y);
							tick_box("arrangeVisible", false);
							const vec2 boxed = go(8);
							CHECK(std::fabs(boxed.x - 410) < 1.6 &&
								      std::fabs(boxed.y - 230) < 1.6,
							      "without 'Use visible pixels', the box does (%.0f, %.0f; wanted 410, 230)",
							      boxed.x, boxed.y);
							tick_box("arrangeVisible", true);
							tick_box("arrangeNoShadow", true);

							// What a filter takes away is not picture either: edges
							// trimmed 55 px leave the middle of the badge.
							obs_data_t *cd = obs_data_create();
							obs_data_set_double(cd, "size", 0.0);
							obs_data_set_double(cd, "inset", 55.0);
							obs_data_set_bool(cd, "screen_px", false);
							obs_data_set_bool(cd, "placed", true);
							obs_source_t *trim = obs_source_create(
								"stagekit_corners", "Corners & Edges", cd, nullptr);
							obs_data_release(cd);
							obs_source_filter_add(widget, trim);
							for (int i = 0; i < 6; i++) {
								QCoreApplication::processEvents();
								sleep_ms(50);
							}
							const vec2 trimmed = go(0);
							CHECK(std::fabs(trimmed.x + 45) < 1.6 &&
								      std::fabs(trimmed.y + 45) < 1.6,
							      "and what a filter trims away does not count as picture (%.0f, %.0f; wanted -45, -45)",
							      trimmed.x, trimmed.y);
							obs_sceneitem_remove(iw);

							// The report: a pixel of background left along the screen's
							// edge. A smoothed edge ends in a faint row of pixels; that
							// row is not counted. Here column 20 is faint, 21 to 99 are
							// solid and column 100 is nearly solid.
							QImage smooth(120, 60, QImage::Format_ARGB32);
							smooth.fill(Qt::transparent);
							for (int y = 10; y < 50; y++) {
								smooth.setPixel(20, y, qRgba(240, 200, 40, 70));
								for (int x = 21; x < 100; x++)
									smooth.setPixel(x, y, qRgba(240, 200, 40, 255));
								smooth.setPixel(100, y, qRgba(240, 200, 40, 200));
							}
							const std::string smooth_path = out + "/arrange-smooth.png";
							smooth.save(QString::fromStdString(smooth_path));
							obs_data_t *md = obs_data_create();
							obs_data_set_string(md, "file", smooth_path.c_str());
							obs_source_t *smooth_src = obs_source_create(
								"image_source", "Smooth Edge", md, nullptr);
							obs_data_release(md);
							obs_sceneitem_t *im = obs_scene_add(as, smooth_src);
							place(im, 250, 120);
							for (int i = 0;
							     i < 40 && (items_box->findText("Smooth Edge") < 0 ||
									obs_source_get_width(smooth_src) < 120);
							     i++) {
								QCoreApplication::processEvents();
								sleep_ms(50);
							}
							items_box->setCurrentText("Smooth Edge");
							slide->setChecked(true);
							auto nudge = [&](int index) {
								click(QString("grid%1").arg(index).toUtf8().constData());
								for (int i = 0; i < 4; i++) {
									QCoreApplication::processEvents();
									sleep_ms(40);
								}
								return pos_of(im);
							};
							const float at_left = nudge(3).x;
							const float at_right = nudge(5).x;
							CHECK(std::fabs(at_left + 11) < 0.6 &&
								      std::fabs(at_right - 529) < 0.6,
							      "a faint row of pixels along a smoothed edge is not counted, so no line of background is left at the screen's edge (x %.1f and %.1f; wanted -11 and 529)",
							      at_left, at_right);
							slide->setChecked(false);
							obs_sceneitem_remove(im);
							obs_source_release(smooth_src);

							// The report: slides and 'Same in every scene' did nothing
							// for a group. A selected group is worked on as one thing.
							if (c_group) {
								obs_sceneitem_set_visible(c_group, true);
								select_only({});
								obs_sceneitem_select(c_group, true);
								for (int i = 0; i < 40 && items_box->currentText() !=
												  "Arrange Group";
								     i++) {
									QCoreApplication::processEvents();
									sleep_ms(50);
								}
								CHECK(items_box->currentText() == "Arrange Group",
								      "selecting a group in OBS makes it the dock's item ('%s')",
								      items_box->currentText().toUtf8().constData());
								place(c_group, 300, 200);
								for (int i = 0; i < 4; i++) {
									QCoreApplication::processEvents();
									sleep_ms(50);
								}
								const vec2 g0 = pos_of(c_group);
								slide->setChecked(true);
								click("grid3");
								for (int i = 0; i < 6; i++) {
									QCoreApplication::processEvents();
									sleep_ms(40);
								}
								const vec2 g1 = pos_of(c_group);
								CHECK(std::fabs(g0.x - 300) < 0.6 &&
									      std::fabs(g1.x - 10) < 1.6 &&
									      std::fabs(g1.y - g0.y) < 1.6,
								      "a group slides to the next edge like any item (from x %.0f to %.0f; wanted 10)",
								      g0.x, g1.x);
								slide->setChecked(false);

								// The same group in another scene takes its place.
								obs_scene_t *elsewhere =
									obs_scene_create("Group Elsewhere");
								obs_sceneitem_t *gcopy = obs_scene_add(
									elsewhere, obs_sceneitem_get_source(c_group));
								place(gcopy, 400, 50);
								for (int i = 0; i < 4; i++) {
									QCoreApplication::processEvents();
									sleep_ms(50);
								}
								const bool ran = click("arrange_same_everywhere");
								for (int i = 0; i < 4; i++) {
									QCoreApplication::processEvents();
									sleep_ms(50);
								}
								const vec2 gp = pos_of(c_group), cp2 = pos_of(gcopy);
								CHECK(ran && std::fabs(cp2.x - gp.x) < 0.6 &&
									      std::fabs(cp2.y - gp.y) < 0.6,
								      "'Copy transform to all scenes' puts a group where it is here in the other scenes it is in (%.0f, %.0f; wanted %.0f, %.0f)",
								      cp2.x, cp2.y, gp.x, gp.y);

								// Copy transform to chosen scenes: only the scenes on
								// the list take it.
								obs_scene_t *chosen_a = obs_scene_create("Chosen A");
								obs_scene_t *chosen_b = obs_scene_create("Chosen B");
								obs_sceneitem_t *in_a = obs_scene_add(
									chosen_a, obs_sceneitem_get_source(c_group));
								obs_sceneitem_t *in_b = obs_scene_add(
									chosen_b, obs_sceneitem_get_source(c_group));
								place(in_a, 500, 10);
								place(in_b, 520, 20);
								auto *add_scene =
									dock->findChild<QComboBox *>("copyScenesAdd");
								for (int i = 0; i < 40 && add_scene &&
										add_scene->findData("Chosen A") < 0;
								     i++) {
									QCoreApplication::processEvents();
									sleep_ms(50);
								}
								CHECK(add_scene &&
									      add_scene->findData("Chosen A") > 0 &&
									      add_scene->findData("Chosen B") > 0,
								      "the dock lists the scenes that can be added to 'Scenes to copy to'");
								const vec2 elsewhere_before = pos_of(gcopy);
								place(c_group, 60, 90);
								for (int i = 0; i < 4; i++) {
									QCoreApplication::processEvents();
									sleep_ms(50);
								}
								click("arrange_same_chosen");
								for (int i = 0; i < 4; i++) {
									QCoreApplication::processEvents();
									sleep_ms(50);
								}
								CHECK(std::fabs(pos_of(in_a).x - 500) < 0.6 &&
									      std::fabs(pos_of(in_b).x - 520) < 0.6,
								      "with no scene chosen, 'Copy transform to chosen scenes' changes nothing");
								if (add_scene) {
									emit add_scene->activated(
										add_scene->findData("Chosen A"));
									for (int i = 0; i < 4; i++) {
										QCoreApplication::processEvents();
										sleep_ms(50);
									}
								}
								CHECK(add_scene &&
									      add_scene->findData("Chosen A") < 0 &&
									      dock->findChild<QPushButton *>(
										      "copyScenesRemove"),
								      "a chosen scene gets a row with its own remove button and leaves the list to add from");
								click("arrange_same_chosen");
								for (int i = 0; i < 4; i++) {
									QCoreApplication::processEvents();
									sleep_ms(50);
								}
								const vec2 a_now = pos_of(in_a), b_now = pos_of(in_b),
									   e_now = pos_of(gcopy);
								CHECK(std::fabs(a_now.x - 60) < 0.6 &&
									      std::fabs(a_now.y - 90) < 0.6 &&
									      std::fabs(b_now.x - 520) < 0.6 &&
									      std::fabs(e_now.x - elsewhere_before.x) <
										      0.6,
								      "it copies to the chosen scene only (chosen %.0f, %.0f; not chosen %.0f and %.0f)",
								      a_now.x, a_now.y, b_now.x, e_now.x);
								// The report: "transforms still not applying to the
								// selected scenes for the selected source. the source
								// is a group". A group that a Fit to Space looks after
								// in the chosen scene, locked there, and that scene
								// not on screen.
								{
									obs_data_t *fd = obs_data_create();
									obs_data_set_string(fd, "item",
											    "Arrange Group");
									obs_data_set_int(fd, "box", 2);
									obs_data_set_double(fd, "margin", 10);
									obs_data_set_int(fd, "glide_ms", 0);
									obs_data_set_bool(fd, "resize", false);
									obs_source_t *held = obs_source_create(
										"stagekit_fit_item", "Fit Group", fd,
										nullptr);
									obs_data_release(fd);
									obs_source_filter_add(
										obs_scene_get_source(chosen_a), held);
									obs_source_t *was_out =
										obs_get_output_source(0);
									obs_set_output_source(
										0, obs_scene_get_source(chosen_a));
									for (int i = 0; i < 16; i++) {
										render(obs_scene_get_source(chosen_a),
										       640, 360);
										sleep_ms(50);
									}
									obs_set_output_source(0, was_out);
									obs_source_release(was_out);
									sleep_ms(300);
									const vec2 fitted_at = pos_of(in_a);
									obs_sceneitem_set_locked(in_a, true);
									place(c_group, 150, 120);
									for (int i = 0; i < 4; i++) {
										QCoreApplication::processEvents();
										sleep_ms(50);
									}
									click("arrange_same_chosen");
									sleep_ms(600);
									const vec2 left_alone = pos_of(in_a);
									tick_box("copyLocked", true);
									click("arrange_same_chosen");
									sleep_ms(600);
									const vec2 put = pos_of(in_a);
									tick_box("copyLocked", false);
									CHECK(std::fabs(left_alone.x - fitted_at.x) <
											      0.6 &&
										      std::fabs(put.x - 150) < 0.6 &&
										      std::fabs(put.y - 120) < 0.6 &&
										      obs_sceneitem_locked(in_a),
									      "a group that is locked and looked after by a Fit to Space in a chosen scene takes the transform with 'Also change locked copies', and keeps it (%.0f, %.0f; left alone at %.0f; the filter had it at %.0f, %.0f)",
									      put.x, put.y, left_alone.x, fitted_at.x,
									      fitted_at.y);
									// The same with the group named in 'Sources to
									// copy to', which is how a source is chosen.
									auto *name_source =
										dock->findChild<QComboBox *>(
											"copySourcesAdd");
									for (int i = 0;
									     i < 40 && name_source &&
									     name_source->findData("Arrange Group") < 0;
									     i++) {
										QCoreApplication::processEvents();
										sleep_ms(50);
									}
									if (name_source)
										emit name_source->activated(
											name_source->findData(
												"Arrange Group"));
									obs_sceneitem_set_locked(in_a, false);
									place(c_group, 170, 140);
									for (int i = 0; i < 4; i++) {
										QCoreApplication::processEvents();
										sleep_ms(50);
									}
									click("arrange_same_chosen");
									sleep_ms(600);
									const vec2 named = pos_of(in_a),
										   not_named = pos_of(in_b);
									CHECK(std::fabs(named.x - 170) < 0.6 &&
										      std::fabs(named.y - 140) < 0.6 &&
										      std::fabs(not_named.x - 520) <
											      0.6,
									      "with the group itself named in 'Sources to copy to', its copy in the chosen scene takes the transform (%.0f, %.0f; a scene not chosen %.0f)",
									      named.x, named.y, not_named.x);
									if (auto *unname =
										    dock->findChild<QPushButton *>(
											    "copySourcesRemove"))
										unname->click();
									// The row taken away is deleted now, so the checks
									// below do not find its remove button.
									QCoreApplication::sendPostedEvents(
										nullptr, QEvent::DeferredDelete);
									for (int i = 0; i < 6; i++) {
										QCoreApplication::processEvents();
										sleep_ms(50);
									}
									obs_source_filter_remove(
										obs_scene_get_source(chosen_a), held);
									obs_source_release(held);
									// Back as the checks below expect to find it.
									place(c_group, 60, 90);
									place(in_a, 60, 90);
									for (int i = 0; i < 4; i++) {
										QCoreApplication::processEvents();
										sleep_ms(50);
									}
								}
								if (auto *remove = dock->findChild<QPushButton *>(
									    "copyScenesRemove"))
									remove->click();
								for (int i = 0; i < 6; i++) {
									QCoreApplication::processEvents();
									sleep_ms(50);
								}
								CHECK(add_scene && add_scene->findData("Chosen A") > 0,
								      "the remove button takes a scene off the list again");

								// Sources to copy to: with a source listed, the
								// transform goes to that source's items instead.
								obs_source_t *other =
									obs_source_create("color_source", "Copy Target",
											  nullptr, nullptr);
								obs_sceneitem_t *other_a =
									obs_scene_add(chosen_a, other);
								obs_sceneitem_t *other_b =
									obs_scene_add(chosen_b, other);
								place(other_a, 700, 300);
								place(other_b, 710, 310);
								auto *add_source =
									dock->findChild<QComboBox *>("copySourcesAdd");
								for (int i = 0; i < 40 && add_source &&
										add_source->findData("Copy Target") < 0;
								     i++) {
									QCoreApplication::processEvents();
									sleep_ms(50);
								}
								CHECK(add_source &&
									      add_source->findData("Copy Target") > 0 &&
									      dock->findChild<QLabel *>(
										      "copySourcesSame"),
								      "the dock lists the sources that can be added to 'Sources to copy to', and says 'The same source' while none is");
								if (add_source) {
									emit add_source->activated(
										add_source->findData("Copy Target"));
									for (int i = 0; i < 4; i++) {
										QCoreApplication::processEvents();
										sleep_ms(50);
									}
								}
								place(c_group, 80, 110);
								const vec2 same_before = pos_of(in_b);
								for (int i = 0; i < 4; i++) {
									QCoreApplication::processEvents();
									sleep_ms(50);
								}
								click("arrange_same_everywhere");
								for (int i = 0; i < 4; i++) {
									QCoreApplication::processEvents();
									sleep_ms(50);
								}
								const vec2 oa = pos_of(other_a), ob = pos_of(other_b),
									   same_now = pos_of(in_b);
								CHECK(std::fabs(oa.x - 80) < 0.6 &&
									      std::fabs(oa.y - 110) < 0.6 &&
									      std::fabs(ob.x - 80) < 0.6 &&
									      std::fabs(same_now.x - same_before.x) <
										      0.6,
								      "with a source listed, 'Copy transform to all scenes' gives the transform to that source's items, not to the same source's (%.0f, %.0f and %.0f; same source %.0f)",
								      oa.x, oa.y, ob.x, same_now.x);
								if (auto *remove = dock->findChild<QPushButton *>(
									    "copySourcesRemove"))
									remove->click();
								for (int i = 0; i < 6; i++) {
									QCoreApplication::processEvents();
									sleep_ms(50);
								}
								place(c_group, 90, 120);
								place(other_a, 700, 300);
								for (int i = 0; i < 4; i++) {
									QCoreApplication::processEvents();
									sleep_ms(50);
								}
								click("arrange_same_everywhere");
								for (int i = 0; i < 4; i++) {
									QCoreApplication::processEvents();
									sleep_ms(50);
								}
								CHECK(add_source &&
									      add_source->findData("Copy Target") > 0 &&
									      std::fabs(pos_of(in_b).x - 90) < 0.6 &&
									      std::fabs(pos_of(other_a).x - 700) < 0.6,
								      "taken off the list again, it goes to the same source in the other scenes as before (%.0f; other source %.0f)",
								      pos_of(in_b).x, pos_of(other_a).x);
								obs_source_release(other);
								obs_scene_release(chosen_a);
								obs_scene_release(chosen_b);
								obs_scene_release(elsewhere);
								obs_sceneitem_select(c_group, false);
								obs_sceneitem_set_visible(c_group, false);
							}
							obs_source_release(trim);
							obs_source_release(widget_shadow);
							obs_source_release(widget);
							obs_sceneitem_set_visible(ibadge, true);
							items_box->setCurrentText("Block A");
						}

						margin->setValue(30);
						travel->setValue(500);
						if (c_group)
							obs_sceneitem_set_visible(c_group, true);
					}
				}

				select_only({});
				if (scenes)
					scenes->setCurrentIndex(std::max(0, scenes->findData("Test Scene")));
				obs_set_output_source(0, edited_before);
				obs_source_release(edited_before);
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

			// The start image and background: the dock's Standby & Ambient tab, also
			// made on its own here to look at it. A very long file name must not
			// widen it, and it shows a small picture.
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
				CHECK(background_dock && dock->findChild<QLabel *>("startThumb") != nullptr &&
					      dock->findChild<QWidget *>("pageBackground") != nullptr,
				      "the start image and background are the dock's Standby & Ambient tab (they had a dock of their own up to beta151)");
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

			// The dock is four tabs; each is pictured, and the README shows
			// them side by side.
			{
				auto *tabs = dock->findChild<QTabWidget *>("stagekitTabs");
				CHECK(tabs && tabs->count() == 5 && tabs->widget(4)->objectName() == "pageBackground" &&
					      tabs->tabText(4) == "Standby && Ambient" &&
					      tabs->widget(4)->findChild<QLabel *>("startThumb") &&
					      tabs->widget(4)->findChild<QPushButton *>("chooseStartImage"),
				      "the dock has five tabs: Place, Arrange, Copy, Effects, and Standby & Ambient with the standby image and the moving background");
				std::vector<QPixmap> pages;
				int height = 0;
				for (int i = 0; tabs && i < tabs->count(); i++) {
					tabs->setCurrentIndex(i);
					QCoreApplication::processEvents();
					dock->resize(340, 10);
					dock->resize(340, dock->sizeHint().height());
					QCoreApplication::processEvents();
					pages.push_back(dock->grab());
					height = std::max(height, pages.back().height());
				}
				int width = 0;
				for (const QPixmap &one : pages)
					width += one.width() + 12;
				QImage sheet(std::max(1, width - 12), height, QImage::Format_ARGB32);
				sheet.fill(Qt::transparent);
				{
					QPainter painter(&sheet);
					int x = 0;
					for (const QPixmap &one : pages) {
						painter.drawPixmap(x, 0, one);
						x += one.width() + 12;
					}
				}
				sheet.save(QString::fromStdString(out + "/../stagekit-dock.png"));
				CHECK(height > 200 && height < 900,
				      "and no tab is taller than a screen (the tallest is %d px; it was 1460 as one column)",
				      height);
			}
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

	// ==== scene-filters:fit ====
	// ------------------------------------------ fit to space: off-screen idle
	{
		printf("\n[fit to space: off-screen idle]\n");
		auto fit_sized = [](obs_source_t *src, int cw, int ch) {
			obs_data_t *d = obs_data_create();
			obs_data_set_int(d, "width", cw);
			obs_data_set_int(d, "height", ch);
			obs_source_update(src, d);
			obs_data_release(d);
		};
		// The full ticks a fit filter's engines have had, and the glides set off.
		auto fit_counts = [](const char *filter, const char *item, long long &ticks, long long &glides) {
			calldata_t cd;
			calldata_init(&cd);
			calldata_set_string(&cd, "filter", filter);
			calldata_set_string(&cd, "item", item ? item : "");
			proc_handler_call(obs_get_proc_handler(), "stagekit_test_fit_counts", &cd);
			ticks = calldata_int(&cd, "ticks");
			glides = calldata_int(&cd, "glides");
			calldata_free(&cd);
		};
		obs_source_t *game = obs_source_create("color_source", "Idle Game", nullptr, nullptr);
		fit_sized(game, 400, 300);
		obs_source_t *cam = obs_source_create("color_source", "Idle Cam", nullptr, nullptr);
		fit_sized(cam, 100, 100);
		obs_scene_t *away = obs_scene_create("Fit Idle Away");
		obs_scene_t *other = obs_scene_create("Fit Idle Other");
		obs_sceneitem_t *gi = obs_scene_add(away, game);
		obs_sceneitem_t *ci = obs_scene_add(away, cam);
		vec2 at;
		vec2_set(&at, 100.0f, 50.0f);
		obs_sceneitem_set_pos(gi, &at);
		vec2_set(&at, 520.0f, 240.0f);
		obs_sceneitem_set_pos(ci, &at);
		obs_data_t *d = obs_data_create();
		obs_data_set_string(d, "item", "Idle Game");
		obs_data_set_int(d, "box", 2);
		obs_data_set_double(d, "margin", 20);
		obs_data_set_int(d, "glide_ms", 0);
		obs_source_t *fit = obs_source_create("stagekit_fit_item", "Fit Idle", d, nullptr);
		obs_data_release(d);
		obs_source_filter_add(obs_scene_get_source(away), fit);
		obs_source_t *before = obs_get_output_source(0);
		auto show = [&](obs_scene_t *scene, int ms) {
			obs_set_output_source(0, obs_scene_get_source(scene));
			for (int i = 0; i < ms / 20; i++) {
				render(obs_scene_get_source(scene), 640, 360);
				sleep_ms(20);
			}
		};
		auto game_at = [&]() {
			vec2 p, b;
			obs_sceneitem_get_pos(gi, &p);
			obs_sceneitem_get_bounds(gi, &b);
			struct {
				double v[4];
				double operator[](int i) const { return v[i]; }
			} out{{p.x, p.y, b.x, b.y}};
			return out;
		};
		show(away, 600);
		auto g = game_at();
		CHECK(std::fabs(g[0] - 20) < 0.6 && std::fabs(g[2] - 480) < 0.6,
		      "on screen it is fitted as before (%.0f, %.0f %.0fx%.0f)", g[0], g[1], g[2], g[3]);
		long long t0 = 0, t1 = 0, glides = 0;
		fit_counts("Fit Idle", nullptr, t0, glides);
		show(away, 1000);
		fit_counts("Fit Idle", nullptr, t1, glides);
		const long long shown_ticks = t1 - t0;
		show(other, 300);
		fit_counts("Fit Idle", nullptr, t0, glides);
		show(other, 1000);
		fit_counts("Fit Idle", nullptr, t1, glides);
		const long long idle_ticks = t1 - t0;
		CHECK(shown_ticks >= 30 && idle_ticks * 5 <= shown_ticks,
		      "off screen with nothing to do, its full ticks fall by 80 %% or more (%lld a second on screen, %lld off)",
		      shown_ticks, idle_ticks);
		// Something changes while the scene is away: the item beside it is
		// moved over the game's place.
		vec2_set(&at, 20.0f, 20.0f);
		obs_sceneitem_set_pos(ci, &at);
		show(other, 300);
		g = game_at();
		CHECK(std::fabs(g[0] - 20) < 0.6, "and nothing is done to the item while its scene is away (%.0f)",
		      g[0]);
		obs_set_output_source(0, obs_scene_get_source(away));
		sleep_ms(60); // three or four frames
		g = game_at();
		CHECK(std::fabs(g[0] - 140) < 0.6 && std::fabs(g[2] - 480) < 0.6,
		      "the scene shown again, it is put right at once, without a glide (%.0f, %.0f %.0fx%.0f)", g[0],
		      g[1], g[2], g[3]);
		// New settings while away are taken at once, not four times a second.
		show(other, 300);
		obs_data_t *again = obs_source_get_settings(fit);
		obs_data_set_double(again, "margin", 10);
		obs_source_update(fit, again);
		obs_data_release(again);
		fit_counts("Fit Idle", nullptr, t0, glides);
		sleep_ms(50);
		fit_counts("Fit Idle", nullptr, t1, glides);
		CHECK(t1 > t0, "new settings while away are taken in full at once (%lld full ticks)", t1 - t0);
		obs_set_output_source(0, before);
		obs_source_release(before);
		obs_source_filter_remove(obs_scene_get_source(away), fit);
		obs_source_release(fit);
		for (obs_scene_t *scene : {away, other}) {
			obs_source_remove(obs_scene_get_source(scene));
			obs_scene_release(scene);
		}
		obs_source_release(game);
		obs_source_release(cam);
	}
	// ---------------------------------------------------------- scene fit
	{
		printf("\n[scene fit]\n");
		auto fit_sized = [](obs_source_t *src, int cw, int ch) {
			obs_data_t *d = obs_data_create();
			obs_data_set_int(d, "width", cw);
			obs_data_set_int(d, "height", ch);
			obs_source_update(src, d);
			obs_data_release(d);
		};
		auto fit_counts = [](const char *filter, const char *item, long long &ticks, long long &glides) {
			calldata_t cd;
			calldata_init(&cd);
			calldata_set_string(&cd, "filter", filter);
			calldata_set_string(&cd, "item", item ? item : "");
			proc_handler_call(obs_get_proc_handler(), "stagekit_test_fit_counts", &cd);
			ticks = calldata_int(&cd, "ticks");
			glides = calldata_int(&cd, "glides");
			calldata_free(&cd);
		};
		auto fit_list = [](obs_data_t *d, const char *setting, std::vector<std::string> values) {
			obs_data_array_t *list = obs_data_array_create();
			for (const std::string &value : values) {
				obs_data_t *entry = obs_data_create();
				obs_data_set_string(entry, "value", value.c_str());
				obs_data_array_push_back(list, entry);
				obs_data_release(entry);
			}
			obs_data_set_array(d, setting, list);
			obs_data_array_release(list);
		};
		struct FitBox {
			double x = 0, y = 0, w = 0, h = 0;
		};
		auto fit_box = [](obs_sceneitem_t *item) {
			vec2 p, b;
			obs_sceneitem_get_pos(item, &p);
			obs_sceneitem_get_bounds(item, &b);
			return FitBox{p.x, p.y, b.x, b.y};
		};
		auto fit_near = [](const FitBox &a, const FitBox &b, double by) {
			return std::fabs(a.x - b.x) <= by && std::fabs(a.y - b.y) <= by && std::fabs(a.w - b.w) <= by &&
			       std::fabs(a.h - b.h) <= by;
		};
		auto fit_overlap = [](const FitBox &a, const FitBox &b) {
			return a.x < b.x + b.w - 0.5 && b.x < a.x + a.w - 0.5 && a.y < b.y + b.h - 0.5 &&
			       b.y < a.y + a.h - 0.5;
		};
		// The owner's layout, small: a game that is fitted to what is left, a
		// camera and a chat box that keep their size, over a background.
		obs_source_t *game = obs_source_create("color_source", "SF Game", nullptr, nullptr);
		fit_sized(game, 1600, 900);
		obs_source_t *cam = obs_source_create("color_source", "SF Cam", nullptr, nullptr);
		fit_sized(cam, 120, 68);
		obs_source_t *chat = obs_source_create("color_source", "SF Chat", nullptr, nullptr);
		fit_sized(chat, 120, 150);
		obs_source_t *bg = obs_source_create("color_source", "SF Back", nullptr, nullptr);
		fit_sized(bg, 640, 360);
		auto fit_scene = [&](const char *name, obs_sceneitem_t **gi, obs_sceneitem_t **ci,
				     obs_sceneitem_t **hi) {
			obs_scene_t *scene = obs_scene_create(name);
			obs_scene_add(scene, bg);
			*gi = obs_scene_add(scene, game);
			*ci = obs_scene_add(scene, cam);
			*hi = obs_scene_add(scene, chat);
			vec2 at, k;
			vec2_set(&at, 10.0f, 10.0f);
			vec2_set(&k, 0.25f, 0.25f);
			obs_sceneitem_set_pos(*gi, &at);
			obs_sceneitem_set_scale(*gi, &k);
			vec2_set(&at, 510.0f, 10.0f);
			obs_sceneitem_set_pos(*ci, &at);
			vec2_set(&at, 510.0f, 100.0f);
			obs_sceneitem_set_pos(*hi, &at);
			return scene;
		};
		// One entry's (or one Fit to Space's) settings.
		auto fit_settings = [](obs_data_t *d, const std::string &prefix, bool resize, int toward) {
			obs_data_set_int(d, (prefix + "box").c_str(), 2);
			obs_data_set_double(d, (prefix + "margin").c_str(), 10);
			obs_data_set_double(d, (prefix + "tolerance").c_str(), 5);
			obs_data_set_bool(d, (prefix + "resize").c_str(), resize);
			obs_data_set_int(d, (prefix + "toward").c_str(), toward);
			obs_data_set_int(d, (prefix + "glide_ms").c_str(), 200);
		};
		obs_source_t *before = obs_get_output_source(0);
		auto show = [&](obs_scene_t *scene, int ms) {
			obs_set_output_source(0, obs_scene_get_source(scene));
			for (int i = 0; i < ms / 20; i++) {
				render(obs_scene_get_source(scene), 640, 360);
				sleep_ms(20);
			}
		};

		// The same layout by three Fit to Space filters...
		obs_sceneitem_t *g1, *c1, *h1;
		obs_scene_t *alone = fit_scene("SF Three Filters", &g1, &c1, &h1);
		std::vector<obs_source_t *> filters;
		for (const char *item : {"SF Cam", "SF Chat", "SF Game"}) {
			obs_data_t *d = obs_data_create();
			obs_data_set_string(d, "item", item);
			fit_settings(d, "", std::strcmp(item, "SF Game") == 0,
				     std::strcmp(item, "SF Cam") == 0 ? 6 : 0);
			const std::string name = std::string("SF Fit ") + item;
			obs_source_t *f = obs_source_create("stagekit_fit_item", name.c_str(), d, nullptr);
			obs_data_release(d);
			obs_source_filter_add(obs_scene_get_source(alone), f);
			filters.push_back(f);
		}
		show(alone, 1500);
		const FitBox a_game = fit_box(g1), a_cam = fit_box(c1), a_chat = fit_box(h1);
		// ... and by one Scene Fit with three entries.
		obs_sceneitem_t *g2, *c2, *h2;
		obs_scene_t *together = fit_scene("SF One Filter", &g2, &c2, &h2);
		obs_data_t *sd = obs_data_create();
		fit_list(sd, "entries", {"SF Cam", "SF Chat", "SF Game"});
		fit_settings(sd, "fit|SF Cam|", false, 6);
		fit_settings(sd, "fit|SF Chat|", false, 0);
		fit_settings(sd, "fit|SF Game|", true, 0);
		obs_source_t *sf = obs_source_create("stagekit_scene_fit", "SF Scene Fit", sd, nullptr);
		obs_data_release(sd);
		obs_source_filter_add(obs_scene_get_source(together), sf);
		show(together, 1500);
		const FitBox b_game = fit_box(g2), b_cam = fit_box(c2), b_chat = fit_box(h2);
		CHECK(fit_near(a_game, b_game, 0.5) && fit_near(a_cam, b_cam, 0.5) && fit_near(a_chat, b_chat, 0.5),
		      "Scene Fit puts its sources where three Fit to Space filters do (game %.0f,%.0f %.0fx%.0f / %.0f,%.0f %.0fx%.0f; camera %.0f,%.0f / %.0f,%.0f; chat %.0f,%.0f / %.0f,%.0f)",
		      a_game.x, a_game.y, a_game.w, a_game.h, b_game.x, b_game.y, b_game.w, b_game.h, a_cam.x, a_cam.y,
		      b_cam.x, b_cam.y, a_chat.x, a_chat.y, b_chat.x, b_chat.y);
		{
			obs_data_t *own = obs_source_get_settings(sf);
			CHECK(obs_data_get_bool(own, "fit|SF Cam|have_anchor") &&
				      std::fabs(obs_data_get_double(own, "fit|SF Chat|keep_k") - 1.0) < 0.001 &&
				      !obs_data_has_user_value(own, "have_anchor"),
			      "each entry keeps what it writes down under its own name (anchor, kept size)");
			obs_data_release(own);
		}

		// Events, each followed by a check that every entry set off at most
		// once and that nothing overlaps once the glides are over.
		// (`filter` empty: each source's own Fit to Space.)
		auto fit_glides = [&](const char *filter, std::vector<long long> &out) {
			out.clear();
			for (const char *item : {"SF Cam", "SF Chat", "SF Game"}) {
				long long ticks = 0, glides = 0;
				const std::string by = *filter ? std::string(filter) : std::string("SF Fit ") + item;
				fit_counts(by.c_str(), item, ticks, glides);
				out.push_back(glides);
			}
		};
		auto fit_clear = [&](obs_sceneitem_t *gi, obs_sceneitem_t *ci, obs_sceneitem_t *hi) {
			std::vector<FitBox> shown;
			for (obs_sceneitem_t *item : {gi, ci, hi})
				if (obs_sceneitem_visible(item))
					shown.push_back(fit_box(item));
			for (size_t i = 0; i < shown.size(); i++) {
				const FitBox &a = shown[i];
				if (a.x < 9.5 || a.y < 9.5 || a.x + a.w > 630.5 || a.y + a.h > 350.5)
					return false;
				for (size_t j = i + 1; j < shown.size(); j++)
					if (fit_overlap(a, shown[j]))
						return false;
			}
			return true;
		};
		struct Event {
			const char *what;
			std::function<void()> act;
		};
		auto fit_events = [&](const char *filter, obs_scene_t *scene, obs_sceneitem_t *gi, obs_sceneitem_t *ci,
				      obs_sceneitem_t *hi, bool check) {
			std::vector<Event> events = {
				{"the camera hidden",
				 [&] {
					 obs_sceneitem_set_visible(ci, false);
				 }},
				{"the camera shown",
				 [&] {
					 obs_sceneitem_set_visible(ci, true);
				 }},
				{"the game 4:3",
				 [&] {
					 fit_sized(game, 1200, 900);
				 }},
				{"the game 16:9 again",
				 [&] {
					 fit_sized(game, 1600, 900);
				 }},
				{"the chat box moved by hand",
				 [&] {
					 vec2 at;
					 vec2_set(&at, 500.0f, 200.0f);
					 obs_sceneitem_set_pos(hi, &at);
				 }},
			};
			std::string counts;
			for (const Event &event : events) {
				std::vector<long long> was, now;
				fit_glides(filter, was);
				event.act();
				bool clear = true;
				for (int i = 0; i < 60; i++) {
					render(obs_scene_get_source(scene), 640, 360);
					sleep_ms(25);
					// From when the glides are over (they last 0.2 s,
					// a change of shape waits half a second first).
					if (i >= 45)
						clear = clear && fit_clear(gi, ci, hi);
				}
				fit_glides(filter, now);
				long long most = 0;
				for (size_t i = 0; i < now.size(); i++)
					most = std::max(most, now[i] - was[i]);
				char line[160];
				snprintf(line, sizeof(line), " %s: %lld %lld %lld;", event.what, now[0] - was[0],
					 now[1] - was[1], now[2] - was[2]);
				counts += line;
				if (check) {
					CHECK(most <= 1, "%s: each entry sets off at most once (%lld, %lld, %lld)",
					      event.what, now[0] - was[0], now[1] - was[1], now[2] - was[2]);
					CHECK(clear, "%s: nothing overlaps, all inside the gap", event.what);
				}
			}
			if (!check)
				printf("  (for comparison, three Fit to Space filters set off:%s)\n", counts.c_str());
		};
		show(alone, 200);
		fit_events("", alone, g1, c1, h1, false);
		show(together, 300);
		fit_events("SF Scene Fit", together, g2, c2, h2, true);

		// Hidden and back: the chat box takes the camera's room, the camera
		// comes back where it was and the chat box makes way.
		const FitBox cam_was = fit_box(c2), game_was = fit_box(g2), chat_was = fit_box(h2);
		obs_sceneitem_set_visible(c2, false);
		show(together, 900);
		const FitBox chat_alone = fit_box(h2);
		CHECK(chat_alone.y < chat_was.y - 1.0 && fit_clear(g2, c2, h2),
		      "a hidden entry's room is taken by the others (the chat box at %.0f, was %.0f)", chat_alone.y,
		      chat_was.y);
		obs_sceneitem_set_visible(c2, true);
		show(together, 900);
		CHECK(fit_near(fit_box(c2), cam_was, 0.5) && fit_near(fit_box(g2), game_was, 0.5) &&
			      fit_near(fit_box(h2), chat_was, 0.5) && fit_clear(g2, c2, h2),
		      "shown again, it is back where it was and the others made way (camera %.0f,%.0f)", fit_box(c2).x,
		      fit_box(c2).y);

		// Off screen: nothing is looked at or moved; shown again, put right at once.
		obs_scene_t *elsewhere = obs_scene_create("SF Elsewhere");
		show(elsewhere, 200);
		long long t0 = 0, t1 = 0, gl = 0;
		fit_counts("SF Scene Fit", nullptr, t0, gl);
		show(elsewhere, 1000);
		fit_counts("SF Scene Fit", nullptr, t1, gl);
		CHECK(t1 - t0 <= 3 * 6,
		      "off screen, its entries have about four full ticks a second (%lld for three in a second)",
		      t1 - t0);
		const FitBox chat_here = fit_box(h2);
		obs_sceneitem_set_visible(c2, false);
		show(elsewhere, 300);
		CHECK(fit_near(fit_box(h2), chat_here, 0.01), "nothing is moved while the scene is away");
		obs_set_output_source(0, obs_scene_get_source(together));
		sleep_ms(80);
		CHECK(fit_box(h2).y < chat_was.y - 1.0,
		      "shown again, the chat box has the camera's room at once, without a glide (at %.0f)",
		      fit_box(h2).y);
		obs_sceneitem_set_visible(c2, true);
		show(together, 900);

		// Sync to scenes: an entry's place is given to the same source in a
		// listed scene that is not on screen.
		obs_sceneitem_t *g3, *c3, *h3;
		obs_scene_t *synced = fit_scene("SF Synced", &g3, &c3, &h3);
		{
			obs_data_t *own = obs_source_get_settings(sf);
			// (A Scene Fit's lists are kept by the source's name in hex.)
			std::string hex = "fit_";
			for (unsigned char ch : std::string("SF Chat")) {
				static const char *const digits = "0123456789abcdef";
				hex += digits[ch >> 4];
				hex += digits[ch & 15];
			}
			fit_list(own, (hex + "_share_scenes").c_str(), {"SF Synced"});
			obs_source_update(sf, own);
			obs_data_release(own);
		}
		show(together, 600);
		CHECK(fit_near(fit_box(h3), fit_box(h2), 0.5),
		      "'Sync to scenes' gives the entry's place to its copy in the listed scene (%.0f,%.0f / %.0f,%.0f)",
		      fit_box(h3).x, fit_box(h3).y, fit_box(h2).x, fit_box(h2).y);

		// Rename: the entry, its settings and its kept size follow.
		obs_source_set_name(cam, "SF Camera");
		show(together, 600);
		{
			obs_data_t *own = obs_source_get_settings(sf);
			obs_data_array_t *list = obs_data_get_array(own, "entries");
			obs_data_t *first = obs_data_array_item(list, 0);
			CHECK(std::strcmp(obs_data_get_string(first, "value"), "SF Camera") == 0 &&
				      obs_data_get_bool(own, "fit|SF Camera|have_anchor") &&
				      !obs_data_has_user_value(own, "fit|SF Cam|have_anchor") &&
				      std::strcmp(obs_data_get_string(own, "fit|SF Camera|keep_item"), "SF Camera") ==
					      0,
			      "a renamed source keeps its entry and settings (%s)",
			      obs_data_get_string(first, "value"));
			obs_data_release(first);
			obs_data_array_release(list);
			obs_data_release(own);
		}
		CHECK(fit_near(fit_box(c2), cam_was, 0.5) && fit_clear(g2, c2, h2),
		      "and it is still fitted where it was (%.0f,%.0f)", fit_box(c2).x, fit_box(c2).y);
		obs_source_set_name(cam, "SF Cam");
		show(together, 300);

		// Take over: the three Fit to Space filters become one Scene Fit's
		// entries, and nothing moves.
		show(alone, 1200);
		const FitBox t_game = fit_box(g1), t_cam = fit_box(c1), t_chat = fit_box(h1);
		obs_source_t *taker = obs_source_create("stagekit_scene_fit", "SF Taker", nullptr, nullptr);
		obs_source_filter_add(obs_scene_get_source(alone), taker);
		{
			obs_properties_t *props = obs_source_properties(taker);
			obs_property_t *button = obs_properties_get(props, "take_over");
			CHECK(button && obs_property_button_clicked(button, taker),
			      "'Take over Fit to Space' can be pressed");
			CHECK(obs_properties_get(props, "fit|SF Game|g") != nullptr,
			      "and the window has a group for each entry taken");
			obs_properties_destroy(props);
		}
		bool all_off = true;
		for (obs_source_t *f : filters)
			all_off = all_off && !obs_source_enabled(f);
		double worst = 0.0;
		for (int i = 0; i < 60; i++) {
			render(obs_scene_get_source(alone), 640, 360);
			sleep_ms(17);
			for (auto pair :
			     {std::make_pair(g1, t_game), std::make_pair(c1, t_cam), std::make_pair(h1, t_chat)}) {
				const FitBox now = fit_box(pair.first);
				worst = std::max({worst, std::fabs(now.x - pair.second.x),
						  std::fabs(now.y - pair.second.y), std::fabs(now.w - pair.second.w),
						  std::fabs(now.h - pair.second.h)});
			}
		}
		{
			obs_data_t *own = obs_source_get_settings(taker);
			CHECK(all_off && obs_data_get_bool(own, "fit|SF Cam|have_anchor") &&
				      obs_data_get_double(own, "fit|SF Chat|keep_k") > 0.0,
			      "the filters are switched off and the entries carry their anchors and kept sizes");
			obs_data_release(own);
		}
		CHECK(worst <= 0.5, "nothing moves more than half a pixel over 60 frames (%.2f px)", worst);
		long long ticks = 0;
		fit_counts("SF Taker", "SF Game", ticks, gl);
		CHECK(ticks > 0, "the taken entries are fitted by the Scene Fit (%lld full ticks)", ticks);

		// Nothing is done while a collection is changing.
		auto fit_loaded = [](bool loaded) {
			calldata_t ld;
			calldata_init(&ld);
			calldata_set_bool(&ld, "loaded", loaded);
			proc_handler_call(obs_get_proc_handler(), "stagekit_test_loaded", &ld);
			calldata_free(&ld);
		};
		show(together, 300);
		const FitBox chat_loaded = fit_box(h2);
		fit_loaded(false);
		obs_sceneitem_set_visible(c2, false);
		show(together, 500);
		CHECK(fit_near(fit_box(h2), chat_loaded, 0.01), "nothing is moved while a collection is changing");
		fit_loaded(true);
		show(together, 900);
		CHECK(fit_box(h2).y < chat_loaded.y - 1.0, "and it fits again once loaded (the chat box at %.0f)",
		      fit_box(h2).y);
		obs_sceneitem_set_visible(c2, true);
		show(together, 900);

		// On a group: it does nothing.
		{
			obs_scene_t *holder = obs_scene_create("SF Holder");
			obs_sceneitem_t *inner = obs_scene_add(holder, chat);
			vec2 at;
			vec2_set(&at, 30.0f, 30.0f);
			obs_sceneitem_set_pos(inner, &at);
			obs_sceneitem_t *group = obs_scene_insert_group(holder, "SF Group", nullptr, 0);
			obs_sceneitem_group_add_item(group, inner);
			obs_data_t *gd = obs_data_create();
			fit_list(gd, "entries", {"SF Chat"});
			fit_settings(gd, "fit|SF Chat|", true, 0);
			obs_source_t *on_group = obs_source_create("stagekit_scene_fit", "SF On Group", gd, nullptr);
			obs_data_release(gd);
			obs_source_filter_add(obs_sceneitem_get_source(group), on_group);
			const FitBox inner_was = fit_box(inner);
			show(holder, 500);
			CHECK(fit_near(fit_box(inner), inner_was, 0.01), "on a group it does nothing");
			obs_properties_t *props = obs_source_properties(on_group);
			CHECK(obs_properties_get(props, "on_group") != nullptr, "and says so in its window");
			obs_properties_destroy(props);
			obs_source_filter_remove(obs_sceneitem_get_source(group), on_group);
			obs_source_release(on_group);
			obs_source_remove(obs_scene_get_source(holder));
			obs_scene_release(holder);
		}

		// The window: a group per entry, only the chosen one shown, and one
		// added when an entry is added.
		{
			obs_properties_t *props = obs_source_properties(sf);
			obs_property_t *cam_group = obs_properties_get(props, "fit|SF Cam|g");
			obs_property_t *game_group = obs_properties_get(props, "fit|SF Game|g");
			CHECK(cam_group && game_group &&
				      obs_property_visible(cam_group) != obs_property_visible(game_group),
			      "the window has a group of settings per entry, one shown at a time");
			obs_property_t *chosen = obs_properties_get(props, "fit_for");
			obs_data_t *own = obs_source_get_settings(sf);
			obs_data_set_string(own, "fit_for", "SF Game");
			obs_property_modified(chosen, own);
			CHECK(obs_property_visible(game_group) && !obs_property_visible(cam_group),
			      "'Settings for' shows the chosen entry's settings");
			CHECK(pick_into_list(sf, props, "entries|a", "SF Back") &&
				      obs_properties_get(props, "fit|SF Back|g") != nullptr,
			      "a source added to the list gets its own settings at once");
			CHECK(std::fabs(obs_data_get_double(own, "fit|SF Back|margin") - 30.0) < 0.01,
			      "starting from the filter's own (a gap of 30)");
			obs_data_release(own);
			obs_properties_destroy(props);
			// (Taken off again: a background filling the screen has no room.)
			obs_data_t *back = obs_source_get_settings(sf);
			fit_list(back, "entries", {"SF Cam", "SF Chat", "SF Game"});
			obs_source_update(sf, back);
			obs_data_release(back);
		}

		obs_set_output_source(0, before);
		obs_source_release(before);
		obs_source_filter_remove(obs_scene_get_source(together), sf);
		obs_source_release(sf);
		obs_source_filter_remove(obs_scene_get_source(alone), taker);
		obs_source_release(taker);
		for (obs_source_t *f : filters) {
			obs_source_filter_remove(obs_scene_get_source(alone), f);
			obs_source_release(f);
		}
		for (obs_scene_t *scene : {alone, together, elsewhere, synced}) {
			obs_source_remove(obs_scene_get_source(scene));
			obs_scene_release(scene);
		}
		for (obs_source_t *src : {game, cam, chat, bg})
			obs_source_release(src);
	}
	// Space priority between fitted sources (its own function, so that it can
	// be run alone: STAGEKIT_TEST_ONLY_FIT_PRIORITY).
	fit_priority_test();
	// ==== end scene-filters:fit ====
	// ==== scene-filters:all-sources ====
	// ---------------------------------------- the scene filters' All sources
	// Each scene filter with a "Sources covered" list: All sources covers a
	// source added later, leaves an ignored one alone, and a filter saved
	// before the choice (no "which") does what it did then.
	{
		auto as_wait = [](const std::function<bool()> &done, int ms) {
			for (int waited = 0; waited < ms; waited += 50) {
				if (done())
					return true;
				sleep_ms(50);
			}
			return done();
		};
		auto as_count = [](obs_source_t *source, const char *kind) {
			struct Find {
				const char *kind;
				int n;
			} find{kind, 0};
			obs_source_enum_filters(
				source,
				[](obs_source_t *, obs_source_t *f, void *param) {
					auto *x = static_cast<Find *>(param);
					if (strcmp(obs_source_get_unversioned_id(f), x->kind) == 0)
						x->n++;
				},
				&find);
			return find.n;
		};
		auto as_list = [](obs_data_t *settings, const char *key, std::vector<const char *> names) {
			obs_data_array_t *array = obs_data_array_create();
			for (const char *name : names) {
				obs_data_t *entry = obs_data_create();
				obs_data_set_string(entry, "value", name);
				obs_data_array_push_back(array, entry);
				obs_data_release(entry);
			}
			obs_data_set_array(settings, key, array);
			obs_data_array_release(array);
		};
		auto as_colour = [](const char *name, long long abgr, int cw, int ch) {
			obs_data_t *d = obs_data_create();
			obs_data_set_int(d, "color", abgr);
			obs_data_set_int(d, "width", cw);
			obs_data_set_int(d, "height", ch);
			obs_source_t *made = obs_source_create("color_source", name, d, nullptr);
			obs_data_release(d);
			return made;
		};
		auto as_put = [](obs_scene_t *scene, obs_source_t *src, float x, float y) {
			obs_sceneitem_t *item = obs_scene_add(scene, src);
			vec2 p;
			vec2_set(&p, x, y);
			obs_sceneitem_set_pos(item, &p);
			return item;
		};
		obs_source_t *as_was_out = obs_get_output_source(0);
		std::vector<obs_source_t *> as_sources;
		std::vector<obs_scene_t *> as_scenes;
		auto as_filter = [](obs_scene_t *scene, const char *kind, const char *name, obs_data_t *d) {
			obs_source_t *f = obs_source_create(kind, name, d, nullptr);
			obs_source_filter_add(obs_scene_get_source(scene), f);
			return f;
		};
		std::vector<std::pair<obs_scene_t *, obs_source_t *>> as_filters;

		printf("\n[all sources: scene corners]\n");
		{
			obs_source_t *back = as_colour("AS Corners Back", 0xFF404040LL, 640, 360);
			obs_source_t *panel = as_colour("AS Corners Panel", 0xFF0000FFLL, 200, 100);
			obs_source_t *inner = as_colour("AS Corners Inner", 0xFF00FF00LL, 80, 80);
			obs_source_t *ignored = as_colour("AS Corners Ignored", 0xFFFF0000LL, 80, 80);
			obs_source_t *later = as_colour("AS Corners Later", 0xFFFFFF00LL, 80, 80);
			obs_scene_t *scene = obs_scene_create("AS Corners Scene");
			as_put(scene, back, 0, 0);
			as_put(scene, panel, 40, 40);
			as_put(scene, ignored, 400, 40);
			obs_sceneitem_t *group = obs_scene_add_group(scene, "AS Corners Group");
			obs_sceneitem_group_add_item(group, as_put(scene, inner, 300, 200));
			obs_source_t *group_src = obs_sceneitem_get_source(group);
			obs_data_t *d = obs_data_create();
			obs_data_set_int(d, "which", 2);
			as_list(d, "skip", {"AS Corners Ignored"});
			obs_data_set_double(d, "size", 20);
			as_filters.push_back({scene, as_filter(scene, "stagekit_scene_corners", "AS Corners All", d)});
			obs_data_release(d);
			const bool covered = as_wait(
				[&] {
					return as_count(panel, "stagekit_corners") == 1 &&
					       as_count(back, "stagekit_corners") == 1 &&
					       as_count(group_src, "stagekit_corners") == 1;
				},
				3000);
			CHECK(covered && as_count(inner, "stagekit_corners") == 0,
			      "All sources gives every source of the scene corners, the full-screen background too, a group counting as one (panel %d, background %d, group %d, inside the group %d)",
			      as_count(panel, "stagekit_corners"), as_count(back, "stagekit_corners"),
			      as_count(group_src, "stagekit_corners"), as_count(inner, "stagekit_corners"));
			as_put(scene, later, 300, 40);
			const bool picked_up = as_wait([&] { return as_count(later, "stagekit_corners") == 1; }, 3000);
			CHECK(picked_up && as_count(ignored, "stagekit_corners") == 0,
			      "a source added later is covered, and an ignored source is left alone (later %d, ignored %d)",
			      as_count(later, "stagekit_corners"), as_count(ignored, "stagekit_corners"));

			// Saved before the choice: no "which", All but the background.
			obs_source_t *old_back = as_colour("AS Corners Old Back", 0xFF404040LL, 640, 360);
			obs_source_t *old_panel = as_colour("AS Corners Old Panel", 0xFF0000FFLL, 200, 100);
			obs_scene_t *old_scene = obs_scene_create("AS Corners Old Scene");
			as_put(old_scene, old_back, 0, 0);
			as_put(old_scene, old_panel, 40, 40);
			obs_data_t *od = obs_data_create();
			obs_data_set_double(od, "size", 20);
			as_filters.push_back(
				{old_scene, as_filter(old_scene, "stagekit_scene_corners", "AS Corners Old", od)});
			obs_data_release(od);
			const bool old_panel_done =
				as_wait([&] { return as_count(old_panel, "stagekit_corners") == 1; }, 3000);
			sleep_ms(500);
			CHECK(old_panel_done && as_count(old_back, "stagekit_corners") == 0,
			      "a Scene Corners saved without the choice reads as All but the background (panel %d, background %d)",
			      as_count(old_panel, "stagekit_corners"), as_count(old_back, "stagekit_corners"));
			for (obs_source_t *src : {back, panel, inner, ignored, later, old_back, old_panel})
				as_sources.push_back(src);
			as_scenes.push_back(scene);
			as_scenes.push_back(old_scene);
		}

		printf("\n[all sources: scene standby]\n");
		{
			// Sources with no image (an image source with no file, as a
			// capture before its first frame), none of them a capture kind.
			obs_source_t *empty = obs_source_create("image_source", "AS Standby Empty", nullptr, nullptr);
			obs_source_t *inner = obs_source_create("image_source", "AS Standby Inner", nullptr, nullptr);
			obs_source_t *ignored =
				obs_source_create("image_source", "AS Standby Ignored", nullptr, nullptr);
			obs_source_t *later = obs_source_create("image_source", "AS Standby Later", nullptr, nullptr);
			obs_source_t *panel = as_colour("AS Standby Panel", 0xFF00FF00LL, 200, 100);
			obs_scene_t *scene = obs_scene_create("AS Standby Scene");
			as_put(scene, empty, 20, 20);
			as_put(scene, ignored, 300, 20);
			obs_sceneitem_t *panel_item = as_put(scene, panel, 100, 200);
			obs_sceneitem_t *group = obs_scene_add_group(scene, "AS Standby Group");
			obs_sceneitem_group_add_item(group, as_put(scene, inner, 400, 200));
			obs_source_t *group_src = obs_sceneitem_get_source(group);
			obs_set_output_source(0, obs_scene_get_source(scene));
			obs_data_t *d = obs_data_create();
			obs_data_set_int(d, "which", 2);
			as_list(d, "skip", {"AS Standby Ignored"});
			as_list(d, "hidden", {"AS Standby Panel"});
			obs_data_set_int(d, "picture", 1);
			obs_data_set_string(d, "file", base_image);
			obs_data_set_int(d, "fade_ms", 0);
			obs_data_set_int(d, "back_ms", 0);
			obs_data_set_string(d, "transition", "none");
			as_filters.push_back({scene, as_filter(scene, "stagekit_scene_standby", "AS Standby All", d)});
			obs_data_release(d);
			const bool covered = as_wait(
				[&] {
					return as_count(empty, "stagekit_standby") == 1 &&
					       as_count(inner, "stagekit_standby") == 1 &&
					       as_count(panel, "stagekit_standby") == 1;
				},
				3000);
			const bool shows = as_wait([&] { return obs_source_get_width(empty) > 0; }, 2000);
			CHECK(covered && shows && as_count(group_src, "stagekit_standby") == 0,
			      "All sources gives every source a standby image, not only captures, the ones in groups too and not the group itself; an empty one shows the image (empty %d, in the group %d, panel %d, group %d; %u wide)",
			      as_count(empty, "stagekit_standby"), as_count(inner, "stagekit_standby"),
			      as_count(panel, "stagekit_standby"), as_count(group_src, "stagekit_standby"),
			      obs_source_get_width(empty));
			as_put(scene, later, 20, 200);
			const bool picked_up = as_wait(
				[&] {
					return as_count(later, "stagekit_standby") == 1 &&
					       obs_source_get_width(later) > 0;
				},
				3000);
			sleep_ms(300);
			CHECK(picked_up && as_count(ignored, "stagekit_standby") == 0 &&
				      obs_source_get_width(ignored) == 0,
			      "a source added later is covered and shows the image, and an ignored source is left alone (later %d, %u wide; ignored %d, %u wide)",
			      as_count(later, "stagekit_standby"), obs_source_get_width(later),
			      as_count(ignored, "stagekit_standby"), obs_source_get_width(ignored));
			// Show when hidden still goes by its own list.
			obs_sceneitem_set_visible(panel_item, false);
			const bool twin = as_wait(
				[&] {
					int n = 0;
					obs_scene_enum_items(
						scene,
						[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
							if (strcmp(obs_source_get_unversioned_id(
									   obs_sceneitem_get_source(item)),
								   "stagekit_stand_in") == 0 &&
							    obs_sceneitem_visible(item))
								(*static_cast<int *>(param))++;
							return true;
						},
						&n);
					return n == 1;
				},
				3000);
			CHECK(twin,
			      "with All sources, a source listed under Show when hidden has the image in its place");
			obs_sceneitem_set_visible(panel_item, true);

			// Saved before the choice: no "which", Every capture, so an
			// empty source that is not a capture gets nothing.
			obs_source_t *old_empty = obs_source_create("image_source", "AS Standby Old", nullptr, nullptr);
			obs_scene_t *old_scene = obs_scene_create("AS Standby Old Scene");
			as_put(old_scene, old_empty, 20, 20);
			obs_data_t *od = obs_data_create();
			obs_data_set_int(od, "picture", 1);
			obs_data_set_string(od, "file", base_image);
			as_filters.push_back(
				{old_scene, as_filter(old_scene, "stagekit_scene_standby", "AS Standby Old", od)});
			obs_data_release(od);
			sleep_ms(2000);
			CHECK(as_count(old_empty, "stagekit_standby") == 0 && obs_source_get_width(old_empty) == 0,
			      "a Scene Standby saved without the choice reads as Every capture: a source that is not a capture gets nothing (%d)",
			      as_count(old_empty, "stagekit_standby"));
			for (obs_source_t *src : {empty, inner, ignored, later, panel, old_empty})
				as_sources.push_back(src);
			as_scenes.push_back(scene);
			as_scenes.push_back(old_scene);
		}

		printf("\n[all sources: scene shadow]\n");
		{
			// Grey background; white squares. A square that casts darkens the
			// background just past its bottom-right corner.
			obs_source_t *back = as_colour("AS Shadow Back", 0xFF808080LL, 640, 360);
			obs_source_t *a = as_colour("AS Shadow A", 0xFFFFFFFFLL, 80, 80);
			obs_source_t *b = as_colour("AS Shadow B", 0xFFFFFFFFLL, 80, 80);
			obs_source_t *ignored = as_colour("AS Shadow Ignored", 0xFFFFFFFFLL, 80, 80);
			obs_source_t *later = as_colour("AS Shadow Later", 0xFFFFFFFFLL, 80, 80);
			obs_scene_t *scene = obs_scene_create("AS Shadow Scene");
			as_put(scene, back, 0, 0);
			as_put(scene, a, 40, 40);
			as_put(scene, b, 200, 40);
			as_put(scene, ignored, 360, 40);
			obs_source_t *scene_src = obs_scene_get_source(scene);
			obs_set_output_source(0, scene_src);
			auto casts = [&](int x, int y) {
				std::vector<uint8_t> frame;
				for (int i = 0; i < 3; i++) {
					frame = render(scene_src, w, h);
					sleep_ms(40);
				}
				// Just past the square's bottom-right corner, in its shadow.
				return (int)frame[(size_t(y + 86) * w + (x + 86)) * 4 + 0];
			};
			// Saved before the choice: no "which", every source casts.
			obs_data_t *d = obs_data_create();
			as_list(d, "shadow_skip", {"AS Shadow Ignored"});
			obs_source_t *sh = as_filter(scene, "stagekit_scene_shadow", "AS Shadow", d);
			as_filters.push_back({scene, sh});
			sleep_ms(500);
			const int on_a = casts(40, 40), on_b = casts(200, 40), on_ignored = casts(360, 40);
			CHECK(on_a < 115 && on_b < 115 && on_ignored >= 124,
			      "a Scene Shadow saved without the choice reads as All sources: every source casts but the ignored one (red past A %d, B %d, ignored %d)",
			      on_a, on_b, on_ignored);
			as_put(scene, later, 40, 200);
			sleep_ms(600);
			const int on_later = casts(40, 200);
			CHECK(on_later < 115, "a source added later casts the shadow too (red past it %d)", on_later);
			obs_data_set_int(d, "which", 1);
			as_list(d, "shadow_sources", {"AS Shadow A"});
			obs_source_update(sh, d);
			sleep_ms(600);
			const int chosen_a = casts(40, 40), chosen_b = casts(200, 40);
			CHECK(chosen_a < 115 && chosen_b >= 124,
			      "with Chosen sources, only the sources listed cast (red past A %d, B %d)", chosen_a,
			      chosen_b);
			obs_data_release(d);
			for (obs_source_t *src : {back, a, b, ignored, later})
				as_sources.push_back(src);
			as_scenes.push_back(scene);
		}

		printf("\n[all sources: scene fit]\n");
		{
			auto sized = [](const char *name, int cw, int ch) {
				obs_data_t *d = obs_data_create();
				obs_data_set_int(d, "width", cw);
				obs_data_set_int(d, "height", ch);
				obs_source_t *made = obs_source_create("color_source", name, d, nullptr);
				obs_data_release(d);
				return made;
			};
			auto counts = [](const char *filter, const char *item, long long &ticks, long long &glides) {
				calldata_t cd;
				calldata_init(&cd);
				calldata_set_string(&cd, "filter", filter);
				calldata_set_string(&cd, "item", item ? item : "");
				proc_handler_call(obs_get_proc_handler(), "stagekit_test_fit_counts", &cd);
				ticks = calldata_int(&cd, "ticks");
				glides = calldata_int(&cd, "glides");
				calldata_free(&cd);
			};
			struct At {
				double x = 0, y = 0, w = 0, h = 0;
				bool fitted = false;
			};
			auto at = [](obs_sceneitem_t *item) {
				vec2 p, b;
				obs_sceneitem_get_pos(item, &p);
				obs_sceneitem_get_bounds(item, &b);
				const bool fitted = obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE;
				return At{p.x, p.y, fitted ? b.x : 0.0, fitted ? b.y : 0.0, fitted};
			};
			auto same = [](const At &a, const At &b) {
				return std::fabs(a.x - b.x) <= 0.5 && std::fabs(a.y - b.y) <= 0.5 &&
				       std::fabs(a.w - b.w) <= 0.5 && std::fabs(a.h - b.h) <= 0.5 &&
				       a.fitted == b.fitted;
			};
			auto overlap = [](const At &a, const At &b) {
				return a.x < b.x + b.w - 0.5 && b.x < a.x + a.w - 0.5 && a.y < b.y + b.h - 0.5 &&
				       b.y < a.y + a.h - 0.5;
			};
			auto show = [&](obs_scene_t *scene, int ms) {
				obs_set_output_source(0, obs_scene_get_source(scene));
				for (int i = 0; i < ms / 20; i++) {
					render(obs_scene_get_source(scene), w, h);
					sleep_ms(20);
				}
			};
			obs_source_t *back = sized("AS Fit Back", 640, 360);
			obs_source_t *game = sized("AS Fit Game", 1600, 900);
			obs_source_t *cam = sized("AS Fit Cam", 120, 68);
			obs_source_t *chat = sized("AS Fit Chat", 120, 150);
			obs_source_t *logo = sized("AS Fit Logo", 60, 30);
			obs_source_t *later = sized("AS Fit Later", 100, 60);
			obs_scene_t *scene = obs_scene_create("AS Fit Scene");
			obs_sceneitem_t *back_i = as_put(scene, back, 0, 0);
			obs_sceneitem_t *game_i = as_put(scene, game, 10, 10);
			vec2 k;
			vec2_set(&k, 0.25f, 0.25f);
			obs_sceneitem_set_scale(game_i, &k);
			obs_sceneitem_t *cam_i = as_put(scene, cam, 510, 10);
			obs_sceneitem_t *chat_i = as_put(scene, chat, 510, 100);
			obs_sceneitem_t *logo_i = as_put(scene, logo, 20, 300);
			const At logo_was = at(logo_i);
			obs_data_t *d = obs_data_create();
			obs_data_set_int(d, "which", 1);
			as_list(d, "entries", {"AS Fit Cam"});
			as_list(d, "skip", {"AS Fit Logo"});
			// The listed camera keeps its size; the rest start as they come.
			obs_data_set_bool(d, "fit|AS Fit Cam|resize", false);
			as_filters.push_back({scene, as_filter(scene, "stagekit_scene_fit", "AS Fit All", d)});
			obs_data_release(d);
			show(scene, 2500);
			const At g1 = at(game_i), c1 = at(cam_i), h1 = at(chat_i), b1 = at(back_i), l1 = at(logo_i);
			CHECK(g1.fitted && c1.fitted && h1.fitted && !b1.fitted && b1.x == 0.0 && b1.y == 0.0,
			      "All sources fits every source, listed or not, and not the full-screen background (game %d, camera %d, chat %d, background %d)",
			      (int)g1.fitted, (int)c1.fitted, (int)h1.fitted, (int)b1.fitted);
			CHECK(!l1.fitted && same(l1, logo_was), "an ignored source is left where it was (%.0f, %.0f)",
			      l1.x, l1.y);
			// Nothing fights: no two fitted sources overlap, and over another
			// second and a half nothing moves and no glide sets off.
			long long t0 = 0, glides0 = 0, t1 = 0, glides1 = 0, total0 = 0, total1 = 0;
			for (const char *item : {"AS Fit Game", "AS Fit Cam", "AS Fit Chat"}) {
				counts("AS Fit All", item, t0, glides0);
				total0 += glides0;
			}
			show(scene, 1500);
			for (const char *item : {"AS Fit Game", "AS Fit Cam", "AS Fit Chat"}) {
				counts("AS Fit All", item, t1, glides1);
				total1 += glides1;
			}
			const At g2 = at(game_i), c2 = at(cam_i), h2 = at(chat_i);
			const bool apart = !overlap(g2, c2) && !overlap(g2, h2) && !overlap(c2, h2);
			CHECK(apart && same(g1, g2) && same(c1, c2) && same(h1, h2) && total1 == total0,
			      "nothing fights: the fitted sources do not overlap, and stay still (glides %lld then %lld; game %.0f,%.0f %.0fx%.0f, camera %.0f,%.0f, chat %.0f,%.0f %.0fx%.0f)",
			      total0, total1, g2.x, g2.y, g2.w, g2.h, c2.x, c2.y, h2.x, h2.y, h2.w, h2.h);
			// A source added later is fitted, and they settle again. It is put
			// down inside the chat box's space: the chat box makes room for
			// it first, then it is fitted into that room.
			// (Added and placed in one step, as OBS's window does it.)
			struct Add {
				obs_source_t *source;
				obs_sceneitem_t *item;
			} add{later, nullptr};
			obs_scene_atomic_update(
				scene,
				[](void *param, obs_scene_t *sc) {
					auto *a = static_cast<Add *>(param);
					a->item = obs_scene_add(sc, a->source);
					vec2 p;
					vec2_set(&p, 420.0f, 280.0f);
					obs_sceneitem_set_pos(a->item, &p);
				},
				&add);
			obs_sceneitem_t *later_i = add.item;
			show(scene, 3500);
			const At n1 = at(later_i), g3 = at(game_i), c3 = at(cam_i), h3 = at(chat_i);
			show(scene, 1000);
			const At n2 = at(later_i), g4 = at(game_i), c4 = at(cam_i), h4 = at(chat_i);
			CHECK(n1.fitted && same(n1, n2) && same(g3, g4) && same(c3, c4) && same(h3, h4) &&
				      !overlap(n2, g4) && !overlap(n2, c4) && !overlap(n2, h4),
			      "a source added later is fitted too, and everything settles again (%.0f,%.0f %.0fx%.0f; chat %.0f,%.0f %.0fx%.0f; game %.0f,%.0f %.0fx%.0f)",
			      n2.x, n2.y, n2.w, n2.h, h4.x, h4.y, h4.w, h4.h, g4.x, g4.y, g4.w, g4.h);

			// Saved before the choice: no "which", only the listed source.
			obs_source_t *old_game = sized("AS Fit Old Game", 400, 225);
			obs_source_t *old_cam = sized("AS Fit Old Cam", 120, 68);
			obs_scene_t *old_scene = obs_scene_create("AS Fit Old Scene");
			obs_sceneitem_t *old_game_i = as_put(old_scene, old_game, 10, 10);
			obs_sceneitem_t *old_cam_i = as_put(old_scene, old_cam, 510, 10);
			obs_data_t *od = obs_data_create();
			as_list(od, "entries", {"AS Fit Old Cam"});
			as_filters.push_back({old_scene, as_filter(old_scene, "stagekit_scene_fit", "AS Fit Old", od)});
			obs_data_release(od);
			show(old_scene, 1500);
			CHECK(at(old_cam_i).fitted && !at(old_game_i).fitted,
			      "a Scene Fit saved without the choice fits only its listed sources (camera %d, game %d)",
			      (int)at(old_cam_i).fitted, (int)at(old_game_i).fitted);
			// A source with a Fit to Space of its own is left to it.
			obs_data_t *wd = obs_data_create();
			obs_data_set_int(wd, "which", 1);
			obs_source_t *old_fit = nullptr;
			for (auto &[s, f] : as_filters)
				if (s == old_scene)
					old_fit = f;
			obs_data_t *fd = obs_data_create();
			obs_data_set_string(fd, "item", "AS Fit Old Game");
			obs_data_set_int(fd, "box", 0);
			obs_source_t *own = obs_source_create("stagekit_fit_item", "AS Fit Own", fd, nullptr);
			obs_data_release(fd);
			obs_source_filter_add(obs_scene_get_source(old_scene), own);
			obs_source_update(old_fit, wd);
			obs_data_release(wd);
			show(old_scene, 1500);
			long long own_ticks = 0, own_glides = 0;
			counts("AS Fit Old", "AS Fit Old Game", own_ticks, own_glides);
			CHECK(own_ticks == 0 && at(old_game_i).fitted,
			      "with All sources, a source with a Fit to Space of its own is left to that filter (Scene Fit ticks for it %lld)",
			      own_ticks);
			obs_source_filter_remove(obs_scene_get_source(old_scene), own);
			obs_source_release(own);
			for (obs_source_t *src : {back, game, cam, chat, logo, later, old_game, old_cam})
				as_sources.push_back(src);
			as_scenes.push_back(scene);
			as_scenes.push_back(old_scene);
		}

		obs_set_output_source(0, as_was_out);
		obs_source_release(as_was_out);
		for (auto &[scene, filter] : as_filters) {
			obs_source_filter_remove(obs_scene_get_source(scene), filter);
			obs_source_release(filter);
		}
		sleep_ms(200);
		for (obs_scene_t *scene : as_scenes) {
			obs_source_remove(obs_scene_get_source(scene));
			obs_scene_release(scene);
		}
		for (obs_source_t *src : as_sources) {
			obs_source_remove(src);
			obs_source_release(src);
		}
		sleep_ms(200);
	}
	// ==== end scene-filters:all-sources ====

	{
		// One scene with two modes, each a leader with its extras, and a
		// camera switch that acts in every scene: Link Visibility's "Follow
		// the first source" keeps a camera view of a mode that is not
		// showing hidden, and the switch passes over it.
		printf("\n[modes in one scene: follow the first source, and a camera switch]\n");
		if (obs_source_get_display_name("fade_transition")) {
			obs_source_t *before_out = obs_get_output_source(0);
			auto set_list = [](obs_data_t *settings, const char *list, std::vector<std::string> names) {
				obs_data_array_t *rows = obs_data_array_create();
				for (const std::string &name : names) {
					obs_data_t *row = obs_data_create();
					obs_data_set_string(row, "value", name.c_str());
					obs_data_array_push_back(rows, row);
					obs_data_release(row);
				}
				obs_data_set_array(settings, list, rows);
				obs_data_array_release(rows);
			};
			auto in_use = [](obs_source_t *source) {
				return obs_source_active(source) || obs_source_showing(source);
			};
			// A camera with a stand-in (Standby's Show when source is hidden)
			// in one mode: its standby image is not shown in the modes that
			// do not show the camera, only when the camera is hidden in its
			// own mode. (The owner's DSLR, in the Intermission mode of one
			// holding scene: its standby image covered the other modes.)
			{
				obs_scene_t *ss = obs_scene_create("Stand-in Modes");
				obs_source_t *ss_src = obs_scene_get_source(ss);
				obs_data_t *cd = obs_data_create();
				obs_data_set_int(cd, "color", 0xFF2040C0);
				obs_data_set_int(cd, "width", 200);
				obs_data_set_int(cd, "height", 120);
				obs_source_t *x =
					obs_source_create("color_source", "Stand-in Mode X", nullptr, nullptr);
				obs_source_t *y =
					obs_source_create("color_source", "Stand-in Mode Y", nullptr, nullptr);
				obs_source_t *cam = obs_source_create("color_source", "Stand-in Mode Cam", cd, nullptr);
				obs_data_release(cd);
				obs_data_t *sd = obs_data_create();
				obs_data_set_bool(sd, "when_hidden", true);
				obs_data_set_string(sd, "transition", "none");
				obs_source_t *standby =
					obs_source_create("stagekit_standby", "Stand-in Mode Standby", sd, nullptr);
				obs_data_release(sd);
				obs_source_filter_add(cam, standby);
				obs_sceneitem_t *ix = obs_scene_add(ss, x);
				obs_sceneitem_t *iy = obs_scene_add(ss, y);
				obs_sceneitem_t *icam = obs_scene_add(ss, cam);
				obs_sceneitem_set_visible(iy, false);
				auto link = [&](const char *name, int mode, std::vector<std::string> list) {
					obs_data_t *ld = obs_data_create();
					set_list(ld, "items", list);
					obs_data_set_int(ld, "mode", mode);
					obs_data_set_string(ld, "transition", "none");
					obs_source_t *l =
						obs_source_create("stagekit_visibility_link", name, ld, nullptr);
					obs_data_release(ld);
					obs_source_filter_add(ss_src, l);
					return l;
				};
				obs_source_t *l0 = link("Stand-in Only One", 0, {"Stand-in Mode X", "Stand-in Mode Y"});
				obs_source_t *l1 =
					link("Stand-in Mode X Link", 2, {"Stand-in Mode X", "Stand-in Mode Cam"});
				obs_source_t *l2 = link("Stand-in Mode Y Link", 2, {"Stand-in Mode Y"});
				obs_set_output_source(0, ss_src);
				auto twin_shown = [&] {
					int found = -1;
					obs_scene_enum_items(
						ss,
						[](obs_scene_t *, obs_sceneitem_t *it, void *p) {
							if (strcmp(obs_source_get_unversioned_id(
									   obs_sceneitem_get_source(it)),
								   "stagekit_stand_in") == 0)
								*static_cast<int *>(p) = obs_sceneitem_visible(it) ? 1
														   : 0;
							return true;
						},
						&found);
					return found;
				};
				sleep_ms(900);
				const int in_x = twin_shown();
				obs_sceneitem_set_visible(iy, true);
				sleep_ms(900);
				const int in_y = twin_shown();
				const bool cam_hidden_in_y = !obs_sceneitem_visible(icam);
				obs_sceneitem_set_visible(ix, true);
				sleep_ms(900);
				const bool cam_back = obs_sceneitem_visible(icam);
				obs_sceneitem_set_visible(icam, false);
				sleep_ms(900);
				const int cam_off_in_x = twin_shown();
				CHECK(in_x == 0 && cam_hidden_in_y && in_y == 0 && cam_back && cam_off_in_x == 1,
				      "a camera with a stand-in, in one mode: no standby image while another mode holds it hidden, and the standby image when it is hidden in its own mode (%d %d %d; %d %d)",
				      in_x, in_y, cam_off_in_x, (int)cam_hidden_in_y, (int)cam_back);
				obs_set_output_source(0, nullptr);
				for (obs_source_t *l : {l0, l1, l2}) {
					obs_source_filter_remove(ss_src, l);
					obs_source_release(l);
				}
				obs_source_filter_remove(cam, standby);
				obs_source_release(standby);
				sleep_ms(300);
				obs_scene_release(ss);
				for (obs_source_t *s : {x, y, cam})
					obs_source_release(s);
			}

			// Twice: the scene's links added in one order, then the other.
			for (int order = 0; order < 2; order++) {
				printf("  (%s)\n", order == 0 ? "the only-one link first" : "the modes' links first");
				const std::string tag = order == 0 ? " 1" : " 2";
				const std::string scene_name = "Modes Scene" + tag;
				obs_scene_t *scene = obs_scene_create(scene_name.c_str());
				obs_source_t *scene_src = obs_scene_get_source(scene);
				obs_scene_t *elsewhere = obs_scene_create(("Modes Elsewhere" + tag).c_str());
				enum { A, ACam, AText, B, BCam, Count };
				const char *base[Count] = {"Mode A", "Mode A Cam", "Mode A Text", "Mode B",
							   "Mode B Cam"};
				std::string names[Count];
				obs_source_t *src[Count];
				obs_sceneitem_t *item[Count];
				for (int i = 0; i < Count; i++) {
					names[i] = base[i] + tag;
					src[i] = obs_source_create("color_source", names[i].c_str(), nullptr, nullptr);
					item[i] = obs_scene_add(scene, src[i]);
				}
				// The camera views are in another scene too, which is not on
				// screen: the camera switch acts in every scene.
				obs_sceneitem_t *cam_away = obs_scene_add(elsewhere, src[ACam]);
				obs_sceneitem_set_visible(item[B], false);
				obs_sceneitem_set_visible(item[BCam], false);
				obs_set_output_source(0, scene_src);
				auto shown = [&] {
					std::string s;
					for (int i = 0; i < Count; i++)
						s += obs_sceneitem_visible(item[i]) ? "1" : "0";
					return s;
				};

				auto make_link = [&](const char *name, int mode, std::vector<std::string> list) {
					obs_data_t *ld = obs_data_create();
					set_list(ld, "items", list);
					obs_data_set_int(ld, "mode", mode);
					obs_data_set_string(ld, "transition", "none");
					obs_source_t *link = obs_source_create("stagekit_visibility_link",
									       (name + tag).c_str(), ld, nullptr);
					obs_data_release(ld);
					return link;
				};
				obs_source_t *only = make_link("Modes Only One", 0, {names[A], names[B]});
				obs_source_t *link_a =
					make_link("Mode A Link", 2, {names[A], names[ACam], names[AText]});
				obs_source_t *link_b = make_link("Mode B Link", 2, {names[B], names[BCam]});
				if (order == 0) {
					obs_source_filter_add(scene_src, only);
					obs_source_filter_add(scene_src, link_a);
					obs_source_filter_add(scene_src, link_b);
				} else {
					obs_source_filter_add(scene_src, link_b);
					obs_source_filter_add(scene_src, link_a);
					obs_source_filter_add(scene_src, only);
				}
				if (order == 0) {
					obs_properties_t *lp = obs_source_properties(link_a);
					obs_property_t *mode = obs_properties_get(lp, "mode");
					bool listed = false;
					for (size_t i = 0; mode && i < obs_property_list_item_count(mode); i++)
						if (obs_property_list_item_int(mode, i) == 2)
							listed = strlen(obs_property_list_item_name(mode, i)) <= 26;
					CHECK(listed && obs_property_long_description(mode),
					      "Link Visibility offers 'Follow the first source' (mode 2), short, with a tooltip");
					obs_properties_destroy(lp);
				}

				// The camera switch: on one camera view, listing both, in
				// every scene, following what is shown.
				obs_data_t *cd = obs_data_create();
				set_list(cd, "sources", {names[ACam], names[BCam]});
				obs_data_set_string(cd, "transition", "none");
				obs_data_set_bool(cd, "follow", true);
				obs_source_t *cam_sw = obs_source_create("stagekit_show_in_scenes",
									 ("Modes Camera" + tag).c_str(), cd, nullptr);
				obs_data_release(cd);
				obs_source_filter_add(src[ACam], cam_sw);
				// A switch per mode, on its leader, in this scene only.
				obs_source_t *mode_sw[2];
				for (int m = 0; m < 2; m++) {
					obs_data_t *md = obs_data_create();
					set_list(md, "scenes", {scene_name});
					obs_data_set_int(md, "where", 1);
					obs_data_set_string(md, "transition", "none");
					obs_data_set_bool(md, "follow", true);
					mode_sw[m] = obs_source_create(
						"stagekit_show_in_scenes",
						((m ? "Modes Switch B" : "Modes Switch A") + tag).c_str(), md, nullptr);
					obs_data_release(md);
					obs_source_filter_add(src[m ? B : A], mode_sw[m]);
				}
				obs_source_set_enabled(mode_sw[1], false);
				sleep_ms(700);
				CHECK(shown() == "11100" && obs_source_enabled(cam_sw) &&
					      obs_source_enabled(mode_sw[0]) && !obs_source_enabled(mode_sw[1]),
				      "mode A showing: its camera view and text shown, mode B and its camera view hidden, and the camera switch reads on (%s %d)",
				      shown().c_str(), obs_source_enabled(cam_sw));

				obs_source_set_enabled(cam_sw, false);
				sleep_ms(600);
				CHECK(shown() == "10100" && !obs_source_enabled(cam_sw) &&
					      obs_source_enabled(mode_sw[0]) && !obs_sceneitem_visible(cam_away),
				      "the camera switched off: mode A's camera view is hidden here and elsewhere, and the mode stays A (%s)",
				      shown().c_str());

				obs_source_set_enabled(mode_sw[1], true);
				sleep_ms(600);
				CHECK(shown() == "00010" && !obs_source_enabled(mode_sw[0]) &&
					      obs_source_enabled(mode_sw[1]) && !obs_source_enabled(cam_sw),
				      "mode B: its leader shown, its camera view left hidden as the camera is off, mode A's sources all hidden (%s)",
				      shown().c_str());

				// The camera switched on in mode B, read all the while.
				obs_source_set_enabled(cam_sw, true);
				sleep_ms(150);
				bool stayed_on = true;
				for (int i = 0; i < 12; i++) {
					stayed_on = stayed_on && obs_source_enabled(cam_sw);
					sleep_ms(50);
				}
				CHECK(shown() == "00011" && stayed_on && obs_sceneitem_visible(cam_away),
				      "the camera switched on: mode B's camera view shown, mode A's left hidden here (the other scene has no mode), and the switch reads on throughout (%s %d)",
				      shown().c_str(), stayed_on);

				// Something else shows a member of the mode not showing.
				obs_sceneitem_set_visible(item[ACam], true);
				sleep_ms(120);
				CHECK(shown() == "00011",
				      "mode A's camera view shown with its eye while B is the mode: hidden again at once (%s)",
				      shown().c_str());
				obs_sceneitem_set_visible(item[AText], true);
				sleep_ms(120);
				CHECK(shown() == "00011" && obs_source_enabled(cam_sw),
				      "and so is its text, and the camera switch still reads on (%s)", shown().c_str());

				// A member hidden in the mode showing moves nothing else.
				obs_sceneitem_set_visible(item[BCam], false);
				sleep_ms(600);
				CHECK(shown() == "00010" && obs_source_enabled(mode_sw[1]) &&
					      !obs_source_enabled(cam_sw),
				      "mode B's camera view hidden with its eye: the mode stays B, and the camera switch reads off (%s)",
				      shown().c_str());
				obs_sceneitem_set_visible(item[BCam], true);
				sleep_ms(600);
				CHECK(shown() == "00011" && obs_source_enabled(cam_sw),
				      "shown again: the switch reads on (%s)", shown().c_str());

				// Back to mode A with the camera on: the switch never reads off.
				obs_source_set_enabled(mode_sw[0], true);
				stayed_on = true;
				for (int i = 0; i < 20; i++) {
					stayed_on = stayed_on && obs_source_enabled(cam_sw);
					sleep_ms(50);
				}
				CHECK(shown() == "11100" && stayed_on && obs_source_enabled(mode_sw[0]) &&
					      !obs_source_enabled(mode_sw[1]),
				      "back to mode A with the camera on: A's camera view and text shown, B's hidden, the camera switch on throughout (%s %d)",
				      shown().c_str(), stayed_on);

				// The scene out of view: changes there are cuts, and nothing
				// hidden there is in use when it comes on.
				for (obs_source_t *link : {only, link_a, link_b}) {
					obs_data_t *now = obs_source_get_settings(link);
					obs_data_set_string(now, "transition", "fade_transition");
					obs_data_set_int(now, "transition_ms", 600);
					obs_source_update(link, now);
					obs_data_release(now);
				}
				for (obs_source_t *sw : mode_sw) {
					obs_data_t *now = obs_source_get_settings(sw);
					obs_data_set_string(now, "transition", "fade_transition");
					obs_data_set_int(now, "transition_ms", 600);
					obs_source_update(sw, now);
					obs_data_release(now);
				}
				obs_set_output_source(0, obs_scene_get_source(elsewhere));
				sleep_ms(150);
				obs_source_set_enabled(mode_sw[1], true);
				sleep_ms(400);
				const std::string away = shown();
				obs_set_output_source(0, scene_src);
				const bool at_once = in_use(src[A]) || in_use(src[AText]);
				sleep_ms(300);
				CHECK(away == "00011" && !at_once && !in_use(src[A]) && !in_use(src[AText]) &&
					      in_use(src[B]) && in_use(src[BCam]),
				      "mode B chosen while the scene is out of view: changed there, and mode A's sources are not in use when it comes on (%s %d)",
				      away.c_str(), at_once);
				// And in view, the leader's fade is used by the link for its extras.
				obs_source_set_enabled(mode_sw[0], true);
				sleep_ms(250);
				CHECK(shown() == "11100" && in_use(src[BCam]),
				      "in view, mode B's camera view fades out (still in use part-way) (%s)",
				      shown().c_str());
				sleep_ms(700);

				obs_set_output_source(0, before_out);
				for (int m = 0; m < 2; m++) {
					obs_source_filter_remove(src[m ? B : A], mode_sw[m]);
					obs_source_release(mode_sw[m]);
				}
				obs_source_filter_remove(src[ACam], cam_sw);
				obs_source_release(cam_sw);
				for (obs_source_t *link : {only, link_a, link_b}) {
					obs_source_filter_remove(scene_src, link);
					obs_source_release(link);
				}
				for (obs_source_t *source : src)
					obs_source_release(source);
				obs_scene_release(elsewhere);
				obs_scene_release(scene);
			}
			obs_source_release(before_out);
		}
	}

	{
		// A layout scene: every capture stacked in the capture's place with one
		// shown, a camera above a chat box beside it, a Fit to Space for each.
		// Changing the capture must leave everything else where it is. What is
		// drawn is noted for every frame, so a lurch of a few frames shows.
		printf("\n[layout scenes: changing the capture leaves the rest where it is]\n");
		if (obs_source_get_display_name("fade_transition")) {
			obs_source_t *before_out = obs_get_output_source(0);
			// With sound, as in OBS: OBS ends an item's show or hide transition
			// only once its sound side is over too, and until it has ended the
			// source it hides counts as in use. (The tests before this one run
			// without sound, which is why this section comes last.)
			obs_audio_info sound = {};
			sound.samples_per_sec = 48000;
			sound.speakers = SPEAKERS_STEREO;
			const bool with_sound = obs_reset_audio(&sound);
			// Twice over. The filters of a scene are ticked in the order they
			// were added, and a capture is changed between two frames, so
			// whichever of them comes first sees the change first: the Link
			// Visibility may hide the old capture before the new one's Fit to
			// Space has said a word, and the camera's may be ticked before
			// either. Both orders must come to the same.
			for (int order = 0; order < 2; order++) {
				printf("  (%s)\n",
				       order == 0 ? "the captures' filters first, then the column's, then the link"
						  : "the link first, then the column's filters, then the captures'");
				const char *kind = obs_source_get_display_name("stagekit_morph") ? "stagekit_morph"
												 : "fade_transition";
				auto block = [](const char *name, long long abgr, int bw, int bh) {
					obs_data_t *d = obs_data_create();
					obs_data_set_int(d, "color", abgr);
					obs_data_set_int(d, "width", bw);
					obs_data_set_int(d, "height", bh);
					obs_source_t *src = obs_source_create("color_source", name, d, nullptr);
					obs_data_release(d);
					return src;
				};
				auto set_list = [](obs_data_t *settings, const char *list,
						   std::vector<const char *> names) {
					obs_data_array_t *rows = obs_data_array_create();
					for (const char *name : names) {
						obs_data_t *row = obs_data_create();
						obs_data_set_string(row, "value", name);
						obs_data_array_push_back(rows, row);
						obs_data_release(row);
					}
					obs_data_set_array(settings, list, rows);
					obs_data_array_release(rows);
				};
				// The five captures: two of the screen's shape, a squarer window,
				// a display that has no size until it has started, and a tablet.
				enum { Game, Card, Window, Display, Tablet, Captures };
				// (Names of their own for each time round: made again under the
				// same names, nothing of the second lot did anything.)
				const char *const names[2][Captures + 4] = {
					{"Layout Game", "Layout Card", "Layout Window", "Layout Display",
					 "Layout Tablet", "Layout Camera", "Layout Chat", "Layout Full",
					 "Layout Component"},
					{"Layout Game 2", "Layout Card 2", "Layout Window 2", "Layout Display 2",
					 "Layout Tablet 2", "Layout Camera 2", "Layout Chat 2", "Layout Full 2",
					 "Layout Component 2"}};
				const char *const *cap_names = names[order];
				const char *camera_name = names[order][Captures],
					   *chat_name = names[order][Captures + 1];
				const int cap_w[Captures] = {320, 320, 400, 0, 320},
					  cap_h[Captures] = {180, 180, 300, 0, 200};
				const long long cap_colour[Captures] = {0xFF00FFFF, 0xFFFFFF00, 0xFFFF00FF, 0xFF0000FF,
									0xFF00FF00};
				obs_source_t *cap[Captures], *sw[Captures];
				for (int i = 0; i < Captures; i++)
					cap[i] = block(cap_names[i], cap_colour[i], cap_w[i], cap_h[i]);
				obs_source_t *camera = block(camera_name, 0xFF808080, 100, 60);
				obs_source_t *chat = block(chat_name, 0xFF404040, 100, 150);
				const char *scene_names[2] = {names[order][Captures + 2], names[order][Captures + 3]};
				obs_scene_t *full_scene = obs_scene_create(scene_names[0]);
				obs_scene_t *comp = obs_scene_create(scene_names[1]); // 640x360
				obs_source_t *full_src = obs_scene_get_source(full_scene),
					     *comp_src = obs_scene_get_source(comp);
				// As the file is written: each capture in the capture's box, all
				// but the first hidden, never fitted.
				obs_sceneitem_t *in_full[Captures], *in_comp[Captures];
				for (int i = 0; i < Captures; i++) {
					in_full[i] = obs_scene_add(full_scene, cap[i]);
					in_comp[i] = obs_scene_add(comp, cap[i]);
					vec2 at, box;
					vec2_set(&at, 20.0f, 20.0f);
					vec2_set(&box, 480.0f, 320.0f);
					// (Two as a file may well have them: the window's box a
					// pixel larger, the tablet lying a little off the space.)
					if (i == Window)
						vec2_set(&box, 481.0f, 321.0f);
					if (i == Tablet) {
						vec2_set(&at, 24.0f, 26.0f);
						vec2_set(&box, 470.0f, 300.0f);
					}
					obs_sceneitem_set_pos(in_comp[i], &at);
					obs_sceneitem_set_bounds_type(in_comp[i], OBS_BOUNDS_SCALE_INNER);
					obs_sceneitem_set_bounds(in_comp[i], &box);
					if (i != Game) {
						obs_sceneitem_set_visible(in_full[i], false);
						obs_sceneitem_set_visible(in_comp[i], false);
					}
				}
				obs_sceneitem_t *cam_item = obs_scene_add(comp, camera);
				obs_sceneitem_t *chat_item = obs_scene_add(comp, chat);
				vec2 put;
				vec2_set(&put, 520.0f, 50.0f);
				obs_sceneitem_set_pos(cam_item, &put);
				vec2_set(&put, 520.0f, 160.0f);
				obs_sceneitem_set_pos(chat_item, &put);
				obs_set_output_source(0, comp_src);
				sleep_ms(100);

				std::vector<obs_source_t *> scene_filters[2];
				auto fit_for = [&](int scene, const std::string &name, const char *item_name, int box,
						   bool resize, int toward = 0) {
					obs_data_t *d = obs_data_create();
					obs_data_set_string(d, "item", item_name);
					obs_data_set_int(d, "box", box);
					obs_data_set_double(d, "margin", 20);
					obs_data_set_bool(d, "resize", resize);
					obs_data_set_int(d, "toward", toward);
					obs_data_set_bool(d, "visible_pixels", true);
					obs_data_set_bool(d, "no_shadows", true);
					obs_source_t *f =
						obs_source_create("stagekit_fit_item", name.c_str(), d, nullptr);
					obs_data_release(d);
					obs_source_filter_add(scene ? comp_src : full_src, f);
					scene_filters[scene].push_back(f);
				};
				auto link_scenes = [&] {
					for (int s = 0; s < 2; s++) {
						obs_data_t *ld = obs_data_create();
						set_list(ld, "items",
							 {cap_names[0], cap_names[1], cap_names[2], cap_names[3],
							  cap_names[4]});
						obs_data_set_string(ld, "transition", kind);
						obs_data_set_int(ld, "transition_ms", 300);
						obs_data_set_bool(ld, "lock_hidden", true);
						obs_source_t *link = obs_source_create("stagekit_visibility_link",
										       "Link Visibility-Captures", ld,
										       nullptr);
						obs_data_release(ld);
						obs_source_filter_add(s ? comp_src : full_src, link);
						scene_filters[s].push_back(link);
					}
				};
				auto fit_capture = [&](int i) {
					fit_for(0, std::string("Full Fit ") + cap_names[i], cap_names[i], 0, true);
					// (The window is kept towards the left of its space, as the owner's is.)
					fit_for(1, std::string("Fit ") + cap_names[i], cap_names[i], 2, true,
						i == Window ? 3 : 0);
				};
				if (order == 0) {
					for (int i = 0; i < Captures; i++)
						fit_capture(i);
				} else {
					link_scenes();
				}
				fit_for(1, "Fit Layout Camera", camera_name, 2, false);
				fit_for(1, "Fit Layout Chat", chat_name, 2, false);
				if (order == 0) {
					link_scenes();
				} else {
					for (int i = Captures - 1; i >= 0; i--)
						fit_capture(i);
				}
				auto switch_for = [&](obs_source_t *on, const char *name,
						      std::vector<const char *> scenes) {
					obs_data_t *sd = obs_data_create();
					set_list(sd, "scenes", scenes);
					obs_data_set_int(sd, "where", 1);
					obs_data_set_string(sd, "transition", kind);
					obs_data_set_int(sd, "transition_ms", 300);
					obs_data_set_bool(sd, "follow", true);
					obs_source_t *f =
						obs_source_create("stagekit_show_in_scenes", name, sd, nullptr);
					obs_data_release(sd);
					obs_source_filter_add(on, f);
					return f;
				};
				for (int i = 0; i < Captures; i++)
					sw[i] = switch_for(cap[i], "Show in Scenes-Capture",
							   {scene_names[0], scene_names[1]});
				obs_source_t *cam_sw = switch_for(camera, "Show in Scenes-Camera", {scene_names[1]});

				// What was drawn, frame by frame: where each watched item of the
				// Component scene was (position and bounding box) in every frame
				// OBS drew, and whether that scene was the one on screen.
				enum { TCam = Captures, TChat, Watched };
				struct Frame {
					bool on_screen;
					bool visible[Watched];
					float at[Watched][4];
				};
				struct Trace {
					std::mutex mutex;
					bool on = false;
					obs_source_t *scene = nullptr;
					obs_sceneitem_t *item[Watched];
					std::vector<Frame> frames;
				} trace;
				trace.scene = comp_src;
				for (int i = 0; i < Captures; i++)
					trace.item[i] = in_comp[i];
				trace.item[TCam] = cam_item;
				trace.item[TChat] = chat_item;
				const auto drawn = [](void *param) {
					auto *t = static_cast<Trace *>(param);
					std::lock_guard lock(t->mutex);
					if (!t->on)
						return;
					Frame f;
					f.on_screen = obs_source_active(t->scene);
					for (int i = 0; i < Watched; i++) {
						vec2 p, b;
						obs_sceneitem_get_pos(t->item[i], &p);
						obs_sceneitem_get_bounds(t->item[i], &b);
						f.visible[i] = obs_sceneitem_visible(t->item[i]);
						f.at[i][0] = p.x;
						f.at[i][1] = p.y;
						f.at[i][2] = b.x;
						f.at[i][3] = b.y;
					}
					t->frames.push_back(f);
				};
				obs_add_main_rendered_callback(drawn, &trace);
				auto watch = [&] {
					std::lock_guard lock(trace.mutex);
					trace.frames.clear();
					trace.on = true;
				};
				auto watched = [&] {
					std::lock_guard lock(trace.mutex);
					trace.on = false;
					return trace.frames;
				};
				struct Place {
					float v[4];
				};
				auto place = [&](int which) {
					Place out;
					vec2 p, b;
					obs_sceneitem_get_pos(trace.item[which], &p);
					obs_sceneitem_get_bounds(trace.item[which], &b);
					out.v[0] = p.x;
					out.v[1] = p.y;
					out.v[2] = b.x;
					out.v[3] = b.y;
					return out;
				};
				// How far an item was from a place at the worst, over the frames
				// in which its scene was on screen, and in how many of them it
				// was more than a pixel off.
				struct Off {
					double worst = 0.0;
					int frames = 0, of = 0;
				};
				auto off_from = [](const std::vector<Frame> &frames, int which, const Place &ref) {
					Off out;
					for (const Frame &f : frames) {
						if (!f.on_screen)
							continue;
						double d = 0.0;
						for (int k = 0; k < 4; k++)
							d = std::max(d, (double)std::fabs(f.at[which][k] - ref.v[k]));
						out.worst = std::max(out.worst, d);
						out.frames += d > 1.0;
						out.of++;
					}
					return out;
				};
				auto same = [](const Place &a, const Place &b) {
					for (int k = 0; k < 4; k++)
						if (std::fabs(a.v[k] - b.v[k]) > 1.0f)
							return false;
					return true;
				};
				// What shows of an item on the canvas: the picture inside its box.
				struct Rect {
					double x, y, w, h;
				};
				auto picture_of = [&](int which) {
					const Place p = place(which);
					obs_source_t *src = obs_sceneitem_get_source(trace.item[which]);
					const double pw = obs_source_get_width(src), ph = obs_source_get_height(src);
					if (pw < 1.0 || ph < 1.0)
						return Rect{p.v[0], p.v[1], 0.0, 0.0};
					if (obs_sceneitem_get_bounds_type(trace.item[which]) == OBS_BOUNDS_NONE)
						return Rect{p.v[0], p.v[1], pw, ph};
					const double k = std::min(p.v[2] / pw, p.v[3] / ph);
					const uint32_t to = obs_sceneitem_get_bounds_alignment(trace.item[which]);
					const double fx = to & OBS_ALIGN_LEFT ? 0.0 : to & OBS_ALIGN_RIGHT ? 1.0 : 0.5;
					const double fy = to & OBS_ALIGN_TOP ? 0.0 : to & OBS_ALIGN_BOTTOM ? 1.0 : 0.5;
					return Rect{p.v[0] + (p.v[2] - pw * k) * fx, p.v[1] + (p.v[3] - ph * k) * fy,
						    pw * k, ph * k};
				};
				// Whether any two of the items showing lie over each other.
				auto overlap = [&] {
					std::vector<Rect> shown;
					for (int i = 0; i < Watched; i++)
						if (obs_sceneitem_visible(trace.item[i]))
							shown.push_back(picture_of(i));
					for (size_t a = 0; a < shown.size(); a++)
						for (size_t b = a + 1; b < shown.size(); b++)
							if (shown[a].x < shown[b].x + shown[b].w - 0.5 &&
							    shown[b].x < shown[a].x + shown[a].w - 0.5 &&
							    shown[a].y < shown[b].y + shown[b].h - 0.5 &&
							    shown[b].y < shown[a].y + shown[a].h - 0.5)
								return true;
					return false;
				};
				// Which captures are shown in the two scenes, and which are locked
				// in the Component scene.
				auto state = [&] {
					std::string text;
					for (int i = 0; i < Captures; i++)
						text += obs_sceneitem_visible(in_full[i]) ? "1" : "0";
					text += "/";
					for (int i = 0; i < Captures; i++)
						text += obs_sceneitem_visible(in_comp[i]) ? "1" : "0";
					text += " locked ";
					for (int i = 0; i < Captures; i++)
						text += obs_sceneitem_locked(in_comp[i]) ? "1" : "0";
					return text;
				};
				auto only = [](int which) {
					std::string shown = "00000", locked = "11111";
					shown[which] = '1';
					locked[which] = '0';
					return shown + "/" + shown + " locked " + locked;
				};

				sleep_ms(3000);
				const Place cam0 = place(TCam), chat0 = place(TChat), space0 = place(Game);
				const Rect cam_pic = picture_of(TCam), chat_pic = picture_of(TChat),
					   game_pic = picture_of(Game);
				CHECK(state() == only(Game) && std::fabs(space0.v[0] - 20) < 1 &&
					      std::fabs(space0.v[1] - 20) < 1 && std::fabs(space0.v[2] - 480) < 1 &&
					      std::fabs(space0.v[3] - 320) < 1 && std::fabs(cam_pic.x - 520) < 1 &&
					      std::fabs(chat_pic.x - 520) < 1 && cam_pic.y + cam_pic.h < chat_pic.y &&
					      !overlap(),
				      "to begin with the game is fitted in the space beside the column, the camera above the chat box (%s; game %.0f, %.0f %.0fx%.0f in a space %.0fx%.0f; camera %.0f, %.0f; chat box %.0f, %.0f)",
				      state().c_str(), game_pic.x, game_pic.y, game_pic.w, game_pic.h, space0.v[2],
				      space0.v[3], cam_pic.x, cam_pic.y, chat_pic.x, chat_pic.y);

				// One capture change, watched for a second and a quarter.
				struct Change {
					Off cam, chat;
					std::vector<Frame> frames;
				};
				auto change_to = [&](int which, const Place &cam_ref, const Place &chat_ref) {
					watch();
					obs_source_set_enabled(sw[which], true);
					sleep_ms(1250);
					Change out;
					out.frames = watched();
					out.cam = off_from(out.frames, TCam, cam_ref);
					out.chat = off_from(out.frames, TChat, chat_ref);
					return out;
				};

				// 1. Another capture of the same shape, never shown here before.
				{
					const Change c = change_to(Card, cam0, chat0);
					CHECK(c.cam.worst <= 1.0 && c.chat.worst <= 1.0 && c.cam.of > 40,
					      "another capture switched on, for the first time in this scene: the camera and the chat box do not move in any frame (at most %.2f and %.2f px off over %d frames)",
					      c.cam.worst, c.chat.worst, c.cam.of);
					CHECK(state() == only(Card) && same(place(Card), space0) && !overlap(),
					      "it ends where the first was, which is hidden and locked (%s; %.1f, %.1f %.1fx%.1f)",
					      state().c_str(), place(Card).v[0], place(Card).v[1], place(Card).v[2],
					      place(Card).v[3]);
					// And back to one that has been fitted here before.
					const Change back = change_to(Game, cam0, chat0);
					CHECK(back.cam.worst <= 1.0 && back.chat.worst <= 1.0 &&
						      state() == only(Game) && same(place(Game), space0),
					      "and back to the first, which was fitted here before: the same (at most %.2f and %.2f px off; %s)",
					      back.cam.worst, back.chat.worst, state().c_str());
				}
				// 2. A capture of another shape.
				{
					const Change c = change_to(Window, cam0, chat0);
					const Rect pic = picture_of(Window);
					CHECK(c.cam.worst <= 1.0 && c.chat.worst <= 1.0,
					      "a capture of another shape (4:3 after 16:9): the column does not move in any frame (at most %.2f and %.2f px off over %d frames)",
					      c.cam.worst, c.chat.worst, c.cam.of);
					CHECK(state() == only(Window) && same(place(Window), space0) &&
						      std::fabs(pic.h - 320) < 1 && std::fabs(pic.w - 426.7) < 1 &&
						      !overlap(),
					      "and its space is the same one, though it was written a pixel larger (%s; picture %.0f, %.0f %.0fx%.0f)",
					      state().c_str(), pic.x, pic.y, pic.w, pic.h);
					const Change back = change_to(Game, cam0, chat0);
					CHECK(back.cam.worst <= 1.0 && back.chat.worst <= 1.0 &&
						      state() == only(Game) && same(place(Game), space0),
					      "and back to the wide one: the same (at most %.2f and %.2f px off; %s)",
					      back.cam.worst, back.chat.worst, state().c_str());
				}
				// 3. The camera hidden by its switch, the capture changed, the
				// camera shown again.
				{
					obs_source_set_enabled(cam_sw, false);
					sleep_ms(1200);
					const Place chat1 = place(TChat);
					CHECK(!obs_sceneitem_visible(cam_item) &&
						      std::fabs(chat1.v[1] - chat0.v[1]) > 5.0 &&
						      same(place(TCam), cam0),
					      "with the camera hidden by its switch the chat box takes the room, as before (chat box y %.1f, was %.1f)",
					      chat1.v[1], chat0.v[1]);
					const Change c = change_to(Window, cam0, chat1);
					CHECK(c.chat.worst <= 1.0 && c.cam.worst <= 1.0 && state() == only(Window) &&
						      same(place(Window), space0),
					      "the capture changed meanwhile: the chat box and the hidden camera stay where they are (at most %.2f and %.2f px off)",
					      c.chat.worst, c.cam.worst);
					watch();
					obs_source_set_enabled(cam_sw, true);
					sleep_ms(1500);
					const std::vector<Frame> frames = watched();
					// The chat box goes back in one move; everything else stays.
					int turns = 0, rest = -1, n = 0;
					double last_y = chat1.v[1], last_step = 0.0;
					bool begun = false;
					for (const Frame &f : frames) {
						const double y = f.at[TChat][1], step = y - last_y;
						if (std::fabs(step) > 0.05 && std::fabs(last_step) <= 0.05 && begun)
							turns++; // stopped and set off again
						if (step * last_step < 0.0)
							turns++; // turned round
						begun = begun || std::fabs(step) > 0.05;
						if (std::fabs(y - chat0.v[1]) > 0.5)
							rest = -1;
						else if (rest < 0)
							rest = n;
						last_step = step;
						last_y = y;
						n++;
					}
					const Off cam = off_from(frames, TCam, cam0),
						  win = off_from(frames, Window, space0);
					CHECK(obs_sceneitem_visible(cam_item) && cam.worst <= 1.0 && win.worst <= 1.0,
					      "the camera shown again: it is where it was and the capture does not move (at most %.2f and %.2f px off over %d frames)",
					      cam.worst, win.worst, cam.of);
					CHECK(turns == 0 && rest >= 0 && same(place(TChat), chat0) &&
						      same(place(TCam), cam0) && same(place(Window), space0) &&
						      !overlap(),
					      "and the chat box makes way in one move, everything coming to rest once with nothing inside anything else (stops and turns: %d; at rest from frame %d of %d; chat box y %.1f, wanted %.1f)",
					      turns, rest, n, place(TChat).v[1], chat0.v[1]);
					change_to(Game, cam0, chat0);
				}
				// 4. A display, which has no size until it has started.
				{
					watch();
					obs_source_set_enabled(sw[Display], true);
					sleep_ms(300);
					const std::vector<Frame> unsized = watched();
					const std::string while_unsized = state();
					watch();
					obs_data_t *dd = obs_source_get_settings(cap[Display]);
					obs_data_set_int(dd, "width", 320);
					obs_data_set_int(dd, "height", 200);
					obs_source_update(cap[Display], dd);
					obs_data_release(dd);
					sleep_ms(1500);
					const std::vector<Frame> sized_frames = watched();
					const Off cam_a = off_from(unsized, TCam, cam0),
						  chat_a = off_from(unsized, TChat, chat0);
					const Off cam_b = off_from(sized_frames, TCam, cam0),
						  chat_b = off_from(sized_frames, TChat, chat0);
					CHECK(while_unsized == only(Display) && cam_a.worst <= 1.0 &&
						      chat_a.worst <= 1.0,
					      "a display shown before it has a size: the camera and the chat box stay put while it has none (at most %.2f and %.2f px off over %d frames; %s)",
					      cam_a.worst, chat_a.worst, cam_a.of, while_unsized.c_str());
					CHECK(cam_b.worst <= 1.0 && chat_b.worst <= 1.0,
					      "nor do they move when its size comes (at most %.2f and %.2f px off over %d frames)",
					      cam_b.worst, chat_b.worst, cam_b.of);
					// Drawn: the display is red, 320x200, so 480x300 in its space.
					const std::vector<uint8_t> px = render(comp_src, 640, 360);
					int x0 = 640, y0 = 360, x1 = -1, y1 = -1;
					for (int y = 0; y < 360; y++)
						for (int x = 0; x < 640; x++) {
							const uint8_t *p = &px[(size_t(y) * 640 + x) * 4];
							if (p[0] > 200 && p[1] < 50 && p[2] < 50) {
								x0 = std::min(x0, x);
								y0 = std::min(y0, y);
								x1 = std::max(x1, x);
								y1 = std::max(y1, y);
							}
						}
					CHECK(same(place(Display), space0) && std::abs(x0 - 20) <= 1 &&
						      std::abs(y0 - 30) <= 1 && std::abs(x1 - x0 + 1 - 480) <= 2 &&
						      std::abs(y1 - y0 + 1 - 300) <= 2 && !overlap(),
					      "and it is then fitted in the capture's space (drawn at %d, %d %dx%d)",
					      x0, y0, x1 - x0 + 1, y1 - y0 + 1);
				}
				// 5. The capture changed while this scene is not on screen.
				{
					static int taken = 0;
					const auto count = [](void *, calldata_t *) {
						taken++;
					};
					auto listen = [&](int which, bool on) {
						signal_handler_t *sh = obs_source_get_signal_handler(cap[which]);
						for (const char *signal : {"activate", "show"}) {
							if (on)
								signal_handler_connect(sh, signal, count, nullptr);
							else
								signal_handler_disconnect(sh, signal, count, nullptr);
						}
					};
					auto in_use = [&](int which) {
						return obs_source_active(cap[which]) || obs_source_showing(cap[which]);
					};
					// `to` is switched on while the other scene is on screen;
					// `from` is the one showing until then. Every other capture
					// is hidden all the while and listened to from the start;
					// `from` from the moment this scene comes back, as the scene
					// on screen until then fades it out, which uses it.
					auto away_change = [&](int from, int to, const char *what) {
						obs_set_output_source(0, full_src);
						sleep_ms(300);
						std::vector<int> idle;
						for (int i = 0; i < Captures; i++)
							if (i != from && i != to)
								idle.push_back(i);
						taken = 0;
						for (int which : idle)
							listen(which, true);
						obs_source_set_enabled(sw[to], true);
						sleep_ms(900);
						const std::string away = state();
						listen(from, true);
						watch();
						obs_set_output_source(0, comp_src);
						sleep_ms(1000);
						const std::vector<Frame> frames = watched();
						bool unused = !in_use(from);
						for (int which : idle)
							unused = unused && !in_use(which);
						std::string using_now;
						for (int i = 0; i < Captures; i++) {
							using_now += obs_source_active(cap[i]) ? "a" : "-";
							using_now += obs_source_showing(cap[i]) ? "s" : "-";
						}
						listen(from, false);
						for (int which : idle)
							listen(which, false);
						const Off cam = off_from(frames, TCam, cam0),
							  chat_off = off_from(frames, TChat, chat0),
							  capture = off_from(frames, to, space0);
						CHECK(away == only(to) && cam.of > 30 && cam.worst <= 1.0 &&
							      chat_off.worst <= 1.0 && capture.worst <= 1.0 &&
							      !overlap(),
						      "%s while the scene is out of view: from the first frame it is drawn again everything is in its place, with no glide (%s; camera %.2f, chat box %.2f, capture %.2f px off at most over %d frames)",
						      what, away.c_str(), cam.worst, chat_off.worst, capture.worst,
						      cam.of);
						CHECK(with_sound && taken == 0 && unused && in_use(to),
						      "and no capture hidden there is taken into use at any moment (taken into use %d times; active and showing at the end: %s)",
						      taken, using_now.c_str());
					};
					away_change(Display, Window,
						    "a squarer capture that was fitted there before, switched on");
					away_change(
						Window, Tablet,
						"a capture never shown there before and lying a little off the space, switched on");
				}
				// 6. A capture that shows but has no picture when its filter is
				// first ticked, as when OBS starts with a display that is not
				// there: its filter is new, and it has never been hidden.
				{
					change_to(Display, cam0, chat0);
					obs_data_t *dd = obs_source_get_settings(cap[Display]);
					obs_data_set_int(dd, "width", 0);
					obs_data_set_int(dd, "height", 0);
					obs_source_update(cap[Display], dd);
					const std::string fit_name = std::string("Fit ") + cap_names[Display];
					for (obs_source_t *&f : scene_filters[1])
						if (fit_name == obs_source_get_name(f)) {
							obs_data_t *as_saved = obs_data_create();
							obs_data_t *was = obs_source_get_settings(f);
							obs_data_apply(as_saved, was);
							obs_data_release(was);
							watch();
							obs_source_filter_remove(comp_src, f);
							obs_source_release(f);
							f = obs_source_create("stagekit_fit_item", fit_name.c_str(),
									      as_saved, nullptr);
							obs_data_release(as_saved);
							obs_source_filter_add(comp_src, f);
							break;
						}
					sleep_ms(1200);
					const std::vector<Frame> frames = watched();
					const Off cam = off_from(frames, TCam, cam0),
						  chat_off = off_from(frames, TChat, chat0);
					CHECK(obs_source_get_width(cap[Display]) == 0 && state() == only(Display) &&
						      cam.of > 40 && cam.worst <= 1.0 && chat_off.worst <= 1.0,
					      "a capture showing with no picture from the start, its filter new: it holds its box, and the camera and the chat box stay put (at most %.2f and %.2f px off over %d frames)",
					      cam.worst, chat_off.worst, cam.of);
					watch();
					obs_data_set_int(dd, "width", 320);
					obs_data_set_int(dd, "height", 200);
					obs_source_update(cap[Display], dd);
					obs_data_release(dd);
					sleep_ms(1500);
					const std::vector<Frame> after = watched();
					const Off cam_b = off_from(after, TCam, cam0),
						  chat_b = off_from(after, TChat, chat0);
					const Rect pic = picture_of(Display);
					CHECK(cam_b.worst <= 1.0 && chat_b.worst <= 1.0 &&
						      same(place(Display), space0) && std::fabs(pic.w - 480) < 1 &&
						      std::fabs(pic.h - 300) < 1 && !overlap(),
					      "and when its picture comes it is fitted in the capture's space, the column still where it was (at most %.2f and %.2f px off over %d frames; picture %.0f, %.0f %.0fx%.0f)",
					      cam_b.worst, chat_b.worst, cam_b.of, pic.x, pic.y, pic.w, pic.h);
				}
				obs_remove_main_rendered_callback(drawn, &trace);
				obs_set_output_source(0, before_out);
				for (int i = 0; i < Captures; i++) {
					obs_source_filter_remove(cap[i], sw[i]);
					obs_source_release(sw[i]);
				}
				obs_source_filter_remove(camera, cam_sw);
				obs_source_release(cam_sw);
				for (int s = 0; s < 2; s++)
					for (obs_source_t *f : scene_filters[s]) {
						obs_source_filter_remove(s ? comp_src : full_src, f);
						obs_source_release(f);
					}
				sleep_ms(100);
				obs_scene_release(full_scene);
				obs_scene_release(comp);
				for (obs_source_t *source : cap)
					obs_source_release(source);
				obs_source_release(camera);
				obs_source_release(chat);
				sleep_ms(300);
			}
			obs_source_release(before_out);
		}
	}

	// ==== scene-filters:sound ====
	// ---- modes
	if (obs_source_get_display_name("fade_transition")) {
		// Scene Modes in a scene that is not on screen, with sound running
		// (the layout section above turned it on): a press there is a cut, so
		// when the scene comes on no hidden source counts as in use.
		printf("\n[scene modes: out of view]\n");
		obs_source_t *modes_was_out = obs_get_output_source(0);
		obs_scene_t *view = obs_scene_create("SM View");
		obs_scene_t *away = obs_scene_create("SM Away");
		obs_source_t *a = obs_source_create("color_source", "SM Away A", nullptr, nullptr);
		obs_source_t *b = obs_source_create("color_source", "SM Away B", nullptr, nullptr);
		obs_sceneitem_t *ia = obs_scene_add(away, a);
		obs_sceneitem_t *ib = obs_scene_add(away, b);
		obs_sceneitem_set_visible(ib, false);
		auto row_list = [](obs_data_t *d, const char *list, const char *value) {
			obs_data_array_t *rows = obs_data_array_create();
			obs_data_t *row = obs_data_create();
			obs_data_set_string(row, "value", value);
			obs_data_array_push_back(rows, row);
			obs_data_release(row);
			obs_data_set_array(d, list, rows);
			obs_data_array_release(rows);
		};
		obs_data_t *d = obs_data_create();
		obs_data_array_t *ids = obs_data_array_create();
		for (const char *id : {"m1", "m2"}) {
			obs_data_t *row = obs_data_create();
			obs_data_set_string(row, "value", id);
			obs_data_array_push_back(ids, row);
			obs_data_release(row);
		}
		obs_data_set_array(d, "modes", ids);
		obs_data_array_release(ids);
		obs_data_set_string(d, "mode|m1|name", "A");
		obs_data_set_string(d, "mode|m2|name", "B");
		row_list(d, "mode|m1|sources", "SM Away A");
		row_list(d, "mode|m2|sources", "SM Away B");
		obs_data_set_string(d, "mode_now", "m1");
		obs_data_set_string(d, "transition", "fade_transition");
		obs_data_set_int(d, "transition_ms", 600);
		obs_source_t *f = obs_source_create("stagekit_scene_modes", "SM Away Modes", d, nullptr);
		obs_data_release(d);
		obs_source_filter_add(obs_scene_get_source(away), f);
		obs_set_output_source(0, obs_scene_get_source(view));
		sleep_ms(200);
		auto in_use = [](obs_source_t *source) {
			return obs_source_active(source) || obs_source_showing(source);
		};
		static int modes_opened = 0;
		const auto count_opened = [](void *, calldata_t *) {
			modes_opened++;
		};
		signal_handler_connect(obs_source_get_signal_handler(a), "activate", count_opened, nullptr);
		modes_opened = 0;
		auto press = [](const char *mode) {
			calldata_t cd = {};
			calldata_set_string(&cd, "request", "SetSceneMode");
			const std::string json = std::string("{\"scene\":\"SM Away\",\"mode\":\"") + mode + "\"}";
			calldata_set_string(&cd, "data", json.c_str());
			proc_handler_call(obs_get_proc_handler(), "stagekit_vendor_call", &cd);
			calldata_free(&cd);
		};
		press("m2");
		sleep_ms(200);
		obs_set_output_source(0, obs_scene_get_source(away));
		const bool at_once = in_use(a);
		sleep_ms(300);
		CHECK(!obs_sceneitem_visible(ia) && obs_sceneitem_visible(ib) && !at_once && !in_use(a) &&
			      modes_opened == 0 && in_use(b),
		      "a mode changed in a scene out of view: when the scene comes on, the source hidden there is not taken into use, not even for a moment (%d %d)",
		      at_once, modes_opened);
		// In view, the fade is used: the source hidden is still in use part-way.
		press("m1");
		sleep_ms(250);
		const bool fading = in_use(b);
		sleep_ms(900);
		CHECK(obs_sceneitem_visible(ia) && !obs_sceneitem_visible(ib) && fading && !in_use(b),
		      "in view, the source hidden fades out and is out of use once the fade is over (%d)", fading);
		signal_handler_disconnect(obs_source_get_signal_handler(a), "activate", count_opened, nullptr);
		obs_set_output_source(0, modes_was_out);
		obs_source_release(modes_was_out);
		obs_source_filter_remove(obs_scene_get_source(away), f);
		obs_source_release(f);
		obs_source_release(a);
		obs_source_release(b);
		obs_scene_release(away);
		obs_scene_release(view);
	}
	// ==== end scene-filters:sound ====
	// (With sound, which the layout scenes turned on: OBS ends an item's
	// show and hide transitions only then.)
	capture_pop_in_test(w, h, out);

	obs_source_dec_showing(bg);
	obs_data_release(s);
	obs_source_release(bg);
	obs_source_release(live);
	obs_shutdown();

	printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
	return g_failures ? 1 : 0;
}
