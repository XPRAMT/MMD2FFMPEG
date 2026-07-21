#include <windows.h>
#include <windowsx.h>
#include <dshow.h>
#include <dmo.h>
#include <ocidl.h>
#include <shellapi.h>
#include <commctrl.h>
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

#include "resource.h"

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

HINSTANCE module_instance() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&module_instance), &module);
    return module;
}

struct Settings {
    std::wstring ffmpeg = L"ffmpeg.exe";
    std::wstring video_args;
    int fps = 30;
    std::wstring backend = L"cpu";
    std::wstring codec = L"hevc";
    int bit_depth = 10;
    int preset = 6;
    std::wstring rate_control = L"crf";
    int qp = 18;
    int bitrate_kbps = 20000;
    std::wstring audio_format = L"none";
    std::wstring audio_sample_rate = L"original";
    std::wstring audio_bit_depth = L"original";
    std::wstring language = L"system";
    std::wstring command_template;
};

enum class UiLanguage { TraditionalChinese, SimplifiedChinese, Japanese, English };

struct UiStrings {
    const wchar_t* title;
    const wchar_t* language;
    const wchar_t* encoder;
    const wchar_t* codec;
    const wchar_t* bit_depth;
    const wchar_t* preset;
    const wchar_t* rate_control;
    const wchar_t* quality;
    const wchar_t* bitrate;
    const wchar_t* command_heading;
    const wchar_t* not_tested;
    const wchar_t* testing;
    const wchar_t* test_passed;
    const wchar_t* settings_changed;
    const wchar_t* test_failed;
    const wchar_t* test_required;
    const wchar_t* test_button;
    const wchar_t* open_log_button;
    const wchar_t* open_log_failed_message;
    const wchar_t* log_title;
    const wchar_t* required_message;
    const wchar_t* required_title;
};

struct UiOptions {
    const wchar_t* software;
    const wchar_t* fastest;
    const wchar_t* best_quality;
    const wchar_t* constant_quality;
    const wchar_t* constant_qp;
    const wchar_t* target_bitrate;
    const wchar_t* global_quality;
    const wchar_t* speed;
    const wchar_t* balanced;
    const wchar_t* quality;
};

UiLanguage system_ui_language() {
    const LANGID language = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(language) == LANG_JAPANESE) return UiLanguage::Japanese;
    if (PRIMARYLANGID(language) == LANG_CHINESE) {
        const WORD sublanguage = SUBLANGID(language);
        return sublanguage == SUBLANG_CHINESE_TRADITIONAL || sublanguage == SUBLANG_CHINESE_HONGKONG ||
               sublanguage == SUBLANG_CHINESE_MACAU ? UiLanguage::TraditionalChinese : UiLanguage::SimplifiedChinese;
    }
    return UiLanguage::English;
}

UiLanguage ui_language(const std::wstring& value) {
    if (value == L"zh-TW") return UiLanguage::TraditionalChinese;
    if (value == L"zh-CN") return UiLanguage::SimplifiedChinese;
    if (value == L"ja") return UiLanguage::Japanese;
    if (value == L"en") return UiLanguage::English;
    return system_ui_language();
}

const UiStrings& ui_strings(UiLanguage language) {
    static constexpr UiStrings traditional{
        L"MMD2FFMPEG 編碼器設定", L"語言", L"編碼器", L"編碼格式", L"位元深度", L"編碼預設",
        L"碼率控制", L"品質 / QP", L"位元率 (kbps)", L"完整 FFmpeg 指令（中間區段可編輯）",
        L"編碼器狀態：尚未測試", L"編碼器狀態：測試中…", L"編碼器狀態：測試通過",
        L"編碼器狀態：設定已變更，請重新測試", L"編碼器狀態：測試失敗 - ",
        L"通過測試後才能儲存或套用。", L"測試編碼", L"開啟log",
        L"無法開啟編碼 log 資料夾。", L"MMD2FFMPEG log",
        L"請先測試目前的編碼指令。\n\n通過測試後才能儲存或套用設定。",
        L"需要測試 MMD2FFMPEG 編碼器"};
    static constexpr UiStrings simplified{
        L"MMD2FFMPEG 编码器设置", L"语言", L"编码器", L"编码格式", L"位深度", L"编码预设",
        L"码率控制", L"质量 / QP", L"比特率 (kbps)", L"完整 FFmpeg 命令（中间部分可编辑）",
        L"编码器状态：尚未测试", L"编码器状态：测试中…", L"编码器状态：测试通过",
        L"编码器状态：设置已更改，请重新测试", L"编码器状态：测试失败 - ",
        L"测试通过后才能保存或应用。", L"测试编码", L"打开日志",
        L"无法打开编码日志文件夹。", L"MMD2FFMPEG 日志",
        L"请先测试当前的编码命令。\n\n测试通过后才能保存或应用设置。",
        L"需要测试 MMD2FFMPEG 编码器"};
    static constexpr UiStrings japanese{
        L"MMD2FFMPEG エンコーダー設定", L"言語", L"エンコーダー", L"コーデック", L"ビット深度", L"プリセット",
        L"レート制御", L"品質 / QP", L"ビットレート (kbps)", L"完全な FFmpeg コマンド（中央部分は編集可能）",
        L"エンコーダー状態：未テスト", L"エンコーダー状態：テスト中…", L"エンコーダー状態：テスト合格",
        L"エンコーダー状態：設定が変更されました。再テストしてください", L"エンコーダー状態：テスト失敗 - ",
        L"保存または適用する前にテストに合格する必要があります。", L"エンコーダーをテスト", L"ログを開く",
        L"エンコードログのフォルダーを開けません。", L"MMD2FFMPEG ログ",
        L"現在のエンコーダーコマンドを先にテストしてください。\n\nテストに合格するまで設定を保存または適用できません。",
        L"MMD2FFMPEG エンコーダーのテストが必要です"};
    static constexpr UiStrings english{
        L"MMD2FFMPEG Encoder Settings", L"Language", L"Encoder", L"Codec", L"Bit depth", L"Encoder preset",
        L"Rate control", L"Quality / QP", L"Bitrate (kbps)", L"Complete FFmpeg command (middle section is editable)",
        L"Encoder status: not tested", L"Encoder status: testing...", L"Encoder status: test passed",
        L"Encoder status: settings changed; test again", L"Encoder status: test failed - ",
        L"Test must pass before saving or applying.", L"Test encoder", L"Open log",
        L"Could not open the encoding log folder.", L"MMD2FFMPEG Log",
        L"Test the current encoder command first.\n\nSettings can only be saved or applied after the test passes.",
        L"MMD2FFMPEG Encoder Test Required"};
    switch (language) {
    case UiLanguage::TraditionalChinese: return traditional;
    case UiLanguage::SimplifiedChinese: return simplified;
    case UiLanguage::Japanese: return japanese;
    default: return english;
    }
}

