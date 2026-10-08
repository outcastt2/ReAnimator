#include "hip_targets.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace skateskitch {
namespace {
bool finite(Vec3 v) {
    return std::all_of(v.begin(), v.end(), [](float x) { return std::isfinite(x) && std::abs(x) < 1000000.0f; });
}
Vec3 add(Vec3 a, Vec3 b) { for (int i=0; i<3; ++i) a[i]+=b[i]; return a; }
Vec3 transform_vector(const Anchor& a, Vec3 v) {
    Vec3 result{};
    for (int i=0; i<3; ++i) for (int j=0; j<3; ++j) result[i]+=a.basis[j][i]*v[j];
    return result;
}
double distance2(Vec3 a, Vec3 b) {
    double result=0;
    for (int i=0; i<3; ++i) { const double d=double(a[i])-b[i]; result+=d*d; }
    return result;
}
bool valid(const Anchor& a) {
    if (!finite(a.position)) return false;
    for (auto v : a.basis) if (!finite(v)) return false;
    // Reject singular anchors before handing them to an eventual physics target.
    const auto& x=a.basis[0]; const auto& y=a.basis[1]; const auto& z=a.basis[2];
    const double determinant=double(x[0])*(double(y[1])*z[2]-double(y[2])*z[1])-
        double(y[0])*(double(x[1])*z[2]-double(x[2])*z[1])+
        double(z[0])*(double(x[1])*y[2]-double(x[2])*y[1]);
    return std::isfinite(determinant) && determinant>1e-9;
}
std::optional<Anchor> joint_frame(const Joint& joint) {
    if (!finite(joint.position) || !finite(joint.scale)) return {};
    for (float s : joint.scale) if (s<0.001f || s>20.0f) return {};
    double length2=0;
    for (float q : joint.rotation) { if (!std::isfinite(q)) return {}; length2+=double(q)*q; }
    if (length2<1e-8 || length2>1e8) return {};
    const double inv=1.0/std::sqrt(length2);
    const double x=joint.rotation[0]*inv, y=joint.rotation[1]*inv,
                 z=joint.rotation[2]*inv, w=joint.rotation[3]*inv;
    Anchor a;
    a.position=joint.position;
    a.basis={{{float(1-2*(y*y+z*z)), float(2*(x*y+w*z)), float(2*(x*z-w*y))},
              {float(2*(x*y-w*z)), float(1-2*(x*x+z*z)), float(2*(y*z+w*x))},
              {float(2*(x*z+w*y)), float(2*(y*z-w*x)), float(1-2*(x*x+y*y))}}};
    for (int j=0; j<3; ++j) for (auto& v : a.basis[j]) v*=joint.scale[j];
    return valid(a) ? std::optional(a) : std::nullopt;
}
}
std::optional<Anchor> hip_anchor(std::span<const Joint> joints, Vec3 offset) {
    // Asset-verified AnimBase_Default_Skeleton: Reference -> AITrajectory -> Hips.
    // AITrajectory already carries world placement. Do not multiply Pose.root again.
    if (joints.size()!=395 || !finite(offset)) return {};
    Anchor result;
    for (const auto index : {0u, 1u, 7u}) {
        const auto local=joint_frame(joints[index]);
        if (!local) return {};
        Anchor next;
        next.position=add(result.position, transform_vector(result, local->position));
        for (int i=0; i<3; ++i) next.basis[i]=transform_vector(result, local->basis[i]);
        if (!valid(next)) return {};
        result=next;
    }
    result.position=add(result.position, transform_vector(result, offset));
    return valid(result) ? std::optional(result) : std::nullopt;
}
HipTargets::HipTargets(TargetBackend& backend, TargetPolicy policy) : backend_(backend), policy_(policy) {
    if (!policy.stale_us || !std::isfinite(policy.teleport_distance) || policy.teleport_distance<=0)
        throw std::invalid_argument("Invalid hip target policy");
}
HipTargets::~HipTargets() { clear(); }
void HipTargets::clear() noexcept {
    for (auto& [key, entry] : entries_) backend_.destroy(entry.handle);
    entries_.clear();
    last_sync_.reset();
}
void HipTargets::sync(WorldKey world, std::uint64_t local_id, std::uint64_t now,
                      std::span<const RemoteSample> samples) {
    if (world!=world_ || (last_sync_ && now<*last_sync_)) { clear(); world_=world; }
    last_sync_=now;
    // An identity collision invalidates every sample for that player this frame.
    std::map<std::uint64_t, unsigned> counts;
    for (const auto& s : samples) ++counts[s.player.id];
    std::map<PlayerKey, const RemoteSample*> accepted;
    for (const auto& s : samples) {
        if (!world.session || !local_id || !s.player.id || s.player.id==local_id ||
            !s.player.epoch || !s.player.generation || s.world!=world || !s.eligible ||
            counts[s.player.id]!=1 || s.received_us>now || now-s.received_us>policy_.stale_us || !valid(s.hip)) continue;
        accepted.emplace(s.player, &s);
    }
    // Destroy retired generations before creating their replacements.
    for (auto it=entries_.begin(); it!=entries_.end();) {
        if (!accepted.contains(it->first)) { backend_.destroy(it->second.handle); it=entries_.erase(it); }
        else ++it;
    }
    for (const auto& [key, sample] : accepted) {
        auto existing=entries_.find(key);
        if (existing!=entries_.end()) {
            if (distance2(existing->second.hip.position,sample->hip.position)>
                double(policy_.teleport_distance)*policy_.teleport_distance ||
                !backend_.move(existing->second.handle,sample->hip)) {
                backend_.destroy(existing->second.handle);
                entries_.erase(existing);
                // Leave one frame without a proxy after a discontinuity or backend failure.
                continue;
            }
            existing->second.hip=sample->hip;
        } else if (const auto handle=backend_.create(key,sample->hip); handle && *handle) {
            entries_.emplace(key,Entry{*handle,sample->hip});
        }
    }
}
std::optional<ProxyHandle> HipTargets::find(PlayerKey key) const {
    const auto it=entries_.find(key);
    return it==entries_.end() ? std::nullopt : std::optional(it->second.handle);
}
std::optional<PlayerKey> HipTargets::nearest(Vec3 position, float radius) const {
    if (!finite(position) || !std::isfinite(radius) || radius<0) return {};
    double best=double(radius)*radius;
    std::optional<PlayerKey> result;
    for (const auto& [key,entry] : entries_) {
        const auto d=distance2(position,entry.hip.position);
        if (d<=best) { if (!result || d<best) { best=d; result=key; } }
    }
    return result;
}
} // namespace skateskitch
