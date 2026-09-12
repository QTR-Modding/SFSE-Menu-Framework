#pragma once

#include <cstdint>
#include <mutex>

namespace SFSEMenuFramework
{
	class BlockingWindowVisibility
	{
	public:
		bool Refresh(std::uint64_t generation, bool stillOpen, std::uint64_t now)
		{
			if (!generation || !stillOpen) { return false; }
			std::scoped_lock lock{ mutex };
			visibleGeneration = generation;
			lastVisible = now;
			return true;
		}
		bool IsRecent(std::uint64_t generation, std::uint64_t now) const
		{
			std::scoped_lock lock{ mutex };
			return generation && generation == visibleGeneration && lastVisible && now - lastVisible <= maximumAgeMs;
		}
	private:
		static constexpr std::uint64_t maximumAgeMs = 250;
		mutable std::mutex mutex;
		std::uint64_t visibleGeneration{}, lastVisible{};
	};
}
