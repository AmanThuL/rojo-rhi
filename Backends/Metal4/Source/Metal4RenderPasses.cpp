//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4RenderPasses.cpp
/// @brief Implements Metal 4 render passes: the pass is recorded and timed, and its render encoder opens lazily, splits and ends.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Device.h"

#include <rojoRHI/RHI.h>

#include "Base/Assert.h"
#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/Formats.h"
#include "Core/HandlePool.h"
#include "Core/RenderPasses.h"
#include "Metal4Common.h"
#include "Metal4DrawState.h"
#include "Metal4Encoders.h"
#include "Metal4Formats.h"
#include "Metal4ListState.h"
#include "Metal4Timestamps.h"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <cstddef>
#include <cstdint>

namespace rojoRHI {
    namespace {
        //==============================================================================================================
        /// Returns the load action of `load`.
        MTL::LoadAction loadAction(LoadOp load)
        {
            switch (load)
            {
            case LoadOp::Load: return MTL::LoadActionLoad;
            case LoadOp::Clear: return MTL::LoadActionClear;
            case LoadOp::DontCare: break;
            }
            return MTL::LoadActionDontCare;
        }

        //==============================================================================================================
        /// Returns true when an attachment stored by `store`, resolving when `resolves`, is created with
        /// `MTL::StoreActionUnknown`: its store action depends on whether the pass splits, so the encoder sets it before it ends.
        /// `Store` without a resolve is the same either way, and costs no call.
        bool storeDeferred(StoreOp store, bool resolves)
        {
            return store != StoreOp::Store || resolves;
        }

        //==============================================================================================================
        /// Returns the store action an attachment stored by `store`, resolving when `resolves`, is created with.
        MTL::StoreAction createdStore(StoreOp store, bool resolves)
        {
            return storeDeferred(store, resolves) ? MTL::StoreActionUnknown : MTL::StoreActionStore;
        }

        //==============================================================================================================
        /// Returns the store action at a split of an attachment resolving when `resolves`: `Store`, so the resuming encoder loads
        /// what this one rendered. Metal requires a resolve form of a deferred store action on an attachment with a resolve
        /// texture, so one with a resolve target also resolves into it, which the resolve at the pass's end overwrites.
        MTL::StoreAction splitStore(bool resolves)
        {
            return resolves ? MTL::StoreActionStoreAndMultisampleResolve : MTL::StoreActionStore;
        }

        //==============================================================================================================
        /// Returns the store action at the end of a pass of an attachment stored by `store`, resolving when `resolves`.
        MTL::StoreAction finalStore(StoreOp store, bool resolves)
        {
            if (resolves)
            {
                return store == StoreOp::Store ? MTL::StoreActionStoreAndMultisampleResolve : MTL::StoreActionMultisampleResolve;
            }
            return store == StoreOp::Store ? MTL::StoreActionStore : MTL::StoreActionDontCare;
        }

