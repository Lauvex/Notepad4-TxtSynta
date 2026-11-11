/******************************************************************************
*
* Notepad4-TxtSyntax
*
* ConfigBackup.cpp
*   User settings snapshot collection and export UI.
*
******************************************************************************/

#include <windows.h>
#include <windowsx.h>
#include <bcrypt.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "SciCall.h"
#include "config.h"
#include "Helpers.h"
#include "Notepad4.h"
#include "Version.h"
#include "ConfigArchive.h"
#include "ConfigBackup.h"
#include "resource.h"

namespace {

constexpr uint64_t ConfigBackup_MaxIniFileSize = 16ULL * 1024 * 1024;
constexpr wchar_t DeviceIdFileName[] = L"device-id.txt";

constexpr const wchar_t *ThemeNames[] = {
	L"Notepad4 DarkTheme.ini",
	L"Notepad4 Solarized Light.ini",
	L"Notepad4 Tango Warm Dark.ini",
	L"Notepad4 Gruvbox Dark.ini",
	L"Notepad4 Gruvbox Light.ini",
};

void SetError(std::wstring *error, const wchar_t *message) noexcept {
	if (error != nullptr) {
		*error = message;
	}
}

void SetWin32Error(std::wstring *error, const wchar_t *message, DWORD code) noexcept {
	if (error != nullptr) {
		wchar_t buffer[512];
		swprintf_s(buffer, L"%s (Windows error %lu).", message, code);
		*error = buffer;
	}
}

std::string Narrow(const std::wstring &text) noexcept {
	if (text.empty()) {
		return {};
	}
	const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	if (length <= 0) {
		return {};
	}
	std::string result(static_cast<size_t>(length), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
	return result;
}

std::wstring Widen(const std::string &text) noexcept {
	if (text.empty()) {
		return {};
	}
	const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
	if (length <= 0) {
		return {};
	}
	std::wstring result(static_cast<size_t>(length), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), length);
	return result;
}

void LoadStringResource(UINT id, LPCWSTR fallback, LPWSTR buffer, int capacity) noexcept {
	if (capacity <= 0) {
		return;
	}
	if (LoadStringW(g_hInstance, id, buffer, capacity) <= 0) {
		lstrcpynW(buffer, fallback, capacity);
	}
}

bool GetUtcTimestamp(std::wstring &timestamp) noexcept {
	SYSTEMTIME time {};
	GetSystemTime(&time);
	wchar_t buffer[32];
	swprintf_s(buffer, L"%04u-%02u-%02uT%02u:%02u:%02uZ",
		time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
	timestamp = buffer;
	return true;
}

std::wstring GetLocalFileNameTimestamp() noexcept {
	SYSTEMTIME time {};
	GetLocalTime(&time);
	wchar_t buffer[32];
	swprintf_s(buffer, L"%04u%02u%02u-%02u%02u%02u",
		time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
	return buffer;
}

bool IsUuidText(const std::wstring &text) noexcept {
	if (text.size() != 36) {
		return false;
	}
	for (size_t i = 0; i < text.size(); ++i) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (text[i] != L'-') {
				return false;
			}
		} else if (!((text[i] >= L'0' && text[i] <= L'9')
			|| (text[i] >= L'a' && text[i] <= L'f')
			|| (text[i] >= L'A' && text[i] <= L'F'))) {
			return false;
		}
	}
	return true;
}

bool NewGuidString(std::wstring &text) noexcept {
	GUID guid {};
	if (FAILED(CoCreateGuid(&guid))) {
		return false;
	}
	wchar_t buffer[48];
	if (StringFromGUID2(guid, buffer, ARRAYSIZE(buffer)) <= 0) {
		return false;
	}
	const size_t length = wcslen(buffer);
	if (length < 3 || buffer[0] != L'{' || buffer[length - 1] != L'}') {
		return false;
	}
	text.assign(buffer + 1, length - 2);
	return true;
}

bool ReadFileBytes(LPCWSTR path, std::vector<uint8_t> &data, std::wstring *error) noexcept {
	data.clear();
	const DWORD attributes = GetFileAttributesW(path);
	if (attributes == INVALID_FILE_ATTRIBUTES) {
		SetError(error, L"Cannot find a configuration file.");
		return false;
	}
	if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
		SetError(error, L"A configuration path is not a regular file.");
		return false;
	}

	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		SetError(error, L"Cannot open a configuration file.");
		return false;
	}
	LARGE_INTEGER size {};
	if (!GetFileSizeEx(file, &size) || size.QuadPart < 0
		|| static_cast<uint64_t>(size.QuadPart) > ConfigBackup_MaxIniFileSize) {
		CloseHandle(file);
		SetError(error, L"The configuration file is too large.");
		return false;
	}
	data.resize(static_cast<size_t>(size.QuadPart));
	size_t offset = 0;
	while (offset < data.size()) {
		const DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - offset, 1U << 20));
		DWORD read = 0;
		if (!ReadFile(file, data.data() + offset, chunk, &read, nullptr) || read == 0) {
			CloseHandle(file);
			data.clear();
			SetError(error, L"Cannot read a configuration file.");
			return false;
		}
		offset += read;
	}
	CloseHandle(file);
	return true;
}

