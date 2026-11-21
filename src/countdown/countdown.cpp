/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "countdown.hpp"
#include "../api/vendor-api.hpp"
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
#include <QFontMetricsF>
#include <QStringList>
#include <QVector>
#include <array>
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
	QSize canvas{1, 1};
	QRectF digit_ink, colon_ink;
	qreal digit_step = 1, colon_step = 1;
	std::array<qreal, 10> offsets{};

	// Include font decorations and negative bearings in the same coordinates used to paint.
	QRectF ink(const QFontMetricsF &m, const QString &s) const
	{
		QRectF bounds = m.tightBoundingRect(s);
		const qreal advance = m.horizontalAdvance(s);
		if (font.underline())
			bounds |= QRectF(0, m.underlinePos() - m.lineWidth(), advance, 2 * m.lineWidth());
		if (font.strikeOut())
			bounds |= QRectF(0, -m.strikeOutPos() - m.lineWidth(), advance, 2 * m.lineWidth());
		return bounds;
	}

	QRectF number_bounds(const QString &number) const
	{
		QRectF result;
		qreal x = 0;
		for (const QChar c : number) {
			const bool separator = c == QLatin1Char(':');
			result |= (separator ? colon_ink : digit_ink).translated(x, 0);
			x += separator ? colon_step : digit_step;
		}
		return result;
	}

	void measure(const QString &reference)
	{
		const QFontMetricsF m(font);
		digit_step = 1;
		for (int i = 0; i < 10; ++i)
			digit_step = std::max(digit_step, m.horizontalAdvance(QChar('0' + i)));
		digit_ink = {};
		for (int i = 0; i < 10; ++i) {
			const QChar c('0' + i);
			offsets[i] = (digit_step - m.horizontalAdvance(c)) / 2;
			digit_ink |= ink(m, QString(c)).translated(offsets[i], 0);
		}
		colon_step = m.horizontalAdvance(QLatin1Char(':'));
		colon_ink = ink(m, QStringLiteral(":"));
		const QRectF bounds = number_bounds(reference);
		canvas = QSize(std::clamp(int(std::ceil(bounds.width())) + 2, 3, 4096),
			       std::clamp(int(std::ceil(bounds.height())) + 2, 3, 2048));
	}

	qreal fit(const QRectF &ink_bounds) const
	{
		return std::min((canvas.width() - 2) / std::max<qreal>(1, ink_bounds.width()),
				(canvas.height() - 2) / std::max<qreal>(1, ink_bounds.height()));
	}

	struct Lines {
		QStringList text;
		QVector<QRectF> bounds;
		QRectF block;
	};

	Lines measure_lines(const QFontMetricsF &m, const QStringList &lines) const
	{
		Lines result;
		result.text = lines;
		for (const auto &line : lines) {
			const auto box = ink(m, line);
			const qreal baseline = result.bounds.size() * m.lineSpacing();
			result.bounds.append(box);
			result.block |= box.translated(-box.center().x(), baseline);
		}
		return result;
	}

	// Keep a short label on one line. Wrap phrases only when doing so makes the letters larger.
	Lines fit_lines(const QString &text) const
	{
		const QFontMetricsF m(font);
		Lines best = measure_lines(m, {text});
		const auto words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
		if (words.size() < 2)
			return best;
		const qreal full = m.horizontalAdvance(text);
		const qreal gap = m.horizontalAdvance(QLatin1Char(' '));
		QVector<qreal> advances;
		for (const QString &word : words)
			advances.append(m.horizontalAdvance(word));
		for (int count = 2; count <= std::min<qsizetype>(4, words.size()); ++count) {
			QStringList lines;
			QString line;
			qreal used = 0;
			for (qsizetype i = 0; i < words.size(); ++i) {
				if (!line.isEmpty() && used + gap + advances[i] > full / count &&
				    lines.size() + 1 < count) {
					lines.append(line);
					line.clear();
					used = 0;
				}
				if (!line.isEmpty()) {
					line += QLatin1Char(' ');
					used += gap;
				}
				line += words[i];
				used += advances[i];
			}
			lines.append(line);
			auto candidate = measure_lines(m, lines);
			if (fit(candidate.block) > fit(best.block) * 1.05)
				best = std::move(candidate);
		}
		return best;
	}

	QImage paint(const QString &text, bool free_text, int density) const
	{
		QImage frame(canvas * density, QImage::Format_RGBA8888_Premultiplied);
		if (frame.isNull())
			return frame;
		frame.setDevicePixelRatio(density);
		frame.fill(Qt::transparent);
		QPainter p(&frame);
		p.setRenderHint(QPainter::TextAntialiasing);
		p.setFont(font);
		p.setPen(color);
		const Lines lines = free_text ? fit_lines(text) : Lines{};
		const QRectF bounds = free_text ? lines.block : number_bounds(text);
		if (bounds.isEmpty())
			return frame;
		p.translate(canvas.width() / 2.0, canvas.height() / 2.0);
		const qreal factor = fit(bounds);
		p.scale(factor, factor);
		p.translate(-bounds.center());
		if (free_text) {
			const qreal step = QFontMetricsF(font).lineSpacing();
			for (qsizetype i = 0; i < lines.text.size(); ++i)
				p.drawText(QPointF(-lines.bounds[i].center().x(), i * step), lines.text[i]);
		} else {
			qreal x = 0;
			for (const QChar c : text) {
				const bool separator = c == QLatin1Char(':');
				p.drawText(QPointF(x + (separator ? 0 : offsets[c.digitValue()]), 0), QString(c));
				x += separator ? colon_step : digit_step;
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
		gs_texture_destroy(outgoing_);
		gs_effect_destroy(morph_);
		obs_leave_graphics();
	}

	void update(obs_data_t *s)
	{
		OBSDataAutoRelease data = obs_data_get_obj(s, "font");
		QFont font(QString::fromUtf8(obs_data_get_string(data, "face")));
		const auto flags = obs_data_get_int(data, "flags");
		font.setHintingPreference(QFont::PreferNoHinting);
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
		const bool format_changed = next.units != clock_.settings.units ||
					    next.leading_zero != clock_.settings.leading_zero;
		clock_.settings = next;
		if (reset)
			clock_.reset();
		const bool font_changed = display_.font != font;
		display_.font = font;
		display_.color = color;
		initialized_ = true;
		dirty_ = true;
		if (reset || font_changed || format_changed)
			measure_locked();
	}

	void start(bool restart)
	{
		{
			std::lock_guard lock(mutex_);
			if (!clock_.start(restart))
				return;
			++*generation_;
		}
		obs_source_media_started(source_);
		vendor_emit_timer_changed(source_);
	}

	void pause()
	{
		// Latch a deadline already crossed before accepting the pause.
		tick();
		{
			std::lock_guard lock(mutex_);
			if (clock_.state != OBS_MEDIA_STATE_PLAYING)
				return;
			clock_.position = clock_.elapsed();
			clock_.state = OBS_MEDIA_STATE_PAUSED;
		}
		vendor_emit_timer_changed(source_);
	}

	void reset()
	{
		{
			std::lock_guard lock(mutex_);
			++*generation_;
			clock_.reset();
		}
		vendor_emit_timer_changed(source_);
	}

	bool command(const char *op, int mark)
	{
		const bool cycle = std::strcmp(op, "cycle") == 0;
		const bool hold = std::strcmp(op, "cycleToggle") == 0;
		const bool toggle = hold || std::strcmp(op, "toggle") == 0;
		const auto before = state();
		if (std::strcmp(op, "stop") == 0 || (toggle && before == OBS_MEDIA_STATE_PLAYING)) {
			reset();
			return true;
		}
		if (std::strcmp(op, "pause") == 0) {
			pause();
			return true;
		}
		if (std::strcmp(op, "start") == 0) {
			start(false);
			return true;
		}
		const bool set_only = std::strcmp(op, "set") == 0 || (cycle && before != OBS_MEDIA_STATE_PLAYING);
		if (!set_only && !toggle && !cycle && std::strcmp(op, "setStart") != 0)
			return false;
		OBSDataAutoRelease settings = obs_source_get_settings(source_);
		if (cycle || hold) {
			const int current = saved_timer_mark(settings);
			if (hold)
				mark = current == 0 || current == 30 || current == 45 ? current : 15;
			else
				mark = current == 15 ? 30 : current == 30 ? 45 : current == 45 ? 0 : 15;
		}
		if (mark != kSavedTimer) {
			const auto next = next_timer_mark(QDateTime::currentDateTime(), mark);
			if (!next.isValid())
				return false;
			obs_data_set_int(settings, "clock_shortcut", mark);
			obs_data_set_int(settings, "timer_mode", int(TimerMode::Deadline));
			obs_data_set_int(settings, "deadline_ms", next.toMSecsSinceEpoch());
			obs_data_set_bool(settings, "count_up_to_date", false);
			obs_source_update(source_, settings);
			// Apply the same settings before starting, including for a hidden or private timer.
			update(settings);
		}
		if (cycle && before == OBS_MEDIA_STATE_PAUSED) {
			{
				std::lock_guard lock(mutex_);
				clock_.reset();
				clock_.state = OBS_MEDIA_STATE_PAUSED;
			}
			vendor_emit_timer_changed(source_);
		} else if (set_only)
			reset();
		else
			start(true);
		return true;
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
		vendor_emit_timer_changed(source_);
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
		return static_cast<uint32_t>(display_.canvas.width());
	}
	uint32_t height() const
	{
		std::lock_guard lock(mutex_);
		return static_cast<uint32_t>(display_.canvas.height());
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
	}

	void render()
	{
		matrix4 transform;
		gs_matrix_get(&transform);
		const float scale =
			std::max(std::hypot(transform.x.x, transform.x.y), std::hypot(transform.y.x, transform.y.y));
		const uint64_t now = os_gettime_ns();
		QSize canvas;
		{
			std::lock_guard lock(mutex_);
			canvas = display_.canvas;
			if (outgoing_ && rendered_canvas_ != canvas) {
				gs_texture_destroy(outgoing_);
				outgoing_ = nullptr;
			}
			const bool down = !clock_.settings.counts_up();
			const int64_t second = (clock_.value() + (down ? 999 : 0)) / 1000;
			const bool msg = clock_.message_visible();
			const float zoom = std::isfinite(scale) ? std::clamp(scale, 1.0f, float(max_density_)) : 1.0f;
			int wanted = int(std::ceil(zoom));
			if (wanted >= density_)
				last_large_draw_ = now;
			else if (now - last_large_draw_ < 1000000000ULL)
				wanted = std::min(density_, max_density_);
			if (!texture_ || second != last_second_ || msg != last_message_ || dirty_ ||
			    wanted != density_) {
				const auto &options = clock_.settings;
				const QString value = options.display_text(second * 1000, clock_.span, msg);
				const QString style = msg ? options.message
							  : (options.format_output ? options.output_format : QString());
				const bool switch_view = rendered_canvas_ == canvas && texture_ &&
							 value != last_value_ &&
							 (msg != last_message_ || style != last_style_) &&
							 now - last_draw_ < 500000000ULL;
				const bool free_text = msg || (options.format_output &&
							       options.output_format != QStringLiteral("%time%"));
				const bool repaint = dirty_ || !texture_ || wanted != density_ || value != last_value_ || msg != last_message_;
				if (repaint && !update_texture(value, free_text, wanted, switch_view, now))
					return;
				last_value_ = value;
				last_style_ = style;
				rendered_canvas_ = canvas;
				density_ = wanted;
				last_second_ = second;
				last_message_ = msg;
				dirty_ = false;
			}
			last_draw_ = now;
		}
		draw(canvas, now);
	}

private:
	void measure_locked()
	{
		const auto &options = clock_.settings;
		// Only the bare clock establishes the transform. An unlimited timer keeps its initial box as it grows.
		display_.measure(options.clock_text(clock_.span, clock_.span));
		const int width = display_.canvas.width(), height = display_.canvas.height();
		const int side_limit = 8192 / std::max(width, height);
		const int area_limit = int(std::sqrt((4.0 * 1024 * 1024) / (width * height)));
		max_density_ = std::max(1, std::min(side_limit, area_limit));
	}

	bool update_texture(const QString &value, bool free_text, int density, bool switch_view, uint64_t now)
	{
		const QImage frame = display_.paint(value, free_text, density);
		if (frame.isNull())
			return false;
		const auto w = static_cast<uint32_t>(frame.width());
		const auto h = static_cast<uint32_t>(frame.height());
		const uint8_t *pixels = frame.constBits();
		if (!switch_view && texture_ && gs_texture_get_width(texture_) == w &&
		    gs_texture_get_height(texture_) == h) {
			gs_texture_set_image(texture_, pixels, static_cast<uint32_t>(frame.bytesPerLine()), false);
		} else {
			auto *next = gs_texture_create(w, h, GS_RGBA, 1, &pixels, GS_DYNAMIC);
			if (!next)
				return false;
			if (switch_view) {
				if (!tried_morph_) {
					tried_morph_ = true;
					char *path = obs_module_file("effects/stagekit-morph.effect");
					morph_ = path ? gs_effect_create_from_file(path, nullptr) : nullptr;
					bfree(path);
				}
				const bool reversing = outgoing_ && value == outgoing_value_ &&
						       now - morph_start_ < morph_ns;
				const uint64_t elapsed = reversing ? morph_ns - (now - morph_start_) : 0;
				gs_texture_destroy(outgoing_);
				outgoing_ = texture_;
				outgoing_value_ = last_value_;
				texture_ = nullptr;
				morph_start_ = now - elapsed;
			}
			gs_texture_destroy(texture_);
			texture_ = next;
		}
		return true;
	}

	void draw(const QSize &canvas, uint64_t now)
	{
		const bool blending = morph_ && outgoing_ && now - morph_start_ < morph_ns;
		if (!blending && outgoing_) {
			gs_texture_destroy(outgoing_);
			outgoing_ = nullptr;
		}
		auto *effect = blending ? morph_ : obs_get_base_effect(OBS_EFFECT_DEFAULT);
		const bool linear = gs_get_linear_srgb();
		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(linear);
		auto bind = [&](const char *name, gs_texture_t *texture) {
			auto *param = gs_effect_get_param_by_name(effect, name);
			if (linear)
				gs_effect_set_texture_srgb(param, texture);
			else
				gs_effect_set_texture(param, texture);
		};
		bind("image", blending ? outgoing_ : texture_);
		if (blending) {
			const float t = float(now - morph_start_) / float(morph_ns);
			const float mix = t * t * (3.0f - 2.0f * t);
			const float scale = 1.0f - 0.32f * mix * (1.0f - mix);
			bind("image2", texture_);
			gs_effect_set_float(gs_effect_get_param_by_name(effect, "amount"), mix);
			gs_effect_set_float(gs_effect_get_param_by_name(effect, "opacity"), 1.0f);
			gs_matrix_push();
			gs_matrix_translate3f(canvas.width() * (1.0f - scale) / 2, canvas.height() * (1.0f - scale) / 2,
					      0);
			gs_matrix_scale3f(scale, scale, 1.0f);
		}
		while (gs_effect_loop(effect, blending ? "Mix" : "Draw"))
			gs_draw_sprite(blending ? outgoing_ : texture_, 0, uint32_t(canvas.width()),
				       uint32_t(canvas.height()));
		if (blending)
			gs_matrix_pop();
		gs_enable_framebuffer_srgb(previous);
	}

	obs_source_t *source_;
	mutable std::mutex mutex_;
	Playback clock_;
	Display display_;
	gs_texture_t *texture_ = nullptr;
	static constexpr uint64_t morph_ns = 240000000ULL;
	gs_texture_t *outgoing_ = nullptr;
	gs_effect_t *morph_ = nullptr;
	uint64_t morph_start_ = 0, last_draw_ = 0;
	QString last_value_, last_style_, outgoing_value_;
	QSize rendered_canvas_;
	bool tried_morph_ = false;
	int64_t last_second_ = -1;
	int density_ = 1, max_density_ = 1;
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

bool timer_command(obs_source_t *source, const char *operation, int mark)
{
	if (!source || !operation || obs_source_removed(source) ||
	    std::strcmp(obs_source_get_id(source), kCountdownId) != 0)
		return false;
	calldata_t call{};
	calldata_set_string(&call, "operation", operation);
	calldata_set_int(&call, "mark", mark);
	proc_handler_call(obs_source_get_proc_handler(source), "timer_command", &call);
	const bool success = calldata_bool(&call, "success");
	calldata_free(&call);
	return success;
}

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
		proc_handler_add(
			obs_source_get_proc_handler(source),
			"void timer_command(in string operation, in int mark, out bool success)",
			[](void *data, calldata_t *params) {
				const char *op = calldata_string(params, "operation");
				calldata_set_bool(params, "success",
						  op && static_cast<Countdown *>(data)->command(
								op, int(calldata_int(params, "mark"))));
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
