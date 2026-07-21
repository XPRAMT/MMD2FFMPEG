#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl_core.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"MMDLocaleLauncherSetupWindow";
constexpr wchar_t kAppName[] = L"MMD Locale Launcher";
constexpr wchar_t kProgId[] = L"MMDLocaleLauncher.PMM";
constexpr int kIdNtleasEdit = 1001;
constexpr int kIdMmdEdit = 1002;
constexpr int kIdBrowseNtleas = 1003;
constexpr int kIdBrowseMmd = 1004;
constexpr int kIdRegisterPmm = 1005;
constexpr int kIdSave = 1006;
constexpr int kIdCancel = 1007;
constexpr COLORREF kWindowBackground = RGB(18, 18, 18);
constexpr COLORREF kControlBackground = RGB(36, 36, 36);
constexpr COLORREF kControlBorder = RGB(82, 82, 82);
constexpr COLORREF kTextPrimary = RGB(245, 245, 245);
constexpr COLORREF kTextSecondary = RGB(185, 185, 185);
constexpr COLORREF kPrimaryButton = RGB(0, 120, 212);
constexpr COLORREF kPrimaryButtonHot = RGB(28, 151, 234);

struct Config {
    std::wstring ntleas_path;
    std::wstring mmd_path;
};

struct SetupState {
    Config config;
    bool saved = false;
    bool register_pmm = false;
    HFONT font = nullptr;
    HWND ntleas_edit = nullptr;
    HWND mmd_edit = nullptr;
    HWND register_checkbox = nullptr;
};

bool is_existing_file(const std::wstring& path);

HBRUSH window_brush() {
    static HBRUSH brush = CreateSolidBrush(kWindowBackground);
    return brush;
}

HBRUSH control_brush() {
    static HBRUSH brush = CreateSolidBrush(kControlBackground);
    return brush;
}

void enable_dark_title_bar(HWND window) {
    const BOOL enabled = TRUE;
    constexpr DWORD kUseImmersiveDarkMode = 20;
    DwmSetWindowAttribute(window, kUseImmersiveDarkMode, &enabled, sizeof(enabled));
}

void apply_mmd_icon(HWND window, const std::wstring& mmd_path) {
    if (!is_existing_file(mmd_path)) {
        return;
    }
    HICON large_icon = nullptr;
    HICON small_icon = nullptr;
    if (ExtractIconExW(mmd_path.c_str(), 0, &large_icon, &small_icon, 1) == 0) {
        return;
    }
    if (large_icon != nullptr) {
        const auto old_icon = reinterpret_cast<HICON>(SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(large_icon)));
        if (old_icon != nullptr && old_icon != large_icon) {
            DestroyIcon(old_icon);
        }
    }
    if (small_icon != nullptr) {
        const auto old_icon = reinterpret_cast<HICON>(SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon)));
        if (old_icon != nullptr && old_icon != small_icon) {
            DestroyIcon(old_icon);
        }
    }
}

void release_window_icons(HWND window) {
    const auto large_icon = reinterpret_cast<HICON>(SendMessageW(window, WM_GETICON, ICON_BIG, 0));
    const auto small_icon = reinterpret_cast<HICON>(SendMessageW(window, WM_GETICON, ICON_SMALL, 0));
    if (large_icon != nullptr) {
        DestroyIcon(large_icon);
    }
    if (small_icon != nullptr && small_icon != large_icon) {
        DestroyIcon(small_icon);
    }
}

void show_error(HWND owner, const std::wstring& text) {
    MessageBoxW(owner, text.c_str(), kAppName, MB_OK | MB_ICONERROR);
}

void show_info(HWND owner, const std::wstring& text) {
    MessageBoxW(owner, text.c_str(), kAppName, MB_OK | MB_ICONINFORMATION);
}