std::wstring TrimCopy(const std::wstring &text) noexcept {
	size_t first = 0;
	while (first < text.size() && iswspace(text[first])) {
		++first;
	}
	size_t last = text.size();
	while (last > first && iswspace(text[last - 1])) {
		--last;
	}
	return text.substr(first, last - first);
}

bool IsSectionName(const std::wstring &name, const wchar_t *candidate) noexcept {
	return _wcsicmp(name.c_str(), candidate) == 0;
}

bool IsNotepadHistorySection(const std::wstring &name) noexcept {
	return IsSectionName(name, L"Recent Files")
		|| IsSectionName(name, L"Recent Find")
		|| IsSectionName(name, L"Recent Replace");
}

bool IsMatepathHistorySection(const std::wstring &name) noexcept {
	return IsSectionName(name, L"Copy/Move MRU");
}

bool SanitizeIniHistory(const std::vector<uint8_t> &input, bool matepath, std::vector<uint8_t> &output, std::wstring *error) noexcept {
	if (input.size() < 2 || input[0] != 0xFF || input[1] != 0xFE || (input.size() & 1) != 0) {
		SetError(error, L"Cannot remove history because a configuration file is not UTF-16LE with BOM.");
		return false;
	}
	const size_t characterCount = (input.size() - 2) / sizeof(wchar_t);
	std::wstring text(characterCount, L'\0');
	memcpy(text.data(), input.data() + 2, characterCount * sizeof(wchar_t));
	if (std::find(text.begin(), text.end(), L'\0') != text.end()) {
		SetError(error, L"Cannot parse a configuration file while removing history.");
		return false;
	}

	std::wstring filtered;
	filtered.reserve(text.size());
	bool skipSection = false;
	size_t start = 0;
	while (start <= text.size()) {
		const size_t end = text.find(L'\n', start);
		std::wstring line = text.substr(start, (end == std::wstring::npos ? text.size() : end) - start);
		if (!line.empty() && line.back() == L'\r') {
			line.pop_back();
		}

		const std::wstring trimmed = TrimCopy(line);
		if (trimmed.size() >= 2 && trimmed.front() == L'[' && trimmed.back() == L']') {
			const std::wstring sectionName = TrimCopy(trimmed.substr(1, trimmed.size() - 2));
			skipSection = matepath ? IsMatepathHistorySection(sectionName) : IsNotepadHistorySection(sectionName);
		}

		if (!skipSection) {
			if (matepath && _wcsnicmp(trimmed.c_str(), L"MRUDirectory=", 13) == 0) {
				// Remove the last visited directory without dropping other matepath settings.
			} else {
				filtered += line;
				filtered += L"\r\n";
			}
		}
		if (end == std::wstring::npos) {
			break;
		}
		start = end + 1;
	}

	output.clear();
	output.reserve(2 + filtered.size() * sizeof(wchar_t));
	output.push_back(0xFF);
	output.push_back(0xFE);
	const size_t byteCount = filtered.size() * sizeof(wchar_t);
	const size_t oldSize = output.size();
	output.resize(oldSize + byteCount);
	memcpy(output.data() + oldSize, filtered.data(), byteCount);
	return true;
}

bool FileExists(LPCWSTR path) noexcept {
	const DWORD attributes = GetFileAttributesW(path);
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring GetLocalAppDataNotepadDirectory() noexcept {
	PWSTR localAppData = nullptr;
	if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppData))) {
		return {};
	}
	std::wstring path = localAppData;
	CoTaskMemFree(localAppData);
	path += L"\\Notepad4";
	return path;
}

