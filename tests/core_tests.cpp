// Console tests for the sync engine (src/sync_core.cpp). Builds without the foobar2000 SDK.

#include "../src/sync_core.h"
#include "../src/report_format.h"
#include "../src/dest_fs.h"

#include <map>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace psync;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

class NullProgress : public Progress {
public:
	int abortAfterItems = -1;
	int items = 0;
	bool abortRequested() override { return abortAfterItems >= 0 && items > abortAfterItems; }
	void setPhase(const wchar_t*) override {}
	void setProgress(double) override {}
	void setFileProgress(double) override {}
	void setItem(const std::wstring&) override { ++items; }
};

static std::wstring g_base;

static void writeFile(const std::wstring& path, const std::string& content) {
	const std::wstring dir = path.substr(0, path.find_last_of(L'\\'));
	for (size_t p = dir.find(L'\\', 3); ; p = dir.find(L'\\', p + 1)) {
		CreateDirectoryW(dir.substr(0, p).c_str(), nullptr);
		if (p == std::wstring::npos) break;
	}
	HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	DWORD w = 0;
	WriteFile(h, content.data(), (DWORD)content.size(), &w, nullptr);
	CloseHandle(h);
}

static bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

static std::string readFile(const std::wstring& path) {
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return "<missing>";
	char buf[65536]; DWORD got = 0;
	ReadFile(h, buf, sizeof(buf), &got, nullptr);
	CloseHandle(h);
	return std::string(buf, got);
}

static void setMtime(const std::wstring& path, int64_t offsetSeconds) {
	HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	FILETIME ft; GetFileTime(h, nullptr, nullptr, &ft);
	ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
	u.QuadPart += offsetSeconds * 10000000LL;
	ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
	SetFileTime(h, nullptr, nullptr, &ft);
	CloseHandle(h);
}

static void getTimes(const std::wstring& path, FILETIME& c, FILETIME& m) {
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	GetFileTime(h, &c, nullptr, &m);
	CloseHandle(h);
}

static void removeTree(const std::wstring& dir) {
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			std::wstring n = fd.cFileName;
			if (n == L"." || n == L"..") continue;
			std::wstring p = dir + L"\\" + n;
			SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeTree(p); else DeleteFileW(p.c_str());
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(dir.c_str());
}

struct Fixture {
	std::wstring src, dst;
	explicit Fixture(const wchar_t* name) {
		src = g_base + L"\\" + name + L"\\src";
		dst = g_base + L"\\" + name + L"\\dst";
		removeTree(g_base + L"\\" + name);
		CreateDirectoryW((g_base + L"\\" + name).c_str(), nullptr);
		CreateDirectoryW(src.c_str(), nullptr);
	}
	SourceFile file(const wchar_t* srcRel, const wchar_t* destRel, uint64_t tag = 1, uint64_t tech = 1) {
		SourceFile f;
		f.sourcePath = src + L"\\" + srcRel;
		f.destRel = destRel;
		f.tagHash = tag;
		f.techHash = tech;
		return f;
	}
	Options opts() { Options o; o.destRoot = dst; return o; }
};

static Report run(const Options& o, const std::vector<SourceFile>& files, const std::vector<PlaylistOut>& pls = {},
	DestTagReader reader = nullptr) {
	NullProgress p;
	return runSync(o, files, pls, p, reader);
}

static void testFreshSyncAndRerun() {
	std::printf("fresh sync + rerun\n");
	Fixture fx(L"fresh");
	writeFile(fx.src + L"\\a.mp3", "aaaa");
	writeFile(fx.src + L"\\b.flac", "bbbbbb");
	setMtime(fx.src + L"\\a.mp3", -86400 * 30);
	std::vector<SourceFile> files{ fx.file(L"a.mp3", L"Artist\\Album\\a.mp3"), fx.file(L"b.flac", L"Artist\\Album\\b.flac") };

	Report r = run(fx.opts(), files);
	CHECK(r.fatalError.empty());
	CHECK(r.count(Action::Added) == 2);
	CHECK(readFile(fx.dst + L"\\Artist\\Album\\a.mp3") == "aaaa");
	CHECK(exists(fx.dst + L"\\.playlist_sync"));
	CHECK(r.bytesCopied == 10);
	CHECK(!exists(fx.dst + L"\\Artist\\Album\\a.mp3.psync-tmp"));

	FILETIME sc, sm, dc, dm;
	getTimes(fx.src + L"\\a.mp3", sc, sm);
	getTimes(fx.dst + L"\\Artist\\Album\\a.mp3", dc, dm);
	CHECK(CompareFileTime(&sm, &dm) == 0);
	CHECK(CompareFileTime(&sc, &dc) == 0);

	Report r2 = run(fx.opts(), files);
	CHECK(r2.count(Action::Skipped) == 2);
	CHECK(r2.count(Action::Added) == 0 && r2.count(Action::Replaced) == 0);

	// Source changes size -> replaced.
	writeFile(fx.src + L"\\a.mp3", "aaaaaaaa");
	Report r3 = run(fx.opts(), files);
	CHECK(r3.count(Action::Replaced) == 1);
	CHECK(readFile(fx.dst + L"\\Artist\\Album\\a.mp3") == "aaaaaaaa");

	// Same size, new date -> replaced (manifest knows the old source date).
	writeFile(fx.src + L"\\b.flac", "BBBBBB");
	setMtime(fx.src + L"\\b.flac", 3600 * 5);
	Report r4 = run(fx.opts(), files);
	CHECK(r4.count(Action::Replaced) == 1);
	CHECK(readFile(fx.dst + L"\\Artist\\Album\\b.flac") == "BBBBBB");
}

