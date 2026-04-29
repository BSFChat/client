#include "net/MatrixClient.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QUrlQuery>
#include <QUuid>

#include <bsfchat/Constants.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

MatrixClient::MatrixClient(QObject* parent)
    : QObject(parent)
{
}

void MatrixClient::setHomeserver(const QString& url)
{
    m_homeserver = url;
    // Strip trailing slash
    while (m_homeserver.endsWith('/'))
        m_homeserver.chop(1);
}

void MatrixClient::setAccessToken(const QString& token)
{
    m_accessToken = token;
}

QUrl MatrixClient::buildUrl(const QString& path) const
{
    return QUrl(m_homeserver + path);
}

QNetworkReply* MatrixClient::makeRequest(const QString& method, const QString& path,
                                          const QByteArray& body)
{
    QNetworkRequest request(buildUrl(path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    if (!m_accessToken.isEmpty()) {
        request.setRawHeader("Authorization", ("Bearer " + m_accessToken).toUtf8());
    }

    QNetworkReply* reply = nullptr;
    if (method == "GET") {
        reply = m_nam.get(request);
    } else if (method == "POST") {
        reply = m_nam.post(request, body);
    } else if (method == "PUT") {
        reply = m_nam.put(request, body);
    } else if (method == "DELETE") {
        reply = m_nam.deleteResource(request);
    }
    return reply;
}

void MatrixClient::login(const QString& username, const QString& password)
{
    bsfchat::LoginRequest req;
    req.type = "m.login.password";
    req.identifier.type = "m.id.user";
    req.identifier.user = username.toStdString();
    req.password = password.toStdString();
    req.initial_device_display_name = "BSFChat Desktop";

    json j;
    bsfchat::to_json(j, req);
    QByteArray body = QByteArray::fromStdString(j.dump());

    auto* reply = makeRequest("POST", QString::fromUtf8(bsfchat::api_path::kLogin), body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit loginError(QString::fromUtf8(data));
            return;
        }
        try {
            auto j = json::parse(data.toStdString());
            bsfchat::LoginResponse resp;
            bsfchat::from_json(j, resp);
            emit loginSuccess(resp);
        } catch (const std::exception& e) {
            emit loginError(QString::fromStdString(e.what()));
        }
    });
}

void MatrixClient::getLoginFlows()
{
    auto* reply = makeRequest("GET", QString::fromUtf8(bsfchat::api_path::kLogin));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit loginError(QString::fromUtf8(data));
            return;
        }
        auto doc = QJsonDocument::fromJson(data);
        QJsonArray flows = doc.object().value("flows").toArray();
        emit loginFlowsResult(flows);
    });
}

void MatrixClient::loginWithToken(const QString& idToken)
{
    QJsonObject body;
    body["type"] = QString("m.login.token");
    body["token"] = idToken;
    QJsonObject identifier;
    identifier["type"] = QString("m.id.user");
    body["identifier"] = identifier;
    body["initial_device_display_name"] = QString("BSFChat Desktop");

    QByteArray bodyData = QJsonDocument(body).toJson(QJsonDocument::Compact);

    auto* reply = makeRequest("POST", QString::fromUtf8(bsfchat::api_path::kLogin), bodyData);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit loginError(QString::fromUtf8(data));
            return;
        }
        try {
            auto j = json::parse(data.toStdString());
            bsfchat::LoginResponse resp;
            bsfchat::from_json(j, resp);
            emit loginSuccess(resp);
        } catch (const std::exception& e) {
            emit loginError(QString::fromStdString(e.what()));
        }
    });
}

void MatrixClient::registerUser(const QString& username, const QString& password)
{
    bsfchat::RegisterRequest req;
    req.username = username.toStdString();
    req.password = password.toStdString();
    req.initial_device_display_name = "BSFChat Desktop";

    json j;
    bsfchat::to_json(j, req);
    QByteArray body = QByteArray::fromStdString(j.dump());

    auto* reply = makeRequest("POST", QString::fromUtf8(bsfchat::api_path::kRegister), body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit registerError(QString::fromUtf8(data));
            return;
        }
        try {
            auto j = json::parse(data.toStdString());
            bsfchat::LoginResponse resp;
            bsfchat::from_json(j, resp);
            emit registerSuccess(resp);
        } catch (const std::exception& e) {
            emit registerError(QString::fromStdString(e.what()));
        }
    });
}

void MatrixClient::sync(const QString& since, int timeout)
{
    QString path = QString::fromUtf8(bsfchat::api_path::kSync);
    QUrlQuery query;
    query.addQueryItem("timeout", QString::number(timeout));
    if (!since.isEmpty()) {
        query.addQueryItem("since", since);
    }

    QUrl url = buildUrl(path);
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!m_accessToken.isEmpty()) {
        request.setRawHeader("Authorization", ("Bearer " + m_accessToken).toUtf8());
    }
    // Long-poll timeout: give extra 30s for network
    request.setTransferTimeout((timeout + 30000));

    auto* reply = m_nam.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit syncError(QString::fromUtf8(data));
            return;
        }
        try {
            auto j = json::parse(data.toStdString());
            bsfchat::SyncResponse resp;
            bsfchat::from_json(j, resp);
            emit syncSuccess(resp);
        } catch (const std::exception& e) {
            emit syncError(QString::fromStdString(e.what()));
        }
    });
}

