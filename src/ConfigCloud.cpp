/******************************************************************************
*
* Notepad4-TxtSyntax
*
* ConfigCloud.cpp
*   AES-256-GCM encrypted WebDAV settings backup and restore.
*
******************************************************************************/

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

#include "SciCall.h"
#include "config.h"
#include "Helpers.h"
#include "Notepad4.h"
#include "ConfigBackup.h"
#include "ConfigCloud.h"
#include "ConfigBackup.h"

namespace {

constexpr char CloudMagic[8] = {'N', 'P', '4', 'C', 'L', 'D', '0', '1'};
constexpr uint32_t CloudVersion = 1;
constexpr uint32_t CloudIterations = 210000;
constexpr uint32_t CloudMinIterations = 10000;
constexpr uint32_t CloudMaxIterations = 2000000;
constexpr size_t SaltLength = 16;
constexpr size_t NonceLength = 12;
constexpr size_t TagLength = 16;
constexpr size_t KeyLength = 32;
constexpr size_t HeaderLength = 8 + 4 + 4 + SaltLength + NonceLength + 8;
constexpr uint64_t MaxCloudFileSize = 256ULL * 1024 * 1024;
constexpr size_t MaxResponseBody = 256ULL * 1024 * 1024;
constexpr wchar_t CloudRemotePrefix[] = L"Notepad4-Cloud-";
constexpr wchar_t CloudAutoPrefix[] = L"Notepad4-Cloud-Auto-";
constexpr wchar_t CloudRemoteSuffix[] = L".np4enc";
constexpr wchar_t CredentialsEntropy[] = L"Notepad4-TxtSyntax.Cloud.Credentials";

ConfigCloudSettings g_CloudSettings;

void SetError(std::wstring *error, const wchar_t *message) noexcept {
	if (error != nullptr) {
		*error = message;
	}
}

void SetWin32Error(std::wstring *error, const wchar_t *action, DWORD code) noexcept {
	if (error != nullptr) {
		*error = std::wstring(action) + L" (Win32 error " + std::to_wstring(code) + L").";
	}
}

void SetHttpError(std::wstring *error, const wchar_t *action, DWORD status) noexcept {
	if (error == nullptr) {
		return;
	}
	const wchar_t *reason = L"request failed";
	switch (status) {
	case 401: reason = L"authentication failed"; break;
	case 403: reason = L"access denied"; break;
	case 404: reason = L"path not found"; break;
	case 409: reason = L"conflict"; break;
	case 412: reason = L"precondition failed"; break;
	case 413: reason = L"payload too large"; break;
	case 423: reason = L"resource locked"; break;
	case 429: reason = L"too many requests"; break;
	case 500: reason = L"server error"; break;
	case 502: reason = L"bad gateway"; break;
	case 503: reason = L"service unavailable"; break;
	case 504: reason = L"gateway timeout"; break;
	case 507: reason = L"insufficient storage"; break;
	default: break;
	}
	*error = std::wstring(action) + L": " + reason + L" (HTTP "
		+ std::to_wstring(status) + L").";
}

std::string Narrow(const std::wstring &text) noexcept {
	if (text.empty()) {
		return {};
	}
	const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
		static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	if (length <= 0) {
		return {};
	}
	std::string result(static_cast<size_t>(length), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
		result.data(), length, nullptr, nullptr);
	return result;
}

std::wstring Widen(const std::string &text) noexcept {
	if (text.empty()) {
		return {};
	}
	const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
		static_cast<int>(text.size()), nullptr, 0);
	if (length <= 0) {
		return {};
	}
	std::wstring result(static_cast<size_t>(length), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
		result.data(), length);
	return result;
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

bool FileExists(LPCWSTR path) noexcept {
	const DWORD attributes = GetFileAttributesW(path);
	return attributes != INVALID_FILE_ATTRIBUTES
		&& (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

bool ReadAllBytes(LPCWSTR path, std::vector<uint8_t> &data, std::wstring *error) noexcept {
	data.clear();
	if (!FileExists(path)) {
		SetError(error, L"The requested file does not exist.");
		return false;
	}
	HANDLE file = CreateFileW(path, GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		SetWin32Error(error, L"Cannot open the requested file", GetLastError());
		return false;
	}
	LARGE_INTEGER size {};
	if (!GetFileSizeEx(file, &size) || size.QuadPart < 0
		|| static_cast<uint64_t>(size.QuadPart) > MaxCloudFileSize) {
		CloseHandle(file);
		SetError(error, L"The requested file is too large.");
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
			SetError(error, L"Cannot read the requested file.");
			return false;
		}
		offset += read;
	}
	CloseHandle(file);
	return true;
}

bool WriteAllBytesAtomic(LPCWSTR path, const uint8_t *data, size_t size,
	std::wstring *error) noexcept {
	WCHAR directory[MAX_PATH];
	lstrcpynW(directory, path, ARRAYSIZE(directory));
	if (!PathRemoveFileSpecW(directory)) {
		SetError(error, L"Cannot resolve the destination directory.");
		return false;
	}
	WCHAR temporary[MAX_PATH];
	if (!GetTempFileNameW(directory, L"NP4C", 0, temporary)) {
		SetWin32Error(error, L"Cannot create a temporary file", GetLastError());
		return false;
	}
	HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	bool written = false;
	if (file != INVALID_HANDLE_VALUE) {
		DWORD count = 0;
		written = (size == 0 || (WriteFile(file, data, static_cast<DWORD>(size), &count, nullptr)
			&& count == size)) && FlushFileBuffers(file);
		CloseHandle(file);
	}
	if (!written || !MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		const DWORD code = GetLastError();
		DeleteFileW(temporary);
		SetWin32Error(error, L"Cannot write the destination file", code);
		return false;
	}
	return true;
}

bool WriteAllBytesAtomic(LPCWSTR path, const std::vector<uint8_t> &data,
	std::wstring *error) noexcept {
	return WriteAllBytesAtomic(path, data.data(), data.size(), error);
}

std::wstring GetCloudDirectory() noexcept {
	const std::wstring dataDirectory = GetNotepad4DataDirectory();
	if (dataDirectory.empty()) {
		return {};
	}
	return dataDirectory + L"\\Cloud";
}

std::wstring GetPrivateCloudDirectory() noexcept {
	const std::wstring privateData = GetNotepad4PrivateDataDirectory();
	if (!privateData.empty()) {
		return privateData + L"\\Cloud";
	}
	return GetCloudDirectory();
}

bool EnsurePrivateCloudDirectory(std::wstring *error) noexcept {
	const std::wstring directory = GetPrivateCloudDirectory();
	if (directory.empty()) {
		SetError(error, L"Cannot resolve the private application data directory.");
		return false;
	}
	const int result = SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
	if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS) {
		SetWin32Error(error, L"Cannot create the private cloud directory", result);
		return false;
	}
	return PathIsDirectoryW(directory.c_str()) != FALSE;
}

bool EnsureCloudDirectory(std::wstring *error) noexcept {
	const std::wstring directory = GetCloudDirectory();
	if (directory.empty()) {
		SetError(error, L"Cannot resolve the local application data directory.");
		return false;
	}
	const int result = SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
	if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS) {
		SetWin32Error(error, L"Cannot create the cloud settings directory", result);
		return false;
	}
	return PathIsDirectoryW(directory.c_str()) != FALSE;
}

