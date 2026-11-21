// SPDX-License-Identifier: GPL-2.0-or-later
#include "../src/common/alpha-bounds.hpp"

#include <cstdio>
#include <random>
#include <vector>

using stagekit::AlphaBounds;

AlphaBounds reference(const uint8_t *data, size_t stride, uint32_t w, uint32_t h)
{
	AlphaBounds b{w, h, 0, 0};
	for (uint32_t y = 0; y < h; ++y) {
		for (uint32_t x = 0; x < w; ++x) {
			if (data[y * stride + x * 4 + 3] <= 12)
				continue;
			b.left = std::min(b.left, x);
			b.right = std::max(b.right, x);
			b.top = std::min(b.top, y);
			b.bottom = std::max(b.bottom, y);
		}
	}
	return b;
}

bool matches(const std::vector<uint8_t> &pixels, size_t stride, uint32_t w, uint32_t h)
{
	const auto a = reference(pixels.data(), stride, w, h);
	const auto b = stagekit::visible_alpha_bounds(pixels.data(), stride, w, h);
	return a.left == b.left && a.right == b.right && a.top == b.top && a.bottom == b.bottom;
}

int main()
{
	std::vector<uint8_t> pixels;
	for (uint32_t w = 0; w <= 4; ++w) {
		for (uint32_t h = 0; h <= 4; ++h) {
			const size_t stride = w * 4 + 12;
			pixels.assign(stride * h, 255);
			for (unsigned mask = 0; mask < (1u << (w * h)); ++mask) {
				for (uint32_t y = 0; y < h; ++y)
					for (uint32_t x = 0; x < w; ++x)
						pixels[y * stride + x * 4 + 3] = mask & (1u << (y * w + x)) ? 13 : 12;
				if (!matches(pixels, stride, w, h)) {
					std::fprintf(stderr, "alpha bounds differ for %ux%u mask %u\n", w, h, mask);
					return 1;
				}
			}
		}
	}
	std::mt19937 rng(8137);
	for (unsigned i = 0; i < 300; ++i) {
		const uint32_t w = 1 + rng() % 240, h = 1 + rng() % 160;
		const size_t stride = 4 * w + rng() % 32;
		pixels.assign(stride * h, 255);
		const unsigned density = 1 + i % 95;
		for (uint32_t y = 0; y < h; ++y)
			for (uint32_t x = 0; x < w; ++x)
				pixels[y * stride + x * 4 + 3] = rng() % 100 < density ? uint8_t(rng()) : 0;
		if (!matches(pixels, stride, w, h))
			return 1;
	}
	std::puts("PASS: visibility bounds match a full scan for every small mask and 300 padded frames");
	return 0;
}
