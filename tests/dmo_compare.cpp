#include <windows.h>
#include <dshow.h>
#include <dmo.h>
#include <dmodshow.h>
#include <ocidl.h>

#include <iomanip>
#include <iostream>
#include <string>

namespace {
constexpr GUID CLSID_MMD2FFMPEG =
    {0xc42d995c, 0x3d1b, 0x4e44, {0xa9, 0x6b, 0x76, 0x7b, 0x6c, 0x2a, 0x46, 0x46}};
constexpr GUID CLSID_UTVIDEO_RGB =
    {0x47524c55, 0xe991, 0x460d, {0x84, 0x0b, 0xc1, 0xc6, 0x49, 0x74, 0x57, 0xef}};

void free_type(DMO_MEDIA_TYPE& type) {
    if (type.cbFormat && type.pbFormat) CoTaskMemFree(type.pbFormat);
    if (type.pUnk) type.pUnk->Release();
    ZeroMemory(&type, sizeof(type));
}

void delete_media_type(AM_MEDIA_TYPE* type) {
    if (!type) return;
    if (type->cbFormat && type->pbFormat) CoTaskMemFree(type->pbFormat);
    if (type->pUnk) type->pUnk->Release();
    CoTaskMemFree(type);
}

DMO_MEDIA_TYPE make_rgb32_type() {
    DMO_MEDIA_TYPE type{};
    type.majortype = MEDIATYPE_Video;
    type.subtype = MEDIASUBTYPE_RGB32;
    type.bFixedSizeSamples = TRUE;
    type.lSampleSize = 1920 * 1080 * 4;
    type.formattype = FORMAT_VideoInfo;
    type.cbFormat = sizeof(VIDEOINFOHEADER);
    type.pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(type.cbFormat));
    ZeroMemory(type.pbFormat, type.cbFormat);
    auto* video = reinterpret_cast<VIDEOINFOHEADER*>(type.pbFormat);
    video->AvgTimePerFrame = 166667;
    video->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    video->bmiHeader.biWidth = 1920;
    video->bmiHeader.biHeight = 1080;
    video->bmiHeader.biPlanes = 1;
    video->bmiHeader.biBitCount = 32;
    video->bmiHeader.biCompression = BI_RGB;
    video->bmiHeader.biSizeImage = type.lSampleSize;
    return type;
}

std::wstring guid_text(REFGUID guid) {
    wchar_t text[64]{};
    StringFromGUID2(guid, text, static_cast<int>(std::size(text)));
    return text;
}

std::wstring fourcc(DWORD value) {
    wchar_t text[5] = {
        static_cast<wchar_t>(value & 0xff),
        static_cast<wchar_t>((value >> 8) & 0xff),
        static_cast<wchar_t>((value >> 16) & 0xff),
        static_cast<wchar_t>((value >> 24) & 0xff), 0};
    return text;
}

void print_type(const DMO_MEDIA_TYPE& type, DWORD index) {
    std::wcout << L"  output[" << index << L"] subtype=" << guid_text(type.subtype)
               << L" fixed=" << type.bFixedSizeSamples
               << L" temporal=" << type.bTemporalCompression
               << L" sample=" << type.lSampleSize
               << L" format=" << guid_text(type.formattype)
               << L" cbFormat=" << type.cbFormat << L"\n";
    if (type.formattype == FORMAT_VideoInfo && type.pbFormat && type.cbFormat >= sizeof(VIDEOINFOHEADER)) {
        const auto* video = reinterpret_cast<const VIDEOINFOHEADER*>(type.pbFormat);
        const auto& bitmap = video->bmiHeader;
        std::wcout << L"    " << bitmap.biWidth << L"x" << bitmap.biHeight
                   << L" bits=" << bitmap.biBitCount
                   << L" compression=" << fourcc(bitmap.biCompression)
                   << L"(0x" << std::hex << bitmap.biCompression << std::dec << L")"
                   << L" image=" << bitmap.biSizeImage
                   << L" frame_time=" << video->AvgTimePerFrame << L"\n";
    }
}

