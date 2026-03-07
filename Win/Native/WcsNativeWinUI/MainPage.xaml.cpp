#include "pch.h"
#include "MainPage.xaml.h"

#if __has_include("MainPage.g.cpp")
#include "MainPage.g.cpp"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <shellapi.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <winsvc.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

constexpr int kControlApiStartupTimeoutMs = 60 * 1000;

std::wstring NowStamp()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[32]{};
    std::swprintf(buf, _countof(buf), L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::wstring ToWide(std::string const& value)
{
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), size);
    return out;
}

std::string ToUtf8(std::wstring const& value)
{
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::string Trim(std::string s)
{
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

std::wstring TrimWide(std::wstring s)
{
    auto notSpace = [](wchar_t c) { return !std::iswspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

std::wstring ToLowerAscii(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c)
        {
            if (c >= L'A' && c <= L'Z') return static_cast<wchar_t>(c + (L'a' - L'A'));
            return c;
        });
    return value;
}

int ParseIntOr(std::string const& text, int fallback)
{
    char* end = nullptr;
    const long v = std::strtol(text.c_str(), &end, 10);
    if (!end || *end != '\0' || v < INT32_MIN || v > INT32_MAX) return fallback;
    return static_cast<int>(v);
}

double ParseDoubleOr(std::string text, double fallback)
{
    std::replace(text.begin(), text.end(), ',', '.');
    char* end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    if (!end || *end != '\0' || !std::isfinite(v)) return fallback;
    return v;
}

std::string JsonEscape(std::string const& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s)
    {
        switch (c)
        {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

std::string JsonNumber(double value)
{
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(3);
    oss << value;
    auto s = oss.str();
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s.empty()) s = "0";
    return s;
}

std::string ExtractJsonString(std::string const& json, std::string const& key)
{
    const std::string pattern = "\"" + key + "\"";
    size_t p = json.find(pattern);
    if (p == std::string::npos) return {};
    p = json.find(':', p + pattern.size());
    if (p == std::string::npos) return {};
    ++p;
    while (p < json.size() && std::isspace(static_cast<unsigned char>(json[p]))) ++p;
    if (p >= json.size() || json[p] != '"') return {};
    ++p;

    std::string out;
    while (p < json.size())
    {
        const char c = json[p++];
        if (c == '"') break;
        if (c == '\\' && p < json.size())
        {
            const char e = json[p++];
            switch (e)
            {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: out.push_back(e); break;
            }
        }
        else
        {
            out.push_back(c);
        }
    }
    return out;
}

bool ReadLineFromSocket(SOCKET sock, std::string& line)
{
    line.clear();
    char c = '\0';
    while (true)
    {
        const int n = recv(sock, &c, 1, 0);
        if (n <= 0) return false;
        if (c == '\n') return true;
        if (c != '\r') line.push_back(c);
        if (line.size() > (64 * 1024)) return false;
    }
}

std::wstring QuoteArg(std::wstring const& arg)
{
    if (arg.find_first_of(L" \t\"") == std::wstring::npos)
    {
        return arg;
    }
    std::wstring quoted = L"\"";
    for (wchar_t c : arg)
    {
        if (c == L'\"') quoted += L"\\\"";
        else quoted.push_back(c);
    }
    quoted += L"\"";
    return quoted;
}

std::wstring FormatWin32Error(DWORD code)
{
    LPWSTR message = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD n = FormatMessageW(flags, nullptr, code, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    std::wstring text;
    if (n > 0 && message)
    {
        text.assign(message, n);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
        {
            text.pop_back();
        }
    }
    if (message) LocalFree(message);
    if (text.empty()) text = L"unknown error";
    return text;
}

void PumpUiMessagesFor(DWORD durationMs)
{
    const ULONGLONG start = GetTickCount64();
    while (true)
    {
        const ULONGLONG elapsed = GetTickCount64() - start;
        if (elapsed >= durationMs)
        {
            break;
        }

        const DWORD remaining = static_cast<DWORD>(durationMs - elapsed);
        const DWORD waitMs = (remaining > 40) ? 40 : remaining;
        const DWORD waitRc = MsgWaitForMultipleObjects(0, nullptr, FALSE, waitMs, QS_ALLINPUT);
        if (waitRc == WAIT_OBJECT_0)
        {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        else if (waitRc == WAIT_FAILED)
        {
            break;
        }
    }
}

struct ServiceProbe
{
    bool exists = false;
    bool running = false;
    DWORD state = SERVICE_STOPPED;
    std::wstring name;
};

ServiceProbe ProbeService(std::initializer_list<const wchar_t*> names)
{
    ServiceProbe probe{};
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return probe;

    for (const wchar_t* name : names)
    {
        SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_QUERY_STATUS);
        if (!svc) continue;

        probe.exists = true;
        probe.name = name;

        SERVICE_STATUS_PROCESS ssp{};
        DWORD needed = 0;
        if (QueryServiceStatusEx(
            svc,
            SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&ssp),
            static_cast<DWORD>(sizeof(ssp)),
            &needed))
        {
            probe.state = ssp.dwCurrentState;
            probe.running = (ssp.dwCurrentState == SERVICE_RUNNING);
        }

        CloseServiceHandle(svc);
        break;
    }

    CloseServiceHandle(scm);
    return probe;
}

std::wstring ServiceStateText(DWORD state)
{
    switch (state)
    {
    case SERVICE_STOPPED: return L"stopped";
    case SERVICE_START_PENDING: return L"start pending";
    case SERVICE_STOP_PENDING: return L"stop pending";
    case SERVICE_RUNNING: return L"running";
    case SERVICE_CONTINUE_PENDING: return L"continue pending";
    case SERVICE_PAUSE_PENDING: return L"pause pending";
    case SERVICE_PAUSED: return L"paused";
    default: return L"unknown";
    }
}

std::wstring GetEnvVar(std::wstring const& name)
{
    DWORD size = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
    if (size == 0) return {};

    std::wstring value(static_cast<size_t>(size), L'\0');
    const DWORD copied = GetEnvironmentVariableW(name.c_str(), value.data(), size);
    if (copied == 0 || copied >= size) return {};
    value.resize(static_cast<size_t>(copied));
    return value;
}

bool ContainsAsciiI(std::wstring const& haystack, std::wstring const& needle)
{
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;

    auto lower = [](wchar_t c) -> wchar_t
        {
            if (c >= L'A' && c <= L'Z') return static_cast<wchar_t>(c + 32);
            return c;
        };

    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i)
    {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j)
        {
            if (lower(haystack[i + j]) != lower(needle[j]))
            {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

std::vector<std::filesystem::path> AppleSupportRoots()
{
    std::vector<std::filesystem::path> roots;

    auto append = [&roots](std::filesystem::path const& p)
        {
            if (p.empty()) return;
            for (auto const& existing : roots)
            {
                if (_wcsicmp(existing.wstring().c_str(), p.wstring().c_str()) == 0) return;
            }
            roots.push_back(p);
        };

    const std::wstring commonPf = GetEnvVar(L"CommonProgramFiles");
    const std::wstring commonPf86 = GetEnvVar(L"CommonProgramFiles(x86)");
    if (!commonPf.empty()) append(std::filesystem::path(commonPf) / L"Apple" / L"Mobile Device Support");
    if (!commonPf86.empty()) append(std::filesystem::path(commonPf86) / L"Apple" / L"Mobile Device Support");

    const std::wstring pf = GetEnvVar(L"ProgramFiles");
    const std::wstring pf86 = GetEnvVar(L"ProgramFiles(x86)");
    if (!pf.empty()) append(std::filesystem::path(pf) / L"Common Files" / L"Apple" / L"Mobile Device Support");
    if (!pf86.empty()) append(std::filesystem::path(pf86) / L"Common Files" / L"Apple" / L"Mobile Device Support");

    return roots;
}

struct AppleUsbProbeResult
{
    ServiceProbe service;
    std::filesystem::path mobileDll;
    std::filesystem::path driverInf;
    bool ready = false;
};

AppleUsbProbeResult ProbeAppleUsbSupport()
{
    AppleUsbProbeResult result{};
    result.service = ProbeService({ L"Apple Mobile Device Service", L"AppleMobileDeviceService" });

    const auto roots = AppleSupportRoots();
    std::error_code ec;
    for (auto const& root : roots)
    {
        if (result.mobileDll.empty())
        {
            const auto dllDirect = root / L"MobileDevice.dll";
            const auto dllBin = root / L"bin" / L"MobileDevice.dll";
            if (std::filesystem::exists(dllDirect, ec) && std::filesystem::is_regular_file(dllDirect, ec)) result.mobileDll = dllDirect;
            else if (std::filesystem::exists(dllBin, ec) && std::filesystem::is_regular_file(dllBin, ec)) result.mobileDll = dllBin;
        }

        if (result.driverInf.empty())
        {
            const auto inf64 = root / L"Drivers" / L"usbaapl64.inf";
            const auto inf32 = root / L"Drivers" / L"usbaapl.inf";
            if (std::filesystem::exists(inf64, ec) && std::filesystem::is_regular_file(inf64, ec)) result.driverInf = inf64;
            else if (std::filesystem::exists(inf32, ec) && std::filesystem::is_regular_file(inf32, ec)) result.driverInf = inf32;
        }
    }

    result.ready = result.service.exists && result.service.running;
    return result;
}

bool EnsureServiceRunning(std::initializer_list<const wchar_t*> names, std::wstring& detail)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm)
    {
        detail = L"OpenSCManager failed (" + std::to_wstring(GetLastError()) + L")";
        return false;
    }

    bool found = false;
    for (const wchar_t* name : names)
    {
        SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_QUERY_STATUS | SERVICE_START);
        if (!svc) continue;
        found = true;

        SERVICE_STATUS_PROCESS ssp{};
        DWORD needed = 0;
        if (!QueryServiceStatusEx(
            svc,
            SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&ssp),
            static_cast<DWORD>(sizeof(ssp)),
            &needed))
        {
            detail = L"QueryServiceStatusEx failed (" + std::to_wstring(GetLastError()) + L") for " + name;
            CloseServiceHandle(svc);
            continue;
        }

        if (ssp.dwCurrentState == SERVICE_RUNNING)
        {
            detail = L"already running (" + std::wstring(name) + L")";
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            return true;
        }

        if (ssp.dwCurrentState == SERVICE_STOPPED)
        {
            if (!StartServiceW(svc, 0, nullptr))
            {
                const DWORD startErr = GetLastError();
                if (startErr != ERROR_SERVICE_ALREADY_RUNNING)
                {
                    detail = L"StartService failed (" + std::to_wstring(startErr) + L") for " + name;
                    CloseServiceHandle(svc);
                    continue;
                }
            }
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (QueryServiceStatusEx(
                svc,
                SC_STATUS_PROCESS_INFO,
                reinterpret_cast<LPBYTE>(&ssp),
                static_cast<DWORD>(sizeof(ssp)),
                &needed))
            {
                if (ssp.dwCurrentState == SERVICE_RUNNING)
                {
                    detail = L"started (" + std::wstring(name) + L")";
                    CloseServiceHandle(svc);
                    CloseServiceHandle(scm);
                    return true;
                }
            }
            PumpUiMessagesFor(400);
        }

        detail = L"service did not reach running state (" + std::wstring(name) + L")";
        CloseServiceHandle(svc);
    }

    CloseServiceHandle(scm);
    if (!found)
    {
        detail = L"service not found";
    }
    return false;
}

bool LaunchElevatedAndWait(
    std::wstring const& file,
    std::wstring const& parameters,
    std::wstring const& workdir,
    DWORD timeoutMs,
    DWORD& exitCode,
    std::wstring& error)
{
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = file.c_str();
    sei.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
    sei.lpDirectory = workdir.empty() ? nullptr : workdir.c_str();
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei))
    {
        const DWORD code = GetLastError();
        error = L"ShellExecuteEx failed (" + std::to_wstring(code) + L"): " + FormatWin32Error(code);
        return false;
    }
    if (!sei.hProcess)
    {
        error = L"ShellExecuteEx returned no process handle";
        return false;
    }

    const ULONGLONG startTick = GetTickCount64();
    HANDLE waitHandle = sei.hProcess;
    while (true)
    {
        const ULONGLONG elapsed = GetTickCount64() - startTick;
        if (elapsed >= timeoutMs)
        {
            error = L"installer timeout after " + std::to_wstring(timeoutMs / 1000) + L"s";
            CloseHandle(sei.hProcess);
            return false;
        }

        const DWORD remaining = static_cast<DWORD>(timeoutMs - elapsed);
        const DWORD slice = (remaining > 250) ? 250 : remaining;
        const DWORD waitRc = MsgWaitForMultipleObjects(1, &waitHandle, FALSE, slice, QS_ALLINPUT);
        if (waitRc == WAIT_OBJECT_0)
        {
            break;
        }
        if (waitRc == WAIT_OBJECT_0 + 1)
        {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            continue;
        }
        if (waitRc == WAIT_TIMEOUT)
        {
            continue;
        }

        const DWORD code = GetLastError();
        error = L"Wait for installer failed (" + std::to_wstring(code) + L"): " + FormatWin32Error(code);
        CloseHandle(sei.hProcess);
        return false;
    }

    exitCode = 0;
    if (!GetExitCodeProcess(sei.hProcess, &exitCode))
    {
        const DWORD code = GetLastError();
        error = L"GetExitCodeProcess failed (" + std::to_wstring(code) + L"): " + FormatWin32Error(code);
        CloseHandle(sei.hProcess);
        return false;
    }

    CloseHandle(sei.hProcess);
    return true;
}

