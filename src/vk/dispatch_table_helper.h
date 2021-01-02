#pragma once
// *** THIS FILE IS GENERATED - DO NOT EDIT ***
// See dispatch_helper_generator.py for modifications

/*
 * Copyright (c) 2015-2020 The Khronos Group Inc.
 * Copyright (c) 2015-2020 Valve Corporation
 * Copyright (c) 2015-2020 LunarG, Inc.
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
 */

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <cstring>
#include <string>
#include <vk/dispatch_table.h>

#ifdef __GNUC__
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wpedantic"
	#pragma GCC diagnostic ignored "-Wunused-parameter"
#elif defined(_MSC_VER)
    #pragma warning(push, 3)
#endif

static inline void layer_init_device_dispatch_table(VkDevice device, VkLayerDispatchTable *table, PFN_vkGetDeviceProcAddr gpa) {
    memset(table, 0, sizeof(*table));
    // Device function pointers
    table->GetDeviceProcAddr = gpa;
    table->DestroyDevice = (PFN_vkDestroyDevice) gpa(device, "vkDestroyDevice");
    table->GetDeviceQueue = (PFN_vkGetDeviceQueue) gpa(device, "vkGetDeviceQueue");
    table->QueueSubmit = (PFN_vkQueueSubmit) gpa(device, "vkQueueSubmit");
    table->QueueWaitIdle = (PFN_vkQueueWaitIdle) gpa(device, "vkQueueWaitIdle");
    table->DeviceWaitIdle = (PFN_vkDeviceWaitIdle) gpa(device, "vkDeviceWaitIdle");
    table->AllocateMemory = (PFN_vkAllocateMemory) gpa(device, "vkAllocateMemory");
    table->FreeMemory = (PFN_vkFreeMemory) gpa(device, "vkFreeMemory");
    table->MapMemory = (PFN_vkMapMemory) gpa(device, "vkMapMemory");
    table->UnmapMemory = (PFN_vkUnmapMemory) gpa(device, "vkUnmapMemory");
    table->FlushMappedMemoryRanges = (PFN_vkFlushMappedMemoryRanges) gpa(device, "vkFlushMappedMemoryRanges");
    table->InvalidateMappedMemoryRanges = (PFN_vkInvalidateMappedMemoryRanges) gpa(device, "vkInvalidateMappedMemoryRanges");
    table->GetDeviceMemoryCommitment = (PFN_vkGetDeviceMemoryCommitment) gpa(device, "vkGetDeviceMemoryCommitment");
    table->BindBufferMemory = (PFN_vkBindBufferMemory) gpa(device, "vkBindBufferMemory");
    table->BindImageMemory = (PFN_vkBindImageMemory) gpa(device, "vkBindImageMemory");
    table->GetBufferMemoryRequirements = (PFN_vkGetBufferMemoryRequirements) gpa(device, "vkGetBufferMemoryRequirements");
    table->GetImageMemoryRequirements = (PFN_vkGetImageMemoryRequirements) gpa(device, "vkGetImageMemoryRequirements");
    table->GetImageSparseMemoryRequirements = (PFN_vkGetImageSparseMemoryRequirements) gpa(device, "vkGetImageSparseMemoryRequirements");
    table->QueueBindSparse = (PFN_vkQueueBindSparse) gpa(device, "vkQueueBindSparse");
    table->CreateFence = (PFN_vkCreateFence) gpa(device, "vkCreateFence");
    table->DestroyFence = (PFN_vkDestroyFence) gpa(device, "vkDestroyFence");
    table->ResetFences = (PFN_vkResetFences) gpa(device, "vkResetFences");
    table->GetFenceStatus = (PFN_vkGetFenceStatus) gpa(device, "vkGetFenceStatus");
    table->WaitForFences = (PFN_vkWaitForFences) gpa(device, "vkWaitForFences");
    table->CreateSemaphore = (PFN_vkCreateSemaphore) gpa(device, "vkCreateSemaphore");
    table->DestroySemaphore = (PFN_vkDestroySemaphore) gpa(device, "vkDestroySemaphore");
    table->CreateEvent = (PFN_vkCreateEvent) gpa(device, "vkCreateEvent");
    table->DestroyEvent = (PFN_vkDestroyEvent) gpa(device, "vkDestroyEvent");
    table->GetEventStatus = (PFN_vkGetEventStatus) gpa(device, "vkGetEventStatus");
    table->SetEvent = (PFN_vkSetEvent) gpa(device, "vkSetEvent");
    table->ResetEvent = (PFN_vkResetEvent) gpa(device, "vkResetEvent");
    table->CreateQueryPool = (PFN_vkCreateQueryPool) gpa(device, "vkCreateQueryPool");
    table->DestroyQueryPool = (PFN_vkDestroyQueryPool) gpa(device, "vkDestroyQueryPool");
    table->GetQueryPoolResults = (PFN_vkGetQueryPoolResults) gpa(device, "vkGetQueryPoolResults");
    table->CreateBuffer = (PFN_vkCreateBuffer) gpa(device, "vkCreateBuffer");
    table->DestroyBuffer = (PFN_vkDestroyBuffer) gpa(device, "vkDestroyBuffer");
    table->CreateBufferView = (PFN_vkCreateBufferView) gpa(device, "vkCreateBufferView");
    table->DestroyBufferView = (PFN_vkDestroyBufferView) gpa(device, "vkDestroyBufferView");
    table->CreateImage = (PFN_vkCreateImage) gpa(device, "vkCreateImage");
    table->DestroyImage = (PFN_vkDestroyImage) gpa(device, "vkDestroyImage");
    table->GetImageSubresourceLayout = (PFN_vkGetImageSubresourceLayout) gpa(device, "vkGetImageSubresourceLayout");
    table->CreateImageView = (PFN_vkCreateImageView) gpa(device, "vkCreateImageView");
    table->DestroyImageView = (PFN_vkDestroyImageView) gpa(device, "vkDestroyImageView");
    table->CreateShaderModule = (PFN_vkCreateShaderModule) gpa(device, "vkCreateShaderModule");
    table->DestroyShaderModule = (PFN_vkDestroyShaderModule) gpa(device, "vkDestroyShaderModule");
    table->CreatePipelineCache = (PFN_vkCreatePipelineCache) gpa(device, "vkCreatePipelineCache");
    table->DestroyPipelineCache = (PFN_vkDestroyPipelineCache) gpa(device, "vkDestroyPipelineCache");
    table->GetPipelineCacheData = (PFN_vkGetPipelineCacheData) gpa(device, "vkGetPipelineCacheData");
    table->MergePipelineCaches = (PFN_vkMergePipelineCaches) gpa(device, "vkMergePipelineCaches");
    table->CreateGraphicsPipelines = (PFN_vkCreateGraphicsPipelines) gpa(device, "vkCreateGraphicsPipelines");
    table->CreateComputePipelines = (PFN_vkCreateComputePipelines) gpa(device, "vkCreateComputePipelines");
    table->DestroyPipeline = (PFN_vkDestroyPipeline) gpa(device, "vkDestroyPipeline");
    table->CreatePipelineLayout = (PFN_vkCreatePipelineLayout) gpa(device, "vkCreatePipelineLayout");
    table->DestroyPipelineLayout = (PFN_vkDestroyPipelineLayout) gpa(device, "vkDestroyPipelineLayout");
    table->CreateSampler = (PFN_vkCreateSampler) gpa(device, "vkCreateSampler");
    table->DestroySampler = (PFN_vkDestroySampler) gpa(device, "vkDestroySampler");
    table->CreateDescriptorSetLayout = (PFN_vkCreateDescriptorSetLayout) gpa(device, "vkCreateDescriptorSetLayout");
    table->DestroyDescriptorSetLayout = (PFN_vkDestroyDescriptorSetLayout) gpa(device, "vkDestroyDescriptorSetLayout");
    table->CreateDescriptorPool = (PFN_vkCreateDescriptorPool) gpa(device, "vkCreateDescriptorPool");
    table->DestroyDescriptorPool = (PFN_vkDestroyDescriptorPool) gpa(device, "vkDestroyDescriptorPool");
    table->ResetDescriptorPool = (PFN_vkResetDescriptorPool) gpa(device, "vkResetDescriptorPool");
    table->AllocateDescriptorSets = (PFN_vkAllocateDescriptorSets) gpa(device, "vkAllocateDescriptorSets");
    table->FreeDescriptorSets = (PFN_vkFreeDescriptorSets) gpa(device, "vkFreeDescriptorSets");
    table->UpdateDescriptorSets = (PFN_vkUpdateDescriptorSets) gpa(device, "vkUpdateDescriptorSets");
    table->CreateFramebuffer = (PFN_vkCreateFramebuffer) gpa(device, "vkCreateFramebuffer");
    table->DestroyFramebuffer = (PFN_vkDestroyFramebuffer) gpa(device, "vkDestroyFramebuffer");
    table->CreateRenderPass = (PFN_vkCreateRenderPass) gpa(device, "vkCreateRenderPass");
    table->DestroyRenderPass = (PFN_vkDestroyRenderPass) gpa(device, "vkDestroyRenderPass");
    table->GetRenderAreaGranularity = (PFN_vkGetRenderAreaGranularity) gpa(device, "vkGetRenderAreaGranularity");
    table->CreateCommandPool = (PFN_vkCreateCommandPool) gpa(device, "vkCreateCommandPool");
    table->DestroyCommandPool = (PFN_vkDestroyCommandPool) gpa(device, "vkDestroyCommandPool");
    table->ResetCommandPool = (PFN_vkResetCommandPool) gpa(device, "vkResetCommandPool");
    table->AllocateCommandBuffers = (PFN_vkAllocateCommandBuffers) gpa(device, "vkAllocateCommandBuffers");
    table->FreeCommandBuffers = (PFN_vkFreeCommandBuffers) gpa(device, "vkFreeCommandBuffers");
    table->BeginCommandBuffer = (PFN_vkBeginCommandBuffer) gpa(device, "vkBeginCommandBuffer");
    table->EndCommandBuffer = (PFN_vkEndCommandBuffer) gpa(device, "vkEndCommandBuffer");
    table->ResetCommandBuffer = (PFN_vkResetCommandBuffer) gpa(device, "vkResetCommandBuffer");
    table->CmdBindPipeline = (PFN_vkCmdBindPipeline) gpa(device, "vkCmdBindPipeline");
    table->CmdSetViewport = (PFN_vkCmdSetViewport) gpa(device, "vkCmdSetViewport");
    table->CmdSetScissor = (PFN_vkCmdSetScissor) gpa(device, "vkCmdSetScissor");
    table->CmdSetLineWidth = (PFN_vkCmdSetLineWidth) gpa(device, "vkCmdSetLineWidth");
    table->CmdSetDepthBias = (PFN_vkCmdSetDepthBias) gpa(device, "vkCmdSetDepthBias");
    table->CmdSetBlendConstants = (PFN_vkCmdSetBlendConstants) gpa(device, "vkCmdSetBlendConstants");
    table->CmdSetDepthBounds = (PFN_vkCmdSetDepthBounds) gpa(device, "vkCmdSetDepthBounds");
    table->CmdSetStencilCompareMask = (PFN_vkCmdSetStencilCompareMask) gpa(device, "vkCmdSetStencilCompareMask");
    table->CmdSetStencilWriteMask = (PFN_vkCmdSetStencilWriteMask) gpa(device, "vkCmdSetStencilWriteMask");
    table->CmdSetStencilReference = (PFN_vkCmdSetStencilReference) gpa(device, "vkCmdSetStencilReference");
    table->CmdBindDescriptorSets = (PFN_vkCmdBindDescriptorSets) gpa(device, "vkCmdBindDescriptorSets");
    table->CmdBindIndexBuffer = (PFN_vkCmdBindIndexBuffer) gpa(device, "vkCmdBindIndexBuffer");
    table->CmdBindVertexBuffers = (PFN_vkCmdBindVertexBuffers) gpa(device, "vkCmdBindVertexBuffers");
    table->CmdDraw = (PFN_vkCmdDraw) gpa(device, "vkCmdDraw");
    table->CmdDrawIndexed = (PFN_vkCmdDrawIndexed) gpa(device, "vkCmdDrawIndexed");
    table->CmdDrawIndirect = (PFN_vkCmdDrawIndirect) gpa(device, "vkCmdDrawIndirect");
    table->CmdDrawIndexedIndirect = (PFN_vkCmdDrawIndexedIndirect) gpa(device, "vkCmdDrawIndexedIndirect");
    table->CmdDispatch = (PFN_vkCmdDispatch) gpa(device, "vkCmdDispatch");
    table->CmdDispatchIndirect = (PFN_vkCmdDispatchIndirect) gpa(device, "vkCmdDispatchIndirect");
    table->CmdCopyBuffer = (PFN_vkCmdCopyBuffer) gpa(device, "vkCmdCopyBuffer");
    table->CmdCopyImage = (PFN_vkCmdCopyImage) gpa(device, "vkCmdCopyImage");
    table->CmdBlitImage = (PFN_vkCmdBlitImage) gpa(device, "vkCmdBlitImage");
    table->CmdCopyBufferToImage = (PFN_vkCmdCopyBufferToImage) gpa(device, "vkCmdCopyBufferToImage");
    table->CmdCopyImageToBuffer = (PFN_vkCmdCopyImageToBuffer) gpa(device, "vkCmdCopyImageToBuffer");
    table->CmdUpdateBuffer = (PFN_vkCmdUpdateBuffer) gpa(device, "vkCmdUpdateBuffer");
    table->CmdFillBuffer = (PFN_vkCmdFillBuffer) gpa(device, "vkCmdFillBuffer");
    table->CmdClearColorImage = (PFN_vkCmdClearColorImage) gpa(device, "vkCmdClearColorImage");
    table->CmdClearDepthStencilImage = (PFN_vkCmdClearDepthStencilImage) gpa(device, "vkCmdClearDepthStencilImage");
    table->CmdClearAttachments = (PFN_vkCmdClearAttachments) gpa(device, "vkCmdClearAttachments");
    table->CmdResolveImage = (PFN_vkCmdResolveImage) gpa(device, "vkCmdResolveImage");
    table->CmdSetEvent = (PFN_vkCmdSetEvent) gpa(device, "vkCmdSetEvent");
    table->CmdResetEvent = (PFN_vkCmdResetEvent) gpa(device, "vkCmdResetEvent");
    table->CmdWaitEvents = (PFN_vkCmdWaitEvents) gpa(device, "vkCmdWaitEvents");
    table->CmdPipelineBarrier = (PFN_vkCmdPipelineBarrier) gpa(device, "vkCmdPipelineBarrier");
    table->CmdBeginQuery = (PFN_vkCmdBeginQuery) gpa(device, "vkCmdBeginQuery");
    table->CmdEndQuery = (PFN_vkCmdEndQuery) gpa(device, "vkCmdEndQuery");
    table->CmdResetQueryPool = (PFN_vkCmdResetQueryPool) gpa(device, "vkCmdResetQueryPool");
    table->CmdWriteTimestamp = (PFN_vkCmdWriteTimestamp) gpa(device, "vkCmdWriteTimestamp");
    table->CmdCopyQueryPoolResults = (PFN_vkCmdCopyQueryPoolResults) gpa(device, "vkCmdCopyQueryPoolResults");
    table->CmdPushConstants = (PFN_vkCmdPushConstants) gpa(device, "vkCmdPushConstants");
    table->CmdBeginRenderPass = (PFN_vkCmdBeginRenderPass) gpa(device, "vkCmdBeginRenderPass");
    table->CmdNextSubpass = (PFN_vkCmdNextSubpass) gpa(device, "vkCmdNextSubpass");
    table->CmdEndRenderPass = (PFN_vkCmdEndRenderPass) gpa(device, "vkCmdEndRenderPass");
    table->CmdExecuteCommands = (PFN_vkCmdExecuteCommands) gpa(device, "vkCmdExecuteCommands");
    table->BindBufferMemory2 = (PFN_vkBindBufferMemory2) gpa(device, "vkBindBufferMemory2");
    // if (table->BindBufferMemory2 == nullptr) { table->BindBufferMemory2 = (PFN_vkBindBufferMemory2)StubBindBufferMemory2; }
    table->BindImageMemory2 = (PFN_vkBindImageMemory2) gpa(device, "vkBindImageMemory2");
    // if (table->BindImageMemory2 == nullptr) { table->BindImageMemory2 = (PFN_vkBindImageMemory2)StubBindImageMemory2; }
    table->GetDeviceGroupPeerMemoryFeatures = (PFN_vkGetDeviceGroupPeerMemoryFeatures) gpa(device, "vkGetDeviceGroupPeerMemoryFeatures");
    // if (table->GetDeviceGroupPeerMemoryFeatures == nullptr) { table->GetDeviceGroupPeerMemoryFeatures = (PFN_vkGetDeviceGroupPeerMemoryFeatures)StubGetDeviceGroupPeerMemoryFeatures; }
    table->CmdSetDeviceMask = (PFN_vkCmdSetDeviceMask) gpa(device, "vkCmdSetDeviceMask");
    // if (table->CmdSetDeviceMask == nullptr) { table->CmdSetDeviceMask = (PFN_vkCmdSetDeviceMask)StubCmdSetDeviceMask; }
    table->CmdDispatchBase = (PFN_vkCmdDispatchBase) gpa(device, "vkCmdDispatchBase");
    // if (table->CmdDispatchBase == nullptr) { table->CmdDispatchBase = (PFN_vkCmdDispatchBase)StubCmdDispatchBase; }
    table->GetImageMemoryRequirements2 = (PFN_vkGetImageMemoryRequirements2) gpa(device, "vkGetImageMemoryRequirements2");
    // if (table->GetImageMemoryRequirements2 == nullptr) { table->GetImageMemoryRequirements2 = (PFN_vkGetImageMemoryRequirements2)StubGetImageMemoryRequirements2; }
    table->GetBufferMemoryRequirements2 = (PFN_vkGetBufferMemoryRequirements2) gpa(device, "vkGetBufferMemoryRequirements2");
    // if (table->GetBufferMemoryRequirements2 == nullptr) { table->GetBufferMemoryRequirements2 = (PFN_vkGetBufferMemoryRequirements2)StubGetBufferMemoryRequirements2; }
    table->GetImageSparseMemoryRequirements2 = (PFN_vkGetImageSparseMemoryRequirements2) gpa(device, "vkGetImageSparseMemoryRequirements2");
    // if (table->GetImageSparseMemoryRequirements2 == nullptr) { table->GetImageSparseMemoryRequirements2 = (PFN_vkGetImageSparseMemoryRequirements2)StubGetImageSparseMemoryRequirements2; }
    table->TrimCommandPool = (PFN_vkTrimCommandPool) gpa(device, "vkTrimCommandPool");
    // if (table->TrimCommandPool == nullptr) { table->TrimCommandPool = (PFN_vkTrimCommandPool)StubTrimCommandPool; }
    table->GetDeviceQueue2 = (PFN_vkGetDeviceQueue2) gpa(device, "vkGetDeviceQueue2");
    // if (table->GetDeviceQueue2 == nullptr) { table->GetDeviceQueue2 = (PFN_vkGetDeviceQueue2)StubGetDeviceQueue2; }
    table->CreateSamplerYcbcrConversion = (PFN_vkCreateSamplerYcbcrConversion) gpa(device, "vkCreateSamplerYcbcrConversion");
    // if (table->CreateSamplerYcbcrConversion == nullptr) { table->CreateSamplerYcbcrConversion = (PFN_vkCreateSamplerYcbcrConversion)StubCreateSamplerYcbcrConversion; }
    table->DestroySamplerYcbcrConversion = (PFN_vkDestroySamplerYcbcrConversion) gpa(device, "vkDestroySamplerYcbcrConversion");
    // if (table->DestroySamplerYcbcrConversion == nullptr) { table->DestroySamplerYcbcrConversion = (PFN_vkDestroySamplerYcbcrConversion)StubDestroySamplerYcbcrConversion; }
    table->CreateDescriptorUpdateTemplate = (PFN_vkCreateDescriptorUpdateTemplate) gpa(device, "vkCreateDescriptorUpdateTemplate");
    // if (table->CreateDescriptorUpdateTemplate == nullptr) { table->CreateDescriptorUpdateTemplate = (PFN_vkCreateDescriptorUpdateTemplate)StubCreateDescriptorUpdateTemplate; }
    table->DestroyDescriptorUpdateTemplate = (PFN_vkDestroyDescriptorUpdateTemplate) gpa(device, "vkDestroyDescriptorUpdateTemplate");
    // if (table->DestroyDescriptorUpdateTemplate == nullptr) { table->DestroyDescriptorUpdateTemplate = (PFN_vkDestroyDescriptorUpdateTemplate)StubDestroyDescriptorUpdateTemplate; }
    table->UpdateDescriptorSetWithTemplate = (PFN_vkUpdateDescriptorSetWithTemplate) gpa(device, "vkUpdateDescriptorSetWithTemplate");
    // if (table->UpdateDescriptorSetWithTemplate == nullptr) { table->UpdateDescriptorSetWithTemplate = (PFN_vkUpdateDescriptorSetWithTemplate)StubUpdateDescriptorSetWithTemplate; }
    table->GetDescriptorSetLayoutSupport = (PFN_vkGetDescriptorSetLayoutSupport) gpa(device, "vkGetDescriptorSetLayoutSupport");
    // if (table->GetDescriptorSetLayoutSupport == nullptr) { table->GetDescriptorSetLayoutSupport = (PFN_vkGetDescriptorSetLayoutSupport)StubGetDescriptorSetLayoutSupport; }
    table->CmdDrawIndirectCount = (PFN_vkCmdDrawIndirectCount) gpa(device, "vkCmdDrawIndirectCount");
    // if (table->CmdDrawIndirectCount == nullptr) { table->CmdDrawIndirectCount = (PFN_vkCmdDrawIndirectCount)StubCmdDrawIndirectCount; }
    table->CmdDrawIndexedIndirectCount = (PFN_vkCmdDrawIndexedIndirectCount) gpa(device, "vkCmdDrawIndexedIndirectCount");
    // if (table->CmdDrawIndexedIndirectCount == nullptr) { table->CmdDrawIndexedIndirectCount = (PFN_vkCmdDrawIndexedIndirectCount)StubCmdDrawIndexedIndirectCount; }
    table->CreateRenderPass2 = (PFN_vkCreateRenderPass2) gpa(device, "vkCreateRenderPass2");
    // if (table->CreateRenderPass2 == nullptr) { table->CreateRenderPass2 = (PFN_vkCreateRenderPass2)StubCreateRenderPass2; }
    table->CmdBeginRenderPass2 = (PFN_vkCmdBeginRenderPass2) gpa(device, "vkCmdBeginRenderPass2");
    // if (table->CmdBeginRenderPass2 == nullptr) { table->CmdBeginRenderPass2 = (PFN_vkCmdBeginRenderPass2)StubCmdBeginRenderPass2; }
    table->CmdNextSubpass2 = (PFN_vkCmdNextSubpass2) gpa(device, "vkCmdNextSubpass2");
    // if (table->CmdNextSubpass2 == nullptr) { table->CmdNextSubpass2 = (PFN_vkCmdNextSubpass2)StubCmdNextSubpass2; }
    table->CmdEndRenderPass2 = (PFN_vkCmdEndRenderPass2) gpa(device, "vkCmdEndRenderPass2");
    // if (table->CmdEndRenderPass2 == nullptr) { table->CmdEndRenderPass2 = (PFN_vkCmdEndRenderPass2)StubCmdEndRenderPass2; }
    table->ResetQueryPool = (PFN_vkResetQueryPool) gpa(device, "vkResetQueryPool");
    // if (table->ResetQueryPool == nullptr) { table->ResetQueryPool = (PFN_vkResetQueryPool)StubResetQueryPool; }
    table->GetSemaphoreCounterValue = (PFN_vkGetSemaphoreCounterValue) gpa(device, "vkGetSemaphoreCounterValue");
    // if (table->GetSemaphoreCounterValue == nullptr) { table->GetSemaphoreCounterValue = (PFN_vkGetSemaphoreCounterValue)StubGetSemaphoreCounterValue; }
    table->WaitSemaphores = (PFN_vkWaitSemaphores) gpa(device, "vkWaitSemaphores");
    // if (table->WaitSemaphores == nullptr) { table->WaitSemaphores = (PFN_vkWaitSemaphores)StubWaitSemaphores; }
    table->SignalSemaphore = (PFN_vkSignalSemaphore) gpa(device, "vkSignalSemaphore");
    // if (table->SignalSemaphore == nullptr) { table->SignalSemaphore = (PFN_vkSignalSemaphore)StubSignalSemaphore; }
    table->GetBufferDeviceAddress = (PFN_vkGetBufferDeviceAddress) gpa(device, "vkGetBufferDeviceAddress");
    // if (table->GetBufferDeviceAddress == nullptr) { table->GetBufferDeviceAddress = (PFN_vkGetBufferDeviceAddress)StubGetBufferDeviceAddress; }
    table->GetBufferOpaqueCaptureAddress = (PFN_vkGetBufferOpaqueCaptureAddress) gpa(device, "vkGetBufferOpaqueCaptureAddress");
    // if (table->GetBufferOpaqueCaptureAddress == nullptr) { table->GetBufferOpaqueCaptureAddress = (PFN_vkGetBufferOpaqueCaptureAddress)StubGetBufferOpaqueCaptureAddress; }
    table->GetDeviceMemoryOpaqueCaptureAddress = (PFN_vkGetDeviceMemoryOpaqueCaptureAddress) gpa(device, "vkGetDeviceMemoryOpaqueCaptureAddress");
    // if (table->GetDeviceMemoryOpaqueCaptureAddress == nullptr) { table->GetDeviceMemoryOpaqueCaptureAddress = (PFN_vkGetDeviceMemoryOpaqueCaptureAddress)StubGetDeviceMemoryOpaqueCaptureAddress; }
    table->CreateSwapchainKHR = (PFN_vkCreateSwapchainKHR) gpa(device, "vkCreateSwapchainKHR");
    // if (table->CreateSwapchainKHR == nullptr) { table->CreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)StubCreateSwapchainKHR; }
    table->DestroySwapchainKHR = (PFN_vkDestroySwapchainKHR) gpa(device, "vkDestroySwapchainKHR");
    // if (table->DestroySwapchainKHR == nullptr) { table->DestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)StubDestroySwapchainKHR; }
    table->GetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR) gpa(device, "vkGetSwapchainImagesKHR");
    // if (table->GetSwapchainImagesKHR == nullptr) { table->GetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)StubGetSwapchainImagesKHR; }
    table->AcquireNextImageKHR = (PFN_vkAcquireNextImageKHR) gpa(device, "vkAcquireNextImageKHR");
    // if (table->AcquireNextImageKHR == nullptr) { table->AcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)StubAcquireNextImageKHR; }
    table->QueuePresentKHR = (PFN_vkQueuePresentKHR) gpa(device, "vkQueuePresentKHR");
    // if (table->QueuePresentKHR == nullptr) { table->QueuePresentKHR = (PFN_vkQueuePresentKHR)StubQueuePresentKHR; }
    table->GetDeviceGroupPresentCapabilitiesKHR = (PFN_vkGetDeviceGroupPresentCapabilitiesKHR) gpa(device, "vkGetDeviceGroupPresentCapabilitiesKHR");
    // if (table->GetDeviceGroupPresentCapabilitiesKHR == nullptr) { table->GetDeviceGroupPresentCapabilitiesKHR = (PFN_vkGetDeviceGroupPresentCapabilitiesKHR)StubGetDeviceGroupPresentCapabilitiesKHR; }
    table->GetDeviceGroupSurfacePresentModesKHR = (PFN_vkGetDeviceGroupSurfacePresentModesKHR) gpa(device, "vkGetDeviceGroupSurfacePresentModesKHR");
    // if (table->GetDeviceGroupSurfacePresentModesKHR == nullptr) { table->GetDeviceGroupSurfacePresentModesKHR = (PFN_vkGetDeviceGroupSurfacePresentModesKHR)StubGetDeviceGroupSurfacePresentModesKHR; }
    table->AcquireNextImage2KHR = (PFN_vkAcquireNextImage2KHR) gpa(device, "vkAcquireNextImage2KHR");
    // if (table->AcquireNextImage2KHR == nullptr) { table->AcquireNextImage2KHR = (PFN_vkAcquireNextImage2KHR)StubAcquireNextImage2KHR; }
    table->CreateSharedSwapchainsKHR = (PFN_vkCreateSharedSwapchainsKHR) gpa(device, "vkCreateSharedSwapchainsKHR");
    // if (table->CreateSharedSwapchainsKHR == nullptr) { table->CreateSharedSwapchainsKHR = (PFN_vkCreateSharedSwapchainsKHR)StubCreateSharedSwapchainsKHR; }
    table->GetDeviceGroupPeerMemoryFeaturesKHR = (PFN_vkGetDeviceGroupPeerMemoryFeaturesKHR) gpa(device, "vkGetDeviceGroupPeerMemoryFeaturesKHR");
    // if (table->GetDeviceGroupPeerMemoryFeaturesKHR == nullptr) { table->GetDeviceGroupPeerMemoryFeaturesKHR = (PFN_vkGetDeviceGroupPeerMemoryFeaturesKHR)StubGetDeviceGroupPeerMemoryFeaturesKHR; }
    table->CmdSetDeviceMaskKHR = (PFN_vkCmdSetDeviceMaskKHR) gpa(device, "vkCmdSetDeviceMaskKHR");
    // if (table->CmdSetDeviceMaskKHR == nullptr) { table->CmdSetDeviceMaskKHR = (PFN_vkCmdSetDeviceMaskKHR)StubCmdSetDeviceMaskKHR; }
    table->CmdDispatchBaseKHR = (PFN_vkCmdDispatchBaseKHR) gpa(device, "vkCmdDispatchBaseKHR");
    // if (table->CmdDispatchBaseKHR == nullptr) { table->CmdDispatchBaseKHR = (PFN_vkCmdDispatchBaseKHR)StubCmdDispatchBaseKHR; }
    table->TrimCommandPoolKHR = (PFN_vkTrimCommandPoolKHR) gpa(device, "vkTrimCommandPoolKHR");
    // if (table->TrimCommandPoolKHR == nullptr) { table->TrimCommandPoolKHR = (PFN_vkTrimCommandPoolKHR)StubTrimCommandPoolKHR; }
