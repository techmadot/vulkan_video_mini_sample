#define VK_USE_PLATFORM_WIN32_KHR
#include "Volk/volk.h"

#include <filesystem>
#include <cassert>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <sstream>
#include <iomanip>
#include <functional>

#include "DeviceContext.h"

#undef ERROR
#undef min
#undef max


#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"

#define H264_IMPLEMENTATION
#include "h264.h"

#define ARRAY_SIZE( x ) \
	( sizeof( x ) / sizeof( x[ 0 ] ) )

#include <vulkan/vulkan.hpp>
#include "VideoPlayer.h"

static constexpr size_t align_to(size_t sz, size_t alignment) {
	return ((sz - 1) / alignment + 1) * alignment;
};


template<typename T>
constexpr bool hasFlag(uint32_t lhs, T rhs)
{
	return (lhs & rhs) == rhs;
}


bool VideoPlayer::Initialize(const char* filePath)
{
	m_decoder = std::make_shared<Decoder>();
	m_decoder->Initialize(filePath);

	auto devCtx = DeviceContext::GetContext();

	// ����p�̃r�b�g�X�g���[��.
	{
		auto numMemoryFrames = DPB::SlotCount + 1;
		uint64_t bufferSize = m_decoder->m_videoData.maxMemoryFrameSizeBytes * numMemoryFrames;
		VkBufferCreateInfo bufferCI{
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.pNext = &m_decoder->m_settings.profileListInfo,
			.flags = 0,
			.size = bufferSize,
			.usage = VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			.queueFamilyIndexCount = 0,
			.pQueueFamilyIndices = nullptr,
		};
		VmaAllocationCreateInfo allocateCI{};
		allocateCI.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
		allocateCI.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
		allocateCI.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
		vmaCreateBuffer(
			devCtx->GetVmaAllocator(),
			&bufferCI,
			&allocateCI,
			&m_movieBitStreamBuffer.buffer,
			&m_movieBitStreamBuffer.allocation,
			&m_movieBitStreamBuffer.allocationInfo);

		void* pData = nullptr;
		vmaMapMemory(devCtx->GetVmaAllocator(), m_movieBitStreamBuffer.allocation, &pData);
		for (int i = 0; i < numMemoryFrames; ++i)
		{
			auto& frame = m_videoFrames[i];
			frame.gpuBitstreamCapacity = m_decoder->m_videoData.maxMemoryFrameSizeBytes;
			frame.gpuBitstreamOffset = i * m_decoder->m_videoData.maxMemoryFrameSizeBytes;
			frame.gpuBitstreamSize = 0;
			frame.gpuBitstreamSliceMappedMemoryAddress = static_cast<uint8_t*>(pData) + frame.gpuBitstreamOffset;
		}
	}

	// DPB �̗p��.
	for (int i = 0; i < DPB::SlotCount; ++i)
	{
		VmaAllocationCreateInfo allocationCI{
			.flags = { },
			.usage = VMA_MEMORY_USAGE_GPU_ONLY,
			.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
		};

		VkImageCreateInfo imageCI = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.pNext = &m_decoder->m_settings.profileListInfo,
			.flags = 0,
			.imageType = VK_IMAGE_TYPE_2D,
			.format = m_decoder->m_properties.formatProps.format,
			.extent = {
				.width = m_decoder->m_videoData.width,
				.height = m_decoder->m_videoData.height,
				.depth = 1
			},
			.mipLevels = 1,
			.arrayLayers = 1,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.tiling = VK_IMAGE_TILING_OPTIMAL,
			.usage = m_decoder->m_properties.usageDPB,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			.queueFamilyIndexCount = 0,
			.pQueueFamilyIndices = nullptr,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};
		VkSamplerYcbcrConversionInfo samplerConversionInfo{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
			.conversion = devCtx->m_samplerYcbcrConversion,
		};
		VkImageViewCreateInfo imageViewCI = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.pNext = &samplerConversionInfo,
			.flags = 0,
			.image = VK_NULL_HANDLE,
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = imageCI.format,
			.components = {},
			.subresourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1,
			},
		};
		auto& dpb = m_dpb.image[i];
		{
			auto res = vmaCreateImage(
				devCtx->GetVmaAllocator(),
				&imageCI,
				&allocationCI,
				&dpb.image,
				&dpb.allocation,
				&dpb.allocationInfo
			);
			assert(res == VK_SUCCESS);
			imageViewCI.image = dpb.image;

			res = vkCreateImageView(
				devCtx->GetVkDevice(),
				&imageViewCI, nullptr, &dpb.view);
			assert(res == VK_SUCCESS);

			std::string name;
			name = "dpbSlot:";
			name += std::to_string(i);
			VkDebugUtilsObjectNameInfoEXT nameInfo{
				.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
				.objectType = VK_OBJECT_TYPE_IMAGE,
				.objectHandle = (uint64_t)(void*)dpb.image,
				.pObjectName = name.c_str(),
			};
			vkSetDebugUtilsObjectNameEXT(devCtx->GetVkDevice(), &nameInfo);
		}
	}

	auto vkDevice = devCtx->GetVkDevice();
	VkCommandPoolCreateInfo commandPoolCI{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	};
	commandPoolCI.queueFamilyIndex = devCtx->GetGraphicsQueueFamilyIndex();
	vkCreateCommandPool(vkDevice, &commandPoolCI, nullptr, &m_gfxCommandPool);
	commandPoolCI.queueFamilyIndex = devCtx->GetDecoderQueueFamilyIndex();
	vkCreateCommandPool(vkDevice, &commandPoolCI, nullptr, &m_videoCommandPool);

	m_commandBuffersInfo.resize(devCtx->GetSwapchain()->GetImageCount());
	for (auto& info : m_commandBuffersInfo)
	{
		VkCommandBufferAllocateInfo ai{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1,
		};
		ai.commandPool = m_gfxCommandPool;
		vkAllocateCommandBuffers(devCtx->GetVkDevice(), &ai, &info.graphicsCommandBuffer);
		ai.commandPool = m_videoCommandPool;
		vkAllocateCommandBuffers(devCtx->GetVkDevice(), &ai, &info.videoCommandBuffer);

		VkSemaphoreCreateInfo semCI{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		};
		vkCreateSemaphore(vkDevice, &semCI, nullptr, &info.semVideoToGfx);
	}
	VkEventCreateInfo eventCI{
		.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO,
	};
	vkCreateEvent(vkDevice, &eventCI, nullptr, &m_evtVideoPlayer);


	return true;
}

void VideoPlayer::Shutdown()
{
	auto devCtx = DeviceContext::GetContext();
	auto vkDevice = devCtx->GetVkDevice();
	vkDestroyEvent(vkDevice, m_evtVideoPlayer, nullptr);
}

