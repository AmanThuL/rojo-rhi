// Standalone integration probe: compile the actual backend without adding ImGui to Tests.
#include "imgui_internal.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-bridge-casts-disallowed-in-nonarc"
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wobjc-missing-super-calls"
#include LMX_IMGUI_BACKEND_SOURCE
#pragma clang diagnostic pop

#include <objc/runtime.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {

std::vector<id<MTLBuffer>> borrowed;
IMP originalDequeue = nullptr;
unsigned failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

// Observe actual checkouts independently of the backend's used/reusable container design.
MetalBuffer* observeDequeue(id object, SEL selector, NSUInteger length, id<MTLDevice> device) {
    using Dequeue = MetalBuffer* (*)(id, SEL, NSUInteger, id<MTLDevice>);
    MetalBuffer* result =
        reinterpret_cast<Dequeue>(originalDequeue)(object, selector, length, device);
    borrowed.push_back(result.buffer);
    return result;
}

struct Geometry {
    ImDrawList list{ImGui::GetDrawListSharedData()};
    ImDrawData data;

    Geometry(id<MTLTexture> white, bool green) {
        const ImU32 color = green ? IM_COL32(0, 255, 0, 255) : IM_COL32(255, 0, 0, 255);
        const float left = green ? 16.0f : 0.0f;
        list.VtxBuffer.push_back({{left, 0}, {0, 0}, color});
        list.VtxBuffer.push_back({{left + 16, 0}, {0, 0}, color});
        list.VtxBuffer.push_back({{left + 16, 32}, {0, 0}, color});
        list.VtxBuffer.push_back({{left, 32}, {0, 0}, color});
        const std::array<ImDrawIdx, 6> indices = green ? std::array<ImDrawIdx, 6>{0, 2, 3, 0, 1, 2}
                                                       : std::array<ImDrawIdx, 6>{0, 1, 2, 0, 2, 3};
        for (auto index : indices)
            list.IdxBuffer.push_back(index);
        ImDrawCmd command;
        command.ClipRect = {0, 0, 32, 32};
        command.TexRef = ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(white)));
        command.ElemCount = 6;
        list.CmdBuffer.push_back(command);
        data.Valid = true;
        data.DisplaySize = {32, 32};
        data.FramebufferScale = {1, 1};
        data.AddDrawList(&list);
    }
};

id<MTLTexture> makeTarget(id<MTLDevice> device) {
    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:32
                                                          height:32
                                                       mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> result = [device newTextureWithDescriptor:descriptor];
    result.label = @"imgui.probe.output";
    return result;
}

MTL4RenderPassDescriptor* makePass(id<MTLTexture> target) {
    MTL4RenderPassDescriptor* result = [[MTL4RenderPassDescriptor new] autorelease];
    result.colorAttachments[0].texture = target;
    result.colorAttachments[0].loadAction = MTLLoadActionClear;
    result.colorAttachments[0].storeAction = MTLStoreActionStore;
    result.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    return result;
}

bool matches(id<MTLBuffer> buffer, const void* bytes, size_t size) {
    return buffer.length >= size && std::memcmp(buffer.contents, bytes, size) == 0;
}

void checkPixel(id<MTLTexture> target, NSUInteger x, bool green) {
    std::array<unsigned char, 4> pixel;
    [target getBytes:pixel.data()
         bytesPerRow:4
          fromRegion:MTLRegionMake2D(x, 16, 1, 1)
         mipmapLevel:0];
    const std::array<unsigned char, 4> expected =
        green ? std::array<unsigned char, 4>{0, 255, 0, 255}
              : std::array<unsigned char, 4>{255, 0, 0, 255};
    check(pixel == expected, "retired render target preserves its own geometry/color");
}

