/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Standby Picture: a picture in a capture's place while the capture has
// nothing to show, so its spot in the scene is not an empty hole.
//
// "Nothing to show" means the capture has no size: no game or window is
// attached to it. A capture that is attached and showing a black picture is
// something, and is left alone.
//
// The picture is the start image unless another is chosen. While it waits it
// keeps its own shape, as tall as the capture last was. When the capture
// arrives the size glides to the capture's while the picture fades into it;
// when the capture goes, the size glides back to the picture's shape. Nothing
// is ever squashed: both are drawn to cover the size of the moment.

#include "standby.hpp"

#include "../common/easing.hpp"
#include "../common/start-image.hpp"
#include "../frame/auto-frame.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

namespace stagekit {

namespace {

// How the picture gives way to the capture, and comes back. Saved: append only.
enum class Style : int {
	Fade = 0,
	Cut = 1,
	SlideLeft = 2,
	SlideRight = 3,
	SlideUp = 4,
	SlideDown = 5,
	WipeLeft = 6,
	WipeRight = 7,
	WipeUp = 8,
	WipeDown = 9,
	Zoom = 10,
};

enum class Picture : int {
	StartImage = 0, // saved: append only
	File = 1,
	Source = 2,
};

// Where a filter comes in the order its source applies them, or -1.
int applied_index(obs_source_t *parent, obs_source_t *filter)
{
	struct Find {
		obs_source_t *wanted;
		int at, found;
	} find{filter, 0, -1};
	obs_source_enum_filters(
		parent,
		[](obs_source_t *, obs_source_t *f, void *param) {
			auto *x = static_cast<Find *>(param);
			if (f == x->wanted)
				x->found = x->at;
			x->at++;
		},
		&find);
	return find.found;
}

obs_source_t *filter_of_kind(obs_source_t *parent, const char *id)
{
	struct Find {
		const char *id;
		obs_source_t *found;
	} find{id, nullptr};
	obs_source_enum_filters(
		parent,
		[](obs_source_t *, obs_source_t *f, void *param) {
			auto *x = static_cast<Find *>(param);
			const char *fid = obs_source_get_unversioned_id(f);
			if (!x->found && fid && strcmp(fid, x->id) == 0)
				x->found = f;
		},
		&find);
	return find.found;
}

bool is_video_file(const std::string &path)
{
	const size_t dot = path.rfind('.');
	if (dot == std::string::npos)
		return false;
	std::string ext = path.substr(dot + 1);
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	for (const char *video : {"mp4", "mov", "mkv", "webm", "avi", "m4v", "flv", "ts", "gif"})
		if (ext == video)
			return true;
	return false;
}

// The size of what comes before a filter. For the capture itself that is its
// own size: asking for its width would come back to this filter.
uint32_t before_width(obs_source_t *filter)
{
	obs_source_t *target = obs_filter_get_target(filter), *parent = obs_filter_get_parent(filter);
	if (!target)
		return 0;
	return target == parent ? obs_source_get_base_width(target) : obs_source_get_width(target);
}
uint32_t before_height(obs_source_t *filter)
{
	obs_source_t *target = obs_filter_get_target(filter), *parent = obs_filter_get_parent(filter);
	if (!target)
		return 0;
	return target == parent ? obs_source_get_base_height(target) : obs_source_get_height(target);
}

class Standby {
public:
	Standby(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		update(settings);
		// For the filters around it: is the picture standing in, or still
		// changing to or from the capture?
		proc_handler_add(
			obs_source_get_proc_handler(source_), "void get_standing(out bool standing, out bool waiting)",
			[](void *data, calldata_t *cd) {
				auto *self = static_cast<Standby *>(data);
				std::lock_guard lock(self->mutex_);
				calldata_set_bool(cd, "standing",
						  self->empty_ || self->mix_ > 0.0 || self->glide_ < 1.0);
				calldata_set_bool(cd, "waiting", self->empty_);
			},
			this);
	}

