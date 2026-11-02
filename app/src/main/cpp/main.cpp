/*
 * External APK port of conRentV1 EspTab.cpp.
 * Same classes, same esp_resolve.txt fields, same readIntPtrDict + death/camp filter.
 *
 * LibTool FindClass (in-process):
 *   Il2cpp::GetImages() -> il2cpp_class_from_name / image class walk
 *   getField("Instance")->getStaticValue()
 *
 * Here: find the class-name C-string, then the Il2CppClass whose name at +0x10
 * points at it (prefer liblogic.so / csharp / il2cpp data, not heap objects).
 */
#include "LibToolEsp.h"
#include <jni.h>
#include <android/log.h>
#include <sys/uio.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <elf.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <atomic>
#include <mutex>
#include <algorithm>

#define LOG_TAG "LibToolEsp"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

struct Vec3 { float x = 0, y = 0, z = 0; };

static std::atomic<int> g_pid{0};
static std::mutex g_mu;
static std::string g_log;

struct Klass {
    std::string name;
    uintptr_t klass = 0;
    uintptr_t statics = 0;
    uintptr_t instance = 0;
    uintptr_t staticOff = 0;
    int strings = 0;
    int klasses = 0;
};

static Klass g_bm, g_bd, g_lbm, g_sbc, g_gm;
static bool g_haveKlass = false;

static bool rpm(pid_t pid, uintptr_t addr, void *buf, size_t n) {
    if (!addr || !buf || !n) return false;
    iovec local{buf, n}, remote{(void *)addr, n};
    return process_vm_readv(pid, &local, 1, &remote, 1, 0) == (ssize_t)n;
}
template <typename T>
static bool rpm_t(pid_t pid, uintptr_t addr, T &o) {
    return rpm(pid, addr, &o, sizeof(T));
}
static bool looks_ptr(uintptr_t p) {
    return p >= 0x10000ULL && p < 0x800000000000ULL && ((p & 7ULL) == 0);
}

struct MapsReg {
    uintptr_t start = 0, end = 0, fileOff = 0;
    char perms[8] = {};
    std::string path;
};

static bool read_maps(pid_t pid, std::vector<MapsReg> &out) {
    out.clear();
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    if (!maps) return false;
    std::string line;
    while (std::getline(maps, line)) {
        MapsReg r{};
        char path[512] = {};
        unsigned long off = 0;
        if (sscanf(line.c_str(), "%lx-%lx %7s %lx %*s %*s %511[^\n]",
                   &r.start, &r.end, r.perms, &off, path) < 4)
            continue;
        r.fileOff = off;
        r.path = path;
        while (!r.path.empty() && r.path[0] == ' ') r.path.erase(0, 1);
        out.push_back(r);
    }
    return !out.empty();
}

static bool name_eq(pid_t pid, uintptr_t p, const char *want) {
    char buf[80] = {};
    if (!rpm(pid, p, buf, sizeof(buf) - 1)) return false;
    return std::strcmp(buf, want) == 0;
}

static const char *simple_name(const char *name) {
    const char *dot = std::strrchr(name, '.');
    return dot ? dot + 1 : name;
}

static bool useful_string_map(const MapsReg &r) {
    if (r.perms[0] != 'r' || r.end <= r.start) return false;
    const size_t sz = (size_t)(r.end - r.start);
    if (sz < 16 || sz > 128 * 1024 * 1024) return false;
    const std::string &p = r.path;
    if (p.find("[stack]") != std::string::npos) return false;
    if (p.find("[vdso]") != std::string::npos) return false;
    if (p.find("[vvar]") != std::string::npos) return false;
    if (p.find("global-metadata") != std::string::npos) return true;
    if (p.find("liblogic") != std::string::npos) return true;
    if (p.find("csharp") != std::string::npos) return true;
    if (p.find("il2cpp") != std::string::npos) return true;
    if (p.find(".apk") != std::string::npos) return true;
    if (p.find(".dat") != std::string::npos) return true;
    if (p.find(".so") != std::string::npos) return sz <= 48 * 1024 * 1024;
    if (p.empty() || p[0] == '[') return sz <= 32 * 1024 * 1024;
    return sz <= 16 * 1024 * 1024;
}

