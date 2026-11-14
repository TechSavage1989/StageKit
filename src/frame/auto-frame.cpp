/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Auto Frame: gives a capture a fixed-size frame and fits whatever the
// capture currently outputs inside it.
//
// A game or emulator capture changes size whenever the program changes
// resolution, which normally means one hand-built scene per picture shape
// plus automation to switch between them. With this filter the source's
// size never changes, so its place in the scene, its shadow and anything
// linked to it stay put, while the picture inside glides to its new fit.

#include "auto-frame.hpp"

#include "../capture/auto-capture.hpp"
#include "../common/easing.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-util.hpp"

#include <obs-module.h>
#include <graphics/vec4.h>
#include <plugin-support.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace stagekit {

namespace {

enum class FitMode : int {
	Fit = 0,     // whole picture visible, bars where the shapes differ
	Fill = 1,    // frame covered, picture cropped where the shapes differ
	Whole = 2,   // largest whole-number scale that fits (crisp pixel art)
	Stretch = 3, // picture stretched to the frame
};

enum class Shape : int {
	Source = 0, // use the capture's own width : height
	R4x3 = 1,
	R16x9 = 2,
	R8x7 = 3,
	R3x2 = 4,
	R1x1 = 5,
	R5x4 = 6,
	R16x10 = 7,
	R10x9 = 8,
	Custom = 9,
};

// Removing black bars a game or emulator draws around its picture.
enum class Trim : int {
	Off = 0,
	Automatic = 1,
	Manual = 2,
};

struct Settings {
	// The frame is the picture itself: no fixed size, no room around it.
	bool follow = false;
	uint32_t frame_w = 1920;
	uint32_t frame_h = 1080;
	FitMode fit = FitMode::Fit;
	Shape shape = Shape::Source;
	double custom_w = 4.0;
	double custom_h = 3.0;
	int align_x = 1; // 0 left, 1 centre, 2 right
	int align_y = 1; // 0 top, 1 centre, 2 bottom
	bool sharp = false;
	Trim trim = Trim::Automatic;
	double trim_x = 0.0; // manual: pixels off the left and off the right
	double trim_y = 0.0; // manual: pixels off the top and off the bottom
	double trim_wait_s = 3.0;
	// Automatic trimming can be told the picture's shape instead of looking
	// for it: by a Picture Shape key (force_ratio), or by a rule naming the
	// program being captured (rules: text found in the program -> shape).
	double force_ratio = 0.0;
	std::vector<std::pair<std::string, double>> rules;
	bool known_programs = true; // use the built-in shapes for common emulators
	double glide_s = 0.35;
	EaseCurve curve = EaseCurve::Cubic;
	EaseMode mode = EaseMode::InOut;
};

struct Rect {
	double x = 0, y = 0, w = 0, h = 0;
};

double shape_ratio(const Settings &s, uint32_t w, uint32_t h)
{
	switch (s.shape) {
	case Shape::R4x3:
		return 4.0 / 3.0;
	case Shape::R16x9:
		return 16.0 / 9.0;
	case Shape::R8x7:
		return 8.0 / 7.0;
	case Shape::R3x2:
		return 3.0 / 2.0;
	case Shape::R1x1:
		return 1.0;
	case Shape::R5x4:
		return 5.0 / 4.0;
	case Shape::R16x10:
		return 16.0 / 10.0;
	case Shape::R10x9:
		return 10.0 / 9.0;
	case Shape::Custom:
		if (s.custom_w > 0.0 && s.custom_h > 0.0)
			return s.custom_w / s.custom_h;
		break;
	case Shape::Source:
		break;
	}
	return (double)w / (double)h;
}

// Where the picture goes inside the frame.
Rect target_rect(const Settings &s, uint32_t w, uint32_t h)
{
	const double fw = s.frame_w, fh = s.frame_h;
	const double ratio = shape_ratio(s, w, h);
	// The picture's size in display pixels at 1x: its height, and the
	// width its shape calls for.
	const double base_h = h;
	const double base_w = h * ratio;

	Rect r;
	switch (s.fit) {
	case FitMode::Stretch:
		r.w = fw;
		r.h = fh;
		break;
	case FitMode::Fill: {
		const double k = std::max(fw / base_w, fh / base_h);
		r.w = base_w * k;
		r.h = base_h * k;
		break;
	}
	case FitMode::Whole: {
		const double k = std::max(1.0, std::floor(std::min(fw / base_w, fh / base_h)));
		r.w = base_w * k;
		r.h = base_h * k;
		break;
	}
	case FitMode::Fit:
	default: {
		const double k = std::min(fw / base_w, fh / base_h);
		r.w = base_w * k;
		r.h = base_h * k;
		break;
	}
	}
	r.x = (fw - r.w) * (s.align_x * 0.5);
	r.y = (fh - r.h) * (s.align_y * 0.5);
	return r;
}

bool same(const Rect &a, const Rect &b)
{
	return std::fabs(a.x - b.x) < 0.01 && std::fabs(a.y - b.y) < 0.01 && std::fabs(a.w - b.w) < 0.01 &&
	       std::fabs(a.h - b.h) < 0.01;
}

std::string lower(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return text;
}

// "4:3", "64:49" or "1.306" -> a width / height ratio, or 0.
double parse_ratio(const std::string &text)
{
	try {
		const size_t colon = text.find(':');
		if (colon == std::string::npos)
			return std::max(0.0, std::stod(text));
		const double a = std::stod(text.substr(0, colon)), b = std::stod(text.substr(colon + 1));
		return a > 0.0 && b > 0.0 ? a / b : 0.0;
	} catch (...) {
		return 0.0;
	}
}

// A rule is written "snes9x = 4:3": text to look for in the captured
// program's name or window title, then the shape of its picture.
bool parse_rule(const std::string &line, std::string &program, double &ratio)
{
	const size_t equals = line.rfind('=');
	if (equals == std::string::npos)
		return false;
	auto trimmed = [](std::string t) {
		const char *space = " \t";
		const size_t a = t.find_first_not_of(space), b = t.find_last_not_of(space);
		return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
	};
	program = lower(trimmed(line.substr(0, equals)));
	ratio = parse_ratio(trimmed(line.substr(equals + 1)));
	return !program.empty() && ratio > 0.0;
}

// The picture shape common emulators show with their usual settings. It is
// only where the picture starts when that program appears: what is then
// seen on screen corrects it, because the shape depends on the emulator's
// own settings. A shape the owner saved for a program is obeyed as it is.
// Emulators that play several consoles (RetroArch, Mednafen, Dolphin,
// PCSX2) are left out on purpose: their shape depends on the game.
const struct {
	const char *program;
	const char *shape;
} kKnownPrograms[] = {
	{"snes9x", "8:7"},  {"bsnes", "64:49"},   {"higan", "64:49"},  {"mgba", "3:2"},      {"visualboy", "3:2"},
	{"vba-m", "3:2"},   {"project64", "4:3"}, {"mupen64", "4:3"},  {"simple64", "4:3"},  {"duckstation", "4:3"},
	{"epsxe", "4:3"},   {"fusion", "4:3"},    {"blastem", "4:3"},  {"flycast", "4:3"},   {"redream", "4:3"},
	{"ppsspp", "16:9"}, {"bgb", "10:9"},      {"sameboy", "10:9"}, {"gambatte", "10:9"}, {"melonds", "2:3"},
	{"desmume", "2:3"}, {"citra", "5:6"},     {"lime3ds", "5:6"},  {"azahar", "5:6"},    {"borked3ds", "5:6"},
};

// The shapes games and emulators output. 64:49 is SNES/NES pixels (8:7 each)
// shown on a 4:3 screen.
const double kPictureShapes[] = {16.0 / 9.0, 16.0 / 10.0, 3.0 / 2.0, 4.0 / 3.0, 64.0 / 49.0, 5.0 / 4.0, 8.0 / 7.0,
				 10.0 / 9.0, 16.0 / 15.0, 1.0,
				 // Two-screen handhelds: DS screens stacked, side by side; 3DS top
				 // screen, both stacked, both side by side.
				 2.0 / 3.0, 8.0 / 3.0, 5.0 / 3.0, 5.0 / 6.0, 3.0 / 1.0,
				 // Other handhelds: PSP, Lynx, WonderSwan, Neo Geo Pocket, Virtual Boy.
				 30.0 / 17.0, 80.0 / 51.0, 14.0 / 9.0, 20.0 / 19.0, 12.0 / 7.0,
				 // Upright and ultra-wide screens.
				 3.0 / 4.0, 9.0 / 16.0, 2.0 / 1.0, 21.0 / 9.0, 64.0 / 27.0, 32.0 / 9.0};

// The listed shape a measured ratio is, when it is within a pixel or two of one.
double snap_shape(double ratio)
{
	for (double shape : kPictureShapes)
		if (std::fabs(ratio - shape) <= shape * 0.006)
			return shape;
	return ratio;
}

// "64:49" for a ratio that has a usual name, otherwise the number.
std::string ratio_name(double ratio)
{
	for (const char *name : {"16:9", "16:10", "3:2", "4:3", "64:49", "5:4", "8:7", "10:9", "16:15", "1:1"})
		if (std::fabs(parse_ratio(name) - ratio) < 0.002)
			return name;
	char text[32];
	snprintf(text, sizeof(text), "%.4f", ratio);
	return text;
}

// What each program's picture was seen to be, kept between sessions as
// "game.exe = 16:9, multi.exe = varies". A program that has shown more than
// one shape has nothing to start from and is kept as -1.
std::map<std::string, double> parse_learned(const std::string &text)
{
	std::map<std::string, double> out;
	size_t start = 0;
	while (start <= text.size()) {
		size_t end = text.find(',', start);
		if (end == std::string::npos)
			end = text.size();
		const std::string entry = text.substr(start, end - start);
		const size_t equals = entry.rfind('=');
		if (equals != std::string::npos) {
			auto trimmed = [](std::string t) {
				const char *space = " \t";
				const size_t a = t.find_first_not_of(space), b = t.find_last_not_of(space);
				return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
			};
			const std::string program = lower(trimmed(entry.substr(0, equals)));
			const std::string shape = trimmed(entry.substr(equals + 1));
			const double ratio = shape == "varies" ? -1.0 : parse_ratio(shape);
			if (!program.empty() && ratio != 0.0)
				out[program] = ratio;
		}
		start = end + 1;
	}
	return out;
}

std::string learned_text(const std::map<std::string, double> &learned)
{
	std::string text;
	for (const auto &[program, ratio] : learned)
		text += (text.empty() ? "" : ", ") + program + " = " + (ratio < 0.0 ? "varies" : ratio_name(ratio));
	return text;
}

// Rules as one line of text: "snes9x = 4:3, mgba = 3:2".
void parse_rules_text(const std::string &text, std::vector<std::pair<std::string, double>> &out)
{
	std::string line;
	auto flush = [&] {
		std::string program;
		double ratio = 0.0;
		if (parse_rule(line, program, ratio))
			out.emplace_back(program, ratio);
		line.clear();
	};
	for (char c : text) {
		if (c == ',' || c == ';' || c == '\n')
			flush();
		else
			line += c;
	}
	flush();
}

class AutoFrame {
public:
	AutoFrame(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		obs_enter_graphics();
		tr_input_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		tr_frame_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		tr_probe_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		gs_sampler_info point = {};
		point.filter = GS_FILTER_POINT;
		point.address_u = GS_ADDRESS_CLAMP;
		point.address_v = GS_ADDRESS_CLAMP;
		point.address_w = GS_ADDRESS_CLAMP;
		point.max_anisotropy = 1;
		point_sampler_ = gs_samplerstate_create(&point);
		obs_leave_graphics();
		update(settings);

		proc_handler_add(
			obs_source_get_proc_handler(source_),
			"void get_picture(out float x, out float y, out float width, out float height)",
			[](void *data, calldata_t *cd) {
				auto *self = static_cast<AutoFrame *>(data);
				// While a Standby Picture stands in for the capture there
				// is no game picture: the rectangle kept from the last
				// game would cut the standby picture to that game's shape.
				if (self->standby_showing()) {
					calldata_set_float(cd, "x", 0.0);
					calldata_set_float(cd, "y", 0.0);
					calldata_set_float(cd, "width", 0.0);
					calldata_set_float(cd, "height", 0.0);
					return;
				}
				std::lock_guard lock(self->mutex_);
				calldata_set_float(cd, "x", self->current_.x);
				calldata_set_float(cd, "y", self->current_.y);
				calldata_set_float(cd, "width", self->current_.w);
				calldata_set_float(cd, "height", self->current_.h);
			},
			this);
		proc_handler_add(
			obs_source_get_proc_handler(source_), "void set_picture_shape(in float ratio)",
			[](void *data, calldata_t *cd) {
				static_cast<AutoFrame *>(data)->set_forced_ratio(calldata_float(cd, "ratio"));
			},
			this);
		proc_handler_add(
			obs_source_get_proc_handler(source_), "void measure_now()",
			[](void *data, calldata_t *) { static_cast<AutoFrame *>(data)->measure_now(); }, this);
		proc_handler_add(
			obs_source_get_proc_handler(source_), "void get_trim(out float x, out float y)",
			[](void *data, calldata_t *cd) {
				auto *self = static_cast<AutoFrame *>(data);
				std::lock_guard lock(self->mutex_);
				calldata_set_float(cd, "x", self->crop_goal_.x);
				calldata_set_float(cd, "y", self->crop_goal_.y);
			},
			this);
	}

