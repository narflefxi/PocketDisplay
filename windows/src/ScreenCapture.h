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

    // Initialize the duplication. If already initialized with same adapter/output,
    // returns true without re-creating (idempotent).
    // If external_capture_mode_ is true, Initialize() will NOT release
    // the existing duplication even if adapter/output differ - caller must
    // explicitly call ForceReinitialize() for output changes.
    bool Initialize(int adapter_idx = 0, int output_idx = 0);

    // Force re-initialization (use when output changes, e.g., VDD appears/disappears).
    // This releases the old duplication and creates a new one.
    bool ForceReinitialize(int adapter_idx = 0, int output_idx = 0);

    // Returns false if no new frame available within timeout, true on success.
    bool CaptureFrame(std::vector<uint8_t>& bgra_out, int& width, int& height);
    int  GetWidth()       const { return width_; }
    int  GetHeight()      const { return height_; }
    RECT GetMonitorRect() const { return monitor_rect_; }
    int  GetAdapterIdx()  const { return adapter_idx_; }
    int  GetOutputIdx()   const { return output_idx_; }
    bool IsInitialized()  const { return duplication_ != nullptr; }

    // In external capture mode, Release() does NOT release the duplication.
    // This allows sessions to "stop" without destroying the capture.
    void SetExternalCaptureMode(bool external) { external_capture_mode_ = external; }
    bool GetExternalCaptureMode() const { return external_capture_mode_; }

    // Full release of all resources including duplication.
    // Safe to call even in external mode - caller must explicitly Release
    // when truly shutting down (e.g., process exit).
    void Release();

private:
    bool CreateStagingTexture(int width, int height);
    bool DoInitialize(int adapter_idx, int output_idx, bool force);

    Microsoft::WRL::ComPtr<ID3D11Device>           device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>    context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>        staging_;
    int  width_  = 0;
    int  height_ = 0;
    RECT monitor_rect_ = {};
    int  adapter_idx_ = 0;
    int  output_idx_  = 0;
    bool external_capture_mode_ = false;  // When true, Release() keeps duplication alive
};
