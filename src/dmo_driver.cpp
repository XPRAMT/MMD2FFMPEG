#include <windows.h>
#include <dshow.h>
#include <dmo.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// {C42D995C-3D1B-4E44-A96B-767B6C2A4646}
constexpr GUID CLSID_MMD2FFMPEG =
    {0xc42d995c, 0x3d1b, 0x4e44, {0xa9, 0x6b, 0x76, 0x7b, 0x6c, 0x2a, 0x46, 0x46}};
constexpr GUID MEDIASUBTYPE_M2FF =
    {0x4646324d, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
std::atomic<long> g_objects{0};
std::atomic<long> g_locks{0};

struct Settings {
    std::wstring ffmpeg = L"C:\\Program Files\\Hybrid\\64bit\\ffmpeg.exe";
    std::wstring output = L"C:\\APP\\MMD\\MMD2FFMPEG\\out\\mmd-output.mkv";
    std::wstring video_args = L"-c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p -movflags +faststart";
    int fps = 30;
};

std::filesystem::path config_path() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    return count > 0 && count < buffer.size()
        ? std::filesystem::path(buffer.data()) / L"MMD2FFMPEG" / L"config.ini"
        : std::filesystem::path(L"config.ini");
}

std::wstring trim(std::wstring value) {
    const auto space = [](wchar_t ch) { return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n'; };
    while (!value.empty() && space(value.front())) value.erase(value.begin());
    while (!value.empty() && space(value.back())) value.pop_back();
    return value;
}

Settings load_settings() {
    Settings settings;
    std::wifstream file(config_path());
    std::wstring line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line.front() == L'#' || line.front() == L';') continue;
        const auto split = line.find(L'=');
        if (split == std::wstring::npos) continue;
        const auto key = trim(line.substr(0, split));
        const auto value = trim(line.substr(split + 1));
        if (key == L"ffmpeg") settings.ffmpeg = value;
        else if (key == L"output") settings.output = value;
        else if (key == L"video_args") settings.video_args = value;
        else if (key == L"fps") {
            try { settings.fps = std::clamp(std::stoi(value), 1, 240); } catch (...) {}
        }
    }
    return settings;
}

std::wstring quote(const std::wstring& value) { return L"\"" + value + L"\""; }

void close_handle(HANDLE& handle) {
    if (handle) { CloseHandle(handle); handle = nullptr; }
}

void free_type(DMO_MEDIA_TYPE& type) {
    if (type.cbFormat && type.pbFormat) CoTaskMemFree(type.pbFormat);
    if (type.pUnk) type.pUnk->Release();
    ZeroMemory(&type, sizeof(type));
}

HRESULT copy_type(DMO_MEDIA_TYPE& target, const DMO_MEDIA_TYPE& source) {
    free_type(target);
    target = source;
    target.pbFormat = nullptr;
    target.pUnk = source.pUnk;
    if (target.pUnk) target.pUnk->AddRef();
    if (source.cbFormat) {
        target.pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(source.cbFormat));
        if (!target.pbFormat) { free_type(target); return E_OUTOFMEMORY; }
        CopyMemory(target.pbFormat, source.pbFormat, source.cbFormat);
    }
    return S_OK;
}

HRESULT make_video_type(DMO_MEDIA_TYPE* type, const GUID& subtype, LONG width, LONG height,
                        WORD bits, DWORD compression, DWORD image_size, REFERENCE_TIME frame_time) {
    if (!type) return E_POINTER;
    ZeroMemory(type, sizeof(*type));
    type->majortype = MEDIATYPE_Video;
    type->subtype = subtype;
    type->bFixedSizeSamples = compression == BI_RGB;
    type->bTemporalCompression = compression != BI_RGB;
    type->lSampleSize = image_size;
    type->formattype = FORMAT_VideoInfo;
    type->cbFormat = sizeof(VIDEOINFOHEADER);
    type->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(type->cbFormat));
    if (!type->pbFormat) return E_OUTOFMEMORY;
    ZeroMemory(type->pbFormat, type->cbFormat);
    auto* video = reinterpret_cast<VIDEOINFOHEADER*>(type->pbFormat);
    video->AvgTimePerFrame = frame_time;
    video->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    video->bmiHeader.biWidth = width;
    video->bmiHeader.biHeight = height;
    video->bmiHeader.biPlanes = 1;
    video->bmiHeader.biBitCount = bits;
    video->bmiHeader.biCompression = compression;
    video->bmiHeader.biSizeImage = image_size;
    return S_OK;
}

