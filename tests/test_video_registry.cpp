// VideoStreamRegistry fan-out: one decoded stream, several surfaces.
//
// This is the property the pop-out and fullscreen windows are built on.
// A feed that is popped out is on screen TWICE — its tile is still live
// in the voice room, because VoiceRoom moves tiles by geometry and never
// destroys them (SPEC §3.4) — and a fullscreen window can make it three.
// If the registry held one sink per (user, stream), opening a pop-out
// would steal the picture from the tile behind it, which is exactly the
// kind of failure that only shows up with two windows open and is
// invisible to every other test in the suite.
//
// No GUI, no decoder, no network: QVideoSink is a plain QObject surface
// and the frames here are synthesised. Runs offscreen.
#include <QtTest>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoSink>

#include "voice/video/VideoStreamRegistry.h"

namespace {

constexpr int kScreen = int(VideoStreamId::Screen);
constexpr int kCamera = int(VideoStreamId::Camera);

// A frame whose luma tells us which one it was, so a test can tell a
// replayed frame from a stale one.
QVideoFrame frameMarked(uchar mark, int w = 32, int h = 16)
{
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(QColor(mark, mark, mark));
    QVideoFrameFormat fmt(img.size(),
        QVideoFrameFormat::pixelFormatFromImageFormat(img.format()));
    QVideoFrame f(fmt);
    if (!f.map(QVideoFrame::WriteOnly)) return {};
    const int dstStride = f.bytesPerLine(0);
    for (int row = 0; row < h; ++row) {
        memcpy(f.bits(0) + row * dstStride, img.constScanLine(row),
               size_t(qMin(int(img.bytesPerLine()), dstStride)));
    }
    f.unmap();
    return f;
}

// The marker back out of whatever the sink is holding. -1 when the sink
// holds no valid frame at all (blanked, or never fed).
int markOf(const QVideoSink& sink)
{
    QVideoFrame f = sink.videoFrame();
    if (!f.isValid()) return -1;
    if (!f.map(QVideoFrame::ReadOnly)) return -2;
    const int m = int(*f.bits(0));
    f.unmap();
    return m;
}

} // namespace

class VideoRegistryTest : public QObject {
    Q_OBJECT
private slots:

    // ── The pop-out case ─────────────────────────────────────────────
    //
    // Two sinks on one stream: the in-room tile and a pop-out window.
    // Both get every frame. Neither is a copy of the decode — the same
    // QVideoFrame object is handed to each.
    void twoSinksOnOneStreamBothReceive()
    {
        VideoStreamRegistry reg;
        QVideoSink tile, popout;

        reg.attachOutput("@abe:x", kScreen, &tile);
        reg.attachOutput("@abe:x", kScreen, &popout);
        QCOMPARE(reg.outputCount("@abe:x", kScreen), 2);

        reg.deliverFrame("@abe:x", kScreen, frameMarked(40));
        QCOMPARE(markOf(tile), 40);
        QCOMPARE(markOf(popout), 40);

        reg.deliverFrame("@abe:x", kScreen, frameMarked(90));
        QCOMPARE(markOf(tile), 90);
        QCOMPARE(markOf(popout), 90);
    }

    // Attaching the same sink twice must not double it: deliverFrame
    // walks the list, and a duplicate would set the same frame twice per
    // frame forever. QML can retry an attach when a delegate is rebuilt.
    void attachingTheSameSinkTwiceIsIdempotent()
    {
        VideoStreamRegistry reg;
        QVideoSink tile;
        reg.attachOutput("@abe:x", kScreen, &tile);
        reg.attachOutput("@abe:x", kScreen, &tile);
        QCOMPARE(reg.outputCount("@abe:x", kScreen), 1);
    }