bool StartsWithNoCase(LPCWSTR text, LPCWSTR prefix) noexcept {
	return _wcsnicmp(text, prefix, lstrlenW(prefix)) == 0;
}

void ExtractFirstToken(LPCWSTR command, LPWSTR token, int capacity) noexcept {
	if (capacity <= 0) {
		return;
	}
	token[0] = L'\0';
	if (command == nullptr) {
		return;
	}
	while (*command == L' ' || *command == L'\t' || *command == L'"') {
		++command;
	}
	int length = 0;
	while (command[length] != L'\0' && command[length] != L' ' && command[length] != L'\t'
		&& command[length] != L'"' && length + 1 < capacity) {
		token[length] = command[length];
		++length;
	}
	token[length] = L'\0';
}

void PrepareFilterString(LPWSTR filter) noexcept {
	if (filter == nullptr) {
		return;
	}
	for (LPWSTR p = filter; *p; ++p) {
		if (*p == L'|') {
			*p = L'\0';
		}
	}
}

std::wstring GetMatepathIniPathInternal(bool mustExist) noexcept {
	WCHAR notepadDir[MAX_PATH];
	lstrcpynW(notepadDir, szExeRealPath, ARRAYSIZE(notepadDir));
	PathRemoveFileSpecW(notepadDir);

	WCHAR browserCommand[MAX_PATH] = L"";
	IniGetString(INI_SECTION_NAME_FLAGS, L"filebrowser.exe", L"", browserCommand, ARRAYSIZE(browserCommand));
	WCHAR browserPath[MAX_PATH] = L"matepath.exe";
	if (browserCommand[0] != L'\0') {
		ExtractFirstToken(browserCommand, browserPath, ARRAYSIZE(browserPath));
	}
	if (browserPath[0] == L'\0') {
		lstrcpynW(browserPath, L"matepath.exe", ARRAYSIZE(browserPath));
	}
	if (PathIsRelativeW(browserPath)) {
		WCHAR resolved[MAX_PATH];
		lstrcpynW(resolved, notepadDir, ARRAYSIZE(resolved));
		if (PathAppendW(resolved, browserPath) && FileExists(resolved)) {
			lstrcpynW(browserPath, resolved, ARRAYSIZE(browserPath));
		}
	}

	WCHAR browserDir[MAX_PATH];
	lstrcpynW(browserDir, browserPath, ARRAYSIZE(browserDir));
	PathRemoveFileSpecW(browserDir);

	const bool nonPortable = StartsWithNoCase(browserPath, L"C:\\Program Files")
		|| StrStrIW(browserPath, L"WinGet") != nullptr
		|| StrStrIW(browserPath, L"hocolatey") != nullptr;
	const std::wstring appData = GetLocalAppDataNotepadDirectory();

	std::wstring candidate;
	if (nonPortable && !appData.empty()) {
		candidate = appData + L"\\matepath.ini";
		if (FileExists(candidate.c_str())) {
			return candidate;
		}
	}

	candidate = browserDir;
	candidate += L"\\matepath.ini";
	if (PathIsDirectoryW(browserDir) && FileExists(candidate.c_str())) {
		return candidate;
	}

	candidate = notepadDir;
	candidate += L"\\matepath.ini";
	if (FileExists(candidate.c_str())) {
		return candidate;
	}

	if (!appData.empty()) {
		candidate = appData + L"\\matepath.ini";
		if (FileExists(candidate.c_str())) {
			return candidate;
		}
	}

	if (!mustExist) {
		if (nonPortable && !appData.empty()) {
			return appData + L"\\matepath.ini";
		}
		if (PathIsDirectoryW(browserDir)) {
			return std::wstring(browserDir) + L"\\matepath.ini";
		}
		return std::wstring(notepadDir) + L"\\matepath.ini";
	}
	return {};
}

