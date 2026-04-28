#include "whatismyip-tb.h"
#include "resource.h"

#include <iphlpapi.h>
#include <netioapi.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace
{
    constexpr wchar_t kWindowClassName[] = L"WhatIsMyIpTrayWindow";
    constexpr wchar_t kLookupHost[] = L"api.ipify.org";
    constexpr wchar_t kLookupPath[] = L"/";

    HWND g_window = nullptr;
    HICON g_icon = nullptr;
    bool g_ownsIcon = false;
    HANDLE g_interfaceNotification = nullptr;
    HANDLE g_routeNotification = nullptr;

    std::mutex g_stateMutex;
    std::wstring g_currentIp = L"Updating...";
    std::wstring g_lastUpdated = L"Never";

    std::mutex g_workerMutex;
    std::condition_variable g_workerWake;
    bool g_fetchRequested = false;
    bool g_stopRequested = false;
    std::thread g_worker;

    std::wstring Utf8ToWide(const std::string& text)
    {
        if (text.empty())
        {
            return {};
        }

        const int required = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (required <= 0)
        {
            return {};
        }

        std::wstring result(static_cast<size_t>(required), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), required);
        return result;
    }

    std::wstring Trim(std::wstring value)
    {
        const auto isSpace = [](wchar_t ch) { return std::iswspace(ch) != 0; };

        value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), isSpace));
        value.erase(std::find_if_not(value.rbegin(), value.rend(), isSpace).base(), value.end());
        return value;
    }

    bool IsLikelyIpv4(std::wstring_view value)
    {
        int dots = 0;
        int segmentDigits = 0;
        int segmentValue = 0;

        for (const wchar_t ch : value)
        {
            if (ch == L'.')
            {
                if (segmentDigits == 0 || segmentValue > 255)
                {
                    return false;
                }

                ++dots;
                segmentDigits = 0;
                segmentValue = 0;
                continue;
            }

            if (ch < L'0' || ch > L'9')
            {
                return false;
            }

            segmentValue = (segmentValue * 10) + (ch - L'0');
            ++segmentDigits;
            if (segmentDigits > 3)
            {
                return false;
            }
        }

        return dots == 3 && segmentDigits > 0 && segmentValue <= 255;
    }

    std::wstring NowText()
    {
        SYSTEMTIME localTime{};
        GetLocalTime(&localTime);

        wchar_t buffer[64]{};
        swprintf_s(
            buffer,
            L"%04u-%02u-%02u %02u:%02u:%02u",
            localTime.wYear,
            localTime.wMonth,
            localTime.wDay,
            localTime.wHour,
            localTime.wMinute,
            localTime.wSecond);
        return buffer;
    }

    std::wstring MakeTooltip()
    {
        std::lock_guard lock(g_stateMutex);
        std::wstring tooltip = L"IPv4: " + g_currentIp + L"\nUpdated: " + g_lastUpdated;
        if (tooltip.size() > 127)
        {
            tooltip.resize(127);
        }
        return tooltip;
    }

    void UpdateTrayTooltip()
    {
        if (!g_window)
        {
            return;
        }

        NOTIFYICONDATA nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = g_window;
        nid.uID = ID_TRAY_ICON;
        nid.uFlags = NIF_TIP;

        const std::wstring tooltip = MakeTooltip();
        wcscpy_s(nid.szTip, tooltip.c_str());
        Shell_NotifyIcon(NIM_MODIFY, &nid);
    }

    void SetIpStatus(std::wstring ip, std::wstring updatedAt)
    {
        {
            std::lock_guard lock(g_stateMutex);
            g_currentIp = std::move(ip);
            g_lastUpdated = std::move(updatedAt);
        }

        PostMessage(g_window, WM_NETWORK_CHANGED, 0, 0);
    }

    std::wstring FetchExternalIpv4()
    {
        HINTERNET session = WinHttpOpen(
            L"whatismyip-tb/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
        if (!session)
        {
            return {};
        }

        WinHttpSetTimeouts(session, 3000, 3000, 5000, 5000);

        HINTERNET connection = WinHttpConnect(session, kLookupHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!connection)
        {
            WinHttpCloseHandle(session);
            return {};
        }

        HINTERNET request = WinHttpOpenRequest(
            connection,
            L"GET",
            kLookupPath,
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE);
        if (!request)
        {
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return {};
        }

        std::wstring result;
        if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr))
        {
            DWORD statusCode = 0;
            DWORD statusSize = sizeof(statusCode);
            WinHttpQueryHeaders(
                request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &statusCode,
                &statusSize,
                WINHTTP_NO_HEADER_INDEX);

            if (statusCode == 200)
            {
                std::string response;
                DWORD available = 0;
                while (WinHttpQueryDataAvailable(request, &available) && available > 0)
                {
                    std::string chunk(static_cast<size_t>(available), '\0');
                    DWORD read = 0;
                    if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0)
                    {
                        break;
                    }
                    chunk.resize(read);
                    response += chunk;
                }

                result = Trim(Utf8ToWide(response));
                if (!IsLikelyIpv4(result))
                {
                    result.clear();
                }
            }
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return result;
    }

    void RequestFetch()
    {
        {
            std::lock_guard lock(g_workerMutex);
            g_fetchRequested = true;
        }
        g_workerWake.notify_one();
    }

    void WorkerProc()
    {
        std::unique_lock lock(g_workerMutex);

        while (!g_stopRequested)
        {
            g_workerWake.wait(lock, [] { return g_fetchRequested || g_stopRequested; });
            if (g_stopRequested)
            {
                break;
            }

            g_fetchRequested = false;
            g_workerWake.wait_for(lock, std::chrono::milliseconds(1500), [] { return g_stopRequested; });
            if (g_stopRequested)
            {
                break;
            }

            lock.unlock();
            const std::wstring ip = FetchExternalIpv4();
            SetIpStatus(ip.empty() ? L"Unavailable" : ip, ip.empty() ? L"Lookup failed" : NowText());
            lock.lock();
        }
    }

    void CALLBACK InterfaceChanged(PVOID, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE)
    {
        RequestFetch();
    }

    void CALLBACK RouteChanged(PVOID, PMIB_IPFORWARD_ROW2, MIB_NOTIFICATION_TYPE)
    {
        RequestFetch();
    }

    void CopyTextToClipboard(HWND owner, const std::wstring& text)
    {
        if (text.empty() || text == L"Updating..." || text == L"Unavailable")
        {
            return;
        }

        if (!OpenClipboard(owner))
        {
            return;
        }

        EmptyClipboard();

        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory)
        {
            void* target = GlobalLock(memory);
            if (target)
            {
                memcpy(target, text.c_str(), bytes);
                GlobalUnlock(memory);
                SetClipboardData(CF_UNICODETEXT, memory);
                memory = nullptr;
            }
        }

        if (memory)
        {
            GlobalFree(memory);
        }

        CloseClipboard();
    }

    void ShowTrayMenu(HWND window)
    {
        std::wstring ip;
        {
            std::lock_guard lock(g_stateMutex);
            ip = g_currentIp;
        }

        HMENU menu = CreatePopupMenu();
        if (!menu)
        {
            return;
        }

        std::wstring copyLabel = L"Copy IPv4: " + ip;
        AppendMenu(menu, MF_STRING, ID_MENU_COPY_IP, copyLabel.c_str());
        AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenu(menu, MF_STRING, ID_MENU_EXIT, L"Exit");

        POINT cursor{};
        GetCursorPos(&cursor);
        SetForegroundWindow(window);

        const UINT command = TrackPopupMenu(
            menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
            cursor.x,
            cursor.y,
            0,
            window,
            nullptr);

        DestroyMenu(menu);

        if (command == ID_MENU_COPY_IP)
        {
            CopyTextToClipboard(window, ip);
        }
        else if (command == ID_MENU_EXIT)
        {
            DestroyWindow(window);
        }
    }

    bool AddTrayIcon(HWND window)
    {
        g_icon = static_cast<HICON>(LoadImage(
            GetModuleHandle(nullptr),
            MAKEINTRESOURCE(IDI_APP_ICON),
            IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON),
            LR_DEFAULTCOLOR));
        if (!g_icon)
        {
            g_icon = LoadIcon(nullptr, IDI_APPLICATION);
        }
        else
        {
            g_ownsIcon = true;
        }

        NOTIFYICONDATA nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = window;
        nid.uID = ID_TRAY_ICON;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_TRAY_ICON;
        nid.hIcon = g_icon;

        const std::wstring tooltip = MakeTooltip();
        wcscpy_s(nid.szTip, tooltip.c_str());

        return Shell_NotifyIcon(NIM_ADD, &nid) != FALSE;
    }

    void RemoveTrayIcon(HWND window)
    {
        NOTIFYICONDATA nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = window;
        nid.uID = ID_TRAY_ICON;
        Shell_NotifyIcon(NIM_DELETE, &nid);
    }

    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
            g_window = window;
            AddTrayIcon(window);

            g_worker = std::thread(WorkerProc);
            NotifyIpInterfaceChange(AF_UNSPEC, InterfaceChanged, nullptr, FALSE, &g_interfaceNotification);
            NotifyRouteChange2(AF_UNSPEC, RouteChanged, nullptr, FALSE, &g_routeNotification);
            RequestFetch();
            return 0;

        case WM_TRAY_ICON:
            if (wParam == ID_TRAY_ICON &&
                (LOWORD(lParam) == WM_LBUTTONUP || LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU))
            {
                ShowTrayMenu(window);
            }
            return 0;

        case WM_NETWORK_CHANGED:
            UpdateTrayTooltip();
            return 0;

        case WM_DESTROY:
            if (g_interfaceNotification)
            {
                CancelMibChangeNotify2(g_interfaceNotification);
                g_interfaceNotification = nullptr;
            }
            if (g_routeNotification)
            {
                CancelMibChangeNotify2(g_routeNotification);
                g_routeNotification = nullptr;
            }

            {
                std::lock_guard lock(g_workerMutex);
                g_stopRequested = true;
            }
            g_workerWake.notify_one();
            if (g_worker.joinable())
            {
                g_worker.join();
            }

            RemoveTrayIcon(window);
            if (g_icon && g_ownsIcon)
            {
                DestroyIcon(g_icon);
                g_icon = nullptr;
                g_ownsIcon = false;
            }
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProc(window, message, wParam, lParam);
        }
    }

    bool RegisterTrayWindowClass(HINSTANCE instance)
    {
        WNDCLASSEX windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = kWindowClassName;

        return RegisterClassEx(&windowClass) != 0;
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    if (!RegisterTrayWindowClass(instance))
    {
        return 1;
    }

    HWND window = CreateWindowEx(
        0,
        kWindowClassName,
        L"What Is My IP",
        WS_OVERLAPPED,
        0,
        0,
        0,
        0,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (!window)
    {
        return 1;
    }

    MSG message{};
    while (GetMessage(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessage(&message);
    }

    return static_cast<int>(message.wParam);
}
