/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "countdown.hpp"
#include "../common/timing.hpp"
#include "../common/tooltip.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.hpp>
#include <util/platform.h>
#include <graphics/matrix4.h>
#include <QApplication>
#include <QColor>
#include <QDateTime>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cmath>
#include <memory>
#include <mutex>

namespace stagekit {
namespace {

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

int64_t monotonic_ms()
{
	return static_cast<int64_t>(os_gettime_ns() / 1000000);
}

// Relative timers use monotonic time. Only a calendar timer follows the wall clock.
struct Playback {
	TimerOptions settings;
	obs_media_state state = OBS_MEDIA_STATE_STOPPED;
	int64_t span = 300000;
	int64_t position = 0;
	int64_t started = 0;

	int64_t until_deadline() const
	{
		return std::max<int64_t>(0, settings.deadline - QDateTime::currentMSecsSinceEpoch());
	}

	void reset()
	{
		state = OBS_MEDIA_STATE_STOPPED;
		position = 0;
		span = settings.mode == TimerMode::Deadline ? until_deadline()
							    : (settings.unlimited() ? 0 : settings.duration);
	}

	int64_t elapsed() const
	{
		if (state != OBS_MEDIA_STATE_PLAYING)
			return position;
		if (settings.mode == TimerMode::Deadline)
			return std::clamp<int64_t>(span - until_deadline(), 0, span);
		if (settings.steady)
			return position;
		const int64_t value = position + monotonic_ms() - started;
		return settings.unlimited() ? value : std::min(span, value);
	}

	int64_t value() const
	{
		if (settings.counts_up())
			return elapsed();
		if (settings.mode == TimerMode::Deadline && state == OBS_MEDIA_STATE_PLAYING)
			return until_deadline();
		return std::max<int64_t>(0, span - elapsed());
	}

	bool start(bool restart)
	{
		if (!restart && state == OBS_MEDIA_STATE_PLAYING)
			return false;
		if (restart || state == OBS_MEDIA_STATE_ENDED)
			reset();
		if (settings.mode == TimerMode::Deadline) {
			const int64_t remaining = until_deadline();
			if (!remaining && state != OBS_MEDIA_STATE_PAUSED) {
				position = span;
				state = OBS_MEDIA_STATE_ENDED;
				return false;
			}
			span = settings.date_up && state == OBS_MEDIA_STATE_STOPPED ? remaining
										    : std::max(span, remaining);
		} else if (!settings.unlimited() && !span) {
			return false;
		}
		started = monotonic_ms();
		state = OBS_MEDIA_STATE_PLAYING;
		return true;
	}

	// A long OBS stall advances a period timer by at most one second.
	void advance_steady()
	{
		if (state != OBS_MEDIA_STATE_PLAYING || !settings.steady || settings.mode == TimerMode::Deadline)
			return;
		const auto now = monotonic_ms();
		position += std::clamp<int64_t>(now - started, 0, 1000);
		if (!settings.unlimited())
			position = std::min(span, position);
		started = now;
	}

	bool finished() const
	{
		if (state != OBS_MEDIA_STATE_PLAYING || settings.unlimited())
			return false;
		return settings.mode == TimerMode::Deadline ? until_deadline() == 0 : elapsed() >= span;
	}

	bool message_visible() const
	{
		return state == OBS_MEDIA_STATE_ENDED && settings.use_message && !settings.message.isEmpty();
	}

	QString text() const { return settings.display_text(value(), span, message_visible()); }
};

struct Display {
	QFont font;
	QColor color;
	int digit = 1, colon = 1, padding = 4, height = 1;

	void measure()
	{
		const QFontMetrics m(font);
		digit = 1;
		for (int i = 0; i < 10; ++i)
			digit = std::max(digit, m.horizontalAdvance(QChar('0' + i)));
		colon = std::max(1, m.horizontalAdvance(QLatin1Char(':')));
		padding = std::max(3, font.pixelSize() / 6);
		height = m.height() + 2 * padding;
	}

