#pragma once

#include "GeometryBatch.h"
#include "../SDL.h"
#include <SDL3_ttf/SDL_textengine.h>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// SDL_ttf owns shaping and positioning. This renderer owns only rasterized
// glyph pages, allowing per-glyph vertex colors without reshaping on redraw.
class TextEngineAtlas {
public:
    TextEngineAtlas(SDL_Renderer* renderer, TTF_Font* font, TTF_Font* outlineFont,
        int outlinePx, SDL_Color outlineColor, bool gradient)
        : renderer_(renderer), font_(font), outlineFont_(outlineFont),
          outlinePx_(outlinePx), outlineColor_(outlineColor), gradient_(gradient) {
        while (pageSize_ < 1024 && pageSize_ < TTF_GetFontSize(font_) * 16)
            pageSize_ *= 2;
        SDL_INIT_INTERFACE(&engine_);
        engine_.userdata = this;
        engine_.CreateText = &createText;
        engine_.DestroyText = &destroyText;
    }

    ~TextEngineAtlas() {
        for (auto& entry : texts_) TTF_DestroyText(entry.second.text);
        for (auto& entry : fallbackOutlines_) TTF_CloseFont(entry.second);
        for (auto& page : fill_.pages) SDL_DestroyTexture(page.texture);
        for (auto& page : outline_.pages) SDL_DestroyTexture(page.texture);
        if (solidTexture_) SDL_DestroyTexture(solidTexture_);
    }

    TextEngineAtlas(const TextEngineAtlas&) = delete;
    TextEngineAtlas& operator=(const TextEngineAtlas&) = delete;

    bool measure(const std::string& string, float scale, float& width) {
        CachedText* cached = getText(string);
        if (!cached) return false;
        width = static_cast<float>(cached->width + (cached->hasGlyph ? 2 * outlinePx_ : 0)) * scale;
        return true;
    }

    bool wrapLines(const std::string& string, float scale, float maxWidth,
        std::vector<std::string>& lines) {
        if (scale <= 0.f || maxWidth <= 0.f) return false;
        TTF_Text* wrapped = TTF_CreateText(nullptr, font_, string.c_str(), string.size());
        if (!wrapped) return false;
        const int width = std::max(1, static_cast<int>(maxWidth / scale) - 2 * outlinePx_);
        bool ok = TTF_SetTextWrapWidth(wrapped, width) && TTF_UpdateText(wrapped);
        if (ok) {
            for (int i = 0; i < wrapped->num_lines; ++i) {
                TTF_SubString line{};
                if (!TTF_GetTextSubStringForLine(wrapped, i, &line)) {
                    ok = false;
                    break;
                }
                lines.emplace_back(string.substr(line.offset, line.length));
            }
            if (wrapped->num_lines == 0) lines.emplace_back();
        }
        TTF_DestroyText(wrapped);
        return ok;
    }

    bool prewarmAscii() {
        if (asciiPrewarmed_) return true;
        std::string separated;
        for (char ch = '!'; ch <= '~'; ++ch) {
            separated += ch;
            separated += ' ';
        }
        CachedText* cached = getText(separated);
        if (!cached || !prepareGlyphs(cached)) return false;
        asciiPrewarmed_ = true;
        return true;
    }

    bool draw(const std::string& string, float x, float y, float scale, SDL_Color color) {
        return drawImpl(string, x, y, scale, color, nullptr, 0, 0, nullptr);
    }

    bool drawTransformed(const std::string& string, float x, float y, float scale,
        SDL_Color color, const ViewInfo& view, int layoutWidth, int layoutHeight,
        const SDL_FRect* clip = nullptr) {
        return drawImpl(string, x, y, scale, color, &view, layoutWidth, layoutHeight, clip);
    }

private:
    static bool SDLCALL createText(void*, TTF_Text* text) {
        text->internal->engine_text = text;
        return true;
    }

    static void SDLCALL destroyText(void*, TTF_Text* text) {
        text->internal->engine_text = nullptr;
    }

