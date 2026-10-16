#pragma once
#include <QObject>
#include <QSize>
#include <memory>

#include "../../libretro/bridge_proto.h"

class QOpenGLContext;
class QOffscreenSurface;
class QOpenGLFramebufferObject;
class QQuickRenderControl;
class QQuickWindow;
class QSocketNotifier;
class InputManager;
class TickAnimationDriver;

// 240-MP's side of the ReplayOS core (libretro/core.c): when the app is
// started by that core, the frontend owns the display, audio and controllers,
// so the UI is rendered off-screen here and handed over frame by frame.
//
// - The QML scene (Main.qml's items, moved out of its never-shown Window) is
//   rendered with QQuickRenderControl into an FBO on the GPU, read back and
//   published to the shared-memory triple buffer.
// - The core's TICK (one per CRT frame) drives everything: input, animations
//   (advanced by exactly one frame, so motion is locked to the CRT) and a
//   render when the scene changed.
// - RetroPad state becomes SDL controller ids for InputManager, and frontend
//   keyboard events become Qt key events.
//
// Protocol and memory layout: libretro/bridge_proto.h.
class LibretroBridge : public QObject {
    Q_OBJECT
public:
    // True when this process was started by the ReplayOS core.
    static bool requested();
    // Must run before QGuiApplication: GL ES context format and Qt Quick API.
    static void configureGraphics();

    explicit LibretroBridge(InputManager *input, QObject *parent = nullptr);
    ~LibretroBridge() override;

    // Attach to the core's socket and shared memory and create the GL
    // context. False (with a logged reason) when the bridge can't run.
    bool open();

    // Takes over the root Window's items; that window is never shown.
    void attach(QQuickWindow *rootWindow);

    QQuickWindow *window() const { return m_window; }

signals:
    // The core is unloading (QUIT) or gone (socket closed).
    void quitRequested();

private:
    void onSocketReadable();
    void applyMode(const QSize &size);
    void onTick(const mp240_msg &msg);
    void handleKey(const mp240_msg &msg);
    void renderFrame();
    void publishFrame();

    InputManager *m_input;
    int m_sock = -1;
    mp240_shm *m_shm = nullptr;
    uint32_t m_back = 2;           // UI layer buffer we render into
    QSocketNotifier *m_notifier = nullptr;

    std::unique_ptr<QOpenGLContext> m_context;
    std::unique_ptr<QOffscreenSurface> m_surface;
    std::unique_ptr<QOpenGLFramebufferObject> m_fbo;
    std::unique_ptr<QQuickRenderControl> m_renderControl;
    QQuickWindow *m_window = nullptr;     // owned, created with m_renderControl
    QQuickWindow *m_rootWindow = nullptr; // Main.qml's Window, kept hidden
    TickAnimationDriver *m_animationDriver = nullptr;
    bool m_bgraReadback = false;

    QSize m_size;
    bool m_initialized = false;
    bool m_sceneDirty = true;

    uint16_t m_lastJoypad[MP240_PORTS] = {};
    int16_t m_lastAnalog[MP240_PORTS][4] = {};
};
