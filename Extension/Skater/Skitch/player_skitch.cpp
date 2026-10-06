#include "player_skitch.h"
#include "../client_source_spawn_internal.h"
#include "../no_bail.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Engine/Core/Log/logging.h"
#include <atomic>

extern "C" void DingoSDKOverlayReadSkitchInput(bool*, bool*, float*);
namespace dingosdk::player_skitch {
using namespace client_source::detail;
namespace {
struct State {
    std::mutex mutex;
    skateskitch::TowController tow;
    std::optional<Request> pending;
    std::atomic<bool> on{true}, failed{};
    std::atomic<std::uint64_t> physics_steps{}, hand_updates{},turn_steps{},hand_attempts{};
    std::atomic<int> hand_side{-1}; // selection hysteresis avoids swapping on small pose jitter
    std::atomic<const char*> hand_detail{"waiting for grab"};
    std::atomic<const char*> failure_reason{"Skitch physics changed; release grab and try again"};
    float steering{};
    std::uintptr_t owner{};
    std::uint64_t next_report{};
    bool ragdoll_active{};
    std::string detail = "Hold V or LB+RB near a player";
};
State& state() { static State s; return s; }
bool write_rotation(std::uintptr_t at, const std::array<float,4>& q) noexcept {
    __try { std::memcpy(reinterpret_cast<void*>(at), q.data(), sizeof(q)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}
bool enabled() noexcept { return state().on.load(); }
void set_enabled(bool on) noexcept { state().on.store(on); if(!on) suspend(); }
std::string status() { auto& s=state(); std::lock_guard lock(s.mutex); return s.detail+
    "; steering="+std::to_string(s.steering)+"; physics steps="+std::to_string(s.physics_steps.load())+"; turn steps="+std::to_string(s.turn_steps.load())+
    "; hand updates="+std::to_string(s.hand_updates.load())+"/"+std::to_string(s.hand_attempts.load())+
    " ("+s.hand_detail.load()+")"; }
void suspend() noexcept {
    auto& s=state(); std::lock_guard lock(s.mutex);
    s.pending.reset(); s.tow.release("Session unavailable; released"); s.detail=std::string(s.tow.status());
}
void fault(const char* reason) noexcept { state().failure_reason.store(reason); state().failed.store(true); }
void note_physics_step() noexcept { state().physics_steps.fetch_add(1); }
void note_turn_step() noexcept { state().turn_steps.fetch_add(1); }
std::optional<Request> request() noexcept {
    auto& s=state(); std::lock_guard lock(s.mutex);
    if(!s.on.load() || s.failed.load() || !s.pending || GetTickCount64()>=s.pending->expires) return {};
    return s.pending;
}
void tick(std::uintptr_t base,std::uintptr_t client,const multiplayer::NativeFrame& local,
    skateskitch::WorldKey world,std::uint64_t local_id,std::uint64_t now,
    std::span<const skateskitch::TowCandidate> candidates) {
    bool active=false, held=false; float steering=0; DingoSDKOverlayReadSkitchInput(&active,&held,&steering);
    active = active && !multiplayer::sample_game_ui_state(base).in_menu;
    auto& native=source_state();
    auto& s=state();
    std::lock_guard lock(s.mutex);
    if(s.pending && GetTickCount64()>=s.pending->expires) s.tow.release("Input expired; release grab to rearm");
    if(s.owner && s.owner!=local.entity) s.tow.release("Local skater replaced; release grab to rearm");
    s.owner=local.entity;
    s.pending.reset();
    if(s.failed.exchange(false)) {
        s.tow.release(s.failure_reason.load());
        logging::log(logging::Level::warning,logging::Channel::runtime,"Player skitch guard: {}",s.failure_reason.load());
    }
    if(!s.on.load() || !local.ready || !native.initialized.load() || !native.velocity_guard_active.load()) {
        s.tow.release("Skitch unavailable; release grab to rearm"); s.detail=std::string(s.tow.status()); return;
    }
    if(native.busy.test_and_set(std::memory_order_acquire)) return;
    SourceBusyScope scope{native.busy};
    try {
        const auto bodies=debug_noclip_bodies(base,client,local.entity);
        SourceReader reader;
        const auto component=reader.pointer(local.entity,0x628);
        const bool impact_bail=(reader.value<std::uint32_t>(bodies.context,0x13c4)&0x8000u)!=0;
        const bool animation_bail=(reader.value<std::uint32_t>(bodies.context,0x13d4)&0x08000000u)!=0;
        const bool bailing=impact_bail||animation_bail;
        // The selector's own state is the reliable ragdoll signal (300 is a
        // ground wipeout); the request bits cover the step before it switches.
        const bool wipeout=dingosdk::observed_physics_state()==addr::no_bail::wipeout_physics_state;
        const bool playable=active && !bodies.offboard && !bailing && !native.trial.debug.noclip &&
            !native.trial.debug.park_editor && !native.trial.debug.camera_owned &&
            reader.value<std::uint8_t>(local.entity,0x7e0)==0;
        // Ragdoll: a grip is kept through a bail, and a floored skater can also
        // reach for a nearby player and grab.
        const bool ragdoll=active && (wipeout||bailing);
        reader.verify();
        const bool was_attached=s.tow.attached();
        s.steering=steering;
        const auto plan=s.tow.update(world,local_id,now,bodies.root,playable,ragdoll,held,candidates,steering,was_attached && s.hand_side.load()==1);
        s.detail=std::string(s.tow.status());
        const bool dragging=plan && plan->ragdoll;
        if(dragging!=s.ragdoll_active) {
            s.ragdoll_active=dragging;
            if(dragging)
                logging::log(logging::Level::info,logging::Channel::runtime,
                    "Player skitch: bail drag engaged; hold the grab to keep it.");
            else
                logging::log(logging::Level::info,logging::Channel::runtime,
                    "Player skitch: bail drag ended.");
        }
        if(!plan || !was_attached) s.hand_side.store(-1);
        if(plan) s.pending=Request{base,client,local.entity,bodies.core,component,GetTickCount64()+150,*plan};
        if((plan && (!was_attached || now>=s.next_report)) || (was_attached && !plan)) {
            logging::log(logging::Level::info,logging::Channel::runtime,
                "Player skitch: {}; steering={:.2f}, physics={}, turns={}, hand={}/{} ({}).",s.detail,s.steering,
                s.physics_steps.load(),s.turn_steps.load(),s.hand_updates.load(),s.hand_attempts.load(),s.hand_detail.load());
            s.next_report=now+5000000;
        }
    } catch(...) {
        s.tow.release("Local skater changed; release grab to rearm"); s.detail=std::string(s.tow.status());
    }
}
void animation_evaluated(std::uintptr_t component) noexcept {
    const auto r=request(); if(!r || r->component!=component) return;
    state().hand_attempts.fetch_add(1);
    try {
        SourceReader reader;
        if(reader.pointer(r->entity,0x628)!=component || reader.pointer(component)!=r->base+addr::engine::skater_component_vtable)
            { fault(); return; }
        const auto holder=reader.pointer(component,0xa0);
        const auto pose=multiplayer::read_native_pose_layout(first_person_read,r->base,holder,512);
        if(!pose.buffer || pose.count!=395) { state().hand_detail.store("pose unavailable"); return; }
        std::array<skateskitch::Joint,395> joints;
        for(const auto i : {0u,1u,7u,42u,43u,44u,45u,46u,47u,48u,49u,50u,275u,276u,277u,278u,283u}) {
            std::array<float,12> bone{};
            if(!first_person_read(pose.buffer+i*0x30ULL,bone.data(),sizeof(bone))) { state().hand_detail.store("joint unreadable"); return; }
            joints[i]={{bone[8],bone[9],bone[10]},{bone[4],bone[5],bone[6],bone[7]},{bone[0],bone[1],bone[2]}};
        }
        const int previous_side=state().hand_side.load();
        const auto nearest=skateskitch::stance_hand(joints,r->plan.heading,
            previous_side<0 ? std::optional<bool>{} : std::optional<bool>{previous_side!=0});
        if(!nearest) { state().hand_detail.store("arm geometry unavailable"); return; }
        const int side=*nearest ? 1 : 0; state().hand_side.store(side);
        const unsigned arm=side ? 276u : 47u, forearm=side ? 277u : 48u;
        if(!skateskitch::reach_hand(joints,r->plan.hand_goal,side!=0)) { state().hand_detail.store("arm geometry rejected"); return; }
        const auto upper=pose.buffer+arm*0x30ULL+0x10, lower=pose.buffer+forearm*0x30ULL+0x10;
        if(!source_writable(upper,16)||!source_writable(lower,16)) { state().hand_detail.store("pose not writable"); return; }
        reader.verify();
        if(!write_rotation(upper,joints[arm].rotation)||!write_rotation(lower,joints[forearm].rotation)) fault();
        else { state().hand_updates.fetch_add(1); state().hand_detail.store(side ? "left hand reach applied" : "right hand reach applied"); }
    } catch(...) { state().hand_detail.store("ownership or layout changed"); fault(); }
}
void render_pose(std::uintptr_t animation_interface) noexcept {
    const auto r=request(); if(!r) return;
    std::uintptr_t holder{};
    if(first_person_read(r->component+0xa0,&holder,sizeof(holder)) && holder && animation_interface==holder+0xc0)
        animation_evaluated(r->component);
}
} // namespace dingosdk::player_skitch
