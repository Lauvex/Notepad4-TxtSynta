/******************************************************************************
*
* Notepad4-TxtSyntax
*
* ConfigCloud.h
*   Encrypted WebDAV settings backup and restore.
*
******************************************************************************/
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

struct ConfigCloudSettings {
	std::wstring url;
	std::wstring userName;
	std::wstring remoteFolder;
	bool saveCredentials = true;
};

struct ConfigCloudCredentials {
	std::wstring password;
	std::wstring passphrase;
};

struct ConfigCloudRemoteEntry {
	std::wstring name;
	uint64_t size = 0;
	std::wstring modified;
};

struct ConfigCloudUploadResult {
	std::wstring remoteName;
	uint64_t encryptedSize = 0;
};

// Policy and non-secret settings; credentials are stored separately with DPAPI.
ConfigCloudSettings &ConfigCloud_Settings() noexcept;
void ConfigCloud_SaveSettingsToIni() noexcept;

bool ConfigCloud_LoadCredentials(ConfigCloudCredentials *credentials,
	std::wstring *error) noexcept;
bool ConfigCloud_SaveCredentials(const ConfigCloudCredentials &credentials,
	std::wstring *error) noexcept;
void ConfigCloud_DeleteCredentials() noexcept;

// Removes stale temporary archives created by this application. Only files
// with the NP4C prefix and a known backup suffix are considered.
void ConfigCloud_CleanupTemporaryFiles() noexcept;

// WebDAV operations. All payloads are encrypted before they leave the process.
bool ConfigCloud_TestConnection(const ConfigCloudSettings &settings,
	const ConfigCloudCredentials &credentials, std::wstring *error) noexcept;

bool ConfigCloud_UploadCurrent(const ConfigCloudSettings &settings,
	const ConfigCloudCredentials &credentials, bool includeHistory,
	ConfigCloudUploadResult *result, std::wstring *error) noexcept;

// Automatic snapshots use a distinct remote name prefix so automatic retention
// never removes a manually uploaded backup.
bool ConfigCloud_UploadAutomatic(const ConfigCloudSettings &settings,
	const ConfigCloudCredentials &credentials, bool includeHistory,
	ConfigCloudUploadResult *result, std::wstring *error) noexcept;

bool ConfigCloud_DeleteRemote(const ConfigCloudSettings &settings,
	const ConfigCloudCredentials &credentials, LPCWSTR remoteName,
	std::wstring *error) noexcept;

bool ConfigCloud_ListRemote(const ConfigCloudSettings &settings,
	const ConfigCloudCredentials &credentials,
	std::vector<ConfigCloudRemoteEntry> &entries, std::wstring *error) noexcept;

// Downloads and decrypts the newest remote snapshot to a temporary ZIP.
// The caller owns the returned file and must delete it.
bool ConfigCloud_DownloadLatest(const ConfigCloudSettings &settings,
	const ConfigCloudCredentials &credentials, std::wstring &localZipPath,
	std::wstring &remoteName, std::wstring *error) noexcept;

// Downloads and decrypts a selected remote snapshot. remoteName must be one of
// the names returned by ConfigCloud_ListRemote.
bool ConfigCloud_DownloadRemote(const ConfigCloudSettings &settings,
	const ConfigCloudCredentials &credentials, LPCWSTR remoteName,
	std::wstring &localZipPath, std::wstring *error) noexcept;

// Native dialog.
void ConfigCloud_ShowDialog(HWND hwndOwner) noexcept;