bool AskUserInstallAppleUsb()
{
    HWND owner = GetActiveWindow();
    if (!owner) owner = GetForegroundWindow();

    const int rc = MessageBoxW(
        owner,
        L"Les prerequis Apple pour iProxy sont absents ou incomplets.\n\n"
        L"Voulez-vous lancer l'installation maintenant ?\n\n"
        L"Oui = installer\n"
        L"Non = fermer l'application",
        L"WinCamStream - Prerequis Apple",
        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1 | MB_TOPMOST);
    return rc == IDYES;
}

void ExitApplicationNow()
{
    try
    {
        auto app = winrt::Microsoft::UI::Xaml::Application::Current();
        if (app)
        {
            app.Exit();
            return;
        }
    }
    catch (...)
    {
    }
    PostQuitMessage(0);
}

} // namespace

namespace winrt::WcsNativeWinUI::implementation
{

MainPage::MainPage()
{
    InitializeComponent();

    m_exeDir = std::filesystem::current_path();
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(_countof(path)));
    if (n > 0 && n < _countof(path))
    {
        m_exeDir = std::filesystem::path(path).parent_path();
    }
    m_runtimeLogPath = RuntimeLogPath();

    WSADATA data{};
    m_wsaReady = (WSAStartup(MAKEWORD(2, 2), &data) == 0);
}

