#include "prop_attach.h"
#include "Gestures/board_gesture_layout.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Extension/Objects/prop_hand_runtime.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/skater_entities.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace dingosdk::skater {
namespace {
using Ptr = std::uintptr_t;
namespace engine = game::build::v20260929::engine;
namespace entities = addr::skater_entities;

// Watch every client frame: a gesture request may be written and consumed
// inside one frame, which a five-per-second sample would simply miss. The
// change log keeps only small-integer transitions -- a gesture id is 0..254 --
// because context floats otherwise drown everything.
constexpr unsigned item_scan_bytes = 0x60;  // each gesture item's interesting head
constexpr unsigned max_watch_items = 64;    // sampled items, not a hard limit on discovery
constexpr unsigned max_changes = 12000;     // the log is a finding, not a trace
constexpr unsigned default_seconds = 60;
constexpr unsigned idle_seconds = 10;       // phase 0 length before the auto mark

struct Item {
    std::string name;
    std::uint32_t hash{};
    Ptr asset{}, data{};
    std::int32_t id{-1};
    std::uint8_t layered{}, stationary{};
};
struct Local {
    Ptr context{}, manager{}, player{}, entity{}, component{}, holder{}, rig{}, definition{};
};
struct Region {
    std::string name;
    Ptr address{};
    bool asset_image{};   // an asset's own bytes: +0x28 is its runtime data pointer
};
struct Word {
    Ptr address{};
    std::uint32_t value{};
    unsigned region{};
    // First change per address is logged, tagged with the phase it happened in:
    // the animation graph churns hundreds of flags while idle, so the finding is
    // the set difference between phases, not any single change.
    bool logged{};
    unsigned phase{};
};
struct State {
    std::mutex mutex;
    std::string status{"Hand props: nothing requested yet."};
    bool report_pending{}, watch_pending{};
    std::string filter;
    unsigned watch_seconds{default_seconds};
    // Live watch: touched only from the client tick.
    bool watching{};
    ULONGLONG watch_until{};
    ULONGLONG watch_start{};
    ULONGLONG next_sample{};
    ULONGLONG next_heartbeat{};
    unsigned repeats{};
    unsigned phase{};
    bool log_every{};   // narrow watches keep the whole timeline, not just the first change
    bool mark_pending{};
    bool weight_pending{};
    unsigned weight_seconds{45};
    bool pose_pending{};
    unsigned pose_seconds{45};
    bool pose_mode{};   // pose-buffer watch: joints and fields, not raw words
    bool assets_pending{};
    std::string assets_extra;
    unsigned assets_seconds{45};
    bool poke_pending{};
    bool poke_stop_pending{};
    unsigned poke_seconds{12};
    // A write test: three layer weights, saved and restored.
    bool poking{};
    ULONGLONG poke_until{};
    std::array<Ptr, 3> poke_addresses{};
    std::array<std::uint32_t, 3> poke_old{};
    unsigned poke_overwrites{}, poke_rewrites{};
    Ptr poke_rig{};
    // The passive creation trace: hook the entity factory for a short window and
    // keep blueprint, entity, thread and caller stack for every creation.
    bool trace_pending{}, trace_off_pending{};
    unsigned trace_seconds{90};
    // The phone-blueprint resolve scan: the asset may only be resident while the
    // phone is out, so check every second instead of only when the command runs.
    bool derive_pending{};
    unsigned derive_seconds{90};
    bool deriving{};
    ULONGLONG derive_until{}, derive_next{};
    bool derive_seen{};
    // Hand-prop follow: take a placed object and hold it at the wrist. The
    // request goes to the Objects runtime; here we only compose the wrist and
    // feed it the target every client tick.
    bool hand_attach_pending{}, hand_detach_pending{};
    std::string hand_name;
    bool hand_left{};
    bool hand_feeding{};
    std::uint16_t hand_joint{278};   // right wrist j278; j049 is the left
    std::uintptr_t hand_definition{};
    std::vector<std::uint16_t> hand_chain;
    // Character morph clamp: the DingoMorph data mapping authors Min/Max for
    // every input (0..1 for all body regions). This tool rewrites the live
    // MaxValue floats and can put the originals back.
    bool morph_find_pending{}, morph_unclamp_pending{}, morph_clamp_pending{};
    float morph_unclamp_max{4.0f};
    std::vector<std::pair<Ptr, float>> morph_originals;
    std::vector<Region> regions;
    std::vector<Word> words;
    std::vector<std::pair<std::string, Ptr>> pending_regions;
    unsigned changes{}, lines{};
};
State &state() {
    static auto *value = new State;
    return *value;
}
std::mutex &status_mutex() {
    static std::mutex mutex;
    return mutex;
}
void set_status(const std::string &text) {
    std::lock_guard lock(status_mutex());
    state().status = text;
}
Ptr pointer(Ptr address) noexcept {
    Ptr value{};
    memory::peek(address, value);
    return value;
}

// The creation trace. The native attachment owner cannot be found by scanning
// resident state, so the game's own factory is watched instead: while a bounded
// window is armed, every create_entity call records its blueprint, the returned
// entity, the calling thread and a module-relative caller stack. The hook does
// no logging and no allocation -- records go into a fixed ring and are drained
// by the client tick -- and it always forwards the original ABI untouched.
struct TraceRecord {
    Ptr blueprint{}, entity{}, type{};
    std::uint32_t thread{};
    unsigned frames{};
    void *stack[16]{};
    char name[128]{};
};
constexpr unsigned trace_capacity = 256;
constexpr unsigned trace_frames = 14;
struct Trace {
    std::mutex mutex;   // the hook only ever try_locks; the tick drain locks
    std::array<TraceRecord, trace_capacity> records{};
    unsigned write{}, read{};
    std::atomic<unsigned> dropped{};
    std::atomic<bool> armed{};
    ULONGLONG until{};
    void *target{};
    void *original{};
    bool prepared{}, installed{};
};
Trace &trace() {
    static auto *value = new Trace;
    return *value;
}

void *create_trace_hook(void *result, void *descriptor, std::uintptr_t blueprint, std::uintptr_t first,
                        std::uintptr_t second) {
    auto &t = trace();
    using Create = void *(*)(void *, void *, std::uintptr_t, std::uintptr_t, std::uintptr_t);
    auto *const value = reinterpret_cast<Create>(t.original)(result, descriptor, blueprint, first, second);
    if (!t.armed.load(std::memory_order_relaxed)) return value;
    TraceRecord record{};
    record.blueprint = blueprint;
    if (result) record.entity = *static_cast<std::uintptr_t *>(result);
    if (blueprint) {
        memory::peek(blueprint + 8, record.type);
        Ptr text{};
        // A failed name lookup must not erase the evidence: the blueprint
        // pointer, its type and the stack are recorded either way.
        if (memory::peek(blueprint + 0x18, text) && text) (void)memory::peek_cstring(text, record.name, sizeof(record.name));
    }
    record.thread = GetCurrentThreadId();
    // Frame 0 is this hook's caller -- the game call site into create_entity.
    record.frames = static_cast<unsigned>(CaptureStackBackTrace(1, trace_frames, record.stack, nullptr));
    if (t.mutex.try_lock()) {
        t.records[t.write % trace_capacity] = record;
        ++t.write;
        t.mutex.unlock();
    } else {
        t.dropped.fetch_add(1, std::memory_order_relaxed);
    }
    return value;
}

void log_trace_record(Ptr base, const TraceRecord &record) {
    std::string type;
    if (record.type) {
        const bool in_game = base && record.type >= base && record.type - base < supported_build::game_image_size;
        type = in_game ? std::format(" type=Skate+{:#x}", record.type - base)
                       : std::format(" type={:#x}", record.type);
    }
    logging::log(logging::Level::info, logging::Channel::skater,
                 "Hand props: trace: entity={:#x} blueprint={:#x}{} name=\"{}\" thread={}", record.entity,
                 record.blueprint, type, record.name, record.thread);
    std::string frames;
    for (unsigned i = 0; i < record.frames; ++i) {
        const auto address = reinterpret_cast<Ptr>(record.stack[i]);
        if (base && address >= base && address - base < supported_build::game_image_size)
            frames += std::format(" Skate+{:#x}", address - base);
        else
            frames += std::format(" {:#x}", address);
    }
    logging::log(logging::Level::info, logging::Channel::skater, "Hand props: trace:   callers:{}", frames);
}

void arm_trace(Ptr base, unsigned seconds) {
    auto &t = trace();
    auto *const target = reinterpret_cast<void *>(base + entities::create_entity);
    if (!t.prepared) {
        // Hook the exact function the build pins, after checking its bytes.
        std::array<unsigned char, entities::create_entity_prefix.size()> actual{};
        if (!memory::read_bytes(base + entities::create_entity, actual.data(), actual.size()) ||
            actual != entities::create_entity_prefix) {
            logging::log(logging::Level::warning, logging::Channel::skater,
                         "Hand props: create_entity fingerprint changed; the trace was not installed.");
            set_status("Hand props: create_entity fingerprint changed; trace not installed.");
            return;
        }
        void *original{};
        const auto status = hook_prepare(target, reinterpret_cast<void *>(&create_trace_hook), &original);
        if (status != HookOk || !original) {
            logging::log(logging::Level::warning, logging::Channel::skater,
                         "Hand props: cannot prepare the creation trace ({}).", hook_status_string(status));
            set_status(std::format("Hand props: cannot prepare the creation trace ({}).",
                                   hook_status_string(status)));
            return;
        }
        t.target = target;
        t.original = original;
        t.prepared = true;
    }
    if (!t.installed) {
        const auto queued = hook_queue_enable(t.target);
        if (queued != HookOk || hook_apply_queued() != HookOk) {
            if (queued == HookOk) (void)hook_disable(t.target);
            logging::log(logging::Level::warning, logging::Channel::skater,
                         "Hand props: cannot enable the creation trace ({}).", hook_status_string(queued));
            set_status(std::format("Hand props: cannot enable the creation trace ({}).",
                                   hook_status_string(queued)));
            return;
        }
        t.installed = true;
    }
    const auto window = seconds ? seconds : 90u;
    {
        std::lock_guard lock(t.mutex);
        t.write = t.read = 0;
        t.until = GetTickCount64() + window * 1000ULL;
    }
    t.dropped.store(0, std::memory_order_relaxed);
    t.armed.store(true, std::memory_order_release);
    logging::log(logging::Level::info, logging::Channel::skater,
                 "Hand props: creation trace armed for {}s (create_entity is hooked; every creation is captured "
                 "with its caller stack).", window);
    set_status(std::format("Hand props: creation trace armed for {}s.", window));
}

void disarm_trace() {
    auto &t = trace();
    if (!t.armed.exchange(false)) {
        set_status("Hand props: the creation trace is not armed.");
        return;
    }
    logging::log(logging::Level::info, logging::Channel::skater,
                 "Hand props: creation trace disarmed; captured records keep draining to the log.");
    set_status("Hand props: creation trace disarmed.");
}

void drain_trace(Ptr base) {
    auto &t = trace();
    const auto now = GetTickCount64();
    bool finished{};
    if (t.armed.load(std::memory_order_acquire) && now >= t.until) {
        t.armed.store(false, std::memory_order_release);
        finished = true;
    }
    std::array<TraceRecord, 12> batch{};
    std::size_t count{};
    unsigned pending{};
    {
        std::lock_guard lock(t.mutex);
        pending = t.write - t.read;
        while (count < batch.size() && t.read != t.write) {
            batch[count++] = t.records[t.read % trace_capacity];
            ++t.read;
        }
    }
    for (std::size_t i = 0; i < count; ++i) log_trace_record(base, batch[i]);
    if (finished) {
        const auto dropped = t.dropped.load(std::memory_order_relaxed);
        logging::log(logging::Level::info, logging::Channel::skater,
                     "Hand props: creation trace window over: {} record(s) captured{}.", pending,
                     dropped ? std::format(", {} dropped (ring full)", dropped) : "");
        set_status(std::format("Hand props: creation trace over; {} record(s) in the log.", pending));
    }
}

// A gesture item, the way the board gesture pass reads it: cosmetics manager
// hash table -> node {hash, asset, next} -> asset {key +0x38, hash +0x48,
// data +0x28} -> data {vtable, type, game state +0x18, id +0x20, layered +0x24,
// stationary +0x25}. `filter` only decides which loaded items are interesting.
bool item_from_asset(Ptr base, Ptr asset, std::uint32_t hash, const std::string &filter, Item &out) {
    Ptr key{}, data{}, vtable{}, type{};
    std::uint32_t stored{};
    std::array<char, 128> text{};
    if (!memory::peek(asset + 0x38, key) || !memory::peek(asset + 0x48, stored) || stored != hash) return false;
    if (memory::peek_cstring(key, text.data(), text.size()) < 0 || text[0] == '\0') return false;
    std::string name(text.data());
    if (!name.starts_with("own_rctn_gesture_all_")) return false;
    if (!filter.empty() && name.find(filter) == std::string::npos) return false;
    if (!memory::peek(asset + 0x28, data) || !(data &= ~Ptr{4})) return false;
    if (!memory::peek(data, vtable) || vtable != base + board_gesture::layout::item_vtable) return false;
    if (!memory::peek(data + 8, type) || type != base + board_gesture::layout::item_type) return false;
    out.name = std::move(name);
    out.hash = hash;
    out.asset = asset;
    out.data = data;
    memory::peek(data + 0x20, out.id);
    memory::peek(data + 0x24, out.layered);
    memory::peek(data + 0x25, out.stationary);
    return true;
}
std::vector<Item> discover(Ptr base, const std::string &filter) {
    std::vector<Item> result;
    Ptr manager{}, buckets{};
    std::uint32_t bucket_count{}, item_count{};
    std::uint8_t ready{};
    if (!memory::peek(base + engine::cosmetics_manager, manager) || !manager) return result;
    Ptr vtable{};
    if (!memory::peek(manager, vtable) || vtable != base + engine::cosmetics_manager_vtable) return result;
    if (!memory::peek(manager + 0xa8, ready) || ready != 1) return result;
    if (!memory::peek(manager + 0x30, buckets) || !buckets) return result;
    if (!memory::peek(manager + 0x38, bucket_count) || !bucket_count || bucket_count > 16384) return result;
    if (!memory::peek(manager + 0x3c, item_count) || !item_count || item_count > 8192) return result;
    std::uint32_t visited = 0;
    for (std::uint32_t bucket = 0; bucket < bucket_count; ++bucket) {
        Ptr node{};
        if (!memory::peek(buckets + std::uintptr_t{bucket} * 8, node)) return result;
        while (node) {
            if (++visited > item_count) return result;
            std::uint32_t hash{};
            Ptr asset{}, next{};
            if (!memory::peek(node, hash) || !memory::peek(node + 8, asset) || !memory::peek(node + 0x10, next)) return result;
            Item item;
            if (item_from_asset(base, asset & ~Ptr{4}, hash, filter, item)) result.push_back(std::move(item));
            node = next;
        }
    }
    return result;
}

bool resolve_local(Ptr base, Ptr client, Local &out) {
    std::uint32_t offset{};
    if (!client || pointer(client) != base + engine::client_vtable) return false;
    const auto context = pointer(client + 8);
    if (!context || !memory::peek(base + engine::context_player_manager_offset, offset) || offset > 0x1000000)
        return false;
    out.context = context;
    const auto manager = pointer(context + offset);
    if (!manager || pointer(manager) != base + engine::local_player_manager_vtable) return false;
    out.manager = manager;
    const auto begin = pointer(manager + 0x4c8), end = pointer(manager + 0x4d0);
    if (!begin || end != begin + 8) return false;
    out.player = pointer(begin);
    if (!out.player || pointer(out.player) != base + engine::local_player_vtable) return false;
    out.entity = pointer(out.player + 0xb8);
    if (!out.entity || pointer(out.entity) != base + engine::skater_entity_vtable) return false;
    out.component = pointer(out.entity + 0x628);
    if (!out.component || pointer(out.component) != base + engine::skater_component_vtable) return false;
    out.holder = pointer(out.component + 0xa0);
    out.rig = out.holder ? pointer(out.holder + 0x78) : 0;
    out.definition = out.rig ? pointer(out.rig + 0x18) : 0;
    return true;
}

void add_region(State &s, std::string name, Ptr address, std::size_t bytes, bool asset_image = false) {
    if (!address) return;
    s.regions.push_back({std::move(name), address, asset_image});
    const auto region = static_cast<unsigned>(s.regions.size() - 1);
    for (std::size_t offset = 0; offset + 4 <= bytes; offset += 4) {
        std::uint32_t value{};
        if (!memory::peek(address + offset, value)) continue;
        s.words.push_back({address + offset, value, region});
    }
}

void report(Ptr base, const std::string &filter) {
    const auto items = discover(base, filter);
    logging::log(logging::Level::info, logging::Channel::skater,
                 "Hand props: {} gesture item(s) loaded{}.", items.size(),
                 filter.empty() ? "" : " matching \"" + filter + "\"");
    for (const auto &item : items) {
        logging::log(logging::Level::info, logging::Channel::skater,
                     "Hand props: {} id={} layered={} stationary={} data={:#x} hash={}",
                     item.name, item.id, item.layered, item.stationary, item.data, item.hash);
    }
    set_status(std::format("Hand props: {} gesture item(s){}; ids in the log.", items.size(),
                           filter.empty() ? "" : " matching \"" + filter + "\""));
}

void arm(Ptr base, Ptr client, const std::string &filter, unsigned seconds) {
    auto &s = state();
    Local local;
    if (!resolve_local(base, client, local)) {
        set_status("Hand props: the local skater is not ready to watch.");
        return;
    }
    s.regions.clear();
    s.words.clear();
    s.changes = s.lines = 0;
    const auto items = discover(base, filter);
    for (const auto &item : items)
        add_region(s, "item " + item.name.substr(std::string("own_rctn_gesture_all_").size()), item.data,
                   item_scan_bytes);
    add_region(s, "context", local.context, 0x100);
    add_region(s, "manager", local.manager, 0x100);
    add_region(s, "player", local.player, 0x200);
    add_region(s, "skater", local.entity, 0x120);
    add_region(s, "anim", local.component, 0x120);
    add_region(s, "holder", local.holder, 0x80);
    // The whole animation instance: the graph's state and its churning flags
    // live in the rig, far past the fields the pose resolver uses.
    add_region(s, "rig", local.rig, 0x4000);
    add_region(s, "definition", local.definition, 0x200);
    // The skater's other components: a gesture request is as likely to live in
    // an emote or expression component as in the animation one.
    const auto collection = pointer(local.component + 0x70);
    std::uint8_t count{};
    if (collection && memory::peek(collection + 8, count) && pointer(collection) == local.entity) {
        for (unsigned i = 0; i < count && i < 24; ++i) {
            const auto component = pointer(collection + 0x20 + std::uintptr_t{i} * 0x20);
            if (!component) continue;
            add_region(s, std::format("comp[{}]", i), component, 0x80);
        }
    }
    if (s.words.empty()) {
        set_status("Hand props: no state to watch yet; load a map first.");
        return;
    }
    const auto window = seconds ? seconds : default_seconds;
    s.watching = true;
    s.watch_start = GetTickCount64();
    s.watch_until = s.watch_start + window * 1000ULL;
    s.phase = 0;
    s.next_sample = 0;
    s.next_heartbeat = s.watch_start + 5000;
    logging::log(logging::Level::info, logging::Channel::skater,
                 "Hand props: watching {} words over {} region(s) for {}s{}. Idle for {}s, then phase1 begins and "
                 "you press the gesture.",
                 s.words.size(), s.regions.size(), window, filter.empty() ? "" : " (filter \"" + filter + "\")",
                 idle_seconds);
    set_status(std::format("Hand props: watching {} words for {}s; idle {}s, then press the gesture.",
                           s.words.size(), window, idle_seconds));
}

void arm_weight(Ptr base, Ptr client, unsigned seconds) {
    auto &s = state();
    Local local;
    if (!resolve_local(base, client, local)) {
        set_status("Hand props: the local skater is not ready to watch.");
        return;
    }
    const auto window = seconds ? seconds : s.weight_seconds;
    s.regions.clear();
    s.words.clear();
    s.changes = s.lines = s.repeats = 0;
    s.phase = 1;
    s.log_every = true;
    // The whole animation instance plus its holders. The narrowed slice came
    // back silent on a session where the phone demonstrably played, so the
    // changing state is somewhere else in the rig and the pointer chain has to
    // be on record to compare sessions.
    add_region(s, "rig", local.rig, 0x4000);
    add_region(s, "holder", local.holder, 0x400);
    add_region(s, "anim", local.component, 0x200);
    logging::log(logging::Level::info, logging::Channel::skater,
                 "Hand props: chain component={:#x} holder={:#x} rig={:#x} definition={:#x}.", local.component,
                 local.holder, local.rig, local.definition);
    if (s.words.empty()) {
        set_status("Hand props: no rig state to watch yet; load a map first.");
        return;
    }
    s.watching = true;
    s.watch_start = GetTickCount64();
    s.watch_until = s.watch_start + window * 1000ULL;
    s.next_heartbeat = s.watch_start + 5000;
    logging::log(logging::Level::info, logging::Channel::skater,
                 "Hand props: watching the animation instance ({} words: rig 16 KB, holder, anim component) for "
                 "{}s, every change. Press the gesture a few times and let each one finish.",
                 s.words.size(), window);
    set_status(std::format("Hand props: watching the layer weights for {}s; press the gesture a few times.", window));
}

// The engine's asset lookup is find-only, but it does answer for any loaded
// asset by name; this is the same domain sweep the effect prototype uses.
Ptr find_named_asset(Ptr base, const char *name, bool &faulted) noexcept {
    __try {
        const auto find = game::native_data().find_asset;
        if (!find) return 0;
        for (std::uint16_t domain = 0; domain < 0xbbf; ++domain) {
            Ptr owner{};
            if (!memory::read_bytes(base + engine::domain_owners + std::uintptr_t{domain} * 8, &owner, 8) || !owner)
                continue;
            if (const auto asset = find(domain, name)) return asset;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
    }
    return 0;
}

// A graph parameter's live value is not in its asset: the animation instance
// holds a reference to the asset, and the value lives beside it. Find those
// references in the instance, the holder, the animation component and the
// definition, then watch the words after each one so a gesture press can be read
// off against an idle phase.
// One-off dump: the values we are hunting are small integers or floats, so show
// both readings for every word.

// The entity factory's type contract, the same walk ai_skaters uses before it
// creates anything: object+8 is the object's TypeInfo, and +0x20 chains to the
// base types. The phone is a plain mesh ObjectBlueprint, so unlike the build-kit
// ECS prefab it may pass this check -- which is the precondition for ever
// handing it to create_entity.
Ptr object_type(Ptr object) noexcept {
    Ptr type{};
    memory::peek(object + 8, type);
    return type;
}
bool derives(Ptr object, Ptr expected) {
    auto type = object_type(object);
    for (int depth = 0; depth < 16 && type; ++depth) {
        if (type == expected) return true;
        Ptr next{};
        if (!memory::peek(type + 0x20, next)) return false;
        type = next;
    }
    return false;
}

void report_derive(Ptr base, const char *queried, Ptr asset) {
    std::array<char, 160> text{};
    Ptr key{};
    std::string name;
    if (memory::peek(asset + 0x18, key) && key && memory::peek_cstring(key, text.data(), text.size()) >= 0)
        name = text.data();
    const bool is_blueprint = derives(asset, base + entities::blueprint_type);
    logging::log(logging::Level::info, logging::Channel::skater,
                 "Hand props: phone blueprint \"{}\" = {:#x} name=\"{}\" derives Blueprint: {}", queried, asset, name,
                 is_blueprint ? "yes" : "no");
    // The ancestry, so a no is as informative as a yes.
    auto type = object_type(asset);
    for (int depth = 0; depth < 10 && type; ++depth) {
        if (type >= base && type - base < supported_build::game_image_size)
            logging::log(logging::Level::info, logging::Channel::skater, "Hand props:   type[{}] Skate+{:#x}", depth,
                         type - base);
        else
            logging::log(logging::Level::info, logging::Channel::skater, "Hand props:   type[{}] {:#x}", depth, type);
        Ptr next{};
        if (!memory::peek(type + 0x20, next)) break;
        type = next;
    }
    set_status(std::format("Hand props: phone blueprint resident; derives Blueprint: {}.",
                           is_blueprint ? "yes" : "no"));
}

void arm_derive(unsigned seconds) {
    auto &s = state();
    const auto window = seconds ? seconds : 90u;
    s.deriving = true;
    s.derive_seen = false;
    s.derive_next = 0;
    s.derive_until = GetTickCount64() + window * 1000ULL;
    logging::log(logging::Level::info, logging::Channel::skater,
                 "Hand props: resolving the phone blueprint every second for {}s; it may only be resident while the "
                 "phone is out. Nothing is created or written.", window);
    set_status(std::format("Hand props: phone blueprint scan armed for {}s.", window));
}

void service_derive(Ptr base) {
    auto &s = state();
    if (!s.deriving) return;
    const auto now = GetTickCount64();
    if (now >= s.derive_until) {
        s.deriving = false;
        if (!s.derive_seen) {
            logging::log(logging::Level::info, logging::Channel::skater,
                         "Hand props: the phone blueprint never resolved; it is not resident, even while the phone "
                         "is out.");
            set_status("Hand props: phone blueprint never resolved during the scan window.");
        }
        return;
    }
    if (s.derive_next && now < s.derive_next) return;
    s.derive_next = now + 1000;
    // Full path, both spellings: the engine names assets by path, and the short
    // characters/props/phone/basicsmartphone form does not resolve.
    static constexpr const char *names[] = {
        "characters/props/phone/basicsmartphone/basicsmartphone",
        "Characters/Props/Phone/basicSmartphone/basicSmartphone",
    };
    bool faulted{};
    for (const char *name : names) {
        if (const auto asset = find_named_asset(base, name, faulted)) {
            s.derive_seen = true;
            s.deriving = false;
            report_derive(base, name, asset);
            return;
        }
    }
}

// Hand-prop follow, skater half: compose the wrist's world transform from the
// output pose buffer every client tick and hand it to the Objects runtime,
// which moves the placed object on the park tick. The composition matches
// effect_attach.cpp's head_joint walk exactly -- same buffer, same math -- so a
// custom animation drives the prop the same way it drives the skater.
struct WorldJoint {
    std::array<float, 4> rotation{0, 0, 0, 1};
    std::array<float, 3> position{};
    float scale{1};
};
std::array<float, 3> rotate_q(const std::array<float, 4> &q, const std::array<float, 3> &v) {
    const float tx = 2 * (q[1] * v[2] - q[2] * v[1]), ty = 2 * (q[2] * v[0] - q[0] * v[2]),
                tz = 2 * (q[0] * v[1] - q[1] * v[0]);
    return {v[0] + q[3] * tx + (q[1] * tz - q[2] * ty), v[1] + q[3] * ty + (q[2] * tx - q[0] * tz),
            v[2] + q[3] * tz + (q[0] * ty - q[1] * tx)};
}
std::array<float, 4> mul_q(const std::array<float, 4> &q, const float b[4]) {
    return {q[3] * b[0] + q[0] * b[3] + q[1] * b[2] - q[2] * b[1],
            q[3] * b[1] - q[0] * b[2] + q[1] * b[3] + q[2] * b[0],
            q[3] * b[2] + q[0] * b[1] - q[1] * b[0] + q[2] * b[3],
            q[3] * b[3] - q[0] * b[0] - q[1] * b[1] - q[2] * b[2]};
}
// Folds one joint's local transform (scale.xyz, quat.xyzw, pos.xyz) onto the
// running world joint. False on an unreadable or not-yet-evaluated joint.
bool compose_child(WorldJoint &joint, Ptr buffer, std::uint16_t index) noexcept {
    std::array<float, 12> bone{};
    if (!memory::peek_bytes(buffer + static_cast<Ptr>(index) * 0x30, bone.data(), sizeof(bone))) return false;
    for (const std::size_t i : {0u, 1u, 2u, 4u, 5u, 6u, 7u, 8u, 9u, 10u})
        if (!std::isfinite(bone[i])) return false;
    const auto offset = rotate_q(joint.rotation,
                                 {bone[8] * joint.scale, bone[9] * joint.scale, bone[10] * joint.scale});
    for (std::size_t i = 0; i < 3; ++i) joint.position[i] += offset[i];
    const float local[4] = {bone[4], bone[5], bone[6], bone[7]};
    joint.rotation = mul_q(joint.rotation, local);
    joint.scale *= (bone[0] + bone[1] + bone[2]) / 3.0f;
    return true;
}
// Where the prop sits in the wrist's frame. Zero puts the object's origin at
// the wrist joint; tuned in game once the first screenshots arrive.
constexpr std::array<float, 3> hand_grip_offset{0.0f, 0.0f, 0.0f};
constexpr std::array<float, 4> hand_grip_rotation{0.0f, 0.0f, 0.0f, 1.0f};

// The skeleton resource's parent table, walked from the wrist to the root. The
// pose buffer stores parent-relative transforms, so the chain is what turns the
// wrist's local transform into a world one.
bool build_hand_chain(Ptr definition, std::uint16_t joint, std::vector<std::uint16_t> &chain,
                      std::string &error) {
    Ptr resource{};
    if (!definition || !memory::peek(definition + 0x1a0, resource) || !resource) {
        error = "the skeleton resource is unavailable";
        return false;
    }
    std::uint32_t count{};
    if (!memory::peek(resource + 0xc, count) || count <= joint || count > 1024) {
        error = "the skeleton joint count is unavailable";
        return false;
    }
    std::vector<std::uint16_t> up;
    std::uint32_t index = joint;
    for (int hop = 0; hop < 64; ++hop) {
        up.push_back(static_cast<std::uint16_t>(index));
        if (index == 0) break;
        std::int32_t parent{};
        if (!memory::peek(resource + 0x40 + static_cast<Ptr>(index) * 4, parent)) {
            error = "the skeleton parents are unreadable";
            return false;
        }
        // Parents are strictly lower indices; anything else is a cycle or a
        // layout this build does not use.
        if (parent < 0 || static_cast<std::uint32_t>(parent) >= count ||
            static_cast<std::uint32_t>(parent) >= index) {
            error = "the skeleton parent chain is invalid";
            return false;
        }
        index = static_cast<std::uint32_t>(parent);
    }
    if (up.empty() || up.back() != 0) {
        error = "the skeleton parent chain has no root";
        return false;
    }
    std::reverse(up.begin(), up.end());
    chain = std::move(up);
    return true;
}

void feed_hand_target(Ptr base, Ptr client) {
    auto &s = state();
    Local local;
    if (!resolve_local(base, client, local) || !local.holder || !local.definition) return;
    if (s.hand_definition != local.definition || s.hand_chain.size() < 2) {
        std::string error;
        if (!build_hand_chain(local.definition, s.hand_joint, s.hand_chain, error)) {
            static std::string last;
            if (error != last) {
                last = error;
                logging::log(logging::Level::warning, logging::Channel::skater,
                             "Hand props: cannot follow the wrist: {}.", error);
            }
            return;
        }
        s.hand_definition = local.definition;
    }
    const auto reader = [](Ptr address, void *out, std::size_t size) { return memory::read_bytes(address, out, size); };
    const auto pose = multiplayer::read_native_pose_layout(reader, base, local.holder, 512);
    if (!pose.buffer || pose.count <= s.hand_joint) return;
    WorldJoint joint;
    for (const auto index : s.hand_chain)
        if (!compose_child(joint, pose.buffer, index)) return;
    const auto offset = rotate_q(joint.rotation, hand_grip_offset);
    const std::array<float, 3> position{joint.position[0] + offset[0], joint.position[1] + offset[1],
                                        joint.position[2] + offset[2]};
    const auto rotation = mul_q(joint.rotation, hand_grip_rotation.data());
    profile_runtime::set_prop_hand_target(position, rotation);
}

bool write_u32(Ptr address, std::uint32_t value) noexcept {
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<void *>(address), &region, sizeof(region)) || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    const auto protection = region.Protect & 0xff;
    if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY && protection != PAGE_EXECUTE_READWRITE &&
        protection != PAGE_EXECUTE_WRITECOPY)
        return false;
    __try {
        std::memcpy(reinterpret_cast<void *>(address), &value, sizeof(value));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The write test. Three floats in the animation instance go to exactly 1.0 while
// a gesture with a prop is pressed; if they are layer weights, holding them at
// 1.0 should keep the phone out with no gesture. The old values are saved and
// restored, and every attempt is logged.
void arm_poke(Ptr base, Ptr client, unsigned seconds) {
    auto &s = state();
    Local local;
    if (!resolve_local(base, client, local) || !local.rig) {
        set_status("Hand props: the animation instance is not ready to poke.");
        return;
    }
    static constexpr Ptr offsets[3] = {0x3d40, 0x3d68, 0x3d6c};
    s.poke_rig = local.rig;
    for (std::size_t i = 0; i < 3; ++i) {
        const auto address = local.rig + offsets[i];
        std::uint32_t value{};
        if (!memory::peek_bytes(address, &value, sizeof(value))) {
            set_status(std::format("Hand props: rig+{:#x} is not readable; nothing written.", offsets[i]));
            return;
        }
        float as_float{};
        std::memcpy(&as_float, &value, sizeof(as_float));
        // A layer weight reads as a normal float in [0, 1]. A subnormal also
        // falls in the numeric range (0x0000f002), which is exactly the hole
        // Codex flagged: it is a possible integer/flag word, not a weight, so
        // the guard must reject it before any write.
        if (!std::isfinite(as_float) || std::fpclassify(as_float) == FP_SUBNORMAL || as_float < 0.0f ||
            as_float > 1.0f) {
            set_status(std::format("Hand props: rig+{:#x} reads {:#010x} ({:.6g}), not a weight; nothing written.",
                                   offsets[i], value, as_float));
            logging::log(logging::Level::warning, logging::Channel::skater,
                         "Hand props: poke refused: rig+{:#x} is {:#010x} ({:.6g}), not a weight.", offsets[i],
                         value, as_float);
            return;
        }
        s.poke_addresses[i] = address;
        s.poke_old[i] = value;
        logging::log(logging::Level::info, logging::Channel::skater,
                     "Hand props: poke: rig+{:#x} is {:#010x} (float {:.6g})", offsets[i], value, as_float);
    }
    constexpr std::uint32_t one = 0x3f800000; // 1.0f
    std::size_t written{};
    for (std::size_t i = 0; i < 3; ++i)
        if (write_u32(s.poke_addresses[i], one)) ++written;
    s.poking = written != 0;
    s.poke_until = GetTickCount64() + (seconds ? seconds : s.poke_seconds) * 1000ULL;
    s.poke_overwrites = s.poke_rewrites = 0;
    logging::log(logging::Level::info, logging::Channel::skater,
                 "Hand props: poke: wrote 1.0 into {} of 3 layer weights; holding {}s. Watch the skater; "
                 "'prop poke off' restores.",
                 written, seconds ? seconds : s.poke_seconds);
    set_status(std::format("Hand props: poked {} weight(s) to 1.0 for {}s.", written, seconds));
}

void stop_poke(bool restore) {
    auto &s = state();
    if (!s.poking) return;
    if (restore)
        for (std::size_t i = 0; i < 3; ++i)
            if (s.poke_addresses[i]) (void)write_u32(s.poke_addresses[i], s.poke_old[i]);
    logging::log(logging::Level::info, logging::Channel::skater,
                 "Hand props: poke {}: {} graph overwrite(s), {} rewrite(s).", restore ? "restored" : "released",
                 s.poke_overwrites, s.poke_rewrites);
    set_status(std::format("Hand props: poke {}.", restore ? "restored" : "released"));
    s.poking = false;
    s.poke_overwrites = s.poke_rewrites = 0;
}

// ---------------------------------------------------------------------------
// Character morph clamp. CAS body sliders and body-type tweakables reach the
// DingoMorph GPU graph through DingoMorphGraphDataMappingAsset: one entry per
// input, each authoring MinValue/MaxValue (0..1 for every body region), and
// that authored range is what clamps a slider. Runtime layout from the Studio
// schema: the mapping container sits at asset+0x20 and each entry is 48 bytes --
// name strings at +0x00/+0x08/+0x10, multiplier float +0x18, field hash +0x1c,
// MaxValue float +0x20, DataFieldKey int +0x24, MinValue float +0x28.
struct MorphEntry {
    Ptr address{};
    std::string name;
    float min{1}, max{}, multiplier{1};
    std::int32_t key{};
};
bool morph_body_input(const std::string &name) {
    static constexpr const char *tokens[] = {
        "BaseOverrideForBody", "feet",   "calfs",    "thighs",  "glute",    "gut",     "arms",
        "chest",               "g_hips", "g_chest",  "g_crotch", "g_butt",  "Archetype",
    };
    for (const char *token : tokens)
        if (name.find(token) != std::string::npos) return true;
    return false;
}
std::string morph_name(Ptr address) {
    Ptr text{};
    std::array<char, 96> buffer{};
    if (!memory::peek(address, text) || !text || memory::peek_cstring(text, buffer.data(), buffer.size()) < 0)
        return {};
    return buffer.data();
}
bool read_morph_mapping(Ptr base, std::vector<MorphEntry> &entries, Ptr &asset, std::string &error) {
    bool faulted{};
    asset = find_named_asset(base, "characters/maincharacters/generic/cas/common/metamorph/cas_rsp_datamappingasset",
                             faulted);
    if (!asset) {
        error = "the morph mapping asset is not loaded (be in a level or in CAS first)";
        return false;
    }
    std::array<Ptr, 3> container{};
    if (!memory::peek_bytes(asset + 0x20, container.data(), sizeof(container))) {
        error = "the mapping container is unreadable";
        return false;
    }
    // The DataContainer is {begin,end,capacity} or {pointer,count,capacity}; take
    // whichever shape lands on the schema's first entry ("BaseOverrideForBody").
    Ptr table{};
    std::size_t count{};
    if (container[1] > container[0] && (container[1] - container[0]) % 48 == 0) {
        table = container[0];
        count = static_cast<std::size_t>((container[1] - container[0]) / 48);
    } else if (container[1] && container[1] <= 4096) {
        table = container[0];
        count = container[1];
    }
    if (!table || count != 94 || morph_name(table + 0x10) != "BaseOverrideForBody") {
        error = "the mapping layout did not match the Studio schema";
        return false;
    }
    entries.clear();
    for (std::size_t i = 0; i < count; ++i) {
        const auto address = table + i * 48;
        MorphEntry entry;
        entry.address = address;
        entry.name = morph_name(address + 0x10);
        (void)memory::peek(address + 0x18, entry.multiplier);
        (void)memory::peek(address + 0x24, entry.key);
        (void)memory::peek(address + 0x20, entry.max);
        (void)memory::peek(address + 0x28, entry.min);
        entries.push_back(std::move(entry));
    }
    return true;
}
void morph_find(Ptr base) {
    std::vector<MorphEntry> entries;
    Ptr asset{};
    std::string error;
    if (!read_morph_mapping(base, entries, asset, error)) {
        logging::log(logging::Level::warning, logging::Channel::skater, "Hand props: morph mapping: {}.", error);
        set_status("Hand props: morph mapping: " + error + ".");
        return;
    }
    logging::log(logging::Level::info, logging::Channel::skater,
                 "Hand props: morph mapping asset {:#x}, {} inputs.", asset, entries.size());
    for (const auto &entry : entries) {
        if (!morph_body_input(entry.name)) continue;
        logging::log(logging::Level::info, logging::Channel::skater,
                     "Hand props: morph {} key={} range [{:.2f}, {:.2f}] multiplier {:.2f}", entry.name, entry.key,
                     entry.min, entry.max, entry.multiplier);
    }
    set_status(std::format("Hand props: morph mapping found; {} inputs, body entries in the log.", entries.size()));
}
void morph_patch(Ptr base, float max_value, bool restore) {
    auto &s = state();
    std::vector<MorphEntry> entries;
    Ptr asset{};
    std::string error;
    if (!read_morph_mapping(base, entries, asset, error)) {
        logging::log(logging::Level::warning, logging::Channel::skater, "Hand props: morph mapping: {}.", error);
        set_status("Hand props: morph mapping: " + error + ".");
        return;
    }
    if (restore) {
        std::size_t count{};
        for (const auto &[address, original] : s.morph_originals) {
            std::uint32_t bits{};
            std::memcpy(&bits, &original, sizeof(bits));
            if (write_u32(address, bits)) ++count;
        }
        s.morph_originals.clear();
        logging::log(logging::Level::info, logging::Channel::skater,
                     "Hand props: morph clamp restored on {} input(s).", count);
        set_status(std::format("Hand props: morph clamp restored on {} input(s).", count));
        return;
    }
    const auto remember = [&](Ptr address, float current) {
        for (const auto &[known, _] : s.morph_originals)
            if (known == address) return;
        s.morph_originals.emplace_back(address, current);
    };
    std::size_t count{};
    for (const auto &entry : entries) {
        if (!morph_body_input(entry.name) || entry.name.empty()) continue;
        remember(entry.address + 0x20, entry.max);
        std::uint32_t bits{};
        std::memcpy(&bits, &max_value, sizeof(bits));
        if (write_u32(entry.address + 0x20, bits)) ++count;
    }
    logging::log(logging::Level::info, logging::Channel::skater,
                 "Hand props: morph max raised to {:.2f} on {} body input(s); 'morph clamp' restores them.",
                 max_value, count);
    set_status(std::format("Hand props: morph max {:.2f} on {} input(s).", max_value, count));
}
void service_morph(Ptr base, bool find, bool unclamp, bool clamp, float max_value) {
    if (clamp) morph_patch(base, 0, true);
    else if (unclamp) morph_patch(base, max_value, false);
    else if (find) morph_find(base);
}

void dump_region(Ptr address, std::size_t bytes, const std::string &label) {
    for (std::size_t offset = 0; offset + 4 <= bytes; offset += 4) {
        std::uint32_t value{};
        if (!memory::read_bytes(address + offset, &value, 4)) break;
        float as_float{};
        std::memcpy(&as_float, &value, sizeof(as_float));
        logging::log(logging::Level::info, logging::Channel::skater,
                     "Hand props: {} +{:#04x} {:#010x} {} (float {:.6g})", label, offset, value, value, as_float);
    }
}

void arm_assets(Ptr base, Ptr client, const std::string &extra, unsigned seconds) {
    auto &s = state();
    Local local;
    if (!resolve_local(base, client, local)) {
        set_status("Hand props: the local skater is not ready.");
        return;
    }
    static constexpr const char *names[] = {
        "bool.anim.phone.forceattachlock",  "bool.anim.ptm.layer.phone.enable", "bool.gp.phonepose.enable",
        "bool.phone.attachlock",            "bool.state.is.phoneposeoffboard",  "ebool.onboard.phone.attachlock",
        "float.anim.phoneposeweight",
    };
    struct Spot {
        std::string label;
        Ptr address{};
    };
    std::vector<Spot> spots;
    std::vector<Spot> assets;
    std::vector<Spot> datas;
    bool faulted{};
    // Studio's browser shows a tree, but the engine names an asset by its full
    // path (the vfx catalog spells them "effects/gestures/effectblueprints/
    // ebp_gesture_saxophone"), so try the path and the bare leaf. The skeleton is
    // the control: if that does not resolve, the lookup or its naming is wrong,
    // not the asset.
    const auto lookup = [&](const std::string &name) -> Ptr {
        if (const auto asset = find_named_asset(base, ("animation/dingo/" + name).c_str(), faulted)) return asset;
        return find_named_asset(base, name.c_str(), faulted);
    };
    {
        const auto control = find_named_asset(base, "Animation/Dingo/AnimBase_Default_Skeleton", faulted);
        logging::log(logging::Level::info, logging::Channel::skater,
                     "Hand props: control asset Animation/Dingo/AnimBase_Default_Skeleton {}.", control
                         ? std::format("= {:#x}", control)
                         : std::string("did not resolve (the lookup, not the phone assets, is the problem)"));
    }
    const auto note = [&](const std::string &name) {
        const auto asset = lookup(name);
        if (!asset) {
            logging::log(logging::Level::info, logging::Channel::skater, "Hand props: {} is not loaded.", name);
            return;
        }
        logging::log(logging::Level::info, logging::Channel::skater, "Hand props: {} = {:#x}.", name, asset);
        // A named graph switch is global: the gameplay code writes the value
        // into the asset, so the asset's own bytes are the live value. Watch
        // them as well as any instance reference.
        assets.push_back({name + " asset", asset});
        std::uint32_t data{};
        if (memory::read_bytes(asset + 0x28, &data, sizeof(data)) && data != 0)
            datas.push_back({name + " data", static_cast<Ptr>(data) & ~Ptr{7}});
        struct Scan {
            const char *region;
            Ptr address;
            std::size_t bytes;
        };
        const Scan regions[] = {
            {"rig", local.rig, 0x4000},
            {"holder", local.holder, 0x400},
            {"anim", local.component, 0x200},
            {"definition", local.definition, 0x400},
        };
        for (const auto &region : regions) {
            for (std::size_t offset = 0; region.address && offset + 8 <= region.bytes; offset += 8) {
                std::uint64_t value{};
                if (!memory::read_bytes(region.address + offset, &value, 8)) break;
                // References are tagged, so compare with the low bits masked off.
                if ((static_cast<Ptr>(value) & ~Ptr{7}) != asset) continue;
                logging::log(logging::Level::info, logging::Channel::skater,
                             "Hand props:   {} held at {}+{:#x} (words {} {})", name, region.region, offset,
                             static_cast<std::uint32_t>(value), static_cast<std::uint32_t>(value >> 32));
                spots.push_back({std::format("{}+{:#x}", region.region, offset), region.address + offset});
            }
        }
    };
    for (const char *name : names) note(name);
    if (!extra.empty()) note(extra);
    logging::log(logging::Level::info, logging::Channel::skater,
                 "Hand props: {} reference(s) found for the phone assets{}.", spots.size(),
                 faulted ? " (the asset sweep faulted at least once)" : "");

    s.regions.clear();
    s.words.clear();
    s.changes = s.lines = s.repeats = 0;
    s.phase = 0;
    s.log_every = false;
    s.pose_mode = false;
    // The asset's own bytes first: a global switch lives there, and the whole
    // asset header plus payload fits in a small window.
    for (const auto &asset : assets) {
        add_region(s, asset.label, asset.address, 0x80, true);
        dump_region(asset.address, 0x40, asset.label);
    }
    for (const auto &data : datas) {
        logging::log(logging::Level::info, logging::Channel::skater,
                     "Hand props: {} is already instantiated at {:#x}.", data.label, data.address);
        // Wide enough to cover the value, wherever in the object it sits.
        add_region(s, data.label, data.address, 0x200);
        dump_region(data.address, 0x100, data.label);
    }
    for (const auto &spot : spots) add_region(s, spot.label, spot.address, 0x20);
    if (s.words.empty()) {
        set_status("Hand props: none of the phone assets are loaded to reference.");
        return;
    }
    const auto window = seconds ? seconds : s.assets_seconds;
    s.watching = true;
    s.watch_start = GetTickCount64();
    s.watch_until = s.watch_start + window * 1000ULL;
    s.next_heartbeat = s.watch_start + 5000;
    logging::log(logging::Level::info, logging::Channel::skater,
                 "Hand props: watching {} words: {} phone asset image(s) and {} instance reference(s), for {}s. "
                 "Idle {}s, then do the gesture (or hold the menu phone) so the two cases can be told apart.",
                 s.words.size(), assets.size(), spots.size(), window, idle_seconds);
    set_status(std::format("Hand props: {} asset image(s), {} reference(s), {} words, {}s; idle {}s then gesture.",
                           assets.size(), spots.size(), s.words.size(), window, idle_seconds));
}

void arm_pose(Ptr base, Ptr client, unsigned seconds) {
    auto &s = state();
    Local local;
    if (!resolve_local(base, client, local)) {
        set_status("Hand props: the local skater is not ready to watch.");
        return;
    }
    const auto reader = [](Ptr address, void *out, std::size_t size) { return memory::read_bytes(address, out, size); };
    const auto pose = multiplayer::read_native_pose_layout(reader, base, local.holder, 512);
    if (!pose.buffer || pose.count < 395) {
        set_status("Hand props: the skater's output pose is not ready.");
        return;
    }
    const auto window = seconds ? seconds : s.pose_seconds;
    s.regions.clear();
    s.words.clear();
    s.changes = s.lines = s.repeats = 0;
    s.phase = 0;
    s.log_every = false;
    s.pose_mode = true;
    // The whole output pose, every joint. If the phone hangs off a socket joint
    // in the skeleton, that joint moves when the phone appears and this is where
    // it shows.
    add_region(s, "pose", pose.buffer, pose.count * 0x30);
    if (s.words.empty()) {
        set_status("Hand props: no pose state to watch yet.");
        return;
    }
    s.watching = true;
    s.watch_start = GetTickCount64();
    s.watch_until = s.watch_start + window * 1000ULL;
    s.next_heartbeat = s.watch_start + 5000;
    logging::log(logging::Level::info, logging::Channel::skater,
                 "Hand props: watching the output pose ({} joints, {} words) for {}s. Idle {}s, then hold the phone "
                 "out (pause menu or gesture); every joint field that moves is logged once per phase.",
                 pose.count, s.words.size(), window, idle_seconds);
    set_status(std::format("Hand props: watching the pose for {}s; idle {}s, then hold the phone out.", window,
                           idle_seconds));
}

void sample() {
    auto &s = state();
    const auto now = GetTickCount64();
    // The idle phase ends by itself, so the press needs no console visit in the
    // middle of the window (opening it steals input focus).
    if (s.phase == 0 && !s.log_every && now >= s.watch_start + idle_seconds * 1000ULL && now < s.watch_until) {
        ++s.phase;
        s.next_heartbeat = 0;
        logging::log(logging::Level::info, logging::Channel::skater,
                     "Hand props: idle phase over, phase1 begins ({}s left). Press the gesture now.",
                     static_cast<unsigned>((s.watch_until - now) / 1000));
    }
    for (auto &word : s.words) {
        std::uint32_t value{};
        if (!memory::peek(word.address, value) || value == word.value) continue;
        ++s.changes;
        {
            // An asset image's +0x28 is its runtime data pointer. When it
            // appears, that object is where the live value is, so follow it.
            const auto &region = s.regions[word.region];
            if (region.asset_image && word.address == region.address + 0x28 && value != 0)
                s.pending_regions.emplace_back(region.name + " data", static_cast<Ptr>(value) & ~Ptr{7});
        }
        if (!word.logged) {
            // One line per address, tagged with the phase it first changed in.
            // The value is not filtered: a request may be a pointer rather than
            // an id, and the phase diff is what separates signal from churn.
            word.logged = true;
            word.phase = s.phase;
            if (s.lines < max_changes) {
                ++s.lines;
                const auto offset = static_cast<unsigned>(word.address - s.regions[word.region].address);
                if (s.pose_mode) {
                    logging::log(logging::Level::info, logging::Channel::skater,
                                 "Hand props: pose phase{} j{:03d}.{} {} -> {} ({:#x} -> {:#x})",
                                 word.phase, offset / 0x30, (offset % 0x30) / 4, word.value, value, word.value,
                                 value);
                } else {
                    logging::log(logging::Level::info, logging::Channel::skater,
                                 "Hand props: change phase{} {} +{:#x} {} -> {} ({:#x} -> {:#x}) ({}s left)",
                                 word.phase, s.regions[word.region].name, offset, word.value, value, word.value,
                                 value,
                                 s.watch_until > now ? static_cast<unsigned>((s.watch_until - now) / 1000) : 0);
                }
            }
        } else if (s.log_every) {
            // Narrow watch: the timeline is the finding, so every transition is
            // logged, with the value read as a float as well -- layer weights
            // are floats, and 1.0 shows up as 1065353216.
            ++s.lines;
            if (s.lines < max_changes) {
                float as_float{};
                std::memcpy(&as_float, &value, sizeof(as_float));
                logging::log(logging::Level::info, logging::Channel::skater,
                             "Hand props: {} +{:#x} {} -> {} (float {:.4f})",
                             s.regions[word.region].name,
                             static_cast<unsigned>(word.address - s.regions[word.region].address), word.value, value,
                             as_float);
            }
        } else {
            ++s.repeats;
        }
        word.value = value;
    }
    // Anything the loop discovered: the asset's data object, now worth reading.
    for (auto &[name, address] : s.pending_regions) {
        logging::log(logging::Level::info, logging::Channel::skater,
                     "Hand props: following {} at {:#x}.", name, address);
        add_region(s, name, address, 0x200);
        dump_region(address, 0x100, name);
    }
    s.pending_regions.clear();
    // A heartbeat, so a quiet window is provably a quiet window and the press
    // can be placed on the timeline from the log alone.
    if (now >= s.next_heartbeat && now < s.watch_until) {
        s.next_heartbeat = now + 5000;
        logging::log(logging::Level::info, logging::Channel::skater,
                     "Hand props: watching, phase{}, {} line(s), {} change(s), {} repeats, {}s left.", s.phase,
                     s.lines, s.changes, s.repeats, static_cast<unsigned>((s.watch_until - now) / 1000));
    }
    if (now >= s.watch_until) {
        s.watching = false;
        logging::log(logging::Level::info, logging::Channel::skater,
                     "Hand props: watch finished: {} line(s), {} change(s), {} words, final phase {}.",
                     s.lines, s.changes, s.words.size(), s.phase);
        set_status(std::format("Hand props: watch finished with {} line(s); see the log.", s.lines));
    }
}
} // namespace

void request_prop_report(std::string filter) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.filter = std::move(filter);
    s.report_pending = true;
}
void request_prop_watch(std::string filter, unsigned seconds) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.filter = std::move(filter);
    s.watch_seconds = seconds;
    s.watch_pending = true;
    s.mark_pending = false;
}
void request_prop_mark() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.mark_pending = true;
}
void request_prop_weight(unsigned seconds) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.weight_seconds = seconds;
    s.weight_pending = true;
}
void request_prop_pose(unsigned seconds) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.pose_seconds = seconds;
    s.pose_pending = true;
}
void request_prop_assets(std::string extra, unsigned seconds) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.assets_extra = std::move(extra);
    s.assets_seconds = seconds;
    s.assets_pending = true;
}
void request_prop_poke(unsigned seconds) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.poke_seconds = seconds;
    s.poke_pending = true;
    s.poke_stop_pending = false;
}
void request_prop_poke_stop() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.poke_stop_pending = true;
    s.poke_pending = false;
}
void request_prop_trace(unsigned seconds) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.trace_seconds = seconds;
    s.trace_pending = true;
    s.trace_off_pending = false;
}
void request_prop_trace_off() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.trace_off_pending = true;
    s.trace_pending = false;
}
void request_prop_derive(unsigned seconds) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.derive_seconds = seconds;
    s.derive_pending = true;
}
void request_prop_hand_attach(std::string name, bool left) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.hand_name = std::move(name);
    s.hand_left = left;
    s.hand_attach_pending = true;
    s.hand_detach_pending = false;
}
void request_prop_hand_detach() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.hand_detach_pending = true;
    s.hand_attach_pending = false;
}
void request_morph_find() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.morph_find_pending = true;
}
void request_morph_unclamp(float max_value) {
    auto &s = state();
    if (!std::isfinite(max_value) || max_value < 1.0f || max_value > 50.0f) max_value = 4.0f;
    std::lock_guard lock(s.mutex);
    s.morph_unclamp_max = max_value;
    s.morph_unclamp_pending = true;
}
void request_morph_clamp() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.morph_clamp_pending = true;
}
std::string prop_status() {
    std::string text;
    {
        std::lock_guard lock(status_mutex());
        text = state().status;
    }
    // The hand follow lives in the objects runtime; show its state beside ours.
    if (const auto hand = profile_runtime::prop_hand_status(); !hand.empty()) text += "  [hand: " + hand + "]";
    return text;
}

