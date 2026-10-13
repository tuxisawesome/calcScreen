/* net.c - see net.h. */
#define _GNU_SOURCE
#include "net.h"
#include "dock_proto.h"
#include "util.h"

#include <curl/curl.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define STATUS_REFRESH_MS 5000u
#define JOB_TIMEOUT_MS 60000u
#define JOB_OUT_MAX 65536u
#define JOB_QUEUE 8
#define MAX_ARGS 12
#define MAX_SOCKS 16

/* ------------------------------------------------------------------ */
/* types                                                               */

enum job_kind { JOB_STATUS, JOB_IP, JOB_RADIO, JOB_SCAN, JOB_CONNECT };

typedef struct job {
    enum job_kind kind;
    char argbuf[512];       /* argv strings, NUL separated */
    char *argv[MAX_ARGS];
} job_t;

typedef struct http {
    bool used;
    CURL *easy;
    struct curl_slist *hdrs;
    uint8_t method;
    uint8_t *body;
    uint32_t body_len, body_have, body_sent;
    bool started, headers_done, done, paused;
    CURLcode result;
    uint8_t *buf;           /* response body not yet read */
    size_t buf_off, buf_len, buf_cap;
} http_t;

typedef struct sock {
    curl_socket_t fd;
    short events;
} sock_t;

struct net {
    /* nmcli */
    char nmcli[256];
    char wifi_if[32];
    bool control;
    job_t queue[JOB_QUEUE];
    int qlen;
    job_t cur;
    pid_t pid;              /* running job, -1 = none */
    int out_fd;
    char out[JOB_OUT_MAX];
    size_t out_len;
    uint64_t job_start_ms;

    /* cached results */
    bool up;
    char ssid[64];
    char ip[48];
    uint64_t status_ms;     /* when the status was last refreshed */
    bool status_queued;
    net_ap_t aps[NET_MAX_APS];
    int naps;
    int scan_state, connect_state;

    /* curl */
    CURLM *multi;
    sock_t socks[MAX_SOCKS];
    int nsocks;
    long timer_ms;          /* -1 = none */
    uint64_t timer_set_ms;
    http_t http[NET_HTTP_MAX];
};

/* ------------------------------------------------------------------ */
/* nmcli jobs                                                          */

/* Build a job from a NULL-terminated list of words. */
static bool job_make(job_t *j, enum job_kind kind, const char *const *words)
{
    size_t off = 0;
    int i;
    memset(j, 0, sizeof *j);
    j->kind = kind;
    for (i = 0; words[i]; i++) {
        size_t len = strlen(words[i]) + 1;
        if (i >= MAX_ARGS - 1 || off + len > sizeof j->argbuf)
            return false;
        memcpy(j->argbuf + off, words[i], len);
        off += len;
    }
    j->argv[i] = NULL; /* pointers are set by job_fix_argv() once the job
                          stops moving */
    return true;
}

static void job_fix_argv(job_t *j)
{
    char *p = j->argbuf;
    int i = 0;
    while (i < MAX_ARGS - 1 && *p) {
        j->argv[i++] = p;
        p += strlen(p) + 1;
    }
    j->argv[i] = NULL;
}

static void job_enqueue(net_t *n, enum job_kind kind, const char *const *words)
{
    if (!n->control || n->qlen >= JOB_QUEUE)
        return;
    if (job_make(&n->queue[n->qlen], kind, words))
        n->qlen++;
}

static void job_finished(net_t *n, int wstatus);

static void job_start_next(net_t *n)
{
    if (n->pid > 0 || n->qlen == 0)
        return;
    n->cur = n->queue[0];
    memmove(n->queue, n->queue + 1, (size_t)(n->qlen - 1) * sizeof n->queue[0]);
    n->qlen--;
    job_fix_argv(&n->cur);
    n->out_len = 0;
    n->out_fd = -1;
    n->job_start_ms = mono_ms();
    n->pid = spawn_argv(n->cur.argv, &n->out_fd, false);
    if (n->pid < 0) {
        logv("net: cannot run %s: %s", n->cur.argv[0], strerror(errno));
        n->pid = -1;
        job_finished(n, 127 << 8); /* report failure, start the next job */
    }
}