	~AutoFrame()
	{
		obs_enter_graphics();
		gs_texrender_destroy(tr_input_);
		gs_texrender_destroy(tr_frame_);
		gs_texrender_destroy(tr_probe_);
		gs_stagesurface_destroy(stage_[0]);
		gs_stagesurface_destroy(stage_[1]);
		gs_samplerstate_destroy(point_sampler_);
		obs_leave_graphics();
	}

	void update(obs_data_t *settings)
	{
		Settings s;
		s.follow = obs_data_get_int(settings, "frame_mode") == 1;
		s.frame_w = (uint32_t)std::clamp<long long>(obs_data_get_int(settings, "frame_width"), 2, 16384);
		s.frame_h = (uint32_t)std::clamp<long long>(obs_data_get_int(settings, "frame_height"), 2, 16384);
		s.fit = static_cast<FitMode>(obs_data_get_int(settings, "fit"));
		s.shape = static_cast<Shape>(obs_data_get_int(settings, "shape"));
		s.custom_w = obs_data_get_double(settings, "custom_w");
		s.custom_h = obs_data_get_double(settings, "custom_h");
		s.align_x = (int)std::clamp<long long>(obs_data_get_int(settings, "align_x"), 0, 2);
		s.align_y = (int)std::clamp<long long>(obs_data_get_int(settings, "align_y"), 0, 2);
		s.sharp = obs_data_get_bool(settings, "sharp");
		s.trim = static_cast<Trim>(obs_data_get_int(settings, "trim"));
		s.trim_x = std::max(0.0, obs_data_get_double(settings, "trim_x"));
		s.trim_y = std::max(0.0, obs_data_get_double(settings, "trim_y"));
		s.trim_wait_s = std::max(0.0, obs_data_get_double(settings, "trim_wait"));
		s.force_ratio = std::max(0.0, obs_data_get_double(settings, "force_ratio"));
		s.known_programs = obs_data_get_bool(settings, "known_programs");
		parse_rules_text(obs_data_get_string(settings, "rules_text"), s.rules);
		// 0.1.0-beta6 to beta9 kept the rules in a list box.
		obs_data_array_t *rules = obs_data_get_array(settings, "rules");
		for (size_t i = 0; i < obs_data_array_count(rules); i++) {
			obs_data_t *entry = obs_data_array_item(rules, i);
			std::string program;
			double ratio = 0.0;
			if (parse_rule(obs_data_get_string(entry, "value"), program, ratio))
				s.rules.emplace_back(program, ratio);
			obs_data_release(entry);
		}
		obs_data_array_release(rules);
		s.glide_s = std::max<long long>(0, obs_data_get_int(settings, "glide_ms")) / 1000.0;
		s.curve = static_cast<EaseCurve>(obs_data_get_int(settings, "ease_curve"));
		s.mode = static_cast<EaseMode>(obs_data_get_int(settings, "ease_mode"));
		const std::map<std::string, double> learned = parse_learned(obs_data_get_string(settings, "learned"));
		// Not in the controls: how long a shape must hold before it is remembered. The tests shorten it.
		const double learn_after = obs_data_has_user_value(settings, "learn_after")
						   ? std::max(0.0, obs_data_get_double(settings, "learn_after"))
						   : kLearnAfter;
		std::lock_guard lock(mutex_);
		settings_ = s;
		learned_ = learned;
		learn_after_ = learn_after;
	}

	void tick(float seconds)
	{
		// A few times a second, see which program is being captured and
		// whether a rule gives its picture shape. A new size often means
		// another program, so then it is checked before the next picture is
		// drawn: a shape held for the last program must not be put on this one.
		identity_timer_ -= seconds;
		{
			obs_source_t *target = obs_filter_get_target(source_);
			const uint32_t w = target ? obs_source_get_base_width(target) : 0;
			const uint32_t h = target ? obs_source_get_base_height(target) : 0;
			std::lock_guard lock(mutex_);
			if (w && h && last_w_ && (w != last_w_ || h != last_h_))
				identity_timer_ = 0.0;
		}
		double rule_ratio = -1.0;
		if (identity_timer_ <= 0.0) {
			identity_timer_ = 0.25;
			const std::string program = captured_program();
			const std::string executable = captured_executable();
			std::lock_guard lock(mutex_);
			rule_ratio = 0.0;
			double usual = 0.0;
			bool seen_before = false;
			for (const auto &[text, ratio] : settings_.rules) {
				if (!program.empty() && program.find(text) != std::string::npos) {
					rule_ratio = ratio;
					break;
				}
			}
			if (rule_ratio <= 0.0 && settings_.known_programs && !executable.empty()) {
				const std::string exe = lower(executable);
				// What this program was seen to show the last time counts for
				// more than the table of common emulators.
				const auto remembered = learned_.find(exe);
				if (remembered != learned_.end()) {
					usual = std::max(0.0, remembered->second);
					seen_before = usual > 0.0;
				} else {
					for (const auto &known : kKnownPrograms) {
						if (exe.find(known.program) != std::string::npos) {
							usual = parse_ratio(known.shape);
							break;
						}
					}
				}
			}
			if (executable != program_ || usual != usual_ratio_) {
				// Another program: start from its usual shape if it has
				// one, otherwise from nothing, and look afresh.
				held_ratio_ = usual;
				last_w_ = last_h_ = 0;
			}
			if (executable != program_) {
				learned_this_time_ = false;
				steady_ratio_ = steady_for_ = 0.0;
			}
			usual_ratio_ = usual;
			usual_seen_before_ = seen_before;
			program_ = executable;
		}
		std::lock_guard lock(mutex_);
		pending_time_ += seconds;
		if (rule_ratio >= 0.0)
			rule_ratio_ = rule_ratio;
	}

	// The shape a Picture Shape key asked for; 0 goes back to automatic.
	// Is a Standby Picture on this source showing its picture (or still
	// changing to or from the capture)?
	bool standby_showing()
	{
		obs_source_t *parent = obs_filter_get_parent(source_);
		if (!parent)
			return false;
		bool standing = false;
		obs_source_enum_filters(
			parent,
			[](obs_source_t *, obs_source_t *filter, void *param) {
				const char *id = obs_source_get_unversioned_id(filter);
				if (!id || strcmp(id, "stagekit_standby") != 0 || !obs_source_enabled(filter))
					return;
				calldata_t cd = {};
				if (proc_handler_call(obs_source_get_proc_handler(filter), "get_standing", &cd) &&
				    calldata_bool(&cd, "standing"))
					*static_cast<bool *>(param) = true;
				calldata_free(&cd);
			},
			&standing);
		return standing;
	}

	void set_forced_ratio(double ratio)
	{
		obs_data_t *settings = obs_source_get_settings(source_);
		obs_data_set_double(settings, "force_ratio", std::max(0.0, ratio));
		obs_source_update(source_, settings);
		obs_data_release(settings);
	}

	obs_source_t *source() const { return source_; }

	// Lower-case text describing the program a capture source is showing:
	// what a game or window capture reports it is attached to, plus its
	// "window" setting (title:class:executable).
	std::string captured_program()
	{
		obs_source_t *parent = obs_filter_get_parent(source_);
		if (!parent)
			return {};
		std::string text;
		calldata_t cd = {};
		if (proc_handler_call(obs_source_get_proc_handler(parent), "get_hooked", &cd)) {
			for (const char *key : {"executable", "title"}) {
				const char *value = nullptr;
				if (calldata_get_string(&cd, key, &value) && value)
					text += std::string(value) + " ";
			}
		}
		calldata_free(&cd);
		obs_data_t *settings = obs_source_get_settings(parent);
		text += obs_data_get_string(settings, "window");
		obs_data_release(settings);
		return lower(text);
	}

