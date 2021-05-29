#ifndef BOSON_WRAP_H
#define BOSON_WRAP_H
#include <stdbool.h>
#include <stdint.h>

struct workspace;

bool wrap_handle(struct workspace *wk, const char *wrap_file, const char *dest_path);
#endif
