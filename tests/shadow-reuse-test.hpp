#pragma once

#include "replacement-frame.hpp"

#include <obs-module.h>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QStringList>
#include <QThread>
#include <array>
#include <atomic>
#include <functional>

namespace shadow_reuse_test {

struct Picture {
	std::atomic<uint32_t> width{120}, height{80};
	std::atomic<bool> background{false};
	obs_source_t *redraw_filter = nullptr; // held only by the fixture's redraw source
	bool refresh = false;
};

inline void configure_picture(Picture *picture, obs_data_t *settings)
{
	picture->width = uint32_t(obs_data_get_int(settings, "width"));
	picture->height = uint32_t(obs_data_get_int(settings, "height"));
	picture->background = obs_data_get_bool(settings, "background");
}

inline void register_picture()
{
	static bool registered = false;
	if (registered)
		return;
	registered = true;
	obs_source_info info{};
	info.id = "stagekit_test_shadow_picture";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	info.get_name = [](void *) {
		return "Shadow reuse fixture";
	};
	info.create = [](obs_data_t *settings, obs_source_t *) -> void * {
		auto *picture = new Picture();
		configure_picture(picture, settings);
		return picture;
	};
	info.destroy = [](void *data) {
		auto *picture = static_cast<Picture *>(data);
		obs_source_release(picture->redraw_filter);
		delete picture;
	};
	info.update = [](void *data, obs_data_t *settings) {
		configure_picture(static_cast<Picture *>(data), settings);
	};
	info.get_width = [](void *data) -> uint32_t {
		return static_cast<Picture *>(data)->width;
	};
	info.get_height = [](void *data) -> uint32_t {
		return static_cast<Picture *>(data)->height;
	};
	info.video_render = [](void *data, gs_effect_t *) {
		auto *picture = static_cast<Picture *>(data);
		const uint32_t w = picture->width, h = picture->height;
		if (picture->redraw_filter) {
			calldata_t cd{};
			calldata_set_bool(&cd, "redraw", true);
			calldata_set_bool(&cd, "refresh", picture->refresh);
			proc_handler_call(obs_source_get_proc_handler(picture->redraw_filter), "shadow_reuse_test",
					  &cd);
			auto *texture = static_cast<gs_texture_t *>(calldata_ptr(&cd, "texture"));
			if (texture) {
				auto *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
				auto *image = gs_effect_get_param_by_name(effect, "image");
				const bool linear = gs_get_linear_srgb(), previous = gs_framebuffer_srgb_enabled();
				gs_enable_framebuffer_srgb(linear);
				if (linear)
					gs_effect_set_texture_srgb(image, texture);
				else
					gs_effect_set_texture(image, texture);
				while (gs_effect_loop(effect, "Draw"))
					gs_draw_sprite(texture, 0, w, h);
				gs_enable_framebuffer_srgb(previous);
			}
			calldata_free(&cd);
			return;
		}
		auto *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
		auto rectangle = [&](float x, float y, uint32_t cx, uint32_t cy, float r, float g, float b, float a) {
			vec4 color;
			vec4_set(&color, r, g, b, a);
			gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &color);
			gs_matrix_push();
			gs_matrix_translate3f(x, y, 0.0f);
			while (gs_effect_loop(effect, "Solid"))
				gs_draw_sprite(nullptr, 0, cx, cy);
			gs_matrix_pop();
		};
		if (picture->background) {
			rectangle(0, 0, w, h, 0.18f, 0.23f, 0.29f, 1.0f);
		} else {
			// Colour, transparent margins and fractional edges all matter to
			// the outlined caster, picture shadows and the final composition.
			rectangle(7.25f, 6.5f, w - 15, h - 13, 0.9f, 0.14f, 0.08f, 0.72f);
			rectangle(20.5f, 12.25f, w / 3, h / 2, 0.08f, 0.35f, 0.95f, 1.0f);
			rectangle(float(w) * 0.55f, float(h) * 0.4f, w / 4, h / 3, 0.1f, 0.9f, 0.25f, 0.45f);
		}
	};
	obs_register_source(&info);
}

