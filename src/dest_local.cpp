#include "dest_fs.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>

namespace psync {

	namespace {

		constexpr const wchar_t* kTempSuffix = L".psync-tmp";

		int64_t toInt64(const FILETIME& ft) { return (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime); }
		FILETIME toFileTime(int64_t v) {
			FILETIME ft;
			ft.dwLowDateTime = (DWORD)((uint64_t)v & 0xFFFFFFFF);
			ft.dwHighDateTime = (DWORD)((uint64_t)v >> 32);
			return ft;
		}

		std::wstring longPath(const std::wstring& p) {
			if (p.rfind(L"\\\\?\\", 0) == 0) return p;
			if (p.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + p.substr(2);
			return L"\\\\?\\" + p;
		}

		bool getAttrs(const std::wstring& path, WIN32_FILE_ATTRIBUTE_DATA& d) {
			return GetFileAttributesExW(longPath(path).c_str(), GetFileExInfoStandard, &d) != 0;
		}

		void clearProtection(const std::wstring& path) {
			DWORD a = GetFileAttributesW(longPath(path).c_str());
			const DWORD mask = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
			if (a != INVALID_FILE_ATTRIBUTES && (a & mask)) SetFileAttributesW(longPath(path).c_str(), a & ~mask);
		}

		struct CopyCtx { CopyProgress* progress; bool aborted = false; };

		DWORD CALLBACK copyProgressRoutine(LARGE_INTEGER total, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER,
			DWORD, DWORD, HANDLE, HANDLE, LPVOID data) {
			auto ctx = static_cast<CopyCtx*>(data);
			if (ctx->progress->abortRequested()) { ctx->aborted = true; return PROGRESS_CANCEL; }
			if (total.QuadPart > 0) ctx->progress->setFileProgress(double(done.QuadPart) / double(total.QuadPart));
			return PROGRESS_CONTINUE;
		}

		class LocalFs : public DestFs {
		public:
			explicit LocalFs(std::wstring root) : m_input(std::move(root)) {}

			std::wstring describe() const override { return m_root.empty() ? m_input : m_root; }

			bool openRoot(bool create, bool& rootMissing, std::wstring& err) override {
				rootMissing = false;
				std::wstring r = m_input;
				std::replace(r.begin(), r.end(), L'/', L'\\');
				std::vector<wchar_t> full(32768);
				DWORD n = GetFullPathNameW(r.c_str(), (DWORD)full.size(), full.data(), nullptr);
				if (n == 0 || n >= full.size()) { err = L"Invalid destination folder: " + m_input; return false; }
				r = full.data();
				while (r.size() > 3 && r.back() == L'\\') r.pop_back();
				if (r.size() == 3 && r[1] == L':' && r[2] == L'\\') r.pop_back(); // "E:" - joins add the separator
				m_root = r;

				WIN32_FILE_ATTRIBUTE_DATA d;
				if (getAttrs(m_root + L"\\", d)) {
					if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { err = L"Destination is not a folder: " + m_root; return false; }
					return true;
				}
				if (!create) { rootMissing = true; return true; }
				// Create missing folders below the volume root.
				wchar_t vol[MAX_PATH + 1] = {};
				if (!GetVolumePathNameW(m_root.c_str(), vol, MAX_PATH) || !getAttrs(vol, d)) {
					err = L"Destination drive is not available: " + m_root;
					return false;
				}
				std::wstring v = vol;
				while (!v.empty() && v.back() == L'\\') v.pop_back();
				DWORD e = 0;
				if (!createBelow(v, m_root, e)) { err = L"Couldn't create destination folder (" + sysErrorText(e) + L"): " + m_root; return false; }
				return true;
			}

			DestStat stat(const std::wstring& rel) override {
				DestStat st;
				WIN32_FILE_ATTRIBUTE_DATA d;
				if (getAttrs(full(rel), d)) {
					st.exists = true;
					st.isDir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
					st.size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
					st.mtime = toInt64(d.ftLastWriteTime);
				}
				return st;
			}

			void listFiles(const std::function<bool(const std::wstring&, uint64_t)>& cb) override {
				std::vector<std::wstring> stack{ L"" };
				while (!stack.empty()) {
					std::wstring relDir = stack.back();
					stack.pop_back();
					WIN32_FIND_DATAW fd;
					HANDLE h = FindFirstFileExW(longPath(full(relDir) + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
					if (h == INVALID_HANDLE_VALUE) continue;
					do {
						std::wstring name = fd.cFileName;
						if (name == L"." || name == L"..") continue;
						if (fd.dwFileAttributes & (FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
						std::wstring rel = relDir.empty() ? name : relDir + L"\\" + name;
						if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
							if (_wcsicmp(name.c_str(), L"System Volume Information") == 0 || _wcsicmp(name.c_str(), L"$RECYCLE.BIN") == 0) continue;
							stack.push_back(rel);
						} else if (!cb(rel, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow)) {
							FindClose(h);
							return;
						}
					} while (FindNextFileW(h, &fd));
					FindClose(h);
				}
			}

			bool ensureDir(const std::wstring& relDir, std::wstring& err) override {
				DWORD e = 0;
				if (!createBelow(m_root, full(relDir), e)) { err = L"Couldn't create folder: " + sysErrorText(e); return false; }
				return true;
			}

			bool copyIn(const std::wstring& src, uint64_t, const std::wstring& rel, bool replacing, const CopyTimes& times,
				CopyProgress& progress, bool& aborted, std::wstring& err) override {
				aborted = false;
				const std::wstring dst = full(rel);
				const std::wstring tmp = dst + kTempSuffix;
				clearProtection(tmp);
				DeleteFileW(longPath(tmp).c_str());

				CopyCtx ctx{ &progress };
				BOOL cancel = FALSE;
				if (!CopyFileExW(longPath(src).c_str(), longPath(tmp).c_str(), copyProgressRoutine, &ctx, &cancel, 0)) {
					DWORD e = GetLastError();
					DeleteFileW(longPath(tmp).c_str());
					if (ctx.aborted || e == ERROR_REQUEST_ABORTED) { aborted = true; return false; }
					err = L"Copy failed: " + sysErrorText(e);
					return false;
				}
				if (replacing) clearProtection(dst);
				if (!MoveFileExW(longPath(tmp).c_str(), longPath(dst).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
					DWORD e = GetLastError();
					DeleteFileW(longPath(tmp).c_str());
					err = L"Couldn't replace the destination file: " + sysErrorText(e);
					return false;
				}
				// Set times after the rename: NTFS "tunneling" would otherwise give the new file the old creation time.
				HANDLE h = CreateFileW(longPath(dst).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
					nullptr, OPEN_EXISTING, 0, nullptr);
				if (h != INVALID_HANDLE_VALUE) {
					FILETIME ct, mt;
					if (times.set) { ct = toFileTime(times.ctime); mt = toFileTime(times.mtime); }
					else { GetSystemTimeAsFileTime(&ct); mt = ct; }
					SetFileTime(h, &ct, nullptr, &mt);
					CloseHandle(h);
				}
				DWORD a = GetFileAttributesW(longPath(dst).c_str());
				if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) SetFileAttributesW(longPath(dst).c_str(), a & ~FILE_ATTRIBUTE_READONLY);
				return true;
			}

			bool remove(const std::wstring& rel, std::wstring& err) override {
				const std::wstring p = full(rel);
				clearProtection(p);
				if (DeleteFileW(longPath(p).c_str())) return true;
				err = L"Couldn't remove: " + sysErrorText(GetLastError());
				return false;
			}

			bool removeDirIfEmpty(const std::wstring& relDir) override {
				if (relDir.empty()) return false;
				return RemoveDirectoryW(longPath(full(relDir)).c_str()) != 0;
			}

			bool readFile(const std::wstring& rel, std::string& out) override {
				HANDLE h = CreateFileW(longPath(full(rel)).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
				if (h == INVALID_HANDLE_VALUE) return false;
				LARGE_INTEGER sz;
				bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart < (64ll << 20);
				if (ok) {
					out.resize((size_t)sz.QuadPart);
					DWORD got = 0;
					ok = out.empty() || (ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size());
				}
				CloseHandle(h);
				return ok;
			}

			bool writeFile(const std::wstring& rel, const std::string& data, bool hidden, std::wstring& err) override {
				const std::wstring p = full(rel);
				const std::wstring tmp = p + kTempSuffix;
				HANDLE h = CreateFileW(longPath(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (h == INVALID_HANDLE_VALUE) { err = sysErrorText(GetLastError()); return false; }
				DWORD written = 0;
				bool ok = data.empty() || (WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size());
				DWORD e = ok ? 0 : GetLastError();
				if (ok) FlushFileBuffers(h);
				CloseHandle(h);
				if (!ok) { DeleteFileW(longPath(tmp).c_str()); err = sysErrorText(e); return false; }
				clearProtection(p);
				if (!MoveFileExW(longPath(tmp).c_str(), longPath(p).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
					e = GetLastError();
					DeleteFileW(longPath(tmp).c_str());
					err = sysErrorText(e);
					return false;
				}
				if (hidden) SetFileAttributesW(longPath(p).c_str(), FILE_ATTRIBUTE_HIDDEN);
				return true;
			}

			bool freeSpace(uint64_t& bytes) override {
				ULARGE_INTEGER f{};
				if (!GetDiskFreeSpaceExW((m_root + L"\\").c_str(), &f, nullptr, nullptr)) return false;
				bytes = f.QuadPart;
				return true;
			}

			bool localPath(const std::wstring& rel, std::wstring& out) override { out = full(rel); return true; }

		private:
			std::wstring full(const std::wstring& rel) const {
				if (rel.empty()) return m_root;
				return m_root + L"\\" + rel;
			}

			static bool createBelow(const std::wstring& base, const std::wstring& dir, DWORD& err) {
				if (dir.size() <= base.size()) return true;
				size_t pos = base.size();
				while (pos < dir.size()) {
					size_t next = dir.find(L'\\', pos + 1);
					if (next == std::wstring::npos) next = dir.size();
					if (!CreateDirectoryW(longPath(dir.substr(0, next)).c_str(), nullptr)) {
						DWORD e = GetLastError();
						if (e != ERROR_ALREADY_EXISTS) { err = e; return false; }
					}
					pos = next;
				}
				return true;
			}

			std::wstring m_input;
			std::wstring m_root;
		};
	}

	std::unique_ptr<DestFs> createLocalFs(const std::wstring& root) { return std::make_unique<LocalFs>(root); }

	bool isMtpPath(const std::wstring& dest) { return _wcsnicmp(dest.c_str(), kMtpPrefix, wcslen(kMtpPrefix)) == 0; }

	std::wstring parentRel(const std::wstring& rel) {
		auto pos = rel.find_last_of(L'\\');
		return pos == std::wstring::npos ? std::wstring() : rel.substr(0, pos);
	}

	std::wstring sysErrorText(unsigned long code) {
		wchar_t* buf = nullptr;
		DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr, code, 0, (LPWSTR)&buf, 0, nullptr);
		std::wstring s;
		if (n && buf) {
			s.assign(buf, n);
			LocalFree(buf);
			while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ' || s.back() == L'.')) s.pop_back();
		} else {
			wchar_t hex[32];
			swprintf_s(hex, L"Error 0x%08lX", code);
			s = hex;
		}
		return s;
	}
}
