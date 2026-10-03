#pragma once

// Destination storage abstraction: a local/USB/network folder (dest_local.cpp) or an MTP device (dest_mtp.cpp).
// All paths given to a DestFs are relative to its root and use backslashes.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace psync {

	struct DestStat {
		bool exists = false;
		bool isDir = false;
		uint64_t size = 0;
		int64_t mtime = 0; // FILETIME (UTC); 0 if unknown
	};

	struct CopyTimes {
		bool set = false;   // false: let the destination use the time of the copy
		int64_t ctime = 0;
		int64_t mtime = 0;
	};

	class CopyProgress {
	public:
		virtual ~CopyProgress() = default;
		virtual bool abortRequested() = 0;
		virtual void setFileProgress(double fraction) = 0;
	};

	class DestFs {
	public:
		virtual ~DestFs() = default;

		// Human readable destination for reports.
		virtual std::wstring describe() const = 0;

		// Locates (and with create=true creates) the root folder. When create is false a missing root
		// is not an error: rootMissing is set and the destination behaves as empty.
		virtual bool openRoot(bool create, bool& rootMissing, std::wstring& err) = 0;

		virtual DestStat stat(const std::wstring& rel) = 0;

		// Every file below the root (recursively), skipping system folders. Return false from cb to stop.
		virtual void listFiles(const std::function<bool(const std::wstring& rel, uint64_t size)>& cb) = 0;

		virtual bool ensureDir(const std::wstring& relDir, std::wstring& err) = 0;

		// Copies a local file to rel. Must never leave a partial file under the final name.
		virtual bool copyIn(const std::wstring& srcPath, uint64_t srcSize, const std::wstring& rel, bool replacing,
			const CopyTimes& times, CopyProgress& progress, bool& aborted, std::wstring& err) = 0;

		virtual bool remove(const std::wstring& rel, std::wstring& err) = 0;

		// Removes the folder if it has no children; returns true if it was removed.
		virtual bool removeDirIfEmpty(const std::wstring& relDir) = 0;

		// Small files (sync state, playlists).
		virtual bool readFile(const std::wstring& rel, std::string& out) = 0;
		virtual bool writeFile(const std::wstring& rel, const std::string& data, bool hidden, std::wstring& err) = 0;

		virtual bool freeSpace(uint64_t& bytes) = 0;

		// A local path for reading tags in place; false when the destination can't provide one (MTP).
		virtual bool localPath(const std::wstring& rel, std::wstring& out) { (void)rel; (void)out; return false; }

		// Whether modification dates on the destination reflect the source (false for MTP).
		virtual bool hasReliableDates() const { return true; }
	};

	constexpr const wchar_t* kMtpPrefix = L"mtp://";
	bool isMtpPath(const std::wstring& dest);

	std::unique_ptr<DestFs> createLocalFs(const std::wstring& root);
	std::unique_ptr<DestFs> createMtpFs(const std::wstring& url);
	inline std::unique_ptr<DestFs> openDestination(const std::wstring& dest) {
		return isMtpPath(dest) ? createMtpFs(dest) : createLocalFs(dest);
	}

	// Connected MTP devices and their storages, for the destination picker.
	struct MtpStorageInfo {
		std::wstring device;
		std::wstring storage;
		std::wstring url() const { return std::wstring(kMtpPrefix) + device + L"/" + storage; }
	};
	std::vector<MtpStorageInfo> listMtpStorages(bool includeMassStorage = false);

	// Shared helpers.
	std::wstring sysErrorText(unsigned long code);
	std::wstring parentRel(const std::wstring& rel);
}
