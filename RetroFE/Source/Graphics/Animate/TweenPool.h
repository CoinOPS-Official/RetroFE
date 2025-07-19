#pragma once

#include "../Animate/Tween.h" // Adjust path as needed
#include <vector>
#include <memory>

class TweenPool {
public:
    // Singleton access method
    static TweenPool& getInstance();

    // Gets a pre-allocated Tween from the pool
    Tween* acquire(TweenProperty property, TweenAlgorithm type, float start, float end, float duration, const std::string& playlistFilter = "");

    // Returns a Tween to the pool so it can be reused
    void release(Tween* tween);

    // Delete copy and move constructors for Singleton pattern
    TweenPool(const TweenPool&) = delete;
    void operator=(const TweenPool&) = delete;

private:
    // Private constructor for Singleton
    TweenPool(size_t initialSize = 500);
    ~TweenPool() = default;

    // This vector OWNS all the tweens for their entire lifetime
    std::vector<std::unique_ptr<Tween>> m_pool;

    // This vector stores raw pointers to tweens that are currently available for use
    std::vector<Tween*> m_available_tweens;
};