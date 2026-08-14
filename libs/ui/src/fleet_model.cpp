#include "lm/ui/fleet_model.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>

#include <QDateTime>
#include <QString>

namespace lm::ui {
namespace {

/// Lower sorts first. Mirrors lm::core::fleet's (internal) urgency ordering:
/// Missing, Offline, Unexpected, Online.
int severity_of(core::HostState state) {
    switch (state) {
        case core::HostState::Missing:    return 0;
        case core::HostState::Offline:    return 1;
        case core::HostState::Unexpected: return 2;
        case core::HostState::Online:     return 3;
    }
    return 4;
}

QString format_last_seen(const std::optional<core::TimePoint>& last_seen) {
    if (!last_seen.has_value()) {
        return QStringLiteral("Never");
    }
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(last_seen->time_since_epoch()).count();
    const QDateTime timestamp = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(seconds), Qt::UTC);
    return timestamp.toString(Qt::ISODate);
}

}  // namespace

FleetModel::FleetModel(QObject* parent) : QAbstractTableModel(parent) {}

int FleetModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int FleetModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(ColumnCount);
}

QVariant FleetModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        static_cast<std::size_t>(index.row()) >= rows_.size()) {
        return {};
    }
    const Row& row = rows_[static_cast<std::size_t>(index.row())];

    switch (role) {
        case HostIdRole:
            return QString::fromStdString(row.entry.host_id);
        case SeverityRole:
            return severity_of(row.entry.state);
        case StateRole:
            return static_cast<int>(row.entry.state);
        case StaleRole:
            return row.entry.stale;
        default:
            break;
    }

    if (role != Qt::DisplayRole && role != Qt::ToolTipRole) {
        return {};
    }

    switch (index.column()) {
        case HostColumn:
            return role == Qt::ToolTipRole ? QString::fromStdString(row.entry.address)
                                            : QString::fromStdString(row.entry.host_id);
        case StateColumn:
            return QString::fromStdString(core::to_string(row.entry.state));
        case CpuColumn:
            return row.has_resources
                       ? QString::number(row.resources.cpu_percent, 'f', 1) + QStringLiteral("%")
                       : QStringLiteral("-");
        case MemoryColumn: {
            if (!row.has_resources || row.resources.mem_total_bytes == 0) {
                return QStringLiteral("-");
            }
            const double percent = 100.0 * static_cast<double>(row.resources.mem_used_bytes) /
                                   static_cast<double>(row.resources.mem_total_bytes);
            return QString::number(percent, 'f', 1) + QStringLiteral("%");
        }
        case DiskColumn: {
            if (!row.has_resources || row.resources.disks.empty()) {
                return QStringLiteral("-");
            }
            double worst = 0.0;
            for (const auto& disk : row.resources.disks) {
                worst = std::max(worst, disk.used_percent());
            }
            return QString::number(worst, 'f', 1) + QStringLiteral("%");
        }
        case RevisionColumn:
            return row.entry.stale ? QStringLiteral("Stale") : QStringLiteral("Current");
        case LastSeenColumn:
            return format_last_seen(row.entry.last_seen);
        default:
            return {};
    }
}

QVariant FleetModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    switch (section) {
        case HostColumn:     return QStringLiteral("Host");
        case StateColumn:    return QStringLiteral("State");
        case CpuColumn:      return QStringLiteral("CPU");
        case MemoryColumn:   return QStringLiteral("Memory");
        case DiskColumn:     return QStringLiteral("Disk");
        case RevisionColumn: return QStringLiteral("Revision");
        case LastSeenColumn: return QStringLiteral("Last Seen");
        default:             return {};
    }
}

void FleetModel::apply(const core::FleetView& view) {
    std::vector<core::FleetEntry> incoming = view.entries;
    std::ranges::sort(incoming, {}, &core::FleetEntry::host_id);

    std::size_t old_index = 0;
    std::size_t new_index = 0;

    while (old_index < rows_.size() || new_index < incoming.size()) {
        const bool have_old = old_index < rows_.size();
        const bool have_new = new_index < incoming.size();

        if (have_old && have_new &&
            rows_[old_index].entry.host_id == incoming[new_index].host_id) {
            const core::FleetEntry& old_entry = rows_[old_index].entry;
            const core::FleetEntry& new_entry = incoming[new_index];
            if (!(old_entry == new_entry)) {
                const bool host_or_state_changed = old_entry.host_id != new_entry.host_id ||
                                                   old_entry.address != new_entry.address ||
                                                   old_entry.state != new_entry.state ||
                                                   old_entry.caps != new_entry.caps;
                const bool revision_or_seen_changed = old_entry.stale != new_entry.stale ||
                                                      old_entry.last_seen != new_entry.last_seen;

                rows_[old_index].entry = new_entry;

                const int row = static_cast<int>(old_index);
                if (host_or_state_changed) {
                    emit dataChanged(index(row, HostColumn), index(row, StateColumn));
                }
                if (revision_or_seen_changed) {
                    emit dataChanged(index(row, RevisionColumn), index(row, LastSeenColumn));
                }
            }
            ++old_index;
            ++new_index;
        } else if (have_new &&
                   (!have_old || incoming[new_index].host_id < rows_[old_index].entry.host_id)) {
            Row row;
            row.entry = incoming[new_index];
            const int position = static_cast<int>(old_index);
            beginInsertRows(QModelIndex(), position, position);
            rows_.insert(rows_.begin() + static_cast<std::ptrdiff_t>(old_index), std::move(row));
            endInsertRows();
            ++old_index;
            ++new_index;
        } else {
            const int position = static_cast<int>(old_index);
            beginRemoveRows(QModelIndex(), position, position);
            rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(old_index));
            endRemoveRows();
            // rows_ shifted left; old_index now already points at the next old row.
        }
    }
}

void FleetModel::apply_sample(const transport::ResourceSampleMessage& sample) {
    const int row = index_of(sample.host_id);
    if (row < 0) {
        return;
    }
    Row& target = rows_[static_cast<std::size_t>(row)];
    if (target.has_resources && target.resources == sample.sample) {
        return;
    }
    target.resources = sample.sample;
    target.has_resources = true;
    emit dataChanged(index(row, CpuColumn), index(row, DiskColumn));
}

int FleetModel::index_of(const core::HostId& host_id) const {
    const auto it = std::lower_bound(rows_.begin(), rows_.end(), host_id,
                                     [](const Row& row, const core::HostId& id) {
                                         return row.entry.host_id < id;
                                     });
    if (it == rows_.end() || it->entry.host_id != host_id) {
        return -1;
    }
    return static_cast<int>(std::distance(rows_.begin(), it));
}

}  // namespace lm::ui
