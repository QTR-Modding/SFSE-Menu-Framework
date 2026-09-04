#include "runtime/HudManager.h"

#include "appearance/fonts/ConsumerFontScope.h"
#include "runtime/CallbackRegistry.h"

#include <new>

// Public names, persistent-frame behavior, and registration-order dispatch
// are adapted from SKSE Menu Framework 3 commit 928e01a (GPL-3.0),
// specifically include/HudManager.h and src/HudManager.cpp. Immutable
// snapshots and quiescent unregister are Starfield-specific.
namespace SFSEMenuFramework::HudManager
{
	namespace
	{
		using Registry = Detail::CallbackRegistry<
			Model::ClientHudElementRenderFunction,
			Model::HudElementHandle>;

		[[nodiscard]] Registry* GetRegistry() noexcept
		{
			static auto* registry = new (std::nothrow) Registry();
			return registry;
		}
	}

	Model::HudElementHandle Register(
		Model::ClientHudElementRenderFunction a_callback) noexcept
	{
		if (!a_callback) {
			return 0;
		}

		auto* registry = GetRegistry();
		return registry ? registry->Register(a_callback) : 0;
	}

	void Unregister(Model::HudElementHandle a_handle) noexcept
	{
		if (auto* registry = GetRegistry()) {
			registry->Unregister(a_handle);
		}
	}

	void Render() noexcept
	{
		if (auto* registry = GetRegistry()) {
			registry->Dispatch([](const auto& a_entry) noexcept {
				ConsumerFontScope::CallbackScope callbackScope{ "HUD" };
				a_entry.Function();
			});
		}
	}
}
