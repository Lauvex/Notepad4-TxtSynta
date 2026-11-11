/******************************************************************************
*
* Notepad4-TxtSyntax
*
* ConfigAutoBackup.h
*   Local automatic settings snapshots and history management.
*
******************************************************************************/
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

#include "ConfigBackup.h"

struct ConfigAutoBackupSettings {
	bool enabled = true;
	int intervalHours = 24;
	int keepCount = 10;
	bool includeHistory = false;
};

struct ConfigAutoBackupEntry {
	std::wstring path;
	std::wstring fileName;
	uint64_t size = 0;
	FILETIME modifiedUtc {};
	bool historyIncluded = false;
};

// Global policy, loaded and saved through the main [Settings] section.
ConfigAutoBackupSettings &ConfigAutoBackup_Settings() noexcept;

// %LOCALAPPDATA%\Notepad4-TxtSyntax\Backups
std::wstring ConfigAutoBackup_GetDirectory() noexcept;
bool ConfigAutoBackup_EnsureDirectory(std::wstring *error) noexcept;

// Copies old LocalAppData automatic snapshots into the current Documents
// directory. Existing files are kept and the old copies are not deleted.
void ConfigAutoBackup_MigrateLegacyBackups() noexcept;

// Newest first; only regular files named Notepad4-Auto-*.zip are returned.
bool ConfigAutoBackup_List(std::vector<ConfigAutoBackupEntry> &entries,
	std::wstring *error) noexcept;

// Creates a snapshot unconditionally and applies the retention policy.
bool ConfigAutoBackup_CreateNow(ConfigBackupResult *result,
	std::wstring *error) noexcept;

// Creates a snapshot only when automatic backups are enabled and the newest
// snapshot is older than the configured interval. Reports whether one was made.
bool ConfigAutoBackup_MaybeRun(bool *created, std::wstring *error) noexcept;

// Deletes one file only when it is inside the automatic backup directory and
// still matches the automatic backup naming rule.
bool ConfigAutoBackup_Delete(LPCWSTR path, std::wstring *error) noexcept;

// Writes the current policy to the main INI without rewriting all settings.
void ConfigAutoBackup_SavePolicyToIni() noexcept;

// Native history dialog.
void ConfigAutoBackup_ShowHistory(HWND hwndOwner) noexcept;
