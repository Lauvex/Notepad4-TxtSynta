/******************************************************************************
*
* Notepad4-TxtSyntax
*
* ConfigRestore.cpp
*   Validated, selective and transactional settings restore.
*
******************************************************************************/

#include <windows.h>
#include <windowsx.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "SciCall.h"
#include "config.h"
#include "Helpers.h"
#include "Notepad4.h"
#include "Version.h"
#include "ConfigArchive.h"
#include "ConfigBackup.h"
#include "ConfigRestore.h"
#include "resource.h"

namespace {

using ByteVector = std::vector<uint8_t>;
namespace fs = std::filesystem;

constexpr uint64_t Restore_MaxConfigFileSize = 16ULL * 1024 * 1024;
constexpr wchar_t RestorePendingFolder[] = L"RestorePending";
constexpr wchar_t RestoreTransactionFolder[] = L"current";
constexpr wchar_t RestoreRollbackFolder[] = L"RestoreRollback";
constexpr wchar_t RestoreJournalName[] = L"journal.txt";
constexpr int DefaultRollbackKeep = 10;
constexpr int MaxRollbackKeep = 50;

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

void LoadStringResource(UINT id, LPCWSTR fallback, LPWSTR buffer, int capacity) noexcept {
	if (capacity <= 0) {
		return;
	}
	if (LoadStringW(g_hInstance, id, buffer, capacity) <= 0) {
		lstrcpynW(buffer, fallback, capacity);
	}
}

std::wstring Trim(std::wstring text) noexcept {
	size_t first = 0;
	while (first < text.size() && iswspace(text[first])) {
		++first;
	}
	size_t last = text.size();
	while (last > first && iswspace(text[last - 1])) {
		--last;
	}
	text.erase(last);
	text.erase(0, first);
	return text;
}

bool EqualsNoCase(const std::wstring &a, const wchar_t *b) noexcept {
	return _wcsicmp(a.c_str(), b) == 0;
}

bool StartsWithNoCase(const std::wstring &text, const wchar_t *prefix) noexcept {
	return _wcsnicmp(text.c_str(), prefix, wcslen(prefix)) == 0;
}

bool FileExists(LPCWSTR path) noexcept {
	const DWORD attributes = GetFileAttributesW(path);
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool IsDirectory(LPCWSTR path) noexcept {
	const DWORD attributes = GetFileAttributesW(path);
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool ReadFileBytes(LPCWSTR path, ByteVector &data, bool &exists, std::wstring *error) noexcept {
	data.clear();
	exists = false;
	const DWORD attributes = GetFileAttributesW(path);
	if (attributes == INVALID_FILE_ATTRIBUTES) {
		const DWORD code = GetLastError();
		if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
			return true;
		}
		SetError(error, L"Cannot query a configuration file.");
		return false;
	}
	if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
		SetError(error, L"A configuration target is not a regular file.");
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
		|| static_cast<uint64_t>(size.QuadPart) > Restore_MaxConfigFileSize) {
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
	exists = true;
	return true;
}

bool EnsureDirectory(LPCWSTR path) noexcept {
	if (PathIsDirectoryW(path)) {
		return true;
	}
	const int result = SHCreateDirectoryExW(nullptr, path, nullptr);
	return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS || PathIsDirectoryW(path);
}

bool WriteFileAtomically(LPCWSTR path, const ByteVector &data, std::wstring *error) noexcept {
	WCHAR directory[MAX_PATH];
	lstrcpynW(directory, path, ARRAYSIZE(directory));
	if (!PathRemoveFileSpecW(directory) || !EnsureDirectory(directory)) {
		SetError(error, L"Cannot create the configuration directory.");
		return false;
	}
	WCHAR tempPath[MAX_PATH];
	if (!GetTempFileNameW(directory, L"NP4", 0, tempPath)) {
		SetError(error, L"Cannot create a temporary configuration file.");
		return false;
	}
	HANDLE file = CreateFileW(tempPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_TEMPORARY, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		DeleteFileW(tempPath);
		SetError(error, L"Cannot create a temporary configuration file.");
		return false;
	}
	bool success = true;
	size_t offset = 0;
	while (offset < data.size()) {
		const DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - offset, 1U << 20));
		DWORD written = 0;
		if (!WriteFile(file, data.data() + offset, chunk, &written, nullptr) || written != chunk) {
			SetError(error, L"Cannot write a configuration file.");
			success = false;
			break;
		}
		offset += written;
	}
	if (success && !FlushFileBuffers(file)) {
		SetError(error, L"Cannot flush a configuration file.");
		success = false;
	}
	CloseHandle(file);
	if (success && !MoveFileExW(tempPath, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		SetError(error, L"Cannot replace a configuration file.");
		success = false;
	}
	if (!success) {
		DeleteFileW(tempPath);
	}
	return success;
}

bool DeleteDirectoryTree(const std::wstring &path) noexcept {
	std::error_code ec;
	fs::remove_all(fs::path(path), ec);
	return !ec;
}

bool MoveOrCopyDirectory(const std::wstring &source, const std::wstring &destination) noexcept {
	if (!PathIsDirectoryW(source.c_str())) {
		return true;
	}
	if (PathIsDirectoryW(destination.c_str())) {
		return true;
	}
	if (MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_COPY_ALLOWED)) {
		return true;
	}
	std::error_code ec;
	fs::copy(source, destination, fs::copy_options::recursive, ec);
	return !ec && PathIsDirectoryW(destination.c_str());
}