#ifdef VK_USE_PLATFORM_WIN32_KHR
    table->GetMemoryWin32HandleKHR = (PFN_vkGetMemoryWin32HandleKHR) gpa(device, "vkGetMemoryWin32HandleKHR");
    // if (table->GetMemoryWin32HandleKHR == nullptr) { table->GetMemoryWin32HandleKHR = (PFN_vkGetMemoryWin32HandleKHR)StubGetMemoryWin32HandleKHR; }
#endif // VK_USE_PLATFORM_WIN32_KHR
#ifdef VK_USE_PLATFORM_WIN32_KHR
    table->GetMemoryWin32HandlePropertiesKHR = (PFN_vkGetMemoryWin32HandlePropertiesKHR) gpa(device, "vkGetMemoryWin32HandlePropertiesKHR");
    // if (table->GetMemoryWin32HandlePropertiesKHR == nullptr) { table->GetMemoryWin32HandlePropertiesKHR = (PFN_vkGetMemoryWin32HandlePropertiesKHR)StubGetMemoryWin32HandlePropertiesKHR; }
#endif // VK_USE_PLATFORM_WIN32_KHR
    table->GetMemoryFdKHR = (PFN_vkGetMemoryFdKHR) gpa(device, "vkGetMemoryFdKHR");
    // if (table->GetMemoryFdKHR == nullptr) { table->GetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)StubGetMemoryFdKHR; }
    table->GetMemoryFdPropertiesKHR = (PFN_vkGetMemoryFdPropertiesKHR) gpa(device, "vkGetMemoryFdPropertiesKHR");
    // if (table->GetMemoryFdPropertiesKHR == nullptr) { table->GetMemoryFdPropertiesKHR = (PFN_vkGetMemoryFdPropertiesKHR)StubGetMemoryFdPropertiesKHR; }
#ifdef VK_USE_PLATFORM_WIN32_KHR
    table->ImportSemaphoreWin32HandleKHR = (PFN_vkImportSemaphoreWin32HandleKHR) gpa(device, "vkImportSemaphoreWin32HandleKHR");
    // if (table->ImportSemaphoreWin32HandleKHR == nullptr) { table->ImportSemaphoreWin32HandleKHR = (PFN_vkImportSemaphoreWin32HandleKHR)StubImportSemaphoreWin32HandleKHR; }
#endif // VK_USE_PLATFORM_WIN32_KHR
#ifdef VK_USE_PLATFORM_WIN32_KHR
    table->GetSemaphoreWin32HandleKHR = (PFN_vkGetSemaphoreWin32HandleKHR) gpa(device, "vkGetSemaphoreWin32HandleKHR");
    // if (table->GetSemaphoreWin32HandleKHR == nullptr) { table->GetSemaphoreWin32HandleKHR = (PFN_vkGetSemaphoreWin32HandleKHR)StubGetSemaphoreWin32HandleKHR; }
#endif // VK_USE_PLATFORM_WIN32_KHR
    table->ImportSemaphoreFdKHR = (PFN_vkImportSemaphoreFdKHR) gpa(device, "vkImportSemaphoreFdKHR");
    // if (table->ImportSemaphoreFdKHR == nullptr) { table->ImportSemaphoreFdKHR = (PFN_vkImportSemaphoreFdKHR)StubImportSemaphoreFdKHR; }
    table->GetSemaphoreFdKHR = (PFN_vkGetSemaphoreFdKHR) gpa(device, "vkGetSemaphoreFdKHR");
    // if (table->GetSemaphoreFdKHR == nullptr) { table->GetSemaphoreFdKHR = (PFN_vkGetSemaphoreFdKHR)StubGetSemaphoreFdKHR; }
    table->CmdPushDescriptorSetKHR = (PFN_vkCmdPushDescriptorSetKHR) gpa(device, "vkCmdPushDescriptorSetKHR");
    // if (table->CmdPushDescriptorSetKHR == nullptr) { table->CmdPushDescriptorSetKHR = (PFN_vkCmdPushDescriptorSetKHR)StubCmdPushDescriptorSetKHR; }
    table->CmdPushDescriptorSetWithTemplateKHR = (PFN_vkCmdPushDescriptorSetWithTemplateKHR) gpa(device, "vkCmdPushDescriptorSetWithTemplateKHR");
    // if (table->CmdPushDescriptorSetWithTemplateKHR == nullptr) { table->CmdPushDescriptorSetWithTemplateKHR = (PFN_vkCmdPushDescriptorSetWithTemplateKHR)StubCmdPushDescriptorSetWithTemplateKHR; }
    table->CreateDescriptorUpdateTemplateKHR = (PFN_vkCreateDescriptorUpdateTemplateKHR) gpa(device, "vkCreateDescriptorUpdateTemplateKHR");
    // if (table->CreateDescriptorUpdateTemplateKHR == nullptr) { table->CreateDescriptorUpdateTemplateKHR = (PFN_vkCreateDescriptorUpdateTemplateKHR)StubCreateDescriptorUpdateTemplateKHR; }
    table->DestroyDescriptorUpdateTemplateKHR = (PFN_vkDestroyDescriptorUpdateTemplateKHR) gpa(device, "vkDestroyDescriptorUpdateTemplateKHR");
    // if (table->DestroyDescriptorUpdateTemplateKHR == nullptr) { table->DestroyDescriptorUpdateTemplateKHR = (PFN_vkDestroyDescriptorUpdateTemplateKHR)StubDestroyDescriptorUpdateTemplateKHR; }
    table->UpdateDescriptorSetWithTemplateKHR = (PFN_vkUpdateDescriptorSetWithTemplateKHR) gpa(device, "vkUpdateDescriptorSetWithTemplateKHR");
    // if (table->UpdateDescriptorSetWithTemplateKHR == nullptr) { table->UpdateDescriptorSetWithTemplateKHR = (PFN_vkUpdateDescriptorSetWithTemplateKHR)StubUpdateDescriptorSetWithTemplateKHR; }
    table->CreateRenderPass2KHR = (PFN_vkCreateRenderPass2KHR) gpa(device, "vkCreateRenderPass2KHR");
    // if (table->CreateRenderPass2KHR == nullptr) { table->CreateRenderPass2KHR = (PFN_vkCreateRenderPass2KHR)StubCreateRenderPass2KHR; }
    table->CmdBeginRenderPass2KHR = (PFN_vkCmdBeginRenderPass2KHR) gpa(device, "vkCmdBeginRenderPass2KHR");
    // if (table->CmdBeginRenderPass2KHR == nullptr) { table->CmdBeginRenderPass2KHR = (PFN_vkCmdBeginRenderPass2KHR)StubCmdBeginRenderPass2KHR; }
    table->CmdNextSubpass2KHR = (PFN_vkCmdNextSubpass2KHR) gpa(device, "vkCmdNextSubpass2KHR");
    // if (table->CmdNextSubpass2KHR == nullptr) { table->CmdNextSubpass2KHR = (PFN_vkCmdNextSubpass2KHR)StubCmdNextSubpass2KHR; }
    table->CmdEndRenderPass2KHR = (PFN_vkCmdEndRenderPass2KHR) gpa(device, "vkCmdEndRenderPass2KHR");
    // if (table->CmdEndRenderPass2KHR == nullptr) { table->CmdEndRenderPass2KHR = (PFN_vkCmdEndRenderPass2KHR)StubCmdEndRenderPass2KHR; }
    table->GetSwapchainStatusKHR = (PFN_vkGetSwapchainStatusKHR) gpa(device, "vkGetSwapchainStatusKHR");
    // if (table->GetSwapchainStatusKHR == nullptr) { table->GetSwapchainStatusKHR = (PFN_vkGetSwapchainStatusKHR)StubGetSwapchainStatusKHR; }