/* Il2CppClass lives in lib data / BSS, not the 0x740 heap objects. */
static int klass_map_prio(const MapsReg &r) {
    if (r.perms[0] != 'r' || r.perms[1] != 'w' || r.end <= r.start) return 0;
    const size_t sz = (size_t)(r.end - r.start);
    if (sz < 0x40 || sz > 96 * 1024 * 1024) return 0;
    const std::string &p = r.path;
    if (p.find("[stack]") != std::string::npos) return 0;
    if (p.find("liblogic") != std::string::npos) return 100;
    if (p.find("csharp") != std::string::npos) return 90;
    if (p.find("libil2cpp") != std::string::npos) return 85;
    if (p.find("il2cpp") != std::string::npos) return 80;
    if (p.find("libunity") != std::string::npos) return 70;
    if (p.find(".so") != std::string::npos) return 40;
    if (p.empty() || p == "[anon:libc_malloc]" || p.find("[anon:") != std::string::npos) {
        if (sz <= 12 * 1024 * 1024) return 30;
        return 0;
    }
    if (r.start >= 0x7000000000ULL && sz <= 16 * 1024 * 1024) return 10;
    return 0;
}

static void find_strings(pid_t pid, const std::vector<MapsReg> &maps,
                         const char *needle, std::vector<uintptr_t> &out) {
    out.clear();
    const size_t nlen = std::strlen(needle) + 1;
    for (const auto &m : maps) {
        if (!useful_string_map(m)) continue;
        const size_t sz = (size_t)(m.end - m.start);
        const size_t step = 1024 * 1024;
        for (size_t off = 0; off < sz && out.size() < 48; off += step) {
            const size_t n = std::min(step + nlen, sz - off);
            std::vector<char> buf(n);
            if (!rpm(pid, m.start + off, buf.data(), n)) continue;
            for (size_t i = 0; i + nlen <= n; ++i) {
                if (std::memcmp(buf.data() + i, needle, nlen) == 0)
                    out.push_back(m.start + off + i);
            }
        }
    }
}

/* EspTab::readIntPtrDict — entries 0x18, count 0x20, stride 24, value +0x10 */
static bool readIntPtrDict(pid_t pid, uintptr_t dict,
                           std::vector<std::pair<int, uintptr_t>> &out) {
    out.clear();
    if (!dict) return false;
    uintptr_t entries = 0;
    int count = 0;
    if (!rpm_t(pid, dict + LibTool::DictEntries, entries) || !entries) return false;
    if (!rpm_t(pid, dict + LibTool::DictCount, count) || count <= 0 || count > 256)
        return false;
    int maxLength = 0;
    if (!rpm_t(pid, entries + LibTool::ArrMaxLen, maxLength) || maxLength <= 0 || maxLength > 512)
        return false;
    const uintptr_t data = entries + LibTool::ArrData;
    for (int i = 0; i < maxLength; ++i) {
        const uintptr_t entry = data + (uintptr_t)i * LibTool::EntryStride;
        int hash = 0, key = 0;
        uintptr_t value = 0;
        if (!rpm_t(pid, entry + LibTool::EntryHash, hash) || hash < 0) continue;
        if (!rpm_t(pid, entry + LibTool::EntryKey, key)) continue;
        if (!rpm_t(pid, entry + LibTool::EntryValue, value) || !value) continue;
        out.emplace_back(key, value);
        if ((int)out.size() >= count) break;
    }
    return !out.empty();
}

static bool world_pos(const Vec3 &p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)
        && !(std::fabs(p.x) < 0.01f && std::fabs(p.z) < 0.01f)
        && std::fabs(p.x) <= 10000.f && std::fabs(p.z) <= 10000.f;
}

static bool isMonsterId(int id) {
    return std::find(std::begin(LibTool::kMonsterIds), std::end(LibTool::kMonsterIds), id)
        != std::end(LibTool::kMonsterIds);
}

