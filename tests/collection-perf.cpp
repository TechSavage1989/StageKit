// Measures what a scene collection costs a frame, in headless libobs with StageKit (Linux, under Xvfb).
//
// collection-perf <stagekit.so> <data dir> <collection.json> [--out result.json]
//                 [--scene NAME]...      scenes to measure, one after another (default: every scene in scene_order);
//                                        "(off)" measures with an empty scene on the output: every scene off screen
//                 [--frames N]           frames per measurement (default 1800 = 30 s at 60 fps)
//                 [--warmup S]           seconds on the scene before measuring (default 4)
//                 [--activity none|toggle|scenes]
//                 [--toggle "A,B,C"]     with toggle: every 60 frames show the next of these items in the scene
//                                        and hide the others (items the scene does not have are passed over)
//                 [--cycle "S1,S2,..."]  with scenes: every 120 frames start the collection's transition to the
//                                        next of these scenes
//                 [--stand-ins]          sources of a kind not loaded here (captures, browsers, ...) become
//                                        colour sources of the size they are shown at, keeping their filters
//                 [--log FILE]           OBS's log, all of it, into FILE (else only warnings and errors)
//                 [--profiler]           OBS's own profiler tree at the end, as in an OBS log
//
// For each measurement: OBS's own profiler (the obs_graphics_thread tree the owner's log ends with), as the
// difference between two snapshots, and StageKit's per-kind costs from `stagekit_timing_take`. Differences
// between collections measured the same way are StageKit's and the scenes'; absolute times under llvmpipe and
// stand-ins are not the owner's. Not run by run-tests.sh: the owner's collections are private.
#include <obs.h>
#include <obs-nix-platform.h>
#include <util/platform.h>
#include <util/profiler.h>
#include <graphics/vec2.h>
#include <QApplication>

#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

FILE *g_log = nullptr;
std::mutex g_log_mutex;
int g_crossfades = 0;

void log_handler(int level, const char *format, va_list args, void *)
{
	// Headless there is no OBS window: every frontend call says so. In OBS
	// the call answers instead; formatting the line here would be a cost
	// OBS does not have.
	if (strncmp(format, "Tried to call ", 14) == 0)
		return;
	char text[4096];
	vsnprintf(text, sizeof(text), format, args);
	std::lock_guard lock(g_log_mutex);
	if (strstr(text, "cross-fade"))
		g_crossfades++;
	if (g_log)
		fprintf(g_log, "%s\n", text);
	else if (level <= LOG_WARNING)
		fprintf(stderr, "%s\n", text);
}

bool load_module(const char *bin, const char *data)
{
	obs_module_t *m = nullptr;
	if (obs_open_module(&m, bin, data) != MODULE_SUCCESS)
		return false;
	return obs_init_module(m);
}

bool registered_source(obs_source_t *source)
{
	// OBS preserves named placeholder objects for unknown source/filter IDs.
	// Existence alone does not prove a collection loaded an implementation.
	const char *id = source ? obs_source_get_id(source) : nullptr;
	return id && *id && obs_source_get_display_name(id) != nullptr;
}

std::vector<std::string> split(const std::string &list)
{
	std::vector<std::string> out;
	size_t at = 0;
	while (at <= list.size()) {
		const size_t comma = list.find(',', at);
		std::string part = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
		while (!part.empty() && part.front() == ' ')
			part.erase(part.begin());
		while (!part.empty() && part.back() == ' ')
			part.pop_back();
		if (!part.empty())
			out.push_back(part);
		if (comma == std::string::npos)
			break;
		at = comma + 1;
	}
	return out;
}

// --------------------------------------------------------------- stand-ins

