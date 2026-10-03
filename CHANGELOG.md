# Changelog

## 0.1.0
First release. A modern x64/x86 replacement for the abandoned foo_ows (One Way Sync).

**Sync**
- One-way sync of selected foobar2000 playlists to a folder, USB drive, memory card, network share or MTP device (phones and players without a drive letter, `mtp://Device/Storage/Folder`; **Browse** lists connected devices).
- File name pattern using title formatting. Default: `Album Artist\Album (Date)\NN - Title.ext`, with the disc number for multi-disc albums and "Unknown Artist"/"Unknown Album" fallbacks. The pattern may include the extension itself (e.g. `.$ext(%_path%)`).
- Choice of when existing files are replaced: size or date differs, tags differ (ReplayGain ignored, extra tags can be ignored, optionally also re-encodes), or never.
- Optional removal of files that are no longer in the playlists: only files copied by Playlist Sync (default), or everything inside the synced folder. Hidden files and folders (such as Android's `.thumbnails`) are never removed.
- A hidden sync record (`.playlist_sync`) on each destination makes comparisons fast and works with phones that don't keep file dates.

**Extras**
- Copy album art into each album folder: image files next to the tracks (`folder`, `front`, `cover` by default) or the embedded front cover, saved as `folder.jpg`/`.png` (name configurable).
- `.m3u8` playlist files with relative paths (Windows or Linux style).
- Optional `.nomedia` file in the destination folder.
- Optional preservation of created/modified dates.

**Reports and safety**
- Preview mode, and a report window after every run listing each file added, replaced, removed or skipped with the reason; filter, sort, double-click to show in Explorer, export to TXT/CSV.
- Optional log of each sync in the foobar2000 profile (on by default).
- Free-space check before anything changes, safe copies (temporary file, then rename) and a safety check against accidental mass deletion.