#ifdef VK_USE_PLATFORM_WIN32_KHR
    table->ImportFenceWin32HandleKHR = (PFN_vkImportFenceWin32HandleKHR) gpa(device, "vkImportFenceWin32HandleKHR");
    // if (table->ImportFenceWin32HandleKHR == nullptr) { table->ImportFenceWin32HandleKHR = (PFN_vkImportFenceWin32HandleKHR)StubImportFenceWin32HandleKHR; }
#endif // VK_USE_PLATFORM_WIN32_KHR
#ifdef VK_USE_PLATFORM_WIN32_KHR
    table->GetFenceWin32HandleKHR = (PFN_vkGetFenceWin32HandleKHR) gpa(device, "vkGetFenceWin32HandleKHR");
    // if (table->GetFenceWin32HandleKHR == nullptr) { table->GetFenceWin32HandleKHR = (PFN_vkGetFenceWin32HandleKHR)StubGetFenceWin32HandleKHR; }
#endif // VK_USE_PLATFORM_WIN32_KHR
    table->ImportFenceFdKHR = (PFN_vkImportFenceFdKHR) gpa(device, "vkImportFenceFdKHR");
    // if (table->ImportFenceFdKHR == nullptr) { table->ImportFenceFdKHR = (PFN_vkImportFenceFdKHR)StubImportFenceFdKHR; }
    table->GetFenceFdKHR = (PFN_vkGetFenceFdKHR) gpa(device, "vkGetFenceFdKHR");
    // if (table->GetFenceFdKHR == nullptr) { table->GetFenceFdKHR = (PFN_vkGetFenceFdKHR)StubGetFenceFdKHR; }
    table->AcquireProfilingLockKHR = (PFN_vkAcquireProfilingLockKHR) gpa(device, "vkAcquireProfilingLockKHR");
    // if (table->AcquireProfilingLockKHR == nullptr) { table->AcquireProfilingLockKHR = (PFN_vkAcquireProfilingLockKHR)StubAcquireProfilingLockKHR; }
    table->ReleaseProfilingLockKHR = (PFN_vkReleaseProfilingLockKHR) gpa(device, "vkReleaseProfilingLockKHR");
    // if (table->ReleaseProfilingLockKHR == nullptr) { table->ReleaseProfilingLockKHR = (PFN_vkReleaseProfilingLockKHR)StubReleaseProfilingLockKHR; }
    table->GetImageMemoryRequirements2KHR = (PFN_vkGetImageMemoryRequirements2KHR) gpa(device, "vkGetImageMemoryRequirements2KHR");
    // if (table->GetImageMemoryRequirements2KHR == nullptr) { table->GetImageMemoryRequirements2KHR = (PFN_vkGetImageMemoryRequirements2KHR)StubGetImageMemoryRequirements2KHR; }
    table->GetBufferMemoryRequirements2KHR = (PFN_vkGetBufferMemoryRequirements2KHR) gpa(device, "vkGetBufferMemoryRequirements2KHR");
    // if (table->GetBufferMemoryRequirements2KHR == nullptr) { table->GetBufferMemoryRequirements2KHR = (PFN_vkGetBufferMemoryRequirements2KHR)StubGetBufferMemoryRequirements2KHR; }
    table->GetImageSparseMemoryRequirements2KHR = (PFN_vkGetImageSparseMemoryRequirements2KHR) gpa(device, "vkGetImageSparseMemoryRequirements2KHR");
    // if (table->GetImageSparseMemoryRequirements2KHR == nullptr) { table->GetImageSparseMemoryRequirements2KHR = (PFN_vkGetImageSparseMemoryRequirements2KHR)StubGetImageSparseMemoryRequirements2KHR; }
    table->CreateSamplerYcbcrConversionKHR = (PFN_vkCreateSamplerYcbcrConversionKHR) gpa(device, "vkCreateSamplerYcbcrConversionKHR");
    // if (table->CreateSamplerYcbcrConversionKHR == nullptr) { table->CreateSamplerYcbcrConversionKHR = (PFN_vkCreateSamplerYcbcrConversionKHR)StubCreateSamplerYcbcrConversionKHR; }
    table->DestroySamplerYcbcrConversionKHR = (PFN_vkDestroySamplerYcbcrConversionKHR) gpa(device, "vkDestroySamplerYcbcrConversionKHR");
    // if (table->DestroySamplerYcbcrConversionKHR == nullptr) { table->DestroySamplerYcbcrConversionKHR = (PFN_vkDestroySamplerYcbcrConversionKHR)StubDestroySamplerYcbcrConversionKHR; }
    table->BindBufferMemory2KHR = (PFN_vkBindBufferMemory2KHR) gpa(device, "vkBindBufferMemory2KHR");
    // if (table->BindBufferMemory2KHR == nullptr) { table->BindBufferMemory2KHR = (PFN_vkBindBufferMemory2KHR)StubBindBufferMemory2KHR; }
    table->BindImageMemory2KHR = (PFN_vkBindImageMemory2KHR) gpa(device, "vkBindImageMemory2KHR");
    // if (table->BindImageMemory2KHR == nullptr) { table->BindImageMemory2KHR = (PFN_vkBindImageMemory2KHR)StubBindImageMemory2KHR; }
    table->GetDescriptorSetLayoutSupportKHR = (PFN_vkGetDescriptorSetLayoutSupportKHR) gpa(device, "vkGetDescriptorSetLayoutSupportKHR");
    // if (table->GetDescriptorSetLayoutSupportKHR == nullptr) { table->GetDescriptorSetLayoutSupportKHR = (PFN_vkGetDescriptorSetLayoutSupportKHR)StubGetDescriptorSetLayoutSupportKHR; }
    table->CmdDrawIndirectCountKHR = (PFN_vkCmdDrawIndirectCountKHR) gpa(device, "vkCmdDrawIndirectCountKHR");
    // if (table->CmdDrawIndirectCountKHR == nullptr) { table->CmdDrawIndirectCountKHR = (PFN_vkCmdDrawIndirectCountKHR)StubCmdDrawIndirectCountKHR; }
    table->CmdDrawIndexedIndirectCountKHR = (PFN_vkCmdDrawIndexedIndirectCountKHR) gpa(device, "vkCmdDrawIndexedIndirectCountKHR");
    // if (table->CmdDrawIndexedIndirectCountKHR == nullptr) { table->CmdDrawIndexedIndirectCountKHR = (PFN_vkCmdDrawIndexedIndirectCountKHR)StubCmdDrawIndexedIndirectCountKHR; }
    table->GetSemaphoreCounterValueKHR = (PFN_vkGetSemaphoreCounterValueKHR) gpa(device, "vkGetSemaphoreCounterValueKHR");
    // if (table->GetSemaphoreCounterValueKHR == nullptr) { table->GetSemaphoreCounterValueKHR = (PFN_vkGetSemaphoreCounterValueKHR)StubGetSemaphoreCounterValueKHR; }
    table->WaitSemaphoresKHR = (PFN_vkWaitSemaphoresKHR) gpa(device, "vkWaitSemaphoresKHR");
    // if (table->WaitSemaphoresKHR == nullptr) { table->WaitSemaphoresKHR = (PFN_vkWaitSemaphoresKHR)StubWaitSemaphoresKHR; }
    table->SignalSemaphoreKHR = (PFN_vkSignalSemaphoreKHR) gpa(device, "vkSignalSemaphoreKHR");
    // if (table->SignalSemaphoreKHR == nullptr) { table->SignalSemaphoreKHR = (PFN_vkSignalSemaphoreKHR)StubSignalSemaphoreKHR; }
    table->CmdSetFragmentShadingRateKHR = (PFN_vkCmdSetFragmentShadingRateKHR) gpa(device, "vkCmdSetFragmentShadingRateKHR");
    // if (table->CmdSetFragmentShadingRateKHR == nullptr) { table->CmdSetFragmentShadingRateKHR = (PFN_vkCmdSetFragmentShadingRateKHR)StubCmdSetFragmentShadingRateKHR; }
    table->GetBufferDeviceAddressKHR = (PFN_vkGetBufferDeviceAddressKHR) gpa(device, "vkGetBufferDeviceAddressKHR");
    // if (table->GetBufferDeviceAddressKHR == nullptr) { table->GetBufferDeviceAddressKHR = (PFN_vkGetBufferDeviceAddressKHR)StubGetBufferDeviceAddressKHR; }
    table->GetBufferOpaqueCaptureAddressKHR = (PFN_vkGetBufferOpaqueCaptureAddressKHR) gpa(device, "vkGetBufferOpaqueCaptureAddressKHR");
    // if (table->GetBufferOpaqueCaptureAddressKHR == nullptr) { table->GetBufferOpaqueCaptureAddressKHR = (PFN_vkGetBufferOpaqueCaptureAddressKHR)StubGetBufferOpaqueCaptureAddressKHR; }
    table->GetDeviceMemoryOpaqueCaptureAddressKHR = (PFN_vkGetDeviceMemoryOpaqueCaptureAddressKHR) gpa(device, "vkGetDeviceMemoryOpaqueCaptureAddressKHR");
    // if (table->GetDeviceMemoryOpaqueCaptureAddressKHR == nullptr) { table->GetDeviceMemoryOpaqueCaptureAddressKHR = (PFN_vkGetDeviceMemoryOpaqueCaptureAddressKHR)StubGetDeviceMemoryOpaqueCaptureAddressKHR; }
    table->CreateDeferredOperationKHR = (PFN_vkCreateDeferredOperationKHR) gpa(device, "vkCreateDeferredOperationKHR");
    // if (table->CreateDeferredOperationKHR == nullptr) { table->CreateDeferredOperationKHR = (PFN_vkCreateDeferredOperationKHR)StubCreateDeferredOperationKHR; }
    table->DestroyDeferredOperationKHR = (PFN_vkDestroyDeferredOperationKHR) gpa(device, "vkDestroyDeferredOperationKHR");
    // if (table->DestroyDeferredOperationKHR == nullptr) { table->DestroyDeferredOperationKHR = (PFN_vkDestroyDeferredOperationKHR)StubDestroyDeferredOperationKHR; }
    table->GetDeferredOperationMaxConcurrencyKHR = (PFN_vkGetDeferredOperationMaxConcurrencyKHR) gpa(device, "vkGetDeferredOperationMaxConcurrencyKHR");
    // if (table->GetDeferredOperationMaxConcurrencyKHR == nullptr) { table->GetDeferredOperationMaxConcurrencyKHR = (PFN_vkGetDeferredOperationMaxConcurrencyKHR)StubGetDeferredOperationMaxConcurrencyKHR; }
    table->GetDeferredOperationResultKHR = (PFN_vkGetDeferredOperationResultKHR) gpa(device, "vkGetDeferredOperationResultKHR");
    // if (table->GetDeferredOperationResultKHR == nullptr) { table->GetDeferredOperationResultKHR = (PFN_vkGetDeferredOperationResultKHR)StubGetDeferredOperationResultKHR; }
    table->DeferredOperationJoinKHR = (PFN_vkDeferredOperationJoinKHR) gpa(device, "vkDeferredOperationJoinKHR");
    // if (table->DeferredOperationJoinKHR == nullptr) { table->DeferredOperationJoinKHR = (PFN_vkDeferredOperationJoinKHR)StubDeferredOperationJoinKHR; }
    table->GetPipelineExecutablePropertiesKHR = (PFN_vkGetPipelineExecutablePropertiesKHR) gpa(device, "vkGetPipelineExecutablePropertiesKHR");
    // if (table->GetPipelineExecutablePropertiesKHR == nullptr) { table->GetPipelineExecutablePropertiesKHR = (PFN_vkGetPipelineExecutablePropertiesKHR)StubGetPipelineExecutablePropertiesKHR; }
    table->GetPipelineExecutableStatisticsKHR = (PFN_vkGetPipelineExecutableStatisticsKHR) gpa(device, "vkGetPipelineExecutableStatisticsKHR");
    // if (table->GetPipelineExecutableStatisticsKHR == nullptr) { table->GetPipelineExecutableStatisticsKHR = (PFN_vkGetPipelineExecutableStatisticsKHR)StubGetPipelineExecutableStatisticsKHR; }
    table->GetPipelineExecutableInternalRepresentationsKHR = (PFN_vkGetPipelineExecutableInternalRepresentationsKHR) gpa(device, "vkGetPipelineExecutableInternalRepresentationsKHR");
    // if (table->GetPipelineExecutableInternalRepresentationsKHR == nullptr) { table->GetPipelineExecutableInternalRepresentationsKHR = (PFN_vkGetPipelineExecutableInternalRepresentationsKHR)StubGetPipelineExecutableInternalRepresentationsKHR; }
    table->CmdCopyBuffer2KHR = (PFN_vkCmdCopyBuffer2KHR) gpa(device, "vkCmdCopyBuffer2KHR");
    // if (table->CmdCopyBuffer2KHR == nullptr) { table->CmdCopyBuffer2KHR = (PFN_vkCmdCopyBuffer2KHR)StubCmdCopyBuffer2KHR; }
    table->CmdCopyImage2KHR = (PFN_vkCmdCopyImage2KHR) gpa(device, "vkCmdCopyImage2KHR");
    // if (table->CmdCopyImage2KHR == nullptr) { table->CmdCopyImage2KHR = (PFN_vkCmdCopyImage2KHR)StubCmdCopyImage2KHR; }
    table->CmdCopyBufferToImage2KHR = (PFN_vkCmdCopyBufferToImage2KHR) gpa(device, "vkCmdCopyBufferToImage2KHR");
    // if (table->CmdCopyBufferToImage2KHR == nullptr) { table->CmdCopyBufferToImage2KHR = (PFN_vkCmdCopyBufferToImage2KHR)StubCmdCopyBufferToImage2KHR; }
    table->CmdCopyImageToBuffer2KHR = (PFN_vkCmdCopyImageToBuffer2KHR) gpa(device, "vkCmdCopyImageToBuffer2KHR");
    // if (table->CmdCopyImageToBuffer2KHR == nullptr) { table->CmdCopyImageToBuffer2KHR = (PFN_vkCmdCopyImageToBuffer2KHR)StubCmdCopyImageToBuffer2KHR; }
    table->CmdBlitImage2KHR = (PFN_vkCmdBlitImage2KHR) gpa(device, "vkCmdBlitImage2KHR");
    // if (table->CmdBlitImage2KHR == nullptr) { table->CmdBlitImage2KHR = (PFN_vkCmdBlitImage2KHR)StubCmdBlitImage2KHR; }
    table->CmdResolveImage2KHR = (PFN_vkCmdResolveImage2KHR) gpa(device, "vkCmdResolveImage2KHR");
    // if (table->CmdResolveImage2KHR == nullptr) { table->CmdResolveImage2KHR = (PFN_vkCmdResolveImage2KHR)StubCmdResolveImage2KHR; }
    table->DebugMarkerSetObjectTagEXT = (PFN_vkDebugMarkerSetObjectTagEXT) gpa(device, "vkDebugMarkerSetObjectTagEXT");
    // if (table->DebugMarkerSetObjectTagEXT == nullptr) { table->DebugMarkerSetObjectTagEXT = (PFN_vkDebugMarkerSetObjectTagEXT)StubDebugMarkerSetObjectTagEXT; }
    table->DebugMarkerSetObjectNameEXT = (PFN_vkDebugMarkerSetObjectNameEXT) gpa(device, "vkDebugMarkerSetObjectNameEXT");
    // if (table->DebugMarkerSetObjectNameEXT == nullptr) { table->DebugMarkerSetObjectNameEXT = (PFN_vkDebugMarkerSetObjectNameEXT)StubDebugMarkerSetObjectNameEXT; }
    table->CmdDebugMarkerBeginEXT = (PFN_vkCmdDebugMarkerBeginEXT) gpa(device, "vkCmdDebugMarkerBeginEXT");
    // if (table->CmdDebugMarkerBeginEXT == nullptr) { table->CmdDebugMarkerBeginEXT = (PFN_vkCmdDebugMarkerBeginEXT)StubCmdDebugMarkerBeginEXT; }
    table->CmdDebugMarkerEndEXT = (PFN_vkCmdDebugMarkerEndEXT) gpa(device, "vkCmdDebugMarkerEndEXT");
    // if (table->CmdDebugMarkerEndEXT == nullptr) { table->CmdDebugMarkerEndEXT = (PFN_vkCmdDebugMarkerEndEXT)StubCmdDebugMarkerEndEXT; }
    table->CmdDebugMarkerInsertEXT = (PFN_vkCmdDebugMarkerInsertEXT) gpa(device, "vkCmdDebugMarkerInsertEXT");
    // if (table->CmdDebugMarkerInsertEXT == nullptr) { table->CmdDebugMarkerInsertEXT = (PFN_vkCmdDebugMarkerInsertEXT)StubCmdDebugMarkerInsertEXT; }
    table->CmdBindTransformFeedbackBuffersEXT = (PFN_vkCmdBindTransformFeedbackBuffersEXT) gpa(device, "vkCmdBindTransformFeedbackBuffersEXT");
    // if (table->CmdBindTransformFeedbackBuffersEXT == nullptr) { table->CmdBindTransformFeedbackBuffersEXT = (PFN_vkCmdBindTransformFeedbackBuffersEXT)StubCmdBindTransformFeedbackBuffersEXT; }
    table->CmdBeginTransformFeedbackEXT = (PFN_vkCmdBeginTransformFeedbackEXT) gpa(device, "vkCmdBeginTransformFeedbackEXT");
    // if (table->CmdBeginTransformFeedbackEXT == nullptr) { table->CmdBeginTransformFeedbackEXT = (PFN_vkCmdBeginTransformFeedbackEXT)StubCmdBeginTransformFeedbackEXT; }
    table->CmdEndTransformFeedbackEXT = (PFN_vkCmdEndTransformFeedbackEXT) gpa(device, "vkCmdEndTransformFeedbackEXT");
    // if (table->CmdEndTransformFeedbackEXT == nullptr) { table->CmdEndTransformFeedbackEXT = (PFN_vkCmdEndTransformFeedbackEXT)StubCmdEndTransformFeedbackEXT; }
    table->CmdBeginQueryIndexedEXT = (PFN_vkCmdBeginQueryIndexedEXT) gpa(device, "vkCmdBeginQueryIndexedEXT");
    // if (table->CmdBeginQueryIndexedEXT == nullptr) { table->CmdBeginQueryIndexedEXT = (PFN_vkCmdBeginQueryIndexedEXT)StubCmdBeginQueryIndexedEXT; }
    table->CmdEndQueryIndexedEXT = (PFN_vkCmdEndQueryIndexedEXT) gpa(device, "vkCmdEndQueryIndexedEXT");
    // if (table->CmdEndQueryIndexedEXT == nullptr) { table->CmdEndQueryIndexedEXT = (PFN_vkCmdEndQueryIndexedEXT)StubCmdEndQueryIndexedEXT; }
    table->CmdDrawIndirectByteCountEXT = (PFN_vkCmdDrawIndirectByteCountEXT) gpa(device, "vkCmdDrawIndirectByteCountEXT");
    // if (table->CmdDrawIndirectByteCountEXT == nullptr) { table->CmdDrawIndirectByteCountEXT = (PFN_vkCmdDrawIndirectByteCountEXT)StubCmdDrawIndirectByteCountEXT; }
    table->GetImageViewHandleNVX = (PFN_vkGetImageViewHandleNVX) gpa(device, "vkGetImageViewHandleNVX");
    // if (table->GetImageViewHandleNVX == nullptr) { table->GetImageViewHandleNVX = (PFN_vkGetImageViewHandleNVX)StubGetImageViewHandleNVX; }
    table->GetImageViewAddressNVX = (PFN_vkGetImageViewAddressNVX) gpa(device, "vkGetImageViewAddressNVX");
    // if (table->GetImageViewAddressNVX == nullptr) { table->GetImageViewAddressNVX = (PFN_vkGetImageViewAddressNVX)StubGetImageViewAddressNVX; }
    table->CmdDrawIndirectCountAMD = (PFN_vkCmdDrawIndirectCountAMD) gpa(device, "vkCmdDrawIndirectCountAMD");
    // if (table->CmdDrawIndirectCountAMD == nullptr) { table->CmdDrawIndirectCountAMD = (PFN_vkCmdDrawIndirectCountAMD)StubCmdDrawIndirectCountAMD; }
    table->CmdDrawIndexedIndirectCountAMD = (PFN_vkCmdDrawIndexedIndirectCountAMD) gpa(device, "vkCmdDrawIndexedIndirectCountAMD");
    // if (table->CmdDrawIndexedIndirectCountAMD == nullptr) { table->CmdDrawIndexedIndirectCountAMD = (PFN_vkCmdDrawIndexedIndirectCountAMD)StubCmdDrawIndexedIndirectCountAMD; }
    table->GetShaderInfoAMD = (PFN_vkGetShaderInfoAMD) gpa(device, "vkGetShaderInfoAMD");
    // if (table->GetShaderInfoAMD == nullptr) { table->GetShaderInfoAMD = (PFN_vkGetShaderInfoAMD)StubGetShaderInfoAMD; }
