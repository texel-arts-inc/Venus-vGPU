// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <vulkan/vulkan.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_vulkan.h>
#include <png.h>

#include "fullscreen_vert.hpp"
#include "raster_frag.hpp"
#include "raster_mesh.hpp"
#include "raster_vert.hpp"
#include "ray_frag.hpp"
#include "scene.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

enum class Mode { raster, mesh, ray };
constexpr std::array modes{Mode::raster, Mode::mesh, Mode::ray};

const char* modeName(Mode mode) {
    switch (mode) {
    case Mode::raster: return "raster";
    case Mode::mesh: return "mesh";
    case Mode::ray: return "ray";
    }
    throw std::logic_error("Invalid renderer mode");
}

const char* modeTitle(Mode mode) {
    switch (mode) {
    case Mode::raster: return "Vertex rasterization";
    case Mode::mesh: return "Mesh shader rasterization";
    case Mode::ray: return "Ray query path tracing";
    }
    throw std::logic_error("Invalid renderer mode");
}

struct Options {
    Mode mode = Mode::raster;
    uint32_t width = 900;
    uint32_t height = 900;
    uint32_t samples = 16;
    std::string capture;
    bool headless = false;
    bool list = false;
    bool once = false;
    bool help = false;
};

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        const auto value = [&]() -> std::string_view {
            if (++i >= argc) {
                throw std::runtime_error(std::string(argument) + " requires a value");
            }
            return argv[i];
        };
        const auto number = [&](uint32_t maximum) {
            const auto input = value();
            uint32_t parsed = 0;
            const auto result = std::from_chars(input.data(), input.data() + input.size(), parsed);
            if (result.ec != std::errc{} || result.ptr != input.data() + input.size() ||
                parsed == 0 || parsed > maximum) {
                throw std::runtime_error(std::string(argument) + " must be between 1 and " +
                                         std::to_string(maximum));
            }
            return parsed;
        };
        if (argument == "--mode") {
            const auto name = value();
            const auto mode = std::find_if(modes.begin(), modes.end(),
                                           [&](Mode entry) { return name == modeName(entry); });
            if (mode == modes.end()) {
                throw std::runtime_error("--mode requires raster, mesh, or ray");
            }
            options.mode = *mode;
        } else if (argument == "--width") {
            options.width = number(8192);
        } else if (argument == "--height") {
            options.height = number(8192);
        } else if (argument == "--samples") {
            options.samples = number(1024);
        } else if (argument == "--capture") {
            options.capture = value();
            if (options.capture.empty()) {
                throw std::runtime_error("--capture requires a PNG path");
            }
        } else if (argument == "--headless") {
            options.headless = true;
        } else if (argument == "--list-modes") {
            options.list = true;
        } else if (argument == "--once") {
            options.once = true;
        } else if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else {
            throw std::runtime_error("Unknown argument: " + std::string(argument));
        }
    }
    return options;
}

void check(VkResult result, const char* action) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(action) + " returned VkResult " +
                                 std::to_string(result));
    }
}

void sdlCheck(bool result, const char* action) {
    if (!result) {
        throw std::runtime_error(std::string(action) + ": " + SDL_GetError());
    }
}

template<class T, class Enumerate>
std::vector<T> enumerate(Enumerate enumerateValues) {
    VkResult result;
    std::vector<T> values;
    do {
        uint32_t count = 0;
        check(enumerateValues(&count, static_cast<T*>(nullptr)), "Vulkan enumeration count");
        if (count == 0) return {};
        values.resize(count);
        result = enumerateValues(&count, values.data());
        values.resize(count);
    } while (result == VK_INCOMPLETE);
    check(result, "Vulkan enumeration");
    return values;
}

