#!/usr/bin/env python3
"""RADV: hooks that let the application run ready-made GPU packets in a command buffer (PSSR 2).

libScePsml writes the PM4 packets of one upscale dispatch into a buffer of its own, and they take
their input and output pictures as 32-byte AGC texture descriptors (T#, 8 dwords). The PS5 winsys
copies the words of every command stream as they are and submits them (sceAgcDriverSubmitDcb), so
the packets only need to be appended to the stream of the command buffer that is being recorded.

  vkEdenCmdInjectDwords       appends the words, then drops every state RADV caches (the packets were
                              written for a stream of their own and change registers RADV believes it
                              knows), as vkCmdExecuteCommands does after a secondary buffer.
  vkEdenGetImageViewDescriptor copies the 8 dwords RADV built for an image view (sampled or storage).
  vkEdenCmdExecuteIndirect    emits an IB2 that runs a buffer IN PLACE (va, dwords) and returns. libScePsml's
                              packets carry the absolute addresses of data inside the same buffer (its
                              texture descriptors), so they must run where they were written: that memory has
                              to be mapped for the GPU at the same address as for the CPU.

The names begin with "vk" and a capital so tools/isolate-radv.py keeps them public. Applied to the
driver checkout after the other patch-radv-*.py; running it again replaces the block it wrote.
"""
from pathlib import Path
import sys
import hashlib

fingerprint = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
if sys.argv[1:2] == ['--digest']:
    print(fingerprint)
    raise SystemExit(0)
if sys.argv[1:2] == ['--check']:
    stamp = Path(sys.argv[2])
    raise SystemExit(0 if stamp.is_file() and stamp.read_text().strip() == fingerprint else 1)

root = Path(__file__).resolve().parents[1]
src = Path(sys.argv[1]) if len(sys.argv) > 1 else root.parent / 'mihawk-vulkan-review/.deps/work/radv-src'
vulkan = src / 'src/amd/vulkan'

BEGIN = '/* ProsperoEden: PSSR 2 hooks begin (tools/patch-radv-pssr.py) */\n'
END = '/* ProsperoEden: PSSR 2 hooks end */\n\n'
hook = BEGIN + '''VKAPI_ATTR void VKAPI_CALL vkEdenCmdInjectDwords(VkCommandBuffer commandBuffer, const uint32_t *dwords,
                                                uint32_t count);
VKAPI_ATTR void VKAPI_CALL vkEdenGetImageViewDescriptor(VkImageView imageView, VkBool32 storage, uint32_t *out);
VKAPI_ATTR void VKAPI_CALL vkEdenCmdExecuteIndirect(VkCommandBuffer commandBuffer, uint64_t va, uint32_t dwords);

VKAPI_ATTR void VKAPI_CALL
vkEdenCmdInjectDwords(VkCommandBuffer commandBuffer, const uint32_t *dwords, uint32_t count)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   radeon_check_space(device->ws, cs->b, count);
   radeon_begin(cs);
   radeon_emit_array(dwords, count);
   radeon_end();

   radv_invalidate_state(cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
vkEdenGetImageViewDescriptor(VkImageView imageView, VkBool32 storage, uint32_t *out)
{
   VK_FROM_HANDLE(radv_image_view, iview, imageView);
   const union radv_descriptor *descriptor = storage ? &iview->storage_descriptor : &iview->descriptor;

   memcpy(out, descriptor->plane_descriptors[0], 8 * sizeof(uint32_t));
}

VKAPI_ATTR void VKAPI_CALL
vkEdenCmdExecuteIndirect(VkCommandBuffer commandBuffer, uint64_t va, uint32_t dwords)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   radeon_check_space(device->ws, cs->b, 4);
   /* No CHAIN bit: an IB2, which runs the buffer and goes on with this one. */
   ac_emit_cp_indirect_buffer(cs->b, va, dwords, 0, false);

   radv_invalidate_state(cmd_buffer);
}
''' + END

cmd = vulkan / 'radv_cmd_buffer.c'
text = cmd.read_text()
anchor = 'VKAPI_ATTR void VKAPI_CALL\nradv_CmdExecuteCommands('
changed = False
if BEGIN in text:
    a = text.index(BEGIN)
    b = text.index(END, a) + len(END)
    if text[a:b] != hook:
        text = text[:a] + hook + text[b:]
        changed = True
else:
    assert text.count(anchor) == 1, 'radv_CmdExecuteCommands moved'
    # A tree patched by the first wording (no markers) carries a bare vkEdenCmdInjectDwords: drop it.
    old = text.find('/* ProsperoEden: PSSR 2 (tools/patch-radv-pssr.py). Appends ready-made PM4 words')
    if old != -1:
        end_old = text.index(anchor, old)
        text = text[:old] + text[end_old:]
    text = text.replace(anchor, hook + anchor)
    changed = True
if changed:
    cmd.write_text(text)
    # Upstream skips the whole build while its receipt names the pinned revision: set the receipt
    # aside so the next build compiles the changed files (Ninja keeps every other object).
    provenance = src.parents[1] / 'native/radv-release/PROVENANCE.txt'
    if provenance.exists():
        provenance.replace(provenance.with_suffix('.base.txt'))
print('RADV PSSR: vkEdenCmdInjectDwords, vkEdenGetImageViewDescriptor (changed=%s)' % changed)
