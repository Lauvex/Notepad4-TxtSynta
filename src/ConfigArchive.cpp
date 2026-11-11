/******************************************************************************
*
* Notepad4-TxtSyntax
*
* ConfigArchive.cpp
*   Versioned settings archive writer and archive format inspection.
*
******************************************************************************/

#include "ConfigArchive.h"

#include <bcrypt.h>
#include <shlwapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <utility>

namespace {

constexpr uint64_t ConfigArchive_MaxConfigFileSize = 16ULL * 1024 * 1024;
constexpr uint64_t ConfigArchive_MaxArchiveSize = 128ULL * 1024 * 1024;
constexpr size_t ConfigArchive_MaxEntries = 4096;
constexpr WORD ZipFlagUtf8 = 0x0800;
constexpr WORD ZipVersion = 20;
constexpr WORD ZipDosDate = 0x0021; // 1980-01-01, fixed for reproducible archives.
constexpr WORD ZipDosTime = 0;

void SetError(std::wstring *error, const wchar_t *message) noexcept {
	if (error != nullptr) {
		*error = message;
	}
}

void SetError(std::wstring *error, const wchar_t *message, DWORD code) noexcept {
	if (error == nullptr) {
		return;
	}
	*error = message;
	*error += L" (";
	*error += std::to_wstring(code);
	*error += L")";
}

void SetError(std::wstring *error, const wchar_t *message, NTSTATUS status) noexcept {
	if (error == nullptr) {
		return;
	}
	*error = message;
	wchar_t buffer[32];
	swprintf_s(buffer, L" (0x%08lX)", static_cast<unsigned long>(status));
	*error += buffer;
}

std::string ToUtf8(const std::wstring &text) noexcept {
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

void AppendJsonString(std::string &json, const std::string &text) noexcept {
	json.push_back('"');
	for (const unsigned char ch : text) {
		switch (ch) {
		case '"':
			json += "\\\"";
			break;
		case '\\':
			json += "\\\\";
			break;
		case '\b':
			json += "\\b";
			break;
		case '\f':
			json += "\\f";
			break;
		case '\n':
			json += "\\n";
			break;
		case '\r':
			json += "\\r";
			break;
		case '\t':
			json += "\\t";
			break;
		default:
			if (ch < 0x20) {
				char escaped[7];
				sprintf_s(escaped, "\\u%04X", ch);
				json += escaped;
			} else {
				json.push_back(static_cast<char>(ch));
			}
			break;
		}
	}
	json.push_back('"');
}

bool Sha256(const uint8_t *data, size_t size, std::array<uint8_t, 32> &digest, std::wstring *error) noexcept {
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	DWORD objectLength = 0;
	DWORD bytesWritten = 0;

	NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
	if (!BCRYPT_SUCCESS(status)) {
		SetError(error, L"Cannot open the SHA-256 provider.", status);
		return false;
	}
	status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &bytesWritten, 0);
	if (!BCRYPT_SUCCESS(status) || objectLength == 0) {
		BCryptCloseAlgorithmProvider(algorithm, 0);
		SetError(error, L"Cannot query the SHA-256 object size.", status);
		return false;
	}

	std::vector<uint8_t> object(objectLength);
	status = BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0);
	if (BCRYPT_SUCCESS(status) && size != 0) {
		status = BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0);
	}
	if (BCRYPT_SUCCESS(status)) {
		status = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
	}
	if (hash != nullptr) {
		BCryptDestroyHash(hash);
	}
	BCryptCloseAlgorithmProvider(algorithm, 0);

	if (!BCRYPT_SUCCESS(status)) {
		SetError(error, L"Cannot calculate SHA-256.", status);
		return false;
	}
	return true;
}

std::string HexString(const uint8_t *data, size_t size) noexcept {
	static constexpr char digits[] = "0123456789abcdef";
	std::string result;
	result.resize(size * 2);
	for (size_t i = 0; i < size; ++i) {
		result[i * 2] = digits[data[i] >> 4];
		result[i * 2 + 1] = digits[data[i] & 0x0F];
	}
	return result;
}

const std::array<uint32_t, 256> &Crc32Table() noexcept {
	static const std::array<uint32_t, 256> table = [] {
		std::array<uint32_t, 256> values {};
		for (uint32_t i = 0; i < 256; ++i) {
			uint32_t crc = i;
			for (int bit = 0; bit < 8; ++bit) {
				crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320U : 0U);
			}
			values[i] = crc;
		}
		return values;
	}();
	return table;
}

uint32_t Crc32(const std::vector<uint8_t> &data) noexcept {
	uint32_t crc = 0xFFFFFFFFU;
	const auto &table = Crc32Table();
	for (const uint8_t value : data) {
		crc = (crc >> 8) ^ table[(crc ^ value) & 0xFF];
	}
	return crc ^ 0xFFFFFFFFU;
}

void Append16(std::vector<uint8_t> &buffer, uint16_t value) noexcept {
	buffer.push_back(static_cast<uint8_t>(value));
	buffer.push_back(static_cast<uint8_t>(value >> 8));
}

void Append32(std::vector<uint8_t> &buffer, uint32_t value) noexcept {
	buffer.push_back(static_cast<uint8_t>(value));
	buffer.push_back(static_cast<uint8_t>(value >> 8));
	buffer.push_back(static_cast<uint8_t>(value >> 16));
	buffer.push_back(static_cast<uint8_t>(value >> 24));
}

uint16_t Read16(const uint8_t *data) noexcept {
	return static_cast<uint16_t>(data[0] | (static_cast<uint16_t>(data[1]) << 8));
}

uint32_t Read32(const uint8_t *data) noexcept {
	return static_cast<uint32_t>(data[0])
		| (static_cast<uint32_t>(data[1]) << 8)
		| (static_cast<uint32_t>(data[2]) << 16)
		| (static_cast<uint32_t>(data[3]) << 24);
}

bool ReadWholeFile(LPCWSTR path, uint64_t maxSize, std::vector<uint8_t> &data, std::wstring *error) noexcept {
	data.clear();
	const DWORD attributes = GetFileAttributesW(path);
	if (attributes == INVALID_FILE_ATTRIBUTES) {
		SetError(error, L"The source file does not exist.", GetLastError());
		return false;
	}
	if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
		SetError(error, L"The source is not a regular file.");
		return false;
	}

	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		SetError(error, L"Cannot open the source file.", GetLastError());
		return false;
	}

	LARGE_INTEGER size {};
	if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || static_cast<uint64_t>(size.QuadPart) > maxSize) {
		const DWORD code = GetLastError();
		CloseHandle(file);
		if (size.QuadPart > 0 && static_cast<uint64_t>(size.QuadPart) > maxSize) {
			SetError(error, L"The source file is too large.");
		} else {
			SetError(error, L"Cannot query the source file size.", code);
		}
		return false;
	}

	data.resize(static_cast<size_t>(size.QuadPart));
	size_t offset = 0;
	while (offset < data.size()) {
		const DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - offset, 1U << 20));
		DWORD read = 0;
		if (!ReadFile(file, data.data() + offset, chunk, &read, nullptr) || read == 0) {
			const DWORD code = GetLastError();
			CloseHandle(file);
			data.clear();
			SetError(error, L"Cannot read the source file.", code);
			return false;
		}
		offset += read;
	}
	CloseHandle(file);
	return true;
}