template<typename Predicate> inline bool until(Predicate predicate)
{
	QElapsedTimer timer;
	timer.start();
	do {
		QCoreApplication::processEvents();
		if (predicate())
			return true;
		QThread::msleep(15);
	} while (timer.elapsed() < 3500);
	return predicate();
}

struct Counts {
	bool available = false;
	long long preparations = 0, reuses = 0, passes = 0, stages = 0, settings = 0;
	long long retentions = 0, eligibility_checks = 0;
};

inline Counts counts(obs_source_t *filter, int bypass = -1)
{
	calldata_t cd{};
	if (bypass >= 0)
		calldata_set_bool(&cd, "bypass", bypass != 0);
	Counts out;
	out.available = filter && proc_handler_call(obs_source_get_proc_handler(filter), "shadow_reuse_test", &cd);
	out.preparations = calldata_int(&cd, "preparations");
	out.reuses = calldata_int(&cd, "reuses");
	out.passes = calldata_int(&cd, "passes");
	out.stages = calldata_int(&cd, "stages");
	out.settings = calldata_int(&cd, "settings");
	out.retentions = calldata_int(&cd, "retentions");
	out.eligibility_checks = calldata_int(&cd, "eligibility_checks");
	calldata_free(&cd);
	return out;
}

inline Counts difference(const Counts &after, const Counts &before)
{
	Counts out = after;
	out.preparations -= before.preparations;
	out.reuses -= before.reuses;
	out.passes -= before.passes;
	out.retentions -= before.retentions;
	out.eligibility_checks -= before.eligibility_checks;
	return out;
}

struct Fixture {
	uint32_t width = 320, height = 180;
	obs_source_t *picture = nullptr, *background = nullptr;
	std::array<obs_source_t *, 2> scenes{}, shadows{}, blurs{};
	std::array<obs_sceneitem_t *, 2> items{}, others{};
	std::array<bool, 2> showing{}, attached{};
	uint64_t last_sample = 0;
	bool valid = false;

	Fixture()
	{
		auto make_picture = [](const char *name, uint32_t w, uint32_t h, bool back) {
			obs_data_t *settings = obs_data_create();
			obs_data_set_int(settings, "width", w);
			obs_data_set_int(settings, "height", h);
			obs_data_set_bool(settings, "background", back);
			obs_source_t *source =
				obs_source_create_private("stagekit_test_shadow_picture", name, settings);
			obs_data_release(settings);
			return source;
		};
		picture = make_picture("Shadow reuse picture", 120, 80, false);
		background = make_picture("Shadow reuse background", width, height, true);
		if (!picture || !background)
			return;
		for (size_t i = 0; i < scenes.size(); i++) {
			obs_data_t *settings = obs_data_create();
			obs_data_set_bool(settings, "custom_size", true);
			obs_data_set_int(settings, "cx", width);
			obs_data_set_int(settings, "cy", height);
			scenes[i] = obs_source_create_private(
				"scene", i ? "Shadow reference scene" : "Shadow reuse scene", settings);
			obs_data_release(settings);
			if (!scenes[i])
				return;
			obs_source_load(scenes[i]);
			populate(i);
			settings = obs_data_create();
			obs_data_set_int(settings, "ramp_ms", 0);
			obs_data_set_int(settings, "ramp_out_ms", 0);
			shadows[i] =
				obs_source_create_private("stagekit_scene_shadow", "Shadow reuse shadow", settings);
			obs_data_release(settings);
			settings = obs_data_create();
			obs_data_set_int(settings, "blur_area", 1);
			obs_data_set_int(settings, "blur_type", 0);
			obs_data_set_double(settings, "strength", 12.0);
			obs_data_set_int(settings, "ramp_in_ms", 0);
			obs_data_set_int(settings, "ramp_out_ms", 0);
			blurs[i] = obs_source_create_private("stagekit_scene_blur", "Shadow reuse blur", settings);
			obs_data_release(settings);
			if (!shadows[i] || !blurs[i] || !items[i] || !others[i])
				return;
			obs_source_filter_add(scenes[i], shadows[i]);
			obs_source_filter_add(scenes[i], blurs[i]);
			attached[i] = true;
			obs_source_inc_showing(scenes[i]);
			showing[i] = true;
		}
		valid = counts(shadows[0]).available && counts(shadows[1], 1).available;
	}

