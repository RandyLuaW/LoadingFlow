#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
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
        std::uint32_t suppressTimeoutMs = 400;
        // How long after the door activation we still treat a LoadingMenu request as belonging to
        // THIS transition. Measured in-game: the engine requests the menu 0.6-1.0 s after Activate
        // (it fades and starts the load first), so a 400 ms window expired before the menu was even
        // asked for. suppressTimeoutMs keeps its own meaning: after we HAVE suppressed, how long we
        // wait for the swap before re-posting the menu.
        std::uint32_t transitionWindowMs = 10000;
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
        bool          noBlackOnlyForManualDoors = true;
        // Cut mask: a brief effect fired exactly at the moment the cell swap lands, to hide the hard
        // cut from "door half open" to "inside". It is NOT the long black screen -- default 80 ms,
        // and the record is swappable (black family, FadeToWhite 0x0010100B, blur 0x000B97F7,
        // desaturate 0x000B7983 ...). 0 = no mask at all.
        std::uint32_t cutMaskImod = 0x000F756D;
        std::uint32_t cutMaskMs = 80;
        // Per-door diagnostic. Measured in-game: NPCs activate doors constantly (502 of 559 log
        // lines in one 3-minute session), so this stays off unless explicitly requested.
        bool          logDoorActivations = false;
        // Adaptive fallback. 0 = always suppress. When > 0, a cell whose last measured load took
        // longer than this shows the vanilla loading menu instead of 2 s of black: on this modlist
        // interior loads measure 1884-3269 ms, which is too long to hide.
        std::uint32_t suppressMaxMs = 0;
        // Adaptive freeze guard. Freeze only feels seamless while the load is short; once a cell has
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
        float postLoadUpdateTimeMS = 100.0f;     // vanilla 500, but THIS MACHINE ships 2000 in
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
        int   loadingQueuedPriorityBudgetMs = 500;  // vanilla 20; loader budget while the screen is up
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
        // is nearly seamless -- that is what Interiors->Interior transit gets here. Ext->Int (cave
        // mouth from outside) still uses the normal fade.
        bool          noBlackForWalkThrough = true;
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

    void LoadLangFile(const char* a_path, bool a_override)
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
        } else {
            logger::warn("Language pack: {} opened but yielded 0 strings", a_path);
        }
    }

    void LoadLang()
    {
        const auto backup = g_lang;
        g_lang.clear();
        LoadLangFile("Data/SKSE/Plugins/LoadingFlow.ini", false);
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

    void SaveConfig()
    {
        static constexpr const char* kIniPath = "Data/SKSE/Plugins/LoadingFlow.ini";
        static constexpr const char* kTmpPath = "Data/SKSE/Plugins/LoadingFlow.ini.tmp";
        // P0-3: canonical must-write section list so new sections are never silently dropped
        static constexpr const char* kMustWriteSections[] = { "General", "ExcludeCells", "Menu" };
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
            f << "uTransitionWindowMs = " << g_config.transitionWindowMs << "\r\n";
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
            f << std::format("sCutMaskImod = 0x{:08X}\r\n", g_config.cutMaskImod);
            f << "uCutMaskMs = " << g_config.cutMaskMs << "\r\n";
            f << "bLogDoorActivations = " << (g_config.logDoorActivations ? 1 : 0) << "\r\n";
            f << "uSuppressMaxMs = " << g_config.suppressMaxMs << "\r\n";
            f << "uFreezeMaxMs = " << g_config.freezeMaxMs << "\r\n";
            f << std::format("fCutMaskFadeOut = {:.2f}\r\n", g_config.cutMaskFadeOut);
            f << "uFreezeDelayMs = " << g_config.freezeDelayMs << "\r\n";
            f << "bNoBlackForWalkThrough = " << (g_config.noBlackForWalkThrough ? 1 : 0) << "\r\n";
            f << "bGlobalMenuSuppression = " << (g_config.globalMenuSuppression ? 1 : 0) << "\r\n";
            f << std::format("fBaseAltitudeZ = {:.1f}\r\n", g_config.baseAltitudeZ);
            f << std::format("fSlotStepZ = {:.1f}\r\n", g_config.slotStepZ);
            f << std::format("fBlinkStrength = {:.2f}\r\n", g_config.blinkStrength);
            f << "uDoorOpenDelayMs = " << g_config.doorOpenDelayMs << "\r\n";
            f << "uMaxRefsPerCell = " << g_config.maxRefsPerCell << "\r\n";
            f << "uMaxPocketSlots = " << g_config.maxPocketSlots << "\r\n";
            f << "\r\n[ExcludeCells]\r\n";
            f << "; Add interior Cell FormIDs (0x...) or EditorIDs here to force vanilla loading screen\r\n";
            for (const auto& s : g_excludeSpecs) {
                f << s << " = 1\r\n";
            }
            f << "\r\n[ForceRealTransition]\r\n";
            f << "; Cells that must use the engine's own (real) cell transition instead of a pocket\n";
            f << "; copy: quest/location scripts inside them listen to cell events we cannot fake.\n";
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

        std::error_code ec;
        std::filesystem::rename(kTmpPath, kIniPath, ec);
        if (ec) {
            std::filesystem::copy_file(kTmpPath, kIniPath, std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(kTmpPath, ec);
        }
        logger::info("Config saved atomically to {}", kIniPath);
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
                        g_config.suppressTimeoutMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 5000));
                    } else if (key == "uTransitionWindowMs") {
                        g_config.transitionWindowMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 500, 30000));
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
                        g_config.suppressMaxMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 30000));
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
    // One-shot instrumentation: LoadingMenu is a kAlwaysOpen menu, so after the first display the
    // engine may reuse the instance and request it with kReshow instead of kShow. Log the first few
    // message types we actually receive so this can be settled from a log instead of guessed.
    std::atomic<int> g_msgTypeLogBudget{ 12 };
    std::atomic<bool>         g_menuWasSuppressed{ false };
    std::atomic<std::int64_t> g_suppressDeadline{ 0 };
    std::atomic<std::int64_t> g_fallbackAt{ 0 };   // set only AFTER we suppressed a menu
    std::atomic<std::int64_t> g_suppressStarted{ 0 };
    std::atomic<RE::FormID>   g_suppressFromCell{ 0 };

    std::int64_t NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // defined below; the global-mode branch of the menu hook needs it before the definition
    void ArmLoadingMenuSuppression(const char* a_reason, bool a_isAutoCaveDoor, RE::FormID a_destCell = 0);
    void RaiseSwapBlack(const char* a_reason);
    void ReleaseSwapBlack();

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
                    g_menuWasSuppressed.store(true);
                    logger::info("[Tier2] LoadingMenu kShow suppressed by global mode");
                    return RE::UI_MESSAGE_RESULTS::kIgnore;
                }
            }
        }
        if (g_suppressArmed.load() && g_suppressWanted.load() && wantsShow) {
            if (NowMs() <= g_suppressDeadline.load()) {
                if (!g_menuWasSuppressed.exchange(true)) {
                    // First suppression of this transition: the re-post fallback is measured from
                    // here, not from the door activation.
                    g_fallbackAt.store(NowMs() + static_cast<std::int64_t>(g_config.suppressTimeoutMs));
                    // Raise the black NOW, roughly a second before the swap, so that the engine's
                    // scene-attach spin happens behind it instead of in front of it.
                    RaiseSwapBlack("menu suppressed");
                }
                logger::info("[Tier2] LoadingMenu suppressed (type={}, {} ms after activation)",
                    static_cast<std::uint32_t>(msgType), NowMs() - g_suppressStarted.load());
                return RE::UI_MESSAGE_RESULTS::kIgnore;
            }
            logger::info("[Tier2] LoadingMenu let through: past the {} ms transition window",
                g_config.transitionWindowMs);
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
        // Freeze policy. Manual (building) doors always qualify. Auto/frameless doors qualify only
        // when the transit is Interiors->Interior -- i.e. a walk-through passage inside a cave whose
        // two sides are visually continuous; a cave mouth entered from outside keeps the fade.
        const bool fromInterior = fromCell && fromCell->IsInteriorCell();
        bool       freeze = false;
        if (g_config.noBlackTransition) {
            if (!a_isAutoCaveDoor) {
                freeze = true;
            } else if (!g_config.noBlackOnlyForManualDoors) {
                freeze = true;
            } else if (g_config.noBlackForWalkThrough && fromInterior) {
                freeze = true;
            }
        }
        g_noBlackWanted.store(freeze);
        // Adaptive guard: a cell we already measured as slow must not freeze (a frozen frame for
        // seconds reads as a hang). Keeps the freeze route usable without a global on/off.
        if (freeze && g_config.freezeMaxMs > 0 && a_destCell != 0) {
            const auto msIt = g_cellLoadMs.find(a_destCell);
            if (msIt != g_cellLoadMs.end() && msIt->second > g_config.freezeMaxMs) {
                logger::info("[Tier2] cell 0x{:08X} last loaded in {} ms (> uFreezeMaxMs {}); using the fade instead of freeze",
                    a_destCell, msIt->second, g_config.freezeMaxMs);
                freeze = false;
                g_noBlackWanted.store(false);
            }
        }
        g_noBlackStartAt.store(NowMs() + static_cast<std::int64_t>(g_config.freezeDelayMs));
        g_suppressFromCell.store(fromCell ? fromCell->GetFormID() : 0);
        g_suppressStarted.store(NowMs());
        g_suppressDeadline.store(NowMs() + static_cast<std::int64_t>(g_config.transitionWindowMs));
        g_fallbackAt.store(0);   // only armed once we actually suppress a menu
        g_menuWasSuppressed.store(false);
        g_suppressArmed.store(true);
        logger::info("[Tier2] real transition started (cell 0x{:08X}, door {}, menu suppression {}, no-black {}, window {} ms, reason: {})",
            g_suppressFromCell.load(), a_isAutoCaveDoor ? "auto/frameless" : "manual",
            g_config.suppressLoadingMenu ? "ON" : "OFF",
            g_noBlackWanted.load() ? "FREEZE" : "fade",
            g_config.suppressTimeoutMs, a_reason);
        // One-shot watchdog. Bounded loop, posts at most one task per step, never re-queues itself
        // (the AddTask feedback loop is what froze this project twice before).
        const std::uint32_t myGen = ++g_suppressGen;
        std::thread([myGen]() {
            constexpr int kStepMs = 25;
            constexpr int kMaxSteps = 600;  // 15 s hard cap
            bool fallbackPosted = false;
            for (int i = 0; i < kMaxSteps; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kStepMs));
                if (g_suppressGen.load() != myGen) {
                    return;   // a newer transition took over
                }
                if (!g_suppressArmed.load()) {
                    return;
                }
                // Fallback re-post: measured from the moment we ACTUALLY suppressed a menu, because
                // the engine may ask for it a full second after the activation.
                const auto fbAt = g_fallbackAt.load();
                if (!fallbackPosted && fbAt != 0 && NowMs() > fbAt) {
                    fallbackPosted = true;
                    if (auto* task = SKSE::GetTaskInterface()) {
                        task->AddTask([myGen]() {
                            if (g_suppressGen.load() != myGen || !g_menuWasSuppressed.load()) {
                                return;
                            }
                            // Stop suppressing from now on, otherwise the re-posted menu would be
                            // swallowed again by this same window and the player would see nothing.
                            g_suppressWanted.store(false);
                            logger::warn("[Tier2] suppressed menu did not complete in time; re-posting the LoadingMenu (fallback)");
                            if (auto* q = RE::UIMessageQueue::GetSingleton()) {
                                q->AddMessage(RE::BSFixedString(RE::LoadingMenu::MENU_NAME.data()),
                                    RE::UI_MESSAGE_TYPE::kShow, nullptr);
                            }
                        });
                    }
                }
                if (NowMs() > g_suppressDeadline.load()) {
                    logger::info("[Tier2] transition window elapsed without a cell change; stopping measurement");
                    g_suppressArmed.store(false);
                    // Never leave our black up: if the transition never reports back, release it.
                    if (auto* task = SKSE::GetTaskInterface()) {
                        task->AddTask([myGen]() {
                            if (g_suppressGen.load() != myGen) {
                                return;
                            }
                            ReleaseSwapBlack();
                        });
                    }
                    return;
                }
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask([myGen]() {
                        if (g_suppressGen.load() != myGen || !g_suppressArmed.load()) {
                            return;
                        }
                        // No-black mode: keep cancelling the engine's fade records for the whole
                        // transition window. The last presented frame then simply holds until the
                        // new cell shows up -- a freeze instead of a fade.
                        if (g_noBlackWanted.load() && NowMs() >= g_noBlackStartAt.load()) {
                            CacheForms();
                            // Only cancel the records that DARKEN the screen. Cancelling the fade-BACK
                            // as well makes the engine's own fade-in restart every 25 ms, which reads
                            // as strobing ("天旋地转") right at the swap.
                            if (g_fadeImod) {
                                RE::ImageSpaceModifierInstanceForm::Stop(g_fadeImod);
                            }
                            if (g_fadeHoldImod) {
                                RE::ImageSpaceModifierInstanceForm::Stop(g_fadeHoldImod);
                            }
                        }
                        auto* pl = RE::PlayerCharacter::GetSingleton();
                        if (!pl) {
                            return;
                        }
                        auto* cell = pl->GetParentCell();
                        if (cell && cell->GetFormID() != g_suppressFromCell.load()) {
                            const auto ms = NowMs() - g_suppressStarted.load();
                            g_cellLoadMs[cell->GetFormID()] = static_cast<std::uint32_t>(ms);
                            const bool wasSuppressed = g_menuWasSuppressed.exchange(false);
                            g_suppressArmed.store(false);
                            logger::info("[Tier2] real transition finished in {} ms (cell 0x{:08X} -> 0x{:08X}, menu {})",
                                ms, g_suppressFromCell.load(), cell->GetFormID(),
                                wasSuppressed ? "suppressed" : "shown");
                            if (g_noBlackWanted.load()) {
                                // Freeze mode leaves a hard cut; mask it for a few frames.
                                if (g_config.cutMaskMs > 0 && g_config.cutMaskImod != 0) {
                                    ApplyCutMask();
                                }
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
                        }
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
        if (REX::W32::VirtualProtect(&table[kProcessMessageIdx], sizeof(void*), kPageExecuteReadWrite, &oldProt)) {
            g_origLoadingProcessMessage = reinterpret_cast<ProcessMessage_t>(table[kProcessMessageIdx]);
            table[kProcessMessageIdx] = reinterpret_cast<void*>(&HookedLoadingMenuProcessMessage);
            REX::W32::VirtualProtect(&table[kProcessMessageIdx], sizeof(void*), oldProt, &oldProt);
            logger::info("[Tier2] LoadingMenu ProcessMessage hook installed at vtable[0x04]=0x{:X} (suppression {})",
                reinterpret_cast<std::uintptr_t>(g_origLoadingProcessMessage),
                g_config.suppressLoadingMenu ? "ENABLED" : "disabled");
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
                logger::info("[LF] >>> Seamless EXIT to Exterior cell 0x{:08X} pos=({:.1f},{:.1f},{:.1f}) yaw={:.2f}",
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
            const auto msIt = g_cellLoadMs.find(destCellID);
            if (msIt != g_cellLoadMs.end() && msIt->second > g_config.suppressMaxMs) {
                logger::info("[Tier2] cell 0x{:08X} last load measured {} ms (> uSuppressMaxMs {}); showing the vanilla menu",
                    destCellID, msIt->second, g_config.suppressMaxMs);
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
            ArmLoadingMenuSuppression("interior->interior disabled", IsAutoOrFramelessDoor(a_targetRef));
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
            ArmLoadingMenuSuppression("pocket unavailable (quota / oversized cell)", IsAutoOrFramelessDoor(a_targetRef));
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

            logger::info("[LF] >>> Seamless ENTER [{}|{}] -> Slot #{} cell=0x{:08X} ({}) src={} pos=({:.1f},{:.1f},{:.1f}) yaw={:.2f}",
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
            if (vtbl && IsExecutableAddress(vtbl[kActivateIdx])) {
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
    void __stdcall RenderMainMenu()
    {
        bool en = g_config.enabled;
        if (ImGuiMCP::Checkbox(L("menu.enable", "Enable LoadingFlow Seamless Door Transitions"), &en)) {
            g_config.enabled = en;
            SaveConfig();
        }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button(L("menu.btn.save", "Save Config"))) {
            SaveConfig();
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
                L("menu.status.inPocket", "Current Status: INSIDE POCKET CELL (0x%08X) | Seamless Transitions: %u"),
                activeID, g_transitionCount.load());
        } else {
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.7f, 0.85f, 1.0f, 1.0f),
                L("menu.status.normal", "Current Status: Normal World/Cell | Seamless Transitions: %u"),
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
        ImGuiMCP::Text(L("menu.sec.routing", "-- Door Transition Routing --"));
        if (ImGuiMCP::Checkbox(L("menu.intToInt", "Enable Interior -> Interior (multi-slot vertical stack)"), &g_config.allowInteriorToInterior)) {
            SaveConfig();
        }
        if (ImGuiMCP::Checkbox(L("menu.pocketToExt", "Enable Pocket Interior -> Exterior (all doors & balconies)"), &g_config.allowPocketToExterior)) {
            SaveConfig();
        }
        if (ImGuiMCP::Checkbox(L("menu.moveActors", "Move interior NPCs into Pocket on entry"), &g_config.moveActorsToPocket)) {
            SaveConfig();
        }
        if (ImGuiMCP::Checkbox(L("menu.blackShell", "Spawn BlackPlane01 light-blocking shell around Pocket"), &g_config.spawnBlackBoxShell)) {
            SaveConfig();
        }
        if (ImGuiMCP::Checkbox(L("menu.doorFeedback", "Play door sound & open animation on transition"), &g_config.playDoorSoundAndAnim)) {
            SaveConfig();
        }
        if (ImGuiMCP::Checkbox(L("menu.blinkImod", "Play brief blink fade (FadeToBlackImod) on transition"), &g_config.triggerBlinkImod)) {
            SaveConfig();
        }
        if (ImGuiMCP::Checkbox(L("menu.sneakVanilla", "Hold Sneak while activating a door to force vanilla load screen"), &g_config.sneakForcesVanillaLoad)) {
            SaveConfig();
        }
        if (ImGuiMCP::Checkbox(L("menu.debugNotify", "Show top-left HUD notification on instant transition"), &g_config.showDebugNotifications)) {
            SaveConfig();
        }
        if (ImGuiMCP::Checkbox(L("menu.fakeLocationEvents", "Fake location-change event on transition (OnLocationChange path)"), &g_config.fakeLocationEvents)) {
            SaveConfig();
        }
        if (ImGuiMCP::Checkbox(L("menu.fakeCellLoadEvents", "Fake cell attach/loaded events (may fire OnCellAttach across the whole cell - test with care)"), &g_config.fakeCellLoadEvents)) {
            SaveConfig();
        }
        if (ImGuiMCP::Checkbox(L("menu.usePocketCopies", "Legacy: high-altitude pocket copies (no navmesh/NPCs - experimental, off)"), &g_config.usePocketCopies)) {
            SaveConfig();
        }
        if (ImGuiMCP::Checkbox(L("menu.suppressLoadingMenu", "Tier2: hide the Loading Menu during real cell transitions (auto fallback)"), &g_config.suppressLoadingMenu)) {
            SaveConfig();
        }
        if (ImGuiMCP::Checkbox(L("menu.prewarmNearbyDoors", "Tier3: pre-warm models of nearby routed interiors while walking"), &g_config.prewarmNearbyDoors)) {
            SaveConfig();
        }
        if (ImGuiMCP::Checkbox(L("menu.visualEnvelope", "Hold black briefly after the swap to hide mesh pop-in"), &g_config.visualEnvelope)) {
            SaveConfig();
        }
        if (ImGuiMCP::Checkbox(L("menu.noBlackTransition", "No-black mode: keep the last frame on screen instead of fading (a freeze, not a fade)"), &g_config.noBlackTransition)) {
            SaveConfig();
        }

        // ---- one-click presets -------------------------------------------------
        // Purely a convenience layer over the same fields the individual controls write, so the
        // per-item checkboxes below keep working exactly as before.
        ImGuiMCP::Separator();
        ImGuiMCP::Text(L("menu.sec.presets", "-- Presets (one click) --"));
        if (ImGuiMCP::Button(L("menu.preset.a", "A  Conservative (hide menu only)"))) {
            g_config.suppressLoadingMenu = true;
            g_config.noBlackTransition = false;
            g_config.noBlackOnlyForManualDoors = true;
            g_config.noBlackForWalkThrough = true;
            g_config.globalMenuSuppression = false;
            g_config.freezeDelayMs = 0;
            g_config.cutMaskMs = 80;
            g_config.cutMaskFadeOut = 0.15f;
            SaveConfig();
            logger::info("[Preset] applied A (conservative)");
        }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button(L("menu.preset.b", "B  Recommended (freeze doors + passages)"))) {
            g_config.suppressLoadingMenu = true;
            g_config.noBlackTransition = true;
            g_config.noBlackOnlyForManualDoors = true;
            g_config.noBlackForWalkThrough = true;
            g_config.globalMenuSuppression = false;
            g_config.freezeDelayMs = 0;
            g_config.cutMaskMs = 80;
            g_config.cutMaskFadeOut = 0.15f;
            SaveConfig();
            logger::info("[Preset] applied B (recommended)");
        }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button(L("menu.preset.c", "C  Aggressive (also trigger volumes)"))) {
            g_config.suppressLoadingMenu = true;
            g_config.noBlackTransition = true;
            g_config.noBlackOnlyForManualDoors = true;
            g_config.noBlackForWalkThrough = true;
            g_config.globalMenuSuppression = true;
            g_config.freezeDelayMs = 0;
            g_config.cutMaskMs = 80;
            g_config.cutMaskFadeOut = 0.15f;
            SaveConfig();
            logger::info("[Preset] applied C (aggressive)");
        }

        // ---- individual controls for the preset fields (kept, as requested) ----
        if (ImGuiMCP::Checkbox(L("menu.noBlackManualOnly", "Freeze: manual (building) doors only"), &g_config.noBlackOnlyForManualDoors)) {
            SaveConfig();
        }
        if (ImGuiMCP::Checkbox(L("menu.noBlackWalkThrough", "Freeze: Interiors->Interior walk-through passages"), &g_config.noBlackForWalkThrough)) {
            SaveConfig();
        }
        if (ImGuiMCP::Checkbox(L("menu.globalSuppress", "Global: suppress the menu for non-door transitions too"), &g_config.globalMenuSuppression)) {
            SaveConfig();
        }
        {
            int freezeDelay = static_cast<int>(g_config.freezeDelayMs);
            if (ImGuiMCP::SliderInt(L("menu.freezeDelay", "Freeze delay: let the door animate first (ms)"), &freezeDelay, 0, 600, "%d ms")) {
                g_config.freezeDelayMs = static_cast<std::uint32_t>(freezeDelay);
                SaveConfig();
            }
            int cutMask = static_cast<int>(g_config.cutMaskMs);
            if (ImGuiMCP::SliderInt(L("menu.cutMaskMs", "Cut mask duration (ms, 0 = off)"), &cutMask, 0, 500, "%d ms")) {
                g_config.cutMaskMs = static_cast<std::uint32_t>(cutMask);
                SaveConfig();
            }
            ImGuiMCP::SliderFloat(L("menu.cutMaskFade", "Cut mask fade-out (s, 0 = hard stop)"), &g_config.cutMaskFadeOut, 0.0f, 1.0f, "%.2f s");
        }

        ImGuiMCP::Separator();
        ImGuiMCP::Text(L("menu.sec.params", "-- Pocket Geometry & Thresholds --"));
        ImGuiMCP::SliderFloat(L("menu.baseZ", "Slot #0 Base Altitude Z"), &g_config.baseAltitudeZ, 10000.0f, 45000.0f, "%.0f");
        ImGuiMCP::SliderFloat(L("menu.stepZ", "Vertical Step per Slot Z"), &g_config.slotStepZ, 2000.0f, 8000.0f, "%.0f");
        int delayMs = static_cast<int>(g_config.doorOpenDelayMs);
        if (ImGuiMCP::SliderInt(L("menu.doorDelay", "Door Crack & Blink Delay (ms)"), &delayMs, 0, 600)) {
            g_config.doorOpenDelayMs = static_cast<std::uint32_t>(delayMs);
        }
        int maxRefs = static_cast<int>(g_config.maxRefsPerCell);
        if (ImGuiMCP::SliderInt(L("menu.maxRefs", "Max References per Cell"), &maxRefs, 100, 2500)) {
            g_config.maxRefsPerCell = static_cast<std::uint32_t>(maxRefs);
        }
        int maxSlots = static_cast<int>(g_config.maxPocketSlots);
        if (ImGuiMCP::SliderInt(L("menu.maxSlots", "Max Active Pocket Slots"), &maxSlots, 1, 16)) {
            g_config.maxPocketSlots = static_cast<std::uint32_t>(maxSlots);
        }

        ImGuiMCP::Separator();
        ImGuiMCP::Text(L("menu.sec.activeSlots", "-- Active Pocket Instances in Memory --"));
        if (ImGuiMCP::Button(L("menu.btn.purge", "Purge All Pocket Instances"))) {
            SKSE::GetTaskInterface()->AddTask([]() {
                PurgeAllPockets(false);
            });
        }
        {
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            ImGuiMCP::Text(L("menu.slotSummary", "Active Slots: %zu / %u | Mapped Doors: %zu | Mapped Containers: %zu"),
                g_pockets.size(), g_config.maxPocketSlots,
                g_clonedDoorToOriginalDoor.size(), g_clonedContToOriginalCont.size());
            for (const auto& [cid, inst] : g_pockets) {
                ImGuiMCP::Text("  [Slot #%u | %s] Cell 0x%08X (%s) | Z=%.0f | Statics=%zu Doors=%zu Conts=%zu Actors=%zu",
                    inst.slotIndex, inst.builtFromBlueprint ? "BP" : "LIVE", inst.interiorCellID,
                    inst.interiorEditorID.empty() ? inst.interiorFullName.c_str() : inst.interiorEditorID.c_str(),
                    inst.pocketOrigin.z,
                    inst.clonedStaticCount, inst.clonedDoorCount, inst.clonedContainerCount, inst.movedActors.size());
            }
        }
    }

    void RegisterMenu()
    {
        if (!SKSEMenuFramework::IsInstalled()) {
            logger::warn("SKSEMenuFramework.dll not found; menu not registered (plugin still works)");
            return;
        }
        SKSEMenuFramework::SetSection("LoadingFlow");
        SKSEMenuFramework::AddSectionItem("Seamless Pocket Settings", RenderMainMenu);
        logger::info("Registered SKSEMenuFramework section: LoadingFlow");
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
    void ApplyEngineTimerTweaks()
    {
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
    }

    void OnMessage(SKSE::MessagingInterface::Message* a_msg)
    {        if (!a_msg) {
            return;
        }
        switch (a_msg->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            logger::info("kDataLoaded: initializing LoadingFlow hooks & auto-verifying blueprint index");
            EnsureBlueprintsLoaded(false);
            ResolveExcludedCells();
            CacheForms();
            InstallVtableHooks(g_config.usePocketCopies);
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
    .Version = REL::Version{ 1, 0, 0, 0 },
    .Name = "LoadingFlow"sv,
    .Author = "RandyLua"sv,
    .SupportEmail = ""sv,
    .StructCompatibility = SKSE::StructCompatibility::Independent,
    .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary,
    .MinimumSKSEVersion = REL::Version{ 0, 2, 0, 20 })

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);

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

    logger::info("LoadingFlow v1.0.0 loading... [MODE: real transitions | hooks: door + LoadingMenu{} | "
                 "suppression {} (re-post after {} ms inside a {} ms window) | GLOBAL suppression {} | prewarm {} | "
                 "no-black {} (manual {}, walk-through {}) | cut mask 0x{:08X}/{} ms | freeze delay {} ms | "
                 "envelope {} ({} ms) | pocket copies {}]",
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
        g_config.usePocketCopies ? "ON" : "off");

    if (auto* messaging = SKSE::GetMessagingInterface()) {
        messaging->RegisterListener(OnMessage);
    }

    return true;
}
