#pragma once

#include "replacement-frame.hpp"
#include "../src/common/filter-input.hpp"

#include <obs-module.h>
#include <obs.hpp>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <util/platform.h>
#include <QElapsedTimer>
#include <QStringList>
#include <QThread>
#include <array>
#include <atomic>
#include <utility>

namespace visible_bounds_reuse_test {

inline constexpr uint32_t kWidth = 96, kHeight = 72;

struct Picture {
	std::atomic<uint32_t> width{kWidth};
	std::atomic<int> renders{0};
	std::atomic<bool> empty{false}, resize_once{false};
};

struct Filter {
	obs_source_t *source = nullptr;
	std::atomic<int> renders{0};
	bool mask = false;
};

inline void rectangle(float x, float y, uint32_t w, uint32_t h, float alpha)
{
	auto *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
	vec4 color;
	vec4_set(&color, alpha, alpha * 0.5f, 0.0f, alpha);
	gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &color);
	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	while (gs_effect_loop(effect, "Solid"))
		gs_draw_sprite(nullptr, 0, w, h);
	gs_matrix_pop();
}

inline void register_sources()
{
	static bool registered = false;
	if (std::exchange(registered, true))
		return;
	obs_source_info input{};
	input.id = "stagekit_test_visible_bounds_picture";
	input.type = OBS_SOURCE_TYPE_INPUT;
	input.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	input.get_name = [](void *) {
		return "Visible bounds picture fixture";
	};
	input.create = [](obs_data_t *, obs_source_t *) -> void * {
		return new Picture();
	};
	input.destroy = [](void *data) {
		delete static_cast<Picture *>(data);
	};
	input.get_width = [](void *data) -> uint32_t {
		return static_cast<Picture *>(data)->width.load();
	};
	input.get_height = [](void *) -> uint32_t {
		return kHeight;
	};
	input.video_render = [](void *data, gs_effect_t *) {
		auto &picture = *static_cast<Picture *>(data);
		++picture.renders;
		if (!picture.empty.load()) {
			// Asymmetric empty margins and a faint one-pixel fringe. The
			// corners stay empty so each faint edge pixel has an opaque
			// inward neighbor. Measuring trims to the 37 by 29 body.
			rectangle(12.0f, 17.0f, 1, 29, 16.0f / 255.0f);
			rectangle(50.0f, 17.0f, 1, 29, 16.0f / 255.0f);
			rectangle(13.0f, 16.0f, 37, 1, 16.0f / 255.0f);
			rectangle(13.0f, 46.0f, 37, 1, 16.0f / 255.0f);
			rectangle(13.0f, 17.0f, 37, 29, 1.0f);
		}
		if (picture.resize_once.exchange(false))
			++picture.width;
	};
	obs_register_source(&input);
	// A counted renderer with a real asynchronous frame supplying its
	// dimensions. Its flag must keep the existing no-bare-render rule.
	input.id = "stagekit_test_visible_bounds_async";
	input.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	obs_register_source(&input);

	obs_source_info filter{};
	filter.id = "stagekit_test_visible_bounds_filter";
	filter.type = OBS_SOURCE_TYPE_FILTER;
	filter.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	filter.get_name = [](void *) {
		return "Visible bounds filter fixture";
	};
	filter.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		auto *data = new Filter();
		data->source = source;
		data->mask = obs_data_get_bool(settings, "mask");
		return data;
	};
	filter.destroy = [](void *data) {
		delete static_cast<Filter *>(data);
	};
	filter.get_width = [](void *data) -> uint32_t {
		return obs_source_get_base_width(obs_filter_get_target(static_cast<Filter *>(data)->source));
	};
	filter.get_height = [](void *data) -> uint32_t {
		return obs_source_get_base_height(obs_filter_get_target(static_cast<Filter *>(data)->source));
	};
	filter.video_render = [](void *data, gs_effect_t *) {
		auto &filter = *static_cast<Filter *>(data);
		++filter.renders;
		if (filter.mask)
			rectangle(20.0f, 23.0f, 11, 9, 1.0f);
		else
			stagekit::skip_filter_input(filter.source);
	};
	obs_register_source(&filter);
}

struct Measurement {
	bool called = false, any = false;
	std::array<double, 4> box{};
	int pictures = 0, filters = 0;
};

