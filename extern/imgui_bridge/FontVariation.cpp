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
		constexpr FT_ULong weightTag = FT_MAKE_TAG('w', 'g', 'h', 't');
		constexpr double fixedPointScale = 65536.0;

		thread_local std::optional<float> requestedWeight;

		template <class T, auto Destroy>
		class Handle final
		{
		public:
			~Handle() {
				if (Value) {
					static_cast<void>(Destroy(Value));
				}
			}
			T Value{};
		};
		using LibraryHandle = Handle<FT_Library, FT_Done_FreeType>;
		using FaceHandle = Handle<FT_Face, FT_Done_Face>;

		class MultipleMasterHandle final
		{
		public:
			explicit MultipleMasterHandle(FT_Library a_library) noexcept :
				Library(a_library) {}

			~MultipleMasterHandle() {
				if (Value) {
					static_cast<void>(FT_Done_MM_Var(Library, Value));
				}
			}
			FT_Library Library{};
			FT_MM_Var* Value{};
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

	}

	ScopedWeight::ScopedWeight(std::optional<float> a_weight) noexcept :
		previous_(requestedWeight)
	{
		requestedWeight = a_weight;
	}
	ScopedWeight::~ScopedWeight() { requestedWeight = previous_; }

	bool ApplyRequestedWeight(FT_Library a_library, FT_Face a_face) noexcept
	{
		if (!requestedWeight || !a_library || !a_face ||
			!std::isfinite(*requestedWeight)) {
			return !requestedWeight;
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
		return FT_Set_Var_Design_Coordinates(a_face, variation.Value->num_axis,
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
		if (FT_New_Memory_Face(library.Value, a_fontBytes.data(),
				static_cast<FT_Long>(a_fontBytes.size()), 0, &face.Value) != 0 ||
			!face.Value) {
			return std::nullopt;
		}
		MultipleMasterHandle variation{ library.Value };
		if (FT_Get_MM_Var(face.Value, &variation.Value) != 0 || !variation.Value) {
			return std::nullopt;
		}
		const auto* axis = FindWeightAxis(*variation.Value);
		if (!axis || axis->minimum > axis->def || axis->def > axis->maximum) {
			return std::nullopt;
		}
		const auto fixed = [](FT_Fixed a_value) {
			return static_cast<float>(static_cast<double>(a_value) / fixedPointScale);
		};
		return WeightAxis{
			fixed(axis->minimum), fixed(axis->def), fixed(axis->maximum) };
	}
}
