/*
 * PanecastEngine: headless AirPlay receiver process for the Panecast app.
 *
 * Copyright (C) 2026 Cobaltline Software
 * Based on UxPlay (https://github.com/FDH2/UxPlay) and the projects it builds on.
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, version 3 of the License.
 * This program is distributed WITHOUT ANY WARRANTY; see the GNU General Public License for details.
 * You should have received a copy of the license along with this program (LICENSE in this repository).
 *
 * What it does
 *   - advertises the receiver over mDNS and runs UxPlay's AirPlay server (lib/);
 *   - writes decrypted video access units and decoded PCM audio to a named pipe the host created;
 *   - reports events to the host as one JSON object per line on stdout;
 *   - reads host commands on stdin: "allow <deviceid>", "deny <deviceid>", "quit".
 *
 * Media pipe frame: 16-byte little-endian header, then `len` payload bytes.
 *   u8 type (1 = H.264 Annex B, 2 = H.265 Annex B, 3 = PCM s16le 44100 Hz stereo, 4 = cover art JPEG)
 *   u8 flags (bit 0: payload failed to decrypt), u16 reserved, u32 len, i64 pts in ns (local NTP clock)
 */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <pthread.h>

#include "raop.h"
#include "dnssd.h"
#include "stream.h"
#include "logger.h"

#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>

#define ENGINE_VERSION "1.0.0"
#define MISSED_FEEDBACK_LIMIT 15
#define APPROVAL_TIMEOUT_MS 25000

/* ---------- state ---------- */

static raop_t *raop = NULL;
static dnssd_t *dnssd = NULL;
static unsigned short raop_port = 0;
static volatile int open_connections = 0;
static volatile int missed_feedback = 0;
/* While the host is asking the user to allow a device, the connection has no feedback traffic: the missed-feedback
 * watchdog must not reset it (it did, after 15 s, while the host's prompt allows 24 s). */
static volatile bool approval_pending = false;
static volatile bool reset_httpd = false;
static volatile bool quit_requested = false;
static int log_level = LOGGER_INFO;
static bool ask_before_connect = false;

static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t pipe_lock = PTHREAD_MUTEX_INITIALIZER;
static HANDLE media_pipe = INVALID_HANDLE_VALUE;

/* pending approval request (one at a time; AirPlay connections arrive serially) */
static pthread_mutex_t approve_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t approve_cond = PTHREAD_COND_INITIALIZER;
static char approve_id[64];
static int approve_answer = -1;   /* -1 waiting, 0 deny, 1 allow */

/* mirroring statistics, reported once a second */
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t stat_frames = 0, stat_bytes = 0, stat_bad = 0;
static int64_t stat_latency_sum = 0;
static uint32_t stat_latency_n = 0;

/* audio decoding */
static AVCodecContext *adec = NULL;
static SwrContext *aswr = NULL;
static AVPacket *apkt = NULL;
static AVFrame *aframe = NULL;
static unsigned char audio_ct = 0;
static pthread_mutex_t audio_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *pcm_buf = NULL;
static int pcm_cap = 0;

/* ---------- stdout JSON ---------- */

static void json_escape(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    for (; src && *src && n + 7 < cap; src++) {
        unsigned char c = (unsigned char) *src;
        if (c == '"' || c == '\\') { dst[n++] = '\\'; dst[n++] = c; }
        else if (c == '\n') { dst[n++] = '\\'; dst[n++] = 'n'; }
        else if (c == '\r') { dst[n++] = '\\'; dst[n++] = 'r'; }
        else if (c == '\t') { dst[n++] = '\\'; dst[n++] = 't'; }
        else if (c < 0x20) { n += snprintf(dst + n, cap - n, "\\u%04x", c); }
        else dst[n++] = c;
    }
    dst[n] = 0;
}

/* emit("ready", "\"port\":%d", port) prints {"ev":"ready","port":7000} */
static void emit(const char *ev, const char *fmt, ...) {
    char body[4096] = "";
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(body, sizeof(body), fmt, ap);
        va_end(ap);
    }
    pthread_mutex_lock(&out_lock);
    if (body[0]) fprintf(stdout, "{\"ev\":\"%s\",%s}\n", ev, body);
    else fprintf(stdout, "{\"ev\":\"%s\"}\n", ev);
    fflush(stdout);
    pthread_mutex_unlock(&out_lock);
}

