// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <boost/container/static_vector.hpp>

#include "common/common_paths.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/microprofile.h"
#include "common/settings.h"
#include "video_core/renderer_vulkan/pica_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_renderpass_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/shader/generator/glsl_fs_shader_gen.h"
#include "video_core/shader/generator/glsl_shader_gen.h"
#include "video_core/shader/generator/spv_fs_shader_gen.h"

using namespace Pica::Shader::Generator;
using Pica::Shader::FSConfig;

MICROPROFILE_DEFINE(Vulkan_Bind, "Vulkan", "Pipeline Bind", MP_RGB(192, 32, 32));

namespace Vulkan {

u32 AttribBytes(Pica::PipelineRegs::VertexAttributeFormat format, u32 size) {
    switch (format) {
    case Pica::PipelineRegs::VertexAttributeFormat::FLOAT:
        return sizeof(float) * size;
    case Pica::PipelineRegs::VertexAttributeFormat::SHORT:
        return sizeof(u16) * size;
    case Pica::PipelineRegs::VertexAttributeFormat::BYTE:
    case Pica::PipelineRegs::VertexAttributeFormat::UBYTE:
        return sizeof(u8) * size;
    }
    return 0;
}

AttribLoadFlags MakeAttribLoadFlag(Pica::PipelineRegs::VertexAttributeFormat format) {
    switch (format) {
    case Pica::PipelineRegs::VertexAttributeFormat::BYTE:
    case Pica::PipelineRegs::VertexAttributeFormat::SHORT:
        return AttribLoadFlags::Sint;
    case Pica::PipelineRegs::VertexAttributeFormat::UBYTE:
        return AttribLoadFlags::Uint;
    default:
        return AttribLoadFlags::Float;
    }
}

constexpr std::array<vk::DescriptorSetLayoutBinding, 6> BUFFER_BINDINGS = {{
    {0, vk::DescriptorType::eUniformBufferDynamic, 1, vk::ShaderStageFlagBits::eVertex},
    {1, vk::DescriptorType::eUniformBufferDynamic, 1,
     vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eGeometry},
    {2, vk::DescriptorType::eUniformBufferDynamic, 1, vk::ShaderStageFlagBits::eFragment},
    {3, vk::DescriptorType::eUniformTexelBuffer, 1, vk::ShaderStageFlagBits::eFragment},
    {4, vk::DescriptorType::eUniformTexelBuffer, 1, vk::ShaderStageFlagBits::eFragment},
    {5, vk::DescriptorType::eUniformTexelBuffer, 1, vk::ShaderStageFlagBits::eFragment},
}};

constexpr std::array<vk::DescriptorSetLayoutBinding, 4> TEXTURE_BINDINGS = {{
    {0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment},
    {1, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment},
    {2, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment},
    {3, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment},
}};

// TODO: Use descriptor array for shadow cube
constexpr std::array<vk::DescriptorSetLayoutBinding, 7> SHADOW_BINDINGS = {{
    {0, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eFragment},
    {1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eFragment},
    {2, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eFragment},
    {3, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eFragment},
    {4, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eFragment},
    {5, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eFragment},
    {6, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eFragment},
}};

PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,
                             RenderpassCache& renderpass_cache_, DescriptorPool& pool_)
    : instance{instance_}, scheduler{scheduler_}, renderpass_cache{renderpass_cache_}, pool{pool_},
      num_worker_threads{std::max(std::thread::hardware_concurrency(), 2U)},
      workers{num_worker_threads, "Pipeline workers"},
      descriptor_set_providers{DescriptorSetProvider{instance, pool, BUFFER_BINDINGS},
                               DescriptorSetProvider{instance, pool, TEXTURE_BINDINGS},
                               DescriptorSetProvider{instance, pool, SHADOW_BINDINGS}},
      trivial_vertex_shader{
          instance, vk::ShaderStageFlagBits::eVertex,
          GLSL::GenerateTrivialVertexShader(instance.IsShaderClipDistanceSupported(), true)} {
    profile = Pica::Shader::Profile{
        .has_separable_shaders = true,
        .has_clip_planes = instance.IsShaderClipDistanceSupported(),
        .has_geometry_shader = instance.UseGeometryShaders(),
        .has_custom_border_color = instance.IsCustomBorderColorSupported(),
        .has_fragment_shader_interlock = instance.IsFragmentShaderInterlockSupported(),
        .has_fragment_shader_barycentric = instance.IsFragmentShaderBarycentricSupported(),
        .has_blend_minmax_factor = false,
        .has_minus_one_to_one_range = false,
        .has_logic_op = !instance.NeedsLogicOpEmulation(),
        .is_vulkan = true,
    };
    BuildLayout();
}