MainPage::~MainPage()
{
    if (m_statusTimer)
    {
        m_statusTimer.Stop();
        m_statusTimer = nullptr;
    }
    CloseProcess(m_vcam);
    CloseProcess(m_iproxyVideo);
    CloseProcess(m_iproxyControl);
    if (m_wsaReady)
    {
        WSACleanup();
        m_wsaReady = false;
    }
}

void MainPage::OnLoaded(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    if (m_initialized)
    {
        return;
    }
    m_initialized = true;

    if (!m_themeLoaded)
    {
        LoadUiSettings();
        m_themeLoaded = true;
    }

    m_suppressThemeEvent = true;
    ThemeToggle().IsOn(m_darkTheme);
    m_suppressThemeEvent = false;
    ApplyTheme(m_darkTheme);

    MainContentRoot().Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
    InitOverlay().Visibility(Microsoft::UI::Xaml::Visibility::Visible);
    InitProgressRing().IsActive(true);
    ResetInitFailureUi();
    SetInitStep(L"Ouverture", L"Demarrage du client WinCamStream.", 5.0);
    SetBadge(L"Initializing");

    ResizeHostWindow(540, 860);

    AppendLog(L"Client ready.");
    AppendLog(L"Runtime log: " + m_runtimeLogPath.wstring());
    if (!m_wsaReady)
    {
        AppendLog(L"WSA startup failed, control API unavailable.");
    }

    auto weak = get_weak();
    DispatcherQueue().TryEnqueue([weak]()
        {
            if (auto self = weak.get())
            {
                self->DispatcherQueue().TryEnqueue([weak]()
                    {
                        if (auto inner = weak.get())
                        {
                            inner->RunStartupSequence();
                        }
                    });
            }
        });
}

void MainPage::OnThemeToggle(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    if (m_suppressThemeEvent)
    {
        return;
    }
    m_darkTheme = ThemeToggle().IsOn();
    ApplyTheme(m_darkTheme);
    SaveUiSettings();
}

void MainPage::OnHelp(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    ShowWelcomeDialog(true);
}

void MainPage::OnRestartIproxy(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    RestartIproxy();
}

void MainPage::OnStatus(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    RefreshStatus(true);
}

void MainPage::OnStart(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    SendSimpleCommand("start", L"start");
}

void MainPage::OnStop(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    SendSimpleCommand("stop", L"stop");
}

void MainPage::OnRestart(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    SendSimpleCommand("restart", L"restart");
}

void MainPage::OnApply(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    std::string response;
    std::string error;
    if (!SendControl(BuildApplyPayload(), response, error))
    {
        AppendLog(L"Apply error: " + ToWide(error));
        SetBadge(L"Apply error");
        return;
    }
    AppendLog(L"Apply: " + ToWide(response));
    SetBadge(L"Applied");
}

void MainPage::OnPreview(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    Preview();
}

void MainPage::OnStartVcam(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    StartVcam();
}

void MainPage::OnStopVcam(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    StopVcam(true);
}

void MainPage::OnInitRetry(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    if (m_startupInProgress)
    {
        return;
    }

    AppendLog(L"Startup retry requested.");
    auto weak = get_weak();
    DispatcherQueue().TryEnqueue([weak]()
        {
            if (auto self = weak.get())
            {
                self->RunStartupSequence();
            }
        });
}

void MainPage::SetInitStep(std::wstring const& step, std::wstring const& detail, double progress)
{
    try
    {
        InitStepText().Text(step);
        InitDetailText().Text(detail);
        const double clamped = std::clamp(progress, 0.0, 100.0);
        InitProgressBar().Value(clamped);
        InitPercentText().Text(std::to_wstring(static_cast<int>(std::llround(clamped))) + L"%");
    }
    catch (...)
    {
        // Keep startup alive even if a named control/resource is missing.
    }
}

void MainPage::ResetInitFailureUi()
{
    try
    {
        InitFailurePanel().Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
        InitFailureText().Text(L"");
        InitTipsText().Text(L"");
    }
    catch (...)
    {
        // Ignore missing controls during startup transitions.
    }
}

void MainPage::ShowInitFailure(
    std::wstring const& step,
    std::wstring const& detail,
    std::wstring const& tips,
    double progress,
    std::wstring const& badge)
{
    SetInitStep(step, detail, progress);
    InitProgressRing().IsActive(false);
    SetBadge(badge);

    try
    {
        InitFailureText().Text(detail);
        InitTipsText().Text(tips);
        InitFailurePanel().Visibility(Microsoft::UI::Xaml::Visibility::Visible);
    }
    catch (...)
    {
        // Best effort UI, logs keep the full diagnostic.
    }
}

void MainPage::ApplyTheme(bool dark)
{
    RequestedTheme(dark
        ? Microsoft::UI::Xaml::ElementTheme::Dark
        : Microsoft::UI::Xaml::ElementTheme::Light);

    auto setBrush = [this](wchar_t const* key, winrt::Windows::UI::Color const& color)
        {
            auto obj = Resources().Lookup(winrt::box_value(key));
            if (auto brush = obj.try_as<Microsoft::UI::Xaml::Media::SolidColorBrush>())
            {
                brush.Color(color);
            }
        };

    auto C = [](uint8_t a, uint8_t r, uint8_t g, uint8_t b)
        {
            return Microsoft::UI::ColorHelper::FromArgb(a, r, g, b);
        };

    if (dark)
    {
        setBrush(L"AppBackgroundBrush", C(0xFF, 0x1C, 0x1F, 0x24));
        setBrush(L"AmbientShape1Brush", C(0x26, 0x4A, 0x89, 0xDC));
        setBrush(L"AmbientShape2Brush", C(0x1F, 0x2D, 0xBF, 0x86));
        setBrush(L"CardBackgroundBrush", C(0xFF, 0x20, 0x28, 0x35));
        setBrush(L"CardBorderBrush", C(0xFF, 0x2F, 0x3A, 0x4E));
        setBrush(L"TextPrimaryBrush", C(0xFF, 0xF2, 0xF6, 0xFF));
        setBrush(L"TextSecondaryBrush", C(0xFF, 0xB8, 0xC3, 0xD7));
        setBrush(L"InputBackgroundBrush", C(0xFF, 0x13, 0x1C, 0x2A));
        setBrush(L"InputBorderBrush", C(0xFF, 0x35, 0x45, 0x5F));
        setBrush(L"ButtonBackgroundBrush", C(0xFF, 0x25, 0x33, 0x4A));
        setBrush(L"ButtonBorderBrush", C(0xFF, 0x3A, 0x4E, 0x6E));
        setBrush(L"ButtonForegroundBrush", C(0xFF, 0xF2, 0xF6, 0xFF));
    }
    else
    {
        setBrush(L"AppBackgroundBrush", C(0xFF, 0xF4, 0xF6, 0xFA));
        setBrush(L"AmbientShape1Brush", C(0x2A, 0x4A, 0x89, 0xDC));
        setBrush(L"AmbientShape2Brush", C(0x24, 0x2D, 0xBF, 0x86));
        setBrush(L"CardBackgroundBrush", C(0xFF, 0xFF, 0xFF, 0xFF));
        setBrush(L"CardBorderBrush", C(0xFF, 0xD7, 0xDF, 0xEA));
        setBrush(L"TextPrimaryBrush", C(0xFF, 0x13, 0x20, 0x36));
        setBrush(L"TextSecondaryBrush", C(0xFF, 0x4A, 0x5A, 0x72));
        setBrush(L"InputBackgroundBrush", C(0xFF, 0xF8, 0xFB, 0xFF));
        setBrush(L"InputBorderBrush", C(0xFF, 0xB6, 0xC3, 0xD6));
        setBrush(L"ButtonBackgroundBrush", C(0xFF, 0xE8, 0xEE, 0xF7));
        setBrush(L"ButtonBorderBrush", C(0xFF, 0xC3, 0xD0, 0xE3));
        setBrush(L"ButtonForegroundBrush", C(0xFF, 0x13, 0x20, 0x36));
    }

    SetBadge(StatusBadgeText().Text().c_str());
}

