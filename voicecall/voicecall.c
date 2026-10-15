/*
 * Copyright (C) 2026 The PenguinOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Runs the DSP managed voice call path for as long as it lives (until SIGTERM).
 *
 * The voice sub-graphs live in the speaker and handset mic backend graphs, like the
 * stock ones; keeping a silent stream running on each backend keeps those graphs up,
 * and audio flows between the modem and the codecs without the host touching it.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <tinyalsa/asoundlib.h>

#define CARD 0
#define KEEPALIVE_RX_DEVICE 1
#define KEEPALIVE_TX_DEVICE 3

struct control {
    const char *name;
    const char *on;
    const char *off;
};

static const struct control kControls[] = {
    {"TERTIARY_MI2S_RX Audio Mixer MultiMedia2", "1", "0"},
    {"MultiMedia4 Mixer TX_CODEC_DMA_TX_3", "1", "0"},
    {"ADC1_MIXER Switch", "1", "0"},
    {"ADC1 Switch", "1", "0"},
    {"TX0 MODE", "ADC_NORMAL", "ADC_INVALID"},
    {"TX DEC0 MUX", "SWR_MIC", NULL},
    {"TX SMIC MUX0", "SWR_MIC0", "ZERO"},
    {"TX_AIF1_CAP Mixer DEC0", "1", "0"},
    {"TX_DEC0 Volume", "100", NULL},
};

static volatile sig_atomic_t stop;

static void on_signal(int sig) {
    (void)sig;
    stop = 1;
}

static void set_controls(struct mixer *mixer, int on) {
    for (size_t i = 0; i < sizeof(kControls) / sizeof(kControls[0]); i++) {
        const char *value = on ? kControls[i].on : kControls[i].off;
        if (!value) continue;
        struct mixer_ctl *ctl = mixer_get_ctl_by_name(mixer, kControls[i].name);
        if (!ctl) {
            fprintf(stderr, "no control '%s'\n", kControls[i].name);
            continue;
        }
        int ret = mixer_ctl_get_type(ctl) == MIXER_CTL_TYPE_ENUM
                          ? mixer_ctl_set_enum_by_string(ctl, value)
                          : mixer_ctl_set_value(ctl, 0, atoi(value));
        if (ret < 0) fprintf(stderr, "set '%s' = %s failed\n", kControls[i].name, value);
    }
}

static struct pcm *open_keepalive(unsigned int flags) {
    struct pcm_config config = {
            .channels = 2,
            .rate = 48000,
            .period_size = 960,
            .period_count = 4,
            .format = PCM_FORMAT_S16_LE,
    };
    struct pcm *pcm = pcm_open(CARD, flags & PCM_IN ? KEEPALIVE_TX_DEVICE : KEEPALIVE_RX_DEVICE, flags, &config);
    if (!pcm || !pcm_is_ready(pcm)) {
        fprintf(stderr, "open %s: %s\n", flags & PCM_IN ? "tx" : "rx", pcm ? pcm_get_error(pcm) : "");
        if (pcm) pcm_close(pcm);
        return NULL;
    }
    return pcm;
}

int main(void) {
    struct mixer *mixer = mixer_open(CARD);
    if (!mixer) {
        fprintf(stderr, "no mixer on card %d\n", CARD);
        return 1;
    }
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    set_controls(mixer, 1);

    struct pcm *tx = open_keepalive(PCM_IN);
    struct pcm *rx = open_keepalive(PCM_OUT);
    unsigned int rx_bytes = rx ? pcm_frames_to_bytes(rx, pcm_get_buffer_size(rx) / 4) : 0;
    unsigned int tx_bytes = tx ? pcm_frames_to_bytes(tx, pcm_get_buffer_size(tx) / 4) : 0;
    void *silence = calloc(1, rx_bytes ? rx_bytes : 1);
    void *discard = malloc(tx_bytes ? tx_bytes : 1);
    printf("voice path %s\n", tx && rx ? "up" : "partial");
    fflush(stdout);

    /* Both streams pace themselves on the hardware, one period at a time. */
    while (!stop && (rx || tx)) {
        if (rx && pcm_write(rx, silence, rx_bytes) < 0) {
            fprintf(stderr, "keepalive rx: %s\n", pcm_get_error(rx));
            pcm_close(rx);
            rx = NULL;
        }
        if (tx && pcm_read(tx, discard, tx_bytes) < 0) {
            fprintf(stderr, "keepalive tx: %s\n", pcm_get_error(tx));
            pcm_close(tx);
            tx = NULL;
        }
    }
    while (!stop) pause();
    free(silence);
    free(discard);

    if (rx) {
        pcm_stop(rx);
        pcm_close(rx);
    }
    if (tx) {
        pcm_stop(tx);
        pcm_close(tx);
    }
    set_controls(mixer, 0);
    mixer_close(mixer);
    return 0;
}
