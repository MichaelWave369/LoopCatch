# LoopCatch

Native, MIT-licensed C++17 audio recorder for Windows 10/11. Capture **all shared-mode system playback audio from one selected output device** to a WAV file, including browser audio, games, and ordinary FL Studio output routed to that device. No virtual audio cable, Python, Electron, or added audio driver needed.

**Status: v0.1.0 prototype.** A Windows CI build checks that the application compiles. Audio capture on different Windows devices needs hands-on testing.

## Build / download

Windows 10/11, Visual Studio 2022 or later with **Desktop development with C++**, Windows SDK, and CMake 3.21+ are required to compile locally.

In PowerShell at the repo root:

~~~powershell
.\build.ps1
.\build\Release\LoopCatch.exe
~~~

Or visit **Actions > Windows C++ Build**, select a successful workflow run, and download the **LoopCatch-Windows-x64** artifact. Extract the ZIP to get the executable. The workflow runs for PRs, pushes, and manual dispatch.

## Record audio

1. Open LoopCatch.exe.
2. Choose the **System default playback device** or select your speakers, headphones, HDMI, or interface output.
3. Click **Record WAV** and pick a writable .wav destination.
4. Play sound in Chrome, Firefox, FL Studio, etc. through the selected Windows playback endpoint.
5. Click **Stop & Save**. Once capture finishes, the file is finalized.

The WAV stores the Windows device's native shared mix format, usually stereo floating-point audio. The level meter currently supports float32 and PCM16 mix formats.

## Important limitations

- **Direct ASIO is not universally capturable:** Dedicated ASIO audio can bypass Windows shared-mode processing. Use an FL Studio driver/routing path that also plays through a Windows output, or a supported hardware loopback input. This is not a universal ASIO interceptor.
- **One endpoint:** Captures a single selected render endpoint, not a mix of multiple independent outputs. "System default" is resolved when recording starts.
- **Silence and pauses:** Some output devices stop delivering loopback packets when playback goes idle. This early version writes actual received frames and explicit silent frames but does **not yet synthesize missing silent gaps**, so long inactive intervals may be shorter in the resulting file.
- **DRM/exclusive:** Protected content and exclusive-mode applications can be inaccessible through system loopback.
- **File size:** Standard RIFF WAV has a 4 GiB limit; the recorder stops and attempts to finalize when near it.
- **No per-app filtering yet:** The capture includes all applications routed through the chosen output.
- **Recording rights:** Record only media you have the right to capture. The app does not bypass content protection.

## Next improvements

- Correct silent-gap accounting using device/QPC timestamps; more diagnostic details and device change handling.
- Per-process loopback capture on supported Windows versions using Microsoft's process loopback API.
- RF64 or stream splitting for long recordings; FLAC export and hotkeys.
- Separate microphone track and more advanced audio interface routing.

## References

- [Microsoft WASAPI loopback recording](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording)
- [Microsoft application loopback sample](https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/ApplicationLoopback)

## License

MIT. See [LICENSE](LICENSE).
