/******************************************************************************
*
* Notepad4-TxtSyntax
*
* ConfigCloudUI.cpp
*   Native WebDAV encrypted backup dialog.
*
******************************************************************************/

#include <windows.h>
#include <windowsx.h>
#include <winhttp.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <commctrl.h>

#include <algorithm>
#include <new>
#include <string>
#include <vector>

#include "SciCall.h"
#include "config.h"
#include "Helpers.h"
#include "Notepad4.h"
#include "ConfigCloud.h"
#include "ConfigRestore.h"
#include "resource.h"

namespace {

void LoadStringResource(UINT id, LPCWSTR fallback, LPWSTR buffer, int capacity) noexcept {
	if (capacity <= 0) {
		return;
	}
	if (LoadStringW(g_hInstance, id, buffer, capacity) <= 0) {
		lstrcpynW(buffer, fallback, capacity);
	}
}

std::wstring GetControlText(HWND hwnd, int id) noexcept {
	WCHAR buffer[4096] = L"";
	if (GetDlgItemTextW(hwnd, id, buffer, ARRAYSIZE(buffer)) <= 0) {
		return {};
	}
	return buffer;
}

std::wstring Trim(const std::wstring &text) noexcept {
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

void SetStatus(HWND hwnd, const std::wstring &text) noexcept {
	SetDlgItemTextW(hwnd, IDC_CLOUD_STATUS, text.c_str());
}

void SetStatusResource(HWND hwnd, UINT id, LPCWSTR fallback) noexcept {
	WCHAR message[512];
	LoadStringResource(id, fallback, message, ARRAYSIZE(message));
	SetStatus(hwnd, message);
}

void SetStatusFormat(HWND hwnd, UINT formatId, LPCWSTR fallback,
	const std::wstring &detail) noexcept {
	WCHAR format[2048];
	LoadStringResource(formatId, fallback, format, ARRAYSIZE(format));
	WCHAR message[4096];
	if (swprintf_s(message, format, detail.empty() ? L"Unknown error." : detail.c_str()) < 0) {
		lstrcpynW(message, detail.c_str(), ARRAYSIZE(message));
	}
	SetStatus(hwnd, message);
}

void ReadFields(HWND hwnd, ConfigCloudSettings &settings,
	ConfigCloudCredentials &credentials) noexcept {
	settings.url = Trim(GetControlText(hwnd, IDC_CLOUD_URL));
	settings.userName = Trim(GetControlText(hwnd, IDC_CLOUD_USER));
	settings.remoteFolder = Trim(GetControlText(hwnd, IDC_CLOUD_FOLDER));
	settings.saveCredentials = IsDlgButtonChecked(hwnd, IDC_CLOUD_SAVE_CREDENTIALS) == BST_CHECKED;
	credentials.password = GetControlText(hwnd, IDC_CLOUD_PASSWORD);
	credentials.passphrase = GetControlText(hwnd, IDC_CLOUD_PASSPHRASE);
}

bool SaveSettings(const ConfigCloudSettings &settings,
	const ConfigCloudCredentials &credentials, bool explicitSave,
	std::wstring *error) noexcept {
	ConfigCloud_Settings() = settings;
	ConfigCloud_SaveSettingsToIni();
	if (settings.saveCredentials) {
		if (!ConfigCloud_SaveCredentials(credentials, error)) {
			return false;
		}
	} else if (explicitSave) {
		ConfigCloud_DeleteCredentials();
	}
	return true;
}

void InitializeDialog(HWND hwnd) noexcept {
	CenterDlgInParent(hwnd);
	const ConfigCloudSettings &settings = ConfigCloud_Settings();
	SetDlgItemTextW(hwnd, IDC_CLOUD_URL, settings.url.c_str());
	SetDlgItemTextW(hwnd, IDC_CLOUD_USER, settings.userName.c_str());
	SetDlgItemTextW(hwnd, IDC_CLOUD_FOLDER, settings.remoteFolder.c_str());
	CheckDlgButton(hwnd, IDC_CLOUD_SAVE_CREDENTIALS,
		settings.saveCredentials ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(hwnd, IDC_CLOUD_HISTORY, BST_UNCHECKED);

	if (settings.saveCredentials) {
		ConfigCloudCredentials credentials;
		std::wstring error;
		if (ConfigCloud_LoadCredentials(&credentials, &error)) {
			SetDlgItemTextW(hwnd, IDC_CLOUD_PASSWORD, credentials.password.c_str());
			SetDlgItemTextW(hwnd, IDC_CLOUD_PASSPHRASE, credentials.passphrase.c_str());
			SecureZeroMemory(credentials.password.data(), credentials.password.size() * sizeof(wchar_t));
			SecureZeroMemory(credentials.passphrase.data(), credentials.passphrase.size() * sizeof(wchar_t));
		} else {
			SetStatusFormat(hwnd, IDS_CLOUD_CREDENTIALS_FAIL,
				L"Cannot load the stored cloud credentials.\n\n%s", error);
		}
	}
	if (GetControlText(hwnd, IDC_CLOUD_STATUS).empty()) {
		WCHAR ready[512];
		LoadStringResource(IDS_CLOUD_READY,
			L"Enter the WebDAV and encryption details. Payloads are encrypted with AES-256-GCM.",
			ready, ARRAYSIZE(ready));
		SetStatus(hwnd, ready);
	}
	SetFocus(GetDlgItem(hwnd, IDC_CLOUD_URL));
}

void TestConnection(HWND hwnd) noexcept {
	ConfigCloudSettings settings;
	ConfigCloudCredentials credentials;
	ReadFields(hwnd, settings, credentials);
	if (settings.url.empty()) {
		SetStatusResource(hwnd, IDS_CLOUD_URL_REQUIRED, L"A WebDAV URL is required.");
		return;
	}
	std::wstring error;
	BeginWaitCursor();
	const bool ok = ConfigCloud_TestConnection(settings, credentials, &error);
	EndWaitCursor();
	if (!ok) {
		SetStatusFormat(hwnd, IDS_CLOUD_TEST_FAIL,
			L"WebDAV connection failed.\n\n%s", error);
		return;
	}
	WCHAR success[512];
	LoadStringResource(IDS_CLOUD_TEST_OK,
		L"WebDAV connection and remote folder are available.",
		success, ARRAYSIZE(success));
	SetStatus(hwnd, success);
}

void UploadBackup(HWND hwnd) noexcept {
	ConfigCloudSettings settings;
	ConfigCloudCredentials credentials;
	ReadFields(hwnd, settings, credentials);
	if (settings.url.empty()) {
		SetStatusResource(hwnd, IDS_CLOUD_URL_REQUIRED, L"A WebDAV URL is required.");
		return;
	}
	if (credentials.passphrase.empty()) {
		WCHAR message[512];
		LoadStringResource(IDS_CLOUD_NEED_PASSPHRASE,
			L"Enter an encryption passphrase.", message, ARRAYSIZE(message));
		SetStatus(hwnd, message);
		return;
	}
	std::wstring error;
	if (!SaveSettings(settings, credentials, false, &error)) {
		SetStatusFormat(hwnd, IDS_CLOUD_CREDENTIALS_FAIL,
			L"Cannot save the cloud credentials.\n\n%s", error);
		return;
	}
	const bool includeHistory = IsDlgButtonChecked(hwnd, IDC_CLOUD_HISTORY) == BST_CHECKED;
	ConfigCloudUploadResult result;
	BeginWaitCursor();
	const bool ok = ConfigCloud_UploadCurrent(settings, credentials, includeHistory,
		&result, &error);
	EndWaitCursor();
	if (!ok) {
		SetStatusFormat(hwnd, IDS_CLOUD_UPLOAD_FAIL,
			L"Cannot upload the encrypted backup.\n\n%s", error);
		return;
	}
	WCHAR format[2048];
	LoadStringResource(IDS_CLOUD_UPLOAD_OK,
		L"Encrypted backup uploaded:\n%s\n\nSize: %llu bytes.",
		format, ARRAYSIZE(format));
	WCHAR message[4096];
	if (swprintf_s(message, format, result.remoteName.c_str(),
		static_cast<unsigned long long>(result.encryptedSize)) < 0) {
		lstrcpynW(message, result.remoteName.c_str(), ARRAYSIZE(message));
	}
	SetStatus(hwnd, message);
}

struct CloudHistoryParams {
	const ConfigCloudSettings *settings = nullptr;
	const ConfigCloudCredentials *credentials = nullptr;
};

struct CloudHistoryState {
	CloudHistoryParams params;
	std::vector<ConfigCloudRemoteEntry> entries;
};

CloudHistoryState *GetHistoryState(HWND hwnd) noexcept {
	return reinterpret_cast<CloudHistoryState *>(GetWindowLongPtrW(hwnd, DWLP_USER));
}

std::wstring FormatCloudTime(const std::wstring &modified) noexcept {
	SYSTEMTIME utc {};
	SYSTEMTIME local {};
	if (!WinHttpTimeToSystemTime(modified.c_str(), &utc)) {
		return L"-";
	}
	if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
		local = utc;
	}
	wchar_t buffer[64];
	swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u",
		local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond);
	return buffer;
}

void FormatCloudSize(uint64_t size, LPWSTR buffer, int capacity) noexcept {
	if (!StrFormatByteSizeW(static_cast<LONGLONG>(size), buffer, capacity)) {
		swprintf_s(buffer, capacity, L"%llu B", static_cast<unsigned long long>(size));
	}
}

void SetHistoryStatus(HWND hwnd, const std::wstring &text) noexcept {
	SetDlgItemTextW(hwnd, IDC_CLOUD_HISTORY_STATUS, text.c_str());
}

void UpdateHistoryButtons(HWND hwnd) noexcept {
	CloudHistoryState *state = GetHistoryState(hwnd);
	const int selected = static_cast<int>(SendDlgItemMessageW(hwnd, IDC_CLOUD_HISTORY_LIST,
		LB_GETCURSEL, 0, 0));
	EnableWindow(GetDlgItem(hwnd, IDC_CLOUD_HISTORY_RESTORE),
		state != nullptr && selected >= 0 && static_cast<size_t>(selected) < state->entries.size());
}

void FillCloudHistory(HWND hwnd) noexcept {
	CloudHistoryState *state = GetHistoryState(hwnd);
	if (state == nullptr || state->params.settings == nullptr || state->params.credentials == nullptr) {
		return;
	}
	SendDlgItemMessageW(hwnd, IDC_CLOUD_HISTORY_LIST, LB_RESETCONTENT, 0, 0);
	state->entries.clear();
	std::wstring error;
	if (!ConfigCloud_ListRemote(*state->params.settings, *state->params.credentials,
		state->entries, &error)) {
		WCHAR format[2048];
		WCHAR message[4096];
		LoadStringResource(IDS_CLOUD_HISTORY_LOAD_FAIL,
			L"Cannot read the cloud backup list.\n\n%s", format, ARRAYSIZE(format));
		if (swprintf_s(message, format, error.empty() ? L"Unknown error." : error.c_str()) < 0) {
			lstrcpynW(message, error.c_str(), ARRAYSIZE(message));
		}
		SendDlgItemMessageW(hwnd, IDC_CLOUD_HISTORY_LIST, LB_ADDSTRING, 0,
			reinterpret_cast<LPARAM>(message));
		SetHistoryStatus(hwnd, L"");
		UpdateHistoryButtons(hwnd);
		return;
	}

	WCHAR itemFormat[512];
	WCHAR autoText[64];
	WCHAR manualText[64];
	LoadStringResource(IDS_CLOUD_HISTORY_ITEM_FMT, L"%s  |  %s  |  %s  |  %s",
		itemFormat, ARRAYSIZE(itemFormat));
	LoadStringResource(IDS_CLOUD_HISTORY_AUTO, L"automatic", autoText, ARRAYSIZE(autoText));
	LoadStringResource(IDS_CLOUD_HISTORY_MANUAL, L"manual", manualText, ARRAYSIZE(manualText));

	for (const ConfigCloudRemoteEntry &entry : state->entries) {
		const std::wstring time = FormatCloudTime(entry.modified);
		WCHAR size[64];
		FormatCloudSize(entry.size, size, ARRAYSIZE(size));
		const bool automatic = _wcsnicmp(entry.name.c_str(), L"Notepad4-Cloud-Auto-",
			wcslen(L"Notepad4-Cloud-Auto-")) == 0;
		WCHAR line[2048];
		if (swprintf_s(line, itemFormat, time.c_str(), size,
			automatic ? autoText : manualText, entry.name.c_str()) >= 0) {
			SendDlgItemMessageW(hwnd, IDC_CLOUD_HISTORY_LIST, LB_ADDSTRING, 0,
				reinterpret_cast<LPARAM>(line));
		}
	}
	if (state->entries.empty()) {
		WCHAR empty[256];
		LoadStringResource(IDS_CLOUD_HISTORY_EMPTY, L"(No encrypted cloud backups found.)",
			empty, ARRAYSIZE(empty));
		SendDlgItemMessageW(hwnd, IDC_CLOUD_HISTORY_LIST, LB_ADDSTRING, 0,
			reinterpret_cast<LPARAM>(empty));
		SetHistoryStatus(hwnd, L"");
	} else {
		SendDlgItemMessageW(hwnd, IDC_CLOUD_HISTORY_LIST, LB_SETCURSEL, 0, 0);
		WCHAR ready[512];
		LoadStringResource(IDS_CLOUD_HISTORY_READY,
			L"Select a backup and press Restore selected.", ready, ARRAYSIZE(ready));
		SetHistoryStatus(hwnd, ready);
	}
	UpdateHistoryButtons(hwnd);
}

void RestoreCloudSelection(HWND hwnd) noexcept {
	CloudHistoryState *state = GetHistoryState(hwnd);
	const int selected = static_cast<int>(SendDlgItemMessageW(hwnd, IDC_CLOUD_HISTORY_LIST,
		LB_GETCURSEL, 0, 0));
	if (state == nullptr || state->params.settings == nullptr || state->params.credentials == nullptr
		|| selected < 0 || static_cast<size_t>(selected) >= state->entries.size()) {
		WCHAR message[512];
		LoadStringResource(IDS_CLOUD_HISTORY_SELECT, L"Select a cloud backup first.",
			message, ARRAYSIZE(message));
		SetHistoryStatus(hwnd, message);
		return;
	}
	const std::wstring remoteName = state->entries[static_cast<size_t>(selected)].name;
	std::wstring zipPath;
	std::wstring error;
	BeginWaitCursor();
	const bool downloaded = ConfigCloud_DownloadRemote(*state->params.settings,
		*state->params.credentials, remoteName.c_str(), zipPath, &error);
	EndWaitCursor();
	if (!downloaded) {
		WCHAR format[2048];
		WCHAR message[4096];
		LoadStringResource(IDS_CLOUD_HISTORY_RESTORE_FAIL,
			L"Cannot download or decrypt the selected backup.\n\n%s",
			format, ARRAYSIZE(format));
		if (swprintf_s(message, format, error.empty() ? L"Unknown error." : error.c_str()) < 0) {
			lstrcpynW(message, error.c_str(), ARRAYSIZE(message));
		}
		SetHistoryStatus(hwnd, message);
		return;
	}
	if (ConfigRestore_ShowDialogForFile(hwnd, zipPath.c_str())) {
		DeleteFileW(zipPath.c_str());
		EndDialog(hwnd, IDOK);
		return;
	}
	DeleteFileW(zipPath.c_str());
	WCHAR cancelled[512];
	LoadStringResource(IDS_CLOUD_RESTORE_CANCELLED,
		L"Restore cancelled.", cancelled, ARRAYSIZE(cancelled));
	SetHistoryStatus(hwnd, cancelled);
}

INT_PTR CALLBACK CloudHistoryDlgProc(HWND hwnd, UINT umsg, WPARAM wParam, LPARAM lParam) noexcept {
	CloudHistoryState *state = GetHistoryState(hwnd);
	switch (umsg) {
	case WM_INITDIALOG: {
		state = new (std::nothrow) CloudHistoryState();
		if (state == nullptr) {
			return FALSE;
		}
		if (lParam != 0) {
			state->params = *reinterpret_cast<const CloudHistoryParams *>(lParam);
		}
		SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
		CenterDlgInParent(hwnd);
		FillCloudHistory(hwnd);
		SetFocus(GetDlgItem(hwnd, IDC_CLOUD_HISTORY_LIST));
		return TRUE;
	}

	case WM_COMMAND:
		if (state == nullptr) {
			break;
		}
		switch (LOWORD(wParam)) {
		case IDC_CLOUD_HISTORY_LIST:
			if (HIWORD(wParam) == LBN_SELCHANGE) {
				UpdateHistoryButtons(hwnd);
				return TRUE;
			}
			if (HIWORD(wParam) == LBN_DBLCLK) {
				RestoreCloudSelection(hwnd);
				return TRUE;
			}
			break;
		case IDC_CLOUD_HISTORY_RESTORE:
			RestoreCloudSelection(hwnd);
			return TRUE;
		case IDC_CLOUD_HISTORY_REFRESH:
			FillCloudHistory(hwnd);
			return TRUE;
		case IDCANCEL:
			EndDialog(hwnd, IDCANCEL);
			return TRUE;
		default:
			break;
		}
		break;

	case WM_CLOSE:
		EndDialog(hwnd, IDCANCEL);
		return TRUE;

	case WM_DESTROY:
		delete state;
		SetWindowLongPtrW(hwnd, DWLP_USER, 0);
		break;

	default:
		break;
	}
	return FALSE;
}

bool ShowCloudHistory(HWND hwnd, const ConfigCloudSettings &settings,
	const ConfigCloudCredentials &credentials) noexcept {
	CloudHistoryParams params;
	params.settings = &settings;
	params.credentials = &credentials;
	const INT_PTR result = ThemedDialogBoxParam(g_hInstance,
		MAKEINTRESOURCE(IDD_CLOUD_HISTORY), hwnd, CloudHistoryDlgProc,
		reinterpret_cast<LPARAM>(&params));
	return result == IDOK;
}

void ChooseCloudBackup(HWND hwnd) noexcept {
	ConfigCloudSettings settings;
	ConfigCloudCredentials credentials;
	ReadFields(hwnd, settings, credentials);
	if (settings.url.empty()) {
		SetStatusResource(hwnd, IDS_CLOUD_URL_REQUIRED, L"A WebDAV URL is required.");
		return;
	}
	if (credentials.passphrase.empty()) {
		WCHAR message[512];
		LoadStringResource(IDS_CLOUD_NEED_PASSPHRASE,
			L"Enter an encryption passphrase.", message, ARRAYSIZE(message));
		SetStatus(hwnd, message);
		return;
	}
	std::wstring error;
	if (!SaveSettings(settings, credentials, false, &error)) {
		SetStatusFormat(hwnd, IDS_CLOUD_CREDENTIALS_FAIL,
			L"Cannot save the cloud credentials.\n\n%s", error);
		return;
	}
	if (ShowCloudHistory(hwnd, settings, credentials)) {
		EndDialog(hwnd, IDOK);
		return;
	}
	WCHAR cancelled[512];
	LoadStringResource(IDS_CLOUD_RESTORE_CANCELLED,
		L"Restore cancelled.", cancelled, ARRAYSIZE(cancelled));
	SetStatus(hwnd, cancelled);
}

void SaveSettingsFromDialog(HWND hwnd) noexcept {
	ConfigCloudSettings settings;
	ConfigCloudCredentials credentials;
	ReadFields(hwnd, settings, credentials);
	std::wstring error;
	if (!SaveSettings(settings, credentials, true, &error)) {
		SetStatusFormat(hwnd, IDS_CLOUD_CREDENTIALS_FAIL,
			L"Cannot save the cloud credentials.\n\n%s", error);
		return;
	}
	WCHAR message[512];
	LoadStringResource(IDS_CLOUD_SETTINGS_SAVED,
		L"Cloud settings saved.", message, ARRAYSIZE(message));
	SetStatus(hwnd, message);
}

INT_PTR CALLBACK CloudDlgProc(HWND hwnd, UINT umsg, WPARAM wParam, LPARAM lParam) noexcept {
	UNREFERENCED_PARAMETER(lParam);
	switch (umsg) {
	case WM_INITDIALOG:
		InitializeDialog(hwnd);
		return TRUE;

	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDC_CLOUD_TEST:
			TestConnection(hwnd);
			return TRUE;
		case IDC_CLOUD_UPLOAD:
			UploadBackup(hwnd);
			return TRUE;
		case IDC_CLOUD_RESTORE:
			ChooseCloudBackup(hwnd);
			return TRUE;
		case IDC_CLOUD_SAVE_SETTINGS:
			SaveSettingsFromDialog(hwnd);
			return TRUE;
		case IDCANCEL:
			EndDialog(hwnd, IDCANCEL);
			return TRUE;
		default:
			break;
		}
		break;

	case WM_CLOSE:
		EndDialog(hwnd, IDCANCEL);
		return TRUE;

	default:
		break;
	}
	return FALSE;
}

} // namespace

void ConfigCloud_ShowDialog(HWND hwndOwner) noexcept {
	ThemedDialogBoxParam(g_hInstance, MAKEINTRESOURCE(IDD_CLOUD_BACKUP),
		hwndOwner, CloudDlgProc, 0);
}
