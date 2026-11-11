#include <switch.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "gfx.hpp"
#include "module.hpp"
#include "update.hpp"

namespace {
using gfx::Align;
using gfx::Font;
using gfx::rgba;

constexpr SDL_Color kBackground = rgba(30, 30, 34);
constexpr SDL_Color kSidebar = rgba(24, 24, 28);
constexpr SDL_Color kLine = rgba(60, 60, 68);
constexpr SDL_Color kSelection = rgba(46, 46, 54);
constexpr SDL_Color kAccent = rgba(18, 196, 255);
constexpr SDL_Color kText = rgba(235, 235, 240);
constexpr SDL_Color kMuted = rgba(150, 150, 160);
constexpr SDL_Color kGood = rgba(80, 210, 140);
constexpr SDL_Color kWarn = rgba(240, 180, 60);
constexpr SDL_Color kBad = rgba(240, 100, 90);

struct Preset {
    const char* name;
    int mbps;
    int fps;
    const char* help;
};

const std::array<Preset, 5> kPresets = {{
    {"Standard", 5, 30, "Like the console's clips. Works on any Wi-Fi."},
    {"High", 10, 30, "Recommended. Clean picture on a good Wi-Fi."},
    {"Very high", 15, 30, "Very clean picture. Needs strong Wi-Fi, 5 GHz is best."},
    {"High 60", 10, 60, "60 fps on a good Wi-Fi; heavy games may slow down."},
    {"Very high 60", 15, 60, "60 fps, needs strong 5 GHz Wi-Fi close to the router."},
}};

constexpr std::array<std::array<int, 2>, 5> kPickProfile = {{{5, 30}, {10, 30}, {10, 60}, {15, 30}, {15, 60}}};
constexpr uint32_t kProbeTimeoutFrames = 5400;
constexpr uint32_t kProbePollFrames = 30;
constexpr uint32_t kProbeDeadlineFrames = 19800;

enum class Probe { Idle, Running, Done, Failed };

constexpr int kProfileKeyframeMs = 500;
constexpr std::array<int, 4> kKeyframeMs = {250, 500, 1000, 2000};
constexpr std::array<int, 7> kHomeFps = {0, 5, 10, 15, 20, 25, 30};
constexpr std::array<int, 7> kHomeMbps = {0, 5, 10, 15, 20, 30, 50};

enum class Item {
    ModuleState, Streaming, Viewer, Ip, ConsoleName, Launch,
    Transport, Native, Probe,
    Profile, Bitrate, Fps, Keyframes, GameVideo, Audio,
    HomeCapture, HomeFps, HomeBandwidth,
    Autostart, Reset,
};

struct Category {
    const char* name;
    std::vector<Item> items;
};

const std::array<Category, 5> kCategories = {{
    {"Status", {Item::ModuleState, Item::Streaming, Item::Viewer, Item::Ip, Item::ConsoleName, Item::Launch}},
    {"Connection", {Item::Transport, Item::Native, Item::Probe}},
    {"Games", {Item::Profile, Item::Bitrate, Item::Fps, Item::Keyframes, Item::GameVideo, Item::Audio}},
    {"HOME", {Item::HomeCapture, Item::HomeFps, Item::HomeBandwidth}},
    {"System", {Item::Autostart, Item::Reset}},
}};

bool adjustable(Item item) {
    return item == Item::Transport || item == Item::Native || item == Item::Profile || item == Item::Bitrate || item == Item::Fps || item == Item::Keyframes ||
           item == Item::HomeFps || item == Item::HomeBandwidth;
}

const char* label(Item item) {
    switch (item) {
        case Item::ModuleState: return "Module";
        case Item::Streaming: return "Streaming";
        case Item::Viewer: return "PC connected";
        case Item::Ip: return "IP address";
        case Item::ConsoleName: return "Console name";
        case Item::Launch: return "Start module";
        case Item::Profile: return "Video quality preset";
        case Item::Transport: return "Connection";
        case Item::Native: return "Resolution";
        case Item::Probe: return "Auto-detect video quality";
        case Item::Bitrate: return "Quality (bitrate)";
        case Item::Fps: return "Frames per second";
        case Item::Keyframes: return "Full frame every";
        case Item::GameVideo: return "Game video";
        case Item::Audio: return "Game audio";
        case Item::HomeCapture: return "HOME and system menus";
        case Item::HomeFps: return "HOME frame rate";
        case Item::HomeBandwidth: return "HOME max bandwidth";
        case Item::Autostart: return "Auto-start";
        case Item::Reset: return "Reset to defaults";
    }
    return "";
}

std::string consoleName() {
    std::string name = "Nintendo Switch";
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysDeviceNickName nick{};
        if (R_SUCCEEDED(setsysGetDeviceNickname(&nick)) && nick.nickname[0]) name = nick.nickname;
        setsysExit();
    }
    return name;
}

template <size_t N>
int step(const std::array<int, N>& values, int current, int direction) {
    int i = 0;
    for (int k = 1; k < int(N); ++k)
        if (std::abs(values[k] - current) < std::abs(values[i] - current)) i = k;
    return direction == 0 ? values[(i + 1) % N] : values[std::clamp(i + direction, 0, int(N) - 1)];
}

