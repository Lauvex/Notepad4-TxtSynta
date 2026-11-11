// This file is part of Notepad4.
// See License.txt for details about distribution and modification.

#include <windows.h>
#include <windowsx.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <vsstyle.h>
#include <vssym32.h>
#include "config.h"
#include "Helpers.h"
#include "DarkMode.h"
#include "SciCall.h"
#include "Notepad4.h"
#include "Styles.h"
#include <memory>
#include <string>
#include <vector>
#include <utility>
#include <cwctype>
#include "Dialogs.h"

namespace { // DialogHook

struct DialogHook {
	HHOOK hook;
	DWORD_PTR dwRefData;

	void Start() noexcept {
		hook = SetWindowsHookEx(WH_CBT, DialogHook::HookProc, nullptr, GetCurrentThreadId());
	}
	void Stop() noexcept {
		HHOOK current = hook;
		if (current) {
			hook = nullptr;
			UnhookWindowsHookEx(current);
		}
	}
	static LRESULT CALLBACK HookProc(int nCode, WPARAM wParam, LPARAM lParam) noexcept;
};

DialogHook dialogHook {nullptr, DialogRefData_CenterParent};
LRESULT CALLBACK InitDialogSubProc(HWND hwnd, UINT umsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) noexcept {
	if (umsg == WM_INITDIALOG || umsg == WM_DESTROY) {
		RemoveWindowSubclass(hwnd, InitDialogSubProc, uIdSubclass);
	}
	const LRESULT result = DefSubclassProc(hwnd, umsg, wParam, lParam);
	if (umsg == WM_INITDIALOG) {
		if (dwRefData > DialogRefData_MaxValue) {
			SetWindowSubclass(hwnd, FileDialog::SubProc, 0, dwRefData);
		} else {
			DarkMode_InitDialog(hwnd, dwRefData);
		}
	}
	return result;
}

LRESULT CALLBACK DialogHook::HookProc(int nCode, WPARAM wParam, LPARAM lParam) noexcept {
	if (nCode == HCBT_CREATEWND) {
		const auto *lpcs = AsPointer<const CBT_CREATEWND *>(lParam)->lpcs;
		if (lpcs->lpszClass == WC_DIALOG) {
			const auto current = dialogHook;
			memset(&dialogHook, 0, sizeof(dialogHook));
			UnhookWindowsHookEx(current.hook);
			HWND hwnd = AsPointer<HWND>(wParam);
			SetWindowSubclass(hwnd, InitDialogSubProc, 0, current.dwRefData);
		}
	}
	return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

}

void DialogHook_Start(DWORD_PTR dwRefData) noexcept {
	dialogHook.dwRefData = dwRefData;
	dialogHook.Start();
}

void DialogHook_Stop() noexcept {
	dialogHook.Stop();
}

// ==================== Lauvex: 六主题 Win32 外观 ====================
namespace {

struct ChromeColors {
    bool enabled;
    COLORREF background;
    COLORREF foreground;
    COLORREF selection;
    COLORREF muted;
    COLORREF border;
    COLORREF menuBackground;
    COLORREF toolbarBackground;
    COLORREF statusBackground;
};

struct ChromeMenuItem {
    HMENU menu;
    UINT position;
    UINT type;
    ULONG_PTR originalData;
    std::wstring text;
};

ChromeColors chromeColors{};
HBRUSH chromeBrush = nullptr;
HBRUSH chromeMenuBrush = nullptr;
HBRUSH chromeToolbarBrush = nullptr;
HBRUSH chromeStatusBrush = nullptr;
std::vector<std::unique_ptr<ChromeMenuItem>> chromeMenuItems;
std::wstring chromeStatusParts[32];
std::wstring chromeStatusSimpleText;
HFONT chromeMenuFont = nullptr;
HMENU chromeMainMenu = nullptr;

// 以编辑区主题编号为唯一真值，系统高对比度时交给 Windows 绘制。
ChromeColors Chrome_GetColors() noexcept {
    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof(hc);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0)
        && (hc.dwFlags & HCF_HIGHCONTRASTON)) {
        return {};
    }
    switch (np2StyleTheme) {
    case StyleTheme_Dark:           return {true, RGB(30, 30, 30), RGB(212, 212, 212), RGB(58, 58, 58), RGB(139, 139, 139), RGB(72, 72, 72), RGB(38, 38, 38), RGB(45, 45, 45), RGB(36, 36, 36)};
    case StyleTheme_SolarizedLight: return {true, RGB(253, 246, 227), RGB(64, 84, 92), RGB(238, 232, 213), RGB(147, 161, 161), RGB(203, 197, 175), RGB(238, 232, 213), RGB(246, 240, 221), RGB(238, 232, 213)};
    case StyleTheme_TangoWarmDark:  return {true, RGB(36, 33, 30), RGB(226, 216, 195), RGB(62, 54, 47), RGB(148, 138, 127), RGB(78, 70, 62), RGB(45, 41, 37), RGB(53, 48, 43), RGB(45, 41, 37)};
    case StyleTheme_GruvboxDark:    return {true, RGB(40, 40, 40), RGB(235, 219, 178), RGB(80, 73, 69), RGB(146, 131, 116), RGB(102, 92, 84), RGB(50, 48, 47), RGB(60, 56, 54), RGB(50, 48, 47)};
    case StyleTheme_GruvboxLight:   return {true, RGB(251, 241, 199), RGB(60, 56, 54), RGB(235, 219, 178), RGB(124, 111, 100), RGB(199, 184, 156), RGB(235, 219, 178), RGB(246, 233, 194), RGB(235, 219, 178)};
    default:                        return {};
    }
}

// 记录已自绘项目的原始文字与数据，避免更改命令 ID 和子菜单结构。
ChromeMenuItem *Chrome_FindMenuData(ULONG_PTR data) noexcept {
    for (const auto &item : chromeMenuItems) {
        if (reinterpret_cast<ULONG_PTR>(item.get()) == data) {
            return item.get();
        }
    }
    return nullptr;
}

// 递归设置菜单外观；Default 恢复原生菜单类型和文字。
void Chrome_ApplyMenu(HMENU menu, bool enabled) noexcept {
    if (!menu) { return; }
    MENUINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = MIM_BACKGROUND;
    info.hbrBack = enabled ? chromeMenuBrush : nullptr;
    SetMenuInfo(menu, &info);

    const int count = GetMenuItemCount(menu);
    for (int index = 0; index < count; ++index) {
        MENUITEMINFOW mii{};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_FTYPE | MIIM_DATA | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, index, TRUE, &mii)) { continue; }
        ChromeMenuItem *data = Chrome_FindMenuData(mii.dwItemData);
        if (enabled && !(mii.fType & (MFT_BITMAP | MFT_SEPARATOR))) {
            if (!data && !(mii.fType & MFT_OWNERDRAW)) {
                WCHAR label[1024]{};
                const int length = GetMenuStringW(menu, index, label, COUNTOF(label), MF_BYPOSITION);
                std::unique_ptr<ChromeMenuItem> newData(new ChromeMenuItem{});
                newData->menu = menu;
                newData->position = static_cast<UINT>(index);
                newData->type = mii.fType;
                newData->originalData = mii.dwItemData;
                newData->text.assign(label, static_cast<size_t>(length));
                data = newData.get();
                chromeMenuItems.push_back(std::move(newData));
            }
            if (data) {
                MENUITEMINFOW change{};
                change.cbSize = sizeof(change);
                change.fMask = MIIM_FTYPE | MIIM_DATA;
                change.fType = mii.fType | MFT_OWNERDRAW;
                change.dwItemData = reinterpret_cast<ULONG_PTR>(data);
                SetMenuItemInfoW(menu, index, TRUE, &change);
            }
        } else if (!enabled && data) {
            MENUITEMINFOW restore{};
            restore.cbSize = sizeof(restore);
            restore.fMask = MIIM_FTYPE | MIIM_DATA | MIIM_STRING;
            restore.fType = data->type;
            restore.dwItemData = data->originalData;
            restore.dwTypeData = const_cast<LPWSTR>(data->text.c_str());
            SetMenuItemInfoW(menu, index, TRUE, &restore);
        }
        if (mii.hSubMenu) {
            Chrome_ApplyMenu(mii.hSubMenu, enabled);
        }
    }
}

// 使用系统菜单字体，适配 DPI 和本地化字体。
HFONT Chrome_GetMenuFont() noexcept {
    if (!chromeMenuFont) {
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
            chromeMenuFont = CreateFontIndirectW(&metrics.lfMenuFont);
        }
    }
    return chromeMenuFont ? chromeMenuFont : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

// 以实际文字宽度计算菜单项，保留快捷键提示右列空间。
bool Chrome_MeasureMenu(LPARAM lParam) noexcept {
    auto *measure = reinterpret_cast<MEASUREITEMSTRUCT *>(lParam);
    if (measure->CtlType != ODT_MENU || !chromeColors.enabled) { return false; }
    const ChromeMenuItem *data = Chrome_FindMenuData(measure->itemData);
    if (!data) { return false; }
    const bool top = data->menu && data->menu == chromeMainMenu;
    HDC dc = GetDC(nullptr);
    if (!dc) { return false; }
    HGDIOBJ old = SelectObject(dc, Chrome_GetMenuFont());
    RECT rect{0, 0, 0, 0};
    DrawTextW(dc, data->text.c_str(), -1, &rect, DT_SINGLELINE | DT_CALCRECT);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    measure->itemWidth = static_cast<UINT>(rect.right + (top ? 4 : 60));
    const int height = rect.bottom + 8;
    const int minimum = GetSystemMetrics(SM_CYMENU);
    measure->itemHeight = static_cast<UINT>(height > minimum ? height : minimum);
    return true;
}

