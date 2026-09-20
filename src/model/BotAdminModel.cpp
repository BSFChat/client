#include "model/BotAdminModel.h"

#include <bsfchat/Constants.h>

#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QVariantMap>

BotAdminModel::BotAdminModel(QObject* parent)
    : QObject(parent)
{
}

void BotAdminModel::setBusy(bool busy)
{
    if (m_busy == busy) return;
    m_busy = busy;
    emit busyChanged();
}

void BotAdminModel::setErrorText(const QString& text)
{
    if (m_errorText == text) return;
    m_errorText = text;
    emit errorTextChanged();
}

QString BotAdminModel::localpartPrefix()
{
    return QString::fromUtf8(bsfchat::bot::kLocalpartPrefix.data(),
                             qsizetype(bsfchat::bot::kLocalpartPrefix.size()));
}

int BotAdminModel::maxLocalpartLength()
{
    return int(bsfchat::limits::kMaxUsernameLength);
}

QString BotAdminModel::localpartError(const QString& localpart)
{
    const QString prefix = localpartPrefix();

    if (localpart.isEmpty())
        return QStringLiteral("Pick a username for the bot.");

    // The bot's user id is @<localpart>:<server>, so a colon here would
    // produce a second one and an id that parses as a different server. The
    // check is spelled out rather than folded into the regex below because
    // it is the one mistake with a genuinely confusing failure mode — the
    // server's 400 would be about the id, not about what was typed.
    if (localpart.contains(':'))
        return QStringLiteral("No colons — the server part is added for you.");

    // The prefix, first and by name. It is the rule an operator is least
    // likely to guess and the one the server states first, so saying it
    // before anything about characters or length matches the order the
    // server's own sentence puts them in.
    if (!localpart.startsWith(prefix)) {
        return QStringLiteral("Bot usernames must start with \"%1\" — "
                              "that prefix is what marks the account as a bot, "
                              "and no person can be given it.")
            .arg(prefix);
    }
    if (localpart.size() <= prefix.size()) {
        return QStringLiteral("Add something after \"%1\" — the prefix on "
                              "its own is not a name.").arg(prefix);
    }
    if (localpart.size() > maxLocalpartLength()) {
        return QStringLiteral("At most %1 characters, including the \"%2\" "
                              "prefix.").arg(maxLocalpartLength()).arg(prefix);
    }

    // BotHandler::valid_bot_localpart's character set, exactly. NOT a wider
    // one: a set this end accepts and the server refuses is a Create button
    // that lights up for a name that cannot be created.
    static const QRegularExpression allowed(
        QStringLiteral("^[a-z0-9._\\-]+$"));
    if (!allowed.match(localpart).hasMatch()) {
        return QStringLiteral(
            "Use lowercase letters, digits, and . _ - only.");
    }
    return QString();
}

void BotAdminModel::refresh()
{
    // A refresh is allowed to overlap nothing, but it is deliberately NOT
    // blocked on `busy`: the dialog calls this on open, and an open that
    // arrives while a create from a previous open is still settling should
    // still populate the list rather than show an empty pane forever.
    setErrorText(QString());
    if (!hooks.listBots) return;
    setBusy(true);
    hooks.listBots();
}

void BotAdminModel::createBot(const QString& localpart,
                              const QString& displayName,
                              const QString& description)
{
    const QString localpartProblem = localpartError(localpart);
    if (!localpartProblem.isEmpty()) {
        setErrorText(localpartProblem);
        return;
    }
    if (!hooks.createBot) return;
    setErrorText(QString());
    setBusy(true);
    hooks.createBot(localpart, displayName, description);
}

void BotAdminModel::rotateToken(const QString& userId)
{
    if (userId.isEmpty() || !hooks.rotateToken) return;
    setErrorText(QString());
    setBusy(true);
    hooks.rotateToken(userId);
}

void BotAdminModel::deactivateBot(const QString& userId)
{
    if (userId.isEmpty() || !hooks.deactivateBot) return;
    setErrorText(QString());
    setBusy(true);
    hooks.deactivateBot(userId);
}

void BotAdminModel::onBotsListed(const QJsonArray& bots)
{
    m_bots.clear();
    m_bots.reserve(bots.size());
    for (const QJsonValue& entry : bots) {
        const QJsonObject o = entry.toObject();
        QVariantMap row;
        row[QStringLiteral("userId")] = o.value(QStringLiteral("user_id")).toString();
        row[QStringLiteral("displayName")] =
            o.value(QStringLiteral("display_name")).toString();
        row[QStringLiteral("description")] =
            o.value(QStringLiteral("description")).toString();
        row[QStringLiteral("ownerId")] = o.value(QStringLiteral("owner_id")).toString();
        // created_at / last_seen_at are server timestamps. They are carried
        // through as-is rather than formatted here: the dialog renders them
        // with the locale's date format, and a view-model that pre-formatted
        // them could not be checked against an expected value in a test that
        // runs in whatever locale CI happens to have.
        row[QStringLiteral("createdAt")] =
            o.value(QStringLiteral("created_at")).toVariant();
        row[QStringLiteral("lastSeenAt")] =
            o.value(QStringLiteral("last_seen_at")).toVariant();
        row[QStringLiteral("deactivated")] =
            o.value(QStringLiteral("deactivated")).toBool(false);
        m_bots.append(row);
    }

    setBusy(false);
    if (!m_loaded) {
        m_loaded = true;
        emit loadedChanged();
    }
    emit botsChanged();
    // Deliberately AFTER botsChanged: ServerConnection reads listedBotUserIds()
    // from this signal and must see the rows that just landed.
    emit botSetChanged();
}