	int time_width(const QString &s) const
	{
		const int separators = int(s.count(QLatin1Char(':')));
		return digit * (int(s.size()) - separators) + colon * separators;
	}

	int line_width(const QString &line) const
	{
		const QFontMetrics metrics(font);
		int reserve = std::max(metrics.horizontalAdvance(line), metrics.boundingRect(line).width());
		for (const auto c : line)
			if (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
				reserve += digit - metrics.horizontalAdvance(c);
		return reserve;
	}

	QImage paint(const QString &text, bool free_text, int width, int density) const
	{
		QImage frame(width * density, height * density, QImage::Format_RGBA8888_Premultiplied);
		if (frame.isNull())
			return frame;
		frame.setDevicePixelRatio(density);
		frame.fill(Qt::transparent);
		QPainter p(&frame);
		p.setRenderHint(QPainter::TextAntialiasing);
		p.setFont(font);
		p.setPen(color);
		if (free_text) {
			const int advance = std::max(1, QFontMetrics(font).horizontalAdvance(text));
			const qreal scale = std::min<qreal>(1, qreal(width - 2 * padding) / advance);
			p.translate(width / 2.0, height / 2.0);
			p.scale(scale, scale);
			p.drawText(QRectF(-advance / 2.0 - padding, -height / 2.0, advance + 2 * padding, height),
				   Qt::AlignCenter | Qt::TextSingleLine, text);
		} else {
			int left = (width - time_width(text)) / 2;
			for (const auto c : text) {
				const int step = c == QLatin1Char(':') ? colon : digit;
				p.drawText(QRect(left - padding, 0, step + 2 * padding, height),
					   Qt::AlignCenter | Qt::TextSingleLine, QString(c));
				left += step;
			}
		}
		p.end();
		return frame;
	}
};

class Countdown {
public:
	explicit Countdown(obs_source_t *s) : source_(s) {}
	obs_source_t *source() const { return source_; }

	~Countdown()
	{
		++*generation_;
		obs_enter_graphics();
		gs_texture_destroy(texture_);
		obs_leave_graphics();
	}

	void update(obs_data_t *s)
	{
		OBSDataAutoRelease data = obs_data_get_obj(s, "font");
		QFont font(QString::fromUtf8(obs_data_get_string(data, "face")));
		const auto flags = obs_data_get_int(data, "flags");
		font.setPixelSize(int(std::clamp<long long>(obs_data_get_int(data, "size"), 8, 512)));
		font.setBold((flags & OBS_FONT_BOLD) != 0);
		font.setItalic((flags & OBS_FONT_ITALIC) != 0);
		font.setUnderline((flags & OBS_FONT_UNDERLINE) != 0);
		font.setStrikeOut((flags & OBS_FONT_STRIKEOUT) != 0);
		const auto rgba = static_cast<uint32_t>(obs_data_get_int(s, "color"));
		const QColor color(rgba & 255, (rgba >> 8) & 255, (rgba >> 16) & 255, (rgba >> 24) & 255);
		const auto next = TimerOptions::read(s);
		std::lock_guard lock(mutex_);
		const bool reset = !initialized_ || !clock_.settings.same_timing(next);
		if (reset || next.scene != clock_.settings.scene || next.switch_scene != clock_.settings.switch_scene)
			++*generation_;
		if (!reset && next.steady != clock_.settings.steady && next.mode != TimerMode::Deadline &&
		    clock_.state == OBS_MEDIA_STATE_PLAYING) {
			clock_.advance_steady();
			clock_.position = clock_.elapsed();
			clock_.started = monotonic_ms();
		}
		clock_.settings = next;
		if (reset)
			clock_.reset();
		display_.font = font;
		display_.color = color;
		display_.measure();
		initialized_ = true;
		dirty_ = true;
		update_width_locked();
	}

	void start(bool restart)
	{
		{
			std::lock_guard lock(mutex_);
			if (!clock_.start(restart))
				return;
			++*generation_;
			update_width_locked();
		}
		obs_source_media_started(source_);
	}

