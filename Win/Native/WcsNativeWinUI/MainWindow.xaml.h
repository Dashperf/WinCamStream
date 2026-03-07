#pragma once

#include "MainWindow.g.h"
#include "pch.h"

namespace winrt::WcsNativeWinUI::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();

    private:
        HWND m_hwnd{ nullptr };
        HICON m_iconSmall{ nullptr };
        HICON m_iconBig{ nullptr };
        HWND GetWindowHandle();
        void ApplyWindowIcon();
        void SetWindowSize(int width, int height);
        void CenterOnCurrentMonitor();
    };
}

namespace winrt::WcsNativeWinUI::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