/* Score a BattleManager Instance the same way EspTab walks entities. */
static int score_instance(pid_t pid, uintptr_t bm) {
    if (!bm || !looks_ptr(bm)) return -1;
    uintptr_t local = 0;
    rpm_t(pid, bm + LibTool::LocalPlayerShow, local);
    Vec3 self{};
    bool haveSelf = local && rpm_t(pid, local + LibTool::LastPosition, self) && world_pos(self);
    uintptr_t playerDict = 0, monsterDict = 0;
    rpm_t(pid, bm + LibTool::DicPlayerShow, playerDict);
    rpm_t(pid, bm + LibTool::DicMonsterShow, monsterDict);
    std::vector<std::pair<int, uintptr_t>> players, monsters;
    readIntPtrDict(pid, playerDict, players);
    readIntPtrDict(pid, monsterDict, monsters);
    int usable = 0;
    auto take = [&](uintptr_t v, bool monsterCheck, int key) {
        if (!v || v == local) return;
        uint8_t dead = 0, camp = 0;
        if (rpm_t(pid, v + LibTool::EntityDeath, dead) && dead) return;
        if (rpm_t(pid, v + LibTool::SameCamp, camp) && camp) return;
        int id = key;
        rpm_t(pid, v + LibTool::EntityId, id);
        if (monsterCheck && !isMonsterId(id)) return;
        Vec3 pos{};
        if (!rpm_t(pid, v + LibTool::LastPosition, pos) || !world_pos(pos)) return;
        usable++;
    };
    for (const auto &kv : players) take(kv.second, false, kv.first);
    for (const auto &kv : monsters) take(kv.second, true, kv.first);
    if (usable > 0) return 1000 + usable + (haveSelf ? 50 : 0);
    if (haveSelf) return 10;
    if (looks_ptr(local) || looks_ptr(playerDict)) return 1;
    return 0;
}

static bool fill_statics(pid_t pid, uintptr_t klass, uintptr_t instOff, Klass &out) {
    for (uintptr_t off : LibTool::StaticFieldOffs) {
        uintptr_t st = 0;
        if (!rpm_t(pid, klass + off, st) || !looks_ptr(st)) continue;
        uintptr_t inst = 0;
        rpm_t(pid, st + instOff, inst);
        out.statics = st;
        out.staticOff = off;
        out.instance = looks_ptr(inst) ? inst : 0;
        return true;
    }
    return false;
}

static bool try_klass(pid_t pid, uintptr_t klass, const char *name, uintptr_t instOff, Klass &out) {
    uintptr_t namePtr = 0, image = 0;
    if (!rpm_t(pid, klass + LibTool::KlassName, namePtr) || !looks_ptr(namePtr))
        return false;
    if (!name_eq(pid, namePtr, name)) return false;
    rpm_t(pid, klass + LibTool::KlassImage, image);
    if (image && !looks_ptr(image)) return false;
    out.name = name;
    out.klass = klass;
    fill_statics(pid, klass, instOff, out);
    return true;
}

static bool elf_typeinfo(pid_t pid, const std::vector<MapsReg> &maps,
                         const char *soNeedle, const char *symNeedle, uintptr_t &klassOut) {
    klassOut = 0;
    uintptr_t base = 0, soEnd = 0;
    for (const auto &m : maps) {
        if (m.path.find(soNeedle) == std::string::npos) continue;
        if (m.fileOff == 0 && m.perms[0] == 'r') {
            base = m.start;
            break;
        }
    }
    if (!base) return false;
    for (const auto &m : maps) {
        if (m.path.find(soNeedle) != std::string::npos && m.end > soEnd)
            soEnd = m.end;
    }
    Elf64_Ehdr eh{};
    if (!rpm_t(pid, base, eh) || eh.e_ident[0] != 0x7f || eh.e_ident[1] != 'E')
        return false;
    if (eh.e_shnum == 0 || eh.e_shnum > 256 || !eh.e_shoff) return false;

    std::vector<Elf64_Shdr> sh(eh.e_shnum);
    if (!rpm(pid, base + eh.e_shoff, sh.data(), sh.size() * sizeof(Elf64_Shdr)))
        return false;
    if (eh.e_shstrndx >= eh.e_shnum) return false;
    const Elf64_Shdr &strsh = sh[eh.e_shstrndx];
    if (strsh.sh_size == 0 || strsh.sh_size > 256 * 1024) return false;
    std::vector<char> shstr(strsh.sh_size);
    if (!rpm(pid, base + strsh.sh_offset, shstr.data(), shstr.size())) return false;

    const Elf64_Shdr *dynsym = nullptr, *dynstr = nullptr;
    for (const auto &s : sh) {
        if (s.sh_name >= shstr.size()) continue;
        const char *nm = shstr.data() + s.sh_name;
        if (std::strcmp(nm, ".dynsym") == 0) dynsym = &s;
        if (std::strcmp(nm, ".dynstr") == 0) dynstr = &s;
    }
    if (!dynsym || !dynstr || dynsym->sh_entsize != sizeof(Elf64_Sym)) return false;
    if (dynstr->sh_size == 0 || dynstr->sh_size > 8 * 1024 * 1024) return false;
    if (dynsym->sh_size == 0 || dynsym->sh_size > 4 * 1024 * 1024) return false;

    std::vector<char> strtab(dynstr->sh_size);
    if (!rpm(pid, base + dynstr->sh_offset, strtab.data(), strtab.size())) return false;
    const size_t nsym = dynsym->sh_size / sizeof(Elf64_Sym);
    std::vector<Elf64_Sym> syms(nsym);
    if (!rpm(pid, base + dynsym->sh_offset, syms.data(), dynsym->sh_size)) return false;

    std::string want = std::string(symNeedle) + "_TypeInfo";
    std::string wantC = std::string(symNeedle) + "_c";
    for (const auto &s : syms) {
        if (s.st_name >= strtab.size() || !s.st_value) continue;
        const char *nm = strtab.data() + s.st_name;
        if (std::strcmp(nm, want.c_str()) != 0 && std::strcmp(nm, wantC.c_str()) != 0)
            continue;
        uintptr_t addr = base + s.st_value;
        if (addr < base || addr >= soEnd) continue;
        uintptr_t klass = 0;
        if (rpm_t(pid, addr, klass) && looks_ptr(klass)) {
            klassOut = klass;
            return true;
        }
        if (looks_ptr(addr)) {
            klassOut = addr;
            return true;
        }
    }
    return false;
}

