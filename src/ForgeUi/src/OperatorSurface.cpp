// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeUi/OperatorSurface.h"

#include "ForgeDomain/Version.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace Forge::Ui {
namespace {

constexpr float kSidebar = 228.0f;
constexpr float kStatus = 28.0f;
constexpr float kTabTop = 92.0f;
constexpr float kTabH = 46.0f;

D2D1_COLOR_F c(float r, float g, float b, float a = 1.0f) {
    return D2D1::ColorF(r, g, b, a);
}

const D2D1_COLOR_F kBg = c(0.008f, 0.016f, 0.040f);
const D2D1_COLOR_F kSidebarBg = c(0.018f, 0.028f, 0.048f);
const D2D1_COLOR_F kCyan = c(0.08f, 0.90f, 0.96f);
const D2D1_COLOR_F kMint = c(0.22f, 0.95f, 0.68f);
const D2D1_COLOR_F kOrange = c(1.00f, 0.55f, 0.18f);
const D2D1_COLOR_F kPurple = c(0.78f, 0.48f, 1.00f);
const D2D1_COLOR_F kGreen = c(0.28f, 0.88f, 0.48f);
const D2D1_COLOR_F kAmber = c(1.00f, 0.78f, 0.22f);
const D2D1_COLOR_F kRed = c(1.00f, 0.38f, 0.32f);
const D2D1_COLOR_F kMuted = c(0.52f, 0.58f, 0.64f);
const D2D1_COLOR_F kText = c(0.90f, 0.93f, 0.95f);
const D2D1_COLOR_F kCard = c(1.0f, 1.0f, 1.0f, 0.045f);

std::wstring formatF(const wchar_t* spec, double value) {
    wchar_t buffer[80]{};
    swprintf(buffer, 80, spec, value);
    return buffer;
}

std::wstring formatII(const wchar_t* spec, int a, int b) {
    wchar_t buffer[80]{};
    swprintf(buffer, 80, spec, a, b);
    return buffer;
}

bool containsPt(const D2D1_RECT_F& box, float x, float y) {
    return x >= box.left && x <= box.right && y >= box.top && y <= box.bottom;
}

} // namespace

struct HitTarget {
    D2D1_RECT_F box{};
    SurfaceHit hit{SurfaceHit::None};
};

struct OperatorSurface::Impl {
    HWND hwnd{nullptr};
    std::uint32_t width{1};
    std::uint32_t height{1};
    float scrollY{0};
    float contentH{0};
    std::vector<HitTarget> hits;
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID3D11DeviceContext> d3dCtx;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> d2d;
    ComPtr<ID2D1Bitmap1> target;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<IDWriteTextFormat> titleFont;
    ComPtr<IDWriteTextFormat> labelFont;
    ComPtr<IDWriteTextFormat> bodyFont;
    ComPtr<IDWriteTextFormat> monoFont;
    ComPtr<IDWriteTextFormat> valueFont;
    ComPtr<IDWriteTextFormat> microFont;
    ComPtr<IDWriteTextFormat> pillFont;
    ComPtr<ID2D1SolidColorBrush> brush;