	~Standby()
	{
		let_go();
		obs_enter_graphics();
		gs_texrender_destroy(render_);
		gs_texrender_destroy(render_live_);
		gs_effect_destroy(effect_);
		obs_leave_graphics();
	}

	void update(obs_data_t *settings)
	{
		std::lock_guard lock(mutex_);
		kind_ = static_cast<Picture>(std::clamp((int)obs_data_get_int(settings, "picture"), 0, 2));
		file_ = obs_data_get_string(settings, "file");
		named_ = obs_data_get_string(settings, "source");
		fade_s_ = std::clamp<long long>(obs_data_get_int(settings, "fade_ms"), 0, 5000) / 1000.0;
		back_s_ = std::clamp<long long>(obs_data_get_int(settings, "back_ms"), 0, 5000) / 1000.0;
		style_ = static_cast<Style>(std::clamp((int)obs_data_get_int(settings, "style"), 0, 10));
		curve_ = static_cast<EaseCurve>(obs_data_get_int(settings, "ease_curve"));
		mode_ = static_cast<EaseMode>(obs_data_get_int(settings, "ease_mode"));
		glide_size_ = obs_data_get_bool(settings, "glide_size");
		last_w_ = (uint32_t)std::max<long long>(0, obs_data_get_int(settings, "last_width"));
		last_h_ = (uint32_t)std::max<long long>(0, obs_data_get_int(settings, "last_height"));
		placed_ = obs_data_get_bool(settings, "placed");
		own_shape_ = obs_data_get_bool(settings, "own_shape");
		changed_ = true;
	}

