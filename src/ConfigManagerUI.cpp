/******************************************************************************
*
* Notepad4-TxtSyntax
*
* ConfigManagerUI.cpp
*   Native configuration management dialog.
*
******************************************************************************/

#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <new>
#include <string>

#include "SciCall.h"
#include "config.h"
#include "Helpers.h"
#include "Notepad4.h"
#include "ConfigAutoBackup.h"
#include "ConfigBackup.h"
#include "ConfigCloud.h"
#include "ConfigCloudAuto.h"
#include "ConfigRestore.h"
#include "ConfigManager.h"
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

int ReadNumber(HWND hwnd, int id, int fallback, int minimum, int maximum) noexcept {
	BOOL translated = FALSE;
	UINT value = GetDlgItemInt(hwnd, id, &translated, FALSE);
	if (!translated) {
		return fallback;
	}
	return std::clamp(static_cast<int>(value), minimum, maximum);
}

void UpdateStatus(HWND hwnd) noexcept {
	const std::wstring directory = GetNotepad4DataDirectory();
	WCHAR format[1024];
	WCHAR data[2048];
	LoadStringResource(IDS_MGR_DATA_FMT, L"Application data: %s", format, ARRAYSIZE(format));
	if (swprintf_s(data, format, directory.c_str()) < 0) {
		lstrcpynW(data, directory.c_str(), ARRAYSIZE(data));
	}
	std::wstring status = data;

	if (ConfigCloudAuto_Settings().enabled) {
		const std::wstring error = ConfigCloudAuto_LastError();
		if (!error.empty()) {
			WCHAR errorFormat[1024];
			WCHAR errorText[2048];
			LoadStringResource(IDS_MGR_CLOUD_ERROR_FMT,
				L"Last automatic cloud error: %s", errorFormat, ARRAYSIZE(errorFormat));
			if (swprintf_s(errorText, errorFormat, error.c_str()) >= 0) {
				status += L"\r\n";
				status += errorText;
			}
		} else if (ConfigCloud_Settings().url.empty()) {
			WCHAR message[512];
			LoadStringResource(IDS_MGR_CLOUD_NOT_CONFIG,
				L"Automatic cloud backup is not configured (WebDAV URL or credentials missing).",
				message, ARRAYSIZE(message));
			status += L"\r\n";
			status += message;
		} else {
			WCHAR message[512];
			LoadStringResource(IDS_MGR_CLOUD_OK, L"Automatic cloud backup: ready.",
				message, ARRAYSIZE(message));
			status += L"\r\n";
			status += message;
		}
	}
	SetDlgItemTextW(hwnd, IDC_MGR_STATUS, status.c_str());
}

void InitializeDialog(HWND hwnd) noexcept {
	CenterDlgInParent(hwnd);
	const ConfigAutoBackupSettings &local = ConfigAutoBackup_Settings();
	CheckDlgButton(hwnd, IDC_MGR_LOCAL_ENABLED, local.enabled ? BST_CHECKED : BST_UNCHECKED);
	SetDlgItemInt(hwnd, IDC_MGR_LOCAL_INTERVAL, static_cast<UINT>(local.intervalHours), FALSE);
	SetDlgItemInt(hwnd, IDC_MGR_LOCAL_KEEP, static_cast<UINT>(local.keepCount), FALSE);
	CheckDlgButton(hwnd, IDC_MGR_LOCAL_HISTORY, local.includeHistory ? BST_CHECKED : BST_UNCHECKED);

	const ConfigCloudAutoSettings &cloud = ConfigCloudAuto_Settings();
	CheckDlgButton(hwnd, IDC_MGR_CLOUD_ENABLED, cloud.enabled ? BST_CHECKED : BST_UNCHECKED);
	SetDlgItemInt(hwnd, IDC_MGR_CLOUD_INTERVAL, static_cast<UINT>(cloud.intervalHours), FALSE);
	SetDlgItemInt(hwnd, IDC_MGR_CLOUD_KEEP, static_cast<UINT>(cloud.keepCount), FALSE);
	CheckDlgButton(hwnd, IDC_MGR_CLOUD_HISTORY, cloud.includeHistory ? BST_CHECKED : BST_UNCHECKED);
	UpdateStatus(hwnd);
	SetFocus(GetDlgItem(hwnd, IDC_MGR_LOCAL_ENABLED));
}

void SavePolicy(HWND hwnd) noexcept {
	ConfigAutoBackupSettings &local = ConfigAutoBackup_Settings();
	local.enabled = IsDlgButtonChecked(hwnd, IDC_MGR_LOCAL_ENABLED) == BST_CHECKED;
	local.intervalHours = ReadNumber(hwnd, IDC_MGR_LOCAL_INTERVAL, local.intervalHours, 1, 24 * 30);
	local.keepCount = ReadNumber(hwnd, IDC_MGR_LOCAL_KEEP, local.keepCount, 1, 50);
	local.includeHistory = IsDlgButtonChecked(hwnd, IDC_MGR_LOCAL_HISTORY) == BST_CHECKED;
	ConfigAutoBackup_SavePolicyToIni();

	ConfigCloudAutoSettings &cloud = ConfigCloudAuto_Settings();
	cloud.enabled = IsDlgButtonChecked(hwnd, IDC_MGR_CLOUD_ENABLED) == BST_CHECKED;
	cloud.intervalHours = ReadNumber(hwnd, IDC_MGR_CLOUD_INTERVAL, cloud.intervalHours, 1, 24 * 30);
	cloud.keepCount = ReadNumber(hwnd, IDC_MGR_CLOUD_KEEP, cloud.keepCount, 1, 50);
	cloud.includeHistory = IsDlgButtonChecked(hwnd, IDC_MGR_CLOUD_HISTORY) == BST_CHECKED;
	ConfigCloudAuto_SavePolicyToIni();

	WCHAR message[512];
	LoadStringResource(IDS_MGR_SAVED, L"Settings saved.", message, ARRAYSIZE(message));
	SetDlgItemTextW(hwnd, IDC_MGR_STATUS, message);
}

INT_PTR CALLBACK ManagerDlgProc(HWND hwnd, UINT umsg, WPARAM wParam, LPARAM lParam) noexcept {
	UNREFERENCED_PARAMETER(lParam);
	switch (umsg) {
	case WM_INITDIALOG:
		InitializeDialog(hwnd);
		return TRUE;

	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDC_MGR_SAVE:
			SavePolicy(hwnd);
			return TRUE;
		case IDC_MGR_LOCAL_HISTORY_BTN:
			ConfigAutoBackup_ShowHistory(hwnd);
			UpdateStatus(hwnd);
			return TRUE;
		case IDC_MGR_CLOUD_BTN:
			ConfigCloud_ShowDialog(hwnd);
			UpdateStatus(hwnd);
			return TRUE;
		case IDC_MGR_EXPORT:
			ConfigBackup_Export(hwnd);
			return TRUE;
		case IDC_MGR_RESTORE:
			ConfigRestore_ShowDialog(hwnd);
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

void ConfigManager_ShowDialog(HWND hwndOwner) noexcept {
	ThemedDialogBoxParam(g_hInstance, MAKEINTRESOURCE(IDD_CONFIG_MANAGER),
		hwndOwner, ManagerDlgProc, 0);
}