#ifdef VK_USE_PLATFORM_WIN32_KHR
    table->GetMemoryWin32HandleNV = (PFN_vkGetMemoryWin32HandleNV) gpa(device, "vkGetMemoryWin32HandleNV");
    // if (table->GetMemoryWin32HandleNV == nullptr) { table->GetMemoryWin32HandleNV = (PFN_vkGetMemoryWin32HandleNV)StubGetMemoryWin32HandleNV; }
#endif // VK_USE_PLATFORM_WIN32_KHR
    table->CmdBeginConditionalRenderingEXT = (PFN_vkCmdBeginConditionalRenderingEXT) gpa(device, "vkCmdBeginConditionalRenderingEXT");
    // if (table->CmdBeginConditionalRenderingEXT == nullptr) { table->CmdBeginConditionalRenderingEXT = (PFN_vkCmdBeginConditionalRenderingEXT)StubCmdBeginConditionalRenderingEXT; }
    table->CmdEndConditionalRenderingEXT = (PFN_vkCmdEndConditionalRenderingEXT) gpa(device, "vkCmdEndConditionalRenderingEXT");
    // if (table->CmdEndConditionalRenderingEXT == nullptr) { table->CmdEndConditionalRenderingEXT = (PFN_vkCmdEndConditionalRenderingEXT)StubCmdEndConditionalRenderingEXT; }
    table->CmdSetViewportWScalingNV = (PFN_vkCmdSetViewportWScalingNV) gpa(device, "vkCmdSetViewportWScalingNV");
    // if (table->CmdSetViewportWScalingNV == nullptr) { table->CmdSetViewportWScalingNV = (PFN_vkCmdSetViewportWScalingNV)StubCmdSetViewportWScalingNV; }
    table->DisplayPowerControlEXT = (PFN_vkDisplayPowerControlEXT) gpa(device, "vkDisplayPowerControlEXT");
    // if (table->DisplayPowerControlEXT == nullptr) { table->DisplayPowerControlEXT = (PFN_vkDisplayPowerControlEXT)StubDisplayPowerControlEXT; }
    table->RegisterDeviceEventEXT = (PFN_vkRegisterDeviceEventEXT) gpa(device, "vkRegisterDeviceEventEXT");
    // if (table->RegisterDeviceEventEXT == nullptr) { table->RegisterDeviceEventEXT = (PFN_vkRegisterDeviceEventEXT)StubRegisterDeviceEventEXT; }
    table->RegisterDisplayEventEXT = (PFN_vkRegisterDisplayEventEXT) gpa(device, "vkRegisterDisplayEventEXT");
    // if (table->RegisterDisplayEventEXT == nullptr) { table->RegisterDisplayEventEXT = (PFN_vkRegisterDisplayEventEXT)StubRegisterDisplayEventEXT; }
    table->GetSwapchainCounterEXT = (PFN_vkGetSwapchainCounterEXT) gpa(device, "vkGetSwapchainCounterEXT");
    // if (table->GetSwapchainCounterEXT == nullptr) { table->GetSwapchainCounterEXT = (PFN_vkGetSwapchainCounterEXT)StubGetSwapchainCounterEXT; }
    table->GetRefreshCycleDurationGOOGLE = (PFN_vkGetRefreshCycleDurationGOOGLE) gpa(device, "vkGetRefreshCycleDurationGOOGLE");
    // if (table->GetRefreshCycleDurationGOOGLE == nullptr) { table->GetRefreshCycleDurationGOOGLE = (PFN_vkGetRefreshCycleDurationGOOGLE)StubGetRefreshCycleDurationGOOGLE; }
    table->GetPastPresentationTimingGOOGLE = (PFN_vkGetPastPresentationTimingGOOGLE) gpa(device, "vkGetPastPresentationTimingGOOGLE");
    // if (table->GetPastPresentationTimingGOOGLE == nullptr) { table->GetPastPresentationTimingGOOGLE = (PFN_vkGetPastPresentationTimingGOOGLE)StubGetPastPresentationTimingGOOGLE; }
    table->CmdSetDiscardRectangleEXT = (PFN_vkCmdSetDiscardRectangleEXT) gpa(device, "vkCmdSetDiscardRectangleEXT");
    // if (table->CmdSetDiscardRectangleEXT == nullptr) { table->CmdSetDiscardRectangleEXT = (PFN_vkCmdSetDiscardRectangleEXT)StubCmdSetDiscardRectangleEXT; }
    table->SetHdrMetadataEXT = (PFN_vkSetHdrMetadataEXT) gpa(device, "vkSetHdrMetadataEXT");
    // if (table->SetHdrMetadataEXT == nullptr) { table->SetHdrMetadataEXT = (PFN_vkSetHdrMetadataEXT)StubSetHdrMetadataEXT; }
    table->SetDebugUtilsObjectNameEXT = (PFN_vkSetDebugUtilsObjectNameEXT) gpa(device, "vkSetDebugUtilsObjectNameEXT");
    // if (table->SetDebugUtilsObjectNameEXT == nullptr) { table->SetDebugUtilsObjectNameEXT = (PFN_vkSetDebugUtilsObjectNameEXT)StubSetDebugUtilsObjectNameEXT; }
    table->SetDebugUtilsObjectTagEXT = (PFN_vkSetDebugUtilsObjectTagEXT) gpa(device, "vkSetDebugUtilsObjectTagEXT");
    // if (table->SetDebugUtilsObjectTagEXT == nullptr) { table->SetDebugUtilsObjectTagEXT = (PFN_vkSetDebugUtilsObjectTagEXT)StubSetDebugUtilsObjectTagEXT; }
    table->QueueBeginDebugUtilsLabelEXT = (PFN_vkQueueBeginDebugUtilsLabelEXT) gpa(device, "vkQueueBeginDebugUtilsLabelEXT");
    // if (table->QueueBeginDebugUtilsLabelEXT == nullptr) { table->QueueBeginDebugUtilsLabelEXT = (PFN_vkQueueBeginDebugUtilsLabelEXT)StubQueueBeginDebugUtilsLabelEXT; }
    table->QueueEndDebugUtilsLabelEXT = (PFN_vkQueueEndDebugUtilsLabelEXT) gpa(device, "vkQueueEndDebugUtilsLabelEXT");
    // if (table->QueueEndDebugUtilsLabelEXT == nullptr) { table->QueueEndDebugUtilsLabelEXT = (PFN_vkQueueEndDebugUtilsLabelEXT)StubQueueEndDebugUtilsLabelEXT; }
    table->QueueInsertDebugUtilsLabelEXT = (PFN_vkQueueInsertDebugUtilsLabelEXT) gpa(device, "vkQueueInsertDebugUtilsLabelEXT");
    // if (table->QueueInsertDebugUtilsLabelEXT == nullptr) { table->QueueInsertDebugUtilsLabelEXT = (PFN_vkQueueInsertDebugUtilsLabelEXT)StubQueueInsertDebugUtilsLabelEXT; }
    table->CmdBeginDebugUtilsLabelEXT = (PFN_vkCmdBeginDebugUtilsLabelEXT) gpa(device, "vkCmdBeginDebugUtilsLabelEXT");
    // if (table->CmdBeginDebugUtilsLabelEXT == nullptr) { table->CmdBeginDebugUtilsLabelEXT = (PFN_vkCmdBeginDebugUtilsLabelEXT)StubCmdBeginDebugUtilsLabelEXT; }
    table->CmdEndDebugUtilsLabelEXT = (PFN_vkCmdEndDebugUtilsLabelEXT) gpa(device, "vkCmdEndDebugUtilsLabelEXT");
    // if (table->CmdEndDebugUtilsLabelEXT == nullptr) { table->CmdEndDebugUtilsLabelEXT = (PFN_vkCmdEndDebugUtilsLabelEXT)StubCmdEndDebugUtilsLabelEXT; }
    table->CmdInsertDebugUtilsLabelEXT = (PFN_vkCmdInsertDebugUtilsLabelEXT) gpa(device, "vkCmdInsertDebugUtilsLabelEXT");
    // if (table->CmdInsertDebugUtilsLabelEXT == nullptr) { table->CmdInsertDebugUtilsLabelEXT = (PFN_vkCmdInsertDebugUtilsLabelEXT)StubCmdInsertDebugUtilsLabelEXT; }
#ifdef VK_USE_PLATFORM_ANDROID_KHR
    table->GetAndroidHardwareBufferPropertiesANDROID = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID) gpa(device, "vkGetAndroidHardwareBufferPropertiesANDROID");
    // if (table->GetAndroidHardwareBufferPropertiesANDROID == nullptr) { table->GetAndroidHardwareBufferPropertiesANDROID = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)StubGetAndroidHardwareBufferPropertiesANDROID; }
#endif // VK_USE_PLATFORM_ANDROID_KHR
#ifdef VK_USE_PLATFORM_ANDROID_KHR
    table->GetMemoryAndroidHardwareBufferANDROID = (PFN_vkGetMemoryAndroidHardwareBufferANDROID) gpa(device, "vkGetMemoryAndroidHardwareBufferANDROID");
    // if (table->GetMemoryAndroidHardwareBufferANDROID == nullptr) { table->GetMemoryAndroidHardwareBufferANDROID = (PFN_vkGetMemoryAndroidHardwareBufferANDROID)StubGetMemoryAndroidHardwareBufferANDROID; }