	// The file name of the program being captured ("snes9x-x64.exe"), or
	// empty when the source is not capturing a program.
	std::string captured_executable()
	{
		obs_source_t *parent = obs_filter_get_parent(source_);
		if (!parent)
			return {};
		std::string executable;
		calldata_t cd = {};
		if (proc_handler_call(obs_source_get_proc_handler(parent), "get_hooked", &cd)) {
			const char *value = nullptr;
			if (calldata_get_string(&cd, "executable", &value) && value)
				executable = value;
		}
		calldata_free(&cd);
		if (executable.empty()) {
			// The "window" setting is title:class:executable, with ':' in
			// a part written "#3A".
			obs_data_t *settings = obs_source_get_settings(parent);
			const std::string window = obs_data_get_string(settings, "window");
			obs_data_release(settings);
			const size_t colon = window.rfind(':');
			if (colon != std::string::npos)
				executable = window.substr(colon + 1);
		}
		return executable;
	}

	// For the controls: what is being captured and which shape applies.
	void status(std::string &program, double &ratio, bool &built_in, bool &forced, bool &found, bool &seen_before)
	{
		std::lock_guard lock(mutex_);
		seen_before = usual_seen_before_;
		program = program_;
		forced = settings_.force_ratio > 0.0;
		ratio = forced ? settings_.force_ratio : rule_ratio_ > 0.0 ? rule_ratio_ : held_ratio_;
		// "Usual for this program" while the starting guess still stands.
		built_in = !forced && rule_ratio_ <= 0.0 && usual_ratio_ > 0.0 &&
			   std::fabs(held_ratio_ - usual_ratio_) < 0.01;
		found = !forced && rule_ratio_ <= 0.0 && !built_in;
	}

	// Stores "program = shape" in the saved shapes, replacing an earlier
	// entry for the same program.
	bool remember_shape()
	{
		obs_data_t *settings = obs_source_get_settings(source_);
		const std::string program = obs_data_get_string(settings, "rule_program");
		const std::string shape = obs_data_get_string(settings, "rule_shape");
		obs_data_release(settings);
		return save_shape(program, shape, false);
	}

	// Stores the picture as it is trimmed right now (found automatically,
	// measured with the button, or set by hand) as the shape for the
	// program being captured, and goes back to automatic trimming so that
	// shape is used for this program and others are still worked out.
	bool remember_current()
	{
		std::string program;
		double ratio = 0.0;
		{
			std::lock_guard lock(mutex_);
			program = program_;
			if (crop_goal_.w > 1.0 && crop_goal_.h > 1.0)
				ratio = crop_goal_.w / crop_goal_.h;
		}
		return ratio > 0.0 && save_shape(program, ratio_name(ratio), true);
	}

	bool save_shape(std::string program, const std::string &shape, bool automatic)
	{
		obs_data_t *settings = obs_source_get_settings(source_);
		program = lower(program);
		if (program.size() > 4 && program.compare(program.size() - 4, 4, ".exe") == 0)
			program.resize(program.size() - 4);
		const bool ok = !program.empty() && parse_ratio(shape) > 0.0;
		if (ok) {
			std::vector<std::pair<std::string, double>> rules;
			parse_rules_text(obs_data_get_string(settings, "rules_text"), rules);
			std::string text;
			for (const auto &[name, ratio] : rules) {
				if (name != program)
					text += name + " = " + ratio_name(ratio) + ", ";
			}
			text += program + " = " + shape;
			obs_data_set_string(settings, "rules_text", text.c_str());
			if (automatic)
				obs_data_set_int(settings, "trim", (int)Trim::Automatic);
			obs_source_update(source_, settings);
		}
		obs_data_release(settings);
		return ok;
	}

	// Asks for the bars to be measured from the picture as it is right now
	// and stored as a fixed trim.
	void measure_now()
	{
		std::lock_guard lock(mutex_);
		measure_once_[0] = measure_once_[1] = true;
		measured_ready_ = false;
	}

	uint32_t width()
	{
		std::lock_guard lock(mutex_);
		return settings_.follow && out_w_ ? out_w_ : settings_.frame_w;
	}
	uint32_t height()
	{
		std::lock_guard lock(mutex_);
		return settings_.follow && out_h_ ? out_h_ : settings_.frame_h;
	}

