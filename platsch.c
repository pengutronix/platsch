/*
 * Copyright (C) 2019 Pengutronix, Uwe Kleine-König <u.kleine-koenig@pengutronix.de>
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Some code parts base on example code written in 2012 by David Herrmann
 * <dh.herrmann@googlemail.com> and dedicated to the Public Domain. It was found
 * in 2019 on
 * https://raw.githubusercontent.com/dvdhrm/docs/master/drm-howto/modeset.c
 */

#define _GNU_SOURCE

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#define debug(fmt, ...) printf("%s:%d: " fmt, __func__, __LINE__, ##__VA_ARGS__)
#define error(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*a))

struct platsch_format {
	uint32_t format;
	uint32_t bpp;
	const char *name;
};

static const struct platsch_format platsch_formats[] = {
	{ DRM_FORMAT_RGB565, 16, "RGB565" }, /* default */
	{ DRM_FORMAT_XRGB8888, 32, "XRGB8888" },
};

void redirect_stdfd(void)
{
	int devnull = open("/dev/null", O_RDWR, 0);

	if (devnull < 0) {
		error("Failed to open /dev/null: %m\n");
		return;
	}

	close(STDIN_FILENO);
	dup2(devnull, STDIN_FILENO);
	close(STDOUT_FILENO);
	dup2(devnull, STDOUT_FILENO);
	close(STDERR_FILENO);
	dup2(devnull, STDERR_FILENO);
	close(devnull);
}

ssize_t readfull(int fd, void *buf, size_t count)
{
	ssize_t ret = 0, err;

	while (count > 0) {
		err = read(fd, buf, count);
		if (err < 0)
			return err;
		else if (err > 0) {
			buf += err;
			count -= err;
			ret += err;
		} else {
			return ret;
		}
	}

	return ret;
}

struct modeset_dev {
	struct modeset_dev *next;

	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t size;
	const struct platsch_format *format;
	uint32_t handle;
	void *map;

	bool setmode;
	drmModeModeInfo mode;
	uint32_t fb_id;
	uint32_t conn_id;
	uint32_t crtc_id;
};

void draw_buffer(struct modeset_dev *dev, const char *dir, const char *base)
{
	int fd_src;
	char *filename;
	ssize_t size;
	int ret;

	/*
	 * make it easy and load a raw file in the right format instead of
	 * opening an (say) PNG and convert the image data to the right format.
	 */
	ret = asprintf(&filename, "%s/%s-%ux%u-%s.bin",
		       dir, base, dev->width, dev->height, dev->format->name);
	if (ret < 0) {
		error("Failed to allocate filename buffer\n");
		return;
	}

	fd_src = open(filename, O_RDONLY | O_CLOEXEC);
	if (fd_src < 0) {
		error("Failed to open %s: %m\n", filename);
		goto out;
	}

	size = readfull(fd_src, dev->map, dev->size);
	if (size < dev->size) {
		if (size < 0)
			error("Failed to read from %s: %m\n", filename);
		else
			error("Could only read %zd/%u bytes from %s\n",
			      size, dev->size, filename);
	}

	ret = close(fd_src);
	if (ret < 0) {
		/* Nothing we can do about this, so just warn */
		error("Failed to close image file\n");
	}

out:
	free(filename);

	return;
}

static struct modeset_dev *modeset_list = NULL;

static int drmprepare_crtc(int fd, drmModeRes *res, drmModeConnector *conn,
			   struct modeset_dev *dev)
{
	drmModeEncoder *enc;
	int i, j;
	uint32_t crtc_id;
	struct modeset_dev *iter;

	/* first try the currently connected encoder+crtc */
	if (conn->encoder_id) {
		debug("connector #%d uses encoder #%d\n", conn->connector_id,
		      conn->encoder_id);
		enc = drmModeGetEncoder(fd, conn->encoder_id);
		assert(enc);
		assert(enc->encoder_id == conn->encoder_id);
	} else {
		debug("connector #%d has no active encoder\n",
		      conn->connector_id);
		enc = NULL;
		dev->setmode = 1;
	}

	if (enc) {
		if (enc->crtc_id) {
			crtc_id = enc->crtc_id;
			bool in_use = false;

			for (iter = modeset_list; iter; iter = iter->next) {
				if (iter->crtc_id == crtc_id) {
					in_use = true;
					break;
				}
			}

			if (!in_use) {
				debug("encoder #%d uses crtc #%d\n",
				      enc->encoder_id, enc->crtc_id);
				drmModeFreeEncoder(enc);
				dev->crtc_id = crtc_id;
				return 0;
			} else {
				debug("encoder #%d used crtc #%d, but that's in use\n",
				      enc->encoder_id, iter->crtc_id);
			}
		} else {
			debug("encoder #%d doesn't have an active crtc\n",
			      enc->encoder_id);
		}

		drmModeFreeEncoder(enc);
	}

