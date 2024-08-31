#pragma once

#include <fstream>
#include <deque>

namespace vku
{
	struct GPUBuffer {
		VkBuffer buffer = VK_NULL_HANDLE;
		VmaAllocation allocation;
		VmaAllocationInfo allocationInfo;
	};
	struct GPUImage {
		VkImage						image = VK_NULL_HANDLE;
		VkImageView				view = VK_NULL_HANDLE;
		VmaAllocation			allocation;
		VmaAllocationInfo	allocationInfo;
	};
}

class VideoPlayer
{
public:
	bool Initialize(const char* filePath);
	void Shutdown();

	// 再生のカウンタを進めるなど、コマンド積み込みが不要な処理を実行.
	void Update(VkCommandBuffer graphicsCmdBuffer, double timestamp);

	// デコード処理をコマンドに積む.
	void UpdateDecode(VkCommandBuffer command, std::vector<VkImageMemoryBarrier2>& requestBarrierOnGfx);

	class Decoder {
	public:
		struct DecoderQueryProperties
		{
			VkVideoDecodeH264CapabilitiesKHR decodeH264Caps;
			VkVideoDecodeCapabilitiesKHR     decodeCaps;
			VkVideoCapabilitiesKHR           caps;
			VkVideoFormatPropertiesKHR       formatProps = {};
			VkImageUsageFlags                usageDPB;
		} m_properties;

		struct Settings
		{
			VkVideoDecodeH264ProfileInfoKHR decodeH264ProfileInfo;
			VkVideoProfileInfoKHR           profileInfo;
			VkVideoProfileListInfoKHR       profileListInfo;
		} m_settings;

		enum class FrameType : uint8_t
		{
			eUnknown = 0,
			eIntra,
			ePredictive,
		};
		struct VideoDataFrameInfo
		{
			uint64_t srcOffset;
			uint64_t frameBytes;
			uint64_t size;

			int poc;
			int bottomFieldOrderCnt;
			int topFieldOrderCnt;
			int gop;
			int displayOrder;
			double decodeTimeSeconds;
			double displayTimeSeconds;
			double duration;

			uint8_t nalUnitType;
			FrameType frameType;
			uint32_t nalRefIdc;
			uint32_t  referencePriority = 0;
		};
		struct VideoFilePropertis
		{
			std::ifstream inputStream;
			uint32_t widthPadd;
			uint32_t heightPadd;
			uint32_t width;
			uint32_t height;
			uint32_t spsCount;
			uint32_t ppsCount;
			uint32_t sliceHeaderCount;

			std::vector<VideoDataFrameInfo> frameInfos;
			uint64_t maxMemoryFrameSizeBytes;
			uint32_t numDPBslots;
			uint32_t maxReferencePictures;

			std::vector<uint8_t> spsBytes;
			std::vector<uint8_t> ppsBytes;
			std::vector<uint8_t> sliceHeaderBytes;
			std::vector<uint64_t> frameDisplayOrder;

			double totalDuration;
		} m_videoData;

		struct VideoMemoryFrameInfo
		{
			const VideoDataFrameInfo* dataFrameInfo;
			uint64_t gpuBitstreamOffset = 0;
			uint64_t gpuBitstreamCapacity = 0;
			uint64_t gpuBitstreamSize = 0;
			uint8_t* gpuBitstreamSliceMappedMemoryAddress;
			int decodingFrameIndex = -1;
		};

		struct DpbImage {
			VkImage image;
			VkImageView view;
			VmaAllocation allocation;
			VmaAllocationInfo allocationInfo;
		};
		struct DpbState {
			int32_t slotindex;
			uint16_t	frameNum;
			StdVideoDecodeH264ReferenceInfo referenceInfo;
		};

		struct VideoDecodeOperation
		{
			enum Flags {
				eNone = 0,
				eSessionReset = 1 << 0,
			};
			enum class FrameType {
				eIntra = 0,
				ePredictive,
			};
			uint32_t flags;
			uint64_t streamOffset = 0;
			uint64_t streamSize = 0;
			FrameType frameType = FrameType::eIntra;
			uint32_t referencePriority = 0;
			int decodedFrameIndex = 0;
			const void* slideHeader = nullptr;
			const void* pps = nullptr;
			const void* sps = nullptr;
			int poc[2] = { 0 };
			uint32_t current_dpb = 0;
			uint32_t dpbReferenceCount = 0;
			const uint8_t* dpbReferenceSlots = nullptr;
			const int* dpbPoc = nullptr;
			const int* dpbFramenum = nullptr;

			uint32_t dpbSlotNum = 0;
			VkImage* pDPBs;
			VkImageView* pDPBviews;
		};

		struct DecoderInfo
		{
			std::vector<VideoMemoryFrameInfo> memoryFrames;
			std::vector<DpbImage> imagesDPB;
			std::deque<DpbState>  dpbState;
			uint32_t dpbTargetSlotIndex = 0;

