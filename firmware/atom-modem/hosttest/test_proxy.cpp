// test_proxy.cpp - tests for the Atom modem's web proxy (proxy.cpp) and its
// HTML simplifier (htmlsimp.cpp), against fakehal.h: simulated time, UART,
// WiFi, and a scripted web server behind UpConnect.
//   make -C hosttest test                       (synthetic pages only)
//   make -C hosttest test PAGES=/path/to/pages  (also real saved pages:
//        bbc.html, guardian.html, wiki.html, hn.html - see README)
// MIT licence (see LICENSE at the top of the repository).
#include "fakehal.h"
#include "../src/htmlsimp.h"
#include <zlib.h>
#include <map>
#include <set>
#include <algorithm>

static int gChecks = 0, gFails = 0;
#define CHECK(c) do { gChecks++; if (!(c)) { gFails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static bool Has(const std::string& s, const char* p) { return s.find(p) != std::string::npos; }

static_assert(sizeof(am::Settings) == 140, "the NVS record must keep its size");
static_assert(sizeof(am::Settings2) == 256, "the 2.0 NVS record must keep its size");

// ===== the simplifier on its own ===============================================

struct StrSink : am::HtmlSink
	{
	std::string s;
	void Put(const char* d, size_t n) override { s.append(d, n); }
	};

// simplified in pieces of aStep bytes (0: all at once)
static std::string Simplify(const std::string& aIn, int aMode = am::HtmlSimplifier::EKeepPictures,
	size_t aStep = 0, const char* aBase = 0)
	{
	StrSink out;
	am::HtmlSimplifier h;
	h.Begin(&out, aMode, aBase);
	if (!aStep) aStep = aIn.size() ? aIn.size() : 1;
	for (size_t i = 0; i < aIn.size(); i += aStep)
		h.Feed((const uint8_t*)aIn.data() + i, std::min(aStep, aIn.size() - i));
	h.End();
	return out.s;
	}

// Well-formed for Links: every '<' opens a known tag; attributes are known
// ones, double-quoted, with no '"' inside; no scripts, styles or event
// handlers; <div> never closed more often than opened. aWhy: the first fault.
static bool WellFormed(const std::string& s, std::string& aWhy)
	{
	static const std::set<std::string> kTags = { "a", "p", "div", "br", "hr", "img", "image", "h1", "h2",
		"h3", "h4", "h5", "h6", "ul", "ol", "li", "dl", "dt", "dd", "menu", "dir", "b", "strong", "i", "em",
		"u", "s", "del", "ins", "strike", "code", "tt", "samp", "kbd", "var", "sub", "sup", "q", "cite",
		"dfn", "abbr", "pre", "listing", "xmp", "blockquote", "address", "center", "table", "caption", "tr",
		"td", "th", "thead", "tbody", "tfoot", "form", "input", "textarea", "select", "option", "button",
		"html", "head", "body", "title", "base", "meta", "frameset", "frame", "noframes" };
	static const std::set<std::string> kAttrs = { "href", "name", "id", "src", "alt", "width", "height",
		"action", "method", "enctype", "accept-charset", "type", "value", "checked", "size", "maxlength",
		"disabled", "readonly", "rows", "cols", "multiple", "selected", "colspan", "rowspan", "start",
		"charset", "http-equiv", "content" };
	int divs = 0;
	for (size_t i = 0; i < s.size(); i++)
		{
		if (s[i] != '<')
			continue;
		size_t j = i + 1;
		bool end = j < s.size() && s[j] == '/';
		if (end) j++;
		size_t n = j;
		while (n < s.size() && ((s[n] >= 'a' && s[n] <= 'z') || (s[n] >= '0' && s[n] <= '9'))) n++;
		std::string name = s.substr(j, n - j);
		if (!kTags.count(name)) { aWhy = "unknown tag <" + s.substr(i, 40); return false; }
		if (name == "div") divs += end ? -1 : 1;
		if (divs < 0) { aWhy = "a </div> too many at " + std::to_string(i); return false; }
		// attributes
		for (;;)
			{
			if (n >= s.size()) { aWhy = "unfinished tag " + name; return false; }
			if (s[n] == '>') break;
			if (s[n] != ' ') { aWhy = "junk in <" + name + ": " + s.substr(i, 60); return false; }
			n++;
			size_t a = n;
			while (n < s.size() && s[n] != '=' && s[n] != ' ' && s[n] != '>') n++;
			std::string attr = s.substr(a, n - a);
			if (end || !kAttrs.count(attr)) { aWhy = "attribute " + attr + " in <" + name; return false; }
			if (s[n] == '=')
				{
				if (s[n + 1] != '"') { aWhy = "unquoted value in <" + name; return false; }
				size_t q = s.find('"', n + 2);
				if (q == std::string::npos) { aWhy = "no closing quote"; return false; }
				std::string v = s.substr(n + 2, q - n - 2);
				if (v.find('<') != std::string::npos && attr != "alt" && attr != "content" && attr != "value")
					{ aWhy = "< in " + attr; return false; }
				n = q + 1;
				}
			}
		i = n;
		}
	return true;
	}