std::string GetDeviceId(std::vector<std::string> &warnings) noexcept {
	std::wstring deviceId;
	std::wstring directory = GetNotepad4PrivateDataDirectory();
	if (directory.empty()) {
		directory = GetNotepad4DataDirectory();
	}
	if (directory.empty()) {
		warnings.push_back("device-id-not-persisted");
		NewGuidString(deviceId);
		return Narrow(deviceId);
	}
	if (!PathIsDirectoryW(directory.c_str())) {
		SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
	}
	if (!PathIsDirectoryW(directory.c_str())) {
		warnings.push_back("device-id-not-persisted");
		NewGuidString(deviceId);
		return Narrow(deviceId);
	}

	const std::wstring privatePath = directory + L"\\" + DeviceIdFileName;
	std::wstring path = privatePath;
	bool legacy = false;
	if (!FileExists(path.c_str())) {
		const std::wstring legacyRoot = GetNotepad4DataDirectory();
		if (!legacyRoot.empty()) {
			const std::wstring legacyPath = legacyRoot + L"\\" + DeviceIdFileName;
			if (FileExists(legacyPath.c_str())) {
				path = legacyPath;
				legacy = true;
			}
		}
	}
	HANDLE existing = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (existing != INVALID_HANDLE_VALUE) {
		char buffer[128] {};
		DWORD read = 0;
		const bool ok = ReadFile(existing, buffer, sizeof(buffer) - 1, &read, nullptr) != 0;
		CloseHandle(existing);
		if (ok && read > 0) {
			std::wstring text = Widen(std::string(buffer, read));
			text = TrimCopy(text);
			if (IsUuidText(text)) {
				if (legacy) {
					CopyFileW(path.c_str(), privatePath.c_str(), TRUE);
				}
				return Narrow(text);
			}
		}
	}

	if (!NewGuidString(deviceId)) {
		warnings.push_back("device-id-not-persisted");
		return {};
	}
	const std::string value = Narrow(deviceId);

	WCHAR tempPath[MAX_PATH];
	if (!GetTempFileNameW(directory.c_str(), L"NP4", 0, tempPath)) {
		warnings.push_back("device-id-not-persisted");
		return value;
	}
	HANDLE output = CreateFileW(tempPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	bool written = false;
	if (output != INVALID_HANDLE_VALUE) {
		DWORD count = 0;
		written = WriteFile(output, value.data(), static_cast<DWORD>(value.size()), &count, nullptr) != 0
			&& count == value.size();
		written = written && FlushFileBuffers(output);
		CloseHandle(output);
	}
	if (!written || !MoveFileExW(tempPath, privatePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		DeleteFileW(tempPath);
		warnings.push_back("device-id-not-persisted");
	}
	return value;
}

bool AddEntry(std::vector<ConfigArchiveEntry> &entries,
	LPCWSTR sourcePath,
	const char *archivePath,
	const char *category,
	bool sanitizeHistory,
	bool required,
	std::wstring *error) noexcept {
	if (!FileExists(sourcePath)) {
		if (required) {
			SetError(error, L"A required configuration file is missing.");
			return false;
		}
		return true;
	}

	ConfigArchiveEntry entry;
	entry.sourcePath = sourcePath;
	entry.archivePath = archivePath;
	entry.category = category;
	if (sanitizeHistory) {
		std::vector<uint8_t> input;
		if (!ReadFileBytes(sourcePath, input, error)) {
			return false;
		}
		if (!SanitizeIniHistory(input, _stricmp(category, "matepath") == 0, entry.inlineData, error)) {
			return false;
		}
		entry.hasInlineData = true;
	}
	entries.push_back(std::move(entry));
	return true;
}

bool BuildBackupEntries(const ConfigBackupOptions &options,
	std::vector<ConfigArchiveEntry> &entries,
	std::vector<std::string> &categories,
	std::vector<std::string> &warnings,
	std::wstring *error) noexcept {
	if (szIniFile[0] == L'\0') {
		SetError(error, L"The main configuration path is not initialized.");
		return false;
	}
	if (!AddEntry(entries, szIniFile, "config/Notepad4.ini", "settings", !options.includeHistory, true, error)) {
		return false;
	}

	WCHAR configDir[MAX_PATH];
	lstrcpynW(configDir, szIniFile, ARRAYSIZE(configDir));
	if (PathRemoveFileSpecW(configDir)) {
		for (const wchar_t *name : ThemeNames) {
			WCHAR themePath[MAX_PATH];
			lstrcpynW(themePath, configDir, ARRAYSIZE(themePath));
			if (!PathAppendW(themePath, name)) {
				continue;
			}
			if (!FileExists(themePath)) {
				std::string warning = "missing-theme:";
				warning += Narrow(name);
				warnings.push_back(std::move(warning));
				continue;
			}
			ConfigArchiveEntry entry;
			entry.sourcePath = themePath;
			entry.archivePath = "config/";
			entry.archivePath += Narrow(name);
			entry.category = "themes";
			entries.push_back(std::move(entry));
		}
	}

	const std::wstring matepathIni = GetMatepathIniPathInternal(true);
	if (!matepathIni.empty()) {
		if (!AddEntry(entries, matepathIni.c_str(), "config/matepath.ini", "matepath", !options.includeHistory, false, error)) {
			return false;
		}
	}

	categories = {"settings", "themes", "syntax"};
	if (!matepathIni.empty()) {
		categories.push_back("matepath");
	}
	if (options.includeHistory) {
		categories.push_back("history");
	}
	return true;
}

bool AppendHash(BCRYPT_HASH_HANDLE hash, const void *data, ULONG size) noexcept {
	return BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void *>(data)), size, 0) >= 0;
}

