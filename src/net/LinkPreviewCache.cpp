#include "net/LinkPreviewCache.h"

#include <QDateTime>
#include <QTimer>
#include <QUrl>

#include "util/MediaUrl.h"

#include <algorithm>

using bsfchat::net::MatrixFailure;

namespace bsfchat::client {

namespace {

// Collapse every run of whitespace or control characters to one space, trim,
// and cut to `maxChars` with an ellipsis. Control characters include the bidi
// overrides a page title can use to make the card read differently from the
// link it stands for.
QString cleanText(const QString& in, int maxChars)
{
    QString out;
    out.reserve(std::min<qsizetype>(in.size(), maxChars + 1));
    bool pendingSpace = false;
    for (const QChar c : in) {
        const auto cat = c.category();
        const bool blank = c.isSpace() || cat == QChar::Other_Control
                           || cat == QChar::Other_Format;
        if (blank) {
            pendingSpace = !out.isEmpty();
            continue;
        }
        if (pendingSpace) {
            out += QLatin1Char(' ');
            pendingSpace = false;
        }
        out += c;
        if (out.size() > maxChars) break;
    }
    if (out.size() > maxChars) {
        qsizetype cut = maxChars - 1;
        // Never split a surrogate pair.
        if (cut > 0 && out.at(cut - 1).isHighSurrogate()) --cut;
        out.truncate(cut);
        out = out.trimmed() + QChar(0x2026);
    }
    return out;
}

QString textField(const QJsonObject& o, const char* key, int maxChars)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isString() ? cleanText(v.toString(), maxChars) : QString();
}

int dimension(const QJsonObject& o, const char* key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (!v.isDouble()) return 0;
    const double d = v.toDouble();
    return (d > 0 && d <= 100000) ? int(d) : 0;
}

} // namespace

LinkPreviewCache::LinkPreviewCache(QObject* parent, Clock clock)
    : QObject(parent)
    , m_clock(clock ? std::move(clock)
                    : Clock([] { return QDateTime::currentMSecsSinceEpoch(); }))
{
}

qint64 LinkPreviewCache::now() const
{
    return m_clock();
}

bool LinkPreviewCache::isPreviewableUrl(const QString& url)
{
    if (url.isEmpty() || url.size() > kMaxUrlLength) return false;
    const QUrl u(url, QUrl::StrictMode);
    if (!u.isValid()) return false;
    const QString scheme = u.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) return false;
    if (u.host().isEmpty()) return false;
    // user:pass@host — not something to hand anybody, the server included.
    if (!u.userInfo().isEmpty()) return false;
    return true;
}

bool LinkPreviewCache::isUnsupportedFailure(const MatrixFailure& f)
{
    if (f.errcode == QLatin1String("M_UNRECOGNIZED")) return true;
    if (f.status == 501) return true;
    // A route the server does not have: 404/405 with no Matrix body. A 404
    // WITH an errcode (M_NOT_FOUND) is the endpoint saying "nothing to
    // preview" for this one URL.
    return (f.status == 404 || f.status == 405) && f.errcode.isEmpty();
}

LinkPreviewCache::Preview LinkPreviewCache::parse(const QJsonObject& body, bool* ok)
{
    Preview p;
    p.title = textField(body, "og:title", kMaxTitleChars);
    p.description = textField(body, "og:description", kMaxDescriptionChars);
    p.siteName = textField(body, "og:site_name", kMaxSiteNameChars);

    const QJsonValue image = body.value(QLatin1String("og:image"));
    if (image.isString() && isValidMxcUri(image.toString())) {
        const QJsonValue size = body.value(QLatin1String("matrix:image:size"));
        const bool tooBig = size.isDouble() && size.toDouble() > double(kMaxImageBytes);
        if (!tooBig) {
            p.imageMxc = image.toString();
            p.imageWidth = dimension(body, "og:image:width");
            p.imageHeight = dimension(body, "og:image:height");
        }
    }
    if (ok) *ok = !p.title.isEmpty() || !p.description.isEmpty() || !p.imageMxc.isEmpty();
    return p;
}

void LinkPreviewCache::setEnabled(bool enabled)
{
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    if (enabled) return;
    const QStringList dropped = m_queue;
    m_queue.clear();
    for (const QString& url : dropped) emit resolved(url);
}

