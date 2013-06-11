/*
 * XWiimote - lib
 * Written 2010-2013 by David Herrmann <dh.herrmann@gmail.com>
 * Dedicated to the Public Domain
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libudev.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "xwiimote.h"

/* interfaces */
enum xwii_if_base_idx {
	/* base interfaces */
	XWII_IF_CORE,
	XWII_IF_ACCEL,
	XWII_IF_IR,

	/* extension interfaces */
	XWII_IF_MOTION_PLUS,
	XWII_IF_NUNCHUK,
	XWII_IF_CLASSIC_CONTROLLER,
	XWII_IF_BALANCE_BOARD,
	XWII_IF_PRO_CONTROLLER,

	XWII_IF_NUM,
};

/* event interface */
struct xwii_if {
	/* device node as /dev/input/eventX or NULL */
	char *node;
	/* open file or -1 */
	int fd;
};

/* main device interface */
struct xwii_iface {
	/* reference count */
	size_t ref;
	/* epoll file descriptor */
	int efd;
	/* udev context */
	struct udev *udev;
	/* main udev device */
	struct udev_device *dev;

	/* bitmask of open interfaces */
	unsigned int ifaces;
	/* interfaces */
	struct xwii_if ifs[XWII_IF_NUM];
	/* device type attribute */
	char *devtype_attr;
	/* extension attribute */
	char *extension_attr;
	/* battery capacity attribute */
	char *battery_attr;
	/* led brightness attributes */
	char *led_attrs[4];

	/* rumble-id for base-core interface force-feedback or -1 */
	int rumble_id;
	/* accelerometer data cache */
	struct xwii_event_abs accel_cache;
	/* IR data cache */
	struct xwii_event_abs ir_cache[4];
	/* balance board weight cache */
	struct xwii_event_abs bboard_cache[4];
	/* motion plus cache */
	struct xwii_event_abs mp_cache;
	/* motion plus normalization stuff */
	bool mp_normalize;
	struct xwii_event_abs mp_normalizer;
	int32_t mp_calibration_factor;
	/* pro controller cache */
	struct xwii_event_abs pro_cache[2];
};

/* table to convert interface to name */
static const char *if_to_name_table[] = {
	[XWII_IF_CORE] = XWII_NAME_CORE,
	[XWII_IF_ACCEL] = XWII_NAME_ACCEL,
	[XWII_IF_IR] = XWII_NAME_IR,
	[XWII_IF_MOTION_PLUS] = XWII_NAME_MOTION_PLUS,
	[XWII_IF_NUNCHUK] = XWII_NAME_NUNCHUK,
	[XWII_IF_CLASSIC_CONTROLLER] = XWII_NAME_CLASSIC_CONTROLLER,
	[XWII_IF_BALANCE_BOARD] = XWII_NAME_BALANCE_BOARD,
	[XWII_IF_PRO_CONTROLLER] = XWII_NAME_PRO_CONTROLLER,
	[XWII_IF_NUM] = NULL,
};

/* convert name to interface or -1 */
static int name_to_if(const char *name)
{
	unsigned int i;

	for (i = 0; i < XWII_IF_NUM; ++i)
		if (!strcmp(name, if_to_name_table[i]))
			return i;

	return -1;
}

/*
 * Scan the device \dev for child input devices and update our device-node
 * cache with the new information. This is called during device setup to
 * find all /dev/input/eventX nodes for all currently available interfaces.
 * We also cache attribute paths for sub-devices like LEDs or batteries.
 */