void PipelineCache::BuildLayout() {
    std::array<vk::DescriptorSetLayout, NUM_RASTERIZER_SETS> descriptor_set_layouts;
    std::transform(descriptor_set_providers.begin(), descriptor_set_providers.end(),
                   descriptor_set_layouts.begin(),
                   [](const auto& provider) { return provider.Layout(); });

    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = NUM_RASTERIZER_SETS,
        .pSetLayouts = descriptor_set_layouts.data(),
        .pushConstantRangeCount = 0,
        .pPushConstantRanges = nullptr,
    };
    pipeline_layout = instance.GetDevice().createPipelineLayoutUnique(layout_info);
}

PipelineCache::~PipelineCache() {
    SaveDiskCache();
}

void PipelineCache::LoadDiskCache() {
    if (!Settings::values.use_disk_shader_cache || !EnsureDirectories()) {
        return;
    }

    const std::string cache_file_path = fmt::format("{}{:x}{:x}.bin", GetPipelineCacheDir(),
                                                    instance.GetVendorID(), instance.GetDeviceID());
    vk::PipelineCacheCreateInfo cache_info = {
        .initialDataSize = 0,
        .pInitialData = nullptr,
    };

    std::vector<u8> cache_data;
    FileUtil::IOFile cache_file{cache_file_path, "r"};
    if (cache_file.IsOpen()) {
        LOG_INFO(Render_Vulkan, "Loading pipeline cache");

        const u64 cache_file_size = cache_file.GetSize();
        cache_data.resize(cache_file_size);
        if (cache_file.ReadBytes(cache_data.data(), cache_file_size)) {
            if (!IsCacheValid(cache_data)) {
                LOG_WARNING(Render_Vulkan, "Pipeline cache provided invalid, ignoring");
            } else {
                cache_info.initialDataSize = cache_file_size;
                cache_info.pInitialData = cache_data.data();
            }
        }

        cache_file.Close();
    }

    vk::Device device = instance.GetDevice();
    pipeline_cache = device.createPipelineCacheUnique(cache_info);
}

void PipelineCache::SaveDiskCache() {
    if (!Settings::values.use_disk_shader_cache || !EnsureDirectories() || !pipeline_cache) {
        return;
    }

    const std::string cache_file_path = fmt::format("{}{:x}{:x}.bin", GetPipelineCacheDir(),
                                                    instance.GetVendorID(), instance.GetDeviceID());
    FileUtil::IOFile cache_file{cache_file_path, "wb"};
    if (!cache_file.IsOpen()) {
        LOG_ERROR(Render_Vulkan, "Unable to open pipeline cache for writing");
        return;
    }

    vk::Device device = instance.GetDevice();
    auto cache_data = device.getPipelineCacheData(*pipeline_cache);
    if (!cache_file.WriteBytes(cache_data.data(), cache_data.size())) {
        LOG_ERROR(Render_Vulkan, "Error during pipeline cache write");
        return;
    }

    cache_file.Close();
}

