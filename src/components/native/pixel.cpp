#include "frame.hpp"

#include <algorithm>
#include <array>

namespace stagekit::native {
namespace {
constexpr std::array<const char *, 3> channels{"redcount", "greencount", "bluecount"};
constexpr std::array<const char *, 3> labels{"Native.RedLevels", "Native.GreenLevels", "Native.BlueLevels"};

struct PixelArt {
	obs_source_t *source;
	Effect shader{"effects/stagekit-pixel.effect"};
	int block = 1, pattern = 2;
	float spread = 0;
	std::array<float, 3> levels{};

	PixelArt(obs_data_t *settings, obs_source_t *context) : source(context) { update(settings); }
	void update(obs_data_t *s)
	{
		block = 1 << std::clamp<int>(int(obs_data_get_int(s, "downscales")), 0, 8);
		pattern = 2 << std::clamp<int>(int(obs_data_get_int(s, "bayerlevel")), 0, 2);
		spread = float(std::clamp(obs_data_get_double(s, "ditherspread"), 0.0, 1.0));
		for (size_t i = 0; i < channels.size(); ++i)
			levels[i] = float(std::clamp<long long>(obs_data_get_int(s, channels[i]), 1, 16));
	}
	void render()
	{
		const Size size = size_of_target(source);
		if (!size || !shader.value) {
			obs_source_skip_video_filter(source);
			return;
		}
		if (!obs_source_process_filter_begin(source, GS_RGBA, OBS_NO_DIRECT_RENDERING))
			return;
		shader.pair("grid", float(std::max(1u, size.width / block)), float(std::max(1u, size.height / block)));
		shader.four("palette", levels[0], levels[1], levels[2], spread);
		shader.scalar("tile", float(pattern));
		obs_source_process_filter_end(source, shader.value, size.width, size.height);
	}
	static void defaults(obs_data_t *s)
	{
		for (const char *key : channels)
			obs_data_set_default_int(s, key, 16);
	}
	static obs_properties_t *properties(void *)
	{
		auto *p = obs_properties_create();
		obs_properties_add_int_slider(p, "downscales", text("Native.PixelSize"), 0, 8, 1);
		auto *d = obs_properties_create();
		obs_properties_add_float_slider(d, "ditherspread", text("Native.DitherAmount"), 0, 1, .01);
		obs_properties_add_int_slider(d, "bayerlevel", text("Native.DitherPattern"), 0, 2, 1);
		for (size_t i = 0; i < channels.size(); ++i)
			obs_properties_add_int_slider(d, channels[i], text(labels[i]), 1, 16, 1);
		obs_properties_add_group(p, "dither", text("Native.Palette"), OBS_GROUP_NORMAL, d);
		return p;
	}
};
void load()
{
	auto info = video_filter<PixelArt>("stagekit_port_pixel_art_plugin", "Ports.PixelArt");
	obs_register_source(&info);
}
} // namespace
const Component component{load};
} // namespace stagekit::native
