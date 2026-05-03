#include "input_inject.h"
#include "logging.h"
#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#include <stdlib.h>
#include <stdio.h>

#define TAG "Input"
#define DOUBLE_CLICK_INTERVAL_SEC 0.5
#define DOUBLE_CLICK_DISTANCE_PX 4

static uint8_t last_button_mask = 0;
static bool accessibility_checked = false;
static bool accessibility_granted = false;
static CGEventFlags current_modifier_flags = 0;
static CFAbsoluteTime last_left_click_time = 0.0;
static CGPoint last_left_click_point = {0.0, 0.0};
static int64_t last_left_click_count = 0;

// Check and request accessibility permission
static bool check_accessibility(void) {
    if (!accessibility_checked) {
        // This call will prompt the user if not already trusted.
        const void *keys[] = { kAXTrustedCheckOptionPrompt };
        const void *values[] = { kCFBooleanTrue };
        CFDictionaryRef options = CFDictionaryCreate(
            kCFAllocatorDefault,
            keys,
            values,
            1,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks
        );

        accessibility_granted = AXIsProcessTrustedWithOptions(options);
        if (options) {
            CFRelease(options);
        }
        accessibility_checked = true;
        if (accessibility_granted) {
            LOG_INFO(TAG, "Accessibility permission granted");
        } else {
            LOG_ERROR(TAG, "Accessibility permission NOT granted - input injection will not work. "
                      "Grant access in System Settings > Privacy & Security > Accessibility");
        }
    }
    return accessibility_granted;
}

// Re-check periodically in case user granted permission after initial check
static bool ensure_accessibility(void) {
    if (accessibility_granted) return true;
    accessibility_granted = AXIsProcessTrusted();
    if (accessibility_granted) {
        LOG_INFO(TAG, "Accessibility permission now granted");
    }
    return accessibility_granted;
}

