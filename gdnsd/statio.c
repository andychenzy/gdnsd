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

#include "statio.h"

#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <sys/uio.h>

#include "conf.h"
#include "dnsio_udp.h"
#include "dnsio_tcp.h"
#include "dnspacket.h"
#include "monio.h"
#include "pkterr.h"

// Macro to add an offset to a void* portably...
#define ADDVOID(_vstar,_offs) ((void*)(((char*)(_vstar)) + _offs))

typedef struct {
    satom_uint_t udp_recvfail;
    satom_uint_t udp_sendfail;
    satom_uint_t udp_tc;
    satom_uint_t udp_edns_big;
    satom_uint_t udp_edns_tc;
    satom_uint_t tcp_recvfail;
    satom_uint_t tcp_recvsize;
    satom_uint_t tcp_sendfail;
    satom_uint_t dns_noerror;
    satom_uint_t dns_refused;
    satom_uint_t dns_nxdomain;
    satom_uint_t dns_notimp;
    satom_uint_t dns_badvers;
    satom_uint_t dns_formerr;
    satom_uint_t dns_dropped;
    satom_uint_t dns_v6;
    satom_uint_t dns_edns;
    satom_uint_t dns_edns_clientsub;
    satom_uint_t udp_reqs;
    satom_uint_t tcp_reqs;
} stats_t;

typedef enum {
    READING_REQ = 0,
    WRITING_RES,
    READING_JUNK
} http_state_t;

typedef struct {
    anysin_t* asin;
    char read_buffer[8];
    struct iovec outbufs[2];
    char* hdr_buf;
    char* data_buf;
    ev_io* read_watcher;
    ev_io* write_watcher;
    ev_timer* timeout_watcher;
    unsigned iovcnt;
    unsigned read_done;
    http_state_t state;
} http_data_t;

// After reading the first 8 bytes of the request (all we care
//  about), we send the response and then linger
//  draining the remaining input in JUNK_SIZE chunks before
//  the final SHUT_RDWR/close().  junk_buffer should be per-
//  thread, but there's only one statio thread and it doesn't
//  matter if multiple connections step all over each other writing
//  to this.
#define JUNK_SIZE 4096
static char* junk_buffer;

// Various fixed sprintf strings for stats output formats

static const char log_dns[] =
    "noerror:%" PRIuPTR " refused:%" PRIuPTR " nxdomain:%" PRIuPTR " notimp:%" PRIuPTR " badvers:%" PRIuPTR " formerr:%" PRIuPTR " dropped:%" PRIuPTR " v6:%" PRIuPTR " edns:%" PRIuPTR " edns_clientsub:%" PRIuPTR;
static const char log_udp[] =
    "udp_reqs:%" PRIuPTR " udp_recvfail:%" PRIuPTR " udp_sendfail:%" PRIuPTR " udp_tc:%" PRIuPTR " udp_edns_big:%" PRIuPTR " udp_edns_tc:%" PRIuPTR;
static const char log_tcp[] =
    "tcp_reqs:%" PRIuPTR " tcp_recvfail:%" PRIuPTR " tcp_recvsize:%" PRIuPTR " tcp_sendfail:%" PRIuPTR;

static const char http_404_hdr[] =
    "HTTP/1.0 404 Not Found\r\n"
    "Server: " PACKAGE_NAME "/" PACKAGE_VERSION "\r\n"
    "Content-type: text/plain; charset=utf-8\r\n"
    "Content-length: 11\r\n\r\n";

static const char http_404_data[] = "Not Found\r\n";

static const char http_headers[] =
    "HTTP/1.0 200 OK\r\n"
    "Server: " PACKAGE_NAME "/" PACKAGE_VERSION "\r\n"
    "Pragma: no-cache\r\n"
    "Expires: Sat, 26 Jul 1997 05:00:00 GMT\r\n"
    "Cache-Control: no-store, no-cache, must-revalidate, post-check=0, pre-check=0, max-age=0\r\n"
    "Refresh: 60\r\n"
    "Content-type: %s; charset=utf-8\r\n"
    "Content-length: %li\r\n\r\n";

