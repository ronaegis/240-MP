#include "LibretroBridge.h"
#include "../input/InputManager.h"

#include <QAnimationDriver>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QPointer>
#include <QQuickGraphicsDevice>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSocketNotifier>
#include <QSurfaceFormat>

#include <cerrno>
#include <cstring>
#include <utility>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif

// Advances QML animations by exactly one CRT frame per TICK instead of by
// wall-clock time, so scrolling and fades step evenly on the CRT.
class TickAnimationDriver : public QAnimationDriver {
public:
    using QAnimationDriver::QAnimationDriver;
    void step(double ms) { m_elapsed += ms; advance(); }
    qint64 elapsed() const override { return qint64(m_elapsed); }
    double frameMs = 1000.0 / 59.94;
private:
    double m_elapsed = 0;
};

namespace {
// Each RetroPad is its own pseudo-device, clear of SDL's instance ids (>= 0)
// and InputManager's "none" (-1).
constexpr int kDeviceBase = -100;

// RETRO_DEVICE_ID_JOYPAD_* bit -> SDL_GameControllerButton, by position:
// RetroPad B is the south face button like SDL's A, A is east, Y west, X north.
// L2/R2 are analog triggers in SDL; -1 marks them for the axis path.
constexpr int kRetroToSdlButton[16] = {
    SDL_CONTROLLER_BUTTON_A,             // B
    SDL_CONTROLLER_BUTTON_X,             // Y
    SDL_CONTROLLER_BUTTON_BACK,          // SELECT
    SDL_CONTROLLER_BUTTON_START,         // START
    SDL_CONTROLLER_BUTTON_DPAD_UP,       // UP
    SDL_CONTROLLER_BUTTON_DPAD_DOWN,     // DOWN
    SDL_CONTROLLER_BUTTON_DPAD_LEFT,     // LEFT
    SDL_CONTROLLER_BUTTON_DPAD_RIGHT,    // RIGHT
    SDL_CONTROLLER_BUTTON_B,             // A
    SDL_CONTROLLER_BUTTON_Y,             // X
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  // L
    SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, // R
    -1,                                  // L2
    -1,                                  // R2
    SDL_CONTROLLER_BUTTON_LEFTSTICK,     // L3
    SDL_CONTROLLER_BUTTON_RIGHTSTICK,    // R3
};
constexpr int kRetroL2 = 12;
constexpr int kRetroR2 = 13;

constexpr int kSdlAxis[4] = {
    SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY,
    SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY,
};

// libretro RETROK_* (SDL 1.2 numbering) -> Qt::Key. Letters and digits are
// handled arithmetically in qtKeyForRetroKey().
int qtKeyForRetroKey(unsigned k) {
    switch (k) {
    case 8:   return Qt::Key_Backspace;
    case 9:   return Qt::Key_Tab;
    case 13:  return Qt::Key_Return;
    case 27:  return Qt::Key_Escape;
    case 32:  return Qt::Key_Space;
    case 127: return Qt::Key_Delete;
    case 271: return Qt::Key_Enter;
    case 273: return Qt::Key_Up;
    case 274: return Qt::Key_Down;
    case 275: return Qt::Key_Right;
    case 276: return Qt::Key_Left;
    case 277: return Qt::Key_Insert;
    case 278: return Qt::Key_Home;
    case 279: return Qt::Key_End;
    case 280: return Qt::Key_PageUp;
    case 281: return Qt::Key_PageDown;
    case 303: return Qt::Key_Shift;      // right shift: InputManager maps it to Back by scan code
    case 304: return Qt::Key_Shift;
    default: break;
    }
    if (k >= 'a' && k <= 'z') return Qt::Key_A + int(k - 'a');
    if (k >= '0' && k <= '9') return Qt::Key_0 + int(k - '0');
    if (k >= 282 && k <= 293) return Qt::Key_F1 + int(k - 282);
    return 0;
}
}

bool LibretroBridge::requested() {
    return qEnvironmentVariableIsSet("MP240_BRIDGE_SOCK") && qEnvironmentVariableIsSet("MP240_BRIDGE_SHM");
}

void LibretroBridge::configureGraphics() {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGLES);
    fmt.setVersion(3, 0);
    // The scene graph's own fallback surface uses the default format; a
    // context created with any other config fails to make it current.
    fmt.setDepthBufferSize(24);
    fmt.setStencilBufferSize(8);
    fmt.setAlphaBufferSize(8);
    QSurfaceFormat::setDefaultFormat(fmt);
}

LibretroBridge::LibretroBridge(InputManager *input, QObject *parent)
    : QObject(parent)
    , m_input(input)
{
}

LibretroBridge::~LibretroBridge() {
    // Hand the items back so they die with the QML engine, not with our window.
    if (m_rootWindow && m_window) {
        const auto items = m_window->contentItem()->childItems();
        for (QQuickItem *item : items)
            item->setParentItem(m_rootWindow->contentItem());
    }
    if (m_context && m_surface)
        m_context->makeCurrent(m_surface.get());
    delete m_window;
    m_renderControl.reset();
    m_fbo.reset();
    if (m_context)
        m_context->doneCurrent();
    if (m_shm)
        munmap(m_shm, MP240_SHM_SIZE);
    if (m_sock >= 0)
        ::close(m_sock);
}

