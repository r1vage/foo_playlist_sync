#include "stdafx.h"
#include "config.h"
#include "sync_task.h"

namespace psync {

	namespace {

		constexpr GUID guid_menu_group = { 0x48cc3383, 0xcccd, 0x4b1f, { 0x9b, 0x09, 0xa0, 0x98, 0x47, 0xd0, 0x39, 0x24 } };
		constexpr GUID guid_cmd_sync = { 0x48de70d7, 0xc919, 0x42f8, { 0xa8, 0xd5, 0x35, 0x45, 0x35, 0x85, 0x5a, 0xc8 } };
		constexpr GUID guid_cmd_preview = { 0xda4d89db, 0x4ad0, 0x43a9, { 0xb3, 0x7b, 0xc8, 0x35, 0xee, 0x8a, 0xd6, 0x59 } };
		constexpr GUID guid_cmd_settings = { 0x129ef772, 0xefe2, 0x4d38, { 0x80, 0xa9, 0x3f, 0x77, 0x16, 0xc8, 0x6d, 0x16 } };

		mainmenu_group_popup_factory g_menuGroup(guid_menu_group, mainmenu_groups::file, mainmenu_commands::sort_priority_dontcare, "Playlist Sync");

		class MainMenuCommands : public mainmenu_commands {
		public:
			enum { cmdSync, cmdPreview, cmdSettings, cmdCount };

			t_uint32 get_command_count() override { return cmdCount; }

			GUID get_command(t_uint32 index) override {
				switch (index) {
				case cmdSync: return guid_cmd_sync;
				case cmdPreview: return guid_cmd_preview;
				case cmdSettings: return guid_cmd_settings;
				default: uBugCheck();
				}
			}

			void get_name(t_uint32 index, pfc::string_base& out) override {
				switch (index) {
				case cmdSync: out = "Sync now"; break;
				case cmdPreview: out = "Preview sync"; break;
				case cmdSettings: out = "Settings..."; break;
				default: uBugCheck();
				}
			}

			bool get_description(t_uint32 index, pfc::string_base& out) override {
				switch (index) {
				case cmdSync: out = "Copies the selected playlists to the destination folder and shows a report."; return true;
				case cmdPreview: out = "Shows what a sync would add, replace and remove, without changing anything."; return true;
				case cmdSettings: out = "Opens the Playlist Sync preferences."; return true;
				default: return false;
				}
			}

			bool get_display(t_uint32 index, pfc::string_base& text, t_uint32& flags) override {
				flags = 0;
				if ((index == cmdSync || index == cmdPreview) && isSyncRunning()) flags = flag_disabled;
				get_name(index, text);
				return true;
			}

			GUID get_parent() override { return guid_menu_group; }

			void execute(t_uint32 index, service_ptr_t<service_base>) override {
				switch (index) {
				case cmdSync: startSync(false); break;
				case cmdPreview: startSync(true); break;
				case cmdSettings: cfg::showPreferences(); break;
				}
			}
		};

		mainmenu_commands_factory_t<MainMenuCommands> g_mainMenuFactory;
	}
}
