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
//
// One item can be in two changes at once. An item left over, with nothing
// of its own kind to change into, changes into (or out of) one that is
// already changing, by the same rules: linked, then the largest, where for
// the largest both must fill a good part of the screen (kJoinShare), not be
// a panel beside it. Hiding a full-screen camera to show a capture brings
// back the camera's corner view too; linked to the camera, the corner view
// takes the camera, and the capture, left over, still comes out of the
// camera instead of fading in by itself.
//
// Make Room: an item left by itself whose Morph says Make Room, and every
// item hidden or shown with the Make Room transition (which never pairs), is
// moved off an edge of the screen by a layer of its own, a job with one side
// (`solo`): the edge nearest to it of those that lie away from the other
// changes in its scene at that moment (`room_edge`), or simply the nearest.
//
// Who does what, on which thread:
//
// The tick callback (OBS's drawing thread, once a frame, with OBS's lock
// round all tick callbacks held) only looks and decides: which transitions
// have started, whose they are, which two go together. It never adds or
// removes a scene item, never makes a source, never lets go of what may be
// the last reference to one, and never asks OBS to work an item's place out
// (which OBS tells every listener of). All of that takes locks and runs
// other code's handlers (OBS's window, obs-websocket, other plugins) on the
// calling thread; from here OBS froze for good, the drawing thread and the
// window's thread each waiting for something the other held.
//
// OBS's own thread, the window's (`ui_work`, a task queued with
// obs_queue_task(OBS_TASK_UI) and never waited for), puts each change's
// layer into its scene, takes it out when the change is over, and lets go of
// everything a change or a tick held. Until its layer is there a change is
// only claimed: the two items' own transitions go on drawing what they drew
// while waiting (the hidden one whole, the shown one not yet), so nothing
// blinks however late the window's thread is. The tick after the layer is
// there (`take_up_layers`), between two frames, the layer starts drawing and
// the two transitions stop, in the same frame.

#include "item-morph.hpp"
#include "../common/item-transition.hpp"
#include "../common/loading.hpp"
#include "../common/scene-util.hpp"
#include "../common/timing.hpp"
#include "../common/ui-task.hpp"
#include "../look/look-paint.hpp"

#include <obs-module.h>
#include <graphics/vec4.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
// The least an item left over, and the item it would join, each cover of
// their scene for it to join a change already under way as the main
// picture: a picture filling most of the screen, not a panel beside it. At
// the main picture's own 15 % the owner's chat box (16 %), left over when the
// full-screen DSLR was shown in Chat Live, joined the camera's change
// (beta158): "the camera is morphing from the chat box".
constexpr double kJoinShare = 1.0 / 3.0;

struct RoomPath {
	Bounds bounds;
	std::atomic<float> t0{-1.0f};
	explicit RoomPath(const Bounds &box) : bounds(box) {}
};

// One change under way. It holds what it draws for as long as the change
// lasts (a second or so), never its scene: a scene held here kept its items,
// their sources and their transitions alive past a change of collection.
// Every change is ended, and so let go of, when its scene goes, when the
// collection changes (`end_all`) and when OBS closes.
//
// It is made by the tick, which takes hold of the two items, their sources
// and the transition whose clock it goes by. The layer is made, put into the
// scene, taken out and let go of on OBS's own thread only (`place_layer`,
// `take_out`), and so is everything it holds (`let_go`).
struct Job {
	obs_sceneitem_t *from = nullptr;         // held while the change lasts
	obs_sceneitem_t *to = nullptr;           // held while the change lasts
	obs_source_t *from_source = nullptr;     // held, and kept showing once the layer is in
	obs_source_t *to_source = nullptr;       // held, and kept showing once the layer is in
	obs_source_t *to_transition = nullptr;   // held: the change goes by its clock
	obs_source_t *from_transition = nullptr; // compared, never used
	obs_weak_source_t *scene = nullptr;      // weak: never kept alive from here
	// Its entries, each by its transition and the number of that start (an
	// entry made since for the same transition is another change's).
	obs_source_t *from_key = nullptr, *to_key = nullptr; // compared, never used
	uint64_t from_seq = 0, to_seq = 0;
	// OBS's own thread only:
	obs_source_t *layer = nullptr;         // held until the change ends
	obs_sceneitem_t *layer_item = nullptr; // held until the change ends
	bool showing = false;                  // the two sources are kept showing
	uint32_t cx = 0, cy = 0;
	EaseCurve curve = EaseCurve::Cubic;
	EaseMode mode = EaseMode::InOut;
	bool seen = true;       // by what shows of the two, not their boxes
	bool no_shadows = true; // and without their shadows, glows and outlines
	// Make Room: one item by itself (`from` hidden, or `to` shown), moved
	// off, or in from, `edge`.
	bool solo = false;
	RoomEdge edge = RoomEdge::Left;
	std::shared_ptr<RoomPath> room_path;
	obs_source_t *room_clock = nullptr; // held for linked items that share the primary's duration
	bool fade = false;
	std::atomic<float> t0{-1.0f}; // the clock's time when the change was first drawn
	// Said by OBS's own thread: the layer is in the scene, or could not be put there.
	std::atomic<bool> placed{false}, failed{false};
	// Said by the tick, the frame after: the layer draws the change, and the
	// items' own transitions draw nothing.
	std::atomic<bool> drawing{false};
	std::atomic<bool> done{false};
	uint64_t made_ns = 0;
	// Held while it is drawn from, and while it is let go of.
	std::recursive_mutex use;
	bool gone = false; // let go of (under `use`)

	// On OBS's own thread (the last reference to an item or a source unmakes
	// it there and then), after its layer is out of the scene. A drawing
	// under way is waited for; what was held is let go of after `use` is,
	// never with it held: letting go may wait for the graphics lock (a
	// source that stops, an item's own picture unmade), which a drawing that
	// waits here for `use` holds.
	void let_go()
	{
		obs_sceneitem_t *items[2] = {nullptr, nullptr};
		obs_source_t *sources[4] = {};
		obs_weak_source_t *weak = nullptr;
		bool shown = false;
		{
			std::lock_guard lock(use);
			if (gone)
				return;
			gone = true;
			std::swap(items[0], from);
			std::swap(items[1], to);
			std::swap(sources[0], from_source);
			std::swap(sources[1], to_source);
			std::swap(sources[2], to_transition);
			std::swap(sources[3], room_clock);
			std::swap(weak, scene);
			shown = showing;
			showing = false;
		}
		if (shown) {
			if (sources[0])
				obs_source_dec_showing(sources[0]);
			if (sources[1])
				obs_source_dec_showing(sources[1]);
		}
		for (obs_source_t *source : sources)
			obs_source_release(source);
		for (obs_sceneitem_t *item : items)
			obs_sceneitem_release(item);
		obs_weak_source_release(weak);
	}

