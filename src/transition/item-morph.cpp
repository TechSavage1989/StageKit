/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Morph between two items of one scene.
//
// OBS draws an item's show and hide transition inside the item's own frame,
// often by way of a picture the size of that frame, so nothing drawn there
// can travel across the screen. The change is drawn by a layer instead: a
// source of the scene's size, put into the scene straight above the two
// items for as long as the change lasts and taken out again after. It draws
// the two sources the way the Morph transition draws a pair, at a place
// part-way between the two items.
//
// The two items stay what OBS and everything else take them for: the one
// hidden, the other shown, from the first moment. Their own transitions (the
// Morph transition, as an item's) draw nothing meanwhile.
//
// Which two go together is found here, whoever hid and showed them: a
// filter, a Stream Deck key, the eye in OBS's list. Every Morph transition
// says when it starts; the ones that are an item's are looked up in the
// scenes, and a hide and a show in one scene within `kGrace` of each other
// are a pair when the two sources are linked (in any Morph's settings) or
// are the largest of each.

#include "item-morph.hpp"
#include "../common/scene-util.hpp"
#include "../common/timing.hpp"

#include <obs-module.h>
#include <graphics/vec4.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace stagekit {

namespace {

// How long an item hidden or shown with Morph waits for its other half
// before it fades by itself. Two keys pressed as one, or two steps of a
// multi-action, come a few hundredths of a second apart.
constexpr uint64_t kGraceNs = 100'000'000ULL;
// A change never lasts this long; one that has is ended.
constexpr uint64_t kTooLongNs = 20'000'000'000ULL;
// The least an item covers of its scene to be taken for the main picture.
constexpr double kMainShare = 0.15;

// One change under way.
struct Job {
	obs_sceneitem_t *from = nullptr;         // held
	obs_sceneitem_t *to = nullptr;           // held
	obs_source_t *from_source = nullptr;     // held, and kept showing
	obs_source_t *to_source = nullptr;       // held, and kept showing
	obs_source_t *to_transition = nullptr;   // held: the change goes by its clock
	obs_source_t *from_transition = nullptr; // compared, never used
	obs_source_t *scene = nullptr;           // held
	obs_source_t *layer = nullptr;           // held until the change ends
	obs_sceneitem_t *layer_item = nullptr;   // held until the change ends
	uint32_t cx = 0, cy = 0;
	EaseCurve curve = EaseCurve::Cubic;
	EaseMode mode = EaseMode::InOut;
	std::atomic<float> t0{-1.0f}; // the clock's time when the change was first drawn
	std::atomic<bool> done{false};
	uint64_t made_ns = 0;

