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

#include "zsrc_rfc1035.h"
#include "gdnsd-misc.h"
#include "zscan_rfc1035.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

static const char RFC1035_DIR[] = "etc/zones/";

// POSIX states that inode+dev uniquely identifies a file on
//   a given system.  Therefore those + mtime should uniquely
//   identify a set of file contents for a given pathname over
//   time through ops like create/destroy/modify/rename/remount/etc...
// The special value of all members being zero indicates a
//   non-existent (e.g. deleted) file.  The same value is used
//   to indicate an invalid zonefile (e.g. the pathname is
//   a subdirectory, a socket, a softlink, etc...)
typedef struct {
    time_t m; // st.st_mtime
    ino_t i;  // st.st_inode
    dev_t d;  // st.st_dev
} statcmp_t;

static bool statcmp_eq(statcmp_t* a, statcmp_t* b) {
    return !((a->m ^ b->m) | (a->i ^ b->i) | (a->d ^ b->d));
}

// check for 0/0/0, indicating deleted or invalid (e.g. socket)
static bool statcmp_nx(statcmp_t* a) {
    return !(a->m | a->i | a->d);
}

// represents a zone file
// on initial load, update_pending is false, and thus "pending" is irrelevant
// when change detection sees a statcmp diff between "loaded" and the
//   filesystem, it's going to set "update_pending" and save the fs info
//   to "pending", while in many cases a quiescence timer waits for further
//   updates.
// when "pending" and the raw FS have stabilized, then the zone is actually
//   reloaded and "loaded" is set to "pending" values and the update_pending
//   flag is cleared.
typedef struct {
    unsigned hash;       // hash of "fn"
    unsigned generation; // generation counter for deletion checks
    char* full_fn;       // "etc/zones/example.com"
    const char* fn;      // ptr to "example.com" in above storage
    zone_t* zone;        // zone data
    ev_timer* pending_event; // pending quiescence timer, NULL if no pending change
    statcmp_t pending;   // lstat() info on pending update
    statcmp_t loaded;    // lstat() info on loaded data
} zfile_t;

// hash of all extant zonefiles
static zfile_t** zfhash = NULL;
static unsigned zfhash_count = 0;
static unsigned zfhash_alloc = 0;
static unsigned generation = 0; // deletion checks use this...

// ZFILE_DELETED is a deleted hash entry that can be reclaimed
static void* const ZFILE_DELETED = (void*)(uintptr_t)0x1;
// SLOT_REAL means not NULL and also not a reclaimable deleted entry
#define SLOT_REAL(x) ((uintptr_t)x & ~1UL)

// XXX this should move over to zlist...
static void zone_delete(zone_t* zone) {
    dmn_assert(zone);
    if(zone->root)
        ltree_destroy(zone->root);
    lta_destroy(zone->arena);
    free(zone->src);
    free(zone);
}

F_NONNULL
static void zf_delete(zfile_t* zf) {
    dmn_assert(zf);
    if(zf->zone)
        zone_delete(zf->zone);
    if(zf->full_fn)
        free(zf->full_fn);
    if(zf->pending_event)
        free(zf->pending_event);
    free(zf);
}

static void statcmp_set(const char* full_fn, statcmp_t* out) {
    dmn_assert(full_fn); dmn_assert(out);

    struct stat st;
    int lstat_rv = lstat(full_fn, &st);
    if(likely(!lstat_rv && S_ISREG(st.st_mode))) {
        out->m = st.st_mtime;
        out->i = st.st_ino;
        out->d = st.st_dev;
    }
    else {
        out->m = 0;
        out->i = 0;
        out->d = 0;
    }
}

// probably a poor choice, just wanted something quick for testing XXX
F_NONNULL
static unsigned str_djb_hash(const char* input) {
   dmn_assert(input);

   unsigned hash = 5381;
   char c;
   while((c = *input++))
       hash = (hash * 33) ^ c;

   return hash;
}

