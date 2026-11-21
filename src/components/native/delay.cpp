#include "frame.hpp"
#include "../../common/easing.hpp"

#include <obs.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>

namespace stagekit::native {
namespace {
constexpr std::array<const char *, 9> key_names{"skip_begin", "skip_end", "forward", "slow_forward", "fast_forward", "backward", "slow_backward", "fast_backward", "pause"};
constexpr std::array<const char *, 9> key_labels{"Native.DelayOldest", "Native.DelayLive", "Native.PlayForward", "Native.SlowForward", "Native.FastForward", "Native.PlayBackward", "Native.SlowBackward", "Native.FastBackward", "Native.Pause"};
constexpr std::array<EaseCurve, 11> curves{EaseCurve::Linear, EaseCurve::Quad, EaseCurve::Cubic, EaseCurve::Quart, EaseCurve::Quint, EaseCurve::Sine, EaseCurve::Circ, EaseCurve::Expo, EaseCurve::Elastic, EaseCurve::Bounce, EaseCurve::Back};
struct Delay {
	struct Sample {
		double time;
		std::unique_ptr<Frame> frame;
	};
	obs_source_t *source;
	std::deque<Sample> history;
	struct Key { Delay *owner; int index; obs_hotkey_id id; };
	std::array<Key, 9> bindings{};
	std::atomic<int> next{-1};
	std::array<double, 9> rates{1, 1, 1, .5, 2, -1, -.5, -2, 0};
	double span = 10, ramp = 1, cursor = 0, frame_time = 0, label_time = 0;
	double speed = 1, goal = 1;
	Tween tween{};
	EaseCurve curve = EaseCurve::Cubic;
	Size canvas{}, stored{};
	gs_color_space space = GS_CS_SRGB;
	std::string text_source, format, last_text;
	Delay(obs_data_t *s, obs_source_t *context) : source(context)
	{
		for (size_t i = 0; i < bindings.size(); ++i)
			bindings[i] = {this, int(i), OBS_INVALID_HOTKEY_ID};
		update(s);
	}
	~Delay() { unbind(); }
	void update(obs_data_t *s)
	{
		span = std::clamp(obs_data_get_double(s, "duration"), 0., 10000.);
		ramp = std::clamp(obs_data_get_double(s, "easing_duration"), 0., 10.);
		curve = curves[size_t(std::clamp<long long>(obs_data_get_int(s, "easing"), 0, 10))];
		for (size_t i : {3U, 4U, 6U, 7U})
			rates[i] = std::clamp(obs_data_get_double(s, key_names[i]) / 100., -10., 10.);
		text_source = obs_data_get_string(s, "text_source");
		format = obs_data_get_string(s, "text_format");
		last_text.clear();
	}
	static void defaults(obs_data_t *s)
	{
		obs_data_set_default_double(s, "duration", 10);
		obs_data_set_default_double(s, "easing_duration", 1);
		obs_data_set_default_int(s, "easing", 2);
		obs_data_set_default_double(s, "slow_forward", 50);
		obs_data_set_default_double(s, "fast_forward", 200);
		obs_data_set_default_double(s, "slow_backward", -50);
		obs_data_set_default_double(s, "fast_backward", -200);
		obs_data_set_default_string(s, "text_format", text("Native.DelayStatus"));
	}
	static obs_properties_t *properties(void *)
	{
		auto *p = obs_properties_create();
		obs_property_float_set_suffix(obs_properties_add_float(p, "duration", text("Native.HistoryDuration"), 0, 10000, .1), " s");
		auto *shape = obs_properties_add_list(p, "easing", text("Native.SpeedCurve"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		for (size_t i = 0; i < curves.size(); ++i)
			obs_property_list_add_int(shape, text(kEaseCurves[int(curves[i])].text), int(i));
		obs_property_float_set_suffix(obs_properties_add_float(p, "easing_duration", text("Native.SpeedTime"), 0, 10, .05), " s");
		const double limits[][2] = {{.1, 99.9}, {100.1, 1000}, {-99.9, -.1}, {-1000, -100.1}};
		size_t j = 0;
		for (size_t i : {3U, 4U, 6U, 7U}) {
			auto *prop = obs_properties_add_float_slider(p, key_names[i], text(key_labels[i]), limits[j][0], limits[j][1], .1);
			obs_property_float_set_suffix(prop, "%");
			++j;
		}
		obs_properties_add_text(p, "text_source", text("Native.StatusSource"), OBS_TEXT_DEFAULT);
		obs_properties_add_text(p, "text_format", text("Native.StatusFormat"), OBS_TEXT_MULTILINE);
		return p;
	}
	void bind(obs_source_t *parent)
	{
		unbind();
		for (auto &key : bindings)
			key.id = obs_hotkey_register_source(parent, key_names[key.index], text(key_labels[key.index]),
				[](void *p, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
					auto &key = *static_cast<Key *>(p);
					if (pressed)
						key.owner->next.store(key.index);
				}, &key);
	}
	void unbind()
	{
		for (auto &key : bindings) {
			if (key.id != OBS_INVALID_HOTKEY_ID)
				obs_hotkey_unregister(key.id);
			key.id = OBS_INVALID_HOTKEY_ID;
		}
	}
	void publish(double now)
	{
		if (text_source.empty() || now - label_time < .1)
			return;
		label_time = now;
		std::string value = format;
		auto expand = [&](const char *token, double number) {
			char buf[48];
			std::snprintf(buf, sizeof(buf), "%.1f", number);
			std::string key = token;
			size_t pos = 0;
			while ((pos = value.find(key, pos)) != std::string::npos) {
				value.replace(pos, key.size(), buf);
				pos += std::char_traits<char>::length(buf);
			}
		};
		expand("%SPEED%", speed * 100);
		expand("%TARGET%", goal * 100);
		expand("%TIME%", now - cursor);
		if (last_text == value)
			return;
		OBSSourceAutoRelease target = obs_get_source_by_name(text_source.c_str());
		if (target && target != source) {
			const std::string id = obs_source_get_unversioned_id(target);
			if (id == "text_gdiplus" || id == "text_ft2_source") {
				OBSDataAutoRelease s = obs_data_create();
				obs_data_set_string(s, "text", value.c_str());
				obs_source_update(target, s);
				last_text = value;
			}
		}
	}
	void render()
	{
		const auto size = size_of_target(source);
		if (!size || span <= 0) {
			history.clear();
			frame_time = 0;
			obs_source_skip_video_filter(source);
			return;
		}
		const double now = double(obs_get_video_frame_time()) / 1e9;
		const auto current_space = gs_get_color_space();
		const double fps = std::clamp(obs_get_active_fps(), 1., 240.);
		// Fit a smooth history in a fixed GPU budget. Live playback always draws the original.
		const double bytes = gs_get_format_from_space(current_space) == GS_RGBA16F ? 8. : 4.;
		const double budget = 256. * 1024. * 1024.;
		const double area = double(size.width) * size.height;
		const double scale = std::min(1., std::max(std::min(1., 180. / size.height),
			std::sqrt(budget / (area * bytes * std::max(2., span * fps + 2)))));
		const Size small{std::max(1U, uint32_t(std::round(size.width * scale))), std::max(1U, uint32_t(std::round(size.height * scale)))};
		const double capacity = std::max(2., std::floor(budget / (double(small.width) * small.height * bytes)));
		if (canvas.width != size.width || canvas.height != size.height || small.width != stored.width || small.height != stored.height || space != current_space || now < frame_time) {
			history.clear();
			canvas = size;
			stored = small;
			space = current_space;
			frame_time = cursor = now;
			speed = goal = 1;
			tween = {};
		}
		const double dt = std::max(0., now - frame_time);
		frame_time = now;
		std::unique_ptr<Frame> recycled;
		while (history.size() > 1 && history[1].time <= now - span) {
			recycled = std::move(history.front().frame);
			history.pop_front();
		}
		if (history.empty() || (now > history.back().time && now - history.back().time >= span / (capacity - 1))) {
			if (!recycled)
				recycled = std::make_unique<Frame>();
			if (recycled->capture(source, small, space))
				history.push_back({now, std::move(recycled)});
		}
		const double start = history.empty() ? now : history.front().time;
		const int cmd = next.exchange(-1);
		if (cmd == 0 || cmd == 1) {
			cursor = cmd == 0 ? start : now;
			speed = goal = 1;
			tween = {};
		} else if (cmd >= 2) {
			goal = rates[size_t(cmd)];
			tween.start(speed, goal, ramp, curve, EaseMode::InOut);
			if (ramp == 0)
				speed = goal;
		}
		if (tween.running())
			speed = tween.advance(dt);
		cursor = std::clamp(cursor + dt * speed, start, now);
		if ((cursor >= now && goal > 1) || (cursor <= start && goal < 0)) {
			tween = {};
			speed = goal = 1;
		}
		publish(now);
		if (history.empty() || cursor >= now - .5 / fps) {
			obs_source_skip_video_filter(source);
			return;
		}
		auto it = std::upper_bound(history.begin(), history.end(), cursor, [](double t, const Sample &sample) { return t < sample.time; });
		if (it != history.begin())
			--it;
		draw_texture(it->frame->texture(), size);
	}
};
void load()
{
	auto info = video_filter<Delay>("stagekit_port_dynamic_delay_filter", "Ports.DynamicDelay");
	info.filter_add = [](void *p, obs_source_t *parent) { static_cast<Delay *>(p)->bind(parent); };
	info.filter_remove = [](void *p, obs_source_t *) { static_cast<Delay *>(p)->unbind(); };
	obs_register_source(&info);
}
} // namespace
const Component component{load};
} // namespace stagekit::native
