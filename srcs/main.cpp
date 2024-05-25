#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#undef ERROR
#ifndef VK_NO_PROTOTYPES
# error "VK_NO_PROTOTYPESを定義して下さい.volkを使用しています"
#endif

// volk をインクルード.
// VOLK_IMPLEMENTATIONを定義して実装も含める.
#define VOLK_IMPLEMENTATION
#define VK_USE_PLATFORM_WIN32_KHR
#include "Volk/volk.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include "GLFW/glfw3.h"
#include "GLFW/glfw3native.h"

#include <cstdint>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <array>
#include <format>

#include "DeviceContext.h"
#include "Swapchain.h"
#include "VideoPlayer.h"

#include "imgui.h"
#define IMGUI_IMPL_VULKAN_HAS_DYNAMIC_RENDERING
#include "backends/imgui_impl_vulkan.h"
#include "backends/imgui_impl_glfw.h"
#include "implot.h"

#include "vertexShader.h"
#include "fragementShader.h"

class Swapchain;

class VkVideoDecodeApp
{
public:
	bool Initialize()
	{
		// GLFWの初期化.
		if (!glfwInit()) {
			return false;
		}

		// GLFWでVulkanを使用することを指定.
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

		// ウィンドウを生成.
		m_window = glfwCreateWindow(1280, 720, "Sample", nullptr, nullptr);
		if (!m_window)
		{
			return false;
		}
		glfwSetWindowUserPointer(m_window, this);

		DeviceContext::Initialize();
		DeviceContext::GetContext()->InitializeDevice(0);
		DeviceContext::GetContext()->InitializeSwapchain(m_window);

		auto devCtx = DeviceContext::GetContext();
		auto vkDevice = devCtx->GetVkDevice();

		std::vector<VkDescriptorPoolSize> descriptorPoolSizes = { {
			{
				.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				.descriptorCount = 1000,
			},
			{
				.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = 1000,
			}
		} };
		VkDescriptorPoolCreateInfo descriptorPoolCI{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.maxSets = 100,
			.poolSizeCount = uint32_t(descriptorPoolSizes.size()),
			.pPoolSizes = descriptorPoolSizes.data(),
		};
		vkCreateDescriptorPool(vkDevice, &descriptorPoolCI, nullptr, &m_descriptorPool);

		VkSamplerCreateInfo samplerCI{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = VK_FILTER_LINEAR,
			.minFilter = VK_FILTER_LINEAR,
			.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		};
		VkSamplerYcbcrConversionInfo samplerConversionInfo{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
			.conversion = devCtx->m_samplerYcbcrConversion,
		};
		samplerCI.pNext = &samplerConversionInfo;
		vkCreateSampler(vkDevice, &samplerCI, nullptr, &m_sampler);

		// ImGui
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGui::StyleColorsDark();
		ImPlot::CreateContext();

		ImGui_ImplGlfw_InitForVulkan(m_window, true);
		ImGui_ImplVulkan_LoadFunctions(
			[](const char* functionName, void* userArgs) {
				auto devCtx = DeviceContext::GetContext();
				auto vkDevice = devCtx->GetVkDevice();
				auto vkInstance = devCtx->GetVkInstance();
				auto devFuncAddr = vkGetDeviceProcAddr(vkDevice, functionName);
				if (devFuncAddr != nullptr)
				{
					return devFuncAddr;
				}
				auto instanceFuncAddr = vkGetInstanceProcAddr(vkInstance, functionName);
				return instanceFuncAddr;
			});
		std::vector<VkDescriptorSetLayoutBinding> dsLayouts = { {
			{
				.binding = 0,
				.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS,
			},
			{
				.binding = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS,
				.pImmutableSamplers = &m_sampler,
			},
		} };
		VkDescriptorSetLayoutCreateInfo dsLayoutCI{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = uint32_t(dsLayouts.size()),
			.pBindings = dsLayouts.data(),
		};
		vkCreateDescriptorSetLayout(vkDevice, &dsLayoutCI, nullptr, &m_dsLayout);

		InitializeRenderPass();
		InitializePipeline();
		InitializeFramebuffers();

		ImGui_ImplVulkan_InitInfo vkInfo{
			.Instance = devCtx->GetVkInstance(),
			.PhysicalDevice = devCtx->GetGPU(0),
			.Device = vkDevice,
			.QueueFamily = devCtx->GetGraphicsQueueFamilyIndex(),
			.Queue = devCtx->GetQueue(DeviceContext::Graphics),
			.DescriptorPool = m_descriptorPool,
			.RenderPass = m_renderPass,
			.MinImageCount = 2,
			.ImageCount = 2,
			.MSAASamples = VK_SAMPLE_COUNT_1_BIT,
		};
		ImGui_ImplVulkan_Init(&vkInfo);
		auto& io = ImGui::GetIO();
		ImFontConfig cfg;
		cfg.SizePixels = 15;
		io.Fonts->AddFontDefault(&cfg);

		// リソースフォルダにムービーファイルを配置して読み込む.
		m_videoPlayer.Initialize("res/oceans.mp4");

		VkSemaphoreCreateInfo semaphoreCreateInfo{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
		};
		vkCreateSemaphore(vkDevice, &semaphoreCreateInfo, nullptr, &m_semRenderComplete);
		vkCreateSemaphore(vkDevice, &semaphoreCreateInfo, nullptr, &m_semPresentComplete);

		return true;
	}

