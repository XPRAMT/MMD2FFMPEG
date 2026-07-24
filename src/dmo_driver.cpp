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
#include <string_view>
#include <thread>
#include <utility>
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
    std::wstring chroma = L"420";
    std::wstring alpha_mode = L"none";
    std::wstring mask_output = L"stacked";
    std::wstring color_space = L"bt709";
    std::wstring color_range = L"tv";
    int preset = 6;
    std::wstring rate_control = L"crf";
    int qp = 18;
    int bitrate_kbps = 20000;
    std::wstring frame_structure_mode = L"auto";
    int gop = 120;
    int b_frames = 3;
    std::wstring cpu_threads = L"auto";
    std::wstring audio_format = L"flac";
    std::wstring audio_sample_rate = L"original";
    std::wstring audio_bit_depth = L"original";
    bool vsr_enabled = false;
    double vsr_scale = 2.0;
    int vsr_quality = 2;
    std::wstring language = L"system";
    std::wstring command_template;
};

struct CodecCapability {
    const wchar_t* key;
    const wchar_t* label;
    bool cpu_only;
    bool supports_10bit;
    bool forces_10bit;
    bool supports_rgba;
    bool rgba_supports_10bit;
    bool supports_rate_control;
    bool forces_444_for_rgba;
    bool supports_b_frames;
};

constexpr std::array<CodecCapability, 5> kCodecCapabilities{{
    {L"avc", L"AVC (H.264)", false, false, false, false, false, true, false, true},
    {L"hevc", L"HEVC (H.265)", false, true, false, false, false, true, false, true},
    {L"av1", L"AV1", false, true, false, false, false, true, false, true},
    {L"vp9", L"VP9", true, true, false, true, false, true, false, false},
    {L"prores", L"ProRes", true, true, true, true, true, false, true, false},
}};

const CodecCapability& codec_capability(std::wstring_view key) {
    for (const auto& capability : kCodecCapabilities) {
        if (key == capability.key) return capability;
    }
    return kCodecCapabilities[1];
}

const CodecCapability& codec_capability_from_index(int index) {
    return kCodecCapabilities[std::clamp(index, 0, static_cast<int>(kCodecCapabilities.size()) - 1)];
}

int codec_index(std::wstring_view key) {
    for (int index = 0; index < static_cast<int>(kCodecCapabilities.size()); ++index) {
        if (key == kCodecCapabilities[index].key) return index;
    }
    return 1;
}

void normalize_codec_settings(Settings& settings) {
    (void)settings;
}

enum class UiLanguage { TraditionalChinese, SimplifiedChinese, Japanese, English };

struct UiStrings {
    const wchar_t* title;
    const wchar_t* language;
    const wchar_t* cpu_threads;
    const wchar_t* encoder;
    const wchar_t* codec;
    const wchar_t* bit_depth;
    const wchar_t* preset;
    const wchar_t* rate_control;
    const wchar_t* quality;
    const wchar_t* bitrate;
    const wchar_t* alpha;
    const wchar_t* mask_output;
    const wchar_t* chroma;
    const wchar_t* color_space;
    const wchar_t* color_range;
    const wchar_t* gop;
    const wchar_t* b_frames;
    const wchar_t* frame_structure_mode;
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
    const wchar_t* automatic;
    const wchar_t* manual;
    const wchar_t* all_cores;
    const wchar_t* all_cores_minus_one;
    const wchar_t* all_cores_minus_two;
};

struct UiTooltips {
    const wchar_t* language;
    const wchar_t* cpu_threads;
    const wchar_t* backend;
    const wchar_t* codec;
    const wchar_t* bit_depth;
    const wchar_t* preset;
    const wchar_t* rate_control;
    const wchar_t* quality;
    const wchar_t* bitrate;
    const wchar_t* gop;
    const wchar_t* b_frames;
    const wchar_t* frame_structure_mode;
    const wchar_t* alpha;
    const wchar_t* mask_output;
    const wchar_t* chroma;
    const wchar_t* color_space;
    const wchar_t* color_range;
    const wchar_t* audio_format;
    const wchar_t* audio_rate;
    const wchar_t* command;
    const wchar_t* test_encoder;
    const wchar_t* open_log;
    const wchar_t* github;
};