void VideoPlayer::Update(VkCommandBuffer graphicsCmdBuffer, double elapsedTime)
{
	m_decodeOpration = {};


	UpdateDisplayFrame(elapsedTime);

	if (m_isStopped)
	{
		m_DPBSlotUsed.assign(m_decoder->m_videoData.maxReferencePictures, 0);
		return;
	}

	if (MAX_TEXTURE_COUNT <= m_outputTexturesUsed.size() )
	{
		OutputDebugStringA("Decode skip\n");
		return;
	}
	if (!m_isPrepared && DPB::SlotCount <= m_outputTexturesUsed.size() )
	{
		// �Œ���̃f�[�^�����܂����珀�������Ƃ���.
		m_isPrepared = true;
	}

	UpdateDecodeVideo();

	vkCmdWaitEvents(graphicsCmdBuffer, 1, &m_evtVideoPlayer,
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		0, nullptr, 0, nullptr, 0, nullptr);
	vkCmdResetEvent(graphicsCmdBuffer, m_evtVideoPlayer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

	// ���M.
	auto devCtx = DeviceContext::GetContext();
	auto& commandBufferInfo = m_commandBuffersInfo[devCtx->GetSwapchain()->GetCurrentIndex()];
	vkEndCommandBuffer(commandBufferInfo.graphicsCommandBuffer);

	auto semaphore = commandBufferInfo.semVideoToGfx;
	VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	
  {
    VkSubmitInfo submitInfo{
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .waitSemaphoreCount = 0,
    .pWaitSemaphores = nullptr,
    .pWaitDstStageMask = &waitStage,
    .commandBufferCount = 1,
    .pCommandBuffers = &commandBufferInfo.videoCommandBuffer,
    .signalSemaphoreCount = 1,
    .pSignalSemaphores = &semaphore,
    };
    devCtx->Submit(DeviceContext::VideoDecode, &submitInfo, VK_NULL_HANDLE);
  }
	{
		VkSubmitInfo submitInfo{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.waitSemaphoreCount = 1,
			.pWaitSemaphores = &semaphore,
			.pWaitDstStageMask = &waitStage,
			.commandBufferCount = 1,
			.pCommandBuffers = &commandBufferInfo.graphicsCommandBuffer,
		};
		devCtx->Submit(DeviceContext::Graphics, &submitInfo, VK_NULL_HANDLE);
	}

}


int VideoPlayer::GetDecodeFrameNumber() const
{
	return m_current_frame;
}

int VideoPlayer::GetDisplayFrameNumber() const
{
	return m_video_cursor.playIndex;
}

int VideoPlayer::GetLastVideoFrameNumber() const
{
	return int(m_decoder->m_videoData.frameInfos.size()-1);
}

const VideoPlayer::Decoder::VideoFilePropertis& VideoPlayer::GetVideoProperties() const
{
	return m_decoder->m_videoData;
}

void VideoPlayer::VideoDecodeCore(std::shared_ptr<Decoder> decoder, const Decoder::VideoDecodeOperation* operation, VkCommandBuffer commandBuffer)
{
	auto sliceHeader = reinterpret_cast<const h264::SliceHeader*>(operation->slideHeader);
	auto pps = reinterpret_cast<const h264::PPS*>(operation->pps);
	auto sps = reinterpret_cast<const h264::SPS*>(operation->sps);

	StdVideoDecodeH264PictureInfo stdPictureInfoH264 = {};
	stdPictureInfoH264.pic_parameter_set_id = sliceHeader->pic_parameter_set_id;
	stdPictureInfoH264.seq_parameter_set_id = pps->seq_parameter_set_id;
	stdPictureInfoH264.frame_num = sliceHeader->frame_num;
	stdPictureInfoH264.PicOrderCnt[0] = operation->poc[0];
	stdPictureInfoH264.PicOrderCnt[1] = operation->poc[1];
	stdPictureInfoH264.idr_pic_id = sliceHeader->idr_pic_id;
	stdPictureInfoH264.flags.is_intra = operation->frameType == Decoder::VideoDecodeOperation::FrameType::eIntra ? 1 : 0;
	stdPictureInfoH264.flags.is_reference = operation->referencePriority > 0 ? 1 : 0;
	stdPictureInfoH264.flags.IdrPicFlag = (stdPictureInfoH264.flags.is_intra && stdPictureInfoH264.flags.is_reference) ? 1 : 0;

	{
		auto& frame = m_decoder->m_videoData.frameInfos[operation->decodedFrameIndex];
		stdPictureInfoH264.flags.IdrPicFlag = (frame.nalUnitType == 5) ? 1 : 0;
	}
	stdPictureInfoH264.flags.field_pic_flag = sliceHeader->field_pic_flag;
	stdPictureInfoH264.flags.bottom_field_flag = sliceHeader->bottom_field_flag;
	stdPictureInfoH264.flags.complementary_field_pair = 0;


	VkVideoReferenceSlotInfoKHR referenceSlotInfos[DPB::SlotCount] = { };
	VkVideoPictureResourceInfoKHR referenceSlotPictures[DPB::SlotCount] = { };
	VkVideoDecodeH264DpbSlotInfoKHR dpbSlotH264[DPB::SlotCount] = { };
	StdVideoDecodeH264ReferenceInfo referenceInfosH264[DPB::SlotCount] = { };
	for (uint32_t i = 0; i < operation->dpbSlotNum; ++i)
	{
		auto& slot = referenceSlotInfos[i];
		auto& pic = referenceSlotPictures[i];
		auto& dpb = dpbSlotH264[i];
		auto& info = referenceInfosH264[i];

		slot.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
		slot.pPictureResource = &pic;
		slot.slotIndex = i;
		slot.pNext = &dpb;

		pic.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
		pic.codedOffset = { .x = 0, .y = 0 };
		pic.codedExtent = {
			.width = decoder->m_videoData.width,
			.height = decoder->m_videoData.height,
		};
		pic.baseArrayLayer = 0;
		pic.imageViewBinding = operation->pDPBviews[i];

		dpb.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR;
		dpb.pStdReferenceInfo = &info;

		info.flags.bottom_field_flag = 0;
		info.flags.top_field_flag = 0;
		info.flags.is_non_existing = 0;
		info.flags.used_for_long_term_reference = 0;
		info.FrameNum = operation->dpbFramenum[i];
		info.PicOrderCnt[0] = operation->dpbPoc[i];
		info.PicOrderCnt[1] = operation->dpbPoc[i];
	}
	VkVideoReferenceSlotInfoKHR referenceSlots[DPB::SlotCount] = { };
	for (uint32_t i = 0; i < operation->dpbReferenceCount; ++i)
	{
		uint32_t refSlot = operation->dpbReferenceSlots[i];
		assert(refSlot != operation->current_dpb);
		referenceSlots[i] = referenceSlotInfos[refSlot];
	}
	referenceSlots[operation->dpbReferenceCount] = referenceSlotInfos[operation->current_dpb];
	referenceSlots[operation->dpbReferenceCount].slotIndex = -1;

	VkVideoBeginCodingInfoKHR beginInfo{
		.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR,
		.videoSession = decoder->m_videoSession,
		.videoSessionParameters = m_decoder->m_videoSessionParameters,
		.referenceSlotCount = operation->dpbReferenceCount + 1, // �J�����g����+1
	};
	if (beginInfo.referenceSlotCount > 0)
	{
		beginInfo.pReferenceSlots = referenceSlots;
	}
	vkCmdBeginVideoCodingKHR(commandBuffer, &beginInfo);

	if (operation->flags & Decoder::VideoDecodeOperation::eSessionReset)
	{
		VkVideoCodingControlInfoKHR controlInfo = {};
		controlInfo.sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR;
		controlInfo.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
		vkCmdControlVideoCodingKHR(commandBuffer, &controlInfo);
	}

	VkVideoDecodeInfoKHR decodeInfo = {};
	decodeInfo.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR;
	decodeInfo.srcBuffer = m_movieBitStreamBuffer.buffer;
	decodeInfo.srcBufferOffset = (VkDeviceSize)operation->streamOffset;
	decodeInfo.srcBufferRange = align_to(operation->streamSize, 256/*VIDEO_DECODE_BITSTREAM_ALIGNMENT*/);
	decodeInfo.dstPictureResource = *referenceSlotInfos[operation->current_dpb].pPictureResource;
	decodeInfo.referenceSlotCount = operation->dpbReferenceCount;
	decodeInfo.pReferenceSlots = decodeInfo.referenceSlotCount == 0 ? nullptr : referenceSlots;
	decodeInfo.pSetupReferenceSlot = &referenceSlotInfos[operation->current_dpb];

	{
		assert(operation->current_dpb < m_decoder->m_videoData.numDPBslots);
	}

	uint32_t sliceOffset = 0;
	VkVideoDecodeH264PictureInfoKHR pictureInfoH264{
		.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR,
		.pStdPictureInfo = &stdPictureInfoH264,
		.sliceCount = 1,
		.pSliceOffsets = &sliceOffset,
	};
	decodeInfo.pNext = &pictureInfoH264;

	vkCmdDecodeVideoKHR(commandBuffer, &decodeInfo);

	{
		m_DPBSlotUsed.assign(m_decoder->m_videoData.maxReferencePictures, 0);
		std::stringstream ss;
		ss << "decoded_frame_index:" << operation->decodedFrameIndex << std::endl;
		ss << "  srcOffset:" << decodeInfo.srcBufferOffset << ", srcBufferRange:" << decodeInfo.srcBufferRange;
		ss << "  referenceSlotCount:" << decodeInfo.referenceSlotCount << std::endl;
		for (uint32_t i = 0; i < decodeInfo.referenceSlotCount; ++i) {
			const auto& slot = decodeInfo.pReferenceSlots[i];
			ss << "    [" << i << "] slotIndex:" << slot.slotIndex << "  view:" << std::hex << slot.pPictureResource->imageViewBinding << "\n";

			m_DPBSlotUsed[slot.slotIndex] = 1;
		}
		ss << "  frame_num: " << std::dec << pictureInfoH264.pStdPictureInfo->frame_num << std::endl;
		ss << "  pSetupReferenceSlot: \n";
		ss << "    slotIndex:" << decodeInfo.pSetupReferenceSlot->slotIndex << std::hex << "\n";
		ss << "    viewBinding:" << std::hex << decodeInfo.pSetupReferenceSlot->pPictureResource->imageViewBinding << "\n";

		ss << std::endl;
		OutputDebugStringA(ss.str().c_str());
	}


	VkVideoEndCodingInfoKHR endInfo{
		.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR,
	};
	vkCmdEndVideoCodingKHR(commandBuffer, &endInfo);
}

void VideoPlayer::WriteVideoFrame(DecodeStreamFrame* frame)
{
	const auto& dataFrame = m_decoder->m_videoData.frameInfos[m_current_frame];
	int64_t frameBytes = dataFrame.frameBytes;
	uint64_t gpuBitstreamSize = frame->gpuBitstreamSize;
	auto dstBuffer = frame->gpuBitstreamSliceMappedMemoryAddress;

	m_decoder->m_videoData.inputStream.seekg(dataFrame.srcOffset, std::ios::beg);
	while (frameBytes > 0)
	{
		uint8_t srcBuffer[4];
		m_decoder->m_videoData.inputStream.read(reinterpret_cast<char*>(srcBuffer), sizeof(srcBuffer));
		uint32_t size = ((uint32_t)(srcBuffer[0]) << 24) | ((uint32_t)(srcBuffer[1]) << 16) | ((uint32_t)(srcBuffer[2]) << 8) | srcBuffer[3];
		size += 4;
		assert(frameBytes >= size);

		uint8_t nalHeaderByte = m_decoder->m_videoData.inputStream.peek();

		h264::Bitstream bs = {};
		bs.init(&nalHeaderByte, sizeof(nalHeaderByte));
		h264::NALHeader nal = {};
		h264::read_nal_header(&nal, &bs);

		// Skip over any frame data that is not idr slice or non-idr slice
		if (nal.type != h264::NAL_UNIT_TYPE_CODED_SLICE_IDR &&
			nal.type != h264::NAL_UNIT_TYPE_CODED_SLICE_NON_IDR) {
			frameBytes -= size;
			m_decoder->m_videoData.inputStream.seekg(size - 4, std::ios_base::cur);
			continue;
		}

		if (frame->gpuBitstreamSize + size <= frame->gpuBitstreamCapacity)
		{
			memcpy(dstBuffer, h264::nal_start_code, sizeof(h264::nal_start_code));
			m_decoder->m_videoData.inputStream.read((char*)(dstBuffer + sizeof(h264::nal_start_code)), size - 4);
			frame->gpuBitstreamSize += size;
		}
		else
		{
			DebugBreak();
		}

		break;
	}
	frame->gpuBitstreamSize = align_to(frame->gpuBitstreamSize, m_decoder->m_properties.caps.minBitstreamBufferSizeAlignment);
}

VideoPlayer::Image VideoPlayer::CreateVideoTexture()
{
	auto devCtx = DeviceContext::GetContext();
	Image ret{};

	VmaAllocationCreateInfo allocationCI{
		.flags = { },
		.usage = VMA_MEMORY_USAGE_GPU_ONLY,
		.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
	};

	VkImageUsageFlags imageUsage = \
		VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | \
		VK_IMAGE_USAGE_SAMPLED_BIT;
	

	VkImageCreateInfo imageCI = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = &m_decoder->m_settings.profileListInfo,
		.flags = 0,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = m_decoder->m_properties.formatProps.format,
		.extent = {
			.width = m_decoder->m_videoData.width,
			.height = m_decoder->m_videoData.height,
			.depth = 1
		},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = imageUsage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = 0,
		.pQueueFamilyIndices = nullptr,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkSamplerYcbcrConversionInfo samplerConversionInfo{
		.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
		.conversion = devCtx->m_samplerYcbcrConversion,
	};

	vmaCreateImage(devCtx->GetVmaAllocator(), &imageCI, &allocationCI, &ret.image, &ret.allocation, &ret.allocationInfo);

	VkImageViewCreateInfo imageViewCI = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.pNext = &samplerConversionInfo,
		.flags = 0,
		.image = ret.image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = imageCI.format,
		.components = {},
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
	};
	vkCreateImageView(devCtx->GetVkDevice(), &imageViewCI, nullptr, &ret.view);

	std::string name;
	name = "dispImage:";
	name += std::to_string(m_current_frame);
	VkDebugUtilsObjectNameInfoEXT nameInfo{
		.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
		.objectType = VK_OBJECT_TYPE_IMAGE,
		.objectHandle = (uint64_t)(void*)ret.image,
		.pObjectName = name.c_str(),
	};
	vkSetDebugUtilsObjectNameEXT(devCtx->GetVkDevice(), &nameInfo);

	return ret;
}