	void Run()
	{
		m_referenceSlots.resize(300);
		for (auto& graph : m_DPBSlotGraph)
		{
			graph.resize(300);
		}

		LARGE_INTEGER prev, freq;
		QueryPerformanceCounter(&prev);
		QueryPerformanceFrequency(&freq);
		while (glfwWindowShouldClose(m_window) == GLFW_FALSE 
			&& glfwGetKey(m_window, GLFW_KEY_ESCAPE) == GLFW_RELEASE)
		{
			glfwPollEvents();

			ImGui_ImplGlfw_NewFrame();
			ImGui_ImplVulkan_NewFrame();
			ImGui::NewFrame();

			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			double elapsed = (now.QuadPart - prev.QuadPart) / double(freq.QuadPart);
			elapsed = std::min(elapsed, 0.5);
			prev = now;

			auto devCtx = DeviceContext::GetContext();
			auto res = devCtx->GetSwapchain()->AcquireNextImage(m_semPresentComplete);
			if (res != VK_SUCCESS)
			{
				devCtx->WaitForIdle();
				return;
			}

			FrameInfo frame;
			BeginFrame(frame);
			auto vkDevice = devCtx->GetVkDevice();

			VkCommandBufferBeginInfo beginInfo{
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			};
			vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);

			// デコード.
			m_videoPlayer.Update(frame.commandBuffer, elapsed);



			auto imageExtent = DeviceContext::GetContext()->GetSwapchain()->GetExtent2D();
			VkClearValue clearValue{};
			clearValue.color = { { 1.0f, 0.6f, 0.5f, 1.0f,} };
			VkRenderPassBeginInfo renderPassBI{};
			renderPassBI.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
			renderPassBI.renderPass = m_renderPass;
			renderPassBI.framebuffer = frame.framebuffer;
			renderPassBI.renderArea.offset = VkOffset2D{ 0, 0 };
			renderPassBI.renderArea.extent = imageExtent;
			renderPassBI.pClearValues = &clearValue;
			renderPassBI.clearValueCount = 1;

			if(m_videoPlayer.IsReady())
			{
				const auto& videoTex = m_videoPlayer.GetVideoTexture();
				// テクスチャを書き込んでみる
				VkDescriptorImageInfo imageInfo{
					.sampler = VK_NULL_HANDLE,
					.imageView = videoTex.texture.view,
					.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				};

				VkWriteDescriptorSet writeDS{
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = frame.descriptorSet,
					.dstBinding = 1,
					.descriptorCount = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.pImageInfo = &imageInfo,
				};
				vkUpdateDescriptorSets(vkDevice, 1, &writeDS, 0, nullptr);
			}

			vkCmdBeginRenderPass(frame.commandBuffer, &renderPassBI, VK_SUBPASS_CONTENTS_INLINE);


			vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
			vkCmdBindDescriptorSets(frame.commandBuffer, 
				VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &frame.descriptorSet, 0, nullptr);

			VkViewport viewport{
				.x = 0,
				.y = 0,
				.width = float(imageExtent.width),
				.height = float(imageExtent.height),
				.minDepth = 0.0f,
				.maxDepth = 1.0f,
			};
			VkRect2D scissor{
				.offset = { 0, 0 },
				.extent = imageExtent,
			};
			vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
			vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);

