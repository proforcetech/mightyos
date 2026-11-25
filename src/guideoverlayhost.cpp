#include "guideoverlayhost.h"

#include "inputrouter.h"

#include <QGuiApplication>
#include <QLoggingCategory>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>

#ifdef MIGHTYOS_HAVE_LAYERSHELL
#include <LayerShellQt/Window>
#include <QtGui/qguiapplication_platform.h>
#include <cstring>
#include <wayland-client.h>
#endif

Q_LOGGING_CATEGORY(lcOverlay, "mightyos.overlay")

GuideOverlayHost::GuideOverlayHost(QQmlEngine *engine, QWindow *host, InputRouter *router, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
    , m_host(host)
    , m_router(router)
{
    m_available = engine && host && compositorHasLayerShell();
    if (m_available)
        qCInfo(lcOverlay) << "Guide overlay: wlr-layer-shell overlay window";
    else
        qCInfo(lcOverlay).noquote() << "Guide overlay: in-window fallback"
                                    << (compiledIn() ? "(the compositor has no wlr-layer-shell)"
                                                     : "(built without LayerShellQt)");
}

GuideOverlayHost::~GuideOverlayHost()
{
    // No event loop left to run a deleteLater(): delete it now, before the
    // engine that created it goes.
    if (QQuickWindow *window = m_window) {
        m_window = nullptr;
        if (m_router)
            m_router->removeWindow(window);
        delete window;
    }
}

bool GuideOverlayHost::compiledIn()
{
#ifdef MIGHTYOS_HAVE_LAYERSHELL
    return true;
#else
    return false;
#endif
}

bool GuideOverlayHost::compositorHasLayerShell()
{
#ifdef MIGHTYOS_HAVE_LAYERSHELL
    auto *app = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
    auto *wayland = app ? app->nativeInterface<QNativeInterface::QWaylandApplication>() : nullptr;
    wl_display *display = wayland ? wayland->display() : nullptr;
    if (!display)
        return false;
    // A private queue and a display wrapper: Qt's own queue and its event
    // thread are not touched.
    wl_event_queue *queue = wl_display_create_queue(display);
    auto *wrapper = static_cast<wl_display *>(wl_proxy_create_wrapper(display));
    if (!queue || !wrapper) {
        if (wrapper)
            wl_proxy_wrapper_destroy(wrapper);
        if (queue)
            wl_event_queue_destroy(queue);
        return false;
    }
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapper), queue);
    wl_registry *registry = wl_display_get_registry(wrapper);
    bool found = false;
    static const wl_registry_listener listener {
        [](void *data, wl_registry *, uint32_t, const char *interface, uint32_t) {
            if (std::strcmp(interface, "zwlr_layer_shell_v1") == 0)
                *static_cast<bool *>(data) = true;
        },
        [](void *, wl_registry *, uint32_t) {},
    };
    wl_registry_add_listener(registry, &listener, &found);
    wl_display_roundtrip_queue(display, queue);
    wl_registry_destroy(registry);
    wl_proxy_wrapper_destroy(wrapper);
    wl_event_queue_destroy(queue);
    return found;
#else
    return false;
#endif
}

bool GuideOverlayHost::show()
{
    if (!m_available || !m_host)
        return false;
    if (m_window)
        return true;
#ifdef MIGHTYOS_HAVE_LAYERSHELL
    QQmlComponent component(m_engine);
    component.loadFromModule("MightyOS.UI", "GuideOverlay");
    QObject *object = component.isReady()
        ? component.createWithInitialProperties({ { QStringLiteral("host"), QVariant::fromValue<QObject *>(m_host) } })
        : nullptr;
    auto *window = qobject_cast<QQuickWindow *>(object);
    if (!window) {
        qCWarning(lcOverlay).noquote() << "Guide overlay not created:" << component.errorString();
        delete object;
        m_available = false;   // don't try again; QML uses the fallback
        return false;
    }
    // Must be set up before the platform window exists (first show()).
    auto *layer = LayerShellQt::Window::get(window);
    layer->setLayer(LayerShellQt::Window::LayerOverlay);
    layer->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorBottom
                                                    | LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight));
    layer->setExclusiveZone(-1);   // cover everything, ignore panels
    layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityExclusive);
    layer->setScope(QStringLiteral("mightyos-guide"));
    if (m_host->screen())
        layer->setScreen(m_host->screen());
    window->resize(m_host->size());
    m_window = window;
    // Closed by the compositor (layer surface dismissed, output gone):
    // treat it as hidden, so QML stops routing input to the menu.
    connect(window, &QWindow::visibleChanged, this, [this, window](bool visible) {
        if (!visible && m_window == window)
            hide();
    });
    if (m_router)
        m_router->addWindow(window);
    window->show();
    window->requestActivate();
    qCInfo(lcOverlay) << "Guide overlay shown";
    emit shownChanged();
    return true;
#else
    return false;
#endif
}

void GuideOverlayHost::hide()
{
    if (!m_window)
        return;
    QQuickWindow *window = m_window;
    m_window = nullptr;
    if (m_router)
        m_router->removeWindow(window);
    window->disconnect(this);
    window->hide();
    // Its scene graph, textures and render thread go away with it.
    window->deleteLater();
    qCInfo(lcOverlay) << "Guide overlay hidden";
    emit shownChanged();
}