#endif // VK_USE_PLATFORM_ANDROID_KHR
    table->CmdSetSampleLocationsEXT = (PFN_vkCmdSetSampleLocationsEXT) gpa(device, "vkCmdSetSampleLocationsEXT");
    // if (table->CmdSetSampleLocationsEXT == nullptr) { table->CmdSetSampleLocationsEXT = (PFN_vkCmdSetSampleLocationsEXT)StubCmdSetSampleLocationsEXT; }
    table->GetImageDrmFormatModifierPropertiesEXT = (PFN_vkGetImageDrmFormatModifierPropertiesEXT) gpa(device, "vkGetImageDrmFormatModifierPropertiesEXT");
    // if (table->GetImageDrmFormatModifierPropertiesEXT == nullptr) { table->GetImageDrmFormatModifierPropertiesEXT = (PFN_vkGetImageDrmFormatModifierPropertiesEXT)StubGetImageDrmFormatModifierPropertiesEXT; }
    table->CreateValidationCacheEXT = (PFN_vkCreateValidationCacheEXT) gpa(device, "vkCreateValidationCacheEXT");
    // if (table->CreateValidationCacheEXT == nullptr) { table->CreateValidationCacheEXT = (PFN_vkCreateValidationCacheEXT)StubCreateValidationCacheEXT; }
    table->DestroyValidationCacheEXT = (PFN_vkDestroyValidationCacheEXT) gpa(device, "vkDestroyValidationCacheEXT");
    // if (table->DestroyValidationCacheEXT == nullptr) { table->DestroyValidationCacheEXT = (PFN_vkDestroyValidationCacheEXT)StubDestroyValidationCacheEXT; }
    table->MergeValidationCachesEXT = (PFN_vkMergeValidationCachesEXT) gpa(device, "vkMergeValidationCachesEXT");
    // if (table->MergeValidationCachesEXT == nullptr) { table->MergeValidationCachesEXT = (PFN_vkMergeValidationCachesEXT)StubMergeValidationCachesEXT; }
    table->GetValidationCacheDataEXT = (PFN_vkGetValidationCacheDataEXT) gpa(device, "vkGetValidationCacheDataEXT");
    // if (table->GetValidationCacheDataEXT == nullptr) { table->GetValidationCacheDataEXT = (PFN_vkGetValidationCacheDataEXT)StubGetValidationCacheDataEXT; }
    table->CmdBindShadingRateImageNV = (PFN_vkCmdBindShadingRateImageNV) gpa(device, "vkCmdBindShadingRateImageNV");
    // if (table->CmdBindShadingRateImageNV == nullptr) { table->CmdBindShadingRateImageNV = (PFN_vkCmdBindShadingRateImageNV)StubCmdBindShadingRateImageNV; }
    table->CmdSetViewportShadingRatePaletteNV = (PFN_vkCmdSetViewportShadingRatePaletteNV) gpa(device, "vkCmdSetViewportShadingRatePaletteNV");
    // if (table->CmdSetViewportShadingRatePaletteNV == nullptr) { table->CmdSetViewportShadingRatePaletteNV = (PFN_vkCmdSetViewportShadingRatePaletteNV)StubCmdSetViewportShadingRatePaletteNV; }
    table->CmdSetCoarseSampleOrderNV = (PFN_vkCmdSetCoarseSampleOrderNV) gpa(device, "vkCmdSetCoarseSampleOrderNV");
    // if (table->CmdSetCoarseSampleOrderNV == nullptr) { table->CmdSetCoarseSampleOrderNV = (PFN_vkCmdSetCoarseSampleOrderNV)StubCmdSetCoarseSampleOrderNV; }
    table->CreateAccelerationStructureNV = (PFN_vkCreateAccelerationStructureNV) gpa(device, "vkCreateAccelerationStructureNV");
    // if (table->CreateAccelerationStructureNV == nullptr) { table->CreateAccelerationStructureNV = (PFN_vkCreateAccelerationStructureNV)StubCreateAccelerationStructureNV; }
    table->DestroyAccelerationStructureNV = (PFN_vkDestroyAccelerationStructureNV) gpa(device, "vkDestroyAccelerationStructureNV");
    // if (table->DestroyAccelerationStructureNV == nullptr) { table->DestroyAccelerationStructureNV = (PFN_vkDestroyAccelerationStructureNV)StubDestroyAccelerationStructureNV; }
    table->GetAccelerationStructureMemoryRequirementsNV = (PFN_vkGetAccelerationStructureMemoryRequirementsNV) gpa(device, "vkGetAccelerationStructureMemoryRequirementsNV");
    // if (table->GetAccelerationStructureMemoryRequirementsNV == nullptr) { table->GetAccelerationStructureMemoryRequirementsNV = (PFN_vkGetAccelerationStructureMemoryRequirementsNV)StubGetAccelerationStructureMemoryRequirementsNV; }
    table->BindAccelerationStructureMemoryNV = (PFN_vkBindAccelerationStructureMemoryNV) gpa(device, "vkBindAccelerationStructureMemoryNV");
    // if (table->BindAccelerationStructureMemoryNV == nullptr) { table->BindAccelerationStructureMemoryNV = (PFN_vkBindAccelerationStructureMemoryNV)StubBindAccelerationStructureMemoryNV; }
    table->CmdBuildAccelerationStructureNV = (PFN_vkCmdBuildAccelerationStructureNV) gpa(device, "vkCmdBuildAccelerationStructureNV");
    // if (table->CmdBuildAccelerationStructureNV == nullptr) { table->CmdBuildAccelerationStructureNV = (PFN_vkCmdBuildAccelerationStructureNV)StubCmdBuildAccelerationStructureNV; }
    table->CmdCopyAccelerationStructureNV = (PFN_vkCmdCopyAccelerationStructureNV) gpa(device, "vkCmdCopyAccelerationStructureNV");
    // if (table->CmdCopyAccelerationStructureNV == nullptr) { table->CmdCopyAccelerationStructureNV = (PFN_vkCmdCopyAccelerationStructureNV)StubCmdCopyAccelerationStructureNV; }
    table->CmdTraceRaysNV = (PFN_vkCmdTraceRaysNV) gpa(device, "vkCmdTraceRaysNV");
    // if (table->CmdTraceRaysNV == nullptr) { table->CmdTraceRaysNV = (PFN_vkCmdTraceRaysNV)StubCmdTraceRaysNV; }
    table->CreateRayTracingPipelinesNV = (PFN_vkCreateRayTracingPipelinesNV) gpa(device, "vkCreateRayTracingPipelinesNV");
    // if (table->CreateRayTracingPipelinesNV == nullptr) { table->CreateRayTracingPipelinesNV = (PFN_vkCreateRayTracingPipelinesNV)StubCreateRayTracingPipelinesNV; }
    table->GetRayTracingShaderGroupHandlesKHR = (PFN_vkGetRayTracingShaderGroupHandlesKHR) gpa(device, "vkGetRayTracingShaderGroupHandlesKHR");
    // if (table->GetRayTracingShaderGroupHandlesKHR == nullptr) { table->GetRayTracingShaderGroupHandlesKHR = (PFN_vkGetRayTracingShaderGroupHandlesKHR)StubGetRayTracingShaderGroupHandlesKHR; }
    table->GetRayTracingShaderGroupHandlesNV = (PFN_vkGetRayTracingShaderGroupHandlesNV) gpa(device, "vkGetRayTracingShaderGroupHandlesNV");
    // if (table->GetRayTracingShaderGroupHandlesNV == nullptr) { table->GetRayTracingShaderGroupHandlesNV = (PFN_vkGetRayTracingShaderGroupHandlesNV)StubGetRayTracingShaderGroupHandlesNV; }
    table->GetAccelerationStructureHandleNV = (PFN_vkGetAccelerationStructureHandleNV) gpa(device, "vkGetAccelerationStructureHandleNV");
    // if (table->GetAccelerationStructureHandleNV == nullptr) { table->GetAccelerationStructureHandleNV = (PFN_vkGetAccelerationStructureHandleNV)StubGetAccelerationStructureHandleNV; }
    table->CmdWriteAccelerationStructuresPropertiesNV = (PFN_vkCmdWriteAccelerationStructuresPropertiesNV) gpa(device, "vkCmdWriteAccelerationStructuresPropertiesNV");
    // if (table->CmdWriteAccelerationStructuresPropertiesNV == nullptr) { table->CmdWriteAccelerationStructuresPropertiesNV = (PFN_vkCmdWriteAccelerationStructuresPropertiesNV)StubCmdWriteAccelerationStructuresPropertiesNV; }
    table->CompileDeferredNV = (PFN_vkCompileDeferredNV) gpa(device, "vkCompileDeferredNV");
    // if (table->CompileDeferredNV == nullptr) { table->CompileDeferredNV = (PFN_vkCompileDeferredNV)StubCompileDeferredNV; }
    table->GetMemoryHostPointerPropertiesEXT = (PFN_vkGetMemoryHostPointerPropertiesEXT) gpa(device, "vkGetMemoryHostPointerPropertiesEXT");
    // if (table->GetMemoryHostPointerPropertiesEXT == nullptr) { table->GetMemoryHostPointerPropertiesEXT = (PFN_vkGetMemoryHostPointerPropertiesEXT)StubGetMemoryHostPointerPropertiesEXT; }
    table->CmdWriteBufferMarkerAMD = (PFN_vkCmdWriteBufferMarkerAMD) gpa(device, "vkCmdWriteBufferMarkerAMD");
    // if (table->CmdWriteBufferMarkerAMD == nullptr) { table->CmdWriteBufferMarkerAMD = (PFN_vkCmdWriteBufferMarkerAMD)StubCmdWriteBufferMarkerAMD; }
    table->GetCalibratedTimestampsEXT = (PFN_vkGetCalibratedTimestampsEXT) gpa(device, "vkGetCalibratedTimestampsEXT");
    // if (table->GetCalibratedTimestampsEXT == nullptr) { table->GetCalibratedTimestampsEXT = (PFN_vkGetCalibratedTimestampsEXT)StubGetCalibratedTimestampsEXT; }
    table->CmdDrawMeshTasksNV = (PFN_vkCmdDrawMeshTasksNV) gpa(device, "vkCmdDrawMeshTasksNV");
    // if (table->CmdDrawMeshTasksNV == nullptr) { table->CmdDrawMeshTasksNV = (PFN_vkCmdDrawMeshTasksNV)StubCmdDrawMeshTasksNV; }
    table->CmdDrawMeshTasksIndirectNV = (PFN_vkCmdDrawMeshTasksIndirectNV) gpa(device, "vkCmdDrawMeshTasksIndirectNV");
    // if (table->CmdDrawMeshTasksIndirectNV == nullptr) { table->CmdDrawMeshTasksIndirectNV = (PFN_vkCmdDrawMeshTasksIndirectNV)StubCmdDrawMeshTasksIndirectNV; }
    table->CmdDrawMeshTasksIndirectCountNV = (PFN_vkCmdDrawMeshTasksIndirectCountNV) gpa(device, "vkCmdDrawMeshTasksIndirectCountNV");
    // if (table->CmdDrawMeshTasksIndirectCountNV == nullptr) { table->CmdDrawMeshTasksIndirectCountNV = (PFN_vkCmdDrawMeshTasksIndirectCountNV)StubCmdDrawMeshTasksIndirectCountNV; }
    table->CmdSetExclusiveScissorNV = (PFN_vkCmdSetExclusiveScissorNV) gpa(device, "vkCmdSetExclusiveScissorNV");
    // if (table->CmdSetExclusiveScissorNV == nullptr) { table->CmdSetExclusiveScissorNV = (PFN_vkCmdSetExclusiveScissorNV)StubCmdSetExclusiveScissorNV; }
    table->CmdSetCheckpointNV = (PFN_vkCmdSetCheckpointNV) gpa(device, "vkCmdSetCheckpointNV");
    // if (table->CmdSetCheckpointNV == nullptr) { table->CmdSetCheckpointNV = (PFN_vkCmdSetCheckpointNV)StubCmdSetCheckpointNV; }
    table->GetQueueCheckpointDataNV = (PFN_vkGetQueueCheckpointDataNV) gpa(device, "vkGetQueueCheckpointDataNV");
    // if (table->GetQueueCheckpointDataNV == nullptr) { table->GetQueueCheckpointDataNV = (PFN_vkGetQueueCheckpointDataNV)StubGetQueueCheckpointDataNV; }
    table->InitializePerformanceApiINTEL = (PFN_vkInitializePerformanceApiINTEL) gpa(device, "vkInitializePerformanceApiINTEL");
    // if (table->InitializePerformanceApiINTEL == nullptr) { table->InitializePerformanceApiINTEL = (PFN_vkInitializePerformanceApiINTEL)StubInitializePerformanceApiINTEL; }
    table->UninitializePerformanceApiINTEL = (PFN_vkUninitializePerformanceApiINTEL) gpa(device, "vkUninitializePerformanceApiINTEL");
    // if (table->UninitializePerformanceApiINTEL == nullptr) { table->UninitializePerformanceApiINTEL = (PFN_vkUninitializePerformanceApiINTEL)StubUninitializePerformanceApiINTEL; }
    table->CmdSetPerformanceMarkerINTEL = (PFN_vkCmdSetPerformanceMarkerINTEL) gpa(device, "vkCmdSetPerformanceMarkerINTEL");
    // if (table->CmdSetPerformanceMarkerINTEL == nullptr) { table->CmdSetPerformanceMarkerINTEL = (PFN_vkCmdSetPerformanceMarkerINTEL)StubCmdSetPerformanceMarkerINTEL; }
    table->CmdSetPerformanceStreamMarkerINTEL = (PFN_vkCmdSetPerformanceStreamMarkerINTEL) gpa(device, "vkCmdSetPerformanceStreamMarkerINTEL");
    // if (table->CmdSetPerformanceStreamMarkerINTEL == nullptr) { table->CmdSetPerformanceStreamMarkerINTEL = (PFN_vkCmdSetPerformanceStreamMarkerINTEL)StubCmdSetPerformanceStreamMarkerINTEL; }
    table->CmdSetPerformanceOverrideINTEL = (PFN_vkCmdSetPerformanceOverrideINTEL) gpa(device, "vkCmdSetPerformanceOverrideINTEL");
    // if (table->CmdSetPerformanceOverrideINTEL == nullptr) { table->CmdSetPerformanceOverrideINTEL = (PFN_vkCmdSetPerformanceOverrideINTEL)StubCmdSetPerformanceOverrideINTEL; }
    table->AcquirePerformanceConfigurationINTEL = (PFN_vkAcquirePerformanceConfigurationINTEL) gpa(device, "vkAcquirePerformanceConfigurationINTEL");
    // if (table->AcquirePerformanceConfigurationINTEL == nullptr) { table->AcquirePerformanceConfigurationINTEL = (PFN_vkAcquirePerformanceConfigurationINTEL)StubAcquirePerformanceConfigurationINTEL; }
    table->ReleasePerformanceConfigurationINTEL = (PFN_vkReleasePerformanceConfigurationINTEL) gpa(device, "vkReleasePerformanceConfigurationINTEL");
    // if (table->ReleasePerformanceConfigurationINTEL == nullptr) { table->ReleasePerformanceConfigurationINTEL = (PFN_vkReleasePerformanceConfigurationINTEL)StubReleasePerformanceConfigurationINTEL; }
    table->QueueSetPerformanceConfigurationINTEL = (PFN_vkQueueSetPerformanceConfigurationINTEL) gpa(device, "vkQueueSetPerformanceConfigurationINTEL");
    // if (table->QueueSetPerformanceConfigurationINTEL == nullptr) { table->QueueSetPerformanceConfigurationINTEL = (PFN_vkQueueSetPerformanceConfigurationINTEL)StubQueueSetPerformanceConfigurationINTEL; }
    table->GetPerformanceParameterINTEL = (PFN_vkGetPerformanceParameterINTEL) gpa(device, "vkGetPerformanceParameterINTEL");
    // if (table->GetPerformanceParameterINTEL == nullptr) { table->GetPerformanceParameterINTEL = (PFN_vkGetPerformanceParameterINTEL)StubGetPerformanceParameterINTEL; }
    table->SetLocalDimmingAMD = (PFN_vkSetLocalDimmingAMD) gpa(device, "vkSetLocalDimmingAMD");
    // if (table->SetLocalDimmingAMD == nullptr) { table->SetLocalDimmingAMD = (PFN_vkSetLocalDimmingAMD)StubSetLocalDimmingAMD; }
    table->GetBufferDeviceAddressEXT = (PFN_vkGetBufferDeviceAddressEXT) gpa(device, "vkGetBufferDeviceAddressEXT");
    // if (table->GetBufferDeviceAddressEXT == nullptr) { table->GetBufferDeviceAddressEXT = (PFN_vkGetBufferDeviceAddressEXT)StubGetBufferDeviceAddressEXT; }
#ifdef VK_USE_PLATFORM_WIN32_KHR
    table->AcquireFullScreenExclusiveModeEXT = (PFN_vkAcquireFullScreenExclusiveModeEXT) gpa(device, "vkAcquireFullScreenExclusiveModeEXT");
    // if (table->AcquireFullScreenExclusiveModeEXT == nullptr) { table->AcquireFullScreenExclusiveModeEXT = (PFN_vkAcquireFullScreenExclusiveModeEXT)StubAcquireFullScreenExclusiveModeEXT; }
#endif // VK_USE_PLATFORM_WIN32_KHR
#ifdef VK_USE_PLATFORM_WIN32_KHR
    table->ReleaseFullScreenExclusiveModeEXT = (PFN_vkReleaseFullScreenExclusiveModeEXT) gpa(device, "vkReleaseFullScreenExclusiveModeEXT");
    // if (table->ReleaseFullScreenExclusiveModeEXT == nullptr) { table->ReleaseFullScreenExclusiveModeEXT = (PFN_vkReleaseFullScreenExclusiveModeEXT)StubReleaseFullScreenExclusiveModeEXT; }
#endif // VK_USE_PLATFORM_WIN32_KHR
#ifdef VK_USE_PLATFORM_WIN32_KHR
    table->GetDeviceGroupSurfacePresentModes2EXT = (PFN_vkGetDeviceGroupSurfacePresentModes2EXT) gpa(device, "vkGetDeviceGroupSurfacePresentModes2EXT");
    // if (table->GetDeviceGroupSurfacePresentModes2EXT == nullptr) { table->GetDeviceGroupSurfacePresentModes2EXT = (PFN_vkGetDeviceGroupSurfacePresentModes2EXT)StubGetDeviceGroupSurfacePresentModes2EXT; }