bool WriteAll(HANDLE file, const void *data, size_t size) noexcept {
	const uint8_t *cursor = static_cast<const uint8_t *>(data);
	while (size > 0) {
		const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size, 1U << 20));
		DWORD written = 0;
		if (!WriteFile(file, cursor, chunk, &written, nullptr) || written != chunk) {
			return false;
		}
		cursor += chunk;
		size -= chunk;
	}
	return true;
}

bool ValidArchivePath(const std::string &path) noexcept {
	if (path.empty() || path.size() > 255 || path.back() == '/') {
		return false;
	}
	if (path.find('\\') != std::string::npos || path.find(':') != std::string::npos) {
		return false;
	}
	for (const unsigned char ch : path) {
		if (ch < 0x20 || ch == 0x7F) {
			return false;
		}
	}
	size_t start = 0;
	while (start < path.size()) {
		const size_t end = path.find('/', start);
		const size_t length = (end == std::string::npos ? path.size() : end) - start;
		if (length == 0 || (length == 1 && path[start] == '.')
			|| (length == 2 && path[start] == '.' && path[start + 1] == '.')) {
			return false;
		}
		if (path[start + length - 1] == ' ' || path[start + length - 1] == '.') {
			return false;
		}
		if (end == std::string::npos) {
			break;
		}
		start = end + 1;
	}
	return true;
}

struct PreparedEntry {
	std::string name;
	std::string category;
	std::vector<uint8_t> data;
	uint32_t crc = 0;
	std::array<uint8_t, 32> digest {};
};

std::string BuildManifest(const ConfigArchiveMetadata &metadata, const std::vector<PreparedEntry> &entries) noexcept {
	std::string json;
	json.reserve(4096);
	json += "{\n";
	json += "  \"schema\": \"notepad4-txtsyntax.settings\",\n";
	json += "  \"formatVersion\": " + std::to_string(ConfigArchive_FormatVersion) + ",\n";
	json += "  \"minimumReaderVersion\": " + std::to_string(ConfigArchive_MinimumReaderVersion) + ",\n";
	json += "  \"application\": {\n";
	json += "    \"name\": \"Notepad4-TxtSyntax\",\n";
	json += "    \"version\": ";
	AppendJsonString(json, metadata.appVersion);
	json += ",\n    \"build\": ";
	AppendJsonString(json, metadata.buildId);
	json += "\n  },\n";
	json += "  \"createdAtUtc\": ";
	AppendJsonString(json, metadata.createdAtUtc);
	json += ",\n  \"deviceId\": ";
	AppendJsonString(json, metadata.deviceId);
	json += ",\n  \"backupId\": ";
	AppendJsonString(json, metadata.backupId);
	json += ",\n  \"historyIncluded\": ";
	json += metadata.includeHistory ? "true" : "false";
	json += ",\n  \"categories\": [";
	for (size_t i = 0; i < metadata.categories.size(); ++i) {
		if (i != 0) {
			json += ", ";
		}
		AppendJsonString(json, metadata.categories[i]);
	}
	json += "],\n  \"files\": [\n";
	for (size_t i = 0; i < entries.size(); ++i) {
		const PreparedEntry &entry = entries[i];
		json += "    {\n      \"path\": ";
		AppendJsonString(json, entry.name);
		json += ",\n      \"category\": ";
		AppendJsonString(json, entry.category);
		json += ",\n      \"size\": " + std::to_string(entry.data.size()) + ",\n      \"sha256\": \"";
		json += HexString(entry.digest.data(), entry.digest.size());
		json += "\"\n    }";
		if (i + 1 != entries.size()) {
			json += ",";
		}
		json += "\n";
	}
	json += "  ],\n  \"warnings\": [";
	for (size_t i = 0; i < metadata.warnings.size(); ++i) {
		if (i != 0) {
			json += ", ";
		}
		AppendJsonString(json, metadata.warnings[i]);
	}
	json += "],\n  \"compatibility\": {\n";
	json += "    \"legacyFormatDetection\": true,\n";
	json += "    \"manifestRequiredForIntegrityChecks\": true\n";
	json += "  }\n}\n";
	return json;
}

bool WriteZipFile(LPCWSTR tempPath, const std::vector<PreparedEntry> &entries, std::wstring *error) noexcept {
	HANDLE file = CreateFileW(tempPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_TEMPORARY, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		SetError(error, L"Cannot create the temporary archive.", GetLastError());
		return false;
	}

	bool success = true;
	uint64_t position = 0;
	std::vector<uint32_t> offsets;
	offsets.reserve(entries.size());

	for (const PreparedEntry &entry : entries) {
		offsets.push_back(static_cast<uint32_t>(position));
		std::vector<uint8_t> header;
		header.reserve(30);
		Append32(header, 0x04034B50);
		Append16(header, ZipVersion);
		Append16(header, ZipFlagUtf8);
		Append16(header, 0);
		Append16(header, ZipDosTime);
		Append16(header, ZipDosDate);
		Append32(header, entry.crc);
		Append32(header, static_cast<uint32_t>(entry.data.size()));
		Append32(header, static_cast<uint32_t>(entry.data.size()));
		Append16(header, static_cast<uint16_t>(entry.name.size()));
		Append16(header, 0);
		if (!WriteAll(file, header.data(), header.size())
			|| !WriteAll(file, entry.name.data(), entry.name.size())
			|| !WriteAll(file, entry.data.data(), entry.data.size())) {
			SetError(error, L"Cannot write the temporary archive.", GetLastError());
			success = false;
			break;
		}
		position += header.size() + entry.name.size() + entry.data.size();
	}

	const uint32_t centralStart = static_cast<uint32_t>(position);
	if (success) {
		for (size_t i = 0; i < entries.size(); ++i) {
			const PreparedEntry &entry = entries[i];
			std::vector<uint8_t> header;
			header.reserve(46);
			Append32(header, 0x02014B50);
			Append16(header, ZipVersion);
			Append16(header, ZipVersion);
			Append16(header, ZipFlagUtf8);
			Append16(header, 0);
			Append16(header, ZipDosTime);
			Append16(header, ZipDosDate);
			Append32(header, entry.crc);
			Append32(header, static_cast<uint32_t>(entry.data.size()));
			Append32(header, static_cast<uint32_t>(entry.data.size()));
			Append16(header, static_cast<uint16_t>(entry.name.size()));
			Append16(header, 0);
			Append16(header, 0);
			Append16(header, 0);
			Append16(header, 0);
			Append32(header, 0);
			Append32(header, offsets[i]);
			if (!WriteAll(file, header.data(), header.size())
				|| !WriteAll(file, entry.name.data(), entry.name.size())) {
				SetError(error, L"Cannot write the temporary archive.", GetLastError());
				success = false;
				break;
			}
			position += header.size() + entry.name.size();
		}
	}

	if (success) {
		if (position > std::numeric_limits<uint32_t>::max()) {
			SetError(error, L"The archive is too large for the ZIP32 format.");
			success = false;
		} else {
			std::vector<uint8_t> footer;
			footer.reserve(22);
			Append32(footer, 0x06054B50);
			Append16(footer, 0);
			Append16(footer, 0);
			Append16(footer, static_cast<uint16_t>(entries.size()));
			Append16(footer, static_cast<uint16_t>(entries.size()));
			Append32(footer, static_cast<uint32_t>(position - centralStart));
			Append32(footer, centralStart);
			Append16(footer, 0);
			if (!WriteAll(file, footer.data(), footer.size())) {
				SetError(error, L"Cannot write the ZIP end record.", GetLastError());
				success = false;
			}
		}
	}

	if (success && !FlushFileBuffers(file)) {
		SetError(error, L"Cannot flush the temporary archive.", GetLastError());
		success = false;
	}
	CloseHandle(file);
	if (!success) {
		DeleteFileW(tempPath);
	}
	return success;
}

