# ReAnimator

A mod pack for [ReSkate](https://github.com/Dingo-Shenanigans/ReSkate) that adds **custom animation
playback**, **player skitching** (hip towing), and **physics-driven ragdoll dragging** — built on top
of the ReSkate runtime so it works offline, in lobbies, and on dedicated servers.

> ReAnimator is a fan project. It is not affiliated with or endorsed by Electronic Arts or Full Circle.
> You need your own copy of **skate.** on Steam and a working [ReSkate](https://github.com/Dingo-Shenanigans/ReSkate) install.

---

## What's inside

| Mod | What it does |
|-----|--------------|
| **Animation** | Play custom animations on your skater in-game. Pose-layer system with auto/full/legs masks, recording, playback, and effect-attach helpers. Driven by the `poseanim` console command. |
| **Skitch** | Tow another player by their hip. Hold the Skitch key (default **V**, fully rebindable) or **LB+RB** near a player to grab on. Works on-foot and while ragdolled. Includes a damped 3D spring tether, hip-target selection, and a follow slot that tracks the leader's height. |
| **Ragdoll Drag** | When you bail, the skitch grip can *physically drag* your ragdoll using the engine's own FBPhysics — no snap-back, no invisible pusher. Your body is pulled by the grip like a hand tether. Toggle with `dragstate on\|off`. |
| **Unlockables** *(Testing branch only)* | All cosmetics, objects, neighborhoods, preset slots, and bus stops unlocked by default. |

---

## Branches

| Branch | Contents |
|--------|----------|
| **`main-testing`** | Full feature set + unlocks. All mods active, all gestures/cosmetics/build items accessible. This is the primary branch. |
| **`animation`** | v1.1.6 base + Animation mod only. No skitch, no unlocks. |
| **`skitch`** | v1.1.6 base + Skitch mod only (no ragdoll skitching). No animation, no unlocks. |
| **`merged`** | Animation + Skitch combined. No unlocks. |

---

## Installation

1. **Install ReSkate** (if you haven't already). Follow the
   [ReSkate guide](https://github.com/Dingo-Shenanigans/ReSkate#getting-started) to get the launcher
   and runtime set up with your Steam copy of skate.
2. **Download the ReAnimator DLL** for the branch you want (from the branch's `dist/` folder or a release).
3. **Drop `ReSkate.dll`** into your game folder (the same folder where the ReSkate launcher put the
   original DLL — typically alongside `Skate.exe`).
4. **Launch the game** through the ReSkate launcher as normal.

> **Note:** ReAnimator replaces the ReSkate runtime DLL. Keep a backup of your original if you want to
> switch back.

---

## Usage

### Skitching

- **Keyboard:** Hold **V** (or whatever key you bound) near a player.
- **Controller:** Hold **LB + RB** near a player.
- **Rebind the key:** Go to **Settings → Controls** in the in-game menu. The **Skitch keyboard key**
  binding sits directly below the Forward/Up Boost bindings. Click it, press any key, and it saves
  to your profile. You can also clear it to disable keyboard skitching (LB+RB still works).
- **Console:** `bind skitchkey <virtual_key>` (0 clears it).

### Animation

Open the console (`~` by default) and use `poseanim`:

| Command | What it does |
|---------|--------------|
| `poseanim test` | Play the loaded test animation |
| `poseanim record` | Start recording your skater's pose |
| `poseanim play` | Play back the recorded animation |
| `poseanim off` | Stop and clear the animation |
| `poseanim save <path>` | Save the current animation to a `.rska` file |
| `poseanim mask auto\|full\|legs` | Set the pose-layer mask |
| `poseanim trace 0\|1` | Toggle layer tracing (debug) |
| `poseanim <file>.rska` | Load and play an animation file |

### Ragdoll Drag

- Open the console and type `dragstate on` to enable physics-driven ragdoll dragging.
- `dragstate off` disables it and returns to normal ragdoll behavior.
- When enabled, bailing while skitching grips the other player and your ragdoll is pulled by the
  engine's FBPhysics — camera follows, no snap-back.

### Unlockables (Testing branch only)

All cosmetics, objects, neighborhoods, preset slots, and bus stops are unlocked by default on the
`main-testing` branch. No console commands needed.

---

## Building from source

ReAnimator is built with the ReSkate toolchain. From the repo root:

```powershell
.\build-runtime.ps1 -Version '1.1.6-skitch'
```

This compiles the runtime, runs the test suite (gesture ownership, pose layers, cosmetic
inventory), and stages `ReSkate.dll` into `dist/`.

---

## License

ReSkate is licensed under the GNU General Public License v3.0. ReAnimator inherits that license.
See [LICENSE](LICENSE) for details.
