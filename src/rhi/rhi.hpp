// L2: thin rendering hardware interface (docs/architecture.md §2, §5 #1).
//
// Shaped for tile-based GPUs (Adreno 702): every pass states how its attachments are
// loaded and stored, so the backend can clear instead of loading tiles and discard
// depth instead of writing it back. Only a GLES 3.0 backend exists; the interface keeps
// out of GL's way enough that a Vulkan 1.0 backend could be added later.
//
// Conventions:
//   - Handles are plain ids; 0 is "none". TargetH{} in a pass means the surface's
//     default framebuffer.
//   - Shaders are GLSL ES 3.00 source. Uniforms live in one std140 block named "Params"
//     (set per draw with set_uniforms); samplers are named u_tex0..u_tex3 and bound to
//     texture slots 0..3.
//   - RGBA8 textures and render targets are top-row-first in memory (read_pixels too): row 0 is
//     sampled at v = 1. Data textures (R8, R16F, RGBA16F) are taken as they are: row 0 at v = 0,
//     so texelFetch(ivec2(x, y)) is byte y * stride + x (camera frames, lookup tables).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace alia::rhi {

template <class Tag>
struct Handle {
    uint32_t id = 0;
    explicit operator bool() const { return id != 0; }
    bool operator==(const Handle&) const = default;
};
using BufferH = Handle<struct BufferTag>;
using TextureH = Handle<struct TextureTag>;
using TargetH = Handle<struct TargetTag>;
using PipelineH = Handle<struct PipelineTag>;
using FenceH = Handle<struct FenceTag>;

enum class BufferUsage { Vertex, Index16 };
struct BufferDesc {
    BufferUsage usage = BufferUsage::Vertex;
    size_t size = 0;
    const void* data = nullptr;
    bool dynamic = false;  // updated every frame
};

enum class Format { RGBA8, Depth24, Depth32F, R8, R16F, RGBA16F };  // 16F: uploaded from floats
enum class Filter { Nearest, Linear };
enum class Wrap { Clamp, Repeat };  // outside 0..1: the edge, or the texture again (tiling)
struct TextureDesc {
    int width = 0, height = 0;
    Format format = Format::RGBA8;
    Filter filter = Filter::Nearest;
    const void* pixels = nullptr;  // RGBA8: top row first; R8: bytes; 16F: floats; may be null
    Wrap wrap = Wrap::Clamp;
};

// A Linux dmabuf holding one 8-bit plane (DRM_FORMAT_R8), imported without a copy (camera frames).
struct DmabufDesc {
    int fd = -1;  // not taken over: the caller keeps it (and may close it once imported)
    int width = 0, height = 0;
    uint32_t stride = 0, offset = 0;
    uint64_t modifier = 0;  // DRM_FORMAT_MOD_LINEAR
};

struct TargetDesc {
    TextureH color;  // RGBA8 texture
    TextureH depth;  // optional depth texture
};

enum class AttribType { Float2, Float3, Float4, UNorm8x4 };
struct VertexAttrib {
    uint32_t location = 0;
    AttribType type = AttribType::Float3;
    uint32_t offset = 0;
    bool per_instance = false;  // read from the instance buffer, once per instance (instance_stride)
};
constexpr int kMaxAttribs = 8;

struct PipelineDesc {
    const char* vertex_source = nullptr;
    const char* fragment_source = nullptr;
    VertexAttrib attribs[kMaxAttribs] = {};
    int attrib_count = 0;
    uint32_t stride = 0;
    uint32_t instance_stride = 0;  // bytes per instance in the instance buffer (per_instance attributes)
    bool depth_test = false;
    bool depth_write = false;
    bool alpha_blend = false;
    bool cull_back = false;
    const char* name = "";
};

enum class LoadOp { Load, Clear, DontCare };
enum class StoreOp { Store, DontCare };

