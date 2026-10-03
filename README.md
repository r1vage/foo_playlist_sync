# Playlist Sync (foo_playlist_sync)

One-way sync of foobar2000 playlists to a folder, USB stick or memory card, with a detailed report of every file that was added, replaced, removed or skipped.

A modern replacement for the abandoned 32-bit [foo_ows (One Way Sync)](https://www.foobar2000.org/components/view/foo_ows). Works with foobar2000 2.x, both 64-bit and 32-bit.

## Install

Download `foo_playlist_sync-<version>.fb2k-component` and double-click it, or use **Preferences › Components › Install…**. Then restart foobar2000.

## Use

1. **Preferences › Tools › Playlist Sync**: choose the destination folder, tick the playlists to sync, and adjust the options.
2. Click **Preview…** to see what would happen without changing anything. From the preview report you can click **Sync now** to go ahead.
3. Later syncs are available from **File › Playlist Sync › Sync now / Preview sync**. You can also put them on a toolbar button or a keyboard shortcut.

## Settings

| Setting | What it does |
|---|---|
| Destination | Where files are copied: a drive letter (`E:\`), a folder, a network share, or an MTP device (phone or player without a drive letter) as `mtp://Device/Storage/Folder`. **Browse ▾** lists the connected MTP devices. |
| File name pattern | Title formatting for the path below the destination. The file extension is added automatically. Characters that aren't allowed in file names are replaced. Default: `$if2(%album artist%,Unknown Artist)\$if2(%album%,Unknown Album)\%filename%` |
| **When the file already exists on the destination:** | |
| Replace if size or date differs | Copies again when the source file changed since the last sync. |
| Replace only if tags differ | Copies again only when tags changed. A file whose date changed but whose tags are the same is skipped. ReplayGain is never compared. *Ignore tags* lists more fields to leave out (separate with `;`, `*` is a wildcard). |
| ↳ Also replace re-encoded files | Also copies again when codec, bitrate, sample rate, channels or exact length changed. Use it to pick up re-encodes that kept the same tags. |
| Never replace | A file at the same path and name always counts as identical. |
| Keep original created and modified dates | Copied files get the source file's timestamps. When off, they get the time of the copy. |
| Remove files that are no longer in the playlists | Deletes destination files that the selected playlists no longer contain. Empty folders are removed too. |
| ↳ Only files copied by Playlist Sync | (Default, safer.) Only removes files that an earlier sync copied. When off, **every** other file in the destination folder is removed. |
| Album art: Copy cover to each album folder | Puts one cover image into every album folder on the destination. **Find** lists image names to look for next to the album's tracks, in priority order (default `folder; front; cover`, as .jpg/.jpeg/.png). **Else use embedded cover** falls back to the front cover embedded in the first track. **Save as** is the name on the destination (default `folder`); the image type is kept. Covers are compared, replaced and removed like tracks. |
| Create .nomedia file in the destination folder | Puts an empty `.nomedia` file in the destination folder. Android media scanners then skip the folder and everything below it. Note that apps relying on Android's media library won't see the music either; folder-based players still do. Playlist Sync never removes this file. |
| Write .m3u8 playlist files | Writes one UTF-8 playlist per synced playlist, with paths relative to the playlist file. *Linux-style* uses `/` and LF line endings for players that need them. |

## How it decides

- Each run compares every track of the selected playlists with the destination. A file that appears in several playlists, or several subsongs of one file, is copied once.
- A small hidden state file, `.playlist_sync`, in the destination folder remembers what was copied: source size and date, plus fingerprints of the tags and technical info. Because of this, "tags only" comparison is instant, and dates don't have to match exactly (FAT32 has 2-second timestamps and DST shifts).
  If the state file is missing (for example, a device you filled by hand), Playlist Sync reads the tags from the files on the destination once, and from then on uses the state file.
- Safety:
  - Copies go to a temporary `*.psync-tmp` file first and replace the old file only when complete. Pulling the cable mid-copy never leaves a half-written file under the real name.
  - Free space is checked before anything is changed.
  - A source file that is temporarily missing (for example, an unplugged library drive) never causes its copy to be deleted.
  - If none of the playlists' files could be synced, nothing is deleted.
- Files are removed first, to make room, and copied afterwards. Cancelling keeps everything consistent: the report lists what was done before you cancelled.

## Report

After every run a report window lists all actions. You can filter by Added / Replaced / Removed / Skipped / Errors / Playlists, and sort by any column. Double-click an entry to show the file in Explorer. **Export…** saves the report as `.txt` or `.csv`.
With **Save a log of each sync** on (the default), every run is also saved to `<foobar2000 profile>\playlist_sync_logs\`; the newest 100 logs are kept. Turning it off only stops the log files; the report window still appears after every run.

## MTP devices (phones)

Connect the phone, unlock it and choose **File transfer** (USB mode). Then pick it under **Browse ▾**. The destination defaults to the storage's `Music` folder; you can edit the path.

Differences from drive-letter destinations:
- Phones usually ignore file dates sent over MTP. Decisions therefore rely on the sync state file, and *Keep original dates* is a best-effort request to the device.
- There's no in-place tag reading over MTP. When a device has no sync state file yet (for example, you filled it by hand), files are compared by size once; after the first sync the state file is used.
- A replaced file is first uploaded under a temporary name. Only then is the old copy deleted and the new one renamed. On devices that don't allow renaming, the old copy is deleted first.
- Transfers are slower than USB mass storage. Large first syncs take a while; later syncs only copy what changed.

## Not supported yet
- Tracks from external `.cue` sheets (embedded cue sheets work)
- Several sync profiles (different devices with different settings)
- Transcoding (intentionally left out)

## Build

Requirements: Visual Studio 2022 with *Desktop development with C++*, plus the *C++ ATL* and *C++ MFC* components and a Windows SDK.
The foobar2000 SDK (2026-10-01) and WTL 10 headers are vendored in `sdk/` and `third_party/wtl/`.

```powershell
scripts\build.ps1          # x86 + x64 Release, runs tests, writes dist\*.fb2k-component
scripts\install-test.ps1   # copies the x64 build into a portable foobar2000 (close it first)
```

Layout:
- `src/sync_core.*`: the sync engine. It has no foobar2000 dependency and is unit-tested by `tests/core_tests.cpp`.
- `src/dest_fs.h`: the destination filesystem layer. `dest_local.cpp` handles folders and drives; `dest_mtp.cpp` handles MTP via Windows Portable Devices.
- `core_tests --mtp` lists the WPD devices; `core_tests --mtp-selftest mtp://Device/Storage/EmptyFolder` runs a write test on real hardware and cleans up afterwards.
- `src/sync_task.cpp`: collects tracks from playlists, computes the tag fingerprints and runs the engine in a background thread.
- `src/preferences.cpp`, `src/report_dialog.cpp`, `src/mainmenu.cpp`: the UI.
