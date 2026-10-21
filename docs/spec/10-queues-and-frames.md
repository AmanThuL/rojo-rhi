# 10. Queues, fences and frames

**Status**: Accepted
**API version**: 0.1.0

A device exposes one queue of each [`QueueType`](07-command-recording.md): `Graphics`, `Compute`
and `Copy`. Work reaches a queue only through `Queue::submit`. Within one queue, work executes in
submission order and barriers order it ([synchronization](09-synchronization.md)); between queues,
and between the GPU and the CPU, only fences order anything.

A fence is a timeline: a 64-bit value that only grows. A submission waits until fences reach
given values before its work starts, and sets fences to given values once its work completes. The
CPU reads a fence's value or blocks until it reaches one. There is no CPU-side signal.

Threading follows [fundamentals](01-fundamentals.md): `Device::queue`, `Device::createFence`,
`Device::completedValue` and `Device::wait` are free-threaded; operations on one `Queue`
(`submit`, [`present`](11-presentation.md) and [`mapTiles`](13-sparse.md)) are serialized by the
caller.

## Frame pacing

Frame pacing is internal to the graphics queue. [`Device::beginFrame`](02-device.md) waits on the
frame slot's internal fence from `DeviceDesc::framesInFlight` frames earlier, then recycles that
slot's command-list pools, frame arena and deferred destructions; [`Device::endFrame`](02-device.md)
signals the slot's fence on the graphics queue after the frame's graphics submissions. That wait and
[`Swapchain::acquire`](11-presentation.md#swapchainacquire), which may block until a swapchain
image, a drawable on Metal 4, is free, are the only implicit waits in the frame loop; device
teardown and presentation setup may also wait.

Other queues synchronize through caller fences. Because a slot retires on the graphics queue's
fence alone, compute and copy work submitted in a frame is joined back into that frame's graphics
work by a fence wait before `endFrame` (`VU-rojo-device-0006`). Data a frame wrote is readable on
the CPU once the caller's own fence reaches its value, or once `beginFrame` has recycled the slot.
A texture handed from one queue to another also sits, at the handoff, in a layout both queues
accept, unless the acquiring queue discards it, which the copy queue cannot: `General` for the
copy queue, and also `ShaderRead`, `ShaderWrite`, `CopySrc` or `CopyDst` between the graphics and
compute queues ([cross-queue handoff](09-synchronization-part2.md#cross-queue-handoff)).

## Host visibility

CPU writes through [`Device::cpuPointer`](03-memory-and-buffers.md) and
[`CommandList::uploadFrameData`](03-memory-and-buffers.md) made before the call to `Queue::submit`
are visible to that submission. GPU writes to `Readback` memory are visible to the CPU once a
fence the writing submission signals is observed at its value through `Device::completedValue` or
`Device::wait`.

### `Device::queue`

```cpp
Queue Device::queue(QueueType type);
```

Returns the device's queue of `type`. The same queue is returned on every call; the device owns it
and it is never destroyed separately.

#### Valid usage
- None.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| `Graphics` | an `MTL4::CommandQueue` | `ID3D12CommandQueue`, `D3D12_COMMAND_LIST_TYPE_DIRECT` | queue of a family with `VK_QUEUE_GRAPHICS_BIT` |
| `Compute` | a second `MTL4::CommandQueue` | `ID3D12CommandQueue`, `D3D12_COMMAND_LIST_TYPE_COMPUTE` | queue of a compute family without graphics, else of the graphics family |
| `Copy` | a third `MTL4::CommandQueue`; copies record on its compute encoder | `ID3D12CommandQueue`, `D3D12_COMMAND_LIST_TYPE_COPY` | queue of a transfer-only family, else of a compute or graphics family |
| Cost | native | native | native |

### `FenceValue`

```cpp
struct FenceValue { Fence fence; uint64_t value; };
```

A point on a fence's timeline, used as a wait or a signal by `Queue::submit` and as a target by
`Device::wait`.

- `fence`: a fence created by `Device::createFence`.
- `value`: the timeline value.

#### Valid usage
- None.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Pair | `MTL::SharedEvent` and value | `ID3D12Fence` and value | timeline `VkSemaphore` and value in `VkSemaphoreSubmitInfo` |
| Cost | native | native | native |

### `Device::createFence`

```cpp
Result<Fence> Device::createFence(uint64_t initial, std::string_view label);
```

Creates a timeline fence whose completed value starts at `initial`. An empty `label` receives the
default `rojorhi.` label ([fundamentals](01-fundamentals.md)). A fence is destroyed through
[`Device::destroy`](02-device.md), deferred like any resource. Fails with an error when the driver
refuses the fence or memory is exhausted.

#### Valid usage
- None.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL::Device::newSharedEvent`, then `MTL::SharedEvent::setSignaledValue(initial)` | `CreateFence(initial, D3D12_FENCE_FLAG_NONE)` | `vkCreateSemaphore` with `VkSemaphoreTypeCreateInfo` of type `TIMELINE` |
| Cost | native | native | native |

### `Queue::submit`

```cpp
void Queue::submit(std::span<const CommandList> lists, std::span<const FenceValue> wait = {},
                   std::span<const FenceValue> signal = {});
```

Submits `lists` for execution in span order. No list starts before every fence in `wait` has
reached its value; every fence in `signal` is set to its value once every list has completed. A
submission with no lists only waits and signals. On Metal 4 the backend commits the device's
residency set first when it has changed ([memory and buffers](03-memory-and-buffers.md)).

No other thread submits, presents or maps tiles on this queue during the call; such a race breaks
the per-queue threading class of [fundamentals](01-fundamentals.md) and no tier detects it. Work
submitted to the `Compute` or `Copy` queue in a frame is joined into a `Graphics` submission of the
same frame by a fence wait, as `VU-rojo-device-0006` of
[`Device::endFrame`](02-device.md#deviceendframe) requires.

#### Valid usage
- `VU-rojo-queue-0001` Every list in `lists` has ended, has not been submitted before, and was
  acquired in the frame that is still open.
- `VU-rojo-queue-0002` Every list in `lists` was acquired for this queue's `QueueType`.
- `VU-rojo-queue-0003` Each fence appears at most once in `signal`, and each signalled value is
  greater than every value already submitted for signal on that fence and than its initial value.
- `VU-rojo-queue-0004` Retired in 0.1.0: Every value in `wait` is at most the fence's initial value
  or a value already submitted for signal on that fence.
- `VU-rojo-queue-0007` Every value in `wait` is at most the fence's initial value or the greatest
  value already submitted for signal on that fence.
- `VU-rojo-queue-0005` Two uses of the same memory on different queues, at least one a write, are
  ordered by a fence: the later use's submission waits, directly or through other fences, on a value
  signalled by or after the earlier use's submission. Ordering through an earlier submission on the
  same queue, and completion the CPU observed through a fence, count as orderings by a fence. Uses
  of the same heap bytes by two placed resources are uses of the same memory. Checked by the
  validation tracker.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Wait | `MTL4::CommandQueue::wait` with the event and value | `ID3D12CommandQueue::Wait` | `pWaitSemaphoreInfos` of `VkSubmitInfo2` |
| Execute | `MTL4::CommandQueue::commit` with the command buffers and count | `ExecuteCommandLists` | `pCommandBufferInfos` of `vkQueueSubmit2` |
| Signal | `MTL4::CommandQueue::signalEvent` with the event and value; a submission on the `Compute` or `Copy` queue that waits or signals with no lists, or signals more than one fence, then signals the queue's own progress event, which keeps a destroyed fence's event until the queue passes it, since no frame join orders all its signals | `ID3D12CommandQueue::Signal` | `pSignalSemaphoreInfos`, after a memory barrier to `VK_PIPELINE_STAGE_2_HOST` that makes writes host-visible |
| Cost | native: one call per wait and signal and one commit; such a `Compute` or `Copy` submission adds one signal, measured on an Apple M3 Max at about 55 ns of CPU and two allocations | native | native |

### `Device::completedValue`

```cpp
uint64_t Device::completedValue(Fence fence);
```

Returns the greatest value `fence` has reached. It never blocks.

#### Valid usage
- None.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL::SharedEvent::signaledValue` | `ID3D12Fence::GetCompletedValue` | `vkGetSemaphoreCounterValue` |
| Cost | native | native | native |

### `Device::wait`

```cpp
bool Device::wait(FenceValue value, uint64_t timeoutNs);
```

Blocks the calling thread until `value.fence` reaches `value.value` or `timeoutNs` nanoseconds
pass. Returns `true` when the value was reached and `false` on timeout. A `timeoutNs` of zero polls;
`UINT64_MAX` waits without limit.

#### Valid usage
- `VU-rojo-queue-0006` Retired in 0.1.0: When `timeoutNs` is `UINT64_MAX`, `value.value` is at most
  the fence's initial value or a value already submitted for signal on that fence.
- `VU-rojo-queue-0008` When `timeoutNs` is `UINT64_MAX`, `value.value` is at most the fence's
  initial value or the greatest value already submitted for signal on that fence.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL::SharedEvent::waitUntilSignaledValue(value, timeoutMS)`, timeout rounded up to milliseconds | `ID3D12Fence::SetEventOnCompletion`, then `WaitForSingleObject`, timeout rounded up to milliseconds | `vkWaitSemaphores` |
| Cost | native | native | native |
