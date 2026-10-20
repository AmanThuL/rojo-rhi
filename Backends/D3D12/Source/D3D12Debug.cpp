//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Debug.cpp
/// @brief Routes the D3D12 debug layer's messages into the message callback and reports a removed device with its data.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Debug.h"

#include "Base/Assert.h"
#include "Base/Log.h"
#include "D3D12Common.h"
#include "D3D12Device.h"

#include <dxgi1_6.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rojoRHI::d3d12 {
    namespace {
        /// A debug-layer message the router drops, with the reason it is not worth reporting.
        struct FilteredMessage
        {
            D3D12_MESSAGE_ID id;     ///< The runtime's message identifier.
            std::string_view reason; ///< One line on why the message is dropped.
        };

        /// The messages the router never forwards. A message is dropped only with a reason that holds for every use of the
        /// library.
        constexpr std::array<FilteredMessage, 0> kFiltered = {};

        /// The most nodes of one list that a removal report prints, so a long history cannot flood the log.
        constexpr size_t kMaxReportedNodes = 32;

        //==============================================================================================================
        /// Returns the name of a message category, for the text of a message.
        std::string_view categoryName(D3D12_MESSAGE_CATEGORY category)
        {
            switch (category)
            {
            case D3D12_MESSAGE_CATEGORY_APPLICATION_DEFINED: return "APPLICATION_DEFINED";
            case D3D12_MESSAGE_CATEGORY_MISCELLANEOUS: return "MISCELLANEOUS";
            case D3D12_MESSAGE_CATEGORY_INITIALIZATION: return "INITIALIZATION";
            case D3D12_MESSAGE_CATEGORY_CLEANUP: return "CLEANUP";
            case D3D12_MESSAGE_CATEGORY_COMPILATION: return "COMPILATION";
            case D3D12_MESSAGE_CATEGORY_STATE_CREATION: return "STATE_CREATION";
            case D3D12_MESSAGE_CATEGORY_STATE_SETTING: return "STATE_SETTING";
            case D3D12_MESSAGE_CATEGORY_STATE_GETTING: return "STATE_GETTING";
            case D3D12_MESSAGE_CATEGORY_RESOURCE_MANIPULATION: return "RESOURCE_MANIPULATION";
            case D3D12_MESSAGE_CATEGORY_EXECUTION: return "EXECUTION";
            case D3D12_MESSAGE_CATEGORY_SHADER: return "SHADER";
            }
            return "UNKNOWN";
        }

        //==============================================================================================================
        /// Receives one debug-layer message on whatever thread the runtime raises it from and forwards it with the matching
        /// severity; after an error or corruption message it aborts the process.
        void __stdcall onMessage(D3D12_MESSAGE_CATEGORY category, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR description, void*)
        {
            MessageSeverity mapped = MessageSeverity::Info;
            if (severity == D3D12_MESSAGE_SEVERITY_WARNING)
            {
                mapped = MessageSeverity::Warning;
            }
            else if (severity == D3D12_MESSAGE_SEVERITY_ERROR || severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
            {
                mapped = MessageSeverity::Error;
            }
            const std::string text = std::format("D3D12 {} {}: {}", categoryName(category), static_cast<int>(id), description != nullptr ? description : "");
            base::emitMessage(mapped, text);
            if (mapped == MessageSeverity::Error)
            {
                std::abort();
            }
        }

        //==============================================================================================================
        /// Returns the name of a breadcrumb or allocation, narrow when the runtime gives one and wide otherwise, or "unnamed".
        std::string nameOf(const char* narrow, const wchar_t* wide)
        {
            if (narrow != nullptr)
            {
                return narrow;
            }
            if (wide != nullptr)
            {
                return utf8(wide);
            }
            return "unnamed";
        }

        //==============================================================================================================
        /// Reports each node of one allocation list as one error message headed by `heading`.
        void reportAllocations(std::string_view heading, const D3D12_DRED_ALLOCATION_NODE1* head)
        {
            size_t count = 0;
            for (const D3D12_DRED_ALLOCATION_NODE1* node = head; node != nullptr; node = node->pNext)
            {
                if (count == kMaxReportedNodes)
                {
                    ROJORHI_LOG_ERROR("D3D12 removal: {} list continues past {} nodes", heading, kMaxReportedNodes);
                    return;
                }
                ++count;
                ROJORHI_LOG_ERROR(
                    "D3D12 removal: {} '{}' (allocation type {})",
                    heading,
                    nameOf(node->ObjectNameA, node->ObjectNameW),
                    static_cast<unsigned>(node->AllocationType)
                );
            }
        }

        //==============================================================================================================
        /// Reports the auto-breadcrumb nodes of `data`, one message each.
        void reportBreadcrumbs(ID3D12DeviceRemovedExtendedData2& data)
        {
            D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 output = {};
            if (FAILED(data.GetAutoBreadcrumbsOutput1(&output)))
            {
                return;
            }
            size_t count = 0;
            for (const D3D12_AUTO_BREADCRUMB_NODE1* node = output.pHeadAutoBreadcrumbNode; node != nullptr; node = node->pNext)
            {
                if (count == kMaxReportedNodes)
                {
                    ROJORHI_LOG_ERROR("D3D12 removal: breadcrumb list continues past {} nodes", kMaxReportedNodes);
                    return;
                }
                ++count;
                const UINT completed = node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0;
                std::string next = "none";
                if (completed < node->BreadcrumbCount && node->pCommandHistory != nullptr)
                {
                    next = std::format("op {}", static_cast<int>(node->pCommandHistory[completed]));
                }
                ROJORHI_LOG_ERROR(
                    "D3D12 removal: command list '{}' on queue '{}' completed {} of {} breadcrumbs, next {}",
                    nameOf(node->pCommandListDebugNameA, node->pCommandListDebugNameW),
                    nameOf(node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW),
                    completed,
                    node->BreadcrumbCount,
                    next
                );
            }
        }

        //==============================================================================================================
        /// Reports the page fault of `data`, if the device recorded one.
        void reportPageFault(ID3D12DeviceRemovedExtendedData2& data)
        {
            D3D12_DRED_PAGE_FAULT_OUTPUT2 fault = {};
            if (FAILED(data.GetPageFaultAllocationOutput2(&fault)) || fault.PageFaultVA == 0)
            {
                return;
            }
            ROJORHI_LOG_ERROR("D3D12 removal: page fault at virtual address 0x{:x}", static_cast<uint64_t>(fault.PageFaultVA));
            reportAllocations("existing allocation", fault.pHeadExistingAllocationNode);
            reportAllocations("recently freed allocation", fault.pHeadRecentFreedAllocationNode);
        }
    } // namespace

    //==================================================================================================================
    MessageRouter::~MessageRouter()
    {
        detach();
    }

    //==================================================================================================================
    void MessageRouter::attach(ID3D12Device12* device, bool layer)
    {
        if (!layer)
        {
            return;
        }
        ROJORHI_INTERNAL_ASSERT(device != nullptr, "the message router was attached to no device");
        ROJORHI_INTERNAL_ASSERT(!m_queue, "the message router was attached twice");
        Com<ID3D12InfoQueue1> queue;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(queue.put()))))
        {
            ROJORHI_INTERNAL_ASSERT(false, "the debug layer is on and the device has no ID3D12InfoQueue1");
            return;
        }

        if constexpr (!kFiltered.empty())
        {
            std::vector<D3D12_MESSAGE_ID> ids;
            for (const FilteredMessage& filtered : kFiltered)
            {
                ids.push_back(filtered.id);
            }
            D3D12_INFO_QUEUE_FILTER filter = {};
            filter.DenyList.NumIDs = static_cast<UINT>(ids.size());
            filter.DenyList.pIDList = ids.data();
            if (FAILED(queue->PushStorageFilter(&filter)))
            {
                ROJORHI_INTERNAL_ASSERT(false, "the debug layer refused the message filter");
                return;
            }
        }

        DWORD cookie = 0;
        if (FAILED(queue->RegisterMessageCallback(onMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE, this, &cookie)))
        {
            ROJORHI_INTERNAL_ASSERT(false, "the debug layer refused the message callback");
            return;
        }
        m_queue = std::move(queue);
        m_cookie = cookie;
    }

    //==================================================================================================================
    void MessageRouter::detach()
    {
        if (!m_queue)
        {
            return;
        }
        if (FAILED(m_queue->UnregisterMessageCallback(m_cookie)))
        {
            ROJORHI_INTERNAL_ASSERT(false, "the debug layer refused to unregister the message callback");
        }
        m_queue.reset();
        m_cookie = 0;
    }

    //==================================================================================================================
    bool deviceRemoved(HRESULT result)
    {
        return result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET || result == DXGI_ERROR_DEVICE_HUNG;
    }

    //==================================================================================================================
    void reportRemoval(DeviceImpl& impl, std::string_view entry)
    {
        ID3D12Device12* device = impl.device();
        if (device == nullptr)
        {
            ROJORHI_LOG_ERROR("{} found the D3D12 device removed, and the device is already released", entry);
            return;
        }
        ROJORHI_LOG_ERROR("{} found the D3D12 device removed, reason 0x{:08x}", entry, hresultBits(device->GetDeviceRemovedReason()));

        Com<ID3D12DeviceRemovedExtendedData2> data;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(data.put()))))
        {
            return;
        }
        reportBreadcrumbs(*data);
        reportPageFault(*data);
    }

    //==================================================================================================================
    Error failure(DeviceImpl& impl, HRESULT result, std::string_view what)
    {
        if (deviceRemoved(result))
        {
            reportRemoval(impl, what);
        }
        return hresultError(result, what);
    }
} // namespace rojoRHI::d3d12