			if(m_videoPlayer.IsReady())
			{
				vkCmdDraw(frame.commandBuffer, 4, 1, 0, 0);
			}


			auto& videoProps = m_videoPlayer.GetVideoProperties();
			auto& decodeOpe = m_videoPlayer.GetDecodeOperation();

			m_referenceSlots.push_back(decodeOpe.dpbReferenceCount);

			auto DPBused = m_videoPlayer.GetDPBSlotUsed();

			const int GRAPTH_SPAN = 300;
      for (int i = 0; i < std::size(DPBused); ++i)
      {
        m_DPBSlotGraph[i].push_back(DPBused[i]);

				if (m_DPBSlotGraph[i].size() > GRAPTH_SPAN)
				{
					m_DPBSlotGraph[i].erase(m_DPBSlotGraph[i].begin());
				}
      }

			if (m_referenceSlots.size() > GRAPTH_SPAN)
			{
				m_referenceSlots.erase(m_referenceSlots.begin());
			}

			ImGui::SetNextWindowPos(ImVec2(0, 0));
			ImGui::SetNextWindowSize(ImVec2(300, 720));
			ImGui::Begin("Information", nullptr, ImGuiWindowFlags_NoDecoration);
			ImGui::Text("Resolution: %d x %d", videoProps.width, videoProps.height);
			ImGui::Text("Display Frame: %d / %d", m_videoPlayer.GetDisplayFrameNumber(), m_videoPlayer.GetLastVideoFrameNumber());
			
			if (ImPlot::BeginPlot("Reference Slots"))
			{
				auto maxReferences = videoProps.maxReferencePictures;
				ImPlot::SetupAxesLimits(0, GRAPTH_SPAN, 0, maxReferences + 1);
				ImPlot::PlotLine("Slot Used", m_referenceSlots.data(), int(m_referenceSlots.size()));
				ImPlot::EndPlot();
			}
			ImGui::End();