struct TabUiStrings {
    const wchar_t* video;
    const wchar_t* audio;
    const wchar_t* settings;
    const wchar_t* audio_format;
    const wchar_t* sample_rate_depth;
    const wchar_t* audio_intro;
    const wchar_t* original;
    const wchar_t* hi_res;
    const wchar_t* hi_res_help;
    const wchar_t* version;
    const wchar_t* author;
    const wchar_t* github;
    const wchar_t* encoding;
    const wchar_t* color;
    const wchar_t* frame_structure;
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

const TabUiStrings& tab_ui_strings(UiLanguage language) {
    static constexpr TabUiStrings traditional{L"影片", L"音訊", L"設定", L"音訊格式", L"取樣率/位元深度", L"影片編碼完成後，自動讀取AVI內含音訊，合併進MKV檔案，花費時間取決於硬碟速度。注意：MMD必須從第0幀開始輸出，AVI中才會包含音訊檔。", L"原始", L"Hi-Res", L"如果原始取樣率小於48KHz，Hi-Res模式以原始取樣率2倍及24bit編碼，以通過 bilibili Hi-Res 判定。", L"版本", L"作者", L"GitHub", L"編碼", L"色彩", L"幀結構"};
    static constexpr TabUiStrings simplified{L"视频", L"音频", L"设置", L"音频格式", L"采样率/位深度", L"视频编码完成后，自动读取AVI内含音频，合并进MKV文件，耗时取决于硬盘速度。注意：MMD必须从第0帧开始输出，AVI中才会包含音频文件。", L"原始", L"Hi-Res", L"如果原始采样率小于48KHz，Hi-Res模式以原始采样率2倍及24bit编码，以通过 bilibili Hi-Res 判定。", L"版本", L"作者", L"GitHub", L"编码", L"色彩", L"帧结构"};
    static constexpr TabUiStrings japanese{L"ビデオ", L"オーディオ", L"設定", L"音声形式", L"サンプルレート/ビット深度", L"動画エンコード完了後、AVI 内の音声を自動的に読み取り、MKV に結合します。所要時間はディスク速度に依存します。注意：AVI に音声を含めるには、MMD の出力開始フレームを 0 にしてください。", L"オリジナル", L"Hi-Res", L"元のサンプルレートが48kHz未満の場合、Hi-Res モードでは元の2倍かつ24bitで再エンコードし、bilibili Hi-Res 判定に対応します。", L"バージョン", L"作者", L"GitHub", L"エンコード", L"カラー", L"フレーム構造"};
    static constexpr TabUiStrings english{L"Video", L"Audio", L"Settings", L"Audio format", L"Sample rate / bit depth", L"After video encoding, audio embedded in the AVI is automatically read and merged into the MKV. Time required depends on disk speed. Note: MMD must export from frame 0 for the AVI to contain audio.", L"Original", L"Hi-Res", L"If the source sample rate is below 48 kHz, Hi-Res mode encodes at twice the original sample rate and 24-bit for bilibili Hi-Res detection.", L"Version", L"Author", L"GitHub", L"Encoding", L"Color", L"Frame structure"};
    switch (language) {
    case UiLanguage::TraditionalChinese: return traditional;
    case UiLanguage::SimplifiedChinese: return simplified;
    case UiLanguage::Japanese: return japanese;
    default: return english;
    }
}

const wchar_t* super_resolution_label(UiLanguage language) {
    switch (language) {
    case UiLanguage::TraditionalChinese: return L"超分";
    case UiLanguage::SimplifiedChinese: return L"超分";
    case UiLanguage::Japanese: return L"超解像";
    default: return L"Super resolution";
    }
}

const std::array<const wchar_t*, 3> vsr_labels(UiLanguage language) {
    switch (language) {
    case UiLanguage::TraditionalChinese: return {L"RTX VSR", L"放大倍率", L"VSR 處理等級"};
    case UiLanguage::SimplifiedChinese: return {L"RTX VSR", L"放大倍率", L"VSR 处理等级"};
    case UiLanguage::Japanese: return {L"RTX VSR", L"拡大倍率", L"VSR 品質レベル"};
    default: return {L"RTX VSR", L"Scale factor", L"VSR quality level"};
    }
}

const std::array<const wchar_t*, 3> vsr_tooltips(UiLanguage language) {
    switch (language) {
    case UiLanguage::TraditionalChinese:
        return {L"啟用 NVIDIA RTX Video Super Resolution。啟用後使用 FFmpeg 將 MMD RGB 封裝為 RGBA/NUT，再由 NVEncC --avsw 接收並執行 NGX VSR。",
                L"設定輸出相對於 MMD 原始尺寸的放大倍率，可輸入 1.00 到 4.00；預設為 2.00。",
                L"設定 NVEncC ngx-vsr 的處理等級 1 到 4。等級越高通常需要更多 GPU 運算。"};
    case UiLanguage::SimplifiedChinese:
        return {L"启用 NVIDIA RTX Video Super Resolution。启用后使用 FFmpeg 将 MMD RGB 封装为 RGBA/NUT，再由 NVEncC --avsw 接收并执行 NGX VSR。",
                L"设置输出相对于 MMD 原始尺寸的放大倍率，可输入 1.00 到 4.00；默认为 2.00。",
                L"设置 NVEncC ngx-vsr 的处理等级 1 到 4。等级越高通常需要更多 GPU 运算。"};
    case UiLanguage::Japanese:
        return {L"NVIDIA RTX Video Super Resolution を有効にします。FFmpeg が MMD RGB を RGBA/NUT に封入し、NVEncC --avsw が NGX VSR を実行します。",
                L"MMD の元解像度に対する倍率を 1.00～4.00 で指定します。既定値は 2.00 です。",
                L"NVEncC ngx-vsr の品質レベルを 1～4 で指定します。高いレベルほど通常は GPU 負荷が増えます。"};
    default:
        return {L"Enable NVIDIA RTX Video Super Resolution. FFmpeg packages MMD RGB as RGBA/NUT and NVEncC --avsw applies NGX VSR.",
                L"Set the output scale relative to the original MMD size from 1.00 to 4.00. The default is 2.00.",
                L"Set NVEncC ngx-vsr quality from 1 to 4. Higher levels usually require more GPU processing."};
    }
}

const UiStrings& ui_strings(UiLanguage language) {
    static constexpr UiStrings traditional{
        L"MMD2FFMPEG 編碼器設定", L"語言", L"編碼核心數", L"編碼器", L"編碼格式", L"位元深度", L"編碼預設",
        L"碼率控制", L"品質 / QP", L"位元率 (kbps)", L"Alpha", L"遮罩輸出", L"色度取樣", L"色彩空間", L"輸出範圍", L"GOP / I 幀間隔", L"B 幀間隔", L"幀結構", L"完整 FFmpeg 指令（中間區段可編輯）",
        L"編碼器狀態：尚未測試", L"編碼器狀態：測試中…", L"編碼器狀態：測試通過",
        L"編碼器狀態：設定已變更，請重新測試", L"編碼器狀態：測試失敗 - ",
        L"通過測試後才能儲存或套用。", L"測試編碼", L"開啟log",
        L"無法開啟編碼 log 資料夾。", L"MMD2FFMPEG log",
        L"請先測試目前的編碼指令。\n\n通過測試後才能儲存或套用設定。",
        L"需要測試 MMD2FFMPEG 編碼器"};
    static constexpr UiStrings simplified{
        L"MMD2FFMPEG 编码器设置", L"语言", L"编码核心数", L"编码器", L"编码格式", L"位深度", L"编码预设",
        L"码率控制", L"质量 / QP", L"比特率 (kbps)", L"Alpha", L"遮罩输出", L"色度采样", L"色彩空间", L"输出范围", L"GOP / I 帧间隔", L"B 帧间隔", L"帧结构", L"完整 FFmpeg 命令（中间部分可编辑）",
        L"编码器状态：尚未测试", L"编码器状态：测试中…", L"编码器状态：测试通过",
        L"编码器状态：设置已更改，请重新测试", L"编码器状态：测试失败 - ",
        L"测试通过后才能保存或应用。", L"测试编码", L"打开日志",
        L"无法打开编码日志文件夹。", L"MMD2FFMPEG 日志",
        L"请先测试当前的编码命令。\n\n测试通过后才能保存或应用设置。",
        L"需要测试 MMD2FFMPEG 编码器"};
    static constexpr UiStrings japanese{
        L"MMD2FFMPEG エンコーダー設定", L"言語", L"エンコード CPU スレッド", L"エンコーダー", L"コーデック", L"ビット深度", L"プリセット",
        L"レート制御", L"品質 / QP", L"ビットレート (kbps)", L"アルファ", L"マスク出力", L"クロマサンプリング", L"色空間", L"出力レンジ", L"GOP / I フレーム間隔", L"B フレーム間隔", L"フレーム構造", L"完全な FFmpeg コマンド（中央部分は編集可能）",
        L"エンコーダー状態：未テスト", L"エンコーダー状態：テスト中…", L"エンコーダー状態：テスト合格",
        L"エンコーダー状態：設定が変更されました。再テストしてください", L"エンコーダー状態：テスト失敗 - ",
        L"保存または適用する前にテストに合格する必要があります。", L"エンコーダーをテスト", L"ログを開く",
        L"エンコードログのフォルダーを開けません。", L"MMD2FFMPEG ログ",
        L"現在のエンコーダーコマンドを先にテストしてください。\n\nテストに合格するまで設定を保存または適用できません。",
        L"MMD2FFMPEG エンコーダーのテストが必要です"};
    static constexpr UiStrings english{
        L"MMD2FFMPEG Encoder Settings", L"Language", L"Encoding CPU threads", L"Encoder", L"Codec", L"Bit depth", L"Encoder preset",
        L"Rate control", L"Quality / QP", L"Bitrate (kbps)", L"Alpha", L"Mask output", L"Chroma sampling", L"Color space", L"Output range", L"GOP / I-frame interval", L"B-frame interval", L"Frame structure", L"Complete FFmpeg command (middle section is editable)",
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
                                              L"VBR 目標位元率", L"全域品質", L"速度", L"平衡", L"品質", L"自動", L"手動", L"全部", L"全部 - 1", L"全部 - 2"};
    static constexpr UiOptions simplified{L"软件", L"最快", L"最佳质量", L"恒定质量", L"恒定 QP",
                                             L"VBR 目标比特率", L"全局质量", L"速度", L"平衡", L"质量", L"自动", L"手动", L"全部", L"全部 - 1", L"全部 - 2"};
    static constexpr UiOptions japanese{L"ソフトウェア", L"最速", L"最高品質", L"固定品質", L"固定 QP",
                                          L"VBR 目標ビットレート", L"グローバル品質", L"速度", L"バランス", L"品質", L"自動", L"手動", L"すべて", L"すべて - 1", L"すべて - 2"};
    static constexpr UiOptions english{L"software", L"fastest", L"best quality", L"constant quality", L"Constant QP",
                                         L"VBR target bitrate", L"global quality", L"speed", L"balanced", L"quality", L"Automatic", L"Manual", L"All", L"All - 1", L"All - 2"};
    switch (language) {
    case UiLanguage::TraditionalChinese: return traditional;
    case UiLanguage::SimplifiedChinese: return simplified;
    case UiLanguage::Japanese: return japanese;
    default: return english;
    }
}

DWORD available_processor_count() {
    const DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (count > 0) return count;
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    return std::max<DWORD>(1, system_info.dwNumberOfProcessors);
}

int cpu_thread_mode_index(const std::wstring& value) {
    if (value == L"all") return 1;
    if (value == L"all-minus-1") return 2;
    if (value == L"all-minus-2") return 3;
    return 0;
}

const wchar_t* cpu_thread_mode_key(int index) {
    static constexpr const wchar_t* values[]{L"auto", L"all", L"all-minus-1", L"all-minus-2"};
    return values[std::clamp(index, 0, 3)];
}

std::array<std::wstring, 4> cpu_thread_options(UiLanguage language) {
    const auto& options = ui_options(language);
    const int all = static_cast<int>(available_processor_count());
    return {options.automatic,
            std::wstring(options.all_cores) + L" (" + std::to_wstring(all) + L")",
            std::wstring(options.all_cores_minus_one) + L" (" + std::to_wstring(std::max(1, all - 1)) + L")",
            std::wstring(options.all_cores_minus_two) + L" (" + std::to_wstring(std::max(1, all - 2)) + L")"};
}

int selected_cpu_threads(const Settings& settings) {
    if (settings.cpu_threads == L"auto") return 0;
    const int all = static_cast<int>(available_processor_count());
    if (settings.cpu_threads == L"all-minus-1") return std::max(1, all - 1);
    if (settings.cpu_threads == L"all-minus-2") return std::max(1, all - 2);
    return std::max(1, all);
}

const UiTooltips& ui_tooltips(UiLanguage language) {
    static constexpr UiTooltips traditional{
        L"選擇設定介面的顯示語言；系統預設會跟隨 Windows。",
        L"選擇 CPU 軟體編碼使用的執行緒數。自動不傳遞 -threads；全部、全部 - 1、全部 - 2 會依 Windows 偵測的可用邏輯處理器數計算。硬體編碼不使用這個設定。",
        L"選擇 FFmpeg 使用的軟體或硬體編碼器。VP9 與 ProRes 僅能使用 CPU。",
        L"選擇影片編碼格式。可用的位元深度、Alpha 與碼率控制會依格式自動限制。",
        L"選擇 8-bit 或 10-bit 輸出。部分編碼格式或 Alpha 模式會固定此選項。",
        L"調整編碼速度與壓縮效率。較快通常檔案較大，較慢通常壓縮較佳。",
        L"選擇固定品質、固定 QP 或目標位元率控制方式。",
        L"設定固定品質或 QP 的數值；數值意義會依目前的編碼器與控制方式而不同。",
        L"設定 VBR 目標位元率（kbps）。只有選擇 VBR 時會使用此值。",
        L"設定 GOP 的最大長度，也就是 I 幀之間最多相隔多少畫面。I 幀是可獨立解碼的基準畫面；實際場景切換可能提早插入 I 幀。對應 FFmpeg 的 -g。",
        L"設定每兩個 I/P 參考幀之間最多插入多少 B 幀。B 幀可同時參考前後 I/P 幀，通常能提高壓縮率但增加延遲；0 代表不使用 B 幀。對應 FFmpeg 的 -bf。",
        L"自動：不傳遞 GOP 或 B 幀參數，交由 FFmpeg 與編碼器決定。手動：使用下方 GOP / I 幀間隔與 B 幀間隔，並寫入 -g、-bf。",
        L"選擇透明輸出方式：無、4 通道，或黑白遮罩。4 通道僅適用於 VP9 與 ProRes。",
        L"黑白遮罩可堆疊在同一影片下方（高度 x2），或輸出為另一個遮罩影片。",
        L"選擇 4:2:0、4:2:2 或 4:4:4 色度取樣。部分 Alpha 組合會固定為 4:4:4。",
        L"指定輸出色彩空間：BT.601、BT.709 或 BT.2020。",
        L"MMD 輸入固定為 PC 全範圍 RGB；這裡選擇輸出使用 TV 限制範圍或 PC 全範圍。",
        L"選擇從 AVI 合併到 MKV 的音訊格式，或不要輸出音訊。",
        L"選擇保留原始音訊，或使用 Hi-Res 重新編碼模式。",
        L"可直接微調 FFmpeg 的中間編碼參數。修改後請重新測試編碼器。",
        L"以目前設定執行小型測試；通過後才能儲存或套用。",
        L"開啟 MMD2FFMPEG 編碼與合併音訊的記錄資料夾。",
        L"開啟專案的 GitHub 網頁。"};
    static constexpr UiTooltips simplified{
        L"选择设置界面的显示语言；系统默认会跟随 Windows。",
        L"选择 CPU 软件编码使用的线程数。自动不传递 -threads；全部、全部 - 1、全部 - 2 会依 Windows 检测的可用逻辑处理器数计算。硬件编码不使用此设置。",
        L"选择 FFmpeg 使用的软件或硬件编码器。VP9 和 ProRes 只能使用 CPU。",
        L"选择视频编码格式。可用的位深度、Alpha 和码率控制会依格式自动限制。",
        L"选择 8-bit 或 10-bit 输出。部分编码格式或 Alpha 模式会固定此选项。",
        L"调整编码速度与压缩效率。较快通常文件较大，较慢通常压缩较佳。",
        L"选择固定质量、固定 QP 或目标比特率控制方式。",
        L"设置固定质量或 QP 的数值；数值含义会依当前的编码器与控制方式而不同。",
        L"设置 VBR 目标比特率（kbps）。只有选择 VBR 时会使用此值。",
        L"设置 GOP 的最大长度，即 I 帧之间最多相隔多少画面。I 帧是可独立解码的基准画面；实际场景切换可能提早插入 I 帧。对应 FFmpeg 的 -g。",
        L"设置每两个 I/P 参考帧之间最多插入多少 B 帧。B 帧可同时参考前后 I/P 帧，通常能提高压缩率但增加延迟；0 代表不使用 B 帧。对应 FFmpeg 的 -bf。",
        L"自动：不传递 GOP 或 B 帧参数，交由 FFmpeg 与编码器决定。手动：使用下方 GOP / I 帧间隔与 B 帧间隔，并写入 -g、-bf。",
        L"选择透明输出方式：无、4 通道，或黑白遮罩。4 通道仅适用于 VP9 和 ProRes。",
        L"黑白遮罩可堆叠在同一视频下方（高度 x2），或输出为另一个遮罩视频。",
        L"选择 4:2:0、4:2:2 或 4:4:4 色度采样。部分 Alpha 组合会固定为 4:4:4。",
        L"指定输出色彩空间：BT.601、BT.709 或 BT.2020。",
        L"MMD 输入固定为 PC 全范围 RGB；这里选择输出使用 TV 限制范围或 PC 全范围。",
        L"选择从 AVI 合并到 MKV 的音频格式，或不输出音频。",
        L"选择保留原始音频，或使用 Hi-Res 重新编码模式。",
        L"可直接微调 FFmpeg 的中间编码参数。修改后请重新测试编码器。",
        L"以当前设置执行小型测试；通过后才能保存或应用。",
        L"打开 MMD2FFMPEG 编码与合并音频的日志文件夹。",
        L"打开项目的 GitHub 网页。"};
    static constexpr UiTooltips japanese{
        L"設定画面の表示言語を選択します。システム既定は Windows の表示言語に従います。",
        L"CPU ソフトウェアエンコードで使用するスレッド数を選択します。自動は -threads を渡しません。すべて、すべて - 1、すべて - 2 は Windows が検出した論理プロセッサ数から計算します。ハードウェアエンコードでは使用しません。",
        L"FFmpeg で使用するソフトウェアまたはハードウェアエンコーダーを選択します。VP9 と ProRes は CPU 専用です。",
        L"動画コーデックを選択します。利用可能なビット深度、アルファ、レート制御は形式に応じて制限されます。",
        L"8-bit または 10-bit 出力を選択します。コーデックまたはアルファモードにより固定される場合があります。",
        L"エンコード速度と圧縮効率を調整します。高速ほど通常はファイルが大きく、低速ほど圧縮効率が高くなります。",
        L"固定品質、固定 QP、または目標ビットレートの制御方法を選択します。",
        L"固定品質または QP の値を設定します。意味は現在のエンコーダーと制御方法によって異なります。",
        L"VBR の目標ビットレート（kbps）を設定します。VBR 選択時のみ使用されます。",
        L"GOP の最大長、つまり I フレーム間の最大フレーム数を設定します。I フレームは単独でデコードできる基準画面です。シーン切替では早めに I フレームが挿入される場合があります。FFmpeg の -g に対応します。",
        L"2 つの I/P 参照フレームの間に挿入する B フレームの最大数を設定します。B フレームは前後の I/P フレームを参照でき、圧縮率を高めますが遅延も増えます。0 は B フレームなしです。FFmpeg の -bf に対応します。",
        L"自動：GOP と B フレームの引数を渡さず、FFmpeg とエンコーダーに任せます。手動：下の GOP / I フレーム間隔と B フレーム間隔を使用し、-g と -bf を指定します。",
        L"透明出力を選択します：なし、4 チャンネル、または白黒マスク。4 チャンネルは VP9 と ProRes のみ対応です。",
        L"白黒マスクは同じ動画の下に積み重ねる（高さ x2）か、別のマスク動画として出力できます。",
        L"4:2:0、4:2:2、4:4:4 のクロマサンプリングを選択します。一部のアルファ組み合わせでは 4:4:4 に固定されます。",
        L"出力色空間を BT.601、BT.709、BT.2020 から選択します。",
        L"MMD の入力は PC フルレンジ RGB に固定です。ここでは出力を TV リミテッドまたは PC フルレンジから選択します。",
        L"AVI から MKV へ結合する音声形式、または音声なしを選択します。",
        L"元の音声を保持するか、Hi-Res 再エンコードモードを使用するか選択します。",
        L"FFmpeg の中央のエンコード引数を直接調整できます。変更後はエンコーダーを再テストしてください。",
        L"現在の設定で小さなテストを実行します。保存または適用する前に合格が必要です。",
        L"MMD2FFMPEG のエンコードおよび音声結合ログのフォルダーを開きます。",
        L"プロジェクトの GitHub ページを開きます。"};
    static constexpr UiTooltips english{
        L"Choose the settings interface language. System default follows the Windows display language.",
        L"Choose the thread count for CPU software encoding. Automatic passes no -threads option; All, All - 1, and All - 2 are calculated from Windows-detected logical processors. Hardware encoding does not use this setting.",
        L"Choose the FFmpeg software or hardware encoder. VP9 and ProRes are CPU-only.",
        L"Choose the video codec. Available bit depth, alpha, and rate control options are limited by the codec.",
        L"Choose 8-bit or 10-bit output. Some codecs or alpha modes lock this option.",
        L"Adjust encoding speed and compression efficiency. Faster presets usually create larger files; slower presets usually compress better.",
        L"Choose constant quality, constant QP, or target bitrate rate control.",
        L"Set the constant-quality or QP value. Its meaning depends on the active encoder and rate control.",
        L"Set the VBR target bitrate in kbps. This value is used only when VBR is selected.",
        L"Set the maximum GOP length: the greatest number of frames between I-frames. An I-frame is an independently decodable reference picture; scene changes can insert one earlier. Maps to FFmpeg -g.",
        L"Set the maximum number of B-frames between I/P reference frames. B-frames can reference both earlier and later I/P frames, usually improving compression at the cost of latency. Set 0 to disable B-frames. Maps to FFmpeg -bf.",
        L"Automatic: pass no GOP or B-frame arguments and let FFmpeg and the encoder decide. Manual: use the GOP / I-frame interval and B-frame interval below, writing -g and -bf.",
        L"Choose transparency output: none, 4-channel, or a black/white mask. 4-channel is available only with VP9 and ProRes.",
        L"Place the black/white mask below the same video (height x2), or write a separate mask video.",
        L"Choose 4:2:0, 4:2:2, or 4:4:4 chroma sampling. Some alpha combinations force 4:4:4.",
        L"Set the output color space to BT.601, BT.709, or BT.2020.",
        L"MMD input is fixed to PC full-range RGB. Choose TV limited-range or PC full-range for output.",
        L"Choose the audio format merged from AVI into MKV, or disable audio output.",
        L"Keep the original audio or use the Hi-Res re-encoding mode.",
        L"Directly fine-tune the middle FFmpeg encoding arguments. Test the encoder again after editing.",
        L"Run a small test using the current settings. It must pass before settings can be saved or applied.",
        L"Open the folder containing MMD2FFMPEG encoding and audio-merge logs.",
        L"Open the project's GitHub page."};
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

std::wstring editable_arguments(const Settings& settings);

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
        else if (key == L"codec" && (value == L"avc" || value == L"hevc" || value == L"av1" || value == L"vp9" || value == L"prores")) settings.codec = value;
        else if (key == L"bit_depth") { try { settings.bit_depth = std::stoi(value) == 8 ? 8 : 10; } catch (...) {} }
        else if (key == L"chroma" && (value == L"420" || value == L"422" || value == L"444")) settings.chroma = value;
        else if (key == L"alpha_mode" && (value == L"none" || value == L"rgba" || value == L"mask")) settings.alpha_mode = value;
        else if (key == L"mask_output" && (value == L"stacked" || value == L"separate")) settings.mask_output = value;
        else if (key == L"color_space" && (value == L"bt601" || value == L"bt709" || value == L"bt2020")) settings.color_space = value;
        else if (key == L"color_range" && (value == L"tv" || value == L"pc")) settings.color_range = value;
        else if (key == L"preset") { try { settings.preset = std::clamp(std::stoi(value), 1, 7); } catch (...) {} }
        else if (key == L"rate_control" && (value == L"crf" || value == L"qp" || value == L"vbr")) settings.rate_control = value;
        else if (key == L"qp") { try { settings.qp = std::clamp(std::stoi(value), 0, 51); } catch (...) {} }
        else if (key == L"bitrate_kbps") { try { settings.bitrate_kbps = std::clamp(std::stoi(value), 100, 1000000); } catch (...) {} }
        else if (key == L"frame_structure_mode" && (value == L"auto" || value == L"manual")) settings.frame_structure_mode = value;
        else if (key == L"gop") { try { settings.gop = std::clamp(std::stoi(value), 1, 10000); } catch (...) {} }
        else if (key == L"b_frames") { try { settings.b_frames = std::clamp(std::stoi(value), 0, 16); } catch (...) {} }
        else if (key == L"cpu_threads" && (value == L"auto" || value == L"all" || value == L"all-minus-1" || value == L"all-minus-2")) settings.cpu_threads = value;
        else if (key == L"audio_format" && (value == L"flac" || value == L"wav" || value == L"none")) settings.audio_format = value;
        else if (key == L"audio_sample_rate" && (value == L"original" || value == L"hires")) settings.audio_sample_rate = value;
        else if (key == L"audio_bit_depth" && (value == L"original" || value == L"24")) settings.audio_bit_depth = value;
        else if (key == L"vsr_enabled") settings.vsr_enabled = value == L"1";
        else if (key == L"vsr_scale") { try { settings.vsr_scale = std::clamp(std::stod(value), 1.0, 4.0); } catch (...) {} }
        else if (key == L"vsr_quality") { try { settings.vsr_quality = std::clamp(std::stoi(value), 1, 4); } catch (...) {} }
        else if (key == L"language" && (value == L"system" || value == L"zh-TW" || value == L"zh-CN" || value == L"ja" || value == L"en")) settings.language = value;
        else if (key == L"command_template") settings.command_template = value;
    }
    normalize_codec_settings(settings);
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
    if (settings.video_args.rfind(L"-c:v ", 0) == 0) settings.video_args = editable_arguments(settings);
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
         << L"chroma=" << settings.chroma << L"\n"
         << L"alpha_mode=" << settings.alpha_mode << L"\n"
         << L"mask_output=" << settings.mask_output << L"\n"
         << L"color_space=" << settings.color_space << L"\n"
         << L"color_range=" << settings.color_range << L"\n"
         << L"preset=" << settings.preset << L"\n"
         << L"rate_control=" << settings.rate_control << L"\n"
         << L"qp=" << settings.qp << L"\n"
         << L"bitrate_kbps=" << settings.bitrate_kbps << L"\n"
         << L"frame_structure_mode=" << settings.frame_structure_mode << L"\n"
         << L"gop=" << settings.gop << L"\n"
         << L"b_frames=" << settings.b_frames << L"\n"
         << L"cpu_threads=" << settings.cpu_threads << L"\n"
         << L"audio_format=" << settings.audio_format << L"\n"
         << L"audio_sample_rate=" << settings.audio_sample_rate << L"\n"
         << L"audio_bit_depth=" << settings.audio_bit_depth << L"\n"
         << L"vsr_enabled=" << (settings.vsr_enabled ? 1 : 0) << L"\n"
         << L"vsr_scale=" << settings.vsr_scale << L"\n"
         << L"vsr_quality=" << settings.vsr_quality << L"\n";
    file << L"language=" << settings.language << L"\n";
    std::wstring video_args = settings.video_args;
    std::replace_if(video_args.begin(), video_args.end(), [](wchar_t character) { return character == L'\r' || character == L'\n'; }, L' ');
    if (!video_args.empty()) file << L"video_args=" << video_args << L"\n";
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
    const bool ten_bit = settings.bit_depth == 10;
    const std::wstring codec_name = settings.codec == L"avc" ? L"h264" : settings.codec;
    std::wstring encoder;
    if (settings.backend == L"cpu")
        encoder = settings.codec == L"avc" ? L"libx264" : settings.codec == L"av1" ? L"libsvtav1" :
                  settings.codec == L"vp9" ? L"libvpx-vp9" : settings.codec == L"prores" ? L"prores_ks" : L"libx265";
    else
        encoder = codec_name + L"_" + settings.backend;
    std::wostringstream args;
    args << L"-c:v " << encoder;
    if (settings.backend == L"cpu") {
        const int threads = selected_cpu_threads(settings);
        if (threads > 0) args << L" -threads " << threads;
    }
    if (settings.codec == L"prores") {
        args << L" -profile:v " << (settings.alpha_mode == L"rgba" || settings.chroma == L"444" ? L"4444" : L"hq");
        return args.str();
    }
    if (settings.codec == L"hevc" && ten_bit) args << L" -profile:v main10";
    else if (settings.codec == L"avc") args << L" -profile:v high";

