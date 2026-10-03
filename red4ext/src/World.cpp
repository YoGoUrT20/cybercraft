#include "World.h"

#include <d3dcompiler.h>

#include <DirectXMath.h>

#include "Builds.h"
#include "Collision.h"
#include "Config.h"
#include "Game.h"
#include "SceneDepth.h"

namespace cybercraft::World
{
	namespace
	{
		using Microsoft::WRL::ComPtr;
		namespace dx = DirectX;

		constexpr char kShader[] = R"(
cbuffer Params : register(b0) {
	float4x4 viewProj;
	float3 origin; float drawPass;
	float4 light;        // x: daylight 0..1 (only without the scene picture)
	float4 eye;          // xyz: the camera, Minecraft coordinates; w: metres per block
	float4 forward;      // xyz: where it looks (unit)
	float4 depthParams;  // near, far, inverted, enabled: Cyberpunk's depth (SceneDepth)
	float4 depthMap;     // screen pixel -> depth texel: scale xy, offset zw
	float4 sceneParams;  // enabled, gain, floor, probe distance (blocks): lighting from Cyberpunk's picture
	float4 tone;         // its colour kept (0 grey, 1 all), Minecraft's face shading kept, haze per metre,
	                     // Minecraft's block light kept
};
Texture2D atlas : register(t0);
SamplerState samp : register(s0);
SamplerState smooth : register(s1);
// One byte per face (6 vertices): 0 when Cyberpunk's city stands in front of it (fallback only).
ByteAddressBuffer visibility : register(t1);
Texture2D<float> sceneDepth : register(t2);
Texture2D sceneColor : register(t3);
struct VSIn {
	float3 pos : POSITION;
	float2 uv : TEXCOORD0;
	float4 color : COLOR0;
	uint light : TEXCOORD1;
	uint flags : TEXCOORD2;
};
struct VSOut {
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
	float4 color : COLOR0;
	float3 world : TEXCOORD1;
	float2 lightPair : TEXCOORD2;  // block light, sky light (0..1)
	float shade : TEXCOORD3;       // Minecraft's fixed face shading
	float3 env : TEXCOORD4;        // Cyberpunk's light at this corner (SceneLight), blended over the face
	nointerpolation uint flags : TEXCOORD5;
	float3 haze : TEXCOORD6;       // Cyberpunk's picture right here: what distance fades the block into
};
float3 SceneLight(float2 uv);
// Where a point lands in Cyberpunk's picture; false behind the camera.
bool ScreenUv(float3 p, out float2 uv) {
	float4 clip = mul(viewProj, float4(p, 1));
	uv = clip.w > 0.01 ? saturate(clip.xy / clip.w * float2(0.5, -0.5) + 0.5) : float2(0.5, 0.5);
	return clip.w > 0.01;
}
// The way the face looks, or 0 without one. Bits 4-6: Minecraft Direction + 1 (down, up, north,
// south, west, east), or 7: models and items, their own normal in bits 8-31 as signed bytes x, y, z.
float3 Normal(uint flags) {
	uint code = (flags >> 4) & 7;
	if (code == 7) {
		float3 n = float3(asint(flags << 16) >> 24, asint(flags << 8) >> 24, asint(flags) >> 24);
		float len = length(n);
		return len > 1.0 ? n / len : float3(0, 0, 0);
	}
	return code == 1 ? float3(0, -1, 0) : code == 2 ? float3(0, 1, 0) : code == 3 ? float3(0, 0, -1)
	     : code == 4 ? float3(0, 0, 1) : code == 5 ? float3(-1, 0, 0) : code == 6 ? float3(1, 0, 0) : float3(0, 0, 0);
}
VSOut VSMain(VSIn i, uint vertexId : SV_VertexID) {
	VSOut o;
	o.world = i.pos + origin;
	o.pos = mul(viewProj, float4(o.world, 1));
	uint face = vertexId / 6;
	uint word = visibility.Load((face / 4) * 4);
	if (((word >> ((face % 4) * 8)) & 0xFF) == 0) {
		o.pos = float4(0, 0, 2, 1);  // beyond the far plane: the whole face is clipped
	}
	o.uv = i.uv;
	o.color = i.color;
	o.flags = i.flags;
	o.lightPair = float2(float(i.light & 0xFF), float((i.light >> 8) & 0xFF)) / 15.0;
	// Minecraft's face shading: up 1, down 0.5, north and south 0.8, west and east 0.6, blended for
	// a model's slanted faces; 0.9 without a normal.
	float3 n = Normal(i.flags);
	o.shade = dot(n, n) > 0.5 ? dot(n * n, float3(0.6, n.y > 0 ? 1.0 : 0.5, 0.8)) : 0.9;
	// Lit per corner, not per pixel: a face blends smoothly between its corners' light, so nothing
	// standing behind it can draw its shape on it (lit per pixel, blocks looked see-through).
	float2 uv;
	ScreenUv(o.world, uv);
	o.haze = SceneLight(uv);
	o.env = o.haze;
	// ... and from the side it faces: the picture a little way out along its normal, so a face
	// turned to a pink sign takes the sign's light, and the face turned away the street's.
	if (dot(n, n) > 0.5 && sceneParams.w > 0 && ScreenUv(o.world + n * sceneParams.w, uv)) {
		o.env = lerp(o.env, SceneLight(uv), 0.6);
	}
	return o;
}
// Cyberpunk's depth at this pixel as a distance along the view axis, in metres.
float SceneViewZ(float2 pixel) {
	float d = sceneDepth.Load(int3(pixel * depthMap.xy + depthMap.zw, 0));
	float n = depthParams.x, f = depthParams.y;
	if (depthParams.z > 0.5) {
		return (f <= n || f > 1e7) ? n / max(d, 1e-7) : n * f / (n + d * (f - n));
	}
	return n * f / max(f - d * (f - n), 1e-7);
}
// The light around this pixel in Cyberpunk's own finished picture, from a tiny copy of it
// (PSDown, 80x45: each texel the mean of a 32x32-pixel patch) read with a tent of bilinear taps.
// Sharp taps of the full picture carried a ghost of what stood behind a block onto it, and the
// block looked see-through.
float3 SceneLight(float2 uv) {
	float2 texel = float2(2.0 / 80.0, 2.0 / 45.0);  // a two-texel tent: about a sixth of the screen
	float3 sum = sceneColor.SampleLevel(smooth, uv, 0).rgb * 4.0;
	sum += sceneColor.SampleLevel(smooth, uv + float2(texel.x, 0), 0).rgb * 2.0;
	sum += sceneColor.SampleLevel(smooth, uv - float2(texel.x, 0), 0).rgb * 2.0;
	sum += sceneColor.SampleLevel(smooth, uv + float2(0, texel.y), 0).rgb * 2.0;
	sum += sceneColor.SampleLevel(smooth, uv - float2(0, texel.y), 0).rgb * 2.0;
	sum += sceneColor.SampleLevel(smooth, uv + texel, 0).rgb;
	sum += sceneColor.SampleLevel(smooth, uv - texel, 0).rgb;
	sum += sceneColor.SampleLevel(smooth, uv + float2(texel.x, -texel.y), 0).rgb;
	sum += sceneColor.SampleLevel(smooth, uv + float2(-texel.x, texel.y), 0).rgb;
	return sum / 16.0;
}
// The downsample pass: a fullscreen triangle into the 80x45 target, reading the full picture (t3).
float4 VSFull(uint id : SV_VertexID) : SV_Position {
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PSDown(float4 pos : SV_Position) : SV_Target {
	float2 size;
	sceneColor.GetDimensions(size.x, size.y);
	float2 cell = size / float2(80.0, 45.0);
	float2 base = floor(pos.xy) * cell;
	float3 sum = 0;
	[unroll] for (int y = 0; y < 8; ++y) {
		[unroll] for (int x = 0; x < 8; ++x) {
			sum += sceneColor.SampleLevel(smooth, (base + (float2(x, y) + 0.5) * cell / 8.0) / size, 0).rgb;
		}
	}
	return float4(sum / 64.0, 1);
}
float4 PSMain(VSOut i) : SV_Target {
	if (depthParams.w > 0.5) {
		float viewZ = dot(i.world - eye.xyz, forward.xyz) * eye.w;  // metres, as the game's depth is
		if (viewZ > SceneViewZ(i.pos.xy) * 1.003 + 0.03) discard;  // something of the game's is in front
	}
	// Bit 3: full detail. Particles and entities cut small pieces out of atlas sprites (a crumb of
	// a block, a shard of a potion bottle), which the atlas's smaller mips blur into their
	// neighbours, so they're sampled at full detail.
	float4 tex = (i.flags & 8) ? atlas.SampleLevel(samp, i.uv, 0) : atlas.Sample(samp, i.uv);
	float4 albedo = tex * i.color;
	if (drawPass < 0.5 && albedo.a < 0.5) discard;   // opaque and cutout pass: alpha test
	float3 lit;
	float  shade = i.shade;
	if (sceneParams.x > 0.5) {
		// Cyberpunk's light rather than Minecraft's: the brightness and colour of its picture around
		// and beside the face (VSMain), as much of the colour as tone.x keeps. Read sharp, the colour
		// of what stood behind a block made it look see-through; smoothed per corner it reads as light.
		float3 around = i.env;
		float3 tint = lerp(dot(around, float3(0.2126, 0.7152, 0.0722)).xxx, around, tone.x);
		// Minecraft's sky light only says how shut in the face is: inside a sealed build the city's
		// light doesn't reach it.
		float open = lerp(0.15, 1.0, i.lightPair.y);
		lit = saturate(tint * sceneParams.y * open + sceneParams.z);
		// Torches and lamps among the blocks, warm, falling off as Minecraft's light does.
		lit += float3(1.0, 0.78, 0.52) * (i.lightPair.x * i.lightPair.x * tone.w);
		shade = lerp(1.0, shade, tone.y);
	} else {
		lit = lerp(0.08, 1.0, max(i.lightPair.x, i.lightPair.y * light.x)).xxx;
	}
	float3 color = albedo.rgb * lit * shade;
	// What gives light glows by itself: light blocks (bit 2), and whatever Minecraft draws at full
	// block light (flames, a blaze, lit TNT's flash).
	if ((i.flags & 4) != 0 || i.lightPair.x > 0.99) {
		color = max(color, albedo.rgb * i.lightPair.x);
	}
	// Further off, the block fades into the picture behind it, as Night City's haze takes its own.
	if (sceneParams.x > 0.5 && tone.z > 0) {
		color = lerp(color, i.haze, (1.0 - exp(-length(i.world - eye.xyz) * eye.w * tone.z)) * 0.85);
	}
	// Drawn over nothing (Mix puts them in the frame): what the opaque pass keeps covers its pixel.
	return float4(color, drawPass < 0.5 ? 1.0 : albedo.a);
}
)";

		// The blocks, drawn over nothing with premultiplied alpha, into Cyberpunk's frame. Under its
		// HUD when the game handed frame generation its picture without the HUD (or the HUD alone):
		// the frame is the HUD over that picture, so the blocks go over the picture, the HUD over them.
		constexpr char kMixShader[] = R"(
cbuffer Mix : register(b0) {
	float4 given;    // x: the HUD-less picture, y: the HUD
	float4 offsets;  // xy: the HUD-less picture's texel under pixel (0, 0), zw: the HUD's
};
Texture2D scene : register(t0);    // Cyberpunk's frame, HUD and all
Texture2D blocks : register(t1);
Texture2D hudless : register(t2);
Texture2D hud : register(t3);
float4 VSFull(uint id : SV_VertexID) : SV_Position {
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PSMix(float4 pos : SV_Position) : SV_Target {
	int2 p = int2(pos.xy);
	float4 w = blocks.Load(int3(p, 0));
	if (w.a <= 0.0) discard;  // no block here: the frame stays as it is
	float3 b = scene.Load(int3(p, 0)).rgb;
	float3 h = given.x > 0.5 ? hudless.Load(int3(p + int2(offsets.xy), 0)).rgb : b;
	float cover = 0.0;  // how much of the HUD stands on this pixel
	if (given.y > 0.5) {
		cover = hud.Load(int3(p + int2(offsets.zw), 0)).a;
	} else if (given.x > 0.5) {
		// The frame is h * (1 - cover) + the HUD's own colour (0..cover), so the HUD covers at least
		// as much as it moved each channel against how far that channel could go that way: down
		// to 0 (h) or up to 1 (1 - h). Less leaves the HUD a negative colour, and a dark panel over a
		// dark picture turns the blocks under it the HUD's tint. A step or two is noise (dithering).
		float3 room = b < h ? h : 1.0 - saturate(h);
		float3 moved = max(abs(b - h) - 2.0 / 255.0, 0.0) / max(room, 1e-3);
		cover = saturate(max(moved.r, max(moved.g, moved.b)));
	}
	// The HUD over (the blocks over the HUD-less picture): b + (1 - cover) * (blocks over h - h).
	// Without the picture h is b, and the blocks go over the frame where the HUD doesn't cover it.
	return float4(b + (1.0 - cover) * (w.rgb - w.a * h), 1.0);
}
// Over everything, blended (the hand; the blocks when there's no copy of the frame to mix with).
float4 PSOver(float4 pos : SV_Position) : SV_Target {
	float4 w = blocks.Load(int3(int2(pos.xy), 0));
	if (w.a <= 0.0) discard;
	return w;
}
)";

