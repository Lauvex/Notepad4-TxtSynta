/******************************************************************************
*
* Notepad4-TxtSyntax
*
* ConfigAutoBackup.cpp
*   Local automatic settings snapshots, retention and history storage.
*
******************************************************************************/

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>

#include "SciCall.h"
#include "config.h"
#include "Helpers.h"
#include "Notepad4.h"
#include "ConfigBackup.h"
#include "ConfigAutoBackup.h"

namespace {

constexpr wchar_t BackupSubdirectory[] = L"Backups";
constexpr wchar_t BackupPrefix[] = L"Notepad4-Auto-";
constexpr wchar_t BackupSuffix[] = L".zip";
constexpr wchar_t FingerprintFileName[] = L".auto-fingerprint";
constexpr int MaxBackupFiles = 500;
constexpr int MinIntervalHours = 1;
constexpr int MaxIntervalHours = 24 * 30;
constexpr int MinKeepCount = 1;
constexpr int MaxKeepCount = 50;

ConfigAutoBackupSettings g_AutoBackupSettings;

void SetError(std::wstring *error, const wchar_t *message) noexcept {
	if (error != nullptr) {
		*error = message;
	}
}

bool IsRegularFile(LPCWSTR path) noexcept {
	const DWORD attributes = GetFileAttributesW(path);
	return attributes != INVALID_FILE_ATTRIBUTES
		&& (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

bool EndsWithNoCase(LPCWSTR text, LPCWSTR suffix) noexcept {
	const size_t textLength = wcslen(text);
	const size_t suffixLength = wcslen(suffix);
	return textLength >= suffixLength
		&& _wcsicmp(text + textLength - suffixLength, suffix) == 0;
}

bool IsAutoBackupName(LPCWSTR name) noexcept {
	if (name == nullptr || *name == L'\0') {
		return false;
	}
	if (_wcsnicmp(name, BackupPrefix, ARRAYSIZE(BackupPrefix) - 1) != 0) {
		return false;
	}
	if (!EndsWithNoCase(name, BackupSuffix)) {
		return false;
	}
	return PathMatchSpecW(name, L"Notepad4-Auto-*.zip") != FALSE;
}

std::wstring MakeLocalFileNameTimestamp() noexcept {
	SYSTEMTIME time {};
	GetLocalTime(&time);
	wchar_t buffer[32];
	swprintf_s(buffer, L"%04u%02u%02u-%02u%02u%02u",
		time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
	return buffer;
}

bool BuildUniquePath(const std::wstring &directory, bool includeHistory,
	std::wstring &path, std::wstring *error) noexcept {
	const std::wstring stamp = MakeLocalFileNameTimestamp();
	std::wstring base = directory + L"\\" + BackupPrefix + stamp;
	if (includeHistory) {
		base += L"-h";
	}
	path = base + BackupSuffix;
	if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
		return true;
	}
	for (int index = 1; index < 1000; ++index) {
		wchar_t suffix[32];
		swprintf_s(suffix, L"-%d", index);
		path = base + suffix + BackupSuffix;
		if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
			return true;
		}
	}
	SetError(error, L"Cannot choose a unique automatic backup file name.");
	return false;
}

std::wstring GetBackupMutexName() noexcept {
	std::wstring key = szIniFile;
	if (key.empty()) {
		key = GetNotepad4DataDirectory();
	}
	uint32_t hash = 2166136261U;
	for (wchar_t ch : key) {
		hash ^= static_cast<uint32_t>(ch);
		hash *= 16777619U;
	}
	wchar_t name[128];
	swprintf_s(name, L"Local\\Notepad4-TxtSyntax.AutoBackup.%08X", hash);
	return name;
}

class AutoBackupLock {
public:
	AutoBackupLock() noexcept {
		const std::wstring name = GetBackupMutexName();
		handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
		if (handle_ == nullptr) {
			return;
		}
		if (WaitForSingleObject(handle_, 0) != WAIT_OBJECT_0) {
			CloseHandle(handle_);
			handle_ = nullptr;
		}
	}

	~AutoBackupLock() noexcept {
		if (handle_ != nullptr) {
			ReleaseMutex(handle_);
			CloseHandle(handle_);
		}
	}

	bool Acquired() const noexcept {
		return handle_ != nullptr;
	}

private:
	HANDLE handle_ = nullptr;
};

bool ApplyRetention() noexcept {
	std::vector<ConfigAutoBackupEntry> entries;
	if (!ConfigAutoBackup_List(entries, nullptr)) {
		return false;
	}
	const int keep = std::clamp(ConfigAutoBackup_Settings().keepCount, MinKeepCount, MaxKeepCount);
	for (size_t i = static_cast<size_t>(keep); i < entries.size(); ++i) {
		DeleteFileW(entries[i].path.c_str());
	}
	return true;
}

bool CreateInternal(ConfigBackupResult *result, std::wstring *error) noexcept {
	if (szIniFile[0] == L'\0' || !IsRegularFile(szIniFile)) {
		SetError(error, L"The saved main configuration file is not available. Press F7 to save settings first.");
		return false;
	}
	if (!ConfigAutoBackup_EnsureDirectory(error)) {
		return false;
	}

	const std::wstring directory = ConfigAutoBackup_GetDirectory();
	if (directory.empty()) {
		SetError(error, L"Cannot resolve the local application data directory.");
		return false;
	}

	const ConfigAutoBackupSettings &settings = ConfigAutoBackup_Settings();
	if (settings.includeHistory) {
		SaveHistoryForBackup();
	}

	std::wstring path;
	if (!BuildUniquePath(directory, settings.includeHistory, path, error)) {
		return false;
	}

	ConfigBackupOptions options;
	options.includeHistory = settings.includeHistory;
	ConfigBackupResult localResult;
	ConfigBackupResult *out = result != nullptr ? result : &localResult;
	if (!ConfigBackup_Create(path.c_str(), options, out, error)) {
		return false;
	}
	ApplyRetention();
	return true;
}

} // namespace

ConfigAutoBackupSettings &ConfigAutoBackup_Settings() noexcept {
	return g_AutoBackupSettings;
}

std::wstring ConfigAutoBackup_GetDirectory() noexcept {
	const std::wstring dataDirectory = GetNotepad4DataDirectory();
	if (dataDirectory.empty()) {
		return {};
	}
	return dataDirectory + L"\\" + BackupSubdirectory;
}

void ConfigAutoBackup_MigrateLegacyBackups() noexcept {
	const std::wstring destinationDirectory = ConfigAutoBackup_GetDirectory();
	const std::wstring privateDirectory = GetNotepad4PrivateDataDirectory();
	if (destinationDirectory.empty() || privateDirectory.empty()
		|| _wcsicmp(destinationDirectory.c_str(),
			(privateDirectory + L"\\" + BackupSubdirectory).c_str()) == 0) {
		return;
	}
	const std::wstring sourceDirectory = privateDirectory + L"\\" + BackupSubdirectory;
	if (!PathIsDirectoryW(sourceDirectory.c_str())) {
		return;
	}
	if (!ConfigAutoBackup_EnsureDirectory(nullptr)) {
		return;
	}
	const std::wstring pattern = sourceDirectory + L"\\*" + BackupSuffix;
	WIN32_FIND_DATAW data {};
	HANDLE find = FindFirstFileW(pattern.c_str(), &data);
	if (find == INVALID_HANDLE_VALUE) {
		return;
	}
	do {
		if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
			continue;
		}
		const std::wstring destination = destinationDirectory + L"\\" + data.cFileName;
		if (GetFileAttributesW(destination.c_str()) == INVALID_FILE_ATTRIBUTES) {
			CopyFileW((sourceDirectory + L"\\" + data.cFileName).c_str(),
				destination.c_str(), TRUE);
		}
	} while (FindNextFileW(find, &data));
	FindClose(find);
}