bool probe(const wchar_t* name, REFCLSID clsid) {
    IMediaObject* object = nullptr;
    HRESULT result = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_IMediaObject,
                                      reinterpret_cast<void**>(&object));
    if (FAILED(result)) {
        std::wcerr << name << L": CoCreateInstance failed 0x" << std::hex << result << std::dec << L"\n";
        return false;
    }
    ISpecifyPropertyPages* pages = nullptr;
    const HRESULT pages_qi = object->QueryInterface(IID_ISpecifyPropertyPages,
                                                     reinterpret_cast<void**>(&pages));
    std::wcout << L"  ISpecifyPropertyPages QI=0x" << std::hex << pages_qi << std::dec;
    if (pages) {
        CAUUID page_ids{};
        const HRESULT get_pages = pages->GetPages(&page_ids);
        std::wcout << L" GetPages=0x" << std::hex << get_pages << std::dec
                   << L" count=" << page_ids.cElems;
        for (ULONG i = 0; i < page_ids.cElems; ++i) {
            std::wcout << L" page[" << i << L"]=" << guid_text(page_ids.pElems[i]);
        }
        CoTaskMemFree(page_ids.pElems);
        pages->Release();
    }
    std::wcout << L"\n";
    IAMVfwCompressDialogs* direct_dialogs = nullptr;
    const HRESULT direct_dialogs_qi = object->QueryInterface(IID_IAMVfwCompressDialogs,
                                                              reinterpret_cast<void**>(&direct_dialogs));
    std::wcout << L"  IAMVfwCompressDialogs QI=0x" << std::hex << direct_dialogs_qi << std::dec;
    if (direct_dialogs) {
        std::wcout << L" QueryConfig=0x" << std::hex
                   << direct_dialogs->ShowDialog(VfwCompressDialog_QueryConfig, nullptr) << std::dec;
        direct_dialogs->Release();
    }
    std::wcout << L"\n";
    DWORD input_flags = 0, output_flags = 0;
    object->GetInputStreamInfo(0, &input_flags);
    object->GetOutputStreamInfo(0, &output_flags);
    std::wcout << L"[" << name << L"] input_flags=0x" << std::hex << input_flags
               << L" output_flags=0x" << output_flags << std::dec << L"\n";

    auto input = make_rgb32_type();
    result = object->SetInputType(0, &input, 0);
    std::wcout << L"  SetInputType RGB32 1920x1080: 0x" << std::hex << result << std::dec << L"\n";
    if (FAILED(result)) {
        free_type(input); object->Release(); return false;
    }

    for (DWORD index = 0;; ++index) {
        DMO_MEDIA_TYPE output{};
        result = object->GetOutputType(0, index, &output);
        if (result == DMO_E_NO_MORE_ITEMS) break;
        if (FAILED(result)) {
            std::wcout << L"  GetOutputType[" << index << L"] failed 0x" << std::hex << result << std::dec << L"\n";
            break;
        }
        print_type(output, index);
        const HRESULT test_result = object->SetOutputType(0, &output, DMO_SET_TYPEF_TEST_ONLY);
        std::wcout << L"    SetOutputType(TEST_ONLY): 0x" << std::hex << test_result << std::dec << L"\n";
        free_type(output);
    }
    free_type(input);
    object->Release();
    return true;
}