        //==============================================================================================================
        /// Points the list's render pass descriptor at the open pass's attachments: the caller's load actions on the pass's
        /// first encoder, `Load` on one resuming it, and the store actions they are created with; it empties the attachments the
        /// previous pass named that this one does not. The caller holds an autorelease pool, because the attachment getters may
        /// autorelease.
        void configure(metal4::DrawState& draw)
        {
            MTL4::RenderPassDescriptor* descriptor = draw.descriptor;
            MTL::RenderPassColorAttachmentDescriptorArray* attachments = descriptor->colorAttachments();
            const auto count = static_cast<uint32_t>(draw.colors.size());
            for (uint32_t i = 0; i < count; ++i)
            {
                const metal4::PassColor& color = draw.colors[i];
                MTL::RenderPassColorAttachmentDescriptor* attachment = attachments->object(i);
                attachment->setTexture(color.texture);
                attachment->setLevel(color.mip);
                // A cube face is a slice, as it is a RojoRHI layer.
                attachment->setSlice(color.layer);
                attachment->setLoadAction(draw.resuming ? MTL::LoadActionLoad : loadAction(color.load));
                attachment->setClearColor(color.clear);
                attachment->setResolveTexture(color.resolve);
                attachment->setStoreAction(createdStore(color.store, color.resolve != nullptr));
            }
            for (uint32_t i = count; i < draw.descriptorColors; ++i)
            {
                MTL::RenderPassColorAttachmentDescriptor* attachment = attachments->object(i);
                attachment->setTexture(nullptr);
                attachment->setResolveTexture(nullptr);
            }
            draw.descriptorColors = count;
            draw.descriptorDepth = draw.depth.texture != nullptr;
            const metal4::PassDepth& pass = draw.depth;
            MTL::RenderPassDepthAttachmentDescriptor* depth = descriptor->depthAttachment();
            depth->setTexture(pass.texture);
            if (pass.texture != nullptr)
            {
                depth->setLevel(pass.mip);
                depth->setSlice(pass.layer);
                depth->setLoadAction(draw.resuming ? MTL::LoadActionLoad : loadAction(pass.load));
                depth->setClearDepth(pass.clearDepth);
                depth->setStoreAction(createdStore(pass.store, false));
            }
            MTL::RenderPassStencilAttachmentDescriptor* stencil = descriptor->stencilAttachment();
            stencil->setTexture(pass.stencil ? pass.texture : nullptr);
            if (pass.stencil)
            {
                stencil->setLevel(pass.mip);
                stencil->setSlice(pass.layer);
                stencil->setLoadAction(draw.resuming ? MTL::LoadActionLoad : loadAction(pass.stencilLoad));
                stencil->setClearStencil(pass.clearStencil);
                stencil->setStoreAction(createdStore(pass.stencilStore, false));
            }
        }
    } // namespace

    namespace metal4 {
        //==============================================================================================================
        MTL4::RenderCommandEncoder* openRender(CommandListImpl& list)
        {
            DrawState& draw = list.draw;
            ListState& state = list.state;
            ROJORHI_INTERNAL_ASSERT(draw.passOpen, "a render encoder opens with no render pass open");
            // A list has one open encoder, so a compute encoder open inside the pass ends first.
            list.encoders.close(list.core);
            MTL4::RenderCommandEncoder* encoder = nullptr;
            {
                // The attachment getters may autorelease, and the encoder factory does.
                const AutoreleasePool pool;
                if (draw.descriptor == nullptr)
                {
                    draw.descriptor = MTL4::RenderPassDescriptor::alloc()->init();
                }
                configure(draw);
                encoder = list.encoders.openPass(list.core, state, draw.descriptor);
            }
            // The pass's first encoder writes its begin timestamp after its opening calls and before any command, unless the
            // kernels of its counted draws ran first and wrote it.
            writePassBegin(list, encoder);
            // A new encoder holds Metal's defaults.
            const RenderHeld previous = draw.held;
            draw.held = {};
            if (draw.resuming)
            {
                // The encoder a split ended held the state recorded before it, which this one needs too.
                restoreHeld(list, previous);
                // A split popped the groups of the scopes open inside the pass; they enclose this encoder's work too.
                for (size_t i = state.scopeLabels.size() - draw.passGroups; i < state.scopeLabels.size(); ++i)
                {
                    encoder->pushDebugGroup(state.scopeLabels[i]);
                    core::noteNativeCall(list.core);
                }
            }
            draw.resuming = true;
            // The state recorded while the encoder was closed, in order.
            replayPrefix(list);
            return encoder;
        }

