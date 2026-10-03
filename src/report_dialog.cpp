#include "stdafx.h"
#include "report_dialog.h"
#include "report_format.h"
#include "resource.h"
#include "sync_task.h"

#include <helpers/atl-misc.h>
#include <helpers/DarkMode.h>

#include <algorithm>
#include <commdlg.h>
#include <shellapi.h>

namespace psync {

	namespace {

		enum Column { colAction, colFile, colSize, colReason, colSource, colCount };

		enum Filter { fChanges, fAll, fAdded, fReplaced, fRemoved, fSkipped, fErrors, fPlaylists, fCount };

		bool filterMatches(Filter f, Action a) {
			switch (f) {
			case fChanges: return a != Action::Skipped && a != Action::Playlist;
			case fAll: return true;
			case fAdded: return a == Action::Added;
			case fReplaced: return a == Action::Replaced;
			case fRemoved: return a == Action::Removed;
			case fSkipped: return a == Action::Skipped;
			case fErrors: return a == Action::Error;
			case fPlaylists: return a == Action::Playlist;
			default: return true;
			}
		}

		std::wstring logFolder() {
			pfc::string8 dir = filesystem::g_get_native_path(core_api::pathInProfile("playlist_sync_logs"));
			return std::wstring(pfc::stringcvt::string_wide_from_utf8(dir).get_ptr());
		}

		bool writeUtf8File(const std::wstring& path, const std::wstring& text) {
			pfc::stringcvt::string_utf8_from_wide u(text.c_str());
			HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE) return false;
			DWORD w = 0;
			bool ok = WriteFile(h, "\xEF\xBB\xBF", 3, &w, nullptr) && WriteFile(h, u.get_ptr(), (DWORD)u.length(), &w, nullptr);
			CloseHandle(h);
			return ok;
		}

		class CReportDialog : public CDialogImpl<CReportDialog> {
		public:
			enum { IDD = IDD_REPORT };

			explicit CReportDialog(Report&& r) : m_report(std::move(r)) {}

			BEGIN_MSG_MAP_EX(CReportDialog)
				MSG_WM_INITDIALOG(OnInitDialog)
				MSG_WM_SIZE(OnSize)
				MSG_WM_GETMINMAXINFO(OnGetMinMaxInfo)
				MSG_WM_DESTROY(OnDestroy)
				COMMAND_ID_HANDLER_EX(IDCANCEL, OnCloseCmd)
				COMMAND_ID_HANDLER_EX(IDC_EXPORT, OnExport)
				COMMAND_ID_HANDLER_EX(IDC_OPENLOG, OnOpenLog)
				COMMAND_ID_HANDLER_EX(IDC_RUNSYNC, OnRunSync)
				COMMAND_HANDLER_EX(IDC_FILTER, CBN_SELCHANGE, OnFilterChange)
				NOTIFY_HANDLER_EX(IDC_LIST, LVN_GETDISPINFO, OnGetDispInfo)
				NOTIFY_HANDLER_EX(IDC_LIST, LVN_COLUMNCLICK, OnColumnClick)
				NOTIFY_HANDLER_EX(IDC_LIST, NM_DBLCLK, OnDoubleClick)
			END_MSG_MAP()

			void OnFinalMessage(HWND) override { delete this; }

		private:
			struct Anchor { int id; bool moveX, moveY, sizeX, sizeY; CRect rc; };

			BOOL OnInitDialog(CWindow, LPARAM) {
				m_dark.AddDialogWithControls(*this);
				SetWindowText(m_report.dryRun ? L"Playlist Sync preview" : L"Playlist Sync report");
				SetDlgItemText(IDC_SUMMARY, reportSummary(m_report).c_str());
				GetDlgItem(IDC_RUNSYNC).ShowWindow(m_report.dryRun && m_report.fatalError.empty() ? SW_SHOW : SW_HIDE);

				m_list = GetDlgItem(IDC_LIST);
				m_list.SetExtendedListViewStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP | LVS_EX_LABELTIP);
				struct { const wchar_t* name; int dlu; int fmt; } cols[colCount] = {
					{ L"Action", 50, LVCFMT_LEFT }, { L"File", 200, LVCFMT_LEFT }, { L"Size", 40, LVCFMT_RIGHT },
					{ L"Reason", 140, LVCFMT_LEFT }, { L"Source", 180, LVCFMT_LEFT } };
				for (int i = 0; i < colCount; ++i) {
					CRect r(0, 0, cols[i].dlu, 0);
					MapDialogRect(&r);
					m_list.InsertColumn(i, cols[i].name, cols[i].fmt, r.Width());
				}

