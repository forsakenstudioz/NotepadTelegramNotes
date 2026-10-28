// Telegram Notes - a Notepad++ plugin that talks to your own Telegram bot.
//
//  * Send the current note / selection / a todo list from Notepad++ to your phone.
//  * While Notepad++ is open, the bot answers your phone: /list, /get, /todo, /grocery ...
//  * Anything you type to the bot is kept in an inbox you can open in Notepad++.
//
// Only messages from the chat id in the settings are ever answered.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <algorithm>
#include "NppPlugin.h"

static const wchar_t PLUGIN_NAME[] = L"Telegram Notes";
static const wchar_t INBOX_FILE[] = L"_inbox_from_phone.txt";

static HINSTANCE g_hInst = nullptr;
static NppData g_npp = {};
static HWND g_msgWnd = nullptr;
static HANDLE g_pollThread = nullptr;
static HANDLE g_stopEvent = nullptr;
static CRITICAL_SECTION g_lock;      // guards g_pollRequest
static CRITICAL_SECTION g_fileLock;  // guards inbox and list files
static HINTERNET g_pollRequest = nullptr;
static volatile LONG g_stopping = 0;
static volatile LONG g_pausePoll = 0;
static std::wstring g_iniPath, g_statePath;

#define WM_TN_AUTOFETCH (WM_APP + 1)
#define WM_TN_STATUS (WM_APP + 2)

static void CmdConnect();

// ============================================================================
// String helpers
// ============================================================================
static std::string W2U(const std::wstring &w) {
	if (w.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

static std::wstring U2W(const std::string &s, UINT cp = CP_UTF8) {
	if (s.empty()) return {};
	int n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), &w[0], n);
	return w;
}

static std::string FromAnsi(const std::string &s) { return W2U(U2W(s, CP_ACP)); }

static std::string ToAnsi(const std::string &u) {
	std::wstring w = U2W(u);
	if (w.empty()) return {};
	int n = WideCharToMultiByte(CP_ACP, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_ACP, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

// Bytes of unknown encoding (file on disk) -> UTF-8.
static std::string AnyToUtf8(std::string b) {
	if (b.size() >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF)
		b.erase(0, 3);
	if (b.size() >= 2 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xFE)
		return W2U(std::wstring((const wchar_t *)(b.data() + 2), (b.size() - 2) / 2));
	if (b.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, b.data(), (int)b.size(), nullptr, 0) > 0)
		return b;
	return FromAnsi(b);
}

static int Utf16Len(const std::string &u) {
	return u.empty() ? 0 : MultiByteToWideChar(CP_UTF8, 0, u.data(), (int)u.size(), nullptr, 0);
}

static std::string Trim(const std::string &s) {
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos) return {};
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

static std::string Lower(std::string s) {
	for (auto &c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
	return s;
}

static std::wstring LowerW(std::wstring s) {
	if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
	return s;
}

static bool StartsWith(const std::string &s, const char *p) { return s.compare(0, strlen(p), p) == 0; }

static std::string HtmlEsc(const std::string &s) {
	std::string o;
	o.reserve(s.size() + 16);
	for (char c : s) {
		if (c == '&') o += "&amp;";
		else if (c == '<') o += "&lt;";
		else if (c == '>') o += "&gt;";
		else o += c;
	}
	return o;
}

static std::string JsonStr(const std::string &s) {
	std::string o = "\"";
	for (unsigned char c : s) {
		switch (c) {
		case '"': o += "\\\""; break;
		case '\\': o += "\\\\"; break;
		case '\n': o += "\\n"; break;
		case '\r': o += "\\r"; break;
		case '\t': o += "\\t"; break;
		default:
			if (c < 0x20) {
				char buf[8];
				wsprintfA(buf, "\\u%04x", c);
				o += buf;
			} else o += (char)c;
		}
	}
	return o + "\"";
}

static std::string RemoveCR(const std::string &s) {
	std::string o;
	o.reserve(s.size());
	for (char c : s) if (c != '\r') o += c;
	return o;
}

static std::vector<std::string> SplitLines(const std::string &s) {
	std::vector<std::string> v;
	size_t start = 0;
	for (;;) {
		size_t p = s.find('\n', start);
		std::string line = s.substr(start, p == std::string::npos ? std::string::npos : p - start);
		if (!line.empty() && line.back() == '\r') line.pop_back();
		v.push_back(line);
		if (p == std::string::npos) break;
		start = p + 1;
	}
	return v;
}

static std::string FormatTime(const SYSTEMTIME &st) {
	static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	char buf[64];
	wsprintfA(buf, "%d %s %d, %02d:%02d", st.wDay, months[(st.wMonth + 11) % 12], st.wYear, st.wHour, st.wMinute);
	return buf;
}

static std::string UnixToLocal(long long t) {
	ULARGE_INTEGER u;
	u.QuadPart = (ULONGLONG)(t + 11644473600LL) * 10000000ULL;
	FILETIME ft = {u.LowPart, u.HighPart}, lft;
	SYSTEMTIME st;
	FileTimeToLocalFileTime(&ft, &lft);
	FileTimeToSystemTime(&lft, &st);
	return FormatTime(st);
}

static std::string NowLocal() {
	SYSTEMTIME st;
	GetLocalTime(&st);
	return FormatTime(st);
}

// ============================================================================
// Tiny JSON parser (enough for Telegram replies)
// ============================================================================
struct JVal {
	enum Type { Null, Bool, Num, Str, Arr, Obj } t = Null;
	bool b = false;
	std::string s;  // string value, or the raw text of a number (keeps 64-bit ids exact)
	std::vector<JVal> a;
	std::vector<std::pair<std::string, JVal>> o;

	const JVal &operator[](const char *k) const {
		static const JVal none;
		for (auto &kv : o) if (kv.first == k) return kv.second;
		return none;
	}
	long long i64() const { return t == Num ? _atoi64(s.c_str()) : 0; }
};

struct JParser {
	const char *p, *end;
	void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p; }
	static void putUtf8(std::string &o, unsigned cp) {
		if (cp < 0x80) o += (char)cp;
		else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
		else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
		else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
	}
	bool hex4(unsigned &v) {
		if (end - p < 4) return false;
		v = 0;
		for (int i = 0; i < 4; ++i) {
			char c = *p++;
			v <<= 4;
			if (c >= '0' && c <= '9') v |= c - '0';
			else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
			else return false;
		}
		return true;
	}
	bool str(std::string &o) {
		if (p >= end || *p != '"') return false;
		++p;
		while (p < end && *p != '"') {
			if (*p == '\\') {
				if (++p >= end) return false;
				char c = *p++;
				switch (c) {
				case 'n': o += '\n'; break;
				case 't': o += '\t'; break;
				case 'r': o += '\r'; break;
				case 'b': o += '\b'; break;
				case 'f': o += '\f'; break;
				case 'u': {
					unsigned cp;
					if (!hex4(cp)) return false;
					if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
						p += 2;
						unsigned lo;
						if (!hex4(lo)) return false;
						cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
					}
					putUtf8(o, cp);
					break;
				}
				default: o += c;
				}
			} else o += *p++;
		}
		if (p >= end) return false;
		++p;
		return true;
	}
	bool val(JVal &v, int depth = 0) {
		if (depth > 64) return false;
		ws();
		if (p >= end) return false;
		char c = *p;
		if (c == '{') {
			v.t = JVal::Obj;
			++p;
			ws();
			if (p < end && *p == '}') { ++p; return true; }
			for (;;) {
				ws();
				std::string k;
				if (!str(k)) return false;
				ws();
				if (p >= end || *p != ':') return false;
				++p;
				v.o.emplace_back(k, JVal());
				if (!val(v.o.back().second, depth + 1)) return false;
				ws();
				if (p < end && *p == ',') { ++p; continue; }
				if (p < end && *p == '}') { ++p; return true; }
				return false;
			}
		}
		if (c == '[') {
			v.t = JVal::Arr;
			++p;
			ws();
			if (p < end && *p == ']') { ++p; return true; }
			for (;;) {
				v.a.emplace_back();
				if (!val(v.a.back(), depth + 1)) return false;
				ws();
				if (p < end && *p == ',') { ++p; continue; }
				if (p < end && *p == ']') { ++p; return true; }
				return false;
			}
		}
		if (c == '"') { v.t = JVal::Str; return str(v.s); }
		if (c == 't' && end - p >= 4 && !strncmp(p, "true", 4)) { v.t = JVal::Bool; v.b = true; p += 4; return true; }
		if (c == 'f' && end - p >= 5 && !strncmp(p, "false", 5)) { v.t = JVal::Bool; p += 5; return true; }
		if (c == 'n' && end - p >= 4 && !strncmp(p, "null", 4)) { p += 4; return true; }
		const char *s = p;
		while (p < end && (strchr("+-.eE", *p) || (*p >= '0' && *p <= '9'))) ++p;
		if (p == s) return false;
		v.t = JVal::Num;
		v.s.assign(s, p);
		return true;
	}
};

