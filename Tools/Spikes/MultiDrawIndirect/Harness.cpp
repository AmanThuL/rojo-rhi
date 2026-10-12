// Spike 3 host: native Metal 4, no RojoRHI code. Throwaway spike source; see
// docs/research/2026-10-12-spike-3-multi-draw-indirect.md for what it measures and why.
//
//   Harness limits METALLIB
//   Harness run METALLIB MAX_COUNT ORDER correctness|timing OUTPUT_JSON
//
// Diagnostic switches, off for every recorded run: SPIKE3_CPU_RANGE executes arm E with a CPU
// range; SPIKE3_ARG_ENCODER hands the ICB to the kernel through an argument encoder;
// SPIKE3_OBJECT_BINDS sets the mesh ICB's object-buffer bind count; SPIKE3_CPU_ENCODE encodes the
// mesh ICB on the CPU instead of in the kernel.
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <dispatch/dispatch.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kTargetSize = 256;
constexpr uint32_t kWarmup = 50;
constexpr uint32_t kMeasured = 200;
constexpr uint32_t kThreadgroup = 64;
constexpr uint32_t kIcbProbe = 1u << 20;

struct DrawArgs {
    uint32_t drawId, vertexCount, instanceCount, firstVertex, firstInstance;
};
struct IndexedArgs {
    uint32_t drawId, indexCount, instanceCount, firstIndex;
    int32_t baseVertex;
    uint32_t firstInstance;
};
struct MeshArgs {
    uint32_t drawId, x, y, z;
};
struct NativeDraw {
    uint32_t vertexCount, instanceCount, vertexStart, baseInstance;
};
struct NativeIndexed {
    uint32_t indexCount, instanceCount, indexStart;
    int32_t baseVertex;
    uint32_t baseInstance;
};
struct NativeMesh {
    uint32_t x, y, z;
};
static_assert(sizeof(DrawArgs) == 20 && sizeof(IndexedArgs) == 24 && sizeof(MeshArgs) == 16);
static_assert(sizeof(NativeDraw) == sizeof(MTL::DrawPrimitivesIndirectArguments));
static_assert(sizeof(NativeIndexed) == sizeof(MTL::DrawIndexedPrimitivesIndirectArguments));
static_assert(sizeof(NativeMesh) == sizeof(MTL::DispatchThreadgroupsIndirectArguments));

// Mirrors Root in Kernels.metal.
struct Root {
    uint64_t records;
    uint64_t count;
    uint64_t out;
    uint64_t range;
    uint64_t indices;
    uint32_t maxCount;
    uint32_t pad;
};

enum class Variant { Draw, Indexed, Mesh };
constexpr Variant kVariants[] = {Variant::Draw, Variant::Indexed, Variant::Mesh};

const char* nameOf(Variant v) {
    return v == Variant::Draw ? "draw" : v == Variant::Indexed ? "indexed" : "mesh";
}

size_t recordSize(Variant v) {
    return v == Variant::Draw ? sizeof(DrawArgs)
           : v == Variant::Indexed ? sizeof(IndexedArgs)
                                   : sizeof(MeshArgs);
}

size_t nativeSize(Variant v) {
    return v == Variant::Draw ? sizeof(NativeDraw)
           : v == Variant::Indexed ? sizeof(NativeIndexed)
                                   : sizeof(NativeMesh);
}

// A permutation of [0, maxCount) for power-of-two maxCount, since 7919 is an odd prime.
uint32_t drawIdOf(uint32_t i, uint32_t maxCount) {
    return uint32_t((7919ull * i + 13) % maxCount);
}

NS::String* str(const std::string& s) {
    return NS::String::string(s.c_str(), NS::UTF8StringEncoding);
}