struct PassDesc {
    TargetH target;  // {} = default framebuffer of the surface
    int width = 0, height = 0;  // size of the target (needed for the default framebuffer)
    LoadOp color_load = LoadOp::Clear;
    StoreOp color_store = StoreOp::Store;
    LoadOp depth_load = LoadOp::Clear;
    StoreOp depth_store = StoreOp::DontCare;  // tile GPUs: never write depth back unless asked
    float clear_color[4] = {0, 0, 0, 1};
    float clear_depth = 1.0f;
    const char* name = "";
};

struct Viewport {
    int x = 0, y = 0, width = 0, height = 0;  // top-left origin, pixels
};

struct FrameStats {
    uint32_t passes = 0;
    uint32_t draws = 0;
    uint32_t triangles = 0;
};

class Device {
public:
    virtual ~Device() = default;

    virtual BufferH create_buffer(const BufferDesc& desc) = 0;
    virtual void update_buffer(BufferH buf, const void* data, size_t size, size_t offset = 0) = 0;
    virtual TextureH create_texture(const TextureDesc& desc) = 0;
    // Replaces all pixels of an RGBA8 texture (top row first).
    virtual void update_texture(TextureH tex, const void* pixels) = 0;
    // Replaces all of a data texture (R8: row_bytes apart, 0 = tight; 16F: tightly packed floats).
    virtual void update_data_texture(TextureH tex, const void* data, size_t row_bytes = 0) = 0;
    // An R8 texture reading the dmabuf in place, NEAREST; {} when this GPU or display can't import it
    // (then mmap the fd and update_data_texture instead). destroy() it like any texture.
    virtual TextureH import_dmabuf(const DmabufDesc& desc, std::string* error) = 0;
    virtual TargetH create_target(const TargetDesc& desc) = 0;
    virtual PipelineH create_pipeline(const PipelineDesc& desc, std::string* error) = 0;
    virtual void destroy(BufferH h) = 0;
    virtual void destroy(TextureH h) = 0;
    virtual void destroy(TargetH h) = 0;
    virtual void destroy(PipelineH h) = 0;
    virtual void destroy(FenceH h) = 0;

    // A fence after everything submitted so far; signaled() once the GPU has finished it (never blocks).
    virtual FenceH fence() = 0;
    virtual bool signaled(FenceH h) = 0;

    // Commands. Everything between begin_pass and end_pass draws into that pass's target.
    virtual void begin_pass(const PassDesc& desc) = 0;
    virtual void set_viewport(const Viewport& vp) = 0;  // defaults to the whole target
    virtual void set_pipeline(PipelineH p) = 0;
    virtual void set_vertex_buffer(BufferH b) = 0;
    // The buffer per_instance attributes read from, starting `offset` bytes in (GLES 3.0 has no
    // base instance, so a batch picks its slice with the offset).
    virtual void set_instance_buffer(BufferH b, size_t offset = 0) = 0;
    virtual void set_index_buffer(BufferH b) = 0;
    // filter overrides the texture's own filter for this bind (GLES sampler objects); the wrap stays the texture's.
    virtual void set_texture(int slot, TextureH t) = 0;
    virtual void set_texture(int slot, TextureH t, Filter filter) = 0;
    virtual void set_uniforms(const void* data, size_t size) = 0;  // the "Params" block
    virtual void draw(uint32_t vertex_count, uint32_t first_vertex = 0) = 0;
    virtual void draw_indexed(uint32_t index_count, uint32_t first_index = 0) = 0;
    virtual void draw_instanced(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex = 0) = 0;
    virtual void end_pass() = 0;

    // Read a target back as RGBA8, top row first (tests, screenshots). Slow: stalls the GPU.
    virtual void read_pixels(TargetH target, int width, int height, uint8_t* rgba) = 0;

    virtual FrameStats take_stats() = 0;  // counters since the last call
    virtual std::string describe() const = 0;  // GL vendor / renderer / version
};

// Requires a current GLES 3.0+ context (made by a platform::Surface).
std::unique_ptr<Device> create_gles_device();

}  // namespace alia::rhi