std::wstring MakeTemporaryPath(LPCWSTR extension, std::wstring *error) noexcept {
	WCHAR tempDirectory[MAX_PATH];
	const DWORD length = GetTempPathW(ARRAYSIZE(tempDirectory), tempDirectory);
	if (length == 0 || length >= ARRAYSIZE(tempDirectory)) {
		SetWin32Error(error, L"Cannot resolve the temporary directory", GetLastError());
		return {};
	}
	WCHAR path[MAX_PATH];
	if (!GetTempFileNameW(tempDirectory, L"NP4C", 0, path)) {
		SetWin32Error(error, L"Cannot create a temporary file", GetLastError());
		return {};
	}
	DeleteFileW(path);
	std::wstring result = path;
	result += extension;
	return result;
}

void CleanupTemporaryFiles() noexcept {
	WCHAR directory[MAX_PATH];
	if (GetTempPathW(ARRAYSIZE(directory), directory) == 0) {
		return;
	}
	FILETIME now {};
	GetSystemTimeAsFileTime(&now);
	ULARGE_INTEGER nowValue {};
	nowValue.LowPart = now.dwLowDateTime;
	nowValue.HighPart = now.dwHighDateTime;
	const uint64_t maxAge = 24ULL * 60 * 60 * 10000000ULL;
	for (const wchar_t *pattern : { L"NP4C*.zip", L"NP4C*.np4enc" }) {
		WIN32_FIND_DATAW data {};
		const std::wstring query = std::wstring(directory) + pattern;
		HANDLE find = FindFirstFileW(query.c_str(), &data);
		if (find == INVALID_HANDLE_VALUE) {
			continue;
		}
		do {
			if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
				|| _wcsnicmp(data.cFileName, L"NP4C", 4) != 0) {
				continue;
			}
			ULARGE_INTEGER modified {};
			modified.LowPart = data.ftLastWriteTime.dwLowDateTime;
			modified.HighPart = data.ftLastWriteTime.dwHighDateTime;
			if (nowValue.QuadPart > modified.QuadPart
				&& nowValue.QuadPart - modified.QuadPart > maxAge) {
				const std::wstring path = std::wstring(directory) + data.cFileName;
				DeleteFileW(path.c_str());
			}
		} while (FindNextFileW(find, &data));
		FindClose(find);
	}
}

class TempFile {
public:
	explicit TempFile(std::wstring path) noexcept : path_(std::move(path)) {}
	~TempFile() noexcept {
		if (!path_.empty()) {
			DeleteFileW(path_.c_str());
		}
	}
	TempFile(const TempFile &) = delete;
	TempFile &operator=(const TempFile &) = delete;
	const std::wstring &Path() const noexcept { return path_; }
	void Release() noexcept { path_.clear(); }

private:
	std::wstring path_;
};

bool RandomBytes(uint8_t *data, size_t size, std::wstring *error) noexcept {
	const NTSTATUS status = BCryptGenRandom(nullptr, data, static_cast<ULONG>(size),
		BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	if (!BCRYPT_SUCCESS(status)) {
		SetError(error, L"Cannot generate cryptographically secure random bytes.");
		return false;
	}
	return true;
}

bool DeriveKey(const std::wstring &passphrase, const uint8_t *salt, size_t saltSize,
	uint32_t iterations, std::array<uint8_t, KeyLength> &key, std::wstring *error) noexcept {
	if (passphrase.empty()) {
		SetError(error, L"An encryption passphrase is required.");
		return false;
	}
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
		nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
	if (!BCRYPT_SUCCESS(status)) {
		SetError(error, L"Cannot open the PBKDF2 provider.");
		return false;
	}
	using DeriveKeyFunction = NTSTATUS(WINAPI *)(BCRYPT_ALG_HANDLE, PUCHAR, ULONG,
		PUCHAR, ULONG, ULONGLONG, PUCHAR, ULONG, ULONG);
	HMODULE bcrypt = LoadLibraryW(L"bcrypt.dll");
	if (bcrypt == nullptr) {
		BCryptCloseAlgorithmProvider(algorithm, 0);
		SetError(error, L"Cannot load the PBKDF2 provider.");
		return false;
	}
	const auto derive = reinterpret_cast<DeriveKeyFunction>(
		GetProcAddress(bcrypt, "BCryptDeriveKeyPBKDF2"));
	if (derive == nullptr) {
		FreeLibrary(bcrypt);
		BCryptCloseAlgorithmProvider(algorithm, 0);
		SetError(error, L"PBKDF2 requires Windows 7 or later.");
		return false;
	}
	status = derive(algorithm,
		reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(passphrase.data())),
		static_cast<ULONG>(passphrase.size() * sizeof(wchar_t)),
		const_cast<PUCHAR>(salt), static_cast<ULONG>(saltSize),
		iterations, key.data(), static_cast<ULONG>(key.size()), 0);
	FreeLibrary(bcrypt);
	BCryptCloseAlgorithmProvider(algorithm, 0);
	if (!BCRYPT_SUCCESS(status)) {
		SetError(error, L"Cannot derive the encryption key.");
		return false;
	}
	return true;
}

bool AesGcmEncrypt(const std::vector<uint8_t> &plain, const std::wstring &passphrase,
	std::vector<uint8_t> &encrypted, std::wstring *error) noexcept {
	if (plain.empty() || plain.size() > MaxCloudFileSize) {
		SetError(error, L"The settings payload is empty or too large.");
		return false;
	}
	std::array<uint8_t, SaltLength> salt {};
	std::array<uint8_t, NonceLength> nonce {};
	if (!RandomBytes(salt.data(), salt.size(), error) || !RandomBytes(nonce.data(), nonce.size(), error)) {
		return false;
	}
	std::array<uint8_t, KeyLength> key {};
	if (!DeriveKey(passphrase, salt.data(), salt.size(), CloudIterations, key, error)) {
		return false;
	}

	std::vector<uint8_t> header(HeaderLength, 0);
	memcpy(header.data(), CloudMagic, sizeof(CloudMagic));
	const auto put32 = [&](size_t offset, uint32_t value) {
		header[offset] = static_cast<uint8_t>(value);
		header[offset + 1] = static_cast<uint8_t>(value >> 8);
		header[offset + 2] = static_cast<uint8_t>(value >> 16);
		header[offset + 3] = static_cast<uint8_t>(value >> 24);
	};
	const auto put64 = [&](size_t offset, uint64_t value) {
		for (int i = 0; i < 8; ++i) {
			header[offset + i] = static_cast<uint8_t>(value >> (i * 8));
		}
	};
	put32(8, CloudVersion);
	put32(12, CloudIterations);
	memcpy(header.data() + 16, salt.data(), salt.size());
	memcpy(header.data() + 16 + SaltLength, nonce.data(), nonce.size());
	put64(16 + SaltLength + NonceLength, plain.size());

	BCRYPT_ALG_HANDLE algorithm = nullptr;
	BCRYPT_KEY_HANDLE keyHandle = nullptr;
	std::vector<uint8_t> object;
	DWORD objectLength = 0;
	DWORD written = 0;
	std::array<uint8_t, TagLength> tag {};
	NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0);
	if (!BCRYPT_SUCCESS(status)) {
		SecureZeroMemory(key.data(), key.size());
		SetError(error, L"Cannot open the AES provider.");
		return false;
	}
	status = BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
		reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_GCM)),
		sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
	if (BCRYPT_SUCCESS(status)) {
		status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
			reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &written, 0);
	}
	if (BCRYPT_SUCCESS(status) && objectLength != 0) {
		object.resize(objectLength);
		status = BCryptGenerateSymmetricKey(algorithm, &keyHandle, object.data(),
			objectLength, key.data(), static_cast<ULONG>(key.size()), 0);
	}
	if (!BCRYPT_SUCCESS(status)) {
		if (keyHandle != nullptr) {
			BCryptDestroyKey(keyHandle);
		}
		BCryptCloseAlgorithmProvider(algorithm, 0);
		SecureZeroMemory(key.data(), key.size());
		SetError(error, L"Cannot initialize AES-256-GCM.");
		return false;
	}

	std::vector<uint8_t> cipher(plain.size());
	BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
	BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
	authInfo.pbNonce = nonce.data();
	authInfo.cbNonce = static_cast<ULONG>(nonce.size());
	authInfo.pbAuthData = header.data();
	authInfo.cbAuthData = static_cast<ULONG>(header.size());
	authInfo.pbTag = tag.data();
	authInfo.cbTag = static_cast<ULONG>(tag.size());
	status = BCryptEncrypt(keyHandle, const_cast<PUCHAR>(plain.data()),
		static_cast<ULONG>(plain.size()), &authInfo, nullptr, 0, cipher.data(),
		static_cast<ULONG>(cipher.size()), &written, 0);
	BCryptDestroyKey(keyHandle);
	BCryptCloseAlgorithmProvider(algorithm, 0);
	SecureZeroMemory(key.data(), key.size());
	if (!BCRYPT_SUCCESS(status) || written != cipher.size()) {
		SetError(error, L"Cannot encrypt the settings payload.");
		return false;
	}

	encrypted.clear();
	encrypted.reserve(header.size() + cipher.size() + tag.size());
	encrypted.insert(encrypted.end(), header.begin(), header.end());
	encrypted.insert(encrypted.end(), cipher.begin(), cipher.end());
	encrypted.insert(encrypted.end(), tag.begin(), tag.end());
	return true;
}

