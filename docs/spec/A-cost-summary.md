# A. Cost summary

**Status**: Accepted
**API version**: 0.1.0
**Entries**: none

This appendix lists every entry of the reference with the cost class its `Cost` row gives on each
backend. It defines nothing: each row links to the entry that owns it, and that entry's `Cost` row
and note are the authority. The classes are the four of
[conventions](00-conventions.md#mapping-tables-and-cost-classes), `native`, `cheap`, `costly` and
`impossible`, each the cost of the entry over the best hand-written native code for the same work.
The entry's note says whether the CPU or the GPU pays. The D3D12 column describes the designed
mapping of a planned backend and the Vulkan column a design oracle; neither is a capability
RojoRHI has ([conventions](00-conventions.md#scope-and-backends)).

Read the table for its `costly` and `impossible` cells: they are the places where the design pays
for one interface over three APIs, by emulating, tracking or giving an entry up on a backend. Every
other cell is the native call or a fixed, small amount of backend work. In this API version no
entry is `impossible` on any backend.

Spike 1 of [ADR 0014](../decisions/0014-overhead-measurement.md) is pending and may change rows in
chapters 03 and 05, which stay `Proposed` until it reports: it measures `Addr` dereference on
D3D12. Spike 2 kept the Metal 4 ID table for texture and sampler indexing, leaving its rows
`cheap`. Spike 3 kept the kernel-encoded indirect command buffer for counted multi-draw on Metal 4,
leaving its rows `costly`, and moved `drawId` into the base instance, making two record rows
`native`. A change to an entry's `Cost` row updates its row here in the same change.

| Entry | Chapter | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|---|
| [`Result`](01-fundamentals.md#result) | 01 | native | native | native |
| [`Error`](01-fundamentals.md#error) | 01 | native | native | native |
| [`ErrorCategory`](01-fundamentals.md#errorcategory) | 01 | native | native | native |
| [`ResourceHandle`](01-fundamentals.md#resourcehandle) | 01 | cheap | cheap | cheap |
| [`ObjectHandle`](01-fundamentals.md#objecthandle) | 01 | native | native | native |
| [`Extent2D`](01-fundamentals.md#extent2d) | 01 | native | native | native |
| [`Extent3D`](01-fundamentals.md#extent3d) | 01 | native | native | native |
| [`Offset3D`](01-fundamentals.md#offset3d) | 01 | native | native | native |
| [`Rect2D`](01-fundamentals.md#rect2d) | 01 | native | native | native |
| [`Label`](01-fundamentals-part2.md#label) | 01 | native | native | native |
| [`MessageCallback`](01-fundamentals-part2.md#messagecallback) | 01 | native | native | native |
| [`setMessageCallback`](01-fundamentals-part2.md#setmessagecallback) | 01 | native | native | native |
| [`ROJORHI_VERSION_*`](01-fundamentals-part2.md#rojorhi_version_) | 01 | native | native | native |
| [`DeviceDesc`](02-device.md#devicedesc) | 02 | costly | costly | costly |
| [`createDevice`](02-device.md#createdevice) | 02 | native | native | native |
| [`destroyDevice`](02-device.md#destroydevice) | 02 | native | native | native |
| [`Capabilities`](02-device.md#capabilities) | 02 | native | native | native |
| [`Device::capabilities`](02-device.md#devicecapabilities) | 02 | native | native | native |
| [`Device::beginFrame`](02-device.md#devicebeginframe) | 02 | native | native | native |
| [`Device::endFrame`](02-device.md#deviceendframe) | 02 | native | native | native |
| [`Device::destroy`](02-device.md#devicedestroy) | 02 | cheap | cheap | cheap |
| [`Memory`](03-memory-and-buffers.md#memory) | 03 | native | cheap | cheap |
| [`BufferUsage`](03-memory-and-buffers.md#bufferusage) | 03 | native | native | native |
| [`BufferDesc`](03-memory-and-buffers.md#bufferdesc) | 03 | native | native | native |
| [`Device::createBuffer`](03-memory-and-buffers.md#devicecreatebuffer) | 03 | native | cheap | native |
| [`Addr`](03-memory-and-buffers.md#addr) | 03 | native | costly | native |
| [`Device::address`](03-memory-and-buffers.md#deviceaddress) | 03 | native | cheap | native |
| [`Device::cpuPointer`](03-memory-and-buffers.md#devicecpupointer) | 03 | native | native | native |
| [`CommandList::uploadFrameData`](03-memory-and-buffers.md#commandlistuploadframedata) | 03 | cheap | cheap | cheap |
| [`HeapDesc`](03-memory-and-buffers-part2.md#heapdesc) | 03 | native | native | native |
| [`Device::createHeap`](03-memory-and-buffers-part2.md#devicecreateheap) | 03 | native | native | native |
| [`Device::placementRequirements`](03-memory-and-buffers-part2.md#deviceplacementrequirements) | 03 | native | native | native |
| [`Placement`](03-memory-and-buffers-part2.md#placement) | 03 | native | native | native |
| [`TextureType`](04-textures-and-heaps.md#texturetype) | 04 | native | native | native |
| [`TextureUsage`](04-textures-and-heaps.md#textureusage) | 04 | native | native | native |
| [`TextureDesc`](04-textures-and-heaps.md#texturedesc) | 04 | native | native | native |
| [`TextureData`](04-textures-and-heaps.md#texturedata) | 04 | native | cheap | native |
| [`SubresourceRange`](04-textures-and-heaps.md#subresourcerange) | 04 | native | native | native |
| [`Device::createTexture`](04-textures-and-heaps.md#devicecreatetexture) | 04 | native | native | native |
| [`TextureViewDesc`](04-textures-and-heaps.md#textureviewdesc) | 04 | native | native | native |
| [`Device::createView`](04-textures-and-heaps.md#devicecreateview) | 04 | cheap | native | native |
| [`ViewIndex`](04-textures-and-heaps.md#viewindex) | 04 | cheap | native | native |
| [`SamplerDesc`](04-textures-and-heaps-part2.md#samplerdesc) | 04 | native | native | native |
| [`SamplerIndex`](04-textures-and-heaps-part2.md#samplerindex) | 04 | cheap | native | native |
| [`Device::createSampler`](04-textures-and-heaps-part2.md#devicecreatesampler) | 04 | cheap | native | native |
| [`IndexRange`](04-textures-and-heaps-part2.md#indexrange) | 04 | cheap | native | native |
| [`Device::reserveViews`](04-textures-and-heaps-part2.md#devicereserveviews) | 04 | cheap | native | native |
| [`Device::writeView`](04-textures-and-heaps-part2.md#devicewriteview) | 04 | cheap | native | native |
| [`Device::reserveSamplers`](04-textures-and-heaps-part2.md#devicereservesamplers) | 04 | cheap | native | native |
| [`Device::writeSampler`](04-textures-and-heaps-part2.md#devicewritesampler) | 04 | cheap | native | native |
| [`Device::release`](04-textures-and-heaps-part2.md#devicerelease) | 04 | cheap | native | native |
| [`CommandList::setRoot`](05-shader-abi.md#commandlistsetroot) | 05 | native | cheap | native |
| [`CommandList::pushRoot`](05-shader-abi.md#commandlistpushroot) | 05 | cheap | cheap | cheap |
| [`rojoRHI::root`](05-shader-abi.md#rojorhiroot) | 05 | native | native | native |
| [`ROJORHI_SHADER_ABI_VERSION`](05-shader-abi.md#rojorhi_shader_abi_version) | 05 | native | native | native |
| [`rojoRHI::nonUniform`](05-shader-abi-part2.md#rojorhinonuniform) | 05 | native | native | native |
| [`rojoRHI::load`](05-shader-abi-part2.md#rojorhiload) | 05 | native | costly | native |
| [`rojoRHI::store`](05-shader-abi-part2.md#rojorhistore) | 05 | native | costly | native |
| [`rojoRHI::texture`](05-shader-abi-part2.md#rojorhitexture) | 05 | cheap | native | cheap |
| [`rojoRHI::sampler`](05-shader-abi-part2.md#rojorhisampler) | 05 | cheap | native | cheap |
| [`rojoRHI::VertexIds`](05-shader-abi-part3.md#rojorhivertexids) | 05 | native | native | native |
| [`rojoRHI::vertexIndex`](05-shader-abi-part3.md#rojorhivertexindex) | 05 | native | native | native |
| [`rojoRHI::instanceIndex`](05-shader-abi-part3.md#rojorhiinstanceindex) | 05 | cheap | native | cheap |
| [`rojoRHI::drawId`](05-shader-abi-part3.md#rojorhidrawid) | 05 | cheap | native | cheap |
| [`ShaderEntry`](06-pipelines.md#shaderentry) | 06 | native | native | native |
| [`Device::loadShaderLibrary`](06-pipelines.md#deviceloadshaderlibrary) | 06 | native | cheap | native |
| [`BlendState`](06-pipelines.md#blendstate) | 06 | native | native | native |
| [`ColorTarget`](06-pipelines.md#colortarget) | 06 | native | native | native |
| [`RasterState`](06-pipelines.md#rasterstate) | 06 | cheap | native | native |
| [`CompareOp`](06-pipelines.md#compareop) | 06 | native | native | native |
| [`DepthStencilState`](06-pipelines.md#depthstencilstate) | 06 | cheap | native | native |
| [`PrimitiveClass`](06-pipelines.md#primitiveclass) | 06 | native | native | native |
| [`GraphicsPipelineDesc`](06-pipelines.md#graphicspipelinedesc) | 06 | native | native | native |
| [`ComputePipelineDesc`](06-pipelines-part2.md#computepipelinedesc) | 06 | native | native | native |
| [`Device::createPipeline`](06-pipelines-part2.md#devicecreatepipeline) | 06 | native | native | native |
| [`PipelineCache`](06-pipelines-part2.md#pipelinecache) | 06 | cheap | native | native |
| [`Device::pipelineCacheData`](06-pipelines-part2.md#devicepipelinecachedata) | 06 | cheap | native | native |
| [`CommandList::setViewport`](06-pipelines-part2.md#commandlistsetviewport) | 06 | native | native | native |
| [`CommandList::setScissor`](06-pipelines-part2.md#commandlistsetscissor) | 06 | native | native | native |
| [`CommandList::setStencilReference`](06-pipelines-part2.md#commandlistsetstencilreference) | 06 | native | native | native |
| [`CommandList::setBlendConstants`](06-pipelines-part2.md#commandlistsetblendconstants) | 06 | native | native | native |
| [`CommandList::setPrimitiveTopology`](06-pipelines-part2.md#commandlistsetprimitivetopology) | 06 | native | native | native |
| [`CommandList::setDepthBias`](06-pipelines-part2.md#commandlistsetdepthbias) | 06 | cheap | native | native |
| [`QueueType`](07-command-recording.md#queuetype) | 07 | native | native | native |
| [`Device::acquireCommandList`](07-command-recording.md#deviceacquirecommandlist) | 07 | native | native | native |
| [`CommandList::end`](07-command-recording.md#commandlistend) | 07 | native | native | native |
| [`LoadOp`](07-command-recording.md#loadop) | 07 | native | native | native |
| [`StoreOp`](07-command-recording.md#storeop) | 07 | native | native | native |
| [`ColorAttachment`](07-command-recording.md#colorattachment) | 07 | native | cheap | cheap |
| [`DepthAttachment`](07-command-recording.md#depthattachment) | 07 | native | cheap | cheap |
| [`RenderPassDesc`](07-command-recording.md#renderpassdesc) | 07 | native | native | native |
| [`CommandList::beginRenderPass`](07-command-recording.md#commandlistbeginrenderpass) | 07 | native | cheap | native |
| [`CommandList::endRenderPass`](07-command-recording.md#commandlistendrenderpass) | 07 | native | native | native |
| [`CommandList::setPipeline`](07-command-recording-part2.md#commandlistsetpipeline) | 07 | cheap | native | native |
| [`CommandList::draw`](07-command-recording-part2.md#commandlistdraw) | 07 | native | native | native |
| [`IndexType`](07-command-recording-part2.md#indextype) | 07 | native | native | native |
| [`CommandList::drawIndexed`](07-command-recording-part2.md#commandlistdrawindexed) | 07 | native | native | native |
| [`CommandList::drawMeshTasks`](07-command-recording-part2.md#commandlistdrawmeshtasks) | 07 | native | native | native |
| [`CommandList::dispatch`](07-command-recording-part2.md#commandlistdispatch) | 07 | native | native | native |
| [`CommandList::beginScope`](07-command-recording-part2.md#commandlistbeginscope) | 07 | cheap | cheap | cheap |
| [`CommandList::endScope`](07-command-recording-part2.md#commandlistendscope) | 07 | cheap | cheap | cheap |
| [`BufferTextureLayout`](07-command-recording-part3.md#buffertexturelayout) | 07 | native | native | native |
| [`TextureRegion`](07-command-recording-part3.md#textureregion) | 07 | native | native | native |
| [`CommandList::copyBuffer`](07-command-recording-part3.md#commandlistcopybuffer) | 07 | native | native | native |
| [`CommandList::copyBufferToTexture`](07-command-recording-part3.md#commandlistcopybuffertotexture) | 07 | native | native | native |
| [`CommandList::copyTextureToBuffer`](07-command-recording-part3.md#commandlistcopytexturetobuffer) | 07 | native | native | native |
| [`CommandList::copyTexture`](07-command-recording-part3.md#commandlistcopytexture) | 07 | native | native | native |
| [`CommandList::fillBuffer`](07-command-recording-part3.md#commandlistfillbuffer) | 07 | cheap | costly | native |
| [`ClearValue`](07-command-recording-part3.md#clearvalue) | 07 | native | native | native |
| [`CommandList::clearTexture`](07-command-recording-part3.md#commandlistcleartexture) | 07 | costly | costly | native |
| [`DrawIndirectArgs`](08-gpu-driven.md#drawindirectargs) | 08 | native | native | cheap |
| [`DrawIndexedIndirectArgs`](08-gpu-driven.md#drawindexedindirectargs) | 08 | native | native | cheap |
| [`DrawMeshTasksIndirectArgs`](08-gpu-driven.md#drawmeshtasksindirectargs) | 08 | cheap | native | cheap |
| [`DispatchIndirectArgs`](08-gpu-driven.md#dispatchindirectargs) | 08 | native | native | native |
| [`CommandList::drawIndirect`](08-gpu-driven.md#commandlistdrawindirect) | 08 | costly | cheap | native |
| [`CommandList::drawIndexedIndirect`](08-gpu-driven.md#commandlistdrawindexedindirect) | 08 | costly | cheap | native |
| [`CommandList::drawMeshTasksIndirect`](08-gpu-driven.md#commandlistdrawmeshtasksindirect) | 08 | costly | cheap | native |
| [`CommandList::dispatchIndirect`](08-gpu-driven.md#commandlistdispatchindirect) | 08 | native | cheap | native |
| [`Stage`](09-synchronization.md#stage) | 09 | native | cheap | native |
| [`Access`](09-synchronization.md#access) | 09 | native | native | native |
| [`Layout`](09-synchronization.md#layout) | 09 | native | cheap | native |
| [`GlobalBarrier`](09-synchronization-part2.md#globalbarrier) | 09 | native | native | native |
| [`BufferBarrier`](09-synchronization-part2.md#bufferbarrier) | 09 | native | native | native |
| [`TextureBarrier`](09-synchronization-part2.md#texturebarrier) | 09 | native | native | native |
| [`CommandList::barrier`](09-synchronization-part2.md#commandlistbarrier) | 09 | native | native | native |
| [`Device::queue`](10-queues-and-frames.md#devicequeue) | 10 | native | native | native |
| [`FenceValue`](10-queues-and-frames.md#fencevalue) | 10 | native | native | native |
| [`Device::createFence`](10-queues-and-frames.md#devicecreatefence) | 10 | native | native | native |
| [`Queue::submit`](10-queues-and-frames.md#queuesubmit) | 10 | native | native | native |
| [`Device::completedValue`](10-queues-and-frames.md#devicecompletedvalue) | 10 | native | native | native |
| [`Device::wait`](10-queues-and-frames.md#devicewait) | 10 | native | native | native |
| [`ColorSpace`](11-presentation.md#colorspace) | 11 | native | native | native |
| [`PresentMode`](11-presentation.md#presentmode) | 11 | native | native | native |
| [`SwapchainDesc`](11-presentation.md#swapchaindesc) | 11 | cheap | native | cheap |
| [`Device::createSwapchain`](11-presentation.md#devicecreateswapchain) | 11 | native | native | native |
| [`Swapchain::resize`](11-presentation.md#swapchainresize) | 11 | native | native | native |
| [`Swapchain::acquire`](11-presentation.md#swapchainacquire) | 11 | cheap | native | native |
| [`Queue::present`](11-presentation.md#queuepresent) | 11 | native | native | native |
| [`UpscalerKind`](12-upscaling.md#upscalerkind) | 12 | native | native | native |
| [`UpscalerFlags`](12-upscaling.md#upscalerflags) | 12 | native | native | native |
| [`UpscalerDesc`](12-upscaling.md#upscalerdesc) | 12 | native | native | native |
| [`Device::createUpscaler`](12-upscaling.md#devicecreateupscaler) | 12 | native | cheap | cheap |
| [`UpscaleParams`](12-upscaling.md#upscaleparams) | 12 | native | native | native |
| [`CommandList::upscale`](12-upscaling.md#commandlistupscale) | 12 | cheap | cheap | cheap |
| [`TileShape`](13-sparse.md#tileshape) | 13 | native | native | native |
| [`Device::tileShape`](13-sparse.md#devicetileshape) | 13 | native | native | native |
| [`TileMapping`](13-sparse.md#tilemapping) | 13 | native | native | native |
| [`Queue::mapTiles`](13-sparse.md#queuemaptiles) | 13 | cheap | native | native |
| [`ResourceAccess`](14-diagnostics.md#resourceaccess) | 14 | native | native | native |
| [`CommandList::declareAccess`](14-diagnostics.md#commandlistdeclareaccess) | 14 | cheap | cheap | cheap |
| [`Counters`](14-diagnostics.md#counters) | 14 | cheap | cheap | cheap |
| [`Device::counters`](14-diagnostics.md#devicecounters) | 14 | cheap | cheap | cheap |
| [`ScopeTiming`](14-diagnostics.md#scopetiming) | 14 | cheap | native | native |
| [`Device::scopeTimings`](14-diagnostics.md#devicescopetimings) | 14 | native | native | native |
| [`Device::beginCapture`](14-diagnostics.md#devicebegincapture) | 14 | native | native | native |
| [`Device::endCapture`](14-diagnostics.md#deviceendcapture) | 14 | native | native | native |
| [`Format`](15-formats-and-limits.md#format) | 15 | native | native | native |
| [`FormatCapabilities`](15-formats-and-limits.md#formatcapabilities) | 15 | cheap | native | native |
| [`Device::formatCapabilities`](15-formats-and-limits.md#deviceformatcapabilities) | 15 | cheap | native | native |
| [`Limits`](15-formats-and-limits-part2.md#limits-1) | 15 | cheap | native | native |
