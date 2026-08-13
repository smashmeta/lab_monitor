#include "lm/ui/sample_coalescer.hpp"

#include <QMetaType>

#include <utility>

namespace lm::ui {

SampleCoalescer::SampleCoalescer(std::chrono::milliseconds interval, QObject* parent)
    : QObject(parent) {
    // Registration must actually run before flushed() is ever emitted across a
    // queued connection (or observed by QSignalSpy); doing it here, rather
    // than relying on static init order elsewhere, guarantees that.
    //
    // The explicit name matters: this header is written inside `namespace
    // lm::ui`, so the "flushed(QVector<transport::ResourceSampleMessage>)"
    // signal is spelled with the *unqualified* inner-namespace name in the
    // source moc parses textually (it does not resolve C++ name lookup), and
    // records that literal string as the parameter's type name. The default,
    // no-argument qRegisterMetaType<T>() instead derives the fully-qualified
    // name "QVector<lm::transport::ResourceSampleMessage>" from
    // Q_DECLARE_METATYPE's argument. QSignalSpy resolves a signal argument's
    // type by looking up moc's literal string via QMetaType::type(), so
    // without this exact alias the lookup silently fails and every captured
    // argument comes back default-constructed (empty).
    qRegisterMetaType<transport::ResourceSampleMessage>("lm::transport::ResourceSampleMessage");
    qRegisterMetaType<QVector<transport::ResourceSampleMessage>>(
        "QVector<transport::ResourceSampleMessage>");

    timer_.setInterval(static_cast<int>(interval.count()));
    connect(&timer_, &QTimer::timeout, this, &SampleCoalescer::flush);
    timer_.start();
}

void SampleCoalescer::push(transport::ResourceSampleMessage sample) {
    const std::lock_guard<std::mutex> lock(mutex_);
    pending_.insert_or_assign(sample.host_id, std::move(sample));
}

void SampleCoalescer::flush() {
    std::map<std::string, transport::ResourceSampleMessage> batch;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (pending_.empty()) {
            return;
        }
        batch.swap(pending_);
    }

    QVector<transport::ResourceSampleMessage> result;
    result.reserve(static_cast<int>(batch.size()));
    for (auto& [host_id, sample] : batch) {
        (void)host_id;
        result.push_back(std::move(sample));
    }
    emit flushed(std::move(result));
}

}  // namespace lm::ui
