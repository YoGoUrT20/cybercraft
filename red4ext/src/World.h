#pragma once

#include "Link.h"

// The blocks the player places, drawn in Cyberpunk's frame (DESIGN.md §9, Phase 4).
//
// Minecraft builds the meshes with its own block renderer - models, tint, ambient occlusion,
// lighting - and ships them over the render ring together with its block atlas, so what gets
// drawn here is Minecraft's geometry rather than a reimplementation of it.
//
// Blocks are drawn from Cyberpunk's own camera (Game.cpp's CameraView) into its finished frame,
// under its HUD when the game hands frame generation its picture without the HUD (SceneDepth),
// over the HUD without it. They're hidden pixel by pixel behind the city, cars and people with
// Cyberpunk's depth (SceneDepth, DLSS); without it the city hides blocks only face by face:
// UpdateOcclusion casts a ray from the camera to each face and hides the faces something static
// stands in front of.
namespace cybercraft::World
{
	// Creates the pipelines and the targets for this back buffer's size and format. Cheap once ready.
	bool Prepare(ID3D12Device* a_device, DXGI_FORMAT a_format, UINT a_width, UINT a_height);

	// Drains the render ring: atlas uploads, section meshes, removals. Render thread.
	void Consume();

	// Draws every section the player has built, from Cyberpunk's camera (Minecraft's without one),
	// into the back buffer (a_rtv, RENDER_TARGET) under the game's HUD, and the hand over it all.
	// After CaptureScene.
	void Draw(ID3D12GraphicsCommandList* a_commandList, const proto::McState& a_state, UINT a_width, UINT a_height,
		D3D12_CPU_DESCRIPTOR_HANDLE a_rtv);

	// Copies Cyberpunk's finished picture (the back buffer, in PRESENT state, left in it), for the
	// blocks to take their light from and to be mixed into, and this frame's HUD-less picture and
	// HUD. Before anything of ours is drawn into it.
	void CaptureScene(ID3D12GraphicsCommandList* a_commandList, ID3D12Resource* a_backBuffer);

	// Blocks were depth-tested against Cyberpunk's own depth on the last frame (SceneDepth).
	bool DepthActive();

	// The first-person hand and held item are drawn here, lit like the blocks (kRenHand), so
	// Minecraft should leave them out of its overlay (kCyberDrawsHand). [World] bLitHand.
	bool DrawsHand();

	// Ray-tests up to a_budget block faces against Cyberpunk's city from the camera eye (Minecraft
	// coordinates), round robin over all of them. Game thread (ray casts are main-thread only).
	// Only needed without Cyberpunk's depth.
	void UpdateOcclusion(const McVec& a_eyeMc, int a_budget);

	void Shutdown();
}
