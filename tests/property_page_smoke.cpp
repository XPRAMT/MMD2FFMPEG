#include <windows.h>
#include <ocidl.h>
#include <commctrl.h>
#include <array>

#include <filesystem>
#include <iostream>
#include <string>

#include "../src/resource.h"

namespace {
constexpr GUID CLSID_MMD2FFMPEG_SETTINGS =
    {0x65a23874, 0xae1c, 0x4b10, {0x9f, 0x1a, 0x5b, 0xc0, 0xa8, 0xd4, 0x4b, 0x38}};

RECT child_rect(HWND page, int id) {
    RECT rectangle{};
    HWND child = GetDlgItem(page, id);
    if (!child || !GetWindowRect(child, &rectangle)) return {};
    MapWindowPoints(HWND_DESKTOP, page, reinterpret_cast<POINT*>(&rectangle), 2);
    return rectangle;
}

bool valid_rect(const RECT& rectangle) {
    return rectangle.right > rectangle.left && rectangle.bottom > rectangle.top;
}

bool has_visible_style(HWND window) {
    return window && (GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != 0;
}

std::wstring window_text(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(window, value.data(), length + 1);
    value.resize(static_cast<std::size_t>(length));
    return value;
}
}

int wmain(int argument_count, wchar_t** arguments) {
    const std::wstring mode = argument_count > 1 ? arguments[1] : L"pmv2";
    const auto awareness = mode == L"unaware" ? DPI_AWARENESS_CONTEXT_UNAWARE :
                           mode == L"system" ? DPI_AWARENESS_CONTEXT_SYSTEM_AWARE : DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2;
    SetProcessDpiAwarenessContext(awareness);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::array<wchar_t, 32768> executable_path{};
    GetModuleFileNameW(nullptr, executable_path.data(), static_cast<DWORD>(executable_path.size()));
    const auto dll_path = std::filesystem::path(executable_path.data()).parent_path() / L"mmd2ffmpeg_dmo.dll";
    HMODULE module = LoadLibraryW(dll_path.c_str());
    if (!module) {
        std::wcerr << L"Could not load build DLL: " << dll_path.wstring() << L"\n";
        CoUninitialize(); return 1;
    }
    using DllGetClassObjectFn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    const auto get_class_object = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(module, "DllGetClassObject"));
    IClassFactory* factory = nullptr;
    HRESULT result = get_class_object ? get_class_object(CLSID_MMD2FFMPEG_SETTINGS, IID_IClassFactory,
                                                          reinterpret_cast<void**>(&factory)) : E_NOINTERFACE;
    IPropertyPage* page = nullptr;
    if (SUCCEEDED(result)) result = factory->CreateInstance(nullptr, IID_IPropertyPage, reinterpret_cast<void**>(&page));
    if (factory) factory->Release();
    if (FAILED(result)) {
        std::wcerr << L"Build DLL property-page creation failed: 0x" << std::hex << result << L"\n";
        CoUninitialize(); return 1;
    }
    PROPPAGEINFO info{};
    info.cb = sizeof(info);
    result = page->GetPageInfo(&info);
    if (FAILED(result) || info.size.cx <= 0 || info.size.cy <= 0) {
        std::wcerr << L"Invalid property page size.\n";
        page->Release(); CoUninitialize(); return 2;
    }
    CoTaskMemFree(info.pszTitle);
    // The normal test verifies the exact, natural page size reported to the
    // property-frame host.  A separate opt-in mode simulates a host whose
    // available work area is smaller, as can happen with MMD on high-DPI
    // desktops; this must expose scrolling instead of clipping controls.
    const bool constrained_host = argument_count > 2 && std::wstring(arguments[2]) == L"constrained";
    const int page_height = static_cast<int>(info.size.cy);
    const int host_height = constrained_host ? std::max(1, page_height * 3 / 4) : page_height;
    HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED,
                                  0, 0, info.size.cx, host_height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    RECT area{0, 0, info.size.cx, host_height};
    result = page->Activate(parent, &area, FALSE);
    HWND page_window = FindWindowExW(parent, nullptr, nullptr, nullptr);
    if (FAILED(result) || !page_window) {
        std::wcerr << L"Property page activation failed.\n";
        DestroyWindow(parent); page->Release(); CoUninitialize(); return 3;
    }