bool VideoPlayer::IsReady()
{
	return m_isPrepared;
}

const VideoPlayer::OutputImage& VideoPlayer::GetVideoTexture()
{
	return m_outputTexturesUsed[m_video_cursor.frameIndex];
}

// �f�R�[�h�ς݁E�\���t���[��������
void VideoPlayer::UpdateDisplayFrame(double elapsed)
{
	if (!m_isPrepared || m_isStopped)
	{
		return;
	}

	auto FindVideoFrame = [&](int playIndex) {
		auto frameIt = std::find_if(
			m_outputTexturesUsed.begin(),
			m_outputTexturesUsed.end(),
			[=](auto& frame) { return frame.display_order == playIndex; });
		if (frameIt == m_outputTexturesUsed.end())
		{
			// ��ԋ߂����̂�������.
			int indexAbs = INT_MAX;
			int indexOfFrame = -1;
			for (int i = 0; auto & frame : m_outputTexturesUsed)
			{
				auto diff = std::abs(frame.display_order - m_video_cursor.playIndex);
				if (diff < indexAbs)
				{
					indexOfFrame = i;
					indexAbs = diff;
				}
				i++;
			}
			assert(indexOfFrame != -1);
			frameIt = m_outputTexturesUsed.begin() + indexOfFrame;
		}
		return frameIt;
	};

	auto frameIt = FindVideoFrame(m_video_cursor.playIndex);
	assert(frameIt != m_outputTexturesUsed.end());

	// �o�ߎ��ԕ�������.
	frameIt->duration -= elapsed;
	if (frameIt->duration > 0) {
		m_video_cursor.frameIndex = int(std::distance(m_outputTexturesUsed.begin(), frameIt));
		return;
	}

	double remain = std::abs(frameIt->duration);	// �[�����͎��̃t���[���֎����z��.

	m_video_cursor.playIndex++;
	if (m_decoder->m_videoData.frameInfos.size() <= m_video_cursor.playIndex)
	{
		// �����ȍ~�֓��B.
		m_video_cursor.playIndex = int(m_decoder->m_videoData.frameInfos.size() - 1);
		m_isStopped = true;
	}
	else
	{
		// �폜����.
		m_outputTexturesFree.push_back(*frameIt);
		m_outputTexturesUsed.erase(frameIt);
	}

	frameIt = FindVideoFrame(m_video_cursor.playIndex);
	assert(frameIt != m_outputTexturesUsed.end());
	frameIt->duration -= remain;
	m_video_cursor.frameIndex = int(std::distance(m_outputTexturesUsed.begin(), frameIt));

}

void VideoPlayer::UpdateDecodeVideo()
{
	auto devCtx = DeviceContext::GetContext();
	auto& commandBufferInfo = m_commandBuffersInfo[devCtx->GetSwapchain()->GetCurrentIndex()];
	VkCommandBufferBeginInfo beginCommandBuffer{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	};
	vkResetCommandBuffer(commandBufferInfo.graphicsCommandBuffer, 0);
	vkBeginCommandBuffer(commandBufferInfo.graphicsCommandBuffer, &beginCommandBuffer);

	if (m_isStopped)
	{
		vkCmdSetEvent(commandBufferInfo.graphicsCommandBuffer, m_evtVideoPlayer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
		return;
	}


	auto videoCmdBuffer = commandBufferInfo.videoCommandBuffer;
	vkBeginCommandBuffer(videoCmdBuffer, &beginCommandBuffer);

	const auto& frameInfo = m_decoder->m_videoData.frameInfos[m_current_frame];
	assert(m_decoder->GetSliceHeader() != nullptr);
	assert(m_decoder->GetPPS() != nullptr);
	assert(m_decoder->GetSPS() != nullptr);
	const h264::SliceHeader* sliceHeader = (const h264::SliceHeader*)m_decoder->GetSliceHeader() + m_current_frame;
	const auto pps = (const h264::PPS*)m_decoder->GetPPS() + sliceHeader->pic_parameter_set_id;
	const auto sps = (const h264::SPS*)m_decoder->GetSPS() + pps->seq_parameter_set_id;

	Decoder::VideoDecodeOperation decodeOpe;
	if (m_current_frame == 0 || hasFlag(m_flags, Flags::eDecoderReset))
	{
		decodeOpe.flags = Decoder::VideoDecodeOperation::Flags::eSessionReset;
		m_flags &= ~VideoPlayer::Flags::eDecoderReset;
	}

	if (frameInfo.frameType == Decoder::FrameType::eIntra)
	{
		m_dpb.referenceUsage.clear();
		m_dpb.nextRef = 0;
		m_dpb.nextSlot = 0;
	}

	m_dpb.currentSlot = m_dpb.nextSlot;
	m_dpb.pocStatus[m_dpb.currentSlot] = frameInfo.poc;
	m_dpb.framenumStatus[m_dpb.currentSlot] = sliceHeader->frame_num;

	auto DPBSlotNum = m_decoder->m_videoData.numDPBslots + 1;
	std::vector<VkImage> DPBs(DPBSlotNum, VK_NULL_HANDLE);
	std::vector<VkImageView> DPBViews(DPBSlotNum, VK_NULL_HANDLE);

	for (uint32_t i = 0; i < DPBSlotNum; ++i)
	{
		DPBs[i] = m_dpb.image[i].image;
		DPBViews[i] = m_dpb.image[i].view;
	}

	auto useFrameIndex = m_current_frame % std::size(m_videoFrames);
	m_videoFrames[useFrameIndex].gpuBitstreamSize = 0;
	auto* useFrame = &m_videoFrames[useFrameIndex];
	WriteVideoFrame(useFrame);

	decodeOpe.streamOffset = useFrame->gpuBitstreamOffset;
	decodeOpe.streamSize = useFrame->gpuBitstreamSize;
	decodeOpe.poc[0] = frameInfo.poc;
	decodeOpe.poc[1] = frameInfo.poc;
	decodeOpe.frameType = (Decoder::VideoDecodeOperation::FrameType)frameInfo.frameType;
	decodeOpe.referencePriority = frameInfo.referencePriority;
	decodeOpe.decodedFrameIndex = m_current_frame;
	decodeOpe.slideHeader = sliceHeader;
	decodeOpe.pps = pps;
	decodeOpe.sps = sps;
	decodeOpe.current_dpb = m_dpb.currentSlot;
	decodeOpe.dpbReferenceCount = (uint32_t)m_dpb.referenceUsage.size();
	decodeOpe.dpbReferenceSlots = m_dpb.referenceUsage.data();
	decodeOpe.dpbPoc = m_dpb.pocStatus;
	decodeOpe.dpbFramenum = m_dpb.framenumStatus;
	decodeOpe.dpbSlotNum = DPBSlotNum;
	decodeOpe.pDPBs = DPBs.data();
	decodeOpe.pDPBviews = DPBViews.data();

	m_decodeOpration = decodeOpe;	// �\���p�փR�s�[.

	VideoDecodePreBarrier(videoCmdBuffer);

	VideoDecodeCore(m_decoder, &decodeOpe, videoCmdBuffer);

	// DPB�Ǘ�.
	if (frameInfo.referencePriority > 0)
	{
		if (m_dpb.nextRef >= m_dpb.referenceUsage.size())
		{
			m_dpb.referenceUsage.resize(m_dpb.nextRef + 1);
		}
		auto maxSlotNum = m_decoder->m_videoData.numDPBslots;
		m_dpb.referenceUsage[m_dpb.nextRef] = m_dpb.currentSlot;
		m_dpb.nextRef = (m_dpb.nextRef + 1) % (maxSlotNum - 1);
		m_dpb.nextSlot = (m_dpb.nextSlot + 1) % maxSlotNum;
	}

	m_flags |= Flags::eNeedResolve;
	m_flags |= Flags::eInitiallFirstFrameDecoded;

	// �ҋ@�t���[���֒ǉ�.
	if (m_outputTexturesFree.empty())
	{
		auto newImage = CreateVideoTexture();
		auto& output = m_outputTexturesFree.emplace_back();
		output.texture = newImage;
		output.flags = OutputImage::Flags::eInit;
		output.display_order = 0;
	}
	auto output = std::move(m_outputTexturesFree.back());
	m_outputTexturesFree.pop_back();
	output.display_order = m_decoder->m_videoData.frameInfos[m_current_frame].displayOrder;
	output.duration = m_decoder->m_videoData.frameInfos[m_current_frame].duration;

	// DPB->�o�͐��.
	CopyToTexture(videoCmdBuffer, output);
	m_outputTexturesUsed.push_back(std::move(output));

	// �����DPB�p�Ƀo���A��ݒ�.
	VideoDecodePostBarrier(videoCmdBuffer);

	vkEndCommandBuffer(videoCmdBuffer);

	m_current_frame = (m_current_frame+1) % m_decoder->m_videoData.frameInfos.size();

	{
		// �e�N�X�`���Ƃ��Ďg�p���邽�߂̃��C�A�E�g��.
		VkImageMemoryBarrier2 barrier{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
			.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
			.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = output.texture.image,
			.subresourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1,
			},
		};
		VkDependencyInfo info{
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.imageMemoryBarrierCount = 1,
			.pImageMemoryBarriers = &barrier,
		};

		vkCmdPipelineBarrier2(commandBufferInfo.graphicsCommandBuffer, &info);
		vkCmdSetEvent(commandBufferInfo.graphicsCommandBuffer, m_evtVideoPlayer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
	}
}

void VideoPlayer::VideoDecodePreBarrier(VkCommandBuffer videoCmdBuffer)
{
	std::vector<VkImageMemoryBarrier2> imageBarriers;
	auto devCtx = DeviceContext::GetContext();
	auto decodeQueueFamilyIndex = devCtx->GetDecoderQueueFamilyIndex();
	auto& currentDPBState = m_dpb.resourceState[m_dpb.currentSlot];
	auto DPBSlotNum = m_decoder->m_videoData.numDPBslots + 1;
	std::vector<VkImage> DPBs(DPBSlotNum, VK_NULL_HANDLE);
	for (uint32_t i = 0; i < DPBSlotNum; ++i)
	{
		DPBs[i] = m_dpb.image[i].image;
	}

	if (currentDPBState.layout != VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR
		|| currentDPBState.flag != VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR)
	{
		VkImageMemoryBarrier2 barrier{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
			.srcAccessMask = currentDPBState.flag,
			.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
			.dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR,
			.oldLayout = currentDPBState.layout,
			.newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR,
			.srcQueueFamilyIndex = decodeQueueFamilyIndex,
			.dstQueueFamilyIndex = decodeQueueFamilyIndex,
			.image = DPBs[m_dpb.currentSlot],
			.subresourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1,
			},
		};
		imageBarriers.push_back(barrier);
		currentDPBState.layout = barrier.newLayout;
		currentDPBState.flag = barrier.dstAccessMask;
	}

	for (size_t i = 0; i < m_dpb.referenceUsage.size(); ++i)
	{
		auto refIndex = m_dpb.referenceUsage[i];
		auto& refStateDPB = m_dpb.resourceState[refIndex];
		if (refStateDPB.layout != VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR
			|| refStateDPB.flag != VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR)
		{
			VkImageMemoryBarrier2 barrier{
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				.srcStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
				.srcAccessMask = refStateDPB.flag,
				.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
				.dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR,
				.oldLayout = refStateDPB.layout,
				.newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR,
				.srcQueueFamilyIndex = decodeQueueFamilyIndex,
				.dstQueueFamilyIndex = decodeQueueFamilyIndex,
				.image = DPBs[refIndex],
				.subresourceRange = {
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.baseMipLevel = 0,
					.levelCount = 1,
					.baseArrayLayer = 0,
					.layerCount = 1,
				},
			};
			imageBarriers.push_back(barrier);
			refStateDPB.layout = barrier.newLayout;
			refStateDPB.flag = barrier.dstAccessMask;
		}
	}

	if (!imageBarriers.empty())
	{
		VkDependencyInfo info{
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.imageMemoryBarrierCount = uint32_t(imageBarriers.size()),
			.pImageMemoryBarriers = imageBarriers.data(),
		};
		vkCmdPipelineBarrier2(videoCmdBuffer, &info);
	}
}