				CComboBox combo(GetDlgItem(IDC_FILTER));
				const bool dry = m_report.dryRun;
				const wchar_t* names[fCount] = { L"Changes and errors", L"Everything", dry ? L"To add" : L"Added",
					dry ? L"To replace" : L"Replaced", dry ? L"To remove" : L"Removed", L"Skipped", L"Errors", L"Playlists" };
				size_t changes = 0;
				for (int f = 0; f < fCount; ++f) {
					size_t n = std::count_if(m_report.entries.begin(), m_report.entries.end(),
						[f](const ReportEntry& e) { return filterMatches((Filter)f, e.action); });
					if (f == fChanges) changes = n;
					combo.AddString((std::wstring(names[f]) + L" (" + std::to_wstring(n) + L")").c_str());
				}
				combo.SetCurSel(changes > 0 ? fChanges : fAll);
				applyFilter();

				CRect client;
				GetClientRect(&client);
				m_initialClient = client.Size();
				const Anchor anchors[] = {
					{ IDC_SUMMARY, false, false, true, false }, { IDC_LIST, false, false, true, true },
					{ IDC_EXPORT, false, true, false, false }, { IDC_OPENLOG, false, true, false, false },
					{ IDC_RUNSYNC, true, true, false, false }, { IDCANCEL, true, true, false, false } };
				for (auto a : anchors) {
					GetDlgItem(a.id).GetWindowRect(&a.rc);
					ScreenToClient(&a.rc);
					m_anchors.push_back(a);
				}
				CRect wr;
				GetWindowRect(&wr);
				m_minSize = wr.Size();

				modeless_dialog_manager::g_add(m_hWnd);
				ShowWindow(SW_SHOW);
				return TRUE;
			}

			void OnDestroy() {
				modeless_dialog_manager::g_remove(m_hWnd);
			}

			void OnGetMinMaxInfo(LPMINMAXINFO info) {
				info->ptMinTrackSize.x = m_minSize.cx * 2 / 3;
				info->ptMinTrackSize.y = m_minSize.cy / 2;
			}

			void OnSize(UINT, CSize size) {
				if (m_anchors.empty()) return;
				const int dx = size.cx - m_initialClient.cx, dy = size.cy - m_initialClient.cy;
				HDWP dwp = BeginDeferWindowPos((int)m_anchors.size());
				for (auto& a : m_anchors) {
					CRect r = a.rc;
					if (a.moveX) r.OffsetRect(dx, 0);
					if (a.moveY) r.OffsetRect(0, dy);
					if (a.sizeX) r.right += dx;
					if (a.sizeY) r.bottom += dy;
					dwp = ::DeferWindowPos(dwp, GetDlgItem(a.id), nullptr, r.left, r.top, r.Width(), r.Height(), SWP_NOZORDER | SWP_NOACTIVATE);
				}
				EndDeferWindowPos(dwp);
				GetDlgItem(IDC_SUMMARY).Invalidate();
			}

			void OnCloseCmd(UINT, int, CWindow) { DestroyWindow(); }

			void OnRunSync(UINT, int, CWindow) {
				DestroyWindow();
				startSync(false);
			}

			void OnFilterChange(UINT, int, CWindow) { applyFilter(); }

			void applyFilter() {
				Filter f = (Filter)CComboBox(GetDlgItem(IDC_FILTER)).GetCurSel();
				m_view.clear();
				for (size_t i = 0; i < m_report.entries.size(); ++i) {
					if (filterMatches(f, m_report.entries[i].action)) m_view.push_back(i);
				}
				sortView();
				m_list.SetItemCountEx((int)m_view.size(), 0);
				m_list.Invalidate();
			}

			std::wstring cellText(const ReportEntry& e, int col) const {
				switch (col) {
				case colAction: return actionName(e.action, m_report.dryRun);
				case colFile: return e.destRel;
				case colSize: return e.size ? formatBytesShort(e.size) : L"";
				case colReason: return e.reason;
				case colSource: return e.source;
				}
				return L"";
			}