static void testNoPreserveTimestamps() {
	std::printf("timestamps not preserved\n");
	Fixture fx(L"nopreserve");
	writeFile(fx.src + L"\\a.mp3", "aaaa");
	setMtime(fx.src + L"\\a.mp3", -86400 * 365);
	Options o = fx.opts();
	o.preserveTimestamps = false;
	run(o, { fx.file(L"a.mp3", L"a.mp3") });
	FILETIME sc, sm, dc, dm;
	getTimes(fx.src + L"\\a.mp3", sc, sm);
	getTimes(fx.dst + L"\\a.mp3", dc, dm);
	CHECK(CompareFileTime(&sm, &dm) < 0);
	// Re-run must not re-copy just because dates differ: the manifest remembers the source date.
	Report r = run(o, { fx.file(L"a.mp3", L"a.mp3") });
	CHECK(r.count(Action::Skipped) == 1);
}

static void testTagsOnlyPolicy() {
	std::printf("tags-only policy\n");
	Fixture fx(L"tags");
	writeFile(fx.src + L"\\a.mp3", "aaaa");
	Options o = fx.opts();
	o.policy = ExistingPolicy::TagsOnly;
	run(o, { fx.file(L"a.mp3", L"a.mp3", 100, 7) });

	// Retagged file with an identical tag hash (e.g. only ReplayGain changed) and a new date -> skipped.
	writeFile(fx.src + L"\\a.mp3", "aaaaa");
	setMtime(fx.src + L"\\a.mp3", 7200);
	Report r1 = run(o, { fx.file(L"a.mp3", L"a.mp3", 100, 7) });
	CHECK(r1.count(Action::Skipped) == 1);
	CHECK(readFile(fx.dst + L"\\a.mp3") == "aaaa");

	// Tech differs, compareTech off -> skipped; on -> replaced.
	Report r2 = run(o, { fx.file(L"a.mp3", L"a.mp3", 100, 8) });
	CHECK(r2.count(Action::Skipped) == 1);
	o.compareTech = true;
	Report r3 = run(o, { fx.file(L"a.mp3", L"a.mp3", 100, 8) });
	CHECK(r3.count(Action::Replaced) == 1);
	CHECK(readFile(fx.dst + L"\\a.mp3") == "aaaaa");

	// Tags differ -> replaced.
	Report r4 = run(o, { fx.file(L"a.mp3", L"a.mp3", 101, 8) });
	CHECK(r4.count(Action::Replaced) == 1);
}

static void testTagsOnlyWithoutManifestUsesReader() {
	std::printf("tags-only without manifest reads destination\n");
	Fixture fx(L"tagsreader");
	writeFile(fx.src + L"\\a.mp3", "aaaa");
	writeFile(fx.dst + L"\\a.mp3", "different-bytes");
	writeFile(fx.src + L"\\b.mp3", "bbbb");
	writeFile(fx.dst + L"\\b.mp3", "old");
	Options o = fx.opts();
	o.policy = ExistingPolicy::TagsOnly;
	int calls = 0;
	DestTagReader reader = [&](const std::wstring& p, uint64_t& tag, uint64_t& tech) {
		++calls;
		tag = p.find(L"a.mp3") != std::wstring::npos ? 5 : 999;
		tech = 1;
		return true;
	};
	Report r = run(o, { fx.file(L"a.mp3", L"a.mp3", 5, 1), fx.file(L"b.mp3", L"b.mp3", 6, 1) }, {}, reader);
	CHECK(calls == 2);
	CHECK(r.count(Action::Skipped) == 1);
	CHECK(r.count(Action::Replaced) == 1);
	CHECK(readFile(fx.dst + L"\\a.mp3") == "different-bytes");
	CHECK(readFile(fx.dst + L"\\b.mp3") == "bbbb");

	// Second run uses the manifest, no reads.
	calls = 0;
	Report r2 = run(o, { fx.file(L"a.mp3", L"a.mp3", 5, 1), fx.file(L"b.mp3", L"b.mp3", 6, 1) }, {}, reader);
	CHECK(calls == 0);
	CHECK(r2.count(Action::Skipped) == 2);
}