static void TestSimplifier()
	{
	// what goes, what stays
	std::string in =
		"<!DOCTYPE html>\n<html lang=\"en\" class=\"x\">\n  <head>\n    <meta charset=\"utf-8\">\n"
		"    <meta name=\"description\" content=\"long words\">\n    <title>  A  page  </title>\n"
		"    <link rel=\"stylesheet\" href=\"a.css\">\n    <style>body { color: red } </style>\n"
		"    <script>var x = '<div>'; if (a < b) {}</script>\n"
		"    <script type=\"application/ld+json\">{\"@type\": \"NewsArticle\"}</script>\n  </head>\n"
		"  <body onload=\"go()\" style=\"margin:0\">\n<!-- a comment <b>not bold</b> -->\n"
		"    <header class=\"top\"><nav><a href=\"/\" class=\"logo\" onclick=\"x()\">Home</a></nav></header>\n"
		"    <main><article><h1 id=\"t\" class=\"big\">Hello   world</h1>\n"
		"      <p>Some <span class=\"c\">text</span> &amp; <b>bold</b> a < b.</p>\n"
		"      <svg viewBox=\"0 0 1 1\"><path d=\"M0\"/><text>svg text</text></svg>\n"
		"      <noscript><img src=\"pixel.gif\">Turn JS on</noscript>\n"
		"      <iframe src=\"ad.html\">frame text</iframe>\n"
		"      <img src=\"data:image/gif;base64,R0lGOD\" data-src=\"real.jpg\" alt=\"A cat\" width=\"300\" height=\"200\" loading=\"lazy\">\n"
		"      <img src=\"logo.svg\" alt=\"Logo\">\n"
		"      <img src=\"t.gif\" width=\"1\" height=\"1\" alt=\"\">\n"
		"      <a href=\"javascript:void(0)\">js link</a> <a href=\"https://example.org/x?a=1&amp;b=2\">secure</a>\n"
		"      <div class=\"empty\"><div></div></div><div hidden><p>hidden text</p></div>\n"
		"      <div style=\"display: none\">invisible</div><div style=\"color:red\">visible</div>\n"
		"      <pre>  keep   this\n  spacing</pre>\n"
		"      <form action=\"/s\" method=\"get\" class=\"f\"><input type=\"text\" name=\"q\" placeholder=\"Search\" autocomplete=\"off\">"
		"<textarea name=\"t\" rows=\"2\"> raw <b>x</b> </textarea><select name=\"s\"><option value=\"1\" selected>One</option></select>"
		"<button type=\"submit\" class=\"btn\">Go</button></form>\n"
		"      <table class=\"t\"><tr><td colspan=\"2\" style=\"x\">cell</td><td></td></tr></table>\n"
		"    </article></main>\n  <footer><p>Footer</p></footer>\n<custom-thing>custom text</custom-thing>\n</body></html>\n";
	std::string out = Simplify(in);
	std::string why;
	CHECK(WellFormed(out, why));
	if (!why.empty()) printf("   %s\n", why.c_str());
	CHECK(!Has(out, "script") && !Has(out, "style") && !Has(out, "color") && !Has(out, "NewsArticle"));
	CHECK(!Has(out, "comment") && !Has(out, "not bold") && !Has(out, "svg text") && !Has(out, "Turn JS"));
	CHECK(!Has(out, "frame text") && !Has(out, "onclick") && !Has(out, "class=") && !Has(out, "a.css"));
	CHECK(!Has(out, "hidden text") && !Has(out, "invisible") && Has(out, "visible"));
	CHECK(Has(out, "<title> A page </title>") || Has(out, "<title>A page </title>") || Has(out, "<title> A page</title>"));
	CHECK(Has(out, "<meta charset=\"utf-8\">") && !Has(out, "description"));
	CHECK(Has(out, "<h1 id=\"t\">Hello world</h1>"));
	CHECK(Has(out, "<p>Some text &amp; <b>bold</b> a &lt; b.</p>"));
	CHECK(Has(out, "<img src=\"real.jpg\" alt=\"A cat\" width=\"300\" height=\"200\">"));   // lazy loading
	CHECK(Has(out, "[Logo]") && !Has(out, "logo.svg"));                                  // SVG: alt text
	CHECK(!Has(out, "t.gif"));                                                            // tracking pixel
	CHECK(Has(out, "<a>js link</a>") && Has(out, "<a href=\"https://example.org/x?a=1&amp;b=2\">secure</a>"));
	CHECK(Has(out, "<pre>  keep   this\n  spacing</pre>"));
	CHECK(Has(out, "<form action=\"/s\" method=\"get\"><input type=\"text\" name=\"q\">"));
	CHECK(Has(out, "<textarea name=\"t\" rows=\"2\"> raw &lt;b>x&lt;/b> </textarea>"));
	CHECK(Has(out, "<option value=\"1\" selected>One</option>") && Has(out, "<button type=\"submit\">Go</button>"));
	CHECK(Has(out, "<td colspan=\"2\">cell</td><td></td>"));                              // empty cells stay
	CHECK(Has(out, "<div><div><a href=\"/\">Home</a></div></div>"));                      // header, nav: divs
	CHECK(!Has(out, "<div></div>") && Has(out, "custom text") && !Has(out, "custom-thing"));
	CHECK(Has(out, "<p>Footer</p>"));
	CHECK(out.size() < in.size() / 2);
	printf("   synthetic page: %zu -> %zu bytes\n", in.size(), out.size());

	// the same result whatever the pieces it arrives in
	bool same = true;
	for (size_t step = 1; step < 40 && same; step++)
		same = Simplify(in, am::HtmlSimplifier::EKeepPictures, step) == out;
	CHECK(same);

	// text only: pictures become their alt text; nav, aside, footer go
	std::string t = Simplify(in, am::HtmlSimplifier::ETextOnly);
	CHECK(WellFormed(t, why) && !Has(t, "<img") && Has(t, "[A cat]") && !Has(t, "Footer") && !Has(t, "Home</a>"));

	// a redirect's address as <base>, first
	std::string b = Simplify("<p>x</p>", 1, 0, "https://www.example.com/a?b=\"c\"");
	CHECK(b.find("<base href=\"https://www.example.com/a?b=&quot;c&quot;\">") == 0);

	// garbage must not upset it
	std::string junk;
	for (int i = 0; i < 200000; i++) junk += (char)("<>/=\"' a-!\n\x00\xff"[(i * 7919) % 15]);
	std::string j = Simplify(junk);
	CHECK(j.size() < junk.size() * 2);
	// unclosed things at the end
	CHECK(Simplify("<p>a<script>never closed").find("never") == std::string::npos);
	CHECK(Simplify("<p>a<!-- open comment") == "<p>a");
	CHECK(Simplify("a <") == "a &lt;");
	// a raw-text end tag split up, or not really an end tag
	CHECK(Simplify("<title>a </tit b</title>") == "<title>a &lt;/tit b</title>");
	CHECK(Simplify("<script>x</scriptx> y</script ><p>z") == "<p>z");
	// form fields outside a form do nothing without scripts: dropped
	CHECK(Simplify("<button>hide</button><input type=checkbox>Menu<select><option>a</select>"
		"<form><input name=q><button>Go</button></form>") == "Menu<form><input name=\"q\"><button>Go</button></form>");
	// nested skips
	CHECK(Simplify("<svg><svg></svg>in</svg>out") == "out");
	CHECK(Simplify("<div hidden><div>a</div>b</div>c") == "c");
	}

// ===== real pages ================================================================

static std::string ReadFile(const std::string& p)
	{
	FILE* f = fopen(p.c_str(), "rb");
	if (!f) return "";
	std::string s;
	char b[65536];
	size_t n;
	while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
	fclose(f);
	return s;
	}

struct Page { const char* file; const char* must; double maxShare; };
static const Page kPages[] = {
	{ "bbc.html", "BBC", 0.15 },
	{ "guardian.html", "Guardian", 0.15 },
	{ "wiki.html", "Psion", 0.40 },
	{ "hn.html", "Hacker News", 0.60 },
};

static void TestPages()
	{
	const char* dir = getenv("PAGES");
	if (!dir || !*dir)
		{
		printf("   (no PAGES folder: real pages skipped)\n");
		return;
		}
	for (const Page& pg : kPages)
		{
		std::string in = ReadFile(std::string(dir) + "/" + pg.file);
		if (in.empty())
			{
			printf("   %s: missing\n", pg.file);
			continue;
			}
		std::string out = Simplify(in);
		std::string text = Simplify(in, am::HtmlSimplifier::ETextOnly);
		std::string why;
		bool ok = WellFormed(out, why);
		CHECK(ok);
		if (!ok) printf("   %s: %s\n", pg.file, why.c_str());
		CHECK(WellFormed(text, why));
		CHECK(Has(out, pg.must));
		CHECK(out.size() <= in.size() * pg.maxShare);
		CHECK(text.size() <= out.size());
		CHECK(!Has(out, "<script") && !Has(out, "<style") && !Has(out, "<svg"));
		// the same in the Atom's TCP-sized pieces
		CHECK(Simplify(in, am::HtmlSimplifier::EKeepPictures, 1460) == out);
		CHECK(Simplify(in, am::HtmlSimplifier::EKeepPictures, 333) == out);
		printf("   %-14s %8zu -> %7zu bytes (%4.1f%%), text only %7zu (%4.1f%%)\n", pg.file, in.size(),
			out.size(), 100.0 * out.size() / in.size(), text.size(), 100.0 * text.size() / in.size());
		}
	}

// ===== the proxy through the modem ===============================================

struct Rig
	{
	FakeHal hal;
	std::vector<uint8_t> ring;
	am::Modem* modem;
	size_t mark = 0;
	explicit Rig(size_t aRing = 96 * 1024) : ring(aRing)
		{
		modem = new am::Modem(hal, ring.data(), ring.size());
		modem->Begin();
		Run(10);
		}
	~Rig() { delete modem; }
	void Run(uint32_t aMs)
		{
		for (uint64_t t = 0; t < (uint64_t)aMs * 1000; t += 50)
			{
			modem->Loop();
			hal.Advance(50);
			}
		}
	void Type(const std::string& s) { for (char c : s) hal.rx.push_back((uint8_t)c); }
	void TypeRun(const std::string& s, uint32_t ms = 50) { Type(s); Run(ms); }
	std::string Got()
		{
		std::string w = hal.Wire();
		std::string s = w.substr(mark);
		mark = w.size();
		return s;
		}
	// dials psiproxy as PsiWeb does
	void Dial()
		{
		TypeRun("ATE0\r");
		TypeRun("ATDT psiproxy:8080\r", 100);
		Got();
		}
	};

struct Resp
	{
	int status = 0;
	std::map<std::string, std::string> h;   // lower-case names
	std::string body;                       // unpacked if the proxy gzipped it
	std::string wire;                       // as it came
	bool complete = false;
	};

// as much of a gzip stream as can be unpacked (aComplete: it ended properly)
static std::string Gunzip(const std::string& in, bool* aComplete = 0)
	{
	z_stream z = {};
	inflateInit2(&z, 31);
	std::string out;
	char buf[16384];
	z.next_in = (Bytef*)in.data();
	z.avail_in = (uInt)in.size();
	int r;
	do
		{
		z.next_out = (Bytef*)buf;
		z.avail_out = sizeof(buf);
		r = inflate(&z, Z_SYNC_FLUSH);
		out.append(buf, sizeof(buf) - z.avail_out);
		}
	while (r == Z_OK && (z.avail_in || !z.avail_out));
	if (aComplete) *aComplete = r == Z_STREAM_END;
	inflateEnd(&z);
	return out;
	}

static bool gUnpack = true;                 // Responses() gunzips what the proxy packed