static int xwii_iface_read_nodes(struct xwii_iface *dev)
{
	struct udev_enumerate *e;
	struct udev_list_entry *list;
	struct udev_device *d;
	const char *name, *node, *subs;
	char *n;
	int ret, prev_if, tif, len, i;

	e = udev_enumerate_new(dev->udev);
	if (!e)
		return -ENOMEM;

	ret = udev_enumerate_add_match_subsystem(e, "input");
	ret += udev_enumerate_add_match_subsystem(e, "leds");
	ret += udev_enumerate_add_match_subsystem(e, "power_supply");
	ret += udev_enumerate_add_match_parent(e, dev->dev);
	if (ret) {
		udev_enumerate_unref(e);
		return -ENOMEM;
	}

	ret = udev_enumerate_scan_devices(e);
	if (ret) {
		udev_enumerate_unref(e);
		return ret;
	}

	/* The returned list is sorted. So we first get an inputXY entry,
	 * possibly followed by the inputXY/eventXY entry. We remember the type
	 * of a found inputXY entry, and check the next list-entry, whether
	 * it's an eventXY entry. If it is, we save the node, otherwise, it's
	 * skipped.
	 * For other subsystems we simply cache the attribute paths. */
	prev_if = -1;
	for (list = udev_enumerate_get_list_entry(e);
	     list;
	     list = udev_list_entry_get_next(list), udev_device_unref(d)) {

		tif = prev_if;
		prev_if = -1;

		name = udev_list_entry_get_name(list);
		d = udev_device_new_from_syspath(dev->udev, name);
		if (!d)
			continue;

		subs = udev_device_get_subsystem(d);
		if (!strcmp(subs, "input")) {
			name = udev_device_get_sysname(d);
			if (!strncmp(name, "input", 5)) {
				name = udev_device_get_sysattr_value(d, "name");
				if (!name)
					continue;

				tif = name_to_if(name);
				if (tif >= 0) {
					/* skip duplicates */
					if (dev->ifs[tif].node)
						continue;
					prev_if = tif;
				}
			} else if (!strncmp(name, "event", 5)) {
				if (tif < 0)
					continue;
				node = udev_device_get_devnode(d);
				if (!node)
					continue;
				n = strdup(node);
				if (!n)
					continue;
				dev->ifs[tif].node = n;
			}
		} else if (!strcmp(subs, "leds")) {
			len = strlen(name);
			if (name[len - 1] == '0')
				i = 0;
			else if (name[len - 1] == '1')
				i = 1;
			else if (name[len - 1] == '2')
				i = 2;
			else if (name[len - 1] == '3')
				i = 3;
			else
				continue;

			if (dev->led_attrs[i])
				continue;

			ret = asprintf(&dev->led_attrs[i], "%s/%s",
				       name, "brightness");
			if (ret <= 0)
				dev->led_attrs[i] = NULL;
		} else if (!strcmp(subs, "power_supply")) {
			if (dev->battery_attr)
				continue;
			ret = asprintf(&dev->battery_attr, "%s/%s",
				       name, "capacity");
			if (ret <= 0)
				dev->battery_attr = NULL;
		}
	}

	udev_enumerate_unref(e);

	return 0;
}

/*
 * Create new interface structure
 * This creates a new interface for a single Wii Remote device. \syspath must
 * point to the base-directory of the device. It can normally be found as:
 *   /sys/bus/hid/devices/<device>
 * The device is validated and 0 is returned on success. On failure, a negative
 * error code is returned.
 * A pointer to the new object is stored in \dev. \dev is left untouched on
 * failure.
 * Initial refcount is 1 so you need to call *_unref() to free the device.
 */
int xwii_iface_new(struct xwii_iface **dev, const char *syspath)
{
	struct xwii_iface *d;
	const char *driver, *subs;
	int ret, i;

	if (!dev || !syspath)
		return -EINVAL;

	d = malloc(sizeof(*d));
	if (!d)
		return -ENOMEM;

	memset(d, 0, sizeof(*d));
	d->ref = 1;
	d->rumble_id = -1;

	for (i = 0; i < XWII_IF_NUM; ++i)
		d->ifs[i].fd = -1;

	d->efd = epoll_create1(EPOLL_CLOEXEC);
	if (d->efd < 0) {
		ret = -EFAULT;
		goto err_free;
	}

	d->udev = udev_new();
	if (!d->udev) {
		ret = -ENOMEM;
		goto err_efd;
	}

	d->dev = udev_device_new_from_syspath(d->udev, syspath);
	if (!d->dev) {
		ret = -ENODEV;
		goto err_udev;
	}

	driver = udev_device_get_driver(d->dev);
	subs = udev_device_get_subsystem(d->dev);
	if (!driver || strcmp(driver, "wiimote") ||
	    !subs || strcmp(subs, "hid")) {
		ret = -ENODEV;
		goto err_dev;
	}

	ret = asprintf(&d->devtype_attr, "%s/%s", syspath, "devtype");
	if (ret <= 0) {
		ret = -ENOMEM;
		goto err_dev;
	}

	ret = asprintf(&d->extension_attr, "%s/%s", syspath, "extension");
	if (ret <= 0) {
		ret = -ENOMEM;
		goto err_attrs;
	}

	ret = xwii_iface_read_nodes(d);
	if (ret)
		goto err_attrs;

	*dev = d;
	return 0;

err_attrs:
	free(d->extension_attr);
	free(d->devtype_attr);
err_dev:
	udev_device_unref(d->dev);
err_udev:
	udev_unref(d->udev);
err_efd:
	close(d->efd);
err_free:
	free(d);
	return ret;
}