	// (Let go of long before: `ui_work` does it before it drops the change.)
	~Job() { let_go(); }
};

// The layer: what is in the scene while a change lasts.
class Layer : public MorphPaint {
public:
	void set(const std::shared_ptr<Job> &job)
	{
		std::shared_ptr<Job> was; // (dropped after the lock is)
		std::lock_guard lock(mutex_);
		was.swap(job_);
		job_ = job;
		// (Its size is kept when the change is taken from it, a moment before
		// it is taken out of the scene: a size that changed would have OBS
		// work its place out afresh, and say so, in the middle of a drawing.)
		if (job) {
			cx_ = job->cx;
			cy_ = job->cy;
		}
		fresh_ = true;
	}
	std::shared_ptr<Job> job()
	{
		std::lock_guard lock(mutex_);
		return job_;
	}
	uint32_t cx() const { return cx_; }
	uint32_t cy() const { return cy_; }
	void render();

private:
	std::mutex mutex_;
	std::shared_ptr<Job> job_;
	std::atomic<uint32_t> cx_{0}, cy_{0};
	std::atomic<bool> fresh_{false}; // another change: what was seen of the last one's items is let go
};

void Layer::render()
{
	const std::shared_ptr<Job> job = this->job();
	// (Not before the tick has seen the layer in the scene: until then the
	// two items' own transitions draw.)
	if (!job || job->done || !job->drawing || !effect_)
		return;
	// (What it draws from is let go of on OBS's own thread, which waits here
	// for a drawing under way.)
	std::lock_guard use(job->use);
	if (job->gone || job->done)
		return;
	const float clock = obs_transition_get_time(job->room_clock ? job->room_clock : job->to_transition);
	if (clock >= 1.0f)
		return; // the item draws itself from here
	if (fresh_.exchange(false))
		forget_seen();
	auto &start = job->room_clock ? job->room_path->t0 : job->t0;
	float t0 = start.load();
	if (t0 < 0.0f) {
		t0 = std::min(clock, 0.95f);
		start = t0;
	}
	const double way = std::clamp((double)(clock - t0) / (double)(1.0f - t0), 0.0, 1.0);
	const double t = ease(job->curve, job->mode, way);
	const float amount = (float)std::clamp(t, 0.0, 1.0);

	const bool was_linear = gs_set_linear_srgb(true);
	spare_ = 0;
	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	// (Each item's place as it is: this layer is an item of their scene, which
	// has worked every item's place out before drawing any. Nothing is asked
	// of OBS in the middle of a scene's drawing.)
	Shown was, will;
	const bool had = job->from && look(job->from, was, false), has = job->to && look(job->to, will, false);
	if (job->solo) {
		// Make Room: the one item, on its way off (or in from) its edge.
		const Shown &only = had ? was : will;
		if (had || has) {
			const double gone = had ? t : 1.0 - t;
			const Quad quad = room_moved(only.quad,
						     job->room_path ? job->room_path->bounds : bounds_of(only.quad),
						     job->edge, gone, (double)job->cx, (double)job->cy);
			const float opacity = job->fade ? (float)std::clamp(1.0 - gone, 0.0, 1.0) : 1.0f;
			if (opacity >= 0.999f) {
				draw_item(only, quad, only.crop);
			} else if (gs_texture_t *texture = cropped(only, only.crop)) {
				draw_texture(texture, nullptr, quad, 0.0f, opacity);
			}
		}
	} else if (had && has) {
		// By what shows of each, looked at once as the change begins.
		const Crop from_whole = job->seen ? seen_crop(job->from, was) : was.crop;
		const Crop to_whole = job->seen ? seen_crop(job->to, will) : will.crop;
		Crop from_part, to_part;
		match_parts(was, job->no_shadows ? without_room(was, from_whole) : from_whole, from_whole, will,
			    job->no_shadows ? without_room(will, to_whole) : to_whole, to_whole, from_part, to_part);
		const Quad quad = between(part_quad(was, from_part), part_quad(will, to_part), t);
		gs_texture_t *first = cropped(was, from_part);
		gs_texture_t *second = cropped(will, to_part);
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

// A Morph transition that is an item's, since it last started. The item is
// known by its id in a weak scene, never held: an item's transition only
// says it has stopped when it is drawn to its end, which a scene out of view
// never is, and an entry that held its scene kept that scene (its items,
// their sources and this very transition) alive until it was given up on,
// 20 s later. OBS found them still there at a change of collection or at
// exit ("Double destroy" in the tests). Each tick finds the item afresh.
struct Entry {
	ItemMorph state = ItemMorph::Waiting;
	bool show = false;
	bool in_group = false;
	std::atomic<bool> over{false};
	obs_weak_source_t *scene = nullptr; // weak
	int64_t item_id = 0;
	RoomMoveLinks move_links;   // only the links from the press that started this transition
	const char *edge = nullptr; // the nearest edge, as the change began (a string of OBS's own)
	uint64_t at_ns = 0;
	// Which start of its transition this is: a change knows its entries by it.
	uint64_t seq = 0;
	// The changes it is in whose layer is not in the scene yet. Until then it
	// still reads Waiting (a hidden item whole, a shown one not drawn), is
	// not paired afresh and does not go by itself.
	int claims = 0;

	void let_go()
	{
		obs_weak_source_release(scene);
		scene = nullptr;
	}
};

// References the drawing thread took, kept for OBS's own thread to let go of:
// the last one to an item or a source unmakes it there and then (its hotkeys,
// its signals, its source), which must not happen in a tick callback.
struct Bin {
	std::vector<obs_source_t *> sources;
	std::vector<obs_sceneitem_t *> items;
	bool empty() const { return sources.empty() && items.empty(); }
	void take(Bin &from)
	{
		sources.insert(sources.end(), from.sources.begin(), from.sources.end());
		items.insert(items.end(), from.items.begin(), from.items.end());
		from.sources.clear();
		from.items.clear();
	}
	void let_go()
	{
		for (obs_sceneitem_t *item : items)
			obs_sceneitem_release(item);
		for (obs_source_t *source : sources)
			obs_source_release(source);
		items.clear();
		sources.clear();
	}
};

// Guards the tables. Entries are made and unmade on the drawing thread only
// (`tick`), so that thread may keep hold of one between locks; other threads
// only mark one as over. Never held while OBS is asked for anything that
// takes a lock of its own: OBS calls in here holding a scene's lock.
std::mutex g_mutex;
std::unordered_map<obs_source_t *, MorphSaid> g_said; // every Morph there is
std::unordered_map<obs_source_t *, Entry> g_entries;  // those that are an item's
using Starts = std::unordered_map<obs_source_t *, RoomMoveLinks>;
Starts g_started; // started, and not yet looked up
// Changed by the tick only (and by the ending of all changes), with
// `g_tick_mutex` and `g_mutex` both held; read by the tick without the second.
std::vector<std::shared_ptr<Job>> g_jobs;
// For OBS's own thread: changes whose layer is to be put into the scene,
// changes that are over, and references to let go of.
std::vector<std::shared_ptr<Job>> g_to_place, g_to_end;
Bin g_bin;
bool g_asked = false; // a task for that thread is on its way
uint64_t g_seq = 0;
std::atomic<int> g_in_hand{0}; // changes that thread is letting go of at this moment
std::atomic<bool> g_busy{false};
std::atomic<bool> g_stopped{false};
std::mutex g_tick_mutex;       // one tick, or the ending of all changes, at a time
TickCallback g_tick = nullptr; // the tick as OBS was given it (timed)
// For the tests: layers put into scenes and taken out, and how many of those
// were done on OBS's drawing thread, or on any thread but OBS's own. And, of
// those taken out, the ones left to a scene that was going (`take_out`).
std::atomic<long long> g_placed{0}, g_taken_out{0}, g_on_drawing{0}, g_off_own{0}, g_left_to_scene{0};

void note_where(std::atomic<long long> &what)
{
	what++;
	if (obs_in_task_thread(OBS_TASK_GRAPHICS))
		g_on_drawing++;
	if (!obs_in_task_thread(OBS_TASK_UI))
		g_off_own++;
}

// A change's entries, if they are still the ones it was made for. With
// `g_mutex` held.
template<typename Do> void each_entry(const Job &job, Do &&act)
{
	const std::pair<obs_source_t *, uint64_t> keys[] = {{job.from_key, job.from_seq}, {job.to_key, job.to_seq}};
	for (const auto &key : keys) {
		if (!key.first)
			continue;
		auto entry = g_entries.find(key.first);
		if (entry != g_entries.end() && entry->second.seq == key.second)
			act(entry->second);
	}
}

// On OBS's own thread: into the scene, straight above the upper of its
// items, and locked so that a click in the preview goes through it: the
// layer that draws the change. False when it could not be put there (the
// scene or one of the items gone meanwhile); the change is then over and its
// items go by themselves.
bool place_layer(const std::shared_ptr<Job> &job)
{
	obs_source_t *scene_source = obs_weak_source_get_source(job->scene);
	obs_scene_t *scene = scene_source ? obs_scene_from_source(scene_source) : nullptr;
	bool there = scene && !obs_source_removed(scene_source);
	for (obs_sceneitem_t *item : {job->from, job->to})
		there = there && (!item || obs_sceneitem_get_scene(item) == scene);
	obs_source_t *source = there ? obs_source_create_private(kMorphLayerId, obs_module_text("Morph.Layer"), nullptr)
				     : nullptr;
	auto *layer = source ? static_cast<Layer *>(obs_obj_get_data(source)) : nullptr;
	obs_sceneitem_t *item = nullptr;
	struct Order {
		obs_sceneitem_t *a, *b;
		int at = 0, upper = -1;
	} order{job->from, job->to};
	if (layer) {
		layer->set(job);
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *each, void *param) {
				auto *o = static_cast<Order *>(param);
				if (each == o->a || each == o->b)
					o->upper = o->at;
				o->at++;
				return true;
			},
			&order);
		item = obs_scene_add(scene, source);
	}
	if (item) {
		obs_sceneitem_addref(item);
		obs_sceneitem_set_locked(item, true);
		// (A new item is the top one: moved only when that is not its place.)
		if (order.upper >= 0 && order.upper + 1 < order.at)
			obs_sceneitem_set_order_position(item, order.upper + 1);
		// (Its place worked out here, so OBS need not do it, and say so to
		// every listener, in the middle of the next frame's drawing. It still
		// does when a frame is drawn between the adding and this, as for any
		// item OBS's own window adds.)
		refresh_item(item);
		// (A hidden item's capture would stop part-way through.)
		for (obs_source_t *shown : {job->from_source, job->to_source})
			if (shown)
				obs_source_inc_showing(shown);
		job->showing = true;
		job->layer = source;
		job->layer_item = item;
		note_where(g_placed);
	} else {
		if (layer)
			layer->set(nullptr);
		obs_source_release(source);
	}
	obs_source_release(scene_source);
	return item != nullptr;
}

