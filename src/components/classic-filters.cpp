#include "classic-filters.hpp"
#include "../common/tooltip.hpp"

#include <obs-module.h>
#include <graphics/image-file.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <string>

namespace stagekit {
namespace {

enum class Kind { Custom, Shadow, Edges, Gaussian, Toon, Alpha };
struct Definition {
	const char *id;
	const char *label;
	Kind kind;
};

constexpr Definition effects[] = {
	{"stagekit_effect_file", "Classic.Custom", Kind::Custom},
	{"stagekit_drop_shadow", "Classic.Shadow", Kind::Shadow},
	{"stagekit_edge_detection", "Classic.Edges", Kind::Edges},
	{"stagekit_gaussian_blur", "Classic.Gaussian", Kind::Gaussian},
	{"stagekit_toon", "Classic.Toon", Kind::Toon},
	{"stagekit_fix_alpha", "Classic.Alpha", Kind::Alpha},
};

struct Filter {
	obs_source_t *source;
	Kind kind;
	gs_effect_t *effect = nullptr;
	gs_texrender_t *input = nullptr, *horizontal = nullptr;
	gs_image_file_t mask{};
	std::string mask_path, effect_path, error;
	std::atomic<bool> reload{false};
	int x = 30, y = 30, hue = 5, value = 5, radius = 1, spacing = 1;
	bool hide_image = false, hide_shadow = false, use_mask = false, invert = false;
	float threshold = 0.3f;
	vec4 colour{};
	std::array<float, 16> weights{};

	Filter(obs_source_t *src, Kind type) : source(src), kind(type)
	{
		if (kind != Kind::Custom && kind != Kind::Alpha) {
			char *file = obs_module_file("effects/stagekit-classic.effect");
			obs_enter_graphics();
			if (file)
				effect = gs_effect_create_from_file(file, nullptr);
			if (kind == Kind::Gaussian) {
				input = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
				horizontal = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
			}
			obs_leave_graphics();
			bfree(file);
		}
	}

	~Filter()
	{
		obs_enter_graphics();
		gs_image_file_free(&mask);
		gs_texrender_destroy(input);
		gs_texrender_destroy(horizontal);
		gs_effect_destroy(effect);
		obs_leave_graphics();
	}

	void update(obs_data_t *s)
	{
		if (kind == Kind::Custom) {
			std::string next = obs_data_get_string(s, "effect_path");
			const bool force = reload.exchange(false);
			if (effect_path == next && !force)
				return;
			effect_path = next;
			error.clear();
			char *message = nullptr;
			obs_enter_graphics();
			gs_effect_t *loaded = next.empty() ? nullptr : gs_effect_create_from_file(next.c_str(), &message);
			if (loaded && !gs_effect_get_technique(loaded, "Draw")) {
				gs_effect_destroy(loaded);
				loaded = nullptr;
			}
			if (loaded || next.empty()) {
				gs_effect_destroy(effect);
				effect = loaded;
			} else {
				error = message ? message : obs_module_text("Classic.LoadFailed");
				blog(LOG_WARNING, "[stagekit] Effect file: %s", error.c_str());
			}
			bfree(message);
			obs_leave_graphics();
			return;
		}
		if (kind == Kind::Gaussian) {
			radius = std::clamp((int)obs_data_get_int(s, "kernel_size") / 2, 0, 15);
			spacing = std::clamp((int)obs_data_get_int(s, "pixel_skip"), 1, 128);
			const double sigma = std::clamp(obs_data_get_double(s, "sigma"), 1.0, 10.0);
			float sum = 0.0f;
			for (int i = 0; i < 16; ++i) {
				weights[i] = i <= radius ? (float)std::exp(-0.5 * i * i / (sigma * sigma)) : 0.0f;
				sum += weights[i] * (i ? 2.0f : 1.0f);
			}
			for (float &w : weights)
				w /= sum;
			use_mask = obs_data_get_bool(s, "use_mask");
			invert = obs_data_get_bool(s, "invert_mask");
			std::string next = obs_data_get_string(s, "mask_image");
			if (next != mask_path) {
				obs_enter_graphics();
				gs_image_file_free(&mask);
				mask = {};
				mask_path = next;
				if (!next.empty()) {
					gs_image_file_init(&mask, next.c_str());
					if (mask.loaded)
						gs_image_file_init_texture(&mask);
				}
				obs_leave_graphics();
			}
			return;
		}
		x = std::clamp((int)obs_data_get_int(s, "offset_x"), -8192, 8192);
		y = std::clamp((int)obs_data_get_int(s, "offset_y"), -8192, 8192);
		hide_image = obs_data_get_bool(s, "hide_image");
		hide_shadow = obs_data_get_bool(s, "hide_shadow");
		threshold = (float)std::clamp(obs_data_get_double(s, "threshold"), 0.0, 1.0);
		hue = std::clamp((int)obs_data_get_int(s, "hue_steps"), 1, 10);
		value = std::clamp((int)obs_data_get_int(s, "value_steps"), 1, 10);
		vec4_from_rgba(&colour, (uint32_t)obs_data_get_int(s, kind == Kind::Shadow ? "shadow_color" : "color"));
	}