    RECT client{};
    GetClientRect(page_window, &client);
    const int ids[] = {ID_TAB, ID_LANGUAGE, ID_BACKEND, ID_CODEC, ID_DEPTH, ID_PRESET, ID_RATE, ID_QP, ID_BITRATE,
                       ID_STATUS, ID_REFRESH, ID_OPEN_LOG, ID_COMMAND_PREFIX, ID_COMMAND, ID_COMMAND_SUFFIX,
                       ID_TEST_REQUIREMENT, ID_LABEL_LANGUAGE, ID_LABEL_BACKEND, ID_LABEL_CODEC,
                       ID_LABEL_DEPTH, ID_LABEL_PRESET, ID_LABEL_RATE, ID_LABEL_QP, ID_LABEL_BITRATE,
                       ID_COMMAND_HEADING, ID_AUDIO_FORMAT, ID_AUDIO_RATE, ID_LABEL_AUDIO_FORMAT,
                       ID_LABEL_AUDIO_RATE, ID_AUDIO_INTRO, ID_AUDIO_HELP, ID_SETTINGS_INFO, ID_GITHUB_LINK};
    for (const int id : ids) {
        const RECT rectangle = child_rect(page_window, id);
        if (!valid_rect(rectangle) || rectangle.left < 0 || rectangle.right > client.right ||
            (!constrained_host && (rectangle.top < 0 || rectangle.bottom > client.bottom))) {
            std::wcerr << L"Control outside page: " << id << L" rect=" << rectangle.left << L"," << rectangle.top
                       << L"," << rectangle.right << L"," << rectangle.bottom << L" client=" << client.right
                       << L"," << client.bottom << L"\n";
            page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 4;
        }
    }
    const HFONT page_font = reinterpret_cast<HFONT>(SendMessageW(GetDlgItem(page_window, ID_BACKEND), WM_GETFONT, 0, 0));
    const int same_font_ids[] = {ID_TAB, ID_AUDIO_FORMAT, ID_AUDIO_RATE, ID_AUDIO_INTRO, ID_AUDIO_HELP,
                                 ID_LANGUAGE, ID_SETTINGS_INFO, ID_GITHUB_LINK};
    for (const int id : same_font_ids) {
        const HFONT control_font = reinterpret_cast<HFONT>(SendMessageW(GetDlgItem(page_window, id), WM_GETFONT, 0, 0));
        if (!page_font || control_font != page_font) {
            std::wcerr << L"Control does not inherit the shared dialog font: " << id << L"\n";
            page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 11;
        }
    }
    std::array<wchar_t, 16> github_class{};
    GetClassNameW(GetDlgItem(page_window, ID_GITHUB_LINK), github_class.data(), static_cast<int>(github_class.size()));
    if (wcscmp(github_class.data(), L"Edit") != 0) {
        std::wcerr << L"GitHub address must be a selectable edit control.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 13;
    }
    const RECT language_label = child_rect(page_window, ID_LABEL_LANGUAGE);
    const RECT settings_info = child_rect(page_window, ID_SETTINGS_INFO);
    const RECT github_address = child_rect(page_window, ID_GITHUB_LINK);
    if (settings_info.top - language_label.bottom > 48 || github_address.top - settings_info.bottom > 48) {
        std::wcerr << L"Settings controls are not tightly stacked.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 14;
    }
    struct TabVisibility {
        int page;
        bool video;
        bool audio;
        bool settings;
    };
    const TabVisibility tab_visibility[] = {
        {0, true, false, false},
        {1, false, true, false},
        {2, false, false, true},
    };
    for (const auto& expected : tab_visibility) {
        SendMessageW(GetDlgItem(page_window, ID_TAB), TCM_SETCURSEL, expected.page, 0);
        NMHDR selection_changed{GetDlgItem(page_window, ID_TAB), ID_TAB, TCN_SELCHANGE};
        SendMessageW(page_window, WM_NOTIFY, ID_TAB, reinterpret_cast<LPARAM>(&selection_changed));
        if (has_visible_style(GetDlgItem(page_window, ID_BACKEND)) != expected.video ||
            has_visible_style(GetDlgItem(page_window, ID_AUDIO_FORMAT)) != expected.audio ||
            has_visible_style(GetDlgItem(page_window, ID_LANGUAGE)) != expected.settings ||
            has_visible_style(GetDlgItem(page_window, ID_SETTINGS_INFO)) != expected.settings) {
            std::wcerr << L"Tab visibility failed for page " << expected.page << L".\n";
            page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 12;
        }
    }
    SendMessageW(GetDlgItem(page_window, ID_TAB), TCM_SETCURSEL, 0, 0);
    NMHDR video_selected{GetDlgItem(page_window, ID_TAB), ID_TAB, TCN_SELCHANGE};
    SendMessageW(page_window, WM_NOTIFY, ID_TAB, reinterpret_cast<LPARAM>(&video_selected));
    const RECT prefix = child_rect(page_window, ID_COMMAND_PREFIX);
    const RECT command = child_rect(page_window, ID_COMMAND);
    const RECT suffix = child_rect(page_window, ID_COMMAND_SUFFIX);
    if (prefix.bottom > command.top || command.bottom > suffix.top) {
        std::wcerr << L"FFmpeg command controls overlap.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 5;
    }
    if (constrained_host) {
        SCROLLINFO scroll_info{sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS};
        GetScrollInfo(page_window, SB_VERT, &scroll_info);
        if (scroll_info.nMax - static_cast<int>(scroll_info.nPage) + 1 <= 0) {
            std::wcerr << L"Constrained property page did not expose a vertical scrollbar.\n";
            page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 6;
        }
        SendMessageW(page_window, WM_VSCROLL, SB_BOTTOM, 0);
        GetClientRect(page_window, &client);
        const RECT status = child_rect(page_window, ID_STATUS);
        const RECT requirement = child_rect(page_window, ID_TEST_REQUIREMENT);
        const RECT test_button = child_rect(page_window, ID_REFRESH);
        if (status.top < 0 || requirement.top < 0 || test_button.top < 0 ||
            status.bottom > client.bottom || requirement.bottom > client.bottom || test_button.bottom > client.bottom ||
            status.bottom > requirement.top || status.bottom > test_button.top) {
            std::wcerr << L"Encoder test controls are not reachable after scrolling to the bottom.\n";
            page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 7;
        }
    }
    HWND status_control = GetDlgItem(page_window, ID_STATUS);
    HWND command_control = GetDlgItem(page_window, ID_COMMAND);
    HWND language_control = GetDlgItem(page_window, ID_LANGUAGE);
    const LRESULT original_language = SendMessageW(language_control, CB_GETCURSEL, 0, 0);
    const wchar_t* language_labels[]{L"語言", L"語言", L"语言", L"言語", L"Language"};
    const wchar_t* button_labels[]{L"測試編碼", L"測試編碼", L"测试编码", L"エンコーダーをテスト", L"Test encoder"};
    const wchar_t* open_log_labels[]{L"開啟log", L"開啟log", L"打开日志", L"ログを開く", L"Open log"};
    const wchar_t* not_tested_labels[]{L"編碼器狀態：尚未測試", L"編碼器狀態：尚未測試", L"编码器状态：尚未测试",
                                       L"エンコーダー状態：未テスト", L"Encoder status: not tested"};
    if (!IsWindowEnabled(GetDlgItem(page_window, ID_REFRESH))) {
        std::wcerr << L"The property page started an encoder test automatically.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 8;
    }
    for (int language = 0; language < 5; ++language) {
        SendMessageW(language_control, CB_SETCURSEL, language, 0);
        SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_LANGUAGE, CBN_SELCHANGE), reinterpret_cast<LPARAM>(language_control));
        int text_index = language;
        if (language == 0) {
            const LANGID ui = GetUserDefaultUILanguage();
            if (PRIMARYLANGID(ui) == LANG_JAPANESE) text_index = 3;
            else if (PRIMARYLANGID(ui) != LANG_CHINESE) text_index = 4;
            else {
                const WORD sublanguage = SUBLANGID(ui);
                text_index = sublanguage == SUBLANG_CHINESE_TRADITIONAL || sublanguage == SUBLANG_CHINESE_HONGKONG ||
                             sublanguage == SUBLANG_CHINESE_MACAU ? 1 : 2;
            }
        }
        if (window_text(GetDlgItem(page_window, ID_LABEL_LANGUAGE)) != language_labels[text_index] ||
            window_text(GetDlgItem(page_window, ID_REFRESH)) != button_labels[text_index] ||
            window_text(GetDlgItem(page_window, ID_OPEN_LOG)) != open_log_labels[text_index] ||
            window_text(status_control) != not_tested_labels[text_index]) {
            std::wcerr << L"Language switch failed for index " << language << L".\n";
            page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 9;
        }
    }
    SendMessageW(language_control, CB_SETCURSEL, original_language, 0);
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_LANGUAGE, CBN_SELCHANGE), reinterpret_cast<LPARAM>(language_control));
    SetWindowTextW(command_control, (window_text(command_control) + L" ").c_str());
    const std::wstring status_after_change = window_text(status_control);
    if (status_after_change.find(L"testing") != std::wstring::npos || status_after_change.find(L"テスト中") != std::wstring::npos ||
        status_after_change.find(L"測試中") != std::wstring::npos || status_after_change.find(L"测试中") != std::wstring::npos) {
        std::wcerr << L"Changing encoder settings started a test automatically.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 10;
    }
    std::wcout << mode << L" property page layout OK: "
               << info.size.cx << L"x" << info.size.cy << L"\n";
    page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 0;
}
