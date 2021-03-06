/* Copyright © 2012 Brandon L Black <blblack@gmail.com>
 *
 * This file is part of gdnsd.
 *
 * gdnsd is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * gdnsd is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with gdnsd.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef _GDNSD_MISC_PRIV_H
#define _GDNSD_MISC_PRIV_H

#include "gdnsd-misc.h"

// Called by core daemon, sets internal config directory
//  based on absolute path of cfg_file.
F_NONNULL
void gdnsd_set_cfdir(const char* cfg_file);

// Globally initialize meta-prng at daemon startup
void gdnsd_rand_meta_init(void);

#endif // _GDNSD_MISC_PRIV_H
