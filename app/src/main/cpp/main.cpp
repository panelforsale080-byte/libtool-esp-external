#include <jni.h>
#include <android/log.h>
#include <sys/uio.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
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

/* Same field offsets LibTool resolved at runtime (esp_resolve.txt). */
namespace Off {
    constexpr uintptr_t LocalPlayerShow = 0x48;
    constexpr uintptr_t DicPlayerShow = 0x60;
    constexpr uintptr_t DicMonsterShow = 0x68;
    constexpr uintptr_t LastPosition = 0x3F4;
    constexpr uintptr_t EntityId = 0x18C;
    constexpr uintptr_t Death = 0xC5;
    constexpr uintptr_t SameCamp = 0x2A9;
    constexpr uintptr_t Hp = 0x1A4;
    constexpr uintptr_t HpMax = 0x1A8;
    constexpr uintptr_t DictEntries = 0x18;
    constexpr uintptr_t DictCount = 0x20;
    constexpr uintptr_t ArrLen = 0x18;
    constexpr uintptr_t ArrData = 0x20;
    constexpr uintptr_t EntryStride = 24;
    constexpr uintptr_t EntryHash = 0;
    constexpr uintptr_t EntryKey = 8;
    constexpr uintptr_t EntryValue = 16;
}

struct Vec3 { float x, y, z; };

static std::atomic<int> g_pid{0};
static std::atomic<uintptr_t> g_bm{0};
static std::mutex g_mu;
static std::string g_log;

static bool rpm(pid_t pid, uintptr_t addr, void* buf, size_t n) {
    if (!addr || !buf || !n) return false;
    iovec local{buf, n}, remote{(void*)addr, n};
    return process_vm_readv(pid, &local, 1, &remote, 1, 0) == (ssize_t)n;
}
template<typename T>
static bool rpm_t(pid_t pid, uintptr_t addr, T& o) {
    return rpm(pid, addr, &o, sizeof(T));
}
static bool looks_ptr(uintptr_t p) {
    return p >= 0x10000ULL && p < 0x800000000000ULL && ((p & 7ULL) == 0);
}
static bool heap_ptr(uintptr_t p) {
    return looks_ptr(p) && p >= 0x7400000000ULL && p < 0x7C00000000ULL;
}
static bool world_pos(const Vec3& p) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
    if (std::fabs(p.x) < 0.5f && std::fabs(p.z) < 0.5f) return false;
    if (std::fabs(p.x) > 10000.f || std::fabs(p.z) > 10000.f || std::fabs(p.y) > 2000.f)
        return false;
    return true;
}

static bool read_dict(pid_t pid, uintptr_t dict, std::vector<uintptr_t>& out);

static bool is_battle_manager(pid_t pid, uintptr_t inst, Vec3* outPos = nullptr, int* outCount = nullptr) {
    if (!heap_ptr(inst)) return false;
    uintptr_t local = 0, dict = 0;
    if (!rpm_t(pid, inst + Off::LocalPlayerShow, local) || !heap_ptr(local)) return false;
    if (!rpm_t(pid, inst + Off::DicPlayerShow, dict) || !heap_ptr(dict)) return false;
    Vec3 pos{};
    if (!rpm_t(pid, local + Off::LastPosition, pos) || !world_pos(pos)) return false;
    int count = 0;
    if (!rpm_t(pid, dict + Off::DictCount, count) || count <= 0 || count > 16) return false;
    if (outPos) *outPos = pos;
    if (outCount) *outCount = count;
    return true;
}

static int count_usable(pid_t pid, uintptr_t inst) {
    uintptr_t local = 0, dict = 0;
    rpm_t(pid, inst + Off::LocalPlayerShow, local);
    rpm_t(pid, inst + Off::DicPlayerShow, dict);
    std::vector<uintptr_t> ents;
    read_dict(pid, dict, ents);
    int n = 0;
    for (uintptr_t v : ents) {
        if (!v || v == local) continue;
        uint8_t dead = 1;
        if (!rpm_t(pid, v + Off::Death, dead) || dead > 1) continue;
        if (dead) continue;
        Vec3 pos{};
        if (!rpm_t(pid, v + Off::LastPosition, pos) || !world_pos(pos)) continue;
        n++;
    }
    return n;
}