static void testNeverPolicy() {
	std::printf("never-replace policy\n");
	Fixture fx(L"never");
	writeFile(fx.src + L"\\a.mp3", "new content");
	writeFile(fx.dst + L"\\a.mp3", "old");
	Options o = fx.opts();
	o.policy = ExistingPolicy::Never;
	Report r = run(o, { fx.file(L"a.mp3", L"a.mp3") });
	CHECK(r.count(Action::Skipped) == 1);
	CHECK(readFile(fx.dst + L"\\a.mp3") == "old");
}

static void testRemoveManagedOnly() {
	std::printf("remove orphans (managed only)\n");
	Fixture fx(L"rmmanaged");
	writeFile(fx.src + L"\\a.mp3", "a");
	writeFile(fx.src + L"\\b.mp3", "b");
	Options o = fx.opts();
	run(o, { fx.file(L"a.mp3", L"X\\a.mp3"), fx.file(L"b.mp3", L"Y\\Z\\b.mp3") });
	writeFile(fx.dst + L"\\foreign.txt", "keep me");

	o.removeOrphans = true;
	o.removeOnlyManaged = true;
	Report r = run(o, { fx.file(L"a.mp3", L"X\\a.mp3") });
	CHECK(r.count(Action::Removed) == 1);
	CHECK(!exists(fx.dst + L"\\Y\\Z\\b.mp3"));
	CHECK(!exists(fx.dst + L"\\Y"));                // empty folders pruned
	CHECK(exists(fx.dst + L"\\foreign.txt"));       // not ours, untouched
	CHECK(exists(fx.dst + L"\\X\\a.mp3"));
	CHECK(exists(fx.dst));
}

static void testRemoveAll() {
	std::printf("remove orphans (everything)\n");
	Fixture fx(L"rmall");
	writeFile(fx.src + L"\\a.mp3", "a");
	writeFile(fx.dst + L"\\foreign.txt", "bye");
	writeFile(fx.dst + L"\\Old\\Album\\x.mp3", "bye");
	Options o = fx.opts();
	o.removeOrphans = true;
	o.removeOnlyManaged = false;
	Report r = run(o, { fx.file(L"a.mp3", L"a.mp3") });
	CHECK(r.count(Action::Removed) == 2);
	CHECK(!exists(fx.dst + L"\\foreign.txt"));
	CHECK(!exists(fx.dst + L"\\Old"));
	CHECK(exists(fx.dst + L"\\a.mp3"));
	CHECK(exists(fx.dst + L"\\.playlist_sync"));

	// Re-run: manifest must not be treated as an orphan.
	Report r2 = run(o, { fx.file(L"a.mp3", L"a.mp3") });
	CHECK(r2.count(Action::Removed) == 0);
	CHECK(exists(fx.dst + L"\\.playlist_sync"));
}

static void testRemoveAllScopeAndHidden() {
	std::printf("remove everything: scope folder and hidden files\n");
	Fixture fx(L"rmscope");
	writeFile(fx.src + L"\\a.mp3", "a");
	writeFile(fx.dst + L"\\Telegram\\audio reel.mp3", "not ours");
	writeFile(fx.dst + L"\\.thumbnails\\.database_uuid", "android");
	writeFile(fx.dst + L"\\Music2\\Old\\gone.mp3", "stale");
	writeFile(fx.dst + L"\\Music2\\.hidden\\x", "hidden");
	Options o = fx.opts();
	o.removeOrphans = true;
	o.removeOnlyManaged = false;
	o.removalScope = L"Music2";
	Report r = run(o, { fx.file(L"a.mp3", L"Music2\\Art\\a.mp3") });
	CHECK(r.count(Action::Removed) == 1);
	CHECK(!exists(fx.dst + L"\\Music2\\Old\\gone.mp3"));
	CHECK(exists(fx.dst + L"\\Telegram\\audio reel.mp3"));
	CHECK(exists(fx.dst + L"\\.thumbnails\\.database_uuid"));
	CHECK(exists(fx.dst + L"\\Music2\\.hidden\\x"));

	// Without a scope, foreign visible files are removed but hidden ones still never are.
	o.removalScope.clear();
	Report r2 = run(o, { fx.file(L"a.mp3", L"Music2\\Art\\a.mp3") });
	CHECK(r2.count(Action::Removed) == 1);
	CHECK(!exists(fx.dst + L"\\Telegram\\audio reel.mp3"));
	CHECK(exists(fx.dst + L"\\.thumbnails\\.database_uuid"));
}

static void testSafetyEmptyList() {
	std::printf("safety: nothing to sync never wipes destination\n");
	Fixture fx(L"safety");
	writeFile(fx.dst + L"\\music.mp3", "precious");
	Options o = fx.opts();
	o.removeOrphans = true;
	o.removeOnlyManaged = false;
	Report r = run(o, {});
	CHECK(!r.fatalError.empty());
	CHECK(exists(fx.dst + L"\\music.mp3"));

	SourceFile bad; bad.sourcePath = L"http://example/stream"; bad.error = L"Not a local file";
	Report r2 = run(o, { bad });
	CHECK(!r2.fatalError.empty());
	CHECK(exists(fx.dst + L"\\music.mp3"));
}