constexpr int kRow = 64;
constexpr int kArrowSlot = 28;
constexpr int kHintBand = 56;
constexpr SDL_Rect kProbePanel{240, 200, 800, 320};
constexpr const char* kUpdateApply = "(A) Update";
constexpr const char* kUpdateCancel = "(B) Cancel";
constexpr const char* kProbeApply = "(A) Apply";
constexpr const char* kProbeCancel = "(B) Cancel";

SDL_Rect sidebarRow(int i) { return SDL_Rect{0, 110 + i * 64, 360, 64}; }
SDL_Rect itemRow(int i) { return SDL_Rect{400, 104 + i * kRow, 840, kRow}; }
int valueRight(const SDL_Rect& row) { return row.x + row.w - 24; }
int leftArrowRight(const SDL_Rect& row, int valueWidth) { return valueRight(row) - kArrowSlot - valueWidth - 12; }
SDL_Rect updatePanel(int lines) {
    const int h = 190 + lines * 48;
    return SDL_Rect{240, (gfx::kHeight - h) / 2, 800, h};
}
SDL_Rect footerBand(const SDL_Rect& panel) { return SDL_Rect{panel.x, panel.y + panel.h - kHintBand - 10, panel.w, kHintBand}; }
bool inside(const SDL_Rect& r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
std::string hint(const char* a, const char* b) { return std::string(a) + "    " + b; }

class App {
public:
    explicit App(const char* argv0) : argv0_(argv0 ? argv0 : "") {}

    int run() {
        if (!canvas_.init()) return 1;
        romfsInit();
        logo_ = canvas_.loadImage("romfs:/logo.png");
        module::init();
        update::init();
        updater_ = std::make_unique<update::Updater>(argv0_.c_str());
        updater_->startCheck();
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&pad_);
        hidInitializeTouchScreen();

        settings_ = module::Settings::load();
        mbps_ = static_cast<int>(
            (module::recorderSetting(module::kBitrateKey, module::kDefaultBitrate) + 500000) / 1000000);
        fps_ = static_cast<int>(module::recorderSetting(module::kFpsKey, module::kDefaultFps));
        keyframeMs_ =
            static_cast<int>(module::recorderSetting(module::kKeyframeKey, module::kDefaultKeyframes)) * 1000 /
            std::max(fps_, 1);
        readActiveRecorder();
        name_ = consoleName();
        status_ = module::query();
        if (settings_.native) {
            bool changed = false;
            if (module::refreshTapConfig(settings_.usb, changed) && changed)
                message("Restart the console to apply.", kAccent);
        }

        while (appletMainLoop() && input()) {
            pollUpdate();
            if (++frame_ % 60 == 0) status_ = module::query();
            if (probe_ == Probe::Running && frame_ % kProbePollFrames == 0) pollProbe();
            draw();
        }

        updater_.reset();
        update::exit();
        module::exit();
        if (logo_) SDL_DestroyTexture(logo_);
        romfsExit();
        canvas_.shutdown();
        return 0;
    }

private:
    const char* lockedReason(Item item) const {
        if (settings_.native &&
            (item == Item::Probe || item == Item::Profile || item == Item::Bitrate || item == Item::Fps || item == Item::Keyframes))
            return item == Item::Fps ? "1080p streams at 30 fps." : "Not used with 1080p.";
        if (settings_.usb &&
            (item == Item::Ip || item == Item::Probe || item == Item::Profile || item == Item::Bitrate || item == Item::HomeBandwidth))
            return "Not used with USB.";
        return nullptr;
    }

    std::vector<Item> items() const {
        return kCategories[category_].items;
    }

    bool input() {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_QUIT) return false;

        padUpdate(&pad_);
        const u64 down = padGetButtonsDown(&pad_);
        const u64 held = padGetButtons(&pad_);
        if (down & HidNpadButton_Plus) return false;

        HidTouchScreenState touch{};
        hidGetTouchScreenStates(&touch, 1);
        const bool tap = touch.count > 0 && touchCount_ == 0;
        touchCount_ = touch.count;
        const int tx = tap ? int(touch.touches[0].x) : 0;
        const int ty = tap ? int(touch.touches[0].y) : 0;

        const update::State us = updater_->state();
        if (us == update::State::Offer && !popupOpen_) {
            bool a = down & HidNpadButton_A;
            bool b = down & HidNpadButton_B;
            if (tap) {
                const SDL_Rect panel = updatePanel(int(updater_->summary().size()));
                if (inside(footerBand(panel), tx, ty)) a = firstHintTapped(tx, panel.x + panel.w - 40, kUpdateApply, kUpdateCancel);
                b = !a && !(inside(panel, tx, ty) && !inside(footerBand(panel), tx, ty));
            }
            if (a) updater_->startApply();
            else if (b) updater_->dismiss();
            return true;
        }
        if (us == update::State::Downloading) return true;

        if (popupOpen_) {
            bool a = down & HidNpadButton_A;
            bool b = down & HidNpadButton_B;
            if (tap) {
                const SDL_Rect band = footerBand(kProbePanel);
                if (inside(band, tx, ty))
                    a = probe_ == Probe::Done &&
                        firstHintTapped(tx, kProbePanel.x + kProbePanel.w - 48, kProbeApply, kProbeCancel);
                b = !a && !(inside(kProbePanel, tx, ty) && !inside(band, tx, ty));
            }
            if (probe_ == Probe::Done && a) applyProbe();
            else if (b) {
                if (probe_ != Probe::Running) probe_ = Probe::Idle;
            } else {
                return true;
            }
            popupOpen_ = false;
            return true;
        }

        if (tap) {
            touchSelect(tx, ty);
            return true;
        }

        const int up = (down & HidNpadButton_AnyUp) ? -1 : (down & HidNpadButton_AnyDown) ? 1 : 0;
        bool left = down & HidNpadButton_AnyLeft;
        bool right = down & HidNpadButton_AnyRight;
        if (held & (HidNpadButton_AnyLeft | HidNpadButton_AnyRight)) {
            if (++repeat_ > 24 && repeat_ % 6 == 0) {
                left = held & HidNpadButton_AnyLeft;
                right = held & HidNpadButton_AnyRight;
            }
        } else {
            repeat_ = 0;
        }

        if (!inItems_) {
            if (up) category_ = std::clamp(category_ + up, 0, int(kCategories.size()) - 1);
            if ((down & HidNpadButton_AnyRight) || (down & HidNpadButton_A)) {
                inItems_ = true;
                item_ = 0;
                repeat_ = 0;
            }
            if (down & HidNpadButton_B) return false;
            return true;
        }

        const std::vector<Item> list = items();
        item_ = std::clamp(item_ + up, 0, int(list.size()) - 1);
        if (down & HidNpadButton_B) {
            inItems_ = false;
            repeat_ = 0;
        }
        if (lockedReason(list[item_])) {
            if (down & HidNpadButton_AnyLeft) {
                inItems_ = false;
                repeat_ = 0;
            }
            return true;
        }
        if (down & HidNpadButton_A) change(list[item_], 0);
        if (left || right) {
            const Item item = list[item_];
            if (adjustable(item)) change(item, right ? 1 : -1);
            else if (down & HidNpadButton_AnyLeft) {
                inItems_ = false;
                repeat_ = 0;
            }
        }
        return true;
    }

    bool firstHintTapped(int x, int right, const char* a, const char* b) {
        const int wa = canvas_.measure(a, Font::Small);
        const int wb = canvas_.measure(b, Font::Small);
        const int total = canvas_.measure(hint(a, b), Font::Small);
        return x < right - total + wa + (total - wa - wb) / 2;
    }

    void touchSelect(int x, int y) {
        for (int i = 0; i < int(kCategories.size()); ++i) {
            if (!inside(sidebarRow(i), x, y)) continue;
            category_ = i;
            item_ = 0;
            inItems_ = false;
            repeat_ = 0;
            return;
        }
        const std::vector<Item> list = items();
        for (int i = 0; i < int(list.size()); ++i) {
            const SDL_Rect row = itemRow(i);
            if (!inside(row, x, y)) continue;
            inItems_ = true;
            item_ = i;
            repeat_ = 0;
            const Item item = list[i];
            if (lockedReason(item)) return;
            if (!adjustable(item)) {
                change(item, 0);
                return;
            }
            SDL_Color c;
            const int leftEdge = leftArrowRight(row, canvas_.measure(value(item, c), Font::Body));
            if (x >= valueRight(row) - kArrowSlot - 16) {
                if (canMove(item, 1)) change(item, 1);
            } else if (x >= leftEdge - 48 && x < leftEdge + 12) {
                if (canMove(item, -1)) change(item, -1);
            }
            return;
        }
    }

    void pollUpdate() {
        const update::State s = updater_->state();
        if (s == update::State::Done) {
            message(updater_->moduleChanged() ? "Updated. Restart the console to apply."
                                              : "Updated. Reopen NXStream to apply.",
                    kGood);
            messageUntil_ = frame_ + 600;
            updater_->dismiss();
        } else if (s == update::State::Failed) {
            message(("Update failed: " + updater_->reason()).c_str(), kBad);
            messageUntil_ = frame_ + 420;
            updater_->dismiss();
        }
    }

    void drawUpdate() {
        const update::State s = updater_->state();
        if ((s != update::State::Offer || popupOpen_) && s != update::State::Downloading) return;
        SDL_Renderer* r = canvas_.renderer();
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        fill(SDL_Rect{0, 0, gfx::kWidth, gfx::kHeight}, rgba(0, 0, 0, 170));
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

        const auto& lines = updater_->summary();
        const SDL_Rect panel = updatePanel(int(lines.size()));
        fill(panel, kBackground);
        fill(SDL_Rect{panel.x, panel.y, panel.w, 4}, kAccent);
        canvas_.text("Update available", panel.x + 40, panel.y + 32, Font::Large, kText);
        for (size_t i = 0; i < lines.size(); ++i)
            canvas_.text(lines[i], panel.x + 40, panel.y + 104 + int(i) * 48, Font::Body, kText);
        const int footer = panel.y + panel.h - 56;
        if (s == update::State::Offer) {
            canvas_.text(hint(kUpdateApply, kUpdateCancel), panel.x + panel.w - 40, footer, Font::Small, kMuted, Align::Right);
        } else {
            char buf[48];
            std::snprintf(buf, sizeof buf, "Downloading update... %d%%", updater_->percent());
            canvas_.text(buf, panel.x + 40, footer, Font::Small, kAccent);
        }
    }

    void readActiveRecorder() {
        if (R_FAILED(setsysInitialize())) return;
        auto read = [](const char* key, uint32_t fallback) {
            u32 value = 0;
            u64 size = 0;
            if (R_FAILED(setsysGetSettingsItemValue("am.debug", key, &value, sizeof value, &size)) || value == 0)
                return fallback;
            return static_cast<uint32_t>(value);
        };
        activeMbps_ = static_cast<int>((read(module::kBitrateKey, module::kDefaultBitrate) + 500000) / 1000000);
        activeFps_ = static_cast<int>(read(module::kFpsKey, module::kDefaultFps));
        activeKeyframeMs_ =
            static_cast<int>(read(module::kKeyframeKey, module::kDefaultKeyframes)) * 1000 / std::max(activeFps_, 1);
        setsysExit();
    }

    int currentProfile() const {
        for (size_t i = 0; i < kPresets.size(); ++i)
            if (kPresets[i].mbps == mbps_ && kPresets[i].fps == fps_ && keyframeMs_ == kProfileKeyframeMs)
                return int(i);
        return -1;
    }

    bool writeRecorder() {
        const bool ok =
            module::setRecorderSettings(static_cast<uint32_t>(mbps_) * 1000000u, static_cast<uint32_t>(fps_),
                                        static_cast<uint32_t>(std::max(1, fps_ * keyframeMs_ / 1000)));
        if (ok) message("Restart the console to apply", kAccent);
        else message("Cannot write atmosphere/config/system_settings.ini", kBad);
        return ok;
    }

    int pickPreset(int pick) const {
        const int idx = pick < 0 || pick >= int(kPickProfile.size()) ? 0 : pick;
        for (size_t i = 0; i < kPresets.size(); ++i)
            if (kPresets[i].mbps == kPickProfile[idx][0] && kPresets[i].fps == kPickProfile[idx][1]) return int(i);
        return 0;
    }

    void applyProbe() {
        if (lockedReason(Item::Probe)) {
            probe_ = Probe::Idle;
            popupOpen_ = false;
            return;
        }
        const int p = pickPreset(probePick_);
        mbps_ = kPresets[p].mbps;
        fps_ = kPresets[p].fps;
        keyframeMs_ = kProfileKeyframeMs;
        probe_ = Probe::Idle;
        writeRecorder();
        status_ = module::query();
    }

    const char* probeFailure() const {
        return probeReason_ == "nopc"  ? "No PC viewer connected. Connect it and try again."
               : probeReason_ == "lost" ? "The PC disconnected during the test. Try again."
               : probeReason_ == "off"  ? "Streaming is off or the console is sleeping."
               : probeReason_ == "timeout" ? "No answer from the module. Try again."
                                           : "The test failed. Try again.";
    }

    std::string dots() const { return std::string(size_t(frame_ / 24 % 3 + 1), '.'); }

    void probeAction() {
        if (probe_ == Probe::Running) {
            popupOpen_ = true;
            return;
        }
        if (probe_ == Probe::Done) return applyProbe();
        status_ = module::query();
        if (!status_.running || status_.viewer != module::ViewerState::Yes) {
            probeNoViewer_ = true;
            return message("Start the PC viewer and connect it first", kWarn);
        }
        probeNoViewer_ = false;
        if (!module::requestProbe(settings_, probeId_)) return writeFailed();
        probe_ = Probe::Running;
        probeStep_ = 0;
        probeRun_ = 0;
        popupOpen_ = true;
        probeReason_.clear();
        probeChanged_ = frame_;
        probeStarted_ = frame_;
    }

    void pollProbe() {
        if (lockedReason(Item::Probe)) {
            probe_ = Probe::Idle;
            popupOpen_ = false;
            return;
        }
        const module::ProbeResult r = module::readProbeResult();
        if (r.valid && r.id == probeId_) {
            if (r.state == "done") {
                probe_ = Probe::Done;
                probePick_ = r.pick;
                probeCapacityKbps_ = r.capacityKbps;
                popupOpen_ = true;
                return;
            }
            if (r.state == "failed") {
                probe_ = Probe::Failed;
                probeReason_ = r.reason;
                popupOpen_ = true;
                return;
            }
            if (r.step != probeStep_ || r.run != probeRun_) {
                probeStep_ = r.step;
                probeRun_ = r.run;
                probeChanged_ = frame_;
            }
        }
        if (frame_ - probeChanged_ > kProbeTimeoutFrames || frame_ - probeStarted_ > kProbeDeadlineFrames) {
            probe_ = Probe::Failed;
            probeReason_ = "timeout";
            popupOpen_ = true;
        }
    }

    void change(Item item, int direction) {
        switch (item) {
            case Item::Launch:
                if (!status_.installed) return message("Module not installed on the SD card", kBad);
                if (status_.running) return message("The module is already running", kMuted);
                module::launch() ? message("Module started", kGood) : message("Start failed", kBad);
                break;
            case Item::Transport: {
                const bool usb = direction == 0 ? !settings_.usb : direction > 0;
                if (usb == settings_.usb) return;
                uint32_t bitrate = 0;
                if (!module::setTransport(settings_, usb, static_cast<uint32_t>(mbps_) * 1000000u, bitrate))
                    return writeFailed();
                mbps_ = static_cast<int>(bitrate / 1000000);
                if (writeRecorder()) message("Restart the console to apply the new video quality.", kAccent);
                messageUntil_ = frame_ + 420;
                break;
            }
            case Item::Native: {
                const bool native = direction == 0 ? !settings_.native : direction > 0;
                if (native == settings_.native) return;
                if (native && !module::nativeFilesPresent()) {
                    message("1080p module files are missing on the SD card.", kBad);
                    messageUntil_ = frame_ + 420;
                    return;
                }
                if (!module::setNative(settings_, native)) return writeFailed();
                message("Restart the console to switch capture mode.", kAccent);
                messageUntil_ = frame_ + 420;
                break;
            }
            case Item::Probe: probeAction(); return;
            case Item::Profile: {
                const int n = int(kPresets.size());
                const int from = currentProfile();
                const int next = from < 0 ? (direction < 0 ? n - 1 : 0)
                                          : direction == 0 ? (from + 1) % n : std::clamp(from + direction, 0, n - 1);
                if (next == from) return;
                mbps_ = kPresets[next].mbps;
                fps_ = kPresets[next].fps;
                keyframeMs_ = kProfileKeyframeMs;
                writeRecorder();
                break;
            }
            case Item::Bitrate: {
                const int next = std::clamp(mbps_ + (direction == 0 ? 1 : direction), 2, 30);
                if (next == mbps_) return;
                mbps_ = next;
                writeRecorder();
                break;
            }
            case Item::Fps:
                fps_ = fps_ == 60 ? 30 : 60;
                writeRecorder();
                break;
            case Item::Keyframes: {
                const int next = step(kKeyframeMs, keyframeMs_, direction);
                if (next == keyframeMs_) return;
                keyframeMs_ = next;
                writeRecorder();
                break;
            }
            case Item::Streaming: settings_.enabled = !settings_.enabled; save(); break;
            case Item::GameVideo: settings_.gameCapture = !settings_.gameCapture; save(); break;
            case Item::Audio: settings_.audio = !settings_.audio; save(); break;
            case Item::HomeCapture: settings_.screenCapture = !settings_.screenCapture; save(); break;
            case Item::HomeFps: {
                const int next = step(kHomeFps, settings_.screenFps, direction);
                if (next == settings_.screenFps) return;
                settings_.screenFps = next;
                save();
                break;
            }
            case Item::HomeBandwidth: {
                const int next = step(kHomeMbps, settings_.screenKbps / 1000, direction);
                if (next * 1000 == settings_.screenKbps) return;
                settings_.screenKbps = next * 1000;
                save();
                break;
            }
            case Item::Autostart:
                if (!status_.installed) return message("Module not installed on the SD card", kBad);
                if (!module::setAutostart(!status_.autostart)) return message("Cannot write to the SD card", kBad);
                break;
            case Item::Reset: {
                const bool usb = settings_.usb;
                const bool native = settings_.native;
                settings_ = module::Settings{};
                settings_.usb = usb;
                settings_.native = native;
                save();
                mbps_ = usb ? 30 : 10;
                fps_ = 30;
                keyframeMs_ = kProfileKeyframeMs;
                if (writeRecorder()) message("Defaults restored. Restart the console for the video quality", kGood);
                break;
            }
            default:
                return;
        }
        status_ = module::query();
    }

    void save() {
        if (!settings_.save()) writeFailed();
    }

    void writeFailed() {
        std::string text = "Cannot write to the SD card";
        if (module::lastWriteError()[0]) text += std::string(" (") + module::lastWriteError() + ")";
        message(text.c_str(), kBad);
    }

    void message(const char* text, SDL_Color c) {
        message_ = text;
        messageColor_ = c;
        messageUntil_ = frame_ + 180;
    }

    const char* help(Item item) {
        if (const char* reason = lockedReason(item)) return reason;
        switch (item) {
            case Item::Streaming:
                return "Off: stops capture at once. Turn off Auto-start to keep it off at boot.";
            case Item::Ip: return "Type this in NXStream on the PC if the console is not found.";
            case Item::Launch: return "Starts NXStream without restarting the console.";
            case Item::Profile: {
                const int p = currentProfile();
                if (mbps_ > 15) return "Above 15 Mbps Wi-Fi may stutter. Pick a preset or lower it.";
                return p < 0 ? "Set by hand. Pick a preset to set all three values at once." : kPresets[p].help;
            }
            case Item::Probe:
                if (probe_ == Probe::Running) return "Testing the network. Press A to show the progress.";
                if (probe_ == Probe::Done) {
                    probeHelp_ = probePick_ < 0 ? "Wi-Fi too weak even for Standard. A applies Standard."
                                                : std::string("Best for this Wi-Fi: ") + kPresets[pickPreset(probePick_)].name +
                                                      ". Press A to apply.";
                    return probeHelp_.c_str();
                }
                if (probe_ == Probe::Failed) return probeFailure();
                if (probeNoViewer_) return "Start the PC viewer and connect it first.";
                return "Tests this Wi-Fi with the PC viewer. Takes up to 4 minutes.";
            case Item::Bitrate:
                return mbps_ <= 5 ? "Like the console's clips. Any Wi-Fi, but blocky in full screen."
                       : mbps_ <= 10 ? "Recommended: clean picture on a good Wi-Fi."
                       : mbps_ <= 15 ? "Very clean: needs a strong Wi-Fi, 5 GHz is best."
                                     : "Above 15 Mbps Wi-Fi may stutter. Pick a preset or lower it.";
            case Item::Fps:
                return fps_ == 60 ? "Smooth motion, heavy games may slow down."
                                  : "Like the console's clips.";
            case Item::Keyframes:
                return "More often: faster recovery from Wi-Fi drops, slightly lower quality.";
            case Item::GameVideo: return "Off: games also go through the (slow) HOME capture.";
            case Item::Audio: return "Sends game audio to the PC. HOME sounds can't be captured.";
            case Item::HomeCapture: return "Shows HOME, settings and overlays outside games.";
            case Item::HomeFps: return "Auto = same as games. The console captures 20-30 per second at most.";
            case Item::HomeBandwidth:
                return "Auto = 1.5x the game bitrate, min 10 Mbps. Higher can flood the Wi-Fi.";
            case Item::Autostart: return "The module starts by itself when you turn on the console.";
            case Item::Reset:
                return settings_.usb ? "Back to the recommended settings (30 Mbps, 30 fps)."
                                     : "Back to the recommended settings (10 Mbps, 30 fps).";
            case Item::Native:
                return settings_.native ? "Games render docked, 1080p at up to 30 fps. HOME stays 720p."
                                        : "Standard capture. Works everywhere, HOME included.";
            case Item::Transport:
                return settings_.usb ? "Stream over the USB-C cable. Undocked only. Other USB apps can't run."
                                     : "Stream over Wi-Fi or LAN. The PC finds the console on the network.";
            default: return "";
        }
    }

    bool canMove(Item item, int direction) const {
        switch (item) {
            case Item::Profile: {
                const int p = currentProfile();
                return p < 0 || (direction < 0 ? p > 0 : p < int(kPresets.size()) - 1);
            }
            case Item::Bitrate: return direction < 0 ? mbps_ > 2 : mbps_ < 30;
            case Item::Transport: return direction < 0 ? settings_.usb : !settings_.usb;
            case Item::Native: return direction < 0 ? settings_.native : !settings_.native;
            case Item::Fps: return true;
            case Item::Keyframes: return step(kKeyframeMs, keyframeMs_, direction) != keyframeMs_;
            case Item::HomeFps: return step(kHomeFps, settings_.screenFps, direction) != settings_.screenFps;
            case Item::HomeBandwidth:
                return step(kHomeMbps, settings_.screenKbps / 1000, direction) * 1000 != settings_.screenKbps;
            default: return false;
        }
    }

    std::string value(Item item, SDL_Color& c) const {
        c = kMuted;
        auto onOff = [&](bool on) {
            c = on ? kAccent : kMuted;
            return std::string(on ? "On" : "Off");
        };
        char buf[64];
        switch (item) {
            case Item::ModuleState:
                c = !status_.installed ? kBad : status_.running ? (settings_.enabled ? kGood : kWarn) : kBad;
                return !status_.installed ? "Not installed"
                       : status_.running  ? (settings_.enabled ? "Active" : "Running, streaming off")
                                          : "Not running";
            case Item::Streaming: return onOff(settings_.enabled);
            case Item::Viewer:
                return status_.viewer == module::ViewerState::Unknown ? "-"
                       : status_.viewer == module::ViewerState::Yes   ? "Yes"
                                                                      : "No";
            case Item::Ip:
                c = status_.ip.empty() ? kBad : kText;
                return status_.ip.empty() ? "No network" : status_.ip;
            case Item::ConsoleName: c = kText; return name_;
            case Item::Launch: return status_.running ? "Already running" : "";
            case Item::Profile: {
                c = kAccent;
                const int p = currentProfile();
                if (p < 0) return "Custom";
                std::snprintf(buf, sizeof buf, "%s · %d Mbps, %d fps", kPresets[p].name, kPresets[p].mbps, kPresets[p].fps);
                return buf;
            }
            case Item::Transport: c = kAccent; return settings_.usb ? "USB" : "Network";
            case Item::Native:
                if (status_.nativeFlags != status_.nativeRunning) {
                    c = kWarn;
                    return settings_.native ? "1080p · after restart" : "720p · after restart";
                }
                c = kAccent;
                return settings_.native ? "1080p" : "720p";
            case Item::Probe: {
                c = kAccent;
                if (probe_ == Probe::Running) {
                    std::snprintf(buf, sizeof buf, "Testing%s", dots().c_str());
                    return buf;
                }
                if (probe_ == Probe::Failed) return "";
                if (probe_ == Probe::Idle) return "";
                const int p = pickPreset(probePick_);
                std::snprintf(buf, sizeof buf, "%s · %d Mbps, %d fps", kPresets[p].name, kPresets[p].mbps, kPresets[p].fps);
                return buf;
            }
            case Item::Bitrate:
                if (lockedReason(item)) {
                    std::snprintf(buf, sizeof buf, "%d Mbps", mbps_);
                    return buf;
                }
                c = mbps_ != activeMbps_ ? kWarn : mbps_ > 15 ? kWarn : kAccent;
                std::snprintf(buf, sizeof buf, "%d Mbps%s%s", mbps_,
                              mbps_ == 5 ? " (console default)" : mbps_ > 15 ? " (beta)" : "",
                              mbps_ != activeMbps_ ? " · after restart" : "");
                return buf;
            case Item::Fps:
                if (settings_.native) return "30 fps";
                c = fps_ != activeFps_ ? kWarn : kAccent;
                std::snprintf(buf, sizeof buf, "%d%s", fps_, fps_ != activeFps_ ? " · after restart" : "");
                return buf;
            case Item::Keyframes:
                if (lockedReason(item)) {
                    std::snprintf(buf, sizeof buf, "%d.%02d s", keyframeMs_ / 1000, keyframeMs_ % 1000 / 10);
                    return buf;
                }
                c = keyframeMs_ != activeKeyframeMs_ ? kWarn : kAccent;
                std::snprintf(buf, sizeof buf, "%d.%02d s%s", keyframeMs_ / 1000, keyframeMs_ % 1000 / 10,
                              keyframeMs_ != activeKeyframeMs_ ? " · after restart" : "");
                return buf;
            case Item::GameVideo: return onOff(settings_.gameCapture);
            case Item::Audio: return onOff(settings_.audio);
            case Item::HomeCapture: return onOff(settings_.screenCapture);
            case Item::HomeFps:
                c = kAccent;
                if (settings_.screenFps == 0) return "Auto";
                std::snprintf(buf, sizeof buf, "%d", settings_.screenFps);
                return buf;
            case Item::HomeBandwidth:
                c = kAccent;
                if (settings_.screenKbps == 0) return "Auto";
                std::snprintf(buf, sizeof buf, "%d Mbps", settings_.screenKbps / 1000);
                return buf;
            case Item::Autostart: return onOff(status_.autostart);
            case Item::Reset: return "";
        }
        return "";
    }

    void draw() {
        SDL_Renderer* r = canvas_.renderer();
        SDL_SetRenderDrawColor(r, kBackground.r, kBackground.g, kBackground.b, 255);
        SDL_RenderClear(r);

        canvas_.image(logo_, SDL_Rect{40, 22, 48, 48});
        canvas_.text("NXStream", 104, 30, Font::Large, kText);
        fill(SDL_Rect{0, 88, gfx::kWidth, 1}, kLine);

        fill(SDL_Rect{0, 89, 360, 562}, kSidebar);
        for (int i = 0; i < int(kCategories.size()); ++i) {
            const SDL_Rect row = sidebarRow(i);
            if (i == category_) {
                fill(row, inItems_ ? kBackground : kSelection);
                fill(SDL_Rect{row.x, row.y + 12, 4, row.h - 24}, kAccent);
            }
            canvas_.text(kCategories[i].name, 48, row.y + 16, Font::Body, i == category_ ? kText : kMuted);
        }

        const std::vector<Item> list = items();
        item_ = std::clamp(item_, 0, int(list.size()) - 1);
        for (int i = 0; i < int(list.size()); ++i) {
            const SDL_Rect row = itemRow(i);
            const bool selected = inItems_ && i == item_;
            if (selected) fill(row, kSelection);
            fill(SDL_Rect{row.x, row.y + row.h - 1, row.w, 1}, kLine);
            canvas_.text(label(list[i]), row.x + 24, row.y + 16, Font::Body, lockedReason(list[i]) ? kMuted : kText);
            SDL_Color c;
            const std::string v = value(list[i], c);
            const bool locked = lockedReason(list[i]) != nullptr;
            if (locked) c = kMuted;
            if (!locked && adjustable(list[i]) && !v.empty()) {
                const int right = valueRight(row);
                if (canMove(list[i], 1)) canvas_.text(">", right, row.y + 16, Font::Body, c, Align::Right);
                const SDL_Point size = canvas_.text(v, right - kArrowSlot, row.y + 16, Font::Body, c, Align::Right);
                if (canMove(list[i], -1))
                    canvas_.text("<", leftArrowRight(row, size.x), row.y + 16, Font::Body, c, Align::Right);
            } else {
                canvas_.text(v, valueRight(row), row.y + 16, Font::Body, c, Align::Right);
            }
        }
        if (inItems_) canvas_.text(help(list[item_]), 424, 104 + int(list.size()) * kRow + 18, Font::Small, kMuted);

        fill(SDL_Rect{0, 650, gfx::kWidth, 1}, kLine);
        if (frame_ < messageUntil_) canvas_.text(message_, 40, 670, Font::Small, messageColor_);
        canvas_.text(inItems_ ? "(A) Change    (B) Back    (+) Exit" : "(A) Open    (+) Exit",
                     gfx::kWidth - 40, 670, Font::Small, kMuted, Align::Right);

        drawProbe();
        drawUpdate();
        canvas_.present();
    }

    void disc(int cx, int cy, int radius, SDL_Color c) {
        for (int dy = -radius; dy <= radius; ++dy) {
            const int dx = static_cast<int>(std::sqrt(double(radius * radius - dy * dy)));
            fill(SDL_Rect{cx - dx, cy + dy, 2 * dx + 1, 1}, c);
        }
    }

    void roundRect(SDL_Rect rect, int radius, SDL_Color c) {
        fill(SDL_Rect{rect.x + radius, rect.y, rect.w - 2 * radius, rect.h}, c);
        fill(SDL_Rect{rect.x, rect.y + radius, rect.w, rect.h - 2 * radius}, c);
        disc(rect.x + radius, rect.y + radius, radius, c);
        disc(rect.x + rect.w - radius - 1, rect.y + radius, radius, c);
        disc(rect.x + radius, rect.y + rect.h - radius - 1, radius, c);
        disc(rect.x + rect.w - radius - 1, rect.y + rect.h - radius - 1, radius, c);
    }

    void drawProbe() {
        if (!popupOpen_ || probe_ == Probe::Idle) return;
        SDL_Renderer* r = canvas_.renderer();
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        fill(SDL_Rect{0, 0, gfx::kWidth, gfx::kHeight}, rgba(0, 0, 0, 170));
        const SDL_Rect panel = kProbePanel;
        roundRect(panel, 20, kBackground);
        const bool running = probe_ == Probe::Running;
        int textX = panel.x + 48;
        if (running) {
            const int cx = panel.x + 100;
            const int cy = panel.y + panel.h / 2 - 10;
            for (int i = 0; i < 8; ++i) {
                const double a = i * 3.14159265358979 / 4;
                const int lead = int(frame_ / 5 % 8);
                const int age = (lead - i + 8) % 8;
                const SDL_Color c = rgba(kAccent.r, kAccent.g, kAccent.b, uint8_t(255 - age * 28));
                disc(cx + int(std::lround(std::sin(a) * 38)), cy - int(std::lround(std::cos(a) * 38)), 7, c);
            }
            textX = panel.x + 180;
        }
        const std::string pickValue = [&] {
            const int p = pickPreset(probePick_);
            char buf[96];
            std::snprintf(buf, sizeof buf, "%s · %d Mbps, %d fps", kPresets[p].name, kPresets[p].mbps, kPresets[p].fps);
            return std::string(buf);
        }();
        const char* title = running ? "Testing the network"
                            : probe_ == Probe::Failed ? "Test failed"
                            : probePick_ < 0 ? "Test result" : "Best for this Wi-Fi";
        canvas_.text(title, textX, panel.y + 40, Font::Large, kText);
        if (running) {
            canvas_.text("Testing" + dots(), textX, panel.y + 128, Font::Body, kAccent);
            canvas_.text("The stream on the PC pauses during the test.", textX, panel.y + 190, Font::Small, kMuted);
        } else if (probe_ == Probe::Failed) {
            canvas_.text(probeFailure(), textX, panel.y + 128, Font::Body, kBad);
        } else {
            if (probeCapacityKbps_ > 0) {
                char cap[48];
                std::snprintf(cap, sizeof cap, "Measured speed: %d.%d Mbit/s", (probeCapacityKbps_ + 50) / 1000,
                              (probeCapacityKbps_ + 50) % 1000 / 100);
                canvas_.text(cap, textX, panel.y + 190, Font::Small, kMuted);
            }
            canvas_.text(probePick_ < 0 ? "Wi-Fi too weak even for Standard." : pickValue, textX, panel.y + 128,
                         Font::Body, probePick_ < 0 ? kWarn : kAccent);
        }
        canvas_.text(running ? "(B) Hide" : probe_ == Probe::Failed ? "(B) Close" : hint(kProbeApply, kProbeCancel),
                     panel.x + panel.w - 48, panel.y + panel.h - 56, Font::Small, kMuted, Align::Right);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    }

    void fill(SDL_Rect rect, SDL_Color c) {
        SDL_SetRenderDrawColor(canvas_.renderer(), c.r, c.g, c.b, c.a);
        SDL_RenderFillRect(canvas_.renderer(), &rect);
    }

    gfx::Canvas canvas_;
    std::string argv0_;
    std::unique_ptr<update::Updater> updater_;
    SDL_Texture* logo_ = nullptr;
    PadState pad_{};
    module::Settings settings_;
    module::Status status_;
    std::string name_;
    int mbps_ = 5;
    int fps_ = 30;
    int keyframeMs_ = 500;
    int activeMbps_ = 5;
    int activeFps_ = 30;
    int activeKeyframeMs_ = 500;
    Probe probe_ = Probe::Idle;
    uint32_t probeId_ = 0;
    uint32_t probeChanged_ = 0;
    uint32_t probeStarted_ = 0;
    int probeStep_ = 0;
    int probeRun_ = 0;
    bool popupOpen_ = false;
    int probePick_ = -1;
    int probeCapacityKbps_ = 0;
    bool probeNoViewer_ = false;
    std::string probeReason_;
    std::string probeHelp_;
    int category_ = 0;
    int item_ = 0;
    int repeat_ = 0;
    s32 touchCount_ = 0;
    bool inItems_ = false;
    uint32_t frame_ = 0;
    std::string message_;
    SDL_Color messageColor_{};
    uint32_t messageUntil_ = 0;
};
}

int main(int argc, char** argv) {
    App app(argc > 0 ? argv[0] : nullptr);
    return app.run();
}
