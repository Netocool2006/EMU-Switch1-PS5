// SPDX-License-Identifier: GPL-3.0-or-later
// PSSR 2 upscale pass. Inserted into the port's copy of present/fsr.cpp by prepare-vulkan-port.py,
// after the PushConstants alias: PssrDraw() records one libScePsml upscale in place of FSR's EASU and
// RCAS passes (FSR::Draw calls it first, see vulkan_pssr_pass_use.inc).
#ifdef ENABLE_PSSR
// The SPIR-V of the optical flow shader (EDEN_PSSR_FLOW_COMP_SPV, built by prepare-vulkan-port.py). vulkan_fsr.cpp
// does not include it on its own, so it is included here, inside namespace Vulkan: the header only declares
// constexpr arrays (and <cstdint>, already included), so that is harmless.
// FASE2-VERIFICAR: if the compiler objects to the include at this point, move it to the include list that
// prepare-vulkan-port.py adds to vulkan_fsr.cpp (the 'adapt(... fsr.cpp ...)' entry, next to "pssr_bridge.h").
#include "vulkan_hud_shaders.h"
extern "C" {
// Hooks added to RADV by tools/patch-radv-pssr.py.
void vkEdenCmdInjectDwords(VkCommandBuffer commandBuffer, const uint32_t* dwords, uint32_t count);
void vkEdenCmdExecuteIndirect(VkCommandBuffer commandBuffer, uint64_t va, uint32_t dwords);
void vkEdenGetImageViewDescriptor(VkImageView imageView, VkBool32 storage, uint32_t* out);
}

namespace {
// What libScePsml requires and a Switch game cannot give: motion vectors (zero) and depth (constant).
// Never destroyed on purpose: a destructor at exit would call into a driver that is already gone.
struct PssrAux {
    vk::Image motion;
    vk::Image depth;
    vk::ImageView motion_view;
    vk::ImageView depth_view;
    VkExtent2D extent{};
    // Optical flow (vulkan_pssr_flow.comp): the previous picture, sampled next to the current one by a compute
    // pass that writes `motion` before PSSR reads it. The pipeline objects are made once; `prev` follows the
    // input size. A ring of descriptor sets: the game's view can be another image every frame, so the set is
    // rewritten each frame, and a set is not touched again until kFlowSets frames later.
    static constexpr unsigned kFlowSets = 8;
    vk::Image prev;
    vk::ImageView prev_view;
    vk::Sampler flow_sampler;
    vk::DescriptorSetLayout flow_set_layout;
    vk::DescriptorPool flow_pool;
    vk::DescriptorSets flow_sets;
    vk::PipelineLayout flow_layout;
    vk::Pipeline flow_pipeline;
    bool flow_tried = false;   // creation attempted (once per run)
    bool flow_ok = false;      // the pass runs; otherwise `motion` stays the zeros it was cleared to
    bool first_frame = true;   // `prev` holds nothing yet: sign 0 and a history reset for PSSR
    unsigned flow_frame = 0;   // the ring index
};
PssrAux* const g_pssr_aux = new PssrAux;
unsigned g_pssr_reports = 0;
unsigned g_pssr_dumps = 0;

// The push constants of vulkan_pssr_flow.comp: {uvec2 size; int radius; float sign;}.
struct PssrFlowPush {
    uint32_t width, height;
    int32_t radius;
    float sign;
};
static_assert(sizeof(PssrFlowPush) == 16, "push constants of vulkan_pssr_flow.comp");

// Makes the optical flow pipeline once. On any failure the pass is left off and PSSR keeps the zero motion of
// before (the clear at creation): never a reason to abort the upscale.
void PssrFlowCreate(const Device& device) {
    PssrAux& aux = *g_pssr_aux;
    if (aux.flow_tried) return;
    aux.flow_tried = true;
    // FASE2-VERIFICAR: the wrappers throw vk::Exception (a std::exception) on a failed call; if the build has no
    // exceptions, replace the try/catch by checking each handle.
    try {
        aux.flow_sampler = CreateBilinearSampler(device);
        // FASE2-VERIFICAR: the 3-argument form with the stage is the one vulkan_hud_prepare.inc uses; three
        // bindings in the order of the shader (cur, prev, motion).
        aux.flow_set_layout = CreateWrappedDescriptorSetLayout(
            device,
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
            VK_SHADER_STAGE_COMPUTE_BIT);
        // FASE2-VERIFICAR: the HUD passes only (device, max_descriptors, max_sets); the fourth argument (the
        // descriptor types of the pool) exists in the helper's signature upstream with a default of
        // {COMBINED_IMAGE_SAMPLER}, and STORAGE_IMAGE is needed here.
        aux.flow_pool = CreateWrappedDescriptorPool(device, 3 * PssrAux::kFlowSets, PssrAux::kFlowSets,
                                                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                     VK_DESCRIPTOR_TYPE_STORAGE_IMAGE});
        std::array<VkDescriptorSetLayout, PssrAux::kFlowSets> layouts;
        layouts.fill(*aux.flow_set_layout);
        // FASE2-VERIFICAR: the HUD passes one VkDescriptorSetLayout handle; here a span of kFlowSets of them.
        aux.flow_sets = CreateWrappedDescriptorSets(aux.flow_pool, layouts);
        const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PssrFlowPush)};
        aux.flow_layout = device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .setLayoutCount = 1,
            .pSetLayouts = aux.flow_set_layout.address(),
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        });
        auto shader = BuildShader(device, EDEN_PSSR_FLOW_COMP_SPV);
        // FASE2-VERIFICAR: no compute pipeline is made anywhere else in the port; vk::Device::CreateComputePipeline
        // (vulkan_wrapper.h) takes the create info and an optional VkPipelineCache.
        aux.flow_pipeline = device.GetLogical().CreateComputePipeline(VkComputePipelineCreateInfo{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage =
                {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                    .module = *shader,
                    .pName = "main",
                    .pSpecializationInfo = nullptr,
                },
            .layout = *aux.flow_layout,
            .basePipelineHandle = nullptr,
            .basePipelineIndex = 0,
        });
        aux.flow_ok = true;
        Eden::Report("pssr", "flow pipeline listo (vulkan_pssr_flow.comp, 8x8 groups, radius 4)");
    } catch (const std::exception& e) {
        aux.flow_ok = false;
        char line[240];
        std::snprintf(line, sizeof(line), "flow pipeline NOT created (%s): motion vectors stay zero", e.what());
        Eden::Report("pssr", line);
    }
}