static void emit_log(int level, const char *msg) {
    char esc[3000];
    json_escape(esc, sizeof(esc), msg);
    const char *lvl = level <= LOGGER_ERR ? "E" : level == LOGGER_WARNING ? "W" : level <= LOGGER_INFO ? "I" : "D";
    emit("log", "\"lvl\":\"%s\",\"msg\":\"%s\"", lvl, esc);
}

static void elog(int level, const char *fmt, ...) {
    if (level > log_level) return;
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    emit_log(level, msg);
}

/* ---------- media pipe ---------- */

static void pipe_write_frame(uint8_t type, uint8_t flags, int64_t pts, const void *a, uint32_t alen,
                             const void *b, uint32_t blen) {
    if (media_pipe == INVALID_HANDLE_VALUE) return;
    uint8_t hdr[16];
    uint32_t len = alen + blen;
    hdr[0] = type; hdr[1] = flags; hdr[2] = 0; hdr[3] = 0;
    memcpy(hdr + 4, &len, 4);
    memcpy(hdr + 8, &pts, 8);
    pthread_mutex_lock(&pipe_lock);
    DWORD w;
    bool ok = WriteFile(media_pipe, hdr, sizeof(hdr), &w, NULL);
    if (ok && alen) ok = WriteFile(media_pipe, a, alen, &w, NULL);
    if (ok && blen) ok = WriteFile(media_pipe, b, blen, &w, NULL);
    pthread_mutex_unlock(&pipe_lock);
    if (!ok) {
        /* the host went away: nothing left to do */
        emit("fatal", "\"msg\":\"media pipe closed\"");
        quit_requested = true;
    }
}