	/* If the connector is not currently bound to an encoder or if the
	 * encoder+crtc is already used by another connector (actually unlikely
	 * but let's be safe), iterate all other available encoders to find a
	 * matching CRTC. */
	for (i = 0; i < conn->count_encoders; ++i) {
		enc = drmModeGetEncoder(fd, conn->encoders[i]);
		if (!enc) {
			error("Cannot retrieve encoder %u: %m\n",
			      conn->encoders[i]);
			continue;
		}
		assert(enc->encoder_id == conn->encoders[i]);

		/* iterate all global CRTCs */
		for (j = 0; j < res->count_crtcs; ++j) {
			bool in_use = false;

			/* check whether this CRTC works with the encoder */
			if (!(enc->possible_crtcs & (1 << j)))
				continue;

			/* check that no other device already uses this CRTC */
			crtc_id = res->crtcs[j];
			for (iter = modeset_list; iter; iter = iter->next) {
				if (iter->crtc_id == crtc_id) {
					in_use = true;
					break;
				}
			}

			/* we have found a CRTC, so save it and return */
			if (!in_use) {
				debug("encoder #%d will use crtc #%d\n",
				      enc->encoder_id, crtc_id);
				drmModeFreeEncoder(enc);
				dev->crtc_id = crtc_id;
				return 0;
			}

		}
		drmModeFreeEncoder(enc);
	}

	error("Cannot find suitable CRTC for connector #%u\n",
	      conn->connector_id);
	return -ENOENT;
}

static int modeset_create_fb(int fd, struct modeset_dev *dev)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_destroy_dumb dreq;
	struct drm_mode_map_dumb mreq;
	int ret;

	/* create dumb buffer */
	memset(&creq, 0, sizeof(creq));
	creq.width = dev->width;
	creq.height = dev->height;
	creq.bpp = dev->format->bpp;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
	if (ret < 0) {
		error("Cannot create dumb buffer: %m\n");
		return -errno;
	}
	dev->stride = creq.pitch;
	dev->size = creq.size;
	dev->handle = creq.handle;

	/* create framebuffer object for the dumb-buffer */
	ret = drmModeAddFB2(fd, dev->width, dev->height,
			    dev->format->format,
			    (uint32_t[4]){ dev->handle, },
			    (uint32_t[4]){ dev->stride, },
			    (uint32_t[4]){ 0, },
			    &dev->fb_id, 0);
	if (ret) {
		ret = -errno;
		error("Cannot create framebuffer: %m\n");
		goto err_destroy;
	}

	/* prepare buffer for memory mapping */
	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = dev->handle;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
	if (ret) {
		ret = -errno;
		error("Cannot get mmap offset: %m\n");
		goto err_fb;
	}

	/* perform actual memory mapping */
	dev->map = mmap(0, dev->size, PROT_READ | PROT_WRITE, MAP_SHARED,
			fd, mreq.offset);
	if (dev->map == MAP_FAILED) {
		ret = -errno;
		error("Cannot mmap dumb buffer: %m\n");
		goto err_fb;
	}

	/*
	 * Clear the framebuffer. Normally it's overwritten later with some
	 * image data, but in case this fails, initialize to all-black.
	 */
	memset(dev->map, 0x0, dev->size);

	return 0;

err_fb:
	drmModeRmFB(fd, dev->fb_id);
err_destroy:
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = dev->handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	return ret;
}

/* Returns lowercase connector type names with '_' for '-' */
static char *get_normalized_conn_type_name(uint32_t connector_type)
{
	int i;
	const char *connector_name;
	char *normalized_name;

	connector_name = drmModeGetConnectorTypeName(connector_type);
	if (!connector_name)
		return NULL;

	normalized_name = strdup(connector_name);

	for (i = 0; normalized_name[i]; i++) {
		normalized_name[i] = tolower(normalized_name[i]);
		if (normalized_name[i] == '-')
			normalized_name[i] = '_';
	}

	return normalized_name;
}

static const struct platsch_format *platsch_format_find(const char *name)
{
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(platsch_formats); i++)
		if (!strcmp(platsch_formats[i].name, name))
			return &platsch_formats[i];

	return NULL;
}

static int set_env_connector_mode(drmModeConnector *conn,
				  struct modeset_dev *dev)
{
	int ret, i = 0;
	u_int32_t width = 0, height = 0;
	const char *mode;
	char *connector_type_name, *mode_env_name = NULL, fmt_specifier[32] = "";
	const struct platsch_format *format = NULL;

