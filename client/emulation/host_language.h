#pragma once

#include <QLocale>

namespace uzume::emu {

// The language libretro cores get from RETRO_ENVIRONMENT_GET_LANGUAGE (a RETRO_LANGUAGE_* value): the system/UI locale
// of the PC mapped to the libretro list, English for everything the list does not know. Cores whose language option is
// "Auto" or "Use host" (DeSmuME, melonDS DS) follow it, so both DS cores start in the same language. Cores with their own
// option that defaults to English (Azahar `citra_language`) do not.
unsigned retroLanguageFor(const QLocale& locale);

}  // namespace uzume::emu