/* Split one "-t" (terse) nmcli line into fields, honouring "\:" escapes. */
static int terse_fields(char *line, char **f, int max)
{
    int n = 0;
    char *r = line, *w = line;
    f[n++] = w;
    while (*r && n <= max) {
        if (*r == '\\' && r[1]) {
            *w++ = r[1];
            r += 2;
        } else if (*r == ':') {
            *w++ = '\0';
            r++;
            if (n < max)
                f[n++] = w;
            else
                break;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
    return n;
}

static void parse_status(net_t *n, char *out)
{
    char *save = NULL, *line;
    n->up = false;
    n->ssid[0] = '\0';
    for (line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *f[2];
        if (terse_fields(line, f, 2) == 2 && strcmp(f[0], "yes") == 0) {
            n->up = true;
            str_copy(n->ssid, sizeof n->ssid, f[1]);
            break;
        }
    }
}

static void parse_ip(net_t *n, char *out)
{
    char *save = NULL, *line;
    n->ip[0] = '\0';
    for (line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *f[2], *slash;
        if (terse_fields(line, f, 2) == 2 && strncmp(f[0], "IP4.ADDRESS", 11) == 0) {
            slash = strchr(f[1], '/');
            if (slash)
                *slash = '\0';
            str_copy(n->ip, sizeof n->ip, f[1]);
            break;
        }
    }
    if (!n->ip[0])
        n->up = false;
}

static int cmp_ap(const void *a, const void *b)
{
    return (int)((const net_ap_t *)b)->signal - (int)((const net_ap_t *)a)->signal;
}

static void parse_scan(net_t *n, char *out)
{
    char *save = NULL, *line;
    int i;
    n->naps = 0;
    for (line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *f[3];
        long sig;
        if (terse_fields(line, f, 3) != 3 || !f[0][0] || strlen(f[0]) > 32)
            continue;
        sig = strtol(f[1], NULL, 10);
        sig = sig < 0 ? 0 : sig > 100 ? 100 : sig;
        for (i = 0; i < n->naps && strcmp(n->aps[i].ssid, f[0]) != 0; i++)
            ;
        if (i == n->naps) {
            if (n->naps >= NET_MAX_APS)
                continue;
            str_copy(n->aps[i].ssid, sizeof n->aps[i].ssid, f[0]);
            n->aps[i].signal = 0;
            n->naps++;
        }
        if ((uint8_t)sig >= n->aps[i].signal)
            n->aps[i].signal = (uint8_t)sig;
        n->aps[i].secure = f[2][0] != '\0' && strcmp(f[2], "--") != 0;
    }
    qsort(n->aps, (size_t)n->naps, sizeof n->aps[0], cmp_ap);
}

static void queue_status(net_t *n)
{
    const char *st[] = {n->nmcli, "-t", "-f", "ACTIVE,SSID", "dev", "wifi", NULL};
    const char *ip[] = {n->nmcli, "-t", "-f", "IP4.ADDRESS", "dev", "show", n->wifi_if, NULL};
    if (n->status_queued)
        return;
    job_enqueue(n, JOB_STATUS, st);
    job_enqueue(n, JOB_IP, ip);
    n->status_queued = true;
}

static void job_finished(net_t *n, int wstatus)
{
    bool ok = WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0;
    n->out[n->out_len < sizeof n->out ? n->out_len : sizeof n->out - 1] = '\0';
    switch (n->cur.kind) {
    case JOB_STATUS:
        if (ok)
            parse_status(n, n->out);
        else
            n->up = false;
        break;
    case JOB_IP:
        if (ok)
            parse_ip(n, n->out);
        n->status_queued = false;
        n->status_ms = mono_ms();
        break;
    case JOB_RADIO:
        queue_status(n);
        break;
    case JOB_SCAN:
        if (ok)
            parse_scan(n, n->out);
        n->scan_state = ok ? NET_RESULT_OK : NET_RESULT_FAIL;
        break;
    case JOB_CONNECT:
        n->connect_state = ok ? NET_RESULT_OK : NET_RESULT_FAIL;
        queue_status(n);
        break;
    }
    n->pid = -1;
    job_start_next(n);
}

static void job_read(net_t *n)
{
    for (;;) {
        ssize_t r;
        char sink[512];
        if (n->out_len < sizeof n->out - 1)
            r = read(n->out_fd, n->out + n->out_len, sizeof n->out - 1 - n->out_len);
        else
            r = read(n->out_fd, sink, sizeof sink); /* overflow: discard */
        if (r > 0) {
            if (n->out_len < sizeof n->out - 1)
                n->out_len += (size_t)r;
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0 && errno == EAGAIN)
            return;
        close(n->out_fd); /* EOF or error */
        n->out_fd = -1;
        return;
    }
}

static void job_reap(net_t *n, uint64_t now)
{
    int st = 0;
    pid_t r;
    if (n->pid <= 0)
        return;
    if (now - n->job_start_ms > JOB_TIMEOUT_MS)
        kill(n->pid, SIGKILL);
    if (n->out_fd >= 0 && now - n->job_start_ms <= JOB_TIMEOUT_MS)
        return; /* still producing output */
    r = waitpid(n->pid, &st, WNOHANG);
    if (r == n->pid || (r < 0 && errno == ECHILD)) {
        if (n->out_fd >= 0) {
            close(n->out_fd);
            n->out_fd = -1;
        }
        job_finished(n, r == n->pid ? st : 1 << 8);
    }
}

/* ------------------------------------------------------------------ */
/* curl plumbing                                                       */

static int sock_cb(CURL *easy, curl_socket_t s, int what, void *userp, void *socketp)
{
    net_t *n = userp;
    int i;
    (void)easy;
    (void)socketp;
    for (i = 0; i < n->nsocks && n->socks[i].fd != s; i++)
        ;
    if (what == CURL_POLL_REMOVE) {
        if (i < n->nsocks)
            n->socks[i] = n->socks[--n->nsocks];
        return 0;
    }
    if (i == n->nsocks) {
        if (n->nsocks >= MAX_SOCKS)
            return -1;
        n->socks[n->nsocks++].fd = s;
    }
    n->socks[i].events = (short)(((what & CURL_POLL_IN) ? POLLIN : 0) |
                                 ((what & CURL_POLL_OUT) ? POLLOUT : 0));
    return 0;
}

static int timer_cb(CURLM *multi, long timeout_ms, void *userp)
{
    net_t *n = userp;
    (void)multi;
    n->timer_ms = timeout_ms;
    n->timer_set_ms = mono_ms();
    return 0;
}

static http_t *http_get(net_t *n, uint8_t h)
{
    return h < NET_HTTP_MAX && n->http[h].used ? &n->http[h] : NULL;
}

static void collect_done(net_t *n)
{
    CURLMsg *m;
    int left;
    while ((m = curl_multi_info_read(n->multi, &left)) != NULL) {
        http_t *h = NULL;
        if (m->msg != CURLMSG_DONE)
            continue;
        curl_easy_getinfo(m->easy_handle, CURLINFO_PRIVATE, (char **)&h);
        if (!h)
            continue;
        h->done = true;
        h->headers_done = true;
        h->result = m->data.result;
        logv("net: http done: %s", curl_easy_strerror(h->result));
    }
}

static void socket_action(net_t *n, curl_socket_t fd, int ev)
{
    int running = 0;
    curl_multi_socket_action(n->multi, fd, ev, &running);
    collect_done(n);
}

static void run_timer(net_t *n)
{
    if (n->timer_ms < 0)
        return;
    if (mono_ms() - n->timer_set_ms >= (uint64_t)n->timer_ms) {
        n->timer_ms = -1;
        socket_action(n, CURL_SOCKET_TIMEOUT, 0);
    }
}

static int curl_pollfds(net_t *n, struct pollfd *pfd, int max)
{
    int i, c = 0;
    for (i = 0; i < n->nsocks && c < max; i++) {
        pfd[c].fd = n->socks[i].fd;
        pfd[c].events = n->socks[i].events;
        pfd[c++].revents = 0;
    }
    return c;
}

static void curl_handle(net_t *n, const struct pollfd *pfd, int cnt)
{
    int i, j;
    for (i = 0; i < cnt; i++) {
        int ev = 0;
        if (!pfd[i].revents)
            continue;
        for (j = 0; j < n->nsocks && n->socks[j].fd != pfd[i].fd; j++)
            ;
        if (j == n->nsocks)
            continue; /* not a curl socket */
        if (pfd[i].revents & POLLIN)
            ev |= CURL_CSELECT_IN;
        if (pfd[i].revents & POLLOUT)
            ev |= CURL_CSELECT_OUT;
        if (pfd[i].revents & (POLLERR | POLLHUP | POLLNVAL))
            ev |= CURL_CSELECT_ERR;
        socket_action(n, pfd[i].fd, ev);
    }
    run_timer(n);
}

/* ------------------------------------------------------------------ */
/* public: lifecycle and poll                                          */

net_t *net_open(const char *nmcli, const char *wifi_if)
{
    net_t *n = calloc(1, sizeof *n);
    if (!n)
        return NULL;
    n->pid = -1;
    n->out_fd = -1;
    n->timer_ms = -1;
    n->control = nmcli && strcmp(nmcli, "none") != 0;
    str_copy(n->nmcli, sizeof n->nmcli, n->control ? nmcli : "nmcli");
    str_copy(n->wifi_if, sizeof n->wifi_if, wifi_if ? wifi_if : "wlan0");
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK ||
        !(n->multi = curl_multi_init())) {
        loge("net: curl init failed");
        free(n);
        return NULL;
    }
    curl_multi_setopt(n->multi, CURLMOPT_SOCKETFUNCTION, sock_cb);
    curl_multi_setopt(n->multi, CURLMOPT_SOCKETDATA, n);
    curl_multi_setopt(n->multi, CURLMOPT_TIMERFUNCTION, timer_cb);
    curl_multi_setopt(n->multi, CURLMOPT_TIMERDATA, n);
    if (n->control)
        queue_status(n);
    job_start_next(n);
    return n;
}

void net_close(net_t *n)
{
    if (!n)
        return;
    net_http_end_all(n);
    if (n->pid > 0) {
        kill(n->pid, SIGKILL);
        waitpid(n->pid, NULL, 0);
    }
    if (n->out_fd >= 0)
        close(n->out_fd);
    curl_multi_cleanup(n->multi);
    curl_global_cleanup();
    free(n);
}

int net_pollfds(net_t *n, struct pollfd *pfd, int max)
{
    int c = 0;
    if (n->out_fd >= 0 && max > 0) {
        pfd[c].fd = n->out_fd;
        pfd[c].events = POLLIN;
        pfd[c++].revents = 0;
    }
    return c + curl_pollfds(n, pfd + c, max - c);
}

void net_handle(net_t *n, const struct pollfd *pfd, int cnt)
{
    int i;
    for (i = 0; i < cnt; i++) {
        if (n->out_fd >= 0 && pfd[i].fd == n->out_fd && pfd[i].revents)
            job_read(n);
    }
    curl_handle(n, pfd, cnt);
    job_reap(n, mono_ms());
}

void net_tick(net_t *n, uint64_t now_ms)
{
    job_reap(n, now_ms);
    job_start_next(n);
    run_timer(n);
}

int net_timeout_ms(const net_t *n, uint64_t now_ms)
{
    int t = -1;
    if (n->pid > 0 && n->out_fd < 0)
        t = 20; /* output closed, waiting for the exit status */
    else if (n->pid > 0)
        t = 1000; /* job timeout check */
    if (n->timer_ms >= 0) {
        uint64_t due = n->timer_set_ms + (uint64_t)n->timer_ms;
        int ct = due <= now_ms ? 0 : (int)(due - now_ms);
        if (t < 0 || ct < t)
            t = ct;
    }
    return t;
}

/* ------------------------------------------------------------------ */
/* public: Wi-Fi                                                       */

bool net_control_available(const net_t *n)
{
    return n->control;
}

void net_set_radio(net_t *n, bool on)
{
    const char *w[] = {n->nmcli, "radio", "wifi", on ? "on" : "off", NULL};
    job_enqueue(n, JOB_RADIO, w);
    job_start_next(n);
}

void net_get_status(net_t *n, bool *up, char *ssid, size_t ssid_cap, char *ip,
                    size_t ip_cap)
{
    if (n->control && mono_ms() - n->status_ms >= STATUS_REFRESH_MS) {
        queue_status(n);
        job_start_next(n);
    }
    *up = n->control && n->up;
    str_copy(ssid, ssid_cap, *up ? n->ssid : "");
    str_copy(ip, ip_cap, *up ? n->ip : "");
}

void net_scan_start(net_t *n)
{
    const char *w[] = {n->nmcli, "-t", "-f", "SSID,SIGNAL,SECURITY", "dev", "wifi",
                       "list", "--rescan", "yes", NULL};
    if (!n->control) {
        n->scan_state = NET_RESULT_FAIL;
        return;
    }
    n->scan_state = NET_RESULT_PENDING;
    job_enqueue(n, JOB_SCAN, w);
    job_start_next(n);
}

int net_scan_result(net_t *n, const net_ap_t **aps, int *count)
{
    *aps = n->aps;
    *count = n->naps;
    return n->scan_state;
}

void net_connect_start(net_t *n, const char *ssid, const char *password)
{
    const char *with_pw[] = {n->nmcli, "-w", "30", "dev", "wifi", "connect", ssid,
                             "password", password, NULL};
    const char *open_net[] = {n->nmcli, "-w", "30", "dev", "wifi", "connect", ssid, NULL};
    if (!n->control) {
        n->connect_state = NET_RESULT_FAIL;
        return;
    }
    n->connect_state = NET_RESULT_PENDING;
    job_enqueue(n, JOB_CONNECT, password && password[0] ? with_pw : open_net);
    job_start_next(n);
}

int net_connect_result(net_t *n)
{
    return n->connect_state;
}

/* ------------------------------------------------------------------ */
/* public: HTTP                                                        */

static size_t write_cb(char *p, size_t size, size_t nmemb, void *user)
{
    http_t *h = user;
    size_t n = size * nmemb;
    h->headers_done = true;
    if (h->buf_off > 0 && h->buf_len + n > h->buf_cap) {
        memmove(h->buf, h->buf + h->buf_off, h->buf_len);
        h->buf_off = 0;
    }
    if (h->buf_len + n > h->buf_cap) {
        size_t cap = h->buf_cap ? h->buf_cap * 2 : 65536;
        uint8_t *nb;
        while (cap < h->buf_len + n)
            cap *= 2;
        if (cap > NET_HTTP_BUF) {
            if (h->buf_len + n > NET_HTTP_BUF) {
                h->paused = true; /* resumed by net_http_read */
                return CURL_WRITEFUNC_PAUSE;
            }
            cap = NET_HTTP_BUF;
        }
        nb = realloc(h->buf, cap);
        if (!nb)
            return 0; /* aborts the transfer */
        h->buf = nb;
        h->buf_cap = cap;
    }
    memcpy(h->buf + h->buf_off + h->buf_len, p, n);
    h->buf_len += n;
    return n;
}

static size_t read_cb(char *p, size_t size, size_t nmemb, void *user)
{
    http_t *h = user;
    size_t n = size * nmemb, left = h->body_have - h->body_sent;
    if (n > left)
        n = left;
    memcpy(p, h->body + h->body_sent, n);
    h->body_sent += (uint32_t)n;
    return n;
}

static void http_free(net_t *n, http_t *h)
{
    if (h->easy) {
        curl_multi_remove_handle(n->multi, h->easy);
        curl_easy_cleanup(h->easy);
    }
    curl_slist_free_all(h->hdrs);
    free(h->body);
    free(h->buf);
    memset(h, 0, sizeof *h);
}

/* "Name: value" lines separated by '\n'; CR and empty lines rejected. */
static bool parse_headers(http_t *h, const char *headers)
{
    const char *p = headers;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char line[512];
        struct curl_slist *nl;
        if (len) {
            if (len >= sizeof line || memchr(p, '\r', len) || !memchr(p, ':', len))
                return false;
            memcpy(line, p, len);
            line[len] = '\0';
            nl = curl_slist_append(h->hdrs, line);
            if (!nl)
                return false;
            h->hdrs = nl;
        }
        p += len + (e ? 1 : 0);
    }
    return true;
}

