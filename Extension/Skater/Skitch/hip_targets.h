#pragma once
#include <array>
#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <span>

namespace skateskitch {
using Vec3 = std::array<float, 3>;
struct Joint {
    Vec3 position{};
    std::array<float, 4> rotation{0, 0, 0, 1};
    Vec3 scale{1, 1, 1};
};
// Columns of an affine basis; preserves nonuniform scale along the hierarchy.
struct Anchor {
    std::array<Vec3, 3> basis{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    Vec3 position{};
};
std::optional<Anchor> hip_anchor(std::span<const Joint> joints, Vec3 hip_local_offset = {});

// Epoch and native generation prevent a recycled peer slot/actor inheriting a proxy.
struct PlayerKey {
    std::uint64_t id{}, epoch{}, generation{};
    auto operator<=>(const PlayerKey&) const = default;
};
struct WorldKey {
    std::uint64_t session{}, map{}, world{};
    bool operator==(const WorldKey&) const = default;
};
struct RemoteSample {
    PlayerKey player;
    WorldKey world;
    Anchor hip;
    std::uint64_t received_us{};
    bool eligible{};
};
using ProxyHandle = std::uint64_t;
// Implement only after the ECS prefab spawn/placement/destruction contracts are
// verified. This project supplies a fake backend for isolated tests, not a live hook.
class TargetBackend {
public:
    virtual ~TargetBackend() = default;
    virtual std::optional<ProxyHandle> create(PlayerKey, const Anchor&) = 0;
    virtual bool move(ProxyHandle, const Anchor&) = 0;
    // The engine backend must release any local grab before destroying its proxy.
    virtual void destroy(ProxyHandle) noexcept = 0;
};
struct TargetPolicy {
    std::uint64_t stale_us = 250000;
    float teleport_distance = 10.0f; // prototype policy, not stock tuning
};
class HipTargets {
public:
    explicit HipTargets(TargetBackend& backend, TargetPolicy policy = {});
    ~HipTargets();
    HipTargets(const HipTargets&) = delete;
    HipTargets& operator=(const HipTargets&) = delete;
    // Full frame of remote candidates, on one owning thread. An empty frame clears
    // all proxies. Sample arrival time is network arrival, not render sampling time.
    void sync(WorldKey, std::uint64_t local_id, std::uint64_t now_us,
              std::span<const RemoteSample>);
    void clear() noexcept;
    std::size_t size() const noexcept { return entries_.size(); }
    std::optional<ProxyHandle> find(PlayerKey) const;
    std::optional<PlayerKey> nearest(Vec3 position, float radius) const;
private:
    struct Entry { ProxyHandle handle; Anchor hip; };
    TargetBackend& backend_;
    TargetPolicy policy_;
    WorldKey world_{};
    std::optional<std::uint64_t> last_sync_;
    std::map<PlayerKey, Entry> entries_;
};
} // namespace skateskitch
