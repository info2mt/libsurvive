#include <errno.h>
#include <math.h>

#include "os_generic.h"
#include "survive.h"

#include "survive_recording.h"
#include "survive_internal.h"

#include "survive_default_devices.h"

#include "survive_gz.h"

STATIC_CONFIG_ITEM(PLAYBACK_REPLAY_POSE, "playback-replay-pose", 'i', "Whether or not to output pose", 0)
STATIC_CONFIG_ITEM(PLAYBACK, "playback", 's', "File to be used for playback if playing a recording.", 0)
STATIC_CONFIG_ITEM(PLAYBACK_FACTOR, "playback-factor", 'f',
				   "Time factor of playback -- 1 is run at the same timing as original, 0 is run as fast as possible.",
				   1.0f)
STATIC_CONFIG_ITEM(PLAYBACK_TIME, "playback-time", 'f', "End time of playback", -1.0f)

STATIC_CONFIG_ITEM(PLAYBACK_RUN_TIME, "run-time", 'f', "How long to run for", -1.)


  
#ifdef _MSC_VER
typedef long ssize_t;
#define SSIZE_MAX LONG_MAX

ssize_t getdelim(char **lineptr, size_t *n, int delimiter, FILE *stream);
ssize_t getline(char **lineptr, size_t *n, FILE *stream);
#define RESTRICT_KEYWORD
#else
#define RESTRICT_KEYWORD restrict
#endif

ssize_t gzgetdelim(char **RESTRICT_KEYWORD lineptr, size_t *RESTRICT_KEYWORD n, int delimiter,
				   gzFile RESTRICT_KEYWORD stream);
ssize_t gzgetline(char **RESTRICT_KEYWORD lineptr, size_t *RESTRICT_KEYWORD n, gzFile RESTRICT_KEYWORD stream);

typedef struct SurvivePlaybackData {
    SurviveContext *ctx;
    const char *playback_dir;
    gzFile playback_file;
    int lineno;

    double next_time_s;
    double time_now;
    FLT playback_factor;
	FLT playback_time;
	bool hasRawLight;
    bool hasSweepAngle;
    bool outputExternalPose;

    uint32_t total_sleep_time;
    bool keepRunning;
    og_thread_t playback_thread;
} SurvivePlaybackData;

static double survive_playback_run_time(const SurviveContext *ctx, void *_sp) {
	const struct SurvivePlaybackData *sp = _sp;
	return sp->time_now;
}


static SurviveObject *find_or_warn(SurvivePlaybackData *driver, const char *dev) {
	SurviveContext *ctx = driver->ctx;
	SurviveObject *so = survive_get_so_by_name(driver->ctx, dev);
	if (!so) {
		static bool display_once = false;
		SurviveContext *ctx = driver->ctx;
		if (display_once == false) {
			SV_WARN("Could not find device named %s from lineno %d\r\n", dev, driver->lineno);
		}
		display_once = true;

		return 0;
	}
	return so;
}

static int parse_and_run_sweep(char *line, SurvivePlaybackData *driver) {
	char dev[10];

	survive_channel channel;
	int sensor_id;
	survive_timecode timecode;
	uint8_t flag;

	int rr = sscanf(line, SWEEP_SCANF, SWEEP_SCANF_ARGS);
	if (rr != 5) {
		SurviveContext *ctx = driver->ctx;
		SV_WARN("Only got %d values for a sweep", rr);
		return -1;
	}

	SurviveObject *so = find_or_warn(driver, dev);
	if (!so) {
		return 0;
	}

	driver->hasSweepAngle = true;
	driver->ctx->sweepproc(so, channel, sensor_id, timecode, flag);
	return 0;
}

static int parse_and_run_sync(char *line, SurvivePlaybackData *driver) {
	char dev[10];

	survive_channel channel;
	survive_timecode timecode;
	uint8_t ootx, gen;

	int rr = sscanf(line, SYNC_SCANF, SYNC_SCANF_ARGS);
	if (rr != 5) {
		SurviveContext *ctx = driver->ctx;
		SV_WARN("Only got %d values for a sync", rr);
		return -1;
	}

	SurviveObject *so = find_or_warn(driver, dev);
	if (!so) {
		return 0;
	}

	driver->ctx->syncproc(so, channel, timecode, ootx, gen);
	return 0;
}