std::wstring GetNotepad4DataRoot() noexcept {
	const std::wstring privateRoot = GetNotepad4PrivateDataDirectory();
	const std::wstring legacyRoot = GetNotepad4DataDirectory();
	if (privateRoot.empty()) {
		return legacyRoot;
	}
	if (!PathIsDirectoryW(privateRoot.c_str())) {
		SHCreateDirectoryExW(nullptr, privateRoot.c_str(), nullptr);
	}
	if (!PathIsDirectoryW(privateRoot.c_str())) {
		return legacyRoot;
	}
	if (!legacyRoot.empty() && _wcsicmp(privateRoot.c_str(), legacyRoot.c_str()) != 0) {
		const std::wstring pendingSource = legacyRoot + L"\\" + RestorePendingFolder;
		const std::wstring rollbackSource = legacyRoot + L"\\" + RestoreRollbackFolder;
		const bool pendingOk = MoveOrCopyDirectory(pendingSource,
			privateRoot + L"\\" + RestorePendingFolder);
		const bool rollbackOk = MoveOrCopyDirectory(rollbackSource,
			privateRoot + L"\\" + RestoreRollbackFolder);
		if (!pendingOk || !rollbackOk) {
			return legacyRoot;
		}
	}
	return privateRoot;
}

std::wstring GetTransactionDirectory() noexcept {
	const std::wstring root = GetNotepad4DataRoot();
	if (root.empty()) {
		return {};
	}
	return root + L"\\" + RestorePendingFolder + L"\\" + RestoreTransactionFolder;
}

std::wstring GetRollbackDirectory() noexcept {
	const std::wstring root = GetNotepad4DataRoot();
	if (root.empty()) {
		return {};
	}
	return root + L"\\" + RestoreRollbackFolder;
}