bool supported_input(const DMO_MEDIA_TYPE& type) {
    if (type.majortype != MEDIATYPE_Video || type.formattype != FORMAT_VideoInfo ||
        !type.pbFormat || type.cbFormat < sizeof(VIDEOINFOHEADER)) return false;
    if (type.subtype != MEDIASUBTYPE_RGB24 && type.subtype != MEDIASUBTYPE_RGB32) return false;
    const auto& bitmap = reinterpret_cast<const VIDEOINFOHEADER*>(type.pbFormat)->bmiHeader;
    return bitmap.biWidth > 0 && bitmap.biHeight != 0 && bitmap.biPlanes == 1 &&
           (bitmap.biBitCount == 24 || bitmap.biBitCount == 32) && bitmap.biCompression == BI_RGB;
}

DWORD compressed_buffer_size(LONG width, LONG height) {
    const auto raw_size = static_cast<unsigned long long>(width) * std::abs(height) * 3;
    return static_cast<DWORD>(raw_size + 4096);
}

HRESULT make_output_type(DMO_MEDIA_TYPE* type, LONG width, LONG height, REFERENCE_TIME frame_time) {
    const DWORD image_size = compressed_buffer_size(width, height);
    HRESULT result = make_video_type(type, MEDIASUBTYPE_M2FF, width, height, 24,
                                     0x4646324d, image_size, frame_time);
    if (FAILED(result)) return result;
    auto* expanded = static_cast<BYTE*>(CoTaskMemRealloc(type->pbFormat, sizeof(VIDEOINFOHEADER) + 16));
    if (!expanded) { free_type(*type); return E_OUTOFMEMORY; }
    type->pbFormat = expanded;
    ZeroMemory(type->pbFormat + sizeof(VIDEOINFOHEADER), 16);
    type->cbFormat = sizeof(VIDEOINFOHEADER) + 16;
    type->bFixedSizeSamples = FALSE;
    type->bTemporalCompression = FALSE;
    type->lSampleSize = 0;
    return S_OK;
}

bool supported_output(const DMO_MEDIA_TYPE& type) {
    if (type.majortype != MEDIATYPE_Video || type.subtype != MEDIASUBTYPE_M2FF ||
        type.formattype != FORMAT_VideoInfo || !type.pbFormat ||
        type.cbFormat < sizeof(VIDEOINFOHEADER)) return false;
    const auto& bitmap = reinterpret_cast<const VIDEOINFOHEADER*>(type.pbFormat)->bmiHeader;
    return bitmap.biWidth > 0 && bitmap.biHeight != 0 && bitmap.biPlanes == 1 &&
           bitmap.biBitCount == 24 && bitmap.biCompression == 0x4646324d &&
           bitmap.biSizeImage >= compressed_buffer_size(bitmap.biWidth, bitmap.biHeight);
}