void xwii_iface_ref(struct xwii_iface *dev)
{
	if (!dev || !dev->ref)
		return;

	dev->ref++;
}

void xwii_iface_unref(struct xwii_iface *dev)
{
	unsigned int i;

	if (!dev || !dev->ref || --dev->ref)
		return;

	xwii_iface_close(dev, XWII_IFACE_ALL);

	for (i = 0; i < XWII_IF_NUM; ++i)
		free(dev->ifs[i].node);
	for (i = 0; i < 4; ++i)
		free(dev->led_attrs[i]);
	free(dev->battery_attr);
	free(dev->extension_attr);
	free(dev->devtype_attr);

	udev_device_unref(dev->dev);
	udev_unref(dev->udev);
	close(dev->efd);
	free(dev);
}

int xwii_iface_get_fd(struct xwii_iface *dev)
{
	if (!dev)
		return -1;

	return dev->efd;
}

static int xwii_iface_open_if(struct xwii_iface *dev, unsigned int tif,
			      bool wr)
{
	char name[256];
	struct epoll_event ep;
	unsigned int flags;
	int fd;

	if (dev->ifs[tif].fd >= 0)
		return 0;
	if (!dev->ifs[tif].node)
		return -ENODEV;

	flags = O_NONBLOCK | O_CLOEXEC;
	flags |= wr ? O_RDWR : O_RDONLY;
	fd = open(dev->ifs[tif].node, flags);
	if (fd < 0)
		return -errno;

	if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
		close(fd);
		return -errno;
	}

	name[sizeof(name) - 1] = 0;
	if (strcmp(if_to_name_table[tif], name)) {
		close(fd);
		return -ENODEV;
	}

	memset(&ep, 0, sizeof(ep));
	ep.events = EPOLLIN;
	ep.data.ptr = dev;
	if (epoll_ctl(dev->efd, EPOLL_CTL_ADD, fd, &ep) < 0) {
		close(fd);
		return -errno;
	}

	dev->ifs[tif].fd = fd;
	return 0;
}

/*
 * Upload the generic rumble event to the device. This may later be used for
 * force-feedback effects. The event id is safed for later use.
 */
static void xwii_iface_upload_rumble(struct xwii_iface *dev)
{
	struct ff_effect effect = {
		.type = FF_RUMBLE,
		.id = -1,
		.u.rumble.strong_magnitude = 1,
		.replay.length = 0,
		.replay.delay = 0,
	};

	if (ioctl(dev->ifs[XWII_IF_CORE].fd, EVIOCSFF, &effect) != -1)
		dev->rumble_id = effect.id;
}

int xwii_iface_open(struct xwii_iface *dev, unsigned int ifaces)
{
	bool wr;
	int ret;

	if (!dev)
		return -EINVAL;

	wr = ifaces & XWII_IFACE_WRITABLE;
	ifaces &= XWII_IFACE_ALL;
	ifaces &= ~dev->ifaces;
	if (!ifaces)
		return 0;

	if (ifaces & XWII_IFACE_CORE) {
		ret = xwii_iface_open_if(dev, XWII_IF_CORE, wr);
		if (ret)
			goto err_out;
		dev->ifaces |= XWII_IFACE_CORE;
		xwii_iface_upload_rumble(dev);
	}

	if (ifaces & XWII_IFACE_ACCEL) {
		ret = xwii_iface_open_if(dev, XWII_IF_ACCEL, wr);
		if (ret)
			goto err_out;
		dev->ifaces |= XWII_IFACE_ACCEL;
	}

	if (ifaces & XWII_IFACE_IR) {
		ret = xwii_iface_open_if(dev, XWII_IF_IR, wr);
		if (ret)
			goto err_out;
		dev->ifaces |= XWII_IFACE_IR;
	}

	if (ifaces & XWII_IFACE_MOTION_PLUS) {
		ret = xwii_iface_open_if(dev, XWII_IF_MOTION_PLUS, wr);
		if (ret)
			goto err_out;
		dev->ifaces |= XWII_IFACE_MOTION_PLUS;
	}

	if (ifaces & XWII_IFACE_BALANCE_BOARD) {
		ret = xwii_iface_open_if(dev, XWII_IF_BALANCE_BOARD, wr);
		if (ret)
			goto err_out;
		dev->ifaces |= XWII_IFACE_BALANCE_BOARD;
	}

	if (ifaces & XWII_IFACE_PRO_CONTROLLER) {
		ret = xwii_iface_open_if(dev, XWII_IF_PRO_CONTROLLER, wr);
		if (ret)
			goto err_out;
		dev->ifaces |= XWII_IFACE_PRO_CONTROLLER;
	}

	return 0;

err_out:
	return ret;
}