bool PrepareEntries(const std::vector<ConfigArchiveEntry> &entries,
	std::vector<PreparedEntry> &prepared,
	std::wstring *error) noexcept {
	uint64_t totalSize = 0;
	prepared.clear();
	prepared.reserve(entries.size());
	for (const ConfigArchiveEntry &entry : entries) {
		if (!ValidArchivePath(entry.archivePath)) {
			SetError(error, L"An archive entry name is invalid.");
			return false;
		}
		if (entry.category.empty() || entry.archivePath == "manifest.json") {
			SetError(error, L"An archive entry category is invalid.");
			return false;
		}
		if (std::any_of(prepared.begin(), prepared.end(), [&](const PreparedEntry &item) {
			return _stricmp(item.name.c_str(), entry.archivePath.c_str()) == 0;
		})) {
			SetError(error, L"Duplicate archive entry names are not allowed.");
			return false;
		}

		PreparedEntry item;
		item.name = entry.archivePath;
		item.category = entry.category;
		if (entry.hasInlineData) {
			item.data = entry.inlineData;
		} else if (!ReadWholeFile(entry.sourcePath.c_str(), ConfigArchive_MaxConfigFileSize, item.data, error)) {
			return false;
		}
		if (item.data.size() > ConfigArchive_MaxConfigFileSize
			|| totalSize + item.data.size() > ConfigArchive_MaxArchiveSize) {
			SetError(error, L"The configuration files are too large for a settings archive.");
			return false;
		}
		totalSize += item.data.size();
		item.crc = Crc32(item.data);
		if (!Sha256(item.data.data(), item.data.size(), item.digest, error)) {
			return false;
		}
		prepared.push_back(std::move(item));
	}
	return true;
}

bool IsLegacyName(const std::string &name) noexcept {
	static constexpr const char *names[] = {
		"Notepad4.ini",
		"Notepad4 DarkTheme.ini",
		"Notepad4 Solarized Light.ini",
		"Notepad4 Tango Warm Dark.ini",
		"Notepad4 Gruvbox Dark.ini",
		"Notepad4 Gruvbox Light.ini",
	};
	for (const char *candidate : names) {
		if (name == candidate) {
			return true;
		}
	}
	return false;
}

constexpr uint64_t ConfigArchive_MaxExtractedSize = 64ULL * 1024 * 1024;
constexpr uint64_t ConfigArchive_MaxManifestSize = 1024ULL * 1024;
constexpr size_t ConfigArchive_MaxManifestFiles = 64;

static constexpr const char *LegacyNames[] = {
	"Notepad4.ini",
	"Notepad4 DarkTheme.ini",
	"Notepad4 Solarized Light.ini",
	"Notepad4 Tango Warm Dark.ini",
	"Notepad4 Gruvbox Dark.ini",
	"Notepad4 Gruvbox Light.ini",
};

static constexpr const char *ThemeArchivePaths[] = {
	"config/Notepad4 DarkTheme.ini",
	"config/Notepad4 Solarized Light.ini",
	"config/Notepad4 Tango Warm Dark.ini",
	"config/Notepad4 Gruvbox Dark.ini",
	"config/Notepad4 Gruvbox Light.ini",
};

bool IsAllowedConfigPath(const std::string &path) noexcept {
	if (path == "config/Notepad4.ini" || path == "config/matepath.ini") {
		return true;
	}
	for (const char *candidate : ThemeArchivePaths) {
		if (path == candidate) {
			return true;
		}
	}
	return false;
}

bool IsKnownCategory(const std::string &category) noexcept {
	return category == "settings" || category == "themes" || category == "syntax"
		|| category == "matepath" || category == "history";
}

bool IsHexSha256(const std::string &text) noexcept {
	if (text.size() != 64) {
		return false;
	}
	for (const unsigned char ch : text) {
		if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) {
			return false;
		}
	}
	return true;
}

struct ZipEntryInfo {
	std::string name;
	uint16_t flags = 0;
	uint16_t method = 0;
	uint32_t crc = 0;
	uint32_t compressedSize = 0;
	uint32_t uncompressedSize = 0;
	uint32_t localOffset = 0;
	uint32_t externalAttributes = 0;
};

