/*
 * yunzii-bridge — userspace HID bridge for USB keyboards whose vendor
 * interfaces break the kernel's usbhid probing (e.g. the wired mode of the
 * ROYUAN YUNZII B75 PRO, 3151:4015).
 *
 * What the device does wrong: interfaces 1 and 2 are declared as HID but
 * never answer the report-descriptor request. usbhid stalls ~26 s per
 * interface at every enumeration (and often the device drops off the bus
 * with -EPROTO), and the keyboard firmware hangs when the user presses
 * Fn+Space because it waits forever for the host to read those endpoints.
 *
 * What this daemon does:
 *   - claims interfaces 0, 1 and 2 via libusb (auto-detaching usbhid);
 *   - puts interface 0 into boot protocol (standard 8-byte reports);
 *   - re-exposes the keyboard to the kernel as a uhid device, using the
 *     canonical 63-byte USB HID boot-keyboard report descriptor, so the
 *     kernel keeps doing all HID parsing (keys and LEDs);
 *   - forwards uhid output reports (LEDs) back to the device via SET_REPORT;
 *   - drains the two vendor interrupt endpoints continuously, which keeps
 *     the firmware from blocking (Fn+Space).
 *
 * Recommended kernel cmdline (otherwise usbhid still probes the broken
 * vendor interfaces and stalls at boot):
 *     usbhid.quirks=0x3151:0x4015:0x4
 *
 * Usage: yunzii-bridge [VID_HEX PID_HEX]     (defaults: 3151 4015)
 *
 * Build: gcc -O2 -Wall -Wextra -std=gnu11 yunzii-bridge.c -o yunzii-bridge \
 *            -lusb-1.0 -lpthread
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libusb-1.0/libusb.h>
#include <linux/input.h>
#include <linux/uhid.h>

#define DEFAULT_VID 0x3151
#define DEFAULT_PID 0x4015

#define EP_KEYBOARD 0x81
#define EP_VENDOR1  0x82
#define EP_VENDOR2  0x83

#define POLL_MS 250

static volatile sig_atomic_t g_stop;
static uint16_t g_vid = DEFAULT_VID;
static uint16_t g_pid = DEFAULT_PID;

#define LOG(fmt, ...) \
	fprintf(stderr, "yunzii-bridge: " fmt "\n", ##__VA_ARGS__)

/*
 * Canonical USB HID boot-protocol keyboard report descriptor (63 bytes):
 * 8-byte input reports (modifier, reserved, 6 keys) and a 1-byte LED
 * output report. The device is told to use boot protocol via SET_PROTOCOL.
 */
static const unsigned char boot_kbd_rdesc[63] = {
	0x05, 0x01,             /* Usage Page (Generic Desktop)        */
	0x09, 0x06,             /* Usage (Keyboard)                    */
	0xa1, 0x01,             /* Collection (Application)            */
	0x05, 0x07,             /*   Usage Page (Key Codes)            */
	0x19, 0xe0,             /*   Usage Minimum (224)               */
	0x29, 0xe7,             /*   Usage Maximum (231)               */
	0x15, 0x00,             /*   Logical Minimum (0)               */
	0x25, 0x01,             /*   Logical Maximum (1)               */
	0x75, 0x01,             /*   Report Size (1)                   */
	0x95, 0x08,             /*   Report Count (8)                  */
	0x81, 0x02,             /*   Input (modifier byte)             */
	0x95, 0x01,             /*   Report Count (1)                  */
	0x75, 0x08,             /*   Report Size (8)                   */
	0x81, 0x01,             /*   Input (reserved byte)             */
	0x95, 0x05,             /*   Report Count (5)                  */
	0x75, 0x01,             /*   Report Size (1)                   */
	0x05, 0x08,             /*   Usage Page (LEDs)                 */
	0x19, 0x01,             /*   Usage Minimum (1)                 */
	0x29, 0x05,             /*   Usage Maximum (5)                 */
	0x91, 0x02,             /*   Output (LED byte)                 */
	0x95, 0x01,             /*   Report Count (1)                  */
	0x75, 0x03,             /*   Report Size (3)                   */
	0x91, 0x01,             /*   Output (padding)                  */
	0x95, 0x06,             /*   Report Count (6)                  */
	0x75, 0x08,             /*   Report Size (8)                   */
	0x15, 0x00,             /*   Logical Minimum (0)               */
	0x25, 0x65,             /*   Logical Maximum (101)             */
	0x05, 0x07,             /*   Usage Page (Key Codes)            */
	0x19, 0x00,             /*   Usage Minimum (0)                 */
	0x29, 0x65,             /*   Usage Maximum (101)               */
	0x81, 0x00,             /*   Input (6-key array)               */
	0xc0,                   /* End Collection                      */
};