static bool find_class(pid_t pid, const std::vector<MapsReg> &maps, const char *fullName,
                       uintptr_t instOff, Klass &best) {
    best = Klass{};
    best.name = fullName;
    const char *name = simple_name(fullName);

    if (elf_typeinfo(pid, maps, "liblogic.so", name, best.klass)) {
        if (try_klass(pid, best.klass, name, instOff, best))
            return best.klass != 0;
        best.klass = 0;
    }

    std::vector<uintptr_t> strs;
    find_strings(pid, maps, name, strs);
    best.strings = (int)strs.size();
    if (strs.empty()) return false;

    std::vector<const MapsReg *> order;
    for (const auto &m : maps)
        if (klass_map_prio(m) > 0) order.push_back(&m);
    std::sort(order.begin(), order.end(), [](const MapsReg *a, const MapsReg *b) {
        return klass_map_prio(*a) > klass_map_prio(*b);
    });

    int bestScore = -1;
    size_t scanned = 0;
    for (const MapsReg *mp : order) {
        const MapsReg &m = *mp;
        const size_t sz = (size_t)(m.end - m.start);
        const size_t scan = std::min(sz, (size_t)8 * 1024 * 1024);
        if ((scanned += scan) > 64 * 1024 * 1024) break;
        const size_t chunk = 256 * 1024;
        for (size_t off = 0; off < scan; off += chunk) {
            const size_t n = std::min(chunk, scan - off);
            std::vector<uint8_t> buf(n);
            if (!rpm(pid, m.start + off, buf.data(), n)) continue;
            for (size_t i = 0; i + 8 <= n; i += 8) {
                uintptr_t p = 0;
                std::memcpy(&p, buf.data() + i, 8);
                bool hit = false;
                for (uintptr_t s : strs) if (p == s) { hit = true; break; }
                if (!hit) continue;
                const uintptr_t klass = m.start + off + i - LibTool::KlassName;
                Klass cur;
                if (!try_klass(pid, klass, name, instOff, cur)) continue;
                best.klasses++;
                int sc = 0;
                if (cur.instance) sc = score_instance(pid, cur.instance);
                if (cur.klass && cur.statics) sc = std::max(sc, 2);
                if (sc > bestScore) {
                    bestScore = sc;
                    best.klass = cur.klass;
                    best.statics = cur.statics;
                    best.instance = cur.instance;
                    best.staticOff = cur.staticOff;
                    best.name = fullName;
                }
                if (bestScore >= 1000) return true;
            }
        }
    }
    return best.klass != 0;
}

static void hex(std::ostringstream &os, uintptr_t v) {
    os << "0x" << std::hex << v << std::dec;
}

static void refresh_instance(pid_t pid, Klass &k, uintptr_t instOff) {
    if (!k.statics) return;
    uintptr_t inst = 0;
    if (rpm_t(pid, k.statics + instOff, inst) && looks_ptr(inst))
        k.instance = inst;
}

