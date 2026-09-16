#pragma once

#include <QMutex>
#include <QObject>
#include <QString>
#include <QThread>

#include <atomic>
#include <functional>
#include <utility>

// One background thread that runs at most one pending job — latest
// wins (S-10).
//
// The capture path does real per-frame work on the GUI thread: a full
// Retina QImage memcpy into a QVideoFrame (33 MB at 4K) and, for legacy
// peers, a JPEG encode. Both are milliseconds each, every frame, for as
// long as a share runs, and they land in the same thread that scrolls
// the message list — which is what "the app janks while I share" is.
//
// The queue depth is deliberately ONE: a job that has not started yet is
// replaced by the next one. Frames are only worth what they show, so
// falling behind must drop work rather than accumulate it — the same
// rule VideoSendPipeline and VideoReceivePipeline follow.
//
// No Q_OBJECT and no signals: jobs are plain callables delivered to a
// bare QObject living on the thread, so this header needs no moc and
// can be used from any translation unit. Results come back by the
// caller posting to its own QObject (queued invocations to a destroyed
// receiver are dropped by Qt, so a late result cannot outlive its
// owner).
//
// Destruction joins the thread, so a worker held by value/unique_ptr in
// the object whose `this` its jobs capture is safe by construction.
class LatestWinsWorker {
public:
    explicit LatestWinsWorker(const QString& threadName) {
        m_worker.moveToThread(&m_thread);
        m_thread.setObjectName(threadName);
        m_thread.start();
    }

    ~LatestWinsWorker() {
        // Drop anything not yet started, then join: a job still holding
        // a reference to the owner must not run during its destruction.
        {
            QMutexLocker lock(&m_mutex);
            m_pending = nullptr;
        }
        m_thread.quit();
        m_thread.wait();
    }

    LatestWinsWorker(const LatestWinsWorker&) = delete;
    LatestWinsWorker& operator=(const LatestWinsWorker&) = delete;

    // Thread-safe. Replaces any job that has not started running.
    void submit(std::function<void()> job) {
        {
            QMutexLocker lock(&m_mutex);
            m_pending = std::move(job);
        }
        if (!m_drainQueued.exchange(true)) {
            QMetaObject::invokeMethod(&m_worker, [this]() { drain(); },
                                      Qt::QueuedConnection);
        }
    }

    // True while a job is queued or running — callers use it to skip
    // handing over work the worker demonstrably cannot keep up with.
    bool busy() const { return m_drainQueued.load(); }

private:
    void drain() {
        std::function<void()> job;
        {
            QMutexLocker lock(&m_mutex);
            job = std::move(m_pending);
            m_pending = nullptr;
            // Cleared under the same lock that guards the slot, so a
            // submit racing this either lands in the slot we are about
            // to take or queues its own wakeup. (The bug this ordering
            // avoids is written up in VideoSendPipeline, S-16.)
            m_drainQueued.store(false);
        }
        if (job) job();
    }

    QThread m_thread;
    QObject m_worker;
    QMutex m_mutex;
    std::function<void()> m_pending;
    std::atomic<bool> m_drainQueued{false};
};
