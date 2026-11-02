# LoadingFlow

**Smoother loading when you walk through doors and interiors.**

[English](./README.md) | [简体中文](./README.zh-CN.md)

[![License](https://img.shields.io/badge/license-Dual%20(source--available%20%7C%20GPL--3.0)-red)](./LICENSE)
[![Source](https://img.shields.io/badge/source-audit%20only-black)](./LICENSE)
![Game](https://img.shields.io/badge/Skyrim-SE%201.5.97%20%7C%20AE%201.6.x%20%7C%20AE%201.7.x(experimental)-blue)
![Runtime](https://img.shields.io/badge/SKSE-2.0.20%2B-blue)

An SKSE plugin for Skyrim SE/AE. It removes the loading screen from load doors and trims a few of
the engine's own transition timers. Every door still performs the engine's real cell transition, so
pathing, NPCs, combat, encounters and quest events behave exactly as they do without this plugin.

> The source in this repository is published **for security audit only**. It is not open source:
> modification and redistribution are not granted under the default option. See [LICENSE](./LICENSE).
> A GPL-3.0 alternative exists, and the pre-built binaries are distributed under it.

---

## What it does

| Module | How it works | What you get |
|---|---|---|
| **Loading screen removal** | Hooks `LoadingMenu::ProcessMessage` and answers the engine's `kShow` / `kReshow` requests with `kIgnore` | The loading menu is never created on door transitions. Fast travel and save loading keep their normal vanilla loading screens. |
| **Transition timer trimming** | Rewrites registered INI setting values at runtime: `fFadeToBlackFadeSeconds`, `fNormalDoorFadeSecs`, `fMinSecondsForLoadFadeIn`, `fPostLoadUpdateTimeMS` and related keys | The pre-load wait after pressing E drops from the stock **1002–1022 ms** to **694–734 ms** (variance under 5%). `fPostLoadUpdateTimeMS` goes from this machine's **2000 ms** to **100 ms**. |
| **Model pre-warming** (optional) | When you approach a load door, calls `BSModelDB::Demand` on the destination cell's models, budgeted to 8 ms per slice | Model I/O is spread over the walk to the door instead of all landing on the doorpress. Measured effect on total load time: neutral. |
| **Per-phase timing log** | Records timestamps at three points: pressing E → the engine requesting the menu → the cell swap completing | Emits the total `N` plus the pre-load and load phases separately, so the bottleneck is measurable instead of guessed. |

---

## Design principles

- **The real cell transition is kept.** Nothing is copied, no level data is edited, no teleport is
  intercepted. This is what keeps NPC pathing, combat and quest events vanilla.
- **No engine machine code is modified.** The whole plugin only swaps two vtable entries and rewrites
  INI setting values that are already registered with the engine. It does not patch instructions, and
  it does not use the SKSE trampoline.
- **Settings are re-applied after loading a save**, because the game re-reads its INI files on load and
  would otherwise restore the stock values.
- **Built on CommonLibSSE-NG** (with spdlog). The optional in-game settings menu requires
  `-DWITH_MENU=ON` and SKSE Menu Framework 3.

---

## What it does NOT do

Read this before installing, so your expectations match reality.

- **It does not make loading instant.** On the machine this was developed and measured on (about
  3,800 plugins), the real load between the engine requesting the menu and the cell swap finishing
  still takes **1019–2577 ms**. That is the physical cost of that load order; a plugin cannot
  compress it.
- **It does not hide the scene assembly at the swap.** The engine briefly spins and scales the new
  scene into place. Only a black screen could cover that, and a black screen conflicts with "see the
  room immediately", which is what this plugin chooses.
- **It does not touch every kind of teleport.** By default only `TESObjectDOOR` activation is
  handled. Transitions implemented with activators plus script `MoveTo`, or with trigger volumes, are
  left alone. An optional switch (`bGlobalMenuSuppression`) widens the scope, but fast travel and
  save loading are always exempt.
- **It is not an open-world seamless mod.** Interiors remain separate cells.

---

## Measured numbers

Same door pair, full restart before each group:

| Phase | Before | After |
|---|---|---|
| Press E → engine requests the loading menu | 1002–1022 ms (stock `fFadeToBlackFadeSeconds` = 1.0) | **694–734 ms** |
| Engine requests menu → cell swap done | — | 1019–2577 ms (workload-bound, not compressible here) |
| `fPostLoadUpdateTimeMS` | **2000 ms** on this machine | **100 ms** |

Do not compare successive visits to the same door: the load phase falls every time the files are
already in the OS cache. Only the first transition of a run, or the same position across two runs,
is comparable.

---

## Build

Requirements: Windows, Visual Studio 2022, CMake ≥ 3.21, and
[CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) (with spdlog).

```bat
cmake -S . -B build -DCommonLibSSE_DIR=<path to CommonLibSSEConfig.cmake>
cmake --build build --config Release
```

Output: `build/Release/LoadingFlow.dll`.

- `-DWITH_MENU=ON` builds the in-game settings menu. It needs `SKSEMenuFramework.h` from
  [SKSE Menu Framework 3](https://github.com/QTR-Modding/SKSE-Menu-Framework-3) in `include/` —
  that third-party header is deliberately **not** vendored here.
- `-DCLIBSSE_SRC=<dir>` also strips a local CommonLibSSE checkout out of the binary's paths.

---

## Installation

Install the FOMOD from the release channel. It asks two things: which game runtime you are on
(SE 1.5.97 + AE 1.6.x, or AE 1.7.x experimental), and whether you want the Chinese language pack.

Remove any previous copy of this plugin first — two DLLs would hook the same engine function twice.

---

## Known boundaries (already investigated, do not retry)

| Direction | Conclusion |
|---|---|
| Engine's own linked-interior preload (`bPreloadLinkedInteriors`) | **Enabling it hangs save loading permanently**, reproduced at two different preload caps → the plugin no longer references it at all |
| Raising `iPreloadSizeLimit` | It is only a capacity cap, not a switch; raising it alone has no measurable effect |
| Dual game client / screenshot loading screen / transition vestibule / cloned scene | Rejected: world-state divergence, cannot cover the player model and differing FOV, invented geometry breaking spatial logic, and cascading release of engine resources |
| Covering the swap with an image-space black screen | Does not work: `FadeToBlackImod` only affects the world render, the UI/Scaleform layer is composited above it |

---

## License

**Dual licensed:**

1. **Default — source-available, all rights reserved.** The source is published so it can be read and
   audited. Modification, derivative works, redistribution and commercial use are not granted.
2. **Alternative — GNU GPL v3.0.** You may instead use this work under GPL-3.0
   ([full text](./LICENSE-GPL-3.0)).

**Why the second option exists:** the pre-built plugin includes the optional in-game menu, which links
SKSE Menu Framework 3 (GPL-3.0). Distributing that binary is therefore only lawful under GPL-3.0, and
the pre-built binaries are offered **exclusively under the GPL-3.0 option**. Builds produced with
`-DWITH_MENU=OFF` contain no GPL-3.0 code and are covered only by the default option.

This repository is also the GPL Corresponding Source for those binaries.

Third-party components and their licenses: [THIRD-PARTY-NOTICES.txt](./THIRD-PARTY-NOTICES.txt).

---

## Credits

- [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) — MIT
- [spdlog](https://github.com/gabime/spdlog) — MIT
- [SKSE Menu Framework 3](https://github.com/QTR-Modding/SKSE-Menu-Framework-3) — GPL-3.0 (in-game menu)
- [Faster Loadscreens](https://www.nexusmods.com/skyrimspecialedition/mods/182852) by 1001Bits — for
  publishing the stock values of these engine settings
- [Keizaal Fast Cell Transition](https://www.nexusmods.com/skyrimspecialedition/mods/196720) — for
  documenting the post-load fade behaviour
- SKSE team and the Address Library maintainers

Provided as is, without warranty. Back up your saves. Not affiliated with Bethesda Softworks or the
SKSE team.
