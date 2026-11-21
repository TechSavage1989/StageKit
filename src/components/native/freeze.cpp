#include "frame.hpp"

#include <obs-frontend-api.h>
#include <algorithm>
#include <array>
#include <atomic>

namespace stagekit::native {
namespace {
constexpr std::array<const char *, 4> triggers{"activate_action", "deactivate_action", "show_action", "hide_action"};
constexpr std::array<const char *, 4> events{"Native.Activate", "Native.Deactivate", "Native.Show", "Native.Hide"};
constexpr std::array<const char *, 4> margins{"mask_left", "mask_right", "mask_top", "mask_bottom"};
constexpr std::array<const char *, 4> sides{"Native.Left", "Native.Right", "Native.Top", "Native.Bottom"};

struct Freeze {
	obs_source_t *source;
	Frame frame;
	Effect mask_shader{"effects/stagekit-freeze.effect"};
	Size held_size{};
	gs_color_space held_space = GS_CS_SRGB;
	std::array<float, 4> edges{};
	std::array<std::atomic<int>, 4> actions{};
	std::atomic<int> requested{0};
	std::atomic<bool> reset{true};
	obs_hotkey_pair_id pair = OBS_INVALID_HOTKEY_PAIR_ID;
	obs_hotkey_id screenshot = OBS_INVALID_HOTKEY_ID;
	int maximum = 0, refresh = 0, start_delay = 0, end_delay = 0, pending = 0;
	double elapsed = 0, since_capture = 0, wait = 0;
	bool mask = false, logging = false, valid = false;

