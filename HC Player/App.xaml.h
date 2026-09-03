#pragma once

#include "App.xaml.g.h"
#include <winrt/Microsoft.UI.Xaml.Hosting.h>

namespace winrt::HCPlayer::implementation
{
    struct App : AppT<App>
    {
        App()
            : m_xamlManager(
                winrt::Microsoft::UI::Xaml::Hosting::WindowsXamlManager::InitializeForCurrentThread())
        {
        }

        void OnLaunched(
            winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

    private:
        winrt::Microsoft::UI::Xaml::Hosting::WindowsXamlManager m_xamlManager{ nullptr };
    };
}