	void pause()
	{
		// Latch a deadline already crossed before accepting the pause.
		tick();
		std::lock_guard lock(mutex_);
		if (clock_.state == OBS_MEDIA_STATE_PLAYING) {
			clock_.position = clock_.elapsed();
			clock_.state = OBS_MEDIA_STATE_PAUSED;
		}
	}

	void reset()
	{
		std::lock_guard lock(mutex_);
		++*generation_;
		clock_.reset();
		update_width_locked();
	}

	void activate()
	{
		bool run;
		{
			std::lock_guard lock(mutex_);
			run = clock_.settings.on_program;
		}
		if (run)
			start(true);
	}

	void tick()
	{
		QString scene;
		uint64_t run;
		{
			std::lock_guard lock(mutex_);

			clock_.advance_steady();
			if (!clock_.finished())
				return;
			clock_.position = clock_.span;
			clock_.state = OBS_MEDIA_STATE_ENDED;
			if (clock_.settings.switch_scene)
				scene = clock_.settings.scene;
			run = generation_->load();
		}
		obs_source_media_ended(source_);
		if (!scene.isEmpty() && qApp) {
			const OBSWeakSource weak = OBSGetWeakRef(source_);
			QMetaObject::invokeMethod(
				qApp,
				[weak, scene, token = generation_, run] {
					if (token->load() != run || !obs_frontend_get_main_window())
						return;
					const auto timer = OBSGetStrongRef(weak);
					if (!timer || obs_source_removed(timer))
						return;
					OBSSourceAutoRelease target =
						obs_get_source_by_uuid(scene.toUtf8().constData());
					OBSSourceAutoRelease current = obs_frontend_get_current_scene();
					if (target && target.Get() != current.Get() && !obs_source_removed(target) &&
					    obs_scene_from_source(target) && !obs_source_is_group(target))
						obs_frontend_set_current_scene(target);
				},
				Qt::QueuedConnection);
		}
	}

	int64_t elapsed() const
	{
		std::lock_guard lock(mutex_);
		return clock_.elapsed();
	}
	int64_t duration() const
	{
		std::lock_guard lock(mutex_);
		return clock_.span;
	}
	obs_media_state state() const
	{
		std::lock_guard lock(mutex_);
		return clock_.state;
	}
	QString text(bool &end_message) const
	{
		std::lock_guard lock(mutex_);
		end_message = clock_.message_visible();
		return clock_.text();
	}
	uint32_t width() const
	{
		std::lock_guard lock(mutex_);
		return width_;
	}
	uint32_t height() const
	{
		std::lock_guard lock(mutex_);
		return static_cast<uint32_t>(display_.height);
	}

	void seek(int64_t ms)
	{
		std::lock_guard lock(mutex_);
		if (clock_.settings.mode == TimerMode::Deadline)
			return;
		clock_.position = clock_.settings.unlimited() ? std::max<int64_t>(0, ms)
							      : std::clamp<int64_t>(ms, 0, clock_.span);
		clock_.started = monotonic_ms();
		if (clock_.state == OBS_MEDIA_STATE_ENDED && clock_.position < clock_.span) {
			++*generation_;
			clock_.state = OBS_MEDIA_STATE_PAUSED;
		}
		update_width_locked();
	}

