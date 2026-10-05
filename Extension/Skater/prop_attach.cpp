#include "prop_attach.h"
#include "Gestures/board_gesture_layout.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace dingosdk::skater {
namespace {
using Ptr = std::uintptr_t;
namespace engine = game::build::v20260929::engine;

// Watch every client frame: a gesture request may be written and consumed
// inside one frame, which a five-per-second sample would simply miss. The
// change log keeps only small-integer transitions -- a gesture id is 0..254 --
// because context floats otherwise drown everything.
constexpr unsigned item_scan_bytes = 0x60;  // each gesture item's interesting head
constexpr unsigned max_watch_items = 64;    // sampled items, not a hard limit on discovery
constexpr unsigned max_changes = 6000;      // the log is a finding, not a trace
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
    bool mark_pending{};
    std::vector<Region> regions;
    std::vector<Word> words;
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

void add_region(State &s, std::string name, Ptr address, std::size_t bytes) {
    if (!address) return;
    s.regions.push_back({std::move(name), address});
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

void sample() {
    auto &s = state();
    const auto now = GetTickCount64();
    // The idle phase ends by itself, so the press needs no console visit in the
    // middle of the window (opening it steals input focus).
    if (s.phase == 0 && now >= s.watch_start + idle_seconds * 1000ULL && now < s.watch_until) {
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
        if (!word.logged) {
            // One line per address, tagged with the phase it first changed in.
            // The value is not filtered: a request may be a pointer rather than
            // an id, and the phase diff is what separates signal from churn.
            word.logged = true;
            word.phase = s.phase;
            if (s.lines < max_changes) {
                ++s.lines;
                logging::log(logging::Level::info, logging::Channel::skater,
                             "Hand props: change phase{} {} +{:#x} {} -> {} ({:#x} -> {:#x}) ({}s left)",
                             word.phase, s.regions[word.region].name,
                             static_cast<unsigned>(word.address - s.regions[word.region].address), word.value, value,
                             word.value, value,
                             s.watch_until > now ? static_cast<unsigned>((s.watch_until - now) / 1000) : 0);
            }
        } else {
            ++s.repeats;
        }
        word.value = value;
    }
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
                     "Hand props: watch finished: {} line(s) over {} address(es), {} change(s), {} words, final "
                     "phase {}; diff phase0 against phase1.",
                     s.lines, s.lines, s.changes, s.words.size(), s.phase);
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
std::string prop_status() {
    std::lock_guard lock(status_mutex());
    return state().status;
}

void tick_prop_attach(std::uintptr_t base, std::uintptr_t client) noexcept {
    try {
        auto &s = state();
        std::string filter;
        bool want_report{}, want_watch{};
        unsigned seconds{default_seconds};
        {
            std::lock_guard lock(s.mutex);
            want_report = s.report_pending;
            want_watch = s.watch_pending;
            filter = s.filter;
            seconds = s.watch_seconds;
            s.report_pending = s.watch_pending = false;
        }
        if (want_report) report(base, filter);
        if (want_watch) arm(base, client, filter, seconds);
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