void RemoveWindowPositionSections(std::vector<uint8_t> &data) noexcept {
	if (data.size() < 2) {
		return;
	}
	const bool utf16 = data[0] == 0xFF && data[1] == 0xFE;
	const bool utf8 = data.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF;
	if (!utf16 && !utf8) {
		return;
	}
	std::wstring text;
	if (utf16) {
		text.assign(reinterpret_cast<const wchar_t *>(data.data() + 2), (data.size() - 2) / sizeof(wchar_t));
	} else {
		const int length = MultiByteToWideChar(CP_UTF8, 0,
			reinterpret_cast<const char *>(data.data() + 3),
			static_cast<int>(data.size() - 3), nullptr, 0);
		if (length <= 0) {
			return;
		}
		text.resize(static_cast<size_t>(length));
		MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char *>(data.data() + 3),
			static_cast<int>(data.size() - 3), text.data(), length);
	}

	std::wstring filtered;
	bool skipping = false;
	size_t start = 0;
	while (start <= text.size()) {
		const size_t end = text.find(L'\n', start);
		std::wstring line = text.substr(start,
			(end == std::wstring::npos ? text.size() : end) - start);
		if (!line.empty() && line.back() == L'\r') {
			line.pop_back();
		}
		const bool section = !line.empty() && line.front() == L'[' && line.back() == L']';
		if (section) {
			skipping = _wcsnicmp(line.c_str(), L"[Window Position", 16) == 0;
		}
		const bool defaultCloudLine = line == L"CloudUrl=" || line == L"CloudUser="
			|| line == L"CloudFolder=Notepad4-TxtSyntax" || line == L"CloudSaveCredentials=1";
		if (!skipping && !defaultCloudLine) {
			filtered += line;
			filtered.push_back(L'\r');
			filtered.push_back(L'\n');
		}
		if (end == std::wstring::npos) {
			break;
		}
		start = end + 1;
	}

	data.clear();
	if (utf16) {
		data.push_back(0xFF);
		data.push_back(0xFE);
		const size_t bytes = filtered.size() * sizeof(wchar_t);
		const size_t offset = data.size();
		data.resize(offset + bytes);
		memcpy(data.data() + offset, filtered.data(), bytes);
	} else {
		const int length = WideCharToMultiByte(CP_UTF8, 0, filtered.c_str(),
			static_cast<int>(filtered.size()), nullptr, 0, nullptr, nullptr);
		if (length <= 0) {
			return;
		}
		data = { 0xEF, 0xBB, 0xBF };
		const size_t offset = data.size();
		data.resize(offset + static_cast<size_t>(length));
		WideCharToMultiByte(CP_UTF8, 0, filtered.c_str(), static_cast<int>(filtered.size()),
			reinterpret_cast<char *>(data.data() + offset), length, nullptr, nullptr);
	}
}

