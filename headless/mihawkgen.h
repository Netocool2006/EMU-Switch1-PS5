// ProsperoEden - MihawkGen neural frame generation wrapper.
// Adapts PS5_MihawkGen's C API (framegen.h) to Eden's Vulkan present path.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HAS_MIHAWKGEN

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>

struct fg_context;

namespace Eden {

class MihawkGen {
public:
    struct Config {
        PFN_vkGetInstanceProcAddr gipa;
        VkInstance instance;
        VkDevice device;
        VkPhysicalDevice physical_device;
        uint32_t width;
        uint32_t height;
        uint32_t slots;
        VkFormat output_format;
    };

    MihawkGen();
    ~MihawkGen();

    MihawkGen(const MihawkGen&) = delete;
    MihawkGen& operator=(const MihawkGen&) = delete;

    bool Create(const Config& config);
    bool LoadWeights(const void* blob, size_t size);
    bool LoadWeightsFromFile(const char* path);
    void Destroy();

    bool IsReady() const;
    // The images the generator reads from (diagnostics: they must never be the image it writes to).
    VkImage PrevImage() const { return prev_image_; }
    VkImage CurrImage() const { return curr_image_; }

    void SubmitFrame(VkImage color_image, VkImageView color_view);

    size_t GenerationCount() const;

    // Records barriers + compute dispatches for one generated frame. The caller
    // must NOT have transitioned the inputs or output — this method handles all
    // layout transitions required by the framegen API.
    void RecordGenerate(VkCommandBuffer cmd, uint32_t slot, float t,
                        VkImage output_image, VkImageView output_view);

private:
    fg_context* ctx_ = nullptr;
    bool network_loaded_ = false;
    uint64_t frame_count_ = 0;
    VkImage prev_image_ = VK_NULL_HANDLE;
    VkImageView prev_color_ = VK_NULL_HANDLE;
    VkImage curr_image_ = VK_NULL_HANDLE;
    VkImageView curr_color_ = VK_NULL_HANDLE;
    std::vector<uint8_t> weights_data_;
    // Measured cadence of real game frames (seconds between SubmitFrame calls).
    std::chrono::steady_clock::time_point last_submit_{};
    double interval_ema_ = 0.0;
    uint32_t interval_samples_ = 0;
    // How many frames are added between two real ones (0..3), and the controller that picks it. The cadence
    // seen here is bounded below by the presentation itself (each added frame takes one refresh), so it
    // cannot say how fast the game could go: the level is lowered on trial and given back if the game
    // does not keep up.
    int level_ = 0;
    bool level_known_ = false;
    bool probing_ = false;
    bool low_active_ = false;                              // the game has been under 40 fps since low_since_
    std::chrono::steady_clock::time_point low_since_{};
    double backoff_s_ = 4.0;
    std::chrono::steady_clock::time_point next_probe_{};
    // Test switches read in Create() from the app folder (fg-variant.txt, fg-level.txt).
    VkImageLayout input_old_layout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    int fixed_level_ = -1;            // 0..3 = fixed level, -1 = automatic controller
    void RestartMeasure() { interval_ema_ = 0.0; interval_samples_ = 0; }
    void UpdateLevel(std::chrono::steady_clock::time_point now);
};

} // namespace Eden

#endif // HAS_MIHAWKGEN