struct device_ctx {
	struct device_ctx *next;
	uint8_t bus;
	uint8_t addr;
	libusb_device_handle *handle;
	int uhid_fd;
	atomic_int stop;
	atomic_int uhid_ready;
	pthread_mutex_t uhid_mtx;
	pthread_t threads[4];   /* 3 endpoint workers + 1 uhid reader */
	struct ep_ctx *eps;     /* malloc'ed, one per endpoint       */
	char phys[64];
	unsigned char rdesc[4096]; /* report descriptor for uhid      */
	uint16_t rdesc_size;
	int led_ok;                /* LED output reports supported     */
};

struct ep_ctx {
	struct device_ctx *dev;
	unsigned char ep;
	size_t size;
	time_t last_log;        /* throttle stall logging */
};

static struct device_ctx *g_devices;
static pthread_mutex_t g_devices_mtx = PTHREAD_MUTEX_INITIALIZER;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static ssize_t write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	size_t off = 0;

	while (off < len) {
		ssize_t n = write(fd, p + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		off += (size_t)n;
	}
	return (ssize_t)off;
}

/* ------------------------------------------------------------------ */
/* uhid                                                               */
/* ------------------------------------------------------------------ */

static int uhid_create(struct device_ctx *dev)
{
	struct uhid_event ev;
	int fd;

	fd = open("/dev/uhid", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		LOG("open /dev/uhid failed: %s", strerror(errno));
		return -1;
	}

	memset(&ev, 0, sizeof(ev));
	ev.type = UHID_CREATE2;
	snprintf((char *)ev.u.create2.name, sizeof(ev.u.create2.name),
		 "YUNZII B75 PRO (bridge)");
	snprintf((char *)ev.u.create2.phys, sizeof(ev.u.create2.phys),
		 "%s", dev->phys);
	ev.u.create2.rd_size = dev->rdesc_size;
	memcpy(ev.u.create2.rd_data, dev->rdesc, dev->rdesc_size);
	ev.u.create2.bus = BUS_USB;
	/*
	 * Deliberately NOT the real VID/PID: usbhid.quirks (HID_QUIRK_IGNORE
	 * for the physical device) matches HID devices by vendor/product,
	 * so a virtual device carrying the same IDs would be ignored too.
	 * 0x1209 is the pid.codes vendor ID, safe for userland devices.
	 */
	ev.u.create2.vendor = 0x1209;
	ev.u.create2.product = g_pid;
	ev.u.create2.version = 0;
	ev.u.create2.country = 0;

	if (write_all(fd, &ev, sizeof(ev)) < 0) {
		LOG("uhid CREATE2 write failed: %s", strerror(errno));
		close(fd);
		return -1;
	}

	LOG("uhid device created (%s)", dev->phys);
	dev->uhid_fd = fd;
	return 0;
}