    bool createTarget() {
        target.Reset();
        if (!swap || !d2d) {
            return false;
        }
        ComPtr<IDXGISurface> surface;
        if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&surface)))) {
            return false;
        }
        const auto props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        if (FAILED(d2d->CreateBitmapFromDxgiSurface(surface.Get(), &props, &target))) {
            return false;
        }
        d2d->SetTarget(target.Get());
        return true;
    }

    void text(
        IDWriteTextFormat* font,
        const D2D1_RECT_F& box,
        const wchar_t* value,
        const D2D1_COLOR_F& color,
        DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
        if (!value || !brush || !d2d || !font) {
            return;
        }
        font->SetTextAlignment(align);
        font->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        brush->SetColor(color);
        d2d->DrawTextW(
            value,
            static_cast<UINT32>(wcslen(value)),
            font,
            box,
            brush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
        font->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        font->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }

    void fillRound(const D2D1_RECT_F& box, float radius, const D2D1_COLOR_F& color) {
        brush->SetColor(color);
        d2d->FillRoundedRectangle(D2D1::RoundedRect(box, radius, radius), brush.Get());
    }

    void strokeRound(const D2D1_RECT_F& box, float radius, const D2D1_COLOR_F& color, float strokeWidth = 1.0f) {
        brush->SetColor(color);
        d2d->DrawRoundedRectangle(D2D1::RoundedRect(box, radius, radius), brush.Get(), strokeWidth);
    }

    void fillRect(const D2D1_RECT_F& box, const D2D1_COLOR_F& color) {
        brush->SetColor(color);
        d2d->FillRectangle(box, brush.Get());
    }

    void bar(float x, float y, float w, float h, float frac, const D2D1_COLOR_F& color) {
        fillRound(D2D1::RectF(x, y, x + w, y + h), h * 0.5f, c(0.04f, 0.07f, 0.10f));
        const float fill = w * std::clamp(frac, 0.0f, 1.0f);
        if (fill > 1.0f) {
            fillRound(D2D1::RectF(x, y, x + fill, y + h), h * 0.5f, color);
        }
    }

    void ring(float cx, float cy, float radius, float frac, const D2D1_COLOR_F& color) {
        brush->SetColor(c(0.10f, 0.14f, 0.18f));
        d2d->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush.Get(), 5.0f);
        const float clamped = std::clamp(frac, 0.0f, 1.0f);
        const int steps = (std::max)(2, static_cast<int>(48.0f * clamped));
        brush->SetColor(color);
        for (int i = 0; i < steps; ++i) {
            const float a0 = -1.5707963f + 6.2831853f * (static_cast<float>(i) / 48.0f);
            const float a1 = -1.5707963f + 6.2831853f * (static_cast<float>(i + 1) / 48.0f);
            d2d->DrawLine(
                D2D1::Point2F(cx + std::cos(a0) * radius, cy + std::sin(a0) * radius),
                D2D1::Point2F(cx + std::cos(a1) * radius, cy + std::sin(a1) * radius),
                brush.Get(),
                5.0f);
        }
    }

    void panel(const D2D1_RECT_F& box, const wchar_t* title, const wchar_t* meta, const D2D1_COLOR_F& accent) {
        fillRound(box, 10.0f, c(0.03f, 0.045f, 0.07f));
        strokeRound(box, 10.0f, c(accent.r, accent.g, accent.b, 0.55f), 1.6f);
        fillRect(D2D1::RectF(box.left + 1, box.top + 32, box.right - 1, box.top + 33),
            c(accent.r, accent.g, accent.b, 0.18f));
        text(microFont.Get(), D2D1::RectF(box.left + 14, box.top + 6, box.right - 140, box.top + 28), title, accent);
        if (meta && meta[0]) {
            text(microFont.Get(), D2D1::RectF(box.right - 240, box.top + 6, box.right - 14, box.top + 28),
                meta, kMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
    }

    void pill(float x, float y, float w, float h, const wchar_t* label, bool on, float frac) {
        const auto fill = on ? c(0.05f, 0.22f, 0.18f) : c(0.10f, 0.12f, 0.16f);
        const auto stroke = on ? kGreen : c(0.38f, 0.42f, 0.46f);
        fillRound(D2D1::RectF(x, y, x + w, y + h), 6.0f, fill);
        strokeRound(D2D1::RectF(x, y, x + w, y + h), 6.0f, stroke);
        fillRound(D2D1::RectF(x + 5, y + h - 7, x + 5 + (w - 10) * std::clamp(frac, 0.08f, 1.0f), y + h - 4),
            1.5f, stroke);
        text(pillFont.Get(), D2D1::RectF(x, y, x + w, y + h - 4), label, c(0.92f, 0.96f, 0.96f),
            DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    void button(const D2D1_RECT_F& box, const wchar_t* label, SurfaceHit hit, const D2D1_COLOR_F& fill) {
        fillRound(box, 6.0f, fill);
        text(labelFont.Get(), box, label, c(1, 1, 1), DWRITE_TEXT_ALIGNMENT_CENTER);
        hits.push_back({box, hit});
    }

    void sysCard(const D2D1_RECT_F& box, const wchar_t* title, const wchar_t* value, const wchar_t* meta,
        float frac, const D2D1_COLOR_F& tint) {
        fillRound(box, 8.0f, kCard);
        strokeRound(box, 8.0f, c(tint.r, tint.g, tint.b, 0.30f));
        text(microFont.Get(), D2D1::RectF(box.left + 12, box.top + 8, box.right - 10, box.top + 24), title, kMuted);
        text(valueFont.Get(), D2D1::RectF(box.left + 12, box.top + 22, box.right - 10, box.top + 62), value, tint);
        bar(box.left + 12, box.top + 68, (box.right - box.left) - 24, 12, frac, tint);
        text(microFont.Get(), D2D1::RectF(box.left + 12, box.top + 84, box.right - 10, box.bottom - 6), meta, kMuted);
    }

    float maxScroll() const {
        const float view = static_cast<float>(height) - kStatus - 12.0f;
        return (std::max)(0.0f, contentH - view);
    }
};

OperatorSurface::OperatorSurface() : impl_(std::make_unique<Impl>()) {}
OperatorSurface::~OperatorSurface() { detach(); }

bool OperatorSurface::isAttached() const {
    return impl_ && impl_->swap && impl_->d2d;
}

void OperatorSurface::resetScroll() {
    if (impl_) {
        impl_->scrollY = 0;
    }
}

void OperatorSurface::addScroll(float delta) {
    if (!impl_) {
        return;
    }
    impl_->scrollY = std::clamp(impl_->scrollY + delta, 0.0f, impl_->maxScroll());
}

bool OperatorSurface::attach(HWND hwnd, std::uint32_t width, std::uint32_t height) {
    detach();
    impl_->hwnd = hwnd;
    impl_->width = (std::max)(64u, width);
    impl_->height = (std::max)(64u, height);

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, 1, D3D11_SDK_VERSION, &device, &level, &context))) {
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    device.As(&dxgiDevice);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = impl_->width;
    desc.Height = impl_->height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    ComPtr<IDXGISwapChain1> swap;
    if (FAILED(factory->CreateSwapChainForHwnd(device.Get(), hwnd, &desc, nullptr, nullptr, &swap))) {
        return false;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    ComPtr<ID2D1Factory1> d2dFactory;
    const D2D1_FACTORY_OPTIONS options{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, options, d2dFactory.ReleaseAndGetAddressOf()))) {
        return false;
    }
    ComPtr<ID2D1Device> d2dDevice;
    if (FAILED(d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice))) {
        return false;
    }
    ComPtr<ID2D1DeviceContext> d2d;
    if (FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d))) {
        return false;
    }
    ComPtr<IDWriteFactory> dwrite;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwrite.GetAddressOf())))) {
        return false;
    }

    auto makeFont = [&](const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& out) {
        dwrite->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &out);
        if (out) {
            out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    };
    makeFont(L"Segoe UI", 20.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, impl_->titleFont);
    makeFont(L"Segoe UI", 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, impl_->labelFont);
    makeFont(L"Segoe UI", 12.5f, DWRITE_FONT_WEIGHT_NORMAL, impl_->bodyFont);
    makeFont(L"Cascadia Mono", 11.0f, DWRITE_FONT_WEIGHT_NORMAL, impl_->monoFont);
    makeFont(L"Cascadia Mono", 24.0f, DWRITE_FONT_WEIGHT_BOLD, impl_->valueFont);
    makeFont(L"Cascadia Mono", 10.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, impl_->microFont);
    makeFont(L"Segoe UI", 11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, impl_->pillFont);
    if (!impl_->monoFont) {
        makeFont(L"Consolas", 11.0f, DWRITE_FONT_WEIGHT_NORMAL, impl_->monoFont);
    }
    if (!impl_->valueFont) {
        makeFont(L"Consolas", 24.0f, DWRITE_FONT_WEIGHT_BOLD, impl_->valueFont);
    }
    if (!impl_->microFont) {
        makeFont(L"Consolas", 10.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, impl_->microFont);
    }

    impl_->d3d = device;
    impl_->d3dCtx = context;
    impl_->swap = swap;
    impl_->d2dFactory = d2dFactory;
    impl_->d2dDevice = d2dDevice;
    impl_->d2d = d2d;
    impl_->dwrite = dwrite;
    if (FAILED(d2d->CreateSolidColorBrush(c(1, 1, 1), &impl_->brush))) {
        return false;
    }
    return impl_->createTarget();
}

void OperatorSurface::resize(std::uint32_t width, std::uint32_t height) {
    if (!impl_->swap || !impl_->d2d) {
        return;
    }
    impl_->width = (std::max)(64u, width);
    impl_->height = (std::max)(64u, height);
    impl_->d2d->SetTarget(nullptr);
    impl_->target.Reset();
    if (FAILED(impl_->swap->ResizeBuffers(0, impl_->width, impl_->height, DXGI_FORMAT_UNKNOWN, 0))) {
        return;
    }
    impl_->createTarget();
    impl_->scrollY = std::clamp(impl_->scrollY, 0.0f, impl_->maxScroll());
}