			ImGui::SetNextWindowPos(ImVec2(980, 0));
			ImGui::SetNextWindowSize(ImVec2(300, 720));
			ImGui::Begin("DPBSlot", nullptr, ImGuiWindowFlags_NoResize);
      uint32_t tableFlags = ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV |
        ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable;
      if (ImGui::BeginTable("DPB", 2, tableFlags, ImVec2(-1, 0)))
      {
        ImGui::TableSetupColumn("DPBSlot", ImGuiTableColumnFlags_WidthFixed, 75.0f);
        ImGui::TableSetupColumn("Used");
        ImGui::TableHeadersRow();
        ImPlot::PushColormap(ImPlotColormap_Cool);

        auto maxReferences = videoProps.maxReferencePictures;
        for (int row = 0; row < maxReferences; ++row)
        {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::Text("Slot%d", row);
          ImGui::PushID(row);

          ImGui::TableSetColumnIndex(1);
          ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(0, 0));
          if (ImPlot::BeginPlot("", ImVec2(-1, 35), ImPlotFlags_CanvasOnly))
          {
            ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoDecorations, ImPlotAxisFlags_NoDecorations);
						ImPlot::SetupAxesLimits(0, GRAPTH_SPAN, 0, 1.5, ImGuiCond_Always);
						auto col = ImPlot::GetColormapColor(row);
						ImPlot::SetNextLineStyle(col);
						ImPlot::SetNextFillStyle(col, 0.25);

						ImPlot::PlotLine("", m_DPBSlotGraph[row].data(), m_DPBSlotGraph[row].size(), 1, 0, ImPlotLineFlags_Shaded, 0);

            ImPlot::EndPlot();
          }
          ImPlot::PopStyleVar();
          ImGui::PopID();
        }
        ImPlot::PopColormap();
        ImGui::EndTable();
      }
      ImGui::End();


			ImGui::Render();
			ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.commandBuffer);


			vkCmdEndRenderPass(frame.commandBuffer);

			vkEndCommandBuffer(frame.commandBuffer);

			VkPipelineStageFlags wait_stage[] = { 
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT	};
			std::vector<VkSemaphore> waitSemaphores = {	m_semPresentComplete, };

			VkSubmitInfo submitInfo{
				.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
				.waitSemaphoreCount = uint32_t(waitSemaphores.size()),
				.pWaitSemaphores = waitSemaphores.data(),
				.pWaitDstStageMask = wait_stage,
				.commandBufferCount = 1,
				.pCommandBuffers = &frame.commandBuffer,
				.signalSemaphoreCount = 1,
				.pSignalSemaphores = &m_semRenderComplete,
			};
			devCtx->Submit(DeviceContext::Graphics, &submitInfo, frame.queueSubmitFence);
			devCtx->Present({ m_semRenderComplete });
		}
	}

	void Shutdown()
	{
		auto devCtx = DeviceContext::GetContext();
		auto vkDevice = devCtx->GetVkDevice();
		vkDeviceWaitIdle(vkDevice);

		if (m_pipeline != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(vkDevice, m_pipeline, nullptr);
			m_pipeline = VK_NULL_HANDLE;
		}
		if (m_pipelineLayout != VK_NULL_HANDLE)
		{
			vkDestroyPipelineLayout(vkDevice, m_pipelineLayout, nullptr);
			m_pipelineLayout = VK_NULL_HANDLE;
		}

		TeardownFramebuffers();
		for (auto& frame : m_frames)
		{
			TeardownPerFrame(frame);
		}
		m_frames.clear();

		vkDestroySemaphore(vkDevice, m_semRenderComplete, nullptr);
		vkDestroySemaphore(vkDevice, m_semPresentComplete, nullptr);
		m_semRenderComplete = VK_NULL_HANDLE;
		m_semPresentComplete = VK_NULL_HANDLE;

		if (m_renderPass != VK_NULL_HANDLE)
		{
			vkDestroyRenderPass(vkDevice, m_renderPass, nullptr);
		}

		DeviceContext::Shutdown();
		if (m_window)
		{
			glfwDestroyWindow(m_window);
			m_window = nullptr;
		}
	}