	void render()
	{
		matrix4 transform;
		gs_matrix_get(&transform);
		const float scale =
			std::max(std::hypot(transform.x.x, transform.x.y), std::hypot(transform.y.x, transform.y.y));
		uint32_t logical_width, logical_height;
		{
			std::lock_guard lock(mutex_);
			const bool down = !clock_.settings.counts_up();
			const int64_t second = (clock_.value() + (down ? 999 : 0)) / 1000;
			const bool msg = clock_.message_visible();
			const bool changed = !texture_ || second != last_second_ || msg != last_message_ || dirty_;
			if (changed)
				update_width_locked();
			logical_width = width_;
			logical_height = static_cast<uint32_t>(display_.height);
			int wanted = std::isfinite(scale) ? int(std::ceil(std::clamp(scale, 1.0f, 8.0f))) : 1;
			const uint64_t now = obs_get_video_frame_time();
			if (wanted >= density_)
				last_large_draw_ = now;
			else if (now >= last_large_draw_ && now - last_large_draw_ < 1000000000ULL)
				wanted =
					density_; // Program and smaller previews can draw the same source in one frame.
			while (wanted > 1 &&
			       (logical_width * wanted > 8192 || logical_height * wanted > 8192 ||
				uint64_t(logical_width) * logical_height * wanted * wanted > 4 * 1024 * 1024))
				--wanted;
			if (changed || wanted != density_) {
				const auto &options = clock_.settings;
				const QString value = options.display_text(second * 1000, clock_.span, msg);
				const QImage frame =
					display_.paint(value, msg || options.format_output, int(logical_width), wanted);
				if (frame.isNull())
					return;
				const auto w = static_cast<uint32_t>(frame.width());
				const auto h = static_cast<uint32_t>(frame.height());
				const uint8_t *pixels = frame.constBits();
				if (texture_ && gs_texture_get_width(texture_) == w &&
				    gs_texture_get_height(texture_) == h)
					gs_texture_set_image(texture_, pixels,
							     static_cast<uint32_t>(frame.bytesPerLine()), false);
				else {
					auto *next = gs_texture_create(w, h, GS_RGBA, 1, &pixels, GS_DYNAMIC);
					if (!next)
						return;
					gs_texture_destroy(texture_);
					texture_ = next;
				}
				density_ = wanted;
				last_second_ = second;
				last_message_ = msg;
				dirty_ = false;
			}
		}
		if (!texture_)
			return;
		auto *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		auto *image = gs_effect_get_param_by_name(effect, "image");
		const bool linear = gs_get_linear_srgb();
		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(linear);
		if (linear)
			gs_effect_set_texture_srgb(image, texture_);
		else
			gs_effect_set_texture(image, texture_);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(texture_, 0, logical_width, logical_height);
		gs_enable_framebuffer_srgb(previous);
	}

private:
	void update_width_locked()
	{
		const int64_t range = std::max(clock_.span, clock_.value());
		const auto &options = clock_.settings;
		const QString maximum = options.display_text(range, range, false, true);
		int width = options.format_output ? display_.line_width(maximum) : display_.time_width(maximum);
		if (options.use_message && !options.message.isEmpty()) {
			const QString end = options.display_text(options.counts_up() ? range : 0, range, true, true);
			width = std::max(width, display_.line_width(end));
		}
		width_ = static_cast<uint32_t>(std::min(4096, width + 2 * display_.padding));
	}
	obs_source_t *source_;
	mutable std::mutex mutex_;
	Playback clock_;
	Display display_;
	gs_texture_t *texture_ = nullptr;
	uint32_t width_ = 1;
	int64_t last_second_ = -1;
	int density_ = 1;
	uint64_t last_large_draw_ = 0;
	bool last_message_ = false, dirty_ = true, initialized_ = false;
	std::shared_ptr<std::atomic<uint64_t>> generation_ = std::make_shared<std::atomic<uint64_t>>(0);
};

struct Control {
	const char *id, *button, *hotkey;
	void (*apply)(obs_source_t *);
};

constexpr Control controls[] = {
	{"stagekit.countdown.start", "Countdown.Start", "Countdown.KeyStart",
	 [](obs_source_t *s) {
		 obs_source_media_play_pause(s, false);
	 }},
	{"stagekit.countdown.pause", "Countdown.Pause", "Countdown.KeyPause",
	 [](obs_source_t *s) {
		 obs_source_media_play_pause(s, true);
	 }},
	{"stagekit.countdown.reset", "Countdown.Reset", "Countdown.KeyReset", obs_source_media_stop},
};

void send_control(Countdown *timer, const char *id)
{
	for (const auto &control : controls)
		if (timer && std::strcmp(control.id, id) == 0) {
			control.apply(timer->source());
			return;
		}
}

obs_properties_t *properties(void *data)
{
	auto *p = obs_properties_create();
	obs_properties_add_button2(
		p, "timer_settings", label("Countdown.Settings"),
		[](obs_properties_t *, obs_property_t *, void *d) {
			if (d)
				edit_timer(QApplication::activeWindow(), static_cast<Countdown *>(d)->source());
			return false;
		},
		data);
	obs_properties_add_font(p, "font", label("Countdown.Font"));
	obs_properties_add_color_alpha(p, "color", label("Countdown.Color"));
	for (const auto &control : controls)
		obs_properties_add_button2(
			p, control.id, label(control.button),
			[](obs_properties_t *, obs_property_t *b, void *d) {
				send_control(static_cast<Countdown *>(d), obs_property_name(b));
				return false;
			},
			data);
	add_help_line(p, label("Countdown.Help"));
	return p;
}

void defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, "minutes", 5);
	obs_data_set_default_int(s, "color", 0xffffffffLL);
	OBSDataAutoRelease font = obs_data_create();
	obs_data_set_string(font, "face", default_face());
	obs_data_set_int(font, "size", 88);
	obs_data_set_int(font, "flags", OBS_FONT_BOLD);
	obs_data_set_default_obj(s, "font", font);
}

} // namespace