void MatrixClient::createRoom(const QString& name, const QString& topic, const QString& visibility)
{
    bsfchat::CreateRoomRequest req;
    if (!name.isEmpty()) req.name = name.toStdString();
    if (!topic.isEmpty()) req.topic = topic.toStdString();
    req.visibility = visibility.toStdString();

    json j;
    bsfchat::to_json(j, req);
    QByteArray body = QByteArray::fromStdString(j.dump());

    auto* reply = makeRequest("POST", QString::fromUtf8(bsfchat::api_path::kCreateRoom), body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit createRoomError(QString::fromUtf8(data));
            return;
        }
        try {
            auto j = json::parse(data.toStdString());
            bsfchat::CreateRoomResponse resp;
            bsfchat::from_json(j, resp);
            emit createRoomSuccess(QString::fromStdString(resp.room_id));
        } catch (const std::exception& e) {
            emit createRoomError(QString::fromStdString(e.what()));
        }
    });
}

void MatrixClient::createDirectMessageRoom(const QString& targetUserId)
{
    bsfchat::CreateRoomRequest req;
    req.visibility = "private";
    req.preset = "trusted_private_chat";
    req.is_direct = true;
    req.invite.push_back(targetUserId.toStdString());

    json j;
    bsfchat::to_json(j, req);
    QByteArray body = QByteArray::fromStdString(j.dump());

    auto* reply = makeRequest("POST", QString::fromUtf8(bsfchat::api_path::kCreateRoom), body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit createRoomError(QString::fromUtf8(data));
            return;
        }
        try {
            auto j = json::parse(data.toStdString());
            bsfchat::CreateRoomResponse resp;
            bsfchat::from_json(j, resp);
            emit createRoomSuccess(QString::fromStdString(resp.room_id));
        } catch (const std::exception& e) {
            emit createRoomError(QString::fromStdString(e.what()));
        }
    });
}

void MatrixClient::joinRoom(const QString& roomIdOrAlias)
{
    QString path = QString::fromUtf8(bsfchat::api_path::kJoinByAlias)
                   + QUrl::toPercentEncoding(roomIdOrAlias);

    auto* reply = makeRequest("POST", path, "{}");
    connect(reply, &QNetworkReply::finished, this, [this, reply, roomIdOrAlias]() {
        reply->deleteLater();
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit joinRoomError(QString::fromUtf8(data));
            return;
        }
        try {
            auto j = json::parse(data.toStdString());
            QString roomId = QString::fromStdString(j.value("room_id", roomIdOrAlias.toStdString()));
            emit joinRoomSuccess(roomId);
        } catch (const std::exception& e) {
            emit joinRoomError(QString::fromStdString(e.what()));
        }
    });
}

void MatrixClient::deleteRoom(const QString& roomId)
{
    QString path = QString::fromUtf8(bsfchat::api_path::kRoomPrefix)
                   + QUrl::toPercentEncoding(roomId);
    auto* reply = makeRequest("DELETE", path);
    connect(reply, &QNetworkReply::finished, this, [reply]() { reply->deleteLater(); });
}

void MatrixClient::leaveRoom(const QString& roomId)
{
    QString path = QString::fromUtf8(bsfchat::api_path::kRoomPrefix)
                   + QUrl::toPercentEncoding(roomId) + "/leave";

    auto* reply = makeRequest("POST", path, "{}");
    connect(reply, &QNetworkReply::finished, this, [this, reply, roomId]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit leaveRoomError(QString::fromUtf8(reply->readAll()));
            return;
        }
        emit leaveRoomSuccess(roomId);
    });
}

void MatrixClient::getRoomMembers(const QString& roomId)
{
    QString path = QString::fromUtf8(bsfchat::api_path::kRoomPrefix)
                   + QUrl::toPercentEncoding(roomId) + "/members";
    auto* reply = makeRequest("GET", path);
    connect(reply, &QNetworkReply::finished, this, [this, reply, roomId]() {
        reply->deleteLater();
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) return;
        try {
            auto doc = QJsonDocument::fromJson(data);
            auto chunk = doc.object().value("chunk").toArray();
            emit roomMembersResult(roomId, chunk);
        } catch (...) {}
    });
}

void MatrixClient::getJoinedRooms()
{
    auto* reply = makeRequest("GET", QString::fromUtf8(bsfchat::api_path::kJoinedRooms));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            return;
        }
        try {
            auto j = json::parse(data.toStdString());
            QStringList rooms;
            for (const auto& rid : j["joined_rooms"]) {
                rooms.append(QString::fromStdString(rid.get<std::string>()));
            }
            emit joinedRoomsResult(rooms);
        } catch (...) {}
    });
}