// On OBS's own thread: a change's layer out of its scene, and let go of.
void take_out(const std::shared_ptr<Job> &job)
{
	// (The layer lets go of the change now, not when OBS gets round to
	// destroying it.)
	if (job->layer)
		if (auto *layer = static_cast<Layer *>(obs_obj_get_data(job->layer)))
			layer->set(nullptr);
	if (job->layer_item) {
		// Only with its scene held for the moment. A scene whose last reference
		// is gone takes its own items out, on OBS's thread for unmaking things,
		// and is unmade straight after; OBS's remove reads the item's scene
		// without a lock, and would lock, and take the item out of, a scene
		// that is being unmade at that very moment. Such a scene cannot be
		// held any more, and the layer is left to it.
		obs_source_t *scene = obs_weak_source_get_source(job->scene);
		if (scene)
			obs_sceneitem_remove(job->layer_item);
		else
			g_left_to_scene++;
		obs_source_release(scene);
		obs_sceneitem_release(job->layer_item);
		job->layer_item = nullptr;
		note_where(g_taken_out);
	}
	obs_source_release(job->layer);
	job->layer = nullptr;
}

// What waits for OBS's own thread, done: on that thread, from the task the
// tick asks for, or straight from the frontend's event when the collection
// changes or OBS closes (a task still on its way then finds nothing left).
// Where there is no such thread (no window), as the tick ends.
//
// It goes by what waits at the moment it runs, never by what waited when it
// was asked for: a change that ended before its layer was put in is passed
// over and gets none, and one that ends after is handed over again and its
// layer taken out then.
void ui_work()
{
	std::vector<std::shared_ptr<Job>> place, end;
	Bin bin;
	{
		std::lock_guard lock(g_mutex);
		place.swap(g_to_place);
		end.swap(g_to_end);
		bin.take(g_bin);
		g_in_hand += (int)end.size();
	}
	// What is over first: its layer out of the scene, then all it held.
	for (const std::shared_ptr<Job> &job : end) {
		take_out(job);
		job->let_go();
		g_in_hand--;
	}
	for (const std::shared_ptr<Job> &job : place) {
		if (job->done)
			continue;
		if (place_layer(job))
			job->placed = true;
		else
			job->failed = true;
	}
	bin.let_go();
}