	void render()
	{
		obs_source_t *target = obs_filter_get_target(source_);
		const uint32_t w = target ? obs_source_get_base_width(target) : 0;
		const uint32_t h = target ? obs_source_get_base_height(target) : 0;
		if (!w || !h || !obs_filter_get_parent(source_)) {
			// The capture has gone for a moment (a game re-attaching as it
			// switches between a window and full-screen): keep the last
			// picture up briefly instead of blinking to nothing.
			uint32_t fw, fh;
			bool keep;
			{
				std::lock_guard lock(mutex_);
				gone_for_ += pending_time_;
				pending_time_ = 0.0;
				keep = have_rect_ && gone_for_ <= kKeepWhenGone;
				fw = drawn_w_;
				fh = drawn_h_;
			}
			if (keep)
				draw_frame(fw, fh);
			return;
		}

		Settings s;
		Rect rect, crop;
		double dt;
		bool probe_now = false, publish_measured = false, frozen = false;
		double measured_x = 0.0, measured_y = 0.0;
		std::string learn_program;
		double learn_shape = 0.0;
		{
			std::lock_guard lock(mutex_);
			s = settings_;
			dt = pending_time_;
			pending_time_ = 0.0;
			clock_ += dt;

			if (gone_for_ > kNewSession) {
				// The capture was away long enough for this to be something
				// else being captured: a shape held from before does not
				// carry over. A game switching to full-screen is back sooner.
				held_ratio_ = usual_ratio_;
				last_w_ = last_h_ = 0;
				have_rect_ = false;
				learned_this_time_ = false;
				steady_ratio_ = steady_for_ = 0.0;
			}
			gone_for_ = 0.0;
			if (w != last_w_ || h != last_h_) {
				// A new size is looked at often until the picture's shape is settled.
				eager_ = kEagerLooks;
				// For the log: a window being dragged to a new size changes
				// every frame, so this is said at most once a second.
				if (s.trim == Trim::Automatic && clock_ - said_size_at_ >= 1.0) {
					said_size_at_ = clock_;
					obs_log(LOG_INFO, "Auto Frame: %s is now %ux%u; %s", describe_program().c_str(),
						w, h,
						held_ratio_ > 0.0 ? ("starting from " + ratio_name(held_ratio_)).c_str()
								  : "no shape to start from");
				}
				// While the capture settles at its new size the last good
				// picture stays up, so nothing half-drawn is ever shown.
				if (have_rect_ && last_w_ && last_h_)
					settle_ = kSettle;
				// A new resolution (a window going full-screen, say). The
				// picture's shape was already established, so keep showing
				// that shape: work out the bars it implies at the new size,
				// then check them straight away instead of starting over.
				last_w_ = w;
				last_h_ = h;
				bars_[0] = bars_[1] = Bars();
				if (s.trim == Trim::Automatic && held_ratio_ > 0.0) {
					if ((double)w / (double)h > held_ratio_)
						bars_[0].applied = std::round((w - h * held_ratio_) / 2.0);
					else
						bars_[1].applied = std::round((h - w / held_ratio_) / 2.0);
					for (Bars &bars : bars_)
						if (bars.applied <= 2.0)
							bars.applied = 0.0;
					quick_looks_ = kQuickLooks;
					// Check the carried-over bars first, on this very frame.
					probe_timer_ = 0.0;
					probe_axis_ = bars_[1].applied > 0.0 ? 1 : 0;
				}
			}
			// How much to take off each side.
			double trim_x = 0.0, trim_y = 0.0;
			if (s.trim == Trim::Manual) {
				trim_x = s.trim_x;
				trim_y = s.trim_y;
			} else if (s.trim == Trim::Automatic) {
				trim_x = bars_[0].applied;
				trim_y = bars_[1].applied;
			}
			// A known shape beats looking: centre that shape in the capture.
			const double known = s.force_ratio > 0.0 ? s.force_ratio : rule_ratio_;
			const bool shape_known = s.trim == Trim::Automatic && known > 0.0;
			if (shape_known) {
				trim_x = trim_y = 0.0;
				if ((double)w / (double)h > known)
					trim_x = (w - h * known) / 2.0;
				else
					trim_y = (h - w / known) / 2.0;
			}
			if ((s.trim == Trim::Automatic && !shape_known) || measure_once_[0] || measure_once_[1]) {
				probe_timer_ -= dt;
				if (probe_timer_ <= 0.0) {
					// While measuring on request, look every frame to finish at once.
					const bool hurry = measure_once_[0] || measure_once_[1] || quick_looks_ > 0;
					// Until the picture has been seen to fit its trim both
					// ways, look often: that is when a wrong shape shows.
					const bool eager = eager_ > 0.0 && !(bars_[0].confirmed && bars_[1].confirmed);
					probe_timer_ = hurry   ? 0.0
						       : eager ? (last_look_dark_ ? kEagerDark : kEagerInterval)
							       : kProbeInterval;
					probe_now = true;
				}
			}
			publish_measured = measured_ready_;
			measured_ready_ = false;
			measured_x = measured_[0];
			measured_y = measured_[1];
			trim_x = std::clamp(trim_x, 0.0, w * 0.45);
			trim_y = std::clamp(trim_y, 0.0, h * 0.45);
			const Rect crop_goal{trim_x, trim_y, w - 2.0 * trim_x, h - 2.0 * trim_y};
			// The shape now on show is the one to hold on to, once the picture
			// has been seen to end exactly where it is trimmed. A black screen,
			// or a size the capture only had for a moment while starting up,
			// teaches nothing: taking a shape from those put a square crop on
			// a widescreen game. A picture seen to fill a window of no
			// particular shape has no shape to carry to the next size.
			if (s.trim == Trim::Automatic && !shape_known) {
				if (bars_[0].confirmed && bars_[1].confirmed) {
					const bool shaped = is_picture_shape(crop_goal.w, crop_goal.h);
					held_ratio_ = shaped ? crop_goal.w / crop_goal.h : 0.0;
					// A shape that has held for a while is remembered for this
					// program, so the next time it starts out right.
					const double now_shape = shaped ? snap_shape(held_ratio_) : 0.0;
					if (now_shape != steady_ratio_) {
						steady_ratio_ = now_shape;
						steady_for_ = 0.0;
					} else {
						steady_for_ += dt;
					}
					if (!learned_this_time_ && steady_for_ >= learn_after_ && s.known_programs &&
					    !program_.empty()) {
						// A picture that fills a window of no particular shape
						// is only worth noting for a program that had a shape
						// kept: it has more than one.
						const auto kept = learned_.find(lower(program_));
						if (now_shape > 0.0 || (kept != learned_.end() && kept->second > 0.0)) {
							learned_this_time_ = true;
							learn_program = lower(program_);
							learn_shape = now_shape > 0.0 ? now_shape : -1.0;
						}
					}
				}
			} else if (s.trim != Trim::Automatic) {
				held_ratio_ = 0.0;
			}
			if (s.follow) {
				// The frame takes the picture's own shape, so each scene can
				// place it as it likes.
				const double ratio = shape_ratio(s, (uint32_t)std::lround(crop_goal.w),
								 (uint32_t)std::lround(crop_goal.h));
				// Always the height asked for, whatever the capture's own
				// resolution: a game going from a window to full-screen at
				// the same shape then changes nothing a scene can see.
				s.frame_w = (uint32_t)std::clamp(std::lround(s.frame_h * ratio), 2L, 16384L);
				out_w_ = s.frame_w;
				out_h_ = s.frame_h;
			}
			const Rect goal =
				target_rect(s, (uint32_t)std::lround(crop_goal.w), (uint32_t)std::lround(crop_goal.h));

			if (!have_rect_) {
				// First picture: no glide, just be in the right place.
				current_ = from_ = goal_ = goal;
				crop_current_ = crop_from_ = crop_goal_ = crop_goal;
				have_rect_ = true;
				elapsed_ = duration_ = 0.0;
			} else if (settle_ > 0.0) {
				// Settling after a resolution change. The crop is in the new
				// capture's pixels, so it is taken as it is; the picture
				// glides only if it really ends up somewhere else.
				settle_ -= dt;
				crop_current_ = crop_from_ = crop_goal_ = crop_goal;
				from_ = current_;
				goal_ = goal;
				elapsed_ = 0.0;
				duration_ = same(goal, current_) ? 0.0 : s.glide_s;
				frozen = settle_ > 0.0;
			} else if (!same(goal, goal_) || !same(crop_goal, crop_goal_)) {
				from_ = current_;
				goal_ = goal;
				crop_from_ = crop_current_;
				crop_goal_ = crop_goal;
				elapsed_ = 0.0;
				duration_ = s.glide_s;
			}
			if (frozen) {
				// Nothing moves while the old picture is held.
			} else if (elapsed_ < duration_) {
				elapsed_ = std::min(duration_, elapsed_ + dt);
				const double t = ease(s.curve, s.mode, elapsed_ / duration_);
				auto mix = [t](const Rect &a, const Rect &b) {
					return Rect{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.w + (b.w - a.w) * t,
						    a.h + (b.h - a.h) * t};
				};
				current_ = mix(from_, goal_);
				crop_current_ = mix(crop_from_, crop_goal_);
			} else {
				current_ = goal_;
				crop_current_ = crop_goal_;
			}
			rect = current_;
			crop = crop_current_;
			if (s.follow)
				rect = Rect{0.0, 0.0, (double)s.frame_w, (double)s.frame_h};
		}

		if (!learn_program.empty())
			remember_program_shape(learn_program, learn_shape);

		if (publish_measured) {
			// Store what was measured as a fixed trim, where the user can see and edit it.
			obs_data_t *settings = obs_source_get_settings(source_);
			obs_data_set_int(settings, "trim", (int)Trim::Manual);
			obs_data_set_double(settings, "trim_x", measured_x);
			obs_data_set_double(settings, "trim_y", measured_y);
			obs_source_update(source_, settings);
			obs_data_release(settings);
		}

		// 1. The capture as it is.
		gs_texrender_reset(tr_input_);
		if (!obs_source_process_filter_begin(source_, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
			return;
		if (!gs_texrender_begin(tr_input_, w, h))
			return;
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		obs_source_process_filter_end(source_, obs_get_base_effect(OBS_EFFECT_DEFAULT), w, h);
		gs_blend_state_pop();
		gs_texrender_end(tr_input_);
		gs_texture_t *input = gs_texrender_get_texture(tr_input_);
		if (!input)
			return;

		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");

		if (probe_now)
			look_for_bars(input, w, h, effect, image, s.trim_wait_s);

		// 2. Placed inside the frame. Drawing into a frame-sized target also
		// crops anything a "fill" fit pushes past the edges.
		const bool previous_srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(false);
		if (frozen && gs_texrender_get_texture(tr_frame_)) {
			// Held: the frame drawn before the change is shown again.
			gs_enable_framebuffer_srgb(previous_srgb);
			draw_frame(drawn_w_, drawn_h_);
			return;
		}
		drawn_w_ = s.frame_w;
		drawn_h_ = s.frame_h;
		gs_texrender_reset(tr_frame_);
		if (gs_texrender_begin(tr_frame_, s.frame_w, s.frame_h)) {
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, (float)s.frame_w, 0.0f, (float)s.frame_h, -100.0f, 100.0f);
			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
			gs_effect_set_texture(image, input);
			if (s.sharp)
				gs_effect_set_next_sampler(image, point_sampler_);
			gs_matrix_push();
			gs_matrix_translate3f((float)rect.x, (float)rect.y, 0.0f);
			// Only the part of the capture inside the bars is drawn.
			const uint32_t cx = (uint32_t)std::clamp(std::lround(crop.x), 0L, (long)w - 1);
			const uint32_t cy = (uint32_t)std::clamp(std::lround(crop.y), 0L, (long)h - 1);
			const uint32_t cw = (uint32_t)std::clamp(std::lround(crop.w), 1L, (long)(w - cx));
			const uint32_t ch = (uint32_t)std::clamp(std::lround(crop.h), 1L, (long)(h - cy));
			gs_matrix_scale3f((float)(rect.w / cw), (float)(rect.h / ch), 1.0f);
			while (gs_effect_loop(effect, "Draw"))
				gs_draw_sprite_subregion(input, 0, cx, cy, cw, ch);
			gs_matrix_pop();
			gs_blend_state_pop();
			gs_texrender_end(tr_frame_);
		}
		gs_enable_framebuffer_srgb(previous_srgb);

		// 3. The finished frame.
		draw_frame(s.frame_w, s.frame_h);
	}

private:
	// Draws the finished frame kept in tr_frame_.
	void draw_frame(uint32_t frame_w, uint32_t frame_h)
	{
		gs_texture_t *frame = gs_texrender_get_texture(tr_frame_);
		if (!frame || !frame_w || !frame_h)
			return;
		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
		const bool linear = gs_get_linear_srgb();
		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(linear);
		if (linear)
			gs_effect_set_texture_srgb(image, frame);
		else
			gs_effect_set_texture(image, frame);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(frame, 0, frame_w, frame_h);
		gs_enable_framebuffer_srgb(previous);
	}

	static constexpr double kProbeInterval = 0.4;
	// A line counts as the picture's edge when this share of the strips across
	// it start being lit there, give or take kEdge pixels. Artwork, a logo or
	// a dark scene on black ends raggedly and never reaches it; a picture
	// inside bars does on any frame that is not too dark at its edges.
	static constexpr double kEdgeShare = 0.15;
	static constexpr double kEdge = 2.0;
	// Something lit inside the trimmed part only counts when it shows on this
	// share of the strips: a mouse pointer, a pop-up or a menu strip over the
	// bars is not the picture.
	static constexpr double kInsideShare = 0.3;
	// How long the picture must keep showing inside a trim before it is let
	// go: a trim that was only a starting point, and one that had been seen.
	static constexpr double kGiveUp = 1.0;
	static constexpr double kInsideFor = 4.0;
	// Bars a program draws are one flat level, nearly always pure black. A
	// pixel this much above the darkest the capture's own edge shows is taken
	// as the picture, so a night scene still shows where it ends.
	static constexpr int kLitStep = 4;
	// How long the furthest out the picture has reached is remembered without
	// being seen there again.
	static constexpr double kOuterMemory = 5.0;
	// Looking often while a new size is not settled: how many looks, and how
	// far apart on a lit and on a black picture. A black look costs a quarter.
	static constexpr double kEagerLooks = 80.0;
	static constexpr double kEagerInterval = 0.1;
	static constexpr double kEagerDark = 0.2;
	// How long a shape must hold before it is remembered for its program.
	static constexpr double kLearnAfter = 15.0;

	// What has been seen of the bars along one direction (0: left/right,
	// 1: top/bottom), and how much is currently trimmed.
	struct Bars {
		double applied = 0.0;    // pixels trimmed from each side
		double candidate = -1.0; // what the last look suggested
		double stable_for = 0.0; // seconds the candidate has held
		int agree = 0;           // looks in a row that agreed
		// Seen while the candidate held: the picture ending in a straight
		// line there, on the near side and on the far side.
		bool edge_lo = false, edge_hi = false;
		// The picture has been seen to end exactly at the trim on both
		// sides. Until then the trim is only a starting point (nothing, a
		// program's usual shape, or a shape carried over from the last size)
		// and clear evidence is acted on at once.
		bool confirmed = false;
		double inside_for = 0.0; // seconds the picture has kept showing inside the trim
		int inside_looks = 0;
		double last_look = -1.0; // on the look clock
		// The furthest out the picture has been seen to reach from each end,
		// as a distance in from that end, and when it was last seen there.
		// -1 until something lit has been seen.
		double outer_lo = -1.0, outer_hi = -1.0;
		double outer_lo_at = 0.0, outer_hi_at = 0.0;
	};

	// Looks at the capture for black bars. One direction per call, to keep
	// the cost down: a thin copy of the picture is read back and scanned.
	//
	// Black bars and a black background look the same by brightness, so
	// brightness alone decides nothing. More is trimmed only when the picture
	// is seen to end in a straight line, the same distance in from both
	// sides, leaving exactly a shape consoles and emulators output. Less is
	// trimmed when the picture, not some small thing, shows inside the trim.
	// The first clear sight of a picture is acted on at once; after that a
	// change has to hold for the wait the user set, so letterboxed cut-scenes
	// and menus do not resize the picture.
	//
	// A first sight can also be clear without straight edges, which a dark
	// scene seldom gives: a picture fitted inside the capture, as games and
	// emulators draw one, reaches both ends one way and stops the same
	// distance in from both ends the other way, leaving a real shape. A few
	// lit pixels at each of the four sides show that; artwork or a logo on
	// black never reaches the ends.
	void look_for_bars(gs_texture_t *input, uint32_t w, uint32_t h, gs_effect_t *effect, gs_eparam_t *image,
			   double wait_s)
	{
		const int axis = probe_axis_;
		probe_axis_ = 1 - probe_axis_;
		// Full resolution along the direction being measured, coarse across it.
		const uint32_t pw = axis == 0 ? w : std::min(w, 160u);
		const uint32_t ph = axis == 0 ? std::min(h, 90u) : h;

		const bool previous_srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(false);
		gs_texrender_reset(tr_probe_);
		if (gs_texrender_begin(tr_probe_, pw, ph)) {
			vec4 clear;
			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, (float)pw, 0.0f, (float)ph, -100.0f, 100.0f);
			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
			gs_effect_set_texture(image, input);
			while (gs_effect_loop(effect, "Draw"))
				gs_draw_sprite(input, 0, pw, ph);
			gs_blend_state_pop();
			gs_texrender_end(tr_probe_);
		}
		gs_enable_framebuffer_srgb(previous_srgb);

		gs_stagesurf_t *&stage = stage_[axis];
		if (!stage || gs_stagesurface_get_width(stage) != pw || gs_stagesurface_get_height(stage) != ph) {
			gs_stagesurface_destroy(stage);
			stage = gs_stagesurface_create(pw, ph, GS_RGBA);
		}
		gs_texture_t *probe = gs_texrender_get_texture(tr_probe_);
		if (!stage || !probe)
			return;
		gs_stage_texture(stage, probe);
		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		if (!gs_stagesurface_map(stage, &data, &linesize))
			return;

		// For each strip across the direction being measured: where its lit
		// part begins and ends. Lit means a little brighter than the bars are.
		const uint32_t lines = axis == 0 ? pw : ph;
		const uint32_t across = axis == 0 ? ph : pw;
		auto level = [&](uint32_t line, uint32_t strip) -> int {
			const uint32_t x = axis == 0 ? line : strip, y = axis == 0 ? strip : line;
			const uint8_t *px = data + (size_t)y * linesize + (size_t)x * 4;
			return px[3] > 16 ? std::max({(int)px[0], (int)px[1], (int)px[2]}) : 0;
		};
		// The darkest the capture's own two ends show, which is what bars are.
		int black = 16;
		for (uint32_t strip = 0; strip < across; strip++)
			black = std::min({black, level(0, strip), level(lines - 1, strip)});
		const int lit_above = black + kLitStep;
		auto lit = [&](uint32_t line, uint32_t strip) {
			return level(line, strip) > lit_above;
		};
		strip_lo_.assign(across, lines);
		strip_hi_.assign(across, 0);
		reach_lo_.clear();
		reach_hi_.clear();
		for (uint32_t strip = 0; strip < across; strip++) {
			uint32_t begin = 0;
			while (begin < lines && !lit(begin, strip))
				begin++;
			if (begin == lines)
				continue; // nothing lit along this strip
			uint32_t end = lines;
			while (end > begin + 1 && !lit(end - 1, strip))
				end--;
			strip_lo_[strip] = begin;
			strip_hi_[strip] = end;
			reach_lo_.push_back(begin);
			reach_hi_.push_back(lines - end);
		}
		gs_stagesurface_unmap(stage);
		// How far in from each end the picture begins, leaving out a strip or
		// two: a mouse pointer or a speck is not where the picture ends.
		const size_t skip = across / 50;
		const bool dark = reach_lo_.size() <= skip;
		double near_lo = lines, near_hi = lines;
		if (!dark) {
			std::nth_element(reach_lo_.begin(), reach_lo_.begin() + (std::ptrdiff_t)skip, reach_lo_.end());
			std::nth_element(reach_hi_.begin(), reach_hi_.begin() + (std::ptrdiff_t)skip, reach_hi_.end());
			near_lo = reach_lo_[skip];
			near_hi = reach_hi_[skip];
		}

		std::lock_guard lock(mutex_);
		Bars &bars = bars_[axis];
		Bars &other = bars_[1 - axis];
		// Time since this direction was last looked at. A long gap (the
		// source was not being drawn) is not counted as the picture holding.
		const double since = bars.last_look < 0.0 ? 0.0 : std::clamp(clock_ - bars.last_look, 0.0, 1.0);
		bars.last_look = clock_;
		if (quick_looks_ > 0)
			quick_looks_--;
		last_look_dark_ = dark;
		eager_ -= dark ? 0.25 : 1.0;
		if (dark) {
			// Entirely black (a fade, a loading screen): learn nothing from it.
			bars.candidate = -1.0;
			bars.agree = 0;
			bars.stable_for = 0.0;
			bars.edge_lo = bars.edge_hi = false;
			bars.inside_for = 0.0;
			bars.inside_looks = 0;
			return;
		}
		// Bars are trimmed evenly, by the smaller side, so a dark edge in the
		// picture itself is never mistaken for more bar.
		const double seen = std::min(near_lo, near_hi);
		int edge_lo = 0, edge_hi = 0, inside = 0;
		for (uint32_t strip = 0; strip < across; strip++) {
			if (!strip_hi_[strip])
				continue;
			const double lo = strip_lo_[strip], hi = (double)lines - strip_hi_[strip];
			if (std::fabs(lo - seen) <= kEdge)
				edge_lo++;
			if (std::fabs(hi - seen) <= kEdge)
				edge_hi++;
			if (bars.applied > 2.0 && (lo < bars.applied - 2.0 || hi < bars.applied - 2.0))
				inside++;
		}
		const int edge_enough = std::max(2, (int)std::lround(across * kEdgeShare));
		const int inside_enough = std::max(2, (int)std::lround(across * kInsideShare));

		// The furthest out the picture has been seen from each end. Seeing it
		// further out moves the mark at once; not seeing it there for a while
		// lets the mark go.
		auto reached = [&](double &outer, double &at, double now_at) {
			if (outer < 0.0 || now_at < outer - kEdge || clock_ - at > kOuterMemory) {
				outer = now_at;
				at = clock_;
			} else if (now_at <= outer + kEdge) {
				outer = std::min(outer, now_at);
				at = clock_;
			}
		};
		reached(bars.outer_lo, bars.outer_lo_at, near_lo);
		reached(bars.outer_hi, bars.outer_hi_at, near_hi);

		if (bars.candidate >= 0.0 && std::fabs(seen - bars.candidate) <= 2.0) {
			bars.agree++;
			bars.stable_for += since;
		} else {
			bars.candidate = seen;
			bars.agree = 1;
			bars.stable_for = 0.0;
			bars.edge_lo = bars.edge_hi = false;
		}
		bars.edge_lo = bars.edge_lo || edge_lo >= edge_enough;
		bars.edge_hi = bars.edge_hi || edge_hi >= edge_enough;
		// "Trim to what is on screen now": take this look as it is.
		if (measure_once_[axis]) {
			measure_once_[axis] = false;
			measured_[axis] = seen;
			if (!measure_once_[0] && !measure_once_[1])
				measured_ready_ = true;
		}

		// What the picture would be with this many pixels of bar.
		auto shape_with = [&](double other_trim) {
			const double pic_w = w - 2.0 * (axis == 0 ? seen : other_trim);
			const double pic_h = h - 2.0 * (axis == 1 ? seen : other_trim);
			return is_picture_shape(pic_w, pic_h);
		};
		// This way, the picture has been seen to stop the same distance in
		// from both ends; the other way, to reach both ends.
		const bool even = std::fabs(bars.outer_lo - seen) <= kEdge && std::fabs(bars.outer_hi - seen) <= kEdge;
		const bool across_full = other.outer_lo >= 0.0 && other.outer_lo <= kEdge && other.outer_hi >= 0.0 &&
					 other.outer_hi <= kEdge &&
					 clock_ - std::min(other.outer_lo_at, other.outer_hi_at) <= kOuterMemory;
		const bool edges = bars.edge_lo && bars.edge_hi;
		if (std::fabs(seen - bars.applied) <= 2.0) {
			// The picture ends where it is trimmed. That is borne out by
			// straight edges; with nothing trimmed, by the picture reaching
			// both ends; with something trimmed, by the picture fitting the
			// capture the way one of a real shape does.
			bars.inside_for = 0.0;
			bars.inside_looks = 0;
			const bool reaches = bars.applied <= 2.0 && even;
			const bool fits = bars.applied > 2.0 && even && across_full && shape_with(0.0);
			if (edges || reaches || fits)
				bars.confirmed = true;
			return;
		}

		// A trim across the other direction that was only a starting point
		// does not get a say when the picture is seen to show inside it and
		// this direction explains the picture alone: what is seen counts for
		// more than a guess. The strips of this look that lie in the other
		// direction's trim tell whether anything shows there.
		int other_strips = 0, other_lit = 0;
		if (other.applied > 2.0 && !other.confirmed) {
			const double extent = axis == 0 ? h : w;
			for (uint32_t strip = 0; strip < across; strip++) {
				const double at = (strip + 0.5) * extent / across;
				if (at < other.applied - 2.0 || at > extent - other.applied + 2.0) {
					other_strips++;
					if (strip_hi_[strip])
						other_lit++;
				}
			}
		}
		const bool shows_in_other = other_strips > 0 &&
					    other_lit >= std::max(2, (int)std::lround(other_strips * kInsideShare));
		// A picture fitted inside the capture: the same distance in from both
		// ends this way, right to both ends the other way, and a real shape.
		const bool fitted = !bars.confirmed && seen > 2.0 && even && across_full &&
				    !(other.confirmed && other.applied > 2.0) && shape_with(0.0);
		const bool alone = seen > 2.0 && (shows_in_other || fitted) && shape_with(0.0);
		const bool real_shape = alone || shape_with(other.applied);
		// Clear by its edges: the picture is seen to end at this line on both
		// sides, and that leaves a real picture shape. Reaching the capture's
		// own edge needs no shape to be believed: nothing is cut off by
		// trimming nothing. Clear at first sight: it fits, or it reaches both
		// ends. Once a direction has been borne out only edges will do.
		const bool by_edges = edges && (seen <= 2.0 || real_shape);
		const bool first_sight = !bars.confirmed && (fitted || (seen <= 2.0 && even));
		const bool clear = bars.agree >= 2 && (by_edges || first_sight);

		// Letting go of a trim never cuts anything off, so a steady candidate
		// that leaves a real shape will do for where to stop; failing that,
		// nothing is trimmed and the bars are looked for afresh.
		const double let_go_to = (clear || (real_shape && bars.agree >= 2)) ? seen : 0.0;
		bool change = false;
		double to = seen;
		const char *why = "";
		if (seen < bars.applied) {
			// Something is lit inside the trim.
			if (inside < inside_enough) {
				bars.inside_for = 0.0;
				bars.inside_looks = 0;
				return;
			}
			bars.inside_for += since;
			bars.inside_looks++;
			if (!bars.confirmed) {
				if (clear) {
					change = true;
					why = "its edges are seen further out than the starting shape";
				} else if (bars.inside_for >= kGiveUp && bars.inside_looks >= 2) {
					change = true;
					to = let_go_to;
					why = "the picture shows inside a trim that was only a starting point";
				}
			} else if (clear && bars.stable_for >= wait_s) {
				change = true;
				why = "the picture has grown and held";
			} else if (bars.inside_for >= std::max(kInsideFor, wait_s)) {
				change = true;
				to = let_go_to;
				why = "the picture keeps showing inside the trim";
			}
			if (change && !bars.confirmed)
				held_ratio_ = 0.0; // the shape started from did not suit this picture
		} else {
			bars.inside_for = 0.0;
			bars.inside_looks = 0;
			// More bar than is trimmed. A game with a black background looks
			// just like bars, so only a clear sight counts.
			if (clear && seen > 2.0 && (!bars.confirmed || bars.stable_for >= wait_s)) {
				change = true;
				why = bars.confirmed ? "bars seen clearly and held"
				      : fitted       ? "the picture fits the capture as that shape does"
						     : "bars seen clearly";
			}
		}
		if (!change)
			return;
		if (to <= 2.0)
			to = 0.0;
		obs_log(LOG_INFO, "Auto Frame: %s %s trim %.0f -> %.0f px at %ux%u (%s)", describe_program().c_str(),
			axis == 0 ? "left/right" : "top/bottom", bars.applied, to, w, h, why);
		bars.applied = to;
		bars.inside_for = 0.0;
		bars.inside_looks = 0;
		if (alone && to > 2.0 && other.applied > 2.0) {
			obs_log(LOG_INFO,
				"Auto Frame: %s %s trim %.0f -> 0 px (the other direction explains the picture)",
				describe_program().c_str(), axis == 0 ? "top/bottom" : "left/right", other.applied);
			other = Bars();
		}
	}