static CGKeyCode keysym_to_keycode(uint32_t keysym) {
    switch (keysym) {
        case 0xFF08: return kVK_Delete;
        case 0xFF09: return kVK_Tab;
        case 0xFF0D: return kVK_Return;
        case 0xFF1B: return kVK_Escape;
        case 0xFF63: return kVK_Help;
        case 0xFFFF: return kVK_ForwardDelete;
        case 0xFF50: return kVK_Home;
        case 0xFF57: return kVK_End;
        case 0xFF55: return kVK_PageUp;
        case 0xFF56: return kVK_PageDown;
        case 0xFF51: return kVK_LeftArrow;
        case 0xFF52: return kVK_UpArrow;
        case 0xFF53: return kVK_RightArrow;
        case 0xFF54: return kVK_DownArrow;
        case 0xFFE1: case 0xFFE2: return kVK_Shift;
        case 0xFFE3: case 0xFFE4: return kVK_Control;
        case 0xFFE7: case 0xFFE8: return kVK_Command;
        case 0xFFE9: case 0xFFEA: return kVK_Option;
        case 0xFFE5: return kVK_CapsLock;
        case 0xFFBE: return kVK_F1;
        case 0xFFBF: return kVK_F2;
        case 0xFFC0: return kVK_F3;
        case 0xFFC1: return kVK_F4;
        case 0xFFC2: return kVK_F5;
        case 0xFFC3: return kVK_F6;
        case 0xFFC4: return kVK_F7;
        case 0xFFC5: return kVK_F8;
        case 0xFFC6: return kVK_F9;
        case 0xFFC7: return kVK_F10;
        case 0xFFC8: return kVK_F11;
        case 0xFFC9: return kVK_F12;
        case 0x0020: return kVK_Space;

        // Numeric keypad
        case 0xFF8D: return kVK_ANSI_KeypadEnter;
        case 0xFFAA: return kVK_ANSI_KeypadMultiply;
        case 0xFFAB: return kVK_ANSI_KeypadPlus;
        case 0xFFAD: return kVK_ANSI_KeypadMinus;
        case 0xFFAE: return kVK_ANSI_KeypadDecimal;
        case 0xFFAF: return kVK_ANSI_KeypadDivide;
        case 0xFFB0: return kVK_ANSI_Keypad0;
        case 0xFFB1: return kVK_ANSI_Keypad1;
        case 0xFFB2: return kVK_ANSI_Keypad2;
        case 0xFFB3: return kVK_ANSI_Keypad3;
        case 0xFFB4: return kVK_ANSI_Keypad4;
        case 0xFFB5: return kVK_ANSI_Keypad5;
        case 0xFFB6: return kVK_ANSI_Keypad6;
        case 0xFFB7: return kVK_ANSI_Keypad7;
        case 0xFFB8: return kVK_ANSI_Keypad8;
        case 0xFFB9: return kVK_ANSI_Keypad9;
        case 0xFFBD: return kVK_ANSI_KeypadEquals;

        case 'a': case 'A': return kVK_ANSI_A;
        case 'b': case 'B': return kVK_ANSI_B;
        case 'c': case 'C': return kVK_ANSI_C;
        case 'd': case 'D': return kVK_ANSI_D;
        case 'e': case 'E': return kVK_ANSI_E;
        case 'f': case 'F': return kVK_ANSI_F;
        case 'g': case 'G': return kVK_ANSI_G;
        case 'h': case 'H': return kVK_ANSI_H;
        case 'i': case 'I': return kVK_ANSI_I;
        case 'j': case 'J': return kVK_ANSI_J;
        case 'k': case 'K': return kVK_ANSI_K;
        case 'l': case 'L': return kVK_ANSI_L;
        case 'm': case 'M': return kVK_ANSI_M;
        case 'n': case 'N': return kVK_ANSI_N;
        case 'o': case 'O': return kVK_ANSI_O;
        case 'p': case 'P': return kVK_ANSI_P;
        case 'q': case 'Q': return kVK_ANSI_Q;
        case 'r': case 'R': return kVK_ANSI_R;
        case 's': case 'S': return kVK_ANSI_S;
        case 't': case 'T': return kVK_ANSI_T;
        case 'u': case 'U': return kVK_ANSI_U;
        case 'v': case 'V': return kVK_ANSI_V;
        case 'w': case 'W': return kVK_ANSI_W;
        case 'x': case 'X': return kVK_ANSI_X;
        case 'y': case 'Y': return kVK_ANSI_Y;
        case 'z': case 'Z': return kVK_ANSI_Z;
        case '0': case ')': return kVK_ANSI_0;
        case '1': case '!': return kVK_ANSI_1;
        case '2': case '@': return kVK_ANSI_2;
        case '3': case '#': return kVK_ANSI_3;
        case '4': case '$': return kVK_ANSI_4;
        case '5': case '%': return kVK_ANSI_5;
        case '6': case '^': return kVK_ANSI_6;
        case '7': case '&': return kVK_ANSI_7;
        case '8': case '*': return kVK_ANSI_8;
        case '9': case '(': return kVK_ANSI_9;
        case '-': case '_': return kVK_ANSI_Minus;
        case '=': case '+': return kVK_ANSI_Equal;
        case '[': case '{': return kVK_ANSI_LeftBracket;
        case ']': case '}': return kVK_ANSI_RightBracket;
        case '\\': case '|': return kVK_ANSI_Backslash;
        case ';': case ':': return kVK_ANSI_Semicolon;
        case '\'': case '"': return kVK_ANSI_Quote;
        case ',': case '<': return kVK_ANSI_Comma;
        case '.': case '>': return kVK_ANSI_Period;
        case '/': case '?': return kVK_ANSI_Slash;
        case '`': case '~': return kVK_ANSI_Grave;

        default: return (CGKeyCode)-1;
    }
}

void input_inject_mouse_move(uint16_t x, uint16_t y,
                              uint32_t display_width __attribute__((unused)),
                              uint32_t display_height __attribute__((unused))) {
    if (!ensure_accessibility()) return;

    CGPoint point = CGPointMake((CGFloat)x, (CGFloat)y);

    CGEventType type = kCGEventMouseMoved;
    CGMouseButton button = kCGMouseButtonLeft;

    if (last_button_mask & 0x01) {
        type = kCGEventLeftMouseDragged;
        button = kCGMouseButtonLeft;
    } else if (last_button_mask & 0x04) {
        type = kCGEventOtherMouseDragged;
        button = kCGMouseButtonCenter;
    } else if (last_button_mask & 0x02) {
        type = kCGEventRightMouseDragged;
        button = kCGMouseButtonRight;
    }

    CGEventRef event = CGEventCreateMouseEvent(NULL, type, point, button);
    if (event) {
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
    }
}

