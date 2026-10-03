#include "stdafx.h"
#include "config.h"

namespace psync::cfg {

	static constexpr GUID guid_playlists = { 0x6910483d, 0xa7d1, 0x4bbd, { 0x92, 0xec, 0xcd, 0x2d, 0xa4, 0x00, 0xe4, 0x49 } };
	static constexpr GUID guid_destination = { 0x5f261ede, 0x0012, 0x4a95, { 0xb7, 0xcc, 0x61, 0x67, 0x5a, 0x7d, 0x01, 0xd4 } };
	static constexpr GUID guid_pattern = { 0xb90ad304, 0xd124, 0x4086, { 0x91, 0xee, 0xb4, 0xb5, 0x73, 0x11, 0x1b, 0x1d } };
	static constexpr GUID guid_policy = { 0xa7ea3117, 0x9bd2, 0x4249, { 0xbb, 0x3f, 0xd5, 0xb4, 0xdf, 0xde, 0xae, 0x1c } };
	static constexpr GUID guid_compareTech = { 0x36f0f254, 0x8491, 0x4297, { 0x85, 0x33, 0x26, 0x48, 0x26, 0xf3, 0x66, 0x86 } };
	static constexpr GUID guid_ignoreTags = { 0x0a1c05e0, 0x6af1, 0x453d, { 0x9f, 0xab, 0x1d, 0xc4, 0x71, 0x87, 0x8e, 0x7d } };
	static constexpr GUID guid_preserveTimestamps = { 0x56bf4d6c, 0x440d, 0x4e23, { 0xb0, 0x16, 0x85, 0xef, 0xd1, 0xe7, 0xe5, 0x73 } };
	static constexpr GUID guid_removeOrphans = { 0x9d223e2b, 0xa64e, 0x4a78, { 0x85, 0x6f, 0xcf, 0xdd, 0xbb, 0x2b, 0xd4, 0xe8 } };
	static constexpr GUID guid_removeOnlyManaged = { 0x8d5c8e55, 0xab47, 0x47ce, { 0xbf, 0x64, 0x85, 0xab, 0x38, 0xb4, 0x0b, 0xcf } };
	static constexpr GUID guid_writePlaylists = { 0x7cf1cbb7, 0xe40d, 0x48f8, { 0xaf, 0x59, 0x93, 0xb2, 0x17, 0x4f, 0x85, 0x6e } };
	static constexpr GUID guid_playlistSubdir = { 0x94aa848c, 0x1e74, 0x499c, { 0xb4, 0xcf, 0xa1, 0x0c, 0x0d, 0xb0, 0xb9, 0xa3 } };
	static constexpr GUID guid_playlistUnixStyle = { 0xacd78d34, 0x27df, 0x4ce0, { 0x8a, 0xa1, 0x3d, 0x6a, 0x71, 0xb3, 0xb6, 0xff } };
	static constexpr GUID guid_createNomedia = { 0x4f244d7d, 0x81d2, 0x404c, { 0x95, 0xd9, 0x57, 0xa0, 0xa8, 0x59, 0xb0, 0xa3 } };

	const GUID guidPreferencesPage = { 0x49e93b95, 0xb768, 0x4892, { 0x81, 0x75, 0x6e, 0xc9, 0xbf, 0xc2, 0xb2, 0x10 } };

	const char* const defaultPattern =
		"$if2(%album artist%,Unknown Artist)\\"
		"$cut($if2(%album%,Unknown Album),70)$if(%date%,' ('%date%')')\\"
		"$if(%tracknumber%,$if($or($greater(%totaldiscs%,1),$greater(%discnumber%,1)),%discnumber%-)%tracknumber% - )"
		"$if2(%title%,%filename%).$ext(%_path%)";
	// Earlier defaults; settings still holding one of them are moved to the current default.
	static const char* const previousDefaultPatterns[] = {
		"$if2(%album artist%,Unknown Artist)\\$if2(%album%,Unknown Album)\\%filename%",
		"$if2(%album artist%,Unknown Artist)\\$cut($if2(%album%,Unknown Album),70)$if(%date%,' ('%date%')')\\"
		"$if(%tracknumber%,%tracknumber% - )$if2(%title%,%filename%).$ext(%_path%)",
	};
	const char* const defaultIgnoreTags = "";

