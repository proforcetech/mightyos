#pragma once

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSize>
#include <QString>
#include <QUrl>
#include <functional>
#include <optional>

// SteamGridDB (PLAN D11): https://www.steamgriddb.com/api/v2, artwork on
// its CDN. Network responses are untrusted.
//
//  - HTTPS only, default TLS validation (no sslErrors handler, so any TLS
//    error fails the request), strict host allowlist: steamgriddb.com and
//    its subdomains (www. for the API, cdn2. for images). Redirects are
//    never followed.
//  - The API key goes only to the API host, as "Authorization: Bearer";
//    image downloads never carry it.
//  - Responses are capped while they stream (Content-Length checked first,
//    then every chunk); over the cap the request is aborted.
//  - Images must be PNG or JPEG by magic bytes *and* Content-Type, decode
//    fully with the decoder forced to that format, and be at most
//    kMaxImageDimension px a side: the formats ArtworkProvider accepts.
namespace SteamGridDb {

constexpr qint64 kMaxJsonBytes = 512 * 1024;
constexpr qint64 kMaxImageBytes = 6 * 1024 * 1024;
constexpr int kMaxImageDimension = 4096;
constexpr int kMaxTermLength = 80;

bool isAllowedUrl(const QUrl &url);
bool isApiUrl(const QUrl &url);

QUrl searchUrl(const QString &term);
QUrl gridsForGameUrl(qint64 gameId);
QUrl gridsForSteamUrl(const QString &appid);

// First result's game id.
std::optional<qint64> parseSearch(const QByteArray &json);
// First grid whose URL is allowed and whose mime is PNG or JPEG.
QUrl parseGrids(const QByteArray &json);

// "png" or "jpg" for a valid image, "" otherwise.
QString validateImage(const QByteArray &bytes, const QByteArray &contentType, QString *error = nullptr);

} // namespace SteamGridDb

struct HttpResponse {
    enum class Error { None, Network, TooLarge, Blocked, Redirect, Timeout };
    Error error = Error::None;
    int status = 0;
    QByteArray body;
    QByteArray contentType;
    int retryAfterSeconds = -1;
};

// One GET at a time per call; `done` runs exactly once, on the fetcher's
// thread, never synchronously inside get().
class HttpFetcher : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    using Callback = std::function<void(const HttpResponse &)>;
    // `bearer` empty: no Authorization header.
    virtual void get(const QUrl &url, const QString &bearer, qint64 maxBytes, Callback done) = 0;
};

class NetworkFetcher : public HttpFetcher
{
    Q_OBJECT

public:
    static constexpr int kTimeoutMs = 20000;
    explicit NetworkFetcher(QObject *parent = nullptr);
    void get(const QUrl &url, const QString &bearer, qint64 maxBytes, Callback done) override;

private:
    QNetworkAccessManager m_nam;
};