			uint64_t currentDecodeFrameIndex = 0;
			int processingMemoryFrameIndex = -1;
			int decodeFrameIndex = -1;

			bool controlResetIssued = false;
		} m_info;

		void Initialize(const char* filePath);
		void WriteVideoFrame(VideoMemoryFrameInfo& memoryFrame);

		VkVideoSessionKHR GetVideoSession() {
			return m_videoSession;
		}

		const void* GetSliceHeader() const;
		const void* GetPPS()const;
		const void* GetSPS()const;
	private:
		void ParseMp4Data(const char* filePath);
		void CreateVideoSessionParameters();
		void PrepareDecodedPictureBuffer();
	public:
		VkVideoSessionKHR m_videoSession = VK_NULL_HANDLE;
		VkVideoSessionParametersKHR m_videoSessionParameters = VK_NULL_HANDLE;
		vku::GPUBuffer m_gpuBitstreamBuffer;
		std::vector<VmaAllocation> m_sessionMemoryAllocations;

	};

	struct Image {
		VkImage image;
		VkImageView view;
		VmaAllocation allocation;
		VmaAllocationInfo allocationInfo;
	};
	struct OutputImage
	{
		int display_order = -1;
		Image texture;
		uint32_t flags = 0;
		enum Flags {
			eInit = 1,
		};
		double duration = 0;
	};

	struct CommandBufferInfo
	{
		VkCommandBuffer videoCommandBuffer;
		VkCommandBuffer graphicsCommandBuffer;
		VkSemaphore     semVideoToGfx;
	};
	VkCommandPool m_gfxCommandPool;
	VkCommandPool m_videoCommandPool;
	VkEvent m_evtVideoPlayer;
	std::vector<CommandBufferInfo> m_commandBuffersInfo;

	int GetDecodeFrameNumber() const;
	int GetDisplayFrameNumber() const;
	int GetLastVideoFrameNumber()  const;
	const Decoder::VideoFilePropertis& GetVideoProperties() const;
	const Decoder::VideoDecodeOperation& GetDecodeOperation()const { return m_decodeOpration; }
	std::vector<int> GetDPBSlotUsed() const { return m_DPBSlotUsed; }

private:
	struct DPB
	{
		enum {
			SlotCount = 17,
		};
		Image image[SlotCount];
		struct ResourceState {
			VkAccessFlags2 flag;
			VkImageLayout  layout;
		} resourceState[SlotCount];

		int pocStatus[SlotCount] = { 0 };
		int framenumStatus[SlotCount] = { 0 };
		std::vector<uint8_t> referenceUsage;
		uint8_t nextRef = 0;
		uint8_t nextSlot = 0;
		uint8_t currentSlot = 0;
	} m_dpb;

	enum Flags : uint32_t{
		eNone = 0,
		ePlaying = 1 << 1,
		eInitiallFirstFrameDecoded = 1 << 2,
		eDecoderReset = 1 << 3,
		eNeedResolve = 1 << 4,
	};
	uint32_t m_flags = Flags::eNone;
  std::shared_ptr<Decoder> m_decoder;

	int m_current_frame = 0;

	vku::GPUBuffer m_movieBitStreamBuffer;
	struct DecodeStreamFrame {
		uint64_t gpuBitstreamCapacity;
		uint64_t gpuBitstreamOffset;
		uint64_t gpuBitstreamSize;
		uint8_t* gpuBitstreamSliceMappedMemoryAddress;
	} m_videoFrames[18];

	void VideoDecodeCore(std::shared_ptr<Decoder> decoder, const Decoder::VideoDecodeOperation* operation, VkCommandBuffer commandBuffer);
	void WriteVideoFrame(DecodeStreamFrame* frame);

	Image CreateVideoTexture();

	public:
	std::vector<OutputImage> m_outputTexturesFree;
	std::vector<OutputImage> m_outputTexturesUsed;

	// 再生用のテクスチャが準備できているか.
	bool IsReady();
	// 現在の再生位置が示すテクスチャを取得.
	const OutputImage& GetVideoTexture();

	enum {
		MAX_TEXTURE_COUNT = 64
	};


	void UpdateDisplayFrame(double elapsed);
	void UpdateDecodeVideo();

	void VideoDecodePreBarrier(VkCommandBuffer videoCmdBuffer);
	void VideoDecodePostBarrier(VkCommandBuffer videoCmdBuffer);
	void CopyToTexture(VkCommandBuffer videoCmdBuffer, OutputImage& dstImage);

	struct VideoCursorInfo
	{
		int32_t playIndex;	// 再生中のフレーム番号を指す.
		int32_t frameIndex;	// デコード済み配列内インデックスを指す.
	} m_video_cursor;
	bool m_isPrepared = false;
	bool m_isStopped = false;

	
	Decoder::VideoDecodeOperation m_decodeOpration;	// 情報表示用.
	std::vector<int> m_DPBSlotUsed;
};