void register_countdown()
{
	obs_source_info info{};
	info.id = kCountdownId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB | OBS_SOURCE_CONTROLLABLE_MEDIA;
	info.icon_type = OBS_ICON_TYPE_TEXT;
	info.get_name = [](void *) {
		return label("Countdown.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		auto *timer = new Countdown(source);
		timer->update(settings);
		if (!obs_obj_is_private(source))
			for (const auto &c : controls)
				obs_hotkey_register_source(
					source, c.id, label(c.hotkey),
					[](void *d, obs_hotkey_id, obs_hotkey_t *key, bool pressed) {
						if (pressed)
							send_control(static_cast<Countdown *>(d),
								     obs_hotkey_get_name(key));
					},
					timer);
		proc_handler_add(
			obs_source_get_proc_handler(source),
			"void timer_readout(out string text, out bool end_message)",
			[](void *d, calldata_t *cd) {
				bool end_message;
				const auto value = static_cast<Countdown *>(d)->text(end_message);
				calldata_set_string(cd, "text", value.toUtf8().constData());
				calldata_set_bool(cd, "end_message", end_message);
			},
			timer);
		return timer;
	};
	info.destroy = [](void *d) {
		delete static_cast<Countdown *>(d);
	};
	info.get_defaults = defaults;
	info.get_properties = properties;
	info.update = [](void *d, obs_data_t *s) {
		static_cast<Countdown *>(d)->update(s);
	};
	info.get_width = [](void *d) {
		return static_cast<Countdown *>(d)->width();
	};
	info.get_height = [](void *d) {
		return static_cast<Countdown *>(d)->height();
	};
	info.activate = [](void *d) {
		static_cast<Countdown *>(d)->activate();
	};
	info.video_tick = [](void *d, float) {
		static_cast<Countdown *>(d)->tick();
	};
	info.video_render = [](void *d, gs_effect_t *) {
		static_cast<Countdown *>(d)->render();
	};
	info.media_play_pause = [](void *d, bool pause) {
		auto *c = static_cast<Countdown *>(d);
		if (pause)
			c->pause();
		else
			c->start(false);
	};
	info.media_restart = [](void *d) {
		static_cast<Countdown *>(d)->start(true);
	};
	info.media_stop = [](void *d) {
		static_cast<Countdown *>(d)->reset();
	};
	info.media_get_duration = [](void *d) {
		return static_cast<Countdown *>(d)->duration();
	};
	info.media_get_time = [](void *d) {
		return static_cast<Countdown *>(d)->elapsed();
	};
	info.media_get_state = [](void *d) {
		return static_cast<Countdown *>(d)->state();
	};
	info.media_set_time = [](void *d, int64_t ms) {
		static_cast<Countdown *>(d)->seek(ms);
	};
	register_timed(&info);
}

} // namespace stagekit
