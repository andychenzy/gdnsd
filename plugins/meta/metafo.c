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

#include "config.h"
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#define GDNSD_PLUGIN_NAME metafo

#include <gdnsd-plugin.h>

typedef struct {
   unsigned num_dcs; // count (e.g. 3)
   uint8_t* dc_list; // 1-based numeric indices (e.g. "\1\2\3\0")
                     //   this is always ordered in the metafo case
   char** dc_names;  // array of names, 1-based
} dclist_t;

static unsigned num_dclists = 0;
static dclist_t** dclists = NULL; // one per resource in metafo case

F_NONNULL
static unsigned res_get_mapnum(const vscf_data_t* res_cfg, const char* res_name) {
    dmn_assert(res_cfg); dmn_assert(res_name);

    // Get 'dclist' name, convert, store, return 0-based dclist index
    const vscf_data_t* dc_cfg = vscf_hash_get_data_byconstkey(res_cfg, "datacenters", true);
    if(!dc_cfg)
        log_fatal("plugin_metafo: resource '%s': required key 'datacenters' is missing", res_name);
    dclist_t* dcl = malloc(sizeof(dclist_t));
    if(vscf_is_hash(dc_cfg) || !(dcl->num_dcs = vscf_array_get_len(dc_cfg)))
        log_fatal("plugin_metafo: resource '%s': 'datacenters' must be an array of one or more datacenter name strings", res_name);

    uint8_t* dclptr = dcl->dc_list = malloc(dcl->num_dcs + 1);
    dcl->dc_names = malloc((dcl->num_dcs + 1) * sizeof(char*));
    dcl->dc_names[0] = NULL; // index zero is invalid
    for(unsigned i = 0; i < dcl->num_dcs; i++) {
        const vscf_data_t* dcname_cfg = vscf_array_get_data(dc_cfg, i);
        if(!dcname_cfg || !vscf_is_simple(dcname_cfg))
            log_fatal("plugin_metafo: resource '%s': 'datacenters' must be an array of one or more datacenter name strings", res_name);
        const unsigned dcidx = i + 1;
        *dclptr++ = dcidx;
        dcl->dc_names[dcidx] = strdup(vscf_simple_get_data(dcname_cfg));
    }
    *dclptr = 0;

    const unsigned rv_idx = num_dclists++;
    dclists = realloc(dclists, num_dclists * sizeof(dclist_t*));
    dclists[rv_idx] = dcl;
    return rv_idx;
}

static unsigned map_get_len(const unsigned mapnum) {
    dmn_assert(mapnum < num_dclists);
    dmn_assert(dclists[mapnum]->num_dcs);
    return dclists[mapnum]->num_dcs;
}

F_NONNULL
static unsigned map_get_dcidx(const unsigned mapnum, const char* dcname) {
    dmn_assert(dcname);
    dmn_assert(mapnum < num_dclists);

    dclist_t* this_map = dclists[mapnum];
    for(unsigned i = 1; i <= this_map->num_dcs; i++)
        if(!strcmp(dcname, this_map->dc_names[i]))
            return i;

    return 0;
}

F_UNUSED
static void maps_destroy(void) {
    if(dclists) {
        for(unsigned i = 0; i < num_dclists; i++) {
            dclist_t* dcl = dclists[i];
            for(unsigned j = 1; j <= dcl->num_dcs; j++)
                free(dcl->dc_names[j]);
            free(dcl->dc_names);
            free(dcl->dc_list);
            free(dcl);
        }
        free(dclists);
    }
}

F_NONNULL
static void top_config_hook(const vscf_data_t* top_config V_UNUSED) {
    dmn_assert(top_config); dmn_assert(vscf_is_hash(top_config));
}

F_NONNULL
static const uint8_t* map_get_dclist(const unsigned mapnum, const client_info_t* cinfo V_UNUSED, unsigned* scope_out V_UNUSED) {
    dmn_assert(mapnum < num_dclists); dmn_assert(cinfo); dmn_assert(scope_out);
    return dclists[mapnum]->dc_list;
}

#define PNSTR "metafo"
#define DYNC_OK 0
#define CB_LOAD_CONFIG plugin_metafo_load_config
#define CB_MAP_A plugin_metafo_map_resource_dyna
#define CB_RES_A plugin_metafo_resolve_dynaddr
#define CB_EXIT plugin_metafo_exit
#include "meta_core.c"
