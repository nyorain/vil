#pragma once
// *** THIS FILE IS GENERATED - DO NOT EDIT ***
// See dispatch_table_helper_generator.py for modifications

/*
 * Copyright (c) 2015-2025 The Khronos Group Inc.
 * Copyright (c) 2015-2025 Valve Corporation
 * Copyright (c) 2015-2025 LunarG, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Author: Courtney Goeltzenleuchter <courtney@LunarG.com>
 * Author: Jon Ashburn <jon@lunarg.com>
 * Author: Mark Lobodzinski <mark@lunarg.com>
 * Author: Charles Giessen <charles@lunarg.com>
 */

#include <vk/vulkan.h>
#include <vk/vk_layer.h>
#include <string.h>
#include <vk/dispatch_table.h>

#ifdef __GNUC__
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wpedantic"
	#pragma GCC diagnostic ignored "-Wunused-parameter"
#elif defined(_MSC_VER)
    #pragma warning(push, 3)
#endif

static inline void vkuInitDeviceDispatchTable(VkDevice device, VkuDeviceDispatchTable *table, PFN_vkGetDeviceProcAddr gdpa) {
    memset(table, 0, sizeof(*table));
    // Device function pointers
    table->GetDeviceProcAddr = gdpa;
    table->DestroyDevice = (PFN_vkDestroyDevice)gdpa(device, "vkDestroyDevice");
    table->GetDeviceQueue = (PFN_vkGetDeviceQueue)gdpa(device, "vkGetDeviceQueue");
    table->QueueSubmit = (PFN_vkQueueSubmit)gdpa(device, "vkQueueSubmit");
    table->QueueWaitIdle = (PFN_vkQueueWaitIdle)gdpa(device, "vkQueueWaitIdle");
    table->DeviceWaitIdle = (PFN_vkDeviceWaitIdle)gdpa(device, "vkDeviceWaitIdle");
    table->AllocateMemory = (PFN_vkAllocateMemory)gdpa(device, "vkAllocateMemory");
    table->FreeMemory = (PFN_vkFreeMemory)gdpa(device, "vkFreeMemory");
    table->MapMemory = (PFN_vkMapMemory)gdpa(device, "vkMapMemory");
    table->UnmapMemory = (PFN_vkUnmapMemory)gdpa(device, "vkUnmapMemory");
    table->FlushMappedMemoryRanges = (PFN_vkFlushMappedMemoryRanges)gdpa(device, "vkFlushMappedMemoryRanges");
    table->InvalidateMappedMemoryRanges = (PFN_vkInvalidateMappedMemoryRanges)gdpa(device, "vkInvalidateMappedMemoryRanges");
    table->GetDeviceMemoryCommitment = (PFN_vkGetDeviceMemoryCommitment)gdpa(device, "vkGetDeviceMemoryCommitment");
    table->BindBufferMemory = (PFN_vkBindBufferMemory)gdpa(device, "vkBindBufferMemory");
    table->BindImageMemory = (PFN_vkBindImageMemory)gdpa(device, "vkBindImageMemory");
    table->GetBufferMemoryRequirements = (PFN_vkGetBufferMemoryRequirements)gdpa(device, "vkGetBufferMemoryRequirements");
    table->GetImageMemoryRequirements = (PFN_vkGetImageMemoryRequirements)gdpa(device, "vkGetImageMemoryRequirements");
    table->GetImageSparseMemoryRequirements = (PFN_vkGetImageSparseMemoryRequirements)gdpa(device, "vkGetImageSparseMemoryRequirements");
    table->QueueBindSparse = (PFN_vkQueueBindSparse)gdpa(device, "vkQueueBindSparse");
    table->CreateFence = (PFN_vkCreateFence)gdpa(device, "vkCreateFence");
    table->DestroyFence = (PFN_vkDestroyFence)gdpa(device, "vkDestroyFence");
    table->ResetFences = (PFN_vkResetFences)gdpa(device, "vkResetFences");
    table->GetFenceStatus = (PFN_vkGetFenceStatus)gdpa(device, "vkGetFenceStatus");
    table->WaitForFences = (PFN_vkWaitForFences)gdpa(device, "vkWaitForFences");
    table->CreateSemaphore = (PFN_vkCreateSemaphore)gdpa(device, "vkCreateSemaphore");
    table->DestroySemaphore = (PFN_vkDestroySemaphore)gdpa(device, "vkDestroySemaphore");
    table->CreateQueryPool = (PFN_vkCreateQueryPool)gdpa(device, "vkCreateQueryPool");
    table->DestroyQueryPool = (PFN_vkDestroyQueryPool)gdpa(device, "vkDestroyQueryPool");
    table->GetQueryPoolResults = (PFN_vkGetQueryPoolResults)gdpa(device, "vkGetQueryPoolResults");
    table->CreateBuffer = (PFN_vkCreateBuffer)gdpa(device, "vkCreateBuffer");
    table->DestroyBuffer = (PFN_vkDestroyBuffer)gdpa(device, "vkDestroyBuffer");
    table->CreateImage = (PFN_vkCreateImage)gdpa(device, "vkCreateImage");
    table->DestroyImage = (PFN_vkDestroyImage)gdpa(device, "vkDestroyImage");
    table->GetImageSubresourceLayout = (PFN_vkGetImageSubresourceLayout)gdpa(device, "vkGetImageSubresourceLayout");
    table->CreateImageView = (PFN_vkCreateImageView)gdpa(device, "vkCreateImageView");
    table->DestroyImageView = (PFN_vkDestroyImageView)gdpa(device, "vkDestroyImageView");
    table->CreateCommandPool = (PFN_vkCreateCommandPool)gdpa(device, "vkCreateCommandPool");
    table->DestroyCommandPool = (PFN_vkDestroyCommandPool)gdpa(device, "vkDestroyCommandPool");
    table->ResetCommandPool = (PFN_vkResetCommandPool)gdpa(device, "vkResetCommandPool");
    table->AllocateCommandBuffers = (PFN_vkAllocateCommandBuffers)gdpa(device, "vkAllocateCommandBuffers");
    table->FreeCommandBuffers = (PFN_vkFreeCommandBuffers)gdpa(device, "vkFreeCommandBuffers");
    table->BeginCommandBuffer = (PFN_vkBeginCommandBuffer)gdpa(device, "vkBeginCommandBuffer");
    table->EndCommandBuffer = (PFN_vkEndCommandBuffer)gdpa(device, "vkEndCommandBuffer");
    table->ResetCommandBuffer = (PFN_vkResetCommandBuffer)gdpa(device, "vkResetCommandBuffer");
    table->CmdCopyBuffer = (PFN_vkCmdCopyBuffer)gdpa(device, "vkCmdCopyBuffer");
    table->CmdCopyImage = (PFN_vkCmdCopyImage)gdpa(device, "vkCmdCopyImage");
    table->CmdCopyBufferToImage = (PFN_vkCmdCopyBufferToImage)gdpa(device, "vkCmdCopyBufferToImage");
    table->CmdCopyImageToBuffer = (PFN_vkCmdCopyImageToBuffer)gdpa(device, "vkCmdCopyImageToBuffer");
    table->CmdUpdateBuffer = (PFN_vkCmdUpdateBuffer)gdpa(device, "vkCmdUpdateBuffer");
    table->CmdFillBuffer = (PFN_vkCmdFillBuffer)gdpa(device, "vkCmdFillBuffer");
    table->CmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)gdpa(device, "vkCmdPipelineBarrier");
    table->CmdBeginQuery = (PFN_vkCmdBeginQuery)gdpa(device, "vkCmdBeginQuery");
    table->CmdEndQuery = (PFN_vkCmdEndQuery)gdpa(device, "vkCmdEndQuery");
    table->CmdResetQueryPool = (PFN_vkCmdResetQueryPool)gdpa(device, "vkCmdResetQueryPool");
    table->CmdWriteTimestamp = (PFN_vkCmdWriteTimestamp)gdpa(device, "vkCmdWriteTimestamp");
    table->CmdCopyQueryPoolResults = (PFN_vkCmdCopyQueryPoolResults)gdpa(device, "vkCmdCopyQueryPoolResults");
    table->CmdExecuteCommands = (PFN_vkCmdExecuteCommands)gdpa(device, "vkCmdExecuteCommands");
    table->CreateEvent = (PFN_vkCreateEvent)gdpa(device, "vkCreateEvent");
    table->DestroyEvent = (PFN_vkDestroyEvent)gdpa(device, "vkDestroyEvent");
    table->GetEventStatus = (PFN_vkGetEventStatus)gdpa(device, "vkGetEventStatus");
    table->SetEvent = (PFN_vkSetEvent)gdpa(device, "vkSetEvent");
    table->ResetEvent = (PFN_vkResetEvent)gdpa(device, "vkResetEvent");
    table->CreateBufferView = (PFN_vkCreateBufferView)gdpa(device, "vkCreateBufferView");
    table->DestroyBufferView = (PFN_vkDestroyBufferView)gdpa(device, "vkDestroyBufferView");
    table->CreateShaderModule = (PFN_vkCreateShaderModule)gdpa(device, "vkCreateShaderModule");
    table->DestroyShaderModule = (PFN_vkDestroyShaderModule)gdpa(device, "vkDestroyShaderModule");
    table->CreatePipelineCache = (PFN_vkCreatePipelineCache)gdpa(device, "vkCreatePipelineCache");
    table->DestroyPipelineCache = (PFN_vkDestroyPipelineCache)gdpa(device, "vkDestroyPipelineCache");
    table->GetPipelineCacheData = (PFN_vkGetPipelineCacheData)gdpa(device, "vkGetPipelineCacheData");
    table->MergePipelineCaches = (PFN_vkMergePipelineCaches)gdpa(device, "vkMergePipelineCaches");
    table->CreateComputePipelines = (PFN_vkCreateComputePipelines)gdpa(device, "vkCreateComputePipelines");
    table->DestroyPipeline = (PFN_vkDestroyPipeline)gdpa(device, "vkDestroyPipeline");
    table->CreatePipelineLayout = (PFN_vkCreatePipelineLayout)gdpa(device, "vkCreatePipelineLayout");
    table->DestroyPipelineLayout = (PFN_vkDestroyPipelineLayout)gdpa(device, "vkDestroyPipelineLayout");
    table->CreateSampler = (PFN_vkCreateSampler)gdpa(device, "vkCreateSampler");
    table->DestroySampler = (PFN_vkDestroySampler)gdpa(device, "vkDestroySampler");
    table->CreateDescriptorSetLayout = (PFN_vkCreateDescriptorSetLayout)gdpa(device, "vkCreateDescriptorSetLayout");
    table->DestroyDescriptorSetLayout = (PFN_vkDestroyDescriptorSetLayout)gdpa(device, "vkDestroyDescriptorSetLayout");
    table->CreateDescriptorPool = (PFN_vkCreateDescriptorPool)gdpa(device, "vkCreateDescriptorPool");
    table->DestroyDescriptorPool = (PFN_vkDestroyDescriptorPool)gdpa(device, "vkDestroyDescriptorPool");
    table->ResetDescriptorPool = (PFN_vkResetDescriptorPool)gdpa(device, "vkResetDescriptorPool");
    table->AllocateDescriptorSets = (PFN_vkAllocateDescriptorSets)gdpa(device, "vkAllocateDescriptorSets");
    table->FreeDescriptorSets = (PFN_vkFreeDescriptorSets)gdpa(device, "vkFreeDescriptorSets");
    table->UpdateDescriptorSets = (PFN_vkUpdateDescriptorSets)gdpa(device, "vkUpdateDescriptorSets");
    table->CmdBindPipeline = (PFN_vkCmdBindPipeline)gdpa(device, "vkCmdBindPipeline");
    table->CmdBindDescriptorSets = (PFN_vkCmdBindDescriptorSets)gdpa(device, "vkCmdBindDescriptorSets");
    table->CmdClearColorImage = (PFN_vkCmdClearColorImage)gdpa(device, "vkCmdClearColorImage");
    table->CmdDispatch = (PFN_vkCmdDispatch)gdpa(device, "vkCmdDispatch");
    table->CmdDispatchIndirect = (PFN_vkCmdDispatchIndirect)gdpa(device, "vkCmdDispatchIndirect");
    table->CmdSetEvent = (PFN_vkCmdSetEvent)gdpa(device, "vkCmdSetEvent");
    table->CmdResetEvent = (PFN_vkCmdResetEvent)gdpa(device, "vkCmdResetEvent");
    table->CmdWaitEvents = (PFN_vkCmdWaitEvents)gdpa(device, "vkCmdWaitEvents");
    table->CmdPushConstants = (PFN_vkCmdPushConstants)gdpa(device, "vkCmdPushConstants");
    table->CreateGraphicsPipelines = (PFN_vkCreateGraphicsPipelines)gdpa(device, "vkCreateGraphicsPipelines");
    table->CreateFramebuffer = (PFN_vkCreateFramebuffer)gdpa(device, "vkCreateFramebuffer");
    table->DestroyFramebuffer = (PFN_vkDestroyFramebuffer)gdpa(device, "vkDestroyFramebuffer");
    table->CreateRenderPass = (PFN_vkCreateRenderPass)gdpa(device, "vkCreateRenderPass");
    table->DestroyRenderPass = (PFN_vkDestroyRenderPass)gdpa(device, "vkDestroyRenderPass");
    table->GetRenderAreaGranularity = (PFN_vkGetRenderAreaGranularity)gdpa(device, "vkGetRenderAreaGranularity");
    table->CmdSetViewport = (PFN_vkCmdSetViewport)gdpa(device, "vkCmdSetViewport");
    table->CmdSetScissor = (PFN_vkCmdSetScissor)gdpa(device, "vkCmdSetScissor");
    table->CmdSetLineWidth = (PFN_vkCmdSetLineWidth)gdpa(device, "vkCmdSetLineWidth");
    table->CmdSetDepthBias = (PFN_vkCmdSetDepthBias)gdpa(device, "vkCmdSetDepthBias");
    table->CmdSetBlendConstants = (PFN_vkCmdSetBlendConstants)gdpa(device, "vkCmdSetBlendConstants");
    table->CmdSetDepthBounds = (PFN_vkCmdSetDepthBounds)gdpa(device, "vkCmdSetDepthBounds");
    table->CmdSetStencilCompareMask = (PFN_vkCmdSetStencilCompareMask)gdpa(device, "vkCmdSetStencilCompareMask");
    table->CmdSetStencilWriteMask = (PFN_vkCmdSetStencilWriteMask)gdpa(device, "vkCmdSetStencilWriteMask");
    table->CmdSetStencilReference = (PFN_vkCmdSetStencilReference)gdpa(device, "vkCmdSetStencilReference");
    table->CmdBindIndexBuffer = (PFN_vkCmdBindIndexBuffer)gdpa(device, "vkCmdBindIndexBuffer");
    table->CmdBindVertexBuffers = (PFN_vkCmdBindVertexBuffers)gdpa(device, "vkCmdBindVertexBuffers");
    table->CmdDraw = (PFN_vkCmdDraw)gdpa(device, "vkCmdDraw");
    table->CmdDrawIndexed = (PFN_vkCmdDrawIndexed)gdpa(device, "vkCmdDrawIndexed");
    table->CmdDrawIndirect = (PFN_vkCmdDrawIndirect)gdpa(device, "vkCmdDrawIndirect");
    table->CmdDrawIndexedIndirect = (PFN_vkCmdDrawIndexedIndirect)gdpa(device, "vkCmdDrawIndexedIndirect");
    table->CmdBlitImage = (PFN_vkCmdBlitImage)gdpa(device, "vkCmdBlitImage");
    table->CmdClearDepthStencilImage = (PFN_vkCmdClearDepthStencilImage)gdpa(device, "vkCmdClearDepthStencilImage");
    table->CmdClearAttachments = (PFN_vkCmdClearAttachments)gdpa(device, "vkCmdClearAttachments");
    table->CmdResolveImage = (PFN_vkCmdResolveImage)gdpa(device, "vkCmdResolveImage");
    table->CmdBeginRenderPass = (PFN_vkCmdBeginRenderPass)gdpa(device, "vkCmdBeginRenderPass");
    table->CmdNextSubpass = (PFN_vkCmdNextSubpass)gdpa(device, "vkCmdNextSubpass");
    table->CmdEndRenderPass = (PFN_vkCmdEndRenderPass)gdpa(device, "vkCmdEndRenderPass");
    table->BindBufferMemory2 = (PFN_vkBindBufferMemory2)gdpa(device, "vkBindBufferMemory2");
    table->BindImageMemory2 = (PFN_vkBindImageMemory2)gdpa(device, "vkBindImageMemory2");
    table->GetDeviceGroupPeerMemoryFeatures = (PFN_vkGetDeviceGroupPeerMemoryFeatures)gdpa(device, "vkGetDeviceGroupPeerMemoryFeatures");
    table->CmdSetDeviceMask = (PFN_vkCmdSetDeviceMask)gdpa(device, "vkCmdSetDeviceMask");
    table->GetImageMemoryRequirements2 = (PFN_vkGetImageMemoryRequirements2)gdpa(device, "vkGetImageMemoryRequirements2");
    table->GetBufferMemoryRequirements2 = (PFN_vkGetBufferMemoryRequirements2)gdpa(device, "vkGetBufferMemoryRequirements2");
    table->GetImageSparseMemoryRequirements2 = (PFN_vkGetImageSparseMemoryRequirements2)gdpa(device, "vkGetImageSparseMemoryRequirements2");
    table->TrimCommandPool = (PFN_vkTrimCommandPool)gdpa(device, "vkTrimCommandPool");
    table->GetDeviceQueue2 = (PFN_vkGetDeviceQueue2)gdpa(device, "vkGetDeviceQueue2");
    table->CmdDispatchBase = (PFN_vkCmdDispatchBase)gdpa(device, "vkCmdDispatchBase");
    table->CreateDescriptorUpdateTemplate = (PFN_vkCreateDescriptorUpdateTemplate)gdpa(device, "vkCreateDescriptorUpdateTemplate");
    table->DestroyDescriptorUpdateTemplate = (PFN_vkDestroyDescriptorUpdateTemplate)gdpa(device, "vkDestroyDescriptorUpdateTemplate");
    table->UpdateDescriptorSetWithTemplate = (PFN_vkUpdateDescriptorSetWithTemplate)gdpa(device, "vkUpdateDescriptorSetWithTemplate");
    table->GetDescriptorSetLayoutSupport = (PFN_vkGetDescriptorSetLayoutSupport)gdpa(device, "vkGetDescriptorSetLayoutSupport");
    table->CreateSamplerYcbcrConversion = (PFN_vkCreateSamplerYcbcrConversion)gdpa(device, "vkCreateSamplerYcbcrConversion");
    table->DestroySamplerYcbcrConversion = (PFN_vkDestroySamplerYcbcrConversion)gdpa(device, "vkDestroySamplerYcbcrConversion");
    table->ResetQueryPool = (PFN_vkResetQueryPool)gdpa(device, "vkResetQueryPool");
    table->GetSemaphoreCounterValue = (PFN_vkGetSemaphoreCounterValue)gdpa(device, "vkGetSemaphoreCounterValue");
    table->WaitSemaphores = (PFN_vkWaitSemaphores)gdpa(device, "vkWaitSemaphores");
    table->SignalSemaphore = (PFN_vkSignalSemaphore)gdpa(device, "vkSignalSemaphore");
    table->GetBufferDeviceAddress = (PFN_vkGetBufferDeviceAddress)gdpa(device, "vkGetBufferDeviceAddress");
    table->GetBufferOpaqueCaptureAddress = (PFN_vkGetBufferOpaqueCaptureAddress)gdpa(device, "vkGetBufferOpaqueCaptureAddress");
    table->GetDeviceMemoryOpaqueCaptureAddress = (PFN_vkGetDeviceMemoryOpaqueCaptureAddress)gdpa(device, "vkGetDeviceMemoryOpaqueCaptureAddress");
    table->CmdDrawIndirectCount = (PFN_vkCmdDrawIndirectCount)gdpa(device, "vkCmdDrawIndirectCount");
    table->CmdDrawIndexedIndirectCount = (PFN_vkCmdDrawIndexedIndirectCount)gdpa(device, "vkCmdDrawIndexedIndirectCount");
    table->CreateRenderPass2 = (PFN_vkCreateRenderPass2)gdpa(device, "vkCreateRenderPass2");
    table->CmdBeginRenderPass2 = (PFN_vkCmdBeginRenderPass2)gdpa(device, "vkCmdBeginRenderPass2");
    table->CmdNextSubpass2 = (PFN_vkCmdNextSubpass2)gdpa(device, "vkCmdNextSubpass2");
    table->CmdEndRenderPass2 = (PFN_vkCmdEndRenderPass2)gdpa(device, "vkCmdEndRenderPass2");
    table->CreatePrivateDataSlot = (PFN_vkCreatePrivateDataSlot)gdpa(device, "vkCreatePrivateDataSlot");
    table->DestroyPrivateDataSlot = (PFN_vkDestroyPrivateDataSlot)gdpa(device, "vkDestroyPrivateDataSlot");
    table->SetPrivateData = (PFN_vkSetPrivateData)gdpa(device, "vkSetPrivateData");
    table->GetPrivateData = (PFN_vkGetPrivateData)gdpa(device, "vkGetPrivateData");
    table->CmdPipelineBarrier2 = (PFN_vkCmdPipelineBarrier2)gdpa(device, "vkCmdPipelineBarrier2");
    table->CmdWriteTimestamp2 = (PFN_vkCmdWriteTimestamp2)gdpa(device, "vkCmdWriteTimestamp2");
    table->QueueSubmit2 = (PFN_vkQueueSubmit2)gdpa(device, "vkQueueSubmit2");
    table->CmdCopyBuffer2 = (PFN_vkCmdCopyBuffer2)gdpa(device, "vkCmdCopyBuffer2");
    table->CmdCopyImage2 = (PFN_vkCmdCopyImage2)gdpa(device, "vkCmdCopyImage2");
    table->CmdCopyBufferToImage2 = (PFN_vkCmdCopyBufferToImage2)gdpa(device, "vkCmdCopyBufferToImage2");
    table->CmdCopyImageToBuffer2 = (PFN_vkCmdCopyImageToBuffer2)gdpa(device, "vkCmdCopyImageToBuffer2");
    table->GetDeviceBufferMemoryRequirements = (PFN_vkGetDeviceBufferMemoryRequirements)gdpa(device, "vkGetDeviceBufferMemoryRequirements");
    table->GetDeviceImageMemoryRequirements = (PFN_vkGetDeviceImageMemoryRequirements)gdpa(device, "vkGetDeviceImageMemoryRequirements");
    table->GetDeviceImageSparseMemoryRequirements = (PFN_vkGetDeviceImageSparseMemoryRequirements)gdpa(device, "vkGetDeviceImageSparseMemoryRequirements");
    table->CmdSetEvent2 = (PFN_vkCmdSetEvent2)gdpa(device, "vkCmdSetEvent2");
    table->CmdResetEvent2 = (PFN_vkCmdResetEvent2)gdpa(device, "vkCmdResetEvent2");
    table->CmdWaitEvents2 = (PFN_vkCmdWaitEvents2)gdpa(device, "vkCmdWaitEvents2");
    table->CmdBlitImage2 = (PFN_vkCmdBlitImage2)gdpa(device, "vkCmdBlitImage2");
    table->CmdResolveImage2 = (PFN_vkCmdResolveImage2)gdpa(device, "vkCmdResolveImage2");
    table->CmdBeginRendering = (PFN_vkCmdBeginRendering)gdpa(device, "vkCmdBeginRendering");
    table->CmdEndRendering = (PFN_vkCmdEndRendering)gdpa(device, "vkCmdEndRendering");
    table->CmdSetCullMode = (PFN_vkCmdSetCullMode)gdpa(device, "vkCmdSetCullMode");
    table->CmdSetFrontFace = (PFN_vkCmdSetFrontFace)gdpa(device, "vkCmdSetFrontFace");
    table->CmdSetPrimitiveTopology = (PFN_vkCmdSetPrimitiveTopology)gdpa(device, "vkCmdSetPrimitiveTopology");
    table->CmdSetViewportWithCount = (PFN_vkCmdSetViewportWithCount)gdpa(device, "vkCmdSetViewportWithCount");
    table->CmdSetScissorWithCount = (PFN_vkCmdSetScissorWithCount)gdpa(device, "vkCmdSetScissorWithCount");
    table->CmdBindVertexBuffers2 = (PFN_vkCmdBindVertexBuffers2)gdpa(device, "vkCmdBindVertexBuffers2");
    table->CmdSetDepthTestEnable = (PFN_vkCmdSetDepthTestEnable)gdpa(device, "vkCmdSetDepthTestEnable");
    table->CmdSetDepthWriteEnable = (PFN_vkCmdSetDepthWriteEnable)gdpa(device, "vkCmdSetDepthWriteEnable");
    table->CmdSetDepthCompareOp = (PFN_vkCmdSetDepthCompareOp)gdpa(device, "vkCmdSetDepthCompareOp");
    table->CmdSetDepthBoundsTestEnable = (PFN_vkCmdSetDepthBoundsTestEnable)gdpa(device, "vkCmdSetDepthBoundsTestEnable");
    table->CmdSetStencilTestEnable = (PFN_vkCmdSetStencilTestEnable)gdpa(device, "vkCmdSetStencilTestEnable");
    table->CmdSetStencilOp = (PFN_vkCmdSetStencilOp)gdpa(device, "vkCmdSetStencilOp");
    table->CmdSetRasterizerDiscardEnable = (PFN_vkCmdSetRasterizerDiscardEnable)gdpa(device, "vkCmdSetRasterizerDiscardEnable");
    table->CmdSetDepthBiasEnable = (PFN_vkCmdSetDepthBiasEnable)gdpa(device, "vkCmdSetDepthBiasEnable");
    table->CmdSetPrimitiveRestartEnable = (PFN_vkCmdSetPrimitiveRestartEnable)gdpa(device, "vkCmdSetPrimitiveRestartEnable");
    table->MapMemory2 = (PFN_vkMapMemory2)gdpa(device, "vkMapMemory2");
    table->UnmapMemory2 = (PFN_vkUnmapMemory2)gdpa(device, "vkUnmapMemory2");
    table->GetDeviceImageSubresourceLayout = (PFN_vkGetDeviceImageSubresourceLayout)gdpa(device, "vkGetDeviceImageSubresourceLayout");
    table->GetImageSubresourceLayout2 = (PFN_vkGetImageSubresourceLayout2)gdpa(device, "vkGetImageSubresourceLayout2");
    table->CopyMemoryToImage = (PFN_vkCopyMemoryToImage)gdpa(device, "vkCopyMemoryToImage");
    table->CopyImageToMemory = (PFN_vkCopyImageToMemory)gdpa(device, "vkCopyImageToMemory");
    table->CopyImageToImage = (PFN_vkCopyImageToImage)gdpa(device, "vkCopyImageToImage");
    table->TransitionImageLayout = (PFN_vkTransitionImageLayout)gdpa(device, "vkTransitionImageLayout");
    table->CmdPushDescriptorSet = (PFN_vkCmdPushDescriptorSet)gdpa(device, "vkCmdPushDescriptorSet");
    table->CmdPushDescriptorSetWithTemplate = (PFN_vkCmdPushDescriptorSetWithTemplate)gdpa(device, "vkCmdPushDescriptorSetWithTemplate");
    table->CmdBindDescriptorSets2 = (PFN_vkCmdBindDescriptorSets2)gdpa(device, "vkCmdBindDescriptorSets2");
    table->CmdPushConstants2 = (PFN_vkCmdPushConstants2)gdpa(device, "vkCmdPushConstants2");
    table->CmdPushDescriptorSet2 = (PFN_vkCmdPushDescriptorSet2)gdpa(device, "vkCmdPushDescriptorSet2");
    table->CmdPushDescriptorSetWithTemplate2 = (PFN_vkCmdPushDescriptorSetWithTemplate2)gdpa(device, "vkCmdPushDescriptorSetWithTemplate2");
    table->CmdSetLineStipple = (PFN_vkCmdSetLineStipple)gdpa(device, "vkCmdSetLineStipple");
    table->CmdBindIndexBuffer2 = (PFN_vkCmdBindIndexBuffer2)gdpa(device, "vkCmdBindIndexBuffer2");
    table->GetRenderingAreaGranularity = (PFN_vkGetRenderingAreaGranularity)gdpa(device, "vkGetRenderingAreaGranularity");
    table->CmdSetRenderingAttachmentLocations = (PFN_vkCmdSetRenderingAttachmentLocations)gdpa(device, "vkCmdSetRenderingAttachmentLocations");
    table->CmdSetRenderingInputAttachmentIndices = (PFN_vkCmdSetRenderingInputAttachmentIndices)gdpa(device, "vkCmdSetRenderingInputAttachmentIndices");
    table->CreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)gdpa(device, "vkCreateSwapchainKHR");
    table->DestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)gdpa(device, "vkDestroySwapchainKHR");
    table->GetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)gdpa(device, "vkGetSwapchainImagesKHR");
    table->AcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)gdpa(device, "vkAcquireNextImageKHR");
    table->QueuePresentKHR = (PFN_vkQueuePresentKHR)gdpa(device, "vkQueuePresentKHR");
    table->GetDeviceGroupPresentCapabilitiesKHR = (PFN_vkGetDeviceGroupPresentCapabilitiesKHR)gdpa(device, "vkGetDeviceGroupPresentCapabilitiesKHR");
    table->GetDeviceGroupSurfacePresentModesKHR = (PFN_vkGetDeviceGroupSurfacePresentModesKHR)gdpa(device, "vkGetDeviceGroupSurfacePresentModesKHR");
    table->AcquireNextImage2KHR = (PFN_vkAcquireNextImage2KHR)gdpa(device, "vkAcquireNextImage2KHR");
    table->CreateSharedSwapchainsKHR = (PFN_vkCreateSharedSwapchainsKHR)gdpa(device, "vkCreateSharedSwapchainsKHR");
    table->CreateVideoSessionKHR = (PFN_vkCreateVideoSessionKHR)gdpa(device, "vkCreateVideoSessionKHR");
    table->DestroyVideoSessionKHR = (PFN_vkDestroyVideoSessionKHR)gdpa(device, "vkDestroyVideoSessionKHR");
    table->GetVideoSessionMemoryRequirementsKHR = (PFN_vkGetVideoSessionMemoryRequirementsKHR)gdpa(device, "vkGetVideoSessionMemoryRequirementsKHR");
    table->BindVideoSessionMemoryKHR = (PFN_vkBindVideoSessionMemoryKHR)gdpa(device, "vkBindVideoSessionMemoryKHR");
    table->CreateVideoSessionParametersKHR = (PFN_vkCreateVideoSessionParametersKHR)gdpa(device, "vkCreateVideoSessionParametersKHR");
    table->UpdateVideoSessionParametersKHR = (PFN_vkUpdateVideoSessionParametersKHR)gdpa(device, "vkUpdateVideoSessionParametersKHR");
    table->DestroyVideoSessionParametersKHR = (PFN_vkDestroyVideoSessionParametersKHR)gdpa(device, "vkDestroyVideoSessionParametersKHR");
    table->CmdBeginVideoCodingKHR = (PFN_vkCmdBeginVideoCodingKHR)gdpa(device, "vkCmdBeginVideoCodingKHR");
    table->CmdEndVideoCodingKHR = (PFN_vkCmdEndVideoCodingKHR)gdpa(device, "vkCmdEndVideoCodingKHR");
    table->CmdControlVideoCodingKHR = (PFN_vkCmdControlVideoCodingKHR)gdpa(device, "vkCmdControlVideoCodingKHR");
    table->CmdDecodeVideoKHR = (PFN_vkCmdDecodeVideoKHR)gdpa(device, "vkCmdDecodeVideoKHR");
    table->CmdBeginRenderingKHR = (PFN_vkCmdBeginRenderingKHR)gdpa(device, "vkCmdBeginRenderingKHR");
    table->CmdEndRenderingKHR = (PFN_vkCmdEndRenderingKHR)gdpa(device, "vkCmdEndRenderingKHR");
    table->GetDeviceGroupPeerMemoryFeaturesKHR = (PFN_vkGetDeviceGroupPeerMemoryFeaturesKHR)gdpa(device, "vkGetDeviceGroupPeerMemoryFeaturesKHR");
    table->CmdSetDeviceMaskKHR = (PFN_vkCmdSetDeviceMaskKHR)gdpa(device, "vkCmdSetDeviceMaskKHR");
    table->CmdDispatchBaseKHR = (PFN_vkCmdDispatchBaseKHR)gdpa(device, "vkCmdDispatchBaseKHR");
    table->TrimCommandPoolKHR = (PFN_vkTrimCommandPoolKHR)gdpa(device, "vkTrimCommandPoolKHR");
