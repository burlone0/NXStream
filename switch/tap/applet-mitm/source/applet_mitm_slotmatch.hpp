#pragma once
#include <cstdint>
#include <cstring>

namespace ams::mitm::applet::slotmatch {

    constexpr uint32_t MaxSlots = 8;
    constexpr uint32_t MaxCand  = 48;

    constexpr uint32_t MinPerSlot = 8;
    constexpr int32_t  MinScore   = 600;
    constexpr int32_t  MinMargin  = 300;
    constexpr uint32_t GiveUpAfter = 900;
    constexpr uint32_t MaxSamples  = 3600;

    struct Matcher {
        uint32_t nslots = 0, ncand = 0;
        uint32_t sig[MaxCand] = {};
        uint32_t last = 0;
        bool have = false;
        uint16_t hit_in[MaxSlots][MaxCand] = {};
        uint16_t hit_out[MaxSlots][MaxCand] = {};
        uint16_t tot_in[MaxSlots] = {};
        uint16_t tot_out[MaxSlots] = {};
        uint32_t intervals = 0;
        uint32_t samples = 0;

        void Reset(uint32_t slots, uint32_t cands) {
            nslots = slots < MaxSlots ? slots : MaxSlots;
            ncand  = cands < MaxCand ? cands : MaxCand;
            have = false;
            last = 0;
            intervals = 0;
            samples = 0;
            std::memset(sig, 0, sizeof(sig));
            std::memset(hit_in, 0, sizeof(hit_in));
            std::memset(hit_out, 0, sizeof(hit_out));
            std::memset(tot_in, 0, sizeof(tot_in));
            std::memset(tot_out, 0, sizeof(tot_out));
        }

        void AddSpan(uint32_t count, uint32_t drawn, const uint32_t *sigs) {
            ++samples;
            const uint32_t all = (nslots >= 32) ? 0xFFFFFFFFu : ((1u << nslots) - 1);
            drawn &= all;
            if (have && count > last && drawn != 0 && drawn != all) {
                bool any = false;
                for (uint32_t c = 0; c < ncand; ++c) { any |= sigs[c] != sig[c]; }
                if (any) {
                    ++intervals;
                    for (uint32_t s = 0; s < nslots; ++s) {
                        const bool in = (drawn >> s) & 1;
                        uint16_t &tot = in ? tot_in[s] : tot_out[s];
                        uint16_t *hit = in ? hit_in[s] : hit_out[s];
                        if (tot == 0xFFFF) { continue; }
                        ++tot;
                        for (uint32_t c = 0; c < ncand; ++c) { if (sigs[c] != sig[c]) { ++hit[c]; } }
                    }
                }
            }
            std::memcpy(sig, sigs, ncand * sizeof(uint32_t));
            last = count;
            have = true;
        }

        void Add(uint32_t count, uint32_t slot, const uint32_t *sigs) {
            AddSpan(count, (have && count == last + 1 && slot < nslots) ? (1u << slot) : 0, sigs);
        }

        bool Match(uint8_t *out, int32_t *weakest) const {
            if (nslots < 2) { return false; }
            for (uint32_t s = 0; s < nslots; ++s) { if (tot_in[s] < MinPerSlot || tot_out[s] < MinPerSlot) { return false; } }
            uint8_t m[MaxSlots] = {};
            uint64_t used = 0;
            int32_t worst = 1000;
            for (uint32_t s = 0; s < nslots; ++s) {
                int32_t best = -1, best_score = -100000, second = -100000;
                for (uint32_t c = 0; c < ncand; ++c) {
                    const int32_t score = static_cast<int32_t>(hit_in[s][c] * 1000u / tot_in[s]) -
                                          static_cast<int32_t>(hit_out[s][c] * 1000u / tot_out[s]);
                    if (score > best_score) { second = best_score; best_score = score; best = static_cast<int32_t>(c); }
                    else if (score > second) { second = score; }
                }
                if (best < 0 || best_score < MinScore || best_score - second < MinMargin || (used & (UINT64_C(1) << best)) != 0) { return false; }
                m[s] = static_cast<uint8_t>(best);
                used |= UINT64_C(1) << best;
                if (best_score < worst) { worst = best_score; }
            }
            std::memcpy(out, m, nslots);
            if (weakest != nullptr) { *weakest = worst; }
            return true;
        }

        bool GaveUp() const { return intervals >= GiveUpAfter || samples >= MaxSamples; }
    };

    inline bool StillGuess(const uint32_t *sigs, const bool *varied, uint32_t ncand, uint32_t nslots, bool grouped, uint8_t *out) {
        if (nslots < 2 || nslots > MaxSlots || ncand < nslots) { return false; }
        if (grouped) {
            for (uint32_t c = 0; c + nslots <= ncand; c += nslots) {
                bool same = varied[c];
                for (uint32_t k = 1; k < nslots && same; ++k) { same = varied[c + k] && sigs[c + k] == sigs[c]; }
                if (same) { for (uint32_t k = 0; k < nslots; ++k) { out[k] = static_cast<uint8_t>(c + k); } return true; }
            }
            return false;
        }
        for (uint32_t c = 0; c < ncand; ++c) {
            if (!varied[c]) { continue; }
            uint32_t n = 0;
            for (uint32_t d = c; d < ncand && n < nslots; ++d) {
                if (varied[d] && sigs[d] == sigs[c]) { out[n++] = static_cast<uint8_t>(d); }
            }
            if (n == nslots) { return true; }
        }
        return false;
    }

}