static void testMissingSourceKeepsDest() {
	std::printf("missing source keeps destination copy\n");
	Fixture fx(L"missing");
	writeFile(fx.src + L"\\a.mp3", "a");
	writeFile(fx.src + L"\\b.mp3", "b");
	Options o = fx.opts();
	run(o, { fx.file(L"a.mp3", L"a.mp3"), fx.file(L"b.mp3", L"b.mp3") });
	DeleteFileW((fx.src + L"\\b.mp3").c_str());
	o.removeOrphans = true;
	Report r = run(o, { fx.file(L"a.mp3", L"a.mp3"), fx.file(L"b.mp3", L"b.mp3") });
	CHECK(r.count(Action::Error) == 1);
	CHECK(r.count(Action::Removed) == 0);
	CHECK(exists(fx.dst + L"\\b.mp3"));
}

static void testDryRunChangesNothing() {
	std::printf("preview makes no changes\n");
	Fixture fx(L"dry");
	writeFile(fx.src + L"\\a.mp3", "a");
	writeFile(fx.src + L"\\b.mp3", "bb");
	writeFile(fx.dst + L"\\b.mp3", "old");
	writeFile(fx.dst + L"\\orphan.mp3", "o");
	Options o = fx.opts();
	o.dryRun = true;
	o.removeOrphans = true;
	o.removeOnlyManaged = false;
	o.writePlaylists = true;
	PlaylistOut pl{ L"Mix", { { L"a.mp3", 10, L"A" } } };
	Report r = run(o, { fx.file(L"a.mp3", L"a.mp3"), fx.file(L"b.mp3", L"b.mp3") }, { pl });
	CHECK(r.dryRun);
	CHECK(r.count(Action::Added) == 1);
	CHECK(r.count(Action::Replaced) == 1);
	CHECK(r.count(Action::Removed) == 1);
	CHECK(r.count(Action::Playlist) == 1);
	CHECK(!exists(fx.dst + L"\\a.mp3"));
	CHECK(readFile(fx.dst + L"\\b.mp3") == "old");
	CHECK(exists(fx.dst + L"\\orphan.mp3"));
	CHECK(!exists(fx.dst + L"\\Mix.m3u8"));
	CHECK(!exists(fx.dst + L"\\.playlist_sync"));

	// Preview into a folder that doesn't exist yet.
	Options o2 = o;
	o2.destRoot = fx.dst + L"\\not\\there";
	Report r2 = run(o2, { fx.file(L"a.mp3", L"a.mp3") });
	CHECK(r2.fatalError.empty());
	CHECK(r2.count(Action::Added) == 1);
	CHECK(!exists(o2.destRoot));
}

static void testCollisionAndDuplicates() {
	std::printf("name collisions and duplicate tracks\n");
	Fixture fx(L"collide");
	writeFile(fx.src + L"\\one\\t.mp3", "1");
	writeFile(fx.src + L"\\two\\t.mp3", "2");
	Options o = fx.opts();
	Report r = run(o, { fx.file(L"one\\t.mp3", L"t.mp3"), fx.file(L"two\\t.mp3", L"T.MP3"), fx.file(L"one\\t.mp3", L"t.mp3") });
	CHECK(r.count(Action::Added) == 1);
	CHECK(r.count(Action::Error) == 1);
	CHECK(readFile(fx.dst + L"\\t.mp3") == "1");

	// One source feeding two destinations (a shared cover image for two album folders) is copied to both.
	writeFile(fx.src + L"\\folder.jpg", "jpeg");
	Report r2 = run(o, { fx.file(L"folder.jpg", L"Album A\\folder.jpg"), fx.file(L"folder.jpg", L"Album B\\folder.jpg") });
	CHECK(r2.count(Action::Added) == 2);
	CHECK(readFile(fx.dst + L"\\Album B\\folder.jpg") == "jpeg");
}

