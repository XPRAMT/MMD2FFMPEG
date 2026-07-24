#include <windows.h>
#include <ocidl.h>
#include <commctrl.h>
#include <algorithm>
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

std::wstring combo_item_text(HWND combo, int index) {
    const LRESULT length = SendMessageW(combo, CB_GETLBTEXTLEN, index, 0);
    if (length < 0) return {};
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    SendMessageW(combo, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(value.data()));
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
    bool constrained_host = false;
    bool wide_host = false;
    for (int index = 2; index < argument_count; ++index) {
        const std::wstring option(arguments[index]);
        constrained_host = constrained_host || option == L"constrained";
        wide_host = wide_host || option == L"wide";
    }
    const int page_height = static_cast<int>(info.size.cy);
    const int host_height = constrained_host ? std::max(1, page_height * 3 / 4) : page_height;
    const int host_width = wide_host ? static_cast<int>(info.size.cx) * 5 / 4 : static_cast<int>(info.size.cx);
    HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED,
                                  0, 0, host_width, host_height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    RECT area{0, 0, host_width, host_height};
    result = page->Activate(parent, &area, FALSE);
    HWND page_window = FindWindowExW(parent, nullptr, nullptr, nullptr);
    if (FAILED(result) || !page_window) {
        std::wcerr << L"Property page activation failed.\n";
        DestroyWindow(parent); page->Release(); CoUninitialize(); return 3;
    }
    if (mode == L"vsr_probe" || mode == L"vsr_matrix") {
        const auto select_combo = [&](int id, int index) {
            const HWND control = GetDlgItem(page_window, id);
            SendMessageW(control, CB_SETCURSEL, index, 0);
            SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE),
                         reinterpret_cast<LPARAM>(control));
        };
        select_combo(ID_BACKEND, 2);
        select_combo(ID_CODEC, 1);
        select_combo(ID_ALPHA, 0);
        select_combo(ID_VSR_ENABLED, 1);

        struct VsrCase {
            const wchar_t* name;
            int depth;
            int chroma;
            int preset;
            int quality;
            const wchar_t* scale;
        };
        const std::array<VsrCase, 4> matrix{{
            {L"minimum", 0, 0, 0, 0, L"2.00"},
            {L"current", 1, 0, 6, 1, L"2.00"},
            {L"fractional-422", 1, 1, 5, 2, L"1.50"},
            {L"maximum", 1, 2, 6, 3, L"4.00"},
        }};
        const int case_count = mode == L"vsr_matrix" ? static_cast<int>(matrix.size()) : 1;
        bool all_passed = true;
        for (int case_index = 0; case_index < case_count; ++case_index) {
            const VsrCase& test_case = mode == L"vsr_matrix" ? matrix[case_index] : matrix[1];
            select_combo(ID_DEPTH, test_case.depth);
            select_combo(ID_CHROMA, test_case.chroma);
            select_combo(ID_PRESET, test_case.preset);
            select_combo(ID_VSR_QUALITY, test_case.quality);
            SetWindowTextW(GetDlgItem(page_window, ID_VSR_SCALE), test_case.scale);
            SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_VSR_SCALE, EN_CHANGE),
                         reinterpret_cast<LPARAM>(GetDlgItem(page_window, ID_VSR_SCALE)));

            const ULONGLONG started = GetTickCount64();
            SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_REFRESH, BN_CLICKED),
                         reinterpret_cast<LPARAM>(GetDlgItem(page_window, ID_REFRESH)));
            MSG message{};
            while (!IsWindowEnabled(GetDlgItem(page_window, ID_REFRESH)) &&
                   GetTickCount64() - started < 40000) {
                const DWORD wait_result = MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
                if (wait_result == WAIT_OBJECT_0) {
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                }
            }
            const ULONGLONG elapsed = GetTickCount64() - started;
            const std::wstring status = window_text(GetDlgItem(page_window, ID_STATUS));
            const bool completed = IsWindowEnabled(GetDlgItem(page_window, ID_REFRESH)) != FALSE;
            const bool passed = status.find(L"test passed") != std::wstring::npos ||
                                status.find(L"測試通過") != std::wstring::npos ||
                                status.find(L"测试通过") != std::wstring::npos ||
                                status.find(L"テスト合格") != std::wstring::npos;
            std::wcout << L"VSR case=" << test_case.name << L" elapsed=" << elapsed
                       << L" ms completed=" << completed << L" passed=" << passed << L"\n";
            all_passed = all_passed && completed && passed;
        }
        page->Deactivate();
        DestroyWindow(parent);
        page->Release();
        CoUninitialize();
        return all_passed ? 0 : 45;
    }
    const HWND tooltip = reinterpret_cast<HWND>(GetPropW(page_window, L"MMD2FFMPEG.TooltipWindow"));
    std::array<wchar_t, 32> tooltip_class{};
    GetClassNameW(tooltip, tooltip_class.data(), static_cast<int>(tooltip_class.size()));
    const LRESULT tooltip_count = tooltip ? SendMessageW(tooltip, TTM_GETTOOLCOUNT, 0, 0) : -1;
    if (!tooltip || wcscmp(tooltip_class.data(), TOOLTIPS_CLASSW) != 0 ||
        tooltip_count != 50) {
        std::wcerr << L"Every configurable option must expose a native tooltip. handle=" << tooltip
                   << L" class=" << tooltip_class.data() << L" count=" << tooltip_count << L"\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 30;
    }
    TOOLINFOW backend_tip{};
    backend_tip.cbSize = SendMessageW(tooltip, CCM_GETVERSION, 0, 0) < 6 ? TTTOOLINFOW_V2_SIZE : sizeof(TOOLINFOW);
    backend_tip.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    backend_tip.hwnd = page_window;
    backend_tip.uId = reinterpret_cast<UINT_PTR>(GetDlgItem(page_window, ID_BACKEND));
    std::array<wchar_t, 1024> backend_tip_text{};
    backend_tip.lpszText = backend_tip_text.data();
    SendMessageW(tooltip, TTM_GETTEXTW, 0, reinterpret_cast<LPARAM>(&backend_tip));
    if (backend_tip_text[0] == L'\0') {
        std::wcerr << L"Encoder tooltip text is missing.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 31;
    }
    TOOLINFOW warning_tip = backend_tip;
    warning_tip.uId = reinterpret_cast<UINT_PTR>(GetDlgItem(page_window, ID_COMPAT_WARNING));
    std::array<wchar_t, 1024> warning_tip_text{};
    warning_tip.lpszText = warning_tip_text.data();
    SendMessageW(tooltip, TTM_GETTEXTW, 0, reinterpret_cast<LPARAM>(&warning_tip));
    const std::wstring warning_tooltip(warning_tip_text.data());
    if (warning_tooltip.find(L"可能") == std::wstring::npos &&
        warning_tooltip.find(L"may") == std::wstring::npos) {
        std::wcerr << L"Compatibility warning tooltip text is missing.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 53;
    }

    RECT client{};
    GetClientRect(page_window, &client);
    const int ids[] = {ID_TAB, ID_BACKEND, ID_CODEC, ID_DEPTH, ID_PRESET, ID_RATE, ID_QP, ID_BITRATE,
                       ID_STATUS, ID_LABEL_STATUS, ID_REFRESH, ID_OPEN_LOG, ID_COMMAND_PREFIX, ID_COMMAND, ID_COMMAND_SUFFIX,
                       ID_TEST_REQUIREMENT, ID_LABEL_BACKEND, ID_LABEL_CODEC,
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
    const HFONT page_font = reinterpret_cast<HFONT>(SendMessageW(GetDlgItem(page_window, ID_BACKEND), WM_GETFONT, 0, 0));
    const int same_font_ids[] = {ID_TAB, ID_AUDIO_FORMAT, ID_AUDIO_RATE, ID_AUDIO_INTRO, ID_AUDIO_HELP,
                                 ID_LANGUAGE, ID_CPU_THREADS, ID_SETTINGS_INFO, ID_GITHUB_LINK};
    for (const int id : same_font_ids) {
        const HFONT control_font = reinterpret_cast<HFONT>(SendMessageW(GetDlgItem(page_window, id), WM_GETFONT, 0, 0));
        if (!page_font || control_font != page_font) {
            std::wcerr << L"Control does not inherit the shared dialog font: " << id << L"\n";
            page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 11;
        }
    }
    const RECT tab_bounds = child_rect(page_window, ID_TAB);
    for (const int id : {ID_LABEL_BACKEND}) {
        if (child_rect(page_window, id).left != tab_bounds.left) {
            std::wcerr << L"Tab page content does not share the common left boundary: " << id << L"\n";
            page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 15;
        }
    }
    const RECT command_bounds = child_rect(page_window, ID_COMMAND);
    if (tab_bounds.left <= 0 || tab_bounds.right != client.right - tab_bounds.left ||
        command_bounds.right != tab_bounds.right ||
        child_rect(page_window, ID_OPEN_LOG).right != tab_bounds.right) {
        std::wcerr << L"Tab and right-anchored controls do not preserve symmetric horizontal margins.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 16;
    }
    const RECT encoder_test_button = child_rect(page_window, ID_REFRESH);
    const RECT open_log_button = child_rect(page_window, ID_OPEN_LOG);
    if (encoder_test_button.right >= open_log_button.left) {
        std::wcerr << L"Test encoder and open log buttons overlap.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 17;
    }
    HWND video_tab = GetDlgItem(page_window, ID_VIDEO_TAB);
    if (!video_tab || TabCtrl_GetItemCount(video_tab) != 4) {
        std::wcerr << L"Video page does not expose Encoding, Color, Frame structure, and Super resolution sub-tabs.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 22;
    }
    const RECT video_tab_bounds = child_rect(page_window, ID_VIDEO_TAB);
    if (video_tab_bounds.top > tab_bounds.bottom) {
        std::wcerr << L"Video sub-tabs are separated from the main tabs by a vertical gap.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 26;
    }
    const RECT quality = child_rect(page_window, ID_QP);
    const RECT bitrate = child_rect(page_window, ID_BITRATE);
    const RECT command_heading = child_rect(page_window, ID_COMMAND_HEADING);
    const RECT command_editor = child_rect(page_window, ID_COMMAND);
    const RECT initial_status_bounds = child_rect(page_window, ID_STATUS);
    if (quality.bottom > command_heading.top || bitrate.bottom > command_heading.top) {
        std::wcerr << L"Quality or bitrate control overlaps the FFmpeg command area.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 25;
    }
    const LONG_PTR command_style = GetWindowLongPtrW(GetDlgItem(page_window, ID_COMMAND), GWL_STYLE);
    if ((command_style & ES_MULTILINE) == 0 || (command_style & ES_AUTOHSCROLL) != 0 ||
        command_editor.bottom - command_editor.top < (quality.bottom - quality.top) * 4) {
        std::wcerr << L"Editable FFmpeg command must be a four-line word-wrapping edit control.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 39;
    }
    std::array<wchar_t, 16> status_class{};
    GetClassNameW(GetDlgItem(page_window, ID_STATUS), status_class.data(), static_cast<int>(status_class.size()));
    const LONG_PTR status_style = GetWindowLongPtrW(GetDlgItem(page_window, ID_STATUS), GWL_STYLE);
    if (wcscmp(status_class.data(), L"Edit") != 0 || (status_style & ES_MULTILINE) == 0 ||
        (status_style & ES_READONLY) == 0 || (status_style & WS_VSCROLL) == 0 ||
        initial_status_bounds.bottom - initial_status_bounds.top < (quality.bottom - quality.top) * 2) {
        std::wcerr << L"Encoder status must be a two-line, read-only, scrollable edit control.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 29;
    }
    if (child_rect(page_window, ID_LABEL_STATUS).bottom > initial_status_bounds.top) {
        std::wcerr << L"Encoder status label overlaps the status text box.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 46;
    }
    const RECT command_before_color = child_rect(page_window, ID_COMMAND);
    TabCtrl_SetCurSel(video_tab, 1);
    NMHDR video_tab_change{};
    video_tab_change.hwndFrom = video_tab;
    video_tab_change.idFrom = ID_VIDEO_TAB;
    video_tab_change.code = TCN_SELCHANGE;
    const LRESULT video_tab_result = SendMessageW(page_window, WM_NOTIFY, ID_VIDEO_TAB, reinterpret_cast<LPARAM>(&video_tab_change));
    const int color_ids[]{ID_ALPHA, ID_MASK_OUTPUT, ID_CHROMA, ID_COLORSPACE, ID_COLOR_RANGE,
                          ID_LABEL_ALPHA, ID_LABEL_MASK_OUTPUT, ID_LABEL_CHROMA, ID_LABEL_COLORSPACE, ID_LABEL_COLOR_RANGE};
    for (const int id : color_ids) {
        const RECT rectangle = child_rect(page_window, id);
        if (!has_visible_style(GetDlgItem(page_window, id)) || !valid_rect(rectangle)) {
            std::wcerr << L"Color sub-tab control is unavailable: " << id
                       << L" visible_style=" << has_visible_style(GetDlgItem(page_window, id))
                       << L" notify_result=" << video_tab_result
                       << L" rect=" << rectangle.left << L"," << rectangle.top
                       << L"," << rectangle.right << L"," << rectangle.bottom << L".\n";
            page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 23;
        }
    }
    const RECT command_on_color = child_rect(page_window, ID_COMMAND);
    if (!has_visible_style(GetDlgItem(page_window, ID_COMMAND)) || command_before_color.top != command_on_color.top ||
        command_before_color.bottom != command_on_color.bottom) {
        std::wcerr << L"FFmpeg command area did not remain fixed below the video sub-tabs.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 24;
    }
    TabCtrl_SetCurSel(video_tab, 2);
    const LRESULT frame_tab_result = SendMessageW(page_window, WM_NOTIFY, ID_VIDEO_TAB, reinterpret_cast<LPARAM>(&video_tab_change));
    const int frame_ids[]{ID_FRAME_MODE, ID_GOP, ID_BFRAMES, ID_LABEL_FRAME_MODE, ID_LABEL_GOP, ID_LABEL_BFRAMES};
    for (const int id : frame_ids) {
        const RECT rectangle = child_rect(page_window, id);
        if (!has_visible_style(GetDlgItem(page_window, id)) || !valid_rect(rectangle)) {
            std::wcerr << L"Frame structure sub-tab control is unavailable: " << id
                       << L" notify_result=" << frame_tab_result << L"\n";
            page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 32;
        }
    }
    const int command_edit_height = child_rect(page_window, ID_COMMAND).bottom - child_rect(page_window, ID_COMMAND).top;
    if (child_rect(page_window, ID_GOP).bottom - child_rect(page_window, ID_GOP).top > command_edit_height * 2 ||
        child_rect(page_window, ID_BFRAMES).bottom - child_rect(page_window, ID_BFRAMES).top > command_edit_height * 2) {
        std::wcerr << L"GOP and B-frame inputs must use compact edit-control heights.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 38;
    }
    const RECT command_on_frame_structure = child_rect(page_window, ID_COMMAND);
    if (command_before_color.top != command_on_frame_structure.top || command_before_color.bottom != command_on_frame_structure.bottom) {
        std::wcerr << L"FFmpeg command area did not remain fixed below the Frame structure sub-tab.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 33;
    }
    TabCtrl_SetCurSel(video_tab, 3);
    SendMessageW(page_window, WM_NOTIFY, ID_VIDEO_TAB, reinterpret_cast<LPARAM>(&video_tab_change));
    const int vsr_ids[]{ID_VSR_ENABLED, ID_VSR_SCALE, ID_VSR_QUALITY,
                        ID_LABEL_VSR_ENABLED, ID_LABEL_VSR_SCALE, ID_LABEL_VSR_QUALITY};
    for (const int id : vsr_ids) {
        if (!has_visible_style(GetDlgItem(page_window, id)) || !valid_rect(child_rect(page_window, id))) {
            std::wcerr << L"Super resolution sub-tab control is unavailable: " << id << L"\n";
            page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 40;
        }
    }
    const RECT command_on_vsr = child_rect(page_window, ID_COMMAND);
    if (command_before_color.top != command_on_vsr.top || command_before_color.bottom != command_on_vsr.bottom) {
        std::wcerr << L"FFmpeg command area did not remain fixed below the Super resolution sub-tab.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 41;
    }
    TabCtrl_SetCurSel(video_tab, 0);
    video_tab_change.code = TCN_SELCHANGE;
    SendMessageW(page_window, WM_NOTIFY, ID_VIDEO_TAB, reinterpret_cast<LPARAM>(&video_tab_change));
    HWND codec_combo = GetDlgItem(page_window, ID_CODEC);
    HWND backend_combo = GetDlgItem(page_window, ID_BACKEND);
    HWND depth_combo = GetDlgItem(page_window, ID_DEPTH);
    HWND alpha_combo = GetDlgItem(page_window, ID_ALPHA);
    HWND frame_mode_combo = GetDlgItem(page_window, ID_FRAME_MODE);
    HWND gop_edit = GetDlgItem(page_window, ID_GOP);
    HWND bframes_edit = GetDlgItem(page_window, ID_BFRAMES);
    HWND cpu_threads_combo = GetDlgItem(page_window, ID_CPU_THREADS);
    HWND vsr_enabled_combo = GetDlgItem(page_window, ID_VSR_ENABLED);
    if (SendMessageW(backend_combo, CB_GETCOUNT, 0, 0) != 5 ||
        combo_item_text(backend_combo, 1) != L"NVENC" ||
        combo_item_text(backend_combo, 2) != L"NVEncC (NVIDIA)") {
        std::wcerr << L"Encoder list must distinguish FFmpeg NVENC from NVIDIA NVEncC. count="
                   << SendMessageW(backend_combo, CB_GETCOUNT, 0, 0) << L" item1=\""
                   << combo_item_text(backend_combo, 1) << L"\" item2=\""
                   << combo_item_text(backend_combo, 2) << L"\"\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 47;
    }
    SendMessageW(alpha_combo, CB_SETCURSEL, 0, 0);
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_ALPHA, CBN_SELCHANGE), reinterpret_cast<LPARAM>(alpha_combo));
    SendMessageW(codec_combo, CB_SETCURSEL, 1, 0);
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_CODEC, CBN_SELCHANGE), reinterpret_cast<LPARAM>(codec_combo));
    SendMessageW(backend_combo, CB_SETCURSEL, 1, 0);
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_BACKEND, CBN_SELCHANGE), reinterpret_cast<LPARAM>(backend_combo));
    SendMessageW(vsr_enabled_combo, CB_SETCURSEL, 1, 0);
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_VSR_ENABLED, CBN_SELCHANGE),
                 reinterpret_cast<LPARAM>(vsr_enabled_combo));
    if (!has_visible_style(GetDlgItem(page_window, ID_COMPAT_WARNING)) ||
        window_text(GetDlgItem(page_window, ID_COMMAND)).find(L"hevc_nvenc") == std::wstring::npos ||
        !has_visible_style(GetDlgItem(page_window, ID_COMMAND_PREFIX))) {
        std::wcerr << L"FFmpeg NVENC with VSR must remain an FFmpeg command and show an unavailable warning.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 48;
    }
    SendMessageW(backend_combo, CB_SETCURSEL, 2, 0);
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_BACKEND, CBN_SELCHANGE), reinterpret_cast<LPARAM>(backend_combo));
    SendMessageW(frame_mode_combo, CB_SETCURSEL, 0, 0);
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_FRAME_MODE, CBN_SELCHANGE),
                 reinterpret_cast<LPARAM>(frame_mode_combo));
    const std::wstring nvencc_prefix = window_text(GetDlgItem(page_window, ID_COMMAND_PREFIX));
    const std::wstring nvencc_command = window_text(GetDlgItem(page_window, ID_COMMAND));
    const LONG_PTR nvencc_command_style = GetWindowLongPtrW(GetDlgItem(page_window, ID_COMMAND), GWL_STYLE);
    const RECT nvencc_prefix_bounds = child_rect(page_window, ID_COMMAND_PREFIX);
    const RECT nvencc_command_bounds = child_rect(page_window, ID_COMMAND);
    if ((nvencc_command_style & ES_READONLY) != 0 ||
        !has_visible_style(GetDlgItem(page_window, ID_COMMAND_PREFIX)) ||
        has_visible_style(GetDlgItem(page_window, ID_COMMAND_SUFFIX)) ||
        nvencc_prefix.find(L"\"NVEncC.exe\"") == std::wstring::npos ||
        nvencc_prefix.find(L"--output \"{output}\"") == std::wstring::npos ||
        nvencc_prefix.find(L"--width {width} --height {height}") == std::wstring::npos ||
        nvencc_prefix.find(L"--fps {fps}") == std::wstring::npos ||
        nvencc_prefix.find(L"--input-format {input_pixel_format}") == std::wstring::npos ||
        nvencc_prefix.find(L"mmd2ffmpeg_vsr_bridge.exe") != std::wstring::npos ||
        nvencc_prefix.find(L"--ffmpeg ") != std::wstring::npos ||
        nvencc_command.rfind(L"--vsr --scale ", 0) != 0 ||
        nvencc_command.find(L" --quality ") == std::wstring::npos ||
        nvencc_command.find(L"--metadata date_recorded=") == std::wstring::npos ||
        nvencc_command.find(L"--frame-mode auto") == std::wstring::npos ||
        nvencc_command.find(L"NVEncC.exe") != std::wstring::npos ||
        nvencc_command.find(L"{output}") != std::wstring::npos ||
        nvencc_command.find(L"{width}") != std::wstring::npos ||
        nvencc_command.find(L"{input_pixel_format}") != std::wstring::npos ||
        nvencc_command.find(L"--gop ") != std::wstring::npos ||
        nvencc_command.find(L"--bframes ") != std::wstring::npos ||
        nvencc_command_bounds.top - nvencc_prefix_bounds.bottom > 32) {
        std::wcerr << L"NVEncC must show a fixed compact prefix and only editable middle arguments.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 49;
    }
    SendMessageW(frame_mode_combo, CB_SETCURSEL, 1, 0);
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_FRAME_MODE, CBN_SELCHANGE),
                 reinterpret_cast<LPARAM>(frame_mode_combo));
    SetWindowTextW(gop_edit, L"120");
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_GOP, EN_CHANGE), reinterpret_cast<LPARAM>(gop_edit));
    SetWindowTextW(bframes_edit, L"2");
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_BFRAMES, EN_CHANGE), reinterpret_cast<LPARAM>(bframes_edit));
    const std::wstring manual_nvencc_command = window_text(GetDlgItem(page_window, ID_COMMAND));
    if (manual_nvencc_command.find(L"--frame-mode manual") == std::wstring::npos ||
        manual_nvencc_command.find(L"--gop 120") == std::wstring::npos ||
        manual_nvencc_command.find(L"--bframes 2") == std::wstring::npos) {
        std::wcerr << L"NVEncC manual mode must include the selected GOP and B-frame options.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 51;
    }
    SendMessageW(frame_mode_combo, CB_SETCURSEL, 0, 0);
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_FRAME_MODE, CBN_SELCHANGE),
                 reinterpret_cast<LPARAM>(frame_mode_combo));
    const std::wstring restored_auto_nvencc_command = window_text(GetDlgItem(page_window, ID_COMMAND));
    if (restored_auto_nvencc_command.find(L"--gop ") != std::wstring::npos ||
        restored_auto_nvencc_command.find(L"--bframes ") != std::wstring::npos) {
        std::wcerr << L"Switching NVEncC back to automatic mode must remove GOP and B-frame options.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 52;
    }
    const std::wstring edited_nvencc_command = restored_auto_nvencc_command + L" --ui-edit-smoke";
    SetWindowTextW(GetDlgItem(page_window, ID_COMMAND), edited_nvencc_command.c_str());
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_COMMAND, EN_CHANGE),
                 reinterpret_cast<LPARAM>(GetDlgItem(page_window, ID_COMMAND)));
    if (window_text(GetDlgItem(page_window, ID_COMMAND)) != edited_nvencc_command) {
        std::wcerr << L"NVEncC command edits were not retained.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 50;
    }
    SendMessageW(vsr_enabled_combo, CB_SETCURSEL, 0, 0);
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_VSR_ENABLED, CBN_SELCHANGE),
                 reinterpret_cast<LPARAM>(vsr_enabled_combo));
    const std::wstring nvencc_without_vsr = window_text(GetDlgItem(page_window, ID_COMMAND));
    if (nvencc_without_vsr.find(L"--vsr") != std::wstring::npos ||
        nvencc_without_vsr.find(L"--scale ") != std::wstring::npos ||
        nvencc_without_vsr.find(L"--quality ") != std::wstring::npos ||
        nvencc_without_vsr.rfind(L"--depth ", 0) != 0) {
        std::wcerr << L"Disabled VSR must omit its flag, scale, and quality from the NVEncC command.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 54;
    }
    SendMessageW(alpha_combo, CB_SETCURSEL, 0, 0);
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_ALPHA, CBN_SELCHANGE), reinterpret_cast<LPARAM>(alpha_combo));
    SendMessageW(codec_combo, CB_SETCURSEL, 0, 0);
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_CODEC, CBN_SELCHANGE), reinterpret_cast<LPARAM>(codec_combo));
    SendMessageW(backend_combo, CB_SETCURSEL, 0, 0);
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_BACKEND, CBN_SELCHANGE), reinterpret_cast<LPARAM>(backend_combo));
    SendMessageW(cpu_threads_combo, CB_SETCURSEL, 1, 0);
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_CPU_THREADS, CBN_SELCHANGE), reinterpret_cast<LPARAM>(cpu_threads_combo));
    DWORD processor_count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (processor_count == 0) { SYSTEM_INFO system_info{}; GetSystemInfo(&system_info); processor_count = system_info.dwNumberOfProcessors; }
    const std::wstring all_threads_command = window_text(GetDlgItem(page_window, ID_COMMAND));
    if (all_threads_command.find(L"-threads " + std::to_wstring(std::max<DWORD>(1, processor_count))) == std::wstring::npos) {
        std::wcerr << L"All CPU threads selection was not mapped to the FFmpeg command.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 43;
    }
    SendMessageW(cpu_threads_combo, CB_SETCURSEL, 0, 0);
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_CPU_THREADS, CBN_SELCHANGE), reinterpret_cast<LPARAM>(cpu_threads_combo));
    if (window_text(GetDlgItem(page_window, ID_COMMAND)).find(L"-threads ") != std::wstring::npos) {
        std::wcerr << L"Automatic CPU threads selection must omit the FFmpeg threads argument.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 44;
    }
    SendMessageW(frame_mode_combo, CB_SETCURSEL, 0, 0);
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_FRAME_MODE, CBN_SELCHANGE), reinterpret_cast<LPARAM>(frame_mode_combo));
    const std::wstring automatic_frame_command = window_text(GetDlgItem(page_window, ID_COMMAND));
    if (automatic_frame_command.find(L"-g ") != std::wstring::npos || automatic_frame_command.find(L"-bf ") != std::wstring::npos) {
        std::wcerr << L"Automatic frame structure must omit FFmpeg frame arguments.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 36;
    }
    const std::wstring command_prefix = window_text(GetDlgItem(page_window, ID_COMMAND_PREFIX));
    const std::wstring editable_command = window_text(GetDlgItem(page_window, ID_COMMAND));
    const std::wstring command_suffix = window_text(GetDlgItem(page_window, ID_COMMAND_SUFFIX));
    if (command_prefix.find(L"-i pipe:0 ") == std::wstring::npos ||
        editable_command.find(L"-vf scale=") == std::wstring::npos || editable_command.find(L"-c:v ") == std::wstring::npos ||
        editable_command.find(L"-pix_fmt ") == std::wstring::npos || !command_suffix.empty() ||
        has_visible_style(GetDlgItem(page_window, ID_COMMAND_SUFFIX))) {
        std::wcerr << L"FFmpeg output must remain hidden while the four-line middle section stays editable.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 40;
    }
    HWND mask_output_combo = GetDlgItem(page_window, ID_MASK_OUTPUT);
    SendMessageW(alpha_combo, CB_SETCURSEL, 2, 0);
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_ALPHA, CBN_SELCHANGE), reinterpret_cast<LPARAM>(alpha_combo));
    SendMessageW(mask_output_combo, CB_SETCURSEL, 0, 0);
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_MASK_OUTPUT, CBN_SELCHANGE), reinterpret_cast<LPARAM>(mask_output_combo));
    const std::wstring stacked_command = window_text(GetDlgItem(page_window, ID_COMMAND));
    if (stacked_command.find(L"-filter_complex ") == std::wstring::npos || stacked_command.find(L"vstack=inputs=2") == std::wstring::npos ||
        !window_text(GetDlgItem(page_window, ID_COMMAND_SUFFIX)).empty()) {
        std::wcerr << L"Stacked alpha command display does not match the final FFmpeg command.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 41;
    }
    SendMessageW(mask_output_combo, CB_SETCURSEL, 1, 0);
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_MASK_OUTPUT, CBN_SELCHANGE), reinterpret_cast<LPARAM>(mask_output_combo));
    const std::wstring separate_command = window_text(GetDlgItem(page_window, ID_COMMAND));
    const std::wstring separate_suffix = window_text(GetDlgItem(page_window, ID_COMMAND_SUFFIX));
    if (separate_command.find(L"-filter_complex ") == std::wstring::npos || separate_command.find(L"-map \"[colorout]\"") == std::wstring::npos ||
        !separate_suffix.empty()) {
        std::wcerr << L"Separate alpha command display does not match the final FFmpeg command.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 42;
    }
    SendMessageW(alpha_combo, CB_SETCURSEL, 0, 0);
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_ALPHA, CBN_SELCHANGE), reinterpret_cast<LPARAM>(alpha_combo));
    SendMessageW(frame_mode_combo, CB_SETCURSEL, 1, 0);
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_FRAME_MODE, CBN_SELCHANGE), reinterpret_cast<LPARAM>(frame_mode_combo));
    if (!IsWindowEnabled(gop_edit) || !IsWindowEnabled(bframes_edit)) {
        std::wcerr << L"Frame structure controls must remain enabled in manual mode.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 37;
    }
    SetWindowTextW(gop_edit, L"120");
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_GOP, EN_CHANGE), reinterpret_cast<LPARAM>(gop_edit));
    SetWindowTextW(bframes_edit, L"2");
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_BFRAMES, EN_CHANGE), reinterpret_cast<LPARAM>(bframes_edit));
    const std::wstring frame_command = window_text(GetDlgItem(page_window, ID_COMMAND));
    if (frame_command.find(L"-g 120") == std::wstring::npos || frame_command.find(L"-bf 2") == std::wstring::npos) {
        std::wcerr << L"GOP and B-frame settings were not mapped to the FFmpeg command.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 34;
    }
    SendMessageW(codec_combo, CB_SETCURSEL, 3, 0);
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_CODEC, CBN_SELCHANGE), reinterpret_cast<LPARAM>(codec_combo));
    if (!IsWindowEnabled(depth_combo)) {
        std::wcerr << L"VP9 must expose the 10-bit depth option.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 27;
    }
    SendMessageW(depth_combo, CB_SETCURSEL, 1, 0);
    SendMessageW(page_window, WM_COMMAND, MAKEWPARAM(ID_DEPTH, CBN_SELCHANGE), reinterpret_cast<LPARAM>(depth_combo));
    if (SendMessageW(depth_combo, CB_GETCURSEL, 0, 0) != 1) {
        std::wcerr << L"VP9 10-bit selection was not retained.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 28;
    }
    if (!IsWindowEnabled(bframes_edit) || !has_visible_style(GetDlgItem(page_window, ID_COMPAT_WARNING)) ||
        window_text(GetDlgItem(page_window, ID_COMPAT_WARNING)) != L"\x26A0") {
        std::wcerr << L"Potentially unsupported VP9 B-frames must keep the control enabled and show a warning.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 35;
    }
    std::array<wchar_t, 16> github_class{};
    GetClassNameW(GetDlgItem(page_window, ID_GITHUB_LINK), github_class.data(), static_cast<int>(github_class.size()));
    if (wcscmp(github_class.data(), L"Edit") != 0) {
        std::wcerr << L"GitHub address must be a selectable edit control.\n";
        page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 13;
    }
    const RECT cpu_threads_label = child_rect(page_window, ID_LABEL_CPU_THREADS);
    const RECT settings_info = child_rect(page_window, ID_SETTINGS_INFO);
    const RECT github_address = child_rect(page_window, ID_GITHUB_LINK);
    if (settings_info.top - cpu_threads_label.bottom > 48 || github_address.top - settings_info.bottom > 48) {
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
            has_visible_style(GetDlgItem(page_window, ID_CPU_THREADS)) != expected.settings ||
            has_visible_style(GetDlgItem(page_window, ID_SETTINGS_INFO)) != expected.settings) {
            std::wcerr << L"Tab visibility failed for page " << expected.page << L".\n";
            page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 12;
        }
        const int* page_ids = nullptr;
        int page_id_count = 0;
        const int audio_ids[]{ID_AUDIO_INTRO, ID_LABEL_AUDIO_FORMAT, ID_AUDIO_FORMAT, ID_AUDIO_HELP,
                              ID_LABEL_AUDIO_RATE, ID_AUDIO_RATE};
        const int settings_ids[]{ID_LABEL_LANGUAGE, ID_LANGUAGE, ID_LABEL_CPU_THREADS, ID_CPU_THREADS, ID_SETTINGS_INFO, ID_GITHUB_LINK};
        if (expected.audio) { page_ids = audio_ids; page_id_count = static_cast<int>(std::size(audio_ids)); }
        if (expected.settings) { page_ids = settings_ids; page_id_count = static_cast<int>(std::size(settings_ids)); }
        GetClientRect(page_window, &client);
        for (int index = 0; index < page_id_count; ++index) {
            const RECT rectangle = child_rect(page_window, page_ids[index]);
            if (!valid_rect(rectangle) || rectangle.left < 0 || rectangle.right > client.right ||
                rectangle.top < 0 || rectangle.bottom > client.bottom) {
                std::wcerr << L"Visible tab control outside page: " << page_ids[index] << L"\n";
                page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 18;
            }
            const bool has_common_left_boundary = page_ids[index] != ID_AUDIO_FORMAT &&
                                                  page_ids[index] != ID_AUDIO_RATE &&
                                                  page_ids[index] != ID_LANGUAGE &&
                                                  page_ids[index] != ID_CPU_THREADS;
            if (has_common_left_boundary && rectangle.left != child_rect(page_window, ID_TAB).left) {
                std::wcerr << L"Visible tab control does not share the common left boundary: " << page_ids[index] << L"\n";
                page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 20;
            }
        }
        const RECT active_tab = child_rect(page_window, ID_TAB);
        if ((expected.audio && (child_rect(page_window, ID_AUDIO_FORMAT).right != active_tab.right ||
                                child_rect(page_window, ID_AUDIO_RATE).right != active_tab.right)) ||
            (expected.settings && (child_rect(page_window, ID_LANGUAGE).right != active_tab.right ||
                                   child_rect(page_window, ID_CPU_THREADS).right != active_tab.right))) {
            std::wcerr << L"Visible dropdown does not share the tab right boundary.\n";
            page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 19;
        }
    }
    SendMessageW(GetDlgItem(page_window, ID_TAB), TCM_SETCURSEL, 0, 0);
    NMHDR video_selected{GetDlgItem(page_window, ID_TAB), ID_TAB, TCN_SELCHANGE};
    SendMessageW(page_window, WM_NOTIFY, ID_TAB, reinterpret_cast<LPARAM>(&video_selected));
    const RECT prefix = child_rect(page_window, ID_COMMAND_PREFIX);
    const RECT command = child_rect(page_window, ID_COMMAND);
    if (prefix.bottom > command.top ||
        command.bottom - command.top != nvencc_command_bounds.bottom - nvencc_command_bounds.top ||
        has_visible_style(GetDlgItem(page_window, ID_COMMAND_SUFFIX))) {
        std::wcerr << L"Command controls overlap or the editable command is not fixed at four lines.\n";
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
    const wchar_t* status_heading_labels[]{L"編碼器狀態", L"編碼器狀態", L"编码器状态", L"エンコーダー状態", L"Encoder status"};
    const wchar_t* not_tested_labels[]{L"尚未測試", L"尚未測試", L"尚未测试", L"未テスト", L"Not tested"};
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
            window_text(GetDlgItem(page_window, ID_LABEL_STATUS)) != status_heading_labels[text_index] ||
            window_text(status_control) != not_tested_labels[text_index]) {
            std::wcerr << L"Language switch failed for index " << language << L".\n";
            page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 9;
        }
        const RECT status_bounds = child_rect(page_window, ID_STATUS);
        if (constrained_host) SendMessageW(page_window, WM_VSCROLL, SB_BOTTOM, 0);
        GetClientRect(page_window, &client);
        const RECT requirement_bounds = child_rect(page_window, ID_TEST_REQUIREMENT);
        const RECT test_bounds = child_rect(page_window, ID_REFRESH);
        const RECT log_bounds = child_rect(page_window, ID_OPEN_LOG);
        if ((!constrained_host && requirement_bounds.top - status_bounds.bottom > 48) ||
            requirement_bounds.top < 0 || requirement_bounds.bottom > client.bottom ||
            test_bounds.top < 0 || test_bounds.bottom > client.bottom ||
            log_bounds.top < 0 || log_bounds.bottom > client.bottom || test_bounds.right >= log_bounds.left) {
            std::wcerr << L"Localized action row is clipped, spaced too far from status, or overlaps. status="
                       << status_bounds.left << L"," << status_bounds.top << L"," << status_bounds.right << L","
                       << status_bounds.bottom << L" requirement=" << requirement_bounds.left << L","
                       << requirement_bounds.top << L"," << requirement_bounds.right << L"," << requirement_bounds.bottom
                       << L" test=" << test_bounds.left << L"," << test_bounds.top << L"," << test_bounds.right << L","
                       << test_bounds.bottom << L" log=" << log_bounds.left << L"," << log_bounds.top << L","
                       << log_bounds.right << L"," << log_bounds.bottom << L" client=" << client.right << L","
                       << client.bottom << L"\n";
            page->Deactivate(); DestroyWindow(parent); page->Release(); CoUninitialize(); return 21;
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