	~Job()
	{
		obs_source_dec_showing(from_source);
		obs_source_dec_showing(to_source);
		obs_source_release(from_source);
		obs_source_release(to_source);
		obs_source_release(to_transition);
		obs_sceneitem_release(from);
		obs_sceneitem_release(to);
		obs_source_release(scene);
	}
};

// The layer: what is in the scene while a change lasts.
class Layer : public MorphPaint {
public:
	void set(const std::shared_ptr<Job> &job)
	{
		std::lock_guard lock(mutex_);
		job_ = job;
	}
	std::shared_ptr<Job> job()
	{
		std::lock_guard lock(mutex_);
		return job_;
	}
	void render();

private:
	std::mutex mutex_;
	std::shared_ptr<Job> job_;
};

void Layer::render()
{
	const std::shared_ptr<Job> job = this->job();
	if (!job || job->done || !effect_)
		return;
	const float clock = obs_transition_get_time(job->to_transition);
	if (clock >= 1.0f)
		return; // the item draws itself from here
	float t0 = job->t0;
	if (t0 < 0.0f) {
		t0 = std::min(clock, 0.95f);
		job->t0 = t0;
	}
	const double way = std::clamp((double)(clock - t0) / (double)(1.0f - t0), 0.0, 1.0);
	const double t = ease(job->curve, job->mode, way);
	const float amount = (float)std::clamp(t, 0.0, 1.0);

	const bool was_linear = gs_set_linear_srgb(true);
	spare_ = 0;
	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	Shown was, will;
	const bool had = look(job->from, was), has = look(job->to, will);
	if (had && has) {
		const Quad quad = between(was.quad, will.quad, t);
		gs_texture_t *first = cropped(was, was.crop);
		gs_texture_t *second = cropped(will, will.crop);
		if (first && second)
			draw_texture(first, second, quad, amount);
	} else if (had || has) {
		// One of the two has no picture: the other, fading where it is.
		const Shown &only = had ? was : will;
		if (gs_texture_t *texture = cropped(only, only.crop)) {
			gs_effect_set_float(p_opacity_, had ? 1.0f - amount : amount);
			const bool was_srgb = gs_framebuffer_srgb_enabled();
			gs_enable_framebuffer_srgb(true);
			gs_effect_set_texture_srgb(p_image_, texture);
			gs_matrix_push();
			set_quad(only.quad);
			while (gs_effect_loop(effect_, "Draw"))
				gs_draw_sprite(nullptr, 0, 1, 1);
			gs_matrix_pop();
			gs_enable_framebuffer_srgb(was_srgb);
		}
	}
	gs_blend_state_pop();
	gs_set_linear_srgb(was_linear);
}

// A Morph transition that is an item's, since it last started.
struct Entry {
	ItemMorph state = ItemMorph::Waiting;
	bool show = false;
	bool in_group = false;
	std::atomic<bool> over{false};
	obs_source_t *scene = nullptr;   // held
	obs_sceneitem_t *item = nullptr; // held
	uint64_t at_ns = 0;