static void xwii_iface_close_if(struct xwii_iface *dev, unsigned int tif)
{
	if (dev->ifs[tif].fd < 0)
		return;

	epoll_ctl(dev->efd, EPOLL_CTL_DEL, dev->ifs[tif].fd, NULL);
	close(dev->ifs[tif].fd);
	dev->ifs[tif].fd = -1;
}

void xwii_iface_close(struct xwii_iface *dev, unsigned int ifaces)
{
	if (!dev)
		return;

	ifaces &= XWII_IFACE_ALL;
	if (!ifaces)
		return;

	if (ifaces & XWII_IFACE_CORE) {
		xwii_iface_close_if(dev, XWII_IF_CORE);
		dev->rumble_id = -1;
	}
	if (ifaces & XWII_IFACE_ACCEL)
		xwii_iface_close_if(dev, XWII_IF_ACCEL);
	if (ifaces & XWII_IFACE_IR)
		xwii_iface_close_if(dev, XWII_IF_IR);
	if (ifaces & XWII_IFACE_MOTION_PLUS)
		xwii_iface_close_if(dev, XWII_IF_MOTION_PLUS);
	if (ifaces & XWII_IFACE_NUNCHUK)
		xwii_iface_close_if(dev, XWII_IF_NUNCHUK);
	if (ifaces & XWII_IFACE_CLASSIC_CONTROLLER)
		xwii_iface_close_if(dev, XWII_IF_CLASSIC_CONTROLLER);
	if (ifaces & XWII_IFACE_BALANCE_BOARD)
		xwii_iface_close_if(dev, XWII_IF_BALANCE_BOARD);
	if (ifaces & XWII_IFACE_PRO_CONTROLLER)
		xwii_iface_close_if(dev, XWII_IF_PRO_CONTROLLER);

	dev->ifaces &= ~ifaces;
}

unsigned int xwii_iface_opened(struct xwii_iface *dev)
{
	if (!dev)
		return 0;

	return dev->ifaces;
}

static int read_event(int fd, struct input_event *ev)
{
	int ret;

	ret = read(fd, ev, sizeof(*ev));
	if (ret < 0)
		return -errno;
	else if (ret == 0)
		return -EAGAIN;
	else if (ret != sizeof(*ev))
		return -EIO;
	else
		return 0;
}

static int read_core(struct xwii_iface *dev, struct xwii_event *ev)
{
	int ret, fd;
	struct input_event input;
	unsigned int key;

	fd = dev->ifs[XWII_IF_CORE].fd;
	if (fd < 0)
		return -EAGAIN;

try_again:
	ret = read_event(fd, &input);
	if (ret == -EAGAIN) {
		return -EAGAIN;
	} else if (ret < 0) {
		xwii_iface_close(dev, XWII_IFACE_CORE);
		return -ENODEV;
	}

	if (input.type != EV_KEY)
		goto try_again;

	if (input.value < 0 || input.value > 2)
		goto try_again;

	switch (input.code) {
		case KEY_LEFT:
			key = XWII_KEY_LEFT;
			break;
		case KEY_RIGHT:
			key = XWII_KEY_RIGHT;
			break;
		case KEY_UP:
			key = XWII_KEY_UP;
			break;
		case KEY_DOWN:
			key = XWII_KEY_DOWN;
			break;
		case KEY_NEXT:
			key = XWII_KEY_PLUS;
			break;
		case KEY_PREVIOUS:
			key = XWII_KEY_MINUS;
			break;
		case BTN_1:
			key = XWII_KEY_ONE;
			break;
		case BTN_2:
			key = XWII_KEY_TWO;
			break;
		case BTN_A:
			key = XWII_KEY_A;
			break;
		case BTN_B:
			key = XWII_KEY_B;
			break;
		case BTN_MODE:
			key = XWII_KEY_HOME;
			break;
		default:
			goto try_again;
	}

	memset(ev, 0, sizeof(*ev));
	memcpy(&ev->time, &input.time, sizeof(struct timeval));
	ev->type = XWII_EVENT_KEY;
	ev->v.key.code = key;
	ev->v.key.state = input.value;
	return 0;
}

