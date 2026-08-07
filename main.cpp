// video_wallpaper.cpp (mpv 版本，交互式菜单，无拖拽提示)
// 编译: g++ -municode -DUNICODE -D_UNICODE -o wallpaper.exe main.cpp -luser32 -lshell32 -lshlwapi

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <string>
#include <cstdio>
#include <cwchar>
#include <vector>
#include <iostream>

// 全局变量
static HWND g_hiddenWorkerW = NULL;
static HANDLE g_mpvProcess = NULL;
static HWND g_mpvWindow = NULL;
static bool g_isWallpaperMode = false;

// 辅助函数，查找mpv.exe
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
    wprintf(L"[*] 正在使用 winget 安装 mpv...\n");
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    wchar_t cmd[] = L"winget install mpv --accept-package-agreements --accept-source-agreements";
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (!ok) {
        wprintf(L"[!] winget 启动失败，请手动安装 mpv (winget install mpv)\n");
        return false;
    }
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD exitCode;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (exitCode == 0) {
        wprintf(L"[+] mpv 安装成功\n");
        return true;
    } else {
        wprintf(L"[!] mpv 安装失败，请手动安装 (winget install mpv 或从官网下载)\n");
        return false;
    }
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

// 启动mpv
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
        wprintf(L"[!] 启动 mpv 失败 (错误码: %lu)\n", GetLastError());
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
        wprintf(L"[!] 未找到 mpv 窗口（进程 PID=%lu）\n", pid);
        TerminateProcess(hProcess, 0);
        CloseHandle(hProcess);
        return NULL;
    }

    ShowWindow(hwnd, SW_HIDE);
    wprintf(L"[+] 找到 mpv 窗口并已隐藏: 0x%p\n", hwnd);

    *outWindow = hwnd;
    return hProcess;
}

// 隐藏/恢复WorkerW
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

    HWND hWorkerWallpaper = FindWindowExW(NULL, hWorkerIcon, L"WorkerW", NULL);
    if (hWorkerWallpaper) {
        ShowWindow(hWorkerWallpaper, SW_HIDE);
        g_hiddenWorkerW = hWorkerWallpaper;
        wprintf(L"[调试] 已隐藏壁纸层 WorkerW (0x%p)\n", hWorkerWallpaper);
    } else {
        wprintf(L"[!] 未找到壁纸层 WorkerW\n");
        g_hiddenWorkerW = NULL;
    }
}

void ShowWallpaperWorkerW() {
    if (g_hiddenWorkerW && IsWindow(g_hiddenWorkerW)) {
        ShowWindow(g_hiddenWorkerW, SW_SHOW);
        wprintf(L"[调试] 已恢复壁纸层 WorkerW (0x%p)\n", g_hiddenWorkerW);
        g_hiddenWorkerW = NULL;
    } else {
        wprintf(L"[!] 没有找到之前隐藏的 WorkerW\n");
    }
}

