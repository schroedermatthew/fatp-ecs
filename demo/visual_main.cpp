/**
 * @file visual_main.cpp
 * @brief FAT-P ECS Phase 4b — SDL2 Visual Space Battle Demo
 *
 * Real-time visualization of the FAT-P ECS space battle simulation.
 * The actual C++ ECS ticks every frame; SDL2 renders the result.
 * Frame time shown is the real end-to-end cost: ECS tick + render.
 *
 * Dependencies: SDL2, SDL2_ttf (visual demo only — ECS library is unchanged)
 *
 * Controls:
 *   Space  — pause/resume
 *   1/2/3  — speed 1x/2x/5x ticks per frame
 *   R      — reset simulation
 *   F      — toggle vsync (capped 60fps vs uncapped)
 *   +/-    — increase/decrease wave size
 *   Escape — quit
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <SDL.h>
#include <SDL_ttf.h>

#include "Simulation.h"

// =============================================================================
// Color Palette
// =============================================================================

struct Color
{
    Uint8 r, g, b, a;
};

namespace Colors
{
    // Background
    constexpr Color BG          = {6, 8, 13, 255};
    constexpr Color GRID        = {56, 189, 248, 10};
    constexpr Color DEFENSE_LINE = {56, 189, 248, 40};

    // Turrets
    constexpr Color TURRET       = {14, 165, 233, 255};
    constexpr Color TURRET_FIRE  = {56, 189, 248, 255};
    constexpr Color TURRET_INNER = {12, 74, 110, 255};
    constexpr Color TURRET_RANGE = {56, 189, 248, 15};

    // Bullets
    constexpr Color BULLET       = {250, 204, 21, 255};
    constexpr Color BULLET_TRAIL = {250, 204, 21, 120};

    // AI states
    constexpr Color AI_IDLE      = {100, 116, 139, 255};
    constexpr Color AI_CHASE     = {245, 158, 11, 255};
    constexpr Color AI_ATTACK    = {239, 68, 68, 255};
    constexpr Color AI_DEAD      = {30, 41, 59, 255};

    // HP bars
    constexpr Color HP_BG        = {0, 0, 0, 128};
    constexpr Color HP_HIGH      = {74, 222, 128, 255};
    constexpr Color HP_MID       = {250, 204, 21, 255};
    constexpr Color HP_LOW       = {239, 68, 68, 255};

    // HUD
    constexpr Color HUD_TEXT     = {226, 232, 240, 255};
    constexpr Color HUD_LABEL    = {71, 85, 105, 255};
    constexpr Color HUD_ACCENT   = {56, 189, 248, 255};
    constexpr Color HUD_BG       = {10, 15, 26, 200};

    // Muzzle flash
    constexpr Color MUZZLE       = {250, 204, 21, 150};
} // namespace Colors

// =============================================================================
// Renderer
// =============================================================================

class Renderer
{
public:
    Renderer(SDL_Renderer* sdl, TTF_Font* font, TTF_Font* fontSmall,
             int windowW, int windowH)
        : mSDL(sdl)
        , mFont(font)
        , mFontSmall(fontSmall)
        , mWindowW(windowW)
        , mWindowH(windowH)
    {
    }

    void render(SpaceBattleSim& sim, double fps, double frameMs,
                int speed, bool vsync, bool paused)
    {
        auto& reg = sim.registry();

        // Clear
        setColor(Colors::BG);
        SDL_RenderClear(mSDL);

        // Grid
        setColor(Colors::GRID);
        for (int x = 0; x < mWindowW; x += 40)
        {
            SDL_RenderDrawLine(mSDL, x, 0, x, mWindowH);
        }
        for (int y = 0; y < mWindowH; y += 40)
        {
            SDL_RenderDrawLine(mSDL, 0, y, mWindowW, y);
        }

        // Defense line
        setColor(Colors::DEFENSE_LINE);
        for (int y = 0; y < mWindowH; y += 14)
        {
            SDL_RenderDrawLine(mSDL, 55, y, 55, std::min(y + 6, mWindowH));
        }

        // Turret range circles
        reg.view<TurretTag, Position>().each(
            [this](Entity, TurretTag& turret, Position& pos)
            {
                drawCircle(static_cast<int>(pos.x), static_cast<int>(pos.y),
                           static_cast<int>(turret.range), Colors::TURRET_RANGE);
            });

        // Bullets
        reg.view<BulletTag, Position, Velocity>().each(
            [this]([[maybe_unused]] Entity e,
                   [[maybe_unused]] BulletTag&,
                   Position& pos, Velocity& vel)
            {
                int px = static_cast<int>(pos.x);
                int py = static_cast<int>(pos.y);

                // Trail
                float len = std::sqrt(vel.dx * vel.dx + vel.dy * vel.dy);
                if (len > 0.001f)
                {
                    float nx = vel.dx / len;
                    float ny = vel.dy / len;
                    int tx = static_cast<int>(pos.x - nx * 12.0f);
                    int ty = static_cast<int>(pos.y - ny * 12.0f);
                    setColor(Colors::BULLET_TRAIL);
                    SDL_RenderDrawLine(mSDL, px, py, tx, ty);
                }

                // Dot
                setColor(Colors::BULLET);
                fillRect(px - 2, py - 2, 4, 4);
            });

        // Enemies
        reg.view<EnemyTag, Position, Health, AIComponent>().each(
            [this]([[maybe_unused]] Entity e,
                   [[maybe_unused]] EnemyTag&,
                   Position& pos, Health& hp, AIComponent& ai)
            {
                int px = static_cast<int>(pos.x);
                int py = static_cast<int>(pos.y);

                // Pick color by AI state
                Color color = Colors::AI_IDLE;
                if (ai.stateMachine)
                {
                    int stateIdx = static_cast<int>(ai.stateMachine->currentStateIndex());
                    switch (stateIdx)
                    {
                    case 0: color = Colors::AI_IDLE; break;
                    case 1: color = Colors::AI_CHASE; break;
                    case 2: color = Colors::AI_ATTACK; break;
                    case 3: color = Colors::AI_DEAD; break;
                    default: break;
                    }
                }

                // Triangle (arrow pointing left)
                setColor(color);
                drawTriangle(px, py, 7);

                // HP bar
                float hpPct = (hp.maxHp > 0)
                    ? static_cast<float>(hp.hp) / static_cast<float>(hp.maxHp)
                    : 0.0f;
                if (hpPct < 1.0f)
                {
                    // Background
                    setColor(Colors::HP_BG);
                    fillRect(px - 8, py - 12, 16, 3);

                    // Fill
                    Color hpColor = Colors::HP_HIGH;
                    if (hpPct < 0.25f) hpColor = Colors::HP_LOW;
                    else if (hpPct < 0.5f) hpColor = Colors::HP_MID;
                    setColor(hpColor);
                    int fillW = static_cast<int>(16.0f * hpPct);
                    if (fillW > 0)
                    {
                        fillRect(px - 8, py - 12, fillW, 3);
                    }
                }
            });

        // Turrets
        reg.view<TurretTag, Position>().each(
            [this](Entity, TurretTag& turret, Position& pos)
            {
                int px = static_cast<int>(pos.x);
                int py = static_cast<int>(pos.y);
                bool firing = turret.cooldown >
                    static_cast<int>(static_cast<float>(turret.fireCooldown) * 0.7f);

                // Outer circle
                setColor(firing ? Colors::TURRET_FIRE : Colors::TURRET);
                fillCircle(px, py, 10);

                // Inner
                setColor(Colors::TURRET_INNER);
                fillCircle(px, py, 5);

                // Muzzle flash
                if (firing)
                {
                    setColor(Colors::MUZZLE);
                    fillCircle(px + 12, py, 4);
                }
            });

        // HUD
        renderHUD(sim, fps, frameMs, speed, vsync, paused);

        SDL_RenderPresent(mSDL);
    }

private:
    void setColor(Color c)
    {
        SDL_SetRenderDrawColor(mSDL, c.r, c.g, c.b, c.a);
    }

    void fillRect(int x, int y, int w, int h)
    {
        SDL_Rect rect = {x, y, w, h};
        SDL_RenderFillRect(mSDL, &rect);
    }

    void drawCircle(int cx, int cy, int radius, Color c)
    {
        setColor(c);
        int x = radius;
        int y = 0;
        int err = 1 - radius;
        while (x >= y)
        {
            SDL_RenderDrawPoint(mSDL, cx + x, cy + y);
            SDL_RenderDrawPoint(mSDL, cx - x, cy + y);
            SDL_RenderDrawPoint(mSDL, cx + x, cy - y);
            SDL_RenderDrawPoint(mSDL, cx - x, cy - y);
            SDL_RenderDrawPoint(mSDL, cx + y, cy + x);
            SDL_RenderDrawPoint(mSDL, cx - y, cy + x);
            SDL_RenderDrawPoint(mSDL, cx + y, cy - x);
            SDL_RenderDrawPoint(mSDL, cx - y, cy - x);
            y++;
            if (err < 0)
            {
                err += 2 * y + 1;
            }
            else
            {
                x--;
                err += 2 * (y - x) + 1;
            }
        }
    }

    void fillCircle(int cx, int cy, int radius)
    {
        for (int dy = -radius; dy <= radius; dy++)
        {
            int dx = static_cast<int>(std::sqrt(
                static_cast<float>(radius * radius - dy * dy)));
            SDL_RenderDrawLine(mSDL, cx - dx, cy + dy, cx + dx, cy + dy);
        }
    }

    void drawTriangle(int cx, int cy, int size)
    {
        // Arrow pointing left (enemies move left)
        int x1 = cx - size;
        int y1 = cy - static_cast<int>(size * 0.7f);
        int x2 = cx + size;
        int y2 = cy;
        int x3 = cx - size;
        int y3 = cy + static_cast<int>(size * 0.7f);

        // Filled triangle via scanlines
        int minY = std::min({y1, y2, y3});
        int maxY = std::max({y1, y2, y3});
        for (int y = minY; y <= maxY; y++)
        {
            float xStart = static_cast<float>(x2);
            float xEnd = static_cast<float>(x2);

            auto intersect = [&](int ax, int ay, int bx, int by)
            {
                if ((ay <= y && by > y) || (by <= y && ay > y))
                {
                    float t = static_cast<float>(y - ay) /
                              static_cast<float>(by - ay);
                    float ix = static_cast<float>(ax) +
                               t * static_cast<float>(bx - ax);
                    xStart = std::min(xStart, ix);
                    xEnd = std::max(xEnd, ix);
                }
            };

            intersect(x1, y1, x2, y2);
            intersect(x2, y2, x3, y3);
            intersect(x3, y3, x1, y1);

            SDL_RenderDrawLine(mSDL,
                static_cast<int>(xStart), y,
                static_cast<int>(xEnd), y);
        }
    }

    void renderText(const char* text, int x, int y, Color c, TTF_Font* font)
    {
        SDL_Color sdlColor = {c.r, c.g, c.b, c.a};
        SDL_Surface* surface = TTF_RenderText_Blended(font, text, sdlColor);
        if (surface == nullptr) { return; }
        SDL_Texture* texture = SDL_CreateTextureFromSurface(mSDL, surface);
        if (texture != nullptr)
        {
            SDL_Rect dst = {x, y, surface->w, surface->h};
            SDL_RenderCopy(mSDL, texture, nullptr, &dst);
            SDL_DestroyTexture(texture);
        }
        SDL_FreeSurface(surface);
    }

    void renderHUD(SpaceBattleSim& sim, double fps, double frameMs,
                   int speed, bool vsync, bool paused)
    {
        const auto& s = sim.stats();

        // HUD background panel
        setColor(Colors::HUD_BG);
        fillRect(0, 0, mWindowW, 32);

        // Top bar
        char buf[256];

        // FPS and frame time
        std::snprintf(buf, sizeof(buf), "FPS: %.0f", fps);
        renderText(buf, 10, 6, Colors::HUD_ACCENT, mFont);

        std::snprintf(buf, sizeof(buf), "%.2f ms", frameMs);
        renderText(buf, 110, 6, Colors::HUD_TEXT, mFont);

        // Entity count
        std::snprintf(buf, sizeof(buf), "Entities: %zu", s.entityCount);
        renderText(buf, 230, 6, Colors::HUD_TEXT, mFont);

        // Score
        std::snprintf(buf, sizeof(buf), "Score: %d", sim.getCurrentScore());
        renderText(buf, 400, 6, Colors::HUD_ACCENT, mFont);

        // Kills
        std::snprintf(buf, sizeof(buf), "Kills: %d", s.totalKilled);
        renderText(buf, 540, 6, Colors::HUD_TEXT, mFont);

        // Speed / state
        const char* stateStr = paused ? "PAUSED" : (vsync ? "VSYNC" : "UNCAPPED");
        std::snprintf(buf, sizeof(buf), "%dx  %s", speed, stateStr);
        renderText(buf, mWindowW - 160, 6, Colors::HUD_LABEL, mFont);

        // Bottom stats bar
        int bottomY = mWindowH - 26;
        setColor(Colors::HUD_BG);
        fillRect(0, bottomY, mWindowW, 26);

        std::snprintf(buf, sizeof(buf),
            "Spawned: %d   Peak: %zu   Bullets: %d   AI changes: %d   "
            "Wave: %d   FAT-P: 19/19",
            s.totalSpawned, s.peakEntities, s.bulletsSpawned,
            s.totalAIStateChanges, s.frame / sim.config().spawnInterval);
        renderText(buf, 10, bottomY + 4, Colors::HUD_LABEL, mFontSmall);

        // Controls hint
        renderText("[Space] Pause  [1-3] Speed  [F] VSync  [R] Reset  [+/-] Wave  [Esc] Quit",
                   mWindowW - 550, bottomY + 4, {50, 60, 80, 255}, mFontSmall);

        // AI state legend (top-right)
        int legendX = mWindowW - 220;
        int legendY = 40;
        setColor({10, 15, 26, 180});
        fillRect(legendX - 6, legendY - 4, 216, 22);

        struct LegendEntry { const char* label; Color color; };
        LegendEntry entries[] = {
            {"IDLE", Colors::AI_IDLE},
            {"CHASE", Colors::AI_CHASE},
            {"ATK", Colors::AI_ATTACK},
        };
        int lx = legendX;
        for (const auto& entry : entries)
        {
            setColor(entry.color);
            fillRect(lx, legendY, 8, 8);
            renderText(entry.label, lx + 12, legendY - 2, entry.color, mFontSmall);
            lx += 65;
        }
    }

    SDL_Renderer* mSDL;
    TTF_Font* mFont;
    TTF_Font* mFontSmall;
    int mWindowW;
    int mWindowH;
};

// =============================================================================
// Main
// =============================================================================

int main(int argc, char* argv[])
{
    SimConfig config;
    config.arenaWidth = 1100.0f;
    config.arenaHeight = 650.0f;
    config.totalFrames = 0; // Infinite — visual demo runs until user quits
    config.spawnInterval = 5;
    config.waveSize = 20;

    // Parse args
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--wave-size") == 0 && i + 1 < argc)
        {
            config.waveSize = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
        {
            config.numThreads = static_cast<std::size_t>(std::atoi(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--turrets") == 0 && i + 1 < argc)
        {
            config.numTurrets = std::atoi(argv[++i]);
        }
    }

    // SDL Init
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    if (TTF_Init() != 0)
    {
        std::fprintf(stderr, "TTF_Init failed: %s\n", TTF_GetError());
        SDL_Quit();
        return 1;
    }

    const int WINDOW_W = static_cast<int>(config.arenaWidth);
    const int WINDOW_H = static_cast<int>(config.arenaHeight);

    SDL_Window* window = SDL_CreateWindow(
        "FAT-P ECS — Space Battle",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WINDOW_W, WINDOW_H,
        SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);

    if (window == nullptr)
    {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    bool vsync = true;
    SDL_Renderer* sdlRenderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | (vsync ? SDL_RENDERER_PRESENTVSYNC : 0));

    if (sdlRenderer == nullptr)
    {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    SDL_SetRenderDrawBlendMode(sdlRenderer, SDL_BLENDMODE_BLEND);

    // Load font — try common system paths
    TTF_Font* font = nullptr;
    TTF_Font* fontSmall = nullptr;

    const char* fontPaths[] = {
        // Windows
        "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/cour.ttf",
        "C:/Windows/Fonts/lucon.ttf",
        // Linux
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
        // macOS
        "/System/Library/Fonts/SFMono-Regular.otf",
        "/System/Library/Fonts/Menlo.ttc",
        "/Library/Fonts/Courier New.ttf",
        nullptr
    };

    for (int i = 0; fontPaths[i] != nullptr; ++i)
    {
        font = TTF_OpenFont(fontPaths[i], 14);
        if (font != nullptr)
        {
            fontSmall = TTF_OpenFont(fontPaths[i], 11);
            std::printf("Font loaded: %s\n", fontPaths[i]);
            break;
        }
    }

    if (font == nullptr)
    {
        std::fprintf(stderr,
            "No monospace font found. Place a .ttf in the working directory "
            "or install a system monospace font.\n");
        SDL_DestroyRenderer(sdlRenderer);
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    // Create simulation and renderer
    auto sim = std::make_unique<SpaceBattleSim>(config);
    Renderer renderer(sdlRenderer, font, fontSmall, WINDOW_W, WINDOW_H);

    // Main loop state
    bool running = true;
    bool paused = false;
    int speed = 1;
    double frameMs = 0.0;

    // FPS tracking
    Uint64 fpsCounterStart = SDL_GetPerformanceCounter();
    int fpsFrameCount = 0;
    double fpsDisplay = 0.0;

    FrameTimer frameTimer;

    while (running)
    {
        frameTimer.start();

        // --- Events ---
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_QUIT)
            {
                running = false;
            }
            else if (event.type == SDL_KEYDOWN)
            {
                switch (event.key.keysym.sym)
                {
                case SDLK_ESCAPE:
                    running = false;
                    break;
                case SDLK_SPACE:
                    paused = !paused;
                    break;
                case SDLK_1:
                    speed = 1;
                    break;
                case SDLK_2:
                    speed = 2;
                    break;
                case SDLK_3:
                    speed = 5;
                    break;
                case SDLK_r:
                    sim = std::make_unique<SpaceBattleSim>(config);
                    break;
                case SDLK_f:
                    vsync = !vsync;
                    SDL_DestroyRenderer(sdlRenderer);
                    sdlRenderer = SDL_CreateRenderer(window, -1,
                        SDL_RENDERER_ACCELERATED |
                        (vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
                    SDL_SetRenderDrawBlendMode(sdlRenderer, SDL_BLENDMODE_BLEND);
                    renderer = Renderer(sdlRenderer, font, fontSmall,
                                        WINDOW_W, WINDOW_H);
                    break;
                case SDLK_PLUS:
                case SDLK_EQUALS:
                    config.waveSize = std::min(200, config.waveSize + 10);
                    break;
                case SDLK_MINUS:
                    config.waveSize = std::max(5, config.waveSize - 10);
                    break;
                default:
                    break;
                }
            }
        }

        // --- Tick ---
        if (!paused)
        {
            for (int i = 0; i < speed; ++i)
            {
                sim->tick();
            }
        }

        // --- Render ---
        sim->countAIStateChanges();
        renderer.render(*sim, fpsDisplay, frameMs, speed, vsync, paused);

        // --- Frame timing ---
        frameMs = frameTimer.elapsedMs();
        sim->recordFrameTime(frameMs);

        // FPS counter (updated once per second)
        fpsFrameCount++;
        Uint64 now = SDL_GetPerformanceCounter();
        double elapsed = static_cast<double>(now - fpsCounterStart) /
                         static_cast<double>(SDL_GetPerformanceFrequency());
        if (elapsed >= 1.0)
        {
            fpsDisplay = static_cast<double>(fpsFrameCount) / elapsed;
            fpsFrameCount = 0;
            fpsCounterStart = now;
        }
    }

    // Cleanup
    if (fontSmall != nullptr) TTF_CloseFont(fontSmall);
    if (font != nullptr) TTF_CloseFont(font);
    SDL_DestroyRenderer(sdlRenderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();

    return 0;
}
