// main.cpp (集成视频壁纸 + 配置持久化)
// 编译: g++ -o live_wallpaper.exe main.cpp -lgdi32 -luser32 -lshell32 -lcomdlg32 -lshlwapi -mwindows -municode -DUNICODE -D_UNICODE

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <cwchar>
#include <initguid.h>

// 壁纸功能相关头文件和全局变量
static HWND g_hiddenWorkerW = NULL;
static HANDLE g_mpvProcess = NULL;
static HWND g_mpvWindow = NULL;
static bool g_isWallpaperMode = false;

// 托盘相关全局变量
HWND g_hWnd = nullptr;
HWND g_hEdit = nullptr;
std::wstring g_filePath;
NOTIFYICONDATAW g_nid = {};
HFONT g_hFont = nullptr;

#define WM_TRAYICON (WM_USER + 1)
DEFINE_GUID(GUID_TRAYICON, 0x12345678, 0x1234, 0x1234, 0x12, 0x34, 0x56, 0x78, 0x90, 0xAB, 0xCD, 0xEF);

// 配置读写函数
static const wchar_t* REG_KEY = L"Software\\VideoWallpaper";
static const wchar_t* REG_VALUE = L"VideoPath";

void SaveConfig(const std::wstring& videoPath) {
    HKEY hKey;
    if (RegCreateKeyW(HKEY_CURRENT_USER, REG_KEY, &hKey) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, REG_VALUE, 0, REG_SZ,
            (const BYTE*)videoPath.c_str(),
            (DWORD)((videoPath.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }
}

std::wstring LoadConfig() {
    std::wstring path;
    HKEY hKey;
    if (RegOpenKeyW(HKEY_CURRENT_USER, REG_KEY, &hKey) == ERROR_SUCCESS) {
        wchar_t buffer[MAX_PATH];
        DWORD size = sizeof(buffer);
        DWORD type;
        if (RegQueryValueExW(hKey, REG_VALUE, NULL, &type, (LPBYTE)buffer, &size) == ERROR_SUCCESS && type == REG_SZ) {
            path = buffer;
        }
        RegCloseKey(hKey);
    }
    return path;
}

// 壁纸功能函数声明
std::wstring FindMPVExe();
bool IsMPVInstalled();
bool InstallMPVWithWinget();
bool SpawnWorkerW();
HANDLE StartMPV(const std::wstring& videoPath);
HWND FindMPVWindow(DWORD processId);
HANDLE StartAndWaitForMPV(const std::wstring& videoPath, HWND* outWindow);
void HideWallpaperWorkerW();
void ShowWallpaperWorkerW();
bool SetupWallpaper(const std::wstring& videoPath);
void RestoreWallpaper();

// 托盘和GUI函数
void AddTrayIcon();
void RemoveTrayIcon();
void ShowContextMenu(HWND hWnd);
static BOOL CALLBACK SetFontToChild(HWND hChild, LPARAM lParam);

// 壁纸功能实现
std::wstring FindMPVExe() {
    wchar_t path[MAX_PATH];
    if (SearchPathW(NULL, L"mpv.exe", NULL, MAX_PATH, path, NULL) > 0) {
        return std::wstring(path);
    }

    std::vector<std::wstring> commonDirs = {
        L"C:\\Program Files\\MPV Player",
        L"C:\\Program Files (x86)\\MPV Player",
        L"C:\\Program Files\\mpv",
        L"C:\\Program Files (x86)\\mpv",
        L"%LOCALAPPDATA%\\Programs\\mpv",
    };

    for (auto &dir : commonDirs) {
        wchar_t expanded[MAX_PATH];
        ExpandEnvironmentStringsW(dir.c_str(), expanded, MAX_PATH);
        wchar_t fullPath[MAX_PATH];
        swprintf_s(fullPath, L"%s\\mpv.exe", expanded);
        if (GetFileAttributesW(fullPath) != INVALID_FILE_ATTRIBUTES) {
            return std::wstring(fullPath);
        }
    }
    return L"";
}

bool IsMPVInstalled() {
    return !FindMPVExe().empty();
}

bool InstallMPVWithWinget() {
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    wchar_t cmd[] = L"winget install mpv --accept-package-agreements --accept-source-agreements";
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (!ok) return false;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD exitCode;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (exitCode == 0);
}

bool SpawnWorkerW() {
    HWND progman = FindWindowW(L"Progman", NULL);
    if (!progman) return false;
    DWORD_PTR result;
    LRESULT ret = SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &result);
    if (ret == 0) {
        ret = SendMessageTimeoutW(progman, 0x052C, 0x0D, 0x01, SMTO_NORMAL, 1000, &result);
    }
    Sleep(200);
    return ret != 0;
}

HANDLE StartMPV(const std::wstring& videoPath) {
    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);

    wchar_t cmdLine[4096];
    swprintf_s(cmdLine,
        L"\"%s\" --loop-file=inf --geometry=%dx%d --no-border --keepaspect=no "
        L"--no-osc --no-osd-bar --osd-level=0 --cursor-autohide=always "
        L"--hwdec=auto --vo=gpu-next --gpu-api=d3d11 --profile=fast "
        L"--cache=yes --demuxer-max-bytes=2048MiB --video-sync=display-resample "
        L"--ontop --title=\"VideoWallpaper\" \"%s\"",
        FindMPVExe().c_str(), w, h, videoPath.c_str()
    );

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        return NULL;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

HWND FindMPVWindow(DWORD processId) {
    HWND found = NULL;
    struct EnumData {
        DWORD pid;
        HWND* out;
    } data = { processId, &found };

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* pData = (EnumData*)lParam;
        DWORD pid;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == pData->pid) {
            wchar_t cls[64];
            GetClassNameW(hwnd, cls, 64);
            if (wcsstr(cls, L"mpv") != NULL) {
                *pData->out = hwnd;
                return FALSE;
            }
        }
        return TRUE;
    }, (LPARAM)&data);

    return found;
}

