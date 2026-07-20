#include <windows.h>
#include <vfw.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr DWORD kHandler = mmioFOURCC('m', '2', 'f', 'f');
constexpr wchar_t kCodecName[] = L"MMD2FFMPEG frame bridge";
constexpr wchar_t kCodecDescription[] = L"Streams MMD video frames directly to FFmpeg";

struct Settings {
    std::wstring ffmpeg = L"C:\\Program Files\\Hybrid\\64bit\\ffmpeg.exe";
    std::wstring output = L"C:\\APP\\MMD\\MMD2FFMPEG\\out\\mmd-output.mkv";
    std::wstring video_args = L"-vf format=p010le -c:v hevc_nvenc -profile:v main10 -preset p7 -tune hq -rc constqp -qp 20 -pix_fmt p010le";
    int fps = 30;
};

struct CodecState {
    Settings settings;
    HANDLE process = nullptr;
    HANDLE process_thread = nullptr;
    HANDLE stdin_write = nullptr;
    int width = 0;
    int height = 0;
    int bit_count = 0;
    LONG stride = 0;
    bool bottom_up = false;
    bool started = false;
    int stream_fps = 0;
    std::vector<std::uint8_t> flipped;
};

std::filesystem::path config_path() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (count == 0 || count >= buffer.size()) {
        return L"config.ini";
    }
    return std::filesystem::path(buffer.data()) / L"MMD2FFMPEG" / L"config.ini";
}

std::wstring trim(std::wstring value) {
    const auto is_space = [](wchar_t ch) { return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n'; };
    while (!value.empty() && is_space(value.front())) value.erase(value.begin());
    while (!value.empty() && is_space(value.back())) value.pop_back();
    return value;
}

Settings load_settings() {
    Settings settings;
    std::wifstream file(config_path());
    std::wstring line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line.front() == L'#' || line.front() == L';') continue;
        const auto equals = line.find(L'=');
        if (equals == std::wstring::npos) continue;
        const auto key = trim(line.substr(0, equals));
        const auto value = trim(line.substr(equals + 1));
        if (key == L"ffmpeg") settings.ffmpeg = value;
        else if (key == L"output") settings.output = value;
        else if (key == L"video_args") settings.video_args = value;
        else if (key == L"fps") {
            try { settings.fps = std::clamp(std::stoi(value), 1, 240); } catch (...) {}
        }
    }
    return settings;
}

std::wstring quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

void close_handle(HANDLE& handle) {
    if (handle) {
        CloseHandle(handle);
        handle = nullptr;
    }
}

void stop_ffmpeg(CodecState& state) {
    close_handle(state.stdin_write);
    if (state.process) {
        WaitForSingleObject(state.process, 30000);
    }
    close_handle(state.process_thread);
    close_handle(state.process);
    state.started = false;
}

bool start_ffmpeg(CodecState& state, const BITMAPINFOHEADER& input) {
    stop_ffmpeg(state);
    state.settings = load_settings();
    if (state.stream_fps > 0) state.settings.fps = state.stream_fps;
    state.width = input.biWidth;
    state.height = std::abs(input.biHeight);
    state.bit_count = input.biBitCount;
    state.bottom_up = input.biHeight > 0;
    state.stride = ((state.width * state.bit_count + 31) / 32) * 4;

    if (state.width <= 0 || state.height <= 0 || (state.bit_count != 24 && state.bit_count != 32)) return false;
    if (!std::filesystem::exists(state.settings.ffmpeg)) return false;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(state.settings.output).parent_path(), ec);

    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE stdin_read = nullptr;
    if (!CreatePipe(&stdin_read, &state.stdin_write, &security, 1024 * 1024)) return false;
    SetHandleInformation(state.stdin_write, HANDLE_FLAG_INHERIT, 0);

    const wchar_t* pixel_format = state.bit_count == 24 ? L"bgr24" : L"bgra";
    std::wostringstream command;
    command << quote(state.settings.ffmpeg)
            << L" -hide_banner -loglevel warning -y -f rawvideo -pixel_format " << pixel_format
            << L" -video_size " << state.width << L"x" << state.height
            << L" -framerate " << state.settings.fps << L" -i pipe:0 "
            << state.settings.video_args << L" " << quote(state.settings.output);
    auto command_line = command.str();
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_read;
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        state.settings.ffmpeg.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(stdin_read);
    if (!created) {
        close_handle(state.stdin_write);
        return false;
    }
    state.process = process.hProcess;
    state.process_thread = process.hThread;
    state.started = true;
    return true;
}

bool write_all(HANDLE output, const std::uint8_t* data, DWORD size) {
    while (size > 0) {
        DWORD written = 0;
        if (!WriteFile(output, data, size, &written, nullptr) || written == 0) return false;
        data += written;
        size -= written;
    }
    return true;
}

bool send_frame(CodecState& state, const std::uint8_t* source) {
    if (!state.started || !source) return false;
    const DWORD row_bytes = static_cast<DWORD>(state.width * (state.bit_count / 8));
    if (!state.bottom_up && state.stride == static_cast<LONG>(row_bytes)) {
        return write_all(state.stdin_write, source, row_bytes * state.height);
    }
    state.flipped.resize(static_cast<std::size_t>(row_bytes) * state.height);
    for (int y = 0; y < state.height; ++y) {
        const int source_y = state.bottom_up ? state.height - 1 - y : y;
        std::copy_n(source + static_cast<std::size_t>(source_y) * state.stride,
                    row_bytes,
                    state.flipped.data() + static_cast<std::size_t>(y) * row_bytes);
    }
    return write_all(state.stdin_write, state.flipped.data(), static_cast<DWORD>(state.flipped.size()));
}

