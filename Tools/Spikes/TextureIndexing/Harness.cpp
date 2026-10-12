// Spike 2 host: native Metal 4, no RojoRHI code. Throwaway spike source; see
// docs/research/2026-10-12-spike-2-texture-indexing.md for what it measures and why.
//
//   Harness limits METALLIB
//   Harness run METALLIB COUNT ORDER correctness|timing FIXED_MAX OUTPUT_JSON
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kWidth = 1920;
constexpr uint32_t kHeight = 1080;
constexpr uint32_t kFetches = 8;
constexpr uint32_t kTextureSize = 16;
constexpr uint32_t kWarmup = 50;
constexpr uint32_t kMeasured = 200;
constexpr uint32_t kPoolTarget = 100000;
// Creation without validation accepts more; the validation layer asserts this bound.
constexpr uint32_t kArgumentTableTextureMax = 128;

struct Root {
    uint64_t out;
    uint64_t table;
    uint64_t samplers;
    uint64_t poolBase;
    uint32_t count;
    uint32_t divergent;
    uint32_t width;
    uint32_t pad;
};

NS::String* str(const std::string& s) {
    return NS::String::string(s.c_str(), NS::UTF8StringEncoding);
}

void check(bool ok, const std::string& what, NS::Error* error = nullptr) {
    if (!ok) {
        std::string reason = error ? error->localizedDescription()->utf8String() : "";
        throw std::runtime_error(what + (reason.empty() ? "" : ": " + reason));
    }
}

// Same hash as Kernels.metal.
uint32_t pick(uint32_t key, uint32_t k, uint32_t count) {
    uint32_t h = key * 747796405u + k * 2891336453u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    h = (h >> 22u) ^ h;
    return h % count;
}

uint32_t keyOf(uint32_t x, uint32_t y, bool divergent) {
    return divergent ? y * kWidth + x : y * (kWidth / 32u) + x / 32u;
}

std::vector<uint32_t> expected(uint32_t count, bool divergent) {
    std::vector<uint32_t> out(size_t(kWidth) * kHeight);
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            uint32_t key = keyOf(x, y, divergent), sum = 0;
            for (uint32_t k = 0; k < kFetches; ++k)
                sum += pick(key, k, count);
            out[size_t(y) * kWidth + x] = sum;
        }
    }
    return out;
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
    MTL::SharedEvent* event = nullptr;
    uint64_t eventValue = 0;

    Gpu() {
        device = MTL::CreateSystemDefaultDevice();
        check(device && device->supportsFamily(MTL::GPUFamilyMetal4), "Metal 4 device");
        NS::Error* error = nullptr;
        auto* queueDesc = MTL4::CommandQueueDescriptor::alloc()->init();
        queueDesc->setLabel(str("rojorhi.spike2.queue"));
        queue = device->newMTL4CommandQueue(queueDesc, &error);
        check(queue, "queue", error);
        allocator = device->newCommandAllocator();
        check(allocator, "allocator");
        auto* resDesc = MTL::ResidencySetDescriptor::alloc()->init();
        resDesc->setLabel(str("rojorhi.spike2.residency"));
        residency = device->newResidencySet(resDesc, &error);
        check(residency, "residency set", error);
        queue->addResidencySet(residency);
        event = device->newSharedEvent();
        check(event, "event");
    }

    MTL::Buffer* buffer(size_t bytes, const std::string& name) {
        auto* b = device->newBuffer(bytes, MTL::ResourceStorageModeShared);
        check(b, name);
        b->setLabel(str(name));
        std::memset(b->contents(), 0, bytes);
        residency->addAllocation(b);
        return b;
    }

    // Records one command buffer through `encode`, submits it and waits; throws on GPU error.
    template <class Encode>
    void submit(const std::string& name, Encode encode) {
        residency->commit();
        auto* commands = device->newCommandBuffer();
        check(commands, "command buffer");
        commands->beginCommandBuffer(allocator);
        commands->setLabel(str(name));
        auto* encoder = commands->computeCommandEncoder();
        encoder->setLabel(str(name));
        encode(encoder);
        encoder->endEncoding();
        commands->endCommandBuffer();
        std::mutex mutex;
        std::string failure;
        auto* options = MTL4::CommitOptions::alloc()->init();
        options->addFeedbackHandler([&](MTL4::CommitFeedback* feedback) {
            if (feedback->error()) {
                std::lock_guard lock(mutex);
                failure = feedback->error()->localizedDescription()->utf8String();
            }
        });
        const MTL4::CommandBuffer* list[] = {commands};
        queue->commit(list, 1, options);
        queue->signalEvent(event, ++eventValue);
        check(event->waitUntilSignaledValue(eventValue, 60000), name + " timed out");
        options->release();
        commands->release();
        allocator->reset();
        std::lock_guard lock(mutex);
        check(failure.empty(), name + " failed: " + failure);
    }
};

