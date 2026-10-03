#include "SceneDepth.h"

#include "Config.h"
#include "Game.h"

namespace cybercraft::SceneDepth
{
	namespace
	{
		using Microsoft::WRL::ComPtr;

		// The parts of Streamline 2.7's public headers (sl_struct.h, sl_consts.h, sl_core_types.h;
		// MIT licensed) this needs, member for member.
		namespace sl
		{
			struct StructType
			{
				std::uint32_t data1;
				std::uint16_t data2, data3;
				std::uint8_t  data4[8];
			};
			struct BaseStructure
			{
				BaseStructure* next;
				StructType     structType;
				std::size_t    structVersion;
			};
			enum class ResourceType : char
			{
				eTex2d,
				eBuffer,
			};
			struct Resource : BaseStructure
			{
				ResourceType  type;
				void*         native;
				void*         memory;
				void*         view;
				std::uint32_t state;
				std::uint32_t width, height, nativeFormat, mipLevels, arrayLayers;
				std::uint64_t gpuVirtualAddress;
				std::uint32_t flags, usage, reserved;
			};
			struct Extent
			{
				std::uint32_t top, left, width, height;
			};
			enum ResourceLifecycle : std::int32_t
			{
				eOnlyValidNow,
				eValidUntilPresent,
				eValidUntilEvaluate,
			};
			struct ResourceTag : BaseStructure
			{
				Resource*         resource;
				std::uint32_t     type;  // BufferType: 0 = depth
				ResourceLifecycle lifecycle;
				Extent            extent;
			};
			struct float2
			{
				float x, y;
			};
			struct float3
			{
				float x, y, z;
			};
			struct float4x4
			{
				float m[4][4];
			};
			enum Boolean : char
			{
				eFalse,
				eTrue,
				eInvalid,
			};
			struct Constants : BaseStructure
			{
				float4x4 cameraViewToClip, clipToCameraView, clipToLensClip, clipToPrevClip, prevClipToClip;
				float2   jitterOffset, mvecScale, cameraPinholeOffset;
				float3   cameraPos, cameraUp, cameraRight, cameraFwd;
				float    cameraNear, cameraFar, cameraFOV, cameraAspectRatio, motionVectorsInvalidValue;
				Boolean  depthInverted;
			};
			constexpr StructType  kResourceTagType{ 0x4c6a5aad, 0xb445, 0x496c, { 0x87, 0xff, 0x1a, 0xf3, 0x84, 0x5b, 0xe6, 0x53 } };
			constexpr std::uint32_t kBufferTypeDepth = 0;
			constexpr std::uint32_t kBufferTypeHUDLessColor = 2;
			constexpr std::uint32_t kBufferTypeUIColorAndAlpha = 23;
			constexpr float       kInvalidFloat = 3.40282346638528859811704183484516925440e38f;
		}

		using SetTagFn = std::int32_t (*)(const void* a_viewport, const sl::ResourceTag* a_tags, std::uint32_t a_count, void* a_commandBuffer);
		using SetConstantsFn = std::int32_t (*)(const sl::Constants& a_values, const void* a_frame, const void* a_viewport);
		using EvaluateFn = std::int32_t (*)(std::uint32_t a_feature, const void* a_frame, const sl::BaseStructure** a_inputs, std::uint32_t a_count,
			void* a_commandBuffer);

		RED4ext::v1::PluginHandle pluginHandle = nullptr;
		const RED4ext::v1::Sdk*   sdk = nullptr;
		void*                     setTagTarget = nullptr;
		void*                     setConstantsTarget = nullptr;
		void*                     evaluateTarget = nullptr;
		SetTagFn                  originalSetTag = nullptr;
		SetConstantsFn            originalSetConstants = nullptr;
		EvaluateFn                originalEvaluate = nullptr;

		std::mutex gMutex;

		// The depth tag waiting for a command list to copy it with.
		struct Pending
		{
			ID3D12Resource* resource{ nullptr };
			std::uint32_t   state{ UINT_MAX };
			sl::Extent      extent{};
			bool            valid{ false };
		};
		Pending pending;

