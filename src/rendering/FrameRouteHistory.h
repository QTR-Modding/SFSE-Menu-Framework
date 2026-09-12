#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_set>

namespace SFSEMenuFramework
{
	class FrameRouteHistory
	{
	public:
		void Record(std::uint32_t a_frame, bool a_rendered)
		{
			std::scoped_lock lock{ mutex };
			if (a_rendered) { rendered.insert(a_frame); }
			else { rendered.erase(a_frame); }
		}

		bool HasUI(std::uint32_t a_frame)
		{
			std::scoped_lock lock{ mutex };
			// Engine presentation requests are FIFO. Retain the current frame for
			// additional windows/retries, and future recordings for their own Present.
			std::erase_if(rendered, [a_frame](std::uint32_t frame) {
				const auto age = a_frame - frame;
				return age != 0 && age < 0x80000000u;
			});
			return rendered.contains(a_frame);
		}

	private:
		std::mutex mutex;
		std::unordered_set<std::uint32_t> rendered;
	};
}
