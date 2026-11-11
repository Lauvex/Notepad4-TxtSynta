// This file is part of the Notepad4 TXT Syntax customization.
#include "EditLexer.h"
#include "EditStyleX.h"

static const char * const Keywords_TxtSyntax[] = {
	nullptr
};

// Each style remains editable in Notepad4's scheme customization dialog.
static const EDITSTYLE Styles_TxtSyntax[] = {
	EDITSTYLE_DEFAULT,
	{ SCE_TXT_HEADING, NP2StyleX_Header1, L"bold; fore:#569CD6" },
	{ SCE_TXT_COMMENT, NP2StyleX_Comment, L"fore:#6A9955" },
	{ SCE_TXT_LINK, NP2StyleX_Link, L"fore:#4EC9B0" },
	{ SCE_TXT_VARIABLE, NP2StyleX_Variable, L"fore:#9CDCFE" },
	{ SCE_TXT_NUMBER, NP2StyleX_Number, L"fore:#B5CEA8" },
	{ SCE_TXT_BRACKET, NP2StyleX_Operator, L"fore:#DCDCAA" },
	{ SCE_TXT_STRING, NP2StyleX_String, L"fore:#CE9178" },
	{ SCE_TXT_SEPARATOR, NP2StyleX_Regex, L"fore:#C586C0" },
	{ SCE_TXT_SYMBOL, NP2StyleX_Symbol, L"fore:#858585" },
};

EDITLEXER lexTxtSyntax = {
	SCLEX_TXTSYNTAX, NP2LEX_TXTSYNTAX,
	LexerAttr_NoBlockComment | LexerAttr_PlainTextFile,
	TAB_WIDTH_4, INDENT_WIDTH_4,
	0, 0,
	'\0', 0, 0,
	0,
	0, 0,
	0, 0,
	KeywordAttr_NoLexer,
	SCE_TXT_COMMENT,
	SCE_TXT_STRING, SCE_TXT_STRING,
	EDITLEXER_TEXT(L"TXT Syntax", Keywords_TxtSyntax, Styles_TxtSyntax),
	L"txt",
	Keywords_TxtSyntax,
	Styles_TxtSyntax
};