std::filesystem::path MainPage::UiSettingsPath() const
{
    wchar_t localAppData[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, static_cast<DWORD>(_countof(localAppData)));
    if (n > 0 && n < _countof(localAppData))
    {
        return std::filesystem::path(localAppData) / L"WinCamStream" / L"ui-settings.ini";
    }
    return m_exeDir / L"ui-settings.ini";
}

std::filesystem::path MainPage::RuntimeLogPath() const
{
    wchar_t localAppData[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, static_cast<DWORD>(_countof(localAppData)));
    if (n > 0 && n < _countof(localAppData))
    {
        return std::filesystem::path(localAppData) / L"WinCamStream" / L"logs" / L"wcs_native_winui.log";
    }
    return m_exeDir / L"wcs_native_winui.log";
}

void MainPage::LoadUiSettings()
{
    m_darkTheme = true;
    m_welcomeSeen = false;

    std::error_code ec;
    const auto path = UiSettingsPath();
    if (!std::filesystem::exists(path, ec)) return;

    std::ifstream in(path);
    if (!in.is_open()) return;

    std::string line;
    while (std::getline(in, line))
    {
        const auto p = line.find('=');
        if (p == std::string::npos) continue;

        const std::string key = Trim(line.substr(0, p));
        const std::string value = Trim(line.substr(p + 1));
        if (key == "theme")
        {
            m_darkTheme = !(value == "light" || value == "0" || value == "false");
        }
        else if (key == "welcome_seen")
        {
            m_welcomeSeen = (value == "1" || value == "true" || value == "yes");
        }
    }
}

void MainPage::SaveUiSettings() const
{
    const auto path = UiSettingsPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) return;

    out << "theme=" << (m_darkTheme ? "dark" : "light") << "\n";
    out << "welcome_seen=" << (m_welcomeSeen ? "1" : "0") << "\n";
}

bool MainPage::WaitForControlApiReady(int timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    int attempt = 0;
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::string response;
        std::string error;
        if (SendControl("{\"cmd\":\"get_status\"}", response, error))
        {
            return true;
        }

        ++attempt;
        if ((attempt % 4) == 0)
        {
            AppendLog(L"Waiting for control API bridge (phone/link)... ");
        }
        PumpUiMessagesFor(250);
    }
    return false;
}

void MainPage::ResizeHostWindow(int width, int height) const
{
    HWND hwnd = GetActiveWindow();
    if (!hwnd) hwnd = GetForegroundWindow();
    if (!hwnd) return;

    const UINT dpi = GetDpiForWindow(hwnd);
    const float scale = static_cast<float>(dpi) / 96.0f;
    const int targetW = static_cast<int>(width * scale);
    const int targetH = static_cast<int>(height * scale);

    RECT rc{};
    GetWindowRect(hwnd, &rc);
    SetWindowPos(hwnd, nullptr, rc.left, rc.top, targetW, targetH, SWP_NOZORDER | SWP_NOACTIVATE);
}

winrt::fire_and_forget MainPage::RunStartupSequence()
{
    auto lifetime = get_strong();
    if (m_startupInProgress)
    {
        co_return;
    }
    m_startupInProgress = true;

    try
    {
        MainContentRoot().Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
        InitOverlay().Visibility(Microsoft::UI::Xaml::Visibility::Visible);
        InitProgressRing().IsActive(true);
        ResetInitFailureUi();

        SetInitStep(L"Verification des dependances iProxy", L"Controle des prerequis iProxy...", 18.0);
        AppendLog(L"Running iProxy prerequisite check...");
        RestartIproxy();

        if (!m_iproxyVideo.running || !m_iproxyControl.running)
        {
            AppendLog(L"Startup blocked: iProxy dependencies are not ready.");
            ShowInitFailure(
                L"Verification des dependances iProxy",
                L"Impossible de demarrer iProxy. Les dependances USB Apple ou les DLL iProxy sont manquantes.",
                L"Tips:\n- Verifier l'iPhone en USB et accepter 'Faire confiance'.\n- Verifier Apple Mobile Device Support.\n- Verifier le dossier Runtime (iproxy.exe + DLL).",
                30.0,
                L"Startup blocked");
            m_startupInProgress = false;
            co_return;
        }

        SetInitStep(L"Liaison control API", L"En attente de connexion du telephone (max 60 s)...", 78.0);
        const bool controlReady = WaitForControlApiReady(kControlApiStartupTimeoutMs);

        if (controlReady)
        {
            SetBadge(L"Connected");
            RefreshStatus(false);
        }
        else
        {
            AppendLog(L"Control API timeout after 60s. Startup overlay kept active.");
            ShowInitFailure(
                L"Liaison control API",
                L"Timeout de la liaison Control API apres 60 secondes.",
                L"Tips:\n- Ouvrir l'app iOS et verifier que le stream est lance.\n- Verifier les ports Video/Control (5000/5001).\n- Rebrancher le cable USB puis cliquer Reessayer.\n- Cliquer 'Restart iProxy' si besoin.",
                78.0,
                L"Waiting phone");
            m_startupInProgress = false;
            co_return;
        }

        if (!m_statusTimer)
        {
            m_statusTimer = DispatcherQueue().CreateTimer();
            m_statusTimer.Interval(std::chrono::seconds(2));
            auto weak = get_weak();
            m_statusTimer.Tick([weak](auto const&, auto const&)
                {
                    if (auto self = weak.get())
                    {
                        self->RefreshStatus(false);
                    }
                });
            m_statusTimer.Start();
        }

        SetInitStep(L"Ouverture", L"Chargement de l'interface principale...", 100.0);
        InitProgressRing().IsActive(false);
        InitOverlay().Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
        MainContentRoot().Visibility(Microsoft::UI::Xaml::Visibility::Visible);
        ResizeHostWindow(1280, 860);

        if (!m_welcomeSeen)
        {
            m_welcomeSeen = true;
            SaveUiSettings();
            ShowWelcomeDialog(false);
        }
    }
    catch (winrt::hresult_error const& ex)
    {
        AppendLog(L"Startup sequence failed: " + std::wstring(ex.message().c_str()));
        ShowInitFailure(
            L"Ouverture",
            L"Echec au demarrage de l'interface.",
            L"Tips:\n- Verifier les dependances Runtime.\n- Consulter les logs dans %LOCALAPPDATA%\\WinCamStream\\logs.\n- Cliquer Reessayer.",
            90.0,
            L"Startup error");
    }
    catch (std::exception const& ex)
    {
        AppendLog(L"Startup sequence failed: " + ToWide(ex.what()));
        ShowInitFailure(
            L"Ouverture",
            L"Echec au demarrage de l'interface.",
            L"Tips:\n- Verifier les dependances Runtime.\n- Consulter les logs dans %LOCALAPPDATA%\\WinCamStream\\logs.\n- Cliquer Reessayer.",
            90.0,
            L"Startup error");
    }
    catch (...)
    {
        AppendLog(L"Startup sequence failed unexpectedly.");
        ShowInitFailure(
            L"Ouverture",
            L"Echec au demarrage de l'interface.",
            L"Tips:\n- Verifier les dependances Runtime.\n- Consulter les logs dans %LOCALAPPDATA%\\WinCamStream\\logs.\n- Cliquer Reessayer.",
            90.0,
            L"Startup error");
    }

    m_startupInProgress = false;
    co_return;
}

