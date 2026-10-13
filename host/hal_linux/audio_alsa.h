/*
 * audio_alsa.h - non-blocking audio output for calculator programs.
 *
 * Two sources: PCM streamed by the calculator (a ~1 s ring buffer; writes
 * accept what fits so the calculator can pace itself) and PCM WAV files from
 * the dock's drives (streamed to the device from the poll loop; only the
 * header is parsed). Backends: "alsa" (snd_pcm, opened non-blocking) and
 * "null" (consumes samples in real time using the monotonic clock; tests).
 *
 * The playback position is frames handed to the device minus the device's
 * queue (snd_pcm_delay), i.e. what has actually been heard. It never goes
 * backwards during one playback, so video can be slaved to it.
 *
 * Functions returning int return an enum dock_status.
 */
#ifndef HOST_AUDIO_ALSA_H
#define HOST_AUDIO_ALSA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct audio audio_t;

/* backend: "alsa" or "null". device: ALSA PCM name (ignored for null). */
audio_t *audio_open(const char *backend, const char *device);
void audio_close(audio_t *a);

/* Takes effect at the next playback (also re-probes the mixer). */
void audio_set_device(audio_t *a, const char *device);

int audio_stream_open(audio_t *a, uint32_t rate, uint8_t fmt, uint8_t channels);
int audio_stream_write(audio_t *a, const uint8_t *pcm, size_t n, size_t *accepted);
/* Play a WAV file; takes ownership of fd (closed on any outcome). */
int audio_play_wav_fd(audio_t *a, int fd);
int audio_pos(audio_t *a, uint32_t *pos_ms, bool *playing);
int audio_stop(audio_t *a);

/* Volume = master (dock settings) * program (calculator request) / 100. */
void audio_set_master_volume(audio_t *a, uint8_t percent);
int audio_set_program_volume(audio_t *a, uint8_t percent);

/* Move audio to the device. Call every loop iteration. */
void audio_pump(audio_t *a);
/* Max poll() timeout while playing, -1 when idle. */
int audio_timeout_ms(const audio_t *a);

#endif /* HOST_AUDIO_ALSA_H */