bool PipelineCache::BindPipeline(const PipelineInfo& info, bool wait_built) {
    MICROPROFILE_SCOPE(Vulkan_Bind);

    u64 shader_hash = 0;
    for (u32 i = 0; i < MAX_SHADER_STAGES; i++) {
        shader_hash = Common::HashCombine(shader_hash, shader_hashes[i]);
    }

    const u64 info_hash = info.Hash(instance);
    const u64 pipeline_hash = Common::HashCombine(shader_hash, info_hash);

    auto [it, new_pipeline] = graphics_pipelines.try_emplace(pipeline_hash);
    if (new_pipeline) {
        it.value() =
            std::make_unique<GraphicsPipeline>(instance, renderpass_cache, info, *pipeline_cache,
                                               *pipeline_layout, current_shaders, &workers);
    }

    GraphicsPipeline* const pipeline{it->second.get()};
    if (!pipeline->IsDone() && !pipeline->TryBuild(wait_built)) {
        return false;
    }

    u32 new_descriptors_start = 0;
    std::span<vk::DescriptorSet> new_descriptors_span{};
    std::span<u32> new_offsets_span{};

    // Ensure all the descriptor sets are set at least once at the beginning.
    if (scheduler.IsStateDirty(StateFlags::DescriptorSets)) {
        set_dirty.set();
    }

    if (set_dirty.any()) {
        for (u32 i = 0; i < NUM_RASTERIZER_SETS; i++) {
            if (!set_dirty.test(i)) {
                continue;
            }
            bound_descriptor_sets[i] = descriptor_set_providers[i].Acquire(update_data[i]);
        }
        new_descriptors_span = bound_descriptor_sets;

        // Only send new offsets if the buffer descriptor-set changed.
        if (set_dirty.test(0)) {
            new_offsets_span = offsets;
        }

        // Try to compact the number of updated descriptor-set slots to the ones that have actually
        // changed
        if (!set_dirty.all()) {
            const u64 dirty_mask = set_dirty.to_ulong();
            new_descriptors_start = static_cast<u32>(std::countr_zero(dirty_mask));
            const u32 new_descriptors_end = 64u - static_cast<u32>(std::countl_zero(dirty_mask));
            const u32 new_descriptors_size = new_descriptors_end - new_descriptors_start;

            new_descriptors_span =
                new_descriptors_span.subspan(new_descriptors_start, new_descriptors_size);
        }

        set_dirty.reset();
    }

    boost::container::static_vector<vk::DescriptorSet, NUM_RASTERIZER_SETS> new_descriptors(
        new_descriptors_span.begin(), new_descriptors_span.end());
    boost::container::static_vector<u32, NUM_DYNAMIC_OFFSETS> new_offsets(new_offsets_span.begin(),
                                                                          new_offsets_span.end());

    const bool is_dirty = scheduler.IsStateDirty(StateFlags::Pipeline);
    const bool pipeline_dirty = (current_pipeline != pipeline) || is_dirty;
    scheduler.Record([this, is_dirty, pipeline_dirty, pipeline,
                      current_dynamic = current_info.dynamic, dynamic = info.dynamic,
                      new_descriptors_start, descriptor_sets = std::move(new_descriptors),
                      offsets = std::move(new_offsets),
                      current_rasterization = current_info.rasterization,
                      current_depth_stencil = current_info.depth_stencil,
                      rasterization = info.rasterization,
                      depth_stencil = info.depth_stencil](vk::CommandBuffer cmdbuf) {
        if (dynamic.viewport != current_dynamic.viewport || is_dirty) {
            const vk::Viewport vk_viewport = {
                .x = static_cast<f32>(dynamic.viewport.left),
                .y = static_cast<f32>(dynamic.viewport.top),
                .width = static_cast<f32>(dynamic.viewport.GetWidth()),
                .height = static_cast<f32>(dynamic.viewport.GetHeight()),
                .minDepth = 0.f,
                .maxDepth = 1.f,
            };
            cmdbuf.setViewport(0, vk_viewport);
        }

        if (dynamic.scissor != current_dynamic.scissor || is_dirty) {
            const vk::Rect2D scissor = {
                .offset{
                    .x = static_cast<s32>(dynamic.scissor.left),
                    .y = static_cast<s32>(dynamic.scissor.bottom),
                },
                .extent{
                    .width = dynamic.scissor.GetWidth(),
                    .height = dynamic.scissor.GetHeight(),
                },
            };
            cmdbuf.setScissor(0, scissor);
        }

        if (dynamic.stencil_compare_mask != current_dynamic.stencil_compare_mask || is_dirty) {
            cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                         dynamic.stencil_compare_mask);
        }

        if (dynamic.stencil_write_mask != current_dynamic.stencil_write_mask || is_dirty) {
            cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                       dynamic.stencil_write_mask);
        }

        if (dynamic.stencil_reference != current_dynamic.stencil_reference || is_dirty) {
            cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack,
                                       dynamic.stencil_reference);
        }

        if (dynamic.blend_color != current_dynamic.blend_color || is_dirty) {
            const Common::Vec4f color = PicaToVK::ColorRGBA8(dynamic.blend_color);
            cmdbuf.setBlendConstants(color.AsArray());
        }

        if (instance.IsExtendedDynamicStateSupported()) {
            if (rasterization.cull_mode != current_rasterization.cull_mode || is_dirty) {
                cmdbuf.setCullModeEXT(PicaToVK::CullMode(rasterization.cull_mode));
                cmdbuf.setFrontFaceEXT(PicaToVK::FrontFace(rasterization.cull_mode));
            }

            if (depth_stencil.depth_compare_op != current_depth_stencil.depth_compare_op ||
                is_dirty) {
                cmdbuf.setDepthCompareOpEXT(PicaToVK::CompareFunc(depth_stencil.depth_compare_op));
            }

            if (depth_stencil.depth_test_enable != current_depth_stencil.depth_test_enable ||
                is_dirty) {
                cmdbuf.setDepthTestEnableEXT(depth_stencil.depth_test_enable);
            }

            if (depth_stencil.depth_write_enable != current_depth_stencil.depth_write_enable ||
                is_dirty) {
                cmdbuf.setDepthWriteEnableEXT(depth_stencil.depth_write_enable);
            }

            if (rasterization.topology != current_rasterization.topology || is_dirty) {
                cmdbuf.setPrimitiveTopologyEXT(PicaToVK::PrimitiveTopology(rasterization.topology));
            }

            if (depth_stencil.stencil_test_enable != current_depth_stencil.stencil_test_enable ||
                is_dirty) {
                cmdbuf.setStencilTestEnableEXT(depth_stencil.stencil_test_enable);
            }

            if (depth_stencil.stencil_fail_op != current_depth_stencil.stencil_fail_op ||
                depth_stencil.stencil_pass_op != current_depth_stencil.stencil_pass_op ||
                depth_stencil.stencil_depth_fail_op !=
                    current_depth_stencil.stencil_depth_fail_op ||
                depth_stencil.stencil_compare_op != current_depth_stencil.stencil_compare_op ||
                is_dirty) {
                cmdbuf.setStencilOpEXT(vk::StencilFaceFlagBits::eFrontAndBack,
                                       PicaToVK::StencilOp(depth_stencil.stencil_fail_op),
                                       PicaToVK::StencilOp(depth_stencil.stencil_pass_op),
                                       PicaToVK::StencilOp(depth_stencil.stencil_depth_fail_op),
                                       PicaToVK::CompareFunc(depth_stencil.stencil_compare_op));
            }
        }

        if (pipeline_dirty) {
            if (!pipeline->IsDone()) {
                pipeline->WaitDone();
            }
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());
        }

        if (descriptor_sets.size()) {
            cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *pipeline_layout,
                                      new_descriptors_start, descriptor_sets, offsets);
        }
    });

    current_info = info;
    current_pipeline = pipeline;
    scheduler.MarkStateNonDirty(StateFlags::Pipeline | StateFlags::DescriptorSets);

    return true;
}