void MatrixClient::sendMessage(const QString& roomId, const QString& body)
{
    // Generate a transaction ID
    static int txnCounter = 0;
    QString txnId = QString("m%1.%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(++txnCounter);

    QString path = QString::fromUtf8(bsfchat::api_path::kRoomPrefix)
                   + QUrl::toPercentEncoding(roomId)
                   + "/send/m.room.message/" + txnId;

    json content;
    content["msgtype"] = "m.text";
    content["body"] = body.toStdString();

    QByteArray reqBody = QByteArray::fromStdString(content.dump());

    auto* reply = makeRequest("PUT", path, reqBody);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit sendMessageError(QString::fromUtf8(data));
            return;
        }
        try {
            auto j = json::parse(data.toStdString());
            emit messageSent(QString::fromStdString(j.value("event_id", "")));
        } catch (const std::exception& e) {
            emit sendMessageError(QString::fromStdString(e.what()));
        }
    });
}

void MatrixClient::sendRichMessage(const QString& roomId, const QString& body,
                                    const QString& formattedBody,
                                    const QStringList& mentionedUserIds)
{
    static int txnCounter = 0;
    QString txnId = QString("m%1.%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(++txnCounter);

    QString path = QString::fromUtf8(bsfchat::api_path::kRoomPrefix)
                   + QUrl::toPercentEncoding(roomId)
                   + "/send/m.room.message/" + txnId;

    json content;
    content["msgtype"] = "m.text";
    content["body"] = body.toStdString();
    if (!formattedBody.isEmpty()) {
        content["format"] = "org.matrix.custom.html";
        content["formatted_body"] = formattedBody.toStdString();
    }
    if (!mentionedUserIds.isEmpty()) {
        // m.mentions is the modern Matrix mention signal (MSC3952).
        // Servers implementing it elevate push notifications for listed
        // users regardless of the recipient's notification settings.
        json users = json::array();
        for (const auto& u : mentionedUserIds) users.push_back(u.toStdString());
        content["m.mentions"] = { {"user_ids", users} };
    }

    QByteArray reqBody = QByteArray::fromStdString(content.dump());
    auto* reply = makeRequest("PUT", path, reqBody);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit sendMessageError(QString::fromUtf8(data));
            return;
        }
        try {
            auto j = json::parse(data.toStdString());
            emit messageSent(QString::fromStdString(j.value("event_id", "")));
        } catch (const std::exception& e) {
            emit sendMessageError(QString::fromStdString(e.what()));
        }
    });
}

void MatrixClient::editMessage(const QString& roomId, const QString& targetEventId, const QString& newBody)
{
    static int txnCounter = 0;
    QString txnId = QString("e%1.%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(++txnCounter);

    QString path = QString::fromUtf8(bsfchat::api_path::kRoomPrefix)
                   + QUrl::toPercentEncoding(roomId)
                   + "/send/m.room.message/" + txnId;

    // Matrix edits: the top-level body/msgtype is a fallback for clients
    // that don't understand m.replace relations (prefixed with "* " by
    // convention). The authoritative replacement lives under m.new_content.
    json newContent;
    newContent["msgtype"] = "m.text";
    newContent["body"] = newBody.toStdString();

    json content;
    content["msgtype"] = "m.text";
    content["body"] = ("* " + newBody).toStdString();
    content["m.new_content"] = newContent;
    content["m.relates_to"] = {
        {"rel_type", "m.replace"},
        {"event_id", targetEventId.toStdString()},
    };

    QByteArray reqBody = QByteArray::fromStdString(content.dump());
    auto* reply = makeRequest("PUT", path, reqBody);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit sendMessageError(QString::fromUtf8(data));
            return;
        }
        try {
            auto j = json::parse(data.toStdString());
            emit messageSent(QString::fromStdString(j.value("event_id", "")));
        } catch (const std::exception& e) {
            emit sendMessageError(QString::fromStdString(e.what()));
        }
    });
}

void MatrixClient::replyToMessage(const QString& roomId, const QString& body,
                                    const QString& targetEventId)
{
    static int txnCounter = 0;
    QString txnId = QString("r%1.%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(++txnCounter);

    QString path = QString::fromUtf8(bsfchat::api_path::kRoomPrefix)
                   + QUrl::toPercentEncoding(roomId)
                   + "/send/m.room.message/" + txnId;

    // Replies are just ordinary m.room.message events carrying a relation
    // pointer. No m.replace, no m.new_content — those are for edits.
    json content;
    content["msgtype"] = "m.text";
    content["body"] = body.toStdString();
    content["m.relates_to"] = {
        {"m.in_reply_to", {{"event_id", targetEventId.toStdString()}}},
    };

    QByteArray reqBody = QByteArray::fromStdString(content.dump());
    auto* reply = makeRequest("PUT", path, reqBody);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit sendMessageError(QString::fromUtf8(data));
            return;
        }
        try {
            auto j = json::parse(data.toStdString());
            emit messageSent(QString::fromStdString(j.value("event_id", "")));
        } catch (const std::exception& e) {
            emit sendMessageError(QString::fromStdString(e.what()));
        }
    });
}

