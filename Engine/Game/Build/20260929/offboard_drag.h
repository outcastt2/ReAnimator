#pragma once
#include "Engine/Game/Build/fingerprint.h"

namespace dingosdk::game::build::v20260929::offboard_drag {
// The wipeout motion states are animation-driven: every update recomputes the
// position from the offboard machine's context, which is why external writes to
// their fields are lost (Falling on the ground, FollowRagdoll and
// FollowAnimatedRagdoll in the air -- the animation publisher observes
// FollowAnimatedRagdoll as the default bail, Falling on the ground).
//
// The machine's method at 0x4822b80 (its vtable+0x10) calls the active
// substate's vtable+0x18, i.e. exactly these updates, with the same
// (state, machine) pair the dispatcher uses: rcx = the substate, rdx = the
// machine, whose +8 is the context, +0x48 the active substate and +0xd30 the
// published placement. A substate keeps its velocity at +0x10 and its position
// at +0x30 (three floats each). The state updates publish state+0x30 to
// machine+0xd30 with a 16-byte store, so that offset is the engine's own
// placement channel for a bailed skater.
//
// The machine embeds the whole follow family as members starting at +0x70; the
// active member pointer at +0x48 selects the behaviour. Member offsets and the
// vtable -> update pairs (verified against the binary):
//   +0x70  Base                  0x65e8c20
//   +0x1c0 FollowGround          0x65e8c80
//   +0x310 FollowGroundSlide     0x65e8ce0
//   +0x460 FollowAnimation       0x65e8d40
//   +0x5b0 FollowTrajectory      0x65e8da0 -> 0x4820860
//   +0x720 Falling               0x65e8df8 -> 0x4832c80
//   +0x8e0 FollowRagdoll         0x65e8e60 -> 0x4830a70
//   +0xaa0 FollowAnimatedRagdoll 0x65e8ed0 -> 0x48332f0
inline constexpr std::uintptr_t follow_trajectory_update_rva = 0x4820860;
inline constexpr std::array<unsigned char, 16> follow_trajectory_update_prologue{
    0x4c,0x8b,0xdc,0x53,0x56,0x57,0x41,0x55,0x48,0x81,0xec,0xa8,0x02,0x00,0x00,0xc4};
inline constexpr std::uintptr_t falling_update_rva = 0x4832c80;
inline constexpr std::array<unsigned char, 16> falling_update_prologue{
    0x48,0x8b,0xc4,0x53,0x56,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0xd8};
inline constexpr std::uintptr_t follow_ragdoll_update_rva = 0x4830a70;
inline constexpr std::array<unsigned char, 16> follow_ragdoll_update_prologue{
    0x48,0x8b,0xc4,0x53,0x56,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0xd8};
inline constexpr std::uintptr_t follow_animated_ragdoll_update_rva = 0x48332f0;
inline constexpr std::array<unsigned char, 16> follow_animated_ragdoll_update_prologue{
    0x48,0x8b,0xc4,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x81};
inline constexpr std::uintptr_t machine_context_offset = 0x8;
inline constexpr std::uintptr_t machine_active_offset = 0x48;
inline constexpr std::uintptr_t machine_publish_offset = 0xd30;
inline constexpr std::uintptr_t state_velocity_offset = 0x10;
inline constexpr std::uintptr_t state_position_offset = 0x30;
inline constexpr std::uintptr_t context_dt_offset = 0x17ec;
// The physics core stores the offboard machine at +0x3b0; its own vtable is
// 0x65e8f40 (the constructor sets it), and the ragdoll substates carry these
// vtables, whose +0x18 entry is the motion update the machine dispatches.
inline constexpr std::uintptr_t core_machine_offset = 0x3b0;
inline constexpr std::uintptr_t machine_vtable_rva = 0x65e8f40;
inline constexpr std::uintptr_t follow_trajectory_vtable_rva = 0x65e8da0;
inline constexpr std::uintptr_t falling_vtable_rva = 0x65e8df8;
inline constexpr std::uintptr_t follow_ragdoll_vtable_rva = 0x65e8e60;
inline constexpr std::uintptr_t follow_animated_ragdoll_vtable_rva = 0x65e8ed0;
// The ragdoll motion wrapper: all three ragdoll states call it directly, right
// after they recompute their velocity and before it is integrated into the
// position, so a velocity written there is the one the engine's own
// integration consumes and publishes. Its inputs are a by-value block on the
// stack, so a normal detour cannot forward it -- relay it with a thunk that
// only preserves the registers and jumps to the original.
inline constexpr std::uintptr_t ragdoll_motion_wrapper_rva = 0x477fce0;
inline constexpr std::array<unsigned char, 16> ragdoll_motion_wrapper_prologue{
    0x48,0x81,0xec,0x88,0x00,0x00,0x00,0x0f,0xb6,0x84,0x24,0x00,0x01,0x00,0x00,0xc5};
// The velocity dispatcher every ragdoll state update calls right after it
// recomputes its velocity and right before the motion wrapper integrates it:
// state = rcx, machine = rdx, and the velocity destination (state+0x10) is the
// fifth argument. A normal detour forwards it, so a velocity added here is
// consumed by the engine's own integration -- the one write window the ragdoll
// motion has.
inline constexpr std::uintptr_t ragdoll_velocity_dispatch_rva = 0x482cf10;
inline constexpr std::array<unsigned char, 32> ragdoll_velocity_dispatch_prologue{
    0x40,0x53,0x48,0x81,0xec,0xe0,0x00,0x00,0x00,0x48,0x8b,0xda,0x4d,0x8b,0xd1,0x48,
    0x8b,0x52,0x08,0x8b,0x8a,0xc0,0x09,0x00,0x00,0x0f,0xba,0xe1,0x0d,0x0f,0x83,0xa4};
} // namespace dingosdk::game::build::v20260929::offboard_drag