#endif // VK_USE_PLATFORM_WIN32_KHR
    table->CmdSetLineStippleEXT = (PFN_vkCmdSetLineStippleEXT) gpa(device, "vkCmdSetLineStippleEXT");
    // if (table->CmdSetLineStippleEXT == nullptr) { table->CmdSetLineStippleEXT = (PFN_vkCmdSetLineStippleEXT)StubCmdSetLineStippleEXT; }
    table->ResetQueryPoolEXT = (PFN_vkResetQueryPoolEXT) gpa(device, "vkResetQueryPoolEXT");
    // if (table->ResetQueryPoolEXT == nullptr) { table->ResetQueryPoolEXT = (PFN_vkResetQueryPoolEXT)StubResetQueryPoolEXT; }
    table->CmdSetCullModeEXT = (PFN_vkCmdSetCullModeEXT) gpa(device, "vkCmdSetCullModeEXT");
    // if (table->CmdSetCullModeEXT == nullptr) { table->CmdSetCullModeEXT = (PFN_vkCmdSetCullModeEXT)StubCmdSetCullModeEXT; }
    table->CmdSetFrontFaceEXT = (PFN_vkCmdSetFrontFaceEXT) gpa(device, "vkCmdSetFrontFaceEXT");
    // if (table->CmdSetFrontFaceEXT == nullptr) { table->CmdSetFrontFaceEXT = (PFN_vkCmdSetFrontFaceEXT)StubCmdSetFrontFaceEXT; }
    table->CmdSetPrimitiveTopologyEXT = (PFN_vkCmdSetPrimitiveTopologyEXT) gpa(device, "vkCmdSetPrimitiveTopologyEXT");
    // if (table->CmdSetPrimitiveTopologyEXT == nullptr) { table->CmdSetPrimitiveTopologyEXT = (PFN_vkCmdSetPrimitiveTopologyEXT)StubCmdSetPrimitiveTopologyEXT; }
    table->CmdSetViewportWithCountEXT = (PFN_vkCmdSetViewportWithCountEXT) gpa(device, "vkCmdSetViewportWithCountEXT");
    // if (table->CmdSetViewportWithCountEXT == nullptr) { table->CmdSetViewportWithCountEXT = (PFN_vkCmdSetViewportWithCountEXT)StubCmdSetViewportWithCountEXT; }
    table->CmdSetScissorWithCountEXT = (PFN_vkCmdSetScissorWithCountEXT) gpa(device, "vkCmdSetScissorWithCountEXT");
    // if (table->CmdSetScissorWithCountEXT == nullptr) { table->CmdSetScissorWithCountEXT = (PFN_vkCmdSetScissorWithCountEXT)StubCmdSetScissorWithCountEXT; }
    table->CmdBindVertexBuffers2EXT = (PFN_vkCmdBindVertexBuffers2EXT) gpa(device, "vkCmdBindVertexBuffers2EXT");
    // if (table->CmdBindVertexBuffers2EXT == nullptr) { table->CmdBindVertexBuffers2EXT = (PFN_vkCmdBindVertexBuffers2EXT)StubCmdBindVertexBuffers2EXT; }
    table->CmdSetDepthTestEnableEXT = (PFN_vkCmdSetDepthTestEnableEXT) gpa(device, "vkCmdSetDepthTestEnableEXT");
    // if (table->CmdSetDepthTestEnableEXT == nullptr) { table->CmdSetDepthTestEnableEXT = (PFN_vkCmdSetDepthTestEnableEXT)StubCmdSetDepthTestEnableEXT; }
    table->CmdSetDepthWriteEnableEXT = (PFN_vkCmdSetDepthWriteEnableEXT) gpa(device, "vkCmdSetDepthWriteEnableEXT");
    // if (table->CmdSetDepthWriteEnableEXT == nullptr) { table->CmdSetDepthWriteEnableEXT = (PFN_vkCmdSetDepthWriteEnableEXT)StubCmdSetDepthWriteEnableEXT; }
    table->CmdSetDepthCompareOpEXT = (PFN_vkCmdSetDepthCompareOpEXT) gpa(device, "vkCmdSetDepthCompareOpEXT");
    // if (table->CmdSetDepthCompareOpEXT == nullptr) { table->CmdSetDepthCompareOpEXT = (PFN_vkCmdSetDepthCompareOpEXT)StubCmdSetDepthCompareOpEXT; }
    table->CmdSetDepthBoundsTestEnableEXT = (PFN_vkCmdSetDepthBoundsTestEnableEXT) gpa(device, "vkCmdSetDepthBoundsTestEnableEXT");
    // if (table->CmdSetDepthBoundsTestEnableEXT == nullptr) { table->CmdSetDepthBoundsTestEnableEXT = (PFN_vkCmdSetDepthBoundsTestEnableEXT)StubCmdSetDepthBoundsTestEnableEXT; }
    table->CmdSetStencilTestEnableEXT = (PFN_vkCmdSetStencilTestEnableEXT) gpa(device, "vkCmdSetStencilTestEnableEXT");
    // if (table->CmdSetStencilTestEnableEXT == nullptr) { table->CmdSetStencilTestEnableEXT = (PFN_vkCmdSetStencilTestEnableEXT)StubCmdSetStencilTestEnableEXT; }
    table->CmdSetStencilOpEXT = (PFN_vkCmdSetStencilOpEXT) gpa(device, "vkCmdSetStencilOpEXT");
    // if (table->CmdSetStencilOpEXT == nullptr) { table->CmdSetStencilOpEXT = (PFN_vkCmdSetStencilOpEXT)StubCmdSetStencilOpEXT; }
    table->GetGeneratedCommandsMemoryRequirementsNV = (PFN_vkGetGeneratedCommandsMemoryRequirementsNV) gpa(device, "vkGetGeneratedCommandsMemoryRequirementsNV");
    // if (table->GetGeneratedCommandsMemoryRequirementsNV == nullptr) { table->GetGeneratedCommandsMemoryRequirementsNV = (PFN_vkGetGeneratedCommandsMemoryRequirementsNV)StubGetGeneratedCommandsMemoryRequirementsNV; }
    table->CmdPreprocessGeneratedCommandsNV = (PFN_vkCmdPreprocessGeneratedCommandsNV) gpa(device, "vkCmdPreprocessGeneratedCommandsNV");
    // if (table->CmdPreprocessGeneratedCommandsNV == nullptr) { table->CmdPreprocessGeneratedCommandsNV = (PFN_vkCmdPreprocessGeneratedCommandsNV)StubCmdPreprocessGeneratedCommandsNV; }
    table->CmdExecuteGeneratedCommandsNV = (PFN_vkCmdExecuteGeneratedCommandsNV) gpa(device, "vkCmdExecuteGeneratedCommandsNV");
    // if (table->CmdExecuteGeneratedCommandsNV == nullptr) { table->CmdExecuteGeneratedCommandsNV = (PFN_vkCmdExecuteGeneratedCommandsNV)StubCmdExecuteGeneratedCommandsNV; }
    table->CmdBindPipelineShaderGroupNV = (PFN_vkCmdBindPipelineShaderGroupNV) gpa(device, "vkCmdBindPipelineShaderGroupNV");
    // if (table->CmdBindPipelineShaderGroupNV == nullptr) { table->CmdBindPipelineShaderGroupNV = (PFN_vkCmdBindPipelineShaderGroupNV)StubCmdBindPipelineShaderGroupNV; }
    table->CreateIndirectCommandsLayoutNV = (PFN_vkCreateIndirectCommandsLayoutNV) gpa(device, "vkCreateIndirectCommandsLayoutNV");
    // if (table->CreateIndirectCommandsLayoutNV == nullptr) { table->CreateIndirectCommandsLayoutNV = (PFN_vkCreateIndirectCommandsLayoutNV)StubCreateIndirectCommandsLayoutNV; }
    table->DestroyIndirectCommandsLayoutNV = (PFN_vkDestroyIndirectCommandsLayoutNV) gpa(device, "vkDestroyIndirectCommandsLayoutNV");
    // if (table->DestroyIndirectCommandsLayoutNV == nullptr) { table->DestroyIndirectCommandsLayoutNV = (PFN_vkDestroyIndirectCommandsLayoutNV)StubDestroyIndirectCommandsLayoutNV; }
    table->CreatePrivateDataSlotEXT = (PFN_vkCreatePrivateDataSlotEXT) gpa(device, "vkCreatePrivateDataSlotEXT");
    // if (table->CreatePrivateDataSlotEXT == nullptr) { table->CreatePrivateDataSlotEXT = (PFN_vkCreatePrivateDataSlotEXT)StubCreatePrivateDataSlotEXT; }
    table->DestroyPrivateDataSlotEXT = (PFN_vkDestroyPrivateDataSlotEXT) gpa(device, "vkDestroyPrivateDataSlotEXT");
    // // if (table->DestroyPrivateDataSlotEXT == nullptr) { table->DestroyPrivateDataSlotEXT = (PFN_vkDestroyPrivateDataSlotEXT)StubDestroyPrivateDataSlotEXT; }
    table->SetPrivateDataEXT = (PFN_vkSetPrivateDataEXT) gpa(device, "vkSetPrivateDataEXT");
    // if (table->SetPrivateDataEXT == nullptr) { table->SetPrivateDataEXT = (PFN_vkSetPrivateDataEXT)StubSetPrivateDataEXT; }
    table->GetPrivateDataEXT = (PFN_vkGetPrivateDataEXT) gpa(device, "vkGetPrivateDataEXT");
    // if (table->GetPrivateDataEXT == nullptr) { table->GetPrivateDataEXT = (PFN_vkGetPrivateDataEXT)StubGetPrivateDataEXT; }
    table->CmdSetFragmentShadingRateEnumNV = (PFN_vkCmdSetFragmentShadingRateEnumNV) gpa(device, "vkCmdSetFragmentShadingRateEnumNV");
    // if (table->CmdSetFragmentShadingRateEnumNV == nullptr) { table->CmdSetFragmentShadingRateEnumNV = (PFN_vkCmdSetFragmentShadingRateEnumNV)StubCmdSetFragmentShadingRateEnumNV; }
    table->CreateAccelerationStructureKHR = (PFN_vkCreateAccelerationStructureKHR) gpa(device, "vkCreateAccelerationStructureKHR");
    // if (table->CreateAccelerationStructureKHR == nullptr) { table->CreateAccelerationStructureKHR = (PFN_vkCreateAccelerationStructureKHR)StubCreateAccelerationStructureKHR; }
    table->DestroyAccelerationStructureKHR = (PFN_vkDestroyAccelerationStructureKHR) gpa(device, "vkDestroyAccelerationStructureKHR");
    // if (table->DestroyAccelerationStructureKHR == nullptr) { table->DestroyAccelerationStructureKHR = (PFN_vkDestroyAccelerationStructureKHR)StubDestroyAccelerationStructureKHR; }
    table->CmdBuildAccelerationStructuresKHR = (PFN_vkCmdBuildAccelerationStructuresKHR) gpa(device, "vkCmdBuildAccelerationStructuresKHR");
    // if (table->CmdBuildAccelerationStructuresKHR == nullptr) { table->CmdBuildAccelerationStructuresKHR = (PFN_vkCmdBuildAccelerationStructuresKHR)StubCmdBuildAccelerationStructuresKHR; }
    table->CmdBuildAccelerationStructuresIndirectKHR = (PFN_vkCmdBuildAccelerationStructuresIndirectKHR) gpa(device, "vkCmdBuildAccelerationStructuresIndirectKHR");
    // if (table->CmdBuildAccelerationStructuresIndirectKHR == nullptr) { table->CmdBuildAccelerationStructuresIndirectKHR = (PFN_vkCmdBuildAccelerationStructuresIndirectKHR)StubCmdBuildAccelerationStructuresIndirectKHR; }
    table->BuildAccelerationStructuresKHR = (PFN_vkBuildAccelerationStructuresKHR) gpa(device, "vkBuildAccelerationStructuresKHR");
    // if (table->BuildAccelerationStructuresKHR == nullptr) { table->BuildAccelerationStructuresKHR = (PFN_vkBuildAccelerationStructuresKHR)StubBuildAccelerationStructuresKHR; }
    table->CopyAccelerationStructureKHR = (PFN_vkCopyAccelerationStructureKHR) gpa(device, "vkCopyAccelerationStructureKHR");
    // if (table->CopyAccelerationStructureKHR == nullptr) { table->CopyAccelerationStructureKHR = (PFN_vkCopyAccelerationStructureKHR)StubCopyAccelerationStructureKHR; }
    table->CopyAccelerationStructureToMemoryKHR = (PFN_vkCopyAccelerationStructureToMemoryKHR) gpa(device, "vkCopyAccelerationStructureToMemoryKHR");
    // if (table->CopyAccelerationStructureToMemoryKHR == nullptr) { table->CopyAccelerationStructureToMemoryKHR = (PFN_vkCopyAccelerationStructureToMemoryKHR)StubCopyAccelerationStructureToMemoryKHR; }
    table->CopyMemoryToAccelerationStructureKHR = (PFN_vkCopyMemoryToAccelerationStructureKHR) gpa(device, "vkCopyMemoryToAccelerationStructureKHR");
    // if (table->CopyMemoryToAccelerationStructureKHR == nullptr) { table->CopyMemoryToAccelerationStructureKHR = (PFN_vkCopyMemoryToAccelerationStructureKHR)StubCopyMemoryToAccelerationStructureKHR; }
    table->WriteAccelerationStructuresPropertiesKHR = (PFN_vkWriteAccelerationStructuresPropertiesKHR) gpa(device, "vkWriteAccelerationStructuresPropertiesKHR");
    // if (table->WriteAccelerationStructuresPropertiesKHR == nullptr) { table->WriteAccelerationStructuresPropertiesKHR = (PFN_vkWriteAccelerationStructuresPropertiesKHR)StubWriteAccelerationStructuresPropertiesKHR; }
    table->CmdCopyAccelerationStructureKHR = (PFN_vkCmdCopyAccelerationStructureKHR) gpa(device, "vkCmdCopyAccelerationStructureKHR");
    // if (table->CmdCopyAccelerationStructureKHR == nullptr) { table->CmdCopyAccelerationStructureKHR = (PFN_vkCmdCopyAccelerationStructureKHR)StubCmdCopyAccelerationStructureKHR; }
    table->CmdCopyAccelerationStructureToMemoryKHR = (PFN_vkCmdCopyAccelerationStructureToMemoryKHR) gpa(device, "vkCmdCopyAccelerationStructureToMemoryKHR");
    // if (table->CmdCopyAccelerationStructureToMemoryKHR == nullptr) { table->CmdCopyAccelerationStructureToMemoryKHR = (PFN_vkCmdCopyAccelerationStructureToMemoryKHR)StubCmdCopyAccelerationStructureToMemoryKHR; }
    table->CmdCopyMemoryToAccelerationStructureKHR = (PFN_vkCmdCopyMemoryToAccelerationStructureKHR) gpa(device, "vkCmdCopyMemoryToAccelerationStructureKHR");
    // if (table->CmdCopyMemoryToAccelerationStructureKHR == nullptr) { table->CmdCopyMemoryToAccelerationStructureKHR = (PFN_vkCmdCopyMemoryToAccelerationStructureKHR)StubCmdCopyMemoryToAccelerationStructureKHR; }
    table->GetAccelerationStructureDeviceAddressKHR = (PFN_vkGetAccelerationStructureDeviceAddressKHR) gpa(device, "vkGetAccelerationStructureDeviceAddressKHR");
    // if (table->GetAccelerationStructureDeviceAddressKHR == nullptr) { table->GetAccelerationStructureDeviceAddressKHR = (PFN_vkGetAccelerationStructureDeviceAddressKHR)StubGetAccelerationStructureDeviceAddressKHR; }
    table->CmdWriteAccelerationStructuresPropertiesKHR = (PFN_vkCmdWriteAccelerationStructuresPropertiesKHR) gpa(device, "vkCmdWriteAccelerationStructuresPropertiesKHR");
    // if (table->CmdWriteAccelerationStructuresPropertiesKHR == nullptr) { table->CmdWriteAccelerationStructuresPropertiesKHR = (PFN_vkCmdWriteAccelerationStructuresPropertiesKHR)StubCmdWriteAccelerationStructuresPropertiesKHR; }
    table->GetDeviceAccelerationStructureCompatibilityKHR = (PFN_vkGetDeviceAccelerationStructureCompatibilityKHR) gpa(device, "vkGetDeviceAccelerationStructureCompatibilityKHR");
    // if (table->GetDeviceAccelerationStructureCompatibilityKHR == nullptr) { table->GetDeviceAccelerationStructureCompatibilityKHR = (PFN_vkGetDeviceAccelerationStructureCompatibilityKHR)StubGetDeviceAccelerationStructureCompatibilityKHR; }
    table->GetAccelerationStructureBuildSizesKHR = (PFN_vkGetAccelerationStructureBuildSizesKHR) gpa(device, "vkGetAccelerationStructureBuildSizesKHR");
    // if (table->GetAccelerationStructureBuildSizesKHR == nullptr) { table->GetAccelerationStructureBuildSizesKHR = (PFN_vkGetAccelerationStructureBuildSizesKHR)StubGetAccelerationStructureBuildSizesKHR; }
    table->CmdTraceRaysKHR = (PFN_vkCmdTraceRaysKHR) gpa(device, "vkCmdTraceRaysKHR");
    // if (table->CmdTraceRaysKHR == nullptr) { table->CmdTraceRaysKHR = (PFN_vkCmdTraceRaysKHR)StubCmdTraceRaysKHR; }
    table->CreateRayTracingPipelinesKHR = (PFN_vkCreateRayTracingPipelinesKHR) gpa(device, "vkCreateRayTracingPipelinesKHR");
    // if (table->CreateRayTracingPipelinesKHR == nullptr) { table->CreateRayTracingPipelinesKHR = (PFN_vkCreateRayTracingPipelinesKHR)StubCreateRayTracingPipelinesKHR; }
    table->GetRayTracingCaptureReplayShaderGroupHandlesKHR = (PFN_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR) gpa(device, "vkGetRayTracingCaptureReplayShaderGroupHandlesKHR");
    // if (table->GetRayTracingCaptureReplayShaderGroupHandlesKHR == nullptr) { table->GetRayTracingCaptureReplayShaderGroupHandlesKHR = (PFN_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR)StubGetRayTracingCaptureReplayShaderGroupHandlesKHR; }
    table->CmdTraceRaysIndirectKHR = (PFN_vkCmdTraceRaysIndirectKHR) gpa(device, "vkCmdTraceRaysIndirectKHR");
    // if (table->CmdTraceRaysIndirectKHR == nullptr) { table->CmdTraceRaysIndirectKHR = (PFN_vkCmdTraceRaysIndirectKHR)StubCmdTraceRaysIndirectKHR; }
    table->GetRayTracingShaderGroupStackSizeKHR = (PFN_vkGetRayTracingShaderGroupStackSizeKHR) gpa(device, "vkGetRayTracingShaderGroupStackSizeKHR");
    // if (table->GetRayTracingShaderGroupStackSizeKHR == nullptr) { table->GetRayTracingShaderGroupStackSizeKHR = (PFN_vkGetRayTracingShaderGroupStackSizeKHR)StubGetRayTracingShaderGroupStackSizeKHR; }
    table->CmdSetRayTracingPipelineStackSizeKHR = (PFN_vkCmdSetRayTracingPipelineStackSizeKHR) gpa(device, "vkCmdSetRayTracingPipelineStackSizeKHR");
    // if (table->CmdSetRayTracingPipelineStackSizeKHR == nullptr) { table->CmdSetRayTracingPipelineStackSizeKHR = (PFN_vkCmdSetRayTracingPipelineStackSizeKHR)StubCmdSetRayTracingPipelineStackSizeKHR; }
}


