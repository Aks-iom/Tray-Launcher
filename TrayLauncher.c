// TrayLauncher - минимальный нативный Win32 трей-лаунчер (без .NET, без CRT)
//
// Экономия ресурсов:
//  * нет окон, GDI+, WinForms, .NET - только Win32 (~1-2 МБ памяти, десятки КБ на диске)
//  * GetMessage блокирует поток -> 0% CPU в простое
//  * пока приложение запущено - опроса НЕТ: ждём завершение процесса через
//    RegisterWaitForSingleObject (ядро само разбудит нас)
//  * пока не запущено - лёгкий опрос раз в POLL_MS (снимок процессов ~1 мс)
//  * launch.txt перечитывается только если изменилось время его модификации
//  * иконки рисуются один раз при старте, после инициализации рабочий набор обрезается
//
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#define POLL_MS        2000   // период опроса, когда приложение НЕ запущено
#define FAST_MS        500    // быстрый опрос сразу после ручного запуска
#define WM_TRAY        (WM_APP + 0)
#define WM_PROC_EXIT   (WM_APP + 1)
#define ID_TIMER       1
#define ID_START       1001
#define ID_STOP        1002
#define ID_AUTO        1003
#define ID_EDIT        1004
#define ID_EXIT        1005

#define APP_NAME       L"TrayLauncher"
#define RUN_KEY        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define SET_KEY        L"Software\\TrayLauncher"

enum { ST_ERR = 0, ST_OFF = 1, ST_ON = 2 };

// ---- всё в статике, чтобы не плодить memset/стек ----
static HWND      g_hwnd;
static HICON     g_icons[3];
static UINT      g_msgTaskbar;
static NOTIFYICONDATAW g_nid;
static WNDCLASSEXW     g_wc;
static MSG       g_msg;
static PROCESSENTRY32W g_pe;

static WCHAR     g_exeDir[MAX_PATH];
static WCHAR     g_exePath[MAX_PATH];
static WCHAR     g_cfgPath[MAX_PATH];
static WCHAR     g_target[MAX_PATH];     // что запускаем
static WCHAR     g_procExe[MAX_PATH];    // имя процесса с .exe
static WCHAR     g_tip[128];
static BOOL      g_cfgOk;
static FILETIME  g_cfgTime;

static int       g_state = -1;
static HANDLE    g_hProc;                // дескриптор наблюдаемого процесса
static HANDLE    g_hWait;                // регистрация ожидания
static DWORD     g_watchPid;
static UINT      g_timerMs;
static int       g_fastTicks;

static BYTE      g_buf[8192];
static WCHAR     g_wbuf[4096];

// ------------------------------------------------------------
static void TrimWS(void) { SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1); }

static void SetPoll(UINT ms)
{
    if (g_timerMs == ms) return;
    if (ms) SetTimer(g_hwnd, ID_TIMER, ms, NULL); else KillTimer(g_hwnd, ID_TIMER);
    g_timerMs = ms;
}

// ---------- иконки: круг с антиалиасингом, считаем пиксели сами ----------
static HICON MakeIcon(int sz, BYTE r, BYTE g, BYTE b)
{
    static DWORD px[48 * 48];
    static BYTE  mask[48 * 6];
    if (sz > 48) sz = 48;
    float c = sz * 0.5f, rad = sz * 0.5f - 1.0f;
    for (int y = 0; y < sz; y++)
        for (int x = 0; x < sz; x++) {
            float dx = x + 0.5f - c, dy = y + 0.5f - c;
            float d2 = dx * dx + dy * dy;
            float s = __builtin_sqrtf(d2);
            float a = rad + 0.5f - s;                 // покрытие у края
            if (a < 0) a = 0;
            if (a > 1) a = 1;
            float edge = rad - s;                     // тёмная окантовка
            DWORD A = (DWORD)(a * 255), R = r, G = g, B = b;
            if (edge < 1.2f && a > 0) { R = R * 6 / 10; G = G * 6 / 10; B = B * 6 / 10; }
            // premultiplied не нужен: иконки используют straight alpha
            px[y * sz + x] = (A << 24) | (R << 16) | (G << 8) | B;
        }
    for (int i = 0; i < 48 * 6; i++) mask[i] = 0;
    HBITMAP hc = CreateBitmap(sz, sz, 1, 32, px);
    HBITMAP hm = CreateBitmap(sz, sz, 1, 1, mask);
    ICONINFO ii; ii.fIcon = TRUE; ii.xHotspot = 0; ii.yHotspot = 0; ii.hbmMask = hm; ii.hbmColor = hc;
    HICON h = CreateIconIndirect(&ii);
    DeleteObject(hc); DeleteObject(hm);
    return h;
}

