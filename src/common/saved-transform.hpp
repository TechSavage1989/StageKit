#pragma once

#include <obs.h>
#include <graphics/vec2.h>

#include <algorithm>
#include <cmath>

namespace stagekit {

// A scene item's full saved transform.
// The item's own values (inside a group, the group's coordinates), so the
// same item gets them back as it had them.
struct Xf {
	double x = 0, y = 0, sx = 1, sy = 1, rot = 0, bw = 0, bh = 0;
	long long align = 0, btype = 0, balign = 0;
	long long cl = 0, ct = 0, cr = 0, cb = 0;
	bool has_bounds_crop = false, bounds_crop = false;
};

inline Xf xf_of(obs_sceneitem_t *item, bool include_bounds_crop = false)
{
	Xf x;
	vec2 v;
	obs_sceneitem_get_pos(item, &v);
	x.x = v.x;
	x.y = v.y;
	obs_sceneitem_get_scale(item, &v);
	x.sx = v.x;
	x.sy = v.y;
	x.rot = obs_sceneitem_get_rot(item);
	x.align = obs_sceneitem_get_alignment(item);
	x.btype = obs_sceneitem_get_bounds_type(item);
	x.balign = obs_sceneitem_get_bounds_alignment(item);
	obs_sceneitem_get_bounds(item, &v);
	x.bw = v.x;
	x.bh = v.y;
	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	x.cl = crop.left;
	x.ct = crop.top;
	x.cr = crop.right;
	x.cb = crop.bottom;
#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(30, 1, 0)
	x.has_bounds_crop = include_bounds_crop;
	if (include_bounds_crop)
		x.bounds_crop = obs_sceneitem_get_bounds_crop(item);
#else
	(void)include_bounds_crop;
#endif
	return x;
}

inline void xf_set(obs_sceneitem_t *item, const Xf &x, bool framing = true)
{
	obs_sceneitem_defer_update_begin(item);
	vec2 v;
	vec2_set(&v, (float)x.x, (float)x.y);
	obs_sceneitem_set_pos(item, &v);
	vec2_set(&v, (float)x.sx, (float)x.sy);
	obs_sceneitem_set_scale(item, &v);
	obs_sceneitem_set_rot(item, (float)x.rot);
	if (!framing) {
		obs_sceneitem_defer_update_end(item);
		return;
	}
#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(30, 1, 0)
	if (x.has_bounds_crop)
		obs_sceneitem_set_bounds_crop(item, x.bounds_crop);
#endif
	obs_sceneitem_set_alignment(item, (uint32_t)x.align);
	obs_sceneitem_set_bounds_type(item, (obs_bounds_type)x.btype);
	obs_sceneitem_set_bounds_alignment(item, (uint32_t)x.balign);
	vec2_set(&v, (float)x.bw, (float)x.bh);
	obs_sceneitem_set_bounds(item, &v);
	obs_sceneitem_crop crop = {(int)x.cl, (int)x.ct, (int)x.cr, (int)x.cb};
	obs_sceneitem_set_crop(item, &crop);
	obs_sceneitem_defer_update_end(item);
}

// Part-way from a to b. The kinds (alignment, bounds type) are b's from the
// start; when the bounds type changes, the bounds size is b's at once too.
inline Xf xf_between(const Xf &a, const Xf &b, double t)
{
	auto mix = [t](double p, double q) {
		return p + (q - p) * t;
	};
	auto mixi = [&](long long p, long long q) {
		return (long long)std::llround(mix((double)p, (double)q));
	};
	Xf x = b;
	x.x = mix(a.x, b.x);
	x.y = mix(a.y, b.y);
	x.sx = mix(a.sx, b.sx);
	x.sy = mix(a.sy, b.sy);
	x.rot = mix(a.rot, b.rot);
	if (a.btype == b.btype) {
		x.bw = mix(a.bw, b.bw);
		x.bh = mix(a.bh, b.bh);
	}
	x.cl = std::max(0LL, mixi(a.cl, b.cl));
	x.ct = std::max(0LL, mixi(a.ct, b.ct));
	x.cr = std::max(0LL, mixi(a.cr, b.cr));
	x.cb = std::max(0LL, mixi(a.cb, b.cb));
	return x;
}

inline obs_data_t *xf_data(const Xf &x)
{
	obs_data_t *d = obs_data_create();
	obs_data_set_double(d, "x", x.x);
	obs_data_set_double(d, "y", x.y);
	obs_data_set_double(d, "sx", x.sx);
	obs_data_set_double(d, "sy", x.sy);
	obs_data_set_double(d, "rot", x.rot);
	obs_data_set_int(d, "align", x.align);
	obs_data_set_int(d, "btype", x.btype);
	obs_data_set_int(d, "balign", x.balign);
	obs_data_set_double(d, "bw", x.bw);
	obs_data_set_double(d, "bh", x.bh);
	obs_data_set_int(d, "cl", x.cl);
	obs_data_set_int(d, "ct", x.ct);
	obs_data_set_int(d, "cr", x.cr);
	obs_data_set_int(d, "cb", x.cb);
	if (x.has_bounds_crop)
		obs_data_set_bool(d, "bounds_crop", x.bounds_crop);
	return d;
}

inline Xf xf_read(obs_data_t *d)
{
	Xf x;
	x.x = obs_data_get_double(d, "x");
	x.y = obs_data_get_double(d, "y");
	x.sx = obs_data_get_double(d, "sx");
	x.sy = obs_data_get_double(d, "sy");
	x.rot = obs_data_get_double(d, "rot");
	x.align = obs_data_get_int(d, "align");
	x.btype = obs_data_get_int(d, "btype");
	x.balign = obs_data_get_int(d, "balign");
	x.bw = obs_data_get_double(d, "bw");
	x.bh = obs_data_get_double(d, "bh");
	x.cl = obs_data_get_int(d, "cl");
	x.ct = obs_data_get_int(d, "ct");
	x.cr = obs_data_get_int(d, "cr");
	x.cb = obs_data_get_int(d, "cb");
	x.has_bounds_crop = obs_data_has_user_value(d, "bounds_crop");
	x.bounds_crop = obs_data_get_bool(d, "bounds_crop");
	return x;
}

} // namespace stagekit
