#include <windows.h>
#include <vfw.h>

#include <cstdint>
#include <iostream>
#include <vector>

int wmain() {
    constexpr DWORD handler = mmioFOURCC('m', '2', 'f', 'f');
    bool enumerated = false;
    ICINFO info{};
    info.dwSize = sizeof(info);
    for (DWORD index = 0; ICInfo(ICTYPE_VIDEO, index, &info); ++index) {
        if (info.fccHandler == handler) {
            enumerated = true;
            break;
        }
        info = {};
        info.dwSize = sizeof(info);
    }
    if (!enumerated) {
        std::wcerr << L"ICInfo did not enumerate the M2FF VFW codec.\n";
        info = {};
        info.dwSize = sizeof(info);
        for (DWORD index = 0; ICInfo(ICTYPE_VIDEO, index, &info); ++index) {
            std::wcerr << L"  handler="
                       << static_cast<wchar_t>(info.fccHandler & 0xff)
                       << static_cast<wchar_t>((info.fccHandler >> 8) & 0xff)
                       << static_cast<wchar_t>((info.fccHandler >> 16) & 0xff)
                       << static_cast<wchar_t>((info.fccHandler >> 24) & 0xff)
                       << L" name=" << info.szName << L"\n";
            info = {};
            info.dwSize = sizeof(info);
        }
        return 4;
    }

    HIC codec = ICOpen(ICTYPE_VIDEO, handler, ICMODE_COMPRESS);
    if (!codec) {
        std::wcerr << L"ICOpen failed; install the codec first.\n";
        return 1;
    }

    BITMAPINFOHEADER input{};
    input.biSize = sizeof(input);
    constexpr int width = 640;
    constexpr int height = 360;
    input.biWidth = width;
    input.biHeight = height;
    input.biPlanes = 1;
    input.biBitCount = 24;
    input.biCompression = BI_RGB;
    input.biSizeImage = width * height * 3;

    BITMAPINFOHEADER output{};
    ICCOMPRESSFRAMES frames_info{};
    frames_info.dwFlags = ICCOMPRESSFRAMES_PADDING;
    frames_info.lpbiOutput = &output;
    frames_info.lpbiInput = &input;
    frames_info.dwRate = 60;
    frames_info.dwScale = 1;
    if (ICSendMessage(codec, ICM_COMPRESS_FRAMES_INFO,
                      reinterpret_cast<DWORD_PTR>(&frames_info), sizeof(frames_info)) != ICERR_OK ||
        ICCompressGetFormat(codec, &input, &output) != ICERR_OK ||
        ICCompressBegin(codec, &input, &output) != ICERR_OK) {
        std::wcerr << L"Codec rejected the test format.\n";
        ICClose(codec);
        return 2;
    }

    std::vector<std::uint8_t> frame(input.biSizeImage);
    std::uint8_t dummy = 0;
    for (DWORD index = 0; index < 30; ++index) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const auto offset = static_cast<std::size_t>((y * width + x) * 3);
                frame[offset + 0] = static_cast<std::uint8_t>(x * 255 / width);
                frame[offset + 1] = static_cast<std::uint8_t>(y * 255 / height);
                frame[offset + 2] = static_cast<std::uint8_t>(index * 8);
            }
        }
        DWORD flags = 0;
        const LRESULT result = ICCompress(codec, 0, &output, &dummy, &input, frame.data(),
                                          nullptr, &flags, index, 0, 0, nullptr, nullptr);
        if (result != ICERR_OK) {
            std::wcerr << L"ICCompress failed at frame " << index << L".\n";
            ICCompressEnd(codec);
            ICClose(codec);
            return 3;
        }
    }
    ICCompressEnd(codec);
    ICClose(codec);
    std::wcout << L"Streamed 30 frames through the installed codec.\n";
    return 0;
}
