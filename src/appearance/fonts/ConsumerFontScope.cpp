#include "appearance/fonts/ConsumerFontScope.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>

namespace SFSEMenuFramework::ConsumerFontScope
{
	namespace
	{
		thread_local CallbackScope* currentScope{};
	}

	CallbackScope::CallbackScope(const char* a_callbackKind) noexcept :
		_callbackKind(a_callbackKind ? a_callbackKind : "unknown"),
		_context(ImGui::GetCurrentContext()),
		_thread(std::this_thread::get_id()),
		_previous(currentScope)
	{
		// Always shadow an outer callback. If this scope is invalid, font API calls
		// made by the nested callback must not fall through to the outer scope.
		currentScope = this;
		if (!_context || !_context->WithinFrameScope) {
			return;
		}

		_baselineDepth = static_cast<std::size_t>(_context->FontStack.Size);
		if (_baselineDepth > maximumTrackedFonts) {
			logger::error(
				"Could not establish the consumer {} callback font scope: "
				"baseline depth {} exceeds the safety limit {}",
				_callbackKind,
				_baselineDepth,
				maximumTrackedFonts);
			return;
		}
		std::copy_n(
			_context->FontStack.begin(), _baselineDepth, _baselineFonts.begin());
		_active = true;
	}

	CallbackScope::~CallbackScope() noexcept
	{
		currentScope = _previous;
		if (!_active) {
			return;
		}
		if (!IsCurrentFrame()) {
			logger::error(
				"Could not restore the consumer {} callback font stack because "
				"its ImGui context or frame changed",
				_callbackKind);
			return;
		}

		auto& stack = _context->FontStack;
		const auto observedDepth = static_cast<std::size_t>(stack.Size);
		std::size_t commonDepth{};
		while (commonDepth < _baselineDepth && commonDepth < observedDepth &&
			_baselineFonts[commonDepth] == stack[static_cast<int>(commonDepth)]) {
			++commonDepth;
		}
		const bool changed = commonDepth != _baselineDepth ||
			observedDepth != _baselineDepth;

		if (changed) {
			while (static_cast<std::size_t>(stack.Size) > commonDepth) {
				ImGui::PopFont();
			}
			for (auto index = commonDepth; index < _baselineDepth; ++index) {
				ImGui::PushFont(_baselineFonts[index]);
			}
		}

		if (changed || _pushCount != 0) {
			logger::warn(
				"Restored an unbalanced consumer {} callback font stack "
				"(baseline {}, observed {}, outstanding framework pushes {})",
				_callbackKind,
				_baselineDepth,
				observedDepth,
				_pushCount);
		}
	}

	bool CallbackScope::IsCurrentFrame() const noexcept
	{
		return std::this_thread::get_id() == _thread &&
			ImGui::GetCurrentContext() == _context &&
			_context && _context->WithinFrameScope;
	}

	bool IsActive() noexcept
	{
		return currentScope && currentScope->_active &&
			currentScope->IsCurrentFrame();
	}

	bool Push(ImFont* a_font) noexcept
	{
		auto* scope = currentScope;
		if (!scope || !scope->_active || !scope->IsCurrentFrame() || !a_font ||
			!a_font->IsLoaded() ||
			a_font->ContainerAtlas != scope->_context->IO.Fonts ||
			scope->_pushCount == scope->_pushes.size()) {
			return false;
		}

		ImGui::PushFont(a_font);
		scope->_pushes[scope->_pushCount++] = {
			.Font = a_font,
			.Depth = static_cast<std::size_t>(scope->_context->FontStack.Size)
		};
		return true;
	}

	bool Pop() noexcept
	{
		auto* scope = currentScope;
		if (!scope || !scope->_active || !scope->IsCurrentFrame() ||
			scope->_pushCount == 0) {
			return false;
		}
		const auto& push = scope->_pushes[scope->_pushCount - 1];
		const auto& stack = scope->_context->FontStack;
		if (static_cast<std::size_t>(stack.Size) != push.Depth ||
			stack.back() != push.Font) {
			return false;
		}

		ImGui::PopFont();
		--scope->_pushCount;
		return true;
	}
}
