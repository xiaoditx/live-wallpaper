// main.cpp (集成视频壁纸 + 配置持久化 + 资源图标)
// 编译: mingw32-make (或 g++ 直接编译)
// 依赖于 resources.rc 和 resources.hpp

// 减少不必要的头文件加载，提高编译速度
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <cwchar>
#include <initguid.h>

// 资源文件头文件
#include "resources.hpp"

// 壁纸功能相关头文件和全局变量
static HWND g_hiddenWorkerW = NULL;    // 中层WorkerW窗口的句柄，找到后存储以便回复
static HANDLE g_mpvProcess = NULL;     // mpv播放进程句柄，启动后存储以便终止
static HWND g_mpvWindow = NULL;        // mpv播放窗口句柄，启动后存储以便设置父窗口和关闭
static bool g_isWallpaperMode = false; // 当前是否处于壁纸模式

// 其他全局变量
HWND g_hWnd = nullptr;      // 主窗口句柄
HWND g_hEdit = nullptr;     // 编辑框句柄
std::wstring g_filePath;    // 当前选择的视频文件路径
NOTIFYICONDATAW g_nid = {}; // 托盘图标数据结构
HFONT g_hFont = nullptr;    // 字体句柄

// 自定义消息
#define WM_TRAYICON (WM_USER + 1)
#ifdef DEFINE_GUID
DEFINE_GUID(GUID_TRAYICON, 0x12345678, 0x1234, 0x1234, 0x12, 0x34, 0x56, 0x78, 0x90, 0xAB, 0xCD, 0xEF);
#endif // DEFINE_GUID

// 配置读写常量
static const wchar_t *REG_KEY = L"Software\\VideoWallpaper"; // 注册表键路径
static const wchar_t *REG_VALUE = L"VideoPath";              // 注册表值名称

