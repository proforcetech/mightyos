#pragma once

#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

class ArtworkProvider;
class GuideOverlayHost;

// Session-level state shared by main() and QML (PLAN M6).
//
//  - sessionMode: started with --session-mode (the MightyOS login session,
//    session/mightyos-session). Only then are crashes counted, leftover
//    app scopes stopped and the group warning toasted.
//  - safeMode: SessionState counted kSafeModeCrashes crashes within its
//    window (or --safe-mode). Safe mode: no artwork downloads (Scraper
//    paused), no image decoding at all (ArtworkProvider disabled: cached
//    artwork could be what crashed), no HDMI-CEC, and a "Safe mode" banner.
//    (Themes and automatic CEC Active Source don't exist yet.)
//  - riskyGroups: the session user's groups that a kiosk user shouldn't
//    have (sudo, video, input, ...; PLAN D8, D16). Always logged;
//    groupWarning is true once per new set in session mode (one toast).
//  - Guide overlay: overlayAvailable when GuideOverlayHost can show a
//    layer-shell window; showOverlay() / hideOverlay(). QML keeps
//    `guideOpen` (overlay or in-window fallback menu) up to date; main()
//    lets gamepad and remote input through while it is open.
//  - dormant: QML sets it while an app runs and the Guide menu is closed.
//    Entering it releases what can be rebuilt (scene-graph caches and
//    unused pixmaps via QQuickWindow::releaseResources(), the artwork
//    memory cache, malloc arenas); QML stops its timers and hides the
//    screens, so nothing renders until the app exits.
//  - guideRequested / closeRequested / quitRequested: the labwc panic
//    keybinds (`mightyos --signal=guide|close|quit` -> SIGUSR1 / SIGUSR2 /
//    SIGRTMIN+1 through SignalWatcher, see session/labwc/rc.xml.in).
class Session : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool sessionMode READ isSessionMode NOTIFY configured FINAL)
    Q_PROPERTY(bool safeMode READ isSafeMode NOTIFY configured FINAL)
    Q_PROPERTY(int recentCrashes READ recentCrashes NOTIFY configured FINAL)
    Q_PROPERTY(QStringList riskyGroups READ riskyGroups NOTIFY configured FINAL)
    Q_PROPERTY(bool groupWarning READ groupWarning NOTIFY groupWarningChanged FINAL)
    Q_PROPERTY(bool overlayAvailable READ isOverlayAvailable NOTIFY overlayChanged FINAL)
    Q_PROPERTY(bool overlayShown READ isOverlayShown NOTIFY overlayChanged FINAL)
    Q_PROPERTY(bool guideOpen READ isGuideOpen WRITE setGuideOpen NOTIFY guideOpenChanged FINAL)
    Q_PROPERTY(bool dormant READ isDormant WRITE setDormant NOTIFY dormantChanged FINAL)

public:
    struct Options {
        bool sessionMode = false;
        bool safeMode = false;
        int recentCrashes = 0;
        QStringList riskyGroups;
        bool groupWarning = false;
    };

    explicit Session(QObject *parent = nullptr);
    ~Session() override;

    // main(), before QML loads.
    void configure(const Options &options);
    void setOverlayHost(GuideOverlayHost *host);
    void setArtworkProvider(ArtworkProvider *provider) { m_artwork = provider; }
    void setMainWindow(QQuickWindow *window) { m_window = window; }

    bool isSessionMode() const { return m_options.sessionMode; }
    bool isSafeMode() const { return m_options.safeMode; }
    int recentCrashes() const { return m_options.recentCrashes; }
    QStringList riskyGroups() const { return m_options.riskyGroups; }
    bool groupWarning() const { return m_options.groupWarning; }
    // The toast was shown: don't show it again for this group set.
    Q_INVOKABLE void acknowledgeGroupWarning();

    bool isOverlayAvailable() const;
    bool isOverlayShown() const;
    Q_INVOKABLE bool showOverlay();
    Q_INVOKABLE void hideOverlay();

    bool isGuideOpen() const { return m_guideOpen; }
    void setGuideOpen(bool open);
    bool isDormant() const { return m_dormant; }
    void setDormant(bool dormant);
    // What entering dormant mode does (also for tests). Returns the bytes
    // the artwork memory cache gave back.
    qsizetype releaseCaches();

    // From the signal watcher (main) or tests.
    Q_INVOKABLE void requestGuide() { emit guideRequested(); }
    Q_INVOKABLE void requestClose() { emit closeRequested(); }
    // Ctrl+Alt+Backspace (SIGRTMIN+1): QML decides (a standard profile is
    // asked to confirm, a kids profile gets "Ask a parent").
    Q_INVOKABLE void requestQuit() { emit quitRequested(); }
    // Leave MightyOS: a clean quit, exit status 0, so mightyos-session ends
    // the session (with autologin it starts again).
    Q_INVOKABLE void quit();

signals:
    void configured();
    void groupWarningChanged();
    void groupWarningAcknowledged(const QStringList &groups);
    void overlayChanged();
    void guideOpenChanged();
    void dormantChanged();
    void guideRequested();
    void closeRequested();
    void quitRequested();

private:
    Options m_options;
    QPointer<GuideOverlayHost> m_overlay;
    ArtworkProvider *m_artwork = nullptr;
    QPointer<QQuickWindow> m_window;
    bool m_guideOpen = false;
    bool m_dormant = false;
};
