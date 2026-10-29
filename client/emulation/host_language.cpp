#include "host_language.h"

#include "third_party/libretro/libretro.h"

namespace uzume::emu {

unsigned retroLanguageFor(const QLocale& locale) {
  switch (locale.language()) {
    case QLocale::Japanese: return RETRO_LANGUAGE_JAPANESE;
    case QLocale::French: return RETRO_LANGUAGE_FRENCH;
    case QLocale::Spanish: return RETRO_LANGUAGE_SPANISH;
    case QLocale::German: return RETRO_LANGUAGE_GERMAN;
    case QLocale::Italian: return RETRO_LANGUAGE_ITALIAN;
    case QLocale::Dutch: return RETRO_LANGUAGE_DUTCH;
    case QLocale::Portuguese:
      return locale.territory() == QLocale::Portugal ? RETRO_LANGUAGE_PORTUGUESE_PORTUGAL : RETRO_LANGUAGE_PORTUGUESE_BRAZIL;
    case QLocale::Russian: return RETRO_LANGUAGE_RUSSIAN;
    case QLocale::Korean: return RETRO_LANGUAGE_KOREAN;
    case QLocale::Chinese:
      return (locale.script() == QLocale::TraditionalChineseScript || locale.territory() == QLocale::Taiwan ||
              locale.territory() == QLocale::HongKong || locale.territory() == QLocale::Macao)
                 ? RETRO_LANGUAGE_CHINESE_TRADITIONAL
                 : RETRO_LANGUAGE_CHINESE_SIMPLIFIED;
    default: return RETRO_LANGUAGE_ENGLISH;
  }
}

}  // namespace uzume::emu
