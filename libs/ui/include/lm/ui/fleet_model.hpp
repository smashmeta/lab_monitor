#pragma once

#include <QAbstractTableModel>
#include <QVector>
#include <vector>

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
        RevisionColumn,
        LastSeenColumn,
        ColumnCount
    };

    enum Role {
        HostIdRole = Qt::UserRole + 1,
        SeverityRole,  ///< lower sorts first
        StateRole,
        StaleRole,
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

private:
    struct Row {
        core::FleetEntry entry;
        core::ResourceSample resources;
        bool has_resources = false;
    };

    [[nodiscard]] int index_of(const core::HostId& host_id) const;

    std::vector<Row> rows_;  ///< always sorted by entry.host_id
};

}  // namespace lm::ui
