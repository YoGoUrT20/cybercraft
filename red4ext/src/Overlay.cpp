#include "Overlay.h"

#include <d3dcompiler.h>

#include <fstream>

#include <RED4ext/GpuApi/DeviceData.hpp>

#include "Config.h"
#include "Game.h"
#include "SceneDepth.h"
#include "World.h"

namespace cybercraft
{
	namespace
	{
		using Microsoft::WRL::ComPtr;

		using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
		using Present1Fn = HRESULT(WINAPI*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
		using ExecuteFn = void(WINAPI*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

		constexpr UINT kVtPresent = 8;    // IDXGISwapChain::Present
		constexpr UINT kVtPresent1 = 22;  // IDXGISwapChain1::Present1
		constexpr UINT kVtExecute = 10;   // ID3D12CommandQueue::ExecuteCommandLists
		constexpr UINT kSlots = 2;        // frames in flight of our own

		ExecuteFn originalExecute = nullptr;

		// Present and Present1 of each swap chain class whose vtable points at ours: dxgi's, and the
		// one the game presents through when that's another (Streamline's proxy).
		struct Hooked
		{
			void**     vtable{ nullptr };
			PresentFn  present{ nullptr };
			Present1Fn present1{ nullptr };
		};
		std::array<Hooked, 4>    presentHooks{};
		std::atomic<std::size_t> presentHookCount{ 0 };

		// The queue the game renders with: D3D12 gives no way to ask a swap chain for it, so it is
		// caught on its way through ExecuteCommandLists (the same trick every D3D12 overlay uses).
		// Only for when the game's own swap chain wasn't found (below).
		std::atomic<ID3D12CommandQueue*> gameQueue{ nullptr };

		// The swap chain the game presents and its direct queue, from RED4ext's GpuApi (where Cyber
		// Engine Tweaks draws its overlay too). The game holds Streamline's proxy, not dxgi's swap
		// chain, and with frame generation on the proxy's back buffers are frame generation's:
		// drawn into before the game's Present, Minecraft goes through frame generation with the
		// game's own HUD. dxgi's Present comes afterwards, inside the proxy's or, with frame
		// generation on, from its own thread for real and generated frames alike, on back buffers
		// it owns; drawing there removed the device (0x887a002b).
		std::atomic<IDXGISwapChain*>     gameSwapChain{ nullptr };
		std::atomic<ID3D12CommandQueue*> gameDirectQueue{ nullptr };
		std::atomic<bool>                gameSwapChainIsDxgi{ false };

		thread_local bool presenting = false;  // inside a Present already: a wrapper calling down

		struct Slot
		{
			ComPtr<ID3D12CommandAllocator> allocator;
			ComPtr<ID3D12Resource>         upload;
			UINT64                         uploadBytes{ 0 };
			UINT64                         fenceValue{ 0 };
		};

		struct State12
		{
			ComPtr<ID3D12Device>              device;
			ComPtr<ID3D12RootSignature>       rootSignature;
			ComPtr<ID3D12PipelineState>       psoMain;
			ComPtr<ID3D12PipelineState>       psoInvert;
			ComPtr<ID3D12GraphicsCommandList> commandList;
			ComPtr<ID3D12DescriptorHeap>      srvHeap;
			ComPtr<ID3D12DescriptorHeap>      rtvHeap;
			ComPtr<ID3D12Resource>            texture;
			ComPtr<ID3D12Resource>            cursor;  // the system's arrow (CreateCursor)
			UINT                              cursorW{ 0 }, cursorH{ 0 };
			UINT                              cursorHotX{ 0 }, cursorHotY{ 0 };
			ComPtr<ID3D12Fence>               fence;
			HANDLE                            fenceEvent{ nullptr };
			UINT64                            fenceValue{ 0 };
			std::array<Slot, kSlots>          slots;
			UINT                              slot{ 0 };
			UINT                              texW{ 0 }, texH{ 0 };
			bool                              ready{ false };
			bool                              failed{ false };
			bool                              haveFrame{ false };
			bool                              flipY{ true };
		};

		State12 g;
		std::mutex gMutex;

		// Composites Minecraft's overlay image (premultiplied alpha), with the cursor and the
		// crosshair's invert rectangle.
		constexpr char kShader[] = R"(
cbuffer Params : register(b0) { float2 cursor; float2 viewport; float cursorOn; float flipY; float2 cursorSize; float4 invertRect; float2 cursorHot; float2 pad; };
Texture2D overlay : register(t0);
Buffer<uint> cursorImage : register(t1);   // the system's arrow, premultiplied RGBA8, row by row
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
	VSOut o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	o.uv = uv;
	return o;
}
bool InInvertRect(float2 p) { return all(p >= invertRect.xy) && all(p < invertRect.zw); }
float4 Overlay(float2 uv) {
	if (flipY > 0.5) uv.y = 1 - uv.y;
	return overlay.Sample(samp, uv);   // premultiplied alpha straight from Minecraft
}
// The mouse pointer while a Minecraft screen is open, its hotspot on the cursor position.
float4 Cursor(float2 pos) {
	float2 p = pos - cursor;
	if (cursorSize.x < 1) {
		// No system arrow to draw: a plain one.
		if (p.x >= 0 && p.y >= 0 && p.y < 18 && p.x <= p.y * 0.6) {
			bool edge = p.x < 1.5 || p.x > p.y * 0.6 - 1.5 || p.y > 16.5;
			return float4(edge ? float3(0, 0, 0) : float3(1, 1, 1), 1);
		}
		return 0;
	}
	int2 texel = int2(floor(p + cursorHot));
	if (any(texel < 0) || any(texel >= int2(cursorSize))) return 0;
	uint v = cursorImage[texel.y * int(cursorSize.x) + texel.x];
	return float4(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24) / 255.0;
}
// Minecraft's crosshair and attack indicator: drawn with Minecraft's invert blend against
// Cyberpunk's picture, in a pass of their own; left out of the main one.
float4 PSInvert(VSOut i) : SV_Target {
	if (!InInvertRect(i.pos.xy)) discard;
	return float4(Overlay(i.uv).rgb, 0);
}
float4 PSMain(VSOut i) : SV_Target {
	float2 uv = i.uv;
	if (InInvertRect(i.pos.xy)) return 0;
	float4 c = Overlay(uv);
	if (cursorOn > 0.5) {
		float4 k = Cursor(i.pos.xy);
		c = k + c * (1 - k.a);
	}
	return c;
}
)";

