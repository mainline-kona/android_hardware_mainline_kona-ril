/*
 * kebab-voicecall: keep the CS-VOICE front end open for the duration of a call.
 *
 * The ADSP owns call audio end to end; the host PCM is a control channel only.
 * The RIL starts this helper before dialling and stops it when the last call
 * ends; it is deliberately NOT started by the audio HAL
 * (vendor.audio.mainline.voice_call_service is empty) so only one process owns
 * the PCM. Opening playback and capture makes q6voice create / tear down the
 * MVM, CVS and CVP sessions.
 */

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <log/log.h>
#include <tinyalsa/asoundlib.h>

#define TAG "kebab-voicecall"
#define PCM_NAME "CS-VOICE"

static volatile sig_atomic_t s_stop;

static void on_signal(int sig)
{
	(void)sig;
	s_stop = 1;
}

/* Find card/device of the PCM stream whose info name contains PCM_NAME. */
static int find_pcm(int capture, int *card, int *device)
{
	DIR *cards = opendir("/proc/asound");
	struct dirent *ce;
	char path[256], buf[256], name[128];
	int found = -1;

	if (!cards)
		return -1;

	while (!s_stop && (ce = readdir(cards))) {
		DIR *pcms;
		struct dirent *pe;

		if (strncmp(ce->d_name, "card", 4))
			continue;
		snprintf(path, sizeof(path), "/proc/asound/%s", ce->d_name);
		pcms = opendir(path);
		if (!pcms)
			continue;
		while ((pe = readdir(pcms))) {
			char suffix = pe->d_name[strlen(pe->d_name) - 1];
			FILE *f;
			int dev;

			if (sscanf(pe->d_name, "pcm%dp", &dev) != 1 &&
			    sscanf(pe->d_name, "pcm%dc", &dev) != 1)
				continue;
			if ((capture && suffix != 'c') || (!capture && suffix != 'p'))
				continue;
			snprintf(path, sizeof(path), "/proc/asound/%s/%s/info", ce->d_name,
				 pe->d_name);
			f = fopen(path, "r");
			if (!f)
				continue;
			name[0] = 0;
			while (fgets(buf, sizeof(buf), f)) {
				/* the human name field is empty for these
				 * fe-dai PCMs; the id line carries CS-VOICE */
				if (sscanf(buf, "id: %127[^\n]", name) == 1 ||
				    sscanf(buf, "name: %127[^\n]", name) == 1)
					break;
			}
			fclose(f);
			if (strstr(name, PCM_NAME)) {
				*card = atoi(ce->d_name + 4);
				*device = dev;
				ALOGI("%s PCM %s at card %d device %d",
				      capture ? "capture" : "playback", name, *card, *device);
				found = 0;
				closedir(pcms);
				goto out;
			}
		}
		closedir(pcms);
	}
out:
	closedir(cards);
	if (found != 0) {
		/* Fallback: On SM8250 Kona, Card 0 Device 0 is always CS-VOICE front-end */
		*card = 0;
		*device = 0;
		ALOGI("%s PCM CS-VOICE fallback to card %d device %d",
		      capture ? "capture" : "playback", *card, *device);
		found = 0;
	}
	return found;
}

/*
 * The CS-VOICE front end is a DPCM link: without an enabled backend the
 * kernel refuses the open outright (EINVAL, "no backend DAIs enabled").
 * These are the same two routes the vendor UCM flips for a call; set them
 * here so opening the PCM works no matter what the HAL configured.
 */
static void enable_routes(int card)
{
	static const char * const routes[] = {
		"TERT_MI2S_RX Voice Mixer CS-Voice",
		"CS-Voice Capture Mixer TX_CODEC_DMA_TX_3",
		"CS-Voice Capture Mixer TERT_MI2S_TX",
		"TX_AIF1_CAP Mixer DEC1",
		"TX_AIF1_CAP Mixer DEC2",
	};
	static const struct {
		const char *name;
		const char *val;
	} enums[] = {
		{ "TX DMIC MUX1", "DMIC3" },
		{ "TX DMIC MUX2", "DMIC1" },
		{ "TX_CDC_DMA_TX_3 Channels", "Two" },
	};
	struct mixer *mixer = mixer_open(card);
	unsigned int i;

	if (!mixer) {
		ALOGE("mixer open: %s", strerror(errno));
		return;
	}
	for (i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
		struct mixer_ctl *ctl = mixer_get_ctl_by_name(mixer, routes[i]);

		if (!ctl) {
			ALOGW("missing route %s", routes[i]);
			continue;
		}
		if (mixer_ctl_set_value(ctl, 0, 1))
			ALOGE("route %s: %s", routes[i], strerror(errno));
	}
	for (i = 0; i < sizeof(enums) / sizeof(enums[0]); i++) {
		struct mixer_ctl *ctl = mixer_get_ctl_by_name(mixer, enums[i].name);

		if (!ctl)
			continue;
		if (mixer_ctl_set_enum_by_string(ctl, enums[i].val))
			ALOGW("enum %s=%s: %s", enums[i].name, enums[i].val, strerror(errno));
	}
	mixer_close(mixer);
}