// the HTTP responses in what the Psion received
static std::vector<Resp> Responses(const std::string& w)
	{
	std::vector<Resp> v;
	size_t p = 0;
	while (p < w.size())
		{
		size_t e = w.find("\r\n\r\n", p);
		if (e == std::string::npos) break;
		Resp r;
		std::string head = w.substr(p, e - p);
		r.status = atoi(head.c_str() + 9);
		size_t l = head.find("\r\n");
		while (l != std::string::npos)
			{
			size_t n = head.find("\r\n", l + 2);
			std::string line = head.substr(l + 2, (n == std::string::npos ? head.size() : n) - l - 2);
			size_t c = line.find(':');
			if (c != std::string::npos)
				{
				std::string k = line.substr(0, c);
				for (auto& ch : k) ch = (char)tolower(ch);
				std::string val = line.substr(c + 1);
				while (!val.empty() && val[0] == ' ') val.erase(0, 1);
				r.h[k] = r.h.count(k) ? r.h[k] + "\n" + val : val;
				}
			l = n;
			}
		p = e + 4;
		if (r.h["transfer-encoding"] == "chunked")
			{
			for (;;)
				{
				size_t le = w.find("\r\n", p);
				if (le == std::string::npos) return v;
				size_t n = strtoul(w.c_str() + p, 0, 16);
				p = le + 2;
				if (!n) { p += 2; r.complete = true; break; }
				if (p + n + 2 > w.size()) return v;
				r.body += w.substr(p, n);
				p += n + 2;
				}
			}
		else
			{
			size_t n = r.h.count("content-length") ? (size_t)atol(r.h["content-length"].c_str()) : 0;
			if (p + n > w.size()) return v;
			r.body = w.substr(p, n);
			p += n;
			r.complete = true;
			}
		r.wire = r.body;
		if (r.h["content-encoding"] == "gzip" && gUnpack)
			{
			bool done = false;
			r.body = Gunzip(r.wire, &done);
			if (!done) r.complete = false;
			}
		v.push_back(r);
		}
	return v;
	}

static std::string Reply(int code, const std::string& type, const std::string& body, const std::string& extra = "")
	{
	char h[300];
	snprintf(h, sizeof(h), "HTTP/1.1 %d X\r\nContent-Type: %s\r\nContent-Length: %zu\r\n", code, type.c_str(), body.size());
	return std::string(h) + extra + "\r\n" + body;
	}

static std::string Chunked(const std::string& body, size_t piece = 1000)
	{
	std::string s;
	for (size_t i = 0; i < body.size(); i += piece)
		{
		size_t n = std::min(piece, body.size() - i);
		char h[20];
		snprintf(h, sizeof(h), "%zx;ext=1\r\n", n);
		s += h + body.substr(i, n) + "\r\n";
		}
	return s + "0\r\nX-Trailer: 1\r\n\r\n";
	}

static std::string Gzip(const std::string& in, bool zlibWrap = false)
	{
	z_stream z = {};
	deflateInit2(&z, 9, Z_DEFLATED, zlibWrap ? 15 : 31, 8, Z_DEFAULT_STRATEGY);
	std::string out(deflateBound(&z, in.size()) + 64, 0);
	z.next_in = (Bytef*)in.data(); z.avail_in = (uInt)in.size();
	z.next_out = (Bytef*)&out[0]; z.avail_out = (uInt)out.size();
	deflate(&z, Z_FINISH);
	out.resize(z.total_out);
	deflateEnd(&z);
	return out;
	}

static const char* kReq = "GET http://example.com/ HTTP/1.1\r\nHost: example.com\r\nUser-Agent: Links (2.30; PsiWeb)\r\n"
	"Accept: */*\r\nAccept-Encoding: gzip, deflate\r\nProxy-Connection: keep-alive\r\n\r\n";

static void TestProxyBasics()
	{
	Rig r;
	std::vector<std::string> reqs;
	r.hal.responder = [&](const std::string& q, bool&) {
		reqs.push_back(q);
		return Reply(200, "text/html; charset=utf-8",
			"<html><head><script>x()</script></head><body><p class=a>Hello <b>there</b></p></body></html>");
	};
	r.TypeRun("ATE0\r");
	r.Got();
	r.TypeRun("ATDT psiproxy:8080\r", 100);
	CHECK(r.Got() == "\r\nCONNECT 115200\r\n");
	CHECK(r.modem->Connected() && r.modem->ProxyCall() && r.hal.upConnects == 0 && r.hal.tcpHost.empty());
	r.TypeRun(kReq, 300);
	std::vector<Resp> v = Responses(r.Got());
	CHECK(v.size() == 1 && v[0].status == 200 && v[0].complete);
	if (v.size() == 1)
		{
		CHECK(v[0].body == "<html><head></head><body><p>Hello <b>there</b></p></body></html>");
		CHECK(v[0].h["transfer-encoding"] == "chunked" && v[0].h["connection"] == "keep-alive");
		CHECK(v[0].h["content-type"] == "text/html; charset=utf-8");
		}
	CHECK(r.hal.upLog.size() == 1 && r.hal.upLog[0] == "example.com:80");
	CHECK(reqs.size() == 1);
	if (reqs.size() == 1)
		{
		const std::string& q = reqs[0];
		CHECK(q.find("GET / HTTP/1.1\r\nHost: example.com\r\n") == 0);
		CHECK(Has(q, "User-Agent: Links (2.30; PsiWeb)\r\n") && Has(q, "Accept-Encoding: identity\r\n"));
		CHECK(!Has(q, "Proxy-Connection") && !Has(q, "gzip") && Has(q, "Connection: keep-alive\r\n"));
		}
	// the next request on the same call and the same server connection
	r.TypeRun("GET http://example.com/two?x=1#frag HTTP/1.1\r\nHost: example.com\r\n\r\n", 300);
	v = Responses(r.Got());
	CHECK(v.size() == 1 && v[0].status == 200);
	CHECK(r.hal.upConnects == 1 && reqs.size() == 2 && reqs[1].find("GET /two?x=1 HTTP/1.1\r\n") == 0);
	// https:// goes over TLS from the modem; another host, another connection
	r.TypeRun("GET https://secure.example.org:8443/a HTTP/1.1\r\nHost: secure.example.org\r\n\r\n", 300);
	v = Responses(r.Got());
	CHECK(v.size() == 1 && r.hal.upLog.back() == "secure.example.org:8443 tls");
	CHECK(reqs.back().find("GET /a HTTP/1.1\r\nHost: secure.example.org:8443\r\n") == 0);
	// http://psiproxy/: the proxy's own page
	r.TypeRun("GET http://psiproxy/ HTTP/1.1\r\nHost: psiproxy\r\n\r\n", 300);
	v = Responses(r.Got());
	CHECK(v.size() == 1 && v[0].status == 200 && Has(v[0].body, "Requests: 4") && r.hal.upConnects == 2);
	// hang up as psiglue does; the next dial is an ordinary TCP one again
	r.Run(1100); r.TypeRun("+++", 1100); r.TypeRun("ATH\r", 300);
	CHECK(!r.modem->Connected() && !r.modem->ProxyCall() && !r.hal.tcpOpen);
	r.Got();
	r.TypeRun("ATDT bbs.example.org:23\r", 100);
	CHECK(Has(r.Got(), "CONNECT") && r.hal.tcpHost == "bbs.example.org" && r.hal.tcpPort == 23 && !r.modem->ProxyCall());
	}

static void TestProxyRedirects()
	{
	Rig r;
	std::vector<std::string> reqs;
	r.hal.responder = [&](const std::string& q, bool&) {
		reqs.push_back(q);
		if (q.find("GET /old ") == 0 && !r.hal.upTls)
			return Reply(301, "text/html", "<p>moved</p>", "Location: https://example.com/new/page?a=1\r\n");
		if (q.find("GET /new/page?a=1 ") == 0)
			return Reply(302, "text/html", "", "Location: ../final\r\nSet-Cookie: a=1\r\n");
		if (q.find("GET /final ") == 0)
			return Reply(200, "text/html", "<a href=\"x\">rel</a>", "Set-Cookie: b=2\r\n");
		return Reply(200, "text/html", "<p>other " + q.substr(0, q.find(' ', 4)) + "</p>");
	};
	r.Dial();
	r.TypeRun("GET http://example.com/old HTTP/1.1\r\nHost: example.com\r\n\r\n", 500);
	std::vector<Resp> v = Responses(r.Got());
	CHECK(v.size() == 1 && v[0].status == 200);
	if (getenv("DEBUG")) for (auto& x : v) printf("[%s] cookie [%s]\n", x.body.c_str(), x.h["set-cookie"].c_str());
	if (v.size() == 1)
		{
		CHECK(v[0].body == "<base href=\"https://example.com/final\">\n<a href=\"x\">rel</a>");
		CHECK(v[0].h["set-cookie"] == "b=2");
		}
	CHECK(r.hal.upLog.size() == 3 && r.hal.upLog[1] == "example.com:443 tls");
	// http://example.com moved to https: the next http:// request goes there at once
	r.TypeRun("GET http://example.com/again HTTP/1.1\r\nHost: example.com\r\n\r\n", 300);
	v = Responses(r.Got());
	CHECK(v.size() == 1 && Has(v[0].body, "other GET /again"));
	CHECK(r.hal.upLog.back() == "example.com:443 tls");
	// too many redirects: the last one goes to the Psion, Location and all
	r.hal.responder = [&](const std::string& q, bool&) {
		return Reply(302, "text/html", "<p>loop</p>", "Location: /loop" + std::to_string(reqs.size()) + "\r\n");
	};
	r.TypeRun("GET http://loop.example.com/ HTTP/1.1\r\nHost: loop.example.com\r\n\r\n", 500);
	v = Responses(r.Got());
	CHECK(v.size() == 1 && v[0].status == 302 && Has(v[0].h["location"], "/loop") && Has(v[0].body, "<p>loop</p>"));
	// a form sent with POST: its body goes on; 303 is fetched with GET
	reqs.clear();
	r.hal.responder = [&](const std::string& q, bool&) {
		reqs.push_back(q);
		if (q.find("POST ") == 0)
			return Reply(303, "text/html", "", "Location: /done\r\n");
		return Reply(200, "text/html", "<p>thanks</p>");
	};
	r.TypeRun("POST http://form.example.com/send HTTP/1.1\r\nHost: form.example.com\r\n"
		"Content-Type: application/x-www-form-urlencoded\r\nContent-Length: 7\r\n\r\nq=hello", 500);
	v = Responses(r.Got());
	if (getenv("DEBUG")) for (auto& x : v) printf("[%d %s]\n", x.status, x.body.c_str());
	CHECK(v.size() == 1 && v[0].body == "<base href=\"http://form.example.com/done\">\n<p>thanks</p>");
	CHECK(reqs.size() == 2 && Has(reqs[0], "Content-Length: 7\r\n") && reqs[0].substr(reqs[0].size() - 7) == "q=hello");
	CHECK(reqs.size() == 2 && reqs[1].find("GET /done HTTP/1.1") == 0 && !Has(reqs[1], "Content-Length"));
	}

