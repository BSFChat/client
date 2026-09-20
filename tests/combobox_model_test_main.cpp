// Runner for tests/qml_models/ — QML tests that need a REAL C++-backed
// model, not a QML literal.
//
// Separate from the tests/qml/ runner (test_playback_math) on purpose.
// That one is deliberately engine-only: pure functions over plain
// objects, nothing registered, nothing to configure. These tests are the
// opposite — the whole point is the C++ side of the boundary, because
// the defect they exist for lives exactly there.
//
// The defect: Channel settings > Role overrides opened a dropdown of the
// right height with six blank rows, because its model is
//     ServerConnection::Q_PROPERTY(QJsonArray serverRoles ...)
// and ComboBox's textRole lookup silently yields "" for every row of a
// QJsonArray. A test written against a QML array literal — the shape the
// slowmode combo in the same dialog uses — passes happily and proves
// nothing, in the same way test_self_roles'
// everyEnforcedPermissionHasASwitchInTheRoleEditor compared the client's
// mask against the client's own kAllFlags and made drift invisible. So
// `ComboModelProbe` below declares its properties with the SAME
// metatypes the app uses, and the QML side drives real ComboBox
// instances against them.
#include <QtQuickTest>
#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QQmlEngine>
#include <QQmlContext>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace {

// A QAbstractListModel with roleNames — the one shape that ONLY
// ComboBox's own textRole lookup can read (JS indexing cannot), which is
// why the resolver must keep trying textAt() first.
class RoleNamedModel : public QAbstractListModel
{
    Q_OBJECT
public:
    explicit RoleNamedModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex& parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : int(m_names.size());
    }
    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid() || index.row() >= m_names.size()) return {};
        if (role == Qt::UserRole) return m_names.at(index.row());
        return {};
    }
    QHash<int, QByteArray> roleNames() const override
    {
        return {{Qt::UserRole, QByteArrayLiteral("name")}};
    }

private:
    QStringList m_names{QStringLiteral("admin"), QStringLiteral("mod"),
                        QStringLiteral("member")};
};

// The four model shapes a ThemedComboBox can be handed, each declared
// with the metatype the real code uses:
//
//   jsonRoles    — ServerConnection::serverRoles  (the one that broke)
//   variantRoles — Settings::audioInputDevices    (works today)
//   objectRoles  — QAbstractListModel with roleNames
//   plainRoles   — a bare string list, no textRole
class ComboModelProbe : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QJsonArray jsonRoles READ jsonRoles CONSTANT)
    Q_PROPERTY(QVariantList variantRoles READ variantRoles CONSTANT)
    Q_PROPERTY(QObject* objectRoles READ objectRoles CONSTANT)
    Q_PROPERTY(QStringList plainRoles READ plainRoles CONSTANT)
public:
    explicit ComboModelProbe(QObject* parent = nullptr)
        : QObject(parent), m_objectRoles(new RoleNamedModel(this)) {}

    // Shaped like the real role documents: an `id` alongside `name`, so
    // a fallback that grabbed "the first string in the object" would be
    // caught rather than flattered.
    QJsonArray jsonRoles() const
    {
        QJsonArray a;
        for (const QString& n : names()) {
            QJsonObject o;
            o[QStringLiteral("id")] = n + QStringLiteral("-id");
            o[QStringLiteral("name")] = n;
            a.append(o);
        }
        return a;
    }
    QVariantList variantRoles() const
    {
        QVariantList l;
        for (const QString& n : names()) {
            QVariantMap m;
            m[QStringLiteral("id")] = n + QStringLiteral("-id");
            m[QStringLiteral("name")] = n;
            l.append(m);
        }
        return l;
    }
    QObject* objectRoles() const { return m_objectRoles; }
    QStringList plainRoles() const { return names(); }

private:
    static QStringList names()
    {
        return {QStringLiteral("admin"), QStringLiteral("mod"),
                QStringLiteral("member")};
    }
    RoleNamedModel* m_objectRoles;
};

class Setup : public QObject
{
    Q_OBJECT
public:
    Setup() = default;

public slots:
    void qmlEngineAvailable(QQmlEngine* engine)
    {
        engine->rootContext()->setContextProperty(
            QStringLiteral("probe"), new ComboModelProbe(engine));
    }
};

} // namespace

QUICK_TEST_MAIN_WITH_SETUP(combobox_model, Setup)

#include "combobox_model_test_main.moc"
