/******************************************************************************
*
* Notepad4-TxtSyntax
*
* ConfigAutoBackupUI.cpp
*   Native local backup history dialog.
*
******************************************************************************/

#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>

#include <algorithm>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "SciCall.h"
#include "config.h"
#include "Helpers.h"
#include "Notepad4.h"
#include "ConfigAutoBackup.h"
#include "ConfigRestore.h"
#include "resource.h"

namespace {

struct HistoryDialogState {
	std::vector<ConfigAutoBackupEntry> entries;
};

void LoadStringResource(UINT id, LPCWSTR fallback, LPWSTR buffer, int capacity) noexcept {
	if (capacity <= 0) {
		return;
	}
	if (LoadStringW(g_hInstance, id, buffer, capacity) <= 0) {
		lstrcpynW(buffer, fallback, capacity);
	}
}

HistoryDialogState *GetState(HWND hwnd) noexcept {
	return reinterpret_cast<HistoryDialogState *>(GetWindowLongPtrW(hwnd, DWLP_USER));
}

bool FormatLocalTime(const FILETIME &utc, LPWSTR buffer, int capacity) noexcept {
	FILETIME local {};
	if (!FileTimeToLocalFileTime(&utc, &local)) {
		return false;
	}
	SYSTEMTIME time {};
	if (!FileTimeToSystemTime(&local, &time)) {
		return false;
	}
	return swprintf_s(buffer, capacity, L"%04u-%02u-%02u %02u:%02u:%02u",
		time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond) >= 0;
}

void FormatSize(uint64_t size, LPWSTR buffer, int capacity) noexcept {
	if (!StrFormatByteSizeW(static_cast<LONGLONG>(size), buffer, capacity)) {
		swprintf_s(buffer, capacity, L"%llu B", static_cast<unsigned long long>(size));
	}
}

int SelectedIndex(HWND hwnd) noexcept {
	return static_cast<int>(SendDlgItemMessageW(hwnd, IDC_HISTORY_LIST, LB_GETCURSEL, 0, 0));
}

void UpdateButtons(HWND hwnd) noexcept {
	HistoryDialogState *state = GetState(hwnd);
	const int selected = SelectedIndex(hwnd);
	const bool hasSelection = state != nullptr && selected >= 0
		&& static_cast<size_t>(selected) < state->entries.size();
	EnableWindow(GetDlgItem(hwnd, IDC_HISTORY_RESTORE), hasSelection);
	EnableWindow(GetDlgItem(hwnd, IDC_HISTORY_DELETE), hasSelection);
}

void UpdateSummary(HWND hwnd) noexcept {
	const ConfigAutoBackupSettings &settings = ConfigAutoBackup_Settings();
	WCHAR yes[64];
	WCHAR no[64];
	WCHAR on[64];
	WCHAR off[64];
	WCHAR format[1024];
	WCHAR summary[2048];
	LoadStringResource(IDS_HISTORY_YES, L"included", yes, ARRAYSIZE(yes));
	LoadStringResource(IDS_HISTORY_NO, L"excluded", no, ARRAYSIZE(no));
	LoadStringResource(IDS_HISTORY_ON, L"on", on, ARRAYSIZE(on));
	LoadStringResource(IDS_HISTORY_OFF, L"off", off, ARRAYSIZE(off));
	LoadStringResource(IDS_HISTORY_SUMMARY_FMT,
		L"Automatic backups: %s; interval: %d hours; keep: %d; history: %s.",
		format, ARRAYSIZE(format));
	if (swprintf_s(summary, format, settings.enabled ? on : off,
		settings.intervalHours, settings.keepCount,
		settings.includeHistory ? yes : no) < 0) {
		lstrcpynW(summary, L"Automatic backup policy is unavailable.", ARRAYSIZE(summary));
	}
	SetDlgItemTextW(hwnd, IDC_HISTORY_SUMMARY, summary);
	const std::wstring directory = ConfigAutoBackup_GetDirectory();
	SetDlgItemTextW(hwnd, IDC_HISTORY_FOLDER, directory.c_str());
}

void FillList(HWND hwnd) noexcept {
	HistoryDialogState *state = GetState(hwnd);
	if (state == nullptr) {
		return;
	}

	SendDlgItemMessageW(hwnd, IDC_HISTORY_LIST, LB_RESETCONTENT, 0, 0);
	state->entries.clear();
	std::wstring error;
	if (!ConfigAutoBackup_List(state->entries, &error)) {
		WCHAR message[2048];
		LoadStringResource(IDS_HISTORY_READ_FAIL, L"Cannot read the local backup history.\n\n%s",
			message, ARRAYSIZE(message));
		WCHAR text[2048];
		if (swprintf_s(text, message, error.empty() ? L"Unknown error." : error.c_str()) < 0) {
			lstrcpynW(text, error.c_str(), ARRAYSIZE(text));
		}
		SendDlgItemMessageW(hwnd, IDC_HISTORY_LIST, LB_ADDSTRING, 0,
			reinterpret_cast<LPARAM>(text));
		UpdateSummary(hwnd);
		UpdateButtons(hwnd);
		return;
	}

	WCHAR itemFormat[512];
	WCHAR yes[64];
	WCHAR no[64];
	LoadStringResource(IDS_HISTORY_ITEM_FMT, L"%s  |  %s  |  %s  |  %s",
		itemFormat, ARRAYSIZE(itemFormat));
	LoadStringResource(IDS_HISTORY_YES, L"history", yes, ARRAYSIZE(yes));
	LoadStringResource(IDS_HISTORY_NO, L"no history", no, ARRAYSIZE(no));

	for (const ConfigAutoBackupEntry &entry : state->entries) {
		WCHAR timeText[64] = L"?";
		WCHAR sizeText[64] = L"?";
		FormatLocalTime(entry.modifiedUtc, timeText, ARRAYSIZE(timeText));
		FormatSize(entry.size, sizeText, ARRAYSIZE(sizeText));
		WCHAR line[2048];
		if (swprintf_s(line, itemFormat, timeText, sizeText,
			entry.historyIncluded ? yes : no, entry.fileName.c_str()) >= 0) {
			SendDlgItemMessageW(hwnd, IDC_HISTORY_LIST, LB_ADDSTRING, 0,
				reinterpret_cast<LPARAM>(line));
		}
	}

	if (state->entries.empty()) {
		WCHAR empty[256];
		LoadStringResource(IDS_HISTORY_EMPTY, L"(No local backups yet.)",
			empty, ARRAYSIZE(empty));
		SendDlgItemMessageW(hwnd, IDC_HISTORY_LIST, LB_ADDSTRING, 0,
			reinterpret_cast<LPARAM>(empty));
	} else {
		SendDlgItemMessageW(hwnd, IDC_HISTORY_LIST, LB_SETCURSEL, 0, 0);
	}
	UpdateSummary(hwnd);
	UpdateButtons(hwnd);
}

void ShowError(HWND hwnd, UINT formatId, LPCWSTR fallback, const std::wstring &error) noexcept {
	WCHAR title[128];
	WCHAR format[1024];
	WCHAR message[2048];
	LoadStringResource(IDS_HISTORY_TITLE, L"Local Settings Backups", title, ARRAYSIZE(title));
	LoadStringResource(formatId, fallback, format, ARRAYSIZE(format));
	if (swprintf_s(message, format, error.empty() ? L"Unknown error." : error.c_str()) < 0) {
		lstrcpynW(message, error.c_str(), ARRAYSIZE(message));
	}
	MessageBoxW(hwnd, message, title, MB_OK | MB_ICONERROR);
}

void BackupNow(HWND hwnd) noexcept {
	ConfigBackupResult result;
	std::wstring error;
	BeginWaitCursor();
	const bool created = ConfigAutoBackup_CreateNow(&result, &error);
	EndWaitCursor();
	if (!created) {
		ShowError(hwnd, IDS_HISTORY_BACKUP_FAIL,
			L"Cannot create the local backup.\n\n%s", error);
		return;
	}

	WCHAR title[128];
	WCHAR format[1024];
	WCHAR message[2048];
	LoadStringResource(IDS_HISTORY_TITLE, L"Local Settings Backups", title, ARRAYSIZE(title));
	LoadStringResource(IDS_HISTORY_BACKUP_SUCCESS,
		L"Local backup created:\n%s\n\nKeeping the newest %d snapshots.",
		format, ARRAYSIZE(format));
	if (swprintf_s(message, format, result.path.c_str(),
		ConfigAutoBackup_Settings().keepCount) < 0) {
		lstrcpynW(message, result.path.c_str(), ARRAYSIZE(message));
	}
	MessageBoxW(hwnd, message, title, MB_OK | MB_ICONINFORMATION);
	FillList(hwnd);
}

void RestoreSelected(HWND hwnd) noexcept {
	HistoryDialogState *state = GetState(hwnd);
	const int selected = SelectedIndex(hwnd);
	if (state == nullptr || selected < 0 || static_cast<size_t>(selected) >= state->entries.size()) {
		WCHAR title[128];
		WCHAR message[512];
		LoadStringResource(IDS_HISTORY_TITLE, L"Local Settings Backups", title, ARRAYSIZE(title));
		LoadStringResource(IDS_HISTORY_SELECT, L"Select a local backup first.",
			message, ARRAYSIZE(message));
		MessageBoxW(hwnd, message, title, MB_OK | MB_ICONWARNING);
		return;
	}
	const std::wstring path = state->entries[static_cast<size_t>(selected)].path;
	if (ConfigRestore_ShowDialogForFile(hwnd, path.c_str())) {
		EndDialog(hwnd, IDOK);
	}
}

void DeleteSelected(HWND hwnd) noexcept {
	HistoryDialogState *state = GetState(hwnd);
	const int selected = SelectedIndex(hwnd);
	if (state == nullptr || selected < 0 || static_cast<size_t>(selected) >= state->entries.size()) {
		return;
	}
	const ConfigAutoBackupEntry &entry = state->entries[static_cast<size_t>(selected)];
	WCHAR title[128];
	WCHAR format[1024];
	WCHAR message[2048];
	LoadStringResource(IDS_HISTORY_TITLE, L"Local Settings Backups", title, ARRAYSIZE(title));
	LoadStringResource(IDS_HISTORY_DELETE_CONFIRM,
		L"Delete the selected local backup?\n\n%s", format, ARRAYSIZE(format));
	if (swprintf_s(message, format, entry.fileName.c_str()) < 0) {
		lstrcpynW(message, entry.fileName.c_str(), ARRAYSIZE(message));
	}
	if (MessageBoxW(hwnd, message, title, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
		return;
	}
	std::wstring error;
	if (!ConfigAutoBackup_Delete(entry.path.c_str(), &error)) {
		ShowError(hwnd, IDS_HISTORY_DELETE_FAIL,
			L"Cannot delete the selected local backup.\n\n%s", error);
	}
	FillList(hwnd);
}

void OpenBackupFolder(HWND hwnd) noexcept {
	std::wstring error;
	if (!ConfigAutoBackup_EnsureDirectory(&error)) {
		ShowError(hwnd, IDS_HISTORY_OPEN_FAIL,
			L"Cannot open the local backup folder.\n\n%s", error);
		return;
	}
	const std::wstring directory = ConfigAutoBackup_GetDirectory();
	if (reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd, L"open", directory.c_str(), nullptr,
		nullptr, SW_SHOWNORMAL)) <= 32) {
		ShowError(hwnd, IDS_HISTORY_OPEN_FAIL,
			L"Cannot open the local backup folder.", L"");
	}
}

