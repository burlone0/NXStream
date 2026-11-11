#include "update.hpp"

#include <curl/curl.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifndef NXSTREAM_CONTENT_ID
#define NXSTREAM_CONTENT_ID "dev"
#endif
#ifndef NXSTREAM_VERSION
#define NXSTREAM_VERSION "0.0.0"
#endif

namespace update {
namespace {
constexpr const char* kManifestUrl = "https://github.com/burlone0/NXStream/releases/latest/download/nxstream-manifest.json";
constexpr const char* kReleasePrefix = "https://github.com/burlone0/NXStream/releases/download/";
constexpr const char* kOverridePath = "sdmc:/config/nxstream/update_url.txt";
constexpr const char* kIdPath = "sdmc:/atmosphere/contents/4200000000004E53/nxstream.id";
constexpr const char* kToolboxPath = "sdmc:/atmosphere/contents/4200000000004E53/toolbox.json";
constexpr const char* kExefsPath = "sdmc:/atmosphere/contents/4200000000004E53/exefs.nsp";
constexpr const char* kDefaultNro = "sdmc:/switch/NXStream/nxstream.nro";
constexpr const char* kManagerDir = "sdmc:/switch/NXStream";
constexpr size_t kManifestLimit = 64 * 1024;
constexpr size_t kThreadStack = 0x20000;

bool g_nifm = false;
bool g_curl = false;

bool exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool readFile(const char* path, std::string& out, size_t limit) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    char buf[256];
    size_t n;
    out.clear();
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && out.size() <= limit) out.append(buf, n);
    std::fclose(f);
    return out.size() <= limit;
}

std::string hex(const u8* hash) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < SHA256_HASH_SIZE; ++i) {
        out += digits[hash[i] >> 4];
        out += digits[hash[i] & 15];
    }
    return out;
}

std::string sha256File(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return "";
    Sha256Context ctx;
    sha256ContextCreate(&ctx);
    std::vector<char> buf(16 * 1024);
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) sha256ContextUpdate(&ctx, buf.data(), n);
    std::fclose(f);
    u8 hash[SHA256_HASH_SIZE];
    sha256ContextGetHash(&ctx, hash);
    return hex(hash);
}

int commitRank(const std::string& dest) {
    if (dest.size() >= 12 && dest.compare(dest.size() - 12, 12, "toolbox.json") == 0) return 0;
    if (dest.size() >= 11 && dest.compare(dest.size() - 11, 11, "nxstream.id") == 0) return 1;
    return 2;
}

struct Transfer {
    FILE* file = nullptr;
    std::string* memory = nullptr;
    Sha256Context sha{};
    uint64_t received = 0;
    uint64_t limit = 0;
    uint64_t base = 0;
    uint64_t total = 0;
    std::atomic<bool>* cancel = nullptr;
    std::atomic<int>* percent = nullptr;
    bool untrusted = false;
};

size_t onWrite(char* data, size_t size, size_t count, void* user) {
    auto* t = static_cast<Transfer*>(user);
    const size_t n = size * count;
    if (t->received + n > t->limit) return 0;
    if (t->memory) {
        t->memory->append(data, n);
    } else {
        if (std::fwrite(data, 1, n, t->file) != n) return 0;
        sha256ContextUpdate(&t->sha, data, n);
    }
    t->received += n;
    if (t->percent && t->total) t->percent->store(static_cast<int>((t->base + t->received) * 100 / t->total));
    return n;
}

int onProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* t = static_cast<Transfer*>(user);
    return t->cancel && t->cancel->load() ? 1 : 0;
}

bool fetch(const std::string& url, Transfer& t, const std::string& overridePrefix) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    const long protocols = overridePrefix.compare(0, 7, "http://") == 0 ? CURLPROTO_HTTP : CURLPROTO_HTTPS;
    const std::string agent = std::string("NXStream-manager/") + NXSTREAM_VERSION;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, protocols);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, protocols);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, agent.c_str());
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, onProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
    const CURLcode rc = curl_easy_perform(curl);
    bool trusted = false;
    if (rc == CURLE_OK) {
        char* effective = nullptr;
        trusted = curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective) == CURLE_OK && effective &&
                  trustedFinalUrl(effective, overridePrefix);
        t.untrusted = !trusted;
    }
    curl_easy_cleanup(curl);
    return rc == CURLE_OK && trusted;
}

bool endsWith(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}
}