#ifdef VK_USE_PLATFORM_WIN32_KHR
    table->GetMemoryWin32HandleKHR = (PFN_vkGetMemoryWin32HandleKHR)gdpa(device, "vkGetMemoryWin32HandleKHR");
    table->GetMemoryWin32HandlePropertiesKHR = (PFN_vkGetMemoryWin32HandlePropertiesKHR)gdpa(device, "vkGetMemoryWin32HandlePropertiesKHR");
#endif  // VK_USE_PLATFORM_WIN32_KHR
    table->GetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)gdpa(device, "vkGetMemoryFdKHR");
    table->GetMemoryFdPropertiesKHR = (PFN_vkGetMemoryFdPropertiesKHR)gdpa(device, "vkGetMemoryFdPropertiesKHR");
#ifdef VK_USE_PLATFORM_WIN32_KHR
    table->ImportSemaphoreWin32HandleKHR = (PFN_vkImportSemaphoreWin32HandleKHR)gdpa(device, "vkImportSemaphoreWin32HandleKHR");
    table->GetSemaphoreWin32HandleKHR = (PFN_vkGetSemaphoreWin32HandleKHR)gdpa(device, "vkGetSemaphoreWin32HandleKHR");
#endif  // VK_USE_PLATFORM_WIN32_KHR
    table->ImportSemaphoreFdKHR = (PFN_vkImportSemaphoreFdKHR)gdpa(device, "vkImportSemaphoreFdKHR");
    table->GetSemaphoreFdKHR = (PFN_vkGetSemaphoreFdKHR)gdpa(device, "vkGetSemaphoreFdKHR");
    table->CmdPushDescriptorSetKHR = (PFN_vkCmdPushDescriptorSetKHR)gdpa(device, "vkCmdPushDescriptorSetKHR");
    table->CmdPushDescriptorSetWithTemplateKHR = (PFN_vkCmdPushDescriptorSetWithTemplateKHR)gdpa(device, "vkCmdPushDescriptorSetWithTemplateKHR");
    table->CreateDescriptorUpdateTemplateKHR = (PFN_vkCreateDescriptorUpdateTemplateKHR)gdpa(device, "vkCreateDescriptorUpdateTemplateKHR");
    table->DestroyDescriptorUpdateTemplateKHR = (PFN_vkDestroyDescriptorUpdateTemplateKHR)gdpa(device, "vkDestroyDescriptorUpdateTemplateKHR");
    table->UpdateDescriptorSetWithTemplateKHR = (PFN_vkUpdateDescriptorSetWithTemplateKHR)gdpa(device, "vkUpdateDescriptorSetWithTemplateKHR");
    table->CreateRenderPass2KHR = (PFN_vkCreateRenderPass2KHR)gdpa(device, "vkCreateRenderPass2KHR");
    table->CmdBeginRenderPass2KHR = (PFN_vkCmdBeginRenderPass2KHR)gdpa(device, "vkCmdBeginRenderPass2KHR");
    table->CmdNextSubpass2KHR = (PFN_vkCmdNextSubpass2KHR)gdpa(device, "vkCmdNextSubpass2KHR");
    table->CmdEndRenderPass2KHR = (PFN_vkCmdEndRenderPass2KHR)gdpa(device, "vkCmdEndRenderPass2KHR");
    table->GetSwapchainStatusKHR = (PFN_vkGetSwapchainStatusKHR)gdpa(device, "vkGetSwapchainStatusKHR");
#ifdef VK_USE_PLATFORM_WIN32_KHR
    table->ImportFenceWin32HandleKHR = (PFN_vkImportFenceWin32HandleKHR)gdpa(device, "vkImportFenceWin32HandleKHR");
    table->GetFenceWin32HandleKHR = (PFN_vkGetFenceWin32HandleKHR)gdpa(device, "vkGetFenceWin32HandleKHR");
