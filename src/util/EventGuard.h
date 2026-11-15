#pragma once

// Run the handling of ONE timeline/state event so that a failure skips that
// event instead of the rest of the batch.
//
// Every read of event content is meant to be type-checked (util/JsonSafe.h),
// so nothing here is expected to fire. It is the backstop for the read that
// was missed: before it, one malformed event threw out of the sync handler
// (dropping the rest of the batch after `since` had already advanced) or out
// of setActiveRoom, a Q_INVOKABLE, which terminates the process — and because
// the event was already cached, it did so again on every launch.

#include <QDebug>

#include <exception>
#include <string>

#include <bsfchat/MatrixTypes.h>

namespace bsfchat::client {

inline void logSkippedEvent(const bsfchat::RoomEvent& event, const char* where,
                            const char* what = nullptr)
{
    auto line = qWarning().noquote();
    line << "[events] skipped malformed event" << QString::fromStdString(event.event_id)
         << QString::fromStdString(event.type) << "in" << where;
    if (what) line << "—" << what;
}

template <typename Fn>
bool guardEvent(const bsfchat::RoomEvent& event, const char* where, Fn&& fn) noexcept
{
    try {
        fn();
        return true;
    } catch (const std::exception& e) {
        logSkippedEvent(event, where, e.what());
    } catch (...) {
        logSkippedEvent(event, where);
    }
    return false;
}

} // namespace bsfchat::client