static void TestProxyBodies()
	{
	Rig r;
	std::string page = "<html><body>";
	for (int i = 0; i < 400; i++)
		page += "<div class=\"story\"><h3><a href=\"/s" + std::to_string(i) + "\" data-x=\"y\">Story " + std::to_string(i)
			+ "</a></h3><script>track(" + std::to_string(i) + ")</script></div>\n";
	page += "</body></html>";
	std::string expect = Simplify(page);
	std::string png(70000, 0);
	for (size_t i = 0; i < png.size(); i++) png[i] = (char)(i * 131 % 256);
	r.hal.responder = [&](const std::string& q, bool& close) {
		if (Has(q, "GET /chunked "))
			return std::string("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nTransfer-Encoding: chunked\r\n\r\n") + Chunked(page, 777);
		if (Has(q, "GET /gzip "))
			return std::string("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Encoding: gzip\r\nTransfer-Encoding: chunked\r\n\r\n")
				+ Chunked(Gzip(page), 500);
		if (Has(q, "GET /deflate "))
			return Reply(200, "text/html", Gzip(page, true), "Content-Encoding: deflate\r\n");
		if (Has(q, "GET /png "))
			return Reply(200, "image/png", png);
		if (Has(q, "GET /close "))
			{
			close = true;
			return std::string("HTTP/1.0 200 OK\r\nContent-Type: image/gif\r\n\r\nGIF89a-to-the-close");
			}
		if (Has(q, "HEAD /"))
			return std::string("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 1234\r\n\r\n");
		return Reply(404, "text/html", "<p>no</p>");
	};
	r.hal.readChunk = 600;                   // the server's data in small pieces
	r.TypeRun("AT$PR=0\r");                  // the line rate: quicker to simulate
	r.Dial();
	const char* paths[] = { "/chunked", "/gzip", "/deflate" };
	for (const char* p : paths)
		{
		r.TypeRun(std::string("GET http://example.com") + p + " HTTP/1.1\r\nHost: example.com\r\n\r\n", 12000);
		std::vector<Resp> v = Responses(r.Got());
		bool found = false;
		for (auto& x : v)
			if (x.status == 200)
				{
				found = true;
				CHECK(x.complete && x.body == expect);
				CHECK(x.h["content-encoding"].empty());
				}
		CHECK(found);
		if (!found) printf("   %s: no 200\n", p);
		}
	// a picture passes through untouched, with its length
	r.TypeRun("GET http://example.com/png HTTP/1.1\r\nHost: example.com\r\n\r\n", 15000);
	std::string pw = r.Got();
	std::vector<Resp> v = Responses(pw);
	if (getenv("DEBUG")) printf("png: %zu responses, wire %zu, head %s\n", v.size(), pw.size(), pw.substr(0, 200).c_str());
	CHECK(v.size() == 1 && v[0].body == png && v[0].h["content-length"] == "70000" && v[0].h["transfer-encoding"].empty());
	// a body that runs to the server's close: chunked to the Psion
	r.TypeRun("GET http://example.com/close HTTP/1.1\r\nHost: example.com\r\n\r\n", 500);
	v = Responses(r.Got());
	CHECK(v.size() == 1 && v[0].body == "GIF89a-to-the-close" && v[0].h["transfer-encoding"] == "chunked");
	// HEAD: no body, whatever the length says
	r.TypeRun("HEAD http://example.com/ HTTP/1.1\r\nHost: example.com\r\n\r\n", 500);
	std::string g = r.Got();
	CHECK(Has(g, "HTTP/1.1 200") && g.substr(g.size() - 4) == "\r\n\r\n");
	// the server cannot be reached: a page that says so, and the call stays up
	r.hal.tcpOk = false;
	r.TypeRun("GET http://nowhere.example/ HTTP/1.1\r\nHost: nowhere.example\r\n\r\n", 500);
	v = Responses(r.Got());
	CHECK(v.size() == 1 && v[0].status == 502 && Has(v[0].body, "could not be reached") && r.modem->Connected());
	r.hal.tcpOk = true;
	// Connection: close from the Psion: the answer, then NO CARRIER
	r.TypeRun("GET http://example.com/zz HTTP/1.1\r\nHost: example.com\r\nProxy-Connection: close\r\n\r\n", 800);
	g = r.Got();
	CHECK(Has(g, "HTTP/1.1 404") && Has(g, "Connection: close") && Has(g, "\r\nNO CARRIER\r\n"));
	CHECK(!r.modem->Connected() && !r.modem->ProxyCall());
	}

// a big file and a small ring: the server is read only as the Psion takes it
static void TestProxyBackPressure()
	{
	Rig r(16 * 1024);
	std::string big(150000, 0);
	for (size_t i = 0; i < big.size(); i++) big[i] = (char)(i * 7 % 253);
	r.hal.responder = [&](const std::string&, bool&) { return Reply(200, "application/octet-stream", big); };
	r.Dial();
	r.TypeRun("GET http://example.com/big HTTP/1.1\r\nHost: example.com\r\n\r\n", 1000);
	CHECK(r.modem->Buffered() <= 16 * 1024);
	CHECK(r.hal.server.size() > 100000);      // most of it still waits at the server
	size_t peak = 0;
	for (int i = 0; i < 300; i++) { r.Run(100); peak = std::max(peak, r.modem->Buffered()); }
	CHECK(peak <= 16 * 1024);
	std::vector<Resp> v = Responses(r.Got());
	CHECK(v.size() == 1 && v[0].body == big);
	}

