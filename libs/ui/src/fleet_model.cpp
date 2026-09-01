#include "lm/ui/fleet_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>

#include <QDateTime>
#include <QStringList>
#include <QString>
#include <QTimeZone>

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
        case core::HostState::Paused:     return 3;
        case core::HostState::Online:     return 4;
    }
    return 5;
}

/// Lower sorts first, within the band its state gives it.
///
/// State decides the band: a machine nobody can hear from outranks a broken
/// rule, because the rule is a known problem with a known fix and the silence
/// is an unknown. Inside the Online band the ordering is the one the Compliance
/// tab used to provide and would otherwise be lost with it -- failing worst
/// first, then not-yet-reported, then clean. Two views disagreeing about which
/// host is most urgent is worse than either order, and now there is only one.
int severity_score(core::HostState state, const std::optional<core::ComplianceSummary>& summary) {
    const int band = severity_of(state) * 1000;
    if (state != core::HostState::Online) {
        return band;
    }
    if (!summary) {
        return band + 900;  // reported nothing yet: less urgent than a failure
    }
    if (summary->failing == 0 && summary->errors == 0) {
        return band + 999;  // clean: the bottom of the list
    }
    // 0..899, so the worst-scoring host sorts above every other failing one and
    // all of them sort above the two cases handled just now.
    return band + static_cast<int>(std::lround(summary->passed_ratio() * 899.0));
}

/// True for a host that is not currently reporting.
///
/// Paused belongs here even though the machine is demonstrably alive and still
/// announcing: what makes a report stale is that no new one is coming, and that
/// is equally true whether the silence was chosen or not.
bool is_silent(core::HostState state) {
    return state == core::HostState::Missing || state == core::HostState::Offline ||
           state == core::HostState::Paused;
}

/// Whether this row should name the rules a host is not passing.
///
/// Online only, which excludes two rather different cases for two reasons.
///
/// A silent host, because those rules describe a machine that is no longer
/// answering and a row of live-looking tags claims a freshness the reading does
/// not have.
///
/// An Unexpected host, because its problem is that it is on the network at all.
/// Its rule failures are live and true, and listing them still buries the one
/// fact worth acting on under detail about a machine nobody has agreed to
/// manage. The ratio stays -- it is a real reading -- and the full list is a
/// hover away, in the same place the tags that do not fit already live.
bool shows_tags(core::HostState state) {
    return state == core::HostState::Online;
}

QString format_last_seen(const std::optional<core::TimePoint>& last_seen) {
    if (!last_seen.has_value()) {
        return QStringLiteral("Never");
    }
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(last_seen->time_since_epoch()).count();
    const QDateTime timestamp = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(seconds), QTimeZone::UTC);
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