bool ConfigAutoBackup_EnsureDirectory(std::wstring *error) noexcept {
	const std::wstring directory = ConfigAutoBackup_GetDirectory();
	if (directory.empty()) {
		SetError(error, L"Cannot resolve the local application data directory.");
		return false;
	}
	const int result = SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
	if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS) {
		SetError(error, L"Cannot create the local backup directory.");
		return false;
	}
	if (!PathIsDirectoryW(directory.c_str())) {
		SetError(error, L"The local backup path is not a directory.");
		return false;
	}
	return true;
}

bool ConfigAutoBackup_List(std::vector<ConfigAutoBackupEntry> &entries,
	std::wstring *error) noexcept {
	entries.clear();
	const std::wstring directory = ConfigAutoBackup_GetDirectory();
	if (directory.empty()) {
		SetError(error, L"Cannot resolve the local application data directory.");
		return false;
	}
	if (!PathIsDirectoryW(directory.c_str())) {
		return true;
	}

	const std::wstring pattern = directory + L"\\" + BackupPrefix + L"*.zip";
	WIN32_FIND_DATAW findData {};
	HANDLE find = FindFirstFileW(pattern.c_str(), &findData);
	if (find == INVALID_HANDLE_VALUE) {
		return true;
	}

	try {
		do {
			if ((findData.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
				continue;
			}
			if (!IsAutoBackupName(findData.cFileName)) {
				continue;
			}
			ConfigAutoBackupEntry entry;
			entry.path = directory + L"\\" + findData.cFileName;
			entry.fileName = findData.cFileName;
			entry.size = (static_cast<uint64_t>(findData.nFileSizeHigh) << 32) | findData.nFileSizeLow;
			entry.modifiedUtc = findData.ftLastWriteTime;
			entry.historyIncluded = EndsWithNoCase(findData.cFileName, L"-h.zip");
			entries.push_back(std::move(entry));
		} while (FindNextFileW(find, &findData));
	} catch (...) {
		FindClose(find);
		SetError(error, L"Not enough memory to read the local backup history.");
		return false;
	}
	FindClose(find);

	std::sort(entries.begin(), entries.end(), [](const ConfigAutoBackupEntry &a,
		const ConfigAutoBackupEntry &b) noexcept {
		const int compare = CompareFileTime(&a.modifiedUtc, &b.modifiedUtc);
		if (compare != 0) {
			return compare > 0;
		}
		return _wcsicmp(a.fileName.c_str(), b.fileName.c_str()) > 0;
	});
	if (entries.size() > MaxBackupFiles) {
		entries.resize(MaxBackupFiles);
	}
	return true;
}

bool ConfigAutoBackup_CreateNow(ConfigBackupResult *result,
	std::wstring *error) noexcept {
	if (error != nullptr) {
		error->clear();
	}
	if (result != nullptr) {
		*result = {};
	}
	AutoBackupLock lock;
	if (!lock.Acquired()) {
		SetError(error, L"Another automatic backup is already running.");
		return false;
	}
	return CreateInternal(result, error);
}

bool ConfigAutoBackup_MaybeRun(bool *created, std::wstring *error) noexcept {
	if (created != nullptr) {
		*created = false;
	}
	if (!ConfigAutoBackup_Settings().enabled) {
		return true;
	}

	AutoBackupLock lock;
	if (!lock.Acquired()) {
		return true;
	}

	std::vector<ConfigAutoBackupEntry> entries;
	if (!ConfigAutoBackup_List(entries, error)) {
		return false;
	}
	std::string fingerprint;
	const std::wstring directory = ConfigAutoBackup_GetDirectory();
	const bool hasFingerprint = !directory.empty()
		&& ConfigBackup_ComputeFingerprint(ConfigAutoBackup_Settings().includeHistory,
			fingerprint, nullptr);
	const std::wstring fingerprintPath = directory + L"\\" + FingerprintFileName;
	bool fingerprintChanged = false;
	if (hasFingerprint && !entries.empty()) {
		std::string lastFingerprint;
		if (ConfigBackup_ReadFingerprint(fingerprintPath.c_str(), lastFingerprint)) {
			if (lastFingerprint == fingerprint) {
				return true;
			}
			fingerprintChanged = true;
		} else {
			fingerprintChanged = true;
		}
	}
	if (!entries.empty() && !fingerprintChanged) {
		ULARGE_INTEGER newest {};
		newest.LowPart = entries.front().modifiedUtc.dwLowDateTime;
		newest.HighPart = entries.front().modifiedUtc.dwHighDateTime;
		const int hours = std::clamp(ConfigAutoBackup_Settings().intervalHours,
			MinIntervalHours, MaxIntervalHours);
		const uint64_t interval = static_cast<uint64_t>(hours) * 3600ULL * 10000000ULL;
		FILETIME nowFileTime {};
		GetSystemTimeAsFileTime(&nowFileTime);
		ULARGE_INTEGER now {};
		now.LowPart = nowFileTime.dwLowDateTime;
		now.HighPart = nowFileTime.dwHighDateTime;
		if (newest.QuadPart + interval > now.QuadPart) {
			return true;
		}
	}

	if (!CreateInternal(nullptr, error)) {
		return false;
	}
	if (hasFingerprint) {
		std::wstring stateError;
		if (!ConfigBackup_WriteFingerprint(fingerprintPath.c_str(), fingerprint, &stateError)) {
			// A missing state file only causes a future duplicate backup; the
			// snapshot itself has already been created successfully.
		}
	}
	if (created != nullptr) {
		*created = true;
	}
	return true;
}

bool ConfigAutoBackup_Delete(LPCWSTR path, std::wstring *error) noexcept {
	if (path == nullptr || *path == L'\0') {
		SetError(error, L"No local backup was selected.");
		return false;
	}
	const std::wstring directory = ConfigAutoBackup_GetDirectory();
	if (directory.empty()) {
		SetError(error, L"Cannot resolve the local application data directory.");
		return false;
	}

	WCHAR parent[MAX_PATH];
	lstrcpynW(parent, path, ARRAYSIZE(parent));
	PathRemoveFileSpecW(parent);
	if (_wcsicmp(parent, directory.c_str()) != 0) {
		SetError(error, L"The selected file is outside the local backup directory.");
		return false;
	}
	const wchar_t *name = PathFindFileNameW(path);
	if (!IsAutoBackupName(name) || !IsRegularFile(path)) {
		SetError(error, L"The selected file is not a local automatic backup.");
		return false;
	}

	AutoBackupLock lock;
	if (!lock.Acquired()) {
		SetError(error, L"Another automatic backup is already running.");
		return false;
	}
	if (!DeleteFileW(path)) {
		SetError(error, L"Cannot delete the selected local backup.");
		return false;
	}
	return true;
}

void ConfigAutoBackup_SavePolicyToIni() noexcept {
	if (szIniFile[0] == L'\0') {
		return;
	}
	ConfigAutoBackupSettings &settings = ConfigAutoBackup_Settings();
	settings.intervalHours = std::clamp(settings.intervalHours, MinIntervalHours, MaxIntervalHours);
	settings.keepCount = std::clamp(settings.keepCount, MinKeepCount, MaxKeepCount);
	IniSetBoolEx(INI_SECTION_NAME_SETTINGS, L"AutoBackup", settings.enabled, true);
	IniSetIntEx(INI_SECTION_NAME_SETTINGS, L"AutoBackupIntervalHours", settings.intervalHours, 24);
	IniSetIntEx(INI_SECTION_NAME_SETTINGS, L"AutoBackupKeep", settings.keepCount, 10);
	IniSetBoolEx(INI_SECTION_NAME_SETTINGS, L"AutoBackupHistory", settings.includeHistory, false);
}
