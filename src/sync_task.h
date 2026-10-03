#pragma once

namespace psync {
	// Gathers tracks from the configured playlists (main thread) and runs the sync in a worker
	// thread with a progress dialog; shows the report window when done.
	void startSync(bool dryRun);
	bool isSyncRunning();

	// Example of the file name pattern applied to a track, for the preferences page.
	pfc::string8 previewPattern(const char* pattern);
}