static void *uhid_thread(void *arg)
{
	struct device_ctx *dev = arg;
	struct uhid_event ev;
	time_t created = time(NULL);

	while (!atomic_load_explicit(&dev->stop, memory_order_relaxed)) {
		struct pollfd pfd = { .fd = dev->uhid_fd, .events = POLLIN };
		int pr = poll(&pfd, 1, POLL_MS);

		if (pr < 0) {
			if (errno == EINTR)
				continue;
			LOG("poll(uhid) failed: %s", strerror(errno));
			break;
		}
		if (pr == 0) {
			/* If the kernel rejected UHID_CREATE2 (e.g. because a
			 * hid quirk matches our VID/PID), it sends no event:
			 * detect the silence and give up. */
			if (!atomic_load_explicit(&dev->uhid_ready,
						  memory_order_acquire) &&
			    time(NULL) - created > 5) {
				LOG("uhid device never started (create rejected "
				    "by the kernel?)");
				break;
			}
			continue;
		}

		ssize_t n = read(dev->uhid_fd, &ev, sizeof(ev));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			LOG("read(uhid) failed: %s", strerror(errno));
			break;
		}

		switch (ev.type) {
		case UHID_START:
			atomic_store_explicit(&dev->uhid_ready, 1,
					      memory_order_release);
			LOG("uhid device started (%s)", dev->phys);
			break;
		case UHID_OUTPUT:
			/* LED output report -> SET_REPORT on interface 0. */
			if (dev->led_ok &&
			    ev.u.output.rtype == UHID_OUTPUT_REPORT &&
			    ev.u.output.size > 0) {
				unsigned char led = ev.u.output.data[0];
				int rc = libusb_control_transfer(dev->handle,
					/* host-to-device, class, interface */
					0x21,
					/* SET_REPORT */
					0x09,
					/* report type output, report id 0 */
					0x0200,
					/* interface 0 */
					0,
					&led, 1, 500);
				if (rc < 0) {
					dev->led_ok = 0;
					LOG("LED output not supported (%s), "
					    "disabling", libusb_error_name(rc));
				}
			}
			break;
		case UHID_GET_REPORT: {
			struct uhid_event reply;

			memset(&reply, 0, sizeof(reply));
			reply.type = UHID_GET_REPORT_REPLY;
			reply.u.get_report_reply.id = ev.u.get_report.id;
			reply.u.get_report_reply.err = 0;
			reply.u.get_report_reply.size = 0;
			if (write_all(dev->uhid_fd, &reply, sizeof(reply)) < 0)
				LOG("uhid GET_REPORT_REPLY failed: %s",
				    strerror(errno));
			break;
		}
		case UHID_DESTROY:
			return NULL;
		default:
			break;
		}
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* USB endpoint workers                                               */
/* ------------------------------------------------------------------ */

static void *ep_thread(void *arg)
{
	struct ep_ctx *e = arg;
	struct device_ctx *dev = e->dev;
	unsigned char buf[16];
	int actual = 0;

	while (!atomic_load_explicit(&dev->stop, memory_order_relaxed)) {
		int rc = libusb_interrupt_transfer(dev->handle, e->ep, buf,
						   (int)e->size, &actual,
						   POLL_MS);

		if (rc == 0) {
			if (e->ep != EP_KEYBOARD)
				continue; /* vendor endpoint: drain */

			if (!atomic_load_explicit(&dev->uhid_ready,
						  memory_order_acquire))
				continue; /* kernel not ready yet: drop */

			struct uhid_event ev;

			memset(&ev, 0, sizeof(ev));
			ev.type = UHID_INPUT2;
			ev.u.input2.size = (uint16_t)actual;
			memcpy(ev.u.input2.data, buf, (size_t)actual);

			pthread_mutex_lock(&dev->uhid_mtx);
			if (write_all(dev->uhid_fd, &ev, sizeof(ev)) < 0)
				LOG("uhid INPUT2 failed: %s", strerror(errno));
			pthread_mutex_unlock(&dev->uhid_mtx);
			continue;
		}

		switch (rc) {
		case LIBUSB_ERROR_TIMEOUT:
		case LIBUSB_ERROR_INTERRUPTED:
			continue;
		case LIBUSB_ERROR_PIPE:
		case LIBUSB_ERROR_OVERFLOW:
			if (time(NULL) - e->last_log > 5) {
				e->last_log = time(NULL);
				LOG("endpoint 0x%02x stalled (%s), clearing",
				    e->ep, libusb_error_name(rc));
			}
			libusb_clear_halt(dev->handle, e->ep);
			continue;
		default:
			/* NO_DEVICE / NOT_FOUND / IO: device is gone. */
			return NULL;
		}
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Device lifecycle                                                   */
/* ------------------------------------------------------------------ */

static void device_free(struct device_ctx *dev);

static struct device_ctx *device_new(libusb_device *usbdev)
{
	static const struct {
		unsigned char ep;
		size_t size;
	} epspec[3] = {
		{ EP_KEYBOARD, 8 },
		{ EP_VENDOR1, 16 },
		{ EP_VENDOR2, 8 },
	};

	struct device_ctx *dev = calloc(1, sizeof(*dev));
	int rc, i;

	if (!dev)
		return NULL;

	atomic_init(&dev->stop, 0);
	atomic_init(&dev->uhid_ready, 0);
	pthread_mutex_init(&dev->uhid_mtx, NULL);
	dev->uhid_fd = -1;
	dev->led_ok = 1;

	dev->bus = libusb_get_bus_number(usbdev);
	dev->addr = libusb_get_device_address(usbdev);
	snprintf(dev->phys, sizeof(dev->phys),
		 "yunzii-bridge/%u/%u", dev->bus, dev->addr);

	rc = libusb_open(usbdev, &dev->handle);
	if (rc) {
		LOG("open device %u-%u failed: %s", dev->bus, dev->addr,
		    libusb_error_name(rc));
		goto fail;
	}
	libusb_set_auto_detach_kernel_driver(dev->handle, 1);

	/* Claim all three interfaces. At enumeration usbhid may still be
	 * probing the (broken) vendor interfaces for up to ~50 s, so retry
	 * with a backoff instead of giving up on the first EBUSY. With the
	 * recommended usbhid quirk enabled this completes immediately. */
	for (i = 0; i < 3; i++) {
		int attempt;

		for (attempt = 0; attempt < 40; attempt++) {
			rc = libusb_claim_interface(dev->handle, i);
			if (rc == 0)
				break;
			if (attempt == 0)
				LOG("claim interface %d on %u-%u pending "
				    "(%s), retrying", i, dev->bus, dev->addr,
				    libusb_error_name(rc));
			usleep(500000);
		}
		if (rc) {
			LOG("claim interface %d on %u-%u failed: %s", i,
			    dev->bus, dev->addr, libusb_error_name(rc));
			goto fail;
		}
	}

	/* Prefer the device's real report descriptor (read it while the
	 * device is still fresh: on this hardware, protocol-related control
	 * requests can make the firmware start STALLing everything). */
	rc = libusb_control_transfer(dev->handle,
		0x81,    /* device-to-host, standard, interface */
		0x06,    /* GET_DESCRIPTOR */
		0x2200,  /* HID report descriptor, index 0 */
		0,       /* interface 0 */
		dev->rdesc, (int)sizeof(dev->rdesc), 3000);
	if (rc > 0) {
		dev->rdesc_size = (uint16_t)rc;
		LOG("report descriptor read: %d bytes", rc);
	} else {
		LOG("GET_DESCRIPTOR failed (%s), trying boot protocol",
		    libusb_error_name(rc));
		rc = libusb_control_transfer(dev->handle,
			0x21,   /* host-to-device, class, interface */
			0x0b,   /* SET_PROTOCOL */
			0,      /* boot protocol */
			0,      /* interface 0 */
			NULL, 0, 1000);
		if (rc >= 0) {
			LOG("SET_PROTOCOL(boot) ok, using boot descriptor");
			memcpy(dev->rdesc, boot_kbd_rdesc,
			       sizeof(boot_kbd_rdesc));
			dev->rdesc_size = (uint16_t)sizeof(boot_kbd_rdesc);
		} else {
			/* Last resort: assume a boot-style report layout. */
			LOG("SET_PROTOCOL(boot) failed (%s), falling back "
			    "to boot descriptor", libusb_error_name(rc));
			memcpy(dev->rdesc, boot_kbd_rdesc,
			       sizeof(boot_kbd_rdesc));
			dev->rdesc_size = (uint16_t)sizeof(boot_kbd_rdesc);
		}
	}
	/* SET_IDLE(0): report only on change; harmless if unsupported. */
	libusb_control_transfer(dev->handle,
		0x21, 0x0a, 0, 0, NULL, 0, 500);

	if (uhid_create(dev) < 0)
		goto fail;

	dev->eps = calloc(3, sizeof(*dev->eps));
	if (!dev->eps)
		goto fail;

	for (i = 0; i < 3; i++) {
		dev->eps[i].dev = dev;
		dev->eps[i].ep = epspec[i].ep;
		dev->eps[i].size = epspec[i].size;
		rc = pthread_create(&dev->threads[i], NULL, ep_thread,
				    &dev->eps[i]);
		if (rc) {
			LOG("pthread_create(ep) failed: %s", strerror(rc));
			goto fail;
		}
	}

	rc = pthread_create(&dev->threads[3], NULL, uhid_thread, dev);
	if (rc) {
		LOG("pthread_create(uhid) failed: %s", strerror(rc));
		goto fail;
	}

	LOG("device %u-%u attached", dev->bus, dev->addr);
	return dev;

fail:
	atomic_store(&dev->stop, 1);
	for (i = 0; i < 4; i++)
		if (dev->threads[i])
			pthread_join(dev->threads[i], NULL);
	device_free(dev);
	return NULL;
}

static void device_free(struct device_ctx *dev)
{
	int i;

	atomic_store(&dev->stop, 1);

	for (i = 0; i < 4; i++)
		if (dev->threads[i])
			pthread_join(dev->threads[i], NULL);

	if (dev->uhid_fd >= 0)
		close(dev->uhid_fd); /* kernel destroys the uhid device */

	for (i = 0; i < 3; i++)
		libusb_release_interface(dev->handle, i);

	if (dev->handle)
		libusb_close(dev->handle);

	free(dev->eps);
	pthread_mutex_destroy(&dev->uhid_mtx);
	LOG("device %u-%u detached", dev->bus, dev->addr);
	free(dev);
}

/* ------------------------------------------------------------------ */
/* Hotplug                                                            */
/* ------------------------------------------------------------------ */

static int hotplug_cb(libusb_context *ctx, libusb_device *usbdev,
		      libusb_hotplug_event event, void *userdata)
{
	uint8_t bus = libusb_get_bus_number(usbdev);
	uint8_t addr = libusb_get_device_address(usbdev);
	(void)ctx;
	(void)userdata;

	pthread_mutex_lock(&g_devices_mtx);

	if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) {
		struct device_ctx *it;

		for (it = g_devices; it; it = it->next)
			if (it->bus == bus && it->addr == addr)
				break;
		if (!it) {
			struct device_ctx *dev = device_new(usbdev);

			if (dev) {
				dev->next = g_devices;
				g_devices = dev;
			}
		}
	} else { /* DEVICE_LEFT */
		struct device_ctx **pp = &g_devices;

		while (*pp) {
			struct device_ctx *dev = *pp;

			if (dev->bus == bus && dev->addr == addr) {
				*pp = dev->next;
				device_free(dev);
				break;
			}
			pp = &dev->next;
		}
	}

	pthread_mutex_unlock(&g_devices_mtx);
	return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [VID_HEX PID_HEX]\n"
		"  defaults: vid=0x%04x pid=0x%04x\n",
		argv0, DEFAULT_VID, DEFAULT_PID);
}

static int parse_hex(const char *s, uint16_t *out)
{
	char *end;
	unsigned long v = strtoul(s, &end, 16);

	if (*s == '\0' || *end != '\0' || v > 0xffff)
		return -1;
	*out = (uint16_t)v;
	return 0;
}

int main(int argc, char **argv)
{
	libusb_context *ctx = NULL;
	libusb_hotplug_callback_handle hp = 0;
	struct sigaction sa;
	struct timeval tv;
	int rc;

	if (argc == 3) {
		if (parse_hex(argv[1], &g_vid) || parse_hex(argv[2], &g_pid)) {
			usage(argv[0]);
			return 2;
		}
	} else if (argc != 1) {
		usage(argv[0]);
		return 2;
	}

	rc = libusb_init(&ctx);
	if (rc) {
		LOG("libusb_init failed: %s", libusb_error_name(rc));
		return 1;
	}

	rc = libusb_hotplug_register_callback(ctx,
		LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
		LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
		LIBUSB_HOTPLUG_ENUMERATE,
		g_vid, g_pid, LIBUSB_HOTPLUG_MATCH_ANY,
		hotplug_cb, NULL, &hp);
	if (rc) {
		LOG("hotplug register failed: %s", libusb_error_name(rc));
		libusb_exit(ctx);
		return 1;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	LOG("started (vid=0x%04x pid=0x%04x)", g_vid, g_pid);

	while (!g_stop) {
		tv.tv_sec = 0;
		tv.tv_usec = POLL_MS * 1000;
		libusb_handle_events_timeout_completed(ctx, &tv, NULL);
	}

	LOG("shutting down");

	pthread_mutex_lock(&g_devices_mtx);
	while (g_devices) {
		struct device_ctx *dev = g_devices;

		g_devices = dev->next;
		device_free(dev);
	}
	pthread_mutex_unlock(&g_devices_mtx);

	libusb_hotplug_deregister_callback(ctx, hp);
	libusb_exit(ctx);
	return 0;
}
