#pragma once

#include <obs.h>

namespace stagekit {

// OBS applies defaults before create(). Only user values describe a supplied configuration.
inline bool has_saved_settings(obs_data_t *settings)
{
	for (obs_data_item_t *item = obs_data_first(settings); item; obs_data_item_next(&item)) {
		if (obs_data_item_has_user_value(item)) {
			obs_data_item_release(&item);
			return true;
		}
	}
	return false;
}

} // namespace stagekit