#endif  // VK_USE_PLATFORM_WIN32_KHR
    table->ImportFenceFdKHR = (PFN_vkImportFenceFdKHR)gdpa(device, "vkImportFenceFdKHR");
    table->GetFenceFdKHR = (PFN_vkGetFenceFdKHR)gdpa(device, "vkGetFenceFdKHR");
    table->AcquireProfilingLockKHR = (PFN_vkAcquireProfilingLockKHR)gdpa(device, "vkAcquireProfilingLockKHR");
    table->ReleaseProfilingLockKHR = (PFN_vkReleaseProfilingLockKHR)gdpa(device, "vkReleaseProfilingLockKHR");
    table->GetImageMemoryRequirements2KHR = (PFN_vkGetImageMemoryRequirements2KHR)gdpa(device, "vkGetImageMemoryRequirements2KHR");
    table->GetBufferMemoryRequirements2KHR = (PFN_vkGetBufferMemoryRequirements2KHR)gdpa(device, "vkGetBufferMemoryRequirements2KHR");
    table->GetImageSparseMemoryRequirements2KHR = (PFN_vkGetImageSparseMemoryRequirements2KHR)gdpa(device, "vkGetImageSparseMemoryRequirements2KHR");
    table->CreateSamplerYcbcrConversionKHR = (PFN_vkCreateSamplerYcbcrConversionKHR)gdpa(device, "vkCreateSamplerYcbcrConversionKHR");
    table->DestroySamplerYcbcrConversionKHR = (PFN_vkDestroySamplerYcbcrConversionKHR)gdpa(device, "vkDestroySamplerYcbcrConversionKHR");
    table->BindBufferMemory2KHR = (PFN_vkBindBufferMemory2KHR)gdpa(device, "vkBindBufferMemory2KHR");
    table->BindImageMemory2KHR = (PFN_vkBindImageMemory2KHR)gdpa(device, "vkBindImageMemory2KHR");
    table->GetDescriptorSetLayoutSupportKHR = (PFN_vkGetDescriptorSetLayoutSupportKHR)gdpa(device, "vkGetDescriptorSetLayoutSupportKHR");
    table->CmdDrawIndirectCountKHR = (PFN_vkCmdDrawIndirectCountKHR)gdpa(device, "vkCmdDrawIndirectCountKHR");
    table->CmdDrawIndexedIndirectCountKHR = (PFN_vkCmdDrawIndexedIndirectCountKHR)gdpa(device, "vkCmdDrawIndexedIndirectCountKHR");
    table->GetSemaphoreCounterValueKHR = (PFN_vkGetSemaphoreCounterValueKHR)gdpa(device, "vkGetSemaphoreCounterValueKHR");
    table->WaitSemaphoresKHR = (PFN_vkWaitSemaphoresKHR)gdpa(device, "vkWaitSemaphoresKHR");
    table->SignalSemaphoreKHR = (PFN_vkSignalSemaphoreKHR)gdpa(device, "vkSignalSemaphoreKHR");
    table->CmdSetFragmentShadingRateKHR = (PFN_vkCmdSetFragmentShadingRateKHR)gdpa(device, "vkCmdSetFragmentShadingRateKHR");
    table->CmdSetRenderingAttachmentLocationsKHR = (PFN_vkCmdSetRenderingAttachmentLocationsKHR)gdpa(device, "vkCmdSetRenderingAttachmentLocationsKHR");
    table->CmdSetRenderingInputAttachmentIndicesKHR = (PFN_vkCmdSetRenderingInputAttachmentIndicesKHR)gdpa(device, "vkCmdSetRenderingInputAttachmentIndicesKHR");
    table->WaitForPresentKHR = (PFN_vkWaitForPresentKHR)gdpa(device, "vkWaitForPresentKHR");
    table->GetBufferDeviceAddressKHR = (PFN_vkGetBufferDeviceAddressKHR)gdpa(device, "vkGetBufferDeviceAddressKHR");
    table->GetBufferOpaqueCaptureAddressKHR = (PFN_vkGetBufferOpaqueCaptureAddressKHR)gdpa(device, "vkGetBufferOpaqueCaptureAddressKHR");
    table->GetDeviceMemoryOpaqueCaptureAddressKHR = (PFN_vkGetDeviceMemoryOpaqueCaptureAddressKHR)gdpa(device, "vkGetDeviceMemoryOpaqueCaptureAddressKHR");
    table->CreateDeferredOperationKHR = (PFN_vkCreateDeferredOperationKHR)gdpa(device, "vkCreateDeferredOperationKHR");
    table->DestroyDeferredOperationKHR = (PFN_vkDestroyDeferredOperationKHR)gdpa(device, "vkDestroyDeferredOperationKHR");
    table->GetDeferredOperationMaxConcurrencyKHR = (PFN_vkGetDeferredOperationMaxConcurrencyKHR)gdpa(device, "vkGetDeferredOperationMaxConcurrencyKHR");
    table->GetDeferredOperationResultKHR = (PFN_vkGetDeferredOperationResultKHR)gdpa(device, "vkGetDeferredOperationResultKHR");
    table->DeferredOperationJoinKHR = (PFN_vkDeferredOperationJoinKHR)gdpa(device, "vkDeferredOperationJoinKHR");
    table->GetPipelineExecutablePropertiesKHR = (PFN_vkGetPipelineExecutablePropertiesKHR)gdpa(device, "vkGetPipelineExecutablePropertiesKHR");
    table->GetPipelineExecutableStatisticsKHR = (PFN_vkGetPipelineExecutableStatisticsKHR)gdpa(device, "vkGetPipelineExecutableStatisticsKHR");
    table->GetPipelineExecutableInternalRepresentationsKHR = (PFN_vkGetPipelineExecutableInternalRepresentationsKHR)gdpa(device, "vkGetPipelineExecutableInternalRepresentationsKHR");
    table->MapMemory2KHR = (PFN_vkMapMemory2KHR)gdpa(device, "vkMapMemory2KHR");
    table->UnmapMemory2KHR = (PFN_vkUnmapMemory2KHR)gdpa(device, "vkUnmapMemory2KHR");
    table->GetEncodedVideoSessionParametersKHR = (PFN_vkGetEncodedVideoSessionParametersKHR)gdpa(device, "vkGetEncodedVideoSessionParametersKHR");
    table->CmdEncodeVideoKHR = (PFN_vkCmdEncodeVideoKHR)gdpa(device, "vkCmdEncodeVideoKHR");
    table->CmdSetEvent2KHR = (PFN_vkCmdSetEvent2KHR)gdpa(device, "vkCmdSetEvent2KHR");
    table->CmdResetEvent2KHR = (PFN_vkCmdResetEvent2KHR)gdpa(device, "vkCmdResetEvent2KHR");
    table->CmdWaitEvents2KHR = (PFN_vkCmdWaitEvents2KHR)gdpa(device, "vkCmdWaitEvents2KHR");
    table->CmdPipelineBarrier2KHR = (PFN_vkCmdPipelineBarrier2KHR)gdpa(device, "vkCmdPipelineBarrier2KHR");
    table->CmdWriteTimestamp2KHR = (PFN_vkCmdWriteTimestamp2KHR)gdpa(device, "vkCmdWriteTimestamp2KHR");
    table->QueueSubmit2KHR = (PFN_vkQueueSubmit2KHR)gdpa(device, "vkQueueSubmit2KHR");
    table->CmdBindIndexBuffer3KHR = (PFN_vkCmdBindIndexBuffer3KHR)gdpa(device, "vkCmdBindIndexBuffer3KHR");
    table->CmdBindVertexBuffers3KHR = (PFN_vkCmdBindVertexBuffers3KHR)gdpa(device, "vkCmdBindVertexBuffers3KHR");
    table->CmdDrawIndirect2KHR = (PFN_vkCmdDrawIndirect2KHR)gdpa(device, "vkCmdDrawIndirect2KHR");
    table->CmdDrawIndexedIndirect2KHR = (PFN_vkCmdDrawIndexedIndirect2KHR)gdpa(device, "vkCmdDrawIndexedIndirect2KHR");
    table->CmdDispatchIndirect2KHR = (PFN_vkCmdDispatchIndirect2KHR)gdpa(device, "vkCmdDispatchIndirect2KHR");
    table->CmdCopyMemoryKHR = (PFN_vkCmdCopyMemoryKHR)gdpa(device, "vkCmdCopyMemoryKHR");
    table->CmdCopyMemoryToImageKHR = (PFN_vkCmdCopyMemoryToImageKHR)gdpa(device, "vkCmdCopyMemoryToImageKHR");
    table->CmdCopyImageToMemoryKHR = (PFN_vkCmdCopyImageToMemoryKHR)gdpa(device, "vkCmdCopyImageToMemoryKHR");
    table->CmdUpdateMemoryKHR = (PFN_vkCmdUpdateMemoryKHR)gdpa(device, "vkCmdUpdateMemoryKHR");
    table->CmdFillMemoryKHR = (PFN_vkCmdFillMemoryKHR)gdpa(device, "vkCmdFillMemoryKHR");
    table->CmdCopyQueryPoolResultsToMemoryKHR = (PFN_vkCmdCopyQueryPoolResultsToMemoryKHR)gdpa(device, "vkCmdCopyQueryPoolResultsToMemoryKHR");
    table->CmdDrawIndirectCount2KHR = (PFN_vkCmdDrawIndirectCount2KHR)gdpa(device, "vkCmdDrawIndirectCount2KHR");
    table->CmdDrawIndexedIndirectCount2KHR = (PFN_vkCmdDrawIndexedIndirectCount2KHR)gdpa(device, "vkCmdDrawIndexedIndirectCount2KHR");
    table->CmdBeginConditionalRendering2EXT = (PFN_vkCmdBeginConditionalRendering2EXT)gdpa(device, "vkCmdBeginConditionalRendering2EXT");
    table->CmdBindTransformFeedbackBuffers2EXT = (PFN_vkCmdBindTransformFeedbackBuffers2EXT)gdpa(device, "vkCmdBindTransformFeedbackBuffers2EXT");
    table->CmdBeginTransformFeedback2EXT = (PFN_vkCmdBeginTransformFeedback2EXT)gdpa(device, "vkCmdBeginTransformFeedback2EXT");
    table->CmdEndTransformFeedback2EXT = (PFN_vkCmdEndTransformFeedback2EXT)gdpa(device, "vkCmdEndTransformFeedback2EXT");
    table->CmdDrawIndirectByteCount2EXT = (PFN_vkCmdDrawIndirectByteCount2EXT)gdpa(device, "vkCmdDrawIndirectByteCount2EXT");
    table->CmdDrawMeshTasksIndirect2EXT = (PFN_vkCmdDrawMeshTasksIndirect2EXT)gdpa(device, "vkCmdDrawMeshTasksIndirect2EXT");
    table->CmdDrawMeshTasksIndirectCount2EXT = (PFN_vkCmdDrawMeshTasksIndirectCount2EXT)gdpa(device, "vkCmdDrawMeshTasksIndirectCount2EXT");
    table->CmdWriteMarkerToMemoryAMD = (PFN_vkCmdWriteMarkerToMemoryAMD)gdpa(device, "vkCmdWriteMarkerToMemoryAMD");
    table->CreateAccelerationStructure2KHR = (PFN_vkCreateAccelerationStructure2KHR)gdpa(device, "vkCreateAccelerationStructure2KHR");
    table->CmdCopyBuffer2KHR = (PFN_vkCmdCopyBuffer2KHR)gdpa(device, "vkCmdCopyBuffer2KHR");
    table->CmdCopyImage2KHR = (PFN_vkCmdCopyImage2KHR)gdpa(device, "vkCmdCopyImage2KHR");
    table->CmdCopyBufferToImage2KHR = (PFN_vkCmdCopyBufferToImage2KHR)gdpa(device, "vkCmdCopyBufferToImage2KHR");
    table->CmdCopyImageToBuffer2KHR = (PFN_vkCmdCopyImageToBuffer2KHR)gdpa(device, "vkCmdCopyImageToBuffer2KHR");
    table->CmdBlitImage2KHR = (PFN_vkCmdBlitImage2KHR)gdpa(device, "vkCmdBlitImage2KHR");
    table->CmdResolveImage2KHR = (PFN_vkCmdResolveImage2KHR)gdpa(device, "vkCmdResolveImage2KHR");
    table->CmdTraceRaysIndirect2KHR = (PFN_vkCmdTraceRaysIndirect2KHR)gdpa(device, "vkCmdTraceRaysIndirect2KHR");
    table->GetDeviceBufferMemoryRequirementsKHR = (PFN_vkGetDeviceBufferMemoryRequirementsKHR)gdpa(device, "vkGetDeviceBufferMemoryRequirementsKHR");
    table->GetDeviceImageMemoryRequirementsKHR = (PFN_vkGetDeviceImageMemoryRequirementsKHR)gdpa(device, "vkGetDeviceImageMemoryRequirementsKHR");
    table->GetDeviceImageSparseMemoryRequirementsKHR = (PFN_vkGetDeviceImageSparseMemoryRequirementsKHR)gdpa(device, "vkGetDeviceImageSparseMemoryRequirementsKHR");
    table->CmdBindIndexBuffer2KHR = (PFN_vkCmdBindIndexBuffer2KHR)gdpa(device, "vkCmdBindIndexBuffer2KHR");
    table->GetRenderingAreaGranularityKHR = (PFN_vkGetRenderingAreaGranularityKHR)gdpa(device, "vkGetRenderingAreaGranularityKHR");
    table->GetDeviceImageSubresourceLayoutKHR = (PFN_vkGetDeviceImageSubresourceLayoutKHR)gdpa(device, "vkGetDeviceImageSubresourceLayoutKHR");
    table->GetImageSubresourceLayout2KHR = (PFN_vkGetImageSubresourceLayout2KHR)gdpa(device, "vkGetImageSubresourceLayout2KHR");
    table->WaitForPresent2KHR = (PFN_vkWaitForPresent2KHR)gdpa(device, "vkWaitForPresent2KHR");
    table->CreatePipelineBinariesKHR = (PFN_vkCreatePipelineBinariesKHR)gdpa(device, "vkCreatePipelineBinariesKHR");
    table->DestroyPipelineBinaryKHR = (PFN_vkDestroyPipelineBinaryKHR)gdpa(device, "vkDestroyPipelineBinaryKHR");
    table->GetPipelineKeyKHR = (PFN_vkGetPipelineKeyKHR)gdpa(device, "vkGetPipelineKeyKHR");
    table->GetPipelineBinaryDataKHR = (PFN_vkGetPipelineBinaryDataKHR)gdpa(device, "vkGetPipelineBinaryDataKHR");
    table->ReleaseCapturedPipelineDataKHR = (PFN_vkReleaseCapturedPipelineDataKHR)gdpa(device, "vkReleaseCapturedPipelineDataKHR");
    table->ReleaseSwapchainImagesKHR = (PFN_vkReleaseSwapchainImagesKHR)gdpa(device, "vkReleaseSwapchainImagesKHR");
    table->CmdSetLineStippleKHR = (PFN_vkCmdSetLineStippleKHR)gdpa(device, "vkCmdSetLineStippleKHR");
    table->GetCalibratedTimestampsKHR = (PFN_vkGetCalibratedTimestampsKHR)gdpa(device, "vkGetCalibratedTimestampsKHR");
    table->CmdBindDescriptorSets2KHR = (PFN_vkCmdBindDescriptorSets2KHR)gdpa(device, "vkCmdBindDescriptorSets2KHR");
    table->CmdPushConstants2KHR = (PFN_vkCmdPushConstants2KHR)gdpa(device, "vkCmdPushConstants2KHR");
    table->CmdPushDescriptorSet2KHR = (PFN_vkCmdPushDescriptorSet2KHR)gdpa(device, "vkCmdPushDescriptorSet2KHR");
    table->CmdPushDescriptorSetWithTemplate2KHR = (PFN_vkCmdPushDescriptorSetWithTemplate2KHR)gdpa(device, "vkCmdPushDescriptorSetWithTemplate2KHR");
    table->CmdSetDescriptorBufferOffsets2EXT = (PFN_vkCmdSetDescriptorBufferOffsets2EXT)gdpa(device, "vkCmdSetDescriptorBufferOffsets2EXT");
    table->CmdBindDescriptorBufferEmbeddedSamplers2EXT = (PFN_vkCmdBindDescriptorBufferEmbeddedSamplers2EXT)gdpa(device, "vkCmdBindDescriptorBufferEmbeddedSamplers2EXT");
    table->CmdCopyMemoryIndirectKHR = (PFN_vkCmdCopyMemoryIndirectKHR)gdpa(device, "vkCmdCopyMemoryIndirectKHR");
    table->CmdCopyMemoryToImageIndirectKHR = (PFN_vkCmdCopyMemoryToImageIndirectKHR)gdpa(device, "vkCmdCopyMemoryToImageIndirectKHR");
    table->GetDeviceFaultReportsKHR = (PFN_vkGetDeviceFaultReportsKHR)gdpa(device, "vkGetDeviceFaultReportsKHR");
    table->GetDeviceFaultDebugInfoKHR = (PFN_vkGetDeviceFaultDebugInfoKHR)gdpa(device, "vkGetDeviceFaultDebugInfoKHR");
    table->CmdEndRendering2KHR = (PFN_vkCmdEndRendering2KHR)gdpa(device, "vkCmdEndRendering2KHR");
    table->DebugMarkerSetObjectTagEXT = (PFN_vkDebugMarkerSetObjectTagEXT)gdpa(device, "vkDebugMarkerSetObjectTagEXT");
    table->DebugMarkerSetObjectNameEXT = (PFN_vkDebugMarkerSetObjectNameEXT)gdpa(device, "vkDebugMarkerSetObjectNameEXT");
    table->CmdDebugMarkerBeginEXT = (PFN_vkCmdDebugMarkerBeginEXT)gdpa(device, "vkCmdDebugMarkerBeginEXT");
    table->CmdDebugMarkerEndEXT = (PFN_vkCmdDebugMarkerEndEXT)gdpa(device, "vkCmdDebugMarkerEndEXT");
    table->CmdDebugMarkerInsertEXT = (PFN_vkCmdDebugMarkerInsertEXT)gdpa(device, "vkCmdDebugMarkerInsertEXT");
    table->CmdBindTransformFeedbackBuffersEXT = (PFN_vkCmdBindTransformFeedbackBuffersEXT)gdpa(device, "vkCmdBindTransformFeedbackBuffersEXT");
    table->CmdBeginTransformFeedbackEXT = (PFN_vkCmdBeginTransformFeedbackEXT)gdpa(device, "vkCmdBeginTransformFeedbackEXT");
    table->CmdEndTransformFeedbackEXT = (PFN_vkCmdEndTransformFeedbackEXT)gdpa(device, "vkCmdEndTransformFeedbackEXT");
    table->CmdBeginQueryIndexedEXT = (PFN_vkCmdBeginQueryIndexedEXT)gdpa(device, "vkCmdBeginQueryIndexedEXT");
    table->CmdEndQueryIndexedEXT = (PFN_vkCmdEndQueryIndexedEXT)gdpa(device, "vkCmdEndQueryIndexedEXT");
    table->CmdDrawIndirectByteCountEXT = (PFN_vkCmdDrawIndirectByteCountEXT)gdpa(device, "vkCmdDrawIndirectByteCountEXT");
    table->CreateCuModuleNVX = (PFN_vkCreateCuModuleNVX)gdpa(device, "vkCreateCuModuleNVX");
    table->CreateCuFunctionNVX = (PFN_vkCreateCuFunctionNVX)gdpa(device, "vkCreateCuFunctionNVX");
    table->DestroyCuModuleNVX = (PFN_vkDestroyCuModuleNVX)gdpa(device, "vkDestroyCuModuleNVX");
    table->DestroyCuFunctionNVX = (PFN_vkDestroyCuFunctionNVX)gdpa(device, "vkDestroyCuFunctionNVX");
    table->CmdCuLaunchKernelNVX = (PFN_vkCmdCuLaunchKernelNVX)gdpa(device, "vkCmdCuLaunchKernelNVX");
    table->GetImageViewHandleNVX = (PFN_vkGetImageViewHandleNVX)gdpa(device, "vkGetImageViewHandleNVX");
    table->GetImageViewHandle64NVX = (PFN_vkGetImageViewHandle64NVX)gdpa(device, "vkGetImageViewHandle64NVX");
    table->GetImageViewAddressNVX = (PFN_vkGetImageViewAddressNVX)gdpa(device, "vkGetImageViewAddressNVX");
    table->GetDeviceCombinedImageSamplerIndexNVX = (PFN_vkGetDeviceCombinedImageSamplerIndexNVX)gdpa(device, "vkGetDeviceCombinedImageSamplerIndexNVX");
    table->CmdDrawIndirectCountAMD = (PFN_vkCmdDrawIndirectCountAMD)gdpa(device, "vkCmdDrawIndirectCountAMD");
    table->CmdDrawIndexedIndirectCountAMD = (PFN_vkCmdDrawIndexedIndirectCountAMD)gdpa(device, "vkCmdDrawIndexedIndirectCountAMD");
    table->GetShaderInfoAMD = (PFN_vkGetShaderInfoAMD)gdpa(device, "vkGetShaderInfoAMD");
