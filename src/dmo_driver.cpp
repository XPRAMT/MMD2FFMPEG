#include <windows.h>
#include <windowsx.h>
#include <dshow.h>
#include <dmo.h>
#include <ocidl.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

// {C42D995C-3D1B-4E44-A96B-767B6C2A4646}
constexpr GUID CLSID_MMD2FFMPEG =
    {0xc42d995c, 0x3d1b, 0x4e44, {0xa9, 0x6b, 0x76, 0x7b, 0x6c, 0x2a, 0x46, 0x46}};
// {65A23874-AE1C-4B10-9F1A-5BC0A8D44B38}
constexpr GUID CLSID_MMD2FFMPEG_SETTINGS =
    {0x65a23874, 0xae1c, 0x4b10, {0x9f, 0x1a, 0x5b, 0xc0, 0xa8, 0xd4, 0x4b, 0x38}};
constexpr GUID MEDIASUBTYPE_M2FF =
    {0x4646324d, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
std::atomic<long> g_objects{0};
std::atomic<long> g_locks{0};

struct Settings {
    std::wstring ffmpeg = L"ffmpeg.exe";
    std::wstring output = L"C:\\APP\\MMD\\MMD2FFMPEG\\out\\mmd-output.mkv";
    std::wstring video_args;
    int fps = 30;
    std::wstring backend = L"nvenc";
    std::wstring codec = L"hevc";
    int bit_depth = 10;
    int preset = 7;
    std::wstring rate_control = L"qp";
    int qp = 20;
    int bitrate_kbps = 20000;
    std::wstring command_template;
};

struct SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX_LOCAL {
    PVOID object;
    ULONG_PTR process_id;
    ULONG_PTR handle_value;
    ULONG granted_access;
    USHORT creator_back_trace_index;
    USHORT object_type_index;
    ULONG handle_attributes;
    ULONG reserved;
};

struct SYSTEM_HANDLE_INFORMATION_EX_LOCAL {
    ULONG_PTR handle_count;
    ULONG_PTR reserved;
    SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX_LOCAL handles[1];
};

std::filesystem::path config_path() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    return count > 0 && count < buffer.size()
        ? std::filesystem::path(buffer.data()) / L"MMD2FFMPEG" / L"config.ini"
        : std::filesystem::path(L"config.ini");
}

std::filesystem::path local_data_dir() { return config_path().parent_path(); }

std::filesystem::path make_log_path() {
    const auto directory = local_data_dir() / L"logs";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::wostringstream name;
    name << std::setfill(L'0') << time.wYear << std::setw(2) << time.wMonth << std::setw(2) << time.wDay
         << L'-' << std::setw(2) << time.wHour << std::setw(2) << time.wMinute << std::setw(2) << time.wSecond
         << L'-' << GetCurrentProcessId() << L".log";
    return directory / name.str();
}

void prune_logs() {
    const auto directory = local_data_dir() / L"logs";
    std::error_code error;
    std::vector<std::filesystem::directory_entry> logs;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error))
        if (entry.is_regular_file(error) && _wcsicmp(entry.path().extension().c_str(), L".log") == 0) logs.push_back(entry);
    std::sort(logs.begin(), logs.end(), [](const auto& left, const auto& right) {
        std::error_code left_error, right_error;
        return left.last_write_time(left_error) > right.last_write_time(right_error);
    });
    for (std::size_t index = 30; index < logs.size(); ++index) std::filesystem::remove(logs[index].path(), error);
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
        else if (key == L"backend" && (value == L"cpu" || value == L"nvenc" || value == L"qsv" || value == L"amf")) settings.backend = value;
        else if (key == L"codec" && (value == L"avc" || value == L"hevc" || value == L"av1")) settings.codec = value;
        else if (key == L"bit_depth") { try { settings.bit_depth = std::stoi(value) == 8 ? 8 : 10; } catch (...) {} }
        else if (key == L"preset") { try { settings.preset = std::clamp(std::stoi(value), 1, 7); } catch (...) {} }
        else if (key == L"rate_control" && (value == L"crf" || value == L"qp" || value == L"vbr")) settings.rate_control = value;
        else if (key == L"qp") { try { settings.qp = std::clamp(std::stoi(value), 0, 51); } catch (...) {} }
        else if (key == L"bitrate_kbps") { try { settings.bitrate_kbps = std::clamp(std::stoi(value), 100, 1000000); } catch (...) {} }
        else if (key == L"command_template") settings.command_template = value;
    }
    if (settings.codec == L"avc") settings.bit_depth = 8;
    if (_wcsicmp(settings.ffmpeg.c_str(), L"C:\\Program Files\\Hybrid\\64bit\\ffmpeg.exe") == 0)
        settings.ffmpeg = L"ffmpeg.exe";
    if (settings.video_args.empty() && !settings.command_template.empty()) {
        const auto input_end = settings.command_template.find(L"-i pipe:0 ");
        const auto output_begin = settings.command_template.rfind(L" \"{output}\"");
        if (input_end != std::wstring::npos && output_begin != std::wstring::npos) {
            const auto arguments_begin = input_end + std::wstring(L"-i pipe:0 ").size();
            if (arguments_begin < output_begin) {
                settings.video_args = settings.command_template.substr(arguments_begin, output_begin - arguments_begin);
            }
        }
    }
    settings.command_template.clear();
    const auto encoder_begin = settings.video_args.find(L"-c:v ");
    const auto fixed_output_begin = settings.video_args.find(L" -pix_fmt ", encoder_begin);
    if (encoder_begin != std::wstring::npos && fixed_output_begin != std::wstring::npos) {
        settings.video_args = settings.video_args.substr(encoder_begin, fixed_output_begin - encoder_begin);
    }
    return settings;
}

