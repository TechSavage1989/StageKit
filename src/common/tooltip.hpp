/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// A control's tooltip, broken into lines. OBS shows a long description as
// one line as wide as its text: the owner, "all tooltips in the plugin are
// hard to read because they are just one really long line of text". Every
// tooltip StageKit sets goes through set_tip, never through
// obs_property_set_long_description itself.

#pragma once

#include <obs.h>

#include <cstddef>
#include <string>

namespace stagekit {

// The longest line a tooltip is broken to, in characters.
inline constexpr size_t kTipWidth = 64;

// How many characters (not bytes) a UTF-8 text holds.
inline size_t tip_chars(const std::string &text)
{
	size_t n = 0;
	for (unsigned char c : text)
		if ((c & 0xC0) != 0x80)
			n++;
	return n;
}

// The text broken at spaces into lines of at most `width` characters. A
// break already in it is kept; a word longer than a line is a line of its
// own, never split; spaces at a break are dropped, nothing else is.
inline std::string wrap_tip(const std::string &text, size_t width = kTipWidth)
{
	std::string out;
	size_t start = 0;
	for (;;) {
		const size_t end = text.find('\n', start);
		const std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
		std::string cur;
		size_t cur_n = 0;
		size_t i = 0;
		bool first = true;
		while (i <= line.size()) {
			// The next word, and how many spaces came before it.
			size_t spaces = 0;
			while (i < line.size() && line[i] == ' ') {
				i++;
				spaces++;
			}
			if (i >= line.size())
				break;
			size_t j = line.find(' ', i);
			if (j == std::string::npos)
				j = line.size();
			const std::string word = line.substr(i, j - i);
			const size_t word_n = tip_chars(word);
			if (first) {
				// Spaces at the start of a line are kept (an indented list).
				cur = std::string(spaces, ' ') + word;
				cur_n = spaces + word_n;
				first = false;
			} else if (cur_n + spaces + word_n <= width) {
				cur += std::string(spaces, ' ') + word;
				cur_n += spaces + word_n;
			} else {
				out += cur;
				out += '\n';
				cur = word;
				cur_n = word_n;
			}
			i = j;
		}
		out += cur;
		if (end == std::string::npos)
			break;
		out += '\n';
		start = end + 1;
	}
	return out;
}

inline void set_tip(obs_property_t *p, const char *text)
{
	if (!text) {
		obs_property_set_long_description(p, nullptr);
		return;
	}
	obs_property_set_long_description(p, wrap_tip(text).c_str());
}

inline void set_tip(obs_property_t *p, const std::string &text)
{
	obs_property_set_long_description(p, wrap_tip(text).c_str());
}

} // namespace stagekit
