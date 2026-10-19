// LoopCatch: native C++17 Windows audio recorder.
// Copyright (c) 2026 LoopCatch contributors. MIT License.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <commdlg.h>
#include <commctrl.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
constexpr int ID_DEVICE = 101, ID_REFRESH = 102, ID_RECORD = 103;
constexpr int ID_STOP = 104, ID_STATUS = 105, ID_PATH = 106, ID_LEVEL = 107;
constexpr UINT WM_CAPTURE_DONE = WM_APP + 1;

struct Device { std::wstring id, name; };
std::vector<Device> g_devices;
HWND g_window{}, g_device{}, g_refresh{}, g_record{}, g_stop{}, g_status{}, g_path{}, g_level{};
std::atomic<bool> g_stopRequested{false};
std::atomic<uint64_t> g_audioBytes{0};
std::atomic<ULONGLONG> g_startedAt{0};
std::atomic<int> g_peak{0};
std::thread g_worker;
std::wstring g_result;
bool g_recording = false;

std::wstring ErrorCode(HRESULT value) {
    std::wostringstream s;
    s << L"0x" << std::uppercase << std::hex << std::setfill(L'0')
      << std::setw(8) << static_cast<unsigned long>(value);
    return s.str();
}
std::wstring WinError(DWORD value) {
    return L"Windows error " + std::to_wstring(value);
}
std::wstring DeviceName(IMMDevice* device) {
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store)))
        return L"Unnamed playback device";
    PROPVARIANT name;
    PropVariantInit(&name);
    std::wstring text = L"Unnamed playback device";
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &name)) &&
        name.vt == VT_LPWSTR && name.pwszVal)
        text = name.pwszVal;
    PropVariantClear(&name);
    return text;
}
bool RefreshDevices(std::wstring& error) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        error = L"Audio enumeration failed: " + ErrorCode(hr);
        return false;
    }
    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) {
        error = L"Could not list output devices: " + ErrorCode(hr);
        return false;
    }
    std::vector<Device> devices{{L"", L"System default playback device"}};
    UINT count = 0;
    hr = collection->GetCount(&count);
    if (FAILED(hr)) {
        error = L"Device count unavailable: " + ErrorCode(hr);
        return false;
    }
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;
        LPWSTR id = nullptr;
        if (FAILED(device->GetId(&id)) || !id) continue;
        devices.push_back({id, DeviceName(device.Get())});
        CoTaskMemFree(id);
    }
    g_devices.swap(devices);
    SendMessageW(g_device, CB_RESETCONTENT, 0, 0);
    for (const auto& device : g_devices)
        SendMessageW(g_device, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(device.name.c_str()));
    SendMessageW(g_device, CB_SETCURSEL, 0, 0);
    return true;
}