void VideoPlayer::VideoDecodePostBarrier(VkCommandBuffer videoCmdBuffer)
{
	std::vector<VkImageMemoryBarrier2> imageBarriers;
	auto devCtx = DeviceContext::GetContext();
	auto decodeQueueFamilyIndex = devCtx->GetDecoderQueueFamilyIndex();
	auto& currentDPBState = m_dpb.resourceState[m_dpb.currentSlot];
	auto& srcImageDPB = m_dpb.image[m_dpb.currentSlot];

	// ����DPB�Ŏg�����߂̃o���A�ݒ�.
  VkImageMemoryBarrier2 barrier{
    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
    .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
    .srcAccessMask = currentDPBState.flag,
    .dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
    .dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR,
    .oldLayout = currentDPBState.layout,
    .newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR,
    .srcQueueFamilyIndex = decodeQueueFamilyIndex,
    .dstQueueFamilyIndex = decodeQueueFamilyIndex,
    .image = srcImageDPB.image,
    .subresourceRange = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .baseMipLevel = 0,
      .levelCount = 1,
      .baseArrayLayer = 0,
      .layerCount = 1,
    },
  };
  currentDPBState.layout = barrier.newLayout;
  currentDPBState.flag = barrier.dstAccessMask;
  imageBarriers.push_back(barrier);

	VkDependencyInfo info{
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = uint32_t(imageBarriers.size()),
		.pImageMemoryBarriers = imageBarriers.data(),
	};
	vkCmdPipelineBarrier2(videoCmdBuffer, &info);
}

void VideoPlayer::CopyToTexture(VkCommandBuffer videoCmdBuffer, OutputImage& dstImage)
{
	std::vector<VkImageMemoryBarrier2> imageBarriers;
	auto devCtx = DeviceContext::GetContext();
	auto decodeQueueFamilyIndex = devCtx->GetDecoderQueueFamilyIndex();
	auto& currentDPBState = m_dpb.resourceState[m_dpb.currentSlot];
	auto& srcImageDPB = m_dpb.image[m_dpb.currentSlot];
	// DPB��]�����֑J��.
	{
		VkImageMemoryBarrier2 barrier{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
			.srcAccessMask = currentDPBState.flag,
			.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
			.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
			.oldLayout = currentDPBState.layout,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			.srcQueueFamilyIndex = decodeQueueFamilyIndex,
			.dstQueueFamilyIndex = decodeQueueFamilyIndex,
			.image = srcImageDPB.image,
			.subresourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1,
			},
		};
		currentDPBState.layout = barrier.newLayout;
		currentDPBState.flag = barrier.dstAccessMask;
		imageBarriers.push_back(barrier);
	}
	// �o�͐��]�����Ԃ֑J��.
	{
		VkImageMemoryBarrier2 barrier{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
			.srcAccessMask = 0,
			.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
			.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.srcQueueFamilyIndex = decodeQueueFamilyIndex,
			.dstQueueFamilyIndex = decodeQueueFamilyIndex,
			.image = dstImage.texture.image,
			.subresourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1,
			},
		};
		if (dstImage.flags & OutputImage::Flags::eInit)
		{
			dstImage.flags = 0;
			barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		}
		imageBarriers.push_back(barrier);
	}
	{
		VkDependencyInfo info{
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.imageMemoryBarrierCount = uint32_t(imageBarriers.size()),
			.pImageMemoryBarriers = imageBarriers.data(),
		};
		vkCmdPipelineBarrier2(videoCmdBuffer, &info);
	}


	// �e�N�X�`���Ƃ��ăR�s�[.
	const auto width = m_decoder->m_videoData.width;
	const auto height = m_decoder->m_videoData.height;
	VkImageCopy2 regions[] = {
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
			.srcSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT,
				.mipLevel = 0, .baseArrayLayer = 0,	.layerCount = 1,
			},
			.srcOffset = { },
			.dstSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT,
				.mipLevel = 0, .baseArrayLayer = 0,	.layerCount = 1,
			},
			.dstOffset = { },
			.extent = {	.width = width, .height = height, .depth = 1,	}
		},
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
			.srcSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT,
				.mipLevel = 0, .baseArrayLayer = 0,	.layerCount = 1,
			},
			.srcOffset = { },
			.dstSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT,
				.mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1,
			},
			.dstOffset = { },
			.extent = { .width = width / 2, .height = height / 2, .depth = 1,	}
		},
	};
	{
		VkCopyImageInfo2 info = {
			.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2,
			.srcImage = srcImageDPB.image,
			.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			.dstImage = dstImage.texture.image,
			.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.regionCount = uint32_t(std::size(regions)),
			.pRegions = regions,
		};
		vkCmdCopyImage2(videoCmdBuffer, &info);
	}
}