bool LibretroBridge::open() {
    m_sock = qEnvironmentVariableIntValue("MP240_BRIDGE_SOCK");
    // Processes 240-MP starts must not hold the socket: the core watches it
    // to notice that 240-MP is gone. The shared memory descriptor (and the
    // MP240_BRIDGE_SHM variable naming it) stays inheritable on purpose: the
    // mpv stand-in (libretro/mpv_bridge.c) draws and plays through it.
    fcntl(m_sock, F_SETFD, FD_CLOEXEC);
    const int shmFd = qEnvironmentVariableIntValue("MP240_BRIDGE_SHM");
    void *p = mmap(nullptr, MP240_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0);
    if (p == MAP_FAILED) {
        qCritical("[bridge] cannot map shared memory: %s", strerror(errno));
        return false;
    }
    m_shm = static_cast<mp240_shm *>(p);
    if (m_shm->magic != MP240_BRIDGE_MAGIC || m_shm->version != MP240_BRIDGE_VERSION) {
        qCritical("[bridge] core protocol version %u, expected %u", m_shm->version, MP240_BRIDGE_VERSION);
        return false;
    }

    m_context = std::make_unique<QOpenGLContext>();
    m_context->setFormat(QSurfaceFormat::defaultFormat());
    if (!m_context->create()) {
        qCritical("[bridge] cannot create an OpenGL ES context");
        return false;
    }
    m_surface = std::make_unique<QOffscreenSurface>();
    m_surface->setFormat(m_context->format());
    m_surface->create();
    if (!m_context->makeCurrent(m_surface.get())) {
        qCritical("[bridge] cannot make the OpenGL ES context current");
        return false;
    }
    m_bgraReadback = m_context->hasExtension("GL_EXT_read_format_bgra");
    qInfo("[bridge] %s, %s readback", reinterpret_cast<const char *>(
              m_context->functions()->glGetString(GL_RENDERER)),
          m_bgraReadback ? "BGRA" : "RGBA");

    // Before any QQuickWindow exists, or the scene graph installs its own.
    // Main.qml's hidden Window still tries to later, which logs a harmless
    // "QUnifiedTimer: animation driver already installed...".
    m_animationDriver = new TickAnimationDriver(this);
    m_animationDriver->install();

    m_renderControl = std::make_unique<QQuickRenderControl>();
    m_window = new QQuickWindow(m_renderControl.get());
    m_window->setGraphicsDevice(QQuickGraphicsDevice::fromOpenGLContext(m_context.get()));
    connect(m_renderControl.get(), &QQuickRenderControl::renderRequested, this, [this] { m_sceneDirty = true; });
    connect(m_renderControl.get(), &QQuickRenderControl::sceneChanged, this, [this] { m_sceneDirty = true; });

    m_notifier = new QSocketNotifier(m_sock, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &LibretroBridge::onSocketReadable);
    return true;
}

void LibretroBridge::attach(QQuickWindow *rootWindow) {
    m_rootWindow = rootWindow;
    const auto items = rootWindow->contentItem()->childItems();
    for (QQuickItem *item : items)
        item->setParentItem(m_window->contentItem());
    m_window->setColor(rootWindow->color());
    connect(rootWindow, &QQuickWindow::colorChanged, m_window, &QQuickWindow::setColor);

    // The window never gets OS focus; give its focus chain a start so key
    // events reach the QML focus item.
    QFocusEvent focusIn(QEvent::FocusIn, Qt::OtherFocusReason);
    QCoreApplication::sendEvent(m_window, &focusIn);

    if (m_size.isValid()) {
        const QSize size = m_size;
        m_size = QSize();
        applyMode(size);
    }
}

void LibretroBridge::onSocketReadable() {
    bool ticked = false;
    for (;;) {
        mp240_msg msg;
        const ssize_t r = recv(m_sock, &msg, sizeof msg, MSG_DONTWAIT);
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (r <= 0) {
            qInfo("[bridge] core closed the connection");
            m_notifier->setEnabled(false);
            emit quitRequested();
            return;
        }
        switch (msg.type) {
        case MP240_MSG_MODE:
            qInfo("[bridge] mode %ux%u%s @ %.2f Hz", msg.u.mode.width, msg.u.mode.height,
                  msg.u.mode.interlaced ? "i" : "p", msg.u.mode.fps);
            if (msg.u.mode.fps > 0)
                m_animationDriver->frameMs = 1000.0 / msg.u.mode.fps;
            applyMode(QSize(int(qMin<uint32_t>(msg.u.mode.width, MP240_MAX_WIDTH)),
                            int(qMin<uint32_t>(msg.u.mode.height, MP240_MAX_HEIGHT))));
            break;
        case MP240_MSG_TICK:
            onTick(msg);
            ticked = true;
            break;
        case MP240_MSG_KEY:
            handleKey(msg);
            break;
        case MP240_MSG_QUIT:
            qInfo("[bridge] core is unloading");
            m_notifier->setEnabled(false);
            emit quitRequested();
            return;
        }
    }
    // Ticks that queued up while we were busy each advanced input and
    // animations; only the newest is worth a frame.
    if (ticked && m_sceneDirty && m_initialized && m_rootWindow) {
        renderFrame();
        publishFrame();
    }
}

