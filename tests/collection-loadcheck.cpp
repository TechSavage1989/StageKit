// Loads a scene collection in headless libobs with StageKit and reports what it finds.
#include <obs.h>
#include <obs-nix-platform.h>
#include <graphics/matrix4.h>
#include <util/platform.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static bool load_module(const char *bin, const char *data)
{
	obs_module_t *m = nullptr;
	if (obs_open_module(&m, bin, data) != MODULE_SUCCESS)
		return false;
	return obs_init_module(m);
}

int main(int argc, char **argv)
{
	void *x11 = dlopen("libX11.so.6", RTLD_NOW);
	auto xopen = (void *(*)(const char *))dlsym(x11, "XOpenDisplay");
	void *display = xopen(nullptr);
	obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);
	obs_set_nix_platform_display(display);
	if (!obs_startup("en-US", nullptr, nullptr))
		return 1;
	obs_video_info ovi = {};
	ovi.graphics_module = "libobs-opengl";
	ovi.fps_num = 60;
	ovi.fps_den = 1;
	ovi.base_width = ovi.output_width = 2560;
	ovi.base_height = ovi.output_height = 1440;
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.gpu_conversion = true;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	if (obs_reset_video(&ovi) != OBS_VIDEO_SUCCESS)
		return 1;
	// Every stock plugin this machine has, so scenes are exercised as fully as they can be here.
	for (const char *name : {"image-source", "obs-transitions", "obs-filters", "text-freetype2", "obs-ffmpeg"}) {
		const std::string bin = std::string("/usr/lib/x86_64-linux-gnu/obs-plugins/") + name + ".so";
		const std::string data = std::string("/usr/share/obs/obs-plugins/") + name;
		load_module(bin.c_str(), data.c_str());
	}
	if (!load_module(argv[1], argv[2]))
		return 1;
	obs_post_load_modules();

	obs_data_t *file = obs_data_create_from_json_file(argv[3]);
	if (!file) {
		printf("FAIL: not valid JSON\n");
		return 1;
	}
	obs_data_array_t *sources = obs_data_get_array(file, "sources");
	obs_data_array_t *groups = obs_data_get_array(file, "groups");
	for (size_t i = 0; i < obs_data_array_count(groups); i++) {
		obs_data_t *g = obs_data_array_item(groups, i);
		obs_data_array_push_back(sources, g);
		obs_data_release(g);
	}
	int loaded = 0;
	obs_load_sources(
		sources,
		[](void *param, obs_source_t *source) {
			(*static_cast<int *>(param))++;
			obs_source_get_ref(source); // the OBS window would hold these; here we do
		},
		&loaded);
	printf("loaded %d of %zu sources\n", loaded, obs_data_array_count(sources));

	obs_data_array_t *order = obs_data_get_array(file, "scene_order");
	int failures = 0;
	for (int pass = 0; pass < 2; pass++) {
		if (pass == 1)
			os_sleep_ms(1500); // let StageKit's filters settle the items
		for (size_t i = 0; i < obs_data_array_count(order); i++) {
			obs_data_t *entry = obs_data_array_item(order, i);
			const char *name = obs_data_get_string(entry, "name");
			obs_source_t *src = obs_get_source_by_name(name);
			obs_scene_t *scene = obs_scene_from_source(src);
			if (!scene) {
				if (pass == 0) {
					printf("FAIL: scene '%s' did not load\n", name);
					failures++;
				}
				obs_data_release(entry);
				continue;
			}
			if (pass == 1 && argc > 4 && strcmp(name, argv[4]) == 0) {
				printf("== %s after settling\n", name);
				obs_scene_enum_items(
					scene,
					[](obs_scene_t *, obs_sceneitem_t *item, void *) {
						vec2 pos, scale, bounds;
						obs_sceneitem_get_pos(item, &pos);
						obs_sceneitem_get_scale(item, &scale);
						obs_sceneitem_get_bounds(item, &bounds);
						obs_source_t *s = obs_sceneitem_get_source(item);
						printf("   %-34s %-28s @%8.1f,%8.1f x%.3f box %.0fx%.0f size %ux%u\n",
						       obs_source_get_name(s), obs_source_get_id(s), pos.x, pos.y,
						       scale.x, bounds.x, bounds.y, obs_source_get_width(s),
						       obs_source_get_height(s));
						return true;
					},
					nullptr);
			}
			if (pass == 0) {
				printf("showing %s\n", name);
				fflush(stdout);
				obs_set_output_source(0,
						      src); // on screen for a moment: renders through the video thread
				os_sleep_ms(250);
			}
			obs_source_release(src);
			obs_data_release(entry);
		}
	}
	obs_set_output_source(0, nullptr);
	// Every StageKit filter must have been created as its real type.
	struct Count {
		int stagekit = 0, missing = 0;
	} count;
	obs_enum_all_sources(
		[](void *param, obs_source_t *s) {
			auto *c = static_cast<Count *>(param);
			const char *id = obs_source_get_unversioned_id(s);
			if (id && strncmp(id, "stagekit_", 9) == 0) {
				c->stagekit++;
				if (!obs_source_configurable(s) && !obs_source_get_display_name(id))
					c->missing++;
			}
			return true;
		},
		&count);
	printf("StageKit sources and filters created: %d (unknown type: %d)\n", count.stagekit, count.missing);
	printf(failures ? "FAILED\n" : "LOAD CHECK PASSED\n");
	obs_data_array_release(order);
	obs_data_array_release(groups);
	obs_data_array_release(sources);
	obs_data_release(file);
	fflush(stdout);
	_exit(failures); // skip library teardown: the video threads are still running
}