    bool drawImpl(const std::string& string, float x, float y, float scale,
        SDL_Color color, const ViewInfo* view, int layoutWidth, int layoutHeight,
        const SDL_FRect* clip) {
        CachedText* cached = getText(string);
        if (!cached || !renderer_) return false;
        TTF_TextData* data = cached->text->internal;
        if (!data) return false;

        // A completed draw has flushed every geometry run, so old pages can be
        // discarded here without invalidating textures queued for rendering.
        if (fill_.pages.size() + outline_.pages.size() > maxAtlasPages_)
            resetAtlases();

        // Upload every missing glyph before emitting geometry. Updating an atlas
        // page while a batch references it would break draw ordering.
        if (!prepareGlyphs(cached)) return false;

        GeometryBatch batch;
        const SDL_FColor fillTop{color.r / 255.f, color.g / 255.f,
            color.b / 255.f, color.a / 255.f};
        SDL_FColor fillBottom = fillTop;
        if (gradient_) {
            constexpr float bottom = 128.f / 255.f;
            fillBottom.r *= bottom;
            fillBottom.g *= bottom;
            fillBottom.b *= bottom;
        }
        const SDL_FColor outlineColor{outlineColor_.r / 255.f,
            outlineColor_.g / 255.f, outlineColor_.b / 255.f,
            (outlineColor_.a / 255.f) * (color.a / 255.f)};

        ViewInfo transformed;
        if (view) {
            transformed = *view;
            if (clip) {
                SDL_FRect area = *clip;
                if (transformed.ContainerWidth > 0 && transformed.ContainerHeight > 0) {
                    const float left = std::max(area.x, transformed.ContainerX);
                    const float top = std::max(area.y, transformed.ContainerY);
                    const float right = std::min(area.x + area.w,
                        transformed.ContainerX + transformed.ContainerWidth);
                    const float bottom = std::min(area.y + area.h,
                        transformed.ContainerY + transformed.ContainerHeight);
                    area = {left, top, std::max(0.f, right - left),
                        std::max(0.f, bottom - top)};
                }
                if (area.w <= 0 || area.h <= 0) return true;
                transformed.ContainerX = area.x;
                transformed.ContainerY = area.y;
                transformed.ContainerWidth = area.w;
                transformed.ContainerHeight = area.h;
            }
        }

        auto pass = [&](bool isOutline) -> bool {
            const Atlas& atlas = isOutline ? outline_ : fill_;
            for (int i = 0; i < data->num_ops; ++i) {
                const auto& op = data->ops[i];
                if (op.cmd == TTF_DRAW_COMMAND_FILL && !isOutline) {
                    if (!solidTexture_ && !createSolidTexture()) return false;
                    const SDL_FRect dst{x + op.fill.rect.x * scale,
                        y + op.fill.rect.y * scale,
                        op.fill.rect.w * scale, op.fill.rect.h * scale};
                    if (view) {
                        const SDL_Rect src{0, 0, 1, 1};
                        if (!SDL::appendCopyFGradient(batch, solidTexture_, view->Alpha,
                            &src, &dst, transformed, layoutWidth, layoutHeight,
                            fillTop, fillBottom)) return false;
                    } else if (!batch.appendGradientTexture(renderer_, solidTexture_,
                        {0, 0, 1, 1}, dst, fillTop, fillBottom)) return false;
                    continue;
                }
                if (op.cmd != TTF_DRAW_COMMAND_COPY) continue;
                const Glyph& glyph = *(isOutline ? cached->outlineGlyphs[i] : cached->fillGlyphs[i]);
                SDL_Texture* texture = atlas.pages[glyph.page].texture;
                const float offset = isOutline ? static_cast<float>(outlinePx_) : 0.f;
                const SDL_FRect dst{
                    x + (op.copy.dst.x - offset) * scale,
                    y + (op.copy.dst.y - offset) * scale,
                    glyph.src.w * scale, glyph.src.h * scale};
                if (clip && view && !view->hasReflection &&
                    (dst.x + dst.w <= clip->x || dst.y + dst.h <= clip->y ||
                     dst.x >= clip->x + clip->w || dst.y >= clip->y + clip->h))
                    continue;
                const SDL_FColor top = (isOutline ? outlineColor : glyph.imageType == TTF_IMAGE_COLOR
                    ? SDL_FColor{1, 1, 1, fillTop.a} : fillTop);
                const SDL_FColor bottom = (isOutline ? outlineColor : glyph.imageType == TTF_IMAGE_COLOR
                    ? SDL_FColor{1, 1, 1, fillBottom.a} : fillBottom);
                if (view) {
                    if (!SDL::appendCopyFGradient(batch, texture, view->Alpha,
                        &glyph.src, &dst, transformed, layoutWidth, layoutHeight,
                        top, bottom)) return false;
                } else if (!batch.appendGradientTexture(renderer_, texture,
                    glyph.src, dst, top, bottom)) return false;
            }
            return true;
        };
        if (outlinePx_ && !pass(true)) return false;
        if (!pass(false)) return false;
        return batch.flush();
    }