bool PipelineCache::UseProgrammableVertexShader(const Pica::Regs& regs,
                                                Pica::Shader::ShaderSetup& setup,
                                                const VertexLayout& layout) {
    // Enable the geometry-shader only if we are actually doing per-fragment lighting
    // and care about proper quaternions. Otherwise just use standard vertex+fragment shaders.
    // We also don't need the geometry shader if we have the barycentric extension.
    const bool use_geometry_shader = instance.UseGeometryShaders() && !regs.lighting.disable &&
                                     !instance.IsFragmentShaderBarycentricSupported();

    PicaVSConfig config{regs, setup, instance.IsShaderClipDistanceSupported(), use_geometry_shader};

    for (u32 i = 0; i < layout.attribute_count; i++) {
        const VertexAttribute& attr = layout.attributes[i];
        const FormatTraits& traits = instance.GetTraits(attr.type, attr.size);
        const u32 location = attr.location.Value();
        AttribLoadFlags& flags = config.state.load_flags[location];

        if (traits.needs_conversion) {
            flags = MakeAttribLoadFlag(attr.type);
        }
        if (traits.needs_emulation) {
            flags |= AttribLoadFlags::ZeroW;
        }
    }

    auto [it, new_config] = programmable_vertex_map.try_emplace(config);
    if (new_config) {
        auto program = GLSL::GenerateVertexShader(setup, config, true);
        if (program.empty()) {
            LOG_ERROR(Render_Vulkan, "Failed to retrieve programmable vertex shader");
            programmable_vertex_map[config] = nullptr;
            return false;
        }

        auto [iter, new_program] = programmable_vertex_cache.try_emplace(program, instance);
        auto& shader = iter->second;

        if (new_program) {
            shader.program = std::move(program);
            const vk::Device device = instance.GetDevice();
            workers.QueueWork([device, &shader] {
                shader.module = Compile(shader.program, vk::ShaderStageFlagBits::eVertex, device);
                shader.MarkDone();
            });
        }

        it->second = &shader;
    }

    Shader* const shader{it->second};
    if (!shader) {
        LOG_ERROR(Render_Vulkan, "Failed to retrieve programmable vertex shader");
        return false;
    }

    current_shaders[ProgramType::VS] = shader;
    shader_hashes[ProgramType::VS] = config.Hash();

    return true;
}