#ifdef VK_USE_PLATFORM_WIN32_KHR
    table->GetMemoryWin32HandleNV = (PFN_vkGetMemoryWin32HandleNV)gdpa(device, "vkGetMemoryWin32HandleNV");
#endif  // VK_USE_PLATFORM_WIN32_KHR
    table->CmdBeginConditionalRenderingEXT = (PFN_vkCmdBeginConditionalRenderingEXT)gdpa(device, "vkCmdBeginConditionalRenderingEXT");
    table->CmdEndConditionalRenderingEXT = (PFN_vkCmdEndConditionalRenderingEXT)gdpa(device, "vkCmdEndConditionalRenderingEXT");
    table->CmdSetViewportWScalingNV = (PFN_vkCmdSetViewportWScalingNV)gdpa(device, "vkCmdSetViewportWScalingNV");
    table->DisplayPowerControlEXT = (PFN_vkDisplayPowerControlEXT)gdpa(device, "vkDisplayPowerControlEXT");
    table->RegisterDeviceEventEXT = (PFN_vkRegisterDeviceEventEXT)gdpa(device, "vkRegisterDeviceEventEXT");
    table->RegisterDisplayEventEXT = (PFN_vkRegisterDisplayEventEXT)gdpa(device, "vkRegisterDisplayEventEXT");
    table->GetSwapchainCounterEXT = (PFN_vkGetSwapchainCounterEXT)gdpa(device, "vkGetSwapchainCounterEXT");
    table->GetRefreshCycleDurationGOOGLE = (PFN_vkGetRefreshCycleDurationGOOGLE)gdpa(device, "vkGetRefreshCycleDurationGOOGLE");
    table->GetPastPresentationTimingGOOGLE = (PFN_vkGetPastPresentationTimingGOOGLE)gdpa(device, "vkGetPastPresentationTimingGOOGLE");
    table->CmdSetDiscardRectangleEXT = (PFN_vkCmdSetDiscardRectangleEXT)gdpa(device, "vkCmdSetDiscardRectangleEXT");
    table->CmdSetDiscardRectangleEnableEXT = (PFN_vkCmdSetDiscardRectangleEnableEXT)gdpa(device, "vkCmdSetDiscardRectangleEnableEXT");
    table->CmdSetDiscardRectangleModeEXT = (PFN_vkCmdSetDiscardRectangleModeEXT)gdpa(device, "vkCmdSetDiscardRectangleModeEXT");
    table->SetHdrMetadataEXT = (PFN_vkSetHdrMetadataEXT)gdpa(device, "vkSetHdrMetadataEXT");
    table->SetDebugUtilsObjectNameEXT = (PFN_vkSetDebugUtilsObjectNameEXT)gdpa(device, "vkSetDebugUtilsObjectNameEXT");
    table->SetDebugUtilsObjectTagEXT = (PFN_vkSetDebugUtilsObjectTagEXT)gdpa(device, "vkSetDebugUtilsObjectTagEXT");
    table->QueueBeginDebugUtilsLabelEXT = (PFN_vkQueueBeginDebugUtilsLabelEXT)gdpa(device, "vkQueueBeginDebugUtilsLabelEXT");
    table->QueueEndDebugUtilsLabelEXT = (PFN_vkQueueEndDebugUtilsLabelEXT)gdpa(device, "vkQueueEndDebugUtilsLabelEXT");
    table->QueueInsertDebugUtilsLabelEXT = (PFN_vkQueueInsertDebugUtilsLabelEXT)gdpa(device, "vkQueueInsertDebugUtilsLabelEXT");
    table->CmdBeginDebugUtilsLabelEXT = (PFN_vkCmdBeginDebugUtilsLabelEXT)gdpa(device, "vkCmdBeginDebugUtilsLabelEXT");
    table->CmdEndDebugUtilsLabelEXT = (PFN_vkCmdEndDebugUtilsLabelEXT)gdpa(device, "vkCmdEndDebugUtilsLabelEXT");
    table->CmdInsertDebugUtilsLabelEXT = (PFN_vkCmdInsertDebugUtilsLabelEXT)gdpa(device, "vkCmdInsertDebugUtilsLabelEXT");
#ifdef VK_USE_PLATFORM_ANDROID_KHR
    table->GetAndroidHardwareBufferPropertiesANDROID = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)gdpa(device, "vkGetAndroidHardwareBufferPropertiesANDROID");
    table->GetMemoryAndroidHardwareBufferANDROID = (PFN_vkGetMemoryAndroidHardwareBufferANDROID)gdpa(device, "vkGetMemoryAndroidHardwareBufferANDROID");
#endif  // VK_USE_PLATFORM_ANDROID_KHR
    table->CreateGpaSessionAMD = (PFN_vkCreateGpaSessionAMD)gdpa(device, "vkCreateGpaSessionAMD");
    table->DestroyGpaSessionAMD = (PFN_vkDestroyGpaSessionAMD)gdpa(device, "vkDestroyGpaSessionAMD");
    table->SetGpaDeviceClockModeAMD = (PFN_vkSetGpaDeviceClockModeAMD)gdpa(device, "vkSetGpaDeviceClockModeAMD");
    table->GetGpaDeviceClockInfoAMD = (PFN_vkGetGpaDeviceClockInfoAMD)gdpa(device, "vkGetGpaDeviceClockInfoAMD");
    table->CmdBeginGpaSessionAMD = (PFN_vkCmdBeginGpaSessionAMD)gdpa(device, "vkCmdBeginGpaSessionAMD");
    table->CmdEndGpaSessionAMD = (PFN_vkCmdEndGpaSessionAMD)gdpa(device, "vkCmdEndGpaSessionAMD");
    table->CmdBeginGpaSampleAMD = (PFN_vkCmdBeginGpaSampleAMD)gdpa(device, "vkCmdBeginGpaSampleAMD");
    table->CmdEndGpaSampleAMD = (PFN_vkCmdEndGpaSampleAMD)gdpa(device, "vkCmdEndGpaSampleAMD");
    table->GetGpaSessionStatusAMD = (PFN_vkGetGpaSessionStatusAMD)gdpa(device, "vkGetGpaSessionStatusAMD");
    table->GetGpaSessionResultsAMD = (PFN_vkGetGpaSessionResultsAMD)gdpa(device, "vkGetGpaSessionResultsAMD");
    table->ResetGpaSessionAMD = (PFN_vkResetGpaSessionAMD)gdpa(device, "vkResetGpaSessionAMD");
    table->CmdCopyGpaSessionResultsAMD = (PFN_vkCmdCopyGpaSessionResultsAMD)gdpa(device, "vkCmdCopyGpaSessionResultsAMD");
#ifdef VK_ENABLE_BETA_EXTENSIONS
    table->CreateExecutionGraphPipelinesAMDX = (PFN_vkCreateExecutionGraphPipelinesAMDX)gdpa(device, "vkCreateExecutionGraphPipelinesAMDX");
    table->GetExecutionGraphPipelineScratchSizeAMDX = (PFN_vkGetExecutionGraphPipelineScratchSizeAMDX)gdpa(device, "vkGetExecutionGraphPipelineScratchSizeAMDX");
    table->GetExecutionGraphPipelineNodeIndexAMDX = (PFN_vkGetExecutionGraphPipelineNodeIndexAMDX)gdpa(device, "vkGetExecutionGraphPipelineNodeIndexAMDX");
    table->CmdInitializeGraphScratchMemoryAMDX = (PFN_vkCmdInitializeGraphScratchMemoryAMDX)gdpa(device, "vkCmdInitializeGraphScratchMemoryAMDX");
    table->CmdDispatchGraphAMDX = (PFN_vkCmdDispatchGraphAMDX)gdpa(device, "vkCmdDispatchGraphAMDX");
    table->CmdDispatchGraphIndirectAMDX = (PFN_vkCmdDispatchGraphIndirectAMDX)gdpa(device, "vkCmdDispatchGraphIndirectAMDX");
    table->CmdDispatchGraphIndirectCountAMDX = (PFN_vkCmdDispatchGraphIndirectCountAMDX)gdpa(device, "vkCmdDispatchGraphIndirectCountAMDX");
#endif  // VK_ENABLE_BETA_EXTENSIONS
    table->WriteSamplerDescriptorsEXT = (PFN_vkWriteSamplerDescriptorsEXT)gdpa(device, "vkWriteSamplerDescriptorsEXT");
    table->WriteResourceDescriptorsEXT = (PFN_vkWriteResourceDescriptorsEXT)gdpa(device, "vkWriteResourceDescriptorsEXT");
    table->CmdBindSamplerHeapEXT = (PFN_vkCmdBindSamplerHeapEXT)gdpa(device, "vkCmdBindSamplerHeapEXT");
    table->CmdBindResourceHeapEXT = (PFN_vkCmdBindResourceHeapEXT)gdpa(device, "vkCmdBindResourceHeapEXT");
    table->CmdPushDataEXT = (PFN_vkCmdPushDataEXT)gdpa(device, "vkCmdPushDataEXT");
    table->GetImageOpaqueCaptureDataEXT = (PFN_vkGetImageOpaqueCaptureDataEXT)gdpa(device, "vkGetImageOpaqueCaptureDataEXT");
    table->RegisterCustomBorderColorEXT = (PFN_vkRegisterCustomBorderColorEXT)gdpa(device, "vkRegisterCustomBorderColorEXT");
    table->UnregisterCustomBorderColorEXT = (PFN_vkUnregisterCustomBorderColorEXT)gdpa(device, "vkUnregisterCustomBorderColorEXT");
    table->GetTensorOpaqueCaptureDataARM = (PFN_vkGetTensorOpaqueCaptureDataARM)gdpa(device, "vkGetTensorOpaqueCaptureDataARM");
    table->CmdSetSampleLocationsEXT = (PFN_vkCmdSetSampleLocationsEXT)gdpa(device, "vkCmdSetSampleLocationsEXT");
    table->GetImageDrmFormatModifierPropertiesEXT = (PFN_vkGetImageDrmFormatModifierPropertiesEXT)gdpa(device, "vkGetImageDrmFormatModifierPropertiesEXT");
    table->CreateValidationCacheEXT = (PFN_vkCreateValidationCacheEXT)gdpa(device, "vkCreateValidationCacheEXT");
    table->DestroyValidationCacheEXT = (PFN_vkDestroyValidationCacheEXT)gdpa(device, "vkDestroyValidationCacheEXT");
    table->MergeValidationCachesEXT = (PFN_vkMergeValidationCachesEXT)gdpa(device, "vkMergeValidationCachesEXT");
    table->GetValidationCacheDataEXT = (PFN_vkGetValidationCacheDataEXT)gdpa(device, "vkGetValidationCacheDataEXT");
    table->CmdBindShadingRateImageNV = (PFN_vkCmdBindShadingRateImageNV)gdpa(device, "vkCmdBindShadingRateImageNV");
    table->CmdSetViewportShadingRatePaletteNV = (PFN_vkCmdSetViewportShadingRatePaletteNV)gdpa(device, "vkCmdSetViewportShadingRatePaletteNV");
    table->CmdSetCoarseSampleOrderNV = (PFN_vkCmdSetCoarseSampleOrderNV)gdpa(device, "vkCmdSetCoarseSampleOrderNV");
    table->CreateAccelerationStructureNV = (PFN_vkCreateAccelerationStructureNV)gdpa(device, "vkCreateAccelerationStructureNV");
    table->DestroyAccelerationStructureNV = (PFN_vkDestroyAccelerationStructureNV)gdpa(device, "vkDestroyAccelerationStructureNV");
    table->GetAccelerationStructureMemoryRequirementsNV = (PFN_vkGetAccelerationStructureMemoryRequirementsNV)gdpa(device, "vkGetAccelerationStructureMemoryRequirementsNV");
    table->BindAccelerationStructureMemoryNV = (PFN_vkBindAccelerationStructureMemoryNV)gdpa(device, "vkBindAccelerationStructureMemoryNV");
    table->CmdBuildAccelerationStructureNV = (PFN_vkCmdBuildAccelerationStructureNV)gdpa(device, "vkCmdBuildAccelerationStructureNV");
    table->CmdCopyAccelerationStructureNV = (PFN_vkCmdCopyAccelerationStructureNV)gdpa(device, "vkCmdCopyAccelerationStructureNV");
    table->CmdTraceRaysNV = (PFN_vkCmdTraceRaysNV)gdpa(device, "vkCmdTraceRaysNV");
    table->CreateRayTracingPipelinesNV = (PFN_vkCreateRayTracingPipelinesNV)gdpa(device, "vkCreateRayTracingPipelinesNV");
    table->GetRayTracingShaderGroupHandlesKHR = (PFN_vkGetRayTracingShaderGroupHandlesKHR)gdpa(device, "vkGetRayTracingShaderGroupHandlesKHR");
    table->GetRayTracingShaderGroupHandlesNV = (PFN_vkGetRayTracingShaderGroupHandlesNV)gdpa(device, "vkGetRayTracingShaderGroupHandlesNV");
    table->GetAccelerationStructureHandleNV = (PFN_vkGetAccelerationStructureHandleNV)gdpa(device, "vkGetAccelerationStructureHandleNV");
    table->CmdWriteAccelerationStructuresPropertiesNV = (PFN_vkCmdWriteAccelerationStructuresPropertiesNV)gdpa(device, "vkCmdWriteAccelerationStructuresPropertiesNV");
    table->CompileDeferredNV = (PFN_vkCompileDeferredNV)gdpa(device, "vkCompileDeferredNV");
    table->GetMemoryHostPointerPropertiesEXT = (PFN_vkGetMemoryHostPointerPropertiesEXT)gdpa(device, "vkGetMemoryHostPointerPropertiesEXT");
    table->CmdWriteBufferMarkerAMD = (PFN_vkCmdWriteBufferMarkerAMD)gdpa(device, "vkCmdWriteBufferMarkerAMD");
    table->CmdWriteBufferMarker2AMD = (PFN_vkCmdWriteBufferMarker2AMD)gdpa(device, "vkCmdWriteBufferMarker2AMD");
    table->GetCalibratedTimestampsEXT = (PFN_vkGetCalibratedTimestampsEXT)gdpa(device, "vkGetCalibratedTimestampsEXT");
    table->CmdDrawMeshTasksNV = (PFN_vkCmdDrawMeshTasksNV)gdpa(device, "vkCmdDrawMeshTasksNV");
    table->CmdDrawMeshTasksIndirectNV = (PFN_vkCmdDrawMeshTasksIndirectNV)gdpa(device, "vkCmdDrawMeshTasksIndirectNV");
    table->CmdDrawMeshTasksIndirectCountNV = (PFN_vkCmdDrawMeshTasksIndirectCountNV)gdpa(device, "vkCmdDrawMeshTasksIndirectCountNV");
    table->CmdSetExclusiveScissorEnableNV = (PFN_vkCmdSetExclusiveScissorEnableNV)gdpa(device, "vkCmdSetExclusiveScissorEnableNV");
    table->CmdSetExclusiveScissorNV = (PFN_vkCmdSetExclusiveScissorNV)gdpa(device, "vkCmdSetExclusiveScissorNV");
    table->CmdSetCheckpointNV = (PFN_vkCmdSetCheckpointNV)gdpa(device, "vkCmdSetCheckpointNV");
    table->GetQueueCheckpointDataNV = (PFN_vkGetQueueCheckpointDataNV)gdpa(device, "vkGetQueueCheckpointDataNV");
    table->GetQueueCheckpointData2NV = (PFN_vkGetQueueCheckpointData2NV)gdpa(device, "vkGetQueueCheckpointData2NV");
    table->SetSwapchainPresentTimingQueueSizeEXT = (PFN_vkSetSwapchainPresentTimingQueueSizeEXT)gdpa(device, "vkSetSwapchainPresentTimingQueueSizeEXT");
    table->GetSwapchainTimingPropertiesEXT = (PFN_vkGetSwapchainTimingPropertiesEXT)gdpa(device, "vkGetSwapchainTimingPropertiesEXT");
    table->GetSwapchainTimeDomainPropertiesEXT = (PFN_vkGetSwapchainTimeDomainPropertiesEXT)gdpa(device, "vkGetSwapchainTimeDomainPropertiesEXT");
    table->GetPastPresentationTimingEXT = (PFN_vkGetPastPresentationTimingEXT)gdpa(device, "vkGetPastPresentationTimingEXT");
    table->InitializePerformanceApiINTEL = (PFN_vkInitializePerformanceApiINTEL)gdpa(device, "vkInitializePerformanceApiINTEL");
    table->UninitializePerformanceApiINTEL = (PFN_vkUninitializePerformanceApiINTEL)gdpa(device, "vkUninitializePerformanceApiINTEL");
    table->CmdSetPerformanceMarkerINTEL = (PFN_vkCmdSetPerformanceMarkerINTEL)gdpa(device, "vkCmdSetPerformanceMarkerINTEL");
    table->CmdSetPerformanceStreamMarkerINTEL = (PFN_vkCmdSetPerformanceStreamMarkerINTEL)gdpa(device, "vkCmdSetPerformanceStreamMarkerINTEL");
    table->CmdSetPerformanceOverrideINTEL = (PFN_vkCmdSetPerformanceOverrideINTEL)gdpa(device, "vkCmdSetPerformanceOverrideINTEL");
    table->AcquirePerformanceConfigurationINTEL = (PFN_vkAcquirePerformanceConfigurationINTEL)gdpa(device, "vkAcquirePerformanceConfigurationINTEL");
    table->ReleasePerformanceConfigurationINTEL = (PFN_vkReleasePerformanceConfigurationINTEL)gdpa(device, "vkReleasePerformanceConfigurationINTEL");
    table->QueueSetPerformanceConfigurationINTEL = (PFN_vkQueueSetPerformanceConfigurationINTEL)gdpa(device, "vkQueueSetPerformanceConfigurationINTEL");
    table->GetPerformanceParameterINTEL = (PFN_vkGetPerformanceParameterINTEL)gdpa(device, "vkGetPerformanceParameterINTEL");
    table->SetLocalDimmingAMD = (PFN_vkSetLocalDimmingAMD)gdpa(device, "vkSetLocalDimmingAMD");
    table->GetBufferDeviceAddressEXT = (PFN_vkGetBufferDeviceAddressEXT)gdpa(device, "vkGetBufferDeviceAddressEXT");
