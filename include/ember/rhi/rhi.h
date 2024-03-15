#pragma once

#include "core/platform.h"
#include <vector>

namespace Ember
{
    // Possible types of Buffers we can create through the RHI
    enum class BufferUsage : u32
    {
        Vertex,
        Index,
        Uniform,
        Staging,
        Count
    };

    // Defines the types of Memory a resource can be allocated to
    enum class MemoryType : u32
    {
        Host,
        GPU,
        Shared,
        Auto,
        Count,
    };

    // Defines the types of Queues the RHI provides access to
    enum class QueueType : u32
    {
        Graphics,
        Compute,
        Transfer,
        Count,
    };

    // Defines the possible shader stages
    enum class ShaderStage : u32
    {
        Vertex,
        Fragment,
        All,
        Count
    };

    // Defines VertexAttribute formats
    enum class Format : u32
    {
        Float,
        Float2,
        Float3,
        Float4,
        Count,
    };

    // Configuration Struct for a Buffer object
    struct BufferDef
    {
        BufferUsage usage   = BufferUsage::Vertex;
        MemoryType memory   = MemoryType::Auto;
        void*       data    = nullptr;
        u64         size    = 0;
    };

    // Configuration Struct for a Shader (PSO)
    struct ShaderDef
    {
        // Defines a Vertex Attribute
        struct VertexAttribute
        {
            u64 offset = 0;
            Format format = Format::Float;
        };

        // Defines a VertexAttribute Binding
        struct VertexBinding
        {
            u32 stride = 0;
            std::vector<VertexAttribute> attributes;
        };

        const char* name;
        std::vector<char>  vertex;
        std::vector<char>  fragment;
        std::vector<VertexBinding> vertex_bindings;
        //std::vector<Handle<DescriptorSet>> descriptor_sets;
    };
}

#ifdef EMBER_VULKAN
#   include "rhi/vulkan/render_device.h"
#   include "rhi/vulkan/buffer.h"
#   include "rhi/vulkan/shader.h"
#   include "rhi/vulkan/command_buffer.h"
#elif EMBER_DX12
    // not implemented yet
#endif