bool init() {
    g_nifm = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
    g_curl = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    return g_nifm && g_curl;
}

void exit() {
    if (g_curl) curl_global_cleanup();
    if (g_nifm) nifmExit();
    g_curl = false;
    g_nifm = false;
}

Updater::Updater(const char* argv0) : argv0_(argv0 ? argv0 : "") {
    for (const char* dest : {kExefsPath, kIdPath, kToolboxPath, kDefaultNro}) recover(dest);
    if (argv0_.compare(0, 6, "sdmc:/") == 0 && endsWith(argv0_, ".nro")) recover(argv0_);
}

void Updater::recover(const std::string& dest) {
    const std::string bak = dest + ".bak";
    std::remove((dest + ".tmp").c_str());
    if (!exists(dest) && exists(bak)) std::rename(bak.c_str(), dest.c_str());
}

Updater::~Updater() {
    cancel_ = true;
    join();
}

void Updater::join() {
    if (!threadActive_) return;
    threadWaitForExit(&thread_);
    threadClose(&thread_);
    threadActive_ = false;
}

void Updater::entry(void* self) {
    auto* u = static_cast<Updater*>(self);
    if (u->applying_) u->apply();
    else u->check();
}

void Updater::startCheck() {
    if (!g_nifm || !g_curl || state() != State::Idle) return;
    NifmInternetConnectionType type;
    u32 strength;
    NifmInternetConnectionStatus status;
    if (R_FAILED(nifmGetInternetConnectionStatus(&type, &strength, &status)) ||
        status != NifmInternetConnectionStatus_Connected)
        return;
    join();
    applying_ = false;
    state_.store(State::Checking, std::memory_order_release);
    if (R_FAILED(threadCreate(&thread_, entry, this, nullptr, kThreadStack, 0x2C, -2)) ||
        R_FAILED(threadStart(&thread_))) {
        state_.store(State::Idle, std::memory_order_release);
        return;
    }
    threadActive_ = true;
}

void Updater::startApply() {
    if (state() != State::Offer) return;
    join();
    applying_ = true;
    percent_ = 0;
    state_.store(State::Downloading, std::memory_order_release);
    if (R_FAILED(threadCreate(&thread_, entry, this, nullptr, kThreadStack, 0x2C, -2)) ||
        R_FAILED(threadStart(&thread_))) {
        fail("cannot start");
        return;
    }
    threadActive_ = true;
}

void Updater::dismiss() {
    const State s = state();
    if (s == State::Offer || s == State::Done || s == State::Failed) state_.store(State::Idle, std::memory_order_release);
}

void Updater::fail(const char* why) {
    reason_ = why;
    state_.store(State::Failed, std::memory_order_release);
}

bool Updater::loadOverride() {
    manifestUrl_ = kManifestUrl;
    overridePrefix_.clear();
    std::string text, manifest, prefix;
    if (!exists(kOverridePath) || !readFile(kOverridePath, text, 1024)) return true;
    if (!parseOverride(text, manifest, prefix)) return true;
    manifestUrl_ = manifest;
    overridePrefix_ = prefix;
    return true;
}

std::string Updater::destination(const std::string& component, const std::string& name) const {
    const char* fixed = destFor(component, name);
    if (!fixed) return "";
    if (component == "switch-manager" && argv0_.compare(0, 6, "sdmc:/") == 0 && endsWith(argv0_, ".nro")) return argv0_;
    return fixed;
}