void PipelineCache::UseTrivialVertexShader() {
    current_shaders[ProgramType::VS] = &trivial_vertex_shader;
    shader_hashes[ProgramType::VS] = 0;
}

bool PipelineCache::UseFixedGeometryShader(const Pica::Regs& regs) {
    if (!instance.UseGeometryShaders()) {
        UseTrivialGeometryShader();
        return true;
    }

    const PicaFixedGSConfig gs_config{regs, instance.IsShaderClipDistanceSupported()};
    auto [it, new_shader] = fixed_geometry_shaders.try_emplace(gs_config, instance);
    auto& shader = it->second;

    if (new_shader) {
        workers.QueueWork([gs_config, device = instance.GetDevice(), &shader]() {
            const auto code = GLSL::GenerateFixedGeometryShader(gs_config, true);
            shader.module = Compile(code, vk::ShaderStageFlagBits::eGeometry, device);
            shader.MarkDone();
        });
    }

    current_shaders[ProgramType::GS] = &shader;
    shader_hashes[ProgramType::GS] = gs_config.Hash();

    return true;
}

void PipelineCache::UseTrivialGeometryShader() {
    current_shaders[ProgramType::GS] = nullptr;
    shader_hashes[ProgramType::GS] = 0;
}

void PipelineCache::UseFragmentShader(const Pica::Regs& regs,
                                      const Pica::Shader::UserConfig& user) {
    const FSConfig fs_config{regs, user, profile};
    const auto [it, new_shader] = fragment_shaders.try_emplace(fs_config, instance);
    auto& shader = it->second;

    if (new_shader) {
        const bool use_spirv = Settings::values.spirv_shader_gen.GetValue();
        if (use_spirv && !fs_config.UsesShadowPipeline()) {
            const std::vector code = SPIRV::GenerateFragmentShader(fs_config, profile);
            shader.module = CompileSPV(code, instance.GetDevice());
            shader.MarkDone();
        } else {
            workers.QueueWork([fs_config, this, &shader]() {
                const std::string code = GLSL::GenerateFragmentShader(fs_config, profile);
                shader.module =
                    Compile(code, vk::ShaderStageFlagBits::eFragment, instance.GetDevice());
                shader.MarkDone();
            });
        }
    }

    current_shaders[ProgramType::FS] = &shader;
    shader_hashes[ProgramType::FS] = fs_config.Hash();
}