// The size a source is shown at: a browser's own size, else the bounds of
// the first item showing it, else the canvas.
void shown_size(obs_data_t *source, obs_data_array_t *all, uint32_t cw, uint32_t ch, uint32_t &w, uint32_t &h)
{
	w = cw;
	h = ch;
	obs_data_t *settings = obs_data_get_obj(source, "settings");
	const long long bw = obs_data_get_int(settings, "width"), bh = obs_data_get_int(settings, "height");
	obs_data_release(settings);
	if (strcmp(obs_data_get_string(source, "id"), "browser_source") == 0 && bw > 0 && bh > 0) {
		w = (uint32_t)bw;
		h = (uint32_t)bh;
		return;
	}
	const char *name = obs_data_get_string(source, "name");
	for (size_t i = 0; i < obs_data_array_count(all); i++) {
		obs_data_t *other = obs_data_array_item(all, i);
		const std::string id = obs_data_get_string(other, "id");
		bool found = false;
		if (id == "scene" || id == "group") {
			obs_data_t *os = obs_data_get_obj(other, "settings");
			obs_data_array_t *items = obs_data_get_array(os, "items");
			for (size_t j = 0; j < obs_data_array_count(items) && !found; j++) {
				obs_data_t *item = obs_data_array_item(items, j);
				if (strcmp(obs_data_get_string(item, "name"), name) == 0 &&
				    obs_data_get_int(item, "bounds_type") != 0) {
					vec2 bounds;
					obs_data_get_vec2(item, "bounds", &bounds);
					if (bounds.x >= 2.0f && bounds.y >= 2.0f) {
						w = (uint32_t)std::lround(bounds.x);
						h = (uint32_t)std::lround(bounds.y);
						found = true;
					}
				}
				obs_data_release(item);
			}
			obs_data_array_release(items);
			obs_data_release(os);
		}
		obs_data_release(other);
		if (found)
			return;
	}
}

int make_stand_ins(obs_data_array_t *all, uint32_t cw, uint32_t ch)
{
	int replaced = 0;
	for (size_t i = 0; i < obs_data_array_count(all); i++) {
		obs_data_t *source = obs_data_array_item(all, i);
		const std::string id = obs_data_get_string(source, "id");
		if (!id.empty() && !obs_source_get_display_name(id.c_str())) {
			uint32_t w = 0, h = 0;
			shown_size(source, all, cw, ch, w, h);
			// A colour of its own for each, so what shows can be measured.
			uint32_t hash = 2166136261u;
			for (const char *c = obs_data_get_string(source, "name"); *c; c++)
				hash = (hash ^ (unsigned char)*c) * 16777619u;
			obs_data_t *settings = obs_data_create();
			obs_data_set_int(settings, "color", 0xFF000000LL | (hash & 0x7F7F7F) | 0x404040);
			obs_data_set_int(settings, "width", w);
			obs_data_set_int(settings, "height", h);
			obs_data_set_obj(source, "settings", settings);
			obs_data_release(settings);
			obs_data_set_string(source, "id", "color_source");
			obs_data_set_string(source, "versioned_id", "color_source_v3");
			replaced++;
		}
		obs_data_release(source);
	}
	return replaced;
}

// --------------------------------------------------------------- profiler

struct Found {
	const char *name;
	bool prefix;
	profiler_snapshot_entry_t *entry;
};

bool find_entry(void *param, profiler_snapshot_entry_t *entry)
{
	auto *f = static_cast<Found *>(param);
	if (f->entry)
		return false;
	const char *name = profiler_snapshot_entry_name(entry);
	if (name && (f->prefix ? strncmp(name, f->name, strlen(f->name)) == 0 : strcmp(name, f->name) == 0)) {
		f->entry = entry;
		return false;
	}
	profiler_snapshot_enumerate_children(entry, find_entry, param);
	return !f->entry;
}

// Each call's time (µs) and how many calls took it.
std::map<uint64_t, uint64_t> times_of(profiler_snapshot_t *snap, const char *name, bool prefix)
{
	std::map<uint64_t, uint64_t> times;
	Found f{name, prefix, nullptr};
	profiler_snapshot_enumerate_roots(snap, find_entry, &f);
	if (!f.entry)
		return times;
	profiler_time_entries_t *entries = profiler_snapshot_entry_times(f.entry);
	for (size_t i = 0; entries && i < entries->num; i++)
		times[entries->array[i].time_delta] += entries->array[i].count;
	return times;
}

struct Figures {
	uint64_t calls = 0;
	double median = 0, p95 = 0, p99 = 0, mean = 0, over_1ms = 0; // ms; share in %
};