bool hasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    return std::any_of(extensions.begin(), extensions.end(), [&](const auto& extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

struct Buffer {
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;

    Buffer() = default;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    ~Buffer() { reset(); }

    void reset() {
        if (handle) {
            vkDestroyBuffer(device, handle, nullptr);
            handle = VK_NULL_HANDLE;
        }
        if (memory) {
            vkFreeMemory(device, memory, nullptr);
            memory = VK_NULL_HANDLE;
        }
    }
};

struct Image {
    VkImage handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;

    void reset(VkDevice device) {
        if (view) vkDestroyImageView(device, view, nullptr);
        if (handle) vkDestroyImage(device, handle, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
        *this = {};
    }
};

struct AccelerationStructure {
    Buffer storage;
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
};

struct DeviceChoice {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    uint32_t family = 0;
    bool mesh = false;
    bool ray = false;
    bool portability = false;

    bool supports(Mode mode) const {
        switch (mode) {
        case Mode::raster: return true;
        case Mode::mesh: return mesh;
        case Mode::ray: return ray;
        }
        return false;
    }
};

struct PushConstants {
    float width, height;
    uint32_t samples;
    uint32_t encodeSrgb;
};

constexpr auto hostMemory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

class Renderer {
public:
    Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    ~Renderer();

    void initialize(const Options& options, SDL_Window* window);
    void initializeTargets(uint32_t width, uint32_t height);
    bool draw(Mode mode, uint32_t samples, const std::string& capture);
    bool supports(Mode mode) const { return choice.supports(mode); }

private:
    void createInstance(SDL_Window* window);
    void selectDevice(Mode requested, bool list);
    void createDevice();
    void createCommands();
    void createScene();
    void createDescriptors();
    void createSwapchain(uint32_t width, uint32_t height);
    void createRenderPass();
    void createPipeline(Mode mode);
    void destroyTargets();

    uint32_t memoryType(uint32_t allowed, VkMemoryPropertyFlags required) const;
    void createBuffer(Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags properties);
    void upload(const Buffer& buffer, const void* data, VkDeviceSize size);
    VkDeviceAddress addressOf(VkBuffer buffer) const;
    void createImage(Image& image, VkFormat format, VkImageUsageFlags usage,
                     VkImageAspectFlags aspect);
    void buildSceneAccelerationStructures();
    void buildAccelerationStructure(AccelerationStructure& target,
                                    VkAccelerationStructureTypeKHR type,
                                    const VkAccelerationStructureGeometryKHR& geometry,
                                    uint32_t primitiveCount);
    void beginCommands();
    void submitCommands(VkSemaphore acquired = VK_NULL_HANDLE,
                        VkSemaphore ready = VK_NULL_HANDLE);
    void waitCommands();
    void recordReadback(const Buffer& readback);
    void recordPresent(VkImage destination);
    void savePng(const Buffer& readback, const std::string& path);

    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    DeviceChoice choice;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence submitted = VK_NULL_HANDLE;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> swapImages;
    std::vector<VkSemaphore> presentReady;
    VkExtent2D extent{};
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    Image color, depth;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkShaderStageFlags pushStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    std::array<VkPipeline, modes.size()> pipelines{};
    uint32_t vertexCount = 0;
    Buffer scene;
    AccelerationStructure bottomLevel, topLevel;
    uint32_t scratchAlignment = 0;
    PFN_vkCreateAccelerationStructureKHR createAS = nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroyAS = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR getASBuildSizes = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR getASAddress = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR cmdBuildAS = nullptr;
    PFN_vkCmdDrawMeshTasksEXT cmdDrawMeshTasks = nullptr;
};

Renderer::~Renderer() {
    if (device) {
        vkDeviceWaitIdle(device);
        destroyTargets();
        if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        if (setLayout) vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        if (topLevel.handle) destroyAS(device, topLevel.handle, nullptr);
        if (bottomLevel.handle) destroyAS(device, bottomLevel.handle, nullptr);
        topLevel.storage.reset();
        bottomLevel.storage.reset();
        scene.reset();
        if (imageAvailable) vkDestroySemaphore(device, imageAvailable, nullptr);
        if (submitted) vkDestroyFence(device, submitted, nullptr);
        if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
        vkDestroyDevice(device, nullptr);
    }
    if (surface) SDL_Vulkan_DestroySurface(instance, surface, nullptr);
    if (instance) vkDestroyInstance(instance, nullptr);
}

void Renderer::initialize(const Options& options, SDL_Window* window) {
    createInstance(window);
    selectDevice(options.mode, options.list);
    if (options.list) {
        return;
    }
    createDevice();
    createCommands();
    createScene();
    createDescriptors();
}

void Renderer::createInstance(SDL_Window* window) {
    const auto available = enumerate<VkExtensionProperties>([](uint32_t* count, auto* values) {
        return vkEnumerateInstanceExtensionProperties(nullptr, count, values);
    });
    std::vector<const char*> extensions;
    if (window) {
        uint32_t count = 0;
        const auto* required = SDL_Vulkan_GetInstanceExtensions(&count);
        sdlCheck(required != nullptr, "SDL Vulkan instance extensions");
        extensions.assign(required, required + count);
    }

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "Venus Cornell Box";
    application.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    if (hasExtension(available, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        const auto* name = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
        const auto hasName = [&](const char* extension) { return std::strcmp(extension, name) == 0; };
        if (std::none_of(extensions.begin(), extensions.end(), hasName)) {
            extensions.push_back(name);
        }
        info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    info.pApplicationInfo = &application;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    check(vkCreateInstance(&info, nullptr, &instance), "Create Vulkan instance");
    if (window) {
        sdlCheck(SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface),
                 "Create Vulkan window surface");
    }
}

void Renderer::selectDevice(Mode requested, bool list) {
    const auto devices = enumerate<VkPhysicalDevice>([&](uint32_t* count, auto* values) {
        return vkEnumeratePhysicalDevices(instance, count, values);
    });
    int bestScore = -1;
    bool compatibleDevice = false;
    for (const auto physical : devices) {
        DeviceChoice candidate;
        candidate.physical = physical;
        vkGetPhysicalDeviceProperties(physical, &candidate.properties);
        if (candidate.properties.apiVersion < VK_API_VERSION_1_2) continue;

        const auto families = enumerate<VkQueueFamilyProperties>([&](uint32_t* count, auto* values) {
            vkGetPhysicalDeviceQueueFamilyProperties(physical, count, values);
            return VK_SUCCESS;
        });
        const auto extensions = enumerate<VkExtensionProperties>([&](uint32_t* count, auto* values) {
            return vkEnumerateDeviceExtensionProperties(physical, nullptr, count, values);
        });
        const auto has = [&](const char* name) { return hasExtension(extensions, name); };
        if (surface && !has(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
        std::optional<uint32_t> family;
        for (uint32_t i = 0; i < families.size(); ++i) {
            if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || families[i].queueCount == 0) {
                continue;
            }
            VkBool32 present = VK_TRUE;
            if (surface) {
                check(vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &present),
                      "Query presentation support");
            }
            if (!present) continue;
            if (!family) family = i;
            // Ray query uses the graphics queue to build its acceleration structures.
            if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                family = i;
                break;
            }
        }
        if (!family) continue;
        compatibleDevice = true;
        candidate.family = *family;
        candidate.portability = has("VK_KHR_portability_subset");

        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        VkPhysicalDeviceBufferDeviceAddressFeatures bufferAddress{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        VkPhysicalDeviceMeshShaderFeaturesEXT meshShader{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
        features.pNext = &bufferAddress;
        void** next = &bufferAddress.pNext;
        if (has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
            has(VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
            has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)) {
            *next = &acceleration;
            acceleration.pNext = &rayQuery;
            next = &rayQuery.pNext;
        }
        if (has(VK_EXT_MESH_SHADER_EXTENSION_NAME)) {
            *next = &meshShader;
        }
        vkGetPhysicalDeviceFeatures2(physical, &features);

        VkFormatProperties vertices{};
        vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R32G32B32_SFLOAT, &vertices);
        candidate.mesh = meshShader.meshShader;
        candidate.ray = rayQuery.rayQuery && acceleration.accelerationStructure &&
                        bufferAddress.bufferDeviceAddress &&
                        (families[*family].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
                        (vertices.bufferFeatures &
                         VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR);
        if (list) {
            std::cout << candidate.properties.deviceName << '\n';
            for (const auto mode : modes) {
                std::cout << "  " << modeName(mode) << ' '
                          << (candidate.supports(mode) ? "available" : "unavailable") << '\n';
            }
        }

        if (candidate.supports(requested)) {
            const int score = static_cast<int>(candidate.mesh) + static_cast<int>(candidate.ray) +
                (candidate.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1 : 0);
            if (score > bestScore) {
                bestScore = score;
                choice = candidate;
            }
        }
    }

    if (!choice.physical && !list) {
        throw std::runtime_error(std::string("No compatible Vulkan device supports mode ") +
                                 modeName(requested) + ". Run --list-modes to inspect features.");
    }
    if (list && !compatibleDevice) {
        throw std::runtime_error("No Vulkan 1.2 devices with a graphics queue found");
    }
    if (!list) {
        std::cout << "Vulkan device: " << choice.properties.deviceName << '\n';
    }
}

template<class Function>
Function deviceFunction(VkDevice device, const char* name) {
    const auto function = reinterpret_cast<Function>(vkGetDeviceProcAddr(device, name));
    if (!function) throw std::runtime_error(std::string("Vulkan function missing: ") + name);
    return function;
}

void Renderer::createDevice() {
    std::vector<const char*> extensions;
    if (surface) extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (choice.portability) extensions.push_back("VK_KHR_portability_subset");

    VkPhysicalDeviceBufferDeviceAddressFeatures bufferAddress{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceMeshShaderFeaturesEXT meshShader{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    void** next = &features.pNext;
    if (choice.ray) {
        extensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        extensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        extensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
        bufferAddress.bufferDeviceAddress = VK_TRUE;
        acceleration.accelerationStructure = VK_TRUE;
        rayQuery.rayQuery = VK_TRUE;
        *next = &bufferAddress;
        bufferAddress.pNext = &acceleration;
        acceleration.pNext = &rayQuery;
        next = &rayQuery.pNext;
    }
    if (choice.mesh) {
        extensions.push_back(VK_EXT_MESH_SHADER_EXTENSION_NAME);
        meshShader.meshShader = VK_TRUE;
        *next = &meshShader;
        pushStages |= VK_SHADER_STAGE_MESH_BIT_EXT;
    }

    const float priority = 1;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = choice.family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &features;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    check(vkCreateDevice(choice.physical, &info, nullptr, &device), "Create Vulkan device");
    vkGetDeviceQueue(device, choice.family, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(choice.physical, &memoryProperties);

    if (choice.mesh) {
        cmdDrawMeshTasks = deviceFunction<PFN_vkCmdDrawMeshTasksEXT>(device, "vkCmdDrawMeshTasksEXT");
    }
    if (choice.ray) {
        createAS = deviceFunction<PFN_vkCreateAccelerationStructureKHR>(
            device, "vkCreateAccelerationStructureKHR");
        destroyAS = deviceFunction<PFN_vkDestroyAccelerationStructureKHR>(
            device, "vkDestroyAccelerationStructureKHR");
        getASBuildSizes = deviceFunction<PFN_vkGetAccelerationStructureBuildSizesKHR>(
            device, "vkGetAccelerationStructureBuildSizesKHR");
        getASAddress = deviceFunction<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
            device, "vkGetAccelerationStructureDeviceAddressKHR");
        cmdBuildAS = deviceFunction<PFN_vkCmdBuildAccelerationStructuresKHR>(
            device, "vkCmdBuildAccelerationStructuresKHR");
        VkPhysicalDeviceAccelerationStructurePropertiesKHR properties{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 extended{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        extended.pNext = &properties;
        vkGetPhysicalDeviceProperties2(choice.physical, &extended);
        scratchAlignment = properties.minAccelerationStructureScratchOffsetAlignment;
    }
}

void Renderer::createCommands() {
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = choice.family;
    check(vkCreateCommandPool(device, &pool, nullptr, &commandPool), "Create command pool");
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = commandPool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(device, &allocation, &command), "Allocate command buffer");
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    check(vkCreateFence(device, &fence, nullptr, &submitted), "Create submission fence");
    if (surface) {
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(vkCreateSemaphore(device, &semaphore, nullptr, &imageAvailable),
              "Create image acquisition semaphore");
    }
}

uint32_t Renderer::memoryType(uint32_t allowed, VkMemoryPropertyFlags required) const {
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((allowed & (1u << i)) &&
            (memoryProperties.memoryTypes[i].propertyFlags & required) == required) {
            return i;
        }
    }
    throw std::runtime_error("Vulkan device has no compatible memory type");
}

void Renderer::createBuffer(Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage,
                            VkMemoryPropertyFlags properties) {
    buffer.device = device;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device, &info, nullptr, &buffer.handle), "Create buffer");

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer.handle, &requirements);
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) allocation.pNext = &flags;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits, properties);
    check(vkAllocateMemory(device, &allocation, nullptr, &buffer.memory), "Allocate buffer memory");
    check(vkBindBufferMemory(device, buffer.handle, buffer.memory, 0), "Bind buffer memory");
}

void Renderer::upload(const Buffer& buffer, const void* data, VkDeviceSize size) {
    void* mapping = nullptr;
    check(vkMapMemory(device, buffer.memory, 0, size, 0, &mapping), "Map scene memory");
    std::memcpy(mapping, data, static_cast<size_t>(size));
    vkUnmapMemory(device, buffer.memory);
}

VkDeviceAddress Renderer::addressOf(VkBuffer buffer) const {
    VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = buffer;
    return vkGetBufferDeviceAddress(device, &info);
}

void Renderer::createScene() {
    const auto vertices = cornellBox();
    vertexCount = static_cast<uint32_t>(vertices.size());
    const auto size = vertexCount * sizeof(Vertex);
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (choice.ray) {
        usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    }
    createBuffer(scene, size, usage, hostMemory);
    upload(scene, vertices.data(), size);
    if (choice.ray) {
        buildSceneAccelerationStructures();
    }
}

void Renderer::buildSceneAccelerationStructures() {
    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    auto& triangles = geometry.geometry.triangles;
    triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.vertexData.deviceAddress = addressOf(scene.handle);
    triangles.vertexStride = sizeof(Vertex);
    triangles.maxVertex = vertexCount - 1;
    triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
    buildAccelerationStructure(bottomLevel, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                               geometry, vertexCount / 3);

    VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    addressInfo.accelerationStructure = bottomLevel.handle;
    VkAccelerationStructureInstanceKHR instanceData{};
    instanceData.transform.matrix[0][0] = 1;
    instanceData.transform.matrix[1][1] = 1;
    instanceData.transform.matrix[2][2] = 1;
    instanceData.mask = 0xff;
    instanceData.accelerationStructureReference = getASAddress(device, &addressInfo);
    Buffer instances;
    createBuffer(instances, sizeof(instanceData),
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, hostMemory);
    upload(instances, &instanceData, sizeof(instanceData));

    geometry = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geometry.geometry.instances.data.deviceAddress = addressOf(instances.handle);
    buildAccelerationStructure(topLevel, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, geometry, 1);
}

void Renderer::buildAccelerationStructure(AccelerationStructure& target,
                                          VkAccelerationStructureTypeKHR type,
                                          const VkAccelerationStructureGeometryKHR& geometry,
                                          uint32_t primitiveCount) {
    VkAccelerationStructureBuildGeometryInfoKHR build{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type = type;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.geometryCount = 1;
    build.pGeometries = &geometry;

    VkAccelerationStructureBuildSizesInfoKHR sizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    getASBuildSizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build,
                    &primitiveCount, &sizes);
    createBuffer(target.storage, sizes.accelerationStructureSize,
                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkAccelerationStructureCreateInfoKHR create{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    create.buffer = target.storage.handle;
    create.size = sizes.accelerationStructureSize;
    create.type = type;
    check(createAS(device, &create, nullptr, &target.handle), "Create acceleration structure");

    Buffer scratch;
    createBuffer(scratch, sizes.buildScratchSize + scratchAlignment - 1,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    const VkDeviceAddress address = addressOf(scratch.handle);
    build.scratchData.deviceAddress = (address + scratchAlignment - 1) &
                                      ~(VkDeviceAddress(scratchAlignment) - 1);
    build.dstAccelerationStructure = target.handle;
    const VkAccelerationStructureBuildRangeInfoKHR range{primitiveCount, 0, 0, 0};
    const auto* rangePointer = &range;
    beginCommands();
    cmdBuildAS(command, 1, &build, &rangePointer);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &barrier,
                         0, nullptr, 0, nullptr);
    submitCommands();
    waitCommands();
}

void Renderer::createDescriptors() {
    if (choice.mesh || choice.ray) {
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[0].descriptorCount = 1;
        if (choice.mesh) bindings[0].stageFlags |= VK_SHADER_STAGE_MESH_BIT_EXT;
        if (choice.ray) bindings[0].stageFlags |= VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setInfo.bindingCount = choice.ray ? 2 : 1;
        setInfo.pBindings = bindings.data();
        check(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayout),
              "Create scene descriptor layout");

        const std::array<VkDescriptorPoolSize, 2> sizes{{
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
            {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
        }};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 1;
        pool.poolSizeCount = setInfo.bindingCount;
        pool.pPoolSizes = sizes.data();
        check(vkCreateDescriptorPool(device, &pool, nullptr, &descriptorPool),
              "Create scene descriptor pool");

        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = descriptorPool;
        allocation.descriptorSetCount = 1;
        allocation.pSetLayouts = &setLayout;
        check(vkAllocateDescriptorSets(device, &allocation, &descriptorSet),
              "Allocate scene descriptor set");

        const VkDescriptorBufferInfo buffer{scene.handle, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSetAccelerationStructureKHR asWrite{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        asWrite.accelerationStructureCount = 1;
        asWrite.pAccelerationStructures = &topLevel.handle;

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptorSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo = &buffer;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].pNext = &asWrite;
        writes[1].dstSet = descriptorSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        vkUpdateDescriptorSets(device, setInfo.bindingCount, writes.data(), 0, nullptr);
    }

    const VkPushConstantRange push{pushStages, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = setLayout ? 1 : 0;
    layout.pSetLayouts = &setLayout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(device, &layout, nullptr, &pipelineLayout), "Create pipeline layout");
}

constexpr std::array colorFormats{VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB,
                                 VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};

bool isSrgb(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB;
}

void Renderer::initializeTargets(uint32_t width, uint32_t height) {
    check(vkDeviceWaitIdle(device), "Wait before rebuilding render targets");
    destroyTargets();
    if (surface) {
        createSwapchain(width, height);
    } else {
        extent = {width, height};
        colorFormat = VK_FORMAT_R8G8B8A8_SRGB;
    }

    depthFormat = VK_FORMAT_UNDEFINED;
    for (const auto format : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT,
                              VK_FORMAT_D16_UNORM}) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(choice.physical, format, &properties);
        if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            depthFormat = format;
            break;
        }
    }
    if (depthFormat == VK_FORMAT_UNDEFINED) {
        throw std::runtime_error("Vulkan device has no supported depth attachment format");
    }
    if (extent.width > choice.properties.limits.maxImageDimension2D ||
        extent.height > choice.properties.limits.maxImageDimension2D ||
        extent.width > choice.properties.limits.maxFramebufferWidth ||
        extent.height > choice.properties.limits.maxFramebufferHeight) {
        throw std::runtime_error("Requested image exceeds the device's framebuffer limits");
    }
    createImage(color, colorFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    createImage(depth, depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                VK_IMAGE_ASPECT_DEPTH_BIT);
    createRenderPass();

    const std::array attachments{color.view, depth.view};
    VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    info.renderPass = renderPass;
    info.attachmentCount = static_cast<uint32_t>(attachments.size());
    info.pAttachments = attachments.data();
    info.width = extent.width;
    info.height = extent.height;
    info.layers = 1;
    check(vkCreateFramebuffer(device, &info, nullptr, &framebuffer), "Create framebuffer");
}

void Renderer::createSwapchain(uint32_t width, uint32_t height) {
    VkSurfaceCapabilitiesKHR capabilities{};
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(choice.physical, surface, &capabilities),
          "Query Vulkan surface capabilities");
    if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        throw std::runtime_error("Vulkan surface does not support image transfers for presentation");
    }
    const auto formats = enumerate<VkSurfaceFormatKHR>([&](uint32_t* count, auto* values) {
        return vkGetPhysicalDeviceSurfaceFormatsKHR(choice.physical, surface, count, values);
    });
    std::optional<VkSurfaceFormatKHR> selected;
    for (const auto format : colorFormats) {
        const auto it = std::find_if(formats.begin(), formats.end(), [&](const auto& candidate) {
            return (candidate.format == format || candidate.format == VK_FORMAT_UNDEFINED) &&
                   candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
        if (it != formats.end()) {
            selected = {format, it->colorSpace};
            break;
        }
    }
    if (!selected) {
        throw std::runtime_error("Vulkan surface has no supported sRGB color format");
    }
    colorFormat = selected->format;

    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        extent = capabilities.currentExtent;
    } else {
        extent = {std::clamp(width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
                  std::clamp(height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height)};
    }
    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount) imageCount = std::min(imageCount, capabilities.maxImageCount);

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface;
    info.minImageCount = imageCount;
    info.imageFormat = colorFormat;
    info.imageColorSpace = selected->colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
    for (const auto alpha : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                             VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                             VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                             VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
        if (capabilities.supportedCompositeAlpha & alpha) {
            info.compositeAlpha = alpha;
            break;
        }
    }
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    check(vkCreateSwapchainKHR(device, &info, nullptr, &swapchain), "Create Vulkan swapchain");
    swapImages = enumerate<VkImage>([&](uint32_t* count, auto* values) {
        return vkGetSwapchainImagesKHR(device, swapchain, count, values);
    });

    // A present semaphore belongs to its swapchain image and is reused only after reacquiring it.
    presentReady.resize(swapImages.size());
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& ready : presentReady) {
        check(vkCreateSemaphore(device, &semaphore, nullptr, &ready), "Create present semaphore");
    }
}

void Renderer::createImage(Image& image, VkFormat format, VkImageUsageFlags usage,
                           VkImageAspectFlags aspect) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(device, &info, nullptr, &image.handle), "Create render image");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, image.handle, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex =
        memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device, &allocation, nullptr, &image.memory), "Allocate render image");
    check(vkBindImageMemory(device, image.handle, image.memory, 0), "Bind render image memory");

    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image.handle;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {aspect, 0, 1, 0, 1};
    check(vkCreateImageView(device, &view, nullptr, &image.view), "Create render image view");
}

