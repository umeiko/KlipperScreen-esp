#pragma once
#include "printer.h"
bool moonraker_files_start(unsigned offset);
bool moonraker_files_poll(printer_file_page_t *page);
void moonraker_files_cancel(void);
