/******************************************************************************
*
* Notepad4-TxtSyntax
*
* ConfigRestore.h
*   Validated, selective and transactional settings restore.
*
******************************************************************************/
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

#include "ConfigArchive.h"

enum ConfigRestoreCategory : uint32_t {
	ConfigRestoreCategory_Basic = 1U << 0,
	ConfigRestoreCategory_Interface = 1U << 1,
	ConfigRestoreCategory_Themes = 1U << 2,
	ConfigRestoreCategory_Syntax = 1U << 3,
	ConfigRestoreCategory_History = 1U << 4,
	ConfigRestoreCategory_Matepath = 1U << 5,
};

constexpr uint32_t ConfigRestoreCategory_All =
	ConfigRestoreCategory_Basic | ConfigRestoreCategory_Interface
	| ConfigRestoreCategory_Themes | ConfigRestoreCategory_Syntax
	| ConfigRestoreCategory_History | ConfigRestoreCategory_Matepath;

constexpr uint32_t ConfigRestoreCategory_Default =
	ConfigRestoreCategory_Basic | ConfigRestoreCategory_Interface
	| ConfigRestoreCategory_Themes | ConfigRestoreCategory_Syntax
	| ConfigRestoreCategory_Matepath;

struct ConfigRestorePreviewFile {
	std::wstring fileName;
	int changed = 0;
	int added = 0;
	int removed = 0;
	bool wholeFile = false;
	std::vector<std::wstring> details;
};

struct ConfigRestorePreview {
	std::vector<ConfigRestorePreviewFile> files;
	int changed = 0;
	int added = 0;
	int removed = 0;
	bool hasChanges = false;
	bool legacy = false;
};

struct ConfigRestoreResult {
	std::wstring rollbackBackupPath;
	int filesApplied = 0;
	bool legacy = false;
};

bool ConfigRestore_Load(LPCWSTR archivePath,
	ConfigArchivePackage *package,
	std::wstring *error) noexcept;

bool ConfigRestore_BuildPreview(const ConfigArchivePackage &package,
	uint32_t categories,
	ConfigRestorePreview *preview,
	std::wstring *error) noexcept;

// Creates a rollback snapshot, stages all files, writes a transaction journal,
// atomically replaces the selected target files and verifies them. Any failure
// before the commit point restores the previous files.
bool ConfigRestore_Apply(const ConfigArchivePackage &package,
	uint32_t categories,
	LPCWSTR archivePath,
	ConfigRestoreResult *result,
	std::wstring *error) noexcept;

// Called before LoadSettings() on startup. Rolls back an interrupted restore.
void ConfigRestore_RecoverPendingTransaction() noexcept;

// Native restore dialog and restart handling.
void ConfigRestore_ShowDialog(HWND hwndOwner) noexcept;

// Opens the same dialog with a backup already selected. Returns true when a
// restore was applied and a restart has been scheduled.
bool ConfigRestore_ShowDialogForFile(HWND hwndOwner, LPCWSTR archivePath) noexcept;