static bool open_media_pipe(const wchar_t *name) {
    for (int i = 0; i < 100; i++) {
        media_pipe = CreateFileW(name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (media_pipe != INVALID_HANDLE_VALUE) return true;
        if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW(name, 200);
        else Sleep(50);
    }
    return false;
}

/* ---------- audio decode (AAC-ELD, AAC-LC, ALAC -> s16le 44.1 kHz stereo) ---------- */

static void audio_decoder_close(void) {
    if (adec) avcodec_free_context(&adec);
    if (aswr) swr_free(&aswr);
    audio_ct = 0;
}

static bool audio_decoder_open(unsigned char ct) {
    static const uint8_t eld_asc[] = { 0xf8, 0xe8, 0x50, 0x00 };
    static const uint8_t lc_asc[]  = { 0x12, 0x10 };
    static const uint8_t alac_cookie[] = {
        0x00,0x00,0x00,0x24, 0x61,0x6c,0x61,0x63, 0x00,0x00,0x00,0x00, 0x00,0x00,0x01,0x60,
        0x00,0x10,0x28,0x0a, 0x0e,0x02,0x00,0xff, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
        0x00,0x00,0xac,0x44 };
    audio_decoder_close();
    const uint8_t *extra; int extra_len; enum AVCodecID id;
    switch (ct) {
    case 2: id = AV_CODEC_ID_ALAC; extra = alac_cookie; extra_len = sizeof(alac_cookie); break;
    case 4: id = AV_CODEC_ID_AAC;  extra = lc_asc;      extra_len = sizeof(lc_asc); break;
    case 8: id = AV_CODEC_ID_AAC;  extra = eld_asc;     extra_len = sizeof(eld_asc); break;
    default: elog(LOGGER_ERR, "unsupported audio compression type %d", ct); return false;
    }
    const AVCodec *codec = avcodec_find_decoder(id);
    if (!codec) return false;
    adec = avcodec_alloc_context3(codec);
    adec->extradata = av_mallocz(extra_len + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(adec->extradata, extra, extra_len);
    adec->extradata_size = extra_len;
    adec->sample_rate = 44100;
    av_channel_layout_default(&adec->ch_layout, 2);
    if (avcodec_open2(adec, codec, NULL) < 0) {
        elog(LOGGER_ERR, "could not open audio decoder for ct=%d", ct);
        avcodec_free_context(&adec);
        return false;
    }
    if (!apkt) apkt = av_packet_alloc();
    if (!aframe) aframe = av_frame_alloc();
    audio_ct = ct;
    return true;
}

static void audio_decode_and_send(const unsigned char *data, int len, int64_t pts) {
    if (!adec || len <= 0) return;
    if (av_new_packet(apkt, len) < 0) return;
    memcpy(apkt->data, data, len);
    int ret = avcodec_send_packet(adec, apkt);
    av_packet_unref(apkt);
    if (ret < 0) return;
    while (avcodec_receive_frame(adec, aframe) == 0) {
        if (!aswr) {
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            if (swr_alloc_set_opts2(&aswr, &stereo, AV_SAMPLE_FMT_S16, 44100,
                                    &aframe->ch_layout, (enum AVSampleFormat) aframe->format,
                                    aframe->sample_rate, 0, NULL) < 0 || swr_init(aswr) < 0) {
                elog(LOGGER_ERR, "could not set up audio resampler");
                swr_free(&aswr);
                return;
            }
        }
        int out_samples = swr_get_out_samples(aswr, aframe->nb_samples);
        int need = out_samples * 4;
        if (need > pcm_cap) {
            pcm_buf = realloc(pcm_buf, need);
            pcm_cap = need;
        }
        uint8_t *out[1] = { pcm_buf };
        int got = swr_convert(aswr, out, out_samples, (const uint8_t **) aframe->extended_data, aframe->nb_samples);
        if (got > 0) pipe_write_frame(3, 0, pts, pcm_buf, (uint32_t) got * 4, NULL, 0);
        av_frame_unref(aframe);
    }
}

/* ---------- raop callbacks ---------- */

static void conn_init(void *cls) {
    open_connections++;
    emit("conn", "\"open\":%d", open_connections);
}

static void conn_destroy(void *cls) {
    open_connections--;
    if (open_connections <= 0) {
        open_connections = 0;
        pthread_mutex_lock(&audio_lock);
        audio_decoder_close();
        pthread_mutex_unlock(&audio_lock);
        emit("disconnected", NULL);
    } else {
        emit("conn", "\"open\":%d", open_connections);
    }
}

static void conn_feedback(void *cls) {
    missed_feedback = 0;
}

static void conn_reset(void *cls, int reason) {
    emit("reset", "\"reason\":\"%s\"", reason == 1 ? "lost" : reason == 2 ? "unsupported" : "other");
    reset_httpd = true;
}

static void video_reset(void *cls, reset_type_t type) {
    if (type == RESET_TYPE_RTP_SHUTDOWN || type == RESET_TYPE_NOHOLD || type == RESET_TYPE_RTP_TO_HLS_TEARDOWN) {
        emit("video_stop", "\"type\":%d", (int) type);
    }
}

static void report_client_request(void *cls, char *deviceid, char *model, char *name, bool *admit) {
    char id[128], m[128], n[256];
    json_escape(id, sizeof(id), deviceid);
    json_escape(m, sizeof(m), model);
    json_escape(n, sizeof(n), name);
    if (!ask_before_connect) {
        emit("client", "\"id\":\"%s\",\"model\":\"%s\",\"name\":\"%s\",\"admit\":true", id, m, n);
        *admit = true;
        return;
    }
    pthread_mutex_lock(&approve_lock);
    snprintf(approve_id, sizeof(approve_id), "%s", deviceid ? deviceid : "");
    approve_answer = -1;
    approval_pending = true;
    emit("request", "\"id\":\"%s\",\"model\":\"%s\",\"name\":\"%s\"", id, m, n);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += APPROVAL_TIMEOUT_MS / 1000;
    while (approve_answer < 0 && !quit_requested) {
        if (pthread_cond_timedwait(&approve_cond, &approve_lock, &deadline) != 0) break;
    }
    *admit = approve_answer == 1;
    approve_id[0] = 0;
    approval_pending = false;
    missed_feedback = 0;
    pthread_mutex_unlock(&approve_lock);
    emit("client", "\"id\":\"%s\",\"model\":\"%s\",\"name\":\"%s\",\"admit\":%s", id, m, n, *admit ? "true" : "false");
}

static void display_pin(void *cls, char *pin) {
    emit("pin", "\"pin\":\"%s\"", pin);
}

static const char *passwd(void *cls, int *len) {
    *len = 0;
    return NULL;
}

static void register_client(void *cls, const char *device_id, const char *pk_str, const char *name) { }

static bool check_register(void *cls, const char *pk_str) {
    return true;
}

static void audio_get_format(void *cls, unsigned char *ct, unsigned short *spf, bool *usingScreen, bool *isMedia,
                             uint64_t *audioFormat) {
    pthread_mutex_lock(&audio_lock);
    audio_decoder_open(*ct);
    pthread_mutex_unlock(&audio_lock);
    emit("audio", "\"ct\":%d,\"spf\":%d,\"screen\":%s,\"media\":%s", *ct, *spf,
         *usingScreen ? "true" : "false", *isMedia ? "true" : "false");
}

static void audio_process(void *cls, raop_ntp_t *ntp, audio_decode_struct *data) {
    pthread_mutex_lock(&audio_lock);
    if (data->ct != audio_ct) audio_decoder_open(data->ct);
    int64_t pts = (int64_t) (data->ntp_time_local ? data->ntp_time_local : raop_ntp_get_local_time());
    audio_decode_and_send(data->data, data->data_len, pts);
    pthread_mutex_unlock(&audio_lock);
}

static void video_process(void *cls, raop_ntp_t *ntp, video_decode_struct *data) {
    uint64_t now = raop_ntp_get_local_time();
    bool bad = data->data_len > 0 && data->data[0] == 1;
    int64_t pts = (int64_t) (data->ntp_time_local ? data->ntp_time_local : now);
    pipe_write_frame(data->is_h265 ? 2 : 1, bad ? 1 : 0, pts, data->data, (uint32_t) data->data_len, NULL, 0);
    pthread_mutex_lock(&stats_lock);
    stat_frames++;
    stat_bytes += (uint32_t) data->data_len;
    if (bad) stat_bad++;
    if (data->ntp_time_local) {
        int64_t lat = (int64_t) now - (int64_t) data->ntp_time_local;
        if (lat > -1000000000LL && lat < 5000000000LL) {
            stat_latency_sum += lat;
            stat_latency_n++;
        }
    }
    pthread_mutex_unlock(&stats_lock);
}

static void video_report_size(void *cls, float *width_source, float *height_source, float *width, float *height) {
    emit("size", "\"srcw\":%.0f,\"srch\":%.0f,\"w\":%.0f,\"h\":%.0f", *width_source, *height_source, *width, *height);
}

static int video_set_codec(void *cls, video_codec_t codec) {
    emit("codec", "\"codec\":\"%s\"", codec == VIDEO_CODEC_H265 ? "h265" : "h264");
    return 0;
}

static void video_pause(void *cls) { emit("pause", NULL); }
static void video_resume(void *cls) { emit("resume", NULL); }

static void audio_flush(void *cls) {
    pthread_mutex_lock(&audio_lock);
    if (adec) avcodec_flush_buffers(adec);
    pthread_mutex_unlock(&audio_lock);
    emit("audio_flush", NULL);
}

static void video_flush(void *cls) { emit("video_flush", NULL); }

static double current_volume = 0.0;
static double audio_set_client_volume(void *cls) { return current_volume; }

static void audio_set_volume(void *cls, float volume) {
    current_volume = volume;
    emit("volume", "\"db\":%.2f", (double) volume);
}

static int dmap_header(const unsigned char *p, char tag[5], int *len) {
    memcpy(tag, p, 4);
    tag[4] = 0;
    *len = (p[4] << 24) | (p[5] << 16) | (p[6] << 8) | p[7];
    return (*len < 0) ? -1 : 0;
}

static void audio_set_metadata(void *cls, const void *buffer, int buflen) {
    const unsigned char *p = buffer;
    char tag[5]; int len;
    char title[512] = "", artist[512] = "", album[512] = "";
    if (buflen < 8 || dmap_header(p, tag, &len) || strcmp(tag, "mlit")) return;
    p += 8; buflen -= 8;
    while (buflen >= 8) {
        if (dmap_header(p, tag, &len) || len > buflen - 8) break;
        char *dst = !strcmp(tag, "minm") ? title : !strcmp(tag, "asar") ? artist : !strcmp(tag, "asal") ? album : NULL;
        if (dst) {
            int n = len < 511 ? len : 511;
            memcpy(dst, p + 8, n);
            dst[n] = 0;
        }
        p += 8 + len; buflen -= 8 + len;
    }
    char t[1100], a[1100], b[1100];
    json_escape(t, sizeof(t), title);
    json_escape(a, sizeof(a), artist);
    json_escape(b, sizeof(b), album);
    emit("meta", "\"title\":\"%s\",\"artist\":\"%s\",\"album\":\"%s\"", t, a, b);
}

static void audio_set_coverart(void *cls, const void *buffer, int buflen) {
    if (buffer && buflen > 0) {
        pipe_write_frame(4, 0, (int64_t) raop_ntp_get_local_time(), buffer, (uint32_t) buflen, NULL, 0);
        emit("cover", "\"len\":%d", buflen);
    }
}

static void audio_stop_coverart_rendering(void *cls) { emit("cover_stop", NULL); }

static void audio_set_progress(void *cls, uint32_t *start, uint32_t *curr, uint32_t *end) {
    double pos = (double) (*curr - *start) / 44100.0;
    double dur = (double) (*end - *start) / 44100.0;
    emit("progress", "\"pos\":%.2f,\"dur\":%.2f", pos, dur);
}

static void export_dacp(void *cls, const char *active_remote, const char *dacp_id) { }

static void log_callback(void *cls, int level, const char *msg) {
    if (level <= log_level) emit_log(level, msg);
}

/* ---------- host commands on stdin ---------- */

static void *stdin_thread(void *arg) {
    char line[512];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        if ((unsigned char) line[0] == 0xEF && (unsigned char) line[1] == 0xBB && (unsigned char) line[2] == 0xBF) {
            memmove(line, line + 3, strlen(line + 3) + 1);   /* a UTF-8 BOM from the host's writer */
        }
        elog(LOGGER_DEBUG, "host command: %s", line);
        if (!strncmp(line, "allow ", 6) || !strncmp(line, "deny ", 5)) {
            bool allow = line[0] == 'a';
            const char *id = line + (allow ? 6 : 5);
            pthread_mutex_lock(&approve_lock);
            if (approve_id[0] && !strcmp(id, approve_id)) {
                approve_answer = allow ? 1 : 0;
                pthread_cond_broadcast(&approve_cond);
            }
            pthread_mutex_unlock(&approve_lock);
        } else if (!strcmp(line, "quit")) {
            break;
        }
    }
    /* stdin closed or "quit": the host is done with us */
    quit_requested = true;
    pthread_mutex_lock(&approve_lock);
    pthread_cond_broadcast(&approve_cond);
    pthread_mutex_unlock(&approve_lock);
    return NULL;
}

/* ---------- setup ---------- */

static char *utf8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = malloc(n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

static bool parse_ports(const char *s, unsigned short out[3]) {
    int a, b, c;
    if (sscanf(s, "%d,%d,%d", &a, &b, &c) != 3) return false;
    out[0] = (unsigned short) a; out[1] = (unsigned short) b; out[2] = (unsigned short) c;
    return true;
}

static void set_features(bool h265) {
    /* the UxPlay defaults: mirroring with audio, legacy pairing, FairPlay authentication */
    static const int on[] = { 1, 2, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 25, 27, 28, 30 };
    for (int bit = 0; bit < 62; bit++) dnssd_set_airplay_features(dnssd, bit, 0);
    for (size_t i = 0; i < sizeof(on) / sizeof(on[0]); i++) dnssd_set_airplay_features(dnssd, on[i], 1);
    dnssd_set_airplay_features(dnssd, 42, h265 ? 1 : 0);   /* screen multi-codec (H.265) */
}

static void usage(void) {
    fprintf(stderr,
        "PanecastEngine " ENGINE_VERSION " (GPL-3.0, based on UxPlay)\n"
        "  --name <receiver name>   --hwaddr AA:BB:CC:DD:EE:FF   --keyfile <file>\n"
        "  --pipe \\\\.\\pipe\\<name>   [--tcp 7100,7000,7001] [--udp 7011,6001,6000]\n"
        "  [--size 1920x1080] [--fps 60] [--h265] [--ask] [--pin] [--debug]\n");
}

int wmain(int argc, wchar_t **wargv) {
    char *name = NULL, *hwaddr = NULL, *keyfile = NULL;
    wchar_t *pipe_name = NULL;
    unsigned short tcp[3] = { 7100, 7000, 7001 }, udp[3] = { 7011, 6001, 6000 };
    int width = 1920, height = 1080, fps = 60;
    bool h265 = false, pin = false;

    for (int i = 1; i < argc; i++) {
        char *a = utf8(wargv[i]);
        bool has = i + 1 < argc;
        if (!strcmp(a, "--name") && has) name = utf8(wargv[++i]);
        else if (!strcmp(a, "--hwaddr") && has) hwaddr = utf8(wargv[++i]);
        else if (!strcmp(a, "--keyfile") && has) keyfile = utf8(wargv[++i]);
        else if (!strcmp(a, "--pipe") && has) pipe_name = wargv[++i];
        else if (!strcmp(a, "--tcp") && has) { char *v = utf8(wargv[++i]); parse_ports(v, tcp); free(v); }
        else if (!strcmp(a, "--udp") && has) { char *v = utf8(wargv[++i]); parse_ports(v, udp); free(v); }
        else if (!strcmp(a, "--size") && has) { char *v = utf8(wargv[++i]); sscanf(v, "%dx%d", &width, &height); free(v); }
        else if (!strcmp(a, "--fps") && has) { char *v = utf8(wargv[++i]); fps = atoi(v); free(v); }
        else if (!strcmp(a, "--h265")) h265 = true;
        else if (!strcmp(a, "--ask")) ask_before_connect = true;
        else if (!strcmp(a, "--pin")) pin = true;
        else if (!strcmp(a, "--debug")) log_level = LOGGER_DEBUG;
        else if (!strcmp(a, "--version")) { printf("%s\n", ENGINE_VERSION); return 0; }
        else { usage(); return 2; }
        free(a);
    }
    if (!name || !hwaddr || !pipe_name) { usage(); return 2; }

    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    if (!open_media_pipe(pipe_name)) {
        emit("fatal", "\"msg\":\"cannot open media pipe\"");
        return 3;
    }

    pthread_t th;
    pthread_create(&th, NULL, stdin_thread, NULL);

    unsigned char hw[6] = { 0 };
    if (sscanf(hwaddr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &hw[0], &hw[1], &hw[2], &hw[3], &hw[4], &hw[5]) != 6) {
        emit("fatal", "\"msg\":\"bad --hwaddr\"");
        return 2;
    }

    ntp_global_init();
    int err = 0;
    dnssd = dnssd_init(name, (int) strlen(name), (const char *) hw, 6, pin ? 1 : 0, &err);
    if (err || !dnssd) {
        emit("fatal", "\"msg\":\"mdns init failed (%d)\"", err);
        return 4;
    }
    set_features(h265);

    raop_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.conn_init = conn_init;
    cbs.conn_destroy = conn_destroy;
    cbs.conn_reset = conn_reset;
    cbs.conn_feedback = conn_feedback;
    cbs.audio_process = audio_process;
    cbs.video_process = video_process;
    cbs.audio_flush = audio_flush;
    cbs.video_flush = video_flush;
    cbs.video_pause = video_pause;
    cbs.video_resume = video_resume;
    cbs.audio_set_client_volume = audio_set_client_volume;
    cbs.audio_set_volume = audio_set_volume;
    cbs.audio_get_format = audio_get_format;
    cbs.video_report_size = video_report_size;
    cbs.audio_set_metadata = audio_set_metadata;
    cbs.audio_set_coverart = audio_set_coverart;
    cbs.audio_stop_coverart_rendering = audio_stop_coverart_rendering;
    cbs.audio_set_progress = audio_set_progress;
    cbs.report_client_request = report_client_request;
    cbs.display_pin = display_pin;
    cbs.register_client = register_client;
    cbs.check_register = check_register;
    cbs.passwd = passwd;
    cbs.export_dacp = export_dacp;
    cbs.video_reset = video_reset;
    cbs.video_set_codec = video_set_codec;

    raop = raop_init(&cbs);
    if (!raop) { emit("fatal", "\"msg\":\"raop init failed\""); return 5; }
    raop_set_log_callback(raop, log_callback, NULL);
    raop_set_log_level(raop, log_level);
    if (raop_init2(raop, 1, hwaddr, keyfile ? keyfile : "")) {
        emit("fatal", "\"msg\":\"raop init2 failed (key file?)\"");
        return 5;
    }
    raop_set_plist(raop, "width", width);
    raop_set_plist(raop, "height", height);
    raop_set_plist(raop, "refreshRate", fps);
    raop_set_plist(raop, "maxFPS", fps);
    if (pin) raop_set_plist(raop, "pin", 1);
    raop_set_tcp_ports(raop, tcp);
    raop_set_udp_ports(raop, udp);
    raop_port = raop_get_port(raop);
    if (raop_start_httpd(raop, &raop_port) < 0) {
        emit("fatal", "\"msg\":\"cannot listen on TCP port %d\"", raop_port);
        return 6;
    }
    raop_set_port(raop, raop_port);
    raop_set_dnssd(raop, dnssd);

    if (dnssd_register_raop(dnssd, raop_port) || dnssd_register_airplay(dnssd, raop_port)) {
        emit("fatal", "\"msg\":\"mdns registration failed (UDP 5353)\"");
        return 7;
    }
    char esc[600];
    json_escape(esc, sizeof(esc), name);
    emit("ready", "\"port\":%d,\"name\":\"%s\",\"version\":\"%s\"", raop_port, esc, ENGINE_VERSION);

    int tick = 0;
    while (!quit_requested) {
        Sleep(100);
        if (++tick % 10 == 0) {
            if (open_connections > 0 && !approval_pending) {
                if (++missed_feedback > MISSED_FEEDBACK_LIMIT) {
                    emit("reset", "\"reason\":\"timeout\"");
                    reset_httpd = true;
                    missed_feedback = 0;
                }
            } else {
                missed_feedback = 0;
            }
            pthread_mutex_lock(&stats_lock);
            if (stat_frames || stat_bytes) {
                double lat = stat_latency_n ? (double) stat_latency_sum / stat_latency_n / 1e6 : -1.0;
                emit("stats", "\"fps\":%u,\"kbps\":%u,\"bad\":%u,\"lat\":%.1f", stat_frames, stat_bytes * 8 / 1000, stat_bad, lat);
            }
            stat_frames = stat_bytes = stat_bad = 0;
            stat_latency_sum = 0;
            stat_latency_n = 0;
            pthread_mutex_unlock(&stats_lock);
        }
        if (reset_httpd) {
            reset_httpd = false;
            raop_stop_httpd(raop);
            raop_remove_known_connections(raop);
            unsigned short port = raop_get_port(raop);
            raop_start_httpd(raop, &port);
            raop_set_port(raop, port);
            emit("restarted", NULL);
        }
    }

    emit("stopping", NULL);
    dnssd_unregister_raop(dnssd);
    dnssd_unregister_airplay(dnssd);
    elog(LOGGER_DEBUG, "mdns unregistered");
    raop_destroy(raop);
    elog(LOGGER_DEBUG, "raop destroyed");
    dnssd_destroy(dnssd);
    if (media_pipe != INVALID_HANDLE_VALUE) CloseHandle(media_pipe);
    emit("stopped", NULL);
    return 0;
}