	~Fixture()
	{
		for (size_t i = 0; i < scenes.size(); i++) {
			if (showing[i])
				obs_source_dec_showing(scenes[i]);
			if (attached[i]) {
				obs_source_filter_remove(scenes[i], blurs[i]);
				obs_source_filter_remove(scenes[i], shadows[i]);
			}
			obs_source_release(blurs[i]);
			obs_source_release(shadows[i]);
			obs_source_release(scenes[i]);
		}
		obs_source_release(background);
		obs_source_release(picture);
	}

	void populate(size_t i)
	{
		auto *scene = obs_scene_from_source(scenes[i]);
		obs_scene_add(scene, background);
		items[i] = obs_scene_add(scene, picture);
		others[i] = obs_scene_add(scene, picture);
		if (!items[i] || !others[i])
			return;
		vec2 position;
		vec2_set(&position, 62.25f, 40.5f);
		obs_sceneitem_set_pos(items[i], &position);
		vec2_set(&position, 126.5f, 72.25f);
		obs_sceneitem_set_pos(others[i], &position);
		obs_sceneitem_set_visible(others[i], false);
	}

	bool configure(const std::function<void(obs_data_t *)> &change)
	{
		const std::array<long long, 2> before = {counts(shadows[0]).settings, counts(shadows[1]).settings};
		for (obs_source_t *shadow : shadows) {
			obs_data_t *settings = obs_source_get_settings(shadow);
			change(settings);
			obs_source_update(shadow, settings);
			obs_data_release(settings);
		}
		return until([&] {
			return counts(shadows[0]).settings > before[0] && counts(shadows[1]).settings > before[1];
		});
	}

	void resize(uint32_t w, uint32_t h)
	{
		width = w;
		height = h;
		obs_data_t *settings = obs_source_get_settings(background);
		obs_data_set_int(settings, "width", w);
		obs_data_set_int(settings, "height", h);
		obs_source_update(background, settings);
		obs_data_release(settings);
		for (size_t i = 0; i < scenes.size(); i++) {
			settings = obs_source_get_settings(scenes[i]);
			obs_data_set_int(settings, "cx", w);
			obs_data_set_int(settings, "cy", h);
			obs_data_release(settings);
			// Loading the scene's settings changes its custom canvas and
			// removes the old items; the same live filters keep their caches.
			obs_source_load(scenes[i]);
			populate(i);
		}
	}
};

struct Sample {
	std::array<Counts, 2> work{};
	std::array<std::vector<uint8_t>, 2> pixels;
	bool repeated_equal = false, repeated_idle = false, same_frame = false;
};

inline Sample sample(Fixture &fixture, bool linear = false)
{
	until([&] { return obs_get_video_frame_time() != fixture.last_sample; });
	struct Job {
		Fixture *fixture;
		bool linear;
		Sample out;
	} job{&fixture, linear, {}};
	obs_queue_task(
		OBS_TASK_GRAPHICS,
		[](void *data) {
			auto &job = *static_cast<Job *>(data);
			auto &fixture = *job.fixture;
			// OBS_TASK_GRAPHICS selects the video thread; its callbacks must
			// still enter graphics before reading or changing colour state.
			obs_enter_graphics();
			const uint64_t frame = obs_get_video_frame_time();
			const bool previous = gs_get_linear_srgb();
			gs_set_linear_srgb(job.linear);
			for (size_t i = 0; i < fixture.scenes.size(); i++) {
				const Counts before = counts(fixture.shadows[i]);
				job.out.pixels[i] =
					replacement_test::render(fixture.scenes[i], fixture.width, fixture.height);
				job.out.work[i] = difference(counts(fixture.shadows[i]), before);
			}
			const Counts before = counts(fixture.shadows[0]);
			const auto again = replacement_test::render(fixture.scenes[0], fixture.width, fixture.height);
			const Counts repeated = difference(counts(fixture.shadows[0]), before);
			job.out.repeated_equal = again == job.out.pixels[0];
			job.out.repeated_idle = repeated.preparations == 0 && repeated.reuses == 0 &&
						repeated.passes == 0 && repeated.retentions == 0 &&
						repeated.eligibility_checks == 0;
			job.out.same_frame = frame == obs_get_video_frame_time();
			fixture.last_sample = frame;
			gs_set_linear_srgb(previous);
			obs_leave_graphics();
		},
		&job, true);
	return job.out;
}