void U32(std::vector<BYTE>& out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<BYTE>(value >> (8 * i)));
}
void Tag(std::vector<BYTE>& out, const char* tag) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<BYTE>(tag[i]));
}
class WavWriter {
public:
    ~WavWriter() {
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }
    bool Open(const std::wstring& path, const WAVEFORMATEX* fmt, std::wstring& error) {
        if (!fmt || fmt->nBlockAlign == 0 || fmt->nSamplesPerSec == 0 ||
            fmt->nChannels == 0 || fmt->cbSize > 4096 ||
            (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize < 22)) {
            error = L"Windows returned an unsupported mix format.";
            return false;
        }
        const uint32_t fmtBytes = fmt->wFormatTag == WAVE_FORMAT_PCM
            ? 16u : 18u + fmt->cbSize;
        std::vector<BYTE> header;
        Tag(header, "RIFF"); U32(header, 0);
        Tag(header, "WAVE"); Tag(header, "fmt "); U32(header, fmtBytes);
        const BYTE* formatBytes = reinterpret_cast<const BYTE*>(fmt);
        header.insert(header.end(), formatBytes, formatBytes + fmtBytes);
        if (fmtBytes & 1u) header.push_back(0);
        if (fmt->wFormatTag != WAVE_FORMAT_PCM) {
            Tag(header, "fact"); U32(header, 4);
            factOffset_ = static_cast<DWORD>(header.size());
            U32(header, 0);
        }
        Tag(header, "data");
        dataOffset_ = static_cast<DWORD>(header.size());
        U32(header, 0);
        headerSize_ = static_cast<uint32_t>(header.size());
        blockAlign_ = fmt->nBlockAlign;
        file_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            error = L"Cannot create WAV: " + WinError(GetLastError());
            return false;
        }
        DWORD written = 0;
        if (!WriteFile(file_, header.data(), static_cast<DWORD>(header.size()),
                       &written, nullptr) || written != header.size()) {
            error = L"Could not write WAV header.";
            return false;
        }
        return true;
    }
    bool Append(const BYTE* data, DWORD length, bool silent, std::wstring& error) {
        if (!length) return true;
        const uint64_t nextSize = audioBytes_ + length;
        const uint64_t riffSize = headerSize_ + nextSize + (nextSize & 1u) - 8u;
        if (riffSize > UINT32_MAX || nextSize > UINT32_MAX) {
            error = L"WAV reached its 4 GiB RIFF limit.";
            return false;
        }
        DWORD written = 0;
        BOOL ok = FALSE;
        if (silent) {
            std::vector<BYTE> zeros(length, 0);
            ok = WriteFile(file_, zeros.data(), length, &written, nullptr);
        } else if (data) {
            ok = WriteFile(file_, data, length, &written, nullptr);
        }
        if (!ok || written != length) {
            error = L"Audio write failed: " + WinError(GetLastError());
            return false;
        }
        audioBytes_ = nextSize;
        g_audioBytes.store(audioBytes_);
        return true;
    }
    bool Finish(std::wstring& error) {
        if (file_ == INVALID_HANDLE_VALUE) return false;
        if (audioBytes_ & 1u) {
            const BYTE pad = 0;
            DWORD count = 0;
            if (!WriteFile(file_, &pad, 1, &count, nullptr) || count != 1) {
                error = L"Could not pad the WAV file.";
                return false;
            }
        }
        const auto riffSize = static_cast<uint32_t>(
            headerSize_ + audioBytes_ + (audioBytes_ & 1u) - 8u);
        if (!Patch(4, riffSize) ||
            !Patch(dataOffset_, static_cast<uint32_t>(audioBytes_)) ||
            (factOffset_ && !Patch(factOffset_,
                static_cast<uint32_t>(audioBytes_ / blockAlign_)))) {
            error = L"Could not finalize WAV header.";
            return false;
        }
        if (!FlushFileBuffers(file_)) {
            error = L"Could not flush WAV data: " + WinError(GetLastError());
            return false;
        }
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
        return true;
    }
private:
    bool Patch(DWORD offset, uint32_t value) {
        LARGE_INTEGER pos{};
        pos.QuadPart = offset;
        if (!SetFilePointerEx(file_, pos, nullptr, FILE_BEGIN)) return false;
        BYTE bytes[4] = {static_cast<BYTE>(value), static_cast<BYTE>(value >> 8),
                         static_cast<BYTE>(value >> 16), static_cast<BYTE>(value >> 24)};
        DWORD count = 0;
        return WriteFile(file_, bytes, 4, &count, nullptr) && count == 4;
    }
    HANDLE file_ = INVALID_HANDLE_VALUE;
    uint64_t audioBytes_ = 0;
    uint32_t headerSize_ = 0;
    DWORD dataOffset_ = 0, factOffset_ = 0;
    WORD blockAlign_ = 0;
};

