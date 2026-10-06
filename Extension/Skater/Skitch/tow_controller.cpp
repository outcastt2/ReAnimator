#include "tow_controller.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace skateskitch {
namespace {
using Q = std::array<float,4>;
Vec3 sub(Vec3 a, Vec3 b) { for(int i=0;i<3;++i) a[i]-=b[i]; return a; }
Vec3 sum(Vec3 a, Vec3 b) { for(int i=0;i<3;++i) a[i]+=b[i]; return a; }
Vec3 mul(Vec3 a,float s) { for(auto& v:a) v*=s; return a; }
float dot(Vec3 a,Vec3 b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
Vec3 cross(Vec3 a,Vec3 b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
float length(Vec3 a) { return std::sqrt(dot(a,a)); }
bool valid(Vec3 a) { for(auto v:a) if(!std::isfinite(v)||std::abs(v)>1000000) return false; return true; }
Vec3 cap(Vec3 a,float n) { const auto l=length(a); return l>n ? mul(a,n/l) : a; }
Q product(Q a,Q b) {
    return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
            a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
            a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
            a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
}
Q conjugate(Q q) { return {-q[0],-q[1],-q[2],q[3]}; }
std::optional<Q> unit(Q q) {
    double n=0; for(float v:q) { if(!std::isfinite(v)) return {}; n+=double(v)*v; }
    if(n<1e-8 || n>1e8) return {};
    for(auto& v:q) v=static_cast<float>(v/std::sqrt(n));
    return q;
}
Vec3 rotate(Q q,Vec3 v) { const auto t=mul(cross({q[0],q[1],q[2]},v),2); return sum(v,sum(mul(t,q[3]),cross({q[0],q[1],q[2]},t))); }
// Heading uses only yaw so a kickflip does not invert the towing offset.
std::optional<Q> yaw(Q rotation) {
    const auto q=unit(rotation); if(!q) return {};
    const auto forward=rotate(*q,{0,0,1});
    if(forward[0]*forward[0]+forward[2]*forward[2]<0.01f) return {};
    const float a=std::atan2(forward[0],forward[2])*0.5f;
    return Q{0,std::sin(a),0,std::cos(a)};
}
struct Frame { Vec3 position{}; Q rotation{0,0,0,1}; float scale=1; };
constexpr std::array<unsigned,12> right_chain{0,1,7,42,43,44,45,46,47,48,49,50};
constexpr std::array<unsigned,12> left_chain{0,1,7,42,43,44,45,275,276,277,278,283};
std::optional<std::array<Frame,12>> frames(std::span<const Joint> pose,bool left=false) {
    const auto& chain=left ? left_chain : right_chain;
    std::array<Frame,12> output;
    Frame parent;
    for(std::size_t i=0;i<chain.size();++i) {
        const auto& j=pose[chain[i]];
        const auto q=unit(j.rotation);
        if(!q || !valid(j.position) || !valid(j.scale) || j.scale[0]<0.01f || j.scale[0]>20 ||
            std::abs(j.scale[0]-j.scale[1])>0.001f || std::abs(j.scale[0]-j.scale[2])>0.001f) return {};
        Frame f{sum(parent.position,rotate(parent.rotation,mul(j.position,parent.scale))),
                product(parent.rotation,*q), parent.scale*j.scale[0]};
        if(!valid(f.position)||!std::isfinite(f.scale)) return {};
        output[i]=f; parent=f;
    }
    return output;
}
}
void TowController::observe(bool held) noexcept {
    previous_held_=held;
    if(!held) { attached_=false; needs_release_=false; status_="Released; normal skating"; }
}
void TowController::release(std::string_view reason) noexcept {
    attached_=false; needs_release_=true; velocity_={}; status_=reason;
}
std::optional<TowPlan> TowController::update(WorldKey world,std::uint64_t local_id,std::uint64_t now,
    Vec3 root,bool playable,bool ragdoll,bool held,std::span<const TowCandidate> candidates,float steering,bool left_hand) {
    steering=std::isfinite(steering) ? std::clamp(steering,-1.f,1.f) : 0;
    const bool pressed=held&&!previous_held_; previous_held_=held;
    if(!held) { attached_=false; needs_release_=false; status_="Released; normal skating"; return {}; }
    if(!valid(root) || !world.session || !local_id) { release("Unavailable; release grab to rearm"); return {}; }
    // A ragdoll keeps an existing grip and can also reach for a leader: the
    // press works the same floored or riding. Only walking on foot (neither
    // flag) releases and re-arms.
    if(!playable && !ragdoll) { release("Unavailable; release grab to rearm"); return {}; }
    if(attached_ && world!=world_) { release("World changed; released"); return {}; }
    const auto eligible=[&](const TowCandidate& c) {
        return c.sample.eligible && c.sample.world==world && c.sample.player.id && c.sample.player.id!=local_id &&
            c.sample.player.epoch && c.sample.player.generation && c.sample.received_us<=now &&
            now-c.sample.received_us<=250000 && valid(c.sample.hip.position) && yaw(c.heading).has_value();
    };
    const TowCandidate* target=nullptr;
    bool stale_hold=false;
    if(attached_) {
        for(const auto& c:candidates) if(c.sample.player==target_ && eligible(c)) { target=&c; break; }
        if(!target) {
            // The leader's pose stream drops for a beat far more often than the
            // player leaves. Keep the grip driving toward the last known hip
            // through that window instead of releasing on every gap.
            if(have_last_ && now>=last_seen_us_ && now-last_seen_us_<=tow_stale_grace_us) stale_hold=true;
            else { release("Target disappeared or became stale; released"); return {}; }
        }
    } else {
        if(!pressed || needs_release_) return {};
        float best=2.6f;
        for(const auto& c:candidates) if(eligible(c)) {
            const float d=length(sub(c.sample.hip.position,root));
            if(d<best) { best=d; target=&c; }
        }
        if(!target) { release("No player within 2.6 m; release grab and try again"); return {}; }
        attached_=true; world_=world; target_=target->sample.player;
        // A consistent rear-left follow slot avoids riding directly into the
        // leader, including grabs acquired from the side or while overlapping.
        // Bounded physics correction eases into the slot without teleporting.
        offset_={left_hand ? -tow_follow_left : tow_follow_left,0,-tow_follow_back}; steering_offset_=0;
        travel_heading_=*yaw(target->heading);
        // Resolve a backwards pose root toward the approached hip on acquisition.
        auto approach=sub(target->sample.hip.position,root); approach[1]=0;
        if(length(approach)>.1f && dot(rotate(travel_heading_,{0,0,1}),approach)<0)
            travel_heading_=product(Q{0,1,0,0},travel_heading_);
        previous_hip_=target->sample.hip.position; previous_time_=now; velocity_={};
        velocity_hip_=previous_hip_; velocity_time_=now;
        last_hip_=previous_hip_; last_seen_us_=now; have_last_=true;
        status_="Attached";
    }
    const auto hip=target ? target->sample.hip.position : last_hip_;
    if(now<previous_time_) { release("Clock changed; released"); return {}; }
    const auto elapsed=now-previous_time_;
    if(elapsed>250000) { release("Frame gap; released"); return {}; }
    // A fixed five-metre per-frame limit mistakes normal high-speed movement
    // during a longer frame for a teleport. Allow travel proportional to time,
    // plus a small network correction, but still reject a large position jump.
    const float travel_limit=std::min(35.f,4.f+tow_max_target_speed*static_cast<float>(elapsed)*1e-6f);
    if(length(sub(hip,previous_hip_))>travel_limit) { release("Target teleported; released"); return {}; }
    if(length(sub(hip,root))>tow_max_separation) { release("Tether separation exceeded 24 m; released"); return {}; }
    if(now-velocity_time_>=50000 && !stale_hold) {
        // Measure across a network-sized window. Per-frame derivatives of a
        // bursty pose alternated between zero and several times actual speed;
        // capping those spikes biased the estimate and let the rider fall behind.
        const float dt=static_cast<float>(now-velocity_time_)*1e-6f;
        auto observed=mul(sub(hip,velocity_hip_),1/dt); observed[1]=0;
        const float weight=1-std::exp(-dt*35);
        velocity_=cap(sum(mul(velocity_,1-weight),mul(observed,weight)),tow_max_target_speed);
        velocity_hip_=hip; velocity_time_=now;
    }
    // Allow the rider to choose a lateral lane while staying tethered. Smooth
    // the lane change; native controller input still reaches skating physics.
    const float steering_dt=static_cast<float>(elapsed)*1e-6f;
    steering_offset_+=(steering-steering_offset_)*(1-std::exp(-4.f*steering_dt));
    previous_hip_=hip; previous_time_=now;
    if(!stale_hold) { last_hip_=hip; last_seen_us_=now; have_last_=true; }
    // Travel, rather than stance-dependent pose/root yaw, owns the follow lane.
    // Keep the last direction below walking speed to avoid jitter at rest.
    if(length(velocity_)>1.f) {
        const float half=std::atan2(velocity_[0],velocity_[2])*.5f;
        travel_heading_={0,std::sin(half),0,std::cos(half)};
    }
    // Mirror the neutral lane with the chosen reaching arm. Ease across on
    // stance changes instead of snapping through the leader's model.
    const float side=left_hand ? -tow_follow_left : tow_follow_left;
    offset_[0]+=(side-offset_[0])*(1-std::exp(-4.f*steering_dt));
    auto slot=offset_; slot[0]-=.65f*steering_offset_;
    auto goal=sum(hip,rotate(travel_heading_,slot)); goal[1]=root[1];
    auto heading=travel_heading_;
    // Aim each segment at the player immediately ahead. Position lag naturally
    // delays a corner through the line rather than turning every rider at once.
    // Aim along the follow lane, not diagonally across the lateral gap. This
    // keeps neutral riders parallel on straights rather than accumulating a
    // leftward yaw in every segment of a long chain.
    const auto lane_anchor=sum(hip,rotate(travel_heading_,{slot[0],0,0}));
    const auto ahead=sub(lane_anchor,root);
    if(std::hypot(ahead[0],ahead[2])>.25f) {
        const float half=std::atan2(ahead[0],ahead[2])*.5f;
        heading={0,std::sin(half),0,std::cos(half)};
    }
    // A modest steering bias and reduced yaw authority let native steering
    // influence orientation instead of immediately correcting it away.
    const float bias=-steering_offset_*.45f*.5f;
    heading=product(Q{0,std::sin(bias),0,std::cos(bias)},heading);
    status_=ragdoll ? "Bail: holding on" : "Attached";
    return TowPlan{target_,goal,velocity_,hip,heading,steering,ragdoll};
}
float skitch_steering_axis(float normalized) {
    if(!std::isfinite(normalized)) return 0;
    const float value=std::clamp(normalized,-1.f,1.f);
    if(std::abs(value)<=.2f) return 0;
    return std::copysign((std::abs(value)-.2f)/.8f,value);
}
std::optional<Vec3> tow_velocity_delta(Vec3 root,Vec3 v,const TowPlan& plan,float dt) {
    if(!valid(root)||!valid(v)||!valid(plan.root_goal)||!valid(plan.target_velocity)||
        !std::isfinite(dt)||dt<=0||dt>0.1f) return {};
    auto error=sub(plan.root_goal,root); error[1]=0;
    if(length(error)>tow_max_separation) return {};
    auto mismatch=sub(plan.target_velocity,v); mismatch[1]=0;
    // Strong catch-up only when the target is outrunning the rider; near the
    // goal the same controller settles smoothly without changing gravity.
    const float limit=std::clamp(60.f+length(mismatch)*14.f,60.f,tow_max_acceleration);
    auto acceleration=cap(sum(mul(error,32),mul(mismatch,18)),limit);
    auto delta=mul(acceleration,dt); delta[1]=0;
    auto result=sum(v,delta); result[1]=0;
    if(length(result)>tow_max_rider_speed) return {};
    return delta;
}
bool turn_motion_target(std::array<float,16>& matrix,const TowPlan& plan,float dt) {
    if(!std::isfinite(dt)||dt<=0||dt>.1f) return false;
    for(const auto v:matrix) if(!std::isfinite(v)) return false;
    const auto desired=yaw(plan.heading); if(!desired) return false;
    // Engine transforms store right/up/forward vectors in consecutive rows.
    const float horizontal=std::hypot(matrix[8],matrix[10]);
    if(horizontal<.1f) return false;
    const auto f=rotate(*desired,{0,0,1});
    const float error=std::remainder(std::atan2(f[0],f[2])-std::atan2(matrix[8],matrix[10]),6.28318530718f);
    if(!std::isfinite(plan.steering)) return false;
    const float authority=1-.75f*std::clamp(std::abs(plan.steering),0.f,1.f);
    const float rate=tow_turn_rate*authority;
    const float angle=std::clamp(error*(1-std::exp(-tow_turn_response*authority*dt)),-rate*dt,rate*dt);
    const float c=std::cos(angle),s=std::sin(angle);
    for(unsigned row=0;row<3;++row) {
        const auto x=matrix[row*4],z=matrix[row*4+2];
        matrix[row*4]=c*x+s*z; matrix[row*4+2]=-s*x+c*z;
    }
    return true;
}
std::optional<float> tow_yaw_step(const std::array<float,16>& current,const TowPlan& plan,float dt) {
    auto next=current;
    auto stance_plan=plan;
    const auto desired=yaw(plan.heading); if(!desired) return {};
    const auto forward=rotate(*desired,{0,0,1});
    if(forward[0]*current[8]+forward[2]*current[10]<0)
        stance_plan.heading=product(Q{0,1,0,0},*desired);
    if(!turn_motion_target(next,stance_plan,dt)) return {};
    return std::remainder(std::atan2(next[8],next[10])-std::atan2(current[8],current[10]),6.28318530718f);
}
bool rotate_body_yaw(std::array<float,16>& matrix,Vec3 pivot,float angle) {
    if(!valid(pivot)||!std::isfinite(angle)||std::abs(angle)>.3501f) return false;
    for(unsigned row=0;row<3;++row) {
        const Vec3 axis{matrix[row*4],matrix[row*4+1],matrix[row*4+2]};
        if(!valid(axis)||std::abs(dot(axis,axis)-1)>.02f) return false;
    }
    const Vec3 position{matrix[12],matrix[13],matrix[14]};
    if(!valid(position)||length(sub(position,pivot))>8) return false;
    const float c=std::cos(angle),s=std::sin(angle);
    for(unsigned row=0;row<3;++row) {
        const auto x=matrix[row*4],z=matrix[row*4+2];
        matrix[row*4]=c*x+s*z; matrix[row*4+2]=-s*x+c*z;
    }
    const float x=position[0]-pivot[0],z=position[2]-pivot[2];
    matrix[12]=pivot[0]+c*x+s*z; matrix[14]=pivot[2]-s*x+c*z;
    // SIMD W lanes can contain native metadata, including NaN sentinels.
    // Preserve them, along with height; only XYZ bases/position are geometry.
    return true;
}
std::optional<bool> nearest_hand(std::span<const Joint> pose,Vec3 goal,std::optional<bool> previous) {
    if(pose.size()!=395 || !valid(goal)) return {};
    const auto right=frames(pose),left=frames(pose,true);
    if(!right || !left) return {};
    const float left_distance=length(sub((*left)[8].position,goal));
    const float right_distance=length(sub((*right)[8].position,goal));
    // Change sides for a real stance/geometry change, not tiny pose jitter.
    if(previous && std::abs(left_distance-right_distance)<.15f) return *previous;
    return left_distance<right_distance;
}
std::optional<bool> stance_hand(std::span<const Joint> pose,Q heading,std::optional<bool> previous) {
    const auto direction=yaw(heading); const auto hip=hip_anchor(pose);
    if(!direction || !hip) return {};
    // Use the forward shoulder so the arm reaches ahead rather than across
    // the torso. Follow-side mapping is independent of this arm choice.
    const auto ahead=sum(hip->position,rotate(*direction,{0,0,2}));
    return nearest_hand(pose,ahead,previous);
}
bool reach_hand(std::span<Joint> pose,Vec3 goal,bool left) {
    if(pose.size()!=395 || !valid(goal)) return false;
    const auto& chain=left ? left_chain : right_chain;
    const auto initial=frames(pose,left); if(!initial) return false;
    const auto shoulder=(*initial)[8].position;
    const float upper=length(sub((*initial)[9].position,shoulder));
    const float lower=length(sub((*initial)[10].position,(*initial)[9].position));
    if(upper<.1f||upper>.8f||lower<.1f||lower>.8f) return false;
    auto direction=sub(goal,shoulder); const float requested=length(direction);
    if(requested<.0001f) return false;
    direction=mul(direction,1/requested);
    const float reach=std::clamp(requested,std::abs(upper-lower)+.001f,(upper+lower)*.98f);
    goal=sum(shoulder,mul(direction,reach));
    // Stable world-down elbow pole avoids the unconstrained CCD elbow flipping
    // over the shoulder when the native animation mirrors the rider's stance.
    auto pole=sub(Vec3{0,-1,0},mul(direction,dot({0,-1,0},direction)));
    if(length(pole)<.01f) {
        const auto forward=rotate((*initial)[7].rotation,{0,0,1});
        pole=sub(forward,mul(direction,dot(forward,direction)));
    }
    if(length(pole)<.0001f) return false;
    pole=mul(pole,1/length(pole));
    const float along=(upper*upper-lower*lower+reach*reach)/(2*reach);
    const float bend=std::sqrt(std::max(0.f,upper*upper-along*along));
    const auto elbow=sum(shoulder,sum(mul(direction,along),mul(pole,bend)));
    const auto old_upper=pose[chain[8]].rotation,old_lower=pose[chain[9]].rotation;
    const auto align=[&](unsigned joint,Vec3 wanted) {
        const auto f=frames(pose,left); if(!f) return false;
        auto from=sub((*f)[joint+1].position,(*f)[joint].position);
        auto to=sub(wanted,(*f)[joint].position);
        if(length(from)<.0001f||length(to)<.0001f) return false;
        from=mul(from,1/length(from)); to=mul(to,1/length(to));
        const float cosine=std::clamp(dot(from,to),-1.f,1.f);
        Q delta;
        if(cosine<-.9999f) {
            auto axis=cross(from,pole);
            if(length(axis)<.0001f) axis=cross(from,std::abs(from[0])<.9f ? Vec3{1,0,0} : Vec3{0,1,0});
            axis=mul(axis,1/length(axis)); delta={axis[0],axis[1],axis[2],0};
        } else {
            const auto axis=cross(from,to);
            const auto q=unit(Q{axis[0],axis[1],axis[2],1+cosine}); if(!q) return false;
            delta=*q;
        }
        const auto parent=(*f)[joint-1].rotation;
        const auto q=unit(product(product(product(conjugate(parent),delta),parent),pose[chain[joint]].rotation));
        if(!q) return false;
        pose[chain[joint]].rotation=*q; return true;
    };
    if(!align(8,elbow)||!align(9,goal)) {
        pose[chain[8]].rotation=old_upper; pose[chain[9]].rotation=old_lower; return false;
    }
    return true;
}
bool reach_right_hand(std::span<Joint> pose,Vec3 goal) { return reach_hand(pose,goal,false); }
} // namespace skateskitch