inline bool visible(const std::vector<uint8_t> &pixels)
{
	size_t colored = 0;
	for (size_t i = 0; i + 3 < pixels.size(); i += 4)
		colored += pixels[i + 3] > 200 && int(pixels[i]) > int(pixels[i + 1]) + 20;
	return colored > 200;
}

inline bool check_sample(const char *name, const Sample &s, bool reuse, QStringList &issues)
{
	const Counts &a = s.work[0], &b = s.work[1];
	const bool equal = s.same_frame && s.pixels[0] == s.pixels[1] && visible(s.pixels[0]);
	const bool work = reuse ? a.preparations == 1 && a.reuses == 1 && a.passes > 0 && b.preparations == 2 &&
					  b.reuses == 0 && b.passes == a.passes * 2
				: a.preparations >= 2 && a.preparations == b.preparations && a.reuses == 0 &&
					  b.reuses == 0 && a.passes > 0 && a.passes == b.passes;
	const bool tracking = b.retentions == 0 && b.eligibility_checks == 0 &&
			      (reuse ? a.retentions > 0 : a.retentions == 0 && a.eligibility_checks == 0);
	const bool ok = equal && work && tracking && s.repeated_equal && s.repeated_idle;
	if (!ok)
		issues << QStringLiteral(
				  "Shadow reuse %1: equal=%2 same frame=%3 repeat=%4/%5; current prep/reuse/passes/stages=%6/%7/%8/%9, reference=%10/%11/%12/%13")
				  .arg(QString::fromUtf8(name))
				  .arg(equal)
				  .arg(s.same_frame)
				  .arg(s.repeated_equal)
				  .arg(s.repeated_idle)
				  .arg(a.preparations)
				  .arg(a.reuses)
				  .arg(a.passes)
				  .arg(a.stages)
				  .arg(b.preparations)
				  .arg(b.reuses)
				  .arg(b.passes)
				  .arg(b.stages);
	if (!tracking)
		issues << QStringLiteral("Shadow reuse %1: retention/checks current=%2/%3, reference=%4/%5")
				  .arg(QString::fromUtf8(name))
				  .arg(a.retentions)
				  .arg(a.eligibility_checks)
				  .arg(b.retentions)
				  .arg(b.eligibility_checks);
	blog(LOG_INFO, "Shadow reuse %s: exact=%d, prepared=%lld/%lld, reused=%lld/%lld, passes=%lld/%lld", name,
	     int(equal), a.preparations, b.preparations, a.reuses, b.reuses, a.passes, b.passes);
	return ok;
}