bool IsFormat(const WAVEFORMATEX* fmt, WORD subtype, WORD bits) {
    if (!fmt || fmt->wBitsPerSample != bits) return false;
    if (fmt->wFormatTag == subtype) return true;
    if (fmt->wFormatTag != WAVE_FORMAT_EXTENSIBLE || fmt->cbSize < 22) return false;
    return reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt)->SubFormat.Data1 == subtype;
}
void ReadPeak(const BYTE* bytes, UINT32 frames, const WAVEFORMATEX* fmt) {
    if (!bytes || !frames) { g_peak.store(0); return; }
    const size_t samples = static_cast<size_t>(frames) * fmt->nChannels;
    float peak = 0;
    if (IsFormat(fmt, WAVE_FORMAT_IEEE_FLOAT, 32)) {
        for (size_t i = 0; i < samples; ++i) {
            float value = 0;
            std::memcpy(&value, bytes + i * sizeof(float), sizeof(float));
            if (std::isfinite(value)) peak = (std::max)(peak, std::fabs(value));
        }
    } else if (IsFormat(fmt, WAVE_FORMAT_PCM, 16)) {
        for (size_t i = 0; i < samples; ++i) {
            int16_t value = 0;
            std::memcpy(&value, bytes + i * sizeof(value), sizeof(value));
            peak = (std::max)(peak, std::fabs(static_cast<float>(value) / 32768.f));
        }
    }
    g_peak.store(static_cast<int>((std::min)(peak, 1.f) * 1000));
}
bool Capture(const std::wstring& deviceId, const std::wstring& path,
             std::wstring& result) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        result = L"Cannot access audio endpoints: " + ErrorCode(hr);
        return false;
    }
    ComPtr<IMMDevice> device;
    hr = deviceId.empty()
        ? enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device)
        : enumerator->GetDevice(deviceId.c_str(), &device);
    if (FAILED(hr)) {
        result = L"Playback endpoint unavailable: " + ErrorCode(hr);
        return false;
    }
    ComPtr<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(hr)) {
        result = L"Cannot activate Windows audio client: " + ErrorCode(hr);
        return false;
    }
    WAVEFORMATEX* raw = nullptr;
    hr = client->GetMixFormat(&raw);
    if (FAILED(hr) || !raw) {
        result = L"Cannot read output mix format: " + ErrorCode(hr);
        return false;
    }
    std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> format(raw, &CoTaskMemFree);
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                            10000000, 0, format.get(), nullptr);
    if (FAILED(hr)) {
        result = L"WASAPI loopback unavailable: " + ErrorCode(hr) +
                 L". Ensure the audio reaches a shared-mode Windows output.";
        return false;
    }
    ComPtr<IAudioCaptureClient> capture;
    hr = client->GetService(IID_PPV_ARGS(&capture));
    if (FAILED(hr)) {
        result = L"Cannot start capture service: " + ErrorCode(hr);
        return false;
    }
    WavWriter writer;
    if (!writer.Open(path, format.get(), result)) return false;
    hr = client->Start();
    if (FAILED(hr)) {
        std::wstring ignored;
        writer.Finish(ignored);
        result = L"Windows audio capture did not start: " + ErrorCode(hr);
        return false;
    }
    g_startedAt.store(GetTickCount64());
    bool healthy = true;
    while (!g_stopRequested.load() && healthy) {
        UINT32 framesReady = 0;
        hr = capture->GetNextPacketSize(&framesReady);
        if (FAILED(hr)) {
            result = L"Audio device changed or stopped: " + ErrorCode(hr);
            healthy = false;
            break;
        }
        while (framesReady && healthy) {
            BYTE* bytes = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture->GetBuffer(&bytes, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                result = L"Audio packet read failed: " + ErrorCode(hr);
                healthy = false;
                break;
            }
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            if (frames > UINT32_MAX / format->nBlockAlign) {
                result = L"Audio packet size is invalid.";
                healthy = false;
            } else {
                if (silent) g_peak.store(0);
                else ReadPeak(bytes, frames, format.get());
                healthy = writer.Append(bytes, frames * format->nBlockAlign, silent, result);
            }
            capture->ReleaseBuffer(frames);
            if (healthy) {
                hr = capture->GetNextPacketSize(&framesReady);
                if (FAILED(hr)) {
                    result = L"Audio queue failed: " + ErrorCode(hr);
                    healthy = false;
                }
            }
        }
        if (healthy) std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    client->Stop();
    std::wstring finishError;
    const bool finalized = writer.Finish(finishError);
    g_peak.store(0);
    if (!finalized) {
        result = finishError;
        return false;
    }
    if (!healthy) {
        result += L" Partial recording saved.";
        return false;
    }
    result = L"WAV saved successfully.";
    return true;
}
void CaptureThread(std::wstring deviceId, std::wstring path) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    if (SUCCEEDED(hr)) {
        ok = Capture(deviceId, path, g_result);
        CoUninitialize();
    } else {
        g_result = L"Windows COM initialization failed: " + ErrorCode(hr);
    }
    PostMessageW(g_window, WM_CAPTURE_DONE, ok ? 1 : 0, 0);
}
void SetRecordingUI(bool recording) {
    g_recording = recording;
    EnableWindow(g_device, !recording);
    EnableWindow(g_refresh, !recording);
    EnableWindow(g_record, !recording);
    EnableWindow(g_stop, recording);
}
std::wstring SuggestedName() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t buffer[128]{};
    swprintf_s(buffer, L"LoopCatch_%04u%02u%02u_%02u%02u%02u.wav",
               t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buffer;
}
void StartRecording() {
    if (g_recording) return;
    const LRESULT choice = SendMessageW(g_device, CB_GETCURSEL, 0, 0);
    if (choice == CB_ERR || static_cast<size_t>(choice) >= g_devices.size()) return;
    const std::wstring deviceId = g_devices[static_cast<size_t>(choice)].id;
    wchar_t path[MAX_PATH]{};
    wcscpy_s(path, SuggestedName().c_str());
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_window;
    dialog.lpstrFilter = L"WAV files (*.wav)\0*.wav\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"wav";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&dialog)) return;
    g_stopRequested.store(false);
    g_peak.store(0);
    g_audioBytes.store(0);
    g_startedAt.store(GetTickCount64());
    g_result.clear();
    SetWindowTextW(g_path, path);
    SetWindowTextW(g_status, L"Starting capture...");
    SetRecordingUI(true);
    try {
        g_worker = std::thread(CaptureThread, deviceId, std::wstring(path));
    } catch (...) {
        SetRecordingUI(false);
        SetWindowTextW(g_status, L"Cannot create audio capture thread.");
    }
}
HWND Control(const wchar_t* kind, const wchar_t* text, DWORD style,
             int x, int y, int w, int h, int id) {
    HWND result = CreateWindowExW(0, kind, text, WS_CHILD | WS_VISIBLE | style,
                                  x, y, w, h, g_window,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                  GetModuleHandleW(nullptr), nullptr);
    SendMessageW(result, WM_SETFONT,
                 reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return result;
}
void SetupUI() {
    Control(L"STATIC", L"Record audio being played through a Windows output device:",
            0, 20, 18, 680, 25, 0);
    g_device = Control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                       20, 46, 515, 250, ID_DEVICE);
    g_refresh = Control(L"BUTTON", L"Refresh devices", BS_PUSHBUTTON,
                        545, 46, 154, 30, ID_REFRESH);
    g_record = Control(L"BUTTON", L"Record WAV", BS_PUSHBUTTON,
                       20, 102, 166, 40, ID_RECORD);
    g_stop = Control(L"BUTTON", L"Stop & Save", BS_PUSHBUTTON,
                     197, 102, 166, 40, ID_STOP);
    Control(L"STATIC", L"Live level:", 0, 20, 161, 80, 24, 0);
    g_level = Control(PROGRESS_CLASSW, L"", PBS_SMOOTH,
                      105, 160, 594, 22, ID_LEVEL);
    SendMessageW(g_level, PBM_SETRANGE32, 0, 1000);
    g_status = Control(L"STATIC", L"Ready.", 0, 20, 196, 679, 24, ID_STATUS);
    Control(L"STATIC", L"Output file:", 0, 20, 226, 679, 22, 0);
    g_path = Control(L"STATIC", L"Choose a WAV filename when you press Record.",
                     SS_PATHELLIPSIS, 20, 246, 679, 25, ID_PATH);
    Control(L"STATIC", L"Captures shared-mode Windows audio. ASIO, exclusive audio, and DRM may bypass it.",
            0, 20, 283, 690, 25, 0);
    std::wstring error;
    if (!RefreshDevices(error)) SetWindowTextW(g_status, error.c_str());
    SetRecordingUI(false);
    SetTimer(g_window, 1, 100, nullptr);
}
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE:
        g_window = window;
        SetupUI();
        return 0;
    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED) {
            switch (LOWORD(wp)) {
            case ID_RECORD:
                StartRecording();
                return 0;
            case ID_STOP:
                if (g_recording) {
                    g_stopRequested.store(true);
                    EnableWindow(g_stop, FALSE);
                    SetWindowTextW(g_status, L"Stopping and finalizing WAV...");
                }
                return 0;
            case ID_REFRESH: {
                std::wstring error;
                if (RefreshDevices(error)) error = L"Playback devices refreshed.";
                SetWindowTextW(g_status, error.c_str());
                return 0;
            }
            }
        }
        break;
    case WM_TIMER:
        if (g_recording && !g_stopRequested.load()) {
            const ULONGLONG ms = GetTickCount64() - g_startedAt.load();
            std::wostringstream line;
            line << L"Recording " << ms / 60000 << L":"
                 << std::setfill(L'0') << std::setw(2) << (ms / 1000) % 60
                 << L"  |  " << g_audioBytes.load() / 1024 << L" KiB";
            SetWindowTextW(g_status, line.str().c_str());
            SendMessageW(g_level, PBM_SETPOS, g_peak.load(), 0);
        }
        return 0;
    case WM_CAPTURE_DONE:
        if (g_worker.joinable()) g_worker.join();
        SetRecordingUI(false);
        SendMessageW(g_level, PBM_SETPOS, 0, 0);
        SetWindowTextW(g_status, g_result.c_str());
        if (!wp) MessageBoxW(window, g_result.c_str(),
                             L"LoopCatch capture notice", MB_OK | MB_ICONWARNING);
        return 0;
    case WM_CLOSE:
        g_stopRequested.store(true);
        if (g_worker.joinable()) g_worker.join();
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Windows COM initialization failed.",
                    L"LoopCatch", MB_OK | MB_ICONERROR);
        return 1;
    }
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&common);
    WNDCLASSW klass{};
    klass.hInstance = instance;
    klass.lpfnWndProc = WindowProc;
    klass.lpszClassName = L"LoopCatchNative";
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&klass)) {
        CoUninitialize();
        return 1;
    }
    HWND window = CreateWindowExW(0, klass.lpszClassName,
        L"LoopCatch v0.1 | Windows Desktop Audio Recorder",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 735, 370,
        nullptr, nullptr, instance, nullptr);
    if (!window) {
        CoUninitialize();
        return 1;
    }
    ShowWindow(window, show);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