Figures difference(profiler_snapshot_t *a, profiler_snapshot_t *b, const char *name, bool prefix = false)
{
	std::map<uint64_t, uint64_t> before = times_of(a, name, prefix), after = times_of(b, name, prefix);
	std::vector<std::pair<uint64_t, uint64_t>> diff;
	Figures f;
	double sum = 0;
	uint64_t over = 0;
	for (const auto &[us, count] : after) {
		const uint64_t was = before.count(us) ? before[us] : 0;
		if (count > was) {
			diff.push_back({us, count - was});
			f.calls += count - was;
			sum += double(us) * double(count - was);
			if (us > 1000)
				over += count - was;
		}
	}
	if (!f.calls)
		return f;
	auto at = [&](double share) {
		const uint64_t want = (uint64_t)std::ceil(share * f.calls);
		uint64_t seen = 0;
		for (const auto &[us, count] : diff) {
			seen += count;
			if (seen >= want)
				return us / 1000.0;
		}
		return diff.back().first / 1000.0;
	};
	f.median = at(0.5);
	f.p95 = at(0.95);
	f.p99 = at(0.99);
	f.mean = sum / f.calls / 1000.0;
	f.over_1ms = 100.0 * over / f.calls;
	return f;
}

// --------------------------------------------------------------- StageKit's timing

obs_data_t *take_timing()
{
	calldata_t cd;
	calldata_init(&cd);
	obs_data_t *data = nullptr;
	if (proc_handler_call(obs_get_proc_handler(), "stagekit_timing_take", &cd)) {
		const char *json = calldata_string(&cd, "json");
		data = json ? obs_data_create_from_json(json) : nullptr;
	}
	calldata_free(&cd);
	return data ? data : obs_data_create();
}

// What changed in the scenes (signals) and how often StageKit's periodic
// looks ran, so far (`stagekit_test_looks`).
obs_data_t *take_looks()
{
	calldata_t cd;
	calldata_init(&cd);
	obs_data_t *data = nullptr;
	if (proc_handler_call(obs_get_proc_handler(), "stagekit_test_looks", &cd)) {
		const char *json = calldata_string(&cd, "json");
		data = json ? obs_data_create_from_json(json) : nullptr;
	}
	calldata_free(&cd);
	const bool available = data != nullptr;
	if (!data)
		data = obs_data_create();
	obs_data_set_bool(data, "available", available);
	calldata_init(&cd);
	long long known = 0, draws = 0;
	const bool ambient = proc_handler_call(obs_get_proc_handler(), "stagekit_test_ambient_frames", &cd) &&
			     calldata_get_int(&cd, "known", &known) && calldata_get_int(&cd, "draws", &draws);
	obs_data_set_bool(data, "ambient_available", ambient);
	obs_data_set_int(data, "ambient_known", known);
	obs_data_set_int(data, "ambient_draws", draws);
	calldata_free(&cd);
	// These optional per-filter counters were added after the frozen baseline.
	// Missing support is recorded, never interpreted as zero work by the baseline.
	std::vector<obs_source_t *> shadows;
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			obs_source_enum_filters(
				source,
				[](obs_source_t *, obs_source_t *filter, void *param) {
					if (strcmp(obs_source_get_id(filter), "stagekit_scene_shadow") != 0)
						return;
					static_cast<std::vector<obs_source_t *> *>(param)->push_back(
						obs_source_get_ref(filter));
				},
				param);
			return true;
		},
		&shadows);
	// The procedure enters graphics. Do not call it while OBS's source/filter
	// enumeration locks are held; the render thread can need those locks too.
	obs_data_set_int(data, "shadow_filters", (long long)shadows.size());
	for (obs_source_t *filter : shadows) {
		calldata_t call;
		calldata_init(&call);
		if (proc_handler_call(obs_source_get_proc_handler(filter), "shadow_reuse_test", &call)) {
			obs_data_set_int(data, "shadow_probe_filters",
					 obs_data_get_int(data, "shadow_probe_filters") + 1);
			for (const char *key : {"preparations", "reuses", "passes"}) {
				const std::string name = std::string("shadow_") + key;
				obs_data_set_int(data, name.c_str(),
						 obs_data_get_int(data, name.c_str()) + calldata_int(&call, key));
			}
		}
		calldata_free(&call);
		obs_source_release(filter);
	}
	return data;
}

// --------------------------------------------------------------- scenes

void show_only(obs_source_t *scene_source, const std::vector<std::string> &names, size_t shown)
{
	obs_scene_t *scene = obs_scene_from_source(scene_source);
	if (!scene)
		return;
	for (size_t i = 0; i < names.size(); i++) {
		obs_sceneitem_t *item = obs_scene_find_source(scene, names[i].c_str());
		if (item && obs_sceneitem_visible(item) != (i == shown))
			obs_sceneitem_set_visible(item, i == shown);
	}
}

