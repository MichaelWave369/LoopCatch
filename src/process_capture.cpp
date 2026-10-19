// LoopCatch original implementation. MIT License.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "process_capture.h"
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <objidl.h>
#include <atomic>
#include <new>
#include <set>
#include <string>
#include <utility>

namespace {
class CompletionHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    CompletionHandler() : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~CompletionHandler() {
        if (done_) CloseHandle(done_);
    }
    bool Valid() const { return done_ != nullptr; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id == __uuidof(IUnknown) ||
            id == __uuidof(IActivateAudioInterfaceCompletionHandler))
            *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (id == __uuidof(IAgileObject))
            *out = static_cast<IAgileObject*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --references_;
        if (!left) delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(
        IActivateAudioInterfaceAsyncOperation* operation) override {
        Microsoft::WRL::ComPtr<IUnknown> unknown;
        HRESULT activation = E_FAIL;
        const HRESULT request = operation->GetActivateResult(
            &activation, unknown.GetAddressOf());
        result_ = FAILED(request) ? request : activation;
        if (SUCCEEDED(result_)) {
            if (unknown) result_ = unknown.As(&client_);
            else result_ = E_NOINTERFACE;
        }
        SetEvent(done_);
        return S_OK;
    }

    HRESULT WaitAndRead(Microsoft::WRL::ComPtr<IAudioClient>& out) {
        // No timeout: callback ownership must last until activation completes.
        if (WaitForSingleObject(done_, INFINITE) != WAIT_OBJECT_0)
            return HRESULT_FROM_WIN32(GetLastError());
        if (SUCCEEDED(result_)) out = client_;
        return result_;
    }

private:
    std::atomic<ULONG> references_{1};
    HANDLE done_ = nullptr;
    HRESULT result_ = E_PENDING;
    Microsoft::WRL::ComPtr<IAudioClient> client_;
};

struct PickerContext {
    std::vector<CaptureApp> apps;
    std::set<DWORD> seen;
};
BOOL CALLBACK EnumerateWindow(HWND window, LPARAM arg) {
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER)) return TRUE;
    const int length = GetWindowTextLengthW(window);
    if (length < 1) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!pid || pid == GetCurrentProcessId()) return TRUE;
    auto& context = *reinterpret_cast<PickerContext*>(arg);
    if (!context.seen.insert(pid).second) return TRUE;

    std::wstring title(static_cast<size_t>(length) + 1, L'\0');
    const int written = GetWindowTextW(window, title.data(), length + 1);
    if (written <= 0) return TRUE;
    title.resize(static_cast<size_t>(written));

    std::wstring name = L"Application";
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process) {
        wchar_t fullPath[32768]{};
        DWORD characters = static_cast<DWORD>(sizeof(fullPath) / sizeof(fullPath[0]));
        if (QueryFullProcessImageNameW(process, 0, fullPath, &characters)) {
            std::wstring path(fullPath, characters);
            const auto pos = path.find_last_of(L"\\/");
            name = pos == std::wstring::npos ? path : path.substr(pos + 1);
        }
        CloseHandle(process);
    }
    // Visible windows do not necessarily emit audio.
    context.apps.push_back({pid, name + L"  |  " + title +
                                  L"  (PID " + std::to_wstring(pid) + L")"});
    return TRUE;
}
} // namespace

std::vector<CaptureApp> ListCaptureApps() {
    PickerContext context;
    EnumWindows(&EnumerateWindow, reinterpret_cast<LPARAM>(&context));
    return std::move(context.apps);
}

bool SupportsProcessLoopback() {
    // Avoid version shims that can alter GetVersionEx results.
    using VersionFn = LONG (WINAPI*)(OSVERSIONINFOW*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    const auto getVersion = reinterpret_cast<VersionFn>(
        GetProcAddress(ntdll, "RtlGetVersion"));
    if (!getVersion) return false;
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    return getVersion(&version) >= 0 &&
           version.dwMajorVersion >= 10 && version.dwBuildNumber >= 20348;
}

HRESULT ActivateProcessLoopback(DWORD pid, Microsoft::WRL::ComPtr<IAudioClient>& client) {
    if (!pid) return E_INVALIDARG;
    if (!SupportsProcessLoopback()) return HRESULT_FROM_WIN32(ERROR_OLD_WIN_VERSION);

    Microsoft::WRL::ComPtr<CompletionHandler> handler;
    handler.Attach(new (std::nothrow) CompletionHandler());
    if (!handler) return E_OUTOFMEMORY;
    if (!handler->Valid()) return E_OUTOFMEMORY;

    AUDIOCLIENT_ACTIVATION_PARAMS activation{};
    activation.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    activation.ProcessLoopbackParams.TargetProcessId = pid;
    activation.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT params{};
    params.vt = VT_BLOB;
    params.blob.cbSize = sizeof(activation);
    params.blob.pBlobData = reinterpret_cast<BYTE*>(&activation);

    Microsoft::WRL::ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    const HRESULT result = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
        &params, handler.Get(), &operation);
    if (FAILED(result)) return result;
    return handler->WaitAndRead(client);
}
