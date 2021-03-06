/*
 * rtcwake -- enter a system sleep state until specified wakeup time.
 *
 * This uses cross-platform Linux interfaces to enter a system sleep state,
 * and leave it no later than a specified time.  It uses any RTC framework
 * driver that supports standard driver model wakeup flags.
 *
 * This is normally used like the old "apmsleep" utility, to wake from a
 * suspend state like ACPI S1 (standby) or S3 (suspend-to-RAM).  Most
 * platforms can implement those without analogues of BIOS, APM, or ACPI.
 *
 * On some systems, this can also be used like "nvram-wakeup", waking
 * from states like ACPI S4 (suspend to disk).  Not all systems have
 * persistent media that are appropriate for such suspend modes.
 *
 * The best way to set the system's RTC is so that it holds the current
 * time in UTC.  Use the "-l" flag to tell this program that the system
 * RTC uses a local timezone instead (maybe you dual-boot MS-Windows).
 * That flag should not be needed on systems with adjtime support.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/rtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "c.h"
#include "closestream.h"
#include "nls.h"
#include "optutils.h"
#include "pathnames.h"
#include "strutils.h"
#include "strv.h"
#include "timeutils.h"
#include "xalloc.h"

#ifndef RTC_AF
# define RTC_AF		0x20	/* Alarm interrupt */
#endif

#define ADJTIME_ZONE_BUFSIZ		8
#define SYS_WAKEUP_PATH_TEMPLATE	"/sys/class/rtc/%s/device/power/wakeup"
#define SYS_POWER_STATE_PATH		"/sys/power/state"
#define DEFAULT_RTC_DEVICE		"/dev/rtc0"

enum rtc_modes {	/* manual page --mode option explains these. */
	OFF_MODE = 0,
	NO_MODE,
	ON_MODE,
	DISABLE_MODE,
	SHOW_MODE,

	SYSFS_MODE	/* keep it last */

};

static const char *rtcwake_mode_string[] = {
	[OFF_MODE] = "off",
	[NO_MODE] = "no",
	[ON_MODE] = "on",
	[DISABLE_MODE] = "disable",
	[SHOW_MODE] = "show"
};

enum clock_modes {
	CM_AUTO,
	CM_UTC,
	CM_LOCAL
};

struct rtcwake_control {
	char *mode_str;			/* name of the requested mode */
	char **possible_modes;		/* modes listed in /sys/power/state */
	char *adjfile;			/* adjtime file path */
	enum clock_modes clock_mode;	/* hwclock timezone */
	time_t sys_time;		/* system time */
	time_t rtc_time;		/* hardware time */
	unsigned int verbose:1,		/* verbose messaging */
		     dryrun:1;		/* do not set alarm, suspend system, etc */
};