static void testPlaylists() {
	std::printf("playlist files\n");
	Fixture fx(L"playlists");
	writeFile(fx.src + L"\\a.mp3", "a");
	writeFile(fx.src + L"\\b.mp3", "b");
	Options o = fx.opts();
	o.writePlaylists = true;
	o.playlistSubdir = L"Playlists";
	o.playlistUnixStyle = true;
	PlaylistOut pl{ L"Road: trip?", { { L"Art\\a.mp3", 61, L"Art - A" }, { L"Art\\b.mp3", 5, L"Art - B" }, { L"Art\\missing.mp3", 1, L"x" } } };
	Report r = run(o, { fx.file(L"a.mp3", L"Art\\a.mp3"), fx.file(L"b.mp3", L"Art\\b.mp3") }, { pl });
	CHECK(r.count(Action::Playlist) == 1);
	std::string m3u = readFile(fx.dst + L"\\Playlists\\Road_ trip_.m3u8");
	CHECK(m3u == "#EXTM3U\n#EXTINF:61,Art - A\n../Art/a.mp3\n#EXTINF:5,Art - B\n../Art/b.mp3\n");

	// Rewriting the same content reports "Unchanged".
	Report r2 = run(o, { fx.file(L"a.mp3", L"Art\\a.mp3"), fx.file(L"b.mp3", L"Art\\b.mp3") }, { pl });
	CHECK(r2.entries.back().reason.find(L"Unchanged") == 0);

	// Windows style in root.
	o.playlistSubdir.clear();
	o.playlistUnixStyle = false;
	run(o, { fx.file(L"a.mp3", L"Art\\a.mp3") }, { PlaylistOut{ L"Win", { { L"Art\\a.mp3", 1, L"A" } } } });
	CHECK(readFile(fx.dst + L"\\Win.m3u8") == "#EXTM3U\r\n#EXTINF:1,A\r\nArt\\a.mp3\r\n");

	// Playlists written by us are not orphans in "remove everything" mode.
	o.removeOrphans = true;
	o.removeOnlyManaged = false;
	Report r3 = run(o, { fx.file(L"a.mp3", L"Art\\a.mp3") }, { PlaylistOut{ L"Win", { { L"Art\\a.mp3", 1, L"A" } } } });
	CHECK(exists(fx.dst + L"\\Win.m3u8"));
	CHECK(!exists(fx.dst + L"\\Art\\b.mp3"));
}

static void testAbort() {
	std::printf("abort mid-way keeps state consistent\n");
	Fixture fx(L"abort");
	std::vector<SourceFile> files;
	for (int i = 0; i < 6; ++i) {
		std::wstring n = L"f" + std::to_wstring(i) + L".mp3";
		writeFile(fx.src + L"\\" + n, "x");
		files.push_back(fx.file(n.c_str(), n.c_str()));
	}
	NullProgress p;
	p.abortAfterItems = 8; // 6 compare items + 2 copies
	Report r = runSync(fx.opts(), files, {}, p, nullptr);
	CHECK(r.aborted);
	CHECK(r.count(Action::Added) >= 1 && r.count(Action::Added) < 6);
	size_t added = r.count(Action::Added);
	Report r2 = run(fx.opts(), files);
	CHECK(r2.count(Action::Skipped) == added);
	CHECK(r2.count(Action::Added) == 6 - added);
}

static void testHelpers() {
	std::printf("helpers\n");
	CHECK(wildcardMatchNoCase(L"REPLAYGAIN_*", L"replaygain_track_gain"));
	CHECK(wildcardMatchNoCase(L"*played*", L"LAST_PLAYED"));
	CHECK(wildcardMatchNoCase(L"a?c", L"ABC"));
	CHECK(!wildcardMatchNoCase(L"REPLAYGAIN_*", L"ARTIST"));
	CHECK(!wildcardMatchNoCase(L"abc", L"abcd"));
	CHECK(makePlaylistFileName(L"CON") == L"_CON.m3u8");
	CHECK(makePlaylistFileName(L"a/b") == L"a_b.m3u8");
	CHECK(pathKey(L"a/B\\c") == pathKey(L"A\\b\\C"));
}


static void testReportFormat() {
	std::printf("report text and csv\n");
	Report r;
	r.destRoot = L"E:\\Music";
	r.entries.push_back(ReportEntry{ Action::Added, L"A\\b.mp3", L"C:\\src\\b.mp3", 2048, L"New file" });
	r.entries.push_back(ReportEntry{ Action::Error, L"x, \"y\".mp3", L"", 0, L"Copy failed" });
	r.entries.push_back(ReportEntry{ Action::Skipped, L"c.mp3", L"", 10, L"Tags identical" });
	std::wstring text = reportAsText(r);
	CHECK(text.find(L"[Added] 1") != std::wstring::npos);
	CHECK(text.find(L"[Errors] 1") != std::wstring::npos);
	bool loneLf = false;
	for (size_t i = 0; i < text.size(); ++i) if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) loneLf = true;
	CHECK(!loneLf);
	std::wstring csv = reportAsCsv(r);
	CHECK(csv.find(L"Error,\"x, \"\"y\"\".mp3\",0,Copy failed,") != std::wstring::npos);
	CHECK(reportSummary(r).find(L"Added: 1") != std::wstring::npos);
}

// In-memory destination that behaves like MTP: no usable dates, no local path for tag reading.
class MemFs : public DestFs {
public:
	struct File { std::wstring rel; std::string data; };
	std::map<std::wstring, File> files;  // keyed by pathKey
	std::map<std::wstring, std::wstring> dirs;
	bool rootExists = true;
	uint64_t freeBytes = 1ull << 40;
	int uploads = 0;