	void tick(float seconds)
	{
		obs_source_t *parent = obs_filter_get_parent(source_);
		obs_source_t *target = obs_filter_get_target(source_);
		if (!parent || !target)
			return;
		Picture kind;
		std::string file, named;
		bool changed, placed;
		double fade_s;
		{
			std::lock_guard lock(mutex_);
			kind = kind_;
			file = file_;
			named = named_;
			changed = changed_;
			changed_ = false;
			placed = placed_;
			fade_s = fade_s_;
		}
		// Its place among the filters, the first time only: straight after an
		// Auto Frame, so the picture fills the frame, and otherwise first, so
		// corners, shadows and the rest are put on the picture too.
		if (!placed) {
			obs_source_t *frame = filter_of_kind(parent, kAutoFrameId);
			for (int i = 0; i < 32; i++) {
				const int mine = applied_index(parent, source_);
				const int wanted = frame ? applied_index(parent, frame) + 1 : 0;
				if (mine < 0 || mine <= wanted)
					break;
				obs_source_filter_set_order(parent, source_, OBS_ORDER_MOVE_UP);
				if (applied_index(parent, source_) == mine)
					break;
			}
			obs_data_t *settings = obs_source_get_settings(source_);
			obs_data_set_bool(settings, "placed", true);
			obs_data_release(settings);
			std::lock_guard lock(mutex_);
			placed_ = true;
		}
		// An Auto Frame must come before this, always: put after it, an Auto
		// Frame takes the standby picture for a game and trims it to the
		// last game's shape. Looked at twice a second.
		order_check_ += seconds;
		if (order_check_ >= 0.5) {
			order_check_ = 0.0;
			if (obs_source_t *frame = filter_of_kind(parent, kAutoFrameId)) {
				for (int i = 0; i < 32; i++) {
					const int mine = applied_index(parent, source_);
					if (mine < 0 || mine > applied_index(parent, frame))
						break;
					obs_source_filter_set_order(parent, source_, OBS_ORDER_MOVE_DOWN);
					if (applied_index(parent, source_) == mine)
						break;
				}
			}
		}
		if (changed)
			make_picture(kind, file, named);

		// Nothing to show: the capture itself has no size.
		const bool empty = obs_source_get_base_width(parent) == 0 || obs_source_get_base_height(parent) == 0;
		const uint32_t tw = before_width(source_), th = before_height(source_);
		if (!empty && tw && th && (tw != last_w_ || th != last_h_)) {
			// Remembered, for the next time there is nothing.
			obs_data_t *settings = obs_source_get_settings(source_);
			obs_data_set_int(settings, "last_width", tw);
			obs_data_set_int(settings, "last_height", th);
			obs_data_release(settings);
			std::lock_guard lock(mutex_);
			last_w_ = tw;
			last_h_ = th;
		}
		// The size to be: the capture's while it shows; otherwise the
		// picture's own shape, as tall as the capture last was, or (asked to
		// fill) the size the capture last had.
		const uint32_t pw = picture_ ? obs_source_get_width(picture_) : 0;
		const uint32_t ph = picture_ ? obs_source_get_height(picture_) : 0;
		double want_w, want_h;
		{
			std::lock_guard lock(mutex_);
			const double ref_w = last_w_ ? last_w_ : 1920.0, ref_h = last_h_ ? last_h_ : 1080.0;
			if (!empty && tw && th) {
				want_w = tw;
				want_h = th;
			} else if (own_shape_ && pw && ph) {
				want_h = ref_h;
				want_w = ref_h * (double)pw / (double)ph;
			} else {
				want_w = ref_w;
				want_h = ref_h;
			}
			// It glides from the size it has to the size to be, however that
			// changes and whatever changes it: a game starting or ending, or
			// a filter before this one settling on another shape. Towards
			// the capture it takes the first time; back to the picture, the
			// second.
			const double over = empty ? back_s_ : fade_s_;
			const bool cut = style_ == Style::Cut || over <= 0.0;
			// At the very start the picture may not have loaded yet: wait
			// for it rather than begin in the last capture's shape.
			const bool unknown = empty && own_shape_ && picture_ && !(pw && ph) &&
					     (waited_ += seconds) < 2.0;
			if (unknown) {
				// No size yet.
			} else if (cur_w_ <= 0.0 || cur_h_ <= 0.0) {
				cur_w_ = from_w_ = to_w_ = want_w;
				cur_h_ = from_h_ = to_h_ = want_h;
				glide_ = 1.0;
			} else if (std::fabs(want_w - to_w_) > 0.5 || std::fabs(want_h - to_h_) > 0.5) {
				from_w_ = cur_w_;
				from_h_ = cur_h_;
				to_w_ = want_w;
				to_h_ = want_h;
				glide_ = 0.0;
			}
			if (glide_ < 1.0) {
				glide_ = cut || !glide_size_ ? 1.0 : std::min(1.0, glide_ + seconds / over);
				const double t = std::clamp(ease(curve_, mode_, glide_), 0.0, 1.0);
				cur_w_ = from_w_ + (to_w_ - from_w_) * t;
				cur_h_ = from_h_ + (to_h_ - from_h_) * t;
			}
			// How much of the picture shows: all of it with nothing captured,
			// none with a capture, changing over the same times.
			const double step = cut ? 1.0 : seconds / over;
			mix_ = empty ? std::min(1.0, mix_ + step) : std::max(0.0, mix_ - step);
			empty_ = empty;
			live_w_ = empty ? 0 : tw;
			live_h_ = empty ? 0 : th;
		}
		double mix;
		{
			std::lock_guard lock(mutex_);
			mix = mix_;
		}
		// The picture only runs while it is wanted.
		const bool wanted = mix > 0.0 && obs_source_showing(parent);
		if (wanted != holding_ && picture_) {
			if (wanted) {
				obs_source_inc_showing(picture_);
				obs_source_inc_active(picture_);
			} else {
				obs_source_dec_active(picture_);
				obs_source_dec_showing(picture_);
			}
			holding_ = wanted;
		}
	}

	uint32_t width() { return size(true); }
	uint32_t height() { return size(false); }

