#pragma once

namespace SFSEMenuFramework::LifecycleProbe
{
	void RecordInputCallback(bool a_nonEmptyQueue) noexcept;
	void RecordPostDataLoad() noexcept;
	void Flush() noexcept;
}
