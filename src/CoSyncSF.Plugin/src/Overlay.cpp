#include "Overlay.h"

#include "MetroUi.h"

#include "cosync/shared/Log.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#include <atomic>
#include <format>
#include <mutex>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

namespace cosync::plugin::overlay
{
    namespace
    {
        using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
        using Resize_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
        using Execute_t = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

        Present_t g_present = nullptr;
        Resize_t g_resize = nullptr;
        Execute_t g_execute = nullptr;
        std::atomic<ID3D12CommandQueue*> g_queue{ nullptr };
        std::atomic<bool> g_open{ false };
        WNDPROC g_originalWndProc = nullptr;

        constexpr UINT kSrvCount = 64;
        constexpr UINT kToggleKey = VK_F7;

        struct Frame
        {
            ID3D12CommandAllocator* allocator = nullptr;
            ID3D12Resource* target = nullptr;
            D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
            UINT64 fenceValue = 0;
        };

        struct Gfx
        {
            IDXGISwapChain3* swap = nullptr;  // not owned
            ID3D12Device* device = nullptr;
            ID3D12DescriptorHeap* rtvHeap = nullptr;
            ID3D12DescriptorHeap* srvHeap = nullptr;
            ID3D12GraphicsCommandList* list = nullptr;
            ID3D12Fence* fence = nullptr;
            HANDLE fenceEvent = nullptr;
            UINT64 fenceCounter = 0;
            std::vector<Frame> frames;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            HWND hwnd = nullptr;
            bool imgui = false;
            bool targetsValid = false;
            UINT srvNext = 0;
            bool failed = false;
        } g;

        // Snapshot/commands exchange with the main thread.
        std::mutex g_lock;
        ui::UiSnapshot g_snapshot;
        std::vector<ui::UiCommand> g_commands;

        template <class T>
        void SafeRelease(T*& p)
        {
            if (p) {
                p->Release();
                p = nullptr;
            }
        }

        void WaitIdle()
        {
            auto* queue = g_queue.load();
            if (!queue || !g.fence) return;
            const UINT64 v = ++g.fenceCounter;
            if (SUCCEEDED(queue->Signal(g.fence, v)) && g.fence->GetCompletedValue() < v) {
                g.fence->SetEventOnCompletion(v, g.fenceEvent);
                WaitForSingleObject(g.fenceEvent, 1000);
            }
        }

        void ReleaseTargets()
        {
            for (auto& f : g.frames) SafeRelease(f.target);
            g.targetsValid = false;
        }

        bool CreateTargets()
        {
            const UINT rtvSize = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            auto handle = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
            for (UINT i = 0; i < g.frames.size(); ++i) {
                if (FAILED(g.swap->GetBuffer(i, IID_PPV_ARGS(&g.frames[i].target)))) return false;
                g.frames[i].rtv = handle;
                g.device->CreateRenderTargetView(g.frames[i].target, nullptr, handle);
                handle.ptr += rtvSize;
            }
            g.targetsValid = true;
            return true;
        }

        void SrvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
        {
            const UINT size = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            const UINT index = g.srvNext < kSrvCount ? g.srvNext++ : kSrvCount - 1;
            cpu->ptr = g.srvHeap->GetCPUDescriptorHandleForHeapStart().ptr + static_cast<SIZE_T>(index) * size;
            gpu->ptr = g.srvHeap->GetGPUDescriptorHandleForHeapStart().ptr + static_cast<UINT64>(index) * size;
        }
        void SrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {}

        bool IsInputMessage(UINT msg)
        {
            return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
                   msg == WM_INPUT || msg == WM_CHAR;
        }

        LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
        {
            if (msg == WM_KEYDOWN && wparam == kToggleKey && !(lparam & (1 << 30))) {
                g_open = !g_open.load();
                return 0;
            }
            if (g_open.load() && g.imgui) {
                ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
                if (IsInputMessage(msg)) return 0;  // the UI owns input while open
            }
            return CallWindowProcW(g_originalWndProc, hwnd, msg, wparam, lparam);
        }

