// Spike 2 kernels: one body, three ways to reach texture i. Throwaway spike source.
#include <metal_stdlib>
using namespace metal;

#ifndef FIXED_MAX
#define FIXED_MAX 128
#endif

constant uint kFetches = 8;

struct TexHandle {
    texture2d<float> t;
};

struct SamplerHandle {
    sampler s;
};

struct Root {
    device uint* out;
    constant TexHandle* table;
    constant SamplerHandle* samplers;
    ulong poolBase;
    uint count;
    uint divergent;
    uint width;
};

constexpr sampler kLinear(filter::linear, address::clamp_to_edge, coord::normalized);

// The host repeats this hash to compute the expected output.
static uint pick(uint key, uint k, uint count) {
    uint h = key * 747796405u + k * 2891336453u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    h = (h >> 22u) ^ h;
    return h % count;
}

// Uniform: the key is constant across a 32-wide row of a (32, 8) threadgroup, one SIMD group.
static uint keyOf(uint2 gid, constant Root& r) {
    return r.divergent != 0 ? gid.y * r.width + gid.x : gid.y * (r.width / 32u) + gid.x / 32u;
}

static uint decode(float4 v) {
    uint4 c = uint4(round(v * 255.0f));
    return c.r | (c.g << 8) | (c.b << 16);
}

static float4 readAt(texture2d<float> t, uint k) {
    return t.read(uint2(k & 15u, (k * 5u) & 15u));
}

static float4 sampleAt(texture2d<float> t, sampler s, uint k) {
    return t.sample(s, float2(0.37f + 0.05f * float(k), 0.61f), level(0.0f));
}

// The pool's handle for slot i is formed by punning base + i into a handle struct; MSL rejects
// as_type to a texture and has no integer constructor for MTLResourceID.
static texture2d<float> poolTexture(constant Root& r, uint i) {
    ulong id = r.poolBase + i;
    thread TexHandle* h = reinterpret_cast<thread TexHandle*>(&id);
    return h->t;
}

#define SPIKE_KERNEL(name, EXTRA, TEXTURE, FETCH)                                                \
    kernel void name(constant Root& r [[buffer(0)]] EXTRA, uint2 gid [[thread_position_in_grid]]) \
    {                                                                                            \
        uint key = keyOf(gid, r);                                                                \
        uint sum = 0;                                                                            \
        for (uint k = 0; k < kFetches; ++k) {                                                    \
            uint i = pick(key, k, r.count);                                                      \
            sum += decode(FETCH(TEXTURE, k));                                                    \
        }                                                                                        \
        r.out[gid.y * r.width + gid.x] = sum;                                                    \
    }

#define FIXED_ARG , array<texture2d<float>, FIXED_MAX> fixed [[texture(0)]]
#define READ(t, k) readAt(t, k)
#define SAMPLE(t, k) sampleAt(t, kLinear, k)
#define SAMPLE_TABLE(t, k) sampleAt(t, r.samplers[0].s, k)

SPIKE_KERNEL(readF, FIXED_ARG, fixed[i], READ)
SPIKE_KERNEL(sampleF, FIXED_ARG, fixed[i], SAMPLE)
SPIKE_KERNEL(readT, , r.table[i].t, READ)
SPIKE_KERNEL(sampleT, , r.table[i].t, SAMPLE)
SPIKE_KERNEL(readP, , poolTexture(r, i), READ)
SPIKE_KERNEL(sampleP, , poolTexture(r, i), SAMPLE)
SPIKE_KERNEL(sampleTS, , r.table[i].t, SAMPLE_TABLE)