	void render()
	{
		double mix;
		bool empty;
		uint32_t live_w, live_h;
		{
			std::lock_guard lock(mutex_);
			mix = mix_;
			empty = empty_;
			live_w = live_w_;
			live_h = live_h_;
		}
		const uint32_t w = width(), h = height();
		const bool live = !empty && live_w && live_h;
		if (!w || !h) {
			if (live)
				obs_source_skip_video_filter(source_);
			return;
		}
		if (!effect_) {
			char *path = obs_module_file("effects/stagekit-fade.effect");
			char *errors = nullptr;
			effect_ = path ? gs_effect_create_from_file(path, &errors) : nullptr;
			if (!effect_)
				obs_log(LOG_ERROR, "Standby Picture: could not load its effect: %s",
					errors ? errors : "file not found");
			bfree(errors);
			bfree(path);
			if (!effect_) {
				if (live)
					obs_source_skip_video_filter(source_);
				return;
			}
		}
		// The capture is always drawn by way of a copy, which is kept: when
		// the capture ends there is nothing left to draw, and the change
		// back to the picture starts from that last frame instead of from
		// nothing.
		// While the size glides, both the capture and the picture are drawn
		// to fill the size of the moment: as large as needed to cover it,
		// centred, what does not fit cut off. Neither is ever squashed.
		if (live) {
			if (!render_live_)
				render_live_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
			if (gs_texture_t *texture = covered(render_live_, w, h, live_w, live_h, nullptr)) {
				draw(texture, w, h, 1.0f);
				kept_w_ = w;
				kept_h_ = h;
			} else {
				kept_w_ = kept_h_ = 0;
			}
		} else if (mix < 1.0 && render_live_ && kept_w_ && kept_h_) {
			if (gs_texture_t *texture = gs_texrender_get_texture(render_live_))
				draw_kept(texture, w, h);
		} else {
			kept_w_ = kept_h_ = 0;
		}
		obs_source_t *picture = picture_;
		const uint32_t pw = picture ? obs_source_get_width(picture) : 0;
		const uint32_t ph = picture ? obs_source_get_height(picture) : 0;
		if (mix > 0.0 && pw && ph) {
			if (!render_)
				render_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
			if (gs_texture_t *texture = covered(render_, w, h, pw, ph, picture))
				draw_styled(texture, w, h, mix);
		}
	}

	bool showing()
	{
		std::lock_guard lock(mutex_);
		return empty_;
	}
	obs_source_t *source() const { return source_; }

private:
	// The size the source has through this filter: the one it is gliding
	// through, in whole pixels.
	uint32_t size(bool across)
	{
		std::lock_guard lock(mutex_);
		const double now = across ? cur_w_ : cur_h_;
		if (now >= 1.0)
			return (uint32_t)std::lround(now);
		if (last_w_ && last_h_)
			return across ? last_w_ : last_h_;
		return across ? 1920 : 1080;
	}

	// Draws a source (or, with none given, what comes before this filter)
	// into `into` at w x h, covering it. Returns the texture.
	gs_texture_t *covered(gs_texrender_t *into, uint32_t w, uint32_t h, uint32_t sw, uint32_t sh,
			      obs_source_t *source)
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
			const float scale = std::max((float)w / (float)sw, (float)h / (float)sh);
			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
			gs_matrix_push();
			gs_matrix_translate3f(((float)w - (float)sw * scale) / 2.0f,
					      ((float)h - (float)sh * scale) / 2.0f, 0.0f);
			gs_matrix_scale3f(scale, scale, 1.0f);
			if (source)
				obs_source_video_render(source);
			else
				obs_source_skip_video_filter(source_);
			gs_matrix_pop();
			gs_blend_state_pop();
			gs_texrender_end(into);
			drawn = true;
		}
		gs_enable_framebuffer_srgb(was_srgb);
		gs_set_linear_srgb(was_linear);
		return drawn ? gs_texrender_get_texture(into) : nullptr;
	}