    struct GlyphKey {
        TTF_Font* font = nullptr;
        Uint32 index = 0;
        bool operator==(const GlyphKey& other) const {
            return font == other.font && index == other.index;
        }
    };
    struct GlyphKeyHash {
        size_t operator()(const GlyphKey& key) const {
            return (std::hash<TTF_Font*>{}(key.font) << 1) ^ std::hash<Uint32>{}(key.index);
        }
    };
    struct Glyph { size_t page = 0; SDL_Rect src{}; TTF_ImageType imageType = TTF_IMAGE_INVALID; };
    struct CachedText {
        TTF_Text* text = nullptr;
        int width = 0;
        bool hasGlyph = false;
        bool ready = false;
        std::vector<const Glyph*> fillGlyphs;
        std::vector<const Glyph*> outlineGlyphs;
        std::list<std::string>::iterator lru;
    };
    struct Page {
        SDL_Texture* texture = nullptr;
        int nextX = 1;
        int nextY = 1;
        int rowHeight = 0;
    };
    struct Atlas {
        std::vector<Page> pages;
        std::unordered_map<GlyphKey, Glyph, GlyphKeyHash> glyphs;
    };

    bool prepareGlyphs(CachedText* cached) {
        if (cached->ready) return true;
        TTF_TextData* data = cached->text->internal;
        if (!data) return false;
        cached->fillGlyphs.assign(data->num_ops, nullptr);
        if (outlinePx_) cached->outlineGlyphs.assign(data->num_ops, nullptr);
        for (int i = 0; i < data->num_ops; ++i) {
            const auto& op = data->ops[i];
            if (op.cmd != TTF_DRAW_COMMAND_COPY) continue;
            const GlyphKey key{op.copy.glyph_font, op.copy.glyph_index};
            if (!ensure(fill_, key, op.copy.glyph_font)) return false;
            cached->fillGlyphs[i] = &fill_.glyphs.at(key);
            if (outlinePx_) {
                if (!ensure(outline_, key, outlineFor(op.copy.glyph_font))) return false;
                cached->outlineGlyphs[i] = &outline_.glyphs.at(key);
            }
        }
        cached->ready = true;
        return true;
    }

    CachedText* getText(const std::string& string) {
        auto found = texts_.find(string);
        if (found != texts_.end()) {
            recency_.splice(recency_.end(), recency_, found->second.lru);
            return &found->second;
        }
        TTF_Text* text = TTF_CreateText(&engine_, font_, string.c_str(), string.size());
        if (!text) return nullptr;
        if (!TTF_UpdateText(text)) { TTF_DestroyText(text); return nullptr; }
        int width = 0;
        if (!TTF_GetTextSize(text, &width, nullptr)) {
            TTF_DestroyText(text);
            return nullptr;
        }
        // Keep recently used rows across selections without retaining an
        // unbounded number of SDL_ttf layouts.
        if (texts_.size() >= 1024) {
            auto oldest = texts_.find(recency_.front());
            TTF_DestroyText(oldest->second.text);
            texts_.erase(oldest);
            recency_.pop_front();
        }
        recency_.push_back(string);
        CachedText cached;
        cached.text = text;
        cached.width = width;
        for (int i = 0; i < text->internal->num_ops; ++i) {
            if (text->internal->ops[i].cmd == TTF_DRAW_COMMAND_COPY) {
                cached.hasGlyph = true;
                break;
            }
        }
        cached.lru = std::prev(recency_.end());
        return &texts_.emplace(string, std::move(cached)).first->second;
    }

    TTF_Font* outlineFor(TTF_Font* font) {
        if (font == font_) return outlineFont_;
        auto found = fallbackOutlines_.find(font);
        if (found != fallbackOutlines_.end()) return found->second;
        TTF_Font* copy = TTF_CopyFont(font);
        if (!copy) return nullptr;
        if (!TTF_SetFontOutline(copy, outlinePx_)) {
            TTF_CloseFont(copy);
            return nullptr;
        }
        fallbackOutlines_.emplace(font, copy);
        return copy;
    }

