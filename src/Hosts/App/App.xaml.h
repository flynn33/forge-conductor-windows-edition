#pragma once
#include "App.xaml.g.h"
namespace winrt::ForgeConductorApp::implementation {
// C++/WinRT derives its ABI implementation from this type.
struct App : AppT<App> {
    App();
    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);
private:
    Microsoft::UI::Xaml::Window window_{nullptr};
};
}
