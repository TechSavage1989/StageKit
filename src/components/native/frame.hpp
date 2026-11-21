#pragma once

#include "component.hpp"
#include <graphics/vec2.h>
#include <graphics/vec4.h>

namespace stagekit::native {
struct Size {
	uint32_t width, height;
	explicit operator bool() const { return width && height; }
};
inline Size size_of_target(obs_source_t *source)
{
	auto *target = obs_filter_get_target(source);
	return target ? Size{obs_source_get_base_width(target), obs_source_get_base_height(target)} : Size{};
}

class Effect {
public:
	explicit Effect(const char *resource)
	{
		char *file = obs_find_module_file(obs_get_module("stagekit"), resource);
		if (file) {
			obs_enter_graphics();
			value = gs_effect_create_from_file(file, nullptr);
			obs_leave_graphics();
			bfree(file);
		}
	}
	~Effect()
	{
		obs_enter_graphics();
		gs_effect_destroy(value);
		obs_leave_graphics();
	}
	Effect(const Effect &) = delete;
	Effect &operator=(const Effect &) = delete;
	gs_eparam_t *parameter(const char *name) const { return gs_effect_get_param_by_name(value, name); }
	void scalar(const char *name, float v) const { gs_effect_set_float(parameter(name), v); }
	void pair(const char *name, float x, float y) const
	{
		vec2 v;
		vec2_set(&v, x, y);
		gs_effect_set_vec2(parameter(name), &v);
	}
	void four(const char *name, float x, float y, float z, float w) const
	{
		vec4 v;
		vec4_set(&v, x, y, z, w);
		gs_effect_set_vec4(parameter(name), &v);
	}
	gs_effect_t *value = nullptr;
};

// Capture the preceding filter chain without inheriting the compositing transform
// of the scene item that happens to render it first.
class Frame {
public:
	Frame() = default;
	Frame(const Frame &) = delete;
	Frame &operator=(const Frame &) = delete;
	~Frame()
	{
		obs_enter_graphics();
		gs_texrender_destroy(target);
		obs_leave_graphics();
	}
	bool begin(Size size, gs_color_space space = GS_CS_SRGB)
	{
		const auto needed = gs_get_format_from_space(space);
		if (!target || format != needed) {
			gs_texrender_destroy(target);
			format = needed;
			target = gs_texrender_create(format, GS_ZS_NONE);
		}
		if (!target || !size)
			return false;
		gs_texrender_reset(target);
		if (!gs_texrender_begin_with_color_space(target, size.width, size.height, space))
			return false;
		vec4 clear{};
		gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
		gs_ortho(0, float(size.width), 0, float(size.height), -100, 100);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		return true;
	}
	void end()
	{
		gs_blend_state_pop();
		gs_texrender_end(target);
	}
	bool capture(obs_source_t *source, Size size, gs_color_space space = GS_CS_SRGB)
	{
		if (!begin(size, space))
			return false;
		const bool ok = obs_source_process_filter_begin_with_color_space(source, format, space,
										 OBS_NO_DIRECT_RENDERING);
		if (ok)
			obs_source_process_filter_end(source, obs_get_base_effect(OBS_EFFECT_DEFAULT), size.width,
						      size.height);
		end();
		return ok;
	}
	gs_texture_t *texture() const { return target ? gs_texrender_get_texture(target) : nullptr; }

private:
	gs_color_format format = GS_RGBA;
	gs_texrender_t *target = nullptr;
};

inline void draw_texture(gs_texture_t *tex, Size size)
{
	if (!tex)
		return;
	auto *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	auto *image = gs_effect_get_param_by_name(effect, "image");
	const bool previous = gs_framebuffer_srgb_enabled();
	const bool linear = gs_get_linear_srgb();
	gs_enable_framebuffer_srgb(linear);
	if (linear)
		gs_effect_set_texture_srgb(image, tex);
	else
		gs_effect_set_texture(image, tex);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(tex, 0, size.width, size.height);
	gs_enable_framebuffer_srgb(previous);
}
} // namespace stagekit::native
