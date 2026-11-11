#pragma once
#include <cstddef>
#include <cstring>
#include <cstdio>
#include "shot_buttons.hpp"

namespace ams::mitm::applet::armfile {

    inline bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

    inline bool FindToken(const char *buf, size_t n, const char *key, const char **val, size_t *val_len) {
        const size_t klen = std::strlen(key);
        if (klen == 0) { return false; }
        for (size_t i = 0; i < n; ) {
            while (i < n && IsSpace(buf[i])) { ++i; }
            size_t j = i;
            while (j < n && !IsSpace(buf[j]) && buf[j] != '\0') { ++j; }
            if (j > i) {
                size_t name_len = j - i;
                for (size_t k = i; k < j; ++k) { if (buf[k] == '=') { name_len = k - i; break; } }
                if (name_len == klen && std::memcmp(buf + i, key, klen) == 0) {
                    const size_t v = (i + name_len < j) ? i + name_len + 1 : j;
                    if (val != nullptr)     { *val = buf + v; }
                    if (val_len != nullptr) { *val_len = j - v; }
                    return true;
                }
            }
            if (j < n && buf[j] == '\0') { break; }
            i = j;
        }
        return false;
    }

    inline bool Contains(const char *buf, size_t n, const char *key) {
        return FindToken(buf, n, key, nullptr, nullptr);
    }

    inline unsigned Number(const char *buf, size_t n, const char *key, unsigned def) {
        const char *v = nullptr;
        size_t len = 0;
        if (!FindToken(buf, n, key, &v, &len) || len == 0) { return def; }
        unsigned out = 0;
        for (size_t i = 0; i < len; ++i) {
            if (v[i] < '0' || v[i] > '9') { return def; }
            out = out * 10 + static_cast<unsigned>(v[i] - '0');
        }
        return out;
    }

    inline bool IniValue(const char *ini, size_t n, const char *key, char *out, size_t cap) {
        const size_t klen = std::strlen(key);
        for (size_t i = 0; i < n; ) {
            size_t e = i;
            while (e < n && ini[e] != '\n' && ini[e] != '\0') { ++e; }
            size_t a = i;
            while (a < e && IsSpace(ini[a])) { ++a; }
            if (a < e && ini[a] != '#' && ini[a] != ';' && ini[a] != '[') {
                size_t eq = a;
                while (eq < e && ini[eq] != '=') { ++eq; }
                size_t kend = eq;
                while (kend > a && IsSpace(ini[kend - 1])) { --kend; }
                if (eq < e && kend - a == klen && std::memcmp(ini + a, key, klen) == 0) {
                    size_t v = eq + 1, ve = e;
                    while (v < ve && IsSpace(ini[v])) { ++v; }
                    while (ve > v && IsSpace(ini[ve - 1])) { --ve; }
                    if (ve - v >= cap) { return false; }
                    std::memcpy(out, ini + v, ve - v);
                    out[ve - v] = '\0';
                    return true;
                }
            }
            if (e < n && ini[e] == '\0') { break; }
            i = e + 1;
        }
        return false;
    }

    inline bool ParseUnsigned(const char *s, unsigned *out) {
        if (*s == '\0') { return false; }
        unsigned v = 0;
        for (; *s != '\0'; ++s) {
            if (*s < '0' || *s > '9' || v > 100000u) { return false; }
            v = v * 10 + static_cast<unsigned>(*s - '0');
        }
        *out = v;
        return true;
    }

