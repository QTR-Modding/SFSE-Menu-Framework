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

		thread_local std::span<const WeightRequest> requestedWeights;

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

		[[nodiscard]] const WeightRequest* FindWeightRequest(
			std::span<const std::uint8_t> a_fontBytes) noexcept
		{
			for (const auto& request : requestedWeights) {
				if (request.FontBytes.data() == a_fontBytes.data() &&
					request.FontBytes.size() == a_fontBytes.size()) {
					return &request;
				}
			}
			return nullptr;
		}

		[[nodiscard]] bool HasPrintableAscii(FT_Face a_face) noexcept
		{
			for (FT_ULong codepoint = 0x20; codepoint <= 0x7E; ++codepoint) {
				if (FT_Get_Char_Index(a_face, codepoint) == 0) {
					return false;
				}
			}
			return true;
		}
	}

	ScopedWeightTable::ScopedWeightTable(
		std::span<const WeightRequest> a_requests) noexcept :
		previous_(requestedWeights)
	{
		requestedWeights = a_requests;
	}
	ScopedWeightTable::~ScopedWeightTable() noexcept {
		requestedWeights = previous_;
	}

	bool ApplyRequestedWeight(
		FT_Library a_library,
		FT_Face a_face,
		std::span<const std::uint8_t> a_fontBytes) noexcept
	{
		const auto* request = FindWeightRequest(a_fontBytes);
		if (!request) {
			return true;
		}
		if (!a_library || !a_face) {
			return false;
		}
		MultipleMasterHandle variation{ a_library };
		if (FT_Get_MM_Var(a_face, &variation.Value) != 0 || !variation.Value) {
			return false;
		}
		FT_UInt weightIndex{};
		const auto* weightAxis = FindWeightAxis(*variation.Value, &weightIndex);
		if (!weightAxis) {
			return false;
		}
		if (!std::isfinite(request->Weight) ||
			variation.Value->num_axis == 0 ||
			variation.Value->num_axis >
				(std::numeric_limits<std::size_t>::max)() / sizeof(FT_Fixed) ||
			weightAxis->minimum > weightAxis->maximum) {
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

		const auto requested = std::clamp(
			static_cast<double>(request->Weight),
			static_cast<double>(weightAxis->minimum) / fixedPointScale,
			static_cast<double>(weightAxis->maximum) / fixedPointScale);
		coordinates[weightIndex] = static_cast<FT_Fixed>(
			std::llround(requested * fixedPointScale));
		return FT_Set_Var_Design_Coordinates(a_face, variation.Value->num_axis,
			coordinates.get()) == 0;
	}

	FontInspection InspectFont(
		std::span<const std::uint8_t> a_fontBytes) noexcept
	{
		FontInspection inspection;
		if (a_fontBytes.empty() ||
			a_fontBytes.size() > static_cast<std::size_t>(
				(std::numeric_limits<FT_Long>::max)())) {
			return inspection;
		}

		LibraryHandle library;
		if (FT_Init_FreeType(&library.Value) != 0 || !library.Value) {
			return inspection;
		}

		FaceHandle face;
		if (FT_New_Memory_Face(library.Value, a_fontBytes.data(),
				static_cast<FT_Long>(a_fontBytes.size()), 0, &face.Value) != 0 ||
			!face.Value) {
			return inspection;
		}
		inspection.Kind = FontKind::Fixed;
		inspection.HasUnicodeCharmap =
			FT_Select_Charmap(face.Value, FT_ENCODING_UNICODE) == 0;
		inspection.HasPrintableAscii = inspection.HasUnicodeCharmap &&
			HasPrintableAscii(face.Value);

		MultipleMasterHandle variation{ library.Value };
		if (FT_Get_MM_Var(face.Value, &variation.Value) != 0 || !variation.Value) {
			return inspection;
		}
		const auto* axis = FindWeightAxis(*variation.Value);
		if (!axis || axis->minimum > axis->def || axis->def > axis->maximum) {
			return inspection;
		}
		const auto fixed = [](FT_Fixed a_value) {
			return static_cast<float>(static_cast<double>(a_value) / fixedPointScale);
		};
		inspection.Kind = FontKind::VariableWeight;
		inspection.Weight = WeightAxis{
			fixed(axis->minimum), fixed(axis->def), fixed(axis->maximum) };
		return inspection;
	}

}