std::wstring GetTimestampForFileName() noexcept {
	SYSTEMTIME time {};
	GetLocalTime(&time);
	wchar_t buffer[32];
	swprintf_s(buffer, L"%04u%02u%02u-%02u%02u%02u",
		time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
	return buffer;
}

std::wstring GetConfigDirectory() noexcept {
	WCHAR directory[MAX_PATH];
	lstrcpynW(directory, szIniFile, ARRAYSIZE(directory));
	if (!PathRemoveFileSpecW(directory)) {
		return {};
	}
	return directory;
}

bool ResolveTargetPath(const std::string &archivePath, std::wstring &target, std::wstring *error) noexcept {
	target.clear();
	std::string name = archivePath;
	if (name.rfind("config/", 0) == 0) {
		name.erase(0, 7);
	}
	if (name == "Notepad4.ini") {
		target = szIniFile;
	} else if (name == "matepath.ini") {
		target = ConfigBackup_GetMatepathIniTargetPath();
	} else {
		const std::wstring directory = GetConfigDirectory();
		if (directory.empty()) {
			SetError(error, L"The configuration directory is not available.");
			return false;
		}
		bool theme = false;
		for (const wchar_t *themeName : ThemeNames) {
			char narrow[128] {};
			WideCharToMultiByte(CP_UTF8, 0, themeName, -1, narrow, ARRAYSIZE(narrow), nullptr, nullptr);
			if (name == narrow) {
				theme = true;
				break;
			}
		}
		if (!theme) {
			SetError(error, L"The archive contains an unknown configuration file.");
			return false;
		}
		target = directory + L"\\";
		target += std::wstring(name.begin(), name.end());
	}
	if (target.empty()) {
		SetError(error, L"Cannot resolve a configuration target path.");
		return false;
	}
	return true;
}

bool IsAllowedTargetPath(const std::wstring &path) noexcept {
	if (PathEqual(path.c_str(), szIniFile)) {
		return true;
	}
	const std::wstring directory = GetConfigDirectory();
	if (directory.empty()) {
		return false;
	}
	for (const wchar_t *name : ThemeNames) {
		const std::wstring candidate = directory + L"\\" + name;
		if (PathEqual(path.c_str(), candidate.c_str())) {
			return true;
		}
	}
	const std::wstring matepath = ConfigBackup_GetMatepathIniTargetPath();
	return !matepath.empty() && PathEqual(path.c_str(), matepath.c_str());
}

enum class IniFileKind {
	Main,
	Theme,
	Matepath,
};

struct IniSection {
	std::wstring name;
	std::vector<std::wstring> lines;
};

struct IniDocument {
	std::vector<std::wstring> preamble;
	std::vector<IniSection> sections;
};

IniSection *FindSection(IniDocument &doc, const std::wstring &name) noexcept {
	for (IniSection &section : doc.sections) {
		if (_wcsicmp(section.name.c_str(), name.c_str()) == 0) {
			return &section;
		}
	}
	return nullptr;
}

const IniSection *FindSection(const IniDocument &doc, const std::wstring &name) noexcept {
	for (const IniSection &section : doc.sections) {
		if (_wcsicmp(section.name.c_str(), name.c_str()) == 0) {
			return &section;
		}
	}
	return nullptr;
}

bool ParseKey(const std::wstring &line, std::wstring &key, std::wstring &value) noexcept {
	const std::wstring trimmed = Trim(line);
	if (trimmed.empty() || trimmed[0] == L';' || trimmed[0] == L'#') {
		return false;
	}
	const size_t equals = trimmed.find(L'=');
	if (equals == std::wstring::npos) {
		return false;
	}
	key = Trim(trimmed.substr(0, equals));
	value = Trim(trimmed.substr(equals + 1));
	return !key.empty();
}

bool ParseSectionHeader(const std::wstring &line, std::wstring &name) noexcept {
	const std::wstring trimmed = Trim(line);
	if (trimmed.size() < 2 || trimmed.front() != L'[' || trimmed.back() != L']') {
		return false;
	}
	name = Trim(trimmed.substr(1, trimmed.size() - 2));
	return !name.empty();
}

bool ParseIni(const ByteVector &data, IniDocument &doc, std::wstring *error) noexcept {
	doc = {};
	if (data.empty()) {
		return true;
	}
	if (data.size() < 2 || data[0] != 0xFF || data[1] != 0xFE || (data.size() & 1) != 0) {
		SetError(error, L"A configuration file is not UTF-16LE with BOM.");
		return false;
	}
	const size_t count = (data.size() - 2) / sizeof(wchar_t);
	std::wstring text(count, L'\0');
	memcpy(text.data(), data.data() + 2, count * sizeof(wchar_t));
	if (std::find(text.begin(), text.end(), L'\0') != text.end()) {
		SetError(error, L"A configuration file contains invalid characters.");
		return false;
	}

	IniSection *current = nullptr;
	std::wstring sectionName;
	size_t start = 0;
	while (start <= text.size()) {
		size_t end = start;
		while (end < text.size() && text[end] != L'\r' && text[end] != L'\n') {
			++end;
		}
		std::wstring line = text.substr(start, end - start);
		if (ParseSectionHeader(line, sectionName)) {
			current = FindSection(doc, sectionName);
			if (current == nullptr) {
				IniSection section;
				section.name = sectionName;
				doc.sections.push_back(std::move(section));
				current = &doc.sections.back();
			}
		} else if (current == nullptr) {
			doc.preamble.push_back(std::move(line));
		} else {
			current->lines.push_back(std::move(line));
		}
		if (end >= text.size()) {
			break;
		}
		start = end + ((text[end] == L'\r' && end + 1 < text.size() && text[end + 1] == L'\n') ? 2 : 1);
	}
	return true;
}

ByteVector SerializeIni(const IniDocument &doc) noexcept {
	std::wstring text;
	for (const std::wstring &line : doc.preamble) {
		text += line;
		text += L"\r\n";
	}
	for (const IniSection &section : doc.sections) {
		text += L"[";
		text += section.name;
		text += L"]\r\n";
		for (const std::wstring &line : section.lines) {
			text += line;
			text += L"\r\n";
		}
	}
	ByteVector data;
	data.reserve(2 + text.size() * sizeof(wchar_t));
	data.push_back(0xFF);
	data.push_back(0xFE);
	const size_t offset = data.size();
	data.resize(offset + text.size() * sizeof(wchar_t));
	memcpy(data.data() + offset, text.data(), text.size() * sizeof(wchar_t));
	return data;
}

bool IsHistorySection(const std::wstring &name) noexcept {
	return EqualsNoCase(name, L"Recent Files") || EqualsNoCase(name, L"Recent Find")
		|| EqualsNoCase(name, L"Recent Replace");
}

bool IsInterfaceSection(const std::wstring &name) noexcept {
	return StartsWithNoCase(name, L"Window Position") || EqualsNoCase(name, L"Toolbar Labels");
}

bool IsThemeSection(const std::wstring &name) noexcept {
	return EqualsNoCase(name, L"Global Styles") || EqualsNoCase(name, L"Custom Colors");
}

bool IsUiSettingKey(const std::wstring &key) noexcept {
	static constexpr const wchar_t *keys[] = {
		L"ShowMenu", L"ShowToolbar", L"ToolbarButtons", L"AutoScaleToolbar",
		L"ShowStatusbar", L"FullScreenMode", L"AlwaysOnTop", L"MinimizeToTray",
		L"TransparentMode", L"EditLayoutRTL", L"WindowLayoutRTL", L"WindowPosX",
		L"WindowPosY", L"PathNameFormat",
	};
	for (const wchar_t *candidate : keys) {
		if (EqualsNoCase(key, candidate)) {
			return true;
		}
	}
	return false;
}

uint32_t KeyCategory(IniFileKind kind, const std::wstring &section, const std::wstring &key) noexcept {
	if (kind == IniFileKind::Matepath) {
		if (EqualsNoCase(section, L"Copy/Move MRU") || EqualsNoCase(key, L"MRUDirectory")) {
			return ConfigRestoreCategory_History;
		}
		return ConfigRestoreCategory_Matepath;
	}
	if (kind == IniFileKind::Theme) {
		return IsThemeSection(section) ? ConfigRestoreCategory_Themes : ConfigRestoreCategory_Syntax;
	}
	if (IsHistorySection(section)) {
		return ConfigRestoreCategory_History;
	}
	if (IsInterfaceSection(section)) {
		return ConfigRestoreCategory_Interface;
	}
	if (EqualsNoCase(section, L"Styles")) {
		return EqualsNoCase(key, L"StyleTheme") ? ConfigRestoreCategory_Themes : ConfigRestoreCategory_Syntax;
	}
	if (EqualsNoCase(section, L"Settings")) {
		return IsUiSettingKey(key) ? ConfigRestoreCategory_Interface : ConfigRestoreCategory_Basic;
	}
	if (EqualsNoCase(section, L"Settings2") || EqualsNoCase(section, L"Notepad4")
		|| EqualsNoCase(section, L"Suppressed Messages")) {
		return ConfigRestoreCategory_Basic;
	}
	if (EqualsNoCase(section, L"Custom Colors")) {
		return ConfigRestoreCategory_Themes;
	}
	if (EqualsNoCase(section, L"File Extensions")) {
		return ConfigRestoreCategory_Syntax;
	}
	return ConfigRestoreCategory_Syntax;
}

bool HasSelectedContent(IniFileKind kind, const IniDocument &doc, uint32_t categories) noexcept {
	if (kind == IniFileKind::Matepath && (categories & ConfigRestoreCategory_Matepath) == 0) {
		return false;
	}
	for (const IniSection &section : doc.sections) {
		for (const std::wstring &line : section.lines) {
			std::wstring key;
			std::wstring value;
			if (ParseKey(line, key, value)
				&& (KeyCategory(kind, section.name, key) & categories) != 0) {
				return true;
			}
		}
	}
	return false;
}

bool MergeIni(IniFileKind kind, const IniDocument &backup, const IniDocument &current,
	uint32_t categories, IniDocument &merged) noexcept {
	merged = {};
	merged.preamble = current.preamble.empty() ? backup.preamble : current.preamble;
	for (const IniSection &backupSection : backup.sections) {
		bool anySelected = false;
		for (const std::wstring &line : backupSection.lines) {
			std::wstring key;
			std::wstring value;
			if (ParseKey(line, key, value)
				&& (KeyCategory(kind, backupSection.name, key) & categories) != 0) {
				anySelected = true;
				break;
			}
		}
		if (!anySelected) {
			continue;
		}
		const IniSection *currentSection = FindSection(current, backupSection.name);
		IniSection outSection;
		outSection.name = backupSection.name;
		for (const std::wstring &line : backupSection.lines) {
			std::wstring key;
			std::wstring value;
			if (ParseKey(line, key, value)
				&& (KeyCategory(kind, backupSection.name, key) & categories) != 0) {
				outSection.lines.push_back(line);
			}
		}
		if (currentSection != nullptr) {
			for (const std::wstring &line : currentSection->lines) {
				std::wstring key;
				std::wstring value;
				if (!ParseKey(line, key, value)) {
					continue;
				}
				bool restored = false;
				for (const std::wstring &backupLine : backupSection.lines) {
					std::wstring backupKey;
					std::wstring backupValue;
					if (ParseKey(backupLine, backupKey, backupValue) && _wcsicmp(backupKey.c_str(), key.c_str()) == 0
						&& (KeyCategory(kind, backupSection.name, backupKey) & categories) != 0) {
						restored = true;
						break;
					}
				}
				if (!restored) {
					outSection.lines.push_back(line);
				}
			}
		}
		if (!outSection.lines.empty()) {
			merged.sections.push_back(std::move(outSection));
		}
	}
	for (const IniSection &currentSection : current.sections) {
		if (FindSection(merged, currentSection.name) == nullptr) {
			merged.sections.push_back(currentSection);
		}
	}
	return true;
}

std::wstring TruncateValue(const std::wstring &value) noexcept {
	constexpr size_t MaxLength = 100;
	if (value.size() <= MaxLength) {
		return value;
	}
	std::wstring result = value.substr(0, MaxLength);
	result += L"...";
	return result;
}

struct DiffResult {
	int changed = 0;
	int added = 0;
	int removed = 0;
	std::vector<std::wstring> details;
};

bool SectionHasKey(const IniSection &section, const std::wstring &key) noexcept {
	for (const std::wstring &line : section.lines) {
		std::wstring candidateKey;
		std::wstring candidateValue;
		if (ParseKey(line, candidateKey, candidateValue) && _wcsicmp(candidateKey.c_str(), key.c_str()) == 0) {
			return true;
		}
	}
	return false;
}

DiffResult DiffIni(const IniDocument &current, const IniDocument &next) noexcept {
	DiffResult diff;
	constexpr size_t MaxDetails = 40;
	for (const IniSection &section : next.sections) {
		const IniSection *oldSection = FindSection(current, section.name);
		for (const std::wstring &line : section.lines) {
			std::wstring key;
			std::wstring value;
			if (!ParseKey(line, key, value)) {
				continue;
			}
			std::wstring oldValue;
			bool oldValueFound = false;
			if (oldSection != nullptr) {
				for (const std::wstring &oldLine : oldSection->lines) {
					std::wstring oldKey;
					std::wstring candidate;
					if (ParseKey(oldLine, oldKey, candidate) && _wcsicmp(oldKey.c_str(), key.c_str()) == 0) {
						oldValue = candidate;
						oldValueFound = true;
						break;
					}
				}
			}
			if (!oldValueFound) {
				++diff.added;
				if (diff.details.size() < MaxDetails) {
					diff.details.push_back(L"+ [" + section.name + L"] " + key + L" = " + TruncateValue(value));
				}
			} else if (oldValue != value) {
				++diff.changed;
				if (diff.details.size() < MaxDetails) {
					diff.details.push_back(L"~ [" + section.name + L"] " + key + L": "
						+ TruncateValue(oldValue) + L" -> " + TruncateValue(value));
				}
			}
		}
	}
	for (const IniSection &section : current.sections) {
		const IniSection *newSection = FindSection(next, section.name);
		for (const std::wstring &line : section.lines) {
			std::wstring key;
			std::wstring value;
			if (!ParseKey(line, key, value)) {
				continue;
			}
			if (newSection != nullptr && SectionHasKey(*newSection, key)) {
				continue;
			}
			++diff.removed;
			if (diff.details.size() < MaxDetails) {
				diff.details.push_back(L"- [" + section.name + L"] " + key);
			}
		}
	}
	return diff;
}

enum class RestoreTargetKind {
	Main,
	Theme,
	Matepath,
};

RestoreTargetKind GetTargetKind(const std::string &archivePath, const std::wstring &target) noexcept {
	if (archivePath.find("matepath.ini") != std::string::npos) {
		return RestoreTargetKind::Matepath;
	}
	if (PathEqual(target.c_str(), szIniFile)) {
		return RestoreTargetKind::Main;
	}
	return RestoreTargetKind::Theme;
}

struct PlanItem {
	std::wstring targetPath;
	std::wstring fileName;
	std::string archivePath;
	RestoreTargetKind kind = RestoreTargetKind::Main;
	ByteVector originalData;
	ByteVector newData;
	bool originalExists = false;
	bool wholeFile = false;
	ConfigRestorePreviewFile preview;
};

bool BuildPlan(const ConfigArchivePackage &package, uint32_t categories,
	std::vector<PlanItem> &plan, ConfigRestorePreview &preview, std::wstring *error) noexcept {
	plan.clear();
	preview = {};
	preview.legacy = package.format != ConfigArchiveFormat::Current;
	if (categories == 0) {
		SetError(error, L"No restore categories were selected.");
		return false;
	}

	try {
		std::vector<std::wstring> targets;
		for (const ConfigArchiveFile &file : package.files) {
			std::wstring target;
			if (!ResolveTargetPath(file.path, target, error)) {
				return false;
			}
			for (const std::wstring &existing : targets) {
				if (PathEqual(existing.c_str(), target.c_str())) {
					SetError(error, L"Two backup files resolve to the same target.");
					return false;
				}
			}
			targets.push_back(target);

			const RestoreTargetKind kind = GetTargetKind(file.path, target);
			if (kind == RestoreTargetKind::Matepath
				&& (categories & ConfigRestoreCategory_Matepath) == 0) {
				continue;
			}
			const IniFileKind iniKind = (kind == RestoreTargetKind::Main) ? IniFileKind::Main
				: (kind == RestoreTargetKind::Theme) ? IniFileKind::Theme : IniFileKind::Matepath;

			ByteVector currentData;
			bool currentExists = false;
			if (!ReadFileBytes(target.c_str(), currentData, currentExists, error)) {
				return false;
			}

			IniDocument backupDoc;
			if (!ParseIni(file.data, backupDoc, error)) {
				return false;
			}
			ByteVector nextData;
			bool wholeFile = categories == ConfigRestoreCategory_All;
			if (wholeFile) {
				nextData = file.data;
			} else {
				if (!HasSelectedContent(iniKind, backupDoc, categories)) {
					continue;
				}
				IniDocument currentDoc;
				if (!ParseIni(currentData, currentDoc, error)) {
					return false;
				}
				IniDocument merged;
				MergeIni(iniKind, backupDoc, currentDoc, categories, merged);
				nextData = SerializeIni(merged);
			}

			if (currentExists && nextData == currentData) {
				continue;
			}
			if (!wholeFile && nextData == currentData) {
				continue;
			}

			ConfigRestorePreviewFile filePreview;
			filePreview.fileName = PathFindFileNameW(target.c_str());
			filePreview.wholeFile = wholeFile;
			if (currentExists) {
				IniDocument currentDoc;
				IniDocument nextDoc;
				const bool currentParsed = ParseIni(currentData, currentDoc, nullptr);
				const bool nextParsed = ParseIni(nextData, nextDoc, nullptr);
				if (currentParsed && nextParsed) {
					const DiffResult diff = DiffIni(currentDoc, nextDoc);
					filePreview.changed = diff.changed;
					filePreview.added = diff.added;
					filePreview.removed = diff.removed;
					filePreview.details = diff.details;
					if (!wholeFile && diff.changed == 0 && diff.added == 0 && diff.removed == 0) {
						continue;
					}
				} else if (!wholeFile) {
					SetError(error, L"Cannot parse the current or restored configuration file.");
					return false;
				} else {
					filePreview.changed = 1;
					filePreview.details.push_back(L"The file will be replaced.");
				}
			} else {
				IniDocument nextDoc;
				if (ParseIni(nextData, nextDoc, nullptr)) {
					const DiffResult diff = DiffIni(IniDocument {}, nextDoc);
					filePreview.changed = diff.changed;
					filePreview.added = diff.added;
					filePreview.removed = diff.removed;
					filePreview.details = diff.details;
				}
			}

			PlanItem item;
			item.targetPath = target;
			item.fileName = filePreview.fileName;
			item.archivePath = file.path;
			item.kind = kind;
			item.originalData = std::move(currentData);
			item.newData = std::move(nextData);
			item.originalExists = currentExists;
			item.wholeFile = wholeFile;
			item.preview = std::move(filePreview);
			plan.push_back(std::move(item));
		}

		for (const PlanItem &item : plan) {
			preview.changed += item.preview.changed;
			preview.added += item.preview.added;
			preview.removed += item.preview.removed;
			preview.files.push_back(item.preview);
		}
		preview.hasChanges = !plan.empty();
		return true;
	} catch (const std::exception &) {
		SetError(error, L"Not enough memory to build the restore plan.");
	}
	return false;
}

bool WriteJournal(const std::wstring &transactionDir, const wchar_t *state,
	const std::vector<std::pair<std::wstring, bool>> &items, std::wstring *error) noexcept {
	std::wstring text = L"NP4RESTORE1\r\nstate=";
	text += state;
	text += L"\r\ncount=";
	text += std::to_wstring(items.size());
	text += L"\r\n";
	for (size_t i = 0; i < items.size(); ++i) {
		text += L"target" + std::to_wstring(i) + L"=" + items[i].first + L"\r\n";
		text += L"existed" + std::to_wstring(i) + L"=" + (items[i].second ? L"1" : L"0") + L"\r\n";
	}
	ByteVector data;
	data.push_back(0xFF);
	data.push_back(0xFE);
	const size_t offset = data.size();
	data.resize(offset + text.size() * sizeof(wchar_t));
	memcpy(data.data() + offset, text.data(), text.size() * sizeof(wchar_t));
	const std::wstring journal = transactionDir + L"\\" + RestoreJournalName;
	return WriteFileAtomically(journal.c_str(), data, error);
}

struct JournalData {
	std::wstring state;
	std::vector<std::pair<std::wstring, bool>> items;
};

bool ReadJournal(const std::wstring &transactionDir, JournalData &journal, std::wstring *error) noexcept {
	journal = {};
	const std::wstring path = transactionDir + L"\\" + RestoreJournalName;
	ByteVector data;
	bool exists = false;
	if (!ReadFileBytes(path.c_str(), data, exists, error) || !exists) {
		if (error != nullptr && error->empty()) {
			SetError(error, L"The restore journal is missing.");
		}
		return false;
	}
	if (data.size() < 2 || data[0] != 0xFF || data[1] != 0xFE || (data.size() & 1) != 0) {
		SetError(error, L"The restore journal is invalid.");
		return false;
	}
	const size_t count = (data.size() - 2) / sizeof(wchar_t);
	std::wstring text(count, L'\0');
	memcpy(text.data(), data.data() + 2, count * sizeof(wchar_t));
	size_t start = 0;
	std::vector<std::wstring> lines;
	while (start <= text.size()) {
		size_t end = start;
		while (end < text.size() && text[end] != L'\r' && text[end] != L'\n') {
			++end;
		}
		lines.push_back(text.substr(start, end - start));
		if (end >= text.size()) {
			break;
		}
		start = end + ((text[end] == L'\r' && end + 1 < text.size() && text[end + 1] == L'\n') ? 2 : 1);
	}
	if (lines.empty() || lines[0] != L"NP4RESTORE1") {
		SetError(error, L"The restore journal has an unknown version.");
		return false;
	}
	int itemCount = -1;
	for (const std::wstring &line : lines) {
		if (line.rfind(L"state=", 0) == 0) {
			journal.state = line.substr(6);
		} else if (line.rfind(L"count=", 0) == 0) {
			itemCount = _wtoi(line.c_str() + 6);
		}
	}
	if (journal.state != L"prepared" && journal.state != L"applying" && journal.state != L"committed") {
		SetError(error, L"The restore journal has an invalid state.");
		return false;
	}
	if (itemCount < 0 || itemCount > 64) {
		SetError(error, L"The restore journal has an invalid item count.");
		return false;
	}
	journal.items.assign(static_cast<size_t>(itemCount), {std::wstring(), false});
	for (const std::wstring &line : lines) {
		for (int i = 0; i < itemCount; ++i) {
			const std::wstring targetPrefix = L"target" + std::to_wstring(i) + L"=";
			const std::wstring existedPrefix = L"existed" + std::to_wstring(i) + L"=";
			if (line.rfind(targetPrefix, 0) == 0) {
				journal.items[i].first = line.substr(targetPrefix.size());
			} else if (line.rfind(existedPrefix, 0) == 0) {
				journal.items[i].second = line.substr(existedPrefix.size()) == L"1";
			}
		}
	}
	for (const auto &item : journal.items) {
		if (item.first.empty() || !IsAllowedTargetPath(item.first)) {
			SetError(error, L"The restore journal contains an unsafe target path.");
			return false;
		}
	}
	return true;
}

bool RollbackTransaction(const std::wstring &transactionDir, const JournalData &journal, std::wstring *error) noexcept {
	for (size_t i = journal.items.size(); i-- > 0;) {
		const std::wstring &target = journal.items[i].first;
		if (journal.items[i].second) {
			ByteVector backup;
			std::wstring backupPath = transactionDir + L"\\" + std::to_wstring(i) + L".bin";
			bool exists = false;
			if (!ReadFileBytes(backupPath.c_str(), backup, exists, error) || !exists) {
				SetError(error, L"A restore rollback file is missing.");
				return false;
			}
			if (!WriteFileAtomically(target.c_str(), backup, error)) {
				return false;
			}
		} else {
			if (FileExists(target.c_str()) && !DeleteFileW(target.c_str())) {
				SetError(error, L"Cannot remove a newly restored configuration file.");
				return false;
			}
		}
	}
	return true;
}

bool VerifyTargetBytes(const std::wstring &target, const ByteVector &expected) noexcept {
	ByteVector actual;
	bool exists = false;
	std::wstring ignored;
	if (!ReadFileBytes(target.c_str(), actual, exists, &ignored) || !exists) {
		return false;
	}
	return actual == expected;
}

bool CheckDiskSpace(const std::vector<PlanItem> &plan, std::wstring *error) noexcept {
	uint64_t transactionBytes = 0;
	std::map<std::wstring, uint64_t> targetBytes;
	for (const PlanItem &item : plan) {
		transactionBytes += item.originalData.size() + item.newData.size();
		const DWORD attributes = GetFileAttributesW(item.targetPath.c_str());
		if (attributes != INVALID_FILE_ATTRIBUTES) {
			if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
				SetError(error, L"A configuration target is not a regular file.");
				return false;
			}
			if ((attributes & FILE_ATTRIBUTE_READONLY) != 0) {
				SetError(error, L"A configuration file is read-only.");
				return false;
			}
		}
		WCHAR parent[MAX_PATH];
		lstrcpynW(parent, item.targetPath.c_str(), ARRAYSIZE(parent));
		if (!PathRemoveFileSpecW(parent) || !EnsureDirectory(parent)) {
			SetError(error, L"Cannot create a configuration directory.");
			return false;
		}
		targetBytes[parent] += item.newData.size();
	}
	const std::wstring transactionDir = GetTransactionDirectory();
	if (transactionDir.empty()) {
		SetError(error, L"The restore transaction directory is not available.");
		return false;
	}
	const std::wstring transactionRoot = transactionDir.substr(0, transactionDir.find_last_of(L'\\'));
	if (!EnsureDirectory(transactionRoot.c_str())) {
		SetError(error, L"Cannot create the restore transaction directory.");
		return false;
	}
	ULARGE_INTEGER available {};
	if (!GetDiskFreeSpaceExW(transactionRoot.c_str(), &available, nullptr, nullptr)
		|| available.QuadPart < transactionBytes + 1024 * 1024) {
		SetError(error, L"Not enough free disk space for the restore transaction.");
		return false;
	}
	for (const auto &entry : targetBytes) {
		ULARGE_INTEGER freeBytes {};
		if (!GetDiskFreeSpaceExW(entry.first.c_str(), &freeBytes, nullptr, nullptr)
			|| freeBytes.QuadPart < entry.second + 1024 * 1024) {
			SetError(error, L"Not enough free disk space in the configuration directory.");
			return false;
		}
	}
	return true;
}

