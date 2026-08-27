#pragma once

#include "lm/ui/export.hpp"

#include <QAbstractTableModel>
#include <QColor>
#include <QMetaType>
#include <QString>
#include <QVector>

#include <optional>
#include <vector>

#include "lm/core/compliance.hpp"
#include "lm/core/fleet.hpp"
#include "lm/transport/messages.hpp"

namespace lm::ui {

/// One rule a host is not passing, reduced to what the fleet table shows for it.
///
/// The label is resolved by the *caller*, not here. core::CheckResult carries
/// only a rule id, and the bundle mapping an id to its description lives in the
/// server -- so passing it in keeps FleetModel from learning about
/// TemplateBundle lookups, and keeps core::rules_for() the single route to that
/// mapping. Walking every template instead once labelled a row with an
/// unrelated rule's description, which is the failure that route exists to
/// prevent.
struct LM_UI_EXPORT ComplianceTag {
    /// The rule's authored description, or ui::describe()'s generated summary
    /// when the author left it blank. Never a rule id: ids are generated join
    /// keys and say nothing about what is being checked.
    QString label;
    /// Fail or Error. Never Pass or NotApplicable -- a passing rule is counted,
    /// not listed, and a rule the host cannot evaluate is not a problem with
    /// the host.
    core::CheckStatus status = core::CheckStatus::Fail;
    friend bool operator==(const ComplianceTag&, const ComplianceTag&) = default;
};

/// Rows are held in a stable order (by host id) and are never reordered. Wrap
/// this in a QSortFilterProxyModel with sortRole == SeverityRole to present the
/// most-urgent-first order without churning rows in the view.
class LM_UI_EXPORT FleetModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        HostColumn = 0,
        StateColumn,
        CpuColumn,
        MemoryColumn,
        DiskColumn,
        AdaptersColumn,
        ComplianceColumn,
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
        Paused,      ///< alive and announcing, but the operator stopped its reporting
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
        /// QVector<ComplianceTag> for ComplianceColumn, empty elsewhere. The
        /// column's DisplayRole is only the ratio; the tags themselves are
        /// painted by ComplianceTagDelegate, which reads them from here.
        ComplianceTagsRole,
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

    /// Records a host's latest compliance result: the row's colour, and the
    /// compliance column's ratio and tags.
    ///
    /// core::FleetEntry carries liveness but not compliance, and reports arrive
    /// on their own topic, so the two are merged here rather than in reconcile().
    /// A report for an unknown host is ignored.
    ///
    /// `tags` is every Fail and Error in the report, already labelled -- see
    /// ComplianceTag for why the labelling is the caller's job.
    void apply_compliance(const core::ComplianceReport& report, QVector<ComplianceTag> tags);

    [[nodiscard]] RowHealth health_of(int row) const;

    /// The colour a row of this health paints in. Static so the legend can use
    /// the same mapping the rows do, rather than duplicating it.
    [[nodiscard]] static QColor colour_for(RowHealth health);

private:
    struct Row {
        core::FleetEntry entry;
        core::ResourceSample resources;
        bool has_resources = false;
        /// Optional rather than a zeroed summary: "no report yet" must not read
        /// as compliant, or an unchecked machine shows a green light and a
        /// "0 / 0" score, which reads as a clean bill of health.
        std::optional<core::ComplianceSummary> summary;
        /// Every failing and erroring rule, in report order.
        QVector<ComplianceTag> tags;
    };

    [[nodiscard]] int index_of(const core::HostId& host_id) const;

    /// The percentage a resource column is reporting, or nullopt when it has
    /// nothing to report. One source for both the number and the colour, so the
    /// two can never disagree about what the machine is doing.
    [[nodiscard]] static std::optional<double> load_percent(const Row& row, int column);

    std::vector<Row> rows_;  ///< always sorted by entry.host_id
};

}  // namespace lm::ui

// Registered so a ComplianceTag list can ride a QVariant out through
// ComplianceTagsRole. The name is spelled out rather than left to the macro's
// default because moc records the unqualified literal it sees, and the two
// silently disagreeing is what makes a QVariant come back empty.
Q_DECLARE_METATYPE(lm::ui::ComplianceTag)
Q_DECLARE_METATYPE(QVector<lm::ui::ComplianceTag>)