	std::wstring describe() const override { return L"mem://test"; }
	bool openRoot(bool create, bool& missing, std::wstring&) override {
		missing = false;
		if (!rootExists) { if (create) rootExists = true; else missing = true; }
		return true;
	}
	DestStat stat(const std::wstring& rel) override {
		DestStat st;
		auto f = files.find(pathKey(rel));
		if (f != files.end()) { st.exists = true; st.size = f->second.data.size(); return st; }
		if (dirs.count(pathKey(rel))) { st.exists = true; st.isDir = true; }
		return st;
	}
	void listFiles(const std::function<bool(const std::wstring&, uint64_t)>& cb) override {
		auto copy = files;
		for (auto& kv : copy) if (!cb(kv.second.rel, kv.second.data.size())) return;
	}
	bool ensureDir(const std::wstring& relDir, std::wstring&) override {
		for (std::wstring d = relDir; !d.empty(); d = parentRel(d)) dirs[pathKey(d)] = d;
		return true;
	}
	bool copyIn(const std::wstring& src, uint64_t, const std::wstring& rel, bool, const CopyTimes&, CopyProgress&, bool& aborted, std::wstring&) override {
		aborted = false;
		++uploads;
		files[pathKey(rel)] = File{ rel, readFile_(src) };
		return true;
	}
	bool remove(const std::wstring& rel, std::wstring&) override { files.erase(pathKey(rel)); return true; }
	bool removeDirIfEmpty(const std::wstring& relDir) override {
		const std::wstring prefix = pathKey(relDir) + L"\\";
		for (auto& kv : files) if (kv.first.rfind(prefix, 0) == 0) return false;
		for (auto& kv : dirs) if (kv.first.rfind(prefix, 0) == 0) return false;
		return dirs.erase(pathKey(relDir)) > 0;
	}
	bool readFile(const std::wstring& rel, std::string& out) override {
		auto f = files.find(pathKey(rel));
		if (f == files.end()) return false;
		out = f->second.data;
		return true;
	}
	bool writeFile(const std::wstring& rel, const std::string& data, bool, std::wstring&) override {
		files[pathKey(rel)] = File{ rel, data };
		return true;
	}
	bool freeSpace(uint64_t& b) override { b = freeBytes; return true; }
	bool hasReliableDates() const override { return false; }

private:
	static std::string readFile_(const std::wstring& path) { return ::readFile(path); }
};

static Report runMem(MemFs& fs, Options o, const std::vector<SourceFile>& files, DestTagReader reader = nullptr) {
	NullProgress p;
	return runSync(o, fs, files, {}, p, reader);
}

static void testMtpLikeDestination() {
	std::printf("MTP-like destination (no dates, no local path)\n");
	Fixture fx(L"memfs");
	writeFile(fx.src + L"\\a.mp3", "aaaa");
	writeFile(fx.src + L"\\b.mp3", "bbbb");
	MemFs fs;
	Options o = fx.opts();
	std::vector<SourceFile> files{ fx.file(L"a.mp3", L"Art\\a.mp3"), fx.file(L"b.mp3", L"Art\\b.mp3") };

	Report r1 = runMem(fs, o, files);
	CHECK(r1.count(Action::Added) == 2);
	CHECK(fs.files.count(pathKey(L".playlist_sync")) == 1);

	// The device reports no dates; the manifest alone decides.
	Report r2 = runMem(fs, o, files);
	CHECK(r2.count(Action::Skipped) == 2);
	setMtime(fx.src + L"\\a.mp3", 3600);
	Report r3 = runMem(fs, o, files);
	CHECK(r3.count(Action::Replaced) == 1);

	// Without a manifest: same size counts as identical, a different size is replaced.
	fs.files.erase(pathKey(L".playlist_sync"));
	fs.files[pathKey(L"Art\\b.mp3")].data = "old";
	int reads = 0;
	o.policy = ExistingPolicy::TagsOnly;
	Report r4 = runMem(fs, o, files, [&](const std::wstring&, uint64_t&, uint64_t&) { ++reads; return true; });
	CHECK(reads == 0);
	CHECK(r4.count(Action::Skipped) == 1);
	CHECK(r4.count(Action::Replaced) == 1);
	CHECK(fs.files[pathKey(L"Art\\b.mp3")].data == "bbbb");

	// Removal and folder pruning through the abstraction.
	o.removeOrphans = true;
	Report r5 = runMem(fs, o, { fx.file(L"a.mp3", L"Art\\a.mp3") });
	CHECK(r5.count(Action::Removed) == 1);
	CHECK(fs.files.count(pathKey(L"Art\\b.mp3")) == 0);

	// Free space is checked before anything changes.
	writeFile(fx.src + L"\\big.mp3", std::string(5000, 'x'));
	fs.freeBytes = 100;
	const int before = fs.uploads;
	Report r6 = runMem(fs, o, { fx.file(L"a.mp3", L"Art\\a.mp3"), fx.file(L"big.mp3", L"big.mp3") });
	CHECK(!r6.fatalError.empty());
	CHECK(fs.uploads == before);
}

