/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <obs.h>

#include <array>

namespace stagekit {

// A self-contained dual-Kawase blur for features that just need "blur this
// texture by about N pixels". All methods must be called inside the
// graphics context. Works on raw colour values (no sRGB conversion).
class KawaseBlur {
public:
	KawaseBlur();
	~KawaseBlur();
	KawaseBlur(const KawaseBlur &) = delete;
	KawaseBlur &operator=(const KawaseBlur &) = delete;

	// strength is on the same scale as Ramp Blur's strength. Returns the
	// input unchanged when strength is negligible. The result is owned by
	// this object and valid until the next call.
	gs_texture_t *blur(gs_texture_t *input, uint32_t w, uint32_t h, double strength);

private:
	static constexpr int kLevels = 7;
	void pass(gs_texrender_t *dst, gs_texture_t *src, uint32_t w, uint32_t h, const char *technique);

	gs_effect_t *effect_ = nullptr;
	gs_eparam_t *p_image_ = nullptr;
	gs_eparam_t *p_texel_ = nullptr;
	gs_eparam_t *p_offset_ = nullptr;
	gs_texrender_t *out_ = nullptr;
	std::array<gs_texrender_t *, kLevels> down_{};
	std::array<gs_texrender_t *, kLevels> up_{};
};

} // namespace stagekit
