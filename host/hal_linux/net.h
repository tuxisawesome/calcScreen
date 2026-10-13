/*
 * net.h - Wi-Fi control through NetworkManager's nmcli, and an HTTP relay
 * through libcurl, both driven from the poll loop without blocking.
 *
 * nmcli runs as a child process (fork/exec with an argv, never a shell), one
 * job at a time; results are cached. HTTP uses the curl multi interface with
 * socket/timer callbacks.
 *
 * HTTP flow for a calculator request (handles 0..NET_HTTP_MAX-1):
 *   begin -> (PUT: body chunks until body_len bytes arrived; the transfer
 *   starts then; GET starts immediately) -> response (DOCK_E_BUSY until the
 *   headers are in; dockd waits up to a few seconds before answering BUSY,
 *   and the calculator may simply ask again) -> read (buffered body bytes;
 *   DOCK_E_BUSY if none yet, DOCK_E_END when complete and drained) -> end.
 * The response buffer holds at most NET_HTTP_BUF bytes; when it is full the
 * transfer is paused until the calculator reads, so bodies of any size work.
 */
#ifndef HOST_NET_H
#define HOST_NET_H

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_HTTP_MAX 4
#define NET_HTTP_BUF (4u * 1024u * 1024u)
#define NET_HTTP_BODY_MAX (4u * 1024u * 1024u)
#define NET_MAX_APS 32

typedef struct net net_t;

typedef struct net_ap {
    char ssid[33];
    uint8_t signal; /* 0..100 */
    bool secure;
} net_ap_t;

enum net_job_result { NET_RESULT_IDLE, NET_RESULT_PENDING, NET_RESULT_OK, NET_RESULT_FAIL };

/* nmcli: program to run, or NULL / "none" to disable Wi-Fi control (status
 * then always reads "down"). wifi_if: interface for the IP lookup. */
net_t *net_open(const char *nmcli, const char *wifi_if);
void net_close(net_t *n);

int net_pollfds(net_t *n, struct pollfd *pfd, int max);
void net_handle(net_t *n, const struct pollfd *pfd, int cnt);
void net_tick(net_t *n, uint64_t now_ms);
int net_timeout_ms(const net_t *n, uint64_t now_ms);

/* ---- Wi-Fi ---- */
bool net_control_available(const net_t *n);
void net_set_radio(net_t *n, bool on);
/* Cached status; schedules a refresh when older than a few seconds. */
void net_get_status(net_t *n, bool *up, char *ssid, size_t ssid_cap, char *ip,
                    size_t ip_cap);
void net_scan_start(net_t *n);
/* NET_RESULT_*; on OK, *aps / *count describe the scan list. */
int net_scan_result(net_t *n, const net_ap_t **aps, int *count);
void net_connect_start(net_t *n, const char *ssid, const char *password);
int net_connect_result(net_t *n);

/* ---- HTTP (enum dock_status results) ---- */
int net_http_begin(net_t *n, uint8_t method, const char *url, const char *headers,
                   uint32_t body_len, uint8_t *handle);
int net_http_body(net_t *n, uint8_t handle, const uint8_t *data, size_t len);
int net_http_response(net_t *n, uint8_t handle, uint16_t *status, uint32_t *content_len);
int net_http_read(net_t *n, uint8_t handle, uint8_t *dst, size_t max, size_t *got);
int net_http_end(net_t *n, uint8_t handle);
void net_http_end_all(net_t *n);
/* True if the handle has headers (want_data=false) or body bytes / EOF
 * (want_data=true) available, or failed. */
bool net_http_ready(net_t *n, uint8_t handle, bool want_data);
/* Run curl (only) for up to timeout_ms or until handle is ready. */
void net_http_wait(net_t *n, uint8_t handle, bool want_data, int timeout_ms);

#endif /* HOST_NET_H */
