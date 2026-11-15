/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Capture App in Use: one Window Capture for several apps.
//
// The owner had a Window Capture for each app and a scene filter showing
// the one in use: "I'd like to have a single Window capture, and a new
// filter added that can have multiple apps associated with the Window
// capture and effectively emulate my current process and visibility
// functionality based on the open and captured apps, but with only a single
// window capture source."
//
// The filter sits on the Window Capture and aims it, by the rules of the
// scene filter it stands in for:
//
//  - An app on the list that opens is taken.
//  - With "Follow the app in use", clicking into one of the apps takes it.
//    Clicking into anything else changes nothing: the last one stays.
//  - When the app being captured closes, the one used before it comes back.
//
// It acts only when one of those things happens, so a capture aimed by hand
// stays until the next one; the Capture Front Window key switches the filter
// off, and switching it on again hands the capture back.
//
// One capture cannot show two windows at once, so the change between them is
// drawn here. The filter keeps the last picture the capture had; while the
// capture has none (it is being aimed at the other window, or its app has
// just closed) that picture stays, and the new one comes in over it in the
// way chosen. Standby Picture is told meanwhile (`app_capture_holding`), or
// it would stand in for that moment.
//
// Finding windows uses the Windows API (shared with Auto Capture). On other
// systems the filter loads and does nothing but pass the picture on.

#include "app-capture.hpp"
#include "auto-capture.hpp"
#include "../common/easing.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-util.hpp"
#include "../common/timing.hpp"
#include "../transition/morph-paint.hpp"

#include <obs-module.h>
#include <graphics/vec4.h>
#include <plugin-support.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace stagekit {

namespace {

// How the new picture comes in over the last one. Saved by number: new ones
// go at the end.
enum class Style : int {
	Fade = 0,
	Cut = 1,
	Zoom = 2, // fades in as it grows
	FromLeft = 3,
	FromRight = 4,
	FromTop = 5,
	FromBottom = 6,
};

// How long the last picture is kept when the capture loses its own with
// nothing said about another window yet (its app has closed: which one to
// go back to is known a moment later).
constexpr double kLostFor = 0.8;
// After being aimed, a capture goes on showing its old window for a moment:
// the new picture is believed once the capture has been seen with none, or
// after this long.
constexpr double kSettle = 0.6;
// And if no picture comes at all, the wait ends.
constexpr double kWaitFor = 3.0;

std::string lowered(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return text;
}

// One part of a capture's "window" setting, which OBS writes as
// title:class:executable with ':' as "#3A" and '#' as "#22".
std::string decoded(const std::string &part)
{
	std::string out;
	for (size_t i = 0; i < part.size(); i++) {
		if (part.compare(i, 3, "#3A") == 0) {
			out += ':';
			i += 2;
		} else if (part.compare(i, 3, "#22") == 0) {
			out += '#';
			i += 2;
		} else {
			out += part[i];
		}
	}
	return out;
}

// How well a window answers to an entry of the list: 2 by its program's
// file name, 1 by part of its title, 0 not at all.
int answers(const std::string &entry, const OpenWindow &w)
{
	const std::string e = lowered(entry);
	if (e.empty())
		return 0;
	if (lowered(w.executable) == e)
		return 2;
	return lowered(w.title).find(e) != std::string::npos ? 1 : 0;
}

// The size of what comes before a filter. For the capture itself that is its
// own size: asking for its width would come back to this filter.
uint32_t before_size(obs_source_t *filter, bool across)
{
	obs_source_t *target = obs_filter_get_target(filter), *parent = obs_filter_get_parent(filter);
	if (!target)
		return 0;
	if (target == parent)
		return across ? obs_source_get_base_width(target) : obs_source_get_base_height(target);
	return across ? obs_source_get_width(target) : obs_source_get_height(target);
}

class AppCapture : public MorphPaint {
public:
	AppCapture(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		obs_enter_graphics();
		last_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		next_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		obs_leave_graphics();
		update(settings);
		// For the tests: stand-in windows, one per line, "title|exe|front".
		proc_handler_add(
			obs_source_get_proc_handler(source_), "void set_test_windows(in string windows)",
			[](void *data, calldata_t *cd) {
				auto *self = static_cast<AppCapture *>(data);
				const char *text = calldata_string(cd, "windows");
				std::vector<OpenWindow> windows;
				std::vector<std::string> fields(1);
				auto flush = [&] {
					fields.resize(3);
					if (!fields[1].empty())
						windows.push_back({fields[0], fields[1], fields[2] == "front", ""});
					fields.assign(1, std::string());
				};
				for (const char *p = text ? text : ""; *p; p++) {
					if (*p == '\n')
						flush();
					else if (*p == '|')
						fields.emplace_back();
					else
						fields.back() += *p;
				}
				flush();
				std::lock_guard lock(self->mutex_);
				self->test_windows_ = windows;
				self->use_test_windows_ = true;
				self->timer_ = 0.0;
			},
			this);
	}

