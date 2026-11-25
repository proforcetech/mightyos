#pragma once

#include <QObject>
#include <QPointer>
#include <QQuickWindow>

class InputRouter;
class QQmlEngine;

// The Guide overlay over a running app (PLAN section 3, M6): a separate
// window on the wlr-layer-shell *overlay* layer (LayerShellQt), anchored to
// every edge, with exclusive keyboard interactivity while it is shown. The
// app's fullscreen window stays where it is; hiding the overlay gives the
// keyboard back to it (labwc refocuses the topmost toplevel). Raising
// MightyOS's own window instead would need the compositor to give focus
// back to the app on Resume, which none of labwc's client protocols do
// without extra IPC.
//
// The overlay window (qml/GuideOverlay.qml) is created on show() and
// destroyed on hide(), so its scene graph and textures cost nothing while a
// game runs. It gets the main window as `host` and renders the same
// GuideMenu as the in-window fallback.
//
// available() needs: a build with LayerShellQt (Ubuntu:
// liblayershellqtinterface-dev; MIGHTYOS_HAVE_LAYERSHELL), the Wayland
// platform plugin and a compositor that offers zwlr_layer_shell_v1 (checked
// on a private event queue at construction). Otherwise QML falls back to
// the in-window Guide menu (only useful where MightyOS's window is visible).
class GuideOverlayHost : public QObject
{
    Q_OBJECT

public:
    GuideOverlayHost(QQmlEngine *engine, QWindow *host, InputRouter *router, QObject *parent = nullptr);
    ~GuideOverlayHost() override;

    bool isAvailable() const { return m_available; }
    bool isShown() const { return !m_window.isNull(); }
    QQuickWindow *window() const { return m_window; }

    // False if unavailable or the window could not be created (logged).
    bool show();
    void hide();

    // Built with LayerShellQt at all.
    static bool compiledIn();
    // The running compositor offers zwlr_layer_shell_v1 (false off Wayland
    // or without LayerShellQt in the build).
    static bool compositorHasLayerShell();

signals:
    void shownChanged();

private:
    QQmlEngine *m_engine;
    QPointer<QWindow> m_host;
    InputRouter *m_router;
    QPointer<QQuickWindow> m_window;
    bool m_available = false;
};