void input_inject_mouse_button(uint16_t x, uint16_t y, uint8_t button_mask,
                                uint32_t display_width __attribute__((unused)),
                                uint32_t display_height __attribute__((unused))) {
    if (!ensure_accessibility()) return;

    CGPoint point = CGPointMake((CGFloat)x, (CGFloat)y);
    uint8_t changed = button_mask ^ last_button_mask;

    // Left button (bit 0)
    if (changed & 0x01) {
        CGEventType type = (button_mask & 0x01) ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
        CGEventRef event = CGEventCreateMouseEvent(NULL, type, point, kCGMouseButtonLeft);
        if (event) {
            if (button_mask & 0x01) {
                CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
                bool within_time = (now - last_left_click_time) <= DOUBLE_CLICK_INTERVAL_SEC;
                bool within_distance =
                    abs((int)x - (int)last_left_click_point.x) <= DOUBLE_CLICK_DISTANCE_PX &&
                    abs((int)y - (int)last_left_click_point.y) <= DOUBLE_CLICK_DISTANCE_PX;

                if (within_time && within_distance && last_left_click_count < 2)
                    last_left_click_count++;
                else
                    last_left_click_count = 1;

                last_left_click_time = now;
                last_left_click_point = point;
            }

            CGEventSetIntegerValueField(event, kCGMouseEventClickState, last_left_click_count);
            CGEventPost(kCGHIDEventTap, event);
            CFRelease(event);
            LOG_DEBUG(TAG, "Mouse %s at (%d,%d)",
                      (button_mask & 0x01) ? "LEFT_DOWN" : "LEFT_UP", x, y);
        }
    }

    // Middle button (bit 1) - RFB bit 1 = middle
    if (changed & 0x02) {
        CGEventType type = (button_mask & 0x02) ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
        CGEventRef event = CGEventCreateMouseEvent(NULL, type, point, kCGMouseButtonCenter);
        if (event) {
            CGEventPost(kCGHIDEventTap, event);
            CFRelease(event);
        }
    }

    // Right button (bit 2) - RFB bit 2 = right
    if (changed & 0x04) {
        CGEventType type = (button_mask & 0x04) ? kCGEventRightMouseDown : kCGEventRightMouseUp;
        CGEventRef event = CGEventCreateMouseEvent(NULL, type, point, kCGMouseButtonRight);
        if (event) {
            CGEventPost(kCGHIDEventTap, event);
            CFRelease(event);
        }
    }

    // Scroll wheel: RFB bit 3 = scroll up, bit 4 = scroll down
    if (button_mask & 0x08) {
        CGEventRef event = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine, 1, -3);
        if (event) { CGEventPost(kCGHIDEventTap, event); CFRelease(event); }
    }
    if (button_mask & 0x10) {
        CGEventRef event = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine, 1, 3);
        if (event) { CGEventPost(kCGHIDEventTap, event); CFRelease(event); }
    }

    last_button_mask = button_mask;
}

static CGEventFlags keysym_to_modifier_flag(uint32_t keysym) {
    switch (keysym) {
        case 0xFFE1: case 0xFFE2: return kCGEventFlagMaskShift;
        case 0xFFE3: case 0xFFE4: return kCGEventFlagMaskControl;
        case 0xFFE7: case 0xFFE8: return kCGEventFlagMaskCommand;
        case 0xFFE9: case 0xFFEA: return kCGEventFlagMaskAlternate;
        case 0xFFE5:              return kCGEventFlagMaskAlphaShift;
        default: return 0;
    }
}

void input_inject_key_event(uint32_t keysym, bool down) {
    if (!ensure_accessibility()) return;

    CGKeyCode keycode = keysym_to_keycode(keysym);
    if (keycode == (CGKeyCode)-1) {
        LOG_DEBUG(TAG, "Unknown keysym: 0x%04X (ignored)", keysym);
        return;
    }

    CGEventFlags flag = keysym_to_modifier_flag(keysym);
    if (flag) {
        if (down)
            current_modifier_flags |= flag;
        else
            current_modifier_flags &= ~flag;
    }

    CGEventRef event = CGEventCreateKeyboardEvent(NULL, keycode, down);
    if (event) {
        CGEventSetFlags(event, current_modifier_flags);
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
    }
}

void input_inject_init(void) {
    check_accessibility();
}