float PssrHalf(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1f, man = h & 0x3ff;
    float f;
    if (exp == 0) f = std::ldexp(static_cast<float>(man), -24);
    else if (exp == 31) f = man ? std::nanf("") : INFINITY;
    else f = std::ldexp(static_cast<float>(man | 0x400), static_cast<int>(exp) - 25);
    return sign ? -f : f;
}

// One-time check of what PSSR wrote: reads a 256x256 crop of the middle of the 2160p image back and reports
// its statistics (a mean of 0 and every pixel zero would mean nothing was written).
bool PssrProbe(MemoryAllocator& allocator, Scheduler& scheduler, VkImage out_image, VkExtent2D out,
               bool log_it = true) {
    constexpr uint32_t kCrop = 256;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(kCrop) * kCrop * 8;   // R16G16B16A16_SFLOAT
    vk::Buffer buffer = CreateWrappedBuffer(allocator, bytes, MemoryUsage::Download);
    const VkBufferImageCopy region{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
        .imageOffset = {static_cast<int32_t>(out.width / 2 - kCrop / 2), static_cast<int32_t>(out.height / 2 - kCrop / 2), 0},
        .imageExtent = {kCrop, kCrop, 1},
    };
    const VkBuffer raw = *buffer;
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        constexpr VkMemoryBarrier everything{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, everything);
        cmdbuf.CopyImageToBuffer(out_image, VK_IMAGE_LAYOUT_GENERAL, raw, region);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, everything);
    });
    scheduler.Finish();
    buffer.Invalidate();
    const auto mapped = buffer.Mapped();
    const auto* px = reinterpret_cast<const uint16_t*>(mapped.data());
    double sum[4] = {0, 0, 0, 0};
    float lo = 1e30f, hi = -1e30f;
    uint32_t zero = 0, bad = 0;
    for (uint32_t i = 0; i < kCrop * kCrop; ++i) {
        float v[4];
        bool all_zero = true;
        for (int c = 0; c < 4; ++c) {
            v[c] = PssrHalf(px[i * 4 + c]);
            if (v[c] != 0.0f) all_zero = false;
            if (!std::isfinite(v[c])) { ++bad; v[c] = 0; }
            sum[c] += v[c];
            lo = std::min(lo, v[c]);
            hi = std::max(hi, v[c]);
        }
        if (all_zero) ++zero;
    }
    const double n = static_cast<double>(kCrop) * kCrop;
    char line[300];
    std::snprintf(line, sizeof(line),
                  "PROBE out center %ux%u: mean RGBA=(%.4f %.4f %.4f %.4f) min=%.3f max=%.3f zero_pixels=%.1f%% nonfinite=%u first_px=(%.3f %.3f %.3f %.3f)",
                  kCrop, kCrop, sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n, lo, hi, 100.0 * zero / n, bad,
                  PssrHalf(px[0]), PssrHalf(px[1]), PssrHalf(px[2]), PssrHalf(px[3]));
    if (log_it) Eden::Report("pssr", line);
    // Picture in it: most pixels are not all-zero and nothing is NaN or infinite.
    return zero * 2 < kCrop * kCrop && bad == 0;
}