		struct Copy
		{
			ComPtr<ID3D12Resource> texture;
			DXGI_FORMAT            sourceFormat{ DXGI_FORMAT_UNKNOWN };
			DXGI_FORMAT            srvFormat{ DXGI_FORMAT_UNKNOWN };
			UINT                   width{ 0 }, height{ 0 };
			sl::Extent             extent{};
			std::chrono::steady_clock::time_point at{};
		};
		Copy copy;
		// Copies replaced on a size change stay alive a while: frames in flight may still read them.
		std::deque<std::pair<std::chrono::steady_clock::time_point, ComPtr<ID3D12Resource>>> retired;

		float cameraNear = 0.0f;
		float cameraFar = 0.0f;
		bool  inverted = false;
		bool  loggedFirst = false;
		bool  sceneDepth = true;  // [World] bSceneDepth
		bool  hudMask = true;     // [World] bHudMask

		// A colour buffer the game hands frame generation (the picture without its HUD, or the HUD),
		// copied into a texture of ours: where it's tagged when only valid there, else at Present.
		struct Layer
		{
			const char*            name;
			ComPtr<ID3D12Resource> source;  // tagged this frame, waiting for Present to be copied
			std::uint32_t          state{ UINT_MAX };
			sl::Extent             extent{};
			ComPtr<ID3D12Resource> copy;
			DXGI_FORMAT            sourceFormat{ DXGI_FORMAT_UNKNOWN };
			DXGI_FORMAT            srvFormat{ DXGI_FORMAT_UNKNOWN };
			UINT                   width{ 0 }, height{ 0 };
			bool                   copied{ false };  // this frame's is in copy
			bool                   warned{ false };
		};
		Layer hudless{ "HUD-less picture" };
		Layer ui{ "HUD" };

		// Buffer types the game has tagged so far, each logged once: what frame generation and DLSS
		// are handed tells what else could be borrowed.
		std::array<bool, 64> seenTypes{};

		// The typeless format a copy of this depth format needs to be readable, and what to read it as.
		bool Formats(DXGI_FORMAT a_source, DXGI_FORMAT& a_typeless, DXGI_FORMAT& a_srv)
		{
			switch (a_source) {
			case DXGI_FORMAT_D32_FLOAT:
			case DXGI_FORMAT_R32_TYPELESS:
			case DXGI_FORMAT_R32_FLOAT:
				a_typeless = DXGI_FORMAT_R32_TYPELESS;
				a_srv = DXGI_FORMAT_R32_FLOAT;
				return true;
			case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
			case DXGI_FORMAT_R32G8X24_TYPELESS:
				a_typeless = DXGI_FORMAT_R32G8X24_TYPELESS;
				a_srv = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
				return true;
			case DXGI_FORMAT_D24_UNORM_S8_UINT:
			case DXGI_FORMAT_R24G8_TYPELESS:
				a_typeless = DXGI_FORMAT_R24G8_TYPELESS;
				a_srv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
				return true;
			case DXGI_FORMAT_D16_UNORM:
			case DXGI_FORMAT_R16_TYPELESS:
				a_typeless = DXGI_FORMAT_R16_TYPELESS;
				a_srv = DXGI_FORMAT_R16_UNORM;
				return true;
			default:
				return false;
			}
		}

		// The same for a colour buffer: copied as its typeless family, read without sRGB decoding (as
		// the back buffer is, which a flip-model swap chain can't have in an sRGB format).
		bool ColorFormats(DXGI_FORMAT a_source, DXGI_FORMAT& a_typeless, DXGI_FORMAT& a_srv)
		{
			switch (a_source) {
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				a_typeless = DXGI_FORMAT_R8G8B8A8_TYPELESS;
				a_srv = DXGI_FORMAT_R8G8B8A8_UNORM;
				return true;
			case DXGI_FORMAT_B8G8R8A8_TYPELESS:
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
				a_typeless = DXGI_FORMAT_B8G8R8A8_TYPELESS;
				a_srv = DXGI_FORMAT_B8G8R8A8_UNORM;
				return true;
			case DXGI_FORMAT_R10G10B10A2_TYPELESS:
			case DXGI_FORMAT_R10G10B10A2_UNORM:
				a_typeless = DXGI_FORMAT_R10G10B10A2_TYPELESS;
				a_srv = DXGI_FORMAT_R10G10B10A2_UNORM;
				return true;
			case DXGI_FORMAT_R16G16B16A16_TYPELESS:
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
				a_typeless = DXGI_FORMAT_R16G16B16A16_TYPELESS;
				a_srv = DXGI_FORMAT_R16G16B16A16_FLOAT;
				return true;
			case DXGI_FORMAT_R11G11B10_FLOAT:
				a_typeless = a_srv = a_source;
				return true;
			default:
				return false;
			}
		}