void MatrixClient::sendEmote(const QString& roomId, const QString& body)
{
    static int txnCounter = 0;
    QString txnId = QString("e%1.%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(++txnCounter);
    QString path = QString::fromUtf8(bsfchat::api_path::kRoomPrefix)
                   + QUrl::toPercentEncoding(roomId)
                   + "/send/m.room.message/" + txnId;
    json content;
    content["msgtype"] = "m.emote";
    content["body"] = body.toStdString();
    QByteArray reqBody = QByteArray::fromStdString(content.dump());
    auto* reply = makeRequest("PUT", path, reqBody);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit sendMessageError(QString::fromUtf8(data));
            return;
        }
        try {
            auto j = json::parse(data.toStdString());
            emit messageSent(QString::fromStdString(j.value("event_id", "")));
        } catch (const std::exception& e) {
            emit sendMessageError(QString::fromStdString(e.what()));
        }
    });
}

void MatrixClient::sendThreadReply(const QString& roomId, const QString& body,
                                    const QString& threadRootId)
{
    static int txnCounter = 0;
    QString txnId = QString("t%1.%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(++txnCounter);

    QString path = QString::fromUtf8(bsfchat::api_path::kRoomPrefix)
                   + QUrl::toPercentEncoding(roomId)
                   + "/send/m.room.message/" + txnId;

    json content;
    content["msgtype"] = "m.text";
    content["body"] = body.toStdString();
    content["m.relates_to"] = {
        {"rel_type", "m.thread"},
        {"event_id", threadRootId.toStdString()}
    };

    QByteArray reqBody = QByteArray::fromStdString(content.dump());
    auto* reply = makeRequest("PUT", path, reqBody);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit sendMessageError(QString::fromUtf8(data));
            return;
        }
        try {
            auto j = json::parse(data.toStdString());
            emit messageSent(QString::fromStdString(j.value("event_id", "")));
        } catch (const std::exception& e) {
            emit sendMessageError(QString::fromStdString(e.what()));
        }
    });
}

void MatrixClient::forwardMessage(const QString& destRoomId, const QString& body,
                                    const QString& sourceChannelName,
                                    const QString& sourceSenderName,
                                    const QString& sourceServerUrl,
                                    const QString& sourceRoomId,
                                    const QString& sourceEventId)
{
    static int txnCounter = 0;
    QString txnId = QString("f%1.%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(++txnCounter);

    QString path = QString::fromUtf8(bsfchat::api_path::kRoomPrefix)
                   + QUrl::toPercentEncoding(destRoomId)
                   + "/send/m.room.message/" + txnId;

    // Build a Markdown-friendly forwarded body. The attribution header
    // lives on its own quoted line; the original body gets a blockquote
    // prefix on each line so the client's Markdown renderer produces a
    // visually distinct quote block.
    QString channelLabel = sourceChannelName.isEmpty() ? QStringLiteral("unknown")
                                                        : sourceChannelName;
    QString senderLabel = sourceSenderName.isEmpty() ? QStringLiteral("unknown")
                                                      : sourceSenderName;
    // If we have source coordinates, wrap the "#channel" label in a
    // Markdown link pointing at the original message. The client's markdown
    // renderer turns this into a clickable anchor; MessageBubble intercepts
    // bsfchat://message/... clicks and navigates.
    QString channelToken;
    if (!sourceServerUrl.isEmpty() && !sourceRoomId.isEmpty()
        && !sourceEventId.isEmpty()) {
        const QByteArray serverEnc = QUrl::toPercentEncoding(sourceServerUrl, "", "/");
        const QByteArray roomEnc = QUrl::toPercentEncoding(sourceRoomId);
        const QByteArray eventEnc = QUrl::toPercentEncoding(sourceEventId);
        channelToken = QStringLiteral("[#%1](bsfchat://message/%2/%3/%4)")
                           .arg(channelLabel,
                                QString::fromUtf8(serverEnc),
                                QString::fromUtf8(roomEnc),
                                QString::fromUtf8(eventEnc));
    } else {
        channelToken = QStringLiteral("#%1").arg(channelLabel);
    }
    QString header = QStringLiteral("> **Forwarded from %1 by @%2**")
                         .arg(channelToken, senderLabel);
    QString quoted;
    const QStringList lines = body.split('\n');
    for (const auto& line : lines) {
        quoted += QStringLiteral("> ") + line + QStringLiteral("\n");
    }
    if (quoted.endsWith('\n')) quoted.chop(1);
    QString full = header + QStringLiteral("\n") + quoted;

    json content;
    content["msgtype"] = "m.text";
    content["body"] = full.toStdString();

    QByteArray reqBody = QByteArray::fromStdString(content.dump());
    auto* reply = makeRequest("PUT", path, reqBody);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit sendMessageError(QString::fromUtf8(data));
            return;
        }
        try {
            auto j = json::parse(data.toStdString());
            emit messageSent(QString::fromStdString(j.value("event_id", "")));
        } catch (const std::exception& e) {
            emit sendMessageError(QString::fromStdString(e.what()));
        }
    });
}

