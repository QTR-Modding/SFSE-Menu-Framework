#pragma once

// Host-internal callback and state types. Client mods use SFSE-MCP.

#include <atomic>
#include <cstdint>

namespace RE
{
	class InputEvent;
}

namespace SFSEMenuFramework::Model
{
	using EventHandle = std::uint64_t;
	using InputEventHandle = std::uint64_t;
	using HudElementHandle = std::uint64_t;

	enum EventType : std::uint32_t
	{
		kNone = 0,
		kOpenMenu = 1,
		kCloseMenu = 2,
		kBeforeRender = 3,
		kAfterRender = 4
	};

	using ClientWindowRenderFunction = void(__stdcall*)();
	using EventCallback = void(__stdcall*)(EventType);
	using InputEventCallback = bool(__stdcall*)(RE::InputEvent*);
	using ClientHudElementRenderFunction = void(__stdcall*)();

	class WindowInterface
	{
	public:
		std::atomic<bool> IsOpen{ false };
		std::atomic<bool> BlockUserInput{ true };
	};

	static_assert(sizeof(void*) == 8);
	static_assert(std::atomic<bool>::is_always_lock_free);
}
