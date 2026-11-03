#ifdef ENABLE_PSSR
    // Reference for the test knob cut=-2: the same likeness check as PSSR's, on what FSR just drew. FSR is a
    // trustworthy upscaler, so this is what calibrates the check.
    if (Eden::PssrBridge::test_cut.load(std::memory_order_relaxed) == -2) {
        static unsigned fsr_calls = 0;
        ++fsr_calls;
        if (fsr_calls == 60 || (fsr_calls > 60 && fsr_calls % 600 == 0))
            PssrFidelity(m_memory_allocator, scheduler, source_image, input_image_extent, rcas_image, m_extent, true,
                         "FSR reference");
    }
#endif
