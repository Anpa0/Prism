# Performance roadmap

Version 0.1 deliberately uses the simplest correct frame transport. Nothing
below is a prerequisite for it, and none of it should be attempted before
capture and ReShade are working end to end.

## Where the time goes today

Per frame, at 2560×1440 BGRx (14.7 MB):

| Stage | Cost | Notes |
| --- | --- | --- |
| PipeWire → mailbox | one memcpy | on the capture thread |
| mailbox → D3D11 dynamic texture | one memcpy | on the render thread, into a mapped upload buffer |
| upload → GPU | driver-side | DXVK stages this into VRAM |
| draw + present | negligible | one triangle |

So two CPU copies and one host-to-device transfer. The diagnostics panel breaks
this out: `Texture upload`, `Draw`, `Present`, and end-to-end `Capture → present`.

Measure before optimising. On a 1080p or 1440p source at 60 Hz, the copies are
not the bottleneck; at 4K and 144 Hz they start to matter.

## 1. Remove the second copy

The cheapest real win, and it needs no new platform support: have the render
thread map the D3D11 dynamic texture and let the capture thread copy PipeWire's
buffer straight into the mapped pointer, turning two copies into one.

The catch is that mapping is a device-context operation and the D3D11 immediate
context is not free-threaded, so this needs either a deferred context or a
ring of pre-mapped staging buffers with the map/unmap kept on the render thread.
Prototype it behind a setting and compare `Texture upload` before and after.

## 2. PipeWire DMA-BUF import

The real prize. KWin can export capture buffers as DMA-BUFs, which never touch
system memory at all.

* Negotiate `SPA_PARAM_BUFFERS_dataType` with `SPA_DATA_DmaBuf` alongside the
  current `MemPtr` path, and keep the memcpy path as the fallback — the bridge
  already counts and recycles buffers it cannot map (`Dropped unmappable`).
* The bridge receives a DMA-BUF fd, modifier and per-plane offsets.

Getting from there into D3D11 is the hard part, and it is a Wine problem rather
than a Prism one:

* import the fd into Vulkan with `VK_EXT_external_memory_dma_buf` and
  `VK_EXT_image_drm_format_modifier`;
* hand the resulting `VkImage` to DXVK as the backing of a D3D11 texture. DXVK
  has no public interface for adopting a foreign image, so this needs either a
  DXVK extension or a Wine-side shim.

Treat this as a research task with a written-up outcome, not a scheduled
feature. The fallback path must keep working whatever happens.

## 3. Shared-resource transport

If DXVK gains a way to import external memory, the same fd could back a D3D11
shared resource (`D3D11_RESOURCE_MISC_SHARED_NTHANDLE`). Worth re-evaluating
whenever DXVK's external-memory support changes.

## 4. Latency reduction

Independent of copies, and cheaper to land:

* **Waitable swap chain.** `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`
  plus `SetMaximumFrameLatency(1)` removes a frame of queueing. Check DXVK
  support first.
* **Present timing.** Prism presents on arrival today. Presenting just ahead of
  the compositor's deadline, derived from the PipeWire `pts` stream, would cut
  the wait without dropping frames.
* **Ceiling accuracy.** The ceiling currently uses a simple minimum interval. A
  drift-compensated accumulator would keep 143.8 fps from reading as 144.

## 5. Higher capture rates

* Confirm the negotiated framerate range is not being clamped by the portal
  backend; the bridge already asks for up to 1000/1.
* Watch `Dropped at bridge` versus `Dropped as stale` to see whether the
  bottleneck is the bridge's ceiling or the renderer.

## 6. Colour and HDR

Out of scope for 0.1 beyond "change nothing". When it is time:

* negotiate 10-bit formats and map to `DXGI_FORMAT_R10G10B10A2_UNORM`;
* an `_SRGB` or scRGB swap chain plus `IDXGISwapChain4::SetColorSpace1`;
* ReShade's own HDR handling needs checking before any of it is exposed.

Until then the rule stands: no encoding, no subsampling, no gamma adjustment, no
automatic sharpening, no scaling that was not asked for.