static int read_accel(struct xwii_iface *dev, struct xwii_event *ev)
{
	int ret, fd;
	struct input_event input;

	fd = dev->ifs[XWII_IF_ACCEL].fd;
	if (fd < 0)
		return -EAGAIN;

try_again:
	ret = read_event(fd, &input);
	if (ret == -EAGAIN) {
		return -EAGAIN;
	} else if (ret < 0) {
		xwii_iface_close(dev, XWII_IFACE_ACCEL);
		return -ENODEV;
	}

	if (input.type == EV_SYN) {
		memset(ev, 0, sizeof(*ev));
		memcpy(&ev->time, &input.time, sizeof(struct timeval));
		memcpy(ev->v.abs, &dev->accel_cache, sizeof(dev->accel_cache));
		ev->type = XWII_EVENT_ACCEL;
		return 0;
	}

	if (input.type != EV_ABS)
		goto try_again;

	if (input.code == ABS_RX)
		dev->accel_cache.x = input.value;
	else if (input.code == ABS_RY)
		dev->accel_cache.y = input.value;
	else if (input.code == ABS_RZ)
		dev->accel_cache.z = input.value;

	goto try_again;
}

static int read_ir(struct xwii_iface *dev, struct xwii_event *ev)
{
	int ret, fd;
	struct input_event input;

	fd = dev->ifs[XWII_IF_IR].fd;
	if (fd < 0)
		return -EAGAIN;

try_again:
	ret = read_event(fd, &input);
	if (ret == -EAGAIN) {
		return -EAGAIN;
	} else if (ret < 0) {
		xwii_iface_close(dev, XWII_IFACE_IR);
		return -ENODEV;
	}

	if (input.type == EV_SYN) {
		memset(ev, 0, sizeof(*ev));
		memcpy(&ev->time, &input.time, sizeof(struct timeval));
		memcpy(&ev->v.abs, dev->ir_cache, sizeof(dev->ir_cache));
		ev->type = XWII_EVENT_IR;
		return 0;
	}

	if (input.type != EV_ABS)
		goto try_again;

	if (input.code == ABS_HAT0X)
		dev->ir_cache[0].x = input.value;
	else if (input.code == ABS_HAT0Y)
		dev->ir_cache[0].y = input.value;
	else if (input.code == ABS_HAT1X)
		dev->ir_cache[1].x = input.value;
	else if (input.code == ABS_HAT1Y)
		dev->ir_cache[1].y = input.value;
	else if (input.code == ABS_HAT2X)
		dev->ir_cache[2].x = input.value;
	else if (input.code == ABS_HAT2Y)
		dev->ir_cache[2].y = input.value;
	else if (input.code == ABS_HAT3X)
		dev->ir_cache[3].x = input.value;
	else if (input.code == ABS_HAT3Y)
		dev->ir_cache[3].y = input.value;

	goto try_again;
}