bool ReadZipCentralDirectory(const std::vector<uint8_t> &data,
	std::vector<ZipEntryInfo> &entries,
	std::wstring *error) noexcept {
	entries.clear();
	if (data.size() < 22) {
		SetError(error, L"The file is too small to be a ZIP archive.");
		return false;
	}
	size_t eocd = SIZE_MAX;
	const size_t searchStart = data.size() >= 22 + 0xFFFF ? data.size() - 22 - 0xFFFF : 0;
	for (size_t pos = data.size() - 22; pos >= searchStart; --pos) {
		if (Read32(data.data() + pos) == 0x06054B50
			&& pos + 22 + Read16(data.data() + pos + 20) == data.size()) {
			eocd = pos;
			break;
		}
		if (pos == 0) {
			break;
		}
	}
	if (eocd == SIZE_MAX) {
		SetError(error, L"ZIP end-of-central-directory record not found.");
		return false;
	}

	const uint16_t disk = Read16(data.data() + eocd + 4);
	const uint16_t centralDisk = Read16(data.data() + eocd + 6);
	const uint16_t entriesOnDisk = Read16(data.data() + eocd + 8);
	const uint16_t entryCount = Read16(data.data() + eocd + 10);
	const uint32_t centralSize = Read32(data.data() + eocd + 12);
	const uint32_t centralOffset = Read32(data.data() + eocd + 16);
	if (disk != 0 || centralDisk != 0 || entriesOnDisk != entryCount) {
		SetError(error, L"Multi-disk ZIP archives are not supported.");
		return false;
	}
	if (entryCount > ConfigArchive_MaxEntries) {
		SetError(error, L"The archive contains too many entries.");
		return false;
	}
	if (static_cast<uint64_t>(centralOffset) + centralSize > eocd) {
		SetError(error, L"The ZIP central directory is outside the file.");
		return false;
	}

	size_t pos = centralOffset;
	uint64_t totalUncompressed = 0;
	for (uint16_t i = 0; i < entryCount; ++i) {
		if (pos + 46 > data.size() || Read32(data.data() + pos) != 0x02014B50) {
			SetError(error, L"The ZIP central directory is malformed.");
			return false;
		}
		const uint16_t flags = Read16(data.data() + pos + 8);
		const uint16_t method = Read16(data.data() + pos + 10);
		const uint32_t crc = Read32(data.data() + pos + 16);
		const uint32_t compressedSize = Read32(data.data() + pos + 20);
		const uint32_t uncompressedSize = Read32(data.data() + pos + 24);
		const uint16_t nameLength = Read16(data.data() + pos + 28);
		const uint16_t extraLength = Read16(data.data() + pos + 30);
		const uint16_t commentLength = Read16(data.data() + pos + 32);
		const uint32_t externalAttributes = Read32(data.data() + pos + 38);
		const uint32_t localOffset = Read32(data.data() + pos + 42);
		const size_t next = pos + 46ULL + nameLength + extraLength + commentLength;
		if (nameLength == 0 || next > data.size() || localOffset + 30ULL > data.size()) {
			SetError(error, L"A ZIP central directory entry is truncated.");
			return false;
		}
		std::string name(reinterpret_cast<const char *>(data.data() + pos + 46), nameLength);
		if (!ValidArchivePath(name)) {
			SetError(error, L"A ZIP entry has an unsafe path.");
			return false;
		}
		if ((flags & ~ZipFlagUtf8) != 0 || method != 0 || (externalAttributes & 0x10) != 0) {
			SetError(error, L"The archive uses an unsupported ZIP entry type.");
			return false;
		}
		if (((externalAttributes >> 16) & 0xF000) == 0xA000) {
			SetError(error, L"Symbolic links are not allowed in settings archives.");
			return false;
		}
		if (uncompressedSize > ConfigArchive_MaxConfigFileSize
			|| compressedSize > ConfigArchive_MaxArchiveSize
			|| totalUncompressed + uncompressedSize > ConfigArchive_MaxExtractedSize) {
			SetError(error, L"The archive expands beyond the allowed settings size.");
			return false;
		}
		if (std::any_of(entries.begin(), entries.end(), [&](const ZipEntryInfo &entry) {
			return _stricmp(entry.name.c_str(), name.c_str()) == 0;
		})) {
			SetError(error, L"The archive contains duplicate file names.");
			return false;
		}
		totalUncompressed += uncompressedSize;
		entries.push_back({std::move(name), flags, method, crc, compressedSize, uncompressedSize, localOffset, externalAttributes});
		pos = next;
	}
	return true;
}

bool ExtractStoredEntry(const std::vector<uint8_t> &data, const ZipEntryInfo &entry,
	std::vector<uint8_t> &output, std::wstring *error) noexcept {
	output.clear();
	const size_t local = entry.localOffset;
	if (local + 30 > data.size() || Read32(data.data() + local) != 0x04034B50) {
		SetError(error, L"A ZIP local file header is missing or malformed.");
		return false;
	}
	const uint16_t localFlags = Read16(data.data() + local + 6);
	const uint16_t localMethod = Read16(data.data() + local + 8);
	const uint32_t localCrc = Read32(data.data() + local + 14);
	const uint32_t localCompressed = Read32(data.data() + local + 18);
	const uint32_t localUncompressed = Read32(data.data() + local + 22);
	const uint16_t nameLength = Read16(data.data() + local + 26);
	const uint16_t extraLength = Read16(data.data() + local + 28);
	if (localFlags != entry.flags || localMethod != entry.method
		|| localCrc != entry.crc || localCompressed != entry.compressedSize
		|| localUncompressed != entry.uncompressedSize
		|| local + 30ULL + nameLength + extraLength > data.size()) {
		SetError(error, L"A ZIP local file header does not match its directory entry.");
		return false;
	}
	const std::string localName(reinterpret_cast<const char *>(data.data() + local + 30), nameLength);
	if (localName != entry.name) {
		SetError(error, L"A ZIP local file name does not match its directory entry.");
		return false;
	}
	const uint64_t start = local + 30ULL + nameLength + extraLength;
	if (start + entry.compressedSize > data.size() || entry.compressedSize != entry.uncompressedSize) {
		SetError(error, L"A ZIP file entry is truncated or uses unsupported compression.");
		return false;
	}
	output.assign(data.begin() + static_cast<size_t>(start),
		data.begin() + static_cast<size_t>(start + entry.compressedSize));
	if (Crc32(output) != entry.crc) {
		SetError(error, L"A ZIP file failed its CRC check.");
		output.clear();
		return false;
	}
	return true;
}

struct JsonValue {
	enum class Type {
		Null,
		Bool,
		Number,
		String,
		Array,
		Object,
	};

	Type type = Type::Null;
	bool boolValue = false;
	double numberValue = 0;
	std::string stringValue;
	std::string objectKey;
	std::vector<JsonValue> arrayValue;
	std::vector<JsonValue> objectValue;

	const JsonValue *Find(const char *key) const noexcept {
		for (const auto &item : objectValue) {
			if (item.objectKey == key) {
				return &item;
			}
		}
		return nullptr;
	}
};

class JsonParser {
public:
	JsonParser(const std::string &text) noexcept : text_(text) {}