// ---------- конфиг ----------
static void WriteDefaultCfg(void)
{
    static const WCHAR txt[] =
        L"# Строка 1: путь к файлу/программе для запуска (можно относительный, можно %ПЕРЕМЕННЫЕ%)\r\n"
        L"# Строка 2 (необязательно): имя процесса для проверки, без .exe\r\n"
        L"# Строки, начинающиеся с #, игнорируются\r\n"
        L"C:\\Windows\\System32\\notepad.exe\r\n";
    static BYTE out[2048];
    int n = WideCharToMultiByte(CP_UTF8, 0, txt, -1, (LPSTR)out + 3, sizeof(out) - 3, NULL, NULL);
    out[0] = 0xEF; out[1] = 0xBB; out[2] = 0xBF;
    HANDLE h = CreateFileW(g_cfgPath, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w; WriteFile(h, out, (n > 0 ? n - 1 : 0) + 3, &w, NULL); CloseHandle(h);
    }
}

// Читает launch.txt, если он изменился. Возвращает TRUE если конфиг перечитан.
static BOOL LoadCfg(BOOL force)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(g_cfgPath, GetFileExInfoStandard, &fa)) {
        WriteDefaultCfg();
        if (!GetFileAttributesExW(g_cfgPath, GetFileExInfoStandard, &fa)) { g_cfgOk = FALSE; return TRUE; }
        force = TRUE;
    }
    if (!force && CompareFileTime(&fa.ftLastWriteTime, &g_cfgTime) == 0) return FALSE;
    g_cfgTime = fa.ftLastWriteTime;
    g_cfgOk = FALSE;

    HANDLE h = CreateFileW(g_cfgPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { g_cfgTime.dwLowDateTime = 0; g_cfgTime.dwHighDateTime = 0; return TRUE; }
    DWORD n = 0;
    ReadFile(h, g_buf, sizeof(g_buf) - 2, &n, NULL);
    CloseHandle(h);
    g_buf[n] = 0; g_buf[n + 1] = 0;

    int wl;
    if (n >= 2 && g_buf[0] == 0xFF && g_buf[1] == 0xFE) {          // UTF-16 LE
        wl = (int)((n - 2) / 2);
        if (wl > 4095) wl = 4095;
        for (int i = 0; i < wl; i++) g_wbuf[i] = ((WCHAR *)(g_buf + 2))[i];
    } else {                                                        // UTF-8 (с BOM или без)
        int off = (n >= 3 && g_buf[0] == 0xEF && g_buf[1] == 0xBB && g_buf[2] == 0xBF) ? 3 : 0;
        wl = MultiByteToWideChar(CP_UTF8, 0, (LPCSTR)g_buf + off, (int)n - off, g_wbuf, 4095);
        if (wl < 0) wl = 0;
    }
    g_wbuf[wl] = 0;

    WCHAR *lines[2] = { NULL, NULL };
    int cnt = 0;
    WCHAR *p = g_wbuf;
    while (*p && cnt < 2) {
        WCHAR *s = p;
        while (*p && *p != L'\n') p++;
        if (*p) *p++ = 0;
        while (*s == L' ' || *s == L'\t' || *s == L'"') s++;
        WCHAR *e = s; while (*e) e++;
        while (e > s && (e[-1] == L'\r' || e[-1] == L' ' || e[-1] == L'\t' || e[-1] == L'"')) *--e = 0;
        if (*s == 0 || *s == L'#') continue;
        lines[cnt++] = s;
    }
    if (cnt == 0) return TRUE;

    static WCHAR tmp[MAX_PATH];
    ExpandEnvironmentStringsW(lines[0], tmp, MAX_PATH);
    BOOL abs = (tmp[0] && tmp[1] == L':') || (tmp[0] == L'\\' && tmp[1] == L'\\');
    if (abs) lstrcpynW(g_target, tmp, MAX_PATH);
    else { lstrcpynW(g_target, g_exeDir, MAX_PATH); lstrcatW(g_target, tmp); }

    // имя процесса
    if (cnt > 1) lstrcpynW(tmp, lines[1], MAX_PATH);
    else {
        WCHAR *nm = g_target, *q;
        for (q = g_target; *q; q++) if (*q == L'\\' || *q == L'/') nm = q + 1;
        lstrcpynW(tmp, nm, MAX_PATH);
        for (q = tmp + lstrlenW(tmp); q > tmp; q--) if (*q == L'.') { *q = 0; break; }
    }
    int L = lstrlenW(tmp);
    if (L > 4 && (tmp[L - 4] == L'.') && (tmp[L - 3] | 32) == L'e' && (tmp[L - 2] | 32) == L'x' && (tmp[L - 1] | 32) == L'e')
        tmp[L - 4] = 0;
    lstrcpynW(g_procExe, tmp, MAX_PATH - 5);
    lstrcatW(g_procExe, L".exe");
    g_cfgOk = TRUE;
    return TRUE;
}

