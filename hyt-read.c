#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <stdarg.h>

#include <linux/i2c-dev.h>

static void die_errno(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	fprintf(stderr, ": %s\n", strerror(errno));
	exit(1);
}

static void die_alloc()
{
	fprintf(stderr, "failed to allocate memory\n");
	exit(1);
}

/* Does the file at <dir>/<subdir>/name contain the given i2c bus name? */
static int name_file_matches(const char *dir, const char *subdir,
			     const char *want)
{
	char *path;
	char *buf;
	int fd;
	int res = 0;
	size_t len = strlen(want);
	ssize_t got;

	if (asprintf(&path, "%s/%s/name", dir, subdir) < 2)
		die_alloc();

	fd = open(path, O_RDONLY);
	if (fd < 0)
		die_errno("opening %s", path);

	free(path);
	buf = malloc(len + 2);

	got = read(fd, buf, len + 2);
	if (got < 0)
		die_errno("reading %s", path);

	/* The file should contain a string the same length as the
	   name, except for the trailing newline. */
	if (got == len || (got == len + 1 && buf[len] == '\n'))
		res = (memcmp(want, buf, len) == 0);

	close(fd);
	free(buf);
	return res;
}

/* Open /dev/blah */
static int open_i2c_dev(const char *file)
{
	char *path;
	int fd;

	if (asprintf(&path, "/dev/%s", file) < 1)
		die_alloc();

	fd = open(path, O_RDWR);
	if (fd < 0)
		die_errno("opening %s", path);

	free(path);
	return fd;
}

/* Find the i2c bus with the given name, and open it. */
static int open_i2c_bus(const char *name)
{
	const char *dir = "/sys/class/i2c-dev";
	struct dirent *de;
	DIR *dp = opendir(dir);
	if (!dp)
		die_errno("opening directory %s", dir);

	for (;;) {
		errno = 0;
		de = readdir(dp);
		if (!de) {
			if (errno)
				die_errno("reading directory %s", dir);

			break;
		}

		if (de->d_name[0] == '.')
			continue;

		if (name_file_matches(dir, de->d_name, name)) {
			int fd = open_i2c_dev(de->d_name);
			closedir(dp);
			return fd;
		}
	}

	fprintf(stderr, "could not find i2c bus %s\n", name);
	exit(1);
}

struct reading {
	float humidity;
	float temperature;
};

static void take_reading(int fd, struct reading *r)
{
	unsigned char data[4];
	ssize_t sz;

	data[0] = 0;
	sz = write(fd, data, 1);
	if (sz < 0)
		die_errno("writing to i2c");

	/* wait for sensor to provide reading */
	usleep(60 * 1000);

	sz = read(fd, data, 4);
	if (sz < 0)
		die_errno("reading from i2c");

	if (sz < 4) {
		fprintf(stderr, "short read (%d bytes)\n", (int)sz);
		exit(1);
	}

	/*printf("%x %x %x %x\n", data[0], data[1], data[2], data[3]);*/

	/*
	 * Sensor reading are two bytes for humidity, and two bytes
	 * for temperature, big-endian.  The top two bits of the
	 * humidity value and the bottom two bits of the temperature
	 * value are status bits (of undocumented purpose).  Humidity
	 * readings range from 0 to 100%; temperature readings range
	 * from -40 to 120 degrees C.  In both cases, the ranges
	 * correspond to the full range of available bits.
	 */
	r->humidity = ((data[0] & 0x3f) << 8 | data[1]) * (100.0 / 0x3fff);
	r->temperature = (data[2] << 8 | (data[3] & 0xfc)) * (165.0 / 0xfffc) - 40;
}

void usage(void)
{
	printf("Usage: hyt-read [ -b I2C bus name | -d device file ] [ -a I2C slave address ]\n"
	       "                [ -i seconds ] [ -T ] [ -H ]\n"
	       "Options:\n"
	       "\t-b X\tOpen the I2C bus named X (e.g. bcm2708_i2c.1)\n"
	       "\t-d X\tOpen the I2C device named X (e.g. /dev/i2c-0)\n"
	       "\t-a X\tTarget I2C slave address X (default 0x28)\n"
	       "\t-i X\tRead data every X seconds\n"
	       "\t-T\tPrint only temperature\n"
	       "\t-H\tPrint only humidity\n"
	       "\t-h\tShow this message\n\n");
	exit(1);
}

void both_b_and_d(void)
{
	fprintf(stderr, "Cannot use both -d and -b options\n\n");
	usage();
}

int parse_i2c_slave_address(char *s)
{
	char *endptr;
	long n = strtol(s, &endptr, 0);

	if (endptr == s || *endptr != 0) {
		fprintf(stderr, "bad slave address '%s'\n", s);
		exit(1);
	}

	if (n < 0x3 || n > 0x77) {
		fprintf(stderr, "slave address %ld outside legal range\n", n);
		exit(1);
	}

	return n;
}

int main(int argc, char **argv)
{
	int fd = 0, c, interval = 0;
	int ptemp = 0, phum = 0;
	unsigned int slave = 0x28;

	while ((c = getopt (argc, argv, "HTd:b:i:a:h")) != -1) {
		switch (c) {
		case 'H':
			phum=1;
			break;

		case 'T':
			ptemp=1;
			break;

		case 'b':
			if (fd)
				both_b_and_d();

			fd = open_i2c_bus(optarg);
			break;

		case 'd':
			if (fd)
				both_b_and_d();

			fd = open(optarg, O_RDWR);
			if (fd < 0)
				die_errno("opening %s", optarg);

			break;

		case 'i':
			interval = atoi(optarg);
			break;

		case 'a':
			slave = parse_i2c_slave_address(optarg);
			break;

		case 'h':
			usage();
			break;
		}
	}

	if (optind != argc) {
		usage();
		return 1;
	}

	/* If the bus name was not specified, argv[optind] is the device file */
	if (!fd) {
		fprintf(stderr,
			"Either the -d or -b option must be present\n\n");
		usage();
	}

	/* If neither the -T not the -H option was specified, show both */
	if (ptemp == 0 && phum == 0)
		ptemp = phum = 1;

	if (ioctl(fd, I2C_SLAVE, slave) < 0)
		die_errno("ioctl(I2C_SLAVE)");

	/* initiate reading */
	do {
		char *sep = "";
		struct reading r;
		take_reading(fd, &r);

		if (phum) {
			printf("%f", r.humidity);
			sep = " ";
		}

		if (ptemp) {
			printf("%s%f", sep, r.temperature);
		}

		printf("\n");

		sleep(interval);
	} while (interval > 0);

	close(fd);
	return 0;
}
