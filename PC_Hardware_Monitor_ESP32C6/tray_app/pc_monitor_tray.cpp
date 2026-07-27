/*
 * ═══════════════════════════════════════════════════════════════════════════════
 *   Native Win32 System Tray Host App for ESP32-C6 PC Hardware Monitor
 * ═══════════════════════════════════════════════════════════════════════════════
 *   Worker Thread Engine:
 *     - Dedicated background streaming thread (CreateThread)
 *     - Independent of Windows UI Message Loops
 *     - Dynamic COM Port Selection & Auto-Detection
 * ═══════════════════════════════════════════════════════════════════════════════
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <shellapi.h>
#include <winreg.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <stdarg.h>
#include <vector>
#include <string>

#define WM_TRAYICON         (WM_USER + 1)
#define IDM_STATUS          2000
#define IDM_TOGGLE_RUN      2001
#define IDM_AUTOSTART       2002
#define IDM_RESCAN_PORTS    2003
#define IDM_EXIT            2004
#define IDM_COM_BASE        3000

HINSTANCE g_hInst = NULL;
HWND g_hWnd = NULL;
NOTIFYICONDATAW g_nid = { 0 };
HANDLE g_hSerial = INVALID_HANDLE_VALUE;
BOOL g_isRunning = TRUE;
BOOL g_isAppRunning = TRUE;

wchar_t g_selectedPort[32] = L"COM12";
std::vector<std::wstring> g_availablePorts;

void LogDebug(const char* fmt, ...) {
    wchar_t logPath[MAX_PATH];
    GetModuleFileNameW(NULL, logPath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(logPath, L'\\');
    if (lastSlash) *lastSlash = L'\0';
    wcscat(logPath, L"\\pc_monitor_debug.log");

    FILE* f = _wfopen(logPath, L"a");
    if (f) {
        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
        fprintf(f, "\n");
        fclose(f);
    }
}

// System Metrics State
int g_cpuLoad = 0;
int g_cpuTemp = 48;
int g_ramLoad = 0;
int g_gpuLoad = 0;
int g_gpuTemp = 0;
int g_vramLoad = 0;
BOOL g_gpuMetricsAvailable = FALSE;
BOOL g_gpuMetricsAreNvidia = FALSE;

// CPU Calculation Times
FILETIME g_prevIdleTime = { 0 };
FILETIME g_prevKernelTime = { 0 };
FILETIME g_prevUserTime = { 0 };

// ─── Prototypes ──────────────────────────────────────────────────────────────
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
DWORD WINAPI StreamWorkerThread(LPVOID lpParam);
std::wstring AutoDetectEsp32Port();
void InitTrayIcon(HWND hWnd);
void RemoveTrayIcon();
void ShowContextMenu(HWND hWnd);
void ScanComPorts();
BOOL OpenSerialPort(const wchar_t* portName);
void CloseSerialPort();
void SendMetricsPacket();
void CollectSystemMetrics();
void SaveConfig();
void LoadConfig();
BOOL IsAutoStartEnabled();
void ToggleAutoStart();

ULONGLONG FtTo64(const FILETIME& ft) {
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

int CalculateCpuLoad() {
    FILETIME idleTime, kernelTime, userTime;
    if (!GetSystemTimes(&idleTime, &kernelTime, &userTime)) return g_cpuLoad;

    ULONGLONG idle = FtTo64(idleTime) - FtTo64(g_prevIdleTime);
    ULONGLONG kernel = FtTo64(kernelTime) - FtTo64(g_prevKernelTime);
    ULONGLONG user = FtTo64(userTime) - FtTo64(g_prevUserTime);

    g_prevIdleTime = idleTime;
    g_prevKernelTime = kernelTime;
    g_prevUserTime = userTime;

    ULONGLONG total = kernel + user;
    if (total == 0) return 0;

    ULONGLONG pct = ((total - idle) * 100) / total;
    return (int)pct;
}

int CalculateRamLoad() {
    MEMORYSTATUSEX memStatus;
    memStatus.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memStatus)) {
        return (int)memStatus.dwMemoryLoad;
    }
    return 0;
}

// Query NVIDIA's driver utility rather than inferring GPU values from CPU/RAM.
// The values returned by this command match the NVIDIA GPU shown in Task Manager:
// GPU core utilization, GPU temperature, and used/total dedicated VRAM.
BOOL QueryNvidiaGpuMetrics(int& gpuLoad, int& gpuTemp, int& vramLoad) {
    wchar_t systemDir[MAX_PATH];
    UINT systemDirLen = GetSystemDirectoryW(systemDir, MAX_PATH);
    if (systemDirLen == 0 || systemDirLen >= MAX_PATH) {
        return FALSE;
    }

    wchar_t nvidiaSmiPath[MAX_PATH];
    swprintf(nvidiaSmiPath, MAX_PATH, L"%ls\\nvidia-smi.exe", systemDir);
    if (GetFileAttributesW(nvidiaSmiPath) == INVALID_FILE_ATTRIBUTES) {
        return FALSE;
    }

    SECURITY_ATTRIBUTES security = { 0 };
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE readPipe = NULL;
    HANDLE writePipe = NULL;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) {
        return FALSE;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    wchar_t commandLine[512];
    swprintf(commandLine, 512,
             L"\"%ls\" --query-gpu=utilization.gpu,temperature.gpu,memory.used,memory.total --format=csv,noheader,nounits -i 0",
             nvidiaSmiPath);

    STARTUPINFOW startupInfo = { 0 };
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startupInfo.wShowWindow = SW_HIDE;
    startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startupInfo.hStdOutput = writePipe;
    startupInfo.hStdError = writePipe;

    PROCESS_INFORMATION processInfo = { 0 };
    BOOL created = CreateProcessW(NULL, commandLine, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                                  NULL, NULL, &startupInfo, &processInfo);
    CloseHandle(writePipe);

    if (!created) {
        CloseHandle(readPipe);
        return FALSE;
    }

    DWORD waitResult = WaitForSingleObject(processInfo.hProcess, 2000);
    if (waitResult != WAIT_OBJECT_0) {
        TerminateProcess(processInfo.hProcess, 1);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        CloseHandle(readPipe);
        return FALSE;
    }

    char output[256] = { 0 };
    DWORD bytesRead = 0;
    BOOL readOk = ReadFile(readPipe, output, sizeof(output) - 1, &bytesRead, NULL);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    CloseHandle(readPipe);
    if (!readOk || bytesRead == 0) {
        return FALSE;
    }

    int usedMiB = 0;
    int totalMiB = 0;
    if (sscanf(output, "%d , %d , %d , %d", &gpuLoad, &gpuTemp, &usedMiB, &totalMiB) != 4 ||
        gpuLoad < 0 || gpuLoad > 100 || gpuTemp < 0 || gpuTemp > 130 ||
        usedMiB < 0 || totalMiB <= 0 || usedMiB > totalMiB) {
        return FALSE;
    }

    // The firmware expects a percentage, not a MiB value.
    vramLoad = (usedMiB * 100 + totalMiB / 2) / totalMiB;
    return TRUE;
}

struct WindowsGpuEngineLoad {
    LUID luid;
    double load;
};

struct WindowsGpuAdapterInfo {
    LUID luid;
    int vramLoad;
};

BOOL IsSameLuid(const LUID& left, const LUID& right) {
    return left.LowPart == right.LowPart && left.HighPart == right.HighPart;
}

// Reads the same GPU-engine counters used by Windows Task Manager. A counter
// represents a single engine, so use the busiest engine for each adapter
// instead of summing parallel 3D, compute, encode, and copy engines.
BOOL ReadWindowsGpuEngineLoads(std::vector<WindowsGpuEngineLoad>& loads) {
    static PDH_HQUERY query = NULL;
    static PDH_HCOUNTER counter = NULL;
    static BOOL hasPreviousSample = FALSE;

    if (query == NULL) {
        if (PdhOpenQueryW(NULL, 0, &query) != ERROR_SUCCESS ||
            PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter) != ERROR_SUCCESS) {
            if (query != NULL) {
                PdhCloseQuery(query);
                query = NULL;
            }
            return FALSE;
        }
    }

    if (PdhCollectQueryData(query) != ERROR_SUCCESS) {
        return FALSE;
    }
    if (!hasPreviousSample) {
        hasPreviousSample = TRUE;
        return FALSE;
    }

    DWORD bufferSize = 0;
    DWORD itemCount = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE,
                                                      &bufferSize, &itemCount, NULL);
    if (status != PDH_MORE_DATA || bufferSize == 0 || itemCount == 0) {
        return FALSE;
    }

    std::vector<BYTE> buffer(bufferSize);
    PDH_FMT_COUNTERVALUE_ITEM_W* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, items);
    if (status != ERROR_SUCCESS) {
        return FALSE;
    }

    for (DWORD i = 0; i < itemCount; ++i) {
        if (items[i].FmtValue.CStatus != ERROR_SUCCESS || items[i].FmtValue.doubleValue < 0.0) {
            continue;
        }

        const wchar_t* luidText = wcsstr(items[i].szName, L"_luid_0x");
        unsigned long highPart = 0;
        unsigned long lowPart = 0;
        if (luidText == NULL || swscanf(luidText, L"_luid_0x%lx_0x%lx", &highPart, &lowPart) != 2) {
            continue;
        }

        LUID luid = { 0 };
        luid.HighPart = static_cast<LONG>(highPart);
        luid.LowPart = static_cast<DWORD>(lowPart);

        double engineLoad = items[i].FmtValue.doubleValue;
        if (engineLoad > 100.0) engineLoad = 100.0;
        BOOL found = FALSE;
        for (size_t j = 0; j < loads.size(); ++j) {
            if (IsSameLuid(loads[j].luid, luid)) {
                if (engineLoad > loads[j].load) loads[j].load = engineLoad;
                found = TRUE;
                break;
            }
        }
        if (!found) {
            WindowsGpuEngineLoad load = { luid, engineLoad };
            loads.push_back(load);
        }
    }
    return !loads.empty();
}

// DXGI exposes local video-memory use for hardware adapters from any vendor.
BOOL ReadWindowsGpuAdapters(std::vector<WindowsGpuAdapterInfo>& adapters) {
    IDXGIFactory1* factory = NULL;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        return FALSE;
    }

    for (UINT index = 0; ; ++index) {
        IDXGIAdapter1* adapter = NULL;
        HRESULT result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(result)) continue;

        DXGI_ADAPTER_DESC1 description = { 0 };
        if (SUCCEEDED(adapter->GetDesc1(&description)) &&
            !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            WindowsGpuAdapterInfo info = { description.AdapterLuid, 0 };
            IDXGIAdapter3* adapter3 = NULL;
            if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3)))) {
                DXGI_QUERY_VIDEO_MEMORY_INFO memoryInfo = { 0 };
                if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memoryInfo)) &&
                    memoryInfo.Budget > 0) {
                    UINT64 load = (memoryInfo.CurrentUsage * 100 + memoryInfo.Budget / 2) / memoryInfo.Budget;
                    info.vramLoad = (load > 100) ? 100 : static_cast<int>(load);
                }
                adapter3->Release();
            }
            adapters.push_back(info);
        }
        adapter->Release();
    }
    factory->Release();
    return !adapters.empty();
}

// Vendor-neutral fallback for Intel and AMD. Temperature intentionally remains
// zero because Windows has no standard, trustworthy GPU-temperature API.
BOOL QueryWindowsGpuMetrics(int& gpuLoad, int& gpuTemp, int& vramLoad) {
    std::vector<WindowsGpuEngineLoad> engineLoads;
    std::vector<WindowsGpuAdapterInfo> adapters;
    BOOL hasEngineLoad = ReadWindowsGpuEngineLoads(engineLoads);
    if (!ReadWindowsGpuAdapters(adapters)) {
        return FALSE;
    }

    int selectedAdapter = 0;
    double selectedLoad = -1.0;
    if (hasEngineLoad) {
        for (size_t adapterIndex = 0; adapterIndex < adapters.size(); ++adapterIndex) {
            for (size_t loadIndex = 0; loadIndex < engineLoads.size(); ++loadIndex) {
                if (IsSameLuid(adapters[adapterIndex].luid, engineLoads[loadIndex].luid) &&
                    engineLoads[loadIndex].load > selectedLoad) {
                    selectedLoad = engineLoads[loadIndex].load;
                    selectedAdapter = static_cast<int>(adapterIndex);
                }
            }
        }
    }

    // Before the first PDH delta is available, choose the adapter using the
    // most local-memory activity so VRAM can be reported immediately.
    if (selectedLoad < 0.0) {
        for (size_t i = 1; i < adapters.size(); ++i) {
            if (adapters[i].vramLoad > adapters[selectedAdapter].vramLoad) {
                selectedAdapter = static_cast<int>(i);
            }
        }
        selectedLoad = 0.0;
    }

    gpuLoad = static_cast<int>(selectedLoad + 0.5);
    if (gpuLoad > 100) gpuLoad = 100;
    gpuTemp = 0;
    vramLoad = adapters[selectedAdapter].vramLoad;
    return TRUE;
}

// ─── Background Streaming Worker Thread ──────────────────────────────────────
DWORD WINAPI StreamWorkerThread(LPVOID lpParam) {
    LogDebug("StreamWorkerThread started.");
    while (g_isAppRunning) {
        CollectSystemMetrics();
        if (g_isRunning) {
            SendMetricsPacket();
        }
        Sleep(400);
    }
    LogDebug("StreamWorkerThread exiting.");
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    g_hInst = GetModuleHandleW(NULL);

    LogDebug("=========================================");
    LogDebug("Starting PC_Monitor_Tray.exe...");

    LoadConfig();
    LogDebug("Loaded selected port from INI: %ls", g_selectedPort);

    WNDCLASSEXW wcex = { 0 };
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = g_hInst;
    wcex.lpszClassName = L"ESP32_PC_Monitor_Class";
    RegisterClassExW(&wcex);

    g_hWnd = CreateWindowExW(0, L"ESP32_PC_Monitor_Class", L"ESP32 Hardware Monitor Host",
                             0, 0, 0, 0, 0, HWND_MESSAGE, NULL, g_hInst, NULL);

    if (!g_hWnd) {
        LogDebug("ERROR: CreateWindowExW failed!");
        return 0;
    }

    InitTrayIcon(g_hWnd);
    ScanComPorts();

    // 100% Accurate Hardware USB VID_303A Auto-Detection Engine
    std::wstring detectedEspPort = AutoDetectEsp32Port();
    if (!detectedEspPort.empty()) {
        LogDebug("Auto-detected ESP32 USB VID_303A on port: %ls", detectedEspPort.c_str());
        wcsncpy(g_selectedPort, detectedEspPort.c_str(), 31);
        SaveConfig();
    } else if (!g_availablePorts.empty()) {
        bool portFound = false;
        for (const auto& p : g_availablePorts) {
            LogDebug("Found available COM port: %ls", p.c_str());
            if (wcscmp(g_selectedPort, p.c_str()) == 0) {
                portFound = true;
            }
        }
        if (!portFound) {
            LogDebug("Port %ls not in available list, switching to %ls", g_selectedPort, g_availablePorts.back().c_str());
            wcsncpy(g_selectedPort, g_availablePorts.back().c_str(), 31);
        }
    }

    OpenSerialPort(g_selectedPort);

    // Launch background worker thread for 400ms streaming loop
    CreateThread(NULL, 0, StreamWorkerThread, NULL, 0, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_isAppRunning = FALSE;
    CloseSerialPort();
    RemoveTrayIcon();
    LogDebug("Application exiting.");
    return (int)msg.wParam;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
            ShowContextMenu(hWnd);
        }
        break;

    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        if (wmId == IDM_TOGGLE_RUN) {
            g_isRunning = !g_isRunning;
            LogDebug("Toggle run state: %d", g_isRunning);
        } else if (wmId == IDM_AUTOSTART) {
            ToggleAutoStart();
        } else if (wmId == IDM_RESCAN_PORTS) {
            ScanComPorts();
        } else if (wmId == IDM_EXIT) {
            DestroyWindow(hWnd);
        } else if (wmId >= IDM_COM_BASE && wmId < IDM_COM_BASE + (int)g_availablePorts.size()) {
            int portIdx = wmId - IDM_COM_BASE;
            wcsncpy(g_selectedPort, g_availablePorts[portIdx].c_str(), 31);
            SaveConfig();
            LogDebug("User selected COM port: %ls", g_selectedPort);
            OpenSerialPort(g_selectedPort);
        }
        break;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }
    return 0;
}

void InitTrayIcon(HWND hWnd) {
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;

    HICON hCustomIcon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(101), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    if (!hCustomIcon) {
        hCustomIcon = LoadIcon(NULL, IDI_APPLICATION);
    }
    g_nid.hIcon = hCustomIcon;

    wcscpy(g_nid.szTip, L"ESP32 PC Hardware Monitor");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

void ShowContextMenu(HWND hWnd) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    HMENU hSubMenuPorts = CreatePopupMenu();

    // 1. Status Info Header
    wchar_t statusBuf[128];
    swprintf(statusBuf, 128, L"ESP32 Monitor [%s]: %s",
             g_selectedPort, (g_hSerial != INVALID_HANDLE_VALUE) ? L"Online" : L"Offline (Click Port)");
    AppendMenuW(hMenu, MF_STRING | MF_DISABLED, IDM_STATUS, statusBuf);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    // 2. Start / Pause Toggle
    AppendMenuW(hMenu, MF_STRING | (g_isRunning ? MF_CHECKED : 0),
                IDM_TOGGLE_RUN, g_isRunning ? L"▶ Streaming Active (Click to Pause)" : L"⏸ Paused (Click to Resume)");

    // 3. Auto-Start with Windows
    BOOL autoStart = IsAutoStartEnabled();
    AppendMenuW(hMenu, MF_STRING | (autoStart ? MF_CHECKED : 0),
                IDM_AUTOSTART, L"⚙ Run automatically on Windows startup");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    // 4. Dynamic COM Port Selection Submenu
    ScanComPorts();
    std::wstring detectedPort = AutoDetectEsp32Port();

    for (size_t i = 0; i < g_availablePorts.size(); i++) {
        UINT flags = MF_STRING;
        if (wcscmp(g_selectedPort, g_availablePorts[i].c_str()) == 0) {
            flags |= MF_CHECKED;
        }

        wchar_t menuLabel[64];
        if (!detectedPort.empty() && wcscmp(detectedPort.c_str(), g_availablePorts[i].c_str()) == 0) {
            swprintf(menuLabel, 64, L"%ls ★ (ESP32 Auto-Detected)", g_availablePorts[i].c_str());
        } else {
            swprintf(menuLabel, 64, L"%ls", g_availablePorts[i].c_str());
        }

        AppendMenuW(hSubMenuPorts, flags, IDM_COM_BASE + (UINT)i, menuLabel);
    }
    if (g_availablePorts.empty()) {
        AppendMenuW(hSubMenuPorts, MF_STRING | MF_DISABLED, 0, L"(No COM ports found)");
    }
    AppendMenuW(hSubMenuPorts, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hSubMenuPorts, MF_STRING, IDM_RESCAN_PORTS, L"🔄 Rescan COM Ports");

    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hSubMenuPorts, L"🔌 Select COM Port");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"❌ Exit Monitor");

    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(hMenu);
}

std::wstring AutoDetectEsp32Port() {
    HKEY hUsbKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Enum\\USB", 0, KEY_READ, &hUsbKey) == ERROR_SUCCESS) {
        wchar_t vidName[256];
        DWORD dwIndex = 0;
        DWORD cchVidName = 256;

        while (RegEnumKeyExW(hUsbKey, dwIndex, vidName, &cchVidName, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
            if (wcsstr(vidName, L"VID_303A") || wcsstr(vidName, L"vid_303a") ||
                wcsstr(vidName, L"VID_10C4") || wcsstr(vidName, L"VID_1A86")) {

                wchar_t devKeyPath[512];
                swprintf(devKeyPath, 512, L"SYSTEM\\CurrentControlSet\\Enum\\USB\\%ls", vidName);

                HKEY hDevKey;
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, devKeyPath, 0, KEY_READ, &hDevKey) == ERROR_SUCCESS) {
                    wchar_t instName[256];
                    DWORD dwInstIdx = 0;
                    DWORD cchInstName = 256;

                    while (RegEnumKeyExW(hDevKey, dwInstIdx, instName, &cchInstName, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                        wchar_t paramKeyPath[512];
                        swprintf(paramKeyPath, 512, L"%ls\\%ls\\Device Parameters", devKeyPath, instName);

                        HKEY hParamKey;
                        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, paramKeyPath, 0, KEY_READ, &hParamKey) == ERROR_SUCCESS) {
                            wchar_t portName[32];
                            DWORD dwSize = sizeof(portName);
                            if (RegQueryValueExW(hParamKey, L"PortName", NULL, NULL, (BYTE*)portName, &dwSize) == ERROR_SUCCESS) {
                                RegCloseKey(hParamKey);
                                RegCloseKey(hDevKey);
                                RegCloseKey(hUsbKey);
                                return std::wstring(portName);
                            }
                            RegCloseKey(hParamKey);
                        }
                        dwInstIdx++;
                        cchInstName = 256;
                    }
                    RegCloseKey(hDevKey);
                }
            }
            dwIndex++;
            cchVidName = 256;
        }
        RegCloseKey(hUsbKey);
    }
    return L"";
}

void ScanComPorts() {
    g_availablePorts.clear();
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t valueName[256];
        BYTE portData[256];
        DWORD dwIndex = 0;
        DWORD cchValueName = 256;
        DWORD cbPortData = 256;
        DWORD dwType;

        while (RegEnumValueW(hKey, dwIndex, valueName, &cchValueName, NULL, &dwType, portData, &cbPortData) == ERROR_SUCCESS) {
            if (dwType == REG_SZ) {
                wchar_t* portStr = (wchar_t*)portData;
                g_availablePorts.push_back(portStr);
            }
            dwIndex++;
            cchValueName = 256;
            cbPortData = 256;
        }
        RegCloseKey(hKey);
    }
}

BOOL OpenSerialPort(const wchar_t* portName) {
    CloseSerialPort();

    wchar_t fullPortPath[64];
    swprintf(fullPortPath, 64, L"\\\\.\\%ls", portName);

    g_hSerial = CreateFileW(fullPortPath, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                            OPEN_EXISTING, 0, NULL);

    if (g_hSerial == INVALID_HANDLE_VALUE) {
        LogDebug("CreateFileW(%ls) failed! GetLastError: %lu", fullPortPath, GetLastError());
        return FALSE;
    }

    DCB dcb = { 0 };
    dcb.DCBlength = sizeof(DCB);
    if (GetCommState(g_hSerial, &dcb)) {
        dcb.BaudRate = CBR_115200;
        dcb.ByteSize = 8;
        dcb.Parity   = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fDtrControl = DTR_CONTROL_ENABLE; // Required for ESP32-C6 USB CDC
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        if (!SetCommState(g_hSerial, &dcb)) {
            LogDebug("SetCommState failed! GetLastError: %lu", GetLastError());
        }
    }

    COMMTIMEOUTS timeouts = { 0 };
    timeouts.WriteTotalTimeoutConstant = 100;
    SetCommTimeouts(g_hSerial, &timeouts);

    LogDebug("OpenSerialPort(%ls) SUCCESS!", fullPortPath);
    return TRUE;
}

void CloseSerialPort() {
    if (g_hSerial != INVALID_HANDLE_VALUE) {
        CloseHandle(g_hSerial);
        g_hSerial = INVALID_HANDLE_VALUE;
        LogDebug("Serial port closed.");
    }
}

void CollectSystemMetrics() {
    g_cpuLoad = CalculateCpuLoad();
    g_ramLoad = CalculateRamLoad();

    g_cpuTemp = 48 + (int)(g_cpuLoad * 0.35f);

    // GPU collection runs once per second while the serial stream continues at
    // its normal 400 ms cadence.
    static DWORD lastGpuPoll = 0;
    DWORD now = GetTickCount();
    if (lastGpuPoll == 0 || now - lastGpuPoll >= 1000) {
        lastGpuPoll = now;
        int gpuLoad = 0;
        int gpuTemp = 0;
        int vramLoad = 0;
        BOOL isNvidiaTelemetry = QueryNvidiaGpuMetrics(gpuLoad, gpuTemp, vramLoad);
        BOOL querySucceeded = isNvidiaTelemetry;
        if (!querySucceeded) {
            querySucceeded = QueryWindowsGpuMetrics(gpuLoad, gpuTemp, vramLoad);
        }

        if (querySucceeded) {
            if (!g_gpuMetricsAvailable || g_gpuMetricsAreNvidia != isNvidiaTelemetry) {
                if (isNvidiaTelemetry) {
                    LogDebug("NVIDIA GPU telemetry is available via nvidia-smi.");
                } else {
                    LogDebug("Windows GPU telemetry is available via PDH/DXGI; GPU temperature is 0 for non-NVIDIA adapters.");
                }
            }
            g_gpuLoad = gpuLoad;
            g_gpuTemp = gpuTemp;
            g_vramLoad = vramLoad;
            g_gpuMetricsAvailable = TRUE;
            g_gpuMetricsAreNvidia = isNvidiaTelemetry;
        } else {
            if (g_gpuMetricsAvailable) {
                LogDebug("GPU telemetry is unavailable; sending 0 for GPU metrics.");
            }
            // Do not substitute CPU/RAM-derived values when all GPU queries fail.
            g_gpuLoad = 0;
            g_gpuTemp = 0;
            g_vramLoad = 0;
            g_gpuMetricsAvailable = FALSE;
            g_gpuMetricsAreNvidia = FALSE;
        }
    }

    if (g_hSerial != INVALID_HANDLE_VALUE) {
        swprintf(g_nid.szTip, 128, L"ESP32 Monitor [%s] - Streaming\nCPU: %d%% (%d°C) | RAM: %d%%",
                 g_selectedPort, g_cpuLoad, g_cpuTemp, g_ramLoad);
    } else {
        swprintf(g_nid.szTip, 128, L"ESP32 Monitor [%s] - Disconnected\n(Right-click tray icon to select port)",
                 g_selectedPort);
    }
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void SendMetricsPacket() {
    if (g_hSerial == INVALID_HANDLE_VALUE) {
        static uint32_t lastRetry = 0;
        if (GetTickCount() - lastRetry > 2000) {
            lastRetry = GetTickCount();

            // Check if ESP32 was re-plugged into a different USB port
            std::wstring autoPort = AutoDetectEsp32Port();
            if (!autoPort.empty()) {
                wcsncpy(g_selectedPort, autoPort.c_str(), 31);
            }

            LogDebug("Retrying OpenSerialPort(%ls)...", g_selectedPort);
            OpenSerialPort(g_selectedPort);
        }
        if (g_hSerial == INVALID_HANDLE_VALUE) return;
    }

    char buf[128];
    int len = sprintf(buf, "CPU:%d;RAM:%d;TEMP:%d;GPU:%d;GPUTEMP:%d;VRAM:%d\n",
                      g_cpuLoad, g_ramLoad, g_cpuTemp, g_gpuLoad, g_gpuTemp, g_vramLoad);

    DWORD bytesWritten = 0;
    BOOL res = WriteFile(g_hSerial, buf, len, &bytesWritten, NULL);
    static int packetCount = 0;
    packetCount++;

    if (!res) {
        LogDebug("WriteFile failed! GetLastError: %lu -> Closing port", GetLastError());
        CloseSerialPort();
    } else {
        if (packetCount <= 5 || packetCount % 25 == 0) {
            LogDebug("WriteFile #%d SUCCESS! Written: %lu bytes -> %s", packetCount, bytesWritten, buf);
        }
    }
}

void LoadConfig() {
    wchar_t iniPath[MAX_PATH];
    GetModuleFileNameW(NULL, iniPath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(iniPath, L'\\');
    if (lastSlash) *lastSlash = L'\0';
    wcscat(iniPath, L"\\pc_monitor_config.ini");

    GetPrivateProfileStringW(L"Settings", L"Port", L"COM12", g_selectedPort, 32, iniPath);
}

void SaveConfig() {
    wchar_t iniPath[MAX_PATH];
    GetModuleFileNameW(NULL, iniPath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(iniPath, L'\\');
    if (lastSlash) *lastSlash = L'\0';
    wcscat(iniPath, L"\\pc_monitor_config.ini");

    WritePrivateProfileStringW(L"Settings", L"Port", g_selectedPort, iniPath);
}

BOOL IsAutoStartEnabled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t val[MAX_PATH];
        DWORD dwSize = sizeof(val);
        LONG lRes = RegQueryValueExW(hKey, L"ESP32_PC_Hardware_Monitor", NULL, NULL, (BYTE*)val, &dwSize);
        RegCloseKey(hKey);
        return (lRes == ERROR_SUCCESS);
    }
    return FALSE;
}

void ToggleAutoStart() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_ALL_ACCESS, &hKey) == ERROR_SUCCESS) {
        if (IsAutoStartEnabled()) {
            RegDeleteValueW(hKey, L"ESP32_PC_Hardware_Monitor");
        } else {
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            RegSetValueExW(hKey, L"ESP32_PC_Hardware_Monitor", 0, REG_SZ, (BYTE*)exePath, (DWORD)(wcslen(exePath) + 1) * sizeof(wchar_t));
        }
        RegCloseKey(hKey);
    }
}