struct MapsReg { uintptr_t start, end; char perms[8]; std::string path; };

static bool read_maps(pid_t pid, std::vector<MapsReg>& out) {
    out.clear();
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    if (!maps) return false;
    std::string line;
    while (std::getline(maps, line)) {
        MapsReg r{};
        char path[512] = {};
        if (sscanf(line.c_str(), "%lx-%lx %7s %*s %*s %*s %511[^\n]",
                   &r.start, &r.end, r.perms, path) < 3)
            continue;
        r.path = path;
        out.push_back(r);
    }
    return !out.empty();
}

static bool read_dict_layout(pid_t pid, uintptr_t dict, uintptr_t entriesOff, uintptr_t countOff,
                             uintptr_t stride, uintptr_t valueOff, std::vector<uintptr_t>& out) {
    out.clear();
    if (!looks_ptr(dict)) return false;
    uintptr_t entries = 0;
    int count = 0;
    if (!rpm_t(pid, dict + entriesOff, entries) || !looks_ptr(entries)) return false;
    if (!rpm_t(pid, dict + countOff, count) || count <= 0 || count > 32) return false;
    int maxLen = 0;
    if (!rpm_t(pid, entries + Off::ArrLen, maxLen) || maxLen <= 0 || maxLen > 512) return false;
    const uintptr_t data = entries + Off::ArrData;
    for (int i = 0; i < maxLen; ++i) {
        const uintptr_t e = data + (uintptr_t)i * stride;
        int hash = 0;
        uintptr_t value = 0;
        if (!rpm_t(pid, e, hash) || hash < 0) continue;
        if (!rpm_t(pid, e + valueOff, value) || !looks_ptr(value)) continue;
        out.push_back(value);
        if ((int)out.size() >= count) break;
    }
    return !out.empty();
}

static bool read_dict(pid_t pid, uintptr_t dict, std::vector<uintptr_t>& out) {
    if (read_dict_layout(pid, dict, 0x18, 0x20, 24, 0x10, out)) return true;
    if (read_dict_layout(pid, dict, 0x10, 0x18, 24, 0x10, out)) return true;
    if (read_dict_layout(pid, dict, 0x18, 0x20, 16, 0x08, out)) return true;
    return false;
}

static bool read_list(pid_t pid, uintptr_t list, std::vector<uintptr_t>& out) {
    out.clear();
    if (!looks_ptr(list)) return false;
    uintptr_t items = 0;
    int size = 0;
    if (!rpm_t(pid, list + 0x10, items) || !looks_ptr(items)) return false;
    if (!rpm_t(pid, list + 0x18, size) || size <= 0 || size > 32) return false;
    for (int i = 0; i < size; ++i) {
        uintptr_t v = 0;
        if (rpm_t(pid, items + Off::ArrData + (uintptr_t)i * 8, v) && looks_ptr(v))
            out.push_back(v);
    }
    return !out.empty();
}

