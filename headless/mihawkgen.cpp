// ProsperoEden - MihawkGen neural frame generation wrapper.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HAS_MIHAWKGEN

#include "mihawkgen.h"
#include "framegen.h"
#include "diagnostics.h"
#include "mihawkgen_bridge.h"
#include "storage_paths.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace Eden {

MihawkGen::MihawkGen() = default;

MihawkGen::~MihawkGen() {
    Destroy();
}

bool MihawkGen::Create(const Config& config) {
    Destroy();

    Eden::Report("framegen",
                 ("MihawkGen Create: " + std::to_string(config.width) + "x" +
                  std::to_string(config.height) + " slots=" +
                  std::to_string(config.slots)).c_str());

    if (!config.gipa) {
        Eden::Report("framegen", "MihawkGen: null gipa in config");
        return false;
    }
    Eden::Report("framegen", "MihawkGen: initializing volk");
    volkInitializeCustom(config.gipa);
    volkLoadInstance(config.instance);
    volkLoadDevice(config.device);
    Eden::Report("framegen", "MihawkGen: volk ready");

    fg_create_info info{};
    info.device = config.device;
    info.physical_device = config.physical_device;
    info.width = config.width;
    info.height = config.height;
    info.slots = config.slots;
    info.output_format = config.output_format;

    char error[256] = {};
    Eden::Report("framegen", "MihawkGen: calling fg_context_create");
    VkResult result = fg_context_create(&info, &ctx_, error, sizeof(error));
    if (result != VK_SUCCESS) {
        Eden::Report("framegen",
                     ("MihawkGen: fg_context_create failed: " + std::string(error)).c_str());
        ctx_ = nullptr;
        return false;
    }
    Eden::Report("framegen", "MihawkGen: context created OK");

    // Test switches read from the app folder, so variants can be tried without rebuilding.
    // fg-variant.txt: oldLayout of the two input barriers ("undefined", "general", "color" = default).
    {
        std::string word;
        std::ifstream in(Eden::AppFile("fg-variant.txt"));
        if (in) {
            in >> word;
        }
        if (word == "undefined") {
            input_old_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
        } else if (word == "general") {
            input_old_layout_ = VK_IMAGE_LAYOUT_GENERAL;
        } else {
            input_old_layout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            word = "color";
        }
        Eden::Report("framegen", ("MihawkGen: input oldLayout = " + word).c_str());
    }
    // The level: without fg-level.txt it is what Settings > Video > Frame gen level chose
    // (MihawkGenBridge::level, set by main.cpp: -1 the controller, which adds nothing to a game that
    // makes 60 and only starts after 10 seconds under 40 fps; 0..3 fixed). fg-level.txt in the app
    // folder is a development override that wins when it exists: "0".."3" fixes the level, "auto"
    // (or anything else) enables the controller.
    {
        std::ifstream in(Eden::AppFile("fg-level.txt"));
        const bool from_file = static_cast<bool>(in);
        if (!from_file) {
            fixed_level_ = std::clamp(Eden::MihawkGenBridge::level.load(), -1, 3);
        } else {
            std::string word;
            in >> word;
            if (word.size() == 1 && word[0] >= '0' && word[0] <= '3') {
                fixed_level_ = word[0] - '0';
            } else {
                fixed_level_ = -1;    // "auto" (or anything else): automatic controller
            }
        }
        Eden::Report("framegen",
                     ((fixed_level_ >= 0 ? "MihawkGen: fixed level = " + std::to_string(fixed_level_)
                                         : std::string("MihawkGen: level = auto")) +
                      (from_file ? " (fg-level.txt)" : " (settings)")).c_str());
    }
    return true;
}

bool MihawkGen::LoadWeights(const void* blob, size_t size) {
    if (!ctx_) {
        return false;
    }
    char error[256] = {};
    Eden::Report("framegen",
                 ("MihawkGen: loading " + std::to_string(size) + " bytes of weights").c_str());
    VkResult result = fg_context_load_network(ctx_, blob, size, error, sizeof(error));
    if (result != VK_SUCCESS) {
        Eden::Report("framegen",
                     ("MihawkGen: load_network failed: " + std::string(error)).c_str());
        network_loaded_ = false;
        return false;
    }
    Eden::Report("framegen", "MihawkGen: weights loaded OK");
    network_loaded_ = true;
    return true;
}

bool MihawkGen::LoadWeightsFromFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        Eden::Report("framegen",
                     ("MihawkGen: cannot open weights: " + std::string(path)).c_str());
        return false;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        fclose(f);
        return false;
    }
    weights_data_.resize(static_cast<size_t>(len));
    size_t read = fread(weights_data_.data(), 1, weights_data_.size(), f);
    fclose(f);
    if (read != weights_data_.size()) {
        weights_data_.clear();
        return false;
    }
    bool ok = LoadWeights(weights_data_.data(), weights_data_.size());
    if (!ok) {
        weights_data_.clear();
    }
    return ok;
}

void MihawkGen::Destroy() {
    if (ctx_) {
        fg_context_destroy(ctx_);
        ctx_ = nullptr;
    }
    network_loaded_ = false;
    frame_count_ = 0;
    interval_ema_ = 0.0;
    interval_samples_ = 0;
    level_ = 0;
    level_known_ = false;
    probing_ = false;
    low_active_ = false;
    backoff_s_ = 4.0;
    prev_image_ = VK_NULL_HANDLE;
    prev_color_ = VK_NULL_HANDLE;
    curr_image_ = VK_NULL_HANDLE;
    curr_color_ = VK_NULL_HANDLE;
    weights_data_.clear();
}