	~AppCapture()
	{
		obs_enter_graphics();
		gs_texrender_destroy(last_);
		gs_texrender_destroy(next_);
		obs_leave_graphics();
	}

	obs_source_t *source() const { return source_; }

	void update(obs_data_t *settings)
	{
		const std::vector<std::string> apps = pick_list_read(settings, "apps", false);
		std::lock_guard lock(mutex_);
		apps_ = apps;
		follow_ = obs_data_get_bool(settings, "follow");
		style_ = static_cast<Style>(std::clamp<long long>(obs_data_get_int(settings, "change"), 0, 6));
		change_s_ = std::clamp<long long>(obs_data_get_int(settings, "change_ms"), 0, 10000) / 1000.0;
		curve_ = static_cast<EaseCurve>(obs_data_get_int(settings, "ease_curve"));
		mode_ = static_cast<EaseMode>(obs_data_get_int(settings, "ease_mode"));
		started_ = false; // settings changed: work out what to capture afresh
		timer_ = 0.0;
	}

	std::string status()
	{
		std::lock_guard lock(mutex_);
		return current_;
	}

	bool holding()
	{
		std::lock_guard lock(mutex_);
		return phase_ == Phase::Lost || phase_ == Phase::Wait;
	}

	uint32_t size(bool across)
	{
		const uint32_t before = before_size(source_, across);
		std::lock_guard lock(mutex_);
		const uint32_t kept = across ? kept_w_ : kept_h_;
		if (phase_ == Phase::Lost || phase_ == Phase::Wait)
			return kept ? kept : before;
		if (phase_ == Phase::Mix)
			return before ? before : kept;
		return before;
	}

	void tick(float seconds);
	void render();

private:
	void follow(obs_source_t *parent);
	void aim(obs_source_t *parent, const OpenWindow &window, const std::string &entry, bool by_title);
	bool copy_before(gs_texrender_t *into, uint32_t w, uint32_t h);
	void put(gs_texture_t *texture, double x, double y, double w, double h, float strength);

	enum class Phase {
		Idle, // the capture's picture as it is
		Lost, // the capture has none all of a sudden: the last one stays for a moment
		Wait, // aimed at another window: the last one stays until the new one is there
		Mix,  // the new one comes in over the last
	};

