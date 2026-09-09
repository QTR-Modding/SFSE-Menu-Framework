#include "appearance/WallpaperImage.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <system_error>

namespace SFSEMenuFramework
{
	namespace
	{
		using Microsoft::WRL::ComPtr;
		constexpr std::uintmax_t maximumFileBytes = 32 * 1024 * 1024;
		constexpr UINT maximumDimension = 4096;

		struct ComScope final
		{
			HRESULT Result{ ::CoInitializeEx(nullptr, COINIT_MULTITHREADED) };
			~ComScope() { if (SUCCEEDED(Result)) { ::CoUninitialize(); } }
		};

		[[nodiscard]] std::filesystem::path ResolveImagePath(
			const std::filesystem::path& a_directory, std::string_view a_name)
		{
			if (a_name.empty() || a_name.size() > 240 ||
				std::ranges::any_of(a_name, [](unsigned char c) {
					return c < 0x20 || c > 0x7E || c == ':';
				})) {
				return {};
			}
			const std::filesystem::path relative{ a_name };
			if (relative.has_root_path() ||
				std::ranges::any_of(relative, [](const auto& part) { return part == ".."; })) {
				return {};
			}
			auto extension = relative.extension().string();
			std::ranges::transform(extension, extension.begin(), [](char c) {
				return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
			});
			if (extension != ".png" && extension != ".jpg" && extension != ".jpeg") {
				return {};
			}

			std::error_code error;
			const auto root = std::filesystem::weakly_canonical(a_directory, error);
			if (error || root.empty()) {
				return {};
			}
			const auto candidate = std::filesystem::weakly_canonical(root / relative, error);
			if (error) {
				return {};
			}
			const auto [rootEnd, candidateEnd] = std::mismatch(
				root.begin(), root.end(), candidate.begin(), candidate.end());
			return rootEnd == root.end() && candidateEnd != candidate.end() ? candidate :
				std::filesystem::path{};
		}
	}

	std::shared_ptr<const WallpaperImage> LoadWallpaperImage(
		const std::filesystem::path& a_themeDirectory, std::string_view a_relativePath)
	{
		const auto path = ResolveImagePath(a_themeDirectory, a_relativePath);
		std::error_code error;
		const auto size = path.empty() ? 0 : std::filesystem::file_size(path, error);
		if (error || size == 0 || size > maximumFileBytes) {
			return {};
		}
		std::ifstream file{ path, std::ios::binary };
		std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
		if (!file.read(reinterpret_cast<char*>(bytes.data()),
			static_cast<std::streamsize>(bytes.size())) ||
			file.peek() != std::char_traits<char>::eof()) {
			return {};
		}

		// Accept an existing STA too; balance only our own successful COM init.
		const ComScope com;
		if (FAILED(com.Result) && com.Result != RPC_E_CHANGED_MODE) {
			return {};
		}
		ComPtr<IWICImagingFactory> factory;
		ComPtr<IWICStream> stream;
		ComPtr<IWICBitmapDecoder> decoder;
		ComPtr<IWICBitmapFrameDecode> frame;
		ComPtr<IWICFormatConverter> converter;
		if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
			CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()))) ||
			FAILED(factory->CreateStream(stream.GetAddressOf())) ||
			FAILED(stream->InitializeFromMemory(bytes.data(), static_cast<DWORD>(bytes.size()))) ||
			FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
				WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf()))) {
			return {};
		}
		GUID format{};
		UINT frames{};
		if (FAILED(decoder->GetContainerFormat(&format)) ||
			(format != GUID_ContainerFormatPng && format != GUID_ContainerFormatJpeg) ||
			FAILED(decoder->GetFrameCount(&frames)) || frames != 1 ||
			FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) {
			return {};
		}
		auto image = std::make_shared<WallpaperImage>();
		if (FAILED(frame->GetSize(&image->Width, &image->Height)) ||
			image->Width == 0 || image->Height == 0 ||
			image->Width > maximumDimension || image->Height > maximumDimension ||
			FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) ||
			FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
				WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
			return {};
		}
		const UINT stride = image->Width * 4;
		image->Pixels.resize(static_cast<std::size_t>(stride) * image->Height);
		if (FAILED(converter->CopyPixels(nullptr, stride,
			static_cast<UINT>(image->Pixels.size()), image->Pixels.data()))) {
			return {};
		}
		return image;
	}
}