const UiOptions& ui_options(UiLanguage language) {
    static constexpr UiOptions traditional{L"軟體", L"最快", L"最佳品質", L"固定品質", L"固定 QP",
                                              L"VBR 目標位元率", L"全域品質", L"速度", L"平衡", L"品質"};
    static constexpr UiOptions simplified{L"软件", L"最快", L"最佳质量", L"恒定质量", L"恒定 QP",
                                             L"VBR 目标比特率", L"全局质量", L"速度", L"平衡", L"质量"};
    static constexpr UiOptions japanese{L"ソフトウェア", L"最速", L"最高品質", L"固定品質", L"固定 QP",
                                          L"VBR 目標ビットレート", L"グローバル品質", L"速度", L"バランス", L"品質"};
    static constexpr UiOptions english{L"software", L"fastest", L"best quality", L"constant quality", L"Constant QP",
                                         L"VBR target bitrate", L"global quality", L"speed", L"balanced", L"quality"};
    switch (language) {
    case UiLanguage::TraditionalChinese: return traditional;
    case UiLanguage::SimplifiedChinese: return simplified;
    case UiLanguage::Japanese: return japanese;
    default: return english;
    }
}

int language_index(const std::wstring& value) {
    if (value == L"zh-TW") return 1;
    if (value == L"zh-CN") return 2;
    if (value == L"ja") return 3;
    if (value == L"en") return 4;
    return 0;
}

const wchar_t* language_key(int index) {
    static constexpr const wchar_t* values[]{L"system", L"zh-TW", L"zh-CN", L"ja", L"en"};
    return values[std::clamp(index, 0, 4)];
}

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

std::filesystem::path logs_directory() { return local_data_dir() / L"logs"; }

std::filesystem::path make_log_path() {
    const auto directory = logs_directory();
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
    const auto directory = logs_directory();
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
        else if (key == L"audio_format" && (value == L"flac" || value == L"wav" || value == L"none")) settings.audio_format = value;
        else if (key == L"audio_sample_rate" && (value == L"original" || value == L"hires")) settings.audio_sample_rate = value;
        else if (key == L"audio_bit_depth" && (value == L"original" || value == L"24")) settings.audio_bit_depth = value;
        else if (key == L"language" && (value == L"system" || value == L"zh-TW" || value == L"zh-CN" || value == L"ja" || value == L"en")) settings.language = value;
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
         << L"fps=" << settings.fps << L"\n"
         << L"backend=" << settings.backend << L"\n"
         << L"codec=" << settings.codec << L"\n"
         << L"bit_depth=" << settings.bit_depth << L"\n"
         << L"preset=" << settings.preset << L"\n"
         << L"rate_control=" << settings.rate_control << L"\n"
         << L"qp=" << settings.qp << L"\n"
         << L"bitrate_kbps=" << settings.bitrate_kbps << L"\n"
         << L"audio_format=" << settings.audio_format << L"\n"
         << L"audio_sample_rate=" << settings.audio_sample_rate << L"\n"
         << L"audio_bit_depth=" << settings.audio_bit_depth << L"\n";
    file << L"language=" << settings.language << L"\n";
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

std::wstring recording_date_metadata();

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
           L" -colorspace bt709 -color_primaries bt709 -color_trc bt709"
           L" -metadata date_recorded=" + recording_date_metadata() + L" \"{output}\"";
}

void replace_all(std::wstring& value, const std::wstring& from, const std::wstring& to) {
    std::size_t position = 0;
    while ((position = value.find(from, position)) != std::wstring::npos) {
        value.replace(position, from.size(), to);
        position += to.size();
    }
}

std::wstring recording_date_metadata() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::wostringstream value;
    value << time.wYear << L'-' << time.wMonth << L'-' << time.wDay;
    return value.str();
}

std::wstring build_ffmpeg_command(const Settings& settings, int width, int height, int bits,
                                  const std::wstring& output_path) {
    const auto arguments = settings.video_args.empty() ? encoding_arguments(settings) : settings.video_args;
    std::wstring command = command_prefix(settings) + arguments + command_suffix(settings);
    replace_all(command, L"{input_pixel_format}", bits == 24 ? L"bgr24" : L"bgra");
    replace_all(command, L"{width}", std::to_wstring(width));
    replace_all(command, L"{height}", std::to_wstring(height));
    replace_all(command, L"{fps}", std::to_wstring(settings.fps));
    replace_all(command, L"{output}", output_path);
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

std::wstring ffmpeg_version_line(const std::filesystem::path& ffmpeg_path) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE output_read = nullptr;
    HANDLE output_write = nullptr;
    if (!CreatePipe(&output_read, &output_write, &security, 65536)) return L"Unavailable";
    SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0);
    std::wstring command = L"\"" + ffmpeg_path.wstring() + L"\" -version";
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
        return L"Unavailable (Windows error " + std::to_wstring(GetLastError()) + L")";
    }
    const DWORD wait_result = WaitForSingleObject(process.hProcess, 5000);
    if (wait_result == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 1000);
    }
    std::vector<char> output;
    std::array<char, 4096> buffer{};
    DWORD read = 0;
    while (output.size() < 16384 && ReadFile(output_read, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read)
        output.insert(output.end(), buffer.data(), buffer.data() + read);
    close_handle(output_read);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    const std::wstring version = decode_process_output(output);
    const auto newline = version.find_first_of(L"\r\n");
    return newline == std::wstring::npos ? version : version.substr(0, newline);
}

