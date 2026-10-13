#include "board_gesture_layout.h"
#include "board_gesture.h"
#include "board_gesture_routes.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/20260929/offboard_flight.h"
#include <Windows.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <mutex>
#include <string_view>
#include <vector>
#include <algorithm>
#include <chrono>

namespace dingosdk::board_gesture {
namespace {
namespace engine=game::build::v20260929::engine;
struct Item { std::uintptr_t asset{},data{}; };
struct Owned {Item item;bool layered{},stationary{};};
struct State {
    std::atomic<bool> requested{true};
    std::mutex mutex;
    std::uintptr_t manager{};
    std::vector<Owned> owned;
    std::vector<Item> cached;std::uintptr_t cached_game{},reference_asset{};
    std::uint64_t next_scan{};bool previous_effective{true};
    std::string detail="Waiting for automatic gesture assets";
};
State& state() { static State value; return value; }
constexpr std::uint32_t sax_hash=4238209789u,arms_hash=986424460u;
constexpr std::uintptr_t gesture_vtable=layout::item_vtable,gesture_type=layout::item_type;
bool key_matches(std::uintptr_t asset,std::string_view expected,std::uint32_t hash) {
    std::uintptr_t key{}; std::uint32_t actual{}; std::array<char,96> text{};
    return memory::peek(asset+0x38,key) && memory::peek(asset+0x48,actual) && actual==hash &&
        memory::peek_cstring(key,text.data(),text.size())>=0 && std::string_view(text.data())==expected;
}
bool item_valid(std::uintptr_t base,Item item,std::string_view key,std::uint32_t hash,std::int32_t value,
    std::uint8_t& layered,std::uintptr_t& game_state) {
    std::uintptr_t data{},vtable{},type{}; std::int32_t actual{}; std::uint8_t stationary{};
    return key_matches(item.asset,key,hash) && memory::peek(item.asset+0x28,data) && (data&~std::uintptr_t{4})==item.data &&
        memory::peek(item.data,vtable) && vtable==base+gesture_vtable &&
        memory::peek(item.data+8,type) && type==base+gesture_type &&
        memory::peek(item.data+0x18,game_state) && game_state &&
        memory::peek(item.data+0x20,actual) && actual==value &&
        memory::peek(item.data+0x24,layered) && layered<=1 &&
        memory::peek(item.data+0x25,stationary) && stationary==0;
}
bool find_pair(std::uintptr_t base,std::uintptr_t& manager,Item& sax,Item& arms) {
    std::uintptr_t vtable{},buckets{}; std::uint32_t bucket_count{},item_count{}; std::uint8_t ready{};
    if(!memory::peek(base+engine::cosmetics_manager,manager) || !manager ||
       !memory::peek(manager,vtable) || vtable!=base+engine::cosmetics_manager_vtable ||
       !memory::peek(manager+0xa8,ready) || ready!=1 || !memory::peek(manager+0x30,buckets) || !buckets ||
       !memory::peek(manager+0x38,bucket_count) || !bucket_count || bucket_count>16384 ||
       !memory::peek(manager+0x3c,item_count) || !item_count || item_count>8192) return false;
    std::uint32_t visited=0;
    for(std::uint32_t bucket=0;bucket<bucket_count;++bucket) {
        std::uintptr_t node{}; if(!memory::peek(buckets+bucket*8ULL,node)) return false;
        while(node) {
            if(++visited>item_count) return false;
            std::uint32_t hash{}; std::uintptr_t asset{},next{};
            if(!memory::peek(node,hash) || !memory::peek(node+0x10,next)) return false;
            if(hash==sax_hash || hash==arms_hash) {
                if(!memory::peek(node+8,asset)) return false; asset&=~std::uintptr_t{4};
                const std::string_view key=hash==sax_hash ? "own_rctn_gesture_all_airsaxophone" : "own_rctn_gesture_all_armscrossed";
                std::uintptr_t data{};
                if(!key_matches(asset,key,hash) || !memory::peek(asset+0x28,data)) return false;
                (hash==sax_hash ? sax : arms)=Item{asset,data&~std::uintptr_t{4}};
                if(sax.data && arms.data) return true;
            }
            node=next;
        }
    }
    return false;
}
bool writable_byte(std::uintptr_t at) {
    MEMORY_BASIC_INFORMATION region{};
    if(!VirtualQuery(reinterpret_cast<void*>(at),&region,sizeof(region)) || region.State!=MEM_COMMIT ||
       (region.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return false;
    const auto protection=region.Protect&0xff;
    return protection==PAGE_READWRITE || protection==PAGE_WRITECOPY || protection==PAGE_EXECUTE_READWRITE || protection==PAGE_EXECUTE_WRITECOPY;
}
bool write_flag(std::uintptr_t at,std::uint8_t value) noexcept {
 // Cold discovery verifies page protection. Mode switches use an atomic byte
 // exchange, guarded against an unloaded object; avoid per-flag system calls.
 __try {return static_cast<std::uint8_t>(_InterlockedCompareExchange8(reinterpret_cast<volatile CHAR*>(at),static_cast<CHAR>(value),static_cast<CHAR>(1-value)))==1-value;}
 __except(EXCEPTION_EXECUTE_HANDLER){return false;}

}
}
void set_enabled(bool value) noexcept { state().requested.store(value); }
bool enabled() noexcept { return state().requested.load(); }
std::string status() { auto& s=state(); std::lock_guard lock(s.mutex); return s.detail+"; "+routes::status(); }
bool mounted(std::uintptr_t base,std::uintptr_t client) noexcept {
 using Ptr=std::uintptr_t;
 const auto pointer=[](Ptr p){Ptr value{};memory::peek(p,value);return value;};
 const auto context=pointer(client+8);std::uint32_t offset{};
 if(!client || pointer(client)!=base+engine::client_vtable || !context ||
    !memory::peek(base+engine::context_player_manager_offset,offset) || offset>0x1000000)return false;
 const auto manager=pointer(context+offset);
 if(!manager || pointer(manager)!=base+engine::local_player_manager_vtable)return false;
 const auto begin=pointer(manager+0x4c8),end=pointer(manager+0x4d0);
 if(!begin || end!=begin+8)return false;
 const auto player=pointer(begin),entity=pointer(player+0xb8);
 if(!player || pointer(player)!=base+engine::local_player_vtable || pointer(player+0x78)!=context ||
    !entity || pointer(entity)!=base+engine::skater_entity_vtable || pointer(entity+0xf8)!=player)return false;
 const auto component=pointer(entity+0x628);
 if(!component || pointer(component)!=base+engine::skater_component_vtable)return false;
 const auto core=pointer(component+0x70);
 if(!core || pointer(core)!=base+game::build::v20260929::no_bail::bail_core_vtable)return false;
 const auto physics_state=pointer(core+0x3b0),physics_context=pointer(core+0x3c0);
 if(!physics_state || !physics_context || pointer(physics_state)==base+game::build::v20260929::offboard_flight::offboard_flight_vtable)return false;
 std::uint32_t first{},second{};
 return memory::peek(physics_context+0x13c4,first) && memory::peek(physics_context+0x13d4,second) &&
        !(first&0x8000u) && !(second&0x08000000u);
}
void tick(std::uintptr_t base,bool on_board) noexcept {
 auto& s=state();try {
  std::lock_guard lock(s.mutex);const bool requested=s.requested.load() && on_board;
  const bool transition=requested!=s.previous_effective;s.previous_effective=requested;
  const auto transition_start=std::chrono::steady_clock::now();
  const auto now=GetTickCount64();if(!transition && now<s.next_scan)return;s.next_scan=now+1000;
  std::uintptr_t live_manager{},live_vtable{};std::uint8_t ready{};
  bool valid=memory::peek(base+engine::cosmetics_manager,live_manager) && live_manager==s.manager && live_manager &&
   memory::peek(live_manager,live_vtable) && live_vtable==base+engine::cosmetics_manager_vtable &&
   memory::peek(live_manager+0xa8,ready) && ready==1 && !s.cached.empty();
  if(valid)for(const auto& item:s.cached) {
   std::uintptr_t data{},type{},gs{};std::int32_t value{};
   if(!memory::peek(item.asset+40,data) || (data&~std::uintptr_t{4})!=item.data ||
      !memory::peek(item.data,live_vtable)||live_vtable!=base+gesture_vtable||!memory::peek(item.data+8,type)||type!=base+gesture_type||
      !memory::peek(item.data+24,gs)||gs!=s.cached_game||!memory::peek(item.data+32,value)||value<0||value>254||
      (item.asset==s.reference_asset && value!=80)){valid=false;break;}
  }
  std::vector<Item> items;
  if(valid)items=s.cached;
  else {
  Item sax,arms;std::uintptr_t manager{},game{};std::uint8_t layer{};
  // Keep a known native layered gesture as an ABI/game-state anchor.
  if(!find_pair(base,manager,sax,arms) || !item_valid(base,arms,"own_rctn_gesture_all_armscrossed",arms_hash,80,layer,game) || layer!=1) {
   s.detail="Waiting for verified gesture item layout";return;
  }
  if(s.manager!=manager){s.owned.clear();s.cached.clear();s.manager=manager;}
  std::uintptr_t buckets{};std::uint32_t bucket_count{},item_count{};
  if(!memory::peek(manager+0x30,buckets)||!memory::peek(manager+0x38,bucket_count)||!memory::peek(manager+0x3c,item_count))return;
  unsigned visited=0;
  for(unsigned bucket=0;bucket<bucket_count;++bucket) {
   std::uintptr_t node{};if(!memory::peek(buckets+bucket*8ULL,node))return;
   while(node) {
    if(++visited>item_count)return;
    std::uintptr_t asset{},data{},next{},key{},vt{},info{},gs{};std::int32_t value{};std::array<char,128> text{};std::uint8_t l{},st{};
    if(!memory::peek(node+8,asset)||!memory::peek(node+16,next))return;asset&=~std::uintptr_t{4};
    if(memory::peek(asset+0x38,key) && memory::peek_cstring(key,text.data(),text.size())>=0 &&
       std::string_view(text.data()).starts_with("own_rctn_gesture_all_") && memory::peek(asset+0x28,data)) {
     data&=~std::uintptr_t{4};
     if(!memory::peek(data,vt)||vt!=base+gesture_vtable||!memory::peek(data+8,info)||info!=base+gesture_type||
        !memory::peek(data+24,gs)||gs!=game||!memory::peek(data+32,value)||value<0||value>254||
        !memory::peek(data+36,l)||l>1||!memory::peek(data+37,st)||st>1) {
      s.detail="Gesture item layout differs; no new writes";return;
     }
     if(!writable_byte(data+36)||!writable_byte(data+37)){s.detail="Gesture flags not writable";return;}
     items.push_back({asset,data});
    }node=next;
   }
  }
   s.cached=items;s.cached_game=game;s.reference_asset=arms.asset;
  }
  // Drop borrowed identities that no longer occur in the current manager.
  std::erase_if(s.owned,[&](const Owned& o){return std::none_of(items.begin(),items.end(),[&](const Item& i){return i.asset==o.item.asset && i.data==o.item.data;});});
  if(!requested) {
   for(const auto& o:s.owned) {
    std::uint8_t l{},st{};
    if(o.layered && memory::peek(o.item.data+36,l) && l==1)write_flag(o.item.data+36,0);
    if(o.stationary && memory::peek(o.item.data+37,st) && st==0)write_flag(o.item.data+37,1);
   }s.owned.clear();s.detail="Off-board original gesture item flags restored";
   if(transition)logging::log(logging::Level::info,logging::Channel::runtime,"Board gesture off-board item transition {:.3f} ms; cached={}",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-transition_start).count(),valid);
   return;
  }
  unsigned changed=0;
  for(const auto& item:items) {
   std::uint8_t l{},st{};if(!memory::peek(item.data+36,l)||!memory::peek(item.data+37,st))continue;
   auto it=std::find_if(s.owned.begin(),s.owned.end(),[&](const Owned& o){return o.item.asset==item.asset && o.item.data==item.data;});
   if((l==0||st==1) && it==s.owned.end()){s.owned.push_back({item});it=std::prev(s.owned.end());}
   if(l==0 && write_flag(item.data+36,1)){it->layered=true;++changed;}
   if(st==1 && write_flag(item.data+37,0))it->stationary=true;
  }
  if(transition)logging::log(logging::Level::info,logging::Channel::runtime,"Board gesture on-board item transition {:.3f} ms; cached={}",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-transition_start).count(),valid);
  if(changed)logging::log(logging::Level::info,logging::Channel::runtime,"Board gesture: automatic layered item pass applied.");
  s.detail="Automatic layered item pass: "+std::to_string(items.size())+" gestures; boardgesture 0 restores owned changes";
 }catch(...) {}
}
} // namespace dingosdk::board_gesture
