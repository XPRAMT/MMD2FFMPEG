#include <windows.h>
#include <vfw.h>

#include <cstdint>
#include <iostream>
#include <vector>

int wmain() {
    constexpr DWORD handler = mmioFOURCC('M', '2', 'F', 'F');
    HIC codec = ICOpen(ICTYPE_VIDEO, handler, ICMODE_COMPRESS);
    if (!codec) {
        std::wcerr << L"ICOpen failed; install the codec first.\n";
        return 1;
    }

    BITMAPINFOHEADER input{};
    input.biSize = sizeof(input);
    input.biWidth = 64;
    input.biHeight = 64;
    input.biPlanes = 1;
    input.biBitCount = 24;
    input.biCompression = BI_RGB;
    input.biSizeImage = 64 * 64 * 3;

    BITMAPINFOHEADER output{};
    if (ICCompressGetFormat(codec, &input, &output) != ICERR_OK ||
        ICCompressBegin(codec, &input, &output) != ICERR_OK) {
        std::wcerr << L"Codec rejected the test format.\n";
        ICClose(codec);
        return 2;
    }

    std::vector<std::uint8_t> frame(input.biSizeImage);
    std::uint8_t dummy = 0;
    for (DWORD index = 0; index < 30; ++index) {
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const auto offset = static_cast<std::size_t>((y * 64 + x) * 3);
                frame[offset + 0] = static_cast<std::uint8_t>(x * 4);
                frame[offset + 1] = static_cast<std::uint8_t>(y * 4);
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

