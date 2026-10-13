#include "board_gesture_routes.h"
#include <Windows.h>
#include <array>
#include <vector>
#include <string>
#include <cstring>
#include <cmath>
#include <stdexcept>
extern void check(bool,const char*);
namespace {
using Ptr=std::uintptr_t;
template<class T> void put(Ptr p,T v){std::memcpy(reinterpret_cast<void*>(p),&v,sizeof(v));}
template<class T> T get(Ptr p){T v{};std::memcpy(&v,reinterpret_cast<void*>(p),sizeof(v));return v;}
struct Buffer {
 alignas(16) std::array<unsigned char,1024> b{};
 Ptr ptr(){return reinterpret_cast<Ptr>(b.data())+16;}
 void header(unsigned n){put(ptr()-4,n);}
 void type(Ptr base,Ptr table,Ptr info){put(ptr(),base+table);put(ptr()+8,base+info);}
};
std::uint64_t hash(const std::string& s){std::uint64_t h=5381;for(auto c:s) {if(c>='A'&&c<='Z')c+=32;h=(h*33)^static_cast<unsigned char>(c);}return h;}
struct Phase {
 std::string db_name,donor_name,sax_name;
 Buffer asset,db,assets,entries,donor,sax,actor,sax_actor,actors,sax_actors,tracks,sax_tracks,anim,sax_anim,node[3],control[3];
 Buffer tag_set,groups,group,tags,bool_tag,spline_tag,quantized_tag,branch_tag,spline,quantized,branch_group,branch_tags;
 unsigned fields{},vectors{};
 void setup(Ptr base,unsigned phase) {
  const std::array<std::string,3> names{"Into","Loop","Exit"};auto lower=names[phase];for(auto& c:lower)if(c>='A'&&c<='Z')c+=32;
  db_name="Animation/Dingo/CDB_Gesture_onb_"+names[phase]+"_ApprovedContent";
  donor_name="Animation/Dingo/seq_proto_onb_gesture_armscrossed_"+lower+"_01";
  sax_name="Animation/Dingo/seq_proto_ofb_gesture_airsax_"+lower+"_lfoot_01";
  asset.type(base,0x61a7708,0x729beb0);db.type(base,0x61a7390,0x729bb48);
  put(asset.ptr()+24,reinterpret_cast<Ptr>(db_name.c_str()));put(asset.ptr()+64,db.ptr());
  fields=phase==0?2:1;vectors=fields*6;assets.header(43);entries.header(vectors);
  put(db.ptr()+48,assets.ptr());put(db.ptr()+128,entries.ptr());
  put(db.ptr()+160,unsigned{1});put(db.ptr()+164,fields);put(db.ptr()+168,unsigned{43});put(db.ptr()+172,fields*2);put(db.ptr()+176,unsigned{2});
  for(unsigned row=0;row<43;++row)for(unsigned f=0;f<fields;++f) {
   const auto offset=(row/16)*fields*32+f*32+row%16;
   put(entries.ptr()+offset,static_cast<unsigned char>(row==28?80:row+1));put(entries.ptr()+offset+16,static_cast<unsigned char>(255));
  }
  for(auto* seq:{&donor,&sax})seq->type(base,0x61bab98,0x72b8490);
  for(auto* act:{&actor,&sax_actor})act->type(base,0x61ba838,0x72b87d8);
  put(donor.ptr()+24,reinterpret_cast<Ptr>(donor_name.c_str()));put(sax.ptr()+24,reinterpret_cast<Ptr>(sax_name.c_str()));
  actors.header(1);sax_actors.header(1);tracks.header(2);sax_tracks.header(2);anim.header(1);sax_anim.header(1);
  put(donor.ptr()+64,actors.ptr());put(sax.ptr()+64,sax_actors.ptr());put(actors.ptr(),actor.ptr());put(sax_actors.ptr(),sax_actor.ptr());
  put(actor.ptr()+64,Ptr{0xaaaa0000});put(sax_actor.ptr()+64,Ptr{0xbbbb0000});put(actor.ptr()+104,Ptr{0xcccc0000});put(sax_actor.ptr()+104,Ptr{0xdddd0000});put(actor.ptr()+112,-1.0f);put(sax_actor.ptr()+112,10.0f);
  tag_set.type(base,0x61afc98,0x72b2710);group.type(base,0x61affa0,0x72b29e0);
  groups.header(2);tags.header(4);put(actor.ptr()+48,tag_set.ptr());put(sax_actor.ptr()+48,Ptr{0xdead0000});
  put(tag_set.ptr()+40,groups.ptr());put(groups.ptr(),group.ptr());put(group.ptr()+40,tags.ptr());
  branch_group.type(base,0x61a4960,0x7299d10);branch_tags.header(1);
  put(groups.ptr()+8,branch_group.ptr());put(branch_group.ptr()+40,branch_tags.ptr());put(branch_tags.ptr(),branch_tag.ptr());
  bool_tag.type(base,0x6189e38,0x728e648);spline_tag.type(base,0x6189a38,0x728e3b8);
  quantized_tag.type(base,0x6189bf0,0x728e4d8);branch_tag.type(base,0x6195600,0x7293980);
  std::array<Buffer*,4> events{&bool_tag,&spline_tag,&quantized_tag,&branch_tag};
  for(unsigned j=0;j<events.size();++j) {
   put(tags.ptr()+j*8,events[j]->ptr());put(events[j]->ptr()+40,15.f);put(events[j]->ptr()+48,85.f);
   put(events[j]->ptr()+64,Ptr{0xfeed0000+j*16});
  }
  put(branch_tag.ptr()+100,1.f);put(branch_tag.ptr()+104,2.f);
  spline.header(101);quantized.header(101);put(spline_tag.ptr()+88,spline.ptr());put(quantized_tag.ptr()+88,quantized.ptr());
  for(unsigned j=0;j<101;++j){put(spline.ptr()+j*4,float(j)/100);put(quantized.ptr()+j,static_cast<std::uint8_t>(j));}
  put(actor.ptr()+88,tracks.ptr());put(sax_actor.ptr()+88,sax_tracks.ptr());put(actor.ptr()+116,100.0f);put(sax_actor.ptr()+116,200.0f);
  put(tracks.ptr(),Ptr{0xabcde0});put(sax_tracks.ptr(),Ptr{0xabcde0});put(tracks.ptr()+16,anim.ptr());put(sax_tracks.ptr()+16,sax_anim.ptr());
  put(anim.ptr(),Ptr{0x123456});put(anim.ptr()+8,Ptr{0x654321});put(anim.ptr()+24,1.0f);put(anim.ptr()+34,std::uint16_t{100});
  put(sax_anim.ptr(),Ptr{0x987654});put(sax_anim.ptr()+8,Ptr{0x777777});put(sax_anim.ptr()+24,1.0f);put(sax_anim.ptr()+34,std::uint16_t{200});
  put(assets.ptr()+28*8,donor.ptr());
  const std::array<std::string*,3> keys{&db_name,&donor_name,&sax_name};const std::array<Ptr,3> values{asset.ptr(),donor.ptr(),sax.ptr()};
  for(unsigned i=0;i<3;++i){put(node[i].ptr(),hash(*keys[i]));put(node[i].ptr()+8,control[i].ptr());put(control[i].ptr(),values[i]);}
 }
};
}
void route_tests() {
 using namespace dingosdk::board_gesture;
 const auto reservation=VirtualAlloc(nullptr,0x7730000,MEM_RESERVE,PAGE_NOACCESS);
 if(!reservation)throw std::runtime_error("route test image reservation");
 const auto base=reinterpret_cast<Ptr>(reservation);
 try {
  check(VirtualAlloc(reinterpret_cast<void*>(base+0x7726000),4096,MEM_COMMIT,PAGE_READWRITE)!=nullptr,"route domain page");
  std::array<Phase,3> phase{};Buffer owner,manager,class_owner,class_manager,static_asset,static_db,static_entries,static_assets,static_node,static_control,classifier,class_node,class_control;
  const std::string class_name="Animation/Dingo/EBool.Intent.FullBodyGesture";
  alignas(16) std::array<unsigned char,6432> kernel_storage{};
  const auto kernel=reinterpret_cast<Ptr>(kernel_storage.data())+16;
  const std::array<unsigned,68> constants{
   19,20,27,31,34,35,37,42,44,38,40,41,45,43,39,36,22,46,25,47,60,55,48,71,66,67,68,49,69,56,50,53,52,58,54,70,51,59,72,73,74,75,76,77,78,79,57,86,85,82,84,87,88,89,96,97,98,100,99,107,108,109,110,111,112,113,114,115};
  classifier.type(base,0x61b1390,0x72b3738);put(classifier.ptr()+24,reinterpret_cast<Ptr>(class_name.c_str()));
  put(classifier.ptr()+96,kernel);put(classifier.ptr()+128,unsigned{6416});put(kernel-4,unsigned{401});
  put(kernel+16,unsigned{0x876af93f});put(kernel+32,unsigned{64});put(kernel+36,unsigned{272});put(kernel+40,unsigned{138});put(kernel+44,unsigned{1225});
  put(kernel,Ptr{0xabc000});put(kernel+8,Ptr{0xabc000});
  const Ptr existing_actor_kernel=kernel;
  std::memcpy(reinterpret_cast<void*>(kernel+80),constants.data(),272);
  std::memset(reinterpret_cast<void*>(kernel+352),0xab,6416-352);
  put(class_node.ptr(),hash(class_name));put(class_node.ptr()+8,class_control.ptr());put(class_control.ptr(),classifier.ptr());
  std::vector<Ptr> buckets(99733);
  for(unsigned i=0;i<3;++i)phase[i].setup(base,i);
  put(base+0x7726d10+6*8,owner.ptr());put(owner.ptr()+0x48,manager.ptr());put(manager.ptr()+0xf8,reinterpret_cast<Ptr>(buckets.data()));put(manager.ptr()+0x100,unsigned{99733});
  for(unsigned i=0;i<9;++i) {
   const auto node=phase[i/3].node[i%3].ptr();const auto bucket=get<std::uint64_t>(node)%buckets.size();
   put(node+16,buckets[bucket]);buckets[bucket]=node;
  }
  const std::string static_name="Animation/Dingo/CDB_Gesture_OffBoard_Static_Loop";
  static_asset.type(base,0x61a7708,0x729beb0);static_db.type(base,0x61a7390,0x729bb48);
  put(static_asset.ptr()+24,reinterpret_cast<Ptr>(static_name.c_str()));put(static_asset.ptr()+64,static_db.ptr());
  put(static_db.ptr()+48,static_assets.ptr());put(static_db.ptr()+128,static_entries.ptr());
  put(static_db.ptr()+164,unsigned{2});put(static_db.ptr()+168,unsigned{2});put(static_db.ptr()+172,unsigned{4});
  static_assets.header(2);static_entries.header(4);
  put(static_assets.ptr(),phase[1].sax.ptr());put(static_assets.ptr()+8,phase[1].sax.ptr());
  put(static_entries.ptr()+32,std::uint8_t{47});put(static_entries.ptr()+48,std::uint8_t{255});
  put(static_entries.ptr()+33,std::uint8_t{50});put(static_entries.ptr()+49,std::uint8_t{255});
  put(static_node.ptr(),hash(static_name));put(static_node.ptr()+8,static_control.ptr());put(static_control.ptr(),static_asset.ptr());
  const auto static_bucket=hash(static_name)%buckets.size();put(static_node.ptr()+16,buckets[static_bucket]);buckets[static_bucket]=static_node.ptr();
  std::vector<Ptr> class_buckets(99733);
  put(base+0x7726d10+5*8,class_owner.ptr());put(class_owner.ptr()+0x48,class_manager.ptr());
  put(class_manager.ptr()+0xf8,reinterpret_cast<Ptr>(class_buckets.data()));put(class_manager.ptr()+0x100,unsigned{99733});
  class_buckets[hash(class_name)%class_buckets.size()]=class_node.ptr();
  const auto original_kernel=kernel_storage;
  put(phase[2].db.ptr()+172,unsigned{9});routes::tick(base,true);
  check(get<Ptr>(phase[0].asset.ptr()+64)==phase[0].db.ptr(),"invalid exit route publishes none of the phases");
  put(phase[2].db.ptr()+172,unsigned{2});put(kernel+80+19*4,unsigned{999});Sleep(1010);routes::tick(base,true);
  check(get<Ptr>(phase[0].asset.ptr()+64)==phase[0].db.ptr() && get<Ptr>(classifier.ptr()+96)==kernel,"unexpected classification fails without publishing any route");
  put(kernel+80+19*4,unsigned{47});Sleep(1010);routes::tick(base,true);
  if(routes::status().find("active")==std::string::npos)throw std::runtime_error("route setup: "+routes::status());
  check(true,"all verified routes become active");
  check(get<unsigned>(manager.ptr()+0x100)==99733,"multiplayer-size asset registry supported");
  const auto class_copy=get<Ptr>(classifier.ptr()+96);
  check(class_copy==kernel,"existing actor kernel and prepared pointers retained");
  check(get<unsigned>(kernel+80+19*4)==0x7fffffff,"Air Sax removed from original kernel seen by existing actor contexts");
  check(std::memcmp(original_kernel.data()+16+352,reinterpret_cast<void*>(class_copy+352),6416-352)==0,"compiled expression instructions unchanged");
  check(get<Ptr>(kernel)==0xabc000 && get<Ptr>(kernel+8)==0xabc000,"native allocator headers retained");
  check(get<unsigned>(existing_actor_kernel+156)==0x7fffffff,"previously prepared actor observes changed page-zero constant");
  bool only_air_changes=true;
  for(unsigned value=0;value<116;++value) {
   bool before=false,after=false;
   for(unsigned i=0;i<68;++i) {before|=constants[i]==value;after|=get<unsigned>(class_copy+80+i*4)==value;}
   if(before!=after && value!=47 && value!=50)only_air_changes=false;
  }
  check(only_air_changes,"other gesture classification membership preserved");
  for(auto& p:phase) {
   const auto replacement=get<Ptr>(p.asset.ptr()+64);check(replacement!=p.db.ptr(),"database pointer swapped");
   check(get<unsigned>(replacement+168)==45 && get<unsigned>(p.db.ptr()+168)==43,"new row appended without modifying source database");
   const auto assets=get<Ptr>(replacement+48),entries=get<Ptr>(replacement+128);
   check(get<unsigned>(assets-4)==45 && get<Ptr>(assets+28*8)==p.donor.ptr(),"existing donor remains selectable");
   bool preserved=true;
   for(unsigned row=0;row<43;++row)for(unsigned f=0;f<p.fields;++f) {
    const auto at=(row/16)*p.fields*32+f*32+row%16;
    if(get<unsigned char>(entries+at)!=get<unsigned char>(p.entries.ptr()+at)||get<unsigned char>(entries+at+16)!=255)preserved=false;
   }
   check(preserved,"all existing SIMD query rows preserved");
   const auto offset=(43/16)*p.fields*32+43%16;
   check(get<unsigned char>(entries+offset)==47 && get<unsigned char>(entries+offset+16)==255,"Air Sax query inserted with exact mask");
   if(p.fields==2)check(get<unsigned char>(entries+offset+32)==47,"Into selected gesture metadata is Air Sax");
   const auto seq=get<Ptr>(assets+43*8),actor=get<Ptr>(get<Ptr>(seq+64)),track=get<Ptr>(actor+88),anim=get<Ptr>(track+16);
   check(get<Ptr>(anim)==0x987654 && get<Ptr>(anim+8)==0x777777,"source body clip and authored blend retained");
   check(get<float>(anim+24)==1.0f && get<std::uint16_t>(anim+34)==200 && get<float>(actor+116)==200,"source clip duration and native speed retained");
   check(get<float>(anim+24)*get<std::uint16_t>(anim+34)==get<float>(actor+116),"source clip endpoint and original controller endpoint agree");
   check(get<Ptr>(actor+64)==get<Ptr>(p.sax_actor.ptr()+64) && get<float>(actor+112)==get<float>(p.sax_actor.ptr()+112),"source loop point and native interface retained");
   const auto tag_set=get<Ptr>(actor+48),group=get<Ptr>(get<Ptr>(tag_set+40)),tags=get<Ptr>(group+40);
   check(tag_set!=p.tag_set.ptr() && tag_set!=get<Ptr>(p.sax_actor.ptr()+48),"riding event snapshot replaces standing controls without editing originals");
   const auto event=get<Ptr>(tags),spline_event=get<Ptr>(tags+8),quantized_event=get<Ptr>(tags+16),branch=get<Ptr>(tags+24);
   check(get<float>(event+40)==30 && get<float>(event+48)==170 && get<float>(p.bool_tag.ptr()+48)==85,"riding control intervals follow source duration without mutating donor");
   check(get<Ptr>(branch+64)==get<Ptr>(p.branch_tag.ptr()+64) && get<float>(branch+100)==2 && get<float>(branch+104)==4,"riding exit branch and retimed tuning retained");
   const auto branch_group=get<Ptr>(get<Ptr>(tag_set+40)+8);
   check(get<Ptr>(branch_group+8)==base+0x7299d10 && get<float>(get<Ptr>(get<Ptr>(branch_group+40))+48)==170,"separate native branch collection retained and retimed");
   const auto spline=get<Ptr>(spline_event+88),quantized=get<Ptr>(quantized_event+88);
   check(get<unsigned>(spline-4)==201 && get<float>(spline+100*4)==.5f && get<float>(spline+200*4)==1.f,"riding phase spline spans source clock through its last tick");
   check(get<unsigned>(quantized-4)==201 && get<std::uint8_t>(quantized+100)==50 && get<std::uint8_t>(quantized+200)==100,"quantized riding weight resampled without changing endpoints");
   check(get<Ptr>(actor+104)==0xcccc0000,"on-board actor init retained separately from source timing");
   check(get<Ptr>(p.anim.ptr())==0x123456,"donor body clip untouched");
   check(std::memcmp(reinterpret_cast<void*>(track+24),reinterpret_cast<void*>(p.tracks.ptr()+24),24)==0,"board track preserved");
  }
  routes::tick(base,true,false);
  check(get<unsigned>(kernel+156)==47 && get<unsigned>(kernel+200)==50,"dismount immediately restores native full-body classification");
  const auto retained=get<Ptr>(phase[0].asset.ptr()+64);
  routes::tick(base,true,true);
  check(get<unsigned>(kernel+156)==0x7fffffff && get<Ptr>(phase[0].asset.ptr()+64)==retained,"remount immediately rearms without replacing route arena");
  check(get<unsigned>(kernel+80+30*4)==0x7fffffff,"second gesture classified automatically");
  for(auto& p:phase) {
   const auto db=get<Ptr>(p.asset.ptr()+64),entries=get<Ptr>(db+128);
   const auto at=(44/16)*p.fields*32+44%16;
   check(get<std::uint8_t>(entries+at)==50,"second gesture appended across SIMD rows");
  }
  routes::tick(base,false);
  for(auto& p:phase)check(get<Ptr>(p.asset.ptr()+64)==p.db.ptr(),"disable restores original phase database");
  check(get<Ptr>(classifier.ptr()+96)==kernel && get<unsigned>(kernel+156)==47,"disable restores original full-body classification constant");
  VirtualFree(reservation,0,MEM_RELEASE);
 }catch(...) {routes::tick(base,false);VirtualFree(reservation,0,MEM_RELEASE);throw;}
}
