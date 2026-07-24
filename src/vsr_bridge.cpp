#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::wstring quote(const std::wstring& value) { return L"\"" + value + L"\""; }

void close_handle(HANDLE& handle) {
    if (handle) {
        CloseHandle(handle);
        handle = nullptr;
    }
}

std::wstring argument(const std::vector<std::wstring>& args, const std::wstring& key,
                      const std::wstring& fallback = L"") {
    for (std::size_t index = 0; index + 1 < args.size(); ++index)
        if (args[index] == key) return args[index + 1];
    return fallback;
}

bool flag(const std::vector<std::wstring>& args, const std::wstring& key) {
    return std::find(args.begin(), args.end(), key) != args.end();
}

bool start_process(const std::filesystem::path& executable, std::wstring command,
                   HANDLE input, HANDLE output, HANDLE error, PROCESS_INFORMATION& process) {
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = output;
    startup.hStdError = error;
    return CreateProcessW(executable.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::vector<std::wstring> args(argv + 1, argv + argc);
    const std::filesystem::path ffmpeg = argument(args, L"--ffmpeg");
    const std::filesystem::path nvenc = argument(args, L"--nvenc");
    const std::filesystem::path output = argument(args, L"--output");
    if (ffmpeg.empty() || nvenc.empty() || output.empty()) return 2;

    const int width = std::max(1, _wtoi(argument(args, L"--width", L"1920").c_str()));
    const int height = std::max(1, _wtoi(argument(args, L"--height", L"1080").c_str()));
    const int fps = std::max(1, _wtoi(argument(args, L"--fps", L"30").c_str()));
    const double scale = std::clamp(_wtof(argument(args, L"--scale", L"2").c_str()), 1.0, 4.0);
    const int quality = std::clamp(_wtoi(argument(args, L"--quality", L"2").c_str()), 0, 4);
    const bool alpha = flag(args, L"--alpha");
    const bool vsr = flag(args, L"--vsr");
    const bool probe = flag(args, L"--probe");
    const int output_width = std::max(1, static_cast<int>(width * scale + 0.5));
    const int output_height = std::max(1, static_cast<int>(height * scale + 0.5));

    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE nut_read = nullptr, nut_write = nullptr;
    if (!CreatePipe(&nut_read, &nut_write, &security, 1024 * 1024)) return 3;

    const HANDLE log = GetStdHandle(STD_ERROR_HANDLE);
    std::wstring nvenc_command = quote(nvenc.wstring()) + L" --avsw -i - -o " + quote(output.wstring()) +
        L" --codec hevc";
    if (vsr)
        nvenc_command += L" --output-res " + std::to_wstring(output_width) + L"x" + std::to_wstring(output_height) +
                         L" --vpp-resize algo=ngx-vsr,vsr-quality=" + std::to_wstring(quality);
    nvenc_command +=
        L" --output-csp " + (alpha ? L"yuva420" : argument(args, L"--output-csp", L"yuv420")) +
        L" --output-depth " + (alpha ? L"8" : argument(args, L"--depth", L"10")) +
        L" --preset " + argument(args, L"--preset", L"p6");
    const auto rate = argument(args, L"--rate", L"qp");
    if (rate == L"vbr") nvenc_command += L" --vbr " + argument(args, L"--bitrate", L"20000");
    else if (rate == L"crf") nvenc_command += L" --qvbr " + argument(args, L"--qp", L"18");
    else nvenc_command += L" --cqp " + argument(args, L"--qp", L"18");
    if (argument(args, L"--frame-mode") == L"manual")
        nvenc_command += L" --gop-len " + argument(args, L"--gop", L"120") +
                         L" --bframes " + argument(args, L"--bframes", L"3");

    PROCESS_INFORMATION nvenc_process{};
    SetHandleInformation(nut_write, HANDLE_FLAG_INHERIT, 0);
    if (!start_process(nvenc, nvenc_command, nut_read, log, log, nvenc_process)) {
        close_handle(nut_read); close_handle(nut_write); return 4;
    }
    close_handle(nut_read);
    SetHandleInformation(nut_write, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    std::wstring ffmpeg_command = quote(ffmpeg.wstring()) + L" -hide_banner -loglevel warning -y ";
    if (probe)
        ffmpeg_command += L"-f lavfi -i color=c=black@0.5:s=" + std::to_wstring(width) + L"x" +
                          std::to_wstring(height) + L":r=" + std::to_wstring(fps) + L",format=rgba -frames:v 1 ";
    else
        ffmpeg_command += L"-f rawvideo -pixel_format " + argument(args, L"--input-format", L"bgra") +
                          L" -video_size " + std::to_wstring(width) + L"x" + std::to_wstring(height) +
                          L" -framerate " + std::to_wstring(fps) + L" -i pipe:0 ";
    ffmpeg_command += L"-vf format=rgba -c:v rawvideo -pix_fmt rgba -f nut pipe:1";

    PROCESS_INFORMATION ffmpeg_process{};
    const HANDLE ffmpeg_input = probe ? GetStdHandle(STD_INPUT_HANDLE) : GetStdHandle(STD_INPUT_HANDLE);
    if (!start_process(ffmpeg, ffmpeg_command, ffmpeg_input, nut_write, log, ffmpeg_process)) {
        TerminateProcess(nvenc_process.hProcess, 1);
        close_handle(nut_write);
        CloseHandle(nvenc_process.hThread); CloseHandle(nvenc_process.hProcess);
        return 5;
    }
    close_handle(nut_write);
    WaitForSingleObject(ffmpeg_process.hProcess, INFINITE);
    WaitForSingleObject(nvenc_process.hProcess, INFINITE);
    DWORD ffmpeg_exit = 1, nvenc_exit = 1;
    GetExitCodeProcess(ffmpeg_process.hProcess, &ffmpeg_exit);
    GetExitCodeProcess(nvenc_process.hProcess, &nvenc_exit);
    CloseHandle(ffmpeg_process.hThread); CloseHandle(ffmpeg_process.hProcess);
    CloseHandle(nvenc_process.hThread); CloseHandle(nvenc_process.hProcess);
    if (ffmpeg_exit != 0 || nvenc_exit != 0)
        std::wcerr << L"VSR bridge child exit codes: FFmpeg=" << ffmpeg_exit
                   << L", NVEncC=" << nvenc_exit << L"\n";
    return ffmpeg_exit == 0 && nvenc_exit == 0 ? 0 : 6;
}
