#pragma once
#include "hip_targets.h"
#include <string_view>

namespace skateskitch {
// Follow behind and to the leader's left, independent of grab approach.
// Scaled so the hip-to-goal distance is ~6 in (0.152 m) shorter than the
// original 1.25/0.65 slot: the rider rides a touch closer without changing
// the rear-left geometry or the side swap.
inline constexpr float tow_follow_back=1.115f;
inline constexpr float tow_follow_left=.58f;
inline constexpr float tow_follow_distance=1.25683f; // hypot(back, left)
inline constexpr float tow_turn_response=7.f;
inline constexpr float tow_turn_rate=2.65f;
inline constexpr float tow_max_separation=24.f;
inline constexpr float tow_max_target_speed=160.f;
inline constexpr float tow_max_rider_speed=180.f;
inline constexpr float tow_max_acceleration=600.f;
// A missing network sample is far more common than a leader actually leaving:
// keep the grip on the last known hip through this window instead of tearing
// the drag apart on every pose-stream gap.
inline constexpr std::uint64_t tow_stale_grace_us=900000;
struct TowCandidate {
    RemoteSample sample;
    std::array<float,4> heading{0,0,0,1};
};
// Why the last acquisition attempt saw what it saw: one count per eligibility
// predicate across every candidate offered on the press, plus the nearest hip,
// so a press that finds nobody explains itself in the log.
struct TowDiag {
    std::uint32_t n{};            // candidates offered on the press
    std::uint32_t fresh{};        // sample arrived inside the 250 ms window
    std::uint32_t future{};       // arrival timestamp ahead of the local clock
    std::uint32_t world{};        // same world/session as the local rider
    std::uint32_t identity{};     // epoch and generation both present
    std::uint32_t hip{};          // hip position finite
    std::uint32_t yaw{};          // heading resolves to a yaw
    float nearest{-1.f};          // metres to the closest offered hip; -1 = none
    std::uint64_t age_min_us{};   // sample age window across the offered set
    std::uint64_t age_max_us{};
};
struct TowPlan {
    PlayerKey player;
    Vec3 root_goal{}, target_velocity{}, hand_goal{};
    std::array<float,4> heading{0,0,0,1};
    float steering{}; // live input reduces automatic yaw authority
    bool ragdoll{};   // the grip survived a bail: drag the ragdoll, no board yaw
};
class TowController {
public:
    std::optional<TowPlan> update(WorldKey, std::uint64_t local_id, std::uint64_t now_us,
        Vec3 local_root, bool playable, bool ragdoll, bool held, std::span<const TowCandidate> candidates,
        float steering=0, bool left_hand=false);
    // Record the grab input without the per-frame work. The idle gate in the
    // client tick skips update(), and the press edge (and the re-arm flag) must
    // survive those skipped ticks or a later press would never acquire.
    void observe(bool held) noexcept;
    void release(std::string_view reason) noexcept;
    bool attached() const noexcept { return attached_; }
    std::string_view status() const noexcept { return status_; }
    // Diagnostics of the last acquisition press (see TowDiag) and whether it
    // ended on the no-candidate release.
    const TowDiag& diag() const noexcept { return diag_; }
    bool no_candidate() const noexcept { return no_candidate_; }
private:
    bool attached_{}, previous_held_{}, needs_release_{}, no_candidate_{};
    TowDiag diag_{};
    WorldKey world_{};
    PlayerKey target_{};
    float steering_offset_{};
    std::array<float,4> travel_heading_{0,0,0,1};
    Vec3 offset_{}, previous_hip_{}, velocity_{}, velocity_hip_{};
    std::uint64_t previous_time_{}, velocity_time_{};
    // Last fresh sample, held through a stale window (see tow_stale_grace_us).
    Vec3 last_hip_{};
    std::uint64_t last_seen_us_{};
    bool have_last_{};
    std::string_view status_ = "Ready: hold V or R1 near a player";
};
// Deadzone and normalize left-stick X; does not consume native game input.
float skitch_steering_axis(float normalized);
// Applies only a bounded horizontal velocity increment. Native Y velocity/gravity
// and the body's angular velocity remain owned by the game.
std::optional<Vec3> tow_velocity_delta(Vec3 root, Vec3 velocity, const TowPlan&, float dt);
// Two-bone reach with a downward elbow pole; nearest arm with selection hysteresis.
std::optional<bool> nearest_hand(std::span<const Joint> pose, Vec3 goal, std::optional<bool> previous={});
std::optional<bool> stance_hand(std::span<const Joint> pose, std::array<float,4> heading, std::optional<bool> previous={});
bool reach_hand(std::span<Joint> pose, Vec3 goal, bool left);
// Compatibility entry for right-arm tests.
bool reach_right_hand(std::span<Joint> pose, Vec3 goal);
// Rotate the native motion goal about world Y, preserving lean and translation.
bool turn_motion_target(std::array<float,16>& matrix, const TowPlan&, float dt);
// Bounded world yaw for the active physics callback, plus rigid assembly rotation.
std::optional<float> tow_yaw_step(const std::array<float,16>& current, const TowPlan&, float dt);
bool rotate_body_yaw(std::array<float,16>& matrix, Vec3 pivot, float angle);
} // namespace skateskitch