    bool ensure(Atlas& atlas, const GlyphKey& key, TTF_Font* rasterFont) {
        if (atlas.glyphs.find(key) != atlas.glyphs.end()) return true;
        if (!rasterFont) return false;
        TTF_ImageType imageType = TTF_IMAGE_INVALID;
        SDL_Surface* surface = TTF_GetGlyphImageForIndex(rasterFont, key.index, &imageType);
        if (!surface) return false;
        if (surface->format != SDL_PIXELFORMAT_ARGB8888) {
            SDL_Surface* converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_ARGB8888);
            SDL_DestroySurface(surface);
            if (!converted) return false;
            surface = converted;
        }
        const int w = surface->w;
        const int h = surface->h;
        if (w + 2 > pageSize_ || h + 2 > pageSize_) {
            SDL_DestroySurface(surface);
            return SDL_SetError("Text engine glyph exceeds atlas page");
        }
        if (atlas.pages.empty() || !fits(atlas.pages.back(), w, h)) {
            if (!addPage(atlas)) { SDL_DestroySurface(surface); return false; }
        }
        Page& page = atlas.pages.back();
        if (page.nextX + w + 1 > pageSize_) {
            page.nextX = 1;
            page.nextY += page.rowHeight + 1;
            page.rowHeight = 0;
        }
        const SDL_Rect src{page.nextX, page.nextY, w, h};
        // Only the sampled rectangle needs initialization. Its transparent
        // one-pixel border keeps linear filtering from reading neighboring
        // glyphs, without clearing and uploading the entire atlas page.
        std::vector<Uint8> pixels(static_cast<size_t>(w + 2) * (h + 2) * 4, 0);
        const int pitch = (w + 2) * 4;
        for (int row = 0; row < h; ++row) {
            std::memcpy(pixels.data() + (row + 1) * pitch + 4,
                static_cast<const Uint8*>(surface->pixels) + row * surface->pitch,
                static_cast<size_t>(w) * 4);
        }
        const SDL_Rect upload{src.x - 1, src.y - 1, w + 2, h + 2};
        const bool uploaded = SDL_UpdateTexture(page.texture, &upload, pixels.data(), pitch);
        SDL_DestroySurface(surface);
        if (!uploaded) return false;
        atlas.glyphs.emplace(key, Glyph{atlas.pages.size() - 1, src, imageType});
        page.nextX += w + 1;
        page.rowHeight = std::max(page.rowHeight, h);
        return true;
    }

    bool fits(const Page& page, int w, int h) const {
        if (page.nextX + w + 1 <= pageSize_ &&
            page.nextY + std::max(page.rowHeight, h) + 1 <= pageSize_) return true;
        return page.nextY + page.rowHeight + h + 2 <= pageSize_;
    }

    bool addPage(Atlas& atlas) {
        SDL_Texture* texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STATIC, pageSize_, pageSize_);
        if (!texture) return false;
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
        atlas.pages.push_back(Page{texture});
        return true;
    }

    void resetAtlases() {
        for (auto& page : fill_.pages) SDL_DestroyTexture(page.texture);
        for (auto& page : outline_.pages) SDL_DestroyTexture(page.texture);
        fill_ = Atlas{};
        outline_ = Atlas{};
        for (auto& entry : texts_) {
            entry.second.ready = false;
            entry.second.fillGlyphs.clear();
            entry.second.outlineGlyphs.clear();
        }
        asciiPrewarmed_ = false;
    }

    bool createSolidTexture() {
        solidTexture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STATIC, 1, 1);
        if (!solidTexture_) return false;
        const Uint32 white = 0xffffffffu;
        if (!SDL_UpdateTexture(solidTexture_, nullptr, &white, sizeof(white)) ||
            !SDL_SetTextureBlendMode(solidTexture_, SDL_BLENDMODE_BLEND)) {
            SDL_DestroyTexture(solidTexture_);
            solidTexture_ = nullptr;
            return false;
        }
        return true;
    }

    int pageSize_ = 512;
    static constexpr size_t maxAtlasPages_ = 16;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* solidTexture_ = nullptr;
    TTF_Font* font_ = nullptr;
    TTF_Font* outlineFont_ = nullptr;
    int outlinePx_ = 0;
    SDL_Color outlineColor_{};
    bool gradient_ = false;
    bool asciiPrewarmed_ = false;
    TTF_TextEngine engine_{};
    std::unordered_map<std::string, CachedText> texts_;
    std::list<std::string> recency_;
    std::unordered_map<TTF_Font*, TTF_Font*> fallbackOutlines_;
    Atlas fill_;
    Atlas outline_;
};