void OperatorSurface::detach() {
    if (!impl_) {
        return;
    }
    if (impl_->d2d) {
        impl_->d2d->SetTarget(nullptr);
    }
    impl_->target.Reset();
    impl_->brush.Reset();
    impl_->titleFont.Reset();
    impl_->labelFont.Reset();
    impl_->bodyFont.Reset();
    impl_->monoFont.Reset();
    impl_->valueFont.Reset();
    impl_->microFont.Reset();
    impl_->pillFont.Reset();
    impl_->dwrite.Reset();
    impl_->d2d.Reset();
    impl_->d2dDevice.Reset();
    impl_->d2dFactory.Reset();
    impl_->swap.Reset();
    impl_->d3dCtx.Reset();
    impl_->d3d.Reset();
}

SurfaceHit OperatorSurface::hitTest(int x, int y) const {
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    for (const auto& hit : impl_->hits) {
        if (containsPt(hit.box, fx, fy)) {
            return hit.hit;
        }
    }
    return SurfaceHit::None;
}

void OperatorSurface::render(const SurfaceFrame& frame) {
    if (!impl_->d2d || !impl_->target || !impl_->brush) {
        return;
    }
    const float w = static_cast<float>(impl_->width);
    const float h = static_cast<float>(impl_->height);
    auto& g = *impl_;
    g.hits.clear();
    g.d2d->BeginDraw();
    g.d2d->Clear(kBg);

    g.fillRect(D2D1::RectF(0, 0, kSidebar, h), kSidebarBg);
    g.fillRect(D2D1::RectF(kSidebar - 1, 0, kSidebar, h), c(0.08f, 0.90f, 0.96f, 0.18f));
    g.text(g.titleFont.Get(), D2D1::RectF(18, 16, kSidebar - 12, 42), L"FORGE", kCyan);
    g.text(g.microFont.Get(), D2D1::RectF(18, 42, kSidebar - 12, 60), L"CONDUCTOR  ·  WINDOWS", kMuted);

    static const wchar_t* labels[] = {
        L"FORGE RIG", L"LM Studio MCP", L"Agents", L"Tools", L"Live Feed", L"Diagnostics", L"Manager",
    };
    for (int i = 0; i < static_cast<int>(SurfaceTab::Count); ++i) {
        const float y = kTabTop + static_cast<float>(i) * kTabH;
        const bool selected = static_cast<int>(frame.tab) == i;
        const auto box = D2D1::RectF(12, y, kSidebar - 12, y + 38);
        if (selected) {
            g.fillRound(box, 7.0f, c(0.00f, 0.40f, 0.50f));
            g.strokeRound(box, 7.0f, c(0.08f, 0.90f, 0.96f, 0.45f));
        }
        g.text(g.labelFont.Get(), D2D1::RectF(26, y + 4, kSidebar - 18, y + 34), labels[i],
            selected ? c(1, 1, 1) : c(0.70f, 0.76f, 0.80f));
        g.hits.push_back({box, static_cast<SurfaceHit>(static_cast<int>(SurfaceHit::TabRig) + i)});
    }

    const std::string ver = Forge::Domain::kVersion;
    const std::wstring footer = L"v" + std::wstring(ver.begin(), ver.end()) + L"  native";
    g.text(g.microFont.Get(), D2D1::RectF(18, h - 48, kSidebar - 12, h - 28), footer.c_str(), kMuted);

    const float pad = 14.0f;
    const float x = kSidebar + pad;
    const float contentW = w - x - pad;
    const float viewTop = 8.0f;
    const float scroll = g.scrollY;
    float y = viewTop - scroll;

    auto pushY = [&](float add) { y += add; };

    if (frame.tab == SurfaceTab::Rig) {
        g.text(g.titleFont.Get(), D2D1::RectF(x, y, x + 360, y + 28), L"FORGE RIG // LM STUDIO", kCyan);
        g.text(g.microFont.Get(), D2D1::RectF(x, y + 28, x + 620, y + 46),
            L"LOCAL MODELS  ·  GPU  ·  DISK  ·  MCP  ·  LIVE FEED  ·  DIRECT2D", kMuted);
        const float pillW = 86.0f;
        float px = x + 380.0f;
        if (px + pillW * 4.0f + 18.0f > x + contentW) {
            px = x + contentW - (pillW * 4.0f + 18.0f);
        }
        const auto mcpLabel = formatII(L"MCP %d/%d", frame.liveMcp, (std::max)(frame.configuredMcp, 1));
        const auto loadLabel = formatF(L"LOAD %.0f", static_cast<double>(frame.cpu));
        g.pill(px, y + 8, pillW, 34, L"LINK", true, 1.0f);
        g.pill(px + 92, y + 8, pillW, 34, frame.orchOk ? L"ORCH OK" : L"ORCH", frame.orchOk, frame.orchOk ? 1.f : 0.25f);
        g.pill(px + 184, y + 8, pillW, 34, mcpLabel.c_str(), frame.liveMcp > 0,
            frame.configuredMcp > 0 ? static_cast<float>(frame.liveMcp) / static_cast<float>(frame.configuredMcp) : 0);
        g.pill(px + 276, y + 8, pillW, 34, loadLabel.c_str(), frame.cpu < 90.f, frame.cpu / 100.f);
        pushY(56);

        const float cardGap = 10.0f;
        const float cardW = (std::min)(248.0f, (contentW - cardGap * 4.0f) / 5.0f);
        const float cardH = 108.0f;
        const auto cpuVal = formatF(L"%.1f%%", frame.cpu);
        const auto freqVal = frame.freqMhz > 0 ? formatF(L"%.0f", frame.freqMhz) : L"—";
        const auto ramVal = formatF(L"%.1f%%", frame.ram);
        const auto gpuVal = formatF(L"%.1f%%", frame.gpu);
        const auto dioVal = formatF(L"%.1f", frame.diskReadMBs + frame.diskWriteMBs);
        g.sysCard(D2D1::RectF(x, y, x + cardW, y + cardH), L"CPU", cpuVal.c_str(),
            frame.cpuBrand.empty() ? L"host processor" : frame.cpuBrand.c_str(), frame.cpu / 100.f, kCyan);
        g.sysCard(D2D1::RectF(x + cardW + cardGap, y, x + 2 * cardW + cardGap, y + cardH), L"FREQ", freqVal.c_str(),
            L"MHz  registry", std::clamp(frame.freqMhz / 5000.f, 0.f, 1.f), kMint);
        g.sysCard(D2D1::RectF(x + 2 * (cardW + cardGap), y, x + 3 * cardW + 2 * cardGap, y + cardH), L"RAM",
            ramVal.c_str(), frame.ramMeta.empty() ? L"committed" : frame.ramMeta.c_str(), frame.ram / 100.f, kOrange);
        g.sysCard(D2D1::RectF(x + 3 * (cardW + cardGap), y, x + 4 * cardW + 3 * cardGap, y + cardH), L"GPU",
            gpuVal.c_str(), frame.gpuName.empty() ? L"adapter" : frame.gpuName.c_str(), frame.gpu / 100.f, kGreen);
        g.sysCard(D2D1::RectF(x + 4 * (cardW + cardGap), y, x + 5 * cardW + 4 * cardGap, y + cardH), L"DISK I/O",
            dioVal.c_str(), frame.diskIoMeta.empty() ? L"MB/s" : frame.diskIoMeta.c_str(),
            std::clamp((frame.diskReadMBs + frame.diskWriteMBs) / 200.f, 0.f, 1.f), kPurple);
        pushY(cardH + 10);

        const float traceH = 148.0f;
        g.panel(D2D1::RectF(x, y, x + contentW, y + traceH), L"▸ LOAD TRACE  ·  Direct2D  ·  REAL-TIME", L"CPU  RAM  GPU", kCyan);
        const float tx0 = x + 12, ty0 = y + 30, tx1 = x + contentW - 12, ty1 = y + traceH - 10;
        g.fillRound(D2D1::RectF(tx0, ty0, tx1, ty1), 6.0f, c(0.012f, 0.024f, 0.040f));
        g.brush->SetColor(c(0.08f, 0.90f, 0.96f, 0.10f));
        for (int i = 1; i < 4; ++i) {
            const float gy = ty1 - (ty1 - ty0) * (static_cast<float>(i) / 4.0f);
            g.d2d->DrawLine(D2D1::Point2F(tx0, gy), D2D1::Point2F(tx1, gy), g.brush.Get(), 1.0f);
        }
        auto series = [&](const std::vector<float>& hist, const D2D1_COLOR_F& color, float width) {
            if (hist.size() < 2) return;
            g.brush->SetColor(color);
            for (std::size_t i = 1; i < hist.size(); ++i) {
                const float t0 = static_cast<float>(i - 1) / static_cast<float>(hist.size() - 1);
                const float t1 = static_cast<float>(i) / static_cast<float>(hist.size() - 1);
                g.d2d->DrawLine(
                    D2D1::Point2F(tx0 + (tx1 - tx0) * t0, ty1 - (ty1 - ty0) * std::clamp(hist[i - 1] / 100.f, 0.f, 1.f)),
                    D2D1::Point2F(tx0 + (tx1 - tx0) * t1, ty1 - (ty1 - ty0) * std::clamp(hist[i] / 100.f, 0.f, 1.f)),
                    g.brush.Get(), width);
            }
        };
        series(frame.ramHistory, kMint, 1.6f);
        series(frame.gpuHistory, kOrange, 1.6f);
        series(frame.cpuHistory, kCyan, 2.2f);
        pushY(traceH + 10);

        const float midH = 138.0f;
        const float leftW = contentW * 0.62f;
        g.panel(D2D1::RectF(x, y, x + leftW - 6, y + midH), L"▸ CPU CORES",
            formatII(L"%d logical", static_cast<int>(frame.perCore.size()), 0).c_str(), kCyan);
        if (!frame.perCore.empty()) {
            const float innerX = x + 12, innerY = y + 32;
            const float innerW = leftW - 30, innerH = midH - 46;
            const float gap = 3.0f;
            const auto n = static_cast<float>(frame.perCore.size());
            const float barW = (std::min)(16.0f, (std::max)(5.0f, (innerW - gap * (n - 1)) / n));
            for (std::size_t i = 0; i < frame.perCore.size(); ++i) {
                const float bx = innerX + static_cast<float>(i) * (barW + gap);
                const float frac = std::clamp(frame.perCore[i] / 100.f, 0.f, 1.f);
                const float bh = (std::max)(3.0f, innerH * frac);
                g.fillRound(D2D1::RectF(bx, innerY, bx + barW, innerY + innerH), 3.0f, c(0.04f, 0.08f, 0.12f));
                g.fillRound(D2D1::RectF(bx, innerY + innerH - bh, bx + barW, innerY + innerH), 3.0f, kCyan);
            }
        }
        g.panel(D2D1::RectF(x + leftW + 6, y, x + contentW, y + midH), L"▸ GPU ENGINES",
            frame.gpuName.empty() ? L"PDH" : frame.gpuName.c_str(), kPurple);
        {
            const float gx = x + leftW + 18;
            auto engine = [&](float ey, const wchar_t* name, float pct, const D2D1_COLOR_F& tint) {
                g.text(g.microFont.Get(), D2D1::RectF(gx, ey, gx + 70, ey + 16), name, kMuted);
                g.bar(gx + 74, ey + 3, contentW - leftW - 110, 10, pct / 100.f, tint);
                const auto txt = formatF(L"%.0f%%", pct);
                g.text(g.microFont.Get(), D2D1::RectF(x + contentW - 52, ey, x + contentW - 14, ey + 16),
                    txt.c_str(), tint, DWRITE_TEXT_ALIGNMENT_TRAILING);
            };
            engine(y + 36, L"DEVICE", frame.gpuDevice > 0 ? frame.gpuDevice : frame.gpu, kPurple);
            engine(y + 62, L"3D", frame.gpuDevice, kCyan);
            engine(y + 88, L"COMPUTE", frame.gpuCompute, kMint);
            engine(y + 114, L"COPY", frame.gpuCopy, kOrange);
        }
        pushY(midH + 10);

        const float storeH = 168.0f;
        g.panel(D2D1::RectF(x, y, x + contentW, y + storeH), L"▸ STORAGE",
            formatF(L"%.1f MB/s total", frame.diskReadMBs + frame.diskWriteMBs).c_str(), kPurple);
        {
            const float col = (contentW - 48) / 3.0f;
            auto io = [&](float ix, const wchar_t* title, float mbs, float iops, const D2D1_COLOR_F& tint) {
                g.text(g.microFont.Get(), D2D1::RectF(ix, y + 32, ix + col, y + 48), title, kMuted);
                const auto mv = formatF(L"%.1f MB/s", mbs);
                g.text(g.monoFont.Get(), D2D1::RectF(ix, y + 48, ix + col, y + 66), mv.c_str(), tint);
                g.bar(ix, y + 68, col - 12, 8, std::clamp(mbs / 100.f, 0.f, 1.f), tint);
                const auto iv = formatF(L"%.0f IOPS", iops);
                g.text(g.microFont.Get(), D2D1::RectF(ix, y + 78, ix + col, y + 94), iv.c_str(), kMuted);
            };
            io(x + 16, L"READ", frame.diskReadMBs, frame.diskReadIops, kCyan);
            io(x + 16 + col, L"WRITE", frame.diskWriteMBs, frame.diskWriteIops, kOrange);
            io(x + 16 + 2 * col, L"TOTAL", frame.diskReadMBs + frame.diskWriteMBs,
                frame.diskReadIops + frame.diskWriteIops, kPurple);
            float dy = y + 102;
            for (const auto& disk : frame.disks) {
                if (dy + 20 > y + storeH - 8) break;
                g.text(g.monoFont.Get(), D2D1::RectF(x + 16, dy, x + 70, dy + 16), disk.title.c_str(), kText);
                g.bar(x + 76, dy + 3, contentW - 220, 10, disk.fraction, kPurple);
                g.text(g.microFont.Get(), D2D1::RectF(x + contentW - 140, dy, x + contentW - 16, dy + 16),
                    disk.meta.c_str(), kMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);
                dy += 20;
            }
        }
        pushY(storeH + 10);

        const float orchH = 118.0f;
        g.panel(D2D1::RectF(x, y, x + contentW, y + orchH), L"▸ ORCHESTRATION",
            frame.orchOk ? L"OK" : L"ISSUES", kCyan);
        if (!frame.orch.empty()) {
            const float oc = (contentW - 24.0f - 8.0f * static_cast<float>(frame.orch.size() - 1)) /
                static_cast<float>((std::max)(static_cast<int>(frame.orch.size()), 1));
            for (std::size_t i = 0; i < frame.orch.size(); ++i) {
                const auto& card = frame.orch[i];
                const float ox = x + 12 + static_cast<float>(i) * (oc + 8);
                const auto tint = c(card.r, card.g, card.b);
                g.fillRound(D2D1::RectF(ox, y + 32, ox + oc, y + orchH - 10), 6.0f, kCard);
                g.strokeRound(D2D1::RectF(ox, y + 32, ox + oc, y + orchH - 10), 6.0f, c(tint.r, tint.g, tint.b, 0.4f));
                g.text(g.microFont.Get(), D2D1::RectF(ox + 8, y + 36, ox + oc - 8, y + 52), card.title.c_str(), kMuted);
                g.text(g.monoFont.Get(), D2D1::RectF(ox + 8, y + 52, ox + oc - 8, y + 70), card.state.c_str(), tint);
                g.bar(ox + 8, y + 74, oc - 16, 7, card.fraction, tint);
                g.text(g.microFont.Get(), D2D1::RectF(ox + 8, y + 84, ox + oc - 8, y + 102), card.detail.c_str(), kMuted);
            }
        }
        pushY(orchH + 10);

        const float mcpH = 168.0f;
        g.panel(D2D1::RectF(x, y, x + contentW * 0.48f - 5, y + mcpH), L"▸ MCP SERVERS",
            formatII(L"%d cards", static_cast<int>(frame.mcpCards.size()), 0).c_str(), kCyan);
        {
            float cy = y + 32;
            if (frame.mcpCards.empty()) {
                g.text(g.bodyFont.Get(), D2D1::RectF(x + 14, cy, x + contentW * 0.48f - 16, cy + 20),
                    L"NO MCP PRESENCE — deploy, then wait for heartbeat", kMuted);
            }
            for (const auto& card : frame.mcpCards) {
                if (cy + 60 > y + mcpH - 8) break;
                const auto tint = card.live ? kGreen : kMuted;
                g.fillRound(D2D1::RectF(x + 12, cy, x + contentW * 0.48f - 16, cy + 56), 6.0f, kCard);
                g.strokeRound(D2D1::RectF(x + 12, cy, x + contentW * 0.48f - 16, cy + 56), 6.0f, c(tint.r, tint.g, tint.b, 0.35f));
                g.ring(x + 40, cy + 28, 16, card.activity / 100.f, tint);
                g.text(g.microFont.Get(), D2D1::RectF(x + 32, cy + 20, x + 50, cy + 36),
                    card.live ? L"ON" : L"SB", kText, DWRITE_TEXT_ALIGNMENT_CENTER);
                g.text(g.monoFont.Get(), D2D1::RectF(x + 64, cy + 8, x + contentW * 0.48f - 24, cy + 24),
                    card.label.c_str(), kText);
                g.text(g.microFont.Get(), D2D1::RectF(x + 64, cy + 24, x + contentW * 0.48f - 24, cy + 38),
                    card.meta.c_str(), kMuted);
                g.bar(x + 64, cy + 42, contentW * 0.48f - 92, 6, card.activity / 100.f, tint);
                cy += 62;
            }
        }
        g.panel(D2D1::RectF(x + contentW * 0.48f + 5, y, x + contentW, y + mcpH), L"▸ MCP TOOLS",
            formatII(L"%d tools", frame.toolCount, 0).c_str(), kMint);
        {
            const float tx = x + contentW * 0.48f + 16;
            const float tw = contentW * 0.52f - 28;
            const float tile = 64.0f;
            const float gap = 8.0f;
            const int cols = (std::max)(1, static_cast<int>((tw + gap) / (tile + gap)));
            int i = 0;
            for (const auto& tool : frame.tools) {
                if (i >= cols * 2) break;
                const int col = i % cols;
                const int row = i / cols;
                const float bx = tx + static_cast<float>(col) * (tile + gap);
                const float by = y + 32 + static_cast<float>(row) * 62.0f;
                const auto tint = tool.loadTier >= 2 ? kOrange : (tool.events > 0 ? kCyan : kMuted);
                g.fillRound(D2D1::RectF(bx, by, bx + tile, by + 54), 5.0f, kCard);
                g.strokeRound(D2D1::RectF(bx, by, bx + tile, by + 54), 5.0f, c(tint.r, tint.g, tint.b, 0.35f));
                g.text(g.microFont.Get(), D2D1::RectF(bx + 4, by + 6, bx + tile - 4, by + 22),
                    tool.shortLabel.c_str(), kText, DWRITE_TEXT_ALIGNMENT_CENTER);
                g.bar(bx + 6, by + 28, tile - 12, 6, tool.activity / 100.f, tint);
                g.text(g.microFont.Get(), D2D1::RectF(bx + 4, by + 36, bx + tile - 4, by + 50),
                    tool.health.c_str(), kMuted, DWRITE_TEXT_ALIGNMENT_CENTER);
                ++i;
            }
        }
        pushY(mcpH + 10);

        const float lowH = 200.0f;
        const float col = (contentW - 20.0f) / 3.0f;
        g.panel(D2D1::RectF(x, y, x + col, y + lowH), L"▸ SUB-AGENTS",
            formatII(L"%d playbooks", frame.agentCount, 0).c_str(), kCyan);
        {
            float ay = y + 32;
            for (const auto& agent : frame.agents) {
                if (ay + 36 > y + lowH - 8) break;
                const auto tint = agent.live ? kGreen : kMuted;
                g.ring(x + 28, ay + 16, 12, agent.activity / 100.f, tint);
                g.text(g.monoFont.Get(), D2D1::RectF(x + 48, ay, x + col - 10, ay + 16), agent.name.c_str(), kText);
                g.text(g.microFont.Get(), D2D1::RectF(x + 48, ay + 16, x + col - 10, ay + 30), agent.status.c_str(), kMuted);
                ay += 36;
            }
        }
        g.panel(D2D1::RectF(x + col + 10, y, x + 2 * col + 10, y + lowH), L"▸ HOT PROCESSES", L"CPU · RSS", kOrange);
        {
            float py = y + 32;
            g.text(g.microFont.Get(), D2D1::RectF(x + col + 20, py, x + 2 * col, py + 14), L"PID   NAME                CPU        RSS", kMuted);
            py += 16;
            for (const auto& proc : frame.processes) {
                if (py + 16 > y + lowH - 8) break;
                const auto pid = formatII(L"%d", proc.pid, 0);
                g.text(g.microFont.Get(), D2D1::RectF(x + col + 20, py, x + col + 62, py + 14), pid.c_str(), kMuted);
                g.text(g.monoFont.Get(), D2D1::RectF(x + col + 64, py, x + 2 * col - 90, py + 14), proc.name.c_str(), kText);
                g.bar(x + 2 * col - 86, py + 3, 44, 8, std::clamp(proc.cpu / 100.f, 0.f, 1.f),
                    proc.cpu > 50.f ? kOrange : kCyan);
                g.text(g.microFont.Get(), D2D1::RectF(x + 2 * col - 38, py, x + 2 * col, py + 14),
                    proc.rss.c_str(), kMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);
                py += 16;
            }
        }
        g.panel(D2D1::RectF(x + 2 * col + 20, y, x + contentW, y + lowH), L"▸ LIVE STREAM",
            formatII(L"%d events", static_cast<int>(frame.feed.size()), 0).c_str(), kAmber);
        {
            float fy = y + 32;
            for (const auto& ev : frame.feed) {
                if (fy + 16 > y + lowH - 8) break;
                g.text(g.microFont.Get(), D2D1::RectF(x + 2 * col + 30, fy, x + 2 * col + 86, fy + 14),
                    ev.timestamp.c_str(), kMuted);
                g.text(g.microFont.Get(), D2D1::RectF(x + 2 * col + 88, fy, x + 2 * col + 130, fy + 14),
                    ev.status.c_str(), c(ev.r, ev.g, ev.b));
                g.text(g.monoFont.Get(), D2D1::RectF(x + 2 * col + 132, fy, x + contentW - 56, fy + 14),
                    ev.tool.c_str(), kText);
                if (!ev.duration.empty()) {
                    g.text(g.microFont.Get(), D2D1::RectF(x + contentW - 54, fy, x + contentW - 10, fy + 14),
                        ev.duration.c_str(), kMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);
                }
                fy += 16;
            }
        }
        pushY(lowH + 16);
    } else {
        g.text(g.titleFont.Get(), D2D1::RectF(x, y, x + 400, y + 30), labels[static_cast<int>(frame.tab)], kCyan);
        pushY(36);
        if (frame.tab == SurfaceTab::Mcp) {
            g.panel(D2D1::RectF(x, y, x + contentW, y + 88), L"▸ ACTIONS", L"product flow", kCyan);
            g.button(D2D1::RectF(x + 14, y + 42, x + 202, y + 76), L"Deploy to LM Studio", SurfaceHit::Deploy,
                c(0.00f, 0.48f, 0.58f));
            g.button(D2D1::RectF(x + 212, y + 42, x + 314, y + 76), L"Refresh", SurfaceHit::RefreshMcp,
                c(0.16f, 0.22f, 0.28f));
            g.button(D2D1::RectF(x + 324, y + 42, x + 474, y + 76), L"Prune presence", SurfaceHit::PrunePresence,
                c(0.25f, 0.18f, 0.16f));
            g.text(g.microFont.Get(), D2D1::RectF(x + 490, y + 48, x + contentW - 16, y + 72),
                L"Install LM Studio  →  Deploy  →  chat with MCP on", kMuted);
            pushY(100);
            const auto bannerTint = frame.pluginOk ? kGreen : kAmber;
            g.panel(D2D1::RectF(x, y, x + contentW, y + 78), L"▸ CONNECTION",
                frame.pluginOk ? L"DEPLOYED" : L"NOT DEPLOYED", bannerTint);
            g.text(g.labelFont.Get(), D2D1::RectF(x + 14, y + 40, x + contentW - 14, y + 58),
                frame.pluginOk ? L"LM Studio connection deployed" : L"Not fully deployed to LM Studio", bannerTint);
            g.text(g.bodyFont.Get(), D2D1::RectF(x + 14, y + 56, x + contentW - 14, y + 74),
                frame.pluginStatus.empty() ? L"Deploy writes primary + failover mcp.json and plugin dirs."
                                           : frame.pluginStatus.c_str(),
                kMuted);
            pushY(90);
            const float cardH = 124.0f;
            const float cardW = (contentW - 14) / 2.0f;
            const int mcpCount = (std::max)(static_cast<int>(frame.mcpCards.size()), 1);
            const int mcpRows = (mcpCount + 1) / 2;
            g.panel(D2D1::RectF(x, y, x + contentW, y + 40.0f + static_cast<float>(mcpRows) * (cardH + 10) + 8),
                L"▸ MCP SERVERS", formatII(L"%d cards", static_cast<int>(frame.mcpCards.size()), 0).c_str(), kCyan);
            if (frame.mcpCards.empty()) {
                g.text(g.bodyFont.Get(), D2D1::RectF(x + 16, y + 44, x + contentW - 16, y + 68),
                    L"No LM Studio MCP activity yet. Deploy, then open a chat with MCP enabled.", kMuted);
            }
            int i = 0;
            for (const auto& card : frame.mcpCards) {
                const int col = i % 2;
                const int row = i / 2;
                const float bx = x + 14 + static_cast<float>(col) * (cardW + 10);
                const float by = y + 42 + static_cast<float>(row) * (cardH + 10);
                const auto tint = card.live ? kGreen : kMuted;
                g.fillRound(D2D1::RectF(bx, by, bx + cardW - 8, by + cardH), 8.0f, kCard);
                g.strokeRound(D2D1::RectF(bx, by, bx + cardW - 8, by + cardH), 8.0f, c(tint.r, tint.g, tint.b, 0.55f), 1.4f);
                g.ring(bx + 36, by + 44, 22, card.activity / 100.f, tint);
                g.text(g.microFont.Get(), D2D1::RectF(bx + 20, by + 34, bx + 54, by + 54),
                    card.live ? L"ON" : L"SB", kText, DWRITE_TEXT_ALIGNMENT_CENTER);
                g.text(g.monoFont.Get(), D2D1::RectF(bx + 68, by + 12, bx + cardW - 20, by + 34), card.label.c_str(), kText);
                g.text(g.microFont.Get(), D2D1::RectF(bx + 68, by + 34, bx + cardW - 20, by + 52), card.role.c_str(), kMuted);
                g.text(g.bodyFont.Get(), D2D1::RectF(bx + 14, by + 78, bx + cardW - 20, by + 110), card.meta.c_str(), kMuted);
                ++i;
            }
            pushY(48.0f + static_cast<float>(mcpRows) * (cardH + 10) + 16);
        } else if (frame.tab == SurfaceTab::Agents) {
            const float cardH = 128.0f;
            const float cardW = (contentW - 28) / 3.0f;
            const int agentRows = (std::max)(1, (static_cast<int>(frame.agents.size()) + 2) / 3);
            const float gridH = 88.0f + static_cast<float>(agentRows) * (cardH + 12);
            g.panel(D2D1::RectF(x, y, x + contentW, y + gridH), L"▸ SUB-AGENTS",
                formatII(L"%d playbooks", frame.agentCount, 0).c_str(), kCyan);
            g.button(D2D1::RectF(x + 14, y + 42, x + 194, y + 74), L"Prune idle sessions",
                SurfaceHit::PruneSessions, c(0.16f, 0.22f, 0.28f));
            g.text(g.microFont.Get(), D2D1::RectF(x + 208, y + 48, x + contentW - 16, y + 70),
                L"Health is live when an agent session is open.", kMuted);
            int i = 0;
            for (const auto& agent : frame.agents) {
                const int col = i % 3;
                const int row = i / 3;
                const float bx = x + 14 + static_cast<float>(col) * (cardW + 10);
                const float by = y + 86 + static_cast<float>(row) * (cardH + 12);
                const auto tint = agent.live ? kGreen : kCyan;
                g.fillRound(D2D1::RectF(bx, by, bx + cardW - 6, by + cardH), 8.0f, kCard);
                g.strokeRound(D2D1::RectF(bx, by, bx + cardW - 6, by + cardH), 8.0f, c(tint.r, tint.g, tint.b, 0.50f), 1.4f);
                g.ring(bx + 28, by + 28, 14, agent.live ? 0.85f : 0.12f, tint);
                g.text(g.labelFont.Get(), D2D1::RectF(bx + 50, by + 10, bx + cardW - 14, by + 32), agent.name.c_str(), kText);
                g.text(g.microFont.Get(), D2D1::RectF(bx + 50, by + 32, bx + cardW - 14, by + 48), agent.health.c_str(), tint);
                g.text(g.bodyFont.Get(), D2D1::RectF(bx + 12, by + 56, bx + cardW - 14, by + 96), agent.summary.c_str(), kMuted);
                g.text(g.monoFont.Get(), D2D1::RectF(bx + 12, by + 100, bx + cardW - 14, by + 118), agent.status.c_str(), kCyan);
                ++i;
            }
            pushY(gridH + 16);
        } else if (frame.tab == SurfaceTab::Tools) {
            const float listH = 96.0f + 20.0f * static_cast<float>((std::max)(static_cast<int>(frame.tools.size()), 1));
            g.panel(D2D1::RectF(x, y, x + contentW, y + listH), L"▸ TOOL CATALOG",
                formatII(L"%d tools", static_cast<int>(frame.tools.size()), 0).c_str(), kCyan);
            g.text(g.bodyFont.Get(), D2D1::RectF(x + 14, y + 40, x + 70, y + 66), L"Filter", kMuted);
            g.fillRound(D2D1::RectF(x + 70, y + 42, x + 360, y + 66), 5.0f, c(0.04f, 0.06f, 0.08f));
            g.strokeRound(D2D1::RectF(x + 70, y + 42, x + 360, y + 66), 5.0f, c(0.08f, 0.90f, 0.96f, 0.35f));
            const auto filter = frame.toolFilter.empty() ? L"(type to filter)" : frame.toolFilter.c_str();
            g.text(g.monoFont.Get(), D2D1::RectF(x + 80, y + 42, x + 350, y + 66), filter,
                frame.toolFilter.empty() ? kMuted : kText);
            g.text(g.microFont.Get(), D2D1::RectF(x + 372, y + 46, x + contentW - 16, y + 64),
                L"READY = registered. IDLE until invoked.", kMuted);
            float ry = y + 76;
            g.text(g.microFont.Get(), D2D1::RectF(x + 16, ry, x + 236, ry + 16), L"NAME", kCyan);
            g.text(g.microFont.Get(), D2D1::RectF(x + 246, ry, x + 356, ry + 16), L"PACK", kCyan);
            g.text(g.microFont.Get(), D2D1::RectF(x + 366, ry, x + 466, ry + 16), L"HEALTH", kCyan);
            g.text(g.microFont.Get(), D2D1::RectF(x + 476, ry, x + 636, ry + 16), L"EVENTS", kCyan);
            ry += 20;
            int stripe = 0;
            for (const auto& tool : frame.tools) {
                if ((stripe++ % 2) == 0) {
                    g.fillRect(D2D1::RectF(x + 10, ry - 2, x + contentW - 10, ry + 18), c(1, 1, 1, 0.03f));
                }
                g.text(g.monoFont.Get(), D2D1::RectF(x + 16, ry, x + 236, ry + 16), tool.name.c_str(), kText);
                g.text(g.microFont.Get(), D2D1::RectF(x + 246, ry, x + 356, ry + 16), tool.pack.c_str(), kMuted);
                g.text(g.microFont.Get(), D2D1::RectF(x + 366, ry, x + 466, ry + 16), tool.health.c_str(),
                    tool.events > 0 ? kMint : kMuted);
                const auto ev = formatII(L"%d", tool.events, 0);
                g.text(g.microFont.Get(), D2D1::RectF(x + 476, ry, x + 536, ry + 16), ev.c_str(), kMuted);
                g.bar(x + 546, ry + 4, 140, 8, tool.activity / 100.f, kCyan);
                ry += 20;
            }
            pushY(listH + 16);
        } else if (frame.tab == SurfaceTab::Feed) {
            const auto counts = formatII(L"ERR %d   DEN %d", frame.feedErrors, frame.feedDenied);
            const float listH = 56.0f + 22.0f * static_cast<float>((std::max)(static_cast<int>(frame.feed.size()), 1));
            g.panel(D2D1::RectF(x, y, x + contentW, y + listH), L"▸ LIVE STREAM", counts.c_str(), kAmber);
            float fy = y + 40;
            if (frame.feed.empty()) {
                g.text(g.bodyFont.Get(), D2D1::RectF(x + 16, fy, x + contentW - 16, fy + 24),
                    L"Empty. Deploy, then open an LM Studio chat with MCP enabled.", kMuted);
            }
            for (const auto& ev : frame.feed) {
                g.text(g.microFont.Get(), D2D1::RectF(x + 16, fy, x + 96, fy + 18), ev.timestamp.c_str(), kMuted);
                g.text(g.microFont.Get(), D2D1::RectF(x + 104, fy, x + 166, fy + 18), ev.status.c_str(), c(ev.r, ev.g, ev.b));
                g.text(g.monoFont.Get(), D2D1::RectF(x + 174, fy, x + contentW - 90, fy + 18), ev.tool.c_str(), kText);
                if (ev.durationFrac > 0) {
                    g.bar(x + contentW - 200, fy + 5, 50, 6, ev.durationFrac, kMint);
                }
                g.text(g.microFont.Get(), D2D1::RectF(x + contentW - 86, fy, x + contentW - 16, fy + 18),
                    ev.duration.c_str(), kMuted, DWRITE_TEXT_ALIGNMENT_TRAILING);
                fy += 22;
            }
            pushY(listH + 16);
        } else if (frame.tab == SurfaceTab::Diagnostics) {
            g.panel(D2D1::RectF(x, y, x + contentW, y + 88), L"▸ EXPORT", L"jsonl → json + md", kCyan);
            g.button(D2D1::RectF(x + 14, y + 42, x + 144, y + 76), L"Refresh log", SurfaceHit::RefreshDiagnostics,
                c(0.16f, 0.22f, 0.28f));
            g.button(D2D1::RectF(x + 154, y + 42, x + 374, y + 76), L"Export JSON + Markdown",
                SurfaceHit::ExportDiagnostics, c(0.00f, 0.48f, 0.58f));
            g.text(g.microFont.Get(), D2D1::RectF(x + 388, y + 48, x + contentW - 16, y + 72),
                L"%USERPROFILE%\\.forge-conductor\\logs\\diagnostics.jsonl", kMuted);
            pushY(100);
            const float listH = 48.0f + 20.0f * static_cast<float>((std::max)(static_cast<int>(frame.diagnostics.size()), 1));
            g.panel(D2D1::RectF(x, y, x + contentW, y + listH), L"▸ DIAGNOSTIC LOG",
                formatII(L"%d records", static_cast<int>(frame.diagnostics.size()), 0).c_str(), kOrange);
            float dy = y + 40;
            if (frame.diagnostics.empty()) {
                g.text(g.bodyFont.Get(), D2D1::RectF(x + 16, dy, x + contentW - 16, dy + 24),
                    L"No diagnostic records yet. Deploy or use tools to generate events.", kMuted);
            }
            for (const auto& row : frame.diagnostics) {
                g.text(g.microFont.Get(), D2D1::RectF(x + 16, dy, x + 86, dy + 18), row.b.c_str(),
                    row.b == L"error" ? kRed : (row.b == L"warn" ? kAmber : kMuted));
                g.text(g.monoFont.Get(), D2D1::RectF(x + 92, dy, x + 336, dy + 18), row.a.c_str(), kText);
                g.text(g.bodyFont.Get(), D2D1::RectF(x + 342, dy, x + contentW - 16, dy + 18), row.c.c_str(), kMuted);
                dy += 20;
            }
            pushY(listH + 16);
        } else if (frame.tab == SurfaceTab::Manager) {
            g.panel(D2D1::RectF(x, y, x + contentW, y + 96), L"▸ SERVICE",
                frame.managerAlive ? L"RUNNING" : L"STOPPED", frame.managerAlive ? kGreen : kAmber);
            g.button(D2D1::RectF(x + 14, y + 46, x + 114, y + 80), L"Start", SurfaceHit::ManagerStart,
                c(0.08f, 0.42f, 0.28f));
            g.button(D2D1::RectF(x + 124, y + 46, x + 224, y + 80), L"Stop", SurfaceHit::ManagerStop,
                c(0.42f, 0.16f, 0.14f));
            g.button(D2D1::RectF(x + 234, y + 46, x + 344, y + 80), L"Restart", SurfaceHit::ManagerRestart,
                c(0.00f, 0.48f, 0.58f));
            g.button(D2D1::RectF(x + 354, y + 46, x + 534, y + 80),
                frame.startWithWindows ? L"Login: ON" : L"Login: OFF", SurfaceHit::ManagerLogin,
                c(0.16f, 0.22f, 0.28f));
            pushY(108);
            const float setH = 48.0f + 22.0f * static_cast<float>((std::max)(static_cast<int>(frame.rows.size()), 1));
            g.panel(D2D1::RectF(x, y, x + contentW, y + setH), L"▸ RUNTIME / SETTINGS", L"config.json", kCyan);
            float my = y + 42;
            for (const auto& row : frame.rows) {
                g.text(g.microFont.Get(), D2D1::RectF(x + 16, my, x + 236, my + 20), row.a.c_str(), kMuted);
                g.text(g.monoFont.Get(), D2D1::RectF(x + 246, my, x + 536, my + 20), row.b.c_str(), kText);
                g.text(g.bodyFont.Get(), D2D1::RectF(x + 546, my, x + contentW - 16, my + 20), row.c.c_str(), kMuted);
                my += 22;
            }
            pushY(setH + 12);
            g.panel(D2D1::RectF(x, y, x + contentW, y + 88), L"▸ NOTES", L"Windows 0.1.0", kMuted);
            g.text(g.bodyFont.Get(), D2D1::RectF(x + 16, y + 40, x + contentW - 16, y + 78),
                L"Start/Stop/Restart the headless manager. Login registers a per-user Task Scheduler job. "
                L"Deploy lives on the LM Studio MCP tab. Diagnostics export is on Diagnostics.",
                kMuted);
            pushY(104);
        }
    }

    g.contentH = y + scroll;
    g.scrollY = std::clamp(g.scrollY, 0.0f, g.maxScroll());

    if (g.maxScroll() > 1.0f) {
        const float trackX = w - 8;
        const float trackTop = 10;
        const float trackH = h - kStatus - 20;
        const float thumbH = (std::max)(28.0f, trackH * ((h - kStatus) / (g.contentH + 1.0f)));
        const float thumbY = trackTop + (trackH - thumbH) * (g.scrollY / g.maxScroll());
        g.fillRound(D2D1::RectF(trackX, thumbY, trackX + 5, thumbY + thumbH), 2.0f, c(0.08f, 0.90f, 0.96f, 0.45f));
    }

    g.fillRect(D2D1::RectF(0, h - kStatus, w, h), c(0.012f, 0.016f, 0.028f));
    g.fillRect(D2D1::RectF(0, h - kStatus, w, h - kStatus + 1), c(0.08f, 0.90f, 0.96f, 0.18f));
    g.text(g.monoFont.Get(), D2D1::RectF(16, h - 24, w - 16, h - 4),
        frame.status.empty() ? L"Forge Conductor for Windows" : frame.status.c_str(), kMuted);

    if (FAILED(g.d2d->EndDraw())) {
        impl_->createTarget();
    }
    impl_->swap->Present(1, 0);
}

} // namespace Forge::Ui
