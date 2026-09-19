// capture/capture_factory.cpp — backend selection (design doc §6.5).
//
// Desktop Duplication is always preferred: it is the only path that keeps the
// frame on the GPU. GDI BitBlt is the documented fallback for protected or
// locked desktops, and switching to it is a user-visible event so the UI can
// say "capture unavailable" instead of silently degrading.
#include "capture/capture.h"

#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdio>
#include <memory>
#include <string>
#include <utility>

namespace mag {
namespace {

// ErrorReported code for "backend switched"; the frozen headers declare no
// enum, so the capture module owns this small numbering.
constexpr std::uint32_t kCodeCaptureFallback = 1;

// ErrorReported carries a bare const char*, which must outlive the publish, so
// only string literals are ever published from here.
constexpr const char* kTextCaptureFallback =
    "Desktop Duplication unavailable; switched to GDI BitBlt fallback capture";

std::string hresult_text(HRESULT hr) {
    char buf[24]{};
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(buf);
}

template <typename T>
void publish(BoundedEventBus& bus, T payload) noexcept {
    try {
        AppEvent event = AppEvent::make(std::move(payload));
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        event.timestamp_qpc = static_cast<std::uint64_t>(counter.QuadPart);
        event.source_thread = static_cast<std::uint32_t>(::GetCurrentThreadId());
        bus.publish(event);
    } catch (...) {
    }
}

struct DuplicationProbe {
    std::size_t output_count{0};
    bool duplication_available{false};
    bool saw_error{false};
    HRESULT first_error{E_FAIL};
    std::uint32_t first_error_output{0};
};

// Answers the only question the factory can ask without a topology service:
// will Desktop Duplication open on any output at all? The probe's own
// duplication is released before returning so the real backend can take it.
DuplicationProbe probe_desktop_duplication() noexcept {
    DuplicationProbe probe;

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) ||
        factory == nullptr) {
        return probe;
    }

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_1};
    const HRESULT dhr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 3,
                                          D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(dhr) || device == nullptr) {
        if (context) {
            context->Release();
        }
        factory->Release();
        return probe;
    }

    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) == S_OK && !probe.duplication_available;
         ++a) {
        IDXGIOutput* output = nullptr;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) == S_OK; ++o) {
            const std::uint32_t ordinal = static_cast<std::uint32_t>(probe.output_count);
            ++probe.output_count;

            IDXGIOutput1* output1 = nullptr;
            if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput1),
                                                 reinterpret_cast<void**>(&output1))) &&
                output1 != nullptr) {
                IDXGIOutputDuplication* duplication = nullptr;
                const HRESULT hr = output1->DuplicateOutput(device, &duplication);
                if (SUCCEEDED(hr) && duplication != nullptr) {
                    probe.duplication_available = true;
                    duplication->Release();
                } else if (!probe.saw_error) {
                    probe.saw_error = true;
                    probe.first_error = hr;
                    probe.first_error_output = ordinal;
                }
                output1->Release();
            }
            output->Release();
            if (probe.duplication_available) {
                break;
            }
        }
        adapter->Release();
    }

    context->Release();
    device->Release();
    factory->Release();
    return probe;
}

}  // namespace

BackendSelection create_capture_backend(BoundedEventBus& bus) {
    BackendSelection selection;

    std::string dxgi_failure;
    try {
        const DuplicationProbe probe = probe_desktop_duplication();
        // Only a positive refusal on a real output counts as "unavailable":
        // an enumeration that found no output at all is a different problem and
        // is left for start() to report with a proper HRESULT.
        const bool refused = probe.output_count > 0 && !probe.duplication_available;
        if (!refused) {
            selection.backend = make_dxgi_backend(bus);
            selection.used_fallback = false;
            selection.note = probe.output_count == 0
                                 ? "no DXGI output enumerated; Desktop Duplication backend selected"
                                 : std::string{};
            return selection;
        }
        dxgi_failure = "Desktop Duplication refused on all " +
                       std::to_string(probe.output_count) + " outputs (output " +
                       std::to_string(probe.first_error_output) + " first failed with " +
                       hresult_text(probe.saw_error ? probe.first_error : E_FAIL) + ")";
    } catch (const std::exception& error) {
        dxgi_failure = std::string("Desktop Duplication backend failed: ") + error.what();
    }

    selection.backend = make_gdi_backend(bus);
    selection.used_fallback = true;
    selection.note = dxgi_failure.empty()
                         ? "Desktop Duplication unavailable; using GDI BitBlt fallback"
                         : dxgi_failure + "; using GDI BitBlt fallback";
    publish(bus, ErrorReported{kCodeCaptureFallback, kTextCaptureFallback});
    return selection;
}

}  // namespace mag
