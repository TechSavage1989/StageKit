#pragma once

#include "component.hpp"

namespace stagekit::native {
class EnableKeys {
public:
	explicit EnableKeys(obs_source_t *filter) : source(filter) {}
	~EnableKeys() { clear(); }
	void bind(obs_source_t *parent, const char *on, const char *off, const char *on_text, const char *off_text)
	{
		clear();
		id = obs_hotkey_pair_register_source(parent, on, text(on_text), off, text(off_text), press<true>, press<false>, source, source);
	}
	void clear()
	{
		if (id != OBS_INVALID_HOTKEY_PAIR_ID) {
			obs_hotkey_pair_unregister(id);
			id = OBS_INVALID_HOTKEY_PAIR_ID;
		}
	}
private:
	obs_source_t *source;
	obs_hotkey_pair_id id = OBS_INVALID_HOTKEY_PAIR_ID;
	template<bool State> static bool press(void *data, obs_hotkey_pair_id, obs_hotkey_t *, bool down)
	{
		auto *source = static_cast<obs_source_t *>(data);
		if (!down || obs_source_enabled(source) == State)
			return false;
		obs_source_set_enabled(source, State);
		return true;
	}
};
} // namespace stagekit::native