		struct Params
		{
			dx::XMFLOAT4X4 viewProj;
			float          origin[3];
			float          pass;
			float          light[4];        // x: daylight 0..1
			float          eye[4];          // xyz, w: metres per block
			float          forward[4];      // xyz
			float          depthParams[4];  // near, far, inverted, enabled
			float          depthMap[4];     // scale xy, offset zw
			float          sceneParams[4];  // enabled, gain, floor, probe distance
			float          tone[4];         // colour kept, face shading kept, haze per metre, block light kept
		};
		static_assert(sizeof(Params) == 192);

		struct Section
		{
			ComPtr<ID3D12Resource> vertices;
			std::int32_t           sx{ 0 }, sy{ 0 }, sz{ 0 };
			UINT                   count{ 0 };
			UINT                   capacity{ 0 };
			UINT                   opaque{ 0 };  // vertices [0, opaque) are opaque or cutout, the rest translucent
		};

		struct State
		{
			ComPtr<ID3D12Device>         device;
			ComPtr<ID3D12RootSignature>  rootSignature;
			ComPtr<ID3D12PipelineState>  psoOpaque;
			ComPtr<ID3D12PipelineState>  psoTranslucent;
			ComPtr<ID3D12DescriptorHeap> srvHeap;
			ComPtr<ID3D12DescriptorHeap> dsvHeap;
			ComPtr<ID3D12Resource>       depth;
			// What the blocks are drawn into, over nothing, before Mix puts them in the frame.
			ComPtr<ID3D12Resource>       blocks;
			ComPtr<ID3D12RootSignature>  mixRootSignature;
			ComPtr<ID3D12PipelineState>  psoMix;   // under the game's HUD
			ComPtr<ID3D12PipelineState>  psoOver;  // over everything
			DXGI_FORMAT                  mixFormat{ DXGI_FORMAT_UNKNOWN };  // the back buffer's, which they draw into
			std::vector<ComPtr<ID3D12PipelineState>> oldPipelines;          // theirs for earlier formats
			ComPtr<ID3D12Resource>       atlas;
			ComPtr<ID3D12Resource>       atlasUpload;
			std::vector<std::uint8_t>    atlasPending;
			bool                         atlasLive{ false };  // sampled by the shader (not COPY_DEST)
			UINT                         atlasW{ 0 }, atlasH{ 0 };
			UINT                         depthW{ 0 }, depthH{ 0 };
			bool                         ready{ false };
			bool                         failed{ false };
			std::unordered_map<std::uint64_t, Section> sections;
			// Resources the GPU may still be reading (last frames' command lists), kept alive a few
			// frames past their last use. Freeing them on the spot hung the GPU (0x887a0006) as soon
			// as a placed block grew a section's mesh.
			std::deque<std::pair<std::uint64_t, ComPtr<ID3D12Resource>>> retired;
			std::uint64_t                                                frame{ 0 };
			// Face visibility goes up through this persistently mapped ring, a slice per section
			// per frame. Frames use a few KB of its 8 MB, so a slice is long done with before the
			// ring comes round to it again.
			ComPtr<ID3D12Resource> visRing;
			std::uint8_t*          visMapped{ nullptr };
			UINT64                 visOffset{ 0 };

			// Entities and particles: Minecraft's own entity renderer output (kRenScene), redrawn
			// every frame until the next one arrives, relative to sceneOrigin.
			double                         sceneOrigin[3]{};
			std::vector<proto::RenBatch>   sceneBatches;
			std::vector<proto::RenVertex>  sceneVertices;
			UINT                           sceneParticlesPeak{ 0 };  // diagnostics: most particle vertices in one scene since the last log
			// The player's own body as Minecraft draws it in third person (kRenAvatar), relative to its
			// feet; drawn at V's feet while ThirdPerson has it shown.
			std::vector<proto::RenBatch>   avatarBatches;
			std::vector<proto::RenVertex>  avatarVertices;
			// The first-person hands and held items (kRenHand), in Minecraft's view space, drawn over
			// everything else with its hand projection.
			std::vector<proto::RenBatch>   handBatches;
			std::vector<proto::RenVertex>  handVertices;
			float                          handFov{ 70.0f };
			// Entity textures (kRenTexture: skins, mobs, TNT, ...): SRV slot = id; slot 0 is the atlas.
			struct EntityTexture
			{
				ComPtr<ID3D12Resource>    texture;
				std::vector<std::uint8_t> pending;  // pixels waiting for a command list to copy them
				UINT                      width{ 0 }, height{ 0 };
				UINT                      slot{ 0 };
				bool                      live{ false };
			};
			std::unordered_map<std::uint32_t, EntityTexture> textures;
			UINT                                             srvIncrement{ 0 };
			// Descriptor slots 1.. for entity textures. A replaced texture's slot is reused only
			// once the frames that may still read its descriptor are done; one never used is free
			// at once. Waiting from frame 0 instead refused every slot to the textures that arrive
			// with the first frames (the player's skin, armour, cape), and Minecraft sends a
			// texture only once.
			std::array<bool, 256>          slotUsed{};
			std::array<std::uint64_t, 256> slotFreeFrom{};  // the frame a freed slot may be reused from
			// Animated atlas sprites' current frames (kRenAtlasRegion: water, lava, fire, portals).
			struct Region
			{
				UINT                      x, y, width, height;
				std::vector<std::uint8_t> pixels;
			};
			std::vector<Region> regionsPending;
			// This frame's generated and streamed vertices (scene, arrows, items) go up through
			// this persistently mapped ring, like the visibility ring.
			ComPtr<ID3D12Resource> dynRing;
			std::uint8_t*          dynMapped{ nullptr };
			UINT64                 dynOffset{ 0 };

			// Cyberpunk's finished picture, copied each frame before anything of ours is drawn: the
			// blocks take their light from it.
			ComPtr<ID3D12Resource> sceneColor;
			UINT                   sceneW{ 0 }, sceneH{ 0 };
			DXGI_FORMAT            sceneFormat{ DXGI_FORMAT_UNKNOWN };
			bool                   sceneLive{ false };  // copied this frame
			// ... and shrunk to 80x45 (PSDown) for the blocks to read their light from.
			ComPtr<ID3D12Resource>       sceneSmall;
			ComPtr<ID3D12DescriptorHeap> rtvHeap;  // kRtvSmall, kRtvBlocks
			ComPtr<ID3D12PipelineState>  psoDown;
			// The depth copy the depth slot describes (SceneDepth owns it).
			ID3D12Resource* depthBound{ nullptr };
			// This frame's picture without the game's HUD and the HUD alone (SceneDepth::AcquireHud),
			// and the copies their slots describe.
			SceneDepth::Hud hud;
			ID3D12Resource* hudlessBound{ nullptr };
			ID3D12Resource* uiBound{ nullptr };
		};

		State g;

		constexpr std::uint64_t kRetireFrames = 8;
		constexpr UINT64        kVisRingBytes = 8ull << 20;
		constexpr UINT64        kDynRingBytes = 64ull << 20;
		constexpr UINT64        kDynFrameMax = kDynRingBytes / 8;  // a frame never laps the GPU
		constexpr UINT          kAtlasMips = 5;                     // 16-texel tiles: 16, 8, 4, 2, 1 (Minecraft's own count)
		constexpr UINT          kSrvSlots = 256;                    // atlas, entity textures, then the frame's own
		constexpr UINT          kSceneFullSlot = kSrvSlots - 6;      // Cyberpunk's finished picture (t3 of the downsample; Mix's t0..t3 from here)
		constexpr UINT          kBlocksSlot = kSrvSlots - 5;         // the blocks, drawn over nothing
		constexpr UINT          kHudlessSlot = kSrvSlots - 4;        // the picture without the game's HUD
		constexpr UINT          kHudSlot = kSrvSlots - 3;            // the HUD alone
		constexpr UINT          kDepthSlot = kSrvSlots - 2;          // Cyberpunk's depth copy (t2)
		constexpr UINT          kSceneSlot = kSrvSlots - 1;          // the picture, 80x45 (t3 of the blocks)
		constexpr UINT          kSceneSmallW = 80, kSceneSmallH = 45;  // must match the shader's
		constexpr UINT          kRtvSmall = 0, kRtvBlocks = 1;
		// Light from Cyberpunk's picture can be brighter than white in HDR, and translucent blocks
		// need their coverage kept finely.
		constexpr DXGI_FORMAT kBlocksFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

		// Each section's face centres (section-relative, 3 floats a face) and which faces the city
		// hides: written by the render thread when meshes arrive, ray-tested on the game thread.
		struct Faces
		{
			std::int32_t              sx{ 0 }, sy{ 0 }, sz{ 0 };
			std::vector<float>        centres;
			std::vector<std::uint8_t> visible;
		};
		std::mutex                                facesMutex;
		std::unordered_map<std::uint64_t, Faces> faces;
		std::size_t                               faceCursor = 0;  // round robin over every face
		// Blocks are depth-tested against Cyberpunk's depth this frame: the face rays can rest.
		std::atomic<bool> depthActive{ false };
		// The hand is drawn here (kCyberDrawsHand): Minecraft leaves it out of the overlay.
		std::atomic<bool> drawsHand{ false };

		void Retire(ComPtr<ID3D12Resource>& a_resource)
		{
			if (a_resource) {
				g.retired.emplace_back(g.frame, std::move(a_resource));
			}
			a_resource.Reset();
		}

		void RetireSection(std::uint64_t a_key)
		{
			if (const auto found = g.sections.find(a_key); found != g.sections.end()) {
				Retire(found->second.vertices);
				g.sections.erase(found);
			}
			std::lock_guard lock(facesMutex);
			faces.erase(a_key);
		}

		// A face is 6 vertices (Minecraft's quads as two triangles, 0 1 2 0 2 3): its centre is
		// the mean of the quad's corners, vertices 0, 1, 2 and 5. New faces start visible.
		void SetFaces(std::uint64_t a_key, const Section& a_section, const proto::RenVertex* a_vertices, UINT a_count)
		{
			Faces f;
			f.sx = a_section.sx;
			f.sy = a_section.sy;
			f.sz = a_section.sz;
			const UINT count = a_count / 6;
			f.centres.resize(std::size_t(count) * 3);
			f.visible.assign(count, 1);
			for (UINT i = 0; i < count; ++i) {
				const auto* v = a_vertices + std::size_t(i) * 6;
				f.centres[i * 3 + 0] = (v[0].x + v[1].x + v[2].x + v[5].x) * 0.25f;
				f.centres[i * 3 + 1] = (v[0].y + v[1].y + v[2].y + v[5].y) * 0.25f;
				f.centres[i * 3 + 2] = (v[0].z + v[1].z + v[2].z + v[5].z) * 0.25f;
			}
			std::lock_guard lock(facesMutex);
			faces[a_key] = std::move(f);
		}

