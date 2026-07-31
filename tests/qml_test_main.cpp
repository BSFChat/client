// Runner for the QML-side unit tests under tests/qml/.
//
// These cover pure functions only (qml/js/*.js) — no windows, no decoder, no
// input. Interaction behaviour in the video player (hit-testing, z-order,
// focus) is deliberately NOT tested here: a QML test can synthesise a click on
// a specific item, which is exactly the thing that was never in doubt; what
// broke was which item was under the cursor in a real window.
#include <QtQuickTest>

QUICK_TEST_MAIN(qml)
