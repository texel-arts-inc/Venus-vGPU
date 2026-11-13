# Ollama Vulkan benchmark on the RTX 4000 SFF Ada

Ollama 0.40.16 ran natively on Apple M5 with macOS 27.3. The CMake build
uses llama.cpp `6c01da6278ab6c69ed67d9124806b50d2b299ba7` with Vulkan enabled
and Metal and MLX disabled. `Vulkan0` identified the RTX 4000 SFF Ada through
Venus (Vulkan 1.4.343; 20,475 MiB reported by the device). The Fedora reference
guest used NVIDIA's 620.70.06 driver. The enclosure negotiated Thunderbolt 3
and BAR1 stayed at 256 MiB. All layers went to the RTX: 41 of 41 for Devstral
and 29 of 29 for Qwen. Ollama reported 13.91 GiB and 4.43 GiB in VRAM,
respectively.

| Model | Sample | Model load | New prompt tokens | Cached tokens | Prompt tokens/s | Output tokens/s |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Devstral Small 2 24B | Unloaded | 31.94 s | 521 | 0 | 365.44 | 12.34 |
| Devstral Small 2 24B | Resident 1 | 0.59 ms | 520 | 1 | 208.95 | 12.26 |
| Devstral Small 2 24B | Resident 2 | 0.39 ms | 520 | 1 | 419.61 | 11.60 |
| Devstral Small 2 24B | Resident 3 | 0.56 ms | 521 | 1 | 397.74 | 12.00 |
| Qwen2.5-Coder 7B | Unloaded | 8.57 s | 546 | 0 | 1,145.46 | 27.32 |
| Qwen2.5-Coder 7B | Resident | 1.18 ms | 514 | 32 | 1,472.96 | 30.22 |

Use **Devstral Small 2 24B** for the larger local coding model and
**Qwen2.5-Coder 7B** for faster iteration. These measurements compare
inference throughput; they do not compare coding accuracy or image input.

Both models used Q4_K_M weights, a 4,096-token context, and one request at
a time. Each request produced 128 tokens with temperature 0 and seed 42.
The requests were text-only and varied between samples. Prompt throughput
uses the number of newly processed tokens; decode throughput excludes load
and prefill time. An unloaded run includes initialization and upload from
local storage. Resident runs reuse weights and may reuse a prompt prefix.
The Devstral numbers use the CMake-staged build. Qwen was measured with a
separate build of the same Ollama and llama.cpp revisions. The small sample
shows observed throughput and is not a sustained performance guarantee.

| Ollama tag | Tested manifest SHA-256 |
| --- | --- |
| `devstral-small-2:24b` | `24277f07f62db8f9cb68e9dfc679ea1818a7fbac47a50eff0a701d3f645b63c8` |
| `qwen2.5-coder:7b` | `dae161e27b0e90dd1856c8bb3209201fd6736d8eb66298e75ed87571486f4364` |

See the [Ollama build and measurement instructions](../inference/README.md)
to reproduce the configuration. Model weights and generated responses are
outside this source distribution.
