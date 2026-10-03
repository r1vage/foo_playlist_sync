#pragma once

#include "sync_core.h"

namespace psync::cfg {

	// Selected playlists, one per line: "{GUID}\tName". The GUID survives renames; the name is a fallback.
	extern cfg_string playlists;
	extern cfg_string destination;
	extern cfg_string pattern;
	extern cfg_int policy;              // psync::ExistingPolicy
	extern cfg_bool compareTech;
	extern cfg_string ignoreTags;       // ';'-separated, wildcards allowed
	extern cfg_bool preserveTimestamps;
	extern cfg_bool removeOrphans;
	extern cfg_bool removeOnlyManaged;
	extern cfg_bool writePlaylists;
	extern cfg_string playlistSubdir;
	extern cfg_bool playlistUnixStyle;
	extern cfg_bool createNomedia;
	extern cfg_bool copyArt;            // copy a cover image into each album folder
	extern cfg_string artNames;         // image file names to look for next to the tracks, in priority order
	extern cfg_string artDestName;      // file name on the destination, without extension
	extern cfg_bool artEmbedded;        // fall back to the embedded front cover
	extern cfg_bool saveLogs;           // write each report to profile\playlist_sync_logs (the report window always shows)
	extern const char* const defaultArtNames;
	extern const char* const defaultArtDestName;

	extern const char* const defaultPattern;
	pfc::string8 getPattern(); // configured pattern, or the default
	extern const char* const defaultIgnoreTags;

	struct SelectedPlaylist {
		GUID guid;
		pfc::string8 name;
	};
	std::vector<SelectedPlaylist> getSelectedPlaylists();
	void setSelectedPlaylists(const std::vector<SelectedPlaylist>& list);

	extern const GUID guidPreferencesPage;
	void showPreferences();
}
