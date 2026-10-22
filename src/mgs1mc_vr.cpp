// mgs1mc_vr ctrl v1 -- head tracking + world-locked picture for the MGS1 VR port to the Master Collection
// (US 1.0 and PAL).
//
// Installed as dinput8.dll next to "METAL GEAR SOLID.exe", with openxr_loader.dll (Khronos, x64) beside it.
// It forwards DirectInput8Create to the system dinput8.dll and runs three things on its own threads:
//
//  1. In-game hook (MIPS, in emulated PS1 RAM). DG_MakeCameraMatrix's jal to TransposeMatrix
//     (US 1.0 0x80017AE0 / PAL 0x800174F0) is retargeted to our routine at 0x8000A460. For the world
//     channel only, it post-multiplies a 3x3 head rotation (x4096) into chanl->eye, then tail-jumps to
//     TransposeMatrix so the view matrix follows. Mailbox at 0x8000A400 (see asm_cave2.py).
//  2. OpenXR session (any runtime: Virtual Desktop, SteamVR, Meta, ...). Each headset frame it reads the
//     head orientation, makes it relative to a recentred forward, converts it to the PS1 camera's axes and
//     writes it to the mailbox. Head tracking applies in first person (hold the first-person button);
//     it recentres each time first person starts.
//  3. Picture: the game's own D3D11 Present is hooked; each presented frame is copied to a shared
//     texture, cropped to the PS1 picture (auto-detected), and shown in the headset:
//       - while head tracking is active: WORLD-LOCKED. Each picture is placed where the game camera was
//         looking when it drew it (the head pose that frame used, found through a pose-sequence ring the
//         MIPS routine fills), at the angular size of the game's field of view. Between game frames the
//         world holds still. (GOG FrameViewRecord design, done with a quad layer.)
//       - otherwise (third person, menus, cutscenes): a fixed virtual screen in the room, placed in front
//         of you at the last recentre.
//  4. FOV: optional override of the game's projection distance (chanl->screen, 320 = 53 deg).
//
// Keys: F8 head tracking on/off, F6 recentre, F9 RAM snapshot, F10 save the captured picture (BMP).
// Ini: mgs1mc_vr.ini [phase2] (enabled, vr, track_third_person, recentre_on_first_person,
//      screen_distance_m, screen_width_m) and [phase3] (tag_delay, fov_screen, crop, tracked_distance_m),
//      re-read every 2 s.
// Log: mgs1mc_vr.log next to the exe. Snapshots: mgs1mc_vr_probe\ram_p2_<seq>_<why>.bin.
//
// History: phase1 v1-v3 (2026-10-07/08) proved the hook on PAL and US 1.0. phase2 v1 (2026-10-08): head tracking
// confirmed by the owner (Quest 2 / Virtual Desktop). phase3 v1 (2026-10-08): crop, world lock, FOV.
// phase3 v1 test: world lock held on slow turns, BUT the headset loop fell to 11 Hz (1/8 of 90) a few seconds
// after the game started, and the right eye lagged (VD reprojection). Auto-crop failed: MC draws wallpaper art
// in the pillarbox (PS1 picture = x 240-1680, y 36-1044 of 1920x1080).
// phase3 v2: all game-RAM writes moved to a writer thread; per-step timing of the headset loop and of the
// picture copy; 4:3 crop rule by default; cutscene head-look (second hook in FrameRunDemo); read-only watch of
// spare RAM blocks for future code.
// phase3 v2 test: 90 Hz restored; cutscene head-look and first-person look confirmed by the owner ("very smooth").
// walk v1 (2026-10-08): Snake's-eyes walking camera (routine mode 2). The 1998 builds cannot walk in first person
// (GM_FirstPerson is Integral-only), so in third-person gameplay the routine replaces the whole view: rotation =
// Snake's facing at entry (+ right-stick turning) composed with the head, position = GM_SnakeCamera.position (the
// game's own first-person eye point, updated every frame by Snake) + a forward offset, and PadOrg = view direction
// so the stick walks Snake where you look. F7 toggles walking mode.
// walk v1 test: walking worked (body visible looking down) but Snake's head clipped into view when moving (eye was
// anchored to the body centre; the head joint sits 60-90+ units ahead), Quest controllers did nothing (not built
// yet), and the headset loop collapsed again to 11 Hz: TIMING showed xrEndFrame blocking ~89 ms every frame from
// boot (before any game hook). Cause: our GPU reads of the game's MISC_SHARED texture with no synchronisation.
// walk v2: keyed-mutex hand-off (game side copies only when it holds key 0, headset side copies into a private
// texture only when it can take key 1, both with zero timeout, so neither GPU ever waits on the other); eye x,z
// from Snake's head joint (routine v5 reads body DG_OBJS objs[6].world.t; pointer found by walking the actor lists).
// walk v2 test (2026-10-11): still 11 Hz, and the collapse began ~7 s into the headset session, ~30 s BEFORE any game
// hook, with the texture hand-off fully decoupled: so the cost is in the compositor. Every collapsing build submitted
// a world-locked QUAD layer; the GOG mod's notes say an extra quad layer collapsed its frame rate on this setup, and its
// world lock submits a PROJECTION layer instead. Owner also felt "not in game, a virtual game" (53 deg window).
// walk v3: no quad layers at all. One projection layer (both eyes, same mono picture) whose view pose is the head pose
// the picture was drawn with and whose FOV is exactly the game's (h: atan(160/screen), v: atan(123.6/screen), the
// 58/64 overscan squeeze undone); the compositor reprojects = world lock. The fixed screen (menus, third person) is the
// same trick with the recentre pose and a fixed FOV. Default game FOV 106 deg (fov_screen=120, the GOG default).
// walk v3 test (2026-10-11): gameplay "fine" (mostly 45 Hz, spells of 90); main menu very laggy (10 Hz). Owner is wireless.
// ctrl v1: VR controllers. OpenXR actions (Touch + Index bindings) -> PS1 buttons and the left stick, merged into
// GV_PadData by a second MIPS routine that wraps the frame loop's jal GV_UpdatePadSystem (asm_pad.py; US 0x8001FD4C,
// PAL 0x8001F3FC). Right stick turns in walk mode, right stick click recentres. Mapping in [controls].
// Plus a PERF log line every 3 s from Windows performance counters: GPU 3D and video-encode engine load (total and
// this process) and CPU, to tell a GPU-bound stall from a wireless/network one.

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <pdh.h>
#include <pdhmsg.h>
#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <atomic>
#include <string>
#include <vector>

static const size_t PSX_RAM = 0x200000;

static HMODULE g_real = nullptr;
static FILE*   g_log  = nullptr;
static std::wstring g_dir;
static CRITICAL_SECTION g_logcs;

static void logf(const char* fmt, ...)
{
    if (!g_log) return;
    EnterCriticalSection(&g_logcs);
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logcs);
}

// ---------------------------------------------------------------- forwarding
typedef HRESULT (WINAPI *DI8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static void di_wrap(void* di);

static void load_real()
{
    if (g_real) return;
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring p = std::wstring(sys) + L"\\dinput8.dll";
    g_real = LoadLibraryW(p.c_str());
}

// ---------------------------------------------------------------- Quest controllers in the Master Collection menus
// The launcher and M2's own menus are not the PS1 game, so the pad routine cannot reach them. M2 reads its keyboard
// through DirectInput, and this DLL is dinput8.dll: the keyboard device's reads are wrapped and the Quest buttons are
// added as key presses -- only while the PS1 game itself is not running (otherwise the pad routine handles them).
// Real key presses are logged (KEYBOARD lines) so the menus' own bindings can be read off the log.
static std::atomic<uint8_t> g_inj[256];                 // keys the Quest controllers hold down right now (0x80 = down)
static std::atomic<bool>    g_inj_on{false};            // main thread: the PS1 game is not running, inject
static std::atomic<ULONGLONG> g_di_kbd_read_ms{0};      // last time the game read the DirectInput keyboard
static void* g_kbd_dev[8];                              // keyboard device objects the game created
static CRITICAL_SECTION g_dics;
static uint8_t g_di_sent[256];                          // what buffered reads have reported for injected keys
static uint8_t g_real_prev[256];
typedef HRESULT (STDMETHODCALLTYPE *DICreateDevice_t)(void*, REFGUID, void**, void*);
typedef HRESULT (STDMETHODCALLTYPE *DIGetState_t)(void*, DWORD, LPVOID);
typedef HRESULT (STDMETHODCALLTYPE *DIGetData_t)(void*, DWORD, void*, LPDWORD, DWORD);
static const GUID kGUID_SysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID kGUID_SysMouse    = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

static bool is_kbd(void* dev) { for (void* k : g_kbd_dev) if (k == dev) return true; return false; }
// originals per vtable (the ANSI and wide interfaces have separate vtables)
struct VtOrig { void** vt; int idx; void* orig; };
static VtOrig g_vto[16]; static int g_nvto = 0;
static void* orig_of(void* self, int idx)
{
    void** vt = *(void***)self;
    for (int i = 0; i < g_nvto; ++i) if (g_vto[i].vt == vt && g_vto[i].idx == idx) return g_vto[i].orig;
    return nullptr;
}
static void patch_vt(void** vt, int idx, void* fn)
{
    if (vt[idx] == fn || g_nvto >= 16) return;
    DWORD old;
    if (!VirtualProtect(&vt[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return;
    g_vto[g_nvto] = {vt, idx, vt[idx]}; ++g_nvto;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    vt[idx] = fn;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
}
// Key presses are logged only in the Master Collection menus (not during play), only while the game window has focus,
// and at most 300 lines a session: enough to read the menus' bindings off the log.
static std::atomic<bool> g_menus_active{false};
static HWND g_game_hwnd = nullptr;
static void log_real_key(int k, bool down)
{
    static int lines = 0;
    if (!g_menus_active.load() || lines >= 300) return;
    HWND fg = GetForegroundWindow();
    if (!g_game_hwnd || (fg != g_game_hwnd && GetAncestor(g_game_hwnd, GA_ROOT) != fg)) return;
    ++lines;
    logf("KEYBOARD: key 0x%02X %s (in the menus)", k, down ? "down" : "up");
}

static HRESULT STDMETHODCALLTYPE hk_GetDeviceState(void* self, DWORD cb, LPVOID data)
{
    auto o = (DIGetState_t)orig_of(self, 9); if (!o) return E_FAIL;
    HRESULT hr = o(self, cb, data);
    if (SUCCEEDED(hr) && data && cb == 256 && is_kbd(self)) {
        g_di_kbd_read_ms = GetTickCount64();
        uint8_t* st = (uint8_t*)data;
        EnterCriticalSection(&g_dics);
        for (int k = 0; k < 256; ++k) {
            uint8_t real = st[k] & 0x80;
            if (real != g_real_prev[k]) { g_real_prev[k] = real; log_real_key(k, real != 0); }
        }
        LeaveCriticalSection(&g_dics);
        if (g_inj_on) for (int k = 0; k < 256; ++k) st[k] |= g_inj[k].load();
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_GetDeviceData(void* self, DWORD cbObj, void* rg, LPDWORD inout, DWORD flags)
{
    DWORD cap = inout ? *inout : 0;
    auto o = (DIGetData_t)orig_of(self, 10); if (!o) return E_FAIL;
    HRESULT hr = o(self, cbObj, rg, inout, flags);
    if (FAILED(hr) || !inout || !is_kbd(self)) return hr;
    g_di_kbd_read_ms = GetTickCount64();
    if (!rg || cbObj < 16) return hr;
    EnterCriticalSection(&g_dics);
    uint8_t* base = (uint8_t*)rg;
    for (DWORD i = 0; i < *inout; ++i) {                                     // real events: log them
        DWORD ofs = *(DWORD*)(base + i * cbObj), dat = *(DWORD*)(base + i * cbObj + 4);
        if (ofs < 256 && !(flags & 1)) log_real_key((int)ofs, (dat & 0x80) != 0);
    }
    DWORD n = *inout;
    bool on = g_inj_on.load();
    for (int k = 0; k < 256 && n < cap; ++k) {                               // injected key changes, as events
        uint8_t want = on ? g_inj[k].load() : 0;
        if (want == g_di_sent[k]) continue;
        uint8_t* e = base + n * cbObj; memset(e, 0, cbObj);
        *(DWORD*)(e + 0) = (DWORD)k; *(DWORD*)(e + 4) = want; *(DWORD*)(e + 8) = GetTickCount();
        if (!(flags & 1)) g_di_sent[k] = want;                              // DIGDD_PEEK leaves the queue as is
        ++n;
    }
    *inout = n;
    LeaveCriticalSection(&g_dics);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreateDevice(void* self, REFGUID g, void** dev, void* outer)
{
    auto o = (DICreateDevice_t)orig_of(self, 3); if (!o) return E_FAIL;
    HRESULT hr = o(self, g, dev, outer);
    bool kb = IsEqualGUID(g, kGUID_SysKeyboard), ms = IsEqualGUID(g, kGUID_SysMouse);
    logf("DINPUT: game created a %s device (%s)", kb ? "keyboard" : ms ? "mouse" : "game controller", SUCCEEDED(hr) ? "ok" : "failed");
    if (SUCCEEDED(hr) && dev && *dev && kb) {
        EnterCriticalSection(&g_dics);
        for (auto& k : g_kbd_dev) if (!k) { k = *dev; break; }
        void** vt = *(void***)*dev;
        patch_vt(vt, 9, (void*)hk_GetDeviceState);
        patch_vt(vt, 10, (void*)hk_GetDeviceData);
        LeaveCriticalSection(&g_dics);
    }
    return hr;
}

static void di_wrap(void* di)
{
    static bool init = false;
    if (!init) { InitializeCriticalSection(&g_dics); init = true; }
    void** vt = *(void***)di;
    EnterCriticalSection(&g_dics); patch_vt(vt, 3, (void*)hk_CreateDevice); LeaveCriticalSection(&g_dics);
    logf("DINPUT: DirectInput8Create wrapped (keyboard reads will carry Quest menu buttons)");
}

// SendInput fallback, for menus that read window messages instead of DirectInput
static uint8_t g_si_sent[256];
static void sendinput_sync(bool allowed)
{
    HWND fg = GetForegroundWindow();
    bool ours = allowed && g_game_hwnd && (fg == g_game_hwnd || GetAncestor(g_game_hwnd, GA_ROOT) == fg);
    for (int k = 1; k < 256; ++k) {
        uint8_t want = ours ? g_inj[k].load() : 0;
        if (want == g_si_sent[k]) continue;
        INPUT in = {}; in.type = INPUT_KEYBOARD;
        in.ki.wScan = (WORD)(k & 0x7F);
        in.ki.dwFlags = KEYEVENTF_SCANCODE | (k & 0x80 ? KEYEVENTF_EXTENDEDKEY : 0) | (want ? 0 : KEYEVENTF_KEYUP);
        SendInput(1, &in, sizeof in);
        g_si_sent[k] = want;
    }
}

extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(
    HINSTANCE hinst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    load_real();
    if (!g_real) return E_FAIL;
    auto fn = (DI8Create_t)GetProcAddress(g_real, "DirectInput8Create");
    if (!fn) return E_FAIL;
    HRESULT hr = fn(hinst, ver, riid, out, outer);
    if (SUCCEEDED(hr) && out && *out) di_wrap(*out);
    return hr;
}

// ---------------------------------------------------------------- safe memory access
static bool safe_read(const void* src, void* dst, size_t n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), src, dst, n, &got) && got == n;
}
static bool safe_write(void* dst, const void* src, size_t n)
{
    SIZE_T put = 0;
    return WriteProcessMemory(GetCurrentProcess(), dst, src, n, &put) && put == n;
}
static inline bool rd(uint8_t* ram, uint32_t psx, void* out, size_t n) { return safe_read(ram + (psx & 0x1FFFFF), out, n); }
static inline bool wr(uint8_t* ram, uint32_t psx, const void* in, size_t n) { return safe_write(ram + (psx & 0x1FFFFF), in, n); }
static inline uint32_t rd32(uint8_t* ram, uint32_t psx) { uint32_t v = 0; rd(ram, psx, &v, 4); return v; }
static inline bool wr32(uint8_t* ram, uint32_t psx, uint32_t v) { return wr(ram, psx, &v, 4); }

// ---------------------------------------------------------------- MIPS routines (asm_cave6.py / asm_pad.py output)
static const uint32_t CAVE11_PAL[186] = {
    0x3C088001, 0x2508A400, 0x8D09000C, 0x00000000, 0x25290001, 0xAD09000C,
    0xAD040014, 0x8D090004, 0x3C0A800B, 0x254A2A74, 0x112000AD, 0x00000000,
    0x148A00AB, 0x00000000, 0x8D0A0008, 0x00000000, 0x250B0020, 0x11400002,
    0x00000000, 0x250B0038, 0x240A0002, 0x112A004E, 0x00000000, 0x00806021,
    0x240D0003, 0x858E0000, 0x858F0002, 0x85980004, 0x00000000, 0x85620000,
    0x85630006, 0x8566000C, 0x01C20018, 0x0000C812, 0x00000000, 0x00000000,
    0x01E30018, 0x00003812, 0x0327C821, 0x00000000, 0x03060018, 0x00003812,
    0x0327C821, 0x0019CB03, 0xA5990000, 0x85620002, 0x85630008, 0x8566000E,
    0x01C20018, 0x0000C812, 0x00000000, 0x00000000, 0x01E30018, 0x00003812,
    0x0327C821, 0x00000000, 0x03060018, 0x00003812, 0x0327C821, 0x0019CB03,
    0xA5990002, 0x85620004, 0x8563000A, 0x85660010, 0x01C20018, 0x0000C812,
    0x00000000, 0x00000000, 0x01E30018, 0x00003812, 0x0327C821, 0x00000000,
    0x03060018, 0x00003812, 0x0327C821, 0x0019CB03, 0xA5990004, 0x258C0006,
    0x25ADFFFF, 0x15A0FFC9, 0x00000000, 0x3C0C8001, 0x258CB580, 0x858D0000,
    0x8C8E0014, 0x00000000, 0x01CD7021, 0xAC8E0014, 0x858D0002, 0x8C8E0018,
    0x00000000, 0x01CD7021, 0xAC8E0018, 0x858D0004, 0x8C8E001C, 0x00000000,
    0x01CD7021, 0xAC8E001C, 0x080029D1, 0x00000000, 0x856D0000, 0x00000000,
    0xA48D0000, 0x856D0002, 0x00000000, 0xA48D0002, 0x856D0004, 0x00000000,
    0xA48D0004, 0x856D0006, 0x00000000, 0xA48D0006, 0x856D0008, 0x00000000,
    0xA48D0008, 0x856D000A, 0x00000000, 0xA48D000A, 0x856D000C, 0x00000000,
    0xA48D000C, 0x856D000E, 0x00000000, 0xA48D000E, 0x856D0010, 0x00000000,
    0xA48D0010, 0x3C0C800C, 0x258C8800, 0x8D0F007C, 0x00000000, 0x05E10016,
    0x00000000, 0x3C0E8020, 0x01EE702B, 0x11C00012, 0x00000000, 0x8DED0284,
    0x850E0070, 0x00000000, 0x01AE6821, 0xAC8D0014, 0x858D0002, 0x850E0072,
    0x00000000, 0x01AE6821, 0xAC8D0018, 0x8DED028C, 0x850E0074, 0x00000000,
    0x01AE6821, 0xAC8D001C, 0x080029C9, 0x00000000, 0x858D0000, 0x850E0070,
    0x00000000, 0x01AE6821, 0xAC8D0014, 0x858D0002, 0x850E0072, 0x00000000,
    0x01AE6821, 0xAC8D0018, 0x858D0004, 0x850E0074, 0x00000000, 0x01AE6821,
    0xAC8D001C, 0x8D0D0078, 0x00000000, 0x11A00005, 0x00000000, 0x950D0076,
    0x3C0C800B, 0x00000000, 0xAD8DC3E4, 0x950D0018, 0x00000000, 0x11A00002,
    0x00000000, 0xA48D0020, 0x08002A74, 0x00000000, 0x08007B4E, 0x00000000,
};
static const uint32_t SCAVE11_PAL[147] = {
    0x3C098001, 0x2529A780, 0xAD3F0018, 0x3C0A0008, 0x254ABF48, 0x001F68C0,
    0x000D68C2, 0x15AA0006, 0x00000000, 0xAD330014, 0x3C0A800B, 0x8D4AC39C,
    0x00000000, 0xAD2A001C, 0x3C0C8001, 0x258CA940, 0x3C0E800B, 0x8DCEC978,
    0x858D0000, 0x31CE0001, 0x3C0A8001, 0x8D4AB5C0, 0x00000000, 0x2D4A0001,
    0x014E5024, 0x11A00026, 0x00007821, 0x15400002, 0x00000000, 0x000D6823,
    0x84980000, 0x00000000, 0x030D0018, 0x0000C012, 0x00000000, 0x00000000,
    0x0018C303, 0x8C990014, 0x00000000, 0x0338C821, 0xAC990014, 0x84980006,
    0x00000000, 0x030D0018, 0x0000C012, 0x00000000, 0x00000000, 0x0018C303,
    0x8C990018, 0x00000000, 0x0338C821, 0xAC990018, 0x8498000C, 0x00000000,
    0x030D0018, 0x0000C012, 0x00000000, 0x00000000, 0x0018C303, 0x8C99001C,
    0x00000000, 0x0338C821, 0xAC99001C, 0x254F0001, 0x000FC080, 0x030CC021,
    0x8F190008, 0x00000000, 0x27390001, 0xAF190008, 0x8F190014, 0x00000000,
    0x000F7F80, 0x8D090010, 0x8D6D0014, 0x312A0007, 0x000A5080, 0x01485021,
    0x01AF6825, 0xAD4D0050, 0xAD0D001C, 0x25290001, 0xAD090010, 0x25870088,
    0x31A60003, 0x00C73021, 0x90C30000, 0x000D3082, 0x30C60003, 0x00C73021,
    0x90C60000, 0x00000000, 0x00063200, 0x00661825, 0x000D3102, 0x30C60003,
    0x00C73021, 0x90C60000, 0x00000000, 0x00063400, 0x00661825, 0x3C066000,
    0x00661825, 0x3C09800B, 0x8D29C39C, 0x000EC080, 0x030CC021, 0x8F0A0080,
    0x00000000, 0x112A0023, 0x00000000, 0xAF090080, 0x000EC140, 0x000E4900,
    0x0309C021, 0x030CC021, 0x27180020, 0xAF190004, 0xAF030024, 0x3C19800B,
    0x27392C30, 0x000E4880, 0x0329C821, 0x8F390000, 0x00000000, 0x8F290000,
    0x3C0D00FF, 0x35ADFFFF, 0x012D5024, 0x3C020300, 0x01425025, 0xAF0A0020,
    0x270A0020, 0x014D5024, 0x01425025, 0xAF0A0010, 0x270A0010, 0x014D5024,
    0x01425025, 0xAF0A0000, 0x3C02FF00, 0x01224824, 0x030D5024, 0x012A4825,
    0xAF290000, 0x08007B4E, 0x00000000,
};
static const uint32_t CAVE11_US10[186] = {
    0x3C088001, 0x2508A400, 0x8D09000C, 0x00000000, 0x25290001, 0xAD09000C,
    0xAD040014, 0x8D090004, 0x3C0A800B, 0x254A4194, 0x112000AD, 0x00000000,
    0x148A00AB, 0x00000000, 0x8D0A0008, 0x00000000, 0x250B0020, 0x11400002,
    0x00000000, 0x250B0038, 0x240A0002, 0x112A004E, 0x00000000, 0x00806021,
    0x240D0003, 0x858E0000, 0x858F0002, 0x85980004, 0x00000000, 0x85620000,
    0x85630006, 0x8566000C, 0x01C20018, 0x0000C812, 0x00000000, 0x00000000,
    0x01E30018, 0x00003812, 0x0327C821, 0x00000000, 0x03060018, 0x00003812,
    0x0327C821, 0x0019CB03, 0xA5990000, 0x85620002, 0x85630008, 0x8566000E,
    0x01C20018, 0x0000C812, 0x00000000, 0x00000000, 0x01E30018, 0x00003812,
    0x0327C821, 0x00000000, 0x03060018, 0x00003812, 0x0327C821, 0x0019CB03,
    0xA5990002, 0x85620004, 0x8563000A, 0x85660010, 0x01C20018, 0x0000C812,
    0x00000000, 0x00000000, 0x01E30018, 0x00003812, 0x0327C821, 0x00000000,
    0x03060018, 0x00003812, 0x0327C821, 0x0019CB03, 0xA5990004, 0x258C0006,
    0x25ADFFFF, 0x15A0FFC9, 0x00000000, 0x3C0C8001, 0x258CB580, 0x858D0000,
    0x8C8E0014, 0x00000000, 0x01CD7021, 0xAC8E0014, 0x858D0002, 0x8C8E0018,
    0x00000000, 0x01CD7021, 0xAC8E0018, 0x858D0004, 0x8C8E001C, 0x00000000,
    0x01CD7021, 0xAC8E001C, 0x080029D1, 0x00000000, 0x856D0000, 0x00000000,
    0xA48D0000, 0x856D0002, 0x00000000, 0xA48D0002, 0x856D0004, 0x00000000,
    0xA48D0004, 0x856D0006, 0x00000000, 0xA48D0006, 0x856D0008, 0x00000000,
    0xA48D0008, 0x856D000A, 0x00000000, 0xA48D000A, 0x856D000C, 0x00000000,
    0xA48D000C, 0x856D000E, 0x00000000, 0xA48D000E, 0x856D0010, 0x00000000,
    0xA48D0010, 0x3C0C800C, 0x258C9F20, 0x8D0F007C, 0x00000000, 0x05E10016,
    0x00000000, 0x3C0E8020, 0x01EE702B, 0x11C00012, 0x00000000, 0x8DED0284,
    0x850E0070, 0x00000000, 0x01AE6821, 0xAC8D0014, 0x858D0002, 0x850E0072,
    0x00000000, 0x01AE6821, 0xAC8D0018, 0x8DED028C, 0x850E0074, 0x00000000,
    0x01AE6821, 0xAC8D001C, 0x080029C9, 0x00000000, 0x858D0000, 0x850E0070,
    0x00000000, 0x01AE6821, 0xAC8D0014, 0x858D0002, 0x850E0072, 0x00000000,
    0x01AE6821, 0xAC8D0018, 0x858D0004, 0x850E0074, 0x00000000, 0x01AE6821,
    0xAC8D001C, 0x8D0D0078, 0x00000000, 0x11A00005, 0x00000000, 0x950D0076,
    0x3C0C800B, 0x00000000, 0xAD8DDAA8, 0x950D0018, 0x00000000, 0x11A00002,
    0x00000000, 0xA48D0020, 0x08002A74, 0x00000000, 0x08007DA2, 0x00000000,
};
static const uint32_t SCAVE11_US10[147] = {
    0x3C098001, 0x2529A780, 0xAD3F0018, 0x3C0A0008, 0x254AD8B4, 0x001F68C0,
    0x000D68C2, 0x15AA0006, 0x00000000, 0xAD330014, 0x3C0A800B, 0x8D4ADA48,
    0x00000000, 0xAD2A001C, 0x3C0C8001, 0x258CA940, 0x3C0E800B, 0x8DCEE098,
    0x858D0000, 0x31CE0001, 0x3C0A8001, 0x8D4AB5C0, 0x00000000, 0x2D4A0001,
    0x014E5024, 0x11A00026, 0x00007821, 0x15400002, 0x00000000, 0x000D6823,
    0x84980000, 0x00000000, 0x030D0018, 0x0000C012, 0x00000000, 0x00000000,
    0x0018C303, 0x8C990014, 0x00000000, 0x0338C821, 0xAC990014, 0x84980006,
    0x00000000, 0x030D0018, 0x0000C012, 0x00000000, 0x00000000, 0x0018C303,
    0x8C990018, 0x00000000, 0x0338C821, 0xAC990018, 0x8498000C, 0x00000000,
    0x030D0018, 0x0000C012, 0x00000000, 0x00000000, 0x0018C303, 0x8C99001C,
    0x00000000, 0x0338C821, 0xAC99001C, 0x254F0001, 0x000FC080, 0x030CC021,
    0x8F190008, 0x00000000, 0x27390001, 0xAF190008, 0x8F190014, 0x00000000,
    0x000F7F80, 0x8D090010, 0x8D6D0014, 0x312A0007, 0x000A5080, 0x01485021,
    0x01AF6825, 0xAD4D0050, 0xAD0D001C, 0x25290001, 0xAD090010, 0x25870088,
    0x31A60003, 0x00C73021, 0x90C30000, 0x000D3082, 0x30C60003, 0x00C73021,
    0x90C60000, 0x00000000, 0x00063200, 0x00661825, 0x000D3102, 0x30C60003,
    0x00C73021, 0x90C60000, 0x00000000, 0x00063400, 0x00661825, 0x3C066000,
    0x00661825, 0x3C09800B, 0x8D29DA48, 0x000EC080, 0x030CC021, 0x8F0A0080,
    0x00000000, 0x112A0023, 0x00000000, 0xAF090080, 0x000EC140, 0x000E4900,
    0x0309C021, 0x030CC021, 0x27180020, 0xAF190004, 0xAF030024, 0x3C19800B,
    0x27394350, 0x000E4880, 0x0329C821, 0x8F390000, 0x00000000, 0x8F290000,
    0x3C0D00FF, 0x35ADFFFF, 0x012D5024, 0x3C020300, 0x01425025, 0xAF0A0020,
    0x270A0020, 0x014D5024, 0x01425025, 0xAF0A0010, 0x270A0010, 0x014D5024,
    0x01425025, 0xAF0A0000, 0x3C02FF00, 0x01224824, 0x030D5024, 0x012A4825,
    0xAF290000, 0x08007DA2, 0x00000000,
};
static const uint32_t PADCAVE_PAL[98] = {
    0x27BDFFE8, 0xAFBF0014, 0x0C0059CA, 0x00000000, 0x3C088001, 0x2508A780,
    0x8D090010, 0x00000000, 0x25290001, 0xAD090010, 0x8D090000, 0x00000000,
    0x1120004E, 0x00000000, 0x3C09800B, 0x8D29C438, 0x3C0A5800, 0x00000000,
    0x012A4824, 0x15200047, 0x00000000, 0x3C09800B, 0x8D29C3EC, 0x00000000,
    0x1D200042, 0x00000000, 0x950A0004, 0x950B0006, 0x00000000, 0x01606027,
    0x018A6024, 0x01406827, 0x01AB6824, 0xA50A0006, 0x950E000A, 0x950F0008,
    0x3C18800B, 0x8F18C3E4, 0x00000000, 0x01F87821, 0x31EF0FFF, 0x9106000C,
    0x9107000D, 0x3C19800B, 0x27391618, 0x97220000, 0x00000000, 0x004A1025,
    0xA7220000, 0x97220002, 0x00000000, 0x004C1025, 0xA7220002, 0x97220004,
    0x00000000, 0x004D1025, 0xA7220004, 0x11C0000A, 0x00000000, 0xA72F0008,
    0x8722000A, 0x00000000, 0x14400003, 0x00000000, 0x24020001, 0xA722000A,
    0xA326000E, 0xA327000F, 0x97220020, 0x00000000, 0x004A1025, 0xA7220020,
    0x97220022, 0x00000000, 0x004C1025, 0xA7220022, 0x97220024, 0x00000000,
    0x004D1025, 0xA7220024, 0x11C0000A, 0x00000000, 0xA72F0028, 0x8722002A,
    0x00000000, 0x14400003, 0x00000000, 0x24020001, 0xA722002A, 0xA326002E,
    0xA327002F, 0x0C002C40, 0x00000000, 0x8FBF0014, 0x00000000, 0x27BD0018,
    0x03E00008, 0x00000000,
};
static const uint32_t PADCAVE_US10[98] = {
    0x27BDFFE8, 0xAFBF0014, 0x0C005B9F, 0x00000000, 0x3C088001, 0x2508A780,
    0x8D090010, 0x00000000, 0x25290001, 0xAD090010, 0x8D090000, 0x00000000,
    0x1120004E, 0x00000000, 0x3C09800B, 0x8D29DB3C, 0x3C0A5800, 0x00000000,
    0x012A4824, 0x15200047, 0x00000000, 0x3C09800B, 0x8D29DAB0, 0x00000000,
    0x1D200042, 0x00000000, 0x950A0004, 0x950B0006, 0x00000000, 0x01606027,
    0x018A6024, 0x01406827, 0x01AB6824, 0xA50A0006, 0x950E000A, 0x950F0008,
    0x3C18800B, 0x8F18DAA8, 0x00000000, 0x01F87821, 0x31EF0FFF, 0x9106000C,
    0x9107000D, 0x3C19800B, 0x27392D38, 0x97220000, 0x00000000, 0x004A1025,
    0xA7220000, 0x97220002, 0x00000000, 0x004C1025, 0xA7220002, 0x97220004,
    0x00000000, 0x004D1025, 0xA7220004, 0x11C0000A, 0x00000000, 0xA72F0008,
    0x8722000A, 0x00000000, 0x14400003, 0x00000000, 0x24020001, 0xA722000A,
    0xA326000E, 0xA327000F, 0x97220020, 0x00000000, 0x004A1025, 0xA7220020,
    0x97220022, 0x00000000, 0x004C1025, 0xA7220022, 0x97220024, 0x00000000,
    0x004D1025, 0xA7220024, 0x11C0000A, 0x00000000, 0xA72F0028, 0x8722002A,
    0x00000000, 0x14400003, 0x00000000, 0x24020001, 0xA722002A, 0xA326002E,
    0xA327002F, 0x0C002C40, 0x00000000, 0x8FBF0014, 0x00000000, 0x27BD0018,
    0x03E00008, 0x00000000,
};
static const uint32_t ACTCAVE_PAL[77] = {
    0x3C088001, 0x2508AC60, 0x8D090010, 0x00000000, 0x25290001, 0xAD090010,
    0x8D090000, 0x00000000, 0x10490007, 0x00000000, 0x8D090004, 0x00000000,
    0x1049001A, 0x00000000, 0x00400008, 0x00000000, 0x27BDFFE8, 0xAFBF0014,
    0x8D0C0014, 0x00000000, 0x258C0001, 0xAD0C0014, 0x3C0A800C, 0x854B8852,
    0x00000000, 0xAD0B000C, 0x240B0001, 0xA54B8852, 0x0040F809, 0x00000000,
    0x3C088001, 0x2508AC60, 0x8D0B000C, 0x3C0A800C, 0xA54B8852, 0x8FBF0014,
    0x27BD0018, 0x03E00008, 0x00000000, 0x27BDFFE8, 0xAFBF0014, 0x8D0C0018,
    0x00000000, 0x258C0001, 0xAD0C0018, 0xAD00000C, 0x8D0A0008, 0x00000000,
    0x11400008, 0x00000000, 0x8D4B0028, 0x00000000, 0x316C0080, 0xAD0C000C,
    0x240DFF7F, 0x016D5824, 0xAD4B0028, 0x0040F809, 0x00000000, 0x3C088001,
    0x2508AC60, 0x8D0C000C, 0x00000000, 0x11800009, 0x00000000, 0x8D0A0008,
    0x00000000, 0x11400005, 0x00000000, 0x8D4B0028, 0x00000000, 0x356B0080,
    0xAD4B0028, 0x8FBF0014, 0x27BD0018, 0x03E00008, 0x00000000,
};
static const uint32_t ACTCAVE_US10[77] = {
    0x3C088001, 0x2508AC60, 0x8D090010, 0x00000000, 0x25290001, 0xAD090010,
    0x8D090000, 0x00000000, 0x12090007, 0x00000000, 0x8D090004, 0x00000000,
    0x1209001A, 0x00000000, 0x02000008, 0x00000000, 0x27BDFFE8, 0xAFBF0014,
    0x8D0C0014, 0x00000000, 0x258C0001, 0xAD0C0014, 0x3C0A800C, 0x854B9F72,
    0x00000000, 0xAD0B000C, 0x240B0001, 0xA54B9F72, 0x0200F809, 0x00000000,
    0x3C088001, 0x2508AC60, 0x8D0B000C, 0x3C0A800C, 0xA54B9F72, 0x8FBF0014,
    0x27BD0018, 0x03E00008, 0x00000000, 0x27BDFFE8, 0xAFBF0014, 0x8D0C0018,
    0x00000000, 0x258C0001, 0xAD0C0018, 0xAD00000C, 0x8D0A0008, 0x00000000,
    0x11400008, 0x00000000, 0x8D4B0028, 0x00000000, 0x316C0080, 0xAD0C000C,
    0x240DFF7F, 0x016D5824, 0xAD4B0028, 0x0200F809, 0x00000000, 0x3C088001,
    0x2508AC60, 0x8D0C000C, 0x00000000, 0x11800009, 0x00000000, 0x8D0A0008,
    0x00000000, 0x11400005, 0x00000000, 0x8D4B0028, 0x00000000, 0x356B0080,
    0xAD4B0028, 0x8FBF0014, 0x27BD0018, 0x03E00008, 0x00000000,
};
// Act-dispatch wrapper (asm_act.py, emulated: 7 cases pass on US and PAL). GV_ExecActorSystem's `jalr <act>` becomes
// `jal ACTCAVE`. While box.c acts, first_person = 1 (its inside-the-box view); while the equipped weapon acts, Snake's
// body is shown (so a body hidden from the camera does not hide the gun, grenade or laser).
static const uint32_t ACTMB = 0x8000AC60, ACTCAVE = 0x8000AC80, ACT_HOOK_WORD = 0x0C002B20, ACTCAVE_WORDS = 77;
static const uint32_t PADCAVE_WORDS = 98;   // v11: + jal CCODE (the call routine) at its exit
// v11 call routine (asm_call.py): the DLL asks the game to call one of its own functions, once per frame at most
static const uint32_t CALLCAVE[38] = {
    0x27BDFFD8, 0xAFBF0024, 0x3C088001, 0x2508AC80, 0x8D090024, 0x00000000,
    0x25290001, 0xAD090024, 0x8D090000, 0x8D0A0004, 0x00000000, 0x112A0015,
    0x00000000, 0xAD090004, 0x8D190008, 0x00000000, 0x13200010, 0x00000000,
    0x8D0B001C, 0x00000000, 0xAFAB0010, 0x8D04000C, 0x8D050010, 0x8D060014,
    0x8D070018, 0x0320F809, 0x00000000, 0x3C088001, 0x2508AC80, 0xAD020020,
    0x8D090004, 0x00000000, 0xAD090028, 0x8FBF0024, 0x00000000, 0x27BD0028,
    0x03E00008, 0x00000000,
};
static const uint32_t CALLCAVE_WORDS = sizeof(CALLCAVE) / 4;
// v13: the call routine has 10 slots (asm_call2.py); slot 0 = the first-person overlays (was v11's single mailbox)
static const uint32_t CALLCAVE2[46] = {
    0x27BDFFD8, 0xAFBF0024, 0xAFB00020, 0xAFB1001C, 0x3C088001, 0x2508B2FC,
    0x8D090000, 0x00000000, 0x25290001, 0xAD090000, 0x3C108001, 0x2610B300,
    0x2411000A, 0x8E090000, 0x8E0A0004, 0x00000000, 0x112A0013, 0x00000000,
    0xAE090004, 0x8E190008, 0x00000000, 0x1320000E, 0x00000000, 0x8E0B001C,
    0x00000000, 0xAFAB0010, 0x8E04000C, 0x8E050010, 0x8E060014, 0x8E070018,
    0x0320F809, 0x00000000, 0xAE020020, 0x8E090004, 0x00000000, 0xAE090024,
    0x2631FFFF, 0x26100028, 0x1E20FFE6, 0x00000000, 0x8FBF0024, 0x8FB00020,
    0x8FB1001C, 0x27BD0028, 0x03E00008, 0x00000000,
};
static const uint32_t CALLCAVE2_WORDS = sizeof(CALLCAVE2) / 4;
static const uint32_t CCODE = 0x8000B100, SLOT2 = 0x8000B300, SLOT2SZ = 0x28, NSLOT2 = 10, CALL2_ALIVE = 0x8000B2FC;
static const uint32_t CMB = SLOT2, CMB_BOXFLAG = 0x8000B500;                // slot 0, and the box sight's keep-flag
static const uint32_t ITEMLIST = 0x8000B510, HZDV = 0x8000B540, HZDPT = 0x8000B560;   // item-use list, line ends, hit points
enum { SL_OVL = 0, SL_HZD_R = 1, SL_HZD_L = 4, SL_ITEM = 7, SL_KNOCK = 8 };
// v13 bar recorder (asm_bar.py) and hands (asm_hands.py), US 1.0
static const uint32_t BARCAVE_US10[35] = {
    0x3C088001, 0x25089FB0, 0x8D090000, 0x00000000, 0x25290001, 0xAD090000,
    0x8FAA0018, 0x250B0010, 0x240C0008, 0x8D6D0000, 0x00000000, 0x11AA000C,
    0x00000000, 0x11A0000A, 0x00000000, 0x258CFFFF, 0x1D80FFF8, 0x256B001C,
    0x312D0007, 0x000D70C0, 0x000D6880, 0x01CD7023, 0x010E5821, 0x256B0010,
    0xAD6A0000, 0xAD670004, 0x8FAD0010, 0x8FAE0014, 0xAD690010, 0xAD6D0008,
    0xAD6E000C, 0xAD650014, 0xAD660018, 0x0801046B, 0x00000000,
};
static const uint32_t HANDCAVE_US10[117] = {
    0x27BDFFD8, 0xAFBF0024, 0xAFB00020, 0xAFB1001C, 0xAFB20018, 0xAFB30014,
    0xAFB40010, 0x00808021, 0x00A08821, 0x3C128000, 0x265275E0, 0x8E48000C,
    0x00000000, 0x25080001, 0xAE48000C, 0x8E480000, 0x00000000, 0x11000057,
    0x00000000, 0x8E480004, 0x00000000, 0x15100053, 0x00000000, 0x1A200051,
    0x00000000, 0x2A280019, 0x1100004E, 0x00000000, 0x8E480010, 0x00000000,
    0x25080001, 0xAE480010, 0x00009821, 0x3C141F80, 0x26940040, 0x96480008,
    0x00000000, 0x1113001C, 0x00000000, 0x9648000A, 0x00000000, 0x1113002A,
    0x00000000, 0x02534821, 0x91290060, 0x240A0002, 0x00000000, 0x112A0034,
    0x00000000, 0x26580020, 0x11200002, 0x00000000, 0x26580040, 0xAE800000,
    0xAE800004, 0xAE800008, 0xAE80000C, 0xAE800010, 0x8F080014, 0x8F090018,
    0x8F0A001C, 0xAE880014, 0xAE890018, 0xAE8A001C, 0x10000023, 0x00000000,
    0x8E480020, 0x8E490024, 0x8E4A0028, 0x8E4B002C, 0x8E4C0030, 0x8E4D0034,
    0x8E4E0038, 0x8E4F003C, 0xAE880000, 0xAE890004, 0xAE8A0008, 0xAE8B000C,
    0xAE8C0010, 0xAE8D0014, 0xAE8E0018, 0xAE8F001C, 0x10000011, 0x00000000,
    0x8E480040, 0x8E490044, 0x8E4A0048, 0x8E4B004C, 0x8E4C0050, 0x8E4D0054,
    0x8E4E0058, 0x8E4F005C, 0xAE880000, 0xAE890004, 0xAE8A0008, 0xAE8B000C,
    0xAE8C0010, 0xAE8D0014, 0xAE8E0018, 0xAE8F001C, 0x26730001, 0x26940020,
    0x0271402A, 0x1500FFBB, 0x00000000, 0x02002021, 0x0C0073B1, 0x02202821,
    0x8FBF0024, 0x8FB00020, 0x8FB1001C, 0x8FB20018, 0x8FB30014, 0x8FB40010,
    0x27BD0028, 0x03E00008, 0x00000000,
};
static const uint32_t BARTAB = 0x80009FB0, BARREC = 0x8000A0C0, HMB = 0x800075E0, HCODE = 0x80007A60;
struct V13Addr { uint32_t drawbar, drawbar_sites[5], item_use, hands_site, screenmodels, hzd_check, hzd_point, hzd_atr,
                 new_afterse, noise_table, player_control_map_off, alert_mode; };
static const V13Addr V13_US10 = {0x800411AC, {0x80041814, 0x80041854, 0x800418B8, 0x8004190C, 0x80041960}, 0x8003E5AC,
                                 0x8001D998, 0x8001CEC4, 0x80029974, 0x80029DB0, 0x80029D48, 0x80062624, 0x800B9E48, 0x2C, 0x800AE178};
// first-person overlays: the game's own constructors (US 1.0, found by their GV_SetNamedActor filename strings; decomp
// thing/sight.c NewSight, equip/gmsight.c NewGasmaskSight, equip/gglsight.c NewGoggleSight, libgv GV_DestroyOtherActor)
struct OvlAddr { uint32_t new_sight, new_gasmask_sight, new_goggle_sight, destroy_other, sight_cur; };
static const OvlAddr OVL_US10 = {0x80074304, 0x800658F4, 0x8007AE84, 0x8001543C, 0x800A1D4C};
static std::atomic<uint32_t> g_call_epoch{0};   // ++ at every hook install: the call mailbox was reset
static uint32_t gc_issue(uint8_t* ram, int slot, uint32_t fn, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0, uint32_t a4 = 0)
{   // fn + args first, the request number last; the routine runs it on the next game frame
    uint32_t base = SLOT2 + slot * SLOT2SZ, sq = std::max(rd32(ram, base), rd32(ram, base + 4)) + 1;
    uint32_t args[6] = {fn, a0, a1, a2, a3, a4}; wr(ram, base + 8, args, sizeof args); wr32(ram, base, sq); return sq;
}
static bool gc_done(uint8_t* ram, int slot, uint32_t sq, uint32_t* v0 = nullptr)
{
    uint32_t base = SLOT2 + slot * SLOT2SZ;
    if (rd32(ram, base + 0x24) != sq) return false;
    if (v0) *v0 = rd32(ram, base + 0x20);
    return true;
}
static const V13Addr* v13_addr(const struct GameTable* T);
static std::atomic<bool> g_v13_bars{false}, g_v13_hands{false};       // hooks in place (install)
static std::atomic<bool> g_hands_live{false}, g_hands_fail{false}, g_hands_drawing{false}, g_scoped{false}, g_aim_head{false};
static std::atomic<int> g_item_use_req{0}, g_knock_rumble{0};
static std::atomic<uint32_t> g_hands_hits{0}, g_hands_knocks{0}, g_hands_tests{0};
static std::atomic<int> g_last_knock_atr{-1};
static const uint32_t PMBOX = 0x8000A780, PADCAVE = 0x8000A7A0, PAD_HOOK_WORD = 0x0C0029E8;   // jal 0x8000A7A0
static const uint32_t CAVE_WORDS = 186;   // v15: routine v12 (mode-1 offset at DIO)
static const uint32_t SCAVE_WORDS = 147;   // v20: routine v13 stereo block (true-stereo left eye)
static const uint32_t SMB = 0x8000A940, SCAVE = 0x8000A9D0;   // stereo mailbox (0x8C bytes) and block (ends 0x8000AC08)
static const uint32_t CODE_END = SCAVE + SCAVE_WORDS * 4;   // 0x8000AC08
static const uint32_t MBOX       = 0x8000A400;
static const uint32_t CAVE       = 0x8000A480;
static const uint32_t HOOK_WORD  = 0x0C002920; // jal 0x8000A480
static const uint32_t MBOX_MAGIC = 0x4256434D; // 'MCVB' (routine v11)
static const uint32_t MB_ENABLE = 0x04, MB_SLOT = 0x08, MB_CALLS = 0x0C, MB_APPLIED = 0x10, MB_LAST = 0x14,
                      MB_FOV = 0x18, MB_LASTSEQ = 0x1C, MB_M0 = 0x20, MB_SEQ0 = 0x34, MB_M1 = 0x38, MB_SEQ1 = 0x4C,
                      MB_RING = 0x50, MB_OFF = 0x70, MB_PADORG = 0x76, MB_PADORG_EN = 0x78, MB_SNAKEOBJS = 0x7C, MB_SIZE = 0x80;
// MB_ENABLE (+0x04) is the routine's mode: 0 off, 1 head-look (eye = eye * M), 2 walk (eye = M, t = SnakeCamera + offset)

// ---------------------------------------------------------------- per-version address tables
struct GameTable {
    const char* name;
    uint32_t gv_time, game_status, pauselevel, stage_name, gm_camera, chanl1, paddata;
    uint32_t hook_site, hook_orig, sig2_site, sig2_word;
    const uint32_t* cave; const uint32_t* scave;
    uint32_t hook2_site, hook2_orig;   // FrameRunDemo's jal TransposeMatrix (cutscene camera)
    uint32_t snakecam, padorg;         // GM_SnakeCamera (position +0, rotate2 +0x28), PadOrg (stick origin)
    uint32_t actor_lists, snake_act;   // GV actor lists (9 x 0x44, next at +4, act at +8); Snake's act function
    const uint32_t* padcave; uint32_t pad_site, pad_orig;   // frame loop's jal GV_UpdatePadSystem (libdg/dgd.c)
    uint32_t player_control, player_status;                 // GM_PlayerControl (CONTROL*: rot.vy +0xA, turn.vy +0x4E), GM_PlayerStatus
    uint32_t o2;                                            // v10: GM_O2 (short, 1024 = full)
};
static const GameTable T_PAL = { "PAL SLES-01370",
    0x800AC39C, 0x800AC438, 0x800AC980, 0x800B5DE0, 0x800B8830, 0x800B2A44, 0x800B1618,
    0x800174F0, 0x0C007B4E, 0x80017524, 0x0C025036, CAVE11_PAL, SCAVE11_PAL, 0x8007BF40, 0x0C007B4E, 0x800B8800, 0x800AC3E4, 0x800ADC70, 0x8005B628, PADCAVE_PAL, 0x8001F3FC, 0x0C0059CA, 0x800ACA4C, 0x800ACAA8, 0x800ACA8C };
static const GameTable T_US10 = { "US 1.0 SLUS-00594",
    0x800ADA48, 0x800ADB3C, 0x800AE0A0, 0x800B7500, 0x800B9F50, 0x800B4164, 0x800B2D38,
    0x80017AE0, 0x0C007DA2, 0x80017B14, 0x0C0255E1, CAVE11_US10, SCAVE11_US10, 0x8007D8AC, 0x0C007DA2, 0x800B9F20, 0x800ADAA8, 0x800AF390, 0x8005CE1C, PADCAVE_US10, 0x8001FD4C, 0x0C005B9F, 0x800AE16C, 0x800AE1C8, 0x800AE1AC };

// state shared between threads
static std::atomic<uint8_t*>         g_ram{nullptr};      // live PS1 RAM, set when our hook is installed
static std::atomic<bool>             g_hook_ready{false}; // mailbox + routine in place
static std::atomic<bool>             g_head_valid{false}; // OpenXR is delivering head poses
static std::atomic<bool>             g_recentre{true};    // request: make current head direction forward
static std::atomic<int>              g_head_yaw_cdeg{0}, g_head_pitch_cdeg{0}, g_head_roll_cdeg{0};
static std::atomic<const struct GameTable*> g_T{nullptr};  // table of the running game (for the Present thread)
static std::atomic<int>              g_tag_delay{1};       // game frames between the pose a frame used and its present
static std::atomic<bool>             g_save_frame{false};  // F10
static std::atomic<bool>             g_save_wrist{false};  // v11 F10: the wrist panel texture too
static std::atomic<bool>             g_scan_ok{false};     // PS1 game picture on screen (not MC menus): auto-crop may measure
static std::atomic<uint32_t>         g_scan_epoch{0};      // bumped when the game (re)starts: auto-crop starts over
// what the most recent captured picture was drawn with (set in Present, read by the XR thread)
static std::atomic<uint32_t>         g_cap_tag_seq{0}, g_cap_tag_screen{320};
static std::atomic<int>              g_cap_tag_tracked{0};
// timing (microseconds) of the Present-hook work, read by the XR thread for its log line
static std::atomic<uint32_t>         g_pres_us_max{0}, g_pres_us_sum{0}, g_pres_n{0};
static double qpc_us()
{
    static LARGE_INTEGER f = {}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart * 1e6 / (double)f.QuadPart;
}
// head matrix handed from the XR thread to the writer thread (the XR thread never touches game RAM)
static SRWLOCK g_pend_lock = SRWLOCK_INIT;
static int16_t g_pend_M[9]; static uint32_t g_pend_seq = 0; static bool g_pend_fresh = false; static double g_pend_yaw = 0;
static XrVector3f g_pend_p = {0, 0, 0};   // head position (LOCAL) of the pending pose
static XrVector3f g_pend_ref = {0, 0, 0}; // recentred head position (LOCAL): lean is measured from here
static std::atomic<int> g_lean_mm[3] = {}, g_lean_max_mm{0};
// v15: DIO = the routine's mode-1 offset (int16 x,y,z); DIO_FAKE = the diorama's stand-in body DG_OBJS: the routine reads its
// "head joint" x at +0x284 and z at +0x28C, i.e. 0x8000B5A0 / 0x8000B5A8 (absolute world x, z written by the writer)
static const uint32_t DIO = 0x8000B580, DIO_FAKE = 0x8000B5A0 - 0x284;
// v20 TRUE STEREO (asm_ts.py): mailbox, code, hooks (US 1.0)
static const uint32_t TSMB = 0x8000B5C0, TSCODE1 = 0x8000A150, TSCODE2 = 0x8000B5E0;
static const uint32_t TSCAVE1_US10[139] = {
    0x27BDFFD0, 0xAFBF002C, 0xAFB00028, 0xAFB10024, 0xAFB20020, 0x00808021,
    0x0C0061B8, 0x02002021, 0x3C128001, 0x2652B5C0, 0x8E480000, 0x00000000,
    0x11000077, 0x00000000, 0x3A110001, 0x0C006150, 0x02202021, 0x3C0C8001,
    0x258CA940, 0x0011C140, 0x00114900, 0x0309C021, 0x030CC021, 0x27180020,
    0x8D99001C, 0x00000000, 0xAF190004, 0x00104940, 0x00105100, 0x012A4821,
    0x012C4821, 0x25290020, 0x8D2A0024, 0x00000000, 0xAF0A0024, 0x3C19800B,
    0x27394350, 0x00114880, 0x0329C821, 0x8F390000, 0x00000000, 0x8F290000,
    0x3C0D00FF, 0x35ADFFFF, 0x012D5024, 0x3C020300, 0x01425025, 0xAF0A0020,
    0x270A0020, 0x014D5024, 0x01425025, 0xAF0A0010, 0x270A0010, 0x014D5024,
    0x01425025, 0xAF0A0000, 0x3C02FF00, 0x01224824, 0x030D5024, 0x012A4825,
    0xAF290000, 0x3C0B800B, 0x256B4164, 0x8D680024, 0x8D690044, 0x8D6A0048,
    0x8D6C004C, 0xAFA80010, 0xAFA90014, 0xAFAA0018, 0xAFAC001C, 0x8E4A0004,
    0x00000000, 0x010A4023, 0xAD680024, 0x856E0030, 0x00000000, 0x01CA0018,
    0x00007012, 0x00000000, 0x00000000, 0x000E7303, 0x8D6F0044, 0x00000000,
    0x01EE7821, 0xAD6F0044, 0x856E0036, 0x00000000, 0x01CA0018, 0x00007012,
    0x00000000, 0x00000000, 0x000E7303, 0x8D6F0048, 0x00000000, 0x01EE7821,
    0xAD6F0048, 0x856E003C, 0x00000000, 0x01CA0018, 0x00007012, 0x00000000,
    0x00000000, 0x000E7303, 0x8D6F004C, 0x00000000, 0x01EE7821, 0xAD6F004C,
    0x3C0C800B, 0x258CE098, 0xAD910000, 0x0C0061B8, 0x02202021, 0x3C0C800B,
    0x258CE098, 0xAD900000, 0x3C0B800B, 0x256B4164, 0x8FA80010, 0x8FA90014,
    0x8FAA0018, 0x8FAC001C, 0xAD680024, 0xAD690044, 0xAD6A0048, 0xAD6C004C,
    0x24080001, 0xAE480008, 0x8E48000C, 0x00000000, 0x25080001, 0xAE48000C,
    0x8FBF002C, 0x8FB00028, 0x8FB10024, 0x8FB20020, 0x27BD0030, 0x03E00008,
    0x00000000,
};
static const uint32_t TSCAVE2_US10[44] = {
    0x27BDFFE0, 0xAFBF001C, 0xAFB00018, 0x00808021, 0x0C00613B, 0x00000000,
    0x3C088001, 0x2508B5C0, 0x8D090000, 0x8D0A0008, 0x00000000, 0x1120000C,
    0x00000000, 0x1140000A, 0x00000000, 0xAD000008, 0x8D090010, 0x00000000,
    0x25290001, 0xAD090010, 0x0C00613B, 0x3A040001, 0x0C0248BC, 0x00002021,
    0x8FBF001C, 0x8FB00018, 0x27BD0020, 0x03E00008, 0x00000000, 0x3C088001,
    0x2508B5C0, 0x8D090000, 0x00000000, 0x11200003, 0x240A0140, 0xA4800000,
    0x240A0280, 0xA48A0004, 0x8D090014, 0x00000000, 0x25290001, 0xAD090014,
    0x08024AC8, 0x00000000,
};
static const uint32_t TS_HOOKS_US10[3][3] = { {0x80017904, 0x0C0061B8, 0x0C002854}, {0x80017894, 0x0C00613B, 0x0C002D78}, {0x80017874, 0x0C024AC8, 0x0C002D95} };
static std::atomic<bool> g_ts_installed{false}, g_ts_on{false};
static std::atomic<int>  g_sbs_hud_half{-1};    // newest picture side by side: the half with the game's HUD / overlay (left eye), else -1
static std::atomic<uint32_t> g_sbs_pics{0};
static std::atomic<bool>     g_dio_reset{false};            // main -> writer: place the table again
static std::atomic<int>      g_dio_yaw_units{0};            // main -> writer: table heading on a reset (4096 = 360 deg)
static std::atomic<int>      g_dio_scale_x100{2000};        // writer: current scale (for the stereo baseline and hold)
static std::atomic<int>      g_dio_cuts{0}, g_dio_turns{0}, g_dio_clamped{0};
static std::atomic<uint32_t> g_sp_objs{0};                  // v15 set piece: Snake's body DG_OBJS inside the vehicle / rope / rack actor
static std::atomic<int>      g_sp_kind{-1};
static std::atomic<bool>     g_setpiece{false};             // main: the eye is in a set piece right now
static const char* const kSpName[7] = {"jeep", "jeep (escape)", "sne17a", "REX top", "rappel rope", "torture rack", "sne_03c"};
static const char* const kSpFile[7] = {"jeep_sne.c", "pkjp_sne.c", "sne17a.c", "snake18.c", "rope.c", "torture.c", "sne_03c.c"};
static std::atomic<int>      g_mode_want{0}, g_mode_cur{0};   // routine mode wanted (main thread) / written (writer)
static std::atomic<int>      g_yaw_ref_units{0};              // Snake's facing when walking mode started (4096 = 360 deg)
static std::atomic<bool>     g_yaw_ref_new{false};
static std::atomic<int>      g_walk_heading_cdeg{0};
// VR controller state handed from the XR thread to the writer thread
struct VrPad { bool active = false; uint16_t buttons = 0; uint16_t angle = 0; bool stick = false; uint8_t lx = 128, ly = 128;
               float rx = 0, ry = 0; bool pa = false, pb = false; };   // v21: physical A / B (menus: Enter / Escape)
static SRWLOCK g_vrpad_lock = SRWLOCK_INIT;
static VrPad   g_vrpad;
static std::atomic<int> g_vrpad_buttons_log{0};
// performance counters (main thread fills, XR thread logs)
static std::atomic<int> g_perf_gpu3d{-1}, g_perf_gpu3d_self{-1}, g_perf_enc{-1}, g_perf_cpu{-1}, g_perf_net_kbps{-1}, g_perf_wifi{-1};
static std::atomic<bool> g_perf_ok{false};
static std::atomic<uint32_t> g_wr_us_max{0}, g_wr_n{0};
// stereo (writer decides, XR thread logs)
static std::atomic<bool>     g_stereo_on{true};            // F5
static std::atomic<int>      g_stereo_half{0};             // half offset written this moment (game units, 0 = mono)
static std::atomic<uint32_t> g_hold_move{0}, g_hold_head{0}, g_hold_turn{0}, g_hold_fps{0};   // hold triggers
static std::atomic<int>      g_game_fps_x10{0};
static std::atomic<uint32_t> g_snap_count{0}, g_body_turns{0};
static std::atomic<bool>     g_cut_pov{false};
static std::atomic<bool>     g_keep_pose{false};      // a pause that stays in the VR view: keep the last picture's pose
// MOTION (gun in hand). XR thread -> writer: the right hand, predicted ahead (LOCAL space).
struct GunHand { bool valid = false; XrQuaternionf aim_q{0, 0, 0, 1}; XrVector3f grip_p{0, 0, 0}; double t_ms = 0;
                 bool lvalid = false; XrQuaternionf laim_q{0, 0, 0, 1}; XrVector3f lgrip_p{0, 0, 0}; };   // v13: left hand
static SRWLOCK g_gunhand_lock = SRWLOCK_INIT;
static GunHand g_gunhand;
static std::atomic<double> g_ref_yaw_rad{0.0};          // recentred forward (XR thread writes)
// main thread -> writer: the equipped weapon actor (found by its source name), checked again before every write
struct GunActor { uint32_t work = 0, act = 0, fname = 0, objs = 0, orig = 0, laser = 0, light = 0, objoff = 0x20; int kind = 0;
                 uint32_t robj = 0, unit = 0; };   // kind 1 SOCOM, 2 FAMAS, 3 grenade / stun / chaff; robj = Snake's OBJECT (grenade)
// v10 throw from the hand (grenade.c): stand-in OBJECT at THROWOBJ (its objs = THROW_DGOBJS(unit)); the copy of the hand
// matrix at THROWMTX is that stand-in's objs[unit].world; its flag word (+0x28) mirrors Snake's body flag.
static const uint32_t THROWOBJ = 0x8000ADC0, THROWMTX = 0x8000B040;
static uint32_t throw_dgobjs(uint32_t unit) { return THROWMTX - 0x48 - unit * 0x5C; }
static uint32_t k0x(uint32_t a) { return ((a & 0x1FFFFFFFu) < 0x200000u && (a & 0x1FFFFFu) >= 0x10000u) ? (0x80000000u | (a & 0x1FFFFFu)) : 0; }
static std::atomic<uint32_t> g_throw_work{0}, g_throw_robj{0};   // writer: grenade work whose root_obj we replaced, and the original
static std::atomic<uint32_t> g_throw_redirects{0}, g_throw_restores{0}, g_throw_spawned{0};
static SRWLOCK g_gunact_lock = SRWLOCK_INIT;
static GunActor g_gunact;
static const uint32_t GUNMTX = 0x8000AC40;               // the MATRIX the held gun hangs from (spare RAM, 0x20 bytes)
static std::atomic<uint32_t> g_gun_writes{0}, g_gun_redirects{0}, g_gun_restores{0}, g_gun_skips{0};
static std::atomic<int>      g_gun_state{0};             // 0 off, 1 in hand, 2 waiting for the controller, 3 no weapon
static std::atomic<int>      g_gun_reach_mm{0}, g_gun_yaw_cdeg{0}, g_gun_pitch_cdeg{0};
static std::atomic<bool>     g_screen_anchor{true};
static std::atomic<uint16_t> g_melee_bits{0};           // Circle / Square from melee gestures (XR thread)
static std::atomic<int>      g_weapon_id{-1};
static std::atomic<int>      g_grip_mask{0};             // grips held (bit 0 left, bit 1 right), XR thread
static std::atomic<int>      g_pl_now{0};                // GV_PauseLevel, main thread
static std::atomic<uint32_t> g_body_hidden{0};           // Snake's body we keep hidden from the camera (walk mode), writer
static std::atomic<uint32_t> g_body_rehides{0}, g_act_runs_box{0}, g_act_runs_weap{0};
// cardboard box (box.c): its model hangs from Snake; while walking through his eyes it hangs from a zero-size matrix
static std::atomic<uint32_t> g_box_objs{0}, g_box_orig{0};
static const uint32_t FARMTX = 0x8000AC60;              // zero-size MATRIX (spare RAM, 0x20 bytes): whatever hangs from it is not seen
// wrist HUD data, read from game memory (linkvar block at stage name + 0x10; menuman.c's MenuWork for the open list)
struct HudData { bool valid = false; int vit = 0, vmax = 0, weapon = -1, wammo = 0, wmax = 0, item = -1, icount = 0;
                 int menu = 0, ln = 0, lsel = 0; int16_t lid[24] = {}, lnum[24] = {};
                 int16_t wown[10] = {}, iown[24] = {};   // v10: GM_Weapons[] / item counts (-1 = not owned)
                 int o2 = 1024; bool o2_ok = false; int pl = 0; bool radar_on = true;
                 struct OBar { char name[20]; int now, max, x, y; uint8_t rgb[3]; } ob[4]; int nob = 0; };   // v13: other LIFE bars
static std::atomic<bool> g_demo{false};       // v10: GM_GameStatus bit 31 (a cutscene is running), main thread
static SRWLOCK g_hud_lock = SRWLOCK_INIT;
static HudData g_hud;
static const char* const kWeaponName[10] = {"SOCOM", "FA-MAS", "GRENADE", "NIKITA", "STINGER", "CLAYMORE", "C4", "STUN G.", "CHAFF G.", "PSG1"};
static const char* const kItemName[24] = {"CIGS", "SCOPE", "C.BOX A", "C.BOX B", "C.BOX C", "N.V.G.", "THERM.G.", "GAS MASK", "B.ARMOR",
    "KETCHUP", "STEALTH", "BANDANA", "CAMERA", "RATION", "MEDICINE", "DIAZEPAM", "PAL KEY", "CARD", "TIME BOMB", "MINE.D", "MO DISK",
    "ROPE", "HANDKER.", "SUPPR."};
static std::atomic<bool>     g_act_hook{false};          // act-dispatch wrapper installed (main thread)
static std::atomic<bool>     g_act_live{false};          // ... and the emulator is running it (its call counter moves)
static std::atomic<uint32_t> g_act_calls{0};          // equipped weapon (GM work, stage name + 0x2C; -1 = none), main thread
static std::atomic<uint32_t> g_melee_punches{0}, g_melee_grabs{0}, g_melee_squeezes{0}, g_gun_shots_felt{0};
// layer accounting (XR thread): how often the extra quad layers were up, and how often the wrist texture was redrawn
static uint32_t g_ly_wrist = 0, g_ly_wrist_draw = 0, g_ly_panel = 0, g_ly_max = 0;   // place the floating screen where you look, next time it shows   // cutscene through Snake's eyes: routine mode 2 on the demo cast's Snake

// ---------------------------------------------------------------- finding PS1 RAM
static bool readable(DWORD prot)
{
    if (prot & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return (prot & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READ |
                    PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) != 0;
}

enum Ver { V_NONE, V_PAL, V_US10, V_US_OTHER, V_OTHER };
static const char* ver_name(Ver v)
{
    switch (v) { case V_PAL: return "PAL SLES-01370"; case V_US10: return "US 1.0 SLUS-00594";
                 case V_US_OTHER: return "US (not 1.0: probably 1.1)"; case V_OTHER: return "other (boot/menu or JP)";
                 default: return "none"; }
}

static Ver version_of(uint8_t* ram)
{
    char id[0x60] = {0};
    if (!rd(ram, 0x80010000, id, sizeof id)) return V_NONE;
    for (size_t i = 0; i + 10 <= sizeof id; ++i) {
        if (memcmp(id + i, "SLES-01370", 10) == 0) return V_PAL;
        if (memcmp(id + i, "SLUS-00594", 10) == 0) {
            char k[8] = {0}; rd(ram, 0x800ADC98, k, 7);
            return memcmp(k, "KETCHUP", 7) == 0 ? V_US10 : V_US_OTHER;
        }
    }
    for (size_t i = 0; i + 4 <= sizeof id; ++i)
        if (!memcmp(id + i, "SLPM", 4) || !memcmp(id + i, "SLES", 4) || !memcmp(id + i, "SLUS", 4)) return V_OTHER;
    return V_NONE;
}

static const GameTable* table_for(Ver v) { return v == V_PAL ? &T_PAL : v == V_US10 ? &T_US10 : nullptr; }

static bool signature_ok(uint8_t* ram, const GameTable& T)
{
    uint32_t h = rd32(ram, T.hook_site), s2 = rd32(ram, T.sig2_site);
    return s2 == T.sig2_word && (h == T.hook_orig || h == HOOK_WORD);
}

static uint8_t* find_ram(Ver* ver_out, uint32_t* off_out)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    uint8_t* p = (uint8_t*)si.lpMinimumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    static const uint32_t offs[] = { 0x60, 0x40, 0x20, 0x00, 0x80, 0xA0, 0xC0, 0xE0, 0x100 };
    while (p < (uint8_t*)si.lpMaximumApplicationAddress && VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_COMMIT && mbi.RegionSize == 0x211000 && readable(mbi.Protect)) {
            for (int pass = 0; pass < 2; ++pass)
            for (uint32_t o : offs) {
                uint8_t* b = (uint8_t*)mbi.BaseAddress + o;
                Ver v = version_of(b);
                if (v == V_NONE) continue;
                const GameTable* T = table_for(v);
                if (T && !signature_ok(b, *T)) continue;
                char hdr[3] = {0};
                if (pass == 0 && (!rd(b, 0x80010000, hdr, 3) || memcmp(hdr, "MGS", 3) != 0)) continue;
                *ver_out = v; *off_out = o;
                return b;
            }
        }
        p = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
    }
    return nullptr;
}

// ---------------------------------------------------------------- settings
struct Settings {
    // [stereo] alternate-eye stereo
    int stereo = 1, ipd_mm = 64, units_per_m = 1000, swap_eyes = 0, hold_units = 20, hold_deg_s = 300, hold_ms = 330,
        eye_timeout_ms = 120, min_fps = 20;
    bool stereo_same(const struct Settings& o) const {
        return stereo == o.stereo && ipd_mm == o.ipd_mm && units_per_m == o.units_per_m && swap_eyes == o.swap_eyes &&
               hold_units == o.hold_units && hold_deg_s == o.hold_deg_s && hold_ms == o.hold_ms &&
               eye_timeout_ms == o.eye_timeout_ms && min_fps == o.min_fps; }
    uint8_t menu_key[16] = {};   // DirectInput key code per PS1 button bit (0 = none)
    int menu_method = 0;          // 0 auto, 1 DirectInput only, 2 SendInput only, 3 off
    int enabled = 1, vr = 1, track_third_person = 0, recentre_on_first_person = 1;
    double screen_distance_m = 2.5, screen_width_m = 3.2;
    int tag_delay = 1, fov_screen = 0; double tracked_distance_m = 3.0;
    int crop_auto = 2, crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0;   // 2 = 4:3 rule, 1 = detect, 0 = manual, -1 = off
    int cutscene_head_look = 1;
    int screen_codec = 1, screen_pause = 1, screen_menus = 1;   // [screen] which pauses go on the floating screen
    // [wrist_hud] as in the Integral mod: LIFE, radar and item on the left wrist, weapon (with ammo) on the right
    int hud_wrist = 1, hud_width_mm = 90, hud_gaze_deg = 28;
    double hud_off_cm[2][3] = {{-1.0, 4.0, 10.0}, {1.0, 4.0, 10.0}}, hud_rot_deg[2][3] = {{-60, 0, 0}, {-60, 0, 0}};
    double hud_rect[4][4] = {{0.031, 0.025, 0.450, 0.150}, {0.722, 0.050, 0.244, 0.258},    // life, radar
                             {0.030, 0.755, 0.180, 0.140}, {0.780, 0.755, 0.180, 0.140}};   // item box, weapon box (of the 4:3 picture)
    int cut_snake_eyes = 1, cut_eye_forward = 100; uint16_t cut_models[8] = {0x992D, 0x34C2}; int cut_n_models = 2;
    int walk_mode = 1, eye_forward = 120, eye_up = 0, stick_deadzone = 24; double turn_speed_deg = 120;
    int turn_mode = 0, snap_deg = 30;                      // 0 smooth, 1 snap
    int body_follows_head = 1, body_deadband_deg = 30, body_turn_speed_deg = 360;
    // [motion] gun in hand: the game's own SOCOM / FAMAS model hangs from your right controller and fires from it
    int gun_in_hand = 1, gun_predict_ms = 40, gun_max_reach_mm = 1100;
    int perf_counters = 0;
    // [melee] as in the GOG mod: punch by throwing a hand forward (Circle), chokehold by bringing both hands together
    // in front of you while unarmed (holds Square, the game's grab; a sharp jerk presses it again)
    int melee_punch = 1, punch_speed_cms = 200, punch_reach_cm = 18, melee_choke = 1, choke_apart_cm = 30, choke_hold_ms = 350,
        choke_jerk_cms = 250, rumble = 1;
    int hide_body = 1;
    int lean = 1, lean_max_cm = 100, throw_from_hand = 1, fp_overlays = 1;   // v11: [walk] fp_overlays
    // v13: [hands], [scope], [wrist_hud] show_boss_life / item_use_button
    int hands = 1, hand_r = 4, hand_l = 9, hands_collision = 1, hands_knock = 1, knock_speed_cms = 60, knock_cooldown_ms = 300, hands_margin = 30;
    int hand_left_parts[8] = {7, 8, -1, -1, -1, -1, -1, -1}, hand_keep_parts[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int scope_lens = 1, scope_display_deg = 48, scope_pitch_sign = 1, aim_by_head = 1, boss_bars = 1, item_use_button = 1;   // v10: [walk] lean / lean_max_cm, [motion] throw_from_hand
    // v10 [wrist_hud], 1:1 with the GOG mod (vr_aim.cpp): mounts, gaze hysteresis, translucent pixel-font panels, game boxes
    // cut from the picture, compact lists on the grips, and the game's own HUD hidden in the view
    int hud_mount_side[2] = {0, 0}, hud_list_mount = 2, hud_hide_boxes = 1, hud_hide_life = 1, hud_hide_radar = 1, hud_hide_style = 1,
        hud_show_boxes = 1, hud_show_radar = 1, hud_bar_follows_max = 1, hud_bar_scale_pct = 125, hud_select_on_wrist = 1;
    double hud_in_off_cm[2][3] = {{3.0, 1.0, 7.0}, {-3.0, 1.0, 7.0}}, hud_in_rot_deg[2][3] = {{0, 0, 0}, {0, 0, 0}};
    double hud_back_off_cm[2][3] = {{-3.0, 1.0, 7.0}, {3.0, 1.0, 7.0}}, hud_back_rot_deg[2][3] = {{45, 0, 0}, {45, 0, 0}};
    double radar_ps1[4] = {232, 13, 76, 58};   // [wrist_hud] radar_ps1: the radar and its frame, PS1 pixels (x, y, w, h; decomp 235,16 69x52)   // [walk] hide_body: Snake's body hidden from the camera in first-person walking (as the game's own first person)
    double hud_sel[2][4] = {{0.030, 0.420, 0.530, 0.480}, {0.430, 0.420, 0.540, 0.480}};   // [wrist_hud] item_selection / weapon_selection   // [motion] perf_counters: PDH GPU / encoder / network counters + Wi-Fi query every 2 s (A/B for wireless)
    double gun_rot_deg[3] = {0, 0, 0}, gun_off_mm[3] = {0, 0, 0};
    int controls = 1;
    uint16_t map_a = 0x40, map_b = 0x20, map_x = 0x01, map_y = 0x02, map_lt = 0x10, map_rt = 0x80,
             map_lg = 0x04, map_rg = 0x08, map_lclick = 0x100, map_rclick = 0, map_menu = 0x800;
    bool rclick_recentre = true;
    struct V15 {   // v15 ([diorama], [walk] third_person_lean, [setpiece], [wrist_hud] selector / wheel_*): ints only (compared with memcmp)
        int diorama = 0, dio_scale = 20, dio_dist_cm = 60, dio_drop_cm = 50, dio_follow_radius = 1500, dio_follow_speed = 30,
            dio_cut_units = 4000, dio_reorient = 1, dio_eye_height = 1500, dio_turn_deg = 30;
        int tp_lean = 1, setpiece_eyes = 1, wheel = 2, wheel_size_cm = 30, wheel_height_cm = 12, wheel_stereo_deg = 4;
        int carousel_size_cm = 13, pause_sel = 1;   // v16
        int sub_on = 1, sub_w_cm = 80, sub_dist_cm = 110, sub_down_deg = 20, sub_strip[4] = {0, 7667, 10000, 1667};   // v17 [subtitles] (strip x 1e-4)
        int mc_menu = 1, r3_view = 1;
        int codec_ear = 1, codec_ear_cm = 17;   // v22 [controls] codec_ear_gesture / codec_ear_cm
        int blur_effect = 0;                     // v23 [cutscene] blur_effect
        int xy_start = 1;                        // v25 [controls] xy_start: X + Y together = Start
        int cut_stereo = 0;   // v18 [cutscene] stereo
        int hold_hand_cms = 25;   // v19 [stereo] hold_hand_cm_s
        int true_stereo = 0;      // v20 [stereo] true_stereo                                                                                     // v17 [controls]
    } v15;
};

// PS1 button names for [controls]; "recentre" (right stick click only) and "none" are special.
static uint16_t ps_button(const wchar_t* n, bool* recentre)
{
    struct { const wchar_t* name; uint16_t bit; } T[] = {
        {L"triangle", 0x10}, {L"circle", 0x20}, {L"cross", 0x40}, {L"square", 0x80}, {L"l2", 0x01}, {L"r2", 0x02},
        {L"l1", 0x04}, {L"r1", 0x08}, {L"select", 0x100}, {L"l3", 0x200}, {L"r3", 0x400}, {L"start", 0x800},
        {L"up", 0x1000}, {L"right", 0x2000}, {L"down", 0x4000}, {L"left", 0x8000}, {L"none", 0} };
    if (recentre) *recentre = _wcsicmp(n, L"recentre") == 0 || _wcsicmp(n, L"recenter") == 0;
    for (auto& t : T) if (_wcsicmp(n, t.name) == 0) return t.bit;
    return 0;
}

static double ini_double(const wchar_t* key, const wchar_t* def, const std::wstring& ini)
{
    wchar_t buf[64] = {0};
    GetPrivateProfileStringW(L"phase2", key, def, buf, 64, ini.c_str());
    return _wtof(buf);
}

static Settings read_ini(bool log_it)
{
    Settings s;
    std::wstring ini = g_dir + L"mgs1mc_vr.ini";
    wchar_t probe[8] = {0};
    GetPrivateProfileStringW(L"phase2", L"enabled", L"", probe, 8, ini.c_str());
    if (!probe[0]) {
        FILE* f = _wfopen(ini.c_str(), L"a");
        if (f) {
            fputs("\n; ---- Phase 2: head tracking (re-read every 2 seconds) ----\n"
                  "[phase2]\n"
                  "; 1 = install the head-tracking hook in the game, 0 = leave the game untouched\n"
                  "enabled=1\n"
                  "; 1 = start OpenXR (headset), 0 = no headset session\n"
                  "vr=1\n"
                  "; 0 = head tracking only in first person (recommended), 1 = also in third person\n"
                  "track_third_person=0\n"
                  "; 1 = wherever you look when first person starts becomes straight ahead\n"
                  "recentre_on_first_person=1\n"
                  "; the game screen in the headset (metres)\n"
                  "screen_distance_m=2.5\n"
                  "screen_width_m=3.2\n", f);
            fclose(f);
        }
    }
    s.enabled = GetPrivateProfileIntW(L"phase2", L"enabled", 1, ini.c_str());
    s.vr = GetPrivateProfileIntW(L"phase2", L"vr", 1, ini.c_str());
    s.track_third_person = GetPrivateProfileIntW(L"phase2", L"track_third_person", 0, ini.c_str());
    s.recentre_on_first_person = GetPrivateProfileIntW(L"phase2", L"recentre_on_first_person", 1, ini.c_str());
    s.screen_distance_m = ini_double(L"screen_distance_m", L"2.5", ini);
    s.screen_width_m = ini_double(L"screen_width_m", L"3.2", ini);
    if (s.screen_distance_m < 0.5 || s.screen_distance_m > 20) s.screen_distance_m = 2.5;
    if (s.screen_width_m < 0.3 || s.screen_width_m > 30) s.screen_width_m = 3.2;
    // [phase3]
    GetPrivateProfileStringW(L"phase3", L"tag_delay", L"", probe, 8, ini.c_str());
    if (!probe[0]) {
        FILE* f = _wfopen(ini.c_str(), L"a");
        if (f) {
            fputs("\n; ---- Phase 3: picture (re-read every 2 seconds) ----\n"
                  "[phase3]\n"
                  "; game frames between the head pose a frame was drawn with and its appearance on screen.\n"
                  "; If the world swims WITH your head when you turn, raise it; if it swims AGAINST, lower it. 0-6.\n"
                  "tag_delay=1\n"
                  "; field of view: the game's projection distance while head tracking is active.\n"
                  "; 0 = the game's own (320 = 53 deg). 240 = 67 deg, 200 = 77 deg, 160 = 90 deg, 120 = 106 deg.\n"
                  "fov_screen=120\n"
                  "; crop to the PS1 picture: auto (centred 4:3, 224 of 240 lines), detect, off, or x,y,w,h in pixels\n"
                  "crop=auto\n"
                  "; 1 = head tracking also in in-engine cutscenes (look around inside the director's shot)\n"
                  "cutscene_head_look=1\n"
                  "; how far away the world-locked picture is drawn (metres; only affects stereo comfort later)\n"
                  "tracked_distance_m=3.0\n", f);
            fclose(f);
        }
    }
    s.tag_delay = GetPrivateProfileIntW(L"phase3", L"tag_delay", 1, ini.c_str());
    if (s.tag_delay < 0) s.tag_delay = 0; if (s.tag_delay > 6) s.tag_delay = 6;
    if (GetPrivateProfileIntW(L"phase3", L"fov_default_v3", 0, ini.c_str()) == 0) {
        // older builds wrote fov_screen=0 (the game's 53 deg); move to the GOG default once, keeping any other choice
        if (GetPrivateProfileIntW(L"phase3", L"fov_screen", 0, ini.c_str()) == 0)
            WritePrivateProfileStringW(L"phase3", L"fov_screen", L"120", ini.c_str());
        WritePrivateProfileStringW(L"phase3", L"fov_default_v3", L"1", ini.c_str());
    }
    s.fov_screen = GetPrivateProfileIntW(L"phase3", L"fov_screen", 120, ini.c_str());
    if (s.fov_screen != 0 && (s.fov_screen < 60 || s.fov_screen > 2000)) s.fov_screen = 0;
    { wchar_t cb[64] = {0}; GetPrivateProfileStringW(L"phase3", L"crop", L"auto", cb, 64, ini.c_str());
      int x, y, w, h;
      if (swscanf(cb, L"%d,%d,%d,%d", &x, &y, &w, &h) == 4 && w > 16 && h > 16) { s.crop_auto = 0; s.crop_x = x; s.crop_y = y; s.crop_w = w; s.crop_h = h; }
      else s.crop_auto = _wcsicmp(cb, L"off") == 0 ? -1 : _wcsicmp(cb, L"detect") == 0 ? 1 : 2; }
    s.cutscene_head_look = GetPrivateProfileIntW(L"phase3", L"cutscene_head_look", 1, ini.c_str());
    {
        wchar_t pr[8] = {0}; GetPrivateProfileStringW(L"screen", L"codec", L"", pr, 8, ini.c_str());
        if (!pr[0]) {
            FILE* f = _wfopen(ini.c_str(), L"a");
            if (f) {
                fputs("\n; ---- Floating screen and wrist display, re-read every 2 seconds ----\n"
                      "[screen]\n"
                      "; 1 = show it on a floating screen in front of you (placed where you look when it opens);\n"
                      "; 0 = keep the normal VR view while it is up. codec also covers the game's movies and game over.\n"
                      "codec=1\n"
                      "pause=1\n"
                      "menus=1\n", f);
                fclose(f);
            }
        }
        s.screen_codec = GetPrivateProfileIntW(L"screen", L"codec", 1, ini.c_str());
        s.screen_pause = GetPrivateProfileIntW(L"screen", L"pause", 1, ini.c_str());
        s.screen_menus = GetPrivateProfileIntW(L"screen", L"menus", 1, ini.c_str());
        GetPrivateProfileStringW(L"wrist_hud", L"enabled", L"", pr, 8, ini.c_str());
        if (!pr[0]) {
            FILE* f = _wfopen(ini.c_str(), L"a");
            if (f) {
                fputs(
                      "\n; ---- Wrist panels, re-read every 2 seconds ----\n[wrist_hud]\n"
                      "; As in the Integral mod: LIFE, the radar and the equipped item on your LEFT wrist, the equipped weapon\n"
                      "; (with ammo) on your RIGHT wrist. A panel shows when you look toward it.\n"
                      "enabled=1\n"
                      "; panel width in millimetres\n"
                      "width_mm=90\n"
                      "; where each panel sits from the controller grip, cm: x (+ right), y (+ up, over the back of the hand),\n"
                      "; z (+ back toward the elbow); and its tilt in degrees: pitch, yaw, roll (pitch -60 = facing up and back)\n"
                      "left_offset_cm=-1.0,4.0,10.0\n"
                      "right_offset_cm=1.0,4.0,10.0\n"
                      "left_rotation_deg=-60,0,0\n"
                      "right_rotation_deg=-60,0,0\n"
                      "; show a panel only when it is within this many degrees of where you look (0 = always)\n"
                      "gaze_cone_deg=28\n"
                      "; crop rectangles as fractions of the game picture (4:3, 320x240): x, y, width, height\n"
                      "life=0.031,0.025,0.450,0.150\n"
                      "radar=0.722,0.050,0.244,0.258\n"
                      "item_box=0.030,0.755,0.180,0.140\n"
                      "weapon_box=0.780,0.755,0.180,0.140\n", f);
                fclose(f);
            }
        }
        GetPrivateProfileStringW(L"motion", L"gun_in_hand", L"", pr, 8, ini.c_str());
        if (!pr[0]) {
            FILE* f = _wfopen(ini.c_str(), L"a");
            if (f) {
                fputs(
                      "\n; ---- Motion controls, re-read every 2 seconds ----\n[motion]\n"
                      "; 1 = the game's own SOCOM / FAMAS is held in your RIGHT hand: it is drawn at the controller and its\n"
                      "; bullets leave its muzzle, so you aim by pointing. Fire with the right trigger as before.\n"
                      "gun_in_hand=1\n"
                      "; how far ahead (ms) the hand is predicted, so the gun keeps up with a moving hand (0 = off)\n"
                      "gun_predict_ms=40\n"
                      "; extra rotation of the gun in your hand, degrees: pitch (+ muzzle up), yaw (+ muzzle left), roll\n"
                      "gun_rotation_deg=0,0,0\n"
                      "; where the grip sits from the controller, mm: x (+ right), y (+ up), z (+ back toward you)\n"
                      "gun_offset_mm=0,0,0\n"
                      "; the gun never goes further than this from your head (mm), so a tracking glitch can't throw it\n"
                      "max_reach_mm=1100\n", f);
                fclose(f);
            }
        }
        GetPrivateProfileStringW(L"melee", L"punch", L"", pr, 8, ini.c_str());
        if (!pr[0]) {
            FILE* f = _wfopen(ini.c_str(), L"a");
            if (f) {
                fputs(
                      "\n; ---- Melee by motion, re-read every 2 seconds ----\n[melee]\n"
                      "; PUNCH: throw either hand forward, fast. Each punch presses Circle once -- the game's own punch, so keep\n"
                      "; punching for its punch-punch-kick combo. A hand whose trigger is held never punches.\n"
                      "punch=1\n"
                      "; how fast (cm/s, along where you face) and how far in front of your head (cm) a hand must be\n"
                      "punch_speed_cms=200\n"
                      "punch_reach_cm=18\n"
                      "; CHOKEHOLD (no weapon equipped): bring both hands together in front of your chest and hold them still.\n"
                      "; That holds Square, the game's grab; a sharp jerk of either hand presses it again (the squeeze).\n"
                      "chokehold=1\n"
                      "choke_hands_apart_cm=30\n"
                      "choke_hold_ms=350\n"
                      "choke_jerk_cms=250\n"
                      "; controller rumble when a punch or grab registers and when the gun in your hand fires\n"
                      "rumble=1\n", f);
                fclose(f);
            }
        }
        s.melee_punch = GetPrivateProfileIntW(L"melee", L"punch", 1, ini.c_str());
        s.punch_speed_cms = GetPrivateProfileIntW(L"melee", L"punch_speed_cms", 200, ini.c_str());
        if (s.punch_speed_cms < 80 || s.punch_speed_cms > 800) s.punch_speed_cms = 200;
        s.punch_reach_cm = GetPrivateProfileIntW(L"melee", L"punch_reach_cm", 18, ini.c_str());
        if (s.punch_reach_cm < 0 || s.punch_reach_cm > 80) s.punch_reach_cm = 18;
        s.melee_choke = GetPrivateProfileIntW(L"melee", L"chokehold", 1, ini.c_str());
        s.choke_apart_cm = GetPrivateProfileIntW(L"melee", L"choke_hands_apart_cm", 30, ini.c_str());
        s.choke_hold_ms = GetPrivateProfileIntW(L"melee", L"choke_hold_ms", 350, ini.c_str());
        s.choke_jerk_cms = GetPrivateProfileIntW(L"melee", L"choke_jerk_cms", 250, ini.c_str());
        s.rumble = GetPrivateProfileIntW(L"melee", L"rumble", 1, ini.c_str());
        s.gun_in_hand = GetPrivateProfileIntW(L"motion", L"gun_in_hand", 1, ini.c_str());
        s.hide_body = GetPrivateProfileIntW(L"walk", L"hide_body", 1, ini.c_str());
        {   // v10: new keys go into their existing sections (a second [section] header would never be read)
            struct { const wchar_t* sec; const wchar_t* key; const wchar_t* val; } NK[] = {
                {L"walk", L"lean", L"1"}, {L"walk", L"lean_max_cm", L"100"}, {L"motion", L"throw_from_hand", L"1"},
                {L"walk", L"fp_overlays", L"1"},
                {L"hands", L"enabled", L"1"}, {L"hands", L"right_part", L"4"}, {L"hands", L"left_part", L"9"},
                {L"hands", L"left_arm_parts", L"7,8"}, {L"hands", L"keep_parts", L"-1"}, {L"hands", L"wall_collision", L"1"},
                {L"hands", L"knock", L"1"}, {L"hands", L"knock_speed_cm_s", L"60"}, {L"hands", L"knock_cooldown_ms", L"300"},
                {L"hands", L"wall_margin", L"30"},
                {L"scope", L"psg1_lens", L"1"}, {L"scope", L"display_deg", L"48"}, {L"scope", L"pitch_sign", L"1"},
                {L"scope", L"aim_by_head", L"1"},
                {L"wrist_hud", L"show_boss_life", L"1"}, {L"wrist_hud", L"item_use_button", L"1"},
                {L"wrist_hud", L"left_mount", L"0"}, {L"wrist_hud", L"right_mount", L"0"}, {L"wrist_hud", L"list_mount", L"2"},
                {L"wrist_hud", L"inside_left_offset_cm", L"3.0,1.0,7.0"}, {L"wrist_hud", L"inside_right_offset_cm", L"-3.0,1.0,7.0"},
                {L"wrist_hud", L"inside_left_rotation_deg", L"0,0,0"}, {L"wrist_hud", L"inside_right_rotation_deg", L"0,0,0"},
                {L"wrist_hud", L"back_left_offset_cm", L"-3.0,1.0,7.0"}, {L"wrist_hud", L"back_right_offset_cm", L"3.0,1.0,7.0"},
                {L"wrist_hud", L"back_left_rotation_deg", L"45,0,0"}, {L"wrist_hud", L"back_right_rotation_deg", L"45,0,0"},
                {L"wrist_hud", L"hide_boxes_in_view", L"1"}, {L"wrist_hud", L"hide_life_in_view", L"1"},
                {L"wrist_hud", L"hide_radar_in_view", L"1"}, {L"wrist_hud", L"hide_style", L"1"},
                {L"wrist_hud", L"show_game_boxes", L"1"}, {L"wrist_hud", L"show_radar", L"1"},
                {L"wrist_hud", L"life_bar_follows_max", L"1"}, {L"wrist_hud", L"life_bar_scale_percent", L"125"},
                {L"wrist_hud", L"select_on_wrist", L"1"} };
            int added = 0;
            for (auto& k : NK) { wchar_t t[8] = {0}; GetPrivateProfileStringW(k.sec, k.key, L"", t, 8, ini.c_str());
                                 if (!t[0]) { WritePrivateProfileStringW(k.sec, k.key, k.val, ini.c_str()); ++added; } }
            if (added && log_it) logf("INI: added %d v10 settings (lean in [walk], throw_from_hand in [motion], GOG wrist panel keys in [wrist_hud])", added);
        }
        s.lean = GetPrivateProfileIntW(L"walk", L"lean", 1, ini.c_str());
        s.lean_max_cm = GetPrivateProfileIntW(L"walk", L"lean_max_cm", 100, ini.c_str());
        if (s.lean_max_cm < 5 || s.lean_max_cm > 300) s.lean_max_cm = 100;
        s.throw_from_hand = GetPrivateProfileIntW(L"motion", L"throw_from_hand", 1, ini.c_str());
        s.fp_overlays = GetPrivateProfileIntW(L"walk", L"fp_overlays", 1, ini.c_str());
        s.hands = GetPrivateProfileIntW(L"hands", L"enabled", 1, ini.c_str());
        s.hand_r = GetPrivateProfileIntW(L"hands", L"right_part", 4, ini.c_str());
        s.hand_l = GetPrivateProfileIntW(L"hands", L"left_part", 9, ini.c_str());
        s.hands_collision = GetPrivateProfileIntW(L"hands", L"wall_collision", 1, ini.c_str());
        s.hands_knock = GetPrivateProfileIntW(L"hands", L"knock", 1, ini.c_str());
        s.knock_speed_cms = GetPrivateProfileIntW(L"hands", L"knock_speed_cm_s", 60, ini.c_str());
        s.knock_cooldown_ms = GetPrivateProfileIntW(L"hands", L"knock_cooldown_ms", 300, ini.c_str());
        s.hands_margin = GetPrivateProfileIntW(L"hands", L"wall_margin", 30, ini.c_str());
        {   wchar_t pb[96] = {0}; const wchar_t* pk[2] = {L"left_arm_parts", L"keep_parts"}; int* pd[2] = {s.hand_left_parts, s.hand_keep_parts};
            for (int q = 0; q < 2; ++q) {
                GetPrivateProfileStringW(L"hands", pk[q], q ? L"-1" : L"7,8", pb, 96, ini.c_str());
                for (int k = 0; k < 8; ++k) pd[q][k] = -1;
                int k = 0; for (wchar_t* c = pb; *c && k < 8; ) { wchar_t* e2 = nullptr; long v = wcstol(c, &e2, 10); if (e2 == c) break;
                                                                 pd[q][k++] = (int)v; c = e2; while (*c == L',' || *c == L' ') ++c; } } }
        if (s.hand_r < 0 || s.hand_r > 31) s.hand_r = 4; if (s.hand_l < 0 || s.hand_l > 31) s.hand_l = 9;
        s.scope_lens = GetPrivateProfileIntW(L"scope", L"psg1_lens", 1, ini.c_str());
        s.scope_display_deg = GetPrivateProfileIntW(L"scope", L"display_deg", 48, ini.c_str());
        if (s.scope_display_deg < 8 || s.scope_display_deg > 100) s.scope_display_deg = 48;
        s.scope_pitch_sign = GetPrivateProfileIntW(L"scope", L"pitch_sign", 1, ini.c_str()) < 0 ? -1 : 1;
        s.aim_by_head = GetPrivateProfileIntW(L"scope", L"aim_by_head", 1, ini.c_str());
        s.boss_bars = GetPrivateProfileIntW(L"wrist_hud", L"show_boss_life", 1, ini.c_str());
        s.item_use_button = GetPrivateProfileIntW(L"wrist_hud", L"item_use_button", 1, ini.c_str());
        {   // v15: new keys into their sections (written once, documented in the ini)
            struct { const wchar_t* sec; const wchar_t* key; const wchar_t* val; } NK[] = {
                {L"diorama", L"enabled", L"0"}, {L"diorama", L"scale", L"20"}, {L"diorama", L"table_distance_cm", L"60"},
                {L"diorama", L"table_drop_cm", L"50"}, {L"diorama", L"follow_radius", L"1500"}, {L"diorama", L"follow_speed", L"30"},
                {L"diorama", L"cut_units", L"4000"}, {L"diorama", L"reorient_on_area_change", L"1"}, {L"diorama", L"snake_eye_height", L"1500"},
                {L"diorama", L"turn_step_deg", L"30"}, {L"walk", L"third_person_lean", L"1"}, {L"setpiece", L"snake_eyes", L"1"},
                {L"wrist_hud", L"selector", L"1"}, {L"wrist_hud", L"wheel_size_cm", L"30"}, {L"wrist_hud", L"wheel_height_cm", L"12"},
                {L"wrist_hud", L"wheel_stereo_deg", L"4"} };
            int added = 0;
            for (auto& k : NK) { wchar_t t[8] = {0}; GetPrivateProfileStringW(k.sec, k.key, L"", t, 8, ini.c_str());
                                 if (!t[0]) { WritePrivateProfileStringW(k.sec, k.key, k.val, ini.c_str()); ++added; } }
            if (added && log_it) logf("INI: added %d v15 settings ([diorama], [setpiece], [walk] third_person_lean, [wrist_hud] selector / wheel_*)", added);
            auto I = [&](const wchar_t* sec, const wchar_t* key, int def, int lo, int hi) {
                int v = GetPrivateProfileIntW(sec, key, def, ini.c_str()); return v < lo || v > hi ? def : v; };
            s.v15.diorama = I(L"diorama", L"enabled", 0, 0, 1);
            s.v15.dio_scale = I(L"diorama", L"scale", 20, 5, 60);
            s.v15.dio_dist_cm = I(L"diorama", L"table_distance_cm", 60, 10, 200);
            s.v15.dio_drop_cm = I(L"diorama", L"table_drop_cm", 50, -50, 150);
            s.v15.dio_follow_radius = I(L"diorama", L"follow_radius", 1500, 0, 20000);
            s.v15.dio_follow_speed = I(L"diorama", L"follow_speed", 30, 1, 200);
            s.v15.dio_cut_units = I(L"diorama", L"cut_units", 4000, 500, 30000);
            s.v15.dio_reorient = I(L"diorama", L"reorient_on_area_change", 1, 0, 1);
            s.v15.dio_eye_height = I(L"diorama", L"snake_eye_height", 1500, 0, 4000);
            s.v15.dio_turn_deg = I(L"diorama", L"turn_step_deg", 30, 5, 90);
            s.v15.tp_lean = I(L"walk", L"third_person_lean", 1, 0, 1);
            s.v15.setpiece_eyes = I(L"setpiece", L"snake_eyes", 1, 0, 1);
            {   // v16: GOG's default is the 3D carousel; v15 wrote selector=1 -> moved to 2 once
                wchar_t t[8] = {0}; GetPrivateProfileStringW(L"wrist_hud", L"selector_v16", L"", t, 8, ini.c_str());
                if (!t[0]) { WritePrivateProfileStringW(L"wrist_hud", L"selector", L"2", ini.c_str());
                             WritePrivateProfileStringW(L"wrist_hud", L"selector_v16", L"1", ini.c_str());
                             wchar_t t2[8] = {0}; GetPrivateProfileStringW(L"wrist_hud", L"carousel_size_cm", L"", t2, 8, ini.c_str());
                             if (!t2[0]) WritePrivateProfileStringW(L"wrist_hud", L"carousel_size_cm", L"13", ini.c_str());
                             GetPrivateProfileStringW(L"wrist_hud", L"pause_while_selecting", L"", t2, 8, ini.c_str());
                             if (!t2[0]) WritePrivateProfileStringW(L"wrist_hud", L"pause_while_selecting", L"1", ini.c_str());
                             if (log_it) logf("INI v16: [wrist_hud] selector=2 (the GOG 3D carousel), carousel_size_cm, pause_while_selecting"); }
            }
            s.v15.wheel = I(L"wrist_hud", L"selector", 2, 0, 2);   // GOG: 0 list, 1 2D wheel, 2 3D carousel
            s.v15.carousel_size_cm = I(L"wrist_hud", L"carousel_size_cm", 13, 6, 30);
            s.v15.pause_sel = I(L"wrist_hud", L"pause_while_selecting", 1, 0, 1);
            {   // v17
                struct { const wchar_t* sec; const wchar_t* key; const wchar_t* val; } NK2[] = {
                    {L"subtitles", L"enabled", L"1"}, {L"subtitles", L"strip", L"0.0,0.7667,1.0,0.1667"}, {L"subtitles", L"panel_width_cm", L"80"},
                    {L"subtitles", L"panel_distance_cm", L"110"}, {L"subtitles", L"panel_down_deg", L"20"},
                    {L"controls", L"mc_menu_both_sticks", L"1"}, {L"controls", L"r3_double_view", L"1"} };
                int added2 = 0;
                for (auto& k : NK2) { wchar_t t[8] = {0}; GetPrivateProfileStringW(k.sec, k.key, L"", t, 8, ini.c_str());
                                      if (!t[0]) { WritePrivateProfileStringW(k.sec, k.key, k.val, ini.c_str()); ++added2; } }
                if (added2 && log_it) logf("INI: added %d v17 settings ([subtitles], [controls] mc_menu_both_sticks / r3_double_view)", added2);
                s.v15.sub_on = I(L"subtitles", L"enabled", 1, 0, 1);
                s.v15.sub_w_cm = I(L"subtitles", L"panel_width_cm", 80, 20, 300);
                s.v15.sub_dist_cm = I(L"subtitles", L"panel_distance_cm", 110, 30, 400);
                s.v15.sub_down_deg = I(L"subtitles", L"panel_down_deg", 20, -30, 60);
                wchar_t sb[96] = {0}; GetPrivateProfileStringW(L"subtitles", L"strip", L"0.0,0.7667,1.0,0.1667", sb, 96, ini.c_str());
                double f[4] = {0, 0.7667, 1.0, 0.1667}; int k = 0;
                for (wchar_t* c = sb; *c && k < 4; ) { wchar_t* e2 = nullptr; double v = wcstod(c, &e2); if (e2 == c) break; f[k++] = v; c = e2; while (*c == L',' || *c == L' ') ++c; }
                for (int i = 0; i < 4; ++i) { if (f[i] < 0) f[i] = 0; if (f[i] > 1) f[i] = 1; s.v15.sub_strip[i] = (int)lround(f[i] * 10000); }
                s.v15.mc_menu = I(L"controls", L"mc_menu_both_sticks", 1, 0, 1);
                s.v15.r3_view = I(L"controls", L"r3_double_view", 1, 0, 1);
                { wchar_t t[8] = {0}; GetPrivateProfileStringW(L"controls", L"codec_ear_gesture", L"", t, 8, ini.c_str());
                  if (!t[0]) { WritePrivateProfileStringW(L"controls", L"codec_ear_gesture", L"1", ini.c_str());
                               WritePrivateProfileStringW(L"controls", L"codec_ear_cm", L"17", ini.c_str()); } }
                s.v15.codec_ear = I(L"controls", L"codec_ear_gesture", 1, 0, 1);
                s.v15.codec_ear_cm = I(L"controls", L"codec_ear_cm", 17, 5, 40);
                { wchar_t t[8] = {0}; GetPrivateProfileStringW(L"cutscene", L"blur_effect", L"", t, 8, ini.c_str());
                  if (!t[0]) WritePrivateProfileStringW(L"cutscene", L"blur_effect", L"0", ini.c_str()); }
                s.v15.blur_effect = I(L"cutscene", L"blur_effect", 0, 0, 1);
                { wchar_t t[8] = {0}; GetPrivateProfileStringW(L"controls", L"xy_start", L"", t, 8, ini.c_str());
                  if (!t[0]) WritePrivateProfileStringW(L"controls", L"xy_start", L"1", ini.c_str()); }
                s.v15.xy_start = I(L"controls", L"xy_start", 1, 0, 1);
                { wchar_t t[8] = {0}; GetPrivateProfileStringW(L"cutscene", L"stereo", L"", t, 8, ini.c_str());
                  if (!t[0]) WritePrivateProfileStringW(L"cutscene", L"stereo", L"0", ini.c_str()); }
                s.v15.cut_stereo = I(L"cutscene", L"stereo", 0, 0, 1);   // v18
            }
            s.v15.wheel_size_cm = I(L"wrist_hud", L"wheel_size_cm", 30, 15, 60);
            s.v15.wheel_height_cm = I(L"wrist_hud", L"wheel_height_cm", 12, 0, 40);
            {   // v19: per-eye model turn off by default (read as doubling); moved to 0 once
                wchar_t t[8] = {0}; GetPrivateProfileStringW(L"wrist_hud", L"wheel_stereo_v19", L"", t, 8, ini.c_str());
                if (!t[0]) { WritePrivateProfileStringW(L"wrist_hud", L"wheel_stereo_deg", L"0", ini.c_str());
                             WritePrivateProfileStringW(L"wrist_hud", L"wheel_stereo_v19", L"1", ini.c_str()); }
                GetPrivateProfileStringW(L"stereo", L"hold_hand_cm_s", L"", t, 8, ini.c_str());
                if (!t[0]) WritePrivateProfileStringW(L"stereo", L"hold_hand_cm_s", L"25", ini.c_str());
            }
            s.v15.wheel_stereo_deg = I(L"wrist_hud", L"wheel_stereo_deg", 0, 0, 15);
            s.v15.hold_hand_cms = I(L"stereo", L"hold_hand_cm_s", 25, 0, 500);
            { wchar_t t[8] = {0}; GetPrivateProfileStringW(L"stereo", L"true_stereo", L"", t, 8, ini.c_str());
              if (!t[0]) WritePrivateProfileStringW(L"stereo", L"true_stereo", L"0", ini.c_str()); }
            s.v15.true_stereo = I(L"stereo", L"true_stereo", 0, 0, 1);   // v20
        }
        s.hud_mount_side[0] = GetPrivateProfileIntW(L"wrist_hud", L"left_mount", 0, ini.c_str());
        s.hud_mount_side[1] = GetPrivateProfileIntW(L"wrist_hud", L"right_mount", 0, ini.c_str());
        s.hud_list_mount = GetPrivateProfileIntW(L"wrist_hud", L"list_mount", 2, ini.c_str());
        s.hud_hide_boxes = GetPrivateProfileIntW(L"wrist_hud", L"hide_boxes_in_view", 1, ini.c_str());
        s.hud_hide_life = GetPrivateProfileIntW(L"wrist_hud", L"hide_life_in_view", 1, ini.c_str());
        s.hud_hide_radar = GetPrivateProfileIntW(L"wrist_hud", L"hide_radar_in_view", 1, ini.c_str());
        s.hud_hide_style = GetPrivateProfileIntW(L"wrist_hud", L"hide_style", 1, ini.c_str()) != 0 ? 1 : 0;
        s.hud_show_boxes = GetPrivateProfileIntW(L"wrist_hud", L"show_game_boxes", 1, ini.c_str());
        s.hud_show_radar = GetPrivateProfileIntW(L"wrist_hud", L"show_radar", 1, ini.c_str());
        s.hud_bar_follows_max = GetPrivateProfileIntW(L"wrist_hud", L"life_bar_follows_max", 1, ini.c_str());
        s.hud_bar_scale_pct = GetPrivateProfileIntW(L"wrist_hud", L"life_bar_scale_percent", 125, ini.c_str());
        if (s.hud_bar_scale_pct < 25 || s.hud_bar_scale_pct > 400) s.hud_bar_scale_pct = 125;
        s.hud_select_on_wrist = GetPrivateProfileIntW(L"wrist_hud", L"select_on_wrist", 1, ini.c_str());
        {
            const wchar_t* mk[8] = {L"inside_left_offset_cm", L"inside_right_offset_cm", L"inside_left_rotation_deg", L"inside_right_rotation_deg",
                                    L"back_left_offset_cm", L"back_right_offset_cm", L"back_left_rotation_deg", L"back_right_rotation_deg"};
            double* md[8] = {s.hud_in_off_cm[0], s.hud_in_off_cm[1], s.hud_in_rot_deg[0], s.hud_in_rot_deg[1],
                             s.hud_back_off_cm[0], s.hud_back_off_cm[1], s.hud_back_rot_deg[0], s.hud_back_rot_deg[1]};
            for (int k = 0; k < 8; ++k) { wchar_t mb[96] = {0}; double a0, a1, a2;
                GetPrivateProfileStringW(L"wrist_hud", mk[k], L"", mb, 96, ini.c_str());
                if (swscanf(mb, L"%lf,%lf,%lf", &a0, &a1, &a2) == 3) { md[k][0] = a0; md[k][1] = a1; md[k][2] = a2; } }
        }
        { wchar_t rb[96] = {0}; double a0, a1, a2, a3;
          GetPrivateProfileStringW(L"wrist_hud", L"radar_ps1", L"232,13,76,58", rb, 96, ini.c_str());
          if (swscanf(rb, L"%lf,%lf,%lf,%lf", &a0, &a1, &a2, &a3) == 4 && a2 > 4 && a3 > 4) { s.radar_ps1[0] = a0; s.radar_ps1[1] = a1; s.radar_ps1[2] = a2; s.radar_ps1[3] = a3; } }
        {
            const wchar_t* sk[2] = {L"item_selection", L"weapon_selection"};
            const wchar_t* sd[2] = {L"0.030,0.420,0.530,0.480", L"0.430,0.420,0.540,0.480"};
            for (int k = 0; k < 2; ++k) {
                wchar_t sb[96] = {0}; double a0, a1, a2, a3;
                GetPrivateProfileStringW(L"wrist_hud", sk[k], sd[k], sb, 96, ini.c_str());
                if (swscanf(sb, L"%lf,%lf,%lf,%lf", &a0, &a1, &a2, &a3) == 4 && a2 > 0.01 && a3 > 0.01 && a0 >= 0 && a1 >= 0 && a0 + a2 <= 1.01 && a1 + a3 <= 1.01) {
                    s.hud_sel[k][0] = a0; s.hud_sel[k][1] = a1; s.hud_sel[k][2] = a2; s.hud_sel[k][3] = a3; }
            }
        }
        s.perf_counters = GetPrivateProfileIntW(L"motion", L"perf_counters", 0, ini.c_str());
        s.gun_predict_ms = GetPrivateProfileIntW(L"motion", L"gun_predict_ms", 40, ini.c_str());
        if (s.gun_predict_ms < 0 || s.gun_predict_ms > 150) s.gun_predict_ms = 40;
        s.gun_max_reach_mm = GetPrivateProfileIntW(L"motion", L"max_reach_mm", 1100, ini.c_str());
        if (s.gun_max_reach_mm < 300 || s.gun_max_reach_mm > 3000) s.gun_max_reach_mm = 1100;
        {
            wchar_t gb[96] = {0}; double g0, g1, g2;
            GetPrivateProfileStringW(L"motion", L"gun_rotation_deg", L"0,0,0", gb, 96, ini.c_str());
            if (swscanf(gb, L"%lf,%lf,%lf", &g0, &g1, &g2) == 3) { s.gun_rot_deg[0] = g0; s.gun_rot_deg[1] = g1; s.gun_rot_deg[2] = g2; }
            GetPrivateProfileStringW(L"motion", L"gun_offset_mm", L"0,0,0", gb, 96, ini.c_str());
            if (swscanf(gb, L"%lf,%lf,%lf", &g0, &g1, &g2) == 3) { s.gun_off_mm[0] = g0; s.gun_off_mm[1] = g1; s.gun_off_mm[2] = g2; }
        }
        s.hud_wrist = GetPrivateProfileIntW(L"wrist_hud", L"enabled", 1, ini.c_str());
        s.hud_width_mm = GetPrivateProfileIntW(L"wrist_hud", L"width_mm", 90, ini.c_str());
        if (s.hud_width_mm < 30 || s.hud_width_mm > 400) s.hud_width_mm = 90;
        s.hud_gaze_deg = GetPrivateProfileIntW(L"wrist_hud", L"gaze_cone_deg", 28, ini.c_str());
        wchar_t b[96] = {0}; double d0, d1, d2, d3;
        const wchar_t* vk[4] = {L"left_offset_cm", L"right_offset_cm", L"left_rotation_deg", L"right_rotation_deg"};
        const wchar_t* vd[4] = {L"-1.0,4.0,10.0", L"1.0,4.0,10.0", L"-60,0,0", L"-60,0,0"};
        for (int k = 0; k < 4; ++k) {
            GetPrivateProfileStringW(L"wrist_hud", vk[k], vd[k], b, 96, ini.c_str());
            if (swscanf(b, L"%lf,%lf,%lf", &d0, &d1, &d2) == 3) {
                double* dst = k < 2 ? s.hud_off_cm[k] : s.hud_rot_deg[k - 2];
                dst[0] = d0; dst[1] = d1; dst[2] = d2;
            }
        }
        const wchar_t* rk[4] = {L"life", L"radar", L"item_box", L"weapon_box"};
        const wchar_t* rd[4] = {L"0.031,0.025,0.450,0.150", L"0.722,0.050,0.244,0.258", L"0.030,0.755,0.180,0.140", L"0.780,0.755,0.180,0.140"};
        for (int k = 0; k < 4; ++k) {
            GetPrivateProfileStringW(L"wrist_hud", rk[k], rd[k], b, 96, ini.c_str());
            if (swscanf(b, L"%lf,%lf,%lf,%lf", &d0, &d1, &d2, &d3) == 4 && d2 > 0.01 && d3 > 0.01 && d0 >= 0 && d1 >= 0 && d0 + d2 <= 1.01 && d1 + d3 <= 1.01) {
                s.hud_rect[k][0] = d0; s.hud_rect[k][1] = d1; s.hud_rect[k][2] = d2; s.hud_rect[k][3] = d3; }
        }
    }
    {
        wchar_t pr[8] = {0}; GetPrivateProfileStringW(L"cutscene", L"snake_eyes", L"", pr, 8, ini.c_str());
        if (!pr[0]) {
            FILE* f = _wfopen(ini.c_str(), L"a");
            if (f) {
                fputs("\n; ---- Cutscenes, re-read every 2 seconds ----\n"
                      "[cutscene]\n"
                      "; 1 = watch cutscenes through Snake's eyes (his head is the camera; your head looks around). Shots where\n"
                      "; Snake is not on set fall back to the director's camera with head-look. 0 = director's camera only.\n"
                      "snake_eyes=1\n"
                      "; eye position in front of the head joint (game units, about 1 unit = 1 mm)\n"
                      "eye_forward=100\n"
                      "; Snake's cutscene models (GV_StrCode hashes, hex): snake, sne_nude. The log's CUTSCENE lines list each cast.\n"
                      "snake_models=992D,34C2\n", f);
                fclose(f);
            }
        }
        s.cut_snake_eyes = GetPrivateProfileIntW(L"cutscene", L"snake_eyes", 1, ini.c_str());
        s.cut_eye_forward = GetPrivateProfileIntW(L"cutscene", L"eye_forward", 100, ini.c_str());
        if (s.cut_eye_forward < -500 || s.cut_eye_forward > 1000) s.cut_eye_forward = 100;
        wchar_t ml[128] = {0}; GetPrivateProfileStringW(L"cutscene", L"snake_models", L"992D,34C2", ml, 128, ini.c_str());
        s.cut_n_models = 0;
        for (wchar_t* q = ml; *q && s.cut_n_models < 8; ) {
            wchar_t* e = nullptr; unsigned long v = wcstoul(q, &e, 16);
            if (e == q) { ++q; continue; }
            s.cut_models[s.cut_n_models++] = (uint16_t)v; q = e;
        }
    }
    GetPrivateProfileStringW(L"walk", L"walk_mode", L"", probe, 8, ini.c_str());
    if (!probe[0]) {
        FILE* f = _wfopen(ini.c_str(), L"a");
        if (f) {
            fputs("\n; ---- Walking in first person (Snake's eyes), re-read every 2 seconds ----\n"
                  "[walk]\n"
                  "; 1 = in normal (third-person) gameplay, see through Snake's eyes and walk where you look. F7 toggles.\n"
                  "walk_mode=1\n"
                  "; eye position relative to Snake's head (game units; about 1 unit = 1 mm)\n"
                  "eye_forward=120\n"
                  "eye_up=0\n"
                  "; right stick turning: degrees per second at full tilt, and dead zone (0-100)\n"
                  "turn_speed_deg=120\n"
                  "stick_deadzone=24\n", f);
            fclose(f);
        }
    }
    s.walk_mode = GetPrivateProfileIntW(L"walk", L"walk_mode", 1, ini.c_str());
    s.eye_forward = GetPrivateProfileIntW(L"walk", L"eye_forward", 120, ini.c_str());
    s.eye_up = GetPrivateProfileIntW(L"walk", L"eye_up", 0, ini.c_str());
    s.stick_deadzone = GetPrivateProfileIntW(L"walk", L"stick_deadzone", 24, ini.c_str());
    { wchar_t tb[32] = {0}; GetPrivateProfileStringW(L"walk", L"turn_speed_deg", L"120", tb, 32, ini.c_str()); s.turn_speed_deg = _wtof(tb); }
    if (s.eye_forward < -500 || s.eye_forward > 1000) s.eye_forward = 120;
    if (s.eye_up < -1000 || s.eye_up > 1000) s.eye_up = 0;
    if (s.stick_deadzone < 0 || s.stick_deadzone > 100) s.stick_deadzone = 24;
    if (s.turn_speed_deg < 0 || s.turn_speed_deg > 720) s.turn_speed_deg = 120;
    GetPrivateProfileStringW(L"walk", L"turn_mode", L"", probe, 8, ini.c_str());
    if (!probe[0]) {   // add the new keys inside the existing [walk] section (a second [walk] header would be ignored)
        WritePrivateProfileStringW(L"walk", L"turn_mode", L"smooth", ini.c_str());
        WritePrivateProfileStringW(L"walk", L"snap_deg", L"30", ini.c_str());
        WritePrivateProfileStringW(L"walk", L"body_follows_head", L"1", ini.c_str());
        WritePrivateProfileStringW(L"walk", L"body_deadband_deg", L"30", ini.c_str());
        WritePrivateProfileStringW(L"walk", L"body_turn_speed_deg", L"360", ini.c_str());
    }
    { wchar_t tm[16] = {0}; GetPrivateProfileStringW(L"walk", L"turn_mode", L"smooth", tm, 16, ini.c_str()); s.turn_mode = _wcsicmp(tm, L"snap") == 0 ? 1 : 0; }
    s.snap_deg = GetPrivateProfileIntW(L"walk", L"snap_deg", 30, ini.c_str());
    s.body_follows_head = GetPrivateProfileIntW(L"walk", L"body_follows_head", 1, ini.c_str());
    s.body_deadband_deg = GetPrivateProfileIntW(L"walk", L"body_deadband_deg", 30, ini.c_str());
    s.body_turn_speed_deg = GetPrivateProfileIntW(L"walk", L"body_turn_speed_deg", 360, ini.c_str());
    if (s.snap_deg < 5 || s.snap_deg > 90) s.snap_deg = 30;
    if (s.body_deadband_deg < 0 || s.body_deadband_deg > 180) s.body_deadband_deg = 30;
    if (s.body_turn_speed_deg < 30 || s.body_turn_speed_deg > 3600) s.body_turn_speed_deg = 360;
    GetPrivateProfileStringW(L"stereo", L"enabled", L"", probe, 8, ini.c_str());
    if (!probe[0]) {
        FILE* f = _wfopen(ini.c_str(), L"a");
        if (f) {
            fputs("\n; ---- Stereo (real 3D depth), re-read every 2 seconds ----\n"
                  "[stereo]\n"
                  "; Alternate-eye stereo: the game camera moves half an eye-distance left on one game frame and right on\n"
                  "; the next, and each picture goes to its own eye. 1 = on, 0 = off. F5 toggles it while playing.\n"
                  "enabled=1\n"
                  "; distance between your eyes, millimetres\n"
                  "ipd_mm=64\n"
                  "; game units per real metre: sets how big the world feels (bigger number = smaller world)\n"
                  "units_per_m=1000\n"
                  "; 1 if the depth looks inside-out (near things far away)\n"
                  "swap_eyes=0\n"
                  "; both eyes show the same newest picture (no depth, no doubling) while the camera moves more than\n"
                  "; hold_units game units per game frame, the head turns faster than hold_deg_s degrees/second, or the\n"
                  "; right stick turns; stereo returns hold_ms after the movement stops. 0 disables that check.\n"
                  "hold_units=20\n"
                  "hold_deg_s=300\n"
                  "hold_ms=330\n"
                  "; an eye whose picture is older than this (ms) shows the newest picture instead\n"
                  "eye_timeout_ms=120\n"
                  "; below this game frame rate both eyes show the newest picture\n"
                  "min_fps=20\n", f);
            fclose(f);
        }
    }
    s.stereo = GetPrivateProfileIntW(L"stereo", L"enabled", 1, ini.c_str());
    s.ipd_mm = GetPrivateProfileIntW(L"stereo", L"ipd_mm", 64, ini.c_str());
    s.units_per_m = GetPrivateProfileIntW(L"stereo", L"units_per_m", 1000, ini.c_str());
    s.swap_eyes = GetPrivateProfileIntW(L"stereo", L"swap_eyes", 0, ini.c_str());
    s.hold_units = GetPrivateProfileIntW(L"stereo", L"hold_units", 20, ini.c_str());
    s.hold_deg_s = GetPrivateProfileIntW(L"stereo", L"hold_deg_s", 300, ini.c_str());
    s.hold_ms = GetPrivateProfileIntW(L"stereo", L"hold_ms", 330, ini.c_str());
    s.eye_timeout_ms = GetPrivateProfileIntW(L"stereo", L"eye_timeout_ms", 120, ini.c_str());
    s.min_fps = GetPrivateProfileIntW(L"stereo", L"min_fps", 20, ini.c_str());
    if (s.ipd_mm < 0 || s.ipd_mm > 120) s.ipd_mm = 64;
    if (s.units_per_m < 100 || s.units_per_m > 10000) s.units_per_m = 1000;
    if (s.hold_ms < 0 || s.hold_ms > 5000) s.hold_ms = 330;
    if (s.eye_timeout_ms < 20 || s.eye_timeout_ms > 1000) s.eye_timeout_ms = 120;
    GetPrivateProfileStringW(L"controls", L"enabled", L"", probe, 8, ini.c_str());
    if (!probe[0]) {
        FILE* f = _wfopen(ini.c_str(), L"a");
        if (f) {
            fputs("\n; ---- VR controllers (re-read every 2 seconds) ----\n"
                  "; Left stick moves Snake (push further to run). Right stick turns (walk mode).\n"
                  "; Each control maps to a PS1 button: cross circle square triangle l1 l2 r1 r2 select start l3 r3\n"
                  "; up down left right none. right_stick_click can also be: recentre\n"
                  "[controls]\n"
                  "enabled=1\n"
                  "a=circle\n"
                  "b=triangle\n"
                  "x=l1\n"
                  "y=r1\n"
                  "right_trigger=square\n"
                  "left_trigger=cross\n"
                  "right_grip=r2\n"
                  "left_grip=l2\n"
                  "left_stick_click=select\n"
                  "right_stick_click=recentre\n"
                  "menu=start\n", f);
            fclose(f);
        }
    }
    s.controls = GetPrivateProfileIntW(L"controls", L"enabled", 1, ini.c_str());
    if (GetPrivateProfileIntW(L"controls", L"layout", 1, ini.c_str()) < 2) {   // once: the old default layout moves the lists to the grips
        wchar_t a[16] = {0}, b2[16] = {0}, c[16] = {0}, d[16] = {0};
        GetPrivateProfileStringW(L"controls", L"x", L"l2", a, 16, ini.c_str()); GetPrivateProfileStringW(L"controls", L"y", L"r2", b2, 16, ini.c_str());
        GetPrivateProfileStringW(L"controls", L"left_grip", L"l1", c, 16, ini.c_str()); GetPrivateProfileStringW(L"controls", L"right_grip", L"r1", d, 16, ini.c_str());
        if (!_wcsicmp(a, L"l2") && !_wcsicmp(b2, L"r2") && !_wcsicmp(c, L"l1") && !_wcsicmp(d, L"r1")) {
            WritePrivateProfileStringW(L"controls", L"x", L"l1", ini.c_str()); WritePrivateProfileStringW(L"controls", L"y", L"r1", ini.c_str());
            WritePrivateProfileStringW(L"controls", L"left_grip", L"l2", ini.c_str()); WritePrivateProfileStringW(L"controls", L"right_grip", L"r2", ini.c_str());
            if (log_it) logf("INI controls: grips now open the item (left, L2) and weapon (right, R2) lists; X / Y lean (L1 / R1)");
        }
        WritePrivateProfileStringW(L"controls", L"layout", L"2", ini.c_str());
    }
    if (GetPrivateProfileIntW(L"controls", L"layout", 2, ini.c_str()) < 3) {   // v21: once -- the GOG layout, if still on the old defaults
        wchar_t a[16] = {0}, b2[16] = {0}, lt[16] = {0};
        GetPrivateProfileStringW(L"controls", L"a", L"cross", a, 16, ini.c_str()); GetPrivateProfileStringW(L"controls", L"b", L"circle", b2, 16, ini.c_str());
        GetPrivateProfileStringW(L"controls", L"left_trigger", L"triangle", lt, 16, ini.c_str());
        if (!_wcsicmp(a, L"cross") && !_wcsicmp(b2, L"circle") && !_wcsicmp(lt, L"triangle")) {
            WritePrivateProfileStringW(L"controls", L"a", L"circle", ini.c_str()); WritePrivateProfileStringW(L"controls", L"b", L"triangle", ini.c_str());
            WritePrivateProfileStringW(L"controls", L"left_trigger", L"cross", ini.c_str());
            if (log_it) logf("INI controls: GOG layout -- A = Circle (action), B = Triangle (first person), left trigger = Cross (crawl)");
        }
        WritePrivateProfileStringW(L"controls", L"layout", L"3", ini.c_str());
    }
    {
        struct { const wchar_t* key; const wchar_t* def; uint16_t* dst; } M[] = {
            {L"a", L"circle", &s.map_a}, {L"b", L"triangle", &s.map_b}, {L"x", L"l1", &s.map_x}, {L"y", L"r1", &s.map_y},
            {L"right_trigger", L"square", &s.map_rt}, {L"left_trigger", L"cross", &s.map_lt},
            {L"right_grip", L"r2", &s.map_rg}, {L"left_grip", L"l2", &s.map_lg},
            {L"left_stick_click", L"select", &s.map_lclick}, {L"menu", L"start", &s.map_menu} };
        for (auto& m : M) { wchar_t b[32] = {0}; GetPrivateProfileStringW(L"controls", m.key, m.def, b, 32, ini.c_str()); *m.dst = ps_button(b, nullptr); }
        wchar_t b[32] = {0}; GetPrivateProfileStringW(L"controls", L"right_stick_click", L"recentre", b, 32, ini.c_str());
        bool rc = false; s.map_rclick = ps_button(b, &rc); s.rclick_recentre = rc;
    }
    {   // Quest buttons in the Master Collection menus: PS1 button -> keyboard key (DirectInput codes)
        GetPrivateProfileStringW(L"menus", L"method", L"", probe, 8, ini.c_str());
        if (!probe[0]) {
            FILE* f = _wfopen(ini.c_str(), L"a");
            if (f) {
                fputs("\n[menus]\n"
                      "; Quest controllers in the Master Collection menus (launcher, settings, pause): the buttons act as\n"
                      "; keyboard keys there. method = auto | dinput | sendinput | off. Keys are DirectInput codes (hex);\n"
                      "; the log's KEYBOARD lines show the code of any key you press.\n"
                      "method=auto\n"
                      "up=0xC8\ndown=0xD0\nleft=0xCB\nright=0xCD\n"
                      "cross=0x1C\ncircle=0x01\nsquare=0x39\ntriangle=0x0E\n"
                      "l1=0x10\nr1=0x12\nl2=0xC9\nr2=0xD1\nstart=0x0F\nselect=0x00\n", f);
                fclose(f);
            }
        }
        wchar_t m[16] = {0}; GetPrivateProfileStringW(L"menus", L"method", L"auto", m, 16, ini.c_str());
        s.menu_method = !_wcsicmp(m, L"dinput") ? 1 : !_wcsicmp(m, L"sendinput") ? 2 : !_wcsicmp(m, L"off") ? 3 : 0;
        struct { const wchar_t* key; const wchar_t* def; int bit; } K[] = {
            {L"up", L"0xC8", 12}, {L"down", L"0xD0", 14}, {L"left", L"0xCB", 15}, {L"right", L"0xCD", 13},
            {L"cross", L"0x1C", 6}, {L"circle", L"0x01", 5}, {L"square", L"0x39", 7}, {L"triangle", L"0x0E", 4},
            {L"l1", L"0x10", 2}, {L"r1", L"0x12", 3}, {L"l2", L"0xC9", 0}, {L"r2", L"0xD1", 1},
            {L"start", L"0x0F", 11}, {L"select", L"0x00", 8} };
        for (auto& k : K) { wchar_t b[16] = {0}; GetPrivateProfileStringW(L"menus", k.key, k.def, b, 16, ini.c_str());
                            s.menu_key[k.bit] = (uint8_t)wcstoul(b, nullptr, 0); }
    }
    { wchar_t db[32] = {0}; GetPrivateProfileStringW(L"phase3", L"tracked_distance_m", L"3.0", db, 32, ini.c_str());
      s.tracked_distance_m = _wtof(db); if (s.tracked_distance_m < 0.5 || s.tracked_distance_m > 50) s.tracked_distance_m = 3.0; }
    if (log_it) logf("INI enabled=%d vr=%d track_third_person=%d recentre_on_first_person=%d screen %.2f m wide at %.2f m | "
                     "tag_delay=%d fov_screen=%d crop=%s(%d,%d,%d,%d) tracked_distance=%.1f m",
                     s.enabled, s.vr, s.track_third_person, s.recentre_on_first_person, s.screen_width_m, s.screen_distance_m,
                     s.tag_delay, s.fov_screen, s.crop_auto == 2 ? "auto 4:3" : s.crop_auto == 1 ? "detect" : s.crop_auto == 0 ? "manual" : "off",
                     s.crop_x, s.crop_y, s.crop_w, s.crop_h, s.tracked_distance_m);
    return s;
}
static Settings g_cfg;   // written by the main thread only; the XR thread reads the screen size (benign race)

// ---------------------------------------------------------------- snapshots
static std::vector<uint8_t> g_snap(PSX_RAM);
static int g_seq = 0;
static void dump(uint8_t* b, const char* why)
{
    if (!safe_read(b, g_snap.data(), PSX_RAM)) return;
    wchar_t name[MAX_PATH];
    swprintf(name, MAX_PATH, L"%lsmgs1mc_vr_probe\\ram_p3_%03d_%hs.bin", g_dir.c_str(), g_seq, why);
    FILE* f = _wfopen(name, L"wb");
    if (!f) { logf("snapshot %d: could not open file", g_seq); return; }
    fwrite(g_snap.data(), 1, PSX_RAM, f);
    fclose(f);
    logf("snapshot ram_p3_%03d_%s.bin saved", g_seq, why);
    ++g_seq;
}

// ================================================================= picture capture (game's Present)
typedef HRESULT (STDMETHODCALLTYPE *Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *Present1_t)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static Present_t  o_Present  = nullptr;
static Present1_t o_Present1 = nullptr;

// Picture hand-off (ctrl v2): three plain shared textures, no keyed mutex. The game side copies its backbuffer into a
// free slot and marks an EVENT query; the slot is published only once that query says the copy is finished on the GPU.
// The headset side copies the newest published slot into its own texture and holds the slot until its own copy's query
// completes. Neither GPU queue ever waits on the other (the keyed mutex put a GPU wait on the game's queue -- which
// can sit behind the game's vsync-blocked frames -- into every headset frame, and the runtime then waited for that).
static const int CAP_SLOTS = 3;
struct Capture {
    ID3D11Texture2D* tex[CAP_SLOTS] = {};   // on the game's device, MISC_SHARED
    ID3D11Query* q[CAP_SLOTS] = {};
    ID3D11Texture2D* stg[CAP_SLOTS] = {};   // small CPU copy of the marker corner of each slot
    ID3D11Texture2D* stg2[CAP_SLOTS] = {};  // v20: the same at the start of the right half (side-by-side true stereo)
    D3D11_BOX box2 = {};
    D3D11_BOX box = {}; int mark_min = 4, lead = 2; double psx = 4.5, psy = 4.5;     // where the marker sits in the picture, and the pixel count that proves it
    int pending = -1;                       // slot whose copy is still on the GPU
    UINT w = 0, h = 0; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;            // device the slots belong to (not referenced)
};
static Capture g_cap;                          // game render thread only
static std::atomic<uint32_t> g_cap_skipped{0};   // presents not copied because the previous copy was still on the GPU
static std::atomic<HANDLE>   g_cap_handles[CAP_SLOTS];
static std::atomic<int>      g_cap_ready{-1};    // newest finished slot (game thread writes)
static std::atomic<uint32_t> g_cap_ready_seq{0}; // bumps on every publish
static std::atomic<int>      g_xr_slot{-1};      // slot the headset side is reading (XR thread writes)
static std::atomic<uint32_t> g_slot_seq[CAP_SLOTS], g_slot_screen[CAP_SLOTS];
static std::atomic<int>      g_slot_tracked[CAP_SLOTS];
static std::atomic<int>      g_slot_mark[CAP_SLOTS];
static std::atomic<int>      g_slot_mseq[CAP_SLOTS];
static std::atomic<int>      g_slot_mark2[CAP_SLOTS], g_slot_mseq2[CAP_SLOTS];   // v20: right-half marker      // low 6 bits of the head-pose seq read from the pixels, -1 = none      // eye marker read from the pixels: 0 none, 1 left, 2 right, 3 mono
static std::atomic<bool>     g_test_blank{false};   // F11: headset shows a plain test colour, no game picture at all
static std::atomic<uint32_t> g_cap_gen{0}, g_cap_frames{0}, g_cap_w{0}, g_cap_h{0}, g_cap_fmt{0};

static void capture_inner(IDXGISwapChain* sc, UINT flags);
// The camera routine draws a 3x2-pixel eye colour (red left, green right, blue mono) and a magenta 3x2 next to it in the
// top-left corner of every frame it drives. Count the pixels of each colour in the corner copy; magenta proves the marker.
static std::atomic<uint32_t> g_mark_stats[5];   // none, left, right, mono, unreadable
static std::atomic<uint32_t> g_mseq_bad{0};
static int read_marker(ID3D11DeviceContext* ctx, int slot, DXGI_FORMAT fmt, int* mseq, int which = 0, bool half = false)
{
    *mseq = -1;
    ID3D11Texture2D* st = which ? g_cap.stg2[slot] : g_cap.stg[slot];
    const D3D11_BOX& bxm = which ? g_cap.box2 : g_cap.box;
    const double psx = half ? g_cap.psx / 2 : g_cap.psx;   // v20: a 640-wide picture has PS1 pixels half as wide
    if (!st) return 0;
    D3D11_MAPPED_SUBRESOURCE m;
    (void)0;
    if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) { g_mark_stats[4]++; return 0; }
    bool bgra = fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || fmt == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
                fmt == DXGI_FORMAT_B8G8R8X8_UNORM || fmt == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    bool rgba = fmt == DXGI_FORMAT_R8G8B8A8_UNORM || fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || fmt == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    int n[4] = {0, 0, 0, 0};   // magenta, red, green, blue
    int mx0 = 1 << 30, mx1 = -1, my0 = 1 << 30, my1 = -1;   // magenta bounding box
    int seqc[3] = {-1, -1, -1};
    if (bgra || rgba) {
        int w = (int)(bxm.right - bxm.left), h = (int)(bxm.bottom - bxm.top);
        int lead = g_cap.lead;                       // host pixels between the box edge and PS1 x = 0
        double eye_x1 = lead + 3 * psx + 2, mag_x0 = lead + 3 * psx - 2, mag_x1 = lead + 6 * psx + 1;
        for (int y = 0; y < h; ++y) {
            const uint8_t* row = (const uint8_t*)m.pData + y * m.RowPitch;
            for (int x = 0; x < w; ++x) {
                const uint8_t* px = row + 4 * x;
                int r = bgra ? px[2] : px[0], g = px[1], b = bgra ? px[0] : px[2];
                bool hr = r > 150, hg = g > 150, hb = b > 150, lr = r < 110, lg = g < 110, lb = b < 110;
                // each tile is looked for only in its own strip: the pose tile's colour can look like any of them
                bool mag = hr && hb && lg;
                if (mag && x >= mag_x0 && x < mag_x1) { n[0]++; if (x < mx0) mx0 = x; if (x > mx1) mx1 = x; if (y < my0) my0 = y; if (y > my1) my1 = y; }
                else if (!mag && x < eye_x1) { if (hr && lg && lb) n[1]++; else if (hg && lr && lb) n[2]++; else if (hb && lr && lg) n[3]++; }
            }
        }
        // the pose tile sits 3 PS1 pixels right of the magenta tile: average 3x3 host pixels at its centre
        if (mx1 >= mx0 && my1 >= my0) {
            int cx = (int)lround((mx0 + mx1) / 2.0 + 3 * psx), cy = (my0 + my1) / 2;
            if (cx >= 1 && cx + 1 < w && cy >= 1 && cy + 1 < h) {
                int sum[3] = {0, 0, 0};
                for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                    const uint8_t* px = (const uint8_t*)m.pData + (cy + dy) * m.RowPitch + 4 * (cx + dx);
                    sum[0] += bgra ? px[2] : px[0]; sum[1] += px[1]; sum[2] += bgra ? px[0] : px[2];
                }
                for (int k = 0; k < 3; ++k) seqc[k] = sum[k] / 9;
            }
        }
    }
    ctx->Unmap(st, 0);
    int mn = half ? (g_cap.mark_min + 1) / 2 : g_cap.mark_min; if (mn < 3) mn = 3;
    if (n[0] < mn) { g_mark_stats[0]++; return 0; }
    int best = 1; for (int k = 2; k <= 3; ++k) if (n[k] > n[best]) best = k;
    int second = 0; for (int k = 1; k <= 3; ++k) if (k != best && n[k] > second) second = n[k];
    if (n[best] < mn || n[best] < 2 * second) { g_mark_stats[0]++; return 0; }
    int code = best == 1 ? 1 : best == 2 ? 2 : 3;
    g_mark_stats[code]++;
    if (seqc[0] >= 0) {   // levels 0/85/170/255 per channel, 2 bits each: R = bits 0-1, G = bits 2-3, B = bits 4-5
        int v = 0; bool ok = true;
        for (int k = 0; k < 3; ++k) {
            int lvl = (seqc[k] + 42) / 85; if (lvl > 3) lvl = 3;
            if (abs(seqc[k] - lvl * 85) > 40) ok = false;
            v |= lvl << (2 * k);
        }
        if (ok) *mseq = v; else g_mseq_bad++;
    }
    return code;
}
static void capture(IDXGISwapChain* sc, UINT flags)
{
    if (flags & DXGI_PRESENT_TEST) return;
    double t0 = qpc_us();
    capture_inner(sc, flags);
    uint32_t us = (uint32_t)(qpc_us() - t0);
    g_pres_us_sum += us; g_pres_n++;
    uint32_t m = g_pres_us_max.load(); while (us > m && !g_pres_us_max.compare_exchange_weak(m, us)) {}
}
static void capture_inner(IDXGISwapChain* sc, UINT flags)
{
    static int fails = 0;
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) {
        if (fails++ == 0) logf("CAPTURE: presenting swapchain is not D3D11; no picture");
        return;
    }
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) { dev->Release(); return; }
    D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
    if (sc != g_cap.sc) {
        DXGI_SWAP_CHAIN_DESC scd = {}; sc->GetDesc(&scd);
        logf("CAPTURE: game swapchain %p window %p backbuffer %ux%u format %d samples %u", (void*)sc, (void*)scd.OutputWindow,
             d.Width, d.Height, (int)d.Format, d.SampleDesc.Count);
        g_cap.sc = sc; g_game_hwnd = scd.OutputWindow;
    }
    if (!g_cap.tex[0] || g_cap.dev != dev || g_cap.w != d.Width || g_cap.h != d.Height || g_cap.fmt != d.Format) {
        g_cap_ready = -1; g_cap_gen++;                       // headset side lets go of the old slots
        for (int k = 0; k < CAP_SLOTS; ++k) {
            if (g_cap.q[k]) { g_cap.q[k]->Release(); g_cap.q[k] = nullptr; }
            if (g_cap.tex[k]) { g_cap.tex[k]->Release(); g_cap.tex[k] = nullptr; }
            if (g_cap.stg[k]) { g_cap.stg[k]->Release(); g_cap.stg[k] = nullptr; }
            if (g_cap.stg2[k]) { g_cap.stg2[k]->Release(); g_cap.stg2[k] = nullptr; }
            g_cap_handles[k] = nullptr;
        }
        g_cap.pending = -1; g_cap.dev = dev;
        {   // the marker: PS1 pixels (0,0)-(5,1) of the 4:3 picture, 224 of 240 lines
            double fw = d.Height * 4.0 / 3.0; if (fw > d.Width) fw = d.Width;
            double x0 = (d.Width - fw) / 2, y0 = d.Height * 8.0 / 240, sx = fw / 320, sy = d.Height / 240.0;
            int bx0 = (int)x0 - 2, by0 = (int)y0 - 2; if (bx0 < 0) bx0 = 0; if (by0 < 0) by0 = 0;
            int bw = (int)(10 * sx) + 4, bh = (int)(3 * sy) + 4; if (bw > 64) bw = 64; if (bh > 32) bh = 32;
            g_cap.psx = sx; g_cap.psy = sy; g_cap.lead = (int)(x0 - bx0);
            g_cap.box = {(UINT)bx0, (UINT)by0, 0, (UINT)(bx0 + bw), (UINT)(by0 + bh), 1};
            int bx2 = bx0 + (int)lround(fw / 2);   // v20: the right half starts half a picture to the right
            g_cap.box2 = {(UINT)bx2, (UINT)by0, 0, (UINT)(bx2 + bw), (UINT)(by0 + bh), 1};
            g_cap.mark_min = (int)(0.25 * 3 * sx * 2 * sy); if (g_cap.mark_min < 4) g_cap.mark_min = 4;
        }
        D3D11_TEXTURE2D_DESC t = {};
        t.Width = d.Width; t.Height = d.Height; t.MipLevels = 1; t.ArraySize = 1; t.Format = d.Format;
        t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT; t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        t.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
        bool ok = true;
        for (int k = 0; k < CAP_SLOTS && ok; ++k) {
            HANDLE hnd = nullptr;
            if (SUCCEEDED(dev->CreateTexture2D(&t, nullptr, &g_cap.tex[k]))) {
                IDXGIResource* r = nullptr;
                if (SUCCEEDED(g_cap.tex[k]->QueryInterface(__uuidof(IDXGIResource), (void**)&r))) { r->GetSharedHandle(&hnd); r->Release(); }
            }
            if (FAILED(dev->CreateQuery(&qd, &g_cap.q[k]))) g_cap.q[k] = nullptr;
            D3D11_TEXTURE2D_DESC sd = t; sd.Width = 64; sd.Height = 32; sd.MiscFlags = 0; sd.BindFlags = 0;
            sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(dev->CreateTexture2D(&sd, nullptr, &g_cap.stg[k]))) g_cap.stg[k] = nullptr;
            if (FAILED(dev->CreateTexture2D(&sd, nullptr, &g_cap.stg2[k]))) g_cap.stg2[k] = nullptr;
            ok = hnd && g_cap.q[k];
            g_cap_handles[k] = hnd;
        }
        if (!ok) {
            if (fails++ < 3) logf("CAPTURE: could not create shared %ux%u textures (format %d)", d.Width, d.Height, (int)d.Format);
            for (int k = 0; k < CAP_SLOTS; ++k) {
                if (g_cap.q[k]) { g_cap.q[k]->Release(); g_cap.q[k] = nullptr; }
                if (g_cap.tex[k]) { g_cap.tex[k]->Release(); g_cap.tex[k] = nullptr; }
                g_cap_handles[k] = nullptr;
            }
            bb->Release(); dev->Release(); return;
        }
        g_cap.w = d.Width; g_cap.h = d.Height; g_cap.fmt = d.Format;
        g_cap_w = d.Width; g_cap_h = d.Height; g_cap_fmt = (uint32_t)d.Format;
        g_cap_gen++;
        logf("CAPTURE: %d shared slots %ux%u format %d ready, GPU-query hand-off (generation %u)", CAP_SLOTS, d.Width, d.Height, (int)d.Format, g_cap_gen.load());
    }
    ID3D11DeviceContext* ctx = nullptr; dev->GetImmediateContext(&ctx);
    if (!ctx) { bb->Release(); dev->Release(); return; }
    // publish the previous copy once the GPU has finished it (never wait)
    if (g_cap.pending >= 0) {
        if (ctx->GetData(g_cap.q[g_cap.pending], nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
            int ms = -1;
            if (g_ts_on.load()) {   // v20: side by side -- a half-width marker at the start of each half
                int ms2 = -1;
                g_slot_mark[g_cap.pending] = read_marker(ctx, g_cap.pending, d.Format, &ms, 0, true);
                g_slot_mark2[g_cap.pending] = read_marker(ctx, g_cap.pending, d.Format, &ms2, 1, true); g_slot_mseq2[g_cap.pending] = ms2;
                if (g_slot_mark[g_cap.pending] == 0) { g_slot_mark[g_cap.pending] = read_marker(ctx, g_cap.pending, d.Format, &ms); g_slot_mark2[g_cap.pending] = 0; }
            } else { g_slot_mark[g_cap.pending] = read_marker(ctx, g_cap.pending, d.Format, &ms); g_slot_mark2[g_cap.pending] = 0; }
            g_slot_mseq[g_cap.pending] = ms;
            g_cap_ready = g_cap.pending; g_cap_ready_seq++; g_cap_frames++;
            g_cap.pending = -1;
        } else { g_cap_skipped++; ctx->Release(); bb->Release(); dev->Release(); return; }
    }
    // which head pose was this picture drawn with? ring[(applied - 1 - delay) & 7] in the mailbox
    uint32_t seq = 0, scr = 320; int tracked = 0;
    {
        uint8_t* ram = g_ram.load(); const GameTable* T = g_T.load();
        if (ram && T && g_hook_ready.load()) {
            uint32_t applied = rd32(ram, MBOX + MB_APPLIED), en = rd32(ram, MBOX + MB_ENABLE);
            uint32_t dly = (uint32_t)g_tag_delay.load();
            if (applied > dly && en) { seq = rd32(ram, MBOX + MB_RING + 4 * ((applied - 1 - dly) & 7)); tracked = 1; }
            int16_t s16 = 0; rd(ram, T->chanl1 + 0x50, &s16, 2); if (s16 > 0) scr = (uint32_t)s16;
        }
    }
    // a slot that is neither the newest picture nor the one the headset side is reading
    int ready = g_cap_ready.load(), xr = g_xr_slot.load(), w = -1;
    for (int k = 0; k < CAP_SLOTS; ++k) if (k != ready && k != xr) { w = k; break; }
    if (w >= 0 && !g_test_blank.load()) {
        if (d.SampleDesc.Count > 1) ctx->ResolveSubresource(g_cap.tex[w], 0, bb, 0, d.Format);
        else ctx->CopyResource(g_cap.tex[w], bb);
        if (g_cap.stg[w]) ctx->CopySubresourceRegion(g_cap.stg[w], 0, 0, 0, 0, g_cap.tex[w], 0, &g_cap.box);
        if (g_cap.stg2[w] && g_ts_on.load()) ctx->CopySubresourceRegion(g_cap.stg2[w], 0, 0, 0, 0, g_cap.tex[w], 0, &g_cap.box2);
        ctx->End(g_cap.q[w]);
        g_slot_seq[w] = seq; g_slot_screen[w] = scr; g_slot_tracked[w] = tracked;   // tags travel with this picture
        g_cap.pending = w;
    }
    ctx->Release();
    bb->Release(); dev->Release();
}

static HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags)
{ capture(sc, flags); return o_Present(sc, sync, flags); }
static HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p)
{ capture(sc, flags); return o_Present1(sc, sync, flags, p); }

static bool patch_vtable(void** vt, int idx, void* fn, void** orig)
{
    DWORD old;
    if (!VirtualProtect(&vt[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    *orig = vt[idx]; vt[idx] = fn;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
    return true;
}

// Get IDXGISwapChain's vtable from a throwaway swapchain on a hidden window, then patch Present/Present1.
// The vtable lives in dxgi.dll and is shared by every swapchain, including the game's.
static void install_present_hook()
{
    WNDCLASSW wc = {}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"mgs1mc_vr_dummy";
    RegisterClassW(&wc);
    HWND hw = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1; scd.BufferDesc.Width = 64; scd.BufferDesc.Height = 64; scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; scd.OutputWindow = hw; scd.SampleDesc.Count = 1; scd.Windowed = TRUE;
    IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                               &scd, &sc, &dev, nullptr, &ctx);
    if (FAILED(hr)) hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                                       &scd, &sc, &dev, nullptr, &ctx);
    if (FAILED(hr) || !sc) { logf("PRESENT HOOK: could not create the probe swapchain (0x%08lX); no picture", hr); if (hw) DestroyWindow(hw); return; }
    void** vt = *(void***)sc;
    bool p0 = patch_vtable(vt, 8, (void*)hk_Present, (void**)&o_Present);
    bool p1 = false;
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1) {
        void** vt1 = *(void***)sc1;
        p1 = patch_vtable(vt1, 22, (void*)hk_Present1, (void**)&o_Present1);
        sc1->Release();
    }
    logf("PRESENT HOOK: Present %s, Present1 %s (vtable %p)", p0 ? "hooked" : "FAILED", p1 ? "hooked" : "not hooked", (void*)vt);
    sc->Release(); ctx->Release(); dev->Release();
    DestroyWindow(hw);
}

// ================================================================= OpenXR
#define XR_FN(name) static PFN_##name p##name = nullptr;
XR_FN(xrGetInstanceProcAddr) XR_FN(xrEnumerateInstanceExtensionProperties) XR_FN(xrCreateInstance) XR_FN(xrDestroyInstance)
XR_FN(xrGetSystem) XR_FN(xrGetSystemProperties) XR_FN(xrGetInstanceProperties) XR_FN(xrCreateSession) XR_FN(xrDestroySession)
XR_FN(xrCreateReferenceSpace) XR_FN(xrDestroySpace) XR_FN(xrBeginSession) XR_FN(xrEndSession) XR_FN(xrPollEvent)
XR_FN(xrWaitFrame) XR_FN(xrBeginFrame) XR_FN(xrEndFrame) XR_FN(xrLocateSpace) XR_FN(xrEnumerateSwapchainFormats)
XR_FN(xrCreateSwapchain) XR_FN(xrDestroySwapchain) XR_FN(xrEnumerateSwapchainImages) XR_FN(xrAcquireSwapchainImage)
XR_FN(xrWaitSwapchainImage) XR_FN(xrReleaseSwapchainImage) XR_FN(xrGetD3D11GraphicsRequirementsKHR)
XR_FN(xrStringToPath) XR_FN(xrCreateActionSet) XR_FN(xrCreateAction) XR_FN(xrSuggestInteractionProfileBindings)
XR_FN(xrAttachSessionActionSets) XR_FN(xrSyncActions) XR_FN(xrGetActionStateBoolean) XR_FN(xrGetActionStateFloat)
XR_FN(xrGetActionStateVector2f) XR_FN(xrCreateActionSpace) XR_FN(xrDestroyActionSet) XR_FN(xrGetCurrentInteractionProfile) XR_FN(xrPathToString)
XR_FN(xrApplyHapticFeedback)

// ---------------------------------------------------------------- VR controllers (OpenXR actions)
enum { A_LSTICK, A_RSTICK, A_A, A_B, A_X, A_Y, A_LTRIG, A_RTRIG, A_LGRIP, A_RGRIP, A_LCLICK, A_RCLICK, A_MENU, A_LPOSE, A_RPOSE, A_RAIM, A_LHAP, A_RHAP, A_LAIM, A_COUNT };
static bool xr_ok(XrResult r, const char* what);
struct XrInput { XrActionSet set = XR_NULL_HANDLE; XrAction act[A_COUNT] = {}; bool ok = false; bool rclick_prev = false;
                 XrSpace hand[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};   // left/right grip pose spaces (wrist display, gun position)
                 XrSpace raim = XR_NULL_HANDLE;                         // right AIM pose space (where the controller points: the gun barrel)
                 XrSpace laim = XR_NULL_HANDLE;                         // v13: left AIM pose space (the left hand)
                 double menu_down_t = 0; bool menu_prev = false, menu_used = false; double start_until = 0; double lclick_until = 0;
                 bool combo = false;   // v17
                 double codec_until = 0;   // v22
                 bool trig[2] = {false, false}; };                      // trigger held (melee: that hand never punches)

// ---------------------------------------------------------------- melee by motion + rumble (XR thread)
// As in the GOG mod. PUNCH: a hand thrown forward (along where you face, flat) faster than punch_speed and at least
// punch_reach in front of your head presses Circle once (70 ms); it re-arms when the hand slows under 0.6 m/s.
// CHOKEHOLD (no weapon): both hands close together, in front of the chest, still, for choke_hold_ms -> Square held
// (the game's grab); a jerk faster than choke_jerk while holding lets go for 60 ms so the game sees one more press.
static void xr_buzz(XrSession ses, XrInput& in, int hand, float amp, int ms)
{
    if (!g_cfg.rumble || !pxrApplyHapticFeedback || !in.ok) return;
    for (int h = 0; h < 2; ++h) {
        if (hand != 2 && hand != h) continue;
        XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO}; hi.action = in.act[h ? A_RHAP : A_LHAP];
        XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION}; v.amplitude = amp; v.duration = (XrDuration)ms * 1000000; v.frequency = XR_FREQUENCY_UNSPECIFIED;
        pxrApplyHapticFeedback(ses, &hi, (const XrHapticBaseHeader*)&v);
    }
}
struct HandTrk { bool have = false; double p[3] = {0, 0, 0}, v[3] = {0, 0, 0}, t = 0; bool armed = true; };
static uint16_t melee_update(XrSession ses, XrInput& in, const XrPosef* hand, const bool* hand_ok, const XrPosef& head, bool gate)
{
    static HandTrk H[2];
    static double punch_until = 0, last_punch = 0, choke_since = 0, choke_lost = 0, gap_until = 0, last_snap = 0;
    static bool choking = false, rtrig_prev = false;
    double now = qpc_us() / 1000.0;
    for (int i = 0; i < 2; ++i) {                     // smoothed hand velocity (m/s, LOCAL)
        HandTrk& h = H[i];
        if (!hand_ok[i]) { h.have = false; continue; }
        double p[3] = {hand[i].position.x, hand[i].position.y, hand[i].position.z};
        if (h.have) {
            double dt = (now - h.t) / 1000.0;
            if (dt > 0.002 && dt < 0.1) { double a = dt / (0.025 + dt); for (int k = 0; k < 3; ++k) h.v[k] += ((p[k] - h.p[k]) / dt - h.v[k]) * a; }
        } else h.v[0] = h.v[1] = h.v[2] = 0;
        memcpy(h.p, p, sizeof p); h.t = now; h.have = true;
    }
    // the gun in your hand fired: a short kick in the right hand
    bool rt = in.trig[1];
    if (rt && !rtrig_prev && gate && g_gun_state.load() == 1) { xr_buzz(ses, in, 1, 0.6f, 40); g_gun_shots_felt++; }
    rtrig_prev = rt;
    uint16_t out = 0;
    if (!gate) { choking = false; choke_since = 0; return now < punch_until ? 0x20 : 0; }
    XrQuaternionf q = head.orientation;
    double fx = -2 * (q.x * q.z + q.w * q.y), fz = -(1 - 2 * (q.x * q.x + q.y * q.y)), fl = sqrt(fx * fx + fz * fz);
    if (fl < 1e-3) return 0;
    fx /= fl; fz /= fl;
    const double hp[3] = {head.position.x, head.position.y, head.position.z};
    if (g_cfg.melee_punch) {
        for (int i = 0; i < 2; ++i) {
            HandTrk& h = H[i];
            if (!h.have) continue;
            double speed = sqrt(h.v[0] * h.v[0] + h.v[1] * h.v[1] + h.v[2] * h.v[2]);
            double vf = h.v[0] * fx + h.v[2] * fz, reach = (h.p[0] - hp[0]) * fx + (h.p[2] - hp[2]) * fz;
            if (vf < 0.6) h.armed = true;
            if (h.armed && !choking && !in.trig[i] && vf > g_cfg.punch_speed_cms / 100.0 && vf > 0.7 * speed &&
                reach > g_cfg.punch_reach_cm / 100.0 && now - last_punch > 150) {
                h.armed = false; last_punch = now; punch_until = now + 70; g_melee_punches++;
                xr_buzz(ses, in, i, 0.7f, 45);
                static int logged = 0;
                if (logged++ < 12) logf("MELEE: %s punch (%.1f m/s, %.0f cm out) -> Circle", i ? "right" : "left", vf, reach * 100);
            }
        }
    }
    if (now < punch_until) out |= 0x20;               // Circle
    bool pose = false;
    if (g_cfg.melee_choke && g_weapon_id.load() < 0 && H[0].have && H[1].have) {
        double d[3] = {H[0].p[0] - H[1].p[0], H[0].p[1] - H[1].p[1], H[0].p[2] - H[1].p[2]};
        double apart = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        auto front = [&](const HandTrk& h) { double reach = (h.p[0] - hp[0]) * fx + (h.p[2] - hp[2]) * fz, below = hp[1] - h.p[1];
                                             return reach > 0.12 && reach < 0.50 && below > 0.10 && below < 0.70; };
        auto still = [](const HandTrk& h) { return sqrt(h.v[0] * h.v[0] + h.v[1] * h.v[1] + h.v[2] * h.v[2]) < 0.8; };
        pose = apart < g_cfg.choke_apart_cm / 100.0 && front(H[0]) && front(H[1]) && !in.trig[0] && !in.trig[1] &&
               (choking || (still(H[0]) && still(H[1]))) && now - last_punch > 600;
    }
    if (pose) {
        choke_lost = 0;
        if (!choke_since) choke_since = now;
        if (!choking && now - choke_since >= g_cfg.choke_hold_ms) {
            choking = true; g_melee_grabs++; xr_buzz(ses, in, 2, 0.4f, 120);
            logf("MELEE: chokehold pose -> holding Square (the game's grab)");
        }
    } else if (choking) {
        if (!choke_lost) choke_lost = now;
        if (now - choke_lost > 150) { choking = false; choke_since = 0; logf("MELEE: chokehold released"); }
    } else choke_since = 0;
    if (choking) {
        double jerk = 0;
        for (int i = 0; i < 2; ++i) jerk = fmax(jerk, sqrt(H[i].v[0] * H[i].v[0] + H[i].v[1] * H[i].v[1] + H[i].v[2] * H[i].v[2]));
        if (jerk > g_cfg.choke_jerk_cms / 100.0 && now - last_snap > 250 && now - choke_since > g_cfg.choke_hold_ms + 300) {
            last_snap = now; gap_until = now + 60; g_melee_squeezes++; xr_buzz(ses, in, 2, 0.8f, 60);
            static int logged = 0;
            if (logged++ < 12) logf("MELEE: jerk while holding (%.1f m/s) -> Square pressed again", jerk);
        }
        if (now >= gap_until) out |= 0x80;            // Square
    }
    return out;
}

// ---------------------------------------------------------------- in-headset settings panel (XR thread)
// Hold the left menu button ~0.7 s to open or close. Stick up/down picks a line, left/right changes it, A presses,
// B closes. While open, the controllers drive the panel only. Every change is written to mgs1mc_vr.ini at once.
static std::atomic<bool> g_ini_force{false};   // main thread: re-read the ini now
struct PanelItem { const wchar_t* label; const wchar_t* sec; const wchar_t* key; int kind; int step, lo, hi, def; const wchar_t* const* opts; };
// kind: 0 on/off, 1 number, 2 smooth/snap, 3 action recentre, 4 action close, 5 fov (number shown as degrees), 6 tenths,
//       7 text choice from opts (stored as the text), 8 numbered choice from opts (stored as 0..n-1), 9 normal / inverted (1 / -1)
// v15: the panel is paged; row 0 of every page is the page switch.
static const wchar_t* const kOptBtn[] = {L"cross", L"circle", L"square", L"triangle", L"l1", L"l2", L"r1", L"r2", L"select", L"start",
                                         L"l3", L"r3", L"up", L"down", L"left", L"right", L"none", nullptr};
static const wchar_t* const kOptBtnRc[] = {L"recentre", L"cross", L"circle", L"square", L"triangle", L"l1", L"l2", L"r1", L"r2", L"select",
                                           L"start", L"l3", L"r3", L"up", L"down", L"left", L"right", L"none", nullptr};
static const wchar_t* const kOptSelector[] = {L"List", L"Hologram wheel", L"3D carousel", nullptr};
#define PANEL_TAIL {L"Recentre view", nullptr, nullptr, 3, 0, 0, 0, 0, nullptr}, {L"Close", nullptr, nullptr, 4, 0, 0, 0, 0, nullptr}
static const PanelItem kPgView[] = {
    {L"Stereo 3D",               L"stereo", L"enabled",            0, 1, 0, 1, 1},
    {L"Eye distance (mm)",       L"stereo", L"ipd_mm",             1, 1, 50, 80, 64},
    {L"World scale (units per m)", L"stereo", L"units_per_m",      1, 50, 400, 3000, 1000},
    {L"Swap eyes",               L"stereo", L"swap_eyes",          0, 1, 0, 1, 0},
    {L"True stereo (test)",      L"stereo", L"true_stereo",        0, 1, 0, 1, 0},
    {L"Mono while hands move (cm/s)", L"stereo", L"hold_hand_cm_s", 1, 5, 0, 500, 25},
    {L"Field of view",           L"phase3", L"fov_screen",         5, 10, 80, 320, 120},
    {L"Floating screen width (m)", L"phase2", L"screen_width_m",   6, 2, 10, 80, 32},
    {L"Codec on floating screen", L"screen", L"codec",             0, 1, 0, 1, 1},
    {L"Pause on floating screen", L"screen", L"pause",             0, 1, 0, 1, 1},
    {L"Menus on floating screen", L"screen", L"menus",             0, 1, 0, 1, 1},
    {L"Head-look in third person", L"phase2", L"track_third_person", 0, 1, 0, 1, 0},
    PANEL_TAIL };
static const PanelItem kPgMove[] = {
    {L"Walk in first person",    L"walk",   L"walk_mode",          0, 1, 0, 1, 1},
    {L"Turning",                 L"walk",   L"turn_mode",          2, 1, 0, 1, 0},
    {L"Snap angle (deg)",        L"walk",   L"snap_deg",           1, 15, 15, 90, 30},
    {L"Smooth turn speed (deg/s)", L"walk", L"turn_speed_deg",     1, 15, 30, 360, 120},
    {L"Body follows head",       L"walk",   L"body_follows_head",  0, 1, 0, 1, 1},
    {L"Lean (move your head)",   L"walk",   L"lean",               0, 1, 0, 1, 1},
    {L"Lean reach (cm)",         L"walk",   L"lean_max_cm",        1, 5, 5, 300, 100},
    {L"Lean in head-look views", L"walk",   L"third_person_lean",  0, 1, 0, 1, 1},
    {L"Hide Snake's body",       L"walk",   L"hide_body",          0, 1, 0, 1, 1},
    {L"Box / mask / goggle views", L"walk", L"fp_overlays",        0, 1, 0, 1, 1},
    PANEL_TAIL };
static const PanelItem kPgDiorama[] = {
    {L"Diorama mode",            L"diorama", L"enabled",           0, 1, 0, 1, 0},
    {L"Scale (1:N)",             L"diorama", L"scale",             1, 5, 5, 60, 20},
    {L"Table distance (cm)",     L"diorama", L"table_distance_cm", 1, 5, 10, 200, 60},
    {L"Table below eyes (cm)",   L"diorama", L"table_drop_cm",     1, 5, -50, 150, 50},
    {L"Follow Snake after (units)", L"diorama", L"follow_radius",  1, 250, 0, 20000, 1500},
    {L"Follow speed (/10 s)",    L"diorama", L"follow_speed",      1, 5, 1, 200, 30},
    {L"New area jump (units)",   L"diorama", L"cut_units",         1, 500, 500, 30000, 4000},
    {L"Turn to new areas",       L"diorama", L"reorient_on_area_change", 0, 1, 0, 1, 1},
    {L"Snake eye height (units)", L"diorama", L"snake_eye_height", 1, 100, 0, 4000, 1500},
    {L"Table turn step (deg)",   L"diorama", L"turn_step_deg",     1, 5, 5, 90, 30},
    PANEL_TAIL };
static const PanelItem kPgHands[] = {
    {L"Gun in hand (aim by pointing)", L"motion", L"gun_in_hand",  0, 1, 0, 1, 1},
    {L"Throw from your hand",    L"motion", L"throw_from_hand",    0, 1, 0, 1, 1},
    {L"Floating hands",          L"hands",  L"enabled",            0, 1, 0, 1, 1},
    {L"Hands stop at walls",     L"hands",  L"wall_collision",     0, 1, 0, 1, 1},
    {L"Knock on walls",          L"hands",  L"knock",              0, 1, 0, 1, 1},
    {L"Punch by swinging",       L"melee",  L"punch",              0, 1, 0, 1, 1},
    {L"Chokehold: hands together", L"melee", L"chokehold",         0, 1, 0, 1, 1},
    {L"Controller rumble",       L"melee",  L"rumble",             0, 1, 0, 1, 1},
    {L"PSG1 round lens",         L"scope",  L"psg1_lens",          0, 1, 0, 1, 1},
    {L"Scope size (deg)",        L"scope",  L"display_deg",        1, 2, 8, 100, 48},
    {L"Aim PSG1 / Stinger by head", L"scope", L"aim_by_head",      0, 1, 0, 1, 1},
    {L"Scope up / down",         L"scope",  L"pitch_sign",         9, 1, -1, 1, 1},
    PANEL_TAIL };
static const PanelItem kPgWrist[] = {
    {L"Wrist display",           L"wrist_hud", L"enabled",         0, 1, 0, 1, 1},
    {L"Choose on the wrist (grips)", L"wrist_hud", L"select_on_wrist", 0, 1, 0, 1, 1},
    {L"Selection style",         L"wrist_hud", L"selector",        8, 1, 0, 2, 2, kOptSelector},
    {L"Carousel model size (cm)", L"wrist_hud", L"carousel_size_cm", 1, 1, 6, 30, 13},
    {L"Pause while choosing",    L"wrist_hud", L"pause_while_selecting", 0, 1, 0, 1, 1},
    {L"Wheel size (cm)",         L"wrist_hud", L"wheel_size_cm",   1, 1, 15, 60, 30},
    {L"Wheel height (cm)",       L"wrist_hud", L"wheel_height_cm", 1, 1, 0, 40, 12},
    {L"Wheel model depth (deg)", L"wrist_hud", L"wheel_stereo_deg", 1, 1, 0, 15, 4},
    {L"Boss LIFE bars",          L"wrist_hud", L"show_boss_life",  0, 1, 0, 1, 1},
    {L"Use item with A",         L"wrist_hud", L"item_use_button", 0, 1, 0, 1, 1},
    PANEL_TAIL };
static const PanelItem kPgScenes[] = {
    {L"Cutscenes: Snake's eyes", L"cutscene", L"snake_eyes",       0, 1, 0, 1, 1},
    {L"Snake's-eyes cutscenes in 3D", L"cutscene", L"stereo",      0, 1, 0, 1, 0},
    {L"Game's blur/focus in VR", L"cutscene", L"blur_effect",      0, 1, 0, 1, 0},
    {L"Vehicles / rope / rack: Snake's eyes", L"setpiece", L"snake_eyes", 0, 1, 0, 1, 1},
    {L"Cutscene subtitles lifted", L"subtitles", L"enabled",        0, 1, 0, 1, 1},
    {L"Subtitles below gaze (deg)", L"subtitles", L"panel_down_deg", 1, 2, -30, 60, 20},
    {L"Subtitle panel width (cm)", L"subtitles", L"panel_width_cm", 1, 5, 20, 300, 80},
    {L"Performance logging",     L"motion", L"perf_counters",      0, 1, 0, 1, 0},
    PANEL_TAIL };
static const PanelItem kPgControls[] = {
    {L"VR controllers",          L"controls", L"enabled",          0, 1, 0, 1, 1},
    {L"A",                       L"controls", L"a",                7, 1, 0, 0, 1, kOptBtn},
    {L"B",                       L"controls", L"b",                7, 1, 0, 0, 3, kOptBtn},
    {L"X",                       L"controls", L"x",                7, 1, 0, 0, 4, kOptBtn},
    {L"Y",                       L"controls", L"y",                7, 1, 0, 0, 6, kOptBtn},
    {L"Right trigger",           L"controls", L"right_trigger",    7, 1, 0, 0, 2, kOptBtn},
    {L"Left trigger",            L"controls", L"left_trigger",     7, 1, 0, 0, 0, kOptBtn},
    {L"Right grip",              L"controls", L"right_grip",       7, 1, 0, 0, 7, kOptBtn},
    {L"Left grip",               L"controls", L"left_grip",        7, 1, 0, 0, 5, kOptBtn},
    {L"Left stick click",        L"controls", L"left_stick_click", 7, 1, 0, 0, 8, kOptBtn},
    {L"Right stick click",       L"controls", L"right_stick_click", 7, 1, 0, 0, 0, kOptBtnRc},
    {L"Menu (short press)",      L"controls", L"menu",             7, 1, 0, 0, 9, kOptBtn},
    {L"R3 x2: VR / diorama / flat", L"controls", L"r3_double_view", 0, 1, 0, 1, 1},
    {L"L3 + R3: Master Coll. menu", L"controls", L"mc_menu_both_sticks", 0, 1, 0, 1, 1},
    {L"Left grip at ear: Codec", L"controls", L"codec_ear_gesture", 0, 1, 0, 1, 1},
    {L"X + Y together: Start", L"controls", L"xy_start",            0, 1, 0, 1, 1},
    PANEL_TAIL };
#undef PANEL_TAIL
struct PanelPage { const wchar_t* name; const PanelItem* items; int n; };
#define PANEL_PAGE(nm, a) {nm, a, (int)(sizeof(a) / sizeof(a[0]))}
static const PanelPage kPages[] = { PANEL_PAGE(L"VIEW", kPgView), PANEL_PAGE(L"MOVEMENT", kPgMove), PANEL_PAGE(L"DIORAMA", kPgDiorama),
                                    PANEL_PAGE(L"HANDS & AIM", kPgHands), PANEL_PAGE(L"WRIST", kPgWrist), PANEL_PAGE(L"SCENES", kPgScenes),
                                    PANEL_PAGE(L"CONTROLS", kPgControls) };
#undef PANEL_PAGE
static const int kPageN = (int)(sizeof(kPages) / sizeof(kPages[0]));
struct PanelState {
    bool open = false, dirty = true; int sel = 0, page = 0;   // sel 0 = the page row, 1.. = the page's items
    double rep_t = 0; int rep_dir = 0;          // stick auto-repeat
    bool a_prev = false, b_prev = false;
    XrPosef pose{{0, 0, 0, 1}, {0, 0, -1}};
};
static PanelState g_panel;
static XrPosef g_last_head{{0, 0, 0, 1}, {0, 0, 0}};

static int opt_count(const wchar_t* const* o) { int n = 0; while (o && o[n]) ++n; return n; }
static int panel_get(const PanelItem& it)
{
    std::wstring ini = g_dir + L"mgs1mc_vr.ini";
    if (it.kind == 2) { wchar_t b[16] = {0}; GetPrivateProfileStringW(it.sec, it.key, L"smooth", b, 16, ini.c_str()); return _wcsicmp(b, L"snap") == 0; }
    if (it.kind == 6) { wchar_t b[32] = {0}; wchar_t d[16]; swprintf(d, 16, L"%.1f", it.def / 10.0);
                        GetPrivateProfileStringW(it.sec, it.key, d, b, 32, ini.c_str()); return (int)lround(_wtof(b) * 10); }
    if (it.kind == 7) {   // text choice: index of the stored text in opts (unknown text -> the default)
        wchar_t b[32] = {0}; GetPrivateProfileStringW(it.sec, it.key, it.opts[it.def], b, 32, ini.c_str());
        if (!_wcsicmp(b, L"recenter")) wcscpy(b, L"recentre");
        for (int k = 0; it.opts[k]; ++k) if (!_wcsicmp(b, it.opts[k])) return k;
        return it.def;
    }
    if (it.kind == 9) return GetPrivateProfileIntW(it.sec, it.key, it.def, ini.c_str()) < 0 ? -1 : 1;
    int v = GetPrivateProfileIntW(it.sec, it.key, it.def, ini.c_str());
    if (it.kind == 8) { int n = opt_count(it.opts); if (v < 0 || v >= n) v = n ? (v > 0 ? n - 1 : 0) : 0; }
    return v;
}
static void panel_set(const PanelItem& it, int v)
{
    std::wstring ini = g_dir + L"mgs1mc_vr.ini";
    wchar_t b[32];
    if (it.kind == 2) wcscpy(b, v ? L"snap" : L"smooth"); else if (it.kind == 6) swprintf(b, 32, L"%.1f", v / 10.0);
    else if (it.kind == 7) wcscpy(b, it.opts[v]); else swprintf(b, 32, L"%d", v);
    WritePrivateProfileStringW(it.sec, it.key, b, ini.c_str());
    g_ini_force = true;
    char lab[64]; WideCharToMultiByte(CP_UTF8, 0, it.label, -1, lab, 64, nullptr, nullptr);
    char val[32]; WideCharToMultiByte(CP_UTF8, 0, b, -1, val, 32, nullptr, nullptr);
    char sec[24]; WideCharToMultiByte(CP_UTF8, 0, it.sec, -1, sec, 24, nullptr, nullptr);
    char key[40]; WideCharToMultiByte(CP_UTF8, 0, it.key, -1, key, 40, nullptr, nullptr);
    logf("PANEL: %s -> %s  ([%s] %s)", lab, val, sec, key);
}
static void panel_open(bool open)
{
    g_panel.open = open; g_panel.dirty = true;
    if (open) {   // 1 m in front of the head, level, facing you
        XrQuaternionf q = g_last_head.orientation;
        double fx = -2 * (q.x * q.z + q.w * q.y), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));   // head forward (-Z) in local
        double len = sqrt(fx * fx + fz * fz); if (len < 1e-3) { fx = 0; fz = -1; len = 1; }
        fx /= len; fz /= len;
        double yaw = atan2(-fx, -fz);
        g_panel.pose.orientation = {0, (float)sin(yaw / 2), 0, (float)cos(yaw / 2)};
        g_panel.pose.position = {g_last_head.position.x + (float)fx, g_last_head.position.y - 0.05f, g_last_head.position.z + (float)fz};
    }
    logf("PANEL: %s", open ? "opened" : "closed");
}
// change one item by a stick step (+1 right / -1 left)
static void panel_change(const PanelItem& it, int d)
{
    if (it.kind == 0 || it.kind == 2) panel_set(it, panel_get(it) ? 0 : 1);
    else if (it.kind == 1 || it.kind == 5 || it.kind == 6) {
        int v = panel_get(it), dd = d * it.step;
        if (it.kind == 5) dd = -dd;                     // fov: a smaller screen distance is a wider view
        v += dd; if (v < it.lo) v = it.lo; if (v > it.hi) v = it.hi; panel_set(it, v);
    }
    else if (it.kind == 7 || it.kind == 8) { int n = opt_count(it.opts); if (n) panel_set(it, ((panel_get(it) + d) % n + n) % n); }
    else if (it.kind == 9) panel_set(it, -panel_get(it));
}
// stick/buttons -> panel actions (called with the raw controller state while the panel is open)
static void panel_input(float sx, float sy, bool a, bool b, double now, bool lt, bool rt, bool y)
{
    {   // v16 (GOG): triggers = previous / next page, Y = back to default
        static bool lt_prev = false, rt_prev = false, y_prev = false;
        if ((lt && !lt_prev) || (rt && !rt_prev)) {
            g_panel.page = (g_panel.page + (rt && !rt_prev ? 1 : kPageN - 1)) % kPageN; g_panel.sel = 0; g_panel.dirty = true;
            char nm[32]; WideCharToMultiByte(CP_UTF8, 0, kPages[g_panel.page].name, -1, nm, 32, nullptr, nullptr); logf("PANEL: page %s", nm);
        }
        if (y && !y_prev && g_panel.sel > 0) {
            const PanelItem& it = kPages[g_panel.page].items[g_panel.sel - 1];
            if (it.sec && it.kind != 3 && it.kind != 4) { panel_set(it, it.def); g_panel.dirty = true; }
        }
        lt_prev = lt; rt_prev = rt; y_prev = y;
    }
    int dir = sy > 0.6f ? 1 : sy < -0.6f ? 2 : sx > 0.6f ? 3 : sx < -0.6f ? 4 : 0;   // 1 up 2 down 3 right 4 left
    bool fire = false;
    if (dir != g_panel.rep_dir) { g_panel.rep_dir = dir; fire = dir != 0; g_panel.rep_t = now + 400; }
    else if (dir && now >= g_panel.rep_t) { fire = true; g_panel.rep_t = now + 150; }
    const PanelPage& pg = kPages[g_panel.page];
    int rows = pg.n + 1;
    if (g_panel.sel >= rows) g_panel.sel = rows - 1;
    if (fire) {
        if (dir == 1) g_panel.sel = (g_panel.sel + rows - 1) % rows;
        else if (dir == 2) g_panel.sel = (g_panel.sel + 1) % rows;
        else if (g_panel.sel == 0) {   // page row: left / right turns the page
            g_panel.page = (g_panel.page + (dir == 3 ? 1 : kPageN - 1)) % kPageN;
            char nm[32]; WideCharToMultiByte(CP_UTF8, 0, kPages[g_panel.page].name, -1, nm, 32, nullptr, nullptr);
            logf("PANEL: page %s", nm);
        }
        else panel_change(pg.items[g_panel.sel - 1], dir == 3 ? 1 : -1);
        g_panel.dirty = true;
    }
    if (a && !g_panel.a_prev) {
        if (g_panel.sel == 0) g_panel.page = (g_panel.page + 1) % kPageN;
        else {
            const PanelItem& it = pg.items[g_panel.sel - 1];
            if (it.kind == 0 || it.kind == 2 || it.kind == 7 || it.kind == 8 || it.kind == 9) panel_change(it, 1);
            else if (it.kind == 3) { g_recentre = true; logf("PANEL: recentre"); }
            else if (it.kind == 4) panel_open(false);
        }
        g_panel.dirty = true;
    }
    if (b && !g_panel.b_prev) panel_open(false);
    g_panel.a_prev = a; g_panel.b_prev = b;
}

static bool xr_setup_input(XrInstance inst, XrSession ses, XrInput& in)
{
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(si.actionSetName, "gameplay"); strcpy(si.localizedActionSetName, "Gameplay"); si.priority = 0;
    if (!xr_ok(pxrCreateActionSet(inst, &si, &in.set), "xrCreateActionSet")) return false;
    struct { const char* name; const char* loc; XrActionType type; } D[A_COUNT] = {
        {"left_stick", "Move", XR_ACTION_TYPE_VECTOR2F_INPUT}, {"right_stick", "Turn", XR_ACTION_TYPE_VECTOR2F_INPUT},
        {"button_a", "A", XR_ACTION_TYPE_BOOLEAN_INPUT}, {"button_b", "B", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {"button_x", "X", XR_ACTION_TYPE_BOOLEAN_INPUT}, {"button_y", "Y", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {"left_trigger", "Left trigger", XR_ACTION_TYPE_FLOAT_INPUT}, {"right_trigger", "Right trigger", XR_ACTION_TYPE_FLOAT_INPUT},
        {"left_grip", "Left grip", XR_ACTION_TYPE_FLOAT_INPUT}, {"right_grip", "Right grip", XR_ACTION_TYPE_FLOAT_INPUT},
        {"left_stick_click", "Left stick click", XR_ACTION_TYPE_BOOLEAN_INPUT}, {"right_stick_click", "Right stick click", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {"menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {"left_hand", "Left hand", XR_ACTION_TYPE_POSE_INPUT}, {"right_hand", "Right hand", XR_ACTION_TYPE_POSE_INPUT},
        {"right_aim", "Right hand aim", XR_ACTION_TYPE_POSE_INPUT},
        {"left_haptic", "Left rumble", XR_ACTION_TYPE_VIBRATION_OUTPUT}, {"right_haptic", "Right rumble", XR_ACTION_TYPE_VIBRATION_OUTPUT},
        {"left_aim", "Left hand aim", XR_ACTION_TYPE_POSE_INPUT} };
    for (int i = 0; i < A_COUNT; ++i) {
        XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
        strcpy(ai.actionName, D[i].name); strcpy(ai.localizedActionName, D[i].loc); ai.actionType = D[i].type;
        if (!xr_ok(pxrCreateAction(in.set, &ai, &in.act[i]), D[i].name)) return false;
    }
    auto P = [&](const char* s) { XrPath p = XR_NULL_PATH; pxrStringToPath(inst, s, &p); return p; };
    struct B { int a; const char* path; };
    const B touch[] = {
        {A_LSTICK, "/user/hand/left/input/thumbstick"}, {A_RSTICK, "/user/hand/right/input/thumbstick"},
        {A_A, "/user/hand/right/input/a/click"}, {A_B, "/user/hand/right/input/b/click"},
        {A_X, "/user/hand/left/input/x/click"}, {A_Y, "/user/hand/left/input/y/click"},
        {A_LTRIG, "/user/hand/left/input/trigger/value"}, {A_RTRIG, "/user/hand/right/input/trigger/value"},
        {A_LGRIP, "/user/hand/left/input/squeeze/value"}, {A_RGRIP, "/user/hand/right/input/squeeze/value"},
        {A_LCLICK, "/user/hand/left/input/thumbstick/click"}, {A_RCLICK, "/user/hand/right/input/thumbstick/click"},
        {A_MENU, "/user/hand/left/input/menu/click"},
        {A_LPOSE, "/user/hand/left/input/grip/pose"}, {A_RPOSE, "/user/hand/right/input/grip/pose"},
        {A_RAIM, "/user/hand/right/input/aim/pose"},
        {A_LHAP, "/user/hand/left/output/haptic"}, {A_RHAP, "/user/hand/right/output/haptic"},
        {A_LAIM, "/user/hand/left/input/aim/pose"} };
    const B index[] = {
        {A_LSTICK, "/user/hand/left/input/thumbstick"}, {A_RSTICK, "/user/hand/right/input/thumbstick"},
        {A_A, "/user/hand/right/input/a/click"}, {A_B, "/user/hand/right/input/b/click"},
        {A_X, "/user/hand/left/input/a/click"}, {A_Y, "/user/hand/left/input/b/click"},
        {A_LTRIG, "/user/hand/left/input/trigger/value"}, {A_RTRIG, "/user/hand/right/input/trigger/value"},
        {A_LGRIP, "/user/hand/left/input/squeeze/value"}, {A_RGRIP, "/user/hand/right/input/squeeze/value"},
        {A_LCLICK, "/user/hand/left/input/thumbstick/click"}, {A_RCLICK, "/user/hand/right/input/thumbstick/click"},
        {A_LPOSE, "/user/hand/left/input/grip/pose"}, {A_RPOSE, "/user/hand/right/input/grip/pose"},
        {A_RAIM, "/user/hand/right/input/aim/pose"},
        {A_LHAP, "/user/hand/left/output/haptic"}, {A_RHAP, "/user/hand/right/output/haptic"},
        {A_LAIM, "/user/hand/left/input/aim/pose"} };
    auto suggest = [&](const char* profile, const B* b, int n) {
        std::vector<XrActionSuggestedBinding> v;
        for (int i = 0; i < n; ++i) v.push_back({in.act[b[i].a], P(b[i].path)});
        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = P(profile); sb.countSuggestedBindings = (uint32_t)v.size(); sb.suggestedBindings = v.data();
        XrResult r = pxrSuggestInteractionProfileBindings(inst, &sb);
        logf("INPUT: bindings for %s %s", profile, XR_SUCCEEDED(r) ? "accepted" : "rejected");
    };
    suggest("/interaction_profiles/oculus/touch_controller", touch, (int)(sizeof touch / sizeof touch[0]));
    suggest("/interaction_profiles/valve/index_controller", index, (int)(sizeof index / sizeof index[0]));
    XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO}; at.countActionSets = 1; at.actionSets = &in.set;
    if (!xr_ok(pxrAttachSessionActionSets(ses, &at), "xrAttachSessionActionSets")) return false;
    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo asi{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        asi.action = in.act[h ? A_RPOSE : A_LPOSE]; asi.poseInActionSpace.orientation.w = 1;
        if (!pxrCreateActionSpace || XR_FAILED(pxrCreateActionSpace(ses, &asi, &in.hand[h]))) in.hand[h] = XR_NULL_HANDLE;
    }
    {
        XrActionSpaceCreateInfo asi{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        asi.action = in.act[A_RAIM]; asi.poseInActionSpace.orientation.w = 1;
        if (!pxrCreateActionSpace || XR_FAILED(pxrCreateActionSpace(ses, &asi, &in.raim))) in.raim = XR_NULL_HANDLE;
        asi.action = in.act[A_LAIM];
        if (!pxrCreateActionSpace || XR_FAILED(pxrCreateActionSpace(ses, &asi, &in.laim))) in.laim = XR_NULL_HANDLE;
    }
    logf("INPUT: hand pose spaces %s, right aim pose %s", in.hand[0] && in.hand[1] ? "ready" : "not available", in.raim ? "ready" : "not available");
    in.ok = true;
    return true;
}

#include <algorithm>
#include <initializer_list>
// ======================================================================================================================
// v15/v16 HOLOGRAM WHEEL + 3D CAROUSEL: the GOG mod's wrist_wheel.cpp (2026-10-14, wheel and carousel) and the parts of its game_models.cpp that draw, with the
// model source swapped for MC: the Master Collection packs the PS1 data, so instead of stage.mgz the models and the menu
// icons are read from the PS1 RAM, where the game keeps them resident for the whole run (GV cache, libgv/cache.c):
// weapon / prop KMDs (cache id StrCode(name) | 'k' spec) and item.rpk ('r' spec). Both are relocated in RAM (pointers in
// place of file offsets); they are turned back into offsets before GOG's parsers see them. Textures live in the PS1 VRAM,
// which the DLL cannot read, so the models are drawn untextured in hologram cyan.
// ======================================================================================================================
namespace WristWheel {

// Canvas: the wheel in the middle with room round it for the names.
constexpr int kW = 768, kH = 576;
constexpr int kCX = kW / 2, kCY = kH / 2;
constexpr int kDiscR = 254;          // the wheel's outer edge, in canvas pixels
constexpr int kMaxEntries = 26;

struct Entry {
    int  id = -1;              // weapon / item id, -1 = NONE (unequip)
    char name[16] = {};        // "FA-MAS"
    char detail[16] = {};      // "120" (ammo), "x3" (count), "LV5" (card), "" for none
    bool equipped = false;
    char desc[192] = {};       // the game's own description, lines split by '\n'; first line = full name
};

struct View {
    bool  weapons = true;      // weapon wheel (right wrist) or item wheel (left)
    bool  suppressed = false;  // SOCOM shows with its suppressor
    int   count = 0;
    Entry e[kMaxEntries];
    int   sel = 0;
    bool  cursorOn = false;    // where a fingertip / pointing ray is (wheel pixels)
    int   cursorX = 0, cursorY = 0;
    float spinRad = 0.0f;      // model turntable angle
    float stereoDeg = 4.0f;    // eye-to-eye model angle; 0 = flat
    bool  labels = true;       // small names under the ring icons
    const char* hint = nullptr;  // one line at the bottom ("RELEASE GRIP TO EQUIP")
};

// Draws the wheel. Returns true if the right-eye half differs from the left
// (a 3D model was drawn in stereo); false = both eyes can use rows 0..kSize-1.
bool Draw(const View& v, uint8_t* rgba /* kSize x (2*kSize) */);

// Slot under a point given in wheel units: x right, y up, 1.0 = the wheel's
// outer edge. -1 when the point is in the middle or outside the ring.
int SlotAt(int count, float x, float y);

// Slot a stick direction points at (x right, y up), or -1 if barely pushed.
int SlotForStick(int count, float x, float y, float deadzone = 0.55f);

// ---- CAROUSEL (2026-10-14) ----------------------------------------------------
// The "Iron Man" way: the entries as holographic 3D models in a row floating
// above the wrist, swiped left / right with the other hand. Each model is its
// own quad placed in 3D (vr_aim.cpp); these draw one model tile and the name
// plate under the focused one.
constexpr int kTile = 192;                    // one model tile (square)
constexpr int kAtlasW = 6 * kTile;            // 1152: six tiles per row
constexpr int kLabelH = 256;                  // name plate row under two rows of tiles
constexpr int kAtlasH = 2 * kTile + kLabelH;  // 640
constexpr int kTileSlots = 12;                // 2 rows x 6

// One tile at (x, y) in an atlas kAtlasW wide: a cyan hologram plinth, the
// model (spinning when focused, a fixed three-quarter view otherwise) or the
// game's icon, a small name under it when not focused, a gold mark if equipped.
void DrawCarouselTile(const Entry& e, bool weapons, bool suppressed, bool focused,
                      float spinRad, float eyeShiftRad, uint8_t* atlas, int x, int y);
// The name plate (kAtlasW x kLabelH at row y): big name, ammo / count, the
// game's own description of it, a hint.
void DrawCarouselLabel(const Entry& e, const char* hint, bool canLeft, bool canRight,
                       uint8_t* atlas, int y);

} // namespace WristWheel

namespace GameModels {
namespace {
uint16_t U16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t U32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t StrCode(const char* s) {
    uint16_t h = 0;
    for (; *s; ++s) { h = (uint16_t)((h << 5) | (h >> 11)); h = (uint16_t)(h + (uint8_t)*s); }
    return h;
}
struct Tex { int w = 0, h = 0; std::vector<uint8_t> rgba; };
struct Tri { float p[3][3]; float uv[3][2]; int tex; float nrm[3]; };
struct Model { std::string name; std::vector<Tri> tris; float c[3] = {}; float rXZ = 1, rY = 1; };   // turntable radius / half height

int32_t I32(const uint8_t* p) { return (int32_t)U32(p); }

bool ParseKmd(const uint8_t* d, size_t n, Model& m, const std::vector<uint16_t>& texIds, int frame) {
    if (n < 32) return false;
    const int nModels = I32(d);
    if (nModels <= 0 || nModels > 64 || 32 + (size_t)nModels * 88 > n) return false;
    std::vector<float> wx(nModels), wy(nModels), wz(nModels);
    float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
    for (int mi = 0; mi < nModels; ++mi) {
        const uint8_t* h = d + 32 + (size_t)mi * 88;
        const int nFaces = I32(h + 4);
        const int tx = I32(h + 32), ty = I32(h + 36), tz = I32(h + 40), parent = I32(h + 44);
        const int nVerts = I32(h + 52);
        const uint32_t vOff = U32(h + 56), iOff = U32(h + 60), uvOff = U32(h + 76), tOff = U32(h + 80);
        wx[mi] = (float)tx; wy[mi] = (float)ty; wz[mi] = (float)tz;
        if (parent >= 0 && parent < mi) { wx[mi] += wx[parent]; wy[mi] += wy[parent]; wz[mi] += wz[parent]; }
        if (nFaces < 0 || nVerts < 0 || vOff + (size_t)nVerts * 8 > n || iOff + (size_t)nFaces * 4 > n) return false;
        if (uvOff && uvOff + (size_t)nFaces * 8 > n) return false;
        if (tOff && tOff + (size_t)nFaces * 2 > n) return false;
        for (int f = 0; f < nFaces; ++f) {
            const uint8_t* ix = d + iOff + (size_t)f * 4;
            if ((ix[0] | ix[1] | ix[2] | ix[3]) & 0x80) continue;   // vertex shared from the parent part: not needed for props
            float P[4][3], UV[4][2];
            bool ok = true;
            for (int k = 0; k < 4; ++k) {
                if (ix[k] >= nVerts) { ok = false; break; }
                const uint8_t* v = d + vOff + (size_t)ix[k] * 8;
                const float x = (float)(int16_t)U16(v) + wx[mi], y = (float)(int16_t)U16(v + 2) + wy[mi], z = (float)(int16_t)U16(v + 4) + wz[mi];
                // Display frame: x right, y down, z away. Weapons are modelled
                // in Snake's hand: barrel along -Y, the top of the gun toward
                // +Z -> barrel to the right, scope / slide up, grip down
                // (checked against all ten: the claymore then reads "FRONT").
                // Props (cigar, box) are shown as modelled.
                if (frame == 1) { P[k][0] = -y; P[k][1] = -z; P[k][2] = x; }
                else            { P[k][0] = x;  P[k][1] = y; P[k][2] = z; }
                UV[k][0] = uvOff ? d[uvOff + (size_t)f * 8 + k * 2] / 256.0f : 0.5f;
                UV[k][1] = uvOff ? d[uvOff + (size_t)f * 8 + k * 2 + 1] / 256.0f : 0.5f;
            }
            if (!ok) continue;
            int tex = -1;
            if (tOff) {
                const uint16_t id = U16(d + tOff + (size_t)f * 2);
                for (size_t t = 0; t < texIds.size(); ++t) if (texIds[t] == id) { tex = (int)t; break; }
            }
            const int order[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
            for (int s = 0; s < 2; ++s) {
                Tri t{};
                for (int k = 0; k < 3; ++k) {
                    for (int a = 0; a < 3; ++a) t.p[k][a] = P[order[s][k]][a];
                    t.uv[k][0] = UV[order[s][k]][0]; t.uv[k][1] = UV[order[s][k]][1];
                }
                const float e1[3] = { t.p[1][0] - t.p[0][0], t.p[1][1] - t.p[0][1], t.p[1][2] - t.p[0][2] };
                const float e2[3] = { t.p[2][0] - t.p[0][0], t.p[2][1] - t.p[0][1], t.p[2][2] - t.p[0][2] };
                float nn[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
                const float l = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
                if (l < 1e-6f) continue;   // degenerate (a triangle stored as a quad)
                for (int a = 0; a < 3; ++a) t.nrm[a] = nn[a] / l;
                t.tex = tex;
                for (int k = 0; k < 3; ++k) for (int a = 0; a < 3; ++a) { mn[a] = std::min(mn[a], t.p[k][a]); mx[a] = std::max(mx[a], t.p[k][a]); }
                m.tris.push_back(t);
            }
        }
    }
    if (m.tris.empty()) return false;
    float r2 = 0, ry = 0;
    for (int a = 0; a < 3; ++a) m.c[a] = (mn[a] + mx[a]) * 0.5f;
    for (const Tri& t : m.tris) for (int k = 0; k < 3; ++k) {
        const float dx = t.p[k][0] - m.c[0], dz = t.p[k][2] - m.c[2], dy = t.p[k][1] - m.c[1];
        r2 = std::max(r2, dx * dx + dz * dz);
        ry = std::max(ry, std::fabs(dy));
    }
    m.rXZ = std::max(1.0f, std::sqrt(r2)); m.rY = std::max(1.0f, ry);
    return true;
}

bool DecodeRpkIcon(const uint8_t* b, size_t n, int img, int pal, Tex& t) {
    if (n < 4) return false;
    const int count = b[0] + b[1];
    const size_t data = 4 + (size_t)count * 4;
    if (img >= count || pal >= count || data > n) return false;
    const size_t pi = data + U32(b + 4 + (size_t)pal * 4), ii = data + U32(b + 4 + (size_t)img * 4);
    if (pi + 4 + 32 > n || ii + 4 > n) return false;
    const int w = b[ii + 2] * 4, h = b[ii + 3];
    if (w <= 0 || h <= 0 || ii + 4 + (size_t)(w / 2) * h > n) return false;
    uint8_t colors[16][4];
    for (int i = 0; i < 16; ++i) {
        const uint16_t v = U16(b + pi + 4 + i * 2);
        colors[i][0] = (uint8_t)((v & 31) * 255 / 31);
        colors[i][1] = (uint8_t)(((v >> 5) & 31) * 255 / 31);
        colors[i][2] = (uint8_t)(((v >> 10) & 31) * 255 / 31);
        colors[i][3] = v ? 255 : 0;
    }
    t.w = w; t.h = h; t.rgba.resize((size_t)w * h * 4);
    const uint8_t* px = b + ii + 4;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t v = px[(size_t)y * (w / 2) + x / 2];
            std::memcpy(&t.rgba[((size_t)y * w + x) * 4], colors[(x & 1) ? (v >> 4) : (v & 15)], 4);
        }
    return true;
}

struct Catalog {
    std::vector<Model> models;
    std::vector<Tex> textures;   // MC: always empty
    Tex weaponIcon[11];
    Tex itemIcon[24];
};
Catalog g_cat;
std::atomic<int> g_state{ 0 };   // 0 idle, 2 ready, 3 failed
struct Want { const char* file; int frame; };
const Want kWant[] = {
    { "socom", 1 }, { "socom2", 1 }, { "famas", 1 }, { "grenade", 1 }, { "nikita", 1 }, { "stinger", 1 },
    { "claymore", 1 }, { "c4_bomb", 1 }, { "can_gren", 1 }, { "rifle", 1 },
    { "cigar", 0 }, { "cb_box", 0 },
};
constexpr int kNumWant = (int)(sizeof(kWant) / sizeof(kWant[0]));
int g_index[kNumWant];
int Slot(const char* file) {
    for (int i = 0; i < kNumWant; ++i) if (!std::strcmp(kWant[i].file, file)) return g_index[i];
    return -1;
}
struct Scratch { std::vector<float> z; std::vector<uint8_t> c; };
thread_local Scratch t_scr;

void RasterTri(const float a[3], const float b[3], const float c[3], const float ua[2], const float ub[2], const float uc[2],
               const Tex* tex, float shade, int W, int H, float* zb, uint8_t* cb) {
    const float area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (std::fabs(area) < 1e-6f) return;
    const int x0 = std::max(0, (int)std::floor(std::min({ a[0], b[0], c[0] })));
    const int x1 = std::min(W - 1, (int)std::ceil(std::max({ a[0], b[0], c[0] })));
    const int y0 = std::max(0, (int)std::floor(std::min({ a[1], b[1], c[1] })));
    const int y1 = std::min(H - 1, (int)std::ceil(std::max({ a[1], b[1], c[1] })));
    const float inv = 1.0f / area;
    for (int y = y0; y <= y1; ++y) {
        const float py = (float)y + 0.5f;
        for (int x = x0; x <= x1; ++x) {
            const float px = (float)x + 0.5f;
            const float w0 = ((b[0] - px) * (c[1] - py) - (b[1] - py) * (c[0] - px)) * inv;
            const float w1 = ((c[0] - px) * (a[1] - py) - (c[1] - py) * (a[0] - px)) * inv;
            const float w2 = 1.0f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            const float z = w0 * a[2] + w1 * b[2] + w2 * c[2];
            float& zd = zb[(size_t)y * W + x];
            if (z >= zd) continue;
            uint8_t r = 120, g = 205, bl = 235;   // MC: no textures (they live in the PS1 VRAM) -> hologram cyan
            if (tex && tex->w > 0) {
                const float u = w0 * ua[0] + w1 * ub[0] + w2 * uc[0];
                const float v = w0 * ua[1] + w1 * ub[1] + w2 * uc[1];
                const int tx = std::min(tex->w - 1, std::max(0, (int)(u * tex->w)));
                const int ty = std::min(tex->h - 1, std::max(0, (int)(v * tex->h)));
                const uint8_t* t = &tex->rgba[((size_t)ty * tex->w + tx) * 4];
                if (t[3] == 0) continue;   // PSX transparent texel
                r = t[0]; g = t[1]; bl = t[2];
            }
            zd = z;
            uint8_t* o = &cb[((size_t)y * W + x) * 4];
            o[0] = (uint8_t)std::min(255.0f, r * shade);
            o[1] = (uint8_t)std::min(255.0f, g * shade);
            o[2] = (uint8_t)std::min(255.0f, bl * shade);
            o[3] = 255;
        }
    }
}
} // namespace

bool Ready() { return g_state.load(std::memory_order_acquire) == 2; }
int WeaponModel(int weaponId, bool suppressed) {
    if (!Ready()) return -1;
    switch (weaponId) {
    case 0: return Slot(suppressed ? "socom2" : "socom");   // WP_Socom
    case 1: return Slot("famas");                           // WP_Famas
    case 2: return Slot("grenade");                         // WP_Grenade
    case 3: return Slot("nikita");                          // WP_Nikita
    case 4: return Slot("stinger");                         // WP_Stinger
    case 5: return Slot("claymore");                        // WP_Claymore
    case 6: return Slot("c4_bomb");                         // WP_C4
    case 7: case 8: return Slot("can_gren");                // WP_StunGrenade / WP_ChaffGrenade
    case 9: return Slot("rifle");                           // WP_Rifle (PSG1)
    default: return -1;
    }
}

int ItemModel(int itemId) {
    if (!Ready()) return -1;
    switch (itemId) {
    case 0: return Slot("cigar");                           // IT_Cigs
    case 2: case 3: case 4: return Slot("cb_box");          // IT_Box1..3
    case 18: return Slot("c4_bomb");                        // IT_TimerBomb (grenade.c GRD_TBOMB)
    default: return -1;
    }
}

bool DrawIcon(bool weapon, int id, bool suppressed, uint8_t* canvas, int canvasW, int canvasH,
              int x, int y, int w, int h, int maxScale) {
    if (!Ready() || !canvas) return false;
    const Tex* t = nullptr;
    if (weapon && id >= 0 && id < 10) t = &g_cat.weaponIcon[(id == 0 && suppressed) ? 10 : id];
    else if (!weapon && id >= 0 && id < 24) t = &g_cat.itemIcon[id];
    if (!t || t->w <= 0) return false;
    int sc = std::max(1, std::min({ maxScale, w / t->w, h / t->h }));
    float fs = (float)sc;
    if (w < t->w || h < t->h) fs = std::min((float)w / t->w, (float)h / t->h);   // shrink to fit
    const int dw = std::max(1, (int)(t->w * fs)), dh = std::max(1, (int)(t->h * fs));
    const int ox = x + (w - dw) / 2, oy = y + (h - dh) / 2;
    for (int yy = 0; yy < dh; ++yy) {
        const int py = oy + yy;
        if (py < 0 || py >= canvasH) continue;
        const int sy = std::min(t->h - 1, (int)(yy / fs));
        for (int xx = 0; xx < dw; ++xx) {
            const int px = ox + xx;
            if (px < 0 || px >= canvasW) continue;
            const uint8_t* s = &t->rgba[((size_t)sy * t->w + std::min(t->w - 1, (int)(xx / fs))) * 4];
            if (!s[3]) continue;
            uint8_t* o = &canvas[((size_t)py * canvasW + px) * 4];
            o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255;
        }
    }
    return true;
}

const char* ModelName(int model) {
    if (!Ready() || model < 0 || model >= (int)g_cat.models.size()) return "";
    return g_cat.models[model].name.c_str();
}

bool Render(int model, float spinRad, float eyeShiftRad, uint8_t* canvas, int canvasW, int canvasH,
            int x, int y, int w, int h) {
    if (!Ready() || model < 0 || model >= (int)g_cat.models.size() || !canvas) return false;
    if (w < 4 || h < 4) return false;
    const Model& m = g_cat.models[model];
    constexpr int SS = 2;
    const int W = w * SS, H = h * SS;
    Scratch& s = t_scr;
    s.z.assign((size_t)W * H, 1e30f);
    s.c.assign((size_t)W * H * 4, 0);

    const float yaw = spinRad + eyeShiftRad, tilt = -0.30f;   // a little from above
    const float cy = std::cos(yaw), sy = std::sin(yaw), ct = std::cos(tilt), st = std::sin(tilt);
    // Fit so the model never leaves the box at any turn angle: its turntable
    // radius across, its height (plus what the tilt adds) down.
    const float halfW = m.rXZ, halfH = m.rY * ct + m.rXZ * std::fabs(st);
    const float scale = 0.92f * std::min(0.5f * W / halfW, 0.5f * H / halfH);
    const float ox = W * 0.5f, oy = H * 0.5f;
    auto xf = [&](const float p[3], float o[3]) {
        const float dx = p[0] - m.c[0], dy = p[1] - m.c[1], dz = p[2] - m.c[2];
        const float x1 = cy * dx + sy * dz, z1 = -sy * dx + cy * dz;   // turntable about the vertical
        const float y2 = ct * dy - st * z1, z2 = st * dy + ct * z1;    // tilt toward the viewer
        o[0] = ox + x1 * scale; o[1] = oy + y2 * scale; o[2] = z2;
    };
    auto rot = [&](const float n[3], float o[3]) {
        const float x1 = cy * n[0] + sy * n[2], z1 = -sy * n[0] + cy * n[2];
        o[0] = x1; o[1] = ct * n[1] - st * z1; o[2] = st * n[1] + ct * z1;
    };
    const float L[3] = { -0.40f, -0.65f, -0.65f };   // light from upper left, toward the viewer
    for (const Tri& t : m.tris) {
        float a[3], b[3], c[3], n[3];
        xf(t.p[0], a); xf(t.p[1], b); xf(t.p[2], c); rot(t.nrm, n);
        const float d = std::fabs(n[0] * L[0] + n[1] * L[1] + n[2] * L[2]);   // two-sided: PSX models are not closed
        const float shade = 0.80f + 0.85f * d;
        const Tex* tx = (t.tex >= 0 && t.tex < (int)g_cat.textures.size()) ? &g_cat.textures[t.tex] : nullptr;
        RasterTri(a, b, c, t.uv[0], t.uv[1], t.uv[2], tx, shade, W, H, s.z.data(), s.c.data());
    }
    // 2x2 box down-sample, then "over" onto the canvas.
    for (int yy = 0; yy < h; ++yy) {
        const int dy = y + yy;
        if (dy < 0 || dy >= canvasH) continue;
        for (int xx = 0; xx < w; ++xx) {
            const int dx = x + xx;
            if (dx < 0 || dx >= canvasW) continue;
            int acc[4] = { 0, 0, 0, 0 };
            for (int j = 0; j < SS; ++j) for (int i = 0; i < SS; ++i) {
                const uint8_t* p = &s.c[(((size_t)(yy * SS + j)) * W + (xx * SS + i)) * 4];
                if (!p[3]) continue;
                acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += 1;
            }
            if (!acc[3]) continue;
            const float a = (float)acc[3] / (SS * SS);
            uint8_t* o = &canvas[((size_t)dy * canvasW + dx) * 4];
            for (int k = 0; k < 3; ++k) {
                const float src = (float)acc[k] / (float)acc[3];
                o[k] = (uint8_t)(src * a + o[k] * (1.0f - a) + 0.5f);
            }
            o[3] = (uint8_t)std::max<int>(o[3], (int)(255 * a));
        }
    }
    return true;
}

// MC: find the GV cache table (128 x {id, buf}, an entry usually sits at id % 128) in a 2 MB PS1 RAM copy.
uint32_t FindCacheTable(const uint8_t* ram) {
    uint32_t best = 0; int bestHits = 0;
    for (uint32_t o = 0x90000; o + 128 * 8 <= 0xC0000; o += 8) {
        int hits = 0, bad = 0;
        for (int i = 0; i < 128 && bad < 6; ++i) {
            uint32_t id = U32(ram + o + i * 8), buf = U32(ram + o + i * 8 + 4);
            if (!id && !buf) continue;
            if ((buf >> 21) != (0x80000000u >> 21) || (id >> 25)) { ++bad; continue; }
            if ((int)((id & 0xFFFFFF) % 128) == i) ++hits;
        }
        if (bad < 6 && hits > bestHits) { bestHits = hits; best = 0x80000000u | o; }
    }
    return bestHits >= 16 ? best : 0;
}
static uint32_t CacheGet(const uint8_t* ram, uint32_t table, const char* name, char spec) {
    uint32_t want = StrCode(name) | ((uint32_t)(spec - 'a') << 16);
    for (int i = 0; i < 128; ++i) {
        uint32_t id = U32(ram + (table & 0x1FFFFF) + i * 8), buf = U32(ram + (table & 0x1FFFFF) + i * 8 + 4);
        if ((id & 0xFFFFFF) == want && (buf >> 21) == (0x80000000u >> 21)) return buf;
    }
    return 0;
}
// v17: the game's own item / weapon descriptions (menu/item.c itm_descriptions, menu/weapon.c wpn_descriptions): two
// pointer tables in the game's data (US 1.0: items 0x800A0B30, 24 + 2 frozen variants; weapons 0x800A0D1C, 10), found by
// shape: a run of pointers to strings that start with the title code B0 14. Text is ASCII with two-byte codes:
// B0 14 title start, D0 15 title end, 80 7C new line, 80 80 / 80 81 / 80 00 the circle / cross / square button glyphs,
// C0 xx a full-width character, 81 xx a font switch (skipped).
char g_desc[2][24][192];
static void DecodeDesc(const uint8_t* s, size_t max, char* out, int cap)
{
    int o = 0; size_t i = 0;
    auto put = [&](const char* t) { while (*t && o < cap - 1) out[o++] = *t++; };
    while (i < max && o < cap - 1) {
        uint8_t b = s[i];
        if (b == 0) break;
        if (b < 0x80) { char t[2] = {(char)b, 0}; put(t); ++i; continue; }
        if (i + 1 >= max) break;
        uint8_t c = s[i + 1]; i += 2;
        if (b == 0xD0 && c == 0x15) put("\n");
        else if (b == 0x80 && c == 0x7C) put("\n");
        else if (b == 0x80 && c == 0x80) put("(O)");
        else if (b == 0x80 && c == 0x81) put("(X)");
        else if (b == 0x80 && c == 0x00) put("[]");
        else if (b == 0xC0 && c >= 0x20 && c < 0x7F) { char t[2] = {(char)c, 0}; put(t); }
    }
    out[o] = 0;
    // collapse "\n " and double new lines
    int w = 0; for (int r = 0; out[r]; ++r) { if (out[r] == '\n' && w > 0 && out[w - 1] == '\n') continue; out[w++] = out[r]; } out[w] = 0;
}
static int LoadDescs(const uint8_t* ram)
{
    memset(g_desc, 0, sizeof g_desc);
    auto ok = [&](uint32_t a) { uint32_t o = a & 0x1FFFFF; return (a >> 21) == 0x400 && o + 2 < 0x200000 && ram[o] == 0xB0 && ram[o + 1] == 0x14; };
    uint32_t items = 0, weapons = 0;
    for (uint32_t b = 0x10000; b + 4 * 26 < 0x200000; b += 4) {
        int n = 0; while (n < 30 && ok(U32(ram + b + 4 * n))) ++n;
        if (n >= 24 && !items) items = b; else if (n >= 10 && n < 24 && !weapons) weapons = b;
        if (n) b += 4 * (n - 1);
        if (items && weapons) break;
    }
    int got = 0;
    for (int w = 0; w < 2; ++w) {
        uint32_t t = w ? weapons : items; if (!t) continue;
        for (int k = 0; k < (w ? 10 : 24); ++k) {
            uint32_t a = U32(ram + t + 4 * k) & 0x1FFFFF;
            DecodeDesc(ram + a, 0x200000 - a > 400 ? 400 : 0x200000 - a, g_desc[w][k], 192);
            if (g_desc[w][k][0]) ++got;
        }
    }
    return got;
}
const char* Desc(bool weapon, int id) { return (id >= 0 && id < (weapon ? 10 : 24)) ? g_desc[weapon ? 1 : 0][id] : ""; }
// Loads every model and icon from a 2 MB snapshot of the PS1 RAM. Returns a one-line report.
bool LoadFromRam(const uint8_t* ram, char* report, size_t rn) {
    uint32_t table = FindCacheTable(ram);
    if (!table) { snprintf(report, rn, "GV cache table not found in the PS1 RAM"); g_state = 3; return false; }
    Catalog cat; int nm = 0;
    for (int i = 0; i < kNumWant; ++i) {
        g_index[i] = -1;
        uint32_t a = CacheGet(ram, table, kWant[i].file, 'k');
        if (!a) continue;
        uint32_t base = a & 0x1FFFFF; size_t n = 0x200000 - base; if (n > 0x40000) n = 0x40000;
        std::vector<uint8_t> d(ram + base, ram + base + n);
        int nModels = (int)U32(d.data());
        if (nModels <= 0 || nModels > 64 || 32 + (size_t)nModels * 88 > n) continue;
        for (int mi = 0; mi < nModels; ++mi)   // relocated pointers (verts, indices, normals, uvs, texture ids) -> offsets
            for (int f = 56; f <= 84; f += 4) {
                uint8_t* p = &d[32 + (size_t)mi * 88 + f]; uint32_t v = U32(p);
                if (v >= (0x80000000u | base) && v < 0x80200000u) { v -= 0x80000000u | base; memcpy(p, &v, 4); }
            }
        Model m; m.name = kWant[i].file;
        std::vector<uint16_t> noTex;
        if (ParseKmd(d.data(), n, m, noTex, kWant[i].frame)) { g_index[i] = (int)cat.models.size(); cat.models.push_back(std::move(m)); ++nm; }
    }
    int nIcons = 0;
    if (uint32_t r = CacheGet(ram, table, "item", 'r')) {
        uint32_t base = r & 0x1FFFFF; size_t n = 0x200000 - base; if (n > 0x20000) n = 0x20000;
        std::vector<uint8_t> b(ram + base, ram + base + n);
        const int count = b[0] + b[1]; const uint32_t data = 0x80000000u | (base + 4 + count * 4);
        if (4 + (size_t)count * 4 <= n)
            for (int k = 0; k < count; ++k) {   // menu_rpk_init turned the offsets into pointers
                uint8_t* p = &b[4 + k * 4]; uint32_t v = U32(p);
                if (v >= data && v < 0x80200000u) { v -= data; memcpy(p, &v, 4); }
            }
        static const int kWpImg[10] = { 1, 3, 5, 10, 9, 8, 7, 6, 11, 4 };
        static const int kItImg[24] = { 14, 30, 17, 17, 17, 15, 16, 19, 18, 25, 32, 31, 12, 22, 21, 21, 23, 27, 26, 20, 28, 24, 29, 13 };
        for (int i = 0; i < 10; ++i) nIcons += DecodeRpkIcon(b.data(), n, kWpImg[i], 0, cat.weaponIcon[i]);
        nIcons += DecodeRpkIcon(b.data(), n, 2, 0, cat.weaponIcon[10]);
        for (int i = 0; i < 24; ++i) { const int img = kItImg[i]; const int pal = img == 22 ? 46 : (img == 27 ? 47 : 0);
                                        nIcons += DecodeRpkIcon(b.data(), n, img, pal, cat.itemIcon[i]); }
    }
    int nDesc = LoadDescs(ram);
    snprintf(report, rn, "GV cache at %08X: %d of %d models, %d of 35 menu icons, %d of 34 descriptions (untextured: textures are in the PS1 VRAM)",
             table, nm, kNumWant, nIcons, nDesc);
    g_cat = std::move(cat);
    g_state = (nm > 0 || nIcons > 0) ? 2 : 3;
    return Ready();
}
} // namespace GameModels

namespace WristWheel {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr int   kRing = 182;              // radius of the slot centres
constexpr int   kInner = 118;             // radius of the centre disc

thread_local uint8_t* g_px = nullptr;     // current canvas (rows 0..kH-1)

void Put(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || y < 0 || x >= kW || y >= kH) return;
    uint8_t* p = &g_px[((size_t)y * kW + x) * 4];
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}
void Fill(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int yy = y; yy < y + h; ++yy) for (int xx = x; xx < x + w; ++xx) Put(xx, yy, r, g, b, a);
}
// Anti-aliased-ish filled disc / ring band (coverage from distance).
void Disc(float cx, float cy, float r0, float r1, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const int x0 = (int)std::floor(cx - r1 - 1), x1 = (int)std::ceil(cx + r1 + 1);
    const int y0 = (int)std::floor(cy - r1 - 1), y1 = (int)std::ceil(cy + r1 + 1);
    for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) {
        if (x < 0 || y < 0 || x >= kW || y >= kH) continue;
        const float d = std::sqrt((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
        float cov = std::min(1.0f, std::max(0.0f, r1 - d + 0.5f)) * std::min(1.0f, std::max(0.0f, d - r0 + 0.5f));
        if (r0 <= 0.0f) cov = std::min(1.0f, std::max(0.0f, r1 - d + 0.5f));
        if (cov <= 0.0f) continue;
        uint8_t* p = &g_px[((size_t)y * kW + x) * 4];
        const float k = cov * (a / 255.0f);
        // "over" onto whatever is there, keeping the canvas alpha meaningful
        const float da = p[3] / 255.0f, oa = k + da * (1.0f - k);
        if (oa <= 0.0f) continue;
        p[0] = (uint8_t)((r * k + p[0] * da * (1.0f - k)) / oa + 0.5f);
        p[1] = (uint8_t)((g * k + p[1] * da * (1.0f - k)) / oa + 0.5f);
        p[2] = (uint8_t)((b * k + p[2] * da * (1.0f - k)) / oa + 0.5f);
        p[3] = (uint8_t)(oa * 255.0f + 0.5f);
    }
}

// ---- 5x7 font (same glyphs as the wrist panels) ------------------------------
const char* Glyph(char c) {
    switch (c) {
    case 'A': return "01110100011000111111100011000110001";
    case 'B': return "11110100011000111110100011000111110";
    case 'C': return "01110100011000010000100001000101110";
    case 'D': return "11110100011000110001100011000111110";
    case 'E': return "11111100001000011110100001000011111";
    case 'F': return "11111100001000011110100001000010000";
    case 'G': return "01110100011000010111100011000101111";
    case 'H': return "10001100011000111111100011000110001";
    case 'I': return "01110001000010000100001000010001110";
    case 'J': return "00111000100001000010000101001001100";
    case 'K': return "10001100101010011000101001001010001";
    case 'L': return "10000100001000010000100001000011111";
    case 'M': return "10001110111010110101100011000110001";
    case 'N': return "10001110011010110011100011000110001";
    case 'O': return "01110100011000110001100011000101110";
    case 'P': return "11110100011000111110100001000010000";
    case 'Q': return "01110100011000110001101011001001101";
    case 'R': return "11110100011000111110101001001010001";
    case 'S': return "01111100001000001110000010000111110";
    case 'T': return "11111001000010000100001000010000100";
    case 'U': return "10001100011000110001100011000101110";
    case 'V': return "10001100011000110001100010101000100";
    case 'W': return "10001100011000110101101011010101010";
    case 'X': return "10001100010101000100010101000110001";
    case 'Y': return "10001100010101000100001000010000100";
    case 'Z': return "11111000010001000100010001000011111";
    case '0': return "01110100011001110101110011000101110";
    case '1': return "00100011000010000100001000010001110";
    case '2': return "01110100010000100010001000100011111";
    case '3': return "11111000100010000010000011000101110";
    case '4': return "00010001100101010010111110001000010";
    case '5': return "11111100001111000001000011000101110";
    case '6': return "00110010001000011110100011000101110";
    case '7': return "11111000010001000100010000100001000";
    case '8': return "01110100011000101110100011000101110";
    case '9': return "01110100011000101111000010001001100";
    case '.': return "00000000000000000000000000110001100";
    case '-': return "00000000000000011111000000000000000";
    case '/': return "00000000010001000100010001000000000";
    case ':': return "00000011000110000000011000110000000";
    case '+': return "00000001000010011111001000010000000";
    case '<': return "00010001000100010000010000010000010";
    case '>': return "01000001000001000001000100010001000";
    case ',': return "00000000000000000000001100010001000";
    case '\'': return "01100001000100000000000000000000000";
    case '!': return "00100001000010000100001000000000100";
    case '(': return "00010001000100001000010000010000010";
    case ')': return "01000001000001000010000100010001000";
    case '?': return "01110100010000100010001000000000100";
    case '%': return "11000110010001000100010001001100011";
    default:  return nullptr;
    }
}
int TextW(const char* s, int scale) { return s && *s ? ((int)std::strlen(s) * 6 - 1) * scale : 0; }
// Text with a dark outline so it reads over anything; (x, y) = top-left.
void Text(int x, int y, const char* s, int scale, uint8_t r, uint8_t g, uint8_t b) {
    for (int pass = 0; pass < 2; ++pass) {
        int cx = x;
        for (const char* c = s; *c; ++c) {
            const char ch = (*c >= 'a' && *c <= 'z') ? (char)(*c - 32) : *c;
            if (const char* rows = Glyph(ch)) {
                for (int gy = 0; gy < 7; ++gy) for (int gx = 0; gx < 5; ++gx) {
                    if (rows[gy * 5 + gx] != '1') continue;
                    if (pass == 0) Fill(cx + gx * scale - 1, y + gy * scale - 1, scale + 2, scale + 2, 0, 0, 0, 255);
                    else           Fill(cx + gx * scale, y + gy * scale, scale, scale, r, g, b, 255);
                }
            }
            cx += 6 * scale;
        }
    }
}
void TextC(int cx, int y, const char* s, int scale, uint8_t r, uint8_t g, uint8_t b) {
    Text(cx - TextW(s, scale) / 2, y, s, scale, r, g, b);
}

void SlotPos(int count, int k, float& x, float& y) {
    const float a = (count > 0 ? (float)k / (float)count : 0.0f) * 2.0f * kPi;   // clockwise from the top
    x = kCX + kRing * std::sin(a);
    y = kCY - kRing * std::cos(a);
}

} // namespace

int SlotAt(int count, float x, float y) {
    if (count <= 0) return -1;
    const float r = std::sqrt(x * x + y * y);
    if (r < (float)kInner / kDiscR * 0.92f || r > 1.08f) return -1;
    float a = std::atan2(x, y);                 // 0 = top, clockwise
    if (a < 0) a += 2.0f * kPi;
    return (int)std::lround(a / (2.0f * kPi) * count) % count;
}

int SlotForStick(int count, float x, float y, float deadzone) {
    if (count <= 0 || x * x + y * y < deadzone * deadzone) return -1;
    float a = std::atan2(x, y);
    if (a < 0) a += 2.0f * kPi;
    return (int)std::lround(a / (2.0f * kPi) * count) % count;
}

bool Draw(const View& v, uint8_t* rgba) {
    g_px = rgba;
    std::memset(rgba, 0, (size_t)kW * kH * 4);
    const int n = std::max(0, std::min(v.count, kMaxEntries));

    // Backdrop: a dark glass disc, a faint track for the ring, the centre.
    Disc((float)kCX, (float)kCY, 0.0f, (float)kDiscR, 6, 10, 14, 170);
    Disc((float)kCX, (float)kCY, kRing - 1.5f, kRing + 1.5f, 70, 120, 110, 160);
    Disc((float)kCX, (float)kCY, 0.0f, (float)kInner, 0, 0, 0, 120);

    // Slot size from the spacing round the ring.
    const float spacing = n > 0 ? 2.0f * kPi * kRing / n : 100.0f;
    const float slotR = std::min(40.0f, spacing * 0.5f - 3.0f);
    // Names round the ring: all of them while they fit (bigger for a short
    // ring), otherwise the highlighted one and its two neighbours each side.
    const int labelScale = n <= 16 ? 2 : 1;
    auto labelled = [&](int k) {
        if (!v.labels) return false;
        if (n <= 20) return true;
        const int d = std::min((k - v.sel + n) % n, (v.sel - k + n) % n);
        return d <= 2;
    };

    for (int k = 0; k < n; ++k) {
        float x, y; SlotPos(n, k, x, y);
        const bool hi = (k == v.sel);
        const float r = hi ? slotR + 5.0f : slotR;
        if (hi) Disc(x, y, 0.0f, r + 4.0f, 90, 255, 150, 255);                       // highlight ring
        Disc(x, y, 0.0f, r, hi ? 30 : 16, hi ? 60 : 26, hi ? 44 : 30, 235);
        if (v.e[k].equipped) Disc(x, y, r - 3.0f, r, 255, 210, 80, 255);             // equipped: gold rim
        const int box = (int)(r * 1.5f);
        if (v.e[k].id < 0) {
            // NONE: a struck-through circle
            Disc(x, y, r * 0.42f, r * 0.55f, 200, 200, 200, 230);
            for (int t = -(int)(r * 0.5f); t <= (int)(r * 0.5f); ++t)
                for (int w = -1; w <= 1; ++w) Put((int)x + t + w, (int)y - t, 200, 200, 200, 230);
        }
        else if (!GameModels::DrawIcon(v.weapons, v.e[k].id, v.suppressed, rgba, kW, kH,
                                       (int)x - box / 2, (int)y - box / 2, box, box, 2)) {
            TextC((int)x, (int)y - 4, v.e[k].name, 1, 220, 220, 220);
        }
        if (labelled(k)) {
            // Name outside the wheel, along the spoke: right half grows
            // rightward, left half leftward, top / bottom centred.
            const int ls = labelScale;
            const float a = (float)k / n * 2.0f * kPi;
            const float sa = std::sin(a), ca = std::cos(a);
            const float lr = (float)kDiscR + 8.0f;
            const float lx = kCX + lr * sa, lyc = kCY - lr * ca;
            const int tw = TextW(v.e[k].name, ls), th = 7 * ls;
            int tx, ty;
            if (sa > 0.25f)       { tx = (int)lx;           ty = (int)lyc - th / 2; }
            else if (sa < -0.25f) { tx = (int)lx - tw;      ty = (int)lyc - th / 2; }
            else                  { tx = (int)lx - tw / 2;  ty = ca > 0 ? (int)lyc - th : (int)lyc; }
            tx = std::max(2, std::min(kW - tw - 2, tx));
            ty = std::max(2, std::min(kH - th - 2, ty));
            Text(tx, ty, v.e[k].name, ls, hi ? 140 : 215, hi ? 255 : 220, hi ? 180 : 220);
        }
    }

    // Centre: model (or a big icon), name, ammo / count.
    int model = -1;
    const Entry* cur = (n > 0 && v.sel >= 0 && v.sel < n) ? &v.e[v.sel] : nullptr;
    const int mx = kCX - 96, my = kCY - 92, mw = 192, mh = 104;
    if (cur && cur->id >= 0) {
        model = v.weapons ? GameModels::WeaponModel(cur->id, v.suppressed) : GameModels::ItemModel(cur->id);
        if (model < 0) GameModels::DrawIcon(v.weapons, cur->id, v.suppressed, rgba, kW, kH, mx, my, mw, mh, 4);
    }
    if (cur) {
        const char* nm = cur->id < 0 ? "NONE" : cur->name;
        int sc = 3;
        while (sc > 1 && TextW(nm, sc) > 2 * kInner - 24) --sc;
        TextC(kCX, kCY + 18, nm, sc, 255, 255, 255);
        if (cur->detail[0]) TextC(kCX, kCY + 18 + 7 * sc + 8, cur->detail, 2, 255, 220, 120);
    }
    if (v.hint) TextC(kCX, kCY + kInner - 22, v.hint, 1, 170, 200, 190);
    TextC(kCX, kCY - kInner + 14, v.weapons ? "WEAPON" : "ITEM", 2, 120, 200, 255);

    if (v.cursorOn) {
        Disc((float)v.cursorX, (float)v.cursorY, 0.0f, 7.0f, 0, 0, 0, 255);
        Disc((float)v.cursorX, (float)v.cursorY, 0.0f, 5.0f, 140, 255, 180, 255);
    }

    // Right eye: same picture, then the model drawn per eye.
    uint8_t* right = rgba + (size_t)kW * kH * 4;
    std::memcpy(right, rgba, (size_t)kW * kH * 4);
    if (model < 0) return false;
    const float half = 0.5f * std::max(0.0f, v.stereoDeg) * kPi / 180.0f;
    GameModels::Render(model, v.spinRad, -half, rgba, kW, kH, mx, my, mw, mh);
    GameModels::Render(model, v.spinRad, half, right, kW, kH, mx, my, mw, mh);
    return half > 0.0f;
}
// ---- CAROUSEL ------------------------------------------------------------------
namespace {
thread_local uint8_t* g_cv = nullptr; thread_local int g_cvW = 0, g_cvH = 0;   // canvas for the carousel helpers (worker thread)
void CPut(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || y < 0 || x >= g_cvW || y >= g_cvH) return;
    uint8_t* p = &g_cv[((size_t)y * g_cvW + x) * 4];
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}
// Blend "over" with coverage k (0..1).
void CBlend(int x, int y, uint8_t r, uint8_t g, uint8_t b, float k) {
    if (x < 0 || y < 0 || x >= g_cvW || y >= g_cvH || k <= 0.0f) return;
    uint8_t* p = &g_cv[((size_t)y * g_cvW + x) * 4];
    k = std::min(1.0f, k);
    const float da = p[3] / 255.0f, oa = k + da * (1.0f - k);
    if (oa <= 0.0f) return;
    p[0] = (uint8_t)((r * k + p[0] * da * (1.0f - k)) / oa + 0.5f);
    p[1] = (uint8_t)((g * k + p[1] * da * (1.0f - k)) / oa + 0.5f);
    p[2] = (uint8_t)((b * k + p[2] * da * (1.0f - k)) / oa + 0.5f);
    p[3] = (uint8_t)(oa * 255.0f + 0.5f);
}
void CText(int x, int y, const char* s, int scale, uint8_t r, uint8_t g, uint8_t b) {
    for (int pass = 0; pass < 2; ++pass) {
        int cx = x;
        for (const char* c = s; *c; ++c) {
            const char ch = (*c >= 'a' && *c <= 'z') ? (char)(*c - 32) : *c;
            if (const char* rows = Glyph(ch)) {
                for (int gy = 0; gy < 7; ++gy) for (int gx = 0; gx < 5; ++gx) {
                    if (rows[gy * 5 + gx] != '1') continue;
                    const int o = pass == 0 ? 1 : 0;
                    for (int yy = -o; yy < scale + o; ++yy) for (int xx = -o; xx < scale + o; ++xx)
                        if (pass == 0) CPut(cx + gx * scale + xx, y + gy * scale + yy, 0, 0, 0, 255);
                        else           CPut(cx + gx * scale + xx, y + gy * scale + yy, r, g, b, 255);
                }
            }
            cx += 6 * scale;
        }
    }
}
void CTextC(int cx, int y, const char* s, int scale, uint8_t r, uint8_t g, uint8_t b) {
    CText(cx - TextW(s, scale) / 2, y, s, scale, r, g, b);
}
// Filled ellipse band with soft edges.
void CEllipse(float cx, float cy, float rx, float ry, float inner, uint8_t r, uint8_t g, uint8_t b, float a) {
    for (int y = (int)(cy - ry - 2); y <= (int)(cy + ry + 2); ++y)
        for (int x = (int)(cx - rx - 2); x <= (int)(cx + rx + 2); ++x) {
            const float dx = (x + 0.5f - cx) / rx, dy = (y + 0.5f - cy) / ry;
            const float d = std::sqrt(dx * dx + dy * dy);
            float cov = std::min(1.0f, std::max(0.0f, (1.0f - d) * rx * 0.7f));
            if (inner > 0.0f) cov *= std::min(1.0f, std::max(0.0f, (d - inner) * rx * 0.7f));
            CBlend(x, y, r, g, b, cov * a);
        }
}
void CTriangle(int cx, int cy, int size, bool right, uint8_t r, uint8_t g, uint8_t b) {
    for (int dy = -size; dy <= size; ++dy) {
        const int w = size - std::abs(dy);
        for (int dx = 0; dx <= w; ++dx) CPut(right ? cx + dx : cx - dx, cy + dy, r, g, b, 255);
    }
}
} // namespace

void DrawCarouselTile(const Entry& e, bool weapons, bool suppressed, bool focused,
                      float spinRad, float eyeShiftRad, uint8_t* atlas, int x0, int y0) {
    // Clear the tile.
    for (int y = 0; y < kTile; ++y) std::memset(&atlas[((size_t)(y0 + y) * kAtlasW + x0) * 4], 0, (size_t)kTile * 4);
    // Work in a tile-sized view of the atlas: a sub-canvas via offsets.
    g_cv = atlas; g_cvW = kAtlasW; g_cvH = kAtlasH;
    const float cx = x0 + kTile * 0.5f;
    const float baseY = y0 + kTile * 0.80f;
    const uint8_t hr = focused ? 90 : 60, hg = focused ? 230 : 190, hb = 255;
    // Projection beam: a faint cyan glow rising from the plinth.
    for (int y = (int)(y0 + kTile * 0.14f); y < (int)baseY; ++y) {
        const float t = (y - (y0 + kTile * 0.14f)) / (baseY - (y0 + kTile * 0.14f));   // 0 top .. 1 base
        const float half = kTile * (0.20f + 0.18f * t);
        for (int x = (int)(cx - half); x <= (int)(cx + half); ++x) {
            const float edge = 1.0f - std::fabs(x + 0.5f - cx) / half;
            CBlend(x, y, hr, hg, hb, (focused ? 0.16f : 0.09f) * t * edge);
        }
    }
    // Plinth: a glowing disc seen from slightly above, with a bright rim.
    CEllipse(cx, baseY, kTile * 0.36f, kTile * 0.075f, 0.0f, hr, hg, hb, focused ? 0.35f : 0.22f);
    CEllipse(cx, baseY, kTile * 0.36f, kTile * 0.075f, 0.86f, hr, hg, hb, focused ? 0.95f : 0.6f);

    // The model, or the game's icon, or NONE.
    const int mx = x0 + 10, my = y0 + 14, mw = kTile - 20, mh = (int)(kTile * 0.66f);
    bool drawn = false;
    if (e.id >= 0) {
        const int model = weapons ? GameModels::WeaponModel(e.id, suppressed) : GameModels::ItemModel(e.id);
        if (model >= 0)
            drawn = GameModels::Render(model, focused ? spinRad : 0.65f, eyeShiftRad, atlas, kAtlasW, kAtlasH, mx, my, mw, mh);
        if (!drawn)
            drawn = GameModels::DrawIcon(weapons, e.id, suppressed, atlas, kAtlasW, kAtlasH, mx + 10, my + 20, mw - 20, mh - 30, 4);
    }
    if (e.id < 0 || !drawn) {
        // NONE (or anything missing): a hologram "empty" mark.
        const float r = kTile * 0.20f, ccy = my + mh * 0.55f;
        CEllipse(cx, ccy, r, r, 0.78f, hr, hg, hb, 0.95f);
        for (int t = -(int)(r * 0.7f); t <= (int)(r * 0.7f); ++t)
            for (int w = -2; w <= 2; ++w) CBlend((int)cx + t + w, (int)ccy - t, hr, hg, hb, 0.95f);
    }
    // Equipped: a gold diamond in the corner.
    if (e.equipped) {
        const int ex = x0 + kTile - 22, ey = y0 + 18;
        for (int dy = -9; dy <= 9; ++dy) for (int dx = -(9 - std::abs(dy)); dx <= 9 - std::abs(dy); ++dx)
            CPut(ex + dx, ey + dy, 255, 205, 70, 255);
    }
    // Small name under the plinth on the side tiles (the focused one has the plate).
    if (!focused) {
        const char* nm = e.id < 0 ? "NONE" : e.name;
        int sc = 2;
        if (TextW(nm, sc) > kTile - 8) sc = 1;
        CTextC((int)cx, (int)(baseY + kTile * 0.09f), nm, sc, 170, 230, 255);
    }
}

void DrawCarouselLabel(const Entry& e, const char* hint, bool canLeft, bool canRight, uint8_t* atlas, int y0) {
    g_cv = atlas; g_cvW = kAtlasW; g_cvH = kAtlasH;
    for (int y = 0; y < kLabelH; ++y) std::memset(&atlas[((size_t)(y0 + y) * kAtlasW) * 4], 0, (size_t)kAtlasW * 4);
    // Glass plate with cyan edges.
    const int px0 = 40, px1 = kAtlasW - 40, py0 = y0 + 6, py1 = y0 + kLabelH - 6;
    for (int y = py0; y < py1; ++y) for (int x = px0; x < px1; ++x) {
        const bool edge = (y < py0 + 3 || y >= py1 - 3);
        if (edge) CPut(x, y, 90, 230, 255, 220);
        else      CPut(x, y, 4, 14, 20, 190);
    }
    // Split the game's description: first line is the full name.
    char lines[6][64] = {};
    int nl = 0;
    if (e.id >= 0 && e.desc[0]) {
        const char* p = e.desc;
        while (*p && nl < 6) {
            int k = 0;
            while (*p && *p != '\n' && k < 63) lines[nl][k++] = *p++;
            lines[nl][k] = 0;
            while (*p && *p != '\n') ++p;
            if (*p == '\n') ++p;
            if (k > 0) ++nl;
        }
    }
    const char* nm = e.id < 0 ? "NONE" : (nl > 0 ? lines[0] : e.name);
    const int textW = px1 - px0 - 150;
    int sc = 5;
    while (sc > 2 && TextW(nm, sc) > textW) --sc;
    int y = y0 + 18;
    // Name, with ammo / count to its right in gold.
    const int nw = TextW(nm, sc), dw = e.detail[0] ? TextW(e.detail, 4) + 24 : 0;
    const int nx = kAtlasW / 2 - (nw + dw) / 2;
    CText(nx, y, nm, sc, 255, 255, 255);
    if (e.detail[0]) CText(nx + nw + 24, y + (7 * sc - 7 * 4) / 2, e.detail, 4, 255, 215, 110);
    y += 7 * sc + 16;
    // Description lines.
    for (int i = 1; i < nl; ++i) {
        int ds = 3;
        while (ds > 1 && TextW(lines[i], ds) > textW) --ds;
        CTextC(kAtlasW / 2, y, lines[i], ds, 175, 225, 240);
        y += 7 * ds + 8;
        if (y > py1 - 40) break;
    }
    if (hint) CTextC(kAtlasW / 2, py1 - 24, hint, 2, 110, 170, 190);
    if (canLeft)  CTriangle(px0 + 34, y0 + kLabelH / 2, 24, false, 90, 230, 255);
    if (canRight) CTriangle(px1 - 34, y0 + kLabelH / 2, 24, true, 90, 230, 255);
}
} // namespace WristWheel

static std::atomic<int> g_wheel_state{0};   // 0 not tried, 2 models ready, 3 failed (XR thread)
static std::atomic<bool> g_sel_open{false};       // v16: a wrist selection is open (XR -> main: pause while choosing)
static std::atomic<int>  g_view_mode{-1};          // v17: 0 VR, 1 diorama, 2 flat screen (-1 = not set yet: from the ini)
static std::atomic<ULONGLONG> g_mc_menu_until{0};  // v17: hold E + M (DirectInput codes 0x12 / 0x32) until this tick
static const char* const kViewName[3] = {"VR (Snake's eyes)", "DIORAMA", "FLAT SCREEN (the game untouched)"};
static std::atomic<bool> g_sel_pause_owned{false};
static std::atomic<double> g_hand_hold_until{0};       // v19: mono until (ms, qpc) while a controller moves fast
static std::atomic<uint32_t> g_hold_hand{0};
static XrPosef g_hand[2]; static bool g_hand_ok[2] = {false, false};   // XR thread only
static void quat_rot(const XrQuaternionf& q, const float v[3], float o[3])
{
    float x = q.x, y = q.y, z = q.z, w = q.w;
    float tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    o[0] = v[0] + w * tx + (y * tz - z * ty); o[1] = v[1] + w * ty + (z * tx - x * tz); o[2] = v[2] + w * tz + (x * ty - y * tx);
}
static XrQuaternionf quat_mul(const XrQuaternionf& a, const XrQuaternionf& b)
{
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
// GOG WheelPoseFor: above the wrist, nudged 3 cm toward you, upright, facing your eyes
static XrPosef wheel_pose(const XrPosef& grip, const XrPosef& head, float height_m)
{
    XrPosef p;
    float c[3] = { grip.position.x, grip.position.y + height_m, grip.position.z };
    float th[3] = { head.position.x - c[0], 0.0f, head.position.z - c[2] };
    float hl = sqrtf(th[0] * th[0] + th[2] * th[2]);
    if (hl > 1e-3f) { c[0] += th[0] / hl * 0.03f; c[2] += th[2] / hl * 0.03f; }
    p.position = { c[0], c[1], c[2] };
    float Z[3] = { head.position.x - c[0], head.position.y - c[1], head.position.z - c[2] };
    float zl = sqrtf(Z[0] * Z[0] + Z[1] * Z[1] + Z[2] * Z[2]); if (zl < 1e-3f) { Z[0] = 0; Z[1] = 0; Z[2] = 1; zl = 1; }
    for (float& v : Z) v /= zl;
    float X[3] = { Z[2], 0.0f, -Z[0] }; float xl = sqrtf(X[0] * X[0] + X[2] * X[2]); if (xl < 1e-3f) { X[0] = 1; X[2] = 0; xl = 1; }
    X[0] /= xl; X[2] /= xl;
    const float Y[3] = { Z[1] * X[2] - Z[2] * X[1], Z[2] * X[0] - Z[0] * X[2], Z[0] * X[1] - Z[1] * X[0] };
    // rotation matrix columns X, Y, Z -> quaternion
    float m00 = X[0], m11 = Y[1], m22 = Z[2], tr = m00 + m11 + m22; XrQuaternionf q;
    if (tr > 0) { float sq = sqrtf(tr + 1.0f) * 2; q.w = 0.25f * sq; q.x = (Y[2] - Z[1]) / sq; q.y = (Z[0] - X[2]) / sq; q.z = (X[1] - Y[0]) / sq; }
    else if (m00 > m11 && m00 > m22) { float sq = sqrtf(1.0f + m00 - m11 - m22) * 2; q.w = (Y[2] - Z[1]) / sq; q.x = 0.25f * sq; q.y = (Y[0] + X[1]) / sq; q.z = (Z[0] + X[2]) / sq; }
    else if (m11 > m22) { float sq = sqrtf(1.0f + m11 - m00 - m22) * 2; q.w = (Z[0] - X[2]) / sq; q.x = (Y[0] + X[1]) / sq; q.y = 0.25f * sq; q.z = (Z[1] + Y[2]) / sq; }
    else { float sq = sqrtf(1.0f + m22 - m00 - m11) * 2; q.w = (X[1] - Y[0]) / sq; q.x = (Z[0] + X[2]) / sq; q.y = (Z[1] + Y[2]) / sq; q.z = 0.25f * sq; }
    p.orientation = q;
    return p;
}
struct CarState {   // v16 carousel (XR thread only)
    bool open = false, shown = false, armed = true, had_prev = false, have_img = false, right_eye = false;
    int side = -1, epoch = 0, target = 0, last_pick = -1, drawn_epoch = -1, focus = 0, stick_dir = 0;
    double last_stick = 0; float pos = 0, vx = 0, prev_x = 0, radius = 0.18f; XrPosef pose{{0, 0, 0, 1}, {0, 0, 0}};
    int slot_entry[WristWheel::kTileSlots] = {}, slot_of[WristWheel::kMaxEntries] = {}; bool slot_spun[WristWheel::kTileSlots] = {};
    std::vector<uint8_t> atlas;
};
static CarState g_car;

// v10 wrist quick-select state (XR thread), as the GOG mod's WristList
struct WristList { bool open = false, navigated = false, used = false; double opened_at = 0, last_step = 0; int step_dir = 0;
                   int ids[26] = {}; int count = 0, sel = 0, last_equipped = -1; bool prev = false; };
static WristList g_wl[2];                          // 0 = items (left grip), 1 = weapons (right grip)
static std::atomic<int> g_equip_req{0};            // XR -> writer: ((side + 1) << 16) | (uint16_t)id
static std::atomic<bool> g_hud_ctx{false};          // XR thread: wrist panels are up (world-locked gameplay view)
static const char* const kGogWeapon[10] = {"SOCOM", "FA-MAS", "GRENADE", "NIKITA", "STINGER", "CLAYMORE", "C4", "STUN.G", "CHAFF.G", "PSG1"};
static const char* const kGogItem[24] = {"CIGS", "SCOPE", "C.BOX A", "C.BOX B", "C.BOX C", "N.V.G", "THERM.G", "GASMASK",
    "B.ARMOR", "KETCHUP", "STEALTH", "BANDANA", "CAMERA", "RATION", "MEDICINE", "DIAZEPAM", "PAL KEY", "CARD",
    "TIMER.B", "MINE.D", "DISC", "ROPE", "SCARF", "SUPPR."};
static void wrist_select(bool grip[2], const float sx[2], const float sy[2], bool owns[2], double now_ms)
{
    owns[0] = owns[1] = false;
    if (!g_cfg.hud_wrist || !g_cfg.hud_select_on_wrist) { g_wl[0].open = g_wl[1].open = false; return; }
    HudData h; AcquireSRWLockShared(&g_hud_lock); h = g_hud; ReleaseSRWLockShared(&g_hud_lock);
    bool ctx = g_hud_ctx.load() && h.valid && h.pl == 0;
    for (int side = 0; side < 2; ++side) {
        WristList& L = g_wl[side];
        int equipped = side == 1 ? h.weapon : h.item;
        if (!L.open) {
            bool pressed = grip[side] && !L.prev;           // only a press that STARTS in gameplay opens the list
            L.prev = grip[side];
            if (pressed && ctx) {
                L.open = true; L.navigated = false; L.used = false; L.opened_at = now_ms; L.last_step = 0; L.step_dir = 0;
                L.count = 0; L.ids[L.count++] = -1;
                int n = side == 1 ? 10 : 24;
                for (int i = 0; i < n && L.count < 26; ++i) if ((side == 1 ? h.wown[i] : h.iown[i]) >= 0) L.ids[L.count++] = i;
                L.sel = 0; for (int k = 0; k < L.count; ++k) if (L.ids[k] == equipped) L.sel = k;
                logf("WRIST: %s list opened (%d entries, equipped %d)", side == 1 ? "weapon" : "item", L.count - 1, equipped);
            }
            if (!L.open) continue;
        }
        owns[side] = true;
        if (grip[side] && g_cfg.v15.wheel == 2) {   // v16 carousel: flick the stick left / right to step (holding repeats)
            int dir = sx[side] > 0.6f ? 1 : (sx[side] < -0.6f ? -1 : 0);
            if (dir && L.count > 0 && (dir != g_car.stick_dir || now_ms - g_car.last_stick >= 260)) {
                int t = g_car.target + dir; g_car.target = t < 0 ? 0 : t > L.count - 1 ? L.count - 1 : t; g_car.last_stick = now_ms;
            }
            g_car.stick_dir = dir;
            continue;
        }
        if (grip[side] && g_cfg.v15.wheel == 1) {   // v15 wheel: the stick points at a slot (GOG SlotForStick)
            int k = WristWheel::SlotForStick(L.count, sx[side], sy[side]);
            if (k >= 0 && k < L.count && k != L.sel) { L.sel = k; L.navigated = true; }
            continue;
        }
        if (grip[side]) {   // navigate: stick up = previous, down = next; repeats while held
            float v = (fabsf(sy[side]) >= fabsf(sx[side])) ? -sy[side] : sx[side];
            int dir = v > 0.6f ? 1 : (v < -0.6f ? -1 : 0);
            if (dir != 0 && L.count > 0 && (dir != L.step_dir || now_ms - L.last_step >= 170)) {
                L.sel = (L.sel + dir + L.count) % L.count; L.last_step = now_ms; L.navigated = true;
            }
            L.step_dir = dir;
            continue;
        }
        L.open = false; L.prev = false;                  // released: equip (a quick tap with no scrolling toggles)
        if (!ctx) continue;
        if (L.used) { logf("WRIST: item list closed after using an item -- equipment unchanged"); continue; }   // v13 (GOG)
        bool tap = !L.navigated && now_ms - L.opened_at < 300;
        int want = -2;
        if (tap) {
            if (equipped >= 0) { L.last_equipped = equipped; want = -1; }
            else if (L.last_equipped >= 0 && (side == 1 ? h.wown[L.last_equipped % 10] : h.iown[L.last_equipped % 24]) >= 0) want = L.last_equipped;
        } else if (L.count > 0) {
            int id = L.ids[L.sel];
            if (id != equipped) { if (equipped >= 0) L.last_equipped = equipped; want = id; }
        }
        if (want != -2) {
            g_equip_req = ((side + 1) << 16) | (uint16_t)(int16_t)want;
            logf("WRIST: %s -> %s%s", side == 1 ? "weapon" : "item", want < 0 ? "NONE" : side == 1 ? kGogWeapon[want] : kGogItem[want], tap ? " (quick tap)" : "");
        }
    }
}

// Read the controllers and hand a PS1-pad view of them to the writer thread.
static void xr_poll_input(XrSession ses, XrInput& in, bool focused)
{
    VrPad vp;
    if (in.ok && focused && g_cfg.controls) {
        XrActiveActionSet aas{in.set, XR_NULL_PATH};
        XrActionsSyncInfo sy{XR_TYPE_ACTIONS_SYNC_INFO}; sy.countActiveActionSets = 1; sy.activeActionSets = &aas;
        if (XR_SUCCEEDED(pxrSyncActions(ses, &sy))) {
            auto B = [&](int a) { XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = in.act[a];
                                  XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN}; pxrGetActionStateBoolean(ses, &gi, &st);
                                  return st.isActive && st.currentState; };
            auto F = [&](int a) { XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = in.act[a];
                                  XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT}; pxrGetActionStateFloat(ses, &gi, &st);
                                  return st.isActive ? st.currentState : 0.f; };
            auto V = [&](int a) { XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = in.act[a];
                                  XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F}; pxrGetActionStateVector2f(ses, &gi, &st);
                                  return st.isActive ? st.currentState : XrVector2f{0, 0}; };
            double now_ms = qpc_us() / 1000.0;
            // left menu button: a short press is the game's Start (sent on release, held 120 ms so a game frame sees
            // it); held 0.7 s it opens or closes the settings panel instead
            bool menu = B(A_MENU);
            if (menu && !in.menu_prev) { in.menu_down_t = now_ms; in.menu_used = false; }
            if (menu && !in.menu_used && now_ms - in.menu_down_t > 700) { in.menu_used = true; panel_open(!g_panel.open); }
            if (!menu && in.menu_prev && !in.menu_used && !g_panel.open) in.start_until = now_ms + 120;
            in.menu_prev = menu;
            {   // v16 (GOG): hold L3 ~0.9 s = VR Settings; with it open a tap closes it; a tap otherwise = the game's L3 mapping
                static bool l3_prev = false, l3_used = false; static double l3_down = 0;
                bool l3 = B(A_LCLICK);
                {   // v17: both stick clicks together = the Master Collection menu (E + M, as on the keyboard)
                    bool r3n = B(A_RCLICK);
                    if (g_cfg.v15.mc_menu && l3 && r3n && !in.combo) {
                        in.combo = true; g_mc_menu_until = GetTickCount64() + 500;
                        xr_buzz(ses, in, 0, 0.6f, 80); xr_buzz(ses, in, 1, 0.6f, 80);
                        logf("INPUT: both stick clicks -> Master Collection menu (E + M)");
                    }
                    if (!l3 && !r3n) in.combo = false;
                    if (in.combo) l3_used = true;
                }
                if (l3 && !l3_prev) { l3_down = now_ms; l3_used = false; }
                if (l3 && !l3_used && !g_panel.open && now_ms - l3_down >= 900) {
                    l3_used = true; panel_open(true); xr_buzz(ses, in, 0, 0.5f, 80); logf("INPUT: left stick click held -> VR settings");
                }
                if (!l3 && l3_prev && !l3_used) {
                    if (g_panel.open) panel_open(false);
                    else in.lclick_until = now_ms + 120;
                }
                l3_prev = l3;
            }
            if (g_panel.open) {
                XrVector2f l = V(A_LSTICK);
                panel_input(l.x, l.y, B(A_A), B(A_B), now_ms, F(A_LTRIG) > 0.5f, F(A_RTRIG) > 0.5f, B(A_Y));
                AcquireSRWLockExclusive(&g_vrpad_lock); g_vrpad = VrPad(); g_vrpad.active = true; ReleaseSRWLockExclusive(&g_vrpad_lock);
                return;                                         // the game gets nothing while the panel is open
            }
            uint16_t b = 0;
            if (now_ms < in.start_until) b |= g_cfg.map_menu;
            if (B(A_A)) b |= g_cfg.map_a;           if (B(A_B)) b |= g_cfg.map_b;
            {   // v25: X + Y together = Start (SteamVR keeps the left menu button for its dashboard)
                static double xy_first = 0; static bool xy_combo = false; static int xy_log = 0;
                bool bx = B(A_X), by = B(A_Y);
                if (!g_cfg.v15.xy_start) { if (bx) b |= g_cfg.map_x; if (by) b |= g_cfg.map_y; }
                else if (!bx && !by) { xy_first = 0; xy_combo = false; }
                else {
                    if (!xy_first) xy_first = now_ms;
                    if (bx && by && !xy_combo && now_ms - xy_first <= 80) {
                        xy_combo = true; in.start_until = now_ms + 120;
                        xr_buzz(ses, in, 0, 0.5f, 60);
                        if (xy_log++ < 20) logf("INPUT: X + Y together -> Start");
                    }
                    if (!xy_combo && now_ms - xy_first > 80) { if (bx) b |= g_cfg.map_x; if (by) b |= g_cfg.map_y; }
                }
                if (now_ms < in.start_until) b |= g_cfg.map_menu;
            }
            in.trig[0] = F(A_LTRIG) > 0.5f; in.trig[1] = F(A_RTRIG) > 0.5f;
            g_grip_mask = (F(A_LGRIP) > 0.6f ? 1 : 0) | (F(A_RGRIP) > 0.6f ? 2 : 0);
            b |= g_melee_bits.load();
            if (in.trig[0]) b |= g_cfg.map_lt; if (in.trig[1]) b |= g_cfg.map_rt;
            XrVector2f wl = V(A_LSTICK), wr2 = V(A_RSTICK);
            bool wgrip[2] = {F(A_LGRIP) > 0.6f, F(A_RGRIP) > 0.6f}, wown[2] = {false, false};
            {   // v22 CODEC GESTURE (GOG): left grip squeezed with the left hand at your left ear -> Codec (Select), grip not a list
                static bool prevL = false, owns = false; static int clog = 0;
                if (wgrip[0] && !prevL && g_cfg.v15.codec_ear && g_hand_ok[0] && g_hud_ctx.load()) {
                    const float el[3] = {-0.08f, -0.02f, 0.03f}; float ear[3];   // left of, a touch below and behind the eyes
                    quat_rot(g_last_head.orientation, el, ear);
                    float dx = g_hand[0].position.x - (g_last_head.position.x + ear[0]), dy = g_hand[0].position.y - (g_last_head.position.y + ear[1]),
                          dz = g_hand[0].position.z - (g_last_head.position.z + ear[2]);
                    float d = sqrtf(dx * dx + dy * dy + dz * dz), lim = g_cfg.v15.codec_ear_cm / 100.0f;
                    if (d <= lim) { owns = true; in.codec_until = now_ms + 150; xr_buzz(ses, in, 0, 0.6f, 60);
                                    if (clog++ < 10) logf("CODEC: left hand at your ear (%.0f cm) + grip -> Codec", d * 100.0f); }
                    else if (d <= lim * 2 && clog < 10) { ++clog; logf("CODEC: left grip %.0f cm from your ear -- not close enough (codec_ear_cm=%d)", d * 100.0f, g_cfg.v15.codec_ear_cm); }
                }
                if (!wgrip[0]) owns = false;
                prevL = wgrip[0];
                if (owns) wgrip[0] = false;   // the grip is the radio, not the item list
            }
            { float wsx[2] = {wl.x, wr2.x}, wsy[2] = {wl.y, wr2.y}; wrist_select(wgrip, wsx, wsy, wown, now_ms); }
            g_sel_open = g_wl[0].open || g_wl[1].open;   // v16: pause while choosing
            if (wgrip[0] && !wown[0]) b |= g_cfg.map_lg; if (wgrip[1] && !wown[1]) b |= g_cfg.map_rg;
            {   // v13 (GOG WristSelectUseHighlighted): A with the item list open uses the highlighted item; A goes to the list only
                static bool a_prev = false; bool a_now = B(A_A);
                if (g_wl[0].open && g_cfg.item_use_button) {
                    if (a_now && !a_prev) {
                        WristList& L = g_wl[0]; int id = (L.count > 0 && L.sel >= 0 && L.sel < L.count) ? L.ids[L.sel] : -1;
                        if (id >= 0 && id < 24) { g_item_use_req = id + 1; L.used = true; L.navigated = true; }
                    }
                    b &= ~g_cfg.map_a;
                }
                a_prev = a_now;
            }
            if (now_ms < in.lclick_until) b |= g_cfg.map_lclick;   // v16: sent on release (a hold opens VR settings)
            if (now_ms < in.codec_until) b |= 0x100;               // v22: Codec (Select) from the ear gesture
            bool rc = B(A_RCLICK);
            if (g_cfg.rclick_recentre) {   // v17 (GOG): double-press = view mode (VR / diorama / flat), hold 0.7 s or a single tap = recentre
                static bool r3_used = false, tap_pending = false; static double r3_down = 0, tap_t = 0;
                if (rc && !in.rclick_prev) { r3_down = now_ms; r3_used = in.combo; }
                if (in.combo) { r3_used = true; tap_pending = false; }
                if (rc && !r3_used && now_ms - r3_down >= 700) { r3_used = true; tap_pending = false; g_recentre = true; logf("INPUT: right stick click held -> recentre"); }
                if (!rc && in.rclick_prev && !r3_used) {
                    if (g_cfg.v15.r3_view && tap_pending && now_ms - tap_t < 350) {
                        tap_pending = false;
                        int m = g_view_mode.load(); if (m < 0) m = g_cfg.v15.diorama ? 1 : 0;
                        m = (m + 1) % 3; g_view_mode = m;
                        xr_buzz(ses, in, 1, 0.5f, m == 0 ? 60 : m == 1 ? 120 : 200);
                        logf("VIEW: right stick double-click -> %s", kViewName[m]);
                    } else if (g_cfg.v15.r3_view) { tap_pending = true; tap_t = now_ms; }
                    else { g_recentre = true; logf("INPUT: right stick click -> recentre"); }
                }
                if (tap_pending && !rc && now_ms - tap_t >= 350) { tap_pending = false; g_recentre = true; logf("INPUT: right stick click -> recentre"); }
            }
            else if (rc) b |= g_cfg.map_rclick;
            in.rclick_prev = rc;
            XrVector2f l = V(A_LSTICK), r = V(A_RSTICK);
            if (wown[0]) l = XrVector2f{0, 0};             // a list on the left wrist owns the left stick (Snake stands still)
            if (wown[1]) r = XrVector2f{0, 0};             // ... and the right list the right stick (no turning)
            double mag = sqrt((double)l.x * l.x + (double)l.y * l.y);
            if (mag > 0.15) {
                int lx = (int)lround(128 + 127 * l.x), ly = (int)lround(128 - 127 * l.y);
                if (lx < 0) lx = 0; if (lx > 255) lx = 255; if (ly < 0) ly = 0; if (ly > 255) ly = 255;
                vp.lx = (uint8_t)lx; vp.ly = (uint8_t)ly; vp.stick = true;
                // GV_VecDir2 convention: atan2(x, z) with z = ly - 128 (stick up = z negative = 2048)
                double ang = atan2((double)l.x, (double)-l.y) * 4096.0 / (2 * 3.14159265358979);
                vp.angle = (uint16_t)(((int)lround(ang) % 4096 + 4096) % 4096);
                if (lx < 128 - 64) b |= 0x8000; else if (lx > 128 + 64) b |= 0x2000;      // AnalogStat: d-pad bits past 64
                if (ly < 128 - 64) b |= 0x1000; else if (ly > 128 + 64) b |= 0x4000;
            }
            vp.buttons = b; vp.rx = r.x; vp.ry = r.y; vp.active = true; vp.pa = B(A_A); vp.pb = B(A_B);
            static uint16_t last_b = 0xFFFF;
            if (b != last_b) { g_vrpad_buttons_log = b; last_b = b; }
        }
    }
    AcquireSRWLockExclusive(&g_vrpad_lock); g_vrpad = vp; ReleaseSRWLockExclusive(&g_vrpad_lock);
}

static bool xr_ok(XrResult r, const char* what)
{
    if (XR_SUCCEEDED(r)) return true;
    logf("XR: %s failed (XrResult %d)", what, (int)r);
    return false;
}

static const char* kShaderV10 =
    "Texture2D tex : register(t0); SamplerState smp : register(s0);\n"
    "cbuffer C : register(b0) { float4 opts; float4 rect; };\n"
    "cbuffer M : register(b1) { float4 px; float4 bnd; float4 m0; float4 m1; float4 m2; float4 m3; };\n"
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VSOut VSMain(uint id : SV_VertexID) { VSOut o; float2 uv = float2((id << 1) & 2, id & 2);\n"
    "  o.uv = uv; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "float3 toLinear(float3 c) { return lerp(c / 12.92, pow((c + 0.055) / 1.055, 2.4), step(0.04045, c)); }\n"
    "float3 S3(float2 uv, float2 d) { return (tex.SampleLevel(smp, uv - d, 0).rgb + tex.SampleLevel(smp, uv, 0).rgb + tex.SampleLevel(smp, uv + d, 0).rgb) / 3.0; }\n"
    "float3 hide(float3 c, float2 uv, float4 r) {\n"
    "  if (uv.x < r.x || uv.x >= r.z || uv.y < r.y || uv.y >= r.w) return c;\n"
    "  if (px.w < 0.5) return float3(0, 0, 0);\n"
    "  float2 d0 = (uv - r.xy) / px.xy + 1.0; float2 d1 = (r.zw - uv) / px.xy + 1.0;\n"
    "  float xl = r.x - 2.0 * px.x, xr = r.z + 2.0 * px.x, yt = r.y - 2.0 * px.y, yb = r.w + 2.0 * px.y;\n"
    "  float wl = xl > bnd.x ? 1.0 / (d0.x * d0.x) : 0.0; float wr = xr < bnd.z ? 1.0 / (d1.x * d1.x) : 0.0;\n"
    "  float wt = yt > bnd.y ? 1.0 / (d0.y * d0.y) : 0.0; float wb = yb < bnd.w ? 1.0 / (d1.y * d1.y) : 0.0;\n"
    "  float sw = wl + wr + wt + wb;\n"
    "  if (sw <= 0.0) return float3(0, 0, 0);\n"
    "  float2 dv = float2(0, 3.0 * px.y), dh = float2(3.0 * px.x, 0);\n"
    "  float3 acc = wl * S3(float2(xl, uv.y), dv) + wr * S3(float2(xr, uv.y), dv) + wt * S3(float2(uv.x, yt), dh) + wb * S3(float2(uv.x, yb), dh);\n"
    "  return acc / sw; }\n"
    "float4 PSMain(VSOut i) : SV_Target { float2 uv = lerp(rect.xy, rect.zw, i.uv);\n"
    "  float4 t = tex.SampleLevel(smp, uv, 0); float3 c = t.rgb;\n"
    "  if (px.z > 0.5) c = hide(c, uv, m0);\n"
    "  if (px.z > 1.5) c = hide(c, uv, m1);\n"
    "  if (px.z > 2.5) c = hide(c, uv, m2);\n"
    "  if (px.z > 3.5) c = hide(c, uv, m3);\n"
    "  if (opts.w > 0.5) { float2 q = i.uv - 0.5; q.x *= opts.z; c *= saturate((0.5 - length(q)) * 80.0); }\n"
    "  if (opts.x > 0.5) c = toLinear(c);\n"
    "  if (opts.y > 2.5) { float l = max(t.r, max(t.g, t.b)); float ta = saturate((l - 0.28) * 5.0); float hl = 0.0;\n"
    "    for (int k = 0; k < 8; ++k) { float an = k * 0.785398; float4 s2 = tex.SampleLevel(smp, uv + float2(cos(an) * opts.z, sin(an) * opts.w) * 2.5, 0);\n"
    "      hl = max(hl, saturate((max(s2.r, max(s2.g, s2.b)) - 0.28) * 5.0)); }\n"
    "    float ef = saturate(min(i.uv.y, 1.0 - i.uv.y) * 14.0); ta *= ef; hl *= ef;\n"
    "    return float4(c * ta, max(ta, hl * 0.85)); }\n"
    "  float a = 1.0; if (opts.y > 1.5) a = opts.z; else if (opts.y > 0.5) a = t.a;\n"
    "  return float4(c, a); }\n";
static const char* kShader =
    "Texture2D tex : register(t0); SamplerState smp : register(s0);\n"
    "cbuffer C : register(b0) { float4 opts; float4 rect; };\n"
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VSOut VSMain(uint id : SV_VertexID) { VSOut o; float2 uv = float2((id << 1) & 2, id & 2);\n"
    "  o.uv = uv; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "float3 toLinear(float3 c) { return lerp(c / 12.92, pow((c + 0.055) / 1.055, 2.4), step(0.04045, c)); }\n"
    "float4 PSMain(VSOut i) : SV_Target { float3 c = tex.Sample(smp, lerp(rect.xy, rect.zw, i.uv)).rgb;\n"
    "  if (opts.x > 0.5) c = toLinear(c); return float4(c, 1); }\n";

static const int PW = 720, PH = 902;   // panel texture, pixels (0.72 x 0.74 m in the room)
struct PanelGfx { HDC dc = nullptr; HBITMAP bmp = nullptr; void* bits = nullptr; HFONT f1 = nullptr, f2 = nullptr;
                  ID3D11Texture2D* tex = nullptr; ID3D11ShaderResourceView* srv = nullptr; };
static void panel_render(PanelGfx& pg, ID3D11Device* dev, ID3D11DeviceContext* ctx)
{
    if (!pg.dc) {
        pg.dc = CreateCompatibleDC(nullptr);
        BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = PW; bi.bmiHeader.biHeight = -PH;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        pg.bmp = CreateDIBSection(pg.dc, &bi, DIB_RGB_COLORS, &pg.bits, nullptr, 0);
        SelectObject(pg.dc, pg.bmp);
        pg.f1 = CreateFontW(34, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        pg.f2 = CreateFontW(28, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        D3D11_TEXTURE2D_DESC td = {}; td.Width = PW; td.Height = PH; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &pg.tex))) dev->CreateShaderResourceView(pg.tex, nullptr, &pg.srv);
    }
    if (!pg.bits || !pg.tex) return;
    RECT all = {0, 0, PW, PH};
    HBRUSH bg = CreateSolidBrush(RGB(18, 22, 26)); FillRect(pg.dc, &all, bg); DeleteObject(bg);
    SetBkMode(pg.dc, TRANSPARENT);
    SelectObject(pg.dc, pg.f1); SetTextColor(pg.dc, RGB(150, 210, 160));
    RECT t = {28, 16, PW - 28, 60}; DrawTextW(pg.dc, L"MGS1 VR settings", -1, &t, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    SelectObject(pg.dc, pg.f2);
    int y = 66, rh = 32;
    const PanelPage& page = kPages[g_panel.page];
    for (int i = 0; i <= page.n; ++i, y += rh) {
        bool sel = i == g_panel.sel;
        if (sel) { RECT r = {16, y, PW - 16, y + rh}; HBRUSH hb = CreateSolidBrush(RGB(48, 70, 58)); FillRect(pg.dc, &r, hb); DeleteObject(hb); }
        if (i == 0) {   // page row
            wchar_t v[96]; swprintf(v, 96, L"◀  %ls  (%d / %d)  ▶", page.name, g_panel.page + 1, kPageN);
            SetTextColor(pg.dc, sel ? RGB(255, 255, 255) : RGB(150, 210, 160));
            RECT l = {32, y, PW - 32, y + rh}; DrawTextW(pg.dc, v, -1, &l, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            y += 8;   // a little gap under the page row
            continue;
        }
        const PanelItem& it = page.items[i - 1];
        SetTextColor(pg.dc, sel ? RGB(255, 255, 255) : RGB(200, 205, 210));
        RECT l = {32, y, PW / 2 + 120, y + rh}; DrawTextW(pg.dc, it.label, -1, &l, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        wchar_t v[64] = L"";
        if (it.kind == 0) wcscpy(v, panel_get(it) ? L"On" : L"Off");
        else if (it.kind == 1) swprintf(v, 64, L"%d", panel_get(it));
        else if (it.kind == 2) wcscpy(v, panel_get(it) ? L"Snap" : L"Smooth");
        else if (it.kind == 6) swprintf(v, 64, L"%.1f", panel_get(it) / 10.0);
        else if (it.kind == 5) { int sc = panel_get(it); swprintf(v, 64, L"%.0f°", 2 * atan(160.0 / (sc > 0 ? sc : 320)) * 57.29578); }
        else if (it.kind == 7 || it.kind == 8) { int k = panel_get(it); if (k >= 0 && k < opt_count(it.opts)) wcsncpy(v, it.opts[k], 63); }
        else if (it.kind == 9) wcscpy(v, panel_get(it) < 0 ? L"Inverted" : L"Normal");
        else if (it.kind == 3 || it.kind == 4) wcscpy(v, L"press A");
        if (it.kind == 7) for (wchar_t* c = v; *c; ++c) *c = towupper(*c);
        if (it.kind != 3 && it.kind != 4 && sel) { wchar_t w[64]; swprintf(w, 64, L"◀  %ls  ▶", v); wcscpy(v, w); }
        RECT r = {PW / 2 + 120, y, PW - 32, y + rh}; DrawTextW(pg.dc, v, -1, &r, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }
    SetTextColor(pg.dc, RGB(140, 150, 160));
    RECT h = {28, PH - 46, PW - 28, PH - 10};
    DrawTextW(pg.dc, L"Triggers: page   Stick: choose / change   A: press   Y: default   B / L3: close", -1, &h, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    GdiFlush();
    ctx->UpdateSubresource(pg.tex, 0, nullptr, pg.bits, PW * 4, 0);
}
static void panel_free(PanelGfx& pg)
{
    if (pg.srv) pg.srv->Release(); if (pg.tex) pg.tex->Release();
    if (pg.f1) DeleteObject(pg.f1); if (pg.f2) DeleteObject(pg.f2); if (pg.bmp) DeleteObject(pg.bmp); if (pg.dc) DeleteDC(pg.dc);
    pg = PanelGfx();
}

static const int WW = 512, WH = 320;
// v10 wrist panels: a port of the GOG mod's RenderPanels (vr_aim.cpp). Left half = LIFE / O2 / radar / item box, right half =
// weapon box; an open quick-select list replaces its half. Layout in canvas pixels; the radar and the game's boxes are drawn
// afterwards on the GPU into the rectangles laid out here (GOG BlitCrop: fitted, aspect kept, alpha 235).
static const int WPANEL = 256;
static const char* wglyph(char c)
{
    switch (c) {
    case 'A': return "01110100011000111111100011000110001"; case 'B': return "11110100011000111110100011000111110";
    case 'C': return "01110100011000010000100001000101110"; case 'D': return "11110100011000110001100011000111110";
    case 'E': return "11111100001000011110100001000011111"; case 'F': return "11111100001000011110100001000010000";
    case 'G': return "01110100011000010111100011000101111"; case 'H': return "10001100011000111111100011000110001";
    case 'I': return "01110001000010000100001000010001110"; case 'J': return "00111000100001000010000101001001100";
    case 'K': return "10001100101010011000101001001010001"; case 'L': return "10000100001000010000100001000011111";
    case 'M': return "10001110111010110101100011000110001"; case 'N': return "10001110011010110011100011000110001";
    case 'O': return "01110100011000110001100011000101110"; case 'P': return "11110100011000111110100001000010000";
    case 'Q': return "01110100011000110001101011001001101"; case 'R': return "11110100011000111110101001001010001";
    case 'S': return "01111100001000001110000010000111110"; case 'T': return "11111001000010000100001000010000100";
    case 'U': return "10001100011000110001100011000101110"; case 'V': return "10001100011000110001100010101000100";
    case 'W': return "10001100011000110101101011010101010"; case 'X': return "10001100010101000100010101000110001";
    case 'Y': return "10001100010101000100001000010000100"; case 'Z': return "11111000010001000100010001000011111";
    case '0': return "01110100011001110101110011000101110"; case '1': return "00100011000010000100001000010001110";
    case '2': return "01110100010000100010001000100011111"; case '3': return "11111000100010000010000011000101110";
    case '4': return "00010001100101010010111110001000010"; case '5': return "11111100001111000001000011000101110";
    case '6': return "00110010001000011110100011000101110"; case '7': return "11111000010001000100010000100001000";
    case '8': return "01110100011000101110100011000101110"; case '9': return "01110100011000101111000010001001100";
    case '.': return "00000000000000000000000000110001100"; case '-': return "00000000000000011111000000000000000";
    case '/': return "00000000010001000100010001000000000"; case ':': return "00000011000110000000011000110000000";
    default:  return nullptr;
    }
}
struct WristCanvas {
    std::vector<uint8_t> px; int panelH[2] = {0, 0};
    struct Blit { int src = -1; int x = 0, y = 0, w = 0, h = 0; } blit[3];   // src 0 item box, 1 weapon box, 2 radar
    ID3D11Texture2D* tex = nullptr; ID3D11ShaderResourceView* srv = nullptr; uint64_t sig = 0; bool drawn = false;
    void put(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        if (x < 0 || y < 0 || x >= WW || y >= WH) return;
        uint8_t* p = &px[((size_t)y * WW + x) * 4]; p[0] = r; p[1] = g; p[2] = b; p[3] = a; }
    void fill(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        for (int yy = y; yy < y + h; ++yy) for (int xx = x; xx < x + w; ++xx) put(xx, yy, r, g, b, a); }
    int text(int x, int y, const char* s, int sc, uint8_t r, uint8_t g, uint8_t b) {   // 1-px dark outline, as GOG HudText
        for (int pass = 0; pass < 2; ++pass) {
            int cx = x;
            for (const char* c = s; *c; ++c) {
                char ch = (*c >= 'a' && *c <= 'z') ? (char)(*c - 32) : *c;
                const char* rows = wglyph(ch);
                if (rows) for (int gy = 0; gy < 7; ++gy) for (int gx = 0; gx < 5; ++gx) {
                    if (rows[gy * 5 + gx] != '1') continue;
                    if (pass == 0) fill(cx + gx * sc - 1, y + gy * sc - 1, sc + 2, sc + 2, 0, 0, 0, 255);
                    else fill(cx + gx * sc, y + gy * sc, sc, sc, r, g, b, 255);
                }
                cx += 6 * sc;
            }
            if (pass == 1) return cx;
        }
        return x; }
    void bar(int x, int y, int w, int h, float f, uint8_t r, uint8_t g, uint8_t b) {
        f = f < 0 ? 0 : f > 1 ? 1 : f;
        fill(x - 1, y - 1, w + 2, h + 2, 200, 200, 200, 255); fill(x, y, w, h, 20, 20, 20, 230); fill(x, y, (int)(w * f), h, r, g, b, 255); }
    int place(int k, int src, double cw, double ch, int x, int y, int maxW, int maxH) {   // GOG BlitCrop's fitted size
        if (cw <= 1 || ch <= 1 || maxW <= 0 || maxH <= 0) { blit[k].src = -1; return 0; }
        double sc = std::min(maxW / cw, maxH / ch);
        int dw = std::max(1, (int)(cw * sc)), dh = std::max(1, (int)(ch * sc));
        blit[k].src = src; blit[k].x = x; blit[k].y = y; blit[k].w = dw; blit[k].h = dh; return dh; }
};
// the three cut-outs, PS1 drawing pixels (x, y, w, h): item box / weapon box from the GOG fractions of 320x240, the radar
static void hud_src_ps1(int src, double r[4])
{
    if (src == 2) { for (int i = 0; i < 4; ++i) r[i] = g_cfg.radar_ps1[i]; return; }
    const double* f = g_cfg.hud_rect[src == 0 ? 2 : 3];
    r[0] = f[0] * 320; r[1] = f[1] * 240; r[2] = f[2] * 320; r[3] = f[3] * 240;
}
// PS1 drawing pixels -> UV of the captured picture (4:3 centred; 8 lines above line 0, as the radar has used since v9)
static bool ps1_to_uv(const double r[4], float uv[4])
{
    uint32_t cw = g_cap_w.load(), chh = g_cap_h.load();
    if (!cw || !chh) return false;
    double fw = chh * 4.0 / 3.0; if (fw > cw) fw = cw;
    double gx0 = (cw - fw) / 2, sx = fw / 320.0, y0 = chh * 8.0 / 240.0, sy = chh / 240.0;
    { int hh = g_sbs_hud_half.load(); if (hh >= 0) { sx = fw / 640.0; gx0 += hh * fw / 2; } }   // v20: side by side
    uv[0] = (float)((gx0 + r[0] * sx) / cw); uv[1] = (float)((y0 + r[1] * sy) / chh);
    uv[2] = (float)((gx0 + (r[0] + r[2]) * sx) / cw); uv[3] = (float)((y0 + (r[1] + r[3]) * sy) / chh);
    return true;
}
// v17: the caption strip (fractions of the 4:3 frame, GOG [subtitles] strip) -> UV of the captured picture
static const int SUBW = 1024, SUBH = 128;
static bool sub_strip_uv(float uv[4])
{
    uint32_t cw = g_cap_w.load(), chh = g_cap_h.load();
    if (!cw || !chh) return false;
    double fw = chh * 4.0 / 3.0; if (fw > cw) fw = cw;
    double gx0 = (cw - fw) / 2, f[4]; for (int i = 0; i < 4; ++i) f[i] = g_cfg.v15.sub_strip[i] / 10000.0;
    { int hh = g_sbs_hud_half.load(); if (hh >= 0) { fw /= 2; gx0 += hh * fw; } }   // v20: side by side
    uv[0] = (float)((gx0 + f[0] * fw) / cw); uv[1] = (float)f[1]; uv[2] = (float)((gx0 + (f[0] + f[2]) * fw) / cw); uv[3] = (float)(f[1] + f[3]);
    return true;
}
// the main-view masks (GOG: hide_boxes_in_view, hide_radar_in_view, hide_life_in_view); out = up to 4 x (u0, v0, u1, v1)
static int hud_masks(float* out)
{
    HudData h; AcquireSRWLockShared(&g_hud_lock); h = g_hud; ReleaseSRWLockShared(&g_hud_lock);
    int n = 0; double r[4]; float uv[4];
    auto add = [&](const double rr[4]) { if (n < 4 && ps1_to_uv(rr, uv)) { memcpy(out + n * 4, uv, sizeof uv); ++n; } };
    if (g_cfg.hud_hide_boxes) { hud_src_ps1(0, r); add(r); hud_src_ps1(1, r); add(r); }
    if (g_cfg.hud_show_radar && g_cfg.hud_hide_radar) { hud_src_ps1(2, r); add(r); }
    if (g_cfg.hud_hide_life) {   // GOG LifeRect: the bar at (16,16), max/8 long, O2 12 px lower, caption above; padded
        int ml = h.vmax > 0 ? h.vmax : 1024; if (ml > 2048) ml = 2048;
        double x0 = 10, y0 = 6, x1 = 16 + ml / 8.0 + 8, y1 = 16 + 12 + 14;
        for (int i = 0; i < h.nob; ++i) {   // v13: the other bars' captions and bars join the same painted-out rectangle
            x0 = std::min(x0, (double)h.ob[i].x - 6); y0 = std::min(y0, (double)h.ob[i].y - 10);
            x1 = std::max(x1, h.ob[i].x + h.ob[i].max / 8.0 + 8); y1 = std::max(y1, (double)h.ob[i].y + 16);
        }
        if (x1 > 320) x1 = 320;
        double lr[4] = {x0, y0, x1 - x0, y1 - y0}; add(lr);
    }
    return n;
}
static void wrist_list(WristCanvas& w, int X, int side, const WristList& L, const HudData& h)   // GOG DrawWristList
{
    const int rowH = 20, maxRows = 11;
    int first = L.sel - maxRows / 2;
    if (first > L.count - maxRows) first = L.count - maxRows;
    if (first < 0) first = 0;
    int rows = std::min(maxRows, L.count - first), H = 26 + rows * rowH + 4;
    w.fill(X, 0, WPANEL, H, 0, 0, 0, 170);
    w.text(X + 8, 6, side == 1 ? "WEAPON" : "ITEM", 2, 120, 200, 255);
    for (int r = 0; r < rows; ++r) {
        int k = first + r, id = L.ids[k], y = 26 + r * rowH; bool hi = k == L.sel;
        if (hi) w.fill(X + 2, y - 2, WPANEL - 4, rowH, 40, 110, 70, 230);
        char line[40];
        if (id < 0) snprintf(line, sizeof line, "NONE");
        else if (side == 1) snprintf(line, sizeof line, "%s %d", kGogWeapon[id % 10], (int)h.wown[id % 10]);
        else if (id == 17) snprintf(line, sizeof line, "CARD LV%d", (int)h.iown[17]);
        else if (id == 13 || id == 14 || id == 15) snprintf(line, sizeof line, "%s %d", kGogItem[id], (int)h.iown[id]);
        else snprintf(line, sizeof line, "%s", kGogItem[id % 24]);
        bool eq = (side == 1 ? h.weapon : h.item) == id;
        w.text(X + 10, y, line, 2, hi ? 255 : 210, hi ? 255 : 210, eq ? 120 : (hi ? 255 : 210));
    }
    w.panelH[side] = std::min(WPANEL, H);
}
static uint64_t wrist_draw(WristCanvas& w, const HudData& h)   // GOG RenderPanels; returns a signature of what it drew
{
    if (w.px.size() != (size_t)WW * WH * 4) w.px.assign((size_t)WW * WH * 4, 0);
    std::fill(w.px.begin(), w.px.end(), (uint8_t)0);
    for (auto& b : w.blit) b.src = -1;
    uint64_t sig = 1469598103934665603ull; auto mix = [&](uint64_t v) { sig ^= v; sig *= 1099511628211ull; };
    double rr[4];
    {   // LEFT: LIFE, O2, radar, item
        const int X = 0;
        if (g_wl[0].open && !g_cfg.v15.wheel) { wrist_list(w, X, 0, g_wl[0], h); mix(0xA0 + (uint64_t)g_wl[0].sel); mix((uint64_t)g_wl[0].count); mix((uint64_t)(h.item + 1)); }
        else {
            w.fill(X, 0, WPANEL, WPANEL, 0, 0, 0, 150);
            w.text(X + 8, 8, "LIFE", 2, 255, 255, 255);
            float lf = h.vmax > 0 ? (float)h.vit / (float)h.vmax : 0.0f; bool low = lf < 0.25f;
            int lifeFullW = WPANEL - 76, lifeW = lifeFullW;
            if (g_cfg.hud_bar_follows_max && h.vmax > 0)
                lifeW = std::max(12, std::min(lifeFullW, (int)((float)h.vmax / 8.0f * g_cfg.hud_bar_scale_pct / 100.0f + 0.5f)));
            w.bar(X + 64, 8, lifeW, 14, lf, low ? 230 : 40, low ? 50 : 170, low ? 40 : 230);
            int y = 30;
            if (h.o2_ok && h.o2 < 1024) { w.text(X + 8, y, "O2", 2, 255, 255, 255); w.bar(X + 64, y, lifeW, 14, (float)h.o2 / 1024.0f, 60, 200, 255); y += 22; }
            for (int i = 0; i < h.nob && y < WPANEL - 60; ++i) {   // v13, GOG: bosses, Meryl -- their own caption and colour
                const auto& o = h.ob[i];
                y += 2; w.text(X + 8, y, o.name, 2, o.rgb[0], o.rgb[1], o.rgb[2]); y += 18;
                int bw = WPANEL - 16;
                if (g_cfg.hud_bar_follows_max) bw = std::max(12, std::min(WPANEL - 16, (int)((float)o.max / 8.0f * g_cfg.hud_bar_scale_pct / 100.0f + 0.5f)));
                w.bar(X + 8, y, bw, 12, o.max > 0 ? (float)o.now / (float)o.max : 0.f, o.rgb[0], o.rgb[1], o.rgb[2]); y += 18;
                mix((uint64_t)o.now); mix((uint64_t)o.max); mix((uint64_t)o.name[0]);
            }
            mix((uint64_t)h.nob);
            y += 4;
            if (g_cfg.hud_show_radar && h.radar_on) {
                hud_src_ps1(2, rr);
                int radarH = std::max(48, std::min(108, WPANEL - y - 80));
                double sc = std::min((WPANEL - 16) / rr[2], radarH / rr[3]); int dw = (int)(rr[2] * sc);
                y += w.place(2, 2, rr[2], rr[3], X + (WPANEL - dw) / 2, y, WPANEL - 16, radarH) + 6;
            }
            if (g_cfg.hud_show_boxes && h.item >= 0) { hud_src_ps1(0, rr); y += w.place(0, 0, rr[2], rr[3], X + 8, y, WPANEL - 16, std::min(72, WPANEL - y - 4)); }
            else if (h.item >= 0 && h.item < 24) {
                char line[32];
                if (h.icount > 0 && (h.item == 13 || h.item == 14 || h.item == 15)) snprintf(line, sizeof line, "%s %d", kGogItem[h.item], h.icount);
                else if (h.item == 17) snprintf(line, sizeof line, "CARD LV%d", h.icount);
                else snprintf(line, sizeof line, "%s", kGogItem[h.item]);
                w.text(X + 8, y, line, 2, 230, 230, 230); y += 18;
            }
            w.panelH[0] = std::max(40, std::min(WPANEL, y + 6));
            mix((uint64_t)h.vit); mix((uint64_t)h.vmax); mix((uint64_t)(h.o2_ok ? h.o2 : 1024)); mix((uint64_t)(h.item + 1)); mix((uint64_t)h.icount);
        }
        mix(12);
    }
    {   // RIGHT: weapon
        const int X = WPANEL;
        if (g_wl[1].open && !g_cfg.v15.wheel) { wrist_list(w, X, 1, g_wl[1], h); mix(0xB0 + (uint64_t)g_wl[1].sel); mix((uint64_t)g_wl[1].count); mix((uint64_t)(h.weapon + 1)); }
        else if (h.weapon >= 0) {
            w.fill(X, 0, WPANEL, 112, 0, 0, 0, 150);
            int y = 6;
            if (g_cfg.hud_show_boxes) { hud_src_ps1(1, rr); y += w.place(1, 1, rr[2], rr[3], X + 8, y, WPANEL - 16, 100); }
            else if (h.weapon < 10) { char line[32]; snprintf(line, sizeof line, "%s %d", kGogWeapon[h.weapon], h.wammo); w.text(X + 8, y + 4, line, 2, 230, 230, 230); y += 24; }
            w.panelH[1] = std::max(24, std::min(112, y + 6));
            mix((uint64_t)(h.weapon + 1)); mix((uint64_t)h.wammo);
        } else w.panelH[1] = 0;
        mix(22);
    }
    mix((uint64_t)g_cfg.hud_show_boxes); mix((uint64_t)g_cfg.hud_show_radar); mix((uint64_t)h.radar_on);
    return sig;
}
// quaternion helpers (XR thread)
static XrQuaternionf qmul(XrQuaternionf a, XrQuaternionf b) {
    return XrQuaternionf{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                         a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z}; }
static XrVector3f qrot(XrQuaternionf q, XrVector3f v) {
    float tx = 2 * (q.y * v.z - q.z * v.y), ty = 2 * (q.z * v.x - q.x * v.z), tz = 2 * (q.x * v.y - q.y * v.x);
    return XrVector3f{v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz), v.z + q.w * tz + (q.x * ty - q.y * tx)}; }
static XrQuaternionf qaxis(float x, float y, float z, float deg) { float h = deg * 3.14159265f / 360.f, sn = sinf(h); return XrQuaternionf{x * sn, y * sn, z * sn, cosf(h)}; }
static XrQuaternionf qeuler(const double pyr[3]) {   // GOG EulerDeg: yaw (Y), then pitch (X), then roll (Z)
    return qmul(qmul(qaxis(0, 1, 0, (float)pyr[1]), qaxis(1, 0, 0, (float)pyr[0])), qaxis(0, 0, 1, (float)pyr[2])); }

// (v22: g_hand / g_hand_ok declared with quat_rot, the input code reads them)

struct PoseRec { uint32_t seq; XrQuaternionf q; XrVector3f p; };
static PoseRec g_hist[256];          // XR thread only
static uint32_t g_pose_seq = 0;      // XR thread only

struct XrGfx {
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr;
    ID3D11SamplerState* smp = nullptr; ID3D11Buffer* cb = nullptr;
    ID3D11Buffer* cb1 = nullptr; bool v10_shader = false;   // v10: mask constants (b1); false = v9 shader (no masks, opaque)
    ID3D11Texture2D* src = nullptr; ID3D11ShaderResourceView* srv = nullptr; uint32_t src_gen = 0;
    ID3D11Texture2D* staging = nullptr;
    ID3D11Texture2D* slot[CAP_SLOTS] = {};    // the game's shared slots, opened on our device
    ID3D11Query* xq = nullptr; int reading = -1; uint32_t taken_seq = 0;
    ID3D11Texture2D* priv = nullptr;          // our own copy; all drawing and scanning use this
    uint32_t tag_seq = 0, tag_screen = 320; int tag_tracked = 0;   // tags of the picture in priv
    uint32_t new_pics = 0, busy = 0, gpu_wait = 0, pics_total = 0;
    // stereo: the newest picture of each eye (mono pictures go to both)
    ID3D11Texture2D* eyeTex[2] = {}; ID3D11ShaderResourceView* eyeSrv[2] = {};
    struct EyeTag { uint32_t seq = 0, screen = 320; int tracked = 0, code = 0; double t = 0; bool valid = false; bool sbs = false; int half = 0; } eyet[2];
    uint32_t code_count[3] = {0, 0, 0}, eye_timeouts = 0, ring_agree = 0, ring_disagree = 0; double newest_t = 0;
    uint32_t pose_marked = 0, pose_unmarked = 0, pose_unfound = 0, pose_same = 0, pose_diff = 0, pose_diff_abs = 0;
    uint32_t last_seq = 0, last_screen = 320; bool have_last = false;   // last picture that carried its own pose
    // auto crop: union of non-black pixels seen since the picture source was (re)connected
    int bx0 = 1 << 30, by0 = 1 << 30, bx1 = -1, by1 = -1; ULONGLONG last_scan = 0; int scans = 0, unchanged = 0; uint32_t epoch = 0;
};

static bool make_shaders(XrGfx& g)
{
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!dc) { logf("XR: d3dcompiler_47.dll not found; cannot draw the game screen"); return false; }
    auto compile = (pD3DCompile)GetProcAddress(dc, "D3DCompile");
    if (!compile) return false;
    ID3DBlob *vb = nullptr, *pb = nullptr, *err = nullptr;
    const char* src = kShaderV10; g.v10_shader = true;
    if (FAILED(compile(src, strlen(src), "mgs1mc_blit10", nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, &pb, &err))) {
        logf("XR: v10 shader did not compile (%s) -- using the v9 shader: wrist panels opaque, game HUD stays in the view",
             err ? (const char*)err->GetBufferPointer() : "?");
        if (err) { err->Release(); err = nullptr; }
        src = kShader; g.v10_shader = false; pb = nullptr;
        if (FAILED(compile(src, strlen(src), "mgs1mc_blit", nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, &pb, &err))) {
            logf("XR: pixel shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?"); return false; }
    } else logf("XR: v10 shader ready (translucent wrist panels, game HUD hidden in the view)");
    if (FAILED(compile(src, strlen(src), "mgs1mc_blit", nullptr, nullptr, "VSMain", "vs_4_0", 0, 0, &vb, &err))) {
        logf("XR: vertex shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?"); return false; }
    g.dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &g.vs);
    g.dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g.ps);
    vb->Release(); pb->Release();
    D3D11_SAMPLER_DESC sd = {}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
    g.dev->CreateSamplerState(&sd, &g.smp);
    D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 32; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g.dev->CreateBuffer(&bd, nullptr, &g.cb);
    {   // b1: mask constants, all zero = no masks; bound once, rewritten around the main view draw
        float z[24] = {0}; D3D11_SUBRESOURCE_DATA sd0 = {z, 0, 0};
        D3D11_BUFFER_DESC bd1 = bd; bd1.ByteWidth = sizeof z;
        g.dev->CreateBuffer(&bd1, &sd0, &g.cb1);
        if (g.cb1) g.ctx->PSSetConstantBuffers(1, 1, &g.cb1);
    }
    return g.vs && g.ps && g.smp && g.cb;
}

static DXGI_FORMAT typed_of(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}

// Re-open the game's shared texture on our device when the game (re)creates it.
static void refresh_source(XrGfx& g)
{
    uint32_t gen = g_cap_gen.load();
    if (gen == g.src_gen) return;
    if (g.srv) { g.srv->Release(); g.srv = nullptr; }
    if (g.reading >= 0 && g.xq) { while (g.ctx->GetData(g.xq, nullptr, 0, 0) == S_FALSE) Sleep(0); }   // our last copy done
    g.reading = -1; g_xr_slot = -1; g.taken_seq = 0;
    for (auto& t : g.slot) if (t) { t->Release(); t = nullptr; }
    if (g.priv) { g.priv->Release(); g.priv = nullptr; }
    for (int e = 0; e < 2; ++e) {
        if (g.eyeSrv[e]) { g.eyeSrv[e]->Release(); g.eyeSrv[e] = nullptr; }
        if (g.eyeTex[e]) { g.eyeTex[e]->Release(); g.eyeTex[e] = nullptr; }
        g.eyet[e].valid = false;
    }
    g.src = nullptr;
    if (g.staging) { g.staging->Release(); g.staging = nullptr; }
    g.bx0 = g.by0 = 1 << 30; g.bx1 = g.by1 = -1; g.scans = 0;
    g.src_gen = gen;
    if (!g.xq) { D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0}; if (FAILED(g.dev->CreateQuery(&qd, &g.xq))) g.xq = nullptr; }
    for (int k = 0; k < CAP_SLOTS; ++k) {
        HANDLE h = g_cap_handles[k].load();
        if (!h) { for (auto& t : g.slot) if (t) { t->Release(); t = nullptr; } return; }
        if (FAILED(g.dev->OpenSharedResource(h, __uuidof(ID3D11Texture2D), (void**)&g.slot[k])) || !g.slot[k]) {
            logf("XR: could not open the game's shared texture (different GPU?)"); g.slot[k] = nullptr;
            for (auto& t : g.slot) if (t) { t->Release(); t = nullptr; }
            return;
        }
    }
    g.src = g.slot[0];
    D3D11_TEXTURE2D_DESC d; g.src->GetDesc(&d);
    D3D11_TEXTURE2D_DESC pd = d; pd.MiscFlags = 0; pd.BindFlags = D3D11_BIND_SHADER_RESOURCE; pd.Usage = D3D11_USAGE_DEFAULT; pd.CPUAccessFlags = 0;
    if (FAILED(g.dev->CreateTexture2D(&pd, nullptr, &g.priv))) { logf("XR: private picture texture failed"); g.priv = nullptr; return; }
    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = typed_of(d.Format); sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
    for (int e = 0; e < 2; ++e) {
        if (FAILED(g.dev->CreateTexture2D(&pd, nullptr, &g.eyeTex[e])) ||
            FAILED(g.dev->CreateShaderResourceView(g.eyeTex[e], &sv, &g.eyeSrv[e]))) { logf("XR: eye picture texture failed"); }
    }
    if (FAILED(g.dev->CreateShaderResourceView(g.priv, &sv, &g.srv))) { logf("XR: SRV for game texture failed (format %d)", (int)d.Format); g.srv = nullptr; }
    else logf("XR: game picture %ux%u format %d connected (3 slots, GPU-query hand-off, generation %u)", d.Width, d.Height, (int)d.Format, gen);
}

// Take the newest finished picture; never wait on the GPU. Tags travel with it.
static void take_picture(XrGfx& g)
{
    if (!g.priv || !g.xq || !g.slot[0]) return;
    if (g.reading >= 0) {                                   // our previous copy still on the GPU? keep the slot
        if (g.ctx->GetData(g.xq, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) { ++g.gpu_wait; ++g.busy; return; }
        g.reading = -1; g_xr_slot = -1;
    }
    uint32_t rs = g_cap_ready_seq.load();
    int r = g_cap_ready.load();
    if (r < 0 || rs == g.taken_seq) { ++g.busy; return; }
    g_xr_slot = r;                                          // claim it, then make sure it is still the newest
    if (g_cap_ready.load() != r) { g_xr_slot = -1; ++g.busy; return; }
    g.ctx->CopyResource(g.priv, g.slot[r]);
    uint32_t raw = g_slot_seq[r].load();
    int ring_code = (int)(raw >> 30) & 3;                        // the eye the ring guessed (diagnostic only)
    int mark = g_slot_mark[r].load();                             // the eye read from the picture itself
    int code = mark == 1 ? 1 : mark == 2 ? 2 : 0;                 // no marker or mono marker: both eyes
    int mark2 = g_slot_mark2[r].load();
    bool sbs = (mark == 1 && mark2 == 2) || (mark == 2 && mark2 == 1);   // v20: true stereo, both eyes in this picture
    g_sbs_hud_half = sbs ? (mark == 1 ? 0 : 1) : -1;
    if (sbs) { g_sbs_pics++; code = 0; }
    if (mark == 1 || mark == 2) { if (ring_code == mark) g.ring_agree++; else g.ring_disagree++; }
    g.tag_seq = raw & 0x3FFFFFFF; g.tag_screen = g_slot_screen[r].load(); g.tag_tracked = g_slot_tracked[r].load();
    {   // the picture's own pose number (low 6 bits) -> the newest recorded head pose with those bits
        int ms = g_slot_mseq[r].load();
        if (mark != 0 && ms >= 0) {
            uint32_t found = 0;
            for (uint32_t d = 0; d < 128; ++d) {
                uint32_t sq = g_pose_seq - d;
                if ((sq & 63) == (uint32_t)ms && g_hist[sq & 255].seq == sq) { found = sq; break; }
            }
            if (found) {
                int diff = (int)(found - g.tag_seq);
                if (diff == 0) g.pose_same++; else { g.pose_diff++; g.pose_diff_abs += abs(diff); }
                g.tag_seq = found; g.tag_tracked = 1; g.pose_marked++;
                g.last_seq = found; g.last_screen = g.tag_screen; g.have_last = true;
            } else g.pose_unfound++;
        } else {
            g.pose_unmarked++;
            if (mark == 0 && g_keep_pose.load() && g.have_last && g_hist[g.last_seq & 255].seq == g.last_seq) {
                g.tag_seq = g.last_seq; g.tag_screen = g.last_screen; g.tag_tracked = 1;   // the menu over the frozen view
            }
        }
    }
    double now = qpc_us() / 1000.0;
    for (int e = 0; e < 2; ++e) {
        int want_code = (e == 0) != (g_cfg.swap_eyes != 0) ? 1 : 2;
        if (sbs) {   // v20: this eye's half of the side-by-side picture
            if (g.eyeTex[e]) g.ctx->CopyResource(g.eyeTex[e], g.slot[r]);
            g.eyet[e].seq = g.tag_seq; g.eyet[e].screen = g.tag_screen; g.eyet[e].tracked = g.tag_tracked;
            g.eyet[e].code = want_code; g.eyet[e].t = now; g.eyet[e].valid = g.eyeTex[e] != nullptr;
            g.eyet[e].sbs = true; g.eyet[e].half = (mark == want_code) ? 0 : 1;
            continue;
        }
        g.eyet[e].sbs = false;
        if (code != 0 && code != want_code) continue;
        if (g.eyeTex[e]) g.ctx->CopyResource(g.eyeTex[e], g.slot[r]);
        g.eyet[e].seq = g.tag_seq; g.eyet[e].screen = g.tag_screen; g.eyet[e].tracked = g.tag_tracked;
        g.eyet[e].code = code; g.eyet[e].t = now; g.eyet[e].valid = g.eyeTex[e] != nullptr;
    }
    g.newest_t = now; g.code_count[code]++;
    g.ctx->End(g.xq);
    g.ctx->Flush();                                               // so the query is done by the next headset frame
    g.reading = r; g.taken_seq = rs;
    ++g.new_pics; ++g.pics_total;
}

// Read the shared picture back (every 2 s, or on F10): grow the auto-crop box, and save a BMP on request.
static void scan_picture(XrGfx& g, bool save)
{
    if (!g.priv) return;
    uint32_t ep = g_scan_epoch.load();
    if (ep != g.epoch) { g.epoch = ep; g.bx0 = g.by0 = 1 << 30; g.bx1 = g.by1 = -1; g.unchanged = 0; logf("CROP: measuring the PS1 picture area"); }
    bool measure = g_scan_ok.load();
    D3D11_TEXTURE2D_DESC d; g.priv->GetDesc(&d);
    DXGI_FORMAT f = typed_of(d.Format);
    if (f != DXGI_FORMAT_B8G8R8A8_UNORM && f != DXGI_FORMAT_R8G8B8A8_UNORM) {
        if (g.scans++ == 0) logf("CROP: picture format %d not scanned (auto crop off)", (int)d.Format);
        return;
    }
    if (!g.staging) {
        D3D11_TEXTURE2D_DESC t = d; t.Usage = D3D11_USAGE_STAGING; t.BindFlags = 0; t.CPUAccessFlags = D3D11_CPU_ACCESS_READ; t.MiscFlags = 0;
        if (FAILED(g.dev->CreateTexture2D(&t, nullptr, &g.staging))) { g.staging = nullptr; return; }
    }
    g.ctx->CopyResource(g.staging, g.priv);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g.ctx->Map(g.staging, 0, D3D11_MAP_READ, 0, &m))) return;
    const uint8_t* px = (const uint8_t*)m.pData;
    int x0 = g.bx0, y0 = g.by0, x1 = g.bx1, y1 = g.by1;
    for (UINT y = 0; measure && y < d.Height; y += 2) {
        const uint8_t* row = px + (size_t)y * m.RowPitch;
        for (UINT x = 0; x < d.Width; x += 2) {
            const uint8_t* q = row + x * 4;
            if (q[0] > 24 || q[1] > 24 || q[2] > 24) {
                if ((int)x < x0) x0 = x; if ((int)x > x1) x1 = x;
                if ((int)y < y0) y0 = y; if ((int)y > y1) y1 = y;
            }
        }
    }
    if (x1 >= 0 && (x0 != g.bx0 || y0 != g.by0 || x1 != g.bx1 || y1 != g.by1)) {
        g.bx0 = x0; g.by0 = y0; g.bx1 = x1; g.by1 = y1;
        logf("CROP: picture area so far x=%d..%d y=%d..%d (%dx%d of %ux%u)", x0, x1 + 1, y0, y1 + 1, x1 + 2 - x0, y1 + 2 - y0, d.Width, d.Height);
        g.unchanged = 0;
    } else if (measure) {
        if (++g.unchanged == 20) logf("CROP: area stable for 40 s; auto-scan paused (F10 still saves pictures)");
    }
    ++g.scans;
    if (save) {
        static int n = 0;
        wchar_t name[MAX_PATH];
        swprintf(name, MAX_PATH, L"%lsmgs1mc_vr_probe\\frame_%03d.bmp", g_dir.c_str(), n);
        FILE* fo = _wfopen(name, L"wb");
        if (fo) {
            uint32_t rowb = d.Width * 3, pad = (4 - rowb % 4) % 4, img = (rowb + pad) * d.Height;
            uint8_t h[54] = {'B', 'M'};
            uint32_t v;
            v = 54 + img; memcpy(h + 2, &v, 4); v = 54; memcpy(h + 10, &v, 4); v = 40; memcpy(h + 14, &v, 4);
            v = d.Width; memcpy(h + 18, &v, 4); v = d.Height; memcpy(h + 22, &v, 4);
            h[26] = 1; h[28] = 24; v = img; memcpy(h + 34, &v, 4);
            fwrite(h, 1, 54, fo);
            std::vector<uint8_t> line(rowb + pad, 0);
            bool bgra = f == DXGI_FORMAT_B8G8R8A8_UNORM;
            for (int y = (int)d.Height - 1; y >= 0; --y) {
                const uint8_t* row = px + (size_t)y * m.RowPitch;
                for (UINT x = 0; x < d.Width; ++x) {
                    const uint8_t* q = row + x * 4;
                    line[x * 3 + 0] = bgra ? q[0] : q[2]; line[x * 3 + 1] = q[1]; line[x * 3 + 2] = bgra ? q[2] : q[0];
                }
                fwrite(line.data(), 1, line.size(), fo);
            }
            fclose(fo);
            logf("F10: saved mgs1mc_vr_probe\\frame_%03d.bmp (%ux%u)", n, d.Width, d.Height);
            ++n;
        }
    }
    g.ctx->Unmap(g.staging, 0);
}

// Head poses we sent to the game, by sequence number, so a picture can be placed with the pose it was drawn from.
static XrQuaternionf g_ref_q = {0, 0, 0, 1};   // recentred forward (yaw only), XR thread only
static XrVector3f g_ref_p = {0, 0, 0};

// head orientation (OpenXR quaternion, head-to-LOCAL) -> mailbox matrix in PS1 camera axes, relative to
// a recentred forward. Verified in unicorn against US/PAL snapshots (2026-10-08): turn left/right, look up,
// roll left and recentre all move the game camera the same way as the head.
static void publish_head(const XrQuaternionf& q, const XrVector3f& pos)
{
    static double ref_yaw = 0; static bool have_ref = false;
    double x = q.x, y = q.y, z = q.z, w = q.w;
    double R[3][3] = {
        {1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w)},
        {2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
        {2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)} };
    if (g_recentre.exchange(false) || !have_ref) {
        double fx = -R[0][2], fz = -R[2][2];               // head forward = -Z column
        ref_yaw = atan2(-fx, -fz); have_ref = true;
        g_ref_q = {0, (float)sin(ref_yaw / 2), 0, (float)cos(ref_yaw / 2)}; g_ref_p = pos; g_ref_yaw_rad = ref_yaw;
        logf("RECENTRE: forward is now %.1f deg (LOCAL space)", ref_yaw * 57.29578);
        g_screen_anchor = true;
    }
    double c = cos(-ref_yaw), s = sin(-ref_yaw);
    double Ry[3][3] = {{c, 0, s}, {0, 1, 0}, {-s, 0, c}};
    double Rr[3][3];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { Rr[i][j] = 0; for (int k = 0; k < 3; ++k) Rr[i][j] += Ry[i][k] * R[k][j]; }
    static const double S[3] = {1, -1, -1};                 // OpenXR head axes -> PS1 camera axes (x right, y down, z forward)
    int16_t M[9];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
        double v = S[i] * Rr[i][j] * S[j] * 4096.0;
        if (v > 32767) v = 32767; if (v < -32768) v = -32768;
        M[i * 3 + j] = (int16_t)lround(v);
    }
    // relative yaw/pitch/roll for the log
    double fx = -Rr[0][2], fy = -Rr[1][2], fz = -Rr[2][2];
    g_head_yaw_cdeg = (int)(atan2(-fx, -fz) * 5729.578);
    g_head_pitch_cdeg = (int)(asin(fy < -1 ? -1 : fy > 1 ? 1 : fy) * 5729.578);
    g_head_roll_cdeg = (int)(asin(Rr[1][0] < -1 ? -1 : Rr[1][0] > 1 ? 1 : Rr[1][0]) * 5729.578);
    g_head_valid = true;
    if (!g_hook_ready.load()) return;
    uint32_t seq = ++g_pose_seq;
    g_hist[seq & 255] = {seq, q, pos};
    AcquireSRWLockExclusive(&g_pend_lock);
    memcpy(g_pend_M, M, sizeof M); g_pend_seq = seq; g_pend_fresh = true; g_pend_yaw = atan2(-fx, -fz); g_pend_p = pos; g_pend_ref = g_ref_p;
    ReleaseSRWLockExclusive(&g_pend_lock);
}

static const V13Addr* v13_addr(const GameTable* T) { return T && strstr(T->name, "US 1.0") ? &V13_US10 : nullptr; }
// v13: a hand's game MATRIX (hand local = gun local: -Y forward, +Z up), the same chain as the gun in hand: G = Base * S * Ry
struct GMat { int16_t m[9]; int16_t pad; int32_t t[3]; };
static void hand_gmat(const double G[3][3], const XrQuaternionf& q, const double eye[3], const double rel_m[3], GMat& out)
{
    double qx = q.x, qy = q.y, qz = q.z, qw = q.w;
    double A[3][3] = {{1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw)},
                      {2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw)},
                      {2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy)}};
    double fL[3] = {-A[0][2], -A[1][2], -A[2][2]}, uL[3] = {A[0][1], A[1][1], A[2][1]};
    auto mapv = [&](const double v[3], double o[3]) { for (int i = 0; i < 3; ++i) o[i] = G[i][0] * v[0] + G[i][1] * v[1] + G[i][2] * v[2]; };
    double fw[3], up[3]; mapv(fL, fw); mapv(uL, up);
    double fn = sqrt(fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2]); for (double& v : fw) v /= fn;
    double d = up[0] * fw[0] + up[1] * fw[1] + up[2] * fw[2]; for (int i = 0; i < 3; ++i) up[i] -= d * fw[i];
    double un = sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]); for (double& v : up) v /= un;
    double c1[3] = {-fw[0], -fw[1], -fw[2]}, c2[3] = {up[0], up[1], up[2]};
    double c0[3] = {c1[1] * c2[2] - c1[2] * c2[1], c1[2] * c2[0] - c1[0] * c2[2], c1[0] * c2[1] - c1[1] * c2[0]};
    double dg[3]; mapv(rel_m, dg);
    for (int i = 0; i < 3; ++i) {
        const double col[3] = {c0[i], c1[i], c2[i]};
        for (int k = 0; k < 3; ++k) { double v = col[k] * 4096.0; out.m[i * 3 + k] = (int16_t)lround(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
        out.t[i] = (int32_t)lround(eye[i] + dg[i] * g_cfg.units_per_m);
    }
    out.pad = 0;
}

// Writer thread: the only place head matrices go into game RAM (double-buffered slot, then flip).
// Walking view: eye rotation in WORLD axes = Base(heading) * Mhead, where Base is the game's level camera for a
// heading (col0 right = (-cos h,0,sin h), col1 down = (0,-1,0), col2 forward = (sin h,0,cos h); world Y up,
// heading = atan2(fwd.x, fwd.z)). Verified in unicorn on US/PAL snapshots: view heading = Snake facing + head yaw,
// pitch follows the head, eye.t = SnakeCamera.position + offset, PadOrg = heading units + 2048 (the game's own rule).
static DWORD WINAPI writer_thread(LPVOID)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    double h_base = 0, last_t = qpc_us();
    // stereo hold state
    uint32_t st_applied = 0; double st_applied_t = 0, fps_ema_ms = 33.3;
    struct EyeSample { uint32_t gvt; int32_t t[3]; } es[8] = {}; int es_n = 0;
    double hold_until = 0; int16_t last_half = -1;
    bool gun_eye_ok = false; int16_t gun_off[3] = {0, 0, 0};
    bool snap_armed = true; bool body_turning = false; double body_units = 0; int16_t prevM[9] = {4096, 0, 0, 0, 4096, 0, 0, 0, 4096}; double prevM_t = 0;
    for (;;) {
        int16_t M[9]; uint32_t seq = 0; bool fresh = false; double head_yaw = 0; XrVector3f head_p = {0, 0, 0}, ref_p = {0, 0, 0};
        AcquireSRWLockExclusive(&g_pend_lock);
        if (g_pend_fresh) { memcpy(M, g_pend_M, sizeof M); seq = g_pend_seq; head_yaw = g_pend_yaw; head_p = g_pend_p; ref_p = g_pend_ref; g_pend_fresh = false; fresh = true; }
        ReleaseSRWLockExclusive(&g_pend_lock);
        uint8_t* ram = g_ram.load(); const GameTable* T = g_T.load();
        int want = g_mode_want.load();
        bool ok = ram && T && g_hook_ready.load() && rd32(ram, MBOX) == MBOX_MAGIC;
        if (ok && want == 0 && g_mode_cur.load() != 0) { wr32(ram, MBOX + MB_ENABLE, 0); g_mode_cur = 0; }
        if (fresh && ok) {
            double t0 = qpc_us(), dt = (t0 - last_t) / 1e6; last_t = t0; if (dt > 0.1) dt = 0.1;
            int16_t W[9]; memcpy(W, M, sizeof W);
            gun_eye_ok = false;
            if (want == 2) {
                if (g_yaw_ref_new.exchange(false)) h_base = g_yaw_ref_units.load() * (2 * 3.14159265358979 / 4096.0);
                uint8_t rdx = 128; rd(ram, T->paddata + 12, &rdx, 1);               // GV_PAD.right_dx (0x80 = centre)
                { VrPad vp; AcquireSRWLockShared(&g_vrpad_lock); vp = g_vrpad; ReleaseSRWLockShared(&g_vrpad_lock);
                  if (vp.active && fabs(vp.rx) > 0.05f) { int v = (int)lround(128 + 127 * vp.rx); rdx = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); } }
                int dx = (int)rdx - 128, dz = g_cfg.stick_deadzone;
                if (g_cfg.turn_mode == 1) {          // snap: one step per flick past 70%, re-armed under 30%
                    if (snap_armed && (dx > 89 || dx < -89)) {
                        h_base -= (dx > 0 ? 1 : -1) * g_cfg.snap_deg * 3.14159265358979 / 180.0;
                        snap_armed = false; g_snap_count++;
                    } else if (dx < 38 && dx > -38) snap_armed = true;
                } else if (dx > dz || dx < -dz) {
                    double amt = (double)(dx > 0 ? dx - dz : dx + dz) / (127 - dz);
                    h_base -= g_cfg.turn_speed_deg * 3.14159265358979 / 180.0 * dt * amt;   // stick right = turn right
                }
                double c = cos(h_base), s = sin(h_base);
                double B[3][3] = {{-c, 0, s}, {0, -1, 0}, {s, 0, c}};
                for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
                    double v = 0; for (int k = 0; k < 3; ++k) v += B[i][k] * (M[k * 3 + j] / 4096.0);
                    v *= 4096.0; if (v > 32767) v = 32767; if (v < -32768) v = -32768;
                    W[i * 3 + j] = (int16_t)lround(v);
                }
                double fh = h_base + head_yaw;
                bool cut = g_cut_pov.load() || g_setpiece.load();   // v15: a set piece is seen like a cutscene POV
                int eye_fwd = cut ? g_cfg.cut_eye_forward : g_cfg.eye_forward, eye_up = g_cfg.eye_up;
                if (cut) {   // the routine takes y from GM_SnakeCamera; in a cutscene make it the demo Snake's head height
                    uint32_t objs = rd32(ram, MBOX + MB_SNAKEOBJS); int32_t jy = 0; int16_t scy = 0;
                    if (objs >= 0x80000000u && objs < 0x80200000u && rd(ram, objs + 0x288, &jy, 4) && rd(ram, T->snakecam + 2, &scy, 2)) {
                        int v = jy - scy + eye_up; eye_up = v < -32000 ? -32000 : v > 32000 ? 32000 : v;
                    }
                }
                // LEAN (6DoF): head movement since the last recentre, LOCAL -> game world through the same chain as the
                // gun (G = Base(heading) * S * Ry(-ref)), units_per_m game units per metre, clamped per axis.
                double lean[3] = {0, 0, 0};
                if (g_cfg.lean) {
                    double c = cos(h_base), sn = sin(h_base), r = -g_ref_yaw_rad.load(), cr = cos(r), sr = sin(r);
                    double B[3][3] = {{-c, 0, sn}, {0, -1, 0}, {sn, 0, c}}, Ry[3][3] = {{cr, 0, sr}, {0, 1, 0}, {-sr, 0, cr}};
                    double dl[3] = {head_p.x - ref_p.x, head_p.y - ref_p.y, head_p.z - ref_p.z};
                    double lim = g_cfg.lean_max_cm * 10.0 * g_cfg.units_per_m / 1000.0;
                    for (int i = 0; i < 3; ++i) {
                        double v = 0;
                        for (int k = 0; k < 3; ++k) { double gk = 0; for (int j = 0; j < 3; ++j) gk += (k == 0 ? 1.0 : -1.0) * Ry[k][j] * dl[j]; v += B[i][k] * gk; }
                        v *= g_cfg.units_per_m;
                        lean[i] = v > lim ? lim : v < -lim ? -lim : v;
                    }
                    double mm = sqrt(dl[0] * dl[0] + dl[1] * dl[1] + dl[2] * dl[2]) * 1000.0;
                    for (int i = 0; i < 3; ++i) g_lean_mm[i] = (int)lround(lean[i] * 1000.0 / g_cfg.units_per_m);
                    if ((int)mm > g_lean_max_mm.load()) g_lean_max_mm = (int)mm;
                }
                auto cl16 = [](double v) { return (int16_t)lround(v > 32000 ? 32000 : v < -32000 ? -32000 : v); };
                int16_t off[4] = { cl16(eye_fwd * sin(fh) + lean[0]), cl16(eye_up + lean[1]),
                                   cl16(eye_fwd * cos(fh) + lean[2]),
                                   (int16_t)((((int)lround(fh * 4096.0 / (2 * 3.14159265358979)) + 2048) % 4096 + 4096) % 4096) };
                wr(ram, MBOX + MB_OFF, off, sizeof off);
                wr32(ram, MBOX + MB_PADORG_EN, cut ? 0 : 1);                  // no stick steering in a cutscene
                g_walk_heading_cdeg = (int)(fh * 5729.578);
                gun_eye_ok = !cut; memcpy(gun_off, off, sizeof gun_off);
                {   // v13 PSG1 / STINGER: the game's own first-person sight is up -> aim with your head; the PSG1's zoom goes through
                    int16_t fpv2 = 0, wid = -1; rd(ram, T->gm_camera + 0x22, &fpv2, 2); rd(ram, T->stage_name + 0x10 + 14 * 2, &wid, 2);
                    bool sight = !cut && fpv2 == 1 && (wid == 9 || wid == 4);
                    bool scoped = sight && wid == 9;
                    if (scoped != g_scoped.load()) {
                        int16_t zm = 0; rd(ram, T->gm_camera + 0x20, &zm, 2);
                        logf("SCOPE: %s", scoped ? "PSG1 scope UP -- the game's zoom, one picture for both eyes in a round lens, aimed by your head"
                                                 : "PSG1 scope down -- your normal view");
                    }
                    if (sight != g_aim_head.load()) logf("AIM: %s", sight ? (wid == 9 ? "PSG1 aimed by your head" : "Stinger aimed by your head") : "back to normal aiming");
                    g_scoped = scoped; g_aim_head = sight;
                    static uint16_t fov_w = 0xFFFF; uint16_t fv = scoped ? 0 : (uint16_t)g_cfg.fov_screen;   // 0 = keep the game's lens
                    if (fv != fov_w) { wr(ram, MBOX + MB_FOV, &fv, 2); fov_w = fv; }
                    if (sight && g_cfg.aim_by_head && T->player_control) {
                        uint32_t ctl = rd32(ram, T->player_control);
                        if (ctl >= 0x80000000u && ctl < 0x80200000u) {
                            double look = fmod(fh * 4096.0 / (2 * 3.14159265358979), 4096.0); if (look < 0) look += 4096;
                            int16_t vy = (int16_t)((int)lround(look) & 0xFFF);
                            double pitch = g_head_pitch_cdeg.load() / 100.0 * 4096.0 / 360.0;
                            int pv = (int)lround(-pitch * g_cfg.scope_pitch_sign); if (pv > 900) pv = 900; if (pv < -900) pv = -900;
                            int16_t vx = (int16_t)pv;
                            wr(ram, ctl + 0x8, &vx, 2); wr(ram, ctl + 0xA, &vy, 2); wr(ram, ctl + 0x4C, &vx, 2); wr(ram, ctl + 0x4E, &vy, 2);
                            static double aim_log = 0;
                            if (t0 / 1000.0 - aim_log > 2000) {
                                aim_log = t0 / 1000.0; int16_t gr[2] = {0, 0}; rd(ram, ctl + 0x8, gr, 4);
                                logf("AIM: head yaw %.1f deg, pitch %.1f deg -> Snake rot.vy %d, rot.vx %d (pitch_sign %d) | the game holds vy %d, vx %d",
                                     look * 360.0 / 4096, g_head_pitch_cdeg.load() / 100.0, vy, vx, g_cfg.scope_pitch_sign, gr[1], gr[0]);
                            }
                        }
                    }
                }
                // body follows head: standing still, plain stance, looking well away from Snake's facing -> turn him
                if (g_cfg.body_follows_head && T->player_control && !cut) {
                    uint32_t ctl = rd32(ram, T->player_control), st = rd32(ram, T->player_status);
                    const uint32_t busy = 0x0010 | 0x0040 | 0x0080 | 0x0100 | 0x0200 | 0x0800 | 0x1000 | 0x2000 |
                                          0x00010000 | 0x00020000 | 0x01000000 | 0x20000000 | 0x0001 | 0x0008;
                    //  MOVE GROUND NOT_PLAYABLE DAMAGED DOWNED ATTACK CB_BOX GAME_OVER CAUTION(wall) CHECK_WALL WATER PAD_OFF WATCH NORMAL_WATCH
                    VrPad vp2; AcquireSRWLockShared(&g_vrpad_lock); vp2 = g_vrpad; ReleaseSRWLockShared(&g_vrpad_lock);
                    bool idle = !(vp2.active && vp2.stick) && !(st & busy);
                    if (ctl >= 0x80000000u && ctl < 0x80200000u && idle) {
                        int16_t rot = 0; rd(ram, ctl + 0xA, &rot, 2);
                        double look = fmod(fh * 4096.0 / (2 * 3.14159265358979), 4096.0); if (look < 0) look += 4096;
                        double diff = look - (rot & 0xFFF); while (diff > 2048) diff -= 4096; while (diff < -2048) diff += 4096;
                        if (!body_turning && fabs(diff) > g_cfg.body_deadband_deg * 4096.0 / 360) { body_turning = true; body_units = rot & 0xFFF; g_body_turns++; }
                        if (body_turning) {
                            double d2 = look - body_units; while (d2 > 2048) d2 -= 4096; while (d2 < -2048) d2 += 4096;
                            double step = g_cfg.body_turn_speed_deg * 4096.0 / 360 * dt;
                            if (fabs(d2) <= step) { body_units = look; body_turning = false; }
                            else body_units += d2 > 0 ? step : -step;
                            body_units = fmod(body_units + 4096, 4096.0);
                            int16_t v = (int16_t)((int)lround(body_units) & 0xFFF);
                            wr(ram, ctl + 0xA, &v, 2); wr(ram, ctl + 0x4E, &v, 2);   // rot.vy and turn.vy
                        }
                    } else body_turning = false;
                }
            }
            // ---- v15 DIORAMA (GOG "the level as a miniature on a table"): you stand at a table, the level is a 1:scale model
            // on it. The eye is ABSOLUTE in the world: table centre C (follows Snake, glides, re-placed on a cut) + your head
            // offset from the recentred pose x (units_per_m * scale). Routine mode 2 with the stand-in body DIO_FAKE: x / z come
            // from its "head joint" (0x8000B5A0 / B5A8, written here), y = GM_SnakeCamera.y + off.y. Rotation = Base(table) * head.
            {
                static double tyaw = 0, C[3] = {0, 0, 0}, scale = 20; static bool have = false, snap_arm = true, glide = false;
                static int16_t lastS[3] = {0, 0, 0}; static int clamp_logged = 0; static double dio_log = 0;
                if (want == 3) {
                    int16_t S[3] = {0, 0, 0}; rd(ram, T->snakecam, S, sizeof S);
                    const auto& D = g_cfg.v15;
                    bool reset = g_dio_reset.exchange(false) || !have;
                    double jump = sqrt((double)(S[0] - lastS[0]) * (S[0] - lastS[0]) + (double)(S[1] - lastS[1]) * (S[1] - lastS[1]) +
                                       (double)(S[2] - lastS[2]) * (S[2] - lastS[2]));
                    if (!reset && jump > D.dio_cut_units) {   // door, elevator, load: a new area
                        reset = true; g_dio_cuts++;
                        if (D.dio_reorient) { int16_t facing = 0; rd(ram, T->snakecam + 0x2A, &facing, 2); g_dio_yaw_units = facing & 0xFFF; }
                        else g_dio_yaw_units = (int)lround(tyaw * 4096.0 / (2 * 3.14159265358979));
                        logf("DIORAMA: Snake moved %.0f units in one frame -> new area, table placed again%s", jump, D.dio_reorient ? " and turned along Snake" : "");
                    }
                    memcpy(lastS, S, sizeof S);
                    double target[3] = {(double)S[0], (double)S[1] - D.dio_eye_height, (double)S[2]};
                    if (reset) {
                        for (int i = 0; i < 3; ++i) C[i] = target[i];
                        tyaw = g_dio_yaw_units.load() * (2 * 3.14159265358979 / 4096.0); scale = D.dio_scale; have = true; glide = false;
                        logf("DIORAMA: table at (%.0f, %.0f, %.0f), heading %.1f deg, 1:%.0f", C[0], C[1], C[2], tyaw * 57.29578, scale);
                    }
                    {   // follow: Snake may wander follow_radius from the centre, then it glides after him
                        double d[3] = {target[0] - C[0], target[1] - C[1], target[2] - C[2]};
                        double dist = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                        if (D.dio_follow_radius <= 0) { for (int i = 0; i < 3; ++i) C[i] = target[i]; }
                        else {
                            if (dist > D.dio_follow_radius) glide = true;
                            if (glide) {
                                double k = D.dio_follow_speed / 10.0 * dt; if (k > 1) k = 1;
                                for (int i = 0; i < 3; ++i) C[i] += d[i] * k;
                                if (dist < D.dio_follow_radius * 0.25) glide = false;
                            }
                        }
                    }
                    {   // right stick: left / right turns the table in steps, up / down zooms
                        VrPad vp; AcquireSRWLockShared(&g_vrpad_lock); vp = g_vrpad; ReleaseSRWLockShared(&g_vrpad_lock);
                        if (vp.active) {
                            if (snap_arm && fabs(vp.rx) > 0.7f && fabs(vp.rx) > fabs(vp.ry)) {
                                tyaw -= (vp.rx > 0 ? 1 : -1) * D.dio_turn_deg * 3.14159265358979 / 180.0; snap_arm = false; g_dio_turns++;
                            } else if (fabs(vp.rx) < 0.3f) snap_arm = true;
                            if (fabs(vp.ry) > 0.3f && fabs(vp.ry) > fabs(vp.rx)) {
                                scale *= exp(-(vp.ry > 0 ? vp.ry - 0.3 : vp.ry + 0.3) * 1.2 * dt);   // up = bigger model = smaller 1:N
                                if (scale < 5) scale = 5; if (scale > 60) scale = 60;
                            }
                        }
                    }
                    g_dio_scale_x100 = (int)lround(scale * 100);
                    double k = g_cfg.units_per_m * scale;   // game units per real metre
                    double c = cos(tyaw), sn = sin(tyaw), r = -g_ref_yaw_rad.load(), cr = cos(r), sr = sin(r);
                    double dl[3] = {head_p.x - ref_p.x, head_p.y - ref_p.y, head_p.z - ref_p.z};
                    double dr[3] = {cr * dl[0] + sr * dl[2], dl[1], -sr * dl[0] + cr * dl[2]};          // recentred LOCAL (-z ahead)
                    double v[3] = {dr[0], dr[1] + D.dio_drop_cm / 100.0, dr[2] + D.dio_dist_cm / 100.0};  // eye relative to the table centre
                    double gx = -c * v[0] - sn * v[2], gy = v[1], gz = sn * v[0] - c * v[2];              // Base(table) * S * v
                    double eye[3] = {C[0] + gx * k, C[1] + gy * k, C[2] + gz * k};
                    {   double B[3][3] = {{-c, 0, sn}, {0, -1, 0}, {sn, 0, c}};
                        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
                            double vv = 0; for (int q = 0; q < 3; ++q) vv += B[i][q] * (M[q * 3 + j] / 4096.0);
                            vv *= 4096.0; if (vv > 32767) vv = 32767; if (vv < -32768) vv = -32768;
                            W[i * 3 + j] = (int16_t)lround(vv);
                        } }
                    int32_t ax = (int32_t)lround(eye[0]), az = (int32_t)lround(eye[2]);
                    wr(ram, 0x8000B5A0, &ax, 4); wr(ram, 0x8000B5A8, &az, 4);
                    wr32(ram, MBOX + MB_SNAKEOBJS, DIO_FAKE);
                    double oy = eye[1] - S[1];
                    if (oy > 32000 || oy < -32000) { g_dio_clamped++; if (clamp_logged++ < 3) logf("DIORAMA: eye height %.0f units above Snake is past the routine's range -> clamped (lower the scale)", oy); }
                    double fh = tyaw + head_yaw;
                    int16_t off[4] = {0, (int16_t)lround(oy > 32000 ? 32000 : oy < -32000 ? -32000 : oy), 0,
                                      (int16_t)((((int)lround(fh * 4096.0 / (2 * 3.14159265358979)) + 2048) % 4096 + 4096) % 4096)};
                    wr(ram, MBOX + MB_OFF, off, sizeof off);
                    wr32(ram, MBOX + MB_PADORG_EN, 1);   // left stick up = away from you, along where you look
                    { static uint16_t fv_d = 0xFFFF; uint16_t fv = (uint16_t)g_cfg.fov_screen; if (fv != fv_d) { wr(ram, MBOX + MB_FOV, &fv, 2); fv_d = fv; } }
                    if (t0 / 1000.0 - dio_log > 5000) {
                        dio_log = t0 / 1000.0;
                        logf("DIORAMA: 1:%.1f table (%.0f, %.0f, %.0f) heading %.1f deg | Snake (%d, %d, %d) | eye (%.0f, %.0f, %.0f) | head %+.2f %+.2f %+.2f m | "
                             "cuts %d, table turns %d, clamped %d", scale, C[0], C[1], C[2], tyaw * 57.29578, S[0], S[1], S[2], eye[0], eye[1], eye[2],
                             dr[0], dr[1], dr[2], g_dio_cuts.exchange(0), g_dio_turns.exchange(0), g_dio_clamped.exchange(0));
                    }
                } else have = false;
            }
            // ---- v15 LEAN IN HEAD-LOOK (routine mode 1, routine v12): eye.t += DIO each frame. Your head offset since the recentre,
            // in the game camera's own axes (the eye matrix before the head rotation: E * M^T), clamped to lean_max_cm.
            {
                static bool dio_nonzero = false;
                int16_t lo[3] = {0, 0, 0};
                if (want == 1 && g_cfg.lean && g_cfg.v15.tp_lean && !g_demo.load()) {
                    int16_t E[9] = {0}; rd(ram, T->chanl1 + 0x30, E, sizeof E);
                    double r = -g_ref_yaw_rad.load(), cr = cos(r), sr = sin(r);
                    double dl[3] = {head_p.x - ref_p.x, head_p.y - ref_p.y, head_p.z - ref_p.z};
                    double vc[3] = {(cr * dl[0] + sr * dl[2]) * g_cfg.units_per_m, -dl[1] * g_cfg.units_per_m, -(-sr * dl[0] + cr * dl[2]) * g_cfg.units_per_m};
                    double u[3]; for (int j = 0; j < 3; ++j) { u[j] = 0; for (int i = 0; i < 3; ++i) u[j] += M[i * 3 + j] / 4096.0 * vc[i]; }   // M^T vc
                    double lim = g_cfg.lean_max_cm * 10.0 * g_cfg.units_per_m / 1000.0;
                    for (int i = 0; i < 3; ++i) {
                        double w = 0; for (int j = 0; j < 3; ++j) w += E[i * 3 + j] / 4096.0 * u[j];
                        if (w > lim) w = lim; if (w < -lim) w = -lim; lo[i] = (int16_t)lround(w);
                    }
                }
                bool nz = lo[0] || lo[1] || lo[2];
                if (nz || dio_nonzero) { wr(ram, DIO, lo, sizeof lo); dio_nonzero = nz; }
            }

            // head angular speed (from successive head matrices) -> stereo hold
            {
                double tr = 0; for (int k = 0; k < 9; ++k) tr += (prevM[k] / 4096.0) * (M[k] / 4096.0);   // trace(P^T M)
                double c = (tr - 1) / 2; if (c > 1) c = 1; if (c < -1) c = -1;
                double deg = acos(c) * 57.29578, dt2 = (t0 - prevM_t) / 1e6;
                if (prevM_t > 0 && dt2 > 0.002 && dt2 < 0.2) {
                    if (g_cfg.hold_deg_s > 0 && deg / dt2 > g_cfg.hold_deg_s) { hold_until = t0 / 1000.0 + g_cfg.hold_ms; g_hold_head++; }
                    memcpy(prevM, M, sizeof prevM); prevM_t = t0;
                } else if (prevM_t == 0 || dt2 >= 0.2) { memcpy(prevM, M, sizeof prevM); prevM_t = t0; }
            }
            if (want == 2) {   // right stick turning: the turn is not in the head pose, so the older eye would be wrong
                uint8_t rdx = 128; rd(ram, T->paddata + 12, &rdx, 1);
                VrPad vp; AcquireSRWLockShared(&g_vrpad_lock); vp = g_vrpad; ReleaseSRWLockShared(&g_vrpad_lock);
                bool turning = (vp.active && fabs(vp.rx) > 0.15f) || abs((int)rdx - 128) > g_cfg.stick_deadzone;
                if (turning) { hold_until = t0 / 1000.0 + g_cfg.hold_ms; g_hold_turn++; }
            }
            // ---- MOTION: gun in hand -----------------------------------------------------------------------------
            // The weapon actor (socom.c / famas.c) draws its model from objs->root, which DG copies into objs->world
            // at render time; the shot is built from objs->world (DG_SetPos(world) + muzzle offset -> NewBullet). So we
            // write a MATRIX of our own each pose and point objs->root at it: the game draws its own gun in your hand
            // and fires from its muzzle. Gun-local axes (decomp): -Y = barrel, +Z = top of the gun.
            // LOCAL (OpenXR) vector -> game world: G = Base(heading) * S * Ry(-ref), the same chain as the eye matrix.
            {
                GunActor ga; AcquireSRWLockShared(&g_gunact_lock); ga = g_gunact; ReleaseSRWLockShared(&g_gunact_lock);
                GunHand gh; AcquireSRWLockShared(&g_gunhand_lock); gh = g_gunhand; ReleaseSRWLockShared(&g_gunhand_lock);
                bool live = ga.work && rd32(ram, ga.work + 8) == ga.act && rd32(ram, ga.work + 0x14) == ga.fname &&
                            (rd32(ram, ga.work + ga.objoff) & 0x1FFFFF) == (ga.objs & 0x1FFFFF);
                uint32_t root = live ? rd32(ram, ga.objs + 0x20) : 0;
                bool ours_or_orig = root == GUNMTX || root == ga.orig;
                bool hand_fresh = gh.valid && t0 / 1000.0 - gh.t_ms < 250;
                bool want_gun = live && ours_or_orig && g_cfg.gun_in_hand && want == 2 && gun_eye_ok && g_mode_cur.load() == 2;
                int state = !ga.work || !live ? 3 : !want_gun ? 0 : hand_fresh ? 1 : 2;
                if (live && !ours_or_orig) g_gun_skips++;
                if (want_gun && hand_fresh) {
                    double c = cos(h_base), sn = sin(h_base), r = -g_ref_yaw_rad.load(), cr = cos(r), sr = sin(r);
                    double B[3][3] = {{-c, 0, sn}, {0, -1, 0}, {sn, 0, c}}, Ry[3][3] = {{cr, 0, sr}, {0, 1, 0}, {-sr, 0, cr}}, G[3][3];
                    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {   // G = B * S * Ry, S = diag(1,-1,-1)
                        double v = 0; for (int k = 0; k < 3; ++k) v += B[i][k] * (k == 0 ? 1.0 : -1.0) * Ry[k][j];
                        G[i][j] = v;
                    }
                    auto mapv = [&](double x, double y, double z, double o[3]) { for (int i = 0; i < 3; ++i) o[i] = G[i][0] * x + G[i][1] * y + G[i][2] * z; };
                    double qx = gh.aim_q.x, qy = gh.aim_q.y, qz = gh.aim_q.z, qw = gh.aim_q.w;
                    double A[3][3] = {   // aim orientation (aim-local -> LOCAL); -Z points, +Y is up
                        {1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw),     2 * (qx * qz + qy * qw)},
                        {2 * (qx * qy + qz * qw),     1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw)},
                        {2 * (qx * qz - qy * qw),     2 * (qy * qz + qx * qw),     1 - 2 * (qx * qx + qy * qy)} };
                    double fL[3] = {-A[0][2], -A[1][2], -A[2][2]}, uL[3] = {A[0][1], A[1][1], A[2][1]}, sL[3] = {A[0][0], A[1][0], A[2][0]};
                    // trims in the hand's own frame: pitch (+ muzzle up) about its right axis, yaw (+ left) about its up axis, roll
                    auto rot = [](double a[3], double b[3], double ang) {   // rotate a toward b by ang (a, b orthonormal)
                        double ca = cos(ang), sa = sin(ang), na[3], nb[3];
                        for (int i = 0; i < 3; ++i) { na[i] = a[i] * ca + b[i] * sa; nb[i] = b[i] * ca - a[i] * sa; }
                        memcpy(a, na, sizeof na); memcpy(b, nb, sizeof nb); };
                    const double D2R = 3.14159265358979 / 180.0;
                    if (g_cfg.gun_rot_deg[0] != 0) rot(fL, uL, g_cfg.gun_rot_deg[0] * D2R);
                    if (g_cfg.gun_rot_deg[1] != 0) { double nsL[3] = {-sL[0], -sL[1], -sL[2]}; rot(fL, nsL, g_cfg.gun_rot_deg[1] * D2R); for (int i = 0; i < 3; ++i) sL[i] = -nsL[i]; }
                    if (g_cfg.gun_rot_deg[2] != 0) rot(uL, sL, g_cfg.gun_rot_deg[2] * D2R);
                    double fw[3], up[3]; mapv(fL[0], fL[1], fL[2], fw); mapv(uL[0], uL[1], uL[2], up);
                    double fn = sqrt(fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2]); for (double& v : fw) v /= fn;
                    double d = up[0] * fw[0] + up[1] * fw[1] + up[2] * fw[2]; for (int i = 0; i < 3; ++i) up[i] -= d * fw[i];
                    double un = sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]); for (double& v : up) v /= un;
                    double c1[3] = {-fw[0], -fw[1], -fw[2]}, c2[3] = {up[0], up[1], up[2]};
                    double c0[3] = {c1[1] * c2[2] - c1[2] * c2[1], c1[2] * c2[0] - c1[0] * c2[2], c1[0] * c2[1] - c1[1] * c2[0]};
                    // position: the game eye centre (as the camera routine builds it: head joint x/z, game eye height y,
                    // plus our offsets; no stereo shift) + the hand's offset from the head, in game units
                    int32_t ex = 0, ez = 0; int16_t sc3[3] = {0, 0, 0}; rd(ram, T->snakecam, sc3, sizeof sc3);
                    uint32_t so = rd32(ram, MBOX + MB_SNAKEOBJS);
                    if (so >= 0x80000000u && so < 0x80200000u) { rd(ram, so + 0x284, &ex, 4); rd(ram, so + 0x28C, &ez, 4); }
                    else { ex = sc3[0]; ez = sc3[2]; }
                    double eye[3] = {(double)ex + gun_off[0], (double)sc3[1] + gun_off[1], (double)ez + gun_off[2]};
                    double dl[3] = {gh.grip_p.x - head_p.x, gh.grip_p.y - head_p.y, gh.grip_p.z - head_p.z};
                    double reach = sqrt(dl[0] * dl[0] + dl[1] * dl[1] + dl[2] * dl[2]) * 1000.0;
                    if (reach > g_cfg.gun_max_reach_mm) for (double& v : dl) v *= g_cfg.gun_max_reach_mm / reach;
                    double dg[3]; mapv(dl[0], dl[1], dl[2], dg);
                    double upm = g_cfg.units_per_m, mm = upm / 1000.0;
                    const double* go = g_cfg.gun_off_mm;   // grip offset in the controller's axes: x right, y up, z back
                    double tw[3];
                    for (int i = 0; i < 3; ++i) tw[i] = eye[i] + dg[i] * upm;
                    { double ox[3], oy[3], oz[3]; double sLx[3] = {sL[0], sL[1], sL[2]}; mapv(sLx[0], sLx[1], sLx[2], ox);
                      mapv(uL[0], uL[1], uL[2], oy); mapv(-fL[0], -fL[1], -fL[2], oz);
                      for (int i = 0; i < 3; ++i) tw[i] += (ox[i] * go[0] + oy[i] * go[1] + oz[i] * go[2]) * mm; }
                    struct { int16_t m[9]; int16_t pad; int32_t t[3]; } gm;
                    for (int i = 0; i < 3; ++i) {
                        const double col[3] = {c0[i], c1[i], c2[i]};
                        for (int j = 0; j < 3; ++j) { double v = col[j] * 4096.0; gm.m[i * 3 + j] = (int16_t)lround(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
                        gm.t[i] = (int32_t)lround(tw[i]);
                    }
                    gm.pad = 0;
                    wr(ram, GUNMTX, &gm, sizeof gm); g_gun_writes++;
                    if (root != GUNMTX) { wr32(ram, ga.objs + 0x20, GUNMTX); g_gun_redirects++; }
                    if (ga.kind == 3 && g_cfg.throw_from_hand && ga.robj && ga.unit <= 6) {   // grenade.c: the throw leaves from here
                        uint32_t fo = throw_dgobjs(ga.unit), sflag = 0, so2 = k0x(rd32(ram, ga.robj));
                        if (so2) sflag = rd32(ram, so2 + 0x28);
                        wr(ram, THROWMTX, &gm, sizeof gm);
                        wr32(ram, fo + 0x28, sflag);                         // stand-in DG_OBJS.flag = Snake's (invisible bit)
                        wr32(ram, THROWOBJ, fo);                             // stand-in OBJECT.objs
                        uint32_t cur = rd32(ram, ga.work + 0x24);
                        if (cur != THROWOBJ && (cur & 0x1FFFFF) == (ga.robj & 0x1FFFFF)) {
                            g_throw_robj = cur; g_throw_work = ga.work; wr32(ram, ga.work + 0x24, THROWOBJ); g_throw_redirects++;
                        }
                    }
                    for (uint32_t pr : {ga.laser, ga.light}) {   // the SOCOM's laser sight and its glow hang from the same hand joint
                        if (!pr) continue;
                        uint32_t rr = rd32(ram, pr + 0x20);
                        if (rr == ga.orig) wr32(ram, pr + 0x20, GUNMTX);
                    }
                    g_gun_reach_mm = (int)lround(reach);
                    g_gun_yaw_cdeg = (int)lround((atan2(fw[0], fw[2]) - (h_base + head_yaw)) * 5729.578);
                    g_gun_pitch_cdeg = (int)lround(asin(fw[1] < -1 ? -1 : fw[1] > 1 ? 1 : fw[1]) * 5729.578);
                } else if (live && root == GUNMTX) {     // hand the gun back to Snake's hand joint
                    wr32(ram, ga.objs + 0x20, ga.orig); g_gun_restores++;
                    if (ga.kind == 3 && g_throw_work.load() == ga.work && rd32(ram, ga.work + 0x24) == THROWOBJ) {
                        wr32(ram, ga.work + 0x24, g_throw_robj.load()); g_throw_restores++; g_throw_work = 0;
                    }
                    for (uint32_t pr : {ga.laser, ga.light}) if (pr && rd32(ram, pr + 0x20) == GUNMTX) wr32(ram, pr + 0x20, ga.orig);
                }
                {   // v11: give the throw point back whenever it is no longer ours to hold (any reason), if it is still ours
                    uint32_t tw = g_throw_work.load();
                    bool keep = want_gun && hand_fresh && ga.kind == 3 && tw == ga.work && g_cfg.throw_from_hand;
                    if (tw && !keep) {
                        if ((rd32(ram, tw + 0x24) & 0x1FFFFFFFu) == (THROWOBJ & 0x1FFFFFFFu)) { wr32(ram, tw + 0x24, g_throw_robj.load()); g_throw_restores++; }
                        g_throw_work = 0;
                    }
                }
                g_gun_state = state;
                // ---- v13 FLOATING HANDS, WALL TOUCH, KNOCK (GOG motion_aim.cpp) --------------------------------------------
                const V13Addr* A13 = v13_addr(T);
                uint32_t sbody = rd32(ram, MBOX + MB_SNAKEOBJS);
                bool body_ok = sbody >= 0x80000000u && sbody < 0x80200000u && !(rd32(ram, sbody + 0x28) & 0x80);
                bool hands_on = A13 && g_v13_hands.load() && g_cfg.hands && g_hands_live.load() && !g_hands_fail.load() &&
                                want == 2 && gun_eye_ok && g_mode_cur.load() == 2 && !g_scoped.load() && body_ok && gh.valid &&
                                t0 / 1000.0 - gh.t_ms < 250;
                static bool hands_was = false;
                if (hands_on) {
                    double c = cos(h_base), sn = sin(h_base), r = -g_ref_yaw_rad.load(), cr = cos(r), sr = sin(r);
                    double B[3][3] = {{-c, 0, sn}, {0, -1, 0}, {sn, 0, c}}, Ry[3][3] = {{cr, 0, sr}, {0, 1, 0}, {-sr, 0, cr}}, G[3][3];
                    for (int i = 0; i < 3; ++i) for (int jj = 0; jj < 3; ++jj) { double v = 0; for (int k = 0; k < 3; ++k) v += B[i][k] * (k == 0 ? 1.0 : -1.0) * Ry[k][jj]; G[i][jj] = v; }
                    int32_t ex = 0, ez = 0; int16_t sc3[3] = {0, 0, 0}; rd(ram, T->snakecam, sc3, sizeof sc3);
                    rd(ram, sbody + 0x284, &ex, 4); rd(ram, sbody + 0x28C, &ez, 4);
                    double eye[3] = {(double)ex + gun_off[0], (double)sc3[1] + gun_off[1], (double)ez + gun_off[2]};
                    GMat hm[2]; bool hv[2] = {gh.valid, gh.lvalid};
                    const XrQuaternionf* hq[2] = {&gh.aim_q, &gh.laim_q}; const XrVector3f* hp[2] = {&gh.grip_p, &gh.lgrip_p};
                    static double prevp[2][3] = {{0}}, prevt[2] = {0, 0}, speed[2] = {0, 0};
                    for (int hh = 0; hh < 2; ++hh) {
                        if (!hv[hh]) { hv[hh] = false; continue; }
                        double rel[3] = {hp[hh]->x - head_p.x, hp[hh]->y - head_p.y, hp[hh]->z - head_p.z};
                        double rl = sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]) * 1000.0;
                        if (rl > g_cfg.gun_max_reach_mm) for (double& v : rel) v *= g_cfg.gun_max_reach_mm / rl;
                        hand_gmat(G, *hq[hh], eye, rel, hm[hh]);
                        double dtv = t0 / 1e6 - prevt[hh];
                        if (dtv > 0.005 && dtv < 0.2) { double dx = hp[hh]->x - prevp[hh][0], dy = hp[hh]->y - prevp[hh][1], dz = hp[hh]->z - prevp[hh][2];
                                                       speed[hh] = sqrt(dx * dx + dy * dy + dz * dz) / dtv; }
                        prevp[hh][0] = hp[hh]->x; prevp[hh][1] = hp[hh]->y; prevp[hh][2] = hp[hh]->z; prevt[hh] = t0 / 1e6;
                    }
                    if (!hv[1]) { hm[1] = hm[0]; }   // no left controller: the left hand stays with the right
                    if (!hv[0]) { hm[0] = hm[1]; }
                    // wall touch: the last frame's line tests (eye -> hand) stop each hand on the surface, and a fast touch knocks
                    static uint32_t hz_sq[2] = {0, 0}, hz_sq2[2] = {0, 0}; static bool hz_pend[2] = {false, false}, contact[2] = {false, false};
                    static int hz_hit[2] = {0, 0}; static int16_t hz_pt[2][3] = {{0}}; static int hz_atr[2] = {0, 0};
                    static double last_knock[2] = {0, 0};
                    uint32_t ctl = T->player_control ? rd32(ram, T->player_control) : 0, map = 0, hzd = 0;
                    if (ctl >= 0x80000000u && ctl < 0x80200000u) map = k0x(rd32(ram, ctl + A13->player_control_map_off));
                    if (map) hzd = rd32(ram, map + 8);
                    bool hzd_ok = hzd >= 0x80000000u && hzd < 0x80200000u && g_cfg.hands_collision;
                    for (int hh = 0; hh < 2 && hzd_ok; ++hh) {
                        int sb = hh ? SL_HZD_L : SL_HZD_R;
                        if (hz_pend[hh]) {
                            uint32_t v0 = 0, v2 = 0;
                            if (gc_done(ram, sb, hz_sq[hh], &v0) && gc_done(ram, sb + 2, hz_sq2[hh], &v2)) {
                                hz_hit[hh] = (int)v0; hz_atr[hh] = (int16_t)(v2 & 0xFFFF); rd(ram, HZDPT + hh * 8, hz_pt[hh], 6);
                                hz_pend[hh] = false; g_hands_tests++;
                                bool now_contact = hz_hit[hh] != 0;
                                if (now_contact && !contact[hh]) {
                                    g_hands_hits++;
                                    double tnow = t0 / 1000.0;
                                    if (g_cfg.hands_knock && speed[hh] * 100.0 >= g_cfg.knock_speed_cms && tnow - last_knock[hh] >= g_cfg.knock_cooldown_ms) {
                                        last_knock[hh] = tnow;
                                        int atr = hz_atr[hh], code = (atr >> 8) & 7;
                                        uint8_t snd = 0; rd(ram, A13->noise_table + ((atr >> 8) & 3) * 3, &snd, 1);   // GM_GetNoiseSound(atr, 0)
                                        g_last_knock_atr = atr;
                                        if (code < 4 && snd) { gc_issue(ram, SL_KNOCK, A13->new_afterse, snd, 6); g_hands_knocks++;
                                                               g_knock_rumble |= hh ? 2 : 1;
                                                               logf("KNOCK: %s hand on the wall at %.1f m/s -> the wall's own sound %d and the game's noise (atr %04X)",
                                                                    hh ? "left" : "right", speed[hh], snd, atr & 0xFFFF); }
                                        else logf("KNOCK: %s hand touched a surface that does not knock (atr %04X)", hh ? "left" : "right", atr & 0xFFFF);
                                    }
                                }
                                contact[hh] = now_contact;
                            }
                        }
                        if (!hz_pend[hh]) {   // next test: eye -> this hand
                            int16_t v[8] = {(int16_t)eye[0], (int16_t)eye[1], (int16_t)eye[2], 0,
                                            (int16_t)hm[hh].t[0], (int16_t)hm[hh].t[1], (int16_t)hm[hh].t[2], 0};
                            uint32_t vb = HZDV + hh * 0x10;
                            wr(ram, vb, v, sizeof v);
                            hz_sq[hh] = gc_issue(ram, sb, A13->hzd_check, hzd, vb, vb + 8, 0xF, 4);
                            gc_issue(ram, sb + 1, A13->hzd_point, HZDPT + hh * 8);
                            hz_sq2[hh] = gc_issue(ram, sb + 2, A13->hzd_atr);
                            hz_pend[hh] = true;
                        }
                        if (contact[hh]) {   // stop on the surface, margin short of it along the eye -> hand line
                            double d[3] = {hz_pt[hh][0] - eye[0], hz_pt[hh][1] - eye[1], hz_pt[hh][2] - eye[2]};
                            double L = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                            double hd2[3] = {hm[hh].t[0] - eye[0], hm[hh].t[1] - eye[1], hm[hh].t[2] - eye[2]};
                            double HL = sqrt(hd2[0] * hd2[0] + hd2[1] * hd2[1] + hd2[2] * hd2[2]);
                            if (L > 1 && HL > L - g_cfg.hands_margin) {
                                double k = std::max(0.0, (L - g_cfg.hands_margin)) / L;
                                for (int i = 0; i < 3; ++i) hm[hh].t[i] = (int32_t)lround(eye[i] + d[i] * k);
                            }
                        }
                    }
                    if (!hzd_ok) { contact[0] = contact[1] = false; hz_pend[0] = hz_pend[1] = false; }
                    // the gun stays in the right hand when the hand is stopped by a wall
                    if (contact[0] && root == GUNMTX) { int32_t gt[3] = {hm[0].t[0], hm[0].t[1], hm[0].t[2]}; wr(ram, GUNMTX + 0x14, gt, sizeof gt); }
                    wr(ram, HMB + 0x20, &hm[0], sizeof hm[0]); wr(ram, HMB + 0x40, &hm[1], sizeof hm[1]);
                    if (!hands_was || rd32(ram, HMB) != 1) {   // first pose, or the mailbox was reset by a hook reinstall
                        uint8_t side[32]; for (int k = 0; k < 32; ++k) side[k] = 0;
                        for (int v : g_cfg.hand_left_parts) if (v >= 0 && v < 32) side[v] = 1;
                        for (int v : g_cfg.hand_keep_parts) if (v >= 0 && v < 32) side[v] = 2;
                        wr(ram, HMB + 0x60, side, sizeof side);
                        uint16_t idx[2] = {(uint16_t)g_cfg.hand_r, (uint16_t)g_cfg.hand_l}; wr(ram, HMB + 8, idx, sizeof idx);
                        wr32(ram, HMB + 4, sbody); wr32(ram, HMB, 1);
                        if (!hands_was) logf("HANDS: on -- Snake's own hands at your controllers (parts %d / %d; body %08X)", g_cfg.hand_r, g_cfg.hand_l, sbody);
                    }
                    if (rd32(ram, HMB + 4) != sbody) wr32(ram, HMB + 4, sbody);
                } else if (hands_was) {
                    wr32(ram, HMB, 0);
                    logf("HANDS: off");
                }
                hands_was = hands_on; g_hands_drawing = hands_on;
            }
            {   // Hide Snake's body from the CAMERA only. DG_BoundChanl draws an object only if group_id == 0 or
                // (group_id & DG_CurrentGroupID); group 0x8000 is a map no stage uses, so he is not drawn -- but his DG_OBJS flag
                // stays "visible", so his weapon, grenade, laser and equipment act exactly as in third person (they all test
                // that flag). His joints are still posed (DG_ScreenChanl runs for every queued object), so the eye stays on his
                // head. GM_ActObject re-groups him only when he changes map; re-applied here at once.
                static uint32_t hid = 0; static uint16_t orig_grp = 1;
                uint32_t so = rd32(ram, MBOX + MB_SNAKEOBJS); int16_t fpv = 0; rd(ram, T->gm_camera + 0x22, &fpv, 2);
                bool fp_walk = g_cfg.hide_body && want == 2 && gun_eye_ok && g_mode_cur.load() == 2 && fpv == 0 &&
                               so >= 0x80000000u && so < 0x80200000u;
                bool want_hide = fp_walk && !g_hands_drawing.load();   // v13: while the hands routine draws him, only his hands show
                if (want_hide) {
                    if (hid && hid != so) hid = 0;
                    uint16_t gid = 0; rd(ram, so + 0x2C, &gid, 2);
                    if (gid != 0x8000) { orig_grp = gid; uint16_t v = 0x8000; wr(ram, so + 0x2C, &v, 2); g_body_rehides++; }
                    hid = so;
                } else if (hid) {
                    uint16_t gid = 0; rd(ram, hid + 0x2C, &gid, 2);
                    if (gid == 0x8000) wr(ram, hid + 0x2C, &orig_grp, 2);
                    hid = 0;
                }
                g_body_hidden = hid;
                // the cardboard box: hang its model from a zero-size matrix while you look out through Snake's eyes
                uint32_t bo = g_box_objs.load(), borig = g_box_orig.load();
                if (bo && borig) {
                    uint32_t broot = rd32(ram, bo + 0x20);
                    if (fp_walk && broot == borig) {
                        uint8_t z[0x20] = {0}; int32_t t[3] = {0, -30000, 0}; memcpy(z + 0x14, t, sizeof t);
                        wr(ram, FARMTX, z, sizeof z); wr32(ram, bo + 0x20, FARMTX);
                        logf("BOX: in the cardboard box -> its model is taken out of your view (you look out through Snake's eyes)");
                    } else if (!fp_walk && broot == FARMTX) {
                        wr32(ram, bo + 0x20, borig); logf("BOX: box model back on Snake");
                    }
                }
            }
            uint32_t next = rd32(ram, MBOX + MB_SLOT) ? 0 : 1;
            wr(ram, MBOX + (next ? MB_M1 : MB_M0), W, sizeof W);
            wr32(ram, MBOX + (next ? MB_SEQ1 : MB_SEQ0), seq);
            wr32(ram, MBOX + MB_SLOT, next);
            if (want != g_mode_cur.load()) { wr32(ram, MBOX + MB_ENABLE, (uint32_t)(want == 3 ? 2 : want)); g_mode_cur = want; }   // matrix first, then mode (v15: diorama = routine mode 2)
            {   // v10 wrist quick-select: equip
                int rq = g_equip_req.exchange(0);
                if (rq) { int side = (rq >> 16) - 1; int16_t id = (int16_t)(rq & 0xFFFF);
                          wr(ram, T->stage_name + 0x10 + (side == 1 ? 14 : 15) * 2, &id, 2); }
            }
            // VR controllers -> pad mailbox (merged into GV_PadData by the pad routine each game frame)
            {
                VrPad vp; AcquireSRWLockShared(&g_vrpad_lock); vp = g_vrpad; ReleaseSRWLockShared(&g_vrpad_lock);
                uint8_t pm[0x0E] = {0};
                memcpy(pm + 4, &vp.buttons, 2); memcpy(pm + 8, &vp.angle, 2);
                uint16_t st = vp.stick ? 1 : 0; memcpy(pm + 0x0A, &st, 2); pm[0x0C] = vp.lx; pm[0x0D] = vp.ly;
                wr(ram, PMBOX + 4, pm + 4, 2); wr(ram, PMBOX + 8, pm + 8, 6);       // never touch +6 (MIPS-owned prev)
                wr32(ram, PMBOX, vp.active ? 1 : 0);
            }
            // stereo: once per game frame, camera movement and frame rate; then the half offset for the next frame
            {
                uint32_t ap = rd32(ram, MBOX + MB_APPLIED);
                if (ap != st_applied) {
                    double now_ms = t0 / 1000.0;
                    if (st_applied && ap - st_applied < 8 && st_applied_t > 0) {
                        double per = (now_ms - st_applied_t) / (ap - st_applied);
                        if (per > 0 && per < 200) fps_ema_ms = fps_ema_ms * 0.9 + per * 0.1;
                    }
                    st_applied = ap; st_applied_t = now_ms;
                    g_game_fps_x10 = (int)(10000.0 / fps_ema_ms);
                    // world eye position, stamped with the game frame. The eye is chosen by the frame's draw buffer, so
                    // frames an even number apart carry the same stereo offset and their difference is real movement.
                    EyeSample cur = {}; uint32_t ap2 = 0;
                    cur.gvt = rd32(ram, T->gv_time); rd(ram, T->chanl1 + 0x30 + 0x14, cur.t, sizeof cur.t); ap2 = rd32(ram, MBOX + MB_APPLIED);
                    if (ap2 == ap && (es_n == 0 || cur.gvt != es[0].gvt)) {
                        memmove(&es[1], &es[0], sizeof(EyeSample) * 7); es[0] = cur; if (es_n < 8) ++es_n;
                        for (int k = 1; k < es_n && g_cfg.hold_units > 0; ++k) {
                            int32_t dg = (int32_t)(es[0].gvt - es[k].gvt);
                            if (dg <= 0 || dg > 6) break;
                            if (dg & 1) continue;
                            double dx = es[0].t[0] - es[k].t[0], dy = es[0].t[1] - es[k].t[1], dz = es[0].t[2] - es[k].t[2];
                            if (sqrt(dx * dx + dy * dy + dz * dz) / dg > g_cfg.hold_units * (g_mode_cur.load() == 3 ? g_dio_scale_x100.load() / 100.0 : 1.0)) { hold_until = now_ms + g_cfg.hold_ms; g_hold_move++; }
                            break;
                        }
                    }
                    if (g_cfg.min_fps > 0 && 1000.0 / fps_ema_ms < g_cfg.min_fps) { hold_until = now_ms + g_cfg.hold_ms; g_hold_fps++; }
                }
                int mode_now = g_mode_cur.load();
                bool ts = g_cfg.v15.true_stereo && g_ts_installed.load() && g_cfg.stereo && g_stereo_on.load() &&   // v20: both eyes every frame
                          (mode_now == 1 || mode_now == 2 || mode_now == 3) && !g_scoped.load() && g_pl_now.load() == 0 &&
                          !(g_demo.load() && g_cut_pov.load() && !g_cfg.v15.cut_stereo) && !g_sel_pause_owned.load();
                {
                    static bool ts_was = false;
                    if (ts != ts_was) {
                        if (!ts) { wr32(ram, TSMB, 0); wr32(ram, TSMB + 8, 0); }
                        logf("TRUE STEREO: %s", ts ? "ON -- both eyes drawn every game frame, side by side" : "off -- alternate eyes");
                        ts_was = ts;
                    }
                    g_ts_on = ts;
                }
                bool on = ts || g_cfg.stereo && g_stereo_on.load() && (mode_now == 1 || mode_now == 2 || mode_now == 3) && t0 / 1000.0 >= hold_until &&
                          !(g_demo.load() && g_cut_pov.load() && !g_cfg.v15.cut_stereo) && !g_sel_pause_owned.load() &&   // v18/v19: Snake's-eyes cutscenes / paused choosing mono
                          qpc_us() / 1000.0 >= g_hand_hold_until.load() &&   // v19: hands moving
                          !g_scoped.load();   // v13: a scope is one eye (GOG)
                double hk = mode_now == 3 ? g_dio_scale_x100.load() / 100.0 : 1.0;   // v15 diorama: the baseline of a giant
                double hv = g_cfg.ipd_mm / 2000.0 * g_cfg.units_per_m * hk; if (hv > 30000) hv = 30000;
                int16_t half = on ? (int16_t)lround(hv) : 0;
                wr(ram, SMB, &half, 2); last_half = half; g_stereo_half = half;   // every time: a reinstall zeroes it
                if (ts) { int32_t two = 2 * half; wr(ram, TSMB + 4, &two, 4); wr32(ram, TSMB, 1); }   // v20: then the right-eye pass is on
            }
            uint32_t us = (uint32_t)(qpc_us() - t0); g_wr_n++;
            uint32_t m = g_wr_us_max.load(); while (us > m && !g_wr_us_max.compare_exchange_weak(m, us)) {}
        }
        Sleep(1);
    }
    return 0;
}

static DWORD WINAPI xr_thread(LPVOID)
{
    std::wstring lp = g_dir + L"openxr_loader.dll";
    HMODULE ld = LoadLibraryW(lp.c_str());
    if (!ld) { logf("XR: openxr_loader.dll not found next to the game; no headset session"); return 0; }
    pxrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)GetProcAddress(ld, "xrGetInstanceProcAddr");
    if (!pxrGetInstanceProcAddr) { logf("XR: loader has no xrGetInstanceProcAddr"); return 0; }
#define XR_LOAD(inst, name) pxrGetInstanceProcAddr(inst, #name, (PFN_xrVoidFunction*)&p##name)
    XR_LOAD(XR_NULL_HANDLE, xrEnumerateInstanceExtensionProperties);
    XR_LOAD(XR_NULL_HANDLE, xrCreateInstance);
    int attempt = 0;
    for (;;) {   // outer loop: (re)create instance + session; retries while no runtime/headset is available
        if (attempt++) Sleep(5000);
        uint32_t n = 0;
        if (!pxrEnumerateInstanceExtensionProperties ||
            XR_FAILED(pxrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr))) {
            if (attempt <= 2 || attempt % 12 == 0) logf("XR: no OpenXR runtime answering (is Virtual Desktop / SteamVR running?) -- retrying");
            continue;
        }
        std::vector<XrExtensionProperties> ext(n, {XR_TYPE_EXTENSION_PROPERTIES});
        pxrEnumerateInstanceExtensionProperties(nullptr, n, &n, ext.data());
        bool has_d3d11 = false;
        for (auto& e : ext) if (!strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) has_d3d11 = true;
        if (!has_d3d11) { logf("XR: runtime lacks %s", XR_KHR_D3D11_ENABLE_EXTENSION_NAME); continue; }

        const char* exts[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
        XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy(ici.applicationInfo.applicationName, "MGS1 MC VR");
        ici.applicationInfo.applicationVersion = 2;
        strcpy(ici.applicationInfo.engineName, "mgs1mc_vr");
        ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        ici.enabledExtensionCount = 1; ici.enabledExtensionNames = exts;
        XrInstance inst = XR_NULL_HANDLE;
        if (!xr_ok(pxrCreateInstance(&ici, &inst), "xrCreateInstance")) continue;
        XR_LOAD(inst, xrDestroyInstance); XR_LOAD(inst, xrGetSystem); XR_LOAD(inst, xrGetSystemProperties);
        XR_LOAD(inst, xrGetInstanceProperties); XR_LOAD(inst, xrCreateSession); XR_LOAD(inst, xrDestroySession);
        XR_LOAD(inst, xrCreateReferenceSpace); XR_LOAD(inst, xrDestroySpace); XR_LOAD(inst, xrBeginSession);
        XR_LOAD(inst, xrEndSession); XR_LOAD(inst, xrPollEvent); XR_LOAD(inst, xrWaitFrame); XR_LOAD(inst, xrBeginFrame);
        XR_LOAD(inst, xrEndFrame); XR_LOAD(inst, xrLocateSpace); XR_LOAD(inst, xrEnumerateSwapchainFormats);
        XR_LOAD(inst, xrCreateSwapchain); XR_LOAD(inst, xrDestroySwapchain); XR_LOAD(inst, xrEnumerateSwapchainImages);
        XR_LOAD(inst, xrAcquireSwapchainImage); XR_LOAD(inst, xrWaitSwapchainImage); XR_LOAD(inst, xrReleaseSwapchainImage);
        XR_LOAD(inst, xrGetD3D11GraphicsRequirementsKHR);
        XR_LOAD(inst, xrStringToPath); XR_LOAD(inst, xrCreateActionSet); XR_LOAD(inst, xrCreateAction); XR_LOAD(inst, xrCreateActionSpace);
        XR_LOAD(inst, xrSuggestInteractionProfileBindings); XR_LOAD(inst, xrAttachSessionActionSets); XR_LOAD(inst, xrSyncActions);
        XR_LOAD(inst, xrGetActionStateBoolean); XR_LOAD(inst, xrGetActionStateFloat); XR_LOAD(inst, xrGetActionStateVector2f);
        XR_LOAD(inst, xrDestroyActionSet); XR_LOAD(inst, xrGetCurrentInteractionProfile); XR_LOAD(inst, xrPathToString); XR_LOAD(inst, xrApplyHapticFeedback);
        XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
        if (XR_SUCCEEDED(pxrGetInstanceProperties(inst, &ip)))
            logf("XR: runtime '%s' %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

        XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO}; sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrSystemId sys = XR_NULL_SYSTEM_ID;
        XrResult sr = pxrGetSystem(inst, &sgi, &sys);
        if (XR_FAILED(sr)) {
            if (attempt <= 2 || attempt % 12 == 0) logf("XR: no headset yet (xrGetSystem %d) -- retrying", (int)sr);
            pxrDestroyInstance(inst); continue;
        }
        XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
        if (XR_SUCCEEDED(pxrGetSystemProperties(inst, sys, &sp))) logf("XR: headset '%s'", sp.systemName);

        XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        if (!xr_ok(pxrGetD3D11GraphicsRequirementsKHR(inst, sys, &req), "xrGetD3D11GraphicsRequirementsKHR")) { pxrDestroyInstance(inst); continue; }
        XrGfx g;
        IDXGIFactory1* fac = nullptr; IDXGIAdapter1* ad = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac))) {
            for (UINT i = 0; fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 ad1; ad->GetDesc1(&ad1);
                if (!memcmp(&ad1.AdapterLuid, &req.adapterLuid, sizeof(LUID))) { logf("XR: GPU '%ls'", ad1.Description); break; }
                ad->Release(); ad = nullptr;
            }
            fac->Release();
        }
        D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        HRESULT hr = D3D11CreateDevice(ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 2, D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx);
        if (ad) ad->Release();
        if (FAILED(hr)) { logf("XR: D3D11CreateDevice failed 0x%08lX", hr); pxrDestroyInstance(inst); continue; }
        bool can_draw = make_shaders(g);

        XrGraphicsBindingD3D11KHR gb{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR}; gb.device = g.dev;
        XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO}; sci.next = &gb; sci.systemId = sys;
        XrSession ses = XR_NULL_HANDLE;
        if (!xr_ok(pxrCreateSession(inst, &sci, &ses), "xrCreateSession")) { g.ctx->Release(); g.dev->Release(); pxrDestroyInstance(inst); continue; }
        XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO}; rs.poseInReferenceSpace.orientation.w = 1;
        XrSpace local = XR_NULL_HANDLE, view = XR_NULL_HANDLE;
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; xr_ok(pxrCreateReferenceSpace(ses, &rs, &local), "create LOCAL space");
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;  xr_ok(pxrCreateReferenceSpace(ses, &rs, &view), "create VIEW space");
        XrInput input; xr_setup_input(inst, ses, input);

        // swapchain for the game screen
        uint32_t nf = 0; pxrEnumerateSwapchainFormats(ses, 0, &nf, nullptr);
        std::vector<int64_t> fmts(nf); pxrEnumerateSwapchainFormats(ses, nf, &nf, fmts.data());
        int64_t pick = 0;
        for (int64_t want : {(int64_t)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                             (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM, (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM})
            { for (int64_t f : fmts) if (f == want) { pick = f; break; } if (pick) break; }
        if (!pick && !fmts.empty()) pick = fmts[0];
        bool srgb = pick == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || pick == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        const uint32_t SW = 1440, SH = 1080;          // per eye; the swapchain holds both eyes side by side
        XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sc.format = pick; sc.sampleCount = 1; sc.width = SW * 2; sc.height = SH; sc.faceCount = 1; sc.arraySize = 1; sc.mipCount = 1;
        XrSwapchain swc = XR_NULL_HANDLE;
        std::vector<ID3D11RenderTargetView*> rtvs;
        if (xr_ok(pxrCreateSwapchain(ses, &sc, &swc), "xrCreateSwapchain")) {
            uint32_t ni = 0; pxrEnumerateSwapchainImages(swc, 0, &ni, nullptr);
            std::vector<XrSwapchainImageD3D11KHR> imgs(ni, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            pxrEnumerateSwapchainImages(swc, ni, &ni, (XrSwapchainImageBaseHeader*)imgs.data());
            for (auto& im : imgs) {
                D3D11_RENDER_TARGET_VIEW_DESC rv = {}; rv.Format = (DXGI_FORMAT)pick; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                ID3D11RenderTargetView* r = nullptr; g.dev->CreateRenderTargetView(im.texture, &rv, &r); rtvs.push_back(r);
            }
            logf("XR: screen swapchain %ux%u (two eyes of %ux%u) format %lld (%s), %u images", SW * 2, SH, SW, SH, (long long)pick, srgb ? "sRGB" : "linear", ni);
        }
        XrSwapchain pswc = XR_NULL_HANDLE; std::vector<ID3D11RenderTargetView*> prtvs; PanelGfx pgfx;
        XrSwapchain wswc = XR_NULL_HANDLE; std::vector<ID3D11RenderTargetView*> wrtvs;
        XrSwapchain hswc = XR_NULL_HANDLE; std::vector<ID3D11RenderTargetView*> hrtvs;   // v15 hologram wheel (left eye rows, then right)
        ID3D11Texture2D* htex = nullptr; ID3D11ShaderResourceView* hsrv = nullptr; std::vector<uint8_t> hpx;
        XrSwapchain cswc = XR_NULL_HANDLE; std::vector<ID3D11RenderTargetView*> crtvs;   // v16 carousel atlas
        XrSwapchain sswc = XR_NULL_HANDLE; std::vector<ID3D11RenderTargetView*> srtvs;   // v17 subtitle panel
        ID3D11Texture2D* ctex = nullptr; ID3D11ShaderResourceView* csrv = nullptr;
        {
            XrSwapchainCreateInfo wc = sc; wc.width = WW; wc.height = WH;
            if (xr_ok(pxrCreateSwapchain(ses, &wc, &wswc), "xrCreateSwapchain (wrist)")) {
                uint32_t ni = 0; pxrEnumerateSwapchainImages(wswc, 0, &ni, nullptr);
                std::vector<XrSwapchainImageD3D11KHR> imgs(ni, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
                pxrEnumerateSwapchainImages(wswc, ni, &ni, (XrSwapchainImageBaseHeader*)imgs.data());
                for (auto& im : imgs) {
                    D3D11_RENDER_TARGET_VIEW_DESC rv = {}; rv.Format = (DXGI_FORMAT)pick; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                    ID3D11RenderTargetView* r = nullptr; g.dev->CreateRenderTargetView(im.texture, &rv, &r); wrtvs.push_back(r);
                }
            }
        }
        {
            XrSwapchainCreateInfo hc = sc; hc.width = WristWheel::kW; hc.height = WristWheel::kH * 2;
            if (xr_ok(pxrCreateSwapchain(ses, &hc, &hswc), "xrCreateSwapchain (wheel)")) {
                uint32_t ni = 0; pxrEnumerateSwapchainImages(hswc, 0, &ni, nullptr);
                std::vector<XrSwapchainImageD3D11KHR> imgs(ni, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
                pxrEnumerateSwapchainImages(hswc, ni, &ni, (XrSwapchainImageBaseHeader*)imgs.data());
                for (auto& im : imgs) {
                    D3D11_RENDER_TARGET_VIEW_DESC rv = {}; rv.Format = (DXGI_FORMAT)pick; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                    ID3D11RenderTargetView* r = nullptr; g.dev->CreateRenderTargetView(im.texture, &rv, &r); hrtvs.push_back(r);
                }
                D3D11_TEXTURE2D_DESC td = {}; td.Width = WristWheel::kW; td.Height = WristWheel::kH * 2; td.MipLevels = 1; td.ArraySize = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                if (SUCCEEDED(g.dev->CreateTexture2D(&td, nullptr, &htex))) g.dev->CreateShaderResourceView(htex, nullptr, &hsrv);
                hpx.assign((size_t)WristWheel::kW * WristWheel::kH * 2 * 4, 0);
                logf("WHEEL: hologram wheel swapchain %dx%d %s", WristWheel::kW, WristWheel::kH * 2, hsrv ? "ready" : "(no texture: wheel off)");
            }
            XrSwapchainCreateInfo ssc = sc; ssc.width = SUBW; ssc.height = SUBH;
            if (xr_ok(pxrCreateSwapchain(ses, &ssc, &sswc), "xrCreateSwapchain (subtitles)")) {
                uint32_t ni = 0; pxrEnumerateSwapchainImages(sswc, 0, &ni, nullptr);
                std::vector<XrSwapchainImageD3D11KHR> imgs(ni, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
                pxrEnumerateSwapchainImages(sswc, ni, &ni, (XrSwapchainImageBaseHeader*)imgs.data());
                for (auto& im : imgs) {
                    D3D11_RENDER_TARGET_VIEW_DESC rv = {}; rv.Format = (DXGI_FORMAT)pick; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                    ID3D11RenderTargetView* r = nullptr; g.dev->CreateRenderTargetView(im.texture, &rv, &r); srtvs.push_back(r);
                }
                logf("SUBTITLES: caption panel swapchain %dx%d ready", SUBW, SUBH);
            }
            XrSwapchainCreateInfo cc = sc; cc.width = WristWheel::kAtlasW; cc.height = WristWheel::kAtlasH;
            if (xr_ok(pxrCreateSwapchain(ses, &cc, &cswc), "xrCreateSwapchain (carousel)")) {
                uint32_t ni = 0; pxrEnumerateSwapchainImages(cswc, 0, &ni, nullptr);
                std::vector<XrSwapchainImageD3D11KHR> imgs(ni, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
                pxrEnumerateSwapchainImages(cswc, ni, &ni, (XrSwapchainImageBaseHeader*)imgs.data());
                for (auto& im : imgs) {
                    D3D11_RENDER_TARGET_VIEW_DESC rv = {}; rv.Format = (DXGI_FORMAT)pick; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                    ID3D11RenderTargetView* r = nullptr; g.dev->CreateRenderTargetView(im.texture, &rv, &r); crtvs.push_back(r);
                }
                D3D11_TEXTURE2D_DESC td = {}; td.Width = WristWheel::kAtlasW; td.Height = WristWheel::kAtlasH; td.MipLevels = 1; td.ArraySize = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                if (SUCCEEDED(g.dev->CreateTexture2D(&td, nullptr, &ctex))) g.dev->CreateShaderResourceView(ctex, nullptr, &csrv);
                                g_car.atlas.assign((size_t)WristWheel::kAtlasW * WristWheel::kAtlasH * 4, 0);
                logf("CAROUSEL: 3D carousel atlas %dx%d %s", WristWheel::kAtlasW, WristWheel::kAtlasH, csrv ? "ready" : "(no texture: carousel off)");
            }
        }
        {
            XrSwapchainCreateInfo pc = sc; pc.width = PW; pc.height = PH;
            if (xr_ok(pxrCreateSwapchain(ses, &pc, &pswc), "xrCreateSwapchain (panel)")) {
                uint32_t ni = 0; pxrEnumerateSwapchainImages(pswc, 0, &ni, nullptr);
                std::vector<XrSwapchainImageD3D11KHR> imgs(ni, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
                pxrEnumerateSwapchainImages(pswc, ni, &ni, (XrSwapchainImageBaseHeader*)imgs.data());
                for (auto& im : imgs) {
                    D3D11_RENDER_TARGET_VIEW_DESC rv = {}; rv.Format = (DXGI_FORMAT)pick; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                    ID3D11RenderTargetView* r = nullptr; g.dev->CreateRenderTargetView(im.texture, &rv, &r); prtvs.push_back(r);
                }
            }
        }

        bool running = false, quit = false;
        XrSessionState state = XR_SESSION_STATE_UNKNOWN;
        struct St { double sum = 0, mx = 0; int n = 0; void add(double v) { sum += v; if (v > mx) mx = v; ++n; }
                    double avg() const { return n ? sum / n : 0; } };
        St st_wait, st_begin, st_locate, st_img, st_draw, st_end, st_loop; double period_ms = 0; int not_render = 0;
        ULONGLONG last_log = 0; uint32_t frames = 0, frames_win = 0, pic_frames0 = g_cap_frames.load();
        while (!quit) {
            XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
            while (pxrPollEvent(inst, &ev) == XR_SUCCESS) {
                if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                    state = ((XrEventDataSessionStateChanged*)&ev)->state;
                    logf("XR: session state %d", (int)state);
                    if (state == XR_SESSION_STATE_READY) {
                        XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO}; bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        running = xr_ok(pxrBeginSession(ses, &bi), "xrBeginSession");
                        if (running) { g_recentre = true; logf("XR: session running -- look straight ahead; forward is set from your first pose"); }
                    } else if (state == XR_SESSION_STATE_STOPPING) {
                        pxrEndSession(ses); running = false;
                    } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
                        quit = true;
                    }
                } else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
                    XrInteractionProfileState ip{XR_TYPE_INTERACTION_PROFILE_STATE}; XrPath up = XR_NULL_PATH;
                    pxrStringToPath(inst, "/user/hand/right", &up);
                    if (XR_SUCCEEDED(pxrGetCurrentInteractionProfile(ses, up, &ip)) && ip.interactionProfile != XR_NULL_PATH) {
                        char buf[256] = {0}; uint32_t n = 0; pxrPathToString(inst, ip.interactionProfile, sizeof buf, &n, buf);
                        logf("INPUT: controllers use %s", buf);
                    } else logf("INPUT: interaction profile changed (none active)");
                } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) quit = true;
                ev = {XR_TYPE_EVENT_DATA_BUFFER};
            }
            if (quit) break;
            if (!running) { g_head_valid = false; Sleep(20); continue; }

            double t_loop = qpc_us();
            XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO}; XrFrameState fs{XR_TYPE_FRAME_STATE};
            if (!xr_ok(pxrWaitFrame(ses, &fwi, &fs), "xrWaitFrame")) { Sleep(10); continue; }
            double t1 = qpc_us(); st_wait.add((t1 - t_loop) / 1000.0); period_ms = fs.predictedDisplayPeriod / 1e6;
            if (!fs.shouldRender) ++not_render;
            XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
            pxrBeginFrame(ses, &fbi);
            double t2 = qpc_us(); st_begin.add((t2 - t1) / 1000.0);

            xr_poll_input(ses, input, state == XR_SESSION_STATE_FOCUSED);
            {
                uint16_t lb = (uint16_t)g_vrpad_buttons_log.exchange(-1);
                if (lb != 0xFFFF) logf("INPUT: PS1 buttons %04X", lb);
            }
            XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
            if (XR_SUCCEEDED(pxrLocateSpace(view, local, fs.predictedDisplayTime, &loc)) &&
                (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
                { publish_head(loc.pose.orientation, loc.pose.position); g_last_head = loc.pose; }
            for (int hh = 0; hh < 2; ++hh) {
                g_hand_ok[hh] = false;
                if (!input.hand[hh]) continue;
                XrSpaceLocation hl{XR_TYPE_SPACE_LOCATION};
                if (XR_SUCCEEDED(pxrLocateSpace(input.hand[hh], local, fs.predictedDisplayTime, &hl)) &&
                    (hl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) && (hl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
                    { g_hand[hh] = hl.pose; g_hand_ok[hh] = true; }
            }
            {   // v19: a controller moving fast -> both eyes the same picture (the hands are in the game picture)
                static XrVector3f pp[2]; static double pt[2] = {0, 0}; static float spd[2] = {0, 0};
                double tn = qpc_us() / 1000.0;
                for (int hh = 0; hh < 2; ++hh) {
                    if (!g_hand_ok[hh]) { pt[hh] = 0; continue; }
                    XrVector3f q = g_hand[hh].position;
                    if (pt[hh] > 0 && tn - pt[hh] > 1 && tn - pt[hh] < 200) {
                        float dx = q.x - pp[hh].x, dy = q.y - pp[hh].y, dz = q.z - pp[hh].z;
                        float v = sqrtf(dx * dx + dy * dy + dz * dz) / (float)((tn - pt[hh]) / 1000.0);
                        spd[hh] = 0.5f * spd[hh] + 0.5f * v;
                    }
                    pp[hh] = q; pt[hh] = tn;
                }
                float lim = g_cfg.v15.hold_hand_cms / 100.0f;
                if (lim > 0 && g_cfg.hands && g_mode_cur.load() == 2 && (spd[0] > lim || spd[1] > lim)) {
                    if (tn >= g_hand_hold_until.load()) g_hold_hand++;
                    g_hand_hold_until = tn + g_cfg.hold_ms;
                }
            }
            {   // melee gestures and rumble: merged into the PS1 pad for this frame
                bool gate = state == XR_SESSION_STATE_FOCUSED && g_mode_cur.load() == 2 && !g_cut_pov.load() && !g_keep_pose.load() &&
                            !g_panel.open && g_cfg.controls;
                uint16_t mb = melee_update(ses, input, g_hand, g_hand_ok, loc.pose, gate && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT));
                g_melee_bits = mb;                       // xr_poll_input ORs these into the pad on the next frame
                if (int kr = g_knock_rumble.exchange(0)) { if (kr & 1) xr_buzz(ses, input, 1, 0.9f, 45); if (kr & 2) xr_buzz(ses, input, 0, 0.9f, 45); }
            }
            {   // gun in hand: right grip position + right aim orientation, predicted gun_predict_ms ahead
                GunHand gh; gh.t_ms = qpc_us() / 1000.0;
                if (input.raim && input.hand[1]) {
                    XrTime tt = fs.predictedDisplayTime + (XrTime)g_cfg.gun_predict_ms * 1000000;
                    XrSpaceLocation al{XR_TYPE_SPACE_LOCATION}, gl{XR_TYPE_SPACE_LOCATION};
                    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
                    if (XR_SUCCEEDED(pxrLocateSpace(input.raim, local, tt, &al)) && (al.locationFlags & need) == need &&
                        XR_SUCCEEDED(pxrLocateSpace(input.hand[1], local, tt, &gl)) && (gl.locationFlags & need) == need) {
                        gh.valid = true; gh.aim_q = al.pose.orientation; gh.grip_p = gl.pose.position;
                    }
                }
                if (input.laim && input.hand[0]) {   // v13: the left hand, the same way
                    XrTime tt = fs.predictedDisplayTime + (XrTime)g_cfg.gun_predict_ms * 1000000;
                    XrSpaceLocation al{XR_TYPE_SPACE_LOCATION}, gl{XR_TYPE_SPACE_LOCATION};
                    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
                    if (XR_SUCCEEDED(pxrLocateSpace(input.laim, local, tt, &al)) && (al.locationFlags & need) == need &&
                        XR_SUCCEEDED(pxrLocateSpace(input.hand[0], local, tt, &gl)) && (gl.locationFlags & need) == need) {
                        gh.lvalid = true; gh.laim_q = al.pose.orientation; gh.lgrip_p = gl.pose.position;
                    }
                }
                AcquireSRWLockExclusive(&g_gunhand_lock); g_gunhand = gh; ReleaseSRWLockExclusive(&g_gunhand_lock);
            }
            if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) g_head_valid = false;
            double t3 = qpc_us(); st_locate.add((t3 - t2) / 1000.0); double t_img = t3, t_drawn = t3;

            XrCompositionLayerProjectionView pviews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
            XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            const XrCompositionLayerBaseHeader* layers[16]; uint32_t nlayers = 0;
            XrCompositionLayerQuad cquad[10];
            XrCompositionLayerQuad squad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            XrCompositionLayerQuad hquad[2] = {{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}};
            XrCompositionLayerQuad wquad[2] = {{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}};
            XrCompositionLayerQuad pquad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            if (fs.shouldRender && swc != XR_NULL_HANDLE && can_draw) {
                uint32_t idx = 0; XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                if (XR_SUCCEEDED(pxrAcquireSwapchainImage(swc, &ai, &idx))) {
                    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = XR_INFINITE_DURATION;
                    pxrWaitSwapchainImage(swc, &wi);
                    t_img = qpc_us(); st_img.add((t_img - t3) / 1000.0);
                    refresh_source(g);
                    if (!g_test_blank.load()) take_picture(g);
                    ULONGLONG tnow = GetTickCount64();
                    bool save = g_save_frame.exchange(false);
                    bool periodic = g_cfg.crop_auto == 1 && g_scan_ok.load() && (g.unchanged < 20 || g.epoch != g_scan_epoch.load());
                    if (g.priv && (save || (periodic && tnow - g.last_scan >= 2000))) { scan_picture(g, save); g.last_scan = tnow; }
                    // crop rectangle (pixels of the emulator output)
                    float cx0 = 0, cy0 = 0, cx1 = 1, cy1 = 1; double crop_aspect = 0, top_cut = 1.0;
                    {
                        uint32_t cw = g_cap_w.load(), chh = g_cap_h.load();
                        int x0 = 0, y0 = 0, w = (int)cw, h = (int)chh;
                        if (g_cfg.crop_auto == 0) { x0 = g_cfg.crop_x; y0 = g_cfg.crop_y; w = g_cfg.crop_w; h = g_cfg.crop_h; }
                        else if (g_menus_active.load()) { static bool lg = false; if (!lg) { lg = true; logf("SCREEN: Master Collection menu -> the whole 16:9 picture"); } }   // v18
                        else if (g_cfg.crop_auto == 2 && cw && chh) {          // centred 4:3, the 224 visible of 240 lines
                            int fw = (int)(chh * 4 / 3); if (fw > (int)cw) fw = (int)cw;
                            x0 = ((int)cw - fw) / 2; w = fw; y0 = (int)(chh * 10 / 240); h = (int)(chh * 222 / 240);   // rows 2-223:
                            top_cut = 110.0 / 112.0;                                   // the marker's 2 rows stay hidden
                        }
                        else if (g_cfg.crop_auto == 1 && g.bx1 > g.bx0 + 64 && g.by1 > g.by0 + 64) { x0 = g.bx0; y0 = g.by0; w = g.bx1 + 2 - g.bx0; h = g.by1 + 2 - g.by0; }
                        if (cw && chh && w > 0 && h > 0) {
                            cx0 = (float)x0 / cw; cy0 = (float)y0 / chh; cx1 = (float)(x0 + w) / cw; cy1 = (float)(y0 + h) / chh;
                            crop_aspect = (double)h / w;
                        }
                    }
                    // which picture each eye shows: its own newest stereo picture, unless that is too old (then the newest)
                    double tnow_ms = qpc_us() / 1000.0;
                    ID3D11ShaderResourceView* esrv[2]; uint32_t eseq[2], escr[2]; int etr[2], ecode[2]; int ehalf[2] = {-1, -1};
                    for (int e = 0; e < 2; ++e) {
                        const auto& et = g.eyet[e];
                        bool own = et.valid && g.eyeSrv[e] && (g.newest_t - et.t) <= g_cfg.eye_timeout_ms && (tnow_ms - et.t) < 1000;
                        if (!own && et.valid && et.code != 0) g.eye_timeouts++;
                        esrv[e] = own ? g.eyeSrv[e] : g.srv;
                        eseq[e] = own ? et.seq : g.tag_seq; escr[e] = own ? et.screen : g.tag_screen;
                        etr[e] = own ? et.tracked : g.tag_tracked; ecode[e] = own ? et.code : 0;
                        if (own && et.sbs) ehalf[e] = et.half;   // v20
                    }
                    const bool scope_view = g_scoped.load() && g_cfg.scope_lens && g.srv;   // v13: PSG1 lens
                    if (scope_view) { esrv[0] = esrv[1] = g.srv; ecode[0] = ecode[1] = 0; }
                    ID3D11RenderTargetView* rtv = idx < rtvs.size() ? rtvs[idx] : nullptr;
                    if (rtv) {
                        if (g_test_blank.load()) {
                            float teal[4] = {0.02f, 0.10f, 0.12f, 1}; g.ctx->ClearRenderTargetView(rtv, teal);
                        } else if (g.srv) {
                            float opts[8] = { srgb ? 1.f : 0.f, 0, 0, 0, cx0, cy0, cx1, cy1 };
                            if (scope_view) { uint32_t cw0 = g_cap_w.load(), ch0 = g_cap_h.load();
                                              opts[2] = (cy1 > cy0 && ch0) ? (float)(((cx1 - cx0) * cw0) / ((cy1 - cy0) * ch0)) : 1.333f; opts[3] = 1; }
                            g.ctx->UpdateSubresource(g.cb, 0, nullptr, opts, 0, 0);
                            float mk[24] = {0};
                            int nmask = (g.v10_shader && g.cb1 && g_hud_ctx.load()) ? hud_masks(mk + 8) : 0; bool sub_masked = false;
                            if (g.v10_shader && g.cb1 && g_cfg.v15.sub_on && g_cut_pov.load() && g_demo.load() && nmask < 4 && sswc != XR_NULL_HANDLE) {
                                float suv[4]; if (sub_strip_uv(suv)) { memcpy(mk + 8 + nmask * 4, suv, sizeof suv); ++nmask; sub_masked = true; }   // v17: on the panel instead
                            }
                            if (nmask) {
                                uint32_t cw = g_cap_w.load(), chh = g_cap_h.load();
                                mk[0] = cw ? 1.f / cw : 0; mk[1] = chh ? 1.f / chh : 0; mk[2] = (float)nmask; mk[3] = sub_masked ? 0.f : (float)g_cfg.hud_hide_style;   // v18: the caption bar is filled black, never in-painted
                                mk[4] = cx0; mk[5] = cy0; mk[6] = cx1; mk[7] = cy1;
                                g.ctx->UpdateSubresource(g.cb1, 0, nullptr, mk, 0, 0); g.ctx->PSSetConstantBuffers(1, 1, &g.cb1);
                                static int masks_logged = 0;
                                if (!masks_logged) { masks_logged = 1; logf("HUD: the game's own %d HUD box(es) painted out of your view (they are on your wrists)", nmask); }
                            }
                            g.ctx->OMSetRenderTargets(1, &rtv, nullptr);
                            g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); g.ctx->IASetInputLayout(nullptr);
                            g.ctx->VSSetShader(g.vs, nullptr, 0); g.ctx->PSSetShader(g.ps, nullptr, 0);
                            g.ctx->PSSetSamplers(0, 1, &g.smp); g.ctx->PSSetConstantBuffers(0, 1, &g.cb);
                            for (int e = 0; e < 2; ++e) {
                                D3D11_VIEWPORT vp = {(float)(e * SW), 0, (float)SW, (float)SH, 0, 1};
                                g.ctx->RSSetViewports(1, &vp);
                                if (ehalf[0] >= 0 || ehalf[1] >= 0) {   // v20: side by side -- this eye samples its own half
                                    float o2[8]; memcpy(o2, opts, sizeof o2);
                                    if (ehalf[e] >= 0) { float hw = (cx1 - cx0) * 0.5f; o2[4] = cx0 + hw * ehalf[e]; o2[6] = o2[4] + hw; }
                                    g.ctx->UpdateSubresource(g.cb, 0, nullptr, o2, 0, 0);
                                }
                                g.ctx->PSSetShaderResources(0, 1, &esrv[e]);
                                g.ctx->Draw(3, 0);
                            }
                            ID3D11ShaderResourceView* none = nullptr; g.ctx->PSSetShaderResources(0, 1, &none);
                            if (nmask) { float z[24] = {0}; g.ctx->UpdateSubresource(g.cb1, 0, nullptr, z, 0, 0); }
                        } else {
                            float grey[4] = {0.03f, 0.03f, 0.05f, 1}; g.ctx->ClearRenderTargetView(rtv, grey);
                        }
                    }
                    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    pxrReleaseSwapchainImage(swc, &ri);
                    t_drawn = qpc_us(); st_draw.add((t_drawn - t_img) / 1000.0);
                    double aspect = crop_aspect > 0 ? crop_aspect : 0.75;
                    // world lock (GOG FrameViewRecord design): each eye's picture is posed where the game camera looked
                    // when it drew it, claiming exactly the field of view it was projected with. A stereo picture was
                    // drawn half an eye-distance to the side, so its view sits half an IPD along the head's right.
                    int screen_mode = 0; double scr_tan_h = 0, scr_tan_v = 0;
                    static XrQuaternionf scr_q = {0, 0, 0, 1}; static XrVector3f scr_p = {0, 0, 0}; static bool scr_was_used = false;
                    bool newest_tracked = g.tag_tracked && g.tag_seq && g_hist[g.tag_seq & 255].seq == g.tag_seq;
                    if (!newest_tracked && (!scr_was_used || g_screen_anchor.exchange(false))) {
                        XrQuaternionf hq = g_last_head.orientation;
                        double fx = -2 * (hq.x * hq.z + hq.w * hq.y), fz = -(1 - 2 * (hq.x * hq.x + hq.y * hq.y));
                        double yaw = atan2(-fx, -fz);
                        scr_q = {0, (float)sin(yaw / 2), 0, (float)cos(yaw / 2)}; scr_p = g_last_head.position;
                    }
                    scr_was_used = !newest_tracked;
                    if (!newest_tracked) {
                        static double away_since = 0;
                        XrQuaternionf hq = g_last_head.orientation;
                        double fx = -2 * (hq.x * hq.z + hq.w * hq.y), fz = -(1 - 2 * (hq.x * hq.x + hq.y * hq.y));
                        double hyaw = atan2(-fx, -fz), syaw = 2 * atan2((double)scr_q.y, (double)scr_q.w);
                        double d = fabs(remainder(hyaw - syaw, 2 * 3.14159265358979)) * 57.29578;
                        double nowm = qpc_us() / 1000.0;
                        if (d > 70) { if (!away_since) away_since = nowm; else if (nowm - away_since > 1500) {
                            g_screen_anchor = true; away_since = 0;
                            logf("SCREEN: you were looking %.0f deg away from the floating screen -> moved it in front of you", d); } }
                        else away_since = 0;
                    }
                    for (int e = 0; e < 2; ++e) {
                        const PoseRec* pr = nullptr;
                        uint32_t tseq = eseq[e];
                        if (etr[e] && tseq && g_hist[tseq & 255].seq == tseq) pr = &g_hist[tseq & 255];
                        XrQuaternionf qo; XrVector3f po; double tan_h, tan_v;
                        if (scope_view) {   // v13: head-locked lens, the zoomed picture shown across display_deg
                            tan_h = tan(g_cfg.scope_display_deg * 0.5 * 3.14159265358979 / 180.0); tan_v = tan_h * aspect;
                            qo = g_last_head.orientation; po = g_last_head.position; pr = nullptr; screen_mode = 1;
                        } else if (pr) {
                            double scr = (double)escr[e]; if (scr < 30) scr = 320;
                            tan_h = 160.0 / scr;                       // GTE: screen x = 160 +/- x*H/z
                            tan_v = (112.0 * 64.0 / 58.0) / scr;       // 224 lines, rows squeezed 58/64 by DG_AdjustOverscan
                            qo = pr->q; po = pr->p;
                            if (ecode[e] != 0) {
                                float side = (e == 0 ? -1.f : 1.f) * (float)(g_cfg.ipd_mm / 2000.0);
                                // rotate (side, 0, 0) by qo
                                float x = qo.x, y = qo.y, z = qo.z, w = qo.w;
                                po.x += side * (1 - 2 * (y * y + z * z));
                                po.y += side * (2 * (x * y + w * z));
                                po.z += side * (2 * (x * z - w * y));
                            }
                            screen_mode = 1;
                        } else {
                            tan_h = (g_cfg.screen_width_m * 0.5) / g_cfg.screen_distance_m;   // floating screen: angular size
                            tan_v = tan_h * aspect;
                            qo = scr_q; po = scr_p;
                        }
                        scr_tan_h = tan_h; scr_tan_v = tan_v;
                        pviews[e].pose.orientation = qo; pviews[e].pose.position = po;
                        pviews[e].fov = {(float)-atan(tan_h), (float)atan(tan_h), (float)atan(pr ? tan_v * top_cut : tan_v), (float)-atan(tan_v)};
                        pviews[e].subImage.swapchain = swc;
                        pviews[e].subImage.imageRect.offset = {(int32_t)(e * SW), 0};
                        pviews[e].subImage.imageRect.extent = {(int32_t)SW, (int32_t)SH};
                        pviews[e].subImage.imageArrayIndex = 0;
                    }
                    proj.space = local; proj.viewCount = 2; proj.views = pviews;
                    layers[nlayers++] = (const XrCompositionLayerBaseHeader*)&proj;
                    // v10 wrist panels, 1:1 with the GOG mod (vr_aim.cpp BuildWristHudLayers / GazeShows): one 512x320 canvas
                    // (left half = left wrist, right half = right wrist) drawn translucent with the pixel font; the radar and the
                    // game's item / weapon boxes are cut from the newest picture into it on the GPU. Each panel sits on its mount
                    // (0 top of the hand, 1 back of the wrist, 2 inside of the wrist); a list you are scrolling moves to the list
                    // mount; a panel shows while it is inside the gaze cone (8 deg hysteresis) and faces you.
                    {
                        const bool hud_on = g_cfg.hud_wrist && screen_mode == 1 && wswc != XR_NULL_HANDLE && g.srv && !g_test_blank.load();
                        g_hud_ctx = hud_on;
                        HudData w_hd; AcquireSRWLockShared(&g_hud_lock); w_hd = g_hud; ReleaseSRWLockShared(&g_hud_lock);
                        if (!g_cfg.hud_select_on_wrist) {   // select_on_wrist=0: the game's own open list (MenuWork) on that wrist
                            int ls = w_hd.menu == 2 ? 0 : w_hd.menu == 1 ? 1 : -1;
                            for (int sd = 0; sd < 2; ++sd) {
                                WristList& L = g_wl[sd]; L.open = hud_on && ls == sd && w_hd.ln > 0;
                                if (L.open) { L.count = std::min(w_hd.ln, 24); for (int k = 0; k < L.count; ++k) L.ids[k] = w_hd.lid[k];
                                              L.sel = std::max(0, std::min(w_hd.lsel, L.count - 1)); }
                            }
                        }
                        static WristCanvas wcv; static bool wshown[2] = {false, false}, gpu_stale = true, w_have = false;
                        static uint64_t w_sig = 0; static double w_last = -1e9; static int w_logged = 0, w_cfg_logged = 0;
                        XrPosef ppose[2]; bool pshow[2] = {false, false};
                        if (!hud_on) { wshown[0] = wshown[1] = false; }
                        else {
                            if (!w_cfg_logged) {
                                w_cfg_logged = 1;
                                logf("HUD: wrist panels as in the GOG mod | mounts: left %d, right %d, open list %d (0 top of the hand, 1 back of "
                                     "the wrist, 2 inside of the wrist) | width %d mm, gaze cone %d deg | game boxes %d, radar %d | hide in view: "
                                     "boxes %d, radar %d, LIFE %d (%s) | quick-select on the wrist %d",
                                     g_cfg.hud_mount_side[0], g_cfg.hud_mount_side[1], g_cfg.hud_list_mount, g_cfg.hud_width_mm, g_cfg.hud_gaze_deg,
                                     g_cfg.hud_show_boxes, g_cfg.hud_show_radar, g_cfg.hud_hide_boxes, g_cfg.hud_hide_radar, g_cfg.hud_hide_life,
                                     g_cfg.hud_hide_style ? "filled from the surroundings" : "black", g_cfg.hud_select_on_wrist);
                            }
                            // CPU canvas at most ~30 Hz (GOG), uploaded only when it changed
                            double nowm = qpc_us() / 1000.0;
                            if (nowm - w_last >= 33 || !wcv.drawn) {
                                w_last = nowm; gpu_stale = true;
                                uint64_t sig = wrist_draw(wcv, w_hd);
                                if (!wcv.tex) {
                                    D3D11_TEXTURE2D_DESC td = {}; td.Width = WW; td.Height = WH; td.MipLevels = 1; td.ArraySize = 1;
                                    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
                                    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                                    if (SUCCEEDED(g.dev->CreateTexture2D(&td, nullptr, &wcv.tex))) g.dev->CreateShaderResourceView(wcv.tex, nullptr, &wcv.srv);
                                    if (!wcv.srv) logf("HUD: could not create the wrist canvas texture -- wrist panels off");
                                    wcv.drawn = false;
                                }
                                if (wcv.tex && (sig != w_sig || !wcv.drawn)) {
                                    g.ctx->UpdateSubresource(wcv.tex, 0, nullptr, wcv.px.data(), WW * 4, 0);
                                    w_sig = sig; wcv.drawn = true;
                                }
                            }
                            const XrPosef hp = g_last_head;
                            for (int hnd = 0; hnd < 2; ++hnd) {
                                if (!g_hand_ok[hnd] || wcv.panelH[hnd] <= 0) { wshown[hnd] = false; continue; }
                                const bool list_open = g_wl[hnd].open && !g_cfg.v15.wheel;
                                const int mnt = list_open ? g_cfg.hud_list_mount : g_cfg.hud_mount_side[hnd];
                                const bool back = mnt == 1, inside = mnt == 2;
                                const double* oc = inside ? g_cfg.hud_in_off_cm[hnd] : back ? g_cfg.hud_back_off_cm[hnd] : g_cfg.hud_off_cm[hnd];
                                const double* rd = inside ? g_cfg.hud_in_rot_deg[hnd] : back ? g_cfg.hud_back_rot_deg[hnd] : g_cfg.hud_rot_deg[hnd];
                                const XrQuaternionf gq = g_hand[hnd].orientation;
                                XrVector3f o = qrot(gq, XrVector3f{(float)(oc[0] / 100), (float)(oc[1] / 100), (float)(oc[2] / 100)});
                                ppose[hnd].position = {g_hand[hnd].position.x + o.x, g_hand[hnd].position.y + o.y, g_hand[hnd].position.z + o.z};
                                XrQuaternionf face = {0, 0, 0, 1};                                  // (x, y, z, w)
                                if (inside) face = hnd == 1 ? XrQuaternionf{-0.5f, -0.5f, 0.5f, 0.5f} : XrQuaternionf{-0.5f, 0.5f, -0.5f, 0.5f};
                                else if (back) face = XrQuaternionf{0.70710678f, 0.f, hnd == 0 ? -0.70710678f : 0.70710678f, 0.f};
                                ppose[hnd].orientation = qmul(qmul(gq, face), qeuler(rd));
                                // GOG GazeShows: an open list always shows; otherwise inside the cone (hysteresis) and facing you
                                bool show = true;
                                if (!list_open && g_cfg.hud_gaze_deg > 0) {
                                    float dx = ppose[hnd].position.x - hp.position.x, dy = ppose[hnd].position.y - hp.position.y,
                                          dz = ppose[hnd].position.z - hp.position.z, len = sqrtf(dx * dx + dy * dy + dz * dz);
                                    if (len >= 1e-3f) {
                                        XrVector3f f = qrot(hp.orientation, XrVector3f{0, 0, -1});
                                        float c = (f.x * dx + f.y * dy + f.z * dz) / len;
                                        float inC = cosf(g_cfg.hud_gaze_deg * 3.14159265f / 180.f), outC = cosf((g_cfg.hud_gaze_deg + 8) * 3.14159265f / 180.f);
                                        wshown[hnd] = wshown[hnd] ? (c > outC) : (c > inC);
                                        XrVector3f nrm = qrot(ppose[hnd].orientation, XrVector3f{0, 0, 1});
                                        float facing = -(nrm.x * dx + nrm.y * dy + nrm.z * dz) / len;
                                        show = wshown[hnd] && facing > 0.15f;
                                    }
                                }
                                pshow[hnd] = show;
                            }
                            const bool any = pshow[0] || pshow[1];
                            uint32_t wi = 0; XrSwapchainImageAcquireInfo wai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                            if (any && wcv.srv && wcv.drawn && gpu_stale && XR_SUCCEEDED(pxrAcquireSwapchainImage(wswc, &wai, &wi))) {
                                XrSwapchainImageWaitInfo wwi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wwi.timeout = XR_INFINITE_DURATION;
                                pxrWaitSwapchainImage(wswc, &wwi);
                                if (wi < wrtvs.size() && wrtvs[wi]) {
                                    float clr[4] = {0, 0, 0, 0}; g.ctx->ClearRenderTargetView(wrtvs[wi], clr);
                                    if (g.cb1) { float z[24] = {0}; g.ctx->UpdateSubresource(g.cb1, 0, nullptr, z, 0, 0); g.ctx->PSSetConstantBuffers(1, 1, &g.cb1); }
                                    g.ctx->OMSetRenderTargets(1, &wrtvs[wi], nullptr);
                                    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); g.ctx->IASetInputLayout(nullptr);
                                    g.ctx->VSSetShader(g.vs, nullptr, 0); g.ctx->PSSetShader(g.ps, nullptr, 0);
                                    g.ctx->PSSetSamplers(0, 1, &g.smp); g.ctx->PSSetConstantBuffers(0, 1, &g.cb);
                                    {   // the canvas, alpha from the texture (GOG: translucent black boxes, outlined pixel text)
                                        float o1[8] = { srgb ? 1.f : 0.f, 1, 0, 0, 0, 0, 1, 1 };
                                        g.ctx->UpdateSubresource(g.cb, 0, nullptr, o1, 0, 0);
                                        D3D11_VIEWPORT vp = {0, 0, (float)WW, (float)WH, 0, 1}; g.ctx->RSSetViewports(1, &vp);
                                        g.ctx->PSSetShaderResources(0, 1, &wcv.srv); g.ctx->Draw(3, 0);
                                    }
                                    for (int k = 0; k < 3; ++k) {   // radar / item box / weapon box, cut from the newest picture (GOG BlitCrop, alpha 235)
                                        const auto& b = wcv.blit[k];
                                        if (b.src < 0 || b.w <= 0 || b.h <= 0) continue;
                                        double r[4]; float uv[4]; hud_src_ps1(b.src, r);
                                        if (!ps1_to_uv(r, uv)) continue;
                                        float o2[8] = { srgb ? 1.f : 0.f, 2, 235.f / 255.f, 0, uv[0], uv[1], uv[2], uv[3] };
                                        g.ctx->UpdateSubresource(g.cb, 0, nullptr, o2, 0, 0);
                                        D3D11_VIEWPORT vp = {(float)b.x, (float)b.y, (float)b.w, (float)b.h, 0, 1}; g.ctx->RSSetViewports(1, &vp);
                                        g.ctx->PSSetShaderResources(0, 1, &g.srv); g.ctx->Draw(3, 0);
                                    }
                                    ID3D11ShaderResourceView* none = nullptr; g.ctx->PSSetShaderResources(0, 1, &none);
                                    if (g_save_wrist.exchange(false)) {   // v11 F10: what the wrists show, alpha over a grey checker
                                        ID3D11Resource* res = nullptr; wrtvs[wi]->GetResource(&res);
                                        ID3D11Texture2D* tx = nullptr; if (res) { res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tx); res->Release(); }
                                        ID3D11Texture2D* st = nullptr; D3D11_TEXTURE2D_DESC d = {};
                                        if (tx) { tx->GetDesc(&d); D3D11_TEXTURE2D_DESC sd = d; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
                                                  sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0; g.dev->CreateTexture2D(&sd, nullptr, &st); }
                                        D3D11_MAPPED_SUBRESOURCE m;
                                        if (st) { g.ctx->CopyResource(st, tx);
                                            if (SUCCEEDED(g.ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                                                static int wn = 0; wchar_t name[MAX_PATH];
                                                swprintf(name, MAX_PATH, L"%lsmgs1mc_vr_probe\\wrist_%03d.bmp", g_dir.c_str(), wn);
                                                FILE* fo = _wfopen(name, L"wb");
                                                if (fo) {
                                                    uint32_t rowb = d.Width * 3, pad = (4 - rowb % 4) % 4, img = (rowb + pad) * d.Height, v;
                                                    uint8_t h[54] = {'B', 'M'};
                                                    v = 54 + img; memcpy(h + 2, &v, 4); v = 54; memcpy(h + 10, &v, 4); v = 40; memcpy(h + 14, &v, 4);
                                                    v = d.Width; memcpy(h + 18, &v, 4); v = d.Height; memcpy(h + 22, &v, 4); h[26] = 1; h[28] = 24; v = img; memcpy(h + 34, &v, 4);
                                                    fwrite(h, 1, 54, fo);
                                                    bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
                                                    std::vector<uint8_t> line(rowb + pad, 0);
                                                    for (int y = (int)d.Height - 1; y >= 0; --y) {
                                                        const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                                                        for (UINT x = 0; x < d.Width; ++x) {
                                                            const uint8_t* q = row + x * 4; float a = q[3] / 255.f; int chk = ((x / 8 + y / 8) & 1) ? 110 : 70;
                                                            uint8_t r = bgra ? q[2] : q[0], gg = q[1], b = bgra ? q[0] : q[2];
                                                            line[x * 3 + 0] = (uint8_t)(b * a + chk * (1 - a)); line[x * 3 + 1] = (uint8_t)(gg * a + chk * (1 - a));
                                                            line[x * 3 + 2] = (uint8_t)(r * a + chk * (1 - a));
                                                        }
                                                        fwrite(line.data(), 1, line.size(), fo);
                                                    }
                                                    fclose(fo);
                                                    double rr[4]; float ruv[4] = {0, 0, 0, 0}; hud_src_ps1(2, rr); ps1_to_uv(rr, ruv);
                                                    logf("F10: saved mgs1mc_vr_probe\\wrist_%03d.bmp (%ux%u format %d) | radar cut PS1 (%.0f,%.0f %.0fx%.0f) = uv (%.3f,%.3f)-(%.3f,%.3f) of %ux%u, "
                                                         "drawn at (%d,%d %dx%d) | item box at (%d,%d %dx%d) | weapon box at (%d,%d %dx%d) | panel heights %d / %d",
                                                         wn, d.Width, d.Height, (int)d.Format, rr[0], rr[1], rr[2], rr[3], ruv[0], ruv[1], ruv[2], ruv[3], g_cap_w.load(), g_cap_h.load(),
                                                         wcv.blit[2].src >= 0 ? wcv.blit[2].x : -1, wcv.blit[2].y, wcv.blit[2].w, wcv.blit[2].h,
                                                         wcv.blit[0].src >= 0 ? wcv.blit[0].x : -1, wcv.blit[0].y, wcv.blit[0].w, wcv.blit[0].h,
                                                         wcv.blit[1].src >= 0 ? wcv.blit[1].x : -1, wcv.blit[1].y, wcv.blit[1].w, wcv.blit[1].h, wcv.panelH[0], wcv.panelH[1]);
                                                    ++wn;
                                                }
                                                g.ctx->Unmap(st, 0);
                                            }
                                            st->Release();
                                        } else logf("F10: could not read the wrist panel texture back");
                                        if (tx) tx->Release();
                                    }
                                }
                                XrSwapchainImageReleaseInfo wri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                                pxrReleaseSwapchainImage(wswc, &wri);
                                w_have = true; gpu_stale = false; ++g_ly_wrist_draw;
                            }
                            if (w_have && any) {
                                ++g_ly_wrist;
                                for (int hnd = 0; hnd < 2; ++hnd) {
                                    if (!pshow[hnd]) continue;
                                    XrCompositionLayerQuad& wq = wquad[hnd];
                                    wq.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                                    wq.space = local; wq.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                                    wq.subImage.swapchain = wswc; wq.subImage.imageArrayIndex = 0;
                                    wq.subImage.imageRect = XrRect2Di{{hnd * WPANEL, 0}, {WPANEL, wcv.panelH[hnd]}};
                                    float width = g_cfg.hud_width_mm / 1000.f * (g_wl[hnd].open && !g_cfg.v15.wheel ? 1.3f : 1.0f);
                                    wq.pose = ppose[hnd]; wq.size = {width, width * (float)wcv.panelH[hnd] / (float)WPANEL};
                                    layers[nlayers++] = (const XrCompositionLayerBaseHeader*)&wq;
                                    if (w_logged < 3) {
                                        ++w_logged;
                                        logf("HUD: %s wrist panel shown (%s), %.0f x %.0f mm%s | LIFE %d/%d weapon %d item %d",
                                             hnd ? "right" : "left", g_wl[hnd].open ? "list mount" :
                                             (g_cfg.hud_mount_side[hnd] == 1 ? "back of the wrist" : g_cfg.hud_mount_side[hnd] == 2 ? "inside of the wrist" : "top of the hand"),
                                             wq.size.width * 1000.f, wq.size.height * 1000.f, g_wl[hnd].open ? " (list)" : "",
                                             w_hd.vit, w_hd.vmax, w_hd.weapon, w_hd.item);
                                    }
                                }
                            }
                        }
                    }
                    // v15 HOLOGRAM WHEEL (GOG wrist_wheel.cpp): while a grip holds a selection list open, the list floats above that
                    // wrist as a radial wheel of the game's own icons, the highlighted entry's 3D model turning in the middle.
                    if (g_cfg.v15.wheel == 1 && hswc != XR_NULL_HANDLE && hsrv && (g_wl[0].open || g_wl[1].open)) {
                        int side = g_wl[1].open ? 1 : 0;
                        const WristList& L = g_wl[side];
                        static double last_draw = 0; static uint64_t last_sig = 0; static bool have_img = false, stereo_img = false;
                        static int wlog = 0; static int last_side = -1;
                        if (g_wheel_state.load() == 0 || (g_wheel_state.load() == 3 && side != last_side)) {   // models + icons from the PS1 RAM
                            uint8_t* ram = g_ram.load();
                            if (ram) {
                                std::vector<uint8_t> snap(0x200000);
                                char rep[200] = "PS1 RAM not readable";
                                bool ok = rd(ram, 0x80000000u, snap.data(), snap.size()) && GameModels::LoadFromRam(snap.data(), rep, sizeof rep);
                                g_wheel_state = ok ? 2 : 3;
                                logf("WHEEL: %s%s", rep, ok ? "" : " -- the wheel shows names only");
                            }
                        }
                        last_side = side;
                        HudData hh; AcquireSRWLockShared(&g_hud_lock); hh = g_hud; ReleaseSRWLockShared(&g_hud_lock);
                        double nowm = qpc_us() / 1000.0;
                        WristWheel::View v; v.weapons = side == 1; v.suppressed = hh.iown[23] >= 0;
                        v.count = L.count < WristWheel::kMaxEntries ? L.count : WristWheel::kMaxEntries;
                        for (int k = 0; k < v.count; ++k) {
                            int id = L.ids[k]; auto& e = v.e[k]; e.id = id;
                            if (id < 0) { strcpy(e.name, "NONE"); continue; }
                            strncpy(e.name, side ? kGogWeapon[id % 10] : kGogItem[id % 24], 15);
                            strncpy(e.desc, GameModels::Desc(side == 1, id), sizeof e.desc - 1);   // v17
                            e.equipped = id == (side ? hh.weapon : hh.item);
                            int cnt = side ? hh.wown[id % 10] : hh.iown[id % 24];
                            if (side) { if (cnt > 0) snprintf(e.detail, sizeof e.detail, "%d", cnt); }
                            else if (id == 17) snprintf(e.detail, sizeof e.detail, "LV%d", cnt);
                            else if (cnt > 1) snprintf(e.detail, sizeof e.detail, "x%d", cnt);
                        }
                        v.sel = L.sel; v.spinRad = (float)fmod(nowm / 1000.0 * 1.2, 2 * 3.14159265358979);
                        v.stereoDeg = (float)g_cfg.v15.wheel_stereo_deg;
                        v.hint = side == 0 && g_cfg.item_use_button ? "A: USE   RELEASE GRIP: EQUIP" : "RELEASE GRIP TO EQUIP";
                        uint64_t sig = ((uint64_t)side << 60) ^ ((uint64_t)L.sel << 40) ^ ((uint64_t)L.count << 32) ^ (uint64_t)(hh.weapon * 131 + hh.item * 7);
                        bool model_turns = g_wheel_state.load() == 2;
                        if (!have_img || sig != last_sig || (model_turns && nowm - last_draw >= 33)) {
                            stereo_img = WristWheel::Draw(v, hpx.data());
                            g.ctx->UpdateSubresource(htex, 0, nullptr, hpx.data(), WristWheel::kW * 4, 0);
                            last_draw = nowm; last_sig = sig; have_img = true;
                        }
                        uint32_t hi = 0; XrSwapchainImageAcquireInfo hai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                        if (g_hand_ok[side] && XR_SUCCEEDED(pxrAcquireSwapchainImage(hswc, &hai, &hi))) {
                            XrSwapchainImageWaitInfo hwi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; hwi.timeout = XR_INFINITE_DURATION;
                            pxrWaitSwapchainImage(hswc, &hwi);
                            if (hi < hrtvs.size() && hrtvs[hi]) {
                                float clr[4] = {0, 0, 0, 0}; g.ctx->ClearRenderTargetView(hrtvs[hi], clr);
                                if (g.cb1) { float z[24] = {0}; g.ctx->UpdateSubresource(g.cb1, 0, nullptr, z, 0, 0); g.ctx->PSSetConstantBuffers(1, 1, &g.cb1); }
                                float o1[8] = { srgb ? 1.f : 0.f, 1, 0, 0, 0, 0, 1, 1 };   // alpha from the canvas
                                g.ctx->UpdateSubresource(g.cb, 0, nullptr, o1, 0, 0);
                                D3D11_VIEWPORT vp = {0, 0, (float)WristWheel::kW, (float)(WristWheel::kH * 2), 0, 1};
                                g.ctx->OMSetRenderTargets(1, &hrtvs[hi], nullptr); g.ctx->RSSetViewports(1, &vp);
                                g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); g.ctx->IASetInputLayout(nullptr);
                                g.ctx->VSSetShader(g.vs, nullptr, 0); g.ctx->PSSetShader(g.ps, nullptr, 0);
                                g.ctx->PSSetShaderResources(0, 1, &hsrv); g.ctx->PSSetSamplers(0, 1, &g.smp); g.ctx->PSSetConstantBuffers(0, 1, &g.cb);
                                g.ctx->Draw(3, 0);
                                ID3D11ShaderResourceView* none = nullptr; g.ctx->PSSetShaderResources(0, 1, &none);
                            }
                            XrSwapchainImageReleaseInfo hri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                            pxrReleaseSwapchainImage(hswc, &hri);
                            // above the wrist, turned to face your head
                            XrPosef wpose = wheel_pose(g_hand[side], g_last_head, g_cfg.v15.wheel_height_cm / 100.0f);   // v16: GOG WheelPoseFor
                            XrVector3f c = wpose.position; XrQuaternionf q = wpose.orientation;
                            float qw = g_cfg.v15.wheel_size_cm / 100.0f * (float)WristWheel::kW / (2.0f * WristWheel::kDiscR);
                            for (int e = 0; e < (stereo_img ? 2 : 1); ++e) {
                                XrCompositionLayerQuad& hq = hquad[e];
                                hq.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                                hq.space = local;
                                hq.eyeVisibility = stereo_img ? (e == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT) : XR_EYE_VISIBILITY_BOTH;
                                hq.subImage.swapchain = hswc; hq.subImage.imageArrayIndex = 0;
                                hq.subImage.imageRect = XrRect2Di{{0, e * WristWheel::kH}, {WristWheel::kW, WristWheel::kH}};
                                hq.pose.orientation = q; hq.pose.position = c;
                                hq.size = {qw, qw * (float)WristWheel::kH / (float)WristWheel::kW};
                                if (nlayers < 16) layers[nlayers++] = (const XrCompositionLayerBaseHeader*)&hq;
                            }
                            if (wlog < 4) { ++wlog; logf("WHEEL: %s wheel above the %s wrist, %d entries, %.0f cm across, %s", side ? "weapon" : "item", side ? "right" : "left",
                                                         v.count, g_cfg.v15.wheel_size_cm * 1.0, model_turns ? "3D model in the middle" : "names only (models not loaded)"); }
                        }
                    }

                    // v16 3D CAROUSEL (GOG vr_aim.cpp BuildCarouselLayers, the GOG default selector=2): the entries float above the
                    // wrist as 3D models on holographic plinths in a curved row turned toward you, the front one spinning, its name
                    // and ammo / count on a plate under it. Flick the holding hand's stick, or swipe the other hand sideways through
                    // the row. Each entry is its own quad (real depth); the tiles are drawn into an atlas (GOG DrawCarouselTile),
                    // here on this thread with GOG's tile cache (only the spinning front tile is redrawn every 33 ms).
                    if (g_cfg.v15.wheel == 2 && cswc != XR_NULL_HANDLE && csrv && (g_wl[0].open || g_wl[1].open)) {
                        int side = (g_wl[0].open && g_wl[1].open) ? (g_wl[1].opened_at >= g_wl[0].opened_at ? 1 : 0) : (g_wl[1].open ? 1 : 0);
                        WristList& L = g_wl[side];
                        const int n = L.count < WristWheel::kMaxEntries ? L.count : WristWheel::kMaxEntries;
                        double nowm = qpc_us() / 1000.0;
                        static double last_t = 0, last_job = 0; double dtc = last_t > 0 ? (nowm - last_t) / 1000.0 : 0; if (dtc > 0.1) dtc = 0.1; last_t = nowm;
                        if (!g_car.open || g_car.side != side) {
                            g_car.open = true; g_car.side = side; g_car.epoch++; g_car.pos = (float)L.sel; g_car.target = L.sel;
                            g_car.last_pick = L.sel; g_car.armed = true; g_car.had_prev = false; g_car.vx = 0; g_car.drawn_epoch = -1;
                            for (int i = 0; i < WristWheel::kTileSlots; ++i) { g_car.slot_entry[i] = -1; g_car.slot_spun[i] = false; }
                        }
                        if (g_wheel_state.load() == 0 || (g_wheel_state.load() == 3 && g_car.epoch % 8 == 1)) {   // models + icons from the PS1 RAM
                            uint8_t* ram = g_ram.load();
                            if (ram) {
                                std::vector<uint8_t> snap(0x200000); char rep[200] = "PS1 RAM not readable";
                                bool ok = rd(ram, 0x80000000u, snap.data(), snap.size()) && GameModels::LoadFromRam(snap.data(), rep, sizeof rep);
                                g_wheel_state = ok ? 2 : 3; logf("CAROUSEL: %s%s", rep, ok ? "" : " -- icons / names only");
                                g_car.drawn_epoch = -1;
                            }
                        }
                        const int other = 1 - side;
                        // swipe: the other hand's flick sideways near the row (GOG v2: > 0.7 m/s counts, must slow < 0.35 first)
                        if (g_car.shown && g_hand_ok[other] && n > 0) {
                            const XrPosef& cp = g_car.pose; XrVector3f tip = g_hand[other].position;
                            float rel[3] = {tip.x - cp.position.x, tip.y - cp.position.y, tip.z - cp.position.z}, o[3];
                            XrQuaternionf inv = {-cp.orientation.x, -cp.orientation.y, -cp.orientation.z, cp.orientation.w};
                            quat_rot(inv, rel, o);
                            bool in_zone = fabsf(o[0]) < g_car.radius * 1.8f && fabsf(o[1]) < 0.16f && fabsf(o[2]) < 0.22f;
                            if (g_car.had_prev && dtc > 0) g_car.vx = 0.55f * g_car.vx + 0.45f * (float)((o[0] - g_car.prev_x) / dtc);
                            g_car.prev_x = o[0]; g_car.had_prev = true;
                            if (fabsf(g_car.vx) < 0.35f) g_car.armed = true;
                            if (g_car.armed && in_zone && fabsf(g_car.vx) > 0.7f) {
                                g_car.armed = false;
                                int steps = 1 + (int)((fabsf(g_car.vx) - 0.7f) / 0.9f); if (steps > 3) steps = 3;
                                int before = g_car.target, t = g_car.target + (g_car.vx < 0 ? steps : -steps);
                                g_car.target = t < 0 ? 0 : t > n - 1 ? n - 1 : t;
                                static int swl = 0; if (swl++ < 20) logf("CAROUSEL: swipe %s at %.2f m/s -> %d step(s), %d -> %d", g_car.vx < 0 ? "left" : "right", fabsf(g_car.vx), steps, before, g_car.target);
                                if (g_car.target == before) xr_buzz(ses, input, other, 0.5f, 40);   // end of the row
                            }
                        } else g_car.had_prev = false;
                        { float k = (float)(dtc * 12.0); if (k > 1) k = 1; g_car.pos += ((float)g_car.target - g_car.pos) * k; }
                        int pick = g_car.target < 0 ? 0 : g_car.target > n - 1 ? n - 1 : g_car.target;
                        if (n > 0 && pick != L.sel) { L.sel = pick; L.navigated = true; }
                        if (pick != g_car.last_pick) { g_car.last_pick = pick; xr_buzz(ses, input, other, 0.22f, 25); }
                        // 1. the atlas (~30 Hz): tiles of the entries in view, the front one spinning, its plate
                        if (n > 0 && (nowm - last_job >= 33 || g_car.drawn_epoch != g_car.epoch)) {
                            last_job = nowm;
                            HudData hh; AcquireSRWLockShared(&g_hud_lock); hh = g_hud; ReleaseSRWLockShared(&g_hud_lock);
                            bool weapons = side == 1, supp = hh.iown[23] >= 0;
                            WristWheel::Entry E[WristWheel::kMaxEntries];
                            for (int k = 0; k < n; ++k) {
                                int id = L.ids[k]; auto& e = E[k]; e.id = id;
                                if (id < 0) { strcpy(e.name, "NONE"); continue; }
                                strncpy(e.name, weapons ? kGogWeapon[id % 10] : kGogItem[id % 24], 15);
                                strncpy(e.desc, GameModels::Desc(weapons, id), sizeof e.desc - 1);   // v17: the game's own text
                                e.equipped = id == (weapons ? hh.weapon : hh.item);
                                int cnt = weapons ? hh.wown[id % 10] : hh.iown[id % 24];
                                if (weapons) { if (cnt > 0) snprintf(e.detail, sizeof e.detail, "%d", cnt); }
                                else if (id == 17) snprintf(e.detail, sizeof e.detail, "LV%d", cnt);
                                else if (cnt > 1) snprintf(e.detail, sizeof e.detail, "x%d", cnt);
                            }
                            const int right_slot = WristWheel::kTileSlots - 1;
                            int focus = pick, lo = (int)floorf(g_car.pos) - 3, hi = (int)ceilf(g_car.pos) + 3;
                            if (lo < 0) lo = 0; if (hi > n - 1) hi = n - 1;
                            if (g_car.drawn_epoch != g_car.epoch) { std::fill(g_car.atlas.begin(), g_car.atlas.end(), 0);
                                for (int i = 0; i < WristWheel::kTileSlots; ++i) { g_car.slot_entry[i] = -1; g_car.slot_spun[i] = false; } }
                            for (int i = 0; i < right_slot; ++i) if (g_car.slot_entry[i] < lo || g_car.slot_entry[i] > hi) g_car.slot_entry[i] = -1;
                            for (int k = 0; k < WristWheel::kMaxEntries; ++k) g_car.slot_of[k] = -1;
                            float spin = (float)fmod(nowm / 6000.0, 1.0) * 6.2831853f;   // one turn every 6 s (GOG ModelSpin)
                            float half = 0.5f * g_cfg.v15.wheel_stereo_deg * 3.14159265f / 180.0f;
                            for (int k = lo; k <= hi; ++k) {
                                int slot = -1; bool draw = false;
                                for (int i = 0; i < right_slot; ++i) if (g_car.slot_entry[i] == k) { slot = i; break; }
                                if (slot < 0) { for (int i = 0; i < right_slot; ++i) if (g_car.slot_entry[i] < 0) { slot = i; break; }
                                                if (slot < 0) continue; g_car.slot_entry[slot] = k; draw = true; }
                                bool f = k == focus;
                                if (f || g_car.slot_spun[slot]) draw = true;
                                if (draw) WristWheel::DrawCarouselTile(E[k], weapons, supp, f, spin, f ? -half : 0.0f, g_car.atlas.data(),
                                                                      (slot % 6) * WristWheel::kTile, (slot / 6) * WristWheel::kTile);
                                g_car.slot_spun[slot] = f; g_car.slot_of[k] = slot;
                            }
                            g_car.right_eye = false;
                            if (half > 0) { WristWheel::DrawCarouselTile(E[focus], weapons, supp, true, spin, half, g_car.atlas.data(),
                                                                         (right_slot % 6) * WristWheel::kTile, (right_slot / 6) * WristWheel::kTile); g_car.right_eye = true; }
                            WristWheel::DrawCarouselLabel(E[focus], side == 0 && g_cfg.item_use_button ? "FLICK OR SWIPE - A: USE - RELEASE GRIP: EQUIP"
                                                                                                   : "FLICK SIDEWAYS OR PUSH THE STICK - RELEASE GRIP TO EQUIP",
                                                          focus > 0, focus < n - 1, g_car.atlas.data(), 2 * WristWheel::kTile);
                            g_car.focus = focus; g_car.drawn_epoch = g_car.epoch;
                            g.ctx->UpdateSubresource(ctex, 0, nullptr, g_car.atlas.data(), WristWheel::kAtlasW * 4, 0);
                            uint32_t ci = 0; XrSwapchainImageAcquireInfo cai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                            if (XR_SUCCEEDED(pxrAcquireSwapchainImage(cswc, &cai, &ci))) {
                                XrSwapchainImageWaitInfo cwi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; cwi.timeout = XR_INFINITE_DURATION;
                                pxrWaitSwapchainImage(cswc, &cwi);
                                if (ci < crtvs.size() && crtvs[ci]) {
                                    float clr[4] = {0, 0, 0, 0}; g.ctx->ClearRenderTargetView(crtvs[ci], clr);
                                    if (g.cb1) { float z[24] = {0}; g.ctx->UpdateSubresource(g.cb1, 0, nullptr, z, 0, 0); g.ctx->PSSetConstantBuffers(1, 1, &g.cb1); }
                                    float o1[8] = { srgb ? 1.f : 0.f, 1, 0, 0, 0, 0, 1, 1 };
                                    g.ctx->UpdateSubresource(g.cb, 0, nullptr, o1, 0, 0);
                                    D3D11_VIEWPORT vp = {0, 0, (float)WristWheel::kAtlasW, (float)WristWheel::kAtlasH, 0, 1};
                                    g.ctx->OMSetRenderTargets(1, &crtvs[ci], nullptr); g.ctx->RSSetViewports(1, &vp);
                                    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); g.ctx->IASetInputLayout(nullptr);
                                    g.ctx->VSSetShader(g.vs, nullptr, 0); g.ctx->PSSetShader(g.ps, nullptr, 0);
                                    g.ctx->PSSetShaderResources(0, 1, &csrv); g.ctx->PSSetSamplers(0, 1, &g.smp); g.ctx->PSSetConstantBuffers(0, 1, &g.cb);
                                    g.ctx->Draw(3, 0);
                                    ID3D11ShaderResourceView* none = nullptr; g.ctx->PSSetShaderResources(0, 1, &none);
                                }
                                XrSwapchainImageReleaseInfo cri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; pxrReleaseSwapchainImage(cswc, &cri);
                                g_car.have_img = true;
                            }
                        }
                        // 2. the quads: an arc above the wrist, facing you (GOG WheelPoseFor; cover-flow turn)
                        g_car.shown = false;
                        if (g_car.have_img && g_car.drawn_epoch == g_car.epoch && g_hand_ok[side] && n > 0) {
                            XrPosef c = wheel_pose(g_hand[side], g_last_head, g_cfg.v15.wheel_height_cm / 100.0f);
                            g_car.pose = c; g_car.shown = true;
                            float X[3], Y[3], Z[3]; { const float ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0}, ez[3] = {0, 0, 1};
                                                      quat_rot(c.orientation, ex, X); quat_rot(c.orientation, ey, Y); quat_rot(c.orientation, ez, Z); }
                            const float tile_m = g_cfg.v15.carousel_size_cm / 100.0f; g_car.radius = tile_m * 1.4f;
                            int qi = 0;
                            auto push = [&](const XrPosef& pose, float w, float h, int ox, int oy, int ew, int eh, XrEyeVisibility eye) {
                                if (qi >= 10 || nlayers >= 16) return;
                                XrCompositionLayerQuad& q = cquad[qi++]; q = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
                                q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                                q.space = local; q.eyeVisibility = eye; q.subImage.swapchain = cswc; q.subImage.imageArrayIndex = 0;
                                q.subImage.imageRect = XrRect2Di{{ox, oy}, {ew, eh}}; q.pose = pose; q.size = {w, h};
                                layers[nlayers++] = (const XrCompositionLayerBaseHeader*)&q;
                            };
                            {   // name plate under the front model
                                XrPosef lp = c; float lw = tile_m * 2.4f, lh = lw * (float)WristWheel::kLabelH / (float)WristWheel::kAtlasW, drop = tile_m * 0.55f + lh * 0.5f;
                                lp.position.x -= Y[0] * drop; lp.position.y -= Y[1] * drop; lp.position.z -= Y[2] * drop;
                                push(lp, lw, lh, 0, 2 * WristWheel::kTile, WristWheel::kAtlasW, WristWheel::kLabelH, XR_EYE_VISIBILITY_BOTH);
                            }
                            struct It { int k; float d; } its[WristWheel::kMaxEntries]; int ni = 0;
                            for (int k = 0; k < n; ++k) { float d = (float)k - g_car.pos; if (fabsf(d) < 2.6f && g_car.slot_of[k] >= 0) its[ni++] = {k, d}; }
                            std::sort(its, its + ni, [](const It& a, const It& b) { return fabsf(a.d) > fabsf(b.d); });   // far first
                            for (int i = 0; i < ni; ++i) {
                                int k = its[i].k; float d = its[i].d, ad = fabsf(d), phi = d * 0.4538f;   // 26 deg between neighbours
                                float sc = 1.0f - 0.2f * (ad < 2.0f ? ad : 2.0f); if (ad > 2.0f) sc *= (2.6f - ad) / 0.6f;
                                if (sc <= 0.02f) continue;
                                float sx = g_car.radius * sinf(phi), sz = g_car.radius * (cosf(phi) - 1.0f);
                                XrPosef p; p.position = {c.position.x + X[0] * sx + Z[0] * sz, c.position.y + X[1] * sx + Z[1] * sz, c.position.z + X[2] * sx + Z[2] * sz};
                                float ha = -phi * 0.7f * 0.5f; XrQuaternionf ry = {0, sinf(ha), 0, cosf(ha)};
                                p.orientation = quat_mul(c.orientation, ry);
                                int slot = g_car.slot_of[k], ox = (slot % 6) * WristWheel::kTile, oy = (slot / 6) * WristWheel::kTile; float size = tile_m * sc;
                                if (k == g_car.focus && g_car.right_eye) {
                                    const int rs = WristWheel::kTileSlots - 1;
                                    push(p, size, size, ox, oy, WristWheel::kTile, WristWheel::kTile, XR_EYE_VISIBILITY_LEFT);
                                    push(p, size, size, (rs % 6) * WristWheel::kTile, (rs / 6) * WristWheel::kTile, WristWheel::kTile, WristWheel::kTile, XR_EYE_VISIBILITY_RIGHT);
                                } else push(p, size, size, ox, oy, WristWheel::kTile, WristWheel::kTile, XR_EYE_VISIBILITY_BOTH);
                            }
                            static int clog = 0; if (clog < 4) { ++clog; logf("CAROUSEL: %s models above the %s wrist (%d entries, %d quads, front %s)", side ? "weapon" : "item",
                                                                              side ? "right" : "left", n, qi, g_wheel_state.load() == 2 ? "3D model" : "icon / name"); }
                        }
                    } else if (!(g_wl[0].open || g_wl[1].open)) { g_car.open = false; g_car.shown = false; }

                    // v17 CUTSCENE SUBTITLES (GOG cutscene_pov_subtitle_panel): through Snake's eyes the captions sit on the demo's
                    // bottom bar, at the very bottom edge of the lens. That strip of the game picture is lifted onto its own panel
                    // just below your gaze (head-locked, yaw only). The shader keys the text out of the black bar (alpha from
                    // brightness, a dark halo round the letters), so the panel is invisible while the bar has no caption.
                    if (g_cfg.v15.sub_on && g_cut_pov.load() && g_demo.load() && sswc != XR_NULL_HANDLE && g.srv && g.v10_shader) {
                        float suv[4];
                        if (sub_strip_uv(suv)) {
                            uint32_t si = 0; XrSwapchainImageAcquireInfo sai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                            if (XR_SUCCEEDED(pxrAcquireSwapchainImage(sswc, &sai, &si))) {
                                XrSwapchainImageWaitInfo swi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; swi.timeout = XR_INFINITE_DURATION;
                                pxrWaitSwapchainImage(sswc, &swi);
                                if (si < srtvs.size() && srtvs[si]) {
                                    float clr[4] = {0, 0, 0, 0}; g.ctx->ClearRenderTargetView(srtvs[si], clr);
                                    if (g.cb1) { float z[24] = {0}; g.ctx->UpdateSubresource(g.cb1, 0, nullptr, z, 0, 0); g.ctx->PSSetConstantBuffers(1, 1, &g.cb1); }
                                    uint32_t cw = g_cap_w.load(), chh = g_cap_h.load();
                                    float o3[8] = { srgb ? 1.f : 0.f, 3, cw ? 1.f / cw : 0.f, chh ? 1.f / chh : 0.f, suv[0], suv[1], suv[2], suv[3] };
                                    g.ctx->UpdateSubresource(g.cb, 0, nullptr, o3, 0, 0);
                                    D3D11_VIEWPORT vp = {0, 0, (float)SUBW, (float)SUBH, 0, 1};
                                    g.ctx->OMSetRenderTargets(1, &srtvs[si], nullptr); g.ctx->RSSetViewports(1, &vp);
                                    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); g.ctx->IASetInputLayout(nullptr);
                                    g.ctx->VSSetShader(g.vs, nullptr, 0); g.ctx->PSSetShader(g.ps, nullptr, 0);
                                    g.ctx->PSSetShaderResources(0, 1, &g.srv); g.ctx->PSSetSamplers(0, 1, &g.smp); g.ctx->PSSetConstantBuffers(0, 1, &g.cb);
                                    g.ctx->Draw(3, 0);
                                    ID3D11ShaderResourceView* none = nullptr; g.ctx->PSSetShaderResources(0, 1, &none);
                                }
                                XrSwapchainImageReleaseInfo sri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; pxrReleaseSwapchainImage(sswc, &sri);
                                const XrQuaternionf& ho = g_last_head.orientation;
                                double fx = -2 * (ho.x * ho.z + ho.w * ho.y), fz = -(1 - 2 * (ho.x * ho.x + ho.y * ho.y));
                                double yaw = atan2(-fx, -fz), down = g_cfg.v15.sub_down_deg * 3.14159265358979 / 180.0, dist = g_cfg.v15.sub_dist_cm / 100.0;
                                XrPosef sp;
                                sp.position = { g_last_head.position.x - (float)(sin(yaw) * cos(down) * dist), g_last_head.position.y - (float)(sin(down) * dist),
                                                g_last_head.position.z - (float)(cos(yaw) * cos(down) * dist) };
                                XrQuaternionf qy = {0, (float)sin(yaw / 2), 0, (float)cos(yaw / 2)}, qx = {(float)sin(-down / 2), 0, 0, (float)cos(-down / 2)};
                                sp.orientation = quat_mul(qy, qx);   // tilted up to face you
                                squad = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
                                squad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                                squad.space = local; squad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                                squad.subImage.swapchain = sswc; squad.subImage.imageArrayIndex = 0; squad.subImage.imageRect = {{0, 0}, {SUBW, SUBH}};
                                float sw = g_cfg.v15.sub_w_cm / 100.0f;
                                squad.pose = sp; squad.size = {sw, sw * (float)SUBH / (float)SUBW};
                                if (nlayers < 16) layers[nlayers++] = (const XrCompositionLayerBaseHeader*)&squad;
                                static int slog = 0; if (slog < 3) { ++slog; logf("SUBTITLES: cutscene caption strip (uv %.3f,%.3f-%.3f,%.3f) lifted onto a panel %.0f deg below your gaze",
                                                                                  suv[0], suv[1], suv[2], suv[3], (double)g_cfg.v15.sub_down_deg); }
                            }
                        }
                    }

                    // settings panel: its own quad layer, only while open
                    if (g_panel.open && pswc != XR_NULL_HANDLE) {
                        uint32_t pi = 0; XrSwapchainImageAcquireInfo pai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                        if (XR_SUCCEEDED(pxrAcquireSwapchainImage(pswc, &pai, &pi))) {
                            XrSwapchainImageWaitInfo pwi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; pwi.timeout = XR_INFINITE_DURATION;
                            pxrWaitSwapchainImage(pswc, &pwi);
                            if (g_panel.dirty || !pgfx.tex) { panel_render(pgfx, g.dev, g.ctx); g_panel.dirty = false; }
                            if (pi < prtvs.size() && prtvs[pi] && pgfx.srv) {
                                float opts[8] = { srgb ? 1.f : 0.f, 0, 0, 0, 0, 0, 1, 1 };
                                g.ctx->UpdateSubresource(g.cb, 0, nullptr, opts, 0, 0);
                                D3D11_VIEWPORT vp = {0, 0, (float)PW, (float)PH, 0, 1};
                                g.ctx->OMSetRenderTargets(1, &prtvs[pi], nullptr); g.ctx->RSSetViewports(1, &vp);
                                g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); g.ctx->IASetInputLayout(nullptr);
                                g.ctx->VSSetShader(g.vs, nullptr, 0); g.ctx->PSSetShader(g.ps, nullptr, 0);
                                g.ctx->PSSetShaderResources(0, 1, &pgfx.srv); g.ctx->PSSetSamplers(0, 1, &g.smp); g.ctx->PSSetConstantBuffers(0, 1, &g.cb);
                                g.ctx->Draw(3, 0);
                                ID3D11ShaderResourceView* none = nullptr; g.ctx->PSSetShaderResources(0, 1, &none);
                            }
                            XrSwapchainImageReleaseInfo pri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                            pxrReleaseSwapchainImage(pswc, &pri);
                            pquad.space = local; pquad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                            pquad.subImage.swapchain = pswc; pquad.subImage.imageRect = {{0, 0}, {PW, PH}};
                            pquad.pose = g_panel.pose; pquad.size = {PW / 1000.0f, PH / 1000.0f};
                            layers[nlayers++] = (const XrCompositionLayerBaseHeader*)&pquad;
                        }
                    }
                    static int last_mode = -1; int mode = screen_mode;
                    if (mode != last_mode) { logf("SCREEN: %s (projection layer, %.0f x %.0f deg)", mode ? "world-locked to the game camera" : "fixed screen in the room",
                                                    2 * atan(scr_tan_h) * 57.29578, 2 * atan(scr_tan_v) * 57.29578); last_mode = mode; }
                }
            }
            XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
            fei.displayTime = fs.predictedDisplayTime; fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            fei.layerCount = nlayers; fei.layers = layers;
            if (nlayers > g_ly_max) g_ly_max = nlayers;
            if (g_panel.open) ++g_ly_panel;
            pxrEndFrame(ses, &fei);
            double t_end = qpc_us(); st_end.add((t_end - t_drawn) / 1000.0); st_loop.add((t_end - t_loop) / 1000.0);
            ++frames; ++frames_win;
            ULONGLONG now = GetTickCount64();
            if (now - last_log >= 3000) {
                uint32_t pf = g_cap_frames.load();
                logf("XR: %u headset frames, %u game pictures in the last %.1f s | head yaw %.1f pitch %.1f roll %.1f (relative) | "
                     "picture tag seq %u (newest pose %u) screen %u tracked %d | state %d",
                     frames, pf - pic_frames0, last_log ? (now - last_log) / 1000.0 : 0.0,
                     g_head_yaw_cdeg / 100.0, g_head_pitch_cdeg / 100.0, g_head_roll_cdeg / 100.0,
                     g.tag_seq, g_pose_seq, g.tag_screen, g.tag_tracked, (int)state);
                logf("PICTURE: %u new pictures taken, %u frames reused (no newer picture; %u of them waiting on our own copy), "
                     "%u presents skipped (game's previous copy still on the GPU)%s",
                     g.new_pics, g.busy, g.gpu_wait, g_cap_skipped.exchange(0), g_test_blank.load() ? " | F11 TEST COLOUR ON" : "");
                g.new_pics = g.busy = g.gpu_wait = 0;
                { uint32_t sn = g_snap_count.exchange(0), bt = g_body_turns.exchange(0);
                  if (sn || bt) logf("TURN: %u snap turns, %u times Snake's body turned to face where you look", sn, bt); }
                {
                    static const char* gs[4] = {"off (not walking in first person, or switched off)", "in your hand", "waiting for the right controller", "no SOCOM / FAMAS / grenade equipped"};
                    logf("MOTION: gun %s | reach %.2f m, aim yaw %.1f deg from the view, pitch %.1f deg | matrix writes %u, hung from it %u, handed back %u, unexpected root %u",
                         gs[g_gun_state.load() & 3], g_gun_reach_mm.load() / 1000.0, g_gun_yaw_cdeg.load() / 100.0, g_gun_pitch_cdeg.load() / 100.0,
                         g_gun_writes.exchange(0), g_gun_redirects.exchange(0), g_gun_restores.exchange(0), g_gun_skips.exchange(0));
                    if (g_v13_hands.load() || g_v13_bars.load()) {
                        uint8_t* r2 = g_ram.load();
                        uint32_t al = r2 ? rd32(r2, HMB + 0x0C) : 0, ap = r2 ? rd32(r2, HMB + 0x10) : 0;
                        logf("HANDS: %s | draw routine calls %u, Snake's body drawn as hands %u | wall tests %u, touches %u, knocks %u | scope %s, head aim %s",
                             !g_cfg.hands ? "off in the ini" : g_hands_fail.load() ? "FAILED (see above)" : g_hands_drawing.load() ? "ON" :
                             !g_hands_live.load() ? "waiting for the draw routine" : "standing by (first person only)",
                             al, ap, g_hands_tests.exchange(0), g_hands_hits.exchange(0), g_hands_knocks.exchange(0),
                             g_scoped.load() ? "UP" : "down", g_aim_head.load() ? "on" : "off");
                    }
                    logf("LEAN: %s | eye moved by your head x %d, y %d, z %d mm (game axes) | largest head move since the last line %d mm (limit %d cm)",
                         !g_cfg.lean ? "off in the ini ([walk] lean=0)" : g_mode_cur.load() == 2 ? "on" : "waiting for first person",
                         g_lean_mm[0].load(), g_lean_mm[1].load(), g_lean_mm[2].load(), g_lean_max_mm.exchange(0), g_cfg.lean_max_cm);
                    { uint32_t tr = g_throw_redirects.exchange(0), tb = g_throw_restores.exchange(0), ts = g_throw_spawned.exchange(0);
                      if (tr || tb || ts || g_throw_work.load())
                          logf("THROW: %s | throw point moved to your hand %u times, given back %u times, things thrown %u",
                               g_throw_work.load() ? "grenade / stun / chaff throws from your hand" : "nothing in your hand to throw", tr, tb, ts); }
                }
                { uint32_t mp = g_melee_punches.exchange(0), mg = g_melee_grabs.exchange(0), ms = g_melee_squeezes.exchange(0), gs2 = g_gun_shots_felt.exchange(0);
                  { uint32_t rh = g_body_rehides.exchange(0);
                    HudData hl; AcquireSRWLockShared(&g_hud_lock); hl = g_hud; ReleaseSRWLockShared(&g_hud_lock);
                    logf("BODY: %s (re-grouped %u times) | box %s | HUD: LIFE %d/%d, weapon %d (%d), item %d (%d), list %s %d/%d",
                         g_body_hidden.load() ? "hidden from the camera" : "shown", rh, g_box_objs.load() ? "equipped" : "-",
                         hl.vit, hl.vmax, hl.weapon, hl.wammo, hl.item, hl.icount, hl.menu == 1 ? "weapons" : hl.menu == 2 ? "items" : "none", hl.lsel, hl.ln); }
                  if (mp || mg || ms || gs2) logf("MELEE: %u punches, %u grabs, %u squeezes | gun shots felt %u | weapon id %d", mp, mg, ms, gs2, g_weapon_id.load()); }
                logf("STEREO: %s | pictures mono %u, left %u, right %u | eye too old %u | holds: camera moving %u, head turning %u, "
                     "stick turning %u, low frame rate %u, hands moving %u | game %.1f fps, offset %d units",
                     !g_cfg.stereo ? "off in the ini" : !g_stereo_on.load() ? "off (F5)" : g_stereo_half.load() ? "ON" : "holding mono",
                     g.code_count[0], g.code_count[1], g.code_count[2], g.eye_timeouts,
                     g_hold_move.exchange(0), g_hold_head.exchange(0), g_hold_turn.exchange(0), g_hold_fps.exchange(0), g_hold_hand.exchange(0),
                     g_game_fps_x10.load() / 10.0, g_stereo_half.load());
                if (g_ts_installed.load()) {
                    uint8_t* ram5 = g_ram.load(); uint32_t tm[6] = {0}; if (ram5) rd(ram5, TSMB, tm, sizeof tm);
                    static uint32_t prev[3] = {0, 0, 0};
                    logf("TRUE STEREO: %s | right-eye sorts %u, second draws %u, display calls %u, side-by-side pictures %u (last 3 s)",
                         g_ts_on.load() ? "ON" : g_cfg.v15.true_stereo ? "wanted, waiting (not in a VR view / paused / sight / Snake's-eyes cutscene)" : "off ([stereo] true_stereo=0)",
                         tm[3] - prev[0], tm[4] - prev[1], tm[5] - prev[2], g_sbs_pics.exchange(0));
                    prev[0] = tm[3]; prev[1] = tm[4]; prev[2] = tm[5];
                }
                logf("MARKER: pictures read as left %u, right %u, mono %u, no marker %u, unreadable %u | the old timing guess "
                     "matched %u, missed %u", g_mark_stats[1].exchange(0), g_mark_stats[2].exchange(0), g_mark_stats[3].exchange(0),
                     g_mark_stats[0].exchange(0), g_mark_stats[4].exchange(0), g.ring_agree, g.ring_disagree);
                g.ring_agree = g.ring_disagree = 0;
                logf("POSE: pictures with their own pose number %u (old guess right %u, wrong %u by %.1f poses on average), "
                     "no pose number %u, number not found %u, unreadable %u",
                     g.pose_marked, g.pose_same, g.pose_diff, g.pose_diff ? (double)g.pose_diff_abs / g.pose_diff : 0.0,
                     g.pose_unmarked, g.pose_unfound, g_mseq_bad.exchange(0));
                g.pose_marked = g.pose_unmarked = g.pose_unfound = g.pose_same = g.pose_diff = g.pose_diff_abs = 0;
                g.code_count[0] = g.code_count[1] = g.code_count[2] = 0; g.eye_timeouts = 0;
                uint32_t pn = g_pres_n.exchange(0), ps = g_pres_us_sum.exchange(0), pm = g_pres_us_max.exchange(0);
                uint32_t wn = g_wr_n.exchange(0), wm = g_wr_us_max.exchange(0);
                logf("LAYERS: wrist panels up in %u of %u headset frames (texture redrawn %u times), settings panel %u, most layers in one frame %u",
                     g_ly_wrist, frames_win, g_ly_wrist_draw, g_ly_panel, g_ly_max);
                g_ly_wrist = g_ly_wrist_draw = g_ly_panel = g_ly_max = 0; frames_win = 0;
                logf("TIMING ms avg/max: wait %.1f/%.1f begin %.1f/%.1f locate %.1f/%.1f image %.1f/%.1f draw %.1f/%.1f end %.1f/%.1f "
                     "loop %.1f/%.1f | period %.1f ms, shouldRender=0 x%d | present hook %.2f/%.2f (%u) | RAM writes %u, max %.2f",
                     st_wait.avg(), st_wait.mx, st_begin.avg(), st_begin.mx, st_locate.avg(), st_locate.mx, st_img.avg(), st_img.mx,
                     st_draw.avg(), st_draw.mx, st_end.avg(), st_end.mx, st_loop.avg(), st_loop.mx, period_ms, not_render,
                     pn ? ps / 1000.0 / pn : 0.0, pm / 1000.0, pn, wn, wm / 1000.0);
                if (g_perf_ok) {
                    int g3 = g_perf_gpu3d, gs = g_perf_gpu3d_self, en = g_perf_enc, cp = g_perf_cpu, nk = g_perf_net_kbps, wf = g_perf_wifi;
                    const char* verdict =
                        st_end.mx < 30 && st_wait.avg() < period_ms * 1.6 ? "headset loop keeping up" :
                        g3 >= 90 ? "GPU is saturated (PC-side: lower VD quality or the game's resolution)" :
                        en >= 90 ? "video encoder is saturated (lower VD bitrate/quality)" :
                        cp >= 90 ? "CPU is saturated (PC-side)" :
                        "PC has headroom while the headset waits -- points at the stream/runtime (wireless link, VD settings)";
                    logf("PERF: GPU 3D %d%% (this game %d%%), video encode %d%%, CPU %d%%, network send %.1f Mbit/s, PC Wi-Fi signal %s | %s",
                         g3, gs, en, cp, nk >= 0 ? nk / 1000.0 : -1.0, wf >= 0 ? (std::to_string(wf) + "%").c_str() : "n/a (wired or none)", verdict);
                }
                st_wait = st_begin = st_locate = st_img = st_draw = st_end = st_loop = St(); not_render = 0;
                frames = 0; pic_frames0 = pf; last_log = now;
            }
        }
        logf("XR: session ended; cleaning up (will retry)");
        for (auto& hs : input.hand) if (hs) { pxrDestroySpace(hs); hs = XR_NULL_HANDLE; }
        if (input.set) pxrDestroyActionSet(input.set);
        AcquireSRWLockExclusive(&g_vrpad_lock); g_vrpad = VrPad(); ReleaseSRWLockExclusive(&g_vrpad_lock);
        g_head_valid = false;
        for (auto r : rtvs) if (r) r->Release();
        for (auto r : prtvs) if (r) r->Release();
        for (auto r : wrtvs) if (r) r->Release();
        if (wswc) pxrDestroySwapchain(wswc);
        if (pswc) pxrDestroySwapchain(pswc);
        if (hswc) pxrDestroySwapchain(hswc);
        for (auto* r : hrtvs) if (r) r->Release();
        if (hsrv) hsrv->Release(); if (htex) htex->Release();
        if (cswc) pxrDestroySwapchain(cswc);
        if (sswc) pxrDestroySwapchain(sswc);
        for (auto* r : srtvs) if (r) r->Release();
        for (auto* r : crtvs) if (r) r->Release();
        if (csrv) csrv->Release(); if (ctex) ctex->Release();
        g_car.open = false; g_car.have_img = false;
        panel_free(pgfx); g_panel.open = false;
        if (swc) pxrDestroySwapchain(swc);
        if (view) pxrDestroySpace(view); if (local) pxrDestroySpace(local);
        pxrDestroySession(ses);
        g_xr_slot = -1;
        if (g.srv) g.srv->Release(); if (g.priv) g.priv->Release(); if (g.xq) g.xq->Release();
        for (int e = 0; e < 2; ++e) { if (g.eyeSrv[e]) g.eyeSrv[e]->Release(); if (g.eyeTex[e]) g.eyeTex[e]->Release(); }
        for (auto t : g.slot) if (t) t->Release();
        if (g.staging) g.staging->Release();
        if (g.vs) g.vs->Release(); if (g.ps) g.ps->Release(); if (g.smp) g.smp->Release(); if (g.cb) g.cb->Release(); if (g.cb1) g.cb1->Release();
        g.ctx->Release(); g.dev->Release();
        pxrDestroyInstance(inst);
    }
    return 0;
}

// ================================================================= in-game hook
struct HookState {
    bool installed = false;
    bool runtime_on = true;      // F8 toggle
    int  enable_word = -1;
    ULONGLONG installed_at = 0;
    uint32_t gvt_at_install = 0;
    bool reported_running = false, reported_not_running = false;
    int repairs = 0;
};

static bool cave_intact(uint8_t* ram, const GameTable& T)
{
    uint32_t buf[CAVE_WORDS], pbuf[PADCAVE_WORDS], sbuf[SCAVE_WORDS], cbuf[CALLCAVE2_WORDS];
    if (!rd(ram, CAVE, buf, sizeof buf) || !rd(ram, PADCAVE, pbuf, sizeof pbuf) || !rd(ram, SCAVE, sbuf, sizeof sbuf) ||
        !rd(ram, CCODE, cbuf, sizeof cbuf)) return false;
    return memcmp(buf, T.cave, sizeof buf) == 0 && memcmp(pbuf, T.padcave, sizeof pbuf) == 0 && memcmp(cbuf, CALLCAVE2, sizeof cbuf) == 0 &&
           memcmp(sbuf, T.scave, sizeof sbuf) == 0 && rd32(ram, MBOX) == MBOX_MAGIC;
}

static bool cave_area_free(uint8_t* ram)
{
    uint8_t buf[CODE_END - MBOX];                               // 0x8000A400..0x8000AA68: mailboxes and routines
    if (!rd(ram, MBOX, buf, sizeof buf)) return false;
    for (uint8_t c : buf) if (c) return false;
    return true;
}

static bool install(uint8_t* ram, const GameTable& T, HookState& h)
{
    uint32_t site = rd32(ram, T.hook_site);
    if (site != T.hook_orig && site != HOOK_WORD) {
        logf("HOOK refused: word at %08X is %08X, expected %08X", T.hook_site, site, T.hook_orig);
        return false;
    }
    bool ours = rd32(ram, MBOX) == MBOX_MAGIC;
    if (!ours && !cave_area_free(ram)) {
        logf("HOOK refused: cave area %08X-%08X is not empty (the game uses it here?)", MBOX, CODE_END);
        return false;
    }
    // mailbox first (disabled, identity matrices), then code, verify, then the hook word last
    uint8_t mb[MB_SIZE] = {0};
    uint32_t magic = MBOX_MAGIC; memcpy(mb, &magic, 4);
    int16_t ident[9] = {4096, 0, 0, 0, 4096, 0, 0, 0, 4096};
    memcpy(mb + MB_M0, ident, sizeof ident); memcpy(mb + MB_M1, ident, sizeof ident);
    uint16_t fov = (uint16_t)g_cfg.fov_screen; memcpy(mb + MB_FOV, &fov, 2);
    if (!ours) wr(ram, MBOX, mb, sizeof mb); else { wr32(ram, MBOX + MB_ENABLE, 0); wr(ram, MBOX + MB_FOV, &fov, 2); }
    wr(ram, CAVE, T.cave, CAVE_WORDS * 4);
    { uint8_t z[0x290] = {0}; wr(ram, CALL2_ALIVE, z, 4 + NSLOT2 * SLOT2SZ); wr(ram, CMB_BOXFLAG, z, 0x80); }   // v13 call slots: nothing asked
    wr(ram, CCODE, CALLCAVE2, CALLCAVE2_WORDS * 4);               // v13 call routine BEFORE the pad routine that jals it
    g_call_epoch++;
    { uint8_t z[0x20] = {0}; wr(ram, PMBOX, z, sizeof z); }      // pad mailbox: disabled
    wr(ram, PADCAVE, T.padcave, PADCAVE_WORDS * 4);
    if (!ours) {   // stereo mailbox: mono, marker colours, the constant parts of the marker TILEs (never while linked)
        uint8_t sm[0x8C] = {0};
        uint32_t col[3] = {0x60FF0000, 0x600000FF, 0x6000FF00};   // TILE code 0x60 + BGR: blue mono, red left, green right
        memcpy(sm + 0x14, col, sizeof col);
        for (int b = 0x20; b <= 0x50; b += 0x30) {   // 3x2 TILEs at (0,0) eye, (3,0) magenta, (6,0) pose seq
            uint32_t eye[4] = {0, col[0], 0, (2u << 16) | 3}, mag[4] = {0, 0x60FF00FF, 3, (2u << 16) | 3}, sq[4] = {0, 0x60000000, 6, (2u << 16) | 3};
            memcpy(sm + b, eye, sizeof eye); memcpy(sm + b + 0x10, mag, sizeof mag); memcpy(sm + b + 0x20, sq, sizeof sq);
        }
        uint32_t stamp[2] = {0xFFFFFFFF, 0xFFFFFFFF}; memcpy(sm + 0x80, stamp, sizeof stamp);
        uint8_t lv[4] = {0, 85, 170, 255}; memcpy(sm + 0x88, lv, 4);
        wr(ram, SMB, sm, sizeof sm);
    }
    { int16_t z[3] = {0, 0, 0}; wr(ram, DIO, z, sizeof z); }   // v15: routine v12 adds this in mode 1 -- start at zero
    wr(ram, SCAVE, T.scave, SCAVE_WORDS * 4);
    if (!cave_intact(ram, T)) { logf("HOOK refused: routine did not read back correctly"); return false; }
    h.enable_word = 0;
    if (!wr32(ram, T.hook_site, HOOK_WORD)) { logf("HOOK write failed"); return false; }
    uint32_t wp = rd32(ram, T.pad_site);
    if (wp == T.pad_orig) { wr32(ram, T.pad_site, PAD_HOOK_WORD); logf("HOOK pad (VR controllers) installed at %08X", T.pad_site); }
    if (&T == &T_US10) {   // v20 true stereo: code first, mailbox off, then the three jal words (each only over its original)
        uint8_t z[0x18] = {0}; wr(ram, TSMB, z, sizeof z);
        wr(ram, TSCODE1, TSCAVE1_US10, sizeof TSCAVE1_US10); wr(ram, TSCODE2, TSCAVE2_US10, sizeof TSCAVE2_US10);
        int ok = 0;
        for (auto& hk : TS_HOOKS_US10) { uint32_t w0 = rd32(ram, hk[0]); if (w0 == hk[1]) { wr32(ram, hk[0], hk[2]); ++ok; } else if (w0 == hk[2]) ++ok; }
        g_ts_installed = ok == 3;
        logf("TRUE STEREO: %s (%d of 3 hooks: DG_EndFrame sort, DG_StartFrame draw, display)", ok == 3 ? "routines installed, off until [stereo] true_stereo=1" : "NOT installed", ok);
        if (ok != 3) for (auto& hk : TS_HOOKS_US10) if (rd32(ram, hk[0]) == hk[2]) wr32(ram, hk[0], hk[1]);
    }
    else if (wp != PAD_HOOK_WORD) logf("HOOK pad skipped: word at %08X is %08X, expected %08X", T.pad_site, wp, T.pad_orig);
    if (const V13Addr* A = v13_addr(&T)) {   // v13 hooks
        uint32_t bar_jal = 0x0C000000 | ((A->drawbar >> 2) & 0x3FFFFFF), rec_jal = 0x0C000000 | ((BARREC >> 2) & 0x3FFFFFF);
        { uint8_t z[0x110] = {0}; wr(ram, BARTAB, z, sizeof z); }
        wr(ram, BARREC, BARCAVE_US10, sizeof BARCAVE_US10);
        int nb = 0, bad = 0;
        for (uint32_t site : A->drawbar_sites) { uint32_t w = rd32(ram, site);
            if (w == bar_jal) { wr32(ram, site, rec_jal); ++nb; } else if (w == rec_jal) ++nb; else ++bad; }
        g_v13_bars = nb > 0;
        logf("HOOK bars (boss / other LIFE on the wrist): %d of 5 menu_draw_bar calls recorded%s", nb, bad ? " -- some call words were not the expected ones" : "");
        uint32_t sm_jal = 0x0C000000 | ((A->screenmodels >> 2) & 0x3FFFFFF), h_jal = 0x0C000000 | ((HCODE >> 2) & 0x3FFFFFF);
        { uint8_t z[0x80] = {0}; wr(ram, HMB, z, sizeof z); }      // hands off until the writer turns them on
        wr(ram, HCODE, HANDCAVE_US10, sizeof HANDCAVE_US10);
        uint32_t hw = rd32(ram, A->hands_site);
        if (hw == sm_jal) { wr32(ram, A->hands_site, h_jal); g_v13_hands = true; logf("HOOK hands installed at %08X (DG_ScreenObjs -> DG_ScreenModels)", A->hands_site); }
        else if (hw == h_jal) g_v13_hands = true;
        else { g_v13_hands = false; logf("HOOK hands skipped: word at %08X is %08X, expected %08X", A->hands_site, hw, sm_jal); }
        g_hands_live = false; g_hands_fail = false;
    }
    uint32_t w2 = rd32(ram, T.hook2_site);
    if (w2 == T.hook2_orig) { wr32(ram, T.hook2_site, HOOK_WORD); logf("HOOK 2 (cutscene camera) installed at %08X", T.hook2_site); }
    else if (w2 != HOOK_WORD) logf("HOOK 2 skipped: word at %08X is %08X, expected %08X", T.hook2_site, w2, T.hook2_orig);
    h.installed = true;
    h.installed_at = GetTickCount64();
    h.gvt_at_install = rd32(ram, T.gv_time);
    h.reported_running = h.reported_not_running = false;
    g_mode_want = 0; g_mode_cur = 0;
    g_ram = ram; g_T = &T; g_hook_ready = true; g_scan_epoch++;
    logf("HOOK installed (%s): %08X %08X -> %08X (jal %08X), routine %u bytes, mailbox %08X",
         T.name, T.hook_site, T.hook_orig, HOOK_WORD, CAVE, CAVE_WORDS * 4, MBOX);
    return true;
}

static bool act_site(const GameTable& T, uint32_t* site, uint32_t* orig, const uint32_t** code)
{
    if (&T == &T_US10) { *site = 0x80015138; *orig = 0x0200F809; *code = ACTCAVE_US10; return true; }   // jalr $s0
    if (&T == &T_PAL)  { *site = 0x80014E14; *orig = 0x0040F809; *code = ACTCAVE_PAL;  return true; }   // jalr $v0
    return false;
}
static void act_maintain(uint8_t* ram, const GameTable& T)
{
    uint32_t site, orig; const uint32_t* code;
    if (!act_site(T, &site, &orig, &code)) return;
    static int skipped_logs = 0;
    uint32_t w = rd32(ram, site);
    uint32_t buf[ACTCAVE_WORDS]; bool intact = rd(ram, ACTCAVE, buf, sizeof buf) && memcmp(buf, code, sizeof buf) == 0;
    if (w == ACT_HOOK_WORD) {
        if (!intact) { wr32(ram, site, orig); g_act_hook = false; logf("HOOK act: wrapper overwritten; original jalr restored at %08X", site); }
        else g_act_hook = true;
        return;
    }
    if (w != orig) { if (skipped_logs++ < 3) logf("HOOK act skipped: word at %08X is %08X, expected %08X", site, w, orig); g_act_hook = false; return; }
    if (!intact) {
        uint8_t z[0x20] = {0}; wr(ram, ACTMB, z, sizeof z);
        wr(ram, ACTCAVE, code, ACTCAVE_WORDS * 4);
        if (!rd(ram, ACTCAVE, buf, sizeof buf) || memcmp(buf, code, sizeof buf) != 0) { logf("HOOK act refused: wrapper did not read back"); return; }
    }
    wr32(ram, site, ACT_HOOK_WORD); g_act_hook = true;
    logf("HOOK act (box view, weapons with a hidden body) installed at %08X -> jal %08X", site, ACTCAVE);
}
static void uninstall(uint8_t* ram, const GameTable& T, HookState& h, const char* why)
{
    { uint32_t site, orig; const uint32_t* code;
      if (act_site(T, &site, &orig, &code) && rd32(ram, site) == ACT_HOOK_WORD) wr32(ram, site, orig); g_act_hook = false; }
    g_hook_ready = false; g_mode_want = 0; g_mode_cur = 0;
    if (rd32(ram, T.hook_site) == HOOK_WORD) wr32(ram, T.hook_site, T.hook_orig);
    if (rd32(ram, T.hook2_site) == HOOK_WORD) wr32(ram, T.hook2_site, T.hook2_orig);
    if (rd32(ram, T.pad_site) == PAD_HOOK_WORD) wr32(ram, T.pad_site, T.pad_orig);
    if (&T == &T_US10) { wr32(ram, TSMB, 0); for (auto& hk : TS_HOOKS_US10) if (rd32(ram, hk[0]) == hk[2]) wr32(ram, hk[0], hk[1]); g_ts_installed = false; g_ts_on = false; }
    wr32(ram, PMBOX, 0);
    if (rd32(ram, MBOX) == MBOX_MAGIC) wr32(ram, MBOX + MB_ENABLE, 0);
    h.installed = false; h.enable_word = 0;
    logf("HOOK removed (%s): original instruction %08X restored at %08X", why, T.hook_orig, T.hook_site);
}

static void maintain(uint8_t* ram, const GameTable& T, HookState& h)
{
    uint32_t site = rd32(ram, T.hook_site);
    bool intact = cave_intact(ram, T);
    if (site == HOOK_WORD && intact) {
        if (rd32(ram, T.hook2_site) == T.hook2_orig) { wr32(ram, T.hook2_site, HOOK_WORD); logf("HOOK 2 re-applied"); }
        if (rd32(ram, T.pad_site) == T.pad_orig) { wr32(ram, T.pad_site, PAD_HOOK_WORD); logf("HOOK pad re-applied"); }
        return;
    }
    g_hook_ready = false;
    if (rd32(ram, T.hook2_site) == HOOK_WORD) wr32(ram, T.hook2_site, T.hook2_orig);
    if (rd32(ram, T.pad_site) == PAD_HOOK_WORD) wr32(ram, T.pad_site, T.pad_orig);
    if (site != HOOK_WORD && site != T.hook_orig) {
        logf("HOOK site changed to %08X by something else; standing down", site);
        h.installed = false;
        return;
    }
    ++h.repairs;
    logf("REPAIR #%d: hook word=%08X routine intact=%d", h.repairs, site, (int)intact);
    if (site == HOOK_WORD) wr32(ram, T.hook_site, T.hook_orig);
    h.installed = false;
    if (h.repairs <= 50) install(ram, T, h);
    else logf("REPAIR limit reached; hook left off");
}

static void log_status(uint8_t* ram, const GameTable& T, HookState& h, const char* tag)
{
    uint32_t calls = rd32(ram, MBOX + MB_CALLS), applied = rd32(ram, MBOX + MB_APPLIED), last = rd32(ram, MBOX + MB_LAST);
    uint32_t gvt = rd32(ram, T.gv_time);
    int16_t fp = 0, m[9] = {0}, scr = 0; int32_t t[3] = {0}; char stage[12] = {0};
    rd(ram, T.gm_camera + 0x22, &fp, 2);
    rd(ram, T.chanl1 + 0x30, m, 18); rd(ram, T.chanl1 + 0x44, t, 12); rd(ram, T.chanl1 + 0x50, &scr, 2);
    rd(ram, T.stage_name, stage, 8);
    for (int i = 0; i < 8; ++i) if (stage[i] && (stage[i] < 32 || stage[i] > 126)) stage[i] = '?';
    double heading = atan2((double)m[2], (double)m[8]) * 57.29578;
    double pitch = asin(fmax(-1.0, fmin(1.0, m[5] / 4096.0))) * 57.29578;
    logf("%s hook=%s tracking=%d head_valid=%d calls=%u applied=%u last_eye=%08X gv_time=%u stage='%s' first_person=%d screen=%d "
         "view heading %.1f pitch %.1f t=(%d,%d,%d) | head yaw %.1f pitch %.1f roll %.1f | walk heading %.1f",
         tag, h.installed ? "ON" : "off", h.enable_word, (int)g_head_valid.load(), calls, applied, last, gvt, stage, fp, scr,
         heading, pitch, t[0], t[1], t[2], g_head_yaw_cdeg / 100.0, g_head_pitch_cdeg / 100.0, g_head_roll_cdeg / 100.0,
         g_walk_heading_cdeg / 100.0);
}


// ---------------------------------------------------------------- performance counters (own thread: PDH GPU queries can take tens of ms)
// Answers "is the stutter the PC or the wireless link?": GPU 3D load (all processes / this game), video-encode load
// (Virtual Desktop / Air Link encode the headset stream there), CPU, and network send rate (the stream itself).
static double pdh_sum(PDH_HCOUNTER c, const wchar_t* must, int* n_out)
{
    DWORD bytes = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, nullptr) != PDH_MORE_DATA) { if (n_out) *n_out = 0; return -1; }
    std::vector<uint8_t> buf(bytes);
    auto* it = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
    if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, it) != ERROR_SUCCESS) { if (n_out) *n_out = 0; return -1; }
    double sum = 0; int n = 0;
    for (DWORD i = 0; i < count; ++i) {
        if (it[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && it[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
        if (must && !wcsstr(it[i].szName, must)) continue;
        sum += it[i].FmtValue.doubleValue; ++n;
    }
    if (n_out) *n_out = n;
    return sum;
}

static int wifi_signal_percent()
{
    // signal quality of the connected Wi-Fi adapter, if the PC itself is on Wi-Fi (wlanapi loaded dynamically)
    typedef DWORD (WINAPI *OpenH)(DWORD, PVOID, PDWORD, PHANDLE);
    typedef DWORD (WINAPI *Enum)(HANDLE, PVOID, void**);
    typedef DWORD (WINAPI *Query)(HANDLE, const GUID*, int, PVOID, PDWORD, PVOID*, PVOID);
    typedef VOID (WINAPI *FreeM)(PVOID);
    typedef DWORD (WINAPI *CloseH)(HANDLE, PVOID);
    static HMODULE w = LoadLibraryW(L"wlanapi.dll");
    if (!w) return -1;
    auto o = (OpenH)GetProcAddress(w, "WlanOpenHandle"); auto e = (Enum)GetProcAddress(w, "WlanEnumInterfaces");
    auto q = (Query)GetProcAddress(w, "WlanQueryInterface"); auto f = (FreeM)GetProcAddress(w, "WlanFreeMemory");
    auto c = (CloseH)GetProcAddress(w, "WlanCloseHandle");
    if (!o || !e || !q || !f || !c) return -1;
    DWORD ver = 0; HANDLE h = nullptr;
    if (o(2, nullptr, &ver, &h) != 0) return -1;
    int best = -1;
    struct IfInfo { GUID g; wchar_t desc[256]; int state; };
    struct IfList { DWORD n, idx; IfInfo i[1]; };
    IfList* l = nullptr;
    if (e(h, nullptr, (void**)&l) == 0 && l) {
        for (DWORD k = 0; k < l->n; ++k) {
            if (l->i[k].state != 1) continue;                       // wlan_interface_state_connected
            DWORD sz = 0; uint8_t* a = nullptr;
            if (q(h, &l->i[k].g, 7 /*current_connection*/, nullptr, &sz, (PVOID*)&a, nullptr) == 0 && a) {
                // WLAN_CONNECTION_ATTRIBUTES: state(4) mode(4) profile[256 wchar](512) then WLAN_ASSOCIATION_ATTRIBUTES:
                // DOT11_SSID(4+32) bsstype(4) bssid(6) +pad(2) phy type(4) phy index(4) signal quality(4)
                size_t off = 4 + 4 + 512 + 36 + 4 + 6 + 2 + 4 + 4;
                if (sz >= off + 4) { DWORD qv; memcpy(&qv, a + off, 4); if (qv <= 100 && (int)qv > best) best = (int)qv; }
                f(a);
            }
        }
        f(l);
    }
    c(h, nullptr);
    return best;
}

static DWORD WINAPI perf_thread(LPVOID)
{
    PDH_HQUERY q = nullptr;
    Sleep(5000);                                      // the ini is read by then
    if (!g_cfg.perf_counters) logf("PERF: performance counters OFF ([motion] perf_counters=0) -- no GPU / encoder / network / Wi-Fi queries");
    while (!g_cfg.perf_counters) Sleep(1000);         // open nothing until switched on
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) { logf("PERF: performance counters unavailable"); return 0; }
    PDH_HCOUNTER c3d = nullptr, cenc = nullptr, ccpu = nullptr, cnet = nullptr;
    PDH_STATUS s1 = PdhAddEnglishCounterW(q, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &c3d);
    PDH_STATUS s2 = PdhAddEnglishCounterW(q, L"\\GPU Engine(*engtype_VideoEncode)\\Utilization Percentage", 0, &cenc);
    PDH_STATUS s3 = PdhAddEnglishCounterW(q, L"\\Processor(_Total)\\% Processor Time", 0, &ccpu);
    PDH_STATUS s4 = PdhAddEnglishCounterW(q, L"\\Network Interface(*)\\Bytes Sent/sec", 0, &cnet);
    logf("PERF: counters gpu3d %s, encode %s, cpu %s, network %s", s1 ? "missing" : "ok", s2 ? "missing" : "ok",
         s3 ? "missing" : "ok", s4 ? "missing" : "ok");
    wchar_t self[32]; swprintf(self, 32, L"pid_%lu_", (unsigned long)GetCurrentProcessId());
    PdhCollectQueryData(q);
    g_perf_ok = true;
    bool was_on = true;
    for (;;) {
        Sleep(2000);
        bool on = g_cfg.perf_counters != 0;
        if (on != was_on) { logf("PERF: performance counters %s", on ? "ON" : "OFF (no GPU / encoder / network / Wi-Fi queries)"); was_on = on; }
        if (!on) { g_perf_gpu3d = g_perf_gpu3d_self = g_perf_enc = g_perf_cpu = g_perf_net_kbps = g_perf_wifi = -1; continue; }
        if (PdhCollectQueryData(q) != ERROR_SUCCESS) continue;
        int n = 0; double v;
        if (!s1) { v = pdh_sum(c3d, nullptr, &n); g_perf_gpu3d = n ? (int)(v < 100 ? v : 100) : -1;
                   v = pdh_sum(c3d, self, &n);    g_perf_gpu3d_self = n ? (int)(v < 100 ? v : 100) : 0; }
        if (!s2) { v = pdh_sum(cenc, nullptr, &n); g_perf_enc = n ? (int)(v < 100 ? v : 100) : 0; }
        if (!s3) { PDH_FMT_COUNTERVALUE cv; if (PdhGetFormattedCounterValue(ccpu, PDH_FMT_DOUBLE, nullptr, &cv) == ERROR_SUCCESS) g_perf_cpu = (int)cv.doubleValue; }
        if (!s4) { v = pdh_sum(cnet, nullptr, &n); g_perf_net_kbps = n ? (int)(v * 8 / 1000) : -1; }
        g_perf_wifi = wifi_signal_percent();
    }
}

// ---------------------------------------------------------------- menu thread: Quest buttons as keys outside the PS1 game; F11 test
static DWORD WINAPI menu_thread(LPVOID)
{
    uint32_t last_calls = 0; ULONGLONG calls_changed = 0; int last_state = -1; bool f11_prev = false;
    for (;;) {
        Sleep(10);
        ULONGLONG now = GetTickCount64();
        bool f11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (f11 && !f11_prev) {
            bool b = !g_test_blank.load(); g_test_blank = b;
            logf("F11: headset test colour %s (%s)", b ? "ON" : "OFF",
                 b ? "no game picture is copied at all -- if the headset is still slow now, the slowdown is not the picture hand-off"
                   : "game picture back");
        }
        f11_prev = f11;
        static bool f5_prev = false;
        bool f5 = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        if (f5 && !f5_prev) { bool b = !g_stereo_on.load(); g_stereo_on = b; logf("F5: stereo %s", b ? "ON" : "OFF (both eyes show the same picture)"); }
        f5_prev = f5;
        // is the PS1 game running? (our pad routine's call counter moves every game frame)
        bool live = false;
        uint8_t* ram = g_ram.load(); const GameTable* T = g_T.load();
        if (ram && T && g_hook_ready.load() && rd32(ram, T->pad_site) == PAD_HOOK_WORD) {
            uint32_t c = rd32(ram, PMBOX + 0x10);
            if (c != last_calls) { last_calls = c; calls_changed = now; }
            live = now - calls_changed < 300;
        }
        VrPad vp; AcquireSRWLockShared(&g_vrpad_lock); vp = g_vrpad; ReleaseSRWLockShared(&g_vrpad_lock);
        int method = g_cfg.menu_method;
        bool want = !live && g_cfg.controls && method != 3 && vp.active;
        uint8_t keys[256] = {0};
        if (want) {
            uint16_t bb = vp.buttons & ~(g_cfg.map_a | g_cfg.map_b);   // v21: A / B are Enter / Escape in the menus (GOG menu_keys)
            for (int b = 0; b < 16; ++b) if ((bb >> b) & 1) { uint8_t k = g_cfg.menu_key[b]; if (k) keys[k] = 0x80; }
            if (vp.pa) keys[0x1C] = 0x80;
            if (vp.pb) keys[0x01] = 0x80;
        }
        for (int k = 0; k < 256; ++k) g_inj[k] = keys[k];
        bool di_reads = now - g_di_kbd_read_ms.load() < 500;
        bool mcm = now < g_mc_menu_until.load();   // v17: E + M = back to the Master Collection menu
        if (mcm) { keys[0x12] = 0x80; keys[0x32] = 0x80; for (int k = 0; k < 256; ++k) g_inj[k] = keys[k]; }
        bool use_di = mcm || (want && (method == 1 || (method == 0 && di_reads)));   // v21: E + M through both routes
        bool use_si = mcm || (want && (method == 2 || (method == 0 && !di_reads)));
        {   static bool mcm_was = false;
            if (mcm && !mcm_was) {
                HWND fg = GetForegroundWindow();
                bool ours = g_game_hwnd && (fg == g_game_hwnd || GetAncestor(g_game_hwnd, GA_ROOT) == fg);
                if (!ours && g_game_hwnd) SetForegroundWindow(GetAncestor(g_game_hwnd, GA_ROOT));
                logf("MENUS: E + M held 500 ms (DirectInput %s, SendInput; game window %s)", di_reads ? "read by the game" : "not read now",
                     ours ? "in front" : "brought to the front");
            }
            mcm_was = mcm; }
        g_inj_on = use_di; g_menus_active = !live;
        sendinput_sync(use_si);
        int state = !vp.active ? 0 : live ? 1 : use_di ? 2 : use_si ? 3 : 4;
        if (state != last_state) {
            static const char* names[] = {"no VR controllers", "PS1 game running: Quest buttons go to the game pad",
                                          "menus: Quest buttons act as keys (DirectInput)", "menus: Quest buttons act as keys (SendInput to the game window)",
                                          "menus: Quest buttons not sent (method=off or controls disabled)"};
            logf("MENUS: %s", names[state]);
            last_state = state;
        }
    }
}

static DWORD WINAPI main_thread(LPVOID)
{
    Sleep(2000);
    CreateDirectoryW((g_dir + L"mgs1mc_vr_probe").c_str(), nullptr);
    g_cfg = read_ini(true);
    g_tag_delay = g_cfg.tag_delay;
    install_present_hook();
    CreateThread(nullptr, 0, writer_thread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, perf_thread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, menu_thread, nullptr, 0, nullptr);
    if (g_cfg.vr) CreateThread(nullptr, 0, xr_thread, nullptr, 0, nullptr);
    else logf("XR: vr=0 in the ini; no headset session");

    uint8_t* ram = nullptr; Ver ver = V_NONE; uint32_t off = 0; const GameTable* T = nullptr;
    HookState h;
    ULONGLONG last_scan_log = 0, last_ini = 0, last_status = 0, last_maint = 0;
    bool f8_prev = false, f9_prev = false, f6_prev = false, f10_prev = false, f7_prev = false, walk_on = true;
    uint32_t last_gs = 0xFFFFFFFF; int16_t last_pl = -99, last_fp = -99;

    for (;;) {
        ULONGLONG now = GetTickCount64();
        if (!ram) {
            g_hook_ready = false; g_ram = nullptr;
            ram = find_ram(&ver, &off);
            if (!ram) {
                if (now - last_scan_log > 10000) { logf("waiting for PS1 RAM (0x211000 region)..."); last_scan_log = now; }
                Sleep(1000); continue;
            }
            static uint8_t* logged_ram = nullptr; static Ver logged_ver = V_NONE;
            if (ram != logged_ram || ver != logged_ver) {
                logf("PS1 RAM at %p (region base + 0x%X), version: %s", ram, off, ver_name(ver));
                logged_ram = ram; logged_ver = ver;
            }
            bool keep_on = h.runtime_on;
            h = HookState(); h.runtime_on = keep_on;
            last_gs = 0xFFFFFFFF; last_pl = -99; last_fp = -99;
            T = table_for(ver);
            if (!T) {
                if (ver == V_US_OTHER) logf("US version without KETCHUP (probably 1.1): no address table yet; not patching");
                Sleep(2000); ram = nullptr; continue;
            }
        }

        if (version_of(ram) != ver || !signature_ok(ram, *T)) {
            logf("PS1 RAM at %p no longer looks like %s; rescanning", ram, T->name);
            g_hook_ready = false; g_ram = nullptr;
            g_scan_ok = false;
            ram = nullptr; Sleep(1000); continue;
        }

        if (now - last_ini >= 2000 || g_ini_force.exchange(false)) {
            Settings n = read_ini(false);
            if (n.enabled != g_cfg.enabled || n.track_third_person != g_cfg.track_third_person ||
                n.recentre_on_first_person != g_cfg.recentre_on_first_person ||
                n.screen_distance_m != g_cfg.screen_distance_m || n.screen_width_m != g_cfg.screen_width_m ||
                n.tag_delay != g_cfg.tag_delay || n.fov_screen != g_cfg.fov_screen || n.crop_auto != g_cfg.crop_auto ||
                n.crop_x != g_cfg.crop_x || n.crop_y != g_cfg.crop_y || n.crop_w != g_cfg.crop_w || n.crop_h != g_cfg.crop_h ||
                n.tracked_distance_m != g_cfg.tracked_distance_m || n.cutscene_head_look != g_cfg.cutscene_head_look ||
                n.walk_mode != g_cfg.walk_mode || n.eye_forward != g_cfg.eye_forward || n.eye_up != g_cfg.eye_up ||
                n.stick_deadzone != g_cfg.stick_deadzone || n.turn_speed_deg != g_cfg.turn_speed_deg ||
                n.turn_mode != g_cfg.turn_mode || n.snap_deg != g_cfg.snap_deg || n.body_follows_head != g_cfg.body_follows_head ||
                n.body_deadband_deg != g_cfg.body_deadband_deg || n.body_turn_speed_deg != g_cfg.body_turn_speed_deg ||
                n.controls != g_cfg.controls || n.map_a != g_cfg.map_a || n.map_b != g_cfg.map_b || n.map_x != g_cfg.map_x ||
                n.map_y != g_cfg.map_y || n.map_lt != g_cfg.map_lt || n.map_rt != g_cfg.map_rt || n.map_lg != g_cfg.map_lg ||
                n.map_rg != g_cfg.map_rg || n.map_lclick != g_cfg.map_lclick || n.map_rclick != g_cfg.map_rclick ||
                n.map_menu != g_cfg.map_menu || n.rclick_recentre != g_cfg.rclick_recentre || !n.stereo_same(g_cfg) ||
                n.cut_snake_eyes != g_cfg.cut_snake_eyes || n.screen_codec != g_cfg.screen_codec || n.screen_pause != g_cfg.screen_pause ||
                n.screen_menus != g_cfg.screen_menus || n.hud_wrist != g_cfg.hud_wrist || n.hud_width_mm != g_cfg.hud_width_mm ||
                n.hud_gaze_deg != g_cfg.hud_gaze_deg || memcmp(n.hud_off_cm, g_cfg.hud_off_cm, sizeof n.hud_off_cm) != 0 ||
                memcmp(n.hud_rot_deg, g_cfg.hud_rot_deg, sizeof n.hud_rot_deg) != 0 || memcmp(n.hud_rect, g_cfg.hud_rect, sizeof n.hud_rect) != 0 || n.cut_eye_forward != g_cfg.cut_eye_forward || n.cut_n_models != g_cfg.cut_n_models ||
                memcmp(n.cut_models, g_cfg.cut_models, sizeof n.cut_models) != 0 || n.gun_in_hand != g_cfg.gun_in_hand || n.perf_counters != g_cfg.perf_counters || n.hide_body != g_cfg.hide_body ||
                memcmp(n.hud_sel, g_cfg.hud_sel, sizeof n.hud_sel) != 0 || memcmp(n.radar_ps1, g_cfg.radar_ps1, sizeof n.radar_ps1) != 0 ||
                n.lean != g_cfg.lean || n.lean_max_cm != g_cfg.lean_max_cm || n.throw_from_hand != g_cfg.throw_from_hand ||
                n.fp_overlays != g_cfg.fp_overlays || n.hands != g_cfg.hands || n.hands_collision != g_cfg.hands_collision ||
                n.hands_knock != g_cfg.hands_knock || n.scope_lens != g_cfg.scope_lens || n.scope_display_deg != g_cfg.scope_display_deg ||
                n.scope_pitch_sign != g_cfg.scope_pitch_sign || n.aim_by_head != g_cfg.aim_by_head || n.boss_bars != g_cfg.boss_bars ||
                n.item_use_button != g_cfg.item_use_button || n.hand_r != g_cfg.hand_r || n.hand_l != g_cfg.hand_l ||
                memcmp(n.hand_left_parts, g_cfg.hand_left_parts, sizeof n.hand_left_parts) != 0 ||
                memcmp(n.hand_keep_parts, g_cfg.hand_keep_parts, sizeof n.hand_keep_parts) != 0 ||
                memcmp(n.hud_mount_side, g_cfg.hud_mount_side, sizeof n.hud_mount_side) != 0 || n.hud_list_mount != g_cfg.hud_list_mount ||
                n.hud_hide_boxes != g_cfg.hud_hide_boxes || n.hud_hide_life != g_cfg.hud_hide_life || n.hud_hide_radar != g_cfg.hud_hide_radar ||
                n.hud_hide_style != g_cfg.hud_hide_style || n.hud_show_boxes != g_cfg.hud_show_boxes || n.hud_show_radar != g_cfg.hud_show_radar ||
                n.hud_bar_follows_max != g_cfg.hud_bar_follows_max || n.hud_bar_scale_pct != g_cfg.hud_bar_scale_pct ||
                n.hud_select_on_wrist != g_cfg.hud_select_on_wrist ||
                memcmp(n.hud_in_off_cm, g_cfg.hud_in_off_cm, sizeof n.hud_in_off_cm) != 0 || memcmp(n.hud_in_rot_deg, g_cfg.hud_in_rot_deg, sizeof n.hud_in_rot_deg) != 0 ||
                memcmp(n.hud_back_off_cm, g_cfg.hud_back_off_cm, sizeof n.hud_back_off_cm) != 0 || memcmp(n.hud_back_rot_deg, g_cfg.hud_back_rot_deg, sizeof n.hud_back_rot_deg) != 0 ||
                n.melee_punch != g_cfg.melee_punch || n.punch_speed_cms != g_cfg.punch_speed_cms || n.punch_reach_cm != g_cfg.punch_reach_cm ||
                n.melee_choke != g_cfg.melee_choke || n.choke_apart_cm != g_cfg.choke_apart_cm || n.choke_hold_ms != g_cfg.choke_hold_ms ||
                n.choke_jerk_cms != g_cfg.choke_jerk_cms || n.rumble != g_cfg.rumble ||
                n.gun_predict_ms != g_cfg.gun_predict_ms || n.gun_max_reach_mm != g_cfg.gun_max_reach_mm ||
                memcmp(n.gun_rot_deg, g_cfg.gun_rot_deg, sizeof n.gun_rot_deg) != 0 || memcmp(n.gun_off_mm, g_cfg.gun_off_mm, sizeof n.gun_off_mm) != 0 || memcmp(&n.v15, &g_cfg.v15, sizeof n.v15) != 0) {
                if (n.v15.diorama != g_cfg.v15.diorama) { g_view_mode = n.v15.diorama ? 1 : 0; logf("VIEW: %s (settings)", kViewName[g_view_mode.load()]); }
                if (memcmp(&n.v15, &g_cfg.v15, sizeof n.v15) != 0)
                    logf("INI v15: diorama=%d scale 1:%d table %d cm ahead / %d cm down, follow %d units at %.1f/s, cut %d units, reorient %d, "
                         "eye height %d, turn %d deg | third_person_lean=%d | set pieces in Snake's eyes=%d | wheel=%d (%d cm, %d cm up, stereo %d deg)",
                         n.v15.diorama, n.v15.dio_scale, n.v15.dio_dist_cm, n.v15.dio_drop_cm, n.v15.dio_follow_radius, n.v15.dio_follow_speed / 10.0,
                         n.v15.dio_cut_units, n.v15.dio_reorient, n.v15.dio_eye_height, n.v15.dio_turn_deg, n.v15.tp_lean, n.v15.setpiece_eyes,
                         n.v15.wheel, n.v15.wheel_size_cm, n.v15.wheel_height_cm, n.v15.wheel_stereo_deg);
                if (n.gun_in_hand != g_cfg.gun_in_hand || n.gun_predict_ms != g_cfg.gun_predict_ms ||
                    memcmp(n.gun_rot_deg, g_cfg.gun_rot_deg, sizeof n.gun_rot_deg) != 0 || memcmp(n.gun_off_mm, g_cfg.gun_off_mm, sizeof n.gun_off_mm) != 0)
                    logf("INI motion: gun_in_hand=%d predict %d ms, rotation %.0f,%.0f,%.0f deg, offset %.0f,%.0f,%.0f mm, max reach %d mm",
                         n.gun_in_hand, n.gun_predict_ms, n.gun_rot_deg[0], n.gun_rot_deg[1], n.gun_rot_deg[2],
                         n.gun_off_mm[0], n.gun_off_mm[1], n.gun_off_mm[2], n.gun_max_reach_mm);
                if (!n.stereo_same(g_cfg))
                    logf("INI stereo: enabled=%d ipd %d mm, %d units/m (half offset %d units), swap %d, hold %d units/frame %d deg/s %d ms, "
                         "eye timeout %d ms, min %d fps", n.stereo, n.ipd_mm, n.units_per_m, (int)lround(n.ipd_mm / 2000.0 * n.units_per_m),
                         n.swap_eyes, n.hold_units, n.hold_deg_s, n.hold_ms, n.eye_timeout_ms, n.min_fps);
                g_cfg = n; logf("INI changed: enabled=%d track_third_person=%d recentre_on_first_person=%d screen %.2f m at %.2f m | "
                                "tag_delay=%d fov_screen=%d crop=%s(%d,%d,%d,%d) tracked_distance=%.1f",
                                g_cfg.enabled, g_cfg.track_third_person, g_cfg.recentre_on_first_person, g_cfg.screen_width_m, g_cfg.screen_distance_m,
                                g_cfg.tag_delay, g_cfg.fov_screen, g_cfg.crop_auto == 2 ? "auto 4:3" : g_cfg.crop_auto == 1 ? "detect" : g_cfg.crop_auto == 0 ? "manual" : "off",
                                g_cfg.crop_x, g_cfg.crop_y, g_cfg.crop_w, g_cfg.crop_h, g_cfg.tracked_distance_m);
                g_tag_delay = g_cfg.tag_delay;
                logf("INI walk: walk_mode=%d eye_forward=%d eye_up=%d turn_speed=%.0f deadzone=%d",
                     g_cfg.walk_mode, g_cfg.eye_forward, g_cfg.eye_up, g_cfg.turn_speed_deg, g_cfg.stick_deadzone);
                if (h.installed) { uint16_t fov = (uint16_t)g_cfg.fov_screen; wr(ram, MBOX + MB_FOV, &fov, 2); }
            }
            last_ini = now;
        }

        bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (f8 && !f8_prev) { h.runtime_on = !h.runtime_on; logf("F8: head tracking %s", h.runtime_on ? "ON" : "OFF"); }
        f8_prev = f8;
        bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        if (f7 && !f7_prev) { walk_on = !walk_on; logf("F7: walking mode %s", walk_on ? "ON" : "OFF"); }
        f7_prev = f7;
        bool f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        if (f6 && !f6_prev) { g_recentre = true; logf("F6: recentre requested"); }
        f6_prev = f6;
        bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (f10 && !f10_prev) { g_save_frame = true; g_save_wrist = true; logf("F10: saving the next captured picture and the wrist panels"); }
        f10_prev = f10;
        bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (f9 && !f9_prev) {
            log_status(ram, *T, h, "F9");
            char why[40]; snprintf(why, sizeof why, "%s_F9_track%d", ver == V_PAL ? "pal" : "us10", h.enable_word);
            dump(ram, why);
        }
        f9_prev = f9;

        if (g_cfg.enabled && !h.installed && h.repairs <= 50) {
            if (now - last_maint >= 1000) { install(ram, *T, h); last_maint = now; }
        } else if (!g_cfg.enabled && h.installed) {
            uninstall(ram, *T, h, "ini enabled=0");
        } else if (h.installed && now - last_maint >= 250) {
            maintain(ram, *T, h); last_maint = now;
            // (act wrapper not installed: the emulator never executes a patch inside GV_ExecActorSystem)
        }

        // game state, first-person entry -> recentre
        uint32_t gs = rd32(ram, T->game_status); int16_t pl = 0, fp = 0;
        rd(ram, T->pauselevel, &pl, 2); rd(ram, T->gm_camera + 0x22, &fp, 2);
        {   // v16 PAUSE WHILE CHOOSING (GOG): the MENU bit (4) of GV_PauseLevel freezes the action like the game's own item menu.
            // Only taken from plain gameplay, only cleared if we set it; our own bit is hidden from the rest of the DLL.
            static bool owned = false;
            volatile LONG* plp = (volatile LONG*)(ram + (T->pauselevel & 0x1FFFFF));
            bool want = g_cfg.v15.pause_sel && g_cfg.hud_wrist && g_cfg.hud_select_on_wrist && g_sel_open.load() && !((gs >> 31) & 1);
            if (want && !owned && pl == 0) { InterlockedOr(plp, 4); owned = true; pl |= 4; logf("SELECT: game paused while choosing (GV_PauseLevel MENU bit, like the game's own menu)"); }
            else if (owned && !want) { InterlockedAnd(plp, ~4L); owned = false; pl &= ~4; logf("SELECT: closed -- game resumed"); }
            else if (owned && !(pl & 4)) { InterlockedOr(plp, 4); pl |= 4; }
            if (owned) pl &= ~4;
            g_sel_pause_owned = owned;
        }
        if (fp == 1 && last_fp == 0 && g_cfg.recentre_on_first_person) { g_recentre = true; }
        bool demo = (gs >> 31) & 1;
        g_demo = demo;
        if (demo && last_gs != 0xFFFFFFFF && !((last_gs >> 31) & 1) && g_cfg.cutscene_head_look) { g_recentre = true; }
        if (gs != last_gs || pl != last_pl || fp != last_fp) {
            logf("STATE gamestatus=%08X (cutscene bit31=%d) pauselevel=%d first_person=%d", gs, (gs >> 31) & 1, pl, fp);
            if (fp != last_fp && h.installed) log_status(ram, *T, h, fp == 1 ? "FIRST_PERSON_ON" : "FIRST_PERSON_OFF");
            last_gs = gs; last_pl = pl; last_fp = fp;
        }

        // cutscene cast: the routine records s3 (MGSDEMOACT) and ra when FrameRunDemo calls it
        bool cut_pov = false; uint32_t cut_objs = 0;
        {
            // a PS1 RAM pointer in any segment (KUSEG 0x00, KSEG0 0x80, KSEG1 0xA0) -> its KSEG0 form, or 0
            auto K = [](uint32_t a) -> uint32_t {
                uint32_t seg = a & 0xE0000000u;
                if ((seg != 0 && seg != 0x80000000u && seg != 0xA0000000u) || (a & 0x1FFFFFFFu) >= 0x200000u || (a & 0x1FFFFFu) < 0x10000u) return 0;
                return 0x80000000u | (a & 0x1FFFFFu); };
            static uint32_t last_act = 0, logged_act = 0, last_objs = 0; static int32_t last_j[3] = {0, 0, 0};
            static ULONGLONG last_scan = 0; static bool last_vis = false;
            uint32_t act_raw = rd32(ram, PMBOX + 0x14), ra = rd32(ram, PMBOX + 0x18), stamp = rd32(ram, PMBOX + 0x1C);
            uint32_t act = K(act_raw);
            uint32_t gvt = rd32(ram, T->gv_time);
            // the routine records s3 and GV_Time only when FrameRunDemo called it (routine v10), so a fresh stamp = a cutscene
            bool in_demo = demo && h.installed && gvt - stamp < 8 && act != 0;
            static ULONGLONG demo_since = 0, last_dbg = 0; static bool dumped = false;
            if (!demo) demo_since = 0; else if (!demo_since) demo_since = now;
            if (demo && !in_demo && now - last_dbg >= 3000 && demo_since && now - demo_since > 1000) {
                logf("CUTSCENE: no FrameRunDemo record (last caller %08X, record %u frames old, actor %08X)", ra, gvt - stamp, act);
                last_dbg = now;
            }
            if (in_demo && !dumped && demo_since && now - demo_since > 3000) {   // one RAM snapshot of a cutscene, for offline checks
                char why[40]; snprintf(why, sizeof why, "%s_cutscene", ver == V_PAL ? "pal" : "us10"); dump(ram, why); dumped = true;
                logf("CUTSCENE: RAM snapshot saved for analysis (actor %08X)", act);
            }
            if (in_demo && g_cfg.cut_snake_eyes && now - last_scan >= 100) {
                last_scan = now;
                uint32_t hdr = K(rd32(ram, act + 0x30)), models = K(rd32(ram, act + 0x34));
                int32_t n = hdr ? (int32_t)rd32(ram, hdr + 0x10) : 0;
                uint32_t mdl = n > 0 ? K(rd32(ram, hdr + 0x18)) : 0;
                if (!(n > 0 && n < 64 && mdl && models) && now - last_dbg >= 3000) {
                    logf("CUTSCENE: cast not readable (actor %08X header %08X n %d models-def %08X models %08X)", act, hdr, n, mdl, models);
                    last_dbg = now;
                }
                if (n > 0 && n < 64 && mdl && models) {
                    std::string cast; int snake = -1;
                    for (int i = 0; i < n; ++i) {
                        uint16_t fn = (uint16_t)rd32(ram, mdl + i * 0x14 + 0xC);
                        char b[16]; snprintf(b, sizeof b, "%s%04X", i ? "," : "", fn); cast += b;
                        for (int k = 0; k < g_cfg.cut_n_models && snake < 0; ++k) if (fn == g_cfg.cut_models[k]) snake = i;
                    }
                    if (snake >= 0) {
                        uint32_t objs = K(rd32(ram, models + snake * 0x1A4 + 0x7C));
                        int16_t nm = 0; if (objs) rd(ram, objs + 0x2E, &nm, 2);
                        if (objs && nm > 6 && nm < 64) cut_objs = objs;
                        else if (now - last_dbg >= 3000) { logf("CUTSCENE: Snake's model %d has no usable body (objs %08X, %d parts)", snake, objs, nm); last_dbg = now; }
                    }
                    if (act != logged_act) {
                        logf("CUTSCENE: cast of %d models [%s] -> Snake is %s", n, cast.c_str(),
                             snake >= 0 ? ("model " + std::to_string(snake)).c_str() : "not in this cast (director camera)");
                        logged_act = act;
                    }
                }
                last_objs = cut_objs;
            } else if (in_demo && g_cfg.cut_snake_eyes) cut_objs = last_objs;
            if (!in_demo) { last_objs = 0; last_act = 0; }
            if (cut_objs) {
                bool vis = !(rd32(ram, cut_objs + 0x28) & 0x80);          // DG_FLAG_INVISIBLE
                int32_t j[3] = {0, 0, 0}; rd(ram, cut_objs + 0x284, j, sizeof j);
                double jump = sqrt((double)(j[0] - last_j[0]) * (j[0] - last_j[0]) + (double)(j[1] - last_j[1]) * (j[1] - last_j[1]) +
                                   (double)(j[2] - last_j[2]) * (j[2] - last_j[2]));
                cut_pov = vis;
                if (vis && (act != last_act || !last_vis || jump > 1500)) {   // new cutscene, Snake back on set, or a cut
                    int16_t m[9] = {0}; rd(ram, cut_objs + 0x270, m, sizeof m);   // joint 6 world matrix
                    double yaw = atan2((double)m[2], (double)m[8]);              // head forward = column 2
                    int units = ((int)lround(yaw * 4096.0 / (2 * 3.14159265358979)) % 4096 + 4096) % 4096;
                    g_yaw_ref_units = units; g_yaw_ref_new = true; g_recentre = true;
                    logf("CUTSCENE: through Snake's eyes, facing %.1f deg (%s)", units * 360.0 / 4096,
                         act != last_act ? "new cutscene" : !last_vis ? "Snake back on set" : "camera cut");
                    last_act = act;
                }
                if (!vis && last_vis) logf("CUTSCENE: Snake off set -> director camera");
                last_vis = vis; memcpy(last_j, j, sizeof j);
            } else last_vis = false;
            g_cut_pov = cut_pov;
        }
        // Snake's body DG_OBJS for the walking eye (head joint objs[6]); refreshed every 100 ms in walk mode
        {
            static ULONGLONG last_find = 0; static uint32_t last_ptr = 0xFFFFFFFF;
            uint32_t ptr = 0;
            if (cut_pov) {   // cutscene: the demo cast's Snake
                if (cut_objs != last_ptr) { wr32(ram, MBOX + MB_SNAKEOBJS, cut_objs); last_ptr = cut_objs; }
            } else if (h.installed && g_mode_cur.load() == 3) {   // v15 diorama: the stand-in body (the writer also writes it)
                if (last_ptr != DIO_FAKE) { wr32(ram, MBOX + MB_SNAKEOBJS, DIO_FAKE); last_ptr = DIO_FAKE; }
            } else if (h.installed && g_mode_cur.load() == 2 && g_setpiece.load() && g_sp_objs.load()) {   // v15 set piece
                uint32_t sp = g_sp_objs.load();
                if (sp != last_ptr) { wr32(ram, MBOX + MB_SNAKEOBJS, sp); last_ptr = sp;
                                      logf("SETPIECE: eye from Snake's body inside the %s (DG_OBJS %08X)", kSpName[(g_sp_kind.load() + 7) % 7], sp); }
            } else if (h.installed && g_mode_cur.load() == 2 && now - last_find >= 100) {
                last_find = now;
                for (int lvl = 0; lvl < 9 && !ptr; ++lvl) {
                    uint32_t head = T->actor_lists + lvl * 0x44, node = rd32(ram, head + 4);
                    for (int n = 0; n < 256 && node && node != head && (node >> 21) == (0x80000000u >> 21); ++n) {
                        if (rd32(ram, node + 8) == T->snake_act) {
                            uint32_t objs = rd32(ram, node + 0x9C);            // SnaInitWork.body.objs
                            int16_t nm = 0; rd(ram, objs + 0x2E, &nm, 2);       // DG_OBJS.n_models
                            if ((objs >> 21) == (0x80000000u >> 21) && nm > 6 && nm < 64) ptr = objs;
                            break;
                        }
                        node = rd32(ram, node + 4);
                    }
                }
                if (ptr != last_ptr) {
                    wr32(ram, MBOX + MB_SNAKEOBJS, ptr);
                    logf(ptr ? "SNAKE: head joint from body DG_OBJS %08X" : "SNAKE: actor not found; eye uses GM_SnakeCamera", ptr);
                    last_ptr = ptr;
                }
            } else if (h.installed && g_mode_cur.load() != 2 && last_ptr != 0) {
                wr32(ram, MBOX + MB_SNAKEOBJS, 0); last_ptr = 0;
            }
        }
        // MOTION: the equipped weapon actor, found by its source name (GV_ACT +0x14) every 100 ms. Its layout is checked
        // against the decomp before the writer may touch it: objs->root must be Snake's hand joint (or our matrix).
        {
            static ULONGLONG last_gun = 0; static uint32_t logged_work = 0xFFFFFFFF; static ULONGLONG last_bad = 0;
            if (h.installed) { int16_t wid = -1; rd(ram, T->stage_name + 0x2C, &wid, 2); g_weapon_id = wid; }   // GM_CurrentWeaponId
            if (h.installed && now - last_gun >= 100) {
                last_gun = now;
                GunActor ga; uint32_t box_act = 0, weap_act = 0, box_objs = 0, box_orig = 0, menu_work = 0; int tenage_now = 0;
                uint32_t sp_node = 0; int sp_kind = -1; bool sna_live = false;   // v15 set pieces
                uint32_t blur_nodes[8] = {0}; int n_blur = 0;                      // v23 the game's afterimage effect
                uint32_t seen_gm[4] = {0}, seen_gg[4] = {0}; int n_gm = 0, n_gg = 0;
                auto k0 = [](uint32_t a) -> uint32_t { return ((a & 0x1FFFFFFFu) < 0x200000u && (a & 0x1FFFFFu) >= 0x10000u) ? (0x80000000u | (a & 0x1FFFFFu)) : 0; };
                for (int lvl = 0; lvl < 9; ++lvl) {
                    uint32_t head = T->actor_lists + lvl * 0x44, node = rd32(ram, head + 4);
                    for (int n = 0; n < 300 && node && node != head && (node >> 21) == (0x80000000u >> 21); ++n) {
                        uint32_t fnp = rd32(ram, node + 0x14), fk = k0(fnp);
                        char nm[16] = {0}; if (fk) rd(ram, fk, nm, 15);   // v10: 15 (was 8: "grenade.c" / "menuman.c" never matched)
                        int kind = !strcmp(nm, "socom.c") ? 1 : !strcmp(nm, "famas.c") ? 2 : !strcmp(nm, "grenade.c") ? 3 : 0;
                        if (!strcmp(nm, "box.c")) {   // box.c Work: object.objs +0x20, parent OBJECT* +0x48 (root = parent's objs[0].world)
                            box_act = rd32(ram, node + 8);
                            uint32_t bo = k0(rd32(ram, node + 0x20)), par = k0(rd32(ram, node + 0x48)), po = par ? k0(rd32(ram, par)) : 0;
                            uint32_t borig = po ? po + 0x48 : 0, br = bo ? rd32(ram, bo + 0x20) : 0;   // (box.c: unchanged)
                            if (bo && borig && (br == borig || br == FARMTX)) { box_objs = bo; box_orig = borig; }
                        }
                        if (!strcmp(nm, "menuman.c")) menu_work = node;
                        if (rd32(ram, node + 8) == T->snake_act) sna_live = true;
                        for (int q = 0; q < 7; ++q) if (!strcmp(nm, kSpFile[q]) && !sp_node) { sp_node = node; sp_kind = q; }
                        if ((!strcmp(nm, "blur.c") || !strcmp(nm, "blurpure.c") || !strcmp(nm, "focus.c")) && n_blur < 8) blur_nodes[n_blur++] = node;
                        if (nm[0]) {   // v24 diagnostics: each actor name once per stage
                            static char seen[96][16]; static int n_seen = 0; static char seen_stage[16] = {0};
                            char stg[16] = {0}; rd(ram, T->stage_name, stg, 8);
                            if (strcmp(stg, seen_stage)) { memcpy(seen_stage, stg, 16); n_seen = 0; }
                            bool had = false; for (int q = 0; q < n_seen; ++q) if (!strcmp(seen[q], nm)) { had = true; break; }
                            if (!had && n_seen < 96) { memcpy(seen[n_seen++], nm, 16); logf("ACTORS: '%s' running in stage '%s' (actor %08X)", nm, stg, node); }
                        }
                        if (!strcmp(nm, "tenage.c")) ++tenage_now;
                        if (!strcmp(nm, "gmsight.c") && n_gm < 4) seen_gm[n_gm++] = node;      // v11: live overlay actors
                        if (!strcmp(nm, "gglsight.c") && n_gg < 4) seen_gg[n_gg++] = node;
                        static const char* wn[] = {"socom.c", "famas.c", "grenade.c", "rifle.c", "rcm.c", "aam.c", "mine.c", "bomb.c"};
                        for (const char* w : wn) if (!strcmp(nm, w)) weap_act = rd32(ram, node + 8);
                        if (kind && !ga.work && rd32(ram, node + 8)) {
                            uint32_t oo = kind == 3 ? 0x28 : 0x20, ro = kind == 3 ? 0x24 : 0x48;
                            uint32_t objs = k0(rd32(ram, node + oo)), robj = k0(rd32(ram, node + ro)), unit = rd32(ram, node + 0x4C);
                            { uint32_t rraw = rd32(ram, node + ro);   // v11: our stand-in (0x8000ADC0 fails k0's range check) -> Snake's own
                              if (kind == 3 && (rraw & 0x1FFFFFFFu) == (THROWOBJ & 0x1FFFFFFFu)) robj = k0(g_throw_robj.load()); }
                            uint32_t pobjs = robj ? k0(rd32(ram, robj)) : 0;
                            uint32_t orig = pobjs && unit < 32 ? pobjs + 0x48 + unit * 0x5C : 0;
                            uint32_t root = objs ? rd32(ram, objs + 0x20) : 0;
                            if (objs && orig && (root == orig || root == GUNMTX)) {
                                ga.work = node; ga.act = rd32(ram, node + 8); ga.fname = fnp; ga.objs = objs; ga.orig = orig; ga.kind = kind; ga.objoff = oo;
                                ga.robj = robj; ga.unit = unit;
                                if (kind == 1) {   // socom.c Work: laser_prim +0x58, light_prim +0x10C (both rooted at the hand joint)
                                    uint32_t lp = k0(rd32(ram, node + 0x58)), gp = k0(rd32(ram, node + 0x10C));
                                    ga.laser = lp && rd32(ram, lp + 0x20) == orig || (lp && rd32(ram, lp + 0x20) == GUNMTX) ? lp : 0;
                                    ga.light = gp && rd32(ram, gp + 0x20) == orig || (gp && rd32(ram, gp + 0x20) == GUNMTX) ? gp : 0;
                                }
                            } else if (now - last_bad >= 5000) {
                                logf("MOTION: %s actor %08X does not match the expected layout (objs %08X root %08X, Snake obj %08X unit %u -> "
                                     "hand %08X); gun stays in Snake's hand", nm, node, objs, root, robj, unit, orig);
                                last_bad = now;
                            }
                        }
                        node = rd32(ram, node + 4);
                    }
                }
                {   // v15 SET PIECES: a vehicle / rope / rack actor that carries Snake's body. Its work holds a pointer to a
                    // DG_OBJS of Snake's model: 8-40 parts with the head joint (objs[6], world t at +0x284) posed near the root
                    // (+0x5C). The largest such body wins. torture.c / sne_03c.c count only while the normal Snake actor is gone.
                    static uint32_t logged_node = 0xFFFFFFFF; static ULONGLONG last_miss = 0;
                    uint32_t best = 0; int best_n = 0;
                    if (sp_node && sp_kind >= 5 && sna_live) sp_node = 0;
                    if (sp_node) {
                        for (uint32_t off = 0x20; off < 0x800 && !(best_n >= 16); off += 4) {
                            uint32_t q = k0(rd32(ram, sp_node + off)); if (!q) continue;
                            int16_t nm = 0; if (!rd(ram, q + 0x2E, &nm, 2) || nm < 8 || nm > 40) continue;
                            int32_t rt[3] = {0, 0, 0}, hd[3] = {0, 0, 0};
                            rd(ram, q + 0x5C, rt, sizeof rt); rd(ram, q + 0x284, hd, sizeof hd);
                            double dd = sqrt((double)(hd[0] - rt[0]) * (hd[0] - rt[0]) + (double)(hd[1] - rt[1]) * (hd[1] - rt[1]) +
                                             (double)(hd[2] - rt[2]) * (hd[2] - rt[2]));
                            if (dd < 50 || dd > 2500) continue;
                            if (nm > best_n) { best = q; best_n = nm; }
                        }
                    }
                    if (sp_node != logged_node) {
                        if (sp_node && best) logf("SETPIECE: %s (%s, actor %08X): Snake's body DG_OBJS %08X, %d parts", kSpName[sp_kind], kSpFile[sp_kind], sp_node, best, best_n);
                        else if (sp_node) logf("SETPIECE: %s (%s, actor %08X) found, but no posed Snake body in it -- the view stays as it is", kSpName[sp_kind], kSpFile[sp_kind], sp_node);
                        else if (logged_node && logged_node != 0xFFFFFFFF) logf("SETPIECE: over");
                        logged_node = sp_node;
                    } else if (sp_node && !best && now - last_miss > 10000) { last_miss = now; logf("SETPIECE: %s still without a posed body", kSpName[sp_kind]); }
                    g_sp_kind = best ? sp_kind : -1; g_sp_objs = best;
                }

                {   // v23: the game's afterimage blur off in VR views (its act -> a do-nothing routine), given back otherwise
                    static const uint32_t NOPFN = 0x8000B6A0;
                    static uint32_t patched[8][2] = {{0}}; static int n_p = 0;
                    bool off = !g_cfg.v15.blur_effect && g_mode_cur.load() != 0;
                    if (rd32(ram, NOPFN) != 0x03E00008u) { uint32_t f[2] = {0x03E00008u, 0}; wr(ram, NOPFN, f, sizeof f); }   // jr ra; nop
                    for (int i = 0; i < n_blur; ++i) {
                        uint32_t nd = blur_nodes[i], act = rd32(ram, nd + 8);
                        bool known = false; for (int k = 0; k < n_p; ++k) if (patched[k][0] == nd) known = true;
                        if (off && !known && act != NOPFN && n_p < 8) {
                            patched[n_p][0] = nd; patched[n_p][1] = act; ++n_p; wr32(ram, nd + 8, NOPFN);
                            logf("BLUR: the game's blur / focus effect (actor %08X) paused in the VR view -- it lays a shifted copy of the picture over the new one", nd);
                        }
                    }
                    for (int k = 0; k < n_p; ) {   // give back (flat view / setting on), or forget a gone actor
                        uint32_t nd = patched[k][0]; bool alive = false; for (int i = 0; i < n_blur; ++i) if (blur_nodes[i] == nd) alive = true;
                        if (!alive || !off) {
                            if (alive && rd32(ram, nd + 8) == NOPFN) { wr32(ram, nd + 8, patched[k][1]); logf("BLUR: blur / focus effect (actor %08X) given back", nd); }
                            patched[k][0] = patched[n_p - 1][0]; patched[k][1] = patched[n_p - 1][1]; --n_p;
                        } else ++k;
                    }
                }
                AcquireSRWLockExclusive(&g_gunact_lock); g_gunact = ga; ReleaseSRWLockExclusive(&g_gunact_lock);
                if (const V13Addr* A13 = v13_addr(T)) {   // v13: item use from the wrist list, through the game's own routine
                    static int pend_id = -1; static uint32_t pend_sq = 0; static int16_t cnt0 = 0, life0 = 0; static ULONGLONG pend_at = 0;
                    int16_t lv2[64] = {0}; rd(ram, T->stage_name + 0x10, lv2, sizeof lv2);
                    if (pend_id >= 0) {
                        if (gc_done(ram, SL_ITEM, pend_sq)) {
                            logf("ITEM: %s used through the game's own routine -- count %d -> %d, LIFE %d -> %d", kGogItem[pend_id],
                                 cnt0, lv2[37 + pend_id], life0, lv2[11]);
                            pend_id = -1;
                        } else if (now - pend_at > 3000) { logf("ITEM: the game did not run the item use within 3 s -- dropped"); pend_id = -1; }
                    } else if (int rq = g_item_use_req.exchange(0)) {
                        int id = rq - 1;
                        if (lv2[37 + id] <= 0) logf("ITEM: %s -- none left (count %d), nothing used", kGogItem[id], lv2[37 + id]);
                        else {
                            uint8_t lst[0x30] = {0}; int16_t pnl[2] = {(int16_t)id, lv2[37 + id]}; memcpy(lst + 0x20, pnl, 4);
                            wr(ram, ITEMLIST, lst, sizeof lst);
                            cnt0 = lv2[37 + id]; life0 = lv2[11]; pend_id = id; pend_at = now;
                            pend_sq = gc_issue(ram, SL_ITEM, A13->item_use, ITEMLIST, 0x20);   // item_use_check(list, PAD_CIRCLE)
                        }
                    }
                }
                g_box_orig = box_orig; g_box_objs = box_objs;
                if (g_v13_hands.load()) {   // v13 hands: is the routine called (alive), and does it reach Snake's body when on (applied)?
                    static uint32_t alive_prev = 0, applied_prev = 0; static ULONGLONG on_since = 0, applied_at = 0; static bool said_live = false;
                    uint32_t alive = rd32(ram, HMB + 0x0C), applied = rd32(ram, HMB + 0x10), en = rd32(ram, HMB);
                    if (alive != alive_prev && !g_hands_live.load()) { g_hands_live = true; if (!said_live) { said_live = true; logf("HANDS: the draw routine runs (the game executes our patch in DG_ScreenObjs)"); } }
                    alive_prev = alive;
                    static uint32_t alive_mark = 0; static ULONGLONG fail_at = 0;   // v22: counted in game frames (draw-routine calls), retried
                    if (en) {
                        if (!on_since) { on_since = now; alive_mark = alive; }
                        if (applied != applied_prev) alive_mark = alive;
                        if (!g_hands_fail.load() && alive - alive_mark > 300) {
                            g_hands_fail = true; fail_at = now;
                            logf("HANDS: Snake's body did not reach the hands routine in %u draw calls (applied %u) -- hands off, retry in 10 s", alive - alive_mark, applied);
                        }
                    } else { on_since = 0; alive_mark = alive; }
                    if (g_hands_fail.load() && now - fail_at > 10000) { g_hands_fail = false; alive_mark = alive; on_since = 0; logf("HANDS: retrying"); }
                    applied_prev = applied;
                }
                {   // v11 FIRST-PERSON OVERLAYS: box slit, gas mask, NV / thermal goggle frame, made by the game's own code
                    enum { OV_NONE, OV_BOX, OV_MASK, OV_NVG, OV_THERM };
                    static const char* const ovn[5] = {"none", "cardboard box view", "gas mask view", "night-vision goggle view", "thermal goggle view"};
                    static int cur = OV_NONE, pend_kind = OV_NONE, pend_op = 0, fails = 0; static uint32_t actor = 0, pend_seq = 0, epoch = 0, alive0 = 0;
                    static double pend_t = 0, retry_at = 0; static bool logged_pal = false;
                    const OvlAddr* A = strstr(T->name, "US 1.0") ? &OVL_US10 : nullptr;
                    double tnow = (double)now;
                    if (epoch != g_call_epoch.load()) { epoch = g_call_epoch.load(); cur = OV_NONE; pend_op = 0; actor = 0; }
                    if (!A && g_cfg.fp_overlays && !logged_pal) { logged_pal = true; logf("FP: first-person overlays are US 1.0 only so far (this is %s)", T->name); }
                    int16_t lvw = -1, lvi = -1; rd(ram, T->stage_name + 0x10 + 14 * 2, &lvw, 2); rd(ram, T->stage_name + 0x10 + 15 * 2, &lvi, 2);
                    bool on = A && g_cfg.fp_overlays && g_mode_cur.load() == 2 && !g_cut_pov.load() && !g_demo.load() && g_pl_now.load() == 0;
                    bool aimable = lvw != 4 && lvw != 9;   // the game drops these views for the Stinger and the PSG1 (their own sights)
                    int want = !on ? OV_NONE : box_act ? OV_BOX : !aimable ? OV_NONE : lvi == 7 ? OV_MASK : lvi == 5 ? OV_NVG : lvi == 6 ? OV_THERM : OV_NONE;
                    uint32_t sight_cur = A ? rd32(ram, A->sight_cur) : 0xFFFFFFFFu;
                    if (pend_op) {   // a call in flight
                        if (rd32(ram, CMB + 0x24) == pend_seq) {
                            uint32_t v0 = rd32(ram, CMB + 0x20); pend_op = 0;
                            if (pend_kind == OV_NONE) { logf("FP: %s off", ovn[cur]); cur = OV_NONE; actor = 0; }
                            else if (v0) { cur = pend_kind; actor = v0; fails = 0;
                                           logf("FP: %s on -- made by the game itself (actor %08X)", ovn[cur], v0); }
                            else { ++fails; retry_at = tnow + 3000;
                                   logf("FP: the game could not make the %s here (its constructor returned 0; attempt %d, retry in 3 s)", ovn[pend_kind], fails); }
                        } else if (tnow - pend_t > 3000) {
                            uint32_t al = rd32(ram, CALL2_ALIVE);
                            logf("FP: the game did not run our call within 3 s (call routine ran %u times meanwhile) -- dropped", al - alive0);
                            pend_op = 0; retry_at = tnow + 5000;
                        }
                    }
                    if (!pend_op && cur != OV_NONE) {   // still there? (a new stage, or the game's own sight, ends it without us)
                        bool alive = cur == OV_BOX ? (sight_cur == 0xE2A9 && rd32(ram, CMB_BOXFLAG) == 1) : false;
                        if (cur == OV_MASK) for (int k = 0; k < n_gm; ++k) alive |= seen_gm[k] == actor;
                        if (cur == OV_NVG || cur == OV_THERM) for (int k = 0; k < n_gg; ++k) alive |= seen_gg[k] == actor;
                        if (!alive) { logf("FP: %s ended by the game", ovn[cur]); cur = OV_NONE; actor = 0; if (want != OV_NONE) retry_at = tnow + 1000; }
                    }
                    auto call = [&](uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, int kind) {
                        uint32_t sq = std::max(rd32(ram, CMB), rd32(ram, CMB + 4)) + 1;
                        uint32_t args[6] = {fn, a0, a1, a2, a3, a4};
                        wr(ram, CMB + 8, args, sizeof args);
                        wr32(ram, CMB, sq);                                  // the request number last
                        pend_seq = sq; pend_op = 1; pend_kind = kind; pend_t = tnow; alive0 = rd32(ram, CALL2_ALIVE);
                    };
                    if (!pend_op && cur != OV_NONE && cur != want) {   // take it down
                        if (cur == OV_BOX) { wr32(ram, CMB_BOXFLAG, 0); logf("FP: %s off", ovn[cur]); cur = OV_NONE; }
                        else call(A->destroy_other, actor, 0, 0, 0, 0, OV_NONE);
                    } else if (!pend_op && cur == OV_NONE && want != OV_NONE && tnow >= retry_at && fails < 5) {
                        if (sight_cur != 0xFFFFFFFFu) {
                            static double busy_log = 0;
                            if (tnow - busy_log > 10000) { busy_log = tnow; logf("FP: waiting -- the game already shows a sight (id %04X)", sight_cur & 0xFFFF); }
                        } else if (want == OV_BOX) { wr32(ram, CMB_BOXFLAG, 1); call(A->new_sight, 0xE2A9, 0xE2A9, CMB_BOXFLAG, 1, 0, OV_BOX); }
                        else if (want == OV_MASK) call(A->new_gasmask_sight, 0, 0, 0, 0, 0, OV_MASK);
                        else call(A->new_goggle_sight, want == OV_NVG ? 5 : 6, 0, 0, 0, 0, want);
                    }
                    if (want == OV_NONE) fails = 0;
                }
                {   // THROW: a new tenage.c actor = something was thrown; say whether it left from your hand
                    static int last_ten = 0;
                    if (tenage_now > last_ten) {
                        int32_t ht[3] = {0, 0, 0}; rd(ram, THROWMTX + 0x14, ht, sizeof ht);
                        bool ours = g_throw_work.load() != 0;
                        g_throw_spawned++;
                        logf("THROW: %s thrown %s (hand matrix t = %d,%d,%d)", ga.kind == 3 && g_weapon_id.load() >= 0 && g_weapon_id.load() < 10 ?
                             kWeaponName[g_weapon_id.load()] : "something", ours ? "from your hand" : "from Snake's hand (not in your hand right now)",
                             ht[0], ht[1], ht[2]);
                    }
                    last_ten = tenage_now;
                }
                {   // wrist HUD: LIFE, equipped weapon / item and counts from the linkvar block; the open list from MenuWork
                    HudData hd; int16_t lv[64] = {0};
                    if (rd(ram, T->stage_name + 0x10, lv, sizeof lv) && lv[12] > 0 && lv[12] < 4000) {
                        hd.valid = true; hd.vit = lv[11]; hd.vmax = lv[12]; hd.weapon = lv[14]; hd.item = lv[15];
                        if (hd.weapon >= 0 && hd.weapon < 10) { hd.wammo = lv[17 + hd.weapon]; hd.wmax = lv[27 + hd.weapon]; }
                        if (hd.item >= 0 && hd.item < 24) hd.icount = lv[37 + hd.item];
                        for (int k = 0; k < 10; ++k) hd.wown[k] = lv[17 + k];
                        {   // v12: does the game draw its radar? (menu/radar.c draw_radar)
                            uint16_t cfg = (uint16_t)lv[2]; uint32_t ps = rd32(ram, T->player_status);
                            int32_t am = 0; if (strstr(T->name, "US 1.0")) am = (int32_t)rd32(ram, 0x800AE178);
                            bool alert = am >= 1 && am <= 3, intrude = (ps & 2) != 0, cfg_off = (cfg & 0x800) != 0;
                            hd.radar_on = alert || !(intrude || cfg_off);
                            static int last = -1; int code = hd.radar_on ? 0 : cfg_off ? 1 : 2;
                            if (code != last) {
                                last = code;
                                logf("HUD: the game's radar is %s", code == 0 ? (alert ? "on (alert)" : "on") : code == 1 ?
                                     "OFF in this game's settings (GM_Configuration radar-off: the new-game menu sets it for Hard / Extreme) -- no radar on the wrist; it still shows during alerts" :
                                     "off while Snake is in a duct (intrusion) -- back when he is out");
                            }
                        }
                        for (int k = 0; k < 24; ++k) hd.iown[k] = lv[37 + k];
                        // GM_O2 (decomp game/gamed.c .sbss, US 0x800AE1AC / PAL 0x800ACA8C; 1024 = full). Trusted only once
                        // it has read exactly 1024, so a wrong address can never put a stray O2 bar on the wrist.
                        static bool o2_seen_full = false; static uint32_t o2_logged = 0;
                        int16_t o2v = 1024; rd(ram, T->o2, &o2v, 2);
                        if (o2v == 1024) o2_seen_full = true;
                        hd.o2 = o2v; hd.o2_ok = o2_seen_full && o2v >= 0 && o2v <= 1024;
                        if (o2_logged != T->o2) { logf("HUD: O2 reads %d at %08X (%s)", o2v, T->o2, o2v == 1024 ? "full -- trusted" : "waiting to see 1024"); o2_logged = T->o2; }
                    }
                    if (menu_work) {   // MenuWork: state +0x2A (1 weapon list, 2 item list); Menu_Inventory item +0x1DC, weapon +0x1F0
                        uint8_t st = 0; rd(ram, menu_work + 0x2A, &st, 1);
                        int side = (st & 2) ? 0 : (st & 1) ? 1 : -1;
                        if (side >= 0) {
                            uint32_t al = k0(rd32(ram, menu_work + (side ? 0x1F0 : 0x1DC) + 0xC));
                            int32_t n = al ? (int32_t)rd32(ram, al) : 0, sel = al ? (int32_t)rd32(ram, al + 4) : 0;
                            if (al && n > 0 && n <= 24 && sel >= 0 && sel < n) {
                                hd.menu = side ? 1 : 2; hd.ln = n; hd.lsel = sel;
                                for (int k = 0; k < n; ++k) { int16_t e[2] = {0, 0}; rd(ram, al + 0x20 + k * 8, e, 4); hd.lid[k] = e[0]; hd.lnum[k] = e[1]; }
                            }
                        }
                    }
                    hd.pl = g_pl_now.load();
                    if (g_cfg.boss_bars && g_v13_bars.load()) {   // v13: LIFE bars the game drew in the last 0.5 s (not Snake's own)
                        static uint32_t last_seq[8] = {0}; static ULONGLONG seen_at[8] = {0}; static char logged_names[8][20] = {};
                        uint8_t tb[8 * 0x1C]; uint32_t gs = 0; rd(ram, T->game_status, &gs, 4);
                        if (rd(ram, BARTAB + 0x10, tb, sizeof tb) && !(gs & 0x80020400u)) {
                            for (int k = 0; k < 8 && hd.nob < 4; ++k) {
                                uint32_t e[7]; memcpy(e, tb + k * 0x1C, sizeof e);
                                if (!e[0]) continue;
                                if (e[4] != last_seq[k]) { last_seq[k] = e[4]; seen_at[k] = now; }
                                if (now - seen_at[k] > 500) continue;
                                uint32_t conf = k0x(e[0] | 0x80000000u), np = conf ? k0x(rd32(ram, conf)) : 0;
                                char nm[20] = {0}; if (np) rd(ram, np, nm, 19);
                                for (char& c : nm) if (c && (c < 32 || c > 126)) c = '?';
                                if (!strcmp(nm, "LIFE") || !strcmp(nm, "O2")) continue;
                                auto& o = hd.ob[hd.nob++];
                                memcpy(o.name, nm[0] ? nm : "LIFE", 20); o.now = (int)e[2]; o.max = (int)e[3] > 0 ? (int)e[3] : 1024;
                                o.x = (int)e[5]; o.y = (int)e[6]; rd(ram, conf + 7, o.rgb, 3);   // the bar's right-hand (bright) colour
                                if (strncmp(logged_names[k], o.name, 20)) { memcpy(logged_names[k], o.name, 20);
                                    logf("BARS: \"%s\" LIFE %d / %d on the left wrist (drawn by the game at %d,%d)", o.name, o.now, o.max, o.x, o.y); }
                            }
                        }
                    }
                    AcquireSRWLockExclusive(&g_hud_lock); g_hud = hd; ReleaseSRWLockExclusive(&g_hud_lock);
                }
                // act wrapper: only while we hide Snake's body from the camera
                uint32_t bh = g_body_hidden.load();
                uint32_t amb[3] = {bh ? box_act : 0, bh ? weap_act : 0, bh};
                if (g_act_hook.load()) {
                    wr(ram, ACTMB, amb, sizeof amb);
                    static uint32_t lb = 0xFFFFFFFF, lw2 = 0xFFFFFFFF;
                    if (amb[0] != lb) { if (amb[0]) logf("BOX: in the cardboard box -> its first-person view (box act %08X)", amb[0]); lb = amb[0]; }
                    if (amb[1] != lw2) { if (amb[1]) logf("WEAPON: act %08X runs with Snake's body shown, so it still draws while his body is hidden", amb[1]); lw2 = amb[1]; }
                    uint32_t cnt[3]; rd(ram, ACTMB + 0x10, cnt, sizeof cnt); g_act_runs_box = cnt[1]; g_act_runs_weap = cnt[2];
                    static uint32_t last_calls = 0; static ULONGLONG moved_at = 0; static bool was_live = false;
                    if (cnt[0] != last_calls) { last_calls = cnt[0]; moved_at = now; }
                    g_act_calls = cnt[0];
                    static uint32_t last_w = 0; static ULONGLONG wmoved_at = 0;
                    if (cnt[2] != last_w) { last_w = cnt[2]; wmoved_at = now; }
                    if (!amb[1]) wmoved_at = now;            // no weapon equipped: nothing to check
                    static ULONGLONG weap_since = 0; if (!amb[1]) weap_since = 0; else if (!weap_since) weap_since = now;
                    bool weap_ok = !amb[1] || now - weap_since < 1500 || now - wmoved_at < 1500;
                    if (!weap_ok) { static ULONGLONG lw3 = 0; if (now - lw3 > 5000) { logf("HOOK act: weapon act %08X never reached the wrapper (%u weapon runs) -> body stays visible", amb[1], cnt[2]); lw3 = now; } }
                    bool live = moved_at && now - moved_at < 1500 && weap_ok;
                    if (live != was_live) {
                        uint32_t site_w = rd32(ram, T == &T_US10 ? 0x80015138 : 0x80014E14), c0 = rd32(ram, ACTCAVE);
                        logf(live ? "HOOK act RUNNING: the emulator calls the act wrapper (%u calls; site %08X, wrapper %08X)"
                                  : "HOOK act NOT RUNNING (%u calls; site %08X, wrapper %08X): Snake's body stays visible so weapons still show",
                             cnt[0], site_w, c0);
                        was_live = live;
                    }
                    g_act_live = live;
                }
                if (ga.work != logged_work) {
                    if (ga.work) logf("MOTION: %s equipped (actor %08X, model %08X, Snake's hand joint %08X%s) -> %s",
                                      ga.kind == 1 ? "SOCOM" : ga.kind == 2 ? "FAMAS" : "grenade / stun / chaff", ga.work, ga.objs, ga.orig, ga.laser ? ", laser sight" : "",
                                      g_cfg.gun_in_hand ? "in your right hand while walking in first person" : "gun in hand is off");
                    else if (logged_work != 0xFFFFFFFF) logf("MOTION: no SOCOM / FAMAS / grenade equipped");
                    logged_work = ga.work;
                }
            }
        }
        // routine mode: 1 = head-look (game first person, cutscenes), 2 = walking through Snake's eyes, 0 = off
        if (h.installed) {
            char st0 = 0; rd(ram, T->stage_name, &st0, 1);
            // pause bits: 1 STOP (codec, movies, game over), 2 PAUSE (pause screen), 4 MENU (item/weapon menus), 8 read error
            g_pl_now = pl;
            bool grip_list = (pl & 4) && g_cfg.hud_wrist && g_grip_mask.load() != 0;   // a grip opened it: the list is on that wrist
            bool to_screen = ((pl & 1) && g_cfg.screen_codec) || ((pl & 2) && g_cfg.screen_pause) || ((pl & 4) && g_cfg.screen_menus && !grip_list) || (pl & 8);
            g_keep_pose = pl != 0 && !to_screen;
            bool gameplay = !demo && !to_screen && st0 && st0 != 't';
            static int last_pl_log = -1;
            if (pl != last_pl_log) {
                if (pl) logf("SCREEN: game paused (%s%s%s) -> %s", (pl & 1) ? "codec/movie " : "", (pl & 2) ? "pause " : "", (pl & 4) ? "menu " : "",
                             to_screen ? "floating screen" : "VR view kept");
                last_pl_log = pl;
            }
            int want = 0;
            if (g_view_mode.load() < 0) g_view_mode = g_cfg.v15.diorama ? 1 : 0;   // v17: start as the ini says
            if (h.runtime_on && g_head_valid.load() && g_view_mode.load() != 2) {   // v17: flat screen = the game untouched
                bool sp_on = g_sp_objs.load() && gameplay && g_cfg.v15.setpiece_eyes && g_cfg.walk_mode && walk_on;
                if (demo) want = cut_pov ? 2 : g_cfg.cutscene_head_look ? 1 : 0;
                else if (sp_on) want = 2;                                 // v15: vehicles, rope, rack: Snake's eyes
                else if (fp == 1) {   // v14: a PSG1 / Stinger sight stays in Snake's eyes (mode 2), so your head aims it
                    int16_t wid = -1; rd(ram, T->stage_name + 0x10 + 14 * 2, &wid, 2);
                    want = (gameplay && g_cfg.walk_mode && walk_on && g_cfg.aim_by_head && (wid == 9 || wid == 4)) ? 2 : 1;
                }
                else if (gameplay && g_view_mode.load() == 1) want = 3;     // v15: the level as a miniature on a table (v17: R3 x2 too)
                else if (gameplay && g_cfg.walk_mode && walk_on) want = 2;
                else if (g_cfg.track_third_person) want = 1;
                g_setpiece = !demo && sp_on && want == 2;
                if (g_setpiece.load()) {   // the view's heading follows the body (vehicle turns), your head on top
                    static ULONGLONG last_sp_yaw = 0; static int last_units = -1;
                    if (now - last_sp_yaw >= 100) {
                        last_sp_yaw = now; int16_t m[9] = {0}; uint32_t so = g_sp_objs.load();
                        if (rd(ram, so + 0x48, m, sizeof m) && (m[2] || m[8])) {
                            int units = ((int)lround(atan2((double)m[2], (double)m[8]) * 4096.0 / (2 * 3.14159265358979)) % 4096 + 4096) % 4096;
                            if (units != last_units) { g_yaw_ref_units = units; g_yaw_ref_new = true; last_units = units; }
                        }
                    }
                }
            }
            if (want != h.enable_word) {
                if (want == 3) {
                    int16_t facing = 0; rd(ram, T->snakecam + 0x2A, &facing, 2);
                    g_dio_yaw_units = facing & 0xFFF; g_dio_reset = true; g_recentre = true;
                    logf("MODE 3 (diorama): the level as a 1:%d miniature on a table, %d cm in front of you, looking along Snake's facing %.1f deg",
                         g_cfg.v15.dio_scale, g_cfg.v15.dio_dist_cm, (facing & 0xFFF) * 360.0 / 4096);
                }
                else if (want == 2 && g_setpiece.load()) {
                    g_recentre = true;
                    logf("MODE 2 (set piece: %s): Snake's eyes, heading follows the body", kSpName[(g_sp_kind.load() + 7) % 7]);
                }
                else if (want == 2 && cut_pov) logf("MODE 2 (cutscene): Snake's eyes");
                else if (want == 2 && h.enable_word == 1 && fp == 1) logf("MODE 2 (sight): PSG1 / Stinger aimed with your head, through Snake's eyes");
                else if (want == 2) {
                    int16_t facing = 0; rd(ram, T->snakecam + 0x2A, &facing, 2);
                    g_yaw_ref_units = facing & 0xFFF; g_yaw_ref_new = true; g_recentre = true;
                    logf("MODE 2 (walk): Snake's eyes, starting along Snake's facing %d (%.1f deg)", facing & 0xFFF, (facing & 0xFFF) * 360.0 / 4096);
                } else logf("MODE %d (%s)", want, want == 1 ? "head-look" : "off");
                g_mode_want = want; h.enable_word = want;
            }
        }

        // did the emulator run our code? (only judged once the game shows a stage)
        if (h.installed && !h.reported_running) {
            uint32_t calls = rd32(ram, MBOX + MB_CALLS);
            char st[2] = {0}; rd(ram, T->stage_name, st, 1);
            if (calls > 0) {
                h.reported_running = true;
                logf("HOOK RUNNING: the emulator executed our routine (%u calls, %llu ms after install)",
                     calls, (unsigned long long)(now - h.installed_at));
            } else if (!h.reported_not_running && st[0] && now - h.installed_at > 6000 && rd32(ram, T->gv_time) - h.gvt_at_install > 120) {
                h.reported_not_running = true;
                logf("HOOK NOT RUNNING yet: a stage is loaded but the routine has not been called (word at %08X reads %08X)",
                     T->hook_site, rd32(ram, T->hook_site));
            }
        }

        { char st[2] = {0}; rd(ram, T->stage_name, st, 1);
          g_scan_ok = h.installed && st[0] && st[0] != 't' && pl == 0; }   // a stage, not 'title', not paused
        // read-only watch of spare RAM blocks (candidate space for larger routines): log the first time any is used
        static ULONGLONG last_watch = 0;
        if (now - last_watch >= 5000) {
            static const uint32_t blocks[][2] = { {0x80007D00, 0x80008530}, {0x8000A380, 0x8000A400}, {0x8000B060, 0x8000B070},
                                                  {0x8000B6B0, 0x8000B870}, {0x8000FAA0, 0x80010000} };   // v13: minus what v13 uses
            static bool used[5] = {false};
            static std::vector<uint8_t> b(0x1000);
            char stage[12] = {0}; rd(ram, T->stage_name, stage, 8);
            for (int i = 0; i < 5; ++i) {
                uint32_t n = blocks[i][1] - blocks[i][0]; b.resize(n);
                if (used[i] || !rd(ram, blocks[i][0], b.data(), n)) continue;
                for (uint32_t k = 0; k < n; ++k) if (b[k]) {
                    used[i] = true;
                    logf("SPARE RAM: block %08X-%08X became non-zero at %08X (stage '%s') -- not safe for code", blocks[i][0], blocks[i][1], blocks[i][0] + k, stage);
                    break;
                }
            }
            static char last_stage[12] = {0};
            if (strncmp(stage, last_stage, 8)) { memcpy(last_stage, stage, 12); bool all = true; for (bool u : used) all = all && !u;
                logf("SPARE RAM: stage '%s' -- %s", stage, all ? "all 5 blocks still empty" : "some blocks used (see above)"); }
            last_watch = now;
        }
        if (now - last_status >= 3000 && h.installed) { log_status(ram, *T, h, "STATUS"); last_status = now; }
        Sleep(20);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hi, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hi);
        InitializeCriticalSection(&g_logcs);
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        g_dir = exe;
        g_dir = g_dir.substr(0, g_dir.find_last_of(L"\\/") + 1);
        g_log = _wfopen((g_dir + L"mgs1mc_vr.log").c_str(), L"a");
        logf("==== mgs1mc_vr panel v1 loaded into %ls (motion v25: X + Y together = Start (SteamVR keeps the left menu button); motion v24: the game's focus (depth-of-field) effect off in VR views too, actor names logged; motion v23: the game's afterimage blur off in VR views; motion v22: codec at your ear (left grip), hands survive the Master Collection menu; motion v21: GOG button layout (A Circle, B Triangle, LT Cross), A / B = Enter / Escape in menus, E + M through both routes; motion v20: TRUE STEREO probe -- both eyes every frame, side by side ([stereo] true_stereo); motion v19: mono only in Snake's-eyes cutscenes, mono while the hands move, carousel models flat per eye; motion v18: cutscenes mono, subtitle strip black + panel edges, mono while choosing, whole 16:9 Master Collection menus; motion v17: R3 double-click VR / diorama / flat, L3+R3 Master Collection menu, game descriptions on the carousel, cutscene subtitle panel; motion v16: VR settings on held L3, GOG 3D carousel (default) + wheel pose, pause while choosing; motion v15: paged options menu with controls, diorama mode, set pieces in Snake's eyes, hologram selection wheel, third-person lean; motion v14: hands drawn from the renderer's own matrices, PSG1 / Stinger stay in Snake's eyes and follow your head; motion v13: floating hands + wall knock, boss LIFE bars on the wrist, item use from the wrist (A), PSG1 lens + head aim for PSG1 / Stinger; motion v12: wrist radar follows the game's radar; motion v11: first-person box / gas mask / goggle views made by the game, grenade follows the hand again, F10 wrist dump; motion v10: lean (6DoF), grenade / stun / chaff in hand and thrown from it, GOG wrist panels + quick-select on the grips, game HUD hidden in the view; motion v9: body hidden by render group, box model out of view, wrist HUD from game memory + radar; motion v8: floating screen follows you if it is behind you; motion v7: act wrapper liveness check; motion v6: body hidden in first person, box view, grenade in hand, grip lists on the wrist; gun in hand, punch / chokehold, rumble; perf_counters off; gun in hand, wrist panels redrawn per game picture, LAYERS log; floating screen options, Integral wrist panels; Snake's-eyes cutscenes; stereo v3, body follows head, snap turning, settings panel: hold left menu; F5 stereo, F6 recentre, F7 walk, F8 tracking, F9 snapshot, F10 save picture, F11 headset test colour)", exe);
        CreateThread(nullptr, 0, main_thread, nullptr, 0, nullptr);
    }
    return TRUE;
}
