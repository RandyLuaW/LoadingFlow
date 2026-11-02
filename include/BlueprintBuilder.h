#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

namespace PocketBlueprint
{
    namespace logger = SKSE::log;

    #pragma pack(push, 1)
    struct FileHeaderDisk
    {
        char          magic[4];         // "PKC2"
        std::uint32_t version;          // 3 (v2/v3 layout with 64-bit loadFingerprint)
        std::uint64_t loadFingerprint;  // FNV-1a 64-bit hash of active load order + file sizes/timestamps
        std::uint32_t cellCount;
        std::uint32_t doorCount;
    };
    static_assert(sizeof(FileHeaderDisk) == 24);

    struct DoorDisk
    {
        std::uint32_t doorRefID;
        std::uint32_t parentCellID;
        std::uint32_t linkedDoorID;
        float         telPos[3];
        float         telRot[3];
        std::uint32_t isParentInterior;
    };
    static_assert(sizeof(DoorDisk) == 40);

    struct CellHeaderDisk
    {
        std::uint32_t cellID;
        std::uint32_t locationID;
        std::uint32_t ownerID;
        std::uint32_t refCount;
        std::uint32_t actorCount;
        char          edid[60];
    };
    static_assert(sizeof(CellHeaderDisk) == 80);

    struct RefDisk
    {
        std::uint32_t origRefID;
        std::uint32_t baseID;
        std::uint32_t ownerID;
        float         pos[3];
        float         rot[3];
        std::uint16_t scale;
        std::uint16_t flags;  // bit0=teleportDoor, bit1=container, bit2=static
    };
    static_assert(sizeof(RefDisk) == 40);

    struct ActorDisk
    {
        std::uint32_t achrRefID;
        float         pos[3];
        float         rot[3];
    };
    static_assert(sizeof(ActorDisk) == 28);
    #pragma pack(pop)

    struct CellBlueprint
    {
        RE::FormID                           cellID = 0;
        RE::FormID                           locationID = 0;
        RE::FormID                           ownerID = 0;
        std::string                          edid;
        std::vector<RefDisk>                 refs;
        std::vector<ActorDisk>               actors;
    };

    inline void Fnv1aMix(std::uint64_t& a_hash, const void* a_data, std::size_t a_len) noexcept
    {
        const auto* bytes = static_cast<const std::uint8_t*>(a_data);
        for (std::size_t i = 0; i < a_len; ++i) {
            a_hash ^= static_cast<std::uint64_t>(bytes[i]);
            a_hash *= 1099511628211ULL;
        }
    }

    // Collect all active TESFiles in load order and compute a sub-millisecond 64-bit fingerprint
    inline std::uint64_t ComputeLoadOrderFingerprint(std::vector<const RE::TESFile*>& a_outFiles, std::uint32_t a_maxRefsPerCell)
    {
        a_outFiles.clear();
        std::uint64_t h = 14695981039346656037ULL;
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            return h;
        }

