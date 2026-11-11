// This file is part of Notepad4.
// See License.txt for details about distribution and modification.
#pragma once

enum DialogRefData {
	DialogRefData_CenterParent,
	DialogRefData_MessageBox,
	DialogRefData_RightBottom,
	DialogRefData_DefaultPosition,
	DialogRefData_CustomizeToolbar,
	DialogRefData_MaxValue,
};

void DialogHook_Start(DWORD_PTR dwRefData) noexcept;
void DialogHook_Stop() noexcept;
// 六主题菜单、工具栏、ReBar、状态栏绘制入口。
bool ThemeChrome_IsEnabled() noexcept;
void ThemeChrome_Apply(HWND hwnd, HWND toolbar, HWND rebar, HWND status) noexcept;
void ThemeChrome_AttachStatus(HWND hwnd) noexcept;
void ThemeChrome_ApplyMenu(HMENU menu) noexcept;
bool ThemeChrome_HandleMeasure(LPARAM lParam) noexcept;
bool ThemeChrome_HandleDraw(WPARAM wParam, LPARAM lParam) noexcept;
LRESULT ThemeChrome_HandleMenuChar(WPARAM wParam, LPARAM lParam) noexcept;
LRESULT ThemeChrome_HandleNotify(LPARAM lParam) noexcept;
void ThemeChrome_Cleanup() noexcept;

void DarkMode_Init() noexcept;
void DarkMode_Cleanup() noexcept;
void DarkMode_InitDialog(HWND hwnd, DWORD_PTR dwRefData = DialogRefData_CenterParent) noexcept;
void DarkMode_InitTreeView(HWND hwndTV) noexcept;
void DarkMode_InitFileListView(HWND hwndLV) noexcept;