static bool url_ok(const char *url)
{
    size_t i;
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)
        return false;
    for (i = 0; url[i]; i++)
        if ((unsigned char)url[i] <= 0x20 || url[i] == 0x7F)
            return false;
    return i < 2048;
}

static int http_start(net_t *n, http_t *h)
{
    h->started = true;
    if (curl_multi_add_handle(n->multi, h->easy) != CURLM_OK)
        return DOCK_E_IO;
    /* kick the state machine so connecting starts right away */
    socket_action(n, CURL_SOCKET_TIMEOUT, 0);
    return DOCK_OK;
}

int net_http_begin(net_t *n, uint8_t method, const char *url, const char *headers,
                   uint32_t body_len, uint8_t *handle)
{
    http_t *h = NULL;
    uint8_t i;
    if ((method != DOCK_HTTP_GET && method != DOCK_HTTP_PUT) || !url_ok(url))
        return DOCK_E_INVALID;
    if (method == DOCK_HTTP_GET && body_len)
        return DOCK_E_INVALID;
    if (body_len > NET_HTTP_BODY_MAX)
        return DOCK_E_TOO_BIG;
    for (i = 0; i < NET_HTTP_MAX && n->http[i].used; i++)
        ;
    if (i == NET_HTTP_MAX)
        return DOCK_E_BUSY;
    h = &n->http[i];
    h->used = true;
    h->method = method;
    h->body_len = body_len;
    h->easy = curl_easy_init();
    if (!h->easy || (body_len && !(h->body = malloc(body_len)))) {
        http_free(n, h);
        return DOCK_E_IO;
    }
    if (!parse_headers(h, headers ? headers : "")) {
        http_free(n, h);
        return DOCK_E_INVALID;
    }
    curl_easy_setopt(h->easy, CURLOPT_URL, url);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(h->easy, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(h->easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(h->easy, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(h->easy, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(h->easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h->easy, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(h->easy, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(h->easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h->easy, CURLOPT_USERAGENT, "calcScreen-dock/1");
    curl_easy_setopt(h->easy, CURLOPT_HTTPHEADER, h->hdrs);
    curl_easy_setopt(h->easy, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h->easy, CURLOPT_WRITEDATA, h);
    curl_easy_setopt(h->easy, CURLOPT_PRIVATE, (char *)h);
    if (method == DOCK_HTTP_PUT) {
        curl_easy_setopt(h->easy, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(h->easy, CURLOPT_INFILESIZE_LARGE, (curl_off_t)body_len);
        curl_easy_setopt(h->easy, CURLOPT_READFUNCTION, read_cb);
        curl_easy_setopt(h->easy, CURLOPT_READDATA, h);
    }
    *handle = i;
    if (method == DOCK_HTTP_GET || body_len == 0) {
        int rc = http_start(n, h);
        if (rc != DOCK_OK) {
            http_free(n, h);
            return rc;
        }
    }
    return DOCK_OK;
}

int net_http_body(net_t *n, uint8_t handle, const uint8_t *data, size_t len)
{
    http_t *h = http_get(n, handle);
    if (!h || h->method != DOCK_HTTP_PUT || h->started)
        return DOCK_E_INVALID;
    if (len > h->body_len - h->body_have)
        return DOCK_E_TOO_BIG;
    memcpy(h->body + h->body_have, data, len);
    h->body_have += (uint32_t)len;
    return h->body_have == h->body_len ? http_start(n, h) : DOCK_OK;
}

static int fail_status(const http_t *h)
{
    return h->result == CURLE_OPERATION_TIMEDOUT ? DOCK_E_TIMEOUT : DOCK_E_IO;
}

int net_http_response(net_t *n, uint8_t handle, uint16_t *status, uint32_t *content_len)
{
    http_t *h = http_get(n, handle);
    long code = 0;
    curl_off_t cl = -1;
    *status = 0;
    *content_len = 0xFFFFFFFFu;
    if (!h || !h->started)
        return DOCK_E_INVALID;
    if (!h->headers_done)
        return DOCK_E_BUSY;
    curl_easy_getinfo(h->easy, CURLINFO_RESPONSE_CODE, &code);
    if (h->done && h->result != CURLE_OK && code == 0)
        return fail_status(h); /* never got a response */
    curl_easy_getinfo(h->easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
    *status = (uint16_t)(code > 0 && code < 1000 ? code : 0);
    if (cl >= 0 && cl < 0xFFFFFFFF)
        *content_len = (uint32_t)cl;
    return DOCK_OK;
}

int net_http_read(net_t *n, uint8_t handle, uint8_t *dst, size_t max, size_t *got)
{
    http_t *h = http_get(n, handle);
    size_t take;
    *got = 0;
    if (!h || !h->started)
        return DOCK_E_INVALID;
    if (h->buf_len == 0) {
        if (!h->done)
            return DOCK_E_BUSY;
        return h->result == CURLE_OK ? DOCK_E_END : fail_status(h);
    }
    take = h->buf_len < max ? h->buf_len : max;
    memcpy(dst, h->buf + h->buf_off, take);
    h->buf_off += take;
    h->buf_len -= take;
    if (h->buf_len == 0)
        h->buf_off = 0;
    *got = take;
    if (h->paused && h->buf_len <= NET_HTTP_BUF / 2) {
        h->paused = false;
        curl_easy_pause(h->easy, CURLPAUSE_CONT);
    }
    return DOCK_OK;
}

int net_http_end(net_t *n, uint8_t handle)
{
    http_t *h = http_get(n, handle);
    if (!h)
        return DOCK_E_INVALID;
    http_free(n, h);
    return DOCK_OK;
}

void net_http_end_all(net_t *n)
{
    uint8_t i;
    for (i = 0; i < NET_HTTP_MAX; i++)
        if (n->http[i].used)
            http_free(n, &n->http[i]);
}

bool net_http_ready(net_t *n, uint8_t handle, bool want_data)
{
    http_t *h = http_get(n, handle);
    if (!h || !h->started || h->done)
        return true;
    return want_data ? h->buf_len > 0 : h->headers_done;
}

void net_http_wait(net_t *n, uint8_t handle, bool want_data, int timeout_ms)
{
    uint64_t end = mono_ms() + (uint64_t)timeout_ms;
    while (!net_http_ready(n, handle, want_data)) {
        struct pollfd pfd[MAX_SOCKS];
        uint64_t now = mono_ms();
        int cnt, t;
        if (now >= end)
            break;
        t = (int)(end - now);
        if (n->timer_ms >= 0) {
            uint64_t due = n->timer_set_ms + (uint64_t)n->timer_ms;
            int ct = due <= now ? 0 : (int)(due - now);
            if (ct < t)
                t = ct;
        }
        if (t > 50)
            t = 50;
        cnt = curl_pollfds(n, pfd, MAX_SOCKS);
        if (poll(pfd, (nfds_t)cnt, t) < 0 && errno != EINTR)
            break;
        curl_handle(n, pfd, cnt);
    }
}
