#include <d3d11.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <wincodec.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
// Capture the application's rendered back buffer, independent of overlapping desktop windows.
void save_rendered_frame(IDXGISwapChain *swap, ID3D11Device *device, ID3D11DeviceContext *context,
                         const wchar_t *path) {
    auto check = [](HRESULT h) {
        if (FAILED(h))
            throw std::runtime_error("Render capture failed: " +
                                     std::to_string(static_cast<unsigned long>(h)));
    };
    ComPtr<ID3D11Texture2D> source;
    check(swap->GetBuffer(0, IID_PPV_ARGS(&source)));
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    check(device->CreateTexture2D(&desc, nullptr, &staging));
    context->CopyResource(staging.Get(), source.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
        ComPtr<IWICImagingFactory> factory;
        check(
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
        ComPtr<IWICStream> stream;
        check(factory->CreateStream(&stream));
        check(stream->InitializeFromFilename(path, GENERIC_WRITE));
        ComPtr<IWICBitmapEncoder> encoder;
        check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
        check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
        ComPtr<IWICBitmapFrameEncode> frame;
        check(encoder->CreateNewFrame(&frame, nullptr));
        check(frame->Initialize(nullptr));
        check(frame->SetSize(desc.Width, desc.Height));
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        check(frame->SetPixelFormat(&format));
        if (format != GUID_WICPixelFormat32bppBGRA)
            throw std::runtime_error("Unexpected screenshot pixel format");
        std::vector<BYTE> pixels(size_t(desc.Width) * desc.Height * 4);
        for (UINT y = 0; y < desc.Height; ++y) {
            auto *source_row = static_cast<const BYTE *>(mapped.pData) + size_t(y) * mapped.RowPitch;
            auto *row = pixels.data() + size_t(y) * desc.Width * 4;
            for (UINT x = 0; x < desc.Width; ++x) {
                row[x * 4] = source_row[x * 4 + 2];
                row[x * 4 + 1] = source_row[x * 4 + 1];
                row[x * 4 + 2] = source_row[x * 4];
                row[x * 4 + 3] = 255;
            }
        }
        check(frame->WritePixels(desc.Height, desc.Width * 4, desc.Width * 4 * desc.Height, pixels.data()));
        check(frame->Commit());
        check(encoder->Commit());
    } catch (...) {
        context->Unmap(staging.Get(), 0);
        if (SUCCEEDED(com))
            CoUninitialize();
        throw;
    }
    context->Unmap(staging.Get(), 0);
    if (SUCCEEDED(com))
        CoUninitialize();
}
