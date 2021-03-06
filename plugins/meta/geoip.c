/* Copyright © 2012 Brandon L Black <blblack@gmail.com>
 *
 * This file is part of gdnsd-plugin-geoip.
 *
 * gdnsd-plugin-geoip is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * gdnsd-plugin-geoip is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with gdnsd.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#define GDNSD_PLUGIN_NAME geoip

#include <gdnsd-plugin.h>

#include "gdmaps.h"

static gdmaps_t* gdmaps;

F_NONNULL
static unsigned res_get_mapnum(const vscf_data_t* res_cfg, const char* res_name) {
    dmn_assert(res_cfg); dmn_assert(res_name);

    // Get 'map' name, convert to gdmaps index
    const vscf_data_t* map_cfg = vscf_hash_get_data_byconstkey(res_cfg, "map", true);
    if(!map_cfg)
        log_fatal("plugin_geoip: resource '%s': required key 'map' is missing", res_name);
    if(!vscf_is_simple(map_cfg))
        log_fatal("plugin_geoip: resource '%s': 'map' must be a string", res_name);
    const char* map_name = vscf_simple_get_data(map_cfg);
    const int rv = gdmaps_name2idx(gdmaps, map_name);
    if(rv < 0)
        log_fatal("plugin_geoip: resource '%s': map '%s' does not exist", res_name, map_name);
    return (unsigned)rv;
}

static unsigned map_get_len(const unsigned mapnum) {
    return gdmaps_get_dc_count(gdmaps, mapnum);
}

static unsigned map_get_dcidx(const unsigned mapnum, const char* dcname) {
    return gdmaps_dcname2num(gdmaps, mapnum, dcname);
}

F_UNUSED
static void maps_destroy(void) {
    gdmaps_destroy(gdmaps);
    gdmaps = NULL;
}

F_NONNULL
static void top_config_hook(const vscf_data_t* top_config) {
    dmn_assert(top_config); dmn_assert(vscf_is_hash(top_config));

    const vscf_data_t* maps = vscf_hash_get_data_byconstkey(top_config, "maps", true);
    if(!maps)
        log_fatal("plugin_geoip: config has no 'maps' stanza");
    if(!vscf_is_hash(maps))
        log_fatal("plugin_geoip: 'maps' stanza must be a hash");
    if(!vscf_hash_get_len(maps))
        log_fatal("plugin_geoip: 'maps' stanza must contain one or more maps");

    gdmaps = gdmaps_new(maps);
}

void plugin_geoip_full_config(unsigned num_threads V_UNUSED) {
    dmn_assert(gdmaps);
    gdmaps_load_geoip_databases(gdmaps);
}

void plugin_geoip_pre_privdrop(void) {
    gdmaps_setup_geoip_watcher_paths(gdmaps);
}

void plugin_geoip_pre_run(struct ev_loop* loop V_UNUSED) {
    dmn_assert(loop);
    dmn_assert(gdmaps);
    gdmaps_setup_geoip_watchers(gdmaps);
}

F_NONNULL
static const uint8_t* map_get_dclist(const unsigned mapnum, const client_info_t* cinfo, unsigned* scope_out) {
    dmn_assert(gdmaps); dmn_assert(cinfo); dmn_assert(scope_out);
    return gdmaps_lookup(gdmaps, mapnum, cinfo, scope_out);
}

#define PNSTR "geoip"
#define DYNC_OK 1
#define CB_LOAD_CONFIG plugin_geoip_load_config
#define CB_MAP_A plugin_geoip_map_resource_dyna
#define CB_MAP_C plugin_geoip_map_resource_dync
#define CB_RES_A plugin_geoip_resolve_dynaddr
#define CB_RES_C plugin_geoip_resolve_dyncname
#define CB_EXIT plugin_geoip_exit
#include "meta_core.c"