// 写入注册表保存配置
void SaveConfig(const std::wstring &videoPath)
{
    HKEY hKey;
    if (RegCreateKeyW(HKEY_CURRENT_USER, REG_KEY, &hKey) == ERROR_SUCCESS)
    {
        RegSetValueExW(hKey, REG_VALUE, 0, REG_SZ,
                       (const BYTE *)videoPath.c_str(),
                       (DWORD)((videoPath.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }
}

// 从注册表读取配置
std::wstring LoadConfig()
{
    std::wstring path;
    HKEY hKey;
    if (RegOpenKeyW(HKEY_CURRENT_USER, REG_KEY, &hKey) == ERROR_SUCCESS)
    {
        wchar_t buffer[MAX_PATH];
        DWORD size = sizeof(buffer);
        DWORD type;
        if (RegQueryValueExW(hKey, REG_VALUE, NULL, &type, (LPBYTE)buffer, &size) == ERROR_SUCCESS && type == REG_SZ)
        {
            path = buffer;
        }
        RegCloseKey(hKey);
    }
    return path;
}

// 壁纸功能函数向前声明
std::wstring FindMPVExe();
bool IsMPVInstalled();
bool InstallMPVWithWinget();
bool SpawnWorkerW();
HANDLE StartMPV(const std::wstring &videoPath);
HWND FindMPVWindow(DWORD processId);
HANDLE StartAndWaitForMPV(const std::wstring &videoPath, HWND *outWindow);
void HideWallpaperWorkerW();
void ShowWallpaperWorkerW();
bool SetupWallpaper(const std::wstring &videoPath);
void RestoreWallpaper();

// 托盘和GUI函数向前声明
void AddTrayIcon();
void RemoveTrayIcon();
void ShowContextMenu(HWND hWnd);
static BOOL CALLBACK SetFontToChild(HWND hChild, LPARAM lParam);

/* ↓↓↓ 壁纸功能实现 ↓↓↓ */

// 查找mpv.exe的路径
std::wstring FindMPVExe()
{
    wchar_t path[MAX_PATH];
    // 尝试使用SearchPath查找mpv.exe
    if (SearchPathW(NULL, L"mpv.exe", NULL, MAX_PATH, path, NULL) > 0)
    {
        return std::wstring(path);
    }

    // 如果SearchPath失败，尝试常见安装目录

    // 常见安装目录列表
    std::vector<std::wstring> commonDirs = {
        L"C:\\Program Files\\MPV Player",
        L"C:\\Program Files (x86)\\MPV Player",
        L"C:\\Program Files\\mpv",
        L"C:\\Program Files (x86)\\mpv",
        L"%LOCALAPPDATA%\\Programs\\mpv",
    };

    // 遍历常见目录，检查mpv.exe是否存在
    for (auto &dir : commonDirs)
    {
        wchar_t expanded[MAX_PATH];
        // 展开环境变量路径
        ExpandEnvironmentStringsW(dir.c_str(), expanded, MAX_PATH);
        wchar_t fullPath[MAX_PATH];
        swprintf_s(fullPath, L"%s\\mpv.exe", expanded);
        // 检查文件是否存在
        if (GetFileAttributesW(fullPath) != INVALID_FILE_ATTRIBUTES)
        {
            return std::wstring(fullPath);
        }
    }
    return L"";
}

// 检查mpv是否安装
bool IsMPVInstalled()
{
    return !FindMPVExe().empty();
}

// 使用winget安装mpv
bool InstallMPVWithWinget()
{
    // 使用CreateProcess启动winget安装mpv
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    wchar_t cmd[] = L"winget install mpv --accept-package-agreements --accept-source-agreements";
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (!ok)
        return false;
    // 等待安装完成，最多等待60秒
    WaitForSingleObject(pi.hProcess, 60000);
    // 获取退出码，判断是否安装成功
    DWORD exitCode;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    // 关闭句柄
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (exitCode == 0);
}

// 发送0x052C消息给Progman，分裂出两个WorkerW窗口
bool SpawnWorkerW()
{
    // 寻找Progman窗口
    HWND progman = FindWindowW(L"Progman", NULL);
    if (!progman)
        return false;
    // 发送消息
    DWORD_PTR result;
    LRESULT ret = SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &result);
    if (ret == 0)
    {
        ret = SendMessageTimeoutW(progman, 0x052C, 0x0D, 0x01, SMTO_NORMAL, 1000, &result);
    }
    // 等待200毫秒，确保WorkerW窗口创建完成
    Sleep(200);
    return ret != 0;
}

// 启动mpv播放视频，并返回进程句柄
HANDLE StartMPV(const std::wstring &videoPath)
{
    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);

    wchar_t cmdLine[4096];
    /*
    参数说明：
    --loop-file=inf : 无限循环播放视频
    --geometry=%dx%d : 设置窗口大小为屏幕分辨率
    --no-border : 无边框
    --keepaspect=no : 不保持宽高比，填充整个屏幕
    --no-osc : 不显示控制条
    --no-osd-bar : 不显示OSD条
    --osd-level=0 : 不显示OSD信息
    --cursor-autohide=always : 鼠标自动隐藏
    --hwdec=auto : 自动启用硬件解码
    --vo=gpu-next : 使用gpu-next视频输出
    --gpu-api=d3d11 : 使用Direct3D 11作为GPU API
    --profile=fast : 使用快速播放配置
    --cache=yes : 启用缓存
    --demuxer-max-bytes=2048MiB : 设置最大解复用缓存为2GB
    --video-sync=display-resample : 使用显示器刷新率同步视频
    --ontop : 窗口置顶
    --title="VideoWallpaper" : 设置窗口标题为VideoWallpaper
    */
    swprintf_s(cmdLine,
               L"\"%s\" --loop-file=inf --geometry=%dx%d --no-border --keepaspect=no "
               L"--no-osc --no-osd-bar --osd-level=0 --cursor-autohide=always "
               L"--hwdec=auto --vo=gpu-next --gpu-api=d3d11 --profile=fast "
               L"--cache=yes --demuxer-max-bytes=2048MiB --video-sync=display-resample "
               L"--ontop --title=\"VideoWallpaper\" \"%s\"",
               FindMPVExe().c_str(), w, h, videoPath.c_str());

    // 创建进程
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        return NULL;
    }
    // 关闭线程句柄，返回进程句柄
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

// 查找指定进程ID的mpv窗口句柄
HWND FindMPVWindow(DWORD processId)
{
    // 使用EnumWindows枚举所有顶层窗口
    // 查找属于指定进程ID的mpv窗口

    // 输出句柄
    HWND found = NULL;
    // 定义结构体用于传递参数
    struct EnumData
    {
        DWORD pid;
        HWND *out;
    } data = {processId, &found};

    // 枚举窗口
    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL
        {
            auto *pData = (EnumData *)lParam;
            DWORD pid;
            // 获取窗口所属进程ID
            GetWindowThreadProcessId(hwnd, &pid);
            // 检查是否是目标进程
            if (pid == pData->pid)
            {
                // 检查窗口类名是否包含"mpv"
                wchar_t cls[64];
                // 获取窗口类名
                GetClassNameW(hwnd, cls, 64);
                if (wcsstr(cls, L"mpv") != NULL)
                {
                    // 找到mpv窗口，保存句柄并停止枚举
                    *pData->out = hwnd;
                    return FALSE;
                }
            }
            return TRUE;
        },
        (LPARAM)&data // （别动这行注释）
    );

    return found;
}