	bool Parse(JsonValue &value) noexcept {
		if (!ParseValue(value, 0)) {
			return false;
		}
		SkipWhitespace();
		return pos_ == text_.size();
	}

private:
	void SkipWhitespace() noexcept {
		while (pos_ < text_.size()) {
			const char ch = text_[pos_];
			if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
				break;
			}
			++pos_;
		}
	}

	bool Consume(char ch) noexcept {
		SkipWhitespace();
		if (pos_ < text_.size() && text_[pos_] == ch) {
			++pos_;
			return true;
		}
		return false;
	}

	bool ParseValue(JsonValue &value, int depth) noexcept {
		if (depth > 32) {
			return false;
		}
		SkipWhitespace();
		if (pos_ >= text_.size()) {
			return false;
		}
		switch (text_[pos_]) {
		case '{':
			return ParseObject(value, depth + 1);
		case '[':
			return ParseArray(value, depth + 1);
		case '"':
			value.type = JsonValue::Type::String;
			return ParseString(value.stringValue);
		case 't':
			if (text_.compare(pos_, 4, "true") == 0) {
				pos_ += 4;
				value.type = JsonValue::Type::Bool;
				value.boolValue = true;
				return true;
			}
			return false;
		case 'f':
			if (text_.compare(pos_, 5, "false") == 0) {
				pos_ += 5;
				value.type = JsonValue::Type::Bool;
				value.boolValue = false;
				return true;
			}
			return false;
		case 'n':
			if (text_.compare(pos_, 4, "null") == 0) {
				pos_ += 4;
				value.type = JsonValue::Type::Null;
				return true;
			}
			return false;
		default:
			return ParseNumber(value);
		}
	}

	bool ParseObject(JsonValue &value, int depth) noexcept {
		if (!Consume('{')) {
			return false;
		}
		value.type = JsonValue::Type::Object;
		SkipWhitespace();
		if (pos_ < text_.size() && text_[pos_] == '}') {
			++pos_;
			return true;
		}
		while (pos_ < text_.size()) {
			std::string key;
			if (!ParseString(key)) {
				return false;
			}
			if (value.Find(key.c_str()) != nullptr || value.objectValue.size() >= 128) {
				return false;
			}
			if (!Consume(':')) {
				return false;
			}
			JsonValue child;
			if (!ParseValue(child, depth)) {
				return false;
			}
			child.objectKey = std::move(key);
			value.objectValue.push_back(std::move(child));
			SkipWhitespace();
			if (pos_ < text_.size() && text_[pos_] == ',') {
				++pos_;
				continue;
			}
			return Consume('}');
		}
		return false;
	}

	bool ParseArray(JsonValue &value, int depth) noexcept {
		if (!Consume('[')) {
			return false;
		}
		value.type = JsonValue::Type::Array;
		SkipWhitespace();
		if (pos_ < text_.size() && text_[pos_] == ']') {
			++pos_;
			return true;
		}
		while (pos_ < text_.size()) {
			if (value.arrayValue.size() >= 1024) {
				return false;
			}
			JsonValue child;
			if (!ParseValue(child, depth)) {
				return false;
			}
			value.arrayValue.push_back(std::move(child));
			SkipWhitespace();
			if (pos_ < text_.size() && text_[pos_] == ',') {
				++pos_;
				continue;
			}
			return Consume(']');
		}
		return false;
	}

	bool ParseNumber(JsonValue &value) noexcept {
		const size_t start = pos_;
		if (pos_ < text_.size() && text_[pos_] == '-') {
			++pos_;
		}
		if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
			return false;
		}
		if (text_[pos_] == '0') {
			++pos_;
		} else {
			while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
				++pos_;
			}
		}
		if (pos_ < text_.size() && text_[pos_] == '.') {
			++pos_;
			const size_t fractionStart = pos_;
			while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
				++pos_;
			}
			if (pos_ == fractionStart) {
				return false;
			}
		}
		if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
			++pos_;
			if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
				++pos_;
			}
			const size_t exponentStart = pos_;
			while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
				++pos_;
			}
			if (pos_ == exponentStart) {
				return false;
			}
		}
		const std::string number = text_.substr(start, pos_ - start);
		char *end = nullptr;
		value.numberValue = std::strtod(number.c_str(), &end);
		if (end == nullptr || *end != '\0' || !std::isfinite(value.numberValue)) {
			return false;
		}
		value.type = JsonValue::Type::Number;
		return true;
	}

	static void AppendUtf8(std::string &text, uint32_t codepoint) noexcept {
		if (codepoint <= 0x7F) {
			text.push_back(static_cast<char>(codepoint));
		} else if (codepoint <= 0x7FF) {
			text.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
			text.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
		} else if (codepoint <= 0xFFFF) {
			text.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
			text.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
			text.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
		} else {
			text.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
			text.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
			text.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
			text.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
		}
	}

	bool ParseHex4(uint32_t &value) noexcept {
		if (pos_ + 4 > text_.size()) {
			return false;
		}
		value = 0;
		for (int i = 0; i < 4; ++i) {
			const char ch = text_[pos_++];
			value <<= 4;
			if (ch >= '0' && ch <= '9') {
				value |= static_cast<uint32_t>(ch - '0');
			} else if (ch >= 'a' && ch <= 'f') {
				value |= static_cast<uint32_t>(ch - 'a' + 10);
			} else if (ch >= 'A' && ch <= 'F') {
				value |= static_cast<uint32_t>(ch - 'A' + 10);
			} else {
				return false;
			}
		}
		return true;
	}

	bool ParseString(std::string &result) noexcept {
		if (!Consume('"')) {
			return false;
		}
		result.clear();
		while (pos_ < text_.size()) {
			const unsigned char ch = static_cast<unsigned char>(text_[pos_++]);
			if (ch == '"') {
				return true;
			}
			if (ch < 0x20) {
				return false;
			}
			if (ch != '\\') {
				result.push_back(static_cast<char>(ch));
				continue;
			}
			if (pos_ >= text_.size()) {
				return false;
			}
			const char escape = text_[pos_++];
			switch (escape) {
			case '"': result.push_back('"'); break;
			case '\\': result.push_back('\\'); break;
			case '/': result.push_back('/'); break;
			case 'b': result.push_back('\b'); break;
			case 'f': result.push_back('\f'); break;
			case 'n': result.push_back('\n'); break;
			case 'r': result.push_back('\r'); break;
			case 't': result.push_back('\t'); break;
			case 'u': {
				uint32_t codepoint = 0;
				if (!ParseHex4(codepoint) || codepoint == 0) {
					return false;
				}
				if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
					if (pos_ + 2 > text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
						return false;
					}
					pos_ += 2;
					uint32_t low = 0;
					if (!ParseHex4(low) || low < 0xDC00 || low > 0xDFFF) {
						return false;
					}
					codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
				} else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
					return false;
				}
				AppendUtf8(result, codepoint);
			} break;
			default:
				return false;
			}
		}
		return false;
	}

	const std::string &text_;
	size_t pos_ = 0;
};