bool HasOtherNotepadInstances() noexcept {
	const DWORD currentProcess = GetCurrentProcessId();
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) {
		return false;
	}
	PROCESSENTRY32W entry {};
	entry.dwSize = sizeof(entry);
	bool found = false;
	if (Process32FirstW(snapshot, &entry)) {
		do {
			if (entry.th32ProcessID != currentProcess
				&& _wcsicmp(entry.szExeFile, L"Notepad4.exe") == 0) {
				found = true;
				break;
			}
		} while (Process32NextW(snapshot, &entry));
	}
	CloseHandle(snapshot);
	return found;
}

std::wstring GetRollbackBackupPath() noexcept {
	const std::wstring directory = GetRollbackDirectory();
	if (directory.empty()) {
		return {};
	}
	if (!EnsureDirectory(directory.c_str())) {
		return {};
	}
	const std::wstring base = directory + L"\\PreRestore-" + GetTimestampForFileName();
	std::wstring path = base + L".zip";
	int suffix = 1;
	while (FileExists(path.c_str())) {
		path = base + L"-" + std::to_wstring(suffix++) + L".zip";
	}
	return path;
}

std::wstring GetRestoreMutexName() noexcept {
	std::wstring key = szIniFile;
	if (key.empty()) {
		key = GetNotepad4DataRoot();
	}
	uint32_t hash = 2166136261U;
	for (wchar_t ch : key) {
		hash ^= static_cast<uint32_t>(ch);
		hash *= 16777619U;
	}
	wchar_t name[128];
	swprintf_s(name, L"Local\\Notepad4-TxtSyntax.SettingsRestore.%08X", hash);
	return name;
}

