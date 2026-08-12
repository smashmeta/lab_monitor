#pragma once

#include <QObject>
#include <QTimer>
#include <QVector>

#include <chrono>
#include <map>
#include <mutex>
#include <string>

#include "lm/transport/messages.hpp"

Q_DECLARE_METATYPE(lm::transport::ResourceSampleMessage)

namespace lm::ui {

/// Buffers incoming resource samples and emits them in batches, so a burst of
/// DDS traffic produces one repaint instead of dozens. push() is safe to call
/// from a Fast DDS callback thread; flushed() is emitted on the owning thread.
class SampleCoalescer : public QObject {
    Q_OBJECT

public:
    explicit SampleCoalescer(std::chrono::milliseconds interval = std::chrono::milliseconds{100},
                             QObject* parent = nullptr);

    /// Thread-safe. Later samples for the same host replace earlier ones,
    /// mirroring the KEEP_LAST(1) QoS on the ResourceSample topic.
    void push(transport::ResourceSampleMessage sample);

signals:
    void flushed(QVector<transport::ResourceSampleMessage> batch);

private:
    void flush();

    QTimer timer_;
    std::mutex mutex_;
    std::map<std::string, transport::ResourceSampleMessage> pending_;
};

}  // namespace lm::ui
