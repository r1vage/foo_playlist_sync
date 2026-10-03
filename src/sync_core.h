#pragma once

// Core one-way sync engine. Deliberately free of foobar2000 SDK dependencies so it can be
// unit-tested on its own; the component feeds it pre-gathered track data (see sync_task.cpp).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace psync {

	enum class ExistingPolicy : int {
		SizeOrDate = 0, // replace when size or modification date differs
		TagsOnly = 1,   // replace only when tags (and optionally technical info) differ
		Never = 2,      // same path + name counts as identical
	};

	struct Options {
		std::wstring destRoot;
		ExistingPolicy policy = ExistingPolicy::SizeOrDate;
		bool compareTech = false;        // TagsOnly: also replace when codec/bitrate/length differ
		bool preserveTimestamps = true;  // copy created + modified time from the source file
		bool removeOrphans = false;      // delete destination files not part of this sync
		bool removeOnlyManaged = true;   // ...but only files previously copied by us (listed in manifest)
		std::wstring removalScope;       // when removing everything: only look inside this folder (relative, may be empty)
		bool writePlaylists = false;
		std::wstring playlistSubdir;     // relative to destRoot, may be empty
		bool playlistUnixStyle = false;  // forward slashes + LF line endings
		bool createNomedia = false;      // put an empty .nomedia in the destination folder (hides it from Android media scanners)
		bool dryRun = false;
	};

	struct SourceFile {
		std::wstring sourcePath;  // native path (C:\..., \\server\share\...)
		std::wstring destRel;     // path relative to destRoot, backslash separated
		uint64_t tagHash = 0;
		uint64_t techHash = 0;
		std::wstring error;       // non-empty: track could not be prepared (e.g. not a local file)
	};

	struct PlaylistTrack {
		std::wstring destRel;
		int durationSec = -1;
		std::wstring displayTitle;  // "Artist - Title" for #EXTINF
	};

	struct PlaylistOut {
		std::wstring name;
		std::vector<PlaylistTrack> tracks;
	};

	enum class Action { Added, Replaced, Removed, Skipped, Error, Playlist };

	const wchar_t* actionName(Action a, bool dryRun);

	struct ReportEntry {
		Action action;
		std::wstring destRel;
		std::wstring source;
		uint64_t size = 0;
		std::wstring reason;
	};

	struct Report {
		std::wstring destRoot;
		bool dryRun = false;
		bool aborted = false;
		std::wstring fatalError;
		std::vector<ReportEntry> entries;
		uint64_t bytesCopied = 0;
		uint64_t bytesRemoved = 0;
		double seconds = 0;
		int64_t startedAt = 0; // FILETIME of start (local time is derived when displaying)

		size_t count(Action a) const;
	};

	// Progress/abort sink. abortRequested() is polled; the engine stops cleanly when it returns true.
	class Progress {
	public:
		virtual ~Progress() = default;
		virtual bool abortRequested() = 0;
		virtual void setPhase(const wchar_t* phase) = 0;
		virtual void setProgress(double fraction) = 0;
		virtual void setFileProgress(double fraction) = 0;
		virtual void setItem(const std::wstring& item) = 0;
	};

	// Reads tags of a file on the destination; used when the manifest has no record of it.
	using DestTagReader = std::function<bool(const std::wstring& nativePath, uint64_t& tagHash, uint64_t& techHash)>;

	class DestFs;

	// Opens opt.destRoot (folder or mtp://device/storage/path) and syncs to it.
	Report runSync(const Options& opt, std::vector<SourceFile> files, const std::vector<PlaylistOut>& playlists,
		Progress& progress, const DestTagReader& readDestTags);
	// Same, with a caller-supplied destination (tests).
	Report runSync(const Options& opt, DestFs& fs, std::vector<SourceFile> files, const std::vector<PlaylistOut>& playlists,
		Progress& progress, const DestTagReader& readDestTags);

	// Exposed for the component and tests.
	constexpr const wchar_t* kManifestName = L".playlist_sync";
	constexpr const wchar_t* kNomediaName = L".nomedia";
	std::wstring makePlaylistFileName(const std::wstring& playlistName);
	std::wstring pathKey(const std::wstring& path); // case-insensitive comparison key
	uint64_t fnv1a(const void* data, size_t len, uint64_t seed = 14695981039346656037ull);
	bool wildcardMatchNoCase(const wchar_t* pattern, const wchar_t* text);
}
