#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

class ScreenCapture {
public:
    ScreenCapture() = default;
    ~ScreenCapture();

    bool Initialize(int adapter_idx = 0, int output_idx = 0);
    // Returns false if no new frame available within timeout, true on success.
    bool CaptureFrame(std::vector<uint8_t>& bgra_out, int& width, int& height);
    int GetWidth()  const { return width_; }
    int GetHeight() const { return height_; }
    void Release();

private:
    bool CreateStagingTexture(int width, int height);

    Microsoft::WRL::ComPtr<ID3D11Device>           device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>    context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>        staging_;
    int width_  = 0;
    int height_ = 0;
};