	void let_go()
	{
		obs_source_release(scene);
		obs_sceneitem_release(item);
		scene = nullptr;
		item = nullptr;
	}
};

// Guards the three tables. Entries are made and unmade on the drawing
// thread only (`tick`), so that thread may keep hold of one between locks;
// other threads only mark one as over.
std::mutex g_mutex;
std::unordered_map<obs_source_t *, MorphSaid> g_said; // every Morph there is
std::unordered_map<obs_source_t *, Entry> g_entries;  // those that are an item's
std::vector<obs_source_t *> g_started;                // started, and not yet looked up
std::vector<std::shared_ptr<Job>> g_jobs;             // drawing thread only
std::atomic<bool> g_busy{false};
std::atomic<bool> g_stopped{false};
std::mutex g_tick_mutex; // one tick, or the stop, at a time

void end_job(const std::shared_ptr<Job> &job)
{
	job->done = true;
	if (job->layer_item) {
		obs_sceneitem_remove(job->layer_item);
		obs_sceneitem_release(job->layer_item);
		job->layer_item = nullptr;
	}
	if (job->layer) {
		obs_source_release(job->layer);
		job->layer = nullptr;
	}
}

// Whose transitions these are: every scene and group is gone through once.
struct Found {
	obs_source_t *transition;
	obs_source_t *scene;   // held
	obs_sceneitem_t *item; // held
	bool show;
	bool in_group;
};

void find_items(const std::vector<obs_source_t *> &wanted, std::vector<Found> &out)
{
	struct Look {
		const std::vector<obs_source_t *> *wanted;
		std::vector<Found> *out;
		obs_source_t *scene;
	} look{&wanted, &out, nullptr};
	obs_enum_scenes(
		[](void *param, obs_source_t *scene_source) {
			auto *l = static_cast<Look *>(param);
			obs_scene_t *scene = scene_of(scene_source);
			if (!scene)
				return true;
			l->scene = scene_source;
			obs_scene_enum_items(
				scene,
				[](obs_scene_t *, obs_sceneitem_t *item, void *p) {
					auto *l = static_cast<Look *>(p);
					for (const bool show : {true, false}) {
						obs_source_t *has = obs_sceneitem_get_transition(item, show);
						if (!has || std::find(l->wanted->begin(), l->wanted->end(), has) ==
								    l->wanted->end())
							continue;
						obs_sceneitem_addref(item);
						l->out->push_back(Found{has, obs_source_get_ref(l->scene), item, show,
									obs_source_is_group(l->scene)});
					}
					return true;
				},
				l);
			return true;
		},
		&look);
}

// How much of its scene an item covers.
double share_of(obs_sceneitem_t *item, obs_source_t *scene)
{
	const double screen = (double)obs_source_get_width(scene) * (double)obs_source_get_height(scene);
	refresh_item(item);
	Shown shown;
	if (screen < 1.0 || !shown_of(item, shown))
		return 0.0;
	const Bounds b = bounds_of(shown.quad);
	return (b.x1 - b.x0) * (b.y1 - b.y0) / screen;
}

// One half of a pair that may be.
struct Half {
	obs_source_t *transition;
	Entry *entry;
	MorphSaid said;
	bool taken = false;
};

void start_job(Half &hide, Half &show)
{
	// (The transition the change goes by must still be the item's.)
	obs_source_t *clock = obs_sceneitem_get_transition(show.entry->item, true);
	obs_scene_t *scene = obs_scene_from_source(show.entry->scene);
	if (clock != show.transition || !scene)
		return;
	auto job = std::make_shared<Job>();
	job->to_transition = obs_source_get_ref(clock);
	job->from_transition = hide.transition;
	job->from = hide.entry->item;
	job->to = show.entry->item;
	obs_sceneitem_addref(job->from);
	obs_sceneitem_addref(job->to);
	job->from_source = obs_source_get_ref(obs_sceneitem_get_source(job->from));
	job->to_source = obs_source_get_ref(obs_sceneitem_get_source(job->to));
	// (A hidden item's capture would stop part-way through.)
	obs_source_inc_showing(job->from_source);
	obs_source_inc_showing(job->to_source);
	job->scene = obs_source_get_ref(show.entry->scene);
	job->cx = obs_source_get_width(job->scene);
	job->cy = obs_source_get_height(job->scene);
	job->curve = show.said.curve;
	job->mode = show.said.mode;
	job->made_ns = os_gettime_ns();

	job->layer = obs_source_create_private(kMorphLayerId, obs_module_text("Morph.Layer"), nullptr);
	auto *layer = job->layer ? static_cast<Layer *>(obs_obj_get_data(job->layer)) : nullptr;
	if (!layer)
		return; // (the job lets go of everything as it goes)
	layer->set(job);
	// Into the scene, straight above the upper of the two, and locked so
	// that a click in the preview goes through it.
	struct Order {
		obs_sceneitem_t *a, *b;
		int at = 0, upper = -1;
	} order{job->from, job->to};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *o = static_cast<Order *>(param);
			if (item == o->a || item == o->b)
				o->upper = o->at;
			o->at++;
			return true;
		},
		&order);
	obs_sceneitem_t *item = obs_scene_add(scene, job->layer);
	if (!item) {
		layer->set(nullptr);
		obs_source_release(job->layer);
		job->layer = nullptr;
		return;
	}
	obs_sceneitem_addref(item);
	job->layer_item = item;
	obs_sceneitem_set_locked(item, true);
	if (order.upper >= 0)
		obs_sceneitem_set_order_position(item, order.upper + 1);
	{
		std::lock_guard lock(g_mutex);
		hide.entry->state = ItemMorph::Paired;
		show.entry->state = ItemMorph::Paired;
	}
	hide.taken = show.taken = true;
	g_jobs.push_back(job);
}