static int parse_and_run_sweep_angle(char *line, SurvivePlaybackData *driver) {
	char dev[10];

	survive_channel channel;
	int sensor_id;
	survive_timecode timecode;
	int8_t plane;
	FLT angle;

	int rr = sscanf(line, SWEEP_ANGLE_SCANF, SWEEP_ANGLE_SCANF_ARGS);

	if (rr != 6) {
		SurviveContext *ctx = driver->ctx;
		SV_WARN("Only got %d values for sweep angle", rr);
		return -1;
	}

	SurviveObject *so = find_or_warn(driver, dev);
	if (!so) {
		return 0;
	}

	driver->ctx->sweep_angleproc(so, channel, sensor_id, timecode, plane, angle);
	return 0;
}

static int parse_and_run_pose(const char *line, SurvivePlaybackData *driver) {
	char name[128] = "replay_";
	SurvivePose pose;

	int rr = sscanf(line, "%s POSE " SurvivePose_sformat "\r\n", name + strlen(name), &pose.Pos[0], &pose.Pos[1],
					&pose.Pos[2], &pose.Rot[0], &pose.Rot[1], &pose.Rot[2], &pose.Rot[3]);

	SurviveContext *ctx = driver->ctx;
	if (rr != 8) {
		SV_WARN("Only got %d values for a pose", rr);
		return 0;
	}

	ctx->external_poseproc(ctx, name, &pose);
	return 0;
}
static int parse_and_run_imu(const char *line, SurvivePlaybackData *driver, bool raw) {
	char dev[10];
	int timecode = 0;
	FLT accelgyro[9] = { 0 };
	int mask;
	int id;
	SurviveContext *ctx = driver->ctx;

	char i_char = 0;

	int rr = sscanf(line,
					"%s %c %d %d " FLT_sformat " " FLT_sformat " " FLT_sformat " " FLT_sformat " " FLT_sformat
					" " FLT_sformat " " FLT_sformat " " FLT_sformat " " FLT_sformat "%d",
					dev, &i_char, &mask, &timecode, &accelgyro[0], &accelgyro[1], &accelgyro[2], &accelgyro[3],
					&accelgyro[4], &accelgyro[5], &accelgyro[6], &accelgyro[7], &accelgyro[8], &id);

	if (rr == 11) {
		// Older formats might not have mag data
		id = accelgyro[6];
		accelgyro[6] = 0;
	} else if (rr != 14) {
		SV_WARN("On line %d, only %d values read: '%s'", driver->lineno, rr, line);
		return -1;
	}

	assert(raw ^ i_char == 'I');

	SurviveObject *so = find_or_warn(driver, dev);
	if (so)
		(raw ? driver->ctx->raw_imuproc : driver->ctx->imuproc)(so, mask, accelgyro, timecode, id);
	return 0;
}

static int parse_and_run_lhpose(const char *line, struct SurvivePlaybackData *driver) {
	SurvivePose pose;
	int lh = -1;
	int rr = sscanf(line, "%d LH_POSE " SurvivePose_sformat "\n", &lh, &pose.Pos[0], &pose.Pos[1], &pose.Pos[2],
					&pose.Rot[0], &pose.Rot[1], &pose.Rot[2], &pose.Rot[3]);

	SurviveContext *ctx = driver->ctx;
	if (driver->outputExternalPose) {
		char buffer[32] = {0};
		snprintf(buffer, 31, "previous_LH%d", lh);
		ctx->external_poseproc(ctx, buffer, &pose);
	}
	return 0;
}

static int parse_and_run_externalpose(const char *line, SurvivePlaybackData *driver) {
	char name[128] = { 0 };
	SurvivePose pose;

	int rr = sscanf(line, "%s EXTERNAL_POSE " SurvivePose_sformat "\n", name, &pose.Pos[0], &pose.Pos[1], &pose.Pos[2],
					&pose.Rot[0], &pose.Rot[1], &pose.Rot[2], &pose.Rot[3]);

	SurviveContext *ctx = driver->ctx;
	ctx->external_poseproc(ctx, name, &pose);
	return 0;
}

