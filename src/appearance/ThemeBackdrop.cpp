#include "appearance/ThemeBackdrop.h"

#include <imgui_internal.h>

#include <cmath>
#include <cstdint>


namespace SFSEMenuFramework::ThemeBackdrop
{
	namespace
	{
		[[nodiscard]] std::uint32_t Mix(std::uint32_t a_value) noexcept
		{
			a_value ^= a_value >> 16;
			a_value *= 0x7FEB352DU;
			a_value ^= a_value >> 15;
			a_value *= 0x846CA68BU;
			return a_value ^ (a_value >> 16);
		}

		[[nodiscard]] float Unit(std::uint32_t a_value) noexcept
		{
			return static_cast<float>(a_value & 0x00FFFFFFU) /
				static_cast<float>(0x01000000U);
		}

	}

	void RenderCurrentWindow(const Style& a_style, float a_opacity) noexcept
	{
		if (a_style.Kind != Effect::Stars || a_opacity <= 0.0F ||
			ImGui::GetCurrentContext() == nullptr) {
			return;
		}

		const auto* window = ImGui::GetCurrentWindow();
		const auto& padding = window->WindowPadding;
		const auto windowSeed = Mix(static_cast<std::uint32_t>(window->ID));
		const ImVec2 minimum{
			window->InnerRect.Min.x + padding.x,
			window->InnerRect.Min.y + padding.y
		};
		const ImVec2 maximum{
			window->InnerRect.Max.x - padding.x,
			window->InnerRect.Max.y - padding.y
		};
		if (maximum.x <= minimum.x || maximum.y <= minimum.y) {
			return;
		}

		constexpr float cellSize = 68.0F;
		const auto columns = static_cast<std::uint32_t>(
			std::ceil((maximum.x - minimum.x) / cellSize));
		const auto rows = static_cast<std::uint32_t>(
			std::ceil((maximum.y - minimum.y) / cellSize));
		auto* drawList = ImGui::GetWindowDrawList();
		drawList->PushClipRect(minimum, maximum, true);

		for (std::uint32_t row = 0; row < rows; ++row) {
			for (std::uint32_t column = 0; column < columns; ++column) {
				const auto seed = Mix(
					0xA511E9B3U ^ windowSeed ^
					column * 0x9E3779B9U ^ row * 0x85EBCA6BU);
				if (Unit(seed) > a_style.Density) {
					continue;
				}

				const auto xSeed = Mix(seed ^ 0x68BC21EBU);
				const auto ySeed = Mix(seed ^ 0x02E5BE93U);
				const ImVec2 position{
					minimum.x + (static_cast<float>(column) + 0.12F +
						Unit(xSeed) * 0.76F) * cellSize,
					minimum.y + (static_cast<float>(row) + 0.12F +
						Unit(ySeed) * 0.76F) * cellSize
				};
				if (position.x >= maximum.x || position.y >= maximum.y) {
					continue;
				}

				const bool accent = (seed & 0x1FU) == 0;
				auto color = accent ? a_style.AccentColor : a_style.StarColor;
				color.w *= a_opacity *
					(0.65F + Unit(Mix(seed ^ 0xB5297A4DU)) * 0.35F);
				const float radius = (accent ? 1.35F : 0.85F) +
					Unit(Mix(seed ^ 0x1B56C4E9U)) * 0.85F;
				const auto packedColor = ImGui::GetColorU32(color);
				drawList->AddCircleFilled(position, radius, packedColor, 6);
				if (accent) {
					const float arm = radius * 2.4F;
					drawList->AddLine(
						ImVec2{ position.x - arm, position.y },
						ImVec2{ position.x + arm, position.y }, packedColor, 1.0F);
					drawList->AddLine(
						ImVec2{ position.x, position.y - arm },
						ImVec2{ position.x, position.y + arm }, packedColor, 1.0F);
				}
			}
		}

		drawList->PopClipRect();
	}

}