bool ComputeFingerprintInternal(bool includeHistory, std::string &fingerprint,
	std::wstring *error) noexcept {
	fingerprint.clear();
	if (szIniFile[0] == L'\0') {
		SetError(error, L"The main configuration path is not initialized.");
		return false;
	}
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	DWORD hashLength = 0;
	DWORD resultLength = 0;
	bool ok = false;
	if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0
		|| BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
			reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &resultLength, 0) < 0
		|| hashLength != 32
		|| BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
		SetError(error, L"Cannot initialize the SHA-256 fingerprint.");
		goto cleanup;
	}

	{
		const char domain[] = "NP4FP1";
		const uint8_t option = includeHistory ? 1 : 0;
		if (!AppendHash(hash, domain, sizeof(domain) - 1)
			|| !AppendHash(hash, &option, sizeof(option))) {
			SetError(error, L"Cannot hash the fingerprint header.");
			goto cleanup;
		}

		struct Item {
			std::wstring name;
			std::wstring path;
			bool required = false;
			bool matepath = false;
		};
		std::vector<Item> items;
		items.push_back({ L"Notepad4.ini", szIniFile, true, false });
		WCHAR configDir[MAX_PATH];
		lstrcpynW(configDir, szIniFile, ARRAYSIZE(configDir));
		if (PathRemoveFileSpecW(configDir)) {
			for (const wchar_t *name : ThemeNames) {
				WCHAR path[MAX_PATH];
				lstrcpynW(path, configDir, ARRAYSIZE(path));
				if (PathAppendW(path, name)) {
					items.push_back({ name, path, false, false });
				}
			}
		}
		const std::wstring matepath = GetMatepathIniPathInternal(true);
		if (!matepath.empty()) {
			items.push_back({ L"matepath.ini", matepath, false, true });
		}

		for (const Item &item : items) {
			const std::string name = Narrow(item.name);
			const uint32_t nameLength = static_cast<uint32_t>(name.size());
			if (!AppendHash(hash, &nameLength, sizeof(nameLength))
				|| !AppendHash(hash, name.data(), nameLength)) {
				SetError(error, L"Cannot hash a configuration file name.");
				goto cleanup;
			}
			const bool exists = FileExists(item.path.c_str());
			const uint8_t existsByte = exists ? 1 : 0;
			if (!AppendHash(hash, &existsByte, sizeof(existsByte))) {
				SetError(error, L"Cannot hash a configuration file state.");
				goto cleanup;
			}
			if (!exists) {
				if (item.required) {
					SetError(error, L"A required configuration file is missing.");
					goto cleanup;
				}
				continue;
			}
			std::vector<uint8_t> input;
			if (!ReadFileBytes(item.path.c_str(), input, error)) {
				goto cleanup;
			}
			std::vector<uint8_t> sanitized;
			const std::vector<uint8_t> *content = &input;
			if (!includeHistory) {
				if (!SanitizeIniHistory(input, item.matepath, sanitized, error)) {
					goto cleanup;
				}
				content = &sanitized;
			}
			if (item.name == L"Notepad4.ini") {
				if (!includeHistory) {
					RemoveWindowPositionSections(sanitized);
				} else {
					RemoveWindowPositionSections(input);
				}
				content = includeHistory ? &input : &sanitized;
			}
			const uint32_t dataLength = static_cast<uint32_t>(content->size());
			if (!AppendHash(hash, &dataLength, sizeof(dataLength))
				|| !AppendHash(hash, content->data(), dataLength)) {
				SetError(error, L"Cannot hash a configuration file.");
				goto cleanup;
			}
		}
	}

	{
		std::array<uint8_t, 32> digest {};
		if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
			SetError(error, L"Cannot finalize the configuration fingerprint.");
			goto cleanup;
		}
		static constexpr char hex[] = "0123456789abcdef";
		fingerprint.reserve(digest.size() * 2);
		for (uint8_t byte : digest) {
			fingerprint.push_back(hex[byte >> 4]);
			fingerprint.push_back(hex[byte & 0x0F]);
		}
		ok = true;
	}

cleanup:
	if (hash != nullptr) {
		BCryptDestroyHash(hash);
	}
	if (algorithm != nullptr) {
		BCryptCloseAlgorithmProvider(algorithm, 0);
	}
	return ok;
}

bool ReadFingerprintInternal(LPCWSTR path, std::string &fingerprint) noexcept {
	fingerprint.clear();
	if (path == nullptr) {
		return false;
	}
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return false;
	}
	char buffer[128] {};
	DWORD count = 0;
	const bool read = ReadFile(file, buffer, sizeof(buffer) - 1, &count, nullptr) != 0;
	CloseHandle(file);
	if (!read) {
		return false;
	}
	std::string value(buffer, count);
	while (!value.empty() && (value.back() == '\r' || value.back() == '\n'
		|| value.back() == ' ' || value.back() == '\t')) {
		value.pop_back();
	}
	if (value.size() != 64 || !std::all_of(value.begin(), value.end(), [](char ch) {
		return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
	})) {
		return false;
	}
	fingerprint = std::move(value);
	return true;
}

