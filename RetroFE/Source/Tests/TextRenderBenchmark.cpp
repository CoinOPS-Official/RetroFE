#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "../Database/Configuration.h"
#include "../Graphics/Component/Text.h"
#include "../Graphics/Font.h"
#include "../Graphics/Page.h"
#include "../SDL.h"
#include "../Utility/Utils.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

namespace {
struct Entry {
    std::string title;
    std::string year;
    std::string manufacturer;
    std::string players;
};

[[noreturn]] void fail(const char* message) {
    std::fprintf(stderr, "%s: %s\n", message, SDL_GetError());
    std::exit(EXIT_FAILURE);
}

std::vector<Entry> loadEntries(const std::filesystem::path& root) {
    sqlite3* db = nullptr;
    const auto path = (root / "meta.db").string();
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        std::fprintf(stderr, "Cannot read %s: %s\n", path.c_str(), db ? sqlite3_errmsg(db) : "unknown error");
        if (db) sqlite3_close(db);
        std::exit(EXIT_FAILURE);
    }
    sqlite3_stmt* query = nullptr;
    constexpr char sql[] =
        "SELECT title, year, manufacturer, players FROM Meta "
        "WHERE collectionName='MAME' AND title<>'' ORDER BY name";
    if (sqlite3_prepare_v2(db, sql, -1, &query, nullptr) != SQLITE_OK) {
        std::fprintf(stderr, "Cannot query metadata: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        std::exit(EXIT_FAILURE);
    }
    std::vector<Entry> all;
    while (sqlite3_step(query) == SQLITE_ROW) {
        auto column = [&](int index) {
            const auto* value = sqlite3_column_text(query, index);
            return value ? std::string(reinterpret_cast<const char*>(value)) : std::string();
        };
        all.push_back({column(0), column(1), column(2), column(3)});
    }
    sqlite3_finalize(query);
    sqlite3_close(db);
    if (all.size() < 64) {
        std::fprintf(stderr, "Need at least 64 MAME metadata rows, found %zu\n", all.size());
        std::exit(EXIT_FAILURE);
    }
    std::vector<Entry> selected;
    selected.reserve(64);
    for (size_t i = 0; i < 64; ++i)
        selected.push_back(all[i * all.size() / 64]);
    return selected;
}

std::unique_ptr<Text> makeText(Page& page, FontManager& font, float size,
    float x, float y, float width, SDL_Color color) {
    auto text = std::make_unique<Text>("", page, &font, 0);
    text->baseViewInfo.FontSize = size;
    text->baseViewInfo.X = x;
    text->baseViewInfo.Y = y;
    text->baseViewInfo.MaxWidth = width;
    text->baseViewInfo.textColor = color;
    return text;
}

struct Samples {
    std::vector<double> us;
    void add(std::chrono::steady_clock::time_point start) {
        const auto end = std::chrono::steady_clock::now();
        us.push_back(std::chrono::duration<double, std::micro>(end - start).count());
    }
    void print(const char* name) {
        std::sort(us.begin(), us.end());
        const auto percentile = [&](double fraction) {
            return us[std::min(us.size() - 1,
                static_cast<size_t>(std::ceil(fraction * us.size())) - 1)];
        };
        const double total = std::accumulate(us.begin(), us.end(), 0.0);
        std::printf("BENCH %-22s count=%zu avg_us=%.2f p50_us=%.2f p95_us=%.2f p99_us=%.2f max_us=%.2f\n",
            name, us.size(), total / us.size(), percentile(0.50), percentile(0.95),
            percentile(0.99), us.back());
    }
};
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: retrofe_text_benchmark <populated RetroFE root> [renderer] [--prewarm-ascii]\n");
        return EXIT_FAILURE;
    }
    const std::filesystem::path root(argv[1]);
#ifdef _WIN32
    const std::string rendererName = argc > 2 ? argv[2] : "direct3d12";
#else
    const std::string rendererName = argc > 2 ? argv[2] : "opengl";