bool GetJsonString(const JsonValue &object, const char *key, bool required,
	std::string &value, std::wstring *error) noexcept {
	value.clear();
	const JsonValue *item = object.Find(key);
	if (item == nullptr) {
		if (required) {
			SetError(error, L"A required manifest field is missing.");
		}
		return !required;
	}
	if (item->type != JsonValue::Type::String) {
		SetError(error, L"A manifest field has the wrong type.");
		return false;
	}
	value = item->stringValue;
	return true;
}

bool GetJsonUInt(const JsonValue &object, const char *key, bool required,
	uint64_t &value, std::wstring *error) noexcept {
	value = 0;
	const JsonValue *item = object.Find(key);
	if (item == nullptr) {
		if (required) {
			SetError(error, L"A required manifest field is missing.");
		}
		return !required;
	}
	if (item->type != JsonValue::Type::Number || item->numberValue < 0
		|| item->numberValue > static_cast<double>(std::numeric_limits<uint32_t>::max())
		|| std::floor(item->numberValue) != item->numberValue) {
		SetError(error, L"A manifest number is invalid.");
		return false;
	}
	value = static_cast<uint64_t>(item->numberValue);
	return true;
}

bool GetJsonBool(const JsonValue &object, const char *key, bool required,
	bool &value, std::wstring *error) noexcept {
	value = false;
	const JsonValue *item = object.Find(key);
	if (item == nullptr) {
		if (required) {
			SetError(error, L"A required manifest field is missing.");
		}
		return !required;
	}
	if (item->type != JsonValue::Type::Bool) {
		SetError(error, L"A manifest boolean is invalid.");
		return false;
	}
	value = item->boolValue;
	return true;
}

bool ContainsLegacyHistory(const std::vector<uint8_t> &data) noexcept {
	if (data.size() < 2 || data[0] != 0xFF || data[1] != 0xFE || (data.size() & 1) != 0) {
		return false;
	}
	const size_t chars = (data.size() - 2) / 2;
	const wchar_t *text = reinterpret_cast<const wchar_t *>(data.data() + 2);
	static constexpr const wchar_t *sections[] = {
		L"[Recent Files]",
		L"[Recent Find]",
		L"[Recent Replace]",
	};
	for (const wchar_t *section : sections) {
		const size_t length = wcslen(section);
		for (size_t i = 0; i + length <= chars; ++i) {
			if (_wcsnicmp(text + i, section, length) == 0) {
				return true;
			}
		}
	}
	return false;
}

bool ReadLegacyPackage(const std::vector<ZipEntryInfo> &entries,
	const std::vector<uint8_t> &zipData,
	ConfigArchivePackage &package,
	std::wstring *error) noexcept {
	if (entries.size() != ARRAYSIZE(LegacyNames)) {
		return false;
	}
	for (const char *name : LegacyNames) {
		if (std::none_of(entries.begin(), entries.end(), [&](const ZipEntryInfo &entry) {
			return entry.name == name;
		})) {
			return false;
		}
	}
	package = {};
	package.format = ConfigArchiveFormat::LegacySixIni;
	package.limitation = "Legacy six-INI archive: no manifest, no per-file SHA-256, no category metadata, and no matepath configuration.";
	for (const char *name : LegacyNames) {
		const auto it = std::find_if(entries.begin(), entries.end(), [&](const ZipEntryInfo &entry) {
			return entry.name == name;
		});
		ConfigArchiveFile file;
		file.path = name;
		file.category = (lstrcmpiA(name, "Notepad4.ini") == 0) ? "settings" : "themes";
		if (!ExtractStoredEntry(zipData, *it, file.data, error)) {
			return false;
		}
		file.size = file.data.size();
		package.files.push_back(std::move(file));
	}
	const ConfigArchiveFile &main = package.files.front();
	package.historyIncluded = ContainsLegacyHistory(main.data);
	package.categories = {"settings", "themes", "syntax"};
	if (package.historyIncluded) {
		package.categories.push_back("history");
	}
	package.warnings.push_back("legacy-format");
	return true;
}