		struct Params
		{
			float cursor[2];
			float viewport[2];
			float cursorOn;
			float flipY;
			float cursorSize[2];  // 0 x 0: no system arrow, the shader draws a plain one
			float invertRect[4];
			float cursorHot[2];
			float pad[2];
		};
		static_assert(sizeof(Params) == 64);

		ComPtr<ID3DBlob> Compile(const char* a_entry, const char* a_target)
		{
			ComPtr<ID3DBlob> code;
			ComPtr<ID3DBlob> errors;
			const auto       hr = ::D3DCompile(kShader, sizeof(kShader) - 1, "CyberCraftOverlay", nullptr, nullptr, a_entry, a_target,
				D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
			if (FAILED(hr)) {
				logger::error("overlay: shader {} failed: {}", a_entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
				return nullptr;
			}
			return code;
		}

		bool CreatePipelines(DXGI_FORMAT a_format)
		{
			auto vs = Compile("VSMain", "vs_5_0");
			auto ps = Compile("PSMain", "ps_5_0");
			auto psInvert = Compile("PSInvert", "ps_5_0");
			if (!vs || !ps || !psInvert) {
				return false;
			}

			// t0 Minecraft's image, t1 the cursor.
			D3D12_DESCRIPTOR_RANGE range{};
			range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			range.NumDescriptors = 2;
			range.BaseShaderRegister = 0;

			D3D12_ROOT_PARAMETER params[2]{};
			params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			params[0].DescriptorTable.NumDescriptorRanges = 1;
			params[0].DescriptorTable.pDescriptorRanges = &range;
			params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			params[1].Constants.Num32BitValues = sizeof(Params) / 4;

			// Point sampling: Minecraft's GUI is pixel art, and the overlay is usually the same size
			// as the back buffer anyway.
			D3D12_STATIC_SAMPLER_DESC sampler{};
			sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
			sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			sampler.MaxLOD = D3D12_FLOAT32_MAX;
			sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			D3D12_ROOT_SIGNATURE_DESC rootDesc{};
			rootDesc.NumParameters = 2;
			rootDesc.pParameters = params;
			rootDesc.NumStaticSamplers = 1;
			rootDesc.pStaticSamplers = &sampler;
			rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

			ComPtr<ID3DBlob> blob;
			ComPtr<ID3DBlob> errors;
			if (FAILED(::D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors))) {
				logger::error("overlay: root signature failed: {}", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
				return false;
			}
			if (FAILED(g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.rootSignature)))) {
				return false;
			}

			D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
			pso.pRootSignature = g.rootSignature.Get();
			pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
			pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
			pso.SampleMask = UINT_MAX;
			pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			pso.NumRenderTargets = 1;
			pso.RTVFormats[0] = a_format;
			pso.SampleDesc.Count = 1;
			pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			pso.RasterizerState.DepthClipEnable = FALSE;
			pso.DepthStencilState.DepthEnable = FALSE;
			pso.DepthStencilState.StencilEnable = FALSE;

			auto& blend = pso.BlendState.RenderTarget[0];
			blend.BlendEnable = TRUE;
			blend.BlendOp = D3D12_BLEND_OP_ADD;
			blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			// Minecraft hands over premultiplied alpha.
			blend.SrcBlend = D3D12_BLEND_ONE;
			blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
			blend.SrcBlendAlpha = D3D12_BLEND_ONE;
			blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
			if (FAILED(g.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g.psoMain)))) {
				return false;
			}

