#pragma once

#include <QString>
#include <QStringList>

#include <initializer_list>
#include <string>

#include <nlohmann/json.hpp>

namespace bsfchat::client {

// Reads the named string fields out of a reply body that is supposed to be a
// JSON object of strings, without ever throwing.
//
// It exists because the profile endpoints used to be read with
// `json::parse(...).value(key, "")` inside a `try { } catch (...) {}`. Two
// separate things went wrong with that:
//
//   * `value()` throws type_error.306 when the body is a bare `null` — unlike
//     non-const `operator[]`, which quietly turns the null into an object. A
//     server answering `null` for an account that has set neither a display
//     name nor a picture (the default state of every fresh registration) put
//     every profile fetch for such a user straight into the catch;
//   * the catch was empty, so nothing was emitted and nothing was logged. The
//     caller waited forever for a signal that would never arrive, and there
//     was no trace of it in the field.
//
// So the tolerance and the reporting are split. Unusable input yields an empty
// string for that field — the same value the callers already use for "this
// member has no display name" — and `warning` carries a one-line description
// for the log, so a caller can both make progress and say what went wrong.
//
// `warning` is set for a server that answers `null` too, not just for
// genuinely broken JSON. That is deliberately noisy: against a homeserver
// predating the fix it logs once per bare account in a member list. The
// alternative is treating a protocol violation as normal, which is exactly how
// this stayed hidden.
struct StringFields {
    // One entry per requested key, in order. Empty when the key is absent,
    // null, or not a string.
    QStringList values;

    // Empty when the body was understood completely.
    QString warning;
};

inline StringFields readStringFields(const std::string& body,
                                     std::initializer_list<const char*> keys)
{
    StringFields out;
    out.values.reserve(static_cast<qsizetype>(keys.size()));
    for (std::size_t i = 0; i < keys.size(); ++i)
        out.values.append(QString());

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(body);
    } catch (const nlohmann::json::parse_error& e) {
        out.warning = QStringLiteral("malformed JSON: ")
                      + QString::fromUtf8(e.what());
        return out;
    }

    if (!j.is_object()) {
        out.warning = QStringLiteral("expected a JSON object, got ")
                      + QString::fromUtf8(j.type_name());
        return out;
    }

    QStringList wrongType;
    qsizetype slot = 0;
    for (const char* key : keys) {
        const auto it = j.find(key);
        if (it != j.end() && !it->is_null()) {
            if (it->is_string())
                out.values[slot] = QString::fromStdString(it->get<std::string>());
            else
                wrongType.append(QString::fromUtf8(key));
        }
        ++slot;
    }

    if (!wrongType.isEmpty()) {
        out.warning = QStringLiteral("non-string value for ")
                      + wrongType.join(QStringLiteral(", "));
    }
    return out;
}

} // namespace bsfchat::client
