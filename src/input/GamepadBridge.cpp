#include "input/GamepadBridge.h"

#include <QDebug>
#include <QHash>

#ifdef BSFCHAT_HAVE_SDL_GAMEPAD
#include <SDL3/SDL.h>
// NOT SDL3/SDL_main.h, ever. It #defines main, and this process already
// has one in src/main.cpp that Qt needs to own.
#endif

namespace bsfchat::input {

#ifdef BSFCHAT_HAVE_SDL_GAMEPAD

// The constant GamepadDeadzone.h restates so it does not have to include
// SDL. Checked here, where SDL's header is available, so the two cannot
// drift apart across an SDL version bump without the build failing.
static_assert(kAxisMax == SDL_JOYSTICK_AXIS_MAX,
              "GamepadDeadzone.h's kAxisMax no longer matches SDL's "
              "SDL_JOYSTICK_AXIS_MAX; every deadzone constant in that "
              "header is expressed in those units.");

namespace {

// Open gamepad handles, keyed by instance id. SDL will not deliver
// events for a device that has not been opened, so this map existing is
// what makes the whole thing work; it is also what has to be closed on
// removal and on shutdown.
QHash<SDL_JoystickID, SDL_Gamepad*>& openPads()
{
    static QHash<SDL_JoystickID, SDL_Gamepad*> pads;
    return pads;
}

// Explicit translation, no arithmetic on enum values.
//
// SDL's numbering is not ours and must not be assumed to be stable: a
// future SDL could insert a button and renumber everything after it.
// Anything unrecognised becomes Unknown and is dropped by the registry,
// which is the correct failure — a new button arriving as a wrong-but-
// valid one would fire whatever that value is bound to.
GamepadButton translateButton(Uint8 sdl)
{
    switch (sdl) {
    case SDL_GAMEPAD_BUTTON_SOUTH:          return GamepadButton::South;
    case SDL_GAMEPAD_BUTTON_EAST:           return GamepadButton::East;
    case SDL_GAMEPAD_BUTTON_WEST:           return GamepadButton::West;
    case SDL_GAMEPAD_BUTTON_NORTH:          return GamepadButton::North;
    case SDL_GAMEPAD_BUTTON_BACK:           return GamepadButton::Back;
    case SDL_GAMEPAD_BUTTON_GUIDE:          return GamepadButton::Guide;
    case SDL_GAMEPAD_BUTTON_START:          return GamepadButton::Start;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:     return GamepadButton::LeftStick;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:    return GamepadButton::RightStick;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  return GamepadButton::LeftShoulder;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return GamepadButton::RightShoulder;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:        return GamepadButton::DpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:      return GamepadButton::DpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:      return GamepadButton::DpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:     return GamepadButton::DpadRight;
    case SDL_GAMEPAD_BUTTON_MISC1:          return GamepadButton::Misc1;
    case SDL_GAMEPAD_BUTTON_TOUCHPAD:       return GamepadButton::Touchpad;
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1:   return GamepadButton::LeftPaddle1;
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2:   return GamepadButton::LeftPaddle2;
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1:  return GamepadButton::RightPaddle1;
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2:  return GamepadButton::RightPaddle2;
    default:                                return GamepadButton::Unknown;
    }
}

GamepadAxis translateAxis(Uint8 sdl)
{
    switch (sdl) {
    case SDL_GAMEPAD_AXIS_LEFTX:         return GamepadAxis::LeftX;
    case SDL_GAMEPAD_AXIS_LEFTY:         return GamepadAxis::LeftY;
    case SDL_GAMEPAD_AXIS_RIGHTX:        return GamepadAxis::RightX;
    case SDL_GAMEPAD_AXIS_RIGHTY:        return GamepadAxis::RightY;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:  return GamepadAxis::LeftTrigger;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return GamepadAxis::RightTrigger;
    default:                             return GamepadAxis::Unknown;
    }
}

} // namespace

#endif // BSFCHAT_HAVE_SDL_GAMEPAD

GamepadBridge::GamepadBridge(QObject* parent)
    : QObject(parent)
{
    m_clock.start();

    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setSingleShot(false);
    connect(&m_timer, &QTimer::timeout, this, &GamepadBridge::poll);

#ifdef BSFCHAT_HAVE_SDL_GAMEPAD
    // Deliver controller input even when no window of ours has focus.
    //
    // This is the single most important hint for this application. A chat
    // client used while gaming is, by definition, almost never the
    // focused application — the game is. A push-to-talk button that only
    // works when BSFChat is in front is a push-to-talk button that never
    // works. SDL's default is to suppress joystick events when unfocused,
    // which is right for a game and exactly wrong here.
    //
    // Without SDL_INIT_VIDEO there is no SDL window and therefore no
    // focus concept, so today this hint changes nothing. It is set
    // explicitly anyway so that the behaviour is a decision on the record
    // rather than an accident of which subsystems happen to be enabled.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    // GAMEPAD and nothing else. See the header for why pulling in more
    // of SDL would be actively harmful rather than merely wasteful.
    // SDL_INIT_GAMEPAD implies the joystick and event subsystems.
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        // Not fatal and not a user-visible error: a machine with no
        // controller support is a machine where this feature is absent,
        // not broken. `available` stays false and the timer never starts.
        qWarning() << "Gamepad: SDL_Init(SDL_INIT_GAMEPAD) failed:"
                   << SDL_GetError();
        return;
    }
    m_sdlReady = true;
    qInfo() << "Gamepad: SDL" << SDL_GetVersion() << "gamepad subsystem up";

