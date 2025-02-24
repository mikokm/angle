//
// Copyright 2019 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// ContextMtl.mm:
//    Implements the class methods for ContextMtl.
//

#include "libANGLE/renderer/metal/ContextMtl.h"

#include <TargetConditionals.h>

#include "common/debug.h"
#include "libANGLE/renderer/metal/BufferMtl.h"
#include "libANGLE/renderer/metal/CompilerMtl.h"
#include "libANGLE/renderer/metal/DisplayMtl.h"
#include "libANGLE/renderer/metal/FrameBufferMtl.h"
#include "libANGLE/renderer/metal/ProgramMtl.h"
#include "libANGLE/renderer/metal/RenderBufferMtl.h"
#include "libANGLE/renderer/metal/ShaderMtl.h"
#include "libANGLE/renderer/metal/TextureMtl.h"
#include "libANGLE/renderer/metal/VertexArrayMtl.h"
#include "libANGLE/renderer/metal/mtl_command_buffer.h"
#include "libANGLE/renderer/metal/mtl_format_utils.h"
#include "libANGLE/renderer/metal/mtl_utils.h"

namespace rx
{

namespace
{
#if TARGET_OS_OSX
// Unlimited triangle fan buffers
constexpr uint32_t kMaxTriFanLineLoopBuffersPerFrame = 0;
#else
// Allow up to 10 buffers for trifan/line loop generation without stalling the GPU.
constexpr uint32_t kMaxTriFanLineLoopBuffersPerFrame = 10;
#endif

angle::Result TriangleFanBoundCheck(ContextMtl *context, size_t numTris)
{
    bool indexCheck =
        (numTris > std::numeric_limits<unsigned int>::max() / (sizeof(unsigned int) * 3));
    ANGLE_CHECK(context, !indexCheck,
                "Failed to create a scratch index buffer for GL_TRIANGLE_FAN, "
                "too many indices required.",
                GL_OUT_OF_MEMORY);
    return angle::Result::Continue;
}

angle::Result GetTriangleFanIndicesCount(ContextMtl *context,
                                         GLsizei vetexCount,
                                         uint32_t *numElemsOut)
{
    size_t numTris = vetexCount - 2;
    ANGLE_TRY(TriangleFanBoundCheck(context, numTris));
    size_t numIndices = numTris * 3;
    ANGLE_CHECK(context, numIndices <= std::numeric_limits<uint32_t>::max(),
                "Failed to create a scratch index buffer for GL_TRIANGLE_FAN, "
                "too many indices required.",
                GL_OUT_OF_MEMORY);

    *numElemsOut = static_cast<uint32_t>(numIndices);
    return angle::Result::Continue;
}

angle::Result AllocateTriangleFanBufferFromPool(ContextMtl *context,
                                                GLsizei vertexCount,
                                                mtl::BufferPool *pool,
                                                mtl::BufferRef *bufferOut,
                                                uint32_t *offsetOut,
                                                uint32_t *numElemsOut)
{
    uint32_t numIndices;
    ANGLE_TRY(GetTriangleFanIndicesCount(context, vertexCount, &numIndices));

    size_t offset;
    pool->releaseInFlightBuffers(context);
    ANGLE_TRY(pool->allocate(context, numIndices * sizeof(uint32_t), nullptr, bufferOut, &offset,
                             nullptr));

    *offsetOut   = static_cast<uint32_t>(offset);
    *numElemsOut = numIndices;

    return angle::Result::Continue;
}
}  // namespace

ContextMtl::ContextMtl(const gl::State &state, gl::ErrorSet *errorSet, DisplayMtl *display)
    : ContextImpl(state, errorSet),
      mtl::Context(display),
      mCmdBuffer(&display->cmdQueue()),
      mRenderEncoder(&mCmdBuffer),
      mBlitEncoder(&mCmdBuffer),
      mComputeEncoder(&mCmdBuffer)
{}

ContextMtl::~ContextMtl() {}

angle::Result ContextMtl::initialize()
{
    mBlendDesc.reset();
    mDepthStencilDesc.reset();

    mTriFanIndexBuffer.initialize(this, 0, mtl::kBufferSettingOffsetAlignment,
                                  kMaxTriFanLineLoopBuffersPerFrame);
    mLineLoopIndexBuffer.initialize(this, 0, 2 * sizeof(uint32_t),
                                    kMaxTriFanLineLoopBuffersPerFrame);
    mLineLoopIndexBuffer.setAlwaysAllocateNewBuffer(true);

    return angle::Result::Continue;
}

void ContextMtl::onDestroy(const gl::Context *context)
{
    mTriFanIndexBuffer.destroy(this);
    mLineLoopIndexBuffer.destroy(this);
}

// Flush and finish.
angle::Result ContextMtl::flush(const gl::Context *context)
{
    flushCommandBufer();
    return angle::Result::Continue;
}
angle::Result ContextMtl::finish(const gl::Context *context)
{
    ANGLE_TRY(finishCommandBuffer());
    return angle::Result::Continue;
}

// Drawing methods.
angle::Result ContextMtl::drawTriFanArraysWithBaseVertex(const gl::Context *context,
                                                         GLint first,
                                                         GLsizei count)
{
    uint32_t genIndicesCount;
    ANGLE_TRY(GetTriangleFanIndicesCount(this, count, &genIndicesCount));

    size_t indexBufferSize = genIndicesCount * sizeof(uint32_t);
    // We can reuse the previously generated index buffer if it has more than enough indices
    // data already.
    if (mTriFanArraysIndexBuffer == nullptr || mTriFanArraysIndexBuffer->size() < indexBufferSize)
    {
        // Re-generate a new index buffer, which the first index will be zero.
        ANGLE_TRY(
            mtl::Buffer::MakeBuffer(this, indexBufferSize, nullptr, &mTriFanArraysIndexBuffer));
        ANGLE_TRY(getDisplay()->getUtils().generateTriFanBufferFromArrays(
            context, {0, static_cast<uint32_t>(count), mTriFanArraysIndexBuffer, 0}));
    }

    ANGLE_TRY(setupDraw(context, gl::PrimitiveMode::Triangles, first, genIndicesCount, 1,
                        gl::DrawElementsType::UnsignedInt, reinterpret_cast<const void *>(0)));

    // Draw with the zero starting index buffer, shift the vertex index using baseVertex instanced
    // draw:
    mRenderEncoder.drawIndexedBaseVertex(MTLPrimitiveTypeTriangle, genIndicesCount,
                                         MTLIndexTypeUInt32, mTriFanArraysIndexBuffer, 0, first);

    return angle::Result::Continue;
}
angle::Result ContextMtl::drawTriFanArraysLegacy(const gl::Context *context,
                                                 GLint first,
                                                 GLsizei count)
{
    mtl::BufferRef genIdxBuffer;
    uint32_t genIdxBufferOffset;
    uint32_t genIndicesCount;
    ANGLE_TRY(AllocateTriangleFanBufferFromPool(this, count, &mTriFanIndexBuffer, &genIdxBuffer,
                                                &genIdxBufferOffset, &genIndicesCount));
    ANGLE_TRY(getDisplay()->getUtils().generateTriFanBufferFromArrays(
        context, {static_cast<uint32_t>(first), static_cast<uint32_t>(count), genIdxBuffer,
                  genIdxBufferOffset}));

    ANGLE_TRY(setupDraw(context, gl::PrimitiveMode::Triangles, 0, genIndicesCount, 1,
                        gl::DrawElementsType::UnsignedInt,
                        reinterpret_cast<const void *>(genIdxBufferOffset)));

    mRenderEncoder.drawIndexed(MTLPrimitiveTypeTriangle, genIndicesCount, MTLIndexTypeUInt32,
                               genIdxBuffer, genIdxBufferOffset);

    return angle::Result::Continue;
}
angle::Result ContextMtl::drawTriFanArrays(const gl::Context *context, GLint first, GLsizei count)
{
    if (count <= 3)
    {
        return drawArrays(context, gl::PrimitiveMode::Triangles, first, count);
    }
    if (getDisplay()->getFeatures().hasBaseVertexInstancedDraw.enabled)
    {
        return drawTriFanArraysWithBaseVertex(context, first, count);
    }
    return drawTriFanArraysLegacy(context, first, count);
}
angle::Result ContextMtl::drawArrays(const gl::Context *context,
                                     gl::PrimitiveMode mode,
                                     GLint first,
                                     GLsizei count)
{
    if (mCullAllPolygons && gl::IsPolygonMode(mode))
    {
        return angle::Result::Continue;
    }

    if (mode == gl::PrimitiveMode::TriangleFan)
    {
        return drawTriFanArrays(context, first, count);
    }

    MTLPrimitiveType mtlType = mtl::GetPrimitiveType(mode);

    ANGLE_TRY(
        setupDraw(context, mode, first, count, 1, gl::DrawElementsType::InvalidEnum, nullptr));

    mRenderEncoder.draw(mtlType, first, count);

    return angle::Result::Continue;
}
angle::Result ContextMtl::drawArraysInstanced(const gl::Context *context,
                                              gl::PrimitiveMode mode,
                                              GLint first,
                                              GLsizei count,
                                              GLsizei instanceCount)
{
    // NOTE(hqle): ES 3.0
    UNIMPLEMENTED();
    return angle::Result::Stop;
}

angle::Result ContextMtl::drawArraysInstancedBaseInstance(const gl::Context *context,
                                                          gl::PrimitiveMode mode,
                                                          GLint first,
                                                          GLsizei count,
                                                          GLsizei instanceCount,
                                                          GLuint baseInstance)
{
    UNIMPLEMENTED();
    return angle::Result::Stop;
}

angle::Result ContextMtl::drawTriFanElements(const gl::Context *context,
                                             GLsizei count,
                                             gl::DrawElementsType type,
                                             const void *indices)
{
    if (count > 3)
    {
        mtl::BufferRef genIdxBuffer;
        uint32_t genIdxBufferOffset;
        uint32_t genIndicesCount;
        ANGLE_TRY(AllocateTriangleFanBufferFromPool(this, count, &mTriFanIndexBuffer, &genIdxBuffer,
                                                    &genIdxBufferOffset, &genIndicesCount));

        ANGLE_TRY(getDisplay()->getUtils().generateTriFanBufferFromElementsArray(
            context, {type, count, indices, genIdxBuffer, genIdxBufferOffset}));

        ANGLE_TRY(mTriFanIndexBuffer.commit(this));

        ANGLE_TRY(setupDraw(context, gl::PrimitiveMode::Triangles, 0, genIndicesCount, 1,
                            gl::DrawElementsType::UnsignedInt,
                            reinterpret_cast<const void *>(genIdxBufferOffset)));

        mRenderEncoder.drawIndexed(MTLPrimitiveTypeTriangle, genIndicesCount, MTLIndexTypeUInt32,
                                   genIdxBuffer, genIdxBufferOffset);

        return angle::Result::Continue;
    }  // if (count > 3)
    return drawElements(context, gl::PrimitiveMode::Triangles, count, type, indices);
}
angle::Result ContextMtl::drawElements(const gl::Context *context,
                                       gl::PrimitiveMode mode,
                                       GLsizei count,
                                       gl::DrawElementsType type,
                                       const void *indices)
{
    if (mCullAllPolygons && gl::IsPolygonMode(mode))
    {
        return angle::Result::Continue;
    }

    if (mode == gl::PrimitiveMode::TriangleFan)
    {
        return drawTriFanElements(context, count, type, indices);
    }

    mtl::BufferRef idxBuffer;
    size_t convertedOffset             = 0;
    gl::DrawElementsType convertedType = type;

    ANGLE_TRY(mVertexArray->getIndexBuffer(context, type, count, indices, &idxBuffer,
                                           &convertedOffset, &convertedType));

    ASSERT(idxBuffer);
    ASSERT((convertedOffset % mtl::kIndexBufferOffsetAlignment) == 0);

    ANGLE_TRY(setupDraw(context, mode, 0, count, 1, type, indices));

    MTLPrimitiveType mtlType = mtl::GetPrimitiveType(mode);

    MTLIndexType mtlIdxType = mtl::GetIndexType(convertedType);

    mRenderEncoder.drawIndexed(mtlType, count, mtlIdxType, idxBuffer, convertedOffset);

    return angle::Result::Continue;
}
angle::Result ContextMtl::drawElementsInstanced(const gl::Context *context,
                                                gl::PrimitiveMode mode,
                                                GLsizei count,
                                                gl::DrawElementsType type,
                                                const void *indices,
                                                GLsizei instanceCount)
{
    // NOTE(hqle): ES 3.0
    UNIMPLEMENTED();
    return angle::Result::Stop;
}
angle::Result ContextMtl::drawElementsInstancedBaseVertexBaseInstance(const gl::Context *context,
                                                                      gl::PrimitiveMode mode,
                                                                      GLsizei count,
                                                                      gl::DrawElementsType type,
                                                                      const void *indices,
                                                                      GLsizei instances,
                                                                      GLint baseVertex,
                                                                      GLuint baseInstance)
{
    UNIMPLEMENTED();
    return angle::Result::Stop;
}
angle::Result ContextMtl::drawRangeElements(const gl::Context *context,
                                            gl::PrimitiveMode mode,
                                            GLuint start,
                                            GLuint end,
                                            GLsizei count,
                                            gl::DrawElementsType type,
                                            const void *indices)
{
    // NOTE(hqle): ES 3.0
    UNIMPLEMENTED();
    return angle::Result::Stop;
}
angle::Result ContextMtl::drawArraysIndirect(const gl::Context *context,
                                             gl::PrimitiveMode mode,
                                             const void *indirect)
{
    // NOTE(hqle): ES 3.0
    UNIMPLEMENTED();
    return angle::Result::Stop;
}
angle::Result ContextMtl::drawElementsIndirect(const gl::Context *context,
                                               gl::PrimitiveMode mode,
                                               gl::DrawElementsType type,
                                               const void *indirect)
{
    // NOTE(hqle): ES 3.0
    UNIMPLEMENTED();
    return angle::Result::Stop;
}

// Device loss
gl::GraphicsResetStatus ContextMtl::getResetStatus()
{
    return gl::GraphicsResetStatus::NoError;
}

// Vendor and description strings.
std::string ContextMtl::getVendorString() const
{
    return getDisplay()->getVendorString();
}
std::string ContextMtl::getRendererDescription() const
{
    return getDisplay()->getRendererDescription();
}

// EXT_debug_marker
void ContextMtl::insertEventMarker(GLsizei length, const char *marker) {}
void ContextMtl::pushGroupMarker(GLsizei length, const char *marker) {}
void ContextMtl::popGroupMarker()
{
    // TODO(hqle
}

// KHR_debug
void ContextMtl::pushDebugGroup(GLenum source, GLuint id, const std::string &message) {}
void ContextMtl::popDebugGroup() {}

// State sync with dirty bits.
angle::Result ContextMtl::syncState(const gl::Context *context,
                                    const gl::State::DirtyBits &dirtyBits,
                                    const gl::State::DirtyBits &bitMask)
{
    const gl::State &glState = context->getState();

    for (size_t dirtyBit : dirtyBits)
    {
        switch (dirtyBit)
        {
            case gl::State::DIRTY_BIT_SCISSOR_TEST_ENABLED:
            case gl::State::DIRTY_BIT_SCISSOR:
                updateScissor(glState);
                break;
            case gl::State::DIRTY_BIT_VIEWPORT:
            {
                FramebufferMtl *framebufferMtl = mtl::GetImpl(glState.getDrawFramebuffer());
                updateViewport(framebufferMtl, glState.getViewport(), glState.getNearPlane(),
                               glState.getFarPlane());
                // Update the scissor, which will be constrained to the viewport
                updateScissor(glState);
                break;
            }
            case gl::State::DIRTY_BIT_DEPTH_RANGE:
                updateDepthRange(glState.getNearPlane(), glState.getFarPlane());
                break;
            case gl::State::DIRTY_BIT_BLEND_COLOR:
                mDirtyBits.set(DIRTY_BIT_BLEND_COLOR);
                break;
            case gl::State::DIRTY_BIT_BLEND_ENABLED:
                mBlendDesc.updateBlendEnabled(glState.getBlendState());
                invalidateRenderPipeline();
                break;
            case gl::State::DIRTY_BIT_BLEND_FUNCS:
                mBlendDesc.updateBlendFactors(glState.getBlendState());
                invalidateRenderPipeline();
                break;
            case gl::State::DIRTY_BIT_BLEND_EQUATIONS:
                mBlendDesc.updateBlendOps(glState.getBlendState());
                invalidateRenderPipeline();
                break;
            case gl::State::DIRTY_BIT_COLOR_MASK:
                mBlendDesc.updateWriteMask(glState.getBlendState());
                invalidateRenderPipeline();
                break;
            case gl::State::DIRTY_BIT_SAMPLE_ALPHA_TO_COVERAGE_ENABLED:
                // NOTE(hqle): MSAA support
                break;
            case gl::State::DIRTY_BIT_SAMPLE_COVERAGE_ENABLED:
                // NOTE(hqle): MSAA support
                break;
            case gl::State::DIRTY_BIT_SAMPLE_COVERAGE:
                // NOTE(hqle): MSAA support
                break;
            case gl::State::DIRTY_BIT_SAMPLE_MASK_ENABLED:
                // NOTE(hqle): MSAA support
                break;
            case gl::State::DIRTY_BIT_SAMPLE_MASK:
                // NOTE(hqle): MSAA support
                break;
            case gl::State::DIRTY_BIT_DEPTH_TEST_ENABLED:
                mDepthStencilDesc.updateDepthTestEnabled(glState.getDepthStencilState());
                mDirtyBits.set(DIRTY_BIT_DEPTH_STENCIL_DESC);
                break;
            case gl::State::DIRTY_BIT_DEPTH_FUNC:
                mDepthStencilDesc.updateDepthCompareFunc(glState.getDepthStencilState());
                mDirtyBits.set(DIRTY_BIT_DEPTH_STENCIL_DESC);
                break;
            case gl::State::DIRTY_BIT_DEPTH_MASK:
                mDepthStencilDesc.updateDepthWriteEnabled(glState.getDepthStencilState());
                mDirtyBits.set(DIRTY_BIT_DEPTH_STENCIL_DESC);
                break;
            case gl::State::DIRTY_BIT_STENCIL_TEST_ENABLED:
                mDepthStencilDesc.updateStencilTestEnabled(glState.getDepthStencilState());
                mDirtyBits.set(DIRTY_BIT_DEPTH_STENCIL_DESC);
                break;
            case gl::State::DIRTY_BIT_STENCIL_FUNCS_FRONT:
                mDepthStencilDesc.updateStencilFrontFuncs(glState.getDepthStencilState());
                mDirtyBits.set(DIRTY_BIT_DEPTH_STENCIL_DESC);
                mDirtyBits.set(DIRTY_BIT_STENCIL_REF);
                break;
            case gl::State::DIRTY_BIT_STENCIL_FUNCS_BACK:
                mDepthStencilDesc.updateStencilBackFuncs(glState.getDepthStencilState());
                mDirtyBits.set(DIRTY_BIT_DEPTH_STENCIL_DESC);
                mDirtyBits.set(DIRTY_BIT_STENCIL_REF);
                break;
            case gl::State::DIRTY_BIT_STENCIL_OPS_FRONT:
                mDepthStencilDesc.updateStencilFrontOps(glState.getDepthStencilState());
                mDirtyBits.set(DIRTY_BIT_DEPTH_STENCIL_DESC);
                break;
            case gl::State::DIRTY_BIT_STENCIL_OPS_BACK:
                mDepthStencilDesc.updateStencilBackOps(glState.getDepthStencilState());
                mDirtyBits.set(DIRTY_BIT_DEPTH_STENCIL_DESC);
                break;
            case gl::State::DIRTY_BIT_STENCIL_WRITEMASK_FRONT:
                mDepthStencilDesc.updateStencilFrontWriteMask(glState.getDepthStencilState());
                mDirtyBits.set(DIRTY_BIT_DEPTH_STENCIL_DESC);
                break;
            case gl::State::DIRTY_BIT_STENCIL_WRITEMASK_BACK:
                mDepthStencilDesc.updateStencilBackWriteMask(glState.getDepthStencilState());
                mDirtyBits.set(DIRTY_BIT_DEPTH_STENCIL_DESC);
                break;
            case gl::State::DIRTY_BIT_CULL_FACE_ENABLED:
            case gl::State::DIRTY_BIT_CULL_FACE:
                updateCullMode(glState);
                break;
            case gl::State::DIRTY_BIT_FRONT_FACE:
                updateFrontFace(glState);
                break;
            case gl::State::DIRTY_BIT_POLYGON_OFFSET_FILL_ENABLED:
            case gl::State::DIRTY_BIT_POLYGON_OFFSET:
                updateDepthBias(glState);
                break;
            case gl::State::DIRTY_BIT_RASTERIZER_DISCARD_ENABLED:
                // NOTE(hqle): ES 3.0 feature.
                break;
            case gl::State::DIRTY_BIT_LINE_WIDTH:
                // Do nothing
                break;
            case gl::State::DIRTY_BIT_PRIMITIVE_RESTART_ENABLED:
                // NOTE(hqle): ES 3.0 feature.
                break;
            case gl::State::DIRTY_BIT_CLEAR_COLOR:
                mClearColor.red   = glState.getColorClearValue().red;
                mClearColor.green = glState.getColorClearValue().green;
                mClearColor.blue  = glState.getColorClearValue().blue;
                mClearColor.alpha = glState.getColorClearValue().alpha;
                break;
            case gl::State::DIRTY_BIT_CLEAR_DEPTH:
                break;
            case gl::State::DIRTY_BIT_CLEAR_STENCIL:
                break;
            case gl::State::DIRTY_BIT_UNPACK_STATE:
                // This is a no-op, its only important to use the right unpack state when we do
                // setImage or setSubImage in TextureMtl, which is plumbed through the frontend call
                break;
            case gl::State::DIRTY_BIT_UNPACK_BUFFER_BINDING:
                break;
            case gl::State::DIRTY_BIT_PACK_STATE:
                // This is a no-op, its only important to use the right pack state when we do
                // call readPixels later on.
                break;
            case gl::State::DIRTY_BIT_PACK_BUFFER_BINDING:
                break;
            case gl::State::DIRTY_BIT_DITHER_ENABLED:
                break;
            case gl::State::DIRTY_BIT_GENERATE_MIPMAP_HINT:
                break;
            case gl::State::DIRTY_BIT_SHADER_DERIVATIVE_HINT:
                break;
            case gl::State::DIRTY_BIT_READ_FRAMEBUFFER_BINDING:
                break;
            case gl::State::DIRTY_BIT_DRAW_FRAMEBUFFER_BINDING:
                updateDrawFrameBufferBinding(context);
                break;
            case gl::State::DIRTY_BIT_RENDERBUFFER_BINDING:
                break;
            case gl::State::DIRTY_BIT_VERTEX_ARRAY_BINDING:
                updateVertexArray(context);
                break;
            case gl::State::DIRTY_BIT_DRAW_INDIRECT_BUFFER_BINDING:
                break;
            case gl::State::DIRTY_BIT_DISPATCH_INDIRECT_BUFFER_BINDING:
                break;
            case gl::State::DIRTY_BIT_PROGRAM_BINDING:
                mProgram = mtl::GetImpl(glState.getProgram());
                break;
            case gl::State::DIRTY_BIT_PROGRAM_EXECUTABLE:
                updateProgramExecutable(context);
                break;
            case gl::State::DIRTY_BIT_TEXTURE_BINDINGS:
                invalidateCurrentTextures();
                break;
            case gl::State::DIRTY_BIT_SAMPLER_BINDINGS:
                invalidateCurrentTextures();
                break;
            case gl::State::DIRTY_BIT_TRANSFORM_FEEDBACK_BINDING:
                // Nothing to do.
                break;
            case gl::State::DIRTY_BIT_SHADER_STORAGE_BUFFER_BINDING:
                // NOTE(hqle): ES 3.0 feature.
                break;
            case gl::State::DIRTY_BIT_UNIFORM_BUFFER_BINDINGS:
                // NOTE(hqle): ES 3.0 feature.
                break;
            case gl::State::DIRTY_BIT_ATOMIC_COUNTER_BUFFER_BINDING:
                break;
            case gl::State::DIRTY_BIT_IMAGE_BINDINGS:
                break;
            case gl::State::DIRTY_BIT_MULTISAMPLING:
                // NOTE(hqle): MSAA feature.
                break;
            case gl::State::DIRTY_BIT_SAMPLE_ALPHA_TO_ONE:
                // NOTE(hqle): this is part of EXT_multisample_compatibility.
                // NOTE(hqle): MSAA feature.
                break;
            case gl::State::DIRTY_BIT_COVERAGE_MODULATION:
                break;
            case gl::State::DIRTY_BIT_PATH_RENDERING:
                break;
            case gl::State::DIRTY_BIT_FRAMEBUFFER_SRGB:
                break;
            case gl::State::DIRTY_BIT_CURRENT_VALUES:
            {
                invalidateDefaultAttributes(glState.getAndResetDirtyCurrentValues());
                break;
            }
            case gl::State::DIRTY_BIT_PROVOKING_VERTEX:
                break;
            default:
                UNREACHABLE();
                break;
        }
    }

    return angle::Result::Continue;
}

// Disjoint timer queries
GLint ContextMtl::getGPUDisjoint()
{
    UNIMPLEMENTED();
    return 0;
}
GLint64 ContextMtl::getTimestamp()
{
    UNIMPLEMENTED();
    return 0;
}

// Context switching
angle::Result ContextMtl::onMakeCurrent(const gl::Context *context)
{
    invalidateState(context);
    return angle::Result::Continue;
}
angle::Result ContextMtl::onUnMakeCurrent(const gl::Context *context)
{
    flushCommandBufer();
    return angle::Result::Continue;
}

// Native capabilities, unmodified by gl::Context.
gl::Caps ContextMtl::getNativeCaps() const
{
    return getDisplay()->getNativeCaps();
}
const gl::TextureCapsMap &ContextMtl::getNativeTextureCaps() const
{
    return getDisplay()->getNativeTextureCaps();
}
const gl::Extensions &ContextMtl::getNativeExtensions() const
{
    return getDisplay()->getNativeExtensions();
}
const gl::Limitations &ContextMtl::getNativeLimitations() const
{
    return getDisplay()->getNativeLimitations();
}

// Shader creation
CompilerImpl *ContextMtl::createCompiler()
{
    return new CompilerMtl();
}
ShaderImpl *ContextMtl::createShader(const gl::ShaderState &state)
{
    return new ShaderMtl(state);
}
ProgramImpl *ContextMtl::createProgram(const gl::ProgramState &state)
{
    return new ProgramMtl(state);
}

// Framebuffer creation
FramebufferImpl *ContextMtl::createFramebuffer(const gl::FramebufferState &state)
{
    return new FramebufferMtl(state, false, false);
}

// Texture creation
TextureImpl *ContextMtl::createTexture(const gl::TextureState &state)
{
    return new TextureMtl(state);
}

// Renderbuffer creation
RenderbufferImpl *ContextMtl::createRenderbuffer(const gl::RenderbufferState &state)
{
    return new RenderbufferMtl(state);
}

// Buffer creation
BufferImpl *ContextMtl::createBuffer(const gl::BufferState &state)
{
    return new BufferMtl(state);
}

// Vertex Array creation
VertexArrayImpl *ContextMtl::createVertexArray(const gl::VertexArrayState &state)
{
    return new VertexArrayMtl(state, this);
}

// Query and Fence creation
QueryImpl *ContextMtl::createQuery(gl::QueryType type)
{
    // NOTE(hqle): ES 3.0
    UNIMPLEMENTED();
    return nullptr;
}
FenceNVImpl *ContextMtl::createFenceNV()
{
    UNIMPLEMENTED();
    return nullptr;
}
SyncImpl *ContextMtl::createSync()
{
    UNIMPLEMENTED();
    return nullptr;
}

// Transform Feedback creation
TransformFeedbackImpl *ContextMtl::createTransformFeedback(const gl::TransformFeedbackState &state)
{
    // NOTE(hqle): ES 3.0
    UNIMPLEMENTED();
    return nullptr;
}

// Sampler object creation
SamplerImpl *ContextMtl::createSampler(const gl::SamplerState &state)
{
    // NOTE(hqle): ES 3.0
    UNIMPLEMENTED();
    return nullptr;
}

// Program Pipeline object creation
ProgramPipelineImpl *ContextMtl::createProgramPipeline(const gl::ProgramPipelineState &data)
{
    // NOTE(hqle): ES 3.0
    UNIMPLEMENTED();
    return nullptr;
}

// Path object creation
std::vector<PathImpl *> ContextMtl::createPaths(GLsizei)
{
    UNIMPLEMENTED();
    return std::vector<PathImpl *>();
}

// Memory object creation.
MemoryObjectImpl *ContextMtl::createMemoryObject()
{
    UNIMPLEMENTED();
    return nullptr;
}

// Semaphore creation.
SemaphoreImpl *ContextMtl::createSemaphore()
{
    UNIMPLEMENTED();
    return nullptr;
}

OverlayImpl *ContextMtl::createOverlay(const gl::OverlayState &state)
{
    UNIMPLEMENTED();
    return nullptr;
}

angle::Result ContextMtl::dispatchCompute(const gl::Context *context,
                                          GLuint numGroupsX,
                                          GLuint numGroupsY,
                                          GLuint numGroupsZ)
{
    // NOTE(hqle): ES 3.0
    UNIMPLEMENTED();
    return angle::Result::Stop;
}
angle::Result ContextMtl::dispatchComputeIndirect(const gl::Context *context, GLintptr indirect)
{
    // NOTE(hqle): ES 3.0
    UNIMPLEMENTED();
    return angle::Result::Stop;
}

angle::Result ContextMtl::memoryBarrier(const gl::Context *context, GLbitfield barriers)
{
    // NOTE(hqle): ES 3.0
    UNIMPLEMENTED();
    return angle::Result::Stop;
}
angle::Result ContextMtl::memoryBarrierByRegion(const gl::Context *context, GLbitfield barriers)
{
    // NOTE(hqle): ES 3.0
    UNIMPLEMENTED();
    return angle::Result::Stop;
}

// override mtl::ErrorHandler
void ContextMtl::handleError(GLenum glErrorCode,
                             const char *file,
                             const char *function,
                             unsigned int line)
{
    std::stringstream errorStream;
    errorStream << "Metal backend encountered an error. Code=" << glErrorCode << ".";

    mErrors->handleError(glErrorCode, errorStream.str().c_str(), file, function, line);
}

void ContextMtl::handleError(NSError *nserror,
                             const char *file,
                             const char *function,
                             unsigned int line)
{
    if (!nserror)
    {
        return;
    }

    std::stringstream errorStream;
    errorStream << "Metal backend encountered an error: \n"
                << nserror.localizedDescription.UTF8String;

    mErrors->handleError(GL_INVALID_OPERATION, errorStream.str().c_str(), file, function, line);
}

void ContextMtl::invalidateState(const gl::Context *context)
{
    mDirtyBits.set();

    invalidateDefaultAttributes(context->getStateCache().getActiveDefaultAttribsMask());
}

void ContextMtl::invalidateDefaultAttribute(size_t attribIndex)
{
    mDirtyDefaultAttribsMask.set(attribIndex);
    mDirtyBits.set(DIRTY_BIT_DEFAULT_ATTRIBS);
}

void ContextMtl::invalidateDefaultAttributes(const gl::AttributesMask &dirtyMask)
{
    if (dirtyMask.any())
    {
        mDirtyDefaultAttribsMask |= dirtyMask;
        mDirtyBits.set(DIRTY_BIT_DEFAULT_ATTRIBS);
    }
}

void ContextMtl::invalidateCurrentTextures()
{
    mDirtyBits.set(DIRTY_BIT_TEXTURES);
}

void ContextMtl::invalidateDriverUniforms()
{
    mDirtyBits.set(DIRTY_BIT_DRIVER_UNIFORMS);
}

void ContextMtl::invalidateRenderPipeline()
{
    mDirtyBits.set(DIRTY_BIT_RENDER_PIPELINE);
}

const MTLClearColor &ContextMtl::getClearColorValue() const
{
    return mClearColor;
}
MTLColorWriteMask ContextMtl::getColorMask() const
{
    return mBlendDesc.writeMask;
}
float ContextMtl::getClearDepthValue() const
{
    return getState().getDepthClearValue();
}
uint32_t ContextMtl::getClearStencilValue() const
{
    return static_cast<uint32_t>(getState().getStencilClearValue());
}
uint32_t ContextMtl::getStencilMask() const
{
    return getState().getDepthStencilState().stencilWritemask;
}

bool ContextMtl::isDepthWriteEnabled() const
{
    return mDepthStencilDesc.depthWriteEnabled;
}

const mtl::Format &ContextMtl::getPixelFormat(angle::FormatID angleFormatId) const
{
    return getDisplay()->getPixelFormat(angleFormatId);
}

// See mtl::FormatTable::getVertexFormat()
const mtl::VertexFormat &ContextMtl::getVertexFormat(angle::FormatID angleFormatId,
                                                     bool tightlyPacked) const
{
    return getDisplay()->getVertexFormat(angleFormatId, tightlyPacked);
}

void ContextMtl::endEncoding(mtl::RenderCommandEncoder *encoder)
{
    encoder->endEncoding();
}

void ContextMtl::endEncoding(bool forceSaveRenderPassContent)
{
    if (mRenderEncoder.valid())
    {
        if (forceSaveRenderPassContent)
        {
            // Save the work in progress.
            mRenderEncoder.setColorStoreAction(MTLStoreActionStore);
            mRenderEncoder.setDepthStencilStoreAction(MTLStoreActionStore, MTLStoreActionStore);
        }

        mRenderEncoder.endEncoding();
    }

    if (mBlitEncoder.valid())
    {
        mBlitEncoder.endEncoding();
    }

    if (mComputeEncoder.valid())
    {
        mComputeEncoder.endEncoding();
    }
}

void ContextMtl::flushCommandBufer()
{
    if (!mCmdBuffer.valid())
    {
        return;
    }

    endEncoding(true);
    mCmdBuffer.commit();
}

void ContextMtl::present(const gl::Context *context, id<CAMetalDrawable> presentationDrawable)
{
    ensureCommandBufferValid();

    if (hasStartedRenderPass(mDrawFramebuffer))
    {
        mDrawFramebuffer->onFinishedDrawingToFrameBuffer(context, &mRenderEncoder);
    }

    endEncoding(false);
    mCmdBuffer.present(presentationDrawable);
    mCmdBuffer.commit();
}

angle::Result ContextMtl::finishCommandBuffer()
{
    flushCommandBufer();

    if (mCmdBuffer.valid())
    {
        mCmdBuffer.finish();
    }

    return angle::Result::Continue;
}

bool ContextMtl::hasStartedRenderPass(const mtl::RenderPassDesc &desc)
{
    return mRenderEncoder.valid() &&
           mRenderEncoder.renderPassDesc().equalIgnoreLoadStoreOptions(desc);
}

bool ContextMtl::hasStartedRenderPass(FramebufferMtl *framebuffer)
{
    return framebuffer && hasStartedRenderPass(framebuffer->getRenderPassDesc(this));
}

// Get current render encoder
mtl::RenderCommandEncoder *ContextMtl::getRenderCommandEncoder()
{
    if (!mRenderEncoder.valid())
    {
        return nullptr;
    }

    return &mRenderEncoder;
}

mtl::RenderCommandEncoder *ContextMtl::getCurrentFramebufferRenderCommandEncoder()
{
    if (!mDrawFramebuffer)
    {
        return nullptr;
    }

    return getRenderCommandEncoder(mDrawFramebuffer->getRenderPassDesc(this));
}

mtl::RenderCommandEncoder *ContextMtl::getRenderCommandEncoder(const mtl::RenderPassDesc &desc)
{
    if (hasStartedRenderPass(desc))
    {
        return &mRenderEncoder;
    }

    endEncoding(false);

    ensureCommandBufferValid();

    // Need to re-apply everything on next draw call.
    mDirtyBits.set();

    return &mRenderEncoder.restart(desc);
}

// Utilities to quickly create render command enconder to a specific texture:
// The previous content of texture will be loaded if clearColor is not provided
mtl::RenderCommandEncoder *ContextMtl::getRenderCommandEncoder(
    const mtl::TextureRef &textureTarget,
    const gl::ImageIndex &index,
    const Optional<MTLClearColor> &clearColor)
{
    ASSERT(textureTarget && textureTarget->valid());

    mtl::RenderPassDesc rpDesc;

    rpDesc.colorAttachments[0].texture = textureTarget;
    rpDesc.colorAttachments[0].level   = index.getLevelIndex();
    rpDesc.colorAttachments[0].slice   = index.hasLayer() ? index.getLayerIndex() : 0;
    rpDesc.numColorAttachments         = 1;

    if (clearColor.valid())
    {
        rpDesc.colorAttachments[0].loadAction = MTLLoadActionClear;
        rpDesc.colorAttachments[0].clearColor =
            mtl::EmulatedAlphaClearColor(clearColor.value(), textureTarget->getColorWritableMask());
    }

    return getRenderCommandEncoder(rpDesc);
}
// The previous content of texture will be loaded
mtl::RenderCommandEncoder *ContextMtl::getRenderCommandEncoder(const mtl::TextureRef &textureTarget,
                                                               const gl::ImageIndex &index)
{
    return getRenderCommandEncoder(textureTarget, index, Optional<MTLClearColor>());
}

mtl::BlitCommandEncoder *ContextMtl::getBlitCommandEncoder()
{
    if (mBlitEncoder.valid())
    {
        return &mBlitEncoder;
    }

    endEncoding(true);

    ensureCommandBufferValid();

    return &mBlitEncoder.restart();
}

mtl::ComputeCommandEncoder *ContextMtl::getComputeCommandEncoder()
{
    if (mComputeEncoder.valid())
    {
        return &mComputeEncoder;
    }

    endEncoding(true);

    ensureCommandBufferValid();

    return &mComputeEncoder.restart();
}

void ContextMtl::ensureCommandBufferValid()
{
    if (!mCmdBuffer.valid())
    {
        mCmdBuffer.restart();
    }

    ASSERT(mCmdBuffer.valid());
}

void ContextMtl::updateViewport(FramebufferMtl *framebufferMtl,
                                const gl::Rectangle &viewport,
                                float nearPlane,
                                float farPlane)
{
    mViewport = mtl::GetViewport(viewport, framebufferMtl->getState().getDimensions().height,
                                 framebufferMtl->flipY(), nearPlane, farPlane);
    mDirtyBits.set(DIRTY_BIT_VIEWPORT);

    invalidateDriverUniforms();
}

void ContextMtl::updateDepthRange(float nearPlane, float farPlane)
{
    mViewport.znear = nearPlane;
    mViewport.zfar  = farPlane;
    mDirtyBits.set(DIRTY_BIT_VIEWPORT);

    invalidateDriverUniforms();
}

void ContextMtl::updateScissor(const gl::State &glState)
{
    FramebufferMtl *framebufferMtl = mtl::GetImpl(glState.getDrawFramebuffer());
    gl::Rectangle renderArea       = framebufferMtl->getCompleteRenderArea();

    ANGLE_MTL_LOG("renderArea = %d,%d,%d,%d", renderArea.x, renderArea.y, renderArea.width,
                  renderArea.height);

    // Clip the render area to the viewport.
    gl::Rectangle viewportClippedRenderArea;
    gl::ClipRectangle(renderArea, glState.getViewport(), &viewportClippedRenderArea);

    gl::Rectangle scissoredArea = ClipRectToScissor(getState(), viewportClippedRenderArea, false);
    if (framebufferMtl->flipY())
    {
        scissoredArea.y = renderArea.height - scissoredArea.y - scissoredArea.height;
    }

    ANGLE_MTL_LOG("scissoredArea = %d,%d,%d,%d", scissoredArea.x, scissoredArea.y,
                  scissoredArea.width, scissoredArea.height);

    mScissorRect = mtl::GetScissorRect(scissoredArea);
    mDirtyBits.set(DIRTY_BIT_SCISSOR);
}

void ContextMtl::updateCullMode(const gl::State &glState)
{
    const gl::RasterizerState &rasterState = glState.getRasterizerState();

    mCullAllPolygons = false;
    if (!rasterState.cullFace)
    {
        mCullMode = MTLCullModeNone;
    }
    else
    {
        switch (rasterState.cullMode)
        {
            case gl::CullFaceMode::Back:
                mCullMode = MTLCullModeBack;
                break;
            case gl::CullFaceMode::Front:
                mCullMode = MTLCullModeFront;
                break;
            case gl::CullFaceMode::FrontAndBack:
                mCullAllPolygons = true;
                break;
            default:
                UNREACHABLE();
                break;
        }
    }

    mDirtyBits.set(DIRTY_BIT_CULL_MODE);
}

void ContextMtl::updateFrontFace(const gl::State &glState)
{
    FramebufferMtl *framebufferMtl = mtl::GetImpl(glState.getDrawFramebuffer());
    mWinding =
        mtl::GetFontfaceWinding(glState.getRasterizerState().frontFace, !framebufferMtl->flipY());
    mDirtyBits.set(DIRTY_BIT_WINDING);
}

void ContextMtl::updateDepthBias(const gl::State &glState)
{
    mDirtyBits.set(DIRTY_BIT_DEPTH_BIAS);
}

void ContextMtl::updateDrawFrameBufferBinding(const gl::Context *context)
{
    const gl::State &glState = getState();

    auto oldFrameBuffer = mDrawFramebuffer;

    mDrawFramebuffer = mtl::GetImpl(glState.getDrawFramebuffer());

    if (oldFrameBuffer && hasStartedRenderPass(oldFrameBuffer->getRenderPassDesc(this)))
    {
        oldFrameBuffer->onFinishedDrawingToFrameBuffer(context, &mRenderEncoder);
    }

    onDrawFrameBufferChange(context, mDrawFramebuffer);
}

void ContextMtl::onDrawFrameBufferChange(const gl::Context *context, FramebufferMtl *framebuffer)
{
    const gl::State &glState = getState();

    mDirtyBits.set(DIRTY_BIT_DRAW_FRAMEBUFFER);

    updateViewport(framebuffer, glState.getViewport(), glState.getNearPlane(),
                   glState.getFarPlane());
    updateFrontFace(glState);
    updateScissor(glState);

    // Need to re-apply state to RenderCommandEncoder
    invalidateState(context);
}

void ContextMtl::updateProgramExecutable(const gl::Context *context)
{
    // Need to rebind textures
    invalidateCurrentTextures();
    // Need to re-upload default attributes
    invalidateDefaultAttributes(context->getStateCache().getActiveDefaultAttribsMask());
    // Render pipeline need to be re-applied
    invalidateRenderPipeline();
}

void ContextMtl::updateVertexArray(const gl::Context *context)
{
    const gl::State &glState = getState();
    mVertexArray             = mtl::GetImpl(glState.getVertexArray());
    invalidateDefaultAttributes(context->getStateCache().getActiveDefaultAttribsMask());
    invalidateRenderPipeline();
}

angle::Result ContextMtl::updateDefaultAttribute(size_t attribIndex)
{
    const gl::State &glState = mState;
    const gl::VertexAttribCurrentValueData &defaultValue =
        glState.getVertexAttribCurrentValues()[attribIndex];

    constexpr size_t kDefaultGLAttributeValueSize =
        sizeof(gl::VertexAttribCurrentValueData::Values);

    static_assert(kDefaultGLAttributeValueSize == mtl::kDefaultAttributeSize,
                  "Unexpected default attribute size");
    memcpy(mDefaultAttributes[attribIndex].values, &defaultValue.Values,
           mtl::kDefaultAttributeSize);

    return angle::Result::Continue;
}

angle::Result ContextMtl::setupDraw(const gl::Context *context,
                                    gl::PrimitiveMode mode,
                                    GLint firstVertex,
                                    GLsizei vertexOrIndexCount,
                                    GLsizei instanceCount,
                                    gl::DrawElementsType indexTypeOrNone,
                                    const void *indices)
{
    ASSERT(mProgram);

    mtl::BufferRef lineLoopLastSegmentIndexBuffer;
    if (mode == gl::PrimitiveMode::LineLoop)
    {
        // Generate line loop last segment before render command encoder is created
        ANGLE_TRY(genLineLoopLastSegment(context, firstVertex, vertexOrIndexCount, instanceCount,
                                         indexTypeOrNone, indices,
                                         &lineLoopLastSegmentIndexBuffer));
    }

    // Must be called before the command buffer is started.
    if (context->getStateCache().hasAnyActiveClientAttrib())
    {
        ANGLE_TRY(mVertexArray->updateClientAttribs(context, firstVertex, vertexOrIndexCount,
                                                    instanceCount, indexTypeOrNone, indices));
    }

    if (!mRenderEncoder.valid())
    {
        // re-apply everything
        invalidateState(context);
    }

    if (mDirtyBits.test(DIRTY_BIT_DRAW_FRAMEBUFFER))
    {
        // Start new render command encoder
        const mtl::RenderPassDesc &rpDesc = mDrawFramebuffer->getRenderPassDesc(this);
        ANGLE_MTL_TRY(this, getRenderCommandEncoder(rpDesc));

        // re-apply everything
        invalidateState(context);
    }

    Optional<mtl::RenderPipelineDesc> changedPipelineDesc;
    ANGLE_TRY(checkIfPipelineChanged(context, mode, &changedPipelineDesc));

    bool textureChanged = false;
    for (size_t bit : mDirtyBits)
    {
        switch (bit)
        {
            case DIRTY_BIT_DEFAULT_ATTRIBS:
                ANGLE_TRY(handleDirtyDefaultAttribs(context));
                break;
            case DIRTY_BIT_DRIVER_UNIFORMS:
                ANGLE_TRY(handleDirtyDriverUniforms(context));
                break;
            case DIRTY_BIT_TEXTURES:
                textureChanged = true;
                break;
            case DIRTY_BIT_DEPTH_STENCIL_DESC:
                ANGLE_TRY(handleDirtyDepthStencilState(context));
                break;
            case DIRTY_BIT_DEPTH_BIAS:
                ANGLE_TRY(handleDirtyDepthBias(context));
                break;
            case DIRTY_BIT_STENCIL_REF:
                mRenderEncoder.setStencilRefVals(mState.getStencilRef(),
                                                 mState.getStencilBackRef());
                break;
            case DIRTY_BIT_BLEND_COLOR:
                mRenderEncoder.setBlendColor(
                    mState.getBlendColor().red, mState.getBlendColor().green,
                    mState.getBlendColor().blue, mState.getBlendColor().alpha);
                break;
            case DIRTY_BIT_VIEWPORT:
                mRenderEncoder.setViewport(mViewport);
                break;
            case DIRTY_BIT_SCISSOR:
                mRenderEncoder.setScissorRect(mScissorRect);
                break;
            case DIRTY_BIT_CULL_MODE:
                mRenderEncoder.setCullMode(mCullMode);
                break;
            case DIRTY_BIT_WINDING:
                mRenderEncoder.setFrontFacingWinding(mWinding);
                break;
        }

        mDirtyBits.reset(bit);
    }

    ANGLE_TRY(mProgram->setupDraw(context, &mRenderEncoder, changedPipelineDesc, textureChanged));

    if (mode == gl::PrimitiveMode::LineLoop)
    {
        // Draw last segment of line loop here
        mRenderEncoder.drawIndexed(MTLPrimitiveTypeLine, 2, MTLIndexTypeUInt32,
                                   lineLoopLastSegmentIndexBuffer, 0);
    }

    return angle::Result::Continue;
}

angle::Result ContextMtl::genLineLoopLastSegment(const gl::Context *context,
                                                 GLint firstVertex,
                                                 GLsizei vertexOrIndexCount,
                                                 GLsizei instanceCount,
                                                 gl::DrawElementsType indexTypeOrNone,
                                                 const void *indices,
                                                 mtl::BufferRef *lastSegmentIndexBufferOut)
{
    ASSERT(instanceCount == 1);

    mLineLoopIndexBuffer.releaseInFlightBuffers(this);

    mtl::BufferRef newBuffer;
    ANGLE_TRY(mLineLoopIndexBuffer.allocate(this, 2 * sizeof(uint32_t), nullptr, &newBuffer,
                                            nullptr, nullptr));

    if (indexTypeOrNone == gl::DrawElementsType::InvalidEnum)
    {
        ANGLE_TRY(getDisplay()->getUtils().generateLineLoopLastSegment(
            context, firstVertex, firstVertex + vertexOrIndexCount - 1, newBuffer, 0));
    }
    else
    {
        // NOTE(hqle): Support drawRangeElements & instanced draw, which means firstVertex has to be
        // taken into account
        ASSERT(firstVertex == 0);
        ANGLE_TRY(getDisplay()->getUtils().generateLineLoopLastSegmentFromElementsArray(
            context, {indexTypeOrNone, vertexOrIndexCount, indices, newBuffer, 0}));
    }

    ANGLE_TRY(mLineLoopIndexBuffer.commit(this));

    *lastSegmentIndexBufferOut = newBuffer;

    return angle::Result::Continue;
}

angle::Result ContextMtl::handleDirtyDefaultAttribs(const gl::Context *context)
{
    for (size_t attribIndex : mDirtyDefaultAttribsMask)
    {
        ANGLE_TRY(updateDefaultAttribute(attribIndex));
    }

    ASSERT(mRenderEncoder.valid());
    mRenderEncoder.setFragmentData(mDefaultAttributes, mtl::kDefaultAttribsBindingIndex);
    mRenderEncoder.setVertexData(mDefaultAttributes, mtl::kDefaultAttribsBindingIndex);

    mDirtyDefaultAttribsMask.reset();
    mDirtyBits.reset(DIRTY_BIT_DEFAULT_ATTRIBS);
    return angle::Result::Continue;
}

angle::Result ContextMtl::handleDirtyDriverUniforms(const gl::Context *context)
{
    const gl::Rectangle &glViewport = mState.getViewport();

    float depthRangeNear = mState.getNearPlane();
    float depthRangeFar  = mState.getFarPlane();
    float depthRangeDiff = depthRangeFar - depthRangeNear;

    mDriverUniforms.viewport[0] = glViewport.x;
    mDriverUniforms.viewport[1] = glViewport.y;
    mDriverUniforms.viewport[2] = glViewport.width;
    mDriverUniforms.viewport[3] = glViewport.height;

    mDriverUniforms.halfRenderAreaHeight =
        static_cast<float>(mDrawFramebuffer->getState().getDimensions().height) * 0.5f;
    mDriverUniforms.viewportYScale    = mDrawFramebuffer->flipY() ? -1.0f : 1.0f;
    mDriverUniforms.negViewportYScale = -mDriverUniforms.viewportYScale;

    mDriverUniforms.depthRange[0] = depthRangeNear;
    mDriverUniforms.depthRange[1] = depthRangeFar;
    mDriverUniforms.depthRange[2] = depthRangeDiff;

    ASSERT(mRenderEncoder.valid());
    mRenderEncoder.setFragmentData(mDriverUniforms, mtl::kDriverUniformsBindingIndex);
    mRenderEncoder.setVertexData(mDriverUniforms, mtl::kDriverUniformsBindingIndex);

    mDirtyBits.reset(DIRTY_BIT_DRIVER_UNIFORMS);
    return angle::Result::Continue;
}

angle::Result ContextMtl::handleDirtyDepthStencilState(const gl::Context *context)
{
    ASSERT(mRenderEncoder.valid());

    // Need to handle the case when render pass doesn't have depth/stencil attachment.
    mtl::DepthStencilDesc dsDesc              = mDepthStencilDesc;
    const mtl::RenderPassDesc &renderPassDesc = mDrawFramebuffer->getRenderPassDesc(this);

    if (!renderPassDesc.depthAttachment.texture)
    {
        dsDesc.depthWriteEnabled    = false;
        dsDesc.depthCompareFunction = MTLCompareFunctionAlways;
    }

    if (!renderPassDesc.stencilAttachment.texture)
    {
        dsDesc.frontFaceStencil.reset();
        dsDesc.backFaceStencil.reset();
    }

    // Apply depth stencil state
    mRenderEncoder.setDepthStencilState(getDisplay()->getDepthStencilState(dsDesc));

    mDirtyBits.reset(DIRTY_BIT_DEPTH_STENCIL_DESC);
    return angle::Result::Continue;
}

angle::Result ContextMtl::handleDirtyDepthBias(const gl::Context *context)
{
    const gl::RasterizerState &raserState = mState.getRasterizerState();
    ASSERT(mRenderEncoder.valid());
    if (!mState.isPolygonOffsetFillEnabled())
    {
        mRenderEncoder.setDepthBias(0, 0, 0);
    }
    else
    {
        mRenderEncoder.setDepthBias(raserState.polygonOffsetUnits, raserState.polygonOffsetFactor,
                                    0);
    }

    mDirtyBits.reset(DIRTY_BIT_DEPTH_BIAS);
    return angle::Result::Continue;
}

angle::Result ContextMtl::checkIfPipelineChanged(
    const gl::Context *context,
    gl::PrimitiveMode primitiveMode,
    Optional<mtl::RenderPipelineDesc> *changedPipelineDesc)
{
    ASSERT(mRenderEncoder.valid());
    mtl::PrimitiveTopologyClass topologyClass = mtl::GetPrimitiveTopologyClass(primitiveMode);

    bool rppChange = mDirtyBits.test(DIRTY_BIT_RENDER_PIPELINE) ||
                     topologyClass != mRenderPipelineDesc.inputPrimitiveTopology;

    // Obtain RenderPipelineDesc's vertex array descriptor.
    ANGLE_TRY(mVertexArray->setupDraw(context, &mRenderEncoder, &rppChange,
                                      &mRenderPipelineDesc.vertexDescriptor));

    if (rppChange)
    {
        const mtl::RenderPassDesc &renderPassDesc = mDrawFramebuffer->getRenderPassDesc(this);
        // Obtain RenderPipelineDesc's output descriptor.
        renderPassDesc.populateRenderPipelineOutputDesc(mBlendDesc,
                                                        &mRenderPipelineDesc.outputDescriptor);

        mRenderPipelineDesc.inputPrimitiveTopology = topologyClass;

        *changedPipelineDesc = mRenderPipelineDesc;
    }

    return angle::Result::Continue;
}
}
