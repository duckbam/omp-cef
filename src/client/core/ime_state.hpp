#pragma once

#include <windows.h>

// Shared Hangul/English state of the game window's IME.
// While no text input is open the IME is detached from the game window (so it can't disturb
// movement keys). The state is remembered here so the chat opens in the language the player
// last chose - including a Han/Eng key press made while the chat was closed.
namespace ImeState
{
    struct State
    {
        bool  have     = false;   // a state has been saved at least once
        bool  attached = true;    // IME currently attached to the game window
        DWORD conv     = 0;
        DWORD sent     = 0;
        BOOL  open     = FALSE;
    };

    inline State& Get()
    {
        static State s;
        return s;
    }

    constexpr DWORD kCmodeNative = 0x0001;   // IME_CMODE_NATIVE (Hangul on a Korean IME)

    // Han/Eng key pressed while the IME is detached: flip the remembered mode.
    inline void ToggleLanguageWhileDetached()
    {
        State& s = Get();
        if (!s.have || s.attached)
            return;

        s.conv ^= kCmodeNative;
        if (s.conv & kCmodeNative)
            s.open = TRUE;
    }
}
