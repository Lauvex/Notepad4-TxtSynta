/******************************************************************************
*
* Notepad4-TxtSyntax
*
* ConfigCloudAuto.cpp
*   Automatic encrypted WebDAV snapshots, remote retention and status.
*
******************************************************************************/

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "SciCall.h"
#include "config.h"
#include "Helpers.h"
#include "Notepad4.h"
#include "ConfigCloud.h"
#include "ConfigBackup.h"
#include "ConfigCloudAuto.h"

namespace {

std::wstring GetAutomaticPrefix() noexcept {
	const std::string id = ConfigBackup_DeviceId();
	std::wstring tag;
	for (unsigned char ch : id) {
		if (isxdigit(ch) && tag.size() < 8) {
			tag.push_back(static_cast<wchar_t>(towupper(ch)));
		}
	}
	if (tag.empty()) {
		tag = L"UNKNOWN";
	}
	return std::wstring(L"Notepad4-Cloud-Auto-") + tag + L"-";
}
constexpr int MinIntervalHours = 1;
constexpr int MaxIntervalHours = 24 * 30;
constexpr int MinKeepCount = 1;
constexpr int MaxKeepCount = 50;

ConfigCloudAutoSettings g_AutoCloudSettings;
std::wstring g_LastError;
std::mutex g_StatusMutex;

HANDLE g_AsyncThread = nullptr;
HANDLE g_AsyncCancel = nullptr;
std::atomic<bool> g_AsyncRunning {false};

struct AsyncJob {
	ConfigCloudAutoSettings settings;
	ConfigCloudSettings cloud;
};

class CloudAutoLock {
public:
	CloudAutoLock() noexcept {
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
		swprintf_s(name, L"Local\\Notepad4-TxtSyntax.CloudAuto.%08X", hash);
		handle_ = CreateMutexW(nullptr, FALSE, name);
		if (handle_ != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
			CloseHandle(handle_);
			handle_ = nullptr;
			busy_ = true;
		}
	}
	~CloudAutoLock() noexcept {
		if (handle_ != nullptr) {
			CloseHandle(handle_);
		}
	}
	bool Busy() const noexcept { return busy_; }

private:
	HANDLE handle_ = nullptr;
	bool busy_ = false;
};

void SetError(std::wstring *error, const wchar_t *message) noexcept {
	{
		std::lock_guard<std::mutex> lock(g_StatusMutex);
		g_LastError = message != nullptr ? message : L"";
	}
	if (error != nullptr) {
		*error = message != nullptr ? message : L"";
	}
}

void ClearError() noexcept {
	std::lock_guard<std::mutex> lock(g_StatusMutex);
	g_LastError.clear();
}

bool IsCanceled() noexcept {
	return g_AsyncCancel != nullptr && WaitForSingleObject(g_AsyncCancel, 0) == WAIT_OBJECT_0;
}

bool IsAutomaticName(LPCWSTR name) noexcept {
	if (name == nullptr) {
		return false;
	}
	const std::wstring prefix = GetAutomaticPrefix();
	return _wcsnicmp(name, prefix.c_str(), prefix.size()) == 0;
}

bool ParseTimestamp(const std::wstring &name, FILETIME &fileTime) noexcept {
	const std::wstring prefix = GetAutomaticPrefix();
	if (_wcsnicmp(name.c_str(), prefix.c_str(), prefix.size()) != 0) {
		return false;
	}
	const wchar_t *timestamp = name.c_str() + prefix.size();
	unsigned year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
	unsigned milliseconds = 0;
	const int fields = swscanf_s(timestamp, L"%4u%2u%2u-%2u%2u%2u%3u",
		&year, &month, &day, &hour, &minute, &second, &milliseconds);
	if (fields != 7) {
		milliseconds = 0;
		if (swscanf_s(timestamp, L"%4u%2u%2u-%2u%2u%2u",
			&year, &month, &day, &hour, &minute, &second) != 6) {
			return false;
		}
	}
	if (milliseconds > 999) {
		milliseconds = 0;
	}
	SYSTEMTIME time {};
	time.wYear = static_cast<WORD>(year);
	time.wMonth = static_cast<WORD>(month);
	time.wDay = static_cast<WORD>(day);
	time.wHour = static_cast<WORD>(hour);
	time.wMinute = static_cast<WORD>(minute);
	time.wSecond = static_cast<WORD>(second);
	time.wMilliseconds = static_cast<WORD>(milliseconds);
	return SystemTimeToFileTime(&time, &fileTime) != FALSE;
}

bool LoadCredentials(ConfigCloudCredentials &credentials, std::wstring *error) noexcept {
	if (!ConfigCloud_LoadCredentials(&credentials, error)) {
		SetError(error, L"Cannot load the stored WebDAV credentials.");
		return false;
	}
	if (credentials.password.empty() || credentials.passphrase.empty()) {
		SetError(error, L"Stored WebDAV password or encryption passphrase is missing.");
		return false;
	}
	return true;
}

bool ListAutomaticWithCredentials(const ConfigCloudSettings &cloud,
	const ConfigCloudCredentials &credentials,
	std::vector<ConfigCloudRemoteEntry> &entries, std::wstring *error) noexcept {
	std::vector<ConfigCloudRemoteEntry> all;
	if (!ConfigCloud_ListRemote(cloud, credentials, all, error)) {
		return false;
	}
	entries.clear();
	for (ConfigCloudRemoteEntry &entry : all) {
		if (IsAutomaticName(entry.name.c_str())) {
			entries.push_back(std::move(entry));
		}
	}
	return true;
}

bool ApplyRetention(const ConfigCloudAutoSettings &settings,
	const ConfigCloudSettings &cloud, const ConfigCloudCredentials &credentials,
	std::wstring *error) noexcept {
	std::vector<ConfigCloudRemoteEntry> entries;
	if (!ListAutomaticWithCredentials(cloud, credentials, entries, error)) {
		return false;
	}
	const int keep = std::clamp(settings.keepCount, MinKeepCount, MaxKeepCount);
	for (size_t i = static_cast<size_t>(keep); i < entries.size(); ++i) {
		std::wstring deleteError;
		if (!ConfigCloud_DeleteRemote(cloud, credentials,
			entries[i].name.c_str(), &deleteError)) {
			SetError(error, L"Cannot remove an old automatic remote backup.");
			return false;
		}
	}
	return true;
}

bool UploadInternal(const ConfigCloudAutoSettings &settings,
	const ConfigCloudSettings &cloud, const ConfigCloudCredentials &credentials,
	ConfigCloudUploadResult *result, std::wstring *error) noexcept {
	ConfigCloudUploadResult localResult;
	ConfigCloudUploadResult *out = result != nullptr ? result : &localResult;
	if (!ConfigCloud_UploadAutomatic(cloud, credentials,
		settings.includeHistory, out, error)) {
		if (error != nullptr && !error->empty()) {
			std::lock_guard<std::mutex> lock(g_StatusMutex);
			g_LastError = *error;
		} else {
			SetError(error, L"Cannot upload the automatic cloud backup.");
		}
		return false;
	}
	if (IsCanceled()) {
		SetError(error, L"Automatic cloud backup cancelled.");
		return false;
	}
	if (!ApplyRetention(settings, cloud, credentials, error)) {
		// The upload is already complete; cleanup is best effort and must not
		// turn a successful snapshot into a failed one.
		return true;
	}
	ClearError();
	return true;
}

bool IsDue(const ConfigCloudAutoSettings &settings, const ConfigCloudSettings &cloud,
	const ConfigCloudCredentials &credentials, std::wstring *error) noexcept {
	std::vector<ConfigCloudRemoteEntry> entries;
	if (!ListAutomaticWithCredentials(cloud, credentials, entries, error)) {
		return false;
	}
	if (entries.empty()) {
		return true;
	}
	FILETIME newest {};
	if (!ParseTimestamp(entries.front().name, newest)) {
		return true;
	}
	SYSTEMTIME localNow {};
	GetLocalTime(&localNow);
	FILETIME now {};
	if (!SystemTimeToFileTime(&localNow, &now)) {
		return true;
	}
	ULARGE_INTEGER newestValue {};
	newestValue.LowPart = newest.dwLowDateTime;
	newestValue.HighPart = newest.dwHighDateTime;
	ULARGE_INTEGER nowValue {};
	nowValue.LowPart = now.dwLowDateTime;
	nowValue.HighPart = now.dwHighDateTime;
	const int hours = std::clamp(settings.intervalHours, MinIntervalHours, MaxIntervalHours);
	const uint64_t interval = static_cast<uint64_t>(hours) * 3600ULL * 10000000ULL;
	return newestValue.QuadPart + interval <= nowValue.QuadPart;
}

std::wstring FingerprintPath() noexcept {
	const std::wstring data = GetNotepad4PrivateDataDirectory();
	if (data.empty()) {
		return {};
	}
	return data + L"\\Cloud\\.auto-fingerprint";
}

bool RunOnce(const ConfigCloudAutoSettings &settings, const ConfigCloudSettings &cloud,
	bool force, ConfigCloudUploadResult *result, bool *uploaded, std::wstring *error) noexcept {
	if (uploaded != nullptr) {
		*uploaded = false;
	}
	if (!settings.enabled && !force) {
		ClearError();
		return true;
	}
	if (cloud.url.empty()) {
		SetError(error, L"WebDAV URL is not configured.");
		return false;
	}
	CloudAutoLock lock;
	if (lock.Busy()) {
		SetError(error, L"Another automatic cloud backup is already running.");
		return false;
	}

	ConfigCloudCredentials credentials;
	if (!LoadCredentials(credentials, error)) {
		return false;
	}
	std::string fingerprint;
	const std::wstring statePath = FingerprintPath();
	const bool hasFingerprint = !statePath.empty()
		&& ConfigBackup_ComputeFingerprint(settings.includeHistory, fingerprint, nullptr);
	bool fingerprintChanged = false;
	if (!force && hasFingerprint) {
		std::string lastFingerprint;
		if (ConfigBackup_ReadFingerprint(statePath.c_str(), lastFingerprint)) {
			if (lastFingerprint == fingerprint) {
				ClearError();
				return true;
			}
			fingerprintChanged = true;
		} else {
			fingerprintChanged = true;
		}
	}
	if (IsCanceled()) {
		SetError(error, L"Automatic cloud backup cancelled.");
		return false;
	}
	if (!force && !fingerprintChanged) {
		std::wstring dueError;
		if (!IsDue(settings, cloud, credentials, &dueError)) {
			if (!dueError.empty()) {
				SetError(error, dueError.c_str());
				return false;
			}
			return true;
		}
	}
	if (IsCanceled()) {
		SetError(error, L"Automatic cloud backup cancelled.");
		return false;
	}
	const bool uploadedOk = UploadInternal(settings, cloud, credentials, result, error);
	if (!uploadedOk) {
		if (error != nullptr) {
		}
		return false;
	}
	if (hasFingerprint) {
		std::wstring stateError;
		ConfigBackup_WriteFingerprint(statePath.c_str(), fingerprint, &stateError);
	}
	if (uploaded != nullptr) {
		*uploaded = true;
	}
	return true;
}

DWORD WINAPI AsyncThreadProc(void *parameter) noexcept {
	AsyncJob *job = static_cast<AsyncJob *>(parameter);
	std::wstring error;
	bool uploaded = false;
	const bool ok = RunOnce(job->settings, job->cloud, false, nullptr, &uploaded, &error);
	if (IsCanceled()) {
		SetError(nullptr, L"Automatic cloud backup cancelled.");
	} else if (!ok && error.empty()) {
		SetError(nullptr, L"Automatic cloud backup failed.");
	}
	delete job;
	g_AsyncRunning.store(false);
	return 0;
}

} // namespace