void BotAdminModel::presentToken(const QString& userId, const QString& token,
                                 bool rotation)
{
    m_issuedToken = token;
    m_issuedTokenUserId = userId;
    m_issuedTokenIsRotation = rotation;
    emit issuedTokenChanged();
    emit tokenIssued(userId, rotation);
}

void BotAdminModel::onBotCreated(const QString& userId, const QString& displayName,
                                 const QString& token)
{
    setBusy(false);
    setErrorText(QString());
    presentToken(userId, token, /*rotation=*/false);

    // Insert the new bot optimistically so the list is not empty behind the
    // banner while the refresh flies. The server's reply is authoritative and
    // will replace this row wholesale; the fields it owns and we cannot know
    // (owner_id, created_at) are left out rather than guessed, and the dialog
    // renders a missing timestamp as "—".
    QVariantMap row;
    row[QStringLiteral("userId")] = userId;
    row[QStringLiteral("displayName")] = displayName;
    row[QStringLiteral("deactivated")] = false;
    m_bots.append(row);
    emit botsChanged();
    emit botSetChanged();

    // Re-list to pick up the authoritative row. Note this cannot disturb the
    // banner: only dismissToken() clears it.
    if (hooks.listBots) {
        setBusy(true);
        hooks.listBots();
    }
}

void BotAdminModel::onTokenRotated(const QString& userId, const QString& token)
{
    setBusy(false);
    setErrorText(QString());
    presentToken(userId, token, /*rotation=*/true);
    // No list change: rotation does not alter anything the list shows. Not
    // re-listing also keeps a slow list reply from racing the banner into
    // view on a dialog the operator is already reading.
}

void BotAdminModel::onBotDeactivated(const QString& userId)
{
    setBusy(false);
    setErrorText(QString());

    // Mark in place rather than remove. The server's DELETE is a deactivation
    // and is idempotent — the bot keeps its id, its messages and its badge,
    // and stays listed so an admin can see that it exists and is off. A row
    // that vanished would read as "deleted", which is a promise the server
    // does not make.
    for (QVariant& entry : m_bots) {
        QVariantMap row = entry.toMap();
        if (row.value(QStringLiteral("userId")).toString() != userId) continue;
        row[QStringLiteral("deactivated")] = true;
        entry = row;
    }
    emit botsChanged();

    if (hooks.listBots) {
        setBusy(true);
        hooks.listBots();
    }
}

void BotAdminModel::onFailed(const QString& operation, const QString& message)
{
    setBusy(false);
    // `message` is the server's error text. It is shown, not logged: the
    // failure body for a create is the one response on this path that the
    // server could conceivably widen, and nothing here is worth risking a
    // token reaching the log file over.
    QString prefix;
    if (operation == QLatin1String("list"))            prefix = QStringLiteral("Couldn't load bots");
    else if (operation == QLatin1String("create"))     prefix = QStringLiteral("Couldn't create the bot");
    else if (operation == QLatin1String("rotate"))     prefix = QStringLiteral("Couldn't rotate the token");
    else if (operation == QLatin1String("deactivate")) prefix = QStringLiteral("Couldn't deactivate the bot");
    else                                               prefix = QStringLiteral("Bot request failed");

    setErrorText(message.isEmpty() ? prefix : prefix + QStringLiteral(": ") + message);

    // A failed list still counts as "we asked" — otherwise the dialog shows a
    // spinner next to an error message, which reads as though it were still
    // trying.
    if (operation == QLatin1String("list") && !m_loaded) {
        m_loaded = true;
        emit loadedChanged();
    }
}

void BotAdminModel::dismissToken()
{
    if (m_issuedToken.isEmpty()) return;
    // Overwrite before releasing. This is best-effort and nothing more: if
    // QML or a signal handler copied the string, QString is implicitly shared
    // and fill() detaches instead of scrubbing the copy they hold. It costs
    // one pass over a short string and removes the case that is actually
    // likely — this object's own buffer being handed back to the allocator
    // with a live token still in it.
    m_issuedToken.fill(QChar(u'\0'));
    m_issuedToken.clear();
    m_issuedTokenUserId.clear();
    m_issuedTokenIsRotation = false;
    emit issuedTokenChanged();
}

QString BotAdminModel::displayNameFor(const QString& userId) const
{
    for (const QVariant& entry : m_bots) {
        const QVariantMap row = entry.toMap();
        if (row.value(QStringLiteral("userId")).toString() != userId) continue;
        const QString name = row.value(QStringLiteral("displayName")).toString();
        return name.isEmpty() ? userId : name;
    }
    return userId;
}

QStringList BotAdminModel::listedBotUserIds() const
{
    QStringList out;
    out.reserve(m_bots.size());
    for (const QVariant& entry : m_bots) {
        const QString id = entry.toMap().value(QStringLiteral("userId")).toString();
        if (!id.isEmpty()) out.append(id);
    }
    return out;
}

void BotAdminModel::reset()
{
    dismissToken();
    m_bots.clear();
    setErrorText(QString());
    setBusy(false);
    if (m_loaded) {
        m_loaded = false;
        emit loadedChanged();
    }
    emit botsChanged();
}
