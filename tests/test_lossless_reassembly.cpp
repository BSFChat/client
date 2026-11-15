// Chunked lossless-video reassembly (V-7): a peer-supplied chunk total
// must not let one frame grow past the byte cap, a frame that does is
// dropped whole, and normal frames still reassemble.

#include <QtTest>

#include "voice/LosslessChunkAssembler.h"

using R = LosslessChunkAssembler::Result;

class TestLosslessReassembly : public QObject {
    Q_OBJECT
private slots:
    void reassemblesInOrderChunks() {
        LosslessChunkAssembler a;
        QByteArray out;
        QCOMPARE(a.add(7, 0, 3, "ab", 2, &out), R::Incomplete);
        QCOMPARE(a.add(7, 1, 3, "cd", 2, &out), R::Incomplete);
        QCOMPARE(a.add(7, 2, 3, "e", 1, &out), R::Complete);
        QCOMPARE(out, QByteArray("abcde"));
        QCOMPARE(a.bufferedBytes(), 0);
    }

    void dropsOnGapAndRecoversAtNextChunkZero() {
        LosslessChunkAssembler a;
        QByteArray out;
        QCOMPARE(a.add(1, 0, 3, "a", 1, &out), R::Incomplete);
        QCOMPARE(a.add(1, 2, 3, "c", 1, &out), R::Dropped);
        QCOMPARE(a.bufferedBytes(), 0);
        QCOMPARE(a.add(2, 0, 2, "x", 1, &out), R::Incomplete);
        QCOMPARE(a.add(2, 1, 2, "y", 1, &out), R::Complete);
        QCOMPARE(out, QByteArray("xy"));
    }

    void rejectsMalformedHeader() {
        LosslessChunkAssembler a;
        QByteArray out;
        QCOMPARE(a.add(1, 0, 0, "a", 1, &out), R::Ignored);
        QCOMPARE(a.add(1, 3, 3, "a", 1, &out), R::Ignored);
    }

    void capsBytesPerFrame() {
        // Cap of 10 bytes: a 65535-chunk frame stops at the cap, its tail
        // is ignored without buffering, and the next frame is fine.
        LosslessChunkAssembler a(10);
        QByteArray out;
        const QByteArray chunk(4, 'z');
        QCOMPARE(a.add(5, 0, 65535, chunk.constData(), 4, &out), R::Incomplete);
        QCOMPARE(a.add(5, 1, 65535, chunk.constData(), 4, &out), R::Incomplete);
        QCOMPARE(a.add(5, 2, 65535, chunk.constData(), 4, &out), R::Oversize);
        QCOMPARE(a.bufferedBytes(), 0);
        for (quint16 i = 3; i < 100; ++i)
            QCOMPARE(a.add(5, i, 65535, chunk.constData(), 4, &out), R::Ignored);
        QCOMPARE(a.bufferedBytes(), 0);
        QCOMPARE(a.add(6, 0, 2, chunk.constData(), 4, &out), R::Incomplete);
        QCOMPARE(a.add(6, 1, 2, chunk.constData(), 4, &out), R::Complete);
        QCOMPARE(out.size(), 8);
    }

    void exactlyAtCapIsAccepted() {
        LosslessChunkAssembler a(8);
        QByteArray out;
        QCOMPARE(a.add(1, 0, 2, "abcd", 4, &out), R::Incomplete);
        QCOMPARE(a.add(1, 1, 2, "efgh", 4, &out), R::Complete);
        QCOMPARE(out, QByteArray("abcdefgh"));
    }

    void defaultCapIs32MiB() {
        QCOMPARE(LosslessChunkAssembler::kMaxFrameBytes, qsizetype(32) * 1024 * 1024);
    }
};

QTEST_APPLESS_MAIN(TestLosslessReassembly)
#include "test_lossless_reassembly.moc"