static bool ParseJson(const std::string &text, JVal &out) {
	JParser jp{text.data(), text.data() + text.size()};
	return jp.val(out);
}

// ============================================================================
// Files
// ============================================================================
static bool FileExists(const std::wstring &p) {
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool ReadFileBytes(const std::wstring &path, std::string &out, size_t maxBytes = 64 * 1024 * 1024) {
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size;
	bool ok = GetFileSizeEx(h, &size) && (ULONGLONG)size.QuadPart <= maxBytes;
	if (ok) {
		out.resize((size_t)size.QuadPart);
		DWORD got = 0;
		ok = out.empty() || (ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr) && got == out.size());
	}
	CloseHandle(h);
	return ok;
}

static bool WriteFileBytes(const std::wstring &path, const std::string &data, bool append = false) {
	HANDLE h = CreateFileW(path.c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, nullptr,
	                       append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD done = 0;
	bool ok = data.empty() || (WriteFile(h, data.data(), (DWORD)data.size(), &done, nullptr) && done == data.size());
	CloseHandle(h);
	return ok;
}

static std::wstring FileNameOf(const std::wstring &path) {
	size_t p = path.find_last_of(L"\\/");
	return p == std::wstring::npos ? path : path.substr(p + 1);
}

static std::wstring ExtOf(const std::wstring &name) {
	size_t p = name.find_last_of(L'.');
	return p == std::wstring::npos ? L"" : LowerW(name.substr(p + 1));
}

// ============================================================================
// Settings
// ============================================================================
struct Config {
	std::string token, chatId;
	std::wstring notesFolder;
	bool autoOpen = false;
	int longChars = 3800;
	bool ready() const { return !token.empty() && !chatId.empty(); }
};

static const wchar_t INI_TEMPLATE[] =
	L"; =====================================================================\r\n"
	L";  Telegram Notes for Notepad++  -  settings\r\n"
	L"; =====================================================================\r\n"
	L";  STEP 1: Paste the token you got from @BotFather after  BotToken=\r\n"
	L";          It looks like:  BotToken=7123456789:AAHk3xQ...long text...\r\n"
	L";  STEP 2: Save this file (Ctrl+S).\r\n"
	L";  STEP 3: On your phone, open your bot in Telegram and press START.\r\n"
	L";  STEP 4: In Notepad++ click  Plugins > Telegram Notes > Connect my phone\r\n"
	L";          (ChatId below is then filled in for you).\r\n"
	L";\r\n"
	L";  Keep the token secret: anyone who has it can control your bot.\r\n"
	L"; =====================================================================\r\n"
	L"\r\n"
	L"[Telegram]\r\n"
	L"BotToken=\r\n"
	L"ChatId=\r\n"
	L"\r\n"
	L"[Options]\r\n"
	L"; Folder for todo.txt, grocery.txt and any other notes your phone can ask for.\r\n"
	L"; Leave empty to use  Documents\\Telegram Notes\r\n"
	L"NotesFolder=\r\n"
	L"\r\n"
	L"; 1 = notes typed on your phone open in a new tab by themselves\r\n"
	L"; 0 = they wait until you click \"Fetch notes from phone\"\r\n"
	L"AutoOpenPhoneNotes=0\r\n"
	L"\r\n"
	L"; Notes longer than this many characters are sent as a file instead of a message\r\n"
	L"LongNoteChars=3800\r\n";

static void EnsureIni() {
	if (FileExists(g_iniPath)) return;
	CreateDirectoryW(g_iniPath.substr(0, g_iniPath.find_last_of(L'\\')).c_str(), nullptr);
	// UTF-16 with BOM so the Windows INI functions handle non-English folder names.
	std::string bytes = "\xFF\xFE";
	bytes.append((const char *)INI_TEMPLATE, (sizeof(INI_TEMPLATE) / sizeof(wchar_t) - 1) * sizeof(wchar_t));
	WriteFileBytes(g_iniPath, bytes);
}

static std::wstring IniGet(const std::wstring &file, const wchar_t *sec, const wchar_t *key, const wchar_t *def = L"") {
	wchar_t buf[2048];
	GetPrivateProfileStringW(sec, key, def, buf, 2048, file.c_str());
	return buf;
}

static std::wstring DefaultNotesFolder() {
	std::wstring dir;
	PWSTR docs = nullptr;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) dir = docs;
	if (docs) CoTaskMemFree(docs);
	if (dir.empty()) {
		wchar_t buf[MAX_PATH];
		GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
		dir = buf;
	}
	return dir + L"\\Telegram Notes";
}

static Config LoadConfig() {
	Config c;
	c.token = Trim(W2U(IniGet(g_iniPath, L"Telegram", L"BotToken")));
	c.chatId = Trim(W2U(IniGet(g_iniPath, L"Telegram", L"ChatId")));
	// People often paste the token with quotes or the word "bot" in front.
	if (c.token.size() >= 2 && c.token.front() == '"' && c.token.back() == '"') c.token = c.token.substr(1, c.token.size() - 2);
	if (StartsWith(Lower(c.token), "bot") && c.token.find(':') > 3) c.token.erase(0, 3);
	c.notesFolder = IniGet(g_iniPath, L"Options", L"NotesFolder");
	while (!c.notesFolder.empty() && (c.notesFolder.back() == L'\\' || c.notesFolder.back() == L' ')) c.notesFolder.pop_back();
	if (c.notesFolder.empty()) c.notesFolder = DefaultNotesFolder();
	c.autoOpen = _wtoi(IniGet(g_iniPath, L"Options", L"AutoOpenPhoneNotes", L"0").c_str()) != 0;
	c.longChars = std::min(4000, std::max(200, _wtoi(IniGet(g_iniPath, L"Options", L"LongNoteChars", L"3800").c_str())));
	SHCreateDirectoryExW(nullptr, c.notesFolder.c_str(), nullptr);
	return c;
}

static long long LoadOffset() { return _wtoi64(IniGet(g_statePath, L"State", L"NextUpdateId", L"0").c_str()); }

static void SaveOffset(long long v) {
	WritePrivateProfileStringW(L"State", L"NextUpdateId", std::to_wstring(v).c_str(), g_statePath.c_str());
}