INT_PTR CALLBACK HistoryDlgProc(HWND hwnd, UINT umsg, WPARAM wParam, LPARAM lParam) noexcept {
	UNREFERENCED_PARAMETER(lParam);
	HistoryDialogState *state = GetState(hwnd);
	switch (umsg) {
	case WM_INITDIALOG: {
		state = new (std::nothrow) HistoryDialogState();
		if (state == nullptr) {
			return FALSE;
		}
		SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
		CenterDlgInParent(hwnd);
		const ConfigAutoBackupSettings &settings = ConfigAutoBackup_Settings();
		CheckDlgButton(hwnd, IDC_HISTORY_ENABLED, settings.enabled ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(hwnd, IDC_HISTORY_INCLUDE, settings.includeHistory ? BST_CHECKED : BST_UNCHECKED);
		FillList(hwnd);
		SetFocus(GetDlgItem(hwnd, IDC_HISTORY_LIST));
		return FALSE;
	}

	case WM_COMMAND:
		if (state == nullptr) {
			break;
		}
		switch (LOWORD(wParam)) {
		case IDC_HISTORY_ENABLED:
			if (HIWORD(wParam) == BN_CLICKED) {
				ConfigAutoBackup_Settings().enabled =
					IsDlgButtonChecked(hwnd, IDC_HISTORY_ENABLED) == BST_CHECKED;
				ConfigAutoBackup_SavePolicyToIni();
				UpdateSummary(hwnd);
			}
			return TRUE;
		case IDC_HISTORY_INCLUDE:
			if (HIWORD(wParam) == BN_CLICKED) {
				ConfigAutoBackup_Settings().includeHistory =
					IsDlgButtonChecked(hwnd, IDC_HISTORY_INCLUDE) == BST_CHECKED;
				ConfigAutoBackup_SavePolicyToIni();
				UpdateSummary(hwnd);
			}
			return TRUE;
		case IDC_HISTORY_LIST:
			if (HIWORD(wParam) == LBN_SELCHANGE) {
				UpdateButtons(hwnd);
				return TRUE;
			}
			if (HIWORD(wParam) == LBN_DBLCLK) {
				RestoreSelected(hwnd);
				return TRUE;
			}
			break;
		case IDC_HISTORY_BACKUP_NOW:
			BackupNow(hwnd);
			return TRUE;
		case IDC_HISTORY_RESTORE:
			RestoreSelected(hwnd);
			return TRUE;
		case IDC_HISTORY_DELETE:
			DeleteSelected(hwnd);
			return TRUE;
		case IDC_HISTORY_OPEN_FOLDER:
			OpenBackupFolder(hwnd);
			return TRUE;
		case IDC_HISTORY_REFRESH:
			FillList(hwnd);
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

} // namespace

void ConfigAutoBackup_ShowHistory(HWND hwndOwner) noexcept {
	ThemedDialogBoxParam(g_hInstance, MAKEINTRESOURCE(IDD_CONFIG_HISTORY),
		hwndOwner, HistoryDlgProc, 0);
}
