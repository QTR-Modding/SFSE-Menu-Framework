#include "FontVariation.h"

#include FT_MULTIPLE_MASTERS_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>

namespace SFSEMenuFramework::FontVariation
{
	namespace
	{
		constexpr FT_ULong weightTag =
			(static_cast<FT_ULong>('w') << 24) |
			(static_cast<FT_ULong>('g') << 16) |
			(static_cast<FT_ULong>('h') << 8) |
			static_cast<FT_ULong>('t');
		constexpr double fixedPointScale = 65536.0;

		thread_local std::optional<float> requestedWeight;

		class LibraryHandle final
		{
		public:
			~LibraryHandle()
			{
				if (Value) {
					static_cast<void>(FT_Done_FreeType(Value));
				}
			}

			FT_Library Value{};
		};

		class FaceHandle final
		{
		public:
			~FaceHandle()
			{
				if (Value) {
					static_cast<void>(FT_Done_Face(Value));
				}
			}

			FT_Face Value{};
		};

		class MultipleMasterHandle final
		{
		public:
			explicit MultipleMasterHandle(FT_Library a_library) noexcept :
				library_(a_library)
			{}

			~MultipleMasterHandle()
			{
				if (Value) {
					static_cast<void>(FT_Done_MM_Var(library_, Value));
				}
			}

			FT_MM_Var* Value{};

		private:
			FT_Library library_{};
		};

		[[nodiscard]] const FT_Var_Axis* FindWeightAxis(
			const FT_MM_Var& a_variation,
			FT_UInt*         a_index = nullptr) noexcept
		{
			for (FT_UInt index = 0; index < a_variation.num_axis; ++index) {
				if (a_variation.axis[index].tag == weightTag) {
					if (a_index) {
						*a_index = index;
					}
					return &a_variation.axis[index];
				}
			}
			return nullptr;
		}

		[[nodiscard]] float FromFixed(FT_Fixed a_value) noexcept
		{
			return static_cast<float>(
				static_cast<double>(a_value) / fixedPointScale);
		}
	}

	ScopedWeight::ScopedWeight(std::optional<float> a_weight) noexcept :
		previous_(requestedWeight)
	{
		requestedWeight = a_weight;
	}

	ScopedWeight::~ScopedWeight()
	{
		requestedWeight = previous_;
	}

	bool ApplyRequestedWeight(FT_Library a_library, FT_Face a_face) noexcept
	{
		if (!requestedWeight) {
			return true;
		}
		if (!a_library || !a_face || !std::isfinite(*requestedWeight)) {
			return false;
		}

		MultipleMasterHandle variation{ a_library };
		if (FT_Get_MM_Var(a_face, &variation.Value) != 0 || !variation.Value) {
			return false;
		}

		FT_UInt weightIndex{};
		const auto* weightAxis = FindWeightAxis(*variation.Value, &weightIndex);
		if (!weightAxis || variation.Value->num_axis == 0 ||
			variation.Value->num_axis >
				(std::numeric_limits<std::size_t>::max)() / sizeof(FT_Fixed)) {
			return false;
		}

		auto coordinates = std::unique_ptr<FT_Fixed[]>{
			new (std::nothrow) FT_Fixed[variation.Value->num_axis]
		};
		if (!coordinates) {
			return false;
		}
		for (FT_UInt index = 0; index < variation.Value->num_axis; ++index) {
			coordinates[index] = variation.Value->axis[index].def;
		}

		const auto scaled = static_cast<FT_Fixed>(std::llround(
			static_cast<double>(*requestedWeight) * fixedPointScale));
		coordinates[weightIndex] =
			std::clamp(scaled, weightAxis->minimum, weightAxis->maximum);
		return FT_Set_Var_Design_Coordinates(
			a_face,
			variation.Value->num_axis,
			coordinates.get()) == 0;
	}

	std::optional<WeightAxis> InspectWeightAxis(
		std::span<const std::uint8_t> a_fontBytes) noexcept
	{
		if (a_fontBytes.empty() ||
			a_fontBytes.size() > static_cast<std::size_t>(
				(std::numeric_limits<FT_Long>::max)())) {
			return std::nullopt;
		}

		LibraryHandle library;
		if (FT_Init_FreeType(&library.Value) != 0 || !library.Value) {
			return std::nullopt;
		}

		FaceHandle face;
		if (FT_New_Memory_Face(
				library.Value,
				a_fontBytes.data(),
				static_cast<FT_Long>(a_fontBytes.size()),
				0,
				&face.Value) != 0 ||
			!face.Value) {
			return std::nullopt;
		}

		MultipleMasterHandle variation{ library.Value };
		if (FT_Get_MM_Var(face.Value, &variation.Value) != 0 ||
			!variation.Value) {
			return std::nullopt;
		}

		const auto* axis = FindWeightAxis(*variation.Value);
		if (!axis || axis->minimum > axis->def || axis->def > axis->maximum) {
			return std::nullopt;
		}
		return WeightAxis{
			.Minimum = FromFixed(axis->minimum),
			.Default = FromFixed(axis->def),
			.Maximum = FromFixed(axis->maximum)
		};
	}
}
