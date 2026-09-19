// Toolchain smoke test: exercises every platform capability the magnifier
// depends on, so we learn about gaps at build time rather than mid-project.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <shellscalingapi.h>
#include <dwmapi.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#pragma comment(lib, "d3d11.lib")

namespace {

// --- C++20 language/library surface -----------------------------------------

struct A { int v; };
struct B { double d; };
using Var = std::variant<A, B>;

std::atomic<int> g_counter{0};
std::mutex g_mutex;

void thread_probe() {
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([i] {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_counter.fetch_add(i + 1, std::memory_order_relaxed);
        });
    }
    for (auto& t : workers) t.join();
}

struct StopProbe {
    static void run(std::stop_token st, int* out) {
        while (!st.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        *out = 1;
    }
};

// --- Win32 / D3D surface -----------------------------------------------------

struct ProbeResult {
    bool d3d11_device = false;
    bool dxgi_factory = false;
    bool duplication = false;
    bool dcomp = false;
    bool dpi_awareness = false;
    bool hotkey = false;
    int  output_count = 0;
    int  adapter_count = 0;
    std::string notes;
};

ProbeResult probe_graphics() {
    ProbeResult r;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL got{};
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                   D3D11_SDK_VERSION, &device, &got, &ctx);
    if (FAILED(hr)) {
        r.notes += "D3D11CreateDevice hr=0x" + std::to_string(static_cast<unsigned>(hr)) + "; ";
        return r;
    }
    r.d3d11_device = true;

    IDXGIDevice* dxgi_device = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory1* factory1 = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device))) &&
        SUCCEEDED(dxgi_device->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory1)))) {
        r.dxgi_factory = true;

        IDXGIAdapter1* a1 = nullptr;
        for (UINT i = 0; factory1->EnumAdapters1(i, &a1) == S_OK; ++i) {
            ++r.adapter_count;
            IDXGIOutput* out = nullptr;
            for (UINT o = 0; a1->EnumOutputs(o, &out) == S_OK; ++o) {
                ++r.output_count;
                IDXGIOutput1* out1 = nullptr;
                if (SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput1),
                                                  reinterpret_cast<void**>(&out1)))) {
                    IDXGIOutputDuplication* dup = nullptr;
                    // Expected to fail without a real desktop session; a clean
                    // failure still proves the interface vtable resolved.
                    HRESULT dhr = out1->DuplicateOutput(device, &dup);
                    if (SUCCEEDED(dhr)) {
                        r.duplication = true;
                        dup->Release();
                    } else {
                        r.notes += "DuplicateOutput hr=0x" +
                                   std::to_string(static_cast<unsigned>(dhr)) + " (non-fatal; ";
                        r.notes += "desktop may be locked/headless); ";
                    }
                    out1->Release();
                }
                out->Release();
            }
            a1->Release();
        }
        factory1->Release();
    }
    if (adapter) adapter->Release();
    if (dxgi_device) dxgi_device->Release();

    // DirectComposition is the per-pixel-alpha presentation path.
    IDXGIDevice* dxgi_for_dcomp = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice),
                                         reinterpret_cast<void**>(&dxgi_for_dcomp)))) {
        IDCompositionDevice* dcomp = nullptr;
        HRESULT chr = DCompositionCreateDevice(dxgi_for_dcomp, __uuidof(IDCompositionDevice),
                                               reinterpret_cast<void**>(&dcomp));
        if (SUCCEEDED(chr) && dcomp) {
            r.dcomp = true;
            dcomp->Release();
        } else {
            r.notes += "DCompositionCreateDevice hr=0x" +
                       std::to_string(static_cast<unsigned>(chr)) + "; ";
        }
        dxgi_for_dcomp->Release();
    }

    ctx->Release();
    device->Release();
    return r;
}

// Resolve D3DCompile out of d3dcompiler_47.dll at runtime: zig ships no
// d3dcompiler import library, and delay-loading is the portable choice.
bool probe_d3dcompiler() {
    HMODULE mod = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!mod) return false;
    const bool ok = GetProcAddress(mod, "D3DCompile") != nullptr;
    FreeLibrary(mod);
    return ok;
}

LRESULT CALLBACK TestWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int main() {
    std::printf("=== magnifier toolchain smoke test ===\n");

    thread_probe();
    std::printf("[cpp20] std::thread + std::mutex + atomic OK (counter=%d)\n",
                g_counter.load());

    int stop_ok = 0;
    {
        std::stop_source src;
        std::thread t(StopProbe::run, src.get_token(), &stop_ok);
        src.request_stop();
        t.join();
    }
    std::printf("[cpp20] std::stop_token OK (%d)\n", stop_ok);

    Var v = A{7};
    std::printf("[cpp20] std::variant OK (index=%zu)\n", v.index());
    std::printf("[cpp20] std::optional OK (%d)\n", std::optional<int>(3).value_or(-1));

    // Per-monitor DPI v2.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const DPI_AWARENESS_CONTEXT got_ctx = GetThreadDpiAwarenessContext();
    const bool dpi_ok = AreDpiAwarenessContextsEqual(got_ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::printf("[win32] PerMonitorV2 DPI awareness %s\n", dpi_ok ? "OK" : "NOT SET");

    // Virtual desktop metrics in physical pixels.
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    std::printf("[win32] virtual desktop = (%d,%d) %dx%d\n", vx, vy, vw, vh);

    // Global hotkey registration (the background-input path).
    const bool hotkey_ok = RegisterHotKey(nullptr, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'M') != FALSE;
    if (hotkey_ok) UnregisterHotKey(nullptr, 1);
    std::printf("[win32] RegisterHotKey(Ctrl+Alt+M) %s\n", hotkey_ok ? "OK" : "FAILED");

    // A real window with the styles the overlay needs.
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TestWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MagnifierSmokeTest";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                                wc.lpszClassName, L"smoke", WS_POPUP,
                                0, 0, 200, 200, nullptr, nullptr, wc.hInstance, nullptr);
    std::printf("[win32] CreateWindowEx(topmost|toolwindow|layered) %s\n",
                hwnd ? "OK" : "FAILED");
    if (hwnd) {
        BOOL excluded = TRUE;
        const HRESULT ex = SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
        std::printf("[win32] SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) hr=0x%08X\n",
                    static_cast<unsigned>(ex));
        (void)excluded;
        DestroyWindow(hwnd);
    }

    const ProbeResult r = probe_graphics();
    std::printf("[gpu] D3D11 device      : %s\n", r.d3d11_device ? "OK" : "FAILED");
    std::printf("[gpu] DXGI factory      : %s (%d adapters, %d outputs)\n",
                r.dxgi_factory ? "OK" : "FAILED", r.adapter_count, r.output_count);
    std::printf("[gpu] Desktop Duplication: %s\n", r.duplication ? "OK" : "unavailable (see note)");
    std::printf("[gpu] DirectComposition : %s\n", r.dcomp ? "OK" : "FAILED");
    std::printf("[gpu] D3DCompile (d3dcompiler_47): %s\n", probe_d3dcompiler() ? "OK" : "MISSING");
    if (!r.notes.empty()) std::printf("[gpu] notes: %s\n", r.notes.c_str());

    std::printf("=== smoke test complete ===\n");
    return 0;
}
