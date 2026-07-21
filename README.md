[English](README.md) | [繁體中文](README_TW.md)

# MMD2FFMPEG

MMD2FFMPEG is a 64-bit DirectX Media Object (DMO) encoder for MikuMikuDance 9.32 x64. It sends MMD-rendered RGB frames directly to FFmpeg, replacing the oversized AVI workflow with an MKV video produced beside the AVI path selected in MMD.

Audio is intentionally out of scope.

## Features

- Appears in MMD's AVI encoder list as **MMD2FFMPEG DMO Encoder**.
- Streams RGB24/RGB32 frames to FFmpeg through stdin; RGB32 alpha is not preserved.
- Creates MKV output with the same name and folder as MMD's AVI target. After a successful FFmpeg encode, MMD2FFMPEG automatically removes MMD's placeholder AVI; the AVI is preserved if encoding fails.
- Writes the MKV `DATE_RECORDED` metadata field automatically when encoding starts, using the local date in `yyyy-M-d` format.
- Supports CPU software encoding, NVIDIA NVENC, Intel Quick Sync, and AMD AMF through FFmpeg.
- Supports AVC, HEVC, and AV1; 8-bit and supported 10-bit output; CRF/CQ, constant QP, and target-bitrate modes.
- Uses the frame rate chosen by MMD and marks video as BT.709.
- Requires a successful manual encoder test before settings can be saved or applied.
- Includes Traditional Chinese, Simplified Chinese, Japanese, English, and system-default UI languages.
- Writes per-export diagnostics under `%LOCALAPPDATA%\MMD2FFMPEG\logs`, including FFmpeg version, input frames, measured input FPS, elapsed time, exit code, and output size.

## Default first-run encoder

The first generated configuration uses a compatible CPU baseline:

| Setting | Default |
| --- | --- |
| Encoder | CPU software (`libx265`) |
| Codec | HEVC / H.265 |
| Bit depth | 10-bit Main 10 |
| Preset | medium |
| Rate control | CRF |
| CRF | 18 |

## Requirements

- MikuMikuDance 9.32 x64.
- FFmpeg available as `ffmpeg.exe` on the system `PATH`.
- Windows x64.

Check the FFmpeg installation before building:

```powershell
ffmpeg -version
```

## Installation

### Recommended: install from a GitHub Release

1. Download `MMD2FFMPEG-x64.zip` from the repository's **Releases** page.
2. Extract the ZIP to a local folder.
3. Install an x64 FFmpeg build and add its folder to the system or user `PATH`.
4. Close MMD completely. The DMO DLL cannot be replaced while MMD has it loaded.
5. Double-click `install-user.ps1` in the extracted folder. If `.ps1` files open in an editor on the system, right-click it and choose **Run with PowerShell**.
6. Start MMD again. No Windows restart, administrator permission, Visual Studio, or `build.ps1` is required.

The installer registers only for the current Windows user and places the runtime files in `%LOCALAPPDATA%\MMD2FFMPEG`.

### Release package contents

| File | Purpose |
| --- | --- |
| `install-user.ps1` | Copies the runtime files to the current user's local MMD2FFMPEG folder, migrates compatible configuration, and registers the DMO for the current user. |
| `uninstall-user.ps1` | Removes the current user's DMO registration. Runtime files, configuration, and logs are deliberately retained for manual backup or removal. |
| `mmd2ffmpeg_dmo.dll` | The MMD-visible DirectX Media Object encoder. It receives MMD frames and streams them to FFmpeg to create the MKV. |
| `mmd2ffmpeg_cleanup.exe` | Runs only after a successful MKV encode and retries deletion of MMD's temporarily locked placeholder AVI, recording the result in the export log. |

### Build from source

1. Clone or download this repository.
2. Install Visual Studio 2022 with the **Desktop development with C++** workload.
3. Close MMD completely.
4. Build and register the per-user DMO:

   ```powershell
   & 'C:\APP\MMD\MMD2FFMPEG\scripts\build.ps1'
   & 'C:\APP\MMD\MMD2FFMPEG\scripts\install-user.ps1'
   ```

### Creating a Release package

Maintainers can create the GitHub Release asset after a successful build:

```powershell
& 'C:\APP\MMD\MMD2FFMPEG\scripts\make-release.ps1'
```

This creates `release\MMD2FFMPEG-x64\` and `release\MMD2FFMPEG-x64.zip`. Upload the ZIP as the GitHub Release asset.

`build.ps1` runs this packaging step automatically after every successful build.

## Use in MMD

1. Select **File > AVI Output** and choose the desired AVI save path.
2. In **Video encoder**, select **MMD2FFMPEG DMO Encoder**.
3. Open **Detailed settings**, configure the encoder, and use **Test encoder**.
4. Save or apply only after the test passes.
5. Start AVI output. The final MKV is written beside the selected AVI path; MMD's placeholder AVI is deleted automatically after successful encoding.

Use **Open log** in the encoder settings to open the dynamic per-user log folder.

## Updating and uninstalling

- **Update:** Close MMD, run the build command, then run `install-user.ps1` again. Existing `config.ini` is preserved.
- **Uninstall:** Close MMD and run `uninstall-user.ps1` in the Release package, or run from source:

  ```powershell
  & 'C:\APP\MMD\MMD2FFMPEG\scripts\uninstall-user.ps1'
  ```

  This removes the per-user DMO registration. Runtime files, configuration, and logs under `%LOCALAPPDATA%\MMD2FFMPEG` are intentionally left in place for manual backup or deletion.

## Configuration and diagnostics

| Item | Location |
| --- | --- |
| Encoder settings | MMD: **AVI Output > Video encoder > Detailed settings** |
| Output path | The AVI path selected in MMD; MMD2FFMPEG always changes its extension to `.mkv` |
| Per-user configuration | `%LOCALAPPDATA%\MMD2FFMPEG\config.ini` |
| Per-export logs | `%LOCALAPPDATA%\MMD2FFMPEG\logs` |

The advanced command field exposes the editable FFmpeg video-argument section. The fixed input, color conversion, output-container arguments, and output path remain controlled by MMD2FFMPEG. `config.ini` does not save an output path.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| Encoder is missing from MMD | Confirm that MMD is x64, rerun `install-user.ps1`, then restart MMD. |
| Test encoder fails | Run `ffmpeg -version`; ensure the selected hardware encoder is supported by the active FFmpeg, GPU, and driver. |
| Installer cannot replace the DLL | Close every MMD window and retry the installer. |
| AVI remains or no MKV appears | Open the log folder, inspect the latest log, and check the FFmpeg exit code. |
| Very large AVI file | Ensure **MMD2FFMPEG DMO Encoder** is selected and MMD did not fall back to uncompressed output. |

## Notes

- MMD output is SDR BT.709. HDR and audio muxing are not implemented.
- Hardware encoder availability depends on the installed FFmpeg build, GPU, and driver. Use **Test encoder** after changing encoder settings.
- The encoder launches `ffmpeg.exe` from `PATH`. Review custom FFmpeg arguments before saving them.

## License

No license file has been added to this repository yet.