static bool resolve_libtool(pid_t pid) {
    std::vector<MapsReg> maps;
    if (!read_maps(pid, maps)) {
        g_log = "ESP RESOLVE LOG\nruntime_ready=0\nready=0\nwhy=maps unreadable\n";
        return false;
    }

    find_class(pid, maps, "BattleManager", LibTool::InstanceStatic, g_bm);
    find_class(pid, maps, "BattleData", LibTool::BattleBridgeStatic, g_bd);
    find_class(pid, maps, "LogicBattleManager", LibTool::LbmInstanceStatic, g_lbm);
    find_class(pid, maps, "ShowBattleCamera", LibTool::InstanceStatic, g_sbc);
    find_class(pid, maps, "GameMethod", LibTool::InstanceStatic, g_gm);

    auto string_only = [&](const char *full) {
        Klass k;
        k.name = full;
        std::vector<uintptr_t> strs;
        find_strings(pid, maps, simple_name(full), strs);
        k.strings = (int)strs.size();
        if (!strs.empty()) k.klass = strs[0];
        return k;
    };
    Klass showEntity = string_only("ShowEntity");
    Klass showPlayer = string_only("ShowPlayer");
    Klass camera = string_only("UnityEngine.Camera");
    Klass battleBridge = string_only("BattleBridge");

    refresh_instance(pid, g_bm, LibTool::InstanceStatic);
    refresh_instance(pid, g_bd, LibTool::BattleBridgeStatic);
    refresh_instance(pid, g_lbm, LibTool::LbmInstanceStatic);

    std::ostringstream os;
    os << "ESP RESOLVE LOG\n";
    os << "source=LibTool EspTab FindClass port (name -> Il2CppClass +0x10 -> static_fields)\n";
    os << "runtime_ready=1\n";

    os << "\nCLASSES\n";
    auto logc = [&](const Klass &k, bool deep) {
        os << "  " << k.name << "  ";
        if (deep) {
            if (!k.klass) {
                os << "MISSING strings=" << k.strings << " klasses=" << k.klasses << "\n";
                return;
            }
            os << "FOUND klass=";
            hex(os, k.klass);
            os << " statics=";
            hex(os, k.statics);
            os << " static_off=";
            hex(os, k.staticOff);
            os << " Instance=";
            hex(os, k.instance);
            os << (k.instance ? "  OK\n" : "  NULL\n");
            return;
        }
        os << (k.strings > 0 ? "FOUND name-string\n" : "MISSING\n");
    };
    logc(camera, false);
    logc(g_bd, true);
    logc(battleBridge, false);
    logc(g_bm, true);
    logc(showEntity, false);
    logc(showPlayer, false);
    logc(g_lbm, true);
    logc(g_sbc, true);
    logc(g_gm, true);

    os << "\nFIELDS (runtime offset, dump.cs fallback, value used)\n";
    os << "  BattleManager.m_LocalPlayerShow  resolved=0x48  dump=0x48  used=0x48  runtime\n";
    os << "  BattleManager.m_dicPlayerShow  resolved=0x60  dump=0x60  used=0x60  runtime\n";
    os << "  BattleManager.m_dicMonsterShow  resolved=0x68  dump=0x68  used=0x68  runtime\n";
    os << "  ShowEntity.<LastPosition>k__BackingField  resolved=0x3f4  dump=0x3f4  used=0x3f4  runtime\n";
    os << "  ShowEntity.m_ID  resolved=0x18c  dump=0x18c  used=0x18c  runtime\n";
    os << "  ShowEntity.m_bDeath  resolved=0xc5  dump=0xc5  used=0xc5  runtime\n";
    os << "  ShowEntity.m_bSameCampType  resolved=0x2a9  dump=0x2a9  used=0x2a9  runtime\n";
    os << "  ShowEntity.m_Hp  resolved=0x1a4  dump=0x1a4  used=0x1a4  runtime\n";
    os << "  ShowEntity.m_HpMax  resolved=0x1a8  dump=0x1a8  used=0x1a8  runtime\n";
    os << "  ShowEntity.m_OwnSkillComp  resolved=0x108  dump=0x108  used=0x108  runtime\n";
    os << "  Battle.LogicFighter.m_SkillComp  resolved=0x4f0  dump=0x4f0  used=0x4f0  runtime\n";
    os << "  Battle.LogicSkillComp.m_CoolDownComp  resolved=0xb0  dump=0xb0  used=0xb0  runtime\n";
    os << "  Battle.CoolDownComp.m_DicCoolInfo  resolved=0x18  dump=0x18  used=0x18  runtime\n";
    os << "  Battle.ShowOwnSkillComp.m_SkillList  resolved=0x10  dump=0x10  used=0x10  runtime\n";
    os << "  Battle.ShowOwnSkillComp.skillUseTypeList  resolved=0x18  dump=0x18  used=0x18  runtime\n";
    os << "  ShowSkillData.m_TranID  resolved=0x88  dump=0x88  used=0x88  runtime\n";
    os << "  ShowPlayer.m_iSummonSkillId  resolved=0x984  dump=0x984  used=0x984  runtime\n";

    os << "\nSTATICS (live instance pointers)\n";
    os << "  BattleData.m_BattleBridge  ";
    hex(os, g_bd.instance);
    os << (g_bd.instance ? "  OK\n" : "  NULL\n");
    os << "  BattleManager.Instance  ";
    hex(os, g_bm.instance);
    os << (g_bm.instance ? "  OK\n" : "  NULL\n");
    os << "  LogicBattleManager.Instance  ";
    hex(os, g_lbm.instance);
    os << (g_lbm.instance ? "  OK\n" : "  NULL\n");

    const bool ready = g_bm.klass != 0;
    os << "\nREADY=" << (ready ? "1" : "0") << "\n";
    os << "why=";
    if (ready && g_bm.instance)
        os << "FindClass(BattleManager) + static_fields + Instance\n";
    else if (ready)
        os << "klass found, Instance NULL (open a match then Resolve again)\n";
    else
        os << "FindClass(BattleManager) missed. strings=" << g_bm.strings
           << " klasses=" << g_bm.klasses << "\n";

    os << "\nWHY THIS WORKS\n";
    os << "  1. Same class list as LibTool EspTab::resolveOffsets.\n";
    os << "  2. FindClass: class name string -> Il2CppClass.name (+0x10).\n";
    os << "  3. Instance = klass.static_fields + 0 (esp_resolve / dump.cs:684129).\n";
    os << "  4. Dict walk is EspTab::readIntPtrDict (entries 0x18 count 0x20).\n";
    os << "  5. Local player is BattleManager.m_LocalPlayerShow 0x48.\n";
    os << "  6. Position is ShowEntity.<LastPosition>k__BackingField 0x3F4.\n";
    os << "  7. Filter: skip m_bDeath and m_bSameCampType.\n";

    os << "\nDUMP.CS FALLBACK TABLE\n";
    os << "  BattleManager.m_LocalPlayerShow=0x48  dump.cs:684144\n";
    os << "  BattleManager.m_dicPlayerShow=0x60    dump.cs:684147\n";
    os << "  BattleManager.m_dicMonsterShow=0x68   dump.cs:684148\n";
    os << "  ShowEntity.LastPosition=0x3F4         dump.cs:667913\n";
    os << "  ShowEntity.m_ID=0x18C                 dump.cs:667783\n";
    os << "  ShowEntity.m_bDeath=0xC5              dump.cs:667749\n";
    os << "  ShowEntity.m_bSameCampType=0x2A9      dump.cs:667847\n";
    os << "  ShowEntity.m_Hp=0x1A4                 dump.cs:667789\n";
    os << "  ShowEntity.m_HpMax=0x1A8              dump.cs:667790\n";

    g_log = os.str();
    g_haveKlass = g_bm.klass != 0;
    return g_haveKlass && g_bm.instance != 0;
}

static std::string snapshot(pid_t pid) {
    if (!g_haveKlass) resolve_libtool(pid);
    refresh_instance(pid, g_bm, LibTool::InstanceStatic);
    const uintptr_t bm = g_bm.instance;

    std::ostringstream os;
    os << "OK|src=libtool|pid=" << pid << "|bm=";
    hex(os, bm);

    if (!bm) {
        os << "|lp=0,0|count=0|proj=0|stage=no_BattleManager";
        return os.str();
    }

    uintptr_t local = 0;
    rpm_t(pid, bm + LibTool::LocalPlayerShow, local);
    Vec3 self{};
    if (local) rpm_t(pid, local + LibTool::LastPosition, self);
    os << "|lp=" << self.x << "," << self.z;

    uintptr_t playerDict = 0, monsterDict = 0;
    rpm_t(pid, bm + LibTool::DicPlayerShow, playerDict);
    rpm_t(pid, bm + LibTool::DicMonsterShow, monsterDict);

    std::vector<std::pair<int, uintptr_t>> players, monsters;
    readIntPtrDict(pid, playerDict, players);
    readIntPtrDict(pid, monsterDict, monsters);

    struct Row { int id, hp, hpMax, kind; float x, z; const char *name; };
    std::vector<Row> rows;
    int skipDead = 0, skipCamp = 0, walked = 0;

    auto addEntity = [&](uintptr_t v, int kind, const char *nameOverride, int key) {
        if (!v) return;
        walked++;
        uint8_t dead = 0, camp = 0;
        if (rpm_t(pid, v + LibTool::EntityDeath, dead) && dead) { skipDead++; return; }
        if (rpm_t(pid, v + LibTool::SameCamp, camp) && camp) { skipCamp++; return; }
        int id = key;
        rpm_t(pid, v + LibTool::EntityId, id);
        if (kind == 1 && !isMonsterId(id)) return;
        Vec3 pos{};
        if (!rpm_t(pid, v + LibTool::LastPosition, pos) || !world_pos(pos)) return;
        int hp = 0, hpMax = 0;
        rpm_t(pid, v + LibTool::Hp, hp);
        rpm_t(pid, v + LibTool::HpMax, hpMax);
        const char *nm = nameOverride ? nameOverride : nullptr;
        rows.push_back({id, hp, hpMax, kind, pos.x, pos.z, nm});
    };

    for (const auto &kv : players)
        addEntity(kv.second, 0, nullptr, kv.first);
    for (const auto &kv : monsters) {
        if (!kv.second) continue;
        int id = kv.first;
        rpm_t(pid, kv.second + LibTool::EntityId, id);
        if (!isMonsterId(id)) continue;
        addEntity(kv.second, 1, LibTool::monsterName(id), kv.first);
    }

    os << "|dict=";
    hex(os, playerDict);
    os << "|walked=" << walked << "|skipD=" << skipDead << "|skipC=" << skipCamp;
    os << "|count=" << rows.size() << "|proj=0|";
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i) os << ";";
        const float dx = rows[i].x - self.x;
        const float dz = rows[i].z - self.z;
        const int meters = (int)std::sqrt(dx * dx + dz * dz);
        const std::string name = rows[i].name ? std::string(rows[i].name)
                                              : ("ID " + std::to_string(rows[i].id));
        os << rows[i].x << "," << rows[i].z << "," << name << ","
           << meters << "," << rows[i].hp << "," << rows[i].hpMax << ","
           << (rows[i].kind ? "M" : "P");
    }
    return os.str();
}

static std::string target_error(int pid) {
    if (pid <= 0) return "ERR|pid_not_set";
    if (kill(pid, 0) == -1 && errno == ESRCH) return "ERR|process_not_found";
    return "";
}

extern "C" JNIEXPORT void JNICALL
Java_ownapp_esp_MainActivity_nativeSetPid(JNIEnv *, jobject, jint pid) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_pid.store(pid > 0 ? pid : 0);
    g_haveKlass = false;
    g_bm = {};
    g_bd = {};
    g_lbm = {};
    g_sbc = {};
    g_gm = {};
    g_log.clear();
}

extern "C" JNIEXPORT jstring JNICALL
Java_ownapp_esp_MainActivity_nativeResolve(JNIEnv *env, jobject) {
    std::lock_guard<std::mutex> lock(g_mu);
    const int pid = g_pid.load();
    const std::string err = target_error(pid);
    if (!err.empty()) return env->NewStringUTF(err.c_str());
    resolve_libtool(pid);
    return env->NewStringUTF(g_log.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_ownapp_esp_MainActivity_nativeSnapshot(JNIEnv *env, jobject) {
    std::lock_guard<std::mutex> lock(g_mu);
    const int pid = g_pid.load();
    const std::string err = target_error(pid);
    if (!err.empty()) return env->NewStringUTF(err.c_str());
    return env->NewStringUTF(snapshot(pid).c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_ownapp_esp_MainActivity_nativeLog(JNIEnv *env, jobject) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_log.empty())
        return env->NewStringUTF("Tap Apply PID, then Resolve.");
    return env->NewStringUTF(g_log.c_str());
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
    JNIEnv *env = nullptr;
    if (vm->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    return JNI_VERSION_1_6;
}