void Renderer::createRenderPass() {
    std::array<VkAttachmentDescription, 2> attachments{};
    attachments[0].format = colorFormat;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    attachments[1].format = depthFormat;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    const VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = static_cast<uint32_t>(attachments.size());
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = static_cast<uint32_t>(dependencies.size());
    info.pDependencies = dependencies.data();
    check(vkCreateRenderPass(device, &info, nullptr, &renderPass), "Create render pass");
}

struct Shader {
    VkDevice device;
    VkShaderModule module = VK_NULL_HANDLE;

    template<size_t words>
    Shader(VkDevice device, const uint32_t (&spirv)[words]) : device(device) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = sizeof(spirv);
        info.pCode = spirv;
        check(vkCreateShaderModule(device, &info, nullptr, &module), "Create shader module");
    }

    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    ~Shader() { vkDestroyShaderModule(device, module, nullptr); }
};

void Renderer::createPipeline(Mode mode) {
    const bool ray = mode == Mode::ray;
    const bool mesh = mode == Mode::mesh;
    std::optional<Shader> first;
    std::optional<Shader> fragment;
    if (ray) {
        first.emplace(device, fullscreen_vert);
        fragment.emplace(device, ray_frag);
    } else {
        if (mesh) {
            first.emplace(device, raster_mesh);
        } else {
            first.emplace(device, raster_vert);
        }
        fragment.emplace(device, raster_frag);
    }

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = mesh ? VK_SHADER_STAGE_MESH_BIT_EXT : VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = first->module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment->module;
    stages[1].pName = "main";

    const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array<VkVertexInputAttributeDescription, 3> attributes{{
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, material)},
    }};
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (mode == Mode::raster) {
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();
    }
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthState{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthState.depthTestEnable = !ray;
    depthState.depthWriteEnable = !ray;
    depthState.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorState{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorState.attachmentCount = 1;
    colorState.pAttachments = &blend;
    const std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamic.pDynamicStates = dynamicStates.data();

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = static_cast<uint32_t>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = mesh ? nullptr : &vertexInput;
    info.pInputAssemblyState = mesh ? nullptr : &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depthState;
    info.pColorBlendState = &colorState;
    info.pDynamicState = &dynamic;
    info.layout = pipelineLayout;
    info.renderPass = renderPass;
    auto& pipeline = pipelines[static_cast<size_t>(mode)];
    check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
          modeTitle(mode));
}

