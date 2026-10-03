#pragma once

// Minecraft's hand, hotbar, GUI screens and crosshair, composited over Cyberpunk's frame
// (DESIGN.md §9, Phase 2). Minecraft renders them into the shared-memory triple buffer with
// premultiplied alpha; this draws that image over the back buffer just before the game presents.
//
// Cyberpunk is D3D12, so this owns its own command list, and because D3D12 keeps
// pipeline state per command list there is no save/restore dance around the game's state.
namespace cybercraft::Overlay
{
	// Patches IDXGISwapChain::Present and ID3D12CommandQueue::ExecuteCommandLists once, and safe to
	// call before the game's swap chain exists: it builds a throwaway device of its own to find the
	// vtables, which are shared with the game's objects. Call every frame: it also follows the swap
	// chain the game presents through (Streamline's proxy) and hooks that one's Present, which is
	// where Minecraft is drawn, before frame generation.
	void Install();

	// Drops the GPU resources. The vtable patches stay: unpatching races the render thread.
	void Shutdown();
}