LRESULT get_format(const BITMAPINFOHEADER* input, BITMAPINFOHEADER* output) {
    if (!input) return sizeof(BITMAPINFOHEADER);
    if (!output) return sizeof(BITMAPINFOHEADER);
    *output = *input;
    output->biSize = sizeof(BITMAPINFOHEADER);
    output->biCompression = kHandler;
    output->biSizeImage = 1;
    output->biClrUsed = 0;
    output->biClrImportant = 0;
    return ICERR_OK;
}

bool input_supported(const BITMAPINFOHEADER* input) {
    return input && input->biWidth > 0 && input->biHeight != 0 &&
           input->biPlanes == 1 && (input->biBitCount == 24 || input->biBitCount == 32) &&
           (input->biCompression == BI_RGB || input->biCompression == BI_BITFIELDS);
}

} // namespace

extern "C" LRESULT CALLBACK DriverProc(DWORD_PTR driver_id, HDRVR driver, UINT message, LPARAM first, LPARAM second) {
    auto* state = reinterpret_cast<CodecState*>(driver_id);
    switch (message) {
    case DRV_LOAD:
    case DRV_ENABLE:
    case DRV_DISABLE:
    case DRV_FREE:
        return 1;
    case DRV_OPEN: {
        auto instance = std::make_unique<CodecState>();
        instance->settings = load_settings();
        return reinterpret_cast<LRESULT>(instance.release());
    }
    case DRV_CLOSE:
        if (state) {
            stop_ffmpeg(*state);
            delete state;
        }
        return 1;
    case ICM_GETINFO: {
        auto* info = reinterpret_cast<ICINFO*>(first);
        if (!info || second < sizeof(ICINFO)) return 0;
        info->dwSize = sizeof(ICINFO);
        info->fccType = ICTYPE_VIDEO;
        info->fccHandler = kHandler;
        info->dwFlags = VIDCF_FASTTEMPORALC | VIDCF_FASTTEMPORALD;
        info->dwVersion = 0x00010000;
        info->dwVersionICM = ICVERSION;
        lstrcpynW(info->szName, kCodecName, static_cast<int>(std::size(info->szName)));
        lstrcpynW(info->szDescription, kCodecDescription, static_cast<int>(std::size(info->szDescription)));
        return sizeof(ICINFO);
    }
    case ICM_CONFIGURE:
        if (first == -1) return ICERR_OK;
        MessageBoxW(reinterpret_cast<HWND>(first),
                    config_path().c_str(),
                    L"MMD2FFMPEG reads settings from this file",
                    MB_OK | MB_ICONINFORMATION);
        return ICERR_OK;
    case ICM_ABOUT:
        if (first == -1) return ICERR_OK;
        MessageBoxW(reinterpret_cast<HWND>(first), kCodecDescription, L"MMD2FFMPEG", MB_OK);
        return ICERR_OK;
    case ICM_COMPRESS_QUERY: {
        const auto* input = reinterpret_cast<const BITMAPINFOHEADER*>(first);
        const auto* output = reinterpret_cast<const BITMAPINFOHEADER*>(second);
        if (!input_supported(input)) return ICERR_BADFORMAT;
        return (!output || output->biCompression == kHandler) ? ICERR_OK : ICERR_BADFORMAT;
    }
    case ICM_COMPRESS_GET_FORMAT:
        return get_format(reinterpret_cast<const BITMAPINFOHEADER*>(first), reinterpret_cast<BITMAPINFOHEADER*>(second));
    case ICM_COMPRESS_GET_SIZE:
        return 1;
    case ICM_COMPRESS_FRAMES_INFO: {
        if (!state || !first) return ICERR_BADPARAM;
        const auto* frames = reinterpret_cast<const ICCOMPRESSFRAMES*>(first);
        if (frames->dwRate > 0 && frames->dwScale > 0) {
            const auto rounded = (static_cast<unsigned long long>(frames->dwRate) + frames->dwScale / 2) /
                                 frames->dwScale;
            state->stream_fps = std::clamp(static_cast<int>(rounded), 1, 240);
        }
        return ICERR_OK;
    }
    case ICM_COMPRESS_BEGIN: {
        if (!state) return ICERR_ERROR;
        const auto* input = reinterpret_cast<const BITMAPINFOHEADER*>(first);
        return input_supported(input) && start_ffmpeg(*state, *input) ? ICERR_OK : ICERR_ERROR;
    }
    case ICM_COMPRESS: {
        if (!state) return ICERR_ERROR;
        auto* request = reinterpret_cast<ICCOMPRESS*>(first);
        if (!request || !request->lpInput || !request->lpOutput) return ICERR_BADPARAM;
        if (!send_frame(*state, reinterpret_cast<const std::uint8_t*>(request->lpInput))) return ICERR_ERROR;
        reinterpret_cast<std::uint8_t*>(request->lpOutput)[0] = 0;
        request->lpbiOutput->biSizeImage = 1;
        if (request->lpdwFlags) *request->lpdwFlags = AVIIF_KEYFRAME;
        if (request->lpckid) *request->lpckid = 0;
        return ICERR_OK;
    }
    case ICM_COMPRESS_END:
        if (state) stop_ffmpeg(*state);
        return ICERR_OK;
    default:
        return DefDriverProc(driver_id, driver, message, first, second);
    }
}
