#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace update {
struct File {
    std::string name, url, sha256;
    uint64_t size = 0;
};

struct Component {
    std::string id, version;
    std::vector<File> files;
};

struct Manifest {
    int schema = 0;
    std::string release;
    std::map<std::string, Component> components;
};

enum class Verdict { Silent, Current, Offer };

namespace detail {
inline constexpr size_t kMaxDocument = 64 * 1024;
inline constexpr size_t kMaxString = 600;
inline constexpr int kMaxDepth = 8;
inline constexpr size_t kMaxItems = 64;
inline constexpr size_t kMaxFiles = 8;
inline constexpr uint64_t kMaxFileSize = 64ull * 1024 * 1024;

struct Value {
    enum class Kind { Null, String, Number, Object, Array } kind = Kind::Null;
    std::string text;
    uint64_t number = 0;
    std::vector<std::string> keys;
    std::vector<Value> items;

    const Value* find(const char* key) const {
        if (kind != Kind::Object) return nullptr;
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == key) return &items[i];
        return nullptr;
    }
};

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    bool parse(Value& out) {
        if (!value(out, 0)) return false;
        skip();
        return pos_ == s_.size();
    }

private:
    void skip() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
    }

    bool eat(char c) {
        skip();
        if (pos_ < s_.size() && s_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool hex4(unsigned& out) {
        if (pos_ + 4 > s_.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') out |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= unsigned(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }

    bool string(std::string& out) {
        if (!eat('"')) return false;
        out.clear();
        while (pos_ < s_.size()) {
            const unsigned char c = static_cast<unsigned char>(s_[pos_++]);
            if (out.size() > kMaxString) return false;
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') {
                out += char(c);
                continue;
            }
            if (pos_ >= s_.size()) return false;
            const char e = s_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp) || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
                    appendUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool number(uint64_t& out) {
        skip();
        const size_t start = pos_;
        out = 0;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
            if (pos_ - start >= 18) return false;
            out = out * 10 + uint64_t(s_[pos_++] - '0');
        }
        if (pos_ == start) return false;
        if (pos_ - start > 1 && s_[start] == '0') return false;
        return pos_ >= s_.size() || (s_[pos_] != '.' && s_[pos_] != 'e' && s_[pos_] != 'E');
    }

    bool value(Value& out, int depth) {
        if (depth > kMaxDepth) return false;
        skip();
        if (pos_ >= s_.size()) return false;
        const char c = s_[pos_];
        if (c == '"') {
            out.kind = Value::Kind::String;
            return string(out.text);
        }
        if (c >= '0' && c <= '9') {
            out.kind = Value::Kind::Number;
            return number(out.number);
        }
        if (c == '{') {
            ++pos_;
            out.kind = Value::Kind::Object;
            if (eat('}')) return true;
            do {
                if (out.keys.size() >= kMaxItems) return false;
                std::string key;
                if (!string(key) || !eat(':')) return false;
                out.keys.push_back(std::move(key));
                out.items.emplace_back();
                if (!value(out.items.back(), depth + 1)) return false;
            } while (eat(','));
            return eat('}');
        }
        if (c == '[') {
            ++pos_;
            out.kind = Value::Kind::Array;
            if (eat(']')) return true;
            do {
                if (out.items.size() >= kMaxItems) return false;
                out.items.emplace_back();
                if (!value(out.items.back(), depth + 1)) return false;
            } while (eat(','));
            return eat(']');
        }
        return false;
    }

    const std::string& s_;
    size_t pos_ = 0;
};

inline bool isVersion(const std::string& v) {
    int parts = 1;
    size_t digits = 0;
    for (char c : v) {
        if (c == '.') {
            if (digits == 0) return false;
            digits = 0;
            ++parts;
        } else if (c >= '0' && c <= '9') {
            if (++digits > 9) return false;
        } else {
            return false;
        }
    }
    return digits > 0 && parts == 3;
}

inline bool isSha256(std::string& s) {
    if (s.size() != 64) return false;
    for (char& c : s) {
        if (c >= 'A' && c <= 'F') c = char(c - 'A' + 'a');
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

inline bool isSafeId(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' ||
              c == '.'))
            return false;
    return true;
}

inline const std::string* stringField(const Value& v, const char* key) {
    const Value* f = v.find(key);
    return f && f->kind == Value::Kind::String ? &f->text : nullptr;
}

inline bool readComponent(const Value& v, Component& out) {
    const std::string* id = stringField(v, "id");
    const std::string* version = stringField(v, "version");
    const Value* files = v.find("files");
    if (!id || !version || !files || files->kind != Value::Kind::Array) return false;
    if (!isSafeId(*id) || !isVersion(*version) || files->items.size() > kMaxFiles) return false;
    out.id = *id;
    out.version = *version;
    for (const Value& f : files->items) {
        const std::string* name = stringField(f, "name");
        const std::string* url = stringField(f, "url");
        const std::string* sha = stringField(f, "sha256");
        const Value* size = f.find("size");
        if (f.kind != Value::Kind::Object || !name || !url || !sha || !size || size->kind != Value::Kind::Number)
            return false;
        File file;
        file.name = *name;
        file.url = *url;
        file.sha256 = *sha;
        file.size = size->number;
        if (!isSha256(file.sha256) || file.size > kMaxFileSize || file.url.empty()) return false;
        out.files.push_back(std::move(file));
    }
    return true;
}
}

inline bool parseManifest(const std::string& json, Manifest& out) {
    out = Manifest{};
    if (json.empty() || json.size() > detail::kMaxDocument) return false;
    detail::Value root;
    detail::Parser parser(json);
    if (!parser.parse(root) || root.kind != detail::Value::Kind::Object) return false;

    const detail::Value* schema = root.find("schema");
    const std::string* release = detail::stringField(root, "release");
    const detail::Value* components = root.find("components");
    if (!schema || schema->kind != detail::Value::Kind::Number || schema->number != 1) return false;
    if (!release || !detail::isVersion(*release)) return false;
    if (!components || components->kind != detail::Value::Kind::Object) return false;
    out.schema = 1;
    out.release = *release;

    for (size_t i = 0; i < components->keys.size(); ++i) {
        const std::string& name = components->keys[i];
        if (name != "switch-sysmodule" && name != "switch-manager") continue;
        Component c;
        if (!detail::readComponent(components->items[i], c)) {
            out = Manifest{};
            return false;
        }
        out.components[name] = std::move(c);
    }
    return true;
}

inline int compareVersion(const std::string& a, const std::string& b) {
    auto part = [](const std::string& s, size_t& pos) {
        uint64_t n = 0;
        while (pos < s.size() && s[pos] != '.') {
            if (s[pos] >= '0' && s[pos] <= '9' && n < 100000000000ull) n = n * 10 + uint64_t(s[pos] - '0');
            ++pos;
        }
        if (pos < s.size()) ++pos;
        return n;
    };
    size_t pa = 0, pb = 0;
    for (int i = 0; i < 3; ++i) {
        const uint64_t x = part(a, pa), y = part(b, pb);
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

inline Verdict decide(const std::string& localId, const std::string& localVersion, const Component* remote) {
    if (localId.empty() || localId == "dev" || !remote) return Verdict::Silent;
    if (compareVersion(localVersion, remote->version) >= 0) return Verdict::Silent;
    return localId == remote->id ? Verdict::Current : Verdict::Offer;
}

inline const char* destFor(const std::string& component, const std::string& name) {
    if (component == "switch-sysmodule") {
        if (name == "exefs.nsp") return "sdmc:/atmosphere/contents/4200000000004E53/exefs.nsp";
        if (name == "toolbox.json") return "sdmc:/atmosphere/contents/4200000000004E53/toolbox.json";
        if (name == "nxstream.id") return "sdmc:/atmosphere/contents/4200000000004E53/nxstream.id";
    }
    if (component == "switch-manager" && name == "nxstream.nro") return "sdmc:/switch/NXStream/nxstream.nro";
    return nullptr;
}

struct Url {
    std::string scheme, host, port, path;
    std::string origin() const { return scheme + "://" + host + (port.empty() ? "" : ":" + port); }
};

inline bool hasSuffix(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline bool parseUrl(const std::string& url, Url& out, bool strict) {
    out = Url{};
    size_t start;
    if (url.compare(0, 8, "https://") == 0) {
        out.scheme = "https";
        start = 8;
    } else if (url.compare(0, 7, "http://") == 0) {
        out.scheme = "http";
        start = 7;
    } else {
        return false;
    }
    if (strict) {
        for (char c : url)
            if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F || c == '\\' || c == '@') return false;
        if (url.find("..") != std::string::npos || url.find("%2e") != std::string::npos ||
            url.find("%2E") != std::string::npos)
            return false;
    }
    const size_t end = url.find_first_of("/?#", start);
    const std::string authority = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    out.path = end == std::string::npos ? "/" : url.substr(end);
    if (authority.empty()) return false;
    for (char c : authority)
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F || c == '\\' || c == '@' || c == '[' || c == ']')
            return false;
    const size_t colon = authority.find(':');
    out.host = authority.substr(0, colon);
    if (colon != std::string::npos) {
        out.port = authority.substr(colon + 1);
        if (out.port.empty() || out.port.size() > 5) return false;
        for (char c : out.port)
            if (c < '0' || c > '9') return false;
    }
    for (char& c : out.host)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return !out.host.empty();
}

inline bool isPrivateIpv4(const std::string& host) {
    int octet[4];
    size_t pos = 0;
    for (int i = 0; i < 4; ++i) {
        const size_t start = pos;
        int n = 0;
        while (pos < host.size() && host[pos] >= '0' && host[pos] <= '9') {
            n = n * 10 + (host[pos++] - '0');
            if (n > 255) return false;
        }
        const size_t len = pos - start;
        if (len == 0 || len > 3 || (len > 1 && host[start] == '0')) return false;
        octet[i] = n;
        if (i < 3) {
            if (pos >= host.size() || host[pos] != '.') return false;
            ++pos;
        }
    }
    if (pos != host.size()) return false;
    if (octet[0] == 10) return true;
    if (octet[0] == 172 && octet[1] >= 16 && octet[1] <= 31) return true;
    if (octet[0] == 192 && octet[1] == 168) return true;
    return octet[0] == 127 && octet[1] == 0 && octet[2] == 0 && octet[3] == 1;
}

inline bool parseOverride(const std::string& text, std::string& manifestUrl, std::string& prefix) {
    std::string base = text.substr(0, text.find_first_of("\r\n"));
    while (!base.empty() && (base.back() == ' ' || base.back() == '\t' || base.back() == '/')) base.pop_back();
    Url u;
    if (!parseUrl(base, u, true) || !isPrivateIpv4(u.host)) return false;
    manifestUrl = base + "/nxstream-manifest.json";
    prefix = u.origin() + "/";
    return true;
}

inline bool allowedUrl(const std::string& url, const std::string& prefix) {
    Url u, p;
    if (!parseUrl(url, u, true) || !parseUrl(prefix, p, false)) return false;
    if (u.scheme != p.scheme || u.host != p.host || u.port != p.port) return false;
    return u.path.size() > p.path.size() && u.path.compare(0, p.path.size(), p.path) == 0;
}

inline bool trustedFinalUrl(const std::string& effective, const std::string& overridePrefix) {
    Url u;
    if (!parseUrl(effective, u, false)) return false;
    if (!overridePrefix.empty()) {
        Url p;
        return parseUrl(overridePrefix, p, false) && u.scheme == p.scheme && u.host == p.host && u.port == p.port;
    }
    if (u.scheme != "https" || !u.port.empty()) return false;
    const std::string suffix = ".githubusercontent.com";
    return u.host == "github.com" || (u.host.size() > suffix.size() && hasSuffix(u.host, suffix));
}

inline bool parseLocalId(const std::string& content, std::string& id, std::string& version) {
    const size_t sp = content.find(' ');
    if (sp == std::string::npos) return false;
    id = content.substr(0, sp);
    version = content.substr(sp + 1);
    while (!version.empty() && (version.back() == '\n' || version.back() == '\r' || version.back() == ' '))
        version.pop_back();
    return detail::isSafeId(id) && detail::isVersion(version);
}

template <class Fn>
std::vector<File> filesToFetch(const Component& component, Fn installedSha) {
    std::vector<File> out;
    for (const File& f : component.files)
        if (installedSha(f) != f.sha256) out.push_back(f);
    return out;
}

template <class Fn>
Verdict decideBySha(const Component* remote, Fn installedSha) {
    if (!remote) return Verdict::Silent;
    return filesToFetch(*remote, installedSha).empty() ? Verdict::Current : Verdict::Offer;
}
}