HANDLE StartAndWaitForMPV(const std::wstring& videoPath, HWND* outWindow) {
    HANDLE hProcess = StartMPV(videoPath);
    if (!hProcess) return NULL;

    DWORD pid = GetProcessId(hProcess);
    HWND hwnd = NULL;
    for (int i = 0; i < 10; ++i) {
        Sleep(500);
        hwnd = FindMPVWindow(pid);
        if (hwnd) break;
    }
    if (!hwnd) {
        TerminateProcess(hProcess, 0);
        CloseHandle(hProcess);
        return NULL;
    }

    ShowWindow(hwnd, SW_HIDE);
    *outWindow = hwnd;
    return hProcess;
}

void HideWallpaperWorkerW() {
    HWND hWorkerIcon = NULL;
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        wchar_t cls[64];
        if (GetClassNameW(hwnd, cls, 64) && wcscmp(cls, L"WorkerW") == 0) {
            HWND hDefView = FindWindowExW(hwnd, NULL, L"SHELLDLL_DefView", NULL);
            if (hDefView) {
                *((HWND*)lParam) = hwnd;
                return FALSE;
            }
        }
        return TRUE;
    }, (LPARAM)&hWorkerIcon);

    if (!hWorkerIcon) return;

    HWND hWorkerWallpaper = FindWindowExW(NULL, hWorkerIcon, L"WorkerW", NULL);
    if (hWorkerWallpaper) {
        ShowWindow(hWorkerWallpaper, SW_HIDE);
        g_hiddenWorkerW = hWorkerWallpaper;
    } else {
        g_hiddenWorkerW = NULL;
    }
}

void ShowWallpaperWorkerW() {
    if (g_hiddenWorkerW && IsWindow(g_hiddenWorkerW)) {
        ShowWindow(g_hiddenWorkerW, SW_SHOW);
        g_hiddenWorkerW = NULL;
    }
}

