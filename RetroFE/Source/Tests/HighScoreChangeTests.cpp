#include "../Database/HighScoreChange.h"

#include <cstdlib>
#include <iostream>
#include <utility>

namespace {
HighScoreTableView table(
    std::string id,
    std::vector<std::vector<std::string>> rows,
    std::string valueSource = "SCORE") {
    HighScoreTableView result;
    result.id = std::move(id);
    result.columns = {"RANK", "SCORE"};
    result.columnInfo = {
        {"RANK", "index", HighScoreDisplayLevel::Always},
        {"SCORE", std::move(valueSource), HighScoreDisplayLevel::Always}
    };
    result.rows = std::move(rows);
    return result;
}

void require(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(EXIT_FAILURE);
}
}

int main() {
    const HighScoreView previous{{
        table("A", {{"1", "100"}}),
        table("B", {{"1", "200"}})
    }};

    HighScoreView current = previous;
    current.tables[1].rows[0][1] = "250";
    const auto contentChange = compareHighScoreViews(previous, current);
    require(!contentChange.structureChanged, "a value change must not be structural");
    require(contentChange.changedTableIndices == std::vector<std::size_t>{1},
        "only the modified table must be reported");
    require(!contentChange.affectsAnyTable({0}), "an off-screen table must not affect table zero");
    require(contentChange.affectsAnyTable({1}), "the changed table must be affected");
    require(chooseHighScoreTransitionScope(contentChange, {0}, {0}, true, true) ==
        HighScoreTransitionScope::None,
        "an off-screen change must not transition the visible page");
    require(chooseHighScoreTransitionScope(contentChange, {0}, {0, 1}, true, true) ==
        HighScoreTransitionScope::WholePage,
        "a changed page composition must transition the whole page");
    require(chooseHighScoreTransitionScope(contentChange, {0, 1}, {0, 1}, true, true) ==
        HighScoreTransitionScope::ChangedTables,
        "stable visible content must use a panel transition");
    require(chooseHighScoreTransitionScope(contentChange, {0, 1}, {0, 1}, false, true) ==
        HighScoreTransitionScope::WholePage,
        "shared geometry changes must transition the whole page");
    require(chooseHighScoreTransitionScope(contentChange, {0, 1}, {0, 1}, true, false) ==
        HighScoreTransitionScope::WholePage,
        "scroll-position changes must transition the whole page");

    current = previous;
    current.tables[0].columnInfo[1].source = "TIME";
    const auto schemaChange = compareHighScoreViews(previous, current);
    require(schemaChange.structureChanged, "column-source changes must be structural");

    current = previous;
    current.tables.insert(current.tables.begin(), table("NEW", {{"1", "300"}}));
    const auto insertedTable = compareHighScoreViews(previous, current);
    require(insertedTable.structureChanged, "table insertion must be structural");
    require(chooseHighScoreTransitionScope(insertedTable, {0, 1}, {0, 1}, true, true) ==
        HighScoreTransitionScope::WholePage,
        "structural changes must transition the whole page");
    const auto matched = findCorrespondingHighScoreTable(previous.tables[1], current, 1);
    require(matched && *matched == 2, "a uniquely named table must survive index changes");

    HighScoreTableView unnamed = table("", {{"1", "400"}});
    HighScoreView unnamedCurrent{{unnamed}};
    const auto unnamedMatch = findCorrespondingHighScoreTable(unnamed, unnamedCurrent, 0);
    require(unnamedMatch && *unnamedMatch == 0, "an unnamed table must match a unique schema");

    HighScoreTableView localEdit = previous.tables[0];
    localEdit.rows[0][1] = "125";
    require(highScoreTableChangeIsRowLocal(previous.tables[0], localEdit),
        "a same-shape change confined to one row must be local");

    HighScoreTableView broadEdit = table("A", {{"1", "125"}, {"2", "90"}});
    HighScoreTableView broadPrevious = table("A", {{"1", "100"}, {"2", "95"}});
    require(!highScoreTableChangeIsRowLocal(broadPrevious, broadEdit),
        "changes spanning multiple rows must not be local");

    HighScoreTableView insertedRow = previous.tables[0];
    insertedRow.rows.push_back({"2", "90"});
    require(!highScoreTableChangeIsRowLocal(previous.tables[0], insertedRow),
        "row-count changes must not be local");

    HighScoreTableView localSchemaChange = localEdit;
    localSchemaChange.columnInfo[1].source = "TIME";
    require(!highScoreTableChangeIsRowLocal(previous.tables[0], localSchemaChange),
        "schema changes must not be local");

    return EXIT_SUCCESS;
}