	// Keeps the shape a program has held, for the next time it is captured. A
	// program that turns out to show a different shape from the one kept has
	// more than one, and from then on starts from nothing.
	void remember_program_shape(const std::string &program, double shape)
	{
		obs_data_t *settings = obs_source_get_settings(source_);
		std::map<std::string, double> learned = parse_learned(obs_data_get_string(settings, "learned"));
		const auto before = learned.find(program);
		double keep = shape;
		if (shape <= 0.0 || (before != learned.end() &&
				     (before->second < 0.0 || std::fabs(before->second - shape) > shape * 0.01)))
			keep = -1.0;
		if (before == learned.end() || before->second != keep) {
			learned[program] = keep;
			obs_data_set_string(settings, "learned", learned_text(learned).c_str());
			if (keep > 0.0)
				obs_log(LOG_INFO, "Auto Frame: %s shows %s; it will start from that next time",
					program.c_str(), ratio_name(keep).c_str());
			else
				obs_log(LOG_INFO,
					"Auto Frame: %s has shown more than one shape; it will start from nothing",
					program.c_str());
		}
		obs_data_release(settings);
		std::lock_guard lock(mutex_);
		learned_ = learned;
		if (lower(program_) == program) {
			// So the next look at which program this is finds nothing changed.
			usual_ratio_ = std::max(0.0, keep);
			usual_seen_before_ = keep > 0.0;
		}
	}

public:
	// Forgets every shape remembered for a program.
	void forget_learned()
	{
		obs_data_t *settings = obs_source_get_settings(source_);
		obs_data_set_string(settings, "learned", "");
		obs_source_update(source_, settings);
		obs_data_release(settings);
	}