// 启动mpv并等待窗口创建，返回进程句柄和窗口句柄
HANDLE StartAndWaitForMPV(const std::wstring &videoPath, HWND *outWindow)
{
    // 启动mpv进程
    HANDLE hProcess = StartMPV(videoPath);
    if (!hProcess)
        return NULL;

    // 等待mpv窗口创建，最多等待5秒
    DWORD pid = GetProcessId(hProcess);
    HWND hwnd = NULL;
    for (int i = 0; i < 10; ++i)
    {
        Sleep(500);
        hwnd = FindMPVWindow(pid);
        if (hwnd)
            break;
    }

    // 如果未找到窗口，终止进程并返回NULL
    if (!hwnd)
    {
        TerminateProcess(hProcess, 0);
        CloseHandle(hProcess);
        return NULL;
    }

    // 隐藏mpv窗口，防止被用户点到
    ShowWindow(hwnd, SW_HIDE);
    *outWindow = hwnd;
    return hProcess;
}

// 隐藏中层WorkerW窗口，露出mpv窗口
void HideWallpaperWorkerW()
{
    HWND hWorkerIcon = NULL;
    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL
        {
            wchar_t cls[64];
            if (GetClassNameW(hwnd, cls, 64) &&
                wcscmp(cls, L"WorkerW") == 0)
            {
                HWND hDefView = FindWindowExW(
                    hwnd, NULL,
                    L"SHELLDLL_DefView", NULL //
                );

                if (hDefView)
                {
                    *((HWND *)lParam) = hwnd;
                    return FALSE;
                }
            }
            return TRUE;
        },
        (LPARAM)&hWorkerIcon // （别动这行注释）
    );

    // 如果没有找到WorkerW窗口，直接返回
    if (!hWorkerIcon)
        return;

    // 找到WorkerW窗口后，查找其子窗口WorkerW（中层窗口）
    HWND hWorkerWallpaper = FindWindowExW(NULL, hWorkerIcon, L"WorkerW", NULL);
    if (hWorkerWallpaper)
    {
        ShowWindow(hWorkerWallpaper, SW_HIDE);
        g_hiddenWorkerW = hWorkerWallpaper;
    }
    else
    {
        g_hiddenWorkerW = NULL;
    }
}

// 显示中层WorkerW窗口，恢复原有壁纸
void ShowWallpaperWorkerW()
{
    if (g_hiddenWorkerW && IsWindow(g_hiddenWorkerW))
    {
        ShowWindow(g_hiddenWorkerW, SW_SHOW);
        g_hiddenWorkerW = NULL;
    }
}

// 设置视频为壁纸
bool SetupWallpaper(const std::wstring &videoPath)
{
    // 如果已有壁纸，先终止旧 mpv
    if (g_isWallpaperMode)
    {
        if (g_mpvProcess)
        {
            TerminateProcess(g_mpvProcess, 0);
            CloseHandle(g_mpvProcess);
            g_mpvProcess = NULL;
        }
        if (g_mpvWindow && IsWindow(g_mpvWindow))
        {
            SetParent(g_mpvWindow, NULL);
            PostMessageW(g_mpvWindow, WM_CLOSE, 0, 0);
            g_mpvWindow = NULL;
        }
        g_isWallpaperMode = false;
    }

    // 启动 mpv
    g_mpvProcess = StartAndWaitForMPV(videoPath, &g_mpvWindow);
    if (!g_mpvProcess)
        return false;

    HWND hProgman = FindWindowW(L"Progman", NULL);
    if (!hProgman)
    {
        if (g_mpvProcess)
        {
            TerminateProcess(g_mpvProcess, 0);
            CloseHandle(g_mpvProcess);
            g_mpvProcess = NULL;
        }
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

    // 隐藏中层WorkerW窗口，露出mpv窗口
    HideWallpaperWorkerW();

    // 设置标志，表示当前处于壁纸模式
    g_isWallpaperMode = true;

    // 保存配置
    SaveConfig(videoPath);

    return true;
}

// 恢复默认壁纸，终止mpv进程，关闭mpv窗口，恢复中层WorkerW窗口
void RestoreWallpaper()
{
    if (g_mpvProcess)
    {
        TerminateProcess(g_mpvProcess, 0);
        CloseHandle(g_mpvProcess);
        g_mpvProcess = NULL;
    }
    if (g_mpvWindow && IsWindow(g_mpvWindow))
    {
        SetParent(g_mpvWindow, NULL);
        PostMessageW(g_mpvWindow, WM_CLOSE, 0, 0);
        g_mpvWindow = NULL;
    }
    // 显示中层WorkerW窗口，恢复原有壁纸
    ShowWallpaperWorkerW();
    // 通知系统刷新壁纸设置
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, 0, SMTO_NORMAL, 1000, NULL);
    // 清除壁纸模式标志
    g_isWallpaperMode = false;
}

