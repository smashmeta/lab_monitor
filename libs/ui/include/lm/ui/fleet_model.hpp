#pragma once

#include <QAbstractTableModel>
#include <QColor>
#include <QVector>

#include <optional>
#include <vector>

#include "lm/core/compliance.hpp"
#include "lm/core/fleet.hpp"
#include "lm/transport/messages.hpp"

namespace lm::ui {

/// Rows are held in a stable order (by host id) and are never reordered. Wrap
/// this in a QSortFilterProxyModel with sortRole == SeverityRole to present the
/// most-urgent-first order without churning rows in the view.
class FleetModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        HostColumn = 0,
        StateColumn,
        CpuColumn,
        MemoryColumn,
        DiskColumn,
        AdaptersColumn,
        RevisionColumn,
        LastSeenColumn,
        ColumnCount
    };

    /// How a row should read at a glance. Blends liveness with compliance,
    /// because "is it there?" and "is it correct?" are both things an operator
    /// scans for, and a machine that is not there cannot usefully be described
    /// by the last rules it happened to fail -- so state wins over compliance.
    enum class RowHealth {
        Unexpected,  ///< reporting but not in the expected list
        Missing,     ///< expected, never seen
        Offline,     ///< expected, seen before, now silent
        Failing,     ///< online, at least one rule failing
        Compliant,   ///< online, no rule failing
        Unknown,     ///< online, no compliance report yet
    };

    enum Role {
        HostIdRole = Qt::UserRole + 1,
        SeverityRole,  ///< lower sorts first
        StateRole,
        StaleRole,
        /// core::Capabilities::raw(). Lets a view tell "the client cannot
        /// report this" apart from "there is none of it".
        CapabilitiesRole,
    };

    explicit FleetModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role) const override;

    /// Merges a reconciled view: inserts new hosts, removes departed ones, and
    /// emits dataChanged only for cells that actually changed.
    void apply(const core::FleetView& view);

    /// Updates only the CPU, memory and disk columns for one host.
    void apply_sample(const transport::ResourceSampleMessage& sample);

    /// Records a host's latest compliance result so its row can be coloured.
    /// core::FleetEntry carries liveness but not compliance, and reports arrive
    /// on their own topic, so the two are merged here rather than in reconcile().
    /// A report for an unknown host is ignored.
    void apply_compliance(const core::ComplianceReport& report);

    [[nodiscard]] RowHealth health_of(int row) const;

    /// The colour a row of this health paints in. Static so the legend can use
    /// the same mapping the rows do, rather than duplicating it.
    [[nodiscard]] static QColor colour_for(RowHealth health);

private:
    struct Row {
        core::FleetEntry entry;
        core::ResourceSample resources;
        bool has_resources = false;
        /// Tri-state rather than bool: "no report yet" must not read as
        /// compliant, or an unchecked machine shows a green light.
        std::optional<bool> compliant;
    };

    [[nodiscard]] int index_of(const core::HostId& host_id) const;

    /// The percentage a resource column is reporting, or nullopt when it has
    /// nothing to report. One source for both the number and the colour, so the
    /// two can never disagree about what the machine is doing.
    [[nodiscard]] static std::optional<double> load_percent(const Row& row, int column);

    std::vector<Row> rows_;  ///< always sorted by entry.host_id
};

}  // namespace lm::ui