void Renderer::destroyTargets() {
    for (auto& pipeline : pipelines) {
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
    }
    if (framebuffer) vkDestroyFramebuffer(device, framebuffer, nullptr);
    if (renderPass) vkDestroyRenderPass(device, renderPass, nullptr);
    framebuffer = VK_NULL_HANDLE;
    renderPass = VK_NULL_HANDLE;
    color.reset(device);
    depth.reset(device);
    for (const auto ready : presentReady) {
        if (ready) vkDestroySemaphore(device, ready, nullptr);
    }
    presentReady.clear();
    swapImages.clear();
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
}

void Renderer::beginCommands() {
    waitCommands();
    check(vkResetCommandBuffer(command, 0), "Reset command buffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(command, &begin), "Begin command buffer");
}

void Renderer::submitCommands(VkSemaphore acquired, VkSemaphore ready) {
    check(vkEndCommandBuffer(command), "End command buffer");
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = acquired ? 1 : 0;
    submit.pWaitSemaphores = &acquired;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    submit.signalSemaphoreCount = ready ? 1 : 0;
    submit.pSignalSemaphores = &ready;
    check(vkResetFences(device, 1, &submitted), "Reset submission fence");
    check(vkQueueSubmit(queue, 1, &submit, submitted), "Submit command buffer");
}

void Renderer::waitCommands() {
    check(vkWaitForFences(device, 1, &submitted, VK_TRUE, UINT64_MAX),
          "Wait for Vulkan commands");
}

void Renderer::recordReadback(const Buffer& readback) {
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(command, color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          readback.handle, 1, &region);

    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = readback.handle;
    barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
}

void Renderer::recordPresent(VkImage destination) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = destination;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = {extent.width, extent.height, 1};
    vkCmdCopyImage(command, color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void Renderer::savePng(const Buffer& readback, const std::string& path) {
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    png.width = extent.width;
    png.height = extent.height;
    const bool bgra = colorFormat == VK_FORMAT_B8G8R8A8_SRGB ||
                      colorFormat == VK_FORMAT_B8G8R8A8_UNORM;
    png.format = bgra ? PNG_FORMAT_BGRA : PNG_FORMAT_RGBA;
    void* mapping = nullptr;
    check(vkMapMemory(device, readback.memory, 0, VK_WHOLE_SIZE, 0, &mapping),
          "Map captured image");
    const int saved = png_image_write_to_file(&png, path.c_str(), 0, mapping, 0, nullptr);
    vkUnmapMemory(device, readback.memory);
    const std::string error = png.message;
    png_image_free(&png);
    if (!saved) {
        throw std::runtime_error("Write " + path + ": " + error);
    }
    std::cout << "Saved " << path << '\n';
}

bool Renderer::draw(Mode mode, uint32_t samples, const std::string& capture) {
    if (!supports(mode)) {
        throw std::logic_error("Attempt to render an unsupported Vulkan mode");
    }
    const auto modeIndex = static_cast<size_t>(mode);
    if (!pipelines[modeIndex]) createPipeline(mode);
    waitCommands();

    Buffer readback;
    if (!capture.empty()) {
        const VkDeviceSize size = VkDeviceSize(extent.width) * extent.height * 4;
        createBuffer(readback, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, hostMemory);
    }

    uint32_t imageIndex = 0;
    if (swapchain) {
        const auto acquired = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, imageAvailable,
                                                    VK_NULL_HANDLE, &imageIndex);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) return false;
        if (acquired != VK_SUBOPTIMAL_KHR) check(acquired, "Acquire swapchain image");
    }

    beginCommands();
    std::array<VkClearValue, 2> clear{};
    const bool encodeSrgb = !isSrgb(colorFormat);
    clear[0].color = encodeSrgb ? VkClearColorValue{{.026f, .039f, .051f, 1}} :
                                  VkClearColorValue{{.002f, .003f, .004f, 1}};
    clear[1].depthStencil = {1, 0};
    VkRenderPassBeginInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    render.renderPass = renderPass;
    render.framebuffer = framebuffer;
    render.renderArea.extent = extent;
    render.clearValueCount = static_cast<uint32_t>(clear.size());
    render.pClearValues = clear.data();
    vkCmdBeginRenderPass(command, &render, VK_SUBPASS_CONTENTS_INLINE);
    const auto width = static_cast<float>(extent.width);
    const auto height = static_cast<float>(extent.height);
    const VkViewport viewport{0, 0, width, height, 0, 1};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &render.renderArea);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[modeIndex]);
    const PushConstants push{width, height, samples, encodeSrgb};
    vkCmdPushConstants(command, pipelineLayout, pushStages, 0, sizeof(push), &push);

    if (mode == Mode::raster) {
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(command, 0, 1, &scene.handle, &offset);
        vkCmdDraw(command, vertexCount, 1, 0, 0);
    } else {
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);
        if (mode == Mode::mesh) {
            // Each workgroup emits the six vertices of one scene quad.
            cmdDrawMeshTasks(command, vertexCount / 6, 1, 1);
        } else {
            vkCmdDraw(command, 3, 1, 0, 0);
        }
    }
    vkCmdEndRenderPass(command);
    if (readback.handle) recordReadback(readback);
    if (swapchain) recordPresent(swapImages[imageIndex]);
    const VkSemaphore ready = swapchain ? presentReady[imageIndex] : VK_NULL_HANDLE;
    submitCommands(swapchain ? imageAvailable : VK_NULL_HANDLE, ready);

    VkResult presented = VK_SUCCESS;
    if (swapchain) {
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &ready;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &imageIndex;
        presented = vkQueuePresentKHR(queue, &present);
    }
    if (readback.handle) {
        waitCommands();
        if (presented != VK_ERROR_OUT_OF_DATE_KHR) savePng(readback, capture);
    }
    if (presented == VK_ERROR_OUT_OF_DATE_KHR) return false;
    if (presented != VK_SUBOPTIMAL_KHR) check(presented, "Present swapchain image");
    return true;
}

