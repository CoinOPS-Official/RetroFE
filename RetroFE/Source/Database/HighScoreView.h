#pragma once

#include <string>
#include <cstdint>
#include <vector>

enum class HighScoreDisplayLevel {
    Always,
    Extra,
    Debug
};

struct HighScoreColumnView {
    std::string id;
    std::string source;
    HighScoreDisplayLevel display = HighScoreDisplayLevel::Always;

    bool operator==(const HighScoreColumnView&) const = default;
};

enum class HighScoreUpdateOrigin {
    Initial,
    LiveBaseline,
    LiveChange,
    FileRefresh
};

struct HighScoreTableView {
    std::string id;
    std::vector<std::string> columns;
    std::vector<HighScoreColumnView> columnInfo;
    std::vector<std::vector<std::string>> rows;
    std::vector<std::vector<bool>> isPlaceholder;
};

struct HighScoreView {
    std::vector<HighScoreTableView> tables;
};

struct HighScoreSnapshot {
    HighScoreView view;
    uint64_t revision = 0;
    HighScoreUpdateOrigin origin = HighScoreUpdateOrigin::Initial;
};
