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

static bool is_battle_manager(pid_t pid, uintptr_t inst) {
    if (!heap_ptr(inst)) return false;
    uintptr_t local = 0, dict = 0;
    if (!rpm_t(pid, inst + Off::LocalPlayerShow, local) || !heap_ptr(local)) return false;
    if (!rpm_t(pid, inst + Off::DicPlayerShow, dict) || !heap_ptr(dict)) return false;
    Vec3 pos{};
    if (!rpm_t(pid, local + Off::LastPosition, pos) || !world_pos(pos)) return false;
    int count = 0;
    if (!rpm_t(pid, dict + Off::DictCount, count) || count <= 0 || count > 16) return false;
    return true;
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

/* One-shot scan. Cached after first hit. No class-name search. */
static uintptr_t resolve_bm(pid_t pid) {
    uintptr_t cached = g_bm.load();
    if (cached && is_battle_manager(pid, cached)) return cached;

    std::vector<MapsReg> maps;
    if (!read_maps(pid, maps)) {
        g_log = "RESOLVE FAIL\nmaps unreadable\n";
        return 0;
    }

    int rpmOk = 0, rpmFail = 0, cand = 0;
    uintptr_t found = 0;
    const size_t chunk = 256 * 1024;
    int mapsTried = 0;

    for (const auto& m : maps) {
        if (m.perms[0] != 'r' || m.perms[1] != 'w') continue;
        if (m.start < 0x7400000000ULL || m.start >= 0x7C00000000ULL) continue;
        if (m.end <= m.start) continue;
        const size_t sz = (size_t)(m.end - m.start);
        if (sz < 0x80 || sz > 16 * 1024 * 1024) continue;
        if (++mapsTried > 14) break;

        for (uintptr_t off = 0; off < sz && off < 2 * 1024 * 1024; off += chunk) {
            const size_t n = std::min(chunk, (size_t)(sz - off));
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
                if (is_battle_manager(pid, inst)) {
                    found = inst;
                    break;
                }
            }
            if (found) break;
        }
        if (found) break;
    }

    std::ostringstream os;
    os << "LIBTOOL ESP RESOLVE (external APK)\n";
    os << "method=one-shot heap object scan (no class-name walk)\n";
    os << "maps=" << maps.size() << " heap_tried=" << mapsTried
       << " rpm_ok=" << rpmOk << " rpm_fail=" << rpmFail
       << " candidates=" << cand << "\n";
    os << "BattleManager.Instance=0x" << std::hex << found << std::dec
       << (found ? " OK\n" : " FAIL\n");
    os << "FIELDS (from working LibTool esp_resolve.txt)\n";
    os << "  m_LocalPlayerShow=0x48\n";
    os << "  m_dicPlayerShow=0x60\n";
    os << "  m_dicMonsterShow=0x68\n";
    os << "  LastPosition=0x3F4  m_ID=0x18C  m_bDeath=0xC5\n";
    os << "  m_bSameCampType=0x2A9  m_Hp=0x1A4  m_HpMax=0x1A8\n";
    if (rpmFail && !rpmOk)
        os << "WHY FAIL: process_vm_readv blocked. Overlay UID cannot read the game.\n";
    else if (!found)
        os << "WHY FAIL: no object had local pos + player dict. Enter a match, then Resolve.\n";
    else
        os << "WHY OK: same ShowEntity/BattleManager layout as LibTool ESP.\n";
    g_log = os.str();
    g_bm.store(found);
    return found;
}

static bool read_dict(pid_t pid, uintptr_t dict, std::vector<uintptr_t>& out) {
    out.clear();
    if (!looks_ptr(dict)) return false;
    uintptr_t entries = 0;
    int count = 0;
    if (!rpm_t(pid, dict + Off::DictEntries, entries) || !looks_ptr(entries)) return false;
    if (!rpm_t(pid, dict + Off::DictCount, count) || count <= 0 || count > 256) return false;
    int maxLen = 0;
    if (!rpm_t(pid, entries + Off::ArrLen, maxLen) || maxLen <= 0 || maxLen > 512) return false;
    const uintptr_t data = entries + Off::ArrData;
    for (int i = 0; i < maxLen; ++i) {
        const uintptr_t e = data + (uintptr_t)i * Off::EntryStride;
        int hash = 0;
        uintptr_t value = 0;
        if (!rpm_t(pid, e + Off::EntryHash, hash) || hash < 0) continue;
        if (!rpm_t(pid, e + Off::EntryValue, value) || !looks_ptr(value)) continue;
        out.push_back(value);
        if ((int)out.size() >= count) break;
    }
    return !out.empty();
}

static std::string snapshot(pid_t pid) {
    uintptr_t bm = resolve_bm(pid);
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
    uintptr_t md = 0;
    std::vector<uintptr_t> monsters;
    rpm_t(pid, bm + Off::DicMonsterShow, md);
    read_dict(pid, md, monsters);

    struct Row { int id, hp, hpMax, kind; float x, y, z; };
    std::vector<Row> rows;
    auto take = [&](uintptr_t v, int kind) {
        if (!v || v == local) return;
        uint8_t dead = 1, camp = 1;
        if (!rpm_t(pid, v + Off::Death, dead) || dead) return;
        if (!rpm_t(pid, v + Off::SameCamp, camp) || camp) return;
        Vec3 pos{};
        if (!rpm_t(pid, v + Off::LastPosition, pos) || !world_pos(pos)) return;
        int id = 0, hp = 0, hpMax = 0;
        rpm_t(pid, v + Off::EntityId, id);
        rpm_t(pid, v + Off::Hp, hp);
        rpm_t(pid, v + Off::HpMax, hpMax);
        rows.push_back({id, hp, hpMax, kind, pos.x, pos.y, pos.z});
    };
    for (uintptr_t v : ents) take(v, 0);
    for (uintptr_t v : monsters) take(v, 1);

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
