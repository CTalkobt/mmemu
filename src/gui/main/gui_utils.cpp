#include "gui_utils.h"
#include <wx/wx.h>

std::string wxKeyToVic20Name(int code) {
    if (code >= 'A' && code <= 'Z') return std::string(1, (char)('a' + code - 'A'));
    if (code >= 'a' && code <= 'z') return std::string(1, (char)code);
    if (code >= '0' && code <= '9') return std::string(1, (char)code);
    switch (code) {
        case WXK_SPACE:            return "space";
        case WXK_RETURN:           return "return";
        case WXK_CONTROL:          return "control";
        case WXK_ALT:              return "alt";
        case WXK_UP:               return "up";
        case WXK_DOWN:             return "down";
        case WXK_LEFT:             return "left";
        case WXK_RIGHT:            return "right";
        case WXK_HOME:             return "home";
        case WXK_DELETE:
        case WXK_BACK:             return "delete";
        case WXK_ESCAPE:           return "run_stop";
        case WXK_TAB:              return "tab";
        // All function keys — machine-specific onKey handlers remap as needed
        case WXK_F1:               return "f1";
        case WXK_F2:               return "f2";
        case WXK_F3:               return "f3";
        case WXK_F4:               return "f4";
        case WXK_F5:               return "f5";
        case WXK_F6:               return "f6";
        case WXK_F7:               return "f7";
        case WXK_F8:               return "f8";
        case WXK_F9:               return "f9";
        case WXK_F10:              return "f10";
        case WXK_F11:              return "f11";
        case WXK_F12:              return "f12";
        // Punctuation — unshifted only; shifted combos handled by shiftedPetKey()
        case ';':                  return "semicolon";
        case '=':                  return "kpeq";       // PET = key (row 9)
        case '-':                  return "minus";
        case ',':                  return "comma";
        case '.':                  return "period";
        case '/':                  return "slash";
        case '@':                  return "at";
        case '[':                  return "bracket_left";
        case ']':                  return "bracket_right";
        case '\'':                 return "squote";     // PET dedicated ' key
        // Numpad — map to same matrix positions as main keys
        case WXK_NUMPAD0:          return "0";
        case WXK_NUMPAD1:          return "1";
        case WXK_NUMPAD2:          return "2";
        case WXK_NUMPAD3:          return "3";
        case WXK_NUMPAD4:          return "4";
        case WXK_NUMPAD5:          return "5";
        case WXK_NUMPAD6:          return "6";
        case WXK_NUMPAD7:          return "7";
        case WXK_NUMPAD8:          return "8";
        case WXK_NUMPAD9:          return "9";
        case WXK_NUMPAD_ADD:       return "plus";
        case WXK_NUMPAD_SUBTRACT:  return "minus";
        case WXK_NUMPAD_MULTIPLY:  return "asterisk";
        case WXK_NUMPAD_DIVIDE:    return "slash";
        case WXK_NUMPAD_DECIMAL:   return "period";
        case WXK_NUMPAD_ENTER:     return "return";
        default:                   return {};
    }
}
