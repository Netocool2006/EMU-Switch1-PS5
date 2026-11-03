#ifdef ENABLE_PSSR
    {
        // One line each time the size entering FSR changes: says whether the game reaches here below the output.
        static uint32_t seen_w = 0, seen_h = 0;
        if (seen_w != input_image_extent.width || seen_h != input_image_extent.height) {
            seen_w = input_image_extent.width;
            seen_h = input_image_extent.height;
            char line[120];
            std::snprintf(line, sizeof(line), "FSR::Draw input %ux%u, output %ux%u", seen_w, seen_h, m_extent.width,
                          m_extent.height);
            Eden::Report("pssr", line);
        }
    }
    // PSSR 2 takes the place of EASU and RCAS once its context exists and the picture is below the
    // output size; if it refuses (or is not ready) FSR runs as always.
    Eden::Hud::upscaler_out_w.store(m_extent.width, std::memory_order_relaxed);
    Eden::Hud::upscaler_out_h.store(m_extent.height, std::memory_order_relaxed);
    if (Eden::PssrBridge::ready.load(std::memory_order_relaxed) &&
        Eden::PssrBridge::test_cut.load(std::memory_order_relaxed) != -2 &&   // -2: FSR as a reference
        (input_image_extent.width < m_extent.width || input_image_extent.height < m_extent.height)) {
        if (PssrDraw(device, m_memory_allocator, scheduler, source_image, source_image_view, input_image_extent,
                     m_extent, *images.images[Rcas], *images.image_views[Rcas])) {
            // What the overlay says is what happened this frame and what the last read-back of the output found.
            const uint32_t seen = Eden::Hud::upscaler_verified.load(std::memory_order_relaxed);
            Eden::Hud::upscaler.store(seen == 1 ? Eden::Hud::kUpscalerPssrVerified
                                      : seen == 2 ? Eden::Hud::kUpscalerPssrNoImage
                                      : seen == 3 ? Eden::Hud::kUpscalerPssrMismatch
                                                  : Eden::Hud::kUpscalerPssrUnchecked,
                                      std::memory_order_relaxed);
            return *images.image_views[Rcas];
        }
        Eden::Hud::upscaler.store(Eden::Hud::kUpscalerFsrAfterPssrError, std::memory_order_relaxed);
    } else {
        Eden::Hud::upscaler.store(Eden::Hud::kUpscalerFsr, std::memory_order_relaxed);
    }
#endif