// 托盘和GUI函数实现
void AddTrayIcon()
{
    // 初始化托盘图标数据结构
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = g_hWnd; // 设置托盘图标的窗口句柄
    g_nid.uID = 1;       // 设置托盘图标的ID
    // 设置托盘图标的标志，包含图标、消息、提示和GUID
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_GUID;
    // 设置托盘图标的GUID，用于唯一标识托盘图标
    g_nid.guidItem = GUID_TRAYICON;
    // 设置托盘图标的回调消息，当用户点击托盘图标时，窗口会收到WM_TRAYICON消息
    g_nid.uCallbackMessage = WM_TRAYICON;

    // 从资源加载图标
    HICON hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_MY_ICON));
    if (!hIcon)
        hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    g_nid.hIcon = hIcon;

    wcscpy_s(g_nid.szTip, L"视频壁纸工具");

    // 尝试添加托盘图标，如果失败则尝试不使用GUID
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid))
    {
        g_nid.uFlags &= ~NIF_GUID;
        Shell_NotifyIconW(NIM_ADD, &g_nid);
    }
}

// 移除托盘图标
void RemoveTrayIcon()
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

// 显示托盘图标右键菜单
void ShowContextMenu(HWND hWnd)
{
    // 获取鼠标位置
    POINT pt;
    GetCursorPos(&pt);
    // 创建弹出菜单
    HMENU hMenu = CreatePopupMenu();
    // 添加菜单项
    AppendMenuW(hMenu, MF_STRING, 100, L"恢复静态壁纸");
    AppendMenuW(hMenu, MF_STRING, 101, L"退出");
    // 设置窗口为前景窗口，确保菜单显示在最上层
    SetForegroundWindow(hWnd);
    // 显示弹出菜单，使用右键点击位置
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
    PostMessageW(hWnd, WM_NULL, 0, 0);
}