bool WriteFingerprintInternal(LPCWSTR path, const std::string &fingerprint,
	std::wstring *error) noexcept {
	if (path == nullptr || fingerprint.size() != 64) {
		SetError(error, L"The configuration fingerprint is invalid.");
		return false;
	}
	std::wstring tempPath = path;
	tempPath += L".tmp";
	HANDLE file = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		SetWin32Error(error, L"Cannot create the fingerprint state file.", GetLastError());
		return false;
	}
	DWORD written = 0;
	const bool write = WriteFile(file, fingerprint.data(),
		static_cast<DWORD>(fingerprint.size()), &written, nullptr) != 0
		&& written == fingerprint.size() && FlushFileBuffers(file);
	CloseHandle(file);
	if (!write || !MoveFileExW(tempPath.c_str(), path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		DeleteFileW(tempPath.c_str());
		SetWin32Error(error, L"Cannot store the fingerprint state file.", GetLastError());
		return false;
	}
	return true;
}

std::wstring AppVersion() noexcept {
	return VERSION_FILEVERSION;
}

} // namespace

bool ConfigBackup_ComputeFingerprint(bool includeHistory, std::string &fingerprint,
	std::wstring *error) noexcept {
	return ComputeFingerprintInternal(includeHistory, fingerprint, error);
}

bool ConfigBackup_ReadFingerprint(LPCWSTR path, std::string &fingerprint) noexcept {
	return ReadFingerprintInternal(path, fingerprint);
}

bool ConfigBackup_WriteFingerprint(LPCWSTR path, const std::string &fingerprint,
	std::wstring *error) noexcept {
	return WriteFingerprintInternal(path, fingerprint, error);
}

std::wstring ConfigBackup_GetMatepathIniPath() noexcept {
	return GetMatepathIniPathInternal(true);
}

std::wstring ConfigBackup_GetMatepathIniTargetPath() noexcept {
	return GetMatepathIniPathInternal(false);
}

std::string ConfigBackup_DeviceId() noexcept {
	std::vector<std::string> warnings;
	return GetDeviceId(warnings);
}

bool ConfigBackup_Create(LPCWSTR destinationPath,
	const ConfigBackupOptions &options,
	ConfigBackupResult *result,
	std::wstring *error) noexcept {
	if (error != nullptr) {
		error->clear();
	}
	if (result != nullptr) {
		*result = {};
	}
	if (destinationPath == nullptr || destinationPath[0] == L'\0') {
		SetError(error, L"A backup destination is required.");
		return false;
	}

	try {
		std::vector<ConfigArchiveEntry> entries;
		std::vector<std::string> categories;
		std::vector<std::string> warnings;
		if (!BuildBackupEntries(options, entries, categories, warnings, error)) {
			return false;
		}

		ConfigArchiveMetadata metadata;
		metadata.appVersion = Narrow(AppVersion());
		metadata.buildId = Narrow(VERSION_HASH);
		std::wstring timestamp;
		GetUtcTimestamp(timestamp);
		metadata.createdAtUtc = Narrow(timestamp);
		metadata.deviceId = GetDeviceId(warnings);
		{
			std::wstring backupId;
			if (!NewGuidString(backupId)) {
				warnings.push_back("backup-id-generation-failed");
			} else {
				metadata.backupId = Narrow(backupId);
			}
		}
		metadata.includeHistory = options.includeHistory;
		metadata.categories = std::move(categories);
		metadata.warnings = warnings;

		ConfigArchiveCreateResult archiveResult;
		LPCWSTR destination = destinationPath;
		if (!ConfigArchive_Create(entries, metadata, destination, &archiveResult, error)) {
			return false;
		}

		if (result != nullptr) {
			result->path = destinationPath;
			result->sha256 = archiveResult.zipSha256;
			result->size = archiveResult.zipSize;
			result->historyIncluded = options.includeHistory;
			result->warnings = std::move(warnings);
		}
		return true;
	} catch (const std::exception &) {
		SetError(error, L"Not enough memory to create the settings backup.");
	} catch (...) {
		SetError(error, L"Unexpected failure while creating the settings backup.");
	}
	return false;
}

