// Joining voice must not freeze the window while the audio devices open.
//
// AudioEngine::start() made the GUI thread wait for the audio thread to open
// the microphone and speakers — on macOS VoiceProcessingIO setup and a
// Bluetooth headset's profile switch, one to three seconds of a frozen app on
// every join. startAsync() opens them on the audio thread and reports back.
// These pin the contract the voice engine relies on: it returns before the
// pipeline is up, it reports exactly once, and a stop (or a stop and a new
// start) before the report arrives never lets a stale verdict through.
//
// Whether the devices actually open depends on the machine (a CI runner has
// none), so the tests accept either verdict and check that the engine's state
// agrees with it.

#include "voice/AudioEngine.h"

#include <QSignalSpy>
#include <QTest>

class TestAudioEngineAsync : public QObject {
    Q_OBJECT

private slots:
    void itReturnsBeforeTheDevicesAreOpenAndReportsOnce()
    {
        AudioEngine engine;
        QSignalSpy spy(&engine, &AudioEngine::startFinished);
        engine.startAsync();
        // The blocking start() would already be running here.
        QVERIFY(!engine.isRunning());
        QCOMPARE(spy.count(), 0);

        QVERIFY(spy.wait(15000));
        const bool ok = spy.at(0).at(0).toBool();
        QCOMPARE(engine.isRunning(), ok);
        QTest::qWait(200);
        QCOMPARE(spy.count(), 1);

        engine.stop();
        QVERIFY(!engine.isRunning());
    }

    void aStopBeforeTheVerdictReportsNothing()
    {
        AudioEngine engine;
        QSignalSpy spy(&engine, &AudioEngine::startFinished);
        engine.startAsync();
        engine.stop();
        QVERIFY(!engine.isRunning());
        QVERIFY(!spy.wait(1000));
        QCOMPARE(spy.count(), 0);
    }

    void aRestartIgnoresTheFirstAttemptsVerdict()
    {
        AudioEngine engine;
        QSignalSpy spy(&engine, &AudioEngine::startFinished);
        engine.startAsync();
        engine.stop();
        engine.startAsync();
        QVERIFY(spy.wait(15000));
        QTest::qWait(300);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(engine.isRunning(), spy.at(0).at(0).toBool());
        engine.stop();
    }

    // Leaving or switching channels used to block while the devices closed.
    // stop() now hands that to the audio thread; the next start waits for
    // it, so two pipelines never overlap (two VoiceProcessingIO units
    // fighting over the hardware on macOS).
    void stopReturnsAtOnceAndTheNextStartWaitsForIt()
    {
        AudioEngine first;
        QSignalSpy firstSpy(&first, &AudioEngine::startFinished);
        first.startAsync();
        QVERIFY(firstSpy.wait(15000));
        first.stop();
        QVERIFY(!first.isRunning());
        // The old thread is reaped on this thread's event loop, which has
        // not run since stop(): it is still listed as retiring.
        QVERIFY(AudioEngine::retiringInProgress());

        AudioEngine second;
        QSignalSpy secondSpy(&second, &AudioEngine::startFinished);
        second.startAsync();
        QVERIFY(!second.isRunning()); // deferred, not started over it
        QVERIFY(secondSpy.wait(15000));
        QVERIFY(!AudioEngine::retiringInProgress());
        QCOMPARE(second.isRunning(), secondSpy.at(0).at(0).toBool());
        second.stop();
        QVERIFY(AudioEngine::waitForRetiredThreads(5000));
    }

    void aDeferredStartCanBeCancelled()
    {
        AudioEngine first;
        QSignalSpy firstSpy(&first, &AudioEngine::startFinished);
        first.startAsync();
        QVERIFY(firstSpy.wait(15000));
        first.stop();

        AudioEngine second;
        QSignalSpy secondSpy(&second, &AudioEngine::startFinished);
        second.startAsync(); // deferred behind first's teardown
        second.stop();       // and cancelled before it ran
        QVERIFY(!secondSpy.wait(1500));
        QVERIFY(!second.isRunning());
        QVERIFY(!AudioEngine::retiringInProgress());
    }

    void theBlockingStartStillWorks()
    {
        // Kept for callers that need the verdict inline; same pipeline.
        AudioEngine engine;
        const bool ok = engine.start();
        QCOMPARE(engine.isRunning(), ok);
        engine.stop();
        QVERIFY(!engine.isRunning());
    }
};

QTEST_GUILESS_MAIN(TestAudioEngineAsync)
#include "test_audio_engine_async.moc"
