#include "steamgriddb.h"

#include "steamlibrary.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>
#include <memory>

namespace SteamGridDb {

namespace {
const QString kApiHost = QStringLiteral("www.steamgriddb.com");
const QString kApiBase = QStringLiteral("https://www.steamgriddb.com/api/v2/");

QUrl gridQuery(const QString &path)
{
    QUrl url(kApiBase + path);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("dimensions"), QStringLiteral("600x900"));
    query.addQueryItem(QStringLiteral("types"), QStringLiteral("static"));
    query.addQueryItem(QStringLiteral("mimes"), QStringLiteral("image/png,image/jpeg"));
    query.addQueryItem(QStringLiteral("nsfw"), QStringLiteral("false"));
    query.addQueryItem(QStringLiteral("humor"), QStringLiteral("false"));
    query.addQueryItem(QStringLiteral("epilepsy"), QStringLiteral("false"));
    url.setQuery(query);
    return url;
}
} // namespace

bool isAllowedUrl(const QUrl &url)
{
    if (!url.isValid() || url.scheme() != QLatin1String("https") || !url.userInfo().isEmpty())
        return false;
    if (url.port(443) != 443)
        return false;
    const QString host = url.host(QUrl::FullyEncoded).toLower();
    return host == QLatin1String("steamgriddb.com") || host.endsWith(QLatin1String(".steamgriddb.com"));
}

bool isApiUrl(const QUrl &url)
{
    return isAllowedUrl(url) && url.host().toLower() == kApiHost
           && url.path().startsWith(QLatin1String("/api/v2/"));
}

QUrl searchUrl(const QString &term)
{
    const QString cleaned = term.simplified().left(kMaxTermLength);
    if (cleaned.isEmpty())
        return {};
    // Every reserved character percent-encoded: the term is one path segment.
    const QByteArray encoded = QUrl::toPercentEncoding(cleaned);
    return QUrl::fromEncoded(kApiBase.toLatin1() + "search/autocomplete/" + encoded, QUrl::StrictMode);
}

QUrl gridsForGameUrl(qint64 gameId)
{
    return gameId > 0 ? gridQuery(QStringLiteral("grids/game/") + QString::number(gameId)) : QUrl();
}

QUrl gridsForSteamUrl(const QString &appid)
{
    return SteamLibrary::isValidAppId(appid) ? gridQuery(QStringLiteral("grids/steam/") + appid) : QUrl();
}

std::optional<qint64> parseSearch(const QByteArray &json)
{
    if (json.size() > kMaxJsonBytes)
        return std::nullopt;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject() || !doc.object().value(QStringLiteral("success")).toBool())
        return std::nullopt;
    const QJsonArray data = doc.object().value(QStringLiteral("data")).toArray();
    if (data.isEmpty() || !data.first().isObject())
        return std::nullopt;
    const QJsonValue id = data.first().toObject().value(QStringLiteral("id"));
    if (!id.isDouble())
        return std::nullopt;
    const double value = id.toDouble();
    if (value < 1 || value > 1e12 || value != double(qint64(value)))
        return std::nullopt;
    return qint64(value);
}

QUrl parseGrids(const QByteArray &json)
{
    if (json.size() > kMaxJsonBytes)
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject() || !doc.object().value(QStringLiteral("success")).toBool())
        return {};
    const QJsonArray data = doc.object().value(QStringLiteral("data")).toArray();
    int looked = 0;
    for (const QJsonValue &value : data) {
        if (++looked > 50)
            break;
        const QJsonObject o = value.toObject();
        const QString mime = o.value(QStringLiteral("mime")).toString();
        if (mime != QLatin1String("image/png") && mime != QLatin1String("image/jpeg"))
            continue;
        if (o.value(QStringLiteral("nsfw")).toBool(false))
            continue;
        const QString text = o.value(QStringLiteral("url")).toString();
        if (text.size() > 2048)
            continue;
        const QUrl url(text, QUrl::StrictMode);
        if (isAllowedUrl(url) && !isApiUrl(url))
            return url;
    }
    return {};
}