void tick_prop_attach(std::uintptr_t base, std::uintptr_t client) noexcept {
    try {
        auto &s = state();
        std::string filter;
        bool want_report{}, want_watch{}, want_weight{}, want_pose{}, want_assets{}, want_poke{}, want_poke_stop{};
        bool want_trace{}, want_trace_off{}, want_derive{}, want_hand_attach{}, want_hand_detach{};
        bool want_morph_find{}, want_morph_unclamp{}, want_morph_clamp{};
        unsigned seconds{default_seconds};
        unsigned weight_seconds{};
        unsigned pose_seconds{};
        unsigned assets_seconds{};
        unsigned poke_seconds{};
        unsigned trace_seconds{};
        unsigned derive_seconds{};
        std::string assets_extra;
        std::string hand_name;
        bool hand_left{};
        float morph_max{4.0f};
        {
            std::lock_guard lock(s.mutex);
            want_report = s.report_pending;
            want_watch = s.watch_pending;
            want_weight = s.weight_pending;
            want_pose = s.pose_pending;
            want_assets = s.assets_pending;
            want_poke = s.poke_pending;
            want_poke_stop = s.poke_stop_pending;
            want_trace = s.trace_pending;
            want_trace_off = s.trace_off_pending;
            want_derive = s.derive_pending;
            want_hand_attach = s.hand_attach_pending;
            want_hand_detach = s.hand_detach_pending;
            want_morph_find = s.morph_find_pending;
            want_morph_unclamp = s.morph_unclamp_pending;
            want_morph_clamp = s.morph_clamp_pending;
            filter = s.filter;
            seconds = s.watch_seconds;
            weight_seconds = s.weight_seconds;
            pose_seconds = s.pose_seconds;
            assets_seconds = s.assets_seconds;
            poke_seconds = s.poke_seconds;
            trace_seconds = s.trace_seconds;
            derive_seconds = s.derive_seconds;
            assets_extra = s.assets_extra;
            hand_name = s.hand_name;
            hand_left = s.hand_left;
            morph_max = s.morph_unclamp_max;
            s.report_pending = s.watch_pending = s.weight_pending = s.pose_pending = s.assets_pending = false;
            s.poke_pending = s.poke_stop_pending = false;
            s.trace_pending = s.trace_off_pending = s.derive_pending = false;
            s.hand_attach_pending = s.hand_detach_pending = false;
            s.morph_find_pending = s.morph_unclamp_pending = s.morph_clamp_pending = false;
        }
        if (want_poke_stop) stop_poke(true);
        else if (want_poke) arm_poke(base, client, poke_seconds);
        if (s.poking) {
            // Hold the weights and count how often the graph puts them back.
            Local local;
            if (!resolve_local(base, client, local) || !local.rig || local.rig != s.poke_rig) {
                // A different rig means the saved addresses belong to a dead
                // instance; restore is pointless, so release without writing.
                stop_poke(false);
            } else {
                constexpr std::uint32_t one = 0x3f800000;
                for (const auto address : s.poke_addresses) {
                    std::uint32_t value{};
                    if (!address || !memory::peek_bytes(address, &value, sizeof(value)) || value == one) continue;
                    ++s.poke_overwrites;
                    if (s.poke_rewrites < 10)
                        logging::log(logging::Level::info, logging::Channel::skater,
                                     "Hand props: poke: the graph reset rig+{:#x} to {:#010x}; rewriting.",
                                     address - local.rig, value);
                    if (write_u32(address, one)) ++s.poke_rewrites;
                }
                if (GetTickCount64() >= s.poke_until) stop_poke(true);
            }
        }
        if (want_trace_off) disarm_trace();
        else if (want_trace) arm_trace(base, trace_seconds);
        if (want_derive) arm_derive(derive_seconds);
        if (want_hand_detach) {
            profile_runtime::request_prop_hand_release();
            s.hand_feeding = false;
            s.hand_chain.clear();
            s.hand_definition = 0;
            set_status("Hand props: releasing the held prop; it returns to where it was placed.");
        } else if (want_hand_attach) {
            s.hand_joint = hand_left ? 49 : 278; // j049 = left wrist, j278 = right
            s.hand_definition = 0;
            s.hand_chain.clear();
            s.hand_feeding = true;
            profile_runtime::request_prop_hand(hand_name);
            logging::log(logging::Level::info, logging::Channel::skater,
                         "Hand props: hand follow requested ({} wrist{}).", hand_left ? "left" : "right",
                         hand_name.empty() ? "" : ", matching \"" + hand_name + "\"");
            set_status(std::format("Hand props: taking a placed object{} into the {} hand.",
                                   hand_name.empty() ? "" : " matching \"" + hand_name + "\"",
                                   hand_left ? "left" : "right"));
        }
        // The wrist target is refreshed every client tick while following; the
        // Objects runtime applies it on the park tick.
        if (s.hand_feeding) feed_hand_target(base, client);
        // These run every tick: they are inert when idle and must keep working
        // while no watch is armed.
        service_derive(base);
        drain_trace(base);
        service_morph(base, want_morph_find, want_morph_unclamp, want_morph_clamp, morph_max);
        if (want_report) report(base, filter);
        if (want_assets) arm_assets(base, client, assets_extra, assets_seconds);
        else if (want_pose) arm_pose(base, client, pose_seconds);
        else if (want_weight) arm_weight(base, client, weight_seconds);
        else if (want_watch) arm(base, client, filter, seconds);
        if (!s.watching) return;
        // A level change invalidates every address the watch holds; stop rather
        // than keep reading a freed context.
        Local local;
        if (!resolve_local(base, client, local)) {
            s.watching = false;
            logging::log(logging::Level::info, logging::Channel::skater,
                         "Hand props: watch stopped, the local skater went away.");
            set_status("Hand props: watch stopped (the skater went away).");
            return;
        }
        // Every frame: a request written and consumed inside one frame is only
        // visible if the sampling keeps up with the client tick.
        if (s.mark_pending) {
            s.mark_pending = false;
            ++s.phase;
            logging::log(logging::Level::info, logging::Channel::skater,
                         "Hand props: mark: now phase{} ({}s left).", s.phase,
                         s.watch_until > GetTickCount64()
                             ? static_cast<unsigned>((s.watch_until - GetTickCount64()) / 1000)
                             : 0);
        }
        sample();
    } catch (const std::exception &e) {
        set_status(std::string("Hand props: ") + e.what());
    } catch (...) {
        set_status("Hand props: unknown failure.");
    }
}

} // namespace dingosdk::skater
