/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_EXTERNAL_SAMURAI_H
#define MUON_EXTERNAL_SAMURAI_H

#include <stdint.h>
#include <stdbool.h>

extern const bool have_samurai;

bool muon_samu(uint32_t argc, char *const argv[]);
#endif