ConfigCloudAutoSettings &ConfigCloudAuto_Settings() noexcept {
	return g_AutoCloudSettings;
}

void ConfigCloudAuto_SavePolicyToIni() noexcept {
	if (szIniFile[0] == L'\0') {
		return;
	}
	ConfigCloudAutoSettings &settings = g_AutoCloudSettings;
	settings.intervalHours = std::clamp(settings.intervalHours, MinIntervalHours, MaxIntervalHours);
	settings.keepCount = std::clamp(settings.keepCount, MinKeepCount, MaxKeepCount);
	IniSetBoolEx(INI_SECTION_NAME_SETTINGS, L"CloudAutoBackup", settings.enabled, false);
	IniSetIntEx(INI_SECTION_NAME_SETTINGS, L"CloudAutoIntervalHours", settings.intervalHours, 24);
	IniSetIntEx(INI_SECTION_NAME_SETTINGS, L"CloudAutoKeep", settings.keepCount, 10);
	IniSetBoolEx(INI_SECTION_NAME_SETTINGS, L"CloudAutoHistory", settings.includeHistory, false);
}

bool ConfigCloudAuto_MaybeRun(bool *uploaded, std::wstring *error) noexcept {
	const ConfigCloudAutoSettings settings = g_AutoCloudSettings;
	const ConfigCloudSettings cloud = ConfigCloud_Settings();
	return RunOnce(settings, cloud, false, nullptr, uploaded, error);
}

bool ConfigCloudAuto_MaybeRunAsync() noexcept {
	if (!g_AutoCloudSettings.enabled) {
		ClearError();
		return true;
	}
	if (g_AsyncRunning.exchange(true)) {
		return true;
	}
	if (g_AsyncCancel == nullptr) {
		g_AsyncCancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (g_AsyncCancel == nullptr) {
			g_AsyncRunning.store(false);
			return false;
		}
	}
	ResetEvent(g_AsyncCancel);
	if (g_AsyncThread != nullptr) {
		CloseHandle(g_AsyncThread);
		g_AsyncThread = nullptr;
	}
	AsyncJob *job = new (std::nothrow) AsyncJob();
	if (job == nullptr) {
		g_AsyncRunning.store(false);
		SetError(nullptr, L"Cannot allocate the automatic cloud backup job.");
		return false;
	}
	job->settings = g_AutoCloudSettings;
	job->cloud = ConfigCloud_Settings();
	g_AsyncThread = CreateThread(nullptr, 0, AsyncThreadProc, job, 0, nullptr);
	if (g_AsyncThread == nullptr) {
		delete job;
		g_AsyncRunning.store(false);
		SetError(nullptr, L"Cannot start the automatic cloud backup worker.");
		return false;
	}
	return true;
}

void ConfigCloudAuto_ShutdownAsync() noexcept {
	if (g_AsyncCancel != nullptr) {
		SetEvent(g_AsyncCancel);
	}
	if (g_AsyncThread != nullptr) {
		WaitForSingleObject(g_AsyncThread, 1000);
		CloseHandle(g_AsyncThread);
		g_AsyncThread = nullptr;
	}
}

bool ConfigCloudAuto_IsRunning() noexcept {
	return g_AsyncRunning.load();
}

bool ConfigCloudAuto_UploadNow(ConfigCloudUploadResult *result,
	std::wstring *error) noexcept {
	const ConfigCloudAutoSettings settings = g_AutoCloudSettings;
	const ConfigCloudSettings cloud = ConfigCloud_Settings();
	return RunOnce(settings, cloud, true, result, nullptr, error);
}

std::wstring ConfigCloudAuto_LastError() noexcept {
	std::lock_guard<std::mutex> lock(g_StatusMutex);
	return g_LastError;
}