static inline void layer_init_instance_dispatch_table(VkInstance instance, VkLayerInstanceDispatchTable *table, PFN_vkGetInstanceProcAddr gpa) {
    memset(table, 0, sizeof(*table));
    // Instance function pointers
    table->DestroyInstance = (PFN_vkDestroyInstance) gpa(instance, "vkDestroyInstance");
    table->EnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices) gpa(instance, "vkEnumeratePhysicalDevices");
    table->GetPhysicalDeviceFeatures = (PFN_vkGetPhysicalDeviceFeatures) gpa(instance, "vkGetPhysicalDeviceFeatures");
    table->GetPhysicalDeviceFormatProperties = (PFN_vkGetPhysicalDeviceFormatProperties) gpa(instance, "vkGetPhysicalDeviceFormatProperties");
    table->GetPhysicalDeviceImageFormatProperties = (PFN_vkGetPhysicalDeviceImageFormatProperties) gpa(instance, "vkGetPhysicalDeviceImageFormatProperties");
    table->GetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties) gpa(instance, "vkGetPhysicalDeviceProperties");
    table->GetPhysicalDeviceQueueFamilyProperties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties) gpa(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    table->GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties) gpa(instance, "vkGetPhysicalDeviceMemoryProperties");
    table->GetInstanceProcAddr = gpa;
    table->EnumerateDeviceExtensionProperties = (PFN_vkEnumerateDeviceExtensionProperties) gpa(instance, "vkEnumerateDeviceExtensionProperties");
    table->EnumerateDeviceLayerProperties = (PFN_vkEnumerateDeviceLayerProperties) gpa(instance, "vkEnumerateDeviceLayerProperties");
    table->GetPhysicalDeviceSparseImageFormatProperties = (PFN_vkGetPhysicalDeviceSparseImageFormatProperties) gpa(instance, "vkGetPhysicalDeviceSparseImageFormatProperties");
    table->EnumeratePhysicalDeviceGroups = (PFN_vkEnumeratePhysicalDeviceGroups) gpa(instance, "vkEnumeratePhysicalDeviceGroups");
    // if (table->EnumeratePhysicalDeviceGroups == nullptr) { table->EnumeratePhysicalDeviceGroups = (PFN_vkEnumeratePhysicalDeviceGroups)StubEnumeratePhysicalDeviceGroups; }
    table->GetPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2) gpa(instance, "vkGetPhysicalDeviceFeatures2");
    // if (table->GetPhysicalDeviceFeatures2 == nullptr) { table->GetPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2)StubGetPhysicalDeviceFeatures2; }
    table->GetPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2) gpa(instance, "vkGetPhysicalDeviceProperties2");
    // if (table->GetPhysicalDeviceProperties2 == nullptr) { table->GetPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2)StubGetPhysicalDeviceProperties2; }
    table->GetPhysicalDeviceFormatProperties2 = (PFN_vkGetPhysicalDeviceFormatProperties2) gpa(instance, "vkGetPhysicalDeviceFormatProperties2");
    // if (table->GetPhysicalDeviceFormatProperties2 == nullptr) { table->GetPhysicalDeviceFormatProperties2 = (PFN_vkGetPhysicalDeviceFormatProperties2)StubGetPhysicalDeviceFormatProperties2; }
    table->GetPhysicalDeviceImageFormatProperties2 = (PFN_vkGetPhysicalDeviceImageFormatProperties2) gpa(instance, "vkGetPhysicalDeviceImageFormatProperties2");
    // if (table->GetPhysicalDeviceImageFormatProperties2 == nullptr) { table->GetPhysicalDeviceImageFormatProperties2 = (PFN_vkGetPhysicalDeviceImageFormatProperties2)StubGetPhysicalDeviceImageFormatProperties2; }
    table->GetPhysicalDeviceQueueFamilyProperties2 = (PFN_vkGetPhysicalDeviceQueueFamilyProperties2) gpa(instance, "vkGetPhysicalDeviceQueueFamilyProperties2");
    // if (table->GetPhysicalDeviceQueueFamilyProperties2 == nullptr) { table->GetPhysicalDeviceQueueFamilyProperties2 = (PFN_vkGetPhysicalDeviceQueueFamilyProperties2)StubGetPhysicalDeviceQueueFamilyProperties2; }
    table->GetPhysicalDeviceMemoryProperties2 = (PFN_vkGetPhysicalDeviceMemoryProperties2) gpa(instance, "vkGetPhysicalDeviceMemoryProperties2");
    // if (table->GetPhysicalDeviceMemoryProperties2 == nullptr) { table->GetPhysicalDeviceMemoryProperties2 = (PFN_vkGetPhysicalDeviceMemoryProperties2)StubGetPhysicalDeviceMemoryProperties2; }
    table->GetPhysicalDeviceSparseImageFormatProperties2 = (PFN_vkGetPhysicalDeviceSparseImageFormatProperties2) gpa(instance, "vkGetPhysicalDeviceSparseImageFormatProperties2");
    // if (table->GetPhysicalDeviceSparseImageFormatProperties2 == nullptr) { table->GetPhysicalDeviceSparseImageFormatProperties2 = (PFN_vkGetPhysicalDeviceSparseImageFormatProperties2)StubGetPhysicalDeviceSparseImageFormatProperties2; }
    table->GetPhysicalDeviceExternalBufferProperties = (PFN_vkGetPhysicalDeviceExternalBufferProperties) gpa(instance, "vkGetPhysicalDeviceExternalBufferProperties");
    // if (table->GetPhysicalDeviceExternalBufferProperties == nullptr) { table->GetPhysicalDeviceExternalBufferProperties = (PFN_vkGetPhysicalDeviceExternalBufferProperties)StubGetPhysicalDeviceExternalBufferProperties; }
    table->GetPhysicalDeviceExternalFenceProperties = (PFN_vkGetPhysicalDeviceExternalFenceProperties) gpa(instance, "vkGetPhysicalDeviceExternalFenceProperties");
    // if (table->GetPhysicalDeviceExternalFenceProperties == nullptr) { table->GetPhysicalDeviceExternalFenceProperties = (PFN_vkGetPhysicalDeviceExternalFenceProperties)StubGetPhysicalDeviceExternalFenceProperties; }
    table->GetPhysicalDeviceExternalSemaphoreProperties = (PFN_vkGetPhysicalDeviceExternalSemaphoreProperties) gpa(instance, "vkGetPhysicalDeviceExternalSemaphoreProperties");
    // if (table->GetPhysicalDeviceExternalSemaphoreProperties == nullptr) { table->GetPhysicalDeviceExternalSemaphoreProperties = (PFN_vkGetPhysicalDeviceExternalSemaphoreProperties)StubGetPhysicalDeviceExternalSemaphoreProperties; }
    table->DestroySurfaceKHR = (PFN_vkDestroySurfaceKHR) gpa(instance, "vkDestroySurfaceKHR");
    // if (table->DestroySurfaceKHR == nullptr) { table->DestroySurfaceKHR = (PFN_vkDestroySurfaceKHR)StubDestroySurfaceKHR; }
    table->GetPhysicalDeviceSurfaceSupportKHR = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR) gpa(instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
    // if (table->GetPhysicalDeviceSurfaceSupportKHR == nullptr) { table->GetPhysicalDeviceSurfaceSupportKHR = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)StubGetPhysicalDeviceSurfaceSupportKHR; }
    table->GetPhysicalDeviceSurfaceCapabilitiesKHR = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR) gpa(instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    // if (table->GetPhysicalDeviceSurfaceCapabilitiesKHR == nullptr) { table->GetPhysicalDeviceSurfaceCapabilitiesKHR = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)StubGetPhysicalDeviceSurfaceCapabilitiesKHR; }
    table->GetPhysicalDeviceSurfaceFormatsKHR = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR) gpa(instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    // if (table->GetPhysicalDeviceSurfaceFormatsKHR == nullptr) { table->GetPhysicalDeviceSurfaceFormatsKHR = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)StubGetPhysicalDeviceSurfaceFormatsKHR; }
    table->GetPhysicalDeviceSurfacePresentModesKHR = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR) gpa(instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    // if (table->GetPhysicalDeviceSurfacePresentModesKHR == nullptr) { table->GetPhysicalDeviceSurfacePresentModesKHR = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)StubGetPhysicalDeviceSurfacePresentModesKHR; }
    table->GetPhysicalDevicePresentRectanglesKHR = (PFN_vkGetPhysicalDevicePresentRectanglesKHR) gpa(instance, "vkGetPhysicalDevicePresentRectanglesKHR");
    // if (table->GetPhysicalDevicePresentRectanglesKHR == nullptr) { table->GetPhysicalDevicePresentRectanglesKHR = (PFN_vkGetPhysicalDevicePresentRectanglesKHR)StubGetPhysicalDevicePresentRectanglesKHR; }
    table->GetPhysicalDeviceDisplayPropertiesKHR = (PFN_vkGetPhysicalDeviceDisplayPropertiesKHR) gpa(instance, "vkGetPhysicalDeviceDisplayPropertiesKHR");
    // if (table->GetPhysicalDeviceDisplayPropertiesKHR == nullptr) { table->GetPhysicalDeviceDisplayPropertiesKHR = (PFN_vkGetPhysicalDeviceDisplayPropertiesKHR)StubGetPhysicalDeviceDisplayPropertiesKHR; }
    table->GetPhysicalDeviceDisplayPlanePropertiesKHR = (PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR) gpa(instance, "vkGetPhysicalDeviceDisplayPlanePropertiesKHR");
    // if (table->GetPhysicalDeviceDisplayPlanePropertiesKHR == nullptr) { table->GetPhysicalDeviceDisplayPlanePropertiesKHR = (PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR)StubGetPhysicalDeviceDisplayPlanePropertiesKHR; }
    table->GetDisplayPlaneSupportedDisplaysKHR = (PFN_vkGetDisplayPlaneSupportedDisplaysKHR) gpa(instance, "vkGetDisplayPlaneSupportedDisplaysKHR");
    // if (table->GetDisplayPlaneSupportedDisplaysKHR == nullptr) { table->GetDisplayPlaneSupportedDisplaysKHR = (PFN_vkGetDisplayPlaneSupportedDisplaysKHR)StubGetDisplayPlaneSupportedDisplaysKHR; }
    table->GetDisplayModePropertiesKHR = (PFN_vkGetDisplayModePropertiesKHR) gpa(instance, "vkGetDisplayModePropertiesKHR");
    // if (table->GetDisplayModePropertiesKHR == nullptr) { table->GetDisplayModePropertiesKHR = (PFN_vkGetDisplayModePropertiesKHR)StubGetDisplayModePropertiesKHR; }
    table->CreateDisplayModeKHR = (PFN_vkCreateDisplayModeKHR) gpa(instance, "vkCreateDisplayModeKHR");
    // if (table->CreateDisplayModeKHR == nullptr) { table->CreateDisplayModeKHR = (PFN_vkCreateDisplayModeKHR)StubCreateDisplayModeKHR; }
    table->GetDisplayPlaneCapabilitiesKHR = (PFN_vkGetDisplayPlaneCapabilitiesKHR) gpa(instance, "vkGetDisplayPlaneCapabilitiesKHR");
    // if (table->GetDisplayPlaneCapabilitiesKHR == nullptr) { table->GetDisplayPlaneCapabilitiesKHR = (PFN_vkGetDisplayPlaneCapabilitiesKHR)StubGetDisplayPlaneCapabilitiesKHR; }
    table->CreateDisplayPlaneSurfaceKHR = (PFN_vkCreateDisplayPlaneSurfaceKHR) gpa(instance, "vkCreateDisplayPlaneSurfaceKHR");
    // if (table->CreateDisplayPlaneSurfaceKHR == nullptr) { table->CreateDisplayPlaneSurfaceKHR = (PFN_vkCreateDisplayPlaneSurfaceKHR)StubCreateDisplayPlaneSurfaceKHR; }
#ifdef VK_USE_PLATFORM_XLIB_KHR
    table->CreateXlibSurfaceKHR = (PFN_vkCreateXlibSurfaceKHR) gpa(instance, "vkCreateXlibSurfaceKHR");
    // if (table->CreateXlibSurfaceKHR == nullptr) { table->CreateXlibSurfaceKHR = (PFN_vkCreateXlibSurfaceKHR)StubCreateXlibSurfaceKHR; }
#endif // VK_USE_PLATFORM_XLIB_KHR
#ifdef VK_USE_PLATFORM_XLIB_KHR
    table->GetPhysicalDeviceXlibPresentationSupportKHR = (PFN_vkGetPhysicalDeviceXlibPresentationSupportKHR) gpa(instance, "vkGetPhysicalDeviceXlibPresentationSupportKHR");
    // if (table->GetPhysicalDeviceXlibPresentationSupportKHR == nullptr) { table->GetPhysicalDeviceXlibPresentationSupportKHR = (PFN_vkGetPhysicalDeviceXlibPresentationSupportKHR)StubGetPhysicalDeviceXlibPresentationSupportKHR; }
#endif // VK_USE_PLATFORM_XLIB_KHR
#ifdef VK_USE_PLATFORM_XCB_KHR
    table->CreateXcbSurfaceKHR = (PFN_vkCreateXcbSurfaceKHR) gpa(instance, "vkCreateXcbSurfaceKHR");
    // if (table->CreateXcbSurfaceKHR == nullptr) { table->CreateXcbSurfaceKHR = (PFN_vkCreateXcbSurfaceKHR)StubCreateXcbSurfaceKHR; }
#endif // VK_USE_PLATFORM_XCB_KHR
#ifdef VK_USE_PLATFORM_XCB_KHR
    table->GetPhysicalDeviceXcbPresentationSupportKHR = (PFN_vkGetPhysicalDeviceXcbPresentationSupportKHR) gpa(instance, "vkGetPhysicalDeviceXcbPresentationSupportKHR");
    // if (table->GetPhysicalDeviceXcbPresentationSupportKHR == nullptr) { table->GetPhysicalDeviceXcbPresentationSupportKHR = (PFN_vkGetPhysicalDeviceXcbPresentationSupportKHR)StubGetPhysicalDeviceXcbPresentationSupportKHR; }
#endif // VK_USE_PLATFORM_XCB_KHR
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
    table->CreateWaylandSurfaceKHR = (PFN_vkCreateWaylandSurfaceKHR) gpa(instance, "vkCreateWaylandSurfaceKHR");
    // if (table->CreateWaylandSurfaceKHR == nullptr) { table->CreateWaylandSurfaceKHR = (PFN_vkCreateWaylandSurfaceKHR)StubCreateWaylandSurfaceKHR; }
#endif // VK_USE_PLATFORM_WAYLAND_KHR
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
    table->GetPhysicalDeviceWaylandPresentationSupportKHR = (PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR) gpa(instance, "vkGetPhysicalDeviceWaylandPresentationSupportKHR");
    // if (table->GetPhysicalDeviceWaylandPresentationSupportKHR == nullptr) { table->GetPhysicalDeviceWaylandPresentationSupportKHR = (PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR)StubGetPhysicalDeviceWaylandPresentationSupportKHR; }
#endif // VK_USE_PLATFORM_WAYLAND_KHR
#ifdef VK_USE_PLATFORM_ANDROID_KHR
    table->CreateAndroidSurfaceKHR = (PFN_vkCreateAndroidSurfaceKHR) gpa(instance, "vkCreateAndroidSurfaceKHR");
    // if (table->CreateAndroidSurfaceKHR == nullptr) { table->CreateAndroidSurfaceKHR = (PFN_vkCreateAndroidSurfaceKHR)StubCreateAndroidSurfaceKHR; }
#endif // VK_USE_PLATFORM_ANDROID_KHR
#ifdef VK_USE_PLATFORM_WIN32_KHR
    table->CreateWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR) gpa(instance, "vkCreateWin32SurfaceKHR");
    // if (table->CreateWin32SurfaceKHR == nullptr) { table->CreateWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR)StubCreateWin32SurfaceKHR; }
#endif // VK_USE_PLATFORM_WIN32_KHR
#ifdef VK_USE_PLATFORM_WIN32_KHR
    table->GetPhysicalDeviceWin32PresentationSupportKHR = (PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR) gpa(instance, "vkGetPhysicalDeviceWin32PresentationSupportKHR");
    // if (table->GetPhysicalDeviceWin32PresentationSupportKHR == nullptr) { table->GetPhysicalDeviceWin32PresentationSupportKHR = (PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR)StubGetPhysicalDeviceWin32PresentationSupportKHR; }
#endif // VK_USE_PLATFORM_WIN32_KHR
    table->GetPhysicalDeviceFeatures2KHR = (PFN_vkGetPhysicalDeviceFeatures2KHR) gpa(instance, "vkGetPhysicalDeviceFeatures2KHR");
    // if (table->GetPhysicalDeviceFeatures2KHR == nullptr) { table->GetPhysicalDeviceFeatures2KHR = (PFN_vkGetPhysicalDeviceFeatures2KHR)StubGetPhysicalDeviceFeatures2KHR; }
    table->GetPhysicalDeviceProperties2KHR = (PFN_vkGetPhysicalDeviceProperties2KHR) gpa(instance, "vkGetPhysicalDeviceProperties2KHR");
    // if (table->GetPhysicalDeviceProperties2KHR == nullptr) { table->GetPhysicalDeviceProperties2KHR = (PFN_vkGetPhysicalDeviceProperties2KHR)StubGetPhysicalDeviceProperties2KHR; }
    table->GetPhysicalDeviceFormatProperties2KHR = (PFN_vkGetPhysicalDeviceFormatProperties2KHR) gpa(instance, "vkGetPhysicalDeviceFormatProperties2KHR");
    // if (table->GetPhysicalDeviceFormatProperties2KHR == nullptr) { table->GetPhysicalDeviceFormatProperties2KHR = (PFN_vkGetPhysicalDeviceFormatProperties2KHR)StubGetPhysicalDeviceFormatProperties2KHR; }
    table->GetPhysicalDeviceImageFormatProperties2KHR = (PFN_vkGetPhysicalDeviceImageFormatProperties2KHR) gpa(instance, "vkGetPhysicalDeviceImageFormatProperties2KHR");
    // if (table->GetPhysicalDeviceImageFormatProperties2KHR == nullptr) { table->GetPhysicalDeviceImageFormatProperties2KHR = (PFN_vkGetPhysicalDeviceImageFormatProperties2KHR)StubGetPhysicalDeviceImageFormatProperties2KHR; }
    table->GetPhysicalDeviceQueueFamilyProperties2KHR = (PFN_vkGetPhysicalDeviceQueueFamilyProperties2KHR) gpa(instance, "vkGetPhysicalDeviceQueueFamilyProperties2KHR");
    // if (table->GetPhysicalDeviceQueueFamilyProperties2KHR == nullptr) { table->GetPhysicalDeviceQueueFamilyProperties2KHR = (PFN_vkGetPhysicalDeviceQueueFamilyProperties2KHR)StubGetPhysicalDeviceQueueFamilyProperties2KHR; }
    table->GetPhysicalDeviceMemoryProperties2KHR = (PFN_vkGetPhysicalDeviceMemoryProperties2KHR) gpa(instance, "vkGetPhysicalDeviceMemoryProperties2KHR");
    // if (table->GetPhysicalDeviceMemoryProperties2KHR == nullptr) { table->GetPhysicalDeviceMemoryProperties2KHR = (PFN_vkGetPhysicalDeviceMemoryProperties2KHR)StubGetPhysicalDeviceMemoryProperties2KHR; }
    table->GetPhysicalDeviceSparseImageFormatProperties2KHR = (PFN_vkGetPhysicalDeviceSparseImageFormatProperties2KHR) gpa(instance, "vkGetPhysicalDeviceSparseImageFormatProperties2KHR");
    // if (table->GetPhysicalDeviceSparseImageFormatProperties2KHR == nullptr) { table->GetPhysicalDeviceSparseImageFormatProperties2KHR = (PFN_vkGetPhysicalDeviceSparseImageFormatProperties2KHR)StubGetPhysicalDeviceSparseImageFormatProperties2KHR; }
    table->EnumeratePhysicalDeviceGroupsKHR = (PFN_vkEnumeratePhysicalDeviceGroupsKHR) gpa(instance, "vkEnumeratePhysicalDeviceGroupsKHR");
    // if (table->EnumeratePhysicalDeviceGroupsKHR == nullptr) { table->EnumeratePhysicalDeviceGroupsKHR = (PFN_vkEnumeratePhysicalDeviceGroupsKHR)StubEnumeratePhysicalDeviceGroupsKHR; }
    table->GetPhysicalDeviceExternalBufferPropertiesKHR = (PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR) gpa(instance, "vkGetPhysicalDeviceExternalBufferPropertiesKHR");
    // if (table->GetPhysicalDeviceExternalBufferPropertiesKHR == nullptr) { table->GetPhysicalDeviceExternalBufferPropertiesKHR = (PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR)StubGetPhysicalDeviceExternalBufferPropertiesKHR; }
    table->GetPhysicalDeviceExternalSemaphorePropertiesKHR = (PFN_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR) gpa(instance, "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR");
    // if (table->GetPhysicalDeviceExternalSemaphorePropertiesKHR == nullptr) { table->GetPhysicalDeviceExternalSemaphorePropertiesKHR = (PFN_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR)StubGetPhysicalDeviceExternalSemaphorePropertiesKHR; }
    table->GetPhysicalDeviceExternalFencePropertiesKHR = (PFN_vkGetPhysicalDeviceExternalFencePropertiesKHR) gpa(instance, "vkGetPhysicalDeviceExternalFencePropertiesKHR");
    // if (table->GetPhysicalDeviceExternalFencePropertiesKHR == nullptr) { table->GetPhysicalDeviceExternalFencePropertiesKHR = (PFN_vkGetPhysicalDeviceExternalFencePropertiesKHR)StubGetPhysicalDeviceExternalFencePropertiesKHR; }
    table->EnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR = (PFN_vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR) gpa(instance, "vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR");
    // if (table->EnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR == nullptr) { table->EnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR = (PFN_vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR)StubEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR; }
    table->GetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR = (PFN_vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR) gpa(instance, "vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR");
    // if (table->GetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR == nullptr) { table->GetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR = (PFN_vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR)StubGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR; }
    table->GetPhysicalDeviceSurfaceCapabilities2KHR = (PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR) gpa(instance, "vkGetPhysicalDeviceSurfaceCapabilities2KHR");
    // if (table->GetPhysicalDeviceSurfaceCapabilities2KHR == nullptr) { table->GetPhysicalDeviceSurfaceCapabilities2KHR = (PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)StubGetPhysicalDeviceSurfaceCapabilities2KHR; }
    table->GetPhysicalDeviceSurfaceFormats2KHR = (PFN_vkGetPhysicalDeviceSurfaceFormats2KHR) gpa(instance, "vkGetPhysicalDeviceSurfaceFormats2KHR");
    // if (table->GetPhysicalDeviceSurfaceFormats2KHR == nullptr) { table->GetPhysicalDeviceSurfaceFormats2KHR = (PFN_vkGetPhysicalDeviceSurfaceFormats2KHR)StubGetPhysicalDeviceSurfaceFormats2KHR; }
    table->GetPhysicalDeviceDisplayProperties2KHR = (PFN_vkGetPhysicalDeviceDisplayProperties2KHR) gpa(instance, "vkGetPhysicalDeviceDisplayProperties2KHR");
    // if (table->GetPhysicalDeviceDisplayProperties2KHR == nullptr) { table->GetPhysicalDeviceDisplayProperties2KHR = (PFN_vkGetPhysicalDeviceDisplayProperties2KHR)StubGetPhysicalDeviceDisplayProperties2KHR; }
    table->GetPhysicalDeviceDisplayPlaneProperties2KHR = (PFN_vkGetPhysicalDeviceDisplayPlaneProperties2KHR) gpa(instance, "vkGetPhysicalDeviceDisplayPlaneProperties2KHR");
    // if (table->GetPhysicalDeviceDisplayPlaneProperties2KHR == nullptr) { table->GetPhysicalDeviceDisplayPlaneProperties2KHR = (PFN_vkGetPhysicalDeviceDisplayPlaneProperties2KHR)StubGetPhysicalDeviceDisplayPlaneProperties2KHR; }
    table->GetDisplayModeProperties2KHR = (PFN_vkGetDisplayModeProperties2KHR) gpa(instance, "vkGetDisplayModeProperties2KHR");
    // if (table->GetDisplayModeProperties2KHR == nullptr) { table->GetDisplayModeProperties2KHR = (PFN_vkGetDisplayModeProperties2KHR)StubGetDisplayModeProperties2KHR; }
    table->GetDisplayPlaneCapabilities2KHR = (PFN_vkGetDisplayPlaneCapabilities2KHR) gpa(instance, "vkGetDisplayPlaneCapabilities2KHR");
    // if (table->GetDisplayPlaneCapabilities2KHR == nullptr) { table->GetDisplayPlaneCapabilities2KHR = (PFN_vkGetDisplayPlaneCapabilities2KHR)StubGetDisplayPlaneCapabilities2KHR; }
    table->GetPhysicalDeviceFragmentShadingRatesKHR = (PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR) gpa(instance, "vkGetPhysicalDeviceFragmentShadingRatesKHR");
    // if (table->GetPhysicalDeviceFragmentShadingRatesKHR == nullptr) { table->GetPhysicalDeviceFragmentShadingRatesKHR = (PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR)StubGetPhysicalDeviceFragmentShadingRatesKHR; }
    table->CreateDebugReportCallbackEXT = (PFN_vkCreateDebugReportCallbackEXT) gpa(instance, "vkCreateDebugReportCallbackEXT");
    // if (table->CreateDebugReportCallbackEXT == nullptr) { table->CreateDebugReportCallbackEXT = (PFN_vkCreateDebugReportCallbackEXT)StubCreateDebugReportCallbackEXT; }
    table->DestroyDebugReportCallbackEXT = (PFN_vkDestroyDebugReportCallbackEXT) gpa(instance, "vkDestroyDebugReportCallbackEXT");
    // if (table->DestroyDebugReportCallbackEXT == nullptr) { table->DestroyDebugReportCallbackEXT = (PFN_vkDestroyDebugReportCallbackEXT)StubDestroyDebugReportCallbackEXT; }
    table->DebugReportMessageEXT = (PFN_vkDebugReportMessageEXT) gpa(instance, "vkDebugReportMessageEXT");
    // if (table->DebugReportMessageEXT == nullptr) { table->DebugReportMessageEXT = (PFN_vkDebugReportMessageEXT)StubDebugReportMessageEXT; }
#ifdef VK_USE_PLATFORM_GGP
    table->CreateStreamDescriptorSurfaceGGP = (PFN_vkCreateStreamDescriptorSurfaceGGP) gpa(instance, "vkCreateStreamDescriptorSurfaceGGP");
    // if (table->CreateStreamDescriptorSurfaceGGP == nullptr) { table->CreateStreamDescriptorSurfaceGGP = (PFN_vkCreateStreamDescriptorSurfaceGGP)StubCreateStreamDescriptorSurfaceGGP; }
#endif // VK_USE_PLATFORM_GGP
    table->GetPhysicalDeviceExternalImageFormatPropertiesNV = (PFN_vkGetPhysicalDeviceExternalImageFormatPropertiesNV) gpa(instance, "vkGetPhysicalDeviceExternalImageFormatPropertiesNV");
    // if (table->GetPhysicalDeviceExternalImageFormatPropertiesNV == nullptr) { table->GetPhysicalDeviceExternalImageFormatPropertiesNV = (PFN_vkGetPhysicalDeviceExternalImageFormatPropertiesNV)StubGetPhysicalDeviceExternalImageFormatPropertiesNV; }
#ifdef VK_USE_PLATFORM_VI_NN
    table->CreateViSurfaceNN = (PFN_vkCreateViSurfaceNN) gpa(instance, "vkCreateViSurfaceNN");
    // if (table->CreateViSurfaceNN == nullptr) { table->CreateViSurfaceNN = (PFN_vkCreateViSurfaceNN)StubCreateViSurfaceNN; }
#endif // VK_USE_PLATFORM_VI_NN
    table->ReleaseDisplayEXT = (PFN_vkReleaseDisplayEXT) gpa(instance, "vkReleaseDisplayEXT");
    // if (table->ReleaseDisplayEXT == nullptr) { table->ReleaseDisplayEXT = (PFN_vkReleaseDisplayEXT)StubReleaseDisplayEXT; }
#ifdef VK_USE_PLATFORM_XLIB_XRANDR_EXT
    table->AcquireXlibDisplayEXT = (PFN_vkAcquireXlibDisplayEXT) gpa(instance, "vkAcquireXlibDisplayEXT");
    // if (table->AcquireXlibDisplayEXT == nullptr) { table->AcquireXlibDisplayEXT = (PFN_vkAcquireXlibDisplayEXT)StubAcquireXlibDisplayEXT; }
#endif // VK_USE_PLATFORM_XLIB_XRANDR_EXT
#ifdef VK_USE_PLATFORM_XLIB_XRANDR_EXT
    table->GetRandROutputDisplayEXT = (PFN_vkGetRandROutputDisplayEXT) gpa(instance, "vkGetRandROutputDisplayEXT");
    // if (table->GetRandROutputDisplayEXT == nullptr) { table->GetRandROutputDisplayEXT = (PFN_vkGetRandROutputDisplayEXT)StubGetRandROutputDisplayEXT; }
#endif // VK_USE_PLATFORM_XLIB_XRANDR_EXT
    table->GetPhysicalDeviceSurfaceCapabilities2EXT = (PFN_vkGetPhysicalDeviceSurfaceCapabilities2EXT) gpa(instance, "vkGetPhysicalDeviceSurfaceCapabilities2EXT");
    // if (table->GetPhysicalDeviceSurfaceCapabilities2EXT == nullptr) { table->GetPhysicalDeviceSurfaceCapabilities2EXT = (PFN_vkGetPhysicalDeviceSurfaceCapabilities2EXT)StubGetPhysicalDeviceSurfaceCapabilities2EXT; }
#ifdef VK_USE_PLATFORM_IOS_MVK
    table->CreateIOSSurfaceMVK = (PFN_vkCreateIOSSurfaceMVK) gpa(instance, "vkCreateIOSSurfaceMVK");
    // if (table->CreateIOSSurfaceMVK == nullptr) { table->CreateIOSSurfaceMVK = (PFN_vkCreateIOSSurfaceMVK)StubCreateIOSSurfaceMVK; }
#endif // VK_USE_PLATFORM_IOS_MVK
#ifdef VK_USE_PLATFORM_MACOS_MVK
    table->CreateMacOSSurfaceMVK = (PFN_vkCreateMacOSSurfaceMVK) gpa(instance, "vkCreateMacOSSurfaceMVK");
    // if (table->CreateMacOSSurfaceMVK == nullptr) { table->CreateMacOSSurfaceMVK = (PFN_vkCreateMacOSSurfaceMVK)StubCreateMacOSSurfaceMVK; }
#endif // VK_USE_PLATFORM_MACOS_MVK
    table->CreateDebugUtilsMessengerEXT = (PFN_vkCreateDebugUtilsMessengerEXT) gpa(instance, "vkCreateDebugUtilsMessengerEXT");
    // if (table->CreateDebugUtilsMessengerEXT == nullptr) { table->CreateDebugUtilsMessengerEXT = (PFN_vkCreateDebugUtilsMessengerEXT)StubCreateDebugUtilsMessengerEXT; }
    table->DestroyDebugUtilsMessengerEXT = (PFN_vkDestroyDebugUtilsMessengerEXT) gpa(instance, "vkDestroyDebugUtilsMessengerEXT");
    // if (table->DestroyDebugUtilsMessengerEXT == nullptr) { table->DestroyDebugUtilsMessengerEXT = (PFN_vkDestroyDebugUtilsMessengerEXT)StubDestroyDebugUtilsMessengerEXT; }
    table->SubmitDebugUtilsMessageEXT = (PFN_vkSubmitDebugUtilsMessageEXT) gpa(instance, "vkSubmitDebugUtilsMessageEXT");
    // if (table->SubmitDebugUtilsMessageEXT == nullptr) { table->SubmitDebugUtilsMessageEXT = (PFN_vkSubmitDebugUtilsMessageEXT)StubSubmitDebugUtilsMessageEXT; }
    table->GetPhysicalDeviceMultisamplePropertiesEXT = (PFN_vkGetPhysicalDeviceMultisamplePropertiesEXT) gpa(instance, "vkGetPhysicalDeviceMultisamplePropertiesEXT");
    // if (table->GetPhysicalDeviceMultisamplePropertiesEXT == nullptr) { table->GetPhysicalDeviceMultisamplePropertiesEXT = (PFN_vkGetPhysicalDeviceMultisamplePropertiesEXT)StubGetPhysicalDeviceMultisamplePropertiesEXT; }
    table->GetPhysicalDeviceCalibrateableTimeDomainsEXT = (PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT) gpa(instance, "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT");
    // if (table->GetPhysicalDeviceCalibrateableTimeDomainsEXT == nullptr) { table->GetPhysicalDeviceCalibrateableTimeDomainsEXT = (PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT)StubGetPhysicalDeviceCalibrateableTimeDomainsEXT; }
#ifdef VK_USE_PLATFORM_FUCHSIA
    table->CreateImagePipeSurfaceFUCHSIA = (PFN_vkCreateImagePipeSurfaceFUCHSIA) gpa(instance, "vkCreateImagePipeSurfaceFUCHSIA");
    // if (table->CreateImagePipeSurfaceFUCHSIA == nullptr) { table->CreateImagePipeSurfaceFUCHSIA = (PFN_vkCreateImagePipeSurfaceFUCHSIA)StubCreateImagePipeSurfaceFUCHSIA; }
#endif // VK_USE_PLATFORM_FUCHSIA
#ifdef VK_USE_PLATFORM_METAL_EXT
    table->CreateMetalSurfaceEXT = (PFN_vkCreateMetalSurfaceEXT) gpa(instance, "vkCreateMetalSurfaceEXT");
    // if (table->CreateMetalSurfaceEXT == nullptr) { table->CreateMetalSurfaceEXT = (PFN_vkCreateMetalSurfaceEXT)StubCreateMetalSurfaceEXT; }
#endif // VK_USE_PLATFORM_METAL_EXT
    table->GetPhysicalDeviceToolPropertiesEXT = (PFN_vkGetPhysicalDeviceToolPropertiesEXT) gpa(instance, "vkGetPhysicalDeviceToolPropertiesEXT");
    // if (table->GetPhysicalDeviceToolPropertiesEXT == nullptr) { table->GetPhysicalDeviceToolPropertiesEXT = (PFN_vkGetPhysicalDeviceToolPropertiesEXT)StubGetPhysicalDeviceToolPropertiesEXT; }
    table->GetPhysicalDeviceCooperativeMatrixPropertiesNV = (PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesNV) gpa(instance, "vkGetPhysicalDeviceCooperativeMatrixPropertiesNV");
    // if (table->GetPhysicalDeviceCooperativeMatrixPropertiesNV == nullptr) { table->GetPhysicalDeviceCooperativeMatrixPropertiesNV = (PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesNV)StubGetPhysicalDeviceCooperativeMatrixPropertiesNV; }
    table->GetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV = (PFN_vkGetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV) gpa(instance, "vkGetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV");
    // if (table->GetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV == nullptr) { table->GetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV = (PFN_vkGetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV)StubGetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV; }
#ifdef VK_USE_PLATFORM_WIN32_KHR
    table->GetPhysicalDeviceSurfacePresentModes2EXT = (PFN_vkGetPhysicalDeviceSurfacePresentModes2EXT) gpa(instance, "vkGetPhysicalDeviceSurfacePresentModes2EXT");
    // if (table->GetPhysicalDeviceSurfacePresentModes2EXT == nullptr) { table->GetPhysicalDeviceSurfacePresentModes2EXT = (PFN_vkGetPhysicalDeviceSurfacePresentModes2EXT)StubGetPhysicalDeviceSurfacePresentModes2EXT; }
#endif // VK_USE_PLATFORM_WIN32_KHR
    table->CreateHeadlessSurfaceEXT = (PFN_vkCreateHeadlessSurfaceEXT) gpa(instance, "vkCreateHeadlessSurfaceEXT");
    // if (table->CreateHeadlessSurfaceEXT == nullptr) { table->CreateHeadlessSurfaceEXT = (PFN_vkCreateHeadlessSurfaceEXT)StubCreateHeadlessSurfaceEXT; }
#ifdef VK_USE_PLATFORM_DIRECTFB_EXT
    table->CreateDirectFBSurfaceEXT = (PFN_vkCreateDirectFBSurfaceEXT) gpa(instance, "vkCreateDirectFBSurfaceEXT");
    // if (table->CreateDirectFBSurfaceEXT == nullptr) { table->CreateDirectFBSurfaceEXT = (PFN_vkCreateDirectFBSurfaceEXT)StubCreateDirectFBSurfaceEXT; }
#endif // VK_USE_PLATFORM_DIRECTFB_EXT
#ifdef VK_USE_PLATFORM_DIRECTFB_EXT
    table->GetPhysicalDeviceDirectFBPresentationSupportEXT = (PFN_vkGetPhysicalDeviceDirectFBPresentationSupportEXT) gpa(instance, "vkGetPhysicalDeviceDirectFBPresentationSupportEXT");
    // if (table->GetPhysicalDeviceDirectFBPresentationSupportEXT == nullptr) { table->GetPhysicalDeviceDirectFBPresentationSupportEXT = (PFN_vkGetPhysicalDeviceDirectFBPresentationSupportEXT)StubGetPhysicalDeviceDirectFBPresentationSupportEXT; }
#endif // VK_USE_PLATFORM_DIRECTFB_EXT
}


#ifdef __GNUC__
    #pragma GCC diagnostic pop
#elif defined(_MSC_VER)
    #pragma warning(pop)
#endif