// 设置壁纸
bool SetupWallpaper(const std::wstring& videoPath) {
    if (g_isWallpaperMode) {
        wprintf(L"[*] 正在更换壁纸，终止旧的 mpv...\n");
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

    wprintf(L"[*] 启动 mpv (循环播放)...\n");
    g_mpvProcess = StartAndWaitForMPV(videoPath, &g_mpvWindow);
    if (!g_mpvProcess) {
        wprintf(L"[!] 启动 mpv 失败\n");
        return false;
    }

    HWND hProgman = FindWindowW(L"Progman", NULL);
    if (!hProgman) {
        wprintf(L"[!] 未找到 Progman 窗口\n");
        if (g_mpvProcess) { TerminateProcess(g_mpvProcess, 0); CloseHandle(g_mpvProcess); g_mpvProcess = NULL; }
        return false;
    }
    wprintf(L"[+] 找到 Progman: 0x%p\n", hProgman);

    wprintf(L"[*] 触发桌面窗口分裂 (0x52C)...\n");
    if (!SpawnWorkerW()) {
        wprintf(L"[!] 发送 0x52C 失败，可能系统不支持 (Win11 24H2+ 已修改)\n");
    }
    Sleep(500);

    wprintf(L"[*] 将 mpv 窗口挂到 Progman...\n");
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
    wprintf(L"[+] mpv 窗口已挂到 Progman 并显示\n");

    wprintf(L"[*] 隐藏壁纸层 WorkerW...\n");
    HideWallpaperWorkerW();

    g_isWallpaperMode = true;
    wprintf(L"\n[+] 动态壁纸已启动！\n");
    return true;
}

// 恢复壁纸
void RestoreWallpaper() {
    wprintf(L"[*] 正在恢复静态壁纸...\n");
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
    wprintf(L"[+] 已恢复静态壁纸\n");
}

// 获取视频路径
std::wstring GetVideoPathFromUser() {
    wprintf(L"\n请输入视频文件路径: ");
    std::wstring path;
    std::getline(std::wcin, path);
    // 去除首尾引号（如果有）
    if (!path.empty() && path.front() == L'\"') path.erase(0, 1);
    if (!path.empty() && path.back() == L'\"') path.pop_back();
    return path;
}

// 显示菜单
void ShowMenu() {
    wprintf(L"\n========================================\n");
    wprintf(L"  动态壁纸菜单\n");
    wprintf(L"========================================\n");
    wprintf(L"1. 恢复静态壁纸（程序继续运行）\n");
    wprintf(L"2. 退出程序（保留动态壁纸）\n");
    wprintf(L"3. 退出程序并恢复静态壁纸\n");
    wprintf(L"4. 设置新壁纸\n");
    wprintf(L"请选择 (1-4): ");
}

// 主函数
int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    setlocale(LC_ALL, "zh-CN.UTF-8");

    wprintf(L"========================================\n");
    wprintf(L"  视频壁纸工具 (mpv) - 交互式菜单版\n");
    wprintf(L"========================================\n\n");

    // 检测mpv
    wprintf(L"[*] 检测 mpv 环境...\n");
    if (!IsMPVInstalled()) {
        wprintf(L"[!] 未找到 mpv，尝试自动安装...\n");
        if (!InstallMPVWithWinget()) {
            wprintf(L"[!] 自动安装失败，请手动安装 mpv (winget install mpv 或从官网下载)\n");
            return 1;
        }
        if (!IsMPVInstalled()) {
            wprintf(L"[!] 安装后仍无法找到 mpv，请确保已添加到 PATH 或安装到标准目录\n");
            return 1;
        }
    }
    wprintf(L"[+] mpv 已就绪 (路径: %ls)\n", FindMPVExe().c_str());

    // 主循环
    bool shouldExit = false;
    while (!shouldExit) {
        std::wstring videoPath;

        if (argc >= 2) {
            videoPath = argv[1];
            argc = 1; // 只使用一次
        } else {
            videoPath = GetVideoPathFromUser();
            if (videoPath.empty()) {
                wprintf(L"[!] 路径为空，请重新输入或按 Ctrl+C 退出\n");
                continue;
            }
        }

        if (GetFileAttributesW(videoPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            wprintf(L"[!] 视频文件不存在: %ls\n", videoPath.c_str());
            if (argc == 1) {
                wprintf(L"[!] 无效的视频文件，程序退出\n");
                return 1;
            }
            continue;
        }

        if (!SetupWallpaper(videoPath)) {
            wprintf(L"[!] 设置壁纸失败\n");
            if (argc == 1) return 1;
            continue;
        }

        // 进入菜单
        bool menuLoop = true;
        while (menuLoop) {
            ShowMenu();
            int choice;
            std::wcin >> choice;
            if (std::wcin.fail()) {
                std::wcin.clear();
                std::wcin.ignore(1024, L'\n');
                wprintf(L"[!] 输入无效，请输入数字 1-4\n");
                continue;
            }

            switch (choice) {
                case 1:
                    RestoreWallpaper();
                    wprintf(L"[*] 已恢复静态壁纸，您可以再次设置新壁纸。\n");
                    menuLoop = false;
                    break;

                case 2:
                    wprintf(L"[*] 退出程序，保留动态壁纸效果。\n");
                    shouldExit = true;
                    menuLoop = false;
                    break;

                case 3:
                    RestoreWallpaper();
                    wprintf(L"[*] 已恢复静态壁纸，程序退出。\n");
                    shouldExit = true;
                    menuLoop = false;
                    break;

                case 4:
                    wprintf(L"[*] 正在更换壁纸...\n");
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
                    menuLoop = false;
                    break;

                default:
                    wprintf(L"[!] 无效选项，请输入 1-4\n");
                    break;
            }
        }

        if (shouldExit) break;
    }

    // 安全清理（以防残留）
    if (g_mpvProcess && !shouldExit) {
        // 这种情况不会发生，但保留
        TerminateProcess(g_mpvProcess, 0);
        CloseHandle(g_mpvProcess);
        g_mpvProcess = NULL;
    }

    wprintf(L"[+] 程序结束\n");
    return 0;
}