int limits(const char*) {
    Gpu gpu;
    NS::Error* error = nullptr;
    auto poolFits = [&](NS::UInteger n) {
        auto* d = MTL::ResourceViewPoolDescriptor::alloc()->init();
        d->setResourceViewCount(n);
        auto* p = gpu.device->newTextureViewPool(d, &error);
        d->release();
        if (p)
            p->release();
        return p != nullptr;
    };
    auto largest = [](NS::UInteger hi, auto fits) {
        NS::UInteger lo = 0;
        while (lo < hi) {
            NS::UInteger mid = lo + (hi - lo + 1) / 2;
            if (fits(mid))
                lo = mid;
            else
                hi = mid - 1;
        }
        return lo;
    };
    NS::UInteger poolMax = poolFits(kPoolTarget) ? kPoolTarget : largest(kPoolTarget, poolFits);
    std::cout << "{\"argumentTableTextureMax\":" << kArgumentTableTextureMax << ",\"poolViewMax\":" << poolMax
              << ",\"poolProbeCeiling\":" << kPoolTarget << ",\"device\":\""
              << gpu.device->name()->utf8String() << "\",\"apple9\":"
              << gpu.device->supportsFamily(MTL::GPUFamilyApple9) << ",\"os\":\""
              << NS::ProcessInfo::processInfo()->operatingSystemVersionString()->utf8String()
              << "\",\"timestampHz\":" << gpu.device->queryTimestampFrequency() << "}\n";
    return 0;
}

struct Arm {
    std::string name;    // F, T, P or TS
    std::string kernel;  // readF, sampleT, ...
};