static int read_mp(struct xwii_iface *dev, struct xwii_event *ev)
{
	int ret, fd;
	struct input_event input;

	fd = dev->ifs[XWII_IF_MOTION_PLUS].fd;
	if (fd < 0)
		return -EAGAIN;

try_again:
	ret = read_event(fd, &input);
	if (ret == -EAGAIN) {
		return -EAGAIN;
	} else if (ret < 0) {
		xwii_iface_close(dev, XWII_IFACE_MOTION_PLUS);
		return -ENODEV;
	}

	if (input.type == EV_SYN) {
		memset(ev, 0, sizeof(*ev));
		memcpy(&ev->time, &input.time, sizeof(struct timeval));
		if (dev->mp_normalize) {
			ev->v.abs[0].x = dev->mp_cache.x - (dev->mp_normalizer.x / 100);
			ev->v.abs[0].y = dev->mp_cache.y - (dev->mp_normalizer.y / 100);
			ev->v.abs[0].z = dev->mp_cache.z - (dev->mp_normalizer.z / 100);
			dev->mp_normalizer.x += dev->mp_calibration_factor * ((ev->v.abs[0].x > 0) ? 1 : -1);
			dev->mp_normalizer.y += dev->mp_calibration_factor * ((ev->v.abs[0].y > 0) ? 1 : -1);
			dev->mp_normalizer.z += dev->mp_calibration_factor * ((ev->v.abs[0].z > 0) ? 1 : -1);
		} else
			memcpy(&ev->v.abs, &dev->mp_cache, sizeof(dev->mp_cache));
		ev->type = XWII_EVENT_MOTION_PLUS;
		return 0;
	}

	if (input.type != EV_ABS)
		goto try_again;

	if (input.code == ABS_RX)
		dev->mp_cache.x = input.value;
	else if (input.code == ABS_RY)
		dev->mp_cache.y = input.value;
	else if (input.code == ABS_RZ)
		dev->mp_cache.z = input.value;

	goto try_again;
}

static int read_bboard(struct xwii_iface *dev, struct xwii_event *ev)
{
	int ret, fd;
	struct input_event input;

	fd = dev->ifs[XWII_IF_BALANCE_BOARD].fd;
	if (fd < 0)
		return -EAGAIN;

try_again:
	ret = read_event(fd, &input);
	if (ret == -EAGAIN) {
		return -EAGAIN;
	} else if (ret < 0) {
		xwii_iface_close(dev, XWII_IFACE_BALANCE_BOARD);
		return -ENODEV;
	}

	if (input.type == EV_SYN) {
		memset(ev, 0, sizeof(*ev));
		memcpy(&ev->time, &input.time, sizeof(struct timeval));
		memcpy(&ev->v.abs, dev->bboard_cache,
		       sizeof(dev->bboard_cache));
		ev->type = XWII_EVENT_BALANCE_BOARD;
		return 0;
	}

	if (input.type != EV_ABS)
		goto try_again;

	if (input.code == ABS_HAT0X)
		dev->bboard_cache[0].x = input.value;
	else if (input.code == ABS_HAT0Y)
		dev->bboard_cache[1].x = input.value;
	else if (input.code == ABS_HAT1X)
		dev->bboard_cache[2].x = input.value;
	else if (input.code == ABS_HAT1Y)
		dev->bboard_cache[3].x = input.value;

	goto try_again;
}

static int read_pro(struct xwii_iface *dev, struct xwii_event *ev)
{
	int ret, fd;
	struct input_event input;
	unsigned int key;

	fd = dev->ifs[XWII_IF_PRO_CONTROLLER].fd;
	if (fd < 0)
		return -EAGAIN;

try_again:
	ret = read_event(fd, &input);
	if (ret == -EAGAIN) {
		return -EAGAIN;
	} else if (ret < 0) {
		xwii_iface_close(dev, XWII_IFACE_PRO_CONTROLLER);
		return -ENODEV;
	}

	if (input.type == EV_KEY) {
		if (input.value < 0 || input.value > 2)
			goto try_again;

		switch (input.code) {
			case BTN_A:
				key = XWII_KEY_A;
				break;
			case BTN_B:
				key = XWII_KEY_B;
				break;
			case BTN_X:
				key = XWII_KEY_X;
				break;
			case BTN_Y:
				key = XWII_KEY_Y;
				break;
			case BTN_START:
				key = XWII_KEY_PLUS;
				break;
			case BTN_SELECT:
				key = XWII_KEY_MINUS;
				break;
			case BTN_MODE:
				key = XWII_KEY_HOME;
				break;
			case KEY_LEFT:
				key = XWII_KEY_LEFT;
				break;
			case KEY_RIGHT:
				key = XWII_KEY_RIGHT;
				break;
			case KEY_UP:
				key = XWII_KEY_UP;
				break;
			case KEY_DOWN:
				key = XWII_KEY_DOWN;
				break;
			case BTN_TL:
				key = XWII_KEY_TL;
				break;
			case BTN_TR:
				key = XWII_KEY_TR;
				break;
			case BTN_TL2:
				key = XWII_KEY_ZL;
				break;
			case BTN_TR2:
				key = XWII_KEY_ZR;
				break;
			case BTN_THUMBL:
				key = XWII_KEY_THUMBL;
				break;
			case BTN_THUMBR:
				key = XWII_KEY_THUMBR;
				break;
			default:
				goto try_again;
		}

		memset(ev, 0, sizeof(*ev));
		memcpy(&ev->time, &input.time, sizeof(struct timeval));
		ev->type = XWII_EVENT_PRO_CONTROLLER_KEY;
		ev->v.key.code = key;
		ev->v.key.state = input.value;
		return 0;
	} else if (input.type == EV_ABS) {
		if (input.code == ABS_HAT0X)
			dev->pro_cache[0].x = input.value;
		else if (input.code == ABS_HAT0Y)
			dev->pro_cache[0].y = input.value;
		else if (input.code == ABS_HAT1X)
			dev->pro_cache[1].x = input.value;
		else if (input.code == ABS_HAT1Y)
			dev->pro_cache[1].y = input.value;
	} else if (input.type == EV_SYN) {
		memset(ev, 0, sizeof(*ev));
		memcpy(&ev->time, &input.time, sizeof(struct timeval));
		memcpy(&ev->v.abs, dev->pro_cache,
		       sizeof(dev->pro_cache));
		ev->type = XWII_EVENT_PRO_CONTROLLER_MOVE;
		return 0;
	} else {
	}

	goto try_again;
}