void probe_wrapper(const wchar_t* name, REFCLSID clsid) {
    IBaseFilter* filter = nullptr;
    HRESULT result = CoCreateInstance(CLSID_DMOWrapperFilter, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IBaseFilter, reinterpret_cast<void**>(&filter));
    if (FAILED(result)) {
        std::wcout << L"[" << name << L" wrapper] create failed 0x" << std::hex << result << std::dec << L"\n";
        return;
    }
    IDMOWrapperFilter* wrapper = nullptr;
    result = filter->QueryInterface(IID_IDMOWrapperFilter, reinterpret_cast<void**>(&wrapper));
    if (SUCCEEDED(result)) result = wrapper->Init(clsid, DMOCATEGORY_VIDEO_ENCODER);
    std::wcout << L"[" << name << L" wrapper] Init=0x" << std::hex << result << std::dec << L"\n";
    if (wrapper) wrapper->Release();
    if (FAILED(result)) { filter->Release(); return; }

    ISpecifyPropertyPages* filter_pages = nullptr;
    const HRESULT filter_pages_qi = filter->QueryInterface(IID_ISpecifyPropertyPages,
                                                            reinterpret_cast<void**>(&filter_pages));
    std::wcout << L"  filter ISpecifyPropertyPages QI=0x" << std::hex << filter_pages_qi << std::dec;
    if (filter_pages) {
        CAUUID page_ids{};
        const HRESULT get_pages = filter_pages->GetPages(&page_ids);
        std::wcout << L" GetPages=0x" << std::hex << get_pages << std::dec
                   << L" count=" << page_ids.cElems;
        for (ULONG i = 0; i < page_ids.cElems; ++i) {
            std::wcout << L" page[" << i << L"]=" << guid_text(page_ids.pElems[i]);
        }
        CoTaskMemFree(page_ids.pElems);
        filter_pages->Release();
    }
    std::wcout << L"\n";
    IAMVfwCompressDialogs* dialogs = nullptr;
    const HRESULT dialogs_qi = filter->QueryInterface(IID_IAMVfwCompressDialogs,
                                                       reinterpret_cast<void**>(&dialogs));
    std::wcout << L"  filter IAMVfwCompressDialogs QI=0x" << std::hex << dialogs_qi << std::dec;
    if (dialogs) {
        std::wcout << L" QueryConfig=0x" << std::hex
                   << dialogs->ShowDialog(VfwCompressDialog_QueryConfig, nullptr) << std::dec;
        dialogs->Release();
    }
    std::wcout << L"\n";

    IEnumPins* pins = nullptr;
    if (SUCCEEDED(filter->EnumPins(&pins))) {
        IPin* pin = nullptr;
        ULONG fetched = 0;
        while (pins->Next(1, &pin, &fetched) == S_OK) {
            PIN_DIRECTION direction{};
            pin->QueryDirection(&direction);
            PIN_INFO info{};
            pin->QueryPinInfo(&info);
            std::wcout << L"  pin=" << info.achName << L" direction="
                       << (direction == PINDIR_INPUT ? L"input" : L"output") << L"\n";
            if (info.pFilter) info.pFilter->Release();

            IAMStreamConfig* stream_config = nullptr;
            const HRESULT stream_qi = pin->QueryInterface(IID_IAMStreamConfig,
                                                          reinterpret_cast<void**>(&stream_config));
            std::wcout << L"    IAMStreamConfig QI=0x" << std::hex << stream_qi << std::dec;
            if (stream_config) {
                int count = 0, size = 0;
                const HRESULT caps = stream_config->GetNumberOfCapabilities(&count, &size);
                std::wcout << L" caps=0x" << std::hex << caps << std::dec
                           << L" count=" << count << L" size=" << size;
                stream_config->Release();
            }
            std::wcout << L"\n";

            IAMVideoCompression* compression = nullptr;
            const HRESULT compression_qi = pin->QueryInterface(IID_IAMVideoCompression,
                                                               reinterpret_cast<void**>(&compression));
            std::wcout << L"    IAMVideoCompression QI=0x" << std::hex << compression_qi << std::dec << L"\n";
            if (compression) compression->Release();

            IEnumMediaTypes* types = nullptr;
            if (SUCCEEDED(pin->EnumMediaTypes(&types))) {
                AM_MEDIA_TYPE* media = nullptr;
                while (types->Next(1, &media, &fetched) == S_OK) {
                    std::wcout << L"    media subtype=" << guid_text(media->subtype)
                               << L" format=" << guid_text(media->formattype)
                               << L" cbFormat=" << media->cbFormat << L"\n";
                    delete_media_type(media);
                }
                types->Release();
            }
            pin->Release();
        }
        pins->Release();
    }
    filter->Release();
}
}

int wmain() {
    std::wcout << std::unitbuf;
    std::wcout << L"[phase] COM initialization\n";
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::wcout << L"[phase] direct DMO probes\n";
    const bool utvideo = probe(L"UtVideo RGB DMO", CLSID_UTVIDEO_RGB);
    const bool bridge = probe(L"MMD2FFMPEG DMO", CLSID_MMD2FFMPEG);
    std::wcout << L"[phase] DMO Wrapper probes\n";
    probe_wrapper(L"UtVideo RGB DMO", CLSID_UTVIDEO_RGB);
    probe_wrapper(L"MMD2FFMPEG DMO", CLSID_MMD2FFMPEG);
    std::wcout << L"[phase] COM shutdown\n";
    CoUninitialize();
    return utvideo && bridge ? 0 : 1;
}
