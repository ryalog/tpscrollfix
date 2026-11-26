// tpscrollfix: ThinkPad-style middle button for the X13s Elan TrackPoint.
//
// Requires ETD's middle button in "real middle button" mode:
//   HKCU\Software\Elantech\SmartPad\n3A2PST_Middle_Mode = 1
//
// TrackPoint middle button:
//   press + release, no movement  -> middle click
//   press + move stick            -> scroll (cursor stays put), no click
// Middle buttons on other mice (USB, Bluetooth) pass through untouched.
//
// Usage: tpscrollfix.exe [speed] [threshold]
//   speed      wheel units per pixel of stick motion (default 3; 120 = one notch)
//   threshold  pixels of motion before a press becomes a scroll (default 4)
//
// How the device is identified: the low-level mouse hook can't tell which
// device an event came from, so Raw Input runs alongside it. When a middle
// press arrives, the hook holds it until Raw Input reports which device sent
// it. Anything whose device path lacks TRACKPOINT_ID is treated as another
// mouse and its press is replayed immediately.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdlib.h>
#include <wchar.h>

#define TRACKPOINT_ID L"ELAN06F0"
#define TAG ((ULONG_PTR)0x54504D42u)   /* marks our own injected input */
#define DECIDE_TIMER 1
#define DECIDE_TIMEOUT_MS 60

enum {
    IDLE,
    DECIDING,     /* press held; waiting for Raw Input to name the device */
    PENDING,      /* TrackPoint press; not moved yet */
    SCROLLING,    /* TrackPoint press; moving = scrolling */
    PASSTHRU      /* another mouse's press; everything passes through */
};

static HHOOK g_hook;
static HWND g_wnd;
static int g_state = IDLE;
static BOOL g_up_while_deciding;
static POINT g_anchor;
static int g_travel;
static double g_acc_x, g_acc_y;
static double g_speed = 3.0;
static int g_threshold = 4;

/* Device classification cache (handles are stable while the device exists). */
#define CACHE_N 16
static struct { HANDLE dev; BOOL tp; } g_cache[CACHE_N];
static int g_cache_next;

static BOOL is_trackpoint(HANDLE dev)
{
    if (!dev)            /* injected by a driver/service: treat as TrackPoint */
        return TRUE;
    for (int i = 0; i < CACHE_N; i++)
        if (g_cache[i].dev == dev)
            return g_cache[i].tp;

    WCHAR name[512];
    UINT len = ARRAYSIZE(name);
    BOOL tp = TRUE;
    if (GetRawInputDeviceInfoW(dev, RIDI_DEVICENAME, name, &len) != (UINT)-1) {
        _wcsupr(name);
        tp = wcsstr(name, TRACKPOINT_ID) != NULL;
    }
    g_cache[g_cache_next].dev = dev;
    g_cache[g_cache_next].tp = tp;
    g_cache_next = (g_cache_next + 1) % CACHE_N;
    return tp;
}

static void send_mouse(DWORD flags, DWORD data)
{
    INPUT in = {0};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = data;
    in.mi.dwExtraInfo = TAG;
    SendInput(1, &in, sizeof in);
}

/* Called once the device behind a held press is known (or we gave up). */
static void decide(BOOL trackpoint)
{
    KillTimer(g_wnd, DECIDE_TIMER);
    if (g_state != DECIDING)
        return;

    if (trackpoint) {
        if (g_up_while_deciding) {          /* very fast tap */
            g_state = IDLE;
            send_mouse(MOUSEEVENTF_MIDDLEDOWN, 0);
            send_mouse(MOUSEEVENTF_MIDDLEUP, 0);
        } else {
            g_state = PENDING;
        }
    } else {
        send_mouse(MOUSEEVENTF_MIDDLEDOWN, 0);  /* replay the held press */
        if (g_up_while_deciding) {
            send_mouse(MOUSEEVENTF_MIDDLEUP, 0);
            g_state = IDLE;
        } else {
            g_state = PASSTHRU;
        }
    }
}