void VideoPlayer::Decoder::Initialize(const char* filePath)
{
	auto devCtx = DeviceContext::GetContext();
	m_properties.decodeH264Caps = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR,
	};
	m_properties.decodeCaps = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR,
		.pNext = &m_properties.decodeH264Caps
	};
	m_properties.caps = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR,
		.pNext = &m_properties.decodeCaps,
	};
		
		
	vk::VideoDecodeH264CapabilitiesKHR();
	m_properties.decodeCaps = vk::VideoDecodeCapabilitiesKHR();
	m_properties.decodeCaps.pNext = &m_properties.decodeH264Caps;
	
	m_settings.decodeH264ProfileInfo = vk::VideoDecodeH264ProfileInfoKHR()
		.setStdProfileIdc(STD_VIDEO_H264_PROFILE_IDC_BASELINE)
		.setPictureLayout(vk::VideoDecodeH264PictureLayoutFlagBitsKHR::eInterlacedInterleavedLines);
	m_settings.profileInfo = vk::VideoProfileInfoKHR()
		.setPNext(&m_settings.decodeH264ProfileInfo)
		.setVideoCodecOperation(vk::VideoCodecOperationFlagBitsKHR::eDecodeH264)
		.setChromaSubsampling(vk::VideoChromaSubsamplingFlagBitsKHR::e420)
		.setLumaBitDepth(vk::VideoComponentBitDepthFlagBitsKHR::e8)
		.setChromaBitDepth(vk::VideoComponentBitDepthFlagBitsKHR::e8);

	m_properties.caps = vk::VideoCapabilitiesKHR();
  m_properties.caps.pNext = &m_properties.decodeCaps;

	VkResult res;
	res = vkGetPhysicalDeviceVideoCapabilitiesKHR(
		devCtx->GetGPU(), &m_settings.profileInfo, &m_properties.caps);
	assert(res == VK_SUCCESS);

	// �r�f�I�t�H�[�}�b�g�̊m�F.
	m_settings.profileListInfo = vk::VideoProfileListInfoKHR();
	m_settings.profileListInfo.profileCount = 1;
	m_settings.profileListInfo.pProfiles = &m_settings.profileInfo;

	VkPhysicalDeviceVideoFormatInfoKHR formatInfo = vk::PhysicalDeviceVideoFormatInfoKHR()
		.setPNext(&m_settings.profileListInfo)
		.setImageUsage(
			vk::ImageUsageFlagBits::eVideoDecodeSrcKHR |
			vk::ImageUsageFlagBits::eVideoDecodeDstKHR |
			vk::ImageUsageFlagBits::eVideoDecodeDpbKHR |
			vk::ImageUsageFlagBits::eTransferSrc |
			vk::ImageUsageFlagBits::eSampled
		);
	uint32_t formatPropsCount = 0;
	res = vkGetPhysicalDeviceVideoFormatPropertiesKHR(
		devCtx->GetGPU(), &formatInfo, &formatPropsCount, nullptr);
	assert(res == VK_SUCCESS);
	std::vector<VkVideoFormatPropertiesKHR> videoFormatProps;
	videoFormatProps.resize(formatPropsCount, { .sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR });
	res = vkGetPhysicalDeviceVideoFormatPropertiesKHR(
		devCtx->GetGPU(), &formatInfo, &formatPropsCount, videoFormatProps.data());
	assert(res == VK_SUCCESS);
	assert(videoFormatProps.size() != 0);
	m_properties.formatProps = videoFormatProps.front();

	// �t�@�C���ǂݍ���.
	ParseMp4Data(filePath);

	auto numMemoryFrames = m_videoData.numDPBslots + 1;
	auto videoDecoderQueueFamilyIndex = devCtx->GetDecoderQueueFamilyIndex();
	uint64_t bufferSize = m_videoData.maxMemoryFrameSizeBytes * numMemoryFrames;
	VkBufferCreateInfo bufferCI{
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.pNext = &m_settings.profileListInfo,
		.flags = 0,
		.size = bufferSize,
		.usage = VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = 0,
		.pQueueFamilyIndices = nullptr,
	};
	VmaAllocationCreateInfo allocateCI{};
	allocateCI.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
	allocateCI.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
	allocateCI.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	res = vmaCreateBuffer(
		devCtx->GetVmaAllocator(),
		&bufferCI,
		&allocateCI,
		&m_gpuBitstreamBuffer.buffer,
		&m_gpuBitstreamBuffer.allocation,
		&m_gpuBitstreamBuffer.allocationInfo);
	assert(res == VK_SUCCESS);

	void* pData = nullptr;
	vmaMapMemory(devCtx->GetVmaAllocator(), m_gpuBitstreamBuffer.allocation, &pData);

	if (m_videoData.numDPBslots > m_properties.caps.maxDpbSlots)
	{
		char buf[1024] = { 0 };
		sprintf_s(buf, "Number of requested dpb slots is %d, but device can only provide a maximum of %d\n",
			m_videoData.numDPBslots,m_properties.caps.maxDpbSlots);
		OutputDebugStringA(buf);
	}
	m_videoData.maxReferencePictures = std::min(m_videoData.numDPBslots, m_properties.caps.maxActiveReferencePictures);

	VkVideoSessionCreateInfoKHR sessionCI{
		.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR,
		.queueFamilyIndex = videoDecoderQueueFamilyIndex,
		.pVideoProfile = &m_settings.profileInfo,
		.pictureFormat = m_properties.formatProps.format,
		.maxCodedExtent = {
			.width = std::min(m_videoData.width, m_properties.caps.maxCodedExtent.width),
			.height = std::min(m_videoData.height, m_properties.caps.maxCodedExtent.height),
		},
		.referencePictureFormat = m_properties.formatProps.format,
		.maxDpbSlots = m_videoData.numDPBslots,
		.maxActiveReferencePictures = m_videoData.maxReferencePictures,
		.pStdHeaderVersion = &m_properties.caps.stdHeaderVersion,
	};
	res = vkCreateVideoSessionKHR(devCtx->GetVkDevice(), &sessionCI, nullptr, &m_videoSession);
	assert(res == VK_SUCCESS);

	{
		uint32_t requirementCount = 0;
		vkGetVideoSessionMemoryRequirementsKHR(
			devCtx->GetVkDevice(),
			m_videoSession,
			&requirementCount, nullptr);
		std::vector<VkVideoSessionMemoryRequirementsKHR> requirements(requirementCount, { .sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR });
		res = vkGetVideoSessionMemoryRequirementsKHR(
			devCtx->GetVkDevice(),
			m_videoSession,	&requirementCount, requirements.data());
		assert(res == VK_SUCCESS);
		m_sessionMemoryAllocations.resize(requirementCount);

		std::vector<VkBindVideoSessionMemoryInfoKHR> bindSessionMemoryInfos(requirementCount);
		for (uint32_t i = 0; i < requirementCount; ++i)
		{
			auto& req = requirements[i];
			VmaAllocationInfo				allocInfo{};
			VmaAllocationCreateInfo	allocCI{};
			allocCI.memoryTypeBits = req.memoryRequirements.memoryTypeBits;

			res = vmaAllocateMemory(
				devCtx->GetVmaAllocator(),
				&req.memoryRequirements,
				&allocCI,	&m_sessionMemoryAllocations[i],	&allocInfo);
			assert(res == VK_SUCCESS);

			auto& bindInfo = bindSessionMemoryInfos[i];
			bindInfo = {
				.sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR,
				.memoryBindIndex = req.memoryBindIndex,
				.memory = allocInfo.deviceMemory,
				.memoryOffset = allocInfo.offset,
				.memorySize = allocInfo.size,
			};
		}
		res = vkBindVideoSessionMemoryKHR(
			devCtx->GetVkDevice(), m_videoSession, 
			uint32_t(bindSessionMemoryInfos.size()), bindSessionMemoryInfos.data());
		assert(res == VK_SUCCESS);
	}

	CreateVideoSessionParameters();
	
	if (m_properties.decodeCaps.flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR)
	{
		OutputDebugStringA("NOTE: video decode: dpb and output coincide\n");
	}
	else
	{
		OutputDebugStringA("NOTE: video decode: dpb and output NOT coincide\n");
	}
	if (m_properties.decodeCaps.flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_DISTINCT_BIT_KHR)
	{
		OutputDebugStringA("NOTE: video decode: dpb and output distinct\n");
	}
	else
	{
		OutputDebugStringA("NOTE: video decode: dpb and output NOT distinct\n");
	}
	m_properties.usageDPB = \
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
		VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR |
		VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR;

#if _DEBUG
	// ���̃t���O�𗧂ĂĂ����ƁAnsight graphics �Œ��g��������x�m�F�\.
	m_properties.usageDPB |= VK_IMAGE_USAGE_SAMPLED_BIT;
#endif

	PrepareDecodedPictureBuffer();

	m_info.memoryFrames.resize(numMemoryFrames);
	for (auto i = 0; auto& frame : m_info.memoryFrames)
	{
		frame.gpuBitstreamCapacity = m_videoData.maxMemoryFrameSizeBytes;
		frame.gpuBitstreamOffset = i * m_videoData.maxMemoryFrameSizeBytes;
		frame.gpuBitstreamSize = 0;
		frame.gpuBitstreamSliceMappedMemoryAddress = static_cast<uint8_t*>(pData) + frame.gpuBitstreamOffset;
		i++;
	}

	m_videoData.inputStream.seekg(0, std::ios::beg);
}