// grow hash by doubling, while also
//   clearing out deletion placeholders
F_NONNULL
static void zfhash_grow(void) {
    if(unlikely(!zfhash_alloc)) {
        // initial call on empty hash
        dmn_assert(!zfhash);
        dmn_assert(!zfhash_count);
        zfhash_alloc = 16;
        zfhash = calloc(16, sizeof(zfile_t*));
        return;
    }

    const unsigned new_alloc = zfhash_alloc << 1; // double
    const unsigned new_hash_mask = new_alloc - 1;
    zfile_t** new_hash = calloc(new_alloc, sizeof(zfile_t*));

    for(unsigned i = 0; i < zfhash_alloc; i++) {
        zfile_t* zf = zfhash[i];
        if(SLOT_REAL(zf)) {
            unsigned jmpby = 1;
            unsigned slot = zf->hash & new_hash_mask;
            while(new_hash[slot]) {
                slot += jmpby++;
                slot &= new_hash_mask;
            }
            new_hash[slot] = zf;
        }
    }

    free(zfhash);
    zfhash = new_hash;
    zfhash_alloc = new_alloc;
}

// assumes this filename does not exist in hash already,
// called must use zfhash_find() first!
F_NONNULL
static void zfhash_add(zfile_t* zf) {
    dmn_assert(zf);
    dmn_assert(zf->fn);
    dmn_assert(zf->full_fn);

    // Max 25% load
    if(unlikely(zfhash_count >= (zfhash_alloc >> 2)))
        zfhash_grow();

    const unsigned hash_mask = zfhash_alloc - 1;
    unsigned slot = zf->hash & hash_mask;
    unsigned jmpby = 1;
    while(SLOT_REAL(zfhash[slot])) {
        slot += jmpby++;
        slot &= hash_mask;
    }
    zfhash[slot] = zf;
    zfhash_count++;
}

F_NONNULL
static void zfhash_del(zfile_t* zf) {
    dmn_assert(zf);
    dmn_assert(zf->fn);
    dmn_assert(zf->full_fn);

    const unsigned hash_mask = zfhash_alloc - 1;
    unsigned slot = zf->hash & hash_mask;
    unsigned jmpby = 1;
    while(zfhash[slot] != zf) {
        slot += jmpby++;
        slot &= hash_mask;
    }

    zfhash[slot] = ZFILE_DELETED;
    zfhash_count--;

    zf_delete(zf);
}

F_NONNULL
static zfile_t* zfhash_find(const char* zfn) {
    dmn_assert(zfn);

    if(likely(zfhash_alloc)) {
        const unsigned zfn_hash = str_djb_hash(zfn);
        const unsigned hash_mask = zfhash_alloc - 1;
        unsigned slot = zfn_hash & hash_mask;
        unsigned jmpby = 1;
        zfile_t* cand;
        while((cand = zfhash[slot])) {
            if(SLOT_REAL(cand) && cand->hash == zfn_hash && !strcmp(cand->fn, zfn))
                return cand;
            slot += jmpby++;
            slot &= hash_mask;
        }
    }

    return NULL;
}

F_NONNULL
static const uint8_t* make_zone_dname(const char* zf_name, ltarena_t* arena) {
    dmn_assert(zf_name); dmn_assert(arena);

    unsigned zf_name_len = strlen(zf_name);
    uint8_t alter[zf_name_len + 1]; // Storage for making alterations...
    const uint8_t* rv = NULL;

    if(zf_name_len > 1004) {
        log_err("Zone file name '%s' is illegal", zf_name);
        goto out;
    }

    // check for root zone...
    if(unlikely(zf_name_len == 9 && !strncmp(zf_name, "ROOT_ZONE", 9))) {
        alter[0] = '.';
        alter[1] = 0;
        zf_name_len = 1;
    }
    else {
        // else copy the original, and...
        memcpy(alter, zf_name, zf_name_len);
        alter[zf_name_len] = 0;

        // convert all '@' to '/' for RFC2137 reverse delegation zones,
        //   and map uppercase alpha to lowercase.
        for(unsigned i = 0; i < zf_name_len; i++) {
            if(alter[i] <= 'Z' && alter[i] >= 'A')
                alter[i] |= 0x20;
            else if(unlikely(alter[i] == '@'))
                alter[i] = '/';
        }
    }

    // Convert to terminated-dname format and check for problems
    uint8_t dname[256];
    dname_status_t status = dname_from_string(dname, alter, zf_name_len);
    if(status == DNAME_INVALID) {
        log_err("Zone name '%s' is illegal", alter);
        goto out;
    }
    if(dname_iswild(dname)) {
        log_err("Zone '%s': Wildcard zone names not allowed", logf_dname(dname));
        goto out;
    }
    if(status == DNAME_PARTIAL)
        dname_terminate(dname);

    rv = lta_dnamedup(arena, dname);

    out:
    return rv;
}

