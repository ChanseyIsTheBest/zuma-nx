// Touch controls: the Switch's touch screen and the PC's mouse as taps on the game (zuma_touch.cpp).

#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace zuma::touch {

enum class MouseKind { kDown, kMove, kUp, kSecond };

// The PC window's left mouse button and pointer, as 0..1 of the window (called from the window's input thread); the
// right button (kSecond) is a second finger while the left is held.
void OnWindowMouse(MouseKind kind, float x, float y);

// Hands the taps queued since the previous frame to the game (on the Switch it polls the touch screen first). Called on
// the game's thread between frames, from the D3DDevice_Present hook.
void Dispatch(PPCContext& ctx, uint8_t* base);

}  // namespace zuma::touch