void VideoPlayer::Decoder::ParseMp4Data(const char* filePath)
{
	m_videoData.inputStream = std::ifstream(filePath, std::ios::binary | std::ios::ate);
	assert(m_videoData.inputStream);

	auto mp4FileSize = m_videoData.inputStream.tellg();
	m_videoData.inputStream.seekg(0, std::ios::beg);

	MP4D_demux_t mp4 = {};

	struct CallbackUserData
	{
		std::ifstream* stream;
		uint64_t lastOffset;
	};
	auto readCallback = [](int64_t offset, void* buffer, size_t size, void* userData)-> int {
		auto data = reinterpret_cast<CallbackUserData*>(userData);
		uint64_t offsetDelta = offset - data->lastOffset;
		data->stream->seekg(offsetDelta, std::ios::cur);
		data->stream->read(static_cast<char*>(buffer), size);
		data->lastOffset = offset + size;
		return data->stream->eof();
	};
	CallbackUserData userData{
		.stream = &m_videoData.inputStream,
		.lastOffset = 0
	};

	MP4D_open(&mp4, readCallback, &userData, mp4FileSize);

	int ntrack = 0;
	{
		assert(mp4.track != nullptr);
		MP4D_track_t& track = mp4.track[ntrack];
		uint32_t sumDuration = 0;
		int i = 0;
		if (track.handler_type == MP4D_HANDLER_TYPE_VIDE)
		{
			if ( !(track.object_type_indication == MP4_OBJECT_TYPE_AVC
				 || track.object_type_indication == MP4_OBJECT_TYPE_HEVC) )
			{
				OutputDebugStringA("H.264 (AVC) or H.265(HVC) suppoort only.\n");
			}
		}

		{
			// Read SPS
			const void* data = nullptr;
			int size = 0;
			int index = 0;
			while (data = MP4D_read_sps(&mp4, ntrack, index, &size))
			{
				auto spsData = reinterpret_cast<const uint8_t*>(data);
				h264::Bitstream bs = {};
				bs.init(spsData, size);

				h264::NALHeader nal = {};
				h264::read_nal_header(&nal, &bs);

				h264::SPS sps = { };
				h264::read_sps(&sps, &bs);

				// Some validation checks that data parsing returned expected values:
				// https://stackoverflow.com/questions/6394874/fetching-the-dimensions-of-a-h264video-stream
				uint32_t width = ((sps.pic_width_in_mbs_minus1 + 1) * 16) - sps.frame_crop_left_offset * 2 - sps.frame_crop_right_offset * 2;
				uint32_t height = ((2 - sps.frame_mbs_only_flag) * (sps.pic_height_in_map_units_minus1 + 1) * 16) - (sps.frame_crop_top_offset * 2) - (sps.frame_crop_bottom_offset * 2);
				assert(track.SampleDescription.video.width == width);
				assert(track.SampleDescription.video.height == height);

				m_videoData.widthPadd = (sps.pic_width_in_mbs_minus1 + 1) * 16;
				m_videoData.heightPadd = (sps.pic_height_in_map_units_minus1 + 1) * 16;
				m_videoData.numDPBslots = std::max(m_videoData.numDPBslots, uint32_t(sps.num_ref_frames * 2 + 1));
				m_videoData.spsBytes.resize(m_videoData.spsBytes.size() + sizeof(sps));
				memcpy((h264::SPS*)m_videoData.spsBytes.data() + m_videoData.spsCount, &sps, sizeof(sps));
				m_videoData.spsCount++;
				index++;
			}
		}
		{
			// Read PPS
			const void* data = nullptr;
			int size = 0;
			int index = 0;
			while (data = MP4D_read_pps(&mp4, ntrack, index, &size))
			{
				auto ppsData = reinterpret_cast<const uint8_t*>(data);
				h264::Bitstream bs = {};
				bs.init(ppsData, size);

				h264::NALHeader nal = {};
				h264::read_nal_header(&nal, &bs);

				h264::PPS pps = { };
				h264::read_pps(&pps, &bs);
				m_videoData.ppsBytes.resize(m_videoData.ppsBytes.size() + sizeof(pps));
				memcpy((h264::PPS*)m_videoData.ppsBytes.data() + m_videoData.ppsCount, &pps, sizeof(pps));
				m_videoData.ppsCount++;
				index++;
			}
		}

		const std::vector<h264::PPS> ppsArray = {
			(const h264::PPS*)m_videoData.ppsBytes.data(),
			(const h264::PPS*)m_videoData.ppsBytes.data() + m_videoData.ppsCount
		};
		const std::vector<h264::SPS> spsArray = {
			(const h264::SPS*)m_videoData.spsBytes.data(),
			(const h264::SPS*)m_videoData.spsBytes.data() + m_videoData.spsCount
		};
		m_videoData.width = track.SampleDescription.video.width;
		m_videoData.height = track.SampleDescription.video.height;
		const auto timescale_rcp = 1.0 / double(track.timescale);

		int prevPicOrderCntLSB = 0, prevPicOrderCntMSB = 0;
		int pocCycle = -1;
		int prevFrameNum = 0, prevFrameOffset = 0;

		// read frames
		uint32_t trackDuration = 0;
		uint64_t maxFrameSizeBytes = 0;
		uint64_t inputFilePosition = 0;
		m_videoData.inputStream.seekg(0, std::ios::beg);

		m_videoData.frameInfos.reserve(track.sample_count);
		m_videoData.sliceHeaderBytes.reserve(track.sample_count * sizeof(h264::SliceHeader));
		m_videoData.sliceHeaderCount = track.sample_count;

		auto* stream = &m_videoData.inputStream;
		for (uint32_t sampleIndex = 0; sampleIndex < track.sample_count; ++sampleIndex)
		{
			uint32_t frameBytes = 0;
			uint32_t duration = 0;
			uint32_t dts = 0, pts = 0;
			auto offset = MP4D_frame_offset(&mp4, ntrack, sampleIndex, &frameBytes, &dts, &pts, &duration, nullptr);
			trackDuration += duration;

			auto& dataFrame = m_videoData.frameInfos.emplace_back();
			dataFrame.srcOffset = offset;
			dataFrame.frameBytes = frameBytes;

			std::vector<uint8_t> srcBufferData(frameBytes);
			auto srcBuffer = srcBufferData.data();

			if (offset - inputFilePosition != 0)
			{
				stream->seekg(offset - inputFilePosition, std::ios::cur);
			}
			assert( stream->eof() == false);
			stream->read(reinterpret_cast<char*>(srcBuffer), frameBytes);
			inputFilePosition = offset + frameBytes;

			while (frameBytes > 0)
			{
				uint32_t size = ((uint32_t)srcBuffer[0] << 24) | ((uint32_t)srcBuffer[1] << 16) | ((uint32_t)srcBuffer[2] << 8) | srcBuffer[3];
				size += 4;
				assert(frameBytes >= size);

				h264::Bitstream bs = {};
				bs.init(&srcBuffer[4], size);

				h264::NALHeader nal = {};
				h264::read_nal_header(&nal, &bs);

				bool isIDR = false;
				switch (nal.type)
				{
					case h264::NAL_UNIT_TYPE_CODED_SLICE_IDR:
						dataFrame.frameType = FrameType::eIntra;
						isIDR = true;
						break;
					case h264::NAL_UNIT_TYPE_CODED_SLICE_NON_IDR:
						dataFrame.frameType = FrameType::ePredictive;
						break;

					default:
						frameBytes -= size;
						srcBuffer += size;
						continue;
				}

				/*
				 * Decode Picture Order Count
				 * (tig) see ITU-T H.264 (08/2021) pp.113
				 *
				 */
				 // tig: see Rec. ITU-T H.264 (08/2021) p.66 (7-1)
        h264::SliceHeader* sliceHeader = (h264::SliceHeader*)m_videoData.sliceHeaderBytes.data() + sampleIndex;
        *sliceHeader = {};
        h264::read_slice_header(sliceHeader, &nal, ppsArray.data(), spsArray.data(), &bs);
				auto& pps = ppsArray[sliceHeader->pic_parameter_set_id];
				auto& sps = spsArray[pps.seq_parameter_set_id];

				auto maxFrameNum = uint32_t(1) << (sps.log2_max_frame_num_minus4 + 4);
				int maxPicOrderCntLsb = 1 << (sps.log2_max_pic_order_cnt_lsb_minus4 + 4);
				int picOrderCntLSB = sliceHeader->pic_order_cnt_lsb;
				int picOrderCntMSB = 0;

				int frameNumOffset = 0;
				int tmpPicOrderCount = 0;

				switch (sps.pic_order_cnt_type)
				{
				case 0:
					// TYPE 0
					// Rec. ITU-T H.264 (08/2021) page 114
					// important to use the NAL unit type for this - and not the idr flag
					if (isIDR) {
						prevPicOrderCntMSB = 0;
						prevPicOrderCntLSB = 0;
						pocCycle++;
					}

					if ((picOrderCntLSB < prevPicOrderCntLSB) &&
						(prevPicOrderCntLSB - picOrderCntLSB) >= maxPicOrderCntLsb / 2) {
						picOrderCntMSB = prevPicOrderCntMSB + maxPicOrderCntLsb;
					} else if (
						(picOrderCntLSB > prevPicOrderCntLSB) &&
						(picOrderCntLSB - prevPicOrderCntLSB) > maxPicOrderCntLsb / 2) {
						picOrderCntMSB = prevPicOrderCntMSB - maxPicOrderCntLsb;
					} else {
						picOrderCntMSB = prevPicOrderCntMSB;
					}

					{
						// Top and bottom field order count in case the picture is a field
						if (!sliceHeader->field_pic_flag || !sliceHeader->bottom_field_flag) {
							dataFrame.topFieldOrderCnt = picOrderCntMSB + picOrderCntLSB;
						}
						if (!sliceHeader->field_pic_flag) {
							dataFrame.bottomFieldOrderCnt = dataFrame.topFieldOrderCnt + sliceHeader->delta_pic_order_cnt_bottom;
						} else if (sliceHeader->bottom_field_flag) {
							dataFrame.bottomFieldOrderCnt = picOrderCntMSB + sliceHeader->pic_order_cnt_lsb;
						}
					}
					dataFrame.poc = picOrderCntMSB + picOrderCntLSB; // same as top field order count
					dataFrame.gop = pocCycle;

					//  TODO: check for memory management operation command 5

					if (nal.idc != 0) {
						prevPicOrderCntMSB = picOrderCntMSB;
						prevPicOrderCntLSB = picOrderCntLSB;
					}
					break;

				case 2:
					// TYPE 2
					if (isIDR) {
						frameNumOffset = 0;
					} else if (prevFrameNum > sliceHeader->frame_num) {
						frameNumOffset = prevFrameOffset + maxFrameNum;
					} else {
						frameNumOffset = prevFrameOffset;
					}
					prevFrameOffset = frameNumOffset;
					prevFrameNum = sliceHeader->frame_num;

					if (isIDR) {
						tmpPicOrderCount = 0;
					} else if (nal.idc == h264::NAL_REF_IDC(0)) {
						tmpPicOrderCount = 2 * (frameNumOffset + sliceHeader->frame_num) - 1;
					} else {
						tmpPicOrderCount = 2 * (frameNumOffset + sliceHeader->frame_num);
					}

					// (tig) we don't care about bottom or top fields as we assume progressive
					// if it were otherwise, for interleaved either the top or the bottom
					// field shall be set - depending on whether the current picture is the
					// top or the bottom field as indicated by bottom_field_flag
					dataFrame.poc = tmpPicOrderCount;
					if (tmpPicOrderCount == 0) {
						pocCycle++;
					}
					dataFrame.gop = pocCycle;
					break;

				default:
					assert(false && "not implemented");
					break;
				}

				// Accept frame beginning NAL unit:
				dataFrame.nalRefIdc = nal.idc;
				dataFrame.nalUnitType = nal.type;
				dataFrame.size = sizeof(h264::nal_start_code) + size - 4;
				dataFrame.referencePriority = nal.idc;

				dataFrame.decodeTimeSeconds = dts * timescale_rcp;
				dataFrame.displayTimeSeconds = pts * timescale_rcp;
				dataFrame.duration = duration * timescale_rcp;
				break;
			}
			maxFrameSizeBytes = std::max(maxFrameSizeBytes, dataFrame.size);
		}

		{
			m_videoData.frameDisplayOrder.resize(m_videoData.frameInfos.size());
			std::iota(m_videoData.frameDisplayOrder.begin(), m_videoData.frameDisplayOrder.end(), 0);

			std::sort(
				m_videoData.frameDisplayOrder.begin(),
				m_videoData.frameDisplayOrder.end(),
				[&](auto& a, auto& b) {
					const auto& frameA = m_videoData.frameInfos[a];
					const auto& frameB = m_videoData.frameInfos[b];

					uint64_t keyA = (uint64_t(frameA.gop) << 32) | uint64_t(frameA.poc);
					uint64_t keyB = (uint64_t(frameB.gop) << 32) | uint64_t(frameB.poc);
					return keyA < keyB;
				});

			for (uint64_t i = 0; i < m_videoData.frameDisplayOrder.size(); ++i)
			{
				auto& f = m_videoData.frameInfos[m_videoData.frameDisplayOrder[i]];
				f.displayOrder = (int)i;
			}
		}
		m_videoData.maxMemoryFrameSizeBytes = maxFrameSizeBytes;
		m_videoData.totalDuration = trackDuration * timescale_rcp;
	}

	MP4D_close(&mp4);

	uint64_t bufferSize = align_to(m_videoData.maxMemoryFrameSizeBytes, m_properties.caps.minBitstreamBufferOffsetAlignment);
	bufferSize = align_to(bufferSize, m_properties.caps.minBitstreamBufferSizeAlignment);
	m_videoData.maxMemoryFrameSizeBytes = bufferSize;
}