void Updater::check() {
    jobs_.clear();
    summary_.clear();
    moduleChanged_ = false;
    if (!loadOverride()) return state_.store(State::Idle, std::memory_order_release);

    std::string body;
    Transfer t;
    t.memory = &body;
    t.limit = kManifestLimit;
    t.cancel = &cancel_;
    Manifest manifest;
    if (!fetch(manifestUrl_, t, overridePrefix_) || !parseManifest(body, manifest))
        return state_.store(State::Idle, std::memory_order_release);

    auto allowed = [&](const std::string& url) {
        return allowedUrl(url, kReleasePrefix) || (!overridePrefix_.empty() && allowedUrl(url, overridePrefix_));
    };

    auto consider = [&](const std::string& name, const std::string& localId, const std::string& localVersion,
                        const char* label, bool decideByContent) {
        auto it = manifest.components.find(name);
        const Component* remote = it == manifest.components.end() ? nullptr : &it->second;
        auto installed = [&](const File& f) { return sha256File(destination(name, f.name)); };
        const Verdict verdict = decideByContent ? decideBySha(remote, installed) : decide(localId, localVersion, remote);
        if (verdict != Verdict::Offer) return;
        for (const File& f : remote->files)
            if (destination(name, f.name).empty() || !allowed(f.url)) return;
        const auto files = filesToFetch(*remote, installed);
        if (files.empty()) return;
        for (const File& f : files) jobs_.push_back({destination(name, f.name), f});
        summary_.push_back(std::string(label) + " " + remote->version);
        if (name == "switch-sysmodule") moduleChanged_ = true;
    };

    std::string moduleId, moduleVersion, idText;
    bool moduleByContent = false;
    if (readFile(kIdPath, idText, 256)) {
        if (!parseLocalId(idText, moduleId, moduleVersion)) moduleId.clear();
    } else if (exists(kExefsPath)) {
        moduleByContent = true;
        moduleVersion = "1.0.0";
    }
    consider("switch-sysmodule", moduleId, moduleVersion, "NXStream module", moduleByContent);
    consider("switch-manager", NXSTREAM_CONTENT_ID, NXSTREAM_VERSION, "NXStream app", false);

    std::stable_sort(jobs_.begin(), jobs_.end(),
                     [](const Job& a, const Job& b) { return commitRank(a.dest) < commitRank(b.dest); });
    state_.store(jobs_.empty() ? State::Idle : State::Offer, std::memory_order_release);
}

bool Updater::download(const Job& job, uint64_t& done, uint64_t total) {
    const std::string tmp = job.dest + ".tmp";
    Transfer t;
    t.file = std::fopen(tmp.c_str(), "wb");
    if (!t.file) {
        reason_ = "cannot write to the SD card";
        return false;
    }
    sha256ContextCreate(&t.sha);
    t.limit = job.file.size;
    t.base = done;
    t.total = total;
    t.cancel = &cancel_;
    t.percent = &percent_;
    const bool ok = fetch(job.file.url, t, overridePrefix_);
    const bool closed = std::fclose(t.file) == 0;
    if (!ok) {
        reason_ = t.untrusted ? "untrusted download host" : "download error";
        return false;
    }
    if (!closed) {
        reason_ = "cannot write to the SD card";
        return false;
    }
    if (t.received != job.file.size) {
        reason_ = "wrong file size";
        return false;
    }
    u8 hash[SHA256_HASH_SIZE];
    sha256ContextGetHash(&t.sha, hash);
    if (hex(hash) != job.file.sha256) {
        reason_ = "checksum mismatch";
        return false;
    }
    done += job.file.size;
    return true;
}

void Updater::apply() {
    uint64_t total = 0;
    for (const Job& j : jobs_) total += j.file.size;
    uint64_t done = 0;
    mkdir(kManagerDir, 0777);

    bool ok = true;
    for (const Job& j : jobs_) {
        if (!download(j, done, total)) {
            ok = false;
            break;
        }
    }
    if (!ok) {
        for (const Job& j : jobs_) std::remove((j.dest + ".tmp").c_str());
        return state_.store(State::Failed, std::memory_order_release);
    }

    struct Committed {
        std::string dest;
        bool hadOld;
    };
    std::vector<Committed> committed;
    auto rollback = [&]() {
        for (auto it = committed.rbegin(); it != committed.rend(); ++it) {
            std::remove(it->dest.c_str());
            if (it->hadOld) std::rename((it->dest + ".bak").c_str(), it->dest.c_str());
        }
        for (const Job& k : jobs_) std::remove((k.dest + ".tmp").c_str());
    };
    for (const Job& j : jobs_) {
        const std::string tmp = j.dest + ".tmp";
        const std::string bak = j.dest + ".bak";
        std::remove(bak.c_str());
        const bool hadOld = exists(j.dest);
        if (hadOld && std::rename(j.dest.c_str(), bak.c_str()) != 0) {
            rollback();
            fail("cannot write to the SD card");
            return;
        }
        if (std::rename(tmp.c_str(), j.dest.c_str()) != 0) {
            if (hadOld) std::rename(bak.c_str(), j.dest.c_str());
            rollback();
            fail("cannot write to the SD card");
            return;
        }
        committed.push_back({j.dest, hadOld});
    }
    for (const Committed& c : committed) std::remove((c.dest + ".bak").c_str());
    percent_ = 100;
    state_.store(State::Done, std::memory_order_release);
}
}