    // ── Attach replays the last frame ────────────────────────────────
    //
    // A pop-out opened in the middle of a 3 fps screen share must not sit
    // black until the next frame. The window that attaches second gets
    // the picture the first one is already showing.
    void attachReplaysTheLastFrameToALateSink()
    {
        VideoStreamRegistry reg;
        QVideoSink tile;
        reg.attachOutput("@abe:x", kScreen, &tile);
        reg.deliverFrame("@abe:x", kScreen, frameMarked(70));
        QCOMPARE(markOf(tile), 70);

        QVideoSink popout;
        QCOMPARE(markOf(popout), -1);           // blank before attaching
        reg.attachOutput("@abe:x", kScreen, &popout);
        QCOMPARE(markOf(popout), 70);           // caught up on attach
    }

    // Nothing has arrived yet: the late sink stays blank rather than
    // being handed an invalid frame that would count as "a picture".
    void attachToASilentStreamLeavesTheSinkBlank()
    {
        VideoStreamRegistry reg;
        QVideoSink popout;
        reg.attachOutput("@abe:x", kCamera, &popout);
        QCOMPARE(markOf(popout), -1);
    }

    // ── Closing one window leaves the other alone ────────────────────
    //
    // Closing a pop-out destroys its VideoOutput and therefore its sink.
    // The registry has to notice, keep feeding the tile, and above all
    // not write into freed memory on the next frame.
    void destroyingOneSinkLeavesTheOtherLive()
    {
        VideoStreamRegistry reg;
        QVideoSink tile;
        reg.attachOutput("@abe:x", kScreen, &tile);
        {
            QVideoSink popout;
            reg.attachOutput("@abe:x", kScreen, &popout);
            QCOMPARE(reg.outputCount("@abe:x", kScreen), 2);
            reg.deliverFrame("@abe:x", kScreen, frameMarked(50));
            QCOMPARE(markOf(popout), 50);
        }   // pop-out window closed
        QCOMPARE(reg.outputCount("@abe:x", kScreen), 1);

        reg.deliverFrame("@abe:x", kScreen, frameMarked(60));
        QCOMPARE(markOf(tile), 60);
    }

    // The explicit form, for a sink that outlives its interest.
    void detachOutputRemovesOnlyThatSink()
    {
        VideoStreamRegistry reg;
        QVideoSink tile, popout;
        reg.attachOutput("@abe:x", kScreen, &tile);
        reg.attachOutput("@abe:x", kScreen, &popout);

        reg.deliverFrame("@abe:x", kScreen, frameMarked(20));
        reg.detachOutput("@abe:x", kScreen, &popout);
        QCOMPARE(reg.outputCount("@abe:x", kScreen), 1);

        reg.deliverFrame("@abe:x", kScreen, frameMarked(80));
        QCOMPARE(markOf(tile), 80);
        // Detaching does not blank; the detached sink keeps what it had.
        QCOMPARE(markOf(popout), 20);

        // Unknown sink, unknown stream: both no-ops, not crashes.
        reg.detachOutput("@abe:x", kScreen, &popout);
        reg.detachOutput("@nobody:x", kCamera, &tile);
        QCOMPARE(reg.outputCount("@abe:x", kScreen), 1);
    }

    // ── The stream ends ──────────────────────────────────────────────
    //
    // Every surface goes blank together, so a pop-out cannot be left
    // frozen on the last frame of a share that stopped minutes ago. The
    // windows watch the same liveness the tiles do and close themselves.
    void stoppingTheStreamBlanksEverySink()
    {
        VideoStreamRegistry reg;
        QVideoSink tile, popout, fullscreen;
        reg.attachOutput("@abe:x", kScreen, &tile);
        reg.attachOutput("@abe:x", kScreen, &popout);
        reg.attachOutput("@abe:x", kScreen, &fullscreen);

        reg.deliverFrame("@abe:x", kScreen, frameMarked(30));
        QVERIFY(reg.hasLiveVideo("@abe:x", kScreen));

        QSignalSpy spy(&reg, &VideoStreamRegistry::liveVideoChanged);
        reg.setStreamAnnounced("@abe:x", kScreen, false);   // sender said stop

        QCOMPARE(markOf(tile), -1);
        QCOMPARE(markOf(popout), -1);
        QCOMPARE(markOf(fullscreen), -1);
        QVERIFY(!reg.hasLiveVideo("@abe:x", kScreen));
        QVERIFY(reg.streamStopped("@abe:x", kScreen));
        QCOMPARE(spy.count(), 1);

        // Attachments survive a stop: a share that comes back reaches
        // every window that was already open on it, with no re-attach.
        QCOMPARE(reg.outputCount("@abe:x", kScreen), 3);
        reg.deliverFrame("@abe:x", kScreen, frameMarked(45));
        QCOMPARE(markOf(tile), 45);
        QCOMPARE(markOf(popout), 45);
        QCOMPARE(markOf(fullscreen), 45);
    }

