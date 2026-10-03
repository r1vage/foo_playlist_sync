#include "stdafx.h"
#include "config.h"
#include "dest_fs.h"
#include "resource.h"
#include "sync_task.h"

#include <helpers/atl-misc.h>
#include <helpers/DarkMode.h>

namespace psync {

	namespace {

		class CPreferencesPage : public CDialogImpl<CPreferencesPage>, public preferences_page_instance {
		public:
			enum { IDD = IDD_PREFERENCES };

			CPreferencesPage(preferences_page_callback::ptr callback) : m_callback(callback) {}

			t_uint32 get_state() override {
				t_uint32 state = preferences_state::resettable | preferences_state::dark_mode_supported;
				if (m_changed) state |= preferences_state::changed;
				return state;
			}

			void apply() override {
				cfg::destination = getText(IDC_DEST);
				cfg::pattern = getText(IDC_PATTERN);
				cfg::policy = IsDlgButtonChecked(IDC_POLICY_TAGS) ? (int)ExistingPolicy::TagsOnly
					: IsDlgButtonChecked(IDC_POLICY_NEVER) ? (int)ExistingPolicy::Never : (int)ExistingPolicy::SizeOrDate;
				cfg::compareTech = isChecked(IDC_COMPARE_TECH);
				cfg::ignoreTags = getText(IDC_IGNORE_TAGS);
				cfg::preserveTimestamps = isChecked(IDC_PRESERVE_TIMES);
				cfg::removeOrphans = isChecked(IDC_REMOVE_ORPHANS);
				cfg::removeOnlyManaged = isChecked(IDC_REMOVE_MANAGED);
				cfg::writePlaylists = isChecked(IDC_WRITE_PLAYLISTS);
				cfg::playlistSubdir = getText(IDC_PLAYLIST_SUBDIR);
				cfg::playlistUnixStyle = isChecked(IDC_UNIX_STYLE);
				cfg::createNomedia = isChecked(IDC_CREATE_NOMEDIA);
				cfg::copyArt = isChecked(IDC_COPY_ART);
				cfg::artNames = getText(IDC_ART_NAMES);
				cfg::artDestName = getText(IDC_ART_DEST);
				cfg::artEmbedded = isChecked(IDC_ART_EMBEDDED);
				cfg::saveLogs = isChecked(IDC_SAVE_LOGS);

				std::vector<cfg::SelectedPlaylist> sel;
				for (int i = 0; i < m_playlistList.GetItemCount(); ++i) {
					if (m_playlistList.GetCheckState(i)) sel.push_back(m_rows[i]);
				}
				cfg::setSelectedPlaylists(sel);

				m_changed = false;
				m_callback->on_state_changed();
			}

			void reset() override {
				m_loading = true;
				uSetDlgItemText(m_hWnd, IDC_PATTERN, cfg::defaultPattern);
				uSetDlgItemText(m_hWnd, IDC_IGNORE_TAGS, cfg::defaultIgnoreTags);
				setPolicy(ExistingPolicy::SizeOrDate);
				CheckDlgButton(IDC_COMPARE_TECH, BST_CHECKED);
				CheckDlgButton(IDC_PRESERVE_TIMES, BST_CHECKED);
				CheckDlgButton(IDC_REMOVE_ORPHANS, BST_UNCHECKED);
				CheckDlgButton(IDC_REMOVE_MANAGED, BST_CHECKED);
				CheckDlgButton(IDC_WRITE_PLAYLISTS, BST_UNCHECKED);
				uSetDlgItemText(m_hWnd, IDC_PLAYLIST_SUBDIR, "");
				CheckDlgButton(IDC_UNIX_STYLE, BST_UNCHECKED);
				CheckDlgButton(IDC_CREATE_NOMEDIA, BST_UNCHECKED);
				CheckDlgButton(IDC_COPY_ART, BST_UNCHECKED);
				uSetDlgItemText(m_hWnd, IDC_ART_NAMES, cfg::defaultArtNames);
				uSetDlgItemText(m_hWnd, IDC_ART_DEST, cfg::defaultArtDestName);
				CheckDlgButton(IDC_ART_EMBEDDED, BST_CHECKED);
				CheckDlgButton(IDC_SAVE_LOGS, BST_CHECKED);
				// Destination and playlist selection are deliberately kept: they're not "preferences" with sensible defaults.
				m_loading = false;
				updateUi();
				onChanged();
			}