// ---------- процессы ----------
// Ищет процесс по имени. Если kill=TRUE - завершает все найденные.
static DWORD FindProc(BOOL kill)
{
    DWORD found = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    g_pe.dwSize = sizeof(g_pe);
    if (Process32FirstW(snap, &g_pe)) {
        do {
            if (lstrcmpiW(g_pe.szExeFile, g_procExe) == 0) {
                if (!found) found = g_pe.th32ProcessID;
                if (kill) {
                    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, g_pe.th32ProcessID);
                    if (h) { TerminateProcess(h, 0); CloseHandle(h); }
                } else break;
            }
        } while (Process32NextW(snap, &g_pe));
    }
    CloseHandle(snap);
    return found;
}

static VOID CALLBACK OnProcExit(PVOID ctx, BOOLEAN timedOut)
{
    PostMessageW(g_hwnd, WM_PROC_EXIT, 0, 0);   // вызывается из пула потоков ядром, не по таймеру
}

static void StopWatch(void)
{
    if (g_hWait) { UnregisterWait(g_hWait); g_hWait = NULL; }
    if (g_hProc) { CloseHandle(g_hProc); g_hProc = NULL; }
    g_watchPid = 0;
}

static void StartWatch(DWORD pid)
{
    if (g_watchPid == pid && g_hWait) return;
    StopWatch();
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return;                                   // нет доступа -> останемся на опросе
    if (!RegisterWaitForSingleObject(&g_hWait, h, OnProcExit, NULL, INFINITE,
                                     WT_EXECUTEONLYONCE | WT_EXECUTEINWAITTHREAD)) {
        CloseHandle(h); g_hWait = NULL; return;
    }
    g_hProc = h; g_watchPid = pid;
}

// ---------- трей ----------
static void SetTip(const WCHAR *t)
{
    lstrcpynW(g_tip, t, 128);
}

static void UpdateTray(int st, BOOL add)
{
    g_nid.hIcon = g_icons[st];
    lstrcpynW(g_nid.szTip, g_tip, 128);
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &g_nid);
}

static void Refresh(void)
{
    LoadCfg(FALSE);
    int st; DWORD pid = 0;
    if (!g_cfgOk || GetFileAttributesW(g_target) == INVALID_FILE_ATTRIBUTES) {
        st = ST_ERR;
        SetTip(L"Файл из launch.txt не найден");
        StopWatch();
    } else {
        pid = FindProc(FALSE);
        st = pid ? ST_ON : ST_OFF;
        WCHAR *nm = g_target, *q;
        for (q = g_target; *q; q++) if (*q == L'\\' || *q == L'/') nm = q + 1;
        lstrcpynW(g_tip, nm, 90);
        lstrcatW(g_tip, pid ? L" — запущено" : L" — не запущено");
    }

    if (pid) { StartWatch(pid); SetPoll(g_hWait ? 0 : POLL_MS); }   // запущено: опрос выключаем
    else     { StopWatch(); if (g_fastTicks > 0) { g_fastTicks--; SetPoll(FAST_MS); } else SetPoll(POLL_MS); }

    if (st != g_state) {
        BOOL first = (g_state < 0);
        g_state = st;
        UpdateTray(st, first);
        TrimWS();
    }
}

// ---------- автозапуск ----------
static BOOL IsAuto(void)
{
    HKEY k; BOOL r = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        r = RegQueryValueExW(k, APP_NAME, NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
        RegCloseKey(k);
    }
    return r;
}

static void SetAuto(BOOL on)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        static WCHAR v[MAX_PATH + 4];
        v[0] = L'"'; lstrcpynW(v + 1, g_exePath, MAX_PATH); lstrcatW(v, L"\"");
        RegSetValueExW(k, APP_NAME, 0, REG_SZ, (BYTE *)v, (lstrlenW(v) + 1) * sizeof(WCHAR));
    } else RegDeleteValueW(k, APP_NAME);
    RegCloseKey(k);
}

static BOOL AutoDisabledByUser(void)
{
    HKEY k; DWORD v = 0, sz = sizeof(v), t;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SET_KEY, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, L"AutostartDisabled", NULL, &t, (BYTE *)&v, &sz) != ERROR_SUCCESS) v = 0;
        RegCloseKey(k);
    }
    return v == 1;
}

static void RememberAutoChoice(BOOL enabled)
{
    HKEY k; DWORD v = enabled ? 0 : 1;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SET_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, L"AutostartDisabled", 0, REG_DWORD, (BYTE *)&v, sizeof(v));
        RegCloseKey(k);
    }
}

