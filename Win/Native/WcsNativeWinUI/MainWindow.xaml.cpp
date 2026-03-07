#include "pch.h"
#include "MainWindow.xaml.h"

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#include "microsoft.ui.xaml.window.h"
#endif

#include <filesystem>

namespace winrt::WcsNativeWinUI::implementation
{
    MainWindow::MainWindow()
    {
        InitializeComponent();
        Title(L"WinCamStream");
        ApplyWindowIcon();
        SetWindowSize(540, 860);
        CenterOnCurrentMonitor();
    }

    MainWindow::~MainWindow()
    {
        const HICON hSmall = m_iconSmall;
        const HICON hBig = m_iconBig;
        m_iconSmall = nullptr;
        m_iconBig = nullptr;

        if (hSmall) DestroyIcon(hSmall);
        if (hBig && hBig != hSmall) DestroyIcon(hBig);
    }

    HWND MainWindow::GetWindowHandle()
    {
        if (!m_hwnd)
        {
            winrt::Microsoft::UI::Xaml::Window window = *this;
            window.as<IWindowNative>()->get_WindowHandle(&m_hwnd);
        }
        return m_hwnd;
    }

    void MainWindow::ApplyWindowIcon()
    {
        const HWND hwnd = GetWindowHandle();
        if (!hwnd) return;

        wchar_t modulePath[MAX_PATH]{};
        const DWORD n = GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(_countof(modulePath)));
        if (n == 0 || n >= _countof(modulePath)) return;

        const auto exeDir = std::filesystem::path(modulePath).parent_path();
        const auto iconPath = exeDir / L"Assets" / L"WcsLogo.ico";
        if (!std::filesystem::exists(iconPath)) return;

        m_iconSmall = static_cast<HICON>(LoadImageW(
            nullptr,
            iconPath.wstring().c_str(),
            IMAGE_ICON,
            16,
            16,
            LR_LOADFROMFILE));
        m_iconBig = static_cast<HICON>(LoadImageW(
            nullptr,
            iconPath.wstring().c_str(),
            IMAGE_ICON,
            0,
            0,
            LR_LOADFROMFILE | LR_DEFAULTSIZE));

        if (m_iconSmall)
        {
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(m_iconSmall));
            SetClassLongPtrW(hwnd, GCLP_HICONSM, reinterpret_cast<LONG_PTR>(m_iconSmall));
        }
        if (m_iconBig)
        {
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(m_iconBig));
            SetClassLongPtrW(hwnd, GCLP_HICON, reinterpret_cast<LONG_PTR>(m_iconBig));
        }
    }

    void MainWindow::SetWindowSize(int width, int height)
    {
        HWND hwnd = GetWindowHandle();
        const UINT dpi = GetDpiForWindow(hwnd);
        const float scale = static_cast<float>(dpi) / 96.0f;
        SetWindowPos(hwnd, nullptr, 0, 0, static_cast<int>(width * scale), static_cast<int>(height * scale), SWP_NOMOVE | SWP_NOZORDER);
    }

    void MainWindow::CenterOnCurrentMonitor()
    {
        HWND hwnd = GetWindowHandle();
        RECT rc{};
        GetWindowRect(hwnd, &rc);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        const int width = rc.right - rc.left;
        const int height = rc.bottom - rc.top;
        const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - width) / 2;
        const int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - height) / 2;
        SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}