class RestoreMutex {
public:
	RestoreMutex() noexcept {
		const std::wstring name = GetRestoreMutexName();
		handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
		if (handle_ != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
			CloseHandle(handle_);
			handle_ = nullptr;
			busy_ = true;
		}
	}
	~RestoreMutex() noexcept {
		if (handle_ != nullptr) {
			CloseHandle(handle_);
		}
	}
	bool Busy() const noexcept { return busy_; }

private:
	HANDLE handle_ = nullptr;
	bool busy_ = false;
};

bool RecoverTransactionInternal(bool showMessage, std::wstring *error) noexcept {
	const std::wstring transactionDir = GetTransactionDirectory();
	if (transactionDir.empty() || !PathIsDirectoryW(transactionDir.c_str())) {
		return true;
	}
	const std::wstring journalPath = transactionDir + L"\\" + RestoreJournalName;
	if (!FileExists(journalPath.c_str())) {
		// No target is modified before the journal is written.
		if (!DeleteDirectoryTree(transactionDir)) {
			SetError(error, L"Cannot remove an incomplete restore transaction.");
			return false;
		}
		return true;
	}
	JournalData journal;
	if (!ReadJournal(transactionDir, journal, error)) {
		return false;
	}
	if (journal.state == L"committed") {
		if (!DeleteDirectoryTree(transactionDir)) {
			SetError(error, L"Cannot remove a completed restore transaction.");
			return false;
		}
		return true;
	}
	if (!RollbackTransaction(transactionDir, journal, error)) {
		return false;
	}
	if (!DeleteDirectoryTree(transactionDir)) {
		SetError(error, L"Cannot remove a rolled back restore transaction.");
		return false;
	}
	if (showMessage) {
		WCHAR title[128];
		WCHAR message[512];
		LoadStringResource(IDS_RESTORE_TITLE, L"Restore Settings", title, ARRAYSIZE(title));
		LoadStringResource(IDS_RESTORE_RECOVERED,
			L"An interrupted settings restore was rolled back.", message, ARRAYSIZE(message));
		MessageBoxW(nullptr, message, title, MB_OK | MB_ICONWARNING);
	}
	return true;
}

