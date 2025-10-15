#include "PlaylistDirtyRegistry.h"
#include "PlaylistPath.h"

// Define static members
std::mutex PlaylistDirtyRegistry::m_;
std::map<std::string, std::set<std::string>> PlaylistDirtyRegistry::dirty_;

void PlaylistDirtyRegistry::addPath(const std::string& localPath) {
    std::string col, pl;
    if (!PlaylistPath::tryParse(localPath, col, pl)) return;

    std::lock_guard<std::mutex> lk(m_);
    dirty_[col].insert(pl);
}

std::vector<std::string> PlaylistDirtyRegistry::drainForCollection(const std::string& collection) {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lk(m_);
    auto it = dirty_.find(collection);
    if (it == dirty_.end()) return out;
    out.reserve(it->second.size());
    for (auto& name : it->second)
        out.push_back(name);
    dirty_.erase(it);
    return out;
}

void PlaylistDirtyRegistry::clear() {
    std::lock_guard<std::mutex> lk(m_);
    dirty_.clear();
}

bool PlaylistDirtyRegistry::isDirty(const std::string& collection, const std::string& playlist) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = dirty_.find(collection);
    if (it == dirty_.end()) return false;
    return it->second.find(playlist) != it->second.end();
}

void PlaylistDirtyRegistry::clearOne(const std::string& collection, const std::string& playlist) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = dirty_.find(collection);
    if (it == dirty_.end()) return;
    it->second.erase(playlist);
    if (it->second.empty())
        dirty_.erase(it);
}