// Frames the graphics thread went round for: one it was too late for is
// counted in OBS's total, but nothing was ticked or drawn for it.
uint64_t ticked_frames()
{
	const uint64_t total = obs_get_total_frames(), lagged = obs_get_lagged_frames();
	return total > lagged ? total - lagged : 0;
}

bool wait_frames(uint64_t frames, const std::function<void(uint64_t)> &each)
{
	const uint64_t from = ticked_frames();
	const uint64_t give_up = os_gettime_ns() + (uint64_t)(frames * 0.5 + 60.0) * 1000000000ULL;
	uint64_t last = from;
	while (ticked_frames() < from + frames) {
		const uint64_t now = ticked_frames();
		if (now != last && each)
			each(now - from);
		last = now;
		if (os_gettime_ns() > give_up)
			return false;
		os_sleep_ms(2);
	}
	return true;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: %s <stagekit.so> <data dir> <collection.json> [options]\n", argv[0]);
		return 2;
	}
	std::string out_path, activity = "none", log_path;
	std::vector<std::string> scenes, toggle, cycle;
	uint64_t frames = 1800;
	double warmup = 4.0;
	bool stand_ins = false, print_profiler = false;
	for (int i = 4; i < argc; i++) {
		const std::string a = argv[i];
		const char *next = i + 1 < argc ? argv[i + 1] : nullptr;
		if (a == "--stand-ins")
			stand_ins = true;
		else if (a == "--profiler")
			print_profiler = true;
		else if (!next) {
			fprintf(stderr, "%s needs a value\n", a.c_str());
			return 2;
		} else if (a == "--out")
			out_path = argv[++i];
		else if (a == "--scene")
			scenes.push_back(argv[++i]);
		else if (a == "--frames")
			frames = std::max(60ULL, strtoull(argv[++i], nullptr, 10));
		else if (a == "--warmup")
			warmup = std::max(0.0, atof(argv[++i]));
		else if (a == "--activity")
			activity = argv[++i];
		else if (a == "--toggle")
			toggle = split(argv[++i]);
		else if (a == "--cycle")
			cycle = split(argv[++i]);
		else if (a == "--log")
			log_path = argv[++i];
		else {
			fprintf(stderr, "unknown option %s\n", a.c_str());
			return 2;
		}
	}
	if (activity != "none" && activity != "toggle" && activity != "scenes") {
		fprintf(stderr, "--activity is none, toggle or scenes\n");
		return 2;
	}
	// Match the native test/frontend environment before modules or any Qt fonts load.
	QApplication app(argc, argv);
	if (!log_path.empty()) {
		g_log = fopen(log_path.c_str(), "w");
		if (!g_log) {
			fprintf(stderr, "Could not open log %s\n", log_path.c_str());
			return 1;
		}
	}
	base_set_log_handler(log_handler, nullptr);

	obs_data_t *file = obs_data_create_from_json_file(argv[3]);
	if (!file) {
		printf("FAIL: not valid JSON\n");
		return 1;
	}
	vec2 res;
	obs_data_get_vec2(file, "resolution", &res);
	const uint32_t cw = res.x >= 16.0f ? (uint32_t)res.x : 1920, ch = res.y >= 16.0f ? (uint32_t)res.y : 1080;

	void *x11 = dlopen("libX11.so.6", RTLD_NOW);
	auto xopen = x11 ? (void *(*)(const char *))dlsym(x11, "XOpenDisplay") : nullptr;
	void *display = xopen ? xopen(nullptr) : nullptr;
	if (!display) {
		fprintf(stderr, "No X display (run under xvfb-run)\n");
		return 2;
	}
	obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);
	obs_set_nix_platform_display(display);
	profiler_start();
	profiler_name_store_t *names = profiler_name_store_create();
	if (!obs_startup("en-US", nullptr, names))
		return 1;
	obs_video_info ovi = {};
	ovi.graphics_module = "libobs-opengl";
	ovi.fps_num = 60;
	ovi.fps_den = 1;
	ovi.base_width = ovi.output_width = cw;
	ovi.base_height = ovi.output_height = ch;
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.gpu_conversion = true;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	if (obs_reset_video(&ovi) != OBS_VIDEO_SUCCESS)
		return 1;
	// Sound, as in OBS: an item's show and hide transition only ends with it.
	obs_audio_info sound = {};
	sound.samples_per_sec = 48000;
	sound.speakers = SPEAKERS_STEREO;
	if (!obs_reset_audio(&sound)) {
		fprintf(stderr, "FAIL: OBS audio could not initialize\n");
		return 1;
	}
	for (const char *name : {"image-source", "obs-transitions", "obs-filters", "text-freetype2"}) {
		const std::string bin = std::string("/usr/lib/x86_64-linux-gnu/obs-plugins/") + name + ".so";
		const std::string data = std::string("/usr/share/obs/obs-plugins/") + name;
		if (!load_module(bin.c_str(), data.c_str())) {
			fprintf(stderr, "FAIL: required OBS module %s did not load\n", name);
			return 1;
		}
	}
	if (!load_module(argv[1], argv[2])) {
		printf("FAIL: StageKit did not load\n");
		return 1;
	}
	obs_post_load_modules();

	obs_data_array_t *sources = obs_data_get_array(file, "sources");
	obs_data_array_t *groups = obs_data_get_array(file, "groups");
	for (size_t i = 0; i < obs_data_array_count(groups); i++) {
		obs_data_t *g = obs_data_array_item(groups, i);
		obs_data_array_push_back(sources, g);
		obs_data_release(g);
	}
	const int replaced = stand_ins ? make_stand_ins(sources, cw, ch) : 0;
	std::vector<obs_source_t *> held;
	obs_load_sources(
		sources,
		[](void *param, obs_source_t *source) {
			static_cast<std::vector<obs_source_t *> *>(param)->push_back(obs_source_get_ref(source));
		},
		&held);
	printf("collection: %s\n", obs_data_get_string(file, "name"));
	printf("canvas %ux%u; loaded %zu of %zu sources; %d stand-ins\n", cw, ch, held.size(),
	       obs_data_array_count(sources), replaced);

	// The transition on the output, as in OBS: the collection's own.
	const std::string current_tr = obs_data_get_string(file, "current_transition");
	const int duration = std::max(50, (int)obs_data_get_int(file, "transition_duration"));
	obs_source_t *transition = nullptr;
	obs_data_array_t *transitions = obs_data_get_array(file, "transitions");
	for (size_t i = 0; i < obs_data_array_count(transitions) && !transition; i++) {
		obs_data_t *t = obs_data_array_item(transitions, i);
		if (current_tr == obs_data_get_string(t, "name")) {
			obs_data_t *ts = obs_data_get_obj(t, "settings");
			transition = obs_source_create_private(obs_data_get_string(t, "id"), current_tr.c_str(), ts);
			obs_data_release(ts);
		}
		obs_data_release(t);
	}
	obs_data_array_release(transitions);
	if (!transition)
		transition = obs_source_create_private(current_tr == "Cut" ? "cut_transition" : "fade_transition",
						       current_tr.c_str(), nullptr);
	if (!registered_source(transition)) {
		fprintf(stderr, "FAIL: output transition has no registered implementation\n");
		return 1;
	}
	printf("transition: %s (%s), %d ms\n", current_tr.c_str(), transition ? obs_source_get_id(transition) : "none",
	       duration);
	obs_set_output_source(0, transition);

	if (scenes.empty()) {
		obs_data_array_t *order = obs_data_get_array(file, "scene_order");
		for (size_t i = 0; i < obs_data_array_count(order); i++) {
			obs_data_t *entry = obs_data_array_item(order, i);
			scenes.push_back(obs_data_get_string(entry, "name"));
			obs_data_release(entry);
		}
		obs_data_array_release(order);
	}

	obs_data_t *result = obs_data_create();
	obs_data_set_int(result, "schema_version", 2);
	obs_data_set_string(result, "collection", obs_data_get_string(file, "name"));
	obs_data_set_int(result, "stand_ins", replaced);
	obs_data_set_string(result, "activity", activity.c_str());
	obs_data_set_int(result, "requested_frames", (long long)frames);
	obs_data_set_double(result, "warmup_seconds", warmup);
	obs_data_set_int(result, "canvas_width", cw);
	obs_data_set_int(result, "canvas_height", ch);
	obs_data_set_int(result, "target_fps", 60);
	obs_data_set_int(result, "loaded_sources", (long long)held.size());
	obs_data_set_int(result, "expected_sources", (long long)obs_data_array_count(sources));
	obs_data_set_string(result, "obs_version", obs_get_version_string());
	obs_data_set_string(
		result, "render_timing_note",
		"Per-kind render callbacks are inclusive and overlap. Use OBS render_video for total rendering.");
	obs_data_set_string(
		result, "operation_timing_note",
		"Operations are sampled just outside timing snapshots; an in-flight frame can cross the boundary.");
	obs_data_array_t *measured = obs_data_array_create();
	profiler_snapshot_t *last = nullptr;
	int failures = 0;
	if (held.size() != obs_data_array_count(sources)) {
		printf("FAIL: collection sources did not all load\n");
		failures++;
	}
	for (size_t i = 0; i < obs_data_array_count(sources); i++) {
		obs_data_t *saved = obs_data_array_item(sources, i);
		const char *name = obs_data_get_string(saved, "name");
		obs_source_t *loaded = obs_get_source_by_name(name);
		if (!registered_source(loaded)) {
			printf("FAIL: collection source '%s' has no registered implementation\n", name);
			failures++;
		}
		obs_data_array_t *filters = obs_data_get_array(saved, "filters");
		for (size_t j = 0; j < obs_data_array_count(filters); j++) {
			obs_data_t *saved_filter = obs_data_array_item(filters, j);
			const char *filter_name = obs_data_get_string(saved_filter, "name");
			obs_source_t *filter = loaded ? obs_source_get_filter_by_name(loaded, filter_name) : nullptr;
			if (!registered_source(filter)) {
				printf("FAIL: collection source '%s' filter '%s' has no registered implementation\n",
				       name, filter_name);
				failures++;
			}
			obs_source_release(filter);
			obs_data_release(saved_filter);
		}
		obs_data_array_release(filters);
		obs_source_release(loaded);
		obs_data_release(saved);
	}
	if (scenes.empty()) {
		printf("FAIL: no scenes requested or listed\n");
		failures++;
	}
	obs_scene_t *empty = obs_scene_create_private("(off)");
	for (const std::string &name : scenes) {
		obs_source_t *scene = name == "(off)" ? obs_source_get_ref(obs_scene_get_source(empty))
						      : obs_get_source_by_name(name.c_str());
		if (!scene || !obs_scene_from_source(scene)) {
			printf("FAIL: no scene '%s'\n", name.c_str());
			failures++;
			obs_source_release(scene);
			continue;
		}
		obs_transition_set(transition, scene);
		os_sleep_ms((uint32_t)(warmup * 1000.0));
		obs_data_t *looks_a = take_looks();
		obs_data_release(take_timing());
		profiler_snapshot_t *a = profile_snapshot_create();
		std::vector<obs_source_t *> ring;
		for (const std::string &c : cycle)
			if (obs_source_t *s = obs_get_source_by_name(c.c_str()))
				ring.push_back(s);
		size_t shown = 0, next_scene = 0;
		uint64_t done = 0;
		const int crossfades_before = g_crossfades;
		const uint64_t total_before = obs_get_total_frames(), lagged_before = obs_get_lagged_frames();
		const uint64_t started = os_gettime_ns();
		const bool whole = wait_frames(frames, [&](uint64_t at) {
			for (; done + 60 <= at; done += 60) {
				if (activity == "toggle" && !toggle.empty()) {
					shown = (shown + 1) % toggle.size();
					show_only(scene, toggle, shown);
				}
				if (activity == "scenes" && !ring.empty() && (done / 60) % 2 == 1) {
					next_scene = (next_scene + 1) % ring.size();
					obs_transition_start(transition, OBS_TRANSITION_MODE_AUTO, (uint32_t)duration,
							     ring[next_scene]);
				}
			}
		});
		const double elapsed = (os_gettime_ns() - started) / 1000000000.0;
		const uint64_t total_frames = obs_get_total_frames() - total_before;
		const uint64_t lagged_frames = obs_get_lagged_frames() - lagged_before;
		const uint64_t actual_frames = total_frames > lagged_frames ? total_frames - lagged_frames : 0;
		profiler_snapshot_t *b = profile_snapshot_create();
		obs_data_t *took = take_timing();
		obs_data_t *looks_b = take_looks();
		if (!whole) {
			printf("FAIL: '%s' did not reach %llu frames in time\n", name.c_str(),
			       (unsigned long long)frames);
			failures++;
		}
		const Figures g = difference(a, b, "obs_graphics_thread", true);
		const Figures t = difference(a, b, "tick_sources");
		const Figures o = difference(a, b, "output_frame");
		const Figures r = difference(a, b, "render_video");
		const Figures d = difference(a, b, "render_displays");
		const bool profiles_complete = g.calls + 2 >= frames && t.calls + 2 >= frames && r.calls + 2 >= frames;
		if (!profiles_complete) {
			printf("FAIL: '%s' has incomplete OBS profiler samples (graphics %llu, tick %llu, render %llu)\n",
			       name.c_str(), (unsigned long long)g.calls, (unsigned long long)t.calls,
			       (unsigned long long)r.calls);
			failures++;
		}
		printf("%-26s %-6s %5llu fr  graphics %.3f/%.3f  tick_sources %.3f/%.3f  output_frame %.3f/%.3f  "
		       "render_video %.3f/%.3f  (p50/p95 ms; %.1f%% of frames over 1 ms)\n",
		       name.c_str(), activity.c_str(), (unsigned long long)g.calls, g.median, g.p95, t.median, t.p95,
		       o.median, o.p95, r.median, r.p95, g.over_1ms);
		printf("   requested %llu, ticked %llu, lagged %llu, elapsed %.3f s, %.2f ticked frames/s\n",
		       (unsigned long long)frames, (unsigned long long)actual_frames, (unsigned long long)lagged_frames,
		       elapsed, elapsed > 0.0 ? actual_frames / elapsed : 0.0);
		// StageKit's own, by kind.
		const double took_frames = std::max(1.0, (double)obs_data_get_int(took, "frames"));
		struct Row {
			std::string id;
			double calls, tick_us, render_us, p50, p95, max;
		};
		std::vector<Row> rows;
		double tick_total = 0;
		obs_data_array_t *kinds = obs_data_get_array(took, "kinds");
		for (size_t i = 0; i < obs_data_array_count(kinds); i++) {
			obs_data_t *k = obs_data_array_item(kinds, i);
			Row row{obs_data_get_string(k, "id"),          (double)obs_data_get_int(k, "tick_calls"),
				obs_data_get_double(k, "tick_us"),     obs_data_get_double(k, "render_us"),
				obs_data_get_double(k, "tick_p50_us"), obs_data_get_double(k, "tick_p95_us"),
				obs_data_get_double(k, "tick_max_us")};
			if (row.calls > 0 || row.render_us > 0)
				rows.push_back(row);
			tick_total += row.tick_us;
			obs_data_release(k);
		}
		obs_data_array_release(kinds);
		std::sort(rows.begin(), rows.end(), [](const Row &x, const Row &y) { return x.tick_us > y.tick_us; });
		// (Drawing is not summed here: a filter's drawing includes what it
		// draws from, so the kinds' drawing times overlap.)
		printf("   StageKit ticks %.1f us a frame (p50 %.1f, p95 %.1f, max %.1f us)\n",
		       tick_total / took_frames, obs_data_get_double(took, "frame_tick_p50_us"),
		       obs_data_get_double(took, "frame_tick_p95_us"), obs_data_get_double(took, "frame_tick_max_us"));
		printf("   %-24s %5s %9s %8s %8s %8s %9s %10s\n", "kind", "inst", "us/frame", "mean", "p50", "p95",
		       "max", "render/fr");
		for (const Row &row : rows) {
			const std::string short_id = row.id.rfind("stagekit_", 0) == 0 ? row.id.substr(9) : row.id;
			printf("   %-24s %5.1f %9.2f %8.2f %8.2f %8.2f %9.1f %10.1f\n", short_id.c_str(),
			       row.calls / took_frames, row.tick_us / took_frames,
			       row.calls ? row.tick_us / row.calls : 0.0, row.p50, row.p95, row.max,
			       row.render_us / took_frames);
		}
		{
			// Per second: the scene signals and StageKit's periodic looks.
			const double secs = std::max(0.001, obs_data_get_double(took, "seconds"));
			obs_data_t *per_s = obs_data_create();
			obs_data_t *per_frame = obs_data_create();
			obs_data_t *counts = obs_data_create();
			printf("   a second:");
			for (const char *key :
			     {"item_add", "item_remove", "item_visible", "item_transform", "reorder", "refresh",
			      "switch_follow", "link_follow", "shadow_scale", "ambient_sources", "fit_obstacle",
			      "fit_own", "ambient_draws", "shadow_preparations", "shadow_reuses", "shadow_passes"}) {
				const long long count = obs_data_get_int(looks_b, key) - obs_data_get_int(looks_a, key);
				const double n = count / secs;
				obs_data_set_double(per_s, key, n);
				obs_data_set_double(per_frame, key, count / took_frames);
				obs_data_set_int(counts, key, count);
				printf(" %s %.1f", key, n);
			}
			printf("\n");
			obs_data_set_obj(took, "per_second", per_s);
			obs_data_set_obj(took, "operations_per_frame", per_frame);
			obs_data_set_obj(took, "operation_counts", counts);
			obs_data_set_bool(took, "operations_available",
					  obs_data_get_bool(looks_a, "available") &&
						  obs_data_get_bool(looks_b, "available"));
			obs_data_set_bool(took, "ambient_frames_available",
					  obs_data_get_bool(looks_a, "ambient_available") &&
						  obs_data_get_bool(looks_b, "ambient_available"));
			obs_data_set_int(took, "ambient_known", obs_data_get_int(looks_b, "ambient_known"));
			const long long shadow_filters = obs_data_get_int(looks_b, "shadow_filters");
			obs_data_set_int(took, "shadow_filters", shadow_filters);
			obs_data_set_bool(took, "shadow_operations_available",
					  shadow_filters > 0 &&
						  obs_data_get_int(looks_a, "shadow_filters") == shadow_filters &&
						  obs_data_get_int(looks_a, "shadow_probe_filters") == shadow_filters &&
						  obs_data_get_int(looks_b, "shadow_probe_filters") == shadow_filters);
			obs_data_release(per_s);
			obs_data_release(per_frame);
			obs_data_release(counts);
			obs_data_release(looks_a);
			obs_data_release(looks_b);
		}
		if (activity == "scenes")
			printf("   Morph cross-fades logged: %d\n", g_crossfades - crossfades_before);
		fflush(stdout);

		obs_data_t *m = obs_data_create();
		obs_data_set_string(m, "scene", name.c_str());
		obs_data_set_int(m, "frames", (long long)g.calls);
		obs_data_set_bool(m, "complete", whole && profiles_complete);
		obs_data_set_int(m, "requested_frames", (long long)frames);
		obs_data_set_int(m, "ticked_frames", (long long)actual_frames);
		obs_data_set_int(m, "total_frames", (long long)total_frames);
		obs_data_set_int(m, "lagged_frames", (long long)lagged_frames);
		obs_data_set_double(m, "elapsed_seconds", elapsed);
		obs_data_set_double(m, "effective_fps", elapsed > 0.0 ? actual_frames / elapsed : 0.0);
		auto put = [&](const char *key, const Figures &f) {
			obs_data_t *x = obs_data_create();
			obs_data_set_double(x, "median_ms", f.median);
			obs_data_set_double(x, "p95_ms", f.p95);
			obs_data_set_double(x, "p99_ms", f.p99);
			obs_data_set_double(x, "mean_ms", f.mean);
			obs_data_set_double(x, "over_1ms_pct", f.over_1ms);
			obs_data_set_int(x, "calls", (long long)f.calls);
			obs_data_set_obj(m, key, x);
			obs_data_release(x);
		};
		put("obs_graphics_thread", g);
		put("tick_sources", t);
		put("output_frame", o);
		put("render_video", r);
		put("render_displays", d);
		obs_data_set_double(m, "stagekit_tick_us_per_frame", tick_total / took_frames);
		obs_data_set_int(m, "crossfades", g_crossfades - crossfades_before);
		obs_data_set_obj(m, "stagekit", took);
		obs_data_array_push_back(measured, m);
		obs_data_release(m);
		obs_data_release(took);
		profile_snapshot_free(a);
		if (last)
			profile_snapshot_free(last);
		last = b;
		for (obs_source_t *s : ring)
			obs_source_release(s);
		obs_source_release(scene);
	}
	obs_data_set_array(result, "measurements", measured);
	obs_data_set_int(result, "failures", failures);
	obs_data_set_bool(result, "complete", failures == 0);
	obs_data_array_release(measured);
	if (!out_path.empty() && !obs_data_save_json_safe(result, out_path.c_str(), "tmp", "bak")) {
		printf("FAIL: could not write %s\n", out_path.c_str());
		failures++;
	}
	obs_data_release(result);
	if (print_profiler && last) {
		FILE *was = g_log;
		g_log = stdout;
		profiler_print(last);
		g_log = was;
	}
	if (last)
		profile_snapshot_free(last);
	printf(failures ? "FAILED\n" : "MEASURED\n");
	fflush(stdout);
	if (g_log)
		fflush(g_log);
	_exit(failures ? 1 : 0); // skip library teardown: the video threads are still running
}