    const int level = std::clamp(settings.preset, 1, 7);
    if (settings.backend == L"cpu") {
        static constexpr const wchar_t* software_presets[] = {L"ultrafast", L"superfast", L"veryfast", L"faster", L"fast", L"medium", L"slow"};
        static constexpr int svt_presets[] = {13, 11, 9, 8, 7, 6, 4};
        if (settings.codec == L"av1") args << L" -preset " << svt_presets[level - 1];
        else if (settings.codec == L"vp9") args << L" -deadline good -cpu-used " << (8 - level);
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
    if (settings.frame_structure_mode == L"manual") {
        args << L" -g " << std::clamp(settings.gop, 1, 10000);
        args << L" -bf " << std::clamp(settings.b_frames, 0, 16);
    }
    return args.str();
}

const wchar_t* output_pixel_format(const Settings& settings) {
    if (settings.alpha_mode == L"rgba") {
        if (settings.codec == L"prores") return L"yuva444p10le";
        return settings.bit_depth == 10 ? L"yuva420p10le" : L"yuva420p";
    }
    const bool ten_bit = settings.bit_depth == 10;
    if (settings.chroma == L"444") return ten_bit ? L"yuv444p10le" : L"yuv444p";
    if (settings.chroma == L"422") return ten_bit ? L"yuv422p10le" : L"yuv422p";
    return ten_bit ? L"yuv420p10le" : L"yuv420p";
}

std::wstring recording_date_metadata();

struct ColorSpaceSpec { const wchar_t* matrix; const wchar_t* primaries; const wchar_t* transfer; };

ColorSpaceSpec color_space_spec(const Settings& settings) {
    if (settings.color_space == L"bt601") return {L"smpte170m", L"smpte170m", L"bt709"};
    if (settings.color_space == L"bt2020") return {L"bt2020nc", L"bt2020", L"bt709"};
    return {L"bt709", L"bt709", L"bt709"};
}

std::wstring color_metadata(const Settings& settings) {
    const auto color = color_space_spec(settings);
    return L" -colorspace " + std::wstring(color.matrix) + L" -color_primaries " + color.primaries +
           L" -color_trc " + color.transfer + L" -color_range " + settings.color_range +
           L" -metadata date_recorded=" + recording_date_metadata();
}

std::wstring command_prefix(const Settings& settings) {
    return L"\"" + settings.ffmpeg +
           L"\" -hide_banner -loglevel warning -y -f rawvideo -pixel_format {input_pixel_format} "
           L"-video_size {width}x{height} -framerate {fps} -i pipe:0 ";
}

std::wstring command_suffix(const Settings& settings) {
    if (settings.alpha_mode == L"mask" && settings.mask_output == L"separate")
        return L" \"{output}\" -map \"[mask]\" -c:v ffv1 -pix_fmt gray \"{mask_output}\"";
    return L" \"{output}\"";
}

std::wstring editable_arguments(const Settings& settings) {
    if (settings.alpha_mode != L"mask") {
        const auto pixel_format = output_pixel_format(settings);
        const auto color = color_space_spec(settings);
        return L"-vf scale=in_range=pc:out_range=" + settings.color_range + L":out_color_matrix=" + color.matrix +
               L",format=" + pixel_format + L" " + encoding_arguments(settings) +
               L" -pix_fmt " + pixel_format + color_metadata(settings);
    }
    const auto color = color_space_spec(settings);
    Settings color_settings = settings;
    color_settings.alpha_mode = L"none";
    const std::wstring pixel_format = output_pixel_format(color_settings);
    std::wstring arguments = L"-filter_complex \"[0:v]split=2[color][alpha];[color]scale=in_range=pc:out_range=" +
        settings.color_range + L":out_color_matrix=" + color.matrix + L",format=" + pixel_format + L"[colorout];" +
        L"[alpha]alphaextract,format=gray[mask]";
    if (settings.mask_output == L"stacked")
        arguments += L";[colorout][mask]vstack=inputs=2,format=" + pixel_format + L"[out]\" -map \"[out]\" ";
    else
        arguments += L"\" -map \"[colorout]\" ";
    return arguments + encoding_arguments(settings) + L" -pix_fmt " + pixel_format + color_metadata(settings);
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
                                  const std::wstring& output_path, const std::wstring& mask_path = L"") {
    const auto arguments = settings.video_args.empty() ? editable_arguments(settings) : settings.video_args;
    std::wstring command = command_prefix(settings) + arguments + command_suffix(settings);
    replace_all(command, L"{mask_output}", mask_path);
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

std::filesystem::path resolve_nvenc(const std::filesystem::path& ffmpeg_path) {
    const auto sibling = ffmpeg_path.parent_path() / L"NVEncC.exe";
    std::error_code error;
    if (std::filesystem::exists(sibling, error)) return sibling;
    const auto sibling64 = ffmpeg_path.parent_path() / L"NVEncC64.exe";
    if (std::filesystem::exists(sibling64, error)) return sibling64;
    auto path = resolve_executable(L"NVEncC.exe");
    if (!path.empty()) return path;
    return resolve_executable(L"NVEncC64.exe");
}

std::wstring build_vsr_command(const Settings& settings, int width, int height, int bits,
                               const std::filesystem::path& ffmpeg_path,
                               const std::filesystem::path& nvenc_path,
                               const std::filesystem::path& bridge_path,
                               const std::wstring& output_path, bool probe = false) {
    std::wostringstream command;
    command << quote(bridge_path.wstring())
            << L" --ffmpeg " << quote(ffmpeg_path.wstring())
            << L" --nvenc " << quote(nvenc_path.wstring())
            << L" --output " << quote(output_path)
            << L" --width " << width << L" --height " << height << L" --fps " << settings.fps
            << L" --input-format " << (bits == 24 ? L"bgr24" : L"bgra")
            << L" --scale " << std::fixed << std::setprecision(3) << settings.vsr_scale
            << L" --quality " << settings.vsr_quality
            << L" --depth " << settings.bit_depth
            << L" --output-csp yuv" << settings.chroma
            << L" --preset p" << std::clamp(settings.preset, 1, 7)
            << L" --rate " << settings.rate_control
            << L" --qp " << settings.qp
            << L" --bitrate " << settings.bitrate_kbps
            << L" --frame-mode " << settings.frame_structure_mode
            << L" --gop " << settings.gop << L" --bframes " << settings.b_frames;
    if (settings.alpha_mode == L"rgba") command << L" --alpha";
    if (settings.vsr_enabled) command << L" --vsr";
    if (probe) command << L" --probe";
    return command.str();
}

bool uses_nvenc_bridge(const Settings& settings) {
    return settings.vsr_enabled ||
           (settings.backend == L"nvenc" && settings.codec == L"hevc" && settings.alpha_mode == L"rgba");
}

DWORD encoder_test_timeout_ms(const Settings& settings) {
    static_cast<void>(settings);
    return 20000u;
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

void drain_process_output(HANDLE output_read, std::vector<char>& output) {
    std::array<char, 4096> buffer{};
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(output_read, nullptr, 0, nullptr, &available, nullptr) || available == 0) return;
        DWORD read = 0;
        const DWORD request = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
        if (!ReadFile(output_read, buffer.data(), request, &read, nullptr) || read == 0) return;
        if (output.size() < 16384) {
            const std::size_t retained = std::min<std::size_t>(read, 16384 - output.size());
            output.insert(output.end(), buffer.data(), buffer.data() + retained);
        }
    }
}

DWORD wait_for_process_with_output(HANDLE process, HANDLE output_read, DWORD timeout_ms,
                                   std::vector<char>& output) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        drain_process_output(output_read, output);
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return WAIT_TIMEOUT;
        const DWORD remaining = static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, 50));
        const DWORD wait_result = WaitForSingleObject(process, remaining);
        if (wait_result == WAIT_OBJECT_0) {
            drain_process_output(output_read, output);
            return wait_result;
        }
        if (wait_result != WAIT_TIMEOUT) return wait_result;
    }
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
    std::vector<char> output;
    const DWORD wait_result = wait_for_process_with_output(process.hProcess, output_read, 5000, output);
    if (wait_result == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 1000);
        drain_process_output(output_read, output);
    }
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
    std::filesystem::path executable = ffmpeg_path;
    std::filesystem::path probe_output;
    std::wstring command;
    if (uses_nvenc_bridge(settings)) {
        if (settings.alpha_mode == L"mask") {
            error_message = L"RTX VSR does not support the black/white mask output mode.";
            return false;
        }
        if (settings.vsr_enabled && settings.alpha_mode == L"rgba") {
            error_message = L"NVEncC cannot combine NGX VSR and HEVC alpha in the same pass.";
            return false;
        }
        const auto nvenc_path = resolve_nvenc(ffmpeg_path);
        const auto bridge_path = local_data_dir() / L"mmd2ffmpeg_vsr_bridge.exe";
        if (nvenc_path.empty()) {
            error_message = L"NVEncC.exe was not found beside FFmpeg or in PATH.";
            return false;
        }
        std::error_code bridge_error;
        if (!std::filesystem::exists(bridge_path, bridge_error)) {
            error_message = L"mmd2ffmpeg_vsr_bridge.exe is not installed.";
            return false;
        }
        probe_output = local_data_dir() / L"vsr-probe.mkv";
        std::filesystem::remove(probe_output, bridge_error);
        executable = bridge_path;
        command = build_vsr_command(settings, 320, 180, 32, ffmpeg_path, nvenc_path, bridge_path,
                                    probe_output.wstring(), true);
    }
    const auto arguments = settings.video_args.empty() ? editable_arguments(settings) : settings.video_args;
    if (!uses_nvenc_bridge(settings) && settings.alpha_mode == L"mask") {
        command = L"\"" + settings.ffmpeg +
            L"\" -hide_banner -loglevel error -f lavfi -i color=c=black@0.5:s=1920x1080:r=1,format=bgra -frames:v 1 "
            + arguments;
        if (settings.mask_output == L"stacked") {
            command += L" -frames:v 1 -f null -";
        } else {
            command += L" -frames:v 1 -f null - -map \"[mask]\" -c:v ffv1 -pix_fmt gray -frames:v 1 -f null -";
        }
    } else if (!uses_nvenc_bridge(settings)) {
        command = L"\"" + settings.ffmpeg +
            L"\" -hide_banner -loglevel error -f lavfi -i color=c=black@0.5:s=1920x1080:r=1 -frames:v 1 "
            + arguments + L" -f null -";
    }

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
    const BOOL created = CreateProcessW(executable.c_str(), mutable_command.data(), nullptr, nullptr,
                                        TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    close_handle(output_write);
    if (!created) {
        close_handle(output_read);
        error_message = L"Could not start the encoder test (Windows error " + std::to_wstring(GetLastError()) + L").";
        return false;
    }

    const DWORD timeout_ms = encoder_test_timeout_ms(settings);
    std::vector<char> output;
    const DWORD wait_result = wait_for_process_with_output(process.hProcess, output_read, timeout_ms, output);
    if (wait_result == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 1000);
        drain_process_output(output_read, output);
    }
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    close_handle(output_read);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (!probe_output.empty()) {
        std::error_code remove_error;
        std::filesystem::remove(probe_output, remove_error);
    }
    if (wait_result == WAIT_TIMEOUT) {
        error_message = L"Encoder test timed out after " + std::to_wstring(timeout_ms / 1000) + L" seconds.";
        return false;
    }
    if (wait_result != WAIT_OBJECT_0 || exit_code != 0) {
        error_message = decode_process_output(output);
        return false;
    }
    return true;
}