int run() {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device || ![device supportsFamily:MTLGPUFamilyMetal4]) {
        std::fprintf(stderr, "Metal 4 Apple Silicon device required\n");
        return 2;
    }
    NSError* error = nil;
    id<MTL4CommandQueue> queue =
        [device newMTL4CommandQueueWithDescriptor:[MTL4CommandQueueDescriptor new] error:&error];
    if (!queue) {
        std::fprintf(stderr, "Queue creation failed: %s\n", error.localizedDescription.UTF8String);
        return 2;
    }
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    check(ImGui_ImplMetal4_Init(device, queue, 3), "backend initializes");
    Method method = class_getInstanceMethod([MetalContext class],
                                            @selector(dequeueReusableBufferOfLength:device:));
    originalDequeue = method_setImplementation(method, reinterpret_cast<IMP>(observeDequeue));

    MTLTextureDescriptor* whiteDescriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:1
                                                          height:1
                                                       mipmapped:NO];
    whiteDescriptor.storageMode = MTLStorageModeShared;
    whiteDescriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> white = [device newTextureWithDescriptor:whiteDescriptor];
    white.label = @"imgui.probe.white";
    const uint32_t whitePixel = 0xffffffff;
    [white replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
             mipmapLevel:0
               withBytes:&whitePixel
             bytesPerRow:4];
    Geometry red(white, false);
    Geometry green(white, true);
    std::array<id<MTLTexture>, 8> targets;
    for (auto& target : targets)
        target = makeTarget(device);

    // Hold every submission until slot zero is revisited: overwrites fail deterministically,
    // independently of whether a fast GPU happens to finish the main UI before the next upload.
    id<MTLSharedEvent> gate = [device newSharedEvent];
    gate.label = @"imgui.probe.gpu-gate";
    [queue waitForEvent:gate value:1];
    std::atomic<bool> revisitReturned{false};
    std::atomic<bool> observedBlocked{false};
    for (int frame = 0; frame < 4; ++frame) {
        const int slot = frame % 3;
        std::thread releaseGate;
        if (frame == 3) {
            releaseGate = std::thread([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                observedBlocked = !revisitReturned.load();
                gate.signaledValue = 1;
            });
        }
        ImGui_ImplMetal4_NewFrame(makePass(targets[frame * 2]), slot);
        if (frame == 3) {
            revisitReturned = true;
            releaseGate.join();
            check(observedBlocked, "slot revisit waits for the pending platform retirement event");
        }
        MetalContext* context = ImGui_ImplMetal4_GetBackendData()->SharedMetalContext;
        id<MTL4CommandBuffer> commands = [device newCommandBuffer];
        [commands beginCommandBufferWithAllocator:context.commandAllocators[slot]];
        const size_t first = borrowed.size();
        for (int window = 0; window < 2; ++window) {
            id<MTLTexture> target = targets[frame * 2 + window];
            [context.residencySet addAllocation:target];
            id<MTL4RenderCommandEncoder> encoder =
                [commands renderCommandEncoderWithDescriptor:makePass(target)];
            ImGui_ImplMetal4_RenderDrawData(window == 0 ? &red.data : &green.data, commands,
                                            encoder);
            [encoder endEncoding];
            check(borrowed.size() == first + (window + 1) * 2,
                  "each draw data borrows vertex and index buffers");
        }
        check(matches(borrowed[first], red.list.VtxBuffer.Data, red.list.VtxBuffer.size_in_bytes()),
              "second viewport must not overwrite first viewport vertex upload");
        check(matches(borrowed[first + 1], red.list.IdxBuffer.Data,
                      red.list.IdxBuffer.size_in_bytes()),
              "second viewport must not overwrite first viewport index upload");
        for (size_t index = first; index < borrowed.size(); ++index)
            for (size_t previous = frame == 3 ? first : 0; previous < index; ++previous)
                check(borrowed[index] != borrowed[previous],
                      "pending uploads have distinct buffer identities");
        if (frame == 3) {
            for (size_t index = first; index < borrowed.size(); ++index) {
                bool reused = false;
                for (size_t previous = 0; previous < 4; ++previous)
                    reused |= borrowed[index] == borrowed[previous];
                check(reused, "retired slot zero reuses its original storage");
            }
        }
        [commands endCommandBuffer];
        [queue commit:&commands count:1];
        // Same per-slot event protocol as the backend's platform-window submissions.
        [queue signalEvent:context.events[slot] value:++context->eventValues[slot]];
        [commands release];
    }
    id<MTLSharedEvent> drained = [device newSharedEvent];
    [queue signalEvent:drained value:1];
    check([drained waitUntilSignaledValue:1 timeoutMS:10000],
          "queue-wide shutdown retirement completes");
    for (int frame = 0; frame < 4; ++frame) {
        checkPixel(targets[frame * 2], 8, false);
        checkPixel(targets[frame * 2 + 1], 24, true);
    }
    method_setImplementation(method, originalDequeue);
    ImGui_ImplMetal4_Shutdown();
    // Geometry uses shared draw-list data and is destroyed before its ImGui context by the caller.
    std::printf("%s: 4 frames, 3 paced slots, 8 draw-data uploads; %u failures\n",
                failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}

} // namespace

int main() {
    @autoreleasepool {
        const int result = run();
        if (ImGui::GetCurrentContext())
            ImGui::DestroyContext();
        return result;
    }
}