/* One-shot scan. Keep the hit with the most usable entities. */
static uintptr_t resolve_bm(pid_t pid) {
    uintptr_t cached = g_bm.load();
    if (cached && is_battle_manager(pid, cached) && count_usable(pid, cached) > 0)
        return cached;

    std::vector<MapsReg> maps;
    if (!read_maps(pid, maps)) {
        g_log = "RESOLVE FAIL\nmaps unreadable\n";
        return 0;
    }

    struct Hit { uintptr_t inst; int usable; int dictCount; Vec3 lp; };
    std::vector<Hit> hits;
    int rpmOk = 0, rpmFail = 0, cand = 0, mapsTried = 0;
    const size_t chunk = 256 * 1024;

    for (const auto& m : maps) {
        if (m.perms[0] != 'r' || m.perms[1] != 'w') continue;
        if (m.start < 0x7400000000ULL || m.start >= 0x7C00000000ULL) continue;
        if (m.end <= m.start) continue;
        const size_t sz = (size_t)(m.end - m.start);
        if (sz < 0x80) continue;
        if (++mapsTried > 40) break;

        const size_t scan = std::min(sz, (size_t)8 * 1024 * 1024);
        for (uintptr_t off = 0; off < scan; off += chunk) {
            const size_t n = std::min(chunk, scan - off);
            std::vector<uint8_t> buf(n);
            if (!rpm(pid, m.start + off, buf.data(), n)) {
                rpmFail++;
                break;
            }
            rpmOk++;
            for (size_t i = 0; i + 0x70 <= n; i += 16) {
                uintptr_t local = 0, dict = 0;
                memcpy(&local, buf.data() + i + Off::LocalPlayerShow, 8);
                memcpy(&dict, buf.data() + i + Off::DicPlayerShow, 8);
                if (!heap_ptr(local) || !heap_ptr(dict)) continue;
                cand++;
                const uintptr_t inst = m.start + off + i;
                Vec3 lp{};
                int dc = 0;
                if (!is_battle_manager(pid, inst, &lp, &dc)) continue;
                const int usable = count_usable(pid, inst);
                hits.push_back({inst, usable, dc, lp});
                if (hits.size() >= 24) break;
            }
            if (hits.size() >= 24) break;
        }
        if (hits.size() >= 24) break;
    }

    uintptr_t found = 0;
    int bestUse = -1;
    Hit best{};
    for (const auto& h : hits) {
        int score = h.usable * 10 + h.dictCount + (h.inst >= 0x7440000000ULL ? 3 : 0);
        if (h.usable > bestUse || (h.usable == bestUse && h.inst > best.inst)) {
            bestUse = h.usable;
            best = h;
            found = h.inst;
            (void)score;
        }
    }
    if (bestUse <= 0 && !hits.empty()) {
        best = hits.back();
        for (const auto& h : hits)
            if (h.inst > best.inst) best = h;
        found = best.inst;
    }

    std::ostringstream os;
    os << "LIBTOOL ESP RESOLVE (external APK)\n";
    os << "method=score heap objects by usable entities\n";
    os << "maps=" << maps.size() << " heap_tried=" << mapsTried
       << " rpm_ok=" << rpmOk << " rpm_fail=" << rpmFail
       << " candidates=" << cand << " hits=" << hits.size() << "\n";
    os << "BattleManager.Instance=0x" << std::hex << found << std::dec;
    if (found)
        os << " OK usable=" << best.usable << " dict=" << best.dictCount
           << " lp=" << best.lp.x << "," << best.lp.z << "\n";
    else
        os << " FAIL\n";
    for (size_t i = 0; i < hits.size() && i < 8; ++i)
        os << "  hit[" << i << "]=0x" << std::hex << hits[i].inst << std::dec
           << " usable=" << hits[i].usable << " dict=" << hits[i].dictCount << "\n";
    os << "FIELDS m_LocalPlayerShow=0x48 m_dicPlayerShow=0x60 LastPosition=0x3F4\n";
    if (!found)
        os << "WHY FAIL: no BattleManager-like object. Open a match, tap Resolve.\n";
    else if (best.usable <= 0)
        os << "WHY WEAK: Instance has local pos but 0 other living entities. Dict walk or camp/death filter.\n";
    else
        os << "WHY OK: picked the Instance with the most living entities.\n";
    g_log = os.str();
    g_bm.store(found);
    return found;
}