static int parse_and_run_rawlight(const char *line, SurvivePlaybackData *driver) {
	driver->hasRawLight = 1;

	char dev[10];
	char op[10];
	LightcapElement le;
	int rr = sscanf(line, "%s %s %hhu %u %hu\n", dev, op, &le.sensor_id, &le.timestamp, &le.length);

	SurviveObject *so = find_or_warn(driver, dev);

	handle_lightcap(so, &le);
	return 0;
}

static int parse_and_run_lightcode(const char *line, SurvivePlaybackData *driver) {
	char lhn[10];
	char axn[10];
	char dev[10];
	uint32_t timecode = 0;
	int sensor_id = 0;
	int acode = 0;
	int timeinsweep = 0;
	uint32_t length = 0;
	uint32_t lh = 0;
	SurviveContext *ctx = driver->ctx;

	int rr = sscanf(line, "%8s %8s %8s %d %d %d %u %u %u\n", dev, lhn, axn, &sensor_id, &acode, &timeinsweep, &timecode,
					&length, &lh);

	if (rr != 9) {
		SV_WARN("Warning:  On line %d, only %d values read: '%s'\n", driver->lineno, rr, line);
		return -1;
	}

	SurviveObject *so = find_or_warn(driver, dev);
	if (so)
		driver->ctx->lightproc(so, sensor_id, acode, timeinsweep, timecode, length, lh);
	return 0;
}



static int playback_pump_msg(struct SurviveContext *ctx, void *_driver) {
	SurvivePlaybackData *driver = _driver;
	gzFile f = driver->playback_file;

	if (f && !gzeof(f) && !gzerror_dropin(f)) {
		driver->lineno++;
		char *line = 0;

		if (driver->next_time_s == 0) {
			size_t n = 0;
			ssize_t r = gzgetdelim(&line, &n, ' ', f);
			if (r <= 0) {
				free(line);
				return 0;
			}

			if (sscanf(line, "%lf", &driver->next_time_s) != 1) {
				free(line);
				return 0;
			}

			if(!isfinite(driver->next_time_s)) {
				driver->next_time_s = 0;
			}
			free(line);
			line = 0;
		}

		if (driver->next_time_s * driver->playback_factor > OGRelativeTime())
			return 0;

		driver->time_now = driver->next_time_s;
		driver->next_time_s = 0;

		size_t n = 0;
		ssize_t r = gzgetline(&line, &n, f);

		if (r <= 0) {
			free(line);
			return 0;
		}
		while (r && (line[r - 1] == '\n' || line[r - 1] == '\r')) {
			line[--r] = 0;
		}
		char dev[32];
		char op[32];
		if (sscanf(line, "%31s %31s", dev, op) < 2) {
			free(line);
			return 0;
		}

		if (strcmp(dev, "OPTION") == 0) {
			free(line);
			return 0;
		}

		survive_get_ctx_lock(ctx);
		switch (op[0]) {
		case 'W':
			if (op[1] == 0)
				parse_and_run_sweep(line, driver);
			break;
		case 'B':
			if (op[1] == 0 && driver->hasSweepAngle == false)
				parse_and_run_sweep_angle(line, driver);
			break;
		case 'Y':
			if (op[1] == 0)
				parse_and_run_sync(line, driver);
			break;
		case 'E':
			if (strcmp(op, "EXTERNAL_POSE") == 0) {
				parse_and_run_externalpose(line, driver);
				break;
			}
		case 'C':
			if (op[1] == 0)
				parse_and_run_rawlight(line, driver);
			break;
		case 'L':
			if (strcmp(op, "LH_POSE") == 0) {
				parse_and_run_lhpose(line, driver);
				break;
			}
		case 'R':
			if (op[1] == 0 && driver->hasRawLight == false)
				parse_and_run_lightcode(line, driver);
			break;
		case 'i':
			if (op[1] == 0)
				parse_and_run_imu(line, driver, true);
			break;
		case 'I':
			if (op[1] == 0)
				parse_and_run_imu(line, driver, false);
			break;
		case 'P':
			if (strcmp(op, "POSE") == 0 && driver->outputExternalPose)
				parse_and_run_pose(line, driver);
			break;
		case 'A':
		case 'V':
			break;
		default:
			SV_WARN("Playback doesn't understand '%s' op in '%s'", op, line);
		}
		survive_release_ctx_lock(ctx);

		free(line);
	} else {
		SV_VERBOSE(100, "EOF for playback received.");
		if (f) {
			gzclose(driver->playback_file);
		}
		driver->playback_file = 0;
		return -1;
	}

	return 0;
}