	Freeze(obs_data_t *settings, obs_source_t *context) : source(context)
	{
		update(settings);
		signal_handler_connect(obs_source_get_signal_handler(source), "enable", on_enable, this);
	}
	~Freeze()
	{
		signal_handler_disconnect(obs_source_get_signal_handler(source), "enable", on_enable, this);
		unbind();
	}
	static void on_enable(void *data, calldata_t *) { static_cast<Freeze *>(data)->reset.store(true); }
	static void defaults(obs_data_t *) {}
	void update(obs_data_t *s)
	{
		maximum = int(std::clamp<long long>(obs_data_get_int(s, "duration"), 0, 100000));
		refresh = int(std::clamp<long long>(obs_data_get_int(s, "refresh_interval"), 0, 100000));
		start_delay = int(std::clamp<long long>(obs_data_get_int(s, "start_delay"), 0, 100000));
		end_delay = int(std::clamp<long long>(obs_data_get_int(s, "end_delay"), 0, 100000));
		mask = obs_data_get_bool(s, "mask");
		logging = obs_data_get_bool(s, "log_enable");
		for (size_t i = 0; i < 4; ++i) {
			edges[i] = float(std::clamp(obs_data_get_double(s, margins[i]), 0.0, 100.0) / 100.0);
			actions[i].store(int(std::clamp<long long>(obs_data_get_int(s, triggers[i]), 0, 2)));
		}
	}
	static obs_properties_t *properties(void *)
	{
		auto *p = obs_properties_create();
		auto milliseconds = [&](const char *key, const char *label) {
			auto *prop = obs_properties_add_int(p, key, text(label), 0, 100000, 10);
			obs_property_int_set_suffix(prop, " ms");
		};
		milliseconds("duration", "Native.FreezeDuration");
		milliseconds("refresh_interval", "Native.FreezeRefresh");
		milliseconds("start_delay", "Native.StartDelay");
		milliseconds("end_delay", "Native.EndDelay");
		obs_properties_add_bool(p, "log_enable", text("Native.FreezeLog"));
		auto *m = obs_properties_create();
		auto *a = obs_properties_create();
		for (size_t i = 0; i < 4; ++i) {
			auto *side = obs_properties_add_float_slider(m, margins[i], text(sides[i]), 0, 100, .01);
			obs_property_float_set_suffix(side, "%");
			auto *rule = obs_properties_add_list(a, triggers[i], text(events[i]), OBS_COMBO_TYPE_LIST,
							     OBS_COMBO_FORMAT_INT);
			obs_property_list_add_int(rule, text("Native.None"), 0);
			obs_property_list_add_int(rule, text("Native.Hold"), 1);
			obs_property_list_add_int(rule, text("Native.Release"), 2);
		}
		obs_properties_add_group(p, "mask", text("Native.FreezeRegion"), OBS_GROUP_CHECKABLE, m);
		obs_properties_add_group(p, "action", text("Native.FreezeEvents"), OBS_GROUP_NORMAL, a);
		return p;
	}
	template<int Action> static bool key(void *data, obs_hotkey_pair_id, obs_hotkey_t *, bool pressed)
	{
		auto *self = static_cast<Freeze *>(data);
		if (!pressed || obs_source_enabled(self->source) == (Action == 1))
			return false;
		self->requested.store(Action);
		return true;
	}
	void bind(obs_source_t *parent)
	{
		unbind();
		pair = obs_hotkey_pair_register_source(parent, "Freeze.Enable", text("Native.Hold"), "Freeze.Disable",
						       text("Native.Release"), key<1>, key<2>, this, this);
		screenshot = obs_hotkey_register_source(
			parent, "Freeze.Screenshot", text("Native.FreezeScreenshot"),
			[](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
				if (pressed)
					obs_frontend_take_source_screenshot(static_cast<Freeze *>(data)->source);
			},
			this);
	}
	void unbind()
	{
		if (pair != OBS_INVALID_HOTKEY_PAIR_ID)
			obs_hotkey_pair_unregister(pair);
		if (screenshot != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(screenshot);
		pair = OBS_INVALID_HOTKEY_PAIR_ID;
		screenshot = OBS_INVALID_HOTKEY_ID;
	}
	void tick(float seconds)
	{
		const double dt = 1000.0 * seconds;
		if (const int next = requested.exchange(0)) {
			pending = next;
			wait = pending == 1 ? start_delay : end_delay;
		}
		if (pending && (wait -= dt) <= 0) {
			const bool enable = pending == 1;
			pending = 0;
			obs_source_set_enabled(source, enable);
		}
		if (reset.exchange(false) || !obs_source_enabled(source)) {
			valid = false;
			elapsed = since_capture = 0;
		}
		if (!obs_source_enabled(source))
			return;
		elapsed += dt;
		since_capture += dt;
		if (maximum && elapsed >= maximum)
			obs_source_set_enabled(source, false);
		else if (refresh && since_capture >= refresh)
			valid = false;
	}
	void render()
	{
		const Size size = size_of_target(source);
		const auto space = gs_get_color_space();
		if (!size) {
			obs_source_skip_video_filter(source);
			return;
		}
		if (!valid || size.width != held_size.width || size.height != held_size.height || space != held_space) {
			valid = frame.capture(source, size, space);
			held_size = size;
			held_space = space;
			since_capture = 0;
			if (valid && logging)
				blog(LOG_INFO, "[stagekit] Captured a freeze frame for %s",
				     obs_source_get_name(source));
		}
		if (!valid) {
			obs_source_skip_video_filter(source);
		} else if (mask && mask_shader.value) {
			if (!obs_source_process_filter_begin_with_color_space(source, gs_get_format_from_space(space),
									      space, OBS_NO_DIRECT_RENDERING))
				return;
			mask_shader.four("edges", edges[0], edges[1], edges[2], edges[3]);
			gs_effect_set_texture(mask_shader.parameter("held"), frame.texture());
			obs_source_process_filter_end(source, mask_shader.value, size.width, size.height);
		} else {
			draw_texture(frame.texture(), size);
		}
	}
	void event(size_t index)
	{
		if (const int next = actions[index].load())
			requested.store(next);
	}
};
void load()
{
	auto info = video_filter<Freeze>("stagekit_port_freeze_filter", "Ports.Freeze");
	info.output_flags |= OBS_SOURCE_SRGB;
	info.filter_add = [](void *p, obs_source_t *parent) {
		static_cast<Freeze *>(p)->bind(parent);
	};
	info.filter_remove = [](void *p, obs_source_t *) {
		static_cast<Freeze *>(p)->unbind();
	};
	info.activate = [](void *p) {
		static_cast<Freeze *>(p)->event(0);
	};
	info.deactivate = [](void *p) {
		static_cast<Freeze *>(p)->event(1);
	};
	info.show = [](void *p) {
		static_cast<Freeze *>(p)->event(2);
	};
	info.hide = [](void *p) {
		static_cast<Freeze *>(p)->event(3);
	};
	info.video_tick = [](void *p, float seconds) {
		static_cast<Freeze *>(p)->tick(seconds);
	};
	obs_register_source(&info);
}
} // namespace
const Component component{load};
} // namespace stagekit::native
