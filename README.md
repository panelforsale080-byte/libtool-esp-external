# MLBB External ESP (conRentV1 / LibTool port)

Separate **application APK** (`ownapp.esp`) — **does not** inject `libTool.so` into `com.mobile.legends`.

## How external ESP works (no inject into game APK)

| Injected LibTool (conRentV1 in-process) | This external APK |
|----------------------------------------|-------------------|
| Code runs **inside** Unity process | Code runs in **your** APK |
| `Il2cpp::FindClass()` in same address space | **`process_vm_readv(pid, …)`** reads game memory |
| ImGui hooks game `eglSwapBuffers` | **System overlay** draws boxes/lines on top |
| Offsets from live IL2CPP fields | Same **field offsets** from `dump.cs` / `esp_resolve.txt`, applied to **remote pointers** |

### Steps

1. **Find PID** — `com.mobile.legends:UnityKillsMe` (same as ASTRAL loader).
2. **Connect** — native sets target PID; probe checks cross-process read (usually **root** or paired virtual app).
3. **Resolve** (in match) — scan game maps for `BattleManager` class name → `Il2CppClass` → static `Instance` pointer (port of LibTool `EspTab::resolveOffsets`).
4. **Snapshot loop** — walk `m_dicPlayerShow` with fixed offsets (`0x48`, `0x60`, `0x3f4`, …) via `process_vm_readv`.
5. **Overlay** — draw ESP from snapshot data (no code inside MLBB).

Offsets do **not** require inject: they are **byte offsets into C# objects** in the **game process heap**. You only need a valid **base pointer** (e.g. `BattleManager.Instance`) read remotely.

When Moonton updates MLBB, refresh offsets from a new `dump.cs` into `LibToolEsp.h` / resolve log.

## Build

```bash
./gradlew assembleDebug
```

CI: **Build APK** workflow → artifact `mlbb-external-esp-debug`.

## Requirements

- MLBB running (UnityKillsMe visible in `/proc`)
- **Root** (typical) or environment where `process_vm_readv` can read the game PID
- **Overlay permission** for floating ESP
- Resolve while **in a match** (Instance NULL in lobby is normal)

## Related

- Logic source: `conRentV1` / `libtoolv2` `EspTab.cpp`, `EspRemote.cpp`
- Inject loader (different product): `powerhax-loader` + `libTool.so`
