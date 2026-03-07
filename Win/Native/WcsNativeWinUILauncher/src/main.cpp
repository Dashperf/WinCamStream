#include <windows.h>
#include <shellapi.h>

#include <fstream>
#include <filesystem>
#include <string>

namespace
{
std::wstring QuoteArg(std::wstring const& arg)
{
    if (arg.find_first_of(L" \t\"") == std::wstring::npos)
    {
        return arg;
    }

    std::wstring out;
    out.reserve(arg.size() + 2);
    out.push_back(L'"');
    for (wchar_t c : arg)
    {
        if (c == L'"')
        {
            out += L"\\\"";
        }
        else
        {
            out.push_back(c);
        }
    }
    out.push_back(L'"');
    return out;
}

void ShowLaunchError(std::wstring const& message)
{
    MessageBoxW(nullptr, message.c_str(), L"WinCamStream Launcher", MB_ICONERROR | MB_OK);
}

void AppendLog(std::filesystem::path const& logPath, std::wstring const& text)
{
    std::error_code ec;
    std::filesystem::create_directories(logPath.parent_path(), ec);

    std::wofstream log(logPath, std::ios::app);
    if (!log.is_open())
    {
        return;
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t ts[64]{};
    swprintf_s(ts, L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    log << L"[" << ts << L"] " << text << L"\n";
}

std::filesystem::path ResolveLogPath(std::filesystem::path const& runtimeRoot)
{
    wchar_t localAppData[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, static_cast<DWORD>(std::size(localAppData)));
    if (n > 0 && n < std::size(localAppData))
    {
        return std::filesystem::path(localAppData) / L"WinCamStream" / L"logs" / L"wcs_native_winui-launcher.log";
    }

    wchar_t tempPath[MAX_PATH]{};
    const DWORD tn = GetTempPathW(static_cast<DWORD>(std::size(tempPath)), tempPath);
    if (tn > 0 && tn < std::size(tempPath))
    {
        return std::filesystem::path(tempPath) / L"WinCamStream" / L"wcs_native_winui-launcher.log";
    }

    return runtimeRoot / L"wcs_native_winui-launcher.log";
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    wchar_t modulePath[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
    if (len == 0 || len >= std::size(modulePath))
    {
        ShowLaunchError(L"Unable to resolve launcher path.");
        return 1;
    }

    const std::filesystem::path launcherExe(modulePath);
    const std::filesystem::path runtimeRoot = launcherExe.parent_path();
    const std::filesystem::path logPath = ResolveLogPath(runtimeRoot);
    const std::filesystem::path appLogPath = logPath.parent_path() / L"wcs_native_winui-app.log";
    const std::filesystem::path winuiDir = runtimeRoot / L"WcsNativeWinUI";
    const std::filesystem::path winuiExe = winuiDir / L"WcsNativeWinUI.exe";
    AppendLog(logPath, L"Launcher started.");
    AppendLog(logPath, L"Log path: " + logPath.wstring());
    AppendLog(logPath, L"Runtime root: " + runtimeRoot.wstring());
    AppendLog(logPath, L"Target exe: " + winuiExe.wstring());

    std::error_code ec;
    if (!std::filesystem::exists(winuiExe, ec))
    {
        AppendLog(logPath, L"Target missing.");
        ShowLaunchError(L"Missing application payload:\n" + winuiExe.wstring() +
            L"\n\nRun build_native_winui.ps1 to republish runtime files.\n\nLogs:\n" + logPath.wstring());
        return 2;
    }

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring forwarded;
    if (argv != nullptr)
    {
        for (int i = 1; i < argc; ++i)
        {
            forwarded.push_back(L' ');
            forwarded += QuoteArg(argv[i]);
        }
        LocalFree(argv);
    }

    std::wstring commandLine = QuoteArg(winuiExe.wstring()) + forwarded;
    std::wstring mutableCmd = commandLine;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    const BOOL ok = CreateProcessW(
        nullptr,
        mutableCmd.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        winuiDir.wstring().c_str(),
        &si,
        &pi);

    if (!ok)
    {
        const DWORD err = GetLastError();
        AppendLog(logPath, L"CreateProcess failed. error=" + std::to_wstring(err));
        ShowLaunchError(L"Failed to start WinCamStream app.\nTarget:\n" + winuiExe.wstring() +
            L"\n\nCreateProcess error: " + std::to_wstring(err) +
            L"\n\nLogs:\n" + logPath.wstring());
        return static_cast<int>(err);
    }

    AppendLog(logPath, L"CreateProcess OK. pid=" + std::to_wstring(pi.dwProcessId));

    // If the child exits immediately, surface exit code to avoid silent failures.
    const DWORD quickWait = WaitForSingleObject(pi.hProcess, 5000);
    if (quickWait == WAIT_OBJECT_0)
    {
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        AppendLog(logPath, L"Child exited quickly. code=" + std::to_wstring(exitCode));
        ShowLaunchError(
            L"WinCamStream app closed immediately.\nExit code: " + std::to_wstring(exitCode) +
            L"\n\nLogs:\n" + logPath.wstring() +
            L"\n" + appLogPath.wstring());
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return static_cast<int>(exitCode);
    }
    if (quickWait == WAIT_FAILED)
    {
        AppendLog(logPath, L"WaitForSingleObject failed. error=" + std::to_wstring(GetLastError()));
    }
    else
    {
        AppendLog(logPath, L"Child still running after quick check.");
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    AppendLog(logPath, L"Launcher finished.");
    return 0;
}