// 设置字体给所有子控件
static BOOL CALLBACK SetFontToChild(HWND hChild, LPARAM lParam)
{
    if (g_hFont)
        SendMessageW(hChild, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    return TRUE;
}

// 窗口过程函数，处理窗口消息
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        // 创建控件

        // 创建静态文本控件，显示“文件路径：”
        CreateWindowW(L"STATIC", L"文件路径：",
                      WS_CHILD | WS_VISIBLE | SS_LEFT,
                      20, 30, 80, 20, hWnd, (HMENU)0, GetModuleHandleW(nullptr), nullptr);

        // 创建编辑框控件，用于输入或显示视频文件路径
        g_hEdit = CreateWindowW(L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                100, 30, 300, 25, hWnd, (HMENU)1, GetModuleHandleW(nullptr), nullptr);

        // 创建“选择文件”按钮控件
        CreateWindowW(L"BUTTON", L"选择文件",
                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      410, 30, 100, 25, hWnd, (HMENU)2, GetModuleHandleW(nullptr), nullptr);

        // 创建“应用”按钮控件
        CreateWindowW(L"BUTTON", L"应用",
                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      520, 30, 80, 25, hWnd, (HMENU)3, GetModuleHandleW(nullptr), nullptr);

        // 从资源加载窗口图标
        HICON hIconBig = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_MY_ICON));
        if (!hIconBig)
            hIconBig = LoadIconW(nullptr, IDI_APPLICATION);
        if (hIconBig)
        {
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
        if (!IsMPVInstalled())
        {
            int ret = MessageBoxW(hWnd, L"未检测到 mpv，是否立即安装？\n(将使用 winget 静默安装)", L"缺少组件", MB_YESNO | MB_ICONQUESTION);
            if (ret == IDYES)
            {
                if (InstallMPVWithWinget())
                {
                    MessageBoxW(hWnd, L"mpv 安装成功！", L"提示", MB_OK);
                }
                else
                {
                    MessageBoxW(hWnd, L"mpv 安装失败，请手动安装后再使用。", L"错误", MB_ICONERROR);
                }
            }
        }

        PostMessageW(hWnd, WM_APP, 0, 0);
        break;
    }

    case WM_APP:
    {
        AddTrayIcon();
        break;
    }

    case WM_CTLCOLORSTATIC:
    {
        // 设置静态文本控件的文本颜色和背景模式
        HDC hdcStatic = (HDC)wParam;
        SetTextColor(hdcStatic, GetSysColor(COLOR_WINDOWTEXT));
        SetBkMode(hdcStatic, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }

    case WM_COMMAND:
    {
        /* 处理按钮点击事件*/
        // 按钮ID
        int id = LOWORD(wParam);
        if (id == 2)
        { // 选择文件
            OPENFILENAMEW ofn = {};
            wchar_t szFile[MAX_PATH] = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hWnd;
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrFilter = L"视频文件\0*.mp4;*.avi;*.mkv;*.mov;*.wmv;*.flv;*.webm;*.m4v\0所有文件\0*.*\0";
            ofn.nFilterIndex = 1;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn))
            {
                SetWindowTextW(g_hEdit, szFile);
                g_filePath = szFile;
            }
        }
        else if (id == 3)
        { // 应用当前视频
            wchar_t buffer[1024];
            GetWindowTextW(g_hEdit, buffer, 1024);
            g_filePath = buffer;
            if (g_filePath.empty())
            {
                MessageBoxW(hWnd, L"请先选择视频文件", L"提示", MB_OK);
                break;
            }
            if (GetFileAttributesW(g_filePath.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                MessageBoxW(hWnd, L"文件不存在", L"错误", MB_ICONERROR);
                break;
            }
            if (!IsMPVInstalled())
            {
                MessageBoxW(hWnd, L"mpv 未安装，请先安装 mpv", L"错误", MB_ICONERROR);
                break;
            }
            if (!SetupWallpaper(g_filePath))
            {
                MessageBoxW(hWnd, L"设置壁纸失败，请检查视频文件或系统环境。", L"错误", MB_ICONERROR);
            }
        }
        else if (id == 100)
        { // 恢复壁纸
            if (g_isWallpaperMode)
            {
                RestoreWallpaper();
            }
            else
            {
                MessageBoxW(hWnd, L"当前没有动态壁纸", L"提示", MB_OK);
            }
        }
        else if (id == 101)
        { // 退出并恢复默认壁纸
            if (g_isWallpaperMode)
                RestoreWallpaper();
            DestroyWindow(hWnd);
        }
        break;
    }

    case WM_CLOSE:
    {
        ShowWindow(hWnd, SW_HIDE);
        return 0;
    }

    case WM_DESTROY:
    {
        RemoveTrayIcon();
        if (g_hFont)
            DeleteObject(g_hFont);
        if (g_isWallpaperMode)
            RestoreWallpaper();
        PostQuitMessage(0);
        break;
    }

    case WM_TRAYICON:
    {
        if (lParam == WM_LBUTTONDBLCLK)
        {
            ShowWindow(hWnd, SW_SHOW);
            SetForegroundWindow(hWnd);
        }
        else if (lParam == WM_RBUTTONUP)
        {
            ShowContextMenu(hWnd);
        }
        break;
    }

    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow)
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"TrayAppClass";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    // 从资源加载大图标
    HICON hIconBig = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_MY_ICON));
    if (!hIconBig)
        hIconBig = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIcon = hIconBig;

    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);

    if (!RegisterClassW(&wc))
    {
        MessageBoxW(nullptr, L"窗口类注册失败", L"错误", MB_ICONERROR);
        return 1;
    }

    g_hWnd = CreateWindowW(L"TrayAppClass", L"视频壁纸设置",
                           WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
                           CW_USEDEFAULT, CW_USEDEFAULT, 630, 110,
                           nullptr, nullptr, hInstance, nullptr);

    if (!g_hWnd)
    {
        MessageBoxW(nullptr, L"窗口创建失败", L"错误", MB_ICONERROR);
        return 1;
    }

    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    // 启动时自动加载配置
    std::wstring savedPath = LoadConfig();
    if (!savedPath.empty() && GetFileAttributesW(savedPath.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        // 静默加载，不弹出任何消息框
        if (IsMPVInstalled())
        {
            SetupWallpaper(savedPath);
            // 将路径填入编辑框，方便用户查看
            SetWindowTextW(g_hEdit, savedPath.c_str());
            g_filePath = savedPath;
        }
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}