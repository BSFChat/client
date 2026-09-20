import QtQuick
import QtQuick.Controls
import QtTest
import "../../qml/js/ComboBoxText.js" as ComboBoxText

// ThemedComboBox's label resolution, against REAL C++-backed models.
//
// The owner's report, on a release build: Channel settings > Role
// overrides, "the role field list isn't populated or has black text on a
// black background". Neither. The popup had the right height and six
// rows; the rows had no text, and so did the closed field.
//
// The models below come from `probe`, a context property registered in
// tests/combobox_model_test_main.cpp whose declarations use the same
// metatypes the app does — `QJsonArray jsonRoles` is
// ServerConnection::serverRoles, `QVariantList variantRoles` is
// Settings::audioInputDevices. That is the entire point of this file. A
// version of it written against a QML array literal — the shape the
// slowmode combo in the very same dialog uses — passes on the broken
// code, which is how this shipped.
//
// ThemedComboBox.qml itself cannot be instantiated here (it imports the
// BSFChat module, compiled into the app binary), so the real ComboBoxes
// below stand in for its ComboBox base and the resolver under test is
// the real qml/js/ComboBoxText.js the component imports. The wiring
// between the two — that ThemedComboBox actually routes its row text and
// its closed field through the resolver rather than bare textAt /
// displayText — is pinned by test_qml_hygiene.
TestCase {
    id: tc
    name: "ComboBoxText"

    Component {
        id: comboComp
        ComboBox {}
    }

    function make(model, textRole) {
        var cb = createTemporaryObject(comboComp, tc);
        verify(cb, "ComboBox failed to instantiate");
        cb.model = model;
        if (textRole !== undefined) cb.textRole = textRole;
        return cb;
    }

    // Read a whole popup the way ThemedComboBox's delegate does.
    function rows(cb) {
        var out = [];
        for (var i = 0; i < cb.count; ++i)
            out.push(ComboBoxText.resolve(cb.textAt(i), cb.model, i, cb.textRole));
        return out;
    }

    // And the closed field, the way its contentItem does.
    function field(cb) {
        return ComboBoxText.resolve(cb.displayText, cb.model,
                                    cb.currentIndex, cb.textRole);
    }

    // ── 1. The Qt behaviour this whole file exists for ───────────────
    //
    // Pinned as a fact, not as an aspiration: if a future Qt teaches
    // textRole to read a QJsonArray, this is the check that will fail
    // and tell you the fallback below is now dead weight. Measured on
    // Qt 6.10.2.

    function test_textRole_does_not_resolve_against_a_QJsonArray() {
        var cb = make(probe.jsonRoles, "name");
        compare(cb.count, 3, "the count comes through — hence a popup of "
                           + "the right height with nothing in it");
        compare(cb.textAt(0), "", "raw textAt on a QJsonArray model");
        compare(cb.textAt(1), "");
        compare(cb.textAt(2), "");
        compare(cb.displayText, "", "and the closed field with it");
    }

    // ── 2. The bug, through the resolver ─────────────────────────────

    function test_json_array_rows_have_labels() {
        var cb = make(probe.jsonRoles, "name");
        compare(rows(cb), ["admin", "mod", "member"]);
    }

    function test_json_array_closed_field_has_a_label() {
        var cb = make(probe.jsonRoles, "name");
        cb.currentIndex = 0;
        compare(field(cb), "admin", "currentIndex 0, as Role overrides opens");
        cb.currentIndex = 2;
        compare(field(cb), "member", "and after picking a different role");
    }

    // The role objects carry `id` as well as `name`. A fallback that
    // reached for "whatever string is in there" would pass test 2 by
    // luck; this one says the textRole is what is honoured.
    function test_json_array_honours_the_requested_role() {
        var cb = make(probe.jsonRoles, "id");
        compare(rows(cb), ["admin-id", "mod-id", "member-id"]);
    }

    // ── 3. The regression the naive revert would cause ───────────────
    //
    // ThemedComboBox once did `Array.isArray(cb.model) ? ... : ""`, and
    // Array.isArray is FALSE for a QVariantList model, so the audio
    // device combos in ClientSettings.qml went blank — the FIRST "black
    // on black" dropdown. Both shapes have to keep working at once.

    function test_variant_list_rows_still_have_labels() {
        var cb = make(probe.variantRoles, "name");
        compare(rows(cb), ["admin", "mod", "member"]);
        cb.currentIndex = 1;
        compare(field(cb), "mod");
    }

    // ── 4. The shape only Qt's own lookup can read ───────────────────
    //
    // `model[i]` on a QAbstractListModel is undefined, so the fallback
    // cannot serve this one. It is the reason resolve() must try
    // textAt() first rather than indexing first.

    function test_abstract_list_model_rows_still_have_labels() {
        var cb = make(probe.objectRoles, "name");
        compare(cb.count, 3);
        compare(rows(cb), ["admin", "mod", "member"]);
        cb.currentIndex = 2;
        compare(field(cb), "member");
        // Spelt out so the "textAt first" ordering is not something a
        // later edit can reverse without a failure.
        compare(ComboBoxText.fromModel(probe.objectRoles, 0, "name"), "",
                "JS indexing genuinely cannot read this model");
    }

    // ── 5. Plain JS arrays and string lists ──────────────────────────

    function test_js_literal_rows_still_have_labels() {
        var cb = make([{ name: "admin" }, { name: "mod" }, { name: "member" }],
                      "name");
        compare(rows(cb), ["admin", "mod", "member"]);
    }

    function test_string_list_rows_still_have_labels() {
        var cb = make(probe.plainRoles);
        compare(rows(cb), ["admin", "mod", "member"]);
        cb.currentIndex = 1;
        compare(field(cb), "mod");
    }

    // ── 6. Degenerate inputs ─────────────────────────────────────────
    //
    // A blank dropdown is survivable; an exception inside a delegate
    // binding blanks every OTHER row too, so the fallback must return
    // rather than throw.

    function test_empty_and_missing_inputs_resolve_to_empty_string() {
        compare(ComboBoxText.resolve("", null, 0, "name"), "");
        compare(ComboBoxText.resolve("", undefined, 0, "name"), "");
        compare(ComboBoxText.resolve("", [], 0, "name"), "");
        compare(ComboBoxText.resolve("", probe.jsonRoles, -1, "name"), "",
                "currentIndex is -1 on a combo with nothing selected");
        compare(ComboBoxText.resolve("", probe.jsonRoles, 99, "name"), "");
        compare(ComboBoxText.resolve("", probe.jsonRoles, 0, "nosuchrole"), "");
        compare(ComboBoxText.resolve("", probe.jsonRoles, 0, ""), "",
                "no textRole on an object model: show nothing, do not "
              + "invent a label from the first key");
    }

    // Qt's answer wins whenever it has one, so a model that legitimately
    // disagrees with the fallback cannot be silently overridden.
    function test_combo_text_takes_precedence_over_the_fallback() {
        compare(ComboBoxText.resolve("from-qt", probe.jsonRoles, 0, "name"),
                "from-qt");
    }
}
