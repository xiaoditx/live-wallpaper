// video_wallpaper.cpp
// 编译: g++ -municode -DUNICODE -D_UNICODE -o wallpaper.exe main.cpp -luser32 -lshell32 -lshlwapi

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <string>
#include <cstdio>
#include <cwchar>
#include <vector>

// 全局变量：保存被隐藏的壁纸层 WorkerW 句柄
static HWND g_hiddenWorkerW = NULL;

// 辅助函数
bool IsFFmpegInstalled() {
    wchar_t path[MAX_PATH];
    return SearchPathW(NULL, L"ffmpeg.exe", NULL, MAX_PATH, path, NULL) > 0;
}

bool InstallFFmpegWithWinget() {
    wprintf(L"[*] 正在使用 winget 安装 ffmpeg...\n");
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    wchar_t cmd[] = L"winget install ffmpeg --accept-package-agreements --accept-source-agreements";
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (!ok) {
        wprintf(L"[!] winget 启动失败，请手动安装 ffmpeg\n");
        return false;
    }
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD exitCode;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return exitCode == 0;
}

// 触发Progman分裂
bool SpawnWorkerW() {
    HWND progman = FindWindowW(L"Progman", NULL);
    if (!progman) {
        wprintf(L"[!] 未找到 Progman 窗口\n");
        return false;
    }
    DWORD_PTR result;
    LRESULT ret = SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &result);
    if (ret == 0) {
        ret = SendMessageTimeoutW(progman, 0x052C, 0x0D, 0x01, SMTO_NORMAL, 1000, &result);
    }
    Sleep(200);
    return ret != 0;
}

// 启动ffplay
HANDLE StartFFPlay(const std::wstring& videoPath) {
    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);
    wchar_t cmdLine[2048];
    wsprintfW(cmdLine, L"ffplay -loop 0 -window_title \"VideoWallpaper\" -x %d -y %d -noborder \"%s\"",
              w, h, videoPath.c_str());

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        wprintf(L"[!] 启动 ffplay 失败\n");
        return NULL;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

HWND FindFFPlayWindow() {
    // 先尝试用标题精确查找
    HWND hwnd = FindWindowW(L"SDL_app", L"VideoWallpaper");
    if (hwnd && IsWindow(hwnd)) {
        // 验证进程是否为 ffplay
        DWORD pid;
        GetWindowThreadProcessId(hwnd, &pid);
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (hProcess) {
            wchar_t exePath[MAX_PATH];
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(hProcess, 0, exePath, &size)) {
                if (wcsstr(exePath, L"ffplay.exe") != NULL) {
                    CloseHandle(hProcess);
                    return hwnd;
                }
            }
            CloseHandle(hProcess);
        }
    }

    // 若精确标题没找到，则枚举所有 SDL_app 窗口，验证进程
    HWND found = NULL;
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        wchar_t cls[64];
        if (GetClassNameW(hwnd, cls, 64) && wcscmp(cls, L"SDL_app") == 0) {
            DWORD pid;
            GetWindowThreadProcessId(hwnd, &pid);
            HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
            if (hProcess) {
                wchar_t exePath[MAX_PATH];
                DWORD size = MAX_PATH;
                if (QueryFullProcessImageNameW(hProcess, 0, exePath, &size)) {
                    if (wcsstr(exePath, L"ffplay.exe") != NULL) {
                        *((HWND*)lParam) = hwnd;
                        CloseHandle(hProcess);
                        return FALSE; // 找到，停止枚举
                    }
                }
                CloseHandle(hProcess);
            }
        }
        return TRUE;
    }, (LPARAM)&found);

    return found;
}

