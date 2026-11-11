#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace gfx {
inline constexpr int kWidth = 1280;
inline constexpr int kHeight = 720;

enum class Font { Small, Body, Large, Count };
enum class Align { Left, Right };

constexpr SDL_Color rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) { return SDL_Color{r, g, b, a}; }

class Canvas {
public:
    bool init();
    void shutdown();

    SDL_Renderer* renderer() const { return renderer_; }
    void present();

    SDL_Point text(const std::string& s, int x, int y, Font font, SDL_Color c, Align align = Align::Left);

    int measure(const std::string& s, Font font) const;

    SDL_Texture* loadImage(const char* path);
    void image(SDL_Texture* tex, SDL_Rect dst);

private:
    struct Cached {
        SDL_Texture* texture = nullptr;
        int w = 0, h = 0;
        uint32_t lastUsed = 0;
    };
    Cached* cached(const std::string& s, Font font, SDL_Color c);

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    std::array<TTF_Font*, static_cast<size_t>(Font::Count)> fonts_{};
    std::unordered_map<std::string, Cached> cache_;
    uint32_t frame_ = 0;
    bool plReady_ = false;
};
}
