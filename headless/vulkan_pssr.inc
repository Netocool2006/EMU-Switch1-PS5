// SPDX-License-Identifier: GPL-3.0-or-later
// PSSR 2 present-path probe. Injected after each presented frame by prepare-vulkan-port.py.
// It only COUNTS frames and reports PSSR state. Real dispatch needs an AGC command buffer and
// GPU addresses of the frame images, which the Vulkan path does not provide (see pssr_bridge.cpp).
#ifdef ENABLE_PSSR
    Eden::PssrBridge::NoteFrame(frame->width, frame->height,
                                Eden::Hud::guest_width.load(std::memory_order_relaxed),
                                Eden::Hud::guest_height.load(std::memory_order_relaxed));
#endif
