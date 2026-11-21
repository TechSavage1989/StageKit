// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace stagekit {

struct AlphaBounds {
	uint32_t left, top, right, bottom;
};

// Inclusive bounds of RGBA pixels above the visibility threshold. Once a
// column is inside known bounds, its pixels cannot expand them horizontally.
inline AlphaBounds visible_alpha_bounds(const uint8_t *data, size_t stride, uint32_t w, uint32_t h)
{
	AlphaBounds bounds{w, h, 0, 0};
	if (!w || !h)
		return bounds;
	const auto row = [&](uint32_t y) {
		return data + (size_t)y * stride;
	};
	const auto visible = [](const uint8_t *line, uint32_t x) {
		return line[(size_t)x * 4 + 3] > 12;
	};
	// Opposite corners alone establish all four edges, even with holes.
	if ((visible(row(0), 0) && visible(row(h - 1), w - 1)) || (visible(row(0), w - 1) && visible(row(h - 1), 0)))
		return {0, 0, w - 1, h - 1};
	const auto first = [&](const uint8_t *line, uint32_t limit) {
		uint32_t x = 0;
		while (x < limit && !visible(line, x))
			x++;
		return x;
	};
	for (uint32_t y = 0; y < h; y++) {
		const uint32_t x = first(row(y), w);
		if (x < w) {
			bounds = {x, y, x, y};
			break;
		}
	}
	if (bounds.top == h)
		return bounds;
	for (uint32_t y = h - 1; y > bounds.top; y--) {
		const uint32_t x = first(row(y), w);
		if (x < w) {
			bounds.bottom = y;
			bounds.left = std::min(bounds.left, x);
			bounds.right = std::max(bounds.right, x);
			break;
		}
	}
	for (uint32_t y = bounds.top; y <= bounds.bottom && (bounds.left || bounds.right < w - 1); y++) {
		const uint8_t *line = row(y);
		bounds.left = first(line, bounds.left);
		for (uint32_t x = w - 1; x > bounds.right; x--) {
			if (visible(line, x)) {
				bounds.right = x;
				break;
			}
		}
	}
	return bounds;
}

} // namespace stagekit