bool SetupWallpaper(const std::wstring& videoPath) {
    // 如果已有壁纸，先终止旧 mpv
    if (g_isWallpaperMode) {
        if (g_mpvProcess) {
            TerminateProcess(g_mpvProcess, 0);
            CloseHandle(g_mpvProcess);
            g_mpvProcess = NULL;
        }
        if (g_mpvWindow && IsWindow(g_mpvWindow)) {
            SetParent(g_mpvWindow, NULL);
            PostMessageW(g_mpvWindow, WM_CLOSE, 0, 0);
            g_mpvWindow = NULL;
        }
        g_isWallpaperMode = false;
    }

    // 启动 mpv
    g_mpvProcess = StartAndWaitForMPV(videoPath, &g_mpvWindow);
    if (!g_mpvProcess) return false;

    HWND hProgman = FindWindowW(L"Progman", NULL);
    if (!hProgman) {
        if (g_mpvProcess) { TerminateProcess(g_mpvProcess, 0); CloseHandle(g_mpvProcess); g_mpvProcess = NULL; }
        return false;
    }

    SpawnWorkerW();
    Sleep(500);

    // 将mpv窗口设为Progman的子窗口
    LONG style = GetWindowLongW(g_mpvWindow, GWL_STYLE);
    style &= ~(WS_POPUP | WS_CAPTION | WS_BORDER | WS_THICKFRAME);
    style |= WS_CHILD | WS_CLIPCHILDREN | WS_VISIBLE;
    SetWindowLongW(g_mpvWindow, GWL_STYLE, style);

    SetParent(g_mpvWindow, hProgman);
    RECT rect;
    GetClientRect(hProgman, &rect);
    SetWindowPos(g_mpvWindow, HWND_TOP, 0, 0, rect.right, rect.bottom,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);

    ShowWindow(g_mpvWindow, SW_SHOW);

    HideWallpaperWorkerW();

    g_isWallpaperMode = true;

    // 保存配置
    SaveConfig(videoPath);

    return true;
}

void RestoreWallpaper() {
    if (g_mpvProcess) {
        TerminateProcess(g_mpvProcess, 0);
        CloseHandle(g_mpvProcess);
        g_mpvProcess = NULL;
    }
    if (g_mpvWindow && IsWindow(g_mpvWindow)) {
        SetParent(g_mpvWindow, NULL);
        PostMessageW(g_mpvWindow, WM_CLOSE, 0, 0);
        g_mpvWindow = NULL;
    }
    ShowWallpaperWorkerW();
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, 0, SMTO_NORMAL, 1000, NULL);
    g_isWallpaperMode = false;
}

// 托盘和GUI函数实现
void AddTrayIcon() {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = g_hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_GUID;
    g_nid.guidItem = GUID_TRAYICON;
    g_nid.uCallbackMessage = WM_TRAYICON;

    HICON hIcon = (HICON)LoadImageW(nullptr, L"icon.ico", IMAGE_ICON,
        16, 16, LR_LOADFROMFILE | LR_SHARED);
    if (hIcon)
        g_nid.hIcon = hIcon;
    else
        g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);

    wcscpy_s(g_nid.szTip, L"视频壁纸工具");

    if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        g_nid.uFlags &= ~NIF_GUID;
        Shell_NotifyIconW(NIM_ADD, &g_nid);
    }
}

void RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

void ShowContextMenu(HWND hWnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, 100, L"恢复静态壁纸");
    AppendMenuW(hMenu, MF_STRING, 101, L"退出");
    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
    PostMessageW(hWnd, WM_NULL, 0, 0);
}

