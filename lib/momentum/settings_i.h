#include "settings.h"

void momentum_settings_load(void);

/** The card the settings were read from is gone: nothing is saved until a card's are read again */
void momentum_settings_card_removed(void);
