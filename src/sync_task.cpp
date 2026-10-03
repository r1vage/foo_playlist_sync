#include "stdafx.h"
#include "sync_task.h"
#include "config.h"
#include "report_dialog.h"
#include "report_format.h"

#include <helpers/create_directory_helper.h>

#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace psync {

	namespace {

		std::atomic<bool> g_running{ false };

		std::wstring wide(const char* s) { return std::wstring(pfc::stringcvt::string_wide_from_utf8(s).get_ptr()); }
		pfc::string8 utf8(const std::wstring& s) { return pfc::string8(pfc::stringcvt::string_utf8_from_wide(s.c_str()).get_ptr()); }

		std::vector<std::wstring> parseIgnoreList(const char* text) {
			std::vector<std::wstring> out;
			std::wstring all = wide(text);
			size_t s = 0;
			while (s <= all.size()) {
				size_t t = all.find_first_of(L";,", s);
				if (t == std::wstring::npos) t = all.size();
				std::wstring item = all.substr(s, t - s);
				while (!item.empty() && iswspace(item.back())) item.pop_back();
				while (!item.empty() && iswspace(item.front())) item.erase(item.begin());
				if (!item.empty()) out.push_back(item);
				s = t + 1;
			}
			return out;
		}

		// ReplayGain is kept outside the meta fields by foobar2000, so it never takes part in this hash.
		uint64_t hashTags(const file_info& info, const std::vector<std::wstring>& ignore) {
			std::vector<std::pair<std::string, std::string>> fields;
			for (t_size i = 0; i < info.meta_get_count(); ++i) {
				const char* name = info.meta_enum_name(i);
				std::wstring wname = wide(name);
				bool skip = false;
				for (auto& pat : ignore) if (wildcardMatchNoCase(pat.c_str(), wname.c_str())) { skip = true; break; }
				if (skip) continue;
				std::string key = name;
				for (auto& c : key) c = (char)toupper((unsigned char)c);
				std::string val;
				for (t_size v = 0; v < info.meta_enum_value_count(i); ++v) {
					if (v) val += '\x1f';
					val += info.meta_enum_value(i, v);
				}
				fields.emplace_back(std::move(key), std::move(val));
			}
			std::sort(fields.begin(), fields.end());
			uint64_t h = fnv1a("", 0);
			for (auto& f : fields) {
				h = fnv1a(f.first.data(), f.first.size(), h);
				h = fnv1a("\x1e", 1, h);
				h = fnv1a(f.second.data(), f.second.size(), h);
				h = fnv1a("\x1d", 1, h);
			}
			return h;
		}

		uint64_t hashTech(const file_info& info) {
			std::string s;
			for (const char* field : { "codec", "codec_profile", "bitrate", "samplerate", "channels", "bitspersample", "encoding" }) {
				const char* v = info.info_get(field);
				s += field; s += '='; s += v ? v : ""; s += '\x1d';
			}
			s += "samples=" + std::to_string(info.info_get_length_samples());
			return fnv1a(s.data(), s.size());
		}

		class StatusProgress : public Progress {
		public:
			StatusProgress(threaded_process_status& st, abort_callback& ab, bool dryRun) : m_status(st), m_abort(ab), m_dryRun(dryRun) {}
			bool abortRequested() override { return m_abort.is_aborting(); }
			void setPhase(const wchar_t* phase) override {
				pfc::string8 t = m_dryRun ? "Playlist Sync preview: " : "Playlist Sync: ";
				t << utf8(phase);
				m_status.set_title(t);
			}
			void setProgress(double f) override { m_status.set_progress_float(f); }
			void setFileProgress(double f) override { m_status.set_progress_secondary_float(f); }
			void setItem(const std::wstring& item) override { m_status.set_item(utf8(item)); }
		private:
			threaded_process_status& m_status;
			abort_callback& m_abort;
			bool m_dryRun;
		};

		void saveLog(const Report& r) {
			try {
				pfc::string8 dir = filesystem::g_get_native_path(core_api::pathInProfile("playlist_sync_logs"));
				std::wstring wdir = wide(dir);
				CreateDirectoryW(wdir.c_str(), nullptr);
				std::wstring name = formatTimestamp(r.startedAt, true) + (r.dryRun ? L"_preview.txt" : L".txt");
				pfc::string8 text = utf8(reportAsText(r));
				HANDLE h = CreateFileW((wdir + L"\\" + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (h == INVALID_HANDLE_VALUE) return;
				DWORD w = 0;
				WriteFile(h, "\xEF\xBB\xBF", 3, &w, nullptr);
				WriteFile(h, text.get_ptr(), (DWORD)text.length(), &w, nullptr);
				CloseHandle(h);

				// Keep the newest 100 logs.
				std::vector<std::wstring> logs;
				WIN32_FIND_DATAW fd;
				HANDLE f = FindFirstFileW((wdir + L"\\*.txt").c_str(), &fd);
				if (f != INVALID_HANDLE_VALUE) {
					do logs.push_back(fd.cFileName); while (FindNextFileW(f, &fd));
					FindClose(f);
				}
				std::sort(logs.begin(), logs.end());
				for (size_t i = 0; i + 100 < logs.size(); ++i) DeleteFileW((wdir + L"\\" + logs[i]).c_str());
			} catch (...) {}
		}

		struct ArtSettings {
			bool enabled = false;
			std::vector<std::wstring> names; // file names to look for, in priority order
			std::wstring destName;           // without extension
			bool embedded = true;
		};

		struct ArtJob {
			std::wstring destDir;   // album folder on the destination (relative)
			std::wstring audioPath; // first track of that album folder
		};

		std::wstring lower(std::wstring s) {
			if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
			return s;
		}

		const wchar_t* imageExtFromBytes(const std::string& b) {
			auto u = reinterpret_cast<const unsigned char*>(b.data());
			if (b.size() >= 8 && u[0] == 0x89 && u[1] == 'P' && u[2] == 'N' && u[3] == 'G') return L"png";
			if (b.size() >= 6 && memcmp(u, "GIF8", 4) == 0) return L"gif";
			if (b.size() >= 12 && memcmp(u, "RIFF", 4) == 0 && memcmp(u + 8, "WEBP", 4) == 0) return L"webp";
			return L"jpg";
		}

		bool readBytes(const std::wstring& path, std::string& out) {
			HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
			if (h == INVALID_HANDLE_VALUE) return false;
			LARGE_INTEGER sz;
			bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart < (100ll << 20);
			if (ok) {
				out.resize((size_t)sz.QuadPart);
				DWORD got = 0;
				ok = out.empty() || (ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size());
			}
			CloseHandle(h);
			return ok;
		}

		// Finds the cover image for an album folder: an image file next to the tracks, else the embedded
		// front cover (written to a temp file so it can be copied like any other file).
		class ArtResolver {
		public:
			explicit ArtResolver(const ArtSettings& s) : m_s(s) {}
			~ArtResolver() {
				for (auto& f : m_tempFiles) DeleteFileW(f.c_str());
				if (!m_tempDir.empty()) RemoveDirectoryW(m_tempDir.c_str());
			}

			bool resolve(const ArtJob& job, abort_callback& abort, SourceFile& out) {
				const std::wstring dir = job.audioPath.substr(0, job.audioPath.find_last_of(L'\\'));
				std::wstring src = findInFolder(dir);
				std::string bytes;
				std::wstring ext;
				if (!src.empty() && readBytes(src, bytes)) {
					ext = lower(src.substr(src.find_last_of(L'.') + 1));
					if (ext == L"jpeg") ext = L"jpg";
				} else if (m_s.embedded) {
					try {
						pfc::string8 path = "file://";
						path << utf8(job.audioPath);
						auto art = album_art_extractor::g_open(nullptr, path, abort)->query(album_art_ids::cover_front, abort);
						if (!art.is_valid() || art->get_size() == 0) return false;
						bytes.assign(static_cast<const char*>(art->get_ptr()), art->get_size());
					} catch (exception_aborted const&) {
						throw;
					} catch (std::exception const&) {
						return false; // no embedded cover or unsupported format
					}
					ext = imageExtFromBytes(bytes);
					src = writeTemp(bytes, ext, job.audioPath);
					if (src.empty()) return false;
				} else {
					return false;
				}
				out.sourcePath = src;
				out.destRel = job.destDir + L"\\" + m_s.destName + L"." + ext;
				out.tagHash = out.techHash = fnv1a(bytes.data(), bytes.size()); // "tags only" mode compares image content
				return true;
			}

		private:
			std::wstring findInFolder(const std::wstring& dir) {
				const std::wstring key = lower(dir);
				auto cached = m_folderCache.find(key);
				if (cached != m_folderCache.end()) return cached->second;
				std::unordered_map<std::wstring, std::wstring> files; // lower-case name -> actual name
				WIN32_FIND_DATAW fd;
				HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
				if (h != INVALID_HANDLE_VALUE) {
					do if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files[lower(fd.cFileName)] = fd.cFileName;
					while (FindNextFileW(h, &fd));
					FindClose(h);
				}
				std::wstring found;
				for (auto& name : m_s.names) {
					const std::wstring n = lower(name);
					const bool hasExt = n.find(L'.') != std::wstring::npos;
					for (const wchar_t* e : { L"", L".jpg", L".jpeg", L".png" }) {
						if (hasExt != (*e == 0)) continue;
						auto it = files.find(n + e);
						if (it != files.end()) { found = dir + L"\\" + it->second; break; }
					}
					if (!found.empty()) break;
				}
				m_folderCache[key] = found;
				return found;
			}

			std::wstring writeTemp(const std::string& bytes, const std::wstring& ext, const std::wstring& audioPath) {
				if (m_tempDir.empty()) {
					wchar_t tmp[MAX_PATH];
					GetTempPathW(MAX_PATH, tmp);
					m_tempDir = std::wstring(tmp) + L"foo_playlist_sync_art_" + std::to_wstring(GetCurrentProcessId());
					CreateDirectoryW(m_tempDir.c_str(), nullptr);
				}
				wchar_t name[32];
				swprintf_s(name, L"%016llx.", (unsigned long long)fnv1a(bytes.data(), bytes.size()));
				const std::wstring path = m_tempDir + L"\\" + name + ext;
				if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
				HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
				if (h == INVALID_HANDLE_VALUE) return L"";
				DWORD w = 0;
				const bool ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &w, nullptr) && w == bytes.size();
				// Give the temp file the track's date so "size or date" comparisons stay stable between runs.
				WIN32_FILE_ATTRIBUTE_DATA d;
				if (ok && GetFileAttributesExW(audioPath.c_str(), GetFileExInfoStandard, &d)) SetFileTime(h, &d.ftLastWriteTime, nullptr, &d.ftLastWriteTime);
				CloseHandle(h);
				if (!ok) { DeleteFileW(path.c_str()); return L""; }
				m_tempFiles.push_back(path);
				return path;
			}

			const ArtSettings& m_s;
			std::unordered_map<std::wstring, std::wstring> m_folderCache;
			std::wstring m_tempDir;
			std::vector<std::wstring> m_tempFiles;
		};

		class SyncProcess : public threaded_process_callback {
		public:
			SyncProcess(Options opt, std::vector<SourceFile> files, std::vector<PlaylistOut> playlists, std::vector<std::wstring> ignore,
				std::vector<std::wstring> warnings, ArtSettings art, std::vector<ArtJob> artJobs)
				: m_opt(std::move(opt)), m_files(std::move(files)), m_playlists(std::move(playlists)), m_ignore(std::move(ignore)),
				m_warnings(std::move(warnings)), m_art(std::move(art)), m_artJobs(std::move(artJobs)) {}

			void run(threaded_process_status& status, abort_callback& abort) override {
				// MTP destinations use COM (Windows Portable Devices) on this worker thread.
				const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
				run_(status, abort);
				if (SUCCEEDED(coInit)) CoUninitialize();
			}

			void run_(threaded_process_status& status, abort_callback& abort) {
				StatusProgress progress(status, abort, m_opt.dryRun);
				auto reader = [this, &abort](const std::wstring& nativePath, uint64_t& tag, uint64_t& tech) -> bool {
					try {
						pfc::string8 path = "file://";
						path << utf8(nativePath);
						input_info_reader::ptr r;
						input_entry::g_open_for_info_read(r, nullptr, path, abort);
						file_info_impl info;
						r->get_info(0, info, abort);
						tag = hashTags(info, m_ignore);
						tech = hashTech(info);
						return true;
					} catch (exception_aborted const&) {
						return false;
					} catch (std::exception const&) {
						return false;
					}
				};
				try {
					ArtResolver art(m_art); // owns temp files of embedded covers until the sync is done
					if (m_art.enabled && !m_artJobs.empty()) {
						progress.setPhase(L"Finding album art");
						for (size_t i = 0; i < m_artJobs.size() && !abort.is_aborting(); ++i) {
							progress.setProgress(double(i) / m_artJobs.size());
							progress.setItem(m_artJobs[i].destDir);
							SourceFile sf;
							if (art.resolve(m_artJobs[i], abort, sf)) m_files.push_back(std::move(sf));
						}
					}
					m_report = runSync(m_opt, std::move(m_files), m_playlists, progress, reader);
				} catch (exception_aborted const&) {
					m_report.aborted = true;
				} catch (std::exception const& e) {
					m_report.fatalError = L"Unexpected error: " + wide(e.what());
				}
				for (auto& w : m_warnings) m_report.entries.insert(m_report.entries.begin(), ReportEntry{ Action::Error, L"", L"", 0, w });
			}

			void on_done(ctx_t, bool) override {
				g_running = false;
				if (cfg::saveLogs) saveLog(m_report);
				showReportDialog(std::move(m_report));
			}

		private:
			Options m_opt;
			std::vector<SourceFile> m_files;
			std::vector<PlaylistOut> m_playlists;
			std::vector<std::wstring> m_ignore;
			std::vector<std::wstring> m_warnings;
			ArtSettings m_art;
			std::vector<ArtJob> m_artJobs;
			Report m_report;
		};
	}

	bool isSyncRunning() { return g_running; }

	// Fixed folders at the start of the pattern, e.g. "Music2" for "Music2\%artist%\...". Removal of unknown
	// files is limited to that folder so other content of the destination is never touched.
	static std::wstring literalFolderPrefix(const char* pattern) {
		std::string literal;
		for (const char* p = pattern; *p && !strchr("%$['", *p); ++p) literal += *p;
		const size_t pos = literal.find_last_of("\\/");
		return pos == std::string::npos ? std::wstring() : wide(literal.substr(0, pos).c_str());
	}

	// Applies the file name pattern. The pattern may include the extension itself (e.g. ".$ext(%_path%)");
	// otherwise the source file's extension is appended.
	static void formatDestPath(const metadb_handle_ptr& h, const titleformat_object::ptr& script, const char* ext, pfc::string8& out) {
		create_directory_helper::format_filename_ex(h, nullptr, script, "", out);
		pfc::string8 suffix = ".";
		suffix << ext;
		const bool hasExt = out.length() > suffix.length() &&
			pfc::stringEqualsI_utf8(out.get_ptr() + out.length() - suffix.length(), suffix);
		if (!hasExt && *ext) create_directory_helper::format_filename_ex(h, nullptr, script, suffix, out);
	}

	pfc::string8 previewPattern(const char* pattern) {
		metadb_handle_ptr sample;
		auto pm = playlist_manager::get();
		if (!pm->activeplaylist_get_focus_item_handle(sample)) {
			auto pc = playback_control::get();
			if (!pc->get_now_playing(sample)) {
				metadb_handle_list items;
				pm->activeplaylist_get_all_items(items);
				if (items.get_count() == 0) return "";
				sample = items[0];
			}
		}
		titleformat_object::ptr script;
		if (!titleformat_compiler::get()->compile(script, pattern)) return "Invalid pattern";
		pfc::string8 native;
		filesystem::g_get_native_path(sample->get_path(), native);
		pfc::string8 out;
		formatDestPath(sample, script, pfc::string_extension(native), out);
		return out;
	}

	void startSync(bool dryRun) {
		if (g_running) {
			popup_message::g_show("A sync is already running.", "Playlist Sync");
			return;
		}

		Options opt;
		opt.destRoot = wide(cfg::destination.get());
		opt.policy = (ExistingPolicy)std::clamp<int64_t>(cfg::policy.get(), 0, 2);
		opt.compareTech = cfg::compareTech;
		opt.preserveTimestamps = cfg::preserveTimestamps;
		opt.removeOrphans = cfg::removeOrphans;
		opt.removeOnlyManaged = cfg::removeOnlyManaged;
		opt.writePlaylists = cfg::writePlaylists;
		opt.playlistSubdir = wide(cfg::playlistSubdir.get());
		opt.playlistUnixStyle = cfg::playlistUnixStyle;
		opt.createNomedia = cfg::createNomedia;
		opt.dryRun = dryRun;

		if (opt.destRoot.empty()) {
			popup_message::g_show("Choose a destination folder or device first (Preferences > Tools > Playlist Sync).", "Playlist Sync");
			cfg::showPreferences();
			return;
		}
		auto selected = cfg::getSelectedPlaylists();
		if (selected.empty()) {
			popup_message::g_show("Select at least one playlist to sync (Preferences > Tools > Playlist Sync).", "Playlist Sync");
			cfg::showPreferences();
			return;
		}

		titleformat_object::ptr script, titleScript;
		const pfc::string8 pattern = cfg::getPattern();
		opt.removalScope = literalFolderPrefix(pattern);
		if (!titleformat_compiler::get()->compile(script, pattern)) {
			popup_message::g_show("The file name pattern is not valid title formatting.", "Playlist Sync");
			return;
		}
		titleformat_compiler::get()->compile_safe(titleScript, "[%artist% - ]%title%");

		std::vector<std::wstring> ignore = parseIgnoreList(cfg::ignoreTags.get());
		std::vector<std::wstring> warnings;
		std::vector<SourceFile> files;

		ArtSettings art;
		art.enabled = cfg::copyArt;
		art.names = parseIgnoreList(cfg::artNames.get()); // same "a; b; c" list syntax
		art.destName = wide(cfg::artDestName.get());
		if (art.destName.empty()) art.destName = wide(cfg::defaultArtDestName);
		art.embedded = cfg::artEmbedded;
		std::vector<ArtJob> artJobs;
		std::unordered_set<std::wstring> artDirs;
		std::vector<PlaylistOut> playlists;

		// One entry per distinct track; the first track of a physical file decides its destination name.
		std::map<metadb_handle*, std::wstring> destByHandle;
		std::unordered_map<std::wstring, std::wstring> destBySource;

		auto pm = playlist_manager_v5::get();
		for (auto& sp : selected) {
			size_t idx = pm->find_playlist_by_guid(sp.guid);
			if (idx == SIZE_MAX) idx = pm->find_playlist(sp.name);
			if (idx == SIZE_MAX) {
				warnings.push_back(L"Playlist not found: " + wide(sp.name));
				continue;
			}
			pfc::string8 name;
			pm->playlist_get_name(idx, name);
			metadb_handle_list items;
			pm->playlist_get_all_items(idx, items);

			PlaylistOut plOut;
			plOut.name = wide(name);
			for (t_size i = 0; i < items.get_count(); ++i) {
				const metadb_handle_ptr& h = items[i];
				std::wstring destRel;
				auto known = destByHandle.find(h.get_ptr());
				if (known != destByHandle.end()) {
					destRel = known->second;
				} else {
					SourceFile sf;
					pfc::string8 native;
					const bool local = filesystem::g_get_native_path(h->get_path(), native) && !strstr(native, "://");
					sf.sourcePath = wide(native);
					const pfc::string8 ext = pfc::string_extension(native);
					if (!local) {
						sf.error = L"Not a local file (streams and files inside archives can't be synced)";
					} else if (pfc::stringEqualsI_ascii(ext, "cue")) {
						sf.error = L"Tracks from external CUE sheets aren't supported yet";
					} else {
						std::wstring srcKey = pathKey(sf.sourcePath);
						auto same = destBySource.find(srcKey);
						if (same != destBySource.end()) {
							destRel = same->second; // another subsong of a file we already have
						} else {
							pfc::string8 rel;
							formatDestPath(h, script, ext, rel);
							sf.destRel = wide(rel);
							metadb_info_container::ptr info;
							if (h->get_info_ref(info)) {
								sf.tagHash = hashTags(info->info(), ignore);
								sf.techHash = hashTech(info->info());
							}
							destRel = sf.destRel;
							destBySource[srcKey] = destRel;
						}
					}
					if (art.enabled && sf.error.empty() && !sf.destRel.empty()) {
						// One cover per album folder on the destination, taken from that folder's first track.
						std::wstring dir = sf.destRel;
						std::replace(dir.begin(), dir.end(), L'/', L'\\');
						dir = dir.substr(0, dir.find_last_of(L'\\') == std::wstring::npos ? 0 : dir.find_last_of(L'\\'));
						if (!dir.empty() && artDirs.insert(pathKey(dir)).second) artJobs.push_back(ArtJob{ dir, sf.sourcePath });
					}
					if (!sf.error.empty() || !sf.destRel.empty()) files.push_back(std::move(sf));
					destByHandle[h.get_ptr()] = destRel;
				}
				if (opt.writePlaylists && !destRel.empty()) {
					PlaylistTrack t;
					t.destRel = destRel;
					metadb_info_container::ptr info;
					t.durationSec = h->get_info_ref(info) ? (int)(info->info().get_length() + 0.5) : -1;
					pfc::string8 title;
					h->format_title(nullptr, title, titleScript, nullptr);
					t.displayTitle = wide(title);
					plOut.tracks.push_back(std::move(t));
				}
			}
			playlists.push_back(std::move(plOut));
		}

		if (files.empty() && warnings.size() == selected.size()) {
			popup_message::g_show("None of the selected playlists exist any more. Check Preferences > Tools > Playlist Sync.", "Playlist Sync");
			return;
		}

		g_running = true;
		try {
			auto cb = fb2k::service_new<SyncProcess>(std::move(opt), std::move(files), std::move(playlists), std::move(ignore), std::move(warnings),
				std::move(art), std::move(artJobs));
			threaded_process::get()->run_modeless(cb,
				threaded_process::flag_show_progress_dual | threaded_process::flag_show_item | threaded_process::flag_show_abort |
				threaded_process::flag_show_minimize,
				core_api::get_main_window(), dryRun ? "Playlist Sync preview" : "Playlist Sync");
		} catch (std::exception const& e) {
			g_running = false;
			popup_message::g_complain("Could not start Playlist Sync", e);
		}
	}
}