void PruneRollbackSnapshots() noexcept {
	const std::wstring directory = GetRollbackDirectory();
	if (directory.empty() || PathIsDirectoryW(GetTransactionDirectory().c_str())) {
		return;
	}
	int keep = IniGetInt(INI_SECTION_NAME_SETTINGS, L"RestoreRollbackKeep", DefaultRollbackKeep);
	keep = std::clamp(keep, 1, MaxRollbackKeep);
	struct RollbackFile {
		std::wstring path;
		FILETIME modified {};
	};
	std::vector<RollbackFile> files;
	WIN32_FIND_DATAW data {};
	const std::wstring pattern = directory + L"\\PreRestore-*.zip";
	HANDLE find = FindFirstFileW(pattern.c_str(), &data);
	if (find == INVALID_HANDLE_VALUE) {
		return;
	}
	do {
		if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
			files.push_back({ directory + L"\\" + data.cFileName, data.ftLastWriteTime });
		}
	} while (FindNextFileW(find, &data));
	FindClose(find);
	std::sort(files.begin(), files.end(), [](const RollbackFile &a, const RollbackFile &b) {
		return CompareFileTime(&a.modified, &b.modified) > 0;
	});
	for (size_t i = static_cast<size_t>(keep); i < files.size(); ++i) {
		DeleteFileW(files[i].path.c_str());
	}
}