	// What is remembered, as text for the controls.
	std::string learned_now()
	{
		std::lock_guard lock(mutex_);
		return learned_text(learned_);
	}

private:
	// For the log: the program being captured, or the source's name.
	std::string describe_program() const
	{
		if (!program_.empty())
			return program_;
		obs_source_t *parent = obs_filter_get_parent(source_);
		const char *name = parent ? obs_source_get_name(parent) : nullptr;
		return name ? std::string("'") + name + "'" : std::string("the capture");
	}

	// Whether a picture size is one of the shapes games and emulators output,
	// to within a few pixels.
	static bool is_picture_shape(double pic_w, double pic_h)
	{
		if (pic_w < 16.0 || pic_h < 16.0)
			return false;
		const double tolerance = std::max(4.0, pic_h * 0.004);
		for (double shape : kPictureShapes)
			if (std::fabs(pic_w - pic_h * shape) <= tolerance)
				return true;
		return false;
	}

	obs_source_t *source_;
	std::mutex mutex_;
	Settings settings_;
	bool have_rect_ = false;
	Rect current_, from_, goal_;
	Rect crop_current_, crop_from_, crop_goal_;
	uint32_t out_w_ = 0, out_h_ = 0; // the frame's size when it follows the picture
	uint32_t last_w_ = 0, last_h_ = 0;
	Bars bars_[2];
	std::vector<uint32_t> strip_lo_, strip_hi_; // per strip, from the last look; drawing thread only
	std::vector<uint32_t> reach_lo_, reach_hi_; // the lit strips' distances in from each end, for sorting
	double eager_ = 0.0;                        // looks left at the quick pace
	bool last_look_dark_ = false;
	// What each program was seen to show (lower-case executable -> shape, -1 for more than one).
	std::map<std::string, double> learned_;
	double learn_after_ = kLearnAfter;
	double steady_ratio_ = 0.0, steady_for_ = 0.0; // the shape now held, and for how long
	bool learned_this_time_ = false;
	bool usual_seen_before_ = false; // the shape started from is one this program showed before
	double clock_ = 0.0;             // seconds drawn, for timing the looks
	double said_size_at_ = -10.0;
	double probe_timer_ = 0.0;
	// The picture shape found by looking, kept across resolution changes.
	double held_ratio_ = 0.0;
	static constexpr int kQuickLooks = 8;
	// How long the last good picture is held: while the capture settles
	// at a new size, and when the capture drops out altogether.
	static constexpr double kSettle = 0.2;
	static constexpr double kKeepWhenGone = 1.0;
	static constexpr double kNewSession = 4.0; // away this long, the next picture starts afresh
	double settle_ = 0.0;
	double gone_for_ = 0.0;
	uint32_t drawn_w_ = 0, drawn_h_ = 0;
	int quick_looks_ = 0;
	bool measure_once_[2] = {false, false};
	double measured_[2] = {0.0, 0.0};
	double identity_timer_ = 0.0;
	double rule_ratio_ = 0.0;
	double usual_ratio_ = 0.0; // the built-in shape for the program being captured
	std::string program_;
	bool measured_ready_ = false;
	int probe_axis_ = 0;
	gs_texrender_t *tr_probe_ = nullptr;
	gs_stagesurf_t *stage_[2] = {nullptr, nullptr};
	double elapsed_ = 0.0, duration_ = 0.0, pending_time_ = 0.0;
	gs_texrender_t *tr_input_ = nullptr;
	gs_texrender_t *tr_frame_ = nullptr;
	gs_samplerstate_t *point_sampler_ = nullptr;
};

bool shape_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool custom = static_cast<Shape>(obs_data_get_int(settings, "shape")) == Shape::Custom;
	obs_property_set_visible(obs_properties_get(props, "custom_w"), custom);
	obs_property_set_visible(obs_properties_get(props, "custom_h"), custom);
	const auto trim = static_cast<Trim>(obs_data_get_int(settings, "trim"));
	obs_property_set_visible(obs_properties_get(props, "trim_wait"), trim == Trim::Automatic);
	obs_property_set_visible(obs_properties_get(props, "programs_group"), trim == Trim::Automatic);
	obs_property_set_visible(obs_properties_get(props, "trim_x"), trim == Trim::Manual);
	obs_property_set_visible(obs_properties_get(props, "trim_y"), trim == Trim::Manual);
	return true;
}

// Refills the "copy from" item list for the chosen scene.
bool copy_scene_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	obs_property_t *list = obs_properties_get(props, "copy_item");
	obs_property_list_clear(list);
	obs_source_t *scene = obs_get_source_by_name(obs_data_get_string(settings, "copy_scene"));
	add_scene_items_to_list(scene, list);
	obs_source_release(scene);
	return true;
}

// Sets the frame to the size an existing item takes up in its scene.
bool copy_frame_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *filter = static_cast<obs_source_t *>(data);
	obs_data_t *settings = obs_source_get_settings(filter);
	obs_source_t *scene = obs_get_source_by_name(obs_data_get_string(settings, "copy_scene"));
	obs_sceneitem_t *item = find_item(scene, obs_data_get_string(settings, "copy_item"));
	bool changed = false;
	if (item) {
		refresh_item(item);
		const Box box = scene_box(scene, item);
		if (box.w >= 2.0 && box.h >= 2.0) {
			obs_data_set_int(settings, "frame_width", std::lround(box.w));
			obs_data_set_int(settings, "frame_height", std::lround(box.h));
			obs_source_update(filter, settings);
			changed = true;
		}
	}
	obs_source_release(scene);
	obs_data_release(settings);
	return changed;
}

