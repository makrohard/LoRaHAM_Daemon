/* A stub of the libusb-1.0 API subset the Meshstick adapter and monitor use,
 * for tests in lanes that have no libusb. The implementation is
 * tests/fakes/stub_libusb/libusb_stub.c; its state is driven by the test. */
#ifndef LORAHAM_STUB_LIBUSB_H
#define LORAHAM_STUB_LIBUSB_H

#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/types.h>

#define LIBUSB_CALL

typedef struct libusb_context libusb_context;
typedef struct libusb_device libusb_device;
typedef struct libusb_device_handle libusb_device_handle;
typedef int libusb_hotplug_callback_handle;

enum libusb_error {
    LIBUSB_SUCCESS = 0,
    LIBUSB_ERROR_IO = -1,
    LIBUSB_ERROR_ACCESS = -3,
    LIBUSB_ERROR_NO_DEVICE = -4,
    LIBUSB_ERROR_TIMEOUT = -7,
    LIBUSB_ERROR_OTHER = -99
};

enum libusb_capability { LIBUSB_CAP_HAS_HOTPLUG = 1 };

typedef enum {
    LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED = 1,
    LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT = 2
} libusb_hotplug_event;

#define LIBUSB_HOTPLUG_NO_FLAGS 0
#define LIBUSB_HOTPLUG_MATCH_ANY -1
#define LIBUSB_ENDPOINT_IN 0x80
#define LIBUSB_REQUEST_GET_DESCRIPTOR 0x06
#define LIBUSB_DT_STRING 0x03

struct libusb_device_descriptor {
    uint16_t idVendor;
    uint16_t idProduct;
    uint8_t iSerialNumber;
};

struct libusb_pollfd {
    int fd;
    short events;
};

typedef int (LIBUSB_CALL *libusb_hotplug_callback_fn)(libusb_context *, libusb_device *,
                                                       libusb_hotplug_event, void *);
typedef void (LIBUSB_CALL *libusb_pollfd_added_cb)(int, short, void *);
typedef void (LIBUSB_CALL *libusb_pollfd_removed_cb)(int, void *);

int libusb_init(libusb_context **ctx);
void libusb_exit(libusb_context *ctx);
int libusb_has_capability(uint32_t capability);
int libusb_pollfds_handle_timeouts(libusb_context *ctx);
int libusb_hotplug_register_callback(libusb_context *ctx, int events, int flags, int vid,
                                     int pid, int dev_class, libusb_hotplug_callback_fn cb,
                                     void *user, libusb_hotplug_callback_handle *handle);
void libusb_hotplug_deregister_callback(libusb_context *ctx, libusb_hotplug_callback_handle h);
void libusb_set_pollfd_notifiers(libusb_context *ctx, libusb_pollfd_added_cb added,
                                 libusb_pollfd_removed_cb removed, void *user);
const struct libusb_pollfd **libusb_get_pollfds(libusb_context *ctx);
void libusb_free_pollfds(const struct libusb_pollfd **fds);
int libusb_handle_events_timeout_completed(libusb_context *ctx, struct timeval *tv, int *done);
ssize_t libusb_get_device_list(libusb_context *ctx, libusb_device ***list);
void libusb_free_device_list(libusb_device **list, int unref);
libusb_device *libusb_ref_device(libusb_device *dev);
void libusb_unref_device(libusb_device *dev);
libusb_device *libusb_get_device(libusb_device_handle *h);
uint8_t libusb_get_bus_number(libusb_device *dev);
uint8_t libusb_get_device_address(libusb_device *dev);
int libusb_get_port_numbers(libusb_device *dev, uint8_t *ports, int len);
int libusb_get_device_descriptor(libusb_device *dev, struct libusb_device_descriptor *desc);
int libusb_open(libusb_device *dev, libusb_device_handle **h);
void libusb_close(libusb_device_handle *h);
int libusb_control_transfer(libusb_device_handle *h, uint8_t type, uint8_t req, uint16_t value,
                            uint16_t index, unsigned char *data, uint16_t len, unsigned timeout);

#endif