// ============================================================================
// HTTPS + Telegram Bot API
// ============================================================================
static bool HttpPost(const std::wstring &path, const std::string &body, const std::wstring &contentType,
                     std::string &response, DWORD &status, std::string &err, DWORD timeoutMs, bool cancellable) {
	status = 0;
	HINTERNET ses = WinHttpOpen(L"NppTelegramNotes/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!ses) ses = WinHttpOpen(L"NppTelegramNotes/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!ses) { err = "Windows could not start an internet connection."; return false; }
	WinHttpSetTimeouts(ses, 15000, 15000, 30000, (int)timeoutMs);
	HINTERNET con = WinHttpConnect(ses, L"api.telegram.org", INTERNET_DEFAULT_HTTPS_PORT, 0);
	HINTERNET req = con ? WinHttpOpenRequest(con, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
	bool ok = req != nullptr;
	if (ok && cancellable) {
		EnterCriticalSection(&g_lock);
		if (g_stopping || g_pausePoll) ok = false;
		else g_pollRequest = req;
		LeaveCriticalSection(&g_lock);
		if (!ok) { WinHttpCloseHandle(req); req = nullptr; err = "cancelled"; }
	}
	if (ok) {
		std::wstring hdr = L"Content-Type: " + contentType + L"\r\n";
		ok = WinHttpSendRequest(req, hdr.c_str(), (DWORD)-1L, (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) &&
		     WinHttpReceiveResponse(req, nullptr);
		if (ok) {
			DWORD len = sizeof(status);
			WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
			for (;;) {
				DWORD avail = 0, got = 0;
				if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
				size_t old = response.size();
				response.resize(old + avail);
				if (!WinHttpReadData(req, &response[old], avail, &got)) { ok = false; break; }
				response.resize(old + got);
			}
		}
		if (!ok) {
			DWORD e = GetLastError();
			if (e == ERROR_WINHTTP_TIMEOUT) err = "Telegram did not answer in time (timeout).";
			else if (e == ERROR_WINHTTP_NAME_NOT_RESOLVED || e == ERROR_WINHTTP_CANNOT_CONNECT)
				err = "Could not reach api.telegram.org. Check your internet connection (or firewall/proxy).";
			else if (e == ERROR_WINHTTP_OPERATION_CANCELLED || e == ERROR_INVALID_HANDLE) err = "cancelled";
			else err = "Network error " + std::to_string(e) + ".";
		}
	}
	bool closeReq = req != nullptr;
	if (cancellable && req) {
		EnterCriticalSection(&g_lock);
		if (g_pollRequest == req) g_pollRequest = nullptr;
		else closeReq = false;  // CancelPoll() already closed it
		LeaveCriticalSection(&g_lock);
	}
	if (closeReq) WinHttpCloseHandle(req);
	if (con) WinHttpCloseHandle(con);
	WinHttpCloseHandle(ses);
	return ok;
}

static void CancelPoll() {
	EnterCriticalSection(&g_lock);
	if (g_pollRequest) {
		WinHttpCloseHandle(g_pollRequest);
		g_pollRequest = nullptr;
	}
	LeaveCriticalSection(&g_lock);
}

static bool TgRaw(const std::string &token, const char *method, const std::string &body, const std::wstring &contentType,
                  JVal &result, std::string &err, DWORD timeoutMs = 30000, bool cancellable = false) {
	std::string resp;
	DWORD status = 0;
	std::wstring path = L"/bot" + U2W(token) + L"/" + U2W(method);
	if (!HttpPost(path, body, contentType, resp, status, err, timeoutMs, cancellable)) return false;
	JVal j;
	if (!ParseJson(resp, j)) {
		err = "Unexpected answer from Telegram (HTTP " + std::to_string(status) + ").";
		return false;
	}
	if (!j["ok"].b) {
		std::string d = j["description"].s;
		if (status == 401 || status == 404)
			err = "Telegram does not recognise this bot token. Copy it again from @BotFather (the whole thing, e.g. 7123456789:AAH...).";
		else if (status == 409)
			err = "409:" + d;
		else
			err = d.empty() ? "Telegram error " + std::to_string(status) : d;
		return false;
	}
	result = j["result"];
	return true;
}

static bool Tg(const std::string &token, const char *method, const std::string &json, JVal &result, std::string &err,
               DWORD timeoutMs = 30000, bool cancellable = false) {
	return TgRaw(token, method, json, L"application/json", result, err, timeoutMs, cancellable);
}

static bool TgSendHtml(const Config &c, const std::string &html, std::string &err) {
	JVal r;
	std::string body = "{\"chat_id\":" + JsonStr(c.chatId) + ",\"text\":" + JsonStr(html) +
	                   ",\"parse_mode\":\"HTML\",\"link_preview_options\":{\"is_disabled\":true}}";
	return Tg(c.token, "sendMessage", body, r, err);
}

static bool TgSendDocument(const Config &c, std::string fileName, const std::string &data, const std::string &captionHtml, std::string &err) {
	std::string boundary = "----NppTelegramNotes" + std::to_string(GetTickCount64());
	for (auto &ch : fileName) if (ch == '"' || ch == '\r' || ch == '\n') ch = '_';
	std::string b;
	auto field = [&](const char *name, const std::string &value) {
		b += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"\r\n\r\n" + value + "\r\n";
	};
	field("chat_id", c.chatId);
	if (!captionHtml.empty()) {
		field("caption", captionHtml);
		field("parse_mode", "HTML");
	}
	b += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"document\"; filename=\"" + fileName +
	     "\"\r\nContent-Type: application/octet-stream\r\n\r\n";
	b += data;
	b += "\r\n--" + boundary + "--\r\n";
	JVal r;
	return TgRaw(c.token, "sendDocument", b, U2W("multipart/form-data; boundary=" + boundary), r, err, 120000);
}

static const char *BOT_COMMANDS_JSON =
	"{\"commands\":["
	"{\"command\":\"list\",\"description\":\"Show your notes\"},"
	"{\"command\":\"get\",\"description\":\"Send me a note: /get name or /get 2\"},"
	"{\"command\":\"todo\",\"description\":\"Show todo list, or add: /todo buy milk\"},"
	"{\"command\":\"done\",\"description\":\"Tick a todo item: /done 2\"},"
	"{\"command\":\"grocery\",\"description\":\"Show grocery list, or add: /grocery eggs, bread\"},"
	"{\"command\":\"got\",\"description\":\"Tick a grocery item: /got 1\"},"
	"{\"command\":\"clear\",\"description\":\"Remove ticked items: /clear todo or /clear grocery\"},"
	"{\"command\":\"ping\",\"description\":\"Check that Notepad++ is online\"},"
	"{\"command\":\"help\",\"description\":\"How to use this bot\"}]}";

// ============================================================================
// Building what gets sent
// ============================================================================
struct Outgoing {
	std::string title;     // shown in bold above the note
	std::string text;      // UTF-8
	std::string fileName;  // used when sent as a file
	std::string lang;      // Telegram code-block language, "" = none
	bool isCode = false;
	bool todo = false;
	bool forceFile = false;
	std::string fileBytes;  // raw bytes when forceFile and not text
};

static std::string LangFromExt(const std::wstring &ext) {
	static const struct { const wchar_t *ext; const char *lang; } map[] = {
		{L"c", "c"}, {L"h", "cpp"}, {L"cpp", "cpp"}, {L"cc", "cpp"}, {L"hpp", "cpp"}, {L"cs", "csharp"},
		{L"py", "python"}, {L"js", "javascript"}, {L"mjs", "javascript"}, {L"ts", "typescript"}, {L"tsx", "tsx"},
		{L"jsx", "jsx"}, {L"java", "java"}, {L"kt", "kotlin"}, {L"swift", "swift"}, {L"go", "go"}, {L"rs", "rust"},
		{L"rb", "ruby"}, {L"php", "php"}, {L"html", "html"}, {L"htm", "html"}, {L"xml", "xml"}, {L"css", "css"},
		{L"scss", "scss"}, {L"json", "json"}, {L"yml", "yaml"}, {L"yaml", "yaml"}, {L"sql", "sql"}, {L"sh", "bash"},
		{L"bat", "batch"}, {L"cmd", "batch"}, {L"ps1", "powershell"}, {L"lua", "lua"}, {L"ini", "ini"}, {L"md", "markdown"},
		{L"vb", "vbnet"}, {L"pl", "perl"}, {L"r", "r"}, {L"dart", "dart"}, {L"toml", "toml"},
	};
	for (auto &m : map) if (ext == m.ext) return m.lang;
	return {};
}

static bool IsCodeExt(const std::wstring &ext) { return !ext.empty() && ext != L"md" && ext != L"ini" && !LangFromExt(ext).empty(); }

static std::string StripPrefix(const std::string &s, bool &done, bool &marked) {
	static const char *doneMarks[] = {"- [x] ", "* [x] ", "- [X] ", "* [X] ", "[x] ", "[X] ", "\xE2\x98\x91 ", "\xE2\x9C\x94 ", "\xE2\x9C\x85 "};
	static const char *openMarks[] = {"- [ ] ", "* [ ] ", "[ ] ", "[] ", "\xE2\x98\x90 ", "- ", "* ", "+ ", "\xE2\x80\xA2 "};
	done = marked = false;
	for (auto m : doneMarks) if (StartsWith(s, m)) { done = marked = true; return s.substr(strlen(m)); }
	for (auto m : openMarks) if (StartsWith(s, m)) { marked = true; return s.substr(strlen(m)); }
	size_t i = 0;
	while (i < s.size() && isdigit((unsigned char)s[i])) ++i;
	if (i > 0 && i + 1 < s.size() && (s[i] == '.' || s[i] == ')') && s[i + 1] == ' ') { marked = true; return s.substr(i + 2); }
	return s;
}

static std::string TodoHtml(const std::string &text) {
	std::string out;
	for (auto &raw : SplitLines(text)) {
		std::string line = Trim(raw);
		if (line.empty()) continue;
		bool done, marked;
		std::string item = Trim(StripPrefix(line, done, marked));
		if (item.empty() || item == "-" || item == "*" || item == "[ ]" || item == "[]") continue;
		if (!marked && item.back() == ':') out += (out.empty() ? "<b>" : "\n<b>") + HtmlEsc(item) + "</b>\n";
		else out += std::string(done ? "\xE2\x98\x91 <s>" + HtmlEsc(item) + "</s>" : "\xE2\x98\x90 " + HtmlEsc(item)) + "\n";
	}
	return out;
}

static bool SendContent(const Config &c, const Outgoing &o, std::string &err) {
	std::string fileName = o.fileName.empty() ? "note.txt" : o.fileName;
	if (fileName.find('.') == std::string::npos) fileName += ".txt";
	if (o.forceFile)
		return TgSendDocument(c, fileName, o.fileBytes.empty() ? o.text : o.fileBytes, "\xF0\x9F\x93\x84 <b>" + HtmlEsc(o.title) + "</b>", err);

	std::string body = RemoveCR(o.text);
	if (Trim(body).empty()) {
		err = "Nothing to send: the note is empty.";
		return false;
	}
	std::string html;
	if (o.todo) {
		html = "\xF0\x9F\x93\x9D <b>" + HtmlEsc(o.title) + "</b>\n\n" + TodoHtml(body);
		if (Utf16Len(html) > 4000) html.clear();
	} else if (Utf16Len(body) <= c.longChars) {
		if (o.isCode) {
			std::string open = o.lang.empty() ? "<pre>" : "<pre><code class=\"language-" + o.lang + "\">";
			std::string close = o.lang.empty() ? "</pre>" : "</code></pre>";
			html = "\xF0\x9F\x92\xBB <b>" + HtmlEsc(o.title) + "</b>\n" + open + HtmlEsc(body) + close;
		} else {
			html = "\xF0\x9F\x93\x84 <b>" + HtmlEsc(o.title) + "</b>\n\n" + HtmlEsc(Trim(body));
		}
	}
	if (html.empty())  // too long for one message: send it as a file
		return TgSendDocument(c, fileName, o.text, "\xF0\x9F\x93\x84 <b>" + HtmlEsc(o.title) + "</b> (long note, sent as a file)", err);
	return TgSendHtml(c, html, err);
}

// ============================================================================
// Talking to Notepad++ (UI thread)
// ============================================================================
static HWND CurScintilla() {
	int which = -1;
	SendMessageW(g_npp._nppHandle, NPPM_GETCURRENTSCINTILLA, 0, (LPARAM)&which);
	return which == 1 ? g_npp._scintillaSecondHandle : g_npp._scintillaMainHandle;
}

static std::string DocToUtf8(HWND sci, const std::string &raw) {
	return SendMessageW(sci, SCI_GETCODEPAGE, 0, 0) == CP_UTF8 ? raw : FromAnsi(raw);
}

static std::string GetDocText() {
	HWND sci = CurScintilla();
	size_t len = (size_t)SendMessageW(sci, SCI_GETLENGTH, 0, 0);
	std::string buf(len + 2, '\0');
	SendMessageW(sci, SCI_GETTEXT, len + 1, (LPARAM)&buf[0]);
	buf.resize(strlen(buf.c_str()) < len ? strlen(buf.c_str()) : len);
	return DocToUtf8(sci, buf);
}

static std::string GetSelText() {
	HWND sci = CurScintilla();
	if (SendMessageW(sci, SCI_GETSELECTIONEMPTY, 0, 0)) return {};
	size_t len = (size_t)SendMessageW(sci, SCI_GETSELTEXT, 0, 0);
	std::string buf(len + 2, '\0');
	SendMessageW(sci, SCI_GETSELTEXT, 0, (LPARAM)&buf[0]);
	buf.resize(strlen(buf.c_str()));
	return DocToUtf8(sci, buf);
}

static std::wstring GetDocName() {
	wchar_t buf[MAX_PATH] = {};
	SendMessageW(g_npp._nppHandle, NPPM_GETFILENAME, MAX_PATH, (LPARAM)buf);
	return buf;
}

static bool CurrentDocIsCode(const std::wstring &name) {
	int lang = L_TEXT;
	SendMessageW(g_npp._nppHandle, NPPM_GETCURRENTLANGTYPE, 0, (LPARAM)&lang);
	if (lang == L_TEXT || lang == L_USER) return false;
	std::wstring ext = ExtOf(name);
	return ext != L"md" && ext != L"txt";
}

static void OpenInNewTab(const std::string &utf8) {
	SendMessageW(g_npp._nppHandle, NPPM_MENUCOMMAND, 0, IDM_FILE_NEW);
	HWND sci = CurScintilla();
	std::string text = SendMessageW(sci, SCI_GETCODEPAGE, 0, 0) == CP_UTF8 ? utf8 : ToAnsi(utf8);
	SendMessageW(sci, SCI_SETTEXT, 0, (LPARAM)text.c_str());
	SendMessageW(sci, SCI_CONVERTEOLS, SendMessageW(sci, SCI_GETEOLMODE, 0, 0), 0);
	SendMessageW(sci, SCI_DOCUMENTSTART, 0, 0);
}

static void ShowInfo(const std::wstring &msg, UINT icon = MB_ICONINFORMATION) {
	MessageBoxW(g_npp._nppHandle, msg.c_str(), PLUGIN_NAME, MB_OK | icon);
}

// Safe from any thread: the message window lives on the UI thread.
static void PostStatus(const std::wstring &msg, bool isError) {
	if (g_msgWnd && !g_stopping) PostMessageW(g_msgWnd, WM_TN_STATUS, isError, (LPARAM) new std::wstring(msg));
}

// ============================================================================
// Inbox and lists (any thread)
// ============================================================================
static std::wstring InboxPath(const Config &c) { return c.notesFolder + L"\\" + INBOX_FILE; }

static int InboxCount(const Config &c) {
	std::string b;
	ReadFileBytes(InboxPath(c), b);
	int n = 0;
	for (size_t p = 0; (p = b.find("=== ", p)) != std::string::npos; p += 4)
		if (p == 0 || b[p - 1] == '\n') ++n;
	return n;
}

struct ListDef {
	const char *cmd, *tickCmd, *title, *emoji;
	const wchar_t *file;
	bool splitCommas;
};
static const ListDef LISTS[] = {
	{"todo", "done", "Todo", "\xF0\x9F\x93\x9D", L"todo.txt", false},
	{"grocery", "got", "Grocery list", "\xF0\x9F\x9B\x92", L"grocery.txt", true},
};

struct ListItem {
	bool done;
	std::string text;
};

static std::vector<ListItem> LoadList(const Config &c, const ListDef &l) {
	std::vector<ListItem> items;
	std::string b;
	ReadFileBytes(c.notesFolder + L"\\" + l.file, b);
	for (auto &line : SplitLines(AnyToUtf8(b))) {
		std::string t = Trim(line);
		if (t.empty()) continue;
		bool done, marked;
		std::string item = Trim(StripPrefix(t, done, marked));
		if (!item.empty()) items.push_back({done, item});
	}
	return items;
}

static void SaveList(const Config &c, const ListDef &l, const std::vector<ListItem> &items) {
	std::string out;
	for (auto &i : items) out += std::string(i.done ? "[x] " : "[ ] ") + i.text + "\r\n";
	WriteFileBytes(c.notesFolder + L"\\" + l.file, out);
}

static std::string ListHtml(const ListDef &l, const std::vector<ListItem> &items) {
	std::string h = std::string(l.emoji) + " <b>" + l.title + "</b>\n\n";
	if (items.empty())
		return h + "Empty. Add something: /" + l.cmd + (l.splitCommas ? " eggs, bread" : " buy milk");
	int open = 0;
	for (size_t i = 0; i < items.size(); ++i) {
		h += std::to_string(i + 1) + ". " + (items[i].done ? "\xE2\x98\x91 <s>" + HtmlEsc(items[i].text) + "</s>" : "\xE2\x98\x90 " + HtmlEsc(items[i].text)) + "\n";
		if (!items[i].done) ++open;
	}
	h += "\n" + std::to_string(open) + " left. Tick one: /" + l.tickCmd + " 1";
	if (open < (int)items.size()) h += std::string(" \xC2\xB7 Remove ticked: /clear ") + l.cmd;
	return h;
}

// ============================================================================
// Phone commands (poll thread)
// ============================================================================
struct NoteEntry {
	std::wstring name, path;
	bool open;
};
static std::vector<NoteEntry> g_lastList;

static LRESULT NppSend(UINT msg, WPARAM w, LPARAM l) {
	DWORD_PTR r = 0;
	if (!SendMessageTimeoutW(g_npp._nppHandle, msg, w, l, SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &r)) return 0;
	return (LRESULT)r;
}

static std::vector<NoteEntry> CollectNotes(const Config &c) {
	std::vector<NoteEntry> v;
	auto have = [&](const std::wstring &p) {
		for (auto &e : v) if (!lstrcmpiW(e.path.c_str(), p.c_str())) return true;
		return false;
	};
	const int views[2][2] = {{PRIMARY_VIEW, MAIN_VIEW}, {SECOND_VIEW, SUB_VIEW}};
	for (auto &view : views) {
		int n = (int)NppSend(NPPM_GETNBOPENFILES, 0, view[0]);
		for (int i = 0; i < n && v.size() < 60; ++i) {
			LRESULT id = NppSend(NPPM_GETBUFFERIDFROMPOS, i, view[1]);
			if (!id || id == -1) continue;
			int len = (int)NppSend(NPPM_GETFULLPATHFROMBUFFERID, id, 0);
			if (len <= 0 || len > 32000) continue;
			std::wstring p(len + 1, L'\0');
			NppSend(NPPM_GETFULLPATHFROMBUFFERID, id, (LPARAM)&p[0]);
			p.resize(wcslen(p.c_str()));
			if (FileExists(p) && !have(p)) v.push_back({FileNameOf(p), p, true});
		}
	}
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((c.notesFolder + L"\\*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		std::vector<NoteEntry> folder;
		do {
			if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN)) continue;
			if (!lstrcmpiW(fd.cFileName, INBOX_FILE)) continue;
			std::wstring p = c.notesFolder + L"\\" + fd.cFileName;
			if (!have(p)) folder.push_back({fd.cFileName, p, false});
		} while (FindNextFileW(h, &fd) && folder.size() < 60);
		FindClose(h);
		std::sort(folder.begin(), folder.end(), [](const NoteEntry &a, const NoteEntry &b) { return lstrcmpiW(a.name.c_str(), b.name.c_str()) < 0; });
		v.insert(v.end(), folder.begin(), folder.end());
	}
	return v;
}

static void Reply(const Config &c, const std::string &html) {
	std::string err;
	TgSendHtml(c, html, err);
}

static const char *HELP_HTML =
	"\xF0\x9F\x91\x8B <b>Notepad++ Telegram Notes</b>\n"
	"I answer while Notepad++ is open on your PC.\n\n"
	"<b>Type any message</b> and I keep it for Notepad++ "
	"(Plugins \xE2\x80\xBA Telegram Notes \xE2\x80\xBA Fetch notes from phone).\n\n"
	"/list \xE2\x80\x93 show your notes\n"
	"/get name \xE2\x80\x93 send me a note (or /get 2)\n"
	"/todo \xE2\x80\x93 todo list \xC2\xB7 /todo buy milk \xE2\x80\x93 add\n"
	"/done 2 \xE2\x80\x93 tick todo item 2\n"
	"/grocery \xE2\x80\x93 grocery list \xC2\xB7 /grocery eggs, bread \xE2\x80\x93 add\n"
	"/got 1 \xE2\x80\x93 tick grocery item 1\n"
	"/clear todo \xE2\x80\x93 remove ticked items (or /clear grocery)\n"
	"/ping \xE2\x80\x93 check Notepad++ is online";

static void CmdList(const Config &c) {
	g_lastList = CollectNotes(c);
	if (g_lastList.empty()) {
		Reply(c, "No notes yet. Save a file in Notepad++, or put files in your notes folder.");
		return;
	}
	std::string h = "\xF0\x9F\x93\x9A <b>Your notes</b>\n";
	bool lastOpen = !g_lastList[0].open;
	for (size_t i = 0; i < g_lastList.size(); ++i) {
		if (g_lastList[i].open != lastOpen) {
			h += g_lastList[i].open ? "\n<b>Open in Notepad++</b>\n" : "\n<b>Notes folder</b>\n";
			lastOpen = g_lastList[i].open;
		}
		h += std::to_string(i + 1) + ". " + HtmlEsc(W2U(g_lastList[i].name)) + "\n";
	}
	h += "\nSend <code>/get 1</code> or <code>/get name</code>";
	Reply(c, h);
}

static void CmdGet(const Config &c, const std::string &arg) {
	if (arg.empty()) {
		Reply(c, "Which note? Send /list first, then e.g. <code>/get 2</code> or <code>/get shopping</code>.");
		return;
	}
	const NoteEntry *pick = nullptr;
	bool numeric = std::all_of(arg.begin(), arg.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
	if (numeric) {
		if (g_lastList.empty()) g_lastList = CollectNotes(c);
		size_t n = (size_t)atoi(arg.c_str());
		if (n >= 1 && n <= g_lastList.size()) pick = &g_lastList[n - 1];
	} else {
		g_lastList = CollectNotes(c);
		std::wstring want = LowerW(U2W(arg));
		std::vector<const NoteEntry *> exact, partial;
		for (auto &e : g_lastList) {
			std::wstring name = LowerW(e.name);
			std::wstring stem = name.substr(0, name.find_last_of(L'.'));
			if (name == want || stem == want) exact.push_back(&e);
			else if (name.find(want) != std::wstring::npos) partial.push_back(&e);
		}
		if (!exact.empty()) pick = exact[0];
		else if (partial.size() == 1) pick = partial[0];
		else if (partial.size() > 1) {
			std::string h = "Several notes match. Which one?\n";
			for (size_t i = 0; i < g_lastList.size(); ++i)
				if (std::find(partial.begin(), partial.end(), &g_lastList[i]) != partial.end())
					h += "/get " + std::to_string(i + 1) + " \xE2\x80\x93 " + HtmlEsc(W2U(g_lastList[i].name)) + "\n";
			Reply(c, h);
			return;
		}
	}
	if (!pick) {
		Reply(c, "I could not find <b>" + HtmlEsc(arg) + "</b>. Send /list to see your notes.");
		return;
	}
	std::string bytes;
	if (!ReadFileBytes(pick->path, bytes, 49 * 1024 * 1024)) {
		Reply(c, "I could not read <b>" + HtmlEsc(W2U(pick->name)) + "</b> (missing, or bigger than 49 MB).");
		return;
	}
	Outgoing o;
	o.title = o.fileName = W2U(pick->name);
	std::wstring ext = ExtOf(pick->name);
	if (bytes.find('\0') != std::string::npos && !(bytes.size() >= 2 && (unsigned char)bytes[0] == 0xFF && (unsigned char)bytes[1] == 0xFE)) {
		o.forceFile = true;
		o.fileBytes = bytes;
	} else {
		o.text = AnyToUtf8(bytes);
		o.isCode = IsCodeExt(ext);
		o.lang = LangFromExt(ext);
		if (!lstrcmpiW(pick->name.c_str(), L"todo.txt") || !lstrcmpiW(pick->name.c_str(), L"grocery.txt")) o.todo = true;
	}
	if (!o.forceFile && Trim(o.text).empty()) {
		Reply(c, "<b>" + HtmlEsc(o.title) + "</b> is empty.");
		return;
	}
	std::string err;
	if (!SendContent(c, o, err)) Reply(c, "Sorry, sending failed: " + HtmlEsc(err));
}

static void SaveToInbox(const Config &c, const std::string &text, long long date) {
	EnterCriticalSection(&g_fileLock);
	std::string entry = "=== " + UnixToLocal(date) + " ===\r\n";
	for (auto &line : SplitLines(text)) entry += line + "\r\n";
	bool ok = WriteFileBytes(InboxPath(c), entry + "\r\n", true);
	int count = InboxCount(c);
	LeaveCriticalSection(&g_fileLock);
	if (!ok) {
		Reply(c, "\xE2\x9A\xA0 I could not save that on your PC (notes folder not writable?).");
		return;
	}
	if (c.autoOpen) {
		PostMessageW(g_msgWnd, WM_TN_AUTOFETCH, 0, 0);
		Reply(c, "\xE2\x9C\x85 Opened in Notepad++.");
	} else {
		Reply(c, "\xE2\x9C\x85 Saved for Notepad++ (" + std::to_string(count) + " waiting). Open them with Plugins \xE2\x80\xBA Telegram Notes \xE2\x80\xBA Fetch notes from phone.");
		PostStatus(L"Telegram: " + std::to_wstring(count) + L" note(s) from your phone. Plugins > Telegram Notes > Fetch notes from phone", false);
	}
}

static std::vector<int> ParseNumbers(const std::string &s) {
	std::vector<int> v;
	int cur = -1;
	for (char ch : s + " ") {
		if (ch >= '0' && ch <= '9') cur = (cur < 0 ? 0 : cur * 10) + (ch - '0');
		else if (cur >= 0) { v.push_back(cur); cur = -1; }
	}
	return v;
}

static void CmdListOp(const Config &c, const ListDef &l, const std::string &op, const std::string &arg) {
	EnterCriticalSection(&g_fileLock);
	auto items = LoadList(c, l);
	std::string note;
	if (op == "add" && !arg.empty()) {
		int added = 0;
		for (auto &line : SplitLines(arg)) {
			std::vector<std::string> parts;
			if (l.splitCommas) {
				size_t s = 0, p;
				while ((p = line.find(',', s)) != std::string::npos) { parts.push_back(line.substr(s, p - s)); s = p + 1; }
				parts.push_back(line.substr(s));
			} else parts.push_back(line);
			for (auto &part : parts) {
				bool done, marked;
				std::string t = Trim(StripPrefix(Trim(part), done, marked));
				if (!t.empty()) { items.push_back({false, t}); ++added; }
			}
		}
		note = "\xE2\x9E\x95 Added " + std::to_string(added) + ".\n\n";
	} else if (op == "tick") {
		auto nums = ParseNumbers(arg);
		if (nums.empty()) note = "Which number? e.g. <code>/" + std::string(l.tickCmd) + " 2</code>\n\n";
		for (int n : nums) {
			if (n >= 1 && n <= (int)items.size()) items[n - 1].done = !items[n - 1].done;
			else note += "There is no item " + std::to_string(n) + ".\n";
		}
		if (!note.empty()) note += "\n";
	} else if (op == "clear") {
		size_t before = items.size();
		items.erase(std::remove_if(items.begin(), items.end(), [](const ListItem &i) { return i.done; }), items.end());
		note = "\xF0\x9F\xA7\xB9 Removed " + std::to_string(before - items.size()) + " ticked item(s).\n\n";
	}
	if (op != "show") SaveList(c, l, items);
	LeaveCriticalSection(&g_fileLock);
	Reply(c, note + ListHtml(l, items));
}

static void HandleMessage(const Config &c, const JVal &msg) {
	if (msg["chat"]["id"].s != c.chatId) return;  // not you: ignore completely
	std::string text = msg["text"].s;
	if (text.empty()) text = msg["caption"].s;
	if (text.empty()) {
		Reply(c, "Sorry, I can only keep text messages for now.");
		return;
	}
	if (text[0] != '/') {
		SaveToInbox(c, text, msg["date"].i64());
		return;
	}
	size_t sp = text.find_first_of(" \n");
	std::string cmd = Lower(text.substr(1, sp == std::string::npos ? std::string::npos : sp - 1));
	std::string arg = sp == std::string::npos ? "" : Trim(text.substr(sp + 1));
	size_t at = cmd.find('@');
	if (at != std::string::npos) cmd.resize(at);

	if (cmd == "start" || cmd == "help") Reply(c, HELP_HTML);
	else if (cmd == "ping") Reply(c, "\xF0\x9F\x9F\xA2 Notepad++ is online.");
	else if (cmd == "list" || cmd == "notes") CmdList(c);
	else if (cmd == "get") CmdGet(c, arg);
	else if (cmd == "note") {
		if (arg.empty()) Reply(c, "Just type your note as a normal message, I will keep it for Notepad++.");
		else SaveToInbox(c, arg, msg["date"].i64());
	} else if (cmd == "clear") {
		std::string which = Lower(arg);
		const ListDef *l = &LISTS[0];
		for (auto &d : LISTS) if (StartsWith(which, d.cmd)) l = &d;
		CmdListOp(c, *l, "clear", "");
	} else {
		for (auto &l : LISTS) {
			if (cmd == l.cmd) return CmdListOp(c, l, arg.empty() ? "show" : "add", arg);
			if (cmd == l.tickCmd) return CmdListOp(c, l, "tick", arg);
		}
		Reply(c, "I don't know <code>/" + HtmlEsc(cmd) + "</code>. Send /help to see what I can do.");
	}
}

static DWORD WINAPI PollThread(LPVOID) {
	while (!g_stopping) {
		Config c = LoadConfig();
		if (!c.ready() || g_pausePoll) {
			WaitForSingleObject(g_stopEvent, 2000);
			continue;
		}
		long long offset = LoadOffset();
		JVal updates;
		std::string err;
		std::string body = "{\"offset\":" + std::to_string(offset) + ",\"timeout\":25,\"allowed_updates\":[\"message\"]}";
		if (!Tg(c.token, "getUpdates", body, updates, err, 40000, true)) {
			if (g_stopping) break;
			// 409 = another program (or a 2nd Notepad++) is reading this bot, or a webhook is set.
			WaitForSingleObject(g_stopEvent, err == "cancelled" ? 500 : StartsWith(err, "409:") ? 30000 : 10000);
			continue;
		}
		for (auto &u : updates.a) {
			if (g_stopping) break;
			long long id = u["update_id"].i64();
			if (id >= offset) SaveOffset(offset = id + 1);  // confirm first, so a bad message can't loop
			const JVal &m = u["message"];
			if (m.t == JVal::Obj) HandleMessage(c, m);
		}
	}
	return 0;
}

static void StartPolling() {
	if (g_pollThread) return;
	g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	g_pollThread = CreateThread(nullptr, 0, PollThread, nullptr, 0, nullptr);
}

static void StopPolling() {
	InterlockedExchange(&g_stopping, 1);
	if (g_stopEvent) SetEvent(g_stopEvent);
	CancelPoll();
	if (g_pollThread) {
		if (WaitForSingleObject(g_pollThread, 5000) == WAIT_TIMEOUT) TerminateThread(g_pollThread, 0);
		CloseHandle(g_pollThread);
		g_pollThread = nullptr;
	}
}

// ============================================================================
// Menu commands (UI thread)
// ============================================================================
static void CmdSettings() {
	EnsureIni();
	SendMessageW(g_npp._nppHandle, NPPM_DOOPEN, 0, (LPARAM)g_iniPath.c_str());
}

static bool RequireSetup(Config &c) {
	EnsureIni();
	c = LoadConfig();
	if (c.ready()) return true;
	if (MessageBoxW(g_npp._nppHandle,
	                c.token.empty() ? L"Telegram Notes is not set up yet.\n\nOpen the settings now? (It takes about 2 minutes. The steps are written at the top of the settings file.)"
	                                : L"Almost there! Your phone is not connected yet.\n\nRun \"Connect my phone\" now?",
	                PLUGIN_NAME, MB_YESNO | MB_ICONINFORMATION) == IDYES) {
		if (c.token.empty()) CmdSettings();
		else CmdConnect();
	}
	return false;
}

struct SendJob {
	Config c;
	Outgoing o;
};

static DWORD WINAPI SendThread(LPVOID p) {
	SendJob *job = (SendJob *)p;
	std::string err;
	if (SendContent(job->c, job->o, err)) {
		MessageBeep(MB_OK);
		PostStatus(L"Telegram: sent \"" + U2W(job->o.title) + L"\" to your phone ✔", false);
	} else {
		PostStatus(L"Could not send to Telegram.\n\n" + U2W(err), true);
	}
	delete job;
	return 0;
}

static void StartSend(const Config &c, Outgoing o) {
	PostStatus(L"Telegram: sending...", false);
	HANDLE h = CreateThread(nullptr, 0, SendThread, new SendJob{c, std::move(o)}, 0, nullptr);
	if (h) CloseHandle(h);
}

static Outgoing FromCurrentDoc(const std::string &text) {
	Outgoing o;
	std::wstring name = GetDocName();
	o.title = o.fileName = W2U(name);
	o.text = text;
	o.isCode = CurrentDocIsCode(name);
	o.lang = o.isCode ? LangFromExt(ExtOf(name)) : "";
	return o;
}

static void CmdSendNote() {
	Config c;
	if (!RequireSetup(c)) return;
	std::string text = GetDocText();
	if (Trim(text).empty()) return ShowInfo(L"This note is empty, so there is nothing to send.");
	StartSend(c, FromCurrentDoc(text));
}

static void CmdSendSelection() {
	Config c;
	if (!RequireSetup(c)) return;
	std::string sel = GetSelText();
	if (Trim(sel).empty()) return ShowInfo(L"Select some text first, then choose \"Send selection\" again.");
	Outgoing o = FromCurrentDoc(sel);
	o.title += " (selection)";
	StartSend(c, o);
}

static void CmdSendTodo() {
	Config c;
	if (!RequireSetup(c)) return;
	std::string text = GetSelText();
	if (Trim(text).empty()) text = GetDocText();
	if (Trim(text).empty()) return ShowInfo(L"This note is empty. Write one task per line, then try again.");
	Outgoing o = FromCurrentDoc(text);
	o.todo = true;
	o.isCode = false;
	StartSend(c, o);
}

static void CmdSendAsFile() {
	Config c;
	if (!RequireSetup(c)) return;
	std::string text = GetDocText();
	if (Trim(text).empty()) return ShowInfo(L"This note is empty, so there is nothing to send.");
	Outgoing o = FromCurrentDoc(text);
	o.forceFile = true;
	StartSend(c, o);
}

static void FetchInbox(bool quietIfEmpty) {
	Config c = LoadConfig();
	std::string bytes;
	EnterCriticalSection(&g_fileLock);
	ReadFileBytes(InboxPath(c), bytes);
	DeleteFileW(InboxPath(c).c_str());
	LeaveCriticalSection(&g_fileLock);
	std::string text = AnyToUtf8(bytes);
	if (Trim(text).empty()) {
		if (!quietIfEmpty)
			ShowInfo(c.ready() ? L"No new notes from your phone yet.\n\nType a message to your bot in Telegram (Notepad++ must be open), then try again."
			                   : L"Telegram Notes is not set up yet. Use Plugins > Telegram Notes > Settings first.");
		return;
	}
	OpenInNewTab("Notes from your phone (fetched " + NowLocal() + ")\n\n" + text);
	PostStatus(L"Telegram: notes from your phone opened in a new tab.", false);
}

static void CmdFetch() { FetchInbox(false); }

static void CmdOpenFolder() {
	Config c = LoadConfig();
	ShellExecuteW(g_npp._nppHandle, L"open", c.notesFolder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void CmdConnect() {
	EnsureIni();
	Config c = LoadConfig();
	if (c.token.empty()) {
		ShowInfo(L"Step 1 first: paste your bot token into the settings file and save it (Ctrl+S).\n\nThe settings file will open now. The steps are written at the top.");
		return CmdSettings();
	}
	JVal me;
	std::string err;
	HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
	bool ok = Tg(c.token, "getMe", "{}", me, err, 20000);
	SetCursor(old);
	if (!ok) return ShowInfo(L"Telegram did not accept your bot token.\n\n" + U2W(err) + L"\n\nTip: did you save the settings file (Ctrl+S) after pasting?", MB_ICONWARNING);
	std::wstring bot = L"@" + U2W(me["username"].s);

	InterlockedExchange(&g_pausePoll, 1);
	CancelPoll();
	for (;;) {
		JVal ups;
		if (!Tg(c.token, "getUpdates", "{\"timeout\":0,\"allowed_updates\":[\"message\"]}", ups, err, 20000)) {
			if (StartsWith(err, "409:"))
				err = "This bot is already connected to something else (a webhook or another program). "
				      "Easiest fix: create a fresh bot with @BotFather and use its token.";
			ShowInfo(L"Could not read messages from Telegram.\n\n" + U2W(err), MB_ICONWARNING);
			break;
		}
		const JVal *found = nullptr;
		long long lastId = 0;
		for (auto &u : ups.a) {
			lastId = std::max(lastId, u["update_id"].i64());
			if (u["message"]["chat"]["type"].s == "private") found = &u["message"];
		}
		if (found) {
			const JVal &from = (*found)["from"];
			std::wstring who = U2W(Trim(from["first_name"].s + " " + from["last_name"].s));
			if (!from["username"].s.empty()) who += L" (@" + U2W(from["username"].s) + L")";
			std::wstring chat = U2W((*found)["chat"]["id"].s);
			int r = MessageBoxW(g_npp._nppHandle, (L"Found a message from:\n\n    " + who + L"\n\nIs this you?").c_str(), PLUGIN_NAME, MB_YESNOCANCEL | MB_ICONQUESTION);
			std::string confirm = "{\"offset\":" + std::to_string(lastId + 1) + ",\"timeout\":0}";
			JVal ignored;
			Tg(c.token, "getUpdates", confirm, ignored, err, 20000);  // mark these messages as read
			SaveOffset(lastId + 1);
			if (r == IDCANCEL) break;
			if (r == IDYES) {
				WritePrivateProfileStringW(L"Telegram", L"ChatId", chat.c_str(), g_iniPath.c_str());
				c = LoadConfig();
				Tg(c.token, "setMyCommands", BOT_COMMANDS_JSON, ignored, err, 20000);
				if (TgSendHtml(c, "\xE2\x9C\x85 <b>Connected to Notepad++!</b>\n\n" + std::string(HELP_HTML), err))
					ShowInfo(L"All set! Check your phone: " + bot + L" just sent you a welcome message.\n\n"
					         L"Try it: in Notepad++ choose Plugins > Telegram Notes > Send note to Telegram.\n"
					         L"On your phone, send /list or just type a note.");
				else
					ShowInfo(L"Your chat id is saved, but the test message failed:\n\n" + U2W(err), MB_ICONWARNING);
				break;
			}
			continue;  // "No": wait for the right person's message
		}
		if (MessageBoxW(g_npp._nppHandle,
		                (L"Your token works! Your bot is " + bot + L".\n\n"
		                 L"Now on your phone:\n"
		                 L"   1. Open Telegram\n"
		                 L"   2. Search for " + bot + L" and open it\n"
		                 L"   3. Tap START (or send any message, like: hi)\n\n"
		                 L"Then click Retry here.").c_str(),
		                PLUGIN_NAME, MB_RETRYCANCEL | MB_ICONINFORMATION) != IDRETRY)
			break;
	}
	InterlockedExchange(&g_pausePoll, 0);
}

static void CmdHelp() {
	ShowInfo(
		L"SETUP (one time)\n"
		L"1. In Telegram, open @BotFather, send /newbot and follow the questions. It gives you a token.\n"
		L"2. Plugins > Telegram Notes > Settings: paste the token after BotToken= and save (Ctrl+S).\n"
		L"3. On your phone, open your new bot and tap START.\n"
		L"4. Plugins > Telegram Notes > Connect my phone.\n\n"
		L"FROM NOTEPAD++\n"
		L"- Send note: the whole tab (code is sent as a code block, long notes as a file)\n"
		L"- Send selection: only the selected text\n"
		L"- Send as todo list: one task per line, sent as a checklist\n"
		L"- Fetch notes from phone: opens what you typed to the bot in a new tab\n\n"
		L"FROM YOUR PHONE (while Notepad++ is open)\n"
		L"/list, /get name, /todo, /done 2, /grocery, /got 1, /clear todo, /ping\n"
		L"Anything else you type is kept for \"Fetch notes from phone\".\n\n"
		L"Only your own Telegram account (the connected chat) can use the bot.");
}

// ============================================================================
// Message window (lets background threads hand work to the UI thread)
// ============================================================================
static LRESULT CALLBACK MsgWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
	if (m == WM_TN_AUTOFETCH) {
		FetchInbox(true);
		return 0;
	}
	if (m == WM_TN_STATUS) {
		std::wstring *s = (std::wstring *)l;
		if (w) ShowInfo(*s, MB_ICONWARNING);
		else SendMessageW(g_npp._nppHandle, NPPM_SETSTATUSBAR, STATUSBAR_DOC_TYPE, (LPARAM)s->c_str());
		delete s;
		return 0;
	}
	return DefWindowProcW(h, m, w, l);
}

static void CreateMsgWindow() {
	WNDCLASSW wc = {};
	wc.lpfnWndProc = MsgWndProc;
	wc.hInstance = g_hInst;
	wc.lpszClassName = L"NppTelegramNotesMsg";
	RegisterClassW(&wc);
	g_msgWnd = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_hInst, nullptr);
}

// ============================================================================
// Plugin exports
// ============================================================================
static FuncItem g_funcs[] = {
	{L"Send note to Telegram", CmdSendNote, 0, false, nullptr},
	{L"Send selection to Telegram", CmdSendSelection, 0, false, nullptr},
	{L"Send as todo list", CmdSendTodo, 0, false, nullptr},
	{L"Send note as .txt file", CmdSendAsFile, 0, false, nullptr},
	{L"", nullptr, 0, false, nullptr},
	{L"Fetch notes from phone", CmdFetch, 0, false, nullptr},
	{L"Open notes folder", CmdOpenFolder, 0, false, nullptr},
	{L"", nullptr, 0, false, nullptr},
	{L"Settings (bot token)", CmdSettings, 0, false, nullptr},
	{L"Connect my phone", CmdConnect, 0, false, nullptr},
	{L"Help", CmdHelp, 0, false, nullptr},
};

BOOL APIENTRY DllMain(HINSTANCE h, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {
		g_hInst = h;
		InitializeCriticalSection(&g_lock);
		InitializeCriticalSection(&g_fileLock);
	}
	return TRUE;
}

extern "C" __declspec(dllexport) void setInfo(NppData data) {
	g_npp = data;
	wchar_t dir[MAX_PATH] = {};
	SendMessageW(g_npp._nppHandle, NPPM_GETPLUGINSCONFIGDIR, MAX_PATH, (LPARAM)dir);
	g_iniPath = std::wstring(dir) + L"\\TelegramNotes.ini";
	g_statePath = std::wstring(dir) + L"\\TelegramNotes.state.ini";
	CreateMsgWindow();
}

extern "C" __declspec(dllexport) const wchar_t *getName() { return PLUGIN_NAME; }

extern "C" __declspec(dllexport) FuncItem *getFuncsArray(int *n) {
	*n = (int)(sizeof(g_funcs) / sizeof(g_funcs[0]));
	return g_funcs;
}

extern "C" __declspec(dllexport) void beNotified(SCNotification *n) {
	if (n->nmhdr.hwndFrom != g_npp._nppHandle) return;
	if (n->nmhdr.code == NPPN_READY) {
		EnsureIni();
		StartPolling();
		Config c = LoadConfig();
		if (!c.ready() && IniGet(g_statePath, L"State", L"Welcomed") != L"1") {
			WritePrivateProfileStringW(L"State", L"Welcomed", L"1", g_statePath.c_str());
			if (MessageBoxW(g_npp._nppHandle,
			                L"Telegram Notes is installed!\n\nSet it up now? It takes about 2 minutes and the settings file explains every step.\n\n"
			                L"(You can do it later from Plugins > Telegram Notes > Settings.)",
			                PLUGIN_NAME, MB_YESNO | MB_ICONINFORMATION) == IDYES)
				CmdSettings();
		}
	} else if (n->nmhdr.code == NPPN_SHUTDOWN) {
		StopPolling();
		if (g_msgWnd) DestroyWindow(g_msgWnd);
		g_msgWnd = nullptr;
	}
}

extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) { return TRUE; }

extern "C" __declspec(dllexport) BOOL isUnicode() { return TRUE; }