			BEGIN_MSG_MAP_EX(CPreferencesPage)
				MSG_WM_INITDIALOG(OnInitDialog)
				COMMAND_HANDLER_EX(IDC_DEST, EN_CHANGE, OnEditChange)
				COMMAND_HANDLER_EX(IDC_PATTERN, EN_CHANGE, OnPatternChange)
				COMMAND_HANDLER_EX(IDC_IGNORE_TAGS, EN_CHANGE, OnEditChange)
				COMMAND_HANDLER_EX(IDC_PLAYLIST_SUBDIR, EN_CHANGE, OnEditChange)
				COMMAND_HANDLER_EX(IDC_ART_NAMES, EN_CHANGE, OnEditChange)
				COMMAND_HANDLER_EX(IDC_ART_DEST, EN_CHANGE, OnEditChange)
				COMMAND_ID_HANDLER_EX(IDC_COPY_ART, OnToggle)
				COMMAND_ID_HANDLER_EX(IDC_ART_EMBEDDED, OnToggle)
				COMMAND_ID_HANDLER_EX(IDC_SAVE_LOGS, OnToggle)
				COMMAND_ID_HANDLER_EX(IDC_POLICY_SIZEDATE, OnToggle)
				COMMAND_ID_HANDLER_EX(IDC_POLICY_TAGS, OnToggle)
				COMMAND_ID_HANDLER_EX(IDC_POLICY_NEVER, OnToggle)
				COMMAND_ID_HANDLER_EX(IDC_COMPARE_TECH, OnToggle)
				COMMAND_ID_HANDLER_EX(IDC_PRESERVE_TIMES, OnToggle)
				COMMAND_ID_HANDLER_EX(IDC_REMOVE_ORPHANS, OnToggle)
				COMMAND_ID_HANDLER_EX(IDC_REMOVE_MANAGED, OnToggle)
				COMMAND_ID_HANDLER_EX(IDC_WRITE_PLAYLISTS, OnToggle)
				COMMAND_ID_HANDLER_EX(IDC_UNIX_STYLE, OnToggle)
				COMMAND_ID_HANDLER_EX(IDC_CREATE_NOMEDIA, OnToggle)
				COMMAND_ID_HANDLER_EX(IDC_BROWSE, OnBrowse)
				COMMAND_ID_HANDLER_EX(IDC_PREVIEW, OnPreview)
				COMMAND_ID_HANDLER_EX(IDC_SYNCNOW, OnSyncNow)
				NOTIFY_HANDLER_EX(IDC_PLAYLISTS, LVN_ITEMCHANGED, OnPlaylistItemChanged)
			END_MSG_MAP()

