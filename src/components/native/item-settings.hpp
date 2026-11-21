#pragma once

#include <obs.hpp>

namespace stagekit::native {
inline void copy_transform(obs_sceneitem_t *from, obs_sceneitem_t *to)
{
	obs_transform_info transform{};
	obs_sceneitem_crop crop{};
	obs_sceneitem_get_info2(from, &transform);
	obs_sceneitem_get_crop(from, &crop);
	obs_sceneitem_defer_update_begin(to);
	obs_sceneitem_set_info2(to, &transform);
	obs_sceneitem_set_crop(to, &crop);
	obs_sceneitem_defer_update_end(to);
}
inline void copy_item_transitions(obs_sceneitem_t *from, obs_sceneitem_t *to)
{
	for (bool show : {false, true}) {
		OBSDataAutoRelease saved = obs_sceneitem_transition_save(from, show);
		if (saved)
			obs_sceneitem_transition_load(to, saved, show);
		else
			obs_sceneitem_set_transition(to, show, nullptr);
		obs_sceneitem_set_transition_duration(to, show, obs_sceneitem_get_transition_duration(from, show));
	}
}
} // namespace stagekit::native
