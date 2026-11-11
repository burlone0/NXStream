#pragma once

namespace ams::mitm::applet {

    struct EncodeIdleWork {
        bool (*fn)(void *) = nullptr;
        void *ctx = nullptr;

        bool Poll() {
            if (fn != nullptr && fn(ctx)) {
                fn = nullptr;
                ctx = nullptr;
                return true;
            }
            return false;
        }
    };

}