	// Draws the picture as far gone as `shown` says (1 all there, 0 gone), in
	// the style chosen. Whatever leaves stays inside the size: a slide or a
	// zoom draws only the part of the picture still in view.
	void draw_styled(gs_texture_t *texture, uint32_t w, uint32_t h, double shown)
	{
		Style style;
		EaseCurve curve;
		EaseMode mode;
		{
			std::lock_guard lock(mutex_);
			style = style_;
			curve = curve_;
			mode = mode_;
		}
		const double v = std::clamp(ease(curve, mode, std::clamp(shown, 0.0, 1.0)), 0.0, 1.0);
		const double fw = w, fh = h;
		// The part of the picture to draw, where to put it, and how strong.
		double sx = 0, sy = 0, sw = fw, sh = fh, dx = 0, dy = 0, scale_x = 1.0, scale_y = 1.0, strength = 1.0;
		switch (style) {
		case Style::Fade:
			strength = v;
			break;
		case Style::Cut:
			strength = v > 0.0 ? 1.0 : 0.0;
			break;
		case Style::SlideLeft:
			sx = (1.0 - v) * fw;
			sw = fw - sx;
			break;
		case Style::SlideRight:
			sw = v * fw;
			dx = fw - sw;
			break;
		case Style::SlideUp:
			sy = (1.0 - v) * fh;
			sh = fh - sy;
			break;
		case Style::SlideDown:
			sh = v * fh;
			dy = fh - sh;
			break;
		case Style::WipeLeft: // the capture is uncovered from the right
			sw = v * fw;
			break;
		case Style::WipeRight:
			sw = v * fw;
			sx = dx = fw - sw;
			break;
		case Style::WipeUp:
			sh = v * fh;
			break;
		case Style::WipeDown:
			sh = v * fh;
			sy = dy = fh - sh;
			break;
		case Style::Zoom: {
			// It grows as it fades: the middle of it, stretched to the size.
			const double grow = 1.0 + (1.0 - v) * 0.4;
			sw = fw / grow;
			sh = fh / grow;
			sx = (fw - sw) / 2.0;
			sy = (fh - sh) / 2.0;
			scale_x = scale_y = grow;
			strength = v;
			break;
		}
		}
		if (sw < 0.5 || sh < 0.5 || strength <= 0.0)
			return;
		gs_eparam_t *image = gs_effect_get_param_by_name(effect_, "image");
		gs_effect_set_float(gs_effect_get_param_by_name(effect_, "opacity"), (float)strength);
		const bool linear = gs_get_linear_srgb();
		const bool was_srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(linear);
		if (linear)
			gs_effect_set_texture_srgb(image, texture);
		else
			gs_effect_set_texture(image, texture);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		gs_matrix_push();
		gs_matrix_translate3f((float)dx, (float)dy, 0.0f);
		gs_matrix_scale3f((float)scale_x, (float)scale_y, 1.0f);
		while (gs_effect_loop(effect_, "Draw"))
			gs_draw_sprite_subregion(texture, 0, (uint32_t)std::lround(sx), (uint32_t)std::lround(sy),
						 (uint32_t)std::max(1L, std::lround(sw)),
						 (uint32_t)std::max(1L, std::lround(sh)));
		gs_matrix_pop();
		gs_blend_state_pop();
		gs_enable_framebuffer_srgb(was_srgb);
	}

	// Draws a texture over the whole size at a strength from 0 to 1.
	void draw(gs_texture_t *texture, uint32_t w, uint32_t h, float strength)
	{
		gs_eparam_t *image = gs_effect_get_param_by_name(effect_, "image");
		gs_effect_set_float(gs_effect_get_param_by_name(effect_, "opacity"), strength);
		const bool linear = gs_get_linear_srgb();
		const bool was_srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(linear);
		if (linear)
			gs_effect_set_texture_srgb(image, texture);
		else
			gs_effect_set_texture(image, texture);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		while (gs_effect_loop(effect_, "Draw"))
			gs_draw_sprite(texture, 0, w, h);
		gs_blend_state_pop();
		gs_enable_framebuffer_srgb(was_srgb);
	}