inline void check_no_consumer(const char *name, const Sample &s, QStringList &issues)
{
	const Counts &a = s.work[0], &b = s.work[1];
	const bool exact = s.same_frame && s.pixels[0] == s.pixels[1] && visible(s.pixels[0]);
	const bool work = a.preparations == 1 && b.preparations == 1 && a.reuses == 0 && b.reuses == 0 &&
			  a.passes > 0 && a.passes == b.passes && a.retentions == 0 && b.retentions == 0 &&
			  a.eligibility_checks == 0 && b.eligibility_checks == 0;
	if (!exact || !work || !s.repeated_equal || !s.repeated_idle)
		issues << QStringLiteral(
				  "Shadow reuse %1: exact=%2 repeat=%3/%4; prep/reuse/passes/retention/checks current=%5/%6/%7/%8/%9, reference=%10/%11/%12/%13/%14")
				  .arg(QString::fromUtf8(name))
				  .arg(exact)
				  .arg(s.repeated_equal)
				  .arg(s.repeated_idle)
				  .arg(a.preparations)
				  .arg(a.reuses)
				  .arg(a.passes)
				  .arg(a.retentions)
				  .arg(a.eligibility_checks)
				  .arg(b.preparations)
				  .arg(b.reuses)
				  .arg(b.passes)
				  .arg(b.retentions)
				  .arg(b.eligibility_checks);
	blog(LOG_INFO, "Shadow reuse %s: exact=%d, prepared=%lld/%lld, retention=%lld/%lld, checks=%lld/%lld", name,
	     int(exact), a.preparations, b.preparations, a.retentions, b.retentions, a.eligibility_checks,
	     b.eligibility_checks);
}

inline bool check_non_consumers(Fixture &fixture, QStringList &issues)
{
	for (size_t i = 0; i < fixture.scenes.size(); i++)
		obs_source_filter_remove(fixture.scenes[i], fixture.blurs[i]);
	check_no_consumer("no Scene Blur", sample(fixture), issues);
	for (size_t i = 0; i < fixture.scenes.size(); i++)
		obs_source_filter_add(fixture.scenes[i], fixture.blurs[i]);

	for (auto *blur : fixture.blurs)
		obs_source_set_enabled(blur, false);
	check_no_consumer("disabled Scene Blur", sample(fixture), issues);
	for (auto *blur : fixture.blurs)
		obs_source_set_enabled(blur, true);

	auto area = [&](int value) {
		for (auto *blur : fixture.blurs) {
			obs_data_t *settings = obs_source_get_settings(blur);
			obs_data_set_int(settings, "blur_area", value);
			obs_source_update(blur, settings);
			obs_data_release(settings);
		}
	};
	area(0);
	Sample whole;
	const bool whole_ready = until([&] {
		whole = sample(fixture);
		const auto &a = whole.work[0], &b = whole.work[1];
		// OBS accepts the blur-area edit before its native render path has
		// necessarily retired the preceding per-source reuse state.
		// Do not validate a transitional sample as a whole-scene frame.
		return whole.same_frame && whole.repeated_equal && whole.repeated_idle &&
		       whole.pixels[0] == whole.pixels[1] && visible(whole.pixels[0]) && a.preparations == 1 &&
		       b.preparations == 1 && a.reuses == 0 && b.reuses == 0 && a.passes > 0 && a.passes == b.passes &&
		       a.retentions == 0 && b.retentions == 0 && a.eligibility_checks == 0 && b.eligibility_checks == 0;
	});
	check_no_consumer("whole-scene Scene Blur", whole, issues);
	area(1);
	const bool restored = until([&] {
		const Sample ready = sample(fixture);
		return ready.work[0].preparations == 1 && ready.work[0].reuses == 1 && ready.work[1].preparations == 2;
	});
	if (!whole_ready || !restored)
		issues << QStringLiteral("Shadow reuse: changing Scene Blur area did not complete");
	return whole_ready && restored;
}