bool AesGcmDecrypt(const std::vector<uint8_t> &encrypted, const std::wstring &passphrase,
	std::vector<uint8_t> &plain, std::wstring *error) noexcept {
	if (encrypted.size() < HeaderLength + TagLength) {
		SetError(error, L"The encrypted backup is truncated.");
		return false;
	}
	if (memcmp(encrypted.data(), CloudMagic, sizeof(CloudMagic)) != 0) {
		SetError(error, L"The encrypted backup has an unknown format.");
		return false;
	}
	const auto get32 = [&](size_t offset) {
		return static_cast<uint32_t>(encrypted[offset])
			| (static_cast<uint32_t>(encrypted[offset + 1]) << 8)
			| (static_cast<uint32_t>(encrypted[offset + 2]) << 16)
			| (static_cast<uint32_t>(encrypted[offset + 3]) << 24);
	};
	const auto get64 = [&](size_t offset) {
		uint64_t value = 0;
		for (int i = 0; i < 8; ++i) {
			value |= static_cast<uint64_t>(encrypted[offset + i]) << (i * 8);
		}
		return value;
	};
	const uint32_t version = get32(8);
	const uint32_t iterations = get32(12);
	const uint64_t cipherLength = get64(16 + SaltLength + NonceLength);
	if (version != CloudVersion || iterations < CloudMinIterations || iterations > CloudMaxIterations) {
		SetError(error, L"The encrypted backup uses unsupported parameters.");
		return false;
	}
	if (cipherLength > MaxCloudFileSize
		|| static_cast<uint64_t>(encrypted.size()) != HeaderLength + cipherLength + TagLength) {
		SetError(error, L"The encrypted backup has an invalid size.");
		return false;
	}

	const uint8_t *salt = encrypted.data() + 16;
	const uint8_t *nonce = encrypted.data() + 16 + SaltLength;
	const uint8_t *cipher = encrypted.data() + HeaderLength;
	const uint8_t *tag = encrypted.data() + HeaderLength + cipherLength;

	std::array<uint8_t, KeyLength> key {};
	if (!DeriveKey(passphrase, salt, SaltLength, iterations, key, error)) {
		return false;
	}
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	BCRYPT_KEY_HANDLE keyHandle = nullptr;
	std::vector<uint8_t> object;
	DWORD objectLength = 0;
	DWORD written = 0;
	NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0);
	if (!BCRYPT_SUCCESS(status)) {
		SecureZeroMemory(key.data(), key.size());
		SetError(error, L"Cannot open the AES provider.");
		return false;
	}
	status = BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
		reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_GCM)),
		sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
	if (BCRYPT_SUCCESS(status)) {
		status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
			reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &written, 0);
	}
	if (BCRYPT_SUCCESS(status) && objectLength != 0) {
		object.resize(objectLength);
		status = BCryptGenerateSymmetricKey(algorithm, &keyHandle, object.data(),
			objectLength, key.data(), static_cast<ULONG>(key.size()), 0);
	}
	if (!BCRYPT_SUCCESS(status)) {
		if (keyHandle != nullptr) {
			BCryptDestroyKey(keyHandle);
		}
		BCryptCloseAlgorithmProvider(algorithm, 0);
		SecureZeroMemory(key.data(), key.size());
		SetError(error, L"Cannot initialize AES-256-GCM.");
		return false;
	}

	plain.resize(static_cast<size_t>(cipherLength));
	BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
	BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
	authInfo.pbNonce = const_cast<PUCHAR>(nonce);
	authInfo.cbNonce = static_cast<ULONG>(NonceLength);
	authInfo.pbAuthData = const_cast<PUCHAR>(encrypted.data());
	authInfo.cbAuthData = static_cast<ULONG>(HeaderLength);
	authInfo.pbTag = const_cast<PUCHAR>(tag);
	authInfo.cbTag = static_cast<ULONG>(TagLength);
	status = BCryptDecrypt(keyHandle, const_cast<PUCHAR>(cipher),
		static_cast<ULONG>(cipherLength), &authInfo, nullptr, 0,
		plain.data(), static_cast<ULONG>(plain.size()), &written, 0);
	BCryptDestroyKey(keyHandle);
	BCryptCloseAlgorithmProvider(algorithm, 0);
	SecureZeroMemory(key.data(), key.size());
	if (!BCRYPT_SUCCESS(status) || written != plain.size()) {
		plain.clear();
		SetError(error, L"Cannot decrypt the backup. Check the passphrase and file integrity.");
		return false;
	}
	return true;
}

struct CredentialBlob {
	std::wstring password;
	std::wstring passphrase;
};

std::vector<uint8_t> SerializeCredentials(const ConfigCloudCredentials &credentials) noexcept {
	const std::wstring &password = credentials.password;
	const std::wstring &passphrase = credentials.passphrase;
	if (password.size() > 4096 || passphrase.size() > 4096) {
		return {};
	}
	std::vector<uint8_t> data;
	data.reserve(8 + (password.size() + passphrase.size()) * sizeof(wchar_t));
	const auto put32 = [&](uint32_t value) {
		data.push_back(static_cast<uint8_t>(value));
		data.push_back(static_cast<uint8_t>(value >> 8));
		data.push_back(static_cast<uint8_t>(value >> 16));
		data.push_back(static_cast<uint8_t>(value >> 24));
	};
	put32(static_cast<uint32_t>(password.size()));
	put32(static_cast<uint32_t>(passphrase.size()));
	const auto append = [&](const std::wstring &text) {
		const size_t offset = data.size();
		data.resize(offset + text.size() * sizeof(wchar_t));
		memcpy(data.data() + offset, text.data(), text.size() * sizeof(wchar_t));
	};
	append(password);
	append(passphrase);
	return data;
}

bool ParseCredentials(const std::vector<uint8_t> &data, ConfigCloudCredentials &credentials) noexcept {
	if (data.size() < 8) {
		return false;
	}
	const auto get32 = [&](size_t offset) {
		return static_cast<uint32_t>(data[offset])
			| (static_cast<uint32_t>(data[offset + 1]) << 8)
			| (static_cast<uint32_t>(data[offset + 2]) << 16)
			| (static_cast<uint32_t>(data[offset + 3]) << 24);
	};
	const uint32_t passwordLength = get32(0);
	const uint32_t passphraseLength = get32(4);
	if (passwordLength > 4096 || passphraseLength > 4096) {
		return false;
	}
	const uint64_t needed = 8ULL + (static_cast<uint64_t>(passwordLength) + passphraseLength) * sizeof(wchar_t);
	if (data.size() != needed) {
		return false;
	}
	size_t offset = 8;
	credentials.password.resize(passwordLength);
	memcpy(credentials.password.data(), data.data() + offset,
		static_cast<size_t>(passwordLength) * sizeof(wchar_t));
	offset += static_cast<size_t>(passwordLength) * sizeof(wchar_t);
	credentials.passphrase.resize(passphraseLength);
	memcpy(credentials.passphrase.data(), data.data() + offset,
		static_cast<size_t>(passphraseLength) * sizeof(wchar_t));
	return true;
}

bool SaveProtectedCredentials(const ConfigCloudCredentials &credentials, std::wstring *error) noexcept {
	if (!EnsurePrivateCloudDirectory(error)) {
		return false;
	}
	const std::vector<uint8_t> plain = SerializeCredentials(credentials);
	if (plain.empty()) {
		SetError(error, L"The cloud credentials are too large.");
		return false;
	}
	DATA_BLOB input {};
	input.pbData = const_cast<BYTE *>(plain.data());
	input.cbData = static_cast<DWORD>(plain.size());
	DATA_BLOB entropy {};
	entropy.pbData = const_cast<BYTE *>(reinterpret_cast<const BYTE *>(CredentialsEntropy));
	entropy.cbData = static_cast<DWORD>(sizeof(CredentialsEntropy));
	DATA_BLOB output {};
	if (!CryptProtectData(&input, L"Notepad4-TxtSyntax cloud credentials", &entropy,
		nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
		SetWin32Error(error, L"Cannot protect the cloud credentials", GetLastError());
		return false;
	}
	std::vector<uint8_t> protectedData(output.pbData, output.pbData + output.cbData);
	LocalFree(output.pbData);
	const std::wstring path = GetPrivateCloudDirectory() + L"\\credentials.bin";
	const bool saved = WriteAllBytesAtomic(path.c_str(), protectedData, error);
	SecureZeroMemory(protectedData.data(), protectedData.size());
	return saved;
}

bool LoadProtectedCredentials(ConfigCloudCredentials &credentials, std::wstring *error) noexcept {
	credentials = {};
	const std::wstring privateDirectory = GetPrivateCloudDirectory();
	const std::wstring legacyDirectory = GetCloudDirectory();
	if (privateDirectory.empty() && legacyDirectory.empty()) {
		return true;
	}
	std::wstring path = privateDirectory + L"\\credentials.bin";
	bool migrate = false;
	if (!FileExists(path.c_str())) {
		path = legacyDirectory + L"\\credentials.bin";
		migrate = FileExists(path.c_str());
	}
	if (!FileExists(path.c_str())) {
		return true;
	}
	std::vector<uint8_t> protectedData;
	if (!ReadAllBytes(path.c_str(), protectedData, error)) {
		return false;
	}
	DATA_BLOB input {};
	input.pbData = protectedData.data();
	input.cbData = static_cast<DWORD>(protectedData.size());
	DATA_BLOB entropy {};
	entropy.pbData = const_cast<BYTE *>(reinterpret_cast<const BYTE *>(CredentialsEntropy));
	entropy.cbData = static_cast<DWORD>(sizeof(CredentialsEntropy));
	DATA_BLOB output {};
	if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr,
		CRYPTPROTECT_UI_FORBIDDEN, &output)) {
		SecureZeroMemory(protectedData.data(), protectedData.size());
		SetWin32Error(error, L"Cannot unprotect the cloud credentials", GetLastError());
		return false;
	}
	std::vector<uint8_t> plain(output.pbData, output.pbData + output.cbData);
	LocalFree(output.pbData);
	const bool parsed = ParseCredentials(plain, credentials);
	SecureZeroMemory(plain.data(), plain.size());
	if (!parsed) {
		SecureZeroMemory(protectedData.data(), protectedData.size());
		SetError(error, L"The stored cloud credentials are corrupted.");
		return false;
	}
	if (migrate && !privateDirectory.empty()) {
		std::wstring migrationError;
		if (EnsurePrivateCloudDirectory(&migrationError)) {
			const std::wstring privatePath = privateDirectory + L"\\credentials.bin";
			if (WriteAllBytesAtomic(privatePath.c_str(), protectedData, &migrationError)) {
				DeleteFileW(path.c_str());
			}
		}
	}
	SecureZeroMemory(protectedData.data(), protectedData.size());
	return true;
}

struct ParsedUrl {
	bool secure = false;
	std::wstring host;
	INTERNET_PORT port = 0;
	std::wstring basePath;
};

bool ParseUrl(const std::wstring &url, ParsedUrl &parsed, std::wstring *error) noexcept {
	parsed = {};
	if (url.empty()) {
		SetError(error, L"A WebDAV URL is required.");
		return false;
	}
	URL_COMPONENTS components {};
	components.dwStructSize = sizeof(components);
	components.dwSchemeLength = static_cast<DWORD>(-1);
	components.dwHostNameLength = static_cast<DWORD>(-1);
	components.dwUrlPathLength = static_cast<DWORD>(-1);
	components.dwExtraInfoLength = static_cast<DWORD>(-1);
	if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components)) {
		SetWin32Error(error, L"The WebDAV URL is invalid", GetLastError());
		return false;
	}
	if (components.nScheme != INTERNET_SCHEME_HTTPS) {
		SetError(error, L"Only HTTPS WebDAV URLs are supported.");
		return false;
	}
	if (components.dwHostNameLength == 0 || components.dwHostNameLength == static_cast<DWORD>(-1)) {
		SetError(error, L"The WebDAV URL does not contain a host name.");
		return false;
	}
	parsed.secure = components.nScheme == INTERNET_SCHEME_HTTPS;
	parsed.host.assign(components.lpszHostName, components.dwHostNameLength);
	parsed.port = components.nPort;
	parsed.basePath.assign(components.lpszUrlPath, components.dwUrlPathLength);
	if (parsed.basePath.empty()) {
		parsed.basePath = L"/";
	}
	if (parsed.basePath.front() != L'/') {
		parsed.basePath.insert(parsed.basePath.begin(), L'/');
	}
	if (components.dwExtraInfoLength != 0 && components.dwExtraInfoLength != static_cast<DWORD>(-1)) {
		SetError(error, L"WebDAV URLs with query strings are not supported.");
		return false;
	}
	return true;
}