void VideoPlayer::Decoder::CreateVideoSessionParameters()
{
	std::vector<StdVideoH264PictureParameterSet> videoPictureParameterSets(m_videoData.ppsCount);
	std::vector<StdVideoH264ScalingLists>        videoScalingListPPS(m_videoData.ppsCount);
	for (size_t i = 0; i != m_videoData.ppsCount; i++)
	{
		const auto pps = reinterpret_cast<const h264::PPS*>(m_videoData.ppsBytes.data()) + i;

		auto& sl = videoScalingListPPS[i];
		sl = {};
		for (int j = 0; j != std::size(pps->pic_scaling_list_present_flag); j++) {
			sl.scaling_list_present_mask |= uint16_t(pps->pic_scaling_list_present_flag[j]) << j;
		}

		{
			decltype(sl.use_default_scaling_matrix_mask) j;
			for (j = 0; j != ARRAY_SIZE(pps->UseDefaultScalingMatrix4x4Flag); j++) {
				sl.use_default_scaling_matrix_mask |=
					static_cast<decltype(j)>(pps->UseDefaultScalingMatrix4x4Flag[j]) << j;
			}
		}

		for (size_t list_idx = 0;
			list_idx < STD_VIDEO_H264_SCALING_LIST_4X4_NUM_LISTS &&
			list_idx < ARRAY_SIZE(pps->ScalingList4x4);
			list_idx++) {
			for (size_t el_idx = 0;
				el_idx < STD_VIDEO_H264_SCALING_LIST_4X4_NUM_ELEMENTS &&
				el_idx < ARRAY_SIZE(pps->ScalingList4x4[0]);
				el_idx++) {
				sl.ScalingList4x4[list_idx][el_idx] = pps->ScalingList4x4[list_idx][el_idx];
			}
		}

		for (size_t list_idx = 0;
			list_idx < STD_VIDEO_H264_SCALING_LIST_8X8_NUM_LISTS &&
			list_idx < ARRAY_SIZE(pps->ScalingList8x8);
			list_idx++) {
			for (size_t el_idx = 0;
				el_idx < STD_VIDEO_H264_SCALING_LIST_8X8_NUM_ELEMENTS &&
				el_idx < ARRAY_SIZE(pps->ScalingList8x8[0]);
				el_idx++) {
				sl.ScalingList8x8[list_idx][el_idx] = pps->ScalingList8x8[list_idx][el_idx];
			}
		}

		videoPictureParameterSets[i] = {
			.flags = {
				.transform_8x8_mode_flag = uint32_t(pps->transform_8x8_mode_flag),
				.redundant_pic_cnt_present_flag = uint32_t(pps->redundant_pic_cnt_present_flag),
				.constrained_intra_pred_flag = uint32_t(pps->constrained_intra_pred_flag),
				.deblocking_filter_control_present_flag = uint32_t(pps->deblocking_filter_control_present_flag),
				.weighted_pred_flag = uint32_t(pps->weighted_pred_flag),
				.bottom_field_pic_order_in_frame_present_flag = uint32_t(pps->pic_order_present_flag),
				.entropy_coding_mode_flag = uint32_t(pps->entropy_coding_mode_flag),
				.pic_scaling_matrix_present_flag = uint32_t(pps->pic_scaling_matrix_present_flag),
			},
			.seq_parameter_set_id = uint8_t(pps->seq_parameter_set_id),
			.pic_parameter_set_id = uint8_t(pps->pic_parameter_set_id),
			.num_ref_idx_l0_default_active_minus1 = uint8_t(pps->num_ref_idx_l0_active_minus1),
			.num_ref_idx_l1_default_active_minus1 = uint8_t(pps->num_ref_idx_l1_active_minus1),
			.weighted_bipred_idc = StdVideoH264WeightedBipredIdc(pps->weighted_bipred_idc),
			.pic_init_qp_minus26 = int8_t(pps->pic_init_qp_minus26),
			.pic_init_qs_minus26 = int8_t(pps->pic_init_qs_minus26),
			.chroma_qp_index_offset = int8_t(pps->chroma_qp_index_offset),
			.second_chroma_qp_index_offset = int8_t(pps->second_chroma_qp_index_offset),
			.pScalingLists = &videoScalingListPPS[i],
		};
	}

	std::vector<StdVideoH264SequenceParameterSet>    videoSequenceParameterSet(m_videoData.spsCount);
	std::vector<StdVideoH264SequenceParameterSetVui> videoSequenceParameterSetVui(m_videoData.spsCount);
	std::vector<StdVideoH264ScalingLists>            videoScalingListsSPS(m_videoData.spsCount);
	std::vector<StdVideoH264HrdParameters>           videoHrdParameters(m_videoData.spsCount);

	for (size_t i = 0; i != m_videoData.spsCount; i++) {

		const auto sps = (reinterpret_cast<const h264::SPS*>(m_videoData.spsBytes.data()) + i);

		auto get_chroma_format = [](int const& profile, int const& chroma) -> StdVideoH264ChromaFormatIdc {
			if (profile < STD_VIDEO_H264_PROFILE_IDC_HIGH) {
				// If profile is less than HIGH chroma format will not be explicitly given. (A.2)
				// If chroma format is not present, it shall be inferred to be equal to 1 (4:2:0) (7.4.2.1.1)
				return StdVideoH264ChromaFormatIdc::STD_VIDEO_H264_CHROMA_FORMAT_IDC_420;
			} else {
				// If Profile is greater than High, then we assume chroma to be explicitly specified.
				return StdVideoH264ChromaFormatIdc(chroma);
			}
			};

		videoSequenceParameterSet[i] = {
			.flags = {
				.constraint_set0_flag = uint32_t(sps->constraint_set0_flag),
				.constraint_set1_flag = uint32_t(sps->constraint_set1_flag),
				.constraint_set2_flag = uint32_t(sps->constraint_set2_flag),
				.constraint_set3_flag = uint32_t(sps->constraint_set3_flag),
				.constraint_set4_flag = uint32_t(sps->constraint_set4_flag),
				.constraint_set5_flag = uint32_t(sps->constraint_set5_flag),
				.direct_8x8_inference_flag = uint32_t(sps->direct_8x8_inference_flag),
				.mb_adaptive_frame_field_flag = uint32_t(sps->mb_adaptive_frame_field_flag),
				.frame_mbs_only_flag = uint32_t(sps->frame_mbs_only_flag),
				.delta_pic_order_always_zero_flag = uint32_t(sps->delta_pic_order_always_zero_flag),
				.separate_colour_plane_flag = uint32_t(sps->separate_colour_plane_flag),
				.gaps_in_frame_num_value_allowed_flag = uint32_t(sps->gaps_in_frame_num_value_allowed_flag),
				.qpprime_y_zero_transform_bypass_flag = uint32_t(sps->qpprime_y_zero_transform_bypass_flag),
				.frame_cropping_flag = uint32_t(sps->frame_cropping_flag),
				.seq_scaling_matrix_present_flag = uint32_t(sps->seq_scaling_matrix_present_flag),
				.vui_parameters_present_flag = uint32_t(sps->vui_parameters_present_flag),
			},
			.profile_idc = StdVideoH264ProfileIdc(sps->profile_idc),
			.level_idc = StdVideoH264LevelIdc(sps->level_idc),
			.chroma_format_idc = get_chroma_format(sps->profile_idc, sps->chroma_format_idc),
			.seq_parameter_set_id = uint8_t(sps->seq_parameter_set_id),
			.bit_depth_luma_minus8 = uint8_t(sps->bit_depth_luma_minus8),
			.bit_depth_chroma_minus8 = uint8_t(sps->bit_depth_chroma_minus8),
			.log2_max_frame_num_minus4 = uint8_t(sps->log2_max_frame_num_minus4),
			.pic_order_cnt_type = StdVideoH264PocType(sps->pic_order_cnt_type),
			.offset_for_non_ref_pic = int32_t(sps->offset_for_non_ref_pic),
			.offset_for_top_to_bottom_field = int32_t(sps->offset_for_top_to_bottom_field),
			.log2_max_pic_order_cnt_lsb_minus4 = uint8_t(sps->log2_max_pic_order_cnt_lsb_minus4),
			.num_ref_frames_in_pic_order_cnt_cycle = uint8_t(sps->num_ref_frames_in_pic_order_cnt_cycle),
			.max_num_ref_frames = uint8_t(sps->num_ref_frames),
			.reserved1 = 0,
			.pic_width_in_mbs_minus1 = uint32_t(sps->pic_width_in_mbs_minus1),
			.pic_height_in_map_units_minus1 = uint32_t(sps->pic_height_in_map_units_minus1),
			.frame_crop_left_offset = uint32_t(sps->frame_crop_left_offset),
			.frame_crop_right_offset = uint32_t(sps->frame_crop_right_offset),
			.frame_crop_top_offset = uint32_t(sps->frame_crop_top_offset),
			.frame_crop_bottom_offset = uint32_t(sps->frame_crop_bottom_offset),
			.reserved2 = 0,
			.pOffsetForRefFrame = nullptr, // todo:?
			.pScalingLists = &videoScalingListsSPS[i],
			.pSequenceParameterSetVui = &videoSequenceParameterSetVui[i],
		};

		// VUI stands for "Video Usablility Information"
		auto& vui = sps->vui;

		videoSequenceParameterSetVui[i] = {
			.flags = {
				.aspect_ratio_info_present_flag = uint32_t(vui.aspect_ratio_info_present_flag),
				.overscan_info_present_flag = uint32_t(vui.overscan_info_present_flag),
				.overscan_appropriate_flag = uint32_t(vui.overscan_appropriate_flag),
				.video_signal_type_present_flag = uint32_t(vui.video_signal_type_present_flag),
				.video_full_range_flag = uint32_t(vui.video_full_range_flag),
				.color_description_present_flag = uint32_t(vui.colour_description_present_flag),
				.chroma_loc_info_present_flag = uint32_t(vui.chroma_loc_info_present_flag),
				.timing_info_present_flag = uint32_t(vui.timing_info_present_flag),
				.fixed_frame_rate_flag = uint32_t(vui.fixed_frame_rate_flag),
				.bitstream_restriction_flag = uint32_t(vui.bitstream_restriction_flag),
				.nal_hrd_parameters_present_flag = uint32_t(vui.nal_hrd_parameters_present_flag),
				.vcl_hrd_parameters_present_flag = uint32_t(vui.vcl_hrd_parameters_present_flag),
			}, // StdVideoH264SpsVuiFlags
			.aspect_ratio_idc = StdVideoH264AspectRatioIdc(vui.aspect_ratio_idc),
			.sar_width = uint16_t(vui.sar_width),
			.sar_height = uint16_t(vui.sar_height),
			.video_format = uint8_t(vui.video_format),
			.colour_primaries = uint8_t(vui.colour_primaries),
			.transfer_characteristics = uint8_t(vui.transfer_characteristics),
			.matrix_coefficients = uint8_t(vui.matrix_coefficients),
			.num_units_in_tick = uint32_t(vui.num_units_in_tick),
			.time_scale = uint32_t(vui.time_scale),
			.max_num_reorder_frames = uint8_t(vui.num_reorder_frames),
			.max_dec_frame_buffering = uint8_t(vui.max_dec_frame_buffering),
			.chroma_sample_loc_type_top_field = uint8_t(vui.chroma_sample_loc_type_top_field),
			.chroma_sample_loc_type_bottom_field = uint8_t(vui.chroma_sample_loc_type_bottom_field),
			.reserved1 = 0,
			.pHrdParameters = &videoHrdParameters[i],
		};
		{
			StdVideoH264HrdParameters& vk_hrd = videoHrdParameters[i];

			auto const& hrd = sps->hrd;
			vk_hrd = {
				.cpb_cnt_minus1 = uint8_t(hrd.cpb_cnt_minus1),
				.bit_rate_scale = uint8_t(hrd.bit_rate_scale),
				.cpb_size_scale = uint8_t(hrd.cpb_size_scale),
				.reserved1 = uint8_t(),
				.bit_rate_value_minus1 = {},
				.cpb_size_value_minus1 = {},
				.cbr_flag = {},
				.initial_cpb_removal_delay_length_minus1 = uint32_t(hrd.initial_cpb_removal_delay_length_minus1),
				.cpb_removal_delay_length_minus1 = uint32_t(hrd.cpb_removal_delay_length_minus1),
				.dpb_output_delay_length_minus1 = uint32_t(hrd.dpb_output_delay_length_minus1),
				.time_offset_length = uint32_t(hrd.time_offset_length),
			};

			// Sigh, nobody said it was easy ...
			for (int j = 0; j != STD_VIDEO_H264_CPB_CNT_LIST_SIZE; j++) {
				vk_hrd.bit_rate_value_minus1[j] = hrd.bit_rate_value_minus1[j];
				vk_hrd.cpb_size_value_minus1[j] = hrd.cpb_size_value_minus1[j];
				vk_hrd.cbr_flag[j] = hrd.cbr_flag[j];
			}
		}

		{ // Now fill in the Scaling Lists
			StdVideoH264ScalingLists& sl = videoScalingListsSPS[i];
			sl = {};
			{
				decltype(sl.scaling_list_present_mask) j;
				for (j = 0; j != ARRAY_SIZE(sps->seq_scaling_list_present_flag); j++) {
					sl.scaling_list_present_mask |=
						static_cast<decltype(j)>(sps->seq_scaling_list_present_flag[j]) << j;
				}
			}
			{
				decltype(sl.use_default_scaling_matrix_mask) j;
				for (j = 0; j != ARRAY_SIZE(sps->UseDefaultScalingMatrix4x4Flag); j++) {
					sl.use_default_scaling_matrix_mask |=
						static_cast<decltype(j)>(sps->UseDefaultScalingMatrix4x4Flag[j]) << j;
				}
			}

			for (size_t list_idx = 0;
				list_idx < STD_VIDEO_H264_SCALING_LIST_4X4_NUM_LISTS &&
				list_idx < ARRAY_SIZE(sps->ScalingList4x4);
				list_idx++) {
				for (size_t el_idx = 0;
					el_idx < STD_VIDEO_H264_SCALING_LIST_4X4_NUM_ELEMENTS &&
					el_idx < ARRAY_SIZE(sps->ScalingList4x4[0]);
					el_idx++) {
					sl.ScalingList4x4[list_idx][el_idx] = sps->ScalingList4x4[list_idx][el_idx];
				}
			}

			for (size_t list_idx = 0;
				list_idx < STD_VIDEO_H264_SCALING_LIST_8X8_NUM_LISTS &&
				list_idx < ARRAY_SIZE(sps->ScalingList8x8);
				list_idx++) {
				for (size_t el_idx = 0;
					el_idx < STD_VIDEO_H264_SCALING_LIST_8X8_NUM_ELEMENTS &&
					el_idx < ARRAY_SIZE(sps->ScalingList8x8[0]);
					el_idx++) {
					sl.ScalingList8x8[list_idx][el_idx] = sps->ScalingList8x8[list_idx][el_idx];
				}
			}
		}
	}

	VkVideoDecodeH264SessionParametersAddInfoKHR sessionParametersAddInfo = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR,
		.stdSPSCount = m_videoData.spsCount,
		.pStdSPSs = videoSequenceParameterSet.data(),
		.stdPPSCount = m_videoData.ppsCount,
		.pStdPPSs = videoPictureParameterSets.data(),
	};
	VkVideoDecodeH264SessionParametersCreateInfoKHR videoDecodeSessionParamersCI = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR,
		.maxStdSPSCount = m_videoData.spsCount,
		.maxStdPPSCount = m_videoData.ppsCount,
		.pParametersAddInfo = &sessionParametersAddInfo,
	};
	VkVideoSessionParametersCreateInfoKHR videoSessionParametersCI = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR,
		.pNext = &videoDecodeSessionParamersCI,
		.flags = 0,
		.videoSessionParametersTemplate = nullptr,
		.videoSession = m_videoSession,
	};

	auto devCtx = DeviceContext::GetContext();
	VkResult res = vkCreateVideoSessionParametersKHR(
		devCtx->GetVkDevice(),
		&videoSessionParametersCI,
		nullptr, &m_videoSessionParameters);
	assert(res == VK_SUCCESS);
}

