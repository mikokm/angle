//
// Copyright 2018 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// vk_helpers:
//   Helper utilitiy classes that manage Vulkan resources.

#include "libANGLE/renderer/vulkan/vk_helpers.h"

#include "common/utilities.h"
#include "image_util/loadimage.h"
#include "libANGLE/Context.h"
#include "libANGLE/renderer/renderer_utils.h"
#include "libANGLE/renderer/vulkan/BufferVk.h"
#include "libANGLE/renderer/vulkan/ContextVk.h"
#include "libANGLE/renderer/vulkan/DisplayVk.h"
#include "libANGLE/renderer/vulkan/FramebufferVk.h"
#include "libANGLE/renderer/vulkan/RenderTargetVk.h"
#include "libANGLE/renderer/vulkan/RendererVk.h"
#include "libANGLE/renderer/vulkan/vk_utils.h"
#include "libANGLE/trace.h"

namespace rx
{
namespace vk
{
namespace
{
// WebGL requires color textures to be initialized to transparent black.
constexpr VkClearColorValue kWebGLInitColorValue = {{0, 0, 0, 0}};
// When emulating a texture, we want the emulated channels to be 0, with alpha 1.
constexpr VkClearColorValue kEmulatedInitColorValue = {{0, 0, 0, 1.0f}};
// WebGL requires depth/stencil textures to be initialized to depth=1, stencil=0.  We are fine with
// these values for emulated depth/stencil textures too.
constexpr VkClearDepthStencilValue kWebGLInitDepthStencilValue = {1.0f, 0};

constexpr VkBufferUsageFlags kLineLoopDynamicBufferUsage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
constexpr int kLineLoopDynamicBufferInitialSize = 1024 * 1024;
constexpr VkBufferUsageFlags kLineLoopDynamicIndirectBufferUsage =
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
constexpr int kLineLoopDynamicIndirectBufferInitialSize = sizeof(VkDrawIndirectCommand) * 16;

// This is an arbitrary max. We can change this later if necessary.
constexpr uint32_t kDefaultDescriptorPoolMaxSets = 128;

struct ImageMemoryBarrierData
{
    // The Vk layout corresponding to the ImageLayout key.
    VkImageLayout layout;
    // The stage in which the image is used (or Bottom/Top if not using any specific stage).  Unless
    // Bottom/Top (Bottom used for transition to and Top used for transition from), the two values
    // should match.
    VkPipelineStageFlags dstStageMask;
    VkPipelineStageFlags srcStageMask;
    // Access mask when transitioning into this layout.
    VkAccessFlags dstAccessMask;
    // Access mask when transitioning out from this layout.  Note that source access mask never
    // needs a READ bit, as WAR hazards don't need memory barriers (just execution barriers).
    VkAccessFlags srcAccessMask;