void MatrixClient::sendRoomEvent(const QString& roomId, const QString& eventType, const QByteArray& content)
{
    static int txnCounter = 0;
    QString txnId = QString("m%1.%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(++txnCounter);

    QString path = QString::fromUtf8(bsfchat::api_path::kRoomPrefix)
                   + QUrl::toPercentEncoding(roomId)
                   + "/send/" + eventType + "/" + txnId;

    auto* reply = makeRequest("PUT", path, content);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit sendMessageError(QString::fromUtf8(data));
            return;
        }
        try {
            auto j = json::parse(data.toStdString());
            emit messageSent(QString::fromStdString(j.value("event_id", "")));
        } catch (const std::exception& e) {
            emit sendMessageError(QString::fromStdString(e.what()));
        }
    });
}

void MatrixClient::uploadMedia(const QByteArray& data, const QString& contentType, const QString& filename)
{
    QString path = QString::fromUtf8(bsfchat::api_path::kMediaUpload);
    QUrl url = buildUrl(path);
    QUrlQuery query;
    query.addQueryItem("filename", filename);
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, contentType.toUtf8());
    if (!m_accessToken.isEmpty()) {
        request.setRawHeader("Authorization", ("Bearer " + m_accessToken).toUtf8());
    }

    auto* reply = m_nam.post(request, data);
    connect(reply, &QNetworkReply::uploadProgress, this,
        [this, filename](qint64 sent, qint64 total) {
            if (total <= 0) return;
            emit mediaUploadProgress(filename,
                double(sent) / double(total));
        });
    connect(reply, &QNetworkReply::finished, this, [this, reply, filename]() {
        reply->deleteLater();
        emit mediaUploadProgress(filename, 1.0);
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit mediaUploadError(QString::fromUtf8(data));
            return;
        }
        try {
            auto j = json::parse(data.toStdString());
            QString contentUri = QString::fromStdString(j.value("content_uri", ""));
            if (contentUri.isEmpty()) {
                emit mediaUploadError("No content_uri in response");
            } else {
                emit mediaUploaded(contentUri);
            }
        } catch (const std::exception& e) {
            emit mediaUploadError(QString::fromStdString(e.what()));
        }
    });
}

QString MatrixClient::mediaDownloadUrl(const QString& mxcUri) const
{
    // Convert mxc://server/mediaId to http(s)://homeserver/_matrix/media/v3/download/server/mediaId
    if (!mxcUri.startsWith("mxc://"))
        return {};

    QString path = mxcUri.mid(6); // strip "mxc://"
    return m_homeserver + QString::fromUtf8(bsfchat::api_path::kMediaDownload) + path;
}

void MatrixClient::getProfile(const QString& userId)
{
    QString path = QString::fromUtf8(bsfchat::api_path::kProfile)
                   + QUrl::toPercentEncoding(userId);

    auto* reply = makeRequest("GET", path);
    connect(reply, &QNetworkReply::finished, this, [this, reply, userId]() {
        reply->deleteLater();
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) return;
        try {
            auto j = json::parse(data.toStdString());
            QString displayName = QString::fromStdString(j.value("displayname", ""));
            QString avatarUrl = QString::fromStdString(j.value("avatar_url", ""));
            emit profileResult(userId, displayName, avatarUrl);
        } catch (...) {}
    });
}

void MatrixClient::setDisplayName(const QString& userId, const QString& displayName)
{
    QString path = QString::fromUtf8(bsfchat::api_path::kProfile)
                   + QUrl::toPercentEncoding(userId) + "/displayname";

    json body;
    body["displayname"] = displayName.toStdString();
    QByteArray reqBody = QByteArray::fromStdString(body.dump());

    auto* reply = makeRequest("PUT", path, reqBody);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return;
        emit displayNameUpdated();
    });
}

void MatrixClient::putPresence(const QString& userId,
                                const QString& presence,
                                const QString& statusMessage)
{
    QString path = QString::fromUtf8(bsfchat::api_path::kPresence)
                   + QUrl::toPercentEncoding(userId) + "/status";

    json body;
    body["presence"] = presence.toStdString();
    if (!statusMessage.isEmpty()) {
        body["status_msg"] = statusMessage.toStdString();
    }
    QByteArray reqBody = QByteArray::fromStdString(body.dump());

    auto* reply = makeRequest("PUT", path, reqBody);
    // Fire-and-forget — server-side write failures aren't worth
    // surfacing as toasts (presence is best-effort), but we still
    // delete the reply so it doesn't leak.
    connect(reply, &QNetworkReply::finished, this, [reply]() {
        reply->deleteLater();
    });
}

void MatrixClient::setAvatarUrl(const QString& userId, const QString& avatarUrl)
{
    QString path = QString::fromUtf8(bsfchat::api_path::kProfile)
                   + QUrl::toPercentEncoding(userId) + "/avatar_url";

    json body;
    body["avatar_url"] = avatarUrl.toStdString();
    QByteArray reqBody = QByteArray::fromStdString(body.dump());

    auto* reply = makeRequest("PUT", path, reqBody);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return;
        emit avatarUrlUpdated();
    });
}

