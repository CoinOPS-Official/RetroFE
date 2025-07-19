#include "TweenPool.h" // Adjust path as needed
#include "../../Utility/Log.h"

TweenPool& TweenPool::getInstance() {
    static TweenPool instance;
    return instance;
}

TweenPool::TweenPool(size_t initialSize) {
    m_pool.reserve(initialSize);
    m_available_tweens.reserve(initialSize);

    // Pre-allocate all the tweens at once in a contiguous block of memory
    for (size_t i = 0; i < initialSize; ++i) {
        // Create the tween with dummy values; it will be re-initialized on acquire
        m_pool.push_back(std::make_unique<Tween>(TWEEN_PROPERTY_NOP, LINEAR, 0.0f, 0.0f, 0.0f));
        // Add the raw pointer to the available list
        m_available_tweens.push_back(m_pool.back().get());
    }
}

Tween* TweenPool::acquire(TweenProperty property, TweenAlgorithm type, float start, float end, float duration, const std::string& playlistFilter) {
    if (m_available_tweens.empty()) {
        // This should not happen if the pool is sized correctly.
        // In a real-world app, you might expand the pool here.
        LOG_ERROR("TweenPool", "TweenPool exhausted! Increase initialSize.");
        return nullptr;
    }

    // Get a tween from the back of the available list (very fast)
    Tween* tween = m_available_tweens.back();
    m_available_tweens.pop_back();

    // Initialize it with the new properties
    tween->reinit(property, type, start, end, duration, playlistFilter);

    return tween;
}

void TweenPool::release(Tween* tween) {
    // Simply add the pointer back to the available list for reuse (very fast)
    m_available_tweens.push_back(tween);
}