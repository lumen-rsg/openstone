#include "SchematicModel.hpp"

#include <QCoreApplication>
#include <QKeyEvent>

#include <algorithm>

namespace {

std::int32_t virtualKey(const QKeyEvent& event)
{
#ifdef Q_OS_WIN
    if (const auto native = event.nativeVirtualKey(); native > 0 && native <= 0xff) return native;
#endif

    const int key = event.key();
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) return 0x70 + key - Qt::Key_F1;
    if (event.modifiers().testFlag(Qt::KeypadModifier)) {
        if (key >= Qt::Key_0 && key <= Qt::Key_9) return 0x60 + key - Qt::Key_0;
        switch (key) {
        case Qt::Key_Asterisk: return 0x6a;
        case Qt::Key_Plus: return 0x6b;
        case Qt::Key_Comma: return 0x6c;
        case Qt::Key_Minus: return 0x6d;
        case Qt::Key_Period: return 0x6e;
        case Qt::Key_Slash: return 0x6f;
        default: break;
        }
    }

#ifdef Q_OS_MACOS
    const bool swapControlAndMeta = !QCoreApplication::testAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
#else
    constexpr bool swapControlAndMeta = false;
#endif
    switch (key) {
    case Qt::Key_Backspace: return 0x08;
    case Qt::Key_Tab: case Qt::Key_Backtab: return 0x09;
    case Qt::Key_Clear: return 0x0c;
    case Qt::Key_Return: case Qt::Key_Enter: return 0x0d;
    case Qt::Key_Shift: return 0x10;
    case Qt::Key_Control: return swapControlAndMeta ? 0x5b : 0x11;
    case Qt::Key_Meta: return swapControlAndMeta ? 0x11 : 0x5b;
    case Qt::Key_Alt: return 0x12;
    case Qt::Key_AltGr: return 0xa5;
    case Qt::Key_Pause: return 0x13;
    case Qt::Key_CapsLock: return 0x14;
    case Qt::Key_Escape: return 0x1b;
    case Qt::Key_PageUp: return 0x21;
    case Qt::Key_PageDown: return 0x22;
    case Qt::Key_End: return 0x23;
    case Qt::Key_Home: return 0x24;
    case Qt::Key_Left: return 0x25;
    case Qt::Key_Up: return 0x26;
    case Qt::Key_Right: return 0x27;
    case Qt::Key_Down: return 0x28;
    case Qt::Key_Print: return 0x2c;
    case Qt::Key_Insert: return 0x2d;
    case Qt::Key_Delete: return 0x2e;
    case Qt::Key_Help: return 0x2f;
    case Qt::Key_Super_L: return 0x5b;
    case Qt::Key_Super_R: return 0x5c;
    case Qt::Key_Menu: return 0x5d;
    case Qt::Key_NumLock: return 0x90;
    case Qt::Key_ScrollLock: return 0x91;
    default: return openstone::asciiVirtualKey(key);
    }
}

std::int32_t keyFlags(const QKeyEvent& event, bool pressed)
{
    // Win32 packs scan code, extended, Alt context, previous down, and release in the high word.
    int flags = pressed ? (event.isAutoRepeat() ? 0x4000 : 0) : 0xc000;
    if (event.modifiers().testFlag(Qt::AltModifier)) flags |= 0x2000;
#ifdef Q_OS_WIN
    flags |= static_cast<int>(event.nativeScanCode() & 0xff);
    if (event.nativeModifiers() & 0x01000000) flags |= 0x0100;
#else
    const int key = event.key();
    const bool keypad = event.modifiers().testFlag(Qt::KeypadModifier);
    const bool navigation = key == Qt::Key_Insert || key == Qt::Key_Delete ||
                            (key >= Qt::Key_Home && key <= Qt::Key_PageDown);
    if ((!keypad && navigation) || (keypad && (key == Qt::Key_Enter || key == Qt::Key_Slash)) ||
        key == Qt::Key_AltGr) {
        flags |= 0x0100;
    }
#endif
    return flags;
}

}

void SchematicModel::handleKeyboardEvent(const QKeyEvent& event)
{
    const bool pressed = event.type() == QEvent::KeyPress;
    // Some platforms send a release/press pair during auto-repeat.
    if (!pressed && event.isAutoRepeat()) return;
    const auto code = virtualKey(event);
    if (!code) return;
    const int repeat = event.isAutoRepeat() ? std::max(1, event.count()) : 1;
    const auto text = event.text();
    // Char is one character even when Qt combines several repeat events.
    const auto character = text.left(!text.isEmpty() && text.front().isHighSurrogate() ? 2 : 1).toStdString();
    try {
        if (graph().sendKeyEvent({code, pressed, repeat, keyFlags(event, pressed), character})) {
            refreshValues();
        }
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
}
