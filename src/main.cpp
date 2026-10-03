#include "stdafx.h"
#include "version.h"

DECLARE_COMPONENT_VERSION(
	"Playlist Sync",
	PSYNC_VERSION_STRING,
	"One-way sync of foobar2000 playlists to a folder, USB drive or memory card.\n"
	"Copies new and changed files, optionally removes files that are no longer in the playlists, "
	"and shows a detailed report of every file added, replaced, removed or skipped.\n\n"
	"A modern x64 replacement for foo_ows (One Way Sync)."
);

VALIDATE_COMPONENT_FILENAME("foo_playlist_sync.dll");

FOOBAR2000_IMPLEMENT_CFG_VAR_DOWNGRADE;
