#include "pch.h"

#include "App.xaml.h"
#include "MainWindow.xaml.h"

#if __has_include("App.g.cpp")
#include "App.g.cpp"
#endif

#include <filesystem>
#include <fstream>

namespace winrt
{
    using namespace Microsoft::UI::Xaml;
}

namespace winrt::WcsNativeWinUI::implementation
{
    namespace
    {
        std::filesystem::path AppCrashLogPath()
        {
            wchar_t localAppData[MAX_PATH]{};
            const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, static_cast<DWORD>(_countof(localAppData)));
            if (n > 0 && n < _countof(localAppData))
            {
                return std::filesystem::path(localAppData) / L"WinCamStream" / L"logs" / L"wcs_native_winui-app.log";
            }

            wchar_t tempPath[MAX_PATH]{};
            const DWORD tn = GetTempPathW(static_cast<DWORD>(_countof(tempPath)), tempPath);
            if (tn > 0 && tn < _countof(tempPath))
            {
                return std::filesystem::path(tempPath) / L"WinCamStream" / L"wcs_native_winui-app.log";
            }
            return std::filesystem::path(L"wcs_native_winui-app.log");
        }

        void AppendCrashLog(std::wstring const& message)
        {
            std::error_code ec;
            const auto path = AppCrashLogPath();
            std::filesystem::create_directories(path.parent_path(), ec);

            std::wofstream log(path, std::ios::app);
            if (!log.is_open())
            {
                return;
            }

            SYSTEMTIME st{};
            GetLocalTime(&st);
            wchar_t ts[64]{};
            swprintf_s(ts, L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
            log << L"[" << ts << L"] " << message << L"\n";
        }
    }

    App::App()
    {
        InitializeComponent();

        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e)
            {
                std::wstring message = L"Unhandled exception";
                try
                {
                    message += L": ";
                    message += e.Message().c_str();
                }
                catch (...)
                {
                    message += L": <message unavailable>";
                }
                AppendCrashLog(message);

#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
                if (IsDebuggerPresent())
                {
                    (void)e.Message();
                    __debugbreak();
                }
#endif
            });
    }

    void App::OnLaunched(LaunchActivatedEventArgs const&)
    {
        m_window = winrt::make<MainWindow>();
        m_window.Activate();
    }
}