			// Minecraft's invert blend: out = src * (1 - dst) + dst * (1 - src).
			pso.PS = { psInvert->GetBufferPointer(), psInvert->GetBufferSize() };
			blend.SrcBlend = D3D12_BLEND_INV_DEST_COLOR;
			blend.DestBlend = D3D12_BLEND_INV_SRC_COLOR;
			blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
			blend.DestBlendAlpha = D3D12_BLEND_ONE;
			return SUCCEEDED(g.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g.psoInvert)));
		}

		bool EnsureTexture(UINT a_width, UINT a_height)
		{
			if (g.texture && g.texW == a_width && g.texH == a_height) {
				return true;
			}
			// The old texture may still be in flight; the fence wait below happens per slot, so
			// drain everything before dropping it.
			if (g.texture && g.fence) {
				const auto target = g.fenceValue;
				if (g.fence->GetCompletedValue() < target) {
					g.fence->SetEventOnCompletion(target, g.fenceEvent);
					::WaitForSingleObject(g.fenceEvent, 1000);
				}
			}
			g.texture.Reset();

			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = a_width;
			desc.Height = a_height;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
					IID_PPV_ARGS(&g.texture)))) {
				logger::error("overlay: {}x{} texture failed", a_width, a_height);
				return false;
			}

			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = desc.Format;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = 1;
			g.device->CreateShaderResourceView(g.texture.Get(), &srv, g.srvHeap->GetCPUDescriptorHandleForHeapStart());
			g.texW = a_width;
			g.texH = a_height;
			return true;
		}

		bool EnsureUpload(Slot& a_slot, UINT64 a_bytes)
		{
			if (a_slot.upload && a_slot.uploadBytes >= a_bytes) {
				return true;
			}
			a_slot.upload.Reset();
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_UPLOAD;
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			desc.Width = a_bytes;
			desc.Height = 1;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = DXGI_FORMAT_UNKNOWN;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
					IID_PPV_ARGS(&a_slot.upload)))) {
				return false;
			}
			a_slot.uploadBytes = a_bytes;
			return true;
		}

		// 32-bit top-down pixels of a GDI bitmap (BGRA; a monochrome one reads as black or white).
		std::vector<std::uint32_t> ReadBitmap(HBITMAP a_bitmap, UINT& a_width, UINT& a_height)
		{
			std::vector<std::uint32_t> pixels;
			BITMAP bm{};
			if (!a_bitmap || !::GetObjectW(a_bitmap, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0 || bm.bmWidth > 512 || bm.bmHeight > 1024) {
				return pixels;
			}
			BITMAPINFO info{};
			info.bmiHeader.biSize = sizeof(info.bmiHeader);
			info.bmiHeader.biWidth = bm.bmWidth;
			info.bmiHeader.biHeight = -bm.bmHeight;
			info.bmiHeader.biPlanes = 1;
			info.bmiHeader.biBitCount = 32;
			info.bmiHeader.biCompression = BI_RGB;
			pixels.resize(std::size_t(bm.bmWidth) * std::size_t(bm.bmHeight));
			HDC        dc = ::GetDC(nullptr);
			const int  rows = dc ? ::GetDIBits(dc, a_bitmap, 0, static_cast<UINT>(bm.bmHeight), pixels.data(), &info, DIB_RGB_COLORS) : 0;
			if (dc) {
				::ReleaseDC(nullptr, dc);
			}
			if (rows != bm.bmHeight) {
				pixels.clear();
				return pixels;
			}
			a_width = static_cast<UINT>(bm.bmWidth);
			a_height = static_cast<UINT>(bm.bmHeight);
			return pixels;
		}

		struct CursorImage
		{
			std::vector<std::uint32_t> pixels;  // premultiplied RGBA8 (R in the low byte), row by row
			UINT                       width{ 0 }, height{ 0 };
			UINT                       hotX{ 0 }, hotY{ 0 };
		};

		// The system's arrow, as the desktop draws it (the user's pointer scheme and size). Minecraft
		// draws no pointer of its own: its window's is the OS's, and that window is hidden.
		CursorImage LoadArrow()
		{
			CursorImage image;
			ICONINFO    info{};
			const auto  arrow = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
			if (!arrow || !::GetIconInfo(arrow, &info)) {
				return image;
			}
			UINT       colorW = 0, colorH = 0, maskW = 0, maskH = 0;
			const auto color = ReadBitmap(info.hbmColor, colorW, colorH);
			const auto mask = ReadBitmap(info.hbmMask, maskW, maskH);
			if (info.hbmColor) {
				::DeleteObject(info.hbmColor);
			}
			if (info.hbmMask) {
				::DeleteObject(info.hbmMask);
			}

			const auto put = [&](std::uint32_t a_r, std::uint32_t a_g, std::uint32_t a_b, std::uint32_t a_a) {
				image.pixels.push_back((a_r * a_a / 255) | (a_g * a_a / 255) << 8 | (a_b * a_a / 255) << 16 | a_a << 24);
			};
			if (!color.empty()) {
				// A colour cursor: its own alpha, or (an old one) the AND mask's.
				image.width = colorW;
				image.height = colorH;
				const bool alpha = std::ranges::any_of(color, [](std::uint32_t a_px) { return (a_px >> 24) != 0; });
				const bool haveMask = maskW == colorW && maskH >= colorH;
				for (std::size_t i = 0; i < color.size(); ++i) {
					const std::uint32_t blue = color[i] & 0xFF, green = (color[i] >> 8) & 0xFF, red = (color[i] >> 16) & 0xFF;
					if (alpha) {
						put(red, green, blue, color[i] >> 24);
					} else if (!haveMask || (mask[i] & 0xFFFFFF) == 0) {
						put(red, green, blue, 255);
					} else {
						// AND set and a colour: the screen inverted there. Nothing to invert here, so black.
						put(0, 0, 0, (red | green | blue) ? 255 : 0);
					}
				}
			} else if (!mask.empty() && maskH >= 2) {
				// A monochrome cursor: the AND mask over the XOR mask.
				image.width = maskW;
				image.height = maskH / 2;
				for (UINT y = 0; y < image.height; ++y) {
					for (UINT x = 0; x < image.width; ++x) {
						const bool andBit = (mask[std::size_t(y) * maskW + x] & 0xFFFFFF) != 0;
						const bool xorBit = (mask[std::size_t(y + image.height) * maskW + x] & 0xFFFFFF) != 0;
						if (!andBit) {
							put(xorBit ? 255 : 0, xorBit ? 255 : 0, xorBit ? 255 : 0, 255);
						} else {
							put(0, 0, 0, xorBit ? 255 : 0);  // inverted screen, drawn black
						}
					}
				}
			}
			image.hotX = std::min<UINT>(info.xHotspot, image.width);
			image.hotY = std::min<UINT>(info.yHotspot, image.height);
			return image;
		}

		// The arrow goes up once, in a buffer the shader reads straight out of the upload heap (a few
		// KB). Without one the descriptor is a null view, never read: the shader draws a plain arrow.
		void CreateCursor()
		{
			auto handle = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
			handle.ptr += g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_R32_UINT;
			srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Buffer.NumElements = 1;

			const auto image = LoadArrow();
			g.cursor.Reset();
			g.cursorW = g.cursorH = 0;
			if (!image.pixels.empty()) {
				D3D12_HEAP_PROPERTIES heap{};
				heap.Type = D3D12_HEAP_TYPE_UPLOAD;
				D3D12_RESOURCE_DESC desc{};
				desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				desc.Width = image.pixels.size() * sizeof(std::uint32_t);
				desc.Height = 1;
				desc.DepthOrArraySize = 1;
				desc.MipLevels = 1;
				desc.Format = DXGI_FORMAT_UNKNOWN;
				desc.SampleDesc.Count = 1;
				desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				void* mapped = nullptr;
				if (SUCCEEDED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
						IID_PPV_ARGS(&g.cursor))) &&
					SUCCEEDED(g.cursor->Map(0, nullptr, &mapped))) {
					std::memcpy(mapped, image.pixels.data(), desc.Width);
					g.cursor->Unmap(0, nullptr);
					srv.Buffer.NumElements = static_cast<UINT>(image.pixels.size());
					g.cursorW = image.width;
					g.cursorH = image.height;
					g.cursorHotX = image.hotX;
					g.cursorHotY = image.hotY;
				} else {
					g.cursor.Reset();
				}
			}
			g.device->CreateShaderResourceView(g.cursor.Get(), &srv, handle);
			if (g.cursorW) {
				logger::info("overlay: cursor is the system arrow, {}x{} with its hotspot at ({}, {})", g.cursorW, g.cursorH, g.cursorHotX, g.cursorHotY);
			} else {
				logger::warn("overlay: no system arrow to draw; Minecraft's screens get a plain one");
			}
		}

		bool InitResources(IDXGISwapChain* a_swapChain, DXGI_FORMAT a_format)
		{
			if (g.ready) {
				return true;
			}
			if (g.failed) {
				return false;
			}
			g.failed = true;  // cleared on the way out; one failure is enough to stop retrying

			if (FAILED(a_swapChain->GetDevice(IID_PPV_ARGS(&g.device)))) {
				logger::error("overlay: the swap chain is not D3D12");
				return false;
			}
			D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
			srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			srvDesc.NumDescriptors = 2;  // Minecraft's image, the cursor
			srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			if (FAILED(g.device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g.srvHeap)))) {
				return false;
			}
			CreateCursor();
			D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
			rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			rtvDesc.NumDescriptors = 1;
			if (FAILED(g.device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g.rtvHeap)))) {
				return false;
			}
			if (!CreatePipelines(a_format)) {
				return false;
			}
			for (auto& slot : g.slots) {
				if (FAILED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator)))) {
					return false;
				}
			}
			if (FAILED(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.slots[0].allocator.Get(), nullptr, IID_PPV_ARGS(&g.commandList)))) {
				return false;
			}
			g.commandList->Close();
			if (FAILED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) {
				return false;
			}
			g.fenceEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (!g.fenceEvent) {
				return false;
			}
			g.failed = false;
			g.ready = true;
			logger::info("overlay: D3D12 resources ready (back buffer format {})", static_cast<int>(a_format));
			return true;
		}

		// DLSS, FSR and XeSS frame generation own the swap chain's back buffers between the game's
		// Present and dxgi's. Only asked when Minecraft would be drawn at dxgi's Present (the game's
		// own swap chain wasn't found, or is dxgi's): drawing there with frame generation on removed
		// the device (0x887a002b). Read from the game's own settings, again whenever the file changes.
		bool FrameGenerationOn()
		{
			static std::filesystem::file_time_type seen{};
			static bool                            on = false;
			static auto                            nextCheck = std::chrono::steady_clock::time_point{};
			const auto                             now = std::chrono::steady_clock::now();
			if (now < nextCheck) {
				return on;
			}
			nextCheck = now + std::chrono::seconds(2);

			wchar_t local[MAX_PATH]{};
			const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
			if (n == 0 || n >= MAX_PATH) {
				return on;
			}
			const auto      path = std::filesystem::path(local) / L"CD Projekt Red" / L"Cyberpunk 2077" / L"UserSettings.json";
			std::error_code ec;
			const auto      written = std::filesystem::last_write_time(path, ec);
			if (ec || written == seen) {
				return on;
			}
			seen = written;
			std::ifstream in(path, std::ios::binary);
			const std::string json{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };

			bool found = false;
			for (const char* name : { "\"DLSSFrameGen\"", "\"FSR3_FrameGeneration\"", "\"XESS_FrameGeneration\"" }) {
				const auto at = json.find(name);
				const auto value = at == std::string::npos ? std::string::npos : json.find("\"value\"", at);
				if (value == std::string::npos) {
					continue;
				}
				const auto start = json.find_first_not_of(" \t\r\n:", value + 7);
				if (start != std::string::npos && json.compare(start, 4, "true") == 0) {
					found = true;
				}
			}
			if (found != on) {
				if (found) {
					logger::warn("overlay: frame generation is on in the game's graphics settings; Minecraft's overlay and blocks stay hidden until it is off");
				} else {
					logger::info("overlay: frame generation is off; drawing Minecraft");
				}
			}
			on = found;
			return on;
		}

		void Barrier(ID3D12Resource* a_resource, D3D12_RESOURCE_STATES a_from, D3D12_RESOURCE_STATES a_to)
		{
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = a_resource;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = a_from;
			barrier.Transition.StateAfter = a_to;
			g.commandList->ResourceBarrier(1, &barrier);
		}

		// a_queue: the game's direct queue, the one a_swapChain presents on. Called through its
		// vtable: under Streamline it may be a proxy, which takes our native command lists as Cyber
		// Engine Tweaks' do.
		void Composite(IDXGISwapChain* a_swapChain, ID3D12CommandQueue* a_queue)
		{
			auto& st = State();
			auto& link = Link::Get();

			ComPtr<IDXGISwapChain3> swapChain3;
			if (FAILED(a_swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3)))) {
				return;
			}
			DXGI_SWAP_CHAIN_DESC desc{};
			if (FAILED(a_swapChain->GetDesc(&desc))) {
				return;
			}
			st.viewportW = static_cast<int>(desc.BufferDesc.Width);
			st.viewportH = static_cast<int>(desc.BufferDesc.Height);

			// Only while Minecraft drives V, or V is in a car (blocks only): not over Cyberpunk's menus
			// and loading screens, where V is a placeholder at the origin, and not while Minecraft is
			// still opening its world.
			if (!a_queue || !link.Valid() || !link.McAlive() || !st.mcInWorld.load() || !st.drawBlocks.load()) {
				return;
			}
			if (!InitResources(a_swapChain, desc.BufferDesc.Format)) {
				return;
			}

			// A newer Minecraft frame, if there is one; otherwise the last one is drawn again.
			bool newFrame = false;
			if (link.AcquireOverlayFrame()) {
				const auto* hdr = link.FrontHeader();
				if (hdr->width > 0 && hdr->height > 0 && hdr->width <= proto::kMaxOverlayW && hdr->height <= proto::kMaxOverlayH) {
					g.flipY = (hdr->flags & 1) != 0;
					g.haveFrame = EnsureTexture(hdr->width, hdr->height);
					newFrame = g.haveFrame;
				}
			}

			// Diagnostics: how many new Minecraft frames arrive against Cyberpunk's own, and what
			// copying them costs this thread.
			static auto          statsSince = std::chrono::steady_clock::now();
			static std::uint32_t statsPresents = 0, statsFrames = 0;
			static double        statsCopyMs = 0.0;
			++statsPresents;
			statsFrames += newFrame ? 1 : 0;
			if (const auto now = std::chrono::steady_clock::now(); now - statsSince >= std::chrono::seconds(5)) {
				if (Config::Diagnostics()) {
					const double seconds = std::chrono::duration<double>(now - statsSince).count();
					logger::info("overlay: {:.0f} Minecraft frames/s against {:.0f} Cyberpunk frames/s, {:.2f} ms per copy (screen open {})",
						statsFrames / seconds, statsPresents / seconds, statsFrames ? statsCopyMs / statsFrames : 0.0, st.mcScreenOpen.load());
				}
				statsSince = now;
				statsPresents = statsFrames = 0;
				statsCopyMs = 0.0;
			}
			// Blocks are drawn even on a frame where Minecraft published no new overlay, and the
			// overlay is drawn even before the first block arrives; a Cyberpunk menu hides both. In a
			// car only the blocks: Minecraft's hotbar and hand don't belong over Cyberpunk's driving.
			const bool menu = st.gameMenuOpen.load();
			const bool drawOverlay = g.haveFrame && g.texture && !menu && st.puppeting.load();
			const bool drawWorld = !menu && World::Prepare(g.device.Get(), desc.BufferDesc.Format, desc.BufferDesc.Width, desc.BufferDesc.Height);
			if (!drawOverlay && !drawWorld) {
				return;
			}

			auto& slot = g.slots[g.slot];
			if (slot.fenceValue > 0 && g.fence->GetCompletedValue() < slot.fenceValue) {
				if (FAILED(g.fence->SetEventOnCompletion(slot.fenceValue, g.fenceEvent))) {
					return;
				}
				::WaitForSingleObject(g.fenceEvent, 1000);
			}

			// Only a new Minecraft frame is uploaded: the texture keeps the last one, and copying the
			// same ~15 MB again every Cyberpunk frame cost this thread for nothing.
			const UINT rowBytes = g.texW * 4;
			const UINT rowPitch = (rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
			const bool upload = drawOverlay && newFrame;
			if (upload) {
				const auto copyStart = std::chrono::steady_clock::now();
				if (!EnsureUpload(slot, UINT64(rowPitch) * g.texH)) {
					return;
				}
				void* mapped = nullptr;
				if (FAILED(slot.upload->Map(0, nullptr, &mapped))) {
					return;
				}
				const auto* src = link.FrontPixels();
				for (UINT y = 0; y < g.texH; ++y) {
					std::memcpy(static_cast<std::uint8_t*>(mapped) + std::size_t(y) * rowPitch, src + std::size_t(y) * rowBytes, rowBytes);
				}
				slot.upload->Unmap(0, nullptr);
				statsCopyMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - copyStart).count();
			}

			ComPtr<ID3D12Resource> backBuffer;
			if (FAILED(swapChain3->GetBuffer(swapChain3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer)))) {
				return;
			}
			const auto rtv = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
			g.device->CreateRenderTargetView(backBuffer.Get(), nullptr, rtv);

			if (FAILED(slot.allocator->Reset()) || FAILED(g.commandList->Reset(slot.allocator.Get(), nullptr))) {
				return;
			}

			if (upload) {
			Barrier(g.texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
			D3D12_TEXTURE_COPY_LOCATION dstLoc{};
			dstLoc.pResource = g.texture.Get();
			dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dstLoc.SubresourceIndex = 0;
			D3D12_TEXTURE_COPY_LOCATION srcLoc{};
			srcLoc.pResource = slot.upload.Get();
			srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			srcLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			srcLoc.PlacedFootprint.Footprint.Width = g.texW;
			srcLoc.PlacedFootprint.Footprint.Height = g.texH;
			srcLoc.PlacedFootprint.Footprint.Depth = 1;
			srcLoc.PlacedFootprint.Footprint.RowPitch = rowPitch;
			g.commandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
			Barrier(g.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			}

			// Cyberpunk's finished picture, before anything of ours goes over it: the blocks take
			// their light from it.
			if (drawWorld) {
				World::CaptureScene(g.commandList.Get(), backBuffer.Get());
			}
			Barrier(backBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

			// The blocks go down first, in world space with their own depth buffer, under the game's
			// HUD when it hands frame generation the frame without it; the overlay (hand, hotbar,
			// GUI) is drawn on top of them.
			if (drawWorld) {
				World::Consume();
				proto::McState mc{};
				if (link.ReadMcState(mc)) {
					World::Draw(g.commandList.Get(), mc, desc.BufferDesc.Width, desc.BufferDesc.Height, rtv);
				}
			}

			Params params{};
			const float sx = g.texW ? float(desc.BufferDesc.Width) / float(g.texW) : 1.0f;
			const float sy = g.texH ? float(desc.BufferDesc.Height) / float(g.texH) : 1.0f;
			params.cursor[0] = st.cursorX.load() * sx;
			params.cursor[1] = st.cursorY.load() * sy;
			params.viewport[0] = float(desc.BufferDesc.Width);
			params.viewport[1] = float(desc.BufferDesc.Height);
			params.cursorOn = st.mcScreenOpen.load() ? 1.0f : 0.0f;
			params.cursorSize[0] = float(g.cursorW);
			params.cursorSize[1] = float(g.cursorH);
			params.cursorHot[0] = float(g.cursorHotX);
			params.cursorHot[1] = float(g.cursorHotY);
			params.flipY = g.flipY ? 1.0f : 0.0f;
			// Around the screen centre, where Minecraft puts the crosshair (15 GUI pixels) and the
			// attack indicator under it (16 x 16 from 9 GUI pixels below the centre).
			const int   scale = st.mcGuiScale.load();
			const float gui = float(scale) * sx;
			const float cx = float(desc.BufferDesc.Width) * 0.5f;
			const float cy = float(desc.BufferDesc.Height) * 0.5f;
			const bool  invert = st.mcCrosshair.load() && scale > 0;
			params.invertRect[0] = invert ? cx - 12.0f * gui : 0.0f;
			params.invertRect[1] = invert ? cy - 12.0f * gui : 0.0f;
			params.invertRect[2] = invert ? cx + 12.0f * gui : 0.0f;
			params.invertRect[3] = invert ? cy + 28.0f * gui : 0.0f;

			if (!drawOverlay) {
				Barrier(backBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
				if (FAILED(g.commandList->Close())) {
					return;
				}
				ID3D12CommandList* worldOnly[]{ g.commandList.Get() };
				a_queue->ExecuteCommandLists(1, worldOnly);
				slot.fenceValue = ++g.fenceValue;
				a_queue->Signal(g.fence.Get(), slot.fenceValue);
				g.slot = (g.slot + 1) % kSlots;
				return;
			}

			const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, float(desc.BufferDesc.Width), float(desc.BufferDesc.Height), 0.0f, 1.0f };
			const D3D12_RECT     scissor{ 0, 0, LONG(desc.BufferDesc.Width), LONG(desc.BufferDesc.Height) };
			ID3D12DescriptorHeap* heaps[]{ g.srvHeap.Get() };
			g.commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
			g.commandList->RSSetViewports(1, &viewport);
			g.commandList->RSSetScissorRects(1, &scissor);
			g.commandList->SetGraphicsRootSignature(g.rootSignature.Get());
			g.commandList->SetDescriptorHeaps(1, heaps);
			g.commandList->SetGraphicsRootDescriptorTable(0, g.srvHeap->GetGPUDescriptorHandleForHeapStart());
			g.commandList->SetGraphicsRoot32BitConstants(1, sizeof(Params) / 4, &params, 0);
			g.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			g.commandList->SetPipelineState(g.psoMain.Get());
			g.commandList->DrawInstanced(3, 1, 0, 0);
			if (invert) {
				g.commandList->SetPipelineState(g.psoInvert.Get());
				g.commandList->DrawInstanced(3, 1, 0, 0);
			}

			Barrier(backBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
			if (FAILED(g.commandList->Close())) {
				return;
			}

			ID3D12CommandList* lists[]{ g.commandList.Get() };
			a_queue->ExecuteCommandLists(1, lists);
			slot.fenceValue = ++g.fenceValue;
			a_queue->Signal(g.fence.Get(), slot.fenceValue);
			g.slot = (g.slot + 1) % kSlots;
		}

		const Hooked* Find(void** a_vtable)
		{
			const auto count = presentHookCount.load();
			for (std::size_t i = 0; i < count; ++i) {
				if (presentHooks[i].vtable == a_vtable) {
					return &presentHooks[i];
				}
			}
			return nullptr;
		}

		const Hooked& Find(IUnknown* a_object)
		{
			// Only ever called from our hooks, which only sit in vtables listed here.
			return *Find(*reinterpret_cast<void***>(a_object));
		}

		void Draw(IDXGISwapChain* a_swapChain)
		{
			auto* game = gameSwapChain.load();
			if (game && a_swapChain != game) {
				return;  // dxgi's, under the game's: drawn already, or frame generation presenting
			}
			std::lock_guard lock(gMutex);
			if ((!game || gameSwapChainIsDxgi.load()) && FrameGenerationOn()) {
				SceneDepth::EndFrame();
				return;
			}
			Composite(a_swapChain, game ? gameDirectQueue.load() : gameQueue.load());
			// What the game tagged for frame generation belonged to this Present.
			SceneDepth::EndFrame();
		}

		template <class Fn>
		HRESULT Present(IDXGISwapChain* a_swapChain, Fn&& a_original)
		{
			if (presenting) {
				return a_original();
			}
			Draw(a_swapChain);
			presenting = true;
			const HRESULT result = a_original();
			presenting = false;
			return result;
		}

		HRESULT WINAPI PresentHook(IDXGISwapChain* a_swapChain, UINT a_sync, UINT a_flags)
		{
			const auto original = Find(a_swapChain).present;
			return Present(a_swapChain, [&] { return original(a_swapChain, a_sync, a_flags); });
		}

		HRESULT WINAPI Present1Hook(IDXGISwapChain1* a_swapChain, UINT a_sync, UINT a_flags, const DXGI_PRESENT_PARAMETERS* a_params)
		{
			const auto original = Find(a_swapChain).present1;
			return Present(a_swapChain, [&] { return original(a_swapChain, a_sync, a_flags, a_params); });
		}

		void WINAPI ExecuteHook(ID3D12CommandQueue* a_queue, UINT a_count, ID3D12CommandList* const* a_lists)
		{
			if (a_queue && a_queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT && !gameQueue.load()) {
				gameQueue.store(a_queue);
				logger::info("overlay: caught the game's direct command queue");
			}
			originalExecute(a_queue, a_count, a_lists);
		}

		bool WriteSlot(void** a_vtable, UINT a_index, void* a_detour)
		{
			DWORD oldProtect = 0;
			if (!::VirtualProtect(&a_vtable[a_index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
				return false;
			}
			a_vtable[a_index] = a_detour;
			::VirtualProtect(&a_vtable[a_index], sizeof(void*), oldProtect, &oldProtect);
			return true;
		}

		bool PatchVtable(void* a_object, UINT a_index, void* a_detour, void** a_original)
		{
			auto** vtable = *reinterpret_cast<void***>(a_object);
			*a_original = vtable[a_index];
			return WriteSlot(vtable, a_index, a_detour);
		}

		// Points a swap chain class's Present (and Present1) at ours, keeping its originals.
		bool HookPresent(void** a_vtable, bool a_present1)
		{
			if (Find(a_vtable)) {
				return true;
			}
			const auto count = presentHookCount.load();
			if (count == presentHooks.size()) {
				return false;
			}
			auto& entry = presentHooks[count];
			entry.vtable = a_vtable;
			entry.present = reinterpret_cast<PresentFn>(a_vtable[kVtPresent]);
			entry.present1 = a_present1 ? reinterpret_cast<Present1Fn>(a_vtable[kVtPresent1]) : nullptr;
			presentHookCount.store(count + 1);  // findable before the first call through the new slots
			return WriteSlot(a_vtable, kVtPresent, &PresentHook) && (!a_present1 || WriteSlot(a_vtable, kVtPresent1, &Present1Hook));
		}

		// The module an address lies in ("" if none): a vtable that isn't in one isn't a vtable.
		std::string ModuleOf(const void* a_address)
		{
			HMODULE module = nullptr;
			if (!a_address || !::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
								  reinterpret_cast<LPCWSTR>(a_address), &module)) {
				return {};
			}
			wchar_t path[MAX_PATH]{};
			::GetModuleFileNameW(module, path, MAX_PATH);
			return std::filesystem::path(path).filename().string();
		}

		// Main thread, every second: finds the swap chain the game presents (it can be replaced,
		// switching frame generation for one) and hooks its class.
		void TrackGameSwapChain()
		{
			static auto next = std::chrono::steady_clock::time_point{};
			static bool warned = false;
			const auto  now = std::chrono::steady_clock::now();
			if (now < next) {
				return;
			}
			next = now + std::chrono::seconds(1);

			const auto missing = [](const char* a_why) {
				if (!warned && !gameSwapChain.load()) {  // once found, the last one found is kept
					warned = true;
					logger::warn("overlay: the game's own swap chain wasn't found ({}); Minecraft is drawn at dxgi's Present, and not while frame generation is on", a_why);
				}
			};
			auto* data = RED4ext::GpuApi::GetDeviceData();
			if (!data) {
				return missing("no GpuApi device data");
			}
			// RED4ext.SDK's layout: if a game update moved it, this pointer is the first thing off.
			auto& chains = data->swapChains;
			if (&chains.spinLockRef != &data->resourcesSpinLock) {
				return missing("GpuApi layout doesn't match this game version");
			}
			IDXGISwapChain* found = nullptr;
			for (auto& handle : chains.resources) {
				DWORD pid = 0;
				if (handle.IsUsed() && handle.instance.swapChain && ::IsWindow(handle.instance.windowHandle) &&
					::GetWindowThreadProcessId(handle.instance.windowHandle, &pid) && pid == ::GetCurrentProcessId()) {
					found = handle.instance.swapChain.Get();
					break;
				}
			}
			auto* queue = data->directCommandQueue.Get();
			if (!found || !queue) {
				return missing(found ? "no direct queue" : "no swap chain in use");
			}
			if (found == gameSwapChain.load()) {
				return;
			}
			auto**     vtable = *reinterpret_cast<void***>(found);
			const auto module = ModuleOf(vtable);
			if (module.empty() || ModuleOf(*reinterpret_cast<void**>(queue)).empty()) {
				return missing("GpuApi pointers aren't COM objects");
			}
			const bool dxgi = presentHookCount.load() > 0 && vtable == presentHooks[0].vtable;
			if (!HookPresent(vtable, true)) {
				return missing("couldn't hook its Present");
			}
			gameDirectQueue.store(queue);
			gameSwapChainIsDxgi.store(dxgi);
			gameSwapChain.store(found);
			warned = false;
			logger::info("overlay: the game presents through {}'s swap chain; Minecraft is drawn into it {}", module,
				dxgi ? "(not while frame generation is on)" : "before frame generation sees the frame");
		}

		// A throwaway device, queue and swap chain, only to read the vtables: DXGI and D3D12 share
		// one vtable per interface across every instance, so patching these patches the game's.
		bool HookThroughDummyObjects()
		{
			WNDCLASSEXW cls{ sizeof(cls) };
			cls.lpfnWndProc = ::DefWindowProcW;
			cls.hInstance = ::GetModuleHandleW(nullptr);
			cls.lpszClassName = L"CyberCraftOverlayDummy";
			::RegisterClassExW(&cls);
			HWND window = ::CreateWindowExW(0, cls.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr, nullptr, cls.hInstance, nullptr);
			if (!window) {
				logger::error("overlay: no dummy window ({})", ::GetLastError());
				return false;
			}

			bool hooked = false;
			{
				ComPtr<ID3D12Device> device;
				ComPtr<IDXGIFactory4> factory;
				if (SUCCEEDED(::D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) &&
					SUCCEEDED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
					D3D12_COMMAND_QUEUE_DESC queueDesc{};
					queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
					ComPtr<ID3D12CommandQueue> queue;
					ComPtr<IDXGISwapChain1>    swapChain;
					DXGI_SWAP_CHAIN_DESC1      chainDesc{};
					chainDesc.Width = 16;
					chainDesc.Height = 16;
					chainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
					chainDesc.SampleDesc.Count = 1;
					chainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
					chainDesc.BufferCount = 2;
					chainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
					if (SUCCEEDED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))) &&
						SUCCEEDED(factory->CreateSwapChainForHwnd(queue.Get(), window, &chainDesc, nullptr, nullptr, &swapChain))) {
						// dxgi's first: presentHooks[0] is how TrackGameSwapChain tells a game presenting straight to dxgi.
						const bool present = HookPresent(*reinterpret_cast<void***>(swapChain.Get()), false);
						const bool execute = PatchVtable(queue.Get(), kVtExecute, &ExecuteHook, reinterpret_cast<void**>(&originalExecute));
						hooked = present && execute;
						if (!hooked) {
							logger::error("overlay: vtable patch failed (present {}, execute {})", present, execute);
						}
					}
				}
			}
			::DestroyWindow(window);
			::UnregisterClassW(cls.lpszClassName, cls.hInstance);
			return hooked;
		}
	}

	namespace Overlay
	{
		void Install()
		{
			static bool installed = false;
			static bool ready = false;
			if (!installed) {
				installed = true;
				ready = HookThroughDummyObjects();
				if (ready) {
					logger::info("overlay: Present and ExecuteCommandLists hooked");
				}
			}
			if (ready) {
				TrackGameSwapChain();
			}
		}

		void Shutdown()
		{
			std::lock_guard lock(gMutex);
			// The GPU first, then the block resources its last frames were drawing with.
			if (g.fence && g.fenceEvent && g.fenceValue > 0 && g.fence->GetCompletedValue() < g.fenceValue) {
				g.fence->SetEventOnCompletion(g.fenceValue, g.fenceEvent);
				::WaitForSingleObject(g.fenceEvent, 1000);
			}
			World::Shutdown();
			if (g.fenceEvent) {
				::CloseHandle(g.fenceEvent);
			}
			g = State12{};
		}
	}
}