private:

	void InitializeRenderPass()
	{
		auto devCtx = DeviceContext::GetContext();
		auto vkDevice = devCtx->GetVkDevice();
		VkAttachmentDescription attachment = { 0 };
		attachment.format = devCtx->GetSwapchain()->GetSurfaceFormat().format;
		attachment.samples = VK_SAMPLE_COUNT_1_BIT;
		attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

		VkAttachmentReference colorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

		VkSubpassDescription subpass = { 0 };
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &colorRef;

		VkSubpassDependency dependency = { 
			.srcSubpass = VK_SUBPASS_EXTERNAL,
			.dstSubpass = 0,
			.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			.srcAccessMask = 0,
			.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
		};

		VkRenderPassCreateInfo rp_info = { 
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
			.attachmentCount = 1,
			.pAttachments = &attachment,
			.subpassCount = 1,
			.pSubpasses = &subpass,
			.dependencyCount = 1,
			.pDependencies = &dependency
		};

		vkCreateRenderPass(vkDevice, &rp_info, nullptr, &m_renderPass);
	}
	VkShaderModule CreateShaderModule(const uint32_t* data, size_t length)
	{
		auto vkDevice = DeviceContext::GetContext()->GetVkDevice();
		VkShaderModuleCreateInfo moduleCreateInfo{
			.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			.codeSize = length,
			.pCode = data
		};
		VkShaderModule shaderModule;
		vkCreateShaderModule(vkDevice, &moduleCreateInfo, nullptr, &shaderModule);
		return shaderModule;
	}

	void InitializePipeline()
	{
		auto devCtx = DeviceContext::GetContext();
		auto vkDevice = devCtx->GetVkDevice();
		VkPipelineLayoutCreateInfo layoutInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &m_dsLayout,
		};
		vkCreatePipelineLayout(vkDevice, &layoutInfo, nullptr, &m_pipelineLayout);

		VkPipelineVertexInputStateCreateInfo vertexInput{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
		};

		VkPipelineInputAssemblyStateCreateInfo inputAssembly{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
		};
		inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

		VkPipelineRasterizationStateCreateInfo raster{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			.cullMode = VK_CULL_MODE_BACK_BIT,
			.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
			.lineWidth = 1.0f,
		};

		VkPipelineColorBlendAttachmentState blendAttachment{
			.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
		};

		VkPipelineColorBlendStateCreateInfo blend{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.attachmentCount = 1,
			.pAttachments = &blendAttachment
		};

		VkPipelineViewportStateCreateInfo viewport{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
			.viewportCount = 1,
			.scissorCount = 1,
		};

		VkPipelineDepthStencilStateCreateInfo depthStencil{
			.stencilTestEnable = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO
		};

		VkPipelineMultisampleStateCreateInfo multisample{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
			.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
		};

		std::array<VkDynamicState, 2> dynamics{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

		VkPipelineDynamicStateCreateInfo dynamic{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
			.dynamicStateCount = uint32_t(dynamics.size()),
			.pDynamicStates = dynamics.data(),
		};

		std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages{{
			{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_VERTEX_BIT,
				.module= CreateShaderModule(gVS, sizeof(gVS)),
				.pName = "main",
			},
			{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
				.module = CreateShaderModule(gFS, sizeof(gFS)),
				.pName = "main",
			}
		}};

		VkGraphicsPipelineCreateInfo pipelineCreateInfo{
			.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			.stageCount = uint32_t(shaderStages.size()),
			.pStages = shaderStages.data(),
			.pVertexInputState = &vertexInput,
			.pInputAssemblyState = &inputAssembly,
			.pViewportState = &viewport,
			.pRasterizationState = &raster,
			.pMultisampleState = &multisample,
			.pDepthStencilState = &depthStencil,
			.pColorBlendState = &blend,
			.pDynamicState = &dynamic,
			.layout = m_pipelineLayout,
			.renderPass = m_renderPass,
		};

		vkCreateGraphicsPipelines(vkDevice, VK_NULL_HANDLE, 1, &pipelineCreateInfo, nullptr, &m_pipeline);

		for (auto& m : shaderStages)
		{
			vkDestroyShaderModule(vkDevice, m.module, nullptr);
		}
	}
	void InitializeFramebuffers()
	{
		auto devCtx = DeviceContext::GetContext();
		auto swapchain = devCtx->GetSwapchain();
		auto vkDevice = devCtx->GetVkDevice();
		auto count = swapchain->GetImageCount();
		auto extent = swapchain->GetExtent2D();

		m_frames.resize(swapchain->GetImageCount());

		for (uint32_t i=0;i<count;++i)
		{
			auto view = swapchain->GetImageView(i);
			VkFramebufferCreateInfo framebufferCreateInfo{
				.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
				.renderPass = m_renderPass,
				.attachmentCount = 1,
				.pAttachments = &view,
				.width = extent.width,
				.height = extent.height,
				.layers = 1,
			};
			VkFramebuffer fb;
			vkCreateFramebuffer(vkDevice, &framebufferCreateInfo, nullptr, &fb);
			m_frames[i].framebuffer = fb;
			InitPerFrame(m_frames[i]);
		}

	}

