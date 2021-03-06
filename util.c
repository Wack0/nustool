#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "types.h"
#include "util.h"
#include "version.h"

#ifdef _WIN32
	#define MAKEDIR(A, B) _mkdir(A)
#else
	#define MAKEDIR(A, B) mkdir(A, B)
#endif

__attribute__((noreturn)) void oom(void)
{
	fputs("Out of memory.", stderr);
	exit(EXIT_FAILURE);
}

__attribute__((format(printf, 1, 2))) void err(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	fputc('\n', stderr);
}

__attribute__((format(printf, 1, 2))) void msg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);

	fputc('\n', stdout);
}

static void usage(const char *name)
{
	err("Usage: %s [-cDmpr] [-k decrypted_key] [-K encrypted_key]\n"
			"       [-V version] titleid",
	/* The C standard does not guarantee that argv[0] is non-NULL, but does
	 * guarantee that argv[argc] is NULL.
	 *
	 * Similarly, passing NULL as %s is undefined behavior, so we try to
	 * work around these odd conditions.
	 */
	name ? name : "(?)");
}

static void help(const char *name)
{
	usage(name);
	err("\nDownloads and optionally decrypts a title from NUS.\n"
	"\n"
	" -c              try to decrypt the title using the CETK key\n"
	" -D              if decrypting the title using the CETK key, use the\n"
	"                 development common key to decrypt the titlekey\n"
	" -k [key]        the titlekey to use to decrypt the contents\n"
	" -K [key]        the encrypted titlekey to use to decrypt the contents\n"
	" -h              print this help and exit\n"
	" -m              keep meta files (cetk, tmd); usable with make_cdn_cia\n"
	" -l              download files to local directory instead of tid/ver/\n"
	" -p              show progress bars\n"
	" -r              resume download\n"
	" -v              print nustool version and exit\n"
	" -V [version]    the version of the title to download; if not given,\n"
	"                 the latest version will be downloaded\n"
	"\n"
	"If none of -c, -D, -k and -K are given, the raw encrypted contents\n"
	"will be downloaded.\n"
	"\n"
	"All files are downloaded into a titleid/version/ directory.");
}

static void version(void)
{
	msg("nustool version %s", NUSTOOL_VERSION);
}

static bool is_valid_hex_char(int c)
{
	return (
			(c >= 'A' && c <= 'F') ||
			(c >= 'a' && c <= 'f') ||
			(c >= '0' && c <= '9')
	       );
}

static uint8_t get_hex_char_value(int c)
{
	if (c >= 'A' && c <= 'F') {
		return (uint8_t)((c - 'A') + 0xA);
	} else if (c >= 'a' && c <= 'f') {
		return (uint8_t)((c - 'a') + 0xA);
	} else {
		return (uint8_t)(c - '0');
	}
}

static void strip_spaces(char *in) {
	char *src = in, *dst = in;

	while(*src) {
		if(*src == ' ')
			src++;
		else
			*dst++ = *src++;
	}

	*dst = '\0';
}

errno_t util_parse_hex(char *in, byte *out, size_t outlen)
{
	size_t inlen;

	strip_spaces(in);

	if ((inlen = strlen(in)) & 1)
		return -1;
	if (outlen < inlen / 2)
		return -1;

	for (size_t i = 0; i < inlen; i += 2) {
		if (!is_valid_hex_char(in[i]) || !is_valid_hex_char(in[i + 1]))
			return -1;

		out[i / 2] = (byte)((get_hex_char_value(in[i]) << 4) |
				get_hex_char_value(in[i + 1]));
	}

	return 0;
}

errno_t util_create_outdir(void)
{
	if (opts.flags & OPT_LOCAL_FILES)
		return 0;

	char *dir = calloc(MAX_FILEPATH_LEN, sizeof(char));

	snprintf(dir, MAX_FILEPATH_LEN, "%016" PRIx64, opts.titleid);
	if (MAKEDIR(dir, 0777) == -1 && errno != EEXIST) {
		free(dir);
		return -1;
	}

	snprintf(dir + strlen(dir), MAX_FILEPATH_LEN - strlen(dir), "/%" PRIu16, opts.version);
	if (MAKEDIR(dir, 0777) == -1 && errno != EEXIST) {
		free(dir);
		return -1;
	}

	free(dir);
	return 0;
}

char *util_get_filepath(const char *path)
{
	char *filepath = calloc(MAX_FILEPATH_LEN, sizeof(char));

	if (!(opts.flags & OPT_LOCAL_FILES))
		snprintf(filepath, MAX_FILEPATH_LEN - MAX_FILENAME_LEN, "%016" PRIx64 "/%" PRIu16 "/", opts.titleid, opts.version);
	snprintf(filepath + strlen(filepath), MAX_FILENAME_LEN, "%s", path);

	return filepath;
}