bool ApplyPlan(const std::vector<PlanItem> &plan,
	ConfigRestoreResult *result, std::wstring *error) noexcept {
	if (HasOtherNotepadInstances()) {
		SetError(error, L"Close other Notepad4 windows before restoring settings.");
		return false;
	}
	const std::wstring transactionDir = GetTransactionDirectory();
	if (transactionDir.empty()) {
		SetError(error, L"The restore transaction directory is not available.");
		return false;
	}
	if (PathIsDirectoryW(transactionDir.c_str())) {
		if (!RecoverTransactionInternal(false, error)) {
			SetError(error, L"A previous restore transaction could not be recovered.");
			return false;
		}
	}
	if (!CheckDiskSpace(plan, error)) {
		return false;
	}

	std::wstring rollbackPath;
	const bool currentMainExists = FileExists(szIniFile);
	if (currentMainExists) {
		rollbackPath = GetRollbackBackupPath();
		if (rollbackPath.empty()) {
			SetError(error, L"Cannot allocate a rollback snapshot path.");
			return false;
		}
		ConfigBackupOptions options;
		options.includeHistory = true;
		ConfigBackupResult backupResult;
		if (!ConfigBackup_Create(rollbackPath.c_str(), options, &backupResult, error)) {
			SetError(error, L"Cannot create the pre-restore rollback snapshot.");
			return false;
		}
	}

	std::error_code ec;
	fs::create_directories(fs::path(transactionDir), ec);
	if (ec) {
		SetError(error, L"Cannot create the restore transaction directory.");
		return false;
	}
	JournalData journal;
	journal.state = L"prepared";
	for (const PlanItem &item : plan) {
		journal.items.emplace_back(item.targetPath, item.originalExists);
	}
	for (size_t i = 0; i < plan.size(); ++i) {
		if (plan[i].originalExists) {
			const std::wstring backupPath = transactionDir + L"\\" + std::to_wstring(i) + L".bin";
			if (!WriteFileAtomically(backupPath.c_str(), plan[i].originalData, error)) {
				DeleteDirectoryTree(transactionDir);
				return false;
			}
		}
		const std::wstring newPath = transactionDir + L"\\" + std::to_wstring(i) + L".new";
		if (!WriteFileAtomically(newPath.c_str(), plan[i].newData, error)) {
			DeleteDirectoryTree(transactionDir);
			return false;
		}
	}
	if (!WriteJournal(transactionDir, L"prepared", journal.items, error)
		|| !WriteJournal(transactionDir, L"applying", journal.items, error)) {
		DeleteDirectoryTree(transactionDir);
		return false;
	}

	for (const PlanItem &item : plan) {
		if (GetFileAttributesW(item.targetPath.c_str()) != INVALID_FILE_ATTRIBUTES
			&& (GetFileAttributesW(item.targetPath.c_str()) & FILE_ATTRIBUTE_READONLY) != 0) {
			SetError(error, L"A configuration file is read-only.");
			if (RollbackTransaction(transactionDir, journal, error)) {
				DeleteDirectoryTree(transactionDir);
			}
			return false;
		}
		if (!WriteFileAtomically(item.targetPath.c_str(), item.newData, error)) {
			if (RollbackTransaction(transactionDir, journal, error)) {
				DeleteDirectoryTree(transactionDir);
			}
			return false;
		}
		if (!VerifyTargetBytes(item.targetPath, item.newData)) {
			SetError(error, L"A restored configuration file failed verification.");
			if (RollbackTransaction(transactionDir, journal, error)) {
				DeleteDirectoryTree(transactionDir);
			}
			return false;
		}
	}
	if (!WriteJournal(transactionDir, L"committed", journal.items, error)) {
		std::wstring rollbackError;
		if (RollbackTransaction(transactionDir, journal, &rollbackError)) {
			DeleteDirectoryTree(transactionDir);
		}
		SetError(error, rollbackError.empty()
			? L"The restored configuration could not be committed and was rolled back."
			: L"The restored configuration could not be committed or rolled back.");
		return false;
	}
	// The commit point is the committed journal. Cleanup is best-effort;
	// startup recovery removes a stale committed transaction.
	DeleteDirectoryTree(transactionDir);
	PruneRollbackSnapshots();
	if (result != nullptr) {
		result->rollbackBackupPath = rollbackPath;
		result->filesApplied = static_cast<int>(plan.size());
		result->legacy = false;
	}
	return true;
}

} // namespace