void PipelineCache::BindTexture(u32 binding, vk::ImageView image_view, vk::Sampler sampler) {
    auto& info = update_data[1][binding].image_info;
    if (info.imageView == image_view && info.sampler == sampler) {
        return;
    }
    set_dirty[1] = true;
    info = vk::DescriptorImageInfo{
        .sampler = sampler,
        .imageView = image_view,
        .imageLayout = vk::ImageLayout::eGeneral,
    };
}

void PipelineCache::BindStorageImage(u32 binding, vk::ImageView image_view) {
    auto& info = update_data[2][binding].image_info;
    if (info.imageView == image_view) {
        return;
    }
    set_dirty[2] = true;
    info = vk::DescriptorImageInfo{
        .imageView = image_view,
        .imageLayout = vk::ImageLayout::eGeneral,
    };
}

void PipelineCache::BindBuffer(u32 binding, vk::Buffer buffer, u32 offset, u32 size) {
    auto& info = update_data[0][binding].buffer_info;
    if (info.buffer == buffer && info.offset == offset && info.range == size) {
        return;
    }
    set_dirty[0] = true;
    info = vk::DescriptorBufferInfo{
        .buffer = buffer,
        .offset = offset,
        .range = size,
    };
}

void PipelineCache::BindTexelBuffer(u32 binding, vk::BufferView buffer_view) {
    auto& view = update_data[0][binding].buffer_view;
    if (view != buffer_view) {
        set_dirty[0] = true;
        view = buffer_view;
    }
}

void PipelineCache::SetBufferOffset(u32 binding, size_t offset) {
    if (offsets[binding] != static_cast<u32>(offset)) {
        offsets[binding] = static_cast<u32>(offset);
        set_dirty[0] = true;
    }
}

bool PipelineCache::IsCacheValid(std::span<const u8> data) const {
    if (data.size() < sizeof(vk::PipelineCacheHeaderVersionOne)) {
        LOG_ERROR(Render_Vulkan, "Pipeline cache failed validation: Invalid header");
        return false;
    }

    vk::PipelineCacheHeaderVersionOne header;
    std::memcpy(&header, data.data(), sizeof(header));
    if (header.headerSize < sizeof(header)) {
        LOG_ERROR(Render_Vulkan, "Pipeline cache failed validation: Invalid header length");
        return false;
    }

    if (header.headerVersion != vk::PipelineCacheHeaderVersion::eOne) {
        LOG_ERROR(Render_Vulkan, "Pipeline cache failed validation: Invalid header version");
        return false;
    }

    if (u32 vendor_id = instance.GetVendorID(); header.vendorID != vendor_id) {
        LOG_ERROR(
            Render_Vulkan,
            "Pipeline cache failed validation: Incorrect vendor ID (file: {:#X}, device: {:#X})",
            header.vendorID, vendor_id);
        return false;
    }

    if (u32 device_id = instance.GetDeviceID(); header.deviceID != device_id) {
        LOG_ERROR(
            Render_Vulkan,
            "Pipeline cache failed validation: Incorrect device ID (file: {:#X}, device: {:#X})",
            header.deviceID, device_id);
        return false;
    }

    if (header.pipelineCacheUUID != instance.GetPipelineCacheUUID()) {
        LOG_ERROR(Render_Vulkan, "Pipeline cache failed validation: Incorrect UUID");
        return false;
    }

    return true;
}

bool PipelineCache::EnsureDirectories() const {
    const auto create_dir = [](const std::string& dir) {
        if (!FileUtil::CreateDir(dir)) {
            LOG_ERROR(Render_Vulkan, "Failed to create directory={}", dir);
            return false;
        }

        return true;
    };

    return create_dir(FileUtil::GetUserPath(FileUtil::UserPath::ShaderDir)) &&
           create_dir(GetPipelineCacheDir());
}

std::string PipelineCache::GetPipelineCacheDir() const {
    return FileUtil::GetUserPath(FileUtil::UserPath::ShaderDir) + "vulkan" + DIR_SEP;
}

} // namespace Vulkan
