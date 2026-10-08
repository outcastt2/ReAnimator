#pragma once
#include "hip_targets.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include <vector>

namespace skateskitch {
// Call while the session's PeerScope selects this player, after show_remote.
// Pass the final visible pose, not latest_root. Exclude throwdown relocation when
// only unmodified render_pose is available (use hidden_or_relocated). No native
// pointers are retained, and no engine memory is written.
inline std::optional<RemoteSample> remote_sample(
    const dingosdk::multiplayer::Pose& render_pose, PlayerKey player, WorldKey world,
    std::uint64_t arrival_us, bool visible, bool world_ready, bool hidden_or_relocated, bool echo,
    Vec3 hip_local_offset={}) {
    if (!visible || !world_ready || hidden_or_relocated || echo) return {};
    std::vector<Joint> joints;
    joints.reserve(render_pose.skater.size());
    for (const auto& t : render_pose.skater) joints.push_back({t.position,t.rotation,t.scale});
    const auto anchor=hip_anchor(joints,hip_local_offset);
    if (!anchor) return {};
    return RemoteSample{player,world,*anchor,arrival_us,true};
}
} // namespace skateskitch
