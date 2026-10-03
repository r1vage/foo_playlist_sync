#pragma once

#include "sync_core.h"

namespace psync {
	std::wstring formatBytesShort(uint64_t bytes);
	std::wstring formatTimestamp(int64_t fileTimeUtc, bool forFileName);
	std::wstring reportSummary(const Report& r);       // two/three line overview for the dialog header
	std::wstring reportAsText(const Report& r);         // full human readable log
	std::wstring reportAsCsv(const Report& r);
}