// 隐藏壁纸层 WorkerW (WorkerW2)
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

    if (!hWorkerIcon) {
        wprintf(L"[!] 未找到图标层 WorkerW\n");
        return;
    }

    // 找下一个 WorkerW（壁纸层）
    HWND hWorkerWallpaper = FindWindowExW(NULL, hWorkerIcon, L"WorkerW", NULL);
    if (hWorkerWallpaper) {
        ShowWindow(hWorkerWallpaper, SW_HIDE);
        g_hiddenWorkerW = hWorkerWallpaper;  // 保存句柄
        wprintf(L"[调试] 已隐藏壁纸层 WorkerW (0x%p)\n", hWorkerWallpaper);
    } else {
        wprintf(L"[!] 未找到壁纸层 WorkerW (WorkerW2)\n");
        g_hiddenWorkerW = NULL;
    }
}

// 恢复壁纸层 WorkerW（仅恢复我们隐藏的那个） 
void ShowWallpaperWorkerW() {
    if (g_hiddenWorkerW && IsWindow(g_hiddenWorkerW)) {
        ShowWindow(g_hiddenWorkerW, SW_SHOW);
        wprintf(L"[调试] 已恢复壁纸层 WorkerW (0x%p)\n", g_hiddenWorkerW);
        g_hiddenWorkerW = NULL;
    } else {
        wprintf(L"[!] 没有找到之前隐藏的 WorkerW，尝试枚举显示所有隐藏的 WorkerW（不推荐）\n");
        // 作为后备，显示所有隐藏的 WorkerW（但可能导致问题，这里留空）
    }
}

// 恢复静态壁纸 
void RestoreStaticWallpaper(HANDLE ffplayProcess, HWND videoWnd) {
    wprintf(L"[*] 正在恢复静态壁纸...\n");

    // 销毁视频窗口
    if (videoWnd && IsWindow(videoWnd)) {
        // 先解除父子关系，防止父窗口残留
        SetParent(videoWnd, NULL);
        PostMessageW(videoWnd, WM_CLOSE, 0, 0);
        Sleep(200);
    }

    // 终止 ffplay 进程
    if (ffplayProcess) {
        TerminateProcess(ffplayProcess, 0);
        CloseHandle(ffplayProcess);
    }

    // 恢复壁纸层
    ShowWallpaperWorkerW();

    // 刷新桌面
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, 0, SMTO_NORMAL, 1000, NULL);
    wprintf(L"[+] 已恢复静态壁纸\n");
}

// 用户交互 
bool AskUserRestore() {
    wprintf(L"\n是否恢复静态壁纸？(y/n): ");
    wchar_t ch;
    wscanf_s(L" %c", &ch, 1);
    return (ch == L'y' || ch == L'Y');
}

bool IsKeyPressed(int vKey) {
    return (GetAsyncKeyState(vKey) & 0x8000) != 0;
}

