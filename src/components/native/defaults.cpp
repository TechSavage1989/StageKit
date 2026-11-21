#include "component.hpp"
#include "item-settings.hpp"

#include <obs-frontend-api.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace stagekit::native {
namespace {
constexpr const char *video_id = "stagekit_port_source_defaults_video";
constexpr const char *audio_id = "stagekit_port_source_defaults_audio";
constexpr const char *options[] = {
	"copy_properties", "copy_filters",    "copy_audio_monitoring",      "copy_volume",
	"copy_muted",      "copy_balance",    "copy_sync_offset",           "copy_audio_tracks",
	"copy_transform",  "copy_visibility", "copy_visibility_transitions"};
constexpr const char *labels[] = {"Native.CopyProperties", "Native.CopyFilters",    "Native.CopyMonitor",
				  "Native.CopyVolume",     "Native.CopyMute",       "Native.CopyBalance",
				  "Native.CopySync",       "Native.CopyTracks",     "Native.CopyTransform",
				  "Native.CopyVisible",    "Native.CopyTransitions"};
std::atomic<bool> accepting{true};
std::atomic<unsigned> collection_epoch{0};
void frontend_event(obs_frontend_event e, void *)
{
	switch (e) {
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
	case OBS_FRONTEND_EVENT_EXIT:
		accepting.store(false);
		++collection_epoch;
		break;
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		accepting.store(true);
		break;
	default:
		break;
	}
}
bool marker(obs_source_t *source)
{
	const char *id = obs_source_get_unversioned_id(source);
	return std::strcmp(id, video_id) == 0 || std::strcmp(id, audio_id) == 0 ||
	       std::strcmp(id, "source_defaults_video") == 0 || std::strcmp(id, "source_defaults_audio") == 0;
}
struct ItemSearch {
	obs_source_t *source;
	OBSSceneItem result;
};
bool find_item(void *p, obs_source_t *candidate)
{
	auto &search = *static_cast<ItemSearch *>(p);
	auto *scene = obs_scene_from_source(candidate);
	if (!scene)
		return true;
	auto *item = obs_scene_find_source_recursive(scene, obs_source_get_name(search.source));
	if (item && obs_sceneitem_get_source(item) == search.source)
		search.result = item;
	return !search.result;
}
struct Template {
	struct NewSource {
		OBSWeakSource source;
		unsigned epoch;
		float remaining = 2;
		bool applied = false;
	};
	obs_source_t *filter;
	std::mutex mutex;
	std::vector<NewSource> pending;
	bool attached = false;

