# Cornell box

The demo renders one Cornell box scene three ways. The vertex and mesh paths use the same geometry and lighting shader so their images should match. The ray query path uses the same triangles to build a Vulkan BLAS and TLAS, then traces visibility rays and up to five diffuse bounces per sample.

| Key | Mode | Vulkan path |
| --- | --- | --- |
| **1** | Vertex rasterization | Vertex buffer and `vkCmdDraw` |
| **2** | Mesh shader rasterization | `VK_EXT_mesh_shader` and `vkCmdDrawMeshTasksEXT` |
| **3** | Ray query path tracing | `VK_KHR_ray_query` and `VK_KHR_acceleration_structure` |

Press **S** to save a PNG of the current mode, or **Esc** to close the window. The title shows the active mode and reports when a requested mode is unavailable on the selected device.

## Build

Install a Vulkan loader and current Vulkan headers, SDL3, libpng, and glslang. For example, from the repository root:

```sh
cmake -S . -B build -DVENUS_BUILD_DEMO=ON -DVENUS_BUILD_HOST=OFF
cmake --build build --target venus-cornell
```

The demo requires Vulkan 1.2 at runtime. Mesh and ray query modes also require their corresponding Vulkan features. Use the Vulkan loader's `VK_DRIVER_FILES` environment variable to select the Venus ICD if your loader does not already discover it.

## Run

```sh
build/demo/venus-cornell --list-modes
build/demo/venus-cornell
build/demo/venus-cornell --headless --mode ray --width 720 --height 720 \
  --samples 384 --capture cornell-ray.png
```

Replace `--headless` with `--once` to present and capture one frame through the SDL window. The PNG is read back from the Vulkan color image used for presentation. Interactive ray query rendering defaults to 16 samples per pixel. Use `--samples` to change the quality.

For the throughput measurement, keep the window visible and run each mode
three times. The benchmark warms up for 30 frames and reports the pixel
extent, elapsed time, and application frame rate for the timed iterations.

```sh
build/demo/venus-cornell --mode raster --width 720 --height 720 --benchmark-frames 180
build/demo/venus-cornell --mode mesh --width 720 --height 720 --benchmark-frames 180
build/demo/venus-cornell --mode ray --width 720 --height 720 --samples 16 --benchmark-frames 180
```

## Captures

These 720×720 frames were rendered and presented with Venus on an NVIDIA RTX 4000 SFF Ada. The ray query capture used 384 samples per pixel.

| [Vertex rasterization](screenshots/cornell-raster.png) | [Mesh shader rasterization](screenshots/cornell-mesh.png) | [Ray query path tracing](screenshots/cornell-ray.png) |
| --- | --- | --- |
| ![Vertex rasterization](screenshots/cornell-raster.png) | ![Mesh shader rasterization](screenshots/cornell-mesh.png) | ![Ray query path tracing](screenshots/cornell-ray.png) |
