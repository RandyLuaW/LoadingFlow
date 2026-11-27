#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// IMPORTANT (P2-9): Keep <RE/Skyrim.h> BEFORE "SKSEMenuFramework.h" (which includes <windows.h>).
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <spdlog/sinks/basic_file_sink.h>

#include "BlueprintBuilder.h"

#if defined(LOADINGFLOW_WITH_MENU)
#	include "SKSEMenuFramework.h"
#endif

// D3D11 types for the void overlay (v1.4.0). These MUST come after every CommonLib header:
// REX/W32/BASE.h refuses to compile once the Windows API has been pulled in, and <d3d11.h>
// drags in the whole world. Nothing here links d3d11.lib or d3dcompiler -- we only call COM
// methods on interfaces the renderer already owns, and the shaders are precompiled bytecode,
// so the DLL's import table stays system-DLL only (keep it that way: the audit checks it).
#include <d3d11.h>
#include <dxgi1_2.h>

#include "VoidOverlayShader.h"

// Banner shown in the log's first line. Bump this together with the release version --
// the packaging script asserts on it, and "How to verify it is working" tells users to
// check exactly this string.
#ifndef LOADINGFLOW_BANNER
#define LOADINGFLOW_BANNER "1.3.5"
#endif

namespace logger = SKSE::log;
using namespace std::literals;

namespace
{
    // =========================================================================
    //  Configuration (persisted to Data/SKSE/Plugins/LoadingFlow.ini)
    // =========================================================================
    struct Config
    {
        bool          enabled = true;
        bool          allowInteriorToInterior = true;
        bool          allowPocketToExterior = true;
        // P1-1: OFF by default for the first playtest. Pulling an NPC to Z=+26000 is only safe once
        // the return path has been verified in-game; until then an empty-looking shop beats an NPC
        // stranded at pocket altitude in a save you keep playing.
        bool          moveActorsToPocket = false;
        bool          spawnBlackBoxShell = true;
        bool          playDoorSoundAndAnim = true;
        bool          triggerBlinkImod = true;
        bool          sneakForcesVanillaLoad = true;
        bool          showDebugNotifications = false;
        // L2 split into two independent switches (post-research correction):
        //  - location change is well-formed: TESActorLocationChangeEvent carries actor + old/new
        //    location and matches Papyrus OnLocationChange(Location akOldLoc, Location akNewLoc).
        //  - cell attach/loaded is NOT demonstrably ref-scoped. Engine semantics per the Nexus
        //    "OnCellAttach / OnCellDetach" thread is "the CELL enters the world", and the SKSE event
        //    carries only {reference, attached} with no cell field -- if the VM interprets it
        //    cell-wide it would fire OnCellAttach on every ref of the player's loaded cell (all of
        //    Tamriel). Separate switch, default off, so the risky half is never enabled by accident.
        // Both fake-event switches are off by default: with real transitions everywhere the engine
        // raises these events itself, and faking them would only confuse other mods.
        bool          fakeLocationEvents = false;
        bool          fakeCellLoadEvents = false;
        // MASTER ROUTING SWITCH. 0 (default) = every door uses the engine's own cell transition, so
        // navmesh, AI packages, NPCs, combat, interior lighting and quest events are 100% vanilla;
        // LoadingFlow then only hides/masks the loading menu. 1 = the legacy high-altitude clone
        // (zero I/O, but no navmesh and no NPCs -- kept only for A/B experiments).
        bool          usePocketCopies = false;
        bool          suppressLoadingMenu = true;
        std::uint32_t suppressTimeoutMs = 6000;   // v1.3.4d: was 400; the INI template always said 6000, so deleting the key silently changed behaviour
        // How long after the door activation we still treat a LoadingMenu request as belonging to
        // THIS transition. Measured in-game: the engine requests the menu 0.6-1.0 s after Activate
        // (it fades and starts the load first), so a 400 ms window expired before the menu was even
        // asked for. suppressTimeoutMs keeps its own meaning: after we HAVE suppressed, how long we
        // wait for the swap before re-posting the menu.
        // v1.3.4: a CAP, not a wait -- a fast door ends the moment the cell changes, so raising
        // this costs nothing on normal setups and stops heavy cells being abandoned 1.8 s early
        // (measured: 11839 ms load vs the old 10000 ms cap).
        std::uint32_t transitionWindowMs = 30000;
        // Tracks whether uTransitionWindowMs was explicitly set in LoadingFlow.ini; when false,
        // adaptive per-cell windows apply and SaveConfig leaves uTransitionWindowMs commented out.
        bool          transitionWindowPinned = false;
        // Tier 3: proximity pre-warm. While walking around, demand the models of interiors that are
        // routed to a real transition, so the actual load finds them already in the model DB.
        bool          prewarmNearbyDoors = true;
        std::uint32_t prewarmModelCap = 200;
        float         prewarmRadius = 800.0f;
        // Visual envelope. We cannot shorten the real load, and the engine already plays the door
        // sound and its own fade -- so the only thing worth adding is TIMING: hold the screen black
        // for a beat after the cell swap so the fade-in does not reveal half-streamed geometry.
        // OFF by default. An image-space black only affects the WORLD render; the UI/Scaleform layer
        // is composited afterwards, so mods that draw widgets in the interface (BTPS and friends)
        // keep spinning on top of the black. Covering the swap artefact therefore does not work for
        // anyone who runs such a UI mod -- and the extra black is pure loss.
        bool          visualEnvelope = false;
        std::uint32_t envelopeBlackHoldMs = 300;
        float         envelopeFadeStrength = 1.0f;
        // "No black" mode: instead of letting the engine fade to black, stop its fade records for
        // the whole transition so the last rendered frame simply holds until the new cell appears.
        // Not black -- but a freeze. Long loads will look like a hitch, which is why the adaptive
        // rule (fall back to the vanilla menu on slow cells) exists.
        bool          noBlackTransition = false;
        // Policy: freeze-frame only makes sense for BUILDING doors. Cave/mine entrances are already
        // dark inside, so holding the last frame there buys nothing and reads as a still image --
        // those keep the normal fade. 1 (default) = apply no-black only to manual doors.
        bool          noBlackOnlyForManualDoors = false;   // v1.3.0: default OFF (see INI note)
        // Cut mask: a brief effect fired exactly at the moment the cell swap lands, to hide the hard
        // cut from "door half open" to "inside". It is NOT the long black screen -- default 80 ms,
        // and the record is swappable (black family, FadeToWhite 0x0010100B, blur 0x000B97F7,
        // desaturate 0x000B7983 ...). 0 = no mask at all.
        std::uint32_t cutMaskImod = 0x000F756D;
        std::uint32_t cutMaskMs = 0;   // v1.5.2: default OFF -- see the note at the call site
        // Per-door diagnostic. Measured in-game: NPCs activate doors constantly (502 of 559 log
        // lines in one 3-minute session), so this stays off unless explicitly requested.
        bool          logDoorActivations = false;
        // Adaptive fallback. 0 = always suppress. When > 0, a cell whose last measured load took
        // longer than this shows the vanilla loading menu instead of 2 s of black: on a heavily modded
        // setup interior loads can measure 1884-3269 ms, which is too long to hide.
        std::uint32_t suppressMaxMs = 0;
        // Adaptive freeze guard. Freezing only works while the load is short; once a cell has
        // been measured as slower than this, that cell uses the fade instead. 0 = never veto freeze.
        // This keeps the freeze route alive without gambling on every cell being fast.
        std::uint32_t freezeMaxMs = 600;
        // Pre-load wait override. MEASURED: from pressing E to the moment the engine even asks for
        // the LoadingMenu takes 1002-1022 ms, and it stayed at ~1 s even while we were cancelling
        // every fade record -- so that second is a TIMER inside the engine, not the fade itself.
        // "fFadeToBlackFadeSeconds" is the matching game setting. Negative = leave the engine alone;
        // 0.15-0.30 shortens the pre-load wait for every load door.
        // Engine transition timers, values taken from 1001Bits/Faster-Loadscreens (GPL-3.0) which
        // documents the vanilla defaults. These are INI-backed settings ("name:Section"), NOT
        // GameSettings -- looking them up in GameSettingCollection silently fails.
        // Measured on our own log: pressing E -> engine asks for the LoadingMenu takes ~1.0 s, which
        // is fFadeToBlackFadeSeconds at its vanilla 1.0.
        bool  tweakEngineTimers = true;
        float fadeToBlackFadeSeconds = 0.10f;    // vanilla 1.0  <- the pre-load wait
        float normalDoorFadeSecs = 0.10f;        // vanilla ?
        float normalDoorFadeWait = -1.0f;        // -1 = leave vanilla (measured 0.01 on this build)
        float autoDoorFadeSecs = 0.10f;
        float minSecondsForLoadFadeIn = 0.20f;   // vanilla 1.5; the upstream mod settled on 0.20
        // v1.6.0: 100 ms starves post-load scripts (reported crash with IED+SPID, and JK clutter
        // ending up scattered). 800 keeps most of the gain.
        // v1.6.4: default goes back to the v1.1.0 published value. One machine showing no difference
        // is NOT evidence that it makes no difference everywhere -- that inference was over-reach.
        // The menu now carries the trade-off (Stable restores the game's own values).
        float postLoadUpdateTimeMS = 100.0f;     // vanilla 500, but the author's INI ships 2000 in
                                                 // Skyrim.ini [Papyrus] -- 2 s of pure waiting that
                                                 // happens AFTER the cell change, i.e. outside our
                                                 // own N measurement. -1 = leave alone.
        float actorTeleportFadeSeconds = 0.10f;  // fade used when NPCS are teleported with you
        // Cap on how much the engine may preload. -1 = leave the engine's value alone.
        // The ON/OFF switch (bPreloadLinkedInteriors) has been REMOVED from this plugin: enabling it
        // hangs save loading on this load order at both the 25 MiB default cap and ~95 MiB, so it is
        // never touched. Only the cap remains adjustable, and it has shown no measurable effect.
        int   preloadSizeLimit = -1;
        float fastTravelFadeSecs = -1.0f;        // -1 = leave vanilla (0.5)
        // v1.6.5: back to vanilla. We have NO measurement showing 500 buys anything, and a plausible
        // mechanism by which it costs frame time after a load -- so it should not be the default.
        // (Unlike postLoad below, where an aggressive value plausibly does buy speed.)
        int   loadingQueuedPriorityBudgetMs = 20;
        // Release curve for the cut mask. This is the one place where an IMAD-to-IMAD blend is both
        // needed and reachable: Papyrus documents RemoveCrossFade as "fading it out over the
        // specified number of seconds" and CommonLib wraps it as StopCrossFade. (The forward
        // direction, ApplyCrossFade, is NOT wrapped by CommonLibSSE-NG and has no published
        // relocation ID -- guessing one would call an arbitrary function, so it is not used.)
        float         cutMaskFadeOut = 0.15f;
        // Continuity: do not freeze the instant the door is activated -- let the door's own opening
        // animation play for this long first, THEN start cancelling the fades. The held frame is
        // then "door already swinging open" instead of "hand on the handle", which reads as a
        // continuous motion into the cut. 0 = freeze immediately (old behaviour).
        std::uint32_t freezeDelayMs = 0;
        // Walk-through passages (mod cave tunnels): no door, you just walk in and it loads. They are
        // auto/frameless doors with a CONTINUOUS scene on both sides, so freezing on the last frame
        // shows no loading screen -- that is what Interiors->Interior transit gets here. Ext->Int (cave
        // mouth from outside) still uses the normal fade.
        bool          noBlackForWalkThrough = false;      // v1.3.0: default OFF (see INI note)
        // Global mode: suppress the LoadingMenu even when LoadingFlow's own door hook was not what
        // started the transition (trigger volumes, script MoveTo, anything else). Long loads fall
        // back automatically because the timeout re-posts the menu -- which is exactly what keeps
        // fast travel / load-game readable.
        bool          globalMenuSuppression = false;
        float         baseAltitudeZ = 26000.0f;
        float         slotStepZ = 3500.0f;
        float         blinkStrength = 1.0f;
        std::uint32_t doorOpenDelayMs = 260;
        std::uint32_t maxRefsPerCell = 800;
        std::uint32_t maxPocketSlots = 8;
        // ---- Assembly probe (READ-ONLY diagnostics) -------------------------------------------
        // After a transition completes, sample the player's 3D transform and the camera FOV for a
        // short window and report when they stop changing. This answers ONE question:
        //   how long does the engine keep assembling / animating the scene after the cell swap?
        // That number decides how long a retained frame would have to stay up to hide the assembly.
        // Read-only by design: it never writes a transform, never touches D3D, and only reads
        // objects the game already owns. Bounded by probeAssemblyMs per transition.
        bool          probeAssembly = false;
        std::uint32_t probeAssemblyMs = 2500;
        // Also read the engine's renderer struct (device / context / swap chain / main RT pointers)
        // so we know whether a capture path is reachable at all, and whether those pointers survive
        // a transition. Read-only; no COM call, no resource creation.
        bool          probeGfx = false;
        // ---- B: keep the presented frame instead of painting black (EXPERIMENTAL) --------------
        // Stops calling Present for the duration of the transition, so the display keeps showing
        // the exact frame the player last saw (ENB / CS included, because we never touch pixels).
        // Default OFF: this is the only COM vtable the plugin ever patches.
        bool          freezeFramePresent = false;
        std::uint32_t freezeFrameHoldMs = 300;
        // ---- Cover AT THE SWAP only (v1.2.2) ---------------------------------------------------
        // The engine's scene spin was measured at ~21 frames at 60 fps (about 350 ms) -- far shorter
        // than the load itself. Covering the whole load therefore buys nothing and reads as a hang.
        // When uCoverAtSwapMs > 0 the early cover (raised at menu suppression) is DISABLED and the
        // cover is applied only when the cell swap is detected, for at most this long.
        //   iCoverAtSwapMode: 1 = freeze the presented image (skip Present), 2 = black (IMAD)
        std::uint32_t coverAtSwapMs = 0;   // clamped to 350 by the loader
        int           coverAtSwapMode = 1;
        // ---- Void effect: give the 124 dead frames something to do (v1.2.3) --------------------
        // Measured timeline: press E -> hand touches the door, then ~124 frames (~2.07 s) where the
        // old cell is unloading and nothing new is drawn (the screen is one static frame), then ~21
        // frames (~350 ms) of scene-spin, then the new cell. Covering the 2 s is pointless; filling
        // it is not. This ramps an image-space darkening over the void so the wait reads as a
        // deliberate fade rather than a frozen picture, then takes it to full black across the spin
        // and fades back into the finished cell.
        std::uint32_t voidRampMs = 600;      // how long the darkening takes to reach fVoidDarken
        float         voidDarken = 0.0f;    // 0 = off; 0.75 keeps the old scene faintly visible
        std::uint32_t voidHoldMs = 220;      // full-black hold after the swap, before fading back
        // ---- World probe (v1.3.0, READ-ONLY) ---------------------------------------------------
        // The open question is whether the ~124 dead frames are dead because the world is PAUSED or
        // because the old cell is gone. If game time keeps advancing we may be able to keep the world
        // looking alive during the wait, which would beat any cover effect.
        bool          probeWorld = false;
        // ---- Step 2 probe (v1.3.1): present-hook ORDER + optional frame dump --------------------
        // capProbe   : log which module sits above/below us in the Present hook chain (no D3D call)
        // captureFrame: save a few backbuffer frames as BMP into LoadingFlow-capture/ for eyeballing
        bool          capProbe = false;
        bool          captureFrame = false;
        // v1.4.5: idle sampling used to eat the whole budget before the first door (16/16 captures were
        // tag=live). Off by default -- the transition-window burst owns the budget.
        bool          captureLiveFrames = false;
        std::uint32_t captureFrames = 4;
        // v1.4.6: spread the samples over the whole window (one every captureEveryMs) and cap the
        // session at captureTotalMax files -- 3840x2160 BMPs are 24.9 MB each at full resolution.
        std::uint32_t captureEveryMs = 150;
        std::uint32_t captureTotalMax = 48;
        // ---- Void overlay (v1.4.0, EXPERIMENTAL) ----------------------------------------------
        // Draws motion ON TOP of the frozen frame for the ~2 s dead window, then a soft pulse at
        // the cut. Additive only: it brightens pixels and physically cannot darken any of them,
        // which is what the "no darkening anywhere" decision requires. Rendered in the Present
        // hook (the one place that is guaranteed to run while the world is not drawing), never
        // touching engine pixels -- so ENB / Community Shaders grading is preserved underneath.
        bool          voidOverlay = false;
        std::uint32_t overlayMotes = 18;          // 0..32
        float         overlayBrightness = 0.55f;  // 0..2
        float         overlayBreath = 0.020f;     // 0..0.10 (additive lift, 0 disables)
        float         overlayPulse = 0.55f;       // 0..1.5, strength of the cut pulse
        std::uint32_t overlayPulseMs = 300;       // 50..800, must stay under the ~350 ms spin
        std::uint32_t overlayMaxMs = 12000;       // hard cap from arming -- failsafe only: measured windows are 4~5.9 s
        float         overlayMoteScale = 1.0f;    // 0.3..3 mote size multiplier
        // ---- v1.4.7 cross-fade at the cut ------------------------------------------------------
        // The ugliest part is the engine spinning the new cell into place AFTER the cell change
        // (~21 frames measured: radial smearing + a brightness spike). Instead of an additive pulse,
        // keep the last frame of the old cell and dissolve it into the finished one. A 1:1 blit means
        // the HUD is not distorted. The length is capped at 350 ms by the user's rule.
        // v1.2.3-exp: 0 = do not touch the loading menu, 1 = suppress it (drop kShow), 2 = let it open
        // and only hide its visuals. Mode 2 keeps kPausesGame (the game stays paused during the load)
        // and the engine's own loading state machine -- both of which modes 0/1 lose or bypass.
        // -1 = follow the legacy bSuppressLoadingMenu boolean.
        int           suppressMode = -1;
        bool          crossFade = true;
        // v1.4.13 diagnostic: lift the WHOLE screen additively during the dead window. If the picture
        // visibly brightens, our Present-layer drawing reaches the display; if it does not, nothing we
        // draw at Present time is being presented and the route has to change (UI/Scaleform instead).
        bool          overlayDebugFullscreen = false;
        // v1.5.5: how long to withhold Present at the cut so the engine's empty frames never show.
        // v1.3.2: SAFETY CAP for holding the last frame after the cut. 0 = OFF (no freeze at all).
        std::uint32_t holdAtCutMs = 250;
        std::uint32_t crossFadeMs = 350;
        // v1.9.0: how much of that 350 ms is spent simply HOLDING the snapshot before the dissolve
        // starts. A straight dissolve between the dark frozen old cell and a much brighter new one
        // reads as a bright flash; holding first covers the assembly and gives the eye an anchor.
        std::uint32_t crossFadeHoldMs = 3000;
        // ---- v1.3.0 frozen-frame pass ("the picture keeps moving") -----------------------------
        // The dead window is ONE still picture. Nothing darkens it (that route is dead), nothing
        // covers it -- instead the picture we already have is re-sampled with a slowly growing
        // scale, so every pixel changes a little every frame. That reads as motion, which is
        // exactly what the window is missing, and it costs one texture copy.
        //   iVisualMode 0 = the Present hook draws nothing at all (v1.2.1 behaviour) -- DEFAULT, so a
        //                   public build is byte-for-byte the same product as before
        //               1 = dead-window push-in + cut cross-fade          (experimental build)
        //               2 = cut cross-fade only (push-in disabled)
        //               3 = PROBE: fill the whole target magenta for a few frames. One glance tells
        //                   us whether anything we draw at Present time reaches the display; if it
        //                   does not, the whole Present layer is a dead end and only a Fader/Mist
        //                   presentation takeover is left.
        int   visualMode = 0;
        // v1.3.4: submitting through slot 22 reaches the screen (proven: 90 frames of magenta),
        // v1.3.4: slot 22's downstream can be sl.interposer.dll -- NVIDIA Streamline, i.e.
        // DLSS frame generation. While the picture is static that path is idle and the bypass works
        // for hundreds of frames; the moment the scene actually moves (the cut) frame generation
        // engages and needs its own present sequence, and bypassing it wedges the whole chain
        // (observed: frozen picture, then the render collapsing into the top-left corner at a few
        // seconds per frame). So the bypass is OFF by default: stable, no visual effect.
        // Turn it on only with DLSS Frame Generation disabled in the game/driver.
        bool  usePresent1ForVisual = false;
        // v1.8.7: v1.8.4 hedged one path against every single door and lost. The split below is
        // still worth having, but the recommendation has since flipped: the module that swallowed
        // our pixels on slot 8 was DebugMod, and it is now disabled, so the normal chain is the
        // route to test. Slot 22 reaches the screen but its downstream is sl.interposer.dll (NVIDIA
        // Streamline / DLSS frame generation) and using it is confirmed to fight frame generation.
        //   0 = never bypass (slot 8 only) -- DEFAULT, and the one to measure against now
        //   1 = bypass only while the dead window is static, back to slot 8 the instant the cut
        //       fires -- comparison only, not a shipping route
        //   2 = bypass on every drawn frame, cut included (the v1.8.4 route: freezes at the cut)
        int   submitRoute = 0;
        // v1.8.7: the measurement nine rounds of diagnosis never made. Ask DXGI whether the Present
        // we handed to the chain below us was counted at all. `Present returned 0x00000000` only
        // ever proved that the function we called did not report an error -- and three return-0
        // paths in this very file return S_OK without presenting anything.
        bool  presentAudit = false;
        // v1.10.0: the push-in is OFF by default. The user tried it in game and judged the look bad
        // -- the frozen picture moving towards the camera reads as a lurch. The effect is now a
        // gradual loss of focus (frosted glass), which keeps the frame still. The knob stays so the
        // push-in can still be dialled in from the menu without a rebuild.
        float deadZoomAmount = 0.0f;     // 0 .. 0.15, push-in amount (0 = off)
        // v1.10.4: experiment -- after the swap the engine's picture sits at ~half brightness for
        // ~800 ms and then steps to the real scene. The log rules out fPostLoadUpdateTimeMS (104 ms
        // there) and rules out a fade ramp (the value is constant, not ramping), and we already stop
        // FadeToBlack + FadeToBlackHold every step including at the cut. The one form we deliberately
        // never touch is FadeToBlackBackImod -- cancelling it mid-transition restarts the engine's
        // fade-in every 25 ms and strobes. After the cut that trade-off may flip. Off by default
        // because it is still an experiment, not because it is dangerous.
        // v1.12.0 COVER TEST -- the minimal experiment before any rewrite:
        // Can the ENGINE's own fullscreen cover hold the screen during the dead window WITHOUT us
        // touching Present? If yes (covers the whole screen, and the hand-back is clean), then the
        // whole flicker problem is solved by never presenting our pixels again -- and the remaining
        // work is only "show a texture instead of black".
        //   0 = off (default)
        //   1 = keep FadeToBlackImod at full strength through the dead window, stop it at the cut
        //       (measured: this does NOTHING -- IMAD Trigger has no visible effect during a transition)
        //   3 = UI-layer cover, drawn at RUNTIME into a menu movie the engine has already loaded
        //       (v1.13.2). No SWF of our own is involved, so nothing can fail to load.
        //   2 = UI-layer cover (v1.13.0): our own Scaleform menu showing a fullscreen quad, driven by
        //       SetVariable("cover._alpha"). This never touches Present, so the upscaler's present
        //       sequence stays intact -- the whole reason for this route.
        int   coverTest = 0;
        bool  cancelFadeBackAfterCut = false;
        float deadBlurPx = 40.0f;        // 0 .. 120, frosted-glass radius at the end of the dead window
        std::uint32_t deadZoomMs = 2000; // how long it takes to get there -- the user's 124-frame rule
        std::uint32_t probeFrames = 90;  // how many Present calls the magenta probe lasts (1..600)
    };
    Config g_config;

    std::unordered_set<RE::FormID> g_excludeCells;
    std::vector<std::string>       g_excludeSpecs;
    // Cells routed to the vanilla path on purpose (script-bearing interiors, scanned offline).
    // Same effect as [ExcludeCells]; kept in its own section so the intent stays readable.
    std::vector<std::string>       g_forceRealSpecs;
    std::vector<std::string>       g_rawMenuSectionLines;  // P0-3: preserved [Menu] lines (comments + overrides) in LoadingFlow.ini

    // ---------------- Localization (L() table) ----------------
    std::unordered_map<std::string, std::string> g_lang;

    std::size_t CountFormatSpecs(std::string_view a_fmt)
    {
        std::size_t n = 0;
        for (std::size_t i = 0; i < a_fmt.size(); ++i) {
            const char c = a_fmt[i];
            if (c == '%') {
                if (i + 1 < a_fmt.size() && a_fmt[i + 1] == '%') {
                    ++i;
                    continue;
                }
                ++n;
            } else if (c == '{') {
                if (i + 1 < a_fmt.size() && a_fmt[i + 1] == '{') {
                    ++i;
                    continue;
                }
                ++n;
            }
        }
        return n;
    }

    void LoadLangFile(const char* a_path, bool a_override, bool a_warnIfEmpty = true)
    {
        std::ifstream f(a_path);
        if (!f.is_open()) {
            if (a_override) {
                logger::info("Language pack not found (using built-in English): {}", a_path);
            }
            return;
        }
        std::string line;
        std::string section;
        auto trim = [](std::string& x) {
            const auto b = x.find_first_not_of(" \t\r\n");
            const auto e = x.find_last_not_of(" \t\r\n");
            x = (b == std::string::npos) ? std::string{} : x.substr(b, e - b + 1);
        };
        std::size_t loaded = 0;
        while (std::getline(f, line)) {
            if (line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
            trim(line);
            if (line.empty() || line.front() == ';' || line.front() == '#') {
                continue;
            }
            if (line.front() == '[' && line.back() == ']') {
                section = line.substr(1, line.size() - 2);
                continue;
            }
            if (section != "Menu") {
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            trim(k);
            trim(v);
            if (k.empty()) {
                continue;
            }
            if (a_override || g_lang.find(k) == g_lang.end()) {
                g_lang[k] = v;
                ++loaded;
            }
        }
        if (loaded) {
            logger::info("Language pack: {} -> {} menu strings", a_path, loaded);
        } else if (a_warnIfEmpty) {
            // Only the real language pack should warn. The main INI is probed first on purpose
            // (it may carry a [Menu] override section) and normally yields nothing.
            logger::warn("Language pack: {} opened but yielded 0 strings", a_path);
        }
    }

    void LoadLang()
    {
        const auto backup = g_lang;
        g_lang.clear();
        LoadLangFile("Data/SKSE/Plugins/LoadingFlow.ini", false, /*a_warnIfEmpty=*/false);
        LoadLangFile("Data/SKSE/Plugins/LoadingFlow_Lang.ini", true);
        if (g_lang.empty() && !backup.empty()) {
            g_lang = backup;
        }
    }

    const char* L(const char* a_key, const char* a_english)
    {
        const auto it = g_lang.find(a_key);
        if (it == g_lang.end() || it->second.empty()) {
            return a_english;
        }
        if (CountFormatSpecs(it->second) != CountFormatSpecs(a_english)) {
            logger::warn("Menu string '{}' has mismatched format specifiers; falling back to English", a_key);
            return a_english;
        }
        return it->second.c_str();
    }

    std::atomic<bool> g_shuttingDown{ false };

    bool AtomicReplaceFile(const char* a_tmpPath, const char* a_dstPath)
    {
        std::error_code ec;
        std::filesystem::rename(a_tmpPath, a_dstPath, ec);
        if (!ec) {
            return true;
        }
        ec.clear();
        std::filesystem::copy_file(a_tmpPath, a_dstPath,
            std::filesystem::copy_options::overwrite_existing, ec);
        std::error_code rmEc;
        std::filesystem::remove(a_tmpPath, rmEc);
        if (ec) {
            if (!g_shuttingDown.load(std::memory_order_acquire)) {
                logger::error("Failed to replace {} from {}: {}", a_dstPath, a_tmpPath, ec.message());
            }
            return false;
        }
        return true;
    }

    void SaveConfig()
    {
        static constexpr const char* kIniPath = "Data/SKSE/Plugins/LoadingFlow.ini";
        static constexpr const char* kTmpPath = "Data/SKSE/Plugins/LoadingFlow.ini.tmp";
        // P0-3: canonical must-write section list so new sections are never silently dropped
        static constexpr const char* kMustWriteSections[] = { "General", "ExcludeCells", "ForceRealTransition", "Menu" };
        (void)kMustWriteSections;

        {
            std::ofstream f(kTmpPath, std::ios::binary | std::ios::trunc);
            if (!f.is_open()) {
                logger::error("Failed to open {} for atomic writing", kTmpPath);
                return;
            }
            f << "; LoadingFlow configuration (all sections written atomically via .tmp + rename)\r\n";
            f << "[General]\r\n";
            f << "bEnabled = " << (g_config.enabled ? 1 : 0) << "\r\n";
            f << "bAllowInteriorToInterior = " << (g_config.allowInteriorToInterior ? 1 : 0) << "\r\n";
            f << "bAllowPocketToExterior = " << (g_config.allowPocketToExterior ? 1 : 0) << "\r\n";
            f << "bMoveActorsToPocket = " << (g_config.moveActorsToPocket ? 1 : 0) << "\r\n";
            f << "bSpawnBlackBoxShell = " << (g_config.spawnBlackBoxShell ? 1 : 0) << "\r\n";
            f << "bPlayDoorSoundAndAnim = " << (g_config.playDoorSoundAndAnim ? 1 : 0) << "\r\n";
            f << "bTriggerBlinkImod = " << (g_config.triggerBlinkImod ? 1 : 0) << "\r\n";
            f << "bSneakForcesVanillaLoad = " << (g_config.sneakForcesVanillaLoad ? 1 : 0) << "\r\n";
            f << "bShowDebugNotifications = " << (g_config.showDebugNotifications ? 1 : 0) << "\r\n";
            f << "bFakeLocationEvents = " << (g_config.fakeLocationEvents ? 1 : 0) << "\r\n";
            f << "bFakeCellLoadEvents = " << (g_config.fakeCellLoadEvents ? 1 : 0) << "\r\n";
            f << "bUsePocketCopies = " << (g_config.usePocketCopies ? 1 : 0) << "\r\n";
            f << "bSuppressLoadingMenu = " << (g_config.suppressLoadingMenu ? 1 : 0) << "\r\n";
            f << "uSuppressTimeoutMs = " << g_config.suppressTimeoutMs << "\r\n";
            if (g_config.transitionWindowPinned) {
                f << "uTransitionWindowMs = " << g_config.transitionWindowMs << "\r\n";
            } else {
                f << ";uTransitionWindowMs = 30000\r\n";
            }
            f << "bPrewarmNearbyDoors = " << (g_config.prewarmNearbyDoors ? 1 : 0) << "\r\n";
            f << "uPrewarmModelCap = " << g_config.prewarmModelCap << "\r\n";
            f << std::format("fPrewarmRadius = {:.0f}\r\n", g_config.prewarmRadius);
            f << "bVisualEnvelope = " << (g_config.visualEnvelope ? 1 : 0) << "\r\n";
            f << "uEnvelopeBlackHoldMs = " << g_config.envelopeBlackHoldMs << "\r\n";
            f << std::format("fEnvelopeFadeStrength = {:.2f}\r\n", g_config.envelopeFadeStrength);
            f << "bNoBlackTransition = " << (g_config.noBlackTransition ? 1 : 0) << "\r\n";
            f << "bTweakEngineTimers = " << (g_config.tweakEngineTimers ? 1 : 0) << "\r\n";
            f << std::format("fFadeToBlackFadeSeconds = {:.3f}\r\n", g_config.fadeToBlackFadeSeconds);
            f << std::format("fNormalDoorFadeSecs = {:.3f}\r\n", g_config.normalDoorFadeSecs);
            f << std::format("fNormalDoorFadeWait = {:.3f}\r\n", g_config.normalDoorFadeWait);
            f << std::format("fAutoDoorFadeSecs = {:.3f}\r\n", g_config.autoDoorFadeSecs);
            f << std::format("fMinSecondsForLoadFadeIn = {:.4f}\r\n", g_config.minSecondsForLoadFadeIn);
            f << std::format("fPostLoadUpdateTimeMS = {:.1f}\r\n", g_config.postLoadUpdateTimeMS);
            f << std::format("fActorTeleportFadeSeconds = {:.2f}\r\n", g_config.actorTeleportFadeSeconds);
            f << "iPreloadSizeLimit = " << g_config.preloadSizeLimit << "\r\n";
            f << std::format("fFastTravelFadeSecs = {:.2f}\r\n", g_config.fastTravelFadeSecs);
            f << "iLoadingQueuedPriorityBudgetMs = " << g_config.loadingQueuedPriorityBudgetMs << "\r\n";
            f << "bNoBlackOnlyForManualDoors = " << (g_config.noBlackOnlyForManualDoors ? 1 : 0) << "\r\n";
            f << "bNoBlackForWalkThrough = " << (g_config.noBlackForWalkThrough ? 1 : 0) << "\r\n";
            f << std::format("sCutMaskImod = 0x{:08X}\r\n", g_config.cutMaskImod);
            f << "uCutMaskMs = " << g_config.cutMaskMs << "\r\n";
            f << "bLogDoorActivations = " << (g_config.logDoorActivations ? 1 : 0) << "\r\n";
            f << "uSuppressMaxMs = " << g_config.suppressMaxMs << "\r\n";
            f << "uFreezeMaxMs = " << g_config.freezeMaxMs << "\r\n";
            f << std::format("fCutMaskFadeOut = {:.2f}\r\n", g_config.cutMaskFadeOut);
            f << "uFreezeDelayMs = " << g_config.freezeDelayMs << "\r\n";
            f << "uHoldAtCutMs = " << g_config.holdAtCutMs << "\r\n";
            f << "bProbeAssembly = " << (g_config.probeAssembly ? 1 : 0) << "\r\n";
            f << "uProbeAssemblyMs = " << g_config.probeAssemblyMs << "\r\n";
            f << "bProbeGfx = " << (g_config.probeGfx ? 1 : 0) << "\r\n";
            f << "bFreezeFramePresent = " << (g_config.freezeFramePresent ? 1 : 0) << "\r\n";
            f << "uFreezeFrameHoldMs = " << g_config.freezeFrameHoldMs << "\r\n";
            f << "uCoverAtSwapMs = " << g_config.coverAtSwapMs << "\r\n";
            f << "iCoverAtSwapMode = " << g_config.coverAtSwapMode << "\r\n";
            f << "uVoidRampMs = " << g_config.voidRampMs << "\r\n";
            f << std::format("fVoidDarken = {:.2f}\r\n", g_config.voidDarken);
            f << "uVoidHoldMs = " << g_config.voidHoldMs << "\r\n";
            f << "bProbeWorld = " << (g_config.probeWorld ? 1 : 0) << "\r\n";
            f << "bCapProbe = " << (g_config.capProbe ? 1 : 0) << "\r\n";
            f << "bCaptureFrame = " << (g_config.captureFrame ? 1 : 0) << "\r\n";
            f << "bCaptureLiveFrames = " << (g_config.captureLiveFrames ? 1 : 0) << "\r\n";
            f << "uCaptureEveryMs = " << g_config.captureEveryMs << "\r\n";
            f << "uCaptureTotalMax = " << g_config.captureTotalMax << "\r\n";
            f << "uCaptureFrames = " << g_config.captureFrames << "\r\n";
            f << "bVoidOverlay = " << (g_config.voidOverlay ? 1 : 0) << "\r\n";
            f << "uOverlayMotes = " << g_config.overlayMotes << "\r\n";
            f << std::format("fOverlayBrightness = {:.2f}\r\n", g_config.overlayBrightness);
            f << std::format("fOverlayBreath = {:.3f}\r\n", g_config.overlayBreath);
            f << std::format("fOverlayPulse = {:.2f}\r\n", g_config.overlayPulse);
            f << "uOverlayPulseMs = " << g_config.overlayPulseMs << "\r\n";
            f << "uOverlayMaxMs = " << g_config.overlayMaxMs << "\r\n";
            f << std::format("fOverlayMoteScale = {:.2f}\r\n", g_config.overlayMoteScale);
            f << "bCrossFade = " << (g_config.crossFade ? 1 : 0) << "\r\n";
            f << "uCrossFadeMs = " << g_config.crossFadeMs << "\r\n";
            f << "uCrossFadeHoldMs = " << g_config.crossFadeHoldMs << "\r\n";
            // ---- visual layer (v1.10.0) -------------------------------------------------------------
            // These keys were read from the INI but never written back, so ANY menu edit rewrote the file
            // without them and silently wiped the whole visual block -- exactly the failure this project
            // already has a rule about: SaveConfig must write every key it reads.
            f << "iVisualMode = " << g_config.visualMode << "\r\n";
            f << "iSubmitRoute = " << g_config.submitRoute << "\r\n";
            f << "bUsePresent1ForVisual = " << (g_config.usePresent1ForVisual ? 1 : 0) << "\r\n";
            f << "bPresentAudit = " << (g_config.presentAudit ? 1 : 0) << "\r\n";
            f << "iCoverTest = " << g_config.coverTest << "\r\n";
            f << "bCancelFadeBackAfterCut = " << (g_config.cancelFadeBackAfterCut ? 1 : 0) << "\r\n";
            f << std::format("fDeadZoomAmount = {:.3f}\r\n", g_config.deadZoomAmount);
            f << "uDeadZoomMs = " << g_config.deadZoomMs << "\r\n";
            f << std::format("fDeadBlurPx = {:.1f}\r\n", g_config.deadBlurPx);
            f << "uProbeFrames = " << g_config.probeFrames << "\r\n";
            f << "bOverlayDebugFullscreen = " << (g_config.overlayDebugFullscreen ? 1 : 0) << "\r\n";
            f << "iSuppressMode = " << g_config.suppressMode << "\r\n";
            f << "bGlobalMenuSuppression = " << (g_config.globalMenuSuppression ? 1 : 0) << "\r\n";
            f << std::format("fBlinkStrength = {:.2f}\r\n", g_config.blinkStrength);
            f << std::format("fBaseAltitudeZ = {:.1f}\r\n", g_config.baseAltitudeZ);
            f << std::format("fSlotStepZ = {:.1f}\r\n", g_config.slotStepZ);
            f << "uDoorOpenDelayMs = " << g_config.doorOpenDelayMs << "\r\n";
            f << "uMaxRefsPerCell = " << g_config.maxRefsPerCell << "\r\n";
            f << "uMaxPocketSlots = " << g_config.maxPocketSlots << "\r\n";
            f << "\r\n[ExcludeCells]\r\n";
            f << "; Cells that skip the menu suppression entirely (real transition + normal menu).\r\n";
            for (const auto& s : g_excludeSpecs) {
                f << s << " = 1\r\n";
            }
            f << "\r\n[ForceRealTransition]\r\n";
            f << "; Cells that must use the engine's own (real) cell transition instead of a pocket\r\n";
            f << "; copy: quest/location scripts inside them listen to cell events we cannot fake.\r\n";
            for (const auto& s : g_forceRealSpecs) {
                f << s << " = 1\r\n";
            }
            if (!g_rawMenuSectionLines.empty()) {
                f << "\r\n[Menu]\r\n";
                for (const auto& rawLine : g_rawMenuSectionLines) {
                    f << rawLine << "\r\n";
                }
            }
            f.flush();
            if (!f.good()) {
                logger::error("I/O error while flushing {}; aborting atomic replace", kTmpPath);
                return;
            }
        }

        if (AtomicReplaceFile(kTmpPath, kIniPath)) {
            logger::info("Config saved atomically to {}", kIniPath);
        }
    }

    void LoadConfig()
    {
        // P2-6: Always reset to defaults before reading so removed lines revert cleanly on Reload INI
        g_config = Config{};
        g_excludeSpecs.clear();
        g_forceRealSpecs.clear();
        g_rawMenuSectionLines.clear();

        std::ifstream f("Data/SKSE/Plugins/LoadingFlow.ini");
        if (!f.is_open()) {
            logger::info("LoadingFlow.ini not found; writing default configuration file");
            SaveConfig();  // P2-7: create default INI on first run
            LoadLang();
            return;
        }
        std::string line;
        std::string section;
        // Duplicate-key guard: a key appearing twice is a packaging/editing accident. Whichever copy
        // sits outside [General] is silently ignored, which once made a user's edit do nothing.
        std::unordered_map<std::string, int> keySeen;
        int lineNo = 0;
        auto trim = [](std::string& s) {
            const auto b = s.find_first_not_of(" \t\r\n");
            const auto e = s.find_last_not_of(" \t\r\n");
            s = (b == std::string::npos) ? std::string{} : s.substr(b, e - b + 1);
        };
        while (std::getline(f, line)) {
            ++lineNo;
            if (line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
                line.pop_back();
            }
            std::string trimmedHdr = line;
            trim(trimmedHdr);
            if (!trimmedHdr.empty() && trimmedHdr.front() == '[' && trimmedHdr.back() == ']') {
                section = trimmedHdr.substr(1, trimmedHdr.size() - 2);
                trim(section);
                continue;
            }
            if (section == "Menu") {
                // P0-3: preserve every raw line (comments + overrides) under [Menu] in LoadingFlow.ini
                if (!trimmedHdr.empty()) {
                    g_rawMenuSectionLines.push_back(line);
                }
                continue;
            }
            const auto sc = line.find(';');
            if (sc != std::string::npos) {
                line.erase(sc);
            }
            trim(line);
            if (line.empty() || line.front() == '#') {
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            trim(key);
            trim(val);
            if (key.empty()) {
                continue;
            }
            // Any key outside [General] is ignored by this parser, so a duplicated key silently does
            // nothing. Warn loudly: that exact accident made a user's preload settings inert.
            if (++keySeen[key] > 1) {
                logger::warn("[Config] duplicate key '{}' at line {} (section [{}]) -- only the copy inside [General] takes effect",
                    key, lineNo, section);
            } else if (section != "General" && section != "ExcludeCells" && section != "ForceRealTransition") {
                logger::warn("[Config] key '{}' at line {} is in section [{}], which is not read; move it to [General]",
                    key, lineNo, section);
            }
            try {
                if (section == "General") {
                    if (key == "bEnabled") {
                        g_config.enabled = (std::stoi(val) != 0);
                    } else if (key == "bAllowInteriorToInterior") {
                        g_config.allowInteriorToInterior = (std::stoi(val) != 0);
                    } else if (key == "bAllowPocketToExterior") {
                        g_config.allowPocketToExterior = (std::stoi(val) != 0);
                    } else if (key == "bMoveActorsToPocket") {
                        g_config.moveActorsToPocket = (std::stoi(val) != 0);
                    } else if (key == "bSpawnBlackBoxShell") {
                        g_config.spawnBlackBoxShell = (std::stoi(val) != 0);
                    } else if (key == "bPlayDoorSoundAndAnim") {
                        g_config.playDoorSoundAndAnim = (std::stoi(val) != 0);
                    } else if (key == "bTriggerBlinkImod") {
                        g_config.triggerBlinkImod = (std::stoi(val) != 0);
                    } else if (key == "bSneakForcesVanillaLoad") {
                        g_config.sneakForcesVanillaLoad = (std::stoi(val) != 0);
                    } else if (key == "bShowDebugNotifications") {
                        g_config.showDebugNotifications = (std::stoi(val) != 0);
                    } else if (key == "bFakeLocationEvents") {
                        g_config.fakeLocationEvents = (std::stoi(val) != 0);
                    } else if (key == "bFakeCellLoadEvents") {
                        g_config.fakeCellLoadEvents = (std::stoi(val) != 0);
                    } else if (key == "bUsePocketCopies") {
                        g_config.usePocketCopies = (std::stoi(val) != 0);
                    } else if (key == "bSuppressLoadingMenu") {
                        g_config.suppressLoadingMenu = (std::stoi(val) != 0);
                    } else if (key == "uSuppressTimeoutMs") {
                        // v1.3.4e: was clamped to 5000, so an INI value of 6000 silently became 5000 while leaving
                        // the key out gave 6000 -- the cap made the file lie. A heavily modded setup needs
                        // fallbacks well past 5 s; the adaptive window is the real limit anyway.
                        g_config.suppressTimeoutMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 60000));
                    } else if (key == "uTransitionWindowMs") {
                        g_config.transitionWindowMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 500, 60000));
                        g_config.transitionWindowPinned = true;
                    } else if (key == "bPrewarmNearbyDoors") {
                        g_config.prewarmNearbyDoors = (std::stoi(val) != 0);
                    } else if (key == "uPrewarmModelCap") {
                        g_config.prewarmModelCap = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 10, 2000));
                    } else if (key == "fPrewarmRadius") {
                        g_config.prewarmRadius = std::clamp(std::stof(val), 200.0f, 4000.0f);
                    } else if (key == "bVisualEnvelope") {
                        g_config.visualEnvelope = (std::stoi(val) != 0);
                    } else if (key == "uEnvelopeBlackHoldMs") {
                        g_config.envelopeBlackHoldMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 1000));
                    } else if (key == "fEnvelopeFadeStrength") {
                        g_config.envelopeFadeStrength = std::clamp(std::stof(val), 0.1f, 1.0f);
                    } else if (key == "bNoBlackTransition") {
                        g_config.noBlackTransition = (std::stoi(val) != 0);
                    } else if (key == "bNoBlackOnlyForManualDoors") {
                        g_config.noBlackOnlyForManualDoors = (std::stoi(val) != 0);
                    } else if (key == "sCutMaskImod") {
                        g_config.cutMaskImod = (val.starts_with("0x") || val.starts_with("0X"))
                            ? static_cast<std::uint32_t>(std::stoul(val, nullptr, 16))
                            : static_cast<std::uint32_t>(std::stoul(val));
                    } else if (key == "uCutMaskMs") {
                        g_config.cutMaskMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 1000));
                    } else if (key == "bLogDoorActivations") {
                        g_config.logDoorActivations = (std::stoi(val) != 0);
                    } else if (key == "uSuppressMaxMs") {
                        g_config.suppressMaxMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 60000));
                    } else if (key == "uFreezeMaxMs") {
                        g_config.freezeMaxMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 30000));
                    } else if (key == "bTweakEngineTimers") {
                        g_config.tweakEngineTimers = (std::stoi(val) != 0);
                    } else if (key == "fFadeToBlackFadeSeconds") {
                        g_config.fadeToBlackFadeSeconds = std::clamp(std::stof(val), -1.0f, 5.0f);
                    } else if (key == "fNormalDoorFadeSecs") {
                        g_config.normalDoorFadeSecs = std::clamp(std::stof(val), -1.0f, 5.0f);
                    } else if (key == "fNormalDoorFadeWait") {
                        g_config.normalDoorFadeWait = std::clamp(std::stof(val), -1.0f, 5.0f);
                    } else if (key == "fAutoDoorFadeSecs") {
                        g_config.autoDoorFadeSecs = std::clamp(std::stof(val), -1.0f, 5.0f);
                    } else if (key == "fMinSecondsForLoadFadeIn") {
                        g_config.minSecondsForLoadFadeIn = std::clamp(std::stof(val), -1.0f, 5.0f);
                    } else if (key == "fPostLoadUpdateTimeMS") {
                        g_config.postLoadUpdateTimeMS = std::clamp(std::stof(val), -1.0f, 5000.0f);
                    } else if (key == "fActorTeleportFadeSeconds") {
                        g_config.actorTeleportFadeSeconds = std::clamp(std::stof(val), -1.0f, 5.0f);
                    } else if (key == "iPreloadSizeLimit") {
                        // Upper bound is the int32 limit because the engine stores this in a Setting's
                        // int32 slot. Clamping used to happen silently and quietly turned a 1 GiB
                        // request into 95 MiB, so now any truncation is logged.
                        const long long raw = std::stoll(val);
                        constexpr long long kMax = 2147483647LL;
                        const long long clamped = std::clamp(raw, -1LL, kMax);
                        if (clamped != raw) {
                            logger::warn("[Config] iPreloadSizeLimit {} clamped to {} (int32 limit)", raw, clamped);
                        }
                        g_config.preloadSizeLimit = static_cast<int>(clamped);
                    } else if (key == "fFastTravelFadeSecs") {
                        g_config.fastTravelFadeSecs = std::clamp(std::stof(val), -1.0f, 5.0f);
                    } else if (key == "iLoadingQueuedPriorityBudgetMs") {
                        g_config.loadingQueuedPriorityBudgetMs = std::clamp(std::stoi(val), -1, 5000);
                    } else if (key == "fCutMaskFadeOut") {
                        g_config.cutMaskFadeOut = std::clamp(std::stof(val), 0.0f, 2.0f);
                    } else if (key == "uFreezeDelayMs") {
                        g_config.freezeDelayMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 600));
                    } else if (key == "uHoldAtCutMs") {
                        g_config.holdAtCutMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 2000));
                    } else if (key == "bProbeAssembly") {
                        g_config.probeAssembly = (std::stoi(val) != 0);
                    } else if (key == "uProbeAssemblyMs") {
                        g_config.probeAssemblyMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 10000));
                    } else if (key == "bProbeGfx") {
                        g_config.probeGfx = (std::stoi(val) != 0);
                    } else if (key == "bFreezeFramePresent") {
                        g_config.freezeFramePresent = (std::stoi(val) != 0);
                    } else if (key == "uFreezeFrameHoldMs") {
                        g_config.freezeFrameHoldMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 5000));
                    } else if (key == "uCoverAtSwapMs") {
                        g_config.coverAtSwapMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 350));
                    } else if (key == "iCoverAtSwapMode") {
                        g_config.coverAtSwapMode = std::clamp(std::stoi(val), 1, 2);
                    } else if (key == "uVoidRampMs") {
                        g_config.voidRampMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 3000));
                    } else if (key == "fVoidDarken") {
                        g_config.voidDarken = std::clamp(std::stof(val), 0.0f, 1.0f);
                    } else if (key == "uVoidHoldMs") {
                        g_config.voidHoldMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 1500));
                    } else if (key == "bProbeWorld") {
                        g_config.probeWorld = (std::stoi(val) != 0);
                    } else if (key == "bCapProbe") {
                        g_config.capProbe = (std::stoi(val) != 0);
                    } else if (key == "bCaptureFrame") {
                        g_config.captureFrame = (std::stoi(val) != 0);
                    } else if (key == "bCaptureLiveFrames") {
                        g_config.captureLiveFrames = (std::stoi(val) != 0);
                    } else if (key == "uCaptureEveryMs") {
                        g_config.captureEveryMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 16, 2000));
                    } else if (key == "uCaptureTotalMax") {
                        g_config.captureTotalMax = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 1, 200));
                    } else if (key == "uCaptureFrames") {
                        g_config.captureFrames = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 16));
                    } else if (key == "bVoidOverlay") {
                        g_config.voidOverlay = (std::stoi(val) != 0);
                    } else if (key == "uOverlayMotes") {
                        g_config.overlayMotes = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 32));
                    } else if (key == "fOverlayBrightness") {
                        g_config.overlayBrightness = std::clamp(std::stof(val), 0.0f, 2.0f);
                    } else if (key == "fOverlayBreath") {
                        g_config.overlayBreath = std::clamp(std::stof(val), 0.0f, 0.10f);
                    } else if (key == "fOverlayPulse") {
                        g_config.overlayPulse = std::clamp(std::stof(val), 0.0f, 1.5f);
                    } else if (key == "uOverlayPulseMs") {
                        g_config.overlayPulseMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 50, 800));
                    } else if (key == "uOverlayMaxMs") {
                        g_config.overlayMaxMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 500, 15000));
                    } else if (key == "iSuppressMode") {
                        g_config.suppressMode = std::clamp(std::stoi(val), -1, 2);
                    } else if (key == "fOverlayMoteScale") {
                        g_config.overlayMoteScale = std::clamp(std::stof(val), 0.3f, 3.0f);
                    } else if (key == "bCrossFade") {
                        g_config.crossFade = (std::stoi(val) != 0);
                    } else if (key == "uCrossFadeMs") {
                        g_config.crossFadeMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 800));
                    } else if (key == "uCrossFadeHoldMs") {
                        g_config.crossFadeHoldMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 6000));
                    } else if (key == "bOverlayDebugFullscreen") {
                        g_config.overlayDebugFullscreen = (std::stoi(val) != 0);
                    } else if (key == "iVisualMode") {
                        g_config.visualMode = std::clamp(std::stoi(val), 0, 8);
                    } else if (key == "bUsePresent1ForVisual") {
                        g_config.usePresent1ForVisual = (std::stoi(val) != 0);
                    } else if (key == "iSubmitRoute") {
                        g_config.submitRoute = std::clamp(std::stoi(val), 0, 2);
                    } else if (key == "bPresentAudit") {
                        g_config.presentAudit = (std::stoi(val) != 0);
                    } else if (key == "fDeadZoomAmount") {
                        g_config.deadZoomAmount = std::clamp(std::stof(val), 0.0f, 0.15f);
                    } else if (key == "uDeadZoomMs") {
                        g_config.deadZoomMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 4000));
                    } else if (key == "iCoverTest") {
                        g_config.coverTest = std::clamp(std::stoi(val), 0, 4);
                    } else if (key == "bCancelFadeBackAfterCut") {
                        g_config.cancelFadeBackAfterCut = (std::stoi(val) != 0);
                    } else if (key == "fDeadBlurPx") {
                        g_config.deadBlurPx = std::clamp(std::stof(val), 0.0f, 120.0f);
                    } else if (key == "uProbeFrames") {
                        g_config.probeFrames = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 1, 600));
                    } else if (key == "bNoBlackForWalkThrough") {
                        g_config.noBlackForWalkThrough = (std::stoi(val) != 0);
                    } else if (key == "bGlobalMenuSuppression") {
                        g_config.globalMenuSuppression = (std::stoi(val) != 0);
                    } else if (key == "fBaseAltitudeZ") {
                        g_config.baseAltitudeZ = std::clamp(std::stof(val), 4000.0f, 60000.0f);
                    } else if (key == "fSlotStepZ") {
                        g_config.slotStepZ = std::clamp(std::stof(val), 1500.0f, 10000.0f);
                    } else if (key == "fBlinkStrength") {
                        g_config.blinkStrength = std::clamp(std::stof(val), 0.1f, 1.0f);
                    } else if (key == "uDoorOpenDelayMs") {
                        g_config.doorOpenDelayMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 1000));
                    } else if (key == "uMaxRefsPerCell") {
                        g_config.maxRefsPerCell = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 50, 5000));
                    } else if (key == "uMaxPocketSlots") {
                        g_config.maxPocketSlots = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 1, 32));
                    }
                } else if (section == "ExcludeCells") {
                    g_excludeSpecs.push_back(key);
                } else if (section == "ForceRealTransition") {
                    g_forceRealSpecs.push_back(key);
                }
            } catch (...) {
                logger::warn("Failed to parse INI entry: {}={}", key, val);
            }
        }
        LoadLang();
        logger::info("Config loaded: enabled={} baseZ={:.0f} stepZ={:.0f} maxRefs={} maxSlots={} moveActors={}",
            g_config.enabled, g_config.baseAltitudeZ, g_config.slotStepZ,
            g_config.maxRefsPerCell, g_config.maxPocketSlots, g_config.moveActorsToPocket);
    }

    void ResolveExcludedCells()
    {
        g_excludeCells.clear();
        for (const auto* list : { &g_excludeSpecs, &g_forceRealSpecs }) {
            for (const auto& spec : *list) {
            try {
                if (spec.starts_with("0x") || spec.starts_with("0X")) {
                    const auto fid = static_cast<RE::FormID>(std::stoul(spec, nullptr, 16));
                    if (fid != 0) {
                        g_excludeCells.insert(fid);
                    }
                } else if (auto* form = RE::TESForm::LookupByEditorID(spec)) {
                    g_excludeCells.insert(form->GetFormID());
                }
                } catch (...) {
                }
            }
        }
        logger::info("[LF] routing: {} cell spec(s) resolved to REAL transitions "
                     "({} excluded + {} scripted)",
            g_excludeCells.size(), g_excludeSpecs.size(), g_forceRealSpecs.size());
    }

    // =========================================================================
    //  Auto-Indexed Blueprint Database (Data/SKSE/Plugins/LoadingFlow_Blueprints.bin)
    //  P1-4: Uses an immutable snapshot published under g_bpMutex so main-thread
    //  and render-thread readers never race with Force Rebuild Index, and Force
    //  Rebuild runs on the SKSE task thread instead of stalling the UI render thread.
    // =========================================================================
    using BlueprintDoorDisk = PocketBlueprint::DoorDisk;
    using BlueprintRefDisk = PocketBlueprint::RefDisk;
    using BlueprintActorDisk = PocketBlueprint::ActorDisk;
    using BlueprintCell = PocketBlueprint::CellBlueprint;

    struct BlueprintSnapshot
    {
        std::unordered_map<RE::FormID, BlueprintDoorDisk> doors;
        std::unordered_map<RE::FormID, BlueprintCell>     cells;
    };

    std::mutex                             g_bpMutex;
    std::shared_ptr<const BlueprintSnapshot> g_bpSnapshot = std::make_shared<BlueprintSnapshot>();
    std::atomic<std::size_t>               g_bpCellCount{ 0 };
    std::atomic<std::size_t>               g_bpDoorCount{ 0 };
    std::atomic<std::uint64_t>             g_currentFingerprint{ 0 };
    std::atomic<bool>                      g_cacheHitOnStartup{ false };
    std::atomic<bool>                      g_rebuildInProgress{ false };

    std::shared_ptr<const BlueprintSnapshot> GetBlueprintSnapshot()
    {
        std::lock_guard<std::mutex> lk(g_bpMutex);
        return g_bpSnapshot;
    }

    void EnsureBlueprintsLoaded(bool a_forceRebuild = false)
    {
        static constexpr const char* kCachePath = "Data/SKSE/Plugins/LoadingFlow_Blueprints.bin";
        std::vector<const RE::TESFile*> loadedFiles;
        const std::uint64_t fp = PocketBlueprint::ComputeLoadOrderFingerprint(loadedFiles, g_config.maxRefsPerCell);
        g_currentFingerprint.store(fp);

        auto nextSnap = std::make_shared<BlueprintSnapshot>();
        if (!a_forceRebuild && PocketBlueprint::TryLoadCachedBlueprints(kCachePath, fp, nextSnap->doors, nextSnap->cells)) {
            g_cacheHitOnStartup.store(true);
            g_bpCellCount.store(nextSnap->cells.size());
            g_bpDoorCount.store(nextSnap->doors.size());
            {
                std::lock_guard<std::mutex> lk(g_bpMutex);
                g_bpSnapshot = nextSnap;
            }
            logger::info("[LF] Blueprint cache HIT (fingerprint=0x{:016X}, {} mods): "
                         "loaded {} interior cells, {} doors without re-indexing",
                fp, loadedFiles.size(), nextSnap->cells.size(), nextSnap->doors.size());
            return;
        }

        g_cacheHitOnStartup.store(false);
        logger::info("[LF] Blueprint cache {} (fingerprint=0x{:016X}, {} mods) -> auto-indexing CELL records...",
            a_forceRebuild ? "FORCE REBUILD requested" : "MISS / load order changed",
            fp, loadedFiles.size());
        PocketBlueprint::RebuildAndSaveBlueprints(kCachePath, fp, loadedFiles, g_config.maxRefsPerCell, nextSnap->doors, nextSnap->cells);
        g_bpCellCount.store(nextSnap->cells.size());
        g_bpDoorCount.store(nextSnap->doors.size());
        {
            std::lock_guard<std::mutex> lk(g_bpMutex);
            g_bpSnapshot = nextSnap;
        }
    }

    // =========================================================================
    //  Pocket Slot & Runtime State
    // =========================================================================
    struct MovedActorRecord
    {
        RE::ActorHandle actorHandle;
        RE::FormID      actorFormID = 0;
        RE::FormID      origCellID = 0;
        RE::NiPoint3    origPos{};
        RE::NiPoint3    origRot{};
    };

    struct PocketInstance
    {
        std::uint32_t                    slotIndex = 0;
        RE::FormID                       interiorCellID = 0;
        std::string                      interiorEditorID;
        std::string                      interiorFullName;
        RE::FormID                       hostCellID = 0;
        RE::FormID                       hostWorldID = 0;
        RE::FormID                       locationID = 0;
        RE::NiPoint3                     pocketOrigin{};
        RE::NiPoint3                     boundMin{};
        RE::NiPoint3                     boundMax{};
        RE::NiPoint3                     returnWorldPos{};
        RE::NiPoint3                     returnWorldRot{};
        bool                             hasReturnWorldPos = false;
        std::vector<RE::ObjectRefHandle> spawnedRefs;
        std::vector<MovedActorRecord>    movedActors;
        std::vector<RE::FormID>          clonedDoorIDs;
        std::vector<RE::FormID>          clonedContIDs;
        std::size_t                      clonedStaticCount = 0;
        std::size_t                      clonedDoorCount = 0;
        std::size_t                      clonedContainerCount = 0;
        bool                             builtFromBlueprint = false;
        std::uint64_t                    lastUsed = 0;  // P0-2: LRU stamp for slot recycling
    };

    // defined further down; needed here so LRU eviction can send that pocket's NPCs home first
    void TransferPocketActors(PocketInstance& a_inst, bool a_intoPocket);

    // P0-1: recursive by design. The hooked Activate / condition-function hooks can re-enter this
    // critical section on the SAME thread (CreateReferenceAtLocation -> cell attach -> activate,
    // SetPosition -> linked-ref activate, BGSOpenCloseForm -> open/close events). A plain std::mutex
    // turned every such re-entry into a hard game-thread deadlock.
    std::recursive_mutex                           g_mutex;
    std::uint64_t                                  g_slotTick = 0;  // P0-2: monotonic LRU clock
    std::unordered_map<RE::FormID, PocketInstance> g_pockets;
    std::unordered_map<RE::FormID, RE::FormID>     g_clonedDoorToOriginalDoor;
    std::unordered_map<RE::FormID, RE::FormID>     g_clonedContToOriginalCont;
    std::atomic<RE::FormID>                        g_activeLoadingFlowID{ 0 };
    std::atomic<std::uint32_t>                     g_transitionCount{ 0 };
    std::atomic<bool>                              g_bypassContainerHook{ false };
    std::atomic<bool>                              g_transitionInProgress{ false };
    RE::NiPoint3                                   g_lastExteriorReturnPos{};
    RE::NiPoint3                                   g_lastExteriorReturnRot{};
    std::atomic<bool>                              g_hasExteriorReturnPos{ false };

    RE::TESObjectSTAT*         g_blackPlaneStat = nullptr;
    RE::TESImageSpaceModifier* g_fadeImod = nullptr;
    RE::TESImageSpaceModifier* g_fadeBackImod = nullptr;

    // v1.13.0 UI-layer cover (defined far below, used in the transition handlers above it)
    void RegisterCoverMenu();
    void SetCoverAlpha(float a_alpha01);
    void SetRuntimeCoverAlpha(float a_alpha01);
    // v1.14.0 iCoverTest=4: keep the Loading Menu alive and cover it with our own quad.
    std::atomic<bool> g_coverUiWanted{ false };
    // v1.15.0: cut 之后不再立刻撤覆盖,改为渐隐 —— 既不会露出引擎保留的旧帧,
    // 也让引擎那 ~100 ms 的 fade-back(反向白闪的来源)藏在覆盖之下。
    std::atomic<std::int64_t> g_coverFadeStart{ 0 };
    std::atomic<int>          g_coverFadeMs{ 0 };
    // v1.16.0: 磨玻璃纹理 —— 需要拿到"最后一帧"的像素。
    std::atomic<void*>        g_lastSwapChain{ nullptr };
    constexpr int             kCoverW = 40;    // 采样网格;放大后由双线性插值补成模糊
    constexpr int             kCoverH = 22;
    std::uint32_t             g_coverPx[kCoverW * kCoverH]{};
    bool                      g_coverPxValid = false;
    // 0x000F756E = FadeToBlackHoldImod -- the engine's own "stay black" stage. Verified by dumping
    // all 263 IMAD records from Skyrim.esm + DLCs; using it is cleaner than re-triggering
    // FadeToBlackImod to fake a hold.
    RE::TESImageSpaceModifier* g_fadeHoldImod = nullptr;

    void CacheForms()
    {
        if (!g_blackPlaneStat) {
            g_blackPlaneStat = RE::TESForm::LookupByID<RE::TESObjectSTAT>(0x000F13C4);
        }
        if (!g_fadeImod) {
            g_fadeImod = RE::TESForm::LookupByID<RE::TESImageSpaceModifier>(0x000F756D);
        }
        // P2-10: Cache 0x000F756F (FadeToBlackBackImod) so we don't LookupByID on every transition
        if (!g_fadeBackImod) {
            g_fadeBackImod = RE::TESForm::LookupByID<RE::TESImageSpaceModifier>(0x000F756F);
        }
        if (!g_fadeHoldImod) {
            g_fadeHoldImod = RE::TESForm::LookupByID<RE::TESImageSpaceModifier>(0x000F756E);
        }
    }

    // Check if a door is a frameless cave/mine auto-load trigger or hidden portal
    bool IsAutoOrFramelessDoor(RE::TESObjectREFR* a_doorRef)
    {
        if (!a_doorRef) {
            return true;
        }
        auto* baseObj = a_doorRef->GetBaseObject();
        auto* doorForm = baseObj ? baseObj->As<RE::TESObjectDOOR>() : nullptr;
        if (!doorForm) {
            return true;
        }
        if (doorForm->flags.any(RE::TESObjectDOOR::Flag::kAutomatic, RE::TESObjectDOOR::Flag::kHidden)) {
            return true;
        }
        return !doorForm->HasWorldModel();
    }

    // =========================================================================
    //  L2: fake cell / location events (opt-in)
    //  ScriptEventSourceHolder is the source that feeds BOTH SKSE plugin sinks and SkyrimVM (which
    //  turns these into Papyrus OnCellAttach / OnCellLoad / OnLocationChange), so sending here
    //  reaches quest fragments and alias scripts. It also reaches every other mod's sink, hence the
    //  INI switch and a single shot per transition.
    // =========================================================================
    RE::TESObjectCELL* LookupCellOrNull(RE::FormID a_id)
    {
        return a_id != 0 ? RE::TESForm::LookupByID<RE::TESObjectCELL>(a_id) : nullptr;
    }

    void FireFakeCellEvents(RE::TESObjectCELL* a_fromCell, RE::TESObjectCELL* a_toCell, bool a_entering)
    {
        if (!g_config.fakeLocationEvents && !g_config.fakeCellLoadEvents) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
        if (!player || !holder) {
            return;
        }
        RE::BGSLocation* fromLoc = a_fromCell ? a_fromCell->GetLocation() : nullptr;
        RE::BGSLocation* toLoc = a_toCell ? a_toCell->GetLocation() : nullptr;

        if (g_config.fakeCellLoadEvents) {
            // NOTE: engine semantics (Nexus "OnCellAttach / OnCellDetach" thread) is cell-scoped --
            // "the cell enters the world". This event carries no cell field, so how far it reaches is
            // determined by how the VM resolves the reference. Kept behind its own switch.
            RE::TESCellAttachDetachEvent attachEvt{};
            attachEvt.reference = RE::NiPointer<RE::TESObjectREFR>(player);
            attachEvt.attached = a_entering;
            holder->SendEvent(&attachEvt);

            if (a_entering && a_toCell) {
                RE::TESCellFullyLoadedEvent loadedEvt{};
                loadedEvt.cell = a_toCell;
                holder->SendEvent(&loadedEvt);
            }
        }

        if (g_config.fakeLocationEvents) {
            RE::TESActorLocationChangeEvent locEvt{};
            locEvt.actor = RE::NiPointer<RE::TESObjectREFR>(player);
            locEvt.oldLoc = a_entering ? fromLoc : toLoc;
            locEvt.newLoc = a_entering ? toLoc : fromLoc;
            holder->SendEvent(&locEvt);
        }

        logger::info("[LF] FAKE events ({}) cell 0x{:08X} -> 0x{:08X} | loc 0x{:08X} -> 0x{:08X} | sent:{}",
            a_entering ? "enter" : "exit",
            a_fromCell ? a_fromCell->GetFormID() : 0,
            a_toCell ? a_toCell->GetFormID() : 0,
            fromLoc ? fromLoc->GetFormID() : 0,
            toLoc ? toLoc->GetFormID() : 0,
            g_config.fakeCellLoadEvents
                ? (g_config.fakeLocationEvents ? " attach+loaded+location" : " attach+loaded")
                : " location");
    }

    // Stage 1 (T=0ms): Play door sound, crack door/trapdoor open slightly, play player hand reach, and start fade-in
    void BeginDoorTransitionVisuals(RE::TESObjectREFR* a_doorRef, bool a_isAutoCaveDoor)
    {
        if (a_doorRef && g_config.playDoorSoundAndAnim) {
            auto* baseObj = a_doorRef->GetBaseObject();
            auto* doorForm = baseObj ? baseObj->As<RE::TESObjectDOOR>() : nullptr;
            if (doorForm && doorForm->openSound) {
                if (auto* audio = RE::BSAudioManager::GetSingleton()) {
                    audio->Play(doorForm->openSound->GetFormID());
                }
            }
            if (!a_isAutoCaveDoor) {
                if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                    player->NotifyAnimationGraph("IdleActivateDoor");
                }
                RE::BGSOpenCloseForm::SetOpenState(a_doorRef, true, false);
            }
        }
        if (g_config.triggerBlinkImod) {
            CacheForms();
            if (g_fadeImod) {
                RE::ImageSpaceModifierInstanceForm::Trigger(g_fadeImod, g_config.blinkStrength, nullptr);
            }
        }
    }

    // Stage 2 (T=delayMs, at peak dark frame): Snap source door closed so it isn't left ajar, and fade back from black
    void FinishDoorTransitionVisuals(RE::ObjectRefHandle a_sourceDoorHandle, bool a_isAutoCaveDoor)
    {
        if (!a_isAutoCaveDoor && g_config.playDoorSoundAndAnim) {
            if (auto doorRef = a_sourceDoorHandle.get()) {
                RE::BGSOpenCloseForm::SetOpenState(doorRef.get(), false, true);
            }
        }
        if (g_config.triggerBlinkImod) {
            CacheForms();
            if (g_fadeImod) {
                RE::ImageSpaceModifierInstanceForm::Stop(g_fadeImod);
            }
            if (g_fadeBackImod) {
                RE::ImageSpaceModifierInstanceForm::Trigger(g_fadeBackImod, g_config.blinkStrength, nullptr);
            }
        }
    }

    bool ShouldCloneBaseObject(RE::TESBoundObject* a_base, RE::TESObjectREFR* a_origRef)
    {
        if (!a_base || !a_origRef) {
            return false;
        }
        if (a_origRef->IsDisabled() || a_origRef->IsDeleted()) {
            return false;
        }
        if (auto* xEsp = a_origRef->extraList.GetByType<RE::ExtraEnableStateParent>()) {
            if (auto parentPtr = xEsp->parent.get()) {
                if (parentPtr->IsDisabled()) {
                    return false;
                }
            }
        }
        const auto ft = a_base->GetFormType();
        const auto fid = a_base->GetFormID();
        if (fid == 0x0000003B || fid == 0x00000034 || fid == 0x00000003 || fid == 0x00000032) {
            return false;
        }
        switch (ft) {
        case RE::FormType::Static:
        case RE::FormType::MovableStatic:
        case RE::FormType::Furniture:
        case RE::FormType::Door:
        case RE::FormType::Container:
        case RE::FormType::Light:
        case RE::FormType::Misc:
        case RE::FormType::Book:
        case RE::FormType::Flora:
        case RE::FormType::Tree:
        case RE::FormType::AlchemyItem:
        case RE::FormType::Ingredient:
        case RE::FormType::Weapon:
        case RE::FormType::Armor:
        case RE::FormType::Ammo:
        case RE::FormType::KeyMaster:
        case RE::FormType::SoulGem:
        case RE::FormType::Scroll:
        case RE::FormType::Apparatus:
            return true;
        case RE::FormType::Activator:
            if (a_origRef->extraList.HasType(RE::ExtraDataType::kPrimitive) && !a_base->HasWorldModel()) {
                return false;
            }
            return a_base->HasWorldModel();
        default:
            return false;
        }
    }

    void SpawnBlackShell(PocketInstance& a_inst, RE::TESObjectCELL* a_hostCell, RE::TESWorldSpace* a_hostWorld)
    {
        if (!g_config.spawnBlackBoxShell) {
            return;
        }
        CacheForms();
        if (!g_blackPlaneStat) {
            return;
        }
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            return;
        }
        const float cx = 0.5f * (a_inst.boundMin.x + a_inst.boundMax.x);
        const float cy = 0.5f * (a_inst.boundMin.y + a_inst.boundMax.y);
        const float cz = 0.5f * (a_inst.boundMin.z + a_inst.boundMax.z);
        const float pad = 400.0f;
        const float minX = a_inst.boundMin.x - pad;
        const float maxX = a_inst.boundMax.x + pad;
        const float minY = a_inst.boundMin.y - pad;
        const float maxY = a_inst.boundMax.y + pad;
        const float minZ = a_inst.boundMin.z - pad;
        const float maxZ = a_inst.boundMax.z + pad;

        // P1-8: BlackPlane01 (0x000F13C4) has local OBND x:[-256..0], y:[-256..0], z:[0..1] (corner origin).
        // Compute a dynamic scale that covers the full span of this room, and offset each plane's placement
        // by its rotated OBND half-span so the plane center aligns with the face center (cx, cy, cz).
        const float spanX = maxX - minX;
        const float spanY = maxY - minY;
        const float spanZ = maxZ - minZ;
        const float maxSpan = (std::max)({ spanX, spanY, spanZ, 2560.0f });
        const float neededScale = std::clamp((maxSpan * 1.25f) / 256.0f, 10.0f, 40.0f);
        const auto  refScalePct = static_cast<std::uint16_t>( neededScale * 100.0f );
        const float halfSide = 128.0f * neededScale;

        struct PlaneSpec
        {
            RE::NiPoint3 pos;
            RE::NiPoint3 rot;
        };
        constexpr float kHalfPi = 1.5707963f;
        constexpr float kPi = 3.1415926f;
        const PlaneSpec planes[] = {
            { { cx + halfSide, cy - halfSide, maxZ }, { kPi, 0.0f, 0.0f } },
            { { cx + halfSide, cy + halfSide, minZ }, { 0.0f, 0.0f, 0.0f } },
            { { cx + halfSide, maxY, cz - halfSide }, { kHalfPi, 0.0f, 0.0f } },
            { { cx + halfSide, minY, cz + halfSide }, { -kHalfPi, 0.0f, 0.0f } },
            { { maxX, cy + halfSide, cz - halfSide }, { 0.0f, -kHalfPi, 0.0f } },
            { { minX, cy + halfSide, cz + halfSide }, { 0.0f, kHalfPi, 0.0f } }
        };

        for (const auto& pl : planes) {
            auto handle = dh->CreateReferenceAtLocation(
                g_blackPlaneStat, pl.pos, pl.rot, a_hostCell, a_hostWorld,
                nullptr, nullptr, RE::ObjectRefHandle(), false, true);
            if (auto ref = handle.get()) {
                ref->GetReferenceRuntimeData().refScale = refScalePct;
                a_inst.spawnedRefs.push_back(handle);
            }
        }
    }

    PocketInstance* GetOrCreatePocket(RE::FormID a_interiorCellID, RE::TESObjectCELL* a_interiorCell,
        RE::TESObjectCELL* a_hostCell, RE::TESWorldSpace* a_hostWorld, const RE::NiPoint3& a_anchorXY,
        const std::shared_ptr<const BlueprintSnapshot>& a_bpSnap)
    {
        if (a_interiorCellID == 0 || !a_hostCell) {
            return nullptr;
        }
        auto it = g_pockets.find(a_interiorCellID);
        if (it != g_pockets.end()) {
            bool valid = !it->second.spawnedRefs.empty();
            if (valid) {
                auto firstRef = it->second.spawnedRefs.front().get();
                if (!firstRef || firstRef->IsDeleted() || firstRef->IsDisabled()) {
                    valid = false;
                }
            }
            if (valid) {
                it->second.lastUsed = ++g_slotTick;
                return &it->second;
            }
            g_pockets.erase(it);
        }

        // P0-2: reclaim the least-recently-used slot instead of silently degrading to a black
        // loading screen once the player has visited more interiors than uMaxPocketSlots.
        if (g_pockets.size() >= g_config.maxPocketSlots) {
            const RE::FormID activeID = g_activeLoadingFlowID.load();
            auto victimIt = g_pockets.end();
            std::uint64_t victimTick = (std::numeric_limits<std::uint64_t>::max)();
            for (auto vIt = g_pockets.begin(); vIt != g_pockets.end(); ++vIt) {
                if (vIt->first == activeID) {
                    continue;  // never recycle the pocket the player is currently standing in
                }
                if (vIt->second.lastUsed < victimTick) {
                    victimTick = vIt->second.lastUsed;
                    victimIt = vIt;
                }
            }
            if (victimIt != g_pockets.end()) {
                // P1-1: send that pocket's NPCs home before the mapping tables are torn down.
                TransferPocketActors(victimIt->second, false);
                std::size_t freed = 0;
                for (auto& h : victimIt->second.spawnedRefs) {
                    if (auto ref = h.get()) {
                        ref->Disable();
                        ref->SetDelete(true);
                        ++freed;
                    }
                }
                for (const auto fid : victimIt->second.clonedDoorIDs) {
                    g_clonedDoorToOriginalDoor.erase(fid);
                }
                for (const auto fid : victimIt->second.clonedContIDs) {
                    g_clonedContToOriginalCont.erase(fid);
                }
                logger::info("Recycling Pocket Slot #{} cell=0x{:08X} ({}) [LRU] to free a slot for 0x{:08X}: {} refs marked for deletion",
                    victimIt->second.slotIndex, victimIt->first, victimIt->second.interiorEditorID,
                    a_interiorCellID, freed);
                g_pockets.erase(victimIt);
            }
        }

        if (g_pockets.size() >= g_config.maxPocketSlots) {
            logger::warn("Pocket slot limit ({}) reached and no slot could be recycled (player inside pocket); "
                         "falling back to vanilla transition for cell 0x{:08X}",
                g_config.maxPocketSlots, a_interiorCellID);
            return nullptr;
        }

        // First attempt: collect live references from a_interiorCell if its temporary STATs are loaded
        std::vector<RE::TESObjectREFR*> origRefs;
        std::vector<RE::Actor*>         origActors;
        std::size_t                     liveStatCount = 0;

        if (a_interiorCell) {
            a_interiorCell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
                if (!a_ref || a_ref->IsDeleted() || a_ref->IsDisabled()) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                if (auto* actor = a_ref->As<RE::Actor>()) {
                    if (!actor->IsPlayerRef() && !actor->IsDead(false)) {
                        origActors.push_back(actor);
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                auto* base = a_ref->GetBaseObject();
                if (ShouldCloneBaseObject(base, a_ref)) {
                    origRefs.push_back(a_ref);
                    if (base->GetFormType() == RE::FormType::Static) {
                        ++liveStatCount;
                    }
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }

        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            return nullptr;
        }

        const auto slotIdx = static_cast<std::uint32_t>(g_pockets.size());
        const float slotZ = g_config.baseAltitudeZ + static_cast<float>(slotIdx) * g_config.slotStepZ;

        // Branch 1: Live memory has full room geometry (liveStatCount >= 5)
        if (liveStatCount >= 5 && origRefs.size() <= g_config.maxRefsPerCell) {
            RE::NiPoint3 localMin{ 1e9f, 1e9f, 1e9f };
            RE::NiPoint3 localMax{ -1e9f, -1e9f, -1e9f };
            for (auto* r : origRefs) {
                const auto p = r->GetPosition();
                localMin.x = (std::min)(localMin.x, p.x);
                localMin.y = (std::min)(localMin.y, p.y);
                localMin.z = (std::min)(localMin.z, p.z);
                localMax.x = (std::max)(localMax.x, p.x);
                localMax.y = (std::max)(localMax.y, p.y);
                localMax.z = (std::max)(localMax.z, p.z);
            }
            const float localCenterX = 0.5f * (localMin.x + localMax.x);
            const float localCenterY = 0.5f * (localMin.y + localMax.y);

            PocketInstance inst{};
            inst.slotIndex = slotIdx;
            inst.interiorCellID = a_interiorCellID;
            inst.interiorEditorID = (a_interiorCell && a_interiorCell->GetFormEditorID()) ? a_interiorCell->GetFormEditorID() : "";
            inst.interiorFullName = (a_interiorCell && a_interiorCell->GetFullName()) ? a_interiorCell->GetFullName() : "";
            inst.hostCellID = a_hostCell->GetFormID();
            inst.hostWorldID = a_hostWorld ? a_hostWorld->GetFormID() : 0;
            inst.locationID = (a_interiorCell && a_interiorCell->GetLocation()) ? a_interiorCell->GetLocation()->GetFormID() : 0;
            inst.pocketOrigin = { a_anchorXY.x - localCenterX, a_anchorXY.y - localCenterY, slotZ };
            inst.boundMin = { localMin.x + inst.pocketOrigin.x, localMin.y + inst.pocketOrigin.y, localMin.z + inst.pocketOrigin.z };
            inst.boundMax = { localMax.x + inst.pocketOrigin.x, localMax.y + inst.pocketOrigin.y, localMax.z + inst.pocketOrigin.z };
            inst.builtFromBlueprint = false;

            for (auto* origRef : origRefs) {
                auto* baseObj = origRef->GetBaseObject();
                if (!baseObj) {
                    continue;
                }
                const RE::NiPoint3 newPos = origRef->GetPosition() + inst.pocketOrigin;
                const RE::NiPoint3 newRot = origRef->GetAngle();

                auto handle = dh->CreateReferenceAtLocation(
                    baseObj, newPos, newRot, a_hostCell, a_hostWorld,
                    nullptr, nullptr, RE::ObjectRefHandle(), false, true);
                auto newRef = handle.get();
                if (!newRef) {
                    continue;
                }
                newRef->GetReferenceRuntimeData().refScale = origRef->GetReferenceRuntimeData().refScale;
                if (auto* owner = origRef->GetOwner()) {
                    newRef->extraList.SetOwner(owner);
                } else if (a_interiorCell && a_interiorCell->GetOwner()) {
                    newRef->extraList.SetOwner(a_interiorCell->GetOwner());
                }

                const auto ft = baseObj->GetFormType();
                if (ft == RE::FormType::Door) {
                    if (origRef->extraList.HasType(RE::ExtraDataType::kTeleport) || (a_bpSnap && a_bpSnap->doors.count(origRef->GetFormID()) != 0)) {
                        g_clonedDoorToOriginalDoor[newRef->GetFormID()] = origRef->GetFormID();
                        inst.clonedDoorIDs.push_back(newRef->GetFormID());
                        ++inst.clonedDoorCount;
                    }
                } else if (ft == RE::FormType::Container) {
                    g_clonedContToOriginalCont[newRef->GetFormID()] = origRef->GetFormID();
                    inst.clonedContIDs.push_back(newRef->GetFormID());
                    ++inst.clonedContainerCount;
                } else {
                    ++inst.clonedStaticCount;
                }
                inst.spawnedRefs.push_back(handle);
            }

            SpawnBlackShell(inst, a_hostCell, a_hostWorld);

            if (g_config.moveActorsToPocket) {
                for (auto* actor : origActors) {
                    if (!actor) {
                        continue;
                    }
                    MovedActorRecord rec{};
                    rec.actorHandle = actor->GetHandle();
                    rec.actorFormID = actor->GetFormID();
                    rec.origCellID = a_interiorCellID;
                    rec.origPos = actor->GetPosition();
                    rec.origRot = actor->GetAngle();
                    inst.movedActors.push_back(rec);
                }
            }

            logger::info("Built Pocket Slot #{} [LIVE] for cell 0x{:08X} ({}) at origin=({:.1f},{:.1f},{:.1f}): "
                         "statics={} doors={} containers={} actors={}",
                inst.slotIndex, inst.interiorCellID, inst.interiorEditorID,
                inst.pocketOrigin.x, inst.pocketOrigin.y, inst.pocketOrigin.z,
                inst.clonedStaticCount, inst.clonedDoorCount, inst.clonedContainerCount, inst.movedActors.size());

            auto [insIt, _] = g_pockets.emplace(a_interiorCellID, std::move(inst));
            return &insIt->second;
        }

        // Branch 2: Live cell temporary geometry is not in memory yet -> build from offline Blueprint!
        if (!a_bpSnap) {
            logger::info("Cell 0x{:08X} not ready in live memory (stats={}) and no Blueprint snapshot loaded; using vanilla load",
                a_interiorCellID, liveStatCount);
            return nullptr;
        }
        const auto bpIt = a_bpSnap->cells.find(a_interiorCellID);
        if (bpIt == a_bpSnap->cells.end() || bpIt->second.refs.empty() || bpIt->second.refs.size() > g_config.maxRefsPerCell) {
            logger::info("Cell 0x{:08X} not ready in live memory (stats={}) and not in Blueprint table; using vanilla load",
                a_interiorCellID, liveStatCount);
            return nullptr;
        }

        const BlueprintCell& bp = bpIt->second;
        RE::NiPoint3 localMin{ 1e9f, 1e9f, 1e9f };
        RE::NiPoint3 localMax{ -1e9f, -1e9f, -1e9f };
        for (const auto& r : bp.refs) {
            localMin.x = (std::min)(localMin.x, r.pos[0]);
            localMin.y = (std::min)(localMin.y, r.pos[1]);
            localMin.z = (std::min)(localMin.z, r.pos[2]);
            localMax.x = (std::max)(localMax.x, r.pos[0]);
            localMax.y = (std::max)(localMax.y, r.pos[1]);
            localMax.z = (std::max)(localMax.z, r.pos[2]);
        }
        const float localCenterX = 0.5f * (localMin.x + localMax.x);
        const float localCenterY = 0.5f * (localMin.y + localMax.y);

        PocketInstance inst{};
        inst.slotIndex = slotIdx;
        inst.interiorCellID = a_interiorCellID;
        inst.interiorEditorID = bp.edid;
        inst.interiorFullName = (a_interiorCell && a_interiorCell->GetFullName()) ? a_interiorCell->GetFullName() : bp.edid;
        inst.hostCellID = a_hostCell->GetFormID();
        inst.hostWorldID = a_hostWorld ? a_hostWorld->GetFormID() : 0;
        inst.locationID = bp.locationID;
        inst.pocketOrigin = { a_anchorXY.x - localCenterX, a_anchorXY.y - localCenterY, slotZ };
        inst.boundMin = { localMin.x + inst.pocketOrigin.x, localMin.y + inst.pocketOrigin.y, localMin.z + inst.pocketOrigin.z };
        inst.boundMax = { localMax.x + inst.pocketOrigin.x, localMax.y + inst.pocketOrigin.y, localMax.z + inst.pocketOrigin.z };
        inst.builtFromBlueprint = true;

        RE::TESForm* cellOwnerForm = bp.ownerID ? RE::TESForm::LookupByID(bp.ownerID) : nullptr;

        for (const auto& r : bp.refs) {
            auto* form = RE::TESForm::LookupByID(r.baseID);
            auto* baseObj = form ? form->As<RE::TESBoundObject>() : nullptr;
            if (!baseObj) {
                continue;
            }
            const RE::NiPoint3 newPos{
                r.pos[0] + inst.pocketOrigin.x,
                r.pos[1] + inst.pocketOrigin.y,
                r.pos[2] + inst.pocketOrigin.z
            };
            const RE::NiPoint3 newRot{ r.rot[0], r.rot[1], r.rot[2] };

            auto handle = dh->CreateReferenceAtLocation(
                baseObj, newPos, newRot, a_hostCell, a_hostWorld,
                nullptr, nullptr, RE::ObjectRefHandle(), false, true);
            auto newRef = handle.get();
            if (!newRef) {
                continue;
            }
            newRef->GetReferenceRuntimeData().refScale = r.scale;
            if (r.ownerID != 0) {
                if (auto* own = RE::TESForm::LookupByID(r.ownerID)) {
                    newRef->extraList.SetOwner(own);
                }
            } else if (cellOwnerForm) {
                newRef->extraList.SetOwner(cellOwnerForm);
            }

            if ((r.flags & 1) != 0) {
                g_clonedDoorToOriginalDoor[newRef->GetFormID()] = r.origRefID;
                inst.clonedDoorIDs.push_back(newRef->GetFormID());
                ++inst.clonedDoorCount;
            } else if ((r.flags & 2) != 0) {
                g_clonedContToOriginalCont[newRef->GetFormID()] = r.origRefID;
                inst.clonedContIDs.push_back(newRef->GetFormID());
                ++inst.clonedContainerCount;
            } else {
                ++inst.clonedStaticCount;
            }
            inst.spawnedRefs.push_back(handle);
        }

        SpawnBlackShell(inst, a_hostCell, a_hostWorld);

        if (g_config.moveActorsToPocket) {
            for (const auto& a : bp.actors) {
                if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(a.achrRefID)) {
                    if (!actor->IsDead(false) && !actor->IsDisabled()) {
                        MovedActorRecord rec{};
                        rec.actorHandle = actor->GetHandle();
                        rec.actorFormID = actor->GetFormID();
                        rec.origCellID = a_interiorCellID;
                        rec.origPos = { a.pos[0], a.pos[1], a.pos[2] };
                        rec.origRot = { a.rot[0], a.rot[1], a.rot[2] };
                        inst.movedActors.push_back(rec);
                    }
                }
            }
        }

        logger::info("Built Pocket Slot #{} [BLUEPRINT] for cell 0x{:08X} ({}) at origin=({:.1f},{:.1f},{:.1f}): "
                     "statics={} doors={} containers={} actors={}",
            inst.slotIndex, inst.interiorCellID, inst.interiorEditorID,
            inst.pocketOrigin.x, inst.pocketOrigin.y, inst.pocketOrigin.z,
            inst.clonedStaticCount, inst.clonedDoorCount, inst.clonedContainerCount, inst.movedActors.size());

        auto [insIt, _] = g_pockets.emplace(a_interiorCellID, std::move(inst));
        return &insIt->second;
    }

    void TransferPocketActors(PocketInstance& a_inst, bool a_intoPocket)
    {
        if (!g_config.moveActorsToPocket) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }

        // P1-1: to send an NPC home we first need a reference that lives in the ORIGINAL interior
        // cell (MoveTo switches the parent cell, SetPosition alone would leave them at Z=+26000).
        // The pocket's cloned doors map back to the real doors, which are exactly such references.
        RE::TESObjectREFR* origCellAnchor = nullptr;
        if (!a_intoPocket) {
            for (const auto doorID : a_inst.clonedDoorIDs) {
                const auto mapIt = g_clonedDoorToOriginalDoor.find(doorID);
                if (mapIt == g_clonedDoorToOriginalDoor.end()) {
                    continue;
                }
                if (auto* origDoor = RE::TESForm::LookupByID<RE::TESObjectREFR>(mapIt->second)) {
                    origCellAnchor = origDoor;
                    break;
                }
            }
        }

        std::size_t restored = 0;
        std::size_t stranded = 0;
        for (const auto& rec : a_inst.movedActors) {
            auto actorPtr = rec.actorHandle.get();
            if (!actorPtr) {
                actorPtr = RE::NiPointer<RE::Actor>(RE::TESForm::LookupByID<RE::Actor>(rec.actorFormID));
            }
            if (!actorPtr || actorPtr->IsDead(false)) {
                continue;
            }
            if (a_intoPocket) {
                const RE::NiPoint3 targetPos = rec.origPos + a_inst.pocketOrigin;
                actorPtr->MoveTo(player);
                actorPtr->SetPosition(targetPos, true);
                actorPtr->SetRotationZ(rec.origRot.z);
            } else if (origCellAnchor) {
                actorPtr->MoveTo(origCellAnchor);          // back into the real interior cell
                actorPtr->SetPosition(rec.origPos, true);  // then onto its recorded spot
                actorPtr->SetRotationZ(rec.origRot.z);
                ++restored;
            } else {
                ++stranded;
            }
        }

        if (!a_intoPocket) {
            if (restored > 0) {
                logger::info("[LF] Restored {} NPC(s) to cell 0x{:08X}", restored, a_inst.interiorCellID);
            }
            if (stranded > 0) {
                logger::warn("[LF] {} NPC(s) of cell 0x{:08X} could not be restored (no original door anchor); "
                             "they are still at Pocket altitude - toggle 'Move interior NPCs' off and reload a save "
                             "that predates this transition to clear them",
                    stranded, a_inst.interiorCellID);
            }
        }
    }

    void PurgeAllPockets(bool a_teleportPlayerOutIfInside)
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        g_transitionInProgress.store(false);
        const RE::FormID activeID = g_activeLoadingFlowID.exchange(0);

        // P1-2: actually get the player out BEFORE the geometry is deleted, otherwise they are left
        // standing at Z=+26000 with the floor marked for deletion and fall to their death.
        if (a_teleportPlayerOutIfInside && activeID != 0) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player) {
                bool moved = false;
                auto it = g_pockets.find(activeID);
                if (it != g_pockets.end() && it->second.hasReturnWorldPos) {
                    // P1-1: send the NPCs of that pocket home while the mapping tables still exist.
                    TransferPocketActors(it->second, false);
                    player->SetPosition(it->second.returnWorldPos, true);
                    player->SetRotationZ(it->second.returnWorldRot.z);
                    logger::info("PurgeAllPockets: teleported player out of pocket 0x{:08X} to recorded return pos=({:.1f},{:.1f},{:.1f})",
                        activeID, it->second.returnWorldPos.x, it->second.returnWorldPos.y, it->second.returnWorldPos.z);
                    moved = true;
                }
                if (!moved && g_hasExteriorReturnPos.load()) {
                    player->SetPosition(g_lastExteriorReturnPos, true);
                    logger::info("PurgeAllPockets: no per-pocket return pos; used last exterior return pos=({:.1f},{:.1f},{:.1f})",
                        g_lastExteriorReturnPos.x, g_lastExteriorReturnPos.y, g_lastExteriorReturnPos.z);
                    moved = true;
                }
                if (!moved) {
                    const RE::NiPoint3 localPos = player->GetPosition();
                    logger::warn("PurgeAllPockets: player was inside pocket 0x{:08X} at ({:.1f},{:.1f},{:.1f}) and NO return "
                                 "position was recorded; you may fall - press ~ and use 'coc WhiterunOrigin' or reload",
                        activeID, localPos.x, localPos.y, localPos.z);
                }
            }
        }

        std::size_t deletedRefs = 0;
        for (auto& [cellID, inst] : g_pockets) {
            for (auto& h : inst.spawnedRefs) {
                if (auto ref = h.get()) {
                    ref->Disable();
                    ref->SetDelete(true);
                    ++deletedRefs;
                }
            }
        }
        g_pockets.clear();
        g_clonedDoorToOriginalDoor.clear();
        g_clonedContToOriginalCont.clear();
        if (deletedRefs > 0) {
            logger::info("Purged all pockets ({} temporary references marked for deletion)", deletedRefs);
        }
    }

    const PocketInstance* FindPocketContainingPoint(const RE::NiPoint3& a_pt)
    {
        constexpr float kMargin = 350.0f;
        for (const auto& [cellID, inst] : g_pockets) {
            if (a_pt.x >= inst.boundMin.x - kMargin && a_pt.x <= inst.boundMax.x + kMargin &&
                a_pt.y >= inst.boundMin.y - kMargin && a_pt.y <= inst.boundMax.y + kMargin &&
                a_pt.z >= inst.boundMin.z - kMargin && a_pt.z <= inst.boundMax.z + kMargin) {
                return &inst;
            }
        }
        return nullptr;
    }

    // =========================================================================
    //  Condition Function Hooks -- semantics verified against the CK wiki (ck.uesp.net):
    //    GetInCell           exact cell match only (Skyrim does NOT prefix-match)
    //    GetInSameCell       compare the cell each reference is considered to occupy
    //    GetInCurrentLoc     supplied location or any of its ancestors
    //    GetInWorldspace     always 0 while in an interior cell -> 0 inside a pocket
    //    GetInZone           parameter is an EncounterZone (0x32), not a keyword
    // =========================================================================
    RE::SCRIPT_FUNCTION::Condition_t* g_origGetInCell = nullptr;
    RE::SCRIPT_FUNCTION::Condition_t* g_origGetInSameCell = nullptr;
    RE::SCRIPT_FUNCTION::Condition_t* g_origGetInCurrentLoc = nullptr;
    RE::SCRIPT_FUNCTION::Condition_t* g_origGetInWorldspace = nullptr;
    RE::SCRIPT_FUNCTION::Condition_t* g_origGetInZone = nullptr;

    // CK wiki: GetInCurrentLoc matches "the supplied location or a location that has it as a parent".
    // Walking parentLoc by hand avoids depending on the IsParent/IsChild argument-direction convention.
    bool IsLocationOrDescendantOf(RE::BGSLocation* a_loc, RE::BGSLocation* a_ancestor)
    {
        if (!a_loc || !a_ancestor) {
            return false;
        }
        for (auto* loc = a_loc; loc; loc = loc->parentLoc) {
            if (loc == a_ancestor) {
                return true;
            }
        }
        return false;
    }

    // The cell a reference should be *considered* to be in: the pocket's interior cell while it
    // stands inside a pocket volume, otherwise its real parent cell. Caller holds g_mutex.
    RE::FormID EffectiveCellOf(RE::TESObjectREFR* a_ref)
    {
        if (!a_ref) {
            return 0;
        }
        if (const auto* inst = FindPocketContainingPoint(a_ref->GetPosition())) {
            return inst->interiorCellID;
        }
        auto* cell = a_ref->GetParentCell();
        return cell ? cell->GetFormID() : 0;
    }

    bool HookedGetInCell(RE::TESObjectREFR* a_thisObj, void* a_param1, void* a_param2, double& a_result)
    {
        if (g_origGetInCell && g_origGetInCell(a_thisObj, a_param1, a_param2, a_result) && a_result != 0.0) {
            return true;
        }
        if (a_thisObj && a_param1 && g_activeLoadingFlowID.load() != 0) {
            auto* targetCell = static_cast<RE::TESObjectCELL*>(a_param1);
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            if (const auto* inst = FindPocketContainingPoint(a_thisObj->GetPosition())) {
                // CK wiki: Skyrim's GetInCell requires an EXACT cell match -- no name prefix matching.
                if (inst->interiorCellID == targetCell->GetFormID()) {
                    a_result = 1.0;
                    return true;
                }
            }
        }
        return true;
    }

    bool HookedGetInSameCell(RE::TESObjectREFR* a_thisObj, void* a_param1, void* a_param2, double& a_result)
    {
        if (g_origGetInSameCell && g_origGetInSameCell(a_thisObj, a_param1, a_param2, a_result)) {
            if (a_result != 0.0) {
                return true;
            }
        }
        if (a_thisObj && a_param1 && g_activeLoadingFlowID.load() != 0) {
            auto* otherRef = static_cast<RE::TESObjectREFR*>(a_param1);
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            // CK wiki: "in the same cell as TargetRefID" -- so compare the cells each reference is
            // considered to occupy, not just whether both stand in the same pocket volume.
            const RE::FormID cellA = EffectiveCellOf(a_thisObj);
            const RE::FormID cellB = EffectiveCellOf(otherRef);
            if (cellA != 0 && cellA == cellB) {
                a_result = 1.0;
                return true;
            }
        }
        return true;
    }

    bool HookedGetInCurrentLoc(RE::TESObjectREFR* a_thisObj, void* a_param1, void* a_param2, double& a_result)
    {
        if (g_origGetInCurrentLoc && g_origGetInCurrentLoc(a_thisObj, a_param1, a_param2, a_result) && a_result != 0.0) {
            return true;
        }
        if (a_thisObj && a_param1 && g_activeLoadingFlowID.load() != 0) {
            auto* targetLoc = static_cast<RE::BGSLocation*>(a_param1);
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            if (const auto* inst = FindPocketContainingPoint(a_thisObj->GetPosition())) {
                RE::BGSLocation* pocketLoc = nullptr;
                if (inst->locationID != 0) {
                    pocketLoc = RE::TESForm::LookupByID<RE::BGSLocation>(inst->locationID);
                } else if (auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(inst->interiorCellID)) {
                    pocketLoc = cell->GetLocation();
                }
                if (pocketLoc && IsLocationOrDescendantOf(pocketLoc, targetLoc)) {
                    a_result = 1.0;
                    return true;
                }
            }
        }
        return true;
    }

    // L1: the pocket copy sits inside an exterior cell, so "is this ref in worldspace X" answers
    // "yes, Tamriel" even when the player is semantically standing in an interior. Inside a pocket
    // the honest answer is "no worldspace at all", because interior cells belong to none.
    bool HookedGetInWorldspace(RE::TESObjectREFR* a_thisObj, void* a_param1, void* a_param2, double& a_result)
    {
        if (g_origGetInWorldspace && g_origGetInWorldspace(a_thisObj, a_param1, a_param2, a_result) && a_result != 0.0) {
            return true;
        }
        if (!a_thisObj || g_activeLoadingFlowID.load() == 0) {
            return true;
        }
        const RE::NiPoint3 pt = a_thisObj->GetPosition();
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (FindPocketContainingPoint(pt)) {
            a_result = 0.0;
        }
        return true;
    }

    // CK wiki: "GetInZone returns a 1 if the designated target is in the Encounter Zone" -- the
    // parameter is a BGSEncounterZone (SCRIPT_PARAM_TYPE kEncounterZone = 0x32), NOT a location
    // keyword. Matching on a keyword was wrong and is exactly what the param-type gate rejected.
    bool HookedGetInZone(RE::TESObjectREFR* a_thisObj, void* a_param1, void* a_param2, double& a_result)
    {
        if (g_origGetInZone && g_origGetInZone(a_thisObj, a_param1, a_param2, a_result) && a_result != 0.0) {
            return true;
        }
        if (!a_thisObj || !a_param1 || g_activeLoadingFlowID.load() == 0) {
            return true;
        }
        auto* zoneForm = static_cast<RE::TESForm*>(a_param1);
        if (zoneForm->GetFormType() != RE::FormType::EncounterZone) {
            return true;
        }
        const RE::NiPoint3 pt = a_thisObj->GetPosition();
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        const auto* inst = FindPocketContainingPoint(pt);
        if (!inst) {
            return true;
        }
        auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(inst->interiorCellID);
        // The EncounterZone only exists in LOADED_CELL_DATA, i.e. while that cell is actually loaded.
        // A pocket built from the offline blueprint has no loaded data, so leave the vanilla answer.
        if (cell && cell->GetRuntimeData().loadedData &&
            cell->GetRuntimeData().loadedData->encounterZone == static_cast<RE::BGSEncounterZone*>(zoneForm)) {
            a_result = 1.0;
        }
        return true;
    }

    void InstallConditionHooks()
    {
        auto* cmds = RE::SCRIPT_FUNCTION::GetFirstScriptCommand();
        if (!cmds) {
            logger::warn("GetFirstScriptCommand returned null; condition hooks skipped");
            return;
        }

        struct HookSpec
        {
            const char*                        name;
            RE::SCRIPT_FUNCTION::Condition_t** origSlot;
            RE::SCRIPT_FUNCTION::Condition_t*  replacement;
            std::uint32_t                      expectedParam0;  // SCRIPT_PARAM_TYPE value
        };

        // Parameter types are what the engine will actually hand us, so every entry is gated on the
        // real signature instead of trusting the name alone.
        const HookSpec specs[] = {
            { "GetInCell",       &g_origGetInCell,       &HookedGetInCell,       0x09u },  // kCell
            { "GetInSameCell",   &g_origGetInSameCell,   &HookedGetInSameCell,   0x04u },  // kObjectRef
            { "GetInCurrentLoc", &g_origGetInCurrentLoc, &HookedGetInCurrentLoc, 0x3Du },  // kLocation
            { "GetInWorldspace", &g_origGetInWorldspace, &HookedGetInWorldspace, 0x1Bu },  // kWorldOrList
            { "GetInZone",       &g_origGetInZone,       &HookedGetInZone,       0x32u },  // kEncounterZone
        };

        std::size_t hooked = 0;
        std::size_t dumped = 0;
        for (std::uint16_t i = 0; i < RE::SCRIPT_FUNCTION::Commands::kScriptCommandsEnd; ++i) {
            auto& cmd = cmds[i];
            if (!cmd.functionName || !cmd.conditionFunction) {
                continue;
            }

            // L1 diagnostic: print the real name + parameter signature of every cell/location-flavoured
            // condition, so extending this list never depends on guesswork from the CK wiki.
            const char* n = cmd.functionName;
            if (strstr(n, "Cell") || strstr(n, "Loc") || strstr(n, "World") ||
                strstr(n, "Zone") || strstr(n, "Space") || strstr(n, "Marker")) {
                std::string sig;
                for (std::uint16_t p = 0; p < cmd.numParams && cmd.params && p < 8; ++p) {
                    sig += std::format("{}:0x{:X} ", cmd.params[p].paramName ? cmd.params[p].paramName : "?",
                        static_cast<std::uint32_t>(cmd.params[p].paramType.get()));
                }
                logger::info("[cond-dump] idx={} name={} params={} | {}", i, n, cmd.numParams, sig);
                ++dumped;
            }

            for (const auto& spec : specs) {
                if (*spec.origSlot || _stricmp(n, spec.name) != 0) {
                    continue;
                }
                const std::uint32_t param0 = (cmd.numParams > 0 && cmd.params)
                    ? static_cast<std::uint32_t>(cmd.params[0].paramType.get())
                    : 0xFFFFu;
                if (param0 != spec.expectedParam0) {
                    logger::warn("[LF] Skip hooking {}: param0 type 0x{:X}, expected 0x{:X}",
                        spec.name, param0, spec.expectedParam0);
                    continue;
                }
                *spec.origSlot = cmd.conditionFunction;
                cmd.conditionFunction = spec.replacement;
                ++hooked;
                logger::info("Installed condition hook: {} (idx={}, param0=0x{:X})", spec.name, i, param0);
            }
        }
        logger::info("[LF] Condition hooks installed: {}/{} | candidate conditions dumped: {} "
                     "(see [cond-dump] lines above)",
            hooked, sizeof(specs) / sizeof(specs[0]), dumped);
    }

    // =========================================================================
    //  Vtable Hook Helpers
    // =========================================================================
    bool IsAddressInTextSegment(const void* a_addr)
    {
        const auto& mod = REL::Module::get();
        const auto seg = mod.segment(REL::Segment::textx);
        const auto segBase = seg.address();
        const auto segSize = static_cast<std::uintptr_t>(seg.size());
        const auto addr = reinterpret_cast<std::uintptr_t>(a_addr);
        return addr >= segBase && addr < (segBase + segSize);
    }

    bool IsExecutableAddress(const void* a_addr)
    {
        if (!a_addr) {
            return false;
        }
        constexpr std::uint32_t kMemCommit = 0x1000;
        constexpr std::uint32_t kExecMask = 0x10u | 0x20u | 0x40u | 0x80u;
        REX::W32::MEMORY_BASIC_INFORMATION mbi{};
        if (REX::W32::VirtualQuery(a_addr, &mbi, sizeof(mbi)) == 0) {
            return false;
        }
        if (static_cast<std::uint32_t>(mbi.state) != kMemCommit) {
            return false;
        }
        return (static_cast<std::uint32_t>(mbi.protect) & kExecMask) != 0;
    }

    using Activate_t = bool (*)(RE::TESBoundObject*, RE::TESObjectREFR*, RE::TESObjectREFR*, std::uint8_t, RE::TESBoundObject*, std::int32_t);
    Activate_t g_origContActivate = nullptr;
    Activate_t g_origDoorActivate = nullptr;

    // =========================================================================
    //  Tier 2: suppress the Loading Menu during a REAL cell transition
    //  A real transition keeps every quest / location / navmesh / interior-lighting semantic
    //  intact -- only the menu is worth hiding. We drop its kShow at the menu's own
    //  ProcessMessage (vtable idx 4) inside a short window, and re-post it if the transition
    //  overruns, so the worst case degrades to vanilla behaviour instead of a silent hang.
    // =========================================================================
    using ProcessMessage_t = RE::UI_MESSAGE_RESULTS (*)(RE::IMenu*, RE::UIMessage&);
    ProcessMessage_t g_origLoadingProcessMessage = nullptr;

    std::atomic<bool>         g_suppressArmed{ false };
    std::atomic<bool>         g_suppressWanted{ false };  // captured at arm time, not read live
    std::atomic<bool>         g_noBlackWanted{ false };   // ditto
    std::atomic<std::int64_t> g_noBlackStartAt{ 0 };      // fades are left alone until this instant
    // Exemption window for global mode. Duration alone cannot tell a fast travel from a room
    // transition -- on a fast machine a short fast-travel load finishes inside the timeout. The
    // reliable signal is the menu that *precedes* it: the map closes before a fast travel, the main
    // menu closes before load-game. While this window is open, the LoadingMenu is left alone.
    std::atomic<std::int64_t> g_exemptUntil{ 0 };
    // Generation guard. Each arm() starts a watchdog thread; without this, a second transition
    // arming while the first watchdog is still looping would have two threads clearing/re-posting
    // state that belongs to the other one. Every watchdog remembers its generation and exits as
    // soon as a newer one is armed.
    std::atomic<std::uint32_t> g_suppressGen{ 0 };
    // Last measured load time per destination cell. Written and read on the main thread only, so no
    // lock is needed. Drives the adaptive fallback (uSuppressMaxMs).
    std::unordered_map<RE::FormID, std::uint32_t> g_cellLoadMs;
    // v1.3.4: persist the per-cell load history, otherwise the adaptive window forgets everything on
    // restart and every heavy door pays the full "first time" penalty again in each session.
    std::atomic<bool> g_cellTimesDirty{ false };
    std::atomic<std::uint64_t> g_cellTimesSaveGen{ 0 };
    std::mutex g_cellLoadMsMutex;    // protects g_cellLoadMs map
    std::mutex g_cellTimesFileMutex; // serializes LoadingFlow_CellTimes.ini atomic writes
    std::once_flag g_cellTimesLoadOnce;
    constexpr const char* kCellTimesPath = "Data/SKSE/Plugins/LoadingFlow_CellTimes.ini";
    constexpr const char* kCellTimesTmpPath = "Data/SKSE/Plugins/LoadingFlow_CellTimes.ini.tmp";

    void LoadCellTimesOnce()
    {
        std::call_once(g_cellTimesLoadOnce, []() {
            std::ifstream f(kCellTimesPath);
            if (!f.is_open()) {
                return;
            }
            auto trim = [](std::string_view sv) -> std::string_view {
                const auto b = sv.find_first_not_of(" \t\r\n");
                if (b == std::string_view::npos) {
                    return {};
                }
                const auto e = sv.find_last_not_of(" \t\r\n");
                return sv.substr(b, e - b + 1);
            };
            std::string line;
            std::size_t n = 0;
            std::unordered_map<RE::FormID, std::uint32_t> loaded;
            bool firstLine = true;
            while (std::getline(f, line) && n < 8192) {
                if (firstLine) {
                    firstLine = false;
                    if (line.size() >= 3 &&
                        static_cast<unsigned char>(line[0]) == 0xEF &&
                        static_cast<unsigned char>(line[1]) == 0xBB &&
                        static_cast<unsigned char>(line[2]) == 0xBF) {
                        line.erase(0, 3);
                    }
                }
                std::string_view sv(line);
                if (const auto sc = sv.find(';'); sc != std::string_view::npos) {
                    sv = sv.substr(0, sc);
                }
                sv = trim(sv);
                if (sv.empty() || sv.front() == '#') {
                    continue;
                }
                const auto eq = sv.find('=');
                if (eq == std::string_view::npos) {
                    continue;
                }
                const auto key = trim(sv.substr(0, eq));
                const auto val = trim(sv.substr(eq + 1));
                if (key.empty() || val.empty()) {
                    continue;
                }
                std::string_view hexKey = key;
                if (hexKey.starts_with("0x") || hexKey.starts_with("0X")) {
                    hexKey.remove_prefix(2);
                }
                std::uint32_t id = 0;
                std::uint32_t ms = 0;
                const auto [pId, ecId] = std::from_chars(hexKey.data(), hexKey.data() + hexKey.size(), id, 16);
                const auto [pMs, ecMs] = std::from_chars(val.data(), val.data() + val.size(), ms, 10);
                if (ecId == std::errc{} && pId == hexKey.data() + hexKey.size() &&
                    ecMs == std::errc{} && pMs == val.data() + val.size() &&
                    id != 0 && ms > 0 && ms <= 600000u) {
                    auto& slot = loaded[static_cast<RE::FormID>(id)];
                    slot = (ms > slot) ? ms : slot;
                    ++n;
                }
            }
            if (!loaded.empty()) {
                {
                    std::lock_guard<std::mutex> lk(g_cellLoadMsMutex);
                    for (const auto& [id, ms] : loaded) {
                        auto& slot = g_cellLoadMs[id];
                        slot = (ms > slot) ? ms : slot;
                    }
                }
                logger::info("[Tier2] cell load history loaded: {} cell(s) from {}", loaded.size(), kCellTimesPath);
            }
        });
    }

    void SaveCellTimes()
    {
        if (g_shuttingDown.load(std::memory_order_acquire)) {
            return;
        }
        std::lock_guard<std::mutex> fileLk(g_cellTimesFileMutex);
        if (g_shuttingDown.load(std::memory_order_acquire)) {
            return;
        }
        if (!g_cellTimesDirty.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        std::unordered_map<RE::FormID, std::uint32_t> snap;
        {
            std::lock_guard<std::mutex> lk(g_cellLoadMsMutex);
            snap = g_cellLoadMs;
        }
        {
            std::ofstream f(kCellTimesTmpPath, std::ios::binary | std::ios::trunc);
            if (!f.is_open()) {
                g_cellTimesDirty.store(true, std::memory_order_release);
                return;
            }
            f << "; LoadingFlow per-cell load times (written automatically, safe to delete)\r\n";
            for (const auto& [id, ms] : snap) {
                f << std::format("{:08X}={}\r\n", id, ms);
            }
            f.flush();
            if (!f.good()) {
                g_cellTimesDirty.store(true, std::memory_order_release);
                return;
            }
        }
        if (!AtomicReplaceFile(kCellTimesTmpPath, kCellTimesPath)) {
            g_cellTimesDirty.store(true, std::memory_order_release);
        }
    }

    void ScheduleCellTimesSave()
    {
        if (g_shuttingDown.load(std::memory_order_acquire)) {
            return;
        }
        g_cellTimesDirty.store(true, std::memory_order_release);
        const auto gen = g_cellTimesSaveGen.fetch_add(1, std::memory_order_acq_rel) + 1;
        std::thread([gen]() {
            for (int i = 0; i < 15; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (g_shuttingDown.load(std::memory_order_acquire) ||
                    g_cellTimesSaveGen.load(std::memory_order_acquire) != gen) {
                    return;
                }
            }
            SaveCellTimes();
        }).detach();
    }
    // ---- Assembly probe state (READ-ONLY) ----------------------------------------------------
    // Armed right after a transition reports "finished"; the per-step main-thread task then samples
    // the player's transform and the camera FOV until they stop changing. Only the watchdog thread
    // writes the counters, and sampling always happens inside the main-thread task.
    std::atomic<bool>         g_probeArmed{ false };
    std::atomic<std::int64_t> g_probeStartAt{ 0 };
    std::atomic<std::uint32_t> g_probeSamples{ 0 };
    std::atomic<std::uint32_t> g_probeChanges{ 0 };
    std::atomic<std::int64_t> g_probeLastChange{ -1 };
    float g_probePrev[16] = {};
    std::size_t g_probePrevN = 0;
    float g_probePrevPlayer[3] = {};
    float g_probeFadeIn = -1.0f;
    std::atomic<bool> g_probeWalking{ false };
    // v1.1.3: separated accounting (translate / rotation / cell3D transform / child count).
    std::atomic<bool>         g_probeLooking{ false };
    std::atomic<std::int64_t> g_probeLastTranslate{ -1 };
    std::atomic<std::int64_t> g_probeLastRotate{ -1 };
    std::atomic<std::int64_t> g_probeLastCellXform{ -1 };
    std::atomic<std::int64_t> g_probeLastChild{ -1 };
    std::atomic<std::uint32_t> g_probeTranslateChanges{ 0 };
    std::atomic<std::uint32_t> g_probeRotateChanges{ 0 };
    std::atomic<std::uint32_t> g_probeChildChanges{ 0 };
    std::int64_t g_probePrevChildCount = -1;
    // Renderer pointer identities (read-only), so we notice when a texture must be re-created.
    std::atomic<std::uintptr_t> g_probeGfxMainTex{ 0 };
    std::atomic<std::uintptr_t> g_probeGfxFbTex{ 0 };
    std::atomic<std::uintptr_t> g_probeGfxSwapChain{ 0 };
    std::atomic<int> g_voidStep{ -1 };   // last applied darkening step (0..8) for the void ramp
    // One-shot instrumentation: LoadingMenu is a kAlwaysOpen menu, so after the first display the
    // engine may reuse the instance and request it with kReshow instead of kShow. Log the first few
    // message types we actually receive so this can be settled from a log instead of guessed.
    std::atomic<int> g_msgTypeLogBudget{ 12 };
    std::atomic<bool>         g_menuWasSuppressed{ false };
    // v1.6.7: after the cut the engine can ask for the LoadingMenu AGAIN (a late/second request).
    // We used to disarm at the cut, so that request went through -> the vanilla loading screen came
    // up over a playable game, nothing ever hid it, and it ate all input (ESC and console included).
    // Reported by two users. Keep eating those requests for a grace window after the cut.
    std::atomic<std::int64_t> g_graceSuppressUntil{ 0 };
    std::atomic<int>          g_graceEaten{ 0 };
    std::atomic<std::int64_t> g_suppressDeadline{ 0 };
    std::atomic<std::int64_t> g_fallbackAt{ 0 };   // set only AFTER we suppressed a menu
    std::atomic<std::int64_t> g_suppressStarted{ 0 };
    // v1.3.0: the moment the loading menu was actually dropped, i.e. the first frame of the dead
    // window. g_suppressStarted is the DOOR ACTIVATION, ~0.6 s earlier and still a live picture
    // (the door swing) -- driving the push-in from that would freeze the door animation.
    std::atomic<std::int64_t> g_deadWindowAt{ 0 };
    std::atomic<RE::FormID>   g_suppressFromCell{ 0 };
    // v1.3.4: the destination we were told about -- a SECOND acceptance signal for "done".
    // The from-cell inequality alone can silently never fire (measured on Understone Keep:
    // the cell DID change in game, our check never matched, the overlay stayed in the dead
    // window for 10 s and the frozen snapshot stayed on screen).
    std::atomic<RE::FormID>   g_suppressDestCell{ 0 };
    // v1.3.4: the window actually used for THIS transition (may be adapted from per-cell
    // history). The re-post fallback is derived from it so it can never fire mid-load.
    std::atomic<std::uint32_t> g_suppressWindowMs{ 0 };

    std::int64_t NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // defined below; the global-mode branch of the menu hook needs it before the definition
    void ArmLoadingMenuSuppression(const char* a_reason, bool a_isAutoCaveDoor, RE::FormID a_destCell = 0);
    // ---- Assembly probe (READ-ONLY) ----------------------------------------------------------
    RE::Setting* FindIniSetting(const char* a_name);   // defined in the settings section below
    void         ProbeCheckGfxStability();             // defined below, used by ProbeArm

    // Answers ONE question with data: after the cell swap completes, how long does the engine keep
    // changing the scene? That number is what a retained-frame hold would have to cover, so it has
    // to be measured before any capture work is designed. Nothing here writes engine state.
    float EffectiveFadeInSeconds()
    {
        if (!g_config.tweakEngineTimers) {
            return -1.0f;
        }
        auto* s = FindIniSetting("fMinSecondsForLoadFadeIn:Interface");
        return (s && s->GetType() == RE::Setting::Type::kFloat) ? s->data.f : -1.0f;
    }

    void ProbeArm()
    {
        g_probeSamples.store(0);
        g_probeChanges.store(0);
        g_probeLastChange.store(-1);
        g_probePrevN = 0;
        g_probeWalking.store(false);
        g_probeLooking.store(false);
        g_probeLastTranslate.store(-1);
        g_probeLastRotate.store(-1);
        g_probeLastCellXform.store(-1);
        g_probeLastChild.store(-1);
        g_probeTranslateChanges.store(0);
        g_probeRotateChanges.store(0);
        g_probeChildChanges.store(0);
        g_probePrevChildCount = -1;
        g_probeFadeIn = EffectiveFadeInSeconds();
        g_probeStartAt.store(NowMs());
        g_probeArmed.store(true);
        if (g_config.probeGfx) {
            ProbeCheckGfxStability();
        }
        logger::info("[Probe] assembly watch armed (effective fMinSecondsForLoadFadeIn={:.2f}s, budget {} ms)",
            g_probeFadeIn, g_config.probeAssemblyMs);
    }

    // Runs on the MAIN thread, inside the existing per-step task.
    //
    // v1.1.2: samples NON-PLAYER signals only. The first probe version sampled the player's world
    // transform, and every "change" it reported was simply the player walking -- the camera FOV was
    // constant 80.00 across 551 samples, so that whole run measured footsteps, not the engine. The
    // scene-assembly spin can only live on the camera root or on the cell's 3D root, so sample those.
    void ProbeSample()
    {
        float       sig[16] = {};
        std::size_t n = 0;

        // 1) camera root (the node the engine rotates/scales while the new scene is assembled)
        if (auto* cam = RE::PlayerCamera::GetSingleton()) {
            if (auto* root = cam->cameraRoot.get()) {
                sig[n++] = root->world.translate.x;
                sig[n++] = root->world.translate.y;
                sig[n++] = root->world.translate.z;
                sig[n++] = root->world.scale;
                sig[n++] = root->local.scale;
                sig[n++] = root->world.rotate.entry[0][0];
                sig[n++] = root->world.rotate.entry[1][1];
                sig[n++] = root->world.rotate.entry[2][2];
            }
            sig[n++] = cam->worldFOV;
        }

        // 2) the cell's own 3D root. Its child count is the cleanest assembly signal we have:
        // the engine attaches objects one by one, so "children stop growing" == assembly finished.
        // Nothing the player does can change this number.
        auto* pl = RE::PlayerCharacter::GetSingleton();
        auto* cell = pl ? pl->GetParentCell() : nullptr;
        std::uint32_t childCount = 0;
        bool          haveCell3D = false;
        if (cell && cell->GetRuntimeData().loadedData) {
            if (auto* cell3d = cell->GetRuntimeData().loadedData->cell3D.get()) {
                sig[n++] = cell3d->local.scale;
                sig[n++] = cell3d->world.scale;
                sig[n++] = cell3d->local.rotate.entry[0][0];
                sig[n++] = cell3d->local.rotate.entry[1][1];
                sig[n++] = cell3d->local.rotate.entry[2][2];
                sig[n++] = cell3d->world.translate.z;
                childCount = static_cast<std::uint32_t>(cell3d->GetChildren().size());
                haveCell3D = true;
            }
        }

        // 3) walking guard: the player's own movement must never be mistaken for assembly activity.
        float       px = 0.0f, py = 0.0f, pz = 0.0f;
        bool        havePos = false;
        if (auto* node = pl ? pl->Get3D() : nullptr) {
            px = node->world.translate.x;
            py = node->world.translate.y;
            pz = node->world.translate.z;
            havePos = true;
        }

        const auto elapsed = NowMs() - g_probeStartAt.load();
        g_probeSamples.fetch_add(1);

        bool walking = false;
        if (havePos && g_probePrevN > 0) {
            const float dx = px - g_probePrevPlayer[0];
            const float dy = py - g_probePrevPlayer[1];
            const float dz = pz - g_probePrevPlayer[2];
            walking = (dx * dx + dy * dy + dz * dz) > (0.02f * 0.02f);
        }
        if (havePos) {
            g_probePrevPlayer[0] = px;
            g_probePrevPlayer[1] = py;
            g_probePrevPlayer[2] = pz;
        }
        if (walking) {
            if (!g_probeWalking.exchange(true)) {
                logger::info("[Probe] player is moving from +{} ms on -- movement is excluded from the assembly accounting", elapsed);
            }
        }

        // ---- separated accounting -----------------------------------------------------------
        // v1.1.3: translate / rotation / cell-child-count are tracked independently. The previous
        // version merged them into one signature, so a player turning the camera looked exactly like
        // "the engine is still assembling" -- that is why several runs reported 2.5 s of "movement".
        const bool havePrev = (g_probePrevN == n && n > 0);
        if (havePrev) {
            // camera root translation (engine camera interpolation; player input cannot move it)
            if (std::fabs(sig[0] - g_probePrev[0]) > 1.0e-4f ||
                std::fabs(sig[1] - g_probePrev[1]) > 1.0e-4f ||
                std::fabs(sig[2] - g_probePrev[2]) > 1.0e-4f) {
                g_probeTranslateChanges.fetch_add(1);
                g_probeLastTranslate.store(elapsed);
            }
            // camera root rotation (may be the PLAYER looking around -- flagged, not trusted)
            if (std::fabs(sig[5] - g_probePrev[5]) > 1.0e-3f ||
                std::fabs(sig[6] - g_probePrev[6]) > 1.0e-3f ||
                std::fabs(sig[7] - g_probePrev[7]) > 1.0e-3f) {
                g_probeRotateChanges.fetch_add(1);
                g_probeLastRotate.store(elapsed);
                g_probeLooking.store(true);
            }
            // cell 3D own transform (engine-side)
            bool cellMoved = false;
            for (std::size_t i = 8; i < n - 1; ++i) {
                if (std::fabs(sig[i] - g_probePrev[i]) > 1.0e-4f) {
                    cellMoved = true;
                    break;
                }
            }
            if (cellMoved) {
                g_probeLastCellXform.store(elapsed);
            }
        }
        // cell child count: the clean assembly signal -- log every change, nothing else per step
        if (haveCell3D) {
            if (g_probePrevChildCount >= 0 && static_cast<std::uint32_t>(g_probePrevChildCount) != childCount) {
                g_probeChildChanges.fetch_add(1);
                g_probeLastChild.store(elapsed);
                logger::info("[Probe] +{} ms cell3D children {} -> {}{}",
                    elapsed, g_probePrevChildCount, childCount, walking ? " (player walking)" : "");
            }
            g_probePrevChildCount = static_cast<std::int64_t>(childCount);
        }
        for (std::size_t i = 0; i < n; ++i) {
            g_probePrev[i] = sig[i];
        }
        g_probePrevN = n;

        if (elapsed >= static_cast<std::int64_t>(g_config.probeAssemblyMs)) {
            g_probeArmed.store(false);
            const auto samples = g_probeSamples.load();
            logger::info("[Probe] assembly summary after {} ms ({} samples, fadeIn={:.2f}s, walking={}, looking={}):",
                elapsed, samples, g_probeFadeIn,
                g_probeWalking.load() ? "yes(excluded)" : "no",
                g_probeLooking.load() ? "yes(rotation untrusted)" : "no");
            logger::info("[Probe]   camRoot translate : last change +{} ms ({} samples)",
                g_probeLastTranslate.load(), g_probeTranslateChanges.load());
            logger::info("[Probe]   camRoot rotation  : last change +{} ms ({} samples){}",
                g_probeLastRotate.load(), g_probeRotateChanges.load(),
                g_probeLooking.load() ? "  <- player may have been looking around" : "");
            logger::info("[Probe]   cell3D transform  : last change +{} ms",
                g_probeLastCellXform.load());
            logger::info("[Probe]   cell3D children   : {} changes, last at +{} ms, final count {}",
                g_probeChildChanges.load(), g_probeLastChild.load(), g_probePrevChildCount);
            if (g_probeChildChanges.load() == 0) {
                logger::info("[Probe]   -> child count never changed inside the window: assembly was already complete when the transition was reported finished");
            }
        }
    }

    // ---- Renderer reachability (READ-ONLY) ---------------------------------------------------
    // These two answer "can we even touch the pipeline?" without hooking anything: they read the
    // engine's own renderer struct (BSGraphics::Renderer::GetRendererData) and remember pointer
    // identities so a later capture implementation knows when a texture must be re-created.
    // No COM call, no resource creation, no state change -- plain struct reads.
    void ProbeDumpGfxOnce()
    {
        using namespace RE::BSGraphics;
        auto* rd = Renderer::GetRendererData();
        if (!rd) {
            logger::warn("[Probe] gfx: GetRendererData() returned null -- renderer not reachable this way");
            return;
        }
        auto& win = rd->renderWindows[0];
        auto& fb = rd->renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];
        auto& main = rd->renderTargets[RE::RENDER_TARGET::kMAIN];
        logger::info("[Probe] gfx: device={} context={} window={}x{} borderless={} fullscreen={} interval={}",
            static_cast<const void*>(rd->forwarder), static_cast<const void*>(rd->context),
            rd->newWidth, rd->newHeight, rd->borderlessDisplay ? 1 : 0, rd->fullScreen ? 1 : 0,
            rd->presentInterval);
        logger::info("[Probe] gfx: swapchain={} renderView={} | kFRAMEBUFFER tex={} srv={} | kMAIN tex={} rtv={} srv={}",
            static_cast<const void*>(win.swapChain), static_cast<const void*>(win.renderView),
            static_cast<const void*>(fb.texture), static_cast<const void*>(fb.SRV),
            static_cast<const void*>(main.texture), static_cast<const void*>(main.RTV),
            static_cast<const void*>(main.SRV));
        g_probeGfxMainTex.store(reinterpret_cast<std::uintptr_t>(main.texture));
        g_probeGfxFbTex.store(reinterpret_cast<std::uintptr_t>(fb.texture));
        g_probeGfxSwapChain.store(reinterpret_cast<std::uintptr_t>(win.swapChain));
        const bool reachable = rd->forwarder && rd->context && main.texture != nullptr;
        logger::info("[Probe] gfx verdict: capture path {} (device+context+main RT all present? {})",
            reachable ? "REACHABLE without a Present hook" : "NOT reachable -- needs investigation",
            reachable ? "yes" : "no");
    }

    // Called at each transition so we learn whether the pointers survive cell changes / resizes.
    void ProbeCheckGfxStability()
    {
        using namespace RE::BSGraphics;
        auto* rd = Renderer::GetRendererData();
        if (!rd) {
            return;
        }
        auto& win = rd->renderWindows[0];
        const auto mainTex = reinterpret_cast<std::uintptr_t>(rd->renderTargets[RE::RENDER_TARGET::kMAIN].texture);
        const auto fbTex = reinterpret_cast<std::uintptr_t>(rd->renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER].texture);
        const auto sc = reinterpret_cast<std::uintptr_t>(win.swapChain);
        const auto prevMain = g_probeGfxMainTex.exchange(mainTex);
        const auto prevFb = g_probeGfxFbTex.exchange(fbTex);
        const auto prevSc = g_probeGfxSwapChain.exchange(sc);
        if (prevMain != 0 && prevMain != mainTex) {
            logger::warn("[Probe] gfx: kMAIN texture pointer CHANGED (0x{:X} -> 0x{:X}) -- a retained texture must be re-created", prevMain, mainTex);
        }
        if (prevFb != 0 && prevFb != fbTex) {
            logger::warn("[Probe] gfx: kFRAMEBUFFER texture pointer CHANGED (0x{:X} -> 0x{:X})", prevFb, fbTex);
        }
        if (prevSc != 0 && prevSc != sc) {
            logger::warn("[Probe] gfx: swapChain pointer CHANGED (0x{:X} -> 0x{:X})", prevSc, sc);
        }
        logger::info("[Probe] gfx at transition: kMAIN=0x{:X} kFRAMEBUFFER=0x{:X} swapchain=0x{:X} window={}x{}",
            mainTex, fbTex, sc, rd->newWidth, rd->newHeight);
    }

    void LogGraphicsEnvironment()
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const bool enbDir = fs::exists("enbseries", ec);
        const bool d3d11 = fs::exists("d3d11.dll", ec);
        std::vector<std::string> gfx;
        for (const auto& e : fs::directory_iterator("Data/SKSE/Plugins", ec)) {
            if (ec || !e.is_regular_file(ec)) {
                continue;
            }
            auto        name = e.path().filename().string();
            std::string low = name;
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (low.find("community") != std::string::npos || low.find("dlss") != std::string::npos ||
                low.find("fsr") != std::string::npos || low.find("upscal") != std::string::npos ||
                low.find("reshade") != std::string::npos || low.find("enb") != std::string::npos) {
                gfx.push_back(name);
            }
        }
        std::string joined;
        for (const auto& s : gfx) {
            if (!joined.empty()) {
                joined += ", ";
            }
            joined += s;
        }
        logger::info("[Probe] env: enbseries_dir={} d3d11_in_gamedir={} graphics_plugins=[{}]",
            enbDir ? "yes" : "no", d3d11 ? "yes" : "no", joined.empty() ? "none" : joined.c_str());
    }

    // ---- B: freeze the presented image instead of painting black (EXPERIMENTAL) --------------
    // Idea: the player's last frame is already on screen. If we simply do not call Present for the
    // duration of the transition, the display keeps showing that exact image -- no texture capture,
    // no format/sRGB handling, no shader, and it automatically includes whatever ENB / Community
    // Shaders produced, because we never touch their pixels. When the window ends we resume
    // presenting, and the first frame after that is the finished new cell.
    //
    // This patches the swap chain's COM vtable slot for IDXGISwapChain::Present (index 8). It is the
    // only place in this plugin that touches a COM vtable; it is therefore OFF by default and logs
    // loudly. Failure modes seen in the field are "no effect" or a frozen picture -- not a crash --
    // and EndFreezeFrame() is called on every finish path so presentation can never stay suppressed.
    namespace capprobe { void OnPresent(void* a_swapChain); }
    // v1.4.0: the void overlay draws in the Present hook, so it has to be declared before the
    // hook itself. Full definition further down (after the capture probe).
    namespace voidoverlay
    {
        void OnPresent(void* a_swapChain);
        void NotePresent();
        bool NotePresentResult(long a_hr);   // v1.3.1: true the first time, so the caller logs it once
        bool WantsPostPresentProbe();        // v1.3.1 mode 4
        void PostPresentProbe(void* a_swapChain);
        bool BypassToPresent1();             // v1.3.2 mode 6: submit through slot 22 instead of slot 8
        void Arm();
        void Cut();
        void Disarm(const char* a_reason);
        bool Active();
        // v1.8.7: ask DXGI whether the Present we forwarded was counted at all.
        bool AuditWanted();
        void AuditPresent(void* a_swapChain, std::uint32_t a_presentsBefore, long a_hr, bool a_bypassed);
        // v1.8.9 mode 8: after the frame has been handed down the chain, look at the buffers again.
        bool WantsSurvivalCheck();
        void SurvivalCheck(void* a_swapChain);
    }

    namespace presentfreeze
    {
        using PresentFn = long (*)(void*, unsigned int, unsigned int);
        // IDXGISwapChain1::Present1(UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS*)
        // -- the parameter order is NOT the same as a leading pointer, get it wrong and the real
        // Present1 receives a pointer where it expects a sync interval.
        using Present1Fn = long (*)(void*, unsigned int, unsigned int, const void*);

        PresentFn         g_origPresent = nullptr;
        Present1Fn        g_origPresent1 = nullptr;
        // v1.3.3: Present1 (slot 22) is a flip-model API. On a bitblt chain it must not be used at all.
        std::atomic<bool> g_flipModel{ true };
        // v1.3.2: once we have decided this swap chain's vtable cannot be patched, stop retrying --
        // InstallOn is called from several places and a repeated warn would flood the log.
        // Per-swap-chain, not global: if the game recreates the swap chain we must be free to try
        // again -- a fresh chain may well be hookable even when the previous one was not.
        std::atomic<void*> g_gaveUpSwap{ nullptr };
        void*             g_hookedSwapChain = nullptr;
        std::atomic<bool> g_suppressPresent{ false };
        // v1.3.2: when set, the overlay's pixels are submitted through slot 22 (Present1) instead of
        // slot 8, which is where SPS sits and re-renders the frame.
        std::atomic<bool> g_bypassToPresent1{ false };
        std::atomic<bool> g_installed{ false };
        std::atomic<bool> g_firstSkipLogged{ false };
        std::atomic<std::uint32_t> g_skippedFrames{ 0 };
        // Every Present / Present1 that reaches us, suppressed or not (v1.4.2). The world probe
        // turns this into a frame rate for the transition window: a healthy 55-60 Hz means the main
        // loop is still spinning freely, a low number means it is blocked inside the load path --
        // which decides whether throttling simulation work could shorten the window at all.
        std::atomic<std::uint64_t> g_frameCount{ 0 };
        std::atomic<std::int64_t>  g_suppressStartAt{ 0 };
        // v1.5.5: at the cut the engine can present 1~3 frames with nothing drawn in them (the new
        // cell is not up yet) -- that is the "black flash". We cannot paint over them (that needs
        // presentation rights) but we CAN decline to present them, which leaves the last good frame
        // on screen. Bounded to a few tens of ms.
        std::atomic<std::int64_t>  g_holdUntil{ 0 };
        std::atomic<int>           g_heldFrames{ 0 };
        ID3D11Texture2D*           g_probeTex = nullptr;
        std::atomic<bool>          g_holdActive{ false };
        std::atomic<std::int64_t>  g_holdStart{ 0 };
        std::atomic<std::uint32_t> g_holdCapMs{ 250 };
        // v1.5.7: trace what the back buffer actually contains across the whole armed window, so the
        // position of the "black" stops being a guess. 50 ms spacing, 8x8 centre patch.
        std::atomic<std::int64_t>  g_traceNextAt{ 0 };
        std::atomic<std::int64_t>  g_traceArmAt{ 0 };
        // v1.6.5: stutter is measurable -- count frames whose inter-present gap exceeds 40 ms
        // during the 3 s after a transition. That is the only honest way to judge
        // iLoadingQueuedPriorityBudgetMs, which never shows up in the transition-total metric.
        std::atomic<std::int64_t>  g_prevPresentAt{ 0 };
        std::atomic<std::int64_t>  g_hitchUntil{ 0 };
        std::atomic<int>           g_hitchCount{ 0 };
        std::atomic<int>           g_hitchFrames{ 0 };
        std::atomic<bool>          g_hitchReported{ true };

        void ArmHitchWindow()
        {
            g_hitchCount.store(0);
            g_hitchFrames.store(0);
            g_hitchReported.store(false);
            g_hitchUntil.store(NowMs() + 3000);
        }

        void HitchTick()
        {
            const auto now = NowMs();
            const auto prev = g_prevPresentAt.exchange(now);
            if (g_hitchUntil.load() == 0) {
                return;
            }
            if (now > g_hitchUntil.load()) {
                if (!g_hitchReported.exchange(true)) {
                    const int frames = g_hitchFrames.load();
                    logger::info("[Perf] 3 s after the transition: {} frames, {} hitches (>40 ms gap)",
                        frames, g_hitchCount.load());
                }
                return;
            }
            g_hitchFrames.fetch_add(1);
            if (prev != 0 && (now - prev) > 40) {
                g_hitchCount.fetch_add(1);
            }
        }

        float SampleBackbufferLuma(void* a_swapChain);   // defined below

        void TraceArm()
        {
            g_traceArmAt.store(NowMs());
            g_traceNextAt.store(NowMs());
        }

        void TraceStop()
        {
            g_traceNextAt.store(0);
            g_traceArmAt.store(0);
        }

        // v1.5.8: the black is NOT at the cut. Measured trace: one frame at suppression
        // (luma 3.8) and then ~500 ms of pure black (luma 1.0) about 900 ms AFTER the cut.
        // So the gate must be brightness-triggered, not event-triggered.
        std::atomic<bool>         g_autoGate{ false };   // v1.5.9: off by default, see note above
        std::atomic<std::int64_t> g_autoUntil{ 0 };   // keep watching this long after activation

        void ArmAutoGate()
        {
            g_autoUntil.store(NowMs() + 4000);
            g_holdActive.store(false);
            g_holdUntil.store(0);
        }

        void TraceTick(void* a_swapChain)
        {
            if (g_traceNextAt.load() == 0) {
                return;
            }
            if (NowMs() - g_traceArmAt.load() > 6000) {
                TraceStop();
                logger::info("[Trace] stopped (6 s window)");
                return;
            }
            // v1.10.2: this used to sample EVERY frame for the whole 6 s window. A luma sample is a
            // CopySubresourceRegion plus Map(D3D11_MAP_READ) on a staging texture -- a full CPU/GPU
            // sync. At ~130 fps that is roughly 780 blocking syncs per door, and the window runs on
            // well past the cut into the live cell, so it cost real frame time exactly when the
            // player wants control back. A 1-frame black is 7 ms and nobody has ever seen one; the
            // only consumer that needs per-frame data is the auto gate, which is off by default.
            const bool gateLive = g_autoGate.load(std::memory_order_relaxed);
            if (!gateLive && NowMs() < g_traceNextAt.load()) {
                return;   // nothing due this frame
            }
            const float luma = SampleBackbufferLuma(a_swapChain);
            if (g_autoGate.load() && NowMs() < g_autoUntil.load() && !g_holdActive.load() &&
                luma >= 0.0f && luma < 8.0f) {
                // A black frame is about to be presented -- close the gate right now.
                g_holdStart.store(NowMs());
                g_holdCapMs.store(600);
                g_holdActive.store(true);
                logger::info("[Hold] black frame detected (luma {:.1f}) -- withholding until the picture is back", luma);
            }
            if (NowMs() < g_traceNextAt.load()) {
                return;
            }
            g_traceNextAt.store(NowMs() + 50);
            logger::info("[Trace] t=+{} ms luma={:.1f}", NowMs() - g_traceArmAt.load(), luma);
        }

        void HoldAtCut(std::uint32_t a_ms)
        {
            // a_ms is now a SAFETY CAP; the gate opens by itself when the picture returns.
            g_heldFrames.store(0);
            // v1.3.2: 0 means OFF, not "250". Passing 0 used to still freeze for 250 ms, so there
            // was no way at all to turn this off -- neither via the menu nor via the INI.
            if (a_ms == 0) {
                g_holdActive.store(false);
                g_holdCapMs.store(0);
                return;
            }
            g_holdCapMs.store(a_ms);
            g_holdStart.store(NowMs());
            g_holdActive.store(true);
            g_holdUntil.store(0);
        }

        bool HoldingNow()
        {
            const auto until = g_holdUntil.load();
            return until != 0 && NowMs() < until;
        }

        // ---- v1.5.6: measure the empty-frame run instead of assuming a duration ---------------
        // A fixed 80 ms is wrong on any machine but the author's. Instead we sample a few pixels of
        // the back buffer each frame while the gate is closed and open it as soon as the picture is
        // no longer black. uHoldAtCutMs becomes a SAFETY CAP, not a duration.
        // Average luma of a small centre patch, 0..255. Returns -1 when it cannot be read.
        // ---- v1.8.7: which back buffer is "the frame we are about to present"? ------------------
        // This machine logs `2 buffer(s), effect FLIP_DISCARD`, and a flip-model chain rotates its
        // back buffers on every Present: index 0 is the presented one only on every other frame.
        // Every draw site in this file used to hard-code GetBuffer(0), so on half the frames we
        // wrote into a buffer nobody would ever scan out. Worse, the `read-back after clear: luma
        // 170.0` proof could never detect it -- it re-reads the very buffer we just wrote, so it is
        // self-consistent by construction. That proof is therefore NOT evidence that the presented
        // buffer contains our pixels, and it must stop being cited as one.
        // DXGI only exposes the index on IDXGISwapChain3 (slot 36), which an older chain may not
        // implement, so derive it from GetLastPresentCount() % BufferCount -- true for every
        // flip-model effect. A bitblt chain has a single back buffer, so 0 is correct there.
        std::uint32_t CurrentBackBufferIndex(IDXGISwapChain* a_sc)
        {
            if (!a_sc) {
                return 0;
            }
            DXGI_SWAP_CHAIN_DESC desc{};
            if (FAILED(a_sc->GetDesc(&desc)) || desc.BufferCount == 0) {
                return 0;
            }
            const bool flipModel = (desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
                                    desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);
            if (!flipModel || desc.BufferCount < 2) {
                return 0;
            }
            UINT presented = 0;
            const bool haveCount = SUCCEEDED(a_sc->GetLastPresentCount(&presented));
            const std::uint32_t derived = haveCount ? static_cast<std::uint32_t>(presented % desc.BufferCount) : 0u;

            // IDXGISwapChain3::GetCurrentBackBufferIndex (slot 36) is the authority, the modulo above
            // is only a fallback for a chain that predates that interface. QueryInterface first:
            // calling slot 36 on an older chain reads past the end of its vtable. If the two ever
            // disagree, say so once -- a wrong index here would put us straight back into "the
            // effect is invisible", and this time with no way to tell it from a broken chain.
            std::uint32_t actual = derived;
            bool fromChain3 = false;
            // v1.8.9: the GUID used here in v1.8.7 was **IDXGISwapChain2's**, not 3's, so `authority n/a`
            // in the shipped log did not mean "this chain lacks SwapChain3" -- it meant the probe asked
            // the wrong question. Correct IIDs are spelled out so this cannot rot again:
            //   1 = 790A45F7-0D42-4876-983A-0A55CFE6F4AA   2 = A8BE2AC4-1997-4948-B331-ABAC30C2320E
            //   3 = 94D99BDB-F1F8-4AB0-B236-7DA0170EDAB1   4 = 3D585D5A-BD4A-489E-B1F4-3DBCB6452FFB
            static const GUID kIID_SC1 = { 0x790a45f7u, 0x0d42u, 0x4876u, { 0x98, 0x3a, 0x0a, 0x55, 0xcf, 0xe6, 0xf4, 0xaa } };
            static const GUID kIID_SC2 = { 0xa8be2ac4u, 0x1997u, 0x4948u, { 0xb3, 0x31, 0xab, 0xac, 0x30, 0xc2, 0x32, 0x0e } };
            static const GUID kIID_SC3 = { 0x94d99bdbu, 0xf1f8u, 0x4ab0u, { 0xb2, 0x36, 0x7d, 0xa0, 0x17, 0x0e, 0xda, 0xb1 } };
            static const GUID kIID_SC4 = { 0x3d585d5au, 0xbd4au, 0x489eu, { 0xb1, 0xf4, 0x3d, 0xbc, 0xb6, 0x45, 0x2f, 0xfb } };
            auto supports = [a_sc](const GUID& a_iid) -> void* {
                void* p = nullptr;
                if (SUCCEEDED(a_sc->QueryInterface(a_iid, &p)) && p) {
                    return p;   // caller releases
                }
                return nullptr;
            };
            if (void* sc3 = supports(kIID_SC3); sc3) {
                if (auto** vt3 = *reinterpret_cast<void***>(sc3); vt3 && vt3[36]) {
                    using GetCurrentBackBufferIndexFn = UINT(__stdcall*)(void*);
                    // v1.3.2: never call a vtable slot we have not verified. Slot 36 is far past what a
                    // wrapper normally implements; calling junk here is `call rdx` with a garbage target.
                    if (IsExecutableAddress(vt3[36])) {
                        actual = reinterpret_cast<GetCurrentBackBufferIndexFn>(vt3[36])(sc3);
                    }
                    fromChain3 = true;
                }
                reinterpret_cast<IUnknown*>(sc3)->Release();
            }
            static std::atomic<bool> s_bbIdxLogged{ false };
            if (!s_bbIdxLogged.exchange(true)) {
                // Identity of the object we are drawing on. A genuine DXGI swap chain answers all four
                // interfaces and reports the game's own window; a proxy/composited wrapper created by a
                // third-party module typically answers only some of them -- and if the window it reports
                // is not the one we draw on, nothing we write here can ever be scanned out.
                void* p1 = supports(kIID_SC1);
                void* p2 = supports(kIID_SC2);
                void* p4 = supports(kIID_SC4);
                HWND hwndFromChain = nullptr;
                if (p1) {
                    reinterpret_cast<IDXGISwapChain1*>(p1)->GetHwnd(&hwndFromChain);
                }
                logger::info("[Chain] object: SwapChain1={} 2={} 3={} 4={} ({}) hwndFromChain=0x{:X} descOutputWindow=0x{:X}{}",
                    p1 ? "yes" : "NO", p2 ? "yes" : "NO", fromChain3 ? "yes" : "NO", p4 ? "yes" : "NO",
                    (p1 && p4) ? "looks like a genuine DXGI chain" : "*** partial QI surface -- possible wrapper ***",
                    reinterpret_cast<std::uintptr_t>(hwndFromChain),
                    reinterpret_cast<std::uintptr_t>(desc.OutputWindow),
                    (hwndFromChain && desc.OutputWindow && hwndFromChain != desc.OutputWindow)
                        ? "  *** THE CHAIN PRESENTS TO A DIFFERENT WINDOW THAN THE ONE WE DRAW ON ***" : "");
                if (p1) { reinterpret_cast<IUnknown*>(p1)->Release(); }
                if (p2) { reinterpret_cast<IUnknown*>(p2)->Release(); }
                if (p4) { reinterpret_cast<IUnknown*>(p4)->Release(); }
                logger::info("[Chain] back-buffer index: authority {} / modulo-derived {} / {} buffers / lastPresent {}{}",
                    fromChain3 ? std::to_string(actual) : std::string("n/a"), derived, desc.BufferCount,
                    haveCount ? std::to_string(presented) : std::string("n/a"),
                    (fromChain3 && actual != derived) ? "  <== DISAGREE, using slot 36" : "");
            }
            return fromChain3 ? actual : derived;
        }

        // v1.8.9: sample ONE named back buffer. a_index == 0xFFFFFFFF means "whatever the chain says is
        // current". Mode 8 needs to look at every buffer separately, after the present, to find out
        // whether our pixels survived the rest of the chain or were overwritten there.
        float SampleBufferLumaAt(void* a_swapChain, std::uint32_t a_index)
        {
            using namespace RE::BSGraphics;
            auto* rd = Renderer::GetRendererData();
            auto* device = rd ? reinterpret_cast<ID3D11Device*>(rd->forwarder) : nullptr;
            auto* ctx = rd ? reinterpret_cast<ID3D11DeviceContext*>(rd->context) : nullptr;
            if (!device || !ctx || !a_swapChain) {
                return -1.0f;
            }
            auto* sc = static_cast<IDXGISwapChain*>(a_swapChain);
            const std::uint32_t idx = (a_index == 0xFFFFFFFFu) ? CurrentBackBufferIndex(sc) : a_index;
            ID3D11Texture2D* back = nullptr;
            if (FAILED(sc->GetBuffer(idx, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) {
                return -1.0f;
            }
            D3D11_TEXTURE2D_DESC desc{};
            back->GetDesc(&desc);
            if (!g_probeTex) {
                D3D11_TEXTURE2D_DESC sd = desc;
                sd.Width = 8;
                sd.Height = 8;
                sd.Usage = D3D11_USAGE_STAGING;
                sd.BindFlags = 0;
                sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                sd.MiscFlags = 0;
                sd.MipLevels = 1;
                sd.ArraySize = 1;
                if (FAILED(device->CreateTexture2D(&sd, nullptr, &g_probeTex)) || !g_probeTex) {
                    back->Release();
                    return -1.0f;
                }
            }
            const D3D11_BOX box{ desc.Width / 2 - 4, desc.Height / 2 - 4, 0, desc.Width / 2 + 4, desc.Height / 2 + 4, 1 };
            ctx->CopySubresourceRegion(g_probeTex, 0, 0, 0, 0, back, 0, &box);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            float luma = -1.0f;
            if (SUCCEEDED(ctx->Map(g_probeTex, 0, D3D11_MAP_READ, 0, &mapped)) && mapped.pData) {
                const auto* px = static_cast<const std::uint8_t*>(mapped.pData);
                std::uint64_t sum = 0;
                const bool tenBit = (desc.Format == 24);
                for (unsigned int y = 0; y < 8; ++y) {
                    const auto* row = px + static_cast<std::size_t>(y) * mapped.RowPitch;
                    for (unsigned int x = 0; x < 8; ++x) {
                        if (tenBit) {
                            std::uint32_t v = 0;
                            std::memcpy(&v, row + x * 4, 4);
                            sum += (((v >> 20) & 0x3FF) + ((v >> 10) & 0x3FF) + (v & 0x3FF)) / 12u;
                        } else {
                            sum += (row[x * 4 + 0] + row[x * 4 + 1] + row[x * 4 + 2]) / 3u;
                        }
                    }
                }
                luma = static_cast<float>(sum) / 64.0f;
                ctx->Unmap(g_probeTex, 0);
            }
            back->Release();
            return luma;
        }

        float SampleBackbufferLuma(void* a_swapChain)
        {
            return SampleBufferLumaAt(a_swapChain, 0xFFFFFFFFu);
        }

        // true => withhold this frame. Opens the gate by itself as soon as the picture is back.
        bool ShouldWithhold(void* a_swapChain)
        {
            if (g_holdUntil.load() != 0 && NowMs() < g_holdUntil.load()) {
                return true;   // legacy fixed window (unused when the dynamic path is on)
            }
            if (!g_holdActive.load()) {
                return false;
            }
            // v1.3.0 safety guard: never hold the screen past the transition window.
            // This runs on the RENDER thread (the Present hook), which keeps running even while the
            // main thread is blocked inside a cell load. Measured on a stuck transition: our luma
            // trace kept running for the whole 6 s window while the main-thread fallback only landed
            // 31 s late. Without this guard a stalled load leaves the player looking at a frozen
            // frame with the HUD gone -- exactly what two users reported on 1.2.1.
            {
                const auto deadline = g_suppressDeadline.load();
                // Only meaningful while a transition is armed. g_suppressDeadline is never cleared,
                // so without this check a freeze armed outside a transition (the black-frame auto
                // gate) would be released instantly by a stale deadline from a previous door.
                if (g_suppressArmed.load() && deadline != 0 && NowMs() > deadline) {
                    g_holdActive.store(false);
                    logger::warn("[Hold] released by safety guard (past the transition window)");
                    return false;
                }
            }
            const auto elapsed = NowMs() - g_holdStart.load();
            const auto cap = static_cast<std::int64_t>(g_holdCapMs.load());
            const float luma = SampleBackbufferLuma(a_swapChain);
            const bool pictureBack = (luma >= 0.0f) && (luma > 6.0f);
            if (pictureBack || elapsed >= cap || luma < 0.0f) {
                g_holdActive.store(false);
                logger::info("[Hold] released after {} ms (luma {:.1f}, cap {} ms)", elapsed, luma, cap);
                return false;   // present this one -- it has a picture
            }
            return true;
        }

        long HookedPresentBody(void* a_swapChain, unsigned int a_sync, unsigned int a_flags)
        {
            g_frameCount.fetch_add(1, std::memory_order_relaxed);
            g_lastSwapChain.store(a_swapChain);   // v1.16.0: UI 覆盖要用它取最后一帧的像素
            if (ShouldWithhold(a_swapChain)) {
                if (g_heldFrames.fetch_add(1) == 0) {
                    logger::info("[Hold] cut: withholding Present until the picture is back");
                }
                return 0;   // keep the last good frame displayed
            }
            HitchTick();
            TraceTick(a_swapChain);
            capprobe::OnPresent(a_swapChain);
            voidoverlay::NotePresent();
            // Additive overlay for the dead window. No-op unless a transition armed it, and it
            // runs before the suppress check so the pulse can still land on suppressed frames.
            voidoverlay::OnPresent(a_swapChain);
            if (g_suppressPresent.load()) {
                g_skippedFrames.fetch_add(1);
                if (!g_firstSkipLogged.exchange(true)) {
                    logger::info("[B] Present suppressed from here (swapchain {})", a_swapChain);
                }
                return 0;   // S_OK: keep the currently displayed image on screen
            }
            long hr;
            const bool bypass = voidoverlay::BypassToPresent1() && g_origPresent1 != nullptr &&
                                g_flipModel.load();   // v1.3.3: Present1 is flip-model only
            // v1.8.7: read the counter BEFORE we hand the frame down, so the delta belongs to exactly
            // this one Present and to nobody else's.
            UINT presentsBefore = 0;
            const bool audit = voidoverlay::AuditWanted() && a_swapChain && voidoverlay::Active() &&
                               SUCCEEDED(static_cast<IDXGISwapChain*>(a_swapChain)->GetLastPresentCount(&presentsBefore));
            if (bypass) {
                // mode 6 / iSubmitRoute: go around slot 8 (SPS) and submit through slot 22 directly.
                // v1.10.7 route 3: feed the normal chain FIRST so the upscaler sees its usual present,
                // then present again through slot 22 so the frame that lands is ours.
                // v1.3.3 CRASH FIX: pPresentParameters is NOT optional. Passing nullptr makes the
                // call fault inside dxgi.dll (measured: ACCESS_VIOLATION_NULL at dxgi.dll+0x6aab6).
                DXGI_PRESENT_PARAMETERS presentParams{};
                hr = g_origPresent1(a_swapChain, a_sync, a_flags, &presentParams);
            } else {
                hr = g_origPresent ? g_origPresent(a_swapChain, a_sync, a_flags) : 0;
            }
            if (voidoverlay::WantsSurvivalCheck()) {
                voidoverlay::SurvivalCheck(a_swapChain);
            }
            if (audit) {
                static std::atomic<std::uint32_t> s_auditTick{ 0 };
                if ((s_auditTick.fetch_add(1, std::memory_order_relaxed) % 15u) == 0 ||
                    presentsBefore == 0) {
                    voidoverlay::AuditPresent(a_swapChain, static_cast<std::uint32_t>(presentsBefore), hr, bypass);
                }
            }
            // v1.3.1: the return value of the REAL Present is the one fact we never recorded. If it
            // is DXGI_STATUS_OCCLUDED (0x087A0001) the driver is telling us the window is covered,
            // and nothing we draw can ever become visible -- which would look exactly like "the
            // overlay is never presented".
            if (voidoverlay::NotePresentResult(hr)) {
                const auto u = static_cast<unsigned long>(hr);
                const char* note = (u == 0x087A0001ul) ? "  <== DXGI_STATUS_OCCLUDED: the window is occluded"
                                  : (u == 0x087A0002ul) ? "  <== DXGI_STATUS_MODE_CHANGED"
                                  : (u == 0x087A0007ul) ? "  <== DXGI_STATUS_CLIPPED"
                                                        : "";
                logger::info("[Present] the real Present returned 0x{:08X}{}", static_cast<unsigned>(hr), note);
            }
            // v1.3.1 mode 4: flood AFTER the real Present, then present once more. Diagnostic only.
            if (voidoverlay::WantsPostPresentProbe()) {
                voidoverlay::PostPresentProbe(a_swapChain);
                if (g_origPresent) {
                    g_origPresent(a_swapChain, a_sync, a_flags);
                }
            }
            return hr;
        }

        // Same body for IDXGISwapChain1::Present1 (slot 22). A flip-model chain can present through
        // Present1 only, in which case the slot-8 hook would never run -- and the overlay with it.
        long HookedPresent1Body(void* a_swapChain, unsigned int a_sync, unsigned int a_flags, const void* a_params)
        {
            g_frameCount.fetch_add(1, std::memory_order_relaxed);
            if (ShouldWithhold(a_swapChain)) {
                g_heldFrames.fetch_add(1);
                return 0;
            }
            capprobe::OnPresent(a_swapChain);
            voidoverlay::NotePresent();
            voidoverlay::OnPresent(a_swapChain);
            if (g_suppressPresent.load()) {
                g_skippedFrames.fetch_add(1);
                if (!g_firstSkipLogged.exchange(true)) {
                    logger::info("[B] Present1 suppressed from here (swapchain {})", a_swapChain);
                }
                return 0;
            }
            // v1.3.2: pPresentParameters may not be NULL. If a forwarder hands us one, substitute a
            // cleared block instead of passing the NULL through and faulting inside dxgi.dll.
            static thread_local DXGI_PRESENT_PARAMETERS safeParams{};
            return g_origPresent1 ? g_origPresent1(a_swapChain, a_sync, a_flags,
                a_params ? a_params : &safeParams) : 0;
        }

        // v1.10.2: an exception escaping a Present hook lands on the engine's render thread and takes
        // the process with it. Nothing in this file is worth a CTD, so both bodies are wrapped: if
        // anything throws, drop the overlay for the rest of the session and present as if we had
        // never been here.
        long HookedPresent(void* a_swapChain, unsigned int a_sync, unsigned int a_flags)
        {
            try {
                return HookedPresentBody(a_swapChain, a_sync, a_flags);
            } catch (...) {
                logger::error("[Overlay] exception inside the Present hook -- overlay disabled, game kept alive");
                g_bypassToPresent1.store(false);
                voidoverlay::Disarm("exception inside the Present hook");
                return g_origPresent ? g_origPresent(a_swapChain, a_sync, a_flags) : 0;
            }
        }

        long HookedPresent1(void* a_swapChain, unsigned int a_sync, unsigned int a_flags, const void* a_params)
        {
            try {
                return HookedPresent1Body(a_swapChain, a_sync, a_flags, a_params);
            } catch (...) {
                logger::error("[Overlay] exception inside the Present1 hook -- overlay disabled, game kept alive");
                g_bypassToPresent1.store(false);
                voidoverlay::Disarm("exception inside the Present1 hook");
                // v1.3.2: pPresentParameters may not be NULL. If a forwarder hands us one, substitute a
            // cleared block instead of passing the NULL through and faulting inside dxgi.dll.
            static thread_local DXGI_PRESENT_PARAMETERS safeParams{};
            return g_origPresent1 ? g_origPresent1(a_swapChain, a_sync, a_flags,
                a_params ? a_params : &safeParams) : 0;
            }
        }

        bool InstallOn(void* a_swapChain)
        {
            if (!a_swapChain) {
                return false;
            }
            if (a_swapChain == g_hookedSwapChain) {
                return true;
            }
            if (g_gaveUpSwap.load(std::memory_order_relaxed) == a_swapChain) {
                return false;   // already decided this particular chain is not patchable
            }
            auto** vtbl = *reinterpret_cast<void***>(a_swapChain);
            if (!vtbl) {
                return false;
            }
            constexpr std::size_t kPresentIdx = 8;   // IDXGISwapChain::Present

            // v1.3.2 CRASH FIX #2 (measured on a Community Shaders DXGISwapChainProxy):
            // skipping slot 22 was NOT enough. That object answers QI for the newer interfaces but its
            // vtable does not actually carry them, so *any* call through the slots past IDXGISwapChain
            // lands on junk. The second crash kept the same signature (`call rdx`, junk pointer, our
            // Present hook on the stack) even with slot 22 untouched.
            // So: probe the object first. If slot 22 is not real code, treat the WHOLE vtable as
            // untrustworthy and hook nothing at all -- the loading-screen removal and the timer
            // trimming do not need this hook, and the visual layer is off by default anyway.
            {
                IDXGISwapChain1* probe = nullptr;
                if (SUCCEEDED((reinterpret_cast<IUnknown*>(a_swapChain))->QueryInterface(
                        __uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&probe))) && probe) {
                    auto** pvt = *reinterpret_cast<void***>(probe);
                    const bool slot22ok = pvt && IsExecutableAddress(pvt[22]);
                    probe->Release();
                    if (!slot22ok) {
                        logger::warn("[B] this swap chain is a wrapper whose vtable does not actually implement"
                                     " IDXGISwapChain1 (slot 22 is not code) -- hooking NOTHING on it. The visual"
                                     " layer stays off for this session; the loading-screen removal and the timer"
                                     " trimming are unaffected.");
                        g_gaveUpSwap.store(a_swapChain, std::memory_order_relaxed);
                        return false;
                    }
                }
            }

            // v1.3.2 CRASH FIX: validate BEFORE adopting the pointer and BEFORE unprotecting the
            // page. If we adopted first, a proxy that hands back a vtable whose Present entry is
            // not real code would poison g_origPresent forever, and HookedPresent1 forwards into
            // g_origPresent -- so the crash would simply move to the other entry point.
            if (!IsExecutableAddress(vtbl[kPresentIdx])) {
                logger::warn("[B] slot 8 (Present) does not hold a valid code pointer ({}) -- refusing to hook"
                             " this swap chain at all", reinterpret_cast<const void*>(vtbl[kPresentIdx]));
                g_gaveUpSwap.store(a_swapChain, std::memory_order_relaxed);
                return false;
            }
            DWORD oldProtect = 0;
            if (!VirtualProtect(&vtbl[kPresentIdx], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                logger::warn("[B] VirtualProtect failed on the swap chain vtable; freeze-frame disabled");
                g_gaveUpSwap.store(a_swapChain, std::memory_order_relaxed);
                return false;
            }
            if (!g_origPresent) {
                g_origPresent = reinterpret_cast<PresentFn>(vtbl[kPresentIdx]);
            }
            vtbl[kPresentIdx] = reinterpret_cast<void*>(&HookedPresent);
            DWORD ignored = 0;
            VirtualProtect(&vtbl[kPresentIdx], sizeof(void*), oldProtect, &ignored);

            // v1.3.3: Present1 is a flip-model API. On a bitblt chain (DXGI_SWAP_EFFECT_DISCARD /
            // SEQUENTIAL -- the classic 2-buffer path) it is not a supported call, and forwarding
            // into it faults downstream (measured: dxgi.dll+0x6aab6, ACCESS_VIOLATION_NULL).
            // So read the swap effect first and refuse to touch slot 22 unless this really is flip.
            {
                DXGI_SWAP_CHAIN_DESC scd{};
                const bool haveDesc = SUCCEEDED(reinterpret_cast<IDXGISwapChain*>(a_swapChain)->GetDesc(&scd));
                const bool flip = haveDesc && (scd.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
                                               scd.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);
                presentfreeze::g_flipModel.store(flip);
                if (haveDesc && !flip) {
                    logger::warn("[B] swap chain is bitblt (SwapEffect={}, {} buffer(s)) -- Present1 is a"
                                 " flip-model API, so slot 22 is neither hooked nor used on this chain;"
                                 " submissions stay on slot 8",
                        static_cast<int>(scd.SwapEffect), scd.BufferCount);
                }
            }
            // Slot 22 (IDXGISwapChain1::Present1) only exists on a 1+ chain. Writing past the end
            // of a shorter vtable would corrupt the neighbouring .rdata, so ask first.
            IDXGISwapChain1* sc1 = nullptr;
            if (presentfreeze::g_flipModel.load() &&
                SUCCEEDED((reinterpret_cast<IUnknown*>(a_swapChain))->QueryInterface(
                    __uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&sc1))) &&
                sc1) {
                auto** vtbl1 = *reinterpret_cast<void***>(sc1);
                constexpr std::size_t kPresent1Idx = 22;   // IDXGISwapChain1::Present1
                DWORD prot1 = 0;
                // v1.3.2 CRASH FIX: QueryInterface succeeding does NOT prove the vtable actually
                // extends to slot 22. Community Shaders' DXGISwapChainProxy answers the QI but its
                // slot 22 holds junk (measured: 0x43d608efa04bfb29), so adopting it as "original
                // Present1" and calling it later faults at `call rdx` on the first presented frame.
                // Same guard the ProcessMessage / Activate hooks already use.
                const bool slot22IsCode = vtbl1 && IsExecutableAddress(vtbl1[kPresent1Idx]);
                if (vtbl1 && !slot22IsCode) {
                    logger::warn("[B] slot 22 (Present1) does not hold a valid code pointer ({}) -- leaving it alone;"
                                 " this chain is a proxy that does not implement IDXGISwapChain1 properly."
                                 " Overlay submission falls back to slot 8.",
                        reinterpret_cast<const void*>(vtbl1[kPresent1Idx]));
                }
                if (slot22IsCode && VirtualProtect(&vtbl1[kPresent1Idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &prot1)) {
                    if (!g_origPresent1) {
                        g_origPresent1 = reinterpret_cast<Present1Fn>(vtbl1[kPresent1Idx]);
                    }
                    vtbl1[kPresent1Idx] = reinterpret_cast<void*>(&HookedPresent1);
                    DWORD ign1 = 0;
                    VirtualProtect(&vtbl1[kPresent1Idx], sizeof(void*), prot1, &ign1);
                    logger::info("[B] Present1 hook installed (slot 22, original {})",
                        reinterpret_cast<const void*>(g_origPresent1));
                }
                sc1->Release();
            }

            g_hookedSwapChain = a_swapChain;
            g_installed.store(true);
            logger::info("[B] Present hook installed on swapchain {} (original {})",
                a_swapChain, reinterpret_cast<const void*>(g_origPresent));
            return true;
        }

        void* CurrentSwapChain()
        {
            using namespace RE::BSGraphics;
            auto* rd = Renderer::GetRendererData();
            return rd ? static_cast<void*>(rd->renderWindows[0].swapChain) : nullptr;
        }
    }

    // ---- Step 2 probe: present-hook ORDER + optional frame dump (v1.3.1, research only) --------
    // Why: on this setup Community Shaders also hooks Present. Whoever writes the vtable slot LAST
    // sits in the slot and is called FIRST. That decides which image we are looking at:
    //   our hook installed after CS  -> we are called first -> we see the frame BEFORE CS grading
    //   our hook installed before CS -> CS wraps us         -> we are called after CS graded it
    // The order check below needs no D3D call at all: it only compares pointers and module ranges.
    // The optional BMP dump then proves visually which image we actually get.
    namespace capprobe
    {
        using PresentFn  = long (*)(void*, unsigned int, unsigned int);
        using GetBufferFn = long (*)(void*, unsigned int, const GUID*, void**);
        using CreateTexFn = long (*)(void*, const void*, const void*, void**);
        // Map(ID3D11Resource*, UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE*)
        // -- FIVE arguments after `this`. The old typedef dropped MapFlags, so pMappedResource landed in
        // the MapFlags slot and the real out-pointer was read from uninitialised stack: Map() then
        // failed with E_INVALIDARG on every single call (217 x "Map failed" in the field log).
        using MapFn       = long (*)(void*, void*, unsigned int, unsigned int, unsigned int, void*);
        using UnmapFn     = void (*)(void*, void*, unsigned int);
        using CopyResFn   = void (*)(void*, void*, void*);
        using GetDescFn   = void (*)(void*, void*);
        using ReleaseFn   = unsigned long (*)(void*);

        inline constexpr GUID kIIDTexture2D{ 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };

        struct TexDesc
        {
            unsigned int Width, Height, MipLevels, ArraySize, Format;
            unsigned int SampleCount, SampleQuality;
            unsigned int Usage, BindFlags, CPUAccessFlags, MiscFlags;
        };
        struct Mapped
        {
            void*         pData;
            unsigned int  RowPitch, DepthPitch;
        };

        template <class F>
        F Slot(void* a_obj, std::size_t a_idx)
        {
            return reinterpret_cast<F>((*reinterpret_cast<void***>(a_obj))[a_idx]);
        }

        std::atomic<std::uint32_t> g_presents{ 0 };
        // v1.4.3: the field backbuffer is DXGI_FORMAT_R10G10B10A2_UNORM (24), so the old 8-bit-only
        // guard made every capture bail out -- 271 warnings, zero frames. Support it, and stop
        // trying (loudly, once) on a format we still cannot decode.
        std::atomic<bool>          g_captureUnsupported{ false };
        std::atomic<bool>          g_mapFailLogged{ false };
        std::atomic<std::uint32_t> g_captured{ 0 };
        // v1.4.6: a global serial (file names never repeat across transitions), a shared "last capture"
        // stamp so samples are spread over the whole window instead of twelve frames in 0.28 s, and a
        // session-wide ceiling (each BMP used to be 24.9 MB at 4K).
        std::atomic<std::uint32_t> g_serial{ 0 };
        std::atomic<std::int64_t>  g_lastCapAt{ 0 };
        std::atomic<bool>          g_orderChecked{ false };
        void*                      g_slotAddr = nullptr;
        std::atomic<bool>          g_downstream{ false };

        std::string ModuleOf(const void* a_addr)
        {
            HMODULE mod = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCSTR>(a_addr), &mod) &&
                mod) {
                char buf[MAX_PATH]{};
                if (GetModuleFileNameA(mod, buf, MAX_PATH)) {
                    return std::filesystem::path(buf).filename().string();
                }
            }
            return "?";
        }

        // Called once, on the very first Present that reaches us.
        void CheckOrder(void* a_swapChain)
        {
            if (g_orderChecked.exchange(true)) {
                return;
            }
            g_slotAddr = reinterpret_cast<void*>(a_swapChain);
            auto** vtbl = *reinterpret_cast<void***>(a_swapChain);
            const auto slotNow = vtbl ? vtbl[8] : nullptr;
            logger::info("[Cap] present hook installed: our slot fn=0x{:X} in {}",
                reinterpret_cast<std::uintptr_t>(slotNow),
                slotNow ? ModuleOf(slotNow) : "?");
            if (presentfreeze::g_origPresent) {
                const auto orig = reinterpret_cast<const void*>(presentfreeze::g_origPresent);
                logger::info("[Cap] chain below us starts at 0x{:X} in {}",
                    reinterpret_cast<std::uintptr_t>(orig), ModuleOf(orig));
            }
        }

        // Re-check on every Present: if the slot no longer holds our function, someone hooked AFTER
        // us and now sits on top, which means we are called after their work -> downstream.
        void RecheckOrder(void* a_swapChain)
        {
            if (!g_slotAddr || !a_swapChain) {
                return;
            }
            auto** vtbl = *reinterpret_cast<void***>(a_swapChain);
            if (!vtbl) {
                return;
            }
            const auto slotNow = vtbl[8];
            const auto ourFn = reinterpret_cast<void*>(&presentfreeze::HookedPresent);
            if (slotNow != ourFn && !g_downstream.exchange(true)) {
                logger::info("[Cap] another hooker installed on top of us: slot now 0x{:X} in {}",
                    reinterpret_cast<std::uintptr_t>(slotNow),
                    slotNow ? ModuleOf(slotNow) : "?");
                logger::info("[Cap] => we are called AFTER that module processed the frame (downstream: we see the graded image)");
            }
        }

        // a_step > 1 writes a nearest-neighbour downscaled BMP. A 3840x2160 BMP is 24.9 MB, which
        // makes a handful of captures fill a gigabyte; the fingerprint is always taken at full
        // resolution, the BMP only exists to be looked at.
        bool WriteBmp(const std::filesystem::path& a_path, const std::uint8_t* a_pixels,
            unsigned int a_width, unsigned int a_height, unsigned int a_rowPitch, bool a_bgra,
            unsigned int a_step = 1)
        {
            const unsigned int step = (a_step > 0) ? a_step : 1u;
            const unsigned int ow = (a_width / step) ? (a_width / step) : 1u;
            const unsigned int oh = (a_height / step) ? (a_height / step) : 1u;
            const unsigned int rowBytes = ow * 3u;
            const unsigned int pad = (4u - (rowBytes % 4u)) % 4u;
            const unsigned int dataSize = (rowBytes + pad) * oh;
            std::ofstream out(a_path, std::ios::binary);
            if (!out) {
                return false;
            }
            std::uint8_t header[54]{};
            header[0] = 'B'; header[1] = 'M';
            const std::uint32_t fileSize = 54u + dataSize;
            std::memcpy(header + 2, &fileSize, 4);
            const std::uint32_t offset = 54u;
            std::memcpy(header + 10, &offset, 4);
            const std::uint32_t infoSize = 40u;
            std::memcpy(header + 14, &infoSize, 4);
            std::int32_t w = static_cast<std::int32_t>(ow);
            std::int32_t h = static_cast<std::int32_t>(oh);
            std::memcpy(header + 18, &w, 4);
            std::memcpy(header + 22, &h, 4);
            const std::uint16_t planes = 1, bpp = 24;
            std::memcpy(header + 26, &planes, 2);
            std::memcpy(header + 28, &bpp, 2);
            std::memcpy(header + 34, &dataSize, 4);
            out.write(reinterpret_cast<const char*>(header), 54);
            std::vector<std::uint8_t> row(rowBytes + pad, 0);
            for (std::int32_t y = h - 1; y >= 0; --y) {   // BMP stores bottom-up
                const std::uint8_t* src = a_pixels + static_cast<std::size_t>(y) * step * a_rowPitch;
                for (unsigned int x = 0; x < ow; ++x) {
                    const unsigned int sx = x * step;
                    const std::uint8_t b0 = src[sx * 4 + 0];
                    const std::uint8_t b1 = src[sx * 4 + 1];
                    const std::uint8_t b2 = src[sx * 4 + 2];
                    // BGRA source -> BMP BGR wants b,g,r ; RGBA source needs a swap
                    row[x * 3 + 0] = a_bgra ? b0 : b2;
                    row[x * 3 + 1] = b1;
                    row[x * 3 + 2] = a_bgra ? b2 : b0;
                }
                out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(rowBytes + pad));
            }
            logger::info("[Cap] wrote {} ({}x{}, rowPitch {})", a_path.string(), a_width, a_height, a_rowPitch);
            return true;
        }

        // ---- v1.4.1: frame fingerprint ---------------------------------------------------------
        // The question this answers: "is the frame we see at our hook the same one that ends up on
        // screen, and does it change at all while the world is not drawing?"
        //   - identical hash on consecutive presents  => the presented image really is frozen
        //   - different hash                          => something is still drawing
        //   - the `chain=` field names whoever currently sits in vtable slot 8: if that is another
        //     module, it hooked AFTER us, so we are called AFTER it (downstream: we see its output,
        //     i.e. the graded frame). If it is us, we are called first (upstream: pre-grading).
        struct Fingerprint
        {
            double        mean = 0.0;
            std::uint32_t hist[16]{};
            std::uint32_t hash = 0;
        };

        // Samples every 4th pixel in both axes (129 k samples at 1080p): cheap enough to run inside
        // the Present hook, dense enough that a real content change cannot hash the same.
        Fingerprint ComputeFingerprint(const std::uint8_t* a_pixels, unsigned int a_width,
            unsigned int a_height, unsigned int a_rowPitch, bool a_bgra)
        {
            Fingerprint fp{};
            if (!a_pixels || !a_width || !a_height) {
                return fp;
            }
            constexpr unsigned int kGridX = 32;
            constexpr unsigned int kGridY = 18;
            std::uint64_t blockSum[kGridY][kGridX]{};
            std::uint32_t blockCount[kGridY][kGridX]{};
            std::uint64_t total = 0;
            std::uint64_t samples = 0;
            for (unsigned int y = 0; y < a_height; y += 4) {
                const auto* row = a_pixels + static_cast<std::size_t>(y) * a_rowPitch;
                const unsigned int gy = (y * kGridY) / a_height;
                for (unsigned int x = 0; x < a_width; x += 4) {
                    const std::uint8_t c0 = row[x * 4 + 0];
                    const std::uint8_t c1 = row[x * 4 + 1];
                    const std::uint8_t c2 = row[x * 4 + 2];
                    // BGRA source: (B,G,R); RGBA source: (R,G,B). Integer luma, 0..255.
                    const std::uint32_t l = a_bgra
                        ? ((29u * c0 + 150u * c1 + 77u * c2) >> 8)
                        : ((77u * c0 + 150u * c1 + 29u * c2) >> 8);
                    total += l;
                    ++samples;
                    ++fp.hist[(l >> 4) & 0xFu];
                    if (gy < kGridY) {
                        const unsigned int gx = (x * kGridX) / a_width;
                        if (gx < kGridX) {
                            blockSum[gy][gx] += l;
                            ++blockCount[gy][gx];
                        }
                    }
                }
            }
            if (!samples) {
                return fp;
            }
            fp.mean = static_cast<double>(total) / static_cast<double>(samples);
            std::uint32_t h = 2166136261u;
            for (unsigned int gy = 0; gy < kGridY; ++gy) {
                for (unsigned int gx = 0; gx < kGridX; ++gx) {
                    const auto q = blockCount[gy][gx]
                        ? static_cast<std::uint32_t>(blockSum[gy][gx] / blockCount[gy][gx])
                        : 0u;
                    h = (h ^ q) * 16777619u;
                }
            }
            fp.hash = h;
            return fp;
        }

        std::string HistToString(const std::uint32_t (&a_hist)[16])
        {
            std::uint32_t total = 0;
            for (auto v : a_hist) {
                total += v;
            }
            std::string s;
            for (int i = 0; i < 16; ++i) {
                if (!s.empty()) {
                    s += ',';
                }
                s += std::to_string(total ? static_cast<std::uint32_t>((100.0 * a_hist[i]) / total) : 0u);
            }
            return s;
        }

        std::string ChainOwner(void* a_swapChain)
        {
            auto** vtbl = a_swapChain ? *reinterpret_cast<void***>(a_swapChain) : nullptr;
            if (!vtbl) {
                return "?";
            }
            const auto fn = vtbl[8];
            if (fn == reinterpret_cast<void*>(&presentfreeze::HookedPresent)) {
                return "us=first-in-chain";
            }
            return ModuleOf(fn) + "=called-after-us";
        }

        // ---- v1.4.1: capture a burst inside the transition window -------------------------------
        // The old gate was "every 120th present", i.e. roughly one frame per 2 s of ordinary play --
        // which almost never lands inside the ~2 s dead window. Arming a burst makes the next N
        // presents capture back to back, so the frames actually come from the window.
        std::atomic<std::uint32_t> g_windowBurst{ 0 };
        std::atomic<std::int64_t>  g_windowArmedAt{ 0 };

        void ArmWindowCapture(std::uint32_t a_frames)
        {
            if (!g_config.captureFrame) {
                return;
            }
            if (g_serial.load() >= g_config.captureTotalMax) {
                return;
            }
            g_captured.store(0);          // fresh budget for THIS transition, not for the session
            g_windowArmedAt.store(NowMs());
            g_windowBurst.store(a_frames);
            logger::info("[Cap] window burst armed: up to {} frames, one every {} ms (session {} / {})",
                a_frames, g_config.captureEveryMs, g_serial.load(), g_config.captureTotalMax);
        }

        bool CaptureOne(void* a_swapChain, void* a_device, void* a_context, const char* a_tag)
        {
            if (!a_swapChain || !a_device || !a_context) {
                return false;
            }
            auto getBuffer = Slot<GetBufferFn>(a_swapChain, 9);
            auto getDesc = Slot<GetDescFn>(a_device, 5);       // filled below via texture, see note
            (void)getDesc;
            void* tex = nullptr;
            if (getBuffer(a_swapChain, 0, &kIIDTexture2D, &tex) != 0 || !tex) {
                logger::warn("[Cap] GetBuffer failed");
                return false;
            }
            TexDesc desc{};
            Slot<GetDescFn>(tex, 10)(tex, &desc);
            // 87 = B8G8R8A8_UNORM, 28 = R8G8B8A8_UNORM, 24 = R10G10B10A2_UNORM (what this setup runs).
            const bool bgra  = (desc.Format == 87);
            const bool rgba  = (desc.Format == 28);
            const bool rgb10 = (desc.Format == 24);
            if (!bgra && !rgba && !rgb10) {
                if (!g_captureUnsupported.exchange(true)) {
                    logger::warn("[Cap] backbuffer format {} is not decodable (28/87/24 are); capture disabled for this session",
                        desc.Format);
                }
                Slot<ReleaseFn>(tex, 2)(tex);
                return false;
            }
            TexDesc staged = desc;
            staged.MipLevels = 1;
            staged.ArraySize = 1;
            staged.Usage = 3;            // D3D11_USAGE_STAGING
            staged.BindFlags = 0;
            staged.CPUAccessFlags = 0x20000;   // D3D11_CPU_ACCESS_READ
            staged.MiscFlags = 0;
            void* staging = nullptr;
            if (Slot<CreateTexFn>(a_device, 5)(a_device, &staged, nullptr, &staging) != 0 || !staging) {
                logger::warn("[Cap] staging texture creation failed");
                Slot<ReleaseFn>(tex, 2)(tex);
                return false;
            }
            Slot<CopyResFn>(a_context, 47)(a_context, staging, tex);
            Mapped mapped{};
            constexpr unsigned int kMapRead = 1u;   // D3D11_MAP_READ -- the staging texture is CPU_ACCESS_READ
            if (Slot<MapFn>(a_context, 14)(a_context, staging, 0, kMapRead, 0, &mapped) != 0 || !mapped.pData) {
                if (!g_mapFailLogged.exchange(true)) {
                    logger::warn("[Cap] Map failed (logged once; further failures are silent)");
                }
                Slot<ReleaseFn>(staging, 2)(staging);
                Slot<ReleaseFn>(tex, 2)(tex);
                return false;
            }
            // 10-bit path: expand to 8-bit BGRA once, then feed both the fingerprint and the BMP from
            // that buffer, so there is exactly one place that knows about the bit layout.
            std::vector<std::uint8_t> expanded;
            const std::uint8_t* pixels = static_cast<const std::uint8_t*>(mapped.pData);
            unsigned int       rowPitch = mapped.RowPitch;
            if (rgb10) {
                expanded.resize(static_cast<std::size_t>(desc.Width) * desc.Height * 4);
                for (unsigned int y = 0; y < desc.Height; ++y) {
                    const auto* srcRow = static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
                    auto*       dstRow = expanded.data() + static_cast<std::size_t>(y) * desc.Width * 4;
                    for (unsigned int x = 0; x < desc.Width; ++x) {
                        std::uint32_t v = 0;
                        std::memcpy(&v, srcRow + static_cast<std::size_t>(x) * 4, 4);
                        dstRow[x * 4 + 0] = static_cast<std::uint8_t>(((v >> 20) & 0x3FF) * 255 / 1023);   // B
                        dstRow[x * 4 + 1] = static_cast<std::uint8_t>(((v >> 10) & 0x3FF) * 255 / 1023);   // G
                        dstRow[x * 4 + 2] = static_cast<std::uint8_t>(((v >> 0) & 0x3FF) * 255 / 1023);    // R
                        dstRow[x * 4 + 3] = 255;
                    }
                }
                pixels = expanded.data();
                rowPitch = desc.Width * 4;
            }
            const bool pixBgra = !rgba;   // 87 and the expanded 10-bit buffer are BGRA, 28 is RGBA
            const auto fp = ComputeFingerprint(pixels, desc.Width, desc.Height, rowPitch, pixBgra);
            const auto dir = std::filesystem::path("LoadingFlow-capture");
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            const auto n = g_serial.fetch_add(1) + 1;
            const auto slot = g_captured.fetch_add(1) + 1;
            const auto path = dir / std::format("cap-{:02}-{}.bmp", n, a_tag);
            // Keep the BMP browsable: cap its long edge at ~1280 px, the fingerprint stays full-res.
            const unsigned int bmpStep = (desc.Width > 1280u) ? (desc.Width / 1280u) : 1u;
            WriteBmp(path, pixels, desc.Width, desc.Height, rowPitch, pixBgra, bmpStep);
            const auto armedAt = g_windowArmedAt.load();
            const auto relMs = armedAt ? (NowMs() - armedAt) : -1;
            std::string line = std::format(
                "cap-{:02} (win#{}) tag={} {}x{} fmt={} mean={:.2f} hash=0x{:08X} chain={} hist={} t={}ms -> {}",
                n, slot, a_tag, desc.Width, desc.Height, desc.Format, fp.mean, fp.hash,
                ChainOwner(a_swapChain), HistToString(fp.hist),
                relMs >= 0 ? std::to_string(relMs) : std::string("n/a"),
                std::filesystem::absolute(path, ec).string());
            logger::info("[Cap] {}", line);
            Slot<UnmapFn>(a_context, 15)(a_context, staging, 0);
            Slot<ReleaseFn>(staging, 2)(staging);
            Slot<ReleaseFn>(tex, 2)(tex);
            return true;
        }

        // Called from the Present hook, before we decide to skip.
        void OnPresent(void* a_swapChain)
        {
            if (!g_config.capProbe && !g_config.captureFrame) {
                return;
            }
            CheckOrder(a_swapChain);
            RecheckOrder(a_swapChain);
            if (!g_config.captureFrame) {
                return;
            }
            const auto n = g_presents.fetch_add(1) + 1;
            if (g_captured.load() >= g_config.captureFrames || g_captureUnsupported.load()) {
                return;
            }
            // v1.4.1: a transition-window burst captures consecutive presents, so the frames really
            // come from the dead window instead of from ordinary play.
            const bool inWindow = (g_windowBurst.load() > 0);
            if (inWindow) {
                const auto lastCap = g_lastCapAt.load();
                const auto nowCap = NowMs();
                if (lastCap != 0 && (nowCap - lastCap) < static_cast<std::int64_t>(g_config.captureEveryMs)) {
                    return;   // one sample per captureEveryMs, so the frames span the whole window
                }
                g_windowBurst.fetch_sub(1);
                g_lastCapAt.store(nowCap);
            } else if (!g_config.captureLiveFrames || (n % 120) != 0) {
                // v1.4.5: idle frames are opt-in; otherwise they eat the budget the window burst needs.
                return;
            }
            if (g_serial.load() >= g_config.captureTotalMax) {
                return;
            }
            using namespace RE::BSGraphics;
            auto* rd = Renderer::GetRendererData();
            if (!rd) {
                return;
            }
            const char* tag = inWindow
                ? "win"
                : (presentfreeze::g_suppressPresent.load() ? "frozen" : "live");
            CaptureOne(a_swapChain, static_cast<void*>(rd->forwarder), static_cast<void*>(rd->context), tag);
        }
    }

    // ---- Void overlay (v1.4.0, EXPERIMENTAL): fill the dead window with motion ----------------
    // The measured timeline of one real transition is: press E, hand touches the door, then ~124
    // frames (~2.07 s) in which the old cell is detaching and nothing new is drawn -- the screen
    // is a single frozen picture -- then ~21 frames (~350 ms) of scene spin, then the new cell.
    //
    // The 2 s are not black, they are DEAD, and that is what reads as "stiff". Every route that
    // darkened the picture has been rejected (it drags the mod back towards the vanilla loading
    // screen, which is mostly plain black), so this one only ADDS light: a slow breathing lift, a
    // field of soft drifting motes, and one pulse at the cut that hands over to the engine's spin.
    //
    // Why Present: while the world is not drawing there is nowhere else to draw. The swap chain is
    // the only surface guaranteed to be touched, it still runs at the game's frame rate, and the
    // frame we add to already contains whatever ENB / Community Shaders produced -- we never touch
    // their pixels. Blend state is (ONE, ONE): additive, so nothing can be darkened, ever.
    //
    // Failure policy: any D3D error disables the overlay for the rest of the session and says so
    // in the log. It can never take presentation down with it -- the game's own Present is called
    // unchanged either way, and the state block we touch is saved and restored around the draw.
    namespace voidoverlay
    {
        struct alignas(16) Params
        {
            float resolution[2];
            float age;
            float phase;
            float phaseAge;
            float bright;
            float breath;
            float pulseStrength;
            float motes;
            float pulseMs;
            float moteScale;
            float blitAlpha;   // 1 = fully the old frame, 0 = fully the new one
            float zoom;        // v1.3.0: 1.0 = pixel exact, >1 = pushed in (see PSBlit)
            float blurPx;      // v1.10.0: frosted-glass radius in pixels (0 = sharp)
            float pad[2];      // keeps the struct at 64 B, matching the HLSL cbuffer
        };
        static_assert(sizeof(Params) == 64, "Params must match the HLSL cbuffer layout exactly");

        std::atomic<bool>          g_armed{ false };
        std::atomic<bool>          g_cut{ false };
        std::atomic<bool>          g_failed{ false };
        std::atomic<std::int64_t>  g_armAt{ 0 };
        std::atomic<std::int64_t>  g_cutAt{ 0 };
        // v1.10.3 unveil detection: the engine does not reveal the finished cell at the swap. Measured
        // on this setup it sits on a dark frame (luma ~35) for ~800 ms -- exactly fPostLoadUpdateTimeMS
        // -- and then jumps straight to the real scene (luma ~110). A fixed-length dissolve can never
        // hide that: it ends before the jump, so the jump lands with nothing covering it. That step IS
        // the "violent flash". So hold the old picture and start the dissolve when the picture changes.
        std::atomic<float>         g_cutBaseLuma{ -1.0f };   // engine luma right after the swap
        std::atomic<std::int64_t>  g_dissolveAt{ 0 };        // when the alpha ramp starts (0 = not yet)
        std::atomic<std::int64_t>  g_lastCutSample{ 0 };
        std::atomic<float>         g_prevCutLuma{ -1.0f };
        std::atomic<bool>          g_waitCapLogged{ false };
        std::atomic<std::int64_t>  g_lastDrawAt{ 0 };
        std::atomic<std::uint32_t> g_draws{ 0 };
        // Diagnostic: how many Present/Present1 calls actually reached us while a transition was
        // armed. If the game stops presenting during the dead window this counter stays at 0 and
        // the log says so -- that is the difference between "the effect is invisible" and "there
        // was never a frame to draw on".
        std::atomic<std::uint32_t> g_presentSeen{ 0 };
        // Config snapshot taken at Arm() so the render thread never reads g_config directly.
        std::atomic<float>         g_cfgBright{ 0.55f };
        std::atomic<float>         g_cfgBreath{ 0.02f };
        std::atomic<float>         g_cfgPulse{ 0.55f };
        std::atomic<float>         g_cfgMoteScale{ 1.0f };
        std::atomic<std::uint32_t> g_cfgMotes{ 18 };
        std::atomic<std::uint32_t> g_cfgPulseMs{ 300 };
        std::atomic<float>         g_cfgCrossFadeMs{ 350 };
        std::atomic<std::uint32_t> g_cfgCrossFadeHoldMs{ 200 };
        std::atomic<bool>          g_cfgCrossFade{ true };
        // v1.3.0 frozen-frame pass + probe
        std::atomic<int>           g_cfgVisualMode{ 1 };
        std::atomic<float>         g_cfgDeadZoomAmount{ 0.0f };
        std::atomic<float>         g_cfgDeadBlurPx{ 40.0f };
        std::atomic<std::uint32_t> g_cfgDeadZoomMs{ 2000 };
        std::atomic<std::uint32_t> g_cfgProbeFrames{ 90 };
        std::atomic<int>           g_probeLeft{ 0 };
        std::atomic<long>          g_lastPresentHr{ 0 };
        std::atomic<bool>          g_probeReadbackDone{ false };
        std::atomic<bool>          g_probeAllBuffersLogged{ false };
        std::atomic<std::uint32_t> g_transientFailures{ 0 };
        std::atomic<bool>          g_presentHrLogged{ false };
        std::atomic<int>           g_postProbeLeft{ 0 };   // v1.3.1 mode 4
        std::atomic<bool>          g_cfgUsePresent1{ false };   // v1.3.4 blit frames may bypass slot 8
        std::atomic<int>           g_cfgSubmitRoute{ 0 };       // v1.8.7 phase-scoped submit routing
        std::atomic<bool>          g_cfgPresentAudit{ false };  // v1.8.7 ask DXGI whether it counted
        std::atomic<bool>          g_bypassBlockedBySuppress{ false };
        std::atomic<std::uint32_t> g_survivalLeft{ 0 };   // v1.8.9 mode 8 frames still to inspect
        std::atomic<std::uint32_t> g_survivalTick{ 0 };

        // ---- v1.8.9 mode 8: did our pixels survive the rest of the chain? ------------------------
        // Mode 7 proved the buffer choice is not the problem: both back buffers were flooded and the
        // screen stayed clean. That leaves exactly two possibilities, and this separates them:
        //   * the buffers still hold magenta after we hand the frame down  -> nothing rewrote them, so
        //     the buffer we filled is not the one that gets scanned out (the object we hold is not the
        //     chain that reaches the display);
        //   * the buffers no longer hold magenta                          -> something below us redraws
        //     the frame between our clear and the flip.
        bool WantsSurvivalCheck()
        {
            return g_survivalLeft.load(std::memory_order_relaxed) > 0;
        }

        void SurvivalCheck(void* a_swapChain)
        {
            if (!a_swapChain) {
                return;
            }
            const std::uint32_t left = g_survivalLeft.load(std::memory_order_relaxed);
            if (left == 0) {
                return;
            }
            const std::uint32_t tick = g_survivalTick.fetch_add(1, std::memory_order_relaxed);
            if ((tick % 30u) != 0) {
                g_survivalLeft.fetch_sub(1, std::memory_order_relaxed);
                return;
            }
            auto* sc = static_cast<IDXGISwapChain*>(a_swapChain);
            DXGI_SWAP_CHAIN_DESC d{};
            if (FAILED(sc->GetDesc(&d))) {
                g_survivalLeft.fetch_sub(1, std::memory_order_relaxed);
                return;
            }
            for (UINT i = 0; i < d.BufferCount && i < 4; ++i) {
                const float l = presentfreeze::SampleBufferLumaAt(a_swapChain, i);
                const bool magenta = (l > 150.0f && l < 190.0f);
                logger::info("[Probe] after present: buffer {}/{} luma {:.1f} -> {}", i, d.BufferCount, l,
                    magenta ? "STILL OUR MAGENTA -- nothing rewrote it, so this buffer is never scanned out"
                            : "content was replaced after our clear -- something below us redraws the frame");
            }
            g_survivalLeft.fetch_sub(1, std::memory_order_relaxed);
        }

        // ---- v1.8.7: did the frame we forwarded actually get presented? -------------------------
        // Every earlier verdict of "the pixels are discarded somewhere below us" rested on
        // `[Present] the real Present returned 0x00000000`, and S_OK proves only that the function
        // we called did not report an error. It does not prove the frame reached the display, and
        // this file has three paths that return 0 without presenting anything at all.
        // GetLastPresentCount is the one counter in this process that answers the real question, so
        // read it before and after we hand the frame down. delta 0 = the chain never presented our
        // frame (our fault, not the compositor's); delta >= 1 = DXGI accepted it and whatever goes
        // wrong afterwards is downstream of DXGI.
        // GetFrameStatistics is deliberately not used: it is unsupported on a flip-model chain
        // (DXGI_ERROR_UNSUPPORTED), and this chain is FLIP_DISCARD.
        bool AuditWanted()
        {
            return g_cfgPresentAudit.load(std::memory_order_relaxed);
        }

        void AuditPresent(void* a_swapChain, std::uint32_t a_presentsBefore, long a_hr, bool a_bypassed)
        {
            if (!a_swapChain) {
                return;
            }
            auto* sc = static_cast<IDXGISwapChain*>(a_swapChain);
            UINT after = 0;
            if (FAILED(sc->GetLastPresentCount(&after))) {
                logger::info("[PA] GetLastPresentCount failed -- cannot tell whether the frame was counted");
                return;
            }
            const long delta = static_cast<long>(after) - static_cast<long>(a_presentsBefore);
            DXGI_SWAP_CHAIN_DESC desc{};
            const bool haveDesc = SUCCEEDED(sc->GetDesc(&desc));
            // v1.8.8: `delta 0` is NOT proof that the frame was dropped. The shipped v1.8.7 log showed
            // the counter advancing by ~30 between two samples while reading 0 across our own call,
            // i.e. GetLastPresentCount is incremented asynchronously, not inside the call we make.
            // So this line now reports both numbers and never cries wolf: `sincePrev` is the real
            // "is the chain still presenting at all" signal.
            static std::atomic<std::uint32_t> s_lastSeen{ 0 };
            const std::uint32_t prevSeen = s_lastSeen.exchange(after, std::memory_order_relaxed);
            const long sincePrev = static_cast<long>(after) - static_cast<long>(prevSeen);
            logger::info("[PA] present {} -> {} (delta {}, sincePrev {}) hr=0x{:08X}{} route={} bypass={} cut={} bbidx={} of {}",
                a_presentsBefore, after, delta, sincePrev, static_cast<unsigned>(a_hr),
                sincePrev > 0 && delta == 0 ? "  (counter is asynchronous -- delta 0 here is normal)" : "",
                g_cfgSubmitRoute.load(std::memory_order_relaxed), a_bypassed ? 1 : 0,
                g_cut.load(std::memory_order_relaxed) ? 1 : 0,
                presentfreeze::CurrentBackBufferIndex(sc), haveDesc ? desc.BufferCount : 0u);
        }
        std::atomic<bool>          g_fadeLogged{ false };
        std::atomic<bool>          g_fadeResWarned{ false };
        std::atomic<bool>          g_cfgDebugFull{ false };
        std::atomic<int>           g_selfCheckLeft{ 0 };
        std::atomic<std::uint32_t> g_cfgMaxMs{ 4000 };

        // Created on and used by the render thread only (inside the Present hook).
        ID3D11VertexShader*    g_vs = nullptr;
        ID3D11PixelShader*     g_ps = nullptr;
        ID3D11Buffer*          g_cb = nullptr;
        ID3D11BlendState*      g_blend = nullptr;
        ID3D11RasterizerState* g_raster = nullptr;
        // v1.4.7 cross-fade: the last frame of the old cell, blitted 1:1 over the new one at the cut.
        ID3D11PixelShader*            g_blitPs = nullptr;
        ID3D11BlendState*             g_alphaBlend = nullptr;
        ID3D11SamplerState*           g_sampler = nullptr;
        ID3D11Texture2D*              g_snapTex = nullptr;
        ID3D11ShaderResourceView*     g_snapSrv = nullptr;
        UINT                          g_snapW = 0;
        UINT                          g_snapH = 0;
        DXGI_FORMAT                   g_snapFmt = DXGI_FORMAT_UNKNOWN;
        std::int64_t                  g_snapAt = 0;
        bool                          g_snapValid = false;      // g_snapTex holds a real picture
        float                         g_snapLuma = -1.0f;       // luma of the frame we copied
        float                         g_snapMaxLuma = -1.0f;    // brightest frame seen this transition
        std::int64_t                  g_snapSampleAt = 0;       // last luma sample (10 Hz throttle)

        template <class T>
        void SafeRelease(T*& a_ptr)
        {
            if (a_ptr) {
                a_ptr->Release();
                a_ptr = nullptr;
            }
        }

        // ---- v1.3.0 presentation-chain forensics (one line each, first draw only) --------------
        // Every "we drew it but it is not on screen" mystery so far was argued from proxies. This
        // prints the chain itself, once per session: which texture we draw on, which swap chain it
        // came from, what that chain's effect mode is, and whether its device is the one
        // RendererData advertises. If those two disagree, that is the whole answer.
        std::atomic<bool> g_chainLogged{ false };

        const char* FormatName(DXGI_FORMAT a_fmt)
        {
            switch (a_fmt) {
            case DXGI_FORMAT_R8G8B8A8_UNORM:        return "R8G8B8A8_UNORM";
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   return "R8G8B8A8_UNORM_SRGB";
            case DXGI_FORMAT_B8G8R8A8_UNORM:        return "B8G8R8A8_UNORM";
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:   return "B8G8R8A8_UNORM_SRGB";
            case DXGI_FORMAT_R10G10B10A2_UNORM:     return "R10G10B10A2_UNORM (10-bit)";
            case DXGI_FORMAT_R16G16B16A16_FLOAT:    return "R16G16B16A16_FLOAT";
            default:                                return "other";
            }
        }

        const char* EffectName(DXGI_SWAP_EFFECT a_effect)
        {
            switch (a_effect) {
            case DXGI_SWAP_EFFECT_DISCARD:            return "DISCARD (bitblt)";
            case DXGI_SWAP_EFFECT_SEQUENTIAL:         return "SEQUENTIAL (bitblt)";
            case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL:    return "FLIP_SEQUENTIAL";
            case DXGI_SWAP_EFFECT_FLIP_DISCARD:       return "FLIP_DISCARD";
            default:                                  return "unknown";
            }
        }

        void LogChain(IDXGISwapChain* a_sc, ID3D11Texture2D* a_back, ID3D11Device* a_device, ID3D11DeviceContext* a_ctx)
        {
            using namespace RE::BSGraphics;
            D3D11_TEXTURE2D_DESC td{};
            a_back->GetDesc(&td);
            DXGI_SWAP_CHAIN_DESC sd{};
            const bool haveDesc = SUCCEEDED(a_sc->GetDesc(&sd));

            auto*             rd = Renderer::GetRendererData();
            auto*             gameDev = rd ? reinterpret_cast<ID3D11Device*>(rd->forwarder) : nullptr;
            ID3D11DeviceContext* gameCtx = rd ? reinterpret_cast<ID3D11DeviceContext*>(rd->context) : nullptr;
            const bool        sameDev = (gameDev == a_device);
            const bool        sameCtx = (gameCtx == a_ctx);

            logger::info("[Chain] drawing on {}x{} fmt={} ({}, 10bit={}), cbuffer {} B",
                td.Width, td.Height, static_cast<int>(td.Format), FormatName(td.Format),
                td.Format == DXGI_FORMAT_R10G10B10A2_UNORM, sizeof(Params));
            if (haveDesc) {
                logger::info("[Chain] swap chain: {} buffer(s), effect {}, windowed={}, hwnd=0x{:X}, refresh {}",
                    sd.BufferCount, EffectName(sd.SwapEffect), sd.Windowed ? 1 : 0,
                    reinterpret_cast<std::uintptr_t>(sd.OutputWindow),
                    sd.BufferDesc.RefreshRate.Numerator);
            }
            logger::info("[Chain] device 0x{:X} vs RendererData 0x{:X} -> same={}", reinterpret_cast<std::uintptr_t>(a_device),
                reinterpret_cast<std::uintptr_t>(gameDev), sameDev ? "YES" : "*** NO ***");
            logger::info("[Chain] context 0x{:X} vs RendererData 0x{:X} -> same={}", reinterpret_cast<std::uintptr_t>(a_ctx),
                reinterpret_cast<std::uintptr_t>(gameCtx), sameCtx ? "YES" : "*** NO ***");
            if (!sameDev) {
                logger::warn("[Chain] MISMATCH: the buffer we draw on does not belong to the device RendererData"
                             " exposes. Drawing through the RendererData context would have gone nowhere -- that is"
                             " exactly the v1.4.x 'we changed the back buffer, the screen never changed' symptom.");
            }
            // v1.3.1: the engine can own more than one render window / swap chain. Drawing on a chain
            // that is rendered but never scanned out would be invisible while every other measurement
            // still looks perfectly healthy -- so list all of them and mark the one we hold.
            if (rd) {
                int shown = 0;
                for (int i = 0; i < 32 && shown < 4; ++i) {
                    auto& w = rd->renderWindows[i];
                    if (!w.swapChain) {
                        continue;
                    }
                    ++shown;
                    const bool ours = (reinterpret_cast<void*>(w.swapChain) == static_cast<void*>(a_sc));
                    logger::info("[Chain] renderWindow[{}] hwnd=0x{:X} {}x{} swapChain=0x{:X}{}", i,
                        reinterpret_cast<std::uintptr_t>(w.hWnd), w.windowWidth, w.windowHeight,
                        reinterpret_cast<std::uintptr_t>(w.swapChain),
                        ours ? "  <== the one we draw on" : "  (NOT the one we draw on)");
                }
                if (shown > 1) {
                    logger::warn("[Chain] the engine owns {} render window(s) -- if the visible one is not ours,"
                                 " nothing we draw can ever appear", shown);
                }
            }

            // v1.3.2: WHO ELSE is on the present slots. `[Cap] chain below us … in SPS.dll` proved
            // Skyrim Performance Solution sits downstream on slot 8. If slot 22 is still untouched
            // DXGI code, submitting through it puts our pixels on screen after SPS has done its pass.
            if (auto** vtbl = *reinterpret_cast<void***>(a_sc); vtbl) {
                logger::info("[Chain] vtable[8]  Present  = 0x{:X} in {}", reinterpret_cast<std::uintptr_t>(vtbl[8]),
                    vtbl[8] ? capprobe::ModuleOf(vtbl[8]) : "?");
                logger::info("[Chain] vtable[22] Present1 = 0x{:X} in {}", reinterpret_cast<std::uintptr_t>(vtbl[22]),
                    vtbl[22] ? capprobe::ModuleOf(vtbl[22]) : "?");
            }
            logger::info("[Chain] our orig Present  = 0x{:X} in {}",
                reinterpret_cast<std::uintptr_t>(presentfreeze::g_origPresent),
                presentfreeze::g_origPresent ? capprobe::ModuleOf(reinterpret_cast<const void*>(presentfreeze::g_origPresent)) : "?");
            logger::info("[Chain] our orig Present1 = 0x{:X} in {}",
                reinterpret_cast<std::uintptr_t>(presentfreeze::g_origPresent1),
                presentfreeze::g_origPresent1 ? capprobe::ModuleOf(reinterpret_cast<const void*>(presentfreeze::g_origPresent1)) : "?");
        }

        // Which device the cached objects were created on. Borrowed, never AddRef'd: we only use it
        // to notice that the device was replaced (resolution change, alt-tab, fullscreen toggle),
        // because a resource created on the old device silently draws nothing on the new one.
        ID3D11Device* g_resDevice = nullptr;

        void ReleaseResources()
        {
            SafeRelease(g_vs);
            SafeRelease(g_ps);
            SafeRelease(g_cb);
            SafeRelease(g_blend);
            SafeRelease(g_raster);
            SafeRelease(g_blitPs);
            SafeRelease(g_alphaBlend);
            SafeRelease(g_sampler);
            SafeRelease(g_snapSrv);
            SafeRelease(g_snapTex);
            g_snapW = 0;
            g_snapH = 0;
            g_snapFmt = DXGI_FORMAT_UNKNOWN;
            g_snapAt = 0;
            g_resDevice = nullptr;
        }

        bool EnsureResources(ID3D11Device* a_device)
        {
            if (!a_device) {
                return false;
            }
            if (g_resDevice && g_resDevice != a_device) {
                logger::info("[Overlay] the D3D device changed -- rebuilding overlay resources");
                ReleaseResources();
            }
            if (g_vs && g_ps && g_cb && g_blend && g_raster) {
                return true;
            }
            if (FAILED(a_device->CreateVertexShader(g_VoidVS, sizeof(g_VoidVS), nullptr, &g_vs))) {
                logger::warn("[Overlay] CreateVertexShader failed; overlay disabled");
                g_failed.store(true);
                return false;
            }
            if (FAILED(a_device->CreatePixelShader(g_VoidPS, sizeof(g_VoidPS), nullptr, &g_ps))) {
                logger::warn("[Overlay] CreatePixelShader failed; overlay disabled");
                g_failed.store(true);
                return false;
            }
            D3D11_BUFFER_DESC cbDesc{};
            cbDesc.ByteWidth = sizeof(Params);
            cbDesc.Usage = D3D11_USAGE_DYNAMIC;
            cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(a_device->CreateBuffer(&cbDesc, nullptr, &g_cb))) {
                logger::warn("[Overlay] constant buffer creation failed; overlay disabled");
                g_failed.store(true);
                return false;
            }
            D3D11_BLEND_DESC blendDesc{};
            blendDesc.RenderTarget[0].BlendEnable = TRUE;
            blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
            blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
            blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
            blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
            blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            if (FAILED(a_device->CreateBlendState(&blendDesc, &g_blend))) {
                logger::warn("[Overlay] blend state creation failed; overlay disabled");
                g_failed.store(true);
                return false;
            }
            if (FAILED(a_device->CreatePixelShader(g_BlitPS, sizeof(g_BlitPS), nullptr, &g_blitPs))) {
                logger::warn("[Overlay] blit pixel shader failed; cross-fade disabled for this session");
            }
            D3D11_BLEND_DESC alphaDesc{};
            alphaDesc.RenderTarget[0].BlendEnable = TRUE;
            alphaDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
            alphaDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            alphaDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            alphaDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            alphaDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            alphaDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            alphaDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            if (FAILED(a_device->CreateBlendState(&alphaDesc, &g_alphaBlend))) {
                logger::warn("[Overlay] alpha blend state failed; cross-fade disabled for this session");
            }
            D3D11_SAMPLER_DESC sampDesc{};
            sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
            if (FAILED(a_device->CreateSamplerState(&sampDesc, &g_sampler))) {
                logger::warn("[Overlay] sampler failed; cross-fade disabled for this session");
            }
            D3D11_RASTERIZER_DESC rsDesc{};
            rsDesc.FillMode = D3D11_FILL_SOLID;
            rsDesc.CullMode = D3D11_CULL_NONE;
            rsDesc.DepthClipEnable = FALSE;
            rsDesc.ScissorEnable = FALSE;
            if (FAILED(a_device->CreateRasterizerState(&rsDesc, &g_raster))) {
                logger::warn("[Overlay] rasterizer state creation failed; overlay disabled");
                g_failed.store(true);
                return false;
            }
            g_resDevice = a_device;
            logger::info("[Overlay] resources ready (VS {} B, PS {} B, additive blend, no depth)",
                sizeof(g_VoidVS), sizeof(g_VoidPS));
            return true;
        }

        // Everything the draw touches, saved and put back. We deliberately do not touch shader
        // resources / samplers / index buffers, so those need no save. Even if a restore were
        // skipped the game rebinds its own state at the top of every frame -- this is belt and
        // braces for any other Present hook (ENB / Community Shaders) that runs after us.
        struct StateBlock
        {
            ID3D11RenderTargetView*  rtv[8]{};
            ID3D11DepthStencilView*  dsv = nullptr;
            ID3D11BlendState*        blend = nullptr;
            float                    blendFactor[4]{};
            UINT                     sampleMask = 0;
            ID3D11DepthStencilState* depth = nullptr;
            UINT                     stencilRef = 0;
            ID3D11RasterizerState*   raster = nullptr;
            D3D11_VIEWPORT           vp[8]{};
            UINT                     nVp = 0;
            ID3D11InputLayout*       layout = nullptr;
            D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
            ID3D11VertexShader*      vs = nullptr;
            ID3D11PixelShader*       ps = nullptr;
            ID3D11Buffer*            vsCb[2]{};
            ID3D11Buffer*            psCb[2]{};
            ID3D11ShaderResourceView* psSrv0 = nullptr;
            ID3D11SamplerState*       psSamp0 = nullptr;

            void Grab(ID3D11DeviceContext* a_ctx)
            {
                a_ctx->OMGetRenderTargets(8, rtv, &dsv);
                a_ctx->OMGetBlendState(&blend, blendFactor, &sampleMask);
                a_ctx->OMGetDepthStencilState(&depth, &stencilRef);
                a_ctx->RSGetState(&raster);
                nVp = 8;
                a_ctx->RSGetViewports(&nVp, vp);
                a_ctx->IAGetInputLayout(&layout);
                a_ctx->IAGetPrimitiveTopology(&topology);
                a_ctx->VSGetShader(&vs, nullptr, nullptr);
                a_ctx->PSGetShader(&ps, nullptr, nullptr);
                a_ctx->VSGetConstantBuffers(0, 2, vsCb);
                a_ctx->PSGetConstantBuffers(0, 2, psCb);
                a_ctx->PSGetShaderResources(0, 1, &psSrv0);
                a_ctx->PSGetSamplers(0, 1, &psSamp0);
            }

            void Put(ID3D11DeviceContext* a_ctx)
            {
                a_ctx->OMSetRenderTargets(8, rtv, dsv);
                a_ctx->OMSetBlendState(blend, blendFactor, sampleMask);
                a_ctx->OMSetDepthStencilState(depth, stencilRef);
                a_ctx->RSSetState(raster);
                if (nVp) {
                    a_ctx->RSSetViewports(nVp, vp);
                }
                a_ctx->IASetInputLayout(layout);
                if (topology != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) {
                    a_ctx->IASetPrimitiveTopology(topology);
                }
                a_ctx->VSSetShader(vs, nullptr, 0);
                a_ctx->PSSetShader(ps, nullptr, 0);
                a_ctx->VSSetConstantBuffers(0, 2, vsCb);
                a_ctx->PSSetConstantBuffers(0, 2, psCb);
                a_ctx->PSSetShaderResources(0, 1, &psSrv0);
                a_ctx->PSSetSamplers(0, 1, &psSamp0);
                for (auto*& p : rtv) {
                    SafeRelease(p);
                }
                SafeRelease(dsv);
                SafeRelease(blend);
                SafeRelease(depth);
                SafeRelease(raster);
                SafeRelease(layout);
                SafeRelease(vs);
                SafeRelease(ps);
                for (auto*& p : vsCb) {
                    SafeRelease(p);
                }
                for (auto*& p : psCb) {
                    SafeRelease(p);
                }
                SafeRelease(psSrv0);
                SafeRelease(psSamp0);
            }
        };

        // Reads the backbuffer back and returns its average luma (0..255). Staging readback is
        // expensive, so this only runs a handful of frames -- it exists to answer one question:
        // did the overlay actually change the pixels we are about to present?
        float MeanOfBackbuffer(ID3D11Device* a_device, ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_tex)
        {
            if (!a_device || !a_ctx || !a_tex) {
                return -1.0f;
            }
            D3D11_TEXTURE2D_DESC desc{};
            a_tex->GetDesc(&desc);
            D3D11_TEXTURE2D_DESC sd = desc;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            ID3D11Texture2D* staging = nullptr;
            if (FAILED(a_device->CreateTexture2D(&sd, nullptr, &staging)) || !staging) {
                return -1.0f;
            }
            a_ctx->CopyResource(staging, a_tex);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(a_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) {
                staging->Release();
                return -1.0f;
            }
            const auto* px = static_cast<const std::uint8_t*>(mapped.pData);
            std::uint64_t sum = 0;
            std::uint64_t n = 0;
            const bool tenBit = (desc.Format == 24);
            for (unsigned int y = 0; y < desc.Height; y += 4) {
                const auto* row = px + static_cast<std::size_t>(y) * mapped.RowPitch;
                for (unsigned int x = 0; x < desc.Width; x += 4) {
                    if (tenBit) {
                        std::uint32_t v = 0;
                        std::memcpy(&v, row + static_cast<std::size_t>(x) * 4, 4);
                        sum += (((v >> 20) & 0x3FF) + ((v >> 10) & 0x3FF) + (v & 0x3FF)) / 12u;
                    } else {
                        sum += (row[x * 4 + 0] + row[x * 4 + 1] + row[x * 4 + 2]) / 3u;
                    }
                    ++n;
                }
            }
            a_ctx->Unmap(staging, 0);
            staging->Release();
            return n ? static_cast<float>(sum) / static_cast<float>(n) : -1.0f;
        }

        bool Render(void* a_swapChain, Params a_params)
        {
            using namespace RE::BSGraphics;
            if (!a_swapChain) {
                return false;
            }
            auto* sc = static_cast<IDXGISwapChain*>(a_swapChain);
            // v1.8.7: draw into the buffer this chain is about to present, not blindly into 0.
            const std::uint32_t bbIndex = presentfreeze::CurrentBackBufferIndex(sc);
            ID3D11Texture2D* backBuffer = nullptr;
            if (FAILED(sc->GetBuffer(bbIndex, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) || !backBuffer) {
                // v1.10.2: do NOT latch on one failure. A swap chain resize (alt-tab, resolution or
                // fullscreen change) makes GetBuffer fail once, and the old code then disabled the
                // overlay for the rest of the session -- the effect silently died after any alt-tab.
                const auto fails = g_transientFailures.fetch_add(1) + 1;
                if (fails >= 30) {
                    logger::warn("[Overlay] GetBuffer({}) failed {} times in a row; overlay disabled for this session", bbIndex, fails);
                    g_failed.store(true);
                }
                return false;
            }
            g_transientFailures.store(0);
            // v1.3.0: derive the device and the immediate context FROM THE BUFFER WE ARE ABOUT TO
            // DRAW ON, instead of trusting RendererData. On a vanilla install both are the same
            // object, but the test setup runs ReShade (as dxgi.dll) and Community Shaders at once, and
            // "we verifiably changed the back buffer yet the screen never changed" is precisely what
            // a device/chain mismatch looks like. Deriving both from the buffer makes a mismatch
            // impossible by construction. The one-shot [Chain] line below reports whether the two
            // ever disagreed -- if they did, that was the bug all along.
            ID3D11Device* device = nullptr;
            backBuffer->GetDevice(&device);
            ID3D11DeviceContext* ctx = nullptr;
            if (!device) {
                SafeRelease(backBuffer);
                logger::warn("[Overlay] back buffer reports no device; overlay disabled for this session");
                g_failed.store(true);
                return false;
            }
            device->GetImmediateContext(&ctx);
            if (!ctx) {
                device->Release();
                SafeRelease(backBuffer);
                logger::warn("[Overlay] back buffer device has no immediate context; overlay disabled");
                g_failed.store(true);
                return false;
            }
            if (!g_chainLogged.exchange(true)) {
                LogChain(sc, backBuffer, device, ctx);
            }
            if (!EnsureResources(device)) {
                ctx->Release();
                device->Release();
                SafeRelease(backBuffer);
                return false;
            }
            ID3D11RenderTargetView* rtv = nullptr;
            if (FAILED(device->CreateRenderTargetView(backBuffer, nullptr, &rtv)) || !rtv) {
                ctx->Release();
                device->Release();
                SafeRelease(backBuffer);
                logger::warn("[Overlay] CreateRenderTargetView failed; overlay disabled for this session");
                g_failed.store(true);
                return false;
            }
            D3D11_TEXTURE2D_DESC desc{};
            backBuffer->GetDesc(&desc);
            a_params.resolution[0] = static_cast<float>(desc.Width);
            a_params.resolution[1] = static_cast<float>(desc.Height);

            // ---- v1.3.0 probe: one glance, binary answer -------------------------------------
            // Flood the whole target with saturated magenta and present it. No post-processing
            // chain (ReShade, Community Shaders, ENB) can swallow a full-screen primary colour, so
            // this separates "our drawing never reaches the display" from "our drawing is just too
            // subtle to see" with no argument left. If the screen stays unchanged while this runs,
            // every Present-layer idea is dead and only a Fader/Mist takeover is left.
            const int vm = g_cfgVisualMode.load(std::memory_order_relaxed);
            if ((vm == 3 || vm == 6 || vm == 7 || vm == 8) && g_probeLeft.load(std::memory_order_relaxed) > 0) {
                const float magenta[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
                ctx->ClearRenderTargetView(rtv, magenta);
                if (vm == 7 || vm == 8) {
                    // v1.8.8: the index we pick can be wrong, and the log says so -- `authority n/a`
                    // means IDXGISwapChain3 is not reachable on this chain, so the index falls back to
                    // GetLastPresentCount() % BufferCount, and [PA] shows that counter does not advance
                    // synchronously with our call. Flooding EVERY back buffer takes the guess out of
                    // the experiment completely: whichever buffer the chain presents next is magenta.
                    // If the screen stays clean with all of them flooded, buffer selection is not the
                    // answer and slot 8 genuinely cannot deliver our pixels.
                    DXGI_SWAP_CHAIN_DESC scDescAll{};
                    const UINT buffers = SUCCEEDED(sc->GetDesc(&scDescAll)) ? scDescAll.BufferCount : 1u;
                    for (UINT i = 0; i < buffers; ++i) {
                        ID3D11Texture2D* other = nullptr;
                        if (FAILED(sc->GetBuffer(i, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&other))) || !other) {
                            continue;
                        }
                        ID3D11RenderTargetView* ortv = nullptr;
                        if (SUCCEEDED(device->CreateRenderTargetView(other, nullptr, &ortv)) && ortv) {
                            ctx->ClearRenderTargetView(ortv, magenta);
                            SafeRelease(ortv);
                        }
                        SafeRelease(other);
                    }
                    if (!g_probeAllBuffersLogged.exchange(true)) {
                        logger::info("[Probe] mode {}: flooded ALL {} back buffer(s) magenta -- whichever one"
                                     " gets presented is magenta, so `no change on screen` now means the"
                                     " pixels are lost after us, not that we picked the wrong buffer", vm, buffers);
                    }
                }
                if (vm == 6) {
                    // submit this frame through slot 22 so SPS on slot 8 cannot overwrite it
                    presentfreeze::g_bypassToPresent1.store(true);
                }
                // v1.3.1: prove the clear actually landed on the buffer we are about to present.
                // Magenta (1,0,1) scores exactly 170 in this sampler's 10-bit maths, so a read-back
                // near 170 means "the pixels in OUR buffer changed". Anything else means the RTV or
                // format path is broken and no effect of ours was ever drawn in the first place --
                // a completely different problem from "it is drawn but not shown".
                if (!g_probeReadbackDone.exchange(true)) {
                    const float back = presentfreeze::SampleBackbufferLuma(a_swapChain);
                    const bool isMagenta = (back > 150.0f && back < 190.0f);
                    logger::info("[Probe] read-back after clear: luma {:.1f} -> {}", back,
                        isMagenta ? "buffer IS magenta (the clear works; if the screen is not purple, the"
                                    " presented buffer is not the one we cleared, or something downstream"
                                    " overwrites it)"
                                  : "NOT magenta -- the clear did not land on this buffer");
                }
                const auto left = g_probeLeft.fetch_sub(1) - 1;
                if (left == 0) {
                    logger::info("[Probe] magenta flood finished after {} frames ({} mode) -- did the screen turn magenta?",
                        g_cfgProbeFrames.load(), vm == 6 ? "slot-22 submit" : (vm == 7 ? "all-buffer flood, slot 8" : (vm == 8 ? "all-buffer flood + post-present read-back" : "normal submit")));
                    // stop submitting through slot 22 again -- it is a probe, not the normal path
                    presentfreeze::g_bypassToPresent1.store(false);
                    Disarm("probe finished");
                } else if (left % 30 == 0) {
                    logger::info("[Probe] magenta flood: {} frames left", left);
                }
                SafeRelease(rtv);
                ctx->Release();
                device->Release();
                SafeRelease(backBuffer);
                return true;
            }

            // ---- frozen-frame snapshot -------------------------------------------------------
            // ONE texture serves all three jobs: the dead-window push-in, the cut cross-fade, and
            // the "is there actually a picture here?" gate.
            // Sampling is throttled to ~10 Hz because a staging read-back is a full GPU sync, and a
            // copy only happens when the picture really changed -- during the dead window it never
            // changes, so one copy covers the whole window.
            // A frame is only ever promoted to "the snapshot" when its luma says it has a picture in
            // it. Storing a near-black frame is how a cross-fade turns into "it goes black for a
            // moment", which is the exact report that retired the previous attempt.
            const bool haveBlitRes = g_blitPs && g_alphaBlend && g_sampler;
            if (haveBlitRes) {
                if (!g_snapTex || g_snapW != desc.Width || g_snapH != desc.Height || g_snapFmt != desc.Format) {
                    SafeRelease(g_snapSrv);
                    SafeRelease(g_snapTex);
                    D3D11_TEXTURE2D_DESC sd = desc;
                    sd.Usage = D3D11_USAGE_DEFAULT;
                    sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    sd.CPUAccessFlags = 0;
                    sd.MiscFlags = 0;
                    sd.MipLevels = 1;
                    sd.ArraySize = 1;
                    if (SUCCEEDED(device->CreateTexture2D(&sd, nullptr, &g_snapTex)) && g_snapTex &&
                        SUCCEEDED(device->CreateShaderResourceView(g_snapTex, nullptr, &g_snapSrv)) && g_snapSrv) {
                        g_snapW = desc.Width;
                        g_snapH = desc.Height;
                        g_snapFmt = desc.Format;
                        g_snapAt = 0;
                        g_snapValid = false;
                        g_snapSampleAt = 0;
                    } else {
                        SafeRelease(g_snapSrv);
                        SafeRelease(g_snapTex);
                        logger::warn("[Overlay] snapshot texture creation failed; frozen-frame pass disabled");
                    }
                }
                const std::int64_t nowMs = NowMs();
                const std::int64_t deadStart = g_deadWindowAt.load();
                const bool inDead = (deadStart > 0) && !g_cut.load(std::memory_order_relaxed);
                if (g_snapTex && !g_cut.load(std::memory_order_relaxed) && nowMs - g_snapSampleAt >= 100) {
                    g_snapSampleAt = nowMs;
                    const float luma = presentfreeze::SampleBackbufferLuma(a_swapChain);
                    // The luma gate is not an optimisation, it is the safety rule: the frame right
                    // after suppression is near-black (luma 3.8), and storing THAT as "the frozen
                    // picture" is exactly how a cross-fade becomes "it went black for a moment".
                    // A frame below the gate can never replace a good snapshot.
                    if (luma > 6.0f) {
                        // v1.10.6: "the LAST good frame", not "the brightest one".
                        // v1.10.5 kept the brightest frame of the transition, which fixed the dark
                        // frosted glass but broke continuity: the brightest frame can come from the
                        // middle of the door animation, so the frozen picture no longer matched the
                        // animation's final frame and the hand-off visibly jumped.
                        // The rule that gives both: keep replacing while the frame is still within 85%
                        // of the brightest seen so far (i.e. it is still "the picture", not the fade),
                        // and FREEZE the moment it drops below that -- which is the last frame before
                        // the engine started dimming, i.e. exactly the animation's last frame.
                        if (luma > g_snapMaxLuma) {
                            g_snapMaxLuma = luma;
                        }
                        const bool stillThePicture = (luma >= g_snapMaxLuma * 0.85f);
                        if (!g_snapValid || stillThePicture) {
                            ctx->CopyResource(g_snapTex, backBuffer);
                            g_snapLuma = luma;
                            g_snapAt = nowMs;
                        }
                        g_snapValid = true;
                    }
                }
            }

            const bool selfCheck = (g_selfCheckLeft.load() > 0);
            float      meanBefore = -1.0f;
            if (selfCheck) {
                g_selfCheckLeft.fetch_sub(1);
                meanBefore = MeanOfBackbuffer(device, ctx, backBuffer);
            }

            StateBlock saved;
            saved.Grab(ctx);

            D3D11_VIEWPORT vp{};
            vp.Width = static_cast<float>(desc.Width);
            vp.Height = static_cast<float>(desc.Height);
            vp.MaxDepth = 1.0f;
            ctx->RSSetViewports(1, &vp);
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
            ctx->OMSetBlendState(g_blend, nullptr, 0xFFFFFFFFu);
            ctx->OMSetDepthStencilState(nullptr, 0);
            ctx->RSSetState(g_raster);
            ctx->IASetInputLayout(nullptr);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(g_vs, nullptr, 0);
            ctx->VSSetConstantBuffers(0, 1, &g_cb);
            ctx->PSSetConstantBuffers(0, 1, &g_cb);

            const bool wantBlit = (a_params.blitAlpha > 0.0f);
            const bool haveBlit = wantBlit && haveBlitRes && g_snapSrv && g_snapValid;
            if (wantBlit && !haveBlit && !g_fadeResWarned.exchange(true)) {
                logger::warn("[Overlay] frozen-frame pass wanted but not possible (blitPs={} blend={} srv={} sampler={} snapValid={}, zoom={:.3f}) -- drawing the additive layer instead",
                    g_blitPs != nullptr, g_alphaBlend != nullptr, g_snapSrv != nullptr, g_sampler != nullptr,
                    g_snapValid, a_params.zoom);
            }
            if (haveBlit) {
                // v1.3.3: THIS is the frame that carries our pixels, so it must not travel through
                // slot 8. Measured chain:  game -> SPS -> us -> DebugMenu -> real Present, and a
                // full-screen magenta written in front of slot 8 never reaches the screen. The same
                // magenta submitted through slot 22 (game -> us -> sl.interposer -> real Present)
                // does. So only the frames we actually draw on bypass; every other frame stays on
                // the normal chain, and SPS keeps seeing the back buffer the way it expects.
                // v1.8.7: pick the route for THIS frame by phase. See Config::submitRoute.
                const int route = g_cfgSubmitRoute.load(std::memory_order_relaxed);
                bool bypass = false;
                if (route == 2) {
                    bypass = true;                                    // slot 22 on every drawn frame
                } else if (route == 1) {
                    bypass = !g_cut.load(std::memory_order_relaxed);   // static dead window only
                } else {
                    bypass = g_cfgUsePresent1.load(std::memory_order_relaxed);
                }
                if (bypass && presentfreeze::g_suppressPresent.load(std::memory_order_relaxed)) {
                    // The frame is drawn and then thrown away by our own gate below, so the bypass
                    // buys nothing. Log it once per arm: otherwise "the pixels never appeared" gets
                    // blamed on the present chain when the chain was never even asked.
                    if (!g_bypassBlockedBySuppress.exchange(true)) {
                        logger::warn("[Overlay] bypass requested, but present suppression is ON -- this frame is"
                                     " drawn and then dropped by our own gate, not by the chain below us");
                    }
                }
                presentfreeze::g_bypassToPresent1.store(bypass);
                ctx->OMSetBlendState(g_alphaBlend, nullptr, 0xFFFFFFFFu);
                ctx->PSSetShader(g_blitPs, nullptr, 0);
                ctx->PSSetShaderResources(0, 1, &g_snapSrv);
                ctx->PSSetSamplers(0, 1, &g_sampler);
            } else {
                presentfreeze::g_bypassToPresent1.store(false);
                ctx->PSSetShader(g_ps, nullptr, 0);
            }

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                std::memcpy(mapped.pData, &a_params, sizeof(a_params));
                ctx->Unmap(g_cb, 0);
                ctx->Draw(3, 0);
            }

            // v1.10.8: route 3 presents the same frame twice (slot 8 for the upscaler's cadence, then
            // slot 22 so ours lands). But FLIP_DISCARD rotates the back buffer on every Present, so the
            // second Present flips the OTHER buffer -- which never received our drawing. Result on the
            // first attempt: the flash went away (the upscaler was fed) and the effect went away too
            // (our pixels were replaced by the untouched buffer). Fix: after drawing, mirror this exact
            // frame into every other back buffer, so whichever Present lands last carries it.
            if (selfCheck) {
                const float meanAfter = MeanOfBackbuffer(device, ctx, backBuffer);
                logger::info("[Overlay] self-check: backbuffer mean {:.2f} -> {:.2f} (delta {:+.2f}); {}",
                    meanBefore, meanAfter, meanAfter - meanBefore,
                    haveBlit ? "frozen-frame pass" : "additive pass");
            }

            saved.Put(ctx);
            SafeRelease(rtv);
            ctx->Release();
            device->Release();
            SafeRelease(backBuffer);
            return true;
        }

        void Arm()
        {
            // v1.3.0: iVisualMode is the single authority. bVoidOverlay used to gate the whole
            // overlay and defaults to 0, which is why every visual experiment since v1.4 was silently
            // doing nothing unless two switches were flipped. Now: iVisualMode != 0 arms for real.
            if (g_config.visualMode == 0 || g_failed.load()) {
                return;
            }
            g_cfgVisualMode.store(g_config.visualMode);
            g_cfgDeadZoomAmount.store(g_config.deadZoomAmount);
            g_cfgDeadBlurPx.store(g_config.deadBlurPx);
            g_cfgDeadZoomMs.store(g_config.deadZoomMs);
            g_cfgProbeFrames.store(g_config.probeFrames);
            g_cfgUsePresent1.store(g_config.usePresent1ForVisual);
            g_cfgSubmitRoute.store(g_config.submitRoute);
            g_cfgPresentAudit.store(g_config.presentAudit);
            g_probeLeft.store((g_config.visualMode == 3 || g_config.visualMode == 6 ||
                               g_config.visualMode == 7 || g_config.visualMode == 8)
                                  ? static_cast<int>(g_config.probeFrames)
                                  : 0);
            g_postProbeLeft.store(g_config.visualMode == 4 ? static_cast<int>(g_config.probeFrames) : 0);
            presentfreeze::g_bypassToPresent1.store(false);
            g_probeReadbackDone.store(false);
            g_probeAllBuffersLogged.store(false);
            g_survivalLeft.store(g_config.visualMode == 8 ? g_config.probeFrames : 0);
            g_survivalTick.store(0);
            g_bypassBlockedBySuppress.store(false);
            g_presentHrLogged.store(false);
            g_lastPresentHr.store(0);
            g_cfgBright.store(g_config.overlayBrightness);
            g_cfgBreath.store(g_config.overlayBreath);
            g_cfgPulse.store(g_config.overlayPulse);
            g_cfgMoteScale.store(g_config.overlayMoteScale);
            g_cfgMotes.store(g_config.overlayMotes);
            g_cfgPulseMs.store(g_config.overlayPulseMs);
            g_cfgCrossFade.store(g_config.crossFade);
            g_cfgCrossFadeMs.store(g_config.crossFadeMs);
            g_cfgCrossFadeHoldMs.store(g_config.crossFadeHoldMs);
            g_cfgDebugFull.store(g_config.overlayDebugFullscreen);
            g_snapAt = 0;   // force a fresh snapshot for this transition
            g_snapValid = false;
            g_snapLuma = -1.0f;
            g_snapMaxLuma = -1.0f;
            g_snapSampleAt = 0;
            g_fadeLogged.store(false);
            g_selfCheckLeft.store(5);   // 前 5 帧做读回自检
            g_cfgMaxMs.store(g_config.overlayMaxMs);
            g_armAt.store(NowMs());
            g_cutAt.store(0);
            g_cutBaseLuma.store(-1.0f);
            g_dissolveAt.store(0);
            g_lastCutSample.store(0);
            g_prevCutLuma.store(-1.0f);
            g_cut.store(false);
            g_draws.store(0);
            g_lastDrawAt.store(0);
            g_presentSeen.store(0);
            // The overlay lives in the Present hook, so install it here -- otherwise it would only
            // exist when freeze-frame or the swap cover happened to install it first.
            if (auto* sc = presentfreeze::CurrentSwapChain()) {
                presentfreeze::InstallOn(sc);
            } else {
                logger::warn("[Overlay] no swap chain yet; the first transition will go without the overlay");
            }
            if (!g_armed.exchange(true)) {
                logger::info("[Overlay] armed mode {}: push-in {:.1f}% over {} ms, cut cross-fade {} ms, probe {} frames",
                    g_config.visualMode, g_config.deadZoomAmount * 100.0f, g_config.deadZoomMs,
                    g_config.crossFadeMs, g_config.probeFrames);
                logger::info("[Overlay] submit route {} ({}) / present audit {}",
                    g_config.submitRoute,
                    g_config.submitRoute == 1 ? "slot 22 while the dead window is static, slot 8 from the cut on"
                    : g_config.submitRoute == 2 ? "slot 22 on every drawn frame -- v1.8.4 route, expected to freeze at the cut"
                                                : "slot 8 only (bUsePresent1ForVisual decides)",
                    g_config.presentAudit ? 1 : 0);
            }
        }

        // Fired the moment the new cell becomes the player's parent cell -- that is exactly when the
        // engine starts the ~350 ms scene spin. From here the motes fade out over the pulse.
        void Cut()
        {
            if (!g_armed.load()) {
                return;
            }
            const std::int64_t now = NowMs();
            g_cutAt.store(now);
            g_cutBaseLuma.store(-1.0f);
            g_dissolveAt.store(0);
            g_lastCutSample.store(0);
            g_prevCutLuma.store(-1.0f);
            g_waitCapLogged.store(false);
            g_cut.store(true);
            if (g_config.cancelFadeBackAfterCut) {
                // v1.10.4 experiment: after the swap, cancel ALL three fade forms -- including
                // FadeToBlackBackImod, which we normally leave alone because cancelling it mid-transition
                // restarts the engine's fade-in every 25 ms and strobes. After the cut the trade-off may
                // be the other way round: if the still picture is a black plane held at partial strength,
                // this removes it. Bounded to 1.5 s, and posted to the main thread so the render thread
                // never touches engine state.
                std::thread([]() {
                    for (int i = 0; i < 60; ++i) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(25));
                        if (auto* task = SKSE::GetTaskInterface()) {
                            task->AddTask([]() {
                                CacheForms();
                                if (g_fadeImod) {
                                    RE::ImageSpaceModifierInstanceForm::Stop(g_fadeImod);
                                }
                                if (g_fadeHoldImod) {
                                    RE::ImageSpaceModifierInstanceForm::Stop(g_fadeHoldImod);
                                }
                                if (g_fadeBackImod) {
                                    RE::ImageSpaceModifierInstanceForm::Stop(g_fadeBackImod);
                                }
                            });
                        }
                    }
                }).detach();
                logger::info("[NoBlack] bCancelFadeBackAfterCut=1: cancelling ALL fade records for 1.5 s after the cut");
            }
            logger::info("[Overlay] cut after {} ms: {} starting ({} frames drawn, {} Present calls in the dead window, snapshot valid={})",
                now - g_armAt.load(),
                g_cfgVisualMode.load() == 0 ? "nothing" : "cross-fade",
                g_draws.load(), g_presentSeen.load(), g_snapValid);
            // v1.10.3: the blur ramps over uDeadZoomMs but the dead window is only as long as the
            // transition -- set the ramp too long and the blur never reaches full strength, which
            // looks like "the frosted glass is barely there" with no obvious cause.
            const auto rampMs = g_cfgDeadZoomMs.load(std::memory_order_relaxed);
            const auto deadMs = now - g_deadWindowAt.load();
            if (g_deadWindowAt.load() != 0 && rampMs > 0 && deadMs < static_cast<std::int64_t>(rampMs)) {
                logger::warn("[Overlay] the blur ramp is {} ms but the dead window was only {} ms -- it reached"
                             " {}% of fDeadBlurPx. Lower uDeadZoomMs for the full effect.",
                    rampMs, deadMs,
                    static_cast<int>(100 * deadMs / static_cast<std::int64_t>(rampMs)));
            }
        }

        void Disarm(const char* a_reason)
        {
            if (!g_armed.exchange(false)) {
                return;
            }
            // v1.8.7: the submit route must never outlive the overlay. This flag is only safe inside
            // the static dead window; left as `true` it would send every ordinary gameplay frame
            // around slot 8 for the rest of the session, straight into the frame-generation path
            // that wedged the chain in v1.8.4. Disarm is reached on every exit path -- hard cap,
            // failed transition, finished cross-fade -- so this is the one place that always runs.
            presentfreeze::g_bypassToPresent1.store(false);
            g_bypassBlockedBySuppress.store(false);
            const std::int64_t now = NowMs();
            logger::info("[Overlay] off after {} ms, {} frames drawn, {} Present calls seen ({})",
                now - g_armAt.load(), g_draws.load(), g_presentSeen.load(), a_reason ? a_reason : "?");
        }

        bool Active()
        {
            return g_armed.load();
        }

        // Called on every Present / Present1 so the log can state whether the game was presenting
        // at all during the dead window.
        void NotePresent()
        {
            if (g_armed.load(std::memory_order_relaxed)) {
                g_presentSeen.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // v1.3.1: records the return value of the REAL Present, once per transition.
        // Returns true only for the first call after arming, so the caller logs exactly one line.
        bool NotePresentResult(long a_hr)
        {
            if (!g_armed.load(std::memory_order_relaxed) || g_presentHrLogged.exchange(true)) {
                return false;
            }
            g_lastPresentHr.store(a_hr);
            return true;
        }

        // ---- v1.3.1 mode 4: the same flood, but AFTER the real Present --------------------------
        // This is the experiment that tells the two remaining explanations apart:
        //   * if a flood drawn BEFORE Present never shows, while the same flood drawn AFTER Present
        //     and presented again DOES show, then something between us and the scan-out
        //     (ReShade / Community Shaders / the engine's own late pass) rewrites the buffer and
        //     discards our pixels. The fix is then "draw where they cannot overwrite", not
        //     "give up on Present".
        //   * if neither shows, the buffer we hold is not the one being scanned out at all.
        // Cost: one extra Present per frame for the probe window only; it is a diagnostic, never a
        // shipped effect.
        bool WantsPostPresentProbe()
        {
            return g_postProbeLeft.load(std::memory_order_relaxed) > 0;
        }

        // ---- v1.3.2 mode 6: submit through IDXGISwapChain1::Present1 (slot 22) -------------------
        // `[Cap] chain below us … in SPS.dll` proved that Skyrim Performance Solution sits downstream
        // of us on slot 8: it re-renders the whole frame from its own cached state before calling the
        // real Present, so anything we draw is discarded no matter where in slot 8 we draw.
        // SPS needs slot 8 because it consumes the previous frame; it has no reason to touch slot 22.
        // If slot 22 is still the untouched DXGI entry point, submitting through it puts our pixels on
        // screen AFTER SPS has done its pass.
        bool BypassToPresent1()
        {
            return presentfreeze::g_bypassToPresent1.load(std::memory_order_relaxed);
        }


        void PostPresentProbe(void* a_swapChain)
        {
            if (!a_swapChain || g_postProbeLeft.load(std::memory_order_relaxed) <= 0) {
                return;
            }
            auto* sc = static_cast<IDXGISwapChain*>(a_swapChain);
            ID3D11Texture2D* back = nullptr;
            if (FAILED(sc->GetBuffer(presentfreeze::CurrentBackBufferIndex(sc), __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) {
                return;
            }
            ID3D11Device* device = nullptr;
            back->GetDevice(&device);
            ID3D11DeviceContext* ctx = nullptr;
            if (device) {
                device->GetImmediateContext(&ctx);
            }
            if (device && ctx) {
                ID3D11RenderTargetView* rtv = nullptr;
                if (SUCCEEDED(device->CreateRenderTargetView(back, nullptr, &rtv)) && rtv) {
                    const float magenta[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
                    ctx->ClearRenderTargetView(rtv, magenta);
                    rtv->Release();
                    const auto left = g_postProbeLeft.fetch_sub(1) - 1;
                    if (left % 30 == 0 || left == 0) {
                        logger::info("[Probe] post-Present flood: {} frames left (this one is presented after the"
                                     " real Present -- if only THIS is visible, something downstream rewrites the"
                                     " buffer)", left);
                    }
                }
            }
            if (ctx) {
                ctx->Release();
            }
            if (device) {
                device->Release();
            }
            back->Release();
        }

        // Called from the Present hook on the render thread.
        void OnPresent(void* a_swapChain)
        {
            if (!a_swapChain || !g_armed.load(std::memory_order_relaxed) || g_failed.load(std::memory_order_relaxed)) {
                // v1.8.7 belt-and-braces: while the overlay is not drawing, the submit route must be
                // slot 8. One load per frame is nothing next to holding the whole render thread.
                presentfreeze::g_bypassToPresent1.store(false);
                return;
            }
            const std::int64_t now = NowMs();
            // Present and Present1 can both be routed here; draw at most once per frame.
            if (now - g_lastDrawAt.load() < 2) {
                return;
            }
            const std::int64_t armAt = g_armAt.load();
            if (now - armAt > static_cast<std::int64_t>(g_cfgMaxMs.load())) {
                g_lastDrawAt.store(now);
                Disarm("hard cap reached -- the transition never reported back");
                return;
            }

            Params p{};
            p.age = static_cast<float>(now - armAt) / 1000.0f;
            const int           mode = g_cfgVisualMode.load(std::memory_order_relaxed);
            const std::uint32_t fadeMs = g_cfgCrossFadeMs.load();
            const float         zoomAmount = g_cfgDeadZoomAmount.load();
            const std::uint32_t zoomMs = g_cfgDeadZoomMs.load();
            const float         blurMaxPx = g_cfgDeadBlurPx.load();
            p.zoom = 1.0f;
            p.blurPx = 0.0f;

            if (g_cut.load(std::memory_order_relaxed)) {
                // ---- after the cut: the old frame dissolves into the finished cell --------------
                const std::int64_t cutAt = g_cutAt.load();
                const std::int64_t cutAge = now - cutAt;
                const std::uint32_t holdCapMs = g_cfgCrossFadeHoldMs.load(std::memory_order_relaxed);
                // v1.10.3: the dissolve no longer starts on a stopwatch -- it starts when the engine
                // actually unveils the new picture (or when holdCapMs runs out, whichever is first).
                // See the g_cutBaseLuma comment above for the measurement that forced this.
                {
                    const std::int64_t dStart = g_dissolveAt.load();
                    if (dStart == 0) {
                        if (cutAge >= 60) {   // let the swap itself settle before taking a baseline
                            const float luma = presentfreeze::SampleBufferLumaAt(a_swapChain, 0xFFFFFFFFu);
                            if (luma >= 0.0f) {
                                g_cutBaseLuma.store(luma);
                            }
                            // Provisional start = the cap, so the worst case is the old behaviour.
                            g_dissolveAt.store(cutAt + static_cast<std::int64_t>(holdCapMs));
                        }
                    } else if (now - g_lastCutSample.load() >= 50) {
                        g_lastCutSample.store(now);
                        const float     luma = presentfreeze::SampleBufferLumaAt(a_swapChain, 0xFFFFFFFFu);
                        const float     base = g_cutBaseLuma.load();
                        // v1.10.4: print the engine's own brightness through the whole hand-off. This is
                        // the line that tells "a black plane is still on the screen" apart from "the new
                        // cell really is that dark": a constant value = a static cover, a ramp = a fade.
                        logger::info("[Cut] +{} ms engineLuma={:.1f} (baseline {:.1f}) alpha={:.2f}", cutAge,
                            luma, base, (fadeMs > 0 && dStart != 0)
                                ? std::clamp(1.0f - static_cast<float>(now - dStart) / static_cast<float>(fadeMs), 0.0f, 1.0f)
                                : 1.0f);
                        // v1.10.5: the engine keeps presenting a BLACK frame for a while after the
                        // swap (measured luma 2.0 for >2 s on this setup), and the real picture only
                        // arrives later -- as a step. Dissolving on a 1.5 s stopwatch therefore faded
                        // our picture into black, and the real frame then landed with nothing covering
                        // it: the "猛亮一下". So start only when the engine's frame is BOTH not black
                        // (the picture exists) and STABLE (no longer changing between samples), which
                        // also rides out the first over-bright frames.
                        // v1.11.0: start the dissolve as soon as the engine's picture EXISTS (not black).
                        // The old "and settled" test compared two samples 50 ms apart, which a moving
                        // picture passes trivially -- it released the moment it first ran, so the wait
                        // value could not be tuned at all. Non-black alone is the property we care about:
                        // it is what stops the dissolve from fading into the dark frame the engine shows
                        // right after the swap.
                        const float prevLuma = g_prevCutLuma.exchange(luma);
                        if (cutAge < static_cast<std::int64_t>(holdCapMs) && luma > 12.0f) {
                            g_dissolveAt.store(now);
                            logger::info("[Overlay] cut: the engine's picture is back at +{} ms"
                                         " (luma {:.1f}, was {:.1f}); dissolving over {} ms",
                                cutAge, luma, prevLuma, fadeMs);
                        }
                    }
                    const std::int64_t dNow = g_dissolveAt.load();
                    if (dNow != 0 && dNow == cutAt + static_cast<std::int64_t>(holdCapMs) &&
                        cutAge >= static_cast<std::int64_t>(holdCapMs) && g_prevCutLuma.load() <= 12.0f &&
                        !g_waitCapLogged.exchange(true)) {
                        logger::warn("[Overlay] the engine was still black after the {} ms wait -- dissolving anyway"
                                     " (raise uCrossFadeHoldMs if the flash persists)", holdCapMs);
                    }
                    if (dNow != 0 && now - dNow > static_cast<std::int64_t>(fadeMs)) {
                        g_lastDrawAt.store(now);
                        Disarm("cut cross-fade finished");
                        return;
                    }
                }
                p.phase = 1.0f;
                p.phaseAge = static_cast<float>(cutAge) / 1000.0f;
                // Hold the fully blurred picture across the dissolve: the hand-off then reads as
                // frosted glass clearing up, rather than a still image sliding away.
                p.blurPx = blurMaxPx;
                // v1.9.0: carry the pushed-in scale ACROSS the cut. Leaving p.zoom at its default 1.000
                // snapped the picture back a whole 3% on the exact frame of the swap, which is what made
                // the whole transition read as a flash instead of a transition. Use the scale the dead
                // window actually reached, so a short dead window does not pop inward either.
                {
                    float zoomAtCut = 1.0f;
                    // Only mode 1 pushes the picture in, so only mode 1 has a scale to carry across.
                    // In mode 2 the snapshot must appear pixel exact or it would pop outward here.
                    const std::int64_t deadStartForCut = (mode == 1) ? g_deadWindowAt.load() : 0;
                    if (deadStartForCut > 0) {
                        const std::int64_t deadAgeAtCut = cutAt - deadStartForCut;
                        const float td = (zoomMs > 0)
                                             ? std::clamp(static_cast<float>(deadAgeAtCut) / static_cast<float>(zoomMs), 0.0f, 1.0f)
                                             : 1.0f;
                        zoomAtCut = 1.0f + zoomAmount * td;
                    }
                    p.zoom = zoomAtCut;
                }
                if (mode == 1 || mode == 2) {
                    // v1.10.3: uCrossFadeMs now means exactly what the menu label says -- the length of
                    // the dissolve. It used to be the TOTAL, with the hold subtracted from it, so setting
                    // 800 with a 400 hold still only produced a 400 ms dissolve and the UI was lying.
                    const std::int64_t dStart = g_dissolveAt.load();
                    float tc = 0.0f;
                    if (dStart != 0 && fadeMs > 0) {
                        tc = std::clamp(static_cast<float>(now - dStart) / static_cast<float>(fadeMs), 0.0f, 1.0f);
                    } else if (dStart != 0) {
                        tc = 1.0f;
                    }
                    const float eased = tc * tc * (3.0f - 2.0f * tc);   // smoothstep
                    p.blitAlpha = 1.0f - eased;
                }
                p.pulseStrength = 0.0f;
            } else if (mode == 1) {
                // ---- dead window: push the frozen picture in, so it stops being a still image ---
                // Only once the loading menu has actually been dropped. Before that the engine is
                // still drawing the door swing, and blitting a snapshot over it would freeze the
                // door animation -- so blitAlpha stays 0 and we touch nothing at all.
                const std::int64_t deadStart = g_deadWindowAt.load();
                if (deadStart > 0) {
                    const std::int64_t deadAge = now - deadStart;
                    const float t = (zoomMs > 0)
                                        ? std::clamp(static_cast<float>(deadAge) / static_cast<float>(zoomMs), 0.0f, 1.0f)
                                        : 1.0f;
                    p.zoom = 1.0f + zoomAmount * t;
                    p.blurPx = blurMaxPx * t;
                    // alpha 1: the snapshot replaces the frozen frame outright. That is the whole
                    // point -- resampling it every frame is what produces the motion.
                    p.blitAlpha = 1.0f;
                }
            }
            p.bright = g_cfgBright.load();
            // v1.3.0: modes 1 and 2 are the frozen-frame blit and NOTHING else. Breathing / motes /
            // pulse are the retired additive route -- leaving them at their old defaults would put a
            // faint additive lift on every frame we do not blit (the whole door swing, for one),
            // which both muddies the answer and is something the user never asked for.
            const bool blitOnly = (mode == 1 || mode == 2 || mode == 4);
            p.breath = blitOnly ? 0.0f : g_cfgBreath.load();
            p.pulseStrength = blitOnly ? 0.0f : g_cfgPulse.load();
            p.motes = blitOnly ? 0.0f : static_cast<float>(g_cfgMotes.load());
            p.pulseMs = static_cast<float>(fadeMs) / 1000.0f;
            p.moteScale = g_cfgMoteScale.load();
            if (g_cfgDebugFull.load()) {
                // A/B switch for the additive route: lift the WHOLE screen instead of blitting the
                // snapshot. Kept because it is the cheapest way to answer "does a plain full-screen
                // additive write reach the display?" without changing anything else.
                p.blitAlpha = 0.0f;
                p.breath = 0.15f;
                p.motes = 0.0f;
                p.pulseStrength = 0.0f;
            }

            g_lastDrawAt.store(now);
            const bool firstBlitFrame = (p.blitAlpha > 0.0f) && !g_fadeLogged.exchange(true);
            if (!Render(a_swapChain, p)) {
                return;
            }
            if (firstBlitFrame) {
                logger::info("[Overlay] frozen-frame pass frame 1: snapshot age {} ms, alpha {:.2f}, zoom {:.3f}",
                    g_snapAt ? (now - g_snapAt) : -1, p.blitAlpha, p.zoom);
            }
            if (g_draws.fetch_add(1) == 0) {
                logger::info("[Overlay] first frame drawn {:.0f} ms after arming", p.age * 1000.0f);
            }
        }
    }

    void BeginFreezeFrame(const char* a_reason)
    {
        if (!g_config.freezeFramePresent) {
            return;
        }
        auto* sc = presentfreeze::CurrentSwapChain();
        if (!sc) {
            logger::warn("[B] no swap chain yet; freeze-frame skipped ({})", a_reason);
            return;
        }
        if (!presentfreeze::InstallOn(sc)) {
            return;
        }
        presentfreeze::g_skippedFrames.store(0);
        presentfreeze::g_firstSkipLogged.store(false);
        presentfreeze::g_suppressStartAt.store(NowMs());
        presentfreeze::g_suppressPresent.store(true);
        logger::info("[B] freeze-frame ON ({}): presentation will stay on the last frame", a_reason);
        // Failsafe. Everything else may go wrong (the transition may never report back, the player
        // object may be null, a save may be loading), but the screen must never stay frozen forever.
        const auto activeWin = g_suppressWindowMs.load();
        const auto winMs = activeWin > 0 ? activeWin : g_config.transitionWindowMs;
        const auto capMs = static_cast<std::int64_t>(winMs) + 5000;
        std::thread([capMs]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(capMs));
            if (g_shuttingDown.load(std::memory_order_acquire) || !presentfreeze::g_suppressPresent.load()) {
                return;
            }
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([]() {
                    if (g_shuttingDown.load(std::memory_order_acquire)) {
                        return;
                    }
                    if (presentfreeze::g_suppressPresent.exchange(false)) {
                        logger::warn("[B] freeze-frame force-disabled by the failsafe -- the transition never reported back");
                    }
                });
            }
        }).detach();
    }

    // ---- Void effect (v1.2.3) ----------------------------------------------------------------
    // The ~124 frames between "hand touches the door" and the spin are dead: the old cell is
    // unloading, the screen is one static frame. This ramps an image-space darkening across that
    // window in a handful of discrete steps (not every tick -- re-triggering a fade record at 40 Hz
    // is what used to strobe), so the wait reads as a deliberate fade instead of a frozen picture.
    void VoidRampStep()
    {
        if (g_config.voidDarken <= 0.01f) {
            return;
        }
        const auto elapsed = NowMs() - g_suppressStarted.load();
        const auto ramp = static_cast<std::int64_t>(g_config.voidRampMs);
        const float t = (ramp <= 0) ? 1.0f
                                    : std::clamp(static_cast<float>(elapsed) / static_cast<float>(ramp), 0.0f, 1.0f);
        const int step = static_cast<int>(t * 8.0f);   // 9 discrete levels
        if (step == g_voidStep.load()) {
            return;
        }
        g_voidStep.store(step);
        CacheForms();
        if (!g_fadeHoldImod) {
            return;
        }
        const float strength = g_config.voidDarken * (static_cast<float>(step) / 8.0f);
        RE::ImageSpaceModifierInstanceForm::Trigger(g_fadeHoldImod, strength, nullptr);
    }

    // At the swap: take the (already dimmed) picture to full black so the spin happens behind it,
    // then hand back to the engine's fade-in. Reuses the holding record, which is the only one that
    // sets a level directly instead of animating.
    void VoidFadeOutAtSwap(std::uint32_t a_holdMs)
    {
        CacheForms();
        if (g_fadeHoldImod) {
            RE::ImageSpaceModifierInstanceForm::Trigger(g_fadeHoldImod, 1.0f, nullptr);
        }
        logger::info("[Void] full black at the swap, holding {} ms before fading into the new cell", a_holdMs);
        std::thread([a_holdMs]() {
            if (a_holdMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(a_holdMs));
            }
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([]() {
                    CacheForms();
                    if (g_fadeHoldImod) {
                        RE::ImageSpaceModifierInstanceForm::Stop(g_fadeHoldImod);
                    }
                    if (g_fadeImod) {
                        RE::ImageSpaceModifierInstanceForm::Stop(g_fadeImod);
                    }
                    if (g_fadeBackImod) {
                        RE::ImageSpaceModifierInstanceForm::Trigger(g_fadeBackImod, 1.0f, nullptr);
                    }
                    logger::info("[Void] released; fading into the new cell");
                });
            }
        }).detach();
    }

    // ---- World probe (v1.3.0, READ-ONLY) -----------------------------------------------------
    // Answers ONE question: during the ~124 dead frames, is the world paused or just not visible?
    //   game time advancing  -> the world is still simulating; "keep it alive" becomes thinkable
    //   game time frozen     -> the engine pauses the transition; covering is the only option
    // It also records when the old cell's 3D root disappears and whether the music track changes.
    struct WorldProbeState
    {
        bool          started = false;
        std::int64_t  startAt = 0;
        float         startHours = 0.0f;
        std::uintptr_t startCell3D = 0;
        std::uint32_t startChildren = 0;
        std::uintptr_t startMusic = 0;
        std::int64_t  cell3DLostAt = -1;
        std::int64_t  musicChangedAt = -1;
        std::uint32_t samples = 0;
        // ---- v1.4.2: "who is competing for the CPU during the window?" -------------------------
        std::uint64_t startFrames = 0;          // present calls seen at the moment we arm
        bool          startFreezeTime = false;  // Main::freezeTime -- the engine's own time freeze
        std::uint32_t highActors = 0;           // what could be throttled IF the world is running
        std::uint32_t lowActors = 0;
        bool          hookInstalled = false;    // without the Present hook the frame rate is unknown
        static constexpr int kTimeline = 16;    // one point every 8th 25 ms tick, i.e. ~200 ms
        std::int64_t  tlAt[kTimeline]{};
        std::uint64_t tlFrames[kTimeline]{};
        float         tlHours[kTimeline]{};
        std::uint8_t  tlHas3D[kTimeline]{};
        int           tlCount = 0;
    };
    WorldProbeState g_worldProbe;
    std::uintptr_t g_probeWorldLastCell3D = 0;
    std::uintptr_t g_probeWorldLastMusic = 0;

    void WorldProbeBegin()
    {
        g_worldProbe = WorldProbeState{};
        if (!g_config.probeWorld) {
            return;
        }
        auto* cal = RE::Calendar::GetSingleton();
        auto* pl = RE::PlayerCharacter::GetSingleton();
        auto* cell = pl ? pl->GetParentCell() : nullptr;
        auto* cell3d = (cell && cell->GetRuntimeData().loadedData)
            ? cell->GetRuntimeData().loadedData->cell3D.get()
            : nullptr;
        g_worldProbe.started = true;
        g_worldProbe.startAt = NowMs();
        g_worldProbe.startHours = cal ? cal->GetHoursPassed() : 0.0f;
        g_worldProbe.startCell3D = reinterpret_cast<std::uintptr_t>(cell3d);
        g_worldProbe.startChildren = (cell3d && cell3d->GetChildren().size() > 0)
            ? static_cast<std::uint32_t>(cell3d->GetChildren().size())
            : 0;
        if (auto* mus = RE::BSMusicManager::GetSingleton()) {
            g_worldProbe.startMusic = reinterpret_cast<std::uintptr_t>(mus->current);
        }
        logger::info("[World] void start: hoursPassed={:.6f} cell3D=0x{:X} children={} music=0x{:X}",
            g_worldProbe.startHours, g_worldProbe.startCell3D, g_worldProbe.startChildren, g_worldProbe.startMusic);
        // v1.4.2 CPU-contention baseline: three cheap reads that decide whether throttling anything
        // during the window could help at all.
        g_worldProbe.startFrames = presentfreeze::g_frameCount.load();
        g_worldProbe.hookInstalled = presentfreeze::g_installed.load();
        if (auto* main = RE::Main::GetSingleton()) {
            g_worldProbe.startFreezeTime = main->freezeTime;
        }
        if (auto* lists = RE::ProcessLists::GetSingleton()) {
            g_worldProbe.highActors = static_cast<std::uint32_t>(lists->highActorHandles.size());
            g_worldProbe.lowActors = static_cast<std::uint32_t>(lists->lowActorHandles.size());
        }
        logger::info("[World] cpu baseline: freezeTime={} presentHook={} frames={} highActors={} lowActors={}",
            g_worldProbe.startFreezeTime ? "ON" : "off",
            g_worldProbe.hookInstalled ? "installed" : "NOT installed (frame rate unknown)",
            g_worldProbe.startFrames, g_worldProbe.highActors, g_worldProbe.lowActors);
    }

    void WorldProbeStep()
    {
        if (!g_worldProbe.started) {
            return;
        }
        ++g_worldProbe.samples;
        const auto elapsed = NowMs() - g_worldProbe.startAt;
        auto* pl = RE::PlayerCharacter::GetSingleton();
        auto* cell = pl ? pl->GetParentCell() : nullptr;
        auto* cell3d = (cell && cell->GetRuntimeData().loadedData)
            ? cell->GetRuntimeData().loadedData->cell3D.get()
            : nullptr;
        const auto cell3dAddr = reinterpret_cast<std::uintptr_t>(cell3d);
        if (g_probeWorldLastCell3D != cell3dAddr) {
            logger::info("[World] +{} ms cell3D changed: 0x{:X} -> 0x{:X} (children {})",
                elapsed, g_probeWorldLastCell3D, cell3dAddr,
                cell3d ? cell3d->GetChildren().size() : 0);
            g_probeWorldLastCell3D = cell3dAddr;
            if (g_worldProbe.cell3DLostAt < 0 && cell3dAddr == 0) {
                g_worldProbe.cell3DLostAt = elapsed;
            }
        }
        if (auto* mus = RE::BSMusicManager::GetSingleton()) {
            const auto musicAddr = reinterpret_cast<std::uintptr_t>(mus->current);
            if (g_probeWorldLastMusic != musicAddr) {
                logger::info("[World] +{} ms music track changed: 0x{:X} -> 0x{:X}", elapsed, g_probeWorldLastMusic, musicAddr);
                g_probeWorldLastMusic = musicAddr;
                if (g_worldProbe.musicChangedAt < 0) {
                    g_worldProbe.musicChangedAt = elapsed;
                }
            }
        }
        // v1.4.2 timeline: one point every 8th tick (~200 ms). This is what shows WHEN the window
        // actually elapses, whether the frame loop keeps running through it, and whether game time
        // advances -- i.e. whether the wait is CPU-busy or CPU-blocked.
        if ((g_worldProbe.samples % 8) == 0 && g_worldProbe.tlCount < WorldProbeState::kTimeline) {
            const int i = g_worldProbe.tlCount++;
            g_worldProbe.tlAt[i] = elapsed;
            g_worldProbe.tlFrames[i] = presentfreeze::g_frameCount.load();
            auto* calStep = RE::Calendar::GetSingleton();
            g_worldProbe.tlHours[i] = calStep ? calStep->GetHoursPassed() : 0.0f;
            g_worldProbe.tlHas3D[i] = (cell3dAddr != 0) ? 1 : 0;
        }
    }

    void WorldProbeFinish()
    {
        if (!g_worldProbe.started) {
            return;
        }
        g_worldProbe.started = false;
        auto* cal = RE::Calendar::GetSingleton();
        const auto endHours = cal ? cal->GetHoursPassed() : 0.0f;
        const auto delta = endHours - g_worldProbe.startHours;
        const auto elapsed = NowMs() - g_worldProbe.startAt;
        logger::info("[World] void end after {} ms ({} samples): hoursPassed {:.6f} -> {:.6f} (delta {:+.6f}) => world is {}",
            elapsed, g_worldProbe.samples, g_worldProbe.startHours, endHours, delta,
            delta > 0.0005f ? "RUNNING (not paused)" : "PAUSED (time frozen)");
        logger::info("[World] cell3D lost at {} ; music changed at {}",
            g_worldProbe.cell3DLostAt, g_worldProbe.musicChangedAt);

        // ---- v1.4.2: "who is competing for the CPU?" ------------------------------------------
        // fps: the frame loop's actual rate over the window. ~50-60 means the loop is spinning
        // freely (so the time is going into per-frame work, and throttling simulation could help);
        // a low number means the main thread is blocked inside the load path (nothing to throttle).
        const auto endFrames = presentfreeze::g_frameCount.load();
        const std::uint64_t frames = (presentfreeze::g_installed.load() && endFrames >= g_worldProbe.startFrames)
            ? (endFrames - g_worldProbe.startFrames)
            : 0u;
        const double fps = (elapsed > 0 && frames > 0)
            ? (1000.0 * static_cast<double>(frames) / static_cast<double>(elapsed))
            : 0.0;
        bool endFreezeTime = false;
        if (auto* main = RE::Main::GetSingleton()) {
            endFreezeTime = main->freezeTime;
        }
        const double hoursPerSec = (elapsed > 0)
            ? (static_cast<double>(delta) / (static_cast<double>(elapsed) / 1000.0))
            : 0.0;
        logger::info("[World] cpu: {} presents in {} ms => {:.1f} fps | freezeTime {} -> {} | game speed {:.3f} h/s (normal ~0.00029)",
            frames, elapsed, fps,
            g_worldProbe.startFreezeTime ? "ON" : "off", endFreezeTime ? "ON" : "off", hoursPerSec);

        std::string timeline;
        for (int i = 0; i < g_worldProbe.tlCount; ++i) {
            const auto step = (i > 0) ? (g_worldProbe.tlFrames[i] - g_worldProbe.tlFrames[i - 1]) : 0u;
            timeline += std::format("{}@{}ms f{} h{:.6f} 3d{}",
                i > 0 ? " | " : "",
                g_worldProbe.tlAt[i], step, g_worldProbe.tlHours[i],
                g_worldProbe.tlHas3D[i] ? "+" : "-");
        }
        logger::info("[World] timeline ({} points): {}", g_worldProbe.tlCount, timeline);
    }

    void EndFreezeFrameWith(std::uint32_t a_holdMs)    {
        if (!presentfreeze::g_suppressPresent.load()) {
            return;
        }
        const auto startedAt = presentfreeze::g_suppressStartAt.load();
        std::thread([a_holdMs, startedAt]() {
            if (a_holdMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(a_holdMs));
            }
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([startedAt]() {
                    if (!presentfreeze::g_suppressPresent.exchange(false)) {
                        return;
                    }
                    logger::info("[B] freeze-frame OFF after {} ms frozen ({} frames not presented)",
                        NowMs() - startedAt, presentfreeze::g_skippedFrames.load());
                });
            }
        }).detach();
    }

    void EndFreezeFrame()
    {
        if (!g_config.freezeFramePresent) {
            return;
        }
        EndFreezeFrameWith(g_config.freezeFrameHoldMs);
    }

    void RaiseSwapBlack(const char* a_reason);    void ReleaseSwapBlack();

    // Watches menu closes that imply "not a door transition": MapMenu (fast travel) and Main Menu
    // (load game). Sets an exemption window so global suppression passes those loads through.
    class TransitionExemptSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (a_event && !a_event->opening) {
                const char* name = a_event->menuName.c_str();
                if (name && (std::strcmp(name, "MapMenu") == 0 || std::strcmp(name, "Main Menu") == 0)) {
                    g_exemptUntil.store(NowMs() + 5000);
                    logger::info("[Tier2] suppression exempted for 5 s (menu '{}' closed -> fast travel / load game)", name);
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    TransitionExemptSink g_exemptSink;

    // Route C (v1.5.1): cancel the engine's own fade-to-black. It MUST be called from the main
    // thread at moments we know the main thread is alive -- the 25 ms watchdog relies on AddTask,
    // and the task pump dies ~0.6-0.75 s into the transition (measured), i.e. exactly before the
    // long part of the load. That is why the first version of route C did nothing.
    // v1.5.3: Suppressing LoadingMenu does NOT suppress the vanilla "Mist Menu", which is the
    // black/mist backdrop the engine shows during a load. It is a separate menu -- Clean Loading
    // Transitions hooks it for exactly this reason (MistMenu::PostDisplay). Log what is open and
    // try to drop the mist; the log tells us definitively what is on screen at the cut.
    void LogOpenMenus(const char* a_where)
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            return;
        }
        std::string open;
        for (const char* name : { "Loading Menu", "Mist Menu", "Fader Menu", "HUD Menu", "Main Menu" }) {
            if (ui->IsMenuOpen(name)) {
                if (!open.empty()) {
                    open += ", ";
                }
                open += name;
            }
        }
        logger::info("[Menus] open at {}: {}", a_where, open.empty() ? "none" : open.c_str());
    }

    void HideMenuIfOpen(const char* a_name)
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !ui->IsMenuOpen(a_name)) {
            return;
        }
        if (auto* q = RE::UIMessageQueue::GetSingleton()) {
            q->AddMessage(RE::BSFixedString(a_name), RE::UI_MESSAGE_TYPE::kHide, nullptr);
            logger::info("[NoBlack] posted Hide for '{}'", a_name);
        }
    }

    int EffectiveSuppressMode()
    {
        // v1.2.6: mode 2 ("open the menu and hide its visuals") is retired. Measured: the menu being
        // open does NOT pause the game (world kept running, freezeTime stayed off), and hiding the
        // movie's visuals never took (background alpha was already 0; _visible=false left the smoke
        // and the black in place) -- while leaving Menu_mc._visible=false behind made every LATER
        // vanilla loading screen render without its text and spinner. So it is clamped to 1.
        int mode = g_config.suppressMode;
        if (mode < 0) {
            mode = (g_config.suppressLoadingMenu || g_config.globalMenuSuppression) ? 1 : 0;
        }
        if (mode == 2) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                logger::warn("[Tier2] iSuppressMode=2 has been retired; running as mode 1 (menu suppression)");
            }
            mode = 1;
        }
        return mode;
    }

    // Mode 2: the menu is allowed to open -- kPausesGame and the engine's loading state machine keep
    // working exactly as vanilla -- and we only take its visuals away. SetBackgroundAlpha(0) removes
    // the black backdrop; the movie root ("Menu_mc") gets _visible=false so the smoke/model/text go too.
    void StopEngineBlack(const char* a_where)
    {
        CacheForms();
        if (g_fadeImod) {
            RE::ImageSpaceModifierInstanceForm::Stop(g_fadeImod);
        }
        if (g_fadeHoldImod) {
            RE::ImageSpaceModifierInstanceForm::Stop(g_fadeHoldImod);
        }
        logger::info("[NoBlack] stopped engine fade records ({})", a_where);
    }

    std::int64_t ComputeFallbackDelayMs(std::uint32_t a_windowMs, std::uint32_t a_cfgTimeoutMs)
    {
        const auto win = static_cast<std::int64_t>(a_windowMs);
        const auto cfg = static_cast<std::int64_t>(a_cfgTimeoutMs);
        const auto winMinus = win - 1500;   /* avoid std::max: windows.h defines a max macro */
        return (win > 1500 && winMinus > cfg) ? winMinus : cfg;
    }

    bool HasPendingTransitionCompletedOnMainThread()
    {
        auto* pl = RE::PlayerCharacter::GetSingleton();
        auto* cell = pl ? pl->GetParentCell() : nullptr;
        if (!cell) {
            return false;
        }
        const auto curID = cell->GetFormID();
        const auto fromID = g_suppressFromCell.load();
        const auto destID = g_suppressDestCell.load();
        return (curID != fromID) || (destID != 0 && destID != fromID && curID == destID);
    }

    void DisarmTransitionStateOnMainThread(const char* a_reason)
    {
        g_suppressArmed.store(false);
        g_suppressWanted.store(false);
        g_menuWasSuppressed.store(false);
        presentfreeze::g_holdActive.store(false);
        voidoverlay::Disarm(a_reason);
        ReleaseSwapBlack();
        EndFreezeFrame();
    }

    RE::UI_MESSAGE_RESULTS HookedLoadingMenuProcessMessage(RE::IMenu* a_this, RE::UIMessage& a_msg)
    {
        const auto msgType = a_msg.type.get();
        if (auto budget = g_msgTypeLogBudget.load(); budget > 0 &&
            g_msgTypeLogBudget.compare_exchange_strong(budget, budget - 1)) {
            logger::info("[Tier2] LoadingMenu ProcessMessage type={} (0=Update 1=Show 2=Reshow 3=Hide 4=ForceHide)",
                static_cast<std::uint32_t>(msgType));
        }
        // Global mode: catch transitions that LoadingFlow's door hook never saw (trigger volumes,
        // script MoveTo). Arming here captures the CURRENT cell as the "from" cell, which is correct
        // because the menu is requested before the cell actually changes.
        const bool wantsShow = (msgType == RE::UI_MESSAGE_TYPE::kShow || msgType == RE::UI_MESSAGE_TYPE::kReshow);
        if (g_config.globalMenuSuppression && !g_suppressArmed.load() && wantsShow) {
            if (NowMs() < g_exemptUntil.load()) {
                logger::info("[Tier2] global suppression skipped: exempt window (fast travel / load game)");
            } else {
                ArmLoadingMenuSuppression("global (menu requested outside the door hook)", true);
                if (g_suppressArmed.load()) {
                    if (!g_menuWasSuppressed.exchange(true)) {
                        const auto fbDelay = ComputeFallbackDelayMs(
                            g_suppressWindowMs.load(), g_config.suppressTimeoutMs);
                        g_fallbackAt.store(NowMs() + fbDelay);
                        g_deadWindowAt.store(NowMs());
                    }
                    logger::info("[Tier2] LoadingMenu kShow suppressed by global mode");
                    return RE::UI_MESSAGE_RESULTS::kIgnore;
                }
            }
        }
        // v1.6.7: late request after the transition already finished -- eat it, and make sure any
        // loading menu that did manage to open is dismissed.
        if (wantsShow && !g_suppressArmed.load() && NowMs() < g_graceSuppressUntil.load() &&
            g_suppressWanted.load() && g_graceEaten.load() < 2) {
            g_graceEaten.fetch_add(1);
            logger::info("[Tier2] late LoadingMenu request suppressed (post-cut grace, {}/2)", g_graceEaten.load());
            HideMenuIfOpen("Loading Menu");
            return RE::UI_MESSAGE_RESULTS::kIgnore;
        }
        if (g_suppressArmed.load() && (g_suppressWanted.load() || g_menuWasSuppressed.load()) && wantsShow) {
            // v1.2.1: main-thread fallback. The 25 ms watchdog cannot be relied on -- the main thread
            // stops servicing AddTask ~0.6-0.75 s into a load -- so the "re-post the vanilla menu"
            // safety net is evaluated HERE, on the thread that is actually running.
            const auto fallbackAt = g_fallbackAt.load();
            if ((fallbackAt != 0 && NowMs() > fallbackAt) || !g_suppressWanted.load()) {
                if (HasPendingTransitionCompletedOnMainThread()) {
                    logger::info("[Tier2] fallback skipped in menu hook: transition already completed -- ignoring late LoadingMenu");
                    if (NowMs() > g_suppressDeadline.load()) {
                        DisarmTransitionStateOnMainThread("late menu after transition already completed");
                    }
                    HideMenuIfOpen("Loading Menu");
                    return RE::UI_MESSAGE_RESULTS::kIgnore;
                }
                const auto elapsed = NowMs() - g_suppressStarted.load();
                const auto win = g_suppressWindowMs.load();
                logger::info("[Tier2] suppression timeout reached in menu hook after {} ms (window {} ms, floor {} ms) -- letting the vanilla menu through",
                    elapsed, win, g_config.suppressTimeoutMs);
                DisarmTransitionStateOnMainThread("suppression timeout reached in menu hook");
                return g_origLoadingProcessMessage ? g_origLoadingProcessMessage(a_this, a_msg)
                                                   : RE::UI_MESSAGE_RESULTS::kPassOn;
            }
            if (NowMs() <= g_suppressDeadline.load()) {
                if (!g_menuWasSuppressed.exchange(true)) {
                    // First suppression of this transition: the re-post fallback is measured from
                    // here, not from the door activation.
                    // v1.3.4: the fallback is a safety net, so it must not fire before the window
                    // we are willing to wait anyway. Derive it from the (possibly adapted) window.
                    const auto fbDelay = ComputeFallbackDelayMs(
                        g_suppressWindowMs.load(), g_config.suppressTimeoutMs);
                    g_fallbackAt.store(NowMs() + fbDelay);
                    // v1.3.0: this is the start of the dead window -- the picture on screen now
                    // stops changing until the cut. The push-in is timed from here.
                    g_deadWindowAt.store(NowMs());
                    // Raise the black NOW, roughly a second before the swap, so that the engine's
                    // scene-attach spin happens behind it instead of in front of it.
                    // v1.2.2: when a swap-time cover is configured, do NOT cover the whole load --
                    // the spin is only ~350 ms, so a long cover just reads as a hang.
                    if (g_config.coverAtSwapMs == 0) {
                        RaiseSwapBlack("menu suppressed");
                        BeginFreezeFrame("menu suppressed");
                    }
                }
                LogOpenMenus("menu suppression");
                HideMenuIfOpen("Mist Menu");
                StopEngineBlack("at menu suppression");
                if (g_config.coverTest == 3) {
                    SetRuntimeCoverAlpha(1.0f);
                    logger::info("[CoverTest] cover up: UI layer, runtime clip (alpha 100)");
                } else if (g_config.coverTest == 2) {
                    // v1.13.0: UI-layer cover. Nothing is presented by us and no IMAD is touched --
                    // our own Scaleform menu fades a fullscreen quad in, and the engine composites it
                    // after upscaling. This is the route that cannot disturb the upscaler.
                    SetCoverAlpha(1.0f);
                    logger::info("[CoverTest] cover up: UI layer (alpha 100)");
                } else if (g_config.coverTest == 1) {
                    // v1.12.1 cover test: raise the engine's own fullscreen cover for the dead window.
                    // Present is never touched, so the upscaler's sequence stays intact -- that is the
                    // whole point of the test.
                    //
                    // Both records are needed. FadeToBlackImod is a FADE -- it ramps to black and then
                    // ends, so on its own it produces exactly the "it goes black for a moment" that the
                    // user reported. FadeToBlackHoldImod is the record the engine uses to STAY black.
                    // Triggering only the first one was my mistake in v1.12.0.
                    if (g_fadeImod) {
                        RE::ImageSpaceModifierInstanceForm::Trigger(g_fadeImod, 1.0f, nullptr);
                    }
                    if (g_fadeHoldImod) {
                        RE::ImageSpaceModifierInstanceForm::Trigger(g_fadeHoldImod, 1.0f, nullptr);
                    }
                    logger::info("[CoverTest] cover up: FadeToBlackImod + FadeToBlackHoldImod at full strength"
                                 " (fade = {}{}, hold = {}{})",
                        static_cast<const void*>(g_fadeImod), g_fadeImod ? "" : " MISSING",
                        static_cast<const void*>(g_fadeHoldImod), g_fadeHoldImod ? "" : " MISSING");
                }
                if (g_config.coverTest == 4) {
                    // v1.14.0: v1.13.4 proved that every always-open menu (HUD, Fader) is
                    // kCustomRendering -- the engine never draws their movies -- and that the ONLY
                    // normally-rendered menu is the Loading Menu itself. So instead of hiding it,
                    // let it render and paint our own opaque quad on top of it.
                    g_coverUiWanted.store(true);
                    // v1.14.2 关键修正:必须在这里**同步**画上。
                    // 实测(v1.14.1 日志):加载期间主线程不再处理 AddTask,周期任务里的重试要等到
                    // 加载结束才第一次执行 —— 结果是 _alpha=100 与 =0 落在同一毫秒,覆盖从未上屏。
                    // 而 ProcessMessage 本来就是在这条菜单实例上被调用的,所以此刻它一定存在。
                    SetRuntimeCoverAlpha(1.0f);
                    logger::info("[Tier2] LoadingMenu let THROUGH and covered synchronously (iCoverTest=4)");
                    return RE::UI_MESSAGE_RESULTS::kPassOn;
                }
                logger::info("[Tier2] LoadingMenu suppressed (type={}, {} ms after activation)",
                    static_cast<std::uint32_t>(msgType), NowMs() - g_suppressStarted.load());
                return RE::UI_MESSAGE_RESULTS::kIgnore;
            }
            if (HasPendingTransitionCompletedOnMainThread()) {
                logger::info("[Tier2] LoadingMenu request past {} ms window ignored: transition already completed",
                    g_suppressWindowMs.load());
                DisarmTransitionStateOnMainThread("late menu past window after completion");
                HideMenuIfOpen("Loading Menu");
                return RE::UI_MESSAGE_RESULTS::kIgnore;
            }
            logger::info("[Tier2] LoadingMenu let through: past the {} ms transition window",
                g_suppressWindowMs.load());
            // The vanilla menu owns the screen from here; stop drawing and holding frames.
            DisarmTransitionStateOnMainThread("menu let through past the window");
        }
        return g_origLoadingProcessMessage ? g_origLoadingProcessMessage(a_this, a_msg)
                                           : RE::UI_MESSAGE_RESULTS::kPassOn;
    }

    // Hold the frame black for a beat after the cell swap, then fade in. This masks the pop-in of
    // geometry that finished streaming just after the cell became current -- the engine's own
    // fade-back can otherwise reveal half-built meshes. One-shot, bounded, main-thread work only.
    // Release the swap-black and fade back in. Must ALWAYS be able to run: a holding record that is
    // never released leaves the player permanently black with only the UI visible.
    void ReleaseSwapBlack()
    {
        CacheForms();
        if (g_fadeHoldImod) {
            RE::ImageSpaceModifierInstanceForm::Stop(g_fadeHoldImod);
        }
        if (g_fadeImod) {
            RE::ImageSpaceModifierInstanceForm::Stop(g_fadeImod);
        }
        if (g_fadeBackImod) {
            RE::ImageSpaceModifierInstanceForm::Trigger(g_fadeBackImod, g_config.envelopeFadeStrength, nullptr);
        }
        logger::info("[Visual] swap black released; fading back in");
    }

    // Bring the black up EARLY. The engine spins/scales the new scene into place during the first
    // frames after the swap, and those frames are visible the instant the view becomes the new cell.
    // Waiting for the cell change and only then going black misses them entirely (measured in-game:
    // the spin finishes, the view stabilises, and only afterwards does the black start). So the hold
    // is raised while the load is still running -- from the moment we suppress the loading menu.
    void RaiseSwapBlack(const char* a_reason)
    {
        if (!g_config.visualEnvelope || g_config.envelopeBlackHoldMs == 0) {
            return;
        }
        CacheForms();
        if (g_fadeHoldImod) {
            RE::ImageSpaceModifierInstanceForm::Trigger(g_fadeHoldImod, g_config.envelopeFadeStrength, nullptr);
            logger::info("[Visual] swap black raised early ({})", a_reason);
        }
    }

    // Hold the frame black for a beat after the cell swap, then fade in. One-shot, bounded,
    // main-thread work only.
    void HoldBlackThenFadeIn(std::uint32_t a_holdMs)
    {
        const float strength = g_config.envelopeFadeStrength;
        CacheForms();
        // Prefer the engine's dedicated hold stage over re-triggering the fade record.
        if (g_fadeHoldImod) {
            RE::ImageSpaceModifierInstanceForm::Trigger(g_fadeHoldImod, g_config.envelopeFadeStrength, nullptr);
        } else if (g_fadeImod) {
            RE::ImageSpaceModifierInstanceForm::Trigger(g_fadeImod, g_config.envelopeFadeStrength, nullptr);
        }
        logger::info("[Visual] holding black for {} ms to mask pop-in (strength {:.2f})",
            a_holdMs, g_config.envelopeFadeStrength);
        std::thread([a_holdMs, strength]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(a_holdMs));
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([a_holdMs]() {
                    // The hold was already raised early (at suppression); this only releases it.
                    (void)a_holdMs;
                    ReleaseSwapBlack();
                });
            }
        }).detach();
    }

    // Fired exactly when the cell swap lands, to hide the hard cut from "door half open" to
    // "inside". Deliberately short: this is a cut mask, not the loading screen we just removed.
    // Black-family records (FadeToBlack / Hold) are released with FadeToBlackBack; any other record
    // is simply stopped after the window.
    void ApplyCutMask()
    {
        const std::uint32_t imodId = g_config.cutMaskImod;
        const std::uint32_t maskMs = g_config.cutMaskMs;
        const float         strength = g_config.envelopeFadeStrength;
        auto* rec = RE::TESForm::LookupByID<RE::TESImageSpaceModifier>(imodId);
        if (!rec) {
            logger::warn("[Visual] cut mask record 0x{:08X} not found; skipped", imodId);
            return;
        }
        const bool  blackFamily = (imodId == 0x000F756D || imodId == 0x000F756E);
        const float fadeOut = g_config.cutMaskFadeOut;
        RE::ImageSpaceModifierInstanceForm::Trigger(rec, strength, nullptr);
        logger::info("[Visual] cut mask 0x{:08X} for {} ms ({}, fade-out {:.2f}s)", imodId, maskMs,
            blackFamily ? "black family" : "generic record", blackFamily ? 0.0f : fadeOut);
        std::thread([imodId, blackFamily, fadeOut, maskMs, strength]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(maskMs));
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([imodId, blackFamily, fadeOut, strength]() {
                    CacheForms();
                    if (blackFamily) {
                        // Same rule as HoldBlackThenFadeIn: never rely on a triggered record to
                        // clear an active one -- release the mask explicitly first.
                        if (g_fadeImod) {
                            RE::ImageSpaceModifierInstanceForm::Stop(g_fadeImod);
                        }
                        if (g_fadeHoldImod) {
                            RE::ImageSpaceModifierInstanceForm::Stop(g_fadeHoldImod);
                        }
                        if (g_fadeBackImod) {
                            RE::ImageSpaceModifierInstanceForm::Trigger(g_fadeBackImod, strength, nullptr);
                        }
                        return;
                    }
                    if (auto* r = RE::TESForm::LookupByID<RE::TESImageSpaceModifier>(imodId)) {
                        (void)r;
                        // Papyrus documents RemoveCrossFade as "fading it out over the specified
                        // number of seconds"; CommonLib wraps it as StopCrossFade. A hard Stop()
                        // snaps the mask off, which reads as a flash on non-black records.
                        if (fadeOut > 0.0f) {
                            RE::ImageSpaceModifierInstanceForm::StopCrossFade(fadeOut);
                        } else {
                            RE::ImageSpaceModifierInstanceForm::Stop(r);
                        }
                    }
                });
            }
        }).detach();
    }

    // Always runs for a routed (real) transition: the timing is the whole point -- it tells us how
    // much of the perceived wait is actual cell I/O versus UI/fade overhead. Suppression itself is
    // captured here as a separate flag, so both modes are measured identically.
    void ArmLoadingMenuSuppression(const char* a_reason, bool a_isAutoCaveDoor, RE::FormID a_destCell)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* fromCell = player ? player->GetParentCell() : nullptr;
        g_suppressWanted.store(g_config.suppressLoadingMenu || g_config.globalMenuSuppression);
        // ---- route C (v1.5.0): always cancel the ENGINE's own fade-to-black ------------------
        // The engine raises FadeToBlackImod / FadeToBlackHoldImod when a load-door transition starts
        // and holds the screen black until it fades back in after the load. Measured on this setup
        // that is exactly the "it goes black for a moment" the user kept seeing -- it survives with
        // every effect of ours switched off. We never add black of our own; we only STOP those two
        // records, so the last frame simply stays on screen until the new cell is there.
        // NOTE: STOP them, never try to cover them -- a Hold record that is never stopped leaves a
        // permanently black screen (that accident is in the archive, do not repeat it).
        g_noBlackWanted.store(true);
        presentfreeze::TraceArm();
        presentfreeze::ArmAutoGate();
        // v1.3.0: RE-ARM THE VISUAL LAYER.
        // This call did not exist. When the visual route was retired in v1.5.0 the call site was
        // deleted but the whole `voidoverlay` namespace stayed behind, still declared, still
        // compiled, still unreachable -- so a mode set in the INI armed nothing and drew nothing,
        // and there was no log line to say why (g_armed simply never became true, and OnPresent
        // returns immediately in that case). A function that is never called fails silently and
        // looks exactly like "the feature does not work on this setup".
        voidoverlay::Arm();
        // v1.5.5: the Present hook is no longer used for drawing -- it exists solely to withhold the
        // engine's empty frames at the cut. Installed here because this is the first moment a swap
        // chain is guaranteed to exist.
        if (auto* sc = presentfreeze::CurrentSwapChain()) {
            presentfreeze::InstallOn(sc);
        }
        LogOpenMenus("door activation");
        HideMenuIfOpen("Mist Menu");
        StopEngineBlack("at door activation");
        g_voidStep.store(-1);
        g_probeWorldLastCell3D = 0;
        g_probeWorldLastMusic = 0;
        WorldProbeBegin();
        g_noBlackStartAt.store(NowMs());
        g_suppressFromCell.store(fromCell ? fromCell->GetFormID() : 0);
        g_suppressDestCell.store(a_destCell);
        g_suppressStarted.store(NowMs());
        // v1.3.4: adaptive window. A heavily modded setup can need ~12 s for a rebuilt interior
        // while a stock setup needs ~1.5 s; one fixed number cannot serve both. If we have measured this
        // very destination cell before, give it 2x plus 3 s of headroom, clamped to 8..60 s.
        // No history -> the configured default, untouched.
        LoadCellTimesOnce();
        std::uint32_t windowMs = g_config.transitionWindowMs;
        if (!g_config.transitionWindowPinned && a_destCell != 0) {
            std::uint32_t histMs = 0;
            {
                std::lock_guard<std::mutex> lk(g_cellLoadMsMutex);
                const auto it = g_cellLoadMs.find(a_destCell);
                if (it != g_cellLoadMs.end()) {
                    histMs = it->second;
                }
            }
            if (histMs > 0) {
                const std::uint32_t est = histMs * 2u + 3000u;
                windowMs = std::clamp(est, 8000u, 60000u);
                logger::info("[Tier2] window adapted: cell 0x{:08X} last load {} ms -> window {} ms",
                    a_destCell, histMs, windowMs);
            }
        } else if (g_config.transitionWindowPinned) {
            logger::info("[Tier2] window pinned by config: {} ms", windowMs);
        }
        g_suppressWindowMs.store(windowMs);
        g_suppressDeadline.store(NowMs() + static_cast<std::int64_t>(windowMs));
        g_fallbackAt.store(0);   // only armed once we actually suppress a menu
        g_menuWasSuppressed.store(false);
        g_deadWindowAt.store(0);   // v1.3.0: the dead window has not started yet
        g_suppressArmed.store(true);

        logger::info("[Timers] in effect for this transition: postLoad={} ms, queueBudget={} ms, fadeToBlack={} s, preset={}",
            g_config.postLoadUpdateTimeMS, g_config.loadingQueuedPriorityBudgetMs,
            g_config.fadeToBlackFadeSeconds,
            g_config.postLoadUpdateTimeMS >= 1900.0f ? "stable" : (g_config.postLoadUpdateTimeMS <= 150.0f ? "fastest/balanced" : "custom"));
        logger::info("[Tier2] real transition started (cell 0x{:08X}, door {}, menu suppression {}, no-black {}, window {} ms, reason: {})",
            g_suppressFromCell.load(), a_isAutoCaveDoor ? "auto/frameless" : "manual",
            g_config.suppressLoadingMenu ? "ON" : "OFF",
            g_noBlackWanted.load() ? "FREEZE" : "fade",
            windowMs, a_reason);
        // One-shot watchdog. Bounded loop, posts at most one task per step, never re-queues itself
        // (the AddTask feedback loop is what froze this project twice before).
        const std::uint32_t myGen = ++g_suppressGen;
        const std::uint32_t probeMs = g_config.probeAssembly ? g_config.probeAssemblyMs : 0u;
        const std::uint32_t totalWatchMs = windowMs + probeMs + 5000u;
        std::thread([myGen, totalWatchMs]() {
            constexpr int kStepMs = 25;
            const int maxSteps = static_cast<int>((totalWatchMs + kStepMs - 1u) / kStepMs);
            bool fallbackPosted = false;
            for (int i = 0; i < maxSteps; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kStepMs));
                if (g_shuttingDown.load(std::memory_order_acquire) || g_suppressGen.load() != myGen) {
                    return;   // process exiting or a newer transition took over
                }
                if (!g_suppressArmed.load() && !g_probeArmed.load()) {
                    return;   // transition done AND assembly probe done
                }
                // Fallback re-post: measured from the moment we ACTUALLY suppressed a menu, because
                // the engine may ask for it a full second after the activation.
                const auto fbAt = g_fallbackAt.load();
                // v1.3.0: the fallback has to take effect even when the main thread never services
                // the queued task, so the state flags are flipped right here (plain atomics, safe
                // from this thread) and the re-post is retried for a few seconds.
                if (fbAt != 0 && NowMs() > fbAt) {
                    // v1.3.0: the parts that MUST take effect even while the main thread is blocked
                    // are plain atomics -- safe to flip from this thread, and the render thread reads
                    // them immediately. The UI message itself stays on the main thread: posting it
                    // from here would touch the message queue off-thread, which is not thread safe.
                    g_suppressWanted.store(false);
                    presentfreeze::g_holdActive.store(false);
                }
                if (!fallbackPosted && fbAt != 0 && NowMs() > fbAt) {
                    fallbackPosted = true;
                    if (auto* task = SKSE::GetTaskInterface()) {
                        task->AddTask([myGen]() {
                            if (g_shuttingDown.load(std::memory_order_acquire) ||
                                g_suppressGen.load() != myGen || !g_menuWasSuppressed.load()) {
                                return;
                            }
                            // v1.3.4 CRASH-FREE FIX: look again before handing the menu back.
                            // Measured 2026-11-08: the fallback ran at 11871 ms while the transition
                            // had ALREADY completed (current == dest) -- it re-posted the loading menu
                            // onto a world that was already loaded, which is the "stuck, then a loading
                            // screen pops in" the user saw. If we have clearly moved on, do NOT re-post.
                            if (HasPendingTransitionCompletedOnMainThread()) {
                                auto* plFb = RE::PlayerCharacter::GetSingleton();
                                auto* cFb = plFb ? plFb->GetParentCell() : nullptr;
                                logger::info("[Tier2] fallback skipped: the transition already completed"
                                             " (current 0x{:08X}, from 0x{:08X}, dest 0x{:08X}) -- not re-posting",
                                    cFb ? cFb->GetFormID() : 0u, g_suppressFromCell.load(), g_suppressDestCell.load());
                                g_suppressWanted.store(false);
                                presentfreeze::g_holdActive.store(false);
                                return;
                            }
                            // Stop suppressing from now on, otherwise the re-posted menu would be
                            // swallowed again by this same window and the player would see nothing.
                            g_suppressWanted.store(false);
                            g_menuWasSuppressed.store(false);
                            voidoverlay::Disarm("menu re-posted (fallback)");
                            ReleaseSwapBlack();
                            EndFreezeFrame();
                            {
                                const auto waited = NowMs() - g_suppressStarted.load();
                                const auto win = g_suppressWindowMs.load();
                                auto* pl2 = RE::PlayerCharacter::GetSingleton();
                                auto* c2 = pl2 ? pl2->GetParentCell() : nullptr;
                                logger::warn("[Tier2] suppressed menu did not complete in time after {} ms"
                                             " (window {} ms); re-posting the LoadingMenu. current cell 0x{:08X},"
                                             " from 0x{:08X}, dest 0x{:08X}",
                                    waited, win, c2 ? c2->GetFormID() : 0u,
                                    g_suppressFromCell.load(), g_suppressDestCell.load());
                            }
                            if (auto* q = RE::UIMessageQueue::GetSingleton()) {
                                q->AddMessage(RE::BSFixedString(RE::LoadingMenu::MENU_NAME.data()),
                                    RE::UI_MESSAGE_TYPE::kShow, nullptr);
                            }
                        });
                    }
                }
                if (g_suppressArmed.load() && NowMs() > g_suppressDeadline.load()) {
                    const auto winElapsed = g_suppressWindowMs.load();
                    logger::info("[Tier2] transition window ({} ms) elapsed without a cell change; stopping measurement", winElapsed);
                    g_suppressArmed.store(false);
                    g_suppressWanted.store(false);
                    presentfreeze::g_holdActive.store(false);
                    voidoverlay::Disarm("transition window elapsed without a cell change");
                    const bool needRepost = g_menuWasSuppressed.exchange(false) && !fallbackPosted;
                    fallbackPosted = true;
                    // Never leave black/freeze or a swallowed LoadingMenu up if the transition never reports back.
                    if (auto* task = SKSE::GetTaskInterface()) {
                        task->AddTask([myGen, needRepost]() {
                            if (g_shuttingDown.load(std::memory_order_acquire) || g_suppressGen.load() != myGen) {
                                return;
                            }
                            ReleaseSwapBlack();
                            EndFreezeFrame();
                            if (needRepost && !HasPendingTransitionCompletedOnMainThread()) {
                                logger::warn("[Tier2] re-posting LoadingMenu at {} ms window expiry",
                                    g_suppressWindowMs.load());
                                if (auto* q = RE::UIMessageQueue::GetSingleton()) {
                                    q->AddMessage(RE::BSFixedString(RE::LoadingMenu::MENU_NAME.data()),
                                        RE::UI_MESSAGE_TYPE::kShow, nullptr);
                                }
                            }
                        });
                    }
                    if (!g_probeArmed.load()) {
                        return;
                    }
                }
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask([myGen]() {
                        if (g_shuttingDown.load(std::memory_order_acquire) || g_suppressGen.load() != myGen) {
                            return;
                        }
                        if (!g_suppressArmed.load() && !g_probeArmed.load()) {
                            return;
                        }
                        // Probe first: it is independent of the transition state, and this step may be
                        // the very one that reports the transition as finished.
                        if (g_probeArmed.load()) {
                            ProbeSample();
                        }
                        if (!g_suppressArmed.load()) {
                            return;   // probe-only step; the transition itself already completed
                        }
                        // No-black mode: keep cancelling the engine's fade records for the whole
                        // transition window. The last presented frame then simply holds until the
                        // new cell shows up -- a freeze instead of a fade.
                        if (g_noBlackWanted.load() && NowMs() >= g_noBlackStartAt.load()) {
                            CacheForms();
                            // Only cancel the records that DARKEN the screen. Cancelling the fade-BACK
                            // as well makes the engine's own fade-in restart every 25 ms, which reads
                            // as strobing (the "scene spins into place" effect) right at the swap.
                            // v1.12.0 cover test: when it is on we WANT the darkening record alive --
                            // it is the fullscreen cover we are trying to prove out.
                            if (g_fadeImod && g_config.coverTest == 0) {
                                RE::ImageSpaceModifierInstanceForm::Stop(g_fadeImod);
                            }
                            if (g_fadeHoldImod && g_config.coverTest == 0) {
                                RE::ImageSpaceModifierInstanceForm::Stop(g_fadeHoldImod);
                            }
                        }
                        // v1.2.3: keep the dead void moving (ramps an IMAD darkening in steps).
                        if (!g_noBlackWanted.load()) {
                            VoidRampStep();
                        }
                        WorldProbeStep();
                        if (g_config.coverTest == 4 && g_coverUiWanted.load()) {
                            // 只维持满覆盖。渐隐若由 tick 驱动会被饿死(实测 2081/12028 ms),
                            // 所以过渡只能交给 Scaleform 自己推进(见 onEnterFrame 计划)。
                            SetRuntimeCoverAlpha(1.0f);
                        }
                        auto* pl = RE::PlayerCharacter::GetSingleton();
                        if (!pl) {
                            return;
                        }
                        auto* cell = pl->GetParentCell();
                        // v1.3.4: two independent acceptance signals, and one diagnostic line per
                        // second while we wait -- so a future report shows which id disagreed.
                        const auto curID = cell ? cell->GetFormID() : 0u;
                        const auto fromID = g_suppressFromCell.load();
                        const auto destID = g_suppressDestCell.load();
                        const bool movedByFrom = cell && curID != fromID;
                        const bool movedByDest = cell && destID != 0 && destID != fromID && curID == destID;
                        {
                            static std::int64_t lastDiag = 0;
                            if (NowMs() - lastDiag > 1000) {
                                lastDiag = NowMs();
                                logger::info("[Tier2] waiting: current 0x{:08X}, from 0x{:08X}, dest 0x{:08X}",
                                    curID, fromID, destID);
                            }
                        }
                        if (movedByFrom || movedByDest) {
                            const auto ms = NowMs() - g_suppressStarted.load();
                            if (ms > 0) {
                                // v1.3.4d: keep the WORST (cold) time, not the last one. A hot-cache
                                // re-entry is much faster and would shrink the window for the next cold
                                // load -- measured 11578 ms cold vs 5014 ms hot on the same door.
                                std::lock_guard<std::mutex> lk(g_cellLoadMsMutex);
                                auto& slot = g_cellLoadMs[cell->GetFormID()];
                                const auto v = static_cast<std::uint32_t>(ms);
                                slot = (v > slot) ? v : slot;
                            }
                            ScheduleCellTimesSave();
                            const bool wasSuppressed = g_menuWasSuppressed.exchange(false);
                            g_suppressArmed.store(false);
                            logger::info("[Tier2] real transition finished in {} ms (cell 0x{:08X} -> 0x{:08X}, menu {})",
                                ms, g_suppressFromCell.load(), cell->GetFormID(),
                                wasSuppressed ? "suppressed" : "shown");
                            // v1.4.0: this is the cut -- the new cell is current and the engine's
                            // ~350 ms scene spin starts here. Hand the overlay over to its pulse.
                            // v1.3.0: this call was missing too (same v1.5.0 removal). Without it
                            // g_cut never becomes true, so the dead-window push-in would run to its
                            // cap and then sit on a frozen zoomed frame until the hard cap disarmed
                            // the layer -- i.e. no cross-fade, and the picture stuck at max push-in.
                            voidoverlay::Cut();
                            LogOpenMenus("cut");
                            g_graceSuppressUntil.store(NowMs() + 2500);
                            g_graceEaten.store(0);
                            HideMenuIfOpen("Loading Menu");   // safety net: never leave it up
                            StopEngineBlack("at cut (fade-back raises black again)");
                            if (g_config.coverTest == 4) {
                                // v1.16.1: 渐隐改回"立即撤"。
                                // 实测(日志):周期任务驱动一次 500 ms 的渐隐,实际用了 2081 ms 和 12028 ms
                                // —— 覆盖压在新 cell 上近 2~12 秒,比原问题更糟。加载/切场期间我们的
                                // 周期任务跑不动,所以任何"由 C++ tick 驱动的动画"在这条路上都不成立。
                                g_coverUiWanted.store(false);
                                SetRuntimeCoverAlpha(0.0f);
                                logger::info("[CoverTest] cover removed immediately at the cut");
                            } else if (g_config.coverTest == 3) {
                                SetRuntimeCoverAlpha(0.0f);
                                logger::info("[CoverTest] cover down at the cut: UI runtime clip (alpha 0)");
                            } else if (g_config.coverTest == 2) {
                                SetCoverAlpha(0.0f);
                                logger::info("[CoverTest] cover down at the cut: UI layer (alpha 0)");
                            } else if (g_config.coverTest == 1) {
                                logger::info("[CoverTest] cover down at the cut -- watch whether the hand-back is clean");
                            }
                            presentfreeze::HoldAtCut(g_config.holdAtCutMs);
                            presentfreeze::ArmHitchWindow();
                            // v1.4.12: the counter used to be bumped only on the frozen pocket path, so the
                            // menu always showed 0 even though transitions were happening.
                            ++g_transitionCount;
                            WorldProbeFinish();
                            if (g_config.probeAssembly) {
                                ProbeArm();
                            }
                            // v1.2.3: if the void was being dimmed, take it to full black across the
                            // spin and then fade into the finished cell. This is the transition the
                            // user asked for: dim over the dead window, black over the spin, fade in.
                            if (g_config.voidDarken > 0.01f) {
                                VoidFadeOutAtSwap(g_config.voidHoldMs);
                            }
                            // v1.2.2: cover the SWAP only, for at most uCoverAtSwapMs. The engine
                            // attaches the new scene over ~21 frames at 60 fps (~350 ms), which is
                            // the only part worth hiding; the load before it is static anyway.
                            if (g_config.coverAtSwapMs > 0 && g_config.voidDarken <= 0.01f) {
                                if (g_config.coverAtSwapMode == 2) {
                                    HoldBlackThenFadeIn(g_config.coverAtSwapMs);
                                } else if (g_config.freezeFramePresent || g_config.coverAtSwapMode == 1) {
                                    // Skip presentation from here; the display keeps the pre-spin frame.
                                    if (!presentfreeze::g_suppressPresent.load()) {
                                        auto* sc = presentfreeze::CurrentSwapChain();
                                        if (sc && presentfreeze::InstallOn(sc)) {
                                            presentfreeze::g_skippedFrames.store(0);
                                            presentfreeze::g_firstSkipLogged.store(false);
                                            presentfreeze::g_suppressStartAt.store(NowMs());
                                            presentfreeze::g_suppressPresent.store(true);
                                            logger::info("[Cover] swap cover ON for {} ms (freeze the presented image)", g_config.coverAtSwapMs);
                                            EndFreezeFrameWith(g_config.coverAtSwapMs);
                                        }
                                    }
                                }
                            }
                            if (g_noBlackWanted.load()) {
                                // v1.5.2: route C adds NO mask here. The old "cut mask" was a black
                                // image-space drip (sCutMaskImod) triggered right at the cut; with route
                                // C always on it fired on every single transition -- and THAT was the
                                // "black flash" the user kept reporting, surviving every visual switch
                                // because it is part of the core path, not the visual layer.
                            } else if (g_config.visualEnvelope && g_config.envelopeBlackHoldMs > 0) {
                                HoldBlackThenFadeIn(g_config.envelopeBlackHoldMs);
                            } else {
                                // Safety net. A holding record that never gets released leaves the
                                // player permanently black with only the UI visible, so make sure
                                // nothing of ours is still holding the screen.
                                CacheForms();
                                if (g_fadeHoldImod) {
                                    RE::ImageSpaceModifierInstanceForm::Stop(g_fadeHoldImod);
                                }
                            }
                            // Always, on every path: never leave presentation suppressed.
                            EndFreezeFrame();
                        }
                    });
                }
            }
            if (g_shuttingDown.load(std::memory_order_acquire) || g_suppressGen.load() != myGen) {
                return;
            }
            const bool hadSuppress = g_suppressArmed.exchange(false);
            const bool hadProbe = g_probeArmed.exchange(false);
            if (hadSuppress || hadProbe) {
                g_suppressWanted.store(false);
                presentfreeze::g_holdActive.store(false);
                g_menuWasSuppressed.store(false);
                voidoverlay::Disarm("watchdog step cap reached");
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask([myGen]() {
                        if (g_shuttingDown.load(std::memory_order_acquire) || g_suppressGen.load() != myGen) {
                            return;
                        }
                        ReleaseSwapBlack();
                        EndFreezeFrame();
                    });
                }
            }
        }).detach();
    }

    void InstallLoadingMenuHook()
    {
        constexpr std::size_t kProcessMessageIdx = 0x04;
        constexpr std::uint32_t kPageExecuteReadWrite = 0x40;

        REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_LoadingMenu[0] };
        auto** table = reinterpret_cast<void**>(vtbl.address());
        if (!table || !IsExecutableAddress(table[kProcessMessageIdx])) {
            logger::warn("[Tier2] LoadingMenu vtable[0x04] is not executable; menu suppression unavailable");
            return;
        }
        std::uint32_t oldProt = 0;
        // v1.2.2: report WHO owned the slot before us. Anything that hooks the loading menu
        // (the same class of mod as Faster Loadscreens / Loading Screen Truce / custom loading
        // screens) sits on this exact vtable entry, and while we suppress a request our handler
        // returns without calling theirs -- their feature is silently bypassed for that request.
        // The log line is the fastest way for a user to tell us what is chained here.
        const auto prevOwner = capprobe::ModuleOf(table[kProcessMessageIdx]);
        const auto prevAddr = reinterpret_cast<std::uintptr_t>(table[kProcessMessageIdx]);
        if (REX::W32::VirtualProtect(&table[kProcessMessageIdx], sizeof(void*), kPageExecuteReadWrite, &oldProt)) {
            g_origLoadingProcessMessage = reinterpret_cast<ProcessMessage_t>(table[kProcessMessageIdx]);
            table[kProcessMessageIdx] = reinterpret_cast<void*>(&HookedLoadingMenuProcessMessage);
            REX::W32::VirtualProtect(&table[kProcessMessageIdx], sizeof(void*), oldProt, &oldProt);
            logger::info("[Tier2] LoadingMenu ProcessMessage hook installed at vtable[0x04]=0x{:X} in {} (suppression {})",
                prevAddr, prevOwner, g_config.suppressLoadingMenu ? "ENABLED" : "disabled");
            const bool foreign = (prevOwner != "SkyrimSE.exe" && prevOwner != "?");
            if (foreign) {
                logger::warn("[Compat] the loading-menu vtable was already hooked by {} -- while a door "
                             "transition is suppressed, that mod's handler is not called. If loading screens, "
                             "loading-related UI or its own transitions misbehave, set bSuppressLoadingMenu=0 "
                             "and report this line.", prevOwner);
            }
        } else {
            logger::warn("[Tier2] could not make the LoadingMenu vtable writable");
        }
    }

    // =========================================================================
    //  Tier 3: proximity pre-warm
    //  Stands in for the "pin the last N interiors" idea the original design proposed: SE exposes no
    //  way to stop a cell from being detached (TESObjectCELL has IsAttached() and a cellDetached
    //  flag, but no Attach/Detach method), so keeping 3D trees alive is not reachable from the
    //  public surface. Instead we make the *re-load* cheap: while the player walks near a load
    //  door whose destination is routed to a real transition, demand that cell's models into the
    //  model DB ahead of time, using our own blueprint index (no need for the cell to be loaded).
    // =========================================================================
    std::atomic<bool>                            g_prewarmThreadStarted{ false };
    std::mutex                                   g_prewarmMutex;
    std::unordered_map<RE::FormID, std::int64_t> g_prewarmLastMs;
    std::unordered_set<std::string>              g_demandedPaths;   // never demand the same NIF twice
    std::unordered_map<RE::FormID, std::uint32_t> g_prewarmCount;   // per-cell, to honour the cap

    const char* ModelPathOf(RE::TESForm* a_form)
    {
        if (!a_form) {
            return nullptr;
        }
        using FT = RE::FormType;
        switch (a_form->GetFormType()) {
        case FT::Static:
            if (auto* x = a_form->As<RE::TESObjectSTAT>()) return x->GetModel();
            break;
        case FT::MovableStatic:
            if (auto* x = a_form->As<RE::BGSMovableStatic>()) return x->GetModel();
            break;
        case FT::Tree:
            if (auto* x = a_form->As<RE::TESObjectTREE>()) return x->GetModel();
            break;
        case FT::Activator:
            if (auto* x = a_form->As<RE::TESObjectACTI>()) return x->GetModel();
            break;
        case FT::Furniture:
            if (auto* x = a_form->As<RE::TESFurniture>()) return x->GetModel();
            break;
        case FT::Door:
            if (auto* x = a_form->As<RE::TESObjectDOOR>()) return x->GetModel();
            break;
        case FT::Flora:
            if (auto* x = a_form->As<RE::TESFlora>()) return x->GetModel();
            break;
        default:
            break;
        }
        return nullptr;
    }

    // Demands at most a slice of the cell's models, so a single frame never stalls: whatever does
    // not fit in the budget is picked up by the next tick (already-demanded paths are skipped).
    // Returns true when the cell is finished (all unique models handled or the cap was hit).
    bool PrewarmChunk(RE::FormID a_cellID, const BlueprintCell& a_bp)
    {
        constexpr std::int64_t kBudgetMs = 8;
        const auto t0 = NowMs();
        std::uint32_t demanded = 0;
        std::uint32_t noModel = 0;
        std::uint32_t pending = 0;

        for (const auto& r : a_bp.refs) {
            if (NowMs() - t0 > kBudgetMs) {
                pending = 1;   // more work left
                break;
            }
            {
                std::lock_guard<std::mutex> lk(g_prewarmMutex);
                if (g_prewarmCount[a_cellID] >= g_config.prewarmModelCap) {
                    break;
                }
            }
            const char* path = ModelPathOf(RE::TESForm::LookupByID(r.baseID));
            if (!path || !*path) {
                ++noModel;
                continue;
            }
            {
                std::lock_guard<std::mutex> lk(g_prewarmMutex);
                if (!g_demandedPaths.insert(std::string(path)).second) {
                    continue;   // already resident from an earlier cell
                }
                ++g_prewarmCount[a_cellID];
            }
            RE::BSModelDB::DBTraits::ArgsType args{};
            RE::NiPointer<RE::NiNode> model;
            (void)RE::BSModelDB::Demand(path, model, args);
            ++demanded;
        }

        if (demanded > 0) {
            logger::info("[Tier3] prewarmed {} model(s) in {} ms for cell 0x{:08X} ({}, cap {}, refs {})",
                demanded, NowMs() - t0, a_cellID, pending ? "more pending" : "slice done",
                g_config.prewarmModelCap, a_bp.refs.size());
        }
        (void)noModel;
        return pending == 0;
    }

    void PrewarmTick()
    {
        if (!g_config.prewarmNearbyDoors) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        auto* cell = player->GetParentCell();
        if (!cell) {
            return;
        }
        const RE::NiPoint3 ppos = player->GetPosition();
        const float r2 = g_config.prewarmRadius * g_config.prewarmRadius;

        std::vector<RE::FormID> candidates;
        cell->ForEachReference([&](RE::TESObjectREFR* a_ref) -> RE::BSContainer::ForEachResult {
            auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
            if (!base || base->GetFormType() != RE::FormType::Door) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            auto* xTel = a_ref->extraList.GetByType<RE::ExtraTeleport>();
            if (!xTel || !xTel->teleportData) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            const RE::NiPoint3 d = a_ref->GetPosition() - ppos;
            if (d.x * d.x + d.y * d.y + d.z * d.z > r2) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            auto linked = xTel->teleportData->linkedDoor.get();
            if (!linked) {
                linked = RE::TESObjectREFR::LookupByHandle(xTel->teleportData->linkedDoor.native_handle());
            }
            if (!linked) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            auto* dest = linked->GetParentCell();
            if (!dest) {
                dest = linked->GetSaveParentCell();
            }
            if (!dest) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            const auto id = dest->GetFormID();
            // With pocket copies disabled (the default) EVERY door takes the engine's real
            // transition, so every destination cell is worth pre-warming. The old filter only kept
            // cells present in the exclude list -- in this mode that is just the 101 scripted cells,
            // so pre-warming did nothing for ordinary doors. That bug made Tier3 a no-op.
            if (!g_config.usePocketCopies || g_excludeCells.count(id) != 0) {
                candidates.push_back(id);
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });
        if (candidates.empty()) {
            return;
        }

        const auto snap = GetBlueprintSnapshot();
        if (!snap) {
            return;
        }
        const auto now = NowMs();
        for (const auto id : candidates) {
            {
                std::lock_guard<std::mutex> lk(g_prewarmMutex);
                const auto it = g_prewarmLastMs.find(id);
                if (it != g_prewarmLastMs.end() && now - it->second < 60000) {
                    continue;   // finished within the last minute
                }
            }
            const auto bpIt = snap->cells.find(id);
            if (bpIt == snap->cells.end() || bpIt->second.refs.empty()) {
                continue;
            }
            if (PrewarmChunk(id, bpIt->second)) {
                std::lock_guard<std::mutex> lk(g_prewarmMutex);
                g_prewarmLastMs[id] = now;   // only mark "done" once the cell is fully handled
            }
            return;   // one cell per tick keeps the frame cost bounded
        }
    }

    void StartPrewarmThread()
    {
        if (g_prewarmThreadStarted.exchange(true)) {
            return;
        }
        logger::info("[Tier3] prewarm poller started (radius {:.0f}, cap {}, 200 ms interval, enabled={})",
            g_config.prewarmRadius, g_config.prewarmModelCap, g_config.prewarmNearbyDoors ? "yes" : "no");
        std::thread([]() {
            while (true) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if (!g_config.prewarmNearbyDoors) {
                    continue;   // do not even queue a task when the feature is off
                }
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask([]() { PrewarmTick(); });
                }
            }
        }).detach();
    }

    bool HookedContActivate(RE::TESBoundObject* a_this, RE::TESObjectREFR* a_targetRef, RE::TESObjectREFR* a_activatorRef,
        std::uint8_t a_arg3, RE::TESBoundObject* a_object, std::int32_t a_targetCount)
    {
        if (a_targetRef && !g_bypassContainerHook.load()) {
            RE::FormID origContID = 0;
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                const auto it = g_clonedContToOriginalCont.find(a_targetRef->GetFormID());
                if (it != g_clonedContToOriginalCont.end()) {
                    origContID = it->second;
                }
            }
            if (origContID != 0) {
                if (auto* origContRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(origContID)) {
                    logger::info("[LF] Redirecting container activate 0x{:08X} -> original 0x{:08X}",
                        a_targetRef->GetFormID(), origContID);
                    g_bypassContainerHook.store(true);
                    const bool res = origContRef->ActivateRef(a_activatorRef, a_arg3, a_object, a_targetCount, false);
                    g_bypassContainerHook.store(false);
                    return res;
                }
            }
        }
        return g_origContActivate ? g_origContActivate(a_this, a_targetRef, a_activatorRef, a_arg3, a_object, a_targetCount) : false;
    }

    // =========================================================================
    //  TESObjectDOOR::Activate Hook (vtable[0x37])
    // =========================================================================
    bool HookedDoorActivate(RE::TESBoundObject* a_this, RE::TESObjectREFR* a_targetRef, RE::TESObjectREFR* a_activatorRef,
        std::uint8_t a_arg3, RE::TESBoundObject* a_object, std::int32_t a_targetCount)
    {
        auto callOriginal = [&]() {
            return g_origDoorActivate ? g_origDoorActivate(a_this, a_targetRef, a_activatorRef, a_arg3, a_object, a_targetCount) : false;
        };

        if (!g_config.enabled || !a_targetRef || !a_activatorRef) {
            return callOriginal();
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        // Diagnostic (v0.10.7): log door activations only when explicitly enabled -- NPCs activate
        // doors constantly (502 of 559 lines in one session), which drowns the real signal.
        if (g_config.logDoorActivations) {
            logger::info("[Door] activate ref=0x{:08X} base=0x{:08X} by={} locked={} state={} activity={}",
                a_targetRef->GetFormID(),
                a_this ? a_this->GetFormID() : 0,
                (player && a_activatorRef == player) ? "player" : "other",
                a_targetRef->IsLocked() ? "yes" : "no",
                a_targetRef->IsActivationBlocked() ? "blocked" : "ok",
                g_transitionInProgress.load() ? "transition" : "idle");
        }
        if (!player || a_activatorRef != player) {
            return callOriginal();
        }
        if (g_transitionInProgress.load()) {
            return true;
        }

        // Resolve source door (either a real door or a cloned pocket door)
        RE::FormID         sourceDoorID = a_targetRef->GetFormID();
        RE::TESObjectREFR* effectiveDoor = a_targetRef;
        bool               isClonedPocketDoor = false;
        {
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            auto it = g_clonedDoorToOriginalDoor.find(sourceDoorID);
            if (it != g_clonedDoorToOriginalDoor.end()) {
                sourceDoorID = it->second;
                isClonedPocketDoor = true;
                if (auto* origDoor = RE::TESForm::LookupByID<RE::TESObjectREFR>(sourceDoorID)) {
                    effectiveDoor = origDoor;
                }
            }
        }

        if (g_config.sneakForcesVanillaLoad && player->IsSneaking()) {
            logger::info("[LF] Player is sneaking; bypassing LoadingFlow for door 0x{:08X}", sourceDoorID);
            g_activeLoadingFlowID.store(0);
            return g_origDoorActivate(a_this, effectiveDoor, a_activatorRef, a_arg3, a_object, a_targetCount);
        }

        if (effectiveDoor->IsLocked()) {
            logger::info("[LF] Door 0x{:08X} is locked; delegating to vanilla lock handler", sourceDoorID);
            return g_origDoorActivate(a_this, effectiveDoor, a_activatorRef, a_arg3, a_object, a_targetCount);
        }

        // Extract teleport destination from live ExtraTeleport OR offline Blueprint door table
        RE::FormID         destCellID = 0;
        RE::TESObjectCELL* destCell = nullptr;
        bool               destIsInterior = false;
        RE::NiPoint3       telPos{};
        RE::NiPoint3       telRot{};
        bool               hasTel = false;

        if (auto* xTel = effectiveDoor->extraList.GetByType<RE::ExtraTeleport>(); xTel && xTel->teleportData) {
            telPos = xTel->teleportData->position;
            telRot = xTel->teleportData->rotation;
            auto linkedDoorPtr = xTel->teleportData->linkedDoor.get();
            if (!linkedDoorPtr) {
                linkedDoorPtr = RE::TESObjectREFR::LookupByHandle(xTel->teleportData->linkedDoor.native_handle());
            }
            if (linkedDoorPtr) {
                destCell = linkedDoorPtr->GetParentCell();
                if (!destCell) {
                    destCell = linkedDoorPtr->GetSaveParentCell();
                }
                if (destCell) {
                    destCellID = destCell->GetFormID();
                    destIsInterior = destCell->IsInteriorCell();
                    hasTel = true;
                }
            }
        }

        // P1-4: take one immutable Blueprint snapshot for the whole transition (no g_bpMutex held
        // while we call into the engine, and a Force Rebuild can safely swap it mid-transition).
        const auto bpSnap = GetBlueprintSnapshot();

        // Fallback to offline Blueprint door table when source door or linked door is not resolved in memory
        if (!hasTel && bpSnap) {
            const auto dIt = bpSnap->doors.find(sourceDoorID);
            if (dIt != bpSnap->doors.end()) {
                telPos = { dIt->second.telPos[0], dIt->second.telPos[1], dIt->second.telPos[2] };
                telRot = { dIt->second.telRot[0], dIt->second.telRot[1], dIt->second.telRot[2] };
                const auto linkedIt = bpSnap->doors.find(dIt->second.linkedDoorID);
                if (linkedIt != bpSnap->doors.end()) {
                    destCellID = linkedIt->second.parentCellID;
                    destIsInterior = (linkedIt->second.isParentInterior != 0);
                    destCell = RE::TESForm::LookupByID<RE::TESObjectCELL>(destCellID);
                    hasTel = true;
                }
            }
        }

        if (!hasTel || destCellID == 0) {
            return callOriginal();
        }

        RE::TESObjectCELL* playerHostCell = player->GetParentCell();
        RE::TESWorldSpace* playerHostWorld = player->GetWorldspace();
        if (!playerHostCell) {
            return callOriginal();
        }

        // ---------------------------------------------------------------------
        //  Case A: Destination is an EXTERIOR cell (Pocket Interior -> Exterior)
        // ---------------------------------------------------------------------
        if (!destIsInterior) {
            if (!isClonedPocketDoor || !g_config.allowPocketToExterior) {
                g_activeLoadingFlowID.store(0);
                return g_origDoorActivate(a_this, effectiveDoor, a_activatorRef, a_arg3, a_object, a_targetCount);
            }
            if (!playerHostWorld || (destCell && !destCell->IsAttached())) {
                logger::info("[LF] Exterior destination cell 0x{:08X} is not attached; falling back to vanilla load", destCellID);
                g_activeLoadingFlowID.store(0);
                return g_origDoorActivate(a_this, effectiveDoor, a_activatorRef, a_arg3, a_object, a_targetCount);
            }

            const bool isAutoCave = IsAutoOrFramelessDoor(a_targetRef);
            const std::uint32_t delayMs = isAutoCave ? (std::min)(g_config.doorOpenDelayMs, 120u) : g_config.doorOpenDelayMs;
            const RE::ObjectRefHandle doorHandle = a_targetRef->GetHandle();

            BeginDoorTransitionVisuals(a_targetRef, isAutoCave);
            ++g_transitionCount;

            auto finishExit = [destCellID, telPos, telRot, doorHandle, isAutoCave]() {
                const RE::FormID leftPocketID = g_activeLoadingFlowID.exchange(0);
                if (auto* pl = RE::PlayerCharacter::GetSingleton()) {
                    pl->SetPosition(telPos, true);
                    pl->SetRotationZ(telRot.z);
                }
                // P1-1: on the way out, put the NPCs of the pocket we just left back in their real cell.
                if (leftPocketID != 0) {
                    std::lock_guard<std::recursive_mutex> lk(g_mutex);
                    auto pkIt = g_pockets.find(leftPocketID);
                    if (pkIt != g_pockets.end()) {
                        TransferPocketActors(pkIt->second, false);
                    }
                }
                // L2: tell quest scripts we are back outside.
                FireFakeCellEvents(LookupCellOrNull(leftPocketID), LookupCellOrNull(destCellID), false);
                FinishDoorTransitionVisuals(doorHandle, isAutoCave);
                g_transitionInProgress.store(false);
                logger::info("[LF] >>> instant EXIT to Exterior cell 0x{:08X} pos=({:.1f},{:.1f},{:.1f}) yaw={:.2f}",
                    destCellID, telPos.x, telPos.y, telPos.z, telRot.z);
                if (g_config.showDebugNotifications) {
                    RE::DebugNotification("LoadingFlow: Instant Exit");
                }
            };

            if (delayMs == 0) {
                finishExit();
            } else {
                g_transitionInProgress.store(true);
                std::thread([delayMs, finishExit]() mutable {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                    if (auto* task = SKSE::GetTaskInterface()) {
                        task->AddTask(std::move(finishExit));
                    } else {
                        g_transitionInProgress.store(false);
                    }
                }).detach();
            }
            return true;
        }

        // ---------------------------------------------------------------------
        //  Case B: Destination is an INTERIOR cell (Ext->Int or Int->Int)
        // ---------------------------------------------------------------------
        if (g_excludeCells.count(destCellID) != 0) {
            // Hard exclude: real transition AND no menu suppression -- the per-cell escape hatch for
            // anything that misbehaves with a hidden menu.
            logger::info("[LF] Destination cell 0x{:08X} is in [ExcludeCells]; real transition, menu left alone", destCellID);
            return g_origDoorActivate(a_this, effectiveDoor, a_activatorRef, a_arg3, a_object, a_targetCount);
        }

        // Same-cell "transition": a mod door/stairs that teleports you elsewhere inside the SAME
        // cell. There is no cell change and therefore no loading screen to hide -- arming the
        // suppression/freeze machinery here would only cancel an unrelated fade.
        if (destCellID == playerHostCell->GetFormID()) {
            logger::info("[LF] Door 0x{:08X} teleports inside the same cell (0x{:08X}); leaving it vanilla",
                sourceDoorID, destCellID);
            return g_origDoorActivate(a_this, effectiveDoor, a_activatorRef, a_arg3, a_object, a_targetCount);
        }

        // Adaptive fallback: a cell we have already measured as slow gets the vanilla loading menu
        // instead of a long black gap. 0 disables the check.
        if (g_config.suppressMaxMs > 0) {
            LoadCellTimesOnce();
            std::uint32_t lastMs = 0;
            {
                std::lock_guard<std::mutex> lk(g_cellLoadMsMutex);
                const auto msIt = g_cellLoadMs.find(destCellID);
                if (msIt != g_cellLoadMs.end()) {
                    lastMs = msIt->second;
                }
            }
            if (lastMs > g_config.suppressMaxMs) {
                logger::info("[Tier2] cell 0x{:08X} last load measured {} ms (> uSuppressMaxMs {}); showing the vanilla menu",
                    destCellID, lastMs, g_config.suppressMaxMs);
                return g_origDoorActivate(a_this, effectiveDoor, a_activatorRef, a_arg3, a_object, a_targetCount);
            }
        }

        // Master routing: with pocket copies disabled (the default) every door takes the engine's
        // own transition, so navmesh, NPCs, AI packages, combat, lighting and quest events are
        // exactly vanilla. LoadingFlow's only job here is to hide or mask the loading menu.
        if (!g_config.usePocketCopies) {
            ArmLoadingMenuSuppression("real transition (pocket copies disabled)", IsAutoOrFramelessDoor(a_targetRef), destCellID);
            return g_origDoorActivate(a_this, effectiveDoor, a_activatorRef, a_arg3, a_object, a_targetCount);
        }

        const bool isFromInterior = isClonedPocketDoor || playerHostCell->IsInteriorCell();
        if (isFromInterior && !g_config.allowInteriorToInterior) {
            ArmLoadingMenuSuppression("interior->interior disabled", IsAutoOrFramelessDoor(a_targetRef), destCellID);
            return g_origDoorActivate(a_this, effectiveDoor, a_activatorRef, a_arg3, a_object, a_targetCount);
        }

        RE::NiPoint3 anchorXY = a_targetRef->GetPosition();
        const RE::NiPoint3 preEnterPos = player->GetPosition();
        const RE::NiPoint3 preEnterRot = player->GetAngle();
        const bool          isAutoCave = IsAutoOrFramelessDoor(a_targetRef);
        const std::uint32_t delayMs = isAutoCave ? (std::min)(g_config.doorOpenDelayMs, 120u) : g_config.doorOpenDelayMs;
        const RE::ObjectRefHandle doorHandle = a_targetRef->GetHandle();
        // L2: the cell the player is leaving (exterior, or the previous pocket's interior).
        RE::TESObjectCELL* fromCell = playerHostCell;

        // P0-1: the critical section below touches ONLY our own containers.
        // No engine calls, no `return`, and never finishEnter() inside it: finishEnter() takes this
        // same lock, so calling it under it (delayMs == 0) deadlocked the game thread outright, and
        // returning through the vanilla path while holding it re-entered the hooked Activate.
        RE::NiPoint3  destPocketPos{};
        RE::NiPoint3  destPocketRot = telRot;
        std::uint32_t slotIdx = 0;
        std::string   edidCopy;
        std::string   nameCopy;
        bool          fromBp = false;
        bool          pocketReady = false;
        {
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            if (isClonedPocketDoor && g_activeLoadingFlowID.load() != 0) {
                auto curIt = g_pockets.find(g_activeLoadingFlowID.load());
                if (curIt != g_pockets.end()) {
                    anchorXY.x = 0.5f * (curIt->second.boundMin.x + curIt->second.boundMax.x);
                    anchorXY.y = 0.5f * (curIt->second.boundMin.y + curIt->second.boundMax.y);
                }
            }
            // Build or retrieve the pocket immediately on T=0ms so its references are ready while the door cracks open!
            if (PocketInstance* pocket = GetOrCreatePocket(destCellID, destCell, playerHostCell, playerHostWorld, anchorXY, bpSnap)) {
                destPocketPos = telPos + pocket->pocketOrigin;
                slotIdx = pocket->slotIndex;
                edidCopy = pocket->interiorEditorID;
                nameCopy = pocket->interiorFullName;
                fromBp = pocket->builtFromBlueprint;
                // P1-2 / P1-3: remember where the player stood before the snap so Purge and Save
                // can put them back on solid ground instead of leaving them at Z=+26000.
                pocket->returnWorldPos = preEnterPos;
                pocket->returnWorldRot = preEnterRot;
                pocket->hasReturnWorldPos = true;
                g_lastExteriorReturnPos = preEnterPos;
                g_hasExteriorReturnPos.store(true);
                pocketReady = true;
            }
        }

        if (!pocketReady) {
            ArmLoadingMenuSuppression("pocket unavailable (quota / oversized cell)", IsAutoOrFramelessDoor(a_targetRef), destCellID);
            return g_origDoorActivate(a_this, effectiveDoor, a_activatorRef, a_arg3, a_object, a_targetCount);
        }

        auto finishEnter = [destCellID, destPocketPos, destPocketRot, doorHandle, isAutoCave, isFromInterior, slotIdx, edidCopy, nameCopy, fromBp, fromCell]() {
            if (auto* pl = RE::PlayerCharacter::GetSingleton()) {
                g_activeLoadingFlowID.store(destCellID);
                pl->SetPosition(destPocketPos, true);
                pl->SetRotationZ(destPocketRot.z);
            }
            {
                std::lock_guard<std::recursive_mutex> lk(g_mutex);
                auto pkIt = g_pockets.find(destCellID);
                if (pkIt != g_pockets.end()) {
                    TransferPocketActors(pkIt->second, true);
                }
            }
            // L2: tell quest scripts we just walked into that interior.
            FireFakeCellEvents(fromCell, LookupCellOrNull(destCellID), true);
            FinishDoorTransitionVisuals(doorHandle, isAutoCave);
            g_transitionInProgress.store(false);

            logger::info("[LF] >>> instant ENTER [{}|{}] -> Slot #{} cell=0x{:08X} ({}) src={} pos=({:.1f},{:.1f},{:.1f}) yaw={:.2f}",
                isFromInterior ? "Int->Int" : "Ext->Int",
                isAutoCave ? "Cave/Auto" : "Door/Trapdoor",
                slotIdx, destCellID, edidCopy,
                fromBp ? "BLUEPRINT" : "LIVE",
                destPocketPos.x, destPocketPos.y, destPocketPos.z, destPocketRot.z);

            if (g_config.showDebugNotifications) {
                const auto msg = std::format("LoadingFlow: {} [Slot #{}]",
                    nameCopy.empty() ? edidCopy : nameCopy, slotIdx);
                RE::DebugNotification(msg.c_str());
            }
        };

        BeginDoorTransitionVisuals(a_targetRef, isAutoCave);
        ++g_transitionCount;

        if (delayMs == 0) {
            finishEnter();
        } else {
            g_transitionInProgress.store(true);
            std::thread([delayMs, finishEnter]() mutable {
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask(std::move(finishEnter));
                } else {
                    g_transitionInProgress.store(false);
                }
            }).detach();
        }
        return true;
    }

    // a_includeContainer: the container-activate hook only exists to redirect cloned pocket
    // containers to the real ones. With pocket copies disabled (v0.6.0+ default) that hook has
    // nothing to do, so we do not install it at all -- one less engine vtable patched, one less
    // thing that can go wrong.
    void InstallVtableHooks(bool a_includeContainer = true)
    {
        constexpr std::size_t kActivateIdx = 0x37;
        constexpr std::uint32_t kPageExecuteReadWrite = 0x40;

        {
            REL::Relocation<std::uintptr_t> doorVtbl{ RE::VTABLE_TESObjectDOOR[0] };
            auto** vtbl = reinterpret_cast<void**>(doorVtbl.address());
            // v1.10.2: without this guard a second install captures OUR function as the original, and
            // HookedDoorActivate then calls itself -- stack overflow, instant CTD. kDataLoaded only
            // fires once today, but the guard costs nothing and removes a whole failure class.
            if (g_origDoorActivate != nullptr) {
                logger::warn("TESObjectDOOR::Activate hook already installed; skipping the second install");
            } else if (vtbl && IsExecutableAddress(vtbl[kActivateIdx])) {
                std::uint32_t oldProt = 0;
                if (REX::W32::VirtualProtect(&vtbl[kActivateIdx], sizeof(void*), kPageExecuteReadWrite, &oldProt)) {
                    g_origDoorActivate = reinterpret_cast<Activate_t>(vtbl[kActivateIdx]);
                    vtbl[kActivateIdx] = reinterpret_cast<void*>(&HookedDoorActivate);
                    REX::W32::VirtualProtect(&vtbl[kActivateIdx], sizeof(void*), oldProt, &oldProt);
                    logger::info("Installed TESObjectDOOR::Activate hook at vtable[0x37]=0x{:X} ({})",
                        reinterpret_cast<std::uintptr_t>(g_origDoorActivate),
                        IsAddressInTextSegment(reinterpret_cast<void*>(g_origDoorActivate)) ? "engine .text" : "chained");
                }
            } else {
                logger::error("TESObjectDOOR vtable[0x37] is not an executable address; hook aborted");
            }
        }

        if (!a_includeContainer) {
            logger::info("Container Activate hook skipped (pocket copies disabled)");
            return;
        }

        {
            REL::Relocation<std::uintptr_t> contVtbl{ RE::VTABLE_TESObjectCONT[0] };
            auto** vtbl = reinterpret_cast<void**>(contVtbl.address());
            if (vtbl && IsExecutableAddress(vtbl[kActivateIdx])) {
                std::uint32_t oldProt = 0;
                if (REX::W32::VirtualProtect(&vtbl[kActivateIdx], sizeof(void*), kPageExecuteReadWrite, &oldProt)) {
                    g_origContActivate = reinterpret_cast<Activate_t>(vtbl[kActivateIdx]);
                    vtbl[kActivateIdx] = reinterpret_cast<void*>(&HookedContActivate);
                    REX::W32::VirtualProtect(&vtbl[kActivateIdx], sizeof(void*), oldProt, &oldProt);
                    logger::info("Installed TESObjectCONT::Activate hook at vtable[0x37]=0x{:X}",
                        reinterpret_cast<std::uintptr_t>(g_origContActivate));
                }
            }
        }
    }

    // =========================================================================
    //  SKSE Menu Framework UI
    // =========================================================================
#if defined(LOADINGFLOW_WITH_MENU)
    std::atomic<float> g_origPostLoad{ -1.0f };
    std::atomic<int>   g_origQueueBudget{ -1 };
    std::atomic<bool>  g_origCaptured{ false };

    void ApplyEngineTimerTweaks();   // defined below; the menu can re-apply without a restart

    // v1.10.2: a slider fires on every mouse-move while dragging, and SaveConfig rewrites the whole
    // INI (tmp + rename). At 60 Hz that is 60 file writes per second on the game's main thread --
    // a visible stutter in the menu. Coalesce instead: mark "save soon" on every change, and flush
    // once the dragging has been quiet for a moment (checked from the menu's own draw call).
    std::atomic<std::int64_t> g_savePendingAt{ 0 };

    void SaveConfigSoon()
    {
        g_savePendingAt.store(NowMs() + 300, std::memory_order_relaxed);
    }

    void FlushPendingSave()
    {
        const auto at = g_savePendingAt.load(std::memory_order_relaxed);
        if (at == 0 || NowMs() < at) {
            return;
        }
        g_savePendingAt.store(0, std::memory_order_relaxed);
        SaveConfig();
        SaveCellTimes();
    }

    void __stdcall RenderMainMenu()
    {
        bool en = g_config.enabled;
        if (ImGuiMCP::Checkbox(L("menu.enable", "Enable LoadingFlow faster door loading"), &en)) {
            g_config.enabled = en;
            SaveConfigSoon();   // v1.3.2: never write the INI synchronously from a UI callback
        }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button(L("menu.btn.save", "Save Config"))) {
            SaveConfigSoon();   // v1.3.2: never write the INI synchronously from a UI callback
        }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button(L("menu.btn.reload", "Reload INI"))) {
            LoadConfig();
            ResolveExcludedCells();
        }

        ImGuiMCP::Separator();
        const RE::FormID activeID = g_activeLoadingFlowID.load();
        if (activeID != 0) {
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                L("menu.status.inPocket", "Current Status: INSIDE POCKET CELL (0x%08X) | Faster loads: %u"),
                activeID, g_transitionCount.load());
        } else {
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.7f, 0.85f, 1.0f, 1.0f),
                L("menu.status.normal", "Current Status: Normal World/Cell | Faster loads: %u"),
                g_transitionCount.load());
        }
        ImGuiMCP::Text(L("menu.blueprintStatus", "Auto-Indexed Blueprints: %zu interior cells / %zu doors (Hash: %08X, %s)"),
            g_bpCellCount.load(), g_bpDoorCount.load(),
            static_cast<std::uint32_t>(g_currentFingerprint.load() & 0xFFFFFFFFu),
            g_cacheHitOnStartup.load() ? "CACHE HIT" : "AUTO-BUILT");
        ImGuiMCP::SameLine();
        if (g_rebuildInProgress.load()) {
            ImGuiMCP::Text(L("menu.rebuild.busy", "Rebuilding... (background)"));
        } else if (ImGuiMCP::Button(L("menu.btn.rebuildIndex", "Force Rebuild Index"))) {
            // P1-4 / P0-1: never index 2500+ plugins on the render thread, and never do it while
            // holding the state mutex. Hand it to the SKSE task queue and keep it single-flight.
            bool expected = false;
            if (g_rebuildInProgress.compare_exchange_strong(expected, true)) {
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask([]() {
                        EnsureBlueprintsLoaded(true);
                        g_rebuildInProgress.store(false);
                    });
                } else {
                    g_rebuildInProgress.store(false);
                }
            }
        }

        ImGuiMCP::Separator();
        ImGuiMCP::Text(L("menu.sec.transitions", "-- Transitions --"));
        if (ImGuiMCP::Checkbox(L("menu.suppressLoadingMenu", "Tier2: hide the Loading Menu during real cell transitions (auto fallback)"), &g_config.suppressLoadingMenu)) {
            SaveConfigSoon();   // v1.3.2: never write the INI synchronously from a UI callback
        }
        if (g_config.suppressLoadingMenu) {
            // v1.3.0: the risk is now stated where the switch is, not only in the INI.
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.0f, 0.72f, 0.35f, 1.0f),
                L("menu.suppressRisk",
                    "Risk: while the menu is hidden, a cell change that fails or takes very long can leave the HUD off and player controls disabled. Turn this off if you get stuck after a door."));
        }
        {
            int fallbackMs = static_cast<int>(g_config.suppressTimeoutMs);
            if (ImGuiMCP::SliderInt(L("menu.fallbackTimeout",
                    "Give the Loading Menu back if a transition takes longer than this (ms)"),
                    &fallbackMs, 0, 60000, "%d ms")) {
                g_config.suppressTimeoutMs = static_cast<std::uint32_t>(fallbackMs);
                SaveConfigSoon();
            }
        }
        if (ImGuiMCP::Checkbox(L("menu.prewarmNearbyDoors", "Tier3: pre-warm models of nearby interiors while walking"), &g_config.prewarmNearbyDoors)) {
            SaveConfigSoon();   // v1.3.2: never write the INI synchronously from a UI callback
        }
        if (ImGuiMCP::Checkbox(L("menu.sneakVanilla", "Hold Sneak while activating a door to force the vanilla load screen"), &g_config.sneakForcesVanillaLoad)) {
            SaveConfigSoon();   // v1.3.2: never write the INI synchronously from a UI callback
        }
        if (ImGuiMCP::Checkbox(L("menu.debugNotify", "Show a top-left HUD notification on every transition (debug)"), &g_config.showDebugNotifications)) {
            SaveConfigSoon();   // v1.3.2: never write the INI synchronously from a UI callback
        }

        ImGuiMCP::Separator();
        // ---- speed vs stability (v1.6.1) ---------------------------------------
        // These are ENGINE timers: they do not just change how the transition looks, they decide how
        // long scripts and streaming get before the player takes control. Some users want the fastest
        // possible door; others hit crashes/clutter with heavy mod lists. So the trade-off is exposed
        // here instead of being hard-coded -- and "Apply now" really applies it (engine settings are
        // otherwise only written at load time).
        ImGuiMCP::Separator();
        ImGuiMCP::Text(L("menu.sec.timers", "-- Speed vs stability (engine timers) --"));
        {
            auto applyPreset = [](const char* a_name, float a_postLoad, int a_queueBudget, float a_fade) {
                g_config.postLoadUpdateTimeMS = a_postLoad;
                g_config.loadingQueuedPriorityBudgetMs = a_queueBudget;
                g_config.fadeToBlackFadeSeconds = a_fade;
                SaveConfigSoon();   // v1.3.2: never write the INI synchronously from a UI callback
                ApplyEngineTimerTweaks();
                logger::info("[Timers] preset '{}' applied: postLoad={} ms, queueBudget={} ms, fade={} s",
                    a_name, a_postLoad, a_queueBudget, a_fade);
            };
            if (ImGuiMCP::Button(L("menu.timers.stable", "Stable (restore the game's own values)"))) {
                // restore what the game shipped with (captured before our first write)
                g_config.postLoadUpdateTimeMS = g_origPostLoad.load();
                g_config.loadingQueuedPriorityBudgetMs = g_origQueueBudget.load();
                g_config.fadeToBlackFadeSeconds = 1.0f;   // vanilla fade-to-black, 1 s
                SaveConfigSoon();   // v1.3.2: never write the INI synchronously from a UI callback
                ApplyEngineTimerTweaks();
                logger::info("[Timers] preset 'stable' restored engine originals");
            }
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button(L("menu.timers.balanced", "Balanced (recommended)"))) {
                applyPreset("balanced", 800.0f, 20, 0.10f);
            }
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button(L("menu.timers.fast", "Fastest (may break heavy mods)"))) {
                applyPreset("fastest", 100.0f, 500, 0.10f);
            }
            ImGuiMCP::Text(g_config.postLoadUpdateTimeMS < 0.0f
                    ? "current: vanilla waits -- least risk, least speed-up"
                    : std::format("current: post-load wait {} ms, streaming budget {} ms", g_config.postLoadUpdateTimeMS, g_config.loadingQueuedPriorityBudgetMs).c_str());

            int postLoad = static_cast<int>(g_config.postLoadUpdateTimeMS);
            if (ImGuiMCP::SliderInt(L("menu.timers.postLoad", "Post-load script wait (ms, vanilla 2000, lower = faster but riskier)"), &postLoad, -1, 2000, "%d ms")) {
                g_config.postLoadUpdateTimeMS = static_cast<float>(postLoad);
                SaveConfigSoon();   // v1.3.2: never write the INI synchronously from a UI callback
            }
            int queueBudget = g_config.loadingQueuedPriorityBudgetMs;
            if (ImGuiMCP::SliderInt(L("menu.timers.queue", "Background streaming budget (ms, vanilla 20)"), &queueBudget, -1, 1000, "%d ms")) {
                g_config.loadingQueuedPriorityBudgetMs = queueBudget;
                SaveConfigSoon();   // v1.3.2: never write the INI synchronously from a UI callback
            }
            if (ImGuiMCP::Button(L("menu.timers.apply", "Apply engine timers now"))) {
                ApplyEngineTimerTweaks();
                logger::info("[Timers] re-applied from the menu (no restart needed)");
            }
            ImGuiMCP::Text(L("menu.timers.note", "Lower waits = faster doors, but scripts and mods get less time."));
        }

        FlushPendingSave();

        ImGuiMCP::Separator();
        ImGuiMCP::Text(L("menu.sec.visual", "-- Door transition look --"));
        {
            int crossFade = static_cast<int>(g_config.crossFadeMs);
            if (ImGuiMCP::SliderInt(L("menu.visual.crossFade", "Dissolve into the new cell (ms) -- the length of the dissolve itself"), &crossFade, 0, 800, "%d ms")) {
                g_config.crossFadeMs = static_cast<std::uint32_t>(std::clamp(crossFade, 0, 800));
                SaveConfigSoon();
            }
            int holdMs = static_cast<int>(g_config.crossFadeHoldMs);
            if (ImGuiMCP::SliderInt(L("menu.visual.hold", "Hold the old picture at the cut (ms) -- covers the engine's rotation"), &holdMs, 0, 6000, "%d ms")) {
                g_config.crossFadeHoldMs = static_cast<std::uint32_t>(std::clamp(holdMs, 0, 6000));
                SaveConfigSoon();
            }
            int blurPx = static_cast<int>(lroundf(g_config.deadBlurPx));
            if (ImGuiMCP::SliderInt(L("menu.visual.blur", "Frosted-glass blur of the frozen picture (pixels, 0 = off)"), &blurPx, 0, 120, "%d px")) {
                g_config.deadBlurPx = static_cast<float>(blurPx);
                SaveConfigSoon();
            }
            int rampMs = static_cast<int>(g_config.deadZoomMs);
            if (ImGuiMCP::SliderInt(L("menu.visual.ramp", "How long the blur takes to build up (ms)"), &rampMs, 200, 4000, "%d ms")) {
                g_config.deadZoomMs = static_cast<std::uint32_t>(std::clamp(rampMs, 200, 4000));
                SaveConfigSoon();
            }
            int pushIn = static_cast<int>(lroundf(g_config.deadZoomAmount * 100.0f));
            if (ImGuiMCP::SliderInt(L("menu.visual.pushIn", "Push-in instead of blur (percent, 0 = off)"), &pushIn, 0, 15, "%d %%")) {
                g_config.deadZoomAmount = static_cast<float>(pushIn) / 100.0f;
                SaveConfigSoon();
            }
            ImGuiMCP::Text(L("menu.visual.note", "Applies from the next door. No restart needed."));
        }

        ImGuiMCP::Separator();
        ImGuiMCP::Text(L("menu.sec.noBlack", "-- Frozen picture (no-black) --"));
        if (ImGuiMCP::Checkbox(L("menu.noBlack", "Keep the last picture on screen instead of the engine's fade to black"), &g_config.noBlackTransition)) {
            // v1.3.2: this used to only write the INI. A hold that was already armed stayed armed,
            // so unticking the box could leave the screen frozen with nothing on the UI to undo it.
            if (!g_config.noBlackTransition) {
                presentfreeze::g_holdActive.store(false);
            }
            SaveConfigSoon();   // v1.3.2: never write the INI synchronously from a UI callback
        }
        if (g_config.noBlackTransition) {
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.0f, 0.72f, 0.35f, 1.0f),
                L("menu.noBlackRisk",
                    "Off by default since 1.3.0: this is what makes a slow load look like a freeze. It is also the setting that hid a transition softlock."));
        }
        if (ImGuiMCP::Checkbox(L("menu.noBlackManual", "  ... for manual doors"), &g_config.noBlackOnlyForManualDoors)) {
            if (!g_config.noBlackOnlyForManualDoors) {
                presentfreeze::g_holdActive.store(false);
            }
            SaveConfigSoon();   // v1.3.2: never write the INI synchronously from a UI callback
        }
        if (ImGuiMCP::Checkbox(L("menu.noBlackWalk", "  ... for walk-through doors"), &g_config.noBlackForWalkThrough)) {
            if (!g_config.noBlackForWalkThrough) {
                presentfreeze::g_holdActive.store(false);
            }
            SaveConfigSoon();   // v1.3.2: never write the INI synchronously from a UI callback
        }
        {
            int holdCut = static_cast<int>(g_config.holdAtCutMs);
            if (ImGuiMCP::SliderInt(L("menu.holdAtCut",
                    "Freeze the last frame after the cut (ms, 0 = do not freeze at all)"),
                    &holdCut, 0, 600, "%d ms")) {
                g_config.holdAtCutMs = static_cast<std::uint32_t>(holdCut);
                if (holdCut == 0) {
                    // apply immediately: an already-armed hold must not keep the screen frozen
                    presentfreeze::g_holdActive.store(false);
                }
                SaveConfigSoon();
            }
        }
        {
            int fadeCs = static_cast<int>(g_config.fadeToBlackFadeSeconds * 100.0f + 0.5f);
            if (ImGuiMCP::SliderInt(L("menu.timers.fadeToBlack",
                    "Engine fade to/from black (1/100 s; 10 = the old 0.10 s snap, 40 = smooth)"),
                    &fadeCs, 5, 100, "%d / 100 s")) {
                g_config.fadeToBlackFadeSeconds = static_cast<float>(fadeCs) / 100.0f;
                SaveConfigSoon();
            }
        }

        if (ImGuiMCP::Checkbox(L("menu.globalSuppress", "Global: suppress the menu for non-door transitions too"), &g_config.globalMenuSuppression)) {
            SaveConfigSoon();   // v1.3.2: never write the INI synchronously from a UI callback
        }
    }

    void RegisterMenu()
    {
        if (!SKSEMenuFramework::IsInstalled()) {
            logger::warn("SKSEMenuFramework.dll not found; menu not registered (plugin still works)");
            return;
        }
        SKSEMenuFramework::SetSection("LoadingFlow");
        SKSEMenuFramework::AddSectionItem("LoadingFlow", RenderMainMenu);
        logger::info("Registered SKSEMenuFramework section: LoadingFlow");
        // ---- v1.3.5 persistent-state audit -----------------------------------------------------
        // Written because the question "did the last few versions bake some setting into the game?"
        // is answerable, and guessing at it wastes days. Everything this plugin does to the game is
        // one of two things:
        //   1. an in-memory write to a GameSetting (never written back to any INI), or
        //   2. a vtable-slot pointer swap (also process memory).
        // Neither survives the process. The only file we ever create or modify is our own INI.
        // The timestamps are printed by whoever wants them with `ls -la` outside the game; this log
        // states what to compare, so a future reader does not have to re-derive it.
        logger::info("[Audit] files this plugin writes: Data/SKSE/Plugins/LoadingFlow.ini, LoadingFlow_CellTimes.ini, LoadingFlow_Blueprints.bin");
        logger::info("[Audit] game INIs: never written -- engine settings are patched in memory only");
        logger::info("[Audit] vtable patches are process-local; exiting the game restores the originals");
        logger::info("[Audit] no registry keys, no save-game data, no Papyrus scripts, no BSA/ESP edits");
        logger::info("[Audit] visual mode {} / present1 bypass {} / submit route {} / present audit {} -- with mode 0 the Present hook draws nothing",
            g_config.visualMode, g_config.usePresent1ForVisual ? 1 : 0, g_config.submitRoute,
            g_config.presentAudit ? 1 : 0);
    }
#else
    void RegisterMenu() {}
#endif

    // Engine transition timers live in the INI-backed setting collections under names of the form
    // "name:Section". This CommonLib version exposes only the raw BSSimpleList, so scan it.
    RE::Setting* FindIniSetting(const char* a_name)
    {
        auto scan = [&](RE::SettingCollectionList<RE::Setting>* a_coll) -> RE::Setting* {
            if (!a_coll) {
                return nullptr;
            }
            for (auto* s : a_coll->settings) {
                if (s && s->GetName() && std::strcmp(s->GetName(), a_name) == 0) {
                    return s;
                }
            }
            return nullptr;
        };
        if (auto* s = scan(RE::INISettingCollection::GetSingleton())) {
            return s;
        }
        return scan(RE::INIPrefSettingCollection::GetSingleton());
    }

    const char* SettingTypeName(RE::Setting::Type a_type)
    {
        switch (a_type) {
        case RE::Setting::Type::kBool: return "Bool";
        case RE::Setting::Type::kFloat: return "Float";
        case RE::Setting::Type::kSignedInteger: return "SInt";
        case RE::Setting::Type::kUnsignedInteger: return "UInt";
        case RE::Setting::Type::kColor: return "Color";
        case RE::Setting::Type::kString: return "String";
        default: return "Unknown";
        }
    }

    void SetIniFloat(const char* a_name, float a_value, bool a_onlyReport = false)
    {
        auto* s = FindIniSetting(a_name);
        if (!s) {
            logger::warn("[Speed] {} NOT FOUND (left unchanged)", a_name);
            return;
        }
        // NEVER write a union member without checking the setting's real type: writing data.b into
        // an integer setting only overwrites one byte and leaves garbage in the rest, which can make
        // the engine act on a nonsense value.
        if (s->GetType() != RE::Setting::Type::kFloat) {
            logger::warn("[Speed] {} is {} not Float (left unchanged)", a_name, SettingTypeName(s->GetType()));
            return;
        }
        const float old = s->data.f;
        if (a_onlyReport || a_value < 0.0f) {
            logger::info("[Speed] {} = {:.4f} (left at vanilla)", a_name, old);
            return;
        }
        s->data.f = a_value;
        logger::info("[Speed] {} {:.4f} -> {:.4f}", a_name, old, a_value);
    }

    void SetIniInt(const char* a_name, int a_value, bool a_onlyReport = false)
    {
        auto* s = FindIniSetting(a_name);
        if (!s) {
            logger::warn("[Speed] {} NOT FOUND (left unchanged)", a_name);
            return;
        }
        const auto t = s->GetType();
        if (t != RE::Setting::Type::kSignedInteger && t != RE::Setting::Type::kUnsignedInteger) {
            logger::warn("[Speed] {} is {} not integer (left unchanged)", a_name, SettingTypeName(t));
            return;
        }
        const auto old = s->data.i;
        if (a_onlyReport || a_value < 0) {
            logger::info("[Speed] {} = {} (left at vanilla)", a_name, old);
            return;
        }
        s->data.i = a_value;
        logger::info("[Speed] {} {} -> {}", a_name, old, a_value);
    }

    // Try a bare name against several known INI sections, in order. SkyrimSE.exe contains
    // "iPreloadSizeLimit" exists in SkyrimSE.exe but its INI section is not documented, so probe.
    // (The matching ON/OFF switch bPreloadLinkedInteriors was removed -- it hangs save loading.)
    RE::Setting* FindIniSettingAuto(const char* a_name, char* a_outName, std::size_t a_outSize)
    {
        static const char* kSections[] = { "General", "BackgroundLoad", "Display", "Interface",
                                           "Papyrus", "Archive", "Audio", "Combat", "Menu" };
        for (const char* sec : kSections) {
            std::snprintf(a_outName, a_outSize, "%s:%s", a_name, sec);
            if (auto* s = FindIniSetting(a_outName)) {
                return s;
            }
        }
        std::snprintf(a_outName, a_outSize, "%s", a_name);
        return FindIniSetting(a_outName);
    }

    void SetIniIntAuto(const char* a_name, int a_value)
    {
        char full[160] = {};
        auto* s = FindIniSettingAuto(a_name, full, sizeof(full));
        if (!s) {
            logger::warn("[Preload] {} NOT FOUND in any known section (left unchanged)", a_name);
            return;
        }
        const auto t = s->GetType();
        if (t != RE::Setting::Type::kSignedInteger && t != RE::Setting::Type::kUnsignedInteger) {
            logger::warn("[Preload] {} is {} (not integer) -- left unchanged", full, SettingTypeName(t));
            return;
        }
        const auto old = s->data.i;
        if (a_value < 0) {
            logger::info("[Preload] {} = {} (left at vanilla)", full, old);
            return;
        }
        s->data.i = a_value;
        logger::info("[Preload] {} {} -> {}", full, old, a_value);
    }

    // Shortens the engine's own transition timers. The big one is fFadeToBlackFadeSeconds: our log
    // measured a very stable ~1.0 s between pressing E and the engine even asking for the
    // LoadingMenu, and that value IS that setting's vanilla default.
    // v1.6.2: remember what the game actually shipped, so "Stable" can RESTORE it. Before this,
    // Stable wrote -1 ("leave alone"), which changed nothing after a previous preset had already
    // written 100/500 -- that is why the menu felt like it had no effect.
    void CaptureEngineTimerOriginals()
    {
        if (g_origCaptured.exchange(true)) {
            return;
        }
        if (auto* s = FindIniSetting("fPostLoadUpdateTimeMS:Papyrus"); s && s->GetType() == RE::Setting::Type::kFloat) {
            g_origPostLoad.store(s->GetFloat());
        }
        if (auto* s = FindIniSetting("iPostProcessMillisecondsLoadingQueuedPriority:BackgroundLoad"); s && s->GetType() == RE::Setting::Type::kSignedInteger) {
            g_origQueueBudget.store(static_cast<int>(s->data.i));
        }
        logger::info("[Timers] captured engine originals: postLoad={} ms, queueBudget={} ms",
            g_origPostLoad.load(), g_origQueueBudget.load());
    }

    void ApplyEngineTimerTweaks()
    {
        CaptureEngineTimerOriginals();
        const bool on = g_config.tweakEngineTimers;
        logger::info("[Speed] engine transition timers ({}):", on ? "applying" : "report only");
        SetIniFloat("fFadeToBlackFadeSeconds:Interface", on ? g_config.fadeToBlackFadeSeconds : -1.0f, !on);
        SetIniFloat("fNormalDoorFadeSecs:General", on ? g_config.normalDoorFadeSecs : -1.0f, !on);
        SetIniFloat("fNormalDoorFadeWait:General", on ? g_config.normalDoorFadeWait : -1.0f, !on);
        SetIniFloat("fAutoDoorFadeSecs:General", on ? g_config.autoDoorFadeSecs : -1.0f, !on);
        SetIniFloat("fMinSecondsForLoadFadeIn:Interface", on ? g_config.minSecondsForLoadFadeIn : -1.0f, !on);
        SetIniFloat("fPostLoadUpdateTimeMS:Papyrus", on ? g_config.postLoadUpdateTimeMS : -1.0f, !on);
        SetIniFloat("fActorTeleportFadeSeconds:General", on ? g_config.actorTeleportFadeSeconds : -1.0f, !on);
        SetIniFloat("fFastTravelFadeSecs:General", on ? g_config.fastTravelFadeSecs : -1.0f, !on);
        SetIniInt("iPostProcessMillisecondsLoadingQueuedPriority:BackgroundLoad",
            on ? g_config.loadingQueuedPriorityBudgetMs : -1, !on);
        // Engine-side cell preloading. Both settings exist in SkyrimSE.exe; enabling them is the
        // only remaining lever on the load phase itself (our own measurements show the load phase
        // is workload-bound, and model-level pre-warming did nothing).
        SetIniIntAuto("iPreloadSizeLimit", g_config.preloadSizeLimit);
        logger::info("[Timers] effective now: postLoad={} ms, queueBudget={} ms, fadeToBlack={} s",
            g_config.postLoadUpdateTimeMS, g_config.loadingQueuedPriorityBudgetMs, g_config.fadeToBlackFadeSeconds);
    }

    // ============================ v1.13.0 UI-layer cover ============================
    // Why this exists: drawing at Present time (our earlier visual layer) reroutes the frames that
    // carry our pixels through slot 22. That starves the upscaler / frame-generation chain, and the
    // frame handed back afterwards is broken (a 16:9 quadrant of un-upscaled world buffer). A cover
    // that lives in the UI layer is composited by the engine AFTER upscaling, so it never touches the
    // present sequence at all.
    //
    // Mechanics:
    //   * Data/Interface/LoadingFlowCover.swf  -- one fullscreen quad, instance name "cover"
    //   * this IMenu subclass                  -- hosts the movie; the engine advances/displays it
    //   * SetVariable("cover._alpha", 0..100)  -- the on/off switch, a plain dotted path
    // ---- v1.13.2 runtime AS2 cover (iCoverTest = 3) --------------------------------
    // Why this exists: our hand-built 90-byte SWF is accepted by FFDec but the ENGINE's Scaleform
    // refused to load it (menu instantiated, uiMovie stayed null). Drawing at runtime into a movie
    // the engine already loaded sidesteps SWF authoring completely:
    //   createEmptyMovieClip -> beginFill -> moveTo/lineTo x4 -> endFill, then drive _alpha.
    // Host menu: Fader Menu first -- it is kAlwaysOpen and the engine keeps drawing it during a
    // transition (that is exactly what it is for). HUD Menu is the fallback.
    // ---- v1.13.4 runtime AS2 cover, audited -----------------------------------------
    // v1.13.3 findings fixed here:
    //   1. every drawing Invoke() return value was discarded -> a failed beginFill/lineTo left an
    //      EMPTY clip that still accepted _alpha=100. That is indistinguishable from "painted but
    //      not rendered". All steps are now checked and logged.
    //   2. we drew in absolute stage coordinates without setting the clip's own _x/_y -> if the host
    //      movie's origin is not (0,0) the rectangle lands off-screen. Now: draw local (0,0)-(w,h)
    //      and position the clip explicitly.
    //   3. clips were dropped at alpha 0 and rebuilt next door. Now kept and reused.
    //   4. no read-back after setting _alpha -> could not tell "not set" from "set but not drawn".
    //      Now reads the property back and logs it.
    struct CoverClip
    {
        RE::GFxValue clip;
        RE::GFxMovieView* movie = nullptr;
        const char*  host = "";
    };
    std::vector<CoverClip> g_coverClips;

    // v1.16.0: 把"最后一帧"压成 kCoverW x kCoverH 的色块网格。
    // 手段:整帧 CopyResource 到一块同格式的 staging,再在 CPU 上按网格做盒式平均。
    // 缩小本身就是模糊 —— 之后 Scaleform 端再用双线性插值放大,得到磨玻璃观感。
    bool SampleLastFrameGrid()
    {
        auto* sc = static_cast<IDXGISwapChain*>(g_lastSwapChain.load());
        if (!sc) {
            logger::info("[CoverTex] no swapchain recorded yet");
            return false;
        }
        using namespace RE::BSGraphics;
        auto* rd = Renderer::GetRendererData();
        auto* device = rd ? reinterpret_cast<ID3D11Device*>(rd->forwarder) : nullptr;
        auto* ctx = rd ? reinterpret_cast<ID3D11DeviceContext*>(rd->context) : nullptr;
        if (!device || !ctx) {
            return false;
        }
        const std::uint32_t idx = presentfreeze::CurrentBackBufferIndex(sc);
        ID3D11Texture2D* back = nullptr;
        if (FAILED(sc->GetBuffer(idx, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) {
            return false;
        }
        D3D11_TEXTURE2D_DESC desc{};
        back->GetDesc(&desc);

        static ID3D11Texture2D* s_staging = nullptr;
        static std::uint32_t s_w = 0, s_h = 0, s_fmt = 0;
        if (!s_staging || s_w != desc.Width || s_h != desc.Height || s_fmt != static_cast<std::uint32_t>(desc.Format)) {
            if (s_staging) {
                s_staging->Release();
                s_staging = nullptr;
            }
            D3D11_TEXTURE2D_DESC sd = desc;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            if (FAILED(device->CreateTexture2D(&sd, nullptr, &s_staging)) || !s_staging) {
                back->Release();
                logger::info("[CoverTex] staging creation failed");
                return false;
            }
            s_w = desc.Width;
            s_h = desc.Height;
            s_fmt = static_cast<std::uint32_t>(desc.Format);
        }
        ctx->CopyResource(s_staging, back);
        back->Release();

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(ctx->Map(s_staging, 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) {
            logger::info("[CoverTex] map failed");
            return false;
        }
        const auto* base = static_cast<const std::uint8_t*>(mapped.pData);
        const bool tenBit = (desc.Format == 24);
        for (int gy = 0; gy < kCoverH; ++gy) {
            const std::uint32_t y0 = static_cast<std::uint32_t>(static_cast<std::uint64_t>(gy) * s_h / kCoverH);
            const std::uint32_t y1 = static_cast<std::uint32_t>(static_cast<std::uint64_t>(gy + 1) * s_h / kCoverH);
            for (int gx = 0; gx < kCoverW; ++gx) {
                const std::uint32_t x0 = static_cast<std::uint32_t>(static_cast<std::uint64_t>(gx) * s_w / kCoverW);
                const std::uint32_t x1 = static_cast<std::uint32_t>(static_cast<std::uint64_t>(gx + 1) * s_w / kCoverW);
                std::uint64_t r = 0, g = 0, b = 0, n = 0;
                // 每格取 4x4 个采样点求平均,够稳且便宜
                for (int sy = 0; sy < 4; ++sy) {
                    const std::uint32_t py = y0 + (y1 > y0 ? (y1 - y0) * sy / 4 : 0);
                    if (py >= s_h) continue;
                    const auto* row = base + static_cast<std::size_t>(py) * mapped.RowPitch;
                    for (int sx = 0; sx < 4; ++sx) {
                        const std::uint32_t px = x0 + (x1 > x0 ? (x1 - x0) * sx / 4 : 0);
                        if (px >= s_w) continue;
                        if (tenBit) {
                            std::uint32_t v = 0;
                            std::memcpy(&v, row + static_cast<std::size_t>(px) * 4, 4);
                            r += (v >> 20) & 0x3FF;
                            g += (v >> 10) & 0x3FF;
                            b += v & 0x3FF;
                        } else {
                            r += row[static_cast<std::size_t>(px) * 4 + 0];
                            g += row[static_cast<std::size_t>(px) * 4 + 1];
                            b += row[static_cast<std::size_t>(px) * 4 + 2];
                        }
                        ++n;
                    }
                }
                if (n == 0) n = 1;
                std::uint32_t rr, gg, bb;
                if (tenBit) {
                    rr = static_cast<std::uint32_t>(r / n * 255 / 1023);
                    gg = static_cast<std::uint32_t>(g / n * 255 / 1023);
                    bb = static_cast<std::uint32_t>(b / n * 255 / 1023);
                } else {
                    rr = static_cast<std::uint32_t>(r / n);
                    gg = static_cast<std::uint32_t>(g / n);
                    bb = static_cast<std::uint32_t>(b / n);
                }
                g_coverPx[gy * kCoverW + gx] = (rr << 16) | (gg << 8) | bb;
            }
        }
        ctx->Unmap(s_staging, 0);
        g_coverPxValid = true;
        logger::info("[CoverTex] sampled {}x{} grid from {}x{} fmt={}", kCoverW, kCoverH, s_w, s_h,
            static_cast<int>(desc.Format));
        return true;
    }

    bool PaintCoverInto(RE::IMenu* a_menu, const char* a_name)
    {
        if (!a_menu || !a_menu->uiMovie) {
            logger::info("[CoverUi] host '{}': no movie", a_name);
            return false;
        }
        if (a_menu->CustomRendering()) {
            logger::info("[CoverUi] host '{}': SKIPPED (kCustomRendering -- engine never draws its movie)",
                a_name);
            return false;
        }
        auto* movie = a_menu->uiMovie.get();
        const auto rect = movie->GetVisibleFrameRect();
        double ox = rect.left;
        double oy = rect.top;
        double w  = rect.right - rect.left;
        double h  = rect.bottom - rect.top;
        if (!(w > 0.0) || !(h > 0.0)) { ox = 0.0; oy = 0.0; w = 1920.0; h = 1080.0; }
        if (w < 1280.0 || h < 720.0) { ox = -512.0; oy = -512.0; w = 2560.0; h = 2560.0; }

        RE::GFxValue nameArg;
        movie->CreateString(&nameArg, "lfCover");
        RE::GFxValue depthArg;
        depthArg.SetNumber(987654.0);
        auto createArgs = std::array{ nameArg, depthArg };
        RE::GFxValue clip;
        const bool created = movie->Invoke("createEmptyMovieClip", &clip, createArgs.data(), 2);
        if (!created || !clip.IsDisplayObject()) {
            logger::info("[CoverUi] host '{}': createEmptyMovieClip failed (created={} displayObj={})",
                a_name, created ? "true" : "false", clip.IsDisplayObject() ? "true" : "false");
            return false;
        }

        // 画在 clip 的本地空间 (0,0)-(w,h)，再把 clip 摆到 rect 原点 —— 不依赖宿主的坐标原点。
        // v1.14.1: 用洋红而不是黑色。教训:加载界面的背景本来就是大片黑,用黑去盖它
        // 无论生效与否肉眼都分辨不出 —— 前几轮"没生效"的判读因此完全不可信。
        // 洋红只在这一步当"探针色"用,证明通路;下一步立即换成磨玻璃纹理。
        // ---- v1.16.0 frosted cover: BitmapData + smooth upscale first, mosaic as fallback ----
        const bool havePixels = SampleLastFrameGrid();
        bool bFill = false;
        const char* style = "solid";

        auto drawQuad = [&](double qx0, double qy0, double qx1, double qy1) {
            auto mv = std::array{ RE::GFxValue(), RE::GFxValue() };
            mv[0].SetNumber(qx0); mv[1].SetNumber(qy0);
            clip.Invoke("moveTo", nullptr, mv.data(), 2);
            const double q[3][2] = { { qx1, qy0 }, { qx1, qy1 }, { qx0, qy1 } };
            for (const auto& pt : q) {
                auto ln = std::array{ RE::GFxValue(), RE::GFxValue() };
                ln[0].SetNumber(pt[0]); ln[1].SetNumber(pt[1]);
                clip.Invoke("lineTo", nullptr, ln.data(), 2);
            }
            clip.Invoke("endFill", nullptr);
        };

        if (havePixels) {
            RE::GFxValue bwv; bwv.SetNumber(static_cast<double>(kCoverW));
            RE::GFxValue bhv; bhv.SetNumber(static_cast<double>(kCoverH));
            auto bdArgs = std::array{ bwv, bhv };
            RE::GFxValue bd;
            movie->CreateObject(&bd, "flash.display.BitmapData", bdArgs.data(), 2);
            const bool bdObj = bd.IsObject();
            logger::info("[CoverTex] CreateObject BitmapData -> IsObject={} type={}",
                bdObj ? "true" : "false", static_cast<int>(bd.GetType()));
            bool ok = bdObj;
            int pixDone = 0;
            if (ok) {
                for (int gy = 0; gy < kCoverH && ok; ++gy) {
                    for (int gx = 0; gx < kCoverW; ++gx) {
                        auto px = std::array{ RE::GFxValue(), RE::GFxValue(), RE::GFxValue() };
                        px[0].SetNumber(static_cast<double>(gx));
                        px[1].SetNumber(static_cast<double>(gy));
                        px[2].SetNumber(static_cast<double>(g_coverPx[gy * kCoverW + gx]));
                        if (!bd.Invoke("setPixel", nullptr, px.data(), 3)) { ok = false; break; }
                        ++pixDone;
                    }
                }
            }
            logger::info("[CoverTex] setPixel done {} / {}", pixDone, kCoverW * kCoverH);
            RE::GFxValue mat;
            if (ok) {
                movie->CreateObject(&mat, "flash.geom.Matrix", nullptr, 0);
                ok = mat.IsObject();
            }
            if (ok) {
                RE::GFxValue a, d;
                a.SetNumber(w / static_cast<double>(kCoverW));
                d.SetNumber(h / static_cast<double>(kCoverH));
                mat.SetMember("a", a);
                mat.SetMember("d", d);
                RE::GFxValue bf[4];
                bf[0] = bd;
                bf[1] = mat;
                bf[2].SetBoolean(false);
                bf[3].SetBoolean(true);
                // 级联 1: beginBitmapFill 四参(含 smoothing)
                bFill = clip.Invoke("beginBitmapFill", nullptr, bf, 4);
                logger::info("[CoverTex] beginBitmapFill(4 args, smooth) -> {}", bFill ? "ok" : "fail");
                if (bFill) {
                    drawQuad(0.0, 0.0, w, h);
                    style = "bitmap-smooth";
                }
                if (!bFill) {
                    // 级联 2: 三参(不带 smoothing)
                    bFill = clip.Invoke("beginBitmapFill", nullptr, bf, 3);
                    logger::info("[CoverTex] beginBitmapFill(3 args) -> {}", bFill ? "ok" : "fail");
                    if (bFill) {
                        drawQuad(0.0, 0.0, w, h);
                        style = "bitmap";
                    }
                }
                if (!bFill) {
                    // 级联 3: attachBitmap(把位图直接贴到 clip 上,绕过绘制 API)
                    auto at = std::array{ bd, RE::GFxValue() };
                    at[1].SetNumber(1.0);
                    const bool attached = clip.Invoke("attachBitmap", nullptr, at.data(), 2);
                    logger::info("[CoverTex] attachBitmap -> {}", attached ? "ok" : "fail");
                    if (attached) {
                        RE::GFxValue sx, sy;
                        sx.SetNumber(w / static_cast<double>(kCoverW) * 100.0);
                        sy.SetNumber(h / static_cast<double>(kCoverH) * 100.0);
                        clip.SetMember("_xscale", sx);
                        clip.SetMember("_yscale", sy);
                        style = "bitmap-attached";
                        bFill = true;
                    }
                }
            }
            if (!bFill) {
                const double cw = w / kCoverW;
                const double ch = h / kCoverH;
                int drawn = 0;
                for (int gy = 0; gy < kCoverH; ++gy) {
                    for (int gx = 0; gx < kCoverW; ++gx) {
                        RE::GFxValue col, alp;
                        col.SetNumber(static_cast<double>(g_coverPx[gy * kCoverW + gx]));
                        alp.SetNumber(100.0);
                        auto fa = std::array{ col, alp };
                        if (!clip.Invoke("beginFill", nullptr, fa.data(), 2)) { continue; }
                        drawQuad(gx * cw, gy * ch, (gx + 1) * cw, (gy + 1) * ch);
                        ++drawn;
                    }
                }
                style = "mosaic";
                bFill = drawn > 0;
                logger::info("[CoverTex] bitmap path unavailable, fell back to {} mosaic cells", drawn);
            }
        } else {
            RE::GFxValue colour, opacity;
            colour.SetNumber(1315860.0);
            opacity.SetNumber(100.0);
            auto fa = std::array{ colour, opacity };
            bFill = clip.Invoke("beginFill", nullptr, fa.data(), 2);
            if (bFill) {
                drawQuad(0.0, 0.0, w, h);
            }
        }

        RE::GFxValue zero; zero.SetNumber(0.0);
        clip.SetMember("_alpha", zero);
        RE::GFxValue cx; cx.SetNumber(ox);
        RE::GFxValue cy; cy.SetNumber(oy);
        const bool bPos = clip.SetMember("_x", cx) && clip.SetMember("_y", cy);

        g_coverClips.push_back(CoverClip{ clip, movie, a_name });
        logger::info("[CoverUi] host '{}': clip painted MAGENTA rect origin=({:.0f},{:.0f}) size={:.0f}x{:.0f}  "
                     "style={} fill={} position={}",
            a_name, ox, oy, w, h, style, bFill ? "ok" : "FAIL", bPos ? "ok" : "FAIL");
        return true;
    }

    void PaintCoverEverywhere()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            logger::warn("[CoverUi] no UI singleton");
            return;
        }
        g_coverClips.clear();
        static constexpr const char* kHosts[] = { "HUD Menu", "Cursor Menu", "Loading Menu", "Fader Menu" };
        for (auto* name : kHosts) {
            auto menu = ui->GetMenu(name);
            if (menu) {
                PaintCoverInto(menu.get(), name);
            } else {
                logger::info("[CoverUi] host '{}': menu not found", name);
            }
        }
        if (g_coverClips.empty()) {
            logger::warn("[CoverUi] no eligible host menu produced a clip");
        }
    }

    void SetRuntimeCoverAlpha(float a_alpha01)
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            logger::warn("[CoverUi] no UI singleton");
            return;
        }
        // 崩溃防护:菜单可能被引擎销毁/重建,存下来的 movie 指针会悬空。
        // 每次按名字重新取菜单,并与缓存的 movie 比对;不一致就重新画。
        bool stale = g_coverClips.empty();
        for (const auto& c : g_coverClips) {
            auto menu = ui->GetMenu(c.host);
            if (!menu || !menu->uiMovie || menu->uiMovie.get() != c.movie) {
                stale = true;
                break;
            }
        }
        if (stale) {
            PaintCoverEverywhere();
        }

        RE::GFxValue v;
        v.SetNumber(static_cast<double>(a_alpha01) * 100.0);
        int okCount = 0;
        int readCount = 0;
        for (auto& c : g_coverClips) {
            auto menu = ui->GetMenu(c.host);
            if (!menu || !menu->uiMovie || menu->uiMovie.get() != c.movie) {
                continue;   // 指针失效:跳过,绝不解引用
            }
            if (c.clip.SetMember("_alpha", v)) {
                ++okCount;
            }
            RE::GFxValue back;
            if (c.movie->GetVariable(&back, "lfCover._alpha") && back.IsNumber() &&
                std::abs(back.GetNumber() - static_cast<double>(a_alpha01) * 100.0) < 0.5) {
                ++readCount;
            }
        }
        logger::info("[CoverUi] runtime cover _alpha = {:.0f}: set {}/{}, read-back matched {}/{}",
            a_alpha01 * 100.0, okCount, g_coverClips.size(), readCount, g_coverClips.size());
    }

    class CoverMenu final : public RE::IMenu
    {
    public:
        static constexpr std::string_view MENU_NAME = "LoadingFlowCover";
        static constexpr std::string_view FILE_NAME = "LoadingFlowCover";

        CoverMenu()
        {
            menuFlags.set(RE::UI_MENU_FLAGS::kAlwaysOpen,
                RE::UI_MENU_FLAGS::kAllowSaving,
                RE::UI_MENU_FLAGS::kAdvancesUnderPauseMenu,
                RE::UI_MENU_FLAGS::kRendersUnderPauseMenu);
            depthPriority = 6;   // above HUD, low enough to stay under real menus
            inputContext = RE::UserEvents::INPUT_CONTEXT_ID::kNone;

            // v1.16.3: 引擎**不会**为 UI::Register 注册的自定义菜单加载 movie(实测 uiMovie 一路 null,
            // 给 SWF 补 Menu_mc 也没用)。必须自己调 BSScaleformManager::LoadMovie。
            auto* mgr = RE::BSScaleformManager::GetSingleton();
            if (!mgr) {
                logger::warn("[CoverUi] no BSScaleformManager; cannot load the menu movie");
                return;
            }
            const std::string file{ CoverMenu::FILE_NAME };
            const bool loaded = mgr->LoadMovie(this, uiMovie, file.c_str(),
                RE::GFxMovieView::ScaleModeType::kShowAll, 0.0f);
            logger::info("[CoverUi] explicit LoadMovie('{}') -> ok={} uiMovie={}",
                file, loaded ? "true" : "false", uiMovie ? "SET" : "NULL");

            // v1.16.7: 引擎只在**渲染某个菜单时**才给它的 movie 设 viewport。
            // 我们这个是自定义菜单,若引擎不渲染它,viewport 就是空的 —— 画什么都落在空处。
            // 这里按当前后缓冲尺寸自己设一份。
            if (uiMovie) {
                std::int32_t vw = 1280, vh = 720;
                if (auto* sc = static_cast<IDXGISwapChain*>(g_lastSwapChain.load())) {
                    ID3D11Texture2D* back = nullptr;
                    if (SUCCEEDED(sc->GetBuffer(presentfreeze::CurrentBackBufferIndex(sc),
                            __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) && back) {
                        D3D11_TEXTURE2D_DESC d{};
                        back->GetDesc(&d);
                        vw = static_cast<std::int32_t>(d.Width);
                        vh = static_cast<std::int32_t>(d.Height);
                        back->Release();
                    }
                }
                uiMovie->SetViewport(vw, vh, 0, 0, vw, vh, RE::GViewport::Flag::kNone);
                logger::info("[CoverUi] viewport set to {}x{}", vw, vh);
            }
        }

        static RE::IMenu* Create()
        {
            return new CoverMenu();
        }

        RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage& a_message) override
        {
            // Never swallow anything: this menu exists only to draw.
            return RE::UI_MESSAGE_RESULTS::kPassOn;
        }
    };

    bool g_coverMenuRegistered = false;

    void RegisterCoverMenu()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            logger::warn("[CoverUi] no UI singleton; cover menu not registered");
            return;
        }
        ui->Register(CoverMenu::MENU_NAME, &CoverMenu::Create);
        g_coverMenuRegistered = true;
        logger::info("[CoverUi] registered menu '{}' (movie {}.swf)", CoverMenu::MENU_NAME, CoverMenu::FILE_NAME);

        // v1.13.1:Register() 只是把 create 回调登记进 menuMap,**并不会实例化菜单** —— 所以
        // GetMenu() 一直返回 null、movie 永远拿不到(这就是 v1.13.0 "movie not available yet" 的原因)。
        // 必须通过 UI 消息队列发一条 kShow,引擎才会真正 create 菜单并加载 Interface/<name>.swf。
        auto* queue = RE::UIMessageQueue::GetSingleton();
        if (queue) {
            queue->AddMessage(RE::BSFixedString(CoverMenu::MENU_NAME.data()), RE::UI_MESSAGE_TYPE::kShow, nullptr);
            logger::info("[CoverUi] pushed kShow to instantiate the menu");
        } else {
            logger::warn("[CoverUi] no UIMessageQueue; cannot instantiate the menu");
        }
    }

    // alpha01: 0 = hidden, 1 = fully covering. Uses kPermanent so the value sticks even if the movie
    // has not finished loading yet.
    void SetCoverAlpha(float a_alpha01)
    {
        if (!g_coverMenuRegistered) {
            return;
        }
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            return;
        }
        auto menu = ui->GetMenu(CoverMenu::MENU_NAME);
        if (!menu) {
            logger::warn("[CoverUi] menu object is null (kShow not processed yet?); alpha {} not applied", a_alpha01);
            return;
        }
        if (!menu->uiMovie) {
            // 菜单实例存在但没有 movie ⇒ 引擎没能加载 Interface/<name>.swf(文件缺失 / SWF 无法解析)
            logger::warn("[CoverUi] menu exists but uiMovie is NULL -> engine did not load Interface/{}.swf; alpha {} not applied",
                CoverMenu::FILE_NAME, a_alpha01);
            return;
        }
        RE::GFxValue v;
        v.SetNumber(static_cast<double>(a_alpha01) * 100.0);

        // v1.16.4: SetVariable 的点路径一直 miss(虽然 uiMovie 已经 SET),所以改成
        //   "先把 clip 取成对象句柄 -> 再 SetMember 改属性",并对候选路径逐个探测打日志。
        static RE::GFxValue s_clip;
        static const char*  s_clipPath = "";
        if (!s_clip.IsDisplayObject()) {
            static constexpr const char* kCandidates[] = {
                "Menu_mc.cover", "cover", "_root.Menu_mc.cover", "Menu_mc"
            };
            for (auto* path : kCandidates) {
                RE::GFxValue probe;
                const bool got = menu->uiMovie->GetVariable(&probe, path);
                const int ty = static_cast<int>(probe.GetType());
                const bool disp = probe.IsDisplayObject();
                const bool obj = probe.IsObject();
                logger::info("[CoverUi] probe '{}': got={} type={} obj={} display={}",
                    path, got ? "y" : "n", ty, obj ? "y" : "n", disp ? "y" : "n");
                if (got && disp) {
                    s_clip = probe;
                    s_clipPath = path;
                    break;
                }
            }
        }

        if (s_clip.IsDisplayObject()) {
            // v1.16.6: SWF 里那个子级 cover 在运行时没有实例化(实测 Menu_mc.cover got=n),
            // 所以改成"拿到 Menu_mc 句柄后自己往里画"。绘制原语(beginFill/moveTo/lineTo/endFill)
            // 在 Loading Menu 上已经验证过能真正上屏,这里复用同一套。
            static bool s_drawn = false;
            if (!s_drawn) {
                RE::GFxValue col, alp;
                col.SetNumber(1315860.0);   // 0x141414 深灰(先用它验"能不能盖住",之后换磨玻璃纹理)
                alp.SetNumber(100.0);
                auto fa = std::array{ col, alp };
                const bool bFill = s_clip.Invoke("beginFill", nullptr, fa.data(), 2);
                auto mv = std::array{ RE::GFxValue(), RE::GFxValue() };
                mv[0].SetNumber(0.0); mv[1].SetNumber(0.0);
                s_clip.Invoke("moveTo", nullptr, mv.data(), 2);
                const double pts[3][2] = { { 1920.0, 0.0 }, { 1920.0, 1080.0 }, { 0.0, 1080.0 } };
                bool bLine = true;
                for (const auto& pt : pts) {
                    auto ln = std::array{ RE::GFxValue(), RE::GFxValue() };
                    ln[0].SetNumber(pt[0]); ln[1].SetNumber(pt[1]);
                    bLine = s_clip.Invoke("lineTo", nullptr, ln.data(), 2) && bLine;
                }
                static const auto kEnd = std::array<RE::GFxValue, 1>{};
                const bool bEnd = s_clip.Invoke("endFill", nullptr, kEnd.data(), 0);
                s_drawn = true;
                logger::info("[CoverUi] drew cover rect into '{}'  fill={} line={} end={}",
                    s_clipPath, bFill ? "ok" : "FAIL", bLine ? "ok" : "FAIL", bEnd ? "ok" : "FAIL");
            }
            const bool ok = s_clip.SetMember("_alpha", v);
            auto* ui2 = RE::UI::GetSingleton();
            const bool open = ui2 ? ui2->IsMenuOpen(CoverMenu::MENU_NAME) : false;
            logger::info("[CoverUi] alpha={:.0f} via clip '{}' -> {}   menuOpen={}",
                a_alpha01 * 100.0, s_clipPath, ok ? "set" : "FAIL", open ? "y" : "N");
            return;
        }

        // 没有拿到句柄时保留旧的路径写法作为最后兜底
        const bool okNested = menu->uiMovie->SetVariable("Menu_mc.cover._alpha", v,
            RE::GFxMovie::SetVarType::kPermanent);
        const bool okFlat = menu->uiMovie->SetVariable("cover._alpha", v,
            RE::GFxMovie::SetVarType::kPermanent);
        logger::info("[CoverUi] alpha={:.0f}  NO CLIP HANDLE; path SetVariable: Menu_mc.cover={} cover={}",
            a_alpha01 * 100.0, okNested ? "set" : "miss", okFlat ? "set" : "miss");
    }
    // ================================================================================

    void OnMessage(SKSE::MessagingInterface::Message* a_msg)
    {        if (!a_msg) {
            return;
        }
        switch (a_msg->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            logger::info("kDataLoaded: initializing LoadingFlow hooks & auto-verifying blueprint index");
            EnsureBlueprintsLoaded(false);
            ResolveExcludedCells();
            LoadCellTimesOnce();
            CacheForms();
            InstallVtableHooks(g_config.usePocketCopies);
            if (g_config.coverTest == 2) {
                RegisterCoverMenu();
            }
            LogGraphicsEnvironment();
            if (g_config.probeGfx) {
                ProbeDumpGfxOnce();
            }
            // v1.3.1 step-2 probe: install the Present hook NOW (not only at a transition) so the
            // hook-chain order can be judged, and so frames during normal play can be dumped.
            if (g_config.capProbe || g_config.captureFrame) {
                auto* sc = presentfreeze::CurrentSwapChain();
                if (sc && presentfreeze::InstallOn(sc)) {
                    logger::info("[Cap] probe hook armed at kDataLoaded (swapchain {})", sc);
                } else {
                    logger::warn("[Cap] no swap chain at kDataLoaded; the probe will arm on the first transition instead");
                }
            }
            if (g_config.usePocketCopies) {
                logger::warn("============================================================");
                logger::warn("bUsePocketCopies = 1: FROZEN EXPERIMENTAL PATH.");
                logger::warn("The pocket-clone route was rejected (no NAVM => no NPC pathing) and");
                logger::warn("has never been verified in game. Set it back to 0.");
                logger::warn("============================================================");
            }
            if (g_config.usePocketCopies) {
                // Condition hooks exist only to make a pocket copy answer cell/location questions
                // correctly. In real-transition mode the engine answers them itself, so installing
                // them would be pure overhead on every condition evaluation.
                InstallConditionHooks();
            } else {
                logger::info("Condition function hooks skipped (real-transition mode: the engine answers natively)");
            }
            InstallLoadingMenuHook();
            if (auto* ui = RE::UI::GetSingleton()) {
                ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_exemptSink);
                logger::info("[Tier2] menu sink registered (fast-travel / load-game exemption for global mode)");
            }
            ApplyEngineTimerTweaks();
            StartPrewarmThread();
            RegisterMenu();
            break;
        case SKSE::MessagingInterface::kSaveGame:
            // P1-3: never write a save file with the player standing at Z=+26000 inside a replica
            // whose geometry is not persisted. Put them back on the ground first, then let the save run.
            if (g_activeLoadingFlowID.load() != 0) {
                logger::warn("Save requested while inside a high-altitude Pocket: teleporting the player back to the "
                             "recorded ground position and purging temporary references before the save is written");
                PurgeAllPockets(true);
            }
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
        case SKSE::MessagingInterface::kPostLoadGame:
        case SKSE::MessagingInterface::kNewGame:
            PurgeAllPockets(false);
            // A load/new game kills any transition window that was still open; never leave the
            // overlay armed across it (it is bounded anyway, but a stale arm would draw over the
            // loading screen).
            voidoverlay::Disarm("load/new game");
            // Loading a save can re-read the game INIs and revert everything we applied at
            // kDataLoaded. The upstream transition mod re-applies here for exactly this reason.
            ApplyEngineTimerTweaks();
            break;
        default:
            break;
        }
    }
}

SKSEPluginInfo(
    .Version = REL::Version{ 1, 3, 5, 0 },
    .Name = "LoadingFlow"sv,
    .Author = "RandyLua"sv,
    .SupportEmail = ""sv,
    .StructCompatibility = SKSE::StructCompatibility::Independent,
    .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary,
    .MinimumSKSEVersion = REL::Version{ 0, 2, 0, 20 })

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);
    std::atexit([]() { g_shuttingDown.store(true, std::memory_order_release); });

    auto path = logger::log_directory();
    if (!path) {
        return false;
    }
    *path /= "LoadingFlow.log"sv;

    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
    auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
    log->set_level(spdlog::level::info);
    log->flush_on(spdlog::level::info);
    spdlog::set_default_logger(std::move(log));
    spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");

    // Load the INI BEFORE logging the banner -- otherwise the banner prints defaults and lies about
    // the mode the game is actually running in.
    LoadConfig();

    logger::info("LoadingFlow v" LOADINGFLOW_BANNER " loading... [MODE: real transitions | hooks: door + LoadingMenu{} | "
                 "suppression {} (re-post after {} ms inside a {} ms window) | GLOBAL suppression {} | prewarm {} | "
                 "no-black {} (manual {}, walk-through {}) | cut mask 0x{:08X}/{} ms | freeze delay {} ms | "
                 "envelope {} ({} ms) | pocket copies {} | void overlay {} ({} motes, {} ms pulse)]",
        g_config.usePocketCopies ? " + condition + container" : " only",
        g_config.suppressLoadingMenu ? "ON" : "off",
        g_config.suppressTimeoutMs,
        g_config.transitionWindowMs,
        g_config.globalMenuSuppression ? "ON" : "off",
        g_config.prewarmNearbyDoors ? "ON" : "off",
        g_config.noBlackTransition ? "ON (freeze)" : "off",
        g_config.noBlackOnlyForManualDoors ? "yes" : "all doors",
        g_config.noBlackForWalkThrough ? "yes" : "no",
        g_config.cutMaskImod, g_config.cutMaskMs,
        g_config.freezeDelayMs,
        g_config.visualEnvelope ? "ON" : "off",
        g_config.envelopeBlackHoldMs,
        g_config.usePocketCopies ? "ON" : "off",
        g_config.voidOverlay ? "ON" : "off",
        g_config.overlayMotes,
        g_config.overlayPulseMs);
    if (auto* messaging = SKSE::GetMessagingInterface()) {
        messaging->RegisterListener(OnMessage);
    }

    return true;
}
