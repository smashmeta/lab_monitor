#include "lm/ui/fleet_model.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>

#include <QDateTime>
#include <QString>

#include "lm/ui/theme.hpp"

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

namespace {

/// One line per adapter, so the whole picture is a hover away without needing
/// the detail pane.
QString adapter_tooltip(const std::vector<core::NetworkAdapter>& adapters) {
    if (adapters.empty()) {
        return QStringLiteral("No network adapters reported.");
    }
    QStringList lines;
    lines.reserve(static_cast<int>(adapters.size()));
    for (const core::NetworkAdapter& adapter : adapters) {
        lines << QStringLiteral("%1  —  %2  (%3)")
                      .arg(QString::fromStdString(core::to_string(adapter.link)),
                            QString::fromStdString(adapter.name),
                            QString::fromStdString(core::to_string(adapter.type)));
    }
    return lines.join(QChar(u'\n'));
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
        case CapabilitiesRole:
            return row.entry.caps.raw();
        default:
            break;
    }

    // Colour the whole row by health, so an operator scanning the list sees
    // which machines need attention without reading a single word.
    if (role == Qt::ForegroundRole) {
        // Except the percentage columns, which report their own reading: a
        // machine can be perfectly compliant and out of disk, and the row's
        // health colour hides precisely that. Only while the host is actually
        // reporting, though -- the last sample from one that has gone quiet is
        // stale, and painting it green would read as a live, idle machine.
        if (row.entry.state == core::HostState::Online) {
            if (const std::optional<double> percent = load_percent(row, index.column())) {
                return Theme::color_for_load(*percent);
            }
        }
        return colour_for(health_of(index.row()));
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
        case MemoryColumn:
        case DiskColumn: {
            const std::optional<double> percent = load_percent(row, index.column());
            return percent ? QString::number(*percent, 'f', 1) + QStringLiteral("%")
                           : QStringLiteral("-");
        }
        case AdaptersColumn: {
            // "-" rather than "0" when the client does not advertise the
            // capability: a machine with no adapters and a client too old to
            // report them are different facts, and 0 would state the wrong one.
            if (!row.entry.caps.has(core::Capability::Network)) {
                return role == Qt::ToolTipRole
                           ? QStringLiteral("This client does not report network adapters.")
                           : QStringLiteral("-");
            }
            if (role == Qt::ToolTipRole) {
                return adapter_tooltip(row.resources.adapters);
            }
            const auto connected = std::ranges::count_if(
                row.resources.adapters,
                [](const core::NetworkAdapter& adapter) { return core::is_up(adapter.link); });
            // Connected over total, because "6 adapters" alone says nothing
            // about whether the machine is actually on the network.
            return QStringLiteral("%1 / %2").arg(connected).arg(row.resources.adapters.size());
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
        case AdaptersColumn: return QStringLiteral("Adapters");
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
                    // The whole row, not just the two columns whose *text*
                    // changed: health colours every cell, and the CPU cell
                    // additionally switches between its load colour and the
                    // health one as the host starts and stops reporting.
                    emit dataChanged(index(row, HostColumn), index(row, ColumnCount - 1));
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

FleetModel::RowHealth FleetModel::health_of(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return RowHealth::Unknown;
    }
    const Row& r = rows_[static_cast<std::size_t>(row)];

    // Liveness first: a machine that is not there cannot usefully be described
    // by the last rules it happened to fail.
    switch (r.entry.state) {
        case core::HostState::Unexpected: return RowHealth::Unexpected;
        case core::HostState::Missing:    return RowHealth::Missing;
        case core::HostState::Offline:    return RowHealth::Offline;
        case core::HostState::Online:     break;
    }

    if (!r.compliant) {
        return RowHealth::Unknown;
    }
    return *r.compliant ? RowHealth::Compliant : RowHealth::Failing;
}

QColor FleetModel::colour_for(RowHealth health) {
    switch (health) {
        case RowHealth::Unexpected: return QColor(Theme::kUnexpected);
        case RowHealth::Missing:    return QColor(Theme::kMissing);
        case RowHealth::Offline:    return QColor(Theme::kNotApplicable);
        case RowHealth::Failing:    return QColor(Theme::kOffline);
        case RowHealth::Compliant:  return QColor(Theme::kOnline);
        case RowHealth::Unknown:    return QColor(Theme::kTextMuted);
    }
    return QColor(Theme::kText);
}

void FleetModel::apply_compliance(const core::ComplianceReport& report) {
    const int row = index_of(report.host_id);
    if (row < 0) {
        return;  // a report for a host this view does not list
    }

    // Matches core::is_compliant: only Fail counts against a host. A Linux box
    // cannot fail a registry rule, and a transient probe error is not a
    // violation -- neither should paint the row yellow.
    const bool compliant = core::is_compliant(report);

    Row& r = rows_[static_cast<std::size_t>(row)];
    if (r.compliant && *r.compliant == compliant) {
        return;  // nothing visible changed
    }
    r.compliant = compliant;

    // Health colours every column, so the whole row has to repaint -- but only
    // this row, and only when the verdict actually moved.
    emit dataChanged(index(row, 0), index(row, ColumnCount - 1), {Qt::ForegroundRole});
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

std::optional<double> FleetModel::load_percent(const Row& row, int column) {
    if (!row.has_resources) {
        return std::nullopt;
    }
    switch (column) {
        case CpuColumn:
            return row.resources.cpu_percent;
        case MemoryColumn:
            if (row.resources.mem_total_bytes == 0) {
                return std::nullopt;
            }
            return 100.0 * static_cast<double>(row.resources.mem_used_bytes) /
                   static_cast<double>(row.resources.mem_total_bytes);
        case DiskColumn: {
            if (row.resources.disks.empty()) {
                return std::nullopt;
            }
            // The fullest volume: a machine with one full disk is in trouble
            // however much room the others have left.
            double worst = 0.0;
            for (const core::DiskUsage& disk : row.resources.disks) {
                worst = std::max(worst, disk.used_percent());
            }
            return worst;
        }
        default:
            return std::nullopt;
    }
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