struct Window {
    SDL_Window* handle = nullptr;
    ~Window() {
        if (handle) SDL_DestroyWindow(handle);
        SDL_Quit();
    }
};

void setTitle(SDL_Window* window, Mode mode, const std::string& status = {}) {
    std::string title = "Venus Cornell Box | ";
    title += modeTitle(mode);
    title += " | 1 Raster  2 Mesh  3 Ray  S Save";
    if (!status.empty()) title += " | " + status;
    sdlCheck(SDL_SetWindowTitle(window, title.c_str()), "Set window title");
}

void runWindow(Renderer& renderer, SDL_Window* window, const Options& options) {
    Mode mode = options.mode;
    setTitle(window, mode);
    bool running = true;
    bool rebuild = true;
    bool redraw = true;
    std::string capture = options.capture;
    const auto processEvent = [&](const SDL_Event& event) {
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            running = false;
        } else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                   event.type == SDL_EVENT_WINDOW_RESTORED) {
            rebuild = true;
            redraw = true;
        } else if (event.type == SDL_EVENT_WINDOW_EXPOSED) {
            redraw = true;
        } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
            const auto key = event.key.key;
            if (key == SDLK_ESCAPE) {
                running = false;
            } else if (key == SDLK_S) {
                capture = std::string("cornell-") + modeName(mode) + ".png";
                redraw = true;
            } else if (key == SDLK_1 || key == SDLK_2 || key == SDLK_3) {
                const auto selected = modes[static_cast<size_t>(key - SDLK_1)];
                if (renderer.supports(selected)) {
                    mode = selected;
                    setTitle(window, mode);
                    redraw = true;
                } else {
                    const std::string status = std::string(modeTitle(selected)) + " unavailable";
                    setTitle(window, mode, status);
                    std::cerr << status << " on this Vulkan device\n";
                }
            }
        }
    };

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) processEvent(event);
        if (!running) break;
        int width = 0, height = 0;
        sdlCheck(SDL_GetWindowSizeInPixels(window, &width, &height), "Get window size");
        if (redraw && width > 0 && height > 0 &&
            !(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) {
            if (rebuild) {
                renderer.initializeTargets(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
                rebuild = false;
            }
            if (renderer.draw(mode, options.samples, capture)) {
                capture.clear();
                redraw = false;
                if (options.once) return;
            } else {
                rebuild = true;
                SDL_Delay(16);
            }
        } else if (SDL_WaitEvent(&event)) {
            processEvent(event);
        } else {
            sdlCheck(false, "Wait for window events");
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parseOptions(argc, argv);
        if (options.help) {
            std::cout << "Usage: venus-cornell [--mode raster|mesh|ray] [--width N] [--height N]\n"
                         "                     [--samples N] [--capture IMAGE.png] [--headless|--once]\n"
                         "       venus-cornell --list-modes\n"
                         "Keys: 1 vertex raster, 2 mesh raster, 3 ray query, S save PNG, Esc quit.\n";
            return 0;
        }

        Window window;
        if (!options.headless && !options.list) {
            sdlCheck(SDL_Init(SDL_INIT_VIDEO), "Initialize SDL video");
            window.handle = SDL_CreateWindow("Venus Cornell Box", static_cast<int>(options.width),
                                             static_cast<int>(options.height),
                                             SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
            sdlCheck(window.handle != nullptr, "Create Cornell box window");
        }
        Renderer renderer;
        renderer.initialize(options, window.handle);
        if (options.list) return 0;
        if (window.handle) {
            runWindow(renderer, window.handle, options);
        } else {
            renderer.initializeTargets(options.width, options.height);
            renderer.draw(options.mode, options.samples, options.capture);
            std::cout << "Rendered " << modeTitle(options.mode) << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "venus-cornell: " << error.what() << '\n';
        return 1;
    }
}