F_NONNULL
static zone_t* zone_from_zf(zfile_t* zf) {
    dmn_assert(zf);
    zone_t* z = calloc(1, sizeof(zone_t));
    z->src = str_combine("rfc1035:", zf->fn, NULL);
    z->arena = lta_new();
    z->dname = make_zone_dname(zf->fn, z->arena);
    if(!z->dname) {
        zone_delete(z);
        return NULL;
    }

    z->hash = dname_hash(z->dname);
    ltree_init_zone(z);
    if(unlikely(scan_zone(z, zf->full_fn))) {
        zone_delete(z);
        return NULL;
    }

    lta_close(z->arena);
    if(unlikely(ltree_postproc_zone(z))) {
        zone_delete(z);
        return NULL;
    }

    return z;
}

F_NONNULL
static void quiesce_check(struct ev_loop* reload_loop, ev_timer* timer, int revents) {
    dmn_assert(reload_loop);
    dmn_assert(timer);
    dmn_assert(revents = EV_TIMER);

    zfile_t* zf = (zfile_t*)timer->data;
    dmn_assert(zf->pending_event == timer);

    statcmp_t newstat;
    statcmp_set(zf->full_fn, &newstat);
    if(statcmp_eq(&newstat, &zf->pending)) {
        if(statcmp_nx(&newstat)) {
            if(zf->zone) {
                dmn_assert(!statcmp_nx(&zf->loaded));
                zlist_update(zf->zone, NULL);
            }
            zfhash_del(zf);
        }
        else { // quiesced state isn't deleted, we need to load data
            zone_t* z = zone_from_zf(zf);
            // re-check that file didn't change while loading
            statcmp_t post_check;
            statcmp_set(zf->full_fn, &post_check);
            if(!statcmp_eq(&zf->pending, &post_check)) {
                if(z)
                     zone_delete(z);
                ev_timer_set(timer, 5., 0.); // XXX needs to match old timer, or something...?
                ev_timer_start(reload_loop, timer);
            }
            else {
                if(z) {
                    memcpy(&zf->loaded, &zf->pending, sizeof(statcmp_t));
                    z->mtime = zf->loaded.m;
                    zlist_update(zf->zone, z);
                    if(zf->zone)
                        zone_delete(zf->zone);
                    zf->zone = z;
                }
                free(zf->pending_event);
                zf->pending_event = NULL;
            }
        }
    }
    else {
        ev_timer_set(timer, 5., 0.); // XXX needs to match old timer, or something...?
        ev_timer_start(reload_loop, timer);
    }
}