    // The peer left the channel, or we did. Same blanking, every stream.
    void droppingAPeerBlanksEverySinkOnEveryStream()
    {
        VideoStreamRegistry reg;
        QVideoSink share, sharePopout, cam;
        reg.attachOutput("@abe:x", kScreen, &share);
        reg.attachOutput("@abe:x", kScreen, &sharePopout);
        reg.attachOutput("@abe:x", kCamera, &cam);
        reg.deliverFrame("@abe:x", kScreen, frameMarked(30));
        reg.deliverFrame("@abe:x", kCamera, frameMarked(35));

        reg.dropUser("@abe:x");

        QCOMPARE(markOf(share), -1);
        QCOMPARE(markOf(sharePopout), -1);
        QCOMPARE(markOf(cam), -1);
        QVERIFY(!reg.hasLiveVideo("@abe:x", kScreen));
        QVERIFY(!reg.hasLiveVideo("@abe:x", kCamera));
    }

    // Leaving the channel clears everything at once.
    void clearBlanksEverySinkOfEveryPeer()
    {
        VideoStreamRegistry reg;
        QVideoSink abe, abePopout, bea;
        reg.attachOutput("@abe:x", kScreen, &abe);
        reg.attachOutput("@abe:x", kScreen, &abePopout);
        reg.attachOutput("@bea:x", kCamera, &bea);
        reg.deliverFrame("@abe:x", kScreen, frameMarked(30));
        reg.deliverFrame("@bea:x", kCamera, frameMarked(31));

        reg.clear();

        QCOMPARE(markOf(abe), -1);
        QCOMPARE(markOf(abePopout), -1);
        QCOMPARE(markOf(bea), -1);
    }

    // ── Streams stay separate ────────────────────────────────────────
    //
    // A peer sharing AND on camera is two feeds and can be two pop-outs.
    // The camera's frames must not land in the share's windows.
    void streamsOfOnePeerDoNotCrossFeed()
    {
        VideoStreamRegistry reg;
        QVideoSink share, cam;
        reg.attachOutput("@abe:x", kScreen, &share);
        reg.attachOutput("@abe:x", kCamera, &cam);

        reg.deliverFrame("@abe:x", kScreen, frameMarked(10));
        QCOMPARE(markOf(share), 10);
        QCOMPARE(markOf(cam), -1);

        reg.deliverFrame("@abe:x", kCamera, frameMarked(20));
        QCOMPARE(markOf(share), 10);
        QCOMPARE(markOf(cam), 20);

        reg.dropStream("@abe:x", kCamera);
        QCOMPARE(markOf(share), 10);    // the share is untouched
        QCOMPARE(markOf(cam), -1);
    }

    // The legacy JPEG-stills path fans out the same way — it lands in
    // deliverFrame, but a regression that bypassed it would be silent.
    void legacyStillsReachEverySinkToo()
    {
        VideoStreamRegistry reg;
        QVideoSink tile, popout;
        reg.attachOutput("@abe:x", kScreen, &tile);
        reg.attachOutput("@abe:x", kScreen, &popout);

        QImage img(8, 8, QImage::Format_ARGB32);
        img.fill(Qt::red);
        reg.deliverImage("@abe:x", kScreen, img);

        QVERIFY(tile.videoFrame().isValid());
        QVERIFY(popout.videoFrame().isValid());
        QCOMPARE(tile.videoFrame().size(), QSize(8, 8));
        QCOMPARE(popout.videoFrame().size(), QSize(8, 8));
    }
};

QTEST_MAIN(VideoRegistryTest)
#include "test_video_registry.moc"