    // Enumerate what is already attached.
    //
    // SDL does queue an ADDED event for each device present at init, so
    // the first poll would find them anyway. This runs regardless for two
    // reasons: it makes `connected` correct before the first timer tick
    // rather than up to 250 ms later (so a UI built at startup does not
    // flash an empty state), and GamepadRegistry::add() ignores a
    // duplicate id, so the ADDED events that follow are harmless.
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; ++i) {
            GamepadEvent e;
            e.kind = GamepadEvent::Kind::Added;
            e.instanceId = ids[i];
            if (SDL_Gamepad* pad = SDL_OpenGamepad(ids[i])) {
                openPads().insert(ids[i], pad);
                const char* name = SDL_GetGamepadName(pad);
                e.name = name ? QString::fromUtf8(name)
                              : QStringLiteral("Controller");
            } else {
                // Could not open it: do not register a pad we cannot
                // receive events from, or `connected` would be true
                // forever with nothing behind it.
                qWarning() << "Gamepad: SDL_OpenGamepad failed:"
                           << SDL_GetError();
                continue;
            }
            dispatch(m_core.handle(e, nowMs()));
        }
        SDL_free(ids);
    }

    rescheduleTimer();
    m_timer.start();
#endif
}

GamepadBridge::~GamepadBridge()
{
    m_timer.stop();
#ifdef BSFCHAT_HAVE_SDL_GAMEPAD
    if (!m_sdlReady) return;
    for (auto it = openPads().constBegin(); it != openPads().constEnd(); ++it)
        SDL_CloseGamepad(it.value());
    openPads().clear();
    // QuitSubSystem rather than SDL_Quit: symmetry with initialising only
    // one subsystem. SDL_Quit would also tear down anything a future
    // caller had initialised separately.
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
#endif
}

bool GamepadBridge::available() const
{
#ifdef BSFCHAT_HAVE_SDL_GAMEPAD
    return m_sdlReady;
#else
    return false;
#endif
}

qint64 GamepadBridge::nowMs() const
{
    // QElapsedTimer, not QDateTime: this clock is used for repeat
    // cadence, and a monotonic source is the difference between a
    // correct repeat and one that stalls or stampedes when the system
    // clock is stepped (NTP, a timezone change, a laptop waking).
    return m_clock.elapsed();
}