void check(bool ok, const std::string& what, NS::Error* error = nullptr) {
    if (!ok) {
        std::string reason = error ? error->localizedDescription()->utf8String() : "";
        throw std::runtime_error(what + (reason.empty() ? "" : ": " + reason));
    }
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

std::string thermal() {
    switch (NS::ProcessInfo::processInfo()->thermalState()) {
    case NS::ProcessInfoThermalStateNominal: return "nominal";
    case NS::ProcessInfoThermalStateFair: return "fair";
    case NS::ProcessInfoThermalStateSerious: return "serious";
    case NS::ProcessInfoThermalStateCritical: return "critical";
    }
    return "unknown";
}

std::string jsonList(const std::vector<double>& v) {
    std::ostringstream s;
    s << "[";
    for (size_t i = 0; i < v.size(); ++i)
        s << (i ? "," : "") << v[i];
    s << "]";
    return s.str();
}

class Gpu {
public:
    MTL::Device* device = nullptr;
    MTL4::CommandQueue* queue = nullptr;
    MTL4::CommandAllocator* allocator = nullptr;
    MTL::ResidencySet* residency = nullptr;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);

    Gpu() {
        device = MTL::CreateSystemDefaultDevice();
        check(device && device->supportsFamily(MTL::GPUFamilyMetal4), "Metal 4 device");
        NS::Error* error = nullptr;
        auto* queueDesc = MTL4::CommandQueueDescriptor::alloc()->init();
        queueDesc->setLabel(str("rojorhi.spike3.queue"));
        queue = device->newMTL4CommandQueue(queueDesc, &error);
        check(queue, "queue", error);
        allocator = device->newCommandAllocator();
        check(allocator, "allocator");
        auto* resDesc = MTL::ResidencySetDescriptor::alloc()->init();
        resDesc->setLabel(str("rojorhi.spike3.residency"));
        residency = device->newResidencySet(resDesc, &error);
        check(residency, "residency set", error);
        queue->addResidencySet(residency);
    }

    MTL::Buffer* buffer(size_t bytes, const std::string& name) {
        auto* b = device->newBuffer(std::max<size_t>(bytes, 16), MTL::ResourceStorageModeShared);
        check(b, name);
        b->setLabel(str(name));
        std::memset(b->contents(), 0, b->length());
        residency->addAllocation(b);
        return b;
    }

    struct Frame {
        double gpuNs = 0;
        std::string error;
    };

    // Records one command buffer through `encode`, commits it and waits for its feedback.
    template <class Encode>
    Frame frame(const std::string& name, Encode encode) {
        auto* commands = device->newCommandBuffer();
        check(commands, "command buffer");
        commands->beginCommandBuffer(allocator);
        commands->setLabel(str(name));
        encode(commands);
        commands->endCommandBuffer();
        Frame result;
        auto* options = MTL4::CommitOptions::alloc()->init();
        options->addFeedbackHandler([&](MTL4::CommitFeedback* feedback) {
            if (feedback->error())
                result.error = feedback->error()->localizedDescription()->utf8String();
            result.gpuNs = (feedback->GPUEndTime() - feedback->GPUStartTime()) * 1e9;
            dispatch_semaphore_signal(done);
        });
        const MTL4::CommandBuffer* list[] = {commands};
        queue->commit(list, 1, options);
        check(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC)) ==
                  0,
              name + " timed out");
        options->release();
        commands->release();
        allocator->reset();
        return result;
    }
};

MTL::IndirectCommandBuffer* newIcb(MTL::Device* device, Variant v, uint32_t commands) {
    auto* d = MTL::IndirectCommandBufferDescriptor::alloc()->init();
    d->setInheritPipelineState(true);
    if (v == Variant::Mesh) {
        d->setCommandTypes(MTL::IndirectCommandTypeDrawMeshThreadgroups);
        d->setInheritBuffers(false);
        const char* binds = std::getenv("SPIKE3_OBJECT_BINDS");
        d->setMaxObjectBufferBindCount(binds ? std::stoul(binds) : 1);
    } else {
        d->setCommandTypes(v == Variant::Draw ? MTL::IndirectCommandTypeDraw
                                              : MTL::IndirectCommandTypeDrawIndexed);
        d->setInheritBuffers(true);
    }
    auto* icb = device->newIndirectCommandBuffer(
        d, commands,
        std::getenv("SPIKE3_CPU_ENCODE") ? MTL::ResourceStorageModeShared
                                         : MTL::ResourceStorageModePrivate);
    d->release();
    return icb;
}