void ui_task(void *)
{
	{
		std::lock_guard lock(g_mutex);
		g_asked = false;
	}
	// (Stopped: all of it was done there and then, and the tables are no more.)
	if (!g_stopped)
		ui_work();
}

// Changes whose layer OBS's own thread has put into the scene since the last
// frame: from this frame on the layer draws them and the items' own
// transitions draw nothing. Said here, between two frames, so that no frame
// has both or neither. One whose layer could not be put there is over. With
// `g_tick_mutex` held.
void take_up_layers()
{
	std::lock_guard lock(g_mutex);
	// Take a single snapshot before handing any layer its item. The UI thread can
	// finish placing another linked layer while this tick is walking the jobs.
	std::unordered_set<const RoomPath *> waiting;
	for (const auto &job : g_jobs) {
		if (job->room_path && !job->placed && !job->failed && !job->done)
			waiting.insert(job->room_path.get());
	}
	for (const std::shared_ptr<Job> &job : g_jobs) {
		if (job->drawing || job->done)
			continue;
		if (job->failed) {
			job->done = true; // (ended, and its items let go by themselves, as this tick ends)
			continue;
		}
		if (!job->placed || waiting.count(job->room_path.get()))
			continue;
		each_entry(*job, [](Entry &entry) {
			if (entry.claims > 0)
				entry.claims--;
			entry.state = ItemMorph::Paired;
		});
		job->drawing = true;
	}
}

// A change that is over: nothing more is drawn of it, and it is handed to
// OBS's own thread, which takes its layer out of the scene and lets go of
// what it held. With `g_tick_mutex` and `g_mutex` held.
void end_job(const std::shared_ptr<Job> &job)
{
	job->done = true;
	// (Its layer never drew, because the change was over first or the layer
	// could not be put in: its items are no longer kept waiting for it. One
	// that is still on its way goes by itself from here, inside its own
	// frame; it is not paired afresh.)
	if (!job->drawing) {
		each_entry(*job, [](Entry &entry) {
			if (entry.claims > 0)
				entry.claims--;
			if (entry.claims == 0 && entry.state == ItemMorph::Waiting)
				entry.state = ItemMorph::Alone;
		});
	}
	g_to_end.push_back(job);
}

// What the tick took hold of and what waits for OBS's own thread is handed
// to it: a task is asked for, unless one is on its way, and never waited
// for. With `g_tick_mutex` held.
void hand_over(Bin &held)
{
	{
		std::lock_guard lock(g_mutex);
		g_bin.take(held);
		if (g_asked || (g_to_place.empty() && g_to_end.empty() && g_bin.empty()))
			return;
		g_asked = true;
	}
	if (on_ui_thread_later(ui_task, nullptr))
		return;
	// No window, so no such thread, and nobody to wait for one another with
	// (the tests' programs that set no handler for these tasks): here and now.
	{
		std::lock_guard lock(g_mutex);
		g_asked = false;
	}
	ui_work();
	take_up_layers();
}

// Whose transitions these are: every scene and group is gone through once.
struct Found {
	obs_source_t *transition;
	obs_source_t *scene;   // held (by the tick)
	obs_sceneitem_t *item; // held (by the tick)
	bool show;
	bool in_group;
	const char *edge = nullptr;
};

// The scenes are listed first, each held (into `held`), and gone through
// after: OBS's list of sources is locked while it is read, and a scene's own
// locks are never waited for with that lock held. OBS's own thread takes a
// layer out with its scene locked and every listener called meanwhile; one
// that asks OBS for a source there would wait for this thread, and this
// thread for the scene.
void find_items(const Starts &wanted, std::vector<Found> &out, Bin &held)
{
	std::vector<obs_source_t *> scenes;
	obs_enum_scenes(
		[](void *param, obs_source_t *scene) {
			if (obs_source_t *kept = obs_source_get_ref(scene))
				static_cast<std::vector<obs_source_t *> *>(param)->push_back(kept);
			return true;
		},
		&scenes);
	struct Look {
		const Starts *wanted;
		std::vector<Found> *out;
		obs_source_t *scene;
	} look{&wanted, &out, nullptr};
	for (obs_source_t *scene_source : scenes) {
		held.sources.push_back(scene_source);
		obs_scene_t *scene = scene_of(scene_source);
		if (!scene)
			continue;
		look.scene = scene_source;
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *item, void *p) {
				auto *l = static_cast<Look *>(p);
				for (const bool show : {true, false}) {
					obs_source_t *has = obs_sceneitem_get_transition(item, show);
					if (!l->wanted->count(has))
						continue;
					obs_sceneitem_addref(item);
					l->out->push_back(Found{has, obs_source_get_ref(l->scene), item, show,
								obs_source_is_group(l->scene)});
				}
				return true;
			},
			&look);
	}
}

