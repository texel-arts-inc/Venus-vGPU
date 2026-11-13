# RTX validation

These are targeted checks through the macOS Venus ICD, standalone QEMU,
and the reference NVIDIA Linux guest. The reference card is an RTX 4000
SFF Ada running NVIDIA 620.70.06 through a Thunderbolt 3 enclosure.

## Vulkan CTS

The Vulkan CTS revision is `0d18b347943e5e843529a05c95220e27fe30c538`.
Stock CTS reported 27 passes and two skips across the 29 cases below.
There were no test failures. The synchronization2 timeline chain completed
32,768 submissions; its isolated run took 2.36 seconds after Venus stopped
waiting on a transport fence for each timeline signal. Command-buffer
storage now starts with smaller blocks on macOS to fit the shared aperture
when one process keeps several Vulkan instances alive.

Both `wsi.metal.swapchain.create` cases below use 32-bit image usage flags.
The CTS support check also requires `VK_KHR_extended_flags`, which Venus
at these pins does not advertise. For local verification, apply
[`legacy-swapchain-gate.patch`](../patches/vulkan-cts/legacy-swapchain-gate.patch)
to the pinned upstream CTS checkout and rebuild `deqp-vk`. The patch leaves
that extension requirement in place for tests that enable the extension.
The combined run with this one harness change passed all 29 cases. Record
patched and stock results separately. These samples do not establish
Vulkan conformance.

```text
dEQP-VK.api.device_init.create_device.basic
dEQP-VK.api.device_init.create_device_features2.basic
dEQP-VK.api.device_init.create_device_queue_priorities.concurrent_submission
dEQP-VK.api.device_init.create_instance_null_appinfo.basic
dEQP-VK.api.version_check.entry_points
dEQP-VK.api.version_check.unavailable_entry_points
dEQP-VK.api.version_check.version
dEQP-VK.memory.mapping.dedicated_alloc.buffer.full.1048577.simple_map2
dEQP-VK.memory.mapping.suballocation.full.33.simple_map2
dEQP-VK.memory.mapping.suballocation.full.4087.remap_map2
dEQP-VK.synchronization2.basic.binary_semaphore.one_queue
dEQP-VK.synchronization2.basic.timeline_semaphore.chain
dEQP-VK.synchronization2.basic.timeline_semaphore.multi_queue
dEQP-VK.synchronization2.basic.timeline_semaphore.one_queue
dEQP-VK.wsi.metal.surface.create
dEQP-VK.wsi.metal.surface.query_capabilities
dEQP-VK.wsi.metal.surface.query_formats
dEQP-VK.wsi.metal.surface.query_present_modes
dEQP-VK.wsi.metal.surface.query_support
dEQP-VK.wsi.metal.swapchain.acquire.too_many
dEQP-VK.wsi.metal.swapchain.acquire.too_many_timeout
dEQP-VK.wsi.metal.swapchain.create.image_format
dEQP-VK.wsi.metal.swapchain.create.min_image_count
dEQP-VK.wsi.metal.swapchain.destroy.old_swapchain
dEQP-VK.wsi.metal.swapchain.get_images.count
dEQP-VK.wsi.metal.swapchain.get_images.incomplete
dEQP-VK.wsi.metal.swapchain.modify.resize
dEQP-VK.wsi.metal.swapchain.render.basic
dEQP-VK.wsi.metal.swapchain.render.basic2
```

Save the list to a file and pass it to `deqp-vk` with
`--deqp-caselist-file`. Point `VK_DRIVER_FILES` at the tested Venus ICD
manifest and `--deqp-vk-library-path` at the Vulkan loader. Use the data
from the same CTS build via `--deqp-archive-dir`.