std::wstring command_test_signature(const Settings& settings) {
    const auto normalize_rate_values = [](const std::wstring& arguments) {
        static constexpr std::array<std::wstring_view, 9> rate_options{
            L"-crf", L"-qp", L"-cq", L"-b:v", L"-global_quality", L"-qvbr_quality_level", L"-qp_i", L"-qp_p", L"-qp_b"};
        std::wistringstream input(arguments);
        std::wostringstream output;
        std::wstring token;
        bool replace_next = false;
        bool first = true;
        while (input >> token) {
            if (!first) output << L' ';
            first = false;
            if (replace_next) {
                output << L"{rate}";
                replace_next = false;
                continue;
            }
            bool rate_option = false;
            for (const auto option : rate_options) {
                if (token == option) {
                    rate_option = true;
                    break;
                }
                const std::wstring prefix(option);
                if (token.rfind(prefix + L"=", 0) == 0) {
                    output << prefix << L"={rate}";
                    rate_option = false;
                    token.clear();
                    break;
                }
            }
            if (!token.empty()) output << token;
            replace_next = rate_option;
        }
        return output.str();
    };
    const auto ffmpeg_path = resolve_executable(settings.ffmpeg);
    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(ffmpeg_path, error).time_since_epoch().count();
    const auto arguments = normalize_rate_values(settings.video_args.empty() ? editable_arguments(settings) : settings.video_args);
    std::wstring bridge_signature;
    if (uses_nvenc_bridge(settings)) {
        const auto nvenc_path = resolve_nvenc(ffmpeg_path);
        const auto bridge_path = local_data_dir() / L"mmd2ffmpeg_vsr_bridge.exe";
        const auto nvenc_stamp = std::filesystem::last_write_time(nvenc_path, error).time_since_epoch().count();
        const auto bridge_stamp = std::filesystem::last_write_time(bridge_path, error).time_since_epoch().count();
        bridge_signature = L"|" + nvenc_path.wstring() + L"|" + std::to_wstring(nvenc_stamp) +
                           L"|" + bridge_path.wstring() + L"|" + std::to_wstring(bridge_stamp);
    }
    return L"v10-1920x1080|" + ffmpeg_path.wstring() + L"|" + std::to_wstring(stamp) + bridge_signature +
           L"|" + settings.backend + L"|" +
           settings.codec + L"|" + std::to_wstring(settings.bit_depth) + L"|" + settings.chroma + L"|" + settings.alpha_mode +
           L"|" + settings.mask_output + L"|" + settings.color_space + L"|" + settings.color_range + L"|" + arguments +
           L"|vsr=" + std::to_wstring(settings.vsr_enabled ? 1 : 0) + L"|" + std::to_wstring(settings.vsr_scale) +
           L"|" + std::to_wstring(settings.vsr_quality);
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
        std::filesystem::path launch_path = ffmpeg_path;
        std::filesystem::path nvenc_path;
        std::filesystem::path bridge_path;
        if (uses_nvenc_bridge(settings_)) {
            if (settings_.alpha_mode == L"mask") return false;
            if (settings_.vsr_enabled && settings_.alpha_mode == L"rgba") return false;
            nvenc_path = resolve_nvenc(ffmpeg_path);
            bridge_path = local_data_dir() / L"mmd2ffmpeg_vsr_bridge.exe";
            std::error_code bridge_error;
            if (nvenc_path.empty() || !std::filesystem::exists(bridge_path, bridge_error)) return false;
            launch_path = bridge_path;
        }
        final_output_ = avi;
        partial_output_ = final_output_.parent_path() /
            (final_output_.stem().wstring() + L".mmd2ffmpeg-partial-" + std::to_wstring(GetCurrentProcessId()) + final_output_.extension().wstring());
        std::error_code error;
        std::filesystem::remove(partial_output_, error);
        final_mask_output_.clear();
        partial_mask_output_.clear();
        if (settings_.alpha_mode == L"mask" && settings_.mask_output == L"separate") {
            final_mask_output_ = final_output_.parent_path() /
                (final_output_.stem().wstring() + L"_alpha" + final_output_.extension().wstring());
            partial_mask_output_ = final_mask_output_.parent_path() /
                (final_mask_output_.stem().wstring() + L".mmd2ffmpeg-partial-" + std::to_wstring(GetCurrentProcessId()) + final_mask_output_.extension().wstring());
            std::filesystem::remove(partial_mask_output_, error);
        }
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
        auto command = uses_nvenc_bridge(settings_)
            ? build_vsr_command(settings_, width_, height_, bits_, ffmpeg_path, nvenc_path, bridge_path,
                                partial_output_.wstring())
            : build_ffmpeg_command(settings_, width_, height_, bits_, partial_output_.wstring(), partial_mask_output_.wstring());
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
        const BOOL created = CreateProcessW(launch_path.c_str(), mutable_command.data(), nullptr, nullptr,
                                             TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
        CloseHandle(read_pipe);
        if (!created) {
            write_log_line(log_file_, L"\r\n[MMD2FFMPEG] Encoder process could not be started (Windows error " +
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
        if (success && !partial_mask_output_.empty()) {
            const bool mask_ready = std::filesystem::exists(partial_mask_output_, error) &&
                                    std::filesystem::file_size(partial_mask_output_, error) > 0;
            success = mask_ready && MoveFileExW(partial_mask_output_.c_str(), final_mask_output_.c_str(),
                                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        }
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
        if (success) {
            summary << L"Output size: " << output_size << L" bytes\r\n";
            if (!final_mask_output_.empty()) summary << L"Alpha mask: " << final_mask_output_.wstring() << L"\r\n";
        }
        else summary << L"Partial output size: " << partial_size << L" bytes\r\n";
        if (!success && !partial_output_.empty()) {
            const bool removed = std::filesystem::remove(partial_output_, error) ||
                                  !std::filesystem::exists(partial_output_, error);
            summary << L"Partial output cleanup: " << (removed ? L"success" : L"failed") << L"\r\n";
        }
        if (!success && !partial_mask_output_.empty()) std::filesystem::remove(partial_mask_output_, error);
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
    std::filesystem::path final_output_, partial_output_, final_mask_output_, partial_mask_output_, avi_output_, log_path_;
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
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_TAB_CLASSES};
        InitCommonControlsEx(&common);
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
        candidate.codec = codec_capability_from_index(combo_index(ID_CODEC)).key;
        candidate.bit_depth = combo_text(ID_DEPTH) == L"10-bit" ? 10 : 8;
        candidate.chroma = combo_index(ID_CHROMA) == 1 ? L"422" : combo_index(ID_CHROMA) == 2 ? L"444" : L"420";
        candidate.alpha_mode = combo_index(ID_ALPHA) == 1 ? L"rgba" : combo_index(ID_ALPHA) == 2 ? L"mask" : L"none";
        candidate.mask_output = combo_index(ID_MASK_OUTPUT) == 1 ? L"separate" : L"stacked";
        candidate.color_space = combo_index(ID_COLORSPACE) == 0 ? L"bt601" : combo_index(ID_COLORSPACE) == 2 ? L"bt2020" : L"bt709";
        candidate.color_range = combo_index(ID_COLOR_RANGE) == 1 ? L"pc" : L"tv";
        candidate.preset = combo_index(ID_BACKEND) == 3 ? (combo_index(ID_PRESET) == 0 ? 1 : combo_index(ID_PRESET) == 1 ? 4 : 7) : combo_index(ID_PRESET) + 1;
        candidate.rate_control = combo_index(ID_RATE) == 0 ? L"crf" : combo_index(ID_RATE) == 1 ? L"qp" : L"vbr";
        candidate.qp = std::clamp(edit_number(ID_QP, 20), 0, 51);
        candidate.bitrate_kbps = std::clamp(edit_number(ID_BITRATE, 20000), 100, 1000000);
        if (!uses_nvenc_bridge(candidate)) candidate.video_args = edit_text(ID_COMMAND);
        candidate.audio_format = combo_index(ID_AUDIO_FORMAT) == 0 ? L"flac" : combo_index(ID_AUDIO_FORMAT) == 1 ? L"wav" : L"none";
        candidate.audio_sample_rate = combo_index(ID_AUDIO_RATE) == 1 ? L"hires" : L"original";
        candidate.audio_bit_depth = candidate.audio_sample_rate == L"hires" ? L"24" : L"original";
        candidate.vsr_enabled = combo_index(ID_VSR_ENABLED) == 1;
        candidate.vsr_scale = std::clamp(edit_double(ID_VSR_SCALE, 2.0), 1.0, 4.0);
        candidate.vsr_quality = std::clamp(combo_index(ID_VSR_QUALITY) + 1, 1, 4);
        if (uses_nvenc_bridge(candidate)) candidate.video_args = settings_.video_args;
        normalize_codec_settings(candidate);
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
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_TAB_CLASSES | ICC_WIN95_CLASSES};
        InitCommonControlsEx(&common);
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
    UINT tooltip_info_size() const {
        return SendMessageW(tooltip_, CCM_GETVERSION, 0, 0) < 6 ? TTTOOLINFOW_V2_SIZE : sizeof(TOOLINFOW);
    }
    void update_tooltip(int id, const wchar_t* text, bool add) {
        const HWND control = GetDlgItem(window_, id);
        if (!tooltip_ || !control) return;
        TOOLINFOW info{};
        info.cbSize = tooltip_info_size();
        info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        info.hwnd = window_;
        info.uId = reinterpret_cast<UINT_PTR>(control);
        info.lpszText = const_cast<wchar_t*>(text);
        SendMessageW(tooltip_, add ? TTM_ADDTOOLW : TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&info));
    }
    void apply_tooltips() {
        if (!window_) return;
        if (!tooltip_) {
            tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                       CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window_, nullptr,
                                       module_instance(), nullptr);
            if (!tooltip_) return;
            SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 480);
            SetWindowPos(tooltip_, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            SetPropW(window_, L"MMD2FFMPEG.TooltipWindow", tooltip_);
        }
        const auto& tip = ui_tooltips(ui_language(settings_.language));
        const std::array<std::pair<int, const wchar_t*>, 43> items{{
            {ID_LABEL_LANGUAGE, tip.language}, {ID_LANGUAGE, tip.language},
            {ID_LABEL_CPU_THREADS, tip.cpu_threads}, {ID_CPU_THREADS, tip.cpu_threads},
            {ID_LABEL_BACKEND, tip.backend}, {ID_BACKEND, tip.backend},
            {ID_LABEL_CODEC, tip.codec}, {ID_CODEC, tip.codec},
            {ID_LABEL_DEPTH, tip.bit_depth}, {ID_DEPTH, tip.bit_depth},
            {ID_LABEL_PRESET, tip.preset}, {ID_PRESET, tip.preset},
            {ID_LABEL_RATE, tip.rate_control}, {ID_RATE, tip.rate_control},
            {ID_LABEL_QP, tip.quality}, {ID_QP, tip.quality},
            {ID_LABEL_BITRATE, tip.bitrate}, {ID_BITRATE, tip.bitrate},
            {ID_LABEL_GOP, tip.gop}, {ID_GOP, tip.gop},
            {ID_LABEL_BFRAMES, tip.b_frames}, {ID_BFRAMES, tip.b_frames},
            {ID_LABEL_FRAME_MODE, tip.frame_structure_mode}, {ID_FRAME_MODE, tip.frame_structure_mode},
            {ID_LABEL_ALPHA, tip.alpha}, {ID_ALPHA, tip.alpha},
            {ID_LABEL_MASK_OUTPUT, tip.mask_output}, {ID_MASK_OUTPUT, tip.mask_output},
            {ID_LABEL_CHROMA, tip.chroma}, {ID_CHROMA, tip.chroma},
            {ID_LABEL_COLORSPACE, tip.color_space}, {ID_COLORSPACE, tip.color_space},
            {ID_LABEL_COLOR_RANGE, tip.color_range}, {ID_COLOR_RANGE, tip.color_range},
            {ID_LABEL_AUDIO_FORMAT, tip.audio_format}, {ID_AUDIO_FORMAT, tip.audio_format},
            {ID_LABEL_AUDIO_RATE, tip.audio_rate}, {ID_AUDIO_RATE, tip.audio_rate},
            {ID_COMMAND_HEADING, tip.command}, {ID_COMMAND, tip.command},
            {ID_REFRESH, tip.test_encoder}, {ID_OPEN_LOG, tip.open_log}, {ID_GITHUB_LINK, tip.github},
        }};
        for (const auto& item : items) update_tooltip(item.first, item.second, tooltips_initialized_ == false);
        const auto vsr_tip = vsr_tooltips(ui_language(settings_.language));
        const std::array<std::pair<int, const wchar_t*>, 6> vsr_items{{
            {ID_LABEL_VSR_ENABLED, vsr_tip[0]}, {ID_VSR_ENABLED, vsr_tip[0]},
            {ID_LABEL_VSR_SCALE, vsr_tip[1]}, {ID_VSR_SCALE, vsr_tip[1]},
            {ID_LABEL_VSR_QUALITY, vsr_tip[2]}, {ID_VSR_QUALITY, vsr_tip[2]},
        }};
        for (const auto& item : vsr_items) update_tooltip(item.first, item.second, tooltips_initialized_ == false);
        tooltips_initialized_ = true;
    }
    void create_controls() {
        tab_ = GetDlgItem(window_, ID_TAB);
        video_tab_ = GetDlgItem(window_, ID_VIDEO_TAB);
        audio_intro_ = GetDlgItem(window_, ID_AUDIO_INTRO);
        audio_help_ = GetDlgItem(window_, ID_AUDIO_HELP);
        audio_labels_ = {GetDlgItem(window_, ID_LABEL_AUDIO_FORMAT), GetDlgItem(window_, ID_LABEL_AUDIO_RATE)};
        audio_controls_ = {GetDlgItem(window_, ID_AUDIO_FORMAT), GetDlgItem(window_, ID_AUDIO_RATE)};
        settings_info_ = GetDlgItem(window_, ID_SETTINGS_INFO);
        github_link_ = GetDlgItem(window_, ID_GITHUB_LINK);
        SetWindowTextW(GetDlgItem(window_, ID_COMPAT_WARNING), (std::wstring(L"\x26A0") + L"不可用的組合").c_str());

        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t*>(L"Video"); TabCtrl_InsertItem(tab_, 0, &item);
        item.pszText = const_cast<wchar_t*>(L"Audio"); TabCtrl_InsertItem(tab_, 1, &item);
        item.pszText = const_cast<wchar_t*>(L"Settings"); TabCtrl_InsertItem(tab_, 2, &item);
        TabCtrl_SetCurSel(tab_, 0);
        item.pszText = const_cast<wchar_t*>(L"Encoding"); TabCtrl_InsertItem(video_tab_, 0, &item);
        item.pszText = const_cast<wchar_t*>(L"Color"); TabCtrl_InsertItem(video_tab_, 1, &item);
        item.pszText = const_cast<wchar_t*>(L"Frame structure"); TabCtrl_InsertItem(video_tab_, 2, &item);
        item.pszText = const_cast<wchar_t*>(L"Super resolution"); TabCtrl_InsertItem(video_tab_, 3, &item);
        TabCtrl_SetCurSel(video_tab_, 0);

        updating_command_ = true;
        add_combo(ID_LANGUAGE, {L"系統預設", L"繁體中文", L"簡體中文", L"日本語", L"English"}, language_index(settings_.language));
        const auto cpu_threads = cpu_thread_options(ui_language(settings_.language));
        add_combo(ID_CPU_THREADS, {cpu_threads[0].c_str(), cpu_threads[1].c_str(), cpu_threads[2].c_str(), cpu_threads[3].c_str()},
                  cpu_thread_mode_index(settings_.cpu_threads));
        add_combo(ID_BACKEND, {L"CPU", L"NVENC", L"QSV", L"AMF"}, settings_.backend == L"cpu" ? 0 : settings_.backend == L"qsv" ? 2 : settings_.backend == L"amf" ? 3 : 1);
        add_combo(ID_CODEC, {L"AVC (H.264)", L"HEVC (H.265)", L"AV1", L"VP9", L"ProRes"}, codec_index(settings_.codec));
        add_combo(ID_DEPTH, {L"8-bit", L"10-bit"}, settings_.bit_depth == 10 ? 1 : 0);
        add_combo(ID_PRESET, {L"P1", L"P2", L"P3", L"P4", L"P5", L"P6", L"P7"}, settings_.preset - 1);
        add_combo(ID_RATE, {L"CRF", L"QP", L"VBR"}, settings_.rate_control == L"crf" ? 0 : settings_.rate_control == L"vbr" ? 2 : 1);
        add_combo(ID_FRAME_MODE, {L"Automatic", L"Manual"}, settings_.frame_structure_mode == L"manual" ? 1 : 0);
        add_combo(ID_VSR_ENABLED, {L"Off", L"On"}, settings_.vsr_enabled ? 1 : 0);
        add_combo(ID_VSR_QUALITY, {L"1", L"2", L"3", L"4"}, std::clamp(settings_.vsr_quality - 1, 0, 3));
        add_combo(ID_ALPHA, {L"None", L"4-channel", L"Black/white mask"}, settings_.alpha_mode == L"rgba" ? 1 : settings_.alpha_mode == L"mask" ? 2 : 0);
        add_combo(ID_MASK_OUTPUT, {L"Stack x2", L"Separate"}, settings_.mask_output == L"separate" ? 1 : 0);
        add_combo(ID_CHROMA, {L"4:2:0", L"4:2:2", L"4:4:4"}, settings_.chroma == L"422" ? 1 : settings_.chroma == L"444" ? 2 : 0);
        add_combo(ID_COLORSPACE, {L"BT.601", L"BT.709", L"BT.2020"}, settings_.color_space == L"bt601" ? 0 : settings_.color_space == L"bt2020" ? 2 : 1);
        add_combo(ID_COLOR_RANGE, {L"TV", L"PC"}, settings_.color_range == L"pc" ? 1 : 0);
        add_combo(ID_AUDIO_FORMAT, {L"FLAC", L"WAV", L"None"}, settings_.audio_format == L"flac" ? 0 : settings_.audio_format == L"wav" ? 1 : 2);
        add_combo(ID_AUDIO_RATE, {L"Original", L"Hi-Res"}, settings_.audio_sample_rate == L"hires" ? 1 : 0);
        SetWindowTextW(GetDlgItem(window_, ID_QP), std::to_wstring(settings_.qp).c_str());
        SetWindowTextW(GetDlgItem(window_, ID_BITRATE), std::to_wstring(settings_.bitrate_kbps).c_str());
        SetWindowTextW(GetDlgItem(window_, ID_GOP), std::to_wstring(settings_.gop).c_str());
        SetWindowTextW(GetDlgItem(window_, ID_BFRAMES), std::to_wstring(settings_.b_frames).c_str());
        std::wostringstream scale_value; scale_value << std::fixed << std::setprecision(2) << settings_.vsr_scale;
        SetWindowTextW(GetDlgItem(window_, ID_VSR_SCALE), scale_value.str().c_str());
        SetWindowTextW(GetDlgItem(window_, ID_COMMAND_PREFIX), command_prefix(settings_).c_str());
        SetWindowTextW(GetDlgItem(window_, ID_COMMAND), (settings_.video_args.empty() ? editable_arguments(settings_) : settings_.video_args).c_str());
        SetWindowTextW(GetDlgItem(window_, ID_COMMAND_SUFFIX), command_suffix(settings_).c_str());
        update_command_display();
        rebuild_backend_options();
        update_controls();
        updating_command_ = false;
        apply_language();
        switch_tab(0);
        restore_cached_probe();
    }
    void switch_tab(int page) {
        const std::array<int, 9> video_bottom{ID_COMMAND, ID_REFRESH, ID_STATUS, ID_OPEN_LOG, ID_COMMAND_PREFIX, ID_COMMAND_SUFFIX,
                                               ID_TEST_REQUIREMENT, ID_COMMAND_HEADING, ID_VIDEO_TAB};
        for (const int id : video_bottom) ShowWindow(GetDlgItem(window_, id), page == 0 ? SW_SHOW : SW_HIDE);
        for (HWND control : audio_labels_) ShowWindow(control, page == 1 ? SW_SHOW : SW_HIDE);
        for (HWND control : audio_controls_) ShowWindow(control, page == 1 ? SW_SHOW : SW_HIDE);
        ShowWindow(audio_intro_, page == 1 ? SW_SHOW : SW_HIDE);
        ShowWindow(audio_help_, page == 1 ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(window_, ID_LABEL_LANGUAGE), page == 2 ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(window_, ID_LANGUAGE), page == 2 ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(window_, ID_LABEL_CPU_THREADS), page == 2 ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(window_, ID_CPU_THREADS), page == 2 ? SW_SHOW : SW_HIDE);
        ShowWindow(settings_info_, page == 2 ? SW_SHOW : SW_HIDE);
        active_tab_ = page;
        ShowWindow(github_link_, page == 2 ? SW_SHOW : SW_HIDE);
        update_video_subtab_visibility();
        update_compatibility_warning();
        rebuild_layout();
    }
    void switch_video_tab(int page) {
        active_video_tab_ = std::clamp(page, 0, 3);
        update_video_subtab_visibility();
        rebuild_layout();
    }
    bool restore_cached_probe() {
        sync_structured_settings();
        Settings candidate = settings_;
        if (!uses_nvenc_bridge(candidate)) candidate.video_args = edit_text(ID_COMMAND);
        ProbeResult cached{};
        if (!load_cached_probe(candidate, cached) || !cached.success) return false;
        current_command_tested_ = true;
        tested_signature_ = cached.signature;
        SetWindowTextW(GetDlgItem(window_, ID_STATUS), current_text().test_passed);
        update_compatibility_warning();
        return true;
    }
    void update_video_subtab_visibility() {
        const bool video_visible = active_tab_ == 0;
        const std::array<int, 12> encoding{ID_BACKEND, ID_CODEC, ID_DEPTH, ID_PRESET, ID_RATE, ID_QP, ID_BITRATE,
                                            ID_LABEL_BACKEND, ID_LABEL_CODEC, ID_LABEL_DEPTH, ID_LABEL_PRESET, ID_LABEL_RATE};
        const std::array<int, 2> encoding_more{ID_LABEL_QP, ID_LABEL_BITRATE};
        const std::array<int, 10> color{ID_ALPHA, ID_MASK_OUTPUT, ID_CHROMA, ID_COLORSPACE, ID_COLOR_RANGE,
                                         ID_LABEL_ALPHA, ID_LABEL_MASK_OUTPUT, ID_LABEL_CHROMA, ID_LABEL_COLORSPACE, ID_LABEL_COLOR_RANGE};
        const std::array<int, 6> frame_structure{ID_FRAME_MODE, ID_GOP, ID_BFRAMES, ID_LABEL_FRAME_MODE, ID_LABEL_GOP, ID_LABEL_BFRAMES};
        const std::array<int, 6> super_resolution{ID_VSR_ENABLED, ID_VSR_SCALE, ID_VSR_QUALITY,
                                                  ID_LABEL_VSR_ENABLED, ID_LABEL_VSR_SCALE, ID_LABEL_VSR_QUALITY};
        for (const int id : encoding) ShowWindow(GetDlgItem(window_, id), video_visible && active_video_tab_ == 0 ? SW_SHOW : SW_HIDE);
        for (const int id : encoding_more) ShowWindow(GetDlgItem(window_, id), video_visible && active_video_tab_ == 0 ? SW_SHOW : SW_HIDE);
        for (const int id : color) ShowWindow(GetDlgItem(window_, id), video_visible && active_video_tab_ == 1 ? SW_SHOW : SW_HIDE);
        for (const int id : frame_structure) ShowWindow(GetDlgItem(window_, id), video_visible && active_video_tab_ == 2 ? SW_SHOW : SW_HIDE);
        for (const int id : super_resolution) ShowWindow(GetDlgItem(window_, id), video_visible && active_video_tab_ == 3 ? SW_SHOW : SW_HIDE);
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
        updating_command_ = true;
        if (backend == 0) {
            if (combo_index(ID_CODEC) == 2)
                reset_combo(ID_PRESET, {fastest_13.c_str(), L"11", L"9", L"8", L"7", L"6", best_4.c_str()}, old_level - 1);
            else
                reset_combo(ID_PRESET, {L"ultrafast", L"superfast", L"veryfast", L"faster", L"fast", L"medium", L"slow"}, old_level - 1);
            reset_combo(ID_RATE, {L"CRF", L"QP", L"VBR"}, settings_.rate_control == L"crf" ? 0 : settings_.rate_control == L"vbr" ? 2 : 1);
        } else if (backend == 1) {
            reset_combo(ID_PRESET, {fastest_p1.c_str(), L"P2", L"P3", L"P4", L"P5", L"P6", best_p7.c_str()}, old_level - 1);
            reset_combo(ID_RATE, {L"CQ", L"QP", L"VBR"}, settings_.rate_control == L"crf" ? 0 : settings_.rate_control == L"vbr" ? 2 : 1);
        } else if (backend == 2) {
            reset_combo(ID_PRESET, {L"veryfast", L"faster", L"fast", L"medium", L"slow", L"slower", L"veryslow"}, old_level - 1);
            reset_combo(ID_RATE, {L"ICQ", L"QP", L"VBR"}, settings_.rate_control == L"crf" ? 0 : settings_.rate_control == L"vbr" ? 2 : 1);
        } else {
            const int amf_selected = old_level <= 2 ? 0 : old_level <= 5 ? 1 : 2;
            reset_combo(ID_PRESET, {options.speed, options.balanced, options.quality}, amf_selected);
            reset_combo(ID_RATE, {L"QVBR", L"QP", L"VBR"}, settings_.rate_control == L"crf" ? 0 : settings_.rate_control == L"vbr" ? 2 : 1);
        }
        updating_command_ = false;
    }
    void start_probe(bool force = false) {
        if (probe_running_) return;
        if (probe_thread_.joinable()) probe_thread_.join();
        sync_structured_settings();
        Settings candidate = settings_;
        if (!uses_nvenc_bridge(candidate)) candidate.video_args = edit_text(ID_COMMAND);
        ProbeResult cached{};
        if (!force && load_cached_probe(candidate, cached)) {
            auto* result = new ProbeResult(std::move(cached));
            PostMessageW(window_, WM_APP + 42, 0, reinterpret_cast<LPARAM>(result));
            return;
        }
        probe_running_ = true;
        probe_deadline_ = GetTickCount64() + encoder_test_timeout_ms(candidate);
        probe_seconds_remaining_ = -1;
        update_probe_countdown();
        SetTimer(window_, kProbeTimer, 100, nullptr);
        EnableWindow(GetDlgItem(window_, ID_REFRESH), FALSE);
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
    bool has_potential_compatibility_warning() const {
        if (current_command_tested_) return false;
        if (combo_index(ID_VSR_ENABLED) == 1 &&
            (combo_index(ID_ALPHA) == 2 || combo_index(ID_CODEC) != 1 || combo_index(ID_BACKEND) != 1)) return true;
        const auto& capability = codec_capability_from_index(combo_index(ID_CODEC));
        if (capability.cpu_only && combo_index(ID_BACKEND) != 0) return true;
        if (!capability.supports_10bit && combo_index(ID_DEPTH) == 1) return true;
        if (capability.forces_10bit && combo_index(ID_DEPTH) != 1) return true;
        if (combo_index(ID_ALPHA) == 1 && !capability.supports_rgba) return true;
        if (combo_index(ID_ALPHA) == 1 && !capability.rgba_supports_10bit && combo_index(ID_DEPTH) == 1) return true;
        if (combo_index(ID_ALPHA) == 1 && capability.forces_444_for_rgba && combo_index(ID_CHROMA) != 2) return true;
        if (!capability.supports_rate_control && combo_index(ID_RATE) != 0) return true;
        if (combo_index(ID_FRAME_MODE) == 1 && !capability.supports_b_frames && edit_number(ID_BFRAMES, 0) > 0) return true;
        return false;
    }
    void update_compatibility_warning() {
        if (!window_) return;
        ShowWindow(GetDlgItem(window_, ID_COMPAT_WARNING), active_tab_ == 0 && has_potential_compatibility_warning() ? SW_SHOW : SW_HIDE);
    }
    void update_probe_countdown() {
        if (!probe_running_ || !window_) return;
        const ULONGLONG now = GetTickCount64();
        const int remaining = now >= probe_deadline_ ? 0 : static_cast<int>((probe_deadline_ - now + 999) / 1000);
        if (remaining == probe_seconds_remaining_) return;
        probe_seconds_remaining_ = remaining;
        SetWindowTextW(GetDlgItem(window_, ID_STATUS), (std::wstring(current_text().testing) + L" " + std::to_wstring(remaining)).c_str());
    }
    void update_controls() {
        update_compatibility_warning();
    }
    int combo_index(int id) const { return static_cast<int>(ComboBox_GetCurSel(GetDlgItem(window_, id))); }
    std::wstring combo_text(int id) const {
        wchar_t text[64]{}; GetWindowTextW(GetDlgItem(window_, id), text, static_cast<int>(std::size(text))); return text;
    }
    int edit_number(int id, int fallback) const {
        wchar_t text[32]{}; GetWindowTextW(GetDlgItem(window_, id), text, static_cast<int>(std::size(text)));
        try { return std::stoi(text); } catch (...) { return fallback; }
    }
    double edit_double(int id, double fallback) const {
        wchar_t text[32]{}; GetWindowTextW(GetDlgItem(window_, id), text, static_cast<int>(std::size(text)));
        try { return std::stod(text); } catch (...) { return fallback; }
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
        settings_.codec = codec_capability_from_index(combo_index(ID_CODEC)).key;
        settings_.bit_depth = combo_index(ID_DEPTH) == 1 ? 10 : 8;
        settings_.chroma = combo_index(ID_CHROMA) == 1 ? L"422" : combo_index(ID_CHROMA) == 2 ? L"444" : L"420";
        settings_.alpha_mode = combo_index(ID_ALPHA) == 1 ? L"rgba" : combo_index(ID_ALPHA) == 2 ? L"mask" : L"none";
        settings_.mask_output = combo_index(ID_MASK_OUTPUT) == 1 ? L"separate" : L"stacked";
        settings_.color_space = combo_index(ID_COLORSPACE) == 0 ? L"bt601" : combo_index(ID_COLORSPACE) == 2 ? L"bt2020" : L"bt709";
        settings_.color_range = combo_index(ID_COLOR_RANGE) == 1 ? L"pc" : L"tv";
        settings_.preset = combo_index(ID_BACKEND) == 3 ? (combo_index(ID_PRESET) == 0 ? 1 : combo_index(ID_PRESET) == 1 ? 4 : 7) : combo_index(ID_PRESET) + 1;
        settings_.rate_control = combo_index(ID_RATE) == 0 ? L"crf" : combo_index(ID_RATE) == 1 ? L"qp" : L"vbr";
        settings_.qp = std::clamp(edit_number(ID_QP, 20), 0, 51);
        settings_.bitrate_kbps = std::clamp(edit_number(ID_BITRATE, 20000), 100, 1000000);
        settings_.frame_structure_mode = combo_index(ID_FRAME_MODE) == 1 ? L"manual" : L"auto";
        settings_.gop = std::clamp(edit_number(ID_GOP, 120), 1, 10000);
        settings_.b_frames = std::clamp(edit_number(ID_BFRAMES, 3), 0, 16);
        settings_.cpu_threads = cpu_thread_mode_key(combo_index(ID_CPU_THREADS));
        settings_.audio_format = combo_index(ID_AUDIO_FORMAT) == 0 ? L"flac" : combo_index(ID_AUDIO_FORMAT) == 1 ? L"wav" : L"none";
        settings_.audio_sample_rate = combo_index(ID_AUDIO_RATE) == 1 ? L"hires" : L"original";
        settings_.audio_bit_depth = settings_.audio_sample_rate == L"hires" ? L"24" : L"original";
        settings_.vsr_enabled = combo_index(ID_VSR_ENABLED) == 1;
        settings_.vsr_scale = std::clamp(edit_double(ID_VSR_SCALE, 2.0), 1.0, 4.0);
        settings_.vsr_quality = std::clamp(combo_index(ID_VSR_QUALITY) + 1, 1, 4);
        normalize_codec_settings(settings_);
    }
    void update_command_display() {
        if (uses_nvenc_bridge(settings_)) {
            const auto ffmpeg_path = resolve_executable(settings_.ffmpeg);
            const auto nvenc_path = resolve_nvenc(ffmpeg_path);
            const auto bridge_path = local_data_dir() / L"mmd2ffmpeg_vsr_bridge.exe";
            SetWindowTextW(GetDlgItem(window_, ID_COMMAND_PREFIX), L"");
            auto display = build_vsr_command(settings_, 1920, 1080, 32, ffmpeg_path, nvenc_path, bridge_path,
                                             L"{output}");
            replace_all(display, L"--width 1920 --height 1080", L"--width {width} --height {height}");
            SetWindowTextW(GetDlgItem(window_, ID_COMMAND), display.c_str());
            SetWindowTextW(GetDlgItem(window_, ID_COMMAND_SUFFIX), L"");
            SendMessageW(GetDlgItem(window_, ID_COMMAND), EM_SETREADONLY, TRUE, 0);
            return;
        }
        SendMessageW(GetDlgItem(window_, ID_COMMAND), EM_SETREADONLY, FALSE, 0);
        SetWindowTextW(GetDlgItem(window_, ID_COMMAND_PREFIX), command_prefix(settings_).c_str());
        SetWindowTextW(GetDlgItem(window_, ID_COMMAND),
                       (settings_.video_args.empty() ? editable_arguments(settings_) : settings_.video_args).c_str());
        SetWindowTextW(GetDlgItem(window_, ID_COMMAND_SUFFIX), command_suffix(settings_).c_str());
    }
    struct LayoutItem {
        int id;
        RECT rectangle;
        bool scrolls;
    };
    int dlu_x(int value) const {
        RECT rectangle{0, 0, value, 0};
        MapDialogRect(window_, &rectangle);
        return rectangle.right;
    }
    int dlu_y(int value) const {
        RECT rectangle{0, 0, 0, value};
        MapDialogRect(window_, &rectangle);
        return rectangle.bottom;
    }
    int text_width(int id, int minimum) const {
        HWND control = GetDlgItem(window_, id);
        const std::wstring text = edit_text(id);
        HDC dc = GetDC(window_);
        HFONT font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
        HGDIOBJ previous = font ? SelectObject(dc, font) : nullptr;
        SIZE size{};
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
        if (previous) SelectObject(dc, previous);
        ReleaseDC(window_, dc);
        return std::max(minimum, static_cast<int>(size.cx) + dlu_x(4));
    }
    int maximum_text_width(std::initializer_list<int> ids, int minimum) const {
        int width = minimum;
        for (const int id : ids) width = std::max(width, text_width(id, minimum));
        return width;
    }
    int text_height(int id, int width, int minimum) const {
        HWND control = GetDlgItem(window_, id);
        const std::wstring text = edit_text(id);
        HDC dc = GetDC(window_);
        HFONT font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
        HGDIOBJ previous = font ? SelectObject(dc, font) : nullptr;
        RECT rectangle{0, 0, std::max(1, width), 0};
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rectangle,
                  DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL | DT_CALCRECT | DT_NOPREFIX);
        if (previous) SelectObject(dc, previous);
        ReleaseDC(window_, dc);
        return std::max(minimum, static_cast<int>(rectangle.bottom));
    }
    void add_layout(int id, int left, int top, int width, int height, bool scrolls = true) {
        layouts_.push_back({id, {left, top, left + std::max(1, width), top + std::max(1, height)}, scrolls});
    }
    void apply_layout() {
        for (const auto& item : layouts_) {
            const int top = item.rectangle.top - (item.scrolls ? scroll_offset_ : 0);
            SetWindowPos(GetDlgItem(window_, item.id), nullptr, item.rectangle.left, top,
                         item.rectangle.right - item.rectangle.left, item.rectangle.bottom - item.rectangle.top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
        }
    }
    void rebuild_layout() {
        if (!window_) return;
        RECT client{};
        GetClientRect(window_, &client);
        if (client.right <= 0 || client.bottom <= 0) return;
        layouts_.clear();

        const int margin_x = dlu_x(4);
        const int margin_y = dlu_y(4);
        const int right = std::max(margin_x + 1, static_cast<int>(client.right) - margin_x);
        const int full_width = right - margin_x;
        const int tab_height = dlu_y(12);
        const int row_height = dlu_y(16);
        const int label_height = dlu_y(12);
        const int combo_height = dlu_y(80);
        const int edit_height = dlu_y(14);
        const int button_height = dlu_y(16);
        const int gap = dlu_y(2);
        const int field_gap = dlu_x(4);
        add_layout(ID_TAB, margin_x, margin_y, full_width, tab_height, false);
        int y = margin_y + tab_height;

        const auto add_form_row = [&](int label, int control, int label_width, int& row_y) {
            const int field_left = margin_x + label_width + field_gap;
            add_layout(label, margin_x, row_y + dlu_y(2), label_width, label_height);
            add_layout(control, field_left, row_y, std::max(1, right - field_left), combo_height);
            row_y += row_height;
        };
        if (active_tab_ == 0) {
            const int inner_gap = dlu_y(1);
            const int inner_row_height = dlu_y(14);
            const int inner_height = tab_height + inner_gap + inner_row_height * 4;
            add_layout(ID_VIDEO_TAB, margin_x, y, full_width, tab_height);
            int inner_y = y + tab_height + inner_gap;
            const int middle = margin_x + full_width / 2;
            const int column_gap = dlu_x(4);
            const auto add_video_field = [&](int label, int control, int left, int column_right, int row_y) {
                const int label_width = std::min(text_width(label, dlu_x(42)), std::max(1, (column_right - left) / 2));
                const int field_left = left + label_width + field_gap;
                add_layout(label, left, row_y + dlu_y(1), label_width, label_height);
                const int control_height = control == ID_QP || control == ID_BITRATE || control == ID_GOP ||
                                           control == ID_BFRAMES || control == ID_VSR_SCALE ? edit_height : combo_height;
                add_layout(control, field_left, row_y, std::max(1, column_right - field_left), control_height);
            };
            const auto add_video_pair = [&](int left_label, int left_control, int right_label, int right_control, int& row_y) {
                add_video_field(left_label, left_control, margin_x, middle - column_gap, row_y);
                add_video_field(right_label, right_control, middle + column_gap, right, row_y);
                row_y += inner_row_height;
            };
            if (active_video_tab_ == 0) {
                add_video_pair(ID_LABEL_BACKEND, ID_BACKEND, ID_LABEL_CODEC, ID_CODEC, inner_y);
                add_video_pair(ID_LABEL_DEPTH, ID_DEPTH, ID_LABEL_PRESET, ID_PRESET, inner_y);
                add_video_pair(ID_LABEL_RATE, ID_RATE, ID_LABEL_QP, ID_QP, inner_y);
                add_video_field(ID_LABEL_BITRATE, ID_BITRATE, margin_x, right, inner_y);
            } else {
                if (active_video_tab_ == 1) {
                    add_video_pair(ID_LABEL_ALPHA, ID_ALPHA, ID_LABEL_MASK_OUTPUT, ID_MASK_OUTPUT, inner_y);
                    add_video_pair(ID_LABEL_CHROMA, ID_CHROMA, ID_LABEL_COLORSPACE, ID_COLORSPACE, inner_y);
                    add_video_field(ID_LABEL_COLOR_RANGE, ID_COLOR_RANGE, margin_x, right, inner_y);
                } else if (active_video_tab_ == 2) {
                    add_video_field(ID_LABEL_FRAME_MODE, ID_FRAME_MODE, margin_x, right, inner_y);
                    inner_y += inner_row_height;
                    add_video_pair(ID_LABEL_GOP, ID_GOP, ID_LABEL_BFRAMES, ID_BFRAMES, inner_y);
                } else {
                    add_video_field(ID_LABEL_VSR_ENABLED, ID_VSR_ENABLED, margin_x, right, inner_y);
                    inner_y += inner_row_height;
                    add_video_pair(ID_LABEL_VSR_SCALE, ID_VSR_SCALE, ID_LABEL_VSR_QUALITY, ID_VSR_QUALITY, inner_y);
                }
            }
            y += inner_height + inner_gap;
            const int warning_width = text_width(ID_COMPAT_WARNING, dlu_x(60));
            add_layout(ID_COMMAND_HEADING, margin_x, y, std::max(1, full_width - warning_width), label_height);
            add_layout(ID_COMPAT_WARNING, right - warning_width, y, warning_width, label_height);
            y += label_height + dlu_y(2);
            const int prefix_height = text_height(ID_COMMAND_PREFIX, full_width, dlu_y(24));
            add_layout(ID_COMMAND_PREFIX, margin_x, y, full_width, prefix_height);
            y += prefix_height + dlu_y(2);
            const int command_height = edit_height * 4;
            add_layout(ID_COMMAND, margin_x, y, full_width, command_height);
            y += command_height + dlu_y(2);
            const int suffix_height = text_height(ID_COMMAND_SUFFIX, full_width, dlu_y(12));
            add_layout(ID_COMMAND_SUFFIX, margin_x, y, full_width, suffix_height);
            y += suffix_height + dlu_y(1);
            const int status_height = label_height * 2;
            add_layout(ID_STATUS, margin_x, y, full_width, status_height);
            y += status_height;
            const int open_log_width = text_width(ID_OPEN_LOG, dlu_x(58));
            const int test_width = text_width(ID_REFRESH, dlu_x(54));
            const int open_log_left = right - open_log_width;
            const int test_left = open_log_left - field_gap - test_width;
            const int requirement_width = std::max(1, test_left - field_gap - margin_x);
            const int requirement_height = text_height(ID_TEST_REQUIREMENT, requirement_width, label_height);
            const bool stack_actions = requirement_width < dlu_x(88) || requirement_height > button_height * 2;
            if (stack_actions) {
                const int full_requirement_height = text_height(ID_TEST_REQUIREMENT, full_width, label_height);
                add_layout(ID_TEST_REQUIREMENT, margin_x, y, full_width, full_requirement_height);
                y += full_requirement_height + gap;
                add_layout(ID_REFRESH, test_left, y, test_width, button_height);
                add_layout(ID_OPEN_LOG, open_log_left, y, open_log_width, button_height);
                y += button_height + margin_y;
            } else {
                const int action_height = std::max(button_height, requirement_height);
                add_layout(ID_TEST_REQUIREMENT, margin_x, y + (action_height - requirement_height) / 2,
                           requirement_width, requirement_height);
                add_layout(ID_REFRESH, test_left, y + (action_height - button_height) / 2, test_width, button_height);
                add_layout(ID_OPEN_LOG, open_log_left, y + (action_height - button_height) / 2, open_log_width, button_height);
                y += action_height + margin_y;
            }
        } else if (active_tab_ == 1) {
            const int intro_height = text_height(ID_AUDIO_INTRO, full_width, dlu_y(12));
            add_layout(ID_AUDIO_INTRO, margin_x, y, full_width, intro_height);
            y += intro_height + gap;
            const int label_width = maximum_text_width({ID_LABEL_AUDIO_FORMAT, ID_LABEL_AUDIO_RATE}, dlu_x(82));
            add_form_row(ID_LABEL_AUDIO_FORMAT, ID_AUDIO_FORMAT, label_width, y);
            const int help_height = text_height(ID_AUDIO_HELP, full_width, dlu_y(12));
            add_layout(ID_AUDIO_HELP, margin_x, y, full_width, help_height);
            y += help_height + gap;
            add_form_row(ID_LABEL_AUDIO_RATE, ID_AUDIO_RATE, label_width, y);
            y += margin_y;
        } else {
            const int label_width = maximum_text_width({ID_LABEL_LANGUAGE, ID_LABEL_CPU_THREADS}, dlu_x(82));
            add_form_row(ID_LABEL_LANGUAGE, ID_LANGUAGE, label_width, y);
            add_form_row(ID_LABEL_CPU_THREADS, ID_CPU_THREADS, label_width, y);
            const int info_height = text_height(ID_SETTINGS_INFO, full_width, dlu_y(12));
            add_layout(ID_SETTINGS_INFO, margin_x, y, full_width, info_height);
            y += info_height + gap;
            add_layout(ID_GITHUB_LINK, margin_x, y, full_width, edit_height);
            y += edit_height + margin_y;
        }
        content_height_ = y;
        update_vertical_scroll();
    }
    void update_vertical_scroll() {
        if (layouts_.empty()) return;
        RECT client{};
        GetClientRect(window_, &client);
        const int client_height = static_cast<int>(client.bottom);
        const int maximum = std::max(0, content_height_ - client_height);
        scroll_offset_ = std::clamp(scroll_offset_, 0, maximum);
        SCROLLINFO scroll_info{sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, content_height_ - 1),
                               static_cast<UINT>(std::max(0, client_height)), scroll_offset_, 0};
        SetScrollInfo(window_, SB_VERT, &scroll_info, TRUE);
        ShowScrollBar(window_, SB_VERT, maximum > 0);
        apply_layout();
    }
    void scroll_to(int requested_offset) {
        RECT client{};
        GetClientRect(window_, &client);
        const int client_height = static_cast<int>(client.bottom);
        const int maximum = std::max(0, content_height_ - client_height);
        scroll_offset_ = std::clamp(requested_offset, 0, maximum);
        SCROLLINFO scroll_info{sizeof(SCROLLINFO), SIF_POS, 0, 0, 0, scroll_offset_, 0};
        SetScrollInfo(window_, SB_VERT, &scroll_info, TRUE);
        apply_layout();
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
    void apply_tab_language() {
        if (!tab_) return;
        const auto& text = tab_ui_strings(ui_language(settings_.language));
        const wchar_t* tabs[] = {text.video, text.audio, text.settings};
        for (int index = 0; index < 3; ++index) {
            TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = const_cast<wchar_t*>(tabs[index]); TabCtrl_SetItem(tab_, index, &item);
        }
        const wchar_t* video_tabs[] = {text.encoding, text.color, text.frame_structure,
                                       super_resolution_label(ui_language(settings_.language))};
        for (int index = 0; index < 4; ++index) {
            TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = const_cast<wchar_t*>(video_tabs[index]); TabCtrl_SetItem(video_tab_, index, &item);
        }
        SetWindowTextW(audio_labels_[0], text.audio_format);
        SetWindowTextW(audio_labels_[1], text.sample_rate_depth);
        const int format = combo_index(ID_AUDIO_FORMAT), rate = combo_index(ID_AUDIO_RATE);
        reset_combo(ID_AUDIO_FORMAT, {L"FLAC", L"WAV", L"None"}, format);
        reset_combo(ID_AUDIO_RATE, {text.original, text.hi_res}, rate);
        SetWindowTextW(audio_intro_, text.audio_intro);
        SetWindowTextW(audio_help_, text.hi_res_help);
        const auto labels = vsr_labels(ui_language(settings_.language));
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_VSR_ENABLED), labels[0]);
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_VSR_SCALE), labels[1]);
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_VSR_QUALITY), labels[2]);
        const std::wstring info = std::wstring(L"MMD2FFMPEG\r\n") + text.version + L": 0.2.0\r\n" + text.author + L": XPRAMT";
        SetWindowTextW(settings_info_, info.c_str());
    }
    void apply_language() {
        const auto& text = current_text();
        const auto& options = ui_options(ui_language(settings_.language));
        const int backend = combo_index(ID_BACKEND);
        reset_combo(ID_BACKEND, {L"CPU", L"NVENC", L"QSV", L"AMF"}, backend);
        const int codec = combo_index(ID_CODEC);
        reset_combo(ID_CODEC, {L"AVC (H.264)", L"HEVC (H.265)", L"AV1", L"VP9", L"ProRes"}, codec);
        const int frame_structure_mode = combo_index(ID_FRAME_MODE);
        reset_combo(ID_FRAME_MODE, {options.automatic, options.manual}, frame_structure_mode);
        const int cpu_thread_mode = combo_index(ID_CPU_THREADS);
        const auto cpu_threads = cpu_thread_options(ui_language(settings_.language));
        reset_combo(ID_CPU_THREADS, {cpu_threads[0].c_str(), cpu_threads[1].c_str(), cpu_threads[2].c_str(), cpu_threads[3].c_str()}, cpu_thread_mode);
        rebuild_backend_options();
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_LANGUAGE), text.language);
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_CPU_THREADS), text.cpu_threads);
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_BACKEND), text.encoder);
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_CODEC), text.codec);
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_DEPTH), text.bit_depth);
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_PRESET), text.preset);
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_RATE), text.rate_control);
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_QP), text.quality);
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_BITRATE), text.bitrate);
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_ALPHA), text.alpha);
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_MASK_OUTPUT), text.mask_output);
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_CHROMA), text.chroma);
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_COLORSPACE), text.color_space);
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_COLOR_RANGE), text.color_range);
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_GOP), text.gop);
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_BFRAMES), text.b_frames);
        SetWindowTextW(GetDlgItem(window_, ID_LABEL_FRAME_MODE), text.frame_structure_mode);
        SetWindowTextW(GetDlgItem(window_, ID_COMMAND_HEADING), text.command_heading);
        SetWindowTextW(GetDlgItem(window_, ID_TEST_REQUIREMENT), text.test_required);
        SetWindowTextW(GetDlgItem(window_, ID_REFRESH), text.test_button);
        SetWindowTextW(GetDlgItem(window_, ID_OPEN_LOG), text.open_log_button);
        apply_tab_language();
        apply_tooltips();
        if (probe_running_) {
            probe_seconds_remaining_ = -1;
            update_probe_countdown();
        } else {
            SetWindowTextW(GetDlgItem(window_, ID_STATUS), current_command_tested_ ? text.test_passed : text.not_tested);
        }
        rebuild_layout();
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
        if (id != ID_AUDIO_FORMAT && id != ID_AUDIO_RATE) {
            current_command_tested_ = false;
            tested_signature_.clear();
        }
        update_controls();
        if (id == ID_BACKEND || id == ID_CODEC) rebuild_backend_options();
        if (id != ID_COMMAND) {
            sync_structured_settings();
            if (!uses_nvenc_bridge(settings_)) settings_.video_args.clear();
            updating_command_ = true;
            update_command_display();
            updating_command_ = false;
        }
        restore_cached_probe();
        if (probe_running_) {
            probe_seconds_remaining_ = -1;
            update_probe_countdown();
        } else {
            SetWindowTextW(GetDlgItem(window_, ID_STATUS), current_command_tested_ ? current_text().test_passed : current_text().not_tested);
        }
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
            KillTimer(window, kProbeTimer);
            self->probe_running_ = false;
            self->probe_seconds_remaining_ = -1;
            self->sync_structured_settings();
            Settings current = self->settings_;
            if (!uses_nvenc_bridge(current)) current.video_args = self->edit_text(ID_COMMAND);
            self->current_command_tested_ = result->success && result->signature == command_test_signature(current);
            self->tested_signature_ = self->current_command_tested_ ? result->signature : L"";
            std::wstring status;
            const auto& text = self->current_text();
            if (self->current_command_tested_) status = text.test_passed;
            else if (result->signature != command_test_signature(current)) status = text.settings_changed;
            else status = std::wstring(text.test_failed) + result->message;
            if (status.size() > 180) status.resize(180);
            SetWindowTextW(GetDlgItem(window, ID_STATUS), status.c_str());
            self->update_compatibility_warning();
            EnableWindow(GetDlgItem(window, ID_REFRESH), TRUE);
            delete result;
            return TRUE;
        }
        else if (message == WM_TIMER && self && wparam == kProbeTimer) {
            self->update_probe_countdown();
            return TRUE;
        }
        else if (message == WM_SIZE && self) {
            self->rebuild_layout();
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
        else if (message == WM_NOTIFY && self) {
            const auto* header = reinterpret_cast<NMHDR*>(lparam);
            if (header->hwndFrom == self->tab_ && header->code == TCN_SELCHANGE) { self->switch_tab(TabCtrl_GetCurSel(self->tab_)); return TRUE; }
            if ((header->hwndFrom == self->video_tab_ || header->idFrom == ID_VIDEO_TAB) && header->code == TCN_SELCHANGE) {
                self->switch_video_tab(TabCtrl_GetCurSel(self->video_tab_));
                return TRUE;
            }
        }
        else if (message == WM_CTLCOLOREDIT && self && reinterpret_cast<HWND>(lparam) == self->github_link_) {
            SetTextColor(reinterpret_cast<HDC>(wparam), RGB(0, 102, 204));
            SetBkColor(reinterpret_cast<HDC>(wparam), GetSysColor(COLOR_3DFACE));
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_3DFACE));
        }
        else if (message == WM_COMMAND && self && LOWORD(wparam) == ID_REFRESH && HIWORD(wparam) == BN_CLICKED) {
            self->start_probe(true);
            return TRUE;
        }
        else if (message == WM_COMMAND && self && LOWORD(wparam) == ID_OPEN_LOG && HIWORD(wparam) == BN_CLICKED) {
            self->open_log_folder();
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
    HWND tooltip_ = nullptr;
    HWND tab_ = nullptr;
    HWND video_tab_ = nullptr;
    HWND settings_info_ = nullptr;
    HWND github_link_ = nullptr;
    HWND audio_intro_ = nullptr;
    HWND audio_help_ = nullptr;
    std::array<HWND, 2> audio_labels_{};
    std::array<HWND, 2> audio_controls_{};
    int active_tab_ = 0;
    int active_video_tab_ = 0;
    Settings settings_{};
    bool dirty_ = false;
    bool updating_command_ = false;
    bool tooltips_initialized_ = false;
    std::atomic<bool> alive_{true};
    bool probe_running_ = false;
    static constexpr UINT_PTR kProbeTimer = 71;
    ULONGLONG probe_deadline_ = 0;
    int probe_seconds_remaining_ = -1;
    bool current_command_tested_ = false;
    std::wstring tested_signature_;
    std::thread probe_thread_;
    std::vector<LayoutItem> layouts_;
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
