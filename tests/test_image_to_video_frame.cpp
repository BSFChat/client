// imageToVideoFrame must keep every row where it belongs whatever the
// width. QVideoFrame pads lines to 16 bytes and a packed QImage does not,
// so the widths that are not a multiple of four pixels are the ones that
// used to come out sheared.

#include "voice/video/ImageToVideoFrame.h"

#include <QtTest>

class TestImageToVideoFrame : public QObject {
    Q_OBJECT
private slots:
    void rowsLandInPlace_data() {
        QTest::addColumn<int>("width");
        for (int w : {1, 2, 3, 4, 5, 6, 7, 8, 1001, 1279, 1280, 1283})
            QTest::newRow(qPrintable(QString::number(w))) << w;
    }

    void rowsLandInPlace() {
        QFETCH(int, width);
        const int height = 37;
        QImage img(width, height, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                img.setPixel(x, y, qRgba(x & 0xff, y & 0xff, (x >> 8) & 0xff, 0xff));

        QVideoFrame vf = imageToVideoFrame(img);
        QVERIFY(vf.isValid());
        QCOMPARE(vf.size(), img.size());
        QVERIFY(vf.map(QVideoFrame::ReadOnly));
        const int bpl = vf.bytesPerLine(0);
        QVERIFY(bpl >= width * 4);
        for (int y = 0; y < height; ++y) {
            const auto* row = reinterpret_cast<const QRgb*>(vf.bits(0) + y * bpl);
            for (int x = 0; x < width; ++x) {
                if (row[x] != img.pixel(x, y)) {
                    vf.unmap();
                    QFAIL(qPrintable(QStringLiteral("pixel (%1,%2) moved").arg(x).arg(y)));
                }
            }
        }
        vf.unmap();
    }

    void nullImageGivesInvalidFrame() {
        QVERIFY(!imageToVideoFrame(QImage()).isValid());
    }
};

QTEST_GUILESS_MAIN(TestImageToVideoFrame)
#include "test_image_to_video_frame.moc"