winrt::fire_and_forget MainPage::ShowWelcomeDialog(bool fromHelp)
{
    auto lifetime = get_strong();
    try
    {
        Microsoft::UI::Xaml::Controls::ContentDialog dialog;
        dialog.XamlRoot(XamlRoot());
        dialog.Title(winrt::box_value(fromHelp ? L"Aide, credits et licences" : L"Bienvenue sur WinCamStream"));
        dialog.PrimaryButtonText(fromHelp ? L"Fermer" : L"Commencer");
        dialog.CloseButtonText(L"Fermer");

        Microsoft::UI::Xaml::Controls::StackPanel panel;
        panel.Spacing(8);

        Microsoft::UI::Xaml::Controls::TextBlock intro;
        intro.Text(
            fromHelp
            ? L"WinCamStream relie ton smartphone a Windows pour du streaming camera faible latence."
            : L"Ce client configure iProxy, ouvre le pont USB et permet le controle temps reel du stream.");
        intro.TextWrapping(Microsoft::UI::Xaml::TextWrapping::WrapWholeWords);
        panel.Children().Append(intro);

        Microsoft::UI::Xaml::Controls::TextBlock tuto;
        tuto.Text(
            L"Tuto rapide:\n"
            L"1. Connecter l'iPhone en USB.\n"
            L"2. Laisser l'initialisation verifier iProxy.\n"
            L"3. Verifier le statut live puis cliquer Start.\n"
            L"4. Appliquer les reglages a chaud avec Apply.");
        tuto.TextWrapping(Microsoft::UI::Xaml::TextWrapping::WrapWholeWords);
        panel.Children().Append(tuto);

        Microsoft::UI::Xaml::Controls::TextBlock credits;
        credits.Text(
            L"Credits et licences:\n"
            L"- WinCamStream: projet open-source.\n"
            L"- UnityCapture: virtual camera backend.\n"
            L"- FFmpeg, libimobiledevice, libusbmuxd: licences respectives.\n"
            L"- Logo de l'application: fourni pour Dashperf / WinCamStream.");
        credits.TextWrapping(Microsoft::UI::Xaml::TextWrapping::WrapWholeWords);
        panel.Children().Append(credits);

        dialog.Content(panel);

        co_await dialog.ShowAsync();
    }
    catch (...)
    {
        // No-op: fallback if dialog cannot be shown (no valid XamlRoot, shutdown, etc.).
    }
}

bool MainPage::EnsureAppleUsbPrerequisites()
{
    SetInitStep(L"Verification des dependances iProxy", L"Verification du support USB Apple...", 28.0);

    auto before = ProbeAppleUsbSupport();
    if (before.ready)
    {
        return true;
    }

    if (before.service.exists && !before.service.running)
    {
        std::wstring serviceInfo;
        const bool serviceReady = EnsureServiceRunning({ L"Apple Mobile Device Service", L"AppleMobileDeviceService" }, serviceInfo);
        if (serviceReady)
        {
            AppendLog(L"Apple Mobile Device Service: " + serviceInfo + L".");
        }
        else
        {
            AppendLog(L"Apple Mobile Device Service check: " + serviceInfo + L".");
        }

        auto afterService = ProbeAppleUsbSupport();
        if (afterService.ready)
        {
            AppendLog(L"Apple USB prerequisites ready.");
            return true;
        }
        before = afterService;
    }

    AppendLog(L"Apple USB prerequisites missing. User confirmation required before install.");
    LogAppleUsbSupportDiagnostic(true);

    if (!AskUserInstallAppleUsb())
    {
        AppendLog(L"Apple prerequisite installation declined. Closing application.");
        ExitApplicationNow();
        return false;
    }

    SetInitStep(L"Installation des dependances", L"Installation du support Apple Mobile Device...", 44.0);

    bool attemptedInstall = false;
    DWORD installerExitCode = 0;
    std::wstring runError;

    const auto installer = ResolveAppleSupportInstaller();
    if (!installer.empty())
    {
        attemptedInstall = true;
        const auto ext = installer.extension().wstring();
        const bool isMsi = (_wcsicmp(ext.c_str(), L".msi") == 0);
        const std::wstring fileToRun = isMsi ? L"msiexec.exe" : installer.wstring();
        const std::wstring parameters = isMsi
            ? (L"/i " + QuoteArg(installer.wstring()) + L" /qn /norestart")
            : L"/quiet /norestart";
        const std::wstring workdir = installer.parent_path().wstring();

        AppendLog(L"Running embedded Apple installer: " + installer.wstring());
        const bool launched = LaunchElevatedAndWait(fileToRun, parameters, workdir, 30 * 60 * 1000, installerExitCode, runError);
        if (!launched)
        {
            AppendLog(L"Embedded Apple installer failed: " + runError);
        }
        else
        {
            AppendLog(L"Embedded Apple installer finished with exit code " + std::to_wstring(installerExitCode) + L".");
        }
    }
    const auto winget = ResolveExe(L"winget.exe");
    std::error_code ec;
    if (std::filesystem::exists(winget, ec) && std::filesystem::is_regular_file(winget, ec))
    {
        auto runWingetPackage = [&](wchar_t const* packageId, double progress) -> bool
            {
                attemptedInstall = true;
                SetInitStep(L"Telechargement des dependances", L"Telechargement du package " + std::wstring(packageId) + L"...", progress);
                const std::wstring args =
                    L"install --id " + std::wstring(packageId) + L" --source winget "
                    L"--accept-source-agreements --accept-package-agreements --silent --disable-interactivity";
                AppendLog(L"Running automatic winget install: " + std::wstring(packageId));

                DWORD code = 0;
                std::wstring installErr;
                const bool launched = LaunchElevatedAndWait(winget.wstring(), args, L"", 30 * 60 * 1000, code, installErr);
                if (!launched)
                {
                    AppendLog(L"Winget install failed for " + std::wstring(packageId) + L": " + installErr);
                    return false;
                }

                AppendLog(L"Winget install finished for " + std::wstring(packageId) + L" with exit code " + std::to_wstring(code) + L".");
                SetInitStep(L"Installation des dependances", L"Verification post-installation...", progress + 4.0);
                std::wstring localServiceInfo;
                EnsureServiceRunning({ L"Apple Mobile Device Service", L"AppleMobileDeviceService" }, localServiceInfo);
                auto state = ProbeAppleUsbSupport();
                return state.ready;
            };

        if (!before.ready)
        {
            if (runWingetPackage(L"Apple.AppleDevices", 50.0))
            {
                before = ProbeAppleUsbSupport();
            }
        }
        if (!before.ready)
        {
            if (runWingetPackage(L"Apple.AppleMobileDeviceSupport", 56.0))
            {
                before = ProbeAppleUsbSupport();
            }
        }
        if (!before.ready)
        {
            runWingetPackage(L"Apple.iTunes", 62.0);
        }
    }

    std::wstring serviceInfo;
    const bool serviceReady = EnsureServiceRunning({ L"Apple Mobile Device Service", L"AppleMobileDeviceService" }, serviceInfo);
    if (serviceReady)
    {
        AppendLog(L"Apple Mobile Device Service: " + serviceInfo + L".");
    }
    else
    {
        AppendLog(L"Apple Mobile Device Service check: " + serviceInfo + L".");
    }

    auto after = ProbeAppleUsbSupport();
    if (after.ready)
    {
        SetInitStep(L"Verification des dependances iProxy", L"Support USB Apple operationnel.", 70.0);
        AppendLog(L"Apple USB prerequisites ready.");
        return true;
    }

    if (!attemptedInstall)
    {
        AppendLog(L"No installer available: missing embedded Apple MSI and winget not found.");
    }
    else
    {
        AppendLog(L"Automatic installation completed but prerequisites are still incomplete.");
    }
    LogAppleUsbSupportDiagnostic(true);
    return false;
}

