#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef ERROR

#include "DeviceContext.h"
#include "Swapchain.h"

// volk をインクルード.
// VOLK_IMPLEMENTATIONを定義して実装も含める.
//#define VOLK_IMPLEMENTATION
#define VK_USE_PLATFORM_WIN32_KHR
#include "Volk/volk.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include "GLFW/glfw3.h"
#include "GLFW/glfw3native.h"

#include <cstdint>
#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <array>
#include <format>
#include <cassert>

static DeviceContext* gDeviceContext;

static VkBool32 VKAPI_CALL VulkanDebugCallback(
  VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
  VkDebugUtilsMessageTypeFlagsEXT messageType,
  const VkDebugUtilsMessengerCallbackDataEXT* data,
  void* userData)
{
  if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
  {
    OutputDebugStringA(data->pMessage);
    OutputDebugStringA("\n");
  }
  return VK_FALSE;
}

VkInstance DeviceContext::GetVkInstance()
{
  return m_vkInstance;
}
VkDevice DeviceContext::GetVkDevice()
{
  return m_vkDevice;
}

bool DeviceContext::Initialize()
{
  gDeviceContext = new DeviceContext();
  gDeviceContext->InitializeVkInstance();
  gDeviceContext->EnumerateGPUs();

  return true;
}

void DeviceContext::Shutdown()
{
  if (!gDeviceContext)
  {
    return;
  }
  gDeviceContext->GetSwapchain()->Shutdown();
  delete gDeviceContext;
  gDeviceContext = nullptr;
}

DeviceContext* DeviceContext::GetContext()
{
  return gDeviceContext;
}

bool DeviceContext::InitializeDevice(int useGpuIndex)
{
  uint32_t familyPropsCount = 0;
  m_useGpuIndex = useGpuIndex;
  auto gpu = GetGPU();
  vkGetPhysicalDeviceQueueFamilyProperties2(gpu, &familyPropsCount, nullptr);

  std::vector<VkQueueFamilyVideoPropertiesKHR> familyPropsVideo(familyPropsCount);
  std::vector<VkQueueFamilyProperties2> familyProps(familyPropsCount);
  
  for (uint32_t i = 0; i < familyPropsCount; ++i)
  {
    auto& prop = familyProps[i];
    auto& videoProp = familyPropsVideo[i];
    prop.sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
    prop.pNext = &videoProp;
    videoProp.sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
  }
  vkGetPhysicalDeviceQueueFamilyProperties2(gpu, &familyPropsCount, familyProps.data());
  m_queueFamilies.resize(familyPropsCount);
  
  for (uint32_t i = 0; i < familyPropsCount; ++i)
  {
    m_queueFamilies[i].properties = familyProps[i];
    m_queueFamilies[i].propertiesVideo = familyPropsVideo[i];
    m_queueFamilies[i].properties.pNext = nullptr;

    auto& queueFamily = m_queueFamilies[i].properties.queueFamilyProperties;
    if (queueFamily.queueCount > 0 && queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT)
    {
      if (m_graphicsFamily == VK_QUEUE_FAMILY_IGNORED)
      {
        m_graphicsFamily = i;
        m_queueFamilyIndices.push_back(m_graphicsFamily);
      }
    }
    if (queueFamily.queueCount > 0 && queueFamily.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR)
    {
      if (m_videoDecodeFamily == VK_QUEUE_FAMILY_IGNORED)
      {
        // H264 サポートしてる？
        if (m_queueFamilies[i].propertiesVideo.videoCodecOperations & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR)
        {
          m_videoDecodeFamily = i;
          m_queueFamilyIndices.push_back(m_videoDecodeFamily);
        }
      }
    }
  }
  
  m_features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  m_features2.pNext = &m_vulkan12Features;
  m_vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  m_vulkan12Features.pNext = &m_vulkan13Features;
  m_vulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  vkGetPhysicalDeviceFeatures2(gpu, &m_features2);

  float defaultPrior = 1.0f;
  std::vector<VkDeviceQueueCreateInfo> deviceQueueCI = {
    {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = m_graphicsFamily,
      .queueCount = 1,
      .pQueuePriorities = &defaultPrior,
    },
    {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = m_videoDecodeFamily,
      .queueCount = 1,
      .pQueuePriorities = &defaultPrior,
    }
  };

  std::vector<const char*> activeDeviceExtensions = {
      VK_KHR_SWAPCHAIN_EXTENSION_NAME,

      VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
      VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
      VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
      VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
  };

  VkPhysicalDeviceSamplerYcbcrConversionFeatures samplerYcbcrConversionFeatures{
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
    .pNext = nullptr,
    .samplerYcbcrConversion = VK_TRUE,
  };
  m_vulkan13Features.pNext = &samplerYcbcrConversionFeatures;

  VkDeviceCreateInfo deviceCreateInfo{
    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    .pNext = &m_features2,
    .queueCreateInfoCount = uint32_t(deviceQueueCI.size()),
    .pQueueCreateInfos = deviceQueueCI.data(),
    .enabledExtensionCount = uint32_t(activeDeviceExtensions.size()),
    .ppEnabledExtensionNames = activeDeviceExtensions.data(),
  };

  auto res = vkCreateDevice(gpu, &deviceCreateInfo, nullptr, &m_vkDevice);
  if (res != VK_SUCCESS)
  {
    DebugBreak();
  }
  volkLoadDevice(m_vkDevice);

  // デバイスキューを取得.
  vkGetDeviceQueue(m_vkDevice, m_graphicsFamily, 0, &m_graphicsQueue);
  vkGetDeviceQueue(m_vkDevice, m_videoDecodeFamily, 0, &m_videoDecodeQueue);

  m_videoProfileInfo.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
  m_videoProfileInfo.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
  m_videoProfileInfo.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
  m_videoProfileInfo.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
  m_videoProfileInfo.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
  m_videoProfileInfo.pNext = &m_videoDecodeH264.profile;

  m_videoDecodeH264.profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
  m_videoDecodeH264.profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
  m_videoDecodeH264.profile.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_INTERLACED_INTERLEAVED_LINES_BIT_KHR;

  m_videoCapabilities.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
  m_videoCapabilities.pNext = &m_videoDecodeCapabilities;

  m_videoDecodeCapabilities.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR;
  m_videoDecodeCapabilities.pNext = &m_videoDecodeH264.capabilities;

  m_videoDecodeH264.capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR;
  vkGetPhysicalDeviceVideoCapabilitiesKHR(gpu, &m_videoProfileInfo, &m_videoCapabilities);

  VIDEO_DECODE_BITSTREAM_ALIGNMENT = std::max(VIDEO_DECODE_BITSTREAM_ALIGNMENT, m_videoCapabilities.minBitstreamBufferOffsetAlignment);
  VIDEO_DECODE_BITSTREAM_ALIGNMENT = std::max(VIDEO_DECODE_BITSTREAM_ALIGNMENT, m_videoCapabilities.minBitstreamBufferSizeAlignment);


  VmaVulkanFunctions vulkanFunctions = {
    .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
    .vkGetDeviceProcAddr = vkGetDeviceProcAddr,
    .vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties,
    .vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties,
    .vkAllocateMemory = vkAllocateMemory,
    .vkFreeMemory = vkFreeMemory,
    .vkMapMemory = vkMapMemory,
    .vkUnmapMemory = vkUnmapMemory,
    .vkFlushMappedMemoryRanges = vkFlushMappedMemoryRanges,
    .vkInvalidateMappedMemoryRanges = vkInvalidateMappedMemoryRanges,
    .vkBindBufferMemory = vkBindBufferMemory,
    .vkBindImageMemory = vkBindImageMemory,
    .vkGetBufferMemoryRequirements = vkGetBufferMemoryRequirements,
    .vkGetImageMemoryRequirements = vkGetImageMemoryRequirements,
    .vkCreateBuffer = vkCreateBuffer,
    .vkDestroyBuffer = vkDestroyBuffer,
    .vkCreateImage = vkCreateImage,
    .vkDestroyImage = vkDestroyImage,
    .vkCmdCopyBuffer = vkCmdCopyBuffer,
  };

  VmaAllocatorCreateInfo allocatorCI{
    .flags = 0, //VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT,
    .physicalDevice = GetGPU(),
    .device = m_vkDevice,
    //.pAllocationCallbacks = allocationCallbacks,
    .pVulkanFunctions = &vulkanFunctions,
    .instance = m_vkInstance,
    .vulkanApiVersion = VK_API_VERSION_1_3,
  };
  vmaCreateAllocator(&allocatorCI, &m_vmaAllocator);

  VkSamplerYcbcrConversionCreateInfo samplerYcbcrConversionCI = {
    .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO, // VkStructureType
    .pNext = nullptr,                                                // void *, optional
    .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,                     // VkFormat
    .ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,            // VkSamplerYcbcrModelConversion
    .ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW,                      // VkSamplerYcbcrRange
    .components = {},                                                     // VkComponentMapping
    .xChromaOffset = VK_CHROMA_LOCATION_MIDPOINT,                            // VkChromaLocation
    .yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT,                            // VkChromaLocation
    .chromaFilter = VK_FILTER_NEAREST,                                      // VkFilter
    .forceExplicitReconstruction = 0,                                                      // VkBool32
  };

  vkCreateSamplerYcbcrConversion(m_vkDevice, &samplerYcbcrConversionCI, nullptr, &m_samplerYcbcrConversion);

  return true;
}

bool DeviceContext::InitializeSwapchain(GLFWwindow* window)
{
  m_swapchain = std::make_shared<Swapchain>();
  return m_swapchain->Initialize(window);
}


void DeviceContext::CreateBuffer(const GPUBufferDesc* desc, GPUBuffer* buffer)
{
  VkBufferCreateInfo bufferCI{
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
  };

}

void DeviceContext::CreateImage(const GPUImageDesc& desc, GPUImage* image)
{
  VkImageCreateInfo imageCI{
    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
    .flags = 0,
    .imageType = desc.type,
    .format = desc.format,
    .extent = desc.extent,
    .mipLevels = desc.mipLevels,
    .arrayLayers = desc.arraySize,
    .samples = VK_SAMPLE_COUNT_1_BIT,
    .tiling = VK_IMAGE_TILING_OPTIMAL,
    .usage = desc.usage,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    .queueFamilyIndexCount = 0,
    .pQueueFamilyIndices = nullptr,
    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  imageCI.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
 
  VkVideoProfileListInfoKHR profileListInfo{
    .sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR,
    .profileCount = 1,
    .pProfiles = &m_videoProfileInfo,
  };
  if (desc.usage & (VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_SRC_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR))
  {
    imageCI.pNext = &profileListInfo;

    VkPhysicalDeviceVideoFormatInfoKHR videoFormatInfo{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR,
      .pNext = &profileListInfo,
      .imageUsage = imageCI.usage,
    };
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceVideoFormatPropertiesKHR(GetGPU(), &videoFormatInfo, &formatCount, nullptr);
    std::vector<VkVideoFormatPropertiesKHR> videoFormats(formatCount);
    for (auto& v : videoFormats)
    {
      v.sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
    }
    auto res = vkGetPhysicalDeviceVideoFormatPropertiesKHR(GetGPU(), &videoFormatInfo, &formatCount, videoFormats.data());
    assert(res == VK_SUCCESS);
  }

  if (m_queueFamilyIndices.size() > 1)
  {
    imageCI.sharingMode = VK_SHARING_MODE_CONCURRENT;
    imageCI.queueFamilyIndexCount = (uint32_t)m_queueFamilyIndices.size();
    imageCI.pQueueFamilyIndices = m_queueFamilyIndices.data();
  }

  vkCreateImage(m_vkDevice, &imageCI, nullptr, &image->image);

  VkMemoryRequirements2 reqs{
    .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
  };
  VkImageMemoryRequirementsInfo2 info{
    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
    .image = image->image,
  };
  vkGetImageMemoryRequirements2(m_vkDevice, &info, &reqs);
  
  VkMemoryAllocateInfo allocInfo{
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .allocationSize = reqs.memoryRequirements.size,
    .memoryTypeIndex = GetMemoryTypeIndex(reqs, desc.memoryProperty),
  };
  vkAllocateMemory(m_vkDevice, &allocInfo, nullptr, &image->memory);
  vkBindImageMemory(m_vkDevice, image->image, image->memory, 0);

  VkImageViewCreateInfo viewCI{
    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
    .flags = 0,
    .image = image->image,
    .viewType = VK_IMAGE_VIEW_TYPE_2D,
    .format = imageCI.format,
    .components = {
      VK_COMPONENT_SWIZZLE_IDENTITY,
      VK_COMPONENT_SWIZZLE_IDENTITY,
      VK_COMPONENT_SWIZZLE_IDENTITY,
      VK_COMPONENT_SWIZZLE_IDENTITY,
    },
    .subresourceRange = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .baseMipLevel = 0,
      .levelCount = imageCI.mipLevels,
      .baseArrayLayer = 0,
      .layerCount = imageCI.arrayLayers,
    }
  };
  switch (desc.type)
  {
    case VK_IMAGE_TYPE_1D: viewCI.viewType = VK_IMAGE_VIEW_TYPE_1D; break;
    case VK_IMAGE_TYPE_2D: viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D; break;
    case VK_IMAGE_TYPE_3D: viewCI.viewType = VK_IMAGE_VIEW_TYPE_3D; break;
  }
  vkCreateImageView(m_vkDevice, &viewCI, nullptr, &image->imageView);

}

