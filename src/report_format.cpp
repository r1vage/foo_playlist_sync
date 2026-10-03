#include "report_format.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace psync {

	std::wstring formatBytesShort(uint64_t b) {
		wchar_t buf[64];
		if (b >= (1ull << 30)) swprintf_s(buf, L"%.2f GB", b / double(1ull << 30));
		else if (b >= (1ull << 20)) swprintf_s(buf, L"%.1f MB", b / double(1ull << 20));
		else if (b >= 1024) swprintf_s(buf, L"%.0f KB", b / 1024.0);
		else swprintf_s(buf, L"%llu B", (unsigned long long)b);
		return buf;
	}

	std::wstring formatTimestamp(int64_t ft, bool forFileName) {
		FILETIME utc, local;
		utc.dwLowDateTime = (DWORD)((uint64_t)ft & 0xFFFFFFFF);
		utc.dwHighDateTime = (DWORD)((uint64_t)ft >> 32);
		SYSTEMTIME st{};
		if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st)) return L"";
		wchar_t buf[64];
		if (forFileName) swprintf_s(buf, L"%04u-%02u-%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
		else swprintf_s(buf, L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
		return buf;
	}

	static std::wstring statusLine(const Report& r) {
		if (!r.fatalError.empty()) return L"Failed: " + r.fatalError;
		if (r.aborted) return L"Cancelled; the report lists what was done before cancelling.";
		return r.dryRun ? L"Preview only; nothing was changed." : L"Completed.";
	}

	std::wstring reportSummary(const Report& r) {
		wchar_t buf[512];
		const bool dry = r.dryRun;
		swprintf_s(buf, L"%s: %zu    %s: %zu    %s: %zu    Skipped: %zu    Errors: %zu",
			dry ? L"To add" : L"Added", r.count(Action::Added),
			dry ? L"To replace" : L"Replaced", r.count(Action::Replaced),
			dry ? L"To remove" : L"Removed", r.count(Action::Removed),
			r.count(Action::Skipped), r.count(Action::Error));
		std::wstring s = statusLine(r) + L"\n" + buf + L"\n";
		if (dry) {
			uint64_t toCopy = 0;
			for (auto& e : r.entries) if (e.action == Action::Added || e.action == Action::Replaced) toCopy += e.size;
			s += L"Would copy " + formatBytesShort(toCopy);
		} else {
			s += L"Copied " + formatBytesShort(r.bytesCopied) + L",  removed " + formatBytesShort(r.bytesRemoved);
		}
		wchar_t t[64];
		swprintf_s(t, L"  in %.1f s", r.seconds);
		s += t;
		s += L"    Destination: " + r.destRoot;
		return s;
	}

	std::wstring reportAsText(const Report& r) {
		std::wstring s = L"Playlist Sync report, " + formatTimestamp(r.startedAt, false) + (r.dryRun ? L" (preview)" : L"") + L"\r\n";
		s += L"Destination: " + r.destRoot + L"\r\n";
		std::wstring summary = reportSummary(r);
		for (size_t p = 0; (p = summary.find(L'\n', p)) != std::wstring::npos; p += 2) summary.replace(p, 1, L"\r\n");
		s += summary + L"\r\n";

		const Action order[] = { Action::Error, Action::Added, Action::Replaced, Action::Removed, Action::Playlist, Action::Skipped };
		const wchar_t* titles[] = { L"Errors", r.dryRun ? L"To add" : L"Added", r.dryRun ? L"To replace" : L"Replaced",
			r.dryRun ? L"To remove" : L"Removed", L"Playlists", L"Skipped" };
		for (size_t i = 0; i < std::size(order); ++i) {
			size_t n = r.count(order[i]);
			if (!n) continue;
			s += L"\r\n[" + std::wstring(titles[i]) + L"] " + std::to_wstring(n) + L"\r\n";
			for (auto& e : r.entries) {
				if (e.action != order[i]) continue;
				s += L"  " + (e.destRel.empty() ? e.source : e.destRel);
				if (e.size) s += L"  (" + formatBytesShort(e.size) + L")";
				if (!e.reason.empty()) s += L"  - " + e.reason;
				if (!e.source.empty() && !e.destRel.empty() && order[i] != Action::Playlist) s += L"\r\n      from " + e.source;
				s += L"\r\n";
			}
		}
		return s;
	}

	static std::wstring csvField(const std::wstring& v) {
		if (v.find_first_of(L",\"\r\n") == std::wstring::npos) return v;
		std::wstring o = L"\"";
		for (wchar_t c : v) { if (c == L'"') o += L'"'; o += c; }
		return o + L"\"";
	}

	std::wstring reportAsCsv(const Report& r) {
		std::wstring s = L"Action,Destination,Size,Reason,Source\r\n";
		for (auto& e : r.entries) {
			s += csvField(actionName(e.action, r.dryRun)) + L"," + csvField(e.destRel) + L"," + std::to_wstring(e.size) + L"," +
				csvField(e.reason) + L"," + csvField(e.source) + L"\r\n";
		}
		return s;
	}
}