bool ReadCurrentPackage(const std::vector<ZipEntryInfo> &entries,
	const std::vector<uint8_t> &zipData,
	ConfigArchivePackage &package,
	std::wstring *error) noexcept {
	const auto manifestIt = std::find_if(entries.begin(), entries.end(), [](const ZipEntryInfo &entry) {
		return entry.name == "manifest.json";
	});
	if (manifestIt == entries.end()) {
		return false;
	}
	if (manifestIt->uncompressedSize > ConfigArchive_MaxManifestSize) {
		SetError(error, L"The backup manifest is too large.");
		return false;
	}
	std::vector<uint8_t> manifestData;
	if (!ExtractStoredEntry(zipData, *manifestIt, manifestData, error)) {
		return false;
	}
	JsonValue root;
	const std::string manifestText(manifestData.begin(), manifestData.end());
	if (!JsonParser(manifestText).Parse(root) || root.type != JsonValue::Type::Object) {
		SetError(error, L"The backup manifest is not valid JSON.");
		return false;
	}

	std::string schema;
	if (!GetJsonString(root, "schema", true, schema, error) || schema != "notepad4-txtsyntax.settings") {
		SetError(error, L"The backup manifest schema is not supported.");
		return false;
	}
	uint64_t formatVersion = 0;
	uint64_t minimumReaderVersion = 0;
	if (!GetJsonUInt(root, "formatVersion", true, formatVersion, error)
		|| !GetJsonUInt(root, "minimumReaderVersion", true, minimumReaderVersion, error)) {
		return false;
	}
	if (formatVersion != ConfigArchive_FormatVersion) {
		SetError(error, L"This backup format version is not supported.");
		return false;
	}
	if (minimumReaderVersion > ConfigArchive_FormatVersion) {
		SetError(error, L"This backup requires a newer version of Notepad4-TxtSyntax.");
		return false;
	}

	const JsonValue *application = root.Find("application");
	if (application == nullptr || application->type != JsonValue::Type::Object) {
		SetError(error, L"The backup manifest has no application information.");
		return false;
	}
	if (!GetJsonString(*application, "version", false, package.appVersion, error)
		|| !GetJsonString(*application, "build", false, package.buildId, error)
		|| !GetJsonString(root, "createdAtUtc", false, package.createdAtUtc, error)
		|| !GetJsonString(root, "deviceId", false, package.deviceId, error)
		|| !GetJsonString(root, "backupId", false, package.backupId, error)
		|| !GetJsonBool(root, "historyIncluded", true, package.historyIncluded, error)) {
		return false;
	}

	const JsonValue *categories = root.Find("categories");
	if (categories != nullptr) {
		if (categories->type != JsonValue::Type::Array) {
			SetError(error, L"The backup categories are invalid.");
			return false;
		}
		for (const JsonValue &item : categories->arrayValue) {
			if (item.type != JsonValue::Type::String || !IsKnownCategory(item.stringValue)) {
				SetError(error, L"The backup contains an unknown category.");
				return false;
			}
			package.categories.push_back(item.stringValue);
		}
	}
	const JsonValue *warnings = root.Find("warnings");
	if (warnings != nullptr) {
		if (warnings->type != JsonValue::Type::Array) {
			SetError(error, L"The backup warnings are invalid.");
			return false;
		}
		for (const JsonValue &item : warnings->arrayValue) {
			if (item.type != JsonValue::Type::String || item.stringValue.size() > 256) {
				SetError(error, L"The backup contains an invalid warning.");
				return false;
			}
			package.warnings.push_back(item.stringValue);
		}
	}

	const JsonValue *files = root.Find("files");
	if (files == nullptr || files->type != JsonValue::Type::Array
		|| files->arrayValue.empty() || files->arrayValue.size() > ConfigArchive_MaxManifestFiles) {
		SetError(error, L"The backup manifest has no valid file list.");
		return false;
	}
	if (entries.size() != files->arrayValue.size() + 1) {
		SetError(error, L"The backup contains files not listed in its manifest.");
		return false;
	}

	std::vector<bool> used(entries.size(), false);
	const auto manifestIndex = static_cast<size_t>(manifestIt - entries.begin());
	used[manifestIndex] = true;
	for (const JsonValue &fileValue : files->arrayValue) {
		if (fileValue.type != JsonValue::Type::Object) {
			SetError(error, L"A backup file entry is invalid.");
			return false;
		}
		std::string path;
		std::string category;
		std::string sha;
		uint64_t size = 0;
		if (!GetJsonString(fileValue, "path", true, path, error)
			|| !GetJsonString(fileValue, "category", true, category, error)
			|| !GetJsonString(fileValue, "sha256", true, sha, error)
			|| !GetJsonUInt(fileValue, "size", true, size, error)) {
			return false;
		}
		if (!ValidArchivePath(path) || !IsAllowedConfigPath(path)) {
			SetError(error, L"The backup contains an unsupported configuration path.");
			return false;
		}
		if (!IsKnownCategory(category) || !IsHexSha256(sha) || size > ConfigArchive_MaxConfigFileSize) {
			SetError(error, L"A backup file entry has invalid metadata.");
			return false;
		}
		const auto entryIt = std::find_if(entries.begin(), entries.end(), [&](const ZipEntryInfo &entry) {
			return entry.name == path;
		});
		if (entryIt == entries.end()) {
			SetError(error, L"A file listed in the manifest is missing from the archive.");
			return false;
		}
		const size_t index = static_cast<size_t>(entryIt - entries.begin());
		if (used[index]) {
			SetError(error, L"The manifest lists a backup file more than once.");
			return false;
		}
		used[index] = true;
		ConfigArchiveFile file;
		file.path = path;
		file.category = category;
		if (!ExtractStoredEntry(zipData, *entryIt, file.data, error)) {
			return false;
		}
		if (file.data.size() != size) {
			SetError(error, L"A backup file does not match its manifest size.");
			return false;
		}
		std::array<uint8_t, 32> digest {};
		if (!Sha256(file.data.data(), file.data.size(), digest, error)) {
			return false;
		}
		const std::string actual = HexString(digest.data(), digest.size());
		if (_stricmp(actual.c_str(), sha.c_str()) != 0) {
			SetError(error, L"A backup file failed its SHA-256 check.");
			return false;
		}
		file.size = size;
		file.sha256 = sha;
		package.files.push_back(std::move(file));
	}
	if (std::any_of(used.begin(), used.end(), [](bool value) { return !value; })) {
		SetError(error, L"The backup contains unvalidated files.");
		return false;
	}
	if (std::none_of(package.files.begin(), package.files.end(), [](const ConfigArchiveFile &file) {
		return file.path == "config/Notepad4.ini";
	})) {
		SetError(error, L"The backup does not contain the main configuration file.");
		return false;
	}
	if (package.categories.empty()) {
		for (const ConfigArchiveFile &file : package.files) {
			if (std::find(package.categories.begin(), package.categories.end(), file.category) == package.categories.end()) {
				package.categories.push_back(file.category);
			}
		}
		package.categories.push_back("syntax");
	}
	package.format = ConfigArchiveFormat::Current;
	return true;
}

} // namespace