		const char* LifecycleName(sl::ResourceLifecycle a_lifecycle)
		{
			switch (a_lifecycle) {
			case sl::eOnlyValidNow:
				return "only valid where tagged";
			case sl::eValidUntilPresent:
				return "valid until Present";
			case sl::eValidUntilEvaluate:
				return "valid until evaluated";
			default:
				return "unknown lifetime";
			}
		}

		void Barrier(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_resource, D3D12_RESOURCE_STATES a_from, D3D12_RESOURCE_STATES a_to)
		{
			if (a_from == a_to) {
				return;
			}
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = a_resource;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = a_from;
			barrier.Transition.StateAfter = a_to;
			a_list->ResourceBarrier(1, &barrier);
		}

		// A texture of ours shaped like a_source, in a_format (its typeless family, so a view of any
		// format in it reads it), left PIXEL_SHADER_RESOURCE between copies.
		ComPtr<ID3D12Resource> CreateCopy(ID3D12Resource* a_source, DXGI_FORMAT a_format)
		{
			ComPtr<ID3D12Device>   device;
			ComPtr<ID3D12Resource> texture;
			if (FAILED(a_source->GetDevice(IID_PPV_ARGS(&device)))) {
				return texture;
			}
			auto desc = a_source->GetDesc();
			desc.Format = a_format;
			desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
					IID_PPV_ARGS(&texture)))) {
				texture.Reset();
			}
			return texture;
		}

		// Caller holds gMutex.
		void PruneRetired(std::chrono::steady_clock::time_point a_now)
		{
			while (!retired.empty() && a_now - retired.front().first > std::chrono::seconds(2)) {
				retired.pop_front();
			}
		}

		// a_source, in a_state, into a_copy; both are left as they were.
		void RecordCopyInto(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_copy, ID3D12Resource* a_source, D3D12_RESOURCE_STATES a_state)
		{
			Barrier(a_list, a_source, a_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
			Barrier(a_list, a_copy, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
			a_list->CopyResource(a_copy, a_source);
			Barrier(a_list, a_copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			Barrier(a_list, a_source, D3D12_RESOURCE_STATE_COPY_SOURCE, a_state);
		}

		// Records the copy into the game's command list, right where DLSS reads the same depth.
		// Caller holds gMutex.
		void RecordCopy(void* a_commandBuffer)
		{
			if (!pending.valid || !a_commandBuffer) {
				return;
			}
			pending.valid = false;
			// Only while blocks are drawn: Minecraft drives V or V is in a car (not with CyberCraft off).
			if (!State().drawBlocks.load()) {
				return;
			}
			auto* source = pending.resource;
			if (!source || pending.state == UINT_MAX) {
				return;  // no state to transition from: not worth a removed device
			}
			ComPtr<ID3D12GraphicsCommandList> list;
			if (FAILED(static_cast<IUnknown*>(a_commandBuffer)->QueryInterface(IID_PPV_ARGS(&list)))) {
				return;
			}
			const auto desc = source->GetDesc();
			DXGI_FORMAT typeless{}, srv{};
			if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1 ||
				!Formats(desc.Format, typeless, srv)) {
				if (!loggedFirst) {
					loggedFirst = true;
					logger::warn("depth: tagged depth is format {} {}x{} x{} samples, not copied", int(desc.Format), desc.Width, desc.Height,
						desc.SampleDesc.Count);
				}
				return;
			}

			if (!copy.texture || copy.width != UINT(desc.Width) || copy.height != desc.Height || copy.sourceFormat != desc.Format) {
				if (copy.texture) {
					retired.emplace_back(std::chrono::steady_clock::now(), std::move(copy.texture));
				}
				copy.texture = CreateCopy(source, typeless);
				if (!copy.texture) {
					logger::warn("depth: couldn't create a {}x{} copy", desc.Width, desc.Height);
					return;
				}
				copy.width = UINT(desc.Width);
				copy.height = desc.Height;
				copy.sourceFormat = desc.Format;
				copy.srvFormat = srv;
				logger::info("depth: copying the game's depth, {}x{} format {} (state {:#x})", desc.Width, desc.Height, int(desc.Format), pending.state);
			}

			RecordCopyInto(list.Get(), copy.texture.Get(), source, static_cast<D3D12_RESOURCE_STATES>(pending.state));
			copy.extent = pending.extent;
			copy.at = std::chrono::steady_clock::now();
		}

		// Records a_layer's source into its copy, made or remade to fit. Caller holds gMutex.
		bool CopyLayer(Layer& a_layer, ID3D12GraphicsCommandList* a_list)
		{
			auto* source = a_layer.source.Get();
			if (!source || a_layer.state == UINT_MAX) {
				return false;  // no state to transition from: not worth a removed device
			}
			const auto  desc = source->GetDesc();
			DXGI_FORMAT typeless{}, srv{};
			if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1 ||
				!ColorFormats(desc.Format, typeless, srv)) {
				if (!a_layer.warned) {
					a_layer.warned = true;
					logger::warn("hud: the game's {} is format {} {}x{} x{} samples, not used", a_layer.name, int(desc.Format), desc.Width, desc.Height,
						desc.SampleDesc.Count);
				}
				return false;
			}
			if (!a_layer.copy || a_layer.width != UINT(desc.Width) || a_layer.height != desc.Height || a_layer.sourceFormat != desc.Format) {
				if (a_layer.copy) {
					retired.emplace_back(std::chrono::steady_clock::now(), std::move(a_layer.copy));
				}
				a_layer.copy = CreateCopy(source, typeless);
				if (!a_layer.copy) {
					if (!a_layer.warned) {
						a_layer.warned = true;
						logger::warn("hud: couldn't create a {}x{} copy of the game's {}", desc.Width, desc.Height, a_layer.name);
					}
					return false;
				}
				a_layer.width = UINT(desc.Width);
				a_layer.height = desc.Height;
				a_layer.sourceFormat = desc.Format;
				a_layer.srvFormat = srv;
				logger::info("hud: copying the game's {}, {}x{} format {} (state {:#x})", a_layer.name, desc.Width, desc.Height, int(desc.Format), a_layer.state);
			}
			RecordCopyInto(a_list, a_layer.copy.Get(), source, static_cast<D3D12_RESOURCE_STATES>(a_layer.state));
			return true;
		}

		// Caller holds gMutex.
		void NoteLayer(Layer& a_layer, const sl::ResourceTag& a_tag, void* a_commandBuffer)
		{
			a_layer.source = static_cast<ID3D12Resource*>(a_tag.resource->native);
			a_layer.state = a_tag.resource->state;
			a_layer.extent = a_tag.extent;
			a_layer.copied = false;
			if (a_tag.lifecycle != sl::eOnlyValidNow) {
				return;  // copied at Present (AcquireHud), once the game has finished it
			}
			// Only valid in this command list: copied now, and only while blocks are drawn.
			ComPtr<ID3D12GraphicsCommandList> list;
			if (State().drawBlocks.load() && a_commandBuffer &&
				SUCCEEDED(static_cast<IUnknown*>(a_commandBuffer)->QueryInterface(IID_PPV_ARGS(&list)))) {
				a_layer.copied = CopyLayer(a_layer, list.Get());
			}
			a_layer.source.Reset();
		}

		const char* TypeName(std::uint32_t a_type)
		{
			switch (a_type) {
			case sl::kBufferTypeDepth:
				return " (depth)";
			case 1:
				return " (motion vectors)";
			case sl::kBufferTypeHUDLessColor:
				return " (HUD-less colour)";
			case 3:
				return " (upscaler input)";
			case 4:
				return " (upscaler output)";
			case 13:
				return " (exposure)";
			case sl::kBufferTypeUIColorAndAlpha:
				return " (HUD colour and alpha)";
			default:
				return "";
			}
		}

		void NoteTag(const sl::ResourceTag& a_tag, void* a_commandBuffer)
		{
			if (!a_tag.resource || a_tag.resource->type != sl::ResourceType::eTex2d || !a_tag.resource->native) {
				return;
			}
			if (a_tag.type < seenTypes.size() && !seenTypes[a_tag.type]) {
				seenTypes[a_tag.type] = true;
				const auto desc = static_cast<ID3D12Resource*>(a_tag.resource->native)->GetDesc();
				logger::info("streamline: the game tags buffer {}{}, {}x{} format {}, state {:#x}, {}", a_tag.type, TypeName(a_tag.type), desc.Width,
					desc.Height, int(desc.Format), a_tag.resource->state, LifecycleName(a_tag.lifecycle));
			}
			if (a_tag.type == sl::kBufferTypeHUDLessColor || a_tag.type == sl::kBufferTypeUIColorAndAlpha) {
				if (hudMask) {
					NoteLayer(a_tag.type == sl::kBufferTypeHUDLessColor ? hudless : ui, a_tag, a_commandBuffer);
				}
				return;
			}
			if (a_tag.type != sl::kBufferTypeDepth || !sceneDepth) {
				return;
			}
			pending.resource = static_cast<ID3D12Resource*>(a_tag.resource->native);
			pending.state = a_tag.resource->state;
			pending.extent = a_tag.extent;
			pending.valid = true;
			// Volatile depth must be copied now, while the command list is the one it's valid in.
			if (a_tag.lifecycle == sl::eOnlyValidNow) {
				RecordCopy(a_commandBuffer);
			}
		}

		std::int32_t SetTagHook(const void* a_viewport, const sl::ResourceTag* a_tags, std::uint32_t a_count, void* a_commandBuffer)
		{
			{
				std::lock_guard lock(gMutex);
				for (std::uint32_t i = 0; a_tags && i < a_count; ++i) {
					NoteTag(a_tags[i], a_commandBuffer);
				}
			}
			return originalSetTag(a_viewport, a_tags, a_count, a_commandBuffer);
		}

		std::int32_t SetConstantsHook(const sl::Constants& a_values, const void* a_frame, const void* a_viewport)
		{
			{
				std::lock_guard lock(gMutex);
				if (a_values.cameraNear != sl::kInvalidFloat && a_values.cameraNear > 0.0f) {
					cameraNear = a_values.cameraNear;
					cameraFar = a_values.cameraFar;
					inverted = a_values.depthInverted == sl::eTrue;
				}
			}
			return originalSetConstants(a_values, a_frame, a_viewport);
		}

		std::int32_t EvaluateHook(std::uint32_t a_feature, const void* a_frame, const sl::BaseStructure** a_inputs, std::uint32_t a_count,
			void* a_commandBuffer)
		{
			{
				std::lock_guard lock(gMutex);
				// Tags can also come with the evaluate call itself.
				for (std::uint32_t i = 0; a_inputs && i < a_count; ++i) {
					const auto* input = a_inputs[i];
					if (input && std::memcmp(&input->structType, &sl::kResourceTagType, sizeof(sl::StructType)) == 0) {
						NoteTag(*reinterpret_cast<const sl::ResourceTag*>(input), a_commandBuffer);
					}
				}
				RecordCopy(a_commandBuffer);
			}
			return originalEvaluate(a_feature, a_frame, a_inputs, a_count, a_commandBuffer);
		}

		template <class Fn>
		bool Attach(HMODULE a_module, const char* a_name, void* a_detour, void*& a_target, Fn& a_original)
		{
			a_target = reinterpret_cast<void*>(::GetProcAddress(a_module, a_name));
			void* trampoline = nullptr;
			if (!a_target || !sdk->hooking->Attach(pluginHandle, a_target, a_detour, &trampoline) || !trampoline) {
				a_target = nullptr;
				return false;
			}
			a_original = reinterpret_cast<Fn>(trampoline);
			return true;
		}
	}

	void Install(RED4ext::v1::PluginHandle a_handle, const RED4ext::v1::Sdk* a_sdk)
	{
		pluginHandle = a_handle;
		sdk = a_sdk;
		sceneDepth = Config::GetBool(L"World", L"bSceneDepth", true);
		hudMask = Config::GetBool(L"World", L"bHudMask", true);
		if (!sceneDepth) {
			logger::info("depth: bSceneDepth = 0; blocks are hidden face by face");
		}
		if (!hudMask) {
			logger::info("hud: bHudMask = 0; blocks are drawn over the game's HUD");
		}
		if (!sceneDepth && !hudMask) {
			return;
		}
		const auto module = ::GetModuleHandleW(L"sl.interposer.dll");
		if (!module || !a_sdk || !a_sdk->hooking) {
			logger::info("depth: no Streamline (sl.interposer.dll); blocks are hidden face by face and drawn over the game's HUD");
			return;
		}
		const bool tag = Attach(module, "slSetTag", reinterpret_cast<void*>(&SetTagHook), setTagTarget, originalSetTag);
		const bool constants = Attach(module, "slSetConstants", reinterpret_cast<void*>(&SetConstantsHook), setConstantsTarget, originalSetConstants);
		const bool evaluate = Attach(module, "slEvaluateFeature", reinterpret_cast<void*>(&EvaluateHook), evaluateTarget, originalEvaluate);
		logger::info("depth: Streamline hooks: slSetTag {}, slSetConstants {}, slEvaluateFeature {}", tag, constants, evaluate);
	}

	void Uninstall()
	{
		if (!sdk || !sdk->hooking) {
			return;
		}
		for (auto* target : { setTagTarget, setConstantsTarget, evaluateTarget }) {
			if (target) {
				sdk->hooking->Detach(pluginHandle, target);
			}
		}
		setTagTarget = setConstantsTarget = evaluateTarget = nullptr;
	}

	bool Acquire(View& a_out)
	{
		std::lock_guard lock(gMutex);
		const auto      now = std::chrono::steady_clock::now();
		PruneRetired(now);
		if (!copy.texture || now - copy.at > std::chrono::milliseconds(200) || cameraNear <= 0.0f) {
			return false;
		}
		a_out.texture = copy.texture.Get();
		a_out.srvFormat = copy.srvFormat;
		a_out.width = copy.width;
		a_out.height = copy.height;
		const bool whole = copy.extent.width == 0 || copy.extent.height == 0;
		a_out.left = whole ? 0 : copy.extent.left;
		a_out.top = whole ? 0 : copy.extent.top;
		a_out.extentWidth = whole ? copy.width : copy.extent.width;
		a_out.extentHeight = whole ? copy.height : copy.extent.height;
		a_out.cameraNear = cameraNear;
		a_out.cameraFar = cameraFar;
		a_out.inverted = inverted;
		return true;
	}

	bool AcquireHud(ID3D12GraphicsCommandList* a_list, UINT a_width, UINT a_height, Hud& a_out)
	{
		std::lock_guard lock(gMutex);
		PruneRetired(std::chrono::steady_clock::now());
		a_out = {};
		for (auto* layer : { &hudless, &ui }) {
			if (layer->source) {
				layer->copied = CopyLayer(*layer, a_list);
				layer->source.Reset();
			}
		}
		// The back buffer's pixels lie in the layer's tagged area, which must be the back buffer's
		// size, or in all of it.
		const auto place = [&](Layer& a_layer, ID3D12Resource*& a_texture, DXGI_FORMAT& a_format, UINT& a_left, UINT& a_top) {
			if (!a_layer.copied) {
				return;
			}
			const bool whole = a_layer.extent.width == 0 || a_layer.extent.height == 0;
			const UINT left = whole ? 0 : a_layer.extent.left;
			const UINT top = whole ? 0 : a_layer.extent.top;
			const UINT width = whole ? a_layer.width : a_layer.extent.width;
			const UINT height = whole ? a_layer.height : a_layer.extent.height;
			if (width != a_width || height != a_height || left + width > a_layer.width || top + height > a_layer.height) {
				if (!a_layer.warned) {
					a_layer.warned = true;
					logger::warn("hud: the game's {} covers {}x{} at ({}, {}), not the {}x{} back buffer; not used", a_layer.name, width, height, left, top,
						a_width, a_height);
				}
				return;
			}
			a_texture = a_layer.copy.Get();
			a_format = a_layer.srvFormat;
			a_left = left;
			a_top = top;
		};
		place(hudless, a_out.hudless, a_out.hudlessFormat, a_out.hudlessLeft, a_out.hudlessTop);
		place(ui, a_out.ui, a_out.uiFormat, a_out.uiLeft, a_out.uiTop);
		return a_out.hudless || a_out.ui;
	}

	void EndFrame()
	{
		std::lock_guard lock(gMutex);
		for (auto* layer : { &hudless, &ui }) {
			layer->source.Reset();
			layer->copied = false;
		}
	}
}
