/******************************************************************************
*
* Notepad4-TxtSyntax
*
* ConfigRestoreUI.cpp
*   Native restore dialog and restart handoff.
*
******************************************************************************/

#include <windows.h>
#include <windowsx.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "SciCall.h"
#include "config.h"
#include "Helpers.h"
#include "Notepad4.h"
#include "ConfigArchive.h"
#include "ConfigRestore.h"
#include "resource.h"

namespace {

struct RestoreDialogState {
	std::unique_ptr<ConfigArchivePackage> package;
	std::wstring archivePath;
	bool hasMatepath = false;
};

void LoadStringResource(UINT id, LPCWSTR fallback, LPWSTR buffer, int capacity) noexcept {
	if (capacity <= 0) {
		return;
	}
	if (LoadStringW(g_hInstance, id, buffer, capacity) <= 0) {
		lstrcpynW(buffer, fallback, capacity);
	}
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

RestoreDialogState *GetState(HWND hwnd) noexcept {
	return reinterpret_cast<RestoreDialogState *>(GetWindowLongPtrW(hwnd, DWLP_USER));
}

bool HasMatepath(const ConfigArchivePackage &package) noexcept {
	for (const ConfigArchiveFile &file : package.files) {
		if (file.path == "config/matepath.ini") {
			return true;
		}
	}
	return false;
}

uint32_t GetCategories(HWND hwnd) noexcept {
	if (IsDlgButtonChecked(hwnd, IDC_RESTORE_FULL) == BST_CHECKED) {
		return ConfigRestoreCategory_All;
	}
	uint32_t categories = 0;
	if (IsDlgButtonChecked(hwnd, IDC_RESTORE_BASIC) == BST_CHECKED) {
		categories |= ConfigRestoreCategory_Basic;
	}
	if (IsDlgButtonChecked(hwnd, IDC_RESTORE_INTERFACE) == BST_CHECKED) {
		categories |= ConfigRestoreCategory_Interface;
	}
	if (IsDlgButtonChecked(hwnd, IDC_RESTORE_THEMES) == BST_CHECKED) {
		categories |= ConfigRestoreCategory_Themes;
	}
	if (IsDlgButtonChecked(hwnd, IDC_RESTORE_SYNTAX) == BST_CHECKED) {
		categories |= ConfigRestoreCategory_Syntax;
	}
	if (IsDlgButtonChecked(hwnd, IDC_RESTORE_HISTORY) == BST_CHECKED) {
		categories |= ConfigRestoreCategory_History;
	}
	if (IsDlgButtonChecked(hwnd, IDC_RESTORE_MATEPATH) == BST_CHECKED) {
		categories |= ConfigRestoreCategory_Matepath;
	}
	return categories;
}

void UpdateControlStates(HWND hwnd) noexcept {
	RestoreDialogState *state = GetState(hwnd);
	const bool loaded = state != nullptr && state->package != nullptr;
	const bool full = IsDlgButtonChecked(hwnd, IDC_RESTORE_FULL) == BST_CHECKED;
	EnableWindow(GetDlgItem(hwnd, IDC_RESTORE_PREVIEW_BTN), loaded);
	EnableWindow(GetDlgItem(hwnd, IDOK), loaded);
	EnableWindow(GetDlgItem(hwnd, IDC_RESTORE_BASIC), loaded && !full);
	EnableWindow(GetDlgItem(hwnd, IDC_RESTORE_INTERFACE), loaded && !full);
	EnableWindow(GetDlgItem(hwnd, IDC_RESTORE_THEMES), loaded && !full);
	EnableWindow(GetDlgItem(hwnd, IDC_RESTORE_SYNTAX), loaded && !full);
	EnableWindow(GetDlgItem(hwnd, IDC_RESTORE_HISTORY), loaded && !full);
	EnableWindow(GetDlgItem(hwnd, IDC_RESTORE_MATEPATH), loaded && !full && state->hasMatepath);
}

void SetDefaultCategories(HWND hwnd, const ConfigArchivePackage &package) noexcept {
	CheckDlgButton(hwnd, IDC_RESTORE_FULL, BST_UNCHECKED);
	CheckDlgButton(hwnd, IDC_RESTORE_BASIC, BST_CHECKED);
	CheckDlgButton(hwnd, IDC_RESTORE_INTERFACE, BST_CHECKED);
	CheckDlgButton(hwnd, IDC_RESTORE_THEMES, BST_CHECKED);
	CheckDlgButton(hwnd, IDC_RESTORE_SYNTAX, BST_CHECKED);
	CheckDlgButton(hwnd, IDC_RESTORE_HISTORY, package.historyIncluded ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(hwnd, IDC_RESTORE_MATEPATH, HasMatepath(package) ? BST_CHECKED : BST_UNCHECKED);
}

void SetInfoText(HWND hwnd, RestoreDialogState *state) noexcept {
	WCHAR fmt[1024];
	WCHAR currentFormat[256];
	WCHAR legacyFormat[256];
	WCHAR unknown[128];
	WCHAR yes[64];
	WCHAR no[64];
	WCHAR legacyNotice[1024];
	LoadStringResource(IDS_RESTORE_INFO_FMT,
		L"Format: %s   Application: %s\nCreated: %s   Files: %u\nHistory: %s   matepath: %s\n%s",
		fmt, ARRAYSIZE(fmt));
	LoadStringResource(IDS_RESTORE_FORMAT_CURRENT, L"Versioned backup", currentFormat, ARRAYSIZE(currentFormat));
	LoadStringResource(IDS_RESTORE_FORMAT_LEGACY, L"Legacy six-INI backup", legacyFormat, ARRAYSIZE(legacyFormat));
	LoadStringResource(IDS_RESTORE_UNKNOWN, L"Unknown", unknown, ARRAYSIZE(unknown));
	LoadStringResource(IDS_RESTORE_YES, L"Yes", yes, ARRAYSIZE(yes));
	LoadStringResource(IDS_RESTORE_NO, L"No", no, ARRAYSIZE(no));
	LoadStringResource(IDS_RESTORE_LEGACY_NOTICE,
		L"Legacy format: no manifest or per-file SHA-256; only ZIP CRC is available.",
		legacyNotice, ARRAYSIZE(legacyNotice));

	if (state == nullptr || state->package == nullptr) {
		WCHAR empty[512];
		LoadStringResource(IDS_RESTORE_SELECT_FILE, L"Select a settings backup ZIP file.",
			empty, ARRAYSIZE(empty));
		SetDlgItemTextW(hwnd, IDC_RESTORE_INFO, empty);
		return;
	}
	const ConfigArchivePackage &package = *state->package;
	const bool legacy = package.format != ConfigArchiveFormat::Current;
	const std::wstring app = package.appVersion.empty() ? unknown : Widen(package.appVersion);
	const std::wstring created = package.createdAtUtc.empty() ? unknown : Widen(package.createdAtUtc);
	const wchar_t *history = package.historyIncluded ? yes : no;
	const wchar_t *matepath = state->hasMatepath ? yes : no;
	const wchar_t *note = legacy ? legacyNotice : L"";
	WCHAR info[2048];
	if (swprintf_s(info, fmt, legacy ? legacyFormat : currentFormat, app.c_str(), created.c_str(),
		static_cast<unsigned>(package.files.size()), history, matepath, note) < 0) {
		const std::wstring limitation = Widen(package.limitation);
		lstrcpynW(info, limitation.c_str(), ARRAYSIZE(info));
	}
	SetDlgItemTextW(hwnd, IDC_RESTORE_INFO, info);
	SetDlgItemTextW(hwnd, IDC_RESTORE_PATH, state->archivePath.c_str());
}

void ShowError(HWND hwnd, const std::wstring &error) noexcept {
	WCHAR title[128];
	WCHAR format[512];
	WCHAR message[2048];
	LoadStringResource(IDS_RESTORE_TITLE, L"Restore Settings", title, ARRAYSIZE(title));
	LoadStringResource(IDS_RESTORE_FAIL, L"Settings restore failed.\n\n%s", format, ARRAYSIZE(format));
	if (swprintf_s(message, format, error.empty() ? L"Unknown error." : error.c_str()) < 0) {
		lstrcpynW(message, error.c_str(), ARRAYSIZE(message));
	}
	MessageBoxW(hwnd, message, title, MB_OK | MB_ICONERROR);
}

bool SelectBackupFile(HWND hwnd, std::wstring &path) noexcept {
	WCHAR fileName[MAX_PATH] = L"";
	WCHAR title[128];
	WCHAR filter[256];
	LoadStringResource(IDS_RESTORE_TITLE, L"Restore Settings", title, ARRAYSIZE(title));
	LoadStringResource(IDS_RESTORE_FILTER, L"Settings Backup (*.zip)|*.zip|All Files (*.*)|*.*|",
		filter, ARRAYSIZE(filter));
	OPENFILENAMEW dialog {};
	dialog.lStructSize = sizeof(dialog);
	dialog.hwndOwner = hwnd;
	dialog.lpstrFilter = filter;
	dialog.lpstrFile = fileName;
	dialog.nMaxFile = ARRAYSIZE(fileName);
	dialog.lpstrTitle = title;
	dialog.lpstrDefExt = L"zip";
	dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
	PrepareFilterStr(filter);
	if (!GetOpenFileNameW(&dialog)) {
		return false;
	}
	path = fileName;
	return true;
}

bool LoadBackupPath(HWND hwnd, RestoreDialogState *state, const std::wstring &path) noexcept {
	BeginWaitCursor();
	ConfigArchivePackage package;
	std::wstring error;
	const bool success = ConfigRestore_Load(path.c_str(), &package, &error);
	EndWaitCursor();
	if (!success) {
		state->package.reset();
		state->archivePath.clear();
		state->hasMatepath = false;
		SetDlgItemTextW(hwnd, IDC_RESTORE_PATH, L"");
		SetDlgItemTextW(hwnd, IDC_RESTORE_PREVIEW, L"");
		SetInfoText(hwnd, state);
		UpdateControlStates(hwnd);
		ShowError(hwnd, error);
		return false;
	}
	state->package = std::make_unique<ConfigArchivePackage>(std::move(package));
	state->archivePath = path;
	state->hasMatepath = HasMatepath(*state->package);
	SetDefaultCategories(hwnd, *state->package);
	SetInfoText(hwnd, state);
	SetDlgItemTextW(hwnd, IDC_RESTORE_PREVIEW, L"");
	UpdateControlStates(hwnd);
	return true;
}

bool LoadBackup(HWND hwnd, RestoreDialogState *state) noexcept {
	std::wstring path;
	if (!SelectBackupFile(hwnd, path)) {
		return false;
	}
	return LoadBackupPath(hwnd, state, path);
}

bool BuildPreview(HWND hwnd, RestoreDialogState *state) noexcept {
	if (state == nullptr || state->package == nullptr) {
		return false;
	}
	const uint32_t categories = GetCategories(hwnd);
	WCHAR title[128];
	LoadStringResource(IDS_RESTORE_TITLE, L"Restore Settings", title, ARRAYSIZE(title));
	if (categories == 0) {
		WCHAR message[512];
		LoadStringResource(IDS_RESTORE_CATEGORY_EMPTY, L"Select at least one category to restore.",
			message, ARRAYSIZE(message));
		MessageBoxW(hwnd, message, title, MB_OK | MB_ICONWARNING);
		return false;
	}

	BeginWaitCursor();
	ConfigRestorePreview preview;
	std::wstring error;
	const bool success = ConfigRestore_BuildPreview(*state->package, categories, &preview, &error);
	EndWaitCursor();
	if (!success) {
		ShowError(hwnd, error);
		return false;
	}
	if (!preview.hasChanges) {
		WCHAR message[512];
		LoadStringResource(IDS_RESTORE_PREVIEW_EMPTY, L"No configuration changes would be made.",
			message, ARRAYSIZE(message));
		SetDlgItemTextW(hwnd, IDC_RESTORE_PREVIEW, message);
		return true;
	}

	WCHAR summaryFormat[512];
	WCHAR fileFormat[1024];
	LoadStringResource(IDS_RESTORE_PREVIEW_SUMMARY, L"Total: %d changed, %d added, %d removed.\r\n\r\n",
		summaryFormat, ARRAYSIZE(summaryFormat));
	LoadStringResource(IDS_RESTORE_PREVIEW_FILE_FMT,
		L"%s: %d changed, %d added, %d removed\r\n%s\r\n\r\n",
		fileFormat, ARRAYSIZE(fileFormat));
	WCHAR summary[512];
	swprintf_s(summary, summaryFormat, preview.changed, preview.added, preview.removed);
	std::wstring text = summary;
	for (const ConfigRestorePreviewFile &file : preview.files) {
		std::wstring details;
		for (size_t i = 0; i < file.details.size(); ++i) {
			if (i != 0) {
				details += L"\r\n";
			}
			details += file.details[i];
		}
		WCHAR block[4096];
		if (swprintf_s(block, fileFormat, file.fileName.c_str(), file.changed, file.added,
			file.removed, details.c_str()) >= 0) {
			text += block;
		}
		if (text.size() > 60000) {
			break;
		}
	}
	SetDlgItemTextW(hwnd, IDC_RESTORE_PREVIEW, text.c_str());
	return true;
}

bool ApplyRestore(HWND hwnd, RestoreDialogState *state) noexcept {
	if (state == nullptr || state->package == nullptr) {
		return false;
	}
	const uint32_t categories = GetCategories(hwnd);
	WCHAR title[128];
	WCHAR message[2048];
	WCHAR format[1024];
	LoadStringResource(IDS_RESTORE_TITLE, L"Restore Settings", title, ARRAYSIZE(title));
	if (categories == 0) {
		LoadStringResource(IDS_RESTORE_CATEGORY_EMPTY, L"Select at least one category to restore.",
			message, ARRAYSIZE(message));
		MessageBoxW(hwnd, message, title, MB_OK | MB_ICONWARNING);
		return false;
	}

	ConfigRestorePreview preview;
	std::wstring error;
	BeginWaitCursor();
	const bool previewed = ConfigRestore_BuildPreview(*state->package, categories, &preview, &error);
	EndWaitCursor();
	if (!previewed) {
		ShowError(hwnd, error);
		return false;
	}
	if (!preview.hasChanges) {
		LoadStringResource(IDS_RESTORE_NO_CHANGES, L"No settings need to be restored.",
			message, ARRAYSIZE(message));
		MessageBoxW(hwnd, message, title, MB_OK | MB_ICONINFORMATION);
		return false;
	}
	LoadStringResource(IDS_RESTORE_CONFIRM,
		L"Restore the selected settings?\n\nA rollback snapshot will be created automatically. The application will restart after a successful restore.",
		message, ARRAYSIZE(message));
	if (MessageBoxW(hwnd, message, title, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
		return false;
	}

	ConfigRestoreResult result;
	BeginWaitCursor();
	const bool applied = ConfigRestore_Apply(*state->package, categories, state->archivePath.c_str(), &result, &error);
	EndWaitCursor();
	if (!applied) {
		ShowError(hwnd, error);
		return false;
	}
	if (result.filesApplied == 0) {
		LoadStringResource(IDS_RESTORE_NO_CHANGES, L"No settings need to be restored.",
			message, ARRAYSIZE(message));
		MessageBoxW(hwnd, message, title, MB_OK | MB_ICONINFORMATION);
		return false;
	}
	LoadStringResource(IDS_RESTORE_SUCCESS,
		L"Settings restored: %d files.\n\nRollback snapshot:\n%s\n\nThe application will restart to load the restored settings.",
		format, ARRAYSIZE(format));
	if (swprintf_s(message, format, result.filesApplied, result.rollbackBackupPath.c_str()) < 0) {
		lstrcpynW(message, result.rollbackBackupPath.c_str(), ARRAYSIZE(message));
	}
	MessageBoxW(hwnd, message, title, MB_OK | MB_ICONINFORMATION);
	EndDialog(hwnd, IDOK);
	return true;
}

INT_PTR CALLBACK RestoreDlgProc(HWND hwnd, UINT umsg, WPARAM wParam, LPARAM lParam) noexcept {
	RestoreDialogState *state = GetState(hwnd);
	switch (umsg) {
	case WM_INITDIALOG: {
		state = new (std::nothrow) RestoreDialogState();
		if (state == nullptr) {
			return FALSE;
		}
		SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
		CenterDlgInParent(hwnd);
		SetDefaultCategories(hwnd, ConfigArchivePackage {});
		SetInfoText(hwnd, state);
		UpdateControlStates(hwnd);
		const wchar_t *initialPath = reinterpret_cast<const wchar_t *>(lParam);
		if (initialPath != nullptr && initialPath[0] != L'\0') {
			LoadBackupPath(hwnd, state, initialPath);
		}
		SetFocus(GetDlgItem(hwnd, state->package != nullptr ? IDC_RESTORE_PREVIEW_BTN : IDC_RESTORE_BROWSE));
		return TRUE;
	}

	case WM_COMMAND:
		if (state == nullptr) {
			break;
		}
		switch (LOWORD(wParam)) {
		case IDC_RESTORE_BROWSE:
			LoadBackup(hwnd, state);
			return TRUE;
		case IDC_RESTORE_FULL:
			if (HIWORD(wParam) == BN_CLICKED) {
				if (IsDlgButtonChecked(hwnd, IDC_RESTORE_FULL) == BST_CHECKED) {
					CheckDlgButton(hwnd, IDC_RESTORE_BASIC, BST_CHECKED);
					CheckDlgButton(hwnd, IDC_RESTORE_INTERFACE, BST_CHECKED);
					CheckDlgButton(hwnd, IDC_RESTORE_THEMES, BST_CHECKED);
					CheckDlgButton(hwnd, IDC_RESTORE_SYNTAX, BST_CHECKED);
					CheckDlgButton(hwnd, IDC_RESTORE_HISTORY, BST_CHECKED);
					CheckDlgButton(hwnd, IDC_RESTORE_MATEPATH, BST_CHECKED);
				}
				UpdateControlStates(hwnd);
			}
			return TRUE;
		case IDC_RESTORE_PREVIEW_BTN:
			BuildPreview(hwnd, state);
			return TRUE;
		case IDOK:
			ApplyRestore(hwnd, state);
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

bool ShowRestoreDialog(HWND hwndOwner, LPCWSTR initialPath) noexcept {
	const INT_PTR result = ThemedDialogBoxParam(g_hInstance, MAKEINTRESOURCE(IDD_CONFIG_RESTORE),
		hwndOwner, RestoreDlgProc, reinterpret_cast<LPARAM>(initialPath));
	if (result == IDOK) {
		PrepareSettingsRestartAfterRestore();
		PostWMCommand(hwndOwner, IDM_FILE_RESTART);
		return true;
	}
	return false;
}

} // namespace

void ConfigRestore_ShowDialog(HWND hwndOwner) noexcept {
	ShowRestoreDialog(hwndOwner, nullptr);
}

bool ConfigRestore_ShowDialogForFile(HWND hwndOwner, LPCWSTR archivePath) noexcept {
	if (archivePath == nullptr || archivePath[0] == L'\0') {
		return false;
	}
	return ShowRestoreDialog(hwndOwner, archivePath);
}