// How much of its scene an item covers, by its place as OBS last worked it
// out: its scene is on screen, so that was as the last frame was drawn.
// (Nothing is asked of OBS here: see `refresh_item`.)
double share_of(obs_sceneitem_t *item, obs_source_t *scene)
{
	const double screen = (double)obs_source_get_width(scene) * (double)obs_source_get_height(scene);
	Shown shown;
	if (screen < 1.0 || !shown_of(item, shown))
		return 0.0;
	const Bounds b = bounds_of(shown.quad);
	return (b.x1 - b.x0) * (b.y1 - b.y0) / screen;
}

// One half of a pair that may be. `taken`: in a pair already, from this tick
// or one just before; only one left over may still change into it.
// The item and its scene are held for this one tick (in its `Bin`).
struct Half {
	obs_source_t *transition;
	Entry *entry;
	MorphSaid said;
	bool taken = false;
	obs_source_t *scene = nullptr;   // held by the tick
	obs_sceneitem_t *item = nullptr; // held by the tick
};

// An entry's item and scene, found afresh and held (into `held`); false
// when either is gone.
bool reach(const Entry &entry, Bin &held, obs_source_t *&scene, obs_sceneitem_t *&item)
{
	scene = obs_weak_source_get_source(entry.scene);
	if (!scene)
		return false;
	held.sources.push_back(scene);
	item = obs_scene_find_sceneitem_by_id(scene_of(scene), entry.item_id);
	if (!item)
		return false;
	obs_sceneitem_addref(item);
	held.items.push_back(item);
	return true;
}

// A change of two items begins: it takes hold of what it draws and claims
// its two entries, and its layer is asked for (`hand_over`, as the tick
// ends). `why` is for the log: which rule paired the two.
void start_job(Half &hide, Half &show, const char *why)
{
	// (The transition the change goes by must still be the item's.)
	obs_source_t *clock = obs_sceneitem_get_transition(show.item, true);
	if (clock != show.transition || !obs_scene_from_source(show.scene))
		return;
	auto job = std::make_shared<Job>();
	job->to_transition = obs_source_get_ref(clock);
	job->from_transition = hide.transition;
	job->from_key = hide.transition;
	job->to_key = show.transition;
	job->from = hide.item;
	job->to = show.item;
	obs_sceneitem_addref(job->from);
	obs_sceneitem_addref(job->to);
	job->from_source = obs_source_get_ref(obs_sceneitem_get_source(job->from));
	job->to_source = obs_source_get_ref(obs_sceneitem_get_source(job->to));
	job->scene = obs_source_get_weak_source(show.scene);
	job->cx = obs_source_get_width(show.scene);
	job->cy = obs_source_get_height(show.scene);
	job->curve = show.said.curve;
	job->seen = show.said.seen;
	job->no_shadows = show.said.no_shadows;
	job->mode = show.said.mode;
	job->made_ns = os_gettime_ns();
	{
		std::lock_guard lock(g_mutex);
		job->from_seq = hide.entry->seq;
		job->to_seq = show.entry->seq;
		hide.entry->claims++;
		show.entry->claims++;
		g_jobs.push_back(job);
		g_to_place.push_back(job);
	}
	hide.taken = show.taken = true;
	// Once per change, so that a pairing nobody wanted can be told from the log.
	obs_log(LOG_INFO, "Morph: '%s' changes into '%s' in '%s' (%s)", obs_source_get_name(job->from_source),
		obs_source_get_name(job->to_source), obs_source_get_name(show.scene), why);
}

// Every change ended and every entry let go of, with `g_tick_mutex` held: at
// a change of collection (OBS then looks for sources still held), while one
// loads, and when OBS closes. The layers are taken out of their scenes, and
// what the changes held is let go of, by `ui_work`: called straight after
// this on OBS's own thread, or asked for by the tick.
void end_all()
{
	std::lock_guard lock(g_mutex);
	for (const std::shared_ptr<Job> &job : g_jobs)
		end_job(job);
	g_jobs.clear();
	for (auto &pair : g_entries)
		pair.second.let_go();
	g_entries.clear();
	g_started.clear();
	g_busy = false;
}

// Make Room: the middle of what else changes in the item's scene as it goes
// (or comes). For an item going, where the changes under way are going to
// and the items coming in (with Morph, Make Room or any other transition of
// their own); for one coming, where they come from and the items going.
// Each counts by its area; a background (nine tenths of the screen) not at
// all. `opposite` are the items of the other kind that started near it.
// Each item's place is taken as OBS last worked it out (its scene is on
// screen), and the scene's items are held (into `held`) and looked at after
// the scene has been gone through, not while its locks are held.
RoomCentre room_centre(const Half &self, const std::vector<Half> &opposite, Bin &held)
{
	RoomCentre centre;
	obs_source_t *scene = self.scene;
	const bool leaving = !self.entry->show;
	const double screen = (double)obs_source_get_width(scene) * (double)obs_source_get_height(scene);
	std::vector<obs_sceneitem_t *> counted{self.item};
	auto count = [&](obs_sceneitem_t *item) {
		if (!item || std::find(counted.begin(), counted.end(), item) != counted.end())
			return;
		counted.push_back(item);
		Shown shown;
		if (!shown_of(item, shown))
			return;
		const Bounds b = bounds_of(shown.quad);
		if ((b.x1 - b.x0) * (b.y1 - b.y0) < 0.9 * screen)
			centre.add(b);
	};
	for (const std::shared_ptr<Job> &job : g_jobs)
		if (!job->done && obs_weak_source_references_source(job->scene, scene))
			count(leaving ? job->to : job->from);
	for (const Half &h : opposite)
		if (h.scene == scene && h.entry->state != ItemMorph::Paired && h.entry->claims == 0)
			count(h.item);
	std::vector<obs_sceneitem_t *> items;
	obs_scene_enum_items(
		obs_scene_from_source(scene),
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			if (!is_morph_layer(item)) {
				obs_sceneitem_addref(item);
				static_cast<std::vector<obs_sceneitem_t *> *>(param)->push_back(item);
			}
			return true;
		},
		&items);
	for (obs_sceneitem_t *item : items) {
		held.items.push_back(item);
		if (item_transition_running(item, leaving))
			count(item);
	}
	return centre;
}

struct RoomMove {
	Half *half;
	RoomEdge edge;
	bool pushed;
	std::optional<Bounds> bounds;
	RoomMove *primary = nullptr;
	std::shared_ptr<RoomPath> together = nullptr;
};

// Resolve only the items actually moving on the same press, in the same scene and direction.
// Choose the primary's edge before adding followers to the bounds used for travel.
void link_room_moves(std::vector<RoomMove> &moves)
{
	for (RoomMove &move : moves) {
		const Half &follower = *move.half;
		const RoomMoveLinks &links = follower.entry->move_links;
		if (!links || !move.bounds)
			continue;
		const char *name = obs_source_get_name(obs_sceneitem_get_source(follower.item));
		for (const auto &[leader, linked] : *links) {
			if (linked != name)
				continue;
			double best = 0.0;
			for (RoomMove &candidate : moves) {
				const Half &primary = *candidate.half;
				if (!candidate.bounds || primary.scene != follower.scene ||
				    primary.entry->show != follower.entry->show || primary.entry->move_links != links ||
				    leader != obs_source_get_name(obs_sceneitem_get_source(primary.item)))
					continue;
				// If the scene uses the primary more than once, follow the nearest instance.
				const Bounds &a = *move.bounds, &b = *candidate.bounds;
				const double distance =
					std::hypot(a.x0 + a.x1 - b.x0 - b.x1, a.y0 + a.y1 - b.y0 - b.y1);
				if (!move.primary || distance < best ||
				    (distance == best && primary.entry->item_id < move.primary->half->entry->item_id)) {
					move.primary = &candidate;
					best = distance;
				}
			}
			if (move.primary)
				break;
		}
		if (move.primary) {
			auto &path = move.primary->together;
			if (!path)
				path = std::make_shared<RoomPath>(*move.primary->bounds);
			Bounds &a = path->bounds;
			const Bounds &b = *move.bounds;
			a = Bounds{std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1),
				   std::max(a.y1, b.y1)};
		}
	}
}

// Make Room: one item moved off (or in from) its chosen edge by a layer.
// False when it cannot be (it then goes by itself, inside its own frame).
bool start_solo(const RoomMove &move)
{
	Half &h = *move.half;
	const RoomMove &path = move.primary ? *move.primary : move;
	obs_source_t *clock = obs_sceneitem_get_transition(h.item, h.entry->show);
	if (clock != h.transition || !obs_scene_from_source(h.scene))
		return false;
	auto job = std::make_shared<Job>();
	job->solo = true;
	job->edge = path.edge;
	job->room_path = path.together;
	if (job->room_path) {
		auto duration = [](const Half &half) {
			const uint32_t ms = obs_sceneitem_get_transition_duration(half.item, half.entry->show);
			return ms ? ms : 300u;
		};
		if (duration(h) == duration(*path.half))
			job->room_clock = obs_source_get_ref(path.half->transition);
	}
	job->fade = h.said.room_fade;
	job->to_transition = obs_source_get_ref(clock);
	job->from_transition = clock;
	job->to_key = h.transition;
	obs_sceneitem_t *&item = h.entry->show ? job->to : job->from;
	obs_source_t *&source = h.entry->show ? job->to_source : job->from_source;
	item = h.item;
	obs_sceneitem_addref(item);
	source = obs_source_get_ref(obs_sceneitem_get_source(item));
	job->scene = obs_source_get_weak_source(h.scene);
	job->cx = obs_source_get_width(h.scene);
	job->cy = obs_source_get_height(h.scene);
	const MorphSaid &timing = job->room_clock ? path.half->said : h.said;
	job->curve = timing.curve;
	job->mode = timing.mode;
	job->made_ns = os_gettime_ns();
	{
		std::lock_guard lock(g_mutex);
		job->to_seq = h.entry->seq;
		h.entry->claims++; // (its own transition draws nothing once the layer is in)
		g_jobs.push_back(job);
		g_to_place.push_back(job);
	}
	h.taken = true;
	static const char *const kEdges[] = {"left", "right", "top", "bottom"};
	obs_log(LOG_INFO, "Morph: '%s' %s '%s', by the %s edge (%s)", obs_source_get_name(source),
		h.entry->show ? "comes into" : "makes room in", obs_source_get_name(h.scene), kEdges[(int)path.edge],
		move.primary ? "with its linked primary"
			     : (path.pushed ? "away from the change under way" : "the nearest edge"));
	return true;
}

void tick(void *, float)
{
	if (!g_busy || g_stopped)
		return;
	std::lock_guard one(g_tick_mutex);
	if (g_stopped)
		return;
	// What this tick takes hold of, let go of on OBS's own thread (`hand_over`).
	Bin held;
	if (!scenes_loaded()) {
		end_all();
		hand_over(held);
		return;
	}
	const uint64_t now = os_gettime_ns();
	take_up_layers();
	Starts started;
	{
		std::lock_guard lock(g_mutex);
		started.swap(g_started);
	}
	// Whose are the ones that have started?
	std::vector<Found> found;
	if (!started.empty())
		find_items(started, found, held);
	// (Worked out here, not by the transition as it draws: it would have to
	// find the item, and holding it is what this file no longer does. By the
	// item's box as it is: nothing is asked of OBS in a tick callback.)
	for (Found &f : found)
		f.edge = item_off_nearest_edge(f.item, f.scene, f.show, true);
	MorphLinks links;
	std::vector<Half> hides, shows;
	{
		std::lock_guard lock(g_mutex);
		for (const auto &start : started) {
			auto old = g_entries.find(start.first);
			if (old != g_entries.end()) {
				old->second.let_go();
				g_entries.erase(old);
			}
		}
		for (Found &f : found) {
			if (g_said.count(f.transition) && !g_entries.count(f.transition)) {
				Entry &entry = g_entries[f.transition];
				entry.show = f.show;
				entry.in_group = f.in_group;
				entry.scene = obs_source_get_weak_source(f.scene);
				entry.item_id = obs_sceneitem_get_id(f.item);
				entry.edge = f.edge;
				entry.move_links = started.at(f.transition);
				entry.at_ns = now;
				entry.seq = ++g_seq;
			}
		}
		for (const auto &said : g_said)
			links.insert(links.end(), said.second.links.begin(), said.second.links.end());
		for (auto &pair : g_entries) {
			Entry &entry = pair.second;
			if (entry.over)
				continue;
			// Still waiting for its other half; or paired a moment ago (its
			// layer in the scene or not yet), so that one left over may
			// change into it too (see the top).
			const bool waiting = entry.state == ItemMorph::Waiting && entry.claims == 0;
			const bool recent = (entry.state == ItemMorph::Paired || entry.claims > 0) &&
					    now >= entry.at_ns && now - entry.at_ns < kGraceNs;
			if (!waiting && !recent)
				continue;
			auto said = g_said.find(pair.first);
			(entry.show ? shows : hides)
				.push_back(Half{pair.first, &entry, said != g_said.end() ? said->second : MorphSaid(),
						!waiting});
		}
	}
	for (Found &f : found) {
		held.sources.push_back(f.scene);
		held.items.push_back(f.item);
	}
	// Each one's item, found afresh and held for this tick only. (Outside
	// the lock: OBS may call in here holding a scene's lock.) An item that
	// is gone ends its entry.
	for (std::vector<Half> *halves : {&hides, &shows}) {
		for (auto h = halves->begin(); h != halves->end();) {
			if (reach(*h->entry, held, h->scene, h->item)) {
				++h;
			} else {
				h->entry->over = true;
				h = halves->erase(h);
			}
		}
	}
	// A change that was under way for something that has started again is over.
	for (const std::shared_ptr<Job> &job : g_jobs)
		if (started.count(job->to_transition) || started.count(job->from_transition) ||
		    started.count(job->room_clock))
			job->done = true;

	// Pairs: a hide and a show in one scene that is on screen. Linked sources
	// first, then the largest of each.
	auto there = [](const Half &h) {
		return !h.entry->in_group && obs_source_showing(h.scene);
	};
	auto can = [&](const Half &h) {
		return !h.taken && h.said.pairs && there(h);
	};
	auto name = [](const Half &h) {
		return obs_source_get_name(obs_sceneitem_get_source(h.item));
	};
	auto apart = [](const Half &a, const Half &b) {
		return a.scene == b.scene && a.item != b.item;
	};
	// The largest of `of` in `scene` covering at least `least` of it; with
	// `taken_too`, ones already in a pair count as well.
	auto largest = [&](std::vector<Half> &of, obs_source_t *scene, bool taken_too, double least = kMainShare) {
		Half *best = nullptr;
		double best_share = least;
		for (Half &h : of) {
			if (!(taken_too ? there(h) && h.said.pairs : can(h)) || h.scene != scene)
				continue;
			const double share = share_of(h.item, h.scene);
			if (share >= best_share) {
				best = &h;
				best_share = share;
			}
		}
		return best;
	};
	for (Half &show : shows) {
		for (Half &hide : hides) {
			if (!can(show) || !can(hide) || !apart(hide, show) ||
			    !morph_linked(links, name(hide), name(show)))
				continue;
			start_job(hide, show, "linked");
		}
	}
	for (Half &show : shows) {
		if (!can(show) || !show.said.pair_main)
			continue;
		// (The largest shown in this scene, and it is this one?)
		Half *main_show = largest(shows, show.scene, false), *main_hide = largest(hides, show.scene, false);
		if (main_show == &show && main_hide && apart(*main_hide, show))
			start_job(*main_hide, show, "main image");
	}
	// One left over changes into (or out of) one already in a pair: linked
	// first, then the main image. (See the top: the capture that comes out
	// of a camera which is going into its corner view.) The one in a pair
	// must still be hidden, or shown, as it was then.
	auto shared = [&](const Half &left, const Half &other) {
		return can(left) && other.taken && there(other) && apart(left, other) &&
		       obs_sceneitem_visible(other.item) == other.entry->show;
	};
	auto join = [](Half &left, Half &other, const char *why) {
		if (left.entry->show)
			start_job(other, left, why);
		else
			start_job(left, other, why);
	};
	for (const bool showing : {true, false}) {
		std::vector<Half> &lefts = showing ? shows : hides, &others = showing ? hides : shows;
		for (Half &left : lefts)
			for (Half &other : others)
				if (shared(left, other) && morph_linked(links, name(left), name(other)))
					join(left, other, "linked, joining a change under way");
	}
	for (const bool showing : {true, false}) {
		std::vector<Half> &lefts = showing ? shows : hides, &others = showing ? hides : shows;
		// (Only the largest left over in each scene, as it is before any of
		// them joins: one item goes into two changes, not into every one. Both
		// must fill a good part of the screen, kJoinShare: a panel beside the
		// main picture fades by itself.)
		std::vector<Half *> mains;
		for (Half &left : lefts)
			if (can(left) && largest(lefts, left.scene, false, kJoinShare) == &left)
				mains.push_back(&left);
		for (Half *left : mains) {
			Half *other = largest(others, left->scene, true, kJoinShare);
			const Half *show = showing ? left : other;
			if (other && show->said.pair_main && shared(*left, *other))
				join(*left, *other, "main image, joining a change under way");
		}
	}
	// Whatever has waited long enough, or cannot be paired, goes by itself:
	// from now, inside its own frame; or, to make room where a layer can move
	// it across the screen, once that layer is in its scene (it waits as it
	// is until then).
	std::vector<Half *> rooms;
	{
		std::lock_guard lock(g_mutex);
		for (std::vector<Half> *halves : {&hides, &shows}) {
			for (Half &h : *halves) {
				if (h.taken || h.entry->state != ItemMorph::Waiting || h.entry->claims > 0)
					continue;
				if (h.entry->in_group || !obs_source_showing(h.scene) ||
				    (now > h.entry->at_ns && now - h.entry->at_ns >= kGraceNs)) {
					if (h.said.room && there(h))
						rooms.push_back(&h);
					else
						h.entry->state = ItemMorph::Alone;
				}
			}
		}
	}
	// Make Room: each edge is chosen before any of them moves, so the order
	// they are gone through in changes nothing. (Not in a scene off screen,
	// nor in a group: there it goes off its own frame's edge, as a slide.)
	std::vector<RoomMove> moves;
	for (Half *h : rooms) {
		const RoomCentre centre = h->said.room_push ? room_centre(*h, h->entry->show ? hides : shows, held)
							    : RoomCentre();
		Shown shown;
		const double cw = obs_source_get_width(h->scene), ch = obs_source_get_height(h->scene);
		std::optional<Bounds> bounds;
		if (shown_of(h->item, shown))
			bounds = bounds_of(shown.quad);
		const RoomEdge edge = bounds ? room_edge(*bounds, cw, ch, centre.any() ? &centre : nullptr)
					     : RoomEdge::Left;
		moves.push_back({h, edge, centre.any(), bounds});
	}
	link_room_moves(moves);
	for (const RoomMove &move : moves) {
		if (!start_solo(move)) {
			std::lock_guard lock(g_mutex);
			move.half->entry->state = ItemMorph::Alone;
		}
	}
	// Changes that are over: the clock has run out, the item was hidden
	// again or taken out, or something went wrong. (Whether its layer is in
	// the scene yet makes no difference to any of these.)
	for (size_t i = 0; i < g_jobs.size();) {
		const std::shared_ptr<Job> &job = g_jobs[i];
		bool over = job->done || obs_transition_get_time(job->to_transition) >= 1.0f ||
			    (now > job->made_ns && now - job->made_ns > kTooLongNs);
		if (job->solo) {
			// (Shown again while going, hidden again while coming, or taken out.)
			obs_sceneitem_t *item = job->to ? job->to : job->from;
			over = over || obs_sceneitem_visible(item) != (job->to != nullptr) ||
			       !obs_sceneitem_get_scene(item);
		} else {
			over = over || !obs_sceneitem_visible(job->to) || !obs_sceneitem_get_scene(job->to) ||
			       !obs_sceneitem_get_scene(job->from);
		}
		// (Its scene deleted. Asked of the weak reference: taking hold of the
		// scene to ask would mean letting go of it again, here.)
		over = over || obs_weak_source_expired(job->scene);
		std::lock_guard lock(g_mutex);
		if (!over) {
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
	hand_over(held);
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
	g_started[transition].reset(); // a later independent start never inherits a previous press
	g_busy = true;
}

void item_morph_note_move_links(obs_sceneitem_t *item, bool show, const RoomMoveLinks &links)
{
	if (!links)
		return;
	// Show in Scenes applies its whole press on the video thread. OBS announces the
	// transition start synchronously, before set_item_visible returns.
	obs_source_t *transition = obs_sceneitem_get_transition(item, show);
	std::lock_guard lock(g_mutex);
	auto start = g_started.find(transition);
	if (start != g_started.end())
		start->second = links;
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
	g_started.erase(transition);
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

const char *item_morph_edge(obs_source_t *transition)
{
	std::lock_guard lock(g_mutex);
	auto entry = g_entries.find(transition);
	if (entry == g_entries.end() || entry->second.over)
		return nullptr;
	return entry->second.edge;
}

void item_morph_let_go()
{
	if (g_stopped)
		return;
	{
		// (A tick under way is waited for: it asks nothing of this thread.)
		std::lock_guard one(g_tick_mutex);
		end_all();
	}
	// Here and now: this is OBS's own thread, and OBS looks for sources still
	// held as soon as the frontend's event returns.
	ui_work();
}

void item_morph_stop()
{
	if (g_stopped.exchange(true))
		return;
	// (First: OBS calls `tick` holding the lock this waits for.)
	if (g_tick)
		obs_remove_tick_callback(g_tick, nullptr);
	{
		std::lock_guard one(g_tick_mutex);
		end_all();
	}
	ui_work();
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
		return static_cast<Layer *>(data)->cx();
	};
	info.get_height = [](void *data) -> uint32_t {
		return static_cast<Layer *>(data)->cy();
	};
	info.video_render = [](void *data, gs_effect_t *) {
		static_cast<Layer *>(data)->render();
	};
	register_timed(&info);
	// (Timed like a kind's own tick: its cost is in the log's timing lines.)
	g_tick = timed_tick_callback("stagekit_item_morph", tick);
	obs_add_tick_callback(g_tick, nullptr);
	ui_task_add_test_calls();
	// For the tests: what is held at this moment (a change counts until OBS's
	// own thread has let go of all it held).
	proc_handler_add(
		obs_get_proc_handler(),
		"void stagekit_test_item_morph_held(out int entries, out int jobs, out int said)",
		[](void *, calldata_t *cd) {
			std::lock_guard lock(g_mutex);
			calldata_set_int(cd, "entries", (long long)g_entries.size());
			calldata_set_int(cd, "jobs",
					 (long long)(g_jobs.size() + g_to_end.size()) + (long long)g_in_hand.load());
			calldata_set_int(cd, "said", (long long)g_said.size());
		},
		nullptr);
	// And where its layers were put into scenes and taken out: how many of
	// each so far, how many of those on OBS's drawing thread (never, where
	// there is a thread of OBS's own) and on any thread but OBS's own, how
	// many changes wait for that thread now, and how many of the layers taken
	// out were left to a scene that was going.
	proc_handler_add(
		obs_get_proc_handler(),
		"void stagekit_test_item_morph_layers(out int placed, out int removed, out int on_drawing, out int off_own, out int waiting, out int left)",
		[](void *, calldata_t *cd) {
			std::lock_guard lock(g_mutex);
			calldata_set_int(cd, "placed", g_placed.load());
			calldata_set_int(cd, "removed", g_taken_out.load());
			calldata_set_int(cd, "on_drawing", g_on_drawing.load());
			calldata_set_int(cd, "off_own", g_off_own.load());
			calldata_set_int(cd, "waiting", (long long)(g_to_place.size() + g_to_end.size()));
			calldata_set_int(cd, "left", g_left_to_scene.load());
		},
		nullptr);
	// And what the frontend's event does as a collection changes, for a test
	// program, which has no frontend: called on its own main thread.
	proc_handler_add(
		obs_get_proc_handler(), "void stagekit_test_item_morph_let_go()",
		[](void *, calldata_t *) { item_morph_let_go(); }, nullptr);
}

} // namespace stagekit
