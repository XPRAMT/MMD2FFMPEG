#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void log_line(const std::filesystem::path& log, const std::wstring& text) {
    std::wofstream stream(log, std::ios::app);
    stream << L"\n[MMD2FFMPEG] " << text << L"\n";
}

bool wait_for_release(const std::filesystem::path& avi) {
    for (int attempt = 0; attempt < 300; ++attempt) {
        HANDLE handle = CreateFileW(avi.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) { CloseHandle(handle); return true; }
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return false;
        Sleep(1000);
    }
    return false;
}

bool run_ffmpeg(const std::filesystem::path& ffmpeg, const std::wstring& command) {
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(ffmpeg.c_str(), mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) return false;
    WaitForSingleObject(process.hProcess, 300000);
    DWORD exit_code = 1; GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return exit_code == 0;
}

int probe_audio_rate(const std::filesystem::path& ffmpeg, const std::filesystem::path& avi) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE read_pipe = nullptr, write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 65536)) return 0;
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    std::wstring command = L"\"" + ffmpeg.wstring() + L"\" -hide_banner -i \"" + avi.wstring() + L"\"";
    std::vector<wchar_t> mutable_command(command.begin(), command.end()); mutable_command.push_back(L'\0');
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE); startup.hStdOutput = write_pipe; startup.hStdError = write_pipe;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(ffmpeg.c_str(), mutable_command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        CloseHandle(read_pipe); CloseHandle(write_pipe); return 0;
    }
    CloseHandle(write_pipe); std::string output; std::array<char, 4096> buffer{}; DWORD read = 0;
    while (output.size() < 65536 && ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read)
        output.append(buffer.data(), read);
    CloseHandle(read_pipe); WaitForSingleObject(process.hProcess, 5000); CloseHandle(process.hThread); CloseHandle(process.hProcess);
    const auto audio = output.find("Audio:"); const auto hz = output.find(" Hz", audio);
    if (audio == std::string::npos || hz == std::string::npos) return 0;
    std::size_t begin = hz;
    while (begin > audio && output[begin - 1] >= '0' && output[begin - 1] <= '9') --begin;
    try { return std::stoi(output.substr(begin, hz - begin)); } catch (...) { return 0; }
}

}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments || count != 8) { if (arguments) LocalFree(arguments); return 2; }
    const std::filesystem::path avi = arguments[1];
    const std::filesystem::path mkv = arguments[2];
    const std::filesystem::path ffmpeg = arguments[3];
    const std::wstring format = arguments[4];
    const std::wstring rate = arguments[5];
    const std::wstring depth = arguments[6];
    const std::filesystem::path log = arguments[7];
    LocalFree(arguments);
    if (!avi.is_absolute() || !mkv.is_absolute() || _wcsicmp(avi.extension().c_str(), L".avi") != 0) return 3;
    if (!wait_for_release(avi)) { log_line(log, L"AVI was not released within 5 minutes: " + avi.wstring()); return 1; }
    if (format == L"none") {
        if (DeleteFileW(avi.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) { log_line(log, L"Placeholder AVI deleted: " + avi.wstring()); return 0; }
        log_line(log, L"Could not delete placeholder AVI: " + avi.wstring()); return 1;
    }
    const auto temporary = mkv.parent_path() / (mkv.stem().wstring() + L".mmd2ffmpeg-audio-partial" + mkv.extension().wstring());
    std::error_code error; std::filesystem::remove(temporary, error);
    std::wstring audio = format == L"flac" ? L"-c:a flac" : depth == L"24" ? L"-c:a pcm_s24le" : L"-c:a copy";
    if (depth == L"24" && format == L"flac") audio += L" -sample_fmt s32";
    if (rate == L"hires") {
        const int source_rate = probe_audio_rate(ffmpeg, avi);
        if (source_rate > 0 && source_rate < 48000) audio += L" -ar " + std::to_wstring(std::max(48000, source_rate * 2));
    }
    const std::wstring command = L"\"" + ffmpeg.wstring() + L"\" -hide_banner -loglevel warning -y -i \"" + mkv.wstring() +
                                 L"\" -i \"" + avi.wstring() + L"\" -map 0:v:0 -map 1:a:0 -c:v copy " + audio +
                                 L" \"" + temporary.wstring() + L"\"";
    if (!run_ffmpeg(ffmpeg, command) || !std::filesystem::exists(temporary, error) ||
        !MoveFileExW(temporary.c_str(), mkv.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, error);
        log_line(log, L"Audio merge failed; AVI retained for inspection: " + avi.wstring());
        return 1;
    }
    if (DeleteFileW(avi.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) {
        log_line(log, L"Audio merged into MKV and placeholder AVI deleted: " + avi.wstring());
        return 0;
    }
    log_line(log, L"Audio merged, but AVI could not be deleted: " + avi.wstring());
    return 1;
}
