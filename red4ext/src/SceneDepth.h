#pragma once

// Cyberpunk's depth buffer, borrowed from NVIDIA Streamline (sl.interposer.dll), which the game
// hands it every frame for DLSS: slSetTag names the depth resource and the state it is in,
// slSetConstants the camera's near and far planes, and slEvaluateFeature gives the game's command
// list at the moment DLSS runs. A copy is recorded into that same command list, so blocks can be
// hidden pixel by pixel behind everything the game drew (the city, cars, people).
//
// Without DLSS (or another Streamline feature that takes depth) there is nothing to borrow, and
// World falls back to ray-testing whole block faces.
//
// The same tags carry the game's picture without its HUD and the HUD alone, which it hands DLSS
// frame generation (only while that's on): with them blocks go under the HUD instead of over it.
namespace cybercraft::SceneDepth
{
	// Hooks the three Streamline exports. Call once, at plugin load.
	void Install(RED4ext::v1::PluginHandle a_handle, const RED4ext::v1::Sdk* a_sdk);
	void Uninstall();

	struct View
	{
		ID3D12Resource* texture{ nullptr };  // the copy: a typeless depth format, PIXEL_SHADER_RESOURCE
		DXGI_FORMAT     srvFormat{ DXGI_FORMAT_UNKNOWN };
		UINT            width{ 0 }, height{ 0 };  // the copy's size (the game's render resolution)
		UINT            left{ 0 }, top{ 0 }, extentWidth{ 0 }, extentHeight{ 0 };  // the area in use
		float           cameraNear{ 0.0f }, cameraFar{ 0.0f };
		bool            inverted{ false };
	};

	// This frame's depth copy, if one was made in the last moments. The texture stays valid until
	// the next call (a replaced one is kept alive for a few frames). Present thread.
	bool Acquire(View& a_out);

	struct Hud
	{
		ID3D12Resource* hudless{ nullptr };  // the frame without its HUD: a copy, PIXEL_SHADER_RESOURCE
		DXGI_FORMAT     hudlessFormat{ DXGI_FORMAT_UNKNOWN };
		UINT            hudlessLeft{ 0 }, hudlessTop{ 0 };  // its texel under the back buffer's pixel (0, 0)
		ID3D12Resource* ui{ nullptr };  // the HUD alone, its alpha how much of it covers a pixel
		DXGI_FORMAT     uiFormat{ DXGI_FORMAT_UNKNOWN };
		UINT            uiLeft{ 0 }, uiTop{ 0 };
	};

	// This frame's HUD-less picture and HUD, those the game tagged since the last Present that cover
	// an a_width x a_height back buffer. Buffers only valid until Present are copied in a_list, which
	// must run before the game's Present. The copies stay valid until the next call. Present thread.
	bool AcquireHud(ID3D12GraphicsCommandList* a_list, UINT a_width, UINT a_height, Hud& a_out);

	// Forgets this frame's tags: they belong to the Present they come before. Present thread, every
	// Present, drawn or not.
	void EndFrame();
}
