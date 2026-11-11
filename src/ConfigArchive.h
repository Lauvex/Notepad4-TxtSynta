/******************************************************************************
*
* Notepad4-TxtSyntax
*
* ConfigArchive.h
*   Versioned settings archive writer and archive format inspection.
*
******************************************************************************/
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

// Bump when the archive layout or manifest fields change incompatibly.
constexpr int ConfigArchive_FormatVersion = 1;
constexpr int ConfigArchive_MinimumReaderVersion = 1;

enum class ConfigArchiveFormat {
	Unknown = 0,
	LegacySixIni,
	Current,
};

struct ConfigArchiveEntry {
	std::wstring sourcePath;
	std::string archivePath;	// UTF-8, forward slash separated, relative to archive root.
	std::string category;		// UTF-8 manifest category.
	bool hasInlineData = false;
	std::vector<uint8_t> inlineData;
};

struct ConfigArchiveMetadata {
	std::string appVersion;
	std::string buildId;
	std::string createdAtUtc;
	std::string deviceId;
	std::string backupId;
	bool includeHistory = false;
	std::vector<std::string> categories;
	std::vector<std::string> warnings;
};

struct ConfigArchiveCreateResult {
	std::string zipSha256;
	uint64_t zipSize = 0;
};

struct ConfigArchiveInspection {
	ConfigArchiveFormat format = ConfigArchiveFormat::Unknown;
	bool hasManifest = false;
	bool hasConfigDirectory = false;
	bool hasMatepathConfig = false;
	std::vector<std::string> entryNames;
	std::string limitation;
};

struct ConfigArchiveFile {
	std::string path;
	std::string category;
	uint64_t size = 0;
	std::string sha256;
	std::vector<uint8_t> data;
};

struct ConfigArchivePackage {
	ConfigArchiveFormat format = ConfigArchiveFormat::Unknown;
	std::string appVersion;
	std::string buildId;
	std::string createdAtUtc;
	std::string deviceId;
	std::string backupId;
	bool historyIncluded = false;
	std::vector<std::string> categories;
	std::vector<std::string> warnings;
	std::vector<ConfigArchiveFile> files;
	std::string limitation;
};

// Creates a versioned ZIP containing manifest.json and the supplied files.
// The archive is first written to a temporary file and then atomically replaced.
bool ConfigArchive_Create(const std::vector<ConfigArchiveEntry> &entries,
	const ConfigArchiveMetadata &metadata,
	LPCWSTR destinationPath,
	ConfigArchiveCreateResult *result,
	std::wstring *error) noexcept;

// Inspects the ZIP central directory without extracting file contents.
// LegacySixIni identifies the previous six-root-INI export format.
bool ConfigArchive_Inspect(LPCWSTR archivePath,
	ConfigArchiveInspection *inspection,
	std::wstring *error) noexcept;

// Reads, validates and extracts a settings archive into memory. Current
// archives require a valid manifest with matching sizes and SHA-256 values.
// Legacy six-INI archives are accepted with CRC checks and a limitation marker.
bool ConfigArchive_Read(LPCWSTR archivePath,
	ConfigArchivePackage *package,
	std::wstring *error) noexcept;