static void *playback_thread(void *_driver) {
	SurvivePlaybackData *driver = _driver;
	while (driver->keepRunning) {
		double next_time_s_scaled = driver->next_time_s * driver->playback_factor;
		double time_now = OGRelativeTime();
		if (driver->playback_time >= 0 && driver->time_now > driver->playback_time) {
			driver->keepRunning = false;
			return 0;
		}
		if (next_time_s_scaled == 0 || next_time_s_scaled < time_now) {
			int rtnVal = playback_pump_msg(driver->ctx, driver);
			if (rtnVal < 0)
				driver->keepRunning = false;
		} else {
			int sleep_time_ms = 1 + (next_time_s_scaled - time_now) * 1000.;
			int sr = OGUSleep(sleep_time_ms * 1000);
			if (sr == 0)
				driver->total_sleep_time += sleep_time_ms;
		}
	}
	return 0;
}

static int playback_poll(struct SurviveContext *ctx, void *_driver) {
	SurvivePlaybackData *driver = _driver;
	if (driver->keepRunning == false)
		return -1;
	return 0;
}

static int playback_close(struct SurviveContext *ctx, void *_driver) {
	SurvivePlaybackData *driver = _driver;
	driver->keepRunning = false;
	SV_VERBOSE(100, "Waiting on playback thread...");
	survive_release_ctx_lock(ctx);
	OGJoinThread(driver->playback_thread);
	survive_get_ctx_lock(ctx);
	SV_VERBOSE(50, "Playback thread slept for %" PRIu32 "ms", driver->total_sleep_time);
	SV_VERBOSE(10, "Playback thread played back %6.2fs in %6.2fs real-time", driver->time_now, OGRelativeTime());
	if (driver->playback_file)
		gzclose(driver->playback_file);
	driver->playback_file = 0;

	survive_detach_config(ctx, "playback-factor", &driver->playback_factor);
	survive_detach_config(ctx, "playback-time", &driver->playback_time);
	survive_install_run_time_fn(ctx, 0, 0);
	free(driver);
	return 0;
}

