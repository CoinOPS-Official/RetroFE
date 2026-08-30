#pragma once

#include "HighScoreView.h"

#include <cstddef>
#include <optional>
#include <vector>

struct HighScoreViewChange {
    bool structureChanged = false;
    std::vector<std::size_t> changedTableIndices;

    bool anyChange() const;
    bool affectsAnyTable(const std::vector<std::size_t>& tableIndices) const;
};

enum class HighScoreTransitionScope {
    None,
    ChangedTables,
    WholePage
};

bool highScoreTableSchemasEqual(
    const HighScoreTableView& lhs,
    const HighScoreTableView& rhs);

bool highScoreTableChangeIsRowLocal(
    const HighScoreTableView& previous,
    const HighScoreTableView& current);

HighScoreViewChange compareHighScoreViews(
    const HighScoreView& previous,
    const HighScoreView& current);

std::optional<std::size_t> findCorrespondingHighScoreTable(
    const HighScoreTableView& previousTable,
    const HighScoreView& current,
    std::size_t preferredIndex);

HighScoreTransitionScope chooseHighScoreTransitionScope(
    const HighScoreViewChange& change,
    const std::vector<std::size_t>& previousPageTables,
    const std::vector<std::size_t>& currentPageTables,
    bool stablePanelGeometry,
    bool stableScrollPosition);
