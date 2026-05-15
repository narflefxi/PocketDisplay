#include "ScreenCapture.h"
#include <cstring>
#include <stdexcept>

ScreenCapture::~ScreenCapture() {
    Release();
}

bool ScreenCapture::Initialize(int /*adapter_idx*/, int output_idx) {
    D3D_FEATURE_LEVEL feat_level;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0, nullptr, 0, D3D11_SDK_VERSION,
        &device_, &feat_level, &context_
    );
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    hr = device_.As(&dxgi_device);
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device->GetAdapter(&adapter);
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    hr = adapter->EnumOutputs(output_idx, &output);
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
    hr = output.As(&output1);
    if (FAILED(hr)) return false;

    DXGI_OUTPUT_DESC desc = {};
    output->GetDesc(&desc);
    monitor_rect_ = desc.DesktopCoordinates;
    width_  = monitor_rect_.right  - monitor_rect_.left;
    height_ = monitor_rect_.bottom - monitor_rect_.top;

    // Round down to even — required for 4:2:0 chroma subsampling
    width_  &= ~1;
    height_ &= ~1;

    hr = output1->DuplicateOutput(device_.Get(), &duplication_);
    if (FAILED(hr)) return false;

    return CreateStagingTexture(width_, height_);
}

bool ScreenCapture::CreateStagingTexture(int width, int height) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width            = static_cast<UINT>(width);
    desc.Height           = static_cast<UINT>(height);
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;

    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &staging_);
    return SUCCEEDED(hr);
}

bool ScreenCapture::CaptureFrame(std::vector<uint8_t>& bgra_out, int& width, int& height) {
    DXGI_OUTDUPL_FRAME_INFO frame_info = {};
    Microsoft::WRL::ComPtr<IDXGIResource> resource;

    // 33 ms timeout — caller drives frame rate
    HRESULT hr = duplication_->AcquireNextFrame(33, &frame_info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (FAILED(hr)) {
        // Output went away (e.g. resolution change). Re-initialize on next call.
        duplication_.Reset();
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    hr = resource.As(&texture);
    if (FAILED(hr)) {
        duplication_->ReleaseFrame();
        return false;
    }

    context_->CopyResource(staging_.Get(), texture.Get());
    duplication_->ReleaseFrame();

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    width  = width_;
    height = height_;
    bgra_out.resize(static_cast<size_t>(width_) * height_ * 4);

    const auto* src = static_cast<const uint8_t*>(mapped.pData);
    uint8_t*    dst = bgra_out.data();
    const size_t row_bytes = static_cast<size_t>(width_) * 4;

    for (int y = 0; y < height_; ++y) {
        std::memcpy(dst + y * row_bytes, src + y * mapped.RowPitch, row_bytes);
    }

    context_->Unmap(staging_.Get(), 0);
    return true;
}

void ScreenCapture::Release() {
    staging_.Reset();
    duplication_.Reset();
    context_.Reset();
    device_.Reset();
}