inline Measurement measure(obs_source_t *scene, obs_sceneitem_t *item, Picture &picture, Filter *filter,
			   bool visible = true, bool leave_out_shadows = true)
{
	struct Job {
		obs_source_t *scene;
		obs_sceneitem_t *item;
		Picture *picture;
		Filter *filter;
		bool visible, leave_out_shadows;
		Measurement result;
	} job{scene, item, &picture, filter, visible, leave_out_shadows, {}};
	// Counters begin and end in one graphics task. Normal output rendering
	// cannot run between them, so these are actual measurement draws.
	obs_queue_task(
		OBS_TASK_GRAPHICS,
		[](void *param) {
			auto &job = *static_cast<Job *>(param);
			job.picture->renders = 0;
			if (job.filter)
				job.filter->renders = 0;
			calldata_t call{};
			calldata_set_ptr(&call, "scene", job.scene);
			calldata_set_ptr(&call, "item", job.item);
			calldata_set_bool(&call, "visible_pixels", job.visible);
			calldata_set_bool(&call, "leave_out_shadows", job.leave_out_shadows);
			job.result.called =
				proc_handler_call(obs_get_proc_handler(), "stagekit_test_measure_item", &call);
			job.result.any = calldata_bool(&call, "any");
			job.result.box = {calldata_float(&call, "x"), calldata_float(&call, "y"),
					  calldata_float(&call, "w"), calldata_float(&call, "h")};
			calldata_free(&call);
			job.result.pictures = job.picture->renders.load();
			job.result.filters = job.filter ? job.filter->renders.load() : 0;
		},
		&job, true);
	return job.result;
}

