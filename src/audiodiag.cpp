// Diagnostics (DebugEvents=1): logs the Windows default audio output whenever it changes. The
// engine opens its XAudio2 device once at start-up; if SteamVR switches the default output while
// the game runs, the game keeps playing to the old one.
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include "log.h"

void AudioDiagPoll() {
    static DWORD last = 0;
    static wchar_t lastName[256] = L"";
    DWORD now = GetTickCount();
    if (now - last < 2000) return;
    last = now;
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)) && en) {
        IMMDevice* dev = nullptr;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) && dev) {
            IPropertyStore* ps = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) && ps) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && wcscmp(v.pwszVal, lastName) != 0) {
                    Log("audio: default output is now '%ls'%s", v.pwszVal, lastName[0] ? " (changed while running)" : "");
                    wcsncpy(lastName, v.pwszVal, 255);
                }
                PropVariantClear(&v);
                ps->Release();
            }
            dev->Release();
        }
        en->Release();
    }
    if (SUCCEEDED(co)) CoUninitialize();
}