	gs_eparam_t *param(const char *name) const { return gs_effect_get_param_by_name(effect, name); }
	void vector(const char *name, float a, float b) const
	{
		vec2 v;
		vec2_set(&v, a, b);
		if (auto *p = param(name))
			gs_effect_set_vec2(p, &v);
	}

	void gaussian(uint32_t w, uint32_t h)
	{
		if (radius == 0 || !input || !horizontal || (use_mask && !mask.texture)) {
			obs_source_skip_video_filter(source);
			return;
		}
		auto begin = [w, h](gs_texrender_t *target) {
			gs_texrender_reset(target);
			if (!gs_texrender_begin(target, w, h))
				return false;
			vec4 clear{};
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
			return true;
		};
		if (!begin(input)) {
			obs_source_skip_video_filter(source);
			return;
		}
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		const bool captured = obs_source_process_filter_begin(source, GS_RGBA, OBS_NO_DIRECT_RENDERING);
		if (captured)
			obs_source_process_filter_end(source, obs_get_base_effect(OBS_EFFECT_DEFAULT), w, h);
		gs_texrender_end(input);
		if (!captured || !begin(horizontal)) {
			gs_blend_state_pop();
			obs_source_skip_video_filter(source);
			return;
		}
		const char *weight_names[] = {"weights0", "weights1", "weights2", "weights3"};
		auto weights_for_pass = [&] {
			for (int i = 0; i < 4; ++i)
				gs_effect_set_val(param(weight_names[i]), weights.data() + 4 * i, 4 * sizeof(float));
			gs_effect_set_int(param("radius"), radius);
		};
		weights_for_pass();
		vector("pixel_size", (float)spacing / (float)w, 0.0f);
		gs_effect_set_texture(param("image"), gs_texrender_get_texture(input));
		while (gs_effect_loop(effect, "Horizontal"))
			gs_draw_sprite(nullptr, 0, w, h);
		gs_texrender_end(horizontal);
		gs_blend_state_pop();
		weights_for_pass();
		vector("pixel_size", 0.0f, (float)spacing / (float)h);
		gs_effect_set_texture(param("image"), gs_texrender_get_texture(horizontal));
		gs_effect_set_texture(param("original"), gs_texrender_get_texture(input));
		gs_effect_set_texture(param("mask_image"), use_mask ? mask.texture : gs_texrender_get_texture(input));
		gs_effect_set_bool(param("use_mask"), use_mask);
		gs_effect_set_bool(param("invert_mask"), invert);
		while (gs_effect_loop(effect, "Vertical"))
			gs_draw_sprite(nullptr, 0, w, h);
	}

