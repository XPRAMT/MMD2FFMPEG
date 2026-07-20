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
    HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED,
                                  0, 0, info.size.cx, info.size.cy, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    RECT area{0, 0, info.size.cx, info.size.cy};
    result = page->Activate(parent, &area, FALSE);
    HWND page_window = FindWindowExW(parent, nullptr, nullptr, nullptr);
    if (FAILED(result) || !page_window) {
        std::wcerr << L"Property page activation failed.\n";
        DestroyWindow(parent); page->Release(); CoUninitialize(); return 3;
    }

    RECT client{};
    GetClientRect(page_window, &client);
    const int ids[] = {ID_BACKEND, ID_CODEC, ID_DEPTH, ID_PRESET, ID_RATE, ID_QP, ID_BITRATE,
                       ID_STATUS, ID_REFRESH, ID_COMMAND_PREFIX, ID_COMMAND, ID_COMMAND_SUFFIX,
                       ID_TEST_REQUIREMENT};
    for (const int id : ids) {
        const RECT rectangle = child_rect(page_window, id);
        if (!valid_rect(rectangle) || rectangle.left < 0 || rectangle.top < 0 ||
            rectangle.right > client.right || rectangle.bottom > client.bottom) {
            std::wcerr << L"Control outside page: " << id << L"\n";
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
    const RECT status = child_rect(page_window, ID_STATUS);
    const RECT requirement = child_rect(page_window, ID_TEST_REQUIREMENT);
    const RECT test_button = child_rect(page_window, ID_REFRESH);
    if (suffix.bottom > status.top || status.bottom > requirement.top ||
        status.bottom > test_button.top || test_button.bottom > client.bottom) {
        std::wcerr << L"Encoder test controls are not at the bottom or overlap.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 6;
    }
    HWND status_control = GetDlgItem(page_window, ID_STATUS);
    HWND command_control = GetDlgItem(page_window, ID_COMMAND);
    if (window_text(status_control) != L"Encoder status: not tested" || !IsWindowEnabled(GetDlgItem(page_window, ID_REFRESH))) {
        std::wcerr << L"The property page started an encoder test automatically.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 7;
    }
    SetWindowTextW(command_control, (window_text(command_control) + L" ").c_str());
    if (window_text(status_control) != L"Encoder status: not tested") {
        std::wcerr << L"Changing encoder settings started a test automatically.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 8;
    }
    std::wcout << mode << L" property page layout OK: "
               << info.size.cx << L"x" << info.size.cy << L"\n";
    page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 0;
}
