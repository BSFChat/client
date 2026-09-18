#include "voice/video/VideoDecodeHealth.h"

#include "core/AppProfile.h"

#include <QLoggingCategory>
#include <QSettings>
#include <QString>

#include <atomic>
#include <mutex>

#ifndef BSFCHAT_VERSION
#  define BSFCHAT_VERSION "0.0.0"
#endif

Q_LOGGING_CATEGORY(logVideoHealth, "bsfchat.video.health", QtWarningMsg)

namespace {

// The persisted hint stores the BUILD that saw the failure, not a
// bare bool, and is honoured only while that build is still the one
// running. Two reasons:
//
//   * a decoder that refuses today can work after a driver update, a
//     Store HEVC extension install, or a client update that fixes the
//     probe itself — and a bare bool would black-hole H.265 on that
//     machine forever, with no UI to clear it;
//   * the most likely thing that changes the answer IS a client
//     update, so keying on the version makes an upgrade re-probe,
//     which is exactly the behaviour we want and costs at most one
//     self-healing call.
//
// Losing the hint therefore costs a second of black tile at the start
// of the first call after an upgrade, which the in-process latch then
// fixes. Keeping a stale hint would cost H.265 permanently.
constexpr auto kKey = "video/h265DecodeBrokenInVersion";

QSettings& store() {
    // Its own handle, like Updater's: this is read before any Settings
    // object need exist (localCapsJson is static and runs off the
    // capability probes), and the profile-aware names keep two clients
    // on one machine out of each other's file.
    static QSettings s(bsfchat::organizationName(), bsfchat::applicationName());
    return s;
}

QString thisVersion() { return QStringLiteral(BSFCHAT_VERSION); }

// -1 unknown, 0 fine, 1 broken. Seeded once from disk.
std::atomic<int> g_h265Broken{-1};
std::once_flag g_loadOnce;

void loadPersistedHint() {
    std::call_once(g_loadOnce, []() {
        int expected = -1;
        const QString saw = store().value(QLatin1String(kKey)).toString();
        if (saw.isEmpty()) {
            g_h265Broken.compare_exchange_strong(expected, 0);
            return;
        }
        if (saw != thisVersion()) {
            // A different build wrote it — re-probe, and drop the stale
            // key so it cannot come back if the user downgrades.
            store().remove(QLatin1String(kKey));
            store().sync();
            g_h265Broken.compare_exchange_strong(expected, 0);
            return;
        }
        qCWarning(logVideoHealth, "H.265 decode was already proven broken on "
                 "this machine for %s — not advertising it",
                 qPrintable(saw));
        g_h265Broken.compare_exchange_strong(expected, 1);
    });
}

} // namespace

namespace videohealth {

bool h265DecodeBroken() {
    loadPersistedHint();
    return g_h265Broken.load(std::memory_order_acquire) == 1;
}

bool markH265DecodeBroken() {
    loadPersistedHint();
    int was = g_h265Broken.exchange(1, std::memory_order_acq_rel);
    if (was == 1) return false;   // somebody else already latched it
    qCWarning(logVideoHealth, "H.265 decode failed on this machine — "
             "dropping h265 from our advertised caps for the rest of this "
             "process and re-announcing");
    store().setValue(QLatin1String(kKey), thisVersion());
    // The interesting crash is the one right after a decoder blew up,
    // so don't leave this sitting in QSettings' write buffer.
    store().sync();
    return true;
}

void resetForTest() {
    loadPersistedHint();   // burn the once_flag so it can't re-seed later
    g_h265Broken.store(0, std::memory_order_release);
    store().remove(QLatin1String(kKey));
    store().sync();
}

} // namespace videohealth
