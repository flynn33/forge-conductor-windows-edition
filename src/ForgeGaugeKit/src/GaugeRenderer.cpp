// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeGaugeKit/GaugeRenderer.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace Forge::Gauge {
namespace {

constexpr const char* kShader = R"(
struct VSIn { float2 pos : POSITION; float4 col : COLOR; };
struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR; };
VSOut vs_main(VSIn input) {
    VSOut o;
    o.pos = float4(input.pos, 0, 1);
    o.col = input.col;
    return o;
}
float4 ps_main(VSOut input) : SV_Target { return input.col; }
)";

struct Vertex {
    float x, y;
    float r, g, b, a;
};

void addQuad(std::vector<Vertex>& v, float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
    v.push_back({x0, y0, r, g, b, a});
    v.push_back({x1, y0, r, g, b, a});
    v.push_back({x0, y1, r, g, b, a});
    v.push_back({x1, y0, r, g, b, a});
    v.push_back({x1, y1, r, g, b, a});
    v.push_back({x0, y1, r, g, b, a});
}

} // namespace

struct GaugeRenderer::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11BlendState> blend;
    std::uint32_t width{0};
    std::uint32_t height{0};

    bool createTarget() {
        rtv.Reset();
        ComPtr<ID3D11Texture2D> backBuffer;
        if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
            return false;
        }
        return SUCCEEDED(device->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv));
    }
};

GaugeRenderer::GaugeRenderer() : impl_(std::make_unique<Impl>()) {}
GaugeRenderer::~GaugeRenderer() { detach(); }

bool GaugeRenderer::attach(HWND hwnd, std::uint32_t width, std::uint32_t height) {
    detach();
    impl_->width = width;
    impl_->height = height;

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Width = width;
    desc.BufferDesc.Height = height;
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    D3D_FEATURE_LEVEL level{};
    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels,
            1,
            D3D11_SDK_VERSION,
            &desc,
            &impl_->swapChain,
            &impl_->device,
            &level,
            &impl_->context))) {
        return false;
    }
    if (!impl_->createTarget()) {
        return false;
    }

    ComPtr<ID3DBlob> vsBlob, psBlob, errors;
    if (FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vsBlob, &errors))) {
        return false;
    }
    if (FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &psBlob, &errors))) {
        return false;
    }
    impl_->device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &impl_->vs);
    impl_->device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &impl_->ps);
    D3D11_INPUT_ELEMENT_DESC input[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    impl_->device->CreateInputLayout(input, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &impl_->layout);

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    impl_->device->CreateBlendState(&blend, &impl_->blend);
    return true;
}

void GaugeRenderer::resize(std::uint32_t width, std::uint32_t height) {
    if (!impl_->swapChain || width == 0 || height == 0) {
        return;
    }
    impl_->width = width;
    impl_->height = height;
    impl_->rtv.Reset();
    impl_->context->OMSetRenderTargets(0, nullptr, nullptr);
    impl_->swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    impl_->createTarget();
}