// ---- GPU time of the PSSR packets, from the GPU's own clock: a timestamp before them and one after.
struct PssrTimer {
    static constexpr uint32_t kSlots = 64;
    vk::QueryPool pool;
    PFN_vkCmdWriteTimestamp write{};
    double period_ns = 0;
    uint64_t next = 0, first_pending = 0;
    bool failed = false;
    double sum_us = 0;
    uint32_t n = 0;
};
PssrTimer* const g_pssr_timer = new PssrTimer;

// The slot of this frame's two timestamps, or -1 (no timestamps, or the ring is full of unread ones).
int PssrTimerSlot(const Device& device) {
    PssrTimer& t = *g_pssr_timer;
    if (t.failed) return -1;
    if (!t.write) {
        const auto& dld = device.GetDispatchLoader();
        t.write = reinterpret_cast<PFN_vkCmdWriteTimestamp>(
            dld.vkGetDeviceProcAddr(*device.GetLogical(), "vkCmdWriteTimestamp"));
        t.period_ns = device.GetPhysical().GetProperties().limits.timestampPeriod;
        if (!t.write || t.period_ns <= 0) { t.failed = true; return -1; }
        t.pool = device.GetLogical().CreateQueryPool({
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = PssrTimer::kSlots * 2,
            .pipelineStatistics = 0,
        });
    }
    if (t.next - t.first_pending >= PssrTimer::kSlots) return -1;
    return static_cast<int>(t.next++ % PssrTimer::kSlots);
}