// ---------- действия ----------
static void DoStart(void)
{
    if (!g_cfgOk) return;
    static WCHAR dir[MAX_PATH];
    lstrcpynW(dir, g_target, MAX_PATH);
    WCHAR *last = NULL, *q;
    for (q = dir; *q; q++) if (*q == L'\\' || *q == L'/') last = q;
    if (last) *last = 0;
    ShellExecuteW(NULL, NULL, g_target, NULL, last ? dir : NULL, SW_SHOWNORMAL);
    g_fastTicks = 8;
    SetPoll(FAST_MS);
}

static void DoStop(void)
{
    if (!g_cfgOk) return;
    FindProc(TRUE);
    g_fastTicks = 4;
    SetPoll(FAST_MS);
}

static void ShowMenu(void)
{
    Refresh();
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (g_state == ST_OFF ? 0 : MF_GRAYED), ID_START, L"Запустить");
    AppendMenuW(m, MF_STRING | (g_state == ST_ON  ? 0 : MF_GRAYED), ID_STOP,  L"Остановить");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (IsAuto() ? MF_CHECKED : 0), ID_AUTO, L"Автозапуск с Windows");
    AppendMenuW(m, MF_STRING, ID_EDIT, L"Открыть launch.txt");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Выход");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);

    switch (cmd) {
    case ID_START: DoStart(); break;
    case ID_STOP:  DoStop();  break;
    case ID_AUTO: { BOOL en = !IsAuto(); SetAuto(en); RememberAutoChoice(en); } break;
    case ID_EDIT:  ShellExecuteW(NULL, NULL, g_cfgPath, NULL, g_exeDir, SW_SHOWNORMAL); break;
    case ID_EXIT:  DestroyWindow(g_hwnd); return;
    }
    TrimWS();
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == g_msgTaskbar && m != 0) {                 // explorer перезапущен -> вернуть иконку
        if (g_state >= 0) UpdateTray(g_state, TRUE);
        return 0;
    }
    switch (m) {
    case WM_TIMER:
        if (w == ID_TIMER) Refresh();
        return 0;
    case WM_PROC_EXIT:
        StopWatch();
        g_fastTicks = 2;
        Refresh();
        return 0;
    case WM_TRAY:
        if (l == WM_LBUTTONUP) {
            Refresh();
            if (g_state == ST_ON) DoStop(); else if (g_state == ST_OFF) DoStart();
        } else if (l == WM_RBUTTONUP) ShowMenu();
        return 0;
    case WM_DESTROY:
        StopWatch();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void entry(void)
{
    HANDLE mtx = CreateMutexW(NULL, TRUE, L"Local\\TrayLauncher_Single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) ExitProcess(0);

    SetProcessDPIAware();

    HINSTANCE hi = GetModuleHandleW(NULL);
    GetModuleFileNameW(NULL, g_exePath, MAX_PATH);
    lstrcpynW(g_exeDir, g_exePath, MAX_PATH);
    WCHAR *last = g_exeDir, *q;
    for (q = g_exeDir; *q; q++) if (*q == L'\\') last = q;
    last[1] = 0;
    lstrcpynW(g_cfgPath, g_exeDir, MAX_PATH);
    lstrcatW(g_cfgPath, L"launch.txt");

    g_wc.cbSize = sizeof(g_wc);
    g_wc.lpfnWndProc = WndProc;
    g_wc.hInstance = hi;
    g_wc.lpszClassName = L"TrayLauncherWnd";
    RegisterClassExW(&g_wc);
    // обычное (скрытое, не показываемое) окно, а не message-only: иначе не придёт TaskbarCreated
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, g_wc.lpszClassName, APP_NAME, WS_POPUP, 0, 0, 0, 0, NULL, NULL, hi, NULL);
    g_msgTaskbar = RegisterWindowMessageW(L"TaskbarCreated");

    int sz = GetSystemMetrics(SM_CXSMICON);
    g_icons[ST_ON]  = MakeIcon(sz,  46, 204,  64);   // зелёный
    g_icons[ST_OFF] = MakeIcon(sz, 220,  50,  47);   // красный
    g_icons[ST_ERR] = MakeIcon(sz, 140, 140, 140);   // серый

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uCallbackMessage = WM_TRAY;

    if (!AutoDisabledByUser()) SetAuto(TRUE);       // автозапуск включаем сами при первом старте

    LoadCfg(TRUE);
    Refresh();
    TrimWS();

    while (GetMessageW(&g_msg, NULL, 0, 0) > 0) {   // блокируется -> 0% CPU
        TranslateMessage(&g_msg);
        DispatchMessageW(&g_msg);
    }
    ReleaseMutex(mtx);
    ExitProcess(0);
}