void MainPage::AppendLog(std::wstring const& text)
{
    const std::wstring line = L"[" + NowStamp() + L"] " + text;

    std::error_code ec;
    std::filesystem::create_directories(m_runtimeLogPath.parent_path(), ec);
    std::wofstream out(m_runtimeLogPath, std::ios::app);
    if (out.is_open())
    {
        out << line << L"\n";
    }

    try
    {
        std::wstring current = LogBox().Text().c_str();
        if (!current.empty()) current += L"\r\n";
        current += line;
        LogBox().Text(current);
        LogBox().Select(static_cast<int32_t>(current.size()), 0);
    }
    catch (...)
    {
        // UI log area may not be ready yet; file log remains available.
    }
}

void MainPage::SetBadge(std::wstring const& text)
{
    StatusBadgeText().Text(text);

    const std::wstring lower = ToLowerAscii(text);
    auto brush = Microsoft::UI::Xaml::Media::SolidColorBrush(Microsoft::UI::ColorHelper::FromArgb(0xFF, 0x2D, 0x3F, 0x5A));
    std::wstring glyph = L"\xE895"; // Contact

    if (lower.find(L"connected") != std::wstring::npos ||
        lower.find(L"ready") != std::wstring::npos ||
        lower.find(L"running") != std::wstring::npos ||
        lower.find(L"ok") != std::wstring::npos)
    {
        brush.Color(Microsoft::UI::ColorHelper::FromArgb(0xFF, 0x14, 0x8A, 0x62));
        glyph = L"\xE73E"; // Plug connected
    }
    else if (lower.find(L"error") != std::wstring::npos ||
        lower.find(L"failed") != std::wstring::npos ||
        lower.find(L"missing") != std::wstring::npos)
    {
        brush.Color(Microsoft::UI::ColorHelper::FromArgb(0xFF, 0xB2, 0x3A, 0x48));
        glyph = L"\xEA39"; // Error badge
    }
    else if (lower.find(L"disconnected") != std::wstring::npos ||
        lower.find(L"waiting") != std::wstring::npos ||
        lower.find(L"startup") != std::wstring::npos ||
        lower.find(L"check") != std::wstring::npos)
    {
        brush.Color(Microsoft::UI::ColorHelper::FromArgb(0xFF, 0xA4, 0x73, 0x1E));
        glyph = L"\xE9CE"; // Clock
    }

    StatusBadgeBorder().Background(brush);
    StatusBadgeIcon().Glyph(glyph);
}

void MainPage::CloseProcess(ManagedProcess& proc)
{
    if (proc.running && proc.pi.hProcess)
    {
        TerminateProcess(proc.pi.hProcess, 0);
        WaitForSingleObject(proc.pi.hProcess, 1200);
    }
    if (proc.pi.hThread)
    {
        CloseHandle(proc.pi.hThread);
        proc.pi.hThread = nullptr;
    }
    if (proc.pi.hProcess)
    {
        CloseHandle(proc.pi.hProcess);
        proc.pi.hProcess = nullptr;
    }
    proc.running = false;
}

bool MainPage::LaunchProcess(const std::filesystem::path& exe, std::wstring const& args, bool noWindow, ManagedProcess& outProc, std::wstring& err)
{
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    if (!args.empty())
    {
        cmd += L" ";
        cmd += args;
    }

    std::vector<wchar_t> cmdMutable(cmd.begin(), cmd.end());
    cmdMutable.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    DWORD flags = CREATE_NEW_PROCESS_GROUP;
    if (noWindow) flags |= CREATE_NO_WINDOW;

    const BOOL ok = CreateProcessW(
        nullptr,
        cmdMutable.data(),
        nullptr,
        nullptr,
        FALSE,
        flags,
        nullptr,
        exe.parent_path().wstring().c_str(),
        &si,
        &pi);

    if (!ok)
    {
        const DWORD code = GetLastError();
        err = L"CreateProcess failed (" + std::to_wstring(code) + L"): " + FormatWin32Error(code);
        return false;
    }

    CloseProcess(outProc);
    outProc.pi = pi;
    outProc.running = true;
    return true;
}

std::filesystem::path MainPage::ResolveExe(std::wstring const& exeName) const
{
    std::error_code ec;
    std::vector<std::filesystem::path> candidates;
    candidates.reserve(5);

    auto pushUnique = [&candidates](std::filesystem::path const& path)
        {
            if (path.empty()) return;
            for (auto const& existing : candidates)
            {
                if (_wcsicmp(existing.wstring().c_str(), path.wstring().c_str()) == 0) return;
            }
            candidates.push_back(path);
        };

    const std::filesystem::path raw(exeName);
    if (raw.is_absolute()) pushUnique(raw);
    pushUnique(m_exeDir / exeName);
    if (m_exeDir.has_parent_path()) pushUnique(m_exeDir.parent_path() / exeName);

    const auto cwd = std::filesystem::current_path(ec);
    if (!ec) pushUnique(cwd / exeName);

    for (auto const& candidate : candidates)
    {
        if (std::filesystem::exists(candidate, ec) && std::filesystem::is_regular_file(candidate, ec))
        {
            return candidate;
        }
    }

    wchar_t resolved[MAX_PATH]{};
    const DWORD n = SearchPathW(nullptr, exeName.c_str(), nullptr, _countof(resolved), resolved, nullptr);
    if (n > 0 && n < _countof(resolved))
    {
        return std::filesystem::path(resolved);
    }

    return std::filesystem::path(exeName);
}

std::filesystem::path MainPage::ResolveAppleSupportInstaller() const
{
    std::vector<std::filesystem::path> roots;
    roots.reserve(4);
    roots.push_back(m_exeDir / L"Deps");
    if (m_exeDir.has_parent_path()) roots.push_back(m_exeDir.parent_path() / L"Deps");
    roots.push_back(m_exeDir);
    if (m_exeDir.has_parent_path()) roots.push_back(m_exeDir.parent_path());

    static constexpr std::array<const wchar_t*, 6> names = {
        L"AppleMobileDeviceSupport64.msi",
        L"AppleMobileDeviceSupport.msi",
        L"AppleDeviceSupport64.msi",
        L"AppleDeviceSupport.msi",
        L"AppleMobileDeviceSupport64.exe",
        L"AppleMobileDeviceSupport.exe"
    };

    std::error_code ec;
    for (auto const& root : roots)
    {
        for (auto const* name : names)
        {
            const auto path = root / name;
            if (std::filesystem::exists(path, ec) && std::filesystem::is_regular_file(path, ec))
            {
                return path;
            }
        }

        if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec))
        {
            continue;
        }
        for (auto const& entry : std::filesystem::directory_iterator(root, ec))
        {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;

            const auto ext = entry.path().extension().wstring();
            const bool isInstaller =
                (_wcsicmp(ext.c_str(), L".msi") == 0) ||
                (_wcsicmp(ext.c_str(), L".exe") == 0);
            if (!isInstaller) continue;

            const auto fileName = entry.path().filename().wstring();
            const bool looksAppleUsb =
                ContainsAsciiI(fileName, L"apple") &&
                ContainsAsciiI(fileName, L"mobile") &&
                ContainsAsciiI(fileName, L"device") &&
                (ContainsAsciiI(fileName, L"support") || ContainsAsciiI(fileName, L"driver"));
            if (looksAppleUsb)
            {
                return entry.path();
            }
        }
    }
    return {};
}