static BOOL CALLBACK SetFontToChild(HWND hChild, LPARAM lParam) {
    if (g_hFont)
        SendMessageW(hChild, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    return TRUE;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        // 创建控件
        CreateWindowW(L"STATIC", L"文件路径：",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            20, 30, 80, 20, hWnd, (HMENU)0, GetModuleHandleW(nullptr), nullptr);

        g_hEdit = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            100, 30, 300, 25, hWnd, (HMENU)1, GetModuleHandleW(nullptr), nullptr);

        CreateWindowW(L"BUTTON", L"选择文件",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            410, 30, 100, 25, hWnd, (HMENU)2, GetModuleHandleW(nullptr), nullptr);

        CreateWindowW(L"BUTTON", L"应用",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            520, 30, 80, 25, hWnd, (HMENU)3, GetModuleHandleW(nullptr), nullptr);

        // 窗口图标
        HICON hIconBig = (HICON)LoadImageW(nullptr, L"icon.ico", IMAGE_ICON,
            32, 32, LR_LOADFROMFILE | LR_SHARED);
        if (hIconBig) {
            SendMessageW(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconBig);
            SendMessageW(hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
        }

        // 字体
        LOGFONTW lf = {};
        lf.lfHeight = -16;
        lf.lfWeight = FW_NORMAL;
        wcscpy_s(lf.lfFaceName, L"微软雅黑");
        g_hFont = CreateFontIndirectW(&lf);
        if (g_hFont)
            EnumChildWindows(hWnd, SetFontToChild, 0);

        // 检查mpv是否安装
        if (!IsMPVInstalled()) {
            int ret = MessageBoxW(hWnd, L"未检测到 mpv，是否立即安装？\n(将使用 winget 静默安装)", L"缺少组件", MB_YESNO | MB_ICONQUESTION);
            if (ret == IDYES) {
                if (InstallMPVWithWinget()) {
                    MessageBoxW(hWnd, L"mpv 安装成功！", L"提示", MB_OK);
                } else {
                    MessageBoxW(hWnd, L"mpv 安装失败，请手动安装后再使用。", L"错误", MB_ICONERROR);
                }
            }
        }

        PostMessageW(hWnd, WM_APP, 0, 0);
        break;
    }

    case WM_APP: {
        AddTrayIcon();
        break;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdcStatic = (HDC)wParam;
        SetTextColor(hdcStatic, GetSysColor(COLOR_WINDOWTEXT));
        SetBkMode(hdcStatic, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == 2) { // 选择文件
            OPENFILENAMEW ofn = {};
            wchar_t szFile[MAX_PATH] = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hWnd;
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrFilter = L"视频文件\0*.mp4;*.avi;*.mkv;*.mov;*.wmv;*.flv;*.webm;*.m4v\0所有文件\0*.*\0";
            ofn.nFilterIndex = 1;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                SetWindowTextW(g_hEdit, szFile);
                g_filePath = szFile;
            }
        }
        else if (id == 3) { // 应用当前视频
            wchar_t buffer[1024];
            GetWindowTextW(g_hEdit, buffer, 1024);
            g_filePath = buffer;
            if (g_filePath.empty()) {
                MessageBoxW(hWnd, L"请先选择视频文件", L"提示", MB_OK);
                break;
            }
            if (GetFileAttributesW(g_filePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                MessageBoxW(hWnd, L"文件不存在", L"错误", MB_ICONERROR);
                break;
            }
            if (!IsMPVInstalled()) {
                MessageBoxW(hWnd, L"mpv 未安装，请先安装 mpv", L"错误", MB_ICONERROR);
                break;
            }
            if (!SetupWallpaper(g_filePath)) {
                MessageBoxW(hWnd, L"设置壁纸失败，请检查视频文件或系统环境。", L"错误", MB_ICONERROR);
            }
        }
        else if (id == 100) { // 恢复壁纸
            if (g_isWallpaperMode) {
                RestoreWallpaper();
            } else {
                MessageBoxW(hWnd, L"当前没有动态壁纸", L"提示", MB_OK);
            }
        }
        else if (id == 101) { // 退出并恢复默认壁纸
            if (g_isWallpaperMode) RestoreWallpaper();
            DestroyWindow(hWnd);
        }
        break;
    }

    case WM_CLOSE: {
        ShowWindow(hWnd, SW_HIDE);
        return 0;
    }

    case WM_DESTROY: {
        RemoveTrayIcon();
        if (g_hFont) DeleteObject(g_hFont);
        if (g_isWallpaperMode) RestoreWallpaper();
        PostQuitMessage(0);
        break;
    }

    case WM_TRAYICON: {
        if (lParam == WM_LBUTTONDBLCLK) {
            ShowWindow(hWnd, SW_SHOW);
            SetForegroundWindow(hWnd);
        }
        else if (lParam == WM_RBUTTONUP) {
            ShowContextMenu(hWnd);
        }
        break;
    }

    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"TrayAppClass";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    HICON hIconBig = (HICON)LoadImageW(nullptr, L"icon.ico", IMAGE_ICON,
        48, 48, LR_LOADFROMFILE | LR_SHARED);
    if (hIconBig)
        wc.hIcon = hIconBig;
    else
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);

    if (!RegisterClassW(&wc)) {
        MessageBoxW(nullptr, L"窗口类注册失败", L"错误", MB_ICONERROR);
        return 1;
    }

    g_hWnd = CreateWindowW(L"TrayAppClass", L"视频壁纸设置",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 630, 110,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_hWnd) {
        MessageBoxW(nullptr, L"窗口创建失败", L"错误", MB_ICONERROR);
        return 1;
    }

    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    // 启动时自动加载配置
    std::wstring savedPath = LoadConfig();
    if (!savedPath.empty() && GetFileAttributesW(savedPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        // 静默加载，不弹出任何消息框
        if (IsMPVInstalled()) {
            SetupWallpaper(savedPath);
            // 将路径填入编辑框，方便用户查看
            SetWindowTextW(g_hEdit, savedPath.c_str());
            g_filePath = savedPath;
        }
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}