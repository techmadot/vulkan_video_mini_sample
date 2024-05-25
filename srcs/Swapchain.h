#pragma once

#include "DeviceContext.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include "GLFW/glfw3.h"
#include "GLFW/glfw3native.h"

class Swapchain
{
public:
  bool Initialize(GLFWwindow* window, VkFormat desiredFormat = VK_FORMAT_R8G8B8A8_UNORM);
  void Shutdown();

  bool Resize(VkExtent2D newExtent);

  uint32_t GetCurrentIndex() const { return m_currentIndex; }
  VkResult AcquireNextImage(VkSemaphore semPresentComplete);

  VkSurfaceKHR m_surface;

  uint32_t GetImageCount() const { return uint32_t(m_imageViews.size()); }
  VkImageView GetImageView(int index) const { return m_imageViews[index]; }
  VkSurfaceFormatKHR GetSurfaceFormat() const { return m_surfaceFormat; }
  VkExtent2D GetExtent2D() const { return m_extent2D; }
  VkSwapchainKHR GetHandle() const { return m_swapchain; }
private:
  uint32_t m_imageCount = 2;
  VkPresentModeKHR m_presentMode = VK_PRESENT_MODE_FIFO_KHR;
  VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
  VkSurfaceFormatKHR m_surfaceFormat = { .format = VK_FORMAT_UNDEFINED, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };

  VkExtent2D m_extent2D;
  std::vector<VkImage> m_images;
  std::vector<VkImageView> m_imageViews;
  uint32_t m_currentIndex = 0;
};