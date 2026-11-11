/******************************************************************************
*
* Notepad4-TxtSyntax
*
* ConfigBackup.h
*   User settings snapshot collection and export UI.
*
******************************************************************************/
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

struct ConfigBackupOptions {
	bool includeHistory = false;
};

struct ConfigBackupResult {
	std::wstring path;
	std::string sha256;
	uint64_t size = 0;
	bool historyIncluded = false;
	std::vector<std::string> warnings;
};

// Creates a versioned settings snapshot from the application's actual config
// paths. This does not create or modify active user configuration.
bool ConfigBackup_Create(LPCWSTR destinationPath,
	const ConfigBackupOptions &options,
	ConfigBackupResult *result,
	std::wstring *error) noexcept;

// Stable SHA-256 over the sanitized settings, themes and matepath INI content.
bool ConfigBackup_ComputeFingerprint(bool includeHistory, std::string &fingerprint,
	std::wstring *error) noexcept;

// Small state files used by automatic backup deduplication.
bool ConfigBackup_ReadFingerprint(LPCWSTR path, std::string &fingerprint) noexcept;
bool ConfigBackup_WriteFingerprint(LPCWSTR path, const std::string &fingerprint,
	std::wstring *error) noexcept;

// Resolved from the actual filebrowser.exe setting. The first returns a path
// only when the file exists; the second returns the path used for a new file.
std::wstring ConfigBackup_GetMatepathIniPath() noexcept;
std::wstring ConfigBackup_GetMatepathIniTargetPath() noexcept;
std::string ConfigBackup_DeviceId() noexcept;

// Native export entry point used by the Settings menu.
void ConfigBackup_Export(HWND hwndOwner) noexcept;
