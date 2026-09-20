.pragma library

// What a ThemedComboBox row — and its closed field — actually says.
//
// THE INCIDENT (v0.0.44, Channel settings > Role overrides):
// the ROLE dropdown opened to a popup of exactly the right HEIGHT with
// six blank rows, and a blank closed field. Reported as "the role field
// list isn't populated or has black text on a black background". It was
// neither: the rows existed and were the right colour, they just had no
// text.
//
// The model there is ServerConnection's
//     Q_PROPERTY(QJsonArray serverRoles ...)
// and THAT is the whole bug. ComboBox resolves both `textAt(i)` and
// `displayText` through QQmlDelegateModel's role lookup, which knows how
// to read a role out of a QVariantMap or a QVariantHash entry and out of
// a QAbstractListModel's roleNames — and does NOT know how to read one
// out of a QJsonArray. Count comes through fine (hence the right popup
// height); every label comes back "".
//
// Measured on Qt 6.10.2, same four shapes the tests pin:
//     QJsonArray   count=3 displayText=""      textAt=["","",""]
//     QVariantList count=3 displayText="admin" textAt=["admin",...]
//     JS literal   count=3 displayText="admin" textAt=["admin",...]
// which is why this was the ONLY combo that showed it: the slowmode
// combo two sections up the same dialog is a JS array literal, the audio
// device combos in ClientSettings.qml are QVariantList, and every other
// reader of serverRoles (RoleAssignPopup.qml, ServerSettings.qml) is a
// Repeater over `modelData.name` and never goes near textRole.
//
// WHY THE OBVIOUS FIX IS WRONG. ThemedComboBox.qml used to do
//     Array.isArray(cb.model) ? cb.model[index][textRole] : ""
// and that produced the FIRST "black on black" dropdown, because
// `Array.isArray` is false for a QVariantList model, so the audio device
// combos took the else branch and rendered empty strings. Moving to
// `textAt()` fixed those and quietly broke this one. Note the trap in
// full: `Array.isArray` is true for exactly the QJsonArray case that
// textAt cannot read, and false for exactly the cases it can. Reverting
// swaps which combos are blank; it does not fix anything.
//
// So neither source is sufficient alone, and the resolution is to try
// both: `textAt`/`displayText` first, because it is the one that
// understands QAbstractListModel roleNames (which no amount of JS
// indexing can reach), then plain indexing for the JS-visible shapes
// that Qt's role lookup does not cover. A QJsonArray property arrives in
// JS as a real array of plain objects, so `model[i][textRole]` reads it.
//
// Pure functions over plain values, for the usual reason in this
// codebase: the BSFChat QML module is compiled into the app binary, so
// no test target can instantiate ThemedComboBox.qml. Living here means
// tests/qml_models/tst_comboboxmodel.qml can drive the real resolver
// against a real QJsonArray Q_PROPERTY instead of a QML literal that
// already worked.

// `comboText` is what ComboBox itself resolved — textAt(index) for a
// popup row, displayText for the closed field. `model` is cb.model as
// QML sees it, `index` the row, `textRole` cb.textRole.
function resolve(comboText, model, index, textRole) {
    // Qt got there first: trust it. This is the only branch that can
    // read a QAbstractListModel role, so it has to stay first.
    if (comboText !== undefined && comboText !== null && comboText !== "")
        return comboText;
    return fromModel(model, index, textRole);
}

// The fallback: index the model from JS. Covers QJsonArray (arrives as a
// JS array of plain objects) and any JS-native array; returns "" rather
// than throwing for the shapes it cannot index, which is correct because
// resolve() only reaches here when Qt already gave up.
function fromModel(model, index, textRole) {
    if (model === undefined || model === null) return "";
    if (typeof index !== "number" || index < 0) return "";

    var entry;
    // A QAbstractListModel is a QObject; `model[3]` on one is undefined,
    // not an error. Guarding anyway — model is whatever a call site
    // assigned, and a throw here would blank the whole dropdown.
    try { entry = model[index]; } catch (e) { return ""; }
    if (entry === undefined || entry === null) return "";

    if (typeof entry === "object") {
        // No textRole on an object model means ComboBox would have shown
        // nothing either; don't invent a label out of the first key.
        if (!textRole) return "";
        var v = entry[textRole];
        return (v === undefined || v === null) ? "" : String(v);
    }
    // Plain string / number list. textRole is meaningless on these and
    // Qt already handles them, so this is belt and braces.
    return String(entry);
}