static const char csv_fixed[] =
    "uptime\r\n"
    "%li\r\n"
    "noerror,refused,nxdomain,notimp,badvers,formerr,dropped,v6,edns,edns_clientsub\r\n"
    "%" PRIuPTR ",%" PRIuPTR ",%" PRIuPTR ",%" PRIuPTR ",%" PRIuPTR ",%" PRIuPTR ",%" PRIuPTR ",%" PRIuPTR ",%" PRIuPTR ",%" PRIuPTR "\r\n"
    "udp_reqs,udp_recvfail,udp_sendfail,udp_tc,udp_edns_big,udp_edns_tc\r\n"
    "%" PRIuPTR ",%" PRIuPTR ",%" PRIuPTR ",%" PRIuPTR ",%" PRIuPTR ",%" PRIuPTR "\r\n"
    "tcp_reqs,tcp_recvfail,tcp_recvsize,tcp_sendfail\r\n"
    "%" PRIuPTR ",%" PRIuPTR ",%" PRIuPTR ",%" PRIuPTR "\r\n";

static const char html_fixed[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
    "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\" \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\">\r\n"
    "<html xmlns=\"http://www.w3.org/1999/xhtml\" lang=\"en\" xml:lang=\"en\">\r\n"
    "<head><title>" PACKAGE_NAME "</title><style type='text/css'>\r\n"
    ".bold { font-weight: bold }\r\n"
    ".big { font-size: 1.25em; }\r\n"
    "table { border-width: 2px; border-style: ridge; margin: 0.25em; padding: 1px }\r\n"
    "th,td { border-width: 2px; border-style: inset }\r\n"
    "th { background: #CCF; font-weight: bold }\r\n"
    "td.UP { background: #AFA }\r\n"
    "td.DANGER { background: #FC9 }\r\n"
    "td.DOWN { background: #FAA }\r\n"
    "</style></head><body>\r\n"
    "<h2>" PACKAGE_NAME "/" PACKAGE_VERSION "</h2>\r\n"
    "<p class='big'><span class='bold'>Current Time:</span> %s UTC</p>\r\n"
    "<p class='big'><span class='bold'>Uptime:</span> %s</p>\r\n"
    "<p><span class='bold big'>Stats:</span></p><table>\r\n"
    "<tr><th>noerror</th><th>refused</th><th>nxdomain</th><th>notimp</th><th>badvers</th><th>formerr</th><th>dropped</th><th>v6</th><th>edns</th><th>edns_clientsub</th></tr>\r\n"
    "<tr><td>%" PRIuPTR "</td><td>%" PRIuPTR "</td><td>%" PRIuPTR "</td><td>%" PRIuPTR "</td><td>%" PRIuPTR "</td><td>%" PRIuPTR "</td><td>%" PRIuPTR "</td><td>%" PRIuPTR "</td><td>%" PRIuPTR "</td><td>%" PRIuPTR "</td></tr>\r\n"
    "</table><table>\r\n"
    "<tr><th>udp_reqs</th><th>udp_recvfail</th><th>udp_sendfail</th><th>udp_tc</th><th>udp_edns_big</th><th>udp_edns_tc</th></tr>\r\n"
    "<tr><td>%" PRIuPTR "</td><td>%" PRIuPTR "</td><td>%" PRIuPTR "</td><td>%" PRIuPTR "</td><td>%" PRIuPTR "</td><td>%" PRIuPTR "</td></tr>\r\n"
    "</table><table>\r\n"
    "<tr><th>tcp_reqs</th><th>tcp_recvfail</th><th>tcp_recvsize</th><th>tcp_sendfail</th></tr>\r\n"
    "<tr><td>%" PRIuPTR "</td><td>%" PRIuPTR "</td><td>%" PRIuPTR "</td><td>%" PRIuPTR "</td></tr>\r\n"
    "</table>\r\n";

static const char html_footer[] =
    "<p>For machine-readable CSV output, use <a href='/csv'>/csv</a></p>\r\n"
    "</body></html>\r\n";

static time_t start_time;
static ev_timer* log_watcher = NULL;
static ev_io** accept_watchers;
static int* lsocks;
static unsigned num_lsocks;
static unsigned num_conn_watchers = 0;
static unsigned data_buffer_size = 0;
static unsigned hdr_buffer_size = 0;
static stats_t stats;
static time_t pop_stats_time = 0;

static void accumulate_stats(unsigned threadnum) {
    dnspacket_stats_t* this_stats = dnspacket_stats[threadnum];
    dmn_assert(this_stats);

    const satom_uint_t l_noerror   = satom_get(&this_stats->noerror);
    const satom_uint_t l_refused   = satom_get(&this_stats->refused);
    const satom_uint_t l_nxdomain  = satom_get(&this_stats->nxdomain);
    const satom_uint_t l_notimp    = satom_get(&this_stats->notimp);
    const satom_uint_t l_badvers   = satom_get(&this_stats->badvers);
    const satom_uint_t l_formerr   = satom_get(&this_stats->formerr);
    const satom_uint_t l_dropped   = satom_get(&this_stats->dropped);
    stats.dns_noerror  += l_noerror;
    stats.dns_refused  += l_refused;
    stats.dns_nxdomain += l_nxdomain;
    stats.dns_notimp   += l_notimp;
    stats.dns_badvers  += l_badvers;
    stats.dns_formerr  += l_formerr;
    stats.dns_dropped  += l_dropped;

    const satom_uint_t this_reqs = l_noerror + l_refused + l_nxdomain
        + l_notimp + l_badvers + l_formerr + l_dropped;

    if(this_stats->is_udp) {
        stats.udp_reqs     += this_reqs;
        stats.udp_recvfail += satom_get(&this_stats->p.udp.recvfail);
        stats.udp_sendfail += satom_get(&this_stats->p.udp.sendfail);
        stats.udp_tc       += satom_get(&this_stats->p.udp.tc);
        stats.udp_edns_big += satom_get(&this_stats->p.udp.edns_big);
        stats.udp_edns_tc  += satom_get(&this_stats->p.udp.edns_tc);
    }
    else {
        stats.tcp_reqs     += this_reqs;
        stats.tcp_recvfail += satom_get(&this_stats->p.tcp.recvfail);
        stats.tcp_recvsize += satom_get(&this_stats->p.tcp.recvsize);
        stats.tcp_sendfail += satom_get(&this_stats->p.tcp.sendfail);
    }

    stats.dns_v6             += satom_get(&this_stats->v6);
    stats.dns_edns           += satom_get(&this_stats->edns);
    stats.dns_edns_clientsub += satom_get(&this_stats->edns_clientsub);
}

static void populate_stats(void) {
    const time_t now = time(NULL);
    if(gconfig.realtime_stats || now > pop_stats_time) {
        memset(&stats, 0, sizeof(stats));

        const unsigned nio = gconfig.num_io_threads;
        for(unsigned i = 0; i < nio; i++)
            accumulate_stats(i);
        pop_stats_time = now;
    }
}

#define IVAL_BUFSZ 16
static char ival_buf[IVAL_BUFSZ];
static const char* fmt_ival(const unsigned interval) {
    const double dinterval = interval;

    if(interval < 128)           // 2m 8s
        snprintf(ival_buf, IVAL_BUFSZ, "%u secs", interval);
    else if(interval < 7680)     // 2h 8m
        snprintf(ival_buf, IVAL_BUFSZ, "~ %.1f mins", dinterval / 60.0);
    else if(interval < 180000)   // 50h
        snprintf(ival_buf, IVAL_BUFSZ, "~ %.1f hours", dinterval / 3600.0);
    else if(interval < 1209600)  // 14d
        snprintf(ival_buf, IVAL_BUFSZ, "~ %.1f days", dinterval / 86400.0);
    else if(interval < 7776000)  // 90d
        snprintf(ival_buf, IVAL_BUFSZ, "~ %.1f weeks", dinterval / 604800.0);
    else if(interval < 52560057) // ~20 months
        snprintf(ival_buf, IVAL_BUFSZ, "~ %.1f months", dinterval / 2622240.0);
    else
        snprintf(ival_buf, IVAL_BUFSZ, "~ %.1f years", dinterval / 31536000.0);

    return ival_buf;
}

void statio_log_uptime(void) {
    const time_t now = time(NULL);
    const unsigned long uptime = now - start_time;

    log_info("Uptime: %s", fmt_ival(uptime));
}

void statio_log_stats(void) {
    populate_stats();
    log_info(log_dns, stats.dns_noerror, stats.dns_refused, stats.dns_nxdomain, stats.dns_notimp, stats.dns_badvers, stats.dns_formerr, stats.dns_dropped, stats.dns_v6, stats.dns_edns, stats.dns_edns_clientsub);
    log_info(log_udp, stats.udp_reqs, stats.udp_recvfail, stats.udp_sendfail, stats.udp_tc, stats.udp_edns_big, stats.udp_edns_tc);
    log_info(log_tcp, stats.tcp_reqs, stats.tcp_recvfail, stats.tcp_recvsize, stats.tcp_sendfail);
}

F_NONNULL
static void statio_fill_outbuf_csv(struct iovec* outbufs) {
    dmn_assert(outbufs);
    populate_stats();

    outbufs[1].iov_len = snprintf(outbufs[1].iov_base, data_buffer_size, csv_fixed, (long)(pop_stats_time - start_time), stats.dns_noerror, stats.dns_refused, stats.dns_nxdomain, stats.dns_notimp, stats.dns_badvers, stats.dns_formerr, stats.dns_dropped, stats.dns_v6, stats.dns_edns, stats.dns_edns_clientsub, stats.udp_reqs, stats.udp_recvfail, stats.udp_sendfail, stats.udp_tc, stats.udp_edns_big, stats.udp_edns_tc, stats.tcp_reqs, stats.tcp_recvfail, stats.tcp_recvsize, stats.tcp_sendfail);

    outbufs[1].iov_len += monio_stats_out_csv(ADDVOID(outbufs[1].iov_base, outbufs[1].iov_len));
    outbufs[0].iov_len = snprintf(outbufs[0].iov_base, hdr_buffer_size, http_headers, "text/plain", (long)outbufs[1].iov_len);
}

F_NONNULL
static void statio_fill_outbuf_html(struct iovec* outbufs) {
    dmn_assert(outbufs);
    populate_stats();

    const unsigned long uptime = pop_stats_time - start_time;

    struct tm now_tm;
    if(!gmtime_r(&pop_stats_time, &now_tm))
        log_fatal("gmtime_r() failed");

    char now_char[26];
    if(!asctime_r(&now_tm, now_char))
        log_fatal("asctime_r() failed");

    outbufs[1].iov_len = snprintf(outbufs[1].iov_base, data_buffer_size, html_fixed, now_char, fmt_ival(uptime), stats.dns_noerror, stats.dns_refused, stats.dns_nxdomain, stats.dns_notimp, stats.dns_badvers, stats.dns_formerr, stats.dns_dropped, stats.dns_v6, stats.dns_edns, stats.dns_edns_clientsub, stats.udp_reqs, stats.udp_recvfail, stats.udp_sendfail, stats.udp_tc, stats.udp_edns_big, stats.udp_edns_tc, stats.tcp_reqs, stats.tcp_recvfail, stats.tcp_recvsize, stats.tcp_sendfail);

    outbufs[1].iov_len += monio_stats_out_html(ADDVOID(outbufs[1].iov_base, outbufs[1].iov_len));
    memcpy(ADDVOID(outbufs[1].iov_base, outbufs[1].iov_len), html_footer, (sizeof(html_footer)) - 1);
    outbufs[1].iov_len += (sizeof(html_footer)-1);
    outbufs[0].iov_len = snprintf(outbufs[0].iov_base, hdr_buffer_size, http_headers, "application/xhtml+xml", (long)outbufs[1].iov_len);
}

// Could be merged to a single iov, but this keeps things
//  "simple", so that the write code always expects to start
//  out with two iovecs to send.
F_NONNULL
static void statio_fill_outbuf_404(struct iovec* outbufs) {
    dmn_assert(outbufs);
    outbufs[0].iov_len = sizeof(http_404_hdr) - 1;
    outbufs[0].iov_base = (char*)http_404_hdr;
    outbufs[1].iov_len = sizeof(http_404_data) - 1;
    outbufs[1].iov_base = (char*)http_404_data;
}

F_NONNULL
static void log_watcher_cb(struct ev_loop* loop V_UNUSED, ev_timer* t V_UNUSED, int revents V_UNUSED) {
    dmn_assert(loop); dmn_assert(t);
    statio_log_stats();
}

F_NONNULL
static void process_http_query(char* inbuffer, struct iovec* outbufs) {
    dmn_assert(inbuffer); dmn_assert(outbufs);
    if(!memcmp(inbuffer, "GET / ", 6))
        statio_fill_outbuf_html(outbufs);
    else if(!memcmp(inbuffer, "GET /csv", 8))
        statio_fill_outbuf_csv(outbufs);
    else
        statio_fill_outbuf_404(outbufs);
}

F_NONNULL
static void cleanup_conn_watchers(struct ev_loop* loop, http_data_t* tdata) {
    dmn_assert(loop); dmn_assert(tdata);

    shutdown(tdata->read_watcher->fd, SHUT_RDWR);
    close(tdata->read_watcher->fd);
    ev_timer_stop(loop, tdata->timeout_watcher);
    ev_io_stop(loop, tdata->read_watcher);
    ev_io_stop(loop, tdata->write_watcher);
    free(tdata->data_buf);
    free(tdata->hdr_buf);
    free(tdata->timeout_watcher);
    free(tdata->read_watcher);
    free(tdata->write_watcher);
    free(tdata->asin);

    if((num_conn_watchers-- == gconfig.max_http_clients))
        for(unsigned i = 0; i < num_lsocks; i++)
            ev_io_start(loop, accept_watchers[i]);

    free(tdata);
}

F_NONNULL
static void timeout_cb(struct ev_loop* loop V_UNUSED, ev_timer* t, const int revents V_UNUSED) {
    dmn_assert(loop); dmn_assert(t);
    dmn_assert(revents == EV_TIMER);

    http_data_t* tdata = (http_data_t*)t->data;
    log_pkterr("HTTP connection timed out while %s %s",
        tdata->state == READING_REQ
            ? "reading from"
            : tdata->state == WRITING_RES
                ? "writing to"
                : "lingering with",
        logf_anysin(tdata->asin));

    cleanup_conn_watchers(loop, tdata);
}

F_NONNULL
static void write_cb(struct ev_loop* loop, ev_io* io, const int revents V_UNUSED) {
    dmn_assert(loop); dmn_assert(io);
    dmn_assert(revents == EV_WRITE);

    http_data_t* tdata = (http_data_t*)io->data;

    dmn_assert(tdata->iovcnt == 1 || tdata->iovcnt == 2);
    unsigned tosend = tdata->outbufs[0].iov_len;
    struct iovec* iovs = &tdata->outbufs[0];
    if(tdata->iovcnt == 2) {
        tosend += tdata->outbufs[1].iov_len;
    }
    else {
        iovs = &tdata->outbufs[1];
    }

    const ssize_t written = writev(io->fd, iovs, tdata->iovcnt);
    if(unlikely(written == -1)) {
        if(errno == EAGAIN || errno == EINTR) return;
        log_pkterr("HTTP send() failed (%s), dropping response to %s", logf_errno(), logf_anysin(tdata->asin));
        cleanup_conn_watchers(loop, tdata);
        return;
    }

    if(likely(written == (ssize_t)tosend)) {
        tdata->state = READING_JUNK;
        ev_io_stop(loop, tdata->write_watcher);
        ev_io_start(loop, tdata->read_watcher);
    }
    else {
        if(written < (int)tdata->outbufs[0].iov_len) {
            tdata->outbufs[0].iov_base = &(((char*)tdata->outbufs[0].iov_base)[written]);
            tdata->outbufs[0].iov_len -= written;
        }
        else {
            dmn_assert(tdata->iovcnt == 2);
            unsigned adj = (written - tdata->outbufs[0].iov_len);
            tdata->outbufs[1].iov_base = &(((char*)tdata->outbufs[0].iov_base)[adj]);
            tdata->outbufs[1].iov_len -= adj;
            tdata->iovcnt = 1;
        }
    }
}

F_NONNULL
static void read_cb(struct ev_loop* loop, ev_io* io, const int revents V_UNUSED) {
    dmn_assert(loop); dmn_assert(io);
    dmn_assert(revents == EV_READ);
    http_data_t* tdata = (http_data_t*)io->data;

    dmn_assert(tdata);
    dmn_assert(tdata->state != WRITING_RES);

    if(tdata->state == READING_JUNK) {
        ssize_t recvlen = recv(io->fd, junk_buffer, JUNK_SIZE, 0);
        if(unlikely(recvlen == -1)) {
            if(errno == EAGAIN || errno == EINTR) return;
            log_pkterr("HTTP recv() error (lingering) from %s: %s", logf_anysin(tdata->asin), logf_errno());
        }
        if(recvlen < 1) cleanup_conn_watchers(loop, tdata);
        return;
    }

    if(likely(tdata->read_done < 8)) {
        char* destination = &tdata->read_buffer[tdata->read_done];
        const size_t wanted = 8 - tdata->read_done;
        ssize_t recvlen = recv(io->fd, destination, wanted, 0);
        if(unlikely(recvlen == -1)) {
            if(errno != EAGAIN && errno != EINTR) {
                log_pkterr("HTTP recv() error from %s: %s", logf_anysin(tdata->asin), logf_errno());
                cleanup_conn_watchers(loop, tdata);
            }
            return;
        }
        tdata->read_done += recvlen;
        if(tdata->read_done < 8) return;
    }

    // We're relying on the OS to buffer the rest of the request while
    //  we write the response.  After we're done writing we'll drain
    //  the rest of it for a proper lingering close.

    process_http_query(tdata->read_buffer, tdata->outbufs);
    tdata->state = WRITING_RES;
    tdata->read_done = 0;
    ev_io_stop(loop, tdata->read_watcher);
    ev_io_start(loop, tdata->write_watcher);
}

F_NONNULL
static void accept_cb(struct ev_loop* loop, ev_io* io, int revents V_UNUSED) {
    dmn_assert(loop); dmn_assert(io);
    dmn_assert(revents == EV_READ);

    anysin_t* asin = malloc(sizeof(anysin_t));
    asin->len = ANYSIN_MAXLEN;

#ifdef USE_ACCEPT4
    const int sock = accept4(io->fd, &asin->sa, &asin->len, SOCK_NONBLOCK);
#else
    const int sock = accept(io->fd, &asin->sa, &asin->len);
#endif

    if(unlikely(sock == -1)) {
        free(asin);
        switch(errno) {
            case EAGAIN:
            case EINTR:
                break;
#ifdef ENONET
            case ENONET:
#endif
            case ENETDOWN:
            case EPROTO:
            case EHOSTDOWN:
            case EHOSTUNREACH:
            case ENETUNREACH:
                log_pkterr("HTTP: early tcp socket death: %s", logf_errno());
                break;
            default:
                log_err("HTTP: accept() error: %s", logf_errno());
        }
        return;
    }

    log_debug("HTTP: Received connection from %s", logf_anysin(asin));

#ifndef USE_ACCEPT4
    if(unlikely(fcntl(sock, F_SETFL, (fcntl(sock, F_GETFL, 0)) | O_NONBLOCK) == -1)) {
        free(asin);
        close(sock);
        log_err("Failed to set O_NONBLOCK on inbound HTTP socket: %s", logf_errno());
        return;
    }
#endif

    ev_io* read_watcher = malloc(sizeof(ev_io));
    ev_io* write_watcher = malloc(sizeof(ev_io));
    ev_timer* timeout_watcher = malloc(sizeof(ev_timer));

    http_data_t* tdata = calloc(1, sizeof(http_data_t));
    tdata->state = READING_REQ;
    tdata->asin = asin;
    tdata->read_watcher = read_watcher;
    tdata->write_watcher = write_watcher;
    tdata->timeout_watcher = timeout_watcher;

    tdata->hdr_buf = tdata->outbufs[0].iov_base = malloc(hdr_buffer_size);
    tdata->data_buf = tdata->outbufs[1].iov_base = malloc(data_buffer_size);
    tdata->iovcnt = 2;

    read_watcher->data = tdata;
    write_watcher->data = tdata;
    timeout_watcher->data = tdata;

    ev_io_init(tdata->write_watcher, write_cb, sock, EV_WRITE);
    ev_set_priority(tdata->write_watcher, 1);

    ev_io_init(read_watcher, read_cb, sock, EV_READ);
    ev_set_priority(read_watcher, 0);
    ev_io_start(loop, read_watcher);

    ev_timer_init(timeout_watcher, timeout_cb, gconfig.http_timeout, 0);
    ev_set_priority(timeout_watcher, -1);
    ev_timer_start(loop, timeout_watcher);

    if((++num_conn_watchers == gconfig.max_http_clients)) {
        log_pkterr("HTTP connection limit reached");
        for(unsigned i = 0; i < num_lsocks; i++)
            ev_io_stop(loop, accept_watchers[i]);
    }

#ifdef TCP_DEFER_ACCEPT
    // Since we use DEFER_ACCEPT, the request is likely already
    //  queued and available at this point, so start read()-ing
    //  without going through the event loop
    ev_invoke(loop, read_watcher, EV_READ);
#endif
}

void statio_init(void) {
    start_time = time(NULL);

    // the junk buffer
    junk_buffer = malloc(JUNK_SIZE);

    // The largest our output sizes can possibly be:
    hdr_buffer_size =
        (sizeof(http_headers) - 1)      // http_headers format string
        + (21 - 2)                      // "application/xhtml+xml" - "%s"
        + (20 - 3);                     // 64-bit len - "%li"

    data_buffer_size =
        (sizeof(html_fixed) - 1)        // html_fixed format string
        + (25 - 2)                      // max asctime output - 2 for the original %s
        + (IVAL_BUFSZ - 2)              // max fmt_ival output, again - 2 for %s
        + (20 * (20 - strlen(PRIuPTR))) // 20 satom stats, up to 20 bytes long each
        + monio_get_max_stats_len()     // whatever monio tells us...
        + (sizeof(html_footer) - 1);    // html_footer fixed string

    // now set up the normal stuff, like libev event watchers
    if(gconfig.log_stats) {
        log_watcher = malloc(sizeof(ev_timer));
        ev_timer_init(log_watcher, log_watcher_cb, gconfig.log_stats, gconfig.log_stats);
        ev_set_priority(log_watcher, -2);
    }

    num_lsocks = gconfig.num_http_addrs;
    lsocks = malloc(sizeof(int) * num_lsocks);
    accept_watchers = malloc(sizeof(ev_io*) * num_lsocks);

    for(unsigned i = 0; i < num_lsocks; i++) {
        const anysin_t* asin = &gconfig.http_addrs[i];
        lsocks[i] = tcp_listen_pre_setup(asin, gconfig.http_timeout);
        if(bind(lsocks[i], &asin->sa, asin->len))
            log_fatal("Failed to bind() stats TCP socket to %s: %s", logf_anysin(asin), logf_errno());
        if(listen(lsocks[i], 128) == -1)
            log_fatal("Failed to listen(s, %i) on stats TCP socket %s: %s", 128, logf_anysin(asin), logf_errno());
        accept_watchers[i] = malloc(sizeof(ev_io));
        ev_io_init(accept_watchers[i], accept_cb, lsocks[i], EV_READ);
        ev_set_priority(accept_watchers[i], -2);
    }
}

void statio_start(struct ev_loop* statio_loop) {
    dmn_assert(statio_loop);

    if(log_watcher)
        ev_timer_start(statio_loop, log_watcher);

    for(unsigned i = 0; i < num_lsocks; i++)
        ev_io_start(statio_loop, accept_watchers[i]);
}