	void render()
	{
		auto *target = obs_filter_get_target(source);
		const uint32_t w = target ? obs_source_get_base_width(target) : 0;
		const uint32_t h = target ? obs_source_get_base_height(target) : 0;
		auto *active = kind == Kind::Alpha ? obs_get_base_effect(OBS_EFFECT_PREMULTIPLIED_ALPHA) : effect;
		if (!active || !w || !h) {
			obs_source_skip_video_filter(source);
			return;
		}
		if (kind == Kind::Gaussian) {
			gaussian(w, h);
			return;
		}
		if (!obs_source_process_filter_begin(source, GS_RGBA, OBS_NO_DIRECT_RENDERING))
			return;
		if (kind == Kind::Alpha) {
			obs_source_process_filter_end(source, active, w, h);
			return;
		}
		vector("pixel_size", 1.0f / (float)w, 1.0f / (float)h);
		if (kind == Kind::Custom) {
			obs_source_process_filter_end(source, effect, w, h);
		} else if (kind == Kind::Shadow) {
			const uint32_t out_w = w + (uint32_t)std::abs(x), out_h = h + (uint32_t)std::abs(y);
			vector("uv_scale", (float)out_w / (float)w, (float)out_h / (float)h);
			vector("image_origin", (float)std::max(-x, 0) / (float)w, (float)std::max(-y, 0) / (float)h);
			vector("shadow_offset", (float)x / (float)w, (float)y / (float)h);
			gs_effect_set_vec4(param("ink"), &colour);
			gs_effect_set_bool(param("hide_image"), hide_image);
			gs_effect_set_bool(param("hide_shadow"), hide_shadow);
			obs_source_process_filter_tech_end(source, effect, out_w, out_h, "Shadow");
		} else if (kind == Kind::Edges) {
			gs_effect_set_vec4(param("ink"), &colour);
			gs_effect_set_float(param("threshold"), threshold);
			obs_source_process_filter_tech_end(source, effect, w, h, "Edges");
		} else {
			vector("steps", (float)hue, (float)value);
			obs_source_process_filter_tech_end(source, effect, w, h, "Toon");
		}
	}
};

template<Kind K> void defaults(obs_data_t *s)
{
	if constexpr (K == Kind::Shadow) {
		obs_data_set_default_int(s, "offset_x", 30);
		obs_data_set_default_int(s, "offset_y", 30);
		obs_data_set_default_int(s, "shadow_color", 0x80000000);
	} else if constexpr (K == Kind::Edges) {
		obs_data_set_default_int(s, "color", 0xff000000);
		obs_data_set_default_double(s, "threshold", 0.3);
	} else if constexpr (K == Kind::Gaussian) {
		obs_data_set_default_int(s, "kernel_size", 3);
		obs_data_set_default_int(s, "pixel_skip", 1);
		obs_data_set_default_double(s, "sigma", 1.0);
	} else if constexpr (K == Kind::Toon) {
		obs_data_set_default_int(s, "hue_steps", 5);
		obs_data_set_default_int(s, "value_steps", 5);
	}
}

template<Kind K> obs_properties_t *properties(void *data)
{
	auto *p = obs_properties_create();
	if constexpr (K == Kind::Custom) {
		obs_properties_add_path(p, "effect_path", obs_module_text("Classic.File"), OBS_PATH_FILE, "*.effect", nullptr);
		obs_properties_add_button2(p, "reload", obs_module_text("Classic.Reload"),
			[](obs_properties_t *, obs_property_t *, void *ptr) {
				if (auto *f = static_cast<Filter *>(ptr)) {
					f->reload = true;
					obs_source_update(f->source, nullptr);
				}
				return true;
			}, data);
		if (auto *f = static_cast<Filter *>(data); f && !f->error.empty())
			obs_properties_add_text(p, "error", f->error.c_str(), OBS_TEXT_INFO);
	} else if constexpr (K == Kind::Shadow) {
		obs_properties_add_color_alpha(p, "shadow_color", obs_module_text("Classic.Colour"));
		obs_properties_add_int(p, "offset_x", obs_module_text("Classic.OffsetX"), -8192, 8192, 1);
		obs_properties_add_int(p, "offset_y", obs_module_text("Classic.OffsetY"), -8192, 8192, 1);
		obs_properties_add_bool(p, "hide_image", obs_module_text("Classic.HideImage"));
		obs_properties_add_bool(p, "hide_shadow", obs_module_text("Classic.HideShadow"));
	} else if constexpr (K == Kind::Edges) {
		obs_properties_add_color_alpha(p, "color", obs_module_text("Classic.Colour"));
		obs_properties_add_float_slider(p, "threshold", obs_module_text("Classic.Threshold"), 0., 1., 0.01);
	} else if constexpr (K == Kind::Toon) {
		obs_properties_add_int_slider(p, "hue_steps", obs_module_text("Classic.Hue"), 1, 10, 1);
		obs_properties_add_int_slider(p, "value_steps", obs_module_text("Classic.Value"), 1, 10, 1);
	} else if constexpr (K == Kind::Gaussian) {
		obs_properties_add_int_slider(p, "kernel_size", obs_module_text("Classic.Kernel"), 1, 31, 2);
		obs_properties_add_float_slider(p, "sigma", obs_module_text("Classic.Sigma"), 1., 10., 0.01);
		obs_properties_add_int_slider(p, "pixel_skip", obs_module_text("Classic.Spacing"), 1, 128, 1);
		auto *mask = obs_properties_add_bool(p, "use_mask", obs_module_text("Classic.UseMask"));
		obs_properties_add_path(p, "mask_image", obs_module_text("Classic.MaskImage"), OBS_PATH_FILE,
					"*.png *.tga *.bmp *.jpg *.jpeg", nullptr);
		obs_properties_add_bool(p, "invert_mask", obs_module_text("Classic.Invert"));
		obs_property_set_modified_callback(mask, [](obs_properties_t *props, obs_property_t *, obs_data_t *s) {
			const bool enabled = obs_data_get_bool(s, "use_mask");
			obs_property_set_visible(obs_properties_get(props, "mask_image"), enabled);
			obs_property_set_visible(obs_properties_get(props, "invert_mask"), enabled);
			return true;
		});
	}
	const char *help[] = {"Classic.Custom.Help", "Classic.Shadow.Help", "Classic.Edges.Help", "Classic.Gaussian.Help", "Classic.Toon.Help", "Classic.Alpha.Help"};
	add_help_line(p, obs_module_text(help[(size_t)K]));
	return p;
}

template<Kind K> void register_one()
{
	const Definition &d = effects[(size_t)K];
	obs_source_info i{};
	i.id = d.id;
	i.type = OBS_SOURCE_TYPE_FILTER;
	i.output_flags = OBS_SOURCE_VIDEO;
	i.type_data = const_cast<Definition *>(&d);
	i.get_name = [](void *data) { return obs_module_text(static_cast<Definition *>(data)->label); };
	i.create = [](obs_data_t *s, obs_source_t *src) -> void * { auto *f = new Filter(src, K); f->update(s); return f; };
	i.destroy = [](void *ptr) { delete static_cast<Filter *>(ptr); };
	i.get_defaults = defaults<K>;
	i.get_properties = properties<K>;
	i.update = [](void *ptr, obs_data_t *s) { static_cast<Filter *>(ptr)->update(s); };
	i.video_render = [](void *ptr, gs_effect_t *) { static_cast<Filter *>(ptr)->render(); };
	if constexpr (K == Kind::Shadow) {
		i.get_width = [](void *ptr) {
			auto &f = *static_cast<Filter *>(ptr); auto *t = obs_filter_get_target(f.source);
			return t ? obs_source_get_base_width(t) + (uint32_t)std::abs(f.x) : 0U;
		};
		i.get_height = [](void *ptr) {
			auto &f = *static_cast<Filter *>(ptr); auto *t = obs_filter_get_target(f.source);
			return t ? obs_source_get_base_height(t) + (uint32_t)std::abs(f.y) : 0U;
		};
	}
	obs_register_source(&i);
}

} // namespace
void register_classic_filters()
{
	register_one<Kind::Custom>();
	register_one<Kind::Shadow>();
	register_one<Kind::Edges>();
	register_one<Kind::Gaussian>();
	register_one<Kind::Toon>();
	register_one<Kind::Alpha>();
}
} // namespace stagekit
