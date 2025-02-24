//
// Copyright 2019 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// mtl_resources.h:
//    Declares wrapper classes for Metal's MTLTexture and MTLBuffer.
//

#ifndef LIBANGLE_RENDERER_METAL_MTL_RESOURCES_H_
#define LIBANGLE_RENDERER_METAL_MTL_RESOURCES_H_

#import <Metal/Metal.h>

#include <atomic>
#include <memory>

#include "common/FastVector.h"
#include "common/MemoryBuffer.h"
#include "common/angleutils.h"
#include "libANGLE/Error.h"
#include "libANGLE/ImageIndex.h"
#include "libANGLE/angletypes.h"
#include "libANGLE/renderer/metal/mtl_common.h"
#include "libANGLE/renderer/metal/mtl_format_utils.h"

namespace rx
{

class ContextMtl;

namespace mtl
{

class CommandQueue;
class Resource;
class Texture;
class Buffer;

using ResourceRef    = std::shared_ptr<Resource>;
using TextureRef     = std::shared_ptr<Texture>;
using TextureWeakRef = std::weak_ptr<Texture>;
using BufferRef      = std::shared_ptr<Buffer>;
using BufferWeakRef  = std::weak_ptr<Buffer>;

class Resource : angle::NonCopyable
{
  public:
    virtual ~Resource() {}

    bool isBeingUsedByGPU(Context *context) const;

    void setUsedByCommandBufferWithQueueSerial(uint64_t serial, bool writing);

    const std::atomic<uint64_t> &getCommandBufferQueueSerial() const
    {
        return mUsageRef->cmdBufferQueueSerial;
    }

    // Flag indicate whether we should synchornize the content to CPU after GPU changed this
    // resource's content.
    bool isCPUReadMemDirty() const { return mUsageRef->cpuReadMemDirty; }
    void resetCPUReadMemDirty() { mUsageRef->cpuReadMemDirty = false; }

  protected:
    Resource();
    // Share the GPU usage ref with other resource
    Resource(Resource *other);

  private:
    struct UsageRef
    {
        // The id of the last command buffer that is using this resource.
        std::atomic<uint64_t> cmdBufferQueueSerial{0};

        // NOTE(hqle): resource dirty handle is not threadsafe.
        // This flag means the resource was issued to be modified by GPU, if CPU wants to read
        // its content, explicit synchornization call must be invoked.
        bool cpuReadMemDirty = false;
    };

    // One resource object might just be a view of another resource. For example, a texture 2d
    // object might be a view of one face of a cube texture object. Another example is one texture
    // object of size 2x2 might be a mipmap view of a texture object size 4x4. Thus, if one object
    // is being used by a command buffer, it means the other object is being used also. In this
    // case, the two objects must share the same UsageRef property.
    std::shared_ptr<UsageRef> mUsageRef;
};

class Texture final : public Resource,
                      public WrappedObject<id<MTLTexture>>,
                      public std::enable_shared_from_this<Texture>
{
  public:
    static angle::Result Make2DTexture(ContextMtl *context,
                                       const Format &format,
                                       uint32_t width,
                                       uint32_t height,
                                       uint32_t mips /** use zero to create full mipmaps chain */,
                                       bool renderTargetOnly,
                                       TextureRef *refOut);

    static angle::Result MakeCubeTexture(ContextMtl *context,
                                         const Format &format,
                                         uint32_t size,
                                         uint32_t mips /** use zero to create full mipmaps chain */,
                                         bool renderTargetOnly,
                                         TextureRef *refOut);

    static TextureRef MakeFromMetal(id<MTLTexture> metalTexture);

    void replaceRegion(ContextMtl *context,
                       MTLRegion region,
                       uint32_t mipmapLevel,
                       uint32_t slice,
                       const uint8_t *data,
                       size_t bytesPerRow);

    // read pixel data from slice 0
    void getBytes(ContextMtl *context,
                  size_t bytesPerRow,
                  MTLRegion region,
                  uint32_t mipmapLevel,
                  uint8_t *dataOut);

    // Create 2d view of a cube face
    TextureRef createFaceView(uint32_t face);

    MTLTextureType textureType() const;
    MTLPixelFormat pixelFormat() const;

    uint32_t mipmapLevels() const;

    uint32_t width(uint32_t level = 0) const;
    uint32_t height(uint32_t level = 0) const;

    gl::Extents size(uint32_t level = 0) const;
    gl::Extents size(const gl::ImageIndex &index) const;

    // For render target
    MTLColorWriteMask getColorWritableMask() const { return mColorWritableMask; }
    void setColorWritableMask(MTLColorWriteMask mask) { mColorWritableMask = mask; }

    // Change the wrapped metal object. Special case for swapchain image
    void set(id<MTLTexture> metalTexture);

  private:
    using ParentClass = WrappedObject<id<MTLTexture>>;

    Texture(id<MTLTexture> metalTexture);
    Texture(ContextMtl *context,
            MTLTextureDescriptor *desc,
            uint32_t mips,
            bool renderTargetOnly,
            bool supportTextureView);

    // Create a texture view
    Texture(Texture *original, MTLTextureType type, NSRange mipmapLevelRange, uint32_t slice);

    void syncContent(ContextMtl *context);

    MTLColorWriteMask mColorWritableMask = MTLColorWriteMaskAll;
};

class Buffer final : public Resource, public WrappedObject<id<MTLBuffer>>
{
  public:
    static angle::Result MakeBuffer(ContextMtl *context,
                                    size_t size,
                                    const uint8_t *data,
                                    BufferRef *bufferOut);

    angle::Result reset(ContextMtl *context, size_t size, const uint8_t *data);

    uint8_t *map(ContextMtl *context);
    void unmap(ContextMtl *context);

    size_t size() const;

  private:
    Buffer(ContextMtl *context, size_t size, const uint8_t *data);
};

}  // namespace mtl
}  // namespace rx

#endif /* LIBANGLE_RENDERER_METAL_MTL_RESOURCES_H_ */
