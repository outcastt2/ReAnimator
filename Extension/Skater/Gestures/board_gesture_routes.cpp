#include "board_gesture_layout.h"
#include "board_gesture_routes.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Log/logging.h"
#include <Windows.h>
#include <array>
#include <vector>
#include <string_view>
#include <mutex>
#include <cstring>
#include <stdexcept>
#include <cmath>
#include <chrono>
#include <algorithm>

namespace dingosdk::board_gesture::routes {
namespace {
using Ptr=std::uintptr_t;
constexpr Ptr owners=layout::named_asset_owners;
constexpr std::array<const char*,3> phases{"Into","Loop","Exit"};
constexpr const char* classification_name="Animation/Dingo/EBool.Intent.FullBodyGesture";
struct Patch { Ptr asset{},original{},replacement{},slot{64}; bool word{}; Ptr anchor{}; };
struct State {
 std::mutex mutex; std::vector<Patch> patches{};
 unsigned generations{}; ULONGLONG next{}; bool mounted{true};
 std::string detail="Waiting for verified on-board gesture routes";
};
State& state() {static State s;return s;}
template<class T> T get(Ptr p) { T v{};if(!memory::peek(p,v))throw std::runtime_error("Unreadable gesture asset");return v; }
template<class T> void put(Ptr p,T v) {std::memcpy(reinterpret_cast<void*>(p),&v,sizeof(v));}
Ptr resolved(Ptr v) {const auto p=v&~Ptr{6};return v&4 ? get<Ptr>(p):p;}
std::uint64_t hash(std::string_view name) {
 std::uint64_t h=5381;for(auto c:name) {if(c>='A'&&c<='Z')c+=32;h=(h*33)^static_cast<unsigned char>(c);}return h;
}
bool named(Ptr p,std::string_view expected) {
 std::array<char,180> name{};
 if(memory::peek_cstring(get<Ptr>(p+24),name.data(),name.size())<0)return false;
 if(expected.size()!=std::strlen(name.data()))return false;
 for(std::size_t i=0;i<expected.size();++i) {
  auto a=expected[i],b=name[i];if(a>='A'&&a<='Z')a+=32;if(b>='A'&&b<='Z')b+=32;if(a!=b)return false;
 }return true;
}
Ptr lookup_manager(Ptr manager,std::string_view name,unsigned depth=0) {
 if(!manager || depth>4)return 0;
 const auto buckets=get<Ptr>(manager+0xf8);const auto count=get<std::uint32_t>(manager+0x100);
 // The loaded multiplayer core domain has 99,733 buckets. This is a direct
 // hashed lookup (one bucket, <=256 nodes), not a scan over every bucket.
 if(!buckets || !count || count>1000000)return 0;
 const auto key=hash(name);auto node=get<Ptr>(buckets+(key%count)*8);
 for(unsigned visit=0;node && visit<256;++visit) {
  if(get<std::uint64_t>(node)==key) {
   const auto control=get<Ptr>(node+8);const auto asset=control ? resolved(get<Ptr>(control)):0;
   if(asset && named(asset,name))return asset;
   break;
  }node=get<Ptr>(node+16);
 }
 const auto begin=get<Ptr>(manager+0x58),end=get<Ptr>(manager+0x60);
 if(begin && begin<=end && end-begin<=4096*8)for(auto at=begin;at<end;at+=8)
  if(const auto p=lookup_manager(get<Ptr>(at),name,depth+1))return p;
 return 0;
}
Ptr lookup(Ptr base,std::string_view name) {
 // The predicate is in Dingo domain 5; sequence databases are in core domain 6.
 const unsigned domain=name==classification_name ? 5:6;
 const auto owner=get<Ptr>(base+owners+domain*8);
 return owner ? lookup_manager(get<Ptr>(owner+0x48),name):0;
}
void type(Ptr p,Ptr base,Ptr table,Ptr info) {
 if(!p || get<Ptr>(p)!=base+table || get<Ptr>(p+8)!=base+info)throw std::runtime_error("Gesture type differs from verified build");
}
unsigned count(Ptr p,unsigned expected) {
 const auto n=get<std::uint32_t>(p-4);if(n!=expected)throw std::runtime_error("Gesture array shape differs");return n;
}
struct Arena {
 Ptr start{},cursor{},end{};unsigned serial{};
 explicit Arena(unsigned n):serial(n) {
  start=reinterpret_cast<Ptr>(VirtualAlloc(nullptr,1048576,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
  if(!start)throw std::runtime_error("Gesture route allocation failed");cursor=start;end=start+1048576;
 }
 Ptr copy(Ptr source,std::size_t size) {
  auto p=(cursor+15)&~Ptr{15};if(p+size>end)throw std::runtime_error("Gesture arena exhausted");
  if(!memory::peek_bytes(source,reinterpret_cast<void*>(p),size))throw std::runtime_error("Gesture snapshot failed");
  cursor=p+size;return p;
 }
 Ptr object(Ptr source,std::size_t size) {
  const auto p=copy(source-16,size+16)+16;
  // Exported identity prefix must differ from the donor's cached animation ID.
  put(p-16,std::uint64_t{0x534b495443484753});put(p-8,(std::uint64_t{serial}<<32)|(p-start));return p;
 }
 Ptr array(Ptr source,std::size_t width,unsigned old_count,unsigned new_count) {
  count(source,old_count);const auto p=(cursor+15)&~Ptr{15};const auto size=16+width*new_count;
  if(p+size>end)throw std::runtime_error("Gesture arena exhausted");
  if(!memory::peek_bytes(source-16,reinterpret_cast<void*>(p),16+width*old_count))throw std::runtime_error("Gesture array snapshot failed");
  cursor=p+size;put(p+12,new_count);return p+16;
 }
 // Published snapshots are retained for process lifetime: animation workers can
 // still hold them after disable or world reload. At most 16 generations.
};
Ptr riding_tags(Arena& arena,Ptr base,Ptr actor,float ratio) {
 const auto set=resolved(get<Ptr>(actor+48));type(set,base,0x61afc98,0x72b2710);
 const auto groups=get<Ptr>(set+40);const auto group_count=get<unsigned>(groups-4);
 if(group_count<1 || group_count>8)throw std::runtime_error("Unsupported riding tag groups");
 const auto set_copy=arena.object(set,48),group_array=arena.array(groups,8,group_count,group_count);
 put(set_copy+40,group_array);
 for(unsigned i=0;i<group_count;++i) {
  const auto group=resolved(get<Ptr>(groups+i*8));const auto branch_group=get<Ptr>(group+8)==base+0x7299d10;
  type(group,base,branch_group ? 0x61a4960:0x61affa0,branch_group ? 0x7299d10:0x72b29e0);
  const auto tags=get<Ptr>(group+40);const auto tag_count=get<unsigned>(tags-4);
  if(tag_count<1 || tag_count>64)throw std::runtime_error("Unsupported riding tag count");
  const auto group_copy=arena.object(group,branch_group ? 48:56),tag_array=arena.array(tags,8,tag_count,tag_count);
  put(group_array+i*8,group_copy);put(group_copy+40,tag_array);
  for(unsigned j=0;j<tag_count;++j) {
   const auto tag=resolved(get<Ptr>(tags+j*8));const auto info=get<Ptr>(tag+8)-base;
   unsigned size{};Ptr table{};unsigned spline_width{};
   switch(info) {
    case 0x728e1b8:size=88;table=0x6189830;break;
    case 0x728e648:size=88;table=0x6189e38;break;
    case 0x728e3b8:size=96;table=0x6189a38;spline_width=4;break;
    case 0x728e4d8:size=96;table=0x6189bf0;spline_width=1;break;
    case 0x7293980:size=112;table=0x6195600;break;
    default:throw std::runtime_error("Unknown riding control tag");
   }
   type(tag,base,table,info);const auto copy=arena.object(tag,size);put(tag_array+j*8,copy);
   // Both link offsets are durations in the donor clock. Only control events
   // move to the source clock; the animation clip's authored speed stays intact.
   for(const auto offset:{40,48}) {
    const auto time=get<float>(tag+offset);
    if(!std::isfinite(time)||time<0||time>10000)throw std::runtime_error("Invalid riding event time");
    put(copy+offset,time*ratio);
   }
   if(info==0x7293980)for(const auto offset:{100,104})put(copy+offset,get<float>(tag+offset)*ratio);
   if(spline_width) {
    const auto samples=get<Ptr>(tag+88);const auto old_count=get<unsigned>(samples-4);
    if(old_count<1||old_count>10001)throw std::runtime_error("Invalid riding phase spline");
    const auto new_count=static_cast<unsigned>(std::lround((old_count-1)*ratio))+1;
    if(new_count>10001)throw std::runtime_error("Riding phase spline exceeds capacity");
    const auto curve=arena.array(samples,spline_width,old_count,(std::max)(old_count,new_count));
    put(curve-4,new_count);put(copy+88,curve);
    for(unsigned sample=0;sample<new_count;++sample) {
     const auto at=new_count>1 ? double(sample)*(old_count-1)/(new_count-1):0.;
     const auto lo=static_cast<unsigned>(at),hi=(std::min)(lo+1,old_count-1);
     const auto a=spline_width==4 ? get<float>(samples+lo*4):float(get<std::uint8_t>(samples+lo));
     const auto b=spline_width==4 ? get<float>(samples+hi*4):float(get<std::uint8_t>(samples+hi));
     const auto value=float(a+(b-a)*(at-lo));
     if(!std::isfinite(value))throw std::runtime_error("Invalid riding phase sample");
     if(spline_width==4)put(curve+sample*4,value);else put(curve+sample,static_cast<std::uint8_t>(std::lround(value)));
    }
   }
  }
 }
 return set_copy;
}
Ptr clone_sequence(Arena& arena,Ptr base,Ptr donor,Ptr sax) {
 type(donor,base,layout::sequence_vtable,layout::sequence_type);type(sax,base,layout::sequence_vtable,layout::sequence_type);
 auto donor_actors=get<Ptr>(donor+64),sax_actors=get<Ptr>(sax+64);count(donor_actors,1);count(sax_actors,1);
 const auto actor=resolved(get<Ptr>(donor_actors)),source=resolved(get<Ptr>(sax_actors));
 type(actor,base,layout::actor_vtable,layout::actor_type);type(source,base,layout::actor_vtable,layout::actor_type);
 const auto tracks=get<Ptr>(actor+88),source_tracks=get<Ptr>(source+88);count(tracks,2);const auto source_count=get<unsigned>(source_tracks-4);
 if(source_count<1 || source_count>2)throw std::runtime_error("Unsupported body track count");
 const auto animation=get<Ptr>(tracks+16),source_anim=get<Ptr>(source_tracks+16);count(animation,1);count(source_anim,1);
 if(!get<Ptr>(animation+8))throw std::runtime_error("Body track or blend differs");
 const auto donor_length=get<float>(actor+116),sax_length=get<float>(source+116);
 if(!std::isfinite(donor_length)||!std::isfinite(sax_length)||donor_length<1||sax_length<1||donor_length>10000||sax_length>10000)
  throw std::runtime_error("Invalid gesture timing");
 const auto sequence_copy=arena.object(donor,80),actor_copy=arena.object(source,128);
 const auto actor_array=arena.array(donor_actors,8,1,1),track_array=arena.array(tracks,24,2,2),anim_array=arena.array(animation,40,1,1);
 put(sequence_copy+64,actor_array);put(actor_array,actor_copy);put(actor_copy+88,track_array);put(track_array+16,anim_array);
 // Keep the source actor clock, loop point and timed tag collection together.
 // Rescaling a clip into the donor clock changes native playback speed and leaves
 // phase/transition events tied to another animation. The source is already a
 // complete, internally consistent timing recipe.
 // Retain the approved board attachment/init and joint state; the body clip,
 // timing, flags and events come from the source at its authored speed.
 put(actor_copy+96,get<Ptr>(actor+96));put(actor_copy+104,get<Ptr>(actor+104));
 // Static events include standing-only exit branches and wrist locks. Use the
 // approved riding lifecycle, retimed to this clip, so repeated gestures can
 // exit and restart without dismounting. The inherited tag collection is +48;
 // +64 is an embedded native interface, not a tag collection.
 put(actor_copy+48,riding_tags(arena,base,actor,sax_length/donor_length));
 if(!memory::peek_bytes(source_anim,reinterpret_cast<void*>(anim_array),40))throw std::runtime_error("Gesture animation timing snapshot failed");
 // Preserve the authored body blend too (Air Sax: 8 ticks, Arms Crossed: 78).
 // Borrowing the longer donor fade can visibly hold the pose at cycle seams.
 // The donor board track supplies a stable board pose. Its native clip timing
 // stays intact; the source actor owns the body loop's duration and event clock.

 return sequence_copy;
}
struct Gesture {unsigned id{};std::array<Ptr,3> sequence{};};
std::string asset_name(Ptr asset) {
 std::array<char,256> text{};
 if(memory::peek_cstring(get<Ptr>(asset+24),text.data(),text.size())<0)throw std::runtime_error("Unreadable sequence name");
 return text.data();
}
std::vector<Gesture> discover(Ptr base) {
 const auto asset=lookup(base,"Animation/Dingo/CDB_Gesture_OffBoard_Static_Loop");
 type(asset,base,layout::cdb_asset_vtable,layout::cdb_asset_type);
 const auto db=resolved(get<Ptr>(asset+64));type(db,base,layout::cdb_vtable,layout::cdb_type);
 const auto rows=get<unsigned>(db+168);
 if(get<unsigned>(db+164)!=2 || get<unsigned>(db+172)!=4 || rows<1 || rows>254)throw std::runtime_error("Unsupported static gesture query");
 const auto entries=get<Ptr>(db+128),assets=get<Ptr>(db+48);count(assets,rows);count(entries,((rows+15)/16)*4);
 std::vector<Gesture> gestures;
 for(unsigned row=0;row<rows;++row) {
  const auto lane=(row/16)*64+32+row%16;
  if(get<std::uint8_t>(entries+lane+16)!=255)throw std::runtime_error("Static gesture enum mask differs");
  const auto id=get<std::uint8_t>(entries+lane);
  bool duplicate=false;for(const auto& g:gestures)if(g.id==id)duplicate=true;
  if(duplicate)continue;
  auto name=asset_name(resolved(get<Ptr>(assets+row*8)));
  // Backslide's wrapper adds a secondary overlay. Its base sequence supplies
  // the same body clip format; retain the moving-board donor's layer/blend.
  if(const auto wrapper=name.find("_(SuperLayers)");wrapper!=std::string::npos)name.resize(wrapper);
  const auto at=name.find("_loop_");if(at==std::string::npos)throw std::runtime_error("Unknown gesture phase naming");
  Gesture g;g.id=id;
  for(unsigned phase=0;phase<3;++phase) {
   auto source=name;std::string lower=phases[phase];for(auto& c:lower)if(c>='A'&&c<='Z')c+=32;
   source.replace(at,6,"_"+lower+"_");g.sequence[phase]=lookup(base,source);
   // A missing exit is a one-shot gesture: use the approved return-to-skating
   // donor, rather than inventing or repeating the body animation.
   if(!g.sequence[phase] && phase!=2)throw std::runtime_error("Waiting for gesture sequence: "+source);
  }
  gestures.push_back(g);
 }
 return gestures;
}
Patch prepare(Arena& arena,Ptr base,unsigned phase,const std::vector<Gesture>& gestures) {
 const std::string suffix=phases[phase];auto lower=suffix;for(auto& c:lower)if(c>='A'&&c<='Z')c+=32;
 const auto asset=lookup(base,"Animation/Dingo/CDB_Gesture_onb_"+suffix+"_ApprovedContent");
 const auto donor=lookup(base,"Animation/Dingo/seq_proto_onb_gesture_armscrossed_"+lower+"_01");
 if(!asset || !donor)throw std::runtime_error("Waiting for loaded gesture sequences");
 type(asset,base,layout::cdb_asset_vtable,layout::cdb_asset_type);
 const auto original=get<Ptr>(asset+64),db=resolved(original);type(db,base,layout::cdb_vtable,layout::cdb_type);
 const unsigned fields=phase==0 ? 2:1,bytes=fields*2,rows=get<unsigned>(db+168);
 const unsigned vector_count=((rows+15)/16)*fields*2;
 if(get<unsigned>(db+160)!=1 || get<unsigned>(db+164)!=fields || rows<1 || rows>254 ||
    get<unsigned>(db+172)!=bytes || get<unsigned>(db+176)!=2)throw std::runtime_error("On-board query layout differs");
 const auto assets=get<Ptr>(db+48),entries=get<Ptr>(db+128);count(assets,rows);count(entries,vector_count);
 int donor_index=-1;std::array<bool,256> present{};
 for(unsigned row=0;row<rows;++row) {
  const auto lane=(row/16)*16*bytes+row%16;const auto id=get<std::uint8_t>(entries+lane);
  if(get<std::uint8_t>(entries+lane+16)!=255)throw std::runtime_error("On-board enum mask differs");
  present[id]=true;
  if(id==80) {if(donor_index>=0 || resolved(get<Ptr>(assets+row*8))!=donor)throw std::runtime_error("Arms Crossed reference differs");donor_index=static_cast<int>(row);}
 }
 if(donor_index<0)throw std::runtime_error("Arms Crossed on-board route missing");
 unsigned added=0;for(const auto& g:gestures)if(!present[g.id])++added;
 const auto total=rows+added;if(total>254)throw std::runtime_error("Gesture enum table capacity exceeded");
 const auto vectors=((total+15)/16)*fields*2;
 const auto db_copy=arena.object(db,192),asset_array=arena.array(assets,8,rows,total),entry_array=arena.array(entries,16,vector_count,vectors);
 if(vectors>vector_count)std::memset(reinterpret_cast<void*>(entry_array+vector_count*16),0,(vectors-vector_count)*16);
 unsigned row=rows;
 for(const auto& g:gestures)if(!present[g.id]) {
  const auto seq=g.sequence[phase] ? clone_sequence(arena,base,donor,g.sequence[phase]):donor;
  put(asset_array+row*8,seq);
  for(unsigned field=0;field<fields;++field) {
   const auto dst=(row/16)*16*bytes+field*32+row%16;
   put(entry_array+dst,static_cast<std::uint8_t>(g.id));put(entry_array+dst+16,std::uint8_t{255});
  }++row;
 }
 put(db_copy+48,asset_array);put(db_copy+128,entry_array);put(db_copy+168,total);
 return {asset,original,db_copy};
}
std::vector<Patch> prepare_classification(Ptr base,const std::vector<Gesture>& gestures) {
 const auto asset=lookup(base,classification_name);type(asset,base,layout::expression_vtable,layout::expression_type);
 const auto kernel=get<Ptr>(asset+96);const auto vectors=get<unsigned>(kernel-4),pool=get<unsigned>(kernel+36);
 if(vectors<8 || vectors>4096 || get<unsigned>(asset+128)!=vectors*16 || get<unsigned>(kernel+16)!=0x876af93f ||
    pool%4 || pool<4 || pool>1024 || 80+pool>vectors*16 ||
    get<std::uint16_t>(kernel+62)!=0 || get<std::uint16_t>(kernel+64)!=0 || get<std::uint16_t>(kernel+66)!=0)
  throw std::runtime_error("Full-body expression layout differs");
 MEMORY_BASIC_INFORMATION region{};
 if(!VirtualQuery(reinterpret_cast<void*>(kernel+80),&region,sizeof(region)) || region.State!=MEM_COMMIT ||
    (region.Protect&(PAGE_NOACCESS|PAGE_GUARD)) || reinterpret_cast<Ptr>(region.BaseAddress)+region.RegionSize<kernel+80+pool)
  throw std::runtime_error("Full-body constant page unavailable");
 const auto protection=region.Protect&255;
 if(protection!=PAGE_READWRITE && protection!=PAGE_WRITECOPY && protection!=PAGE_EXECUTE_READWRITE && protection!=PAGE_EXECUTE_WRITECOPY)
  throw std::runtime_error("Full-body constant page not writable");
 std::vector<Patch> patches;
 for(unsigned i=0;i<pool/4;++i) {
  const auto value=get<unsigned>(kernel+80+i*4);
  if(value>254)throw std::runtime_error("Full-body enum pool differs");
  for(const auto& g:gestures)if(value==g.id) {
   // Never use -1: it is the normal idle/default gesture state.
   patches.push_back({asset,value,0x7fffffff,80+i*4,true,kernel});break;
  }
 }
 return patches;
}
bool exchange(Ptr slot,Ptr expected,Ptr desired) {
 MEMORY_BASIC_INFORMATION region{};
 if(!VirtualQuery(reinterpret_cast<void*>(slot),&region,sizeof(region)) || region.State!=MEM_COMMIT ||
    (region.Protect&(PAGE_NOACCESS|PAGE_GUARD)))return false;
 const auto p=region.Protect&255;
 if(p!=PAGE_READWRITE && p!=PAGE_WRITECOPY && p!=PAGE_EXECUTE_READWRITE && p!=PAGE_EXECUTE_WRITECOPY)return false;
 return reinterpret_cast<Ptr>(InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot),reinterpret_cast<void*>(desired),reinterpret_cast<void*>(expected)))==expected;
}
Ptr patch_address(const Patch& p) {return (p.word ? p.anchor:p.asset)+p.slot;}
bool exchange_word(Ptr slot,LONG expected,LONG desired) noexcept {
 // Identity/layout and original value were verified during preparation. A cold
 // protection check belongs there, not in every mount/dismount word exchange.
 __try {return slot%4==0 && InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(slot),desired,expected)==expected;}
 __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool publish(const Patch& p,bool undo=false) {
 if(!p.word)return exchange(patch_address(p),undo?p.replacement:p.original,undo?p.original:p.replacement);
 return exchange_word(patch_address(p),static_cast<LONG>(undo?p.replacement:p.original),static_cast<LONG>(undo?p.original:p.replacement));
}
void restore(std::vector<Patch>& patches,Ptr base) {
 const auto classifier=patches.size()>3 ? lookup(base,classification_name):0;
 for(unsigned i=0;i<patches.size();++i)if(patches[i].asset) {
  const auto name=i>=3 ? std::string(classification_name):"Animation/Dingo/CDB_Gesture_onb_"+std::string(phases[i])+"_ApprovedContent";
  const auto current=i>=3 ? classifier:lookup(base,name);
  if(current==patches[i].asset && (!patches[i].word || get<Ptr>(current+96)==patches[i].anchor))publish(patches[i],true);
 }
 patches={};
}
}
std::string status() {auto& s=state();std::lock_guard lock(s.mutex);return s.detail;}
void tick(Ptr base,bool requested,bool mounted) noexcept {
 auto& s=state();try {
  std::lock_guard lock(s.mutex);
  if(!requested) {restore(s.patches,base);s.detail="Automatic on-board gesture routes disabled";return;}
  // A board transition is applied on the next client frame, not the one-second
  // asset discovery timer. Keep the route snapshots but restore native off-board
  // classification; do not allocate another arena for each mount/dismount.
  if(mounted!=s.mounted && !s.patches.empty()) {
   const auto transition_start=std::chrono::steady_clock::now();
   bool valid=true;const auto classifier=lookup(base,classification_name);
   for(const auto& p:s.patches)if(p.word && (classifier!=p.asset || get<Ptr>(p.asset+96)!=p.anchor))valid=false;
   if(valid)for(const auto& p:s.patches)if(p.word && !publish(p,!mounted))valid=false;
   if(!valid){restore(s.patches,base);s.next=0;}
   else logging::log(logging::Level::info,logging::Channel::runtime,"Board gesture: {} classification transition {:.3f} ms",mounted ? "on-board":"off-board native",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-transition_start).count());
   s.next=0;
  }
  s.mounted=mounted;
  const auto now=GetTickCount64();if(now<s.next)return;s.next=now+1000;
  if(!s.patches.empty()) {
   bool current=true;const auto classifier=lookup(base,classification_name);
   for(unsigned i=0;i<s.patches.size();++i) {
    const auto name=i>=3 ? std::string(classification_name):"Animation/Dingo/CDB_Gesture_onb_"+std::string(phases[i])+"_ApprovedContent";
    const auto asset=i>=3 ? classifier:lookup(base,name);
    const auto& p=s.patches[i];
    if(asset!=p.asset || (p.word && get<Ptr>(asset+96)!=p.anchor) ||
       (p.word ? get<unsigned>(patch_address(p)):get<Ptr>(patch_address(p)))!=(p.word && !mounted ? p.original:p.replacement))current=false;
   }
   if(current){s.detail=mounted ? "Automatic board gesture routes active":"Off-board native gestures restored; on-board routes retained";return;}
   restore(s.patches,base);
  }
  if(s.generations>=16) {s.detail="Gesture reload limit reached; restart to rearm";return;}
  Arena arena(++s.generations);std::vector<Patch> prepared;std::vector<Gesture> gestures;
  try {gestures=discover(base);for(unsigned i=0;i<3;++i)prepared.push_back(prepare(arena,base,i,gestures));
   const auto classification=prepare_classification(base,gestures);prepared.insert(prepared.end(),classification.begin(),classification.end());}
  catch(...) {VirtualFree(reinterpret_cast<void*>(arena.start),0,MEM_RELEASE);--s.generations;throw;}
  unsigned applied=0;
  for(;applied<prepared.size();++applied)if(!(prepared[applied].word && !mounted) && !publish(prepared[applied]))break;
  if(applied!=prepared.size()) {restore(prepared,base);s.detail="Gesture route publication changed; no active override";return;}
  s.patches=prepared;s.detail="Automatic on-board gesture pass active: "+std::to_string(gestures.size())+" static gestures; live playback test required";
  logging::log(logging::Level::info,logging::Channel::runtime,"Board gesture: {} static gestures discovered; {} owned classification edits published. Live playback test required.",gestures.size(),prepared.size()-3);
 }catch(const std::exception& e) {std::lock_guard lock(s.mutex);if(s.detail!=e.what())logging::log(logging::Level::info,logging::Channel::runtime,"Board gesture: {}",e.what());s.detail=e.what();}catch(...) {}
}
}