void tick(void *, float)
{
	if (!g_busy || g_stopped)
		return;
	std::lock_guard one(g_tick_mutex);
	if (g_stopped)
		return;
	const uint64_t now = os_gettime_ns();
	std::vector<obs_source_t *> started;
	{
		std::lock_guard lock(g_mutex);
		started.swap(g_started);
	}
	// Whose are the ones that have started?
	std::vector<Found> found;
	if (!started.empty())
		find_items(started, found);
	MorphLinks links;
	std::vector<Half> hides, shows;
	{
		std::lock_guard lock(g_mutex);
		for (obs_source_t *transition : started) {
			auto old = g_entries.find(transition);
			if (old != g_entries.end()) {
				old->second.let_go();
				g_entries.erase(old);
			}
		}
		for (Found &f : found) {
			if (!g_said.count(f.transition) || g_entries.count(f.transition)) {
				obs_source_release(f.scene);
				obs_sceneitem_release(f.item);
				continue;
			}
			Entry &entry = g_entries[f.transition];
			entry.show = f.show;
			entry.in_group = f.in_group;
			entry.scene = f.scene;
			entry.item = f.item;
			entry.at_ns = now;
		}
		for (const auto &said : g_said)
			links.insert(links.end(), said.second.links.begin(), said.second.links.end());
		for (auto &pair : g_entries) {
			Entry &entry = pair.second;
			if (entry.state != ItemMorph::Waiting || entry.over)
				continue;
			auto said = g_said.find(pair.first);
			(entry.show ? shows : hides)
				.push_back(Half{pair.first, &entry, said != g_said.end() ? said->second : MorphSaid()});
		}
	}
	// A change that was under way for something that has started again is over.
	for (const std::shared_ptr<Job> &job : g_jobs)
		if (std::find(started.begin(), started.end(), job->to_transition) != started.end() ||
		    std::find(started.begin(), started.end(), job->from_transition) != started.end())
			job->done = true;

	// Pairs: a hide and a show in one scene that is on screen. Linked sources
	// first, then the largest of each.
	auto can = [](const Half &h) {
		return !h.taken && !h.entry->in_group && obs_source_showing(h.entry->scene);
	};
	auto name = [](const Half &h) {
		return obs_source_get_name(obs_sceneitem_get_source(h.entry->item));
	};
	for (Half &show : shows) {
		for (Half &hide : hides) {
			if (!can(show) || !can(hide) || hide.entry->scene != show.entry->scene ||
			    hide.entry->item == show.entry->item || !morph_linked(links, name(hide), name(show)))
				continue;
			start_job(hide, show);
		}
	}
	for (Half &show : shows) {
		if (!can(show) || !show.said.pair_main)
			continue;
		// (The largest shown in this scene, and it is this one?)
		auto largest = [&](std::vector<Half> &of) {
			Half *best = nullptr;
			double best_share = kMainShare;
			for (Half &h : of) {
				if (!can(h) || h.entry->scene != show.entry->scene)
					continue;
				const double share = share_of(h.entry->item, h.entry->scene);
				if (share >= best_share) {
					best = &h;
					best_share = share;
				}
			}
			return best;
		};
		Half *main_show = largest(shows), *main_hide = largest(hides);
		if (main_show == &show && main_hide && main_hide->entry->item != show.entry->item)
			start_job(*main_hide, show);
	}
	// Whatever has waited long enough, or cannot be paired, goes by itself.
	{
		std::lock_guard lock(g_mutex);
		for (std::vector<Half> *halves : {&hides, &shows}) {
			for (Half &h : *halves) {
				if (h.taken || h.entry->state != ItemMorph::Waiting)
					continue;
				if (h.entry->in_group || !obs_source_showing(h.entry->scene) ||
				    (now > h.entry->at_ns && now - h.entry->at_ns >= kGraceNs))
					h.entry->state = ItemMorph::Alone;
			}
		}
	}
	// Changes that are over: the clock has run out, the item was hidden
	// again or taken out, or something went wrong.
	for (size_t i = 0; i < g_jobs.size();) {
		const std::shared_ptr<Job> &job = g_jobs[i];
		bool over = job->done || obs_transition_get_time(job->to_transition) >= 1.0f ||
			    !obs_sceneitem_visible(job->to) || !obs_sceneitem_get_scene(job->to) ||
			    !obs_sceneitem_get_scene(job->from) ||
			    (now > job->made_ns && now - job->made_ns > kTooLongNs);
		if (!over) {
			std::lock_guard lock(g_mutex);
			auto entry = g_entries.find(job->to_transition);
			over = entry == g_entries.end() || entry->second.over;
		}
		if (over) {
			end_job(job);
			g_jobs.erase(g_jobs.begin() + (long)i);
		} else {
			i++;
		}
	}
	// What is over is let go of.
	{
		std::lock_guard lock(g_mutex);
		for (auto it = g_entries.begin(); it != g_entries.end();) {
			if (it->second.over || (now > it->second.at_ns && now - it->second.at_ns > kTooLongNs)) {
				it->second.let_go();
				it = g_entries.erase(it);
			} else {
				++it;
			}
		}
		g_busy = !g_entries.empty() || !g_started.empty() || !g_jobs.empty();
	}
}

} // namespace