inline void check_guards(Fixture &fixture, QStringList &issues)
{
	std::array<obs_source_t *, 2> redraws{};
	for (size_t i = 0; i < redraws.size(); i++) {
		obs_data_t *settings = obs_data_create();
		obs_data_set_int(settings, "width", fixture.width);
		obs_data_set_int(settings, "height", fixture.height);
		redraws[i] =
			obs_source_create_private("stagekit_test_shadow_picture", "Shadow direct redraw", settings);
		obs_data_release(settings);
		if (redraws[i])
			static_cast<Picture *>(obs_obj_get_data(redraws[i]))->redraw_filter =
				obs_source_get_ref(fixture.shadows[i]);
	}
	if (!redraws[0] || !redraws[1]) {
		issues << QStringLiteral("Shadow reuse: the direct-redraw fixture could not be created");
		for (auto *source : redraws)
			obs_source_release(source);
		return;
	}
	until([&] { return obs_get_video_frame_time() != fixture.last_sample; });
	struct Job {
		Fixture *fixture;
		std::array<obs_source_t *, 2> redraws;
		QStringList issues;
		std::vector<uint8_t> before;
	} job{&fixture, redraws, {}, {}};
	obs_queue_task(
		OBS_TASK_GRAPHICS,
		[](void *data) {
			auto &job = *static_cast<Job *>(data);
			auto &fixture = *job.fixture;
			obs_enter_graphics();
			const uint64_t frame = obs_get_video_frame_time();
			const bool previous = gs_get_linear_srgb();
			gs_set_linear_srgb(false);
			for (size_t i = 0; i < fixture.scenes.size(); i++) {
				const auto pixels =
					replacement_test::render(fixture.scenes[i], fixture.width, fixture.height);
				if (!i)
					job.before = pixels;
			}
			struct Step {
				const char *name;
				bool linear, refresh, reuse;
			};
			const Step steps[] = {{"direct redraw", false, false, true},
					      {"repeated direct redraw", false, false, true},
					      {"same-frame linear change", true, false, false},
					      {"same-frame linear restoration", false, false, false},
					      {"same-frame native settings change", false, true, false},
					      {"redraw after settings change", false, false, false}};
			for (const Step &step : steps) {
				gs_set_linear_srgb(step.linear);
				std::array<Counts, 2> work{};
				std::array<std::vector<uint8_t>, 2> pixels;
				bool refreshed = true;
				for (size_t i = 0; i < fixture.scenes.size(); i++) {
					if (step.refresh) {
						obs_data_t *settings = obs_source_get_settings(fixture.shadows[i]);
						obs_data_set_int(settings, "outline_color", 0xFFE03080);
						obs_data_release(settings);
					}
					static_cast<Picture *>(obs_obj_get_data(job.redraws[i]))->refresh =
						step.refresh;
					const Counts before = counts(fixture.shadows[i]);
					pixels[i] =
						replacement_test::render(job.redraws[i], fixture.width, fixture.height);
					work[i] = difference(counts(fixture.shadows[i]), before);
					if (step.refresh)
						refreshed = refreshed && work[i].settings == before.settings + 1;
				}
				const Counts &a = work[0], &b = work[1];
				const bool current = step.reuse ? a.preparations == 0 && a.reuses == 1 && a.passes == 0
								: a.preparations == 1 && a.reuses == 0 &&
									  a.passes == b.passes;
				const bool exact = pixels[0] == pixels[1] && visible(pixels[0]);
				if (!current || !exact || !refreshed || b.preparations != 1 || b.reuses != 0 ||
				    b.passes <= 0)
					job.issues
						<< QStringLiteral(
							   "Shadow reuse %1: exact=%2 refresh=%3; prep/reuse/passes current=%4/%5/%6, reference=%7/%8/%9")
							   .arg(QString::fromUtf8(step.name))
							   .arg(exact)
							   .arg(refreshed)
							   .arg(a.preparations)
							   .arg(a.reuses)
							   .arg(a.passes)
							   .arg(b.preparations)
							   .arg(b.reuses)
							   .arg(b.passes);
				blog(LOG_INFO,
				     "Shadow reuse %s: exact=%d, prepared=%lld/%lld, reused=%lld/%lld, passes=%lld/%lld",
				     step.name, int(exact), a.preparations, b.preparations, a.reuses, b.reuses,
				     a.passes, b.passes);
			}
			if (frame != obs_get_video_frame_time())
				job.issues << QStringLiteral(
					"Shadow reuse: the direct-redraw checks crossed a video frame");
			fixture.last_sample = frame;
			gs_set_linear_srgb(previous);
			obs_leave_graphics();
		},
		&job, true);
	issues.append(job.issues);
	for (auto *source : redraws)
		obs_source_release(source);
	const Sample next = sample(fixture);
	check_sample("new frame after native settings change", next, true, issues);
	if (next.pixels[0] == job.before)
		issues << QStringLiteral("Shadow reuse: the next frame did not show the changed outline colour");
}

