#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments || count != 3) {
        if (arguments) LocalFree(arguments);
        return 2;
    }
    const std::filesystem::path avi = arguments[1];
    const std::filesystem::path log = arguments[2];
    LocalFree(arguments);
    if (!avi.is_absolute() || _wcsicmp(avi.extension().c_str(), L".avi") != 0) return 3;

    for (int attempt = 0; attempt < 300; ++attempt) {
        if (DeleteFileW(avi.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) {
            std::wofstream stream(log, std::ios::app);
            stream << L"\n[MMD2FFMPEG] Placeholder AVI deleted: " << avi.wstring() << L"\n";
            return 0;
        }
        Sleep(1000);
    }
    std::wofstream stream(log, std::ios::app);
    stream << L"\n[MMD2FFMPEG] Could not delete placeholder AVI after 5 minutes: " << avi.wstring() << L"\n";
    return 1;
}
