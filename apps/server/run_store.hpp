#pragma once

#include <QString>

#include <expected>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "script_run.hpp"

/// A run as stored. One file per run, written once when the run finishes and
/// never mutated: deletion is a file delete, and a corrupt file costs one run
/// rather than the whole history.
[[nodiscard]] nlohmann::json run_to_json(const ScriptRun& run);

/// The inverse. An error string rather than a partially-filled run, because a
/// half-read run in the history is worse than a named gap.
[[nodiscard]] std::expected<ScriptRun, std::string> run_from_json(const nlohmann::json& document);

/// Writes `<runs_dir>/<run_id>.json`, creating the directory. False on failure
/// with `error` set when it is not null.
bool save_run(const QString& runs_dir, const ScriptRun& run, QString* error);

/// Every readable run in the directory, in no particular order. Unreadable
/// files are skipped and appended to `errors` when it is not null -- one bad
/// file must not cost the rest.
[[nodiscard]] std::vector<ScriptRun> load_runs(const QString& runs_dir,
                                               std::vector<QString>* errors);

/// Removes one run's file. False when it was not there.
bool delete_run(const QString& runs_dir, const std::string& run_id, QString* error);