bool IsUnreserved(wchar_t ch) noexcept {
	return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z')
		|| (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'.'
		|| ch == L'_' || ch == L'~';
}

std::wstring EncodePathSegment(const std::wstring &segment) noexcept {
	const std::string bytes = Narrow(segment);
	std::wstring encoded;
	encoded.reserve(bytes.size() * 3);
	for (unsigned char ch : bytes) {
		if (ch < 0x80 && IsUnreserved(static_cast<wchar_t>(ch))) {
			encoded.push_back(static_cast<wchar_t>(ch));
		} else {
			wchar_t buffer[8];
			swprintf_s(buffer, L"%%%02X", ch);
			encoded += buffer;
		}
	}
	return encoded;
}

bool NormalizeFolder(const std::wstring &folder, std::wstring &normalized,
	std::wstring *error) noexcept {
	normalized.clear();
	std::wstring text = TrimCopy(folder);
	std::replace(text.begin(), text.end(), L'\\', L'/');
	size_t start = 0;
	while (start <= text.size()) {
		const size_t end = text.find(L'/', start);
		std::wstring segment = text.substr(start,
			(end == std::wstring::npos ? text.size() : end) - start);
		if (!segment.empty()) {
			if (segment == L"." || segment == L"..") {
				SetError(error, L"The remote folder cannot contain . or .. segments.");
				return false;
			}
			for (wchar_t ch : segment) {
				if (ch < 0x20) {
					SetError(error, L"The remote folder contains an invalid character.");
					return false;
				}
			}
			if (!normalized.empty()) {
				normalized.push_back(L'/');
			}
			normalized += EncodePathSegment(segment);
		}
		if (end == std::wstring::npos) {
			break;
		}
		start = end + 1;
	}
	return true;
}

std::wstring BuildFolderPath(const ParsedUrl &url, const std::wstring &encodedFolder,
	bool trailingSlash) noexcept {
	std::wstring path = url.basePath;
	if (path.empty() || path.back() != L'/') {
		path.push_back(L'/');
	}
	if (!encodedFolder.empty()) {
		path += encodedFolder;
		if (trailingSlash) {
			path.push_back(L'/');
		}
	}
	if (trailingSlash && !path.empty() && path.back() != L'/') {
		path.push_back(L'/');
	}
	return path;
}

struct HttpResponse {
	DWORD status = 0;
	std::vector<uint8_t> body;
};

bool IsHttpSuccess(DWORD status) noexcept {
	return status == 200 || status == 201 || status == 204 || status == 207;
}

bool HttpRequestOnce(const ParsedUrl &url, LPCWSTR method, const std::wstring &path,
	const std::wstring &headers, const std::vector<uint8_t> &body,
	const std::wstring &userName, const ConfigCloudCredentials &credentials,
	HttpResponse &response, std::wstring *error) noexcept {
	response = {};
	HINTERNET session = WinHttpOpen(L"Notepad4-TxtSyntax/1.0",
		WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
		WINHTTP_NO_PROXY_BYPASS, 0);
	if (session == nullptr) {
		SetWin32Error(error, L"Cannot initialize WinHTTP", GetLastError());
		return false;
	}
	WinHttpSetTimeouts(session, 15000, 15000, 30000, 30000);

	HINTERNET connection = WinHttpConnect(session, url.host.c_str(), url.port, 0);
	if (connection == nullptr) {
		SetWin32Error(error, L"Cannot connect to the WebDAV server", GetLastError());
		WinHttpCloseHandle(session);
		return false;
	}
	HINTERNET request = WinHttpOpenRequest(connection, method, path.c_str(), nullptr,
		WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
		url.secure ? WINHTTP_FLAG_SECURE : 0);
	if (request == nullptr) {
		SetWin32Error(error, L"Cannot create the WebDAV request", GetLastError());
		WinHttpCloseHandle(connection);
		WinHttpCloseHandle(session);
		return false;
	}
	const DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
	WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY,
		const_cast<DWORD *>(&redirectPolicy), sizeof(redirectPolicy));
	if (!userName.empty() || !credentials.password.empty()) {
		WinHttpSetCredentials(request, WINHTTP_AUTH_TARGET_SERVER,
			WINHTTP_AUTH_SCHEME_BASIC, userName.c_str(), credentials.password.c_str(), nullptr);
	}

	const DWORD bodyLength = static_cast<DWORD>(body.size());
	BOOL sent = WinHttpSendRequest(request,
		headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
		headers.empty() ? 0 : static_cast<DWORD>(-1),
		body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<BYTE *>(body.data()),
		bodyLength, bodyLength, 0);
	if (!sent) {
		SetWin32Error(error, L"Cannot send the WebDAV request", GetLastError());
		WinHttpCloseHandle(request);
		WinHttpCloseHandle(connection);
		WinHttpCloseHandle(session);
		return false;
	}
	if (!WinHttpReceiveResponse(request, nullptr)) {
		SetWin32Error(error, L"Cannot receive the WebDAV response", GetLastError());
		WinHttpCloseHandle(request);
		WinHttpCloseHandle(connection);
		WinHttpCloseHandle(session);
		return false;
	}

	DWORD status = 0;
	DWORD statusSize = sizeof(status);
	if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
		WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
		response.status = status;
	}

	bool readOk = true;
	size_t total = 0;
	for (;;) {
		DWORD available = 0;
		if (!WinHttpQueryDataAvailable(request, &available)) {
			readOk = false;
			SetWin32Error(error, L"Cannot read the WebDAV response", GetLastError());
			break;
		}
		if (available == 0) {
			break;
		}
		if (total + available > MaxResponseBody) {
			readOk = false;
			SetError(error, L"The WebDAV response is too large.");
			break;
		}
		const size_t offset = response.body.size();
		response.body.resize(offset + available);
		DWORD read = 0;
		if (!WinHttpReadData(request, response.body.data() + offset, available, &read) || read == 0) {
			readOk = false;
			SetWin32Error(error, L"Cannot read the WebDAV response", GetLastError());
			break;
		}
		response.body.resize(offset + read);
		total += read;
	}
	WinHttpCloseHandle(request);
	WinHttpCloseHandle(connection);
	WinHttpCloseHandle(session);
	return readOk;
}

bool IsIdempotentMethod(LPCWSTR method) noexcept {
	return _wcsicmp(method, L"GET") == 0 || _wcsicmp(method, L"PROPFIND") == 0
		|| _wcsicmp(method, L"DELETE") == 0 || _wcsicmp(method, L"MKCOL") == 0;
}

bool IsRetryableStatus(DWORD status) noexcept {
	return status == 408 || status == 425 || status == 429
		|| status == 500 || status == 502 || status == 503 || status == 504;
}

bool HttpRequest(const ParsedUrl &url, LPCWSTR method, const std::wstring &path,
	const std::wstring &headers, const std::vector<uint8_t> &body,
	const std::wstring &userName, const ConfigCloudCredentials &credentials,
	HttpResponse &response, std::wstring *error) noexcept {
	const int attempts = IsIdempotentMethod(method) ? 2 : 1;
	for (int attempt = 0; attempt < attempts; ++attempt) {
		response = {};
		if (error != nullptr) {
			error->clear();
		}
		const bool ok = HttpRequestOnce(url, method, path, headers, body,
			userName, credentials, response, error);
		if (!ok) {
			if (attempt + 1 >= attempts) {
				return false;
			}
			Sleep(200);
			continue;
		}
		if (!IsRetryableStatus(response.status) || attempt + 1 >= attempts) {
			return true;
		}
		Sleep(200);
	}
	return false;
}