		private:
			BOOL OnInitDialog(CWindow, LPARAM) {
				m_dark.AddDialogWithControls(*this);
				m_loading = true;

				uSetDlgItemText(m_hWnd, IDC_DEST, cfg::destination.get());
				uSetDlgItemText(m_hWnd, IDC_PATTERN, cfg::getPattern());
				setPolicy((ExistingPolicy)cfg::policy.get());
				CheckDlgButton(IDC_COMPARE_TECH, cfg::compareTech ? BST_CHECKED : BST_UNCHECKED);
				uSetDlgItemText(m_hWnd, IDC_IGNORE_TAGS, cfg::ignoreTags.get());
				CheckDlgButton(IDC_PRESERVE_TIMES, cfg::preserveTimestamps ? BST_CHECKED : BST_UNCHECKED);
				CheckDlgButton(IDC_REMOVE_ORPHANS, cfg::removeOrphans ? BST_CHECKED : BST_UNCHECKED);
				CheckDlgButton(IDC_REMOVE_MANAGED, cfg::removeOnlyManaged ? BST_CHECKED : BST_UNCHECKED);
				CheckDlgButton(IDC_WRITE_PLAYLISTS, cfg::writePlaylists ? BST_CHECKED : BST_UNCHECKED);
				uSetDlgItemText(m_hWnd, IDC_PLAYLIST_SUBDIR, cfg::playlistSubdir.get());
				CheckDlgButton(IDC_UNIX_STYLE, cfg::playlistUnixStyle ? BST_CHECKED : BST_UNCHECKED);
				CheckDlgButton(IDC_CREATE_NOMEDIA, cfg::createNomedia ? BST_CHECKED : BST_UNCHECKED);
				CheckDlgButton(IDC_COPY_ART, cfg::copyArt ? BST_CHECKED : BST_UNCHECKED);
				uSetDlgItemText(m_hWnd, IDC_ART_NAMES, cfg::artNames.get());
				uSetDlgItemText(m_hWnd, IDC_ART_DEST, cfg::artDestName.get());
				CheckDlgButton(IDC_ART_EMBEDDED, cfg::artEmbedded ? BST_CHECKED : BST_UNCHECKED);
				CheckDlgButton(IDC_SAVE_LOGS, cfg::saveLogs ? BST_CHECKED : BST_UNCHECKED);
				SetDlgItemTextW(IDC_REMOVE_WARNING, L"Warning: deletes every other file in the destination!");

				m_playlistList = GetDlgItem(IDC_PLAYLISTS);
				m_playlistList.SetExtendedListViewStyle(LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
				CRect rc;
				m_playlistList.GetClientRect(&rc);
				m_playlistList.InsertColumn(0, L"Playlist", LVCFMT_LEFT, rc.Width() - GetSystemMetrics(SM_CXVSCROLL));
				fillPlaylists();

				m_loading = false;
				updateUi();
				updatePatternPreview();
				return FALSE;
			}

			void fillPlaylists() {
				auto selected = cfg::getSelectedPlaylists();
				auto isSelected = [&](const GUID& g, const char* name, bool& matchedByName) {
					for (auto& s : selected) if (s.guid == g) return true;
					for (auto& s : selected) if (s.guid == pfc::guid_null && s.name == name) { matchedByName = true; return true; }
					return false;
				};
				auto pm = playlist_manager_v5::get();
				std::vector<bool> used(selected.size(), false);
				const size_t count = pm->get_playlist_count();
				for (size_t i = 0; i < count; ++i) {
					pfc::string8 name;
					pm->playlist_get_name(i, name);
					cfg::SelectedPlaylist row{ pm->playlist_get_guid(i), name };
					bool byName = false;
					bool checked = isSelected(row.guid, name, byName);
					for (size_t s = 0; s < selected.size(); ++s) if (selected[s].guid == row.guid) used[s] = true;
					int idx = m_playlistList.InsertItem(m_playlistList.GetItemCount(), pfc::stringcvt::string_wide_from_utf8(name));
					m_playlistList.SetCheckState(idx, checked);
					m_rows.push_back(row);
				}
				// Selected playlists that were deleted: list them so the user can see and untick them.
				for (size_t s = 0; s < selected.size(); ++s) {
					if (used[s] || pm->find_playlist(selected[s].name) != SIZE_MAX) continue;
					pfc::string8 label = selected[s].name;
					label << " (missing)";
					int idx = m_playlistList.InsertItem(m_playlistList.GetItemCount(), pfc::stringcvt::string_wide_from_utf8(label));
					m_playlistList.SetCheckState(idx, TRUE);
					m_rows.push_back(selected[s]);
				}
			}

			void setPolicy(ExistingPolicy p) {
				CheckRadioButton(IDC_POLICY_SIZEDATE, IDC_POLICY_NEVER,
					p == ExistingPolicy::TagsOnly ? IDC_POLICY_TAGS : p == ExistingPolicy::Never ? IDC_POLICY_NEVER : IDC_POLICY_SIZEDATE);
			}

			pfc::string8 getText(int id) { return uGetDlgItemText(m_hWnd, id); }
			bool isChecked(int id) { return IsDlgButtonChecked(id) == BST_CHECKED; }

			void updateUi() {
				const bool tags = isChecked(IDC_POLICY_TAGS);
				for (int id : { IDC_COMPARE_TECH, IDC_IGNORE_TAGS_LABEL, IDC_IGNORE_TAGS, IDC_IGNORE_TAGS_HINT }) GetDlgItem(id).EnableWindow(tags);
				const bool remove = isChecked(IDC_REMOVE_ORPHANS);
				GetDlgItem(IDC_REMOVE_MANAGED).EnableWindow(remove);
				GetDlgItem(IDC_REMOVE_WARNING).ShowWindow(remove && !isChecked(IDC_REMOVE_MANAGED) ? SW_SHOW : SW_HIDE);
				const bool art = isChecked(IDC_COPY_ART);
				for (int id : { IDC_ART_NAMES_LABEL, IDC_ART_NAMES, IDC_ART_DEST_LABEL, IDC_ART_DEST, IDC_ART_EMBEDDED }) GetDlgItem(id).EnableWindow(art);
				const bool pls = isChecked(IDC_WRITE_PLAYLISTS);
				for (int id : { IDC_PLAYLIST_SUBDIR_LABEL, IDC_PLAYLIST_SUBDIR, IDC_UNIX_STYLE }) GetDlgItem(id).EnableWindow(pls);
			}

			void updatePatternPreview() {
				pfc::string8 pattern = getText(IDC_PATTERN);
				pfc::string8 example = pattern.is_empty() ? pfc::string8() : previewPattern(pattern);
				pfc::string8 text;
				if (pattern.is_empty()) text = "Uses title formatting; the file extension is added automatically.";
				else if (!example.is_empty()) text << "Example: " << example;
				uSetDlgItemText(m_hWnd, IDC_PATTERN_PREVIEW, text);
			}

			void onChanged() {
				if (m_loading) return;
				m_changed = true;
				m_callback->on_state_changed();
			}

			void OnEditChange(UINT, int, CWindow) { onChanged(); }

			void OnPatternChange(UINT, int, CWindow) {
				if (!m_loading) updatePatternPreview();
				onChanged();
			}

			void OnToggle(UINT, int, CWindow) {
				updateUi();
				onChanged();
			}

			LRESULT OnPlaylistItemChanged(LPNMHDR hdr) {
				auto nm = reinterpret_cast<NMLISTVIEW*>(hdr);
				if ((nm->uChanged & LVIF_STATE) && ((nm->uNewState ^ nm->uOldState) & LVIS_STATEIMAGEMASK)) onChanged();
				return 0;
			}

			void OnBrowse(UINT, int, CWindow button) {
				// Phones and players connected over MTP have no drive letter; offer them next to the folder picker.
				std::vector<MtpStorageInfo> storages;
				{
					const HCURSOR prevCursor = SetCursor(LoadCursor(nullptr, IDC_WAIT));
					storages = listMtpStorages();
					SetCursor(prevCursor);
				}
				CMenu menu;
				menu.CreatePopupMenu();
				menu.AppendMenu(MF_STRING, ID_BROWSE_FOLDER, L"Choose folder...");
				menu.AppendMenu(MF_SEPARATOR);
				if (storages.empty()) {
					menu.AppendMenu(MF_STRING | MF_GRAYED, (UINT_PTR)0, L"No MTP devices found (connect, unlock, choose File transfer)");
				}
				for (size_t i = 0; i < storages.size(); ++i) {
					std::wstring label = storages[i].device + L"  >  " + storages[i].storage;
					menu.AppendMenu(MF_STRING, ID_BROWSE_MTP_FIRST + (UINT)i, label.c_str());
				}
				CRect rc;
				button.GetWindowRect(&rc);
				const UINT cmd = menu.TrackPopupMenu(TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN, rc.right, rc.bottom, m_hWnd);
				if (cmd == ID_BROWSE_FOLDER) {
					pfc::string8 path = getText(IDC_DEST);
					if (isMtpPath(std::wstring(pfc::stringcvt::string_wide_from_utf8(path)))) path.reset();
					if (fb2k::browseForFolder(m_hWnd, "Choose the destination folder", path)) uSetDlgItemText(m_hWnd, IDC_DEST, path);
				} else if (cmd >= ID_BROWSE_MTP_FIRST && cmd < ID_BROWSE_MTP_FIRST + storages.size()) {
					// Default to a Music folder; the path can be edited freely afterwards.
					std::wstring url = storages[cmd - ID_BROWSE_MTP_FIRST].url() + L"/Music";
					SetDlgItemTextW(IDC_DEST, url.c_str());
				}
			}

			void runNow(bool dryRun) {
				if (m_changed) apply();
				startSync(dryRun);
			}
			void OnPreview(UINT, int, CWindow) { runNow(true); }
			void OnSyncNow(UINT, int, CWindow) { runNow(false); }

			const preferences_page_callback::ptr m_callback;
			fb2k::CDarkModeHooks m_dark;
			CListViewCtrl m_playlistList;
			std::vector<cfg::SelectedPlaylist> m_rows;
			bool m_loading = false;
			bool m_changed = false;
		};

		class PreferencesPage : public preferences_page_impl<CPreferencesPage> {
		public:
			const char* get_name() override { return "Playlist Sync"; }
			GUID get_guid() override { return cfg::guidPreferencesPage; }
			GUID get_parent_guid() override { return guid_tools; }
		};

		preferences_page_factory_t<PreferencesPage> g_preferencesPageFactory;
	}
}