	// The last frame of a capture that has ended, covering the size of the
	// moment: as large as needed, centred, what does not fit left out.
	void draw_kept(gs_texture_t *texture, uint32_t w, uint32_t h)
	{
		const float scale = std::max((float)w / (float)kept_w_, (float)h / (float)kept_h_);
		const uint32_t part_w = std::clamp((uint32_t)std::lround((float)w / scale), 1u, kept_w_);
		const uint32_t part_h = std::clamp((uint32_t)std::lround((float)h / scale), 1u, kept_h_);
		gs_eparam_t *image = gs_effect_get_param_by_name(effect_, "image");
		gs_effect_set_float(gs_effect_get_param_by_name(effect_, "opacity"), 1.0f);
		const bool linear = gs_get_linear_srgb();
		const bool was_srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(linear);
		if (linear)
			gs_effect_set_texture_srgb(image, texture);
		else
			gs_effect_set_texture(image, texture);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		gs_matrix_push();
		gs_matrix_scale3f((float)w / (float)part_w, (float)h / (float)part_h, 1.0f);
		while (gs_effect_loop(effect_, "Draw"))
			gs_draw_sprite_subregion(texture, 0, (kept_w_ - part_w) / 2, (kept_h_ - part_h) / 2, part_w,
						 part_h);
		gs_matrix_pop();
		gs_blend_state_pop();
		gs_enable_framebuffer_srgb(was_srgb);
	}

	void let_go()
	{
		if (picture_ && holding_) {
			obs_source_dec_active(picture_);
			obs_source_dec_showing(picture_);
		}
		holding_ = false;
		obs_source_release(picture_);
		picture_ = nullptr;
	}

	// The source that draws the picture: one of our own for the start image
	// or a file, or the one named.
	void make_picture(Picture kind, const std::string &file, const std::string &named)
	{
		let_go();
		if (kind == Picture::Source) {
			obs_source_t *found = named.empty() ? nullptr : obs_get_source_by_name(named.c_str());
			// Not the capture this is on, which would draw itself for ever.
			if (found && found == obs_filter_get_parent(source_)) {
				obs_source_release(found);
				found = nullptr;
			}
			picture_ = found;
			return;
		}
		if (kind == Picture::File && !file.empty()) {
			obs_data_t *settings = obs_data_create();
			const bool video = is_video_file(file);
			obs_data_set_string(settings, video ? "local_file" : "file", file.c_str());
			if (video) {
				obs_data_set_bool(settings, "is_local_file", true);
				obs_data_set_bool(settings, "looping", true);
				obs_data_set_bool(settings, "restart_on_activate", false);
			}
			picture_ = obs_source_create_private(video ? "ffmpeg_source" : "image_source",
							     "Standby Picture", settings);
			obs_data_release(settings);
			if (picture_ && video)
				obs_source_set_muted(picture_, true);
			return;
		}
		picture_ = obs_source_create_private(kStartImageId, "Standby Picture", nullptr);
	}