	obs_source_t *source_;
	std::mutex mutex_;
	// Settings.
	std::vector<std::string> apps_;
	bool follow_ = true;
	Style style_ = Style::Fade;
	double change_s_ = 0.3;
	EaseCurve curve_ = EaseCurve::Cubic;
	EaseMode mode_ = EaseMode::InOut;
	// Following the apps (the tick, under the lock where the settings window reads it).
	double timer_ = 0.0;
	bool started_ = false;
	std::string current_;             // the entry being captured, as far as this filter knows
	std::vector<std::string> recent_; // entries, the one used last first
	std::vector<std::string> was_open_;
	std::vector<OpenWindow> test_windows_;
	bool use_test_windows_ = false;
	bool placed_checked_ = false;
	// The change between two windows.
	Phase phase_ = Phase::Idle;
	double phase_s_ = 0.0;
	double mix_ = 0.0;
	bool seen_empty_ = false;
	uint32_t kept_w_ = 0, kept_h_ = 0; // the size of the picture in `last_`
	double drawn_ago_ = 10.0;          // seconds since this filter last drew
	gs_texrender_t *last_ = nullptr;   // the capture's last picture
	gs_texrender_t *next_ = nullptr;   // the new one, while it comes in
};

void AppCapture::aim(obs_source_t *parent, const OpenWindow &window, const std::string &entry, bool by_title)
{
	obs_log(LOG_INFO, "Capture App in Use: '%s' now captures %s (%s)", obs_source_get_name(parent),
		window.executable.c_str(), entry.c_str());
	aim_capture_at(parent, window, by_title);
	std::lock_guard lock(mutex_);
	current_ = entry;
	recent_.erase(std::remove(recent_.begin(), recent_.end(), entry), recent_.end());
	recent_.insert(recent_.begin(), entry);
	// The last picture stays until the new window's is there, if this filter
	// is being drawn and has one.
	if (kept_w_ && kept_h_ && drawn_ago_ < 0.5 && phase_ != Phase::Mix) {
		phase_ = Phase::Wait;
		phase_s_ = 0.0;
		seen_empty_ = false;
	}
}

// Which app to capture, looked at five times a second.
void AppCapture::follow(obs_source_t *parent)
{
	std::vector<std::string> apps, recent, was_open;
	std::vector<OpenWindow> windows;
	std::string current;
	bool follow, use_test, first;
	{
		std::lock_guard lock(mutex_);
		apps = apps_;
		follow = follow_;
		use_test = use_test_windows_;
		if (use_test)
			windows = test_windows_;
		first = !started_;
		started_ = true;
		current = current_;
		recent = recent_;
		was_open = was_open_;
	}
#ifndef _WIN32
	// Nothing to go by on other systems.
	if (!use_test)
		return;
#endif
	if (apps.empty())
		return;
	if (!use_test)
		windows = open_windows();

	// For each entry: the window to take (the one in front if it is one of
	// its windows, else the first), and whether one of them is in front.
	struct Slot {
		const OpenWindow *window = nullptr;
		int how = 0;
		bool front = false;
	};
	std::vector<Slot> slots(apps.size());
	for (size_t i = 0; i < apps.size(); i++) {
		for (const OpenWindow &w : windows) {
			const int how = answers(apps[i], w);
			if (!how)
				continue;
			Slot &slot = slots[i];
			if (!slot.window || (w.foreground && !slot.front) || (how > slot.how && !slot.front)) {
				slot.window = &w;
				slot.how = how;
			}
			slot.front = slot.front || w.foreground;
		}
	}
	auto index_of = [&](const std::string &entry) {
		for (size_t i = 0; i < apps.size(); i++)
			if (apps[i] == entry)
				return (int)i;
		return -1;
	};
	// The window in front may answer to two entries (a program, and a title
	// of that program's): the title, which says more, comes first.
	int front = -1;
	for (size_t i = 0; i < apps.size(); i++)
		if (slots[i].front && (front < 0 || slots[i].how < slots[(size_t)front].how))
			front = (int)i;
	// What to go back to: the one used most recently that is open, else the
	// first that is open.
	auto fallback = [&]() {
		for (const std::string &entry : recent) {
			const int index = index_of(entry);
			if (index >= 0 && slots[(size_t)index].window)
				return index;
		}
		for (size_t i = 0; i < slots.size(); i++)
			if (slots[i].window)
				return (int)i;
		return -1;
	};

	int now = index_of(current);
	if (first && now < 0) {
		// What the capture is aimed at already, if that is one of the apps.
		obs_data_t *settings = obs_source_get_settings(parent);
		const std::string window = obs_data_get_string(settings, "window");
		obs_data_release(settings);
		const size_t colon = window.rfind(':');
		OpenWindow aimed;
		aimed.title = decoded(window.substr(0, window.find(':')));
		aimed.executable = colon == std::string::npos ? std::string() : decoded(window.substr(colon + 1));
		int best = 0;
		for (size_t i = 0; i < apps.size() && !aimed.executable.empty(); i++) {
			const int how = slots[i].window ? answers(apps[i], aimed) : 0;
			if (how && (!best || how < best)) {
				now = (int)i;
				best = how;
			}
		}
		if (now >= 0) {
			std::lock_guard lock(mutex_);
			current_ = current = apps[(size_t)now];
		}
	}
	int target = -1;
	if (first) {
		if (now < 0)
			target = front >= 0 ? front : fallback();
	} else if (follow && front >= 0 && front != now) {
		target = front;
	} else {
		// One that has just opened.
		for (size_t i = 0; i < apps.size(); i++)
			if (slots[i].window && (int)i != now &&
			    std::find(was_open.begin(), was_open.end(), apps[i]) == was_open.end())
				target = (int)i;
	}
	// The one being captured has closed: back to the one before it.
	if (target < 0 && now >= 0 && !slots[(size_t)now].window) {
		target = fallback();
		if (target < 0) {
			std::lock_guard lock(mutex_);
			current_.clear();
		}
	}
	std::vector<std::string> open_now;
	for (size_t i = 0; i < apps.size(); i++)
		if (slots[i].window)
			open_now.push_back(apps[i]);
	{
		std::lock_guard lock(mutex_);
		was_open_ = open_now;
	}
	if (target >= 0 && target != now && slots[(size_t)target].window)
		aim(parent, *slots[(size_t)target].window, apps[(size_t)target], slots[(size_t)target].how == 1);
}

void AppCapture::tick(float seconds)
{
	obs_source_t *parent = obs_filter_get_parent(source_);
	if (!parent)
		return;
	// It belongs first in the list, before whatever frames, shapes or stands
	// in for the picture: moved there once, when it is new.
	if (!placed_checked_) {
		placed_checked_ = true;
		obs_data_t *settings = obs_source_get_settings(source_);
		if (!obs_data_get_bool(settings, "placed")) {
			obs_data_set_bool(settings, "placed", true);
			obs_source_filter_set_order(parent, source_, OBS_ORDER_MOVE_TOP);
		}
		obs_data_release(settings);
	}
	const bool on = obs_source_enabled(source_);
	const bool has = before_size(source_, true) && before_size(source_, false);
	bool look = false;
	double change_s;
	{
		std::lock_guard lock(mutex_);
		drawn_ago_ += seconds;
		change_s = style_ == Style::Cut ? 0.0 : change_s_;
		if (!on) {
			phase_ = Phase::Idle;
			started_ = false;
			return;
		}
		phase_s_ += seconds;
		switch (phase_) {
		case Phase::Idle:
			break;
		case Phase::Lost:
			if (has) {
				// It is back by itself.
				phase_ = Phase::Mix;
				mix_ = 0.0;
			} else if (phase_s_ >= kLostFor) {
				phase_ = Phase::Idle;
				kept_w_ = kept_h_ = 0;
			}
			break;
		case Phase::Wait:
			seen_empty_ = seen_empty_ || !has;
			if (has && (seen_empty_ || phase_s_ >= kSettle)) {
				phase_ = Phase::Mix;
				mix_ = 0.0;
			} else if (phase_s_ >= kWaitFor) {
				phase_ = Phase::Idle;
				kept_w_ = kept_h_ = 0;
			}
			break;
		case Phase::Mix:
			mix_ = change_s > 0.0 ? std::min(1.0, mix_ + seconds / change_s) : 1.0;
			if (mix_ >= 1.0) {
				// The new picture is the last one from here.
				std::swap(last_, next_);
				kept_w_ = before_size(source_, true);
				kept_h_ = before_size(source_, false);
				phase_ = Phase::Idle;
			}
			break;
		}
		timer_ -= seconds;
		if (timer_ <= 0.0) {
			timer_ = 0.2;
			look = true;
		}
	}
	if (look)
		follow(parent);
}

// What comes before this filter, into a picture of its own, as it is.
bool AppCapture::copy_before(gs_texrender_t *into, uint32_t w, uint32_t h)
{
	gs_texrender_reset(into);
	const bool was_linear = gs_set_linear_srgb(false);
	const bool was_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	bool drawn = false;
	if (gs_texrender_begin(into, w, h)) {
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		obs_source_skip_video_filter(source_);
		gs_blend_state_pop();
		gs_texrender_end(into);
		drawn = true;
	}
	gs_enable_framebuffer_srgb(was_srgb);
	gs_set_linear_srgb(was_linear);
	return drawn;
}

void AppCapture::put(gs_texture_t *texture, double x, double y, double w, double h, float strength)
{
	if (!texture || !effect_ || strength <= 0.0f)
		return;
	const bool linear = gs_get_linear_srgb();
	const bool was_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(linear);
	if (linear)
		gs_effect_set_texture_srgb(p_image_, texture);
	else
		gs_effect_set_texture(p_image_, texture);
	gs_effect_set_float(p_opacity_, strength);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_matrix_push();
	set_quad(Quad{w, 0.0, 0.0, h, x, y});
	while (gs_effect_loop(effect_, "Draw"))
		gs_draw_sprite(nullptr, 0, 1, 1);
	gs_matrix_pop();
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(was_srgb);
}

void AppCapture::render()
{
	const uint32_t bw = before_size(source_, true), bh = before_size(source_, false);
	const bool has = bw && bh;
	Phase phase;
	Style style;
	double mix;
	uint32_t kw, kh;
	EaseCurve curve;
	EaseMode mode;
	{
		std::lock_guard lock(mutex_);
		drawn_ago_ = 0.0;
		// The capture has lost its picture all of a sudden: the last one
		// stays while it is found out what to capture next.
		if (phase_ == Phase::Idle && !has && kept_w_ && kept_h_) {
			phase_ = Phase::Lost;
			phase_s_ = 0.0;
		}
		phase = phase_;
		style = style_;
		mix = mix_;
		kw = kept_w_;
		kh = kept_h_;
		curve = curve_;
		mode = mode_;
	}
	if (!effect_ || !obs_source_enabled(source_)) {
		obs_source_skip_video_filter(source_);
		return;
	}
	if (phase == Phase::Idle) {
		if (!has)
			return;
		if (!copy_before(last_, bw, bh)) {
			obs_source_skip_video_filter(source_);
			return;
		}
		{
			std::lock_guard lock(mutex_);
			kept_w_ = bw;
			kept_h_ = bh;
		}
		put(gs_texrender_get_texture(last_), 0.0, 0.0, bw, bh, 1.0f);
		return;
	}
	gs_texture_t *last = gs_texrender_get_texture(last_);
	if (phase != Phase::Mix || !has) {
		put(last, 0.0, 0.0, kw, kh, 1.0f);
		return;
	}
	// The new picture over the last, which is fitted inside the new size.
	if (!copy_before(next_, bw, bh)) {
		put(last, 0.0, 0.0, kw, kh, 1.0f);
		return;
	}
	const double t = std::clamp(ease(curve, mode, mix), 0.0, 1.0);
	if (kw && kh) {
		const double k = std::min((double)bw / kw, (double)bh / kh);
		put(last, (bw - kw * k) / 2.0, (bh - kh * k) / 2.0, kw * k, kh * k, 1.0f);
	}
	gs_texture_t *next = gs_texrender_get_texture(next_);
	switch (style) {
	case Style::Zoom: {
		const double k = 0.85 + 0.15 * t;
		put(next, bw * (1.0 - k) / 2.0, bh * (1.0 - k) / 2.0, bw * k, bh * k, (float)t);
		break;
	}
	case Style::FromLeft:
		put(next, -(double)bw * (1.0 - t), 0.0, bw, bh, 1.0f);
		break;
	case Style::FromRight:
		put(next, (double)bw * (1.0 - t), 0.0, bw, bh, 1.0f);
		break;
	case Style::FromTop:
		put(next, 0.0, -(double)bh * (1.0 - t), bw, bh, 1.0f);
		break;
	case Style::FromBottom:
		put(next, 0.0, (double)bh * (1.0 - t), bw, bh, 1.0f);
		break;
	default:
		put(next, 0.0, 0.0, bw, bh, (float)t);
		break;
	}
}

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<AppCapture *>(data);
	obs_properties_t *props = obs_properties_create();
	const std::string now = self ? self->status() : std::string();
	std::string status = obs_module_text(now.empty() ? "AppCapture.Status.None" : "AppCapture.Status.Now");
	const size_t at = status.find("%1");
	if (at != std::string::npos)
		status.replace(at, 2, now);
	obs_properties_add_text(props, "status", status.c_str(), OBS_TEXT_INFO);