inline void check(QStringList &issues)
{
	register_sources();
	auto require = [&](bool ok, const QString &message) {
		if (!ok)
			issues.append(QStringLiteral("Visible bounds reuse: ") + message);
	};
	OBSSourceAutoRelease source =
		obs_source_create_private("stagekit_test_visible_bounds_picture", "Visible bounds fixture", nullptr);
	OBSSceneAutoRelease scene = obs_scene_create_private("Visible bounds fixture scene");
	if (!source || !scene) {
		require(false, QStringLiteral("fixture source or scene unavailable"));
		return;
	}
	auto &picture = *static_cast<Picture *>(obs_obj_get_data(source));
	auto *item = obs_scene_add(scene, source);
	auto *scene_source = obs_scene_get_source(scene);
	obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
	const std::array<double, 4> body{13.0, 17.0, 37.0, 29.0};
	auto expect = [&](const char *name, const Measurement &got, const std::array<double, 4> &box, int pictures,
			  int filters = 0, bool any = true) {
		const QString label = QString::fromLatin1(name);
		require(got.called, label + QStringLiteral(": measurement procedure unavailable"));
		require(got.any == any && got.box == box, label + QStringLiteral(": occupied bounds changed"));
		require(got.pictures == pictures && got.filters == filters,
			label + QStringLiteral(": expected %1 picture and %2 filter renders, got %3 and %4")
					.arg(pictures)
					.arg(filters)
					.arg(got.pictures)
					.arg(got.filters));
		blog(LOG_INFO, "[visible-bounds] %s: picture=%d filter=%d any=%d box=%.0f,%.0f %.0fx%.0f", name,
		     got.pictures, got.filters, got.any, got.box[0], got.box[1], got.box[2], got.box[3]);
	};

	const auto original = replacement_test::render(source, kWidth, kHeight);
	expect("unfiltered", measure(scene_source, item, picture, nullptr), body, 1);
	expect("shadows included", measure(scene_source, item, picture, nullptr, true, false), body, 1);
	require(replacement_test::render(source, kWidth, kHeight) == original,
		QStringLiteral("unfiltered measurement changed rendered pixels"));
	expect("whole box", measure(scene_source, item, picture, nullptr, false), {0.0, 0.0, kWidth, kHeight}, 0);

	// A same-size filter that changes no pixels still takes the original
	// filtered and bare measurements. A disabled filter does as well.
	OBSSourceAutoRelease passthrough = obs_source_create_private("stagekit_test_visible_bounds_filter",
								     "Visible bounds passthrough fixture", nullptr);
	require(bool(passthrough), QStringLiteral("passthrough fixture unavailable"));
	if (passthrough) {
		auto &filter = *static_cast<Filter *>(obs_obj_get_data(passthrough));
		obs_source_filter_add(source, passthrough);
		expect("passthrough filter", measure(scene_source, item, picture, &filter), body, 2, 1);
		require(replacement_test::render(source, kWidth, kHeight) == original,
			QStringLiteral("passthrough filter changed the reference pixels"));
		obs_source_set_enabled(passthrough, false);
		expect("disabled filter", measure(scene_source, item, picture, &filter), body, 2);
		obs_source_filter_remove(source, passthrough);
	}

	OBSDataAutoRelease mask_settings = obs_data_create();
	obs_data_set_bool(mask_settings, "mask", true);
	OBSSourceAutoRelease mask = obs_source_create_private("stagekit_test_visible_bounds_filter",
							      "Visible bounds mask fixture", mask_settings);
	require(bool(mask), QStringLiteral("mask fixture unavailable"));
	if (mask) {
		auto &filter = *static_cast<Filter *>(obs_obj_get_data(mask));
		obs_source_filter_add(source, mask);
		const auto masked = replacement_test::render(source, kWidth, kHeight);
		expect("masked filter", measure(scene_source, item, picture, &filter), {20.0, 23.0, 11.0, 9.0}, 1, 1);
		require(replacement_test::render(source, kWidth, kHeight) == masked,
			QStringLiteral("filtered measurement changed rendered pixels"));
		obs_source_filter_remove(source, mask);
	}

	// The full and base dimensions agree after drawing, but differ from
	// those used for the first drawing. Its old result must not be reused.
	picture.resize_once = true;
	expect("size changed while drawing", measure(scene_source, item, picture, nullptr), body, 2);
	expect("new size settled", measure(scene_source, item, picture, nullptr), body, 1);
	picture.empty = true;
	expect("empty picture", measure(scene_source, item, picture, nullptr), {0.0, 0.0, kWidth + 1, kHeight}, 1, 0,
	       false);
	picture.empty = false;

	// Placement and crop are still applied to the same occupied source
	// pixels. This crops the body's first seven columns and three rows.
	obs_sceneitem_crop crop{};
	crop.left = crop.top = 20;
	obs_sceneitem_set_crop(item, &crop);
	vec2 position, scale;
	vec2_set(&position, 101.0f, 103.0f);
	vec2_set(&scale, 2.0f, 3.0f);
	obs_sceneitem_set_pos(item, &position);
	obs_sceneitem_set_scale(item, &scale);
	expect("cropped and scaled", measure(scene_source, item, picture, nullptr), {101.0, 103.0, 60.0, 78.0}, 1);

	OBSSourceAutoRelease async = obs_source_create_private("stagekit_test_visible_bounds_async",
							       "Visible bounds async fixture", nullptr);
	require(bool(async), QStringLiteral("async fixture unavailable"));
	if (async) {
		auto &async_picture = *static_cast<Picture *>(obs_obj_get_data(async));
		auto *async_item = obs_scene_add(scene, async);
		obs_sceneitem_set_alignment(async_item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
		std::vector<uint32_t> pixels(size_t(kWidth) * kHeight, 0xFF0080FF);
		obs_source_set_async_unbuffered(async, true);
		obs_source_inc_showing(async);
		obs_source_frame frame{};
		frame.data[0] = reinterpret_cast<uint8_t *>(pixels.data());
		frame.linesize[0] = kWidth * 4;
		frame.width = kWidth;
		frame.height = kHeight;
		frame.format = VIDEO_FORMAT_RGBA;
		frame.full_range = true;
		frame.timestamp = os_gettime_ns();
		obs_source_output_video(async, &frame);
		QElapsedTimer ready;
		ready.start();
		while ((!obs_source_get_base_width(async) || !obs_source_get_base_height(async)) &&
		       ready.elapsed() < 2500)
			QThread::msleep(10);
		require(obs_source_get_base_width(async) == kWidth && obs_source_get_base_height(async) == kHeight,
			QStringLiteral("async frame did not become ready"));
		expect("async source", measure(scene_source, async_item, async_picture, nullptr), body, 1);
		obs_source_dec_showing(async);
	}
}

} // namespace visible_bounds_reuse_test