void ConfigBackup_Export(HWND hwndOwner) noexcept {
	WCHAR title[128];
	WCHAR privacy[1024];
	WCHAR successFormat[1024];
	WCHAR failFormat[1024];
	WCHAR noIni[512];
	WCHAR historyYes[256];
	WCHAR historyNo[256];
	WCHAR filter[256];

	LoadStringResource(IDS_CONFIG_BACKUP_TITLE, L"Export Settings Backup", title, ARRAYSIZE(title));
	LoadStringResource(IDS_CONFIG_BACKUP_PRIVACY,
		L"Include recent files, find history and replace history?\n\n"
		L"Choose No to back up settings, themes and syntax without personal history.\n"
		L"Only saved settings are backed up; press F7 first to save unsaved changes.",
		privacy, ARRAYSIZE(privacy));
	LoadStringResource(IDS_CONFIG_BACKUP_SUCCESS, L"Settings backup created:\n%s\n\nSHA-256:\n%s\n\n%s", successFormat, ARRAYSIZE(successFormat));
	LoadStringResource(IDS_CONFIG_BACKUP_FAIL, L"Cannot create the settings backup.\n\n%s", failFormat, ARRAYSIZE(failFormat));
	LoadStringResource(IDS_CONFIG_BACKUP_NOINI,
		L"Cannot find the saved main configuration file. Press F7 to save settings, then try again.",
		noIni, ARRAYSIZE(noIni));
	LoadStringResource(IDS_CONFIG_BACKUP_HISTORY_YES, L"Recent files and find/replace history are included.", historyYes, ARRAYSIZE(historyYes));
	LoadStringResource(IDS_CONFIG_BACKUP_HISTORY_NO, L"Recent files and find/replace history are excluded.", historyNo, ARRAYSIZE(historyNo));
	LoadStringResource(IDS_CONFIG_BACKUP_FILTER, L"Settings Backup (*.zip)|*.zip|All Files (*.*)|*.*|", filter, ARRAYSIZE(filter));

	if (szIniFile[0] == L'\0' || !FileExists(szIniFile)) {
		MessageBoxW(hwndOwner, noIni, title, MB_OK | MB_ICONWARNING);
		return;
	}

	const int choice = MessageBoxW(hwndOwner, privacy, title,
		MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
	if (choice == IDCANCEL) {
		return;
	}
	ConfigBackupOptions options;
	options.includeHistory = (choice == IDYES);

	WCHAR fileName[MAX_PATH];
	swprintf_s(fileName, L"Notepad4-Settings-%s.zip", GetLocalFileNameTimestamp().c_str());

	OPENFILENAMEW dialog {};
	dialog.lStructSize = sizeof(dialog);
	dialog.hwndOwner = hwndOwner;
	dialog.lpstrFilter = filter;
	dialog.lpstrFile = fileName;
	dialog.nMaxFile = ARRAYSIZE(fileName);
	dialog.lpstrTitle = title;
	dialog.lpstrDefExt = L"zip";
	dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
	PrepareFilterString(filter);
	if (!GetSaveFileNameW(&dialog)) {
		return;
	}

	if (!PathMatchSpecW(fileName, L"*.zip")) {
		if (lstrlenW(fileName) + 4 >= ARRAYSIZE(fileName)) {
			MessageBoxW(hwndOwner, L"The chosen file name is too long.", title, MB_OK | MB_ICONERROR);
			return;
		}
		lstrcatW(fileName, L".zip");
	}
	if (options.includeHistory) {
		// MRU lists are flushed on exit; persist them after the user confirms
		// the destination so a cancelled dialog has no side effects.
		SaveHistoryForBackup();
	}

	ConfigBackupResult result;
	std::wstring error;
	if (!ConfigBackup_Create(fileName, options, &result, &error)) {
		WCHAR detail[1024];
		lstrcpynW(detail, error.empty() ? L"Unknown error." : error.c_str(), ARRAYSIZE(detail));
		WCHAR message[2048];
		if (swprintf_s(message, failFormat, detail) < 0) {
			lstrcpynW(message, detail, ARRAYSIZE(message));
		}
		MessageBoxW(hwndOwner, message, title, MB_OK | MB_ICONERROR);
		return;
	}

	const std::wstring hash = Widen(result.sha256);
	WCHAR message[4096];
	if (swprintf_s(message, successFormat, result.path.c_str(), hash.c_str(),
		options.includeHistory ? historyYes : historyNo) < 0) {
		lstrcpynW(message, result.path.c_str(), ARRAYSIZE(message));
	}
	MessageBoxW(hwndOwner, message, title, MB_OK | MB_ICONINFORMATION);
}