#ifdef VK_USE_PLATFORM_WIN32_KHR
    table->AcquireFullScreenExclusiveModeEXT = (PFN_vkAcquireFullScreenExclusiveModeEXT)gdpa(device, "vkAcquireFullScreenExclusiveModeEXT");
    table->ReleaseFullScreenExclusiveModeEXT = (PFN_vkReleaseFullScreenExclusiveModeEXT)gdpa(device, "vkReleaseFullScreenExclusiveModeEXT");
    table->GetDeviceGroupSurfacePresentModes2EXT = (PFN_vkGetDeviceGroupSurfacePresentModes2EXT)gdpa(device, "vkGetDeviceGroupSurfacePresentModes2EXT");
#endif  // VK_USE_PLATFORM_WIN32_KHR
    table->CmdSetLineStippleEXT = (PFN_vkCmdSetLineStippleEXT)gdpa(device, "vkCmdSetLineStippleEXT");
    table->ResetQueryPoolEXT = (PFN_vkResetQueryPoolEXT)gdpa(device, "vkResetQueryPoolEXT");
    table->CmdSetCullModeEXT = (PFN_vkCmdSetCullModeEXT)gdpa(device, "vkCmdSetCullModeEXT");
    table->CmdSetFrontFaceEXT = (PFN_vkCmdSetFrontFaceEXT)gdpa(device, "vkCmdSetFrontFaceEXT");
    table->CmdSetPrimitiveTopologyEXT = (PFN_vkCmdSetPrimitiveTopologyEXT)gdpa(device, "vkCmdSetPrimitiveTopologyEXT");
    table->CmdSetViewportWithCountEXT = (PFN_vkCmdSetViewportWithCountEXT)gdpa(device, "vkCmdSetViewportWithCountEXT");
    table->CmdSetScissorWithCountEXT = (PFN_vkCmdSetScissorWithCountEXT)gdpa(device, "vkCmdSetScissorWithCountEXT");
    table->CmdBindVertexBuffers2EXT = (PFN_vkCmdBindVertexBuffers2EXT)gdpa(device, "vkCmdBindVertexBuffers2EXT");
    table->CmdSetDepthTestEnableEXT = (PFN_vkCmdSetDepthTestEnableEXT)gdpa(device, "vkCmdSetDepthTestEnableEXT");
    table->CmdSetDepthWriteEnableEXT = (PFN_vkCmdSetDepthWriteEnableEXT)gdpa(device, "vkCmdSetDepthWriteEnableEXT");
    table->CmdSetDepthCompareOpEXT = (PFN_vkCmdSetDepthCompareOpEXT)gdpa(device, "vkCmdSetDepthCompareOpEXT");
    table->CmdSetDepthBoundsTestEnableEXT = (PFN_vkCmdSetDepthBoundsTestEnableEXT)gdpa(device, "vkCmdSetDepthBoundsTestEnableEXT");
    table->CmdSetStencilTestEnableEXT = (PFN_vkCmdSetStencilTestEnableEXT)gdpa(device, "vkCmdSetStencilTestEnableEXT");
    table->CmdSetStencilOpEXT = (PFN_vkCmdSetStencilOpEXT)gdpa(device, "vkCmdSetStencilOpEXT");
    table->CopyMemoryToImageEXT = (PFN_vkCopyMemoryToImageEXT)gdpa(device, "vkCopyMemoryToImageEXT");
    table->CopyImageToMemoryEXT = (PFN_vkCopyImageToMemoryEXT)gdpa(device, "vkCopyImageToMemoryEXT");
    table->CopyImageToImageEXT = (PFN_vkCopyImageToImageEXT)gdpa(device, "vkCopyImageToImageEXT");
    table->TransitionImageLayoutEXT = (PFN_vkTransitionImageLayoutEXT)gdpa(device, "vkTransitionImageLayoutEXT");
    table->GetImageSubresourceLayout2EXT = (PFN_vkGetImageSubresourceLayout2EXT)gdpa(device, "vkGetImageSubresourceLayout2EXT");
    table->ReleaseSwapchainImagesEXT = (PFN_vkReleaseSwapchainImagesEXT)gdpa(device, "vkReleaseSwapchainImagesEXT");
    table->GetGeneratedCommandsMemoryRequirementsNV = (PFN_vkGetGeneratedCommandsMemoryRequirementsNV)gdpa(device, "vkGetGeneratedCommandsMemoryRequirementsNV");
    table->CmdPreprocessGeneratedCommandsNV = (PFN_vkCmdPreprocessGeneratedCommandsNV)gdpa(device, "vkCmdPreprocessGeneratedCommandsNV");
    table->CmdExecuteGeneratedCommandsNV = (PFN_vkCmdExecuteGeneratedCommandsNV)gdpa(device, "vkCmdExecuteGeneratedCommandsNV");
    table->CmdBindPipelineShaderGroupNV = (PFN_vkCmdBindPipelineShaderGroupNV)gdpa(device, "vkCmdBindPipelineShaderGroupNV");
    table->CreateIndirectCommandsLayoutNV = (PFN_vkCreateIndirectCommandsLayoutNV)gdpa(device, "vkCreateIndirectCommandsLayoutNV");
    table->DestroyIndirectCommandsLayoutNV = (PFN_vkDestroyIndirectCommandsLayoutNV)gdpa(device, "vkDestroyIndirectCommandsLayoutNV");
    table->CmdSetDepthBias2EXT = (PFN_vkCmdSetDepthBias2EXT)gdpa(device, "vkCmdSetDepthBias2EXT");
    table->CreatePrivateDataSlotEXT = (PFN_vkCreatePrivateDataSlotEXT)gdpa(device, "vkCreatePrivateDataSlotEXT");
    table->DestroyPrivateDataSlotEXT = (PFN_vkDestroyPrivateDataSlotEXT)gdpa(device, "vkDestroyPrivateDataSlotEXT");
    table->SetPrivateDataEXT = (PFN_vkSetPrivateDataEXT)gdpa(device, "vkSetPrivateDataEXT");
    table->GetPrivateDataEXT = (PFN_vkGetPrivateDataEXT)gdpa(device, "vkGetPrivateDataEXT");
    table->QueueSetPerfHintQCOM = (PFN_vkQueueSetPerfHintQCOM)gdpa(device, "vkQueueSetPerfHintQCOM");
#ifdef VK_ENABLE_BETA_EXTENSIONS
    table->CreateCudaModuleNV = (PFN_vkCreateCudaModuleNV)gdpa(device, "vkCreateCudaModuleNV");
    table->GetCudaModuleCacheNV = (PFN_vkGetCudaModuleCacheNV)gdpa(device, "vkGetCudaModuleCacheNV");
    table->CreateCudaFunctionNV = (PFN_vkCreateCudaFunctionNV)gdpa(device, "vkCreateCudaFunctionNV");
    table->DestroyCudaModuleNV = (PFN_vkDestroyCudaModuleNV)gdpa(device, "vkDestroyCudaModuleNV");
    table->DestroyCudaFunctionNV = (PFN_vkDestroyCudaFunctionNV)gdpa(device, "vkDestroyCudaFunctionNV");
    table->CmdCudaLaunchKernelNV = (PFN_vkCmdCudaLaunchKernelNV)gdpa(device, "vkCmdCudaLaunchKernelNV");
#endif  // VK_ENABLE_BETA_EXTENSIONS
    table->CmdDispatchTileQCOM = (PFN_vkCmdDispatchTileQCOM)gdpa(device, "vkCmdDispatchTileQCOM");
    table->CmdBeginPerTileExecutionQCOM = (PFN_vkCmdBeginPerTileExecutionQCOM)gdpa(device, "vkCmdBeginPerTileExecutionQCOM");
    table->CmdEndPerTileExecutionQCOM = (PFN_vkCmdEndPerTileExecutionQCOM)gdpa(device, "vkCmdEndPerTileExecutionQCOM");
    table->SetLatencySleepModeLegacyNV = (PFN_vkSetLatencySleepModeLegacyNV)gdpa(device, "vkSetLatencySleepModeLegacyNV");
    table->LatencySleepLegacyNV = (PFN_vkLatencySleepLegacyNV)gdpa(device, "vkLatencySleepLegacyNV");
    table->SetLatencyMarkerLegacyNV = (PFN_vkSetLatencyMarkerLegacyNV)gdpa(device, "vkSetLatencyMarkerLegacyNV");
    table->GetLatencyTimingsLegacyNV = (PFN_vkGetLatencyTimingsLegacyNV)gdpa(device, "vkGetLatencyTimingsLegacyNV");
    table->QueueNotifyOutOfBandLegacyNV = (PFN_vkQueueNotifyOutOfBandLegacyNV)gdpa(device, "vkQueueNotifyOutOfBandLegacyNV");
    table->GetSleepStatusLegacyNV = (PFN_vkGetSleepStatusLegacyNV)gdpa(device, "vkGetSleepStatusLegacyNV");
    table->ShutdownLatencyDeviceLegacyNV = (PFN_vkShutdownLatencyDeviceLegacyNV)gdpa(device, "vkShutdownLatencyDeviceLegacyNV");
#ifdef VK_USE_PLATFORM_METAL_EXT
    table->ExportMetalObjectsEXT = (PFN_vkExportMetalObjectsEXT)gdpa(device, "vkExportMetalObjectsEXT");
#endif  // VK_USE_PLATFORM_METAL_EXT
    table->GetDescriptorSetLayoutSizeEXT = (PFN_vkGetDescriptorSetLayoutSizeEXT)gdpa(device, "vkGetDescriptorSetLayoutSizeEXT");
    table->GetDescriptorSetLayoutBindingOffsetEXT = (PFN_vkGetDescriptorSetLayoutBindingOffsetEXT)gdpa(device, "vkGetDescriptorSetLayoutBindingOffsetEXT");
    table->GetDescriptorEXT = (PFN_vkGetDescriptorEXT)gdpa(device, "vkGetDescriptorEXT");
    table->CmdBindDescriptorBuffersEXT = (PFN_vkCmdBindDescriptorBuffersEXT)gdpa(device, "vkCmdBindDescriptorBuffersEXT");
    table->CmdSetDescriptorBufferOffsetsEXT = (PFN_vkCmdSetDescriptorBufferOffsetsEXT)gdpa(device, "vkCmdSetDescriptorBufferOffsetsEXT");
    table->CmdBindDescriptorBufferEmbeddedSamplersEXT = (PFN_vkCmdBindDescriptorBufferEmbeddedSamplersEXT)gdpa(device, "vkCmdBindDescriptorBufferEmbeddedSamplersEXT");
    table->GetBufferOpaqueCaptureDescriptorDataEXT = (PFN_vkGetBufferOpaqueCaptureDescriptorDataEXT)gdpa(device, "vkGetBufferOpaqueCaptureDescriptorDataEXT");
    table->GetImageOpaqueCaptureDescriptorDataEXT = (PFN_vkGetImageOpaqueCaptureDescriptorDataEXT)gdpa(device, "vkGetImageOpaqueCaptureDescriptorDataEXT");
    table->GetImageViewOpaqueCaptureDescriptorDataEXT = (PFN_vkGetImageViewOpaqueCaptureDescriptorDataEXT)gdpa(device, "vkGetImageViewOpaqueCaptureDescriptorDataEXT");
    table->GetSamplerOpaqueCaptureDescriptorDataEXT = (PFN_vkGetSamplerOpaqueCaptureDescriptorDataEXT)gdpa(device, "vkGetSamplerOpaqueCaptureDescriptorDataEXT");
    table->GetAccelerationStructureOpaqueCaptureDescriptorDataEXT = (PFN_vkGetAccelerationStructureOpaqueCaptureDescriptorDataEXT)gdpa(device, "vkGetAccelerationStructureOpaqueCaptureDescriptorDataEXT");
    table->CmdSetFragmentShadingRateEnumNV = (PFN_vkCmdSetFragmentShadingRateEnumNV)gdpa(device, "vkCmdSetFragmentShadingRateEnumNV");
    table->GetDeviceFaultInfoEXT = (PFN_vkGetDeviceFaultInfoEXT)gdpa(device, "vkGetDeviceFaultInfoEXT");
    table->CmdSetVertexInputEXT = (PFN_vkCmdSetVertexInputEXT)gdpa(device, "vkCmdSetVertexInputEXT");
#ifdef VK_USE_PLATFORM_FUCHSIA
    table->GetMemoryZirconHandleFUCHSIA = (PFN_vkGetMemoryZirconHandleFUCHSIA)gdpa(device, "vkGetMemoryZirconHandleFUCHSIA");
    table->GetMemoryZirconHandlePropertiesFUCHSIA = (PFN_vkGetMemoryZirconHandlePropertiesFUCHSIA)gdpa(device, "vkGetMemoryZirconHandlePropertiesFUCHSIA");
    table->ImportSemaphoreZirconHandleFUCHSIA = (PFN_vkImportSemaphoreZirconHandleFUCHSIA)gdpa(device, "vkImportSemaphoreZirconHandleFUCHSIA");
    table->GetSemaphoreZirconHandleFUCHSIA = (PFN_vkGetSemaphoreZirconHandleFUCHSIA)gdpa(device, "vkGetSemaphoreZirconHandleFUCHSIA");
    table->CreateBufferCollectionFUCHSIA = (PFN_vkCreateBufferCollectionFUCHSIA)gdpa(device, "vkCreateBufferCollectionFUCHSIA");
    table->SetBufferCollectionImageConstraintsFUCHSIA = (PFN_vkSetBufferCollectionImageConstraintsFUCHSIA)gdpa(device, "vkSetBufferCollectionImageConstraintsFUCHSIA");
    table->SetBufferCollectionBufferConstraintsFUCHSIA = (PFN_vkSetBufferCollectionBufferConstraintsFUCHSIA)gdpa(device, "vkSetBufferCollectionBufferConstraintsFUCHSIA");
    table->DestroyBufferCollectionFUCHSIA = (PFN_vkDestroyBufferCollectionFUCHSIA)gdpa(device, "vkDestroyBufferCollectionFUCHSIA");
    table->GetBufferCollectionPropertiesFUCHSIA = (PFN_vkGetBufferCollectionPropertiesFUCHSIA)gdpa(device, "vkGetBufferCollectionPropertiesFUCHSIA");