// Reads the stamps the GPU has written so far (never waits) and publishes the average of every 120.
void PssrTimerCollect(const Device& device) {
    PssrTimer& t = *g_pssr_timer;
    if (t.failed || !t.write) return;
    while (t.first_pending < t.next) {
        const uint32_t slot = static_cast<uint32_t>(t.first_pending % PssrTimer::kSlots);
        std::array<uint64_t, 2> stamps{};
        if (device.GetLogical().GetQueryResults(*t.pool, slot * 2, 2, sizeof(stamps), stamps.data(),
                                               sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) break;
        const double us = stamps[1] > stamps[0] ? static_cast<double>(stamps[1] - stamps[0]) * t.period_ns / 1000.0 : 0.0;
        t.sum_us += us;
        ++t.n;
        ++t.first_pending;
        if (t.n == 120) {
            Eden::Hud::pssr_gpu_us.store(static_cast<uint32_t>(t.sum_us / t.n + 0.5), std::memory_order_relaxed);
            t.sum_us = 0;
            t.n = 0;
        }
    }
}

// ---- Does the output look like THIS frame? A crop of the input (1:1) and a crop of the 2160p output (half
// size, so each of its 2x2 blocks averages into one pixel) are copied by the GPU into small float images and
// compared on the CPU, aligned and shifted. Verdict: 1 = same picture, 2 = output empty, 3 = a picture that
// does not match the input, 0 = could not be checked.
struct PssrFid {
    vk::Image a, b;
};
PssrFid* const g_pssr_fid = new PssrFid;

uint32_t PssrFidelity(MemoryAllocator& allocator, Scheduler& scheduler, VkImage source_image, VkExtent2D in,
                      VkImage out_image, VkExtent2D out, bool log_it,
                      const char* who) {
    constexpr uint32_t kAw = 192, kAh = 108;   // the input crop, 1:1
    constexpr uint32_t kBw = 128, kBh = 72;    // the output crop, halved
    if (in.width < kAw || in.height < kAh || out.width < 2 * kBw || out.height < 2 * kBh) return 0;
    const auto make = [&](uint32_t w, uint32_t h) {
        return allocator.CreateImage(VkImageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R16G16B16A16_SFLOAT,
            .extent = {w, h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        });
    };
    PssrFid& fid = *g_pssr_fid;
    if (!fid.a) {
        fid.a = make(kAw, kAh);
        fid.b = make(kBw, kBh);
    }
    vk::Buffer buf_a = CreateWrappedBuffer(allocator, static_cast<VkDeviceSize>(kAw) * kAh * 8, MemoryUsage::Download);
    vk::Buffer buf_b = CreateWrappedBuffer(allocator, static_cast<VkDeviceSize>(kBw) * kBh * 8, MemoryUsage::Download);
    const VkImage image_a = *fid.a, image_b = *fid.b;
    const VkBuffer raw_a = *buf_a, raw_b = *buf_b;
    const int32_t sx = static_cast<int32_t>(in.width / 2 - kAw / 2), sy = static_cast<int32_t>(in.height / 2 - kAh / 2);
    const int32_t ox = static_cast<int32_t>(out.width / 2 - kBw), oy = static_cast<int32_t>(out.height / 2 - kBh);
    const VkImageSubresourceLayers layers{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
    const VkImageBlit blit_a{.srcSubresource = layers,
                             .srcOffsets = {{sx, sy, 0}, {sx + static_cast<int32_t>(kAw), sy + static_cast<int32_t>(kAh), 1}},
                             .dstSubresource = layers,
                             .dstOffsets = {{0, 0, 0}, {static_cast<int32_t>(kAw), static_cast<int32_t>(kAh), 1}}};
    const VkImageBlit blit_b{.srcSubresource = layers,
                             .srcOffsets = {{ox, oy, 0}, {ox + 2 * static_cast<int32_t>(kBw), oy + 2 * static_cast<int32_t>(kBh), 1}},
                             .dstSubresource = layers,
                             .dstOffsets = {{0, 0, 0}, {static_cast<int32_t>(kBw), static_cast<int32_t>(kBh), 1}}};
    const VkBufferImageCopy copy_a{.bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0, .imageSubresource = layers,
                                   .imageOffset = {0, 0, 0}, .imageExtent = {kAw, kAh, 1}};
    const VkBufferImageCopy copy_b{.bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0, .imageSubresource = layers,
                                   .imageOffset = {0, 0, 0}, .imageExtent = {kBw, kBh, 1}};
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        TransitionImageLayout(cmdbuf, source_image, VK_IMAGE_LAYOUT_GENERAL);
        TransitionImageLayout(cmdbuf, out_image, VK_IMAGE_LAYOUT_GENERAL);
        TransitionImageLayout(cmdbuf, image_a, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_UNDEFINED);
        TransitionImageLayout(cmdbuf, image_b, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_UNDEFINED);
        constexpr VkMemoryBarrier everything{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, everything);
        cmdbuf.BlitImage(source_image, VK_IMAGE_LAYOUT_GENERAL, image_a, VK_IMAGE_LAYOUT_GENERAL, blit_a, VK_FILTER_NEAREST);
        cmdbuf.BlitImage(out_image, VK_IMAGE_LAYOUT_GENERAL, image_b, VK_IMAGE_LAYOUT_GENERAL, blit_b, VK_FILTER_LINEAR);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, everything);
        cmdbuf.CopyImageToBuffer(image_a, VK_IMAGE_LAYOUT_GENERAL, raw_a, copy_a);
        cmdbuf.CopyImageToBuffer(image_b, VK_IMAGE_LAYOUT_GENERAL, raw_b, copy_b);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, everything);
    });
    scheduler.Finish();
    buf_a.Invalidate();
    buf_b.Invalidate();
    const auto* pa = reinterpret_cast<const uint16_t*>(buf_a.Mapped().data());
    const auto* pb = reinterpret_cast<const uint16_t*>(buf_b.Mapped().data());
    // The output crop (kBw x kBh) against the input crop placed at (ax, ay): mean absolute difference over RGB and
    // the Pearson correlation of the luminance (insensitive to a change of tone or gamma between the two).
    struct Cmp { double mae, r, std_a; };
    const auto lum = [](const uint16_t* p) {
        return 0.2126 * PssrHalf(p[0]) + 0.7152 * PssrHalf(p[1]) + 0.0722 * PssrHalf(p[2]);
    };
    const auto compare = [&](uint32_t ax, uint32_t ay, bool flip_y) {
        double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, mae = 0;
        const double n = static_cast<double>(kBw) * kBh;
        for (uint32_t y = 0; y < kBh; ++y) {
            const uint32_t yy = flip_y ? kBh - 1 - y : y;
            for (uint32_t x = 0; x < kBw; ++x) {
                const uint16_t* a = pa + (static_cast<size_t>(ay + yy) * kAw + ax + x) * 4;
                const uint16_t* b = pb + (static_cast<size_t>(y) * kBw + x) * 4;
                const double la = lum(a), lb = lum(b);
                sa += la; sb += lb; saa += la * la; sbb += lb * lb; sab += la * lb;
                for (int c = 0; c < 3; ++c) mae += std::fabs(PssrHalf(a[c]) - PssrHalf(b[c])) / 3.0;
            }
        }
        const double ma = sa / n, mb = sb / n;
        const double va = saa / n - ma * ma, vb = sbb / n - mb * mb, cov = sab / n - ma * mb;
        return Cmp{mae / n, (va > 1e-8 && vb > 1e-8) ? cov / std::sqrt(va * vb) : 0.0, std::sqrt(std::max(va, 0.0))};
    };
    const Cmp aligned = compare((kAw - kBw) / 2, (kAh - kBh) / 2, false);
    const Cmp shifted = compare(0, 0, false);                       // the input moved by 32 x 18 pixels
    const Cmp flipped = compare((kAw - kBw) / 2, (kAh - kBh) / 2, true);   // upside down (a diagnosis only)
    double mean_out = 0;
    uint32_t zero = 0;
    for (uint32_t i = 0; i < kBw * kBh; ++i) {
        bool all_zero = true;
        for (int c = 0; c < 4; ++c) {
            const float v = PssrHalf(pb[i * 4 + c]);
            if (v != 0.0f) all_zero = false;
            if (c < 3) mean_out += v;
        }
        if (all_zero) ++zero;
    }
    mean_out /= 3.0 * kBw * kBh;
    // 2 = empty; 0 = the input crop is flat, so a likeness cannot be judged; 1 = it follows the input (high
    // correlation, clearly better aligned than shifted); 3 = a picture that does not follow the input.
    uint32_t verdict;
    if (zero * 2 > kBw * kBh) verdict = 2;
    else if (aligned.std_a < 0.01) verdict = 0;
    // A nearly uniform picture correlates with everything: if the shifted input matches almost as well, the
    // check cannot tell a good result from a bad one, so it says "not judged" instead of a false MISMATCH.
    else if (aligned.r >= 0.85 && shifted.r >= 0.90) verdict = 0;
    else if ((aligned.r >= 0.85 && aligned.r > shifted.r + 0.05) ||
             // A busy, moving picture (Odyssey): the FSR reference scores 0.64 on average there, so a lower
             // correlation still counts when it is far above the one of the shifted input.
             (aligned.r >= 0.50 && aligned.r > shifted.r + 0.25)) verdict = 1;
    else verdict = 3;
    if (verdict == 1)
        Eden::Hud::pssr_diff_milli.store(static_cast<uint32_t>(std::max(0.0, aligned.r) * 1000.0 + 0.5),
                                        std::memory_order_relaxed);   // the likeness, not the (tone-biased) error
    if (log_it || verdict != 1) {
        char line[330];
        std::snprintf(line, sizeof(line),
                      "FIDELITY[%s] r aligned=%.3f shifted=%.3f flipped=%.3f | mae aligned=%.4f | std_in=%.3f mean_out=%.4f empty_px=%.1f%% -> %s",
                      who, aligned.r, shifted.r, flipped.r, aligned.mae, aligned.std_a, mean_out,
                      100.0 * zero / (kBw * kBh),
                      verdict == 1 ? "FOLLOWS INPUT" : verdict == 2 ? "EMPTY" : verdict == 0 ? "FLAT INPUT, NOT JUDGED" : "MISMATCH");
        Eden::Report("pssr", line);
    }
    return verdict;
}