	obs_source_t *source_;
	std::mutex mutex_;
	Picture kind_ = Picture::StartImage;
	std::string file_, named_;
	double fade_s_ = 0.8, back_s_ = 0.8;
	Style style_ = Style::Fade;
	EaseCurve curve_ = EaseCurve::Cubic;
	EaseMode mode_ = EaseMode::InOut;
	bool glide_size_ = true;
	uint32_t last_w_ = 0, last_h_ = 0;
	bool placed_ = false;
	bool changed_ = true;
	double mix_ = 0.0;                 // how much of the picture shows
	bool empty_ = false;               // the capture has nothing to show
	bool own_shape_ = true;            // while waiting, the picture keeps its own shape
	uint32_t live_w_ = 0, live_h_ = 0; // the capture's size while it shows
	// The size, gliding from one to the next.
	double cur_w_ = 0.0, cur_h_ = 0.0, from_w_ = 0.0, from_h_ = 0.0, to_w_ = 0.0, to_h_ = 0.0, glide_ = 1.0;
	// Used on the graphics thread only.
	obs_source_t *picture_ = nullptr;
	bool holding_ = false;
	gs_texrender_t *render_ = nullptr;
	gs_texrender_t *render_live_ = nullptr;
	uint32_t kept_w_ = 0, kept_h_ = 0; // the size of the capture's last frame in render_live_
	double order_check_ = 1.0, waited_ = 0.0;
	gs_effect_t *effect_ = nullptr;
};

