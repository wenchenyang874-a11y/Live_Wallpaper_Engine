#include "render/D3DRenderer.h"

#include <array>
#include <cmath>
#include <cstring>
#include <sstream>
#include <vector>

#include <d3d10.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include "core/Logger.h"

namespace lwe::render {
namespace {

struct VideoVertex final {
    float x = 0.0F;
    float y = 0.0F;
    float u = 0.0F;
    float v = 0.0F;
};

constexpr char kVideoShaderSource[] = R"(
Texture2D videoTexture : register(t0);
SamplerState videoSampler : register(s0);

struct VertexInput { float2 position : POSITION; float2 uv : TEXCOORD0; };
struct PixelInput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };

PixelInput VSMain(VertexInput input) {
    PixelInput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.uv = input.uv;
    return output;
}

float4 PSMain(PixelInput input) : SV_TARGET {
    float3 rgb = videoTexture.Sample(videoSampler, input.uv).rgb;
    return float4(rgb, 1.0);
}
)";

HRESULT CompileVideoShader(const char* entryPoint, const char* target,
                           Microsoft::WRL::ComPtr<ID3DBlob>& bytecode) {
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(
        kVideoShaderSource, std::size(kVideoShaderSource) - 1U,
        "LiveWallpaperVideoComposition", nullptr, nullptr, entryPoint, target,
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &bytecode, &errors);
    if (FAILED(result) && errors) {
        const std::string message(
            static_cast<const char*>(errors->GetBufferPointer()),
            errors->GetBufferSize());
        core::LogError(L"D3D video shader compilation failed: " +
                       std::wstring(message.begin(), message.end()), result);
    }
    return result;
}

}  // namespace

D3DRenderer::~D3DRenderer() {
    Shutdown();
}

