#include "scripts_tab.hpp"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSplitter>
#include <QVBoxLayout>

#include <cstdint>

#include "lm/core/fleet.hpp"
#include "lm/core/types.hpp"
#include "server_controller.hpp"

namespace {

/// The editor's starting content, verbatim from the design spec.
///
/// Three things about it are deliberate and must survive an edit here.
///
/// It **runs as-is and does nothing**, so pressing Run on an untouched
/// template is safe and demonstrates the whole path -- dispatch, capture,
/// verdict -- before anybody has written a line of their own.
///
/// It shows **both** branches, because the failure one is what people get
/// wrong: a script that catches its own error and forgets to exit non-zero
/// reports success, and PowerShell will happily let it.
///
/// It builds its JSON with ConvertTo-Json rather than a hand-built string,
/// because hand-quoting JSON inside PowerShell is a reliable source of
/// malformed markers -- and a malformed marker is silently ignored, so the
/// mistake costs an operator the one line they wrote the script to see.
///
/// This is also the only place the LM-RESULT convention is taught. A page of
/// documentation describing it would be read by nobody; boilerplate that
/// already does it correctly gets edited rather than replaced.
constexpr const char* kStarterTemplate = R"PS(# Runs on each selected host as the lab_monitor agent.
# The exit code decides the outcome: 0 = Completed, anything else = Failed.
# The LM-RESULT line is optional — it lets you say *why*, in the run view.

function Report($ok, $message) {
    $payload = @{ ok = $ok; message = $message } | ConvertTo-Json -Compress
    Write-Output "LM-RESULT: $payload"
}

try {
    # --- your work here ---
    Write-Output "Nothing to do yet."

    Report $true "completed"
    exit 0
}
catch {
    Report $false $_.Exception.Message
    exit 1
}
)PS";

/// Phase 1 has no per-run timeout control, and 120 s matches ScriptRun's own
/// default. Named rather than inlined so the run view and this agree when the
/// control arrives.
constexpr std::uint32_t kDefaultTimeoutSeconds = 120;

/// The name a run made from the editor is recorded under. Custom runs are as
/// auditable as named ones -- the body is kept verbatim either way -- so this
/// says where it came from rather than pretending to be a filename.
const QString kCustomScriptName = QStringLiteral("(custom script)");

constexpr int kHostIdRole = Qt::UserRole + 1;
constexpr int kEligibleRole = Qt::UserRole + 2;

/// Why this host cannot be asked, or an empty string when it can.
///
/// The wording matches ServerController::start_script_run()'s own refusals, so
/// the reason shown before Run is pressed is the reason recorded afterwards.
QString refusal_for(const lm::core::FleetEntry& entry) {
    if (entry.state != lm::core::HostState::Online) {
        return QString::fromStdString(lm::core::to_string(entry.state));
    }
    if (!entry.caps.has(lm::core::Capability::Scripts)) {
        return QStringLiteral("not enrolled");
    }
    return {};
}

}  // namespace

QString ScriptsTab::starter_template() {
    return QString::fromUtf8(kStarterTemplate);
}