void GamepadBridge::poll()
{
#ifdef BSFCHAT_HAVE_SDL_GAMEPAD
    if (!m_sdlReady) return;

    const qint64 now = nowMs();

    // Drain the queue completely rather than capping the loop.
    //
    // A cap would be the defensive-looking choice, but the event this
    // layer cannot afford to drop is a button-up: dropping one leaves a
    // button logically held, which for push-to-talk is an open
    // microphone. A pad producing enough events to make this loop long
    // would have to be faulty, and a faulty pad making the UI briefly
    // sluggish is a far better outcome than a faulty pad leaving the mic
    // open. SDL_PollEvent also pumps, which is what actually samples the
    // devices.
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        GamepadEvent e;
        switch (ev.type) {
        case SDL_EVENT_GAMEPAD_ADDED: {
            e.kind = GamepadEvent::Kind::Added;
            e.instanceId = ev.gdevice.which;
            if (openPads().contains(ev.gdevice.which)) continue;
            SDL_Gamepad* pad = SDL_OpenGamepad(ev.gdevice.which);
            if (!pad) {
                qWarning() << "Gamepad: SDL_OpenGamepad failed:"
                           << SDL_GetError();
                continue;
            }
            openPads().insert(ev.gdevice.which, pad);
            const char* name = SDL_GetGamepadName(pad);
            e.name = name ? QString::fromUtf8(name)
                          : QStringLiteral("Controller");
            break;
        }
        case SDL_EVENT_GAMEPAD_REMOVED: {
            e.kind = GamepadEvent::Kind::Removed;
            e.instanceId = ev.gdevice.which;
            const auto it = openPads().find(ev.gdevice.which);
            if (it != openPads().end()) {
                SDL_CloseGamepad(it.value());
                openPads().erase(it);
            }
            break;
        }
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            e.kind = (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
                ? GamepadEvent::Kind::ButtonDown
                : GamepadEvent::Kind::ButtonUp;
            e.instanceId = ev.gbutton.which;
            e.button = translateButton(ev.gbutton.button);
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            e.kind = GamepadEvent::Kind::AxisMotion;
            e.instanceId = ev.gaxis.which;
            e.axis = translateAxis(ev.gaxis.axis);
            e.raw = ev.gaxis.value;
            break;
        default:
            // Everything else SDL might queue. Consumed (so the queue
            // drains) and ignored.
            continue;
        }
        dispatch(m_core.handle(e, now));
    }

    // Auto-repeat. Runs every poll, including polls where no event
    // arrived — that is the entire point of it.
    dispatch(m_core.tick(now));

    rescheduleTimer();
#endif
}

void GamepadBridge::rescheduleTimer()
{
    const int interval = m_core.pollIntervalMs(nowMs());
    if (m_timer.interval() != interval) m_timer.setInterval(interval);
}

void GamepadBridge::dispatch(const QList<GamepadAction>& actions)
{
    for (const GamepadAction& a : actions) {
        switch (a.kind) {
        case GamepadAction::Kind::Connected:
        case GamepadAction::Kind::Disconnected:
            // connectedChanged also notifies controllerCount.
            break;
        case GamepadAction::Kind::PrimaryChanged:
            break;
        case GamepadAction::Kind::ButtonPressed:
            emit buttonPressed(a.button);
            break;
        case GamepadAction::Kind::ButtonReleased:
            emit buttonReleased(a.button);
            break;
        case GamepadAction::Kind::Navigate:
            emit navigate(a.direction, a.repeat);
            break;
        case GamepadAction::Kind::AxisChanged:
            emit axisChanged(a.axis, a.value);
            break;
        }
    }

    // Property notifications are derived from the resulting state rather
    // than from the Connected/Disconnected actions, so that they are
    // emitted exactly once per real transition however many actions a
    // single event produced. An unplug that synthesises three button
    // releases and a Disconnected must not emit connectedChanged four
    // times, and a QML binding that re-reads on every notify would
    // otherwise do four times the work for one event.
    if (m_core.connected() != m_wasConnected) {
        m_wasConnected = m_core.connected();
        emit connectedChanged();
    }
    if (m_core.primaryName() != m_lastName) {
        m_lastName = m_core.primaryName();
        emit controllerNameChanged();
    }
}

void GamepadBridge::injectForTest(const GamepadEvent& event, qint64 nowMs)
{
    dispatch(m_core.handle(event, nowMs));
}

void GamepadBridge::tickForTest(qint64 nowMs)
{
    dispatch(m_core.tick(nowMs));
}

} // namespace bsfchat::input