void MainPage::LogAppleUsbSupportDiagnostic(bool forceLog)
{
    if (m_appleDiagLogged && !forceLog) return;
    m_appleDiagLogged = true;

    const auto state = ProbeAppleUsbSupport();
    if (!state.service.exists)
    {
        AppendLog(L"Apple Mobile Device Service: missing.");
    }
    else
    {
        AppendLog(L"Apple Mobile Device Service: " + ServiceStateText(state.service.state) + L" (" + state.service.name + L")");
    }

    if (!state.mobileDll.empty()) AppendLog(L"Apple MobileDevice.dll: found at " + state.mobileDll.wstring());
    else AppendLog(L"Apple MobileDevice.dll: missing.");

    if (!state.driverInf.empty()) AppendLog(L"Apple USB driver INF: found at " + state.driverInf.wstring());
    else AppendLog(L"Apple USB driver INF: missing.");

    if (state.ready && (state.mobileDll.empty() || state.driverInf.empty()))
    {
        AppendLog(L"Apple support note: some optional files are not detected, but service is running.");
    }

    if (!state.ready)
    {
        AppendLog(L"USB Apple support incomplete: iProxy may fail until Apple Mobile Device Support is installed.");
        if (state.service.exists && !state.service.running)
        {
            AppendLog(L"Tip: start 'Apple Mobile Device Service' in services.msc.");
        }

        const auto embeddedInstaller = ResolveAppleSupportInstaller();
        if (!embeddedInstaller.empty())
        {
            AppendLog(L"Embedded installer available: " + embeddedInstaller.wstring());
        }
        else
        {
            AppendLog(L"No embedded installer found. Fallback installer source: winget package Apple.AppleMobileDeviceSupport.");
        }
    }
}

bool MainPage::SendControl(std::string const& payload, std::string& response, std::string& error)
{
    if (!m_wsaReady)
    {
        error = "winsock not initialized";
        return false;
    }

    const std::string host = Trim(ToUtf8(HostBox().Text().c_str()));
    const int videoPort = std::clamp(ParseIntOr(Trim(ToUtf8(VideoPortBox().Text().c_str())), 5000), 1, 65535);
    const int controlPort = std::clamp(ParseIntOr(Trim(ToUtf8(ControlPortBox().Text().c_str())), videoPort + 1), 1, 65535);

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* addrs = nullptr;
    const std::string port = std::to_string(controlPort);
    const int gai = getaddrinfo(host.empty() ? "127.0.0.1" : host.c_str(), port.c_str(), &hints, &addrs);
    if (gai != 0)
    {
        error = "getaddrinfo failed";
        return false;
    }

    SOCKET sock = INVALID_SOCKET;
    for (addrinfo* p = addrs; p; p = p->ai_next)
    {
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock == INVALID_SOCKET) continue;

        BOOL noDelay = TRUE;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        DWORD timeoutMs = 3000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

        if (connect(sock, p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0)
        {
            break;
        }

        closesocket(sock);
        sock = INVALID_SOCKET;
    }
    freeaddrinfo(addrs);

    if (sock == INVALID_SOCKET)
    {
        error = "unable to connect control endpoint";
        return false;
    }

    std::string request = payload + "\n";
    int sent = 0;
    while (sent < static_cast<int>(request.size()))
    {
        const int n = send(sock, request.data() + sent, static_cast<int>(request.size()) - sent, 0);
        if (n <= 0)
        {
            error = "send failed";
            closesocket(sock);
            return false;
        }
        sent += n;
    }

    response.clear();
    for (int i = 0; i < 6; ++i)
    {
        std::string line;
        if (!ReadLineFromSocket(sock, line)) break;
        if (line.find("\"type\":\"hello\"") != std::string::npos || line.find("\"type\": \"hello\"") != std::string::npos)
        {
            continue;
        }
        response = line;
        break;
    }

    closesocket(sock);
    if (response.empty())
    {
        error = "no response from control";
        return false;
    }

    return true;
}

void MainPage::SendSimpleCommand(std::string const& cmd, std::wstring const& label)
{
    std::string response;
    std::string error;
    const std::string payload = "{\"cmd\":\"" + cmd + "\"}";

    if (!SendControl(payload, response, error))
    {
        AppendLog(label + L" error: " + ToWide(error));
        SetBadge(L"Error");
        return;
    }

    AppendLog(label + L": " + ToWide(response));
    SetBadge(L"OK");
}

void MainPage::RefreshStatus(bool logRaw)
{
    std::string response;
    std::string error;
    if (!SendControl("{\"cmd\":\"get_status\"}", response, error))
    {
        SetBadge(L"Disconnected");
        if (logRaw)
        {
            AppendLog(L"Status error: " + ToWide(error));
        }
        return;
    }

    if (logRaw)
    {
        AppendLog(L"status: " + ToWide(response));
    }

    const auto status = ExtractJsonString(response, "status");
    const auto metrics = ExtractJsonString(response, "metrics");
    if (!status.empty())
    {
        StatusTextBlock().Text(ToWide(status));
    }
    if (!metrics.empty())
    {
        MetricsTextBlock().Text(ToWide(metrics));
    }

    SetBadge(L"Connected");
}

void MainPage::RestartIproxy()
{
    CloseProcess(m_iproxyVideo);
    CloseProcess(m_iproxyControl);
    SetInitStep(L"Chargement de iProxy", L"Validation des executables iProxy...", 24.0);

    const int videoPort = std::clamp(ParseIntOr(Trim(ToUtf8(VideoPortBox().Text().c_str())), 5000), 1024, 65534);
    const int controlPort = videoPort + 1;

    VideoPortBox().Text(ToWide(std::to_string(videoPort)));
    ControlPortBox().Text(ToWide(std::to_string(controlPort)));

    const auto iproxy = ResolveExe(L"iproxy.exe");
    std::error_code ec;
    if (!std::filesystem::exists(iproxy, ec))
    {
        AppendLog(L"iproxy.exe not found: " + iproxy.wstring());
        AppendLog(L"Search roots: exeDir=" + m_exeDir.wstring() + L" ; runtime=" + m_exeDir.parent_path().wstring());
        SetBadge(L"Missing iProxy");
        return;
    }
    AppendLog(L"Using iProxy: " + iproxy.wstring());

    std::vector<std::wstring> missingDeps;
    static constexpr std::array<const wchar_t*, 10> sidecar = {
        L"libimobiledevice-glue-1.0.dll",
        L"libusbmuxd-2.0.dll",
        L"libplist-2.0.dll",
        L"libplist++-2.0.dll",
        L"libstdc++-6.dll",
        L"libgcc_s_seh-1.dll",
        L"libwinpthread-1.dll",
        L"libgomp-1.dll",
        L"libquadmath-0.dll",
        L"libatomic-1.dll"
    };
    for (auto const* dep : sidecar)
    {
        const auto depPath = iproxy.parent_path() / dep;
        if (!std::filesystem::exists(depPath, ec)) missingDeps.push_back(dep);
    }
    if (!missingDeps.empty())
    {
        std::wstring msg = L"iProxy sidecar DLL(s) missing in " + iproxy.parent_path().wstring() + L": ";
        for (size_t i = 0; i < missingDeps.size(); ++i)
        {
            if (i > 0) msg += L", ";
            msg += missingDeps[i];
        }
        AppendLog(msg);
        SetBadge(L"Missing iProxy deps");
        return;
    }

    const auto appleProbe = ProbeAppleUsbSupport();
    if (!appleProbe.ready)
    {
        AppendLog(L"iProxy prerequisite check failed: Apple USB support is incomplete.");
        if (!EnsureAppleUsbPrerequisites())
        {
            SetBadge(L"Apple prereq missing");
            return;
        }
    }

    std::wstring err;
    const std::wstring argsVideo = L"-l " + std::to_wstring(videoPort) + L":" + std::to_wstring(videoPort);
    const std::wstring argsControl = L"-l " + std::to_wstring(controlPort) + L":" + std::to_wstring(controlPort);

    const bool okVideo = LaunchProcess(iproxy, argsVideo, true, m_iproxyVideo, err);
    if (!okVideo)
    {
        AppendLog(L"iproxy video start failed: " + err);
        LogAppleUsbSupportDiagnostic(true);
        SetBadge(L"iProxy error");
        return;
    }

    const bool okControl = LaunchProcess(iproxy, argsControl, true, m_iproxyControl, err);
    if (!okControl)
    {
        AppendLog(L"iproxy control start failed: " + err);
        LogAppleUsbSupportDiagnostic(true);
        SetBadge(L"iProxy error");
        return;
    }

    AppendLog(L"iProxy tunnels restarted.");
    SetInitStep(L"Chargement de iProxy", L"Tunnels USB iProxy actifs.", 74.0);
    SetBadge(L"iProxy ready");
}