bool EnsureRemoteFolder(const ParsedUrl &url, const std::wstring &encodedFolder,
	const std::wstring &userName, const ConfigCloudCredentials &credentials,
	std::wstring *error) noexcept {
	if (encodedFolder.empty()) {
		return true;
	}
	size_t start = 0;
	while (start <= encodedFolder.size()) {
		const size_t end = encodedFolder.find(L'/', start);
		const std::wstring segment = encodedFolder.substr(start,
			(end == std::wstring::npos ? encodedFolder.size() : end) - start);
		if (!segment.empty()) {
			const std::wstring path = BuildFolderPath(url,
				encodedFolder.substr(0, (end == std::wstring::npos ? encodedFolder.size() : end)),
				true);
			HttpResponse response;
			std::wstring requestError;
			if (!HttpRequest(url, L"MKCOL", path, L"", {}, userName, credentials,
				response, &requestError)) {
				if (error != nullptr) {
					*error = std::move(requestError);
				}
				return false;
			}
			if (response.status != 200 && response.status != 201 && response.status != 204
				&& response.status != 405) {
				SetHttpError(error, L"Cannot create the remote WebDAV folder", response.status);
				return false;
			}
		}
		if (end == std::wstring::npos) {
			break;
		}
		start = end + 1;
	}
	return true;
}

bool XmlDecode(std::wstring &text) noexcept {
	std::wstring output;
	output.reserve(text.size());
	for (size_t i = 0; i < text.size(); ++i) {
		if (text[i] != L'&') {
			output.push_back(text[i]);
			continue;
		}
		const size_t end = text.find(L';', i + 1);
		if (end == std::wstring::npos) {
			output.push_back(text[i]);
			continue;
		}
		const std::wstring entity = text.substr(i + 1, end - i - 1);
		if (entity == L"amp") {
			output.push_back(L'&');
		} else if (entity == L"lt") {
			output.push_back(L'<');
		} else if (entity == L"gt") {
			output.push_back(L'>');
		} else if (entity == L"quot") {
			output.push_back(L'"');
		} else if (entity == L"apos") {
			output.push_back(L'\'');
		} else if (entity.size() > 1 && entity[0] == L'#') {
			wchar_t *endPointer = nullptr;
			const long value = (entity[1] == L'x' || entity[1] == L'X')
				? wcstol(entity.c_str() + 2, &endPointer, 16)
				: wcstol(entity.c_str() + 1, &endPointer, 10);
			if (endPointer != nullptr && *endPointer == L'\0' && value > 0 && value <= 0x10FFFF) {
				output.push_back(static_cast<wchar_t>(value));
			} else {
				output.append(text, i, end - i + 1);
			}
		} else {
			output.append(text, i, end - i + 1);
		}
		i = end;
	}
	text = std::move(output);
	return true;
}

std::wstring PercentDecode(const std::wstring &text) noexcept {
	std::string bytes;
	bytes.reserve(text.size());
	for (size_t i = 0; i < text.size(); ++i) {
		if (text[i] == L'%' && i + 2 < text.size()) {
			const auto hex = [](wchar_t ch) -> int {
				if (ch >= L'0' && ch <= L'9') return ch - L'0';
				if (ch >= L'a' && ch <= L'f') return ch - L'a' + 10;
				if (ch >= L'A' && ch <= L'F') return ch - L'A' + 10;
				return -1;
			};
			const int high = hex(text[i + 1]);
			const int low = hex(text[i + 2]);
			if (high >= 0 && low >= 0) {
				bytes.push_back(static_cast<char>((high << 4) | low));
				i += 2;
				continue;
			}
		}
		if (text[i] < 0x80) {
			bytes.push_back(static_cast<char>(text[i]));
		} else {
			const std::string encoded = Narrow(std::wstring(1, text[i]));
			bytes += encoded;
		}
	}
	return Widen(bytes);
}

struct RemoteXmlEntry {
	std::wstring href;
	std::wstring length;
	std::wstring modified;
	bool collection = false;
};

bool ParsePropfindXml(const std::vector<uint8_t> &body,
	std::vector<ConfigCloudRemoteEntry> &entries, std::wstring *error) noexcept {
	std::string data;
	if (body.size() >= 3 && body[0] == 0xEF && body[1] == 0xBB && body[2] == 0xBF) {
		data.assign(reinterpret_cast<const char *>(body.data() + 3), body.size() - 3);
	} else {
		data.assign(reinterpret_cast<const char *>(body.data()), body.size());
	}
	const std::wstring text = Widen(data);
	if (text.empty()) {
		SetError(error, L"The WebDAV server returned an empty directory listing.");
		return false;
	}

	std::vector<RemoteXmlEntry> rawEntries;
	RemoteXmlEntry current;
	std::wstring textBuffer;
	bool inResponse = false;

	size_t position = 0;
	while (position < text.size()) {
		const size_t lt = text.find(L'<', position);
		if (lt == std::wstring::npos) {
			break;
		}
		textBuffer += text.substr(position, lt - position);
		const size_t gt = text.find(L'>', lt + 1);
		if (gt == std::wstring::npos) {
			break;
		}
		std::wstring tag = text.substr(lt + 1, gt - lt - 1);
		position = gt + 1;
		const bool closing = !tag.empty() && tag.front() == L'/';
		if (closing) {
			tag.erase(tag.begin());
		}
		const bool selfClosing = !tag.empty() && tag.back() == L'/';
		if (selfClosing) {
			tag.pop_back();
		}
		const size_t space = tag.find_first_of(L" \t\r\n");
		if (space != std::wstring::npos) {
			tag.resize(space);
		}
		if (tag.empty()) {
			textBuffer.clear();
			continue;
		}
		const size_t colon = tag.find(L':');
		if (colon != std::wstring::npos) {
			tag.erase(0, colon + 1);
		}
		std::transform(tag.begin(), tag.end(), tag.begin(),
			[](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });

		if (tag == L"response") {
			if (closing) {
				if (inResponse && !current.href.empty()) {
					rawEntries.push_back(std::move(current));
				}
				current = {};
				inResponse = false;
			} else {
				current = {};
				inResponse = true;
			}
			textBuffer.clear();
			continue;
		}
		if (!inResponse) {
			textBuffer.clear();
			continue;
		}
		if (tag == L"collection") {
			current.collection = true;
			textBuffer.clear();
			continue;
		}
		if (tag == L"href" || tag == L"getcontentlength" || tag == L"getlastmodified") {
			if (closing) {
				std::wstring value = TrimCopy(textBuffer);
				if (tag == L"href") {
					current.href = value;
				} else if (tag == L"getcontentlength") {
					current.length = value;
				} else {
					current.modified = value;
				}
				textBuffer.clear();
			} else if (!selfClosing) {
				textBuffer.clear();
			}
			continue;
		}
		textBuffer.clear();
	}

	entries.clear();
	for (RemoteXmlEntry &raw : rawEntries) {
		XmlDecode(raw.href);
		std::wstring href = raw.href;
		const size_t query = href.find_first_of(L"?#");
		if (query != std::wstring::npos) {
			href.resize(query);
		}
		if (href.empty() || href.back() == L'/') {
			continue;
		}
		const size_t slash = href.find_last_of(L'/');
		const std::wstring encodedName = slash == std::wstring::npos ? href : href.substr(slash + 1);
		const std::wstring name = PercentDecode(encodedName);
		if (name.size() < wcslen(CloudRemotePrefix) + wcslen(CloudRemoteSuffix)) {
			continue;
		}
		if (_wcsnicmp(name.c_str(), CloudRemotePrefix, wcslen(CloudRemotePrefix)) != 0
			|| _wcsicmp(name.c_str() + name.size() - wcslen(CloudRemoteSuffix), CloudRemoteSuffix) != 0) {
			continue;
		}
		ConfigCloudRemoteEntry entry;
		entry.name = name;
		entry.size = _wcstoui64(raw.length.c_str(), nullptr, 10);
		entry.modified = std::move(raw.modified);
		entries.push_back(std::move(entry));
	}
	std::sort(entries.begin(), entries.end(), [](const ConfigCloudRemoteEntry &a,
		const ConfigCloudRemoteEntry &b) noexcept {
		SYSTEMTIME aTime {};
		SYSTEMTIME bTime {};
		const bool aOk = WinHttpTimeToSystemTime(a.modified.c_str(), &aTime) != FALSE;
		const bool bOk = WinHttpTimeToSystemTime(b.modified.c_str(), &bTime) != FALSE;
		if (aOk && bOk) {
			FILETIME aFile {};
			FILETIME bFile {};
			if (SystemTimeToFileTime(&aTime, &aFile) && SystemTimeToFileTime(&bTime, &bFile)) {
				const int compare = CompareFileTime(&aFile, &bFile);
				if (compare != 0) {
					return compare > 0;
				}
			}
		}
		return _wcsicmp(a.name.c_str(), b.name.c_str()) > 0;
	});
	return true;
}

