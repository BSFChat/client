import QtQuick
import QtQuick.Layouts
import QtTest
import BSFChat

// MessageInput.qml — the shipped composer's send control, instantiated and
// actually pressed.
//
// WHY THIS FILE EXISTS
//
// `everyComposerHasAVisibleSendControl()` in tests/test_qml_hygiene.cpp
// asserts three strings are present in the file:
//
//     src.contains("sendCurrentMessage()")
//     src.contains("Accessible.name: qsTr(\"Send")
//     /onClicked\s*:[^\n]*_?[Ss]end/
//
// Its own comment says what it is reaching for — "a finger has to be able to
// reach it. An Accessible annotation on nothing is worse than none." That is
// a statement about a control's SIZE, its OPACITY and whether its input
// handler is ENABLED, and none of the three strings above is evidence of any
// of them. All three are still true of a send button faded to opacity 0 with
// a disabled MouseArea, which is precisely what this composer's button is
// for most of its life: `opacity: armed ? 1.0 : 0.0`, `enabled: sendBtn.armed`.
//
// So the text check cannot distinguish the state it wants (armed, reachable)
// from the state it is trying to rule out (annotated, unreachable) — the two
// are the same source text. Only pressing it can.
//
// The environment is stubbed in tests/qml_components_test_main.cpp; the
// composer, its Theme and its icons are the shipped files.
TestCase {
    id: tc
    name: "ComposerSendReal"
    when: windowShown
    width: 600
    height: 220
    visible: true

    Item {
        id: frame
        anchors.fill: parent

        MessageInput {
            id: composer
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
        }
    }

    // ── helpers ──────────────────────────────────────────────────────

    function conn() {
        return serverManager.activeServer;
    }

    function descendants(item) {
        var out = [];
        if (!item || item.children === undefined)
            return out;
        for (var i = 0; i < item.children.length; ++i) {
            var c = item.children[i];
            out.push(c);
            out = out.concat(descendants(c));
        }
        return out;
    }

    // The send control, found by its accessible name rather than by index or
    // by colour — the name is the contract the hygiene guard was reaching
    // for, and finding the control THROUGH it means this test also fails if
    // the annotation and the control ever come apart.
    function sendControl() {
        var d = descendants(composer);
        for (var i = 0; i < d.length; ++i) {
            var it = d[i];
            if (it.Accessible && String(it.Accessible.name) === "Send message")
                return it;
        }
        return null;
    }

    // The text area. Every composer has exactly one thing with a
    // `preeditText` property, which is TextArea/TextInput and nothing else
    // in this tree.
    function inputArea() {
        var d = descendants(composer);
        for (var i = 0; i < d.length; ++i)
            if (d[i].preeditText !== undefined)
                return d[i];
        return null;
    }

    // Is the point at the centre of `item` actually delivered to `item`?
    //
    // This is the question "can a finger reach it". An item can be visible,
    // sized and on screen and still be untouchable because something is laid
    // out over it, so the answer is taken by hit-testing the scene rather
    // than by reading any property: childAt() walks the real scene graph the
    // way a press does.
    function centreHitsTheControl(item) {
        var p = item.mapToItem(frame, item.width / 2, item.height / 2);
        var hit = frame.childAt(p.x, p.y);
        // childAt returns the topmost direct-ish child; walk down to the
        // deepest item under the point.
        while (hit) {
            var next = hit.childAt(hit.mapFromItem(frame, p.x, p.y).x,
                                   hit.mapFromItem(frame, p.x, p.y).y);
            if (!next || next === hit) break;
            hit = next;
        }
        // The control itself, or something inside it (its Icon), both mean
        // the press lands on the control.
        while (hit) {
            if (hit === item) return true;
            hit = hit.parent;
        }
        return false;
    }

    function typeInto(text) {
        var ta = inputArea();
        verify(ta !== null, "composer has no text area");
        ta.text = text;
    }

    function init() {
        serverManager.resetForTest();
        appSettings.resetForTest();
        typeInto("");
        tryVerify(function() {
            var b = sendControl();
            return b !== null && b.opacity < 0.01;
        });
    }

    // ── 1. the control exists, on the shipped component ──────────────

    function test_a_the_composer_has_a_named_send_control() {
        verify(composer !== null);
        var b = sendControl();
        verify(b !== null,
               "no control in the shipped composer is named \"Send message\"");
        // Named AND a button, not a decorative rectangle that happens to
        // carry a name.
        compare(b.Accessible.role, Accessible.Button);
    }

    // ── 2. the state the text check cannot tell apart ────────────────

    // An empty composer: the control is annotated and laid out, and it is
    // NOT reachable. Asserted so the next case means something — "it becomes
    // reachable" is only a fact if there is a state in which it is not.
    function test_b_an_empty_composer_does_not_offer_to_send() {
        var b = sendControl();
        tryCompare(b, "opacity", 0.0);

        // And it refuses the press rather than merely looking faded. This is
        // the half a screenshot would miss.
        compare(conn().sendCalls(), 0);
        mouseClick(b);
        compare(conn().sendCalls(), 0,
                "an empty composer sent something");
    }

    // ── 3. THE ASSERTION ─────────────────────────────────────────────

    // Type something, and the control becomes a thing a finger can hit.
    //
    // Every clause here is a measurement, and every one of them is
    // compatible with the three strings the hygiene guard greps for being
    // present and the control being unusable.
    function test_c_a_typed_message_makes_the_control_reachable() {
        typeInto("hello");
        var b = sendControl();

        // Visible, in the sense that matters: this button is faded, never
        // `visible: false`, so `visible` alone would be true even at
        // opacity 0.
        tryCompare(b, "opacity", 1.0);
        tryVerify(function() { return b.width > 0 && b.height > 0; },
                  5000, "the send control has no size");

        // Inside the composer it belongs to. A control laid out past the
        // right edge is annotated, sized, opaque and off the screen — the
        // exact failure that put the voice dock's hang-up button 197dp off a
        // Pixel.
        var pos = b.mapToItem(composer, 0, 0);
        tryVerify(function() {
            var p = b.mapToItem(composer, 0, 0);
            return p.x >= 0 && p.y >= 0
                && p.x + b.width <= composer.width + 0.5
                && p.y + b.height <= composer.height + 0.5;
        }, 5000, "the send control is outside the composer: x " + pos.x
                 + ", width " + b.width + ", composer width " + composer.width);

        // And nothing is on top of it.
        waitForRendering(frame);
        verify(centreHitsTheControl(b),
               "the centre of the send control does not hit-test to it — "
               + "something is laid out over it");
    }

    // Pressing it sends. The wiring `onClicked: sendCurrentMessage()` is
    // grepped for today; whether that handler is on an ENABLED MouseArea
    // covering the button is not, and `enabled: sendBtn.armed` is exactly
    // the property that decides it.
    function test_d_pressing_the_control_sends_the_message() {
        typeInto("hello world");
        var b = sendControl();
        tryCompare(b, "opacity", 1.0);

        compare(conn().sendCalls(), 0);
        mouseClick(b);

        tryVerify(function() { return conn().sendCalls() === 1; },
                  5000, "clicking the shipped send control sent nothing");
        compare(conn().lastSentBody(), "hello world");

        // And the composer cleared, which is how the user knows it went.
        tryCompare(inputArea(), "text", "");
    }

    // The refusal path, which is the other half of "armed". An over-length
    // message must not be sendable by tapping — Return and the accessible
    // press action are checked inside sendCurrentMessage(), but the button's
    // own MouseArea is gated separately, and a control that looks pressable
    // and silently does nothing is worse than one that looks disabled.
    function test_e_an_over_length_message_disarms_the_control() {
        // maxMessageBytes is 65536 in the stub, matching the server default.
        var big = "x".repeat(70000);
        typeInto(big);

        var b = sendControl();
        tryCompare(b, "opacity", 0.0);
        mouseClick(b);
        compare(conn().sendCalls(), 0,
                "an over-length message was sent by tapping");

        // Back under the limit and it arms again — so the disarm is the
        // limit talking, not the control having died.
        typeInto("short again");
        tryCompare(b, "opacity", 1.0);
        mouseClick(b);
        tryVerify(function() { return conn().sendCalls() === 1; });
        compare(conn().lastSentBody(), "short again");
    }
}
