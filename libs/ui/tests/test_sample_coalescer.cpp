#include <gtest/gtest.h>

#include <QEventLoop>
#include <QSignalSpy>
#include <QTimer>

#include "lm/ui/sample_coalescer.hpp"

using namespace lm::transport;
using namespace lm::ui;
using namespace std::chrono_literals;

namespace {

ResourceSampleMessage sample_for(const std::string& host, double cpu) {
    ResourceSampleMessage message;
    message.host_id = host;
    message.sample.cpu_percent = cpu;
    return message;
}

/// Spins the event loop until the spy sees a signal or the timeout expires.
bool wait_for_signal(QSignalSpy& spy, int milliseconds = 2000) {
    return spy.wait(milliseconds);
}

}  // namespace

TEST(SampleCoalescer, CollapsesABurstForOneHostIntoTheLatestSample) {
    SampleCoalescer coalescer{50ms};
    QSignalSpy spy(&coalescer, &SampleCoalescer::flushed);

    for (double cpu : {10.0, 20.0, 30.0, 99.5}) {
        coalescer.push(sample_for("PC-001", cpu));
    }

    ASSERT_TRUE(wait_for_signal(spy));
    const auto batch = spy.takeFirst().at(0).value<QVector<ResourceSampleMessage>>();
    ASSERT_EQ(batch.size(), 1);
    EXPECT_DOUBLE_EQ(batch.front().sample.cpu_percent, 99.5);
}

TEST(SampleCoalescer, KeepsOneEntryPerHost) {
    SampleCoalescer coalescer{50ms};
    QSignalSpy spy(&coalescer, &SampleCoalescer::flushed);

    coalescer.push(sample_for("PC-001", 10.0));
    coalescer.push(sample_for("PC-002", 20.0));
    coalescer.push(sample_for("PC-001", 30.0));

    ASSERT_TRUE(wait_for_signal(spy));
    const auto batch = spy.takeFirst().at(0).value<QVector<ResourceSampleMessage>>();
    EXPECT_EQ(batch.size(), 2);
}

TEST(SampleCoalescer, DoesNotEmitWhenNothingWasPushed) {
    SampleCoalescer coalescer{50ms};
    QSignalSpy spy(&coalescer, &SampleCoalescer::flushed);

    QEventLoop loop;
    QTimer::singleShot(200, &loop, &QEventLoop::quit);
    loop.exec();

    EXPECT_EQ(spy.count(), 0);
}

TEST(SampleCoalescer, ClearsItsBufferBetweenFlushes) {
    SampleCoalescer coalescer{50ms};
    QSignalSpy spy(&coalescer, &SampleCoalescer::flushed);

    coalescer.push(sample_for("PC-001", 10.0));
    ASSERT_TRUE(wait_for_signal(spy));
    spy.clear();

    QEventLoop loop;
    QTimer::singleShot(200, &loop, &QEventLoop::quit);
    loop.exec();

    EXPECT_EQ(spy.count(), 0);
}
