#include <windows.h>
#include <ocidl.h>

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

    IPropertyPage* page = nullptr;
    HRESULT result = CoCreateInstance(CLSID_MMD2FFMPEG_SETTINGS, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IPropertyPage, reinterpret_cast<void**>(&page));
    if (FAILED(result)) {
        std::wcerr << L"CoCreateInstance failed: 0x" << std::hex << result << L"\n";
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
    const int host_height = constrained_host ? std::min(page_height, 1000) : page_height;
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
    const int ids[] = {ID_LANGUAGE, ID_BACKEND, ID_CODEC, ID_DEPTH, ID_PRESET, ID_RATE, ID_QP, ID_BITRATE,
                       ID_STATUS, ID_REFRESH, ID_COMMAND_PREFIX, ID_COMMAND, ID_COMMAND_SUFFIX,
                       ID_TEST_REQUIREMENT, ID_LABEL_LANGUAGE, ID_LABEL_BACKEND, ID_LABEL_CODEC,
                       ID_LABEL_DEPTH, ID_LABEL_PRESET, ID_LABEL_RATE, ID_LABEL_QP, ID_LABEL_BITRATE,
                       ID_COMMAND_HEADING};
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