static void disable_routes(int card)
{
	static const char * const routes[] = {
		"TERT_MI2S_RX Voice Mixer CS-Voice",
		"CS-Voice Capture Mixer TX_CODEC_DMA_TX_3",
		"CS-Voice Capture Mixer TERT_MI2S_TX",
		"TX_AIF1_CAP Mixer DEC1",
		"TX_AIF1_CAP Mixer DEC2",
	};
	static const struct {
		const char *name;
		const char *val;
	} enums[] = {
		{ "TX DMIC MUX1", "ZERO" },
		{ "TX DMIC MUX2", "ZERO" },
		{ "TX_CDC_DMA_TX_3 Channels", "One" },
	};
	struct mixer *mixer = mixer_open(card);
	unsigned int i;

	if (!mixer)
		return;
	for (i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
		struct mixer_ctl *ctl = mixer_get_ctl_by_name(mixer, routes[i]);
		if (ctl)
			mixer_ctl_set_value(ctl, 0, 0);
	}
	for (i = 0; i < sizeof(enums) / sizeof(enums[0]); i++) {
		struct mixer_ctl *ctl = mixer_get_ctl_by_name(mixer, enums[i].name);
		if (ctl)
			mixer_ctl_set_enum_by_string(ctl, enums[i].val);
	}
	mixer_close(mixer);
}

/* Only one process may own the CS-VOICE PCM. Hold an abstract unix socket for
 * the process lifetime; a second instance fails to bind and exits quietly. */
static int take_single_instance_lock(void)
{
	static const char name[] = "kebab-voicecall";
	struct sockaddr_un addr;
	socklen_t len;
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	addr.sun_path[0] = '\0';
	memcpy(addr.sun_path + 1, name, sizeof(name) - 1);
	len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 +
			  sizeof(name) - 1);
	if (bind(fd, (struct sockaddr *)&addr, len) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

int main(void)
{
	struct pcm_config cfg = {
		.channels = 1,
		.rate = 8000,
		/* q6voice-dai: period_bytes_min 2048, periods 2..4 */
		.period_size = 1024,
		.period_count = 2,
		.format = PCM_FORMAT_S16_LE,
	};
	struct pcm *play = NULL, *capt;
	struct stat st;
	int card, dev, tries;
	int lockfd;

	lockfd = take_single_instance_lock();
	if (lockfd < 0) {
		ALOGI("another kebab-voicecall owns the voice path; exiting");
		return 0;
	}
	(void)lockfd; /* held until process exit */

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	/* ADSP and the audio card may still be coming up. */
	for (tries = 0; tries < 20 && !s_stop; tries++) {
		if (find_pcm(0, &card, &dev) == 0) {
			enable_routes(card);
			play = pcm_open(card, dev, PCM_OUT, &cfg);
			if (play && pcm_is_ready(play))
				break;
			if (play) {
				ALOGE("playback open: %s", pcm_get_error(play));
				pcm_close(play);
				play = NULL;
			}
		}
		sleep(1);
	}
	if (!play) {
		ALOGE("no %s playback PCM", PCM_NAME);
		return 1;
	}

	if (find_pcm(1, &card, &dev) == 0) {
		capt = pcm_open(card, dev, PCM_IN, &cfg);
		if (capt && !pcm_is_ready(capt)) {
			ALOGE("capture open: %s", pcm_get_error(capt));
			pcm_close(capt);
			capt = NULL;
		}
	} else {
		capt = NULL;
	}

	ALOGI("CS-VOICE held open (capture %s)", capt ? "up" : "unavailable");

	/* The RIL passes a pipe on fd 3 so dial can wait until the voice graph
	 * exists before the modem sees the call setup. */
	if (fstat(3, &st) == 0 && S_ISFIFO(st.st_mode)) {
		if (write(3, "1", 1) < 0)
			ALOGW("voice path notify: %s", strerror(errno));
		close(3);
	}

	while (!s_stop)
		pause();

	disable_routes(card);

	if (capt)
		pcm_close(capt);
	pcm_close(play);
	ALOGI("CS-VOICE released");
	return 0;
}