// 绘制菜单文字、选择态及勾选符号，不改变菜单命令逻辑。
bool Chrome_DrawMenu(LPARAM lParam) noexcept {
    auto *item = reinterpret_cast<DRAWITEMSTRUCT *>(lParam);
    if (item->CtlType != ODT_MENU || !chromeColors.enabled) { return false; }
    ChromeMenuItem *data = Chrome_FindMenuData(item->itemData);
    if (!data) { return false; }
    const bool selected = (item->itemState & (ODS_SELECTED | ODS_HOTLIGHT)) != 0;
    HBRUSH brush = CreateSolidBrush(selected ? chromeColors.selection : chromeColors.menuBackground);
    FillRect(item->hDC, &item->rcItem, brush);
    DeleteObject(brush);

    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, (item->itemState & (ODS_GRAYED | ODS_DISABLED))
        ? chromeColors.muted : chromeColors.foreground);
    HGDIOBJ old = SelectObject(item->hDC, Chrome_GetMenuFont());
    const bool top = data->menu && data->menu == chromeMainMenu;
    RECT textRect = item->rcItem;
    textRect.left += top ? 2 : 28;
    textRect.right -= top ? 2 : 16;
    const size_t tab = data->text.find(L'\t');
    if (tab != std::wstring::npos && !top) {
        const std::wstring label = data->text.substr(0, tab);
        const std::wstring shortcut = data->text.substr(tab + 1);
        DrawTextW(item->hDC, label.c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
        DrawTextW(item->hDC, shortcut.c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
    } else {
        DrawTextW(item->hDC, data->text.c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    }
    if (!top && (item->itemState & ODS_CHECKED)) {
        RECT mark = item->rcItem;
        mark.left += 6;
        mark.right = mark.left + 17;
        DrawTextW(item->hDC, L"\u2713", 1, &mark, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }
    SelectObject(item->hDC, old);
    return true;
}

// owner-draw 菜单补齐 Alt+助记键匹配。
LRESULT Chrome_MenuChar(WPARAM wParam, LPARAM lParam) noexcept {
    if (!chromeColors.enabled) { return MAKELRESULT(0, MNC_IGNORE); }
    const wchar_t key = static_cast<wchar_t>(towupper(LOWORD(wParam)));
    HMENU menu = reinterpret_cast<HMENU>(lParam);
    const int count = GetMenuItemCount(menu);
    for (int index = 0; index < count; ++index) {
        MENUITEMINFOW mii{};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_DATA | MIIM_SUBMENU | MIIM_STATE;
        if (!GetMenuItemInfoW(menu, index, TRUE, &mii) || (mii.fState & MFS_DISABLED)) { continue; }
        ChromeMenuItem *data = Chrome_FindMenuData(mii.dwItemData);
        if (!data) { continue; }
        for (size_t pos = 0; pos + 1 < data->text.size(); ++pos) {
            if (data->text[pos] == L'&' && data->text[pos + 1] != L'&'
                && towupper(data->text[pos + 1]) == key) {
                return MAKELRESULT(index, mii.hSubMenu ? MNC_SELECT : MNC_EXECUTE);
            }
        }
    }
    return MAKELRESULT(0, MNC_IGNORE);
}

// 转换状态栏的正常文字为 owner-draw，保留原有状态栏点击事件。
LRESULT CALLBACK Chrome_StatusProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR id, DWORD_PTR data) noexcept {
    UNREFERENCED_PARAMETER(id);
    UNREFERENCED_PARAMETER(data);
    if (message == SB_SETTEXTW) {
        const UINT part = LOWORD(wParam) & 0xff;
        const auto *str = reinterpret_cast<LPCWSTR>(lParam);
        if (part == SB_SIMPLEID) {
            chromeStatusSimpleText = str ? str : L"";
        } else if (part < COUNTOF(chromeStatusParts) && !(wParam & SBT_OWNERDRAW)) {
            chromeStatusParts[part] = str ? str : L"";
            if (chromeColors.enabled) {
                return DefSubclassProc(hwnd, message, (wParam & 0xff) | SBT_OWNERDRAW,
                    reinterpret_cast<LPARAM>(chromeStatusParts[part].c_str()));
            }
        }
    } else if (message == WM_PAINT && chromeColors.enabled
        && SendMessage(hwnd, SB_ISSIMPLE, 0, 0)) {
        // 简单模式不支持 SBT_OWNERDRAW，在默认绘制后覆盖文本。
        const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        HDC dc = GetDC(hwnd);
        if (dc) {
            RECT rect{};
            GetClientRect(hwnd, &rect);
            FillRect(dc, &rect, chromeStatusBrush);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, chromeColors.foreground);
            HGDIOBJ old = SelectObject(dc, GetWindowFont(hwnd));
            rect.left += 5;
            DrawTextW(dc, chromeStatusSimpleText.c_str(), -1, &rect,
                DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
            SelectObject(dc, old);
            ReleaseDC(hwnd, dc);
        }
        return result;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

} // namespace

// 菜单与状态栏的 owner-draw 消息交由主窗口统一路由。
bool ThemeChrome_IsEnabled() noexcept {
    return chromeColors.enabled;
}

LRESULT ThemeChrome_HandleMenuChar(WPARAM wParam, LPARAM lParam) noexcept {
    return Chrome_MenuChar(wParam, lParam);
}

bool ThemeChrome_HandleMeasure(LPARAM lParam) noexcept {
    return Chrome_MeasureMenu(lParam);
}

bool ThemeChrome_HandleDraw(WPARAM wParam, LPARAM lParam) noexcept {
    if (wParam == 0) {
        return Chrome_DrawMenu(lParam);
    }
    if (!chromeColors.enabled || wParam != IDC_STATUSBAR) { return false; }
    const auto *draw = reinterpret_cast<DRAWITEMSTRUCT *>(lParam);
    const auto *str = reinterpret_cast<LPCWSTR>(draw->itemData);
    if (!str) { return false; }
    FillRect(draw->hDC, &draw->rcItem, chromeStatusBrush);
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, chromeColors.foreground);
    HGDIOBJ old = SelectObject(draw->hDC, GetWindowFont(draw->hwndItem));
    RECT textRect = draw->rcItem;
    textRect.left += 4;
    DrawTextW(draw->hDC, str, -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    SelectObject(draw->hDC, old);
    return true;
}

// 工具栏使用系统支持的 NM_CUSTOMDRAW，不变更按钮 ID 和图标列表。
LRESULT ThemeChrome_HandleNotify(LPARAM lParam) noexcept {
    auto *hdr = reinterpret_cast<NMHDR *>(lParam);
    if (!chromeColors.enabled || hdr->code != NM_CUSTOMDRAW) { return CDRF_DODEFAULT; }
    if (hdr->idFrom == IDC_TOOLBAR) {
        auto *draw = reinterpret_cast<NMTBCUSTOMDRAW *>(lParam);
        if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) {
            RECT rect{};
            GetClientRect(hdr->hwndFrom, &rect);
            FillRect(draw->nmcd.hdc, &rect, chromeToolbarBrush);
            return CDRF_NOTIFYITEMDRAW;
        }
        if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
            draw->clrText = chromeColors.foreground;
            draw->clrTextHighlight = chromeColors.foreground;
            draw->clrBtnFace = chromeColors.toolbarBackground;
            draw->clrBtnHighlight = chromeColors.selection;
            draw->clrHighlightHotTrack = chromeColors.selection;
            draw->nStringBkMode = TRANSPARENT;
            draw->nHLStringBkMode = TRANSPARENT;
            return TBCDRF_USECDCOLORS | TBCDRF_HILITEHOTTRACK | TBCDRF_NOETCHEDEFFECT;
        }
    }
    return CDRF_DODEFAULT;
}

// 在系统菜单生成动态项后，补齐其主题绘制数据。
void ThemeChrome_ApplyMenu(HMENU menu) noexcept {
    Chrome_ApplyMenu(menu, chromeColors.enabled);
}

// 当前主题立即同步到菜单、工具栏、ReBar、状态栏。
void ThemeChrome_Apply(HWND hwnd, HWND toolbar, HWND rebar, HWND status) noexcept {
    const HBRUSH oldBrush = chromeBrush;
    const HBRUSH oldMenuBrush = chromeMenuBrush;
    const HBRUSH oldToolbarBrush = chromeToolbarBrush;
    const HBRUSH oldStatusBrush = chromeStatusBrush;
    chromeColors = Chrome_GetColors();
    chromeMainMenu = GetMenu(hwnd);
    if (chromeMenuFont) { DeleteObject(chromeMenuFont); chromeMenuFont = nullptr; }
    chromeBrush = chromeColors.enabled ? CreateSolidBrush(chromeColors.background) : nullptr;
    chromeMenuBrush = chromeColors.enabled ? CreateSolidBrush(chromeColors.menuBackground) : nullptr;
    chromeToolbarBrush = chromeColors.enabled ? CreateSolidBrush(chromeColors.toolbarBackground) : nullptr;
    chromeStatusBrush = chromeColors.enabled ? CreateSolidBrush(chromeColors.statusBackground) : nullptr;
    Chrome_ApplyMenu(GetMenu(hwnd), chromeColors.enabled);
    if (oldBrush) { DeleteObject(oldBrush); }
    if (oldMenuBrush) { DeleteObject(oldMenuBrush); }
    if (oldToolbarBrush) { DeleteObject(oldToolbarBrush); }
    if (oldStatusBrush) { DeleteObject(oldStatusBrush); }
    if (toolbar) {
        SetWindowTheme(toolbar, chromeColors.enabled ? L"" : nullptr, chromeColors.enabled ? L"" : nullptr);
        InvalidateRect(toolbar, nullptr, TRUE);
    }
    if (rebar) {
        // 取消深浅主题中的系统立体边框，用区域背景色区分层次。
        const LONG_PTR style = GetWindowLongPtr(rebar, GWL_STYLE);
        const LONG_PTR themedStyle = chromeColors.enabled
            ? (style & ~(WS_BORDER | RBS_BANDBORDERS))
            : (style | WS_BORDER | RBS_BANDBORDERS);
        if (style != themedStyle) {
            SetWindowLongPtr(rebar, GWL_STYLE, themedStyle);
            SetWindowPos(rebar, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        SetWindowTheme(rebar, chromeColors.enabled ? L"" : nullptr, chromeColors.enabled ? L"" : nullptr);
        SendMessage(rebar, RB_SETBKCOLOR, 0, chromeColors.enabled ? chromeColors.toolbarBackground : CLR_DEFAULT);
        SendMessage(rebar, RB_SETTEXTCOLOR, 0, chromeColors.enabled ? chromeColors.foreground : CLR_DEFAULT);
        InvalidateRect(rebar, nullptr, TRUE);
    }
    if (status) {
        SetWindowTheme(status, chromeColors.enabled ? L"" : nullptr, chromeColors.enabled ? L"" : nullptr);
        SendMessage(status, SB_SETBKCOLOR, 0, chromeColors.enabled ? chromeColors.statusBackground : CLR_DEFAULT);
        for (UINT part = 0; part < COUNTOF(chromeStatusParts); ++part) {
            if (!chromeStatusParts[part].empty()) {
                const WPARAM style = part | (chromeColors.enabled ? SBT_OWNERDRAW : 0);
                SendMessage(status, SB_SETTEXTW, style,
                    reinterpret_cast<LPARAM>(chromeStatusParts[part].c_str()));
            }
        }
        InvalidateRect(status, nullptr, TRUE);
    }
    if (hwnd && GetMenu(hwnd)) { DrawMenuBar(hwnd); }
}

// 每次创建/重建状态栏时安装通知拦截器。
void ThemeChrome_AttachStatus(HWND hwnd) noexcept {
    if (hwnd) {
        SetWindowSubclass(hwnd, Chrome_StatusProc, 0x4C415556, 0);
    }
}

// 回收 GDI 资源与菜单附加数据。
void ThemeChrome_Cleanup() noexcept {
    chromeMenuItems.clear();
    if (chromeBrush) { DeleteObject(chromeBrush); chromeBrush = nullptr; }
    if (chromeMenuBrush) { DeleteObject(chromeMenuBrush); chromeMenuBrush = nullptr; }
    if (chromeToolbarBrush) { DeleteObject(chromeToolbarBrush); chromeToolbarBrush = nullptr; }
    if (chromeStatusBrush) { DeleteObject(chromeStatusBrush); chromeStatusBrush = nullptr; }
    if (chromeMenuFont) { DeleteObject(chromeMenuFont); chromeMenuFont = nullptr; }
}

void DarkMode_Init() noexcept {
}

void DarkMode_Cleanup() noexcept {
    ThemeChrome_Cleanup();
}

NP2_noinline
void DarkMode_InitDialog(HWND hwnd, DWORD_PTR dwRefData) noexcept {
	if (dwRefData >= DialogRefData_DefaultPosition) {
		return;
	}
	if (dwRefData != DialogRefData_RightBottom) {
		CenterDlgInParent(hwnd);
		if (dwRefData == DialogRefData_MessageBox) {
			SnapToDefaultButton(hwnd);
		}
	} else {
		SetToRightBottom(hwnd);
	}
}

NP2_noinline
void DarkMode_InitTreeView(HWND hwndTV) noexcept {
	InitWindowCommon(hwndTV);
	TreeView_SetExtendedStyle(hwndTV, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
	SetWindowTheme(hwndTV, L"Explorer", nullptr);
}

NP2_noinline
void DarkMode_InitFileListView(HWND hwndLV) noexcept {
	InitWindowCommon(hwndLV);
	ListView_SetExtendedListViewStyle(hwndLV, /*LVS_EX_FULLROWSELECT|*/LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
	// SetWindowTheme(hwndLV, L"Explorer", nullptr);

	LVCOLUMN lvc{};
	lvc.mask = LVCF_FMT | LVCF_TEXT;
	lvc.fmt = LVCFMT_LEFT;
	ListView_InsertColumn(hwndLV, 0, &lvc);
}
