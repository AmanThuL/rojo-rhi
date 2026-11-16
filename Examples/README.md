# Examples

Small, self-contained programs that show how RojoRHI is used. Each directory holds its own
`main.cpp` and Slang shaders; `Common/` holds only the GLFW window and the native surface a
swapchain presents to (a `CAMetalLayer` on macOS, an `HWND` on Windows).

| Example | What it shows |
|---|---|
| `01_HelloCompute` | Headless compute: buffers reached by address from a root block, the compute queue joined into the frame by a fence, readback |
| `02_Triangle` | A swapchain loop with no vertex layout and no binding: vertices pulled from per-frame upload memory by `pushRoot` |
| `03_BindlessTextures` | 256 textures drawn in one instanced draw, each instance picking its texture by heap index |
| `04_GpuDrivenCulling` | A compute pass culls 16,384 objects and writes counted indirect draws; each draw finds its object through `drawId`; per-scope GPU timings |
| `05_MeshShaders` | A task shader culls terrain patches and a mesh shader generates their geometry, with no vertex or index buffers |
| `06_AsyncCompute` | A particle simulation on the compute queue overlapping the graphics queue, ordered by two timeline fences |
| `07_Upscaling` | A jittered HDR scene with depth and motion vectors upscaled by the platform's temporal upscaler (MetalFX, FSR), then tone-mapped |

Debug builds run every example under the validation tracker, so a missing or wrong barrier stops
the program with the valid-usage identifier it breaks.

## Building

The examples build on the `metal4` backend (macOS) and the `d3d12` backend (Windows) and fetch
GLFW through xmake. After `xmake setup`:

```
xmake f -m debug -y --rojorhi_examples=y
xmake
xmake run 04_GpuDrivenCulling
```

On Windows, add `--rojorhi_backend=d3d12` to the configure. `07_Upscaling` needs a device that
lists an upscaler: MetalFX on Metal 4, FSR on D3D12 when `xmake setup` fetched FidelityFX.