static std::string snapshot(pid_t pid) {
    uintptr_t cached = g_bm.load();
    uintptr_t bm = (cached && is_battle_manager(pid, cached)) ? cached : resolve_bm(pid);
    std::ostringstream os;
    os << "OK|src=libtool|pid=" << pid << "|bm=0x" << std::hex << bm << std::dec;

    if (!bm) {
        os << "|lp=0,0|count=0|proj=0|stage=no_BattleManager";
        return os.str();
    }

    uintptr_t local = 0;
    Vec3 self{};
    rpm_t(pid, bm + Off::LocalPlayerShow, local);
    if (local) rpm_t(pid, local + Off::LastPosition, self);
    os << "|lp=" << self.x << "," << self.z;

    std::vector<uintptr_t> ents;
    uintptr_t dict = 0;
    rpm_t(pid, bm + Off::DicPlayerShow, dict);
    read_dict(pid, dict, ents);
    if (ents.empty()) {
        uintptr_t list = 0;
        rpm_t(pid, bm + 0x70, list);
        read_list(pid, list, ents);
    }
    uintptr_t md = 0;
    std::vector<uintptr_t> monsters;
    rpm_t(pid, bm + Off::DicMonsterShow, md);
    read_dict(pid, md, monsters);

    struct Row { int id, hp, hpMax, kind; float x, y, z; };
    std::vector<Row> rows;
    int skipLocal = 0, skipDead = 0, skipCamp = 0, skipPos = 0, walked = 0;
    auto take = [&](uintptr_t v, int kind, bool applyCamp) {
        if (!v) return;
        walked++;
        if (v == local) { skipLocal++; return; }
        uint8_t dead = 1, camp = 0;
        if (!rpm_t(pid, v + Off::Death, dead) || dead > 1) { skipPos++; return; }
        if (dead) { skipDead++; return; }
        if (applyCamp) {
            if (rpm_t(pid, v + Off::SameCamp, camp) && camp == 1) { skipCamp++; return; }
        }
        Vec3 pos{};
        if (!rpm_t(pid, v + Off::LastPosition, pos) || !world_pos(pos)) { skipPos++; return; }
        int id = 0, hp = 0, hpMax = 0;
        rpm_t(pid, v + Off::EntityId, id);
        rpm_t(pid, v + Off::Hp, hp);
        rpm_t(pid, v + Off::HpMax, hpMax);
        rows.push_back({id, hp, hpMax, kind, pos.x, pos.y, pos.z});
    };
    for (uintptr_t v : ents) take(v, 0, true);
    if (rows.empty())
        for (uintptr_t v : ents) take(v, 0, false);
    for (uintptr_t v : monsters) take(v, 1, true);

    os << "|dict=0x" << std::hex << dict << std::dec
       << "|walked=" << walked
       << "|skipL=" << skipLocal << "|skipD=" << skipDead
       << "|skipC=" << skipCamp << "|skipP=" << skipPos;
    os << "|count=" << rows.size() << "|proj=0|";
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i) os << ";";
        const float dx = rows[i].x - self.x;
        const float dz = rows[i].z - self.z;
        const int meters = (int)std::sqrt(dx * dx + dz * dz);
        os << rows[i].x << "," << rows[i].z << ",ID " << rows[i].id << ","
           << meters << "," << rows[i].hp << "," << rows[i].hpMax << ","
           << (rows[i].kind ? "M" : "P");
    }
    if (rows.empty())
        os << "stage=empty";
    return os.str();
}

static std::string target_error(int pid) {
    if (pid <= 0) return "ERR|pid_not_set";
    if (kill(pid, 0) == -1 && errno == ESRCH) return "ERR|process_not_found";
    return "";
}

extern "C" JNIEXPORT void JNICALL
Java_ownapp_esp_MainActivity_nativeSetPid(JNIEnv*, jobject, jint pid) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_pid.store(pid > 0 ? pid : 0);
    g_bm.store(0);
    g_log.clear();
}

extern "C" JNIEXPORT jstring JNICALL
Java_ownapp_esp_MainActivity_nativeResolve(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> lock(g_mu);
    int pid = g_pid.load();
    const std::string err = target_error(pid);
    if (!err.empty()) return env->NewStringUTF(err.c_str());
    resolve_bm(pid);
    return env->NewStringUTF(g_log.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_ownapp_esp_MainActivity_nativeSnapshot(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> lock(g_mu);
    int pid = g_pid.load();
    const std::string err = target_error(pid);
    if (!err.empty()) return env->NewStringUTF(err.c_str());
    std::string s = snapshot(pid);
    return env->NewStringUTF(s.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_ownapp_esp_MainActivity_nativeLog(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_log.empty())
        return env->NewStringUTF("Tap Apply PID, then Resolve.");
    return env->NewStringUTF(g_log.c_str());
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    return JNI_VERSION_1_6;
}