std::wstring get_module_path() {
    std::vector<wchar_t> buffer(260);
    for (;;) {
        const DWORD copied = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            return {};
        }
        if (copied < buffer.size() - 1) {
            return std::wstring(buffer.data(), copied);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::filesystem::path config_path() {
    const std::wstring launcher = get_module_path();
    if (launcher.empty()) {
        return L"config.ini";
    }
    return std::filesystem::path(launcher).parent_path() / L"config.ini";
}

std::string utf8_from_wide(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring wide_from_utf8(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::optional<Config> read_config() {
    const auto path = config_path();
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.erase(0, 3);
    }
    const std::wstring wide = wide_from_utf8(text);
    if (wide.empty() && !text.empty()) {
        return std::nullopt;
    }

    Config config;
    size_t start = 0;
    while (start <= wide.size()) {
        const size_t end = wide.find_first_of(L"\r\n", start);
        const std::wstring_view line(wide.data() + start, (end == std::wstring::npos ? wide.size() : end) - start);
        const size_t separator = line.find(L'=');
        if (separator != std::wstring_view::npos) {
            const std::wstring key(line.substr(0, separator));
            const std::wstring value(line.substr(separator + 1));
            if (key == L"ntleas") {
                config.ntleas_path = value;
            } else if (key == L"mmd") {
                config.mmd_path = value;
            }
        }
        if (end == std::wstring::npos) {
            break;
        }
        start = end + 1;
        if (wide[end] == L'\r' && start < wide.size() && wide[start] == L'\n') {
            ++start;
        }
    }
    if (config.ntleas_path.empty() || config.mmd_path.empty()) {
        return std::nullopt;
    }
    return config;
}

bool save_config(const Config& config, std::wstring& error) {
    try {
        const auto path = config_path();
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = L"無法建立設定檔：\n" + path.wstring();
            return false;
        }
        const std::string text = "\xEF\xBB\xBFntleas=" + utf8_from_wide(config.ntleas_path) + "\r\nmmd=" + utf8_from_wide(config.mmd_path) + "\r\n";
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!file) {
            error = L"無法寫入設定檔：\n" + path.wstring();
            return false;
        }
        return true;
    } catch (const std::filesystem::filesystem_error& exception) {
        error = L"無法建立設定資料夾：\n" + std::wstring(exception.what(), exception.what() + std::strlen(exception.what()));
        return false;
    }
}

bool is_existing_file(const std::wstring& path) {
    std::error_code error;
    return !path.empty() && std::filesystem::is_regular_file(std::filesystem::path(path), error);
}

bool is_valid_config(const Config& config) {
    return is_existing_file(config.ntleas_path) && is_existing_file(config.mmd_path);
}

std::wstring read_window_text(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(window, value.data(), length + 1);
    value.resize(static_cast<size_t>(length));
    return value;
}

HFONT create_ui_font(HWND window) {
    const UINT dpi = GetDpiForWindow(window);
    return CreateFontW(-MulDiv(9, static_cast<int>(dpi == 0 ? 96 : dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void set_default_font(HWND window) {
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
}

HWND create_control(DWORD style, int id, HWND parent, const wchar_t* class_name, const wchar_t* text) {
    HWND control = CreateWindowExW(0, class_name, text, WS_CHILD | WS_VISIBLE | style,
        0, 0, 1, 1, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    set_default_font(control);
    return control;
}

void draw_button(const DRAWITEMSTRUCT& item, bool primary) {
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;
    COLORREF background = primary ? kPrimaryButton : kControlBackground;
    if (primary && !disabled && !pressed) {
        background = kPrimaryButtonHot;
    }
    if (pressed) {
        background = primary ? RGB(0, 94, 166) : RGB(56, 56, 56);
    }
    HBRUSH background_brush = CreateSolidBrush(disabled ? RGB(48, 48, 48) : background);
    FillRect(item.hDC, &item.rcItem, background_brush);
    DeleteObject(background_brush);
    HBRUSH border_brush = CreateSolidBrush(focused ? RGB(117, 192, 255) : kControlBorder);
    FrameRect(item.hDC, &item.rcItem, border_brush);
    DeleteObject(border_brush);
    wchar_t text[256]{};
    GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, disabled ? RGB(120, 120, 120) : kTextPrimary);
    RECT text_rect = item.rcItem;
    DrawTextW(item.hDC, text, -1, &text_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void draw_checkbox(const DRAWITEMSTRUCT& item, bool checked) {
    FillRect(item.hDC, &item.rcItem, window_brush());
    const int dpi = GetDpiForWindow(item.hwndItem);
    const int box_size = MulDiv(16, static_cast<int>(dpi == 0 ? 96 : dpi), 96);
    const int left = item.rcItem.left;
    const int top = item.rcItem.top + (item.rcItem.bottom - item.rcItem.top - box_size) / 2;
    RECT box{left, top, left + box_size, top + box_size};
    HBRUSH box_brush = CreateSolidBrush(checked ? kPrimaryButton : kControlBackground);
    FillRect(item.hDC, &box, box_brush);
    DeleteObject(box_brush);
    HBRUSH border_brush = CreateSolidBrush((item.itemState & ODS_FOCUS) ? RGB(117, 192, 255) : kControlBorder);
    FrameRect(item.hDC, &box, border_brush);
    DeleteObject(border_brush);
    if (checked) {
        HPEN pen = CreatePen(PS_SOLID, std::max(1, MulDiv(2, static_cast<int>(dpi == 0 ? 96 : dpi), 96)), kTextPrimary);
        HGDIOBJ old_pen = SelectObject(item.hDC, pen);
        POINT points[3]{
            {box.left + box_size / 5, box.top + box_size / 2},
            {box.left + box_size / 2 - 1, box.bottom - box_size / 4},
            {box.right - box_size / 6, box.top + box_size / 4}
        };
        Polyline(item.hDC, points, static_cast<int>(std::size(points)));
        SelectObject(item.hDC, old_pen);
        DeleteObject(pen);
    }
    wchar_t text[512]{};
    GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
    RECT text_rect{box.right + MulDiv(8, static_cast<int>(dpi == 0 ? 96 : dpi), 96), item.rcItem.top, item.rcItem.right, item.rcItem.bottom};
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, kTextPrimary);
    DrawTextW(item.hDC, text, -1, &text_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

int scale_for(HWND window, int value) {
    const UINT dpi = GetDpiForWindow(window);
    return MulDiv(value, static_cast<int>(dpi == 0 ? 96 : dpi), 96);
}

void apply_ui_font(HWND window, SetupState* state) {
    if (state->font != nullptr) {
        DeleteObject(state->font);
    }
    state->font = create_ui_font(window);
    for (const int id : {1100, 1101, kIdNtleasEdit, kIdBrowseNtleas, 1102, kIdMmdEdit, kIdBrowseMmd, kIdRegisterPmm, 1103, kIdSave, kIdCancel}) {
        SendMessageW(GetDlgItem(window, id), WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
    }
}

void layout_setup_window(HWND window, SetupState* state) {
    RECT client{};
    GetClientRect(window, &client);
    const int margin = scale_for(window, 16);
    const int gap = scale_for(window, 8);
    const int label_height = scale_for(window, 20);
    const int edit_height = scale_for(window, 24);
    const int button_height = scale_for(window, 28);
    const int browse_width = scale_for(window, 78);
    const int button_width = scale_for(window, 104);
    const int usable_width = std::max(scale_for(window, 420), static_cast<int>(client.right) - margin * 2);
    int y = margin;

    HWND info = GetDlgItem(window, 1100);
    MoveWindow(info, margin, y, usable_width, scale_for(window, 40), TRUE);
    y += scale_for(window, 48);

    HWND ntleas_label = GetDlgItem(window, 1101);
    MoveWindow(ntleas_label, margin, y, usable_width, label_height, TRUE);
    y += label_height;
    MoveWindow(state->ntleas_edit, margin, y, usable_width - browse_width - gap, edit_height, TRUE);
    MoveWindow(GetDlgItem(window, kIdBrowseNtleas), margin + usable_width - browse_width, y, browse_width, edit_height, TRUE);
    y += edit_height + gap;

    HWND mmd_label = GetDlgItem(window, 1102);
    MoveWindow(mmd_label, margin, y, usable_width, label_height, TRUE);
    y += label_height;
    MoveWindow(state->mmd_edit, margin, y, usable_width - browse_width - gap, edit_height, TRUE);
    MoveWindow(GetDlgItem(window, kIdBrowseMmd), margin + usable_width - browse_width, y, browse_width, edit_height, TRUE);
    y += edit_height + scale_for(window, 12);

    MoveWindow(state->register_checkbox, margin, y, usable_width, label_height, TRUE);
    y += label_height + scale_for(window, 10);
    MoveWindow(GetDlgItem(window, 1103), margin, y, usable_width, scale_for(window, 26), TRUE);

    const int button_y = client.bottom - margin - button_height;
    MoveWindow(GetDlgItem(window, kIdCancel), margin + usable_width - button_width, button_y, button_width, button_height, TRUE);
    MoveWindow(GetDlgItem(window, kIdSave), margin + usable_width - button_width * 2 - gap, button_y, button_width, button_height, TRUE);
}

std::optional<std::wstring> choose_executable(HWND owner, const wchar_t* title) {
    std::vector<wchar_t> path(32768, L'\0');
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = L"執行檔 (*.exe)\0*.exe\0所有檔案 (*.*)\0*.*\0\0";
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrTitle = title;
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&dialog)) {
        return std::nullopt;
    }
    return std::wstring(path.data());
}

bool set_registry_string(HKEY root, const std::wstring& subkey, const wchar_t* value_name, const std::wstring& value) {
    HKEY key = nullptr;
    const LONG created = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (created != ERROR_SUCCESS) {
        return false;
    }
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LONG written = RegSetValueExW(key, value_name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    RegCloseKey(key);
    return written == ERROR_SUCCESS;
}

bool set_registry_empty_value(HKEY root, const std::wstring& subkey, const std::wstring& value_name) {
    HKEY key = nullptr;
    const LONG created = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (created != ERROR_SUCCESS) {
        return false;
    }
    const LONG written = RegSetValueExW(key, value_name.c_str(), 0, REG_SZ, nullptr, 0);
    RegCloseKey(key);
    return written == ERROR_SUCCESS;
}

bool register_pmm_open_with(const Config& config, std::wstring& error) {
    const std::wstring launcher = get_module_path();
    if (launcher.empty()) {
        error = L"無法取得 MMDLocaleLauncher.exe 的位置。";
        return false;
    }
    const std::wstring classes = L"Software\\Classes\\";
    const std::wstring command = L"\"" + launcher + L"\" \"%1\"";
    const bool ok =
        set_registry_string(HKEY_CURRENT_USER, classes + kProgId, nullptr, L"MMD Locale Launcher PMM Project") &&
        set_registry_string(HKEY_CURRENT_USER, classes + kProgId + L"\\DefaultIcon", nullptr, config.mmd_path + L",0") &&
        set_registry_string(HKEY_CURRENT_USER, classes + kProgId + L"\\shell\\open", nullptr, L"使用 MMD Locale Launcher 開啟") &&
        set_registry_string(HKEY_CURRENT_USER, classes + kProgId + L"\\shell\\open\\command", nullptr, command) &&
        set_registry_empty_value(HKEY_CURRENT_USER, classes + L".pmm\\OpenWithProgids", kProgId) &&
        set_registry_string(HKEY_CURRENT_USER, L"Software\\MMDLocaleLauncher\\Capabilities", L"ApplicationName", kAppName) &&
        set_registry_string(HKEY_CURRENT_USER, L"Software\\MMDLocaleLauncher\\Capabilities", L"ApplicationDescription", L"以日語 CP932 環境開啟 MikuMikuDance 專案") &&
        set_registry_string(HKEY_CURRENT_USER, L"Software\\MMDLocaleLauncher\\Capabilities\\FileAssociations", L".pmm", kProgId) &&
        set_registry_string(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", kAppName, L"Software\\MMDLocaleLauncher\\Capabilities");
    if (!ok) {
        error = L"無法註冊 .pmm 的開啟程式。";
        return false;
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

void open_default_apps_ui(HWND owner) {
    IApplicationAssociationRegistrationUI* ui = nullptr;
    const HRESULT created = CoCreateInstance(CLSID_ApplicationAssociationRegistrationUI, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&ui));
    if (SUCCEEDED(created) && ui != nullptr) {
        const HRESULT launched = ui->LaunchAdvancedAssociationUI(kAppName);
        ui->Release();
        if (SUCCEEDED(launched)) {
            return;
        }
    }
    show_info(owner, L"已將 MMD Locale Launcher 加入 .pmm 的開啟程式清單。\n\n請在 Windows 的「開啟檔案方式」中選擇 MMD Locale Launcher，並勾選「一律使用此應用程式」。");
}

bool validate_config(const Config& config, HWND owner) {
    if (!is_existing_file(config.ntleas_path)) {
        show_error(owner, L"找不到 ntleas 執行檔：\n" + config.ntleas_path);
        return false;
    }
    if (!is_existing_file(config.mmd_path)) {
        show_error(owner, L"找不到 MikuMikuDance 執行檔：\n" + config.mmd_path);
        return false;
    }
    return true;
}

LRESULT CALLBACK setup_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<SetupState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    case WM_CREATE: {
        state = reinterpret_cast<SetupState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        enable_dark_title_bar(window);
        apply_mmd_icon(window, state->config.mmd_path);
        create_control(SS_LEFT, 1100, window, L"STATIC", L"請設定 ntleas 與 MikuMikuDance 的執行檔。儲存後，直接雙擊本程式即可開啟 MMD。");
        create_control(SS_LEFT, 1101, window, L"STATIC", L"ntleas 執行檔（ntleas.exe）");
        state->ntleas_edit = create_control(WS_BORDER | ES_AUTOHSCROLL, kIdNtleasEdit, window, L"EDIT", state->config.ntleas_path.c_str());
        create_control(BS_OWNERDRAW, kIdBrowseNtleas, window, L"BUTTON", L"瀏覽…");
        create_control(SS_LEFT, 1102, window, L"STATIC", L"MikuMikuDance 執行檔（MikuMikuDance.exe）");
        state->mmd_edit = create_control(WS_BORDER | ES_AUTOHSCROLL, kIdMmdEdit, window, L"EDIT", state->config.mmd_path.c_str());
        create_control(BS_OWNERDRAW, kIdBrowseMmd, window, L"BUTTON", L"瀏覽…");
        state->register_checkbox = create_control(BS_OWNERDRAW, kIdRegisterPmm, window, L"BUTTON", L"註冊並設定為 .pmm 的預設開啟程式");
        create_control(SS_LEFT, 1103, window, L"STATIC", L"Windows 會顯示預設應用程式畫面，必須由你確認，不會在背景強制修改預設值。");
        create_control(BS_OWNERDRAW, kIdSave, window, L"BUTTON", L"儲存");
        create_control(BS_OWNERDRAW, kIdCancel, window, L"BUTTON", L"取消");
        apply_ui_font(window, state);
        layout_setup_window(window, state);
        return 0;
    }
    case WM_SIZE:
        if (state != nullptr) {
            layout_setup_window(window, state);
        }
        return 0;
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
            suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (state != nullptr) {
            apply_ui_font(window, state);
            layout_setup_window(window, state);
        }
        return 0;
    }
    case WM_COMMAND:
        if (state == nullptr) {
            return 0;
        }
        switch (LOWORD(wparam)) {
        case kIdBrowseNtleas: {
            const auto chosen = choose_executable(window, L"選擇 ntleas.exe");
            if (chosen) {
                SetWindowTextW(state->ntleas_edit, chosen->c_str());
            }
            return 0;
        }
        case kIdBrowseMmd: {
            const auto chosen = choose_executable(window, L"選擇 MikuMikuDance.exe");
            if (chosen) {
                SetWindowTextW(state->mmd_edit, chosen->c_str());
                apply_mmd_icon(window, *chosen);
            }
            return 0;
        }
        case kIdRegisterPmm:
            if (HIWORD(wparam) == BN_CLICKED) {
                state->register_pmm = !state->register_pmm;
                InvalidateRect(state->register_checkbox, nullptr, TRUE);
            }
            return 0;
        case kIdSave: {
            Config config{read_window_text(state->ntleas_edit), read_window_text(state->mmd_edit)};
            if (!validate_config(config, window)) {
                return 0;
            }
            std::wstring error;
            if (!save_config(config, error)) {
                show_error(window, error);
                return 0;
            }
            state->config = std::move(config);
            state->saved = true;
            DestroyWindow(window);
            return 0;
        }
        case kIdCancel:
            DestroyWindow(window);
            return 0;
        default:
            return 0;
        }
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
        if (item == nullptr) {
            return FALSE;
        }
        if (item->CtlID == kIdRegisterPmm) {
            draw_checkbox(*item, state != nullptr && state->register_pmm);
            return TRUE;
        }
        if (item->CtlID == kIdBrowseNtleas || item->CtlID == kIdBrowseMmd || item->CtlID == kIdSave || item->CtlID == kIdCancel) {
            draw_button(*item, item->CtlID == kIdSave);
            return TRUE;
        }
        return FALSE;
    }
    case WM_CTLCOLORSTATIC:
        SetTextColor(reinterpret_cast<HDC>(wparam), kTextSecondary);
        SetBkMode(reinterpret_cast<HDC>(wparam), TRANSPARENT);
        return reinterpret_cast<LRESULT>(window_brush());
    case WM_CTLCOLOREDIT:
        SetTextColor(reinterpret_cast<HDC>(wparam), kTextPrimary);
        SetBkColor(reinterpret_cast<HDC>(wparam), kControlBackground);
        return reinterpret_cast<LRESULT>(control_brush());
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        release_window_icons(window);
        if (state != nullptr && state->font != nullptr) {
            DeleteObject(state->font);
            state->font = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

bool show_setup_window(Config& config, bool& register_pmm) {
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = setup_window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = window_brush();
    window_class.lpszClassName = kWindowClass;
    if (GetClassInfoW(window_class.hInstance, kWindowClass, &window_class) == FALSE) {
        if (RegisterClassW(&window_class) == 0) {
            show_error(nullptr, L"無法建立設定視窗。");
            return false;
        }
    }

    SetupState state{config};
    const UINT dpi = GetDpiForSystem();
    const int width = MulDiv(700, static_cast<int>(dpi == 0 ? 96 : dpi), 96);
    const int height = MulDiv(330, static_cast<int>(dpi == 0 ? 96 : dpi), 96);
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, kWindowClass, L"MMD Locale Launcher 設定",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), &state);
    if (window == nullptr) {
        show_error(nullptr, L"無法開啟設定視窗。");
        return false;
    }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (!state.saved) {
        return false;
    }
    config = state.config;
    register_pmm = state.register_pmm;
    return true;
}

std::wstring quote_windows_argument(std::wstring_view argument) {
    std::wstring result = L"\"";
    size_t slash_count = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++slash_count;
            continue;
        }
        if (character == L'\"') {
            result.append(slash_count * 2 + 1, L'\\');
            result.push_back(L'\"');
            slash_count = 0;
            continue;
        }
        result.append(slash_count, L'\\');
        slash_count = 0;
        result.push_back(character);
    }
    result.append(slash_count * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

bool launch_mmd(const Config& config, const std::optional<std::wstring>& project_path) {
    if (!validate_config(config, nullptr)) {
        return false;
    }
    if (project_path && project_path->find(L'\'') != std::wstring::npos) {
        show_error(nullptr, L"目前 ntleas 的 A 參數無法安全傳遞含單引號（'）的 PMM 路徑。\n請移動或重新命名該檔案後再開啟。");
        return false;
    }

    std::vector<std::wstring> arguments{
        quote_windows_argument(config.ntleas_path),
        quote_windows_argument(config.mmd_path),
        L"C932",
        L"L1041",
        quote_windows_argument(L"FMS PGothic"),
        L"P4"
    };
    if (project_path) {
        arguments.push_back(quote_windows_argument(L"A'" + *project_path + L"'"));
    }
    std::wstring command_line;
    for (const std::wstring& argument : arguments) {
        if (!command_line.empty()) {
            command_line.push_back(L' ');
        }
        command_line += argument;
    }

    const std::filesystem::path working_directory = std::filesystem::path(config.mmd_path).parent_path();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');
    if (!CreateProcessW(config.ntleas_path.c_str(), mutable_command.data(), nullptr, nullptr, FALSE, 0, nullptr,
        working_directory.c_str(), &startup, &process)) {
        show_error(nullptr, L"無法透過 ntleas 啟動 MMD。\n\nWindows 錯誤碼：" + std::to_wstring(GetLastError()));
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

std::optional<std::wstring> get_project_argument(const std::vector<std::wstring>& arguments) {
    for (const auto& argument : arguments) {
        if (!argument.empty() && argument[0] != L'/' && argument[0] != L'-') {
            return argument;
        }
    }
    return std::nullopt;
}

bool has_option(const std::vector<std::wstring>& arguments, std::wstring_view option) {
    return std::any_of(arguments.begin(), arguments.end(), [option](const std::wstring& argument) {
        return _wcsicmp(argument.c_str(), std::wstring(option).c_str()) == 0;
    });
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    int argc = 0;
    LPWSTR* raw_arguments = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> arguments;
    for (int index = 1; raw_arguments != nullptr && index < argc; ++index) {
        arguments.emplace_back(raw_arguments[index]);
    }
    if (raw_arguments != nullptr) {
        LocalFree(raw_arguments);
    }

    Config config = read_config().value_or(Config{});
    bool register_pmm = false;
    const bool settings_requested = has_option(arguments, L"/settings") || has_option(arguments, L"--settings");
    const bool registration_requested = has_option(arguments, L"/register-pmm") || has_option(arguments, L"--register-pmm");
    if (settings_requested || !is_valid_config(config)) {
        if (!show_setup_window(config, register_pmm)) {
            if (SUCCEEDED(com_result)) {
                CoUninitialize();
            }
            return 0;
        }
    }
    if (registration_requested) {
        register_pmm = true;
    }
    if (register_pmm) {
        std::wstring error;
        if (!register_pmm_open_with(config, error)) {
            show_error(nullptr, error);
        } else {
            open_default_apps_ui(nullptr);
        }
    }

    const std::optional<std::wstring> project = get_project_argument(arguments);
    const bool launched = !settings_requested && launch_mmd(config, project);
    if (SUCCEEDED(com_result)) {
        CoUninitialize();
    }
    return launched || settings_requested ? 0 : 1;
}
