#pragma once

#include <array>
#include <cstddef>
#include <thread>

struct ImFont;
struct ImGuiContext;

namespace SFSEMenuFramework::ConsumerFontScope
{
	[[nodiscard]] bool IsActive() noexcept;
	[[nodiscard]] bool Push(ImFont* a_font) noexcept;
	[[nodiscard]] bool Pop() noexcept;

	class CallbackScope final
	{
	public:
		explicit CallbackScope(const char* a_callbackKind) noexcept;
		~CallbackScope() noexcept;

		CallbackScope(const CallbackScope&) = delete;
		CallbackScope(CallbackScope&&) = delete;
		CallbackScope& operator=(const CallbackScope&) = delete;
		CallbackScope& operator=(CallbackScope&&) = delete;

	private:
		struct PushRecord final
		{
			ImFont*     Font{};
			std::size_t Depth{};
		};

		static constexpr std::size_t maximumTrackedFonts = 64;

		friend bool IsActive() noexcept;
		friend bool Push(ImFont* a_font) noexcept;
		friend bool Pop() noexcept;

		[[nodiscard]] bool IsCurrentFrame() const noexcept;

		const char*          _callbackKind{ "unknown" };
		ImGuiContext*        _context{};
		std::thread::id      _thread{};
		std::array<ImFont*, maximumTrackedFonts> _baselineFonts{};
		std::size_t          _baselineDepth{};
		std::array<PushRecord, maximumTrackedFonts> _pushes{};
		std::size_t          _pushCount{};
		CallbackScope*       _previous{};
		bool                 _active{};
	};
}
