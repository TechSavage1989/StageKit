#include "frame.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace stagekit::native {
namespace {
constexpr const char *type_id = "stagekit_port_3d_effect_filter";
using Match = float (*)(obs_source_t *, obs_source_t **);
Match match = nullptr;
struct Pose {
	std::array<double, 9> values{90, 0, 0, 0, 0, 0, 0, 100, 100};
	bool flat = false;
};
constexpr std::array<const char *, 9> keys{"fov", "rot_x", "rot_y", "rot_z", "pos_x", "pos_y", "pos_z", "scale_x", "scale_y"};
constexpr std::array<const char *, 9> captions{"Native.Fov", "Native.RotateX", "Native.RotateY", "Native.RotateZ", "Native.PositionX",
					      "Native.PositionY", "Native.PositionZ", "Native.ScaleX", "Native.ScaleY"};
Pose read_pose(obs_data_t *s)
{
	Pose pose;
	pose.flat = obs_data_get_int(s, "mode") == 1;
	for (size_t i = 0; i < keys.size(); ++i)
		pose.values[i] = obs_data_get_double(s, keys[i]);
	return pose;
}
struct Plane {
	obs_source_t *source;
	Effect shader{"effects/stagekit-plane.effect"};
	Pose pose;
	Plane(obs_data_t *s, obs_source_t *context) : source(context) { update(s); }
	void update(obs_data_t *s) { pose = read_pose(s); }
	static void defaults(obs_data_t *s)
	{
		const Pose p;
		for (size_t i = 0; i < keys.size(); ++i)
			obs_data_set_default_double(s, keys[i], p.values[i]);
	}
	static obs_properties_t *properties(void *)
	{
		auto *p = obs_properties_create();
		auto *mode = obs_properties_add_list(p, "mode", text("Native.Projection"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(mode, text("Native.Perspective"), 0);
		obs_property_list_add_int(mode, text("Native.Orthographic"), 1);
		for (size_t i = 0; i < keys.size(); ++i) {
			if (!i)
				obs_properties_add_float_slider(p, keys[i], text(captions[i]), .1, 180, .01);
			else if (i <= 3)
				obs_properties_add_float_slider(p, keys[i], text(captions[i]), -180, 180, .01);
			else if (i <= 6)
				obs_properties_add_float(p, keys[i], text(captions[i]), -100000, 100000, 1);
			else
				obs_properties_add_float(p, keys[i], text(captions[i]), 0, 10000, 1);
		}
		return p;
	}
	void render()
	{
		const Size size = size_of_target(source);
		if (!size || !shader.value) {
			obs_source_skip_video_filter(source);
			return;
		}
		auto p = pose.values;
		obs_source_t *other = nullptr;
		const float blend = match ? std::clamp(match(source, &other), 0.0f, 1.0f) : 0;
		if (blend > 0 && other != source) {
			Pose end;
			if (other && std::strcmp(obs_source_get_unversioned_id(other), type_id) == 0) {
				auto *s = obs_source_get_settings(other);
				end = read_pose(s);
				obs_data_release(s);
			}
			for (size_t i = 0; i < p.size(); ++i)
				p[i] += blend * (end.values[i] - p[i]);
		}
		const auto space = gs_get_color_space();
		if (!obs_source_process_filter_begin_with_color_space(source, gs_get_format_from_space(space), space, OBS_NO_DIRECT_RENDERING))
			return;
		constexpr double radians = 0.017453292519943295;
		const double sx = std::sin(p[1] * radians), cx = std::cos(p[1] * radians);
		const double sy = std::sin(p[2] * radians), cy = std::cos(p[2] * radians);
		const double sz = std::sin(p[3] * radians), cz = std::cos(p[3] * radians);
		const double aspect = double(size.width) / size.height;
		shader.four("right_axis", float(cy * cz), float(cx * sz + sx * sy * cz), float(sx * sz - cx * sy * cz), float(aspect));
		shader.four("down_axis", float(-cy * sz), float(cx * cz - sx * sy * sz), float(sx * cz + cx * sy * sz), 1);
		shader.four("placement", float(2 * p[4] / size.height), float(2 * p[5] / size.height),
			    float(p[6] / (double(size.width) + size.height) - 1.0), float(aspect));
		shader.four("lens", float(std::tan(std::clamp(p[0], .1, 179.99) * radians / 2)), pose.flat ? 1.0f : 0.0f,
			    float(p[7] / 100), float(p[8] / 100));
		obs_source_process_filter_end(source, shader.value, size.width, size.height);
	}
};
void load()
{
	auto info = video_filter<Plane>(type_id, "Ports.3DEffect");
	info.output_flags |= OBS_SOURCE_SRGB;
	obs_register_source(&info);
}
void connect_move()
{
	calldata_t call{};
	calldata_set_string(&call, "filter_id", type_id);
	if (proc_handler_call(obs_get_proc_handler(), "stagekit_port_move_get_transition_filter_function", &call))
		match = reinterpret_cast<Match>(calldata_ptr(&call, "callback"));
	calldata_free(&call);
}
} // namespace
const Component component{load, nullptr, connect_move};
} // namespace stagekit::native