void MatrixClient::setTyping(const QString& roomId, const QString& userId,
                              bool typing, int timeout)
{
    QString path = QString::fromUtf8(bsfchat::api_path::kTyping)
                   + QUrl::toPercentEncoding(roomId) + "/typing/"
                   + QUrl::toPercentEncoding(userId);

    json content;
    content["typing"] = typing;
    if (typing) {
        content["timeout"] = timeout;
    }
    QByteArray body = QByteArray::fromStdString(content.dump());

    auto* reply = makeRequest("PUT", path, body);
    connect(reply, &QNetworkReply::finished, this, [reply]() {
        reply->deleteLater();
        // Fire and forget — no response processing needed
    });
}

void MatrixClient::sendReadMarker(const QString& roomId)
{
    QString path = QString::fromUtf8(bsfchat::api_path::kRoomPrefix)
                   + QUrl::toPercentEncoding(roomId) + "/read_marker";

    // Empty body — server marks current max position as read for this user.
    auto* reply = makeRequest("POST", path, "{}");
    connect(reply, &QNetworkReply::finished, this, [reply]() {
        reply->deleteLater();
        // Fire and forget — server pushes new count via sync
    });
}

void MatrixClient::joinVoice(const QString& roomId)
{
    QString path = QString::fromUtf8(bsfchat::api_path::kRoomPrefix)
                   + QUrl::toPercentEncoding(roomId) + "/voice/join";

    auto* reply = makeRequest("POST", path, "{}");
    connect(reply, &QNetworkReply::finished, this, [this, reply, roomId]() {
        reply->deleteLater();
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit voiceError(QString::fromUtf8(data));
            return;
        }
        try {
            auto doc = QJsonDocument::fromJson(data);
            QJsonArray members = doc.object().value("members").toArray();
            emit voiceJoined(roomId, members);
        } catch (...) {
            emit voiceError("Failed to parse voice join response");
        }
    });
}

void MatrixClient::leaveVoice(const QString& roomId)
{
    QString path = QString::fromUtf8(bsfchat::api_path::kRoomPrefix)
                   + QUrl::toPercentEncoding(roomId) + "/voice/leave";

    auto* reply = makeRequest("POST", path, "{}");
    connect(reply, &QNetworkReply::finished, this, [this, reply, roomId]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit voiceError(QString::fromUtf8(reply->readAll()));
            return;
        }
        emit voiceLeft(roomId);
    });
}

void MatrixClient::getVoiceMembers(const QString& roomId)
{
    QString path = QString::fromUtf8(bsfchat::api_path::kRoomPrefix)
                   + QUrl::toPercentEncoding(roomId) + "/voice/members";

    auto* reply = makeRequest("GET", path);
    connect(reply, &QNetworkReply::finished, this, [this, reply, roomId]() {
        reply->deleteLater();
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit voiceError(QString::fromUtf8(data));
            return;
        }
        auto doc = QJsonDocument::fromJson(data);
        QJsonArray members = doc.object().value("members").toArray();
        emit voiceMembersResult(roomId, members);
    });
}

void MatrixClient::updateVoiceState(const QString& roomId, bool muted, bool deafened)
{
    QString path = QString::fromUtf8(bsfchat::api_path::kRoomPrefix)
                   + QUrl::toPercentEncoding(roomId) + "/voice/state";

    json content;
    content["muted"] = muted;
    content["deafened"] = deafened;
    QByteArray body = QByteArray::fromStdString(content.dump());

    auto* reply = makeRequest("PUT", path, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit voiceError(QString::fromUtf8(reply->readAll()));
        }
    });
}

void MatrixClient::createVoiceChannel(const QString& name)
{
    json content;
    content["name"] = name.toStdString();
    content["voice"] = true;
    content["visibility"] = "private";
    QByteArray body = QByteArray::fromStdString(content.dump());

    auto* reply = makeRequest("POST", QString::fromUtf8(bsfchat::api_path::kCreateRoom), body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit voiceError(QString::fromUtf8(data));
            return;
        }
        try {
            auto j = json::parse(data.toStdString());
            bsfchat::CreateRoomResponse resp;
            bsfchat::from_json(j, resp);
            emit voiceChannelCreated(QString::fromStdString(resp.room_id));
        } catch (const std::exception& e) {
            emit voiceError(QString::fromStdString(e.what()));
        }
    });
}

void MatrixClient::getRoomMessages(const QString& roomId, const QString& from,
                                    const QString& dir, int limit)
{
    QString path = QString::fromUtf8(bsfchat::api_path::kRoomPrefix)
                   + QUrl::toPercentEncoding(roomId) + "/messages";

    QUrl url = buildUrl(path);
    QUrlQuery query;
    query.addQueryItem("dir", dir);
    query.addQueryItem("limit", QString::number(limit));
    if (!from.isEmpty()) {
        query.addQueryItem("from", from);
    }
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!m_accessToken.isEmpty()) {
        request.setRawHeader("Authorization", ("Bearer " + m_accessToken).toUtf8());
    }

    auto* reply = m_nam.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit messagesError(QString::fromUtf8(data));
            return;
        }
        try {
            auto j = json::parse(data.toStdString());
            bsfchat::MessagesResponse resp;
            bsfchat::from_json(j, resp);
            emit messagesResult(resp);
        } catch (const std::exception& e) {
            emit messagesError(QString::fromStdString(e.what()));
        }
    });
}