	connector_type_name = get_normalized_conn_type_name(conn->connector_type);
	if (!connector_type_name) {
		error("could not look up name for connector type %u\n",
		      conn->connector_type);
		goto fallback;
	}

	ret = asprintf(&mode_env_name, "platsch_%s%u_mode",
		       connector_type_name, conn->connector_type_id);
	free(connector_type_name);
	if (ret < 0) {
		error("failed to allocate platsch env mode variable\n");
		return -ENOMEM;
	}

	/* check for connector mode configuration in environment */
	debug("looking up %s env variable\n", mode_env_name);
	mode = getenv(mode_env_name);
	if (!mode)
		goto fallback;

	/* format suffix is optional */
	ret = sscanf(mode, "%ux%u@%s", &width, &height, fmt_specifier);
	if (ret < 2) {
		error("error while scanning %s for mode\n", mode_env_name);
		ret = -EFAULT;
		goto err_out;
	}

	/* use first mode matching given resolution */
	for (i = 0; i < conn->count_modes; i++) {
		drmModeModeInfo mode = conn->modes[i];
		if (mode.hdisplay == width && mode.vdisplay == height) {
			memcpy(&dev->mode, &mode, sizeof(dev->mode));
			dev->width = width;
			dev->height = height;
			break;
		}
	}

	if (i == conn->count_modes) {
		error("no mode available matching %ux%u\n", width, height);
		ret = -ENOENT;
		goto err_out;
	}

	format = platsch_format_find(fmt_specifier);
	if (!format) {
		if (strlen(fmt_specifier))
			error("unknown format specifier %s\n", fmt_specifier);
		goto fallback_format;
	}

	dev->format = format;

	free(mode_env_name);

	return 0;

fallback:
	memcpy(&dev->mode, &conn->modes[0], sizeof(dev->mode));
	dev->width = conn->modes[0].hdisplay;
	dev->height = conn->modes[0].vdisplay;
	debug("using default mode for connector #%u\n", conn->connector_id);

fallback_format:
	dev->format = &platsch_formats[0];
	debug("using default format %s for connector #%u\n", dev->format->name,
	      conn->connector_id);

	ret = 0;

err_out:
	free(mode_env_name);

	return ret;
}

static int drmprepare_connector(int fd, drmModeRes *res, drmModeConnector *conn,
				struct modeset_dev *dev)
{
	int ret;

	/* check if a monitor is connected */
	if (conn->connection != DRM_MODE_CONNECTED) {
		error("Ignoring unused connector #%u\n", conn->connector_id);
		return -ENOENT;
	}

	/* check if there is at least one valid mode */
	if (conn->count_modes == 0) {
		error("no valid mode for connector #%u\n", conn->connector_id);
		return -EFAULT;
	}

	/* configure mode information in our device structure */
	ret = set_env_connector_mode(conn, dev);
	if (ret) {
		error("no valid mode for connector #%u\n", conn->connector_id);
		return ret;
	}
	debug("mode for connector #%u is %ux%u@%s\n",
	      conn->connector_id, dev->width, dev->height, dev->format->name);

	/* find a crtc for this connector */
	ret = drmprepare_crtc(fd, res, conn, dev);
	if (ret) {
		error("no valid crtc for connector #%u\n", conn->connector_id);
		return ret;
	}

	/* create a framebuffer for this CRTC */
	ret = modeset_create_fb(fd, dev);
	if (ret) {
		error("cannot create framebuffer for connector #%u\n",
		      conn->connector_id);
		return ret;
	}

	return 0;
}

static int drmprepare(int fd)
{
	drmModeRes *res;
	drmModeConnector *conn;
	int i;
	struct modeset_dev *dev;
	int ret;

	/* retrieve resources */
	res = drmModeGetResources(fd);
	if (!res) {
		error("cannot retrieve DRM resources: %m\n");
		return -errno;
	}

	debug("Found %d connectors\n", res->count_connectors);

	/* iterate all connectors */
	for (i = 0; i < res->count_connectors; ++i) {
		/* get information for each connector */
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (!conn) {
			error("Cannot retrieve DRM connector #%u: %m\n",
				res->connectors[i]);
			continue;
		}
		assert(conn->connector_id == res->connectors[i]);

		debug("Connector #%u has type %s\n", conn->connector_id,
		      drmModeGetConnectorTypeName(conn->connector_type));

		/* create a device structure */
		dev = malloc(sizeof(*dev));
		if (!dev) {
			error("Cannot allocate memory for connector #%u: %m\n",
			      res->connectors[i]);
			continue;
		}
		memset(dev, 0, sizeof(*dev));
		dev->conn_id = conn->connector_id;

		ret = drmprepare_connector(fd, res, conn, dev);
		if (ret) {
			if (ret != -ENOENT) {
				error("Cannot setup device for connector #%u: %m\n",
				      res->connectors[i]);
			}
			free(dev);
			drmModeFreeConnector(conn);
			continue;
		}

		/* free connector data and link device into global list */
		drmModeFreeConnector(conn);
		dev->next = modeset_list;
		modeset_list = dev;
	}

	/* free resources again */
	drmModeFreeResources(res);
	return 0;
}