			void sortView() {
				if (m_sortCol < 0) return;
				auto& entries = m_report.entries;
				const int col = m_sortCol;
				const bool asc = m_sortAsc;
				std::stable_sort(m_view.begin(), m_view.end(), [&](size_t a, size_t b) {
					const ReportEntry& x = entries[asc ? a : b];
					const ReportEntry& y = entries[asc ? b : a];
					if (col == colSize) return x.size < y.size;
					if (col == colAction) return (int)x.action < (int)y.action;
					return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS,
						cellText(x, col).c_str(), -1, cellText(y, col).c_str(), -1, nullptr, nullptr, 0) == CSTR_LESS_THAN;
				});
			}

			LRESULT OnGetDispInfo(LPNMHDR hdr) {
				auto di = reinterpret_cast<NMLVDISPINFOW*>(hdr);
				if ((di->item.mask & LVIF_TEXT) && di->item.iItem >= 0 && (size_t)di->item.iItem < m_view.size()) {
					std::wstring t = cellText(m_report.entries[m_view[di->item.iItem]], di->item.iSubItem);
					wcsncpy_s(di->item.pszText, di->item.cchTextMax, t.c_str(), _TRUNCATE);
				}
				return 0;
			}

			LRESULT OnColumnClick(LPNMHDR hdr) {
				auto nm = reinterpret_cast<NMLISTVIEW*>(hdr);
				if (nm->iSubItem == m_sortCol) m_sortAsc = !m_sortAsc;
				else { m_sortCol = nm->iSubItem; m_sortAsc = true; }
				CHeaderCtrl header = m_list.GetHeader();
				for (int i = 0; i < colCount; ++i) {
					HDITEM item{ HDI_FORMAT };
					header.GetItem(i, &item);
					item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
					if (i == m_sortCol) item.fmt |= m_sortAsc ? HDF_SORTUP : HDF_SORTDOWN;
					header.SetItem(i, &item);
				}
				sortView();
				m_list.Invalidate();
				return 0;
			}

			LRESULT OnDoubleClick(LPNMHDR hdr) {
				auto nm = reinterpret_cast<NMITEMACTIVATE*>(hdr);
				if (nm->iItem < 0 || (size_t)nm->iItem >= m_view.size()) return 0;
				const ReportEntry& e = m_report.entries[m_view[nm->iItem]];
				std::wstring path;
				if (!e.destRel.empty()) path = m_report.destRoot + L"\\" + e.destRel;
				else if (!e.source.empty()) path = e.source;
				if (path.empty()) return 0;
				if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
					std::wstring args = L"/select,\"" + path + L"\"";
					ShellExecuteW(m_hWnd, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
				} else {
					// Removed files: open the folder they were in, or the closest one that still exists.
					std::wstring dir = path;
					while (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
						const size_t slash = dir.find_last_of(L'\\');
						if (slash == std::wstring::npos || slash < 2) return 0;
						dir.resize(slash);
					}
					ShellExecuteW(m_hWnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
				}
				return 0;
			}

			void OnExport(UINT, int, CWindow) {
				wchar_t file[MAX_PATH * 4] = {};
				std::wstring def = L"playlist_sync_" + formatTimestamp(m_report.startedAt, true);
				wcsncpy_s(file, def.c_str(), _TRUNCATE);
				OPENFILENAMEW ofn{ sizeof(ofn) };
				ofn.hwndOwner = m_hWnd;
				ofn.lpstrFilter = L"Text report (*.txt)\0*.txt\0Spreadsheet (*.csv)\0*.csv\0";
				ofn.lpstrFile = file;
				ofn.nMaxFile = (DWORD)std::size(file);
				ofn.lpstrDefExt = L"txt";
				ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
				if (!GetSaveFileNameW(&ofn)) return;
				std::wstring path = file;
				const bool csv = ofn.nFilterIndex == 2 || (path.size() > 4 && _wcsicmp(path.c_str() + path.size() - 4, L".csv") == 0);
				if (csv && (path.size() < 4 || _wcsicmp(path.c_str() + path.size() - 4, L".csv") != 0)) {
					if (path.size() > 4 && _wcsicmp(path.c_str() + path.size() - 4, L".txt") == 0) path.resize(path.size() - 4);
					path += L".csv";
				}
				if (!writeUtf8File(path, csv ? reportAsCsv(m_report) : reportAsText(m_report))) {
					popup_message::g_complain("Couldn't save the report.");
				}
			}

			void OnOpenLog(UINT, int, CWindow) {
				std::wstring dir = logFolder();
				CreateDirectoryW(dir.c_str(), nullptr);
				ShellExecuteW(m_hWnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			}

			Report m_report;
			std::vector<size_t> m_view;
			int m_sortCol = -1;
			bool m_sortAsc = true;
			CListViewCtrl m_list;
			std::vector<Anchor> m_anchors;
			CSize m_initialClient, m_minSize;
			fb2k::CDarkModeHooks m_dark;
		};
	}

	void showReportDialog(Report&& report) {
		auto dlg = new CReportDialog(std::move(report));
		if (!dlg->Create(core_api::get_main_window())) delete dlg;
	}
}
