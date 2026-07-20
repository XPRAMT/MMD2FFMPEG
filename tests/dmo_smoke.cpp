#include <windows.h>
#include <dmo.h>
#include <dmoreg.h>

#include <iostream>

namespace {
constexpr GUID CLSID_MMD2FFMPEG =
    {0xc42d995c, 0x3d1b, 0x4e44, {0xa9, 0x6b, 0x76, 0x7b, 0x6c, 0x2a, 0x46, 0x46}};
}

int wmain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IEnumDMO* enumerator = nullptr;
    HRESULT result = DMOEnum(DMOCATEGORY_VIDEO_ENCODER, 0, 0, nullptr, 0, nullptr, &enumerator);
    bool found = false;
    if (SUCCEEDED(result)) {
        CLSID clsid{};
        WCHAR* name = nullptr;
        ULONG fetched = 0;
        while (enumerator->Next(1, &clsid, &name, &fetched) == S_OK) {
            if (clsid == CLSID_MMD2FFMPEG) {
                found = true;
                std::wcout << L"DMOEnum found: " << (name ? name : L"") << L"\n";
            }
            CoTaskMemFree(name);
            name = nullptr;
        }
        enumerator->Release();
    }
    if (!found) {
        std::wcerr << L"DMOEnum did not return MMD2FFMPEG in the video encoder category.\n";
        CoUninitialize(); return 3;
    }
    IMediaObject* object = nullptr;
    result = CoCreateInstance(CLSID_MMD2FFMPEG, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IMediaObject, reinterpret_cast<void**>(&object));
    if (FAILED(result)) {
        std::wcerr << L"CoCreateInstance failed: 0x" << std::hex << result << L"\n";
        CoUninitialize(); return 1;
    }
    DWORD inputs = 0, outputs = 0;
    const HRESULT count_result = object->GetStreamCount(&inputs, &outputs);
    object->Release(); CoUninitialize();
    if (FAILED(count_result) || inputs != 1 || outputs != 1) {
        std::wcerr << L"Unexpected stream count.\n"; return 2;
    }
    std::wcout << L"Created MMD2FFMPEG DMO with one input and one output stream.\n";
    return 0;
}