	cfg_string playlists(guid_playlists, "");
	cfg_string destination(guid_destination, "");
	cfg_string pattern(guid_pattern, defaultPattern);
	cfg_int policy(guid_policy, (int)ExistingPolicy::SizeOrDate);
	cfg_bool compareTech(guid_compareTech, true);
	cfg_string ignoreTags(guid_ignoreTags, defaultIgnoreTags);
	cfg_bool preserveTimestamps(guid_preserveTimestamps, true);
	cfg_bool removeOrphans(guid_removeOrphans, false);
	cfg_bool removeOnlyManaged(guid_removeOnlyManaged, true);
	cfg_bool writePlaylists(guid_writePlaylists, false);
	cfg_string playlistSubdir(guid_playlistSubdir, "");
	cfg_bool playlistUnixStyle(guid_playlistUnixStyle, false);
	cfg_bool createNomedia(guid_createNomedia, false);

	static constexpr GUID guid_copyArt = { 0x1a7c8585, 0x7049, 0x4291, { 0x85, 0xe2, 0xbc, 0x7a, 0x63, 0x1e, 0xce, 0x1d } };
	static constexpr GUID guid_artNames = { 0x710920b8, 0x8dbf, 0x4417, { 0x80, 0xc1, 0x87, 0xd5, 0x5e, 0x07, 0xfa, 0x3a } };
	static constexpr GUID guid_artDestName = { 0x0104ae80, 0xe505, 0x45f8, { 0x93, 0x69, 0x4a, 0x12, 0x90, 0x9c, 0x95, 0x69 } };
	static constexpr GUID guid_artEmbedded = { 0xe032742d, 0x7f23, 0x4c11, { 0x9b, 0xa5, 0xba, 0x5c, 0x25, 0xe3, 0x06, 0x4d } };
	const char* const defaultArtNames = "folder; front; cover";
	const char* const defaultArtDestName = "folder";
	cfg_bool copyArt(guid_copyArt, false);
	cfg_string artNames(guid_artNames, defaultArtNames);
	cfg_string artDestName(guid_artDestName, defaultArtDestName);
	cfg_bool artEmbedded(guid_artEmbedded, true);

	static constexpr GUID guid_saveLogs = { 0x0647f4e5, 0xe41f, 0x4c09, { 0x8f, 0x96, 0x52, 0x2a, 0x9d, 0x40, 0xfe, 0x9f } };
	cfg_bool saveLogs(guid_saveLogs, true);

	std::vector<SelectedPlaylist> getSelectedPlaylists() {
		std::vector<SelectedPlaylist> out;
		pfc::string8 all = playlists.get();
		const char* p = all.c_str();
		while (*p) {
			const char* eol = strchr(p, '\n');
			pfc::string8 line(p, eol ? (size_t)(eol - p) : strlen(p));
			p = eol ? eol + 1 : p + strlen(p);
			const char* line_c = line.c_str();
			const char* tab = strchr(line_c, '\t');
			if (!tab) continue;
			SelectedPlaylist sp;
			pfc::string8 guidStr(line_c, (size_t)(tab - line_c));
			sp.guid = pfc::GUID_from_text(guidStr);
			sp.name = tab + 1;
			out.push_back(std::move(sp));
		}
		return out;
	}

	void setSelectedPlaylists(const std::vector<SelectedPlaylist>& list) {
		pfc::string8 all;
		for (auto& sp : list) {
			all << pfc::print_guid(sp.guid) << "\t" << sp.name << "\n";
		}
		playlists.set(all);
	}

	pfc::string8 getPattern() {
		pfc::string8 p = pattern.get();
		if (p.is_empty()) p = defaultPattern;
		for (auto old : previousDefaultPatterns) if (p == old) p = defaultPattern;
		return p;
	}

	void showPreferences() {
		ui_control::get()->show_preferences(guidPreferencesPage);
	}
}