static void TestProxySettings()
	{
	Rig r;
	r.TypeRun("ATE0\r"); r.Got();
	r.TypeRun("AT$PX?\r");
	CHECK(Has(r.Got(), "1 (on: simplified pages)"));
	r.TypeRun("ATI\r");
	CHECK(Has(r.Got(), "Web proxy: ATDT psiproxy:8080"));
	r.TypeRun("AT$PX=5\r");
	CHECK(Has(r.Got(), "ERROR"));
	r.TypeRun("AT$PX=4\r"); r.TypeRun("AT$PX?\r");
	CHECK(Has(r.Got(), "4 (on: reader)"));
	r.TypeRun("AT$PX=1\r"); r.Got();
	// off: psiproxy is just a name to dial
	r.TypeRun("AT$PX=0\r"); r.Got();
	r.TypeRun("ATDT psiproxy:8080\r", 100);
	CHECK(Has(r.Got(), "CONNECT") && !r.modem->ProxyCall() && r.hal.tcpHost == "psiproxy");
	r.Run(1100); r.TypeRun("+++", 1100); r.TypeRun("ATH\r", 300); r.Got();
	r.TypeRun("AT$PX=2&W\r");                // (AT$ ends the line: &W is part of the value)
	CHECK(Has(r.Got(), "ERROR"));
	r.TypeRun("AT$PX=2\r"); r.TypeRun("AT&W\r"); r.Got();
	CHECK(r.hal.saved.proxy == 2);
	// a record saved by 1.0 (zero where the proxy byte now is) means on
	am::Settings old = r.hal.saved;
	old.proxy = 0;
	CHECK(am::ProxyMode(old) == am::Proxy::EOn);
	// text only
	r.hal.responder = [&](const std::string&, bool&) {
		return Reply(200, "text/html", "<nav><a href=/>menu</a></nav><p>words <img src=a.jpg alt=pic></p>"); };
	r.TypeRun("ATDT psiproxy:8080\r", 100); r.Got();
	r.TypeRun(kReq, 300);
	std::vector<Resp> v = Responses(r.Got());
	CHECK(v.size() == 1 && v[0].body == "<p>words [pic]</p>");
	r.Run(1100); r.TypeRun("+++", 1100); r.TypeRun("ATH\r", 300); r.Got();
	// unchanged pages: HTML as it came, and the Psion's gzip asked for
	r.TypeRun("AT$PX=3\r"); r.Got();
	std::string asked;
	r.hal.responder = [&](const std::string& q, bool&) {
		asked = q;
		return Reply(200, "text/html", Gzip("<p class=x>raw</p>"), "Content-Encoding: gzip\r\n"); };
	r.TypeRun("ATDT psiproxy\r", 100); r.Got();   // (any port, or none)
	r.TypeRun(kReq, 300);
	v = Responses(r.Got());
	CHECK(v.size() == 1 && v[0].h["content-encoding"] == "gzip" && v[0].wire == Gzip("<p class=x>raw</p>"));
	CHECK(Has(asked, "Accept-Encoding: gzip, deflate\r\n"));
	}

// a real page, gzipped and chunked as a CDN sends it, through the whole modem
static void TestProxyRealPage()
	{
	const char* dir = getenv("PAGES");
	if (!dir || !*dir)
		return;
	std::string page = ReadFile(std::string(dir) + "/bbc.html");
	if (page.empty())
		return;
	Rig r;
	r.hal.readChunk = 1460;
	r.hal.responder = [&](const std::string&, bool&) {
		return std::string("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Encoding: gzip\r\n"
			"Transfer-Encoding: chunked\r\n\r\n") + Chunked(Gzip(page), 8000);
	};
	r.Dial();
	// as Links asks: gzip accepted (then without, for comparison)
	for (int zip = 1; zip >= 0; zip--)
		{
		uint32_t before = r.modem->ToPsion();
		r.TypeRun(std::string("GET https://www.bbc.co.uk/ HTTP/1.1\r\nHost: www.bbc.co.uk\r\n")
			+ (zip ? "Accept-Encoding: gzip, deflate\r\n" : "") + "\r\n", 100);
		uint64_t start = r.hal.nowUs;
		std::vector<Resp> v;
		for (int i = 0; i < 400 && (v.empty() || !v[0].complete); i++)
			{
			r.Run(100);
			v = Responses(r.hal.Wire().substr(r.mark));
			}
		double secs = (r.hal.nowUs - start) / 1e6;
		CHECK(v.size() == 1 && v[0].complete && v[0].body == Simplify(page));
		printf("   bbc.co.uk through the proxy: %zu bytes from the server (%zu unpacked), %u to the Psion%s, %.1f s at 5500 bytes/s\n",
			Gzip(page).size(), page.size(), (unsigned)(r.modem->ToPsion() - before), zip ? " (gzip)" : " (plain)", secs);
		r.Got();
		}
	}

// the simplified page gzipped on the line when PsiWeb accepts it
static void TestProxyGzip()
	{
	Rig r;
	std::string page = "<html><body>";
	for (int i = 0; i < 300; i++)
		page += "<div class=\"story\"><h3><a href=\"/news/s" + std::to_string(i) + "\">Story number " + std::to_string(i)
			+ " of the day</a></h3><p>Some words about story " + std::to_string(i) + ".</p></div>\n";
	page += "</body></html>";
	std::string expect = Simplify(page);
	size_t half = page.size() / 2;
	bool slow = false;
	r.hal.responder = [&](const std::string&, bool&) {
		std::string h = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " + std::to_string(page.size()) + "\r\n\r\n";
		return slow ? h + page.substr(0, half) : h + page;
	};
	r.TypeRun("AT$PR=0\r");
	r.Dial();
	// Links' request (Accept-Encoding: gzip, deflate): packed
	r.TypeRun(kReq, 3000);
	std::vector<Resp> v = Responses(r.Got());
	CHECK(v.size() == 1 && v[0].complete && v[0].body == expect && v[0].h["content-encoding"] == "gzip");
	if (v.size() == 1)
		{
		printf("   gzip on the line: %zu bytes of HTML in %zu\n", v[0].body.size(), v[0].wire.size());
		CHECK(v[0].wire.size() < expect.size() / 3);
		}
	// no Accept-Encoding: plain
	r.TypeRun("GET http://example.com/ HTTP/1.1\r\nHost: example.com\r\n\r\n", 3000);
	v = Responses(r.Got());
	CHECK(v.size() == 1 && v[0].body == expect && v[0].h["content-encoding"].empty());
	// a slow server: what has come is sent, and can be unpacked, while the rest is awaited
	slow = true;
	r.TypeRun(kReq, 3000);
	std::string partial = r.Got();
	size_t e = partial.find("\r\n\r\n");
	std::string body;
	for (size_t p = e + 4; p < partial.size(); )
		{
		size_t le = partial.find("\r\n", p);
		size_t n = strtoul(partial.c_str() + p, 0, 16);
		if (le == std::string::npos || !n) break;
		body += partial.substr(le + 2, n);
		p = le + 2 + n + 2;
		}
	std::string sofar = Gunzip(body);
	CHECK(sofar.size() > expect.size() / 3 && expect.compare(0, sofar.size(), sofar) == 0);
	printf("   while the server waits: %zu bytes of the page already readable\n", sofar.size());
	std::string rest = page.substr(half);
	for (char c : rest) r.hal.server.push_back((uint8_t)c);
	r.Run(3000);
	v = Responses(partial + r.Got());
	CHECK(v.size() == 1 && v[0].complete && v[0].body == expect);
	// AT$PZ=0: never packed
	r.Run(1100); r.TypeRun("+++", 1100); r.TypeRun("ATH\r", 300);
	r.TypeRun("AT$PZ=0\r");
	r.TypeRun("AT$PZ?\r");
	CHECK(Has(r.Got(), "\r\n0\r\n"));
	slow = false;
	r.Dial();
	r.TypeRun(kReq, 3000);
	v = Responses(r.Got());
	CHECK(v.size() == 1 && v[0].body == expect && v[0].h["content-encoding"].empty());
	}

// ===== regressions from review =====================================================

// a page with long-distance repeats (back-references across the whole 32 KB
// window, and past its wrap) for the inflater
static std::string FarRepeats(size_t aLen)
	{
	std::string s = "<html><body>";
	std::vector<std::string> blocks;
	uint32_t x = 12345;
	for (int b = 0; b < 12; b++)
		{
		std::string blk = "<p>";
		for (int i = 0; i < 700; i++)
			{
			x = x * 1103515245 + 12345;
			blk += (char)('a' + (x >> 16) % 26);
			if ((x >> 8) % 7 == 0) blk += ' ';
			}
		blocks.push_back(blk + "</p>\n");
		}
	for (size_t i = 0; s.size() < aLen; i++)
		{
		x = x * 1103515245 + 12345;
		s += blocks[(x >> 16) % blocks.size()];       // (about 25 KB back, often)
		s += "<p>part " + std::to_string(i) + "</p>\n";
		}
	return s + "</body></html>";
	}

// Responses() over everything received so far, for up to aMs of simulated
// time, until aWant complete responses have come
static std::vector<Resp> WaitResponses(Rig& r, size_t aWant, uint32_t aMs)
	{
	std::vector<Resp> v;
	for (uint32_t t = 0; t < aMs; t += 100)
		{
		r.Run(100);
		v = Responses(r.hal.Wire().substr(r.mark));
		size_t done = 0;
		for (auto& x : v) done += x.complete;
		if (done >= aWant) break;
		}
	r.Got();
	return v;
	}