	PickList apps;
	apps.setting = "apps";
	apps.title = "AppCapture.Apps";
	apps.add = "AppCapture.Apps.Add";
	apps.tip = "AppCapture.Apps.Tip";
	apps.choices = [](std::vector<std::string> &names) {
		for (const std::string &name : open_program_names())
			if (std::find(names.begin(), names.end(), name) == names.end())
				names.push_back(name);
		// And the open windows by title, for telling apart two programs
		// with the same file name.
		for (const OpenWindow &w : open_windows())
			if (!w.title.empty() && std::find(names.begin(), names.end(), w.title) == names.end())
				names.push_back(w.title);
	};
	pick_list_add(props, self ? self->source() : nullptr, apps);

	obs_property_t *follow = obs_properties_add_bool(props, "follow", obs_module_text("AppCapture.Follow"));
	obs_property_set_long_description(follow, obs_module_text("AppCapture.Follow.Tip"));

	obs_property_t *change = obs_properties_add_list(props, "change", obs_module_text("AppCapture.Change"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(change, obs_module_text("AppCapture.Change.Fade"), (int)Style::Fade);
	obs_property_list_add_int(change, obs_module_text("AppCapture.Change.Cut"), (int)Style::Cut);
	obs_property_list_add_int(change, obs_module_text("AppCapture.Change.Zoom"), (int)Style::Zoom);
	obs_property_list_add_int(change, obs_module_text("AppCapture.Change.FromLeft"), (int)Style::FromLeft);
	obs_property_list_add_int(change, obs_module_text("AppCapture.Change.FromRight"), (int)Style::FromRight);
	obs_property_list_add_int(change, obs_module_text("AppCapture.Change.FromTop"), (int)Style::FromTop);
	obs_property_list_add_int(change, obs_module_text("AppCapture.Change.FromBottom"), (int)Style::FromBottom);
	obs_property_set_long_description(change, obs_module_text("AppCapture.Change.Tip"));
	obs_property_set_modified_callback(change, [](obs_properties_t *p, obs_property_t *, obs_data_t *settings) {
		const bool timed = obs_data_get_int(settings, "change") != (int)Style::Cut;
		for (const char *name : {"change_ms", "ease_curve", "ease_mode"})
			if (obs_property_t *one = obs_properties_get(p, name))
				obs_property_set_visible(one, timed);
		return true;
	});
	obs_property_t *time =
		obs_properties_add_int_slider(props, "change_ms", obs_module_text("Change.Time"), 50, 5000, 50);
	obs_property_int_set_suffix(time, " ms");
	add_easing_properties(props, "");
	return props;
}

void get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "follow", true);
	obs_data_set_default_int(settings, "change", (int)Style::Fade);
	obs_data_set_default_int(settings, "change_ms", 300);
	obs_data_set_default_int(settings, "ease_curve", (int)EaseCurve::Cubic);
	obs_data_set_default_int(settings, "ease_mode", (int)EaseMode::InOut);
}

} // namespace