// Runs PSSR 2 from `source_view` (the game's picture) into `out_image`. True: the commands are
// recorded and `out_image` will hold the picture; false: nothing was recorded (FSR runs as usual).
bool PssrDraw(const Device& device, MemoryAllocator& allocator, Scheduler& scheduler, VkImage source_image,
              VkImageView source_view, VkExtent2D in, VkExtent2D out, VkImage out_image,
              VkImageView out_view) {
    PssrAux& aux = *g_pssr_aux;
    if (aux.extent.width != in.width || aux.extent.height != in.height) {
        scheduler.Finish();
        // FASE2-VERIFICAR: vulkan_pssr_flow.comp declares `motion` as rgba16f but this image is R16G16_SFLOAT
        // (the format PSSR's T# was built from, kept as is). The store qualifier should become rg16f in the
        // shader (one word; AMD stores by the descriptor's format anyway, but the validator will object).
        aux.motion = CreateWrappedImage(allocator, in, VK_FORMAT_R16G16_SFLOAT);
        aux.depth = CreateWrappedImage(allocator, in, VK_FORMAT_R32_SFLOAT);
        aux.motion_view = CreateWrappedImageView(device, aux.motion, VK_FORMAT_R16G16_SFLOAT);
        aux.depth_view = CreateWrappedImageView(device, aux.depth, VK_FORMAT_R32_SFLOAT);
        // The previous picture for the optical flow: the game's format is not known here, so a float image
        // that a blit converts into (the same as PssrFidelity's crops), sampled by the shader.
        aux.prev = allocator.CreateImage(VkImageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R16G16B16A16_SFLOAT,
            .extent = {in.width, in.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        });
        aux.prev_view = CreateWrappedImageView(device, aux.prev, VK_FORMAT_R16G16B16A16_SFLOAT);
        aux.first_frame = true;
        aux.extent = in;
        const VkImage motion = *aux.motion;
        const VkImage depth = *aux.depth;
        const VkImage prev = *aux.prev;
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([=](vk::CommandBuffer cmdbuf) {
            ClearColorImage(cmdbuf, motion);
            ClearColorImage(cmdbuf, depth);
            ClearColorImage(cmdbuf, prev);   // and from UNDEFINED to GENERAL, where it stays
        });
        scheduler.Finish();
    }
    PssrFlowCreate(device);

    // The 32-byte texture descriptors RADV built for the four views (the library copies them).
    alignas(32) uint32_t d_out[8]{};
    alignas(32) uint32_t d_color[8]{};
    alignas(32) uint32_t d_motion[8]{};
    alignas(32) uint32_t d_depth[8]{};
    vkEdenGetImageViewDescriptor(out_view, VK_TRUE, d_out);
    vkEdenGetImageViewDescriptor(source_view, VK_FALSE, d_color);
    vkEdenGetImageViewDescriptor(*aux.motion_view, VK_FALSE, d_motion);
    vkEdenGetImageViewDescriptor(*aux.depth_view, VK_FALSE, d_depth);

    // The commands run IN PLACE: libScePsml writes into them the absolute addresses of data inside the same
    // buffer (the texture descriptors), so the buffer is a slot of the GPU-mapped arena. It starts as valid
    // PM4 (type-3 NOP headers) so whatever the library leaves unwritten is harmless and the written part can
    // be told apart.
    uint32_t* const words = Eden::PssrBridge::AcquireCommandSlot();
    if (!words) return false;
    constexpr size_t kWords = Eden::PssrBridge::kCommandSlotDwords;
    const auto nop_fill = [](size_t i) { return (i & 1) ? 0u : 0xC0001000u; };
    for (size_t i = 0; i < kWords; ++i) words[i] = nop_fill(i);
    Eden::PssrBridge::PssrDispatchDesc desc;
    desc.cmd_buffer = words;
    desc.cmd_dwords = static_cast<uint32_t>(kWords);
    auto& p = desc.params;
    p.output_color = d_out;
    p.color = d_color;
    p.motion_vectors = d_motion;
    p.depth = d_depth;
    p.pre_exposure = 1.0f;
    p.render_w = in.width;
    p.render_h = in.height;
    p.near_z = 0.1f;
    p.far_z = 1000.0f;
    // The first picture after a (re)creation has no previous one: the motion is zero and PSSR's history is
    // reset (pssr_bridge.h, `reset`, assumed to be that flag).
    p.reset = aux.first_frame ? 1 : 0;
    const int rc = Eden::PssrBridge::DispatchPSSR(desc);
    if (g_pssr_reports < 24 && (rc != 0 || g_pssr_reports < 3)) {
        ++g_pssr_reports;
        char line[320];
        std::snprintf(line, sizeof(line),
                      "draw %ux%u -> %ux%u rc=0x%08x first=%08x %08x last=%08x %08x desc_color=%08x %08x %08x %08x",
                      in.width, in.height, out.width, out.height, static_cast<unsigned>(rc), words[0],
                      words[1], words[2046], words[2047], d_color[0], d_color[1], d_color[2], d_color[3]);
        Eden::Report("pssr", line);
    }
    if (rc != 0) return false;
    size_t used = 0;
    for (size_t i = kWords; i > 0; --i) {
        if (words[i - 1] != nop_fill(i - 1)) { used = i; break; }
    }
    if (g_pssr_dumps < 1) {
        ++g_pssr_dumps;
        char head[96];
        std::snprintf(head, sizeof(head), "dump: %zu of %zu dwords written by libScePsml, slot at %p", used, kWords, static_cast<void*>(words));
        Eden::Report("pssr", head);
        const size_t shown = used < 704 ? used : 704;   // all of it: it is decoded off line
        for (size_t at = 0; at < shown; at += 8) {
            char row[160];
            int n = std::snprintf(row, sizeof(row), "dump[%03zu]:", at);
            for (size_t k = at; k < at + 8 && k < shown; ++k)
                n += std::snprintf(row + n, sizeof(row) - n, " %08x", words[k]);
            Eden::Report("pssr", row);
        }
    }
    // The library closes the buffer with ONE NOP whose header says "skip the rest of the 2048 dwords"; the
    // written part ends with that header alone. Injected without its body it would make the command
    // processor skip the next 1400 dwords of the stream it joins (and hang the GPU), so the stream is cut at
    // the end of the last COMPLETE packet.
    {
        size_t at = 0;
        size_t complete = 0;
        while (at < used) {
            const uint32_t header = words[at];
            if ((header >> 30) != 3) break;
            const size_t n = ((header >> 16) & 0x3fff) + 2;
            if (at + n > used) break;
            at += n;
            complete = at;
        }
        used = complete;
    }
    if (used == 0) return false;

    const VkImage motion = *aux.motion;
    const VkImage depth = *aux.depth;
    // ---- Optical flow: this frame's descriptor set (cur = the game's view, prev, motion), the sign and the
    // handles the recording below needs (the lambda copies them; the wrappers themselves cannot be copied).
    const bool flow = aux.flow_ok;
    const VkImage prev = flow ? *aux.prev : VkImage{};
    const VkPipeline flow_pipeline = flow ? *aux.flow_pipeline : VkPipeline{};
    const VkPipelineLayout flow_layout = flow ? *aux.flow_layout : VkPipelineLayout{};
    VkDescriptorSet flow_set{};
    // sign 0 on the first picture (nothing to compare with) and whatever the knob says afterwards.
    const float flow_sign = (flow && !aux.first_frame)
        ? static_cast<float>(Eden::PssrBridge::flow_sign.load(std::memory_order_relaxed)) : 0.0f;
    if (flow) {
        flow_set = aux.flow_sets[aux.flow_frame++ % PssrAux::kFlowSets];
        // FASE2-VERIFICAR: the set is rewritten every frame (source_view may change); the ring of kFlowSets
        // keeps a set unchanged for longer than the scheduler keeps a command buffer in flight. The two
        // sampled images use CreateWriteDescriptorSet as the HUD does (it fills layout GENERAL, which is the
        // layout both images are in); `infos` must be reserved first, the helper keeps pointers into it.
        std::vector<VkDescriptorImageInfo> infos;
        infos.reserve(2);
        const VkDescriptorImageInfo motion_info{
            .sampler = VK_NULL_HANDLE,
            .imageView = *aux.motion_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        // FASE2-VERIFICAR: no STORAGE_IMAGE write exists in the port; a raw VkWriteDescriptorSet for binding 2.
        const VkWriteDescriptorSet motion_write{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = flow_set,
            .dstBinding = 2,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &motion_info,
            .pBufferInfo = nullptr,
            .pTexelBufferView = nullptr,
        };
        device.GetLogical().UpdateDescriptorSets(
            std::array{CreateWriteDescriptorSet(infos, *aux.flow_sampler, source_view, flow_set, 0),
                       CreateWriteDescriptorSet(infos, *aux.flow_sampler, *aux.prev_view, flow_set, 1),
                       motion_write},
            {});
    }
    const int slot = PssrTimerSlot(device);
    const VkQueryPool pool = slot >= 0 ? *g_pssr_timer->pool : VkQueryPool{};
    const PFN_vkCmdWriteTimestamp write_stamp = g_pssr_timer->write;
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        TransitionImageLayout(cmdbuf, source_image, VK_IMAGE_LAYOUT_GENERAL);
        TransitionImageLayout(cmdbuf, motion, VK_IMAGE_LAYOUT_GENERAL);
        TransitionImageLayout(cmdbuf, depth, VK_IMAGE_LAYOUT_GENERAL);
        TransitionImageLayout(cmdbuf, out_image, VK_IMAGE_LAYOUT_GENERAL);
        constexpr VkMemoryBarrier everything{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                               everything);
        if (flow) {
            // Optical flow before the packets that read `motion`: the game's picture and `prev` are sampled
            // (both GENERAL, the layout the barrier above left them in) and `motion` is stored (GENERAL).
            // FASE2-VERIFICAR: BindPipeline/BindDescriptorSets/PushConstants are used by the HUD with the
            // graphics bind point; Dispatch is not used anywhere else in the port (vk::CommandBuffer::Dispatch).
            cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, flow_pipeline);
            cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, flow_layout, 0, flow_set, {});
            const PssrFlowPush push{in.width, in.height, 4, flow_sign};
            cmdbuf.PushConstants(flow_layout, VK_SHADER_STAGE_COMPUTE_BIT, push);
            cmdbuf.Dispatch((in.width + 7) / 8, (in.height + 7) / 8, 1);
            // The compute writes of `motion` before anything PSSR's packets do (the same coarse barrier the
            // pass already uses around the packets).
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                                   everything);
        }
        // Inline, not an IB2: the stream holds driver commands (NOP packets with a marker) that only the
        // submission of the main stream resolves. Its absolute addresses point into the slot, which stays.
        const int cut = Eden::PssrBridge::test_cut.load(std::memory_order_relaxed);
        const uint32_t count = cut < 0 ? static_cast<uint32_t>(used) : static_cast<uint32_t>(std::min<size_t>(used, cut));
        // The GPU's clock before and after the packets (the barriers around them make the stamps wait for the
        // work before and for the PSSR work itself).
        if (slot >= 0) {
            cmdbuf.ResetQueryPool(pool, static_cast<u32>(slot) * 2, 2);
            write_stamp(*cmdbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, static_cast<u32>(slot) * 2);
        }
        if (cut == -3) {
            // Negative control for the fidelity check: a flat green picture instead of PSSR's, which is not
            // empty but does not look like the input either.
            const VkClearColorValue green{.float32 = {0.0f, 0.8f, 0.0f, 1.0f}};
            const std::array<VkImageSubresourceRange, 1> ranges{{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}}};
            cmdbuf.ClearColorImage(out_image, VK_IMAGE_LAYOUT_GENERAL, green, ranges);
        } else if (count) {
            vkEdenCmdInjectDwords(*cmdbuf, words, count);
        }
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                               everything);
        if (slot >= 0) write_stamp(*cmdbuf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, static_cast<u32>(slot) * 2 + 1);
        if (flow) {
            // This picture becomes the previous one of the next frame. A blit, not a copy: the game's format
            // is not necessarily R16G16B16A16_SFLOAT (the same call PssrFidelity makes on source_image, so
            // the image allows it). Both stay GENERAL.
            const VkImageSubresourceLayers layers{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
            const VkImageBlit blit{
                .srcSubresource = layers,
                .srcOffsets = {{0, 0, 0}, {static_cast<int32_t>(in.width), static_cast<int32_t>(in.height), 1}},
                .dstSubresource = layers,
                .dstOffsets = {{0, 0, 0}, {static_cast<int32_t>(in.width), static_cast<int32_t>(in.height), 1}},
            };
            cmdbuf.BlitImage(source_image, VK_IMAGE_LAYOUT_GENERAL, prev, VK_IMAGE_LAYOUT_GENERAL, blit,
                             VK_FILTER_NEAREST);
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                                   everything);
        }
    });
    aux.first_frame = false;   // from now on `prev` holds the last picture PSSR was given
    // The overlay says what happened, so it is checked, not assumed: the first read-back after 30 frames and
    // then one every 600 (a stall of a few milliseconds each) tells whether PSSR leaves a picture.
    static unsigned calls = 0;
    ++calls;
    PssrTimerCollect(device);
    // A flat picture (a sky, a title screen) cannot be judged: the overlay then goes back to "not checked"
    // instead of keeping an old verdict as if it were current, and the check is repeated sooner.
    static bool last_not_judged = false;
    if (calls == 30 || (calls > 30 && (calls % 600 == 0 || (last_not_judged && calls % 120 == 0)))) {
        const uint32_t verdict = PssrFidelity(allocator, scheduler, source_image, in, out_image, out, calls <= 60, "PSSR");
        last_not_judged = verdict == 0;
        Eden::Hud::upscaler_verified.store(verdict, std::memory_order_relaxed);   // 0 = not judged
    }
    return true;
}
} // namespace
#endif
