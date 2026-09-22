#pragma once

#include "screen_reader.h"

/** Register the "sr" console command. Called once by the service. */
void screen_reader_cli_register(ScreenReader* sr);
