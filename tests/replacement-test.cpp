#include <obs.h>
#include <obs-module.h>
#include <obs-nix-platform.h>
#include "replacement-frame.hpp"
#include "replacement-native.hpp"
#include <graphics/vec2.h>
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

using replacement_test::render;
using replacement_test::retains_artwork;
using replacement_test::stripe_contrast;

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

	QStringList native_issues;
	replacement_test::check_native_filters(QString::fromUtf8(image), native_issues);
	check(native_issues.isEmpty(), "native component renders and saved IDs: " + native_issues.join(", ").toStdString());

	const auto reference = std::filesystem::path(plugin_bin).parent_path() / "tests" / "obs-original";
	for (auto [name, resources] : {std::pair{"move-transition", "move"},
				       {"obs-shaderfilter", "shader"},
				       {"obs-composite-blur", "composite"}}) {
		const std::string binary = (reference / (std::string(name) + ".so")).string();
		const std::string data = std::string(plugin_data) + "/components/" + resources;
		if (!load_module(binary.c_str(), data.c_str()))
			return 1;
	}
	obs_post_load_modules();
	const char *original_name = obs_get_module_name(obs_get_module("move-transition"));
	const char *bundled_name = obs_get_module_name(obs_get_module("stagekit-port-move"));
	check(original_name && strcmp(original_name, "Move Transition") == 0 && bundled_name &&
		      strcmp(bundled_name, "StageKit - Move Transition") == 0,
	      "standalone and bundled Move Transition report distinct plugin names");
	auto move_callback = [](const char *proc, const char *filter) {
		calldata_t data{};
		calldata_set_string(&data, "filter_id", filter);
		proc_handler_call(obs_get_proc_handler(), proc, &data);
		auto *result = calldata_ptr(&data, "callback");
		calldata_free(&data);
		return result;
	};
	const auto *ported =
		move_callback("stagekit_port_move_get_transition_filter_function", "stagekit_port_shader_filter");
	const auto *original_move = move_callback("move_get_transition_filter_function", "shader_filter");
	check(ported && original_move && ported != original_move,
	      "the original Move module and the StageKit component keep independent callbacks");
	const auto has_label = [](const char *id, const char *prefix) {
		const char *version = obs_get_latest_input_type_id(id);
		const char *name = version ? obs_source_get_display_name(version) : nullptr;
		return name && std::string(name).find(prefix) == 0 &&
		       std::string(name).find("(StageKit)") != std::string::npos;
	};
	const char *source_types[] = {"3d_effect_filter",
				      "advanced_masks_filter",
				      "obs_composite_blur",
				      "obs_retro_effects_filter",
				      "obs_noise_displace_filter",
				      "pixel_art_plugin",
				      "freeze_filter",
				      "dynamic_delay_filter",
				      "recursion_effect_filter",
				      "time_warp_scan_filter",
				      "dir_watch_media",
				      "source_defaults_video",
				      "source_defaults_audio",
				      "move_action_filter",
				      "move_value_filter",
				      "move_transition_override_filter",
				      "shader_filter"};
	for (const auto *id : source_types) {
		const std::string type = "stagekit_port_" + std::string(id);
		check(has_label(type.c_str(), "Source Filter:"), type);
	}
	for (const auto *id : {"stagekit_port_move_source_filter", "stagekit_port_move_source_swap_filter"})
		check(has_label(id, "Scene Filter:"), id);
	for (const auto *id : {"stagekit_effect_file", "stagekit_drop_shadow", "stagekit_edge_detection",
			       "stagekit_gaussian_blur", "stagekit_toon", "stagekit_fix_alpha"})
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
	check(w == 320 && h == 180 && retains_artwork(base_pixels), "the original fixture contains visible artwork");
	if (w != 320 || h != 180)
		return 1;
	write_frame(out + "/original.rgba", base_pixels, w, h);
	struct Example {
		const char *id;
		const char *name;
	};
	const Example examples[] = {{"stagekit_effect_file", "custom"},
				    {"stagekit_drop_shadow", "shadow"},
				    {"stagekit_edge_detection", "edges"},
				    {"stagekit_gaussian_blur", "gaussian"},
				    {"stagekit_toon", "toon"},
				    {"stagekit_port_obs_composite_blur", "composite"},
				    {"stagekit_port_obs_composite_blur", "composite-kawase"},
				    {"stagekit_port_obs_composite_blur", "composite-hex"},
				    {"stagekit_port_shader_filter", "saved-shader"}};
	for (const auto &example : examples) {
		const std::string name(example.name);
		const bool composite = name.compare(0, 9, "composite") == 0;
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
		if (composite)
			replacement_test::configure_blur(settings, name);
		if (name == "saved-shader") {
			obs_data_set_bool(settings, "from_file", true);
			const std::string file =
				std::string(plugin_data) + "/components/shader/examples/drop_shadow.shader";
			obs_data_set_string(settings, "shader_file_name", file.c_str());
			obs_data_set_int(settings, "shadow_offset_x", 9);
			obs_data_set_int(settings, "shadow_offset_y", 12);
			obs_data_set_int(settings, "shadow_blur_size", 4);
		}
		auto *filter = obs_source_create(example.id, example.name, settings, nullptr);
		obs_data_release(settings);
		if (!filter) {
			check(false, name + " could not be created");
			continue;
		}
		obs_source_filter_add(image_source, filter);
		sleep_ms(130);
		auto fw = obs_source_get_width(image_source), fh = obs_source_get_height(image_source);
		check(fw && fh, name + " reports nonzero dimensions");
		if (fw && fh) {
			(void)render(image_source, fw, fh);
			sleep_ms(50);
			auto frame = render(image_source, fw, fh);
			write_frame(out + "/" + name + ".rgba", frame, fw, fh);
			check(retains_artwork(frame), name + " retains visible colored artwork");
			if (composite && fw == w && fh == h)
				check(stripe_contrast(frame, w) < 0.85 * stripe_contrast(base_pixels, w),
				      name + " softens the fine stripes");
			if (name == "shadow") {
				check(fw == w + 9 && fh == h + 12, "shadow extends the frame in both directions");
				bool colour = false;
				for (size_t i = 0; i < frame.size(); i += 4)
					if (frame[i + 3] > 16 && frame[i + 2] > frame[i] + 60) {
						colour = true;
						break;
					}
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
					check(mean_abs_diff(masked, inverted) > 0.12,
					      "inverting the Gaussian mask moves the blur to the other half");
					obs_data_release(mask);
				} else if (name == "custom") {
					auto *bad = obs_data_create();
					obs_data_set_string(bad, "effect_path", (out + "/missing.effect").c_str());
					obs_source_update(filter, bad);
					obs_data_release(bad);
					sleep_ms(100);
					check(mean_abs_diff(frame, render(image_source, fw, fh)) < 0.01,
					      "failed reload retains the working custom effect");
				}
			} else {
				check(false, name + " unexpectedly changed the source size");
			}
		}
		if ((composite || name == "saved-shader") && w == fw && h == fh) {
			const auto port_frame = render(image_source, w, h);
			auto *saved = obs_source_get_settings(filter);
			if (composite) {
				// The supplier sets these two choices in its property dialog.
				obs_data_set_int(saved, "blur_algorithm", obs_data_get_int(saved, "blur_algorithm"));
				obs_data_set_int(saved, "blur_type", obs_data_get_int(saved, "blur_type"));
			}
			const char *original_id = composite ? "obs_composite_blur" : "shader_filter";
			auto *original = obs_source_create(original_id, "Upstream comparison", saved, nullptr);
			obs_data_release(saved);
			obs_source_filter_remove(image_source, filter);
			if (original) {
				obs_source_filter_add(image_source, original);
				sleep_ms(140);
				(void)render(image_source, w, h);
				sleep_ms(60);
				const auto original_frame = render(image_source, w, h);
				check(retains_artwork(original_frame),
				      name + " original renders visible colored artwork");
				const double error = mean_abs_diff(port_frame, original_frame);
				printf("Original and replacement %s mean difference %.4f\n", example.name, error);
				check(error <= 0.10, name + " matches the original plugin on the same OBS source");
				obs_source_filter_remove(image_source, original);
				obs_source_release(original);
			} else {
				check(false, name + " original plugin could not be created");
			}
			obs_source_filter_add(image_source, filter);
		}
		obs_source_filter_remove(image_source, filter);
		obs_source_release(filter);
		sleep_ms(40);
	}
	// Drive the same Start button and saved names used by the original Move filters.
	obs_set_ui_task_handler([](obs_task_t task, void *param, bool) { task(param); });
	auto *scene = obs_scene_create("Move test scene");
	auto *scene_source = obs_scene_get_source(scene);
	auto *item = obs_scene_add(scene, image_source);
	obs_source_inc_showing(scene_source);
	auto start_filter = [](obs_source_t *source) {
		auto *props = obs_source_properties(source);
		auto *start = props ? obs_properties_get(props, "move_filter_start") : nullptr;
		if (!start && props)
			start = obs_properties_get(props, "move_source_start");
		check(start != nullptr, "Move Start control is available");
		if (start)
			obs_property_button_clicked(start, source);
		obs_properties_destroy(props);
	};
	auto *movement = obs_data_create();
	obs_data_set_string(movement, "source", "Test art");
	obs_data_set_bool(movement, "custom_duration", true);
	obs_data_set_int(movement, "duration", 50);
	auto *position = obs_data_create();
	obs_data_set_double(position, "x", 104.0);
	obs_data_set_double(position, "y", 71.0);
	obs_data_set_obj(movement, "pos", position);
	obs_data_release(position);
	auto *move = obs_source_create("stagekit_port_move_source_filter", "Move art", movement, nullptr);
	obs_source_filter_add(scene_source, move);
	obs_source_update(move, movement);
	obs_data_release(movement);
	sleep_ms(100);
	vec2 start_position{};
	obs_sceneitem_set_pos(item, &start_position);
	start_filter(move);
	sleep_ms(300);
	vec2 actual{};
	obs_sceneitem_get_pos(item, &actual);
	check(std::fabs(actual.x - 104.0f) < 0.05f && std::fabs(actual.y - 71.0f) < 0.05f,
	      "Move Source reaches the configured scene coordinates");

	auto *edge = obs_source_create("stagekit_edge_detection", "Edges for Move", nullptr, nullptr);
	obs_source_filter_add(image_source, edge);
	auto *value_settings = obs_data_create();
	obs_data_set_string(value_settings, "filter", "Edges for Move");
	obs_data_set_string(value_settings, "setting_name", "threshold");
	obs_data_set_int(value_settings, "value_type", 2);
	obs_data_set_double(value_settings, "setting_float", 0.76);
	obs_data_set_int(value_settings, "duration", 50);
	auto *value_filter =
		obs_source_create("stagekit_port_move_value_filter", "Animate threshold", value_settings, nullptr);
	obs_source_filter_add(image_source, value_filter);
	obs_source_update(value_filter, value_settings);
	obs_data_release(value_settings);
	sleep_ms(100);
	start_filter(value_filter);
	sleep_ms(300);
	auto *updated = obs_source_get_settings(edge);
	check(std::fabs(obs_data_get_double(updated, "threshold") - 0.76) < 0.001,
	      "Move Value changes a named filter setting");
	obs_data_release(updated);

	auto *action_settings = obs_data_create();
	obs_data_set_int(action_settings, "action", 3);
	obs_data_set_string(action_settings, "scene", "Move test scene");
	obs_data_set_string(action_settings, "sceneitem", "Test art");
	obs_data_set_int(action_settings, "enable", 1);
	auto *action = obs_source_create("stagekit_port_move_action_filter", "Hide art", action_settings, nullptr);
	obs_source_filter_add(scene_source, action);
	obs_source_update(action, action_settings);
	obs_data_release(action_settings);
	sleep_ms(100);
	start_filter(action);
	sleep_ms(200);
	check(!obs_sceneitem_visible(item), "Move Action hides the named source inside its scene");
	obs_source_filter_remove(scene_source, action);
	obs_source_release(action);
	obs_source_filter_remove(image_source, value_filter);
	obs_source_release(value_filter);
	obs_source_filter_remove(image_source, edge);
	obs_source_release(edge);
	obs_source_filter_remove(scene_source, move);
	obs_source_release(move);
	obs_wait_for_destroy_queue();
	obs_sceneitem_remove(item);
	obs_source_dec_showing(scene_source);
	obs_source_remove(scene_source);
	obs_scene_release(scene);
	obs_wait_for_destroy_queue();

	obs_source_dec_showing(image_source);
	obs_source_release(image_source);
	obs_wait_for_destroy_queue();
	obs_shutdown();
	return failures ? 1 : 0;
}