void LibretroBridge::applyMode(const QSize &size) {
    if (size == m_size || size.isEmpty())
        return;
    m_size = size;
    if (!m_rootWindow)
        return;  // attach() applies it

    m_context->makeCurrent(m_surface.get());
    m_fbo = std::make_unique<QOpenGLFramebufferObject>(size);
    m_window->setGeometry(0, 0, size.width(), size.height());
    m_window->contentItem()->setSize(size);
    // Main.qml sizes everything from root.sw / root.sh, its Window's size.
    m_rootWindow->setWidth(size.width());
    m_rootWindow->setHeight(size.height());

    if (!m_initialized) {
        if (!m_renderControl->initialize()) {
            qCritical("[bridge] cannot initialize the Qt Quick renderer");
            return;
        }
        m_initialized = true;
    }
    QQuickRenderTarget target = QQuickRenderTarget::fromOpenGLTexture(m_fbo->texture(), size);
    // Top row first in glReadPixels, the order libretro wants.
    target.setMirrorVertically(true);
    m_window->setRenderTarget(target);
    m_sceneDirty = true;
}

void LibretroBridge::onTick(const mp240_msg &msg) {
    for (int port = 0; port < MP240_PORTS; ++port) {
        const int device = kDeviceBase - port;
        const uint16_t now = msg.u.tick.joypad[port];
        const uint16_t changed = now ^ m_lastJoypad[port];
        for (int bit = 0; bit < 16; ++bit) {
            if (!(changed & (1u << bit)))
                continue;
            const bool pressed = now & (1u << bit);
            if (bit == kRetroL2 || bit == kRetroR2)
                m_input->injectControllerAxis(device, bit == kRetroL2 ? SDL_CONTROLLER_AXIS_TRIGGERLEFT
                                                                      : SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
                                              pressed ? 32767 : 0);
            else
                m_input->injectControllerButton(device, kRetroToSdlButton[bit], pressed);
        }
        m_lastJoypad[port] = now;
        for (int axis = 0; axis < 4; ++axis) {
            const int16_t v = msg.u.tick.analog[port][axis];
            if (v != m_lastAnalog[port][axis])
                m_input->injectControllerAxis(device, kSdlAxis[axis], v);
            m_lastAnalog[port][axis] = v;
        }
    }
    m_animationDriver->step(m_animationDriver->frameMs);
}

void LibretroBridge::handleKey(const mp240_msg &msg) {
    const int key = qtKeyForRetroKey(msg.u.key.keycode);
    if (!key || !m_window)
        return;
    Qt::KeyboardModifiers mods;
    if (msg.u.key.modifiers & 0x01) mods |= Qt::ShiftModifier;    // RETROKMOD_SHIFT
    if (msg.u.key.modifiers & 0x02) mods |= Qt::ControlModifier;  // RETROKMOD_CTRL
    if (msg.u.key.modifiers & 0x04) mods |= Qt::AltModifier;      // RETROKMOD_ALT
    const QString text = msg.u.key.character ? QString(QChar(char32_t(msg.u.key.character))) : QString();
    // Right shift carries evdev's KEY_RIGHTSHIFT so InputManager treats it as Back.
    const quint32 scanCode = msg.u.key.keycode == 303 ? 54 : 0;
    QCoreApplication::postEvent(m_window, new QKeyEvent(msg.u.key.down ? QEvent::KeyPress : QEvent::KeyRelease,
                                                        key, mods, scanCode, 0, 0, text));
}

void LibretroBridge::renderFrame() {
    m_context->makeCurrent(m_surface.get());
    m_renderControl->polishItems();
    m_renderControl->beginFrame();
    m_renderControl->sync();
    m_renderControl->render();
    m_renderControl->endFrame();
    m_sceneDirty = false;
}

void LibretroBridge::publishFrame() {
    const int w = m_size.width(), h = m_size.height();
    auto *pixels = reinterpret_cast<uint8_t *>(m_shm) + MP240_FRAME_OFFSET(MP240_LAYER_UI, m_back);
    QOpenGLFunctions *gl = m_context->functions();

    m_fbo->bind();
    gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    gl->glReadPixels(0, 0, w, h, m_bgraReadback ? GL_BGRA_EXT : GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    m_fbo->release();
    if (!m_bgraReadback) {
        // RGBA bytes -> XRGB8888 (B, G, R, X in memory)
        for (int i = 0, n = w * h; i < n; ++i)
            std::swap(pixels[i * 4], pixels[i * 4 + 2]);
    }

    mp240_frame_info &info = m_shm->layer[MP240_LAYER_UI].frame[m_back];
    info.width = uint32_t(w);
    info.height = uint32_t(h);
    info.pitch = uint32_t(w) * 4;
    info.seq++;
    m_back = mp240_publish(m_shm, MP240_LAYER_UI, m_back);
}
