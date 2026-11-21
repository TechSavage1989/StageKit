#pragma once

#include <obs-module.h>

namespace stagekit::native {
struct Component {
	void (*load)();
	void (*unload)() = nullptr;
	void (*post_load)() = nullptr;
};
extern const Component component;

inline const char *text(const char *key)
{
	return obs_module_get_locale_text(obs_get_module("stagekit"), key);
}

template<class Filter> obs_source_info video_filter(const char *id, const char *name)
{
	obs_source_info info{};
	info.id = id;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.type_data = const_cast<char *>(name);
	info.get_name = [](void *key) {
		return text(static_cast<const char *>(key));
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new Filter(settings, source);
	};
	info.destroy = [](void *instance) {
		delete static_cast<Filter *>(instance);
	};
	info.update = [](void *instance, obs_data_t *settings) {
		static_cast<Filter *>(instance)->update(settings);
	};
	info.video_render = [](void *instance, gs_effect_t *) {
		static_cast<Filter *>(instance)->render();
	};
	info.get_defaults = Filter::defaults;
	info.get_properties = Filter::properties;
	return info;
}
} // namespace stagekit::native
