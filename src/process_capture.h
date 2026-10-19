#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <string>
#include <vector>

struct CaptureApp {
    DWORD pid = 0;
    std::wstring label;
};

// Visible windows are an understandable starting point for picking a root PID.
// The manual PID field in the UI covers helper and host processes.
std::vector<CaptureApp> ListCaptureApps();

// Windows process loopback needs build 20348 or newer.
bool SupportsProcessLoopback();

// Called from the MTA recording worker. Includes target PID and descendants.
HRESULT ActivateProcessLoopback(DWORD pid, Microsoft::WRL::ComPtr<IAudioClient>& client);