void MatrixClient::getTurnConfig()
{
    auto* reply = makeRequest("GET", "/_matrix/client/v3/voip/turnServer");
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit voiceError(QString::fromUtf8(data));
            return;
        }
        auto doc = QJsonDocument::fromJson(data);
        emit turnConfigResult(doc.object());
    });
}

void MatrixClient::createCategoryRoom(const QString& name)
{
    json content;
    content["name"] = name.toStdString();
    content["is_category"] = true;
    content["visibility"] = "private";
    QByteArray body = QByteArray::fromStdString(content.dump());

    auto* reply = makeRequest("POST", QString::fromUtf8(bsfchat::api_path::kCreateRoom), body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit createRoomError(QString::fromUtf8(data));
            return;
        }
        try {
            auto j = json::parse(data.toStdString());
            bsfchat::CreateRoomResponse resp;
            bsfchat::from_json(j, resp);
            emit categoryRoomCreated(QString::fromStdString(resp.room_id));
        } catch (const std::exception& e) {
            emit createRoomError(QString::fromStdString(e.what()));
        }
    });
}

void MatrixClient::createChannelInCategory(const QString& name, const QString& categoryId, bool isVoice)
{
    json content;
    content["name"] = name.toStdString();
    // Public = auto-joined by every user. Channel-level visibility is
    // enforced by VIEW_CHANNEL overrides, not by Matrix invite-only semantics.
    content["visibility"] = "public";
    if (!categoryId.isEmpty()) {
        content["parent_id"] = categoryId.toStdString();
    }
    if (isVoice) {
        content["voice"] = true;
    }
    QByteArray body = QByteArray::fromStdString(content.dump());

    auto* reply = makeRequest("POST", QString::fromUtf8(bsfchat::api_path::kCreateRoom), body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit createRoomError(QString::fromUtf8(data));
            return;
        }
        try {
            auto j = json::parse(data.toStdString());
            bsfchat::CreateRoomResponse resp;
            bsfchat::from_json(j, resp);
            emit createRoomSuccess(QString::fromStdString(resp.room_id));
        } catch (const std::exception& e) {
            emit createRoomError(QString::fromStdString(e.what()));
        }
    });
}

void MatrixClient::moveChannel(const QString& roomId, const QString& categoryId)
{
    QString path = QString::fromUtf8(bsfchat::api_path::kRoomPrefix)
                   + QUrl::toPercentEncoding(roomId) + "/category";

    json content;
    content["parent_id"] = categoryId.toStdString();
    QByteArray body = QByteArray::fromStdString(content.dump());

    auto* reply = makeRequest("PUT", path, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            return;
        }
        emit channelMoved();
    });
}

void MatrixClient::setChannelOrder(const QString& roomId, int order)
{
    // Set the sort order via bsfchat.room.category state event
    json content;
    content["order"] = order;
    QByteArray body = QByteArray::fromStdString(content.dump());

    QString path = QString::fromUtf8(bsfchat::api_path::kRoomPrefix)
                   + QUrl::toPercentEncoding(roomId) + "/state/"
                   + QString::fromUtf8(bsfchat::event_type::kRoomCategory) + "/";

    auto* reply = makeRequest("PUT", path, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            return;
        }
        emit channelOrderSet();
    });
}

void MatrixClient::setRoomState(const QString& roomId, const QString& eventType,
                                 const QString& stateKey, const QByteArray& content)
{
    // Encode every user-supplied path segment. state_key in particular can
    // be a user ID (@user:host) or "role:<id>" / "user:<mxid>" — the ":" and
    // "@" need to survive the trip through Qt's URL parser and httplib's
    // server-side route regex without being interpreted as URL syntax.
    QString path = QString::fromUtf8(bsfchat::api_path::kRoomPrefix)
                   + QUrl::toPercentEncoding(roomId) + "/state/"
                   + QUrl::toPercentEncoding(eventType) + "/"
                   + QUrl::toPercentEncoding(stateKey);

    auto* reply = makeRequest("PUT", path, content);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, roomId, eventType, stateKey]() {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError) return;

        // Decode the HTTP status + Matrix error body so the caller can show
        // a toast instead of silently swallowing a 403/500. Without this,
        // "Save assignments" in ServerSettings looked successful even when
        // the server rejected the state write for permission reasons.
        int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QByteArray body = reply->readAll();
        QString msg;
        auto doc = QJsonDocument::fromJson(body);
        if (doc.isObject()) {
            auto o = doc.object();
            msg = o.value("error").toString();
            if (msg.isEmpty()) msg = o.value("errcode").toString();
        }
        if (msg.isEmpty()) msg = reply->errorString();
        qWarning().noquote() << "[setRoomState] FAIL" << status
                             << eventType << stateKey
                             << "-" << msg;
        emit stateEventError(roomId, eventType, stateKey, status, msg);
    });
}

