#include "component.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <future>
#include <mutex>

namespace stagekit::native {
namespace {
struct Search {
	QString directory, suffix, contains;
	int order = 2;
};
struct Result {
	uint64_t generation;
	QString selected;
};
Result find_file(Search query, uint64_t generation, bool random)
{
	Result result{generation, {}};
	if (query.directory.isEmpty())
		return result;
	QFileInfoList entries =
		QDir(query.directory).entryInfoList(QDir::Files | QDir::Readable | QDir::NoDotAndDotDot, QDir::Name);
	entries.erase(std::remove_if(entries.begin(), entries.end(),
				     [&](const QFileInfo &file) {
					     return (!query.suffix.isEmpty() &&
						     file.suffix().compare(query.suffix, Qt::CaseInsensitive) != 0) ||
						    !file.fileName().contains(query.contains);
				     }),
		      entries.end());
	if (entries.empty())
		return result;
	auto stamp = [&](const QFileInfo &entry) {
		auto created = entry.birthTime();
		return query.order < 2 && created.isValid() ? created : entry.lastModified();
	};
	if (random || query.order == 6) {
		result.selected =
			entries.at(QRandomGenerator::global()->bounded(int(entries.size()))).absoluteFilePath();
	} else if (query.order >= 4) {
		result.selected = (query.order == 4 ? entries.first() : entries.last()).absoluteFilePath();
	} else {
		const auto best =
			std::min_element(entries.cbegin(), entries.cend(), [&](const QFileInfo &a, const QFileInfo &b) {
				return query.order % 2 ? stamp(a) < stamp(b) : stamp(a) > stamp(b);
			});
		result.selected = best->absoluteFilePath();
	}
	return result;
}

enum class Command { Clear, Random, Refresh, First, Last, DeleteFirst, DeleteLast };
struct Watch {
	obs_source_t *source;
	Search search;
	uint64_t generation = 0;
	int interval = 1000;
	double elapsed = 0;
	bool refresh = true, random = false, was_enabled = false;
	QString previous;
	std::future<Result> scan;
	std::array<obs_hotkey_id, 7> bindings;
	std::mutex lock;
	std::deque<Command> commands;
	Watch(obs_data_t *s, obs_source_t *context) : source(context)
	{
		bindings.fill(OBS_INVALID_HOTKEY_ID);
		update(s);
	}
	~Watch() { unbind(); }
	void update(obs_data_t *s)
	{
		const auto qstring = [&](const char *key) {
			return QString::fromUtf8(obs_data_get_string(s, key));
		};
		search.directory = qstring("dir");
		search.suffix = qstring("extension").trimmed();
		if (search.suffix.startsWith('.'))
			search.suffix.remove(0, 1);
		search.contains = qstring("filter");
		search.order = int(std::clamp<long long>(obs_data_get_int(s, "sort_by"), 0, 6));
		interval = int(std::clamp<long long>(obs_data_get_int(s, "scan_interval"), 0, 3600000));
		++generation;
		refresh = true;
		previous.clear();
	}
	static void defaults(obs_data_t *s)
	{
		obs_data_set_default_int(s, "sort_by", 2);
		obs_data_set_default_int(s, "scan_interval", 1000);
	}
	static obs_properties_t *properties(void *)
	{
		auto *p = obs_properties_create();
		obs_properties_add_path(p, "dir", text("Native.Directory"), OBS_PATH_DIRECTORY, nullptr, nullptr);
		auto *order = obs_properties_add_list(p, "sort_by", text("Native.PickFile"), OBS_COMBO_TYPE_LIST,
						      OBS_COMBO_FORMAT_INT);
		int i = 0;
		for (const char *label :
		     {"Native.CreatedNew", "Native.CreatedOld", "Native.ModifiedNew", "Native.ModifiedOld",
		      "Native.Alphabetical", "Native.ReverseAlphabetical", "Native.RandomFile"})
			obs_property_list_add_int(order, text(label), i++);
		obs_properties_add_text(p, "extension", text("Native.Extension"), OBS_TEXT_DEFAULT);
		obs_properties_add_text(p, "filter", text("Native.NameContains"), OBS_TEXT_DEFAULT);
		obs_property_int_set_suffix(
			obs_properties_add_int(p, "scan_interval", text("Native.Rescan"), 0, 3600000, 100), " ms");
		return p;
	}
	template<Command C> static void press(void *ptr, obs_hotkey_id, obs_hotkey_t *, bool down)
	{
		if (!down)
			return;
		auto &self = *static_cast<Watch *>(ptr);
		std::lock_guard guard(self.lock);
		self.commands.push_back(C);
	}
	void bind(obs_source_t *parent)
	{
		unbind();
		struct Key {
			const char *id, *label;
			obs_hotkey_func callback;
		};
		const Key keys[] = {{"dwm_clear", "Native.ClearMedia", press<Command::Clear>},
				    {"dwm_random", "Native.RandomFile", press<Command::Random>},
				    {"dwm_refresh", "Native.RefreshMedia", press<Command::Refresh>},
				    {"dwm_remove_first", "Native.RemoveFirst", press<Command::First>},
				    {"dwm_remove_last", "Native.RemoveLast", press<Command::Last>},
				    {"dwm_delete_first", "Native.DeleteFirst", press<Command::DeleteFirst>},
				    {"dwm_delete_last", "Native.DeleteLast", press<Command::DeleteLast>}};
		for (size_t i = 0; i < bindings.size(); ++i)
			bindings[i] = obs_hotkey_register_source(parent, keys[i].id, text(keys[i].label),
								 keys[i].callback, this);
	}
	void unbind()
	{
		for (auto &id : bindings) {
			if (id != OBS_INVALID_HOTKEY_ID)
				obs_hotkey_unregister(id);
			id = OBS_INVALID_HOTKEY_ID;
		}
	}
	void select(obs_source_t *parent, const QString &file)
	{
		const QByteArray id = obs_source_get_unversioned_id(parent);
		auto *settings = obs_data_create();
		const auto path = file.toUtf8();
		bool change = true;
		if (id == "vlc_source") {
			auto *old = obs_source_get_settings(parent);
			auto *list = obs_data_get_array(old, "playlist");
			obs_data_release(old);
			if (!list || file.isEmpty()) {
				obs_data_array_release(list);
				list = obs_data_array_create();
			}
			for (size_t i = 0; i < obs_data_array_count(list); ++i) {
				auto *entry = obs_data_array_item(list, i);
				if (QString::fromUtf8(obs_data_get_string(entry, "value")) == file)
					change = false;
				obs_data_release(entry);
			}
			if (change && !file.isEmpty()) {
				auto *entry = obs_data_create();
				obs_data_set_string(entry, "value", path.constData());
				obs_data_array_push_back(list, entry);
				obs_data_release(entry);
			}
			obs_data_set_array(settings, "playlist", list);
			obs_data_array_release(list);
		} else if (id == "ffmpeg_source") {
			obs_data_set_bool(settings, "is_local_file", true);
			obs_data_set_string(settings, "local_file", path.constData());
		} else if (id == "image_source" || id == "xObsAsyncImageSource") {
			obs_data_set_string(settings, "file", path.constData());
		} else {
			change = false;
		}
		if (change) {
			obs_source_update(parent, settings);
			if (id == "ffmpeg_source" && !file.isEmpty())
				obs_source_media_restart(parent);
		}
		obs_data_release(settings);
	}
	void trim_playlist(obs_source_t *parent, Command command)
	{
		if (QByteArray(obs_source_get_unversioned_id(parent)) != "vlc_source")
			return;
		auto *s = obs_source_get_settings(parent);
		auto *list = obs_data_get_array(s, "playlist");
		const size_t count = obs_data_array_count(list);
		if (count) {
			const size_t i = command == Command::First || command == Command::DeleteFirst ? 0 : count - 1;
			auto *entry = obs_data_array_item(list, i);
			const QFileInfo file(QString::fromUtf8(obs_data_get_string(entry, "value")));
			obs_data_release(entry);
			obs_data_array_erase(list, i);
			obs_source_update(parent, s);
			if (command == Command::DeleteFirst || command == Command::DeleteLast) {
				const auto dir = QDir(search.directory).canonicalPath();
				if (!search.directory.isEmpty() && !dir.isEmpty() && !file.isSymLink() &&
				    file.isFile() && file.canonicalPath() == dir)
					QFile::remove(file.absoluteFilePath());
			}
		}
		obs_data_array_release(list);
		obs_data_release(s);
	}
	void tick(float seconds)
	{
		auto *parent = obs_filter_get_parent(source);
		const bool enabled = obs_source_enabled(source);
		if (!parent || !enabled) {
			if (was_enabled)
				++generation;
			was_enabled = false;
			return;
		}
		if (!was_enabled)
			refresh = true;
		was_enabled = true;
		std::deque<Command> due;
		{
			std::lock_guard guard(lock);
			due.swap(commands);
		}
		for (auto command : due) {
			if (command == Command::Clear) {
				++generation;
				select(parent, {});
			} else if (command == Command::Random || command == Command::Refresh) {
				++generation;
				refresh = true;
				random = command == Command::Random;
				previous.clear();
			} else {
				trim_playlist(parent, command);
			}
		}
		if (scan.valid() && scan.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
			auto result = scan.get();
			if (result.generation == generation && result.selected != previous) {
				previous = result.selected;
				select(parent, previous);
			}
		}
		elapsed += seconds * 1000.;
		if (!scan.valid() && !search.directory.isEmpty() && (refresh || (interval && elapsed >= interval))) {
			scan = std::async(std::launch::async, find_file, search, generation, random);
			refresh = random = false;
			elapsed = 0;
		}
	}
};
void load()
{
	obs_source_info info{};
	info.id = "stagekit_port_dir_watch_media";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) {
		return text("Ports.DirectoryWatchMedia");
	};
	info.create = [](obs_data_t *s, obs_source_t *source) -> void * {
		return new Watch(s, source);
	};
	info.destroy = [](void *p) {
		delete static_cast<Watch *>(p);
	};
	info.update = [](void *p, obs_data_t *s) {
		static_cast<Watch *>(p)->update(s);
	};
	info.get_defaults = Watch::defaults;
	info.get_properties = Watch::properties;
	info.filter_add = [](void *p, obs_source_t *parent) {
		static_cast<Watch *>(p)->bind(parent);
	};
	info.filter_remove = [](void *p, obs_source_t *) {
		static_cast<Watch *>(p)->unbind();
	};
	info.video_tick = [](void *p, float elapsed) {
		static_cast<Watch *>(p)->tick(elapsed);
	};
	info.video_render = [](void *p, gs_effect_t *) {
		obs_source_skip_video_filter(static_cast<Watch *>(p)->source);
	};
	obs_register_source(&info);
}
} // namespace
const Component component{load};
} // namespace stagekit::native