bool picture_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const auto kind = static_cast<Picture>(obs_data_get_int(settings, "picture"));
	obs_property_set_visible(obs_properties_get(props, "file"), kind == Picture::File);
	obs_property_set_visible(obs_properties_get(props, "source"), kind == Picture::Source);
	return true;
}

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<Standby *>(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "status",
				obs_module_text(self && self->showing() ? "Standby.Status.Showing"
									: "Standby.Status.Waiting"),
				OBS_TEXT_INFO);

	obs_property_t *picture = obs_properties_add_list(props, "picture", obs_module_text("Standby.Picture"),
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(picture, obs_module_text("Standby.Picture.Start"), (int)Picture::StartImage);
	obs_property_list_add_int(picture, obs_module_text("Standby.Picture.File"), (int)Picture::File);
	obs_property_list_add_int(picture, obs_module_text("Standby.Picture.Source"), (int)Picture::Source);
	obs_property_set_long_description(picture, obs_module_text("Standby.Picture.Tip"));
	obs_property_set_modified_callback(picture, picture_modified);

	obs_properties_add_path(props, "file", obs_module_text("Standby.File"), OBS_PATH_FILE,
				obs_module_text("Standby.File.Filter"), nullptr);
	obs_property_t *source = obs_properties_add_list(props, "source", obs_module_text("Standby.Source"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	struct Fill {
		obs_property_t *list;
		obs_source_t *self;
	} fill{source, self ? obs_filter_get_parent(self->source()) : nullptr};
	obs_property_list_add_string(source, obs_module_text("Standby.Source.None"), "");
	obs_enum_sources(
		[](void *param, obs_source_t *candidate) {
			auto *f = static_cast<Fill *>(param);
			const char *name = obs_source_get_name(candidate);
			if (candidate != f->self && name && *name &&
			    (obs_source_get_output_flags(candidate) & OBS_SOURCE_VIDEO))
				obs_property_list_add_string(f->list, name, name);
			return true;
		},
		&fill);

	obs_property_t *own_shape = obs_properties_add_bool(props, "own_shape", obs_module_text("Standby.OwnShape"));
	obs_property_set_long_description(own_shape, obs_module_text("Standby.OwnShape.Tip"));

	// How the picture gives way to the capture and comes back.
	obs_property_t *style = obs_properties_add_list(props, "style", obs_module_text("Standby.Style"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	const struct {
		Style value;
		const char *text;
	} styles[] = {{Style::Fade, "Standby.Style.Fade"},
		      {Style::Cut, "Standby.Style.Cut"},
		      {Style::SlideLeft, "Standby.Style.SlideLeft"},
		      {Style::SlideRight, "Standby.Style.SlideRight"},
		      {Style::SlideUp, "Standby.Style.SlideUp"},
		      {Style::SlideDown, "Standby.Style.SlideDown"},
		      {Style::WipeLeft, "Standby.Style.WipeLeft"},
		      {Style::WipeRight, "Standby.Style.WipeRight"},
		      {Style::WipeUp, "Standby.Style.WipeUp"},
		      {Style::WipeDown, "Standby.Style.WipeDown"},
		      {Style::Zoom, "Standby.Style.Zoom"}};
	for (const auto &entry : styles)
		obs_property_list_add_int(style, obs_module_text(entry.text), (int)entry.value);
	obs_property_set_long_description(style, obs_module_text("Standby.Style.Tip"));
	obs_property_set_modified_callback(style, [](obs_properties_t *p, obs_property_t *, obs_data_t *settings) {
		const bool timed = obs_data_get_int(settings, "style") != (int)Style::Cut;
		for (const char *name : {"fade_ms", "back_ms", "ease_curve", "ease_mode", "glide_size"})
			obs_property_set_visible(obs_properties_get(p, name), timed);
		return true;
	});
	obs_property_t *fade =
		obs_properties_add_int_slider(props, "fade_ms", obs_module_text("Standby.Fade"), 0, 5000, 50);
	obs_property_int_set_suffix(fade, " ms");
	obs_property_set_long_description(fade, obs_module_text("Standby.Fade.Tip"));
	obs_property_t *back =
		obs_properties_add_int_slider(props, "back_ms", obs_module_text("Standby.Back"), 0, 5000, 50);
	obs_property_int_set_suffix(back, " ms");
	obs_property_set_long_description(back, obs_module_text("Standby.Back.Tip"));
	add_easing_properties(props, "");
	obs_property_t *glide = obs_properties_add_bool(props, "glide_size", obs_module_text("Standby.GlideSize"));
	obs_property_set_long_description(glide, obs_module_text("Standby.GlideSize.Tip"));

	obs_property_t *help = obs_properties_add_text(props, "help", obs_module_text("Standby.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(help, true);
	return props;
}

} // namespace

bool standby_waiting(obs_source_t *source)
{
	bool waiting = false;
	if (!source)
		return false;
	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			const char *id = obs_source_get_unversioned_id(filter);
			if (!id || strcmp(id, kStandbyId) != 0 || !obs_source_enabled(filter))
				return;
			calldata_t cd = {};
			if (proc_handler_call(obs_source_get_proc_handler(filter), "get_standing", &cd) &&
			    calldata_bool(&cd, "waiting"))
				*static_cast<bool *>(param) = true;
			calldata_free(&cd);
		},
		&waiting);
	return waiting;
}

void register_standby()
{
	obs_source_info info = {};
	info.id = kStandbyId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = [](void *) {
		return obs_module_text("Standby.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new Standby(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<Standby *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<Standby *>(data)->update(settings);
	};
	info.get_defaults = [](obs_data_t *settings) {
		obs_data_set_default_int(settings, "picture", (int)Picture::StartImage);
		obs_data_set_default_int(settings, "fade_ms", 800);
		obs_data_set_default_int(settings, "back_ms", 800);
		obs_data_set_default_int(settings, "style", (int)Style::Fade);
		obs_data_set_default_int(settings, "ease_curve", (int)EaseCurve::Cubic);
		obs_data_set_default_int(settings, "ease_mode", (int)EaseMode::InOut);
		obs_data_set_default_bool(settings, "glide_size", true);
		obs_data_set_default_bool(settings, "own_shape", true);
		obs_data_set_default_bool(settings, "placed", false);
	};
	info.get_properties = get_properties;
	info.video_tick = [](void *data, float seconds) {
		static_cast<Standby *>(data)->tick(seconds);
	};
	info.video_render = [](void *data, gs_effect_t *) {
		static_cast<Standby *>(data)->render();
	};
	info.get_width = [](void *data) {
		return static_cast<Standby *>(data)->width();
	};
	info.get_height = [](void *data) {
		return static_cast<Standby *>(data)->height();
	};
	obs_register_source(&info);
}

} // namespace stagekit