void VideoPlayer::Decoder::PrepareDecodedPictureBuffer()
{
	// Allocate an image array to store decoded pictures in  -
	// we need an array with (num_reference_frames) + 1 elements.
	// the +1 is for the frame that is currently being decoded.
	//
	// we know there will be at max 17 images (16+1) as 16 is the max by the standard.
	VmaAllocationCreateInfo allocationCI{
		.flags = { },
		.usage = VMA_MEMORY_USAGE_GPU_ONLY,
		.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
	};

	m_info.imagesDPB.resize(m_videoData.maxReferencePictures);

	VkImageCreateInfo imageCI = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = &m_settings.profileListInfo,
		.flags = 0,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = m_properties.formatProps.format,
		.extent = {
			.width =  m_videoData.width,
			.height = m_videoData.height,
			.depth = 1
		},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = m_properties.usageDPB,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = 0,
		.pQueueFamilyIndices = nullptr,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkImageViewCreateInfo imageViewCI = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.flags = 0,
		.image = VK_NULL_HANDLE,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = imageCI.format,
		.components = {},
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
	};
	auto devCtx = DeviceContext::GetContext();
	int dpbIndex = 0;
	for (auto& dpb : m_info.imagesDPB)
	{
		auto res = vmaCreateImage(
			devCtx->GetVmaAllocator(),
			&imageCI,
			&allocationCI,
			&dpb.image,
			&dpb.allocation,
			&dpb.allocationInfo
		);
		assert(res == VK_SUCCESS);
		imageViewCI.image = dpb.image;
		res = vkCreateImageView(
			devCtx->GetVkDevice(),
			&imageViewCI, nullptr, &dpb.view);
		assert(res == VK_SUCCESS);

		std::string name;
		name = "myDPBImage";
		name += std::to_string(dpbIndex);
		VkDebugUtilsObjectNameInfoEXT nameInfo{
			.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
			.objectType = VK_OBJECT_TYPE_IMAGE,
			.objectHandle = (uint64_t)(void*)dpb.image,
			.pObjectName = name.c_str(),
		};
		vkSetDebugUtilsObjectNameEXT(devCtx->GetVkDevice(), &nameInfo);
		dpbIndex++;
	}
}

void VideoPlayer::Decoder::WriteVideoFrame(VideoMemoryFrameInfo& memoryFrame)
{
	const auto& dataFrame = m_videoData.frameInfos[memoryFrame.decodingFrameIndex];
	auto dstBuffer = memoryFrame.gpuBitstreamSliceMappedMemoryAddress + memoryFrame.gpuBitstreamSize;
	int64_t frameBytes = dataFrame.frameBytes;

	m_videoData.inputStream.seekg(dataFrame.srcOffset, std::ios::beg);
	while (frameBytes > 0)
	{
		uint8_t srcBuffer[4];
		m_videoData.inputStream.read(reinterpret_cast<char*>(srcBuffer), sizeof(srcBuffer));
		uint32_t size = ((uint32_t)(srcBuffer[0]) << 24) | ((uint32_t)(srcBuffer[1]) << 16) | ((uint32_t)(srcBuffer[2]) << 8) | srcBuffer[3];
		size += 4;
		assert(frameBytes >= size);

		uint8_t nalHeaderByte = m_videoData.inputStream.peek();

		h264::Bitstream bs = {};
		bs.init(&nalHeaderByte, sizeof(nalHeaderByte));
		h264::NALHeader nal = {};
		h264::read_nal_header(&nal, &bs);

		// Skip over any frame data that is not idr slice or non-idr slice
		if (nal.type != h264::NAL_UNIT_TYPE_CODED_SLICE_IDR &&
			nal.type != h264::NAL_UNIT_TYPE_CODED_SLICE_NON_IDR) {
			frameBytes -= size;
			m_videoData.inputStream.seekg(size - 4, std::ios_base::cur);
			continue;
		}

		if (memoryFrame.gpuBitstreamSize + size <= memoryFrame.gpuBitstreamCapacity)
		{
			memcpy(dstBuffer, h264::nal_start_code, sizeof(h264::nal_start_code));
			m_videoData.inputStream.read((char*)(dstBuffer + sizeof(h264::nal_start_code)), size - 4);
			memoryFrame.gpuBitstreamSize += size;
		} else {
			//logger.error("Cannot copy frame data into frame bitstream - out of memory. Frame capacity: %d, frame current size %d, extra size: %d",
			//	memory_frame->gpu_bitstream_capacity, memory_frame->gpu_bitstream_size, size);
		}
		break;
	}
	memoryFrame.gpuBitstreamSize = align_to(memoryFrame.gpuBitstreamSize, m_properties.caps.minBitstreamBufferSizeAlignment);
}

const void* VideoPlayer::Decoder::GetSliceHeader() const
{
	return m_videoData.sliceHeaderBytes.data();
}
const void* VideoPlayer::Decoder::GetPPS() const
{
	return m_videoData.ppsBytes.data();
}
const void* VideoPlayer::Decoder::GetSPS()const
{
	return m_videoData.spsBytes.data();
}