std::wstring DeviceTag() noexcept {
	const std::string id = ConfigBackup_DeviceId();
	std::wstring tag;
	for (unsigned char ch : id) {
		if (isxdigit(ch) && tag.size() < 8) {
			tag.push_back(static_cast<wchar_t>(towupper(ch)));
		}
	}
	return tag.empty() ? L"UNKNOWN" : tag;
}

std::wstring MakeRemoteName(bool automatic) noexcept {
	SYSTEMTIME time {};
	GetLocalTime(&time);
	uint8_t random[2] = {0, 0};
	RandomBytes(random, sizeof(random), nullptr);
	wchar_t buffer[160];
	if (automatic) {
		const std::wstring tag = DeviceTag();
		swprintf_s(buffer, L"%s%s-%04u%02u%02u-%02u%02u%02u%03u-%02X%02X%s",
			CloudAutoPrefix, tag.c_str(), time.wYear, time.wMonth, time.wDay, time.wHour,
			time.wMinute, time.wSecond, time.wMilliseconds,
			random[0], random[1], CloudRemoteSuffix);
	} else {
		swprintf_s(buffer, L"%s%04u%02u%02u-%02u%02u%02u-%02X%02X%s",
			CloudRemotePrefix, time.wYear, time.wMonth, time.wDay, time.wHour,
			time.wMinute, time.wSecond, random[0], random[1], CloudRemoteSuffix);
	}
	return buffer;
}

bool UploadInternal(const ConfigCloudSettings &settings,
	const ConfigCloudCredentials &credentials, bool includeHistory, bool automatic,
	ConfigCloudUploadResult *result, std::wstring *error) noexcept {
	if (error != nullptr) {
		error->clear();
	}
	if (result != nullptr) {
		*result = {};
	}
	if (credentials.passphrase.empty()) {
		SetError(error, L"An encryption passphrase is required.");
		return false;
	}
	ParsedUrl url;
	if (!ParseUrl(settings.url, url, error)) {
		return false;
	}
	std::wstring folder;
	if (!NormalizeFolder(settings.remoteFolder, folder, error)) {
		return false;
	}

	if (includeHistory) {
		SaveHistoryForBackup();
	}
	std::wstring zipPath = MakeTemporaryPath(L".zip", error);
	if (zipPath.empty()) {
		return false;
	}
	TempFile zipFile(zipPath);
	ConfigBackupOptions options;
	options.includeHistory = includeHistory;
	ConfigBackupResult backup;
	if (!ConfigBackup_Create(zipPath.c_str(), options, &backup, error)) {
		return false;
	}
	std::vector<uint8_t> zipData;
	if (!ReadAllBytes(zipPath.c_str(), zipData, error)) {
		return false;
	}
	std::vector<uint8_t> encrypted;
	if (!AesGcmEncrypt(zipData, credentials.passphrase, encrypted, error)) {
		SecureZeroMemory(zipData.data(), zipData.size());
		return false;
	}
	SecureZeroMemory(zipData.data(), zipData.size());

	std::wstring encryptedPath = MakeTemporaryPath(L".np4enc", error);
	if (encryptedPath.empty()) {
		return false;
	}
	TempFile encryptedFile(encryptedPath);
	if (!WriteAllBytesAtomic(encryptedPath.c_str(), encrypted, error)) {
		return false;
	}
	if (!EnsureRemoteFolder(url, folder, settings.userName, credentials, error)) {
		return false;
	}

	std::wstring remoteName;
	HttpResponse response;
	for (int attempt = 0; attempt < 2; ++attempt) {
		remoteName = MakeRemoteName(automatic);
		const std::wstring remotePath = BuildFolderPath(url, folder, true)
			+ EncodePathSegment(remoteName);
		if (!HttpRequest(url, L"PUT", remotePath,
			L"Content-Type: application/octet-stream\r\nIf-None-Match: *\r\n", encrypted,
			settings.userName, credentials, response, error)) {
			return false;
		}
		if (response.status == 412 && attempt == 0) {
			continue;
		}
		break;
	}
	if (!IsHttpSuccess(response.status)) {
		SetHttpError(error, L"Cannot upload the encrypted WebDAV backup", response.status);
		return false;
	}
	if (result != nullptr) {
		result->remoteName = remoteName;
		result->encryptedSize = encrypted.size();
	}
	return true;
}

} // namespace

ConfigCloudSettings &ConfigCloud_Settings() noexcept {
	return g_CloudSettings;
}

void ConfigCloud_SaveSettingsToIni() noexcept {
	if (szIniFile[0] == L'\0') {
		return;
	}
	const ConfigCloudSettings &settings = g_CloudSettings;
	IniSetString(INI_SECTION_NAME_SETTINGS, L"CloudUrl", settings.url.c_str());
	IniSetString(INI_SECTION_NAME_SETTINGS, L"CloudUser", settings.userName.c_str());
	IniSetString(INI_SECTION_NAME_SETTINGS, L"CloudFolder", settings.remoteFolder.c_str());
	IniSetBoolEx(INI_SECTION_NAME_SETTINGS, L"CloudSaveCredentials", settings.saveCredentials, true);
}

bool ConfigCloud_LoadCredentials(ConfigCloudCredentials *credentials,
	std::wstring *error) noexcept {
	if (credentials == nullptr) {
		SetError(error, L"No credentials destination was provided.");
		return false;
	}
	return LoadProtectedCredentials(*credentials, error);
}

bool ConfigCloud_SaveCredentials(const ConfigCloudCredentials &credentials,
	std::wstring *error) noexcept {
	return SaveProtectedCredentials(credentials, error);
}

void ConfigCloud_CleanupTemporaryFiles() noexcept {
	CleanupTemporaryFiles();
}

void ConfigCloud_DeleteCredentials() noexcept {
	const std::wstring privateDirectory = GetPrivateCloudDirectory();
	if (!privateDirectory.empty()) {
		DeleteFileW((privateDirectory + L"\\credentials.bin").c_str());
	}
	const std::wstring legacyDirectory = GetCloudDirectory();
	if (!legacyDirectory.empty() && _wcsicmp(privateDirectory.c_str(), legacyDirectory.c_str()) != 0) {
		DeleteFileW((legacyDirectory + L"\\credentials.bin").c_str());
	}
}

bool ConfigCloud_TestConnection(const ConfigCloudSettings &settings,
	const ConfigCloudCredentials &credentials, std::wstring *error) noexcept {
	ParsedUrl url;
	if (!ParseUrl(settings.url, url, error)) {
		return false;
	}
	std::wstring folder;
	if (!NormalizeFolder(settings.remoteFolder, folder, error)) {
		return false;
	}
	if (!EnsureRemoteFolder(url, folder, settings.userName, credentials, error)) {
		return false;
	}
	const std::wstring path = BuildFolderPath(url, folder, true);
	const std::string xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
		"<d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/></d:prop></d:propfind>";
	const std::vector<uint8_t> body(xml.begin(), xml.end());
	HttpResponse response;
	if (!HttpRequest(url, L"PROPFIND", path,
		L"Depth: 0\r\nContent-Type: application/xml; charset=utf-8\r\n",
		body, settings.userName, credentials, response, error)) {
		return false;
	}
	if (!IsHttpSuccess(response.status)) {
		SetHttpError(error, L"Cannot access the remote WebDAV folder", response.status);
		return false;
	}
	return true;
}

bool ConfigCloud_ListRemote(const ConfigCloudSettings &settings,
	const ConfigCloudCredentials &credentials,
	std::vector<ConfigCloudRemoteEntry> &entries, std::wstring *error) noexcept {
	entries.clear();
	ParsedUrl url;
	if (!ParseUrl(settings.url, url, error)) {
		return false;
	}
	std::wstring folder;
	if (!NormalizeFolder(settings.remoteFolder, folder, error)) {
		return false;
	}
	const std::wstring path = BuildFolderPath(url, folder, true);
	const std::string xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
		"<d:propfind xmlns:d=\"DAV:\"><d:prop><d:getcontentlength/>"
		"<d:getlastmodified/><d:resourcetype/></d:prop></d:propfind>";
	const std::vector<uint8_t> body(xml.begin(), xml.end());
	HttpResponse response;
	if (!HttpRequest(url, L"PROPFIND", path,
		L"Depth: 1\r\nContent-Type: application/xml; charset=utf-8\r\n",
		body, settings.userName, credentials, response, error)) {
		return false;
	}
	if (response.status == 404) {
		SetError(error, L"The remote WebDAV folder does not exist.");
		return false;
	}
	if (!IsHttpSuccess(response.status)) {
		SetHttpError(error, L"Cannot list the remote WebDAV folder", response.status);
		return false;
	}
	return ParsePropfindXml(response.body, entries, error);
}

bool ConfigCloud_UploadCurrent(const ConfigCloudSettings &settings,
	const ConfigCloudCredentials &credentials, bool includeHistory,
	ConfigCloudUploadResult *result, std::wstring *error) noexcept {
	return UploadInternal(settings, credentials, includeHistory, false, result, error);
}

bool ConfigCloud_UploadAutomatic(const ConfigCloudSettings &settings,
	const ConfigCloudCredentials &credentials, bool includeHistory,
	ConfigCloudUploadResult *result, std::wstring *error) noexcept {
	return UploadInternal(settings, credentials, includeHistory, true, result, error);
}

bool ConfigCloud_DeleteRemote(const ConfigCloudSettings &settings,
	const ConfigCloudCredentials &credentials, LPCWSTR remoteName,
	std::wstring *error) noexcept {
	if (remoteName == nullptr || remoteName[0] == L'\0'
		|| _wcsnicmp(remoteName, CloudRemotePrefix, wcslen(CloudRemotePrefix)) != 0
		|| wcslen(remoteName) < wcslen(CloudRemotePrefix) + wcslen(CloudRemoteSuffix)
		|| _wcsicmp(remoteName + wcslen(remoteName) - wcslen(CloudRemoteSuffix),
			CloudRemoteSuffix) != 0
		|| wcschr(remoteName, L'/') != nullptr || wcschr(remoteName, L'\\') != nullptr) {
		SetError(error, L"The remote backup name is invalid.");
		return false;
	}
	ParsedUrl url;
	if (!ParseUrl(settings.url, url, error)) {
		return false;
	}
	std::wstring folder;
	if (!NormalizeFolder(settings.remoteFolder, folder, error)) {
		return false;
	}
	const std::wstring path = BuildFolderPath(url, folder, true)
		+ EncodePathSegment(remoteName);
	HttpResponse response;
	if (!HttpRequest(url, L"DELETE", path, L"", {}, settings.userName,
		credentials, response, error)) {
		return false;
	}
	if (response.status == 404 || response.status == 200 || response.status == 204) {
		return true;
	}
	SetHttpError(error, L"Cannot delete the remote WebDAV backup", response.status);
	return false;
}

bool ConfigCloud_DownloadRemote(const ConfigCloudSettings &settings,
	const ConfigCloudCredentials &credentials, LPCWSTR remoteName,
	std::wstring &localZipPath, std::wstring *error) noexcept {
	localZipPath.clear();
	if (remoteName == nullptr || remoteName[0] == L'\0'
		|| _wcsnicmp(remoteName, CloudRemotePrefix, wcslen(CloudRemotePrefix)) != 0
		|| wcslen(remoteName) < wcslen(CloudRemotePrefix) + wcslen(CloudRemoteSuffix)
		|| _wcsicmp(remoteName + wcslen(remoteName) - wcslen(CloudRemoteSuffix),
			CloudRemoteSuffix) != 0
		|| wcschr(remoteName, L'/') != nullptr || wcschr(remoteName, L'\\') != nullptr) {
		SetError(error, L"The remote backup name is invalid.");
		return false;
	}
	ParsedUrl url;
	if (!ParseUrl(settings.url, url, error)) {
		return false;
	}
	std::wstring folder;
	if (!NormalizeFolder(settings.remoteFolder, folder, error)) {
		return false;
	}
	const std::wstring remotePath = BuildFolderPath(url, folder, true)
		+ EncodePathSegment(remoteName);
	HttpResponse response;
	if (!HttpRequest(url, L"GET", remotePath, L"", {}, settings.userName,
		credentials, response, error)) {
		return false;
	}
	if (response.status != 200) {
		SetHttpError(error, L"Cannot download the encrypted WebDAV backup", response.status);
		return false;
	}
	std::vector<uint8_t> plain;
	if (!AesGcmDecrypt(response.body, credentials.passphrase, plain, error)) {
		return false;
	}
	std::wstring zipPath = MakeTemporaryPath(L".zip", error);
	if (zipPath.empty()) {
		return false;
	}
	if (!WriteAllBytesAtomic(zipPath.c_str(), plain, error)) {
		SecureZeroMemory(plain.data(), plain.size());
		return false;
	}
	SecureZeroMemory(plain.data(), plain.size());
	localZipPath = zipPath;
	return true;
}

bool ConfigCloud_DownloadLatest(const ConfigCloudSettings &settings,
	const ConfigCloudCredentials &credentials, std::wstring &localZipPath,
	std::wstring &remoteName, std::wstring *error) noexcept {
	localZipPath.clear();
	remoteName.clear();
	std::vector<ConfigCloudRemoteEntry> entries;
	if (!ConfigCloud_ListRemote(settings, credentials, entries, error)) {
		return false;
	}
	if (entries.empty()) {
		SetError(error, L"No encrypted WebDAV backups were found.");
		return false;
	}
	remoteName = entries.front().name;
	return ConfigCloud_DownloadRemote(settings, credentials, remoteName.c_str(),
		localZipPath, error);
}