        bool Initialise(IDXGISwapChain* swap)
        {
            auto* queue = g_queue.load();
            if (!queue) return false;  // the game has not submitted work yet
            if (FAILED(swap->QueryInterface(IID_PPV_ARGS(&g.swap)))) return false;
            g.swap->Release();  // keep a non-owning pointer (the game owns the swapchain)
            if (FAILED(g.swap->GetDevice(IID_PPV_ARGS(&g.device)))) return false;
            DXGI_SWAP_CHAIN_DESC desc{};
            g.swap->GetDesc(&desc);
            g.hwnd = desc.OutputWindow;
            g.format = desc.BufferDesc.Format;
            g.frames.resize(desc.BufferCount);

            D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, desc.BufferCount,
                                                D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
            D3D12_DESCRIPTOR_HEAP_DESC srvDesc{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvCount,
                                                D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
            if (FAILED(g.device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g.rtvHeap))) ||
                FAILED(g.device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g.srvHeap))))
                return false;
            for (auto& f : g.frames) {
                if (FAILED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator))))
                    return false;
            }
            if (FAILED(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.frames[0].allocator, nullptr,
                                                   IID_PPV_ARGS(&g.list))))
                return false;
            g.list->Close();
            if (FAILED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) return false;
            g.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!CreateTargets()) return false;

            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGui::GetIO().IniFilename = nullptr;  // no imgui.ini next to the game
            ImGui_ImplWin32_Init(g.hwnd);
            ImGui_ImplDX12_InitInfo info;
            info.Device = g.device;
            info.CommandQueue = queue;
            info.NumFramesInFlight = static_cast<int>(g.frames.size());
            info.RTVFormat = g.format;
            info.SrvDescriptorHeap = g.srvHeap;
            info.SrvDescriptorAllocFn = &SrvAlloc;
            info.SrvDescriptorFreeFn = &SrvFree;
            if (!ImGui_ImplDX12_Init(&info)) return false;
            g.imgui = true;
            g_originalWndProc =
                reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc)));
            log::Info("[ui] overlay ready: {} back buffers, format {}, F7 toggles", g.frames.size(),
                      static_cast<int>(g.format));
            return true;
        }

        void Render(IDXGISwapChain* swap)
        {
            if (g.failed) return;
            if (!g.imgui) {
                if (!Initialise(swap) && g_queue.load()) {
                    g.failed = true;  // device objects could not be created: stay out of the way
                    log::Warn("[ui] overlay initialisation failed; UI disabled");
                }
                return;
            }
            if (static_cast<void*>(swap) != static_cast<void*>(g.swap) || !g_open.load()) return;
            if (!g.targetsValid && !CreateTargets()) return;
            auto* queue = g_queue.load();
            if (!queue) return;

            ui::UiSnapshot snapshot;
            {
                std::lock_guard lock(g_lock);
                snapshot = g_snapshot;
            }
            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            ImGui::GetIO().MouseDrawCursor = true;
            metroui::Draw(snapshot, [](ui::UiCommand c) {
                std::lock_guard lock(g_lock);
                if (g_commands.size() < 64) g_commands.push_back(std::move(c));
            });
            ImGui::Render();

            auto& frame = g.frames[g.swap->GetCurrentBackBufferIndex()];
            if (frame.fenceValue && g.fence->GetCompletedValue() < frame.fenceValue) {
                g.fence->SetEventOnCompletion(frame.fenceValue, g.fenceEvent);
                WaitForSingleObject(g.fenceEvent, 100);
            }
            frame.allocator->Reset();
            g.list->Reset(frame.allocator, nullptr);
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = frame.target;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            g.list->ResourceBarrier(1, &barrier);
            g.list->OMSetRenderTargets(1, &frame.rtv, FALSE, nullptr);
            g.list->SetDescriptorHeaps(1, &g.srvHeap);
            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g.list);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            g.list->ResourceBarrier(1, &barrier);
            g.list->Close();
            ID3D12CommandList* lists[] = { g.list };
            queue->ExecuteCommandLists(1, lists);
            frame.fenceValue = ++g.fenceCounter;
            queue->Signal(g.fence, frame.fenceValue);
        }

        HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* swap, UINT sync, UINT flags)
        {
            static thread_local bool inside = false;
            if (!inside) {
                inside = true;
                Render(swap);
                inside = false;
            }
            return g_present(swap, sync, flags);
        }

        HRESULT STDMETHODCALLTYPE ResizeHook(IDXGISwapChain* swap, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)
        {
            // The game cannot resize while we hold references to its back buffers.
            if (g.imgui && static_cast<void*>(swap) == static_cast<void*>(g.swap)) {
                WaitIdle();
                ReleaseTargets();
            }
            return g_resize(swap, count, w, h, fmt, flags);
        }

        void STDMETHODCALLTYPE ExecuteHook(ID3D12CommandQueue* queue, UINT n, ID3D12CommandList* const* lists)
        {
            if (!g_queue.load()) {
                const auto desc = queue->GetDesc();
                if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) g_queue.store(queue);
            }
            g_execute(queue, n, lists);
        }
    }

    bool Install(std::string& detail)
    {
        // Throwaway device/queue/swapchain on a hidden window, only to find the vtable entries.
        WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr,
                        nullptr, nullptr, L"CoSyncOverlayProbe", nullptr };
        RegisterClassExW(&wc);
        HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                                  wc.hInstance, nullptr);
        ID3D12Device* device = nullptr;
        ID3D12CommandQueue* queue = nullptr;
        IDXGIFactory4* factory = nullptr;
        IDXGISwapChain1* swap = nullptr;
        bool ok = false;
        if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
            D3D12_COMMAND_QUEUE_DESC qd{ D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
            DXGI_SWAP_CHAIN_DESC1 sd{};
            sd.BufferCount = 2;
            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            sd.SampleDesc.Count = 1;
            if (SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) &&
                SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
                SUCCEEDED(factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &swap))) {
                void** swapVtbl = *reinterpret_cast<void***>(swap);
                void** queueVtbl = *reinterpret_cast<void***>(queue);
                const auto init = MH_Initialize();
                ok = init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED;
                ok = ok && MH_CreateHook(swapVtbl[8], reinterpret_cast<void*>(&PresentHook),
                                         reinterpret_cast<void**>(&g_present)) == MH_OK;
                ok = ok && MH_CreateHook(swapVtbl[13], reinterpret_cast<void*>(&ResizeHook),
                                         reinterpret_cast<void**>(&g_resize)) == MH_OK;
                ok = ok && MH_CreateHook(queueVtbl[10], reinterpret_cast<void*>(&ExecuteHook),
                                         reinterpret_cast<void**>(&g_execute)) == MH_OK;
                ok = ok && MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
            }
        }
        SafeRelease(swap);
        SafeRelease(factory);
        SafeRelease(queue);
        SafeRelease(device);
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        detail = ok ? "UI overlay hooks installed (Present, ResizeBuffers, ExecuteCommandLists); F7 opens the UI"
                    : "UI overlay hooks NOT installed";
        return ok;
    }

    void Publish(const ui::UiSnapshot& snapshot)
    {
        std::lock_guard lock(g_lock);
        g_snapshot = snapshot;
    }

    void DrainCommands(std::vector<ui::UiCommand>& out)
    {
        std::lock_guard lock(g_lock);
        for (auto& c : g_commands) out.push_back(std::move(c));
        g_commands.clear();
    }

    bool IsOpen() noexcept { return g_open.load(); }
}
