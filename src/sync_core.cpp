#include "sync_core.h"
#include "dest_fs.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace psync {

	namespace {

		constexpr int64_t kTicksPerSecond = 10'000'000;      // FILETIME units
		constexpr int64_t kDateTolerance = 2 * kTicksPerSecond; // FAT stores times with 2 s resolution
		constexpr const wchar_t* kTempSuffix = L".psync-tmp";
		constexpr const wchar_t* kManifestHeader = L"#playlist_sync manifest 1";

		struct ManifestEntry {
			std::wstring destRel;
			uint64_t srcSize = 0;
			int64_t srcMtime = 0;
			uint64_t tagHash = 0;
			uint64_t techHash = 0;
		};
		using Manifest = std::unordered_map<std::wstring, ManifestEntry>; // keyed by pathKey(destRel)

		struct SrcStat {
			bool exists = false;
			bool isDir = false;
			uint64_t size = 0;
			int64_t mtime = 0;
			int64_t ctime = 0;
		};

		int64_t toInt64(const FILETIME& ft) {
			return (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
		}

		SrcStat statSource(const std::wstring& path) {
			SrcStat st;
			std::wstring lp = path.rfind(L"\\\\?\\", 0) == 0 ? path
				: path.rfind(L"\\\\", 0) == 0 ? L"\\\\?\\UNC\\" + path.substr(2) : L"\\\\?\\" + path;
			WIN32_FILE_ATTRIBUTE_DATA d;
			if (GetFileAttributesExW(lp.c_str(), GetFileExInfoStandard, &d)) {
				st.exists = true;
				st.isDir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
				st.size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
				st.mtime = toInt64(d.ftLastWriteTime);
				st.ctime = toInt64(d.ftCreationTime);
			}
			return st;
		}

		std::string toUtf8(const std::wstring& w) {
			if (w.empty()) return {};
			int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
			std::string s(n, '\0');
			WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
			return s;
		}
		std::wstring fromUtf8(const std::string& s) {
			if (s.empty()) return {};
			int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
			std::wstring w(n, L'\0');
			MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
			return w;
		}

		std::wstring hex64(uint64_t v) {
			wchar_t buf[17];
			swprintf_s(buf, L"%016llx", (unsigned long long)v);
			return buf;
		}

		Manifest loadManifest(DestFs& fs) {
			Manifest m;
			std::string raw;
			if (!fs.readFile(kManifestName, raw)) return m;
			std::wstring text = fromUtf8(raw);
			size_t pos = 0;
			bool first = true;
			while (pos < text.size()) {
				size_t eol = text.find(L'\n', pos);
				if (eol == std::wstring::npos) eol = text.size();
				std::wstring line = text.substr(pos, eol - pos);
				pos = eol + 1;
				if (!line.empty() && line.back() == L'\r') line.pop_back();
				if (first) {
					first = false;
					if (line != kManifestHeader) return {}; // unknown format: ignore rather than misinterpret
					continue;
				}
				if (line.empty() || line[0] == L'#') continue;
				std::vector<std::wstring> f;
				size_t s = 0;
				for (;;) {
					size_t t = line.find(L'\t', s);
					f.push_back(line.substr(s, t == std::wstring::npos ? std::wstring::npos : t - s));
					if (t == std::wstring::npos) break;
					s = t + 1;
				}
				if (f.size() < 5 || f[0].empty()) continue;
				ManifestEntry e;
				e.destRel = f[0];
				e.srcSize = _wcstoui64(f[1].c_str(), nullptr, 10);
				e.srcMtime = _wcstoi64(f[2].c_str(), nullptr, 10);
				e.tagHash = _wcstoui64(f[3].c_str(), nullptr, 16);
				e.techHash = _wcstoui64(f[4].c_str(), nullptr, 16);
				m[pathKey(e.destRel)] = std::move(e);
			}
			return m;
		}

		bool saveManifest(DestFs& fs, const Manifest& m, std::wstring& err) {
			std::vector<const ManifestEntry*> sorted;
			sorted.reserve(m.size());
			for (auto& kv : m) sorted.push_back(&kv.second);
			std::sort(sorted.begin(), sorted.end(), [](auto a, auto b) { return a->destRel < b->destRel; });
			std::wstring text = kManifestHeader;
			text += L"\n# destination\tsource size\tsource mtime\ttag hash\ttech hash\n";
			for (auto e : sorted) {
				text += e->destRel + L"\t" + std::to_wstring(e->srcSize) + L"\t" + std::to_wstring(e->srcMtime) + L"\t" +
					hex64(e->tagHash) + L"\t" + hex64(e->techHash) + L"\n";
			}
			return fs.writeFile(kManifestName, toUtf8(text), true, err);
		}

		bool isReservedDeviceName(const std::wstring& name) {
			std::wstring base = name.substr(0, name.find(L'.'));
			std::wstring up = pathKey(base);
			static const wchar_t* names[] = { L"CON", L"PRN", L"AUX", L"NUL",
				L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
				L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9" };
			for (auto n : names) if (up == n) return true;
			return false;
		}

		std::wstring sanitizeComponent(std::wstring c) {
			for (auto& ch : c) {
				if (ch < 0x20 || wcschr(L"<>:\"/\\|?*", ch)) ch = L'_';
			}
			while (!c.empty() && (c.back() == L'.' || c.back() == L' ')) c.pop_back();
			while (!c.empty() && c.front() == L' ') c.erase(c.begin());
			if (c.empty() || c == L"." || c == L"..") c = L"_";
			if (isReservedDeviceName(c)) c = L"_" + c;
			return c;
		}

		// Defensive normalization of a relative destination path; returns empty if unusable.
		std::wstring sanitizeRel(const std::wstring& rel) {
			std::wstring out;
			size_t s = 0;
			std::wstring r = rel;
			std::replace(r.begin(), r.end(), L'/', L'\\');
			while (s <= r.size()) {
				size_t t = r.find(L'\\', s);
				if (t == std::wstring::npos) t = r.size();
				if (t > s) {
					if (!out.empty()) out += L'\\';
					out += sanitizeComponent(r.substr(s, t - s));
				}
				s = t + 1;
			}
			return out;
		}

		std::wstring formatBytes(uint64_t b) {
			wchar_t buf[64];
			if (b >= (1ull << 30)) swprintf_s(buf, L"%.2f GB", b / double(1ull << 30));
			else if (b >= (1ull << 20)) swprintf_s(buf, L"%.1f MB", b / double(1ull << 20));
			else if (b >= 1024) swprintf_s(buf, L"%.0f KB", b / 1024.0);
			else swprintf_s(buf, L"%llu bytes", (unsigned long long)b);
			return buf;
		}

		bool datesMatch(int64_t a, int64_t b) {
			int64_t d = a > b ? a - b : b - a;
			if (d <= kDateTolerance) return true;
			// FAT drives store local time; a DST change shows up as an exact one hour shift.
			int64_t hour = 3600 * kTicksPerSecond;
			return (d > hour ? d - hour : hour - d) <= kDateTolerance;
		}

		struct PlanItem {
			size_t fileIdx;
			bool replace;
			std::wstring reason;
			SrcStat src;
			DestStat dst;
		};

		class FileProgressAdapter : public CopyProgress {
		public:
			explicit FileProgressAdapter(Progress& p) : m_p(p) {}
			bool abortRequested() override { return m_p.abortRequested(); }
			void setFileProgress(double f) override { m_p.setFileProgress(f); }
		private:
			Progress& m_p;
		};

		std::wstring playlistRelPath(const std::wstring& subdir, const std::wstring& destRel, bool unix) {
			std::wstring up;
			if (!subdir.empty()) {
				size_t depth = 1 + (size_t)std::count(subdir.begin(), subdir.end(), L'\\');
				for (size_t i = 0; i < depth; ++i) up += L"..\\";
			}
			std::wstring p = up + destRel;
			if (unix) std::replace(p.begin(), p.end(), L'\\', L'/');
			return p;
		}

	} // namespace

	const wchar_t* actionName(Action a, bool dryRun) {
		switch (a) {
		case Action::Added: return dryRun ? L"Would add" : L"Added";
		case Action::Replaced: return dryRun ? L"Would replace" : L"Replaced";
		case Action::Removed: return dryRun ? L"Would remove" : L"Removed";
		case Action::Skipped: return L"Skipped";
		case Action::Error: return L"Error";
		case Action::Playlist: return L"Playlist";
		}
		return L"?";
	}

	size_t Report::count(Action a) const {
		return (size_t)std::count_if(entries.begin(), entries.end(), [a](const ReportEntry& e) { return e.action == a; });
	}

	std::wstring pathKey(const std::wstring& path) {
		std::wstring s = path;
		std::replace(s.begin(), s.end(), L'/', L'\\');
		if (s.empty()) return s;
		std::wstring out(s.size(), L'\0');
		int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, s.data(), (int)s.size(), out.data(), (int)out.size(), nullptr, nullptr, 0);
		if (n <= 0) return s;
		out.resize(n);
		return out;
	}

	uint64_t fnv1a(const void* data, size_t len, uint64_t seed) {
		uint64_t h = seed;
		auto p = static_cast<const unsigned char*>(data);
		for (size_t i = 0; i < len; ++i) { h ^= p[i]; h *= 1099511628211ull; }
		return h;
	}

	bool wildcardMatchNoCase(const wchar_t* pat, const wchar_t* txt) {
		// Iterative '*' / '?' matcher with backtracking to the last star.
		const wchar_t* star = nullptr;
		const wchar_t* resume = nullptr;
		while (*txt) {
			if (*pat == L'*') { star = pat++; resume = txt; continue; }
			if (*pat == L'?' || (*pat && towupper(*pat) == towupper(*txt))) { ++pat; ++txt; continue; }
			if (star) { pat = star + 1; txt = ++resume; continue; }
			return false;
		}
		while (*pat == L'*') ++pat;
		return *pat == 0;
	}

	std::wstring makePlaylistFileName(const std::wstring& playlistName) {
		return sanitizeComponent(playlistName) + L".m3u8";
	}

	Report runSync(const Options& opt, std::vector<SourceFile> files, const std::vector<PlaylistOut>& playlists,
		Progress& progress, const DestTagReader& readDestTags) {
		if (opt.destRoot.empty()) {
			Report rep;
			rep.dryRun = opt.dryRun;
			rep.fatalError = L"No destination folder is set.";
			return rep;
		}
		auto fs = openDestination(opt.destRoot);
		return runSync(opt, *fs, std::move(files), playlists, progress, readDestTags);
	}

	Report runSync(const Options& opt, DestFs& fs, std::vector<SourceFile> files, const std::vector<PlaylistOut>& playlists,
		Progress& progress, const DestTagReader& readDestTags) {

		Report rep;
		rep.dryRun = opt.dryRun;
		const ULONGLONG t0 = GetTickCount64();
		{
			FILETIME now; GetSystemTimeAsFileTime(&now);
			rep.startedAt = toInt64(now);
		}
		auto finish = [&]() -> Report& { rep.seconds = (GetTickCount64() - t0) / 1000.0; return rep; };
		auto add = [&](Action a, const std::wstring& rel, const std::wstring& src, uint64_t size, const std::wstring& reason) {
			rep.entries.push_back(ReportEntry{ a, rel, src, size, reason });
		};
		auto checkAbort = [&]() {
			if (!rep.aborted && progress.abortRequested()) rep.aborted = true;
			return rep.aborted;
		};

		// ---- Open destination ----
		progress.setPhase(L"Opening destination");
		bool rootMissing = false;
		{
			std::wstring err;
			if (!fs.openRoot(!opt.dryRun, rootMissing, err)) {
				rep.destRoot = fs.describe();
				rep.fatalError = err;
				return finish();
			}
		}
		rep.destRoot = fs.describe();
		const bool reliableDates = fs.hasReliableDates();
		const std::wstring playlistSubdir = sanitizeRel(opt.playlistSubdir);

		const Manifest oldManifest = rootMissing ? Manifest{} : loadManifest(fs);
		Manifest newManifest;

		// ---- Phase 1: compare ----
		progress.setPhase(L"Comparing files");
		std::unordered_set<std::wstring> seenSources;
		std::unordered_map<std::wstring, size_t> destOwner;    // destKey -> file index
		std::unordered_set<std::wstring> desired;              // destKeys that must stay on the destination
		std::unordered_set<std::wstring> presentAfterSync;     // destKeys that will exist when we're done (for playlists)
		std::vector<PlanItem> copies;
		size_t validTracks = 0;

		for (size_t i = 0; i < files.size(); ++i) {
			if (checkAbort()) break;
			SourceFile& f = files[i];
			progress.setProgress(double(i) / files.size());
			progress.setFileProgress(0);

			if (!f.error.empty()) {
				add(Action::Error, f.destRel, f.sourcePath, 0, f.error);
				continue;
			}
			f.destRel = sanitizeRel(f.destRel);
			// The same source going to the same place again (other playlist, cue subsong...) is copied once.
			// One source may feed several destinations, e.g. a shared cover image for two album folders.
			if (!seenSources.insert(pathKey(f.sourcePath) + L"|" + pathKey(f.destRel)).second) continue;

			if (f.destRel.empty()) { add(Action::Error, L"", f.sourcePath, 0, L"File name pattern produced an empty path"); continue; }
			progress.setItem(f.destRel);

			std::wstring destKey = pathKey(f.destRel);
			auto owner = destOwner.find(destKey);
			if (owner != destOwner.end()) {
				add(Action::Error, f.destRel, f.sourcePath, 0,
					L"Name collision: this destination path is already used by " + files[owner->second].sourcePath);
				continue;
			}
			destOwner[destKey] = i;
			desired.insert(destKey);
			++validTracks;

			const auto oldIt = oldManifest.find(destKey);
			const ManifestEntry* old = oldIt != oldManifest.end() ? &oldIt->second : nullptr;

			SrcStat src = statSource(f.sourcePath);
			const DestStat dst = rootMissing ? DestStat{} : fs.stat(f.destRel);
			if (!src.exists || src.isDir) {
				add(Action::Error, f.destRel, f.sourcePath, 0, L"Source file not found");
				// Keep whatever is on the destination; a temporarily missing source must not delete it.
				if (old) newManifest[destKey] = *old;
				if (dst.exists) presentAfterSync.insert(destKey);
				continue;
			}
			if (dst.exists && dst.isDir) {
				add(Action::Error, f.destRel, f.sourcePath, src.size, L"A folder with this name exists on the destination");
				continue;
			}

			ManifestEntry fresh{ f.destRel, src.size, src.mtime, f.tagHash, f.techHash };

			if (!dst.exists) {
				copies.push_back(PlanItem{ i, false, L"New file", src, dst });
				presentAfterSync.insert(destKey);
				continue;
			}

			bool replace = false;
			std::wstring reason;
			ManifestEntry keep = old ? *old : fresh;
			keep.destRel = f.destRel;

			auto decideBySizeDate = [&](bool useManifest) {
				if (src.size != dst.size) {
					replace = true;
					reason = L"Size differs (source " + formatBytes(src.size) + L", destination " + formatBytes(dst.size) + L")";
				} else if (useManifest && old) {
					if (old->srcMtime != src.mtime || old->srcSize != src.size) { replace = true; reason = L"Source modified since last sync"; }
					else reason = L"Unchanged since last sync";
				} else if (!reliableDates) {
					reason = L"Same size (no earlier sync record; the device's dates can't be compared)";
				} else if (!datesMatch(src.mtime, dst.mtime)) {
					replace = true; reason = L"Modified date differs";
				} else {
					reason = L"Same size and date";
				}
			};

			switch (opt.policy) {
			case ExistingPolicy::Never:
				reason = L"Already exists (same path and name)";
				break;
			case ExistingPolicy::SizeOrDate:
				decideBySizeDate(true);
				break;
			case ExistingPolicy::TagsOnly: {
				uint64_t dTag = 0, dTech = 0;
				bool have = false;
				const wchar_t* via = L"";
				std::wstring local;
				if (old && (old->tagHash || old->techHash)) {
					dTag = old->tagHash; dTech = old->techHash; have = true;
				} else if (readDestTags && fs.localPath(f.destRel, local) && readDestTags(local, dTag, dTech)) {
					have = true; via = L" (read from destination)";
					keep.tagHash = dTag; keep.techHash = dTech;
				}
				if (!have) {
					decideBySizeDate(false);
					reason = L"Couldn't read destination tags; " + reason;
				} else if (dTag != f.tagHash) {
					replace = true; reason = std::wstring(L"Tags changed") + via;
				} else if (opt.compareTech && dTech != f.techHash) {
					replace = true; reason = std::wstring(L"Audio re-encoded (codec, bitrate or length changed)") + via;
				} else {
					reason = std::wstring(L"Tags identical") + via;
				}
				break;
			}
			}

			presentAfterSync.insert(destKey);
			if (replace) {
				copies.push_back(PlanItem{ i, true, reason, src, dst });
				if (old) newManifest[destKey] = *old; // replaced below on success
			} else {
				add(Action::Skipped, f.destRel, f.sourcePath, dst.size, reason);
				newManifest[destKey] = keep;
			}
		}

		// Files we write ourselves are never orphans.
		std::unordered_set<std::wstring> ownFiles{ pathKey(kManifestName), pathKey(std::wstring(kManifestName) + kTempSuffix) };
		if (opt.createNomedia) ownFiles.insert(pathKey(kNomediaName));
		if (opt.writePlaylists) {
			for (auto& pl : playlists) {
				ownFiles.insert(pathKey(playlistSubdir.empty() ? makePlaylistFileName(pl.name)
					: playlistSubdir + L"\\" + makePlaylistFileName(pl.name)));
			}
		}

		// ---- Phase 2: find orphans ----
		struct Removal { std::wstring rel; uint64_t size; };
		std::vector<Removal> removals;
		if (!rep.aborted && opt.removeOrphans && !rootMissing) {
			if (validTracks == 0) {
				rep.fatalError = L"None of the selected playlists' files could be synced, so nothing was removed (safety check).";
				return finish();
			}
			progress.setPhase(L"Looking for files to remove");
			if (opt.removeOnlyManaged) {
				for (auto& kv : oldManifest) {
					if (desired.count(kv.first) || ownFiles.count(kv.first)) continue;
					DestStat st = fs.stat(kv.second.destRel);
					if (st.exists && !st.isDir) removals.push_back(Removal{ kv.second.destRel, st.size });
				}
			} else {
				// Only inside the scope folder, and never hidden (dot) files or folders: on phones those belong
				// to the system or other apps (.thumbnails, .nomedia, ...).
				const std::wstring scope = sanitizeRel(opt.removalScope);
				const std::wstring scopeKey = scope.empty() ? std::wstring() : pathKey(scope) + L"\\";
				fs.listFiles([&](const std::wstring& rel, uint64_t size) {
					const std::wstring key = pathKey(rel);
					if (!scopeKey.empty() && key.rfind(scopeKey, 0) != 0) return !checkAbort();
					const bool hidden = rel[0] == L'.' || rel.find(L"\\.") != std::wstring::npos;
					if (!hidden && !desired.count(key) && !ownFiles.count(key)) removals.push_back(Removal{ rel, size });
					return !checkAbort();
				});
			}
			std::sort(removals.begin(), removals.end(), [](auto& a, auto& b) { return a.rel < b.rel; });
		}

		// ---- Free space check ----
		if (!rep.aborted && !rootMissing) {
			int64_t needed = 0;
			for (auto& c : copies) needed += (int64_t)c.src.size - (c.replace ? (int64_t)c.dst.size : 0);
			for (auto& r : removals) needed -= (int64_t)r.size;
			uint64_t freeBytes = 0;
			if (needed > 0 && fs.freeSpace(freeBytes) && (uint64_t)needed > freeBytes) {
				std::wstring msg = L"Not enough free space on the destination: " + formatBytes((uint64_t)needed) +
					L" needed, " + formatBytes(freeBytes) + L" available.";
				if (!opt.dryRun) { rep.fatalError = msg + L" Nothing was changed."; return finish(); }
				add(Action::Error, L"", L"", 0, msg);
			}
		}

		// ---- Phase 3: remove orphans (first, to make room) ----
		std::set<std::wstring> touchedDirs;
		if (!removals.empty()) progress.setPhase(opt.dryRun ? L"Checking removals" : L"Removing files");
		for (size_t i = 0; i < removals.size(); ++i) {
			if (checkAbort()) break;
			auto& r = removals[i];
			progress.setItem(r.rel);
			progress.setProgress(double(i) / removals.size());
			if (opt.dryRun) {
				add(Action::Removed, r.rel, L"", r.size, L"Not in the selected playlists");
				continue;
			}
			std::wstring err;
			if (fs.remove(r.rel, err)) {
				add(Action::Removed, r.rel, L"", r.size, L"Not in the selected playlists");
				rep.bytesRemoved += r.size;
				touchedDirs.insert(parentRel(r.rel));
			} else {
				add(Action::Error, r.rel, L"", r.size, err);
				auto it = oldManifest.find(pathKey(r.rel));
				if (it != oldManifest.end()) newManifest[it->first] = it->second;
			}
		}
		// Remove folders left empty, deepest first, never the root itself.
		if (!opt.dryRun) {
			std::vector<std::wstring> dirs(touchedDirs.begin(), touchedDirs.end());
			std::sort(dirs.begin(), dirs.end(), [](auto& a, auto& b) { return a.size() > b.size(); });
			for (auto d : dirs) {
				while (!d.empty() && fs.removeDirIfEmpty(d)) d = parentRel(d);
			}
		}

		// ---- Phase 4: copy ----
		uint64_t totalCopyBytes = 0, doneCopyBytes = 0;
		for (auto& c : copies) totalCopyBytes += c.src.size;
		progress.setPhase(opt.dryRun ? L"Preparing preview" : L"Copying files");
		FileProgressAdapter fileProgress(progress);
		for (size_t i = 0; i < copies.size(); ++i) {
			if (checkAbort()) break;
			auto& c = copies[i];
			const SourceFile& f = files[c.fileIdx];
			const std::wstring destKey = pathKey(f.destRel);
			const Action act = c.replace ? Action::Replaced : Action::Added;
			if (opt.dryRun) {
				add(act, f.destRel, f.sourcePath, c.src.size, c.reason);
				continue;
			}
			progress.setItem(f.destRel);
			progress.setProgress(totalCopyBytes ? double(doneCopyBytes) / totalCopyBytes : double(i) / copies.size());
			progress.setFileProgress(0);

			auto fail = [&](const std::wstring& msg) {
				add(Action::Error, f.destRel, f.sourcePath, c.src.size, msg);
				if (!c.replace) presentAfterSync.erase(destKey); // a failed replace leaves the old copy in place
			};
			std::wstring err;
			if (!fs.ensureDir(parentRel(f.destRel), err)) { fail(err); continue; }

			CopyTimes times;
			if (opt.preserveTimestamps) { times.set = true; times.ctime = c.src.ctime; times.mtime = c.src.mtime; }
			bool aborted = false;
			if (!fs.copyIn(f.sourcePath, c.src.size, f.destRel, c.replace, times, fileProgress, aborted, err)) {
				if (aborted) { rep.aborted = true; break; }
				fail(err);
				continue;
			}
			add(act, f.destRel, f.sourcePath, c.src.size, c.reason);
			rep.bytesCopied += c.src.size;
			doneCopyBytes += c.src.size;
			newManifest[destKey] = ManifestEntry{ f.destRel, c.src.size, c.src.mtime, f.tagHash, f.techHash };
		}
		if (rep.aborted) {
			// Copies that never ran: files that existed before stay listed, new ones simply aren't there.
			for (auto& c : copies) {
				const SourceFile& f = files[c.fileIdx];
				if (!c.replace && !newManifest.count(pathKey(f.destRel))) presentAfterSync.erase(pathKey(f.destRel));
			}
		}

		// ---- Phase 5: playlists and .nomedia ----
		if (!rep.aborted && opt.writePlaylists && !playlists.empty()) {
			progress.setPhase(L"Writing playlists");
			std::wstring dirErr;
			const bool dirOk = opt.dryRun || fs.ensureDir(playlistSubdir, dirErr);
			for (auto& pl : playlists) {
				const std::wstring fileName = makePlaylistFileName(pl.name);
				const std::wstring rel = playlistSubdir.empty() ? fileName : playlistSubdir + L"\\" + fileName;
				if (!dirOk) { add(Action::Error, rel, L"", 0, L"Couldn't create playlist folder: " + dirErr); continue; }
				const wchar_t* nl = opt.playlistUnixStyle ? L"\n" : L"\r\n";
				std::wstring text = std::wstring(L"#EXTM3U") + nl;
				size_t n = 0;
				for (auto& t : pl.tracks) {
					std::wstring tr = sanitizeRel(t.destRel);
					if (tr.empty() || !presentAfterSync.count(pathKey(tr))) continue;
					text += L"#EXTINF:" + std::to_wstring(t.durationSec) + L"," + t.displayTitle + nl;
					text += playlistRelPath(playlistSubdir, tr, opt.playlistUnixStyle) + nl;
					++n;
				}
				const std::string data = toUtf8(text);
				std::string existing;
				const bool same = !rootMissing && fs.readFile(rel, existing) && existing == data;
				std::wstring info = std::to_wstring(n) + (n == 1 ? L" track" : L" tracks");
				if (same) { add(Action::Playlist, rel, pl.name, data.size(), L"Unchanged, " + info); continue; }
				if (opt.dryRun) { add(Action::Playlist, rel, pl.name, data.size(), L"Would write, " + info); continue; }
				std::wstring err;
				if (fs.writeFile(rel, data, false, err)) add(Action::Playlist, rel, pl.name, data.size(), L"Written, " + info);
				else add(Action::Error, rel, pl.name, 0, L"Couldn't write playlist: " + err);
			}
		}
		if (!rep.aborted && opt.createNomedia && (rootMissing || !fs.stat(kNomediaName).exists)) {
			const wchar_t* why = L"Hides this folder from media scanners";
			std::wstring err;
			if (opt.dryRun) add(Action::Added, kNomediaName, L"", 0, why);
			else if (fs.writeFile(kNomediaName, std::string(), false, err)) add(Action::Added, kNomediaName, L"", 0, why);
			else add(Action::Error, kNomediaName, L"", 0, L"Couldn't create .nomedia: " + err);
		}

		// ---- Phase 6: manifest ----
		if (!opt.dryRun) {
			// Keep records of files we left in place (removal off, or failed) so a later run can still manage them.
			std::unordered_set<std::wstring> removedKeys;
			for (auto& e : rep.entries) if (e.action == Action::Removed) removedKeys.insert(pathKey(e.destRel));
			for (auto& kv : oldManifest) {
				if (newManifest.count(kv.first) || removedKeys.count(kv.first) || desired.count(kv.first)) continue;
				if (fs.stat(kv.second.destRel).exists) newManifest[kv.first] = kv.second;
			}
			std::wstring err;
			if (!saveManifest(fs, newManifest, err)) {
				add(Action::Error, kManifestName, L"", 0, L"Couldn't save sync state file: " + err);
			}
		}

		progress.setProgress(1.0);
		return finish();
	}
}
