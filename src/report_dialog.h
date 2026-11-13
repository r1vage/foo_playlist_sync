#pragma once

#include "sync_core.h"

namespace psync {
	// Opens a modeless report window; must be called from the main thread.
	void showReportDialog(Report&& report);

	std::wstring logFolder(); // <profile>\playlist_sync_logs
	bool writeUtf8File(const std::wstring& path, const std::wstring& text); // with BOM
}
