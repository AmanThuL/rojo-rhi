//----------------------------------------------------------------------------------------------------------------------
/// @file NativeContext.h
/// @brief Declares the native arm's D3D12 device, queues, root signature, heaps, frame slots and hand-counted calls.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Common/ArmResult.h"
#include "Common/FeedbackCollector.h"
#include "Common/RunOptions.h"

#include <windows.h>

// Defines, rather than declares, every GUID the headers below name (each definition is selectany), so a CLSID the Agility
// headers add links without the Windows SDK's dxguid.lib carrying it.
#include <initguid.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rojoRHI::bench {
    using Microsoft::WRL::ComPtr;

    // The native calls N has made, each counted by a plain increment beside the call: record on a command list, submit on
    // a queue, host on the device (descriptor writes included), a fence, a descriptor or query heap, a command allocator's
    // or a list's Reset, and a resource's Map and Unmap. A barrier counts in record and again in barriers. Filling a
    // descriptor, barrier or pipeline stream struct, naming an object and the debug layer's own interfaces count nowhere.
    struct NativeCalls
    {
        uint64_t record = 0;
        uint64_t submit = 0;
        uint64_t host = 0;
        uint64_t barriers = 0;
    };

    // What every frame of a workload needs, fixed at setup.
    struct NativeFrameShape
    {
        uint32_t lists = 1;      ///< Direct command lists recorded per frame, each with its own allocator.
        uint64_t arenaBytes = 0; ///< Frame data per frame; 0 creates no arena.
    };

    // One command allocator and the list recorded into it, both owned by the context. The list is created closed, so its
    // first use is a Reset.
    struct NativeList
    {
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList9* list = nullptr;
    };

    // A committed buffer owned by the context. An UPLOAD buffer stays mapped for its life; `data` is null otherwise.
    struct NativeBuffer
    {
        ID3D12Resource* resource = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS address = 0;
        std::byte* data = nullptr;
        uint64_t size = 0;
    };

    // A queue's timestamp calibration, read once: its tick frequency and one GetClockCalibration pair, whose CPU side is a
    // QueryPerformanceCounter value.
    struct NativeQueueClock
    {
        double gpuTicksPerSecond = 0.0;
        uint64_t gpuCalibration = 0;
        uint64_t cpuCalibration = 0;
    };

    // The timestamps of one frame slot's lists, made by prepareFrames and counted nowhere: `pairs` pairs of queries, the
    // frame's n-th list reset for timing owning queries 2n and 2n + 1, and a READBACK buffer per queue, direct (0) and
    // compute (1), that each list of the queue's type resolves its pair into, so no resource is written by both queues.
    struct NativeSlotTimestamps
    {
        ID3D12QueryHeap* queries = nullptr;
        std::array<ID3D12Resource*, 2> readbacks{};
        std::array<const uint64_t*, 2> ticks{}; ///< The readback buffers, mapped while the context lives.
        uint32_t pairs = 0;
    };

    // A list of the open frame that wrote its first timestamp, its pair, its queue (0 direct, 1 compute), and whether it
    // has written its last.
    struct NativeTimedList
    {
        ID3D12GraphicsCommandList9* list = nullptr;
        uint32_t pair = 0;
        uint32_t queue = 0;
        bool ended = false;
    };

    // The fixed-function state of a graphics pipeline, each member starting neutral (solid fill, no culling, depth clip on,
    // depth and stencil tests off, blending off, one sample) until a workload sets it. An empty pixel shader makes a
    // depth-only pipeline; a null root signature names the context's global one.
    struct NativeGraphicsDesc
    {
        D3D12_SHADER_BYTECODE vertex{};
        D3D12_SHADER_BYTECODE pixel{};
        D3D12_RASTERIZER_DESC2 rasterizer{ D3D12_FILL_MODE_SOLID,
                                           D3D12_CULL_MODE_NONE,
                                           FALSE,
                                           0.0f,
                                           0.0f,
                                           0.0f,
                                           TRUE,
                                           D3D12_LINE_RASTERIZATION_MODE_ALIASED,
                                           0,
                                           D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF };
        D3D12_DEPTH_STENCIL_DESC2 depthStencil{
            FALSE,
            D3D12_DEPTH_WRITE_MASK_ZERO,
            D3D12_COMPARISON_FUNC_ALWAYS,
            FALSE,
            { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS, 0xFF, 0xFF },
            { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS, 0xFF, 0xFF },
            FALSE,
        };
        // Blending off and every channel written, for every render target.
        D3D12_BLEND_DESC blend = defaultBlend();
        D3D12_PRIMITIVE_TOPOLOGY_TYPE topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        std::vector<DXGI_FORMAT> colorFormats;
        DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
        D3D12_PIPELINE_STATE_FLAGS flags = D3D12_PIPELINE_STATE_FLAG_NONE;
        ID3D12RootSignature* root = nullptr;

        static D3D12_BLEND_DESC defaultBlend();
    };

    // The debug-layer message callback N registers, writing "D3D12 <SEVERITY> <id>: <text>" and a newline to `sink`, a
    // std::FILE*, for each corruption, error and warning that nativeMessageDenied does not name; information and message
    // severities write nothing. N registers it with standard error, so the driver's standard-error rule fails a
    // configuration on any line it writes. It runs on whatever thread the runtime raises the message from and allocates
    // nothing.
    void __stdcall nativeMessage(D3D12_MESSAGE_CATEGORY category, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR description, void* sink);
    // True for the messages N drops for the reasons RojoRHI's D3D12 backend drops them.
    bool nativeMessageDenied(D3D12_MESSAGE_ID id);

    // The device, on its own Agility device factory, with a direct and a compute queue, the global root signature, a
    // shader-visible view heap and sampler heap, a frame fence and three frame slots. Frames are paced as a renderer with
    // three frames in flight paces them: the direct queue signals the frame fence after each frame's submissions, and a
    // frame waits on the CPU for the value of the slot's last frame before reusing the slot. It owns every object created
    // through it and releases them newest first, after the frame fence holds the last frame's value, so teardown never
    // releases an object the GPU still uses.
    class NativeContext
    {
      public:
        static constexpr uint32_t kFrameSlots = 3;
        static constexpr uint64_t kArenaAlignment = 256;
        // View-heap slots: 0 holds a null raw SRV and 1 a null raw UAV, so the zero Addr reaches a null descriptor as it does
        // in RojoRHI; the slots after them are handed out in order of creation.
        static constexpr uint32_t kViewHeapSize = 65'536;
        static constexpr uint32_t kSamplerHeapSize = 2'048;
        static constexpr uint32_t kFirstFreeView = 2;
        // The lists per frame a slot's timestamps hold beyond the frame's direct lists, such as the composed frame's
        // compute lists.
        static constexpr uint32_t kExtraTimedLists = 8;
        // The debug features the build selects (rojorhi_d3d12_debug), as RojoRHI's D3D12 backend enables them: the debug
        // layer and DRED for layer and gbv, and for mode in Debug builds; GPU-based validation for gbv.
#if defined(ROJORHI_D3D12_DEBUG_GBV)
        static constexpr bool kDebugLayer = true;
        static constexpr bool kGpuValidation = true;
#elif defined(ROJORHI_D3D12_DEBUG_LAYER) || (defined(ROJORHI_D3D12_DEBUG_MODE) && !defined(NDEBUG))
        static constexpr bool kDebugLayer = true;
        static constexpr bool kGpuValidation = false;
#else
        static constexpr bool kDebugLayer = false;
        static constexpr bool kGpuValidation = false;
#endif

        // One frame slot: per-list allocators and direct lists, and the frame-data arena.
        struct Slot
        {
            std::vector<ID3D12CommandAllocator*> allocators;
            std::vector<ID3D12GraphicsCommandList9*> lists;
            ID3D12Resource* arena = nullptr;
            std::byte* arenaData = nullptr;
            D3D12_GPU_VIRTUAL_ADDRESS arenaAddress = 0;
            uint64_t arenaSize = 0;
            uint64_t arenaUsed = 0;
            uint64_t frameValue = 0;   ///< The frame fence's value at the end of the slot's last frame; 0 before its first.
            uint64_t overrunAt = 0;    ///< The offset of the first push that did not fit, valid when overrunBytes is not 0.
            uint64_t overrunBytes = 0; ///< The size of the first push of the frame that did not fit; 0 when every push fit.
            NativeSlotTimestamps timestamps;

            // Copies bytes into the arena at the next 256-byte boundary and returns their GPU address. A push past the size
            // fixed at setup writes nothing and returns the arena's base address, so the list stays valid; the frame's
            // submissions are then not executed and endFrame returns an error naming the overrun.
            D3D12_GPU_VIRTUAL_ADDRESS push(std::span<const std::byte> bytes)
            {
                const uint64_t offset = (arenaUsed + kArenaAlignment - 1) & ~(kArenaAlignment - 1);
                if (offset + bytes.size() > arenaSize)
                {
                    if (overrunBytes == 0)
                    {
                        overrunAt = offset;
                        overrunBytes = bytes.size() == 0 ? 1 : bytes.size();
                    }
                    return arenaAddress;
                }
                std::memcpy(arenaData + offset, bytes.data(), bytes.size());
                arenaUsed = offset + bytes.size();
                return arenaAddress + offset;
            }
        };

        NativeContext() = default;
        NativeContext(const NativeContext&) = delete;
        NativeContext& operator=(const NativeContext&) = delete;
        ~NativeContext();

        // Opens the device in the order RojoRHI's D3D12 backend does: the Agility device factory on the D3D12\ directory
        // beside the executable, the build's debug features on that factory, then the adapter of the build's kind
        // (EnumWarpAdapter for warp; for hardware the first high-performance adapter that creates a 12_2 device meeting
        // the hardware floor), the message callback under the debug layer, then the queues, root signature and heaps.
        std::expected<void, std::string> open(const RunOptions& options);

        ID3D12Device12* device() const;
        IDXGIAdapter1* adapter() const;
        // The adapter's description, as UTF-8.
        std::string adapterName() const;
        const DXGI_ADAPTER_DESC1& adapterDesc() const;
        ID3D12CommandQueue* graphicsQueue() const;
        ID3D12CommandQueue* computeQueue() const;
        // The global root signature: a root SRV at t0 with DATA_VOLATILE, one root constant at b1, both heaps directly
        // indexed, every stage visible; the layout the module's DXIL reads.
        ID3D12RootSignature* rootSignature() const;
        ID3D12DescriptorHeap* viewHeap() const;
        ID3D12DescriptorHeap* samplerHeap() const;
        // The view heap then the sampler heap, as SetDescriptorHeaps takes them.
        std::span<ID3D12DescriptorHeap* const> heaps() const;
        // Signalled on the direct queue with 1, 2, 3 and so on, once at the end of each frame.
        ID3D12Fence* frameFence() const;
        // The cookie RegisterMessageCallback returned; 0 when the build enables no debug layer.
        DWORD messageCookie() const;
        // True once the open frame failed to begin: its slot's wait failed, so the GPU may still use the slot's lists.
        bool frameFailed() const;
        NativeCalls& calls();
        CallSnapshot callSnapshot() const;
        FeedbackCollector& collector();

        // Setup. Each object is owned by the context and named with its label.
        // Takes ownership of a +1 reference and names the object `label`.
        void own(ID3D12Object* object, std::string_view label);
        // Takes ownership of a +1 reference of an object D3D12 does not name.
        void own(IUnknown* object);
        // A DEFAULT buffer that allows unordered access; initial bytes go through an UPLOAD staging buffer and one waited
        // setup submission.
        std::expected<NativeBuffer, std::string> defaultBuffer(uint64_t size, std::string_view label, std::span<const std::byte> initial = {});
        // An UPLOAD buffer, zeroed then holding `initial`, mapped for its life.
        std::expected<NativeBuffer, std::string> uploadBuffer(uint64_t size, std::string_view label, std::span<const std::byte> initial = {});
        // A READBACK buffer, not mapped.
        std::expected<NativeBuffer, std::string> readbackBuffer(uint64_t size, std::string_view label);
        // A committed DEFAULT texture created in `layout`.
        std::expected<ID3D12Resource*, std::string> texture(
            const D3D12_RESOURCE_DESC1& desc, D3D12_BARRIER_LAYOUT layout, std::string_view label, const D3D12_CLEAR_VALUE* clear = nullptr
        );
        std::expected<ID3D12DescriptorHeap*, std::string> descriptorHeap(
            D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count, bool shaderVisible, std::string_view label
        );
        std::expected<ID3D12QueryHeap*, std::string> timestampHeap(uint32_t count, std::string_view label);
        std::expected<ID3D12Fence*, std::string> fence(std::string_view label);
        // A command allocator and a closed list of `type`, labelled `label` and `label`.allocator.
        std::expected<NativeList, std::string> commandList(D3D12_COMMAND_LIST_TYPE type, std::string_view label);

        // Views in the shader-visible heaps, at the index a shader reads. Views are handed out in order, so consecutive
        // calls fill consecutive slots. bufferViews writes the raw SRV of a buffer and, when it allows unordered access, its
        // raw UAV in the next slot, the pair a module Addr names by the SRV's index; it returns that index.
        std::expected<uint32_t, std::string> bufferViews(ID3D12Resource* buffer);
        std::expected<uint32_t, std::string> shaderResourceView(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& desc);
        std::expected<uint32_t, std::string> unorderedAccessView(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& desc);
        std::expected<uint32_t, std::string> sampler(const D3D12_SAMPLER_DESC& desc);
        D3D12_CPU_DESCRIPTOR_HANDLE viewCpu(uint32_t index) const;
        D3D12_GPU_DESCRIPTOR_HANDLE viewGpu(uint32_t index) const;
        D3D12_GPU_DESCRIPTOR_HANDLE samplerGpu(uint32_t index) const;

        // The DXIL of one entry beside the executable: Shaders/<name>.<entry>.dxil, which the Slang rule writes for a file
        // of several entries, else Shaders/<name>.dxil, written for a file of one entry or by a hand-written kernel's rule.
        // The bytes stay owned by the context.
        std::expected<D3D12_SHADER_BYTECODE, std::string> library(std::string_view name, std::string_view entry = {});
        // Pipelines through D3D12_PIPELINE_STATE_STREAM_DESC; a null root signature names the global one.
        std::expected<ID3D12PipelineState*, std::string> computePipeline(
            D3D12_SHADER_BYTECODE shader, std::string_view label, ID3D12RootSignature* root = nullptr
        );
        std::expected<ID3D12PipelineState*, std::string> graphicsPipeline(const NativeGraphicsDesc& desc, std::string_view label);

        // Creates the three frame slots; called once, after every resource exists.
        std::expected<void, std::string> prepareFrames(const NativeFrameShape& shape);

        // Frames. beginFrame waits until the frame fence holds the slot's frameValue (one counted SetEventOnCompletion,
        // none for a slot's first frame, whose value 0 the fence holds from creation), resets the slot's allocators
        // (one counted host call each) and arena, and expects `submissions` calls of commit. endFrame signals the frame
        // fence on the direct queue after the frame's submissions (one counted queue call). A frame's work on the compute
        // queue must be joined by a later direct-queue submission of the frame, which the signal follows. endFrame then
        // waits for the frame fence to hold that value, a wait counted nowhere, reports each submission's two timestamps
        // to the collector, in seconds of QueryPerformanceCounter through its queue's calibration, and returns the
        // collector's span of the frame.
        Slot& beginFrame(uint32_t index, uint32_t submissions);
        // ExecuteCommandLists of `lists` on `queue`, the direct or the compute queue: one counted queue call. The
        // submission's span runs from its first list's first timestamp to its last list's last, so both must have written
        // them. A frame that failed to begin or whose arena overran executes nothing; a commit outside a frame, or of a
        // first or last list without timestamps, executes nothing more and fails the frame at endFrame.
        void commit(ID3D12CommandQueue* queue, std::span<ID3D12CommandList* const> lists);
        // Timestamps, as RojoRHI's D3D12 submission timing writes them, counted nowhere. startTimestamps, right after a
        // frame list's Reset, claims the slot's next pair and writes its first timestamp; a frame that resets more lists
        // than the slot holds pairs fails at endFrame. endTimestamps, after the list's last command and before its own
        // resolves and Close, writes the last and resolves the pair into the slot's readback buffer; it does nothing for
        // a list that wrote no first timestamp or already wrote its last, so calling it again before Close is harmless.
        // `type` is the list's type, direct or compute, which picks the readback buffer of its queue.
        void startTimestamps(ID3D12GraphicsCommandList9* list, D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT);
        void endTimestamps(ID3D12GraphicsCommandList9* list);
        std::expected<std::optional<double>, std::string> endFrame();

        // Outside every measurement. Each waits for every frame signalled so far, records into the context's own direct list,
        // submits on the direct queue and waits for it.
        std::expected<void, std::string> runSetup(const std::function<void(ID3D12GraphicsCommandList9& list)>& record);
        std::expected<std::vector<std::byte>, std::string> readBuffer(ID3D12Resource* source, uint64_t offset, uint64_t size);
        // One subresource of a texture in `layout`, its rows tight (without the 256-byte pitch); the texture is left in
        // `layout`.
        std::expected<std::vector<std::byte>, std::string> readTexture(ID3D12Resource* image, D3D12_BARRIER_LAYOUT layout, uint32_t subresource = 0);
        // Writes tight rows into one subresource of a texture, moving it from `before` to COPY_DEST and then to `after`.
        std::expected<void, std::string> writeTexture(
            ID3D12Resource* image, uint32_t subresource, std::span<const std::byte> bytes, D3D12_BARRIER_LAYOUT before, D3D12_BARRIER_LAYOUT after
        );

      private:
        std::expected<void, std::string> openDevice();
        std::expected<void, std::string> createShared();
        std::expected<uint32_t, std::string> claimViews(uint32_t count, std::string_view what);
        std::expected<NativeBuffer, std::string> committedBuffer(D3D12_HEAP_TYPE heap, uint64_t size, std::string_view label);
        std::expected<ID3D12PipelineState*, std::string> createPipeline(const D3D12_PIPELINE_STATE_STREAM_DESC& desc, std::string_view label);
        // Waits up to the frame wait limit for `target` to reach `value`, counting one SetEventOnCompletion when `counted`;
        // a timeout marks the context abandoned, after which every wait fails at once.
        std::expected<void, std::string> waitFor(ID3D12Fence* target, uint64_t value, std::string_view what, bool counted = true);
        // Reads both queues' calibration and makes every slot's timestamps, `pairs` pairs each; the caller takes the
        // counts of the calls back out.
        std::expected<void, std::string> prepareTimestamps(uint32_t pairs);
        // After endFrame's checks passed: waits for the frame, reports each submission's timestamps of `slot` and returns
        // the collector's span.
        std::expected<std::optional<double>, std::string> reportTimestamps(const Slot& slot);

        std::vector<ComPtr<IUnknown>> m_owned;
        ID3D12Device12* m_device = nullptr;
        IDXGIAdapter1* m_adapter = nullptr;
        DXGI_ADAPTER_DESC1 m_adapterDesc{};
        ComPtr<ID3D12InfoQueue1> m_infoQueue; ///< Registered with the message callback while m_cookie is not 0.
        DWORD m_cookie = 0;
        ID3D12CommandQueue* m_graphics = nullptr;
        ID3D12CommandQueue* m_compute = nullptr;
        ID3D12RootSignature* m_root = nullptr;
        std::array<ID3D12DescriptorHeap*, 2> m_heaps{}; ///< The view heap, then the sampler heap.
        uint32_t m_viewIncrement = 0;
        uint32_t m_samplerIncrement = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE m_viewCpuStart{};
        D3D12_GPU_DESCRIPTOR_HANDLE m_viewGpuStart{};
        D3D12_CPU_DESCRIPTOR_HANDLE m_samplerCpuStart{};
        D3D12_GPU_DESCRIPTOR_HANDLE m_samplerGpuStart{};
        uint32_t m_nextView = kFirstFreeView;
        uint32_t m_nextSampler = 0;
        ID3D12Fence* m_frameFence = nullptr;
        ID3D12Fence* m_setupFence = nullptr;
        NativeList m_setup;
        HANDLE m_event = nullptr;    ///< The auto-reset event every wait arms.
        uint64_t m_frameValue = 0;   ///< The frame fence's value signalled last.
        uint64_t m_setupValue = 0;   ///< The setup fence's value signalled last.
        Slot* m_frameSlot = nullptr; ///< The slot of the frame open now.
        uint32_t m_submissions = 0;  ///< Submissions the open frame announced.
        uint32_t m_committed = 0;    ///< Submissions the open frame made.
        std::string m_frameError;    ///< A failed slot wait of the frame open now, which its endFrame returns.
        std::vector<Slot> m_slots;
        // One submission of the open frame: its queue, 0 direct or 1 compute, and the queries of its first list's first
        // timestamp and its last list's last.
        struct TimedSubmission
        {
            uint32_t queue = 0;
            uint32_t start = 0;
            uint32_t end = 0;
        };
        std::vector<TimedSubmission> m_timed;          ///< The open frame's submissions, reserved by prepareFrames.
        std::vector<NativeTimedList> m_timedLists;     ///< The open frame's lists with timestamps, reserved by prepareFrames.
        size_t m_commitCursor = 0;                     ///< Where commit looks for its first list in m_timedLists.
        std::array<NativeQueueClock, 2> m_clocks{};    ///< The direct and the compute queue's calibration.
        double m_cpuTicksPerSecond = 0.0;              ///< QueryPerformanceFrequency.
        std::vector<std::vector<std::byte>> m_shaders; ///< The DXIL every library call read.
        NativeCalls m_calls;
        // On the heap so an abandoned context can leave it behind for reports still in flight.
        std::unique_ptr<FeedbackCollector> m_collector = std::make_unique<FeedbackCollector>();
        bool m_abandoned = false; ///< A wait timed out: the GPU may still use every object, so none is released.
    };
} // namespace rojoRHI::bench