const char *util_print_hex(const byte bytes[], size_t length, char *out)
{
	static const char *hex_str = "0123456789abcdef";

	for (size_t i = 0; i < length; ++i) {
		out[2*i] = hex_str[bytes[i] >> 4];
		out[2*i + 1] = hex_str[bytes[i] & 0xF];
	}

	out[2 * length] = 0;

	return out;
}

static errno_t util_parse_num(const char *str, char flag,
		uint64_t *num, int base,
		uint64_t min, uint64_t max)
{
	char *end;

	errno = 0;
	*num = strtoull(str, &end, base);

	if (*str == '\0') {
		if (flag == 0)
			err("No argument given.");
		else
			err("No argument given for flag %c.", flag);

		return -1;
	}

	if (*end != '\0') {
		err("%s is not a valid number.", str);
		return -1;
	}

	if (errno == ERANGE && *num == ULONG_MAX) {
		err("%s is out of this machine's range (max: %llx).",
				str, ULLONG_MAX);
		return -1;
	}

	if (*num < min || *num > max) {
		err("%s is out of range (range: 0x%llx-0x%llx).", str, min, max);
		return -1;
	}

	return 0;
}

errno_t util_parse_options(int argc, char *argv[])
{
	const char *progname = argv[0];
	int flag;
	uint64_t num;

	/* Set default options */
	memset(&opts, 0, sizeof(opts));

	/* Invalid titleid for verification if it's been set */
	opts.titleid = 0xFFFFFFFFFFFFFFFFULL;

	while ((flag = getopt(argc, argv, "cDhk:K:lmprvV:")) != -1) {
		switch (flag) {
		case 'D':
			opts.flags |= OPT_DEV_KEYS;
			break;
		case 'c':
			if (opts.flags & OPT_HAS_KEY) {
				err("You cannot specify -k/-K and -c/-D together.");
				return -1;
			}

			opts.flags |= (OPT_DECRYPT_FROM_CETK | OPT_KEY_ENCRYPTED);
			break;

		case 'K':
			opts.flags |= OPT_KEY_ENCRYPTED;
			/* fallthrough */
		case 'k':
			if (opts.flags & OPT_HAS_KEY) {
				err("You may only specify one key.");
				return -1;
			}

			if (opts.flags & OPT_DECRYPT_FROM_CETK) {
				err("You cannot specify -k/-K and -c together.");
				return -1;
			}

			if (util_parse_hex(optarg, opts.key, sizeof(opts.key))
					!= 0) {
				err("Error: Unable to parse key %s.", optarg);
				return -1;
			}

			opts.flags |= OPT_HAS_KEY;

			break;

		case 'l':
			opts.flags |= OPT_LOCAL_FILES;
			break;

		case 'm':
			opts.flags |= OPT_KEEP_META;
			break;

		case 'p':
			opts.flags |= OPT_SHOW_PROGRESS;
			break;

		case 'r':
			opts.flags |= OPT_RESUME;
			break;

		case 'V':
			if (util_parse_num(optarg, (char)flag, &num, 0, 0,
						0xFFFFU) != 0)
				return -1;

			opts.flags |= OPT_HAS_VERSION;
			opts.version = (uint16_t)num;

			break;

		case 'v':
			version();
			return 1;

		case 'h':
			help(argv[0]);
			return 1;

		case '?':
			err("Error: Unknown flag or missing flag argument.");
			help(argv[0]);
			return -1;
		}
	}

	argc -= optind;
	argv += optind;

	if (argv[0] == NULL) {
		err("Error: No title ID given.");
		help(progname);
		return -1;
	}

	if (util_parse_num(argv[0], 0, &num, 16,
				/* Minimum TID on the Wii */
				0x0000000100000001ULL,
				/* Maximum theoretical TID */
				0xFFFFFFFFFFFFFFFFULL) != 0)
		return -1;

	opts.titleid = num;

	return 0;
}

errno_t util_create_file(const char *path)
{
	int fd;

	if ((fd = open(path, O_WRONLY | O_CREAT, 0644)) == -1)
		return -1;

	if (close(fd) != 0)
		return -1;

	return 0;
}

errno_t util_get_file_size(const char *path, uint64_t *size)
{
	struct stat buf;

	errno = 0;
	if (stat(path, &buf) != 0) {
		err("Unable to get file size: %s", strerror(errno));
		return -1;
	}

	*size = (uint64_t)buf.st_size;

	return 0;
}

uint8_t util_get_msb64(uint64_t i)
{
	uint8_t msb = 0;

	while (i >>= 1)
		++msb;

	return msb;
}

__attribute__((format(printf, 3, 4))) char *util_realloc_and_append_fmt(
		char *base, size_t appendlen, const char *fmt, ...)
{
	size_t newlen, baselen;
	char *newbuf;
	va_list ap;

	baselen = strlen(base);
	newlen = baselen + appendlen;

	if ((newbuf = realloc(base, newlen + 1)) == NULL) {
		free(base);
		return NULL;
	}

	va_start(ap, fmt);
	vsprintf(newbuf + baselen, fmt, ap);
	va_end(ap);

	return newbuf;
}