inline void check(QStringList &issues)
{
	register_picture();
	Fixture fixture;
	if (!fixture.valid) {
		issues << QStringLiteral("Shadow reuse: the scene, filters or counter procedure could not be created");
		return;
	}
	if (!until([&] { return sample(fixture).work[0].reuses > 0; })) {
		issues << QStringLiteral("Shadow reuse: Scene Shadow and each-source Scene Blur did not become ready");
		return;
	}
	if (!check_non_consumers(fixture, issues))
		return;
	struct Style {
		const char *name;
		bool outline, shadow, glow, picture;
		int design, corners;
		double opacity, line_opacity;
	};
	const Style styles[] = {
		{"drop shadow", false, true, false, false, 0, 0, 60, 100},
		{"coloured glow", false, true, true, false, 0, 0, 75, 100},
		{"picture shadow", false, true, false, true, 0, 0, 65, 100},
		{"rounded solid outline", true, true, false, false, 0, 1, 60, 100},
		{"Neon", true, true, false, true, 5, 1, 80, 85},
		{"coloured outline", true, true, false, true, 4, 1, 45, 65},
		{"moving outline", true, true, false, false, 7, 1, 60, 100},
		{"procedural outline", true, true, false, true, 12, 1, 60, 100},
		{"zero shadow opacity", true, true, false, false, 0, 0, 0, 45},
		{"low shadow opacity", true, true, false, true, 0, 0, 0.1, 60},
		{"Neon without shadow", true, false, false, false, 5, 1, 60, 75},
	};
	for (const Style &style : styles) {
		if (!fixture.configure([&](obs_data_t *settings) {
			    obs_data_set_bool(settings, "shadow", style.shadow);
			    obs_data_set_int(settings, "shadow_type", style.glow ? 1 : 0);
			    obs_data_set_int(settings, "shadow_fill", style.picture ? 1 : 0);
			    obs_data_set_double(settings, "shadow_opacity", style.opacity);
			    obs_data_set_int(settings, "shadow_color", 0xFFB05010);
			    obs_data_set_double(settings, "softness", 20.0);
			    obs_data_set_bool(settings, "outline", style.outline);
			    obs_data_set_int(settings, "outline_design", style.design);
			    obs_data_set_int(settings, "outline_corners", style.corners);
			    obs_data_set_int(settings, "outline_color", 0xFF20A0F0);
			    obs_data_set_int(settings, "outline_color2", 0xFFF08020);
			    obs_data_set_double(settings, "outline_width", 6.0);
			    obs_data_set_double(settings, "outline_opacity", style.line_opacity);
		    })) {
			issues << QStringLiteral("Shadow reuse: the %1 settings update did not complete")
					  .arg(QString::fromUtf8(style.name));
			return;
		}
		check_sample(style.name, sample(fixture), true, issues);
	}
	check_sample("linear light", sample(fixture, true), true, issues);
	check_sample("raw colour after linear light", sample(fixture), true, issues);
	check_guards(fixture, issues);

	fixture.resize(384, 216);
	// Reloading these private scenes replaced their item IDs. Request a
	// native update and observe both complete pipelines after classification
	// (private scenes have no frontend revision). A settings acknowledgement
	// alone can precede the tick that consumes settings_changed_.
	if (!fixture.configure([](obs_data_t *) {}) || !until([&] {
		    return obs_source_get_width(fixture.background) == fixture.width &&
			   obs_source_get_width(fixture.scenes[0]) == fixture.width &&
			   obs_source_get_height(fixture.scenes[0]) == fixture.height &&
			   obs_source_get_width(fixture.scenes[1]) == fixture.width &&
			   obs_source_get_height(fixture.scenes[1]) == fixture.height;
	    })) {
		issues << QStringLiteral("Shadow reuse: resizing the fixture did not complete");
		return;
	}
	if (!until([&] {
		    const Sample ready = sample(fixture);
		    return ready.work[0].preparations == 1 && ready.work[0].reuses == 1 &&
			   ready.work[1].preparations == 2;
	    })) {
		issues << QStringLiteral("Shadow reuse: the resized scene items did not become classified");
		return;
	}
	check_sample("resized canvas", sample(fixture), true, issues);

	for (auto *item : fixture.others)
		obs_sceneitem_set_visible(item, true);
	if (!fixture.configure([](obs_data_t *settings) {
		    obs_data_set_bool(settings, "shadow", true);
		    obs_data_set_int(settings, "shadow_from", 1);
	    })) {
		issues << QStringLiteral("Shadow reuse: changing to multiple stages did not complete");
		return;
	}
	const Sample multiple = sample(fixture);
	check_sample("multiple stages", multiple, false, issues);
	if (multiple.work[0].stages < 2)
		issues << QStringLiteral("Shadow reuse: the multiple-stage fixture did not make two stages");

	for (auto *item : fixture.others)
		obs_sceneitem_set_visible(item, false);
	if (!fixture.configure([](obs_data_t *settings) {
		    obs_data_set_int(settings, "shadow_from", 0);
		    obs_data_set_int(settings, "outline_design", 0);
	    })) {
		issues << QStringLiteral("Shadow reuse: preparing the transition fixture did not complete");
		return;
	}
	std::array<obs_source_t *, 2> transitions{};
	for (size_t i = 0; i < transitions.size(); i++) {
		transitions[i] = obs_source_create_private("fade_transition", "Shadow reuse fade", nullptr);
		if (transitions[i]) {
			obs_sceneitem_set_transition(fixture.items[i], false, transitions[i]);
			obs_sceneitem_set_transition_duration(fixture.items[i], false, 1800);
		}
	}
	if (!transitions[0] || !transitions[1]) {
		issues << QStringLiteral("Shadow reuse: OBS's fade transition was unavailable for fallback coverage");
	} else {
		obs_queue_task(
			OBS_TASK_GRAPHICS,
			[](void *data) {
				auto *fixture = static_cast<Fixture *>(data);
				for (auto *item : fixture->items)
					obs_sceneitem_set_visible(item, false);
			},
			&fixture, true);
		if (!until([&] {
			    return obs_transition_get_time(transitions[0]) > 0.0f &&
				   obs_transition_get_time(transitions[0]) < 1.0f &&
				   obs_transition_get_time(transitions[1]) > 0.0f &&
				   obs_transition_get_time(transitions[1]) < 1.0f;
		    })) {
			issues << QStringLiteral("Shadow reuse: the source hide transitions did not start");
		} else {
			const Sample fading = sample(fixture);
			// OBS can start the two transitions at slightly different clock
			// times. Their work must match; exact output comparisons above
			// use stationary sources within one graphics frame.
			if (!fading.same_frame || fading.work[0].reuses != 0 || fading.work[1].reuses != 0 ||
			    fading.work[0].preparations < 2 ||
			    fading.work[0].preparations != fading.work[1].preparations || fading.work[0].passes <= 0 ||
			    fading.work[0].passes != fading.work[1].passes || fading.work[0].retentions != 0 ||
			    fading.work[1].retentions != 0 || fading.work[0].eligibility_checks != 0 ||
			    fading.work[1].eligibility_checks != 0)
				issues << QStringLiteral(
					"Shadow reuse: a source hide transition did not keep the original preparation path");
		}
	}
	for (size_t i = 0; i < transitions.size(); i++) {
		obs_sceneitem_set_visible(fixture.items[i], true);
		obs_sceneitem_set_transition(fixture.items[i], false, nullptr);
		obs_source_release(transitions[i]);
	}
}

} // namespace shadow_reuse_test
