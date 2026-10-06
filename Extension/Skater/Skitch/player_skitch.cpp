#include "player_skitch.h"
#include "../client_source_spawn_internal.h"
#include "../no_bail.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Engine/Core/Platform/memory.h"
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
    // Grip-holds-the-bail option: while set and a grip is active, wipeout
    // requests are suppressed so the physical tow keeps working. Off by default.
    std::atomic<bool> grip_no_bail_setting{false}, grip_active{false};
    // Whole-body placement drag: the render thread eases into the drag and
    // fades it out on release so the game's own placement is not reasserted as
    // a teleport. Only the pose callback touches these.
    std::atomic<float> drag_blend{0};
    std::atomic<std::uint64_t> drag_time{};
    std::array<float,3> drag_goal{};
    std::uintptr_t fade_base{}, fade_component{};
    std::uint64_t fade_until{};
    bool fade_active{};
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
bool write_position(std::uintptr_t at, const std::array<float,3>& p) noexcept {
    __try { std::memcpy(reinterpret_cast<void*>(at), p.data(), sizeof(p)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Probe of the motion state that owns a wipeout. core+0x3b0 is the parent
// motion object and +0x48 selects the active embedded substate, the same shape
// as the walking flight states. On the first wipeout frame the probe dumps both
// objects and then logs the first change of every watched word, so the field
// the game actually moves the character with can be identified. Read-only.
struct Probe {
    std::atomic<bool> pending{}, active{};
    std::atomic<unsigned> seconds{60};
    bool captured{};
    ULONGLONG until{}, next_heartbeat{};
    std::uintptr_t core{}, motion{}, substate{};
    std::vector<std::uintptr_t> addresses;
    std::vector<std::uint32_t> values;
    std::vector<bool> changed;
    unsigned lines{}, changes{};
};
Probe& probe() { static auto* p = new Probe; return *p; }
void probe_dump(std::uintptr_t address, std::size_t bytes, const char* label) {
    for (std::size_t offset = 0; offset + 4 <= bytes; offset += 4) {
        std::uint32_t value{};
        if (!memory::peek(address + offset, value)) break;
        float as_float{};
        std::memcpy(&as_float, &value, sizeof(as_float));
        logging::log(logging::Level::info, logging::Channel::runtime,
            "Player skitch probe: {} +{:#04x} {:#010x} {} (float {:.6g})", label, offset, value, value, as_float);
    }
}
void probe_capture(std::uintptr_t core) {
    auto& p = probe();
    p.core = core;
    p.motion = p.substate = 0;
    (void)memory::peek(core + 0x3b0, p.motion);
    std::uintptr_t motion_vtable{}, substate_vtable{};
    if (p.motion) {
        (void)memory::peek(p.motion, motion_vtable);
        (void)memory::peek(p.motion + 0x48, p.substate);
    }
    if (p.substate) (void)memory::peek(p.substate, substate_vtable);
    logging::log(logging::Level::info, logging::Channel::runtime,
        "Player skitch probe: core={:#x} motion={:#x} vtable={:#x} substate={:#x} vtable={:#x}", core, p.motion,
        motion_vtable, p.substate, substate_vtable);
    if (p.motion) probe_dump(p.motion, 0x80, "motion");
    if (p.substate) probe_dump(p.substate, 0x100, "substate");
    p.addresses.clear();
    p.values.clear();
    p.changed.clear();
    const std::array<std::pair<std::uintptr_t, std::size_t>, 2> regions{{
        {p.motion, 0x80},
        {p.substate, 0x200},
    }};
    for (const auto& [base, bytes] : regions) {
        if (!base) continue;
        for (std::size_t offset = 0; offset < bytes; offset += 4) {
            std::uint32_t value{};
            if (!memory::peek(base + offset, value)) continue;
            p.addresses.push_back(base + offset);
            p.values.push_back(value);
            p.changed.push_back(false);
        }
    }
    p.captured = true;
    p.lines = p.changes = 0;
    logging::log(logging::Level::info, logging::Channel::runtime,
        "Player skitch probe: watching {} word(s) through this ragdoll.", p.addresses.size());
}
void probe_sample() {
    auto& p = probe();
    for (std::size_t i = 0; i < p.addresses.size(); ++i) {
        std::uint32_t value{};
        if (!memory::peek(p.addresses[i], value) || value == p.values[i]) continue;
        ++p.changes;
        if (!p.changed[i] && p.lines < 400) {
            p.changed[i] = true;
            ++p.lines;
            float before{}, after{};
            std::memcpy(&before, &p.values[i], sizeof(before));
            std::memcpy(&after, &value, sizeof(after));
            logging::log(logging::Level::info, logging::Channel::runtime,
                "Player skitch probe: {:#x} {:#010x} -> {:#010x} (float {:.6g} -> {:.6g})", p.addresses[i],
                p.values[i], value, before, after);
        }
        p.values[i] = value;
    }
}
void probe_tick(std::uintptr_t core, bool wipeout) {
    auto& p = probe();
    const auto now = GetTickCount64();
    if (p.pending.exchange(false)) {
        const auto window = p.seconds.load() ? p.seconds.load() : 60u;
        p.active.store(true);
        p.captured = false;
        p.until = now + window * 1000ULL;
        p.next_heartbeat = now + 5000;
        logging::log(logging::Level::info, logging::Channel::runtime,
            "Player skitch probe: armed for {}s; bail while holding the grab (or just bail).", window);
    }
    if (!p.active.load()) return;
    if (now >= p.until) {
        p.active.store(false);
        logging::log(logging::Level::info, logging::Channel::runtime,
            "Player skitch probe over: {} change(s), {} line(s).", p.changes, p.lines);
        return;
    }
    if (!wipeout) return;
    if (!p.captured) {
        probe_capture(core);
    } else {
        std::uintptr_t motion{}, active{};
        if (memory::peek(core + 0x3b0, motion) &&
            (motion != p.motion || (memory::peek(motion + 0x48, active) && active != p.substate))) {
            logging::log(logging::Level::info, logging::Channel::runtime,
                "Player skitch probe: the active motion state changed; recapturing.");
            probe_capture(core);
        }
    }
    probe_sample();
    if (now >= p.next_heartbeat) {
        p.next_heartbeat = now + 5000;
        unsigned shown{};
        for (std::size_t i = 0; i < p.addresses.size() && shown < 16; ++i) {
            if (!p.changed[i]) continue;
            float as_float{};
            std::memcpy(&as_float, &p.values[i], sizeof(as_float));
            logging::log(logging::Level::info, logging::Channel::runtime,
                "Player skitch probe: now {:#x} {:#010x} (float {:.6g})", p.addresses[i], p.values[i], as_float);
            ++shown;
        }
    }
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
void set_grip_no_bail(bool on) noexcept { state().grip_no_bail_setting.store(on); }
bool grip_no_bail() noexcept { return state().grip_no_bail_setting.load(); }
bool grip_holding() noexcept { return state().grip_active.load(); }
void request_probe(unsigned seconds) noexcept {
    auto &p = probe();
    p.seconds.store(seconds ? seconds : 60);
    p.pending.store(true);
}
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
        if(probe().pending.load(std::memory_order_relaxed)||probe().active.load(std::memory_order_relaxed))
            probe_tick(bodies.core,wipeout||bailing);
        const bool was_attached=s.tow.attached();
        s.steering=steering;
        const auto plan=s.tow.update(world,local_id,now,bodies.root,playable,ragdoll,held,candidates,steering,was_attached && s.hand_side.load()==1);
        s.detail=std::string(s.tow.status());
        s.grip_active.store(plan.has_value());
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
// Whole-body drag placement. The wipeout solver recomputes the ragdoll from
// its own source every update (the state fields are outputs; Falling,
// FollowRagdoll and FollowAnimatedRagdoll all read the same context source),
// so no memory write survives there. The final skeleton response does apply
// this pose, so joint 1 -- the world placement -- is eased to the follow slot
// while the grip holds, and faded back to the game's own placement after
// release instead of snapping. Runs from the pose callback, where the write
// survives to the renderer; the fade path keeps it alive after the request
// ends (the callback is the only place this write is seen).
void drag_placement(std::uintptr_t base, std::uintptr_t component, bool active,
    const skateskitch::Vec3* goal) noexcept {
    auto& s=state();
    const auto now_ms=GetTickCount64();
    const auto previous_ms=s.drag_time.exchange(now_ms);
    const float dt=previous_ms && now_ms>previous_ms
        ? std::min(0.1f,static_cast<float>(now_ms-previous_ms)*0.001f) : 1.f/60.f;
    float blend=s.drag_blend.load();
    if(active && goal) { s.drag_goal=*goal; blend=std::min(1.f,blend+dt*5.f); }
    else blend=std::max(0.f,blend-dt*2.5f);
    s.drag_blend.store(blend);
    if(blend<=0.001f) return;
    std::uintptr_t holder{};
    if(!first_person_read(component+0xa0,&holder,sizeof(holder)) || !holder) return;
    const auto pose=multiplayer::read_native_pose_layout(first_person_read,base,holder,512);
    if(!pose.buffer || pose.count!=395) return;
    const auto placement=pose.buffer+0x30ULL+0x20;
    std::array<float,3> current{};
    if(!first_person_read(placement,current.data(),sizeof(current))) return;
    if(!source_writable(placement,12)) { if(active) s.hand_detail.store("placement not writable"); return; }
    const auto& target=s.drag_goal;
    const std::array<float,3> eased{
        current[0]+(target[0]-current[0])*blend,
        current[1]+(target[1]-current[1])*blend,
        current[2]+(target[2]-current[2])*blend};
    if(!write_position(placement,eased) && active) fault();
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
        drag_placement(r->base,component,r->plan.ragdoll,&r->plan.root_goal);
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
    auto& s=state();
    const auto r=request();
    if(!r) {
        // The release fade has to outlive the request: without a request the
        // callback is not invoked, and this is the only place the placement
        // write is seen by the renderer. Keep running it until the blend ends.
        if(!s.fade_active || GetTickCount64()>s.fade_until || !s.fade_component) return;
        std::uintptr_t holder{};
        if(!first_person_read(s.fade_component+0xa0,&holder,sizeof(holder)) || !holder ||
            animation_interface!=holder+0xc0) return;
        drag_placement(s.fade_base,s.fade_component,false,nullptr);
        if(s.drag_blend.load()<=0.001f) s.fade_active=false;
        return;
    }
    s.fade_active=true; s.fade_until=GetTickCount64()+4000;
    s.fade_base=r->base; s.fade_component=r->component;
    std::uintptr_t holder{};
    if(first_person_read(r->component+0xa0,&holder,sizeof(holder)) && holder && animation_interface==holder+0xc0)
        animation_evaluated(r->component);
}
} // namespace dingosdk::player_skitch
