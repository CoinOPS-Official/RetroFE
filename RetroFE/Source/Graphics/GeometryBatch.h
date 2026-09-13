#pragma once

#include <SDL3/SDL.h>
#include <array>
#include <algorithm>

// A bounded, allocation-free sequence of adjacent geometry runs. Flush before
// changing render target, viewport, clip, texture blend/filter state, or atlas
// contents. Tint and alpha must already be captured in the supplied vertices.
// The caller owns all resources and explicitly flushes before they go away.
class GeometryBatch {
public:
    GeometryBatch() = default;
    GeometryBatch(const GeometryBatch&) = delete;
    GeometryBatch& operator=(const GeometryBatch&) = delete;

    // Offscreen atlas draws already have destination coordinates. Preserve the
    // texture modulation that SDL_RenderTexture would otherwise apply for them.
    bool appendTexture(SDL_Renderer* renderer, SDL_Texture* texture,
        const SDL_Rect& src, const SDL_FRect& dst) {
        if (!texture || texture->w <= 0 || texture->h <= 0) return false;
        if (src.w <= 0 || src.h <= 0 || dst.w <= 0 || dst.h <= 0) return true;
        SDL_FColor color{1, 1, 1, 1};
        if (!SDL_GetTextureColorModFloat(texture, &color.r, &color.g, &color.b) ||
            !SDL_GetTextureAlphaModFloat(texture, &color.a)) return false;
        const float u0 = float(src.x) / texture->w;
        const float v0 = float(src.y) / texture->h;
        const float u1 = float(src.x + src.w) / texture->w;
        const float v1 = float(src.y + src.h) / texture->h;
        const SDL_Vertex vertices[] = {
            {{dst.x, dst.y}, color, {u0, v0}},
            {{dst.x + dst.w, dst.y}, color, {u1, v0}},
            {{dst.x + dst.w, dst.y + dst.h}, color, {u1, v1}},
            {{dst.x, dst.y + dst.h}, color, {u0, v1}}
        };
        constexpr int indices[] = {0, 1, 2, 0, 2, 3};
        return append(renderer, texture, vertices, 4, indices, 6);
    }

    bool append(SDL_Renderer* renderer, SDL_Texture* texture,
        const SDL_Vertex* vertices, int vertexCount,
        const int* indices, int indexCount) {
        if (!renderer || vertexCount < 0 || indexCount < 0 ||
            vertexCount > static_cast<int>(vertices_.size()) ||
            indexCount > static_cast<int>(indices_.size()) ||
            (vertexCount && !vertices) || (indexCount && !indices)) {
            return SDL_SetError("Invalid geometry batch input");
        }
        if (!vertexCount || !indexCount) return true;
        if (renderer_ != renderer || texture_ != texture ||
            vertexCount_ + vertexCount > static_cast<int>(vertices_.size()) ||
            indexCount_ + indexCount > static_cast<int>(indices_.size())) {
            if (!flush()) return false;
        }
        renderer_ = renderer;
        texture_ = texture;
        std::copy_n(vertices, vertexCount, vertices_.data() + vertexCount_);
        for (int i = 0; i < indexCount; ++i) {
            indices_[indexCount_ + i] = indices[i] + vertexCount_;
        }
        vertexCount_ += vertexCount;
        indexCount_ += indexCount;
        return true;
    }

    bool flush() {
        bool result = true;
        if (indexCount_) {
            result = SDL_RenderGeometry(renderer_, texture_, vertices_.data(),
                vertexCount_, indices_.data(), indexCount_);
        }
        vertexCount_ = indexCount_ = 0;
        renderer_ = nullptr;
        texture_ = nullptr;
        return result;
    }

private:
    std::array<SDL_Vertex, 256 * 4> vertices_;
    std::array<int, 256 * 6> indices_;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    int vertexCount_ = 0;
    int indexCount_ = 0;
};
