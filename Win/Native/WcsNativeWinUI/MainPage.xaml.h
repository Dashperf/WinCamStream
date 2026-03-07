#pragma once

#include "MainPage.g.h"
#include "pch.h"

#include <filesystem>
#include <string>

namespace winrt::WcsNativeWinUI::implementation
{
    struct MainPage : MainPageT<MainPage>
    {
        MainPage();
        ~MainPage();

        void OnLoaded(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnThemeToggle(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnHelp(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnRestartIproxy(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnStatus(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnStart(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnStop(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnRestart(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnApply(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnPreview(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnStartVcam(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnStopVcam(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnInitRetry(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        struct ManagedProcess {
            PROCESS_INFORMATION pi{};
            bool running = false;
        };

        std::filesystem::path m_exeDir;
        ManagedProcess m_iproxyVideo;
        ManagedProcess m_iproxyControl;
        ManagedProcess m_vcam;
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_statusTimer{ nullptr };
        bool m_wsaReady = false;
        bool m_appleDiagLogged = false;
        bool m_themeLoaded = false;
        bool m_darkTheme = true;
        bool m_welcomeSeen = false;
        bool m_suppressThemeEvent = false;
        bool m_initialized = false;
        bool m_startupInProgress = false;
        std::filesystem::path m_runtimeLogPath;

        void AppendLog(std::wstring const& text);
        void SetBadge(std::wstring const& text);
        void SetInitStep(std::wstring const& step, std::wstring const& detail, double progress);
        void ResetInitFailureUi();
        void ShowInitFailure(
            std::wstring const& step,
            std::wstring const& detail,
            std::wstring const& tips,
            double progress,
            std::wstring const& badge);
        void ApplyTheme(bool dark);
        void LoadUiSettings();
        void SaveUiSettings() const;
        bool WaitForControlApiReady(int timeoutMs);
        void ResizeHostWindow(int width, int height) const;
        std::filesystem::path UiSettingsPath() const;
        std::filesystem::path RuntimeLogPath() const;
        winrt::fire_and_forget RunStartupSequence();
        winrt::fire_and_forget ShowWelcomeDialog(bool fromHelp);
        void RefreshStatus(bool logRaw);
        void SendSimpleCommand(std::string const& cmd, std::wstring const& label);
        void RestartIproxy();
        bool EnsureAppleUsbPrerequisites();
        void Preview();
        void StartVcam();
        void StopVcam(bool logIfAlreadyStopped);
        std::string BuildApplyPayload();

        void CloseProcess(ManagedProcess& proc);
        bool LaunchProcess(const std::filesystem::path& exe, std::wstring const& args, bool noWindow, ManagedProcess& outProc, std::wstring& err);

        std::filesystem::path ResolveExe(std::wstring const& exeName) const;
        std::filesystem::path ResolveAppleSupportInstaller() const;
        void LogAppleUsbSupportDiagnostic(bool forceLog);
        bool SendControl(std::string const& payload, std::string& response, std::string& error);
    };
}

namespace winrt::WcsNativeWinUI::factory_implementation
{
    struct MainPage : MainPageT<MainPage, implementation::MainPage>
    {
    };
}