private:
	struct FrameInfo
	{
		VkCommandPool commandPool = VK_NULL_HANDLE;
		VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
		VkFence queueSubmitFence = VK_NULL_HANDLE;
		uint32_t queueIndex = 0;

		VkFramebuffer framebuffer = VK_NULL_HANDLE;
		VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
	};

	GLFWwindow* m_window = nullptr;

	std::vector<FrameInfo>   m_frames{};

	VkSemaphore m_semRenderComplete = VK_NULL_HANDLE;
	VkSemaphore m_semPresentComplete = VK_NULL_HANDLE;
	VkRenderPass m_renderPass = VK_NULL_HANDLE;
	VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
	VkPipeline m_pipeline = VK_NULL_HANDLE;

	VkDescriptorSetLayout m_dsLayout = VK_NULL_HANDLE;
	VkSampler m_sampler = VK_NULL_HANDLE;

	VideoPlayer m_videoPlayer;

	std::vector<int> m_referenceSlots;
	std::vector<int> m_DPBSlotGraph[18];


	VkDescriptorPool m_descriptorPool;

	void InitPerFrame(FrameInfo& frameInfo)
	{
		auto devCtx = DeviceContext::GetContext();
		auto vkDevice = devCtx->GetVkDevice();
		VkFenceCreateInfo fenceCreateInfo{
			.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
			.flags = VK_FENCE_CREATE_SIGNALED_BIT,
		};
		vkCreateFence(vkDevice, &fenceCreateInfo, nullptr, &frameInfo.queueSubmitFence);

		VkCommandPoolCreateInfo commandPoolCreateInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
			.queueFamilyIndex = devCtx->GetGraphicsQueueFamilyIndex(),
		};
		vkCreateCommandPool(vkDevice, &commandPoolCreateInfo, nullptr, &frameInfo.commandPool);

		VkCommandBufferAllocateInfo commandBufferAllocateInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = frameInfo.commandPool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1,
		};
		vkAllocateCommandBuffers(vkDevice, &commandBufferAllocateInfo, &frameInfo.commandBuffer);
		frameInfo.queueIndex = 0;

		VkDescriptorSetAllocateInfo dsAllocInfo{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = m_descriptorPool,
			.descriptorSetCount = 1,
			.pSetLayouts = &m_dsLayout,
		};
		vkAllocateDescriptorSets(vkDevice, &dsAllocInfo, &frameInfo.descriptorSet);
	}

	void TeardownPerFrame(FrameInfo& frameInfo)
	{
		auto devCtx = DeviceContext::GetContext();
		auto vkDevice = devCtx->GetVkDevice();

		if (frameInfo.queueSubmitFence != VK_NULL_HANDLE)
		{
			vkDestroyFence(vkDevice, frameInfo.queueSubmitFence, nullptr);
			frameInfo.queueSubmitFence = VK_NULL_HANDLE;
		}
		if (frameInfo.commandBuffer != VK_NULL_HANDLE)
		{
			vkFreeCommandBuffers(vkDevice, frameInfo.commandPool, 1, &frameInfo.commandBuffer);
			frameInfo.commandBuffer = VK_NULL_HANDLE;
		}
		if (frameInfo.commandPool != VK_NULL_HANDLE)
		{
			vkDestroyCommandPool(vkDevice, frameInfo.commandPool, nullptr);
			frameInfo.commandPool = VK_NULL_HANDLE;
		}

		if (frameInfo.framebuffer != VK_NULL_HANDLE)
		{
			vkDestroyFramebuffer(vkDevice, frameInfo.framebuffer, nullptr);
			frameInfo.framebuffer = VK_NULL_HANDLE;
		}

		frameInfo.queueIndex = 0;
	}

	void TeardownFramebuffers()
	{
		auto devCtx = DeviceContext::GetContext();
		devCtx->WaitForIdle();
		auto vkDevice = devCtx->GetVkDevice();
		for (auto& frame : m_frames)
		{
			vkDestroyFramebuffer(vkDevice, frame.framebuffer, nullptr);
			frame.framebuffer = VK_NULL_HANDLE;
		}
	}

	void BeginFrame(FrameInfo& frame)
	{
		auto devCtx = DeviceContext::GetContext();
		auto index = devCtx->GetSwapchain()->GetCurrentIndex();
		auto vkDevice = devCtx->GetVkDevice();
		frame = m_frames[index];
		if (frame.queueSubmitFence != VK_NULL_HANDLE)
		{
			vkWaitForFences(vkDevice, 1, &frame.queueSubmitFence, VK_TRUE, UINT64_MAX);
			vkResetFences(vkDevice, 1, &frame.queueSubmitFence);
		}

		if (frame.commandPool != VK_NULL_HANDLE)
		{
			vkResetCommandPool(vkDevice, frame.commandPool, 0);
		}
	}
};

int __stdcall wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR lpCmdLine,
	_In_ int nCmdShow)
{

	VkVideoDecodeApp app;
	if (app.Initialize())
	{
		app.Run();
		app.Shutdown();
	}
	else
	{
		return -1;
	}

	return 0;
}