#endif  // VK_USE_PLATFORM_FUCHSIA
    table->GetDeviceSubpassShadingMaxWorkgroupSizeHUAWEI = (PFN_vkGetDeviceSubpassShadingMaxWorkgroupSizeHUAWEI)gdpa(device, "vkGetDeviceSubpassShadingMaxWorkgroupSizeHUAWEI");
    table->CmdSubpassShadingHUAWEI = (PFN_vkCmdSubpassShadingHUAWEI)gdpa(device, "vkCmdSubpassShadingHUAWEI");
    table->CmdBindInvocationMaskHUAWEI = (PFN_vkCmdBindInvocationMaskHUAWEI)gdpa(device, "vkCmdBindInvocationMaskHUAWEI");
    table->GetMemoryRemoteAddressNV = (PFN_vkGetMemoryRemoteAddressNV)gdpa(device, "vkGetMemoryRemoteAddressNV");
    table->GetPipelinePropertiesEXT = (PFN_vkGetPipelinePropertiesEXT)gdpa(device, "vkGetPipelinePropertiesEXT");
    table->CmdSetPatchControlPointsEXT = (PFN_vkCmdSetPatchControlPointsEXT)gdpa(device, "vkCmdSetPatchControlPointsEXT");
    table->CmdSetRasterizerDiscardEnableEXT = (PFN_vkCmdSetRasterizerDiscardEnableEXT)gdpa(device, "vkCmdSetRasterizerDiscardEnableEXT");
    table->CmdSetDepthBiasEnableEXT = (PFN_vkCmdSetDepthBiasEnableEXT)gdpa(device, "vkCmdSetDepthBiasEnableEXT");
    table->CmdSetLogicOpEXT = (PFN_vkCmdSetLogicOpEXT)gdpa(device, "vkCmdSetLogicOpEXT");
    table->CmdSetPrimitiveRestartEnableEXT = (PFN_vkCmdSetPrimitiveRestartEnableEXT)gdpa(device, "vkCmdSetPrimitiveRestartEnableEXT");
    table->CmdSetColorWriteEnableEXT = (PFN_vkCmdSetColorWriteEnableEXT)gdpa(device, "vkCmdSetColorWriteEnableEXT");
    table->CmdDrawMultiEXT = (PFN_vkCmdDrawMultiEXT)gdpa(device, "vkCmdDrawMultiEXT");
    table->CmdDrawMultiIndexedEXT = (PFN_vkCmdDrawMultiIndexedEXT)gdpa(device, "vkCmdDrawMultiIndexedEXT");
    table->CreateMicromapEXT = (PFN_vkCreateMicromapEXT)gdpa(device, "vkCreateMicromapEXT");
    table->DestroyMicromapEXT = (PFN_vkDestroyMicromapEXT)gdpa(device, "vkDestroyMicromapEXT");
    table->CmdBuildMicromapsEXT = (PFN_vkCmdBuildMicromapsEXT)gdpa(device, "vkCmdBuildMicromapsEXT");
    table->BuildMicromapsEXT = (PFN_vkBuildMicromapsEXT)gdpa(device, "vkBuildMicromapsEXT");
    table->CopyMicromapEXT = (PFN_vkCopyMicromapEXT)gdpa(device, "vkCopyMicromapEXT");
    table->CopyMicromapToMemoryEXT = (PFN_vkCopyMicromapToMemoryEXT)gdpa(device, "vkCopyMicromapToMemoryEXT");
    table->CopyMemoryToMicromapEXT = (PFN_vkCopyMemoryToMicromapEXT)gdpa(device, "vkCopyMemoryToMicromapEXT");
    table->WriteMicromapsPropertiesEXT = (PFN_vkWriteMicromapsPropertiesEXT)gdpa(device, "vkWriteMicromapsPropertiesEXT");
    table->CmdCopyMicromapEXT = (PFN_vkCmdCopyMicromapEXT)gdpa(device, "vkCmdCopyMicromapEXT");
    table->CmdCopyMicromapToMemoryEXT = (PFN_vkCmdCopyMicromapToMemoryEXT)gdpa(device, "vkCmdCopyMicromapToMemoryEXT");
    table->CmdCopyMemoryToMicromapEXT = (PFN_vkCmdCopyMemoryToMicromapEXT)gdpa(device, "vkCmdCopyMemoryToMicromapEXT");
    table->CmdWriteMicromapsPropertiesEXT = (PFN_vkCmdWriteMicromapsPropertiesEXT)gdpa(device, "vkCmdWriteMicromapsPropertiesEXT");
    table->GetDeviceMicromapCompatibilityEXT = (PFN_vkGetDeviceMicromapCompatibilityEXT)gdpa(device, "vkGetDeviceMicromapCompatibilityEXT");
    table->GetMicromapBuildSizesEXT = (PFN_vkGetMicromapBuildSizesEXT)gdpa(device, "vkGetMicromapBuildSizesEXT");
    table->CmdDrawClusterHUAWEI = (PFN_vkCmdDrawClusterHUAWEI)gdpa(device, "vkCmdDrawClusterHUAWEI");
    table->CmdDrawClusterIndirectHUAWEI = (PFN_vkCmdDrawClusterIndirectHUAWEI)gdpa(device, "vkCmdDrawClusterIndirectHUAWEI");
    table->SetDeviceMemoryPriorityEXT = (PFN_vkSetDeviceMemoryPriorityEXT)gdpa(device, "vkSetDeviceMemoryPriorityEXT");
    table->CmdSetDispatchParametersARM = (PFN_vkCmdSetDispatchParametersARM)gdpa(device, "vkCmdSetDispatchParametersARM");
    table->GetDescriptorSetLayoutHostMappingInfoVALVE = (PFN_vkGetDescriptorSetLayoutHostMappingInfoVALVE)gdpa(device, "vkGetDescriptorSetLayoutHostMappingInfoVALVE");
    table->GetDescriptorSetHostMappingVALVE = (PFN_vkGetDescriptorSetHostMappingVALVE)gdpa(device, "vkGetDescriptorSetHostMappingVALVE");
    table->CmdCopyMemoryIndirectNV = (PFN_vkCmdCopyMemoryIndirectNV)gdpa(device, "vkCmdCopyMemoryIndirectNV");
    table->CmdCopyMemoryToImageIndirectNV = (PFN_vkCmdCopyMemoryToImageIndirectNV)gdpa(device, "vkCmdCopyMemoryToImageIndirectNV");
    table->CmdDecompressMemoryNV = (PFN_vkCmdDecompressMemoryNV)gdpa(device, "vkCmdDecompressMemoryNV");
    table->CmdDecompressMemoryIndirectCountNV = (PFN_vkCmdDecompressMemoryIndirectCountNV)gdpa(device, "vkCmdDecompressMemoryIndirectCountNV");
    table->GetPipelineIndirectMemoryRequirementsNV = (PFN_vkGetPipelineIndirectMemoryRequirementsNV)gdpa(device, "vkGetPipelineIndirectMemoryRequirementsNV");
    table->CmdUpdatePipelineIndirectBufferNV = (PFN_vkCmdUpdatePipelineIndirectBufferNV)gdpa(device, "vkCmdUpdatePipelineIndirectBufferNV");
    table->GetPipelineIndirectDeviceAddressNV = (PFN_vkGetPipelineIndirectDeviceAddressNV)gdpa(device, "vkGetPipelineIndirectDeviceAddressNV");
#ifdef VK_USE_PLATFORM_OHOS
    table->GetNativeBufferPropertiesOHOS = (PFN_vkGetNativeBufferPropertiesOHOS)gdpa(device, "vkGetNativeBufferPropertiesOHOS");
    table->GetMemoryNativeBufferOHOS = (PFN_vkGetMemoryNativeBufferOHOS)gdpa(device, "vkGetMemoryNativeBufferOHOS");
#endif  // VK_USE_PLATFORM_OHOS
    table->CmdSetDepthClampEnableEXT = (PFN_vkCmdSetDepthClampEnableEXT)gdpa(device, "vkCmdSetDepthClampEnableEXT");
    table->CmdSetPolygonModeEXT = (PFN_vkCmdSetPolygonModeEXT)gdpa(device, "vkCmdSetPolygonModeEXT");
    table->CmdSetRasterizationSamplesEXT = (PFN_vkCmdSetRasterizationSamplesEXT)gdpa(device, "vkCmdSetRasterizationSamplesEXT");
    table->CmdSetSampleMaskEXT = (PFN_vkCmdSetSampleMaskEXT)gdpa(device, "vkCmdSetSampleMaskEXT");
    table->CmdSetAlphaToCoverageEnableEXT = (PFN_vkCmdSetAlphaToCoverageEnableEXT)gdpa(device, "vkCmdSetAlphaToCoverageEnableEXT");
    table->CmdSetAlphaToOneEnableEXT = (PFN_vkCmdSetAlphaToOneEnableEXT)gdpa(device, "vkCmdSetAlphaToOneEnableEXT");
    table->CmdSetLogicOpEnableEXT = (PFN_vkCmdSetLogicOpEnableEXT)gdpa(device, "vkCmdSetLogicOpEnableEXT");
    table->CmdSetColorBlendEnableEXT = (PFN_vkCmdSetColorBlendEnableEXT)gdpa(device, "vkCmdSetColorBlendEnableEXT");
    table->CmdSetColorBlendEquationEXT = (PFN_vkCmdSetColorBlendEquationEXT)gdpa(device, "vkCmdSetColorBlendEquationEXT");
    table->CmdSetColorWriteMaskEXT = (PFN_vkCmdSetColorWriteMaskEXT)gdpa(device, "vkCmdSetColorWriteMaskEXT");
    table->CmdSetTessellationDomainOriginEXT = (PFN_vkCmdSetTessellationDomainOriginEXT)gdpa(device, "vkCmdSetTessellationDomainOriginEXT");
    table->CmdSetRasterizationStreamEXT = (PFN_vkCmdSetRasterizationStreamEXT)gdpa(device, "vkCmdSetRasterizationStreamEXT");
    table->CmdSetConservativeRasterizationModeEXT = (PFN_vkCmdSetConservativeRasterizationModeEXT)gdpa(device, "vkCmdSetConservativeRasterizationModeEXT");
    table->CmdSetExtraPrimitiveOverestimationSizeEXT = (PFN_vkCmdSetExtraPrimitiveOverestimationSizeEXT)gdpa(device, "vkCmdSetExtraPrimitiveOverestimationSizeEXT");
    table->CmdSetDepthClipEnableEXT = (PFN_vkCmdSetDepthClipEnableEXT)gdpa(device, "vkCmdSetDepthClipEnableEXT");
    table->CmdSetSampleLocationsEnableEXT = (PFN_vkCmdSetSampleLocationsEnableEXT)gdpa(device, "vkCmdSetSampleLocationsEnableEXT");
    table->CmdSetColorBlendAdvancedEXT = (PFN_vkCmdSetColorBlendAdvancedEXT)gdpa(device, "vkCmdSetColorBlendAdvancedEXT");
    table->CmdSetProvokingVertexModeEXT = (PFN_vkCmdSetProvokingVertexModeEXT)gdpa(device, "vkCmdSetProvokingVertexModeEXT");
    table->CmdSetLineRasterizationModeEXT = (PFN_vkCmdSetLineRasterizationModeEXT)gdpa(device, "vkCmdSetLineRasterizationModeEXT");
    table->CmdSetLineStippleEnableEXT = (PFN_vkCmdSetLineStippleEnableEXT)gdpa(device, "vkCmdSetLineStippleEnableEXT");
    table->CmdSetDepthClipNegativeOneToOneEXT = (PFN_vkCmdSetDepthClipNegativeOneToOneEXT)gdpa(device, "vkCmdSetDepthClipNegativeOneToOneEXT");
    table->CmdSetViewportWScalingEnableNV = (PFN_vkCmdSetViewportWScalingEnableNV)gdpa(device, "vkCmdSetViewportWScalingEnableNV");
    table->CmdSetViewportSwizzleNV = (PFN_vkCmdSetViewportSwizzleNV)gdpa(device, "vkCmdSetViewportSwizzleNV");
    table->CmdSetCoverageToColorEnableNV = (PFN_vkCmdSetCoverageToColorEnableNV)gdpa(device, "vkCmdSetCoverageToColorEnableNV");
    table->CmdSetCoverageToColorLocationNV = (PFN_vkCmdSetCoverageToColorLocationNV)gdpa(device, "vkCmdSetCoverageToColorLocationNV");
    table->CmdSetCoverageModulationModeNV = (PFN_vkCmdSetCoverageModulationModeNV)gdpa(device, "vkCmdSetCoverageModulationModeNV");
    table->CmdSetCoverageModulationTableEnableNV = (PFN_vkCmdSetCoverageModulationTableEnableNV)gdpa(device, "vkCmdSetCoverageModulationTableEnableNV");
    table->CmdSetCoverageModulationTableNV = (PFN_vkCmdSetCoverageModulationTableNV)gdpa(device, "vkCmdSetCoverageModulationTableNV");
    table->CmdSetShadingRateImageEnableNV = (PFN_vkCmdSetShadingRateImageEnableNV)gdpa(device, "vkCmdSetShadingRateImageEnableNV");
    table->CmdSetRepresentativeFragmentTestEnableNV = (PFN_vkCmdSetRepresentativeFragmentTestEnableNV)gdpa(device, "vkCmdSetRepresentativeFragmentTestEnableNV");
    table->CmdSetCoverageReductionModeNV = (PFN_vkCmdSetCoverageReductionModeNV)gdpa(device, "vkCmdSetCoverageReductionModeNV");
    table->CreateTensorARM = (PFN_vkCreateTensorARM)gdpa(device, "vkCreateTensorARM");
    table->DestroyTensorARM = (PFN_vkDestroyTensorARM)gdpa(device, "vkDestroyTensorARM");
    table->CreateTensorViewARM = (PFN_vkCreateTensorViewARM)gdpa(device, "vkCreateTensorViewARM");
    table->DestroyTensorViewARM = (PFN_vkDestroyTensorViewARM)gdpa(device, "vkDestroyTensorViewARM");
    table->GetTensorMemoryRequirementsARM = (PFN_vkGetTensorMemoryRequirementsARM)gdpa(device, "vkGetTensorMemoryRequirementsARM");
    table->BindTensorMemoryARM = (PFN_vkBindTensorMemoryARM)gdpa(device, "vkBindTensorMemoryARM");
    table->GetDeviceTensorMemoryRequirementsARM = (PFN_vkGetDeviceTensorMemoryRequirementsARM)gdpa(device, "vkGetDeviceTensorMemoryRequirementsARM");
    table->CmdCopyTensorARM = (PFN_vkCmdCopyTensorARM)gdpa(device, "vkCmdCopyTensorARM");
    table->GetTensorOpaqueCaptureDescriptorDataARM = (PFN_vkGetTensorOpaqueCaptureDescriptorDataARM)gdpa(device, "vkGetTensorOpaqueCaptureDescriptorDataARM");
    table->GetTensorViewOpaqueCaptureDescriptorDataARM = (PFN_vkGetTensorViewOpaqueCaptureDescriptorDataARM)gdpa(device, "vkGetTensorViewOpaqueCaptureDescriptorDataARM");
    table->GetShaderModuleIdentifierEXT = (PFN_vkGetShaderModuleIdentifierEXT)gdpa(device, "vkGetShaderModuleIdentifierEXT");
    table->GetShaderModuleCreateInfoIdentifierEXT = (PFN_vkGetShaderModuleCreateInfoIdentifierEXT)gdpa(device, "vkGetShaderModuleCreateInfoIdentifierEXT");
    table->CreateOpticalFlowSessionNV = (PFN_vkCreateOpticalFlowSessionNV)gdpa(device, "vkCreateOpticalFlowSessionNV");
    table->DestroyOpticalFlowSessionNV = (PFN_vkDestroyOpticalFlowSessionNV)gdpa(device, "vkDestroyOpticalFlowSessionNV");
    table->BindOpticalFlowSessionImageNV = (PFN_vkBindOpticalFlowSessionImageNV)gdpa(device, "vkBindOpticalFlowSessionImageNV");
    table->CmdOpticalFlowExecuteNV = (PFN_vkCmdOpticalFlowExecuteNV)gdpa(device, "vkCmdOpticalFlowExecuteNV");
    table->AntiLagUpdateAMD = (PFN_vkAntiLagUpdateAMD)gdpa(device, "vkAntiLagUpdateAMD");
    table->CreateShadersEXT = (PFN_vkCreateShadersEXT)gdpa(device, "vkCreateShadersEXT");
    table->DestroyShaderEXT = (PFN_vkDestroyShaderEXT)gdpa(device, "vkDestroyShaderEXT");
    table->GetShaderBinaryDataEXT = (PFN_vkGetShaderBinaryDataEXT)gdpa(device, "vkGetShaderBinaryDataEXT");
    table->CmdBindShadersEXT = (PFN_vkCmdBindShadersEXT)gdpa(device, "vkCmdBindShadersEXT");
    table->CmdSetDepthClampRangeEXT = (PFN_vkCmdSetDepthClampRangeEXT)gdpa(device, "vkCmdSetDepthClampRangeEXT");
    table->GetFramebufferTilePropertiesQCOM = (PFN_vkGetFramebufferTilePropertiesQCOM)gdpa(device, "vkGetFramebufferTilePropertiesQCOM");
    table->GetDynamicRenderingTilePropertiesQCOM = (PFN_vkGetDynamicRenderingTilePropertiesQCOM)gdpa(device, "vkGetDynamicRenderingTilePropertiesQCOM");
    table->ConvertCooperativeVectorMatrixNV = (PFN_vkConvertCooperativeVectorMatrixNV)gdpa(device, "vkConvertCooperativeVectorMatrixNV");
    table->CmdConvertCooperativeVectorMatrixNV = (PFN_vkCmdConvertCooperativeVectorMatrixNV)gdpa(device, "vkCmdConvertCooperativeVectorMatrixNV");
    table->SetLatencySleepModeNV = (PFN_vkSetLatencySleepModeNV)gdpa(device, "vkSetLatencySleepModeNV");
    table->LatencySleepNV = (PFN_vkLatencySleepNV)gdpa(device, "vkLatencySleepNV");
    table->SetLatencyMarkerNV = (PFN_vkSetLatencyMarkerNV)gdpa(device, "vkSetLatencyMarkerNV");
    table->GetLatencyTimingsNV = (PFN_vkGetLatencyTimingsNV)gdpa(device, "vkGetLatencyTimingsNV");
    table->QueueNotifyOutOfBandNV = (PFN_vkQueueNotifyOutOfBandNV)gdpa(device, "vkQueueNotifyOutOfBandNV");
    table->CreateDataGraphPipelinesARM = (PFN_vkCreateDataGraphPipelinesARM)gdpa(device, "vkCreateDataGraphPipelinesARM");
    table->CreateDataGraphPipelineSessionARM = (PFN_vkCreateDataGraphPipelineSessionARM)gdpa(device, "vkCreateDataGraphPipelineSessionARM");
    table->GetDataGraphPipelineSessionBindPointRequirementsARM = (PFN_vkGetDataGraphPipelineSessionBindPointRequirementsARM)gdpa(device, "vkGetDataGraphPipelineSessionBindPointRequirementsARM");
    table->GetDataGraphPipelineSessionMemoryRequirementsARM = (PFN_vkGetDataGraphPipelineSessionMemoryRequirementsARM)gdpa(device, "vkGetDataGraphPipelineSessionMemoryRequirementsARM");
    table->BindDataGraphPipelineSessionMemoryARM = (PFN_vkBindDataGraphPipelineSessionMemoryARM)gdpa(device, "vkBindDataGraphPipelineSessionMemoryARM");
    table->DestroyDataGraphPipelineSessionARM = (PFN_vkDestroyDataGraphPipelineSessionARM)gdpa(device, "vkDestroyDataGraphPipelineSessionARM");
    table->CmdDispatchDataGraphARM = (PFN_vkCmdDispatchDataGraphARM)gdpa(device, "vkCmdDispatchDataGraphARM");
    table->GetDataGraphPipelineAvailablePropertiesARM = (PFN_vkGetDataGraphPipelineAvailablePropertiesARM)gdpa(device, "vkGetDataGraphPipelineAvailablePropertiesARM");
    table->GetDataGraphPipelinePropertiesARM = (PFN_vkGetDataGraphPipelinePropertiesARM)gdpa(device, "vkGetDataGraphPipelinePropertiesARM");
    table->CmdSetAttachmentFeedbackLoopEnableEXT = (PFN_vkCmdSetAttachmentFeedbackLoopEnableEXT)gdpa(device, "vkCmdSetAttachmentFeedbackLoopEnableEXT");