void show_status(obs_properties_t *props, AutoFrame *self)
{
	std::string program;
	double ratio = 0.0;
	bool built_in = false, forced = false, found = false, seen_before = false;
	if (self)
		self->status(program, ratio, built_in, forced, found, seen_before);
	auto fill = [](std::string text, const std::string &with) {
		const size_t at = text.find("%1");
		if (at != std::string::npos)
			text.replace(at, 2, with);
		return text;
	};
	std::string text = program.empty() ? obs_module_text("Frame.Status.NoProgram")
					   : fill(obs_module_text("Frame.Status.Program"), program);
	text += " ";
	if (ratio <= 0.0)
		text += obs_module_text("Frame.Status.Auto");
	else
		text += fill(obs_module_text(forced                    ? "Frame.Status.Key"
					     : built_in && seen_before ? "Frame.Status.SeenBefore"
					     : built_in                ? "Frame.Status.BuiltIn"
					     : found                   ? "Frame.Status.Found"
								       : "Frame.Status.Yours"),
			     ratio_name(ratio));
	obs_property_set_description(obs_properties_get(props, "status"), text.c_str());
}

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<AutoFrame *>(data);
	obs_properties_t *top = obs_properties_create();

	// What it sees right now.
	obs_property_t *status = obs_properties_add_text(top, "status", "", OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(status, true);
	show_status(top, self);

	// 1. The frame.
	obs_properties_t *props = obs_properties_create();
	obs_property_t *mode = obs_properties_add_list(props, "frame_mode", obs_module_text("Frame.Mode"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, obs_module_text("Frame.Mode.Fixed"), 0);
	obs_property_list_add_int(mode, obs_module_text("Frame.Mode.Picture"), 1);
	obs_property_set_long_description(mode, obs_module_text("Frame.Mode.Tip"));
	obs_properties_add_int(props, "frame_width", obs_module_text("Frame.Width"), 2, 16384, 1);
	obs_properties_add_int(props, "frame_height", obs_module_text("Frame.Height"), 2, 16384, 1);

	// Take the size from something already laid out, instead of typing it.
	obs_properties_t *copy = obs_properties_create();
	obs_property_t *copy_scene = obs_properties_add_list(copy, "copy_scene", obs_module_text("Frame.CopyScene"),
							     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(copy_scene, "", "");
	obs_enum_scenes(
		[](void *param, obs_source_t *source) {
			const char *name = obs_source_get_name(source);
			if (name && *name)
				obs_property_list_add_string(static_cast<obs_property_t *>(param), name, name);
			return true;
		},
		copy_scene);
	obs_property_set_modified_callback(copy_scene, copy_scene_modified);
	obs_properties_add_list(copy, "copy_item", obs_module_text("Frame.CopyItem"), OBS_COMBO_TYPE_LIST,
				OBS_COMBO_FORMAT_STRING);
	obs_properties_add_button2(copy, "copy_frame", obs_module_text("Frame.CopyButton"), copy_frame_clicked,
				   data ? static_cast<AutoFrame *>(data)->source() : nullptr);
	obs_properties_add_group(props, "copy_group", obs_module_text("Frame.CopyGroup"), OBS_GROUP_NORMAL, copy);
	obs_properties_add_group(top, "size_group", obs_module_text("Frame.SizeGroup"), OBS_GROUP_NORMAL, props);

	// 2. How the picture sits in it.
	props = obs_properties_create();

	obs_property_t *fit = obs_properties_add_list(props, "fit", obs_module_text("Frame.Fit"), OBS_COMBO_TYPE_LIST,
						      OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(fit, obs_module_text("Frame.Fit.Fit"), (int)FitMode::Fit);
	obs_property_list_add_int(fit, obs_module_text("Frame.Fit.Whole"), (int)FitMode::Whole);
	obs_property_list_add_int(fit, obs_module_text("Frame.Fit.Fill"), (int)FitMode::Fill);
	obs_property_list_add_int(fit, obs_module_text("Frame.Fit.Stretch"), (int)FitMode::Stretch);

	obs_property_t *shape = obs_properties_add_list(props, "shape", obs_module_text("Frame.Shape"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(shape, obs_module_text("Frame.Shape.Source"), (int)Shape::Source);
	struct {
		Shape value;
		const char *text;
	} shapes[] = {{Shape::R4x3, "4:3"},   {Shape::R16x9, "16:9"},   {Shape::R8x7, "8:7"}, {Shape::R3x2, "3:2"},
		      {Shape::R10x9, "10:9"}, {Shape::R16x10, "16:10"}, {Shape::R5x4, "5:4"}, {Shape::R1x1, "1:1"}};
	for (const auto &entry : shapes)
		obs_property_list_add_int(shape, entry.text, (int)entry.value);
	obs_property_list_add_int(shape, obs_module_text("Frame.Shape.Custom"), (int)Shape::Custom);
	obs_property_set_modified_callback(shape, shape_modified);
	obs_properties_add_float(props, "custom_w", obs_module_text("Frame.CustomW"), 0.01, 1000.0, 0.01);
	obs_properties_add_float(props, "custom_h", obs_module_text("Frame.CustomH"), 0.01, 1000.0, 0.01);

	obs_property_t *ax = obs_properties_add_list(props, "align_x", obs_module_text("Frame.AlignX"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(ax, obs_module_text("Frame.Left"), 0);
	obs_property_list_add_int(ax, obs_module_text("Frame.Center"), 1);
	obs_property_list_add_int(ax, obs_module_text("Frame.Right"), 2);
	obs_property_t *ay = obs_properties_add_list(props, "align_y", obs_module_text("Frame.AlignY"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(ay, obs_module_text("Frame.Top"), 0);
	obs_property_list_add_int(ay, obs_module_text("Frame.Center"), 1);
	obs_property_list_add_int(ay, obs_module_text("Frame.Bottom"), 2);
	obs_properties_add_bool(props, "sharp", obs_module_text("Frame.Sharp"));
	obs_properties_add_group(top, "picture_group", obs_module_text("Frame.PictureGroup"), OBS_GROUP_NORMAL, props);

	// 3. Black bars.
	props = obs_properties_create();

	obs_property_t *trim = obs_properties_add_list(props, "trim", obs_module_text("Frame.Trim"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(trim, obs_module_text("Frame.Trim.Automatic"), (int)Trim::Automatic);
	obs_property_list_add_int(trim, obs_module_text("Frame.Trim.Manual"), (int)Trim::Manual);
	obs_property_list_add_int(trim, obs_module_text("Frame.Trim.Off"), (int)Trim::Off);
	obs_property_set_modified_callback(trim, shape_modified);
	obs_properties_add_button2(
		props, "trim_now", obs_module_text("Frame.TrimNow"),
		[](obs_properties_t *, obs_property_t *, void *d) {
			if (d)
				static_cast<AutoFrame *>(d)->measure_now();
			return false;
		},
		data);
	obs_property_t *keep = obs_properties_add_button2(
		props, "trim_keep", obs_module_text("Frame.TrimKeep"),
		[](obs_properties_t *p, obs_property_t *, void *d) {
			auto *frame = static_cast<AutoFrame *>(d);
			const bool saved_one = frame && frame->remember_current();
			if (frame)
				pick_list_refresh(p, frame->source(), "rules_text", true);
			return saved_one;
		},
		data);
	obs_property_set_long_description(keep, obs_module_text("Frame.TrimKeep.Tip"));
	obs_property_t *wait =
		obs_properties_add_float_slider(props, "trim_wait", obs_module_text("Frame.TrimWait"), 0.0, 60.0, 0.5);
	obs_property_float_set_suffix(wait, " s");
	obs_property_set_long_description(wait, obs_module_text("Frame.TrimWait.Tip"));
	obs_property_t *tx =
		obs_properties_add_float(props, "trim_x", obs_module_text("Frame.TrimX"), 0.0, 8000.0, 1.0);
	obs_property_float_set_suffix(tx, " px");
	obs_property_t *ty =
		obs_properties_add_float(props, "trim_y", obs_module_text("Frame.TrimY"), 0.0, 8000.0, 1.0);
	obs_property_float_set_suffix(ty, " px");

	obs_properties_add_group(top, "bars_group", obs_module_text("Frame.BarsGroup"), OBS_GROUP_NORMAL, props);

	// 4. Shapes for programs: built in for common emulators, and the
	// owner's own, chosen from what is open or found on the computer.
	props = obs_properties_create();
	obs_property_t *known = obs_properties_add_bool(props, "known_programs", obs_module_text("Frame.Known"));
	obs_property_set_long_description(known, obs_module_text("Frame.Known.Tip"));
	obs_property_t *forget = obs_properties_add_button2(
		props, "learned_forget", obs_module_text("Frame.Forget"),
		[](obs_properties_t *, obs_property_t *, void *d) {
			if (auto *frame = static_cast<AutoFrame *>(d))
				frame->forget_learned();
			return false;
		},
		data);
	// The tooltip says what is remembered now.
	const std::string remembered = self ? self->learned_now() : std::string();
	const std::string forget_tip = std::string(obs_module_text("Frame.Forget.Tip")) + " " +
				       (remembered.empty() ? obs_module_text("Frame.Forget.None") : remembered);
	obs_property_set_long_description(forget, forget_tip.c_str());
	obs_property_t *program = obs_properties_add_list(props, "rule_program", obs_module_text("Frame.RuleProgram"),
							  OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
	if (self) {
		std::vector<std::string> names;
		const std::string captured = self->captured_executable();
		if (!captured.empty())
			names.push_back(captured);
		for (const std::string &name : open_program_names())
			if (std::find(names.begin(), names.end(), name) == names.end())
				names.push_back(name);
		for (const std::string &name : names)
			obs_property_list_add_string(program, name.c_str(), name.c_str());
	}
	obs_property_t *browse = obs_properties_add_path(props, "rule_browse", obs_module_text("Frame.RuleBrowse"),
							 OBS_PATH_FILE, "Programs (*.exe);;All files (*.*)", nullptr);
	obs_property_set_modified_callback(browse, [](obs_properties_t *, obs_property_t *, obs_data_t *settings) {
		// Choosing a file fills in the program box above.
		std::string path = obs_data_get_string(settings, "rule_browse");
		if (path.empty())
			return false;
		const size_t slash = path.find_last_of("/\\");
		if (slash != std::string::npos)
			path = path.substr(slash + 1);
		obs_data_set_string(settings, "rule_program", path.c_str());
		obs_data_set_string(settings, "rule_browse", "");
		return true;
	});
	obs_property_t *rule_shape = obs_properties_add_list(props, "rule_shape", obs_module_text("Frame.RuleShape"),
							     OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
	for (const char *name : {"4:3", "64:49", "8:7", "3:2", "10:9", "16:9", "16:10", "5:4", "1:1"})
		obs_property_list_add_string(rule_shape, name, name);
	obs_properties_add_button2(
		props, "rule_add", obs_module_text("Frame.RuleAdd"),
		[](obs_properties_t *p, obs_property_t *, void *d) {
			auto *frame = static_cast<AutoFrame *>(d);
			const bool saved_one = frame && frame->remember_shape();
			if (frame)
				pick_list_refresh(p, frame->source(), "rules_text", true);
			return saved_one;
		},
		data);
	obs_properties_add_group(top, "programs_group", obs_module_text("Frame.ProgramsGroup"), OBS_GROUP_NORMAL,
				 props);
	// What has been saved, one row each, each with its own remove button.
	PickList saved;
	saved.setting = "rules_text";
	saved.as_text = true;
	saved.title = "Frame.Rules";
	pick_list_add(top, self ? self->source() : nullptr, saved);

	// 5. Movement.
	props = obs_properties_create();
	obs_property_t *glide =
		obs_properties_add_int_slider(props, "glide_ms", obs_module_text("Frame.Glide"), 0, 5000, 50);
	obs_property_int_set_suffix(glide, " ms");
	add_easing_properties(props, "");

	obs_properties_add_group(top, "motion_group", obs_module_text("Frame.MotionGroup"), OBS_GROUP_NORMAL, props);

	obs_property_t *help = obs_properties_add_text(top, "help", obs_module_text("Frame.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(help, true);
	return top;
}

void get_defaults(obs_data_t *settings)
{
	obs_video_info ovi;
	const bool have = obs_get_video_info(&ovi);
	obs_data_set_default_int(settings, "frame_mode", 0);
	obs_data_set_default_int(settings, "frame_width", have ? ovi.base_width : 1920);
	obs_data_set_default_int(settings, "frame_height", have ? ovi.base_height : 1080);
	obs_data_set_default_int(settings, "fit", (int)FitMode::Fit);
	obs_data_set_default_int(settings, "shape", (int)Shape::Source);
	obs_data_set_default_double(settings, "custom_w", 4.0);
	obs_data_set_default_double(settings, "custom_h", 3.0);
	obs_data_set_default_int(settings, "align_x", 1);
	obs_data_set_default_int(settings, "align_y", 1);
	obs_data_set_default_bool(settings, "sharp", false);
	obs_data_set_default_int(settings, "trim", (int)Trim::Automatic);
	obs_data_set_default_double(settings, "trim_wait", 3.0);
	obs_data_set_default_double(settings, "trim_x", 0.0);
	obs_data_set_default_double(settings, "trim_y", 0.0);
	obs_data_set_default_double(settings, "force_ratio", 0.0);
	obs_data_set_default_bool(settings, "known_programs", true);
	obs_data_set_default_string(settings, "rule_shape", "4:3");
	obs_data_set_default_int(settings, "glide_ms", 350);
	obs_data_set_default_int(settings, "ease_curve", (int)EaseCurve::Cubic);
	obs_data_set_default_int(settings, "ease_mode", (int)EaseMode::InOut);
}

// ============================================================ Picture Shape
// A key for Auto Frame: switching this filter on tells the Auto Frame on the
// same source which shape the picture has (or to go back to working it out),
// then it switches itself off again. Point a Stream Deck Filter key at it.

thread_local bool t_shape_self_change = false;

class PictureShape {
public:
	PictureShape(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		update(settings);
		signal_handler_connect(obs_source_get_signal_handler(source_), "enable", enable_cb, this);
		proc_handler_add(
			obs_source_get_proc_handler(source_), "void trigger()",
			[](void *data, calldata_t *) { static_cast<PictureShape *>(data)->pending_ = true; }, this);
	}

	~PictureShape()
	{
		signal_handler_disconnect(obs_source_get_signal_handler(source_), "enable", enable_cb, this);
		detach();
	}

	void update(obs_data_t *settings)
	{
		std::lock_guard lock(mutex_);
		const std::string shape = obs_data_get_string(settings, "shape");
		ratio_ = shape == "custom" ? parse_ratio(obs_data_get_string(settings, "custom")) : parse_ratio(shape);
	}

	void tick()
	{
		if (!ready_) {
			quiet_enable(false); // a key saved in the "on" state must not fire at start-up
			ready_ = true;
			return;
		}
		if (!pending_.exchange(false))
			return;
		double ratio;
		{
			std::lock_guard lock(mutex_);
			ratio = ratio_;
		}
		if (parent_) {
			obs_source_enum_filters(
				parent_,
				[](obs_source_t *, obs_source_t *filter, void *param) {
					const char *id = obs_source_get_unversioned_id(filter);
					if (id && strcmp(id, kAutoFrameId) == 0) {
						calldata_t cd = {};
						calldata_set_float(&cd, "ratio", *static_cast<double *>(param));
						proc_handler_call(obs_source_get_proc_handler(filter),
								  "set_picture_shape", &cd);
						calldata_free(&cd);
					}
				},
				&ratio);
		}
		quiet_enable(false);
	}

	void attach(obs_source_t *parent)
	{
		detach();
		parent_ = parent;
		const std::string name = obs_source_get_name(source_) ? obs_source_get_name(source_) : "";
		std::string text = obs_module_text("Hotkey.PictureShape");
		const size_t at = text.find("%1");
		if (at != std::string::npos)
			text.replace(at, 2, name);
		hotkey_ = obs_hotkey_register_source(
			parent, ("stagekit.shape." + name).c_str(), text.c_str(),
			[](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
				if (pressed)
					static_cast<PictureShape *>(data)->pending_ = true;
			},
			this);
	}

	void detach()
	{
		if (hotkey_ != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(hotkey_);
		hotkey_ = OBS_INVALID_HOTKEY_ID;
		parent_ = nullptr;
	}

	obs_source_t *source() const { return source_; }

private:
	void quiet_enable(bool enabled)
	{
		if (obs_source_enabled(source_) == enabled)
			return;
		t_shape_self_change = true;
		obs_source_set_enabled(source_, enabled);
		t_shape_self_change = false;
	}

	static void enable_cb(void *data, calldata_t *cd)
	{
		auto *self = static_cast<PictureShape *>(data);
		if (!t_shape_self_change && self->ready_ && calldata_bool(cd, "enabled"))
			self->pending_ = true;
	}

	obs_source_t *source_;
	obs_source_t *parent_ = nullptr;
	std::mutex mutex_;
	double ratio_ = 0.0;
	std::atomic<bool> pending_{false};
	std::atomic<bool> ready_{false};
	obs_hotkey_id hotkey_ = OBS_INVALID_HOTKEY_ID;
};

obs_properties_t *picture_shape_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *shape = obs_properties_add_list(props, "shape", obs_module_text("Shape.Shape"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(shape, obs_module_text("Shape.Automatic"), "0");
	const char *shapes[][2] = {{"4:3", "4:3"},     {"Shape.Snes", "64:49"}, {"3:2", "3:2"}, {"8:7", "8:7"},
				   {"10:9", "10:9"},   {"16:15", "16:15"},      {"5:4", "5:4"}, {"1:1", "1:1"},
				   {"16:10", "16:10"}, {"16:9", "16:9"}};
	for (const auto &entry : shapes)
		obs_property_list_add_string(
			shape, strncmp(entry[0], "Shape.", 6) == 0 ? obs_module_text(entry[0]) : entry[0], entry[1]);
	obs_property_list_add_string(shape, obs_module_text("Frame.Shape.Custom"), "custom");
	obs_property_set_modified_callback(shape, [](obs_properties_t *p, obs_property_t *, obs_data_t *settings) {
		obs_property_set_visible(obs_properties_get(p, "custom"),
					 strcmp(obs_data_get_string(settings, "shape"), "custom") == 0);
		return true;
	});
	obs_properties_add_text(props, "custom", obs_module_text("Shape.Custom"), OBS_TEXT_DEFAULT);
	obs_property_t *help = obs_properties_add_text(props, "help", obs_module_text("Shape.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(help, true);
	return props;
}

} // namespace

bool auto_frame_picture(obs_source_t *source, double &x, double &y, double &width, double &height)
{
	struct Find {
		obs_source_t *frame = nullptr;
		double left = 0.0, top = 0.0; // room added by shadows after the frame
	} find;
	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			auto *f = static_cast<Find *>(param);
			const char *id = obs_source_get_unversioned_id(filter);
			if (!id || !obs_source_enabled(filter))
				return;
			if (strcmp(id, kAutoFrameId) == 0) {
				f->frame = filter;
				f->left = f->top = 0.0;
			} else if (f->frame && strcmp(id, "stagekit_shadow_backdrop") == 0) {
				obs_data_t *settings = obs_source_get_settings(filter);
				f->left += (double)obs_data_get_int(settings, "adjusted_left");
				f->top += (double)obs_data_get_int(settings, "adjusted_top");
				obs_data_release(settings);
			}
		},
		&find);
	if (!find.frame)
		return false;
	calldata_t cd = {};
	bool found = false;
	if (proc_handler_call(obs_source_get_proc_handler(find.frame), "get_picture", &cd)) {
		const double pw = calldata_float(&cd, "width"), ph = calldata_float(&cd, "height");
		if (pw >= 1.0 && ph >= 1.0) {
			x = calldata_float(&cd, "x") + find.left;
			y = calldata_float(&cd, "y") + find.top;
			width = pw;
			height = ph;
			found = true;
		}
	}
	calldata_free(&cd);
	return found;
}

void register_auto_frame()
{
	obs_source_info info = {};
	info.id = kAutoFrameId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	// Colour-aware, like every StageKit filter that draws: see the Shadow,
	// Glow & Outline filter.
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = [](void *) {
		return obs_module_text("Frame.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new AutoFrame(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<AutoFrame *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<AutoFrame *>(data)->update(settings);
	};
	info.get_defaults = get_defaults;
	info.get_properties = get_properties;
	info.get_width = [](void *data) {
		return static_cast<AutoFrame *>(data)->width();
	};
	info.get_height = [](void *data) {
		return static_cast<AutoFrame *>(data)->height();
	};
	info.video_tick = [](void *data, float seconds) {
		static_cast<AutoFrame *>(data)->tick(seconds);
	};
	info.video_render = [](void *data, gs_effect_t *) {
		static_cast<AutoFrame *>(data)->render();
	};
	obs_register_source(&info);

	obs_source_info shape = {};
	shape.id = kPictureShapeId;
	shape.type = OBS_SOURCE_TYPE_FILTER;
	shape.output_flags = OBS_SOURCE_VIDEO;
	shape.get_name = [](void *) {
		return obs_module_text("Shape.Name");
	};
	shape.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new PictureShape(settings, source);
	};
	shape.destroy = [](void *data) {
		delete static_cast<PictureShape *>(data);
	};
	shape.update = [](void *data, obs_data_t *settings) {
		static_cast<PictureShape *>(data)->update(settings);
	};
	shape.get_defaults = [](obs_data_t *settings) {
		obs_data_set_default_string(settings, "shape", "4:3");
		obs_data_set_default_string(settings, "custom", "4:3");
	};
	shape.get_properties = picture_shape_properties;
	shape.video_tick = [](void *data, float) {
		static_cast<PictureShape *>(data)->tick();
	};
	shape.video_render = [](void *data, gs_effect_t *) {
		obs_source_skip_video_filter(static_cast<PictureShape *>(data)->source());
	};
	shape.filter_add = [](void *data, obs_source_t *parent) {
		static_cast<PictureShape *>(data)->attach(parent);
	};
	shape.filter_remove = [](void *data, obs_source_t *) {
		static_cast<PictureShape *>(data)->detach();
	};
	obs_register_source(&shape);
}

} // namespace stagekit