static void TestProxyRegressions()
	{
	// 1. a body with Content-Length and more bytes after it in the same read:
	// the body ends at its length (the proxy used to spin for ever)
	{
	Rig r;
	r.hal.responder = [&](const std::string& q, bool&) {
		if (Has(q, "GET /a "))
			return std::string("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 5\r\n\r\nhello\r\n");
		return Reply(200, "text/plain", "abc");
	};
	r.Dial();
	r.TypeRun("GET http://example.com/a HTTP/1.1\r\nHost: example.com\r\n\r\n", 1000);
	std::vector<Resp> v = Responses(r.Got());
	CHECK(v.size() == 1 && v[0].complete && v[0].body == "hello");
	// the extra bytes were not this body's: a fresh connection for the next
	r.TypeRun("GET http://example.com/b HTTP/1.1\r\nHost: example.com\r\n\r\n", 1000);
	v = Responses(r.Got());
	CHECK(v.size() == 1 && v[0].body == "abc" && r.hal.upConnects == 2);
	}
	// 4. a file passed through with its length that ends short: the Psion
	// still waits for the rest, so the call ends (not the next answer taken
	// as the rest of this one)
	{
	Rig r;
	r.hal.responder = [&](const std::string&, bool& close) {
		close = true;
		return std::string("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: 100\r\n\r\nshort");
	};
	r.Dial();
	r.TypeRun("GET http://example.com/a.png HTTP/1.1\r\nHost: example.com\r\n\r\n", 1000);
	std::string g = r.Got();
	CHECK(Has(g, "Content-Length: 100\r\n") && Has(g, "short") && Has(g, "\r\nNO CARRIER\r\n"));
	CHECK(!r.modem->Connected() && !r.modem->ProxyCall());
	}
	// 3. the simplifier makes up to 4 bytes of each one ("<" is "&lt;"): with
	// a small ring nothing may be lost, packed or not, gzip on the line or not
	for (int kind = 0; kind < 4; kind++)
		{
		Rig r(16 * 1024);
		std::string body(60000, '<');
		std::string g = Gzip(body);
		r.hal.readChunk = 1460;
		r.hal.responder = [&](const std::string&, bool&) {
			if (kind & 1)
				return Reply(200, "text/html", g, "Content-Encoding: gzip\r\n");
			return Reply(200, "text/html", body);
		};
		r.TypeRun("AT$PR=0\r");
		r.Dial();
		r.TypeRun(std::string("GET http://example.com/ HTTP/1.1\r\nHost: example.com\r\n")
			+ ((kind & 2) ? "Accept-Encoding: gzip\r\n" : "") + "\r\n", 100);
		std::vector<Resp> v = WaitResponses(r, 1, 120000);
		CHECK(v.size() == 1 && v[0].complete && v[0].body == Simplify(body) && v[0].body.size() == 240000);
		CHECK(!r.hal.Logged("overflowed"));
		if (v.size() == 1 && v[0].body.size() != 240000)
			printf("   kind %d: %zu bytes of 240000\n", kind, v[0].body.size());
		}
	// 2. gzip and deflate with back-references across the whole window, read
	// in small pieces (the Atom's tinfl: test_proxy_tinfl)
	{
	Rig r(32 * 1024);
	std::string page = FarRepeats(150000);
	std::string expect = Simplify(page);
	r.hal.readChunk = 500;
	r.hal.responder = [&](const std::string& q, bool&) {
		if (Has(q, "GET /deflate "))
			return Reply(200, "text/html", Gzip(page, true), "Content-Encoding: deflate\r\n");
		return std::string("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Encoding: gzip\r\nTransfer-Encoding: chunked\r\n\r\n")
			+ Chunked(Gzip(page), 3000);
	};
	r.TypeRun("AT$PR=0\r");
	r.Dial();
	for (const char* path : { "/gzip", "/deflate" })
		{
		r.TypeRun(std::string("GET http://example.com") + path + " HTTP/1.1\r\nHost: example.com\r\nAccept-Encoding: gzip\r\n\r\n", 100);
		std::vector<Resp> v = WaitResponses(r, 1, 120000);
		CHECK(v.size() == 1 && v[0].complete && v[0].body == expect);
		CHECK(v.size() != 1 || !Has(v[0].body, "damaged"));
		}
	CHECK(!r.hal.Logged("overflowed"));
	}
	// Content-Length and chunked both: the length is ignored
	{
	Rig r;
	std::string page = "<p>" + std::string(3000, 'x') + "</p>";
	r.hal.responder = [&](const std::string&, bool&) {
		return std::string("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 10\r\nTransfer-Encoding: chunked\r\n\r\n")
			+ Chunked(page, 700);
	};
	r.Dial();
	r.TypeRun("GET http://example.com/ HTTP/1.1\r\nHost: example.com\r\n\r\n", 1000);
	std::vector<Resp> v = Responses(r.Got());
	CHECK(v.size() == 1 && v[0].complete && v[0].body == page);
	// and the connection is kept: it ended cleanly
	r.TypeRun("GET http://example.com/ HTTP/1.1\r\nHost: example.com\r\n\r\n", 1000);
	v = Responses(r.Got());
	CHECK(v.size() == 1 && v[0].body == page && r.hal.upConnects == 1);
	// a file passed through: not sent on with the wrong length
	r.hal.responder = [&](const std::string&, bool&) {
		return std::string("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 10\r\nTransfer-Encoding: chunked\r\n\r\n")
			+ Chunked(page, 700);
	};
	r.TypeRun("GET http://example.com/t HTTP/1.1\r\nHost: example.com\r\n\r\n", 1000);
	v = Responses(r.Got());
	CHECK(v.size() == 1 && v[0].complete && v[0].body == page && v[0].h["content-length"].empty());
	}
	// a response head without end: 502 after 16 KB, and the call goes on
	{
	Rig r;
	r.hal.responder = [&](const std::string& q, bool&) {
		if (Has(q, "GET /ok "))
			return Reply(200, "text/plain", "fine");
		std::string h = "HTTP/1.1 200 OK\r\n";
		for (int i = 0; i < 40; i++)
			h += "X-Junk-" + std::to_string(i) + ": " + std::string(900, 'j') + "\r\n";
		return h;
	};
	r.Dial();
	r.TypeRun("GET http://example.com/big HTTP/1.1\r\nHost: example.com\r\n\r\n", 1000);
	std::vector<Resp> v = Responses(r.Got());
	CHECK(v.size() == 1 && v[0].status == 502 && Has(v[0].body, "too many headers"));
	r.TypeRun("GET http://example.com/ok HTTP/1.1\r\nHost: example.com\r\n\r\n", 1000);
	v = Responses(r.Got());
	CHECK(v.size() == 1 && v[0].status == 200 && v[0].body == "fine");
	}
	// requests from the Psion while the proxy is busy, more than it can hold:
	// the modem leaves them in the UART (it used to drop what did not fit)
	{
	Rig r(16 * 1024);
	std::string big(60000, 'b');
	std::vector<std::string> reqs;
	r.hal.responder = [&](const std::string& q, bool&) {
		reqs.push_back(q);
		if (Has(q, "GET /big "))
			return Reply(200, "application/octet-stream", big);
		return Reply(200, "text/plain", q.substr(4, q.find(' ', 4) - 4));
	};
	r.Dial();
	std::string all = "GET http://example.com/big HTTP/1.1\r\nHost: example.com\r\n\r\n";
	for (int i = 0; i < 3; i++)
		all += "GET http://example.com/n" + std::to_string(i) + " HTTP/1.1\r\nHost: example.com\r\nCookie: "
			+ std::string(1800, (char)('p' + i)) + "\r\n\r\n";
	r.Type(all);
	std::vector<Resp> v = WaitResponses(r, 4, 60000);
	CHECK(v.size() == 4);
	if (v.size() == 4)
		{
		CHECK(v[0].body == big && v[1].body == "/n0" && v[2].body == "/n1" && v[3].body == "/n2");
		}
	CHECK(reqs.size() == 4 && Has(reqs.back(), std::string(1800, 'r').c_str()));
	if (getenv("DEBUG")) for (auto& q : reqs) printf("[%zu %.60s]\n", q.size(), q.c_str());
	CHECK(r.modem->Connected());
	}
	}

static void TestSimplifierRegressions()
	{
	// a hidden element whose end tag never comes: it ends with the element
	// it is in, or after kMaxSkip bytes; the rest of the page stays
	std::string o = Simplify("<div><div hidden>secret<p>x</div><p>after</p></div><p>end</p>");
	CHECK(!Has(o, "secret") && Has(o, "after") && Has(o, "end"));
	CHECK(Simplify("<div hidden><div>a</div>b</div>c") == "c");
	CHECK(Simplify("<ul><li>one<svg><g><path d=\"x\"/></g></li><li>two</li></ul>") == "<ul><li>one</li><li>two</li></ul>");
	o = Simplify("<svg>" + std::string(60000, 'a') + "<p>tail</p>");
	CHECK(Has(o, "tail") && o.size() < 60000 - am::HtmlSimplifier::kMaxSkip + 100);
	// the same in pieces
	CHECK(Simplify("<div><div hidden>secret<p>x</div><p>after</p></div>", 1, 7) == Simplify("<div><div hidden>secret<p>x</div><p>after</p></div>"));
	}