        //==============================================================================================================
        void finishPass(CommandListImpl& list, bool split)
        {
            MTL4::RenderCommandEncoder* encoder = list.encoders.renderEncoder();
            const DrawState& draw = list.draw;
            const auto count = static_cast<uint32_t>(draw.colors.size());
            for (uint32_t i = 0; i < count; ++i)
            {
                const PassColor& color = draw.colors[i];
                const bool resolves = color.resolve != nullptr;
                if (storeDeferred(color.store, resolves))
                {
                    encoder->setColorStoreAction(split ? splitStore(resolves) : finalStore(color.store, resolves), i);
                    core::noteNativeCall(list.core);
                }
            }
            const PassDepth& depth = draw.depth;
            if (depth.texture != nullptr && storeDeferred(depth.store, false))
            {
                encoder->setDepthStoreAction(split ? splitStore(false) : finalStore(depth.store, false));
                core::noteNativeCall(list.core);
            }
            if (depth.stencil && storeDeferred(depth.stencilStore, false))
            {
                encoder->setStencilStoreAction(split ? splitStore(false) : finalStore(depth.stencilStore, false));
                core::noteNativeCall(list.core);
            }
        }

        //==============================================================================================================
        void splitPass(CommandListImpl& list)
        {
            MTL4::RenderCommandEncoder* encoder = list.encoders.renderEncoder();
            if (encoder == nullptr)
            {
                return;
            }
            const DrawState& draw = list.draw;
            ROJORHI_INTERNAL_ASSERT(
                draw.passGroups == list.core.scopes.size() - list.core.passScopeBase, "the in-pass debug groups differ from the scopes open inside the pass"
            );
            // The groups belong to the encoder, so they pop before it ends; the strings stay for the resuming encoder.
            for (size_t i = 0; i < draw.passGroups; ++i)
            {
                encoder->popDebugGroup();
                core::noteNativeCall(list.core);
            }
            finishPass(list, true);
            list.encoders.close(list.core);
        }

        //==============================================================================================================
        void applyDrawRecord(CommandListImpl& list, uint64_t address)
        {
            DrawState& draw = list.draw;
            if (draw.tableDrawRecord == address)
            {
                return;
            }
            // The encoders take the table's bindings as they stand at each draw, so the write needs no encoder.
            list.state.table->setAddress(address, 3);
            core::noteNativeCall(list.core);
            draw.tableDrawRecord = address;
        }

        //==============================================================================================================
        void replayCounted(CommandListImpl& list, const PrefixCommand& command)
        {
            MTL4::RenderCommandEncoder* encoder = list.encoders.renderEncoder();
            const bool mesh = command.kind == PrefixKind::MeshExecute || command.kind == PrefixKind::MeshIndirect;
            if (command.kind == PrefixKind::MeshIndirect)
            {
                // The task and mesh entries read the drawId from the caller's record; the native record holds no drawId.
                applyDrawRecord(list, command.mesh.record);
            }
            {
                // The validation layer autoreleases in a draw it checks, so each counted draw holds a pool, as a direct draw does.
                const CommandPool pool(list.state.commandPools);
                switch (command.kind)
                {
                case PrefixKind::Execute:
                case PrefixKind::MeshExecute:
                    // The kernel wrote the range: the records below the count, which the opening barrier made visible.
                    encoder->executeCommandsInBuffer(command.execute.buffer, command.execute.range);
                    break;
                case PrefixKind::MeshIndirect:
                    encoder->drawMeshThreadgroups(command.mesh.native, command.mesh.pipeline->objectThreads, command.mesh.pipeline->meshThreads);
                    break;
                case PrefixKind::DrawIndirect: encoder->drawPrimitives(command.indirect.primitive, command.indirect.record); break;
                case PrefixKind::IndexedIndirect:
                    encoder->drawIndexedPrimitives(
                        command.indirect.primitive, command.indirect.indexType, command.indirect.indices, command.indirect.indexLength, command.indirect.record
                    );
                    break;
                default: ROJORHI_INTERNAL_ASSERT(false, "a prefix entry that is no counted draw is replayed as one"); break;
                }
            }
            core::noteNativeCall(list.core);
            list.encoders.markWork();
            // A mesh draw's barrier held back the object and mesh stages, which the draw spends whether or not its pipeline has
            // a task entry.
            list.encoders.markRan(mesh ? MTL::StageObject | MTL::StageMesh | MTL::StageFragment : MTL::StageVertex | MTL::StageFragment);
        }
    } // namespace metal4

