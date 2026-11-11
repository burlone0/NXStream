#include "gfx.hpp"

#include <SDL2/SDL_image.h>
#include <switch.h>

namespace gfx {
namespace {
constexpr int kPointSizes[] = {22, 28, 40};
constexpr uint32_t kEvictAfterFrames = 300;
}

bool Canvas::init() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return false;
    window_ = SDL_CreateWindow("NXStream", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kWidth, kHeight, 0);
    if (!window_) return false;
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) return false;
    SDL_RenderSetLogicalSize(renderer_, kWidth, kHeight);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    IMG_Init(IMG_INIT_PNG);
    if (TTF_Init() != 0) return false;
    plReady_ = R_SUCCEEDED(plInitialize(PlServiceType_User));
    PlFontData font{};
    if (!plReady_ || R_FAILED(plGetSharedFontByType(&font, PlSharedFontType_Standard))) return false;
    for (size_t i = 0; i < fonts_.size(); ++i) {
        fonts_[i] = TTF_OpenFontRW(SDL_RWFromConstMem(font.address, static_cast<int>(font.size)), 1, kPointSizes[i]);
        if (!fonts_[i]) return false;
    }
    return true;
}

void Canvas::shutdown() {
    for (auto& [key, c] : cache_) SDL_DestroyTexture(c.texture);
    cache_.clear();
    for (auto*& f : fonts_) {
        if (f) TTF_CloseFont(f);
        f = nullptr;
    }
    TTF_Quit();
    IMG_Quit();
    if (plReady_) plExit();
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

void Canvas::present() {
    SDL_RenderPresent(renderer_);
    ++frame_;
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (frame_ - it->second.lastUsed > kEvictAfterFrames) {
            SDL_DestroyTexture(it->second.texture);
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
}

Canvas::Cached* Canvas::cached(const std::string& s, Font font, SDL_Color c) {
    if (s.empty()) return nullptr;
    std::string key;
    key.reserve(s.size() + 5);
    key += char(font);
    key.append(reinterpret_cast<const char*>(&c), sizeof c);
    key += s;
    auto it = cache_.find(key);
    if (it == cache_.end()) {
        SDL_Surface* surface = TTF_RenderUTF8_Blended(fonts_[size_t(font)], s.c_str(), SDL_Color{c.r, c.g, c.b, 255});
        if (!surface) return nullptr;
        Cached entry{SDL_CreateTextureFromSurface(renderer_, surface), surface->w, surface->h, 0};
        SDL_FreeSurface(surface);
        if (!entry.texture) return nullptr;
        SDL_SetTextureAlphaMod(entry.texture, c.a);
        it = cache_.emplace(std::move(key), entry).first;
    }
    it->second.lastUsed = frame_;
    return &it->second;
}

SDL_Point Canvas::text(const std::string& s, int x, int y, Font font, SDL_Color c, Align align) {
    Cached* e = cached(s, font, c);
    if (!e) return {0, 0};
    if (align == Align::Right) x -= e->w;
    const SDL_Rect dst{x, y, e->w, e->h};
    SDL_RenderCopy(renderer_, e->texture, nullptr, &dst);
    return {e->w, e->h};
}

int Canvas::measure(const std::string& s, Font font) const {
    int w = 0;
    if (!s.empty()) TTF_SizeUTF8(fonts_[size_t(font)], s.c_str(), &w, nullptr);
    return w;
}

SDL_Texture* Canvas::loadImage(const char* path) {
    SDL_Surface* surface = IMG_Load(path);
    if (!surface) return nullptr;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer_, surface);
    SDL_FreeSurface(surface);
    if (tex) SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
    return tex;
}

void Canvas::image(SDL_Texture* tex, SDL_Rect dst) {
    if (tex) SDL_RenderCopy(renderer_, tex, nullptr, &dst);
}
}