bool ConfigRestore_Load(LPCWSTR archivePath,
	ConfigArchivePackage *package,
	std::wstring *error) noexcept {
	return ConfigArchive_Read(archivePath, package, error);
}

bool ConfigRestore_BuildPreview(const ConfigArchivePackage &package,
	uint32_t categories,
	ConfigRestorePreview *preview,
	std::wstring *error) noexcept {
	if (preview == nullptr) {
		SetError(error, L"A preview result is required.");
		return false;
	}
	*preview = {};
	std::vector<PlanItem> plan;
	return BuildPlan(package, categories, plan, *preview, error);
}

bool ConfigRestore_Apply(const ConfigArchivePackage &package,
	uint32_t categories,
	LPCWSTR archivePath,
	ConfigRestoreResult *result,
	std::wstring *error) noexcept {
	if (error != nullptr) {
		error->clear();
	}
	if (result != nullptr) {
		*result = {};
	}
	if (archivePath == nullptr || archivePath[0] == L'\0') {
		SetError(error, L"The source backup path is required.");
		return false;
	}
	RestoreMutex mutex;
	if (mutex.Busy()) {
		SetError(error, L"Another configuration restore is already running.");
		return false;
	}
	try {
		std::vector<PlanItem> plan;
		ConfigRestorePreview preview;
		if (!BuildPlan(package, categories, plan, preview, error)) {
			return false;
		}
		if (plan.empty()) {
			if (result != nullptr) {
				result->legacy = package.format != ConfigArchiveFormat::Current;
			}
			return true;
		}
		if (!ApplyPlan(plan, result, error)) {
			return false;
		}
		if (result != nullptr) {
			result->legacy = package.format != ConfigArchiveFormat::Current;
		}
		return true;
	} catch (const std::exception &) {
		SetError(error, L"Not enough memory to apply the configuration restore.");
	} catch (...) {
		SetError(error, L"Unexpected failure while applying the configuration restore.");
	}
	return false;
}

void ConfigRestore_RecoverPendingTransaction() noexcept {
	const std::wstring transactionDir = GetTransactionDirectory();
	if (transactionDir.empty() || !PathIsDirectoryW(transactionDir.c_str())) {
		return;
	}
	RestoreMutex mutex;
	if (mutex.Busy()) {
		return;
	}
	std::wstring error;
	if (!RecoverTransactionInternal(true, &error)) {
		WCHAR title[128];
		WCHAR format[512];
		WCHAR message[1600];
		LoadStringResource(IDS_RESTORE_TITLE, L"Restore Settings", title, ARRAYSIZE(title));
		LoadStringResource(IDS_RESTORE_RECOVERY_FAILED,
			L"An interrupted settings restore could not be rolled back.\n\n%s",
			format, ARRAYSIZE(format));
		if (swprintf_s(message, format, error.c_str()) < 0) {
			lstrcpynW(message, error.c_str(), ARRAYSIZE(message));
		}
		MessageBoxW(nullptr, message, title, MB_OK | MB_ICONERROR);
	}
}