/*
 * Read new event from any opened interface of \dev.
 * Returns -EAGAIN if no new event can be read.
 * Returns 0 on success and writes the new event into \ev.
 * Returns negative error on failure.
 * Returns -ENODEV *once* if *any* interface failed and got closed. Further
 * reads may succeed on other interfaces but this seems unlikely as all event
 * devices are created and destroyed by the kernel at the same time. Therefore,
 * it is recommended to assume the device was disconnected if this returns
 * -ENODEV.
 * Returns -EAGAIN on further reads if no interface is open anymore.
 */
static int read_iface(struct xwii_iface *dev, struct xwii_event *ev)
{
	int ret;

	if (!dev || !ev)
		return -EFAULT;

	ret = read_core(dev, ev);
	if (ret != -EAGAIN)
		return ret;
	ret = read_accel(dev, ev);
	if (ret != -EAGAIN)
		return ret;
	ret = read_ir(dev, ev);
	if (ret != -EAGAIN)
		return ret;
	ret = read_mp(dev, ev);
	if (ret != -EAGAIN)
		return ret;
	ret = read_bboard(dev, ev);
	if (ret != -EAGAIN)
		return ret;
	ret = read_pro(dev, ev);
	if (ret != -EAGAIN)
		return ret;

	return -EAGAIN;
}

int xwii_iface_read(struct xwii_iface *dev, struct xwii_event *ev)
{
	return read_iface(dev, ev);
}

/*
 * Poll for events on device \dev.
 * Returns -EAGAIN if no new events can be read.
 * Returns 0 on success and writes the new event into \ev.
 * Returns negative error on failure.
 * Returns -ENODEV *once* if *any* interface failed and got closed. Further
 * reads may succeed on other interfaces but this seems unlikely as all event
 * devices are created and destroyed by the kernel at the same time. Therefore,
 * it is recommended to assume the device was disconnected if this returns
 * -ENODEV.
 * Returns -EAGAIN on further reads if no interface is open anymore.
 *
 * This also writes all pending requests to the devices in contrast to *_read()
 * which only reads for events. If \ev is NULL, only pending requests are
 * written but no read is performed.
 */
int xwii_iface_poll(struct xwii_iface *dev, struct xwii_event *ev)
{
	if (!dev)
		return -EFAULT;

	if (ev)
		return read_iface(dev, ev);

	return 0;
}

/*
 * Toogle wiimote rumble motor
 * Enable or disable the rumble motor of \dev depending on \on. This requires
 * the core interface to be opened.
 */
int xwii_iface_rumble(struct xwii_iface *dev, bool on)
{
	struct input_event ev;
	int ret, fd;

	if (!dev)
		return -EINVAL;
	fd = dev->ifs[XWII_IF_CORE].fd;
	if (fd < 0 || dev->rumble_id < 0)
		return -ENODEV;

	ev.type = EV_FF;
	ev.code = dev->rumble_id;
	ev.value = on;
	ret = write(fd, &ev, sizeof(ev));

	if (ret == -1)
		return -errno;
	else
		return 0;
}

