// This file is part of the Notepad4 TXT Syntax customization.
// Lightweight, line-oriented lexer inspired by xshrim/vscode-txt-syntax.
// It styles document bytes only and never modifies file contents.

#include <cstdint>
#include <algorithm>
#include <string_view>
#include <initializer_list>

#include "ILexer.h"
#include "Scintilla.h"
#include "SciLexer.h"
#include "WordList.h"
#include <string>
#include "LexAccessor.h"
#include "Accessor.h"
#include "LexerModule.h"

using namespace Scintilla;
using namespace Lexilla;

namespace {

using Pos = Sci_PositionU;

// Return a decoded code point while preserving the byte width for styling.
struct Utf8Char {
	uint32_t value;
	unsigned width;
};

Utf8Char ReadChar(const Accessor &a, Pos p, Pos end) noexcept {
	if (p >= end) return {0, 0};
	const uint8_t b0 = static_cast<uint8_t>(a[p]);
	if (b0 < 0x80) return {b0, 1};
	unsigned width = (b0 >= 0xC2 && b0 <= 0xDF) ? 2 :
		(b0 >= 0xE0 && b0 <= 0xEF) ? 3 :
		(b0 >= 0xF0 && b0 <= 0xF4) ? 4 : 1;
	if (p + width > end) return {b0, 1};
	uint32_t cp = b0 & (0x7F >> width);
	for (unsigned i = 1; i < width; ++i) {
		const uint8_t bx = static_cast<uint8_t>(a[p + i]);
		if ((bx & 0xC0) != 0x80) return {b0, 1};
		cp = (cp << 6) | (bx & 0x3F);
	}
	return {cp, width};
}

constexpr bool IsAlpha(uint32_t c) noexcept {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
constexpr bool IsDigit(uint32_t c) noexcept { return c >= '0' && c <= '9'; }
constexpr bool IsWord(uint32_t c) noexcept { return IsAlpha(c) || IsDigit(c) || c == '_'; }
constexpr bool IsSpace(uint32_t c) noexcept { return c == ' ' || c == '\t'; }
constexpr uint32_t LowerAscii(uint32_t c) noexcept {
	return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

bool MatchAscii(const Accessor &a, Pos p, Pos end, std::string_view word) noexcept {
	if (p + word.size() > end) return false;
	for (size_t j = 0; j < word.size(); ++j) {
		if (LowerAscii(static_cast<uint8_t>(a[p + j])) !=
			LowerAscii(static_cast<uint8_t>(word[j]))) return false;
	}
	return true;
}

// UTF-8 is decoded before testing Chinese chapter markers.
bool InCodePoints(uint32_t c, std::initializer_list<uint32_t> list) noexcept {
	for (const uint32_t x : list) if (x == c) return true;
	return false;
}

bool IsChineseNumber(uint32_t c) noexcept {
	return InCodePoints(c, {
		0x4E00, 0x4E8C, 0x4E09, 0x56DB, 0x4E94, 0x516D, 0x4E03, 0x516B, 0x4E5D,
		0x5341, 0x767E, 0x5343, 0x4E07, 0x4EBF, 0x5146, 0x96F6,
		0x58F9, 0x8D30, 0x53C1, 0x8086, 0x4F0D, 0x9646, 0x67D2, 0x634C, 0x7396,
		0x62FE, 0x4F70, 0x4EDF, 0x7532, 0x4E59, 0x4E19, 0x4E01, 0x620A,
		0x5DF1, 0x5E9A, 0x8F9B, 0x58EC, 0x7678,
		0x5B50, 0x4E11, 0x5BC5, 0x536F, 0x8FB0, 0x5DF3, 0x5348, 0x672A,
		0x7533, 0x9149, 0x620C, 0x4EA5
	});
}

bool IsChapterUnit(uint32_t c) noexcept {
	return InCodePoints(c, {
		0x7AE0, 0x8282, 0x7BC7, 0x90E8, 0x9996, 0x56DE, 0x624B,
		0x8BFE, 0x9875, 0x6BB5, 0x7EC4, 0x5377, 0x533A, 0x573A,
		0x5408, 0x5B63, 0x7EA7, 0x96C6, 0x4EFB, 0x6B65, 0x6761,
		0x4EF6, 0x5E74, 0x6708, 0x65E5, 0x5468, 0x5929, 0x8F6E,
		0x4E2A, 0x9879, 0x7C7B, 0x671F, 0x8BDD, 0x4F8B
	});
}

// Detect heading forms such as "Chapter 3", "3. Note", and Chinese numbered chapters.
bool IsHeading(const Accessor &a, Pos first, Pos end) noexcept {
	if (first >= end) return false;
	if (a[first] == '*' && first + 1 < end && IsSpace(a[first + 1])) return true;
	if (MatchAscii(a, first, end, "-*-") ) return true;

	constexpr std::string_view words[] = {
		"section", "chapter", "sheet", "season", "period", "round", "class",
		"term", "part", "page", "segment", "paragraph", "lesson", "region",
		"step", "level", "set", "grade", "year", "month", "week", "day"
	};
	for (const auto word : words) {
		if (!MatchAscii(a, first, end, word)) continue;
		Pos j = first + word.size();
		if (j >= end || !IsSpace(a[j])) continue;
		while (j < end && IsSpace(a[j])) ++j;
		if (j < end && (IsAlpha(a[j]) || IsDigit(a[j]))) return true;
	}

	// Ordered list headings: "1. Something" or "A. Something".
	Pos j = first;
	while (j < end && (IsDigit(a[j]) || (a[j] >= 'A' && a[j] <= 'Z'))) ++j;
	if (j > first && j + 1 < end && a[j] == '.' && IsSpace(a[j + 1])) return true;

	j = first;
	auto cp = ReadChar(a, j, end);
	const bool hasPrefix = cp.value == 0x7B2C;
	if (hasPrefix) { // "Di" prefix in Chinese numbered chapters.
		j += cp.width;
		while (j < end && IsSpace(a[j])) ++j;
	}
	bool seen = false;
	bool hasChineseNumeral = false;
	while (j < end) {
		cp = ReadChar(a, j, end);
		if (!IsChineseNumber(cp.value) && !IsDigit(cp.value)) break;
		seen = true;
		hasChineseNumeral |= IsChineseNumber(cp.value);
		j += cp.width;
	}
	if (!seen) return false;
	while (j < end && IsSpace(a[j])) ++j;
	if (j == end) return hasPrefix || hasChineseNumeral;
	cp = ReadChar(a, j, end);
	return IsChapterUnit(cp.value) || ((hasPrefix || hasChineseNumeral) && (cp.value == '.' || cp.value == 0x3001));
}

bool IsSection(const Accessor &a, Pos first, Pos end) noexcept {
	if (end <= first + 2 || a[first] != '[') return false;
	Pos e = end;
	while (e > first && IsSpace(a[e - 1])) --e;
	return e > first + 2 && a[e - 1] == ']';
}

// Keep comments and entire heading lines ahead of inline tokens.
int LineStyle(const Accessor &a, Pos start, Pos end) noexcept {
	Pos p = start;
	while (p < end && IsSpace(a[p])) ++p;
	if (p == end) return SCE_TXT_DEFAULT;
	if (a[p] == '#' || a[p] == ';') return SCE_TXT_COMMENT;
	if (IsSection(a, p, end) || IsHeading(a, p, end)) return SCE_TXT_HEADING;
	if ((a[p] == '-' || a[p] == '=' || a[p] == '+') && p + 1 < end && IsSpace(a[p + 1])) {
		return SCE_TXT_SEPARATOR;
	}
	if (a[p] == '$' && p + 1 < end && a[p + 1] != '#') return SCE_TXT_LINK;
	return SCE_TXT_DEFAULT;
}

// Styles [start, end) while preserving unstyled gaps.
void ColorToken(Accessor &a, Pos start, Pos end, int style) {
	a.ColorTo(start, SCE_TXT_DEFAULT);
	a.ColorTo(end, style);
}

// Scan a simple URI/filename token (UTF-8 bytes are intentionally allowed).
Pos ScanNonSpace(const Accessor &a, Pos p, Pos end) noexcept {
	while (p < end) {
		const uint8_t c = static_cast<uint8_t>(a[p]);
		if (c <= ' ' || c == '"' || c == '\'' || c == '`' || c == '<' || c == '>') break;
		++p;
	}
	return p;
}

Pos MatchUrl(const Accessor &a, Pos p, Pos end) noexcept {
	constexpr std::string_view schemes[] = {
		"https://", "http://", "tftp://", "ftp://", "smtp://", "pop3://",
		"snmp://", "telnet://", "imap://", "dhcp://", "dns://", "nfs://",
		"file://", "sftp://", "ssl://", "smb://", "vmess://", "socks://"
	};
	for (auto scheme : schemes) {
		if (MatchAscii(a, p, end, scheme)) return ScanNonSpace(a, p + scheme.size(), end);
	}
	return p;
}

Pos MatchPath(const Accessor &a, Pos p, Pos end) noexcept {
	if (p >= end) return p;
	const uint8_t c = static_cast<uint8_t>(a[p]);
	bool root = (c == '/' && p + 1 < end && a[p + 1] != '/');
	if (IsAlpha(c) && p + 2 < end && a[p + 1] == ':' && (a[p + 2] == '\\' || a[p + 2] == '/')) root = true;
	if (c == '~' && p + 1 < end && (a[p + 1] == '/' || a[p + 1] == '\\')) root = true;
	if (c == '.' && p + 1 < end && (a[p + 1] == '/' || a[p + 1] == '\\')) root = true;
	if (c == '.' && p + 2 < end && a[p + 1] == '.' && (a[p + 2] == '/' || a[p + 2] == '\\')) root = true;
	return root ? ScanNonSpace(a, p, end) : p;
}

Pos MatchEmail(const Accessor &a, Pos p, Pos end) noexcept {
	Pos i = p;
	while (i < end && (IsWord(a[i]) || a[i] == '.' || a[i] == '-' || a[i] == '+')) ++i;
	if (i == p || i >= end || a[i] != '@') return p;
	++i;
	const Pos domainBegin = i;
	bool dot = false;
	while (i < end && (IsWord(a[i]) || a[i] == '-' || a[i] == '.')) {
		if (a[i] == '.') dot = true;
		++i;
	}
	return (dot && i >= domainBegin + 4) ? i : p;
}

Pos MatchKey(const Accessor &a, Pos p, Pos end) noexcept {
	if (!IsAlpha(a[p]) && a[p] != '_' && a[p] != '.' && a[p] != '-') return p;
	Pos i = p + 1;
	while (i < end && (IsWord(a[i]) || a[i] == '.' || a[i] == '-')) ++i;
	Pos eq = i;
	while (eq < end && IsSpace(a[eq])) ++eq;
	return (eq < end && a[eq] == '=') ? i : p;
}

uint32_t ClosingQuote(uint32_t cp) noexcept {
	switch (cp) {
	case '\'': case '"': case '`': return cp;
	case 0x2018: return 0x2019; // curly single quote
	case 0x201C: case 0x201E: return 0x201D; // curly double quote
	default: return 0;
	}
}
uint32_t ClosingBracket(uint32_t cp) noexcept {
	switch (cp) {
	case '[': return ']'; case '{': return '}'; case '(': return ')'; case '<': return '>';
	case 0x3010: return 0x3011; case 0xFF08: return 0xFF09;
	case 0x300A: return 0x300B; case 0x300E: return 0x300F;
	case 0x300C: return 0x300D; case 0x3014: return 0x3015;
	case 0xFF3B: return 0xFF3D; case 0xFF1C: return 0xFF1E;
	case 0x00AB: return 0x00BB; case 0xFE43: return 0xFE44;
	default: return 0;
	}
}

Pos FindCloser(const Accessor &a, Pos p, Pos end, uint32_t closer, bool escapes) noexcept {
	while (p < end) {
		auto cp = ReadChar(a, p, end);
		if (escapes && cp.value == '\\' && p + cp.width < end) {
			p += cp.width;
			cp = ReadChar(a, p, end);
			p += cp.width;
			continue;
		}
		p += cp.width;
		if (cp.value == closer) return p;
	}
	return 0;
}

bool IsSymbol(uint32_t cp) noexcept {
	if (cp < 0x80) {
		constexpr std::string_view punctuation = ",.;:!?@#$%^&*+-_=|\\/<>~";
		return punctuation.find(static_cast<char>(cp)) != std::string_view::npos;
	}
	// Unicode punctuation, mathematical shapes, arrows, circled numerals etc.
	return (cp >= 0x2000 && cp <= 0x33FF) ||
		(cp >= 0xFF01 && cp <= 0xFF65) ||
		InCodePoints(cp, {0x00A9, 0x00AE, 0x00B0, 0x00B1, 0x00D7, 0x00F7});
}

// Apply line-based patterns, then inline highlighters in TextMate-like priority.
void ColouriseTxtSyntax(Sci_PositionU startPos, Sci_Position lengthDoc, int /*initStyle*/,
	LexerWordList /*keywordLists*/, Accessor &styler) {
	if (lengthDoc <= 0) return;
	const Pos docEnd = static_cast<Pos>(styler.Length());
	const Pos targetEnd = std::min(docEnd, startPos + static_cast<Pos>(lengthDoc));
	Sci_Line line = styler.GetLine(startPos);
	Pos lineStart = static_cast<Pos>(styler.LineStart(line));
	styler.StartAt(lineStart);
	styler.StartSegment(lineStart);

	while (lineStart < docEnd && lineStart < targetEnd) {
		const Pos next = static_cast<Pos>(styler.LineStart(line + 1));
		const Pos lineEnd = std::min(next, docEnd);
		Pos contentEnd = lineEnd;
		while (contentEnd > lineStart && (styler[contentEnd - 1] == '\r' || styler[contentEnd - 1] == '\n')) --contentEnd;
		const int fullStyle = LineStyle(styler, lineStart, contentEnd);
		if (fullStyle != SCE_TXT_DEFAULT) {
			styler.ColorTo(lineEnd, fullStyle);
		} else {
			Pos p = lineStart;
			while (p < contentEnd) {
				// -- 1: Full URLs, paths, e-mail addresses --
				Pos last = MatchUrl(styler, p, contentEnd);
				if (last == p) last = MatchPath(styler, p, contentEnd);
				if (last == p && (IsAlpha(styler[p]) || IsDigit(styler[p]))) last = MatchEmail(styler, p, contentEnd);
				if (last > p) { ColorToken(styler, p, last, SCE_TXT_LINK); p = last; continue; }

				// -- 2: $variable names and assignment keys --
				if (styler[p] == '$' && p + 1 < contentEnd && (IsWord(styler[p + 1]))) {
					last = p + 2;
					while (last < contentEnd && (IsWord(styler[last]) || styler[last] == '.')) ++last;
					ColorToken(styler, p, last, SCE_TXT_VARIABLE); p = last; continue;
				}
				last = MatchKey(styler, p, contentEnd);
				if (last > p) { ColorToken(styler, p, last, SCE_TXT_VARIABLE); p = last; continue; }

				// -- 3: Strings and bracketed content --
				const auto cp = ReadChar(styler, p, contentEnd);
				const uint32_t quote = ClosingQuote(cp.value);
				// Skip apostrophes within words (e.g. "it's").
				const bool apostrophe = cp.value == '\'' && p > lineStart && p + 1 < contentEnd &&
					IsAlpha(styler[p - 1]) && IsAlpha(styler[p + 1]);
				if (quote && !apostrophe) {
					last = FindCloser(styler, p + cp.width, contentEnd, quote, true);
					if (!last) last = contentEnd;
					ColorToken(styler, p, last, SCE_TXT_STRING); p = last; continue;
				}
				const uint32_t bracket = ClosingBracket(cp.value);
				if (bracket) {
					last = FindCloser(styler, p + cp.width, contentEnd, bracket, false);
					if (last) { ColorToken(styler, p, last, SCE_TXT_BRACKET); p = last; continue; }
				}

				// -- 4: Numeric literals and punctuation --
				if (IsDigit(cp.value)) {
					last = p + 1;
					while (last < contentEnd && IsDigit(styler[last])) ++last;
					ColorToken(styler, p, last, SCE_TXT_NUMBER); p = last; continue;
				}
				if (IsSymbol(cp.value)) {
					ColorToken(styler, p, p + cp.width, SCE_TXT_SYMBOL);
				}
				p += cp.width;
			}
			styler.ColorTo(lineEnd, SCE_TXT_DEFAULT);
		}
		++line;
		lineStart = lineEnd;
	}
	styler.Flush();
}

} // namespace

extern const LexerModule lmTxtSyntax(SCLEX_TXTSYNTAX, ColouriseTxtSyntax, "txtsyntax");