#include <signal.h>
#include <unistd.h>
static void Hung(int) { printf("FAIL: a test hung (more than 120 s)\n"); fflush(stdout); _exit(1); }

// ===== 2.0: reader mode and pictures ===========================================

static void TestReader()
	{
	const int R = am::HtmlSimplifier::EReader;
	std::string page =
		"<html><head><title>News</title></head><body>"
		"<header><a href=\"/\">Logo</a><ul><li><a href=\"/a\">Home</a></li><li><a href=\"/b\">Sport</a></li></ul></header>"
		"<div role=\"navigation\"><a href=\"/x\">Section one</a> <a href=\"/y\">Section two</a></div>"
		"<div class=\"site-sidebar\"><p>Sidebar promo text</p></div>"
		"<div id=\"cookie-banner\"><p>We use cookies</p></div>"
		"<main><h1>The headline</h1><p>The story, with <a href=\"/more\">a link</a> in it.</p>"
		"<ul><li><a href=\"/1\">One</a></li><li><a href=\"/2\">Two</a></li><li><a href=\"/3\">Three</a></li><li><a href=\"/4\">Four</a></li></ul>"
		"<ul><li>First point about the story</li><li>Second point, see <a href=\"/ref\">the reference</a></li><li>Third point</li></ul>"
		"<ol><li>Step one of the recipe</li><li>Step two</li></ol>"
		"<img src=\"pic.jpg\" alt=\"A picture\">"
		"<div class=\"unavailable-notice\">Still shown</div>"
		"</main>"
		"<aside><p>Related stories</p></aside>"
		"<div class=\"comments\"><p>Comment text</p></div>"
		"<footer><p>Footer text</p></footer>"
		"<p>Trailing text after main</p>"
		"</body></html>";
	std::string out = Simplify(page, R);
	std::string why;
	CHECK(WellFormed(out, why));
	if (!why.empty()) printf("   reader: %s\n", why.c_str());
	CHECK(Has(out, "<title>News</title>"));
	CHECK(Has(out, "The headline") && Has(out, "The story, with <a href=\"/more\">a link</a> in it."));
	CHECK(!Has(out, "Logo") && !Has(out, "Sport") && !Has(out, "Section one"));
	CHECK(!Has(out, "Sidebar promo") && !Has(out, "cookies"));
	CHECK(!Has(out, "href=\"/1\"") && !Has(out, "Three"));              // the menu (all links)
	CHECK(Has(out, "First point") && Has(out, "the reference") && Has(out, "Third point"));   // a real list
	CHECK(Has(out, "Step two"));
	CHECK(Has(out, "[A picture]") && !Has(out, "<img"));
	CHECK(Has(out, "Still shown"));                                       // "unavailable" is not "nav"
	CHECK(!Has(out, "Related") && !Has(out, "Comment text") && !Has(out, "Footer text") && !Has(out, "Trailing text"));
	// the same output whatever the pieces
	CHECK(Simplify(page, R, 1) == out && Simplify(page, R, 7) == out && Simplify(page, R, 300) == out);
	// mode 2 keeps what reader drops (apart from nav, aside and footer)
	std::string lite = Simplify(page, am::HtmlSimplifier::ETextOnly);
	CHECK(Has(lite, "Logo") && Has(lite, "Section one") && Has(lite, "Trailing text") && !Has(lite, "Footer text"));
	// role="main" on a div: its end is found through the nesting of divs
	std::string p2 = "<body><div class=\"menu\"><a href=\"/\">M</a></div><div role=\"main\"><div><p>Inner</p></div><p>Outer</p></div><p>After</p></body>";
	std::string o2 = Simplify(p2, R);
	CHECK(Has(o2, "Inner") && Has(o2, "Outer") && !Has(o2, "After") && !Has(o2, ">M<"));
	// no main at all: the page is kept, furniture dropped
	std::string p3 = "<body><div class=\"nav-bar\">N</div><p>Body text</p><div class=\"share-tools\">S</div></body>";
	std::string o3 = Simplify(p3, R);
	CHECK(Has(o3, "Body text") && !Has(o3, ">N<") && !Has(o3, ">S<"));
	// a list that is too big to judge is kept
	std::string big = "<body><ul>";
	for (int i = 0; i < 200; i++) big += "<li><a href=\"/l" + std::to_string(i) + "\">Link number " + std::to_string(i) + "</a></li>";
	big += "</ul><p>End</p></body>";
	std::string ob = Simplify(big, R);
	CHECK(Has(ob, "Link number 150") && Has(ob, "End"));
	// a list never closed is kept
	CHECK(Has(Simplify("<body><ul><li><a href=\"/a\">A</a></li><li><a href=\"/b\">B</a></li><li><a href=\"/c\">C</a>", R), "href=\"/c\""));
	// garbage
	std::string g;
	for (int i = 0; i < 5000; i++) g += (char)("<>/\"= abcmainulrole"[i * 7 % 20]);
	Simplify(g, R);
	CHECK(true);
	}

static std::string File(const std::string& aPath)
	{
	FILE* f = fopen(aPath.c_str(), "rb");
	if (!f) return "";
	std::string s;
	char b[4096];
	size_t n;
	while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
	fclose(f);
	return s;
	}

// a GIF's size, mean brightness and number of greys, through Pillow (-1: no Pillow)
static bool GifStats(const std::string& aGif, int& w, int& h, int& mean, int& greys)
	{
	FILE* f = fopen("/tmp/atom-test-picture.gif", "wb");
	if (!f) return false;
	fwrite(aGif.data(), 1, aGif.size(), f);
	fclose(f);
	FILE* p = popen("python3 -c \"import sys\nfrom PIL import Image\nim=Image.open('/tmp/atom-test-picture.gif').convert('L')\n"
		"d=list(im.getdata())\nprint(im.width, im.height, sum(d)//len(d), len(set(d)))\" 2>/dev/null", "r");
	if (!p) return false;
	char line[100] = "";
	bool ok = fgets(line, sizeof(line), p) != 0;
	pclose(p);
	return ok && sscanf(line, "%d %d %d %d", &w, &h, &mean, &greys) == 4;
	}

