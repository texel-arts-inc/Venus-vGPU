# Ollama on Venus

The optional CMake target builds Ollama 0.40.16 for Apple Silicon with the
llama.cpp Vulkan backend. The build disables Metal and MLX and stages the
CLI, runner, Vulkan backend, and licenses in the CMake build directory.
The target fetches and verifies the Ollama and llama.cpp Git revisions and
applies the [Ollama patches](../patches/ollama).

Install Xcode Command Line Tools, CMake 3.24 or newer, Git, Ninja, Go 1.26.10
or newer, Vulkan headers and loader, shaderc (`glslc`), and SPIRV-Headers.
For a Homebrew toolchain, use:

```sh
brew install cmake git ninja go vulkan-headers vulkan-loader shaderc spirv-headers
cmake -S . -B build-inference -G Ninja \
  -DVENUS_BUILD_HOST=OFF -DVENUS_BUILD_OLLAMA=ON \
  -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build build-inference --target venus-ollama
```

You can point CMake to Go with `VENUS_OLLAMA_GO` and set the native
build parallelism with `VENUS_OLLAMA_JOBS`. Building requires network
access to the pinned Git sources and Go dependencies.

Start a Venus guest that exposes a Vulkan device. The launcher defaults to
the installed ICD manifest. Set `VENUS_ICD` to an absolute manifest path
to use a different Venus build. It starts a local server on
`127.0.0.1:11435` with a separate model cache and a 4,096-token context.

```sh
build-inference/ollama/ollama-venus serve
```

From a second terminal in the same working directory:

```sh
build-inference/ollama/ollama-venus pull devstral-small-2:24b
build-inference/ollama/ollama-venus run devstral-small-2:24b --verbose
build-inference/ollama/ollama-venus ps
```

Confirm that the server log identifies the intended Vulkan device and
that `ps` reports GPU offload. The launcher verifies that the Venus Vulkan
device can be enumerated before starting the server. The model download
is independent of the build and uses the model's own license.

For benchmark measurements, send the same request to
`/api/generate` with `stream` set to `false` and record `load_duration`,
`prompt_eval_count`, `prompt_eval_duration`, `eval_count`, and
`eval_duration`. The durations are in nanoseconds. Unload the model with
`ollama-venus stop <model>` for an unloaded-model sample. Use distinct
prompts for warmed samples and check `prompt_eval_cached_count` before
computing prompt throughput. Record the actual context size and VRAM
reported by `/api/ps`.

See the [RTX benchmark results](../docs/OLLAMA-BENCHMARK.md) for per-run
timings, model digests, and Vulkan offload details.
