#include "net/HttpFetch.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include <memory>

namespace bsfchat {

FetchFn networkFetch(QObject* owner, int timeoutMs)
{
    auto* nam = new QNetworkAccessManager(owner);
    // Discovery walks .well-known -> base_url deliberately, by reading the
    // file; it must not also be dragged around by HTTP redirects to
    // somewhere the user never asked for.
    nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);

    return [nam = QPointer<QNetworkAccessManager>(nam), timeoutMs](
               const QString& url, std::function<void(const HttpResponse&)> done) {
        if (!nam) {
            done(HttpResponse::unreachable(QStringLiteral("client shutting down")));
            return;
        }

        QNetworkRequest request{QUrl(url)};
        request.setTransferTimeout(timeoutMs);
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                             QNetworkRequest::AlwaysNetwork);

        QNetworkReply* reply = nam->get(request);

        // `answered` guards the one rule every FetchFn has to keep: call
        // `done` exactly once. setTransferTimeout aborts a stalled transfer,
        // which arrives as finished() — but a reply that never even starts
        // needs the belt-and-braces timer below, and an abort can race a
        // finish.
        auto answered = std::make_shared<bool>(false);
        auto respond = [answered, done](const HttpResponse& res) {
            if (*answered) return;
            *answered = true;
            done(res);
        };

        auto* guard = new QTimer(reply);
        guard->setSingleShot(true);
        guard->setInterval(timeoutMs + 500);
        QObject::connect(guard, &QTimer::timeout, reply, [reply, respond]() {
            respond(HttpResponse::unreachable(QStringLiteral("timed out")));
            reply->abort();
        });
        guard->start();

        QObject::connect(reply, &QNetworkReply::finished, reply, [reply, respond]() {
            reply->deleteLater();

            const QVariant code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
            const QByteArray body = reply->readAll();

            // An HTTP error status is still an answer, and the body is the
            // interesting part of it (a 404 page, or a Matrix error JSON).
            // Only the absence of a status line is unreachable.
            if (!code.isValid()) {
                respond(HttpResponse::unreachable(reply->errorString()));
                return;
            }
            respond(HttpResponse::http(code.toInt(), body));
        });
    };
}

} // namespace bsfchat
