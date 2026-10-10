#include "lk2_proto.h"

#include <cstdio>
#include <cstring>

namespace k2se {
namespace lk2 {
namespace {

const char kHex[] = "0123456789abcdef";

// Bytes the escaped form of `s` occupies. Mirrors WriteEscaped exactly.
size_t EscapedLen(const char* s, size_t n) {
    size_t len = 0;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t' || c == '\b' || c == '\f')
            len += 2;
        else if (c < 0x20)
            len += 6;
        else
            len += 1;
    }
    return len;
}

char* WriteEscaped(char* p, const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        char esc = 0;
        switch (c) {
            case '"': esc = '"'; break;
            case '\\': esc = '\\'; break;
            case '\n': esc = 'n'; break;
            case '\r': esc = 'r'; break;
            case '\t': esc = 't'; break;
            case '\b': esc = 'b'; break;
            case '\f': esc = 'f'; break;
            default: break;
        }
        if (esc) {
            *p++ = '\\';
            *p++ = esc;
        } else if (c < 0x20) {
            *p++ = '\\'; *p++ = 'u'; *p++ = '0'; *p++ = '0';
            *p++ = kHex[c >> 4]; *p++ = kHex[c & 0xF];
        } else {
            *p++ = static_cast<char>(c);
        }
    }
    return p;
}

struct Cursor {
    const char* p;
    const char* end;
    bool at_end() const { return p >= end; }
    char peek() const { return at_end() ? '\0' : *p; }
    void skip_ws() {
        while (!at_end() && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    }
    bool take(char c) {
        if (peek() != c) return false;
        ++p;
        return true;
    }
};

int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parses a JSON string starting at the opening quote. Bytes go to `sink`
// (nullable = discard), at most cap-1 of them; `*len` is how many were stored,
// `*truncated` whether any were dropped. The cursor ends past the closing quote.
bool ParseString(Cursor& c, char* sink, uint32_t cap, uint32_t* len, bool* truncated) {
    if (!c.take('"')) return false;
    uint32_t n = 0;
    bool cut = false;
    auto put = [&](char ch) {
        if (sink && n + 1 < cap) {
            sink[n++] = ch;
        } else {
            cut = true;
        }
    };
    for (;;) {
        if (c.at_end()) return false;  // unterminated
        char ch = *c.p++;
        if (ch == '"') break;
        if (ch != '\\') {
            put(ch);
            continue;
        }
        if (c.at_end()) return false;
        ch = *c.p++;
        switch (ch) {
            case '"': put('"'); break;
            case '\\': put('\\'); break;
            case '/': put('/'); break;
            case 'b': put('\b'); break;
            case 'f': put('\f'); break;
            case 'n': put('\n'); break;
            case 'r': put('\r'); break;
            case 't': put('\t'); break;
            case 'u': {
                int code = 0;
                for (int i = 0; i < 4; ++i) {
                    if (c.at_end()) return false;
                    const int h = HexVal(*c.p++);
                    if (h < 0) return false;
                    code = (code << 4) | h;
                }
                put((code > 0 && code <= 0xFF) ? static_cast<char>(code) : '?');
                break;
            }
            default: return false;
        }
    }
    if (sink && cap > 0) sink[n < cap ? n : cap - 1] = '\0';
    if (len) *len = n;
    if (truncated) *truncated = cut;
    return true;
}

// Consumes any JSON value without keeping it. Nested arrays and objects are
// skipped by depth, with strings inside them parsed so a brace in a string
// cannot unbalance the count.
bool SkipValue(Cursor& c) {
    c.skip_ws();
    const char ch = c.peek();
    if (ch == '"') return ParseString(c, nullptr, 0, nullptr, nullptr);
    if (ch == '{' || ch == '[') {
        int depth = 0;
        for (;;) {
            if (c.at_end()) return false;
            const char k = c.peek();
            if (k == '"') {
                if (!ParseString(c, nullptr, 0, nullptr, nullptr)) return false;
                continue;
            }
            ++c.p;
            if (k == '{' || k == '[') ++depth;
            else if (k == '}' || k == ']') {
                if (--depth == 0) return true;
            }
        }
    }
    // number / true / false / null: a run of the characters those can contain
    size_t n = 0;
    while (!c.at_end()) {
        const char k = *c.p;
        const bool ok = (k >= '0' && k <= '9') || k == '-' || k == '+' || k == '.' || k == 'e' ||
                        k == 'E' || (k >= 'a' && k <= 'z');
        if (!ok) break;
        ++c.p;
        ++n;
    }
    return n > 0;
}

bool ParseInt(Cursor& c, int* out) {
    bool neg = false;
    if (c.take('-')) neg = true;
    if (c.peek() < '0' || c.peek() > '9') return false;
    // 64-bit on purpose: `long` is 32 bits under MSVC, where the tenth digit
    // overflowed before the bound below could reject it.
    int64_t v = 0;
    while (c.peek() >= '0' && c.peek() <= '9') {
        v = v * 10 + (*c.p - '0');
        if (v > 2147483647LL) return false;
        ++c.p;
    }
    *out = static_cast<int>(neg ? -v : v);
    return true;
}

}  // namespace

size_t EncodeLine(int id, const char* text, size_t textLen, char* out, size_t cap) {
    char idbuf[16];
    const int idLen = std::snprintf(idbuf, sizeof(idbuf), "%d", id);
    if (idLen <= 0) return 0;
    const size_t need = 6 + static_cast<size_t>(idLen) + 9 + EscapedLen(text, textLen) + 3;
    if (need > cap) return 0;

    char* p = out;
    std::memcpy(p, "{\"id\":", 6); p += 6;
    std::memcpy(p, idbuf, static_cast<size_t>(idLen)); p += idLen;
    std::memcpy(p, ",\"text\":\"", 9); p += 9;
    p = WriteEscaped(p, text, textLen);
    std::memcpy(p, "\"}\n", 3); p += 3;
    const size_t written = static_cast<size_t>(p - out);
    if (written < cap) *p = '\0';
    return written;
}

bool DecodeLine(const char* line, size_t lineLen, int* id, char* textOut, uint32_t cap,
                uint32_t* textLen, bool* truncated) {
    Cursor c{line, line + lineLen};
    c.skip_ws();
    if (!c.take('{')) return false;

    bool haveId = false, haveText = false;
    c.skip_ws();
    if (c.take('}')) return false;  // empty object: neither key present
    for (;;) {
        c.skip_ws();
        char key[32];
        uint32_t keyLen = 0;
        bool keyCut = false;
        if (!ParseString(c, key, sizeof(key), &keyLen, &keyCut)) return false;
        c.skip_ws();
        if (!c.take(':')) return false;
        c.skip_ws();
        if (!keyCut && keyLen == 2 && std::memcmp(key, "id", 2) == 0) {
            if (!ParseInt(c, id)) return false;
            haveId = true;
        } else if (!keyCut && keyLen == 4 && std::memcmp(key, "text", 4) == 0) {
            if (!ParseString(c, textOut, cap, textLen, truncated)) return false;
            haveText = true;
        } else {
            if (!SkipValue(c)) return false;
        }
        c.skip_ws();
        if (c.take(',')) continue;
        if (c.take('}')) break;
        return false;
    }
    c.skip_ws();
    if (!c.at_end()) return false;
    return haveId && haveText;
}

}  // namespace lk2
}  // namespace k2se
