# LoopCatch

Native, MIT-licensed C++17 audio recorder for Windows 10/11. Capture **all shared-mode system playback audio from one output device or one selected application's process tree** to a WAV file, including browser audio, games, and ordinary FL Studio output routed to that device. No virtual audio cable, Python, Electron, or added audio driver needed.

**Status: v0.2.0 prototype.** A Windows CI build checks that the application compiles. Audio capture on different Windows devices needs hands-on testing.

## Build / download

Windows 10/11, Visual Studio 2022 or later with **Desktop development with C++**, Windows SDK, and CMake 3.21+ are required to compile locally.

In PowerShell at the repo root:

~~~powershell
.\build.ps1
.\build\Release\LoopCatch.exe
~~~

Or visit **Actions > Windows C++ Build**, select a successful workflow run, and download the **LoopCatch-Windows-x64** artifact. Extract the ZIP to get the executable. The workflow runs for PRs, pushes, and manual dispatch.

## v0.2: record one application

On **Windows build 20348 or newer** (generally Windows 11), choose **One application / process tree** in Capture Mode. Click **Refresh apps**, select a visible app window, then click **Record WAV**. Only the selected PID and descendants are targeted, even when other applications play sounds.

If audio is actually emitted by a host/helper process that has no window, find its PID in Task Manager and enter it into the **manual PID** field. That field overrides the app dropdown. Process IDs change when programs restart. Capturing a process without shared-mode audio creates silence or an empty recording, depending on the audio endpoint and available packets.

On older Windows releases, use **Entire playback device**. This mode remains fully selectable without the newer API. Dedicated ASIO audio can still bypass both capture paths. App capture targets processes, not the browser tab or the audio plugin inside FL Studio.

## Record audio

1. Open LoopCatch.exe.
2. Select **Entire playback device** and choose your output, or **One application / process tree** on supported Windows builds.
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
- **Per-app limits:** Process capture includes the selected PID and descendants (not unrelated audio hosts) and requires Windows build 20348+. The visible-window picker is a convenience, not a list of active audio streams.
- **Recording rights:** Record only media you have the right to capture. The app does not bypass content protection.

## Next improvements

- Correct silent-gap accounting using device/QPC timestamps; more diagnostic details and device change handling.
- Better per-app audio-session discovery, device/PID diagnostics, and an audio activity test.
- RF64 or stream splitting for long recordings; FLAC export and hotkeys.
- Separate microphone track and more advanced audio interface routing.

## References

- [Microsoft WASAPI loopback recording](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording)
- [Microsoft application loopback sample](https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/ApplicationLoopback)

## License

MIT. See [LICENSE](LICENSE).

## Windows API references

- [Microsoft Application Loopback API sample](https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/ApplicationLoopback)
- [Microsoft process loopback activation requirements](https://learn.microsoft.com/en-us/windows/win32/api/audioclientactivationparams/ne-audioclientactivationparams-audioclient_activation_type)
