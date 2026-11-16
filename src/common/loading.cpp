#include "loading.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <callback/proc.h>
#include <util/platform.h>

#include <atomic>

namespace stagekit {

namespace {
std::atomic<bool> g_loaded{true};
std::atomic<uint64_t> g_loaded_at{0};
std::atomic<bool> g_watching{false};
constexpr uint64_t kAfterLoadingNs = 2000000000ULL;

void on_event(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		g_loaded_at = os_gettime_ns() + kAfterLoadingNs;
		g_loaded = true;
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
	case OBS_FRONTEND_EVENT_EXIT:
		g_loaded = false;
		break;
	default:
		break;
	}
}
} // namespace

void loading_watch_start()
{
	// For the tests, which have no OBS window to say a collection is being
	// changed: "stagekit_test_loaded" (bool loaded) sets it by hand.
	static bool test_call = false;
	if (!test_call) {
		test_call = true;
		proc_handler_add(
			obs_get_proc_handler(), "void stagekit_test_loaded(bool loaded)",
			[](void *, calldata_t *cd) { g_loaded = calldata_bool(cd, "loaded"); }, nullptr);
	}
	if (!obs_frontend_get_main_window() || g_watching.exchange(true))
		return;
	g_loaded = false;
	obs_frontend_add_event_callback(on_event, nullptr);
}

void loading_watch_stop()
{
	if (!g_watching.exchange(false))
		return;
	g_loaded = false;
	obs_frontend_remove_event_callback(on_event, nullptr);
}

bool scenes_loaded()
{
	return g_loaded && os_gettime_ns() >= g_loaded_at;
}

} // namespace stagekit