static void TestPictures()
	{
	std::string jpg = File("pictures/photo.jpg"), prog = File("pictures/photo_prog.jpg"), png = File("pictures/photo.png");
	std::string gif = File("pictures/photo.gif"), small = File("pictures/small.gif"), big = File("pictures/big.gif");
	std::string meanText = File("pictures/mean.txt");
	if (jpg.empty() || png.empty() || gif.empty() || small.empty() || big.empty() || meanText.empty())
		{
		printf("   (no pictures/ folder: run make pictures; pictures skipped)\n");
		return;
		}
	int mean0 = atoi(meanText.c_str());
	printf("   photo.jpg %zu, photo.png %zu, photo.gif %zu, small.gif %zu bytes; mean %d\n", jpg.size(), png.size(),
		gif.size(), small.size(), mean0);
	Rig r;
	r.hal.responder = [&](const std::string& q, bool& close) {
		if (Has(q, "GET /photo.jpg ")) return Reply(200, "image/jpeg", jpg, "Cache-Control: max-age=60\r\n");
		if (Has(q, "GET /photo_prog.jpg ")) return Reply(200, "image/jpeg", prog);
		if (Has(q, "GET /photo.png ")) return Reply(200, "image/png", png);
		if (Has(q, "GET /photo.gif ")) return Reply(200, "image/gif", gif);
		if (Has(q, "GET /big.gif ")) return Reply(200, "image/gif", big);
		if (Has(q, "GET /small.gif ")) return Reply(200, "image/gif", small);
		if (Has(q, "GET /chunked.jpg "))
			return std::string("HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nTransfer-Encoding: chunked\r\n\r\n") + Chunked(jpg, 1000);
		if (Has(q, "GET /bogus.jpg ")) return Reply(200, "image/jpeg", std::string(3000, 'x'));
		if (Has(q, "GET /page ")) return Reply(200, "text/html", "<p>a page</p>");
		if (Has(q, "GET /close.jpg ")) { close = true; return std::string("HTTP/1.0 200 OK\r\nContent-Type: image/jpeg\r\n\r\n") + jpg; }
		return Reply(404, "text/html", "<p>no</p>");
	};
	r.TypeRun("AT$PR=0\r"); r.TypeRun("AT$PI=1\r"); r.TypeRun("AT$PW=200\r"); r.TypeRun("AT$PM=96\r");
	r.Dial();
	auto fetch = [&](const char* path, uint32_t ms = 6000) {
		r.TypeRun(std::string("GET http://pics.example.com") + path + " HTTP/1.1\r\nHost: pics.example.com\r\n\r\n", ms);
		std::vector<Resp> v = Responses(r.Got());
		return v.empty() ? Resp() : v.back();
	};
	// a JPEG: a GIF at most 200 wide, 16 greys, about as bright as the original
	Resp x = fetch("/photo.jpg");
	CHECK(x.status == 200 && x.complete && x.h["content-type"] == "image/gif" && x.body.compare(0, 6, "GIF89a") == 0);
	// the GIF goes with a Content-Length, not chunked: PsiWeb's Links fetch mishandles a chunked image body
	CHECK(x.h["cache-control"] == "max-age=60" && x.h["content-length"] == std::to_string(x.body.size()) && x.h["transfer-encoding"].empty());
	int w = 0, h = 0, mean = 0, greys = 0;
	bool stats = GifStats(x.body, w, h, mean, greys);
	if (!stats)
		printf("   (no Pillow: the GIFs are not decoded back)\n");
	else
		{
		printf("   photo.jpg -> %zu-byte gif %dx%d, mean %d, %d greys\n", x.body.size(), w, h, mean, greys);
		CHECK(w == 200 && h == 125 && greys <= 16 && greys >= 8 && abs(mean - mean0) <= 24);
		}
	CHECK(x.body.size() < jpg.size());
	CHECK(r.modem->WebProxy().Pictures() == 1);
	CHECK(r.hal.Logged("800x500 -> 200x125 gif"));
	// progressive, PNG and GIF sources (whole-number shrinks: 384/2 for the GIF)
	// (a progressive JPEG keeps its coefficients until the last scan, within
	// 96 KB: a photo this size is decoded at 1/8 from the DC terms alone)
	struct { const char* path; int w, h; } more[] = { { "/photo_prog.jpg", 100, 63 }, { "/photo.png", 200, 125 }, { "/photo.gif", 192, 120 }, { 0, 0, 0 } };
	for (int i = 0; more[i].path; i++)
		{
		x = fetch(more[i].path, 8000);
		CHECK(x.status == 200 && x.complete && x.h["content-type"] == "image/gif");
		if (stats && GifStats(x.body, w, h, mean, greys))
			{
			printf("   %s -> %dx%d, mean %d, %d greys\n", more[i].path, w, h, mean, greys);
			CHECK(w == more[i].w && h == more[i].h && greys <= 16 && abs(mean - mean0) <= 24);
			}
		}
	// a photo-sized GIF needs more memory than the Atom has for it: passed through
	x = fetch("/big.gif", 8000);
	CHECK(x.status == 200 && x.complete && x.h["content-type"] == "image/gif" && x.body == big);
	CHECK(r.hal.Logged("not converted (too big)"));
	// a small GIF is converted too (cheaper for Links), at its own size
	x = fetch("/small.gif");
	CHECK(x.status == 200 && x.h["content-type"] == "image/gif" && x.body.compare(0, 6, "GIF89a") == 0);
	CHECK((unsigned char)x.body[6] == 80 && (unsigned char)x.body[8] == 50);
	// chunked from the server (no length): still converted
	x = fetch("/chunked.jpg");
	CHECK(x.status == 200 && x.complete && x.h["content-type"] == "image/gif");
	// a body that runs to the close
	x = fetch("/close.jpg");
	CHECK(x.status == 200 && x.complete && x.h["content-type"] == "image/gif");
	// not a picture after all: passed through as it came
	x = fetch("/bogus.jpg");
	CHECK(x.status == 200 && x.complete && x.h["content-type"] == "image/jpeg" && x.body == std::string(3000, 'x'));
	CHECK(r.hal.Logged("not converted"));
	// a page in between is a page
	x = fetch("/page");
	CHECK(x.status == 200 && Has(x.body, "a page"));
	// too big for the limit: passed through, with its length (the picture
	// settings, like AT$PX, are taken at the dial: hang up and dial again)
	r.Run(1100); r.TypeRun("+++", 1100); r.Got();
	r.TypeRun("AT$PM=16\r"); r.TypeRun("ATH\r", 100); r.Got();
	r.Dial();
	x = fetch("/photo.jpg", 8000);
	CHECK(x.status == 200 && x.complete && x.h["content-type"] == "image/jpeg" && x.body == jpg && x.h["content-length"] == std::to_string(jpg.size()));
	// chunked and over the limit: passed through once the limit is hit
	x = fetch("/chunked.jpg", 8000);
	CHECK(x.status == 200 && x.complete && x.h["content-type"] == "image/jpeg" && x.body == jpg);
	// pictures off: passed through
	r.Run(1100); r.TypeRun("+++", 1100); r.Got();
	r.TypeRun("AT$PM=96\r"); r.TypeRun("AT$PI=0\r");
	r.TypeRun("ATH\r", 100); r.Got();
	r.Dial();
	x = fetch("/photo.jpg", 8000);
	CHECK(x.status == 200 && x.h["content-type"] == "image/jpeg" && x.body == jpg);
	// a small ring: the GIF goes out as room comes, nothing lost
	{
	Rig s(20 * 1024);
	s.hal.responder = r.hal.responder;
	s.TypeRun("AT$PI=1\r"); s.TypeRun("AT$PW=320\r");
	s.Dial();
	s.TypeRun("GET http://pics.example.com/photo.png HTTP/1.1\r\nHost: pics.example.com\r\n\r\n", 30000);
	std::vector<Resp> v = Responses(s.Got());
	CHECK(!v.empty() && v.back().status == 200 && v.back().complete && v.back().h["content-type"] == "image/gif");
	if (!v.empty() && stats && GifStats(v.back().body, w, h, mean, greys))
		CHECK(w <= 320 && w >= 160 && greys <= 16);           // (whole-number shrinks: 800/3)
	}
	// the GIF writer on its own: a known picture round-trips exactly
	{
	StrSink sink;
	am::GifWriter g;
	const int W = 37, H = 11;
	CHECK(g.Begin(&sink, W, H));
	uint8_t row[(W + 1) / 2];
	for (int y = 0; y < H; y++)
		{
		for (int xx = 0; xx < W; xx++)
			{
			uint8_t v = (uint8_t)((xx * 3 + y * 5 + (xx * y) % 7) & 15);
			if (xx & 1) row[xx >> 1] = (uint8_t)((row[xx >> 1] & 0x0f) | (v << 4)); else row[xx >> 1] = v;
			}
		g.Row(row);
		}
	g.End();
	CHECK(sink.s.compare(0, 6, "GIF89a") == 0 && sink.s.back() == 0x3b);
	if (stats && GifStats(sink.s, w, h, mean, greys))
		{
		CHECK(w == W && h == H && greys == 16);
		// every pixel back as it went
		FILE* p = popen("python3 -c \"from PIL import Image\nim=Image.open('/tmp/atom-test-picture.gif').convert('L')\n"
			"d=list(im.getdata())\nprint(' '.join(str(v//17) for v in d))\" 2>/dev/null", "r");
		std::string got;
		char b[4096];
		while (p && fgets(b, sizeof(b), p)) got += b;
		if (p) pclose(p);
		std::string want;
		for (int y = 0; y < H; y++)
			for (int xx = 0; xx < W; xx++)
				want += std::to_string((xx * 3 + y * 5 + (xx * y) % 7) & 15) + " ";
		CHECK(got.compare(0, want.size() - 1, want, 0, want.size() - 1) == 0);
		}
	// a long run of one grey and a dictionary that fills: still valid
	StrSink sink2;
	am::GifWriter g2;
	CHECK(g2.Begin(&sink2, 640, 240));
	uint8_t row2[320];
	for (int y = 0; y < 240; y++)
		{
		for (int i = 0; i < 320; i++) row2[i] = (uint8_t)(y < 120 ? 0x77 : ((i * 13 + y * 7) & 0xff));
		g2.Row(row2);
		}
	g2.End();
	if (stats && GifStats(sink2.s, w, h, mean, greys))
		CHECK(w == 640 && h == 240);
	printf("   640x240 half flat, half noise: %zu bytes\n", sink2.s.size());
	}
	}

int main()
	{
	signal(SIGALRM, Hung);
	alarm(120);
	printf("simplifier\n");  TestSimplifier(); TestSimplifierRegressions();
	printf("pages\n");       TestPages();
	printf("proxy\n");       TestProxyBasics();
	printf("redirects\n");   TestProxyRedirects();
	printf("bodies\n");      TestProxyBodies();
	printf("pressure\n");    TestProxyBackPressure();
	printf("settings\n");    TestProxySettings();
	printf("gzip\n");        TestProxyGzip();
	printf("real page\n");   TestProxyRealPage();
	printf("regressions\n"); TestProxyRegressions();
	alarm(300);
	printf("reader\n");      TestReader();
	printf("pictures\n");    TestPictures();
	printf("%d checks, %d failed\n", gChecks, gFails);
	return gFails ? 1 : 0;
	}