static struct option longopts[] =
{
	{ "help",      no_argument,       0, 'h' },
	{ "directory", required_argument, 0, 'd' },
	{ "basename",  required_argument, 0, 'b' },
	{ NULL,        0,                 0, 0   }
};

static void usage(const char *prog)
{
	error("Usage:\n"
	      "%s [-d|--directory <dir>] [-b|--basename <name>]\n"
	      "   [-h|--help]\n",
	      prog);
}

int main(int argc, char *argv[])
{
	char **initsargv;
	int drmfd;
	struct modeset_dev *iter;
	bool pid1 = getpid() == 1;
	const char *dir = "/usr/share/platsch";
	const char *base = "splash";
	const char *env;
	int ret = 0, c, i;

	env = getenv("platsch_directory");
	if (env)
		dir = env;

	env = getenv("platsch_basename");
	if (env)
		base = env;

	if (!pid1) {
		while ((c = getopt_long(argc, argv, "hd:b:", longopts, NULL)) != EOF) {
			switch(c) {
			case 'd':
				dir = optarg;
				break;
			case 'b':
				base = optarg;
				break;
			case '?':
				/* ‘getopt_long’ already printed an error message. */
				ret = 1;
				/* FALLTHRU */
			case 'h':
				usage(basename(argv[0]));
				exit(ret);
			}
		}

		if (optind < argc) {
			error("Too many arguments!\n");
			usage(basename(argv[0]));
			exit(1);
		}
	}

	for (i = 0; i < 64; i++) {
		struct drm_mode_card_res res = {0};
		char *drmdev;

		/*
		 * XXX: Maybe use drmOpen instead?
		 * (Where should name/busid come from?)
		 * XXX: Loop through drm devices to find one with connectors.
		 */
		ret = asprintf(&drmdev, DRM_DEV_NAME, DRM_DIR_NAME, i);
		if (ret < 0) {
			error("Huh, failed to allocate device name buffer\n");
			goto execinit;
		}

		drmfd = open(drmdev, O_RDWR | O_CLOEXEC, 0);
		free(drmdev);
		if (drmfd < 0) {
			error("Failed to open drm device: %m\n");
			goto execinit;
		}

		ret = drmIoctl(drmfd, DRM_IOCTL_MODE_GETRESOURCES, &res);
		if (ret < 0) {
			close(drmfd);
			continue;
		} else {
			/* Device found */
			break;
		}
	}

	ret = drmprepare(drmfd);
	assert(!ret);

	for (iter = modeset_list; iter; iter = iter->next) {

		/* draw first then set the mode */
		draw_buffer(iter, dir, base);

		if (iter->setmode) {
			debug("set crtc\n");

			ret = drmModeSetCrtc(drmfd, iter->crtc_id, iter->fb_id,
					     0, 0, &iter->conn_id, 1, &iter->mode);
			if (ret)
				error("Cannot set CRTC for connector #%u: %m\n",
				      iter->conn_id);
		} else {
			debug("page flip\n");
			ret = drmModePageFlip(drmfd, iter->crtc_id, iter->fb_id,
					      0, NULL);
			if (ret)
				error("Page flip failed on connector #%u: %m\n",
				      iter->conn_id);
		}
	}

	ret = drmDropMaster(drmfd);
	if (ret)
		error("Failed to drop master on drm device\n");

execinit:
	if (pid1) {
		ret = fork();
		if (ret < 0) {
			error("failed to fork for init: %m\n");
		} else if (ret == 0) {
			/*
			 * in the child go to sleep to keep the drm device open
			 * and give pid 1 to init.
			 */
			goto sleep;
		}

		initsargv = calloc(argc + 1, sizeof(argv[0]));
		if (!initsargv) {
			error("failed to allocate argv for init\n");
			return EXIT_FAILURE;
		}
		memcpy(initsargv, argv, argc * sizeof(argv[0]));
		initsargv[0] = "/sbin/init";
		initsargv[argc] = NULL;

		execv("/sbin/init", initsargv);

		error("failed to exec init: %m\n");

		return EXIT_FAILURE;
	}

sleep:
	redirect_stdfd();

	do {
		sleep(10);
	} while (1);
}
