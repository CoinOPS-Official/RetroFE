// Standalone CPU benchmark for the menu traversal in Page::prepareVideoFrames
// and Page::draw. Build the Release target retrofe_page_traversal_benchmark.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <vector>

namespace {
constexpr unsigned int kLayers = 20;
constexpr int kFrames = 20000;
constexpr int kTrials = 5;

struct Component {
    unsigned int layer;
    int monitor;
    std::uint64_t id;

    virtual ~Component() = default;
    virtual void visit(std::uint64_t& checksum) const = 0;
};

struct BenchComponent final : Component {
    void visit(std::uint64_t& checksum) const override {
        checksum += (id + 1) * (layer + 1);
    }
};

struct Menu {
    std::vector<Component*> components;
};

struct Workload {
    std::vector<std::unique_ptr<BenchComponent>> owned;
    std::vector<std::vector<Menu>> menus;
};

volatile std::uint64_t sink = 0;

Workload makeWorkload(int listCount, int slotsPerList) {
    Workload workload;
    workload.menus.resize(2);
    workload.owned.reserve(listCount * slotsPerList);
    for (int list = 0; list < listCount; ++list) {
        Menu menu;
        menu.components.reserve(slotsPerList);
        for (int slot = 0; slot < slotsPerList; ++slot) {
            auto component = std::make_unique<BenchComponent>();
            component->id = workload.owned.size();
            component->layer = static_cast<unsigned int>((list * 7 + slot) % kLayers);
            component->monitor = (slot % 7 == 0) ? 1 : 0;
            menu.components.push_back(component.get());
            workload.owned.push_back(std::move(component));
        }
        workload.menus[list % 2].push_back(std::move(menu));
    }
    return workload;
}

void resetLayers(Workload& workload) {
    for (const auto& component : workload.owned) {
        component->layer = static_cast<unsigned int>(component->id % kLayers);
    }
}

std::uint64_t oldTraversal(Workload& workload, int frames) {
    std::uint64_t checksum = 0;
    for (int frame = 0; frame < frames; ++frame) {
        workload.owned[(frame * 17u) % workload.owned.size()]->layer = frame % kLayers;
        for (int pass = 0; pass < 2; ++pass) {
            for (unsigned int layer = 0; layer < kLayers; ++layer) {
                for (const auto& menuList : workload.menus) {
                    for (const Menu& menu : menuList) {
                        for (Component* component : menu.components) {
                            if (component && component->layer == layer && component->monitor == 0) {
                                component->visit(checksum);
                            }
                        }
                    }
                }
            }
        }
    }
    sink = checksum;
    return checksum;
}

std::uint64_t groupedTraversal(Workload& workload, int frames) {
    std::array<std::vector<Component*>, kLayers> buckets;
    std::uint64_t checksum = 0;
    for (int frame = 0; frame < frames; ++frame) {
        workload.owned[(frame * 17u) % workload.owned.size()]->layer = frame % kLayers;
        for (int pass = 0; pass < 2; ++pass) {
            for (auto& bucket : buckets) bucket.clear();
            for (const auto& menuList : workload.menus) {
                for (const Menu& menu : menuList) {
                    for (Component* component : menu.components) {
                        if (component && component->monitor == 0 && component->layer < kLayers) {
                            buckets[component->layer].push_back(component);
                        }
                    }
                }
            }
            for (const auto& bucket : buckets) {
                for (Component* component : bucket) component->visit(checksum);
            }
        }
    }
    sink = checksum;
    return checksum;
}

template <typename Traversal>
double measure(Workload& workload, Traversal traversal, std::uint64_t& checksum) {
    resetLayers(workload);
    const auto start = std::chrono::steady_clock::now();
    checksum = traversal(workload, kFrames);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return std::chrono::duration<double, std::micro>(elapsed).count() / kFrames;
}

double median(std::vector<double>& samples) {
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

void benchmark(int lists, int slots) {
    Workload workload = makeWorkload(lists, slots);
    oldTraversal(workload, 1000);
    groupedTraversal(workload, 1000);

    std::vector<double> oldSamples;
    std::vector<double> groupedSamples;
    for (int trial = 0; trial < kTrials; ++trial) {
        std::uint64_t oldChecksum = 0;
        std::uint64_t groupedChecksum = 0;
        if (trial % 2 == 0) {
            oldSamples.push_back(measure(workload, oldTraversal, oldChecksum));
            groupedSamples.push_back(measure(workload, groupedTraversal, groupedChecksum));
        } else {
            groupedSamples.push_back(measure(workload, groupedTraversal, groupedChecksum));
            oldSamples.push_back(measure(workload, oldTraversal, oldChecksum));
        }
        if (oldChecksum != groupedChecksum) {
            std::cerr << "Traversal checksum mismatch\n";
            std::exit(1);
        }
    }

    const double oldUs = median(oldSamples);
    const double groupedUs = median(groupedSamples);
    std::cout << lists << " lists x " << slots << " slots: old=" << oldUs
              << " us/frame, grouped=" << groupedUs << " us/frame, speedup="
              << oldUs / groupedUs << "x\n";
}
} // namespace

int main() {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "CPU traversal only; two passes/frame, " << kFrames
              << " frames/trial, median of " << kTrials << " trials\n";
    benchmark(1, 12);
    benchmark(4, 20);
    benchmark(8, 20);
}
