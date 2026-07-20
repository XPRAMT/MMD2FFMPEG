#include <windows.h>
#include <dshow.h>
#include <dmo.h>
#include <dmoreg.h>

#include <iostream>

namespace {
constexpr GUID CLSID_MMD2FFMPEG =
    {0xc42d995c, 0x3d1b, 0x4e44, {0xa9, 0x6b, 0x76, 0x7b, 0x6c, 0x2a, 0x46, 0x46}};
constexpr GUID MEDIASUBTYPE_M2FF =
    {0x4646324d, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

void free_type(DMO_MEDIA_TYPE& type) {
    if (type.cbFormat && type.pbFormat) CoTaskMemFree(type.pbFormat);
    if (type.pUnk) type.pUnk->Release();
    ZeroMemory(&type, sizeof(type));
}
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
    if (FAILED(count_result) || inputs != 1 || outputs != 1) {
        std::wcerr << L"Unexpected stream count.\n";
        object->Release(); CoUninitialize(); return 2;
    }

    DMO_MEDIA_TYPE input{};
    result = object->GetInputType(0, 0, &input);
    if (FAILED(result) || input.subtype != MEDIASUBTYPE_RGB32) {
        std::wcerr << L"Could not enumerate the RGB32 input type.\n";
        free_type(input); object->Release(); CoUninitialize(); return 4;
    }
    result = object->SetInputType(0, &input, 0);
    if (FAILED(result)) {
        std::wcerr << L"Could not set the RGB32 input type: 0x" << std::hex << result << L"\n";
        free_type(input); object->Release(); CoUninitialize(); return 5;
    }

    DMO_MEDIA_TYPE output{};
    result = object->GetOutputType(0, 0, &output);
    if (FAILED(result) || output.subtype != MEDIASUBTYPE_M2FF || output.bFixedSizeSamples ||
        output.lSampleSize != 0 || output.cbFormat != sizeof(VIDEOINFOHEADER) + 16) {
        std::wcerr << L"Output is not a complete variable-size compressed media type.\n";
        free_type(output); free_type(input); object->Release(); CoUninitialize(); return 6;
    }
    result = object->SetOutputType(0, &output, DMO_SET_TYPEF_TEST_ONLY);
    if (FAILED(result)) {
        std::wcerr << L"DirectShow-compatible output type was rejected: 0x" << std::hex << result << L"\n";
        free_type(output); free_type(input); object->Release(); CoUninitialize(); return 7;
    }

    DWORD output_size = 0, alignment = 0;
    result = object->SetOutputType(0, &output, 0);
    if (SUCCEEDED(result)) result = object->GetOutputSizeInfo(0, &output_size, &alignment);
    const auto* video = reinterpret_cast<const VIDEOINFOHEADER*>(output.pbFormat);
    const bool output_size_valid = SUCCEEDED(result) && output_size == video->bmiHeader.biSizeImage && alignment == 1;
    free_type(output); free_type(input); object->Release(); CoUninitialize();
    if (!output_size_valid) {
        std::wcerr << L"Output buffer contract does not match the media type.\n"; return 8;
    }
    std::wcout << L"Created DMO and negotiated matching RGB input/output media types.\n";
    return 0;
}