void save_settings(const Settings& settings) {
    const auto path = config_path();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::wofstream file(path, std::ios::trunc);
    if (!file) return;
    file << L"ffmpeg=" << settings.ffmpeg << L"\n"
         << L"output=" << settings.output << L"\n"
         << L"fps=" << settings.fps << L"\n"
         << L"backend=" << settings.backend << L"\n"
         << L"codec=" << settings.codec << L"\n"
         << L"bit_depth=" << settings.bit_depth << L"\n"
         << L"preset=" << settings.preset << L"\n"
         << L"rate_control=" << settings.rate_control << L"\n"
         << L"qp=" << settings.qp << L"\n"
         << L"bitrate_kbps=" << settings.bitrate_kbps << L"\n";
    if (!settings.video_args.empty()) file << L"video_args=" << settings.video_args << L"\n";
}

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return value;
}

std::filesystem::path current_output_avi() {
    using NtQuerySystemInformationFn = NTSTATUS(NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
    const auto query = reinterpret_cast<NtQuerySystemInformationFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    if (!query) return {};
    constexpr auto extended_handles = static_cast<SYSTEM_INFORMATION_CLASS>(64);
    std::vector<BYTE> storage(1 << 20);
    ULONG required = 0;
    NTSTATUS status = query(extended_handles, storage.data(), static_cast<ULONG>(storage.size()), &required);
    while (status == static_cast<NTSTATUS>(0xC0000004L) && storage.size() < (1ull << 28)) {
        storage.resize(std::max<std::size_t>(required + 65536, storage.size() * 2));
        status = query(extended_handles, storage.data(), static_cast<ULONG>(storage.size()), &required);
    }
    if (status < 0) return {};
    const auto* information = reinterpret_cast<const SYSTEM_HANDLE_INFORMATION_EX_LOCAL*>(storage.data());
    const ULONG_PTR process_id = GetCurrentProcessId();
    std::filesystem::path newest;
    ULARGE_INTEGER newest_time{};
    std::vector<wchar_t> path_buffer(32768);
    for (ULONG_PTR index = 0; index < information->handle_count; ++index) {
        const auto& entry = information->handles[index];
        if (entry.process_id != process_id) continue;
        const HANDLE handle = reinterpret_cast<HANDLE>(entry.handle_value);
        if (GetFileType(handle) != FILE_TYPE_DISK) continue;
        const DWORD length = GetFinalPathNameByHandleW(handle, path_buffer.data(),
                                                       static_cast<DWORD>(path_buffer.size()),
                                                       FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!length || length >= path_buffer.size()) continue;
        std::wstring path(path_buffer.data(), length);
        if (path.rfind(L"\\\\?\\", 0) == 0) path.erase(0, 4);
        if (lower(std::filesystem::path(path).extension().wstring()) != L".avi") continue;
        FILETIME creation{}, access{}, write{};
        if (!GetFileTime(handle, &creation, &access, &write)) continue;
        ULARGE_INTEGER candidate{};
        candidate.LowPart = creation.dwLowDateTime;
        candidate.HighPart = creation.dwHighDateTime;
        if (candidate.QuadPart >= newest_time.QuadPart) {
            newest_time = candidate;
            newest = path;
        }
    }
    return newest;
}

std::wstring encoding_arguments(const Settings& settings) {
    const bool ten_bit = settings.bit_depth == 10 && settings.codec != L"avc";
    const std::wstring codec_name = settings.codec == L"avc" ? L"h264" : settings.codec;
    std::wstring encoder;
    if (settings.backend == L"cpu")
        encoder = settings.codec == L"avc" ? L"libx264" : settings.codec == L"av1" ? L"libsvtav1" : L"libx265";
    else
        encoder = codec_name + L"_" + settings.backend;
    std::wostringstream args;
    args << L"-c:v " << encoder;
    if (settings.codec == L"hevc" && ten_bit) args << L" -profile:v main10";
    else if (settings.codec == L"avc") args << L" -profile:v high";

    const int level = std::clamp(settings.preset, 1, 7);
    if (settings.backend == L"cpu") {
        static constexpr const wchar_t* software_presets[] = {L"ultrafast", L"superfast", L"veryfast", L"faster", L"fast", L"medium", L"slow"};
        static constexpr int svt_presets[] = {13, 11, 9, 8, 7, 6, 4};
        if (settings.codec == L"av1") args << L" -preset " << svt_presets[level - 1];
        else args << L" -preset " << software_presets[level - 1];
        if (settings.rate_control == L"crf") args << L" -crf " << settings.qp;
        else if (settings.rate_control == L"qp") args << L" -qp " << settings.qp;
        else args << L" -b:v " << settings.bitrate_kbps << L"k";
    } else if (settings.backend == L"nvenc") {
        args << L" -preset p" << level << L" -tune hq";
        if (settings.rate_control == L"crf") args << L" -rc vbr -cq " << settings.qp << L" -b:v 0";
        else if (settings.rate_control == L"qp") args << L" -rc constqp -qp " << settings.qp;
        else args << L" -rc vbr -b:v " << settings.bitrate_kbps << L"k";
    } else if (settings.backend == L"qsv") {
        static constexpr const wchar_t* qsv_presets[] = {L"veryfast", L"faster", L"fast", L"medium", L"slow", L"slower", L"veryslow"};
        args << L" -preset " << qsv_presets[level - 1];
        if (settings.rate_control == L"vbr") args << L" -b:v " << settings.bitrate_kbps << L"k";
        else args << L" -global_quality " << settings.qp;
    } else {
        const wchar_t* quality = level <= 2 ? L"speed" : level <= 5 ? L"balanced" : L"quality";
        args << L" -quality " << quality;
        if (settings.rate_control == L"crf") args << L" -rc qvbr -qvbr_quality_level " << settings.qp;
        else if (settings.rate_control == L"qp")
            args << L" -rc cqp -qp_i " << settings.qp << L" -qp_p " << settings.qp << L" -qp_b " << settings.qp;
        else args << L" -rc vbr_peak -b:v " << settings.bitrate_kbps << L"k";
    }
    return args.str();
}

const wchar_t* output_pixel_format(const Settings& settings) {
    return settings.bit_depth == 10 && settings.codec != L"avc" ? L"p010le" : L"nv12";
}

std::wstring command_prefix(const Settings& settings) {
    const auto pixel_format = output_pixel_format(settings);
    return L"\"" + settings.ffmpeg +
           L"\" -hide_banner -loglevel warning -y -f rawvideo -pixel_format {input_pixel_format} "
           L"-video_size {width}x{height} -framerate {fps} -i pipe:0 "
           L"-vf scale=out_color_matrix=bt709:out_range=tv,format=" + pixel_format + L" ";
}

std::wstring command_suffix(const Settings& settings) {
    const auto pixel_format = output_pixel_format(settings);
    return L" -pix_fmt " + std::wstring(pixel_format) +
           L" -colorspace bt709 -color_primaries bt709 -color_trc bt709 \"{output}\"";
}

void replace_all(std::wstring& value, const std::wstring& from, const std::wstring& to) {
    std::size_t position = 0;
    while ((position = value.find(from, position)) != std::wstring::npos) {
        value.replace(position, from.size(), to);
        position += to.size();
    }
}

std::wstring build_ffmpeg_command(const Settings& settings, int width, int height, int bits) {
    const auto arguments = settings.video_args.empty() ? encoding_arguments(settings) : settings.video_args;
    std::wstring command = command_prefix(settings) + arguments + command_suffix(settings);
    replace_all(command, L"{input_pixel_format}", bits == 24 ? L"bgr24" : L"bgra");
    replace_all(command, L"{width}", std::to_wstring(width));
    replace_all(command, L"{height}", std::to_wstring(height));
    replace_all(command, L"{fps}", std::to_wstring(settings.fps));
    replace_all(command, L"{output}", settings.output);
    return command;
}

std::wstring quote(const std::wstring& value) { return L"\"" + value + L"\""; }

void close_handle(HANDLE& handle) {
    if (handle) { CloseHandle(handle); handle = nullptr; }
}

std::filesystem::path resolve_executable(const std::wstring& executable) {
    if (executable.find_first_of(L"\\/:") != std::wstring::npos) {
        std::error_code error;
        return std::filesystem::exists(executable, error) ? std::filesystem::path(executable) : std::filesystem::path{};
    }
    const DWORD path_length = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (!path_length) return {};
    std::wstring search_path(path_length, L'\0');
    GetEnvironmentVariableW(L"PATH", search_path.data(), path_length);
    search_path.resize(wcslen(search_path.c_str()));
    const DWORD result_length = SearchPathW(search_path.c_str(), executable.c_str(), nullptr, 0, nullptr, nullptr);
    if (!result_length) return {};
    std::wstring resolved(static_cast<std::size_t>(result_length) + 1, L'\0');
    const DWORD copied = SearchPathW(search_path.c_str(), executable.c_str(), nullptr,
                                     static_cast<DWORD>(resolved.size()), resolved.data(), nullptr);
    if (!copied || copied >= resolved.size()) return {};
    resolved.resize(copied);
    return resolved;
}

void write_log_line(HANDLE file, const std::wstring& text) {
    if (!file) return;
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return;
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), length, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

std::wstring decode_process_output(const std::vector<char>& bytes) {
    if (bytes.empty()) return L"FFmpeg exited without an error message.";
    const int source_length = static_cast<int>(bytes.size());
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), source_length, nullptr, 0);
    const UINT code_page = length > 0 ? CP_UTF8 : CP_ACP;
    if (length <= 0) length = MultiByteToWideChar(code_page, 0, bytes.data(), source_length, nullptr, 0);
    if (length <= 0) return L"Unable to decode FFmpeg error output.";
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(code_page, 0, bytes.data(), source_length, result.data(), length);
    return trim(result);
}