bool MihawkGen::IsReady() const {
    return ctx_ && network_loaded_;
}

void MihawkGen::SubmitFrame(VkImage color_image, VkImageView color_view) {
    // Track how often the game actually delivers frames.
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - last_submit_).count();
    last_submit_ = now;
    if (dt >= 0.5 || dt <= 0.0) {
        // First frame, stall or loading screen: start measuring again.
        interval_ema_ = 0.0;
        interval_samples_ = 0;
    } else if (dt > 0.004) {
        interval_ema_ = (interval_samples_ == 0) ? dt : interval_ema_ * 0.9 + dt * 0.1;
        if (interval_samples_ < 1000) {
            ++interval_samples_;
        }
    }

    UpdateLevel(now);

    prev_image_ = curr_image_;
    prev_color_ = curr_color_;
    curr_image_ = color_image;
    curr_color_ = color_view;
    ++frame_count_;
}

void MihawkGen::UpdateLevel(std::chrono::steady_clock::time_point now) {
    if (fixed_level_ >= 0) {
        level_ = fixed_level_;        // fg-level.txt fixes the level: no estimate, no trials
        level_known_ = true;
        probing_ = false;
        return;
    }
    if (interval_samples_ == 0) {
        return;                       // a stall just reset the measure: keep the level, wait for new data
    }
    if (interval_samples_ < 12) {
        return;
    }
    const int target_setting = MihawkGenBridge::target_rate.load(std::memory_order_relaxed);
    const double target =
        target_setting > 0 ? static_cast<double>(std::min(target_setting, 120)) : 60.0;
    // Frames are added only for a game that stays well below the target: a game that makes 60 and dips for a
    // moment (a busy fight) must not get interpolated frames, which made it flicker and slow down. The
    // measure has to stay under 40 fps for 10 seconds in a row before the controller starts to add any.
    if (!level_known_ || level_ == 0) {
        if (interval_ema_ <= 1.0 / 40.0) {
            low_active_ = false;
            return;                   // keeps up (or close): nothing is added
        }
        if (!low_active_) {
            low_active_ = true;
            low_since_ = now;
            return;
        }
        if (now - low_since_ < std::chrono::seconds(10)) {
            return;
        }
    }
    if (!level_known_) {
        // First estimate: target fps / game fps, so 30 fps adds 1, 20 adds 2, 15 adds 3, 60 adds none.
        level_ = std::clamp(static_cast<int>(std::floor(target * interval_ema_ - 1.0 + 0.35)), 0, 3);
        level_known_ = true;
        next_probe_ = now + std::chrono::seconds(4);
        RestartMeasure();
        return;
    }
    // The cadence a game that keeps up would show at this level.
    const double expected = (level_ + 1) / target;
    const bool keeps_up = interval_ema_ <= expected * 1.12;
    if (!keeps_up && level_ < 3) {
        ++level_;                     // the game is slower than this level allows: add more
        if (probing_) {
            probing_ = false;         // the trial failed: wait longer before the next one
            backoff_s_ = std::min(backoff_s_ * 2.0, 60.0);
        }
        next_probe_ = now + std::chrono::milliseconds(static_cast<long long>(backoff_s_ * 1000.0));
        RestartMeasure();
    } else if (keeps_up && probing_) {
        probing_ = false;             // the trial worked: the game can do with fewer added frames
        backoff_s_ = 4.0;
        next_probe_ = now + std::chrono::seconds(4);
        RestartMeasure();
    } else if (keeps_up && level_ > 0 && now >= next_probe_) {
        --level_;                     // try one less; the next measures decide whether it stays
        probing_ = true;
        RestartMeasure();
    }
}

size_t MihawkGen::GenerationCount() const {
    if (!IsReady() || frame_count_ < 2 || prev_color_ == VK_NULL_HANDLE || curr_color_ == VK_NULL_HANDLE ||
        !level_known_) {
        return 0;
    }
    return static_cast<size_t>(level_);
}

static void RecordBarrier(VkCommandBuffer cmd, VkImage image,
                          VkAccessFlags src_access, VkAccessFlags dst_access,
                          VkImageLayout old_layout, VkImageLayout new_layout,
                          VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void MihawkGen::RecordGenerate(VkCommandBuffer cmd, uint32_t slot, float t,
                                VkImage output_image, VkImageView output_view) {
    if (!IsReady() || prev_color_ == VK_NULL_HANDLE || curr_color_ == VK_NULL_HANDLE) {
        return;
    }

    // framegen API: inputs in SHADER_READ_ONLY_OPTIMAL, output in GENERAL
    RecordBarrier(cmd, prev_image_,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  input_old_layout_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    RecordBarrier(cmd, curr_image_,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  input_old_layout_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    RecordBarrier(cmd, output_image,
                  0, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    fg_frame_pair pair{};
    pair.prev_color = prev_color_;
    pair.next_color = curr_color_;
    pair.prev_depth = VK_NULL_HANDLE;
    pair.next_depth = VK_NULL_HANDLE;
    pair.next_motion = VK_NULL_HANDLE;
    pair.t = t;
    fg_record_generate(ctx_, cmd, slot, FG_METHOD_NEURAL, &pair, output_view);

    // Transition output to transfer source for the present blit
    RecordBarrier(cmd, output_image,
                  VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
}

} // namespace Eden

#endif // HAS_MIHAWKGEN