    // If access is read-only, the execution barrier can be skipped altogether if retransitioning to
    // the same layout.  This is because read-after-read does not need an execution or memory
    // barrier.
    //
    // Otherwise, same-layout transitions only require an execution barrier (and not a memory
    // barrier).
    bool sameLayoutTransitionRequiresBarrier;
};

// clang-format off
constexpr angle::PackedEnumMap<ImageLayout, ImageMemoryBarrierData> kImageMemoryBarrierData = {
    {
        ImageLayout::Undefined,
        {
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            // Transition to: we don't expect to transition into Undefined.
            0,
            // Transition from: there's no data in the image to care about.
            0,
            false,
        },
    },
    {
        ImageLayout::ExternalPreInitialized,
        {
            VK_IMAGE_LAYOUT_PREINITIALIZED,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            // Transition to: we don't expect to transition into PreInitialized.
            0,
            // Transition from: all writes must finish before barrier.
            VK_ACCESS_MEMORY_WRITE_BIT,
            false,
        },
    },
    {
        ImageLayout::TransferSrc,
        {
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            // Transition to: all reads must happen after barrier.
            VK_ACCESS_TRANSFER_READ_BIT,
            // Transition from: RAR and WAR don't need memory barrier.
            0,
            false,
        },
    },
    {
        ImageLayout::TransferDst,
        {
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            // Transition to: all writes must happen after barrier.
            VK_ACCESS_TRANSFER_WRITE_BIT,
            // Transition from: all writes must finish before barrier.
            VK_ACCESS_TRANSFER_WRITE_BIT,
            true,
        },
    },
    {
        ImageLayout::ComputeShaderReadOnly,
        {
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            // Transition to: all reads must happen after barrier.
            VK_ACCESS_SHADER_READ_BIT,
            // Transition from: RAR and WAR don't need memory barrier.
            0,
            false,
        },
    },
    {
        ImageLayout::ComputeShaderWrite,
        {
            VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            // Transition to: all reads and writes must happen after barrier.
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            // Transition from: all writes must finish before barrier.
            VK_ACCESS_SHADER_WRITE_BIT,
            true,
        },
    },
    {
        ImageLayout::AllGraphicsShadersReadOnly,
        {
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
            VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
            // Transition to: all reads must happen after barrier.
            VK_ACCESS_SHADER_READ_BIT,
            // Transition from: RAR and WAR don't need memory barrier.
            0,
            false,
        },
    },
    {
        ImageLayout::AllGraphicsShadersWrite,
        {
            VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
            VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
            // Transition to: all reads and writes must happen after barrier.
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            // Transition from: all writes must finish before barrier.
            VK_ACCESS_SHADER_WRITE_BIT,
            true,
        },
    },
    {
        ImageLayout::ColorAttachment,
        {
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            // Transition to: all reads and writes must happen after barrier.
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            // Transition from: all writes must finish before barrier.
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            true,
        },
    },
    {
        ImageLayout::DepthStencilAttachment,
        {
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            // Transition to: all reads and writes must happen after barrier.
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            // Transition from: all writes must finish before barrier.
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            true,
        },
    },
    {
        ImageLayout::Present,
        {
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            // transition to: vkQueuePresentKHR automatically performs the appropriate memory barriers:
            //
            // > Any writes to memory backing the images referenced by the pImageIndices and
            // > pSwapchains members of pPresentInfo, that are available before vkQueuePresentKHR
            // > is executed, are automatically made visible to the read access performed by the
            // > presentation engine.
            0,
            // Transition from: RAR and WAR don't need memory barrier.
            0,
            false,
        },
    },
};
// clang-format on

VkImageCreateFlags GetImageCreateFlags(gl::TextureType textureType)
{
    switch (textureType)
    {
        case gl::TextureType::CubeMap:
            return VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

        case gl::TextureType::_3D:
            return VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;

        default:
            return 0;
    }
}

void HandlePrimitiveRestart(gl::DrawElementsType glIndexType,
                            GLsizei indexCount,
                            const uint8_t *srcPtr,
                            uint8_t *outPtr)
{
    switch (glIndexType)
    {
        case gl::DrawElementsType::UnsignedByte:
            CopyLineLoopIndicesWithRestart<uint8_t, uint16_t>(indexCount, srcPtr, outPtr);
            break;
        case gl::DrawElementsType::UnsignedShort:
            CopyLineLoopIndicesWithRestart<uint16_t, uint16_t>(indexCount, srcPtr, outPtr);
            break;
        case gl::DrawElementsType::UnsignedInt:
            CopyLineLoopIndicesWithRestart<uint32_t, uint32_t>(indexCount, srcPtr, outPtr);
            break;
        default:
            UNREACHABLE();
    }
}

bool HasBothDepthAndStencilAspects(VkImageAspectFlags aspectFlags)
{
    constexpr VkImageAspectFlags kDepthStencilAspects =
        VK_IMAGE_ASPECT_STENCIL_BIT | VK_IMAGE_ASPECT_DEPTH_BIT;
    return (aspectFlags & kDepthStencilAspects) == kDepthStencilAspects;
}

uint32_t GetImageLayerCountForView(const ImageHelper &image)
{
    // Depth > 1 means this is a 3D texture and depth is our layer count
    return image.getExtents().depth > 1 ? image.getExtents().depth : image.getLayerCount();
}

ImageView *GetLevelImageView(ImageViewVector *imageViews, uint32_t level, uint32_t levelCount)
{
    // Lazily allocate the storage for image views. We allocate the full level count because we
    // don't want to trigger any std::vecotr reallocations. Reallocations could invalidate our
    // view pointers.
    if (imageViews->empty())
    {
        imageViews->resize(levelCount);
    }
    ASSERT(imageViews->size() > level);

    return &(*imageViews)[level];
}

// Special rules apply to VkBufferImageCopy with depth/stencil. The components are tightly packed
// into a depth or stencil section of the destination buffer. See the spec:
// https://www.khronos.org/registry/vulkan/specs/1.1-extensions/man/html/VkBufferImageCopy.html
const angle::Format &GetDepthStencilImageToBufferFormat(const angle::Format &imageFormat,
                                                        VkImageAspectFlagBits copyAspect)
{
    if (copyAspect == VK_IMAGE_ASPECT_STENCIL_BIT)
    {
        ASSERT(imageFormat.id == angle::FormatID::D24_UNORM_S8_UINT ||
               imageFormat.id == angle::FormatID::D32_FLOAT_S8X24_UINT ||
               imageFormat.id == angle::FormatID::S8_UINT);
        return angle::Format::Get(angle::FormatID::S8_UINT);
    }

    ASSERT(copyAspect == VK_IMAGE_ASPECT_DEPTH_BIT);

    switch (imageFormat.id)
    {
        case angle::FormatID::D16_UNORM:
            return imageFormat;
        case angle::FormatID::D24_UNORM_X8_UINT:
            return imageFormat;
        case angle::FormatID::D24_UNORM_S8_UINT:
            return angle::Format::Get(angle::FormatID::D24_UNORM_X8_UINT);
        case angle::FormatID::D32_FLOAT:
            return imageFormat;
        case angle::FormatID::D32_FLOAT_S8X24_UINT:
            return angle::Format::Get(angle::FormatID::D32_FLOAT);
        default:
            UNREACHABLE();
            return imageFormat;
    }
}
}  // anonymous namespace

// DynamicBuffer implementation.
DynamicBuffer::DynamicBuffer()
    : mUsage(0),
      mHostVisible(false),
      mInitialSize(0),
      mBuffer(nullptr),
      mNextAllocationOffset(0),
      mLastFlushOrInvalidateOffset(0),
      mSize(0),
      mAlignment(0)
{}

DynamicBuffer::DynamicBuffer(DynamicBuffer &&other)
    : mUsage(other.mUsage),
      mHostVisible(other.mHostVisible),
      mInitialSize(other.mInitialSize),
      mBuffer(other.mBuffer),
      mNextAllocationOffset(other.mNextAllocationOffset),
      mLastFlushOrInvalidateOffset(other.mLastFlushOrInvalidateOffset),
      mSize(other.mSize),
      mAlignment(other.mAlignment),
      mInFlightBuffers(std::move(other.mInFlightBuffers))
{
    other.mBuffer = nullptr;
}

void DynamicBuffer::init(RendererVk *renderer,
                         VkBufferUsageFlags usage,
                         size_t alignment,
                         size_t initialSize,
                         bool hostVisible)
{
    mUsage       = usage;
    mHostVisible = hostVisible;

    // Check that we haven't overriden the initial size of the buffer in setMinimumSizeForTesting.
    if (mInitialSize == 0)
    {
        mInitialSize = initialSize;
        mSize        = 0;
    }

    // Workaround for the mock ICD not supporting allocations greater than 0x1000.
    // Could be removed if https://github.com/KhronosGroup/Vulkan-Tools/issues/84 is fixed.
    if (renderer->isMockICDEnabled())
    {
        mSize = std::min<size_t>(mSize, 0x1000);
    }

    updateAlignment(renderer, alignment);
}

DynamicBuffer::~DynamicBuffer()
{
    ASSERT(mBuffer == nullptr);
}

angle::Result DynamicBuffer::allocateNewBuffer(ContextVk *contextVk)
{
    std::unique_ptr<BufferHelper> buffer = std::make_unique<BufferHelper>();

    VkBufferCreateInfo createInfo    = {};
    createInfo.sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    createInfo.flags                 = 0;
    createInfo.size                  = mSize;
    createInfo.usage                 = mUsage;
    createInfo.sharingMode           = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.queueFamilyIndexCount = 0;
    createInfo.pQueueFamilyIndices   = nullptr;

    const VkMemoryPropertyFlags memoryProperty =
        mHostVisible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    ANGLE_TRY(buffer->init(contextVk, createInfo, memoryProperty));

    ASSERT(!mBuffer);
    mBuffer = buffer.release();

    return angle::Result::Continue;
}

angle::Result DynamicBuffer::allocate(ContextVk *contextVk,
                                      size_t sizeInBytes,
                                      uint8_t **ptrOut,
                                      VkBuffer *bufferOut,
                                      VkDeviceSize *offsetOut,
                                      bool *newBufferAllocatedOut)
{
    size_t sizeToAllocate = roundUp(sizeInBytes, mAlignment);

    angle::base::CheckedNumeric<size_t> checkedNextWriteOffset = mNextAllocationOffset;
    checkedNextWriteOffset += sizeToAllocate;

    if (!checkedNextWriteOffset.IsValid() || checkedNextWriteOffset.ValueOrDie() >= mSize)
    {
        if (mBuffer)
        {
            ANGLE_TRY(flush(contextVk));
            mBuffer->unmap(contextVk->getDevice());

            mInFlightBuffers.push_back(mBuffer);
            mBuffer = nullptr;
        }

        if (sizeToAllocate > mSize)
        {
            mSize = std::max(mInitialSize, sizeToAllocate);

            // Clear the free list since the free buffers are now too small.
            for (BufferHelper *toFree : mBufferFreeList)
            {
                toFree->release(contextVk->getRenderer());
            }
            mBufferFreeList.clear();
        }

        // The front of the free list should be the oldest. Thus if it is in use the rest of the
        // free list should be in use as well.
        if (mBufferFreeList.empty() || mBufferFreeList.front()->isResourceInUse(contextVk))
        {
            ANGLE_TRY(allocateNewBuffer(contextVk));
        }
        else
        {
            mBuffer = mBufferFreeList.front();
            mBufferFreeList.erase(mBufferFreeList.begin());
        }

        ASSERT(mBuffer->getSize() == mSize);

        mNextAllocationOffset        = 0;
        mLastFlushOrInvalidateOffset = 0;

        if (newBufferAllocatedOut != nullptr)
        {
            *newBufferAllocatedOut = true;
        }
    }
    else if (newBufferAllocatedOut != nullptr)
    {
        *newBufferAllocatedOut = false;
    }

    ASSERT(mBuffer != nullptr);

    if (bufferOut != nullptr)
    {
        *bufferOut = mBuffer->getBuffer().getHandle();
    }

    // Optionally map() the buffer if possible
    if (ptrOut)
    {
        ASSERT(mHostVisible);
        uint8_t *mappedMemory;
        ANGLE_TRY(mBuffer->map(contextVk, &mappedMemory));
        *ptrOut = mappedMemory + mNextAllocationOffset;
    }

    *offsetOut = static_cast<VkDeviceSize>(mNextAllocationOffset);
    mNextAllocationOffset += static_cast<uint32_t>(sizeToAllocate);
    return angle::Result::Continue;
}

angle::Result DynamicBuffer::flush(ContextVk *contextVk)
{
    if (mHostVisible && (mNextAllocationOffset > mLastFlushOrInvalidateOffset))
    {
        ASSERT(mBuffer != nullptr);
        ANGLE_TRY(mBuffer->flush(contextVk, mLastFlushOrInvalidateOffset,
                                 mNextAllocationOffset - mLastFlushOrInvalidateOffset));
        mLastFlushOrInvalidateOffset = mNextAllocationOffset;
    }
    return angle::Result::Continue;
}

angle::Result DynamicBuffer::invalidate(ContextVk *contextVk)
{
    if (mHostVisible && (mNextAllocationOffset > mLastFlushOrInvalidateOffset))
    {
        ASSERT(mBuffer != nullptr);
        ANGLE_TRY(mBuffer->invalidate(contextVk, mLastFlushOrInvalidateOffset,
                                      mNextAllocationOffset - mLastFlushOrInvalidateOffset));
        mLastFlushOrInvalidateOffset = mNextAllocationOffset;
    }
    return angle::Result::Continue;
}

void DynamicBuffer::releaseBufferListToRenderer(RendererVk *renderer,
                                                std::vector<BufferHelper *> *buffers)
{
    for (BufferHelper *toFree : *buffers)
    {
        toFree->release(renderer);
        delete toFree;
    }

    buffers->clear();
}

void DynamicBuffer::destroyBufferList(VkDevice device, std::vector<BufferHelper *> *buffers)
{
    for (BufferHelper *toFree : *buffers)
    {
        toFree->destroy(device);
        delete toFree;
    }

    buffers->clear();
}

void DynamicBuffer::release(RendererVk *renderer)
{
    reset();

    releaseBufferListToRenderer(renderer, &mInFlightBuffers);
    releaseBufferListToRenderer(renderer, &mBufferFreeList);

    if (mBuffer)
    {
        mBuffer->release(renderer);
        SafeDelete(mBuffer);
    }
}

void DynamicBuffer::releaseInFlightBuffers(ContextVk *contextVk)
{
    for (BufferHelper *toRelease : mInFlightBuffers)
    {
        // If the dynamic buffer was resized we cannot reuse the retained buffer.
        if (toRelease->getSize() < mSize)
        {
            toRelease->release(contextVk->getRenderer());
        }
        else
        {
            mBufferFreeList.push_back(toRelease);
        }
    }

    mInFlightBuffers.clear();
}

void DynamicBuffer::destroy(VkDevice device)
{
    reset();

    destroyBufferList(device, &mInFlightBuffers);
    destroyBufferList(device, &mBufferFreeList);

    if (mBuffer)
    {
        mBuffer->unmap(device);
        mBuffer->destroy(device);
        delete mBuffer;
        mBuffer = nullptr;
    }
}

void DynamicBuffer::updateAlignment(RendererVk *renderer, size_t alignment)
{
    ASSERT(alignment > 0);

    size_t atomSize =
        static_cast<size_t>(renderer->getPhysicalDeviceProperties().limits.nonCoherentAtomSize);

    // We need lcm(alignment, atomSize).  Usually, one divides the other so std::max() could be used
    // instead.  Only known case where this assumption breaks is for 3-component types with 16- or
    // 32-bit channels, so that's special-cased to avoid a full-fledged lcm implementation.

    if (gl::isPow2(alignment))
    {
        ASSERT(alignment % atomSize == 0 || atomSize % alignment == 0);
        ASSERT(gl::isPow2(atomSize));

        alignment = std::max(alignment, atomSize);
    }
    else
    {
        ASSERT(gl::isPow2(atomSize));
        ASSERT(alignment % 3 == 0);
        ASSERT(gl::isPow2(alignment / 3));

        alignment = std::max(alignment / 3, atomSize) * 3;
    }

    // If alignment has changed, make sure the next allocation is done at an aligned offset.
    if (alignment != mAlignment)
    {
        mNextAllocationOffset = roundUp(mNextAllocationOffset, static_cast<uint32_t>(alignment));
    }

    mAlignment = alignment;
}

void DynamicBuffer::setMinimumSizeForTesting(size_t minSize)
{
    // This will really only have an effect next time we call allocate.
    mInitialSize = minSize;

    // Forces a new allocation on the next allocate.
    mSize = 0;
}

void DynamicBuffer::reset()
{
    mSize                        = 0;
    mNextAllocationOffset        = 0;
    mLastFlushOrInvalidateOffset = 0;
}

// DescriptorPoolHelper implementation.
DescriptorPoolHelper::DescriptorPoolHelper() : mFreeDescriptorSets(0) {}

DescriptorPoolHelper::~DescriptorPoolHelper() = default;

bool DescriptorPoolHelper::hasCapacity(uint32_t descriptorSetCount) const
{
    return mFreeDescriptorSets >= descriptorSetCount;
}

angle::Result DescriptorPoolHelper::init(Context *context,
                                         const std::vector<VkDescriptorPoolSize> &poolSizes,
                                         uint32_t maxSets)
{
    if (mDescriptorPool.valid())
    {
        // This could be improved by recycling the descriptor pool.
        mDescriptorPool.destroy(context->getDevice());
    }

    VkDescriptorPoolCreateInfo descriptorPoolInfo = {};
    descriptorPoolInfo.sType                      = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptorPoolInfo.flags                      = 0;
    descriptorPoolInfo.maxSets                    = maxSets;
    descriptorPoolInfo.poolSizeCount              = static_cast<uint32_t>(poolSizes.size());
    descriptorPoolInfo.pPoolSizes                 = poolSizes.data();

    mFreeDescriptorSets = maxSets;

    ANGLE_VK_TRY(context, mDescriptorPool.init(context->getDevice(), descriptorPoolInfo));
    return angle::Result::Continue;
}

void DescriptorPoolHelper::destroy(VkDevice device)
{
    mDescriptorPool.destroy(device);
}

void DescriptorPoolHelper::release(ContextVk *contextVk)
{
    contextVk->addGarbage(&mDescriptorPool);
}

angle::Result DescriptorPoolHelper::allocateSets(ContextVk *contextVk,
                                                 const VkDescriptorSetLayout *descriptorSetLayout,
                                                 uint32_t descriptorSetCount,
                                                 VkDescriptorSet *descriptorSetsOut)
{
    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType                       = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool              = mDescriptorPool.getHandle();
    allocInfo.descriptorSetCount          = descriptorSetCount;
    allocInfo.pSetLayouts                 = descriptorSetLayout;

    ASSERT(mFreeDescriptorSets >= descriptorSetCount);
    mFreeDescriptorSets -= descriptorSetCount;

    ANGLE_VK_TRY(contextVk, mDescriptorPool.allocateDescriptorSets(contextVk->getDevice(),
                                                                   allocInfo, descriptorSetsOut));
    return angle::Result::Continue;
}

// DynamicDescriptorPool implementation.
DynamicDescriptorPool::DynamicDescriptorPool()
    : mMaxSetsPerPool(kDefaultDescriptorPoolMaxSets), mCurrentPoolIndex(0)
{}

DynamicDescriptorPool::~DynamicDescriptorPool() = default;

angle::Result DynamicDescriptorPool::init(ContextVk *contextVk,
                                          const VkDescriptorPoolSize *setSizes,
                                          uint32_t setSizeCount)
{
    ASSERT(mCurrentPoolIndex == 0);
    ASSERT(mDescriptorPools.empty() || (mDescriptorPools.size() == 1 &&
                                        mDescriptorPools[0]->get().hasCapacity(mMaxSetsPerPool)));

    mPoolSizes.assign(setSizes, setSizes + setSizeCount);
    for (uint32_t i = 0; i < setSizeCount; ++i)
    {
        mPoolSizes[i].descriptorCount *= mMaxSetsPerPool;
    }

    mDescriptorPools.push_back(new RefCountedDescriptorPoolHelper());
    return mDescriptorPools[0]->get().init(contextVk, mPoolSizes, mMaxSetsPerPool);
}

void DynamicDescriptorPool::destroy(VkDevice device)
{
    for (RefCountedDescriptorPoolHelper *pool : mDescriptorPools)
    {
        ASSERT(!pool->isReferenced());
        pool->get().destroy(device);
        delete pool;
    }

    mDescriptorPools.clear();
}

void DynamicDescriptorPool::release(ContextVk *contextVk)
{
    for (RefCountedDescriptorPoolHelper *pool : mDescriptorPools)
    {
        ASSERT(!pool->isReferenced());
        pool->get().release(contextVk);
        delete pool;
    }

    mDescriptorPools.clear();
}

angle::Result DynamicDescriptorPool::allocateSetsAndGetInfo(
    ContextVk *contextVk,
    const VkDescriptorSetLayout *descriptorSetLayout,
    uint32_t descriptorSetCount,
    RefCountedDescriptorPoolBinding *bindingOut,
    VkDescriptorSet *descriptorSetsOut,
    bool *newPoolAllocatedOut)
{
    *newPoolAllocatedOut = false;

    if (!bindingOut->valid() || !bindingOut->get().hasCapacity(descriptorSetCount))
    {
        if (!mDescriptorPools[mCurrentPoolIndex]->get().hasCapacity(descriptorSetCount))
        {
            ANGLE_TRY(allocateNewPool(contextVk));
            *newPoolAllocatedOut = true;
        }

        // Make sure the old binding knows the descriptor sets can still be in-use. We only need
        // to update the serial when we move to a new pool. This is because we only check serials
        // when we move to a new pool.
        if (bindingOut->valid())
        {
            Serial currentSerial = contextVk->getCurrentQueueSerial();
            bindingOut->get().updateSerial(currentSerial);
        }

        bindingOut->set(mDescriptorPools[mCurrentPoolIndex]);
    }

    return bindingOut->get().allocateSets(contextVk, descriptorSetLayout, descriptorSetCount,
                                          descriptorSetsOut);
}

angle::Result DynamicDescriptorPool::allocateNewPool(ContextVk *contextVk)
{
    bool found = false;

    for (size_t poolIndex = 0; poolIndex < mDescriptorPools.size(); ++poolIndex)
    {
        if (!mDescriptorPools[poolIndex]->isReferenced() &&
            !contextVk->isSerialInUse(mDescriptorPools[poolIndex]->get().getSerial()))
        {
            mCurrentPoolIndex = poolIndex;
            found             = true;
            break;
        }
    }

    if (!found)
    {
        mDescriptorPools.push_back(new RefCountedDescriptorPoolHelper());
        mCurrentPoolIndex = mDescriptorPools.size() - 1;

        static constexpr size_t kMaxPools = 99999;
        ANGLE_VK_CHECK(contextVk, mDescriptorPools.size() < kMaxPools, VK_ERROR_TOO_MANY_OBJECTS);
    }

    return mDescriptorPools[mCurrentPoolIndex]->get().init(contextVk, mPoolSizes, mMaxSetsPerPool);
}

void DynamicDescriptorPool::setMaxSetsPerPoolForTesting(uint32_t maxSetsPerPool)
{
    mMaxSetsPerPool = maxSetsPerPool;
}

// DynamicallyGrowingPool implementation
template <typename Pool>
DynamicallyGrowingPool<Pool>::DynamicallyGrowingPool()
    : mPoolSize(0), mCurrentPool(0), mCurrentFreeEntry(0)
{}

template <typename Pool>
DynamicallyGrowingPool<Pool>::~DynamicallyGrowingPool() = default;

template <typename Pool>
angle::Result DynamicallyGrowingPool<Pool>::initEntryPool(Context *contextVk, uint32_t poolSize)
{
    ASSERT(mPools.empty() && mPoolStats.empty());
    mPoolSize = poolSize;
    return angle::Result::Continue;
}

template <typename Pool>
void DynamicallyGrowingPool<Pool>::destroyEntryPool()
{
    mPools.clear();
    mPoolStats.clear();
}

template <typename Pool>
bool DynamicallyGrowingPool<Pool>::findFreeEntryPool(ContextVk *contextVk)
{
    Serial lastCompletedQueueSerial = contextVk->getLastCompletedQueueSerial();
    for (size_t i = 0; i < mPools.size(); ++i)
    {
        if (mPoolStats[i].freedCount == mPoolSize &&
            mPoolStats[i].serial <= lastCompletedQueueSerial)
        {
            mCurrentPool      = i;
            mCurrentFreeEntry = 0;

            mPoolStats[i].freedCount = 0;

            return true;
        }
    }

    return false;
}

template <typename Pool>
angle::Result DynamicallyGrowingPool<Pool>::allocateNewEntryPool(ContextVk *contextVk, Pool &&pool)
{
    mPools.push_back(std::move(pool));

    PoolStats poolStats = {0, Serial()};
    mPoolStats.push_back(poolStats);

    mCurrentPool      = mPools.size() - 1;
    mCurrentFreeEntry = 0;

    return angle::Result::Continue;
}

template <typename Pool>
void DynamicallyGrowingPool<Pool>::onEntryFreed(ContextVk *contextVk, size_t poolIndex)
{
    ASSERT(poolIndex < mPoolStats.size() && mPoolStats[poolIndex].freedCount < mPoolSize);

    // Take note of the current serial to avoid reallocating a query in the same pool
    mPoolStats[poolIndex].serial = contextVk->getCurrentQueueSerial();
    ++mPoolStats[poolIndex].freedCount;
}

// DynamicQueryPool implementation
DynamicQueryPool::DynamicQueryPool() = default;

DynamicQueryPool::~DynamicQueryPool() = default;

angle::Result DynamicQueryPool::init(ContextVk *contextVk, VkQueryType type, uint32_t poolSize)
{
    ANGLE_TRY(initEntryPool(contextVk, poolSize));

    mQueryType = type;
    ANGLE_TRY(allocateNewPool(contextVk));

    return angle::Result::Continue;
}

void DynamicQueryPool::destroy(VkDevice device)
{
    for (QueryPool &queryPool : mPools)
    {
        queryPool.destroy(device);
    }

    destroyEntryPool();
}

angle::Result DynamicQueryPool::allocateQuery(ContextVk *contextVk, QueryHelper *queryOut)
{
    ASSERT(!queryOut->getQueryPool());

    size_t poolIndex    = 0;
    uint32_t queryIndex = 0;
    ANGLE_TRY(allocateQuery(contextVk, &poolIndex, &queryIndex));

    queryOut->init(this, poolIndex, queryIndex);

    return angle::Result::Continue;
}

void DynamicQueryPool::freeQuery(ContextVk *contextVk, QueryHelper *query)
{
    if (query->getQueryPool())
    {
        size_t poolIndex = query->getQueryPoolIndex();
        ASSERT(query->getQueryPool()->valid());

        freeQuery(contextVk, poolIndex, query->getQuery());

        query->deinit();
    }
}

angle::Result DynamicQueryPool::allocateQuery(ContextVk *contextVk,
                                              size_t *poolIndex,
                                              uint32_t *queryIndex)
{
    if (mCurrentFreeEntry >= mPoolSize)
    {
        // No more queries left in this pool, create another one.
        ANGLE_TRY(allocateNewPool(contextVk));
    }

    *poolIndex  = mCurrentPool;
    *queryIndex = mCurrentFreeEntry++;

    return angle::Result::Continue;
}

void DynamicQueryPool::freeQuery(ContextVk *contextVk, size_t poolIndex, uint32_t queryIndex)
{
    ANGLE_UNUSED_VARIABLE(queryIndex);
    onEntryFreed(contextVk, poolIndex);
}

angle::Result DynamicQueryPool::allocateNewPool(ContextVk *contextVk)
{
    if (findFreeEntryPool(contextVk))
    {
        return angle::Result::Continue;
    }

    VkQueryPoolCreateInfo queryPoolInfo = {};
    queryPoolInfo.sType                 = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    queryPoolInfo.flags                 = 0;
    queryPoolInfo.queryType             = mQueryType;
    queryPoolInfo.queryCount            = mPoolSize;
    queryPoolInfo.pipelineStatistics    = 0;

    QueryPool queryPool;

    ANGLE_VK_TRY(contextVk, queryPool.init(contextVk->getDevice(), queryPoolInfo));

    return allocateNewEntryPool(contextVk, std::move(queryPool));
}

// QueryHelper implementation
QueryHelper::QueryHelper() : mDynamicQueryPool(nullptr), mQueryPoolIndex(0), mQuery(0) {}

QueryHelper::~QueryHelper() {}

void QueryHelper::init(const DynamicQueryPool *dynamicQueryPool,
                       const size_t queryPoolIndex,
                       uint32_t query)
{
    mDynamicQueryPool = dynamicQueryPool;
    mQueryPoolIndex   = queryPoolIndex;
    mQuery            = query;
}

void QueryHelper::deinit()
{
    mDynamicQueryPool = nullptr;
    mQueryPoolIndex   = 0;
    mQuery            = 0;
}

void QueryHelper::beginQuery(ContextVk *contextVk)
{
    contextVk->getCommandGraph()->beginQuery(getQueryPool(), getQuery());
    mMostRecentSerial = contextVk->getCurrentQueueSerial();
}

void QueryHelper::endQuery(ContextVk *contextVk)
{
    contextVk->getCommandGraph()->endQuery(getQueryPool(), getQuery());
    mMostRecentSerial = contextVk->getCurrentQueueSerial();
}

void QueryHelper::writeTimestamp(ContextVk *contextVk)
{
    contextVk->getCommandGraph()->writeTimestamp(getQueryPool(), getQuery());
    mMostRecentSerial = contextVk->getCurrentQueueSerial();
}

bool QueryHelper::hasPendingWork(ContextVk *contextVk)
{
    // If the renderer has a queue serial higher than the stored one, the command buffers that
    // recorded this query have already been submitted, so there is no pending work.
    return mMostRecentSerial == contextVk->getCurrentQueueSerial();
}

// DynamicSemaphorePool implementation
DynamicSemaphorePool::DynamicSemaphorePool() = default;

DynamicSemaphorePool::~DynamicSemaphorePool() = default;

angle::Result DynamicSemaphorePool::init(ContextVk *contextVk, uint32_t poolSize)
{
    ANGLE_TRY(initEntryPool(contextVk, poolSize));
    ANGLE_TRY(allocateNewPool(contextVk));
    return angle::Result::Continue;
}

void DynamicSemaphorePool::destroy(VkDevice device)
{
    for (auto &semaphorePool : mPools)
    {
        for (Semaphore &semaphore : semaphorePool)
        {
            semaphore.destroy(device);
        }
    }

    destroyEntryPool();
}

angle::Result DynamicSemaphorePool::allocateSemaphore(ContextVk *contextVk,
                                                      SemaphoreHelper *semaphoreOut)
{
    ASSERT(!semaphoreOut->getSemaphore());

    if (mCurrentFreeEntry >= mPoolSize)
    {
        // No more queries left in this pool, create another one.
        ANGLE_TRY(allocateNewPool(contextVk));
    }

    semaphoreOut->init(mCurrentPool, &mPools[mCurrentPool][mCurrentFreeEntry++]);

    return angle::Result::Continue;
}

void DynamicSemaphorePool::freeSemaphore(ContextVk *contextVk, SemaphoreHelper *semaphore)
{
    if (semaphore->getSemaphore())
    {
        onEntryFreed(contextVk, semaphore->getSemaphorePoolIndex());
        semaphore->deinit();
    }
}

angle::Result DynamicSemaphorePool::allocateNewPool(ContextVk *contextVk)
{
    if (findFreeEntryPool(contextVk))
    {
        return angle::Result::Continue;
    }

    std::vector<Semaphore> newPool(mPoolSize);

    for (Semaphore &semaphore : newPool)
    {
        ANGLE_VK_TRY(contextVk, semaphore.init(contextVk->getDevice()));
    }

    // This code is safe as long as the growth of the outer vector in vector<vector<T>> is done by
    // moving the inner vectors, making sure references to the inner vector remain intact.
    Semaphore *assertMove = mPools.size() > 0 ? mPools[0].data() : nullptr;

    ANGLE_TRY(allocateNewEntryPool(contextVk, std::move(newPool)));

    ASSERT(assertMove == nullptr || assertMove == mPools[0].data());

    return angle::Result::Continue;
}

// SemaphoreHelper implementation
SemaphoreHelper::SemaphoreHelper() : mSemaphorePoolIndex(0), mSemaphore(0) {}

SemaphoreHelper::~SemaphoreHelper() {}

SemaphoreHelper::SemaphoreHelper(SemaphoreHelper &&other)
    : mSemaphorePoolIndex(other.mSemaphorePoolIndex), mSemaphore(other.mSemaphore)
{
    other.mSemaphore = nullptr;
}

SemaphoreHelper &SemaphoreHelper::operator=(SemaphoreHelper &&other)
{
    std::swap(mSemaphorePoolIndex, other.mSemaphorePoolIndex);
    std::swap(mSemaphore, other.mSemaphore);
    return *this;
}

void SemaphoreHelper::init(const size_t semaphorePoolIndex, const Semaphore *semaphore)
{
    mSemaphorePoolIndex = semaphorePoolIndex;
    mSemaphore          = semaphore;
}

void SemaphoreHelper::deinit()
{
    mSemaphorePoolIndex = 0;
    mSemaphore          = nullptr;
}

// LineLoopHelper implementation.
LineLoopHelper::LineLoopHelper(RendererVk *renderer)
{
    // We need to use an alignment of the maximum size we're going to allocate, which is
    // VK_INDEX_TYPE_UINT32. When we switch from a drawElement to a drawArray call, the allocations
    // can vary in size. According to the Vulkan spec, when calling vkCmdBindIndexBuffer: 'The
    // sum of offset and the address of the range of VkDeviceMemory object that is backing buffer,
    // must be a multiple of the type indicated by indexType'.
    mDynamicIndexBuffer.init(renderer, kLineLoopDynamicBufferUsage, sizeof(uint32_t),
                             kLineLoopDynamicBufferInitialSize, true);
    mDynamicIndirectBuffer.init(renderer, kLineLoopDynamicIndirectBufferUsage, sizeof(uint32_t),
                                kLineLoopDynamicIndirectBufferInitialSize, true);
}

LineLoopHelper::~LineLoopHelper() = default;

angle::Result LineLoopHelper::getIndexBufferForDrawArrays(ContextVk *contextVk,
                                                          uint32_t clampedVertexCount,
                                                          GLint firstVertex,
                                                          BufferHelper **bufferOut,
                                                          VkDeviceSize *offsetOut)
{
    uint32_t *indices    = nullptr;
    size_t allocateBytes = sizeof(uint32_t) * (static_cast<size_t>(clampedVertexCount) + 1);

    mDynamicIndexBuffer.releaseInFlightBuffers(contextVk);
    ANGLE_TRY(mDynamicIndexBuffer.allocate(contextVk, allocateBytes,
                                           reinterpret_cast<uint8_t **>(&indices), nullptr,
                                           offsetOut, nullptr));
    *bufferOut = mDynamicIndexBuffer.getCurrentBuffer();

    // Note: there could be an overflow in this addition.
    uint32_t unsignedFirstVertex = static_cast<uint32_t>(firstVertex);
    uint32_t vertexCount         = (clampedVertexCount + unsignedFirstVertex);
    for (uint32_t vertexIndex = unsignedFirstVertex; vertexIndex < vertexCount; vertexIndex++)
    {
        *indices++ = vertexIndex;
    }
    *indices = unsignedFirstVertex;

    // Since we are not using the VK_MEMORY_PROPERTY_HOST_COHERENT_BIT flag when creating the
    // device memory in the StreamingBuffer, we always need to make sure we flush it after
    // writing.
    ANGLE_TRY(mDynamicIndexBuffer.flush(contextVk));

    return angle::Result::Continue;
}

angle::Result LineLoopHelper::getIndexBufferForElementArrayBuffer(ContextVk *contextVk,
                                                                  BufferVk *elementArrayBufferVk,
                                                                  gl::DrawElementsType glIndexType,
                                                                  int indexCount,
                                                                  intptr_t elementArrayOffset,
                                                                  BufferHelper **bufferOut,
                                                                  VkDeviceSize *bufferOffsetOut,
                                                                  uint32_t *indexCountOut)
{
    if (glIndexType == gl::DrawElementsType::UnsignedByte ||
        contextVk->getState().isPrimitiveRestartEnabled())
    {
        ANGLE_TRACE_EVENT0("gpu.angle", "LineLoopHelper::getIndexBufferForElementArrayBuffer");

        void *srcDataMapping = nullptr;
        ANGLE_TRY(elementArrayBufferVk->mapImpl(contextVk, &srcDataMapping));
        ANGLE_TRY(streamIndices(contextVk, glIndexType, indexCount,
                                static_cast<const uint8_t *>(srcDataMapping) + elementArrayOffset,
                                bufferOut, bufferOffsetOut, indexCountOut));
        elementArrayBufferVk->unmapImpl(contextVk);
        return angle::Result::Continue;
    }

    *indexCountOut = indexCount + 1;

    VkIndexType indexType = gl_vk::kIndexTypeMap[glIndexType];
    ASSERT(indexType == VK_INDEX_TYPE_UINT16 || indexType == VK_INDEX_TYPE_UINT32);
    uint32_t *indices = nullptr;

    auto unitSize = (indexType == VK_INDEX_TYPE_UINT16 ? sizeof(uint16_t) : sizeof(uint32_t));
    size_t allocateBytes = unitSize * (indexCount + 1) + 1;

    mDynamicIndexBuffer.releaseInFlightBuffers(contextVk);
    ANGLE_TRY(mDynamicIndexBuffer.allocate(contextVk, allocateBytes,
                                           reinterpret_cast<uint8_t **>(&indices), nullptr,
                                           bufferOffsetOut, nullptr));
    *bufferOut = mDynamicIndexBuffer.getCurrentBuffer();

    VkDeviceSize sourceOffset                  = static_cast<VkDeviceSize>(elementArrayOffset);
    uint64_t unitCount                         = static_cast<VkDeviceSize>(indexCount);
    angle::FixedVector<VkBufferCopy, 3> copies = {
        {sourceOffset, *bufferOffsetOut, unitCount * unitSize},
        {sourceOffset, *bufferOffsetOut + unitCount * unitSize, unitSize},
    };
    if (contextVk->getRenderer()->getFeatures().extraCopyBufferRegion.enabled)
        copies.push_back({sourceOffset, *bufferOffsetOut + (unitCount + 1) * unitSize, 1});

    ANGLE_TRY(elementArrayBufferVk->copyToBuffer(
        contextVk, *bufferOut, static_cast<uint32_t>(copies.size()), copies.data()));
    ANGLE_TRY(mDynamicIndexBuffer.flush(contextVk));
    return angle::Result::Continue;
}

angle::Result LineLoopHelper::streamIndices(ContextVk *contextVk,
                                            gl::DrawElementsType glIndexType,
                                            GLsizei indexCount,
                                            const uint8_t *srcPtr,
                                            BufferHelper **bufferOut,
                                            VkDeviceSize *bufferOffsetOut,
                                            uint32_t *indexCountOut)
{
    VkIndexType indexType = gl_vk::kIndexTypeMap[glIndexType];

    uint8_t *indices = nullptr;

    auto unitSize = (indexType == VK_INDEX_TYPE_UINT16 ? sizeof(uint16_t) : sizeof(uint32_t));
    uint32_t numOutIndices = indexCount + 1;
    if (contextVk->getState().isPrimitiveRestartEnabled())
    {
        numOutIndices = GetLineLoopWithRestartIndexCount(glIndexType, indexCount, srcPtr);
    }
    *indexCountOut       = numOutIndices;
    size_t allocateBytes = unitSize * numOutIndices;
    ANGLE_TRY(mDynamicIndexBuffer.allocate(contextVk, allocateBytes,
                                           reinterpret_cast<uint8_t **>(&indices), nullptr,
                                           bufferOffsetOut, nullptr));
    *bufferOut = mDynamicIndexBuffer.getCurrentBuffer();

    if (contextVk->getState().isPrimitiveRestartEnabled())
    {
        HandlePrimitiveRestart(glIndexType, indexCount, srcPtr, indices);
    }
    else
    {
        if (glIndexType == gl::DrawElementsType::UnsignedByte)
        {
            // Vulkan doesn't support uint8 index types, so we need to emulate it.
            ASSERT(indexType == VK_INDEX_TYPE_UINT16);
            uint16_t *indicesDst = reinterpret_cast<uint16_t *>(indices);
            for (int i = 0; i < indexCount; i++)
            {
                indicesDst[i] = srcPtr[i];
            }

            indicesDst[indexCount] = srcPtr[0];
        }
        else
        {
            memcpy(indices, srcPtr, unitSize * indexCount);
            memcpy(indices + unitSize * indexCount, srcPtr, unitSize);
        }
    }

    ANGLE_TRY(mDynamicIndexBuffer.flush(contextVk));
    return angle::Result::Continue;
}

angle::Result LineLoopHelper::streamIndicesIndirect(ContextVk *contextVk,
                                                    gl::DrawElementsType glIndexType,
                                                    BufferHelper *indexBuffer,
                                                    BufferHelper *indirectBuffer,
                                                    VkDeviceSize indirectBufferOffset,
                                                    BufferHelper **indexBufferOut,
                                                    VkDeviceSize *indexBufferOffsetOut,
                                                    BufferHelper **indirectBufferOut,
                                                    VkDeviceSize *indirectBufferOffsetOut)
{
    VkIndexType indexType = gl_vk::kIndexTypeMap[glIndexType];

    auto unitSize = (indexType == VK_INDEX_TYPE_UINT16 ? sizeof(uint16_t) : sizeof(uint32_t));
    size_t allocateBytes = static_cast<size_t>(indexBuffer->getSize() + unitSize);

    if (contextVk->getState().isPrimitiveRestartEnabled())
    {
        // If primitive restart, new index buffer is 135% the size of the original index buffer. The
        // smallest lineloop with primitive restart is 3 indices (point 1, point 2 and restart
        // value) when converted to linelist becomes 4 vertices. Expansion of 4/3. Any larger
        // lineloops would have less overhead and require less extra space. Any incomplete
        // primitives can be dropped or left incomplete and thus not increase the size of the
        // destination index buffer. Since we don't know the number of indices being used we'll use
        // the size of the index buffer as allocated as the index count.
        size_t numInputIndices    = static_cast<size_t>(indexBuffer->getSize() / unitSize);
        size_t numNewInputIndices = ((numInputIndices * 4) / 3) + 1;
        allocateBytes             = static_cast<size_t>(numNewInputIndices * unitSize);
    }

    mDynamicIndexBuffer.releaseInFlightBuffers(contextVk);
    mDynamicIndirectBuffer.releaseInFlightBuffers(contextVk);

    ANGLE_TRY(mDynamicIndexBuffer.allocate(contextVk, allocateBytes, nullptr, nullptr,
                                           indexBufferOffsetOut, nullptr));
    *indexBufferOut = mDynamicIndexBuffer.getCurrentBuffer();

    ANGLE_TRY(mDynamicIndirectBuffer.allocate(contextVk, sizeof(VkDrawIndexedIndirectCommand),
                                              nullptr, nullptr, indirectBufferOffsetOut, nullptr));
    *indirectBufferOut = mDynamicIndirectBuffer.getCurrentBuffer();

    BufferHelper *destIndexBuffer    = mDynamicIndexBuffer.getCurrentBuffer();
    BufferHelper *destIndirectBuffer = mDynamicIndirectBuffer.getCurrentBuffer();

    // Copy relevant section of the source into destination at allocated offset.  Note that the
    // offset returned by allocate() above is in bytes. As is the indices offset pointer.
    UtilsVk::ConvertLineLoopIndexIndirectParameters params = {};
    params.indirectBufferOffset    = static_cast<uint32_t>(indirectBufferOffset);
    params.dstIndirectBufferOffset = static_cast<uint32_t>(*indirectBufferOffsetOut);
    params.dstIndexBufferOffset    = static_cast<uint32_t>(*indexBufferOffsetOut);
    params.is32Bit                 = unitSize == 4;

    ANGLE_TRY(contextVk->getUtils().convertLineLoopIndexIndirectBuffer(
        contextVk, indirectBuffer, destIndirectBuffer, destIndexBuffer, indexBuffer, params));

    return angle::Result::Continue;
}

angle::Result LineLoopHelper::streamArrayIndirect(ContextVk *contextVk,
                                                  size_t vertexCount,
                                                  BufferHelper *arrayIndirectBuffer,
                                                  VkDeviceSize arrayIndirectBufferOffset,
                                                  BufferHelper **indexBufferOut,
                                                  VkDeviceSize *indexBufferOffsetOut,
                                                  BufferHelper **indexIndirectBufferOut,
                                                  VkDeviceSize *indexIndirectBufferOffsetOut)
{
    auto unitSize        = sizeof(uint32_t);
    size_t allocateBytes = static_cast<size_t>((vertexCount + 1) * unitSize);

    mDynamicIndexBuffer.releaseInFlightBuffers(contextVk);
    mDynamicIndirectBuffer.releaseInFlightBuffers(contextVk);

    ANGLE_TRY(mDynamicIndexBuffer.allocate(contextVk, allocateBytes, nullptr, nullptr,
                                           indexBufferOffsetOut, nullptr));
    *indexBufferOut = mDynamicIndexBuffer.getCurrentBuffer();

    ANGLE_TRY(mDynamicIndirectBuffer.allocate(contextVk, sizeof(VkDrawIndexedIndirectCommand),
                                              nullptr, nullptr, indexIndirectBufferOffsetOut,
                                              nullptr));
    *indexIndirectBufferOut = mDynamicIndirectBuffer.getCurrentBuffer();

    BufferHelper *destIndexBuffer    = mDynamicIndexBuffer.getCurrentBuffer();
    BufferHelper *destIndirectBuffer = mDynamicIndirectBuffer.getCurrentBuffer();

    // Copy relevant section of the source into destination at allocated offset.  Note that the
    // offset returned by allocate() above is in bytes. As is the indices offset pointer.
    UtilsVk::ConvertLineLoopArrayIndirectParameters params = {};
    params.indirectBufferOffset    = static_cast<uint32_t>(arrayIndirectBufferOffset);
    params.dstIndirectBufferOffset = static_cast<uint32_t>(*indexIndirectBufferOffsetOut);
    params.dstIndexBufferOffset    = static_cast<uint32_t>(*indexBufferOffsetOut);

    ANGLE_TRY(contextVk->getUtils().convertLineLoopArrayIndirectBuffer(
        contextVk, arrayIndirectBuffer, destIndirectBuffer, destIndexBuffer, params));

    return angle::Result::Continue;
}

void LineLoopHelper::release(ContextVk *contextVk)
{
    mDynamicIndexBuffer.release(contextVk->getRenderer());
    mDynamicIndirectBuffer.release(contextVk->getRenderer());
}

void LineLoopHelper::destroy(VkDevice device)
{
    mDynamicIndexBuffer.destroy(device);
    mDynamicIndirectBuffer.destroy(device);
}

// static
void LineLoopHelper::Draw(uint32_t count, CommandBuffer *commandBuffer)
{
    // Our first index is always 0 because that's how we set it up in createIndexBuffer*.
    commandBuffer->drawIndexed(count);
}

// BufferHelper implementation.
BufferHelper::BufferHelper()
    : CommandGraphResource(CommandGraphResourceType::Buffer),
      mMemoryPropertyFlags{},
      mSize(0),
      mMappedMemory(nullptr),
      mViewFormat(nullptr),
      mCurrentWriteAccess(0),
      mCurrentReadAccess(0)
{}

BufferHelper::~BufferHelper() = default;

angle::Result BufferHelper::init(ContextVk *contextVk,
                                 const VkBufferCreateInfo &createInfo,
                                 VkMemoryPropertyFlags memoryPropertyFlags)
{
    // TODO: Remove with anglebug.com/2162: Vulkan: Implement device memory sub-allocation
    // Check if we have too many resources allocated already and need to free some before allocating
    // more and (possibly) exceeding the device's limits.
    if (contextVk->shouldFlush())
    {
        ANGLE_TRY(contextVk->flushImpl(nullptr));
    }

    mSize = createInfo.size;
    ANGLE_VK_TRY(contextVk, mBuffer.init(contextVk->getDevice(), createInfo));
    return AllocateBufferMemory(contextVk, memoryPropertyFlags, &mMemoryPropertyFlags, nullptr,
                                &mBuffer, &mDeviceMemory);
}

void BufferHelper::destroy(VkDevice device)
{
    unmap(device);
    mSize       = 0;
    mViewFormat = nullptr;

    mBuffer.destroy(device);
    mBufferView.destroy(device);
    mDeviceMemory.destroy(device);
}

void BufferHelper::release(RendererVk *renderer)
{
    unmap(renderer->getDevice());
    mSize       = 0;
    mViewFormat = nullptr;

    renderer->collectGarbageAndReinit(&mUse, &mBuffer, &mBufferView, &mDeviceMemory);
}

bool BufferHelper::needsOnWriteBarrier(VkAccessFlags readAccessType,
                                       VkAccessFlags writeAccessType,
                                       VkAccessFlags *barrierSrcOut,
                                       VkAccessFlags *barrierDstOut)
{
    bool needsBarrier = mCurrentReadAccess != 0 || mCurrentWriteAccess != 0;

    // Note: mCurrentReadAccess is not part of barrier src flags as "anything-after-read" is
    // satisified by execution barriers alone.
    *barrierSrcOut = mCurrentWriteAccess;
    *barrierDstOut = readAccessType | writeAccessType;

    mCurrentWriteAccess = writeAccessType;
    mCurrentReadAccess  = readAccessType;

    return needsBarrier;
}

void BufferHelper::onWriteAccess(ContextVk *contextVk,
                                 VkAccessFlags readAccessType,
                                 VkAccessFlags writeAccessType)
{
    VkAccessFlags barrierSrc, barrierDst;
    if (needsOnWriteBarrier(readAccessType, writeAccessType, &barrierSrc, &barrierDst))
    {
        addGlobalMemoryBarrier(barrierSrc, barrierDst, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    }

    bool hostVisible = mMemoryPropertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    if (hostVisible && writeAccessType != VK_ACCESS_HOST_WRITE_BIT)
    {
        contextVk->onHostVisibleBufferWrite();
    }
}

angle::Result BufferHelper::copyFromBuffer(ContextVk *contextVk,
                                           const Buffer &buffer,
                                           VkAccessFlags bufferAccessType,
                                           const VkBufferCopy &copyRegion)
{
    // 'recordCommands' will implicitly stop any reads from using the old buffer data.
    CommandBuffer *commandBuffer = nullptr;
    ANGLE_TRY(recordCommands(contextVk, &commandBuffer));

    if (mCurrentReadAccess != 0 || mCurrentWriteAccess != 0 || bufferAccessType != 0)
    {
        // Insert a barrier to ensure reads/writes are complete.
        // Use a global memory barrier to keep things simple.
        VkMemoryBarrier memoryBarrier = {};
        memoryBarrier.sType           = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memoryBarrier.srcAccessMask   = mCurrentReadAccess | mCurrentWriteAccess | bufferAccessType;
        memoryBarrier.dstAccessMask   = VK_ACCESS_TRANSFER_WRITE_BIT;

        commandBuffer->pipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &memoryBarrier, 0,
                                       nullptr, 0, nullptr);
    }

    mCurrentWriteAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    mCurrentReadAccess  = 0;

    commandBuffer->copyBuffer(buffer, mBuffer, 1, &copyRegion);

    return angle::Result::Continue;
}

angle::Result BufferHelper::initBufferView(ContextVk *contextVk, const Format &format)
{
    ASSERT(format.valid());

    if (mBufferView.valid())
    {
        ASSERT(mViewFormat->vkBufferFormat == format.vkBufferFormat);
        return angle::Result::Continue;
    }

    VkBufferViewCreateInfo viewCreateInfo = {};
    viewCreateInfo.sType                  = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
    viewCreateInfo.buffer                 = mBuffer.getHandle();
    viewCreateInfo.format                 = format.vkBufferFormat;
    viewCreateInfo.offset                 = 0;
    viewCreateInfo.range                  = mSize;

    ANGLE_VK_TRY(contextVk, mBufferView.init(contextVk->getDevice(), viewCreateInfo));
    mViewFormat = &format;

    return angle::Result::Continue;
}

angle::Result BufferHelper::mapImpl(ContextVk *contextVk)
{
    ANGLE_VK_TRY(contextVk, mDeviceMemory.map(contextVk->getDevice(), 0, mSize, 0, &mMappedMemory));
    return angle::Result::Continue;
}

void BufferHelper::unmap(VkDevice device)
{
    if (mMappedMemory)
    {
        mDeviceMemory.unmap(device);
        mMappedMemory = nullptr;
    }
}

angle::Result BufferHelper::flush(ContextVk *contextVk, VkDeviceSize offset, VkDeviceSize size)
{
    bool hostVisible  = mMemoryPropertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    bool hostCoherent = mMemoryPropertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (hostVisible && !hostCoherent)
    {
        VkMappedMemoryRange range = {};
        range.sType               = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory              = mDeviceMemory.getHandle();
        range.offset              = offset;
        range.size                = size;
        ANGLE_VK_TRY(contextVk, vkFlushMappedMemoryRanges(contextVk->getDevice(), 1, &range));
    }
    return angle::Result::Continue;
}

angle::Result BufferHelper::invalidate(ContextVk *contextVk, VkDeviceSize offset, VkDeviceSize size)
{
    bool hostVisible  = mMemoryPropertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    bool hostCoherent = mMemoryPropertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (hostVisible && !hostCoherent)
    {
        VkMappedMemoryRange range = {};
        range.sType               = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory              = mDeviceMemory.getHandle();
        range.offset              = offset;
        range.size                = size;
        ANGLE_VK_TRY(contextVk, vkInvalidateMappedMemoryRanges(contextVk->getDevice(), 1, &range));
    }
    return angle::Result::Continue;
}

// ImageHelper implementation.
ImageHelper::ImageHelper()
    : CommandGraphResource(CommandGraphResourceType::Image),
      mFormat(nullptr),
      mSamples(0),
      mCurrentLayout(ImageLayout::Undefined),
      mCurrentQueueFamilyIndex(std::numeric_limits<uint32_t>::max()),
      mBaseLevel(0),
      mMaxLevel(0),
      mLayerCount(0),
      mLevelCount(0)
{}

ImageHelper::ImageHelper(ImageHelper &&other)
    : CommandGraphResource(CommandGraphResourceType::Image),
      mImage(std::move(other.mImage)),
      mDeviceMemory(std::move(other.mDeviceMemory)),
      mExtents(other.mExtents),
      mFormat(other.mFormat),
      mSamples(other.mSamples),
      mCurrentLayout(other.mCurrentLayout),
      mCurrentQueueFamilyIndex(other.mCurrentQueueFamilyIndex),
      mBaseLevel(other.mBaseLevel),
      mMaxLevel(other.mMaxLevel),
      mLayerCount(other.mLayerCount),
      mLevelCount(other.mLevelCount),
      mStagingBuffer(std::move(other.mStagingBuffer)),
      mSubresourceUpdates(std::move(other.mSubresourceUpdates))
{
    ASSERT(this != &other);
    other.mCurrentLayout = ImageLayout::Undefined;
    other.mBaseLevel     = 0;
    other.mMaxLevel      = 0;
    other.mLayerCount    = 0;
    other.mLevelCount    = 0;
}

ImageHelper::~ImageHelper()
{
    ASSERT(!valid());
}

void ImageHelper::initStagingBuffer(RendererVk *renderer,
                                    const Format &format,
                                    VkBufferUsageFlags usageFlags,
                                    size_t initialSize)
{
    mStagingBuffer.init(renderer, usageFlags, format.getImageCopyBufferAlignment(), initialSize,
                        true);
}

angle::Result ImageHelper::init(Context *context,
                                gl::TextureType textureType,
                                const VkExtent3D &extents,
                                const Format &format,
                                GLint samples,
                                VkImageUsageFlags usage,
                                uint32_t baseLevel,
                                uint32_t maxLevel,
                                uint32_t mipLevels,
                                uint32_t layerCount)
{
    return initExternal(context, textureType, extents, format, samples, usage,
                        ImageLayout::Undefined, nullptr, baseLevel, maxLevel, mipLevels,
                        layerCount);
}

angle::Result ImageHelper::initExternal(Context *context,
                                        gl::TextureType textureType,
                                        const VkExtent3D &extents,
                                        const Format &format,
                                        GLint samples,
                                        VkImageUsageFlags usage,
                                        ImageLayout initialLayout,
                                        const void *externalImageCreateInfo,
                                        uint32_t baseLevel,
                                        uint32_t maxLevel,
                                        uint32_t mipLevels,
                                        uint32_t layerCount)
{
    ASSERT(!valid());

    mExtents    = extents;
    mFormat     = &format;
    mSamples    = samples;
    mBaseLevel  = baseLevel;
    mMaxLevel   = maxLevel;
    mLevelCount = mipLevels;
    mLayerCount = layerCount;

    // Validate that mLayerCount is compatible with the texture type
    ASSERT(textureType != gl::TextureType::_3D || mLayerCount == 1);
    ASSERT(textureType != gl::TextureType::_2DArray || mExtents.depth == 1);
    ASSERT(textureType != gl::TextureType::External || mLayerCount == 1);
    ASSERT(textureType != gl::TextureType::Rectangle || mLayerCount == 1);
    ASSERT(textureType != gl::TextureType::CubeMap || mLayerCount == gl::kCubeFaceCount);

    VkImageCreateInfo imageInfo     = {};
    imageInfo.sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.pNext                 = externalImageCreateInfo;
    imageInfo.flags                 = GetImageCreateFlags(textureType);
    imageInfo.imageType             = gl_vk::GetImageType(textureType);
    imageInfo.format                = format.vkImageFormat;
    imageInfo.extent                = mExtents;
    imageInfo.mipLevels             = mipLevels;
    imageInfo.arrayLayers           = mLayerCount;
    imageInfo.samples               = gl_vk::GetSamples(samples);
    imageInfo.tiling                = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage                 = usage;
    imageInfo.sharingMode           = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.queueFamilyIndexCount = 0;
    imageInfo.pQueueFamilyIndices   = nullptr;
    imageInfo.initialLayout         = kImageMemoryBarrierData[initialLayout].layout;

    mCurrentLayout = initialLayout;

    ANGLE_VK_TRY(context, mImage.init(context->getDevice(), imageInfo));

    return angle::Result::Continue;
}

void ImageHelper::releaseImage(RendererVk *renderer)
{
    renderer->collectGarbageAndReinit(&mUse, &mImage, &mDeviceMemory);
}

void ImageHelper::releaseStagingBuffer(RendererVk *renderer)
{
    // Remove updates that never made it to the texture.
    for (SubresourceUpdate &update : mSubresourceUpdates)
    {
        update.release(renderer);
    }
    mStagingBuffer.release(renderer);
    mSubresourceUpdates.clear();
}

void ImageHelper::resetImageWeakReference()
{
    mImage.reset();
}

angle::Result ImageHelper::initMemory(Context *context,
                                      const MemoryProperties &memoryProperties,
                                      VkMemoryPropertyFlags flags)
{
    // TODO(jmadill): Memory sub-allocation. http://anglebug.com/2162
    ANGLE_TRY(AllocateImageMemory(context, flags, nullptr, &mImage, &mDeviceMemory));
    mCurrentQueueFamilyIndex = context->getRenderer()->getQueueFamilyIndex();
    return angle::Result::Continue;
}

angle::Result ImageHelper::initExternalMemory(Context *context,
                                              const MemoryProperties &memoryProperties,
                                              const VkMemoryRequirements &memoryRequirements,
                                              const void *extraAllocationInfo,
                                              uint32_t currentQueueFamilyIndex,

                                              VkMemoryPropertyFlags flags)
{
    // TODO(jmadill): Memory sub-allocation. http://anglebug.com/2162
    ANGLE_TRY(AllocateImageMemoryWithRequirements(context, flags, memoryRequirements,
                                                  extraAllocationInfo, &mImage, &mDeviceMemory));
    mCurrentQueueFamilyIndex = currentQueueFamilyIndex;
    return angle::Result::Continue;
}

angle::Result ImageHelper::initImageView(Context *context,
                                         gl::TextureType textureType,
                                         VkImageAspectFlags aspectMask,
                                         const gl::SwizzleState &swizzleMap,
                                         ImageView *imageViewOut,
                                         uint32_t baseMipLevel,
                                         uint32_t levelCount)
{
    return initLayerImageView(context, textureType, aspectMask, swizzleMap, imageViewOut,
                              baseMipLevel, levelCount, 0, mLayerCount);
}

angle::Result ImageHelper::initLayerImageView(Context *context,
                                              gl::TextureType textureType,
                                              VkImageAspectFlags aspectMask,
                                              const gl::SwizzleState &swizzleMap,
                                              ImageView *imageViewOut,
                                              uint32_t baseMipLevel,
                                              uint32_t levelCount,
                                              uint32_t baseArrayLayer,
                                              uint32_t layerCount) const
{
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType                 = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.flags                 = 0;
    viewInfo.image                 = mImage.getHandle();
    viewInfo.viewType              = gl_vk::GetImageViewType(textureType);
    viewInfo.format                = mFormat->vkImageFormat;
    if (swizzleMap.swizzleRequired())
    {
        viewInfo.components.r = gl_vk::GetSwizzle(swizzleMap.swizzleRed);
        viewInfo.components.g = gl_vk::GetSwizzle(swizzleMap.swizzleGreen);
        viewInfo.components.b = gl_vk::GetSwizzle(swizzleMap.swizzleBlue);
        viewInfo.components.a = gl_vk::GetSwizzle(swizzleMap.swizzleAlpha);
    }
    else
    {
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    }
    viewInfo.subresourceRange.aspectMask     = aspectMask;
    viewInfo.subresourceRange.baseMipLevel   = baseMipLevel;
    viewInfo.subresourceRange.levelCount     = levelCount;
    viewInfo.subresourceRange.baseArrayLayer = baseArrayLayer;
    viewInfo.subresourceRange.layerCount     = layerCount;

    ANGLE_VK_TRY(context, imageViewOut->init(context->getDevice(), viewInfo));
    return angle::Result::Continue;
}

void ImageHelper::destroy(VkDevice device)
{
    mImage.destroy(device);
    mDeviceMemory.destroy(device);
    mStagingBuffer.destroy(device);
    mCurrentLayout = ImageLayout::Undefined;
    mLayerCount    = 0;
    mLevelCount    = 0;
}

void ImageHelper::init2DWeakReference(VkImage handle,
                                      const gl::Extents &glExtents,
                                      const Format &format,
                                      GLint samples)
{
    ASSERT(!valid());

    gl_vk::GetExtent(glExtents, &mExtents);
    mFormat        = &format;
    mSamples       = samples;
    mCurrentLayout = ImageLayout::Undefined;
    mLayerCount    = 1;
    mLevelCount    = 1;

    mImage.setHandle(handle);
}

angle::Result ImageHelper::init2DStaging(Context *context,
                                         const MemoryProperties &memoryProperties,
                                         const gl::Extents &glExtents,
                                         const Format &format,
                                         VkImageUsageFlags usage,
                                         uint32_t layerCount)
{
    ASSERT(!valid());

    gl_vk::GetExtent(glExtents, &mExtents);
    mFormat     = &format;
    mSamples    = 1;
    mLayerCount = layerCount;
    mLevelCount = 1;

    mCurrentLayout = ImageLayout::Undefined;

    VkImageCreateInfo imageInfo     = {};
    imageInfo.sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags                 = 0;
    imageInfo.imageType             = VK_IMAGE_TYPE_2D;
    imageInfo.format                = format.vkImageFormat;
    imageInfo.extent                = mExtents;
    imageInfo.mipLevels             = 1;
    imageInfo.arrayLayers           = mLayerCount;
    imageInfo.samples               = gl_vk::GetSamples(mSamples);
    imageInfo.tiling                = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage                 = usage;
    imageInfo.sharingMode           = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.queueFamilyIndexCount = 0;
    imageInfo.pQueueFamilyIndices   = nullptr;
    imageInfo.initialLayout         = getCurrentLayout();

    ANGLE_VK_TRY(context, mImage.init(context->getDevice(), imageInfo));

    // Allocate and bind device-local memory.
    VkMemoryPropertyFlags memoryPropertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    ANGLE_TRY(initMemory(context, memoryProperties, memoryPropertyFlags));

    return angle::Result::Continue;
}

VkImageAspectFlags ImageHelper::getAspectFlags() const
{
    return GetFormatAspectFlags(mFormat->actualImageFormat());
}

VkImageLayout ImageHelper::getCurrentLayout() const
{
    return kImageMemoryBarrierData[mCurrentLayout].layout;
}

gl::Extents ImageHelper::getLevelExtents2D(uint32_t level) const
{
    uint32_t width  = std::max(mExtents.width >> level, 1u);
    uint32_t height = std::max(mExtents.height >> level, 1u);

    return gl::Extents(width, height, 1);
}

bool ImageHelper::isLayoutChangeNecessary(ImageLayout newLayout) const
{
    const ImageMemoryBarrierData &layoutData = kImageMemoryBarrierData[mCurrentLayout];

    // If transitioning to the same layout, we rarely need a barrier.  RAR (read-after-read)
    // doesn't need a barrier, and WAW (write-after-write) is guaranteed to not require a barrier
    // for color attachment and depth/stencil attachment writes.  Transfer dst and shader writes
    // are basically the only cases where an execution barrier is still necessary.
    bool sameLayoutAndNoNeedForBarrier =
        mCurrentLayout == newLayout && !layoutData.sameLayoutTransitionRequiresBarrier;

    return !sameLayoutAndNoNeedForBarrier;
}

void ImageHelper::changeLayout(VkImageAspectFlags aspectMask,
                               ImageLayout newLayout,
                               CommandBuffer *commandBuffer)
{
    if (!isLayoutChangeNecessary(newLayout))
    {
        return;
    }

    forceChangeLayoutAndQueue(aspectMask, newLayout, mCurrentQueueFamilyIndex, commandBuffer);
}

void ImageHelper::changeLayoutAndQueue(VkImageAspectFlags aspectMask,
                                       ImageLayout newLayout,
                                       uint32_t newQueueFamilyIndex,
                                       CommandBuffer *commandBuffer)
{
    ASSERT(isQueueChangeNeccesary(newQueueFamilyIndex));
    forceChangeLayoutAndQueue(aspectMask, newLayout, newQueueFamilyIndex, commandBuffer);
}

uint32_t ImageHelper::getBaseLevel()
{
    return mBaseLevel;
}

void ImageHelper::setBaseAndMaxLevels(uint32_t baseLevel, uint32_t maxLevel)
{
    mBaseLevel = baseLevel;
    mMaxLevel  = maxLevel;
}

void ImageHelper::forceChangeLayoutAndQueue(VkImageAspectFlags aspectMask,
                                            ImageLayout newLayout,
                                            uint32_t newQueueFamilyIndex,
                                            CommandBuffer *commandBuffer)
{
    // If transitioning to the same layout (and there is no queue transfer), an execution barrier
    // suffices.
    //
    // TODO(syoussefi): AMD driver on windows has a bug where an execution barrier is not sufficient
    // between transfer dst operations (even if the transfer is not to the same subresource!).  A
    // workaround may be necessary.  http://anglebug.com/3554
    if (mCurrentLayout == newLayout && mCurrentQueueFamilyIndex == newQueueFamilyIndex &&
        mCurrentLayout != ImageLayout::TransferDst)
    {
        const ImageMemoryBarrierData &transition = kImageMemoryBarrierData[mCurrentLayout];

        // In this case, the image is going to be used in the same way, so the src and dst stage
        // masks must be necessarily equal.
        ASSERT(transition.srcStageMask == transition.dstStageMask);

        commandBuffer->executionBarrier(transition.dstStageMask);
        return;
    }

    const ImageMemoryBarrierData &transitionFrom = kImageMemoryBarrierData[mCurrentLayout];
    const ImageMemoryBarrierData &transitionTo   = kImageMemoryBarrierData[newLayout];

    VkImageMemoryBarrier imageMemoryBarrier = {};
    imageMemoryBarrier.sType                = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imageMemoryBarrier.srcAccessMask        = transitionFrom.srcAccessMask;
    imageMemoryBarrier.dstAccessMask        = transitionTo.dstAccessMask;
    imageMemoryBarrier.oldLayout            = transitionFrom.layout;
    imageMemoryBarrier.newLayout            = transitionTo.layout;
    imageMemoryBarrier.srcQueueFamilyIndex  = mCurrentQueueFamilyIndex;
    imageMemoryBarrier.dstQueueFamilyIndex  = newQueueFamilyIndex;
    imageMemoryBarrier.image                = mImage.getHandle();

    // TODO(jmadill): Is this needed for mipped/layer images?
    imageMemoryBarrier.subresourceRange.aspectMask     = aspectMask;
    imageMemoryBarrier.subresourceRange.baseMipLevel   = 0;
    imageMemoryBarrier.subresourceRange.levelCount     = mLevelCount;
    imageMemoryBarrier.subresourceRange.baseArrayLayer = 0;
    imageMemoryBarrier.subresourceRange.layerCount     = mLayerCount;

    commandBuffer->imageBarrier(transitionFrom.srcStageMask, transitionTo.dstStageMask,
                                &imageMemoryBarrier);
    mCurrentLayout           = newLayout;
    mCurrentQueueFamilyIndex = newQueueFamilyIndex;
}

void ImageHelper::clearColor(const VkClearColorValue &color,
                             uint32_t baseMipLevel,
                             uint32_t levelCount,
                             uint32_t baseArrayLayer,
                             uint32_t layerCount,
                             CommandBuffer *commandBuffer)
{
    ASSERT(valid());

    ASSERT(mCurrentLayout == ImageLayout::TransferDst);

    VkImageSubresourceRange range = {};
    range.aspectMask              = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel            = baseMipLevel;
    range.levelCount              = levelCount;
    range.baseArrayLayer          = baseArrayLayer;
    range.layerCount              = layerCount;

    commandBuffer->clearColorImage(mImage, getCurrentLayout(), color, 1, &range);
}

void ImageHelper::clearDepthStencil(VkImageAspectFlags imageAspectFlags,
                                    VkImageAspectFlags clearAspectFlags,
                                    const VkClearDepthStencilValue &depthStencil,
                                    uint32_t baseMipLevel,
                                    uint32_t levelCount,
                                    uint32_t baseArrayLayer,
                                    uint32_t layerCount,
                                    CommandBuffer *commandBuffer)
{
    ASSERT(valid());

    ASSERT(mCurrentLayout == ImageLayout::TransferDst);

    VkImageSubresourceRange clearRange = {
        /*aspectMask*/ clearAspectFlags,
        /*baseMipLevel*/ baseMipLevel,
        /*levelCount*/ levelCount,
        /*baseArrayLayer*/ baseArrayLayer,
        /*layerCount*/ layerCount,
    };

    commandBuffer->clearDepthStencilImage(mImage, getCurrentLayout(), depthStencil, 1, &clearRange);
}

void ImageHelper::clear(const VkClearValue &value,
                        uint32_t mipLevel,
                        uint32_t baseArrayLayer,
                        uint32_t layerCount,
                        CommandBuffer *commandBuffer)
{
    const angle::Format &angleFormat = mFormat->intendedFormat();
    bool isDepthStencil              = angleFormat.depthBits > 0 || angleFormat.stencilBits > 0;

    if (isDepthStencil)
    {
        const VkImageAspectFlags aspect = GetDepthStencilAspectFlags(mFormat->actualImageFormat());
        clearDepthStencil(aspect, aspect, value.depthStencil, mipLevel, 1, baseArrayLayer,
                          layerCount, commandBuffer);
    }
    else
    {
        clearColor(value.color, mipLevel, 1, baseArrayLayer, layerCount, commandBuffer);
    }
}

gl::Extents ImageHelper::getSize(const gl::ImageIndex &index) const
{
    GLint mipLevel = index.getLevelIndex();
    // Level 0 should be the size of the extents, after that every time you increase a level
    // you shrink the extents by half.
    return gl::Extents(std::max(1u, mExtents.width >> mipLevel),
                       std::max(1u, mExtents.height >> mipLevel), mExtents.depth);
}

// static
void ImageHelper::Copy(ImageHelper *srcImage,
                       ImageHelper *dstImage,
                       const gl::Offset &srcOffset,
                       const gl::Offset &dstOffset,
                       const gl::Extents &copySize,
                       const VkImageSubresourceLayers &srcSubresource,
                       const VkImageSubresourceLayers &dstSubresource,
                       CommandBuffer *commandBuffer)
{
    ASSERT(commandBuffer->valid() && srcImage->valid() && dstImage->valid());

    ASSERT(srcImage->getCurrentLayout() == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    ASSERT(dstImage->getCurrentLayout() == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageCopy region    = {};
    region.srcSubresource = srcSubresource;
    region.srcOffset.x    = srcOffset.x;
    region.srcOffset.y    = srcOffset.y;
    region.srcOffset.z    = srcOffset.z;
    region.dstSubresource = dstSubresource;
    region.dstOffset.x    = dstOffset.x;
    region.dstOffset.y    = dstOffset.y;
    region.dstOffset.z    = dstOffset.z;
    region.extent.width   = copySize.width;
    region.extent.height  = copySize.height;
    region.extent.depth   = copySize.depth;

    commandBuffer->copyImage(srcImage->getImage(), srcImage->getCurrentLayout(),
                             dstImage->getImage(), dstImage->getCurrentLayout(), 1, &region);
}

angle::Result ImageHelper::generateMipmapsWithBlit(ContextVk *contextVk, GLuint maxLevel)
{
    CommandBuffer *commandBuffer = nullptr;
    ANGLE_TRY(recordCommands(contextVk, &commandBuffer));

    changeLayout(VK_IMAGE_ASPECT_COLOR_BIT, ImageLayout::TransferDst, commandBuffer);

    // We are able to use blitImage since the image format we are using supports it. This
    // is a faster way we can generate the mips.
    int32_t mipWidth  = mExtents.width;
    int32_t mipHeight = mExtents.height;

    // Manually manage the image memory barrier because it uses a lot more parameters than our
    // usual one.
    VkImageMemoryBarrier barrier            = {};
    barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image                           = mImage.getHandle();
    barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount     = mLayerCount;
    barrier.subresourceRange.levelCount     = 1;

    for (uint32_t mipLevel = 1; mipLevel <= maxLevel; mipLevel++)
    {
        int32_t nextMipWidth  = std::max<int32_t>(1, mipWidth >> 1);
        int32_t nextMipHeight = std::max<int32_t>(1, mipHeight >> 1);

        barrier.subresourceRange.baseMipLevel = mipLevel - 1;
        barrier.oldLayout                     = getCurrentLayout();
        barrier.newLayout                     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask                 = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask                 = VK_ACCESS_TRANSFER_READ_BIT;

        // We can do it for all layers at once.
        commandBuffer->imageBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    &barrier);
        VkImageBlit blit                   = {};
        blit.srcOffsets[0]                 = {0, 0, 0};
        blit.srcOffsets[1]                 = {mipWidth, mipHeight, 1};
        blit.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel       = mipLevel - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount     = mLayerCount;
        blit.dstOffsets[0]                 = {0, 0, 0};
        blit.dstOffsets[1]                 = {nextMipWidth, nextMipHeight, 1};
        blit.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel       = mipLevel;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount     = mLayerCount;

        mipWidth  = nextMipWidth;
        mipHeight = nextMipHeight;

        bool formatSupportsLinearFiltering = contextVk->getRenderer()->hasImageFormatFeatureBits(
            getFormat().vkImageFormat, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);

        commandBuffer->blitImage(
            mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
            formatSupportsLinearFiltering ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }

    // Transition the last mip level to the same layout as all the other ones, so we can declare
    // our whole image layout to be SRC_OPTIMAL.
    barrier.subresourceRange.baseMipLevel = maxLevel;
    barrier.oldLayout                     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout                     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    // We can do it for all layers at once.
    commandBuffer->imageBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                &barrier);
    // This is just changing the internal state of the image helper so that the next call
    // to changeLayout will use this layout as the "oldLayout" argument.
    mCurrentLayout = ImageLayout::TransferSrc;

    return angle::Result::Continue;
}

void ImageHelper::resolve(ImageHelper *dest,
                          const VkImageResolve &region,
                          CommandBuffer *commandBuffer)
{
    ASSERT(mCurrentLayout == ImageLayout::TransferSrc);
    dest->changeLayout(region.dstSubresource.aspectMask, ImageLayout::TransferDst, commandBuffer);

    commandBuffer->resolveImage(getImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest->getImage(),
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

void ImageHelper::removeStagedUpdates(ContextVk *contextVk, const gl::ImageIndex &index)
{
    // Find any staged updates for this index and removes them from the pending list.
    uint32_t levelIndex = index.getLevelIndex();
    uint32_t layerIndex = index.hasLayer() ? index.getLayerIndex() : 0;

    for (size_t index = 0; index < mSubresourceUpdates.size();)
    {
        auto update = mSubresourceUpdates.begin() + index;
        if (update->isUpdateToLayerLevel(layerIndex, levelIndex))
        {
            update->release(contextVk->getRenderer());
            mSubresourceUpdates.erase(update);
        }
        else
        {
            index++;
        }
    }
}

angle::Result ImageHelper::stageSubresourceUpdate(ContextVk *contextVk,
                                                  const gl::ImageIndex &index,
                                                  const gl::Extents &glExtents,
                                                  const gl::Offset &offset,
                                                  const gl::InternalFormat &formatInfo,
                                                  const gl::PixelUnpackState &unpack,
                                                  GLenum type,
                                                  const uint8_t *pixels,
                                                  const Format &vkFormat)
{
    GLuint inputRowPitch = 0;
    ANGLE_VK_CHECK_MATH(contextVk,
                        formatInfo.computeRowPitch(type, glExtents.width, unpack.alignment,
                                                   unpack.rowLength, &inputRowPitch));

    GLuint inputDepthPitch = 0;
    ANGLE_VK_CHECK_MATH(
        contextVk, formatInfo.computeDepthPitch(glExtents.height, unpack.imageHeight, inputRowPitch,
                                                &inputDepthPitch));

    GLuint inputSkipBytes = 0;
    ANGLE_VK_CHECK_MATH(contextVk,
                        formatInfo.computeSkipBytes(type, inputRowPitch, inputDepthPitch, unpack,
                                                    index.usesTex3D(), &inputSkipBytes));

    const angle::Format &storageFormat = vkFormat.actualImageFormat();

    size_t outputRowPitch;
    size_t outputDepthPitch;
    size_t stencilAllocationSize = 0;
    uint32_t bufferRowLength;
    uint32_t bufferImageHeight;
    size_t allocationSize;

    LoadImageFunctionInfo loadFunctionInfo = vkFormat.textureLoadFunctions(type);
    LoadImageFunction stencilLoadFunction  = nullptr;

    if (storageFormat.isBlock)
    {
        const gl::InternalFormat &storageFormatInfo = vkFormat.getInternalFormatInfo(type);
        GLuint rowPitch;
        GLuint depthPitch;
        GLuint totalSize;

        ANGLE_VK_CHECK_MATH(contextVk, storageFormatInfo.computeCompressedImageSize(
                                           gl::Extents(glExtents.width, 1, 1), &rowPitch));
        ANGLE_VK_CHECK_MATH(contextVk,
                            storageFormatInfo.computeCompressedImageSize(
                                gl::Extents(glExtents.width, glExtents.height, 1), &depthPitch));

        ANGLE_VK_CHECK_MATH(contextVk,
                            storageFormatInfo.computeCompressedImageSize(glExtents, &totalSize));

        outputRowPitch   = rowPitch;
        outputDepthPitch = depthPitch;

        angle::CheckedNumeric<uint32_t> checkedRowLength =
            rx::CheckedRoundUp<uint32_t>(glExtents.width, storageFormatInfo.compressedBlockWidth);
        angle::CheckedNumeric<uint32_t> checkedImageHeight =
            rx::CheckedRoundUp<uint32_t>(glExtents.height, storageFormatInfo.compressedBlockHeight);

        ANGLE_VK_CHECK_MATH(contextVk, checkedRowLength.IsValid());
        ANGLE_VK_CHECK_MATH(contextVk, checkedImageHeight.IsValid());

        bufferRowLength   = checkedRowLength.ValueOrDie();
        bufferImageHeight = checkedImageHeight.ValueOrDie();
        allocationSize    = totalSize;
    }
    else
    {
        ASSERT(storageFormat.pixelBytes != 0);

        if (storageFormat.id == angle::FormatID::D24_UNORM_S8_UINT)
        {
            stencilLoadFunction = angle::LoadX24S8ToS8;
        }
        if (storageFormat.id == angle::FormatID::D32_FLOAT_S8X24_UINT)
        {
            // If depth is D32FLOAT_S8, we must pack D32F tightly (no stencil) for CopyBufferToImage
            outputRowPitch = sizeof(float) * glExtents.width;

            // The generic load functions don't handle tightly packing D32FS8 to D32F & S8 so call
            // special case load functions.
            switch (type)
            {
                case GL_UNSIGNED_INT:
                    loadFunctionInfo.loadFunction = angle::LoadD32ToD32F;
                    stencilLoadFunction           = nullptr;
                    break;
                case GL_DEPTH32F_STENCIL8:
                case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
                    loadFunctionInfo.loadFunction = angle::LoadD32FS8X24ToD32F;
                    stencilLoadFunction           = angle::LoadX32S8ToS8;
                    break;
                case GL_UNSIGNED_INT_24_8_OES:
                    loadFunctionInfo.loadFunction = angle::LoadD24S8ToD32F;
                    stencilLoadFunction           = angle::LoadX24S8ToS8;
                    break;
                default:
                    UNREACHABLE();
            }
        }
        else
        {
            outputRowPitch = storageFormat.pixelBytes * glExtents.width;
        }
        outputDepthPitch = outputRowPitch * glExtents.height;

        bufferRowLength   = glExtents.width;
        bufferImageHeight = glExtents.height;

        allocationSize = outputDepthPitch * glExtents.depth;

        // Note: because the LoadImageFunctionInfo functions are limited to copying a single
        // component, we have to special case packed depth/stencil use and send the stencil as a
        // separate chunk.
        if (storageFormat.depthBits > 0 && storageFormat.stencilBits > 0 &&
            formatInfo.depthBits > 0 && formatInfo.stencilBits > 0)
        {
            // Note: Stencil is always one byte
            stencilAllocationSize = glExtents.width * glExtents.height * glExtents.depth;
            allocationSize += stencilAllocationSize;
        }
    }

    VkBuffer bufferHandle = VK_NULL_HANDLE;

    uint8_t *stagingPointer    = nullptr;
    VkDeviceSize stagingOffset = 0;
    ANGLE_TRY(mStagingBuffer.allocate(contextVk, allocationSize, &stagingPointer, &bufferHandle,
                                      &stagingOffset, nullptr));

    const uint8_t *source = pixels + static_cast<ptrdiff_t>(inputSkipBytes);

    loadFunctionInfo.loadFunction(glExtents.width, glExtents.height, glExtents.depth, source,
                                  inputRowPitch, inputDepthPitch, stagingPointer, outputRowPitch,
                                  outputDepthPitch);

    VkBufferImageCopy copy         = {};
    VkImageAspectFlags aspectFlags = GetFormatAspectFlags(vkFormat.actualImageFormat());

    copy.bufferOffset      = stagingOffset;
    copy.bufferRowLength   = bufferRowLength;
    copy.bufferImageHeight = bufferImageHeight;

    copy.imageSubresource.mipLevel   = index.getLevelIndex();
    copy.imageSubresource.layerCount = index.getLayerCount();

    gl_vk::GetOffset(offset, &copy.imageOffset);
    gl_vk::GetExtent(glExtents, &copy.imageExtent);

    if (gl::IsArrayTextureType(index.getType()))
    {
        copy.imageSubresource.baseArrayLayer = offset.z;
        copy.imageOffset.z                   = 0;
        copy.imageExtent.depth               = 1;
    }
    else
    {
        copy.imageSubresource.baseArrayLayer = index.hasLayer() ? index.getLayerIndex() : 0;
    }

    if (stencilAllocationSize > 0)
    {
        // Note: Stencil is always one byte
        ASSERT((aspectFlags & VK_IMAGE_ASPECT_STENCIL_BIT) != 0);

        // Skip over depth data.
        stagingPointer += outputDepthPitch * glExtents.depth;
        stagingOffset += outputDepthPitch * glExtents.depth;

        // recompute pitch for stencil data
        outputRowPitch   = glExtents.width;
        outputDepthPitch = outputRowPitch * glExtents.height;

        ASSERT(stencilLoadFunction != nullptr);
        stencilLoadFunction(glExtents.width, glExtents.height, glExtents.depth, source,
                            inputRowPitch, inputDepthPitch, stagingPointer, outputRowPitch,
                            outputDepthPitch);

        VkBufferImageCopy stencilCopy = {};

        stencilCopy.bufferOffset                    = stagingOffset;
        stencilCopy.bufferRowLength                 = bufferRowLength;
        stencilCopy.bufferImageHeight               = bufferImageHeight;
        stencilCopy.imageSubresource.mipLevel       = copy.imageSubresource.mipLevel;
        stencilCopy.imageSubresource.baseArrayLayer = copy.imageSubresource.baseArrayLayer;
        stencilCopy.imageSubresource.layerCount     = copy.imageSubresource.layerCount;
        stencilCopy.imageOffset                     = copy.imageOffset;
        stencilCopy.imageExtent                     = copy.imageExtent;
        stencilCopy.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_STENCIL_BIT;
        mSubresourceUpdates.emplace_back(mStagingBuffer.getCurrentBuffer(), stencilCopy);

        aspectFlags &= ~VK_IMAGE_ASPECT_STENCIL_BIT;
    }

    if (IsMaskFlagSet(aspectFlags, static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_STENCIL_BIT |
                                                                   VK_IMAGE_ASPECT_DEPTH_BIT)))
    {
        // We still have both depth and stencil aspect bits set. That means we have a destination
        // buffer that is packed depth stencil and that the application is only loading one aspect.
        // Figure out which aspect the user is touching and remove the unused aspect bit.
        if (formatInfo.stencilBits > 0)
        {
            aspectFlags &= ~VK_IMAGE_ASPECT_DEPTH_BIT;
        }
        else
        {
            aspectFlags &= ~VK_IMAGE_ASPECT_STENCIL_BIT;
        }
    }

    if (aspectFlags)
    {
        copy.imageSubresource.aspectMask = aspectFlags;
        mSubresourceUpdates.emplace_back(mStagingBuffer.getCurrentBuffer(), copy);
    }

    return angle::Result::Continue;
}

angle::Result ImageHelper::stageSubresourceUpdateAndGetData(ContextVk *contextVk,
                                                            size_t allocationSize,
                                                            const gl::ImageIndex &imageIndex,
                                                            const gl::Extents &glExtents,
                                                            const gl::Offset &offset,
                                                            uint8_t **destData)
{
    VkBuffer bufferHandle;
    VkDeviceSize stagingOffset = 0;
    ANGLE_TRY(mStagingBuffer.allocate(contextVk, allocationSize, destData, &bufferHandle,
                                      &stagingOffset, nullptr));

    VkBufferImageCopy copy               = {};
    copy.bufferOffset                    = stagingOffset;
    copy.bufferRowLength                 = glExtents.width;
    copy.bufferImageHeight               = glExtents.height;
    copy.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.mipLevel       = imageIndex.getLevelIndex();
    copy.imageSubresource.baseArrayLayer = imageIndex.hasLayer() ? imageIndex.getLayerIndex() : 0;
    copy.imageSubresource.layerCount     = imageIndex.getLayerCount();

    // Note: Only support color now
    ASSERT(getAspectFlags() == VK_IMAGE_ASPECT_COLOR_BIT);

    gl_vk::GetOffset(offset, &copy.imageOffset);
    gl_vk::GetExtent(glExtents, &copy.imageExtent);

    mSubresourceUpdates.emplace_back(mStagingBuffer.getCurrentBuffer(), copy);

    return angle::Result::Continue;
}

angle::Result ImageHelper::stageSubresourceUpdateFromBuffer(ContextVk *contextVk,
                                                            size_t allocationSize,
                                                            uint32_t mipLevel,
                                                            uint32_t baseArrayLayer,
                                                            uint32_t layerCount,
                                                            const VkExtent3D &extent,
                                                            const VkOffset3D &offset,
                                                            BufferHelper *bufferHelper,
                                                            VkDeviceSize stagingOffset)
{
    // This function stages an update from explicitly provided handle and offset
    // It is used when the texture base level has changed, and we need to propagate data

    VkBufferImageCopy copy               = {};
    copy.bufferOffset                    = stagingOffset;
    copy.bufferRowLength                 = extent.width;
    copy.bufferImageHeight               = extent.height;
    copy.imageSubresource.aspectMask     = getAspectFlags();
    copy.imageSubresource.mipLevel       = mipLevel;
    copy.imageSubresource.baseArrayLayer = baseArrayLayer;
    copy.imageSubresource.layerCount     = layerCount;
    copy.imageOffset                     = offset;
    copy.imageExtent                     = extent;

    ASSERT(getAspectFlags() == VK_IMAGE_ASPECT_COLOR_BIT ||
           getAspectFlags() == VK_IMAGE_ASPECT_DEPTH_BIT ||
           getAspectFlags() == VK_IMAGE_ASPECT_STENCIL_BIT);

    mSubresourceUpdates.emplace_back(bufferHelper, copy);

    return angle::Result::Continue;
}

angle::Result ImageHelper::stageSubresourceUpdateFromFramebuffer(
    const gl::Context *context,
    const gl::ImageIndex &index,
    const gl::Rectangle &sourceArea,
    const gl::Offset &dstOffset,
    const gl::Extents &dstExtent,
    const gl::InternalFormat &formatInfo,
    FramebufferVk *framebufferVk)
{
    ContextVk *contextVk = GetImpl(context);

    // If the extents and offset is outside the source image, we need to clip.
    gl::Rectangle clippedRectangle;
    const gl::Extents readExtents = framebufferVk->getReadImageExtents();
    if (!ClipRectangle(sourceArea, gl::Rectangle(0, 0, readExtents.width, readExtents.height),
                       &clippedRectangle))
    {
        // Empty source area, nothing to do.
        return angle::Result::Continue;
    }

    bool isViewportFlipEnabled = contextVk->isViewportFlipEnabledForDrawFBO();
    if (isViewportFlipEnabled)
    {
        clippedRectangle.y = readExtents.height - clippedRectangle.y - clippedRectangle.height;
    }

    // 1- obtain a buffer handle to copy to
    RendererVk *renderer = contextVk->getRenderer();

    const Format &vkFormat             = renderer->getFormat(formatInfo.sizedInternalFormat);
    const angle::Format &storageFormat = vkFormat.actualImageFormat();
    LoadImageFunctionInfo loadFunction = vkFormat.textureLoadFunctions(formatInfo.type);

    size_t outputRowPitch   = storageFormat.pixelBytes * clippedRectangle.width;
    size_t outputDepthPitch = outputRowPitch * clippedRectangle.height;

    VkBuffer bufferHandle = VK_NULL_HANDLE;

    uint8_t *stagingPointer    = nullptr;
    VkDeviceSize stagingOffset = 0;

    // The destination is only one layer deep.
    size_t allocationSize = outputDepthPitch;
    ANGLE_TRY(mStagingBuffer.allocate(contextVk, allocationSize, &stagingPointer, &bufferHandle,
                                      &stagingOffset, nullptr));

    const angle::Format &copyFormat =
        GetFormatFromFormatType(formatInfo.internalFormat, formatInfo.type);
    PackPixelsParams params(clippedRectangle, copyFormat, static_cast<GLuint>(outputRowPitch),
                            isViewportFlipEnabled, nullptr, 0);

    RenderTargetVk *readRenderTarget = framebufferVk->getColorReadRenderTarget();

    // 2- copy the source image region to the pixel buffer using a cpu readback
    if (loadFunction.requiresConversion)
    {
        // When a conversion is required, we need to use the loadFunction to read from a temporary
        // buffer instead so its an even slower path.
        size_t bufferSize =
            storageFormat.pixelBytes * clippedRectangle.width * clippedRectangle.height;
        angle::MemoryBuffer *memoryBuffer = nullptr;
        ANGLE_VK_CHECK_ALLOC(contextVk, context->getScratchBuffer(bufferSize, &memoryBuffer));

        // Read into the scratch buffer
        ANGLE_TRY(framebufferVk->readPixelsImpl(contextVk, clippedRectangle, params,
                                                VK_IMAGE_ASPECT_COLOR_BIT, readRenderTarget,
                                                memoryBuffer->data()));

        // Load from scratch buffer to our pixel buffer
        loadFunction.loadFunction(clippedRectangle.width, clippedRectangle.height, 1,
                                  memoryBuffer->data(), outputRowPitch, 0, stagingPointer,
                                  outputRowPitch, 0);
    }
    else
    {
        // We read directly from the framebuffer into our pixel buffer.
        ANGLE_TRY(framebufferVk->readPixelsImpl(contextVk, clippedRectangle, params,
                                                VK_IMAGE_ASPECT_COLOR_BIT, readRenderTarget,
                                                stagingPointer));
    }

    // 3- enqueue the destination image subresource update
    VkBufferImageCopy copyToImage               = {};
    copyToImage.bufferOffset                    = static_cast<VkDeviceSize>(stagingOffset);
    copyToImage.bufferRowLength                 = 0;  // Tightly packed data can be specified as 0.
    copyToImage.bufferImageHeight               = clippedRectangle.height;
    copyToImage.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    copyToImage.imageSubresource.mipLevel       = index.getLevelIndex();
    copyToImage.imageSubresource.baseArrayLayer = index.hasLayer() ? index.getLayerIndex() : 0;
    copyToImage.imageSubresource.layerCount     = index.getLayerCount();
    gl_vk::GetOffset(dstOffset, &copyToImage.imageOffset);
    gl_vk::GetExtent(dstExtent, &copyToImage.imageExtent);

    // 3- enqueue the destination image subresource update
    mSubresourceUpdates.emplace_back(mStagingBuffer.getCurrentBuffer(), copyToImage);
    return angle::Result::Continue;
}

void ImageHelper::stageSubresourceUpdateFromImage(ImageHelper *image,
                                                  const gl::ImageIndex &index,
                                                  const gl::Offset &destOffset,
                                                  const gl::Extents &glExtents,
                                                  const VkImageType imageType)
{
    VkImageCopy copyToImage               = {};
    copyToImage.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyToImage.srcSubresource.layerCount = index.getLayerCount();
    copyToImage.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyToImage.dstSubresource.mipLevel   = index.getLevelIndex();

    if (imageType == VK_IMAGE_TYPE_3D)
    {
        // These values must be set explicitly to follow the Vulkan spec:
        // https://www.khronos.org/registry/vulkan/specs/1.1-extensions/man/html/VkImageCopy.html
        // If either of the calling command’s srcImage or dstImage parameters are of VkImageType
        // VK_IMAGE_TYPE_3D, the baseArrayLayer and layerCount members of the corresponding
        // subresource must be 0 and 1, respectively
        copyToImage.dstSubresource.baseArrayLayer = 0;
        copyToImage.dstSubresource.layerCount     = 1;
        // Preserve the assumption that destOffset.z == "dstSubresource.baseArrayLayer"
        ASSERT(destOffset.z == (index.hasLayer() ? index.getLayerIndex() : 0));
    }
    else
    {
        copyToImage.dstSubresource.baseArrayLayer = index.hasLayer() ? index.getLayerIndex() : 0;
        copyToImage.dstSubresource.layerCount     = index.getLayerCount();
    }

    gl_vk::GetOffset(destOffset, &copyToImage.dstOffset);
    gl_vk::GetExtent(glExtents, &copyToImage.extent);

    mSubresourceUpdates.emplace_back(image, copyToImage);
}

void ImageHelper::stageSubresourceRobustClear(const gl::ImageIndex &index,
                                              const angle::Format &format)
{
    stageSubresourceClear(index, format, kWebGLInitColorValue, kWebGLInitDepthStencilValue);
}

void ImageHelper::stageSubresourceEmulatedClear(const gl::ImageIndex &index,
                                                const angle::Format &format)
{
    stageSubresourceClear(index, format, kEmulatedInitColorValue, kWebGLInitDepthStencilValue);
}

void ImageHelper::stageClearIfEmulatedFormat(const gl::ImageIndex &index, const Format &format)
{
    if (format.hasEmulatedImageChannels())
    {
        stageSubresourceEmulatedClear(index, format.intendedFormat());
    }
}

void ImageHelper::stageSubresourceClear(const gl::ImageIndex &index,
                                        const angle::Format &format,
                                        const VkClearColorValue &colorValue,
                                        const VkClearDepthStencilValue &depthStencilValue)
{
    VkClearValue clearValue;

    bool isDepthStencil = format.depthBits > 0 || format.stencilBits > 0;
    if (isDepthStencil)
    {
        clearValue.depthStencil = depthStencilValue;
    }
    else
    {
        clearValue.color = colorValue;
    }

    // Note that clears can arrive out of order from the front-end with respect to staged changes,
    // but they are intended to be done first.
    mSubresourceUpdates.emplace(mSubresourceUpdates.begin(), clearValue, index);
}

angle::Result ImageHelper::allocateStagingMemory(ContextVk *contextVk,
                                                 size_t sizeInBytes,
                                                 uint8_t **ptrOut,
                                                 BufferHelper **bufferOut,
                                                 VkDeviceSize *offsetOut,
                                                 bool *newBufferAllocatedOut)
{
    VkBuffer handle;
    ANGLE_TRY(mStagingBuffer.allocate(contextVk, sizeInBytes, ptrOut, &handle, offsetOut,
                                      newBufferAllocatedOut));
    *bufferOut = mStagingBuffer.getCurrentBuffer();
    return angle::Result::Continue;
}

angle::Result ImageHelper::flushStagedUpdates(ContextVk *contextVk,
                                              uint32_t levelStart,
                                              uint32_t levelEnd,
                                              uint32_t layerStart,
                                              uint32_t layerEnd,
                                              CommandBuffer *commandBuffer)
{
    if (mSubresourceUpdates.empty())
    {
        return angle::Result::Continue;
    }

    ANGLE_TRY(mStagingBuffer.flush(contextVk));

    std::vector<SubresourceUpdate> updatesToKeep;
    const VkImageAspectFlags aspectFlags = GetFormatAspectFlags(mFormat->actualImageFormat());

    // Upload levels and layers that don't conflict in parallel.  The (level, layer) pair is hashed
    // to `(level * mLayerCount + layer) % 64` and used to track whether that subresource is
    // currently in transfer.  If so, a barrier is inserted.  If mLayerCount * mLevelCount > 64,
    // there will be a few unnecessary barriers.
    constexpr uint32_t kMaxParallelSubresourceUpload = 64;
    uint64_t subresourceUploadsInProgress            = 0;

    // Start in TransferDst.
    changeLayout(aspectFlags, ImageLayout::TransferDst, commandBuffer);

    for (SubresourceUpdate &update : mSubresourceUpdates)
    {
        ASSERT(update.updateSource == UpdateSource::Clear ||
               (update.updateSource == UpdateSource::Buffer &&
                update.buffer.bufferHelper != nullptr) ||
               (update.updateSource == UpdateSource::Image && update.image.image != nullptr &&
                update.image.image->valid()));

        uint32_t updateMipLevel;
        uint32_t updateBaseLayer;
        uint32_t updateLayerCount;
        if (update.updateSource == UpdateSource::Clear)
        {
            updateMipLevel   = update.clear.levelIndex;
            updateBaseLayer  = update.clear.layerIndex;
            updateLayerCount = update.clear.layerCount;
            if (updateLayerCount == static_cast<uint32_t>(gl::ImageIndex::kEntireLevel))
            {
                updateLayerCount = mLayerCount;
            }
        }
        else
        {
            const VkImageSubresourceLayers &dstSubresource = update.dstSubresource();
            updateMipLevel                                 = dstSubresource.mipLevel;
            updateBaseLayer                                = dstSubresource.baseArrayLayer;
            updateLayerCount                               = dstSubresource.layerCount;
            ASSERT(updateLayerCount != static_cast<uint32_t>(gl::ImageIndex::kEntireLevel));
        }

        // If the update level is not within the requested range, skip the update.
        const bool isUpdateLevelOutsideRange =
            updateMipLevel < (levelStart + mBaseLevel) ||
            (updateMipLevel >= (levelEnd + mBaseLevel) || updateMipLevel > mMaxLevel);

        // If the update layers don't intersect the requested layers, skip the update.
        const bool areUpdateLayersOutsideRange =
            updateBaseLayer + updateLayerCount <= layerStart || updateBaseLayer >= layerEnd;

        if (isUpdateLevelOutsideRange || areUpdateLayersOutsideRange)
        {
            updatesToKeep.emplace_back(update);
            continue;
        }

        if (mBaseLevel > 0)
        {
            // We need to shift the miplevel in the update to fall into the vkiamge
            if (update.updateSource == UpdateSource::Clear)
            {
                update.clear.levelIndex -= mBaseLevel;
            }
            else if (update.updateSource == UpdateSource::Buffer)
            {
                update.buffer.copyRegion.imageSubresource.mipLevel -= mBaseLevel;
            }
            else if (update.updateSource == UpdateSource::Image)
            {
                update.image.copyRegion.dstSubresource.mipLevel -= mBaseLevel;
            }
        }

        if (updateLayerCount >= kMaxParallelSubresourceUpload)
        {
            // If there are more subresources than bits we can track, always insert a barrier.
            changeLayout(aspectFlags, ImageLayout::TransferDst, commandBuffer);
            subresourceUploadsInProgress = std::numeric_limits<uint64_t>::max();
        }
        else
        {
            const uint64_t subresourceHashRange = angle::Bit<uint64_t>(updateLayerCount) - 1;
            const uint32_t subresourceHashOffset =
                (updateMipLevel * mLayerCount + updateBaseLayer) % kMaxParallelSubresourceUpload;
            const uint64_t subresourceHash =
                ANGLE_ROTL64(subresourceHashRange, subresourceHashOffset);

            if ((subresourceUploadsInProgress & subresourceHash) != 0)
            {
                // If there's overlap in subresource upload, issue a barrier.
                changeLayout(aspectFlags, ImageLayout::TransferDst, commandBuffer);
                subresourceUploadsInProgress = 0;
            }
            subresourceUploadsInProgress |= subresourceHash;
        }

        if (update.updateSource == UpdateSource::Clear)
        {
            clear(update.clear.value, updateMipLevel, updateBaseLayer, updateLayerCount,
                  commandBuffer);
        }
        else if (update.updateSource == UpdateSource::Buffer)
        {
            BufferUpdate &bufferUpdate = update.buffer;

            BufferHelper *currentBuffer = bufferUpdate.bufferHelper;
            ASSERT(currentBuffer && currentBuffer->valid());
            currentBuffer->onGraphAccess(contextVk->getCommandGraph());

            commandBuffer->copyBufferToImage(currentBuffer->getBuffer().getHandle(), mImage,
                                             getCurrentLayout(), 1, &update.buffer.copyRegion);
        }
        else
        {
            update.image.image->changeLayout(aspectFlags, ImageLayout::TransferSrc, commandBuffer);
            update.image.image->addReadDependency(contextVk, this);

            commandBuffer->copyImage(update.image.image->getImage(),
                                     update.image.image->getCurrentLayout(), mImage,
                                     getCurrentLayout(), 1, &update.image.copyRegion);
        }

        update.release(contextVk->getRenderer());
    }

    // Only remove the updates that were actually applied to the image.
    mSubresourceUpdates = std::move(updatesToKeep);

    if (mSubresourceUpdates.empty())
    {
        mStagingBuffer.releaseInFlightBuffers(contextVk);
    }

    return angle::Result::Continue;
}

angle::Result ImageHelper::flushAllStagedUpdates(ContextVk *contextVk)
{
    // Clear the image.
    CommandBuffer *commandBuffer = nullptr;
    ANGLE_TRY(recordCommands(contextVk, &commandBuffer));
    return flushStagedUpdates(contextVk, 0, mLevelCount, 0, mLayerCount, commandBuffer);
}

bool ImageHelper::isUpdateStaged(uint32_t level, uint32_t layer)
{
    // Check to see if any updates are staged for the given level and layer

    if (mSubresourceUpdates.empty())
    {
        return false;
    }

    for (SubresourceUpdate &update : mSubresourceUpdates)
    {
        uint32_t updateMipLevel;
        uint32_t updateBaseLayer;
        uint32_t updateLayerCount;

        if (update.updateSource == UpdateSource::Clear)
        {
            updateMipLevel   = update.clear.levelIndex;
            updateBaseLayer  = update.clear.layerIndex;
            updateLayerCount = update.clear.layerCount;
        }
        else
        {
            const VkImageSubresourceLayers &dstSubresource = update.dstSubresource();
            updateMipLevel                                 = dstSubresource.mipLevel;
            updateBaseLayer                                = dstSubresource.baseArrayLayer;
            updateLayerCount                               = dstSubresource.layerCount;
        }

        if (updateMipLevel == level)
        {
            if (layer >= updateBaseLayer && layer < (updateBaseLayer + updateLayerCount))
            {
                // The level matches, and the layer is within the range
                return true;
            }
        }
    }

    return false;
}

angle::Result ImageHelper::copyImageDataToBuffer(ContextVk *contextVk,
                                                 size_t sourceLevel,
                                                 uint32_t layerCount,
                                                 uint32_t baseLayer,
                                                 const gl::Box &sourceArea,
                                                 BufferHelper **bufferOut,
                                                 VkDeviceSize *bufferOffsetOut,
                                                 uint8_t **outDataPtr)
{
    ANGLE_TRACE_EVENT0("gpu.angle", "ImageHelper::copyImageDataToBuffer");

    const angle::Format &imageFormat = mFormat->actualImageFormat();
    size_t sourceCopyAllocationSize  = sourceArea.width * sourceArea.height * sourceArea.depth *
                                      imageFormat.pixelBytes * layerCount;

    CommandBuffer *commandBuffer = nullptr;
    ANGLE_TRY(recordCommands(contextVk, &commandBuffer));

    //  http://anglebug.com/3949: Need to handle DS combined aspect, will require copying D & S
    //   separately. See ImageHelper::stageSubresourceUpdate for DS copy buff->image example.
    ASSERT(getAspectFlags() == VK_IMAGE_ASPECT_COLOR_BIT ||
           getAspectFlags() == VK_IMAGE_ASPECT_DEPTH_BIT ||
           getAspectFlags() == VK_IMAGE_ASPECT_STENCIL_BIT);

    // Transition the image to readable layout
    changeLayout(getAspectFlags(), ImageLayout::TransferSrc, commandBuffer);

    VkImageMemoryBarrier barrier            = {};
    barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image                           = mImage.getHandle();
    barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask     = getAspectFlags();
    barrier.subresourceRange.baseArrayLayer = baseLayer;
    barrier.subresourceRange.layerCount     = layerCount;
    barrier.subresourceRange.levelCount     = 1;
    barrier.subresourceRange.baseMipLevel   = static_cast<uint32_t>(sourceLevel);
    barrier.oldLayout                       = getCurrentLayout();
    barrier.newLayout                       = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask                   = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask                   = VK_ACCESS_TRANSFER_READ_BIT;

    commandBuffer->imageBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                &barrier);

    // Allocate staging buffer data
    ANGLE_TRY(allocateStagingMemory(contextVk, sourceCopyAllocationSize, outDataPtr, bufferOut,
                                    bufferOffsetOut, nullptr));

    VkBufferImageCopy region               = {};
    region.bufferOffset                    = *bufferOffsetOut;
    region.bufferRowLength                 = 0;
    region.bufferImageHeight               = 0;
    region.imageExtent.width               = sourceArea.width;
    region.imageExtent.height              = sourceArea.height;
    region.imageExtent.depth               = sourceArea.depth;
    region.imageOffset.x                   = sourceArea.x;
    region.imageOffset.y                   = sourceArea.y;
    region.imageOffset.z                   = sourceArea.z;
    region.imageSubresource.aspectMask     = getAspectFlags();
    region.imageSubresource.baseArrayLayer = baseLayer;
    region.imageSubresource.layerCount     = layerCount;
    region.imageSubresource.mipLevel       = static_cast<uint32_t>(sourceLevel);

    commandBuffer->copyImageToBuffer(mImage, getCurrentLayout(),
                                     (*bufferOut)->getBuffer().getHandle(), 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    commandBuffer->imageBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                &barrier);

    return angle::Result::Continue;
}

// static
angle::Result ImageHelper::GetReadPixelsParams(ContextVk *contextVk,
                                               const gl::PixelPackState &packState,
                                               gl::Buffer *packBuffer,
                                               GLenum format,
                                               GLenum type,
                                               const gl::Rectangle &area,
                                               const gl::Rectangle &clippedArea,
                                               PackPixelsParams *paramsOut,
                                               GLuint *skipBytesOut)
{
    const gl::InternalFormat &sizedFormatInfo = gl::GetInternalFormatInfo(format, type);

    GLuint outputPitch = 0;
    ANGLE_VK_CHECK_MATH(contextVk,
                        sizedFormatInfo.computeRowPitch(type, area.width, packState.alignment,
                                                        packState.rowLength, &outputPitch));
    ANGLE_VK_CHECK_MATH(contextVk, sizedFormatInfo.computeSkipBytes(type, outputPitch, 0, packState,
                                                                    false, skipBytesOut));

    *skipBytesOut += (clippedArea.x - area.x) * sizedFormatInfo.pixelBytes +
                     (clippedArea.y - area.y) * outputPitch;

    const angle::Format &angleFormat = GetFormatFromFormatType(format, type);

    *paramsOut = PackPixelsParams(clippedArea, angleFormat, outputPitch, packState.reverseRowOrder,
                                  packBuffer, 0);
    return angle::Result::Continue;
}

angle::Result ImageHelper::readPixelsForGetImage(ContextVk *contextVk,
                                                 const gl::PixelPackState &packState,
                                                 gl::Buffer *packBuffer,
                                                 uint32_t level,
                                                 uint32_t layer,
                                                 GLenum format,
                                                 GLenum type,
                                                 void *pixels)
{
    const angle::Format &angleFormat = GetFormatFromFormatType(format, type);

    // Depth/stencil readback is not yet implemented.
    // TODO(http://anglebug.com/4058): Depth/stencil readback.
    if (angleFormat.depthBits > 0 || angleFormat.stencilBits > 0)
    {
        UNIMPLEMENTED();
        return angle::Result::Continue;
    }

    PackPixelsParams params;
    GLuint outputSkipBytes = 0;

    uint32_t width  = std::max(1u, mExtents.width >> level);
    uint32_t height = std::max(1u, mExtents.height >> level);
    gl::Rectangle area(0, 0, width, height);

    ANGLE_TRY(GetReadPixelsParams(contextVk, packState, packBuffer, format, type, area, area,
                                  &params, &outputSkipBytes));

    // Use a temporary staging buffer. Could be optimized.
    vk::RendererScoped<vk::DynamicBuffer> stagingBuffer(contextVk->getRenderer());
    stagingBuffer.get().init(contextVk->getRenderer(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1,
                             kStagingBufferSize, true);

    return readPixels(contextVk, area, params, VK_IMAGE_ASPECT_COLOR_BIT, level, layer,
                      static_cast<uint8_t *>(pixels) + outputSkipBytes, &stagingBuffer.get());
}

angle::Result ImageHelper::readPixels(ContextVk *contextVk,
                                      const gl::Rectangle &area,
                                      const PackPixelsParams &packPixelsParams,
                                      VkImageAspectFlagBits copyAspectFlags,
                                      uint32_t level,
                                      uint32_t layer,
                                      void *pixels,
                                      DynamicBuffer *stagingBuffer)
{
    ANGLE_TRACE_EVENT0("gpu.angle", "ImageHelper::readPixels");

    RendererVk *renderer = contextVk->getRenderer();

    // Note that although we're reading from the image, we need to update the layout below.
    CommandBuffer *commandBuffer;
    ANGLE_TRY(recordCommands(contextVk, &commandBuffer));
    changeLayout(copyAspectFlags, ImageLayout::TransferSrc, commandBuffer);

    const angle::Format *readFormat = &mFormat->actualImageFormat();

    if (copyAspectFlags != VK_IMAGE_ASPECT_COLOR_BIT)
    {
        readFormat = &GetDepthStencilImageToBufferFormat(*readFormat, copyAspectFlags);
    }

    VkOffset3D srcOffset = {area.x, area.y, 0};

    VkImageSubresourceLayers srcSubresource = {};
    srcSubresource.aspectMask               = copyAspectFlags;
    srcSubresource.mipLevel                 = level;
    srcSubresource.baseArrayLayer           = layer;
    srcSubresource.layerCount               = 1;

    VkExtent3D srcExtent = {static_cast<uint32_t>(area.width), static_cast<uint32_t>(area.height),
                            1};

    if (mExtents.depth > 1)
    {
        // Depth > 1 means this is a 3D texture and we need special handling
        srcOffset.z                   = layer;
        srcSubresource.baseArrayLayer = 0;
    }

    // If the source image is multisampled, we need to resolve it into a temporary image before
    // performing a readback.
    bool isMultisampled = mSamples > 1;
    DeviceScoped<ImageHelper> resolvedImage(contextVk->getDevice());

    ImageHelper *src = this;

    if (isMultisampled)
    {
        ANGLE_TRY(resolvedImage.get().init2DStaging(
            contextVk, renderer->getMemoryProperties(), gl::Extents(area.width, area.height, 1),
            *mFormat, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1));
        resolvedImage.get().onGraphAccess(contextVk->getCommandGraph());

        // Note: resolve only works on color images (not depth/stencil).
        //
        // TODO: Currently, depth/stencil blit can perform a depth/stencil readback, but that code
        // path will be optimized away.  http://anglebug.com/3200
        ASSERT(copyAspectFlags == VK_IMAGE_ASPECT_COLOR_BIT);

        VkImageResolve resolveRegion                = {};
        resolveRegion.srcSubresource                = srcSubresource;
        resolveRegion.srcOffset                     = srcOffset;
        resolveRegion.dstSubresource.aspectMask     = copyAspectFlags;
        resolveRegion.dstSubresource.mipLevel       = 0;
        resolveRegion.dstSubresource.baseArrayLayer = 0;
        resolveRegion.dstSubresource.layerCount     = 1;
        resolveRegion.dstOffset                     = {};
        resolveRegion.extent                        = srcExtent;

        resolve(&resolvedImage.get(), resolveRegion, commandBuffer);

        resolvedImage.get().changeLayout(copyAspectFlags, ImageLayout::TransferSrc, commandBuffer);

        // Make the resolved image the target of buffer copy.
        src                           = &resolvedImage.get();
        level                         = 0;
        layer                         = 0;
        srcOffset                     = {0, 0, 0};
        srcSubresource.baseArrayLayer = 0;
        srcSubresource.layerCount     = 1;
        srcSubresource.mipLevel       = 0;
    }

    VkBuffer bufferHandle      = VK_NULL_HANDLE;
    uint8_t *readPixelBuffer   = nullptr;
    VkDeviceSize stagingOffset = 0;
    size_t allocationSize      = readFormat->pixelBytes * area.width * area.height;

    ANGLE_TRY(stagingBuffer->allocate(contextVk, allocationSize, &readPixelBuffer, &bufferHandle,
                                      &stagingOffset, nullptr));

    VkBufferImageCopy region = {};
    region.bufferImageHeight = srcExtent.height;
    region.bufferOffset      = stagingOffset;
    region.bufferRowLength   = srcExtent.width;
    region.imageExtent       = srcExtent;
    region.imageOffset       = srcOffset;
    region.imageSubresource  = srcSubresource;

    commandBuffer->copyImageToBuffer(src->getImage(), src->getCurrentLayout(), bufferHandle, 1,
                                     &region);

    // Triggers a full finish.
    // TODO(jmadill): Don't block on asynchronous readback.
    ANGLE_TRY(contextVk->finishImpl());

    // The buffer we copied to needs to be invalidated before we read from it because its not been
    // created with the host coherent bit.
    ANGLE_TRY(stagingBuffer->invalidate(contextVk));

    if (packPixelsParams.packBuffer)
    {
        // Must map the PBO in order to read its contents (and then unmap it later)
        BufferVk *packBufferVk = GetImpl(packPixelsParams.packBuffer);
        void *mapPtr           = nullptr;
        ANGLE_TRY(packBufferVk->mapImpl(contextVk, &mapPtr));
        uint8_t *dest = static_cast<uint8_t *>(mapPtr) + reinterpret_cast<ptrdiff_t>(pixels);
        PackPixels(packPixelsParams, *readFormat, area.width * readFormat->pixelBytes,
                   readPixelBuffer, static_cast<uint8_t *>(dest));
        packBufferVk->unmapImpl(contextVk);
    }
    else
    {
        PackPixels(packPixelsParams, *readFormat, area.width * readFormat->pixelBytes,
                   readPixelBuffer, static_cast<uint8_t *>(pixels));
    }

    return angle::Result::Continue;
}

// ImageHelper::SubresourceUpdate implementation
ImageHelper::SubresourceUpdate::SubresourceUpdate() : updateSource(UpdateSource::Buffer), buffer{}
{}

ImageHelper::SubresourceUpdate::SubresourceUpdate(BufferHelper *bufferHelperIn,
                                                  const VkBufferImageCopy &copyRegionIn)
    : updateSource(UpdateSource::Buffer), buffer{bufferHelperIn, copyRegionIn}
{}

ImageHelper::SubresourceUpdate::SubresourceUpdate(ImageHelper *imageIn,
                                                  const VkImageCopy &copyRegionIn)
    : updateSource(UpdateSource::Image), image{imageIn, copyRegionIn}
{}

ImageHelper::SubresourceUpdate::SubresourceUpdate(const VkClearValue &clearValue,
                                                  const gl::ImageIndex &imageIndex)
    : updateSource(UpdateSource::Clear)
{
    clear.value      = clearValue;
    clear.levelIndex = imageIndex.getLevelIndex();
    clear.layerIndex = imageIndex.hasLayer() ? imageIndex.getLayerIndex() : 0;
    clear.layerCount = imageIndex.getLayerCount();
}

ImageHelper::SubresourceUpdate::SubresourceUpdate(const SubresourceUpdate &other)
    : updateSource(other.updateSource)
{
    if (updateSource == UpdateSource::Clear)
    {
        clear = other.clear;
    }
    else if (updateSource == UpdateSource::Buffer)
    {
        buffer = other.buffer;
    }
    else
    {
        image = other.image;
    }
}

void ImageHelper::SubresourceUpdate::release(RendererVk *renderer)
{
    if (updateSource == UpdateSource::Image)
    {
        image.image->releaseImage(renderer);
        image.image->releaseStagingBuffer(renderer);
        SafeDelete(image.image);
    }
}

bool ImageHelper::SubresourceUpdate::isUpdateToLayerLevel(uint32_t layerIndex,
                                                          uint32_t levelIndex) const
{
    if (updateSource == UpdateSource::Clear)
    {
        return clear.levelIndex == levelIndex && clear.layerIndex == layerIndex;
    }

    const VkImageSubresourceLayers &dst = dstSubresource();
    return dst.baseArrayLayer == layerIndex && dst.mipLevel == levelIndex;
}

// FramebufferHelper implementation.
FramebufferHelper::FramebufferHelper() : CommandGraphResource(CommandGraphResourceType::Framebuffer)
{}

FramebufferHelper::~FramebufferHelper() = default;

angle::Result FramebufferHelper::init(ContextVk *contextVk,
                                      const VkFramebufferCreateInfo &createInfo)
{
    ANGLE_VK_TRY(contextVk, mFramebuffer.init(contextVk->getDevice(), createInfo));
    return angle::Result::Continue;
}

void FramebufferHelper::release(ContextVk *contextVk)
{
    contextVk->addGarbage(&mFramebuffer);
}

// ImageViewHelper implementation.
ImageViewHelper::ImageViewHelper()
{
    mUse.init();
}

ImageViewHelper::ImageViewHelper(ImageViewHelper &&other)
{
    std::swap(mReadImageView, other.mReadImageView);
    std::swap(mFetchImageView, other.mFetchImageView);
    std::swap(mStencilReadImageView, other.mStencilReadImageView);
    std::swap(mLevelDrawImageViews, other.mLevelDrawImageViews);
    std::swap(mLayerLevelDrawImageViews, other.mLayerLevelDrawImageViews);
}

ImageViewHelper::~ImageViewHelper()
{
    mUse.release();
}

void ImageViewHelper::release(RendererVk *renderer)
{
    std::vector<GarbageObject> garbage;

    garbage.emplace_back(GetGarbage(&mReadImageView));
    garbage.emplace_back(GetGarbage(&mFetchImageView));
    garbage.emplace_back(GetGarbage(&mStencilReadImageView));

    for (ImageView &imageView : mLevelDrawImageViews)
    {
        garbage.emplace_back(GetGarbage(&imageView));
    }
    mLevelDrawImageViews.clear();

    for (ImageViewVector &layerViews : mLayerLevelDrawImageViews)
    {
        for (ImageView &imageView : layerViews)
        {
            garbage.emplace_back(GetGarbage(&imageView));
        }
    }
    mLayerLevelDrawImageViews.clear();

    renderer->collectGarbage(std::move(mUse), std::move(garbage));

    // Ensure the resource use is always valid.
    mUse.init();
}

void ImageViewHelper::destroy(VkDevice device)
{
    mReadImageView.destroy(device);
    mFetchImageView.destroy(device);
    mStencilReadImageView.destroy(device);

    for (ImageView &imageView : mLevelDrawImageViews)
    {
        imageView.destroy(device);
    }
    mLevelDrawImageViews.clear();

    for (ImageViewVector &layerViews : mLayerLevelDrawImageViews)
    {
        for (ImageView &imageView : layerViews)
        {
            imageView.destroy(device);
        }
    }
    mLayerLevelDrawImageViews.clear();
}

angle::Result ImageViewHelper::initReadViews(ContextVk *contextVk,
                                             gl::TextureType viewType,
                                             const ImageHelper &image,
                                             const Format &format,
                                             const gl::SwizzleState &swizzleState,
                                             uint32_t baseLevel,
                                             uint32_t levelCount,
                                             uint32_t baseLayer,
                                             uint32_t layerCount)
{
    const VkImageAspectFlags aspectFlags = GetFormatAspectFlags(format.intendedFormat());
    if (HasBothDepthAndStencilAspects(aspectFlags))
    {
        ANGLE_TRY(image.initLayerImageView(contextVk, viewType, VK_IMAGE_ASPECT_DEPTH_BIT,
                                           swizzleState, &mReadImageView, baseLevel, levelCount,
                                           baseLayer, layerCount));
        ANGLE_TRY(image.initLayerImageView(contextVk, viewType, VK_IMAGE_ASPECT_STENCIL_BIT,
                                           swizzleState, &mStencilReadImageView, baseLevel,
                                           levelCount, baseLayer, layerCount));
    }
    else
    {
        ANGLE_TRY(image.initLayerImageView(contextVk, viewType, aspectFlags, swizzleState,
                                           &mReadImageView, baseLevel, levelCount, baseLayer,
                                           layerCount));
    }

    if (viewType == gl::TextureType::CubeMap || viewType == gl::TextureType::_2DArray ||
        viewType == gl::TextureType::_2DMultisampleArray)
    {
        gl::TextureType arrayType = Get2DTextureType(layerCount, image.getSamples());

        // TODO(http://anglebug.com/4004): SwizzleState incorrect for CopyTextureCHROMIUM.
        ANGLE_TRY(image.initLayerImageView(contextVk, arrayType, aspectFlags, swizzleState,
                                           &mFetchImageView, baseLevel, levelCount, baseLayer,
                                           layerCount));
    }

    return angle::Result::Continue;
}

angle::Result ImageViewHelper::getLevelDrawImageView(ContextVk *contextVk,
                                                     gl::TextureType viewType,
                                                     const ImageHelper &image,
                                                     uint32_t level,
                                                     uint32_t layer,
                                                     const ImageView **imageViewOut)
{
    onGraphAccess(contextVk->getCommandGraph());

    // TODO(http://anglebug.com/4008): Possibly incorrect level count.
    ImageView *imageView = GetLevelImageView(&mLevelDrawImageViews, level, 1);

    *imageViewOut = imageView;
    if (imageView->valid())
    {
        return angle::Result::Continue;
    }

    // Create the view.  Note that storage images are not affected by swizzle parameters.
    return image.initLayerImageView(contextVk, viewType, image.getAspectFlags(), gl::SwizzleState(),
                                    imageView, level, 1, layer, image.getLayerCount());
}

angle::Result ImageViewHelper::getLevelLayerDrawImageView(ContextVk *contextVk,
                                                          const ImageHelper &image,
                                                          uint32_t level,
                                                          uint32_t layer,
                                                          const ImageView **imageViewOut)
{
    ASSERT(image.valid());
    ASSERT(!image.getFormat().actualImageFormat().isBlock);

    onGraphAccess(contextVk->getCommandGraph());

    uint32_t layerCount = GetImageLayerCountForView(image);

    // Lazily allocate the storage for image views
    if (mLayerLevelDrawImageViews.empty())
    {
        mLayerLevelDrawImageViews.resize(layerCount);
    }
    ASSERT(mLayerLevelDrawImageViews.size() > layer);

    ImageView *imageView =
        GetLevelImageView(&mLayerLevelDrawImageViews[layer], level, image.getLevelCount());
    *imageViewOut = imageView;

    if (imageView->valid())
    {
        return angle::Result::Continue;
    }

    // Lazily allocate the image view itself.
    // Note that these views are specifically made to be used as color attachments, and therefore
    // don't have swizzle.
    gl::TextureType viewType = Get2DTextureType(1, image.getSamples());
    return image.initLayerImageView(contextVk, viewType, image.getAspectFlags(), gl::SwizzleState(),
                                    imageView, level, 1, layer, 1);
}

// SamplerHelper implementation.
SamplerHelper::SamplerHelper()
{
    mUse.init();
}

SamplerHelper::~SamplerHelper()
{
    mUse.release();
}

void SamplerHelper::release(RendererVk *renderer)
{
    renderer->collectGarbageAndReinit(&mUse, &mSampler);
}

// DispatchHelper implementation.
DispatchHelper::DispatchHelper() : CommandGraphResource(CommandGraphResourceType::Dispatcher) {}

DispatchHelper::~DispatchHelper() = default;

// ShaderProgramHelper implementation.
ShaderProgramHelper::ShaderProgramHelper() = default;

ShaderProgramHelper::~ShaderProgramHelper() = default;

bool ShaderProgramHelper::valid() const
{
    // This will need to be extended for compute shader support.
    return mShaders[gl::ShaderType::Vertex].valid();
}

void ShaderProgramHelper::destroy(VkDevice device)
{
    mGraphicsPipelines.destroy(device);
    mComputePipeline.destroy(device);
    for (BindingPointer<ShaderAndSerial> &shader : mShaders)
    {
        shader.reset();
    }
}

void ShaderProgramHelper::release(ContextVk *contextVk)
{
    mGraphicsPipelines.release(contextVk);
    contextVk->addGarbage(&mComputePipeline.get());
    for (BindingPointer<ShaderAndSerial> &shader : mShaders)
    {
        shader.reset();
    }
}

void ShaderProgramHelper::setShader(gl::ShaderType shaderType, RefCounted<ShaderAndSerial> *shader)
{
    mShaders[shaderType].set(shader);
}

angle::Result ShaderProgramHelper::getComputePipeline(Context *context,
                                                      const PipelineLayout &pipelineLayout,
                                                      PipelineAndSerial **pipelineOut)
{
    if (mComputePipeline.valid())
    {
        *pipelineOut = &mComputePipeline;
        return angle::Result::Continue;
    }

    RendererVk *renderer = context->getRenderer();

    VkPipelineShaderStageCreateInfo shaderStage = {};
    VkComputePipelineCreateInfo createInfo      = {};

    shaderStage.sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStage.flags               = 0;
    shaderStage.stage               = VK_SHADER_STAGE_COMPUTE_BIT;
    shaderStage.module              = mShaders[gl::ShaderType::Compute].get().get().getHandle();
    shaderStage.pName               = "main";
    shaderStage.pSpecializationInfo = nullptr;

    createInfo.sType              = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    createInfo.flags              = 0;
    createInfo.stage              = shaderStage;
    createInfo.layout             = pipelineLayout.getHandle();
    createInfo.basePipelineHandle = VK_NULL_HANDLE;
    createInfo.basePipelineIndex  = 0;

    PipelineCache *pipelineCache = nullptr;
    ANGLE_TRY(renderer->getPipelineCache(&pipelineCache));
    ANGLE_VK_TRY(context, mComputePipeline.get().initCompute(context->getDevice(), createInfo,
                                                             *pipelineCache));

    *pipelineOut = &mComputePipeline;
    return angle::Result::Continue;
}

}  // namespace vk
}  // namespace rx