void MatrixClient::getRoomState(const QString& roomId, const QString& eventType,
                                 const QString& stateKey)
{
    QString path = QString::fromUtf8(bsfchat::api_path::kRoomPrefix)
                   + QUrl::toPercentEncoding(roomId) + "/state/"
                   + eventType + "/" + stateKey;

    auto* reply = makeRequest("GET", path);
    connect(reply, &QNetworkReply::finished, this, [this, reply, roomId, eventType]() {
        reply->deleteLater();
        auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            return;
        }
        auto doc = QJsonDocument::fromJson(data);
        emit roomStateResult(roomId, eventType, doc.object());
    });
}

void MatrixClient::setMemberRoles(const QString& roomId, const QString& userId,
                                   const QStringList& roleIds)
{
    QJsonArray arr;
    for (const auto& id : roleIds) arr.append(id);
    QJsonObject body{{"role_ids", arr}};
    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    setRoomState(roomId, QString::fromUtf8(bsfchat::event_type::kMemberRoles), userId, payload);
}

void MatrixClient::setChannelPermission(const QString& roomId, const QString& targetKey,
                                         quint64 allow, quint64 deny)
{
    QString hexAllow = QStringLiteral("0x") + QString::number(allow, 16);
    QString hexDeny = QStringLiteral("0x") + QString::number(deny, 16);
    QJsonObject body{{"allow", hexAllow}, {"deny", hexDeny}};
    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    setRoomState(roomId, QString::fromUtf8(bsfchat::event_type::kChannelPermissions),
                 targetKey, payload);
}

void MatrixClient::setChannelSlowmode(const QString& roomId, int seconds)
{
    QJsonObject body{{"slowmode_seconds", seconds}};
    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    setRoomState(roomId, QString::fromUtf8(bsfchat::event_type::kChannelSettings),
                 QString(), payload);
}

void MatrixClient::redactEvent(const QString& roomId, const QString& eventId, const QString& reason)
{
    QString txn = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString path = QString::fromUtf8(bsfchat::api_path::kRoomPrefix)
                   + QUrl::toPercentEncoding(roomId) + "/redact/"
                   + QUrl::toPercentEncoding(eventId) + "/" + txn;

    QJsonObject body;
    if (!reason.isEmpty()) body["reason"] = reason;
    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    auto* reply = makeRequest("PUT", path, payload);
    connect(reply, &QNetworkReply::finished, this, [reply]() { reply->deleteLater(); });
}

void MatrixClient::sendReaction(const QString& roomId, const QString& targetEventId,
                                  const QString& emoji)
{
    static int txnCounter = 0;
    QString txnId = QString("rx%1.%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(++txnCounter);

    QString path = QString::fromUtf8(bsfchat::api_path::kRoomPrefix)
                   + QUrl::toPercentEncoding(roomId)
                   + "/send/m.reaction/" + txnId;

    json content;
    content["m.relates_to"] = {
        {"rel_type", "m.annotation"},
        {"event_id", targetEventId.toStdString()},
        {"key", emoji.toStdString()},
    };

    QByteArray reqBody = QByteArray::fromStdString(content.dump());
    auto* reply = makeRequest("PUT", path, reqBody);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit sendMessageError(QString::fromUtf8(reply->readAll()));
        }
    });
}

void MatrixClient::redactReaction(const QString& roomId, const QString& reactionEventId)
{
    // Reuse redactEvent — same server route for any event id.
    redactEvent(roomId, reactionEventId, QString());
}

void MatrixClient::kickUser(const QString& roomId, const QString& userId, const QString& reason)
{
    QString path = QString::fromUtf8(bsfchat::api_path::kRoomPrefix)
                   + QUrl::toPercentEncoding(roomId) + "/kick";
    QJsonObject body{{"user_id", userId}};
    if (!reason.isEmpty()) body["reason"] = reason;
    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    auto* reply = makeRequest("POST", path, payload);
    connect(reply, &QNetworkReply::finished, this, [reply]() { reply->deleteLater(); });
}

void MatrixClient::banUser(const QString& roomId, const QString& userId, const QString& reason)
{
    QString path = QString::fromUtf8(bsfchat::api_path::kRoomPrefix)
                   + QUrl::toPercentEncoding(roomId) + "/ban";
    QJsonObject body{{"user_id", userId}};
    if (!reason.isEmpty()) body["reason"] = reason;
    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    auto* reply = makeRequest("POST", path, payload);
    connect(reply, &QNetworkReply::finished, this, [reply]() { reply->deleteLater(); });
}

void MatrixClient::unbanUser(const QString& roomId, const QString& userId)
{
    QString path = QString::fromUtf8(bsfchat::api_path::kRoomPrefix)
                   + QUrl::toPercentEncoding(roomId) + "/unban";
    QJsonObject body{{"user_id", userId}};
    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    auto* reply = makeRequest("POST", path, payload);
    connect(reply, &QNetworkReply::finished, this, [reply]() { reply->deleteLater(); });
}