#ifdef VK_USE_PLATFORM_SCREEN_QNX
    table->GetScreenBufferPropertiesQNX = (PFN_vkGetScreenBufferPropertiesQNX)gdpa(device, "vkGetScreenBufferPropertiesQNX");
#endif  // VK_USE_PLATFORM_SCREEN_QNX
    table->CmdBindTileMemoryQCOM = (PFN_vkCmdBindTileMemoryQCOM)gdpa(device, "vkCmdBindTileMemoryQCOM");
    table->CmdDecompressMemoryEXT = (PFN_vkCmdDecompressMemoryEXT)gdpa(device, "vkCmdDecompressMemoryEXT");
    table->CmdDecompressMemoryIndirectCountEXT = (PFN_vkCmdDecompressMemoryIndirectCountEXT)gdpa(device, "vkCmdDecompressMemoryIndirectCountEXT");
    table->CreateExternalComputeQueueNV = (PFN_vkCreateExternalComputeQueueNV)gdpa(device, "vkCreateExternalComputeQueueNV");
    table->DestroyExternalComputeQueueNV = (PFN_vkDestroyExternalComputeQueueNV)gdpa(device, "vkDestroyExternalComputeQueueNV");
    table->GetExternalComputeQueueDataNV = (PFN_vkGetExternalComputeQueueDataNV)gdpa(device, "vkGetExternalComputeQueueDataNV");
    table->GetClusterAccelerationStructureBuildSizesNV = (PFN_vkGetClusterAccelerationStructureBuildSizesNV)gdpa(device, "vkGetClusterAccelerationStructureBuildSizesNV");
    table->CmdBuildClusterAccelerationStructureIndirectNV = (PFN_vkCmdBuildClusterAccelerationStructureIndirectNV)gdpa(device, "vkCmdBuildClusterAccelerationStructureIndirectNV");
    table->GetPartitionedAccelerationStructuresBuildSizesNV = (PFN_vkGetPartitionedAccelerationStructuresBuildSizesNV)gdpa(device, "vkGetPartitionedAccelerationStructuresBuildSizesNV");
    table->CmdBuildPartitionedAccelerationStructuresNV = (PFN_vkCmdBuildPartitionedAccelerationStructuresNV)gdpa(device, "vkCmdBuildPartitionedAccelerationStructuresNV");
    table->GetGeneratedCommandsMemoryRequirementsEXT = (PFN_vkGetGeneratedCommandsMemoryRequirementsEXT)gdpa(device, "vkGetGeneratedCommandsMemoryRequirementsEXT");
    table->CmdPreprocessGeneratedCommandsEXT = (PFN_vkCmdPreprocessGeneratedCommandsEXT)gdpa(device, "vkCmdPreprocessGeneratedCommandsEXT");
    table->CmdExecuteGeneratedCommandsEXT = (PFN_vkCmdExecuteGeneratedCommandsEXT)gdpa(device, "vkCmdExecuteGeneratedCommandsEXT");
    table->CreateIndirectCommandsLayoutEXT = (PFN_vkCreateIndirectCommandsLayoutEXT)gdpa(device, "vkCreateIndirectCommandsLayoutEXT");
    table->DestroyIndirectCommandsLayoutEXT = (PFN_vkDestroyIndirectCommandsLayoutEXT)gdpa(device, "vkDestroyIndirectCommandsLayoutEXT");
    table->CreateIndirectExecutionSetEXT = (PFN_vkCreateIndirectExecutionSetEXT)gdpa(device, "vkCreateIndirectExecutionSetEXT");
    table->DestroyIndirectExecutionSetEXT = (PFN_vkDestroyIndirectExecutionSetEXT)gdpa(device, "vkDestroyIndirectExecutionSetEXT");
    table->UpdateIndirectExecutionSetPipelineEXT = (PFN_vkUpdateIndirectExecutionSetPipelineEXT)gdpa(device, "vkUpdateIndirectExecutionSetPipelineEXT");
    table->UpdateIndirectExecutionSetShaderEXT = (PFN_vkUpdateIndirectExecutionSetShaderEXT)gdpa(device, "vkUpdateIndirectExecutionSetShaderEXT");
#ifdef VK_USE_PLATFORM_METAL_EXT
    table->GetMemoryMetalHandleEXT = (PFN_vkGetMemoryMetalHandleEXT)gdpa(device, "vkGetMemoryMetalHandleEXT");
    table->GetMemoryMetalHandlePropertiesEXT = (PFN_vkGetMemoryMetalHandlePropertiesEXT)gdpa(device, "vkGetMemoryMetalHandlePropertiesEXT");
#endif  // VK_USE_PLATFORM_METAL_EXT
    table->CreateShaderInstrumentationARM = (PFN_vkCreateShaderInstrumentationARM)gdpa(device, "vkCreateShaderInstrumentationARM");
    table->DestroyShaderInstrumentationARM = (PFN_vkDestroyShaderInstrumentationARM)gdpa(device, "vkDestroyShaderInstrumentationARM");
    table->CmdBeginShaderInstrumentationARM = (PFN_vkCmdBeginShaderInstrumentationARM)gdpa(device, "vkCmdBeginShaderInstrumentationARM");
    table->CmdEndShaderInstrumentationARM = (PFN_vkCmdEndShaderInstrumentationARM)gdpa(device, "vkCmdEndShaderInstrumentationARM");
    table->GetShaderInstrumentationValuesARM = (PFN_vkGetShaderInstrumentationValuesARM)gdpa(device, "vkGetShaderInstrumentationValuesARM");
    table->ClearShaderInstrumentationMetricsARM = (PFN_vkClearShaderInstrumentationMetricsARM)gdpa(device, "vkClearShaderInstrumentationMetricsARM");
    table->CmdEndRendering2EXT = (PFN_vkCmdEndRendering2EXT)gdpa(device, "vkCmdEndRendering2EXT");
    table->CmdBeginCustomResolveEXT = (PFN_vkCmdBeginCustomResolveEXT)gdpa(device, "vkCmdBeginCustomResolveEXT");
    table->CmdSetComputeOccupancyPriorityNV = (PFN_vkCmdSetComputeOccupancyPriorityNV)gdpa(device, "vkCmdSetComputeOccupancyPriorityNV");
    table->CmdSetPrimitiveRestartIndexEXT = (PFN_vkCmdSetPrimitiveRestartIndexEXT)gdpa(device, "vkCmdSetPrimitiveRestartIndexEXT");
    table->CreateAccelerationStructureKHR = (PFN_vkCreateAccelerationStructureKHR)gdpa(device, "vkCreateAccelerationStructureKHR");
    table->DestroyAccelerationStructureKHR = (PFN_vkDestroyAccelerationStructureKHR)gdpa(device, "vkDestroyAccelerationStructureKHR");
    table->CmdBuildAccelerationStructuresKHR = (PFN_vkCmdBuildAccelerationStructuresKHR)gdpa(device, "vkCmdBuildAccelerationStructuresKHR");
    table->CmdBuildAccelerationStructuresIndirectKHR = (PFN_vkCmdBuildAccelerationStructuresIndirectKHR)gdpa(device, "vkCmdBuildAccelerationStructuresIndirectKHR");
    table->BuildAccelerationStructuresKHR = (PFN_vkBuildAccelerationStructuresKHR)gdpa(device, "vkBuildAccelerationStructuresKHR");
    table->CopyAccelerationStructureKHR = (PFN_vkCopyAccelerationStructureKHR)gdpa(device, "vkCopyAccelerationStructureKHR");
    table->CopyAccelerationStructureToMemoryKHR = (PFN_vkCopyAccelerationStructureToMemoryKHR)gdpa(device, "vkCopyAccelerationStructureToMemoryKHR");
    table->CopyMemoryToAccelerationStructureKHR = (PFN_vkCopyMemoryToAccelerationStructureKHR)gdpa(device, "vkCopyMemoryToAccelerationStructureKHR");
    table->WriteAccelerationStructuresPropertiesKHR = (PFN_vkWriteAccelerationStructuresPropertiesKHR)gdpa(device, "vkWriteAccelerationStructuresPropertiesKHR");
    table->CmdCopyAccelerationStructureKHR = (PFN_vkCmdCopyAccelerationStructureKHR)gdpa(device, "vkCmdCopyAccelerationStructureKHR");
    table->CmdCopyAccelerationStructureToMemoryKHR = (PFN_vkCmdCopyAccelerationStructureToMemoryKHR)gdpa(device, "vkCmdCopyAccelerationStructureToMemoryKHR");
    table->CmdCopyMemoryToAccelerationStructureKHR = (PFN_vkCmdCopyMemoryToAccelerationStructureKHR)gdpa(device, "vkCmdCopyMemoryToAccelerationStructureKHR");
    table->GetAccelerationStructureDeviceAddressKHR = (PFN_vkGetAccelerationStructureDeviceAddressKHR)gdpa(device, "vkGetAccelerationStructureDeviceAddressKHR");
    table->CmdWriteAccelerationStructuresPropertiesKHR = (PFN_vkCmdWriteAccelerationStructuresPropertiesKHR)gdpa(device, "vkCmdWriteAccelerationStructuresPropertiesKHR");
    table->GetDeviceAccelerationStructureCompatibilityKHR = (PFN_vkGetDeviceAccelerationStructureCompatibilityKHR)gdpa(device, "vkGetDeviceAccelerationStructureCompatibilityKHR");
    table->GetAccelerationStructureBuildSizesKHR = (PFN_vkGetAccelerationStructureBuildSizesKHR)gdpa(device, "vkGetAccelerationStructureBuildSizesKHR");
    table->CmdTraceRaysKHR = (PFN_vkCmdTraceRaysKHR)gdpa(device, "vkCmdTraceRaysKHR");
    table->CreateRayTracingPipelinesKHR = (PFN_vkCreateRayTracingPipelinesKHR)gdpa(device, "vkCreateRayTracingPipelinesKHR");
    table->GetRayTracingCaptureReplayShaderGroupHandlesKHR = (PFN_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR)gdpa(device, "vkGetRayTracingCaptureReplayShaderGroupHandlesKHR");
    table->CmdTraceRaysIndirectKHR = (PFN_vkCmdTraceRaysIndirectKHR)gdpa(device, "vkCmdTraceRaysIndirectKHR");
    table->GetRayTracingShaderGroupStackSizeKHR = (PFN_vkGetRayTracingShaderGroupStackSizeKHR)gdpa(device, "vkGetRayTracingShaderGroupStackSizeKHR");
    table->CmdSetRayTracingPipelineStackSizeKHR = (PFN_vkCmdSetRayTracingPipelineStackSizeKHR)gdpa(device, "vkCmdSetRayTracingPipelineStackSizeKHR");
    table->CmdDrawMeshTasksEXT = (PFN_vkCmdDrawMeshTasksEXT)gdpa(device, "vkCmdDrawMeshTasksEXT");
    table->CmdDrawMeshTasksIndirectEXT = (PFN_vkCmdDrawMeshTasksIndirectEXT)gdpa(device, "vkCmdDrawMeshTasksIndirectEXT");
    table->CmdDrawMeshTasksIndirectCountEXT = (PFN_vkCmdDrawMeshTasksIndirectCountEXT)gdpa(device, "vkCmdDrawMeshTasksIndirectCountEXT");
}

static inline void vkuInitInstanceDispatchTable(VkInstance instance, VkuInstanceDispatchTable *table, PFN_vkGetInstanceProcAddr gipa) {
    memset(table, 0, sizeof(*table));
    // Instance function pointers
    table->GetInstanceProcAddr = gipa;
    table->GetPhysicalDeviceProcAddr = (PFN_GetPhysicalDeviceProcAddr)gipa(instance, "vk_layerGetPhysicalDeviceProcAddr");
    table->DestroyInstance = (PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance");
    table->EnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices");
    table->GetPhysicalDeviceFeatures = (PFN_vkGetPhysicalDeviceFeatures)gipa(instance, "vkGetPhysicalDeviceFeatures");
    table->GetPhysicalDeviceFormatProperties = (PFN_vkGetPhysicalDeviceFormatProperties)gipa(instance, "vkGetPhysicalDeviceFormatProperties");
    table->GetPhysicalDeviceImageFormatProperties = (PFN_vkGetPhysicalDeviceImageFormatProperties)gipa(instance, "vkGetPhysicalDeviceImageFormatProperties");
    table->GetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)gipa(instance, "vkGetPhysicalDeviceProperties");
    table->GetPhysicalDeviceQueueFamilyProperties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    table->GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(instance, "vkGetPhysicalDeviceMemoryProperties");
    table->EnumerateDeviceExtensionProperties = (PFN_vkEnumerateDeviceExtensionProperties)gipa(instance, "vkEnumerateDeviceExtensionProperties");
    table->EnumerateDeviceLayerProperties = (PFN_vkEnumerateDeviceLayerProperties)gipa(instance, "vkEnumerateDeviceLayerProperties");
    table->GetPhysicalDeviceSparseImageFormatProperties = (PFN_vkGetPhysicalDeviceSparseImageFormatProperties)gipa(instance, "vkGetPhysicalDeviceSparseImageFormatProperties");
    table->EnumeratePhysicalDeviceGroups = (PFN_vkEnumeratePhysicalDeviceGroups)gipa(instance, "vkEnumeratePhysicalDeviceGroups");
    table->GetPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(instance, "vkGetPhysicalDeviceFeatures2");
    table->GetPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2)gipa(instance, "vkGetPhysicalDeviceProperties2");
    table->GetPhysicalDeviceFormatProperties2 = (PFN_vkGetPhysicalDeviceFormatProperties2)gipa(instance, "vkGetPhysicalDeviceFormatProperties2");
    table->GetPhysicalDeviceImageFormatProperties2 = (PFN_vkGetPhysicalDeviceImageFormatProperties2)gipa(instance, "vkGetPhysicalDeviceImageFormatProperties2");
    table->GetPhysicalDeviceQueueFamilyProperties2 = (PFN_vkGetPhysicalDeviceQueueFamilyProperties2)gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties2");
    table->GetPhysicalDeviceMemoryProperties2 = (PFN_vkGetPhysicalDeviceMemoryProperties2)gipa(instance, "vkGetPhysicalDeviceMemoryProperties2");
    table->GetPhysicalDeviceSparseImageFormatProperties2 = (PFN_vkGetPhysicalDeviceSparseImageFormatProperties2)gipa(instance, "vkGetPhysicalDeviceSparseImageFormatProperties2");
    table->GetPhysicalDeviceExternalBufferProperties = (PFN_vkGetPhysicalDeviceExternalBufferProperties)gipa(instance, "vkGetPhysicalDeviceExternalBufferProperties");
    table->GetPhysicalDeviceExternalFenceProperties = (PFN_vkGetPhysicalDeviceExternalFenceProperties)gipa(instance, "vkGetPhysicalDeviceExternalFenceProperties");
    table->GetPhysicalDeviceExternalSemaphoreProperties = (PFN_vkGetPhysicalDeviceExternalSemaphoreProperties)gipa(instance, "vkGetPhysicalDeviceExternalSemaphoreProperties");
    table->GetPhysicalDeviceToolProperties = (PFN_vkGetPhysicalDeviceToolProperties)gipa(instance, "vkGetPhysicalDeviceToolProperties");
    table->DestroySurfaceKHR = (PFN_vkDestroySurfaceKHR)gipa(instance, "vkDestroySurfaceKHR");
    table->GetPhysicalDeviceSurfaceSupportKHR = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)gipa(instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
    table->GetPhysicalDeviceSurfaceCapabilitiesKHR = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)gipa(instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    table->GetPhysicalDeviceSurfaceFormatsKHR = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)gipa(instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    table->GetPhysicalDeviceSurfacePresentModesKHR = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)gipa(instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    table->GetPhysicalDevicePresentRectanglesKHR = (PFN_vkGetPhysicalDevicePresentRectanglesKHR)gipa(instance, "vkGetPhysicalDevicePresentRectanglesKHR");
    table->GetPhysicalDeviceDisplayPropertiesKHR = (PFN_vkGetPhysicalDeviceDisplayPropertiesKHR)gipa(instance, "vkGetPhysicalDeviceDisplayPropertiesKHR");
    table->GetPhysicalDeviceDisplayPlanePropertiesKHR = (PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR)gipa(instance, "vkGetPhysicalDeviceDisplayPlanePropertiesKHR");
    table->GetDisplayPlaneSupportedDisplaysKHR = (PFN_vkGetDisplayPlaneSupportedDisplaysKHR)gipa(instance, "vkGetDisplayPlaneSupportedDisplaysKHR");
    table->GetDisplayModePropertiesKHR = (PFN_vkGetDisplayModePropertiesKHR)gipa(instance, "vkGetDisplayModePropertiesKHR");
    table->CreateDisplayModeKHR = (PFN_vkCreateDisplayModeKHR)gipa(instance, "vkCreateDisplayModeKHR");
    table->GetDisplayPlaneCapabilitiesKHR = (PFN_vkGetDisplayPlaneCapabilitiesKHR)gipa(instance, "vkGetDisplayPlaneCapabilitiesKHR");
    table->CreateDisplayPlaneSurfaceKHR = (PFN_vkCreateDisplayPlaneSurfaceKHR)gipa(instance, "vkCreateDisplayPlaneSurfaceKHR");
#ifdef VK_USE_PLATFORM_XLIB_KHR
    table->CreateXlibSurfaceKHR = (PFN_vkCreateXlibSurfaceKHR)gipa(instance, "vkCreateXlibSurfaceKHR");
    table->GetPhysicalDeviceXlibPresentationSupportKHR = (PFN_vkGetPhysicalDeviceXlibPresentationSupportKHR)gipa(instance, "vkGetPhysicalDeviceXlibPresentationSupportKHR");
#endif  // VK_USE_PLATFORM_XLIB_KHR
#ifdef VK_USE_PLATFORM_XCB_KHR
    table->CreateXcbSurfaceKHR = (PFN_vkCreateXcbSurfaceKHR)gipa(instance, "vkCreateXcbSurfaceKHR");
    table->GetPhysicalDeviceXcbPresentationSupportKHR = (PFN_vkGetPhysicalDeviceXcbPresentationSupportKHR)gipa(instance, "vkGetPhysicalDeviceXcbPresentationSupportKHR");
#endif  // VK_USE_PLATFORM_XCB_KHR
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
    table->CreateWaylandSurfaceKHR = (PFN_vkCreateWaylandSurfaceKHR)gipa(instance, "vkCreateWaylandSurfaceKHR");
    table->GetPhysicalDeviceWaylandPresentationSupportKHR = (PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR)gipa(instance, "vkGetPhysicalDeviceWaylandPresentationSupportKHR");
#endif  // VK_USE_PLATFORM_WAYLAND_KHR
#ifdef VK_USE_PLATFORM_ANDROID_KHR
    table->CreateAndroidSurfaceKHR = (PFN_vkCreateAndroidSurfaceKHR)gipa(instance, "vkCreateAndroidSurfaceKHR");
#endif  // VK_USE_PLATFORM_ANDROID_KHR
#ifdef VK_USE_PLATFORM_WIN32_KHR
    table->CreateWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR)gipa(instance, "vkCreateWin32SurfaceKHR");
    table->GetPhysicalDeviceWin32PresentationSupportKHR = (PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR)gipa(instance, "vkGetPhysicalDeviceWin32PresentationSupportKHR");
