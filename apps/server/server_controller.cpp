#include "server_controller.hpp"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QMetaObject>

#include <algorithm>
#include <exception>
#include <expected>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "lm/core/json.hpp"

ServerController::ServerController(std::unique_ptr<lm::transport::IServerTransport> transport,
                                    QString config_dir, QObject* parent)
    : QObject(parent), transport_(std::move(transport)), config_dir_(std::move(config_dir)) {}

bool ServerController::can_publish() const {
    return lm::core::content_hash(draft_) != lm::core::content_hash(published_);
}

void ServerController::start() {
    load_config();

    // Every IServerTransport callback below may be invoked from a Fast DDS
    // internal thread (or, for --offline, synchronously from whichever
    // thread calls publish_*() on the in-memory bus -- still not necessarily
    // the GUI thread). registry_ and model_ are only ever touched from the
    // functors that QMetaObject::invokeMethod posts onto *this* object's
    // thread, never directly inside these lambdas.
    transport_->on_announce([this](const lm::transport::ClientAnnounce& announce) {
        QMetaObject::invokeMethod(
            this, [this, announce] { on_announce(announce); }, Qt::QueuedConnection);
    });
    transport_->on_resources([this](const lm::transport::ResourceSampleMessage& sample) {
        // The one exception: SampleCoalescer::push() is documented and
        // implemented as thread-safe (an internal std::mutex protects
        // pending_), so it is safe to call directly from whatever thread
        // this callback runs on. It does not touch registry_ or model_
        // itself -- that happens later in apply_coalesced(), reached only
        // through SampleCoalescer's own QTimer, which lives on the thread
        // the coalescer was constructed on (this object's GUI thread).
        coalescer_.push(sample);
    });
    transport_->on_report([this](const lm::transport::ComplianceReportMessage& report) {
        QMetaObject::invokeMethod(
            this, [this, report] { on_report(report); }, Qt::QueuedConnection);
    });
    transport_->on_client_lost([this](const lm::core::HostId& host_id) {
        QMetaObject::invokeMethod(
            this, [this, host_id] { on_client_lost(host_id); }, Qt::QueuedConnection);
    });

    connect(&coalescer_, &lm::ui::SampleCoalescer::flushed, this, &ServerController::apply_coalesced);

    reconcile_timer_.setInterval(1000);
    connect(&reconcile_timer_, &QTimer::timeout, this, &ServerController::reconcile_now);
    reconcile_timer_.start();

    reconcile_now();
}

void ServerController::mark_draft_dirty() { emit draft_publishable_changed(can_publish()); }

void ServerController::set_expected_hosts(std::vector<lm::core::ExpectedHost> hosts) {
    expected_ = std::move(hosts);
    save_expected_hosts();
    emit expected_hosts_changed();
    reconcile_now();
}

void ServerController::add_expected_host(const lm::core::HostId& host_id, const std::string& address) {
    const auto it = std::find_if(expected_.begin(), expected_.end(),
                                  [&](const lm::core::ExpectedHost& host) { return host.host_id == host_id; });
    if (it != expected_.end()) {
        it->address = address;
    } else {
        expected_.push_back(lm::core::ExpectedHost{host_id, address});
    }
    save_expected_hosts();
    emit expected_hosts_changed();
    reconcile_now();
}

void ServerController::remove_expected_host(const lm::core::HostId& host_id) {
    const auto new_end =
        std::remove_if(expected_.begin(), expected_.end(),
                        [&](const lm::core::ExpectedHost& host) { return host.host_id == host_id; });
    if (new_end == expected_.end()) {
        return;
    }
    expected_.erase(new_end, expected_.end());
    save_expected_hosts();
    emit expected_hosts_changed();
    reconcile_now();
}

void ServerController::publish() {
    if (!can_publish()) {
        return;
    }

    draft_.revision = published_.revision + 1;
    draft_.hash = lm::core::content_hash(draft_);
    published_ = draft_;

    save_published_bundle();

    lm::transport::TemplateBundleMessage message;
    message.revision = published_.revision;
    message.hash = published_.hash;
    message.json = lm::core::serialise_bundle(published_);
    transport_->publish_bundle(message);

    options_.current_revision = published_.revision;

    emit published_changed();
    emit draft_publishable_changed(can_publish());
    // Refresh staleness against the new revision immediately rather than
    // waiting up to 1 s for the next timer tick.
    reconcile_now();
}

void ServerController::on_announce(const lm::transport::ClientAnnounce& announce) {
    registry_.record_announce(announce.host_id, lm::core::Capabilities(announce.capabilities),
                               lm::core::Clock::now());
}

