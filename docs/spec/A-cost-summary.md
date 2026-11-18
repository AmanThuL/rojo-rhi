# A. Cost summary

**Status**: Accepted
**API version**: 1.0.2
**Entries**: none

This appendix lists every entry of the reference with the cost class its `Cost` row gives on each
backend, and the capability bit that gates the entry or one of its options through a valid-usage
rule ([device](02-device.md#capabilities)), or none; `Tools/check_spec.py` fails a missing, extra or
disagreeing row. It defines nothing: each row links to the entry that owns it, and that entry's
`Cost` row and note are the authority. The classes are the four of
[conventions](00-conventions.md#mapping-tables-and-cost-classes), `native`, `cheap`, `costly` and
`impossible`, each the cost of the entry over the best hand-written native code for the same work.
The entry's note says whether the CPU or the GPU pays. The D3D12 column maps the D3D12 backend,
which is implemented and conformant (T1) on one GPU, an NVIDIA GeForce RTX 3080 Ti on driver 621.14,
with its overhead budgets not yet met and other vendors unverified (ADR
0031); on that GPU its first overhead round met
the GPU, allocation, pipeline and native-call budgets and missed the CPU budget in 19 of 32 judged
configurations (the results), and the D3D12
overhead work stays open until a round meets the budget. Round 1 timed ten workloads, not every
entry, so each D3D12 class remains its entry's Cost note. The Vulkan column is a design oracle and
not a capability RojoRHI has ([conventions](00-conventions.md#scope-and-backends)).

Read the table for its `costly` and `impossible` cells: they are the places where the design pays
for one interface over three APIs, by emulating, tracking or giving an entry up on a backend. Every
other cell is the native call or a fixed, small amount of backend work. In this API version no
entry is `impossible` on any backend.

Spike 1 of ADR 0014 measured `Addr` dereference on
D3D12 as costly on an RTX 3080 Ti, leaving the rows of chapters 03 and 05 `costly` (the
result). Spike 2 kept the Metal 4
ID table for texture and sampler indexing, leaving its rows `cheap`. Spike 3 kept the kernel-encoded
indirect command buffer for counted multi-draw on Metal 4, leaving its rows `costly`; ADR
0028 moved the vertex stage's `drawId`
from the base instance to the bound record, making a lone record with a null count native and the
two record rows `cheap`. A change to an entry's `Cost` row updates its row here in the same change.

| Entry | Chapter | Metal 4 | D3D12 | Vulkan | Gated by |
|---|---|---|---|---|---|
| [`Result`](01-fundamentals.md#result) | 01 | native | native | native | none |
| [`Error`](01-fundamentals.md#error) | 01 | native | native | native | none |
| [`ErrorCategory`](01-fundamentals.md#errorcategory) | 01 | native | native | native | none |
| [`ResourceHandle`](01-fundamentals.md#resourcehandle) | 01 | cheap | cheap | cheap | none |
| [`ObjectHandle`](01-fundamentals.md#objecthandle) | 01 | native | native | native | none |
| [`Extent2D`](01-fundamentals.md#extent2d) | 01 | native | native | native | none |
| [`Extent3D`](01-fundamentals.md#extent3d) | 01 | native | native | native | none |
| [`Offset3D`](01-fundamentals.md#offset3d) | 01 | native | native | native | none |
| [`Rect2D`](01-fundamentals.md#rect2d) | 01 | native | native | native | none |
| [`Label`](01-fundamentals-part2.md#label) | 01 | native | native | native | none |
| [`MessageCallback`](01-fundamentals-part2.md#messagecallback) | 01 | native | native | native | none |
| [`setMessageCallback`](01-fundamentals-part2.md#setmessagecallback) | 01 | native | native | native | none |
| [`ROJORHI_VERSION_*`](01-fundamentals-part2.md#rojorhi_version_) | 01 | native | native | native | none |
| [`DeviceDesc`](02-device.md#devicedesc) | 02 | costly | costly | costly | none |
| [`createDevice`](02-device.md#createdevice) | 02 | native | native | native | none |
| [`destroyDevice`](02-device.md#destroydevice) | 02 | native | native | native | none |
| [`Capabilities`](02-device.md#capabilities) | 02 | native | native | native | none |
| [`Device::capabilities`](02-device.md#devicecapabilities) | 02 | native | native | native | none |
| [`Device::beginFrame`](02-device.md#devicebeginframe) | 02 | native | native | native | none |
| [`Device::endFrame`](02-device.md#deviceendframe) | 02 | native | native | native | none |
| [`Device::destroy`](02-device.md#devicedestroy) | 02 | cheap | cheap | cheap | none |
| [`Memory`](03-memory-and-buffers.md#memory) | 03 | native | cheap | cheap | none |
| [`BufferUsage`](03-memory-and-buffers.md#bufferusage) | 03 | native | native | native | `placementSparse` |
| [`BufferDesc`](03-memory-and-buffers.md#bufferdesc) | 03 | native | native | native | none |
| [`Device::createBuffer`](03-memory-and-buffers.md#devicecreatebuffer) | 03 | native | cheap | native | none |
| [`Addr`](03-memory-and-buffers.md#addr) | 03 | native | costly | native | none |
| [`Device::address`](03-memory-and-buffers.md#deviceaddress) | 03 | native | cheap | native | none |
| [`Device::cpuPointer`](03-memory-and-buffers.md#devicecpupointer) | 03 | native | native | native | none |
| [`CommandList::uploadFrameData`](03-memory-and-buffers.md#commandlistuploadframedata) | 03 | cheap | cheap | cheap | none |
| [`HeapDesc`](03-memory-and-buffers-part2.md#heapdesc) | 03 | native | native | native | none |
| [`Device::createHeap`](03-memory-and-buffers-part2.md#devicecreateheap) | 03 | native | native | native | none |
| [`Device::placementRequirements`](03-memory-and-buffers-part2.md#deviceplacementrequirements) | 03 | native | native | native | none |
| [`Placement`](03-memory-and-buffers-part2.md#placement) | 03 | native | native | native | none |
| [`TextureType`](04-textures-and-heaps.md#texturetype) | 04 | native | native | native | none |
| [`TextureUsage`](04-textures-and-heaps.md#textureusage) | 04 | native | native | native | none |
| [`TextureDesc`](04-textures-and-heaps.md#texturedesc) | 04 | native | native | native | `placementSparse` |
| [`TextureData`](04-textures-and-heaps.md#texturedata) | 04 | native | cheap | native | none |
| [`SubresourceRange`](04-textures-and-heaps.md#subresourcerange) | 04 | native | native | native | none |
| [`Device::createTexture`](04-textures-and-heaps.md#devicecreatetexture) | 04 | native | native | native | none |
| [`TextureViewDesc`](04-textures-and-heaps.md#textureviewdesc) | 04 | native | native | native | none |
| [`Device::createView`](04-textures-and-heaps.md#devicecreateview) | 04 | cheap | native | native | none |
| [`ViewIndex`](04-textures-and-heaps.md#viewindex) | 04 | cheap | native | native | none |
| [`SamplerDesc`](04-textures-and-heaps-part2.md#samplerdesc) | 04 | native | native | native | none |
| [`SamplerIndex`](04-textures-and-heaps-part2.md#samplerindex) | 04 | cheap | native | native | none |
| [`Device::createSampler`](04-textures-and-heaps-part2.md#devicecreatesampler) | 04 | cheap | native | native | none |
| [`IndexRange`](04-textures-and-heaps-part2.md#indexrange) | 04 | cheap | native | native | none |
| [`Device::reserveViews`](04-textures-and-heaps-part2.md#devicereserveviews) | 04 | cheap | native | native | none |
| [`Device::writeView`](04-textures-and-heaps-part2.md#devicewriteview) | 04 | cheap | native | native | none |
| [`Device::reserveSamplers`](04-textures-and-heaps-part2.md#devicereservesamplers) | 04 | cheap | native | native | none |
| [`Device::writeSampler`](04-textures-and-heaps-part2.md#devicewritesampler) | 04 | cheap | native | native | none |
| [`Device::release`](04-textures-and-heaps-part2.md#devicerelease) | 04 | cheap | native | native | none |
| [`CommandList::setRoot`](05-shader-abi.md#commandlistsetroot) | 05 | native | cheap | native | none |
| [`CommandList::pushRoot`](05-shader-abi.md#commandlistpushroot) | 05 | cheap | cheap | cheap | none |
| [`rojoRHI::root`](05-shader-abi.md#rojorhiroot) | 05 | native | native | native | none |
| [`ROJORHI_SHADER_ABI_VERSION`](05-shader-abi.md#rojorhi_shader_abi_version) | 05 | native | native | native | none |
| [`rojoRHI::nonUniform`](05-shader-abi-part2.md#rojorhinonuniform) | 05 | native | native | native | none |
| [`rojoRHI::load`](05-shader-abi-part2.md#rojorhiload) | 05 | native | costly | native | none |
| [`rojoRHI::store`](05-shader-abi-part2.md#rojorhistore) | 05 | native | costly | native | none |
| [`rojoRHI::atomicAdd`](05-shader-abi-part2.md#rojorhiatomicadd) | 05 | native | costly | native | none |
| [`rojoRHI::texture`](05-shader-abi-part2.md#rojorhitexture) | 05 | cheap | native | cheap | none |
| [`rojoRHI::wTexture`](05-shader-abi-part2.md#rojorhiwtexture) | 05 | cheap | native | cheap | none |
| [`rojoRHI::sampler`](05-shader-abi-part2.md#rojorhisampler) | 05 | cheap | native | cheap | none |
| [`rojoRHI::VertexIds`](05-shader-abi-part3.md#rojorhivertexids) | 05 | native | native | native | none |
| [`rojoRHI::vertexIndex`](05-shader-abi-part3.md#rojorhivertexindex) | 05 | cheap | native | cheap | none |
| [`rojoRHI::instanceIndex`](05-shader-abi-part3.md#rojorhiinstanceindex) | 05 | cheap | native | cheap | none |
| [`rojoRHI::drawId`](05-shader-abi-part3.md#rojorhidrawid) | 05 | cheap | native | cheap | none |
| [`ShaderEntry`](06-pipelines.md#shaderentry) | 06 | native | native | native | none |
| [`Device::loadShaderLibrary`](06-pipelines.md#deviceloadshaderlibrary) | 06 | native | cheap | native | none |
| [`BlendState`](06-pipelines.md#blendstate) | 06 | native | native | native | none |
| [`ColorTarget`](06-pipelines.md#colortarget) | 06 | native | native | native | none |
| [`RasterState`](06-pipelines.md#rasterstate) | 06 | cheap | native | native | `conservativeRaster` |
| [`CompareOp`](06-pipelines.md#compareop) | 06 | native | native | native | none |
| [`DepthStencilState`](06-pipelines.md#depthstencilstate) | 06 | cheap | native | native | none |
| [`PrimitiveClass`](06-pipelines.md#primitiveclass) | 06 | native | native | native | none |
| [`GraphicsPipelineDesc`](06-pipelines.md#graphicspipelinedesc) | 06 | native | native | native | none |
| [`ComputePipelineDesc`](06-pipelines-part2.md#computepipelinedesc) | 06 | native | native | native | none |
| [`Device::createPipeline`](06-pipelines-part2.md#devicecreatepipeline) | 06 | native | native | native | none |
| [`PipelineCache`](06-pipelines-part2.md#pipelinecache) | 06 | cheap | cheap | native | none |
| [`Device::pipelineCacheData`](06-pipelines-part2.md#devicepipelinecachedata) | 06 | cheap | cheap | native | none |
| [`CommandList::setViewport`](06-pipelines-part2.md#commandlistsetviewport) | 06 | native | native | native | none |
| [`CommandList::setScissor`](06-pipelines-part2.md#commandlistsetscissor) | 06 | native | native | native | none |
| [`CommandList::setStencilReference`](06-pipelines-part2.md#commandlistsetstencilreference) | 06 | native | native | native | none |
| [`CommandList::setBlendConstants`](06-pipelines-part2.md#commandlistsetblendconstants) | 06 | native | native | native | none |
| [`CommandList::setPrimitiveTopology`](06-pipelines-part2.md#commandlistsetprimitivetopology) | 06 | native | native | native | none |
| [`CommandList::setDepthBias`](06-pipelines-part2.md#commandlistsetdepthbias) | 06 | cheap | cheap | native | none |
| [`QueueType`](07-command-recording.md#queuetype) | 07 | native | native | native | none |
| [`Device::acquireCommandList`](07-command-recording.md#deviceacquirecommandlist) | 07 | native | native | native | none |
| [`CommandList::end`](07-command-recording.md#commandlistend) | 07 | native | native | native | none |
| [`LoadOp`](07-command-recording.md#loadop) | 07 | native | native | native | none |
| [`StoreOp`](07-command-recording.md#storeop) | 07 | native | native | native | none |
| [`ColorAttachment`](07-command-recording.md#colorattachment) | 07 | native | cheap | cheap | none |
| [`DepthAttachment`](07-command-recording.md#depthattachment) | 07 | native | cheap | cheap | none |
| [`RenderPassDesc`](07-command-recording.md#renderpassdesc) | 07 | native | native | native | none |
| [`CommandList::beginRenderPass`](07-command-recording.md#commandlistbeginrenderpass) | 07 | native | cheap | native | none |
| [`CommandList::endRenderPass`](07-command-recording.md#commandlistendrenderpass) | 07 | native | cheap | native | none |
| [`CommandList::setPipeline`](07-command-recording-part2.md#commandlistsetpipeline) | 07 | cheap | native | native | none |
| [`CommandList::draw`](07-command-recording-part2.md#commandlistdraw) | 07 | native | native | native | none |
| [`IndexType`](07-command-recording-part2.md#indextype) | 07 | native | native | native | none |
| [`CommandList::drawIndexed`](07-command-recording-part2.md#commandlistdrawindexed) | 07 | native | native | native | none |
| [`CommandList::drawMeshTasks`](07-command-recording-part2.md#commandlistdrawmeshtasks) | 07 | native | native | native | none |
| [`CommandList::dispatch`](07-command-recording-part2.md#commandlistdispatch) | 07 | native | native | native | none |
| [`CommandList::beginScope`](07-command-recording-part2.md#commandlistbeginscope) | 07 | cheap | cheap | cheap | none |
| [`CommandList::endScope`](07-command-recording-part2.md#commandlistendscope) | 07 | cheap | cheap | cheap | none |
| [`BufferTextureLayout`](07-command-recording-part3.md#buffertexturelayout) | 07 | native | native | native | none |
| [`TextureRegion`](07-command-recording-part3.md#textureregion) | 07 | native | native | native | none |
| [`CommandList::copyBuffer`](07-command-recording-part3.md#commandlistcopybuffer) | 07 | native | native | native | none |
| [`CommandList::copyBufferToTexture`](07-command-recording-part3.md#commandlistcopybuffertotexture) | 07 | native | native | native | none |
| [`CommandList::copyTextureToBuffer`](07-command-recording-part3.md#commandlistcopytexturetobuffer) | 07 | native | native | native | none |
| [`CommandList::copyTexture`](07-command-recording-part3.md#commandlistcopytexture) | 07 | native | native | native | none |
| [`CommandList::fillBuffer`](07-command-recording-part3.md#commandlistfillbuffer) | 07 | cheap | costly | native | none |
| [`ClearValue`](07-command-recording-part3.md#clearvalue) | 07 | native | native | native | none |
| [`CommandList::clearTexture`](07-command-recording-part3.md#commandlistcleartexture) | 07 | costly | costly | native | none |
| [`DrawIndirectArgs`](08-gpu-driven.md#drawindirectargs) | 08 | cheap | native | cheap | none |
| [`DrawIndexedIndirectArgs`](08-gpu-driven.md#drawindexedindirectargs) | 08 | cheap | native | cheap | none |
| [`DrawMeshTasksIndirectArgs`](08-gpu-driven.md#drawmeshtasksindirectargs) | 08 | cheap | native | cheap | none |
| [`DispatchIndirectArgs`](08-gpu-driven.md#dispatchindirectargs) | 08 | native | native | native | none |
| [`CommandList::drawIndirect`](08-gpu-driven.md#commandlistdrawindirect) | 08 | costly | cheap | native | none |
| [`CommandList::drawIndexedIndirect`](08-gpu-driven.md#commandlistdrawindexedindirect) | 08 | costly | cheap | native | none |
| [`CommandList::drawMeshTasksIndirect`](08-gpu-driven.md#commandlistdrawmeshtasksindirect) | 08 | costly | cheap | native | `meshTasksIndirect` |
| [`CommandList::dispatchIndirect`](08-gpu-driven.md#commandlistdispatchindirect) | 08 | native | cheap | native | none |
| [`Stage`](09-synchronization.md#stage) | 09 | native | cheap | native | none |
| [`Access`](09-synchronization.md#access) | 09 | native | native | native | none |
| [`Layout`](09-synchronization.md#layout) | 09 | native | cheap | native | none |
| [`GlobalBarrier`](09-synchronization-part2.md#globalbarrier) | 09 | native | native | native | none |
| [`BufferBarrier`](09-synchronization-part2.md#bufferbarrier) | 09 | native | native | native | none |
| [`TextureBarrier`](09-synchronization-part2.md#texturebarrier) | 09 | native | native | native | none |
| [`CommandList::barrier`](09-synchronization-part2.md#commandlistbarrier) | 09 | native | native | native | none |
| [`Device::queue`](10-queues-and-frames.md#devicequeue) | 10 | native | native | native | none |
| [`FenceValue`](10-queues-and-frames.md#fencevalue) | 10 | native | native | native | none |
| [`Device::createFence`](10-queues-and-frames.md#devicecreatefence) | 10 | native | native | native | none |
| [`Queue::submit`](10-queues-and-frames.md#queuesubmit) | 10 | native | native | native | none |
| [`Device::completedValue`](10-queues-and-frames.md#devicecompletedvalue) | 10 | native | native | native | none |
| [`Device::wait`](10-queues-and-frames.md#devicewait) | 10 | native | native | native | none |
| [`ColorSpace`](11-presentation.md#colorspace) | 11 | native | native | native | none |
| [`PresentMode`](11-presentation.md#presentmode) | 11 | native | native | native | none |
| [`SwapchainDesc`](11-presentation.md#swapchaindesc) | 11 | cheap | native | cheap | none |
| [`Device::createSwapchain`](11-presentation.md#devicecreateswapchain) | 11 | native | native | native | none |
| [`Swapchain::resize`](11-presentation.md#swapchainresize) | 11 | native | native | native | none |
| [`Swapchain::acquire`](11-presentation.md#swapchainacquire) | 11 | cheap | native | native | none |
| [`Queue::present`](11-presentation.md#queuepresent) | 11 | native | native | native | none |
| [`UpscalerKind`](12-upscaling.md#upscalerkind) | 12 | native | native | native | none |
| [`UpscalerSupport`](12-upscaling.md#upscalersupport) | 12 | native | native | native | none |
| [`UpscalerFlags`](12-upscaling.md#upscalerflags) | 12 | native | native | native | none |
| [`UpscalerDesc`](12-upscaling.md#upscalerdesc) | 12 | native | native | native | none |
| [`Device::createUpscaler`](12-upscaling.md#devicecreateupscaler) | 12 | native | cheap | cheap | none |
| [`UpscaleParams`](12-upscaling.md#upscaleparams) | 12 | native | native | native | none |
| [`CommandList::upscale`](12-upscaling.md#commandlistupscale) | 12 | cheap | cheap | cheap | none |
| [`TileShape`](13-sparse.md#tileshape) | 13 | native | native | native | none |
| [`Device::tileShape`](13-sparse.md#devicetileshape) | 13 | native | native | native | `placementSparse` |
| [`TileMapping`](13-sparse.md#tilemapping) | 13 | native | native | native | `placementSparse` |
| [`Queue::mapTiles`](13-sparse.md#queuemaptiles) | 13 | cheap | native | native | `placementSparse` |
| [`ResourceAccess`](14-diagnostics.md#resourceaccess) | 14 | native | native | native | none |
| [`CommandList::declareAccess`](14-diagnostics.md#commandlistdeclareaccess) | 14 | cheap | cheap | cheap | none |
| [`Counters`](14-diagnostics.md#counters) | 14 | cheap | cheap | cheap | none |
| [`Device::counters`](14-diagnostics.md#devicecounters) | 14 | cheap | cheap | cheap | none |
| [`ScopeTiming`](14-diagnostics.md#scopetiming) | 14 | cheap | native | native | none |
| [`Device::scopeTimings`](14-diagnostics.md#devicescopetimings) | 14 | native | native | native | none |
| [`Device::beginCapture`](14-diagnostics.md#devicebegincapture) | 14 | native | native | native | none |
| [`Device::endCapture`](14-diagnostics.md#deviceendcapture) | 14 | native | native | native | none |
| [`Format`](15-formats-and-limits.md#format) | 15 | native | native | native | none |
| [`FormatCapabilities`](15-formats-and-limits.md#formatcapabilities) | 15 | cheap | native | native | none |
| [`Device::formatCapabilities`](15-formats-and-limits.md#deviceformatcapabilities) | 15 | cheap | native | native | none |
| [`Limits`](15-formats-and-limits-part2.md#limits-1) | 15 | cheap | native | native | none |

"Gated by" names the bit a valid-usage rule requires. `BufferUsage` and `TextureDesc` are gated
because their rules admit the `Sparse` usage only with `placementSparse`; `TextureUsage`, which
declares the flag and has no such rule, reads `none`. The sparse operations `Device::tileShape`,
`TileMapping` and `Queue::mapTiles` are gated through the resource's `Sparse` usage, which their
rules require, rather than by a rule naming the bit.