void GaugeRenderer::render(const GaugeSample& sample) {
    if (!impl_->context || !impl_->rtv) {
        return;
    }
    const float clear[4] = {0.008f, 0.016f, 0.04f, 1.0f};
    impl_->context->OMSetRenderTargets(1, impl_->rtv.GetAddressOf(), nullptr);
    impl_->context->ClearRenderTargetView(impl_->rtv.Get(), clear);
    D3D11_VIEWPORT viewport{0, 0, static_cast<float>(impl_->width), static_cast<float>(impl_->height), 0, 1};
    impl_->context->RSSetViewports(1, &viewport);

    auto toNdcX = [&](float px) { return (px / static_cast<float>(impl_->width)) * 2.f - 1.f; };
    auto toNdcY = [&](float py) { return 1.f - (py / static_cast<float>(impl_->height)) * 2.f; };

    std::vector<Vertex> vertices;
    const float w = static_cast<float>(impl_->width);

    auto bar = [&](float x, float y, float bw, float bh, float frac, float r, float g, float b) {
        addQuad(vertices, toNdcX(x), toNdcY(y + bh), toNdcX(x + bw), toNdcY(y), 0.05f, 0.12f, 0.18f, 1);
        const float fill = bw * std::clamp(frac, 0.f, 1.f);
        addQuad(vertices, toNdcX(x), toNdcY(y + bh), toNdcX(x + fill), toNdcY(y), r, g, b, 0.95f);
        addQuad(vertices, toNdcX(x), toNdcY(y + 3), toNdcX(x + fill), toNdcY(y), r, g, b, 0.25f);
    };
    auto pill = [&](float x, float y, bool on, float r, float g, float b) {
        addQuad(vertices, toNdcX(x), toNdcY(y + 28), toNdcX(x + 72), toNdcY(y), 0.04f, 0.08f, 0.12f, 1);
        if (on) {
            addQuad(vertices, toNdcX(x + 4), toNdcY(y + 24), toNdcX(x + 68), toNdcY(y + 4), r, g, b, 0.85f);
        }
    };

    pill(w - 330, 18, sample.linkHealthy, 0.18f, 1.f, 0.55f);
    pill(w - 248, 18, sample.mcpAlive, 0.09f, 0.94f, 1.f);
    pill(w - 166, 18, sample.managerAlive, 1.f, 0.42f, 0.12f);
    pill(w - 84, 18, true, 0.75f, 0.45f, 1.f);

    bar(24, 86, w - 48, 22, sample.cpu / 100.f, 0.09f, 0.94f, 1.f);
    bar(24, 118, w - 48, 22, sample.ram / 100.f, 0.18f, 1.f, 0.55f);
    bar(24, 150, w - 48, 22, sample.gpu / 100.f, 1.f, 0.42f, 0.12f);

    const float chartX = 24, chartY = 200, chartW = w - 48, chartH = 180;
    addQuad(vertices, toNdcX(chartX), toNdcY(chartY + chartH), toNdcX(chartX + chartW), toNdcY(chartY), 0.02f, 0.05f, 0.08f, 1);
    auto trace = [&](const std::vector<float>& hist, float r, float g, float b) {
        if (hist.size() < 2) return;
        for (std::size_t i = 1; i < hist.size(); ++i) {
            const float x0 = chartX + chartW * (static_cast<float>(i - 1) / static_cast<float>(hist.size() - 1));
            const float x1 = chartX + chartW * (static_cast<float>(i) / static_cast<float>(hist.size() - 1));
            const float y0 = chartY + chartH - chartH * std::clamp(hist[i - 1] / 100.f, 0.f, 1.f);
            const float y1 = chartY + chartH - chartH * std::clamp(hist[i] / 100.f, 0.f, 1.f);
            addQuad(vertices, toNdcX(x0), toNdcY(y0 + 2), toNdcX(x1), toNdcY(y1 - 2), r, g, b, 0.9f);
        }
    };
    trace(sample.cpuHistory, 0.09f, 0.94f, 1.f);
    trace(sample.ramHistory, 0.18f, 1.f, 0.55f);
    trace(sample.gpuHistory, 1.f, 0.42f, 0.12f);

    float coreY = chartY + chartH + 24;
    const float coreW = std::max(8.f, (w - 48.f) / std::max<std::size_t>(1, sample.perCore.size()) - 4.f);
    for (std::size_t i = 0; i < sample.perCore.size() && i < 64; ++i) {
        const float x = 24.f + static_cast<float>(i) * (coreW + 4.f);
        bar(x, coreY, coreW, 48.f, sample.perCore[i] / 100.f, 0.09f, 0.94f, 1.f);
    }
    float diskY = coreY + 64.f;
    for (std::size_t i = 0; i < sample.disks.size() && i < 8; ++i) {
        bar(24.f, diskY, w - 48.f, 16.f, sample.disks[i] / 100.f, 0.75f, 0.45f, 1.f);
        diskY += 22.f;
    }

    if (!vertices.empty()) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(Vertex));
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA data{vertices.data(), 0, 0};
        ComPtr<ID3D11Buffer> vb;
        impl_->device->CreateBuffer(&bd, &data, &vb);
        UINT stride = sizeof(Vertex), offset = 0;
        impl_->context->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
        impl_->context->IASetInputLayout(impl_->layout.Get());
        impl_->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        impl_->context->VSSetShader(impl_->vs.Get(), nullptr, 0);
        impl_->context->PSSetShader(impl_->ps.Get(), nullptr, 0);
        const float blendFactor[4]{};
        impl_->context->OMSetBlendState(impl_->blend.Get(), blendFactor, 0xffffffff);
        impl_->context->Draw(static_cast<UINT>(vertices.size()), 0);
    }
    impl_->swapChain->Present(1, 0);
}

bool GaugeRenderer::isAttached() const {
    return impl_ && impl_->swapChain;
}

void GaugeRenderer::detach() {
    if (impl_) {
        impl_->rtv.Reset();
        impl_->swapChain.Reset();
        impl_->context.Reset();
        impl_->device.Reset();
    }
}

} // namespace Forge::Gauge
