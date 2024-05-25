#pragma once


#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <vector>
#include <string>
#include <memory>

#pragma warning(push)
#pragma warning(disable: 4068)
#include "vk_mem_alloc.h"
#pragma warning(pop)

#include "Swapchain.h"

struct GPUBufferDesc
{
	VkDeviceSize size = 0;
	VkBufferUsageFlags usage = 0;
	VkMemoryPropertyFlags memoryProperty = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
};
struct GPUImageDesc
{
	VkExtent3D extent{ 0 };
	uint32_t arraySize = 1;
	uint32_t mipLevels = 1;
	VkImageType type = VK_IMAGE_TYPE_2D;
	VkFormat format = VK_FORMAT_UNDEFINED;
	uint32_t sampleCount = 1;
	VkImageUsageFlags usage = 0;
	VkMemoryPropertyFlags memoryProperty = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
};

struct GPUBuffer {
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	
	VkDeviceAddress deviceAddress = 0;
	void* pMapped = nullptr;

	GPUBufferDesc desc;
};
struct GPUImage {
	VkImage image = VK_NULL_HANDLE;
	VkImageView imageView = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;

	VkDeviceAddress deviceAddress = 0;
	void* pMapped = nullptr;

	GPUImageDesc desc;
};

class Swapchain;
struct GLFWwindow;

class DeviceContext
{
public:
	static bool Initialize();
	static void Shutdown();
	static DeviceContext* GetContext();

	bool InitializeDevice(int useGpuIndex);
	bool InitializeSwapchain(GLFWwindow* window);

	uint32_t GetGPUCount()const { return uint32_t(std::size(m_gpus)); }
	VkPhysicalDevice GetGPU(int gpuIndex) const { return m_gpus[gpuIndex]; }
	VkPhysicalDevice GetGPU() const { return m_gpus[m_useGpuIndex]; }

	VkInstance GetVkInstance();
	VkDevice   GetVkDevice();


	void CreateBuffer(const GPUBufferDesc* desc, GPUBuffer* buffer);
	void CreateImage(const GPUImageDesc& desc, GPUImage* image);

	void DestroyBuffer(GPUBuffer*);
	void DestroyImage(GPUImage*);

	enum QueueType
	{
		Graphics, VideoDecode,
	};
	void Submit(QueueType type, const VkSubmitInfo*, VkFence waitFence);

	void Present(std::vector<VkSemaphore> waitSemaphores);

	void WaitForIdle();

	VkDeviceSize VIDEO_DECODE_BITSTREAM_ALIGNMENT = 1;
	uint32_t GetGraphicsQueueFamilyIndex() const { return m_graphicsFamily; }
	uint32_t GetDecoderQueueFamilyIndex() const { return m_videoDecodeFamily; }

	VmaAllocator GetVmaAllocator() const { return m_vmaAllocator; }

	VkSamplerYcbcrConversion m_samplerYcbcrConversion;
	
	std::shared_ptr<Swapchain> GetSwapchain() { return m_swapchain; }

	VkQueue GetQueue(QueueType type);
private:
	bool InitializeVkInstance();
	void EnumerateGPUs();
	uint32_t GetMemoryTypeIndex(VkMemoryRequirements2 reqs, VkMemoryPropertyFlags flags);

	VkInstance m_vkInstance = VK_NULL_HANDLE;
	VkDevice   m_vkDevice = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT m_debugUtils = VK_NULL_HANDLE;
	uint32_t m_useGpuIndex = 0;

	std::vector<VkPhysicalDevice> m_gpus;
	struct QueueFamilyProperties {
		VkQueueFamilyProperties2 properties;
		VkQueueFamilyVideoPropertiesKHR propertiesVideo;
	};
	std::vector<QueueFamilyProperties> m_queueFamilies;
	std::vector<uint32_t> m_queueFamilyIndices;

	uint32_t m_graphicsFamily = VK_QUEUE_FAMILY_IGNORED;
	uint32_t m_videoDecodeFamily = VK_QUEUE_FAMILY_IGNORED;
	VkQueue m_graphicsQueue;
	VkQueue m_videoDecodeQueue;

	std::shared_ptr<Swapchain> m_swapchain;
	std::vector<VkPhysicalDeviceMemoryProperties2> m_physicalDeviceMemoryProps;


	VkPhysicalDeviceFeatures2 m_features2;
	VkPhysicalDeviceVulkan12Features m_vulkan12Features;
	VkPhysicalDeviceVulkan13Features m_vulkan13Features;

	struct VideoDecodeH264
	{
		VkVideoDecodeH264ProfileInfoKHR		profile{};
		VkVideoDecodeH264CapabilitiesKHR	capabilities{};
	};
	VkVideoProfileInfoKHR m_videoProfileInfo;
	VkVideoCapabilitiesKHR m_videoCapabilities;
	VkVideoDecodeCapabilitiesKHR m_videoDecodeCapabilities;
	VideoDecodeH264 m_videoDecodeH264;

	VmaAllocator m_vmaAllocator;
};
