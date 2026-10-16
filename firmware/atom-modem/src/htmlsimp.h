// htmlsimp.h - a streaming HTML simplifier for the Atom's PsiWeb proxy.
// It turns a modern page into the small HTML that Links on the Psion lays
// out quickly: no scripts, styles, SVG, iframes, inline JSON or comments;
// only the attributes Links uses (href, src, alt, form fields...); runs of
// white space collapsed; HTML5 sections made into <div>s, which Links knows.
// Text, headings, lists, links, tables and forms are kept.
//
// It is a state machine fed a byte at a time, with fixed buffers (about
// 5 KB in all): memory does not grow with the page, and output never runs
// more than a tag's worth (a few KB) ahead of input. Plain C++, no Arduino
// calls: the Atom and the host tests run the same code.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_HTMLSIMP_H
#define ATOM_HTMLSIMP_H

#include <stdint.h>
#include <stddef.h>

namespace am {

class HtmlSink
	{
public:
	virtual ~HtmlSink() {}
	virtual void Put(const char* aData, size_t aLen) = 0;
	};

class HtmlSimplifier
	{
public:
	enum TMode
		{
		EKeepPictures = 1,       // simplified, pictures kept (<img src alt>)
		ETextOnly = 2,           // also: pictures become their alt text; nav, aside
		                         // and footer go (the "lite" reading)
		EReader = 3              // also: header goes; elements with a navigation,
		                         // banner, complementary or contentinfo role, or a
		                         // class or id that names page furniture (nav, menu,
		                         // sidebar, cookie, promo, share, social, related,
		                         // comments...) go with their contents; nothing after
		                         // </main> (or role="main") is sent; and a list whose
		                         // items are nearly all links (a menu) goes
		};
	HtmlSimplifier() : iOut(0) { Begin(0, EKeepPictures, 0); }
	// aBase: if not null, a <base href> to put first (the page was reached
	// through a redirect, so relative links must resolve against it)
	void Begin(HtmlSink* aOut, int aMode, const char* aBase);
	void Feed(const uint8_t* aData, size_t aLen);
	void End();                          // flushes anything held
	uint32_t In() const { return iIn; }
	uint32_t Out() const { return iOutCount; }

	// the largest output a single input byte can release (a whole tag)
	static const size_t kMaxBurst = 3500;
	// the most input one dropped element (svg, a hidden div...) may take:
	// past this its end tag is taken to be missing, and the page goes on
	static const uint32_t kMaxSkip = 48 * 1024;

private:
	enum TState
		{
		SText, SLt, STagName, SEndTagName, SBeforeAttr, SAttrName, SAfterAttrName,
		SBeforeValue, SValueDq, SValueSq, SValueUq, SSelfClose, SBang, SBangDash,
		SComment, SDecl, SRaw, SRawLt, SRawSlash, SRawName, SToGt
		};
	void Char(uint8_t aC);
	void TextChar(uint8_t aC);
	void Emit(const char* aS, size_t aLen);
	void Emit(const char* aS);
	void Write(const char* aS, size_t aLen);
	void FlushPending();
	void OpenTag(const char* aName, const char* aAttrs, size_t aLen, bool aDroppable);
	void CloseTag(const char* aName);
	void EmitText(const char* aS, size_t aLen);
	void FlushSpace();
	void StartTag(bool aEnd);
	void AttrDone();
	void TagDone();
	void RawDone();

	HtmlSink* iOut;
	int iMode;
	TState iState;
	uint32_t iIn, iOutCount;
	bool iSpace;                         // white space pending in text
	bool iSpaceNl;                       // ... and it had a line break
	bool iStarted;                       // anything emitted yet
	int iPre;                            // inside <pre> (or textarea): keep white space
	int iForm;                           // inside <form>: its fields are kept (outside,
	                                     // with no script to run them, they do nothing)
	// the tag being read
	bool iEnd;                           // </...>
	bool iSelfClose;
	char iName[16];
	size_t iNameLen;
	bool iNameLong;
	char iAttr[24];
	size_t iAttrLen;
	char iVal[1536];
	size_t iValLen;
	bool iValLong;
	bool iHasVal;
	char iTag[2048];                     // the attributes kept so far
	size_t iTagLen;
	bool iHidden;                        // hidden, or style="display:none"
	bool iHaveSrc;                       // img: a usable src is kept
	bool iMetaUseful;                    // meta: charset, content-type or refresh
	int iImgW, iImgH;                    // img: width/height, for tracking pixels
	char iAlt[160];                      // img: the alt text (text-only mode)
	size_t iAltLen;
	// an element dropped with everything inside it (svg, noscript...)
	char iSkip[16];
	int iSkipDepth;
	int iSkipOther;                      // other elements open inside it
	uint32_t iSkipBytes;                 // input bytes dropped by it so far
	// raw text (script, style: dropped; title, textarea: kept)
	char iRaw[16];
	size_t iRawMatch;
	bool iRawKeep;
	int iDash;                           // comments: '-' seen in a row
	// open tags not yet sent, because nothing has been put in them yet: if
	// their end tag comes next, the empty pair is never sent (<div></div>,
	// a link round an icon that was dropped)
	char iPend[1024];
	size_t iPendLen;
	uint16_t iPendAt[32];
	int iPendN;
	// reader mode
	bool iFurniture;                     // this tag: a role, class or id that marks page furniture
	bool iMainTag;                       // this tag: role="main"
	char iMainName[16];                  // the element that is <main> (or role="main")
	int iMainDepth;                      // inside it: its nesting
	int iMainOther;                      // ... other elements open inside it
	bool iDone;                          // past </main>: nothing more is sent
	int iLinkDepth;                      // inside <a>
	// a list being held back until it is known whether it is a menu
	char iCap[3072];
	size_t iCapLen;
	bool iCapturing;
	int iCapDepth;                       // the list's nesting (ul/ol inside it)
	int iCapOther;
	uint32_t iCapLinkChars, iCapOtherChars, iCapLinks;
	void CaptureEnd(bool aKeep);
	};

} // namespace am

#endif