std::string MainPage::BuildApplyPayload()
{
    const int videoPort = std::clamp(ParseIntOr(Trim(ToUtf8(VideoPortBox().Text().c_str())), 5000), 1024, 65534);
    VideoPortBox().Text(ToWide(std::to_string(videoPort)));
    ControlPortBox().Text(ToWide(std::to_string(videoPort + 1)));

    const auto selectedToUtf8 = [](winrt::Microsoft::UI::Xaml::Controls::ComboBox const& box, std::string const& fallback) {
        auto item = box.SelectedItem();
        if (!item) return fallback;
        auto value = winrt::unbox_value_or<winrt::hstring>(item, L"");
        auto str = Trim(ToUtf8(value.c_str()));
        return str.empty() ? fallback : str;
    };

    const double fps = std::clamp(ParseDoubleOr(Trim(ToUtf8(FpsBox().Text().c_str())), 120.0), 1.0, 240.0);
    const double bitrateM = std::max(1.0, ParseDoubleOr(Trim(ToUtf8(BitrateBox().Text().c_str())), 35.0));
    const double minM = std::max(1.0, ParseDoubleOr(Trim(ToUtf8(MinBitrateBox().Text().c_str())), 6.0));
    const double maxM = std::max(minM, ParseDoubleOr(Trim(ToUtf8(MaxBitrateBox().Text().c_str())), 120.0));

    const std::string resolution = selectedToUtf8(ResolutionBox(), "1080p");
    const std::string profile = selectedToUtf8(ProfileBox(), "high");
    const std::string entropy = selectedToUtf8(EntropyBox(), "cabac");
    const std::string protocol = selectedToUtf8(ProtocolBox(), "annexb");
    const std::string orientation = selectedToUtf8(OrientationBox(), "portrait");

    const int minBitrate = static_cast<int>(std::llround(minM * 1000000.0));
    const int maxBitrate = static_cast<int>(std::llround(maxM * 1000000.0));

    std::string payload = "{\"cmd\":\"apply\",\"config\":{";
    payload += "\"port\":" + std::to_string(videoPort);
    payload += ",\"resolution\":\"" + JsonEscape(resolution) + "\"";
    payload += ",\"fps\":" + JsonNumber(fps);
    payload += ",\"bitrate_mbps\":" + JsonNumber(bitrateM);
    payload += ",\"intra_only\":" + std::string(IntraOnlyToggle().IsOn() ? "true" : "false");
    payload += ",\"protocol\":\"" + JsonEscape(protocol) + "\"";
    payload += ",\"orientation\":\"" + JsonEscape(orientation) + "\"";
    payload += ",\"auto_rotate\":" + std::string(AutoRotateToggle().IsOn() ? "true" : "false");
    payload += ",\"profile\":\"" + JsonEscape(profile) + "\"";
    payload += ",\"entropy\":\"" + JsonEscape(entropy) + "\"";
    payload += ",\"auto_bitrate\":" + std::string(AutoBitrateToggle().IsOn() ? "true" : "false");
    payload += ",\"min_bitrate\":" + std::to_string(minBitrate);
    payload += ",\"max_bitrate\":" + std::to_string(maxBitrate);
    payload += "}}";

    return payload;
}

void MainPage::Preview()
{
    const auto ffplay = ResolveExe(L"ffplay.exe");
    std::error_code ec;
    if (!std::filesystem::exists(ffplay, ec))
    {
        AppendLog(L"ffplay.exe not found: " + ffplay.wstring());
        return;
    }

    const std::string host = Trim(ToUtf8(HostBox().Text().c_str()));
    const int videoPort = std::clamp(ParseIntOr(Trim(ToUtf8(VideoPortBox().Text().c_str())), 5000), 1, 65535);
    const std::string uri = "tcp://" + (host.empty() ? std::string("127.0.0.1") : host) + ":" + std::to_string(videoPort) + "?tcp_nodelay=1";

    const std::wstring args = L"-f h264 -fflags nobuffer -flags low_delay -framedrop -probesize 2048 -analyzeduration 0 -sync ext -i " + QuoteArg(ToWide(uri));
    ManagedProcess previewProc{};
    std::wstring err;
    if (LaunchProcess(ffplay, args, false, previewProc, err))
    {
        AppendLog(L"ffplay preview started.");
        if (previewProc.pi.hThread) CloseHandle(previewProc.pi.hThread);
        if (previewProc.pi.hProcess) CloseHandle(previewProc.pi.hProcess);
        previewProc.pi = {};
    }
    else
    {
        AppendLog(L"ffplay start failed: " + err);
    }
}

void MainPage::StartVcam()
{
    CloseProcess(m_vcam);

    const auto vcam = ResolveExe(L"wcs_native_vcam.exe");
    std::error_code ec;
    if (!std::filesystem::exists(vcam, ec))
    {
        AppendLog(L"wcs_native_vcam.exe not found: " + vcam.wstring());
        SetBadge(L"Missing VCam");
        return;
    }

    const std::string host = Trim(ToUtf8(HostBox().Text().c_str()));
    const int videoPort = std::clamp(ParseIntOr(Trim(ToUtf8(VideoPortBox().Text().c_str())), 5000), 1, 65535);
    const std::string uri = "tcp://" + (host.empty() ? std::string("127.0.0.1") : host) + ":" + std::to_string(videoPort) + "?tcp_nodelay=1";

    const std::wstring args = L"--url " + QuoteArg(ToWide(uri)) + L" --cap 0 --resize-mode linear --timeout-ms 0";
    std::wstring err;
    if (LaunchProcess(vcam, args, true, m_vcam, err))
    {
        AppendLog(L"VCam started (dynamic mode). Select 'Unity Video Capture' in apps.");
        SetBadge(L"VCam running");
    }
    else
    {
        AppendLog(L"VCam start failed: " + err);
        SetBadge(L"VCam error");
    }
}

void MainPage::StopVcam(bool logIfAlreadyStopped)
{
    if (!m_vcam.running)
    {
        if (logIfAlreadyStopped) AppendLog(L"VCam not running.");
        return;
    }
    CloseProcess(m_vcam);
    AppendLog(L"VCam stopped.");
    SetBadge(L"VCam stopped");
}

} // namespace winrt::WcsNativeWinUI::implementation