F_NONNULL
static void process_zonefile(const char* zfn, struct ev_loop* reload_loop, const double quiesce_time) {
    dmn_assert(zfn);
    dmn_assert(reload_loop);

    const char* fn;
    char* full_fn = str_combine(RFC1035_DIR, zfn, &fn);

    statcmp_t newstat;
    statcmp_set(full_fn, &newstat);
    zfile_t* current_zft = zfhash_find(fn);

    if(!statcmp_nx(&newstat) && !current_zft) {
        current_zft = calloc(1, sizeof(zfile_t));
        current_zft->full_fn = full_fn;
        current_zft->fn = fn;
        current_zft->hash = str_djb_hash(fn);
        zfhash_add(current_zft);
    }
    else {
        free(full_fn);
    }

    if(current_zft) {
        current_zft->generation = generation;
        if(current_zft->pending_event) { // already had a change queued...
            if(!statcmp_eq(&newstat, &current_zft->pending)) { // but it changed again!
                memcpy(&current_zft->pending, &newstat, sizeof(statcmp_t));
                ev_timer_stop(reload_loop, current_zft->pending_event);
                ev_timer_set(current_zft->pending_event, quiesce_time, 0.); // XXX timer params config
                ev_timer_start(reload_loop, current_zft->pending_event);
            }
            // else (if pending state has not changed) let timer continue as it was...
        }
        else if(!statcmp_eq(&newstat, &current_zft->pending)) { // initial change detected
            memcpy(&current_zft->pending, &newstat, sizeof(statcmp_t));
            current_zft->pending_event = malloc(sizeof(ev_timer));
            ev_timer_init(current_zft->pending_event, quiesce_check, quiesce_time, 0.); // XXX timer params config
            current_zft->pending_event->data = current_zft;
            ev_timer_start(reload_loop, current_zft->pending_event);
        }
    }
}

static void unload_zones(void) {
    for(unsigned i = 0; i < zfhash_alloc; i++) {
        zfile_t* zf = zfhash[i];
        if(SLOT_REAL(zf)) {
            zlist_update(zf->zone, NULL);
            zf_delete(zf);
        }
    }
}

static void scan_dir(struct ev_loop* reload_loop, double quiesce_time) {
    DIR* zdhandle = opendir(RFC1035_DIR);
    if(!zdhandle) {
        log_err("Cannot open zones directory '%s': %s", RFC1035_DIR, dmn_strerror(errno));
    }
    else {
        struct dirent* zfdi;
        while((zfdi = readdir(zdhandle)))
            if(likely(zfdi->d_name[0] != '.'))
                process_zonefile(zfdi->d_name, reload_loop, quiesce_time);
        if(closedir(zdhandle))
            log_err("closedir(%s) failed: %s", RFC1035_DIR, dmn_strerror(errno));
    }
}

// This is the complement to the periodic scandir(), which
//  detects deletion events.  Its job is to run immediately
//  after the scandir loop and find zfhash entries that lack
//  the current "generation" counter value, indicating they
//  were not seen during scandir(), and feed them back into
//  process_zonefile() to be picked up as deletions.
F_NONNULL
static void check_missing(struct ev_loop* reload_loop) {
    dmn_assert(reload_loop);
    dmn_assert(generation);

    for(unsigned i = 0; i < zfhash_alloc; i++) {
        zfile_t* zf = zfhash[i];
        if(SLOT_REAL(zf)) {
            if(zf->generation != generation) {
                process_zonefile(zf->fn, reload_loop, 5.0); // XXX timeout config again
            }
        }
    }
}

F_NONNULL
static void periodic_scan(struct ev_loop* reload_loop, ev_timer* rtimer, int revents) {
    dmn_assert(reload_loop);
    dmn_assert(rtimer);
    dmn_assert(revents == EV_TIMER);

    generation++;
    scan_dir(reload_loop, 5.);
    check_missing(reload_loop);
}

// ev stuff
static ev_timer* reload_timer = NULL;

/*************************/
/*** Public interfaces ***/
/*************************/

void zsrc_rfc1035_load_zones(void) {
    struct ev_loop* temp_load_loop = ev_loop_new(EVFLAG_AUTO);
    scan_dir(temp_load_loop, 0.);
    ev_run(temp_load_loop, 0);
    ev_loop_destroy(temp_load_loop);
    free(reload_timer);
    if(atexit(unload_zones))
        log_fatal("atexit(unload_zones) failed: %s", logf_errno());
}

void zsrc_rfc1035_runtime_init(struct ev_loop* zdata_loop) {
    dmn_assert(zdata_loop);

    // XXX "30" should be configurable
    reload_timer = calloc(1, sizeof(ev_timer));
    ev_timer_init(reload_timer, periodic_scan, 30., 30.);
    ev_timer_start(zdata_loop, reload_timer);
}