#pragma once

#include "sync_core.h"

namespace psync {
	// Opens a modeless report window; must be called from the main thread.
	void showReportDialog(Report&& report);
}