std::wstring format_local_time() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::wostringstream value;
    value << std::setfill(L'0') << time.wYear << L'-' << std::setw(2) << time.wMonth << L'-' << std::setw(2) << time.wDay
          << L' ' << std::setw(2) << time.wHour << L':' << std::setw(2) << time.wMinute << L':' << std::setw(2) << time.wSecond;
    return value.str();
}

struct ProbeResult { bool success; std::wstring message; std::wstring signature; };

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

std::wstring command_test_signature(const Settings& settings) {
    const auto ffmpeg_path = resolve_executable(settings.ffmpeg);
    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(ffmpeg_path, error).time_since_epoch().count();
    const auto arguments = settings.video_args.empty() ? encoding_arguments(settings) : settings.video_args;
    return L"v3-1920x1080|" + ffmpeg_path.wstring() + L"|" + std::to_wstring(stamp) + L"|" + settings.backend + L"|" +
           settings.codec + L"|" + std::to_wstring(settings.bit_depth) + L"|" + arguments;
}

std::wstring capability_key(const Settings& settings) {
    const auto signature = command_test_signature(settings);
    std::uint64_t hash = 1469598103934665603ull;
    for (const wchar_t character : signature) {
        hash ^= static_cast<std::uint64_t>(character);
        hash *= 1099511628211ull;
    }
    return L"command-" + std::to_wstring(hash);
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
        result.signature = command_test_signature(settings);
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
        ++input_frame_count_;
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
        const Settings dialog_settings = load_settings();
        return OleCreatePropertyFrame(parent, 0, 0, ui_strings(ui_language(dialog_settings.language)).title,
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
        if (avi.empty()) return false;
        avi_output_ = avi;
        avi.replace_extension(L".mkv");
        const auto ffmpeg_path = resolve_executable(settings_.ffmpeg);
        if (ffmpeg_path.empty()) return false;
        final_output_ = avi;
        partial_output_ = final_output_.parent_path() /
            (final_output_.stem().wstring() + L".mmd2ffmpeg-partial-" + std::to_wstring(GetCurrentProcessId()) + final_output_.extension().wstring());
        std::error_code error;
        std::filesystem::remove(partial_output_, error);
        log_path_ = make_log_path();
        prune_logs();
        std::filesystem::create_directories(partial_output_.parent_path(), error);
        log_file_ = CreateFileW(log_path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log_file_ == INVALID_HANDLE_VALUE) log_file_ = nullptr;
        const std::wstring ffmpeg_version = ffmpeg_version_line(ffmpeg_path);
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        HANDLE read_pipe = nullptr;
        if (!CreatePipe(&read_pipe, &stdin_write_, &security, 1024 * 1024)) return false;
        SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0);
        auto command = build_ffmpeg_command(settings_, width_, height_, bits_, partial_output_.wstring());
        std::wostringstream header;
        header << L"MMD2FFMPEG output diagnostics\r\n"
               << L"Started: " << format_local_time() << L"\r\n"
               << L"FFmpeg version: " << ffmpeg_version << L"\r\n"
               << L"Input: " << width_ << L"x" << height_ << L", RGB" << bits_ << L"\r\n"
               << L"Declared FPS (MMD): " << settings_.fps << L"\r\n\r\n"
               << L"MMD2FFMPEG command:\r\n" << command << L"\r\n\r\nFFmpeg output:\r\n";
        write_log_line(log_file_, header.str());
        std::vector<wchar_t> mutable_command(command.begin(), command.end()); mutable_command.push_back(L'\0');
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = read_pipe;
        startup.hStdOutput = log_file_ ? log_file_ : GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = log_file_ ? log_file_ : GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(ffmpeg_path.c_str(), mutable_command.data(), nullptr, nullptr,
                                             TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
        CloseHandle(read_pipe);
        if (!created) {
            write_log_line(log_file_, L"\r\n[MMD2FFMPEG] FFmpeg could not be started (Windows error " +
                                      std::to_wstring(GetLastError()) + L").\r\n");
            close_handle(stdin_write_); close_handle(log_file_); return false;
        }
        job_ = CreateJobObjectW(nullptr, nullptr);
        if (job_) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
            AssignProcessToJobObject(job_, process.hProcess);
        }
        process_ = process.hProcess; process_thread_ = process.hThread;
        input_frame_count_ = 0;
        encoding_started_at_ = std::chrono::steady_clock::now();
        started_ = true;
        return true;
    }
    void stop_ffmpeg() {
        close_handle(stdin_write_);
        bool success = false;
        DWORD exit_code = 1;
        DWORD wait_result = WAIT_FAILED;
        if (process_) {
            wait_result = WaitForSingleObject(process_, 60000);
            if (wait_result == WAIT_TIMEOUT) {
                if (job_) TerminateJobObject(job_, 1); else TerminateProcess(process_, 1);
                WaitForSingleObject(process_, 5000);
            }
            GetExitCodeProcess(process_, &exit_code);
            success = wait_result == WAIT_OBJECT_0 && exit_code == 0;
        }
        const auto elapsed = encoding_started_at_.time_since_epoch().count() == 0
            ? std::chrono::milliseconds(0)
            : std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - encoding_started_at_);
        close_handle(process_thread_); close_handle(process_); close_handle(job_);
        std::error_code error;
        std::uintmax_t partial_size = 0;
        if (!partial_output_.empty() && std::filesystem::exists(partial_output_, error))
            partial_size = std::filesystem::file_size(partial_output_, error);
        std::uintmax_t output_size = 0;
        if (success && !partial_output_.empty() && std::filesystem::exists(partial_output_, error) &&
            partial_size > 0) {
            success = MoveFileExW(partial_output_.c_str(), final_output_.c_str(),
                                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        } else success = false;
        if (success && std::filesystem::exists(final_output_, error)) output_size = std::filesystem::file_size(final_output_, error);
        const double actual_fps = elapsed.count() > 0
            ? static_cast<double>(input_frame_count_) * 1000.0 / static_cast<double>(elapsed.count()) : 0.0;
        std::wostringstream summary;
        summary << L"\r\n[MMD2FFMPEG] Output summary\r\n"
                << L"Finished: " << format_local_time() << L"\r\n"
                << L"Input frames: " << input_frame_count_ << L"\r\n"
                << L"Actual input FPS: " << std::fixed << std::setprecision(3) << actual_fps << L"\r\n"
                << L"Elapsed: " << elapsed.count() << L" ms\r\n"
                << L"FFmpeg exit code: " << exit_code << L"\r\n"
                << L"Result: " << (success ? L"success" : L"failed") << L"\r\n";
        if (success) summary << L"Output size: " << output_size << L" bytes\r\n";
        else summary << L"Partial output size: " << partial_size << L" bytes\r\n";
        if (!success && !partial_output_.empty()) {
            const bool removed = std::filesystem::remove(partial_output_, error) ||
                                 !std::filesystem::exists(partial_output_, error);
            summary << L"Partial output cleanup: " << (removed ? L"success" : L"failed") << L"\r\n";
        }
        write_log_line(log_file_, summary.str());
        close_handle(log_file_);
        if (success && !avi_output_.empty()) {
            const auto cleanup = local_data_dir() / L"mmd2ffmpeg_cleanup.exe";
            const auto ffmpeg = resolve_executable(settings_.ffmpeg);
            if (std::filesystem::exists(cleanup, error) && !ffmpeg.empty()) {
                std::wstring command = L"\"" + cleanup.wstring() + L"\" \"" + avi_output_.wstring() + L"\" \"" +
                    final_output_.wstring() + L"\" \"" + ffmpeg.wstring() + L"\" " + settings_.audio_format + L" " +
                    settings_.audio_sample_rate + L" " + settings_.audio_bit_depth + L" \"" + log_path_.wstring() + L"\"";
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
    std::uint64_t input_frame_count_ = 0;
    std::chrono::steady_clock::time_point encoding_started_at_{};
    REFERENCE_TIME timestamp_ = 0, duration_ = 0;
    std::vector<BYTE> flipped_;
};

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
        settings_ = load_settings();
        window_ = CreateDialogParamW(module_instance(), MAKEINTRESOURCEW(IDD_ENCODER_SETTINGS), parent,
                                     dialog_proc, reinterpret_cast<LPARAM>(this));
        if (window_) MoveWindow(window_, rect->left, rect->top, rect->right - rect->left,
                                rect->bottom - rect->top, TRUE);
        return window_ ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
    HRESULT STDMETHODCALLTYPE Deactivate() override {
        if (window_) { DestroyWindow(window_); window_ = nullptr; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPageInfo(PROPPAGEINFO* info) override {
        if (!info) return E_POINTER;
        ZeroMemory(info, sizeof(*info)); info->cb = sizeof(*info);
        const Settings title_settings = load_settings();
        const wchar_t* title = ui_strings(ui_language(title_settings.language)).title;
        const auto bytes = (wcslen(title) + 1) * sizeof(wchar_t);
        info->pszTitle = static_cast<LPOLESTR>(CoTaskMemAlloc(bytes));
        if (!info->pszTitle) return E_OUTOFMEMORY;
        CopyMemory(info->pszTitle, title, bytes);
        info->size = measure_dialog_size();
        if (info->size.cx <= 0 || info->size.cy <= 0) return E_FAIL;
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
        candidate.audio_format = combo_index(ID_AUDIO_FORMAT) == 0 ? L"flac" : combo_index(ID_AUDIO_FORMAT) == 1 ? L"wav" : L"none";
        candidate.audio_sample_rate = combo_index(ID_AUDIO_RATE) == 1 ? L"hires" : L"original";
        candidate.audio_bit_depth = combo_index(ID_AUDIO_DEPTH) == 1 ? L"24" : L"original";
        const auto signature = command_test_signature(candidate);
        if (!current_command_tested_ || tested_signature_ != signature) {
            const auto& text = current_text();
            MessageBoxW(window_, text.required_message, text.required_title, MB_OK | MB_ICONWARNING);
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
    static SIZE measure_dialog_size() {
        HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                      module_instance(), nullptr);
        HWND dialog = parent ? CreateDialogParamW(module_instance(), MAKEINTRESOURCEW(IDD_ENCODER_SETTINGS), parent,
                                                   dialog_proc, 0) : nullptr;
        SIZE size{};
        if (dialog) {
            RECT client{};
            GetClientRect(dialog, &client);
            size = {static_cast<LONG>(client.right), static_cast<LONG>(client.bottom)};
            DestroyWindow(dialog);
        }
        if (parent) DestroyWindow(parent);
        return size;
    }
    void add_combo(int id, std::initializer_list<const wchar_t*> values, int selected) {
        HWND combo = GetDlgItem(window_, id);
        for (const auto* value : values) ComboBox_AddString(combo, value);
        ComboBox_SetCurSel(combo, selected);
    }
    void create_controls() {
        updating_command_ = true;
        add_combo(ID_LANGUAGE, {L"系統預設", L"繁體中文", L"簡體中文", L"日本語", L"English"}, language_index(settings_.language));
        add_combo(ID_BACKEND, {L"CPU (software)", L"NVIDIA NVENC", L"Intel Quick Sync", L"AMD AMF"}, settings_.backend == L"cpu" ? 0 : settings_.backend == L"qsv" ? 2 : settings_.backend == L"amf" ? 3 : 1);
        add_combo(ID_CODEC, {L"AVC (H.264)", L"HEVC (H.265)", L"AV1"}, settings_.codec == L"avc" ? 0 : settings_.codec == L"av1" ? 2 : 1);
        add_combo(ID_DEPTH, {L"8-bit", L"10-bit"}, settings_.bit_depth == 10 ? 1 : 0);
        add_combo(ID_PRESET, {L"P1", L"P2", L"P3", L"P4", L"P5", L"P6", L"P7"}, settings_.preset - 1);
        add_combo(ID_RATE, {L"CQ (constant quality)", L"Constant QP", L"VBR target bitrate"}, settings_.rate_control == L"crf" ? 0 : settings_.rate_control == L"vbr" ? 2 : 1);
        SetWindowTextW(GetDlgItem(window_, ID_QP), std::to_wstring(settings_.qp).c_str());
        SetWindowTextW(GetDlgItem(window_, ID_BITRATE), std::to_wstring(settings_.bitrate_kbps).c_str());
        SetWindowTextW(GetDlgItem(window_, ID_COMMAND_PREFIX), command_prefix(settings_).c_str());
        SetWindowTextW(GetDlgItem(window_, ID_COMMAND), (settings_.video_args.empty() ? encoding_arguments(settings_) : settings_.video_args).c_str());
        SetWindowTextW(GetDlgItem(window_, ID_COMMAND_SUFFIX), command_suffix(settings_).c_str());
        rebuild_backend_options();
        update_controls();
        updating_command_ = false;
        apply_language();
        create_tab_controls();
        capture_child_positions();
        update_vertical_scroll();
    }
    void create_tab_controls() {
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_TAB_CLASSES};
        InitCommonControlsEx(&common);
        const HFONT dialog_font = reinterpret_cast<HFONT>(SendMessageW(GetDlgItem(window_, ID_LANGUAGE), WM_GETFONT, 0, 0));
        const auto set_dialog_font = [&](HWND control) {
            if (dialog_font && control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(dialog_font), TRUE);
        };
        tab_ = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                               8, 4, 244, 20, window_, reinterpret_cast<HMENU>(ID_TAB), module_instance(), nullptr);
        set_dialog_font(tab_);
        TCITEMW item{}; item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t*>(L"影片"); TabCtrl_InsertItem(tab_, 0, &item);
        item.pszText = const_cast<wchar_t*>(L"音訊"); TabCtrl_InsertItem(tab_, 1, &item);
        item.pszText = const_cast<wchar_t*>(L"設定"); TabCtrl_InsertItem(tab_, 2, &item);
        TabCtrl_SetCurSel(tab_, 0);
        const std::array<int, 24> video_ids{ID_LANGUAGE, ID_BACKEND, ID_CODEC, ID_DEPTH, ID_PRESET, ID_RATE, ID_QP, ID_BITRATE,
                                            ID_COMMAND, ID_REFRESH, ID_STATUS, ID_OPEN_LOG, ID_COMMAND_PREFIX, ID_COMMAND_SUFFIX,
                                            ID_TEST_REQUIREMENT, ID_LABEL_LANGUAGE, ID_LABEL_BACKEND, ID_LABEL_CODEC, ID_LABEL_DEPTH,
                                            ID_LABEL_PRESET, ID_LABEL_RATE, ID_LABEL_QP, ID_LABEL_BITRATE, ID_COMMAND_HEADING};
        for (const int id : video_ids) {
            HWND control = GetDlgItem(window_, id); RECT bounds{};
            GetWindowRect(control, &bounds); MapWindowPoints(HWND_DESKTOP, window_, reinterpret_cast<POINT*>(&bounds), 2);
            SetWindowPos(control, nullptr, bounds.left, bounds.top + 24, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        auto make_label = [&](int id, const wchar_t* text, int y) {
            return CreateWindowExW(0, L"STATIC", text, WS_CHILD, 16, y, 100, 18, window_, reinterpret_cast<HMENU>(id), module_instance(), nullptr);
        };
        auto make_combo = [&](int id, int y) {
            return CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VSCROLL | CBS_DROPDOWNLIST | WS_TABSTOP,
                                   120, y, 128, 120, window_, reinterpret_cast<HMENU>(id), module_instance(), nullptr);
        };
        audio_labels_ = {make_label(ID_LABEL_AUDIO_FORMAT, L"音訊格式", 38), make_label(ID_LABEL_AUDIO_RATE, L"取樣率", 64), make_label(ID_LABEL_AUDIO_DEPTH, L"位元深度", 90)};
        audio_controls_ = {make_combo(ID_AUDIO_FORMAT, 36), make_combo(ID_AUDIO_RATE, 62), make_combo(ID_AUDIO_DEPTH, 88)};
        for (HWND control : audio_labels_) set_dialog_font(control);
        for (HWND control : audio_controls_) set_dialog_font(control);
        add_combo(ID_AUDIO_FORMAT, {L"FLAC", L"WAV", L"None"}, settings_.audio_format == L"flac" ? 0 : settings_.audio_format == L"wav" ? 1 : 2);
        add_combo(ID_AUDIO_RATE, {L"原始", L"大於等於48KHz"}, settings_.audio_sample_rate == L"hires" ? 1 : 0);
        add_combo(ID_AUDIO_DEPTH, {L"原始", L"24bit"}, settings_.audio_bit_depth == L"24" ? 1 : 0);
        settings_info_ = CreateWindowExW(0, L"STATIC", L"MMD2FFMPEG\r\nVersion: 0.2.0\r\nAuthor: XPRAMT\r\nGitHub: github.com/XPRAMT/MMD2FFMPEG",
                                          WS_CHILD | SS_NOTIFY, 16, 38, 228, 90, window_, reinterpret_cast<HMENU>(ID_SETTINGS_INFO), module_instance(), nullptr);
        set_dialog_font(settings_info_);
        switch_tab(0);
    }
    void switch_tab(int page) {
        const std::array<int, 18> video{ID_LANGUAGE, ID_BACKEND, ID_CODEC, ID_DEPTH, ID_PRESET, ID_RATE, ID_QP, ID_BITRATE,
                                        ID_COMMAND, ID_REFRESH, ID_STATUS, ID_OPEN_LOG, ID_COMMAND_PREFIX, ID_COMMAND_SUFFIX,
                                        ID_TEST_REQUIREMENT, ID_LABEL_LANGUAGE, ID_LABEL_BACKEND, ID_LABEL_CODEC};
        const std::array<int, 6> video_more{ID_LABEL_DEPTH, ID_LABEL_PRESET, ID_LABEL_RATE, ID_LABEL_QP, ID_LABEL_BITRATE, ID_COMMAND_HEADING};
        for (const int id : video) ShowWindow(GetDlgItem(window_, id), page == 0 ? SW_SHOW : SW_HIDE);
        for (const int id : video_more) ShowWindow(GetDlgItem(window_, id), page == 0 ? SW_SHOW : SW_HIDE);
        for (HWND control : audio_labels_) ShowWindow(control, page == 1 ? SW_SHOW : SW_HIDE);
        for (HWND control : audio_controls_) ShowWindow(control, page == 1 ? SW_SHOW : SW_HIDE);
        ShowWindow(settings_info_, page == 2 ? SW_SHOW : SW_HIDE);
        active_tab_ = page;
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
        const auto& options = ui_options(ui_language(settings_.language));
        const std::wstring fastest_13 = std::wstring(L"13 (") + options.fastest + L")";
        const std::wstring best_4 = std::wstring(L"4 (") + options.best_quality + L")";
        const std::wstring fastest_p1 = std::wstring(L"P1 (") + options.fastest + L")";
        const std::wstring best_p7 = std::wstring(L"P7 (") + options.best_quality + L")";
        const std::wstring crf = std::wstring(L"CRF (") + options.constant_quality + L")";
        const std::wstring cq = std::wstring(L"CQ (") + options.constant_quality + L")";
        const std::wstring icq = std::wstring(L"ICQ / ") + options.global_quality;
        const std::wstring qvbr = std::wstring(L"QVBR ") + options.quality;
        updating_command_ = true;
        if (backend == 0) {
            if (combo_index(ID_CODEC) == 2)
                reset_combo(ID_PRESET, {fastest_13.c_str(), L"11", L"9", L"8", L"7", L"6", best_4.c_str()}, old_level - 1);
            else
                reset_combo(ID_PRESET, {L"ultrafast", L"superfast", L"veryfast", L"faster", L"fast", L"medium", L"slow"}, old_level - 1);
            reset_combo(ID_RATE, {crf.c_str(), options.constant_qp, options.target_bitrate}, settings_.rate_control == L"crf" ? 0 : settings_.rate_control == L"vbr" ? 2 : 1);
        } else if (backend == 1) {
            reset_combo(ID_PRESET, {fastest_p1.c_str(), L"P2", L"P3", L"P4", L"P5", L"P6", best_p7.c_str()}, old_level - 1);
            reset_combo(ID_RATE, {cq.c_str(), options.constant_qp, options.target_bitrate}, settings_.rate_control == L"crf" ? 0 : settings_.rate_control == L"vbr" ? 2 : 1);
        } else if (backend == 2) {
            reset_combo(ID_PRESET, {L"veryfast", L"faster", L"fast", L"medium", L"slow", L"slower", L"veryslow"}, old_level - 1);
            reset_combo(ID_RATE, {icq.c_str(), options.constant_qp, options.target_bitrate}, settings_.rate_control == L"crf" ? 0 : settings_.rate_control == L"vbr" ? 2 : 1);
        } else {
            const int amf_selected = old_level <= 2 ? 0 : old_level <= 5 ? 1 : 2;
            reset_combo(ID_PRESET, {options.speed, options.balanced, options.quality}, amf_selected);
            reset_combo(ID_RATE, {qvbr.c_str(), options.constant_qp, options.target_bitrate}, settings_.rate_control == L"crf" ? 0 : settings_.rate_control == L"vbr" ? 2 : 1);
        }
        updating_command_ = false;
    }
    void start_probe(bool force = false) {
        if (probe_running_) return;
        if (probe_thread_.joinable()) probe_thread_.join();
        sync_structured_settings();
        Settings candidate = settings_;
        candidate.video_args = edit_text(ID_COMMAND);
        ProbeResult cached{};
        if (!force && load_cached_probe(candidate, cached)) {
            auto* result = new ProbeResult(std::move(cached));
            PostMessageW(window_, WM_APP + 42, 0, reinterpret_cast<LPARAM>(result));
            return;
        }
        SetWindowTextW(GetDlgItem(window_, ID_STATUS), current_text().testing);
        EnableWindow(GetDlgItem(window_, ID_REFRESH), FALSE);
        probe_running_ = true;
        const HWND target = window_;
        probe_thread_ = std::thread([this, target, candidate]() {
            std::wstring message;
            const bool success = test_encoder(candidate, message);
            auto* result = new ProbeResult{success, success ? L"Available" : message, command_test_signature(candidate)};
            save_cached_probe(candidate, *result);
            if (alive_ && IsWindow(target)) PostMessageW(target, WM_APP + 42, 0, reinterpret_cast<LPARAM>(result));
            else delete result;
        });
    }
    void open_log_folder() {
        const auto directory = logs_directory();
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        const auto& text = current_text();
        const auto result = error ? 0 : reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32)
            MessageBoxW(window_, text.open_log_failed_message, text.log_title, MB_OK | MB_ICONERROR);
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
        settings_.audio_format = combo_index(ID_AUDIO_FORMAT) == 0 ? L"flac" : combo_index(ID_AUDIO_FORMAT) == 1 ? L"wav" : L"none";
        settings_.audio_sample_rate = combo_index(ID_AUDIO_RATE) == 1 ? L"hires" : L"original";
        settings_.audio_bit_depth = combo_index(ID_AUDIO_DEPTH) == 1 ? L"24" : L"original";
    }
    void update_command_display() {
        SetWindowTextW(GetDlgItem(window_, ID_COMMAND_PREFIX), command_prefix(settings_).c_str());
        SetWindowTextW(GetDlgItem(window_, ID_COMMAND_SUFFIX), command_suffix(settings_).c_str());
    }
    struct ChildPosition {
        HWND window;
        RECT rectangle;
    };
    void capture_child_positions() {
        child_positions_.clear();
        for (HWND child = GetWindow(window_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            RECT rectangle{};
            GetWindowRect(child, &rectangle);
            MapWindowPoints(HWND_DESKTOP, window_, reinterpret_cast<POINT*>(&rectangle), 2);
            child_positions_.push_back({child, rectangle});
        }
        RECT client{};
        GetClientRect(window_, &client);
        content_height_ = static_cast<int>(client.bottom);
    }
    void layout_scrolled_children() {
        RECT client{};
        GetClientRect(window_, &client);
        for (const auto& child : child_positions_) {
            const int left = static_cast<int>(child.rectangle.left);
            const int top = static_cast<int>(child.rectangle.top);
            const int width = std::min(static_cast<int>(child.rectangle.right - child.rectangle.left),
                                       std::max(1, static_cast<int>(client.right) - left));
            SetWindowPos(child.window, nullptr, left, top - scroll_offset_,
                         width, static_cast<int>(child.rectangle.bottom - child.rectangle.top),
                         SWP_NOACTIVATE | SWP_NOZORDER);
        }
    }
    void update_vertical_scroll() {
        if (child_positions_.empty()) return;
        RECT client{};
        GetClientRect(window_, &client);
        const int client_height = static_cast<int>(client.bottom);
        const int maximum = std::max(0, content_height_ - client_height);
        scroll_offset_ = std::clamp(scroll_offset_, 0, maximum);
        SCROLLINFO scroll_info{sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, content_height_ - 1),
                               static_cast<UINT>(std::max(0, client_height)), scroll_offset_, 0};
        SetScrollInfo(window_, SB_VERT, &scroll_info, TRUE);
        ShowScrollBar(window_, SB_VERT, maximum > 0);
        layout_scrolled_children();
    }
    void scroll_to(int requested_offset) {
        RECT client{};
        GetClientRect(window_, &client);
        const int client_height = static_cast<int>(client.bottom);
        const int maximum = std::max(0, content_height_ - client_height);
        scroll_offset_ = std::clamp(requested_offset, 0, maximum);
        SCROLLINFO scroll_info{sizeof(SCROLLINFO), SIF_POS, 0, 0, 0, scroll_offset_, 0};
        SetScrollInfo(window_, SB_VERT, &scroll_info, TRUE);
        layout_scrolled_children();
    }
    void scroll_vertical(WPARAM wparam) {
        RECT client{};
        GetClientRect(window_, &client);
        int requested = scroll_offset_;
        switch (LOWORD(wparam)) {
        case SB_TOP: requested = 0; break;
        case SB_BOTTOM: requested = content_height_; break;
        case SB_LINEUP: requested -= 40; break;
        case SB_LINEDOWN: requested += 40; break;
        case SB_PAGEUP: requested -= std::max(40, static_cast<int>(client.bottom) * 3 / 4); break;
        case SB_PAGEDOWN: requested += std::max(40, static_cast<int>(client.bottom) * 3 / 4); break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK: {
            SCROLLINFO scroll_info{sizeof(SCROLLINFO), SIF_TRACKPOS};
            GetScrollInfo(window_, SB_VERT, &scroll_info);
            requested = scroll_info.nTrackPos;
            break;
        }
        default: return;
        }
        scroll_to(requested);
    }
    const UiStrings& current_text() const { return ui_strings(ui_language(settings_.language)); }
    void apply_language() {
        const auto& text = current_text();
        const auto& options = ui_options(ui_language(settings_.language));
        const int backend = combo_index(ID_BACKEND);
        reset_combo(ID_BACKEND, {(std::wstring(L"CPU (") + options.software + L")").c_str(),
                                 L"NVIDIA NVENC", L"Intel Quick Sync", L"AMD AMF"}, backend);
        rebuild_backend_options();
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_LANGUAGE), text.language);
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_BACKEND), text.encoder);
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_CODEC), text.codec);
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_DEPTH), text.bit_depth);
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_PRESET), text.preset);
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_RATE), text.rate_control);
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_QP), text.quality);
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_BITRATE), text.bitrate);
        SetWindowTextW(GetDlgItem(window_, ID_COMMAND_HEADING), text.command_heading);
        SetWindowTextW(GetDlgItem(window_, ID_TEST_REQUIREMENT), text.test_required);
        SetWindowTextW(GetDlgItem(window_, ID_REFRESH), text.test_button);
        SetWindowTextW(GetDlgItem(window_, ID_OPEN_LOG), text.open_log_button);
        if (tab_) {
            const UiLanguage current_language = ui_language(settings_.language);
            const wchar_t* names[] = {current_language == UiLanguage::TraditionalChinese ? L"影片" : L"Video",
                                      current_language == UiLanguage::TraditionalChinese ? L"音訊" : L"Audio",
                                      current_language == UiLanguage::TraditionalChinese ? L"設定" : L"Settings"};
            for (int index = 0; index < 3; ++index) { TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = const_cast<wchar_t*>(names[index]); TabCtrl_SetItem(tab_, index, &item); }
        }
        const wchar_t* status = probe_running_ ? text.testing : current_command_tested_ ? text.test_passed : text.not_tested;
        SetWindowTextW(GetDlgItem(window_, ID_STATUS), status);
    }
    void change_language() {
        settings_.language = language_key(combo_index(ID_LANGUAGE));
        apply_language();
        Settings persisted = load_settings();
        persisted.language = settings_.language;
        save_settings(persisted);
    }
    void changed(int id) {
        dirty_ = true;
        if (id != ID_AUDIO_FORMAT && id != ID_AUDIO_RATE && id != ID_AUDIO_DEPTH) {
            current_command_tested_ = false;
            tested_signature_.clear();
        }
        if (id == ID_BACKEND || id == ID_CODEC) rebuild_backend_options();
        update_controls();
        if (id != ID_COMMAND) {
            sync_structured_settings();
            updating_command_ = true;
            SetWindowTextW(GetDlgItem(window_, ID_COMMAND), encoding_arguments(settings_).c_str());
            update_command_display();
            updating_command_ = false;
        }
        SetWindowTextW(GetDlgItem(window_, ID_STATUS), current_text().not_tested);
        if (site_) site_->OnStatusChange(PROPPAGESTATUS_DIRTY);
    }
    static INT_PTR CALLBACK dialog_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<SettingsPropertyPage*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_INITDIALOG) {
            self = reinterpret_cast<SettingsPropertyPage*>(lparam);
            if (!self) return TRUE;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->window_ = window;
            self->create_controls();
            return TRUE;
        }
        else if (message == WM_APP + 42 && self) {
            auto* result = reinterpret_cast<ProbeResult*>(lparam);
            self->probe_running_ = false;
            self->sync_structured_settings();
            Settings current = self->settings_;
            current.video_args = self->edit_text(ID_COMMAND);
            self->current_command_tested_ = result->success && result->signature == command_test_signature(current);
            self->tested_signature_ = self->current_command_tested_ ? result->signature : L"";
            std::wstring status;
            const auto& text = self->current_text();
            if (self->current_command_tested_) status = text.test_passed;
            else if (result->signature != command_test_signature(current)) status = text.settings_changed;
            else status = std::wstring(text.test_failed) + result->message;
            if (status.size() > 180) status.resize(180);
            SetWindowTextW(GetDlgItem(window, ID_STATUS), status.c_str());
            EnableWindow(GetDlgItem(window, ID_REFRESH), TRUE);
            delete result;
            return TRUE;
        }
        else if (message == WM_SIZE && self) {
            self->update_vertical_scroll();
            return TRUE;
        }
        else if (message == WM_VSCROLL && self && lparam == 0) {
            self->scroll_vertical(wparam);
            return TRUE;
        }
        else if (message == WM_MOUSEWHEEL && self) {
            self->scroll_to(self->scroll_offset_ - GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA * 80);
            return TRUE;
        }
        else if (message == WM_NOTIFY && self && reinterpret_cast<NMHDR*>(lparam)->idFrom == ID_TAB &&
                 reinterpret_cast<NMHDR*>(lparam)->code == TCN_SELCHANGE) {
            self->switch_tab(TabCtrl_GetCurSel(self->tab_));
            return TRUE;
        }
        else if (message == WM_COMMAND && self && LOWORD(wparam) == ID_REFRESH && HIWORD(wparam) == BN_CLICKED) {
            self->start_probe(true);
            return TRUE;
        }
        else if (message == WM_COMMAND && self && LOWORD(wparam) == ID_OPEN_LOG && HIWORD(wparam) == BN_CLICKED) {
            self->open_log_folder();
            return TRUE;
        }
        else if (message == WM_COMMAND && self && LOWORD(wparam) == ID_SETTINGS_INFO && HIWORD(wparam) == STN_CLICKED) {
            ShellExecuteW(window, L"open", L"https://github.com/XPRAMT/MMD2FFMPEG", nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        }
        else if (message == WM_COMMAND && self && !self->updating_command_ &&
                 LOWORD(wparam) == ID_LANGUAGE && HIWORD(wparam) == CBN_SELCHANGE) {
            self->change_language();
            return TRUE;
        }
        else if (message == WM_COMMAND && self && !self->updating_command_ &&
                 (HIWORD(wparam) == CBN_SELCHANGE || HIWORD(wparam) == EN_CHANGE || HIWORD(wparam) == BN_CLICKED)) {
            self->changed(LOWORD(wparam));
            return TRUE;
        }
        return FALSE;
    }

    std::atomic<ULONG> references_{1};
    IPropertyPageSite* site_ = nullptr;
    HWND window_ = nullptr;
    HWND tab_ = nullptr;
    HWND settings_info_ = nullptr;
    std::array<HWND, 3> audio_labels_{};
    std::array<HWND, 3> audio_controls_{};
    int active_tab_ = 0;
    Settings settings_{};
    bool dirty_ = false;
    bool updating_command_ = false;
    std::atomic<bool> alive_{true};
    bool probe_running_ = false;
    bool current_command_tested_ = false;
    std::wstring tested_signature_;
    std::thread probe_thread_;
    std::vector<ChildPosition> child_positions_;
    int content_height_ = 0;
    int scroll_offset_ = 0;
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