#endif
    const bool prewarmAscii = argc > 3 && std::string(argv[3]) == "--prewarm-ascii";
    const auto entries = loadEntries(root);
    Configuration::absolutePath = root.string();

    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    Configuration config;
    config.setProperty("screenOrder", std::string("0"));
    config.setProperty("horizontal0", 1920);
    config.setProperty("vertical0", 1080);
    config.setProperty("fullscreen0", false);
    config.setProperty("hideMouse", false);
    config.setProperty("vSync", false);
    config.setProperty("SDLRenderDriver", rendererName);
    config.setProperty("layoutScaleMode", std::string("stretch"));
    if (!SDL::initialize(config)) fail("Initialize renderer");
    if (!TTF_Init()) fail("Initialize SDL_ttf");
    SDL_Renderer* renderer = SDL::getRenderer(0);
    if (!renderer) fail("Get renderer");
    const char* actualRenderer = SDL_GetRendererName(renderer);
    if (!actualRenderer || rendererName != actualRenderer) {
        std::fprintf(stderr, "Requested renderer %s, got %s\n", rendererName.c_str(),
            actualRenderer ? actualRenderer : "null");
        return EXIT_FAILURE;
    }
    if (!SDL_SetRenderTarget(renderer, SDL::getRenderTarget(0))) fail("Set render target");

    std::printf("CONFIG mode=sdl3_ttf renderer=%s entries=%zu layout=1920x1080 vsync=off prewarm=%s timer=CPU_text_draw\n",
        actualRenderer, entries.size(),
        prewarmAscii ? "ascii" : "none");
    {
        Page page(config, 1920, 1080);
        const auto aura = (root / "layouts/Arcades/fonts/aura4k.ttf").string();
        const auto scoreFace = (root / "layouts/Arcades/fonts/font.ttf").string();
        const auto scout = (root / "layouts/Arcades/fonts/ScoutCond-Bold.otf").string();
        FontManager metadataFont(aura, 32, {255, 255, 255, 255}, false, 0, 0);
        FontManager scoreFont(scoreFace, 65, {0, 254, 250, 255}, true, 2, 0);
        FontManager clockFont(scout, 122, {255, 255, 255, 255}, true, 3, 0);
        const auto initStart = std::chrono::steady_clock::now();
        if (!metadataFont.initialize() || !scoreFont.initialize() || !clockFont.initialize())
            fail("Initialize layout fonts");
        const double initMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - initStart).count();
        std::printf("BENCH font_init_ms=%.2f\n", initMs);
        if (prewarmAscii) {
            const auto start = std::chrono::steady_clock::now();
            for (auto* font : {&metadataFont, &scoreFont, &clockFont}) {
                const auto* mip = font->getMipLevelForSize(font->getMaxFontSize());
                if (!font->prewarmTextEngine(mip))
                    fail("Prewarm text engine");
            }
            std::printf("BENCH ascii_prewarm_ms=%.2f\n",
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count());
        }

        std::array<std::unique_ptr<Text>, 4> metadata;
        constexpr std::array<float, 4> x{24.f, 24.f, 24.f, 24.f};
        constexpr std::array<float, 4> y{32.f, 72.f, 112.f, 152.f};
        for (size_t i = 0; i < metadata.size(); ++i)
            metadata[i] = makeText(page, metadataFont, 32.f, x[i], y[i], 1000.f,
                {255, 255, 255, 255});
        std::array<std::unique_ptr<Text>, 8> scoreRows;
        for (size_t i = 0; i < scoreRows.size(); ++i)
            scoreRows[i] = makeText(page, scoreFont, 65.f, 32.f,
                260.f + 84.f * static_cast<float>(i), 1500.f, {0, 254, 250, 255});
        auto clock = makeText(page, clockFont, 122.f, 1100.f, 32.f, 700.f,
            {255, 255, 255, 255});
        clock->setText("12:38");

        auto assignMetadata = [&](size_t index) {
            const Entry& entry = entries[index % entries.size()];
            metadata[0]->setText(entry.title);
            metadata[1]->setText(entry.year);
            metadata[2]->setText(entry.manufacturer);
            metadata[3]->setText(entry.players + " Players");
        };
        auto drawMetadata = [&] {
            for (auto& label : metadata) label->draw();
            clock->draw();
        };
        auto assignScores = [&](size_t index) {
            for (size_t i = 0; i < scoreRows.size(); ++i) {
                const Entry& entry = entries[(index + i) % entries.size()];
                const std::string value = std::to_string(i + 1) + "  " + entry.title +
                    "  " + std::to_string(912500 - static_cast<int>(i) * 43870);
                scoreRows[i]->setText(value);
            }
        };
        auto drawScores = [&] {
            for (auto& row : scoreRows) row->draw();
        };
        auto frame = [&](Samples& samples, auto&& draw) {
            if (!SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255) ||
                !SDL_RenderClear(renderer)) fail("Clear benchmark frame");
            const auto start = std::chrono::steady_clock::now();
            draw();
            samples.add(start);
            if (!SDL_FlushRenderer(renderer)) fail("Flush benchmark frame");
        };

        // First selections include shaping, glyph uploads, and Text cache rebuilds.
        Samples metadataCold;
        for (size_t i = 0; i < entries.size(); ++i) {
            assignMetadata(i);
            frame(metadataCold, drawMetadata);
        }
        for (size_t i = 0; i < metadataCold.us.size(); ++i)
            if (metadataCold.us[i] > 1000.0)
                std::printf("COLD_SPIKE metadata index=%zu us=%.2f\n", i, metadataCold.us[i]);
        metadataCold.print("metadata_change_cold");

        Samples metadataWarm;
        for (int pass = 0; pass < 5; ++pass)
            for (size_t i = 0; i < entries.size(); ++i) {
                assignMetadata(i);
                frame(metadataWarm, drawMetadata);
            }
        metadataWarm.print("metadata_change_warm");

        Samples metadataRedraw;
        for (int i = 0; i < 1000; ++i) frame(metadataRedraw, drawMetadata);
        metadataRedraw.print("metadata_redraw");

        Samples scoreCold;
        for (size_t i = 0; i < entries.size(); ++i) {
            assignScores(i);
            frame(scoreCold, drawScores);
        }
        for (size_t i = 0; i < scoreCold.us.size(); ++i)
            if (scoreCold.us[i] > 1000.0)
                std::printf("COLD_SPIKE scores index=%zu us=%.2f\n", i, scoreCold.us[i]);
        scoreCold.print("score_rows_cold");

        Samples scoreWarm;
        for (int pass = 0; pass < 4; ++pass)
            for (size_t i = 0; i < entries.size(); ++i) {
                assignScores(i);
                frame(scoreWarm, drawScores);
            }
        scoreWarm.print("score_rows_warm");
        std::printf("MEMORY working_set_mb=%.1f\n", Utils::getMemoryUsage() / 1024.0);
    }
    TTF_Quit();
    if (!SDL::deInitialize(true)) fail("Shutdown SDL");
    return EXIT_SUCCESS;
}
