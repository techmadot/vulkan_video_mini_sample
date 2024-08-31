#include "Swapchain.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include "Volk/volk.h"

#include <format>

bool Swapchain::Initialize(GLFWwindow* window, VkFormat desiredFormat)
{
  auto devCtx = DeviceContext::GetContext();
  auto vkInstance = devCtx->GetVkInstance();
  auto result = glfwCreateWindowSurface(vkInstance, window, nullptr, &m_surface) != VK_SUCCESS;

  VkSurfaceCapabilitiesKHR surfaceCaps{};
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(devCtx->GetGPU(), m_surface, &surfaceCaps);
  
  VkExtent2D swapchainSize = { 1280, 720 };
  if (surfaceCaps.currentExtent.width != ~0u)
  {
    swapchainSize = surfaceCaps.currentExtent;
  }
  m_imageCount = (std::min)(surfaceCaps.minImageCount, 2u);

  uint32_t surfaceCount = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(devCtx->GetGPU(), m_surface, &surfaceCount, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(surfaceCount);
  vkGetPhysicalDeviceSurfaceFormatsKHR(devCtx->GetGPU(), m_surface, &surfaceCount, formats.data());

  // スワップチェインのフォーマットを決定.
  for (auto& format : formats)
  {
    if (format.format == desiredFormat)
    {
      m_surfaceFormat = format;
      break;
    }
  }
  if (m_surfaceFormat.format == VK_FORMAT_UNDEFINED)
  {
    for (auto& format : formats)
    {
      if (format.colorSpace == VK_COLORSPACE_SRGB_NONLINEAR_KHR)
      {
        m_surfaceFormat = format;
        break;
      }
    }
    m_surfaceFormat = formats[0];
  }

  return Resize(swapchainSize);
}

void Swapchain::Shutdown()
{
  auto devCtx = DeviceContext::GetContext();
  auto vkDevice = devCtx->GetVkDevice();
  vkDestroySwapchainKHR(vkDevice, m_swapchain, nullptr);

  m_images.clear();
  for (auto& view : m_imageViews)
  {
    vkDestroyImageView(vkDevice, view, nullptr);
  }
  m_imageViews.clear();
  m_swapchain = VK_NULL_HANDLE;
}

bool Swapchain::Resize(VkExtent2D extent)
{
  VkSwapchainKHR oldSwapchain = m_swapchain;
  VkSwapchainCreateInfoKHR swapchainCI{
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = m_surface,
      .minImageCount = m_imageCount,
      .imageFormat = m_surfaceFormat.format,
      .imageColorSpace = m_surfaceFormat.colorSpace,
      .imageExtent = extent,
      .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = m_presentMode,
      .clipped = VK_TRUE,
      .oldSwapchain = oldSwapchain,
  };
  auto devCtx = DeviceContext::GetContext();
  auto vkDevice = devCtx->GetVkDevice();
  auto res = vkCreateSwapchainKHR(vkDevice, &swapchainCI, nullptr, &m_swapchain);
  if (res != VK_SUCCESS)
  {
    char buf[64] = { 0 };
    sprintf_s(buf, "vkCreateSwapchainKHR failed (%d).\n", res);
    return false;
  }
  if (oldSwapchain != VK_NULL_HANDLE)
  {
    for (auto view : m_imageViews)
    {
      vkDestroyImageView(vkDevice, view, nullptr);
    }
    vkDestroySwapchainKHR(vkDevice, oldSwapchain, nullptr);
  }
  uint32_t imageCount = 0;
  vkGetSwapchainImagesKHR(vkDevice, m_swapchain, &imageCount, nullptr);
  m_imageViews.resize(imageCount);
  m_images.resize(imageCount);

  vkGetSwapchainImagesKHR(vkDevice, m_swapchain, &imageCount, m_images.data());
  for (uint32_t i = 0; i < imageCount; ++i)
  {
    VkImageViewCreateInfo viewCI{
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = m_images[i],
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = m_surfaceFormat.format,
        .components = { 
          VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A
        },
        .subresourceRange = {
          .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
          .baseMipLevel = 0,
          .levelCount = 1,
          .baseArrayLayer = 0,
          .layerCount = 1
        }
    };
    vkCreateImageView(vkDevice, &viewCI, nullptr, &m_imageViews[i]);
  }
  m_extent2D = swapchainCI.imageExtent;

  return true;
}

VkResult Swapchain::AcquireNextImage(VkSemaphore semPresentComplete)
{
  auto devCtx = DeviceContext::GetContext();
  auto vkDevice = devCtx->GetVkDevice();
  uint32_t index = 0;
  auto res = vkAcquireNextImageKHR(vkDevice, m_swapchain, UINT64_MAX, semPresentComplete, VK_NULL_HANDLE, &index);
  if (res != VK_SUCCESS)
  {
    auto str = std::format("vkAcquireNextImageKHR failed. (result = {:d})\n", (int)res);
    OutputDebugStringA(str.c_str());
    return res;
  }

  m_currentIndex = index;
  return res;
}