static void testNomedia() {
	std::printf(".nomedia file\n");
	Fixture fx(L"nomedia");
	writeFile(fx.src + L"\\a.mp3", "a");
	Options o = fx.opts();
	o.createNomedia = true;

	o.dryRun = true;
	Report r0 = run(o, { fx.file(L"a.mp3", L"a.mp3") });
	CHECK(r0.count(Action::Added) == 2);
	CHECK(!exists(fx.dst + L"\\.nomedia"));

	o.dryRun = false;
	Report r1 = run(o, { fx.file(L"a.mp3", L"a.mp3") });
	CHECK(exists(fx.dst + L"\\.nomedia"));
	CHECK(r1.count(Action::Added) == 2);

	// Already there: no action; never removed as an orphan.
	o.removeOrphans = true;
	o.removeOnlyManaged = false;
	Report r2 = run(o, { fx.file(L"a.mp3", L"a.mp3") });
	CHECK(r2.count(Action::Added) == 0);
	CHECK(r2.count(Action::Removed) == 0);
	CHECK(exists(fx.dst + L"\\.nomedia"));
}

// Diagnostics against real hardware:
//   core_tests --mtp                 list connected MTP storages
//   core_tests --mtp-preview <url>   open a destination read-only and list what's there
// Full add / skip / replace / remove cycle against a real device folder that must be empty or missing.
// Everything the test creates is removed again at the end (except the empty test folder itself).
static int mtpSelfTest(const std::wstring& url) {
	{
		auto probe = createMtpFs(url);
		bool missing = false;
		std::wstring err;
		if (!probe->openRoot(false, missing, err)) { std::wprintf(L"open failed: %s\n", err.c_str()); return 1; }
		size_t existing = 0;
		if (!missing) probe->listFiles([&](const std::wstring&, uint64_t) { ++existing; return false; });
		if (existing) { std::wprintf(L"refusing: %s is not empty\n", url.c_str()); return 1; }
	}
	Fixture fx(L"mtpself");
	writeFile(fx.src + L"\\one.mp3", std::string(300000, '1'));
	writeFile(fx.src + L"\\two.mp3", "two");
	writeFile(fx.src + L"\\three.flac", "three");
	Options o;
	o.destRoot = url;
	o.writePlaylists = true;
	o.createNomedia = true;
	std::vector<SourceFile> files{ fx.file(L"one.mp3", L"A\\one.mp3"), fx.file(L"two.mp3", L"A\\two.mp3"), fx.file(L"three.flac", L"B\\C\\three.flac") };
	PlaylistOut pl{ L"Self test", { { L"A\\one.mp3", 1, L"one" }, { L"B\\C\\three.flac", 1, L"three" } } };
	auto runUrl = [&](const Options& opt, const std::vector<SourceFile>& f) { NullProgress p; return runSync(opt, f, { pl }, p, nullptr); };
	auto show = [](const char* step, const Report& r) {
		std::printf("%s: added %zu, replaced %zu, removed %zu, skipped %zu, errors %zu%s\n", step,
			r.count(Action::Added), r.count(Action::Replaced), r.count(Action::Removed), r.count(Action::Skipped), r.count(Action::Error),
			r.fatalError.empty() ? "" : " FATAL");
		if (!r.fatalError.empty()) std::wprintf(L"  %s\n", r.fatalError.c_str());
		for (auto& e : r.entries) if (e.action == Action::Error) std::wprintf(L"  error: %s: %s\n", e.destRel.c_str(), e.reason.c_str());
	};

	Report r1 = runUrl(o, files);                 show("1 first sync", r1);
	CHECK(r1.count(Action::Added) == 4 && r1.count(Action::Error) == 0); // 3 tracks + .nomedia
	Report r2 = runUrl(o, files);                 show("2 rerun", r2);
	CHECK(r2.count(Action::Skipped) == 3);
	writeFile(fx.src + L"\\two.mp3", "two, longer now");
	Report r3 = runUrl(o, files);                 show("3 replace", r3);
	CHECK(r3.count(Action::Replaced) == 1 && r3.count(Action::Error) == 0);
	{
		auto fs = createMtpFs(url);
		bool missing = false; std::wstring err;
		fs->openRoot(false, missing, err);
		std::string data;
		CHECK(fs->readFile(L"A\\two.mp3", data) && data == "two, longer now");
		CHECK(!fs->stat(L"A\\two.mp3.psync-tmp").exists);
		CHECK(fs->readFile(L"Self test.m3u8", data) && data.find("B\\C\\three.flac") != std::string::npos);
	}
	o.removeOrphans = true;
	Report r4 = runUrl(o, { files[0] });          show("4 remove", r4);
	CHECK(r4.count(Action::Removed) == 2);
	{
		auto fs = createMtpFs(url);
		bool missing = false; std::wstring err;
		fs->openRoot(false, missing, err);
		CHECK(!fs->stat(L"B").exists); // emptied folders are pruned
		// Clean up everything we created.
		std::vector<std::wstring> all;
		fs->listFiles([&](const std::wstring& rel, uint64_t) { all.push_back(rel); return true; });
		for (auto& rel : all) fs->remove(rel, err);
		fs->removeDirIfEmpty(L"A");
		size_t left = 0;
		fs->listFiles([&](const std::wstring&, uint64_t) { ++left; return true; });
		CHECK(left == 0);
	}
	{
		// Remove the (now empty) test folder itself via its parent.
		const size_t slash = url.find_last_of(L'/');
		auto parent = createMtpFs(url.substr(0, slash));
		bool missing = false; std::wstring err;
		CHECK(parent->openRoot(false, missing, err) && parent->removeDirIfEmpty(url.substr(slash + 1)));
	}
	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}

