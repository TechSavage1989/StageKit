/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include "ambient/ambient-background.hpp"
#include "api/vendor-api.hpp"
#include "blur/ramp-blur.hpp"
#include "capture/auto-capture.hpp"
#include "capture/program-trigger.hpp"
#include "common/start-image.hpp"
#include "dock/stagekit-dock.hpp"
#include "frame/auto-frame.hpp"
#include "placement/place-item.hpp"
#include "placement/scene-tools.hpp"
#include "placement/visibility-link.hpp"
#include "dock/arrange-tools.hpp"
#include "shadow/shadow-backdrop.hpp"
#include "view/cropped-view.hpp"
#include "zoom/ramp-zoom.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

// Global "Blur All" hotkeys (Settings → Hotkeys, top section). Their key
// bindings are stored with the scene collection.
struct GlobalHotkey {
	const char *name;
	const char *text;
	stagekit::BlurAllMode mode;
	obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
};

GlobalHotkey g_hotkeys[] = {
	{"stagekit.blur_all.toggle", "Hotkey.BlurAllToggle", stagekit::BlurAllMode::Toggle},
	{"stagekit.blur_all.on", "Hotkey.BlurAllOn", stagekit::BlurAllMode::On},
	{"stagekit.blur_all.off", "Hotkey.BlurAllOff", stagekit::BlurAllMode::Off},
};

void blur_all_hotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		stagekit::ramp_blur_set_all(static_cast<GlobalHotkey *>(data)->mode);
}

void frontend_save_load(obs_data_t *save_data, bool saving, void *)
{
	if (saving)
		stagekit::start_image_save(save_data);
	else
		stagekit::start_image_load(save_data);
	stagekit::arrange_save_load(save_data, saving);

	for (GlobalHotkey &hk : g_hotkeys) {
		if (saving) {
			obs_data_array_t *bindings = obs_hotkey_save(hk.id);
			obs_data_set_array(save_data, hk.name, bindings);
			obs_data_array_release(bindings);
		} else {
			obs_data_array_t *bindings = obs_data_get_array(save_data, hk.name);
			obs_hotkey_load(hk.id, bindings);
			obs_data_array_release(bindings);
		}
	}
}

} // namespace

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("StageKit.Description");
}

bool obs_module_load(void)
{
	stagekit::register_ramp_blur();
	stagekit::register_start_image();
	stagekit::register_ambient_background();
	stagekit::register_shadow_backdrop();
	stagekit::register_place_item();
	stagekit::register_ramp_zoom();
	stagekit::register_cropped_view();
	stagekit::register_auto_capture();
	stagekit::register_program_trigger();
	stagekit::register_auto_frame();
	stagekit::register_show_hide();
	stagekit::register_visibility_link();
	stagekit::register_match_item();
	stagekit::register_layout();
	for (GlobalHotkey &hk : g_hotkeys)
		hk.id = obs_hotkey_register_frontend(hk.name, obs_module_text(hk.text), blur_all_hotkey, &hk);
	stagekit::register_arrange_hotkeys();
	obs_frontend_add_save_callback(frontend_save_load, nullptr);
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_post_load(void)
{
	// obs-websocket registers its API during its own load, so vendors
	// must be registered after every module has loaded.
	stagekit::vendor_api_register();
	stagekit::register_dock();
}

void obs_module_unload(void)
{
	obs_frontend_remove_save_callback(frontend_save_load, nullptr);
	for (GlobalHotkey &hk : g_hotkeys)
		obs_hotkey_unregister(hk.id);
	stagekit::vendor_api_unregister();
	obs_log(LOG_INFO, "plugin unloaded");
}
