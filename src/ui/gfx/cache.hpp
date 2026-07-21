#pragma once
// LRU bake cache — shaders are free to be expensive; frames are cheap to blit.

#include <list>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "src/ui/gfx/cell_buffer.hpp"
#include "src/ui/gfx/shader.hpp"

namespace cortex::mk3::ui::gfx {

class ShaderCache {
   public:
    explicit ShaderCache(size_t cap = 48) : capacity_(cap < 4 ? 4 : cap) {}

    const CellBuffer* get(const BakeKey& key) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = map_.find(key);
        if (it == map_.end()) return nullptr;
        // move to front
        order_.splice(order_.begin(), order_, it->second.second);
        return &it->second.first;
    }

    const CellBuffer& put(const BakeKey& key, CellBuffer buf) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second.first = std::move(buf);
            order_.splice(order_.begin(), order_, it->second.second);
            return it->second.first;
        }
        order_.push_front(key);
        auto& slot = map_[key];
        slot.first = std::move(buf);
        slot.second = order_.begin();
        while (map_.size() > capacity_) {
            const BakeKey& old = order_.back();
            map_.erase(old);
            order_.pop_back();
        }
        return map_[key].first;
    }

    const CellBuffer& getOrBake(const Shader& shader, ShaderEnv env) {
        if (shader.buckets < 1) env.timeBucket = 0;
        else env.timeBucket = env.timeBucket % shader.buckets;

        BakeKey key;
        key.shaderId = shader.id;
        key.w = env.w;
        key.h = env.h;
        key.timeBucket = env.timeBucket;
        key.variant = env.variant;
        key.uniforms = env.uniforms;

        if (const CellBuffer* hit = get(key)) return *hit;
        return put(key, bake(shader, env));
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mu_);
        return map_.size();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        map_.clear();
        order_.clear();
    }

   private:
    size_t capacity_;
    mutable std::mutex mu_;
    std::list<BakeKey> order_;
    std::unordered_map<BakeKey, std::pair<CellBuffer, std::list<BakeKey>::iterator>, BakeKeyHash>
        map_;
};

// Process-wide cache — shared by palette, hub bg, future widgets.
inline ShaderCache& globalShaderCache() {
    static ShaderCache cache(64);
    return cache;
}

}  // namespace cortex::mk3::ui::gfx
