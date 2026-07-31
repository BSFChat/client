import QtQuick
import QtQuick.Layouts
import QtTest
import "../../qml/js/PlaybackMath.js" as PlaybackMath

// Inline video card sizing: the numbers MessageBubble.qml reserves for the row
// before the card exists, and that VideoPlayerCard.qml then lays out.
//
// The whole point is that those two are the same number. If they diverge the
// row resizes after first paint, contentHeight grows under the viewport, and
// the channel appears to scroll up on its own — the defect this fixes. So this
// file checks the arithmetic AND, on a replica of the card's outer layout,
// that a ColumnLayout built to those numbers really does report that height.
//
// The replica caveat from tst_transportbarinput.qml applies: the real
// component imports the BSFChat module, which only the app binary has.
TestCase {
    id: tc
    name: "CardGeometry"
    when: windowShown
    width: 500
    height: 400
    visible: true

    // Desktop envelope is 400x300, mobile 260x200.
    function test_boxFitsInsideTheEnvelope_data() {
        return [
            // 16:9 is width-bound: 1920/400 is the tighter ratio.
            { tag: "1080p",     w: 1920, h: 1080, mobile: false, bw: 400, bh: 225 },
            // 4:3 fills the envelope exactly.
            { tag: "4:3",       w: 640,  h: 480,  mobile: false, bw: 400, bh: 300 },
            // Portrait (a phone clip) is height-bound — the naive
            // scale-to-width would make it 400x711 and blow the row open.
            { tag: "portrait",  w: 1080, h: 1920, mobile: false, bw: 169, bh: 300 },
            { tag: "square",    w: 800,  h: 800,  mobile: false, bw: 300, bh: 300 },
            // Smaller than the cap: shown at native size, never upscaled.
            { tag: "tiny",      w: 160,  h: 120,  mobile: false, bw: 160, bh: 120 },
            { tag: "1080p mob", w: 1920, h: 1080, mobile: true,  bw: 260, bh: 146 },
            { tag: "portrait mob", w: 1080, h: 1920, mobile: true, bw: 113, bh: 200 },
        ];
    }
    function test_boxFitsInsideTheEnvelope(d) {
        compare(PlaybackMath.cardBoxWidth(d.w, d.h, d.mobile), d.bw, "width");
        compare(PlaybackMath.cardBoxHeight(d.w, d.h, d.mobile), d.bh, "height");
        verify(PlaybackMath.cardBoxWidth(d.w, d.h, d.mobile)
               <= PlaybackMath.cardMaxWidth(d.mobile));
        verify(PlaybackMath.cardBoxHeight(d.w, d.h, d.mobile)
               <= PlaybackMath.cardMaxHeight(d.mobile));
    }

    // Aspect ratio survives the fit, to within the rounding.
    function test_aspectRatioPreserved_data() {
        return [
            { tag: "16:9",     w: 1920, h: 1080 },
            { tag: "portrait", w: 1080, h: 1920 },
            { tag: "cinema",   w: 2048, h: 858 },
        ];
    }
    function test_aspectRatioPreserved(d) {
        var bw = PlaybackMath.cardBoxWidth(d.w, d.h, false);
        var bh = PlaybackMath.cardBoxHeight(d.w, d.h, false);
        fuzzyCompare(bw / bh, d.w / d.h, 0.02);
    }

    // No info.w/h from the server: a 4:3 box at the cap width, which is what
    // the inline-image path in MessageBubble.qml falls back to. Same missing
    // information, same reservation — two fallbacks is how they drift apart.
    function test_missingDimensionsFallBackToFourThirds_data() {
        return [
            { tag: "both zero",  w: 0,   h: 0,   mobile: false, bw: 400, bh: 300 },
            { tag: "width only", w: 640, h: 0,   mobile: false, bw: 400, bh: 300 },
            { tag: "height only",w: 0,   h: 480, mobile: false, bw: 400, bh: 300 },
            { tag: "negative",   w: -1,  h: -1,  mobile: false, bw: 400, bh: 300 },
            { tag: "mobile",     w: 0,   h: 0,   mobile: true,  bw: 260, bh: 195 },
        ];
    }
    function test_missingDimensionsFallBackToFourThirds(d) {
        compare(PlaybackMath.cardBoxWidth(d.w, d.h, d.mobile), d.bw);
        compare(PlaybackMath.cardBoxHeight(d.w, d.h, d.mobile), d.bh);
    }

    function test_captionAddsItsRowAndTheSpacing() {
        var noCaption = PlaybackMath.cardHeight(640, 480, false, false, 14, 4);
        var withCaption = PlaybackMath.cardHeight(640, 480, false, true, 14, 4);
        compare(noCaption, 300, "a card with no filename is just the box");
        compare(withCaption, 300 + 21 + 4, "box + caption + one spacing");
        compare(PlaybackMath.captionRowHeight(false, 14), 0);
    }

    // The claim the fix rests on: a ColumnLayout built the way
    // VideoPlayerCard.qml builds it reports exactly the height
    // MessageBubble.qml reserved for the row. Checked against the live layout
    // engine because the failure modes here are its rules, not arithmetic —
    // spacing counted for a hidden row, or a pinned row that quietly grows to
    // its content.
    Component {
        id: cardReplica
        ColumnLayout {
            property int mediaW: 0
            property int mediaH: 0
            property string fileName: ""
            spacing: 4
            Rectangle {
                Layout.preferredWidth: PlaybackMath.cardBoxWidth(
                    parent.mediaW, parent.mediaH, false)
                Layout.preferredHeight: PlaybackMath.cardBoxHeight(
                    parent.mediaW, parent.mediaH, false)
            }
            RowLayout {
                visible: parent.fileName !== ""
                Layout.preferredHeight: PlaybackMath.captionRowHeight(
                    parent.fileName !== "", 14)
                Layout.maximumHeight: Layout.preferredHeight
                Text { text: parent.parent.fileName; font.pixelSize: 14 }
                Text { text: "· 4.0 MB"; font.pixelSize: 11 }
            }
        }
    }

    function test_laidOutHeightMatchesTheReservation_data() {
        return [
            { tag: "16:9 with caption",   w: 1920, h: 1080, name: "clip.mp4" },
            { tag: "16:9 no caption",     w: 1920, h: 1080, name: "" },
            { tag: "portrait",            w: 1080, h: 1920, name: "phone.mov" },
            { tag: "no dimensions",       w: 0,    h: 0,    name: "unknown.mkv" },
            // A long filename must not wrap the caption into a second line and
            // push everything below it down.
            { tag: "long name", w: 640, h: 480,
              name: "a-really-quite-long-recording-filename-from-a-phone.mp4" },
        ];
    }
    function test_laidOutHeightMatchesTheReservation(d) {
        var card = cardReplica.createObject(tc, {
            mediaW: d.w, mediaH: d.h, fileName: d.name });
        verify(card !== null);
        card.width = 400;
        wait(0);          // let the layout polish
        var reserved = PlaybackMath.cardHeight(d.w, d.h, false, d.name !== "", 14, 4);
        compare(card.implicitHeight, reserved,
                "the row reserved " + reserved + " but the card laid out at "
                + card.implicitHeight);
        card.destroy();
    }
}