bool D3DRenderer::Initialize(const HWND targetWindow) {
    RECT client{};
    if (!GetClientRect(targetWindow, &client)) {
        core::LogError(L"Unable to read the wallpaper window size.",
                       HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }

    const UINT width = static_cast<UINT>(client.right - client.left);
    const UINT height = static_cast<UINT>(client.bottom - client.top);
    if (width == 0 || height == 0) {
        core::LogError(L"Wallpaper window is empty; D3D initialization was cancelled.");
        return false;
    }

    return CreateDevice() && CreateSwapChain(targetWindow, width, height) &&
           CreateRenderTarget() && CreateVideoCompositionPipeline();
}

bool D3DRenderer::CreateDevice() {
    constexpr std::array featureLevels{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    D3D_FEATURE_LEVEL selectedLevel{};
    // Media Foundation frame-server mode shares this device for hardware video
    // decode and processing. VIDEO_SUPPORT is harmless for image/GIF rendering
    // and avoids creating a second D3D device when a video is selected.
    constexpr UINT flags =
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT result = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, featureLevels.data(),
        static_cast<UINT>(featureLevels.size()), D3D11_SDK_VERSION, &device_, &selectedLevel,
        &context_);

    if (FAILED(result)) {
        core::LogWarning(L"Hardware D3D11 device creation failed; trying WARP software rendering.");
        result = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels.data(),
            static_cast<UINT>(featureLevels.size()), D3D11_SDK_VERSION, &device_, &selectedLevel,
            &context_);
    }

    if (FAILED(result)) {
        core::LogError(L"D3D11 device creation failed.", result);
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(device_.As(&multithread))) {
        // Media Foundation may use the shared DXGI device from decoder threads.
        // Protecting the immediate context prevents video frame transfer from
        // racing image/GIF rendering or swap-chain resize operations.
        multithread->SetMultithreadProtected(TRUE);
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    if (SUCCEEDED(device_.As(&dxgiDevice)) &&
        SUCCEEDED(dxgiDevice->GetAdapter(&adapter_))) {
        DXGI_ADAPTER_DESC description{};
        if (SUCCEEDED(adapter_->GetDesc(&description))) {
            core::LogInfo(L"D3D11 adapter: " + std::wstring(description.Description));
        }
    }

    std::wostringstream message;
    message << L"D3D11 feature level selected: 0x" << std::hex
            << static_cast<unsigned int>(selectedLevel) << L'.';
    core::LogInfo(message.str());
    return true;
}

bool D3DRenderer::CreateSwapChain(const HWND targetWindow, const UINT width,
                                  const UINT height) {
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIFactory> factory;

    HRESULT result = device_.As(&dxgiDevice);
    if (SUCCEEDED(result)) {
        result = dxgiDevice->GetAdapter(&adapter);
    }
    if (SUCCEEDED(result)) {
        result = adapter->GetParent(IID_PPV_ARGS(&factory));
    }
    if (FAILED(result)) {
        core::LogError(L"Unable to acquire the DXGI factory.", result);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferDesc.Width = width;
    description.BufferDesc.Height = height;
    description.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 1;
    description.OutputWindow = targetWindow;
    description.Windowed = TRUE;
    // A continuously-presented desktop child must not use DISCARD.
    // DXGI explicitly allows that mode to throw away the back-buffer contents
    // after Present; on the raised Windows 11 desktop some Intel/DWM paths show
    // that discarded state between video frames even though static one-shot
    // presentation looks correct. SEQUENTIAL keeps the BitBlt compatibility
    // required by this desktop host while preserving the last complete frame.
    description.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;

    // Windows 11 raised desktop explicitly supports DXGI BitBlt presents to a
    // fully opaque layered child that is z-ordered between DefView and WorkerW.
    // The former failure was caused by parenting below the WorkerW wallpaper,
    // not by the BitBlt swap chain itself.
    result = factory->CreateSwapChain(device_.Get(), &description, &swapChain_);
    if (FAILED(result)) {
        core::LogError(L"DXGI swap chain creation failed.", result);
        return false;
    }

    factory->MakeWindowAssociation(
        targetWindow, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    core::LogInfo(
        L"DXGI preserving BitBlt swap chain created for desktop compatibility.");
    return true;
}

bool D3DRenderer::CreateRenderTarget() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    const HRESULT result = AcquireBackBuffer(backBuffer);
    if (FAILED(result)) {
        core::LogError(L"Unable to obtain the swap chain back buffer.", result);
        return false;
    }

    const HRESULT viewResult =
        device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTarget_);
    if (FAILED(viewResult)) {
        core::LogError(L"Unable to create the D3D11 render target.", viewResult);
        return false;
    }
    return true;
}

bool D3DRenderer::CreateVideoCompositionPipeline() {
    Microsoft::WRL::ComPtr<ID3DBlob> vertexBytecode;
    Microsoft::WRL::ComPtr<ID3DBlob> pixelBytecode;
    HRESULT result = CompileVideoShader("VSMain", "vs_4_0", vertexBytecode);
    if (SUCCEEDED(result)) {
        result = CompileVideoShader("PSMain", "ps_4_0", pixelBytecode);
    }
    if (SUCCEEDED(result)) {
        result = device_->CreateVertexShader(
            vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(),
            nullptr, &videoVertexShader_);
    }
    if (SUCCEEDED(result)) {
        result = device_->CreatePixelShader(
            pixelBytecode->GetBufferPointer(), pixelBytecode->GetBufferSize(),
            nullptr, &videoPixelShader_);
    }
    constexpr D3D11_INPUT_ELEMENT_DESC elements[]{
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (SUCCEEDED(result)) {
        result = device_->CreateInputLayout(
            elements, static_cast<UINT>(std::size(elements)),
            vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(),
            &videoInputLayout_);
    }
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (SUCCEEDED(result)) {
        result = device_->CreateSamplerState(&sampler, &videoSampler_);
    }
    if (FAILED(result)) {
        core::LogError(L"Unable to create the D3D video composition pipeline.",
                       result);
        return false;
    }
    return true;
}

bool D3DRenderer::Resize(const UINT width, const UINT height) {
    if (!IsInitialized() || width == 0 || height == 0) {
        return true;
    }

    context_->OMSetRenderTargets(0, nullptr, nullptr);
    renderTarget_.Reset();
    videoTransferSurface_.Reset();
    videoSourceView_.Reset();
    videoCompositeTarget_.Reset();
    videoCompositeSurface_.Reset();
    videoVertexBuffer_.Reset();
    videoVertexCount_ = 0;
    context_->Flush();

    const HRESULT result =
        swapChain_->ResizeBuffers(1, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    if (FAILED(result)) {
        core::LogError(L"DXGI ResizeBuffers failed.", result);
        return false;
    }
    return CreateRenderTarget();
}

bool D3DRenderer::Render(const std::chrono::steady_clock::duration elapsed) {
    if (!IsInitialized()) {
        return false;
    }

    const float seconds = std::chrono::duration<float>(elapsed).count();
    // The spike used to change so slowly that a successful launch looked inert.
    // A brighter five-second blue/teal pulse makes rendering visibly testable.
    const float slowWave = (std::sin(seconds * 1.25F) + 1.0F) * 0.5F;
    const float secondWave = (std::sin(seconds * 0.85F + 1.7F) + 1.0F) * 0.5F;
    const float clearColor[4]{
        0.020F + slowWave * 0.040F,
        0.080F + secondWave * 0.220F,
        0.150F + slowWave * 0.300F,
        1.0F,
    };

    ID3D11RenderTargetView* target = renderTarget_.Get();
    context_->OMSetRenderTargets(1, &target, nullptr);
    context_->ClearRenderTargetView(renderTarget_.Get(), clearColor);

    return PresentCurrentFrame();
}

bool D3DRenderer::PresentStaticImage(const std::span<const std::uint8_t> bgraPixels,
                                     const UINT width, const UINT height,
                                     const UINT stride) {
    const std::uint64_t requiredStride = static_cast<std::uint64_t>(width) * 4U;
    const std::uint64_t requiredBytes = static_cast<std::uint64_t>(stride) * height;
    if (!IsInitialized() || width == 0 || height == 0 || stride < requiredStride ||
        requiredBytes > bgraPixels.size()) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT result = AcquireBackBuffer(backBuffer);
    if (FAILED(result)) {
        core::LogError(L"Unable to acquire the static wallpaper back buffer.", result);
        return false;
    }

    D3D11_TEXTURE2D_DESC description{};
    backBuffer->GetDesc(&description);
    if (description.Width != width || description.Height != height ||
        description.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        core::LogError(L"Static wallpaper dimensions do not match the swap chain.");
        return false;
    }

    context_->UpdateSubresource(backBuffer.Get(), 0, nullptr, bgraPixels.data(), stride, 0);
    return PresentCurrentFrame();
}

bool D3DRenderer::PresentImageRegions(
    const std::span<const ImageRegion> regions) {
    if (!IsInitialized() || regions.empty()) {
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT result = AcquireBackBuffer(backBuffer);
    if (FAILED(result)) {
        core::LogError(L"Unable to acquire the wallpaper region back buffer.", result);
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    backBuffer->GetDesc(&description);
    for (const ImageRegion& region : regions) {
        const std::uint64_t requiredStride =
            static_cast<std::uint64_t>(region.width) * 4U;
        const std::uint64_t requiredBytes =
            static_cast<std::uint64_t>(region.stride) * region.height;
        if (region.width == 0 || region.height == 0 ||
            region.stride < requiredStride ||
            requiredBytes > region.bgraPixels.size() || region.left < 0 ||
            region.top < 0 ||
            static_cast<std::uint64_t>(region.left) + region.width >
                description.Width ||
            static_cast<std::uint64_t>(region.top) + region.height >
                description.Height) {
            core::LogError(L"Wallpaper region exceeds the desktop render target.");
            return false;
        }
        const D3D11_BOX box{
            static_cast<UINT>(region.left), static_cast<UINT>(region.top), 0,
            static_cast<UINT>(region.left) + region.width,
            static_cast<UINT>(region.top) + region.height, 1};
        context_->UpdateSubresource(backBuffer.Get(), 0, &box,
                                    region.bgraPixels.data(), region.stride, 0);
    }
    return PresentCurrentFrame();
}

HRESULT D3DRenderer::AcquireBackBuffer(
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& backBuffer) const {
    backBuffer.Reset();
    if (!swapChain_) {
        return E_UNEXPECTED;
    }
    return swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
}

HRESULT D3DRenderer::AcquireVideoTransferSurface(
    const UINT sourceWidth, const UINT sourceHeight,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& transferSurface) {
    transferSurface.Reset();
    if (!device_ || sourceWidth == 0 || sourceHeight == 0) {
        return E_INVALIDARG;
    }

    bool recreate = !videoTransferSurface_;
    if (!recreate) {
        D3D11_TEXTURE2D_DESC existing{};
        videoTransferSurface_->GetDesc(&existing);
        recreate = existing.Width != sourceWidth ||
                   existing.Height != sourceHeight;
    }
    if (recreate) {
        videoSourceView_.Reset();
        videoTransferSurface_.Reset();
        // Decode and color-convert each Media Foundation frame exactly once at
        // the largest selected monitor's useful resolution. The texture is then
        // sampled by D3D for every selected monitor, avoiding one costly
        // TransferVideoFrame per screen and unnecessary 4K output conversion.
        D3D11_TEXTURE2D_DESC description{};
        description.Width = sourceWidth;
        description.Height = sourceHeight;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags =
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT result = device_->CreateTexture2D(&description, nullptr,
                                                  &videoTransferSurface_);
        if (SUCCEEDED(result)) {
            result = device_->CreateShaderResourceView(
                videoTransferSurface_.Get(), nullptr, &videoSourceView_);
        }
        if (FAILED(result)) {
            core::LogError(L"Unable to create the off-screen video surface.", result);
            return result;
        }
        std::wostringstream message;
        message << L"Video transfer surface created at " << sourceWidth << L'x'
                << sourceHeight << L" for one-transfer multi-display composition.";
        core::LogInfo(message.str());
    }
    transferSurface = videoTransferSurface_;
    return S_OK;
}

bool D3DRenderer::EnsureVideoCompositeSurface(const UINT width,
                                              const UINT height) {
    bool recreate = !videoCompositeSurface_;
    if (!recreate) {
        D3D11_TEXTURE2D_DESC existing{};
        videoCompositeSurface_->GetDesc(&existing);
        recreate = existing.Width != width || existing.Height != height;
    }
    if (!recreate) {
        return true;
    }
    videoCompositeTarget_.Reset();
    videoCompositeSurface_.Reset();
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;
    HRESULT result = device_->CreateTexture2D(
        &description, nullptr, &videoCompositeSurface_);
    if (SUCCEEDED(result)) {
        result = device_->CreateRenderTargetView(
            videoCompositeSurface_.Get(), nullptr, &videoCompositeTarget_);
    }
    if (FAILED(result)) {
        core::LogError(L"Unable to create the video composition surface.", result);
        return false;
    }
    return true;
}

bool D3DRenderer::UpdateVideoVertices(
    const std::span<const RECT> destinations, const UINT sourceWidth,
    const UINT sourceHeight, const UINT targetWidth, const UINT targetHeight) {
    std::vector<VideoVertex> vertices;
    vertices.reserve(destinations.size() * 6U);
    const float sourceAspect =
        static_cast<float>(sourceWidth) / static_cast<float>(sourceHeight);
    for (const RECT& destination : destinations) {
        const LONG destinationWidth = destination.right - destination.left;
        const LONG destinationHeight = destination.bottom - destination.top;
        if (destinationWidth <= 0 || destinationHeight <= 0 ||
            destination.left < 0 || destination.top < 0 ||
            destination.right > static_cast<LONG>(targetWidth) ||
            destination.bottom > static_cast<LONG>(targetHeight)) {
            return false;
        }
        const float destinationAspect =
            static_cast<float>(destinationWidth) /
            static_cast<float>(destinationHeight);
        float u0 = 0.0F;
        float v0 = 0.0F;
        float u1 = 1.0F;
        float v1 = 1.0F;
        if (sourceAspect > destinationAspect) {
            const float visibleWidth = destinationAspect / sourceAspect;
            u0 = (1.0F - visibleWidth) * 0.5F;
            u1 = u0 + visibleWidth;
        } else if (sourceAspect < destinationAspect) {
            const float visibleHeight = sourceAspect / destinationAspect;
            v0 = (1.0F - visibleHeight) * 0.5F;
            v1 = v0 + visibleHeight;
        }

        const float left = static_cast<float>(destination.left) * 2.0F /
                               static_cast<float>(targetWidth) -
                           1.0F;
        const float right = static_cast<float>(destination.right) * 2.0F /
                                static_cast<float>(targetWidth) -
                            1.0F;
        const float top = 1.0F - static_cast<float>(destination.top) * 2.0F /
                                     static_cast<float>(targetHeight);
        const float bottom =
            1.0F - static_cast<float>(destination.bottom) * 2.0F /
                       static_cast<float>(targetHeight);
        vertices.insert(vertices.end(), {
            {left, top, u0, v0}, {right, top, u1, v0},
            {right, bottom, u1, v1}, {left, top, u0, v0},
            {right, bottom, u1, v1}, {left, bottom, u0, v1},
        });
    }
    if (vertices.empty()) {
        return false;
    }

    const UINT requiredBytes =
        static_cast<UINT>(vertices.size() * sizeof(VideoVertex));
    bool recreate = !videoVertexBuffer_;
    if (!recreate) {
        D3D11_BUFFER_DESC existing{};
        videoVertexBuffer_->GetDesc(&existing);
        recreate = existing.ByteWidth < requiredBytes;
    }
    if (recreate) {
        videoVertexBuffer_.Reset();
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = requiredBytes;
        description.Usage = D3D11_USAGE_DYNAMIC;
        description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        const HRESULT result =
            device_->CreateBuffer(&description, nullptr, &videoVertexBuffer_);
        if (FAILED(result)) {
            core::LogError(L"Unable to create the video composition vertices.",
                           result);
            return false;
        }
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT result = context_->Map(videoVertexBuffer_.Get(), 0,
                                         D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(result)) {
        core::LogError(L"Unable to update the video composition vertices.", result);
        return false;
    }
    std::memcpy(mapped.pData, vertices.data(), requiredBytes);
    context_->Unmap(videoVertexBuffer_.Get(), 0);
    videoVertexCount_ = static_cast<UINT>(vertices.size());
    return true;
}

bool D3DRenderer::CommitVideoTransferSurface(
    const std::span<const RECT> destinations, const UINT sourceWidth,
    const UINT sourceHeight) {
    if (!videoTransferSurface_ || !videoSourceView_ || !context_ ||
        destinations.empty()) {
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    const HRESULT result = AcquireBackBuffer(backBuffer);
    if (FAILED(result)) {
        core::LogError(L"Unable to acquire the video presentation buffer.", result);
        return false;
    }
    D3D11_TEXTURE2D_DESC targetDescription{};
    backBuffer->GetDesc(&targetDescription);
    if (!EnsureVideoCompositeSurface(targetDescription.Width,
                                     targetDescription.Height) ||
        !UpdateVideoVertices(destinations, sourceWidth, sourceHeight,
                             targetDescription.Width, targetDescription.Height)) {
        return false;
    }

    ID3D11RenderTargetView* target = videoCompositeTarget_.Get();
    context_->OMSetRenderTargets(1, &target, nullptr);
    constexpr float opaqueBlack[4]{0.0F, 0.0F, 0.0F, 1.0F};
    context_->ClearRenderTargetView(target, opaqueBlack);
    const D3D11_VIEWPORT viewport{0.0F, 0.0F,
                                  static_cast<float>(targetDescription.Width),
                                  static_cast<float>(targetDescription.Height),
                                  0.0F, 1.0F};
    context_->RSSetViewports(1, &viewport);
    const UINT stride = sizeof(VideoVertex);
    const UINT offset = 0;
    ID3D11Buffer* vertexBuffer = videoVertexBuffer_.Get();
    context_->IASetInputLayout(videoInputLayout_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
    context_->VSSetShader(videoVertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(videoPixelShader_.Get(), nullptr, 0);
    ID3D11SamplerState* sampler = videoSampler_.Get();
    context_->PSSetSamplers(0, 1, &sampler);
    ID3D11ShaderResourceView* sourceView = videoSourceView_.Get();
    context_->PSSetShaderResources(0, 1, &sourceView);
    context_->Draw(videoVertexCount_, 0);
    ID3D11ShaderResourceView* noSource = nullptr;
    context_->PSSetShaderResources(0, 1, &noSource);

    context_->CopyResource(backBuffer.Get(), videoCompositeSurface_.Get());
    // The pixel shader always writes alpha=1 and the back buffer only changes
    // once per frame, so the layered desktop compositor never observes a
    // partially converted or transparent Media Foundation surface.
    return PresentCurrentFrame(true);
}

bool D3DRenderer::PresentCurrentFrame(const bool synchronize) {
    if (!swapChain_) {
        return false;
    }
    const HRESULT result = swapChain_->Present(synchronize ? 1U : 0U, 0);
    if (result == DXGI_STATUS_OCCLUDED) {
        Sleep(100);
        return true;
    }
    if (FAILED(result)) {
        core::LogError(L"DXGI Present failed.", result);
        return false;
    }
    return true;
}

std::optional<std::uint64_t> D3DRenderer::VideoMemoryUsage() const {
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
    if (!adapter_ || FAILED(adapter_.As(&adapter3))) {
        return std::nullopt;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO information{};
    const HRESULT result = adapter3->QueryVideoMemoryInfo(
        0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &information);
    if (FAILED(result)) {
        return std::nullopt;
    }
    return information.CurrentUsage;
}

void D3DRenderer::Shutdown() {
    if (context_) {
        context_->ClearState();
        context_->Flush();
    }
    renderTarget_.Reset();
    videoTransferSurface_.Reset();
    videoSourceView_.Reset();
    videoCompositeTarget_.Reset();
    videoCompositeSurface_.Reset();
    videoVertexBuffer_.Reset();
    videoSampler_.Reset();
    videoInputLayout_.Reset();
    videoPixelShader_.Reset();
    videoVertexShader_.Reset();
    videoVertexCount_ = 0;
    swapChain_.Reset();
    adapter_.Reset();
    context_.Reset();
    device_.Reset();
}

bool D3DRenderer::IsInitialized() const noexcept {
    return device_ && context_ && swapChain_ && renderTarget_;
}

ID3D11Device* D3DRenderer::Device() const noexcept {
    return device_.Get();
}

}  // namespace lwe::render