		bool EnsureVisRing()
		{
			if (g.visRing) {
				return true;
			}
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_UPLOAD;
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			desc.Width = kVisRingBytes;
			desc.Height = 1;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			void* mapped = nullptr;
			if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
					IID_PPV_ARGS(&g.visRing))) ||
				FAILED(g.visRing->Map(0, nullptr, &mapped))) {
				g.visRing.Reset();
				logger::error("world: visibility ring failed");
				return false;
			}
			g.visMapped = static_cast<std::uint8_t*>(mapped);
			return true;
		}

		bool EnsureDynRing()
		{
			if (g.dynRing) {
				return true;
			}
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_UPLOAD;
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			desc.Width = kDynRingBytes;
			desc.Height = 1;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			void* mapped = nullptr;
			if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
					IID_PPV_ARGS(&g.dynRing))) ||
				FAILED(g.dynRing->Map(0, nullptr, &mapped))) {
				g.dynRing.Reset();
				logger::error("world: vertex ring failed");
				return false;
			}
			g.dynMapped = static_cast<std::uint8_t*>(mapped);
			return true;
		}

		// Copies a_bytes into this frame's part of the vertex ring; 0 when it doesn't fit.
		D3D12_GPU_VIRTUAL_ADDRESS DynUpload(const void* a_data, UINT64 a_bytes, UINT64& a_frameUsed)
		{
			const UINT64 bytes = (a_bytes + 255) & ~UINT64(255);
			if (bytes == 0 || a_frameUsed + bytes > kDynFrameMax) {
				return 0;
			}
			if (g.dynOffset + bytes > kDynRingBytes) {
				g.dynOffset = 0;
			}
			std::memcpy(g.dynMapped + g.dynOffset, a_data, a_bytes);
			const auto address = g.dynRing->GetGPUVirtualAddress() + g.dynOffset;
			g.dynOffset += bytes;
			a_frameUsed += bytes;
			return address;
		}

		// A free descriptor slot for an entity texture, or 0 when none is.
		UINT AllocSlot()
		{
			for (UINT slot = 1; slot < kSceneFullSlot; ++slot) {
				if (!g.slotUsed[slot] && g.slotFreeFrom[slot] <= g.frame) {
					g.slotUsed[slot] = true;
					return slot;
				}
			}
			return 0;
		}

		void FreeSlot(UINT a_slot)
		{
			if (a_slot > 0 && a_slot < kSrvSlots) {
				g.slotUsed[a_slot] = false;
				g.slotFreeFrom[a_slot] = g.frame + kRetireFrames;
			}
		}

		D3D12_CPU_DESCRIPTOR_HANDLE CpuSlot(UINT a_slot)
		{
			auto handle = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
			handle.ptr += SIZE_T(a_slot) * g.srvIncrement;
			return handle;
		}

		D3D12_GPU_DESCRIPTOR_HANDLE SrvSlot(UINT a_slot)
		{
			auto handle = g.srvHeap->GetGPUDescriptorHandleForHeapStart();
			handle.ptr += UINT64(a_slot) * g.srvIncrement;
			return handle;
		}

		// A texture copy from pixels (rows of a_width * 4 bytes) into a_dst at (a_x, a_y), through a
		// fresh upload buffer that is retired afterwards. The caller handles the barriers.
		bool CopyPixels(ID3D12GraphicsCommandList* a_commandList, ID3D12Resource* a_dst, UINT a_x, UINT a_y, UINT a_width, UINT a_height,
			const std::uint8_t* a_pixels)
		{
			const UINT             rowBytes = a_width * 4;
			const UINT             rowPitch = (rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
			ComPtr<ID3D12Resource> upload;
			D3D12_HEAP_PROPERTIES  heap{};
			heap.Type = D3D12_HEAP_TYPE_UPLOAD;
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			desc.Width = UINT64(rowPitch) * a_height;
			desc.Height = 1;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			void* mapped = nullptr;
			if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))) ||
				FAILED(upload->Map(0, nullptr, &mapped))) {
				return false;
			}
			for (UINT y = 0; y < a_height; ++y) {
				std::memcpy(static_cast<std::uint8_t*>(mapped) + std::size_t(y) * rowPitch, a_pixels + std::size_t(y) * rowBytes, rowBytes);
			}
			upload->Unmap(0, nullptr);
			D3D12_TEXTURE_COPY_LOCATION dst{};
			dst.pResource = a_dst;
			dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			D3D12_TEXTURE_COPY_LOCATION src{};
			src.pResource = upload.Get();
			src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			src.PlacedFootprint.Footprint.Width = a_width;
			src.PlacedFootprint.Footprint.Height = a_height;
			src.PlacedFootprint.Footprint.Depth = 1;
			src.PlacedFootprint.Footprint.RowPitch = rowPitch;
			a_commandList->CopyTextureRegion(&dst, a_x, a_y, 0, &src, nullptr);
			Retire(upload);
			return true;
		}

		void Transition(ID3D12GraphicsCommandList* a_commandList, ID3D12Resource* a_resource, D3D12_RESOURCE_STATES a_from, D3D12_RESOURCE_STATES a_to)
		{
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = a_resource;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = a_from;
			barrier.Transition.StateAfter = a_to;
			a_commandList->ResourceBarrier(1, &barrier);
		}

		// Pending entity textures and animated sprite frames go up before anything samples them.
		void UploadTextures(ID3D12GraphicsCommandList* a_commandList)
		{
			for (auto& [id, t] : g.textures) {
				if (t.pending.empty() || !t.texture) {
					continue;
				}
				if (t.live) {
					Transition(a_commandList, t.texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
				}
				CopyPixels(a_commandList, t.texture.Get(), 0, 0, t.width, t.height, t.pending.data());
				Transition(a_commandList, t.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				t.live = true;
				t.pending.clear();
			}
			if (g.regionsPending.empty() || !g.atlasLive) {
				return;
			}
			Transition(a_commandList, g.atlas.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
			for (const auto& r : g.regionsPending) {
				if (r.x + r.width <= g.atlasW && r.y + r.height <= g.atlasH) {
					CopyPixels(a_commandList, g.atlas.Get(), r.x, r.y, r.width, r.height, r.pixels.data());
				}
			}
			Transition(a_commandList, g.atlas.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			g.regionsPending.clear();
		}

		// ---- arrows, dropped items and block cracks (WorldEntities), built here every frame ------
		// Minecraft sends a record per thing and the host
		// makes its geometry, in Minecraft space relative to an origin near the camera.
		constexpr std::uint32_t kFlagCutout = 1;
		constexpr std::uint32_t kFlagTranslucent = 2;
		constexpr std::uint32_t kFullSkyLight = 15u << 8;
		constexpr float         kPi = 3.14159265f;

		std::vector<proto::RenVertex> entityVerts;  // cutout: arrows, items, dropped blocks
		std::vector<proto::RenVertex> crackVerts;   // translucent: block-breaking cracks

		void Quad(std::vector<proto::RenVertex>& a_out, const float a_p[4][3], const float a_uv[4], std::uint32_t a_color, std::uint32_t a_flags)
		{
			const float uv[4][2] = { { a_uv[0], a_uv[1] }, { a_uv[2], a_uv[1] }, { a_uv[2], a_uv[3] }, { a_uv[0], a_uv[3] } };
			for (int k : { 0, 1, 2, 0, 2, 3 }) {
				a_out.push_back({ a_p[k][0], a_p[k][1], a_p[k][2], uv[k][0], uv[k][1], a_color, kFullSkyLight, a_flags });
			}
		}

		// A brightness times a tint, as a vertex colour.
		std::uint32_t Shade(float a_shade, std::uint32_t a_tint = 0)
		{
			float r = a_shade, gr = a_shade, b = a_shade;
			if (a_tint) {
				r *= float(a_tint & 0xFF) / 255.0f;
				gr *= float((a_tint >> 8) & 0xFF) / 255.0f;
				b *= float((a_tint >> 16) & 0xFF) / 255.0f;
			}
			return 0xFF000000u | (std::uint32_t(b * 255.0f) << 16) | (std::uint32_t(gr * 255.0f) << 8) | std::uint32_t(r * 255.0f);
		}

		// An axis-aligned box (rotated a_yaw about its vertical centre line), sides a_side, top
		// a_top, bottom a_bottom. Minecraft's fixed face shading, since nothing else lights it.
		void Box(std::vector<proto::RenVertex>& a_out, const float a_min[3], const float a_size[3], float a_yaw, const float a_side[4],
			const float a_top[4], const float a_bottom[4], std::uint32_t a_topTint, std::uint32_t a_flags, bool a_shaded)
		{
			const float cx = a_min[0] + a_size[0] * 0.5f, cz = a_min[2] + a_size[2] * 0.5f;
			const float c = std::cos(a_yaw), s = std::sin(a_yaw);
			auto        corner = [&](int a_i, float a_outCorner[3]) {
				const float lx = ((a_i & 1) ? 0.5f : -0.5f) * a_size[0], lz = ((a_i & 4) ? 0.5f : -0.5f) * a_size[2];
				a_outCorner[0] = cx + lx * c - lz * s;
				a_outCorner[1] = a_min[1] + ((a_i & 2) ? a_size[1] : 0.0f);
				a_outCorner[2] = cz + lx * s + lz * c;
			};
			// corner bits: 1 = +x, 2 = +y, 4 = +z; each face TL, TR, BR, BL seen from outside
			static constexpr int   kFaces[6][4] = { { 6, 7, 5, 4 }, { 3, 2, 0, 1 }, { 7, 3, 1, 5 }, { 2, 6, 4, 0 }, { 2, 3, 7, 6 }, { 4, 5, 1, 0 } };
			static constexpr float kFaceShade[6] = { 0.8f, 0.8f, 0.6f, 0.6f, 1.0f, 0.5f };  // Minecraft's: sides, top, bottom
			for (int f = 0; f < 6; ++f) {
				float p[4][3];
				for (int k = 0; k < 4; ++k) {
					corner(kFaces[f][k], p[k]);
				}
				const float*        uv = f == 4 ? a_top : f == 5 ? a_bottom : a_side;
				const std::uint32_t color = a_shaded ? Shade(kFaceShade[f], f == 4 ? a_topTint : 0) : 0xFFFFFFFFu;
				Quad(a_out, p, uv, color, a_flags);
			}
		}

		// Minecraft's arrow (or a trident) at a position, flying along d (unit length). Around that
		// axis, s is the horizontal side and u the "up"; the fins sit at 45 degrees between them.
		void BuildArrow(float px, float py, float pz, const float d[3], const float* a_uvSide, const float* a_uvBack, bool a_trident)
		{
			float s[3] = { d[2], 0.0f, -d[0] };
			float sl = std::sqrt(s[0] * s[0] + s[2] * s[2]);
			if (sl < 1e-3f) {
				s[0] = 1.0f, s[2] = 0.0f, sl = 1.0f;
			}
			s[0] /= sl, s[2] /= sl;
			const float     u[3] = { s[1] * d[2] - s[2] * d[1], s[2] * d[0] - s[0] * d[2], s[0] * d[1] - s[1] * d[0] };
			constexpr float r = 0.70710678f;
			const float     fins[2][3] = { { (u[0] + s[0]) * r, (u[1] + s[1]) * r, (u[2] + s[2]) * r }, { (u[0] - s[0]) * r, (u[1] - s[1]) * r, (u[2] - s[2]) * r } };
			auto            at = [&](float a_along, const float* a_q, float a_side, const float* a_q2, float a_side2, float a_outP[3]) {
				for (int k = 0; k < 3; ++k) {
					a_outP[k] = (k == 0 ? px : k == 1 ? py : pz) + d[k] * a_along + a_q[k] * a_side + (a_q2 ? a_q2[k] * a_side2 : 0.0f);
				}
			};
			if (!a_trident) {
				// Minecraft's ArrowModel (1/16 block units, scaled 0.9): two fins 16 long and 4 wide
				// from x -12 (fletching) to +4 (head), and a 4x4 back plate at x -11.
				constexpr float k = 0.9f / 16.0f;
				for (const auto& q : fins) {
					float p[4][3];
					at(-12 * k, q, -2 * k, nullptr, 0, p[0]);
					at(4 * k, q, -2 * k, nullptr, 0, p[1]);
					at(4 * k, q, 2 * k, nullptr, 0, p[2]);
					at(-12 * k, q, 2 * k, nullptr, 0, p[3]);
					Quad(entityVerts, p, a_uvSide, 0xFFFFFFFFu, kFlagCutout);
				}
				float p[4][3];
				at(-11 * k, fins[0], -2 * k, fins[1], -2 * k, p[0]);
				at(-11 * k, fins[0], 2 * k, fins[1], -2 * k, p[1]);
				at(-11 * k, fins[0], 2 * k, fins[1], 2 * k, p[2]);
				at(-11 * k, fins[0], -2 * k, fins[1], 2 * k, p[3]);
				Quad(entityVerts, p, a_uvBack, 0xFFFFFFFFu, kFlagCutout);
			} else {
				// Tridents: the item icon, whose diagonal runs handle (bottom-left) to tip (top-right).
				constexpr float h = 0.9f;
				for (const auto& q : fins) {
					float p[4][3];
					at(0, q, h, nullptr, 0, p[0]);
					at(h, q, 0, nullptr, 0, p[1]);
					at(0, q, -h, nullptr, 0, p[2]);
					at(-h, q, 0, nullptr, 0, p[3]);
					Quad(entityVerts, p, a_uvSide, 0xFFFFFFFFu, kFlagCutout);
				}
			}
		}

		// a_right, a_up: the camera's axes (unit), for sprites that face it.
		void BuildWorldEntities(const proto::WorldEntities& a_entities, const double a_o[3], const float a_right[3], const float a_up[3])
		{
			entityVerts.clear();
			crackVerts.clear();
			const std::uint32_t count = std::min(a_entities.count, proto::kMaxWorldEntities);
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto& e = a_entities.entities[i];
				const float px = float(e.x - a_o[0]), py = float(e.y - a_o[1]), pz = float(e.z - a_o[2]);
				if (e.kind == proto::kWeBlock) {
					// A dropped block: a small cube spinning about its centre.
					const float s = e.scale;
					const float mn[3] = { px - s * 0.5f, py - s * 0.5f, pz - s * 0.5f };
					const float sz[3] = { s, s, s };
					Box(entityVerts, mn, sz, e.yaw * kPi / 180.0f, e.uv[0], e.uv[1], e.uv[2], e.tint, kFlagCutout, true);
				} else if (e.kind == proto::kWeCrack) {
					const float mn[3] = { px, py, pz };
					Box(crackVerts, mn, e.ext, 0.0f, e.uv[0], e.uv[0], e.uv[0], 0, kFlagTranslucent, false);
				} else if (e.kind == proto::kWeArrow || e.kind == proto::kWeTrident) {
					// Minecraft arrows face (sin yaw, sin pitch, cos yaw).
					const float yaw = e.yaw * kPi / 180.0f, pitch = e.pitch * kPi / 180.0f;
					const float d[3] = { std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch) };
					BuildArrow(px, py, pz, d, e.uv[0], e.uv[1], e.kind == proto::kWeTrident);
				} else if (e.kind == proto::kWeItem) {
					// A sprite facing the camera, as Minecraft draws thrown items (pearls, snowballs,
					// eggs). Turning it about the vertical showed it edge-on.
					const float half = e.scale * 0.5f;
					const float rx = a_right[0] * half, ry = a_right[1] * half, rz = a_right[2] * half;
					const float ux = a_up[0] * half, uy = a_up[1] * half, uz = a_up[2] * half;
					const float p[4][3] = {
						{ px - rx + ux, py - ry + uy, pz - rz + uz },
						{ px + rx + ux, py + ry + uy, pz + rz + uz },
						{ px + rx - ux, py + ry - uy, pz + rz - uz },
						{ px - rx - ux, py - ry - uy, pz - rz - uz },
					};
					Quad(entityVerts, p, e.uv[0], 0xFFFFFFFFu, kFlagCutout);
				}
			}
		}

		// This frame's visibility slice for a_faces of a section's faces from a_firstFace on (all
		// visible when the section has no face record), as a GPU address for the root SRV.
		D3D12_GPU_VIRTUAL_ADDRESS VisibilitySlice(std::uint64_t a_key, UINT a_faces, UINT a_firstFace = 0)
		{
			const UINT64 bytes = std::max<UINT64>(16, (UINT64(a_faces) + 15) & ~UINT64(15));
			if (g.visOffset + bytes > kVisRingBytes) {
				g.visOffset = 0;
			}
			auto* dst = g.visMapped + g.visOffset;
			std::memset(dst, 1, bytes);
			{
				std::lock_guard lock(facesMutex);
				if (const auto found = faces.find(a_key); found != faces.end() && a_firstFace < found->second.visible.size()) {
					const auto& visible = found->second.visible;
					std::memcpy(dst, visible.data() + a_firstFace, std::min<std::size_t>(visible.size() - a_firstFace, bytes));
				}
			}
			const auto address = g.visRing->GetGPUVirtualAddress() + g.visOffset;
			g.visOffset += bytes;
			return address;
		}

		std::uint64_t SectionKey(std::int32_t a_sx, std::int32_t a_sy, std::int32_t a_sz)
		{
			constexpr std::uint64_t mask = (1ull << 21) - 1;
			return ((static_cast<std::uint64_t>(a_sx) & mask) << 42) | ((static_cast<std::uint64_t>(a_sy) & mask) << 21) |
			       (static_cast<std::uint64_t>(a_sz) & mask);
		}

		template <std::size_t N>
		ComPtr<ID3DBlob> Compile(const char (&a_source)[N], const char* a_entry, const char* a_target)
		{
			ComPtr<ID3DBlob> code;
			ComPtr<ID3DBlob> errors;
			if (FAILED(::D3DCompile(a_source, N - 1, "CyberCraftWorld", nullptr, nullptr, a_entry, a_target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code,
					&errors))) {
				logger::error("world: shader {} failed: {}", a_entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
				return nullptr;
			}
			return code;
		}

		ComPtr<ID3DBlob> Compile(const char* a_entry, const char* a_target)
		{
			return Compile(kShader, a_entry, a_target);
		}

		D3D12_CPU_DESCRIPTOR_HANDLE RtvSlot(UINT a_slot)
		{
			auto handle = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
			handle.ptr += SIZE_T(a_slot) * g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
			return handle;
		}

		// A view of a_texture in slot a_slot, or a null one without it.
		void SetView(UINT a_slot, ID3D12Resource* a_texture, DXGI_FORMAT a_format)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = a_texture ? a_format : DXGI_FORMAT_R8G8B8A8_UNORM;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = 1;
			g.device->CreateShaderResourceView(a_texture, &srv, CpuSlot(a_slot));
		}

		// The blocks' depth buffer and the target they're drawn into, the back buffer's size.
		bool CreateTargets(UINT a_width, UINT a_height)
		{
			if (g.depth && g.blocks && g.depthW == a_width && g.depthH == a_height) {
				return true;
			}
			Retire(g.depth);
			Retire(g.blocks);
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = a_width;
			desc.Height = a_height;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = DXGI_FORMAT_D32_FLOAT;
			desc.SampleDesc.Count = 1;
			desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
			D3D12_CLEAR_VALUE clear{};
			clear.Format = DXGI_FORMAT_D32_FLOAT;
			clear.DepthStencil.Depth = 1.0f;
			if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
					IID_PPV_ARGS(&g.depth)))) {
				return false;
			}
			D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
			dsv.Format = DXGI_FORMAT_D32_FLOAT;
			dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
			g.device->CreateDepthStencilView(g.depth.Get(), &dsv, g.dsvHeap->GetCPUDescriptorHandleForHeapStart());

			desc.Format = kBlocksFormat;
			desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			clear = {};
			clear.Format = kBlocksFormat;  // transparent black
			if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
					IID_PPV_ARGS(&g.blocks)))) {
				return false;
			}
			g.device->CreateRenderTargetView(g.blocks.Get(), nullptr, RtvSlot(kRtvBlocks));
			SetView(kBlocksSlot, g.blocks.Get(), kBlocksFormat);
			g.depthW = a_width;
			g.depthH = a_height;
			return true;
		}

		bool CreatePipelines()
		{
			auto vs = Compile("VSMain", "vs_5_0");
			auto ps = Compile("PSMain", "ps_5_0");
			if (!vs || !ps) {
				return false;
			}

			D3D12_DESCRIPTOR_RANGE range{};
			range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			range.NumDescriptors = 1;
			D3D12_DESCRIPTOR_RANGE depthRange{};
			depthRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			depthRange.NumDescriptors = 1;
			depthRange.BaseShaderRegister = 2;
			D3D12_DESCRIPTOR_RANGE sceneRange{};
			sceneRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			sceneRange.NumDescriptors = 1;
			sceneRange.BaseShaderRegister = 3;
			D3D12_ROOT_PARAMETER params[5]{};
			params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			params[0].DescriptorTable.NumDescriptorRanges = 1;
			params[0].DescriptorTable.pDescriptorRanges = &range;
			params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			params[1].Constants.Num32BitValues = sizeof(Params) / 4;
			// The section's face visibility (t1), a raw buffer in the visibility ring.
			params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
			params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
			params[2].Descriptor.ShaderRegister = 1;
			// Cyberpunk's depth (t2) and its finished picture (t3).
			params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			params[3].DescriptorTable.NumDescriptorRanges = 1;
			params[3].DescriptorTable.pDescriptorRanges = &depthRange;
			params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;  // blocks read it per corner (VSMain)
			params[4].DescriptorTable.NumDescriptorRanges = 1;
			params[4].DescriptorTable.pDescriptorRanges = &sceneRange;

			D3D12_STATIC_SAMPLER_DESC samplers[2]{};
			samplers[0].Filter = D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;  // pixel art up close, mips blended far away
			samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
			samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			samplers[1] = samplers[0];
			samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;  // the scene picture, smoothly
			samplers[1].ShaderRegister = 1;
			samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

			D3D12_ROOT_SIGNATURE_DESC rootDesc{};
			rootDesc.NumParameters = 5;
			rootDesc.pParameters = params;
			rootDesc.NumStaticSamplers = 2;
			rootDesc.pStaticSamplers = samplers;
			rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

			ComPtr<ID3DBlob> blob;
			ComPtr<ID3DBlob> errors;
			if (FAILED(::D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
				FAILED(g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.rootSignature)))) {
				logger::error("world: root signature failed: {}", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
				return false;
			}

			// Matches proto::RenVertex.
			const D3D12_INPUT_ELEMENT_DESC layout[]{
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 1, DXGI_FORMAT_R32_UINT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 2, DXGI_FORMAT_R32_UINT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			};

			D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
			pso.pRootSignature = g.rootSignature.Get();
			pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
			pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
			pso.InputLayout = { layout, static_cast<UINT>(std::size(layout)) };
			pso.SampleMask = UINT_MAX;
			pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			pso.NumRenderTargets = 1;
			pso.RTVFormats[0] = kBlocksFormat;
			pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
			pso.SampleDesc.Count = 1;
			pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			// Minecraft's winding comes from a right-handed space; rather than guess how it lands
			// after the handedness change, draw both sides.
			pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			pso.RasterizerState.DepthClipEnable = TRUE;
			pso.DepthStencilState.DepthEnable = TRUE;
			pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
			pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
			pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			if (FAILED(g.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g.psoOpaque)))) {
				return false;
			}

			// Glass, water, ice: blended, and no depth writes so they do not hide each other.
			auto& blend = pso.BlendState.RenderTarget[0];
			blend.BlendEnable = TRUE;
			blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
			blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
			blend.BlendOp = D3D12_BLEND_OP_ADD;
			blend.SrcBlendAlpha = D3D12_BLEND_ONE;
			blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
			blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
			if (FAILED(g.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g.psoTranslucent)))) {
				return false;
			}

			// The scene picture's downsample: a fullscreen triangle, no vertices, no depth.
			auto vsFull = Compile("VSFull", "vs_5_0");
			auto psDown = Compile("PSDown", "ps_5_0");
			if (!vsFull || !psDown) {
				return false;
			}
			D3D12_GRAPHICS_PIPELINE_STATE_DESC down{};
			down.pRootSignature = g.rootSignature.Get();
			down.VS = { vsFull->GetBufferPointer(), vsFull->GetBufferSize() };
			down.PS = { psDown->GetBufferPointer(), psDown->GetBufferSize() };
			down.SampleMask = UINT_MAX;
			down.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			down.NumRenderTargets = 1;
			down.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
			down.SampleDesc.Count = 1;
			down.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			down.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			down.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			return SUCCEEDED(g.device->CreateGraphicsPipelineState(&down, IID_PPV_ARGS(&g.psoDown)));
		}

		// Mix and Over, drawing into a back buffer of a_format: Cyberpunk's frame, the blocks, the
		// HUD-less picture and the HUD (t0..t3, a table from kSceneFullSlot), and Mix's constants.
		bool CreateMix(DXGI_FORMAT a_format)
		{
			if (!g.mixRootSignature) {
				D3D12_DESCRIPTOR_RANGE range{};
				range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
				range.NumDescriptors = 4;
				D3D12_ROOT_PARAMETER params[2]{};
				params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
				params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
				params[0].DescriptorTable.NumDescriptorRanges = 1;
				params[0].DescriptorTable.pDescriptorRanges = &range;
				params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
				params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
				params[1].Constants.Num32BitValues = 8;
				D3D12_ROOT_SIGNATURE_DESC rootDesc{};
				rootDesc.NumParameters = 2;
				rootDesc.pParameters = params;
				ComPtr<ID3DBlob> blob;
				ComPtr<ID3DBlob> errors;
				if (FAILED(::D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
					FAILED(g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.mixRootSignature)))) {
					logger::error("world: mix root signature failed: {}", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
					return false;
				}
			}
			auto vs = Compile(kMixShader, "VSFull", "vs_5_0");
			auto psMix = Compile(kMixShader, "PSMix", "ps_5_0");
			auto psOver = Compile(kMixShader, "PSOver", "ps_5_0");
			if (!vs || !psMix || !psOver) {
				return false;
			}
			D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
			pso.pRootSignature = g.mixRootSignature.Get();
			pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
			pso.PS = { psMix->GetBufferPointer(), psMix->GetBufferSize() };
			pso.SampleMask = UINT_MAX;
			pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			pso.NumRenderTargets = 1;
			pso.RTVFormats[0] = a_format;
			pso.SampleDesc.Count = 1;
			pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			// The back buffer's alpha is the game's.
			auto& blend = pso.BlendState.RenderTarget[0];
			blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
			// The last frames may still be drawing with the old ones; a format change is rare enough
			// to keep them for good.
			if (g.psoMix) {
				g.oldPipelines.push_back(std::move(g.psoMix));
			}
			if (g.psoOver) {
				g.oldPipelines.push_back(std::move(g.psoOver));
			}
			if (FAILED(g.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g.psoMix)))) {
				return false;
			}
			pso.PS = { psOver->GetBufferPointer(), psOver->GetBufferSize() };
			blend.BlendEnable = TRUE;
			blend.SrcBlend = D3D12_BLEND_ONE;  // premultiplied
			blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
			blend.BlendOp = D3D12_BLEND_OP_ADD;
			blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
			blend.DestBlendAlpha = D3D12_BLEND_ONE;
			blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			if (FAILED(g.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g.psoOver)))) {
				g.psoMix.Reset();
				return false;
			}
			return true;
		}

		bool IsFloat(DXGI_FORMAT a_format)
		{
			return a_format == DXGI_FORMAT_R16G16B16A16_FLOAT || a_format == DXGI_FORMAT_R11G11B10_FLOAT || a_format == DXGI_FORMAT_R32G32B32A32_FLOAT;
		}

		// This frame's HUD-less picture and HUD (SceneDepth::AcquireHud), their copies made in
		// a_commandList, into their slots; their slots null without them. Logs when that changes.
		void BindHud(ID3D12GraphicsCommandList* a_commandList, UINT a_width, UINT a_height, DXGI_FORMAT a_format)
		{
			auto& hud = g.hud;
			SceneDepth::AcquireHud(a_commandList, a_width, a_height, hud);
			// Compared with the back buffer pixel by pixel: a float picture beside an 8- or 10-bit
			// back buffer is likely linear where the back buffer is encoded, and would read as all HUD.
			static bool warnedFormat = false;
			if (hud.hudless && IsFloat(hud.hudlessFormat) != IsFloat(a_format)) {
				if (!warnedFormat) {
					warnedFormat = true;
					logger::warn("world: the game's HUD-less picture is format {} beside a format {} back buffer; not used", int(hud.hudlessFormat), int(a_format));
				}
				hud.hudless = nullptr;
			}
			if (hud.hudless != g.hudlessBound) {
				SetView(kHudlessSlot, hud.hudless, hud.hudlessFormat);
				g.hudlessBound = hud.hudless;
			}
			if (hud.ui != g.uiBound) {
				SetView(kHudSlot, hud.ui, hud.uiFormat);
				g.uiBound = hud.ui;
			}

			// Frame generation on or off, with a little patience: a frame or two without tags isn't news.
			static int  shown = -1;
			static int  pending = -1;
			static UINT pendingFrames = 0;
			const int   now = hud.ui ? 2 : hud.hudless ? 1 : 0;
			pendingFrames = now == pending ? pendingFrames + 1 : 0;
			pending = now;
			if (now != shown && pendingFrames >= 30) {
				shown = now;
				if (now == 2) {
					logger::info("world: blocks go under the game's HUD (the HUD frame generation is handed)");
				} else if (now == 1) {
					logger::info("world: blocks go under the game's HUD (told apart from the picture frame generation is handed without it)");
				} else {
					logger::info("world: blocks are drawn over the game's HUD: the game hands over its HUD-less picture only with frame generation on");
				}
			}
		}

		// Starts a pass into the blocks target: cleared to nothing, the blocks' own depth cleared too.
		void BeginBlocks(ID3D12GraphicsCommandList* a_commandList, D3D12_CPU_DESCRIPTOR_HANDLE a_dsv)
		{
			static constexpr float kNothing[4]{};
			const auto             rtv = RtvSlot(kRtvBlocks);
			Transition(a_commandList, g.blocks.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			a_commandList->ClearRenderTargetView(rtv, kNothing, 0, nullptr);
			a_commandList->ClearDepthStencilView(a_dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
			a_commandList->OMSetRenderTargets(1, &rtv, FALSE, &a_dsv);
			ID3D12DescriptorHeap* heaps[]{ g.srvHeap.Get() };
			a_commandList->SetGraphicsRootSignature(g.rootSignature.Get());
			a_commandList->SetDescriptorHeaps(1, heaps);
			a_commandList->SetGraphicsRootDescriptorTable(0, SrvSlot(0));  // the atlas
			a_commandList->SetGraphicsRootDescriptorTable(3, SrvSlot(kDepthSlot));
			a_commandList->SetGraphicsRootDescriptorTable(4, SrvSlot(kSceneSlot));
			a_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		}

		// Puts what BeginBlocks' pass drew into the back buffer (a_rtv): under the game's HUD, or over
		// everything. Without this frame's copy of the frame there's nothing to mix with: over it is.
		void EndBlocks(ID3D12GraphicsCommandList* a_commandList, D3D12_CPU_DESCRIPTOR_HANDLE a_rtv, bool a_underHud)
		{
			Transition(a_commandList, g.blocks.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			const auto& hud = g.hud;
			const float constants[8]{ hud.hudless ? 1.0f : 0.0f, hud.ui ? 1.0f : 0.0f, 0.0f, 0.0f, float(hud.hudlessLeft), float(hud.hudlessTop),
				float(hud.uiLeft), float(hud.uiTop) };
			a_commandList->OMSetRenderTargets(1, &a_rtv, FALSE, nullptr);
			a_commandList->SetGraphicsRootSignature(g.mixRootSignature.Get());
			a_commandList->SetGraphicsRootDescriptorTable(0, SrvSlot(kSceneFullSlot));
			a_commandList->SetGraphicsRoot32BitConstants(1, 8, constants, 0);
			a_commandList->SetPipelineState(a_underHud && g.sceneLive ? g.psoMix.Get() : g.psoOver.Get());
			a_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			a_commandList->DrawInstanced(3, 1, 0, 0);
		}

		bool EnsureAtlas(UINT a_width, UINT a_height)
		{
			if (g.atlas && g.atlasW == a_width && g.atlasH == a_height) {
				return true;
			}
			Retire(g.atlas);
			g.atlasLive = false;
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = a_width;
			desc.Height = a_height;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = kAtlasMips;
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
					IID_PPV_ARGS(&g.atlas)))) {
				logger::error("world: {}x{} atlas failed", a_width, a_height);
				return false;
			}
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = desc.Format;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = kAtlasMips;
			g.device->CreateShaderResourceView(g.atlas.Get(), &srv, g.srvHeap->GetCPUDescriptorHandleForHeapStart());
			g.atlasW = a_width;
			g.atlasH = a_height;
			logger::info("world: atlas {}x{}", a_width, a_height);
			return true;
		}

		// Section meshes live in upload-heap buffers: the GPU reads them straight from there, which
		// costs a little bandwidth per draw but keeps the copy plumbing out of this phase. Every
		// update gets a fresh buffer and the old one is retired: the last frames may still be
		// drawing from it, so it is never written or freed in place.
		// Minecraft's section mesh holds its blocks and fluids together, the water interleaved with
		// the blocks around it. Opaque and cutout faces are moved in front of translucent ones so
		// each pass draws its own range: drawing a whole section in the pass its first vertex asked
		// for put blocks in front of water in the translucent pass, without depth writes, and the
		// water showed over them. Returns a_vertices when they're in that order already.
		const proto::RenVertex* SplitLayers(const proto::RenVertex* a_vertices, UINT a_count, UINT& a_outOpaque)
		{
			static std::vector<proto::RenVertex> sorted;
			const auto opaque = [](const proto::RenVertex& a_v) { return (a_v.flags & kFlagTranslucent) == 0; };
			if (std::is_partitioned(a_vertices, a_vertices + a_count, opaque)) {
				a_outOpaque = UINT(std::partition_point(a_vertices, a_vertices + a_count, opaque) - a_vertices);
				return a_vertices;
			}
			// A face's six vertices share their flags, so a stable split keeps every face whole.
			sorted.assign(a_vertices, a_vertices + a_count);
			a_outOpaque = UINT(std::stable_partition(sorted.begin(), sorted.end(), opaque) - sorted.begin());
			return sorted.data();
		}

		bool UploadSection(Section& a_section, const proto::RenVertex* a_vertices, UINT a_count, UINT a_opaque)
		{
			const UINT64           bytes = UINT64(a_count) * sizeof(proto::RenVertex);
			ComPtr<ID3D12Resource> buffer;
			D3D12_HEAP_PROPERTIES  heap{};
			heap.Type = D3D12_HEAP_TYPE_UPLOAD;
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			desc.Width = bytes;
			desc.Height = 1;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
					IID_PPV_ARGS(&buffer)))) {
				return false;
			}
			void* mapped = nullptr;
			if (FAILED(buffer->Map(0, nullptr, &mapped))) {
				return false;
			}
			std::memcpy(mapped, a_vertices, bytes);
			buffer->Unmap(0, nullptr);
			Retire(a_section.vertices);
			a_section.vertices = std::move(buffer);
			a_section.capacity = a_count;
			a_section.count = a_count;
			a_section.opaque = a_opaque;
			return true;
		}
		// The next mip level of an RGBA8 image: each pixel the alpha-weighted mean of a 2x2 block,
		// so transparent texels don't darken the edges of leaves and glass.
		std::vector<std::uint8_t> HalfSize(const std::vector<std::uint8_t>& a_src, UINT a_width, UINT a_height, UINT& a_outWidth, UINT& a_outHeight)
		{
			a_outWidth = std::max(1u, a_width / 2);
			a_outHeight = std::max(1u, a_height / 2);
			std::vector<std::uint8_t> out(std::size_t(a_outWidth) * a_outHeight * 4);
			for (UINT y = 0; y < a_outHeight; ++y) {
				for (UINT x = 0; x < a_outWidth; ++x) {
					float rgb[3]{}, alpha = 0.0f, weight = 0.0f;
					for (UINT dy = 0; dy < 2; ++dy) {
						for (UINT dx = 0; dx < 2; ++dx) {
							const UINT sx = std::min(x * 2 + dx, a_width - 1), sy = std::min(y * 2 + dy, a_height - 1);
							const auto* p = a_src.data() + (std::size_t(sy) * a_width + sx) * 4;
							const float a = p[3] / 255.0f;
							for (int c = 0; c < 3; ++c) {
								rgb[c] += p[c] * a;
							}
							alpha += a;
							weight += a;
						}
					}
					auto* q = out.data() + (std::size_t(y) * a_outWidth + x) * 4;
					for (int c = 0; c < 3; ++c) {
						q[c] = std::uint8_t(weight > 0.0f ? std::clamp(rgb[c] / weight, 0.0f, 255.0f) : 0.0f);
					}
					q[3] = std::uint8_t(std::clamp(alpha / 4.0f * 255.0f, 0.0f, 255.0f));
				}
			}
			return out;
		}

		void UploadAtlas(ID3D12GraphicsCommandList* a_commandList)
		{
			if (g.atlasPending.empty() || !g.atlas) {
				return;
			}
			// Minecraft's mip chain: distant blocks average their texels instead of sparkling
			// against Cyberpunk's smooth, DLSS-filtered picture. Built here, uploaded level by level.
			std::vector<std::vector<std::uint8_t>> levels;
			std::vector<std::pair<UINT, UINT>>     sizes;
			levels.push_back(std::move(g.atlasPending));
			sizes.emplace_back(g.atlasW, g.atlasH);
			for (UINT m = 1; m < kAtlasMips; ++m) {
				UINT w = 0, h = 0;
				levels.push_back(HalfSize(levels.back(), sizes.back().first, sizes.back().second, w, h));
				sizes.emplace_back(w, h);
			}
			g.atlasPending.clear();

			const auto                                      atlasDesc = g.atlas->GetDesc();
			std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, kAtlasMips> layouts{};
			std::array<UINT, kAtlasMips>                    rowCounts{};
			std::array<UINT64, kAtlasMips>                  rowSizes{};
			UINT64                                          bytes = 0;
			g.device->GetCopyableFootprints(&atlasDesc, 0, kAtlasMips, 0, layouts.data(), rowCounts.data(), rowSizes.data(), &bytes);
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_UPLOAD;
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			desc.Width = bytes;
			desc.Height = 1;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			Retire(g.atlasUpload);  // an earlier copy may still be reading it
			if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
					IID_PPV_ARGS(&g.atlasUpload)))) {
				return;
			}
			void* mapped = nullptr;
			if (FAILED(g.atlasUpload->Map(0, nullptr, &mapped))) {
				return;
			}
			for (UINT m = 0; m < kAtlasMips; ++m) {
				const UINT rowBytes = sizes[m].first * 4;
				for (UINT y = 0; y < rowCounts[m]; ++y) {
					std::memcpy(static_cast<std::uint8_t*>(mapped) + layouts[m].Offset + std::size_t(y) * layouts[m].Footprint.RowPitch,
						levels[m].data() + std::size_t(y) * rowBytes, rowBytes);
				}
			}
			g.atlasUpload->Unmap(0, nullptr);

			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = g.atlas.Get();
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			if (g.atlasLive) {
				barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
				barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
				a_commandList->ResourceBarrier(1, &barrier);
			}
			for (UINT m = 0; m < kAtlasMips; ++m) {
				D3D12_TEXTURE_COPY_LOCATION dst{};
				dst.pResource = g.atlas.Get();
				dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				dst.SubresourceIndex = m;
				D3D12_TEXTURE_COPY_LOCATION src{};
				src.pResource = g.atlasUpload.Get();
				src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				src.PlacedFootprint = layouts[m];
				a_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			}
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			a_commandList->ResourceBarrier(1, &barrier);
			g.atlasLive = true;
		}
	}

	bool Prepare(ID3D12Device* a_device, DXGI_FORMAT a_format, UINT a_width, UINT a_height)
	{
		if (g.failed) {
			return false;
		}
		if (!g.ready) {
			g.failed = true;
			g.device = a_device;
			D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
			srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			srvDesc.NumDescriptors = kSrvSlots;
			srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			if (FAILED(g.device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g.srvHeap)))) {
				return false;
			}
			g.srvIncrement = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			// Null views until there is a depth copy and a scene picture: every slot a table points
			// at must hold a valid descriptor.
			for (const auto [slot, format] : { std::pair{ kSceneFullSlot, DXGI_FORMAT_R8G8B8A8_UNORM }, std::pair{ kBlocksSlot, DXGI_FORMAT_R8G8B8A8_UNORM },
					 std::pair{ kHudlessSlot, DXGI_FORMAT_R8G8B8A8_UNORM }, std::pair{ kHudSlot, DXGI_FORMAT_R8G8B8A8_UNORM },
					 std::pair{ kDepthSlot, DXGI_FORMAT_R32_FLOAT }, std::pair{ kSceneSlot, DXGI_FORMAT_R8G8B8A8_UNORM } }) {
				D3D12_SHADER_RESOURCE_VIEW_DESC nullView{};
				nullView.Format = format;
				nullView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				nullView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				nullView.Texture2D.MipLevels = 1;
				g.device->CreateShaderResourceView(nullptr, &nullView, CpuSlot(slot));
			}
			D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
			dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
			dsvDesc.NumDescriptors = 1;
			if (FAILED(g.device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&g.dsvHeap)))) {
				return false;
			}
			D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
			rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			rtvDesc.NumDescriptors = 2;  // kRtvSmall, kRtvBlocks
			if (FAILED(g.device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g.rtvHeap)))) {
				return false;
			}
			if (!CreatePipelines()) {
				return false;
			}
			g.failed = false;
			g.ready = true;
			logger::info("world: block rendering ready");
		}
		// The back buffer's format can change (HDR on or off): Mix draws into it.
		if (a_format != g.mixFormat) {
			g.mixFormat = a_format;
			if (!CreateMix(a_format)) {
				logger::error("world: no mix pipeline for back buffer format {}", int(a_format));
			}
		}
		return g.psoMix && CreateTargets(a_width, a_height);
	}

	void Consume()
	{
		if (!g.ready) {
			return;
		}
		// Bounded per frame: a big build can fill the ring faster than one frame should spend on it.
		Link::Get().DrainRender([](std::uint32_t a_type, const std::uint8_t* a_payload, std::uint32_t a_bytes) {
			switch (a_type) {
			case proto::kRenClearAll:
				Builds::OnClearAll();
				for (auto& [key, section] : g.sections) {
					Retire(section.vertices);
				}
				g.sections.clear();
				{
					std::lock_guard lock(facesMutex);
					faces.clear();
				}
				return;
			case proto::kRenAtlas:
				{
					if (a_bytes < sizeof(proto::RenAtlas)) {
						return;
					}
					const auto& atlas = *reinterpret_cast<const proto::RenAtlas*>(a_payload);
					const auto  pixels = a_payload + sizeof(proto::RenAtlas);
					const UINT64 needed = UINT64(atlas.width) * atlas.height * 4;
					if (atlas.width == 0 || atlas.height == 0 || a_bytes < sizeof(proto::RenAtlas) + needed) {
						return;
					}
					if (EnsureAtlas(atlas.width, atlas.height)) {
						// Kept for the next Draw, which has a command list to copy with.
						g.atlasPending.assign(pixels, pixels + needed);
					}
					return;
				}
			case proto::kRenSection:
				{
					if (a_bytes < sizeof(proto::RenSection)) {
						return;
					}
					const auto& header = *reinterpret_cast<const proto::RenSection*>(a_payload);
					const auto  key = SectionKey(header.sx, header.sy, header.sz);
					if (header.vertexCount == 0) {
						RetireSection(key);
						return;
					}
					const UINT64 needed = UINT64(header.vertexCount) * sizeof(proto::RenVertex);
					if (a_bytes < sizeof(proto::RenSection) + needed) {
						return;
					}
					UINT        opaque = 0;
					const auto* vertices = SplitLayers(reinterpret_cast<const proto::RenVertex*>(a_payload + sizeof(proto::RenSection)), header.vertexCount, opaque);
					auto&       section = g.sections[key];
					section.sx = header.sx;
					section.sy = header.sy;
					section.sz = header.sz;
					if (!UploadSection(section, vertices, header.vertexCount, opaque)) {
						RetireSection(key);
						return;
					}
					SetFaces(key, section, vertices, header.vertexCount);
					return;
				}
			case proto::kRenScene:
				{
					// Every other entity and all particles this frame, as Minecraft drew them.
					if (a_bytes < sizeof(proto::RenScene)) {
						return;
					}
					const auto& header = *reinterpret_cast<const proto::RenScene*>(a_payload);
					const UINT64 need = sizeof(proto::RenScene) + UINT64(header.batchCount) * sizeof(proto::RenBatch) +
					                    UINT64(header.vertexCount) * sizeof(proto::RenVertex);
					g.sceneBatches.clear();
					g.sceneVertices.clear();
					if (header.batchCount == 0 || header.vertexCount == 0 || a_bytes < need) {
						return;
					}
					g.sceneOrigin[0] = header.originX;
					g.sceneOrigin[1] = header.originY;
					g.sceneOrigin[2] = header.originZ;
					const auto* batches = reinterpret_cast<const proto::RenBatch*>(a_payload + sizeof(proto::RenScene));
					const auto* vertices = reinterpret_cast<const proto::RenVertex*>(batches + header.batchCount);
					g.sceneBatches.assign(batches, batches + header.batchCount);
					g.sceneVertices.assign(vertices, vertices + header.vertexCount);
					if (Config::Diagnostics()) {
						// Particles (crits, potion swirls, smoke, ...) are the scene's batches without a
						// face normal: every model, item and block in it has one.
						UINT particles = 0;
						for (const auto& batch : g.sceneBatches) {
							if (batch.count > 0 && batch.first + batch.count <= header.vertexCount && ((vertices[batch.first].flags >> 4) & 7) == 0) {
								particles += batch.count;
							}
						}
						g.sceneParticlesPeak = std::max(g.sceneParticlesPeak, particles);
					}
					return;
				}
			case proto::kRenTexture:
				{
					if (a_bytes < sizeof(proto::RenTexture)) {
						return;
					}
					const auto& header = *reinterpret_cast<const proto::RenTexture*>(a_payload);
					const UINT64 needed = UINT64(header.width) * header.height * 4;
					if (header.id == 0 || header.width == 0 || header.height == 0 || header.width > 4096 || header.height > 4096 ||
						a_bytes < sizeof(proto::RenTexture) + needed) {
						return;
					}
					auto& t = g.textures[header.id];
					Retire(t.texture);
					FreeSlot(t.slot);
					t.slot = AllocSlot();
					t.live = false;
					t.width = header.width;
					t.height = header.height;
					if (t.slot == 0) {
						logger::warn("world: no descriptor slot left for entity texture {}", header.id);
						g.textures.erase(header.id);
						return;
					}
					D3D12_HEAP_PROPERTIES heap{};
					heap.Type = D3D12_HEAP_TYPE_DEFAULT;
					D3D12_RESOURCE_DESC desc{};
					desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
					desc.Width = header.width;
					desc.Height = header.height;
					desc.DepthOrArraySize = 1;
					desc.MipLevels = 1;
					desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
					desc.SampleDesc.Count = 1;
					if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
							IID_PPV_ARGS(&t.texture)))) {
						FreeSlot(t.slot);
						g.textures.erase(header.id);
						return;
					}
					D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
					srv.Format = desc.Format;
					srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
					srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
					srv.Texture2D.MipLevels = 1;
					auto slot = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
					slot.ptr += SIZE_T(t.slot) * g.srvIncrement;
					g.device->CreateShaderResourceView(t.texture.Get(), &srv, slot);
					const auto* pixels = a_payload + sizeof(proto::RenTexture);
					t.pending.assign(pixels, pixels + needed);
					if (Config::Diagnostics()) {
						logger::info("world: entity texture {} ({}x{}) arrived, slot {}", header.id, header.width, header.height, t.slot);
					}
					return;
				}
			case proto::kRenAtlasRegion:
				{
					if (a_bytes < sizeof(proto::RenAtlasRegion)) {
						return;
					}
					const auto& header = *reinterpret_cast<const proto::RenAtlasRegion*>(a_payload);
					const UINT64 needed = UINT64(header.width) * header.height * 4;
					if (header.width == 0 || header.height == 0 || a_bytes < sizeof(proto::RenAtlasRegion) + needed) {
						return;
					}
					// The newest frame of a sprite replaces any older one still waiting.
					const auto* pixels = a_payload + sizeof(proto::RenAtlasRegion);
					for (auto& r : g.regionsPending) {
						if (r.x == header.x && r.y == header.y && r.width == header.width && r.height == header.height) {
							r.pixels.assign(pixels, pixels + needed);
							return;
						}
					}
					g.regionsPending.push_back({ header.x, header.y, header.width, header.height, std::vector<std::uint8_t>(pixels, pixels + needed) });
					return;
				}
			case proto::kRenLights:
				{
					// A section's light-emitting blocks: Builds lights them in Night City.
					if (a_bytes < sizeof(proto::RenLights)) {
						return;
					}
					const auto& header = *reinterpret_cast<const proto::RenLights*>(a_payload);
					const UINT64 need = sizeof(proto::RenLights) + UINT64(header.count) * sizeof(proto::RenLight);
					if (a_bytes < need) {
						return;
					}
					Builds::OnLights(header.sx, header.sy, header.sz, reinterpret_cast<const proto::RenLight*>(a_payload + sizeof(proto::RenLights)), header.count);
					return;
				}
			case proto::kRenSolids:
				{
					// A section's blocks NPCs collide with: Builds gives them colliders.
					if (a_bytes < sizeof(proto::RenSolids)) {
						return;
					}
					const auto& header = *reinterpret_cast<const proto::RenSolids*>(a_payload);
					const bool   any = header.count > 0 && a_bytes >= sizeof(proto::RenSolids) + 512;
					Builds::OnSolids(header.sx, header.sy, header.sz, any ? a_payload + sizeof(proto::RenSolids) : nullptr);
					return;
				}
			case proto::kRenAvatar:
				{
					// The player's body this frame; no batches in first person.
					g.avatarBatches.clear();
					g.avatarVertices.clear();
					if (a_bytes < sizeof(proto::RenAvatar)) {
						return;
					}
					const auto& header = *reinterpret_cast<const proto::RenAvatar*>(a_payload);
					const UINT64 need = sizeof(proto::RenAvatar) + UINT64(header.batchCount) * sizeof(proto::RenBatch) +
					                    UINT64(header.vertexCount) * sizeof(proto::RenVertex);
					if (header.batchCount == 0 || header.vertexCount == 0 || a_bytes < need) {
						return;
					}
					const auto* batches = reinterpret_cast<const proto::RenBatch*>(a_payload + sizeof(proto::RenAvatar));
					const auto* vertices = reinterpret_cast<const proto::RenVertex*>(batches + header.batchCount);
					g.avatarBatches.assign(batches, batches + header.batchCount);
					g.avatarVertices.assign(vertices, vertices + header.vertexCount);
					return;
				}
			case proto::kRenHand:
				{
					// The first-person hands and held items this frame; none in third person.
					g.handBatches.clear();
					g.handVertices.clear();
					if (a_bytes < sizeof(proto::RenHand)) {
						return;
					}
					const auto& header = *reinterpret_cast<const proto::RenHand*>(a_payload);
					const UINT64 need = sizeof(proto::RenHand) + UINT64(header.batchCount) * sizeof(proto::RenBatch) +
					                    UINT64(header.vertexCount) * sizeof(proto::RenVertex);
					if (header.batchCount == 0 || header.vertexCount == 0 || a_bytes < need) {
						return;
					}
					const auto* batches = reinterpret_cast<const proto::RenBatch*>(a_payload + sizeof(proto::RenHand));
					const auto* vertices = reinterpret_cast<const proto::RenVertex*>(batches + header.batchCount);
					g.handBatches.assign(batches, batches + header.batchCount);
					g.handVertices.assign(vertices, vertices + header.vertexCount);
					g.handFov = std::isfinite(header.fovDeg) ? std::clamp(header.fovDeg, 10.0f, 170.0f) : 70.0f;
					return;
				}
			default:
				// The dug-block sets and the death ragdoll's parts: drained so the ring keeps moving.
				return;
			}
		}, 48ull << 20);
	}

	void Draw(ID3D12GraphicsCommandList* a_commandList, const proto::McState& a_state, UINT a_width, UINT a_height,
		D3D12_CPU_DESCRIPTOR_HANDLE a_rtv)
	{
		if (!g.ready || !g.atlas) {
			return;
		}
		++g.frame;
		while (!g.retired.empty() && g.retired.front().first + kRetireFrames <= g.frame) {
			g.retired.pop_front();
		}
		UploadAtlas(a_commandList);
		if (!g.atlasLive || !EnsureVisRing() || !EnsureDynRing()) {
			return;
		}
		UploadTextures(a_commandList);
		// From here on the hand can be drawn: Minecraft hands it over (kCyberDrawsHand) from its next frame.
		static const bool litHand = Config::GetBool(L"World", L"bLitHand", true);
		drawsHand = litHand;

		// Cyberpunk's own camera when there is one, so the blocks sit still in the picture it shows;
		// Minecraft's camera otherwise (it runs ahead of V's smoothed position, so blocks swim).
		const auto camera = GetCameraView();
		double     eyeX = a_state.eyeX, eyeY = a_state.eyeY, eyeZ = a_state.eyeZ;
		dx::XMVECTOR forward;
		dx::XMVECTOR up = dx::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
		float        fov = a_state.fovDeg > 1.0f ? dx::XMConvertToRadians(a_state.fovDeg) : dx::XMConvertToRadians(70.0f);
		if (camera.valid) {
			eyeX = camera.eye[0];
			eyeY = camera.eye[1];
			eyeZ = camera.eye[2];
			forward = dx::XMVectorSet(camera.forward[0], camera.forward[1], camera.forward[2], 0.0f);
			up = dx::XMVectorSet(camera.up[0], camera.up[1], camera.up[2], 0.0f);
			fov = dx::XMConvertToRadians(camera.fovYDeg);
		} else {
			// Minecraft yaw 0 looks along +Z and grows clockwise; pitch is positive looking down.
			const float yaw = dx::XMConvertToRadians(a_state.yaw);
			const float pitch = dx::XMConvertToRadians(a_state.pitch);
			forward = dx::XMVectorSet(-std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch), 0.0f);
		}
		const auto eye = dx::XMVectorSet(float(eyeX), float(eyeY), float(eyeZ), 1.0f);
		// Minecraft's axes are right-handed (east, up, south): a left-handed view drew the world
		// mirrored left to right.
		const auto view = dx::XMMatrixLookToRH(eye, forward, up);
		const auto projection = dx::XMMatrixPerspectiveFovRH(fov, float(a_width) / float(a_height), 0.05f, 512.0f);

		// Stored as DirectXMath builds it (row vectors): the shader's cbuffer is column-major, so it
		// reads the transpose, which is what mul(viewProj, column vector) needs. Transposing here
		// as well applied the wrong matrix and put every block off screen.
		Params params{};
		dx::XMStoreFloat4x4(&params.viewProj, dx::XMMatrixMultiply(view, projection));
		params.light[0] = cybercraft::State().daylight.load();
		dx::XMFLOAT3 look;
		dx::XMStoreFloat3(&look, dx::XMVector3Normalize(forward));
		params.eye[0] = float(eyeX);
		params.eye[1] = float(eyeY);
		params.eye[2] = float(eyeZ);
		params.eye[3] = float(MetresPerBlock());
		params.forward[0] = look.x;
		params.forward[1] = look.y;
		params.forward[2] = look.z;

		// Hidden pixel by pixel behind whatever Cyberpunk drew (its depth, borrowed from DLSS), when
		// the blocks are drawn from Cyberpunk's own camera; face by face from rays otherwise.
		SceneDepth::View depth{};
		const bool       haveDepth = camera.valid && SceneDepth::Acquire(depth);
		if (haveDepth) {
			if (depth.texture != g.depthBound) {
				D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
				srv.Format = depth.srvFormat;
				srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srv.Texture2D.MipLevels = 1;
				g.device->CreateShaderResourceView(depth.texture, &srv, CpuSlot(kDepthSlot));
				g.depthBound = depth.texture;
			}
			params.depthParams[0] = depth.cameraNear;
			params.depthParams[1] = depth.cameraFar;
			params.depthParams[2] = depth.inverted ? 1.0f : 0.0f;
			params.depthParams[3] = 1.0f;
			params.depthMap[0] = float(depth.extentWidth) / float(a_width);
			params.depthMap[1] = float(depth.extentHeight) / float(a_height);
			params.depthMap[2] = float(depth.left);
			params.depthMap[3] = float(depth.top);
		}
		depthActive = haveDepth;
		// Lit by Cyberpunk's own picture around each pixel (CaptureScene), when there is one.
		static const float lightGain = Config::GetFloat(L"World", L"fLightGain", 1.7f);
		static const float lightFloor = Config::GetFloat(L"World", L"fLightFloor", 0.03f);
		static const float lightProbe = Config::GetFloat(L"World", L"fLightProbe", 1.5f);
		static const float lightColor = Config::GetFloat(L"World", L"fLightColor", 0.85f);
		static const float faceShade = Config::GetFloat(L"World", L"fFaceShade", 0.35f);
		static const float haze = Config::GetFloat(L"World", L"fHaze", 0.005f);
		static const float blockLight = Config::GetFloat(L"World", L"fBlockLight", 0.5f);
		if (g.sceneLive) {
			params.sceneParams[0] = 1.0f;
			params.sceneParams[1] = lightGain;
			params.sceneParams[2] = lightFloor;
			params.sceneParams[3] = std::max(lightProbe, 0.0f);
			params.tone[0] = std::max(lightColor, 0.0f);
			params.tone[1] = std::clamp(faceShade, 0.0f, 1.0f);
			params.tone[2] = std::max(haze, 0.0f);
			params.tone[3] = std::max(blockLight, 0.0f);
			params.light[2] = 1.0f / float(a_width);  // screen pixel -> uv of the 80x45 picture
			params.light[3] = 1.0f / float(a_height);
		}

		// Arrows, dropped items and cracks are built around a whole block near the camera, so their
		// floats stay small; the scene (mobs, TNT, particles) comes relative to its own origin.
		const double entityOrigin[3] = { std::floor(eyeX), std::floor(eyeY), std::floor(eyeZ) };
		static proto::WorldEntities worldEntities;
		if (Link::Get().ReadWorldEntities(worldEntities)) {
			// The camera's right and up, for sprites facing it (right-handed: forward x up = right).
			dx::XMFLOAT3 right3, up3;
			const auto   rightV = dx::XMVector3Normalize(dx::XMVector3Cross(forward, up));
			dx::XMStoreFloat3(&right3, rightV);
			dx::XMStoreFloat3(&up3, dx::XMVector3Normalize(dx::XMVector3Cross(rightV, forward)));
			const float right[3] = { right3.x, right3.y, right3.z };
			const float upAxis[3] = { up3.x, up3.y, up3.z };
			BuildWorldEntities(worldEntities, entityOrigin, right, upAxis);
		}
		const bool showAvatar = camera.valid && camera.avatar && !g.avatarVertices.empty();
		// Not in a car: Cyberpunk drives there, and Minecraft's hand has nothing to hold up.
		const bool showHand = litHand && !g.handVertices.empty() && cybercraft::State().puppeting.load();
		if (g.sections.empty() && g.sceneVertices.empty() && entityVerts.empty() && crackVerts.empty() && !showAvatar && !showHand) {
			return;
		}
		UINT64     frameUsed = 0;
		const auto sceneAddress = g.sceneVertices.empty() ? 0 : DynUpload(g.sceneVertices.data(), g.sceneVertices.size() * sizeof(proto::RenVertex), frameUsed);
		const auto entityAddress = entityVerts.empty() ? 0 : DynUpload(entityVerts.data(), entityVerts.size() * sizeof(proto::RenVertex), frameUsed);
		const auto crackAddress = crackVerts.empty() ? 0 : DynUpload(crackVerts.data(), crackVerts.size() * sizeof(proto::RenVertex), frameUsed);
		const auto avatarAddress = showAvatar ? DynUpload(g.avatarVertices.data(), g.avatarVertices.size() * sizeof(proto::RenVertex), frameUsed) : 0;
		const auto handAddress = showHand ? DynUpload(g.handVertices.data(), g.handVertices.size() * sizeof(proto::RenVertex), frameUsed) : 0;
		// Streams are never hidden by the city's rays: one all-visible slice, as long as the longest.
		const UINT longest = UINT(std::max({ g.sceneVertices.size(), entityVerts.size(), crackVerts.size(), showAvatar ? g.avatarVertices.size() : std::size_t(0),
			showHand ? g.handVertices.size() : std::size_t(0) }));
		const auto allVisible = VisibilitySlice(~0ull, (longest + 5) / 6);
		auto       drawStream = [&](D3D12_GPU_VIRTUAL_ADDRESS a_address, UINT a_vertices, UINT a_first, UINT a_count, const float a_origin[3]) {
			params.origin[0] = a_origin[0];
			params.origin[1] = a_origin[1];
			params.origin[2] = a_origin[2];
			a_commandList->SetGraphicsRoot32BitConstants(1, sizeof(Params) / 4, &params, 0);
			a_commandList->SetGraphicsRootShaderResourceView(2, allVisible);
			D3D12_VERTEX_BUFFER_VIEW vertexBuffer{};
			vertexBuffer.BufferLocation = a_address;
			vertexBuffer.SizeInBytes = a_vertices * sizeof(proto::RenVertex);
			vertexBuffer.StrideInBytes = sizeof(proto::RenVertex);
			a_commandList->IASetVertexBuffers(0, 1, &vertexBuffer);
			a_commandList->DrawInstanced(a_count, 1, a_first, 0);
		};
		const float sceneOrigin[3] = { float(g.sceneOrigin[0]), float(g.sceneOrigin[1]), float(g.sceneOrigin[2]) };
		const float nearOrigin[3] = { float(entityOrigin[0]), float(entityOrigin[1]), float(entityOrigin[2]) };
		const float feetOrigin[3] = { float(camera.feet[0]), float(camera.feet[1]), float(camera.feet[2]) };

		// Everything goes into the blocks target first, over nothing, and from there into the back
		// buffer under the game's HUD (EndBlocks).
		const auto dsv = g.dsvHeap->GetCPUDescriptorHandleForHeapStart();
		// A freshly reset command list has an empty scissor rect, which clipped every block away.
		const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, float(a_width), float(a_height), 0.0f, 1.0f };
		const D3D12_RECT     scissor{ 0, 0, LONG(a_width), LONG(a_height) };
		a_commandList->RSSetViewports(1, &viewport);
		a_commandList->RSSetScissorRects(1, &scissor);
		BeginBlocks(a_commandList, dsv);

		// Mobs, TNT, the explosion's particles: Minecraft's own entity renderer output, a batch per
		// texture (0: the atlas, else an entity texture). The player's body and hands the same way.
		auto drawBatches = [&](int a_pass, D3D12_GPU_VIRTUAL_ADDRESS a_address, const std::vector<proto::RenBatch>& a_batches, UINT a_total, const float a_origin[3]) {
			for (const auto& batch : a_batches) {
				if (((batch.flags & 1) != 0) != (a_pass == 1) || batch.count == 0 || batch.first + batch.count > a_total) {
					continue;
				}
				UINT slot = 0;
				if (batch.texture != 0) {
					const auto found = g.textures.find(batch.texture);
					if (found == g.textures.end() || !found->second.live) {
						continue;  // its texture hasn't arrived yet
					}
					slot = found->second.slot;
				}
				a_commandList->SetGraphicsRootDescriptorTable(0, SrvSlot(slot));
				drawStream(a_address, a_total, batch.first, batch.count, a_origin);
			}
			a_commandList->SetGraphicsRootDescriptorTable(0, SrvSlot(0));
		};

		// Opaque and cutout first, then translucent over it, like Minecraft's own layer order.
		for (int pass = 0; pass < 2; ++pass) {
			a_commandList->SetPipelineState(pass == 0 ? g.psoOpaque.Get() : g.psoTranslucent.Get());
			a_commandList->SetGraphicsRootDescriptorTable(0, SrvSlot(0));  // the atlas
			params.pass = float(pass);
			for (const auto& [key, section] : g.sections) {
				// The section's opaque and cutout faces, or its translucent ones after them (SplitLayers).
				const UINT first = pass == 0 ? 0 : section.opaque;
				const UINT count = pass == 0 ? section.opaque : section.count - section.opaque;
				if (count == 0) {
					continue;
				}
				params.origin[0] = float(section.sx * 16);
				params.origin[1] = float(section.sy * 16);
				params.origin[2] = float(section.sz * 16);
				a_commandList->SetGraphicsRoot32BitConstants(1, sizeof(Params) / 4, &params, 0);
				// With the game's depth every pixel is tested, so no face is hidden whole.
				a_commandList->SetGraphicsRootShaderResourceView(2, VisibilitySlice(haveDepth ? ~0ull : key, (count + 5) / 6, first / 6));
				// The view starts at the range, so the shader's vertex ids (and face numbers) count from it.
				D3D12_VERTEX_BUFFER_VIEW vertexBuffer{};
				vertexBuffer.BufferLocation = section.vertices->GetGPUVirtualAddress() + UINT64(first) * sizeof(proto::RenVertex);
				vertexBuffer.SizeInBytes = count * sizeof(proto::RenVertex);
				vertexBuffer.StrideInBytes = sizeof(proto::RenVertex);
				a_commandList->IASetVertexBuffers(0, 1, &vertexBuffer);
				a_commandList->DrawInstanced(count, 1, 0, 0);
			}

			// Arrows, dropped items and blocks (cutout), then block-breaking cracks (translucent).
			if (pass == 0 && entityAddress) {
				drawStream(entityAddress, UINT(entityVerts.size()), 0, UINT(entityVerts.size()), nearOrigin);
			}
			if (pass == 1 && crackAddress) {
				drawStream(crackAddress, UINT(crackVerts.size()), 0, UINT(crackVerts.size()), nearOrigin);
			}
			if (sceneAddress) {
				drawBatches(pass, sceneAddress, g.sceneBatches, UINT(g.sceneVertices.size()), sceneOrigin);
			}
			if (avatarAddress) {
				drawBatches(pass, avatarAddress, g.avatarBatches, UINT(g.avatarVertices.size()), feetOrigin);
			}
		}
		EndBlocks(a_commandList, a_rtv, true);

		// The first-person hands and held items last, over everything (the game's HUD too, as when
		// they were part of Minecraft's overlay), as Minecraft draws them: its view space, its hand
		// projection, a depth buffer of their own. Lit like the blocks, from the picture where they
		// are on screen.
		if (handAddress) {
			const Params world = params;
			dx::XMStoreFloat4x4(&params.viewProj,
				dx::XMMatrixPerspectiveFovRH(dx::XMConvertToRadians(g.handFov), float(a_width) / float(a_height), 0.05f, 64.0f));
			params.depthParams[3] = 0.0f;  // nothing of Cyberpunk's is ever in front of them
			params.sceneParams[3] *= 0.3f;  // the side a face turns to, at the scale of an arm
			params.tone[2] = 0.0f;          // no haze an arm's length away
			const float viewOrigin[3] = { 0.0f, 0.0f, 0.0f };
			BeginBlocks(a_commandList, dsv);
			for (int pass = 0; pass < 2; ++pass) {
				a_commandList->SetPipelineState(pass == 0 ? g.psoOpaque.Get() : g.psoTranslucent.Get());
				params.pass = float(pass);
				drawBatches(pass, handAddress, g.handBatches, UINT(g.handVertices.size()), viewOrigin);
			}
			EndBlocks(a_commandList, a_rtv, false);
			params = world;
		}

		// Diagnostics: what was drawn, and where the nearest section's centre lands on screen
		// (ndc x and y in -1..1 and w > 0 are on screen, in front of the camera).
		static auto nextLog = std::chrono::steady_clock::time_point{};
		const auto  now = std::chrono::steady_clock::now();
		if (Config::Diagnostics() && now >= nextLog) {
			nextLog = now + std::chrono::seconds(5);
			std::size_t drawn = 0;
			UINT        vertices = 0;
			const Section* nearest = nullptr;
			float          nearestSq = 0.0f;
			for (const auto& [key, section] : g.sections) {
				if (section.count == 0) {
					continue;
				}
				++drawn;
				vertices += section.count;
				const float cx = float(section.sx * 16 + 8) - float(eyeX);
				const float cy = float(section.sy * 16 + 8) - float(eyeY);
				const float cz = float(section.sz * 16 + 8) - float(eyeZ);
				const float sq = cx * cx + cy * cy + cz * cz;
				if (!nearest || sq < nearestSq) {
					nearest = &section;
					nearestSq = sq;
				}
			}
			if (nearest) {
				const auto centre = dx::XMVectorSet(float(nearest->sx * 16 + 8), float(nearest->sy * 16 + 8), float(nearest->sz * 16 + 8), 1.0f);
				dx::XMFLOAT4 clip;
				dx::XMStoreFloat4(&clip, dx::XMVector4Transform(centre, dx::XMMatrixMultiply(view, projection)));
				logger::info("world: drew {} sections, {} vertices; {} camera eye ({:.2f}, {:.2f}, {:.2f}) fovY {:.1f} (Minecraft's eye ({:.2f}, {:.2f}, {:.2f})); daylight {:.2f}; nearest section ({}, {}, {}) at ndc ({:.2f}, {:.2f}) w {:.1f}",
					drawn, vertices, camera.valid ? "Cyberpunk's" : "Minecraft's", eyeX, eyeY, eyeZ, dx::XMConvertToDegrees(fov), a_state.eyeX, a_state.eyeY,
					a_state.eyeZ, params.light[0], nearest->sx, nearest->sy, nearest->sz, clip.w != 0.0f ? clip.x / clip.w : 0.0f,
					clip.w != 0.0f ? clip.y / clip.w : 0.0f, clip.w);
			} else {
				logger::info("world: no sections to draw ({} known, atlas {})", g.sections.size(), g.atlasLive);
			}
			// Entities and particles: a crit or a potion swirl since the last log shows as particle
			// vertices; a batch waiting for its texture is skipped until the texture arrives.
			std::size_t waiting = 0;
			for (const auto& batch : g.sceneBatches) {
				if (batch.texture != 0) {
					const auto found = g.textures.find(batch.texture);
					waiting += found == g.textures.end() || !found->second.live;
				}
			}
			logger::info("world: scene {} batches, {} vertices, {} batches waiting for a texture; up to {} particle vertices in the last 5 s",
				g.sceneBatches.size(), g.sceneVertices.size(), waiting, g.sceneParticlesPeak);
			g.sceneParticlesPeak = 0;
		}
	}

	void CaptureScene(ID3D12GraphicsCommandList* a_commandList, ID3D12Resource* a_backBuffer)
	{
		g.sceneLive = false;
		if (!g.ready || !a_backBuffer) {
			return;
		}
		const auto desc = a_backBuffer->GetDesc();
		BindHud(a_commandList, UINT(desc.Width), desc.Height, desc.Format);
		if (!g.sceneColor || g.sceneW != UINT(desc.Width) || g.sceneH != desc.Height || g.sceneFormat != desc.Format) {
			Retire(g.sceneColor);
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_RESOURCE_DESC target{};
			target.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			target.Width = desc.Width;
			target.Height = desc.Height;
			target.DepthOrArraySize = 1;
			target.MipLevels = 1;
			target.Format = desc.Format;
			target.SampleDesc.Count = 1;
			if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
					IID_PPV_ARGS(&g.sceneColor)))) {
				return;
			}
			g.sceneW = UINT(desc.Width);
			g.sceneH = desc.Height;
			g.sceneFormat = desc.Format;
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = desc.Format;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = 1;
			g.device->CreateShaderResourceView(g.sceneColor.Get(), &srv, CpuSlot(kSceneFullSlot));
		}
		if (!g.sceneSmall) {
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_RESOURCE_DESC shrunk{};  // not "small": a Windows header defines that as char
			shrunk.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			shrunk.Width = kSceneSmallW;
			shrunk.Height = kSceneSmallH;
			shrunk.DepthOrArraySize = 1;
			shrunk.MipLevels = 1;
			shrunk.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			shrunk.SampleDesc.Count = 1;
			shrunk.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &shrunk, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
					IID_PPV_ARGS(&g.sceneSmall)))) {
				g.sceneSmall.Reset();
				return;
			}
			g.device->CreateRenderTargetView(g.sceneSmall.Get(), nullptr, RtvSlot(kRtvSmall));
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = shrunk.Format;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = 1;
			g.device->CreateShaderResourceView(g.sceneSmall.Get(), &srv, CpuSlot(kSceneSlot));
		}
		Transition(a_commandList, a_backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
		Transition(a_commandList, g.sceneColor.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
		a_commandList->CopyResource(g.sceneColor.Get(), a_backBuffer);
		Transition(a_commandList, g.sceneColor.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		Transition(a_commandList, a_backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);

		// Shrink it to 80x45 (PSDown): the blocks' light, with no detail of what stands behind them.
		// From the picture without the HUD when the game hands one over the whole back buffer: the HUD
		// isn't light.
		bool lightFromHudless = false;
		if (g.hud.hudless && g.hud.hudlessLeft == 0 && g.hud.hudlessTop == 0) {
			const auto hudless = g.hud.hudless->GetDesc();
			lightFromHudless = hudless.Width == desc.Width && hudless.Height == desc.Height;
		}
		Transition(a_commandList, g.sceneSmall.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		const auto           rtv = RtvSlot(kRtvSmall);
		const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, float(kSceneSmallW), float(kSceneSmallH), 0.0f, 1.0f };
		const D3D12_RECT     scissor{ 0, 0, LONG(kSceneSmallW), LONG(kSceneSmallH) };
		ID3D12DescriptorHeap* heaps[]{ g.srvHeap.Get() };
		a_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
		a_commandList->RSSetViewports(1, &viewport);
		a_commandList->RSSetScissorRects(1, &scissor);
		a_commandList->SetGraphicsRootSignature(g.rootSignature.Get());
		a_commandList->SetDescriptorHeaps(1, heaps);
		// Every table the root signature has must point at valid views, used or not.
		a_commandList->SetGraphicsRootDescriptorTable(0, SrvSlot(0));
		a_commandList->SetGraphicsRootDescriptorTable(3, SrvSlot(kDepthSlot));
		a_commandList->SetGraphicsRootDescriptorTable(4, SrvSlot(lightFromHudless ? kHudlessSlot : kSceneFullSlot));
		a_commandList->SetPipelineState(g.psoDown.Get());
		a_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		a_commandList->DrawInstanced(3, 1, 0, 0);
		Transition(a_commandList, g.sceneSmall.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		g.sceneLive = true;
	}

	bool DepthActive()
	{
		return depthActive.load(std::memory_order_relaxed);
	}

	bool DrawsHand()
	{
		return drawsHand.load(std::memory_order_relaxed);
	}

	void UpdateOcclusion(const McVec& a_eyeMc, int a_budget)
	{
		struct Job
		{
			std::uint64_t key;
			std::size_t   face;
			McVec         centre;
			bool          visible;
		};
		std::vector<Job> jobs;
		{
			std::lock_guard lock(facesMutex);
			std::size_t     total = 0;
			for (const auto& [key, f] : faces) {
				total += f.visible.size();
			}
			if (total == 0 || a_budget <= 0) {
				return;
			}
			const std::size_t want = std::min<std::size_t>(std::size_t(a_budget), total);
			faceCursor %= total;
			// The faces at global indices [cursor, cursor + want), wrapping round.
			std::size_t base = 0;
			for (int pass = 0; pass < 2 && jobs.size() < want; ++pass) {
				base = pass == 0 ? 0 : total;
				for (const auto& [key, f] : faces) {
					const std::size_t n = f.visible.size();
					for (std::size_t i = 0; i < n && jobs.size() < want; ++i) {
						const std::size_t index = base + i;
						if (index < faceCursor || index >= faceCursor + want) {
							continue;
						}
						jobs.push_back({ key, i,
							McVec{ f.sx * 16.0 + f.centres[i * 3], f.sy * 16.0 + f.centres[i * 3 + 1], f.sz * 16.0 + f.centres[i * 3 + 2] }, true });
					}
					base += n;
				}
			}
			faceCursor = (faceCursor + want) % total;
		}

		// A face's centre lies on the block's surface, which may rest on Cyberpunk's ground: stop
		// the ray a little short, and count only what stands well in front of it.
		auto& collision = Collision::Get();
		for (auto& job : jobs) {
			job.visible = !collision.Blocked(a_eyeMc, job.centre, 0.15);
		}

		std::lock_guard lock(facesMutex);
		for (const auto& job : jobs) {
			const auto found = faces.find(job.key);
			if (found != faces.end() && job.face < found->second.visible.size()) {
				found->second.visible[job.face] = job.visible ? 1 : 0;
			}
		}
	}

	void Shutdown()
	{
		g = State{};
		drawsHand = false;
		std::lock_guard lock(facesMutex);
		faces.clear();
	}
}