    inline size_t BuildReleaseArm(const char *ini, size_t n, char *out, size_t cap) {
        unsigned qp = 20, gop = 60, wait = 20, u = 0;
        unsigned shotkey = 0;
        bool cap720 = false, anyfw = false, shot = false, audio = true, uvc = false, uvcraw = false, nvab = false, net = false, nohb = false, nodethome = false, shotalbum = false, screenoff = false, earlyoff = false, earlyab = false, midoff = false, midon = false, midab = false, homemenu = false, overlays = false, hbdocked = false, forcedock = false, vinput = false, scale1080 = false, memboost = false;
        unsigned maxfps = 0;
        char v[32];
        if (IniValue(ini, n, "quality", v, sizeof(v))) {
            if (std::strcmp(v, "medium") == 0) { qp = 24; }
            else if (std::strcmp(v, "low") == 0) { qp = 28; }
        }
        if (IniValue(ini, n, "qp", v, sizeof(v)) && ParseUnsigned(v, &u) && u >= 10 && u <= 40) { qp = u; }
        if (IniValue(ini, n, "keyframe_interval", v, sizeof(v)) && ParseUnsigned(v, &u) && u >= 1 && u <= 250) { gop = u; }
        int usbgop = -1;
        if (IniValue(ini, n, "usb_keyframe_interval", v, sizeof(v)) && ParseUnsigned(v, &u) && u <= 250) { usbgop = static_cast<int>(u); }
        if (IniValue(ini, n, "max_resolution", v, sizeof(v)) && std::strcmp(v, "720") == 0) { cap720 = true; }
        if (IniValue(ini, n, "start_delay", v, sizeof(v)) && ParseUnsigned(v, &u) && u <= 600) { wait = u; }
        if (IniValue(ini, n, "allow_untested_firmware", v, sizeof(v)) && std::strcmp(v, "1") == 0) { anyfw = true; }
        if (IniValue(ini, n, "screenshot", v, sizeof(v)) && std::strcmp(v, "1") == 0) { shot = true; }
        if (IniValue(ini, n, "screenshot_buttons", v, sizeof(v)) && ParseUnsigned(v, &u) && u <= 4) { shotkey = u; }
        unsigned shotmask = 0;
        bool shotcap = false;
        {
            char cv[128];
            if (IniValue(ini, n, "screenshot_custom", cv, sizeof(cv))) { shotmask = sft::shotbuttons::Parse(cv, &shotcap); }
        }
        if (shotkey == 4 && shotmask == 0 && !shotcap) { shotkey = 0; }
        if (IniValue(ini, n, "screenshot_location", v, sizeof(v)) && std::strcmp(v, "album") == 0) { shotalbum = true; }
        if (IniValue(ini, n, "audio", v, sizeof(v)) && std::strcmp(v, "0") == 0) { audio = false; }
        if (IniValue(ini, n, "usb_mode", v, sizeof(v)) && std::strcmp(v, "webcam") == 0) { uvc = true; }
        if (IniValue(ini, n, "encoder_test", v, sizeof(v)) && std::strcmp(v, "1") == 0) { nvab = true; }
        if (IniValue(ini, n, "network", v, sizeof(v)) && std::strcmp(v, "1") == 0) { net = true; uvc = false; }
        if (IniValue(ini, n, "homebrew", v, sizeof(v)) && std::strcmp(v, "0") == 0) { nohb = true; }
        if (IniValue(ini, n, "detach_on_home", v, sizeof(v)) && std::strcmp(v, "0") == 0) { nodethome = true; }
        if (IniValue(ini, n, "screen_off", v, sizeof(v)) && std::strcmp(v, "1") == 0) { screenoff = true; }
        if (IniValue(ini, n, "home_menu", v, sizeof(v)) && std::strcmp(v, "1") == 0) { homemenu = true; }
        if (IniValue(ini, n, "overlays", v, sizeof(v)) && std::strcmp(v, "1") == 0) { overlays = true; }
        if (IniValue(ini, n, "homebrew_docked", v, sizeof(v)) && std::strcmp(v, "1") == 0) { hbdocked = true; }
        if (IniValue(ini, n, "force_docked", v, sizeof(v)) && std::strcmp(v, "1") == 0) { forcedock = true; }
        if (IniValue(ini, n, "viewer_input", v, sizeof(v)) && std::strcmp(v, "1") == 0) { vinput = true; }
        if (IniValue(ini, n, "scale_to_1080", v, sizeof(v)) && std::strcmp(v, "1") == 0) { scale1080 = true; }
        if (IniValue(ini, n, "memory_boost", v, sizeof(v)) && std::strcmp(v, "1") == 0) { memboost = true; }
        if (IniValue(ini, n, "max_fps", v, sizeof(v)) && ParseUnsigned(v, &u) && u >= 1 && u <= 60) { maxfps = u; }
        if (IniValue(ini, n, "early_collect", v, sizeof(v))) {
            if (std::strcmp(v, "0") == 0) { earlyoff = true; }
            else if (std::strcmp(v, "ab") == 0) { earlyab = true; }
        }
        if (IniValue(ini, n, "mid_collect", v, sizeof(v))) {
            if (std::strcmp(v, "0") == 0) { midoff = true; }
            else if (std::strcmp(v, "1") == 0) { midon = true; }
            else if (std::strcmp(v, "ab") == 0) { midab = true; }
        }
        if (IniValue(ini, n, "connection", v, sizeof(v))) {
            if (std::strcmp(v, "network") == 0) { net = true; uvc = uvcraw = false; }
            else if (std::strcmp(v, "webcam") == 0) { uvc = true; uvcraw = false; net = false; }
            else if (std::strcmp(v, "webcam-any") == 0) { uvc = true; uvcraw = true; net = false; }
            else if (std::strcmp(v, "usb") == 0) { uvc = uvcraw = false; net = false; }
        }
        char shotbuf[72] = "";
        if (shot) {
            char custom[40] = "";
            if (shotkey == 4) { std::snprintf(custom, sizeof(custom), " shotmask=%u%s", shotmask, shotcap ? " shotcap" : ""); }
            std::snprintf(shotbuf, sizeof(shotbuf), " shot shotkey=%u%s%s", shotkey, custom, shotalbum ? " shotalbum" : "");
        }
        char usbgopbuf[24] = "";
        if (usbgop >= 0) { std::snprintf(usbgopbuf, sizeof(usbgopbuf), " usbgop=%d", usbgop); }
        char maxfpsbuf[16] = "";
        if (maxfps != 0) { std::snprintf(maxfpsbuf, sizeof(maxfpsbuf), " maxfps=%u", maxfps); }
        const int w = std::snprintf(out, cap, "vic exec dbg %s live nvqp=%u nvgop=%u wait=%u%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s",
                                    net ? "net" : "usb", qp, gop, wait, cap720 ? " cap720" : "", anyfw ? " anyfw" : "", shotbuf, audio ? " audio" : "",
                                    uvc ? " uvc" : "", uvcraw ? " uvcraw" : "", nvab ? " nvab" : "", nohb ? " nohb" : "",
                                    nodethome ? " nodethome" : "", screenoff ? " screenoff" : "",
                                    earlyoff ? " earlyoff" : "", earlyab ? " earlyab" : "", midoff ? " midoff" : midon ? " midon" : midab ? " midab" : "", homemenu ? " homemenu" : "", overlays ? " overlays" : "",
                                    hbdocked ? " hbdocked" : "", forcedock ? " forcedock" : "", vinput ? " vinput" : "", scale1080 ? " scale1080" : "", memboost ? " memboost" : "", usbgopbuf, maxfpsbuf);
        return (w < 0 || static_cast<size_t>(w) >= cap) ? 0 : static_cast<size_t>(w);
    }

}
