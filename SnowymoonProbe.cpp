// Diagnostic only: logs DXVK's COM calls. It does not suppress calls or change rendering.
#include <windows.h>
#include <d3d11_1.h>
#include <MinHook.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cstdint>

namespace {
HANDLE g_log = INVALID_HANDLE_VALUE;
SRWLOCK g_lock = SRWLOCK_INIT;
std::atomic<unsigned> g_events{0};
std::atomic<unsigned> g_discards{0};
std::atomic<unsigned> g_clears{0};
constexpr unsigned kDetailedEvents = 18;
bool g_skip_external_discard = false;
uintptr_t g_dxvk_start = 0;
uintptr_t g_dxvk_end = 0;

void logf(const char* fmt, ...) {
    char message[640];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
    AcquireSRWLockExclusive(&g_lock);
    if (g_log != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(g_log, message, static_cast<DWORD>(strlen(message)), &written, nullptr);
        WriteFile(g_log, "\r\n", 2, &written, nullptr);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

using CreateDevice = HRESULT (WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using CreateDeviceAndSwapChain = HRESULT (WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE,
    UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using DiscardView = decltype(ID3D11DeviceContext1Vtbl::DiscardView);
using ClearView = decltype(ID3D11DeviceContext1Vtbl::ClearView);

CreateDevice g_create_device = nullptr;
CreateDeviceAndSwapChain g_create_device_swap = nullptr;
DiscardView g_discard = nullptr;
ClearView g_clear = nullptr;

void record_view(const char* name, ID3D11View* view) {
    unsigned index = g_events.fetch_add(1, std::memory_order_relaxed);
    if (index >= kDetailedEvents && index % 1000 != 0) return;
    void* vtable = view ? *reinterpret_cast<void**>(view) : nullptr;
    logf("%s #%u thread=%lu view=%p vtable=%p", name, index,
        GetCurrentThreadId(), view, vtable);
}

void STDMETHODCALLTYPE hooked_discard(ID3D11DeviceContext1* self, ID3D11View* view) {
    g_discards.fetch_add(1, std::memory_order_relaxed);
    // Temporary diagnostic: DiscardView is a discard hint. Skip only views
    // whose vtable lies outside the actual DXVK d3d11.dll image.
    auto table = view ? reinterpret_cast<uintptr_t>(*reinterpret_cast<void**>(view)) : 0;
    bool skip = g_skip_external_discard && view && table && g_dxvk_start &&
        (table < g_dxvk_start || table >= g_dxvk_end);
    record_view(skip ? "SKIP DiscardView" : "DiscardView", view);
    if (skip) return;
    g_discard(self, view);
}

void STDMETHODCALLTYPE hooked_clear(ID3D11DeviceContext1* self, ID3D11View* view,
    const FLOAT color[4], const D3D11_RECT* rects, UINT count) {
    g_clears.fetch_add(1, std::memory_order_relaxed);
    record_view("ClearView", view);
    g_clear(self, view, color, rects, count);
}

void attach_context(ID3D11DeviceContext* context) {
    if (!context) return;
    ID3D11DeviceContext1* context1 = nullptr;
    const GUID iid = {0xbb2c6faa, 0xb5fb, 0x4082,
        {0x8e, 0x6b, 0x38, 0x8b, 0x8c, 0xfa, 0x90, 0xe1}};
    HRESULT hr = context->lpVtbl->QueryInterface(context, iid,
        reinterpret_cast<void**>(&context1));
    logf("Context QI(ID3D11DeviceContext1) hr=%08lx ctx=%p ctx1=%p", (unsigned long)hr, context, context1);
    if (FAILED(hr) || !context1) return;
    auto discard = context1->lpVtbl->DiscardView;
    auto clear = context1->lpVtbl->ClearView;
    if (!g_discard) {
        MH_STATUS st = MH_CreateHook(reinterpret_cast<LPVOID>(discard),
            reinterpret_cast<LPVOID>(&hooked_discard), reinterpret_cast<LPVOID*>(&g_discard));
        logf("DiscardView target=%p create=%d", reinterpret_cast<void*>(discard), st);
        if (st == MH_OK) logf("DiscardView enable=%d", MH_EnableHook(reinterpret_cast<LPVOID>(discard)));
    }
    if (!g_clear) {
        MH_STATUS st = MH_CreateHook(reinterpret_cast<LPVOID>(clear),
            reinterpret_cast<LPVOID>(&hooked_clear), reinterpret_cast<LPVOID*>(&g_clear));
        logf("ClearView target=%p create=%d", reinterpret_cast<void*>(clear), st);
        if (st == MH_OK) logf("ClearView enable=%d", MH_EnableHook(reinterpret_cast<LPVOID>(clear)));
    }
    context1->lpVtbl->Release(context1);
}

HRESULT WINAPI hooked_create_device(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software,
    UINT flags, const D3D_FEATURE_LEVEL* levels, UINT level_count, UINT sdk,
    ID3D11Device** device, D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context) {
    HRESULT hr = g_create_device(adapter, type, software, flags, levels, level_count,
        sdk, device, level, context);
    logf("D3D11CreateDevice hr=%08lx device=%p context=%p", (unsigned long)hr,
        device ? *device : nullptr, context ? *context : nullptr);
    if (SUCCEEDED(hr) && context) attach_context(*context);
    return hr;
}

HRESULT WINAPI hooked_create_swap(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software,
    UINT flags, const D3D_FEATURE_LEVEL* levels, UINT level_count, UINT sdk,
    const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swap, ID3D11Device** device,
    D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context) {
    HRESULT hr = g_create_device_swap(adapter, type, software, flags, levels, level_count,
        sdk, desc, swap, device, level, context);
    logf("D3D11CreateDeviceAndSwapChain hr=%08lx device=%p context=%p", (unsigned long)hr,
        device ? *device : nullptr, context ? *context : nullptr);
    if (SUCCEEDED(hr) && context) attach_context(*context);
    return hr;
}

DWORD WINAPI worker(void*) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n && n < MAX_PATH) {
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash) {
            const wchar_t suffix[] = L"SnowymoonProbe.log";
            if (static_cast<size_t>(slash + 1 - path) + (sizeof(suffix) / sizeof(wchar_t)) <= MAX_PATH) {
                wcscpy(slash + 1, suffix);
                g_log = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            }
        }
    }
    logf("Probe loaded; pid=%lu", GetCurrentProcessId());
    HMODULE d3d11 = nullptr;
    for (unsigned i = 0; i != 1200 && !d3d11; ++i) {
        d3d11 = GetModuleHandleW(L"d3d11.dll");
        if (!d3d11) Sleep(50);
    }
    if (!d3d11) { logf("Timeout waiting for d3d11.dll"); return 0; }
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(d3d11);
    auto pe = reinterpret_cast<IMAGE_NT_HEADERS64*>(reinterpret_cast<BYTE*>(d3d11) + dos->e_lfanew);
    g_dxvk_start = reinterpret_cast<uintptr_t>(d3d11);
    g_dxvk_end = g_dxvk_start + pe->OptionalHeader.SizeOfImage;
    logf("d3d11 base=%p timestamp=%08lx size=%08lx", d3d11,
        (unsigned long)pe->FileHeader.TimeDateStamp, (unsigned long)pe->OptionalHeader.SizeOfImage);
    char flag[4] = {};
    g_skip_external_discard = GetEnvironmentVariableA("SNOWY_SKIP_EXTERNAL_DISCARD", flag, sizeof(flag)) == 1 && flag[0] == '1';
    logf("SNOWY_SKIP_EXTERNAL_DISCARD=%d", g_skip_external_discard ? 1 : 0);
    MH_STATUS st = MH_Initialize();
    logf("MH_Initialize=%d", st);
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return 0;
    struct Entry { const char* name; LPVOID detour; LPVOID* original; } entries[] = {
        {"D3D11CreateDevice", reinterpret_cast<LPVOID>(&hooked_create_device), reinterpret_cast<LPVOID*>(&g_create_device)},
        {"D3D11CreateDeviceAndSwapChain", reinterpret_cast<LPVOID>(&hooked_create_swap), reinterpret_cast<LPVOID*>(&g_create_device_swap)},
    };
    for (const auto& e : entries) {
        auto target = reinterpret_cast<LPVOID>(GetProcAddress(d3d11, e.name));
        if (!target) { logf("%s missing", e.name); continue; }
        st = MH_CreateHook(target, e.detour, e.original);
        logf("%s target=%p create=%d", e.name, target, st);
        if (st == MH_OK) logf("%s enable=%d", e.name, MH_EnableHook(target));
    }
    for (;;) {
        Sleep(5000);
        logf("heartbeat uptime_ms=%llu discard_calls=%u clear_calls=%u",
            static_cast<unsigned long long>(GetTickCount64()),
            g_discards.load(std::memory_order_relaxed),
            g_clears.load(std::memory_order_relaxed));
    }
    return 0;
}
} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