int DriverRegPlayback(SurviveContext *ctx) {
	const char *playback_file = survive_configs(ctx, "playback", SC_GET, 0);

	if (playback_file == 0 || strlen(playback_file) == 0) {
		SV_WARN("The playback argument requires a filename");
		return -1;
	}

	if (strstr(playback_file, ".pcap")) {
		int (*usb_driver)(SurviveContext *) = (int (*)(SurviveContext *))GetDriver("DriverRegUSBMon_Playback");
		if (usb_driver) {
			return usb_driver(ctx);
		}
		SV_WARN("Playback file %s is a USB packet capture, but the usbmon playback driver does not exist.",
				playback_file);
		return -1;
	}

	SurvivePlaybackData *sp = SV_CALLOC(1, sizeof(SurvivePlaybackData));
	sp->ctx = ctx;
	sp->playback_dir = playback_file;

	sp->outputExternalPose = survive_configi(ctx, "playback-replay-pose", SC_GET, 0);

	sp->playback_file = gzopen(playback_file, "r");
	if (sp->playback_file == 0) {
		SV_ERROR(SURVIVE_ERROR_INVALID_CONFIG, "Could not open playback events file %s", playback_file);
		return -1;
	}
	survive_install_run_time_fn(ctx, survive_playback_run_time, sp);
	survive_attach_configf(ctx, "playback-factor", &sp->playback_factor);
	survive_attach_configf(ctx, "playback-time", &sp->playback_time);

	SV_INFO("Using playback file '%s' with timefactor of %f until %f", playback_file, sp->playback_factor,
			sp->playback_time);

	ctx->poll_min_time_ms = 1;
	if (sp->playback_factor == 0.0)
		ctx->poll_min_time_ms = 0;

	FLT time;
	while (!gzeof(sp->playback_file) && !gzerror_dropin(sp->playback_file)) {
		char *line = 0;
		size_t n;
		int r = gzgetline(&line, &n, sp->playback_file);

		if (r <= 0) {
			free(line);
			continue;
		}

		if (line[0] == 0x1f) {
			SV_ERROR(SURVIVE_ERROR_INVALID_CONFIG, "Attempting to playback a gz compressed file without gz support.");
			free(line);
			return -1;
		}

		char dev[32];
		char command[32];

		if (sscanf(line, FLT_sformat " %s %s", &time, dev, command) != 3) {
			free(line);
			break;
		}

		// 60 seconds is enough time for all configurations; don't read the whole file -- could be huge
		if (time > 60) {
			free(line);
			break;
		}

		if (strcmp(command, "CONFIG") == 0) {
			char *configStart = line;

			// Skip three spaces
			for (int i = 0; i < 3; i++) {
				while (*(++configStart) != ' ')
					;
			}
			size_t len = strlen(configStart);

			SurviveObject *so = survive_create_device(ctx, "replay", sp, dev, 0);

			char *config = SV_CALLOC(1, len + 1);
			memcpy(config, configStart, len);

			if (ctx->configproc(so, config, len) == 0) {
				SV_INFO("Found %s in playback file...", dev);
				survive_add_object(ctx, so);
			} else {
				SV_WARN("Found %s in playback file, but could not read config description", dev);
				free(so);
			}
		}

		free(line);
	}

	gzseek(sp->playback_file, 0, SEEK_SET); // same as rewind(f);

	sp->keepRunning = true;
	sp->playback_thread = OGCreateThread(playback_thread, "playback", sp);

	survive_add_driver(ctx, sp, playback_poll, playback_close);
	return 0;
}

REGISTER_LINKTIME(DriverRegPlayback)

  
#define _GETDELIM_GROWBY 128 /* amount to grow line buffer by */
#define _GETDELIM_MINLEN 4   /* minimum line buffer size */

ssize_t gzgetdelim(char **RESTRICT_KEYWORD lineptr, size_t *RESTRICT_KEYWORD n, int delimiter,
				   gzFile RESTRICT_KEYWORD stream) {
	char *buf, *pos;
	int c;
	ssize_t bytes;

	if (lineptr == NULL || n == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (stream == NULL) {
		errno = EBADF;
		return -1;
	}

	/* resize (or allocate) the line buffer if necessary */
	buf = *lineptr;
	if (buf == NULL || *n < _GETDELIM_MINLEN) {
		buf = SV_REALLOC(*lineptr, _GETDELIM_GROWBY);
		if (buf == NULL) {
			/* ENOMEM */
			return -1;
		}
		*n = _GETDELIM_GROWBY;
		*lineptr = buf;
	}

	/* read characters until delimiter is found, end of file is reached, or an
	   error occurs. */
	bytes = 0;
	pos = buf;
	while ((c = gzgetc(stream)) != EOF) {
		if (bytes + 1 >= (SIZE_MAX / 2)) {
			errno = EOVERFLOW;
			return -1;
		}
		bytes++;
		if (bytes >= *n - 1) {
			buf = SV_REALLOC(*lineptr, *n + _GETDELIM_GROWBY);
			if (buf == NULL) {
				/* ENOMEM */
				return -1;
			}
			*n += _GETDELIM_GROWBY;
			pos = buf + bytes - 1;
			*lineptr = buf;
		}

		*pos++ = (char)c;
		if (c == delimiter) {
			break;
		}
	}

	if (gzerror_dropin(stream) || (gzeof(stream) && (bytes == 0))) {
		/* EOF, or an error from getc(). */
		return -1;
	}

	*pos = '\0';
	return bytes;
}

ssize_t gzgetline(char **RESTRICT_KEYWORD lineptr, size_t *RESTRICT_KEYWORD n, gzFile RESTRICT_KEYWORD stream) {
	return gzgetdelim(lineptr, n, '\n', stream);
}