class Encoder final : public IMediaObject {
public:
    class InnerUnknown final : public IUnknown {
    public:
        explicit InnerUnknown(Encoder* owner) : owner_(owner) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
            return owner_->nondelegating_query_interface(iid, object);
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return owner_->nondelegating_add_ref(); }
        ULONG STDMETHODCALLTYPE Release() override { return owner_->nondelegating_release(); }
    private:
        Encoder* owner_;
    };

    explicit Encoder(IUnknown* outer) : inner_unknown_(this), outer_(outer ? outer : &inner_unknown_) { ++g_objects; }
    ~Encoder() { stop_ffmpeg(); free_type(input_type_); free_type(output_type_); --g_objects; }

    IUnknown* inner_unknown() { return &inner_unknown_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        return outer_->QueryInterface(iid, object);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return outer_->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return outer_->Release(); }

    HRESULT nondelegating_query_interface(REFIID iid, void** object) {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown) {
            *object = static_cast<IUnknown*>(&inner_unknown_);
            nondelegating_add_ref();
            return S_OK;
        }
        if (iid == IID_IMediaObject) {
            *object = static_cast<IMediaObject*>(this);
            outer_->AddRef();
            return S_OK;
        }
        else return E_NOINTERFACE;
    }
    ULONG nondelegating_add_ref() { return ++references_; }
    ULONG nondelegating_release() {
        const ULONG value = --references_;
        if (!value) delete this;
        return value;
    }

    HRESULT STDMETHODCALLTYPE GetStreamCount(DWORD* inputs, DWORD* outputs) override {
        if (!inputs || !outputs) return E_POINTER;
        *inputs = 1; *outputs = 1; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetInputStreamInfo(DWORD index, DWORD* flags) override {
        if (index != 0) return DMO_E_INVALIDSTREAMINDEX;
        if (!flags) return E_POINTER;
        *flags = DMO_INPUT_STREAMF_WHOLE_SAMPLES | DMO_INPUT_STREAMF_SINGLE_SAMPLE_PER_BUFFER;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetOutputStreamInfo(DWORD index, DWORD* flags) override {
        if (index != 0) return DMO_E_INVALIDSTREAMINDEX;
        if (!flags) return E_POINTER;
        *flags = DMO_OUTPUT_STREAMF_WHOLE_SAMPLES | DMO_OUTPUT_STREAMF_SINGLE_SAMPLE_PER_BUFFER;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetInputType(DWORD index, DWORD type_index, DMO_MEDIA_TYPE* type) override {
        if (index != 0) return DMO_E_INVALIDSTREAMINDEX;
        if (!type) return E_POINTER;
        if (type_index > 1) return DMO_E_NO_MORE_ITEMS;
        const GUID& subtype = type_index == 0 ? MEDIASUBTYPE_RGB32 : MEDIASUBTYPE_RGB24;
        const WORD bits = type_index == 0 ? 32 : 24;
        return make_video_type(type, subtype, 640, 480, bits, BI_RGB, 640 * 480 * bits / 8, 333333);
    }
    HRESULT STDMETHODCALLTYPE GetOutputType(DWORD index, DWORD type_index, DMO_MEDIA_TYPE* type) override {
        if (index != 0) return DMO_E_INVALIDSTREAMINDEX;
        if (!type) return E_POINTER;
        if (type_index != 0) return DMO_E_NO_MORE_ITEMS;
        if (!input_type_.pbFormat) {
            ZeroMemory(type, sizeof(*type));
            type->majortype = MEDIATYPE_Video;
            type->subtype = MEDIASUBTYPE_M2FF;
            type->formattype = GUID_NULL;
            return S_OK;
        }
        const auto* input = reinterpret_cast<const VIDEOINFOHEADER*>(input_type_.pbFormat);
        return make_output_type(type, input->bmiHeader.biWidth, input->bmiHeader.biHeight,
                                input->AvgTimePerFrame);
    }
    HRESULT STDMETHODCALLTYPE SetInputType(DWORD index, const DMO_MEDIA_TYPE* type, DWORD flags) override {
        if (index != 0) return DMO_E_INVALIDSTREAMINDEX;
        if (flags & DMO_SET_TYPEF_CLEAR) { free_type(input_type_); return S_OK; }
        if (!type) return E_POINTER;
        if (!supported_input(*type)) return DMO_E_TYPE_NOT_ACCEPTED;
        if (flags & DMO_SET_TYPEF_TEST_ONLY) return S_OK;
        return copy_type(input_type_, *type);
    }
    HRESULT STDMETHODCALLTYPE SetOutputType(DWORD index, const DMO_MEDIA_TYPE* type, DWORD flags) override {
        if (index != 0) return DMO_E_INVALIDSTREAMINDEX;
        if (flags & DMO_SET_TYPEF_CLEAR) { free_type(output_type_); return S_OK; }
        if (!type) return E_POINTER;
        if (!input_type_.pbFormat || !supported_output(*type)) return DMO_E_TYPE_NOT_ACCEPTED;
        const auto* input = reinterpret_cast<const VIDEOINFOHEADER*>(input_type_.pbFormat);
        const auto* output = reinterpret_cast<const VIDEOINFOHEADER*>(type->pbFormat);
        if (output->bmiHeader.biWidth != input->bmiHeader.biWidth ||
            output->bmiHeader.biHeight != input->bmiHeader.biHeight ||
            output->AvgTimePerFrame != input->AvgTimePerFrame) {
            return DMO_E_TYPE_NOT_ACCEPTED;
        }
        if (flags & DMO_SET_TYPEF_TEST_ONLY) return S_OK;
        return copy_type(output_type_, *type);
    }
    HRESULT STDMETHODCALLTYPE GetInputCurrentType(DWORD index, DMO_MEDIA_TYPE* type) override {
        if (index != 0) return DMO_E_INVALIDSTREAMINDEX;
        if (!type) return E_POINTER;
        if (!input_type_.pbFormat) return DMO_E_TYPE_NOT_SET;
        ZeroMemory(type, sizeof(*type)); return copy_type(*type, input_type_);
    }
    HRESULT STDMETHODCALLTYPE GetOutputCurrentType(DWORD index, DMO_MEDIA_TYPE* type) override {
        if (index != 0) return DMO_E_INVALIDSTREAMINDEX;
        if (!type) return E_POINTER;
        if (!output_type_.pbFormat) return DMO_E_TYPE_NOT_SET;
        ZeroMemory(type, sizeof(*type)); return copy_type(*type, output_type_);
    }
    HRESULT STDMETHODCALLTYPE GetInputSizeInfo(DWORD index, DWORD* size, DWORD* lookahead, DWORD* alignment) override {
        if (index != 0) return DMO_E_INVALIDSTREAMINDEX;
        if (!size || !lookahead || !alignment) return E_POINTER;
        if (!input_type_.pbFormat) return DMO_E_TYPE_NOT_SET;
        *size = reinterpret_cast<VIDEOINFOHEADER*>(input_type_.pbFormat)->bmiHeader.biSizeImage;
        *lookahead = 0; *alignment = 1; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetOutputSizeInfo(DWORD index, DWORD* size, DWORD* alignment) override {
        if (index != 0) return DMO_E_INVALIDSTREAMINDEX;
        if (!size || !alignment) return E_POINTER;
        if (!output_type_.pbFormat) return DMO_E_TYPE_NOT_SET;
        *size = reinterpret_cast<VIDEOINFOHEADER*>(output_type_.pbFormat)->bmiHeader.biSizeImage;
        *alignment = 1; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetInputMaxLatency(DWORD index, REFERENCE_TIME* latency) override {
        if (index != 0) return DMO_E_INVALIDSTREAMINDEX;
        if (!latency) return E_POINTER;
        *latency = 0; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetInputMaxLatency(DWORD index, REFERENCE_TIME) override {
        return index == 0 ? S_OK : DMO_E_INVALIDSTREAMINDEX;
    }
    HRESULT STDMETHODCALLTYPE Flush() override { pending_ = false; stop_ffmpeg(); return S_OK; }
    HRESULT STDMETHODCALLTYPE Discontinuity(DWORD index) override {
        return index == 0 ? S_OK : DMO_E_INVALIDSTREAMINDEX;
    }
    HRESULT STDMETHODCALLTYPE AllocateStreamingResources() override {
        if (!input_type_.pbFormat || !output_type_.pbFormat) return DMO_E_TYPE_NOT_SET;
        return start_ffmpeg() ? S_OK : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE FreeStreamingResources() override {
        stop_ffmpeg(); pending_ = false; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetInputStatus(DWORD index, DWORD* flags) override {
        if (index != 0) return DMO_E_INVALIDSTREAMINDEX;
        if (!flags) return E_POINTER;
        *flags = pending_ ? 0 : DMO_INPUT_STATUSF_ACCEPT_DATA;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ProcessInput(DWORD index, IMediaBuffer* buffer, DWORD,
                                           REFERENCE_TIME timestamp, REFERENCE_TIME duration) override {
        if (index != 0) return DMO_E_INVALIDSTREAMINDEX;
        if (!buffer) return E_POINTER;
        if (pending_) return DMO_E_NOTACCEPTING;
        if (!input_type_.pbFormat || !output_type_.pbFormat) return DMO_E_TYPE_NOT_SET;
        if (!started_ && !start_ffmpeg()) return E_FAIL;
        BYTE* bytes = nullptr; DWORD length = 0;
        HRESULT result = buffer->GetBufferAndLength(&bytes, &length);
        if (FAILED(result)) return result;
        if (!send_frame(bytes, length)) return E_FAIL;
        pending_ = true; timestamp_ = timestamp; duration_ = duration;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ProcessOutput(DWORD, DWORD count, DMO_OUTPUT_DATA_BUFFER* outputs, DWORD* status) override {
        if (!outputs || !status) return E_POINTER;
        if (count != 1) return E_INVALIDARG;
        *status = 0;
        if (!pending_) return S_FALSE;
        if (!outputs[0].pBuffer) return E_POINTER;
        BYTE* bytes = nullptr; DWORD length = 0;
        HRESULT result = outputs[0].pBuffer->GetBufferAndLength(&bytes, &length);
        if (FAILED(result)) return result;
        DWORD maximum = 0; result = outputs[0].pBuffer->GetMaxLength(&maximum);
        if (FAILED(result) || maximum < 1 || !bytes) return E_FAIL;
        bytes[0] = 0;
        result = outputs[0].pBuffer->SetLength(1);
        if (FAILED(result)) return result;
        outputs[0].dwStatus = DMO_OUTPUT_DATA_BUFFERF_SYNCPOINT | DMO_OUTPUT_DATA_BUFFERF_TIME |
                              DMO_OUTPUT_DATA_BUFFERF_TIMELENGTH;
        outputs[0].rtTimestamp = timestamp_;
        outputs[0].rtTimelength = duration_;
        pending_ = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Lock(LONG) override { return S_OK; }

private:
    bool start_ffmpeg() {
        if (started_) return true;
        const auto* video = reinterpret_cast<const VIDEOINFOHEADER*>(input_type_.pbFormat);
        const auto& bitmap = video->bmiHeader;
        width_ = bitmap.biWidth; height_ = std::abs(bitmap.biHeight);
        bits_ = bitmap.biBitCount; bottom_up_ = bitmap.biHeight > 0;
        stride_ = ((width_ * bits_ + 31) / 32) * 4;
        settings_ = load_settings();
        if (video->AvgTimePerFrame > 0) settings_.fps = static_cast<int>((10000000LL + video->AvgTimePerFrame / 2) / video->AvgTimePerFrame);
        if (!std::filesystem::exists(settings_.ffmpeg)) return false;
        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path(settings_.output).parent_path(), error);
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        HANDLE read_pipe = nullptr;
        if (!CreatePipe(&read_pipe, &stdin_write_, &security, 1024 * 1024)) return false;
        SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0);
        std::wostringstream stream;
        stream << quote(settings_.ffmpeg) << L" -hide_banner -loglevel warning -y -f rawvideo -pixel_format "
               << (bits_ == 24 ? L"bgr24" : L"bgra") << L" -video_size " << width_ << L"x" << height_
               << L" -framerate " << settings_.fps << L" -i pipe:0 " << settings_.video_args << L" "
               << quote(settings_.output);
        auto command = stream.str();
        std::vector<wchar_t> mutable_command(command.begin(), command.end()); mutable_command.push_back(L'\0');
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = read_pipe; startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(settings_.ffmpeg.c_str(), mutable_command.data(), nullptr, nullptr,
                                             TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
        CloseHandle(read_pipe);
        if (!created) { close_handle(stdin_write_); return false; }
        process_ = process.hProcess; process_thread_ = process.hThread; started_ = true; return true;
    }
    void stop_ffmpeg() {
        close_handle(stdin_write_);
        if (process_) WaitForSingleObject(process_, 30000);
        close_handle(process_thread_); close_handle(process_); started_ = false;
    }
    bool write_all(const BYTE* bytes, DWORD length) {
        while (length) {
            DWORD written = 0;
            if (!WriteFile(stdin_write_, bytes, length, &written, nullptr) || !written) return false;
            bytes += written; length -= written;
        }
        return true;
    }
    bool send_frame(const BYTE* source, DWORD length) {
        if (!source || length < static_cast<DWORD>(stride_ * height_)) return false;
        const DWORD row_bytes = static_cast<DWORD>(width_ * bits_ / 8);
        if (!bottom_up_ && stride_ == static_cast<LONG>(row_bytes)) return write_all(source, row_bytes * height_);
        flipped_.resize(static_cast<std::size_t>(row_bytes) * height_);
        for (int y = 0; y < height_; ++y) {
            const int source_y = bottom_up_ ? height_ - 1 - y : y;
            std::copy_n(source + static_cast<std::size_t>(source_y) * stride_, row_bytes,
                        flipped_.data() + static_cast<std::size_t>(y) * row_bytes);
        }
        return write_all(flipped_.data(), static_cast<DWORD>(flipped_.size()));
    }

    InnerUnknown inner_unknown_;
    IUnknown* outer_;
    std::atomic<ULONG> references_{1};
    DMO_MEDIA_TYPE input_type_{}; DMO_MEDIA_TYPE output_type_{};
    Settings settings_{};
    HANDLE process_ = nullptr, process_thread_ = nullptr, stdin_write_ = nullptr;
    int width_ = 0, height_ = 0, bits_ = 0; LONG stride_ = 0;
    bool bottom_up_ = false, started_ = false, pending_ = false;
    REFERENCE_TIME timestamp_ = 0, duration_ = 0;
    std::vector<BYTE> flipped_;
};

class Factory final : public IClassFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IClassFactory) *object = static_cast<IClassFactory*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG value = --references_; if (!value) delete this; return value; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** object) override {
        if (outer && iid != IID_IUnknown) return CLASS_E_NOAGGREGATION;
        if (!object) return E_POINTER;
        *object = nullptr;
        auto* encoder = new (std::nothrow) Encoder(outer);
        if (!encoder) return E_OUTOFMEMORY;
        if (outer) {
            *object = encoder->inner_unknown();
            return S_OK;
        }
        const HRESULT result = encoder->nondelegating_query_interface(iid, object);
        encoder->nondelegating_release();
        return result;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override { lock ? ++g_locks : --g_locks; return S_OK; }
private:
    std::atomic<ULONG> references_{1};
};

} // namespace

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID iid, void** object) {
    if (clsid != CLSID_MMD2FFMPEG) return CLASS_E_CLASSNOTAVAILABLE;
    auto* factory = new (std::nothrow) Factory();
    if (!factory) return E_OUTOFMEMORY;
    const HRESULT result = factory->QueryInterface(iid, object);
    factory->Release(); return result;
}

extern "C" HRESULT __stdcall DllCanUnloadNow() {
    return g_objects == 0 && g_locks == 0 ? S_OK : S_FALSE;
}
