#include "session.h"

#include "artworkprovider.h"
#include "guideoverlayhost.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QPixmapCache>
#include <QQuickWindow>

#include <malloc.h>

Q_LOGGING_CATEGORY(lcSession, "mightyos.session")

Session::Session(QObject *parent)
    : QObject(parent)
{
}

Session::~Session() = default;

void Session::configure(const Options &options)
{
    m_options = options;
    emit configured();
    emit groupWarningChanged();
}

void Session::quit()
{
    qCInfo(lcSession) << "leaving MightyOS on request";
    QCoreApplication::quit();
}

void Session::acknowledgeGroupWarning()
{
    if (!m_options.groupWarning)
        return;
    m_options.groupWarning = false;
    emit groupWarningChanged();
    emit groupWarningAcknowledged(m_options.riskyGroups);
}

void Session::setOverlayHost(GuideOverlayHost *host)
{
    if (m_overlay == host)
        return;
    if (m_overlay)
        m_overlay->disconnect(this);
    m_overlay = host;
    if (m_overlay)
        connect(m_overlay, &GuideOverlayHost::shownChanged, this, &Session::overlayChanged);
    emit overlayChanged();
}

bool Session::isOverlayAvailable() const
{
    return m_overlay && m_overlay->isAvailable();
}

bool Session::isOverlayShown() const
{
    return m_overlay && m_overlay->isShown();
}

bool Session::showOverlay()
{
    if (!m_overlay)
        return false;
    const bool wasAvailable = m_overlay->isAvailable();
    const bool shown = m_overlay->show();
    if (wasAvailable != m_overlay->isAvailable())
        emit overlayChanged();   // creation failed: QML switches to the fallback
    return shown;
}

void Session::hideOverlay()
{
    if (m_overlay)
        m_overlay->hide();
}

void Session::setGuideOpen(bool open)
{
    if (m_guideOpen == open)
        return;
    m_guideOpen = open;
    emit guideOpenChanged();
}

void Session::setDormant(bool dormant)
{
    if (m_dormant == dormant)
        return;
    m_dormant = dormant;
    if (dormant) {
        const qsizetype freed = releaseCaches();
        qCInfo(lcSession) << "dormant while the app runs; artwork memory cache freed:" << freed / 1024 << "KiB";
    } else {
        qCInfo(lcSession) << "awake";
    }
    emit dormantChanged();
}

qsizetype Session::releaseCaches()
{
    // Scene-graph caches (glyphs, unused textures) and unreferenced
    // pixmaps; visible items keep their images, so waking up is instant.
    if (m_window)
        m_window->releaseResources();
    QPixmapCache::clear();
    const qsizetype freed = m_artwork ? m_artwork->trimMemory() : 0;
    // Give freed heap back to the system (glibc keeps it otherwise).
    ::malloc_trim(0);
    return freed;
}