void item_morph_said(obs_source_t *transition, const MorphSaid &said)
{
	std::lock_guard lock(g_mutex);
	g_said[transition] = said;
}

void item_morph_started(obs_source_t *transition)
{
	if (g_stopped)
		return;
	std::lock_guard lock(g_mutex);
	auto entry = g_entries.find(transition);
	if (entry != g_entries.end())
		entry->second.over = true;
	if (std::find(g_started.begin(), g_started.end(), transition) == g_started.end())
		g_started.push_back(transition);
	g_busy = true;
}

void item_morph_stopped(obs_source_t *transition)
{
	std::lock_guard lock(g_mutex);
	auto entry = g_entries.find(transition);
	if (entry != g_entries.end())
		entry->second.over = true;
}

void item_morph_gone(obs_source_t *transition)
{
	std::lock_guard lock(g_mutex);
	g_said.erase(transition);
	g_started.erase(std::remove(g_started.begin(), g_started.end(), transition), g_started.end());
	auto entry = g_entries.find(transition);
	if (entry != g_entries.end())
		entry->second.over = true;
}

ItemMorph item_morph_now(obs_source_t *transition)
{
	std::lock_guard lock(g_mutex);
	auto entry = g_entries.find(transition);
	if (entry == g_entries.end() || entry->second.over)
		return ItemMorph::None;
	return entry->second.state;
}

void item_morph_stop()
{
	if (g_stopped.exchange(true))
		return;
	// (First: OBS calls `tick` holding the lock this waits for.)
	obs_remove_tick_callback(tick, nullptr);
	std::lock_guard one(g_tick_mutex);
	for (const std::shared_ptr<Job> &job : g_jobs)
		end_job(job);
	g_jobs.clear();
	std::lock_guard lock(g_mutex);
	for (auto &pair : g_entries)
		pair.second.let_go();
	g_entries.clear();
	g_started.clear();
}

void register_item_morph()
{
	obs_source_info info = {};
	info.id = kMorphLayerId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB | OBS_SOURCE_CAP_DISABLED;
	info.get_name = [](void *) {
		return obs_module_text("Morph.Layer");
	};
	info.create = [](obs_data_t *, obs_source_t *) -> void * {
		return new Layer();
	};
	info.destroy = [](void *data) {
		delete static_cast<Layer *>(data);
	};
	info.get_width = [](void *data) -> uint32_t {
		const std::shared_ptr<Job> job = static_cast<Layer *>(data)->job();
		return job ? job->cx : 0;
	};
	info.get_height = [](void *data) -> uint32_t {
		const std::shared_ptr<Job> job = static_cast<Layer *>(data)->job();
		return job ? job->cy : 0;
	};
	info.video_render = [](void *data, gs_effect_t *) {
		static_cast<Layer *>(data)->render();
	};
	register_timed(&info);
	obs_add_tick_callback(tick, nullptr);
}

} // namespace stagekit
