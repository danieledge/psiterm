// htmlsimp.cpp - see htmlsimp.h. MIT licence (see LICENSE at the top of the repository).
#include "htmlsimp.h"
#include <string.h>
#include <stdlib.h>

namespace am {

namespace {

enum TAction
	{
	AKeep,          // the tag, with the attributes its kind allows
	ADiv,           // an HTML5 block Links does not know: becomes <div>
	ADivLite,       // as ADiv, but dropped with its content in text-only mode
	AUnwrap,        // the tag goes, its content stays
	ASkip,          // the tag and everything inside it go
	ARawDrop,       // script, style: raw text, dropped
	ARawKeep        // title, textarea: raw text, kept
	};

enum TAttrs
	{
	TNone, TA, TImg, TForm, TInput, TTextarea, TSelect, TOption, TButton, TBase,
	TMeta, TCell, TId, TOl, TLi, TFrame
	};

struct TTagInfo
	{
	const char* iName;
	uint8_t iAction;
	uint8_t iAttrs;
	uint8_t iVoid;
	};

const TTagInfo kTags[] =
	{
	// what Links lays out
	{ "a", AKeep, TA, 0 }, { "p", AKeep, TNone, 0 }, { "div", AKeep, TNone, 0 },
	{ "br", AKeep, TNone, 1 }, { "hr", AKeep, TNone, 1 }, { "img", AKeep, TImg, 1 },
	{ "image", AKeep, TImg, 1 },
	{ "h1", AKeep, TId, 0 }, { "h2", AKeep, TId, 0 }, { "h3", AKeep, TId, 0 },
	{ "h4", AKeep, TId, 0 }, { "h5", AKeep, TId, 0 }, { "h6", AKeep, TId, 0 },
	{ "ul", AKeep, TNone, 0 }, { "ol", AKeep, TOl, 0 }, { "li", AKeep, TLi, 0 },
	{ "dl", AKeep, TNone, 0 }, { "dt", AKeep, TId, 0 }, { "dd", AKeep, TNone, 0 },
	{ "menu", AKeep, TNone, 0 }, { "dir", AKeep, TNone, 0 },
	{ "b", AKeep, TNone, 0 }, { "strong", AKeep, TNone, 0 }, { "i", AKeep, TNone, 0 },
	{ "em", AKeep, TNone, 0 }, { "u", AKeep, TNone, 0 }, { "s", AKeep, TNone, 0 },
	{ "del", AKeep, TNone, 0 }, { "ins", AKeep, TNone, 0 }, { "strike", AKeep, TNone, 0 },
	{ "code", AKeep, TNone, 0 }, { "tt", AKeep, TNone, 0 }, { "samp", AKeep, TNone, 0 },
	{ "kbd", AKeep, TNone, 0 }, { "var", AKeep, TNone, 0 },
	{ "sub", AKeep, TNone, 0 }, { "sup", AKeep, TNone, 0 }, { "q", AKeep, TNone, 0 },
	{ "cite", AKeep, TNone, 0 }, { "dfn", AKeep, TNone, 0 }, { "abbr", AKeep, TNone, 0 },
	{ "pre", AKeep, TNone, 0 }, { "listing", AKeep, TNone, 0 }, { "xmp", AKeep, TNone, 0 },
	{ "blockquote", AKeep, TNone, 0 }, { "address", AKeep, TNone, 0 },
	{ "center", AKeep, TNone, 0 },
	{ "table", AKeep, TNone, 0 }, { "caption", AKeep, TNone, 0 }, { "tr", AKeep, TNone, 0 },
	{ "td", AKeep, TCell, 0 }, { "th", AKeep, TCell, 0 }, { "thead", AKeep, TNone, 0 },
	{ "tbody", AKeep, TNone, 0 }, { "tfoot", AKeep, TNone, 0 },
	{ "form", AKeep, TForm, 0 }, { "input", AKeep, TInput, 1 },
	{ "textarea", ARawKeep, TTextarea, 0 }, { "select", AKeep, TSelect, 0 },
	{ "option", AKeep, TOption, 0 }, { "button", AKeep, TButton, 0 },
	{ "html", AKeep, TNone, 0 }, { "head", AKeep, TNone, 0 }, { "body", AKeep, TNone, 0 },
	{ "title", ARawKeep, TNone, 0 }, { "base", AKeep, TBase, 1 }, { "meta", AKeep, TMeta, 1 },
	{ "frameset", AKeep, TFrame, 0 }, { "frame", AKeep, TFrame, 1 }, { "noframes", AKeep, TNone, 0 },
	// HTML5 blocks: Links ignores them, so a <div> keeps the line breaks
	{ "section", ADiv, TNone, 0 }, { "article", ADiv, TNone, 0 }, { "main", ADiv, TNone, 0 },
	{ "header", ADiv, TNone, 0 }, { "figure", ADiv, TNone, 0 }, { "figcaption", ADiv, TNone, 0 },
	{ "details", ADiv, TNone, 0 }, { "summary", ADiv, TNone, 0 }, { "fieldset", ADiv, TNone, 0 },
	{ "legend", ADiv, TNone, 0 }, { "hgroup", ADiv, TNone, 0 }, { "dialog", ADiv, TNone, 0 },
	{ "nav", ADivLite, TNone, 0 }, { "aside", ADivLite, TNone, 0 }, { "footer", ADivLite, TNone, 0 },
	// gone, with what is inside them
	{ "script", ARawDrop, TNone, 0 }, { "style", ARawDrop, TNone, 0 },
	{ "noscript", ASkip, TNone, 0 }, { "svg", ASkip, TNone, 0 }, { "math", ASkip, TNone, 0 },
	{ "iframe", ASkip, TNone, 0 }, { "template", ASkip, TNone, 0 }, { "object", ASkip, TNone, 0 },
	{ "applet", ASkip, TNone, 0 }, { "video", ASkip, TNone, 0 }, { "audio", ASkip, TNone, 0 },
	{ "canvas", ASkip, TNone, 0 }, { "map", ASkip, TNone, 0 }, { "noembed", ASkip, TNone, 0 },
	// gone (void, or only their content matters)
	{ "link", AUnwrap, TNone, 1 }, { "source", AUnwrap, TNone, 1 }, { "track", AUnwrap, TNone, 1 },
	{ "param", AUnwrap, TNone, 1 }, { "wbr", AUnwrap, TNone, 1 }, { "col", AUnwrap, TNone, 1 },
	{ "area", AUnwrap, TNone, 1 }, { "embed", AUnwrap, TNone, 1 }, { "keygen", AUnwrap, TNone, 1 },
	{ 0, 0, 0, 0 }
	};

const TTagInfo kUnknown = { "", AUnwrap, TNone, 0 };   // span, font, label, custom elements...

const TTagInfo& Lookup(const char* aName)
	{
	for (const TTagInfo* t = kTags; t->iName; t++)
		if (strcmp(t->iName, aName) == 0)
			return *t;
	return kUnknown;
	}

bool OneOf(const char* aName, const char* const* aList)
	{
	for (; *aList; aList++)
		if (strcmp(*aList, aName) == 0)
			return true;
	return false;
	}

// the attributes each kind of tag keeps (img, a and meta are special cases)
bool KeepAttr(int aKind, const char* aAttr)
	{
	static const char* const kForm[] = { "action", "method", "enctype", "accept-charset", "name", 0 };
	static const char* const kInput[] = { "type", "name", "value", "checked", "size", "maxlength",
		"disabled", "readonly", 0 };
	static const char* const kTextarea[] = { "name", "rows", "cols", "disabled", "readonly", 0 };
	static const char* const kSelect[] = { "name", "multiple", "size", "disabled", 0 };
	static const char* const kOption[] = { "value", "selected", "disabled", 0 };
	static const char* const kButton[] = { "type", "name", "value", "disabled", 0 };
	static const char* const kCell[] = { "colspan", "rowspan", 0 };
	static const char* const kFrame[] = { "src", "name", "rows", "cols", 0 };
	switch (aKind)
		{
	case TForm: return OneOf(aAttr, kForm);
	case TInput: return OneOf(aAttr, kInput);
	case TTextarea: return OneOf(aAttr, kTextarea);
	case TSelect: return OneOf(aAttr, kSelect);
	case TOption: return OneOf(aAttr, kOption);
	case TButton: return OneOf(aAttr, kButton);
	case TBase: return strcmp(aAttr, "href") == 0;
	case TCell: return OneOf(aAttr, kCell);
	case TId: return strcmp(aAttr, "id") == 0;
	case TOl: return strcmp(aAttr, "start") == 0 || strcmp(aAttr, "type") == 0;
	case TLi: return strcmp(aAttr, "id") == 0 || strcmp(aAttr, "value") == 0;
	case TFrame: return OneOf(aAttr, kFrame);
	default: return false;
		}
	}

// elements that mean nothing when empty (table cells, form fields and the
// like do, so they are always sent)
bool Droppable(const char* aName)
	{
	static const char* const kKeepEmpty[] = { "td", "th", "tr", "table", "thead", "tbody", "tfoot",
		"caption", "textarea", "select", "option", "button", "html", "head", "body", "title",
		"frameset", "noframes", "form", 0 };
	return !OneOf(aName, kKeepEmpty);
	}

// reader mode: a class or id that names page furniture. The word must
// start a token of the value ("main-nav", "navigation", "site_footer"),
// so "unavailable" is not navigation
bool Furniture(const char* aV, size_t aLen)
	{
	static const char* const kWords[] = { "nav", "menu", "sidebar", "footer", "cookie", "consent", "promo",
		"share", "social", "related", "comments", "breadcrumb", "banner", "advert", "sponsor", "newsletter", 0 };
	size_t i = 0;
	while (i < aLen)
		{
		// the start of a token
		for (const char* const* w = kWords; *w; w++)
			{
			size_t l = strlen(*w);
			if (i + l > aLen)
				continue;
			size_t k = 0;
			while (k < l && ((aV[i + k] >= 'A' && aV[i + k] <= 'Z') ? aV[i + k] + 32 : aV[i + k]) == (*w)[k])
				k++;
			if (k == l)
				return true;
			}
		// to the next token
		while (i < aLen && aV[i] != ' ' && aV[i] != '-' && aV[i] != '_' && aV[i] != ':' && aV[i] != '.')
			i++;
		while (i < aLen && (aV[i] == ' ' || aV[i] == '-' || aV[i] == '_' || aV[i] == ':' || aV[i] == '.'))
			i++;
		}
	return false;
	}

bool IsSpace(uint8_t aC) { return aC == ' ' || aC == '\n' || aC == '\r' || aC == '\t' || aC == '\f'; }
bool IsAlpha(uint8_t aC) { return (aC >= 'a' && aC <= 'z') || (aC >= 'A' && aC <= 'Z'); }
uint8_t Lower(uint8_t aC) { return (aC >= 'A' && aC <= 'Z') ? aC + 32 : aC; }

bool StartsNoCase(const char* aS, size_t aLen, const char* aP)
	{
	while (aLen && IsSpace((uint8_t)*aS)) { aS++; aLen--; }
	for (; *aP; aP++, aS++, aLen--)
		if (!aLen || Lower((uint8_t)*aS) != (uint8_t)*aP)
			return false;
	return true;
	}

// a picture Links cannot draw: name.svg (before any ?query or #part)
bool IsSvg(const char* aV, size_t aLen)
	{
	size_t n = 0;
	while (n < aLen && aV[n] != '?' && aV[n] != '#')
		n++;
	return n >= 4 && aV[n - 4] == '.' && Lower((uint8_t)aV[n - 3]) == 's' && Lower((uint8_t)aV[n - 2]) == 'v'
		&& Lower((uint8_t)aV[n - 1]) == 'g';
	}

} // namespace

void HtmlSimplifier::Begin(HtmlSink* aOut, int aMode, const char* aBase)
	{
	iOut = aOut;
	iMode = aMode;
	iState = SText;
	iIn = iOutCount = 0;
	iSpace = iSpaceNl = iStarted = false;
	iPre = 0;
	iForm = 0;
	iSkipDepth = 0;
	iRawKeep = false;
	iPendLen = 0;
	iPendN = 0;
	iFurniture = iMainTag = false;
	iMainDepth = iMainOther = 0;
	iDone = false;
	iLinkDepth = 0;
	iCapLen = 0;
	iCapturing = false;
	iCapDepth = iCapOther = 0;
	iCapLinkChars = iCapOtherChars = iCapLinks = 0;
	if (aBase && aOut)
		{
		Emit("<base href=\"");
		for (const char* p = aBase; *p; p++)
			{
			if (*p == '"') Emit("&quot;");
			else Emit(p, 1);
			}
		Emit("\">\n");
		}
	}

void HtmlSimplifier::Write(const char* aS, size_t aLen)
	{
	if (!aLen || !iOut)
		return;
	if (iCapturing)
		{
		// a list being judged: held back
		if (iCapLen + aLen <= sizeof(iCap))
			{
			memcpy(iCap + iCapLen, aS, aLen);
			iCapLen += aLen;
			return;
			}
		CaptureEnd(true);                    // too big to judge: it is kept
		}
	iOut->Put(aS, aLen);
	iOutCount += aLen;
	iStarted = true;
	}

// the list held back is sent (aKeep), or dropped as a menu
void HtmlSimplifier::CaptureEnd(bool aKeep)
	{
	iCapturing = false;
	if (aKeep && iCapLen && iOut)
		{
		iOut->Put(iCap, iCapLen);
		iOutCount += iCapLen;
		iStarted = true;
		}
	iCapLen = 0;
	}

void HtmlSimplifier::FlushPending()
	{
	size_t n = iPendLen;
	iPendLen = 0;
	iPendN = 0;
	Write(iPend, n);
	}

void HtmlSimplifier::Emit(const char* aS, size_t aLen)
	{
	if (!aLen)
		return;
	if (iPendLen)
		FlushPending();                      // the open tags have content now
	Write(aS, aLen);
	}

// "<name attrs>"; aDroppable: held back until something goes inside it
void HtmlSimplifier::OpenTag(const char* aName, const char* aAttrs, size_t aLen, bool aDroppable)
	{
	FlushSpace();
	size_t n = strlen(aName);
	if (aDroppable && iPendN < (int)(sizeof(iPendAt) / sizeof(iPendAt[0])) && iPendLen + n + aLen + 2 <= sizeof(iPend))
		{
		iPendAt[iPendN++] = (uint16_t)iPendLen;
		iPend[iPendLen++] = '<';
		memcpy(iPend + iPendLen, aName, n);
		iPendLen += n;
		memcpy(iPend + iPendLen, aAttrs, aLen);
		iPendLen += aLen;
		iPend[iPendLen++] = '>';
		return;
		}
	Emit("<", 1);
	Write(aName, n);
	Write(aAttrs, aLen);
	Write(">", 1);
	}

void HtmlSimplifier::CloseTag(const char* aName)
	{
	if (iPendN)
		{
		// the end of the last open tag held back: the pair was empty
		const char* p = iPend + iPendAt[iPendN - 1] + 1;
		size_t n = strlen(aName);
		if (strncmp(p, aName, n) == 0 && (p[n] == '>' || p[n] == ' '))
			{
			iPendLen = iPendAt[--iPendN];
			return;
			}
		}
	FlushSpace();
	Emit("</", 2);
	Write(aName, strlen(aName));
	Write(">", 1);
	}

void HtmlSimplifier::Emit(const char* aS)
	{
	Emit(aS, strlen(aS));
	}

void HtmlSimplifier::FlushSpace()
	{
	if (iSpace && iStarted)
		Emit(iSpaceNl ? "\n" : " ", 1);
	iSpace = iSpaceNl = false;
	}

void HtmlSimplifier::EmitText(const char* aS, size_t aLen)
	{
	FlushSpace();
	if (iCapturing)
		{
		if (iLinkDepth) iCapLinkChars += (uint32_t)aLen; else iCapOtherChars += (uint32_t)aLen;
		}
	Emit(aS, aLen);
	}

void HtmlSimplifier::TextChar(uint8_t aC)
	{
	if (iSkipDepth || aC == 0)
		return;
	if (!iPre && IsSpace(aC))
		{
		iSpace = true;
		if (aC == '\n')
			iSpaceNl = true;
		return;
		}
	char c = (char)aC;
	EmitText(&c, 1);
	}

void HtmlSimplifier::Feed(const uint8_t* aData, size_t aLen)
	{
	iIn += aLen;
	if (iDone)
		return;                              // (reader mode: past </main>)
	for (size_t i = 0; i < aLen && !iDone; i++)
		{
		// an element dropped for too long (its end never came, or the page
		// is not nested as it says): what follows is kept after all
		if (iSkipDepth && ++iSkipBytes > kMaxSkip && iState == SText)
			iSkipDepth = 0;
		Char(aData[i]);
		}
	}

void HtmlSimplifier::End()
	{
	// an unfinished tag or comment at the end is dropped; pending white space too
	if (iState == SLt && !iDone)
		EmitText("&lt;", 4);
	iState = SText;
	iPendLen = 0;                            // empty open tags at the very end
	iPendN = 0;
	if (iCapturing)
		CaptureEnd(true);                    // (a list never closed: kept)
	}

void HtmlSimplifier::StartTag(bool aEnd)
	{
	iEnd = aEnd;
	iSelfClose = false;
	iNameLen = 0;
	iNameLong = false;
	iTagLen = 0;
	iHidden = iHaveSrc = iMetaUseful = false;
	iFurniture = iMainTag = false;
	iImgW = iImgH = -1;
	iAltLen = 0;
	iAttrLen = 0;
	}

void HtmlSimplifier::Char(uint8_t aC)
	{
	switch (iState)
		{
	case SText:
		if (aC == '<')
			iState = SLt;
		else
			TextChar(aC);
		return;

	case SLt:
		if (IsAlpha(aC))
			{
			StartTag(false);
			iName[iNameLen++] = (char)Lower(aC);
			iState = STagName;
			}
		else if (aC == '/')
			{
			StartTag(true);
			iState = SEndTagName;
			}
		else if (aC == '!')
			iState = SBang;
		else if (aC == '?')
			iState = SDecl;
		else
			{
			// a lone '<' in text
			if (!iSkipDepth)
				EmitText("&lt;", 4);
			iState = SText;
			Char(aC);
			}
		return;

	case STagName:
	case SEndTagName:
		if (IsSpace(aC))
			iState = SBeforeAttr;
		else if (aC == '>')
			{
			if (iNameLen || iState == STagName)
				TagDone();
			else
				iState = SText;                  // "</>"
			}
		else if (aC == '/' && iState == STagName)
			iState = SSelfClose;
		else if (iNameLen < sizeof(iName) - 1)
			iName[iNameLen++] = (char)Lower(aC);
		else
			iNameLong = true;
		return;

	case SBeforeAttr:
		if (IsSpace(aC))
			return;
		if (aC == '/') { iState = SSelfClose; return; }
		if (aC == '>') { TagDone(); return; }
		iAttrLen = 0;
		iValLen = 0;
		iValLong = false;
		iHasVal = false;
		iAttr[iAttrLen++] = (char)Lower(aC);
		iState = SAttrName;
		return;

	case SAttrName:
		if (IsSpace(aC)) { iState = SAfterAttrName; return; }
		if (aC == '=') { iState = SBeforeValue; return; }
		if (aC == '/') { AttrDone(); iState = SSelfClose; return; }
		if (aC == '>') { AttrDone(); TagDone(); return; }
		if (iAttrLen < sizeof(iAttr) - 1)
			iAttr[iAttrLen++] = (char)Lower(aC);
		else
			iValLong = true;                     // a name this long is nothing we keep
		return;

	case SAfterAttrName:
		if (IsSpace(aC)) return;
		if (aC == '=') { iState = SBeforeValue; return; }
		AttrDone();
		iState = SBeforeAttr;
		Char(aC);
		return;

	case SBeforeValue:
		if (IsSpace(aC)) return;
		iHasVal = true;
		if (aC == '"') { iState = SValueDq; return; }
		if (aC == '\'') { iState = SValueSq; return; }
		if (aC == '>') { AttrDone(); TagDone(); return; }
		iState = SValueUq;
		Char(aC);
		return;

	case SValueDq:
	case SValueSq:
	case SValueUq:
		if ((iState == SValueDq && aC == '"') || (iState == SValueSq && aC == '\''))
			{
			AttrDone();
			iState = SBeforeAttr;
			return;
			}
		if (iState == SValueUq && (IsSpace(aC) || aC == '>'))
			{
			AttrDone();
			if (aC == '>')
				TagDone();
			else
				iState = SBeforeAttr;
			return;
			}
		if (iValLen < sizeof(iVal) - 1)
			iVal[iValLen++] = (char)aC;
		else
			iValLong = true;
		return;

	case SSelfClose:
		if (aC == '>')
			{
			iSelfClose = true;
			TagDone();
			return;
			}
		iState = SBeforeAttr;
		Char(aC);
		return;

	case SBang:
		iState = aC == '-' ? SBangDash : (aC == '>' ? SText : SDecl);
		return;

	case SBangDash:
		if (aC == '-')
			{
			iDash = 0;
			iState = SComment;
			}
		else
			iState = aC == '>' ? SText : SDecl;
		return;

	case SComment:
		if (aC == '-')
			iDash++;
		else if (aC == '>' && iDash >= 2)
			iState = SText;
		else
			iDash = 0;
		return;

	case SDecl:
		if (aC == '>')
			iState = SText;
		return;

	case SRaw:
		if (aC == '<')
			iState = SRawLt;
		else if (iRawKeep)
			TextChar(aC);
		return;

	case SRawLt:
		if (aC == '/')
			{
			iRawMatch = 0;
			iState = SRawName;
			return;
			}
		if (iRawKeep)
			EmitText("&lt;", 4);
		iState = SRaw;
		Char(aC);
		return;

	case SRawSlash:
	case SRawName:
		{
		size_t len = strlen(iRaw);
		if (iRawMatch < len && Lower(aC) == (uint8_t)iRaw[iRawMatch])
			{
			iRawMatch++;
			return;
			}
		if (iRawMatch == len && (IsSpace(aC) || aC == '/' || aC == '>'))
			{
			if (aC == '>')
				RawDone();
			else
				iState = SToGt;
			return;
			}
		// not the end after all: what was held back is text
		if (iRawKeep)
			{
			EmitText("&lt;/", 5);
			Emit(iRaw, iRawMatch);
			}
		iState = SRaw;
		Char(aC);
		return;
		}

	case SToGt:
		if (aC == '>')
			RawDone();
		return;
		}
	}

void HtmlSimplifier::RawDone()
	{
	if (iRawKeep)
		{
		if (strcmp(iRaw, "textarea") == 0 && iPre > 0)
			iPre--;
		Emit("</");
		Emit(iRaw);
		Emit(">");
		}
	iRawKeep = false;
	iState = SText;
	}

void HtmlSimplifier::AttrDone()
	{
	if (iEnd || !iAttrLen)
		return;
	iAttr[iAttrLen] = 0;
	iVal[iValLen] = 0;
	const char* a = iAttr;
	const TTagInfo& t = Lookup(iNameLong ? "" : (iName[iNameLen] = 0, iName));
	bool keep = false;
	const char* asName = a;
	if (strcmp(a, "hidden") == 0)
		{
		iHidden = true;
		return;
		}
	if (iMode == EReader)
		{
		if (strcmp(a, "role") == 0)
			{
			if (StartsNoCase(iVal, iValLen, "navigation") || StartsNoCase(iVal, iValLen, "banner")
				|| StartsNoCase(iVal, iValLen, "contentinfo") || StartsNoCase(iVal, iValLen, "complementary"))
				iFurniture = true;
			else if (StartsNoCase(iVal, iValLen, "main"))
				iMainTag = true;
			}
		else if ((strcmp(a, "class") == 0 || strcmp(a, "id") == 0) && Furniture(iVal, iValLen))
			iFurniture = true;
		}
	if (strcmp(a, "style") == 0)
		{
		// style="display:none" (any spacing or case)
		char squeezed[24];
		size_t n = 0;
		for (size_t i = 0; i < iValLen; i++)
			{
			uint8_t c = Lower((uint8_t)iVal[i]);
			if (IsSpace(c)) continue;
			if (c == ';') { n = 0; continue; }
			if (n < sizeof(squeezed) - 1) squeezed[n++] = (char)c;
			squeezed[n] = 0;
			if (n == 12 && memcmp(squeezed, "display:none", 12) == 0)
				iHidden = true;
			}
		return;
		}
	switch (t.iAttrs)
		{
	case TImg:
		if (strcmp(a, "src") == 0 || strcmp(a, "data-src") == 0 || strcmp(a, "data-original") == 0
			|| strcmp(a, "data-lazy-src") == 0)
			{
			// lazy loading puts the picture in data-src and a placeholder in src
			if (iHaveSrc || !iValLen || iValLong || StartsNoCase(iVal, iValLen, "data:") || IsSvg(iVal, iValLen))
				return;
			iHaveSrc = true;
			asName = "src";
			keep = true;
			}
		else if (strcmp(a, "alt") == 0)
			{
			size_t n = iValLen < sizeof(iAlt) - 1 ? iValLen : sizeof(iAlt) - 1;
			memcpy(iAlt, iVal, n);
			iAltLen = n;
			keep = true;
			}
		else if (strcmp(a, "width") == 0 || strcmp(a, "height") == 0)
			{
			int v = atoi(iVal);
			if (a[0] == 'w') iImgW = v; else iImgH = v;
			keep = iValLen && iVal[0] >= '0' && iVal[0] <= '9';
			}
		break;
	case TA:
		if (strcmp(a, "href") == 0)
			keep = !StartsNoCase(iVal, iValLen, "javascript:");
		else
			keep = strcmp(a, "name") == 0 || strcmp(a, "id") == 0;
		break;
	case TMeta:
		if (strcmp(a, "charset") == 0)
			{
			iMetaUseful = true;
			keep = true;
			}
		else if (strcmp(a, "http-equiv") == 0)
			{
			if (StartsNoCase(iVal, iValLen, "content-type") || StartsNoCase(iVal, iValLen, "refresh"))
				iMetaUseful = true;
			keep = true;
			}
		else
			keep = strcmp(a, "content") == 0;
		break;
	default:
		keep = KeepAttr(t.iAttrs, a);
		break;
		}
	if (!keep || iValLong)
		return;
	// ' name="value"', with any '"' in the value as &quot;
	size_t need = 1 + strlen(asName) + (iHasVal ? 3 + iValLen : 0);
	for (size_t i = 0; iHasVal && i < iValLen; i++)
		if (iVal[i] == '"')
			need += 5;
	if (iTagLen + need >= sizeof(iTag))
		return;
	iTag[iTagLen++] = ' ';
	size_t l = strlen(asName);
	memcpy(iTag + iTagLen, asName, l);
	iTagLen += l;
	if (iHasVal)
		{
		iTag[iTagLen++] = '=';
		iTag[iTagLen++] = '"';
		for (size_t i = 0; i < iValLen; i++)
			{
			if (iVal[i] == '"')
				{
				memcpy(iTag + iTagLen, "&quot;", 6);
				iTagLen += 6;
				}
			else
				iTag[iTagLen++] = iVal[i];
			}
		iTag[iTagLen++] = '"';
		}
	}

void HtmlSimplifier::TagDone()
	{
	iState = SText;
	iName[iNameLen] = 0;
	const TTagInfo& t = Lookup(iNameLong ? "" : iName);
	int action = t.iAction;
	if (action == ADivLite)
		action = iMode >= ETextOnly ? (int)ASkip : (int)ADiv;
	bool reader = iMode == EReader && !iNameLong;
	if (reader && strcmp(iName, "header") == 0)
		action = ASkip;                          // (the site's masthead and menus)

	// inside an element that is being dropped: only follow its nesting
	if (iSkipDepth)
		{
		bool same = !iNameLong && strcmp(iName, iSkip) == 0;
		if (same)
			{
			if (iEnd)
				iSkipDepth--;
			else if (!iSelfClose)
				iSkipDepth++;
			return;
			}
		if (!iEnd && (action == ARawDrop || action == ARawKeep))
			{
			strcpy(iRaw, iName);
			iRawKeep = false;
			iState = SRaw;
			return;
			}
		// the other elements inside it are counted too, so that an end tag
		// for an element it is inside ends it (its own end tag missing: the
		// rest of the page is not lost)
		if (!iEnd)
			{
			if (!iSelfClose && !t.iVoid)
				iSkipOther++;
			return;
			}
		if (iSkipOther > 0)
			{
			iSkipOther--;
			return;
			}
		if (iNameLong || &t == &kUnknown || t.iVoid)
			return;                              // (a stray end tag)
		iSkipDepth = 0;                          // and this end tag is used as usual
		}

	// form fields outside any form: buttons, check boxes and lists that only
	// a script could make work
	bool field = t.iAttrs == TInput || t.iAttrs == TButton || t.iAttrs == TSelect || t.iAttrs == TTextarea;
	if (field && !iForm)
		{
		if (iEnd)
			return;
		action = t.iAttrs == TInput ? (int)AUnwrap : t.iAttrs == TTextarea ? (int)ARawDrop : (int)ASkip;
		}
	if (t.iAttrs == TForm)
		{
		if (!iEnd)
			iForm++;
		else if (iForm > 0)
			iForm--;
		}

	if (iEnd)
		{
		if (reader && strcmp(iName, "a") == 0 && iLinkDepth > 0)
			iLinkDepth--;
		if (action == AKeep && !t.iVoid)
			{
			if (strcmp(iName, "pre") == 0 || strcmp(iName, "listing") == 0 || strcmp(iName, "xmp") == 0)
				{
				if (iPre > 0) iPre--;
				}
			CloseTag(iName);
			}
		else if (action == ADiv)
			CloseTag("div");
		if (reader)
			{
			// the end of a list being judged: a menu (nearly all links) goes
			if (iCapturing && (strcmp(iName, "ul") == 0 || strcmp(iName, "ol") == 0) && --iCapDepth <= 0)
				{
				bool menu = iCapLinks >= 3 && iCapLinkChars * 5 >= (iCapLinkChars + iCapOtherChars) * 4;
				CaptureEnd(!menu);
				}
			// the end of <main>: nothing more of the page is wanted
			if (iMainDepth && strcmp(iName, iMainName) == 0 && --iMainDepth == 0)
				{
				if (iCapturing)
					CaptureEnd(true);
				iDone = true;
				}
			}
		return;
		}

	// an open tag
	if (iHidden && !t.iVoid && action != ARawDrop && action != ARawKeep && !iSelfClose && !iNameLong)
		action = ASkip;
	if (reader)
		{
		if (iFurniture && !t.iVoid && action != ARawDrop && action != ARawKeep && !iSelfClose)
			action = ASkip;
		else if (action != ASkip)
			{
			if (strcmp(iName, "a") == 0)
				{
				iLinkDepth++;
				if (iCapturing)
					iCapLinks++;
				}
			// <main> or role="main": counted by its name, so its end is known
			if (iMainDepth && strcmp(iName, iMainName) == 0 && !iSelfClose && !t.iVoid)
				iMainDepth++;
			else if (!iMainDepth && (strcmp(iName, "main") == 0 || iMainTag) && !iSelfClose && !t.iVoid)
				{
				strcpy(iMainName, iName);
				iMainDepth = 1;
				}
			// a list: held back until its end says whether it is a menu
			if ((strcmp(iName, "ul") == 0 || strcmp(iName, "ol") == 0) && !iSelfClose)
				{
				if (iCapturing)
					iCapDepth++;
				else if (!iPendN || iPendLen + 64 < sizeof(iPend))
					{
					if (iPendLen)
						FlushPending();          // (what is open goes out as it is)
					iCapturing = true;
					iCapLen = 0;
					iCapDepth = 1;
					iCapLinkChars = iCapOtherChars = iCapLinks = 0;
					}
				}
			}
		}
	switch (action)
		{
	case ARawDrop:
	case ARawKeep:
		strcpy(iRaw, iName);
		iRawKeep = action == ARawKeep;
		iState = SRaw;
		if (iRawKeep)
			{
			FlushSpace();
			Emit("<");
			Emit(iName);
			Emit(iTag, iTagLen);
			Emit(">");
			if (strcmp(iName, "textarea") == 0)
				iPre++;
			}
		return;
	case ASkip:
		if (!iSelfClose && !t.iVoid && !iNameLong)
			{
			strcpy(iSkip, iName);
			iSkipDepth = 1;
			iSkipOther = 0;
			iSkipBytes = 0;
			}
		return;
	case ADiv:
		OpenTag("div", "", 0, true);
		return;
	case AUnwrap:
		return;
		}

	// AKeep
	if (t.iAttrs == TMeta && !iMetaUseful)
		return;
	if (t.iAttrs == TImg)
		{
		if (iImgW >= 0 && iImgH >= 0 && iImgW <= 2 && iImgH <= 2)
			return;                              // a tracking pixel
		if (iMode >= ETextOnly || !iHaveSrc)
			{
			// the alt text in its place
			if (iAltLen)
				{
				FlushSpace();
				EmitText("[", 1);
				Emit(iAlt, iAltLen);
				Emit("]");
				}
			return;
			}
		}
	OpenTag(iName, iTag, iTagLen, !t.iVoid && Droppable(iName));
	if (strcmp(iName, "pre") == 0 || strcmp(iName, "listing") == 0 || strcmp(iName, "xmp") == 0)
		iPre++;
	}

} // namespace am