void DeviceContext::Submit(QueueType type, const VkSubmitInfo* pSubmitInfo, VkFence waitFence)
{
  VkQueue queue = m_graphicsQueue;
  if (type == Graphics) { queue = m_graphicsQueue; }
  if (type == VideoDecode) { queue = m_videoDecodeQueue; }
  vkQueueSubmit(queue, 1, pSubmitInfo, waitFence);
}


void DeviceContext::Present(std::vector<VkSemaphore> waitSemaphores)
{
  auto handle = m_swapchain->GetHandle();
  auto index = m_swapchain->GetCurrentIndex();
  VkPresentInfoKHR present{
    .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
    .waitSemaphoreCount = uint32_t(waitSemaphores.size()),
    .pWaitSemaphores = waitSemaphores.data(),
    .swapchainCount = 1,
    .pSwapchains = &handle,
    .pImageIndices = &index,
  };

  vkQueuePresentKHR(m_graphicsQueue, &present);
  vkQueueWaitIdle(m_graphicsQueue);
}

void DeviceContext::WaitForIdle()
{
  vkQueueWaitIdle(m_graphicsQueue);
  vkQueueWaitIdle(m_videoDecodeQueue);
}

VkQueue DeviceContext::GetQueue(QueueType type)
{
  if (type == Graphics) { return m_graphicsQueue; }
  if (type == VideoDecode) { return m_videoDecodeQueue; }
  return VK_NULL_HANDLE;
}