struct ProbeResult { bool success; std::wstring message; };

bool test_encoder(const Settings& settings, std::wstring& error_message) {
    const auto ffmpeg_path = resolve_executable(settings.ffmpeg);
    if (ffmpeg_path.empty()) {
        error_message = L"FFmpeg was not found in the system PATH:\n" + settings.ffmpeg;
        return false;
    }
    const auto arguments = settings.video_args.empty() ? encoding_arguments(settings) : settings.video_args;
    const auto pixel_format = output_pixel_format(settings);
    std::wstring command = L"\"" + settings.ffmpeg +
        L"\" -hide_banner -loglevel error -f lavfi -i color=c=black:s=1920x1080:r=1 -frames:v 1 "
        L"-vf format=" + pixel_format + L" " + arguments +
        L" -pix_fmt " + pixel_format + L" -f null -";

    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE output_read = nullptr, output_write = nullptr;
    if (!CreatePipe(&output_read, &output_write, &security, 65536)) {
        error_message = L"Could not create the FFmpeg test output pipe.";
        return false;
    }
    SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0);
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = output_write;
    startup.hStdError = output_write;
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(ffmpeg_path.c_str(), mutable_command.data(), nullptr, nullptr,
                                        TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    close_handle(output_write);
    if (!created) {
        close_handle(output_read);
        error_message = L"Could not start FFmpeg (Windows error " + std::to_wstring(GetLastError()) + L").";
        return false;
    }

    const DWORD wait_result = WaitForSingleObject(process.hProcess, 30000);
    if (wait_result == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
    }
    std::vector<char> output;
    std::array<char, 4096> buffer{};
    DWORD read = 0;
    while (output.size() < 16384 && ReadFile(output_read, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read)
        output.insert(output.end(), buffer.data(), buffer.data() + read);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    close_handle(output_read);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (wait_result == WAIT_TIMEOUT) {
        error_message = L"Encoder test timed out after 30 seconds.";
        return false;
    }
    if (wait_result != WAIT_OBJECT_0 || exit_code != 0) {
        error_message = decode_process_output(output);
        return false;
    }
    return true;
}

std::wstring capability_key(const Settings& settings) {
    const auto ffmpeg_path = resolve_executable(settings.ffmpeg);
    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(ffmpeg_path, error).time_since_epoch().count();
    return L"v2-1920x1080|" + ffmpeg_path.wstring() + L"|" + std::to_wstring(stamp) + L"|" + settings.backend + L"|" +
           settings.codec + L"|" + std::to_wstring(settings.bit_depth);
}

bool load_cached_probe(const Settings& settings, ProbeResult& result) {
    const auto path = local_data_dir() / L"capabilities.cache";
    std::error_code error;
    if (!std::filesystem::exists(path, error) ||
        std::filesystem::file_time_type::clock::now() - std::filesystem::last_write_time(path, error) > std::chrono::hours(24)) return false;
    std::wifstream file(path);
    const auto key = capability_key(settings) + L"=";
    std::wstring line;
    bool found = false;
    while (std::getline(file, line)) {
        if (line.rfind(key, 0) != 0) continue;
        const auto value = line.substr(key.size());
        result.success = value.rfind(L"1|", 0) == 0;
        result.message = value.size() > 2 ? value.substr(2) : (result.success ? L"Available" : L"Unavailable");
        found = true;
    }
    return found;
}

void save_cached_probe(const Settings& settings, const ProbeResult& result) {
    const auto path = local_data_dir() / L"capabilities.cache";
    std::wofstream file(path, std::ios::app);
    std::wstring message = result.message;
    std::replace(message.begin(), message.end(), L'\n', L' ');
    std::replace(message.begin(), message.end(), L'\r', L' ');
    file << capability_key(settings) << L"=" << (result.success ? L"1|" : L"0|") << message << L"\n";
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

class Encoder final : public IMediaObject, public ISpecifyPropertyPages, public IAMVfwCompressDialogs {
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
    virtual ~Encoder() { stop_ffmpeg(); free_type(input_type_); free_type(output_type_); --g_objects; }

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
        if (iid == IID_ISpecifyPropertyPages) {
            *object = static_cast<ISpecifyPropertyPages*>(this);
            outer_->AddRef();
            return S_OK;
        }
        if (iid == IID_IAMVfwCompressDialogs) {
            *object = static_cast<IAMVfwCompressDialogs*>(this);
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
    HRESULT STDMETHODCALLTYPE GetPages(CAUUID* pages) override {
        if (!pages) return E_POINTER;
        pages->cElems = 1;
        pages->pElems = static_cast<GUID*>(CoTaskMemAlloc(sizeof(GUID)));
        if (!pages->pElems) { pages->cElems = 0; return E_OUTOFMEMORY; }
        pages->pElems[0] = CLSID_MMD2FFMPEG_SETTINGS;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ShowDialog(int dialog, HWND parent) override {
        if (dialog == VfwCompressDialog_QueryConfig) return S_OK;
        if (dialog == VfwCompressDialog_QueryAbout) return S_OK;
        if (dialog == VfwCompressDialog_About) {
            MessageBoxW(parent, L"FFmpeg frame bridge for MikuMikuDance", L"MMD2FFMPEG", MB_OK | MB_ICONINFORMATION);
            return S_OK;
        }
        if (dialog != VfwCompressDialog_Config) return E_INVALIDARG;
        IUnknown* object = static_cast<IMediaObject*>(this);
        GUID page = CLSID_MMD2FFMPEG_SETTINGS;
        return OleCreatePropertyFrame(parent, 0, 0, L"MMD2FFMPEG Encoder Settings",
                                      1, &object, 1, &page, GetUserDefaultLCID(), 0, nullptr);
    }
    HRESULT STDMETHODCALLTYPE GetState(LPVOID, int*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetState(LPVOID, int) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SendDriverMessage(int, long, long) override { return E_NOTIMPL; }

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
        auto avi = current_output_avi();
        if (!avi.empty()) {
            avi_output_ = avi;
            avi.replace_extension(L".mkv");
            settings_.output = avi.wstring();
        }
        const auto ffmpeg_path = resolve_executable(settings_.ffmpeg);
        if (ffmpeg_path.empty()) return false;
        final_output_ = settings_.output;
        partial_output_ = final_output_.parent_path() /
            (final_output_.stem().wstring() + L".mmd2ffmpeg-partial-" + std::to_wstring(GetCurrentProcessId()) + final_output_.extension().wstring());
        std::error_code error;
        std::filesystem::remove(partial_output_, error);
        settings_.output = partial_output_.wstring();
        log_path_ = make_log_path();
        prune_logs();
        std::filesystem::create_directories(std::filesystem::path(settings_.output).parent_path(), error);
        log_file_ = CreateFileW(log_path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log_file_ == INVALID_HANDLE_VALUE) log_file_ = nullptr;
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        HANDLE read_pipe = nullptr;
        if (!CreatePipe(&read_pipe, &stdin_write_, &security, 1024 * 1024)) return false;
        SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0);
        auto command = build_ffmpeg_command(settings_, width_, height_, bits_);
        write_log_line(log_file_, L"MMD2FFMPEG command:\r\n" + command + L"\r\n\r\nFFmpeg output:\r\n");
        std::vector<wchar_t> mutable_command(command.begin(), command.end()); mutable_command.push_back(L'\0');
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = read_pipe;
        startup.hStdOutput = log_file_ ? log_file_ : GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = log_file_ ? log_file_ : GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(ffmpeg_path.c_str(), mutable_command.data(), nullptr, nullptr,
                                             TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
        CloseHandle(read_pipe);
        if (!created) { close_handle(stdin_write_); close_handle(log_file_); return false; }
        job_ = CreateJobObjectW(nullptr, nullptr);
        if (job_) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
            AssignProcessToJobObject(job_, process.hProcess);
        }
        process_ = process.hProcess; process_thread_ = process.hThread; started_ = true; return true;
    }
    void stop_ffmpeg() {
        close_handle(stdin_write_);
        bool success = false;
        DWORD exit_code = 1;
        if (process_) {
            const DWORD wait = WaitForSingleObject(process_, 60000);
            if (wait == WAIT_TIMEOUT) {
                if (job_) TerminateJobObject(job_, 1); else TerminateProcess(process_, 1);
                WaitForSingleObject(process_, 5000);
            }
            GetExitCodeProcess(process_, &exit_code);
            success = wait == WAIT_OBJECT_0 && exit_code == 0;
        }
        close_handle(process_thread_); close_handle(process_); close_handle(job_); close_handle(log_file_);
        std::error_code error;
        if (success && !partial_output_.empty() && std::filesystem::exists(partial_output_, error) &&
            std::filesystem::file_size(partial_output_, error) > 0) {
            success = MoveFileExW(partial_output_.c_str(), final_output_.c_str(),
                                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        } else success = false;
        if (!success && !partial_output_.empty()) std::filesystem::remove(partial_output_, error);
        if (success && !avi_output_.empty()) {
            const auto cleanup = local_data_dir() / L"mmd2ffmpeg_cleanup.exe";
            if (std::filesystem::exists(cleanup, error)) {
                std::wstring command = L"\"" + cleanup.wstring() + L"\" \"" + avi_output_.wstring() + L"\" \"" + log_path_.wstring() + L"\"";
                std::vector<wchar_t> mutable_command(command.begin(), command.end()); mutable_command.push_back(L'\0');
                STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION cleanup_process{};
                if (CreateProcessW(cleanup.c_str(), mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                   nullptr, nullptr, &startup, &cleanup_process)) {
                    CloseHandle(cleanup_process.hThread); CloseHandle(cleanup_process.hProcess);
                }
            }
        }
        if (!success && started_) {
            const std::wstring message = L"FFmpeg output failed (exit code " + std::to_wstring(exit_code) +
                                         L").\nThe original MKV and AVI were preserved.\n\nLog: " + log_path_.wstring();
            MessageBoxW(nullptr, message.c_str(), L"MMD2FFMPEG Output Error", MB_OK | MB_ICONERROR);
        }
        started_ = false;
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
    HANDLE process_ = nullptr, process_thread_ = nullptr, stdin_write_ = nullptr, job_ = nullptr, log_file_ = nullptr;
    std::filesystem::path final_output_, partial_output_, avi_output_, log_path_;
    int width_ = 0, height_ = 0, bits_ = 0; LONG stride_ = 0;
    bool bottom_up_ = false, started_ = false, pending_ = false;
    REFERENCE_TIME timestamp_ = 0, duration_ = 0;
    std::vector<BYTE> flipped_;
};

enum ControlId : int {
    ID_BACKEND = 1001, ID_CODEC, ID_DEPTH, ID_PRESET, ID_RATE, ID_QP, ID_BITRATE, ID_COMMAND,
    ID_REFRESH, ID_STATUS
};
constexpr int ID_COMMAND_PREFIX = 1101;
constexpr int ID_COMMAND_SUFFIX = 1102;

class SettingsPropertyPage final : public IPropertyPage {
public:
    SettingsPropertyPage() { ++g_objects; }
    ~SettingsPropertyPage() {
        alive_ = false;
        if (probe_thread_.joinable()) probe_thread_.join();
        Deactivate(); if (site_) site_->Release(); --g_objects;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IPropertyPage) *object = static_cast<IPropertyPage*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG value = --references_; if (!value) delete this; return value;
    }
    HRESULT STDMETHODCALLTYPE SetPageSite(IPropertyPageSite* site) override {
        if (site_) site_->Release();
        site_ = site;
        if (site_) site_->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Activate(HWND parent, LPCRECT rect, BOOL) override {
        if (window_) return E_UNEXPECTED;
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpszClassName = L"MMD2FFMPEGSettingsPage";
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        RegisterClassW(&window_class);
        settings_ = load_settings();
        window_ = CreateWindowExW(0, window_class.lpszClassName, L"", WS_CHILD | WS_VISIBLE,
                                  rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
                                  parent, nullptr, window_class.hInstance, this);
        return window_ ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
    HRESULT STDMETHODCALLTYPE Deactivate() override {
        if (window_) { DestroyWindow(window_); window_ = nullptr; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPageInfo(PROPPAGEINFO* info) override {
        if (!info) return E_POINTER;
        ZeroMemory(info, sizeof(*info)); info->cb = sizeof(*info);
        const wchar_t* title = L"MMD2FFMPEG Encoder Settings";
        const auto bytes = (wcslen(title) + 1) * sizeof(wchar_t);
        info->pszTitle = static_cast<LPOLESTR>(CoTaskMemAlloc(bytes));
        if (!info->pszTitle) return E_OUTOFMEMORY;
        CopyMemory(info->pszTitle, title, bytes);
        info->size = {520, 420};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetObjects(ULONG, IUnknown**) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Show(UINT command) override {
        if (!window_) return E_UNEXPECTED;
        ShowWindow(window_, command); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Move(LPCRECT rect) override {
        if (!window_ || !rect) return E_POINTER;
        MoveWindow(window_, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, TRUE);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE IsPageDirty() override { return dirty_ ? S_OK : S_FALSE; }
    HRESULT STDMETHODCALLTYPE Apply() override {
        if (!window_) return E_UNEXPECTED;
        Settings candidate = settings_;
        candidate.backend = combo_index(ID_BACKEND) == 0 ? L"cpu" : combo_index(ID_BACKEND) == 2 ? L"qsv" : combo_index(ID_BACKEND) == 3 ? L"amf" : L"nvenc";
        candidate.codec = combo_text(ID_CODEC) == L"AVC (H.264)" ? L"avc" :
                          combo_text(ID_CODEC) == L"AV1" ? L"av1" : L"hevc";
        candidate.bit_depth = combo_text(ID_DEPTH) == L"10-bit" && candidate.codec != L"avc" ? 10 : 8;
        candidate.preset = combo_index(ID_BACKEND) == 3 ? (combo_index(ID_PRESET) == 0 ? 1 : combo_index(ID_PRESET) == 1 ? 4 : 7) : combo_index(ID_PRESET) + 1;
        candidate.rate_control = combo_index(ID_RATE) == 0 ? L"crf" : combo_index(ID_RATE) == 1 ? L"qp" : L"vbr";
        candidate.qp = std::clamp(edit_number(ID_QP, 20), 0, 51);
        candidate.bitrate_kbps = std::clamp(edit_number(ID_BITRATE, 20000), 100, 1000000);
        candidate.video_args = edit_text(ID_COMMAND);
        std::wstring test_error;
        if (!test_encoder(candidate, test_error)) {
            MessageBoxW(window_, (L"The encoder test failed. Settings were not saved.\n\n" + test_error).c_str(),
                        L"MMD2FFMPEG Encoder Test", MB_OK | MB_ICONERROR);
            return E_FAIL;
        }
        settings_ = std::move(candidate);
        save_settings(settings_); dirty_ = false;
        if (site_) site_->OnStatusChange(PROPPAGESTATUS_CLEAN);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Help(LPCOLESTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE TranslateAccelerator(MSG* message) override {
        return window_ && IsDialogMessageW(window_, message) ? S_OK : S_FALSE;
    }

private:
    static HWND label(HWND parent, const wchar_t* text, int x, int y) {
        return CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, 120, 22,
                             parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    }
    HWND control(const wchar_t* type, DWORD style, int id, int x, int y, int width = 190, int height = 24) {
        return CreateWindowExW(WS_EX_CLIENTEDGE, type, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                               x, y, width, height, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               GetModuleHandleW(nullptr), nullptr);
    }
    void add_combo(int id, int x, int y, std::initializer_list<const wchar_t*> values, int selected) {
        HWND combo = control(L"COMBOBOX", CBS_DROPDOWNLIST | WS_VSCROLL, id, x, y, 190, 120);
        for (const auto* value : values) ComboBox_AddString(combo, value);
        ComboBox_SetCurSel(combo, selected);
    }
    void create_controls() {
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        label(window_, L"Encoder", 15, 17); add_combo(ID_BACKEND, 135, 12, {L"CPU (software)", L"NVIDIA NVENC", L"Intel Quick Sync", L"AMD AMF"}, settings_.backend == L"cpu" ? 0 : settings_.backend == L"qsv" ? 2 : settings_.backend == L"amf" ? 3 : 1);
        label(window_, L"Codec", 15, 49); add_combo(ID_CODEC, 135, 44, {L"AVC (H.264)", L"HEVC (H.265)", L"AV1"}, settings_.codec == L"avc" ? 0 : settings_.codec == L"av1" ? 2 : 1);
        label(window_, L"Bit depth", 15, 81); add_combo(ID_DEPTH, 135, 76, {L"8-bit", L"10-bit"}, settings_.bit_depth == 10 ? 1 : 0);
        label(window_, L"Encoder preset", 15, 113); add_combo(ID_PRESET, 135, 108, {L"P1", L"P2", L"P3", L"P4", L"P5", L"P6", L"P7"}, settings_.preset - 1);
        label(window_, L"Rate control", 15, 145); add_combo(ID_RATE, 135, 140, {L"CQ (constant quality)", L"Constant QP", L"VBR target bitrate"}, settings_.rate_control == L"crf" ? 0 : settings_.rate_control == L"vbr" ? 2 : 1);
        label(window_, L"Quality / QP", 15, 177); HWND qp = control(L"EDIT", ES_NUMBER | ES_AUTOHSCROLL, ID_QP, 135, 172, 90);
        label(window_, L"Bitrate (kbps)", 240, 177); HWND bitrate = control(L"EDIT", ES_NUMBER | ES_AUTOHSCROLL, ID_BITRATE, 345, 172, 75);
        SetWindowTextW(qp, std::to_wstring(settings_.qp).c_str()); SetWindowTextW(bitrate, std::to_wstring(settings_.bitrate_kbps).c_str());
        CreateWindowW(L"BUTTON", L"Retest", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                      410, 205, 95, 24, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_REFRESH)), GetModuleHandleW(nullptr), nullptr);
        CreateWindowW(L"STATIC", L"Encoder status: testing...", WS_CHILD | WS_VISIBLE,
                      15, 210, 385, 22, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STATUS)), GetModuleHandleW(nullptr), nullptr);
        CreateWindowW(L"STATIC", L"Complete FFmpeg command (middle section is editable)",
                      WS_CHILD | WS_VISIBLE, 15, 238, 490, 22, window_, nullptr, GetModuleHandleW(nullptr), nullptr);
        CreateWindowW(L"STATIC", command_prefix(settings_).c_str(),
                      WS_CHILD | WS_VISIBLE, 15, 260, 490, 50, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_COMMAND_PREFIX)), GetModuleHandleW(nullptr), nullptr);
        HWND command = control(L"EDIT", ES_AUTOHSCROLL, ID_COMMAND, 15, 313, 490);
        SetWindowTextW(command, (settings_.video_args.empty() ? encoding_arguments(settings_) : settings_.video_args).c_str());
        CreateWindowW(L"STATIC", command_suffix(settings_).c_str(),
                      WS_CHILD | WS_VISIBLE, 15, 343, 490, 45, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_COMMAND_SUFFIX)), GetModuleHandleW(nullptr), nullptr);
        EnumChildWindows(window_, [](HWND child, LPARAM value) -> BOOL { SendMessageW(child, WM_SETFONT, value, TRUE); return TRUE; }, reinterpret_cast<LPARAM>(font));
        rebuild_backend_options();
        update_controls();
        start_probe();
    }
    void reset_combo(int id, std::initializer_list<const wchar_t*> values, int selected) {
        HWND combo = GetDlgItem(window_, id);
        ComboBox_ResetContent(combo);
        for (const auto* value : values) ComboBox_AddString(combo, value);
        ComboBox_SetCurSel(combo, std::clamp(selected, 0, static_cast<int>(values.size()) - 1));
    }
    void rebuild_backend_options() {
        const int backend = combo_index(ID_BACKEND);
        const int old_level = std::clamp(settings_.preset, 1, 7);
        updating_command_ = true;
        if (backend == 0) {
            if (combo_index(ID_CODEC) == 2)
                reset_combo(ID_PRESET, {L"13 (fastest)", L"11", L"9", L"8", L"7", L"6", L"4 (best quality)"}, old_level - 1);
            else
                reset_combo(ID_PRESET, {L"ultrafast", L"superfast", L"veryfast", L"faster", L"fast", L"medium", L"slow"}, old_level - 1);
            reset_combo(ID_RATE, {L"CRF (constant quality)", L"Constant QP", L"VBR target bitrate"}, settings_.rate_control == L"crf" ? 0 : settings_.rate_control == L"vbr" ? 2 : 1);
        } else if (backend == 1) {
            reset_combo(ID_PRESET, {L"P1 (fastest)", L"P2", L"P3", L"P4", L"P5", L"P6", L"P7 (best quality)"}, old_level - 1);
            reset_combo(ID_RATE, {L"CQ (constant quality)", L"Constant QP", L"VBR target bitrate"}, settings_.rate_control == L"crf" ? 0 : settings_.rate_control == L"vbr" ? 2 : 1);
        } else if (backend == 2) {
            reset_combo(ID_PRESET, {L"veryfast", L"faster", L"fast", L"medium", L"slow", L"slower", L"veryslow"}, old_level - 1);
            reset_combo(ID_RATE, {L"ICQ / global quality", L"Constant QP", L"VBR target bitrate"}, settings_.rate_control == L"crf" ? 0 : settings_.rate_control == L"vbr" ? 2 : 1);
        } else {
            const int amf_selected = old_level <= 2 ? 0 : old_level <= 5 ? 1 : 2;
            reset_combo(ID_PRESET, {L"speed", L"balanced", L"quality"}, amf_selected);
            reset_combo(ID_RATE, {L"QVBR quality", L"Constant QP", L"VBR target bitrate"}, settings_.rate_control == L"crf" ? 0 : settings_.rate_control == L"vbr" ? 2 : 1);
        }
        updating_command_ = false;
    }
    void start_probe(bool force = false) {
        if (probe_running_) return;
        if (probe_thread_.joinable()) probe_thread_.join();
        sync_structured_settings();
        Settings candidate = settings_;
        candidate.video_args = encoding_arguments(candidate);
        ProbeResult cached{};
        if (!force && load_cached_probe(candidate, cached)) {
            auto* result = new ProbeResult(std::move(cached));
            PostMessageW(window_, WM_APP + 42, 0, reinterpret_cast<LPARAM>(result));
            return;
        }
        SetWindowTextW(GetDlgItem(window_, ID_STATUS), L"Encoder status: testing...");
        EnableWindow(GetDlgItem(window_, ID_REFRESH), FALSE);
        probe_running_ = true;
        const HWND target = window_;
        probe_thread_ = std::thread([this, target, candidate]() {
            std::wstring message;
            const bool success = test_encoder(candidate, message);
            auto* result = new ProbeResult{success, success ? L"Available" : message};
            save_cached_probe(candidate, *result);
            if (alive_ && IsWindow(target)) PostMessageW(target, WM_APP + 42, 0, reinterpret_cast<LPARAM>(result));
            else delete result;
        });
    }
    void update_controls() {
        const bool avc = combo_index(ID_CODEC) == 0;
        if (avc) ComboBox_SetCurSel(GetDlgItem(window_, ID_DEPTH), 0);
        EnableWindow(GetDlgItem(window_, ID_DEPTH), !avc);
        const bool bitrate = combo_index(ID_RATE) == 2;
        EnableWindow(GetDlgItem(window_, ID_QP), !bitrate);
        EnableWindow(GetDlgItem(window_, ID_BITRATE), bitrate);
    }
    int combo_index(int id) const { return static_cast<int>(ComboBox_GetCurSel(GetDlgItem(window_, id))); }
    std::wstring combo_text(int id) const {
        wchar_t text[64]{}; GetWindowTextW(GetDlgItem(window_, id), text, static_cast<int>(std::size(text))); return text;
    }
    int edit_number(int id, int fallback) const {
        wchar_t text[32]{}; GetWindowTextW(GetDlgItem(window_, id), text, static_cast<int>(std::size(text)));
        try { return std::stoi(text); } catch (...) { return fallback; }
    }
    std::wstring edit_text(int id) const {
        const HWND edit = GetDlgItem(window_, id);
        const int length = GetWindowTextLengthW(edit);
        std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
        GetWindowTextW(edit, text.data(), length + 1); text.resize(static_cast<std::size_t>(length));
        return text;
    }
    void sync_structured_settings() {
        settings_.backend = combo_index(ID_BACKEND) == 0 ? L"cpu" : combo_index(ID_BACKEND) == 2 ? L"qsv" : combo_index(ID_BACKEND) == 3 ? L"amf" : L"nvenc";
        settings_.codec = combo_index(ID_CODEC) == 0 ? L"avc" : combo_index(ID_CODEC) == 2 ? L"av1" : L"hevc";
        settings_.bit_depth = combo_index(ID_DEPTH) == 1 && settings_.codec != L"avc" ? 10 : 8;
        settings_.preset = combo_index(ID_BACKEND) == 3 ? (combo_index(ID_PRESET) == 0 ? 1 : combo_index(ID_PRESET) == 1 ? 4 : 7) : combo_index(ID_PRESET) + 1;
        settings_.rate_control = combo_index(ID_RATE) == 0 ? L"crf" : combo_index(ID_RATE) == 1 ? L"qp" : L"vbr";
        settings_.qp = std::clamp(edit_number(ID_QP, 20), 0, 51);
        settings_.bitrate_kbps = std::clamp(edit_number(ID_BITRATE, 20000), 100, 1000000);
    }
    void update_command_display() {
        SetWindowTextW(GetDlgItem(window_, ID_COMMAND_PREFIX), command_prefix(settings_).c_str());
        SetWindowTextW(GetDlgItem(window_, ID_COMMAND_SUFFIX), command_suffix(settings_).c_str());
    }
    void changed(int id) {
        dirty_ = true;
        if (id == ID_BACKEND || id == ID_CODEC) rebuild_backend_options();
        update_controls();
        if (id != ID_COMMAND) {
            sync_structured_settings();
            updating_command_ = true;
            SetWindowTextW(GetDlgItem(window_, ID_COMMAND), encoding_arguments(settings_).c_str());
            update_command_display();
            updating_command_ = false;
        }
        if (id == ID_BACKEND || id == ID_CODEC || id == ID_DEPTH) {
            SetWindowTextW(GetDlgItem(window_, ID_STATUS), L"Encoder status: not tested");
            if (!probe_running_) start_probe();
        }
        if (site_) site_->OnStatusChange(PROPPAGESTATUS_DIRTY);
    }
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<SettingsPropertyPage*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<SettingsPropertyPage*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); self->window_ = window;
        } else if (message == WM_CREATE && self) self->create_controls();
        else if (message == WM_APP + 42 && self) {
            auto* result = reinterpret_cast<ProbeResult*>(lparam);
            self->probe_running_ = false;
            self->probe_available_ = result->success;
            std::wstring status = result->success ? L"Encoder status: Available" : L"Encoder status: Unavailable - " + result->message;
            if (status.size() > 180) status.resize(180);
            SetWindowTextW(GetDlgItem(window, ID_STATUS), status.c_str());
            EnableWindow(GetDlgItem(window, ID_REFRESH), TRUE);
            delete result;
        }
        else if (message == WM_COMMAND && self && LOWORD(wparam) == ID_REFRESH && HIWORD(wparam) == BN_CLICKED) self->start_probe(true);
        else if (message == WM_COMMAND && self && !self->updating_command_ &&
                 (HIWORD(wparam) == CBN_SELCHANGE || HIWORD(wparam) == EN_CHANGE || HIWORD(wparam) == BN_CLICKED))
            self->changed(LOWORD(wparam));
        return DefWindowProcW(window, message, wparam, lparam);
    }

    std::atomic<ULONG> references_{1};
    IPropertyPageSite* site_ = nullptr;
    HWND window_ = nullptr;
    Settings settings_{};
    bool dirty_ = false;
    bool updating_command_ = false;
    std::atomic<bool> alive_{true};
    bool probe_running_ = false;
    bool probe_available_ = false;
    std::thread probe_thread_;
};

class Factory final : public IClassFactory {
public:
    explicit Factory(bool settings_page) : settings_page_(settings_page) {}
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
        if (settings_page_) {
            if (outer) return CLASS_E_NOAGGREGATION;
            if (!object) return E_POINTER;
            auto* page = new (std::nothrow) SettingsPropertyPage();
            if (!page) return E_OUTOFMEMORY;
            const HRESULT result = page->QueryInterface(iid, object); page->Release(); return result;
        }
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
    bool settings_page_ = false;
};

} // namespace

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID iid, void** object) {
    if (clsid != CLSID_MMD2FFMPEG && clsid != CLSID_MMD2FFMPEG_SETTINGS) return CLASS_E_CLASSNOTAVAILABLE;
    auto* factory = new (std::nothrow) Factory(clsid == CLSID_MMD2FFMPEG_SETTINGS);
    if (!factory) return E_OUTOFMEMORY;
    const HRESULT result = factory->QueryInterface(iid, object);
    factory->Release(); return result;
}

extern "C" HRESULT __stdcall DllCanUnloadNow() {
    return g_objects == 0 && g_locks == 0 ? S_OK : S_FALSE;
}
