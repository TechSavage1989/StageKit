// What a filter is given to work on, drawn afresh.
//
// OBS's own way (obs_source_process_filter_begin and _end) draws what comes
// before a filter into a texture of the filter's that is only cleared once a
// frame, in the filter's tick: a source drawn twice in one frame gets the
// first drawing both times. A Standby filter standing in for a hidden source
// draws that source two ways in one frame for the whole of a transition (the
// source itself, and as its standby image), and with such a filter after it
// both came out the same. The owner: "most of the transitions don't work.
// they just instantly switch in place". So the filters that may come after a
// Standby filter take their input with this instead: the same drawing, the
// same blending, straight into the filter's own texture, every time.
#pragma once

#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <obs-module.h>

namespace stagekit {

// Draws what comes before `filter` into `into` (w by h, cleared first).
// False when nothing could be drawn. A picture in a wide colour space goes
// OBS's own way, which converts it.
inline bool draw_filter_input(obs_source_t *filter, gs_texrender_t *into, uint32_t w, uint32_t h)
{
	obs_source_t *target = obs_filter_get_target(filter);
	obs_source_t *parent = obs_filter_get_parent(filter);
	if (!target || !parent || !into || !w || !h)
		return false;
	gs_texrender_reset(into);
	const enum gs_color_space wanted[] = {GS_CS_SRGB};
	const bool plain = obs_source_get_color_space(target, 1, wanted) == GS_CS_SRGB;
	if (!plain && !obs_source_process_filter_begin(filter, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
		return false;
	if (!gs_texrender_begin(into, w, h))
		return false;
	vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	gs_blend_state_push();
	if (plain) {
		// As OBS fills a filter's texture.
		gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		const uint32_t flags = obs_source_get_output_flags(parent);
		if (target == parent && !(flags & OBS_SOURCE_CUSTOM_DRAW) && !(flags & OBS_SOURCE_ASYNC))
			obs_source_default_render(target);
		else
			obs_source_video_render(target);
	} else {
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		obs_source_process_filter_end(filter, obs_get_base_effect(OBS_EFFECT_DEFAULT), w, h);
	}
	gs_blend_state_pop();
	gs_texrender_end(into);
	return true;
}

// obs_source_skip_video_filter, unless the filter has been taken off its
// source meanwhile: OBS takes a filter off (from another thread, the
// window's) while the video thread may be drawing it, and its skip then
// reads the parent it no longer has. A filter that draws for long (Scene
// Shadow) and then gives up must skip through this; nothing is drawn then.
inline void skip_filter_input(obs_source_t *filter)
{
	if (obs_filter_get_parent(filter) && obs_filter_get_target(filter))
		obs_source_skip_video_filter(filter);
}

} // namespace stagekit