ScriptsTab::ScriptsTab(ServerController* controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    auto* layout = new QVBoxLayout(this);
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    auto* script_side = new QWidget(splitter);
    auto* script_layout = new QVBoxLayout(script_side);
    script_layout->setContentsMargins(0, 0, 0, 0);
    script_layout->addWidget(new QLabel(QStringLiteral("Script"), script_side));

    editor_ = new QPlainTextEdit(script_side);
    editor_->setObjectName(QStringLiteral("ScriptEditor"));
    // Fixed-width for the same reason the shopping cart's path pane is: in the
    // proportional UI font PowerShell's punctuation runs together, and this is
    // text somebody has to read character by character before running it on a
    // hundred machines.
    editor_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    editor_->setPlainText(starter_template());
    script_layout->addWidget(editor_, 1);

    auto* reset_button = new QPushButton(QStringLiteral("Reset to template"), script_side);
    reset_button->setObjectName(QStringLiteral("ResetTemplateButton"));
    auto* script_buttons = new QHBoxLayout();
    script_buttons->addWidget(reset_button);
    script_buttons->addStretch(1);
    script_layout->addLayout(script_buttons);
    splitter->addWidget(script_side);

    auto* host_side = new QWidget(splitter);
    auto* host_layout = new QVBoxLayout(host_side);
    host_layout->setContentsMargins(0, 0, 0, 0);
    host_layout->addWidget(new QLabel(QStringLiteral("Hosts"), host_side));

    host_list_ = new QListWidget(host_side);
    host_list_->setObjectName(QStringLiteral("HostList"));
    host_layout->addWidget(host_list_, 1);

    auto* select_all_button = new QPushButton(QStringLiteral("Select all"), host_side);
    select_all_button->setObjectName(QStringLiteral("SelectAllButton"));
    auto* clear_button = new QPushButton(QStringLiteral("Clear"), host_side);
    clear_button->setObjectName(QStringLiteral("ClearButton"));
    auto* host_buttons = new QHBoxLayout();
    host_buttons->addWidget(select_all_button);
    host_buttons->addWidget(clear_button);
    host_buttons->addStretch(1);
    host_layout->addLayout(host_buttons);
    splitter->addWidget(host_side);

    // The editor is the working surface; the host list is a column of names.
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);

    // The count sits beside Run because the blast radius must be readable
    // without counting checkboxes -- this is the last thing seen before code
    // goes out to other people's machines.
    target_count_label_ = new QLabel(this);
    target_count_label_->setObjectName(QStringLiteral("TargetCountLabel"));
    run_button_ = new QPushButton(QStringLiteral("Run"), this);
    run_button_->setObjectName(QStringLiteral("RunButton"));
    auto* run_row = new QHBoxLayout();
    run_row->addStretch(1);
    run_row->addWidget(target_count_label_);
    run_row->addWidget(run_button_);
    layout->addLayout(run_row);

    connect(reset_button, &QPushButton::clicked, this,
            [this] { editor_->setPlainText(starter_template()); });
    connect(select_all_button, &QPushButton::clicked, this, [this] {
        for (int row = 0; row < host_list_->count(); ++row) {
            QListWidgetItem* item = host_list_->item(row);
            // Eligible rows only. A bulk gesture must never quietly target a
            // machine that cannot run the script -- those get a Refused target
            // and no execution, which is noise in the run view rather than a
            // result. Ticking one by hand still works: that is a decision
            // somebody made about a named machine.
            if (item->data(kEligibleRole).toBool()) {
                item->setCheckState(Qt::Checked);
            }
        }
    });
    connect(clear_button, &QPushButton::clicked, this, [this] {
        for (int row = 0; row < host_list_->count(); ++row) {
            host_list_->item(row)->setCheckState(Qt::Unchecked);
        }
    });
    connect(host_list_, &QListWidget::itemChanged, this, [this](QListWidgetItem*) {
        if (!rebuilding_) {
            update_target_count();
        }
    });
    connect(run_button_, &QPushButton::clicked, this, &ScriptsTab::on_run_clicked);
    connect(controller_, &ServerController::fleet_changed, this, &ScriptsTab::rebuild_host_list);

    rebuild_host_list();
}

void ScriptsTab::rebuild_host_list() {
    // Ticks survive a rebuild: fleet_changed() fires whenever any host changes
    // state, and losing a hand-made selection to an unrelated machine going
    // offline mid-edit would be its own kind of blast radius.
    QSet<QString> previously_checked;
    for (int row = 0; row < host_list_->count(); ++row) {
        const QListWidgetItem* item = host_list_->item(row);
        if (item->checkState() == Qt::Checked) {
            previously_checked.insert(item->data(kHostIdRole).toString());
        }
    }

    rebuilding_ = true;
    host_list_->clear();
    for (const lm::core::FleetEntry& entry : controller_->fleet().entries) {
        const QString host_id = QString::fromStdString(entry.host_id);
        const QString refusal = refusal_for(entry);

        // Listed, explained and unchecked -- never hidden. An operator should
        // see that a machine is excluded and why, not wonder where it went;
        // and they may still tick it deliberately, which is why the row stays
        // checkable rather than being disabled.
        auto* item = new QListWidgetItem(
            refusal.isEmpty() ? host_id : host_id + QStringLiteral(" — ") + refusal, host_list_);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setData(kHostIdRole, host_id);
        item->setData(kEligibleRole, refusal.isEmpty());
        // A tick survives even on a row that has since become ineligible: it
        // was a deliberate act, and silently undoing it would be worse than
        // letting the run record the refusal.
        item->setCheckState(previously_checked.contains(host_id) ? Qt::Checked : Qt::Unchecked);
    }
    rebuilding_ = false;

    update_target_count();
}

std::vector<std::string> ScriptsTab::checked_hosts() const {
    std::vector<std::string> hosts;
    for (int row = 0; row < host_list_->count(); ++row) {
        const QListWidgetItem* item = host_list_->item(row);
        if (item->checkState() == Qt::Checked) {
            hosts.push_back(item->data(kHostIdRole).toString().toStdString());
        }
    }
    return hosts;
}

void ScriptsTab::update_target_count() {
    const auto count = checked_hosts().size();
    // Run with nothing targeted is a no-op that reads as a failure. The button
    // says so by being unavailable rather than by doing nothing.
    run_button_->setEnabled(count > 0);
    if (count == 0) {
        target_count_label_->setText(QStringLiteral("No hosts selected"));
    } else if (count == 1) {
        target_count_label_->setText(QStringLiteral("Run on 1 host"));
    } else {
        target_count_label_->setText(
            QStringLiteral("Run on %1 hosts").arg(static_cast<int>(count)));
    }
}

void ScriptsTab::on_run_clicked() {
    const std::vector<std::string> hosts = checked_hosts();
    if (hosts.empty()) {
        return;  // the button is disabled; belt and braces for a programmatic click
    }
    controller_->start_script_run(kCustomScriptName.toStdString(),
                                  editor_->toPlainText().toStdString(), hosts,
                                  kDefaultTimeoutSeconds);
}