static int mtpDiagnostics(int argc, wchar_t** argv) {
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	auto storages = listMtpStorages(true); // include ordinary drives so the WPD code can be exercised without a phone
	std::printf("%zu MTP storage(s)\n", storages.size());
	for (auto& s : storages) std::wprintf(L"  %s\n", s.url().c_str());
	if (argc >= 3 && wcscmp(argv[1], L"--mtp-preview") == 0) {
		auto fs = createMtpFs(argv[2]);
		bool missing = false;
		std::wstring err;
		if (!fs->openRoot(false, missing, err)) { std::wprintf(L"open failed: %s\n", err.c_str()); return 1; }
		std::wprintf(L"opened %s%s\n", argv[2], missing ? L" (folder doesn't exist yet)" : L"");
		uint64_t freeBytes = 0;
		if (fs->freeSpace(freeBytes)) std::printf("free space: %.1f GB\n", freeBytes / double(1ull << 30));
		size_t n = 0;
		if (!missing) fs->listFiles([&](const std::wstring& rel, uint64_t size) {
			if (n < 20) std::wprintf(L"  %s  (%llu)\n", rel.c_str(), (unsigned long long)size);
			++n;
			return true;
		});
		std::printf("%zu file(s)\n", n);
	}
	if (argc >= 3 && wcscmp(argv[1], L"--mtp-selftest") == 0) return mtpSelfTest(argv[2]);
	if (argc >= 3 && wcscmp(argv[1], L"--mtp-dryrun") == 0) {
		// Preview (dry run) of every file in the destination's sync record; sources are fake, so this
		// measures the destination side of a comparison only. Changes nothing.
		auto fs = createMtpFs(argv[2]);
		bool missing = false;
		std::wstring err;
		std::string manifest;
		if (!fs->openRoot(false, missing, err) || !fs->readFile(kManifestName, manifest)) { std::printf("no sync record\n"); return 1; }
		std::vector<SourceFile> files;
		size_t pos = 0;
		while ((pos = manifest.find('\n', pos)) != std::string::npos) {
			size_t tab = manifest.find('\t', ++pos);
			if (tab == std::string::npos || manifest[pos] == '#') continue;
			std::string rel = manifest.substr(pos, tab - pos);
			int n = MultiByteToWideChar(CP_UTF8, 0, rel.data(), (int)rel.size(), nullptr, 0);
			std::wstring w(n, L'\0');
			MultiByteToWideChar(CP_UTF8, 0, rel.data(), (int)rel.size(), w.data(), n);
			SourceFile f;
			f.sourcePath = L"Z:\\does-not-exist\\" + w;
			f.destRel = w;
			files.push_back(f);
		}
		Options o;
		o.destRoot = argv[2];
		o.dryRun = true;
		const ULONGLONG t0 = GetTickCount64();
		NullProgress p;
		Report r = runSync(o, files, {}, p, nullptr);
		std::printf("%zu files compared in %llu ms (%zu present on device)\n", files.size(), GetTickCount64() - t0, r.count(Action::Error));
	}
	return 0;
}

int wmain(int argc, wchar_t** argv) {
	wchar_t tmp[MAX_PATH];
	GetTempPathW(MAX_PATH, tmp);
	g_base = std::wstring(tmp) + L"psync_tests";
	removeTree(g_base);
	CreateDirectoryW(g_base.c_str(), nullptr);
	if (argc >= 2 && wcsncmp(argv[1], L"--mtp", 5) == 0) {
		const int rc = mtpDiagnostics(argc, argv);
		removeTree(g_base);
		return rc;
	}

	testHelpers();
	testReportFormat();
	testFreshSyncAndRerun();
	testNoPreserveTimestamps();
	testTagsOnlyPolicy();
	testTagsOnlyWithoutManifestUsesReader();
	testNeverPolicy();
	testRemoveManagedOnly();
	testRemoveAll();
	testRemoveAllScopeAndHidden();
	testSafetyEmptyList();
	testMissingSourceKeepsDest();
	testDryRunChangesNothing();
	testCollisionAndDuplicates();
	testPlaylists();
	testAbort();
	testMtpLikeDestination();
	testNomedia();

	removeTree(g_base);
	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
