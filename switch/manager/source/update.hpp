#pragma once

#include <switch.h>

#include <atomic>
#include <string>
#include <vector>

#include "update_core.hpp"

namespace update {
bool init();
void exit();

enum class State { Idle, Checking, Offer, Downloading, Done, Failed };

class Updater {
public:
    explicit Updater(const char* argv0);
    ~Updater();

    void startCheck();
    void startApply();

    State state() const { return state_.load(std::memory_order_acquire); }
    int percent() const { return percent_.load(); }
    void dismiss();

    const std::vector<std::string>& summary() const { return summary_; }
    bool moduleChanged() const { return moduleChanged_; }
    const std::string& reason() const { return reason_; }

private:
    struct Job {
        std::string dest;
        File file;
    };

    static void entry(void* self);
    void join();
    void check();
    bool loadOverride();
    std::string destination(const std::string& component, const std::string& name) const;
    void apply();
    bool download(const Job& job, uint64_t& done, uint64_t total);
    void fail(const char* why);
    static void recover(const std::string& dest);

    Thread thread_{};
    bool threadActive_ = false;
    bool applying_ = false;
    std::atomic<State> state_{State::Idle};
    std::atomic<int> percent_{0};
    std::atomic<bool> cancel_{false};
    std::string argv0_;
    std::string overridePrefix_;
    std::string manifestUrl_;
    std::vector<Job> jobs_;
    std::vector<std::string> summary_;
    bool moduleChanged_ = false;
    std::string reason_;
};
}
