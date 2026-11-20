/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// A filter's hotkeys carry the filter's name in their ids. When the filter
// is renamed they are registered again under the new name and keep the
// user's key bindings, as Blur's and Move's always have (review C3).

#pragma once

#include <obs.h>

#include <functional>
#include <vector>

namespace stagekit {

class HotkeyRename {
public:
	// `ids` gives the filter's hotkey ids, always in the same order;
	// `again` unregisters them and registers them under the filter's
	// current name (nothing when the filter is on no source).
	HotkeyRename(obs_source_t *filter, std::function<std::vector<obs_hotkey_id *>()> ids,
		     std::function<void()> again)
		: filter_(filter),
		  ids_(std::move(ids)),
		  again_(std::move(again))
	{
		signal_handler_connect(obs_source_get_signal_handler(filter_), "rename", renamed, this);
	}
	~HotkeyRename() { signal_handler_disconnect(obs_source_get_signal_handler(filter_), "rename", renamed, this); }
	HotkeyRename(const HotkeyRename &) = delete;
	HotkeyRename &operator=(const HotkeyRename &) = delete;

private:
	static void renamed(void *data, calldata_t *)
	{
		auto *self = static_cast<HotkeyRename *>(data);
		std::vector<obs_data_array_t *> kept;
		bool any = false;
		for (obs_hotkey_id *id : self->ids_()) {
			const bool there = *id != OBS_INVALID_HOTKEY_ID;
			kept.push_back(there ? obs_hotkey_save(*id) : nullptr);
			any = any || there;
		}
		// Not registered yet: it will be, under the new name.
		if (any)
			self->again_();
		const std::vector<obs_hotkey_id *> now = self->ids_();
		for (size_t i = 0; i < kept.size(); i++) {
			if (!kept[i])
				continue;
			if (i < now.size() && *now[i] != OBS_INVALID_HOTKEY_ID)
				obs_hotkey_load(*now[i], kept[i]);
			obs_data_array_release(kept[i]);
		}
	}

	obs_source_t *filter_;
	std::function<std::vector<obs_hotkey_id *>()> ids_;
	std::function<void()> again_;
};

} // namespace stagekit
