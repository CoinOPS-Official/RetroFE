#include "HighScoreChange.h"

#include <algorithm>

namespace {
bool tablesEqual(const HighScoreTableView& lhs, const HighScoreTableView& rhs) {
    return highScoreTableSchemasEqual(lhs, rhs) &&
        lhs.rows == rhs.rows &&
        lhs.isPlaceholder == rhs.isPlaceholder;
}
}

bool HighScoreViewChange::anyChange() const {
    return structureChanged || !changedTableIndices.empty();
}

bool HighScoreViewChange::affectsAnyTable(
    const std::vector<std::size_t>& tableIndices) const {
    if (structureChanged) return true;
    return std::any_of(tableIndices.begin(), tableIndices.end(), [&](std::size_t tableIndex) {
        return std::find(changedTableIndices.begin(), changedTableIndices.end(), tableIndex) !=
            changedTableIndices.end();
    });
}

bool highScoreTableSchemasEqual(
    const HighScoreTableView& lhs,
    const HighScoreTableView& rhs) {
    return lhs.id == rhs.id &&
        lhs.columns == rhs.columns &&
        lhs.columnInfo == rhs.columnInfo;
}

bool highScoreTableChangeIsRowLocal(
    const HighScoreTableView& previous,
    const HighScoreTableView& current) {
    if (!highScoreTableSchemasEqual(previous, current) ||
        previous.isPlaceholder != current.isPlaceholder ||
        previous.rows.size() != current.rows.size()) {
        return false;
    }

    std::size_t changedRows = 0;
    for (std::size_t row = 0; row < previous.rows.size(); ++row) {
        if (previous.rows[row] == current.rows[row]) continue;
        if (++changedRows > 1) return false;
    }
    return changedRows == 1;
}

HighScoreViewChange compareHighScoreViews(
    const HighScoreView& previous,
    const HighScoreView& current) {
    HighScoreViewChange change;
    if (previous.tables.size() != current.tables.size()) {
        change.structureChanged = true;
        return change;
    }

    for (std::size_t tableIndex = 0; tableIndex < previous.tables.size(); ++tableIndex) {
        if (!highScoreTableSchemasEqual(previous.tables[tableIndex], current.tables[tableIndex])) {
            change.structureChanged = true;
            return change;
        }
    }

    for (std::size_t tableIndex = 0; tableIndex < previous.tables.size(); ++tableIndex) {
        if (!tablesEqual(previous.tables[tableIndex], current.tables[tableIndex])) {
            change.changedTableIndices.push_back(tableIndex);
        }
    }
    return change;
}

std::optional<std::size_t> findCorrespondingHighScoreTable(
    const HighScoreTableView& previousTable,
    const HighScoreView& current,
    std::size_t preferredIndex) {
    if (!previousTable.id.empty()) {
        std::optional<std::size_t> matchingId;
        for (std::size_t index = 0; index < current.tables.size(); ++index) {
            if (current.tables[index].id != previousTable.id) continue;
            if (matchingId) {
                matchingId.reset();
                break;
            }
            matchingId = index;
        }
        if (matchingId) return matchingId;
    }

    std::optional<std::size_t> matchingSchema;
    for (std::size_t index = 0; index < current.tables.size(); ++index) {
        if (!highScoreTableSchemasEqual(previousTable, current.tables[index])) continue;
        if (matchingSchema) {
            matchingSchema.reset();
            break;
        }
        matchingSchema = index;
    }
    if (matchingSchema) return matchingSchema;
    if (preferredIndex < current.tables.size()) return preferredIndex;
    return std::nullopt;
}

HighScoreTransitionScope chooseHighScoreTransitionScope(
    const HighScoreViewChange& change,
    const std::vector<std::size_t>& previousPageTables,
    const std::vector<std::size_t>& currentPageTables,
    bool stablePanelGeometry,
    bool stableScrollPosition) {
    if (!change.anyChange()) {
        return HighScoreTransitionScope::None;
    }
    if (change.structureChanged || previousPageTables != currentPageTables) {
        return HighScoreTransitionScope::WholePage;
    }
    if (!change.affectsAnyTable(previousPageTables)) {
        return HighScoreTransitionScope::None;
    }
    if (
        !stablePanelGeometry || !stableScrollPosition) {
        return HighScoreTransitionScope::WholePage;
    }
    return HighScoreTransitionScope::ChangedTables;
}