int limits(const char*) {
    Gpu gpu;
    std::cout << "{\"device\":\"" << gpu.device->name()->utf8String()
              << "\",\"apple9\":" << gpu.device->supportsFamily(MTL::GPUFamilyApple9)
              << ",\"os\":\""
              << NS::ProcessInfo::processInfo()->operatingSystemVersionString()->utf8String()
              << "\",\"icbProbeCommands\":" << kIcbProbe << ",\"icbProbe\":{";
    bool first = true;
    for (Variant v : kVariants) {
        auto* icb = newIcb(gpu.device, v, kIcbProbe);
        std::cout << (first ? "" : ",") << "\"" << nameOf(v) << "\":" << (icb != nullptr);
        if (icb)
            icb->release();
        first = false;
    }
    std::cout << "}}\n";
    return 0;
}

enum class Arm { E, L, N, N0 };

const char* nameOf(Arm a) {
    return a == Arm::E ? "E" : a == Arm::L ? "L" : a == Arm::N ? "N" : "N0";
}

struct Fill {
    std::string name;
    std::optional<uint32_t> count;  // nullopt is a null count
};

class Spike {
public:
    Spike(Gpu& gpu, const char* metallib, uint32_t maxCount) : gpu(gpu), maxCount(maxCount) {
        MTL::Device* device = gpu.device;
        NS::Error* error = nullptr;
        auto* library = device->newLibrary(NS::URL::fileURLWithPath(str(metallib)), &error);
        check(library, "library", error);
        compiler = device->newCompiler(MTL4::CompilerDescriptor::alloc()->init(), &error);
        check(compiler, "compiler", error);
        auto function = [&](const char* name) {
            auto* f = MTL4::LibraryFunctionDescriptor::alloc()->init();
            f->setLibrary(library);
            f->setName(str(name));
            return f;
        };
        for (const char* name :
             {"encodeDraw", "encodeIndexed", "encodeMesh", "copyDraw", "copyIndexed", "copyMesh"}) {
            auto* d = MTL4::ComputePipelineDescriptor::alloc()->init();
            d->setComputeFunctionDescriptor(function(name));
            d->setLabel(str(std::string("rojorhi.spike3.") + name));
            kernels[name] = compiler->newComputePipelineState(d, nullptr, &error);
            check(kernels[name], name, error);
        }
        auto support = [](bool on) {
            return on ? MTL4::IndirectCommandBufferSupportStateEnabled
                      : MTL4::IndirectCommandBufferSupportStateDisabled;
        };
        for (bool flag : {true, false}) {
            auto* d = MTL4::RenderPipelineDescriptor::alloc()->init();
            d->setVertexFunctionDescriptor(function("drawVertex"));
            d->setFragmentFunctionDescriptor(function("drawFragment"));
            d->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatR32Uint);
            d->setSupportIndirectCommandBuffers(support(flag));
            d->setLabel(str(flag ? "rojorhi.spike3.draw" : "rojorhi.spike3.drawNoIcb"));
            (flag ? drawPipeline : drawPipelineNoIcb) =
                compiler->newRenderPipelineState(d, nullptr, &error);
            check(flag ? drawPipeline : drawPipelineNoIcb, "draw pipeline", error);
        }
        auto* m = MTL4::MeshRenderPipelineDescriptor::alloc()->init();
        m->setObjectFunctionDescriptor(function("meshObject"));
        m->setMeshFunctionDescriptor(function("meshMain"));
        m->setFragmentFunctionDescriptor(function("drawFragment"));
        m->setPayloadMemoryLength(16);
        m->setMaxTotalThreadsPerObjectThreadgroup(1);
        m->setMaxTotalThreadsPerMeshThreadgroup(1);
        m->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatR32Uint);
        m->setSupportIndirectCommandBuffers(support(true));
        m->setLabel(str("rojorhi.spike3.mesh"));
        meshPipeline = compiler->newRenderPipelineState(m, nullptr, &error);
        check(meshPipeline, "mesh pipeline", error);

        indices = gpu.buffer(sizeof(uint32_t), "rojorhi.spike3.indices");
        countBuffer = gpu.buffer(sizeof(uint32_t), "rojorhi.spike3.count");
        for (Variant v : kVariants) {
            std::string tag = nameOf(v);
            Resources& r = resources[v];
            r.records = gpu.buffer(maxCount * recordSize(v), "rojorhi.spike3.records." + tag);
            r.scratch = gpu.buffer(maxCount * nativeSize(v), "rojorhi.spike3.scratch." + tag);
            r.floor = gpu.buffer(maxCount * nativeSize(v), "rojorhi.spike3.floor." + tag);
            r.range = gpu.buffer(2 * sizeof(uint32_t), "rojorhi.spike3.range." + tag);
            r.icb = newIcb(device, v, maxCount);
            check(r.icb, "indirect command buffer " + tag);
            r.icb->setLabel(str("rojorhi.spike3.icb." + tag));
            gpu.residency->addAllocation(r.icb);
            r.icbArg = gpu.buffer(sizeof(MTL::ResourceID), "rojorhi.spike3.icbArg." + tag);
            if (std::getenv("SPIKE3_ARG_ENCODER")) {
                auto* arg = MTL::ArgumentDescriptor::alloc()->init();
                arg->setDataType(MTL::DataTypeIndirectCommandBuffer);
                arg->setIndex(0);
                arg->setAccess(MTL::ArgumentAccessReadWrite);
                const NS::Object* args[] = {arg};
                auto* encoder = device->newArgumentEncoder(NS::Array::array(args, 1));
                check(encoder && encoder->encodedLength() <= r.icbArg->length(), "ICB encoder");
                encoder->setArgumentBuffer(r.icbArg, 0);
                encoder->setIndirectCommandBuffer(r.icb, 0);
            } else {
                *static_cast<MTL::ResourceID*>(r.icbArg->contents()) = r.icb->gpuResourceID();
            }
            for (Arm a : {Arm::E, Arm::L}) {
                bool e = a == Arm::E;
                auto* root = gpu.buffer(sizeof(Root), "rojorhi.spike3.root." + tag + nameOf(a));
                *static_cast<Root*>(root->contents()) = {
                    r.records->gpuAddress(), 0,
                    e ? r.icbArg->gpuAddress() : r.scratch->gpuAddress(),
                    r.range->gpuAddress(), indices->gpuAddress(), maxCount, 0};
                (e ? r.rootE : r.rootL) = root;
                (e ? r.tableE : r.tableL) = table(1, "rojorhi.spike3.args." + tag + nameOf(a));
                (e ? r.tableE : r.tableL)->setAddress(root->gpuAddress(), 0);
            }
            fillRecords(v, r);
        }
        meshTable = table(1, "rojorhi.spike3.args.meshDraw");

        auto* t = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatR32Uint, kTargetSize,
                                                              kTargetSize, false);
        t->setStorageMode(MTL::StorageModeShared);
        t->setUsage(MTL::TextureUsageRenderTarget);
        target = device->newTexture(t);
        check(target, "target");
        target->setLabel(str("rojorhi.spike3.target"));
        gpu.residency->addAllocation(target);
        gpu.residency->commit();

        pass = MTL4::RenderPassDescriptor::alloc()->init();
        auto* a = pass->colorAttachments()->object(0);
        a->setTexture(target);
        a->setLoadAction(MTL::LoadActionClear);
        a->setStoreAction(MTL::StoreActionStore);
        a->setClearColor(MTL::ClearColor::Make(0, 0, 0, 0));
    }

    std::vector<Fill> fills(bool timing) const {
        // Correctness runs the full fill first, so a stale command left past a smaller count shows.
        std::vector<Fill> f = {{"full", maxCount}, {"quarter", maxCount / 4}, {"zero", 0}};
        if (!timing) {
            f.push_back({"double", 2 * maxCount});
            f.push_back({"null", std::nullopt});
        }
        return f;
    }

    void setFill(const Fill& fill) {
        if (fill.count)
            *static_cast<uint32_t*>(countBuffer->contents()) = *fill.count;
        for (auto& [v, r] : resources) {
            for (auto* root : {r.rootE, r.rootL})
                static_cast<Root*>(root->contents())->count =
                    fill.count ? countBuffer->gpuAddress() : 0;
        }
        drawn = fill.count ? std::min(*fill.count, maxCount) : maxCount;
        if (std::getenv("SPIKE3_CPU_ENCODE")) {
            Resources& m = resources[Variant::Mesh];
            for (uint32_t i = 0; i < drawn; ++i) {
                auto* cmd = m.icb->indirectRenderCommand(i);
                cmd->setObjectBuffer(m.records, i * sizeof(MeshArgs), 0);
                cmd->drawMeshThreadgroups(MTL::Size::Make(1, 1, 1), MTL::Size::Make(1, 1, 1),
                                          MTL::Size::Make(1, 1, 1));
            }
        }
    }

    struct Encoded {
        double cpuNs = 0;
        uint32_t calls = 0;  // native encoder and argument-table calls made for the RojoRHI call
    };

    // One frame: the arm's kernel encoder, if any, then one render pass holding its draws.
    Encoded encode(MTL4::CommandBuffer* commands, Variant v, Arm arm) {
        using Clock = std::chrono::steady_clock;
        Resources& r = resources[v];
        Encoded out;
        auto span = [&](Clock::time_point start) {
            out.cpuNs += std::chrono::duration<double, std::nano>(Clock::now() - start).count();
        };
        const bool cpuEncoded = std::getenv("SPIKE3_CPU_ENCODE") && v == Variant::Mesh &&
                                arm == Arm::E;
        if ((arm == Arm::E || arm == Arm::L) && !cpuEncoded) {
            bool e = arm == Arm::E;
            std::string kernel = std::string(e ? "encode" : "copy") +
                                 (v == Variant::Draw ? "Draw" : v == Variant::Indexed ? "Indexed"
                                                                                      : "Mesh");
            auto* pipeline = kernels[kernel];
            auto start = Clock::now();
            auto* c = commands->computeCommandEncoder();
            c->setComputePipelineState(pipeline);
            c->setArgumentTable(e ? r.tableE : r.tableL);
            c->dispatchThreads(MTL::Size::Make(maxCount, 1, 1),
                               MTL::Size::Make(kThreadgroup, 1, 1));
            c->endEncoding();
            span(start);
            out.calls += 5;
        }
        auto* e = commands->renderCommandEncoder(pass);
        e->setViewport({0, 0, double(kTargetSize), double(kTargetSize), 0, 1});
        e->setRenderPipelineState(v == Variant::Mesh ? meshPipeline
                                  : arm == Arm::N0   ? drawPipelineNoIcb
                                                     : drawPipeline);
        auto start = Clock::now();
        if (arm == Arm::E || arm == Arm::L) {
            e->barrierAfterQueueStages(MTL::StageDispatch,
                                       MTL::StageVertex | MTL::StageObject | MTL::StageMesh |
                                           MTL::StageFragment,
                                       MTL4::VisibilityOptionDevice);
            ++out.calls;
        }
        const uint64_t indexAddress = indices->gpuAddress();
        if (arm == Arm::E) {
            if (std::getenv("SPIKE3_CPU_RANGE"))
                e->executeCommandsInBuffer(r.icb, NS::Range::Make(0, drawn));
            else
                e->executeCommandsInBuffer(r.icb, r.range->gpuAddress());
            ++out.calls;
        } else {
            const bool loop = arm == Arm::L;
            const uint64_t base = (loop ? r.scratch : r.floor)->gpuAddress();
            const uint32_t draws = loop ? maxCount : drawn;
            if (v == Variant::Draw) {
                for (uint32_t i = 0; i < draws; ++i)
                    e->drawPrimitives(MTL::PrimitiveTypePoint, base + i * sizeof(NativeDraw));
            } else if (v == Variant::Indexed) {
                for (uint32_t i = 0; i < draws; ++i)
                    e->drawIndexedPrimitives(MTL::PrimitiveTypePoint, MTL::IndexTypeUInt32,
                                             indexAddress, sizeof(uint32_t),
                                             base + i * sizeof(NativeIndexed));
            } else {
                const uint64_t records = r.records->gpuAddress();
                e->setArgumentTable(meshTable, MTL::RenderStageObject);
                for (uint32_t i = 0; i < draws; ++i) {
                    meshTable->setAddress(records + i * sizeof(MeshArgs), 0);
                    e->drawMeshThreadgroups(base + i * sizeof(NativeMesh),
                                            MTL::Size::Make(1, 1, 1), MTL::Size::Make(1, 1, 1));
                }
                out.calls += 1 + draws;
            }
            out.calls += draws;
        }
        span(start);
        e->endEncoding();
        return out;
    }

    // Pixels that differ from the image `drawn` draws must produce.
    size_t mismatches(std::string& firstBad) const {
        std::vector<uint32_t> want(size_t(kTargetSize) * kTargetSize, 0), got(want.size());
        for (uint32_t i = 0; i < drawn; ++i) {
            uint32_t id = drawIdOf(i, maxCount);
            want[id] = id + 1;
        }
        target->getBytes(got.data(), kTargetSize * sizeof(uint32_t),
                         MTL::Region::Make2D(0, 0, kTargetSize, kTargetSize), 0);
        size_t bad = 0;
        for (size_t p = 0; p < want.size(); ++p) {
            if (got[p] != want[p] && !bad++)
                firstBad = "pixel " + std::to_string(p) + " got " + std::to_string(got[p]) +
                           " want " + std::to_string(want[p]);
        }
        return bad;
    }

    // The execution range the encode kernel last wrote for `v`.
    std::string rangeJson(Variant v) {
        auto* range = static_cast<const uint32_t*>(resources[v].range->contents());
        return "[" + std::to_string(range[0]) + "," + std::to_string(range[1]) + "]";
    }

    std::string memoryJson() {
        std::ostringstream s;
        bool first = true;
        for (auto& [v, r] : resources) {
            s << (first ? "" : ",") << "\"" << nameOf(v)
              << "\":{\"icbAllocatedBytes\":" << r.icb->allocatedSize()
              << ",\"scratchAllocatedBytes\":" << r.scratch->allocatedSize()
              << ",\"scratchBytes\":" << maxCount * nativeSize(v) << "}";
            first = false;
        }
        return s.str();
    }

    uint32_t drawn = 0;

private:
    struct Resources {
        MTL::Buffer* records = nullptr;
        MTL::Buffer* scratch = nullptr;  // L: native records written by the kernel
        MTL::Buffer* floor = nullptr;    // N: native records written by the CPU
        MTL::Buffer* range = nullptr;
        MTL::Buffer* icbArg = nullptr;
        MTL::Buffer* rootE = nullptr;
        MTL::Buffer* rootL = nullptr;
        MTL4::ArgumentTable* tableE = nullptr;
        MTL4::ArgumentTable* tableL = nullptr;
        MTL::IndirectCommandBuffer* icb = nullptr;
    };

    MTL4::ArgumentTable* table(uint32_t buffers, const std::string& name) {
        NS::Error* error = nullptr;
        auto* d = MTL4::ArgumentTableDescriptor::alloc()->init();
        d->setInitializeBindings(true);
        d->setMaxBufferBindCount(buffers);
        d->setLabel(str(name));
        auto* t = gpu.device->newArgumentTable(d, &error);
        check(t, name, error);
        d->release();
        return t;
    }

    // Record i draws one point with drawIdOf(i); its firstInstance is non-zero and must not show.
    void fillRecords(Variant v, Resources& r) {
        for (uint32_t i = 0; i < maxCount; ++i) {
            uint32_t id = drawIdOf(i, maxCount);
            uint32_t firstInstance = 1000 + i;
            if (v == Variant::Draw) {
                static_cast<DrawArgs*>(r.records->contents())[i] = {id, 1, 1, 0, firstInstance};
                static_cast<NativeDraw*>(r.floor->contents())[i] = {1, 1, 0, id};
            } else if (v == Variant::Indexed) {
                static_cast<IndexedArgs*>(r.records->contents())[i] = {id, 1, 1, 0, 0,
                                                                       firstInstance};
                static_cast<NativeIndexed*>(r.floor->contents())[i] = {1, 1, 0, 0, id};
            } else {
                static_cast<MeshArgs*>(r.records->contents())[i] = {id, 1, 1, 1};
            }
        }
    }

    Gpu& gpu;
    uint32_t maxCount;
    MTL4::Compiler* compiler = nullptr;
    std::map<std::string, MTL::ComputePipelineState*> kernels;
    MTL::RenderPipelineState* drawPipeline = nullptr;
    MTL::RenderPipelineState* drawPipelineNoIcb = nullptr;
    MTL::RenderPipelineState* meshPipeline = nullptr;
    MTL::Buffer* indices = nullptr;
    MTL::Buffer* countBuffer = nullptr;
    MTL4::ArgumentTable* meshTable = nullptr;
    MTL::Texture* target = nullptr;
    MTL4::RenderPassDescriptor* pass = nullptr;
    std::map<Variant, Resources> resources;
};

int run(const char* metallib, uint32_t maxCount, uint32_t order, bool timing,
        const char* outputPath) {
    check(maxCount && (maxCount & (maxCount - 1)) == 0 &&
              maxCount <= kTargetSize * kTargetSize,
          "MAX_COUNT is a power of two up to 65536");
    Gpu gpu;
    std::string thermalStart = thermal();
    Spike spike(gpu, metallib, maxCount);

    std::ostringstream results;
    bool firstResult = true;
    for (Variant v : kVariants) {
        if (timing && v == Variant::Mesh)
            continue;  // the mesh gate is correctness only
        std::vector<Arm> arms = {Arm::E, Arm::L};
        if (v != Variant::Mesh) {
            arms.push_back(Arm::N);
            arms.push_back(Arm::N0);
        }
        if (order % 2)
            std::reverse(arms.begin(), arms.end());
        for (const Fill& fill : spike.fills(timing)) {
            spike.setFill(fill);
            for (Arm arm : arms) {
                std::string name = std::string("rojorhi.spike3.") + nameOf(v) + "." + nameOf(arm);
                std::ostringstream entry;
                entry << "{\"variant\":\"" << nameOf(v) << "\",\"fill\":\"" << fill.name
                      << "\",\"count\":"
                      << (fill.count ? std::to_string(*fill.count) : std::string("null"))
                      << ",\"drawn\":" << spike.drawn << ",\"arm\":\"" << nameOf(arm) << "\"";
                Spike::Encoded encoded;
                std::string errors;
                std::vector<double> gpuNs, cpuNs;
                const uint32_t frames = timing ? kWarmup + kMeasured : 1;
                for (uint32_t f = 0; f < frames; ++f) {
                    Gpu::Frame frame = gpu.frame(name, [&](MTL4::CommandBuffer* commands) {
                        encoded = spike.encode(commands, v, arm);
                    });
                    if (!frame.error.empty() && errors.empty())
                        errors = frame.error;
                    if (f >= frames - (timing ? kMeasured : 1)) {
                        gpuNs.push_back(frame.gpuNs);
                        cpuNs.push_back(encoded.cpuNs);
                    }
                }
                std::string firstBad;
                size_t bad = spike.mismatches(firstBad);
                entry << ",\"calls\":" << encoded.calls << ",\"mismatches\":" << bad
                      << ",\"firstBad\":\"" << firstBad << "\",\"gpuError\":\"" << errors
                      << "\"";
                if (arm == Arm::E)
                    entry << ",\"range\":" << spike.rangeJson(v);
                if (timing)
                    entry << ",\"gpuMedianNs\":" << median(gpuNs) << ",\"cpuMedianNs\":"
                          << median(cpuNs) << ",\"gpuNs\":" << jsonList(gpuNs)
                          << ",\"cpuNs\":" << jsonList(cpuNs);
                entry << "}";
                results << (firstResult ? "" : ",") << entry.str();
                firstResult = false;
            }
        }
    }

    std::ofstream file(outputPath);
    check(bool(file), std::string("open ") + outputPath);
    file << "{\"maxCount\":" << maxCount << ",\"order\":" << order << ",\"mode\":\""
         << (timing ? "timing" : "correctness") << "\",\"device\":\""
         << gpu.device->name()->utf8String() << "\",\"os\":\""
         << NS::ProcessInfo::processInfo()->operatingSystemVersionString()->utf8String()
         << "\",\"memory\":{" << spike.memoryJson() << "},\"thermalStart\":\"" << thermalStart
         << "\",\"thermalEnd\":\"" << thermal() << "\",\"results\":[" << results.str() << "]}\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    auto* pool = NS::AutoreleasePool::alloc()->init();
    int status = 1;
    try {
        std::string mode = argc > 1 ? argv[1] : "";
        if (mode == "limits" && argc == 3) {
            status = limits(argv[2]);
        } else if (mode == "run" && argc == 7) {
            std::string kind = argv[5];
            check(kind == "correctness" || kind == "timing", "mode is correctness or timing");
            status = run(argv[2], uint32_t(std::stoul(argv[3])), uint32_t(std::stoul(argv[4])),
                         kind == "timing", argv[6]);
        } else {
            std::cerr << "usage: Harness limits METALLIB\n"
                         "       Harness run METALLIB MAX_COUNT ORDER correctness|timing "
                         "OUTPUT_JSON\n";
        }
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n";
        status = 1;
    }
    pool->release();
    return status;
}