bool ConfigArchive_Create(const std::vector<ConfigArchiveEntry> &entries,
	const ConfigArchiveMetadata &metadata,
	LPCWSTR destinationPath,
	ConfigArchiveCreateResult *result,
	std::wstring *error) noexcept {
	if (error != nullptr) {
		error->clear();
	}
	if (result != nullptr) {
		*result = {};
	}
	if (destinationPath == nullptr || destinationPath[0] == L'\0') {
		SetError(error, L"A destination path is required.");
		return false;
	}
	if (entries.empty()) {
		SetError(error, L"No configuration files were selected for backup.");
		return false;
	}
	if (entries.size() + 1 > ConfigArchive_MaxEntries) {
		SetError(error, L"Too many files were selected for backup.");
		return false;
	}

	try {
		std::vector<PreparedEntry> prepared;
		if (!PrepareEntries(entries, prepared, error)) {
			return false;
		}

		PreparedEntry manifest;
		manifest.name = "manifest.json";
		manifest.category = "manifest";
		const std::string manifestJson = BuildManifest(metadata, prepared);
		manifest.data.assign(manifestJson.begin(), manifestJson.end());
		manifest.crc = Crc32(manifest.data);
		if (!Sha256(manifest.data.data(), manifest.data.size(), manifest.digest, error)) {
			return false;
		}
		prepared.insert(prepared.begin(), std::move(manifest));

		if (wcslen(destinationPath) >= MAX_PATH) {
			SetError(error, L"The destination path is too long.");
			return false;
		}
		WCHAR destination[MAX_PATH];
		lstrcpynW(destination, destinationPath, ARRAYSIZE(destination));
		WCHAR directory[MAX_PATH];
		lstrcpynW(directory, destination, ARRAYSIZE(directory));
		if (!PathRemoveFileSpecW(directory)) {
			SetError(error, L"The destination directory is invalid.");
			return false;
		}
		WCHAR tempPath[MAX_PATH];
		if (!GetTempFileNameW(directory, L"NP4", 0, tempPath)) {
			SetError(error, L"Cannot create a temporary file next to the destination.", GetLastError());
			return false;
		}

		if (!WriteZipFile(tempPath, prepared, error)) {
			DeleteFileW(tempPath);
			return false;
		}

		std::vector<uint8_t> archiveData;
		if (!ReadWholeFile(tempPath, ConfigArchive_MaxArchiveSize, archiveData, error)) {
			DeleteFileW(tempPath);
			return false;
		}
		std::array<uint8_t, 32> archiveDigest {};
		if (!Sha256(archiveData.data(), archiveData.size(), archiveDigest, error)) {
			DeleteFileW(tempPath);
			return false;
		}
		const std::string archiveHash = HexString(archiveDigest.data(), archiveDigest.size());

		if (!MoveFileExW(tempPath, destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			SetError(error, L"Cannot replace the destination archive.", GetLastError());
			DeleteFileW(tempPath);
			return false;
		}

		if (result != nullptr) {
			result->zipSha256 = archiveHash;
			result->zipSize = static_cast<uint64_t>(archiveData.size());
		}
		return true;
	} catch (const std::exception &) {
		SetError(error, L"Not enough memory to create the settings archive.");
	} catch (...) {
		SetError(error, L"Unexpected failure while creating the settings archive.");
	}
	return false;
}

bool ConfigArchive_Inspect(LPCWSTR archivePath,
	ConfigArchiveInspection *inspection,
	std::wstring *error) noexcept {
	if (error != nullptr) {
		error->clear();
	}
	if (inspection == nullptr) {
		SetError(error, L"An inspection result is required.");
		return false;
	}
	*inspection = {};
	if (archivePath == nullptr || archivePath[0] == L'\0') {
		SetError(error, L"An archive path is required.");
		return false;
	}

	try {
		std::vector<uint8_t> data;
		if (!ReadWholeFile(archivePath, ConfigArchive_MaxArchiveSize, data, error)) {
			return false;
		}
		if (data.size() < 22) {
			SetError(error, L"The file is too small to be a ZIP archive.");
			return false;
		}

		size_t eocd = SIZE_MAX;
		const size_t searchStart = data.size() >= 22 + 0xFFFF ? data.size() - 22 - 0xFFFF : 0;
		for (size_t pos = data.size() - 22; pos >= searchStart; --pos) {
			if (Read32(data.data() + pos) == 0x06054B50
				&& pos + 22 + Read16(data.data() + pos + 20) == data.size()) {
				eocd = pos;
				break;
			}
			if (pos == 0) {
				break;
			}
		}
		if (eocd == SIZE_MAX) {
			SetError(error, L"ZIP end-of-central-directory record not found.");
			return false;
		}

		const uint16_t disk = Read16(data.data() + eocd + 4);
		const uint16_t centralDisk = Read16(data.data() + eocd + 6);
		const uint16_t entriesOnDisk = Read16(data.data() + eocd + 8);
		const uint16_t entryCount = Read16(data.data() + eocd + 10);
		const uint32_t centralSize = Read32(data.data() + eocd + 12);
		const uint32_t centralOffset = Read32(data.data() + eocd + 16);
		if (disk != 0 || centralDisk != 0 || entriesOnDisk != entryCount) {
			SetError(error, L"Multi-disk ZIP archives are not supported.");
			return false;
		}
		if (entryCount > ConfigArchive_MaxEntries) {
			SetError(error, L"The archive contains too many entries.");
			return false;
		}
		if (static_cast<uint64_t>(centralOffset) + centralSize > eocd) {
			SetError(error, L"The ZIP central directory is outside the file.");
			return false;
		}

		size_t pos = centralOffset;
		for (uint16_t i = 0; i < entryCount; ++i) {
			if (pos + 46 > data.size() || Read32(data.data() + pos) != 0x02014B50) {
				SetError(error, L"The ZIP central directory is malformed.");
				return false;
			}
			const uint16_t nameLength = Read16(data.data() + pos + 28);
			const uint16_t extraLength = Read16(data.data() + pos + 30);
			const uint16_t commentLength = Read16(data.data() + pos + 32);
			const size_t next = pos + 46ULL + nameLength + extraLength + commentLength;
			if (next > data.size()) {
				SetError(error, L"A ZIP central directory entry is truncated.");
				return false;
			}
			inspection->entryNames.emplace_back(
				reinterpret_cast<const char *>(data.data() + pos + 46), nameLength);
			pos = next;
		}

		for (const std::string &name : inspection->entryNames) {
			if (name == "manifest.json") {
				inspection->hasManifest = true;
			}
			if (name.rfind("config/", 0) == 0) {
				inspection->hasConfigDirectory = true;
			}
			if (name == "config/matepath.ini") {
				inspection->hasMatepathConfig = true;
			}
		}

		if (inspection->hasManifest && inspection->hasConfigDirectory) {
			inspection->format = ConfigArchiveFormat::Current;
		} else if (!inspection->hasManifest && inspection->entryNames.size() == 6
			&& std::all_of(inspection->entryNames.begin(), inspection->entryNames.end(), IsLegacyName)) {
			inspection->format = ConfigArchiveFormat::LegacySixIni;
			inspection->limitation = "Legacy six-INI archive: no manifest, no per-file SHA-256, no category metadata, and no matepath configuration.";
		} else {
			inspection->format = ConfigArchiveFormat::Unknown;
			inspection->limitation = "Unknown settings archive layout.";
		}
		return true;
	} catch (const std::exception &) {
		SetError(error, L"Not enough memory to inspect the settings archive.");
	} catch (...) {
		SetError(error, L"Unexpected failure while inspecting the settings archive.");
	}
	return false;
}

bool ConfigArchive_Read(LPCWSTR archivePath,
	ConfigArchivePackage *package,
	std::wstring *error) noexcept {
	if (error != nullptr) {
		error->clear();
	}
	if (package == nullptr) {
		SetError(error, L"A package result is required.");
		return false;
	}
	*package = {};
	if (archivePath == nullptr || archivePath[0] == L'\0') {
		SetError(error, L"An archive path is required.");
		return false;
	}

	try {
		std::vector<uint8_t> zipData;
		if (!ReadWholeFile(archivePath, ConfigArchive_MaxArchiveSize, zipData, error)) {
			return false;
		}
		std::vector<ZipEntryInfo> entries;
		if (!ReadZipCentralDirectory(zipData, entries, error)) {
			return false;
		}

		const bool hasManifest = std::any_of(entries.begin(), entries.end(), [](const ZipEntryInfo &entry) {
			return entry.name == "manifest.json";
		});
		if (hasManifest) {
			if (!ReadCurrentPackage(entries, zipData, *package, error)) {
				if (error != nullptr && error->empty()) {
					SetError(error, L"The backup format is not supported.");
				}
				return false;
			}
		} else if (entries.size() == ARRAYSIZE(LegacyNames)) {
			if (!ReadLegacyPackage(entries, zipData, *package, error)) {
				SetError(error, L"The legacy backup is not a supported six-INI archive.");
				return false;
			}
		} else {
			SetError(error, L"The archive is not a supported settings backup.");
			return false;
		}
		return true;
	} catch (const std::exception &) {
		SetError(error, L"Not enough memory to read the settings archive.");
	} catch (...) {
		SetError(error, L"Unexpected failure while reading the settings archive.");
	}
	return false;
}
