// macOS screen capture: which path runs, and what a failure does.
//
// Incident: 0.0.44 on macOS 26.3 kept prompting the owner for Screen
// Recording permission the app "already had". MacScreenCapturer asked
// for the legacy blanket grant (CGRequestScreenCaptureAccess) on the
// picker path, where no grant is needed, and carried a fallback that
// captured a whole display without the picker — the access macOS 15+
// re-confirms periodically. ScreenCapturePolicy.h holds the rules; this
// pins them, and pins the .mm source against the two calls returning.
//
// Headless by design: nothing here touches ScreenCaptureKit or starts
// bsfchat-app. Running a capture on a dev Mac pops the very prompts the
// fix is about.

#include "voice/ScreenCapturePolicy.h"

#include <QFile>
#include <QRegularExpression>
#include <QTest>

using namespace screencap;

class TestScreenCapturePolicy : public QObject {
    Q_OBJECT

private:
    static QString capturerSource()
    {
        QFile f(QStringLiteral(BSFCHAT_SRC_DIR "/voice/MacScreenCapturer.mm"));
        if (!f.open(QIODevice::ReadOnly)) return {};
        return QString::fromUtf8(f.readAll());
    }

    // The source with // comments stripped — the file explains at
    // length why these calls are gone, and naming them there is fine.
    static QString capturerCode()
    {
        QString src = capturerSource();
        src.remove(QRegularExpression(QStringLiteral("//[^\n]*")));
        return src;
    }

private slots:
    // --- Which path a share request takes -----------------------------

    void startPresentsPickerWhenAvailable()
    {
        QCOMPARE(routeStart(true), StartRoute::PresentPicker);
    }

    void startWithoutPickerFailsInsteadOfCapturingADisplay()
    {
        QCOMPARE(routeStart(false), StartRoute::Unsupported);
    }

    // --- What a capture tick grabs ------------------------------------

    void tickCapturesThePickedFilter()
    {
        QCOMPARE(routeTick(true, true), TickRoute::CaptureWithPickerFilter);
    }

    // The heart of it: before a pick, after a cancel, after a stop — no
    // filter means grab NOTHING. The old code built a display filter
    // from SCShareableContent here.
    void tickWithoutFilterGrabsNothing()
    {
        QCOMPARE(routeTick(true, false), TickRoute::Skip);
    }

    void tickWithoutPickerGrabsNothing()
    {
        QCOMPARE(routeTick(false, false), TickRoute::Unsupported);
        QCOMPARE(routeTick(false, true), TickRoute::Unsupported);
    }

    // --- Failures on the picker path ----------------------------------

    void selectionGoneEndsTheShareImmediately_data()
    {
        QTest::addColumn<long>("code");
        QTest::newRow("user stopped from menu bar") << sc_error::UserStopped;
        QTest::newRow("system stopped")             << sc_error::SystemStopped;
        QTest::newRow("window/display gone")        << sc_error::NoCaptureSource;
        QTest::newRow("no window list")             << sc_error::NoWindowList;
        QTest::newRow("no display list")            << sc_error::NoDisplayList;
    }

    void selectionGoneEndsTheShareImmediately()
    {
        QFETCH(long, code);
        const auto kind = classifyFailure(true, code);
        QCOMPARE(kind, FailureKind::SelectionEnded);
        // First failure, nothing reported yet: no five-tick wait on a
        // filter that can never capture again.
        QCOMPARE(onCaptureFailure(kind, 1, 5, false), FailureAction::EndShare);
    }

    // The same numbers from another error domain are not these errors.
    void foreignDomainCodesAreNotSelectionEnd()
    {
        QCOMPARE(classifyFailure(false, sc_error::UserStopped),
                 FailureKind::Retry);
    }

    void otherFailuresRetryThenReportOnce()
    {
        const auto kind = classifyFailure(true, sc_error::UserDeclined);
        QCOMPARE(kind, FailureKind::Retry);
        for (int n = 1; n < 5; ++n)
            QCOMPARE(onCaptureFailure(kind, n, 5, false),
                     FailureAction::KeepTrying);
        QCOMPARE(onCaptureFailure(kind, 5, 5, false),
                 FailureAction::ReportFailure);
        // Latched per share attempt: not re-reported every tick after.
        QCOMPARE(onCaptureFailure(kind, 6, 5, true),
                 FailureAction::KeepTrying);
    }

    void menuBarStopIsSilentOtherEndsExplain()
    {
        QCOMPARE(QString::fromUtf8(selectionEndedMessage(sc_error::UserStopped)),
                 QString());
        QVERIFY(QString::fromUtf8(
                    selectionEndedMessage(sc_error::NoCaptureSource))
                    .contains(QStringLiteral("Share again")));
        QVERIFY(QString::fromUtf8(
                    selectionEndedMessage(sc_error::SystemStopped))
                    .contains(QStringLiteral("Share again")));
    }

    // --- Source guards ------------------------------------------------
    //
    // No behavioural test can observe these from a headless run, and
    // each has an obvious-looking reason to come back ("SCScreenshotManager
    // never prompts, so we must ask"; "no filter yet, capture the main
    // display"). Both are exactly what produced the prompts.

    void capturerSourceIsReadable()
    {
        QVERIFY2(!capturerSource().isEmpty(),
                 "could not read src/voice/MacScreenCapturer.mm");
    }

    void capturerNeverRequestsLegacyScreenRecordingGrant()
    {
        const QString code = capturerCode();
        QVERIFY2(!code.contains(QStringLiteral("CGRequestScreenCaptureAccess")),
                 "MacScreenCapturer.mm requests the legacy Screen Recording "
                 "grant. The picker needs none; asking re-prompts every "
                 "launch. See ScreenCapturePolicy.h.");
        QVERIFY2(!code.contains(QStringLiteral("CGPreflightScreenCaptureAccess")),
                 "MacScreenCapturer.mm consults the legacy grant; a false "
                 "preflight is normal for a picker-only app.");
    }

    void capturerNeverCapturesOutsideThePicker()
    {
        const QString code = capturerCode();
        QVERIFY2(!code.contains(QStringLiteral("SCShareableContent")),
                 "MacScreenCapturer.mm enumerates shareable content — "
                 "capture outside the picker, which macOS 15+ periodically "
                 "makes the user re-approve.");
        QVERIFY2(!code.contains(QStringLiteral("initWithDisplay")),
                 "MacScreenCapturer.mm builds its own display filter.");
        QVERIFY2(!code.contains(QStringLiteral("initWithDesktopIndependentWindow")),
                 "MacScreenCapturer.mm builds its own window filter.");
        QVERIFY2(!code.contains(QStringLiteral("CGDisplayCreateImage")),
                 "MacScreenCapturer.mm uses a legacy capture API.");
        QVERIFY2(!code.contains(QStringLiteral("CGWindowListCreateImage")),
                 "MacScreenCapturer.mm uses a legacy capture API.");
    }
};

QTEST_GUILESS_MAIN(TestScreenCapturePolicy)
#include "test_screen_capture_policy.moc"