static void __attribute__((__noreturn__)) usage(FILE *out)
{
	fputs(USAGE_HEADER, out);
	fprintf(out,
	      _(" %s [options]\n"), program_invocation_short_name);

	fputs(USAGE_SEPARATOR, out);
	fputs(_("Enter a system sleep state until a specified wakeup time.\n"), out);

	fputs(USAGE_OPTIONS, out);
	fputs(_(" -a, --auto               reads the clock mode from adjust file (default)\n"), out);
	fprintf(out,
	      _(" -A, --adjfile <file>     specifies the path to the adjust file\n"
		"                            the default is %s\n"), _PATH_ADJTIME);
	fputs(_("     --date <timestamp>   date time of timestamp to wake\n"), out);
	fputs(_(" -d, --device <device>    select rtc device (rtc0|rtc1|...)\n"), out);
	fputs(_(" -n, --dry-run            does everything, but suspend\n"), out);
	fputs(_(" -l, --local              RTC uses local timezone\n"), out);
	fputs(_("     --list-modes         list available modes\n"), out);
	fputs(_(" -m, --mode <mode>        standby|mem|... sleep mode\n"), out);
	fputs(_(" -s, --seconds <seconds>  seconds to sleep\n"), out);
	fputs(_(" -t, --time <time_t>      time to wake\n"), out);
	fputs(_(" -u, --utc                RTC uses UTC\n"), out);
	fputs(_(" -v, --verbose            verbose messages\n"), out);

	printf(USAGE_SEPARATOR);
	printf(USAGE_HELP);
	printf(USAGE_VERSION);

	printf(USAGE_MAN_TAIL("rtcwake(8)"));

	exit(out == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}

static int is_wakeup_enabled(const char *devname)
{
	char	buf[128], *s;
	FILE	*f;
	size_t	skip = 0;

	if (startswith(devname, "/dev/"))
		skip = 5;
	snprintf(buf, sizeof buf, SYS_WAKEUP_PATH_TEMPLATE, devname + skip);
	f = fopen(buf, "r");
	if (!f) {
		warn(_("cannot open %s"), buf);
		return 0;
	}

	s = fgets(buf, sizeof buf, f);
	fclose(f);
	if (!s)
		return 0;
	s = strchr(buf, '\n');
	if (!s)
		return 0;
	*s = 0;
	/* wakeup events could be disabled or not supported */
	return strcmp(buf, "enabled") == 0;
}

static int get_basetimes(struct rtcwake_control *ctl, int fd)
{
	struct tm tm = { 0 };
	struct rtc_time	rtc;

	/* This process works in RTC time, except when working
	 * with the system clock (which always uses UTC).
	 */
	if (ctl->clock_mode == CM_UTC)
		setenv("TZ", "UTC", 1);
	tzset();
	/* Read rtc and system clocks "at the same time", or as
	 * precisely (+/- a second) as we can read them.
	 */
	if (ioctl(fd, RTC_RD_TIME, &rtc) < 0) {
		warn(_("read rtc time failed"));
		return -1;
	}

	ctl->sys_time = time(0);
	if (ctl->sys_time == (time_t)-1) {
		warn(_("read system time failed"));
		return -1;
	}
	/* Convert rtc_time to normal arithmetic-friendly form,
	 * updating tm.tm_wday as used by asctime().
	 */
	tm.tm_sec = rtc.tm_sec;
	tm.tm_min = rtc.tm_min;
	tm.tm_hour = rtc.tm_hour;
	tm.tm_mday = rtc.tm_mday;
	tm.tm_mon = rtc.tm_mon;
	tm.tm_year = rtc.tm_year;
	tm.tm_isdst = -1;  /* assume the system knows better than the RTC */

	ctl->rtc_time = mktime(&tm);
	if (ctl->rtc_time == (time_t)-1) {
		warn(_("convert rtc time failed"));
		return -1;
	}

	if (ctl->verbose) {
		/* Unless the system uses UTC, either delta or tzone
		 * reflects a seconds offset from UTC.  The value can
		 * help sort out problems like bugs in your C library. */
		printf("\tdelta   = %ld\n", ctl->sys_time - ctl->rtc_time);
		printf("\ttzone   = %ld\n", timezone);
		printf("\ttzname  = %s\n", tzname[daylight]);
		gmtime_r(&ctl->rtc_time, &tm);
		printf("\tsystime = %ld, (UTC) %s",
				(long) ctl->sys_time, asctime(gmtime(&ctl->sys_time)));
		printf("\trtctime = %ld, (UTC) %s",
				(long) ctl->rtc_time, asctime(&tm));
	}
	return 0;
}

static int setup_alarm(struct rtcwake_control *ctl, int fd, time_t *wakeup)
{
	struct tm		*tm;
	struct rtc_wkalrm	wake = { 0 };

	/* The wakeup time is in POSIX time (more or less UTC).  Ideally
	 * RTCs use that same time; but PCs can't do that if they need to
	 * boot MS-Windows.  Messy...
	 *
	 * When clock_mode == CM_UTC this process's timezone is UTC, so
	 * we'll pass a UTC date to the RTC.
	 *
	 * Else clock_mode == CM_LOCAL so the time given to the RTC will
	 * instead use the local time zone. */
	tm = localtime(wakeup);
	wake.time.tm_sec = tm->tm_sec;
	wake.time.tm_min = tm->tm_min;
	wake.time.tm_hour = tm->tm_hour;
	wake.time.tm_mday = tm->tm_mday;
	wake.time.tm_mon = tm->tm_mon;
	wake.time.tm_year = tm->tm_year;
	/* wday, yday, and isdst fields are unused */
	wake.time.tm_wday = -1;
	wake.time.tm_yday = -1;
	wake.time.tm_isdst = -1;
	wake.enabled = 1;

	if (!ctl->dryrun && ioctl(fd, RTC_WKALM_SET, &wake) < 0) {
		warn(_("set rtc wake alarm failed"));
		return -1;
	}
	return 0;
}

static char **get_sys_power_states(struct rtcwake_control *ctl)
{
	int fd = -1;

	if (!ctl->possible_modes) {
		char buf[256] = { 0 };

		fd = open(SYS_POWER_STATE_PATH, O_RDONLY);
		if (fd < 0)
			goto nothing;
		if (read(fd, &buf, sizeof buf) <= 0)
			goto nothing;
		ctl->possible_modes = strv_split(buf, " \n");
		close(fd);
	}
	return ctl->possible_modes;
nothing:
	if (fd >= 0)
		close(fd);
	return NULL;
}

static void suspend_system(struct rtcwake_control *ctl)
{
	FILE	*f = fopen(SYS_POWER_STATE_PATH, "w");

	if (!f) {
		warn(_("cannot open %s"), SYS_POWER_STATE_PATH);
		return;
	}

	if (!ctl->dryrun) {
		fprintf(f, "%s\n", ctl->mode_str);
		fflush(f);
	}
	/* this executes after wake from suspend */
	if (close_stream(f))
		errx(EXIT_FAILURE, _("write error"));
}

static int read_clock_mode(struct rtcwake_control *ctl)
{
	FILE *fp;
	char linebuf[ADJTIME_ZONE_BUFSIZ];

	fp = fopen(ctl->adjfile, "r");
	if (!fp)
		return -1;
	/* skip two lines */
	if (skip_fline(fp) || skip_fline(fp)) {
		fclose(fp);
		return -1;
	}
	/* read third line */
	if (!fgets(linebuf, sizeof linebuf, fp)) {
		fclose(fp);
		return -1;
	}

	if (strncmp(linebuf, "UTC", 3) == 0)
		ctl->clock_mode = CM_UTC;
	else if (strncmp(linebuf, "LOCAL", 5) == 0)
		ctl->clock_mode = CM_LOCAL;
	else if (ctl->verbose)
		warnx(_("unexpected third line in: %s: %s"), ctl->adjfile, linebuf);

	fclose(fp);
	return 0;
}

static int print_alarm(struct rtcwake_control *ctl, int fd)
{
	struct rtc_wkalrm wake;
	struct tm tm = { 0 };
	time_t alarm;

	if (ioctl(fd, RTC_WKALM_RD, &wake) < 0) {
		warn(_("read rtc alarm failed"));
		return -1;
	}

	if (wake.enabled != 1 || wake.time.tm_year == -1) {
		printf(_("alarm: off\n"));
		return 0;
	}
	tm.tm_sec = wake.time.tm_sec;
	tm.tm_min = wake.time.tm_min;
	tm.tm_hour = wake.time.tm_hour;
	tm.tm_mday = wake.time.tm_mday;
	tm.tm_mon = wake.time.tm_mon;
	tm.tm_year = wake.time.tm_year;
	tm.tm_isdst = -1;  /* assume the system knows better than the RTC */

	alarm = mktime(&tm);
	if (alarm == (time_t)-1) {
		warn(_("convert time failed"));
		return -1;
	}
	/* 0 if both UTC, or expresses diff if RTC in local time */
	alarm += ctl->sys_time - ctl->rtc_time;
	printf(_("alarm: on  %s"), ctime(&alarm));

	return 0;
}

static int get_rtc_mode(struct rtcwake_control *ctl, const char *optarg)
{
	size_t i;
	char **modes = get_sys_power_states(ctl), **m;

	STRV_FOREACH(m, modes) {
		if (strcmp(optarg, *m) == 0)
			return SYSFS_MODE;
	}

	for (i = 0; i < ARRAY_SIZE(rtcwake_mode_string); i++)
		if (!strcmp(optarg, rtcwake_mode_string[i]))
			return i;

	return -EINVAL;
}

static int open_dev_rtc(const char *devname)
{
	int fd;
	char *devpath = NULL;

	if (startswith(devname, "/dev"))
		devpath = xstrdup(devname);
	else
		xasprintf(&devpath, "/dev/%s", devname);
	fd = open(devpath, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		err(EXIT_FAILURE, _("%s: unable to find device"), devpath);
	free(devpath);
	return fd;
}

static void list_modes(struct rtcwake_control *ctl)
{
	size_t i;
	char **modes = get_sys_power_states(ctl), **m;

	if (!modes)
		errx(EXIT_FAILURE, _("could not read: %s"), SYS_POWER_STATE_PATH);

	STRV_FOREACH(m, modes)
		printf("%s ", *m);

	for (i = 0; i < ARRAY_SIZE(rtcwake_mode_string); i++)
		printf("%s ", rtcwake_mode_string[i]);
	putchar('\n');
}

int main(int argc, char **argv)
{
	struct rtcwake_control ctl = {
		.mode_str = "suspend",		/* default mode */
		.adjfile = _PATH_ADJTIME,
		.clock_mode = CM_AUTO
	};
	char *devname = DEFAULT_RTC_DEVICE;
	unsigned seconds = 0;
	int suspend = SYSFS_MODE;
	int rc = EXIT_SUCCESS;
	int t;
	int fd;
	time_t alarm = 0;
	enum {
		OPT_DATE = CHAR_MAX + 1,
		OPT_LIST
	};
	static const struct option long_options[] = {
		{"adjfile",     required_argument,      0, 'A'},
		{"auto",	no_argument,		0, 'a'},
		{"dry-run",	no_argument,		0, 'n'},
		{"local",	no_argument,		0, 'l'},
		{"utc",		no_argument,		0, 'u'},
		{"verbose",	no_argument,		0, 'v'},
		{"version",	no_argument,		0, 'V'},
		{"help",	no_argument,		0, 'h'},
		{"mode",	required_argument,	0, 'm'},
		{"device",	required_argument,	0, 'd'},
		{"seconds",	required_argument,	0, 's'},
		{"time",	required_argument,	0, 't'},
		{"date",	required_argument,	0, OPT_DATE},
		{"list-modes",	no_argument,		0, OPT_LIST},
		{0,		0,			0, 0  }
	};
	static const ul_excl_t excl[] = {
		{ 'a', 'l', 'u' },
		{ 's', 't', OPT_DATE },
	};
	int excl_st[ARRAY_SIZE(excl)] = UL_EXCL_STATUS_INIT;

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	atexit(close_stdout);

	while ((t = getopt_long(argc, argv, "A:ahd:lm:ns:t:uVv",
					long_options, NULL)) != EOF) {
		err_exclusive_options(t, long_options, excl, excl_st);
		switch (t) {
		case 'A':
			/* for better compatibility with hwclock */
			ctl.adjfile = optarg;
			break;
		case 'a':
			ctl.clock_mode = CM_AUTO;
			break;
		case 'd':
			devname = optarg;
			break;
		case 'l':
			ctl.clock_mode = CM_LOCAL;
			break;

		case OPT_LIST:
			list_modes(&ctl);
			return EXIT_SUCCESS;

		case 'm':
			if ((suspend = get_rtc_mode(&ctl, optarg)) < 0)
				errx(EXIT_FAILURE, _("unrecognized suspend state '%s'"), optarg);
			ctl.mode_str = optarg;
			break;
		case 'n':
			ctl.dryrun = 1;
			break;
		case 's':
			/* alarm time, seconds-to-sleep (relative) */
			seconds = strtou32_or_err(optarg, _("invalid seconds argument"));
			break;
		case 't':
			/* alarm time, time_t (absolute, seconds since epoc) */
			alarm = strtou32_or_err(optarg, _("invalid time argument"));
			break;
		case OPT_DATE:
		{	/* alarm time, see timestamp format from manual */
			usec_t p;
			if (parse_timestamp(optarg, &p) < 0)
				errx(EXIT_FAILURE, _("invalid time value \"%s\""), optarg);
			alarm = (time_t) (p / 1000000);
			break;
		}
		case 'u':
			ctl.clock_mode = CM_UTC;
			break;
		case 'v':
			ctl.verbose = 1;
			break;
		case 'V':
			printf(UTIL_LINUX_VERSION);
			exit(EXIT_SUCCESS);
		case 'h':
			usage(stdout);
		default:
			usage(stderr);
		}
	}

	if (ctl.clock_mode == CM_AUTO) {
		if (read_clock_mode(&ctl) < 0) {
			printf(_("%s: assuming RTC uses UTC ...\n"),
					program_invocation_short_name);
			ctl.clock_mode = CM_UTC;
		}
	}

	if (ctl.verbose)
		printf("%s",  ctl.clock_mode == CM_UTC ? _("Using UTC time.\n") :
				_("Using local time.\n"));

	if (!alarm && !seconds && suspend != DISABLE_MODE && suspend != SHOW_MODE)
		errx(EXIT_FAILURE, _("must provide wake time (see --seconds, --time and --date options)"));

	/* device must exist and (if we'll sleep) be wakeup-enabled */
	fd = open_dev_rtc(devname);

	if (suspend != ON_MODE && suspend != NO_MODE && !is_wakeup_enabled(devname))
		errx(EXIT_FAILURE, _("%s not enabled for wakeup events"), devname);

	/* relative or absolute alarm time, normalized to time_t */
	if (get_basetimes(&ctl, fd) < 0)
		exit(EXIT_FAILURE);

	if (ctl.verbose)
		printf(_("alarm %ld, sys_time %ld, rtc_time %ld, seconds %u\n"),
				alarm, ctl.sys_time, ctl.rtc_time, seconds);

	if (suspend != DISABLE_MODE && suspend != SHOW_MODE) {
		/* perform alarm setup when the show or disable modes are not set */
		if (alarm) {
			if (alarm < ctl.sys_time)
				errx(EXIT_FAILURE, _("time doesn't go backward to %s"),
						ctime(&alarm));
			alarm += ctl.sys_time - ctl.rtc_time;
		} else
			alarm = ctl.rtc_time + seconds + 1;

		if (setup_alarm(&ctl, fd, &alarm) < 0)
			exit(EXIT_FAILURE);

		if (suspend == NO_MODE || suspend == ON_MODE)
			printf(_("%s: wakeup using %s at %s"),
				program_invocation_short_name, devname,
				ctime(&alarm));
		else
			printf(_("%s: wakeup from \"%s\" using %s at %s"),
				program_invocation_short_name, ctl.mode_str, devname,
				ctime(&alarm));
		fflush(stdout);
		xusleep(10 * 1000);
	}

	switch (suspend) {
	case NO_MODE:
		if (ctl.verbose)
			printf(_("suspend mode: no; leaving\n"));
		ctl.dryrun = 1;	/* to skip disabling alarm at the end */
		break;
	case OFF_MODE:
	{
		char *arg[5];
		int i = 0;

		if (ctl.verbose)
			printf(_("suspend mode: off; executing %s\n"),
						_PATH_SHUTDOWN);
		arg[i++] = _PATH_SHUTDOWN;
		arg[i++] = "-h";
		arg[i++] = "-P";
		arg[i++] = "now";
		arg[i]   = NULL;
		if (!ctl.dryrun) {
			execv(arg[0], arg);
			warn(_("failed to execute %s"), _PATH_SHUTDOWN);
			rc = EXIT_FAILURE;
		}
		break;
	}
	case ON_MODE:
	{
		unsigned long data;

		if (ctl.verbose)
			printf(_("suspend mode: on; reading rtc\n"));
		if (!ctl.dryrun) {
			do {
				t = read(fd, &data, sizeof data);
				if (t < 0) {
					warn(_("rtc read failed"));
					break;
				}
				if (ctl.verbose)
					printf("... %s: %03lx\n", devname, data);
			} while (!(data & RTC_AF));
		}
		break;
	}
	case DISABLE_MODE:
		/* just break, alarm gets disabled in the end */
		if (ctl.verbose)
			printf(_("suspend mode: disable; disabling alarm\n"));
		break;
	case SHOW_MODE:
		if (ctl.verbose)
			printf(_("suspend mode: show; printing alarm info\n"));
		if (print_alarm(&ctl, fd))
			rc = EXIT_FAILURE;
		ctl.dryrun = 1;	/* don't really disable alarm in the end, just show */
		break;
	default:
		if (ctl.verbose)
			printf(_("suspend mode: %s; suspending system\n"), ctl.mode_str);
		sync();
		suspend_system(&ctl);
	}

	if (!ctl.dryrun) {
		struct rtc_wkalrm wake;

		if (ioctl(fd, RTC_WKALM_RD, &wake) < 0) {
			warn(_("read rtc alarm failed"));
			rc = EXIT_FAILURE;
		} else {
			wake.enabled = 0;
			if (ioctl(fd, RTC_WKALM_SET, &wake) < 0) {
				warn(_("disable rtc alarm interrupt failed"));
				rc = EXIT_FAILURE;
			}
		}
	}

	close(fd);
	return rc;
}