    //==================================================================================================================
    void CommandList::beginRenderPass(const RenderPassDesc& desc)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::beginPass(list.core, desc);
        // No encoder opens: the render encoder waits for the first command that needs it, so the compute encoder ends here.
        list.encoders.close(list.core);
        // The pass unbinds the pipeline, the compute one included.
        list.draw.compute = nullptr;
        // The pass's scope is a debug group on the command buffer, as a scope outside a pass is; its label, or the default
        // the core gave it, names the encoders inside the pass.
        metal4::ListState& state = list.state;
        NS::String* string = state.makeGroupLabel(list.core.scopeLog.back().label, "rojorhi.pass.unnamed");
        state.commandBuffer->pushDebugGroup(string);
        core::noteNativeCall(list.core);
        state.scopeLabels.push_back(string);
        // The pair is claimed now and written on the pass's encoders: the begin timestamp at the start of its first encoder,
        // the kernels' encoder of its counted draws when they run first, so the pass's duration takes in those kernels and
        // the encoders its splits resume. A command buffer's timestamp between encoders would slow the render encoder's draws.
        const uint32_t pair = metal4::claimPair(list);
        // A record's payload is fixed from creation to retirement, so each texture is read with one load and no lock (ADR 0020).
        const auto& textures = list.core.device->pool<Texture>();
        metal4::DrawState& draw = list.draw;
        draw.colors.clear();
        for (const ColorAttachment& attachment : desc.colors)
        {
            const auto* resolve = attachment.resolve.bits == 0 ? nullptr : static_cast<const MTL::Texture*>(textures.native(attachment.resolve));
            draw.colors.push_back(
                metal4::PassColor{ .texture = static_cast<const MTL::Texture*>(textures.native(attachment.texture)),
                                   .mip = attachment.mip,
                                   .layer = attachment.layer,
                                   .load = attachment.load,
                                   .store = attachment.store,
                                   .clear = MTL::ClearColor(attachment.clear.r, attachment.clear.g, attachment.clear.b, attachment.clear.a),
                                   .resolve = resolve }
            );
        }
        draw.depth = {};
        if (desc.depth.texture.bits != 0)
        {
            const DepthAttachment& depth = desc.depth;
            const auto* texture = static_cast<const MTL::Texture*>(textures.native(depth.texture));
            draw.depth = metal4::PassDepth{ .texture = texture,
                                            .mip = depth.mip,
                                            .layer = depth.layer,
                                            .load = depth.load,
                                            .store = depth.store,
                                            .clearDepth = depth.clearDepth,
                                            .stencil = core::hasStencilPlane(metal4::formatOf(texture->pixelFormat())),
                                            .stencilLoad = depth.stencilLoad,
                                            .stencilStore = depth.stencilStore,
                                            .clearStencil = depth.clearStencil };
        }
        draw.passOpen = true;
        draw.resuming = false;
        draw.passGroups = 0;
        draw.passBegin = pair;
    }

    //==================================================================================================================
    void CommandList::endRenderPass()
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::endPass(list.core);
        // A pass that recorded nothing opens its encoder now, so its load and store actions run; that encoder writes both
        // timestamps.
        MTL4::RenderCommandEncoder* encoder = metal4::ensureRender(list);
        metal4::finishPass(list, false);
        // The end timestamp goes on the pass's last encoder, after the fragment stage of all its work, just before it ends.
        metal4::writeInPass(list, encoder, metal4::popPairEnd(list));
        list.encoders.close(list.core);
        metal4::ListState& state = list.state;
        state.commandBuffer->popDebugGroup();
        core::noteNativeCall(list.core);
        state.scopeLabels.back()->release();
        state.scopeLabels.pop_back();
        list.draw.endPass();
    }
} // namespace rojoRHI