#endif  // VK_USE_PLATFORM_WIN32_KHR
    table->GetPhysicalDeviceVideoCapabilitiesKHR = (PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR)gipa(instance, "vkGetPhysicalDeviceVideoCapabilitiesKHR");
    table->GetPhysicalDeviceVideoFormatPropertiesKHR = (PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR)gipa(instance, "vkGetPhysicalDeviceVideoFormatPropertiesKHR");
    table->GetPhysicalDeviceFeatures2KHR = (PFN_vkGetPhysicalDeviceFeatures2KHR)gipa(instance, "vkGetPhysicalDeviceFeatures2KHR");
    table->GetPhysicalDeviceProperties2KHR = (PFN_vkGetPhysicalDeviceProperties2KHR)gipa(instance, "vkGetPhysicalDeviceProperties2KHR");
    table->GetPhysicalDeviceFormatProperties2KHR = (PFN_vkGetPhysicalDeviceFormatProperties2KHR)gipa(instance, "vkGetPhysicalDeviceFormatProperties2KHR");
    table->GetPhysicalDeviceImageFormatProperties2KHR = (PFN_vkGetPhysicalDeviceImageFormatProperties2KHR)gipa(instance, "vkGetPhysicalDeviceImageFormatProperties2KHR");
    table->GetPhysicalDeviceQueueFamilyProperties2KHR = (PFN_vkGetPhysicalDeviceQueueFamilyProperties2KHR)gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties2KHR");
    table->GetPhysicalDeviceMemoryProperties2KHR = (PFN_vkGetPhysicalDeviceMemoryProperties2KHR)gipa(instance, "vkGetPhysicalDeviceMemoryProperties2KHR");
    table->GetPhysicalDeviceSparseImageFormatProperties2KHR = (PFN_vkGetPhysicalDeviceSparseImageFormatProperties2KHR)gipa(instance, "vkGetPhysicalDeviceSparseImageFormatProperties2KHR");
    table->EnumeratePhysicalDeviceGroupsKHR = (PFN_vkEnumeratePhysicalDeviceGroupsKHR)gipa(instance, "vkEnumeratePhysicalDeviceGroupsKHR");
    table->GetPhysicalDeviceExternalBufferPropertiesKHR = (PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR)gipa(instance, "vkGetPhysicalDeviceExternalBufferPropertiesKHR");
    table->GetPhysicalDeviceExternalSemaphorePropertiesKHR = (PFN_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR)gipa(instance, "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR");
    table->GetPhysicalDeviceExternalFencePropertiesKHR = (PFN_vkGetPhysicalDeviceExternalFencePropertiesKHR)gipa(instance, "vkGetPhysicalDeviceExternalFencePropertiesKHR");
    table->EnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR = (PFN_vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR)gipa(instance, "vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR");
    table->GetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR = (PFN_vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR)gipa(instance, "vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR");
    table->GetPhysicalDeviceSurfaceCapabilities2KHR = (PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)gipa(instance, "vkGetPhysicalDeviceSurfaceCapabilities2KHR");
    table->GetPhysicalDeviceSurfaceFormats2KHR = (PFN_vkGetPhysicalDeviceSurfaceFormats2KHR)gipa(instance, "vkGetPhysicalDeviceSurfaceFormats2KHR");
    table->GetPhysicalDeviceDisplayProperties2KHR = (PFN_vkGetPhysicalDeviceDisplayProperties2KHR)gipa(instance, "vkGetPhysicalDeviceDisplayProperties2KHR");
    table->GetPhysicalDeviceDisplayPlaneProperties2KHR = (PFN_vkGetPhysicalDeviceDisplayPlaneProperties2KHR)gipa(instance, "vkGetPhysicalDeviceDisplayPlaneProperties2KHR");
    table->GetDisplayModeProperties2KHR = (PFN_vkGetDisplayModeProperties2KHR)gipa(instance, "vkGetDisplayModeProperties2KHR");
    table->GetDisplayPlaneCapabilities2KHR = (PFN_vkGetDisplayPlaneCapabilities2KHR)gipa(instance, "vkGetDisplayPlaneCapabilities2KHR");
    table->GetPhysicalDeviceFragmentShadingRatesKHR = (PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR)gipa(instance, "vkGetPhysicalDeviceFragmentShadingRatesKHR");
    table->GetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR = (PFN_vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR)gipa(instance, "vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR");
    table->GetPhysicalDeviceCooperativeMatrixPropertiesKHR = (PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR)gipa(instance, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR");
    table->GetPhysicalDeviceCalibrateableTimeDomainsKHR = (PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR)gipa(instance, "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR");
    table->CreateDebugReportCallbackEXT = (PFN_vkCreateDebugReportCallbackEXT)gipa(instance, "vkCreateDebugReportCallbackEXT");
    table->DestroyDebugReportCallbackEXT = (PFN_vkDestroyDebugReportCallbackEXT)gipa(instance, "vkDestroyDebugReportCallbackEXT");
    table->DebugReportMessageEXT = (PFN_vkDebugReportMessageEXT)gipa(instance, "vkDebugReportMessageEXT");
#ifdef VK_USE_PLATFORM_GGP
    table->CreateStreamDescriptorSurfaceGGP = (PFN_vkCreateStreamDescriptorSurfaceGGP)gipa(instance, "vkCreateStreamDescriptorSurfaceGGP");
#endif  // VK_USE_PLATFORM_GGP
    table->GetPhysicalDeviceExternalImageFormatPropertiesNV = (PFN_vkGetPhysicalDeviceExternalImageFormatPropertiesNV)gipa(instance, "vkGetPhysicalDeviceExternalImageFormatPropertiesNV");
#ifdef VK_USE_PLATFORM_VI_NN
    table->CreateViSurfaceNN = (PFN_vkCreateViSurfaceNN)gipa(instance, "vkCreateViSurfaceNN");
#endif  // VK_USE_PLATFORM_VI_NN
    table->ReleaseDisplayEXT = (PFN_vkReleaseDisplayEXT)gipa(instance, "vkReleaseDisplayEXT");
#ifdef VK_USE_PLATFORM_XLIB_XRANDR_EXT
    table->AcquireXlibDisplayEXT = (PFN_vkAcquireXlibDisplayEXT)gipa(instance, "vkAcquireXlibDisplayEXT");
    table->GetRandROutputDisplayEXT = (PFN_vkGetRandROutputDisplayEXT)gipa(instance, "vkGetRandROutputDisplayEXT");
#endif  // VK_USE_PLATFORM_XLIB_XRANDR_EXT
    table->GetPhysicalDeviceSurfaceCapabilities2EXT = (PFN_vkGetPhysicalDeviceSurfaceCapabilities2EXT)gipa(instance, "vkGetPhysicalDeviceSurfaceCapabilities2EXT");
#ifdef VK_USE_PLATFORM_IOS_MVK
    table->CreateIOSSurfaceMVK = (PFN_vkCreateIOSSurfaceMVK)gipa(instance, "vkCreateIOSSurfaceMVK");
#endif  // VK_USE_PLATFORM_IOS_MVK
#ifdef VK_USE_PLATFORM_MACOS_MVK
    table->CreateMacOSSurfaceMVK = (PFN_vkCreateMacOSSurfaceMVK)gipa(instance, "vkCreateMacOSSurfaceMVK");
#endif  // VK_USE_PLATFORM_MACOS_MVK
    table->CreateDebugUtilsMessengerEXT = (PFN_vkCreateDebugUtilsMessengerEXT)gipa(instance, "vkCreateDebugUtilsMessengerEXT");
    table->DestroyDebugUtilsMessengerEXT = (PFN_vkDestroyDebugUtilsMessengerEXT)gipa(instance, "vkDestroyDebugUtilsMessengerEXT");
    table->SubmitDebugUtilsMessageEXT = (PFN_vkSubmitDebugUtilsMessageEXT)gipa(instance, "vkSubmitDebugUtilsMessageEXT");
    table->GetPhysicalDeviceDescriptorSizeEXT = (PFN_vkGetPhysicalDeviceDescriptorSizeEXT)gipa(instance, "vkGetPhysicalDeviceDescriptorSizeEXT");
    table->GetPhysicalDeviceMultisamplePropertiesEXT = (PFN_vkGetPhysicalDeviceMultisamplePropertiesEXT)gipa(instance, "vkGetPhysicalDeviceMultisamplePropertiesEXT");
    table->GetPhysicalDeviceCalibrateableTimeDomainsEXT = (PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT)gipa(instance, "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT");
#ifdef VK_USE_PLATFORM_FUCHSIA
    table->CreateImagePipeSurfaceFUCHSIA = (PFN_vkCreateImagePipeSurfaceFUCHSIA)gipa(instance, "vkCreateImagePipeSurfaceFUCHSIA");
#endif  // VK_USE_PLATFORM_FUCHSIA
#ifdef VK_USE_PLATFORM_METAL_EXT
    table->CreateMetalSurfaceEXT = (PFN_vkCreateMetalSurfaceEXT)gipa(instance, "vkCreateMetalSurfaceEXT");
#endif  // VK_USE_PLATFORM_METAL_EXT
    table->GetPhysicalDeviceToolPropertiesEXT = (PFN_vkGetPhysicalDeviceToolPropertiesEXT)gipa(instance, "vkGetPhysicalDeviceToolPropertiesEXT");
    table->GetPhysicalDeviceCooperativeMatrixPropertiesNV = (PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesNV)gipa(instance, "vkGetPhysicalDeviceCooperativeMatrixPropertiesNV");
    table->GetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV = (PFN_vkGetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV)gipa(instance, "vkGetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV");
#ifdef VK_USE_PLATFORM_WIN32_KHR
    table->GetPhysicalDeviceSurfacePresentModes2EXT = (PFN_vkGetPhysicalDeviceSurfacePresentModes2EXT)gipa(instance, "vkGetPhysicalDeviceSurfacePresentModes2EXT");
#endif  // VK_USE_PLATFORM_WIN32_KHR
    table->CreateHeadlessSurfaceEXT = (PFN_vkCreateHeadlessSurfaceEXT)gipa(instance, "vkCreateHeadlessSurfaceEXT");
    table->AcquireDrmDisplayEXT = (PFN_vkAcquireDrmDisplayEXT)gipa(instance, "vkAcquireDrmDisplayEXT");
    table->GetDrmDisplayEXT = (PFN_vkGetDrmDisplayEXT)gipa(instance, "vkGetDrmDisplayEXT");
#ifdef VK_USE_PLATFORM_WIN32_KHR
    table->AcquireWinrtDisplayNV = (PFN_vkAcquireWinrtDisplayNV)gipa(instance, "vkAcquireWinrtDisplayNV");
    table->GetWinrtDisplayNV = (PFN_vkGetWinrtDisplayNV)gipa(instance, "vkGetWinrtDisplayNV");
#endif  // VK_USE_PLATFORM_WIN32_KHR
#ifdef VK_USE_PLATFORM_DIRECTFB_EXT
    table->CreateDirectFBSurfaceEXT = (PFN_vkCreateDirectFBSurfaceEXT)gipa(instance, "vkCreateDirectFBSurfaceEXT");
    table->GetPhysicalDeviceDirectFBPresentationSupportEXT = (PFN_vkGetPhysicalDeviceDirectFBPresentationSupportEXT)gipa(instance, "vkGetPhysicalDeviceDirectFBPresentationSupportEXT");
#endif  // VK_USE_PLATFORM_DIRECTFB_EXT
#ifdef VK_USE_PLATFORM_SCREEN_QNX
    table->CreateScreenSurfaceQNX = (PFN_vkCreateScreenSurfaceQNX)gipa(instance, "vkCreateScreenSurfaceQNX");
    table->GetPhysicalDeviceScreenPresentationSupportQNX = (PFN_vkGetPhysicalDeviceScreenPresentationSupportQNX)gipa(instance, "vkGetPhysicalDeviceScreenPresentationSupportQNX");
#endif  // VK_USE_PLATFORM_SCREEN_QNX
    table->GetPhysicalDeviceExternalTensorPropertiesARM = (PFN_vkGetPhysicalDeviceExternalTensorPropertiesARM)gipa(instance, "vkGetPhysicalDeviceExternalTensorPropertiesARM");
    table->GetPhysicalDeviceOpticalFlowImageFormatsNV = (PFN_vkGetPhysicalDeviceOpticalFlowImageFormatsNV)gipa(instance, "vkGetPhysicalDeviceOpticalFlowImageFormatsNV");
    table->GetPhysicalDeviceCooperativeVectorPropertiesNV = (PFN_vkGetPhysicalDeviceCooperativeVectorPropertiesNV)gipa(instance, "vkGetPhysicalDeviceCooperativeVectorPropertiesNV");
    table->GetPhysicalDeviceQueueFamilyDataGraphPropertiesARM = (PFN_vkGetPhysicalDeviceQueueFamilyDataGraphPropertiesARM)gipa(instance, "vkGetPhysicalDeviceQueueFamilyDataGraphPropertiesARM");
    table->GetPhysicalDeviceQueueFamilyDataGraphProcessingEnginePropertiesARM = (PFN_vkGetPhysicalDeviceQueueFamilyDataGraphProcessingEnginePropertiesARM)gipa(instance, "vkGetPhysicalDeviceQueueFamilyDataGraphProcessingEnginePropertiesARM");
    table->GetPhysicalDeviceQueueFamilyDataGraphEngineOperationPropertiesARM = (PFN_vkGetPhysicalDeviceQueueFamilyDataGraphEngineOperationPropertiesARM)gipa(instance, "vkGetPhysicalDeviceQueueFamilyDataGraphEngineOperationPropertiesARM");
#ifdef VK_USE_PLATFORM_OHOS
    table->CreateSurfaceOHOS = (PFN_vkCreateSurfaceOHOS)gipa(instance, "vkCreateSurfaceOHOS");
#endif  // VK_USE_PLATFORM_OHOS
    table->GetPhysicalDeviceCooperativeMatrixFlexibleDimensionsPropertiesNV = (PFN_vkGetPhysicalDeviceCooperativeMatrixFlexibleDimensionsPropertiesNV)gipa(instance, "vkGetPhysicalDeviceCooperativeMatrixFlexibleDimensionsPropertiesNV");
    table->EnumeratePhysicalDeviceQueueFamilyPerformanceCountersByRegionARM = (PFN_vkEnumeratePhysicalDeviceQueueFamilyPerformanceCountersByRegionARM)gipa(instance, "vkEnumeratePhysicalDeviceQueueFamilyPerformanceCountersByRegionARM");
    table->EnumeratePhysicalDeviceShaderInstrumentationMetricsARM = (PFN_vkEnumeratePhysicalDeviceShaderInstrumentationMetricsARM)gipa(instance, "vkEnumeratePhysicalDeviceShaderInstrumentationMetricsARM");
    table->GetPhysicalDeviceQueueFamilyDataGraphOpticalFlowImageFormatsARM = (PFN_vkGetPhysicalDeviceQueueFamilyDataGraphOpticalFlowImageFormatsARM)gipa(instance, "vkGetPhysicalDeviceQueueFamilyDataGraphOpticalFlowImageFormatsARM");
#ifdef VK_USE_PLATFORM_UBM_SEC
    table->CreateUbmSurfaceSEC = (PFN_vkCreateUbmSurfaceSEC)gipa(instance, "vkCreateUbmSurfaceSEC");
    table->GetPhysicalDeviceUbmPresentationSupportSEC = (PFN_vkGetPhysicalDeviceUbmPresentationSupportSEC)gipa(instance, "vkGetPhysicalDeviceUbmPresentationSupportSEC");
#endif  // VK_USE_PLATFORM_UBM_SEC
}

#ifdef __GNUC__
    #pragma GCC diagnostic pop
#elif defined(_MSC_VER)
    #pragma warning(pop)
#endif