LinkPreviewCache::State LinkPreviewCache::state(const QString& url) const
{
    if (!m_enabled || !m_supported) return State::Empty;
    if (m_inFlight.contains(url) || m_queue.contains(url)) return State::Pending;
    auto it = m_entries.find(url);
    if (it == m_entries.end()) return State::Unknown;
    if (it->expiresAtMs <= now()) {
        m_lru.erase(it->lru);
        m_entries.erase(it);
        return State::Unknown;
    }
    m_lru.splice(m_lru.begin(), m_lru, it->lru);
    return it->state;
}

LinkPreviewCache::Preview LinkPreviewCache::preview(const QString& url) const
{
    if (state(url) != State::Ready) return {};
    return m_entries.value(url).preview;
}

bool LinkPreviewCache::request(const QString& url)
{
    if (!m_enabled || !m_supported || !isPreviewableUrl(url)) return false;
    if (m_inFlight.contains(url) || m_queue.contains(url)) return true;
    // state() also drops an expired entry, which is what lets it refetch.
    const State known = state(url);
    if (known == State::Ready || known == State::Empty) return false;

    m_queue.append(url);
    while (m_queue.size() > kMaxQueued) {
        // Oldest first: in a timeline the newest requests are the cards on
        // screen. The dropped card reads Unknown and stays hidden until it is
        // recreated.
        const QString dropped = m_queue.takeFirst();
        emit resolved(dropped);
    }
    pump();
    return true;
}

void LinkPreviewCache::pump()
{
    if (m_pausedUntilMs > 0) {
        const qint64 wait = m_pausedUntilMs - now();
        if (wait > 0) {
            if (!m_resumeScheduled) {
                m_resumeScheduled = true;
                QTimer::singleShot(int(wait), this, [this]() {
                    m_resumeScheduled = false;
                    pump();
                });
            }
            return;
        }
        m_pausedUntilMs = 0;
    }
    while (m_enabled && m_supported && m_inFlight.size() < kMaxInFlight
           && !m_queue.isEmpty()) {
        const QString url = m_queue.takeFirst();
        m_inFlight.insert(url);
        emit fetchRequested(url);
    }
}

void LinkPreviewCache::store(const QString& url, State state, const Preview& preview,
                             qint64 ttlMs)
{
    auto it = m_entries.find(url);
    if (it != m_entries.end()) {
        m_lru.erase(it->lru);
        m_entries.erase(it);
    }
    m_lru.push_front(url);
    Entry e;
    e.state = state;
    e.preview = preview;
    e.expiresAtMs = now() + ttlMs;
    e.lru = m_lru.begin();
    m_entries.insert(url, e);
    while (m_entries.size() > kMaxEntries) {
        m_entries.remove(m_lru.back());
        m_lru.pop_back();
    }
}

void LinkPreviewCache::fetchSucceeded(const QString& url, const QJsonObject& body)
{
    if (m_inFlight.remove(url)) {
        bool ok = false;
        const Preview p = parse(body, &ok);
        if (ok) store(url, State::Ready, p, kPositiveTtlMs);
        else store(url, State::Empty, {}, kNegativeTtlMs);
        emit resolved(url);
    }
    pump();
}

void LinkPreviewCache::fetchFailed(const QString& url, const MatrixFailure& failure)
{
    const bool wasInFlight = m_inFlight.remove(url);
    if (isUnsupportedFailure(failure)) {
        markUnsupported();
        if (wasInFlight) emit resolved(url);
        return;
    }
    if (!wasInFlight) {
        pump();
        return;
    }
    if (failure.isRateLimited()) {
        // Not this URL's fault: put it back at the head of the queue and hold
        // everything until the server's wait is over.
        const qint64 wait = std::clamp<qint64>(failure.retryAfterMs, 2000, 5 * 60 * 1000);
        m_pausedUntilMs = std::max(m_pausedUntilMs, now() + wait);
        if (m_enabled && m_supported) m_queue.prepend(url);
        else emit resolved(url);
        pump();
        return;
    }
    const bool transient = failure.status == 0 || failure.status == 401
                           || failure.status >= 500;
    store(url, State::Empty, {}, transient ? kTransientTtlMs : kNegativeTtlMs);
    emit resolved(url);
    pump();
}

void LinkPreviewCache::markUnsupported()
{
    if (!m_supported) return;
    m_supported = false;
    QStringList waiting = m_queue;
    waiting += QStringList(m_inFlight.cbegin(), m_inFlight.cend());
    m_queue.clear();
    m_inFlight.clear();
    emit supportedChanged();
    for (const QString& url : waiting) emit resolved(url);
}

} // namespace bsfchat::client