/// The whole compliance picture for one host, since the cell itself can only
/// fit so many tags before it has to say "+3 more". This is the hover that
/// makes that truncation safe -- appropriate on the Fleet tab, which is read by
/// someone sitting at the keyboard, and exactly what the Compliance tab could
/// not do when it was read from across a room.
QString compliance_tooltip(core::HostState state,
                           const std::optional<core::ComplianceSummary>& summary,
                           const QVector<ComplianceTag>& tags) {
    if (is_silent(state)) {
        QString text = state == core::HostState::Paused
                           ? QStringLiteral("Paused by the operator - no rules are being checked.")
                           : QStringLiteral("Not reporting - no rules are being checked.");
        if (summary) {
            text += QStringLiteral("\n\nLast known: %1 of %2 checked rules passed.")
                        .arg(summary->passed)
                        .arg(summary->checked());
        }
        return text;
    }
    if (!summary) {
        return QStringLiteral("No compliance report yet.");
    }

    QStringList lines;
    lines << QStringLiteral("%1 of %2 checked rules passed.")
                 .arg(summary->passed)
                 .arg(summary->checked());
    if (summary->not_applicable > 0) {
        // Named rather than silently dropped: they are out of the ratio on
        // purpose, and a reader counting rules would otherwise find some
        // missing with nothing to say where they went.
        lines << QStringLiteral("%1 not applicable to this host, and so not counted.")
                     .arg(summary->not_applicable);
    }
    if (!tags.isEmpty()) {
        lines << QString();
        for (const ComplianceTag& tag : tags) {
            lines << QStringLiteral("%1\t%2").arg(Theme::glyph_for(tag.status), tag.label);
        }
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
            return severity_score(row.entry.state, row.summary);
        case ComplianceTagsRole:
            return shows_tags(row.entry.state) ? QVariant::fromValue(row.tags)
                                               : QVariant::fromValue(QVector<ComplianceTag>());
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
        case HostColumn: {
            if (role != Qt::ToolTipRole) {
                return QString::fromStdString(row.entry.host_id);
            }
            // Revision and Last Seen are hidden columns now, and the detail
            // pane shows neither -- so without this, when a machine was last
            // heard from would be nowhere in the UI, which is the one thing
            // worth knowing about a host that has gone quiet.
            QStringList lines;
            if (!row.entry.address.empty()) {
                lines << QString::fromStdString(row.entry.address);
                lines << QString();
            }
            lines << QStringLiteral("Last seen:\t%1").arg(format_last_seen(row.entry.last_seen));
            lines << QStringLiteral("Template:\t%1")
                         .arg(row.entry.stale ? QStringLiteral("Stale")
                                              : QStringLiteral("Current"));
            return lines.join(QChar(u'\n'));
        }
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
        case ComplianceColumn: {
            if (role == Qt::ToolTipRole) {
                return compliance_tooltip(row.entry.state, row.summary, row.tags);
            }
            if (is_silent(row.entry.state)) {
                // Never "0 / 0": on a machine nobody has heard from that reads
                // as a clean bill of health rather than as an absence of one.
                // A report from before it went quiet is shown, and labelled as
                // the historical reading it is.
                if (row.summary) {
                    return QStringLiteral("last known %1 / %2")
                        .arg(row.summary->passed)
                        .arg(row.summary->checked());
                }
                // Named for the reason it is quiet: "Paused" is a thing someone
                // did and can undo, where "Not reporting" is a machine to go
                // and look at. Reading them the same way wastes a trip.
                return row.entry.state == core::HostState::Paused
                           ? QStringLiteral("Paused")
                           : QStringLiteral("Not reporting");
            }
            if (!row.summary) {
                return QStringLiteral("-");
            }
            // passed over checked(), which excludes NotApplicable: a rule the
            // client cannot evaluate can never pass, so counting it would park
            // a Linux box at "5 / 10" forever with no action able to improve it.
            return QStringLiteral("%1 / %2").arg(row.summary->passed).arg(row.summary->checked());
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
        case HostColumn:       return QStringLiteral("Host");
        case StateColumn:      return QStringLiteral("State");
        case ComplianceColumn: return QStringLiteral("Compliance");
        case CpuColumn:        return QStringLiteral("CPU");
        case MemoryColumn:     return QStringLiteral("Memory");
        case DiskColumn:       return QStringLiteral("Disk");
        case AdaptersColumn:   return QStringLiteral("Adapters");
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
        case core::HostState::Paused:     return RowHealth::Paused;
        case core::HostState::Online:     break;
    }

    if (!r.summary) {
        return RowHealth::Unknown;
    }
    // Matches core::is_compliant: only Fail counts against a host. A Linux box
    // cannot fail a registry rule, and a transient probe error is not a
    // violation -- neither should paint the row yellow.
    return r.summary->failing == 0 ? RowHealth::Compliant : RowHealth::Failing;
}

QColor FleetModel::colour_for(RowHealth health) {
    switch (health) {
        case RowHealth::Unexpected: return QColor(Theme::kUnexpected);
        case RowHealth::Missing:    return QColor(Theme::kMissing);
        case RowHealth::Offline:    return QColor(Theme::kNotApplicable);
        case RowHealth::Paused:     return QColor(Theme::kPaused);
        case RowHealth::Failing:    return QColor(Theme::kOffline);
        case RowHealth::Compliant:  return QColor(Theme::kOnline);
        case RowHealth::Unknown:    return QColor(Theme::kTextMuted);
    }
    return QColor(Theme::kText);
}

void FleetModel::apply_compliance(const core::ComplianceReport& report,
                                  QVector<ComplianceTag> tags) {
    const int row = index_of(report.host_id);
    if (row < 0) {
        return;  // a report for a host this view does not list
    }

    const core::ComplianceSummary summary = core::summarise(report);

    Row& r = rows_[static_cast<std::size_t>(row)];
    if (r.summary && *r.summary == summary && r.tags == tags) {
        return;  // nothing visible changed
    }
    r.summary = summary;
    r.tags = std::move(tags);

    // The whole row, and not only ForegroundRole. Health still colours every
    // column, but the compliance column's own text and tags moved too, and its
    // SeverityRole -- which the proxy sorts on -- moved with the ratio.
    emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
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
