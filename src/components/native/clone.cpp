#include "frame.hpp"

#include <obs.hpp>
#include <media-io/audio-io.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

namespace stagekit::native {
namespace {
constexpr auto clone_id = "stagekit_port_source-clone";
constexpr auto audio_id = "stagekit_port_source_clone_audio_wrapper_source";

bool would_recurse(obs_source_t *owner, obs_source_t *candidate)
{
	if (owner == candidate) return true;
	struct Search {obs_source_t *owner; bool found; } search{owner,false};
	obs_source_enum_full_tree(candidate, [](obs_source_t *, obs_source_t *child, void *p) {
		auto &s = *static_cast<Search *>(p);
		s.found |= s.owner == child;
	}, &search);
	return search.found;
}
struct Link {
	OBSWeakSource target;
	OBSSource tap;
	bool graph=false, captured=false;
};

struct Clone {
	obs_source_t *self;
	std::mutex mutex;
	Link link;
	std::string name, canvas;
	int mode=0, buffer=0;
	std::atomic_bool audio{true};
	bool active=false, bare=false, showing=false, visible_ref=false, reselect=true;
	OBSWeakSource last_scene, previous;
	Frame frame;
	uint64_t drawn=0;
	Size stored{};
	std::atomic<bool> redraw{true};
	Clone(obs_data_t *s,obs_source_t *source):self(source){update(s);}
	~Clone(){disconnect();}
	Link links()
	{
		std::lock_guard guard(mutex);
		return link;
	}
	void update(obs_data_t *s)
	{
		const std::string next_name=obs_data_get_string(s,"clone"), next_canvas=obs_data_get_string(s,"canvas");
		const int next_mode=int(obs_data_get_int(s,"clone_type"));
		const bool next_audio=obs_data_get_bool(s,"audio"),next_active=obs_data_get_bool(s,"active_clone");
		if (next_name!=name || next_canvas!=canvas || next_mode!=mode || next_audio!=audio || next_active!=active) reselect=true;
		if(next_canvas!=canvas) {last_scene=nullptr;previous=nullptr;}
		name=next_name; canvas=next_canvas; mode=next_mode; audio=next_audio; active=next_active;
		buffer=int(std::clamp<long long>(obs_data_get_int(s,"buffer_frame"),0,4));
		bare=obs_data_get_bool(s,"no_filters");
		redraw.store(true);
	}
	static void samples(void *argument,obs_source_t *,const audio_data *packet,bool)
	{
		auto *tap=static_cast<obs_source_t *>(argument);
		obs_audio_info info{};
		if (!packet || !packet->frames || !obs_get_audio_info(&info)) return;
		obs_source_audio audio{};
		audio.frames=packet->frames;
		audio.timestamp=packet->timestamp;
		audio.samples_per_sec=info.samples_per_sec;
		audio.speakers=info.speakers;
		audio.format=AUDIO_FORMAT_FLOAT_PLANAR;
		for(size_t ch=0;ch<get_audio_channels(info.speakers);++ch)
			audio.data[ch]=packet->data[ch];
		obs_source_output_audio(tap,&audio);
	}
	void disconnect()
	{
		auto old=links();
		{
			std::lock_guard guard(mutex);
			link={};
		}
		auto source=OBSGetStrongRef(old.target);
		if(source) {
			if(old.captured && old.tap)obs_source_remove_audio_capture_callback(source,samples,old.tap.Get());
			if(visible_ref)obs_source_dec_showing(source);
			if(old.graph)obs_source_remove_active_child(self,source);
		}
		if(old.tap)obs_source_remove_active_child(self,old.tap);
		visible_ref=false;
		redraw.store(true);
	}
	void connect(obs_source_t *source)
	{
		disconnect();
		if(!source || obs_source_removed(source) || would_recurse(self,source)) return;
		Link next;
		next.target=OBSGetWeakRef(source);
		const auto flags=obs_source_get_output_flags(source);
		const bool mixed=(flags&OBS_SOURCE_COMPOSITE) || std::strcmp(obs_source_get_unversioned_id(source),clone_id)==0;
		next.graph=active || (audio && mixed);
		if(next.graph && !obs_source_add_active_child(self,source)) return;
		if(audio && !mixed && (flags&OBS_SOURCE_AUDIO)) {
			OBSSourceAutoRelease raw=obs_source_create_private(audio_id,"StageKit clone audio",nullptr);
			if(raw && obs_source_add_active_child(self,raw)) {
				next.tap=raw.Get();
				obs_source_set_audio_mixers(raw,0x3f);
				obs_source_add_audio_capture_callback(source,samples,next.tap.Get());
				next.captured=true;
			}
		}
		if(!next.graph && showing) {obs_source_inc_showing(source);visible_ref=true;}
		{
			std::lock_guard guard(mutex);
			link=std::move(next);
		}
	}
	void shown(bool now)
	{
		showing=now;
		auto state=links();
		auto source=OBSGetStrongRef(state.target);
		if(!source || state.graph || visible_ref==now) return;
		if(now)obs_source_inc_showing(source); else obs_source_dec_showing(source);
		visible_ref=now;
	}
	void tick()
	{
		auto state=links();
		auto old=OBSGetStrongRef(state.target);
		OBSSource chosen;
		if(mode==0 && !reselect && old && !obs_source_removed(old)) return;
		OBSCanvasAutoRelease selected_canvas=canvas.empty()?nullptr:obs_get_canvas_by_name(canvas.c_str());
		if(!canvas.empty() && !selected_canvas) {if(old)disconnect();return;}
		if(mode==0) {
			OBSSourceAutoRelease named = selected_canvas?obs_canvas_get_source_by_name(selected_canvas,name.c_str()):obs_get_source_by_name(name.c_str());
			chosen=named.Get();
		} else {
			OBSSourceAutoRelease output=selected_canvas?obs_canvas_get_channel(selected_canvas,0):obs_get_output_source(0);
			if(output && obs_source_get_type(output)==OBS_SOURCE_TYPE_TRANSITION)
				output=obs_transition_get_active_source(output);
			if (output && !obs_weak_source_references_source(last_scene,output)) {
				previous=last_scene;
				last_scene=OBSGetWeakRef(output);
			}
			chosen=mode==2?OBSGetStrongRef(previous):OBSSource(output.Get());
		}
		if((old.Get()!=chosen.Get() || reselect || (state.target && !old)) && chosen.Get()!=self) connect(chosen);
		else if (chosen.Get()==self && old) disconnect();
		reselect=false;
	}
	void children(obs_source_enum_proc_t cb,void *arg,bool all)
	{
		auto state=links();
		auto source=OBSGetStrongRef(state.target);
		if(source && (all || state.graph)) cb(self,source,arg);
		if(state.tap)cb(self,state.tap,arg);
	}
	static bool bypass(obs_source_t *s,bool requested)
	{
		return requested && !(obs_source_get_output_flags(s)&(OBS_SOURCE_ASYNC|OBS_SOURCE_CUSTOM_DRAW));
	}
	static Size dimensions(obs_source_t *s,bool bare,int divisor)
	{
		if (!s) return {1,1};
		auto w=bare?obs_source_get_base_width(s):obs_source_get_width(s);
		auto h=bare?obs_source_get_base_height(s):obs_source_get_height(s);
		return {std::max(1u,w/uint32_t(std::max(1,divisor))),std::max(1u,h/uint32_t(std::max(1,divisor)))};
	}
	Size size()
	{
		auto s=OBSGetStrongRef(links().target);
		return dimensions(s,s && bypass(s,bare),buffer);
	}
	void render()
	{
		auto s=OBSGetStrongRef(links().target);
		if(!s || obs_source_removed(s))return;
		const bool raw=bypass(s,bare);
		auto draw=[&] {if(raw)obs_source_default_render(s);else obs_source_video_render(s);};
		if(!buffer){draw();return;}
		const auto size=dimensions(s,raw,buffer), full=dimensions(s,raw,1);
		const auto now=obs_get_video_frame_time();
		if(redraw.exchange(false) || drawn!=now || size.width!=stored.width || size.height!=stored.height) {
			if(!frame.begin(size,gs_get_color_space()))return;
			gs_ortho(0,float(full.width),0,float(full.height),-100,100);
			draw();
			frame.end();
			stored=size;
			drawn=now;
		}
		draw_texture(frame.texture(),size);
	}
	bool mix(uint64_t *timestamp,obs_source_audio_mix *out,uint32_t mixers,size_t channels)
	{
		auto state=links();
		auto s=state.tap ? state.tap : OBSGetStrongRef(state.target);
		if(!s || (!state.tap && !(obs_source_get_output_flags(s)&OBS_SOURCE_COMPOSITE) && std::strcmp(obs_source_get_unversioned_id(s),clone_id)!=0) || !audio || obs_source_audio_pending(s))return false;
		const auto time=obs_source_get_audio_timestamp(s);
		if(!time)return false;
		obs_source_audio_mix input{};
		obs_source_get_audio_mix(s,&input);
		for(size_t bus=0;bus<MAX_AUDIO_MIXES;++bus) {
			if(!(mixers&(1u<<bus)))continue;
			for(size_t ch=0;ch<channels;++ch)
				if(input.output[bus].data[ch] && out->output[bus].data[ch])
					std::copy_n(input.output[bus].data[ch],AUDIO_OUTPUT_FRAMES,out->output[bus].data[ch]);
		}
		*timestamp=time;
		return true;
	}
};
obs_properties_t *properties(void *data)
{
	auto *filter=static_cast<Clone *>(data);
	auto *p=obs_properties_create();
	auto *mode=obs_properties_add_list(p,"clone_type",text("Native.CloneMode"),OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode,text("Native.Source"),0);
	obs_property_list_add_int(mode,text("Native.CurrentScene"),1);
	obs_property_list_add_int(mode,text("Native.PreviousScene"),2);
	auto *canvas=obs_properties_add_list(p,"canvas",text("Native.Canvas"),OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(canvas,text("Native.MainCanvas"),"");
	obs_enum_canvases([](void *p,obs_canvas_t *c) {
		auto name=obs_canvas_get_name(c);
		if(name && *name)obs_property_list_add_string(static_cast<obs_property_t *>(p),name,name);
		return true;
	},canvas);
	auto *source=obs_properties_add_list(p,"clone",text("Native.Source"),OBS_COMBO_TYPE_EDITABLE,OBS_COMBO_FORMAT_STRING);
	struct Picker {obs_property_t *prop;obs_source_t *owner;std::string peers;Clone *clone;} picker{source,filter?filter->self:nullptr,{},filter};
	auto add=[](void *p,obs_source_t *source) {
		auto &v=*static_cast<Picker *>(p);
		if(source==v.owner || (v.owner && would_recurse(v.owner,source)))return true;
		const char *name=obs_source_get_name(source);
		obs_property_list_add_string(v.prop,name,name);
		if(v.clone && std::strcmp(obs_source_get_unversioned_id(source),clone_id)==0) {
			OBSDataAutoRelease s=obs_source_get_settings(source);
			int mode=int(obs_data_get_int(s,"clone_type"));
			if(mode==v.clone->mode && (mode!=0 || v.clone->name==obs_data_get_string(s,"clone"))) {
				if(!v.peers.empty())v.peers+=", ";
				v.peers+=name;
			}
		}
		return true;
	};
	obs_enum_sources(add,&picker);
	obs_enum_canvases([](void *p,obs_canvas_t *c) {
		obs_canvas_enum_scenes(c,[](void *p,obs_source_t *scene) {
			auto &picker=*static_cast<Picker *>(p);
			if(scene!=picker.owner && (!picker.owner || !would_recurse(picker.owner,scene))) {
				const char *name=obs_source_get_name(scene);
				obs_property_list_add_string(picker.prop,name,name);
			}
			return true;
		},p);
		return true;
	},&picker);
	obs_properties_add_bool(p,"audio",text("Native.CloneAudio"));
	auto *res=obs_properties_add_list(p,"buffer_frame",text("Native.VideoBuffer"),OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
	int value=0;
	for (const auto *label : {"Native.None","Native.Full","Native.Half","Native.Third","Native.Quarter"})
		obs_property_list_add_int(res,text(label),value++);
	obs_properties_add_bool(p,"active_clone",text("Native.ActiveClone"));
	obs_properties_add_bool(p,"no_filters",text("Native.BypassFilters"));
	if(!picker.peers.empty()) obs_properties_add_text(p,"same_clones",picker.peers.c_str(),OBS_TEXT_INFO);
	auto visibility=[](obs_properties_t *props,obs_property_t *,obs_data_t *s){
		obs_property_set_visible(obs_properties_get(props,"clone"),obs_data_get_int(s,"clone_type")==0);
		return true;
	};
	obs_property_set_modified_callback(mode,visibility);
	return p;
}
void load()
{
	obs_source_info tap{};
	tap.id=audio_id;tap.type=OBS_SOURCE_TYPE_SCENE;
	tap.output_flags=OBS_SOURCE_AUDIO|OBS_SOURCE_CAP_DISABLED;
	tap.get_name=[](void *){return text("Ports.CloneAudio");};
	tap.create=[](obs_data_t *,obs_source_t *s)->void *{return s;};
	tap.destroy=[](void *){};
	obs_register_source(&tap);
	obs_source_info info{};
	info.id=clone_id;info.type=OBS_SOURCE_TYPE_INPUT;
	info.output_flags=OBS_SOURCE_VIDEO|OBS_SOURCE_AUDIO|OBS_SOURCE_CUSTOM_DRAW|OBS_SOURCE_SRGB;
	info.get_name=[](void *){return text("Ports.Clone");};
	info.create=[](obs_data_t *s,obs_source_t *self)->void *{return new Clone(s,self);};
	info.destroy=[](void *p){delete static_cast<Clone *>(p);};
	info.update=[](void *p,obs_data_t *s){static_cast<Clone *>(p)->update(s);};
	info.load=info.update;
	info.save=[](void *p,obs_data_t *s){
		auto &clone=*static_cast<Clone *>(p);
		auto target=OBSGetStrongRef(clone.links().target);
		if(clone.mode==0 && target)obs_data_set_string(s,"clone",obs_source_get_name(target));
	};
	info.video_tick=[](void *p,float){static_cast<Clone *>(p)->tick();};
	info.video_render=[](void *p,gs_effect_t *){static_cast<Clone *>(p)->render();};
	info.get_width=[](void *p){return static_cast<Clone *>(p)->size().width;};
	info.get_height=[](void *p){return static_cast<Clone *>(p)->size().height;};
	info.enum_active_sources=[](void *p,obs_source_enum_proc_t cb,void *arg){static_cast<Clone *>(p)->children(cb,arg,false);};
	info.enum_all_sources=[](void *p,obs_source_enum_proc_t cb,void *arg){static_cast<Clone *>(p)->children(cb,arg,true);};
	info.audio_render=[](void *p,uint64_t *ts,obs_source_audio_mix *out,uint32_t mixers,size_t ch,size_t){return static_cast<Clone *>(p)->mix(ts,out,mixers,ch);};
	info.show=[](void *p){static_cast<Clone *>(p)->shown(true);};
	info.hide=[](void *p){static_cast<Clone *>(p)->shown(false);};
	info.video_get_color_space=[](void *p,size_t count,const gs_color_space *preferred){
		auto target=OBSGetStrongRef(static_cast<Clone *>(p)->links().target);
		return target?obs_source_get_color_space(target,count,preferred):GS_CS_SRGB;
	};
	info.get_properties=properties;
	info.get_defaults=[](obs_data_t *s){obs_data_set_default_bool(s,"audio",true);};
	obs_register_source(&info);
}
} // namespace
const Component component{load};
} // namespace stagekit::native
