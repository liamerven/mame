// license:BSD-3-Clause
// copyright-holders:Liam Erven
/***************************************************************************

    ui/uispeech.h

    Screen reader/text-to-speech output for the internal MAME UI.

***************************************************************************/
#ifndef MAME_FRONTEND_UI_UISPEECH_H
#define MAME_FRONTEND_UI_UISPEECH_H

#pragma once

#include <string_view>

namespace ui::speech {

// returns true if a speech backend is available on this platform
bool available();

// queue UTF-8 text to be spoken, optionally interrupting speech in progress
void speak(std::string_view text, bool interrupt = true);

// silence any speech in progress and discard queued announcements
void stop();

} // namespace ui::speech

#endif // MAME_FRONTEND_UI_UISPEECH_H