void ServerController::on_report(const lm::transport::ComplianceReportMessage& report) {
    registry_.record_report(report.report.host_id, report.report.applied_revision, lm::core::Clock::now());

    const QString host_id = QString::fromStdString(report.report.host_id);
    report_cache_.insert(host_id, report.report);
    emit compliance_report_received(host_id, report.report);
}

void ServerController::on_client_lost(const lm::core::HostId& host_id) {
    // Expected hosts are deliberately left in the registry with their last
    // last_seen timestamp: reconcile()'s own liveliness-lease check (run
    // every 1 s by reconcile_timer_) independently transitions them from
    // Online to Offline once `now - last_seen` exceeds the lease -- which is
    // exactly the Offline (not Missing) behaviour the vertical slice expects
    // when a client stops. Erasing here would instead make them Missing the
    // instant this callback fires.
    //
    // Unexpected hosts get no such lease check at all (see
    // lm::core::reconcile's unexpected-entries loop, which never calls
    // within_lease()), so without removing them here they would linger in
    // the fleet view forever after disconnecting. mark_lost() is reserved
    // for exactly that case.
    const bool is_expected =
        std::any_of(expected_.begin(), expected_.end(),
                    [&](const lm::core::ExpectedHost& host) { return host.host_id == host_id; });
    if (!is_expected) {
        registry_.mark_lost(host_id);
    }
    reconcile_now();
}

void ServerController::apply_coalesced(QVector<lm::transport::ResourceSampleMessage> batch) {
    const lm::core::TimePoint now = lm::core::Clock::now();
    for (const lm::transport::ResourceSampleMessage& sample : batch) {
        registry_.record_sample(sample.host_id, now);
        model_.apply_sample(sample);

        const QString host_id = QString::fromStdString(sample.host_id);
        resource_cache_.insert(host_id, sample.sample);
        emit resource_sample_received(host_id, sample.sample);
    }
}

void ServerController::reconcile_now() {
    options_.current_revision = published_.revision;
    const lm::core::FleetView view =
        lm::core::reconcile(expected_, registry_.snapshot(), lm::core::Clock::now(), options_);
    model_.apply(view);
    emit counts_changed(view.counts);
}

QString ServerController::expected_hosts_path() const {
    return config_dir_ + QStringLiteral("/expected_hosts.json");
}

QString ServerController::bundle_path() const { return config_dir_ + QStringLiteral("/bundle.json"); }

void ServerController::load_config() {
    QDir().mkpath(config_dir_);

    QFile expected_file(expected_hosts_path());
    if (expected_file.exists() && expected_file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QByteArray raw = expected_file.readAll();
        try {
            const nlohmann::json doc = nlohmann::json::parse(raw.constData(), raw.constData() + raw.size());
            std::vector<lm::core::ExpectedHost> hosts;
            for (const auto& item : doc) {
                lm::core::ExpectedHost host;
                host.host_id = item.at("host_id").get<std::string>();
                host.address = item.value("address", std::string{});
                hosts.push_back(std::move(host));
            }
            expected_ = std::move(hosts);
        } catch (const std::exception& error) {
            const QString message =
                QStringLiteral("Failed to parse expected hosts config: %1").arg(error.what());
            spdlog::error(message.toStdString());
            emit config_error(message);
            // Keep whatever expected_ already held (empty on a first run).
        }
    }

    QFile bundle_file(bundle_path());
    if (bundle_file.exists() && bundle_file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QByteArray raw = bundle_file.readAll();
        const std::expected<lm::core::TemplateBundle, std::string> parsed =
            lm::core::parse_bundle(std::string(raw.constData(), static_cast<std::size_t>(raw.size())));
        if (parsed) {
            published_ = *parsed;
            draft_ = published_;
            options_.current_revision = published_.revision;
        } else {
            const QString message = QStringLiteral("Failed to parse template bundle config: %1")
                                         .arg(QString::fromStdString(parsed.error()));
            spdlog::error(message.toStdString());
            emit config_error(message);
            // Keep the last good bundle in memory -- default-constructed
            // (empty) on a first run, or whatever was already loaded.
        }
    }
}

void ServerController::save_expected_hosts() const {
    nlohmann::json doc = nlohmann::json::array();
    for (const lm::core::ExpectedHost& host : expected_) {
        doc.push_back({{"host_id", host.host_id}, {"address", host.address}});
    }
    QFile file(expected_hosts_path());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        const std::string text = doc.dump(2);
        file.write(text.data(), static_cast<qint64>(text.size()));
    }
}

void ServerController::save_published_bundle() const {
    QFile file(bundle_path());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        const std::string text = lm::core::serialise_bundle(published_);
        file.write(text.data(), static_cast<qint64>(text.size()));
    }
}
