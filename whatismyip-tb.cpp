#include "whatismyip-tb.h"
#include "resource.h"

#include <iphlpapi.h>
#include <netioapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
    constexpr wchar_t kWindowClassName[] = L"WhatIsMyIpTrayWindow";
    constexpr wchar_t kAppDisplayName[] = L"What Is My IP";
    constexpr wchar_t kInstallFolderName[] = L"What Is My IP";
    constexpr wchar_t kExecutableName[] = L"whatismyip-tb.exe";
    constexpr wchar_t kStartupShortcutName[] = L"What Is My IP.lnk";
    constexpr wchar_t kStartupArgument[] = L"--startup";
    constexpr wchar_t kUninstallArgument[] = L"--uninstall";
    constexpr wchar_t kUninstallKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\WhatIsMyIpTb";
    constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\WhatIsMyIpTb.SingleInstance";
    constexpr wchar_t kLookupHost[] = L"api.ipify.org";
    constexpr wchar_t kLookupPath[] = L"/";
    constexpr auto kNetworkChangeDebounce = std::chrono::milliseconds(1500);
    constexpr auto kFirstRetryDelay = std::chrono::seconds(30);
    constexpr auto kLaterRetryDelay = std::chrono::minutes(2);

    HWND g_window = nullptr;
    HICON g_icon = nullptr;
    bool g_ownsIcon = false;
    HANDLE g_singleInstanceMutex = nullptr;
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

    bool AcquireSingleInstance()
    {
        g_singleInstanceMutex = CreateMutex(nullptr, TRUE, kSingleInstanceMutexName);
        if (!g_singleInstanceMutex)
        {
            return false;
        }

        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            CloseHandle(g_singleInstanceMutex);
            g_singleInstanceMutex = nullptr;
            return false;
        }

        return true;
    }

    void ReleaseSingleInstance()
    {
        if (g_singleInstanceMutex)
        {
            ReleaseMutex(g_singleInstanceMutex);
            CloseHandle(g_singleInstanceMutex);
            g_singleInstanceMutex = nullptr;
        }
    }

    bool StopRunningInstanceForUninstall()
    {
        HANDLE existingMutex = OpenMutex(SYNCHRONIZE, FALSE, kSingleInstanceMutexName);
        if (!existingMutex)
        {
            return true;
        }

        HWND existingWindow = FindWindow(kWindowClassName, kAppDisplayName);
        if (existingWindow)
        {
            PostMessage(existingWindow, WM_CLOSE, 0, 0);
        }

        const DWORD waitResult = WaitForSingleObject(existingMutex, 7000);
        CloseHandle(existingMutex);
        return waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED;
    }

    std::wstring GetModulePath()
    {
        std::wstring path(MAX_PATH, L'\0');
        DWORD length = GetModuleFileName(nullptr, path.data(), static_cast<DWORD>(path.size()));
        while (length == path.size())
        {
            path.resize(path.size() * 2);
            length = GetModuleFileName(nullptr, path.data(), static_cast<DWORD>(path.size()));
        }

        path.resize(length);
        return path;
    }

    std::wstring GetKnownFolder(REFKNOWNFOLDERID folderId)
    {
        PWSTR rawPath = nullptr;
        if (FAILED(SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, nullptr, &rawPath)))
        {
            return {};
        }

        std::wstring path = rawPath;
        CoTaskMemFree(rawPath);
        return path;
    }

    std::wstring GetEnvironmentString(const wchar_t* name)
    {
        const DWORD required = GetEnvironmentVariable(name, nullptr, 0);
        if (required == 0)
        {
            return {};
        }

        std::wstring value(required, L'\0');
        const DWORD written = GetEnvironmentVariable(name, value.data(), required);
        if (written == 0 || written >= required)
        {
            return {};
        }

        value.resize(written);
        return value;
    }

    std::wstring JoinPath(std::wstring_view left, std::wstring_view right)
    {
        if (left.empty())
        {
            return std::wstring(right);
        }

        std::wstring result(left);
        if (result.back() != L'\\' && result.back() != L'/')
        {
            result += L'\\';
        }
        result += right;
        return result;
    }

    std::wstring InstallFolder()
    {
        std::wstring localAppData = GetEnvironmentString(L"LOCALAPPDATA");
        if (localAppData.empty())
        {
            localAppData = GetKnownFolder(FOLDERID_LocalAppData);
        }
        if (localAppData.empty())
        {
            return {};
        }

        return JoinPath(JoinPath(localAppData, L"Programs"), kInstallFolderName);
    }

    std::wstring InstalledExecutablePath()
    {
        const std::wstring folder = InstallFolder();
        if (folder.empty())
        {
            return {};
        }

        return JoinPath(folder, kExecutableName);
    }

    std::wstring StartupShortcutPath()
    {
        std::wstring startupFolder = GetEnvironmentString(L"APPDATA");
        if (!startupFolder.empty())
        {
            startupFolder = JoinPath(startupFolder, L"Microsoft\\Windows\\Start Menu\\Programs\\Startup");
        }
        if (startupFolder.empty())
        {
            startupFolder = GetKnownFolder(FOLDERID_Startup);
        }
        if (startupFolder.empty())
        {
            return {};
        }

        return JoinPath(startupFolder, kStartupShortcutName);
    }

    bool FileExists(const std::wstring& path)
    {
        const DWORD attributes = GetFileAttributes(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    bool DirectoryExists(const std::wstring& path)
    {
        const DWORD attributes = GetFileAttributes(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    std::wstring Quote(std::wstring_view value)
    {
        return L"\"" + std::wstring(value) + L"\"";
    }

    std::wstring FullPath(std::wstring path)
    {
        DWORD required = GetFullPathName(path.c_str(), 0, nullptr, nullptr);
        if (required == 0)
        {
            return path;
        }

        std::wstring result(required, L'\0');
        DWORD written = GetFullPathName(path.c_str(), required, result.data(), nullptr);
        if (written == 0)
        {
            return path;
        }

        result.resize(written);
        return result;
    }

    bool SamePath(const std::wstring& first, const std::wstring& second)
    {
        return _wcsicmp(FullPath(first).c_str(), FullPath(second).c_str()) == 0;
    }

    bool CreateDirectoryTree(const std::wstring& path)
    {
        if (path.empty() || DirectoryExists(path))
        {
            return !path.empty();
        }

        return SHCreateDirectoryEx(nullptr, path.c_str(), nullptr) == ERROR_SUCCESS || DirectoryExists(path);
    }

    bool SetRegistryString(HKEY key, const wchar_t* name, const std::wstring& value)
    {
        const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
        return RegSetValueEx(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes) == ERROR_SUCCESS;
    }

    bool SetRegistryDword(HKEY key, const wchar_t* name, DWORD value)
    {
        return RegSetValueEx(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS;
    }

    bool IsRegisteredForUninstall()
    {
        constexpr REGSAM kRegistryViews[] = { KEY_WOW64_64KEY, 0 };
        for (const REGSAM viewFlags : kRegistryViews)
        {
            HKEY key = nullptr;
            const LSTATUS status = RegOpenKeyEx(HKEY_CURRENT_USER, kUninstallKeyPath, 0, KEY_QUERY_VALUE | viewFlags, &key);
            if (status == ERROR_SUCCESS)
            {
                RegCloseKey(key);
                return true;
            }
        }

        return false;
    }

    bool RegisterForUninstall(HKEY root, REGSAM viewFlags, const std::wstring& installFolder, const std::wstring& installedExe)
    {
        HKEY key = nullptr;
        DWORD disposition = 0;
        const LSTATUS status = RegCreateKeyEx(
            root,
            kUninstallKeyPath,
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE | viewFlags,
            nullptr,
            &key,
            &disposition);
        if (status != ERROR_SUCCESS)
        {
            return false;
        }

        SYSTEMTIME localTime{};
        GetLocalTime(&localTime);
        wchar_t installDate[16]{};
        swprintf_s(installDate, L"%04u%02u%02u", localTime.wYear, localTime.wMonth, localTime.wDay);

        WIN32_FILE_ATTRIBUTE_DATA fileData{};
        DWORD estimatedKb = 0;
        if (GetFileAttributesEx(installedExe.c_str(), GetFileExInfoStandard, &fileData))
        {
            ULARGE_INTEGER size{};
            size.HighPart = fileData.nFileSizeHigh;
            size.LowPart = fileData.nFileSizeLow;
            estimatedKb = static_cast<DWORD>((size.QuadPart + 1023) / 1024);
        }

        const std::wstring uninstallCommand = Quote(installedExe) + L" " + kUninstallArgument;
        const std::wstring iconPath = installedExe + L",0";

        const bool ok =
            SetRegistryString(key, L"DisplayName", kAppDisplayName) &&
            SetRegistryString(key, L"DisplayVersion", L"1.0") &&
            SetRegistryString(key, L"Publisher", L"Local User") &&
            SetRegistryString(key, L"InstallLocation", installFolder) &&
            SetRegistryString(key, L"DisplayIcon", iconPath) &&
            SetRegistryString(key, L"UninstallString", uninstallCommand) &&
            SetRegistryString(key, L"QuietUninstallString", uninstallCommand) &&
            SetRegistryString(key, L"InstallDate", installDate) &&
            SetRegistryDword(key, L"NoModify", 1) &&
            SetRegistryDword(key, L"NoRepair", 1) &&
            SetRegistryDword(key, L"EstimatedSize", estimatedKb);

        RegCloseKey(key);
        return ok;
    }

    bool RegisterForUninstall(const std::wstring& installFolder, const std::wstring& installedExe)
    {
        bool wroteRegistration = false;
        constexpr REGSAM kRegistryViews[] = { KEY_WOW64_64KEY, 0 };
        for (const REGSAM viewFlags : kRegistryViews)
        {
            wroteRegistration = RegisterForUninstall(HKEY_CURRENT_USER, viewFlags, installFolder, installedExe);
            if (wroteRegistration)
            {
                break;
            }
        }

        return wroteRegistration && IsRegisteredForUninstall();
    }

    bool CreateStartupShortcut(const std::wstring& shortcutPath, const std::wstring& installedExe)
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool shouldUninitialize = SUCCEEDED(hr);
        if (hr == RPC_E_CHANGED_MODE)
        {
            hr = S_OK;
        }
        if (FAILED(hr))
        {
            return false;
        }

        IShellLink* shellLink = nullptr;
        hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLink, reinterpret_cast<void**>(&shellLink));
        if (SUCCEEDED(hr))
        {
            shellLink->SetPath(installedExe.c_str());
            shellLink->SetArguments(kStartupArgument);
            shellLink->SetDescription(kAppDisplayName);
            shellLink->SetIconLocation(installedExe.c_str(), 0);

            IPersistFile* persistFile = nullptr;
            hr = shellLink->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&persistFile));
            if (SUCCEEDED(hr))
            {
                hr = persistFile->Save(shortcutPath.c_str(), TRUE);
                persistFile->Release();
            }
            shellLink->Release();
        }

        if (shouldUninitialize)
        {
            CoUninitialize();
        }
        return SUCCEEDED(hr);
    }

    bool IsPerUserInstallComplete()
    {
        const std::wstring installedExe = InstalledExecutablePath();
        const std::wstring shortcut = StartupShortcutPath();
        return !installedExe.empty() &&
            !shortcut.empty() &&
            FileExists(installedExe) &&
            IsRegisteredForUninstall() &&
            FileExists(shortcut);
    }

    void RepairRegistrationIfInstalledCopyExists()
    {
        if (IsRegisteredForUninstall())
        {
            return;
        }

        const std::wstring installFolder = InstallFolder();
        const std::wstring installedExe = InstalledExecutablePath();
        const std::wstring shortcut = StartupShortcutPath();
        if (!installFolder.empty() && !installedExe.empty() && !shortcut.empty() && FileExists(installedExe) && FileExists(shortcut))
        {
            RegisterForUninstall(installFolder, installedExe);
        }
    }

    bool InstallForCurrentUser(HWND owner)
    {
        const std::wstring sourceExe = GetModulePath();
        const std::wstring installFolder = InstallFolder();
        const std::wstring installedExe = InstalledExecutablePath();
        const std::wstring shortcutPath = StartupShortcutPath();

        if (sourceExe.empty() || installFolder.empty() || installedExe.empty() || shortcutPath.empty())
        {
            MessageBox(owner, L"Could not determine the per-user install folders.", kAppDisplayName, MB_OK | MB_ICONERROR);
            return false;
        }

        if (!CreateDirectoryTree(installFolder))
        {
            MessageBox(owner, (L"Could not create install folder:\n\n" + installFolder).c_str(), kAppDisplayName, MB_OK | MB_ICONERROR);
            return false;
        }

        if (!SamePath(sourceExe, installedExe) && !CopyFile(sourceExe.c_str(), installedExe.c_str(), FALSE))
        {
            MessageBox(owner, (L"Could not copy the application to:\n\n" + installedExe).c_str(), kAppDisplayName, MB_OK | MB_ICONERROR);
            return false;
        }

        if (!RegisterForUninstall(installFolder, installedExe))
        {
            MessageBox(owner, L"Could not register the app in Windows Apps settings.", kAppDisplayName, MB_OK | MB_ICONERROR);
            return false;
        }

        if (!CreateStartupShortcut(shortcutPath, installedExe))
        {
            MessageBox(owner, (L"Could not create Startup shortcut:\n\n" + shortcutPath).c_str(), kAppDisplayName, MB_OK | MB_ICONERROR);
            return false;
        }

        return true;
    }

    void OfferInstallIfNeeded(HWND owner)
    {
        if (IsPerUserInstallComplete())
        {
            return;
        }

        const std::wstring installFolder = InstallFolder();
        if (installFolder.empty())
        {
            return;
        }

        const std::wstring message =
            L"Install What Is My IP for the current user?\n\n"
            L"Folder:\n" + installFolder + L"\n\n"
            L"This will copy the app there, register it in Windows Apps settings, and add it to Startup.";

        const int choice = MessageBox(owner, message.c_str(), kAppDisplayName, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (choice == IDYES && InstallForCurrentUser(owner))
        {
            MessageBox(owner, L"Installed for the current user.", kAppDisplayName, MB_OK | MB_ICONINFORMATION);
        }
    }

    void LaunchDeferredFolderDelete(const std::wstring& folder)
    {
        if (folder.empty())
        {
            return;
        }

        const std::wstring command = L"cmd.exe /d /c \"timeout /t 2 /nobreak >nul & rmdir /s /q " + Quote(folder) + L"\"";
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');

        STARTUPINFO startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.dwFlags = STARTF_USESHOWWINDOW;
        startupInfo.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION processInfo{};
        if (CreateProcess(
            nullptr,
            mutableCommand.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startupInfo,
            &processInfo))
        {
            CloseHandle(processInfo.hThread);
            CloseHandle(processInfo.hProcess);
        }
    }

    int UninstallCurrentUserApp()
    {
        const std::wstring shortcutPath = StartupShortcutPath();
        if (!shortcutPath.empty())
        {
            DeleteFile(shortcutPath.c_str());
        }

        RegDeleteTree(HKEY_CURRENT_USER, kUninstallKeyPath);

        const std::wstring installFolder = InstallFolder();
        const std::wstring installedExe = InstalledExecutablePath();
        const std::wstring runningExe = GetModulePath();

        if (!installedExe.empty() && !runningExe.empty() && SamePath(installedExe, runningExe))
        {
            LaunchDeferredFolderDelete(installFolder);
        }
        else if (!installedExe.empty())
        {
            DeleteFile(installedExe.c_str());
            RemoveDirectory(installFolder.c_str());
        }

        return 0;
    }

    bool HasCommandLineArguments()
    {
        int argc = 0;
        PWSTR* argv = CommandLineToArgvW(GetCommandLine(), &argc);
        if (!argv)
        {
            return false;
        }

        const bool hasArguments = argc > 1;
        LocalFree(argv);
        return hasArguments;
    }

    bool HasCommandLineArgument(std::wstring_view expected)
    {
        int argc = 0;
        PWSTR* argv = CommandLineToArgvW(GetCommandLine(), &argc);
        if (!argv)
        {
            return false;
        }

        bool found = false;
        for (int index = 1; index < argc; ++index)
        {
            if (_wcsicmp(argv[index], expected.data()) == 0)
            {
                found = true;
                break;
            }
        }

        LocalFree(argv);
        return found;
    }

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
        int consecutiveFailures = 0;
        auto nextRetryAt = std::chrono::steady_clock::time_point::max();

        while (!g_stopRequested)
        {
            if (!g_fetchRequested && nextRetryAt == std::chrono::steady_clock::time_point::max())
            {
                g_workerWake.wait(lock, [] { return g_fetchRequested || g_stopRequested; });
            }
            else if (!g_fetchRequested)
            {
                g_workerWake.wait_until(lock, nextRetryAt, [] { return g_fetchRequested || g_stopRequested; });
            }

            if (g_stopRequested)
            {
                break;
            }

            const bool networkTriggered = g_fetchRequested;
            const bool retryTriggered =
                !networkTriggered &&
                nextRetryAt != std::chrono::steady_clock::time_point::max() &&
                std::chrono::steady_clock::now() >= nextRetryAt;

            if (!networkTriggered && !retryTriggered)
            {
                continue;
            }

            if (networkTriggered)
            {
                g_fetchRequested = false;
                consecutiveFailures = 0;
                nextRetryAt = std::chrono::steady_clock::time_point::max();
                g_workerWake.wait_for(lock, kNetworkChangeDebounce, [] { return g_stopRequested; });
            }
            else
            {
                nextRetryAt = std::chrono::steady_clock::time_point::max();
            }

            if (g_stopRequested)
            {
                break;
            }

            lock.unlock();
            const std::wstring ip = FetchExternalIpv4();
            const bool lookupSucceeded = !ip.empty();
            SetIpStatus(lookupSucceeded ? ip : L"Unavailable", lookupSucceeded ? NowText() : L"Lookup failed");
            lock.lock();

            if (lookupSucceeded)
            {
                consecutiveFailures = 0;
                nextRetryAt = std::chrono::steady_clock::time_point::max();
            }
            else
            {
                ++consecutiveFailures;
                nextRetryAt = std::chrono::steady_clock::now() + (consecutiveFailures == 1 ? kFirstRetryDelay : kLaterRetryDelay);
            }
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
    if (HasCommandLineArgument(kUninstallArgument))
    {
        if (!StopRunningInstanceForUninstall() || !AcquireSingleInstance())
        {
            return 1;
        }

        const int result = UninstallCurrentUserApp();
        ReleaseSingleInstance();
        return result;
    }

    if (!AcquireSingleInstance())
    {
        return 0;
    }

    RepairRegistrationIfInstalledCopyExists();

    if (!HasCommandLineArguments())
    {
        OfferInstallIfNeeded(nullptr);
    }

    if (!RegisterTrayWindowClass(instance))
    {
        return 1;
    }

    HWND window = CreateWindowEx(
        0,
        kWindowClassName,
        kAppDisplayName,
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

    const int result = static_cast<int>(message.wParam);
    ReleaseSingleInstance();
    return result;
}
