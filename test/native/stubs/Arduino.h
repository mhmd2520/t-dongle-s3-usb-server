// Minimal Arduino stub for host/native unit tests.
// Provides String, millis(), and other thin wrappers that the
// pure-logic functions under test depend on — nothing hardware-specific.
#pragma once

#include <string>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <algorithm>

// ── String — wraps std::string with the Arduino String API surface ────────────
// Only the methods actually used by the four functions under test are provided.
// Any attempt to use an unimplemented method produces a link error, which is
// the correct signal to add a stub rather than silently pass/fail.
class String {
public:
    String() = default;
    String(const char* s)  : _s(s ? s : "") {}
    String(const String&)  = default;
    String(String&&)       = default;
    String& operator=(const String&) = default;
    String& operator=(String&&)      = default;
    String& operator=(const char* s) { _s = s ? s : ""; return *this; }

    bool isEmpty()  const { return _s.empty(); }
    unsigned int length() const { return (unsigned int)_s.size(); }

    char operator[](unsigned int i) const {
        return i < _s.size() ? _s[i] : '\0';
    }

    bool startsWith(const char* prefix) const {
        return _s.rfind(prefix, 0) == 0;
    }
    bool startsWith(const String& prefix) const {
        return startsWith(prefix.c_str());
    }

    bool endsWith(const char* suffix) const {
        if (strlen(suffix) > _s.size()) return false;
        return _s.compare(_s.size() - strlen(suffix), strlen(suffix), suffix) == 0;
    }

    int indexOf(const char* needle) const {
        auto pos = _s.find(needle);
        return pos == std::string::npos ? -1 : (int)pos;
    }
    int indexOf(char c) const {
        auto pos = _s.find(c);
        return pos == std::string::npos ? -1 : (int)pos;
    }
    int indexOf(const String& needle) const {
        return indexOf(needle.c_str());
    }

    bool equalsIgnoreCase(const char* other) const {
        if (_s.size() != strlen(other)) return false;
        for (size_t i = 0; i < _s.size(); i++) {
            if (tolower((unsigned char)_s[i]) != tolower((unsigned char)other[i]))
                return false;
        }
        return true;
    }
    bool equalsIgnoreCase(const String& other) const {
        return equalsIgnoreCase(other.c_str());
    }

    bool operator==(const char* other) const { return _s == (other ? other : ""); }
    bool operator==(const String& other) const { return _s == other._s; }
    bool operator!=(const char* other) const { return !(*this == other); }
    bool operator!=(const String& other) const { return !(*this == other); }

    String substring(unsigned int from) const {
        if (from >= _s.size()) return String("");
        return String(_s.substr(from).c_str());
    }
    String substring(unsigned int from, unsigned int to) const {
        if (from >= _s.size()) return String("");
        return String(_s.substr(from, to - from).c_str());
    }

    // toInt(): matches Arduino behaviour — stops at first non-digit after
    // optional leading whitespace/sign.  Uses strtol for correctness.
    long toInt() const { return strtol(_s.c_str(), nullptr, 10); }

    void replace(const char* from, const char* to) {
        std::string result;
        size_t pos = 0, flen = strlen(from), tlen = strlen(to);
        while (pos < _s.size()) {
            auto found = _s.find(from, pos);
            if (found == std::string::npos) { result += _s.substr(pos); break; }
            result += _s.substr(pos, found - pos);
            result += to;
            pos = found + flen;
        }
        _s = result;
    }

    String operator+(const char* rhs) const { return String((_s + (rhs ? rhs : "")).c_str()); }
    String operator+(const String& rhs) const { return *this + rhs.c_str(); }
    String& operator+=(const char* rhs) { _s += rhs ? rhs : ""; return *this; }
    String& operator+=(const String& rhs) { return *this += rhs.c_str(); }

    const char* c_str() const { return _s.c_str(); }

private:
    std::string _s;
};

// ── millis() stub ─────────────────────────────────────────────────────────────
#include <chrono>
inline uint32_t millis() {
    using namespace std::chrono;
    static auto start = steady_clock::now();
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - start).count();
}

// ── Minimal Serial stub ───────────────────────────────────────────────────────
struct SerialStub {
    void println(const char*) {}
    void printf(const char*, ...) {}
};
static SerialStub Serial;