bool DeviceContext::InitializeVkInstance()
{
  if (volkInitialize() != VK_SUCCESS)
  {
    OutputDebugStringA("volkInitialize failed.\n");
    return false;
  }
  uint32_t instanceExtensionCount;
  vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, nullptr);
  std::vector<VkExtensionProperties> instanceExtensions(instanceExtensionCount);
  vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, instanceExtensions.data());

  std::vector<const char*> activeInstanceExtensions;

  uint32_t glfwRequiredCount;
  auto glfwRequiredExtensionNames = glfwGetRequiredInstanceExtensions(&glfwRequiredCount);
  std::for_each_n(glfwRequiredExtensionNames, glfwRequiredCount, [&](auto v) { activeInstanceExtensions.push_back(v); });

  // VK_EXT_full_screen_exclusiveのために依存する拡張機能を有効化する.
  auto instanceExtensionRequired = {
    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
    // ----
#ifdef _DEBUG
      VK_EXT_DEBUG_UTILS_EXTENSION_NAME
#endif
  };
  for (auto v : instanceExtensionRequired)
  {
    activeInstanceExtensions.push_back(v);
  }

  std::vector<const char*> activeInstanceLayers = {
#ifdef _DEBUG
      "VK_LAYER_KHRONOS_validation"
#endif
  };

  VkApplicationInfo appInfo{
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName = "Sample",
    .pEngineName = "Sample",
    .engineVersion = VK_API_VERSION_1_3,
    .apiVersion = VK_API_VERSION_1_3,
  };

  VkInstanceCreateInfo instanceCreateInfo = {
    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    .pApplicationInfo = &appInfo,
    .enabledLayerCount = uint32_t(activeInstanceLayers.size()),
    .ppEnabledLayerNames = (activeInstanceLayers.size() > 0) ? activeInstanceLayers.data() : nullptr,
    .enabledExtensionCount = uint32_t(activeInstanceExtensions.size()),
    .ppEnabledExtensionNames = (activeInstanceExtensions.size() > 0) ? activeInstanceExtensions.data() : nullptr,
  };

#ifdef _DEBUG
  VkDebugUtilsMessengerCreateInfoEXT debugUtilsCreateInfo{
    .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
    .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
    .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
    .pfnUserCallback = VulkanDebugCallback,
  };
  instanceCreateInfo.pNext = &debugUtilsCreateInfo;
#endif
  auto res = vkCreateInstance(&instanceCreateInfo, nullptr, &m_vkInstance);
  if (res != VK_SUCCESS)
  {
    OutputDebugStringA("Failed vkCreateInstance().\n");
    return false;
  }
  volkLoadInstance(m_vkInstance);
#ifdef _DEBUG
  res = vkCreateDebugUtilsMessengerEXT(m_vkInstance, &debugUtilsCreateInfo, nullptr, &m_debugUtils);
  if (res != VK_SUCCESS)
  {
    OutputDebugStringA("Failed vkCreateDebugUtilsMessengerEXT().\n");
    return false;
  }
#endif
  return true;
}

void DeviceContext::EnumerateGPUs()
{
  uint32_t gpuCount = 0;
  vkEnumeratePhysicalDevices(m_vkInstance, &gpuCount, nullptr);
  m_gpus.resize(gpuCount);
  vkEnumeratePhysicalDevices(m_vkInstance, &gpuCount, m_gpus.data());

  m_physicalDeviceMemoryProps.resize(gpuCount);
  for (uint32_t i = 0; i < gpuCount; ++i)
  {
    auto& memProps = m_physicalDeviceMemoryProps[i];
    memProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    memProps.pNext = nullptr;

    vkGetPhysicalDeviceMemoryProperties2(m_gpus[i], &memProps);
  }
}

uint32_t DeviceContext::GetMemoryTypeIndex(VkMemoryRequirements2 reqs, VkMemoryPropertyFlags flags)
{
  auto requestBits = reqs.memoryRequirements.memoryTypeBits;
  const auto& memoryProps = m_physicalDeviceMemoryProps[m_useGpuIndex].memoryProperties;
  for (uint32_t i = 0; i < memoryProps.memoryTypeCount; ++i)
  {
    if (requestBits & 1)
    {
      // 要求されたメモリプロパティと一致するものを見つける.
      const auto types = memoryProps.memoryTypes[i];
      if ((types.propertyFlags & flags) == flags)
      {
        return i;
      }
    }
    requestBits >>= 1;
  }
  return UINT32_MAX;
}