QString validateImage(const QByteArray &bytes, const QByteArray &contentType, QString *error)
{
    auto fail = [error](const char *why) {
        if (error)
            *error = QString::fromLatin1(why);
        return QString();
    };
    if (bytes.isEmpty() || bytes.size() > kMaxImageBytes)
        return fail("empty or too large");
    const QByteArray type = contentType.split(';').first().trimmed().toLower();
    QByteArray format;
    QString ext;
    if (bytes.startsWith("\x89PNG\r\n\x1a\n") && type == "image/png") {
        format = "png";
        ext = QStringLiteral("png");
    } else if (bytes.startsWith("\xFF\xD8\xFF") && (type == "image/jpeg" || type == "image/jpg")) {
        format = "jpeg";
        ext = QStringLiteral("jpg");
    } else {
        return fail("not a PNG or JPEG (magic bytes and Content-Type must agree)");
    }
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, format);
    reader.setAutoDetectImageFormat(false);
    reader.setDecideFormatFromContent(false);
    const QSize size = reader.size();
    if (!size.isValid() || size.width() > kMaxImageDimension || size.height() > kMaxImageDimension)
        return fail("missing or too large dimensions");
    // A full decode, so a truncated or malformed file is rejected now
    // rather than shown as a broken tile.
    if (reader.read().isNull())
        return fail("does not decode");
    return ext;
}

} // namespace SteamGridDb

NetworkFetcher::NetworkFetcher(QObject *parent)
    : HttpFetcher(parent)
{
    m_nam.setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
    m_nam.setStrictTransportSecurityEnabled(true);
    m_nam.setTransferTimeout(kTimeoutMs);
}

void NetworkFetcher::get(const QUrl &url, const QString &bearer, qint64 maxBytes, Callback done)
{
    auto finish = [this](Callback cb, HttpResponse response) {
        QMetaObject::invokeMethod(this, [cb = std::move(cb), response] { cb(response); }, Qt::QueuedConnection);
    };
    if (!SteamGridDb::isAllowedUrl(url) || (!bearer.isEmpty() && !SteamGridDb::isApiUrl(url))) {
        HttpResponse blocked;
        blocked.error = HttpResponse::Error::Blocked;
        finish(std::move(done), blocked);
        return;
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("MightyOS/%1").arg(QCoreApplication::applicationVersion()));
    if (!bearer.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + bearer.toLatin1());
    request.setTransferTimeout(kTimeoutMs);

    QNetworkReply *reply = m_nam.get(request);
    auto body = std::make_shared<QByteArray>();
    auto tooLarge = std::make_shared<bool>(false);
    connect(reply, &QNetworkReply::metaDataChanged, reply, [reply, maxBytes, tooLarge] {
        const QVariant length = reply->header(QNetworkRequest::ContentLengthHeader);
        if (length.isValid() && length.toLongLong() > maxBytes) {
            *tooLarge = true;
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::readyRead, reply, [reply, maxBytes, body, tooLarge] {
        if (*tooLarge)
            return;
        body->append(reply->read(maxBytes + 1 - body->size()));
        if (body->size() > maxBytes) {
            *tooLarge = true;
            body->clear();
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [reply, body, tooLarge, maxBytes, done = std::move(done)] {
        reply->deleteLater();
        if (!*tooLarge) {
            body->append(reply->read(maxBytes + 1 - body->size()));
            if (body->size() > maxBytes) {
                *tooLarge = true;
                body->clear();
            }
        }
        HttpResponse response;
        response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        response.contentType = reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
        bool ok = false;
        const int retry = reply->rawHeader("Retry-After").toInt(&ok);
        response.retryAfterSeconds = ok ? qBound(0, retry, 3600) : -1;
        if (*tooLarge) {
            response.error = HttpResponse::Error::TooLarge;
        } else if (response.status >= 300 && response.status < 400) {
            response.error = HttpResponse::Error::Redirect;
        } else if (reply->error() == QNetworkReply::OperationCanceledError
                   || reply->error() == QNetworkReply::TimeoutError) {
            response.error = HttpResponse::Error::Timeout;
        } else if (reply->error() != QNetworkReply::NoError && response.status == 0) {
            response.error = HttpResponse::Error::Network;
        } else {
            response.body = *body;
        }
        done(response);
    });
}