        for (auto it = dh->files.begin(); it != dh->files.end(); ++it) {
            const RE::TESFile* file = *it;
            if (!file || file->compileIndex == 0xFF) {
                continue;
            }
            a_outFiles.push_back(file);

            // Mix lowercase filename, compileIndex, smallFileCompileIndex, filesize, and ftLastWriteTime
            const std::string_view fname = file->GetFilename();
            for (char c : fname) {
                const auto lc = static_cast<std::uint8_t>(std::tolower(static_cast<unsigned char>(c)));
                Fnv1aMix(h, &lc, 1);
            }
            Fnv1aMix(h, &file->compileIndex, sizeof(file->compileIndex));
            Fnv1aMix(h, &file->smallFileCompileIndex, sizeof(file->smallFileCompileIndex));
            Fnv1aMix(h, &file->filesize, sizeof(file->filesize));
            Fnv1aMix(h, &file->fileData.lastWriteTime, sizeof(file->fileData.lastWriteTime));
            Fnv1aMix(h, &file->fileData.fileSizeLo, sizeof(file->fileData.fileSizeLo));
        }
        Fnv1aMix(h, &a_maxRefsPerCell, sizeof(a_maxRefsPerCell));
        return h;
    }

    // Map a raw FormID inside `a_mod` to its live runtime FormID using `a_mod->masterPtrs`
    inline RE::FormID ResolveRuntimeFormID(const RE::TESFile* a_mod, RE::FormID a_rawFormID) noexcept
    {
        if (!a_mod || a_rawFormID == 0) {
            return 0;
        }
        const std::uint32_t modIdx = (a_rawFormID >> 24) & 0xFFu;
        const RE::TESFile*  ownerFile = a_mod;
        if (modIdx < a_mod->masterCount && a_mod->masterPtrs) {
            ownerFile = a_mod->masterPtrs[modIdx];
        }
        if (!ownerFile || ownerFile->compileIndex == 0xFF) {
            return 0;
        }
        if (ownerFile->IsLight()) {
            return 0xFE000000u |
                   ((static_cast<std::uint32_t>(ownerFile->smallFileCompileIndex) & 0xFFFu) << 12) |
                   (a_rawFormID & 0xFFFu);
        }
        return (static_cast<std::uint32_t>(ownerFile->compileIndex) << 24) | (a_rawFormID & 0x00FFFFFFu);
    }

    inline bool IsClonableRuntimeBase(RE::FormID a_baseID, std::uint16_t& a_outFlags)
    {
        a_outFlags = 0;
        if (a_baseID == 0 || a_baseID == 0x0000003B || a_baseID == 0x00000034 ||
            a_baseID == 0x00000003 || a_baseID == 0x00000032 || a_baseID == 0x00000015) {
            return false;
        }
        auto* form = RE::TESForm::LookupByID(a_baseID);
        auto* bound = form ? form->As<RE::TESBoundObject>() : nullptr;
        if (!bound) {
            return false;
        }
        const auto ft = bound->GetFormType();
        if (ft == RE::FormType::Static) {
            a_outFlags |= 4;
            return true;
        }
        if (ft == RE::FormType::Door) {
            return true;
        }
        if (ft == RE::FormType::Container) {
            a_outFlags |= 2;
            return true;
        }
        switch (ft) {
        case RE::FormType::MovableStatic:
        case RE::FormType::Furniture:
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
            return bound->HasWorldModel();
        default:
            return false;
        }
    }

    struct RawCellBuildState
    {
        RE::FormID                                     cellID = 0;
        RE::FormID                                     locationID = 0;
        RE::FormID                                     ownerID = 0;
        std::string                                    edid;
        std::unordered_map<RE::FormID, RefDisk>        refsByID;
        std::unordered_map<RE::FormID, ActorDisk>      actorsByID;
    };

    inline bool IsExcludedInteriorEdid(std::string_view a_edid) noexcept
    {
        static constexpr std::string_view kSkip[] = {
            "Blackreach", "Test", "Holding"
        };
        for (const auto kw : kSkip) {
            if (a_edid.find(kw) != std::string_view::npos) {
                return true;
            }
        }
        return false;
    }

    // Parse uncompressed REFR/ACHR inside a CELL GRUP buffer and merge into winning-override maps
    inline void WalkCellGrupBuffer(
        const RE::TESFile* a_mod,
        const std::uint8_t* a_buf,
        std::size_t a_start,
        std::size_t a_end,
        RE::FormID a_curCellID,
        std::unordered_map<RE::FormID, RawCellBuildState>& a_cells,
        std::unordered_map<RE::FormID, DoorDisk>& a_doors)
    {
        std::size_t p = a_start;
        while (p + 24 <= a_end) {
            if (std::memcmp(a_buf + p, "GRUP", 4) == 0) {
                std::uint32_t gsize = 0;
                std::uint32_t rawLabel = 0;
                std::int32_t  gtype = 0;
                std::memcpy(&gsize, a_buf + p + 4, 4);
                std::memcpy(&rawLabel, a_buf + p + 8, 4);
                std::memcpy(&gtype, a_buf + p + 12, 4);
                if (gsize < 24 || p + gsize > a_end) {
                    break;
                }
                RE::FormID nextCellID = a_curCellID;
                if (gtype == 6) {
                    nextCellID = ResolveRuntimeFormID(a_mod, rawLabel);
                }
                WalkCellGrupBuffer(a_mod, a_buf, p + 24, p + gsize, nextCellID, a_cells, a_doors);
                p += gsize;
                continue;
            }

            char          rtype[4]{};
            std::uint32_t rsize = 0;
            std::uint32_t rflags = 0;
            std::uint32_t rawFid = 0;
            std::memcpy(rtype, a_buf + p, 4);
            std::memcpy(&rsize, a_buf + p + 4, 4);
            std::memcpy(&rflags, a_buf + p + 8, 4);
            std::memcpy(&rawFid, a_buf + p + 12, 4);
            if (p + 24 + rsize > a_end) {
                break;
            }

            const std::uint8_t* body = a_buf + p + 24;
            p += 24 + rsize;

            const bool isRefr = (std::memcmp(rtype, "REFR", 4) == 0);
            const bool isAchr = (std::memcmp(rtype, "ACHR", 4) == 0);
            if ((!isRefr && !isAchr) || a_curCellID == 0 || (rflags & 0x00040000u) != 0) {
                continue;
            }

            const RE::FormID runtimeRefID = ResolveRuntimeFormID(a_mod, rawFid);
            if (runtimeRefID == 0) {
                continue;
            }

            // Look up or initialize the target cell state from the engine's live TESObjectCELL
            auto cellIt = a_cells.find(a_curCellID);
            if (cellIt == a_cells.end()) {
                auto* liveCell = RE::TESForm::LookupByID<RE::TESObjectCELL>(a_curCellID);
                if (!liveCell || !liveCell->IsInteriorCell()) {
                    continue;
                }
                const char* cEdid = liveCell->GetFormEditorID();
                std::string edidStr = cEdid ? cEdid : "";
                if (IsExcludedInteriorEdid(edidStr)) {
                    continue;
                }
                RawCellBuildState st{};
                st.cellID = a_curCellID;
                st.edid = std::move(edidStr);
                st.locationID = liveCell->GetLocation() ? liveCell->GetLocation()->GetFormID() : 0;
                st.ownerID = liveCell->GetOwner() ? liveCell->GetOwner()->GetFormID() : 0;
                cellIt = a_cells.emplace(a_curCellID, std::move(st)).first;
            }

            // If an override marks this reference as Deleted (0x20) or InitiallyDisabled (0x800), erase it!
            if ((rflags & 0x820u) != 0) {
                if (isRefr) {
                    cellIt->second.refsByID.erase(runtimeRefID);
                    a_doors.erase(runtimeRefID);
                } else {
                    cellIt->second.actorsByID.erase(runtimeRefID);
                }
                continue;
            }

            // Parse subrecords of this REFR / ACHR
            RE::FormID    rawBaseID = 0;
            RE::FormID    rawOwnerID = 0;
            RE::FormID    rawLinkedDoorID = 0;
            float         posRot[6]{};
            float         telPosRot[6]{};
            float         scaleF = 1.0f;
            bool          hasData = false;
            bool          hasXtel = false;
            bool          hasXespOrXprm = false;

            std::size_t sp = 0;
            while (sp + 6 <= rsize) {
                char          st[4]{};
                std::uint16_t sz = 0;
                std::memcpy(st, body + sp, 4);
                std::memcpy(&sz, body + sp + 4, 2);
                if (std::memcmp(st, "XXXX", 4) == 0 && sz == 4 && sp + 16 <= rsize) {
                    std::uint32_t extSz = 0;
                    std::memcpy(&extSz, body + sp + 6, 4);
                    sp += 16 + extSz;
                    continue;
                }
                if (sp + 6 + sz > rsize) {
                    break;
                }
                const std::uint8_t* val = body + sp + 6;
                if (std::memcmp(st, "NAME", 4) == 0 && sz == 4) {
                    std::memcpy(&rawBaseID, val, 4);
                } else if (std::memcmp(st, "DATA", 4) == 0 && sz == 24) {
                    std::memcpy(posRot, val, 24);
                    hasData = true;
                } else if (std::memcmp(st, "XSCL", 4) == 0 && sz == 4) {
                    std::memcpy(&scaleF, val, 4);
                } else if (std::memcmp(st, "XOWN", 4) == 0 && sz >= 4) {
                    std::memcpy(&rawOwnerID, val, 4);
                } else if (std::memcmp(st, "XTEL", 4) == 0 && sz >= 28) {
                    std::memcpy(&rawLinkedDoorID, val, 4);
                    std::memcpy(telPosRot, val + 4, 24);
                    hasXtel = true;
                } else if (std::memcmp(st, "XESP", 4) == 0 || std::memcmp(st, "XPRM", 4) == 0) {
                    hasXespOrXprm = true;
                }
                sp += 6 + sz;
            }

            if (hasXespOrXprm || !hasData || rawBaseID == 0) {
                cellIt->second.refsByID.erase(runtimeRefID);
                cellIt->second.actorsByID.erase(runtimeRefID);
                continue;
            }

            if (isAchr) {
                ActorDisk ad{};
                ad.achrRefID = runtimeRefID;
                std::memcpy(ad.pos, posRot, 12);
                std::memcpy(ad.rot, posRot + 3, 12);
                cellIt->second.actorsByID[runtimeRefID] = ad;
                continue;
            }

            const RE::FormID runtimeBaseID = ResolveRuntimeFormID(a_mod, rawBaseID);
            std::uint16_t    refFlags = 0;
            if (!IsClonableRuntimeBase(runtimeBaseID, refFlags)) {
                cellIt->second.refsByID.erase(runtimeRefID);
                continue;
            }

            if (hasXtel) {
                const RE::FormID runtimeLinkedDoor = ResolveRuntimeFormID(a_mod, rawLinkedDoorID);
                if (runtimeLinkedDoor != 0) {
                    refFlags |= 1;
                    DoorDisk dd{};
                    dd.doorRefID = runtimeRefID;
                    dd.parentCellID = a_curCellID;
                    dd.linkedDoorID = runtimeLinkedDoor;
                    std::memcpy(dd.telPos, telPosRot, 12);
                    std::memcpy(dd.telRot, telPosRot + 3, 12);
                    dd.isParentInterior = 1;
                    a_doors[runtimeRefID] = dd;
                }
            }

            RefDisk rd{};
            rd.origRefID = runtimeRefID;
            rd.baseID = runtimeBaseID;
            rd.ownerID = ResolveRuntimeFormID(a_mod, rawOwnerID);
            std::memcpy(rd.pos, posRot, 12);
            std::memcpy(rd.rot, posRot + 3, 12);
            rd.scale = static_cast<std::uint16_t>(std::clamp(static_cast<int>(std::lround(scaleF * 100.0f)), 10, 10000));
            rd.flags = refFlags;
            cellIt->second.refsByID[runtimeRefID] = rd;
        }
    }

    inline bool TryLoadCachedBlueprints(
        const char* a_cachePath,
        std::uint64_t a_expectedFingerprint,
        std::unordered_map<RE::FormID, DoorDisk>& a_outDoors,
        std::unordered_map<RE::FormID, CellBlueprint>& a_outCells)
    {
        std::ifstream f(a_cachePath, std::ios::binary);
        if (!f.is_open()) {
            return false;
        }
        FileHeaderDisk hdr{};
        f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
        if (!f) {
            return false;
        }
        // Require PKC2 header (v2 or v3) with matching load-order fingerprint
        if (std::memcmp(hdr.magic, "PKC2", 4) != 0 || (hdr.version != 2 && hdr.version != 3) || hdr.loadFingerprint != a_expectedFingerprint) {
            return false;
        }

        a_outDoors.clear();
        a_outCells.clear();
        for (std::uint32_t i = 0; i < hdr.doorCount; ++i) {
            DoorDisk d{};
            f.read(reinterpret_cast<char*>(&d), sizeof(d));
            if (!f) {
                return false;
            }
            a_outDoors[d.doorRefID] = d;
        }
        for (std::uint32_t i = 0; i < hdr.cellCount; ++i) {
            CellHeaderDisk ch{};
            f.read(reinterpret_cast<char*>(&ch), sizeof(ch));
            if (!f) {
                return false;
            }
            ch.edid[sizeof(ch.edid) - 1] = '\0';
            CellBlueprint cb{};
            cb.cellID = ch.cellID;
            cb.locationID = ch.locationID;
            cb.ownerID = ch.ownerID;
            cb.edid = ch.edid;
            cb.refs.resize(ch.refCount);
            if (ch.refCount > 0) {
                f.read(reinterpret_cast<char*>(cb.refs.data()), ch.refCount * sizeof(RefDisk));
            }
            cb.actors.resize(ch.actorCount);
            if (ch.actorCount > 0) {
                f.read(reinterpret_cast<char*>(cb.actors.data()), ch.actorCount * sizeof(ActorDisk));
            }
            if (!f) {
                return false;
            }
            a_outCells[cb.cellID] = std::move(cb);
        }
        return true;
    }

    inline void RebuildAndSaveBlueprints(
        const char* a_cachePath,
        std::uint64_t a_fingerprint,
        const std::vector<const RE::TESFile*>& a_loadedFiles,
        std::uint32_t a_maxRefsPerCell,
        std::unordered_map<RE::FormID, DoorDisk>& a_outDoors,
        std::unordered_map<RE::FormID, CellBlueprint>& a_outCells)
    {
        const auto t0 = std::chrono::steady_clock::now();
        std::unordered_map<RE::FormID, RawCellBuildState> rawCells;
        a_outDoors.clear();
        a_outCells.clear();

        std::size_t scannedFilesWithCell = 0;
        std::vector<std::uint8_t> grupBuf;

        for (const RE::TESFile* mod : a_loadedFiles) {
            if (!mod) {
                continue;
            }
            std::string path = "Data/";
            path += mod->GetFilename();
            std::ifstream f(path, std::ios::binary);
            if (!f.is_open()) {
                continue;
            }

            char          tes4Hdr[24]{};
            std::uint32_t tes4Size = 0;
            f.read(tes4Hdr, 24);
            if (!f || std::memcmp(tes4Hdr, "TES4", 4) != 0) {
                continue;
            }
            std::memcpy(&tes4Size, tes4Hdr + 4, 4);
            f.seekg(24ULL + tes4Size, std::ios::beg);

            bool hadCellGrup = false;
            while (f) {
                char          gHdr[24]{};
                std::uint32_t gSize = 0;
                std::int32_t  gType = 0;
                f.read(gHdr, 24);
                if (!f || std::memcmp(gHdr, "GRUP", 4) != 0) {
                    break;
                }
                std::memcpy(&gSize, gHdr + 4, 4);
                std::memcpy(&gType, gHdr + 12, 4);
                if (gSize < 24) {
                    break;
                }
                if (gType == 0 && std::memcmp(gHdr + 8, "CELL", 4) == 0) {
                    const std::uint32_t payloadSize = gSize - 24;
                    if (payloadSize > 0) {
                        grupBuf.resize(payloadSize);
                        f.read(reinterpret_cast<char*>(grupBuf.data()), payloadSize);
                        if (!f) {
                            break;
                        }
                        hadCellGrup = true;
                        WalkCellGrupBuffer(mod, grupBuf.data(), 0, payloadSize, 0, rawCells, a_outDoors);
                    }
                } else {
                    // Fast skip non-CELL top-level GRUP in O(1) without reading payload
                    f.seekg(static_cast<std::streamoff>(gSize - 24), std::ios::cur);
                }
            }
            if (hadCellGrup) {
                ++scannedFilesWithCell;
            }
        }

        // Also register exterior linked doors so bidirectional lookups work even when an interior door's
        // ExtraTeleport is not yet resolved
        std::vector<DoorDisk> extraExtDoors;
        for (const auto& [intDoorID, dd] : a_outDoors) {
            if (dd.linkedDoorID != 0 && a_outDoors.find(dd.linkedDoorID) == a_outDoors.end()) {
                if (auto* extDoorRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(dd.linkedDoorID)) {
                    auto* pCell = extDoorRef->GetParentCell();
                    if (!pCell) {
                        pCell = extDoorRef->GetSaveParentCell();
                    }
                    if (pCell) {
                        DoorDisk extDd{};
                        extDd.doorRefID = dd.linkedDoorID;
                        extDd.parentCellID = pCell->GetFormID();
                        extDd.linkedDoorID = intDoorID;
                        extDd.isParentInterior = pCell->IsInteriorCell() ? 1u : 0u;
                        if (auto* xTel = extDoorRef->extraList.GetByType<RE::ExtraTeleport>(); xTel && xTel->teleportData) {
                            extDd.telPos[0] = xTel->teleportData->position.x;
                            extDd.telPos[1] = xTel->teleportData->position.y;
                            extDd.telPos[2] = xTel->teleportData->position.z;
                            extDd.telRot[0] = xTel->teleportData->rotation.x;
                            extDd.telRot[1] = xTel->teleportData->rotation.y;
                            extDd.telRot[2] = xTel->teleportData->rotation.z;
                        }
                        extraExtDoors.push_back(extDd);
                    }
                }
            }
        }
        for (const auto& extDd : extraExtDoors) {
            a_outDoors.emplace(extDd.doorRefID, extDd);
        }

        // Filter and finalize valid interior cells (>= 5 STATs, >= 1 teleport door, 15 <= refs <= a_maxRefsPerCell)
        std::size_t totalRefs = 0;
        std::size_t totalActors = 0;
        for (auto& [cid, st] : rawCells) {
            if (st.refsByID.size() < 15 || st.refsByID.size() > a_maxRefsPerCell) {
                continue;
            }
            std::size_t statCnt = 0;
            std::size_t doorCnt = 0;
            for (const auto& [rid, r] : st.refsByID) {
                if ((r.flags & 4) != 0) {
                    ++statCnt;
                }
                if ((r.flags & 1) != 0) {
                    ++doorCnt;
                }
            }
            if (statCnt < 5 || doorCnt < 1) {
                continue;
            }

            CellBlueprint cb{};
            cb.cellID = st.cellID;
            cb.locationID = st.locationID;
            cb.ownerID = st.ownerID;
            cb.edid = std::move(st.edid);
            cb.refs.reserve(st.refsByID.size());
            for (const auto& [rid, r] : st.refsByID) {
                cb.refs.push_back(r);
            }
            cb.actors.reserve(st.actorsByID.size());
            for (const auto& [aid, a] : st.actorsByID) {
                cb.actors.push_back(a);
            }
            totalRefs += cb.refs.size();
            totalActors += cb.actors.size();
            a_outCells.emplace(cid, std::move(cb));
        }

        // Persist cache to disk
        std::ofstream out(a_cachePath, std::ios::binary | std::ios::trunc);
        if (out.is_open()) {
            FileHeaderDisk hdr{};
            std::memcpy(hdr.magic, "PKC2", 4);
            hdr.version = 3;
            hdr.loadFingerprint = a_fingerprint;
            hdr.cellCount = static_cast<std::uint32_t>(a_outCells.size());
            hdr.doorCount = static_cast<std::uint32_t>(a_outDoors.size());
            out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

            for (const auto& [did, d] : a_outDoors) {
                out.write(reinterpret_cast<const char*>(&d), sizeof(d));
            }
            for (const auto& [cid, cb] : a_outCells) {
                CellHeaderDisk ch{};
                ch.cellID = cb.cellID;
                ch.locationID = cb.locationID;
                ch.ownerID = cb.ownerID;
                ch.refCount = static_cast<std::uint32_t>(cb.refs.size());
                ch.actorCount = static_cast<std::uint32_t>(cb.actors.size());
                std::strncpy(ch.edid, cb.edid.c_str(), sizeof(ch.edid) - 1);
                out.write(reinterpret_cast<const char*>(&ch), sizeof(ch));
                if (!cb.refs.empty()) {
                    out.write(reinterpret_cast<const char*>(cb.refs.data()), cb.refs.size() * sizeof(RefDisk));
                }
                if (!cb.actors.empty()) {
                    out.write(reinterpret_cast<const char*>(cb.actors.data()), cb.actors.size() * sizeof(ActorDisk));
                }
            }
        }

        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        logger::info("[LF] Rebuilt Blueprint cache in {} ms (scanned {} plugins with CELL groups / {} total): "
                     "{} interior cells, {} doors, {} refs, {} actors (fingerprint=0x{:016X})",
            ms, scannedFilesWithCell, a_loadedFiles.size(),
            a_outCells.size(), a_outDoors.size(), totalRefs, totalActors, a_fingerprint);
    }
}