	explicit Template(obs_source_t *self) : filter(self) {}
	~Template() { detach(); }
	void attach()
	{
		if (attached)
			return;
		attached = true;
		auto *signals = obs_get_signal_handler();
		signal_handler_connect(signals, "source_create", created, this);
		signal_handler_connect(signals, "source_load", restored, this);
		signal_handler_connect(signals, "source_rename", renamed, this);
		obs_add_tick_callback(tick, this);
	}
	void detach()
	{
		if (!attached)
			return;
		attached = false;
		obs_remove_tick_callback(tick, this);
		auto *signals = obs_get_signal_handler();
		signal_handler_disconnect(signals, "source_create", created, this);
		signal_handler_disconnect(signals, "source_load", restored, this);
		signal_handler_disconnect(signals, "source_rename", renamed, this);
		std::lock_guard guard(mutex);
		pending.clear();
	}
	static void created(void *p, calldata_t *cd)
	{
		auto &self = *static_cast<Template *>(p);
		auto *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
		auto *model = obs_filter_get_parent(self.filter);
		if (!accepting.load() || !model || !source || source == model || !obs_source_enabled(self.filter) ||
		    obs_obj_is_private(source) || obs_source_get_type(source) != OBS_SOURCE_TYPE_INPUT ||
		    obs_scene_from_source(source) || obs_group_from_source(source) ||
		    std::strcmp(obs_source_get_unversioned_id(model), obs_source_get_unversioned_id(source)) != 0)
			return;
		std::lock_guard guard(self.mutex);
		self.pending.push_back({OBSGetWeakRef(source), collection_epoch.load()});
	}
	static void restored(void *p, calldata_t *cd)
	{
		auto &self = *static_cast<Template *>(p);
		auto *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
		std::lock_guard guard(self.mutex);
		self.pending.erase(std::remove_if(self.pending.begin(), self.pending.end(),
						  [source](const NewSource &v) {
							  return obs_weak_source_references_source(v.source, source);
						  }),
				   self.pending.end());
	}
	static void renamed(void *p, calldata_t *cd)
	{
		auto &self = *static_cast<Template *>(p);
		auto *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
		if (!source || !obs_scene_from_source(source))
			return;
		OBSDataAutoRelease s = obs_source_get_settings(self.filter);
		const char *before = calldata_string(cd, "prev_name");
		if (before && std::strcmp(before, obs_data_get_string(s, "parent_scene")) == 0)
			obs_data_set_string(s, "parent_scene", obs_source_get_name(source));
	}
	static void apply_source(obs_source_t *model, obs_source_t *source, obs_data_t *s)
	{
		if (obs_data_get_bool(s, "copy_properties")) {
			OBSDataAutoRelease properties = obs_source_get_settings(model);
			obs_source_update(source, properties);
		}
		if (obs_data_get_bool(s, "copy_filters"))
			obs_source_enum_filters(
				model,
				[](obs_source_t *, obs_source_t *child, void *arg) {
					if (!marker(child))
						obs_source_copy_single_filter(static_cast<obs_source_t *>(arg), child);
				},
				source);
		if (obs_data_get_bool(s, "copy_audio_monitoring"))
			obs_source_set_monitoring_type(source, obs_source_get_monitoring_type(model));
		if (obs_data_get_bool(s, "copy_volume"))
			obs_source_set_volume(source, obs_source_get_volume(model));
		if (obs_data_get_bool(s, "copy_muted"))
			obs_source_set_muted(source, obs_source_muted(model));
		if (obs_data_get_bool(s, "copy_balance"))
			obs_source_set_balance_value(source, obs_source_get_balance_value(model));
		if (obs_data_get_bool(s, "copy_sync_offset"))
			obs_source_set_sync_offset(source, obs_source_get_sync_offset(model));
		if (obs_data_get_bool(s, "copy_audio_tracks"))
			obs_source_set_audio_mixers(source, obs_source_get_audio_mixers(model));
		const std::string prefix = obs_data_get_string(s, "name_prefix");
		const std::string name = obs_source_get_name(source);
		if (obs_data_get_bool(s, "name_settings") && !prefix.empty() &&
		    (!obs_data_get_bool(s, "prefix_not_yet_applied") || name.rfind(prefix, 0) != 0))
			obs_source_set_name(source, (prefix + name).c_str());
	}
	static bool apply_item(obs_source_t *model, obs_source_t *source, obs_data_t *s)
	{
		if (!obs_data_get_bool(s, "scene_item_settings"))
			return true;
		ItemSearch from{model, {}}, to{source, {}};
		const char *name = obs_data_get_string(s, "parent_scene");
		if (*name) {
			OBSSourceAutoRelease scene = obs_get_source_by_name(name);
			if (scene)
				find_item(&from, scene);
		} else {
			obs_enum_scenes(find_item, &from);
		}
		if (!from.result)
			return false;
		obs_enum_scenes(find_item, &to);
		if (!to.result)
			return false;
		if (obs_data_get_bool(s, "copy_transform"))
			copy_transform(from.result, to.result);
		if (obs_data_get_bool(s, "copy_visibility"))
			obs_sceneitem_set_visible(to.result, obs_sceneitem_visible(from.result));
		if (obs_data_get_bool(s, "copy_visibility_transitions"))
			copy_item_transitions(from.result, to.result);
		return true;
	}
	static void tick(void *p, float seconds)
	{
		auto &self = *static_cast<Template *>(p);
		std::vector<NewSource> work;
		{
			std::lock_guard guard(self.mutex);
			work.swap(self.pending);
		}
		if (work.empty() || !accepting.load() || !obs_source_enabled(self.filter))
			return;
		OBSSource model = obs_filter_get_parent(self.filter);
		if (!model)
			return;
		OBSDataAutoRelease settings = obs_source_get_settings(self.filter);
		for (auto &entry : work) {
			auto target = OBSGetStrongRef(entry.source);
			if (entry.epoch != collection_epoch.load() || !target || obs_source_removed(target))
				continue;
			if (!entry.applied) {
				apply_source(model, target, settings);
				entry.applied = true;
			}
			if (!apply_item(model, target, settings) && (entry.remaining -= seconds) > 0) {
				std::lock_guard guard(self.mutex);
				self.pending.push_back(std::move(entry));
			}
		}
	}
};
obs_properties_t *properties(void *)
{
	auto *p = obs_properties_create();
	for (size_t i = 0; i < 8; ++i)
		obs_properties_add_bool(p, options[i], text(labels[i]));
	auto *items = obs_properties_create();
	auto *scenes = obs_properties_add_list(items, "parent_scene", text("Native.TemplateScene"), OBS_COMBO_TYPE_LIST,
					       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(scenes, text("Native.AnyScene"), "");
	obs_enum_scenes(
		[](void *arg, obs_source_t *scene) {
			const char *name = obs_source_get_name(scene);
			obs_property_list_add_string(static_cast<obs_property_t *>(arg), name, name);
			return true;
		},
		scenes);
	for (size_t i = 8; i < 11; ++i)
		obs_properties_add_bool(items, options[i], text(labels[i]));
	obs_properties_add_group(p, "scene_item_settings", text("Native.ItemDefaults"), OBS_GROUP_CHECKABLE, items);
	auto *names = obs_properties_create();
	obs_properties_add_text(names, "name_prefix", text("Native.Prefix"), OBS_TEXT_DEFAULT);
	obs_properties_add_bool(names, "prefix_not_yet_applied", text("Native.PrefixOnce"));
	obs_properties_add_group(p, "name_settings", text("Native.SourceNaming"), OBS_GROUP_CHECKABLE, names);
	return p;
}
void load()
{
	if (obs_frontend_get_main_window()) {
		accepting.store(false);
		obs_frontend_add_event_callback(frontend_event, nullptr);
	}
	for (const char *id : {video_id, audio_id}) {
		obs_source_info info{};
		info.id = id;
		info.version = 1;
		info.type = OBS_SOURCE_TYPE_FILTER;
		info.output_flags = id == video_id ? OBS_SOURCE_VIDEO : OBS_SOURCE_AUDIO;
		info.get_name = [](void *) {
			return text("Ports.SourceDefaults");
		};
		info.create = [](obs_data_t *, obs_source_t *source) -> void * {
			return new Template(source);
		};
		info.destroy = [](void *p) {
			delete static_cast<Template *>(p);
		};
		info.filter_add = [](void *p, obs_source_t *) {
			static_cast<Template *>(p)->attach();
		};
		info.filter_remove = [](void *p, obs_source_t *) {
			static_cast<Template *>(p)->detach();
		};
		info.get_properties = properties;
		info.get_defaults = [](obs_data_t *s) {
			for (const char *key : options)
				obs_data_set_default_bool(s, key, true);
			obs_data_set_default_bool(s, "name_settings", true);
			obs_data_set_default_bool(s, "prefix_not_yet_applied", true);
		};
		if (id == video_id)
			info.video_render = [](void *p, gs_effect_t *) {
				obs_source_skip_video_filter(static_cast<Template *>(p)->filter);
			};
		obs_register_source(&info);
	}
}
void unload()
{
	accepting.store(false);
	if (obs_frontend_get_main_window())
		obs_frontend_remove_event_callback(frontend_event, nullptr);
}
} // namespace
const Component component{load, unload};
} // namespace stagekit::native