int run(const char* metallib, uint32_t count, uint32_t order, bool timing, uint32_t fixedMax,
        const char* outputPath) {
    Gpu gpu;
    MTL::Device* device = gpu.device;
    NS::Error* error = nullptr;
    std::string thermalStart = thermal();

    auto* library = device->newLibrary(NS::URL::fileURLWithPath(str(metallib)), &error);
    check(library, "library", error);
    auto* compilerDesc = MTL4::CompilerDescriptor::alloc()->init();
    auto* compiler = device->newCompiler(compilerDesc, &error);
    check(compiler, "compiler", error);
    std::map<std::string, MTL::ComputePipelineState*> pipelines;
    for (const char* name :
         {"readF", "sampleF", "readT", "sampleT", "readP", "sampleP", "sampleTS"}) {
        auto* fn = MTL4::LibraryFunctionDescriptor::alloc()->init();
        fn->setLibrary(library);
        fn->setName(str(name));
        auto* d = MTL4::ComputePipelineDescriptor::alloc()->init();
        d->setComputeFunctionDescriptor(fn);
        d->setLabel(str(std::string("rojorhi.spike2.") + name));
        pipelines[name] = compiler->newComputePipelineState(d, nullptr, &error);
        check(pipelines[name], name, error);
    }

    // Textures: texture i holds one color encoding i in its red, green and blue bytes.
    constexpr size_t kTextureBytes = size_t(kTextureSize) * kTextureSize * 4;
    auto* staging = gpu.buffer(size_t(count) * kTextureBytes, "rojorhi.spike2.staging");
    auto* texels = static_cast<uint8_t*>(staging->contents());
    std::vector<MTL::Texture*> textures(count);
    auto* texDesc = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm,
                                                                kTextureSize, kTextureSize, false);
    texDesc->setStorageMode(MTL::StorageModePrivate);
    texDesc->setUsage(MTL::TextureUsageShaderRead);
    for (uint32_t i = 0; i < count; ++i) {
        textures[i] = device->newTexture(texDesc);
        check(textures[i], "texture " + std::to_string(i));
        textures[i]->setLabel(str("rojorhi.spike2.texture." + std::to_string(i)));
        gpu.residency->addAllocation(textures[i]);
        uint8_t color[4] = {uint8_t(i), uint8_t(i >> 8), uint8_t(i >> 16), 255};
        for (size_t t = 0; t < kTextureBytes; t += 4)
            std::memcpy(texels + size_t(i) * kTextureBytes + t, color, 4);
    }
    constexpr uint32_t kUploadBatch = 8192;
    for (uint32_t first = 0; first < count; first += kUploadBatch) {
        gpu.submit("rojorhi.spike2.upload", [&](MTL4::ComputeCommandEncoder* e) {
            for (uint32_t i = first; i < std::min(count, first + kUploadBatch); ++i)
                e->copyFromBuffer(staging, size_t(i) * kTextureBytes, kTextureSize * 4,
                                  kTextureBytes, MTL::Size::Make(kTextureSize, kTextureSize, 1),
                                  textures[i], 0, 0, MTL::Origin::Make(0, 0, 0));
        });
    }

    // Publishing: T writes IDs into a table, P fills a view pool, F binds argument-table slots.
    uint32_t fixedCount = std::min(count, fixedMax);
    bool fixedArm = count <= fixedMax;
    auto* table = gpu.buffer(size_t(count) * sizeof(MTL::ResourceID), "rojorhi.spike2.table");
    auto* tableIds = static_cast<MTL::ResourceID*>(table->contents());
    auto* poolDesc = MTL::ResourceViewPoolDescriptor::alloc()->init();
    poolDesc->setResourceViewCount(count);
    poolDesc->setLabel(str("rojorhi.spike2.pool"));
    auto* pool = device->newTextureViewPool(poolDesc, &error);
    check(pool, "texture view pool", error);
    auto* argDesc = MTL4::ArgumentTableDescriptor::alloc()->init();
    argDesc->setInitializeBindings(true);
    argDesc->setMaxBufferBindCount(1);
    argDesc->setMaxTextureBindCount(fixedMax);
    argDesc->setLabel(str("rojorhi.spike2.arguments"));
    auto* args = device->newArgumentTable(argDesc, &error);
    check(args, "argument table", error);

    std::vector<MTL::ResourceID> poolIds(count);
    auto timePublish = [&](const std::string& arm) {
        auto start = std::chrono::steady_clock::now();
        if (arm == "T") {
            for (uint32_t i = 0; i < count; ++i)
                tableIds[i] = textures[i]->gpuResourceID();
        } else if (arm == "P") {
            for (uint32_t i = 0; i < count; ++i)
                poolIds[i] = pool->setTextureView(textures[i], i);
        } else {
            for (uint32_t i = 0; i < fixedCount; ++i)
                args->setTexture(textures[i]->gpuResourceID(), i);
        }
        auto ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start);
        return ns.count() / (arm == "F" ? fixedCount : count);
    };
    std::vector<std::string> publishOrder = {"F", "T", "P"};
    if (order % 2)
        std::reverse(publishOrder.begin(), publishOrder.end());
    std::map<std::string, double> publishNs;
    for (const auto& arm : publishOrder)
        publishNs[arm] = timePublish(arm);

    uint64_t poolBase = pool->baseResourceID()._impl;
    uint32_t poolNonContiguous = 0;
    for (uint32_t i = 0; i < count; ++i)
        poolNonContiguous += poolIds[i]._impl != poolBase + i;

    auto* samplerDesc = MTL::SamplerDescriptor::alloc()->init();
    samplerDesc->setMinFilter(MTL::SamplerMinMagFilterLinear);
    samplerDesc->setMagFilter(MTL::SamplerMinMagFilterLinear);
    samplerDesc->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
    samplerDesc->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
    samplerDesc->setNormalizedCoordinates(true);
    samplerDesc->setSupportArgumentBuffers(true);
    auto* samplerState = device->newSamplerState(samplerDesc);
    check(samplerState, "sampler");
    auto* samplers = gpu.buffer(sizeof(MTL::ResourceID), "rojorhi.spike2.samplers");
    *static_cast<MTL::ResourceID*>(samplers->contents()) = samplerState->gpuResourceID();

    const size_t elements = size_t(kWidth) * kHeight;
    auto* out = gpu.buffer(elements * sizeof(uint32_t), "rojorhi.spike2.out");
    auto* rootBuffer = gpu.buffer(sizeof(Root), "rojorhi.spike2.root");
    auto* root = static_cast<Root*>(rootBuffer->contents());
    // Negative control: a skewed base must make arm P fail the correctness gate.
    uint64_t rootPoolBase = poolBase + (std::getenv("SPIKE2_POOL_SKEW") ? 1 : 0);
    *root = {out->gpuAddress(), table->gpuAddress(), samplers->gpuAddress(), rootPoolBase, count,
             0, kWidth, 0};
    args->setAddress(rootBuffer->gpuAddress(), 0);

    auto* heapDesc = MTL4::CounterHeapDescriptor::alloc()->init();
    heapDesc->setType(MTL4::CounterHeapTypeTimestamp);
    heapDesc->setCount(2 * kMeasured);
    auto* heap = device->newCounterHeap(heapDesc, &error);
    check(heap, "counter heap", error);
    const double nsPerTick = 1e9 / double(device->queryTimestampFrequency());

    const MTL::Size grid = MTL::Size::Make(kWidth, kHeight, 1);
    const MTL::Size group = MTL::Size::Make(32, 8, 1);
    std::ostringstream results;
    bool firstResult = true;
    for (bool divergent : {false, true}) {
        root->divergent = divergent;
        std::vector<uint32_t> want = expected(count, divergent);
        for (const char* access : {"read", "sample"}) {
            std::vector<Arm> arms;
            if (fixedArm)
                arms.push_back({"F", std::string(access) + "F"});
            arms.push_back({"T", std::string(access) + "T"});
            arms.push_back({"P", std::string(access) + "P"});
            if (std::string(access) == "sample")
                arms.push_back({"TS", "sampleTS"});
            if (order % 2)
                std::reverse(arms.begin(), arms.end());
            for (const Arm& arm : arms) {
                auto* pipeline = pipelines[arm.kernel];
                std::ostringstream entry;
                entry << "{\"pattern\":\"" << (divergent ? "divergent" : "uniform")
                      << "\",\"access\":\"" << access << "\",\"arm\":\"" << arm.name << "\"";
                if (!timing) {
                    std::memset(out->contents(), 0xFF, elements * sizeof(uint32_t));
                    std::string failure;
                    try {
                        gpu.submit("rojorhi.spike2." + arm.kernel,
                                   [&](MTL4::ComputeCommandEncoder* e) {
                                       e->setComputePipelineState(pipeline);
                                       e->setArgumentTable(args);
                                       e->dispatchThreads(grid, group);
                                   });
                    } catch (const std::exception& ex) {
                        failure = ex.what();
                    }
                    auto* got = static_cast<const uint32_t*>(out->contents());
                    size_t mismatches = 0, firstBad = elements;
                    for (size_t i = 0; i < elements; ++i) {
                        if (got[i] != want[i]) {
                            if (!mismatches)
                                firstBad = i;
                            ++mismatches;
                        }
                    }
                    entry << ",\"mismatches\":" << mismatches;
                    if (mismatches)
                        entry << ",\"firstBad\":" << firstBad << ",\"got\":" << got[firstBad]
                              << ",\"want\":" << want[firstBad];
                    entry << ",\"gpuError\":\"" << failure << "\"";
                } else {
                    gpu.submit("rojorhi.spike2." + arm.kernel,
                               [&](MTL4::ComputeCommandEncoder* e) {
                                   e->setComputePipelineState(pipeline);
                                   e->setArgumentTable(args);
                                   for (uint32_t d = 0; d < kWarmup + kMeasured; ++d) {
                                       if (d)
                                           e->barrierAfterEncoderStages(MTL::StageDispatch,
                                                                        MTL::StageDispatch,
                                                                        MTL4::VisibilityOptionNone);
                                       bool measured = d >= kWarmup;
                                       uint32_t m = d - kWarmup;
                                       if (measured)
                                           e->writeTimestamp(MTL4::TimestampGranularityPrecise,
                                                             heap, 2 * m);
                                       e->dispatchThreads(grid, group);
                                       if (measured)
                                           e->writeTimestamp(MTL4::TimestampGranularityPrecise,
                                                             heap, 2 * m + 1);
                                   }
                               });
                    NS::Data* data = heap->resolveCounterRange(NS::Range::Make(0, 2 * kMeasured));
                    check(data && data->length() >= 2 * kMeasured * sizeof(MTL4::TimestampHeapEntry),
                          "resolve timestamps");
                    auto* stamps = static_cast<const MTL4::TimestampHeapEntry*>(data->bytes());
                    std::vector<double> ns(kMeasured);
                    for (uint32_t m = 0; m < kMeasured; ++m)
                        ns[m] = double(stamps[2 * m + 1].timestamp - stamps[2 * m].timestamp) *
                                nsPerTick;
                    entry << ",\"medianNs\":" << median(ns) << ",\"samplesNs\":" << jsonList(ns);
                }
                entry << "}";
                results << (firstResult ? "" : ",") << entry.str();
                firstResult = false;
            }
        }
    }

    std::ofstream file(outputPath);
    check(bool(file), std::string("open ") + outputPath);
    file << "{\"count\":" << count << ",\"order\":" << order << ",\"mode\":\""
         << (timing ? "timing" : "correctness") << "\",\"fixedMax\":" << fixedMax
         << ",\"fixedArm\":" << fixedArm << ",\"device\":\"" << device->name()->utf8String()
         << "\",\"os\":\""
         << NS::ProcessInfo::processInfo()->operatingSystemVersionString()->utf8String()
         << "\",\"poolBase\":" << poolBase << ",\"poolNonContiguous\":" << poolNonContiguous
         << ",\"publishNsPerTexture\":{";
    bool firstPublish = true;
    for (const auto& [arm, ns] : publishNs) {
        file << (firstPublish ? "" : ",") << "\"" << arm << "\":" << ns;
        firstPublish = false;
    }
    file << "},\"thermalStart\":\"" << thermalStart << "\",\"thermalEnd\":\"" << thermal()
         << "\",\"results\":[" << results.str() << "]}\n";
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
        } else if (mode == "run" && argc == 8) {
            std::string kind = argv[5];
            check(kind == "correctness" || kind == "timing", "mode is correctness or timing");
            status = run(argv[2], uint32_t(std::stoul(argv[3])), uint32_t(std::stoul(argv[4])),
                         kind == "timing", uint32_t(std::stoul(argv[6])), argv[7]);
        } else {
            std::cerr << "usage: Harness limits METALLIB\n"
                         "       Harness run METALLIB COUNT ORDER correctness|timing FIXED_MAX "
                         "OUTPUT_JSON\n";
        }
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n";
        status = 1;
    }
    pool->release();
    return status;
}