static int read_line(const char *path, char **out)
{
	FILE *f;
	char buf[4096], *line;

	f = fopen(path, "re");
	if (!f)
		return -errno;

	if (!fgets(buf, sizeof(buf), f)) {
		if (ferror(f)) {
			fclose(f);
			return errno ? -errno : -EINVAL;
		}
		buf[0] = 0;
	}

	fclose(f);

	line = strdup(buf);
	if (!line)
		return -ENOMEM;
	line[strcspn(line, "\n")] = 0;

	*out = line;
	return 0;
}

static int write_string(const char *path, const char *line)
{
	FILE *f;

	f = fopen(path, "we");
	if (!f)
		return -errno;

	fputs(line, f);
	fflush(f);

	if (ferror(f)) {
		fclose(f);
		return errno ? -errno : -EINVAL;
	}

	fclose(f);
	return 0;
}

static int read_led(const char *path, bool *state)
{
	int ret;
	char *line;

	ret = read_line(path, &line);
	if (ret)
		return ret;

	*state = !!atoi(line);
	free(line);

	return 0;
}

int xwii_iface_get_led(struct xwii_iface *dev, unsigned int led, bool *state)
{
	if (led > XWII_LED4 || led < XWII_LED1)
		return -EINVAL;
	if (!dev || !state)
		return -EINVAL;

	--led;
	if (!dev->led_attrs[led])
		return -ENODEV;

	return read_led(dev->led_attrs[led], state);
}

int xwii_iface_set_led(struct xwii_iface *dev, unsigned int led, bool state)
{
	if (!dev || led > XWII_LED4 || led < XWII_LED1)
		return -EINVAL;

	--led;
	if (!dev->led_attrs[led])
		return -ENODEV;

	return write_string(dev->led_attrs[led], state ? "1\n" : "0\n");
}

static int read_battery(const char *path, uint8_t *capacity)
{
	int ret;
	char *line;

	ret = read_line(path, &line);
	if (ret)
		return ret;

	*capacity = atoi(line);
	free(line);

	return 0;
}

int xwii_iface_get_battery(struct xwii_iface *dev, uint8_t *capacity)
{
	if (!dev || !capacity)
		return -EINVAL;
	if (!dev->battery_attr)
		return -ENODEV;

	return read_battery(dev->battery_attr, capacity);
}

int xwii_iface_get_devtype(struct xwii_iface *dev, char **devtype)
{
	if (!dev || !devtype)
		return -EINVAL;
	if (!dev->devtype_attr)
		return -ENODEV;

	return read_line(dev->devtype_attr, devtype);
}

int xwii_iface_get_extension(struct xwii_iface *dev, char **extension)
{
	if (!dev || !extension)
		return -EINVAL;
	if (!dev->extension_attr)
		return -ENODEV;

	return read_line(dev->extension_attr, extension);
}

int xwii_iface_mp_start_normalize(struct xwii_iface *dev, int32_t x, int32_t y, int32_t z, int32_t calibration_factor)
{
	if (!dev)
		return -EINVAL;
	int fd = dev->ifs[XWII_IF_MOTION_PLUS].fd;
	if (fd < 0)
		return -EINVAL;

	dev->mp_normalize = true;
		
	dev->mp_normalizer.x = x;
	dev->mp_normalizer.y = y;
	dev->mp_normalizer.z = z;
	dev->mp_calibration_factor = calibration_factor;
	return 0;
}

int xwii_iface_mp_get_normalize(struct xwii_iface *dev, int32_t *x, int32_t *y, int32_t *z, int32_t *calibration_factor)
{
	if (!dev || !x || !y || !z || !calibration_factor)
		return -EINVAL;
	int fd = dev->ifs[XWII_IF_MOTION_PLUS].fd;
	if (fd < 0)
		return -EINVAL;

	if (!dev->mp_normalize)
		return 1;
		
	*x = dev->mp_normalizer.x;
	*y = dev->mp_normalizer.y;
	*z = dev->mp_normalizer.z;
	*calibration_factor = dev->mp_calibration_factor; 
	return 0;
}

int xwii_iface_mp_stop_normalize(struct xwii_iface *dev)
{
	if (!dev)
		return -EINVAL;
	dev->mp_normalize = false;
	return 0;
}