bool app_capture_holding(obs_source_t *capture)
{
	bool holding = false;
	obs_source_enum_filters(
		capture,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			const char *id = obs_source_get_unversioned_id(filter);
			if (!id || strcmp(id, kAppCaptureId) != 0 || !obs_source_enabled(filter))
				return;
			auto *self = static_cast<AppCapture *>(obs_obj_get_data(filter));
			if (self && self->holding())
				*static_cast<bool *>(param) = true;
		},
		&holding);
	return holding;
}

void register_app_capture()
{
	obs_source_info info = {};
	info.id = kAppCaptureId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = [](void *) {
		return obs_module_text("AppCapture.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new AppCapture(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<AppCapture *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<AppCapture *>(data)->update(settings);
	};
	info.video_tick = [](void *data, float seconds) {
		static_cast<AppCapture *>(data)->tick(seconds);
	};
	info.video_render = [](void *data, gs_effect_t *) {
		static_cast<AppCapture *>(data)->render();
	};
	info.get_width = [](void *data) {
		return static_cast<AppCapture *>(data)->size(true);
	};
	info.get_height = [](void *data) {
		return static_cast<AppCapture *>(data)->size(false);
	};
	info.get_properties = get_properties;
	info.get_defaults = get_defaults;
	register_timed(&info);
}

} // namespace stagekit
