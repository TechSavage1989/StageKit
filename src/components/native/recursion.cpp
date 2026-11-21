#include "frame.hpp"
#include "enable-keys.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <memory>

namespace stagekit::native {
namespace {
struct Feedback {
	struct Sample {
		uint64_t time;
		std::unique_ptr<Frame> frame;
	};
	obs_source_t *source;
	EnableKeys keys;
	Effect shader{"effects/stagekit-feedback.effect"};
	std::deque<Sample> history;
	std::unique_ptr<Frame> work = std::make_unique<Frame>();
	Frame *last = nullptr;
	Size canvas{};
	uint64_t rendered = 0, delay = 0;
	gs_color_space space = GS_CS_SRGB;
	float x = 0, y = 0, sx = 1, sy = 1, angle = 0, opacity = 1;
	bool foreground = false;
	std::atomic<int> trigger{0};
	std::atomic<bool> reset{true};
	Feedback(obs_data_t *s, obs_source_t *context) : source(context), keys(context)
	{
		update(s);
		signal_handler_connect(obs_source_get_signal_handler(source), "enable", enabled, this);
	}
	~Feedback() { signal_handler_disconnect(obs_source_get_signal_handler(source), "enable", enabled, this); }
	static void enabled(void *ptr, calldata_t *cd)
	{
		if (calldata_bool(cd, "enabled"))
			static_cast<Feedback *>(ptr)->event(5);
	}
	void event(int which)
	{
		if (trigger.load() == which)
			reset.store(true);
	}
	void update(obs_data_t *s)
	{
		delay = uint64_t(std::clamp<long long>(obs_data_get_int(s, "delay_ms"), 0, 1000)) * 1000000ULL;
		x = float(obs_data_get_double(s, "offset_x"));
		y = float(obs_data_get_double(s, "offset_y"));
		sx = float(std::clamp(obs_data_get_double(s, "scale_x"), .01, 10.));
		sy = float(std::clamp(obs_data_get_double(s, "scale_y"), .01, 10.));
		angle = float(obs_data_get_double(s, "rotation") * .017453292519943295);
		opacity = float(std::clamp(obs_data_get_double(s, "alpha"), .0, 1.0));
		foreground = obs_data_get_bool(s, "inversed");
		trigger.store(int(obs_data_get_int(s, "reset_trigger")));
	}
	static void defaults(obs_data_t *s)
	{
		for (const char *key : {"alpha", "scale_x", "scale_y"})
			obs_data_set_default_double(s, key, 1.0);
	}
	static obs_properties_t *properties(void *)
	{
		auto *p = obs_properties_create();
		obs_property_int_set_suffix(
			obs_properties_add_int(p, "delay_ms", text("Native.FeedbackDelay"), 1, 1000, 1), " ms");
		obs_properties_add_float_slider(p, "offset_x", text("Native.PositionX"), -1000, 1000, 1);
		obs_properties_add_float_slider(p, "offset_y", text("Native.PositionY"), -1000, 1000, 1);
		obs_properties_add_float_slider(p, "scale_x", text("Native.HorizontalScale"), .01, 10, .01);
		obs_properties_add_float_slider(p, "scale_y", text("Native.VerticalScale"), .01, 10, .01);
		obs_properties_add_float_slider(p, "rotation", text("Native.Rotation"), -360, 360, 1);
		obs_properties_add_float_slider(p, "alpha", text("Native.FeedbackOpacity"), .001, 1, .001);
		obs_properties_add_bool(p, "inversed", text("Native.FeedbackFront"));
		auto *choice = obs_properties_add_list(p, "reset_trigger", text("Native.ResetHistory"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		int value = 0;
		for (const char *label : {"Native.None", "Native.Show", "Native.Hide", "Native.Activate",
					  "Native.Deactivate", "Native.WhenEnabled"})
			obs_property_list_add_int(choice, text(label), value++);
		return p;
	}
	void render()
	{
		const Size size = size_of_target(source);
		if (!size || !shader.value) {
			obs_source_skip_video_filter(source);
			return;
		}
		const auto now = obs_get_video_frame_time();
		const auto current_space = gs_get_color_space();
		if (reset.exchange(false) || size.width != canvas.width || size.height != canvas.height ||
		    current_space != space) {
			last = nullptr;
			history.clear();
			canvas = size;
			space = current_space;
		}
		if (last && rendered == now) {
			draw_texture(last->texture(), size);
			return;
		}
		const uint64_t cutoff = now > delay ? now - delay : 0;
		std::unique_ptr<Frame> recycle;
		while (history.size() > 1 && history[1].time <= cutoff) {
			recycle = std::move(history.front().frame);
			history.pop_front();
		}
		bool ok = false;
		if (history.empty() || history.front().time > cutoff) {
			ok = work->capture(source, size, space);
		} else if (work->begin(size, space)) {
			ok = obs_source_process_filter_begin_with_color_space(source, gs_get_format_from_space(space),
									      space, OBS_NO_DIRECT_RENDERING);
			if (ok) {
				shader.pair("canvas", float(size.width), float(size.height));
				shader.four("motion", x, y, std::cos(angle), std::sin(angle));
				shader.four("mixing", sx, sy, opacity, foreground ? 1.0f : 0.0f);
				gs_effect_set_texture(shader.parameter("older"), history.front().frame->texture());
				obs_source_process_filter_end(source, shader.value, size.width, size.height);
			}
			work->end();
		}
		if (!ok) {
			last = nullptr;
			obs_source_skip_video_filter(source);
			return;
		}
		last = work.get();
		rendered = now;
		const uint64_t frame_bytes =
			uint64_t(size.width) * size.height * (gs_get_format_from_space(space) == GS_RGBA16F ? 8 : 4);
		const uint64_t slots = std::clamp<uint64_t>((128ULL << 20) / frame_bytes, 2, 120);
		if (history.empty() || now - history.back().time >= (delay + slots - 2) / (slots - 1)) {
			history.push_back({now, std::move(work)});
			work = recycle ? std::move(recycle) : std::make_unique<Frame>();
		}
		draw_texture(last->texture(), size);
	}
};
void load()
{
	auto info = video_filter<Feedback>("stagekit_port_recursion_effect_filter", "Ports.RecursionEffect");
	info.output_flags |= OBS_SOURCE_SRGB;
	info.filter_add = [](void *p, obs_source_t *parent) {
		static_cast<Feedback *>(p)->keys.bind(parent, "RecursionEffect.Enable", "RecursionEffect.Disable",
						      "Native.FeedbackOn", "Native.FeedbackOff");
	};
	info.filter_remove = [](void *p, obs_source_t *) {
		static_cast<Feedback *>(p)->keys.clear();
	};
	info.show = [](void *p) {
		static_cast<Feedback *>(p)->event(1);
	};
	info.hide = [](void *p) {
		static_cast<Feedback *>(p)->event(2);
	};
	info.activate = [](void *p) {
		static_cast<Feedback *>(p)->event(3);
	};
	info.deactivate = [](void *p) {
		static_cast<Feedback *>(p)->event(4);
	};
	obs_register_source(&info);
}
} // namespace
const Component component{load};
} // namespace stagekit::native