static LRESULT CALLBACK mouse_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code != HC_ACTION)
        return CallNextHookEx(g_hook, code, wp, lp);

    const MSLLHOOKSTRUCT *m = (const MSLLHOOKSTRUCT *)lp;
    if (m->dwExtraInfo == TAG)
        return CallNextHookEx(g_hook, code, wp, lp);

    switch (wp) {
    case WM_MBUTTONDOWN:
        g_anchor = m->pt;
        g_travel = 0;
        g_acc_x = g_acc_y = 0;
        g_up_while_deciding = FALSE;
        g_state = DECIDING;
        SetTimer(g_wnd, DECIDE_TIMER, DECIDE_TIMEOUT_MS, NULL);
        return 1;

    case WM_MOUSEMOVE:
        if (g_state == IDLE || g_state == PASSTHRU)
            break;
        {
            int dx = m->pt.x - g_anchor.x;
            int dy = m->pt.y - g_anchor.y;
            if (g_state == DECIDING || g_state == PENDING) {
                g_travel += abs(dx) + abs(dy);
                if (g_state == DECIDING || g_travel < g_threshold)
                    return 1;
                g_state = SCROLLING;
            }
            g_acc_y += -dy * g_speed;        /* stick down = scroll down */
            g_acc_x += dx * g_speed;
            int wy = (int)g_acc_y, wx = (int)g_acc_x;
            g_acc_y -= wy;
            g_acc_x -= wx;
            if (wy) PostMessageW(g_wnd, WM_APP, MOUSEEVENTF_WHEEL, wy);
            if (wx) PostMessageW(g_wnd, WM_APP, MOUSEEVENTF_HWHEEL, wx);
        }
        return 1;

    case WM_MBUTTONUP:
        switch (g_state) {
        case DECIDING:
            g_up_while_deciding = TRUE;
            return 1;
        case PENDING:
            g_state = IDLE;
            PostMessageW(g_wnd, WM_APP + 1, 0, 0);   /* middle click */
            return 1;
        case SCROLLING:
            g_state = IDLE;
            return 1;
        case PASSTHRU:
            g_state = IDLE;
            break;
        }
        break;
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INPUT: {
        RAWINPUT ri;
        UINT size = sizeof ri;
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size,
                            sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE &&
            (ri.data.mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN) &&
            g_state == DECIDING)
            decide(is_trackpoint(ri.header.hDevice));
        break;
    }
    case WM_TIMER:
        if (wp == DECIDE_TIMER)
            decide(TRUE);   /* no Raw Input seen: assume TrackPoint */
        return 0;
    case WM_APP:            /* wheel */
        send_mouse((DWORD)wp, (DWORD)(int)lp);
        return 0;
    case WM_APP + 1:        /* click */
        send_mouse(MOUSEEVENTF_MIDDLEDOWN, 0);
        send_mouse(MOUSEEVENTF_MIDDLEUP, 0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev; (void)cmd; (void)show;

    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\tpscrollfix-single-instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

    int argc;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        if (argc > 1) { double s = _wtof(argv[1]); if (s > 0) g_speed = s; }
        if (argc > 2) { int t = _wtoi(argv[2]); if (t >= 0) g_threshold = t; }
        LocalFree(argv);
    }

    /* Hook coordinates are physical pixels; stay DPI-aware so nothing gets rescaled. */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"tpscrollfix";
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, L"tpscrollfix", 0, 0, 0, 0, 0,
                            HWND_MESSAGE, NULL, inst, NULL);

    RAWINPUTDEVICE rid = { 0x01, 0x02, RIDEV_INPUTSINK, g_wnd };  /* all mice */
    RegisterRawInputDevices(&rid, 1, sizeof rid);

    g_hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc, inst, 0);
    if (!g_wnd || !g_hook) {
        MessageBoxW(NULL, L"Could not install mouse hook.", L"tpscrollfix", MB_ICONERROR);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
        DispatchMessageW(&msg);

    UnhookWindowsHookEx(g_hook);
    if (mutex) CloseHandle(mutex);
    return 0;
}