// 主函数 
int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    setlocale(LC_ALL, "zh-CN.UTF-8");

    wprintf(L"========================================\n");
    wprintf(L"  视频壁纸工具 (基于 0x52C 消息)\n");
    wprintf(L"========================================\n\n");

    if (argc < 2) {
        wprintf(L"用法: %ls <视频文件路径>\n", argv[0]);
        wprintf(L"示例: %ls C:\\videos\\wallpaper.mp4\n", argv[0]);
        return 1;
    }

    std::wstring videoPath = argv[1];
    if (GetFileAttributesW(videoPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"[!] 视频文件不存在: %ls\n", videoPath.c_str());
        return 1;
    }

    // 检查 ffmpeg 
    wprintf(L"[*] 检测 ffmpeg 环境...\n");
    if (!IsFFmpegInstalled()) {
        wprintf(L"[!] 未找到 ffmpeg，尝试自动安装...\n");
        if (!InstallFFmpegWithWinget()) {
            wprintf(L"[!] 自动安装失败，请手动安装: winget install ffmpeg\n");
            return 1;
        }
        if (!IsFFmpegInstalled()) {
            wprintf(L"[!] 安装后仍无法检测到 ffmpeg，请检查 PATH\n");
            return 1;
        }
    }
    wprintf(L"[+] ffmpeg 已就绪\n");

    // 启动 ffplay 
    wprintf(L"[*] 启动视频播放 (循环)...\n");
    HANDLE ffplayProcess = StartFFPlay(videoPath);
    if (!ffplayProcess) {
        wprintf(L"[!] 启动 ffplay 失败\n");
        return 1;
    }

    HWND videoWnd = NULL;
    for (int i = 0; i < 10; ++i) {
        Sleep(500);
        videoWnd = FindFFPlayWindow();
        if (videoWnd) break;
    }
    if (!videoWnd) {
        wprintf(L"[!] 未找到 ffplay 窗口\n");
        TerminateProcess(ffplayProcess, 0);
        CloseHandle(ffplayProcess);
        return 1;
    }
    wprintf(L"[+] 找到视频窗口: 0x%p\n", videoWnd);

    // 获取 Progman 
    HWND hProgman = FindWindowW(L"Progman", NULL);
    if (!hProgman) {
        wprintf(L"[!] 未找到 Progman 窗口\n");
        TerminateProcess(ffplayProcess, 0);
        CloseHandle(ffplayProcess);
        return 1;
    }
    wprintf(L"[+] 找到 Progman: 0x%p\n", hProgman);

    // 发送 0x52C 
    wprintf(L"[*] 触发桌面窗口分裂 (0x52C)...\n");
    if (!SpawnWorkerW()) {
        wprintf(L"[!] 发送 0x52C 失败，可能系统不支持 (Win11 24H2+ 已修改)\n");
    }
    Sleep(500);

    // 将视频窗口挂到 Progman 
    wprintf(L"[*] 将视频窗口挂到 Progman...\n");
    LONG style = GetWindowLongW(videoWnd, GWL_STYLE);
    style &= ~(WS_POPUP | WS_CAPTION | WS_BORDER | WS_THICKFRAME);
    style |= WS_CHILD | WS_CLIPCHILDREN | WS_VISIBLE;
    SetWindowLongW(videoWnd, GWL_STYLE, style);

    SetParent(videoWnd, hProgman);
    RECT rect;
    GetClientRect(hProgman, &rect);
    SetWindowPos(videoWnd, HWND_TOP, 0, 0, rect.right, rect.bottom,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ShowWindow(videoWnd, SW_SHOW);
    wprintf(L"[+] 视频窗口已挂到 Progman\n");

    // 隐藏壁纸层 WorkerW 
    wprintf(L"[*] 隐藏壁纸层 WorkerW...\n");
    HideWallpaperWorkerW();

    wprintf(L"\n[+] 动态壁纸已启动！\n");
    wprintf(L"[*] 按 ESC 或 Q 退出程序\n\n");

    // 等待退出事件 
    bool shouldQuit = false;
    while (!shouldQuit) {
        if (IsKeyPressed(VK_ESCAPE) || IsKeyPressed('Q')) {
            shouldQuit = true;
            break;
        }

        DWORD exitCode;
        if (GetExitCodeProcess(ffplayProcess, &exitCode) && exitCode != STILL_ACTIVE) {
            wprintf(L"\n[*] ffplay 已退出 (代码: %lu)\n", exitCode);
            shouldQuit = true;
            break;
        }

        Sleep(200);
    }

    // 清理 
    wprintf(L"\n[*] 正在退出...\n");
    if (AskUserRestore()) {
        RestoreStaticWallpaper(ffplayProcess, videoWnd);
    } else {
        wprintf(L"[*] 保留当前壁纸设置\n");
        if (ffplayProcess) CloseHandle(ffplayProcess);
        // 如果保留，我们需要把 ffplay 窗口从 Progman 中分离，否则下次启动可能冲突。
        // 这里简单处理：将视频窗口设为桌面子窗口并隐藏（避免干扰）
        if (videoWnd && IsWindow(videoWnd)) {
            SetParent(videoWnd, GetDesktopWindow());
            ShowWindow(videoWnd, SW_HIDE);
        }
    }

    wprintf(L"[+] 程序结束\n");
    return 0;
}
