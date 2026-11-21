/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "countdown.hpp"
#include "../common/timing.hpp"
#include "../common/tooltip.hpp"

#include <obs-module.h>
#include <util/platform.h>
#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <algorithm>
#include <cstring>
#include <mutex>

namespace stagekit {
namespace {

constexpr int64_t kSecond = 1000;

int64_t now_ms()
{
	return static_cast<int64_t>(os_gettime_ns() / 1000000);
}

const char *label(const char *key)
{
	return obs_module_text(key);
}

const char *default_face()
{
#ifdef _WIN32
	return "Consolas";
#elif defined(__APPLE__)
	return "Menlo";
#else
	return "DejaVu Sans Mono";
#endif
}

int64_t read_duration(obs_data_t *settings)
{
	const auto hours = std::clamp<long long>(obs_data_get_int(settings, "hours"), 0, 99);
	const auto minutes = std::clamp<long long>(obs_data_get_int(settings, "minutes"), 0, 59);
	const auto seconds = std::clamp<long long>(obs_data_get_int(settings, "seconds"), 0, 59);
	return (hours * 3600 + minutes * 60 + seconds) * kSecond;
}

QString time_text(int seconds, bool show_hours)
{
	auto two = [](int value) { return QString::number(value).rightJustified(2, QLatin1Char('0')); };
	QString result = two(seconds / 60 % 60) + QLatin1Char(':') + two(seconds % 60);
	if (show_hours)
		result.prepend(two(seconds / 3600) + QLatin1Char(':'));
	return result;
}

struct Display {
	QFont font;
	QColor color;
	bool show_hours = false;
	int digit_width = 0;
	int colon_width = 0;
	int height = 0;
	int padding = 0;

	int width() const { return (show_hours ? 6 : 4) * digit_width + (show_hours ? 2 : 1) * colon_width + 2 * padding; }

	void measure()
	{
		const QFontMetrics metrics(font);
		digit_width = 1;
		for (int i = 0; i != 10; ++i)
			digit_width = std::max(digit_width, metrics.horizontalAdvance(QChar('0' + i)));
		colon_width = std::max(1, metrics.horizontalAdvance(QLatin1Char(':')));
		padding = std::max(3, font.pixelSize() / 6);
		height = metrics.height() + 2 * padding;
	}

	QImage paint(int seconds) const
	{
		QImage frame(width(), height, QImage::Format_RGBA8888_Premultiplied);
		frame.fill(Qt::transparent);
		QPainter painter(&frame);
		painter.setRenderHint(QPainter::TextAntialiasing);
		painter.setFont(font);
		painter.setPen(color);
		int left = padding;
		for (const auto character : time_text(seconds, show_hours)) {
			const int advance = character == QLatin1Char(':') ? colon_width : digit_width;
			painter.drawText(QRect(left - padding, 0, advance + 2 * padding, height),
					 Qt::AlignCenter | Qt::TextSingleLine, QString(character));
			left += advance;
		}
		painter.end();
		return frame;
	}
};

class Countdown {
public:
	explicit Countdown(obs_source_t *source) : source_(source) {}
	obs_source_t *source() const { return source_; }

	~Countdown()
	{
		obs_enter_graphics();
		gs_texture_destroy(texture_);
		obs_leave_graphics();
	}

	void update(obs_data_t *settings)
	{
		obs_data_t *font_data = obs_data_get_obj(settings, "font");
		const auto size = int(std::clamp<long long>(obs_data_get_int(font_data, "size"), 8, 512));
		const auto flags = obs_data_get_int(font_data, "flags");
		QFont font(QString::fromUtf8(obs_data_get_string(font_data, "face")));
		obs_data_release(font_data);
		font.setPixelSize(size);
		font.setBold((flags & OBS_FONT_BOLD) != 0);
		font.setItalic((flags & OBS_FONT_ITALIC) != 0);
		font.setUnderline((flags & OBS_FONT_UNDERLINE) != 0);
		font.setStrikeOut((flags & OBS_FONT_STRIKEOUT) != 0);
		const auto rgba = static_cast<uint32_t>(obs_data_get_int(settings, "color"));
		const QColor color(rgba & 255, (rgba >> 8) & 255, (rgba >> 16) & 255, (rgba >> 24) & 255);
		const int64_t duration = read_duration(settings);
		std::lock_guard lock(mutex_);
		restart_when_active_ = obs_data_get_bool(settings, "restart_when_active");
		if (duration != duration_) {
			duration_ = duration;
			position_ = 0;
			started_ = now_ms();
			if (!duration_ || state_ != OBS_MEDIA_STATE_PLAYING)
				state_ = OBS_MEDIA_STATE_STOPPED;
		}
		const bool hours = duration >= 3600 * kSecond;
		if (display_.height == 0 || display_.show_hours != hours || display_.font != font || display_.color != color) {
			display_.font = font;
			display_.color = color;
			display_.show_hours = hours;
			display_.measure();
			drawn_seconds_ = -1;
		}
	}

	void play(bool restart)
	{
		{
			std::lock_guard lock(mutex_);
			if (duration_ <= 0 || (!restart && state_ == OBS_MEDIA_STATE_PLAYING))
				return;
			if (restart || state_ == OBS_MEDIA_STATE_ENDED)
				position_ = 0;
			started_ = now_ms();
			state_ = OBS_MEDIA_STATE_PLAYING;
		}
		obs_source_media_started(source_);
	}

	void pause()
	{
		{
			std::lock_guard lock(mutex_);
			if (state_ != OBS_MEDIA_STATE_PLAYING)
				return;
			position_ = elapsed_locked();
			state_ = position_ == duration_ ? OBS_MEDIA_STATE_ENDED : OBS_MEDIA_STATE_PAUSED;
			if (state_ != OBS_MEDIA_STATE_ENDED)
				return;
		}
		obs_source_media_ended(source_);
	}

	void reset()
	{
		std::lock_guard lock(mutex_);
		position_ = 0;
		state_ = OBS_MEDIA_STATE_STOPPED;
	}

	void activate()
	{
		bool restart;
		{
			std::lock_guard lock(mutex_);
			restart = restart_when_active_;
		}
		if (restart)
			play(true);
	}

	int64_t elapsed() const
	{
		std::lock_guard lock(mutex_);
		return elapsed_locked();
	}

	int64_t duration() const
	{
		std::lock_guard lock(mutex_);
		return duration_;
	}

	obs_media_state state() const
	{
		std::lock_guard lock(mutex_);
		return state_;
	}

	void seek(int64_t ms)
	{
		std::lock_guard lock(mutex_);
		position_ = std::clamp<int64_t>(ms, 0, duration_);
		started_ = now_ms();
		if (state_ == OBS_MEDIA_STATE_ENDED && position_ < duration_)
			state_ = OBS_MEDIA_STATE_PAUSED;
	}

	void tick()
	{
		{
			std::lock_guard lock(mutex_);
			if (state_ != OBS_MEDIA_STATE_PLAYING || elapsed_locked() < duration_)
				return;
			position_ = duration_;
			state_ = OBS_MEDIA_STATE_ENDED;
		}
		obs_source_media_ended(source_);
	}

	uint32_t width() const
	{
		std::lock_guard lock(mutex_);
		return static_cast<uint32_t>(display_.width());
	}

	uint32_t height() const
	{
		std::lock_guard lock(mutex_);
		return static_cast<uint32_t>(display_.height);
	}

	void render()
	{
		{
			std::lock_guard lock(mutex_);
			const int seconds = int((duration_ - elapsed_locked() + kSecond - 1) / kSecond);
			if (!texture_ || seconds != drawn_seconds_) {
				const QImage frame = display_.paint(seconds);
				const auto w = static_cast<uint32_t>(frame.width());
				const auto h = static_cast<uint32_t>(frame.height());
				if (frame.isNull())
					return;
				const uint8_t *pixels = frame.constBits();
				if (texture_ && gs_texture_get_width(texture_) == w && gs_texture_get_height(texture_) == h) {
					gs_texture_set_image(texture_, pixels, static_cast<uint32_t>(frame.bytesPerLine()), false);
				} else {
					gs_texture_destroy(texture_);
					texture_ = gs_texture_create(w, h, GS_RGBA, 1, &pixels, GS_DYNAMIC);
				}
				drawn_seconds_ = seconds;
			}
		}
		if (!texture_)
			return;
		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		const bool linear = gs_get_linear_srgb();
		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(linear);
		gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
		if (linear)
			gs_effect_set_texture_srgb(image, texture_);
		else
			gs_effect_set_texture(image, texture_);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(texture_, 0, 0, 0);
		gs_enable_framebuffer_srgb(previous);
	}

private:
	int64_t elapsed_locked() const
	{
		if (state_ != OBS_MEDIA_STATE_PLAYING)
			return position_;
		return std::min(duration_, position_ + now_ms() - started_);
	}

	obs_source_t *source_;
	mutable std::mutex mutex_;
	int64_t duration_ = 0;
	int64_t position_ = 0;
	int64_t started_ = 0;
	obs_media_state state_ = OBS_MEDIA_STATE_STOPPED;
	bool restart_when_active_ = false;
	Display display_;
	gs_texture_t *texture_ = nullptr;
	int drawn_seconds_ = -1;
};

struct Control {
	const char *id;
	const char *button;
	const char *hotkey;
	void (*apply)(obs_source_t *);
};

constexpr Control controls[] = {
	{"stagekit.countdown.start", "Countdown.Start", "Countdown.KeyStart",
	 [](obs_source_t *s) { obs_source_media_play_pause(s, false); }},
	{"stagekit.countdown.pause", "Countdown.Pause", "Countdown.KeyPause",
	 [](obs_source_t *s) { obs_source_media_play_pause(s, true); }},
	{"stagekit.countdown.reset", "Countdown.Reset", "Countdown.KeyReset", obs_source_media_stop},
};

void send_control(Countdown *timer, const char *id)
{
	if (!timer)
		return;
	for (const auto &control : controls)
		if (std::strcmp(control.id, id) == 0) {
			control.apply(timer->source());
			return;
		}
}

void register_keys(Countdown *timer)
{
	for (const auto &control : controls) {
		obs_hotkey_register_source(timer->source(), control.id, label(control.hotkey),
					   [](void *data, obs_hotkey_id, obs_hotkey_t *key, bool pressed) {
						   if (pressed)
							   send_control(static_cast<Countdown *>(data), obs_hotkey_get_name(key));
					   }, timer);
	}
}

obs_properties_t *properties(void *)
{
	auto *props = obs_properties_create();
	obs_properties_add_int(props, "hours", label("Countdown.Hours"), 0, 99, 1);
	obs_properties_add_int(props, "minutes", label("Countdown.Minutes"), 0, 59, 1);
	obs_properties_add_int(props, "seconds", label("Countdown.Seconds"), 0, 59, 1);
	set_tip(obs_properties_add_bool(props, "restart_when_active", label("Countdown.Auto")), label("Countdown.AutoTip"));
	obs_properties_add_font(props, "font", label("Countdown.Font"));
	obs_properties_add_color_alpha(props, "color", label("Countdown.Color"));
	for (const auto &control : controls) {
		obs_properties_add_button(props, control.id, label(control.button),
			[](obs_properties_t *, obs_property_t *p, void *data) {
				send_control(static_cast<Countdown *>(data), obs_property_name(p));
				return false;
			});
	}
	add_help_line(props, label("Countdown.Help"));
	return props;
}

void defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, "minutes", 5);
	obs_data_set_default_int(s, "color", 0xffffffffLL);
	obs_data_set_default_bool(s, "restart_when_active", false);
	auto *font = obs_data_create();
	obs_data_set_string(font, "face", default_face());
	obs_data_set_int(font, "size", 88);
	obs_data_set_int(font, "flags", OBS_FONT_BOLD);
	obs_data_set_default_obj(s, "font", font);
	obs_data_release(font);
}

} // namespace

void register_countdown()
{
	obs_source_info info{};
	info.id = "stagekit_countdown";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB | OBS_SOURCE_CONTROLLABLE_MEDIA;
	info.icon_type = OBS_ICON_TYPE_TEXT;
	info.get_name = [](void *) { return label("Countdown.Name"); };
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		auto *timer = new Countdown(source);
		timer->update(settings);
		register_keys(timer);
		return timer;
	};
	info.destroy = [](void *data) { delete static_cast<Countdown *>(data); };
	info.get_defaults = defaults;
	info.get_properties = properties;
	info.update = [](void *d, obs_data_t *s) { static_cast<Countdown *>(d)->update(s); };
	info.get_width = [](void *d) { return static_cast<Countdown *>(d)->width(); };
	info.get_height = [](void *d) { return static_cast<Countdown *>(d)->height(); };
	info.activate = [](void *d) { static_cast<Countdown *>(d)->activate(); };
	info.video_tick = [](void *d, float) { static_cast<Countdown *>(d)->tick(); };
	info.video_render = [](void *d, gs_effect_t *) { static_cast<Countdown *>(d)->render(); };
	info.media_play_pause = [](void *d, bool pause) {
		auto *timer = static_cast<Countdown *>(d);
		if (pause)
			timer->pause();
		else
			timer->play(false);
	};
	info.media_restart = [](void *d) { static_cast<Countdown *>(d)->play(true); };
	info.media_stop = [](void *d) { static_cast<Countdown *>(d)->reset(); };
	info.media_get_duration = [](void *d) { return static_cast<Countdown *>(d)->duration(); };
	info.media_get_time = [](void *d) { return static_cast<Countdown *>(d)->elapsed(); };
	info.media_get_state = [](void *d) { return static_cast<Countdown *>(d)->state(); };
	info.media_set_time = [](void *d, int64_t ms) { static_cast<Countdown *>(d)->seek(ms); };
	register_timed(&info);
}

} // namespace stagekit
