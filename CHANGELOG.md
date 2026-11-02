# Changelog

All notable changes to LoadingFlow are documented here.
The project was called **PocketCell** until v1.0.0; that name is retired.

---

## [1.0.0] — 2026-11-03

First public release. The plugin was renamed from PocketCell to LoadingFlow and the whole
distribution was reworked around what the plugin actually does today.

### What it does

- **Hides the loading screen on load doors.** The Loading Menu's show request is answered with
  "ignore", so the menu is never created. Both `kShow` and `kReshow` are handled.
- **Trims the engine's own transition timers** at runtime: fade-to-black, normal/automatic door
  fades, the post-load fade-in minimum, and the post-load Papyrus wait.
- **Re-applies those settings after loading a save**, because the game re-reads its INI files on
  load and would otherwise restore the vanilla values.
- **Measures every transition** and writes one log line per phase, so the effect is verifiable
  instead of felt.
- **Optional model pre-warming** of nearby doors' destination cells, budgeted to small slices.
- **Fast travel and save loading keep their normal vanilla loading screens** through a short
  exemption window armed by closing the map or the main menu.

### What it deliberately does not do

- It does not copy cells, replace interiors, or intercept the teleport. Every transition is the
  engine's own, so pathing, NPCs, combat and quest events stay vanilla.
- It does not shorten the load itself. Measured on a ~3,800-plugin setup: the real load remains
  1.0–2.6 s.
- It does not hide the engine's scene-attach spin at the swap point.
- It does not touch teleports implemented by activators, scripts or trigger volumes by default.

### Distribution

- Single **FOMOD** installer. Two steps: game runtime, then optional Chinese language pack.
- Two builds ship in the one archive:
  - `dll_se16` — built against the unpatched CommonLibSSE-NG baseline; Skyrim **SE 1.5.97**
    (verified in game) and **AE 1.6.x**.
  - `dll_ae17` — built against the patched CommonLibSSE-NG that understands runtime version 7;
    adds **AE 1.7.x**. **Experimental, not verified in game.**

### Licensing

- **Dual licensed**: the default is source-available / all rights reserved (published for security
  audit only); the alternative is **GNU GPL v3.0**.
- The GPL option is not decorative: the pre-built binaries include the optional in-game menu, which
  links **SKSE Menu Framework 3 (GPL-3.0)**, so the binaries can only be distributed under GPL-3.0.
  The pre-built binaries are therefore offered exclusively under that option.
- License texts ship inside the archive under `licenses/` (a GPL-3.0 §4 requirement).
- Source, which doubles as the GPL Corresponding Source: https://github.com/RandyLuaW/LoadingFlow

### Removed before release

- The engine's own linked-interior preload switch (`bPreloadLinkedInteriors`) is not referenced by
  the plugin at all. Enabling it caused a permanent hang while loading a save, reproduced at two
  different preload caps, so it was removed rather than worked around.
- The loading-loop probe and every code-patching path it needed. The plugin now modifies **no engine
  machine code**; it only swaps two vtable entries and rewrites registered INI setting values.

### Known limitations

- The swap point still shows a fraction of a second of the engine assembling the new scene.
- Heavy interiors on large load orders still feel like a pause. The log reports how long.
- AE 1.6.x and AE 1.7.x builds are untested by the author.
- Do not install together with **Faster Loadscreens** — both rewrite the same engine fade settings.
