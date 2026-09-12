#include "rendering/StreamlineUIPrototype.h"

#include <Windows.h>
#include <d3d12.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <wrl/client.h>

namespace SFSEMenuFramework::StreamlineUIPrototype
{
	namespace
	{
		using Microsoft::WRL::ComPtr;

		struct StructType final
		{
			std::uint32_t Data1;
			std::uint16_t Data2;
			std::uint16_t Data3;
			std::uint8_t Data4[8];
		};

		struct BaseStructure final
		{
			void* Next;
			StructType Type;
			std::size_t Version;
		};

		struct Extent final
		{
			std::uint32_t Top;
			std::uint32_t Left;
			std::uint32_t Width;
			std::uint32_t Height;
		};

		struct Resource final
		{
			BaseStructure Base;
			std::uint8_t Type;
			std::uint8_t Padding[7];
			void* Native;
			void* Memory;
			void* View;
			std::uint32_t State;
			std::uint32_t Width;
			std::uint32_t Height;
			std::uint32_t NativeFormat;
			std::uint32_t MipLevels;
			std::uint32_t ArrayLayers;
			std::uint64_t GpuVirtualAddress;
			std::uint32_t Flags;
			std::uint32_t Usage;
			std::uint16_t InternalFlags;
			std::uint16_t Reserved;
		};

		struct ResourceTag final
		{
			BaseStructure Base;
			Resource* ResourceData;
			std::uint32_t Type;
			std::uint32_t Lifecycle;
			Extent Region;
		};

		static_assert(offsetof(Resource, Native) == 40);
		static_assert(offsetof(Resource, State) == 64);
		static_assert(sizeof(Resource) == 112);
		static_assert(offsetof(ResourceTag, ResourceData) == 32);
		static_assert(offsetof(ResourceTag, Type) == 40);
		static_assert(sizeof(ResourceTag) == 64);

		using Result = std::int32_t;
		using SetTagFn = Result (*)(const void*, const ResourceTag*, std::uint32_t, void*);

		constexpr std::uint32_t uiColorAndAlpha = 23;
		constexpr std::size_t descriptorCount = 16;
		constexpr StructType resourceTagType{
			0x4C6A5AAD, 0xB445, 0x496C,
			{ 0x87, 0xFF, 0x1A, 0xF3, 0x84, 0x5B, 0xE6, 0x53 }
		};
		constexpr GUID streamlineNativeInterface{
			0xADEC44E2,
			0x61F0,
			0x45C3,
			{ 0xAD, 0x9F, 0x1B, 0x37, 0x37, 0x92, 0x84, 0xFF }
		};

		struct TargetSlot final
		{
			ComPtr<ID3D12Resource> Resource;
		};

		struct State final
		{
			ComPtr<ID3D12Device> Device;
			ComPtr<ID3D12DescriptorHeap> RtvHeap;
			std::array<TargetSlot, descriptorCount> Targets{};
			UINT RtvStride{};
		};

		std::atomic<SetTagFn> originalSetTag{};
		std::atomic_flag installed{};
		std::atomic_flag markerLogged{};
		std::atomic_flag nativeListLogged{};
		std::atomic_flag rejectionLogged{};

		[[nodiscard]] State& GetState()
		{
			static auto* state = new State();
			return *state;
		}

		[[nodiscard]] std::mutex& GetMutex()
		{
			static auto* mutex = new std::mutex();
			return *mutex;
		}

		void LogRejection(const char* a_reason) noexcept
		{
			if (!rejectionLogged.test_and_set(std::memory_order_relaxed)) {
				logger::warn("Streamline UI overlay prototype rejected: {}", a_reason);
			}
		}

		[[nodiscard]] bool SameType(const StructType& a_left, const StructType& a_right) noexcept
		{
			return std::memcmp(&a_left, &a_right, sizeof(StructType)) == 0;
		}

		[[nodiscard]] bool SameIdentity(IUnknown* a_left, IUnknown* a_right) noexcept
		{
			if (!a_left || !a_right) {
				return false;
			}
			ComPtr<IUnknown> left;
			ComPtr<IUnknown> right;
			return SUCCEEDED(a_left->QueryInterface(IID_PPV_ARGS(left.GetAddressOf()))) &&
			       SUCCEEDED(a_right->QueryInterface(IID_PPV_ARGS(right.GetAddressOf()))) &&
			       left.Get() == right.Get();
		}

		[[nodiscard]] bool GetStreamlineNativeCommandList(
			ID3D12GraphicsCommandList* a_commandList,
			ComPtr<ID3D12GraphicsCommandList>& a_native) noexcept
		{
			a_native.Reset();
			if (!a_commandList) {
				return false;
			}

			ID3D12GraphicsCommandList* native{};
			if (FAILED(a_commandList->QueryInterface(
					streamlineNativeInterface,
					reinterpret_cast<void**>(&native))) ||
				!native) {
				return false;
			}

			a_native.Attach(native);
			if (native == a_commandList) {
				a_native.Reset();
				return false;
			}
			return true;
		}

		[[nodiscard]] bool GetRtvHandle(
			ID3D12Device* a_device,
			ID3D12Resource* a_target,
			D3D12_CPU_DESCRIPTOR_HANDLE& a_handle) noexcept
		{
			std::scoped_lock lock{ GetMutex() };
			auto& state = GetState();
			if (!state.Device) {
				D3D12_DESCRIPTOR_HEAP_DESC heap{};
				heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
				heap.NumDescriptors = static_cast<UINT>(descriptorCount);
				if (FAILED(a_device->CreateDescriptorHeap(
						&heap, IID_PPV_ARGS(state.RtvHeap.GetAddressOf())))) {
					return false;
				}
				state.Device = a_device;
				state.RtvStride =
					a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
			} else if (!SameIdentity(state.Device.Get(), a_device)) {
				return false;
			}

			std::size_t slot = descriptorCount;
			for (std::size_t index = 0; index < state.Targets.size(); ++index) {
				if (state.Targets[index].Resource &&
					SameIdentity(state.Targets[index].Resource.Get(), a_target)) {
					slot = index;
					break;
				}
				if (slot == descriptorCount && !state.Targets[index].Resource) {
					slot = index;
				}
			}
			if (slot == descriptorCount) {
				return false;
			}

			a_handle = state.RtvHeap->GetCPUDescriptorHandleForHeapStart();
			a_handle.ptr += slot * state.RtvStride;
			if (!state.Targets[slot].Resource) {
				D3D12_RENDER_TARGET_VIEW_DESC view{};
				view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
				view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
				a_device->CreateRenderTargetView(a_target, &view, a_handle);
				state.Targets[slot].Resource = a_target;
			}
			return true;
		}

		void DrawMarker(const ResourceTag& a_tag, void* a_commandBuffer) noexcept
		{
			if (!a_tag.ResourceData || !a_tag.ResourceData->Native) {
				return;
			}
			if (!a_commandBuffer) {
				LogRejection("UI tag has no command buffer");
				return;
			}
			if (a_tag.ResourceData->State == UINT_MAX) {
				LogRejection("UI resource state is unknown");
				return;
			}

			ComPtr<ID3D12Resource> target;
			if (FAILED(reinterpret_cast<IUnknown*>(a_tag.ResourceData->Native)->QueryInterface(
					IID_PPV_ARGS(target.GetAddressOf()))) || !target) {
				LogRejection("UI native resource is not ID3D12Resource");
				return;
			}
			const auto desc = target->GetDesc();
			if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
				desc.Format != DXGI_FORMAT_R16G16B16A16_TYPELESS ||
				desc.SampleDesc.Count != 1 ||
				(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0) {
				LogRejection("UI texture description is unsupported");
				return;
			}

			ComPtr<ID3D12GraphicsCommandList> commandList;
			if (FAILED(reinterpret_cast<IUnknown*>(a_commandBuffer)->QueryInterface(
					IID_PPV_ARGS(commandList.GetAddressOf()))) ||
				!commandList || commandList->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
				LogRejection("command buffer is not a direct D3D12 command list");
				return;
			}

			ComPtr<ID3D12GraphicsCommandList> nativeCommandList;
			if (GetStreamlineNativeCommandList(commandList.Get(), nativeCommandList)) {
				commandList = nativeCommandList;
				if (!nativeListLogged.test_and_set(std::memory_order_relaxed)) {
					logger::info("Streamline UI overlay prototype using native command list");
				}
			}

			ComPtr<ID3D12Device> targetDevice;
			ComPtr<ID3D12Device> listDevice;
			if (FAILED(target->GetDevice(IID_PPV_ARGS(targetDevice.GetAddressOf()))) ||
				FAILED(commandList->GetDevice(IID_PPV_ARGS(listDevice.GetAddressOf()))) ||
				!targetDevice || !listDevice) {
				LogRejection("could not resolve D3D12 devices");
				return;
			}
			if (!SameIdentity(targetDevice.Get(), listDevice.Get())) {
				LogRejection("command list and UI resource device identity differ");
				return;
			}

			D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
			if (!GetRtvHandle(targetDevice.Get(), target.Get(), rtv)) {
				LogRejection("could not allocate UI render-target view");
				return;
			}

			const auto originalState =
				static_cast<D3D12_RESOURCE_STATES>(a_tag.ResourceData->State);
			if (originalState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
				D3D12_RESOURCE_BARRIER barrier{};
				barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barrier.Transition.pResource = target.Get();
				barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				barrier.Transition.StateBefore = originalState;
				barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
				commandList->ResourceBarrier(1, &barrier);
			}

			constexpr float markerColor[4]{ 1.0f, 0.0f, 1.0f, 1.0f };
			const auto right = static_cast<LONG>(desc.Width < 432 ? desc.Width : 432);
			const auto bottom = static_cast<LONG>(desc.Height < 168 ? desc.Height : 168);
			if (right > 48 && bottom > 48) {
				const D3D12_RECT rect{ 48, 48, right, bottom };
				commandList->ClearRenderTargetView(rtv, markerColor, 1, &rect);
			}

			if (originalState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
				D3D12_RESOURCE_BARRIER barrier{};
				barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barrier.Transition.pResource = target.Get();
				barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
				barrier.Transition.StateAfter = originalState;
				commandList->ResourceBarrier(1, &barrier);
			}

			if (!markerLogged.test_and_set(std::memory_order_relaxed)) {
				logger::info("Streamline UI overlay prototype marker injected");
			}
		}

		Result SetTagThunk(
			const void* a_viewport,
			const ResourceTag* a_tags,
			std::uint32_t a_count,
			void* a_commandBuffer) noexcept
		{
			if (a_tags && a_count <= 64) {
				for (std::uint32_t index = 0; index < a_count; ++index) {
					const auto& tag = a_tags[index];
					if (SameType(tag.Base.Type, resourceTagType) &&
						tag.Type == uiColorAndAlpha) {
						DrawMarker(tag, a_commandBuffer);
						break;
					}
				}
			}

			const auto original = originalSetTag.load(std::memory_order_acquire);
			return original ? original(a_viewport, a_tags, a_count, a_commandBuffer) : -1;
		}

		[[nodiscard]] bool PatchImport(void** a_original) noexcept
		{
			auto* base = reinterpret_cast<std::byte*>(::GetModuleHandleW(nullptr));
			if (!base) {
				return false;
			}
			auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
				return false;
			}
			auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE) {
				return false;
			}
			const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
			if (!directory.VirtualAddress || !directory.Size) {
				return false;
			}

			auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
			for (; descriptor->Name; ++descriptor) {
				const auto* dll = reinterpret_cast<const char*>(base + descriptor->Name);
				if (_stricmp(dll, "sl.interposer.dll") != 0 || !descriptor->OriginalFirstThunk) {
					continue;
				}
				auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
				auto* entries = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
				for (; names->u1.AddressOfData; ++names, ++entries) {
					if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
						continue;
					}
					auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
					if (std::strcmp(reinterpret_cast<const char*>(import->Name), "slSetTag") != 0) {
						continue;
					}

					*a_original = reinterpret_cast<void*>(entries->u1.Function);
					DWORD oldProtect{};
					if (!::VirtualProtect(
							reinterpret_cast<void*>(&entries->u1.Function),
							sizeof(entries->u1.Function), PAGE_READWRITE, &oldProtect)) {
						*a_original = nullptr;
						return false;
					}
					entries->u1.Function = reinterpret_cast<ULONG_PTR>(&SetTagThunk);
					DWORD ignored{};
					static_cast<void>(::VirtualProtect(
						reinterpret_cast<void*>(&entries->u1.Function),
						sizeof(entries->u1.Function), oldProtect, &ignored));
					return true;
				}
			}
			return false;
		}
	}

	bool Install() noexcept
	{
		if (installed.test_and_set(std::memory_order_acq_rel)) {
			return originalSetTag.load(std::memory_order_acquire) != nullptr;
		}

		void* original{};
		if (!PatchImport(&original) || !original) {
			logger::warn("Streamline UI overlay prototype could not hook slSetTag");
			return false;
		}
		originalSetTag.store(reinterpret_cast<SetTagFn>(original), std::memory_order_release);
		logger::info("Streamline UI overlay prototype installed");
		return true;
	}
}
