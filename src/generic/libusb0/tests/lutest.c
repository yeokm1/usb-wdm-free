/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * Host tests of LIBUSB0.DLL's pure parts, linked with the package's own
 * src/descriptors.c and src/error.c (LGPL-2.1-or-later, hence this
 * file's licence): descriptor parsing as libusb-0.1 programs see it, the
 * two-step configuration fetch, the libusb_request layout and IOCTL codes
 * the driver and DLL share, and the Win32 error mapping. No device, driver
 * or USB stack is touched: usb_control_msg is a stub serving a canned
 * descriptor set.
 */

#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <windows.h>
#include <winioctl.h>

#include "usbi.h"
#include "driver_api.h"

int usb_debug = 0;

static int failures = 0;
static int checks = 0;

#define CHECK(c) do { checks++; if (!(c)) { failures++; \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- the canned device ------------------------------------------------ */

/* Configuration 1: a class descriptor before the first interface (config
 * extra), interface 0 with two alternate settings (alt 0: a class
 * interface descriptor and two bulk endpoints; alt 1: one interrupt
 * endpoint followed by a class endpoint descriptor), interface 1 with no
 * endpoints. */
static unsigned char cfg[] = {
  9, 2, 0, 0, 2, 1, 0, 0x80, 50,                   /* config, wTotalLength set below */
  8, 0x0B, 0, 2, 0xFF, 0, 0, 0,                    /* interface association (extra) */
  9, 4, 0, 0, 2, 0xFF, 0, 0, 0,                    /* if 0 alt 0 */
  5, 0x24, 1, 2, 3,                                /* class interface descriptor */
  7, 5, 0x81, 2, 0x40, 0x00, 0,                    /* ep 81 bulk 64 */
  7, 5, 0x02, 2, 0x00, 0x02, 0,                    /* ep 02 bulk 512 */
  9, 4, 0, 1, 1, 0xFF, 0, 0, 0,                    /* if 0 alt 1 */
  7, 5, 0x83, 3, 0x08, 0x00, 10,                   /* ep 83 interrupt 8 */
  4, 0x25, 1, 0,                                   /* class endpoint descriptor */
  9, 4, 1, 0, 0, 0xFE, 1, 1, 0,                    /* if 1 alt 0, no endpoints */
};

static int ctl_calls = 0;
static int ctl_len[4];
static int ctl_value[4];
static int ctl_type[4];

int usb_control_msg(usb_dev_handle *dev, int requesttype, int request,
                    int value, int index, char *bytes, int size, int timeout)
{
  int n;
  (void)dev; (void)index; (void)timeout;
  if (ctl_calls < 4) {
    ctl_len[ctl_calls] = size;
    ctl_value[ctl_calls] = value;
    ctl_type[ctl_calls] = requesttype;
  }
  ctl_calls++;
  if (request != USB_REQ_GET_DESCRIPTOR || (value >> 8) != USB_DT_CONFIG)
    return -EINVAL;
  n = size < (int)sizeof(cfg) ? size : (int)sizeof(cfg);
  memcpy(bytes, cfg, n);
  return n;
}

/* ---- tests ------------------------------------------------------------ */

static void test_parse_descriptor(void)
{
  static unsigned char dd[18] = {
    18, 1, 0x10, 0x01, 0xFF, 0, 0, 8, 0xC0, 0x16, 0xDC, 0x05, 0x02, 0x01, 1, 2, 0, 1
  };
  struct usb_device_descriptor d;
  int n;

  memset(&d, 0xAA, sizeof(d));
  n = usb_parse_descriptor(dd, "bbwbbbbwwwbbbb", &d);
  CHECK(n == 18);
  CHECK(sizeof(d) == 18);
  CHECK(d.bLength == 18 && d.bDescriptorType == USB_DT_DEVICE);
  CHECK(d.bcdUSB == 0x0110);
  CHECK(d.bMaxPacketSize0 == 8);
  CHECK(d.idVendor == 0x16C0 && d.idProduct == 0x05DC);
  CHECK(d.bcdDevice == 0x0102);
  CHECK(d.iManufacturer == 1 && d.iProduct == 2 && d.iSerialNumber == 0);
  CHECK(d.bNumConfigurations == 1);
}

static void check_config(struct usb_config_descriptor *c)
{
  struct usb_interface *i0, *i1;
  struct usb_interface_descriptor *a0, *a1;

  CHECK(c->bNumInterfaces == 2);
  CHECK(c->wTotalLength == sizeof(cfg));
  CHECK(c->bConfigurationValue == 1 && c->bmAttributes == 0x80 && c->MaxPower == 50);
  CHECK(c->extralen == 8 && c->extra && c->extra[1] == 0x0B);
  i0 = &c->interface[0];
  i1 = &c->interface[1];
  CHECK(i0->num_altsetting == 2);
  a0 = &i0->altsetting[0];
  a1 = &i0->altsetting[1];
  CHECK(a0->bInterfaceNumber == 0 && a0->bAlternateSetting == 0 && a0->bNumEndpoints == 2);
  CHECK(a0->extralen == 5 && a0->extra && a0->extra[1] == 0x24);
  CHECK(a0->endpoint[0].bEndpointAddress == 0x81 && a0->endpoint[0].wMaxPacketSize == 64);
  CHECK(a0->endpoint[1].bEndpointAddress == 0x02 && a0->endpoint[1].wMaxPacketSize == 512);
  CHECK(a0->endpoint[0].extralen == 0 && a0->endpoint[0].extra == NULL);
  CHECK(a1->bAlternateSetting == 1 && a1->bNumEndpoints == 1);
  CHECK(a1->endpoint[0].bEndpointAddress == 0x83 && a1->endpoint[0].bmAttributes == 3);
  CHECK(a1->endpoint[0].bInterval == 10);
  CHECK(a1->endpoint[0].extralen == 4 && a1->endpoint[0].extra[1] == 0x25);
  CHECK(i1->num_altsetting == 1);
  CHECK(i1->altsetting[0].bInterfaceNumber == 1 && i1->altsetting[0].bNumEndpoints == 0);
  CHECK(i1->altsetting[0].endpoint == NULL && i1->altsetting[0].bInterfaceClass == 0xFE);
}

static void test_parse_configuration(void)
{
  struct usb_device dev;

  memset(&dev, 0, sizeof(dev));
  dev.descriptor.bNumConfigurations = 1;
  dev.config = malloc(sizeof(struct usb_config_descriptor));
  memset(dev.config, 0, sizeof(struct usb_config_descriptor));
  CHECK(usb_parse_configuration(dev.config, cfg) == 0);   /* nothing left over */
  check_config(dev.config);
  usb_destroy_configuration(&dev);
}

static void test_malformed(void)
{
  unsigned char b[sizeof(cfg)];
  struct usb_config_descriptor c;

  /* A zero-length descriptor before the first interface. */
  memcpy(b, cfg, sizeof(cfg));
  b[9] = 0;
  memset(&c, 0, sizeof(c));
  CHECK(usb_parse_configuration(&c, b) < 0);

  /* A one-byte descriptor after an interface descriptor. */
  memcpy(b, cfg, sizeof(cfg));
  b[26] = 1;
  memset(&c, 0, sizeof(c));
  CHECK(usb_parse_configuration(&c, b) < 0);

  /* More than USB_MAXINTERFACES interfaces. */
  memcpy(b, cfg, sizeof(cfg));
  b[4] = USB_MAXINTERFACES + 1;
  memset(&c, 0, sizeof(c));
  CHECK(usb_parse_configuration(&c, b) < 0);

  /* More than USB_MAXENDPOINTS endpoints in one interface. */
  memcpy(b, cfg, sizeof(cfg));
  b[17 + 4] = USB_MAXENDPOINTS + 1;
  memset(&c, 0, sizeof(c));
  CHECK(usb_parse_configuration(&c, b) < 0);
}

static void test_fetch(void)
{
  struct usb_device dev;
  struct usb_dev_handle h;

  memset(&dev, 0, sizeof(dev));
  memset(&h, 0, sizeof(h));
  h.device = &dev;
  dev.descriptor.bNumConfigurations = 1;
  ctl_calls = 0;
  usb_fetch_and_parse_descriptors(&h);
  /* First the 8-byte header, then wTotalLength bytes. */
  CHECK(ctl_calls == 2);
  CHECK(ctl_len[0] == 8 && ctl_len[1] == sizeof(cfg));
  CHECK(ctl_value[0] == (USB_DT_CONFIG << 8) && ctl_value[1] == (USB_DT_CONFIG << 8));
  CHECK(ctl_type[0] == USB_ENDPOINT_IN && ctl_type[1] == USB_ENDPOINT_IN);
  CHECK(dev.config != NULL);
  if (dev.config)
    check_config(dev.config);
  usb_destroy_configuration(&dev);

  /* More configurations than USB_MAXCONFIG: nothing is fetched. */
  memset(&dev, 0, sizeof(dev));
  dev.descriptor.bNumConfigurations = USB_MAXCONFIG + 1;
  ctl_calls = 0;
  usb_fetch_and_parse_descriptors(&h);
  CHECK(ctl_calls == 0 && dev.config == NULL);
}

/* The request block and IOCTL codes are the DLL/driver ABI that upstream
 * DLLs and drivers of 0.1.12.x and 1.2.x share (docs/design/libusb.md 2.5). */
static void test_abi(void)
{
  libusb_request r;

  CHECK(sizeof(libusb_request) == 24);
  CHECK(offsetof(libusb_request, timeout) == 0);
  CHECK((char *)&r.configuration.configuration - (char *)&r == 4);
  CHECK((char *)&r.endpoint.endpoint - (char *)&r == 4);
  CHECK((char *)&r.endpoint.packet_size - (char *)&r == 8);
  CHECK((char *)&r.vendor.index - (char *)&r == 20);
  CHECK((char *)&r.descriptor.recipient - (char *)&r == 16);
  CHECK((char *)&r.version.nano - (char *)&r == 16);
  CHECK(LIBUSB_IOCTL_SET_CONFIGURATION == 0x00222004);
  CHECK(LIBUSB_IOCTL_GET_DESCRIPTOR == 0x00222024);
  CHECK(LIBUSB_IOCTL_INTERRUPT_OR_BULK_WRITE == 0x00222029);
  CHECK(LIBUSB_IOCTL_INTERRUPT_OR_BULK_READ == 0x0022202E);
  CHECK(LIBUSB_IOCTL_VENDOR_READ == 0x00222034);
  CHECK(LIBUSB_IOCTL_RESET_DEVICE == 0x00222040);
  CHECK(LIBUSB_IOCTL_GET_VERSION == 0x00222048);
  CHECK(LIBUSB_IOCTL_ISOCHRONOUS_READ == 0x00222052);
  CHECK(LIBUSB_IOCTL_RELEASE_INTERFACE == 0x00222058);
  CHECK(LIBUSB_MAX_READ_WRITE == 65536);
}

static void test_errors(void)
{
  SetLastError(ERROR_SUCCESS);
  CHECK(usb_win_error_to_errno() == 0);
  SetLastError(ERROR_INVALID_PARAMETER);
  CHECK(usb_win_error_to_errno() == EINVAL);
  SetLastError(ERROR_SEM_TIMEOUT);
  CHECK(usb_win_error_to_errno() == ETIMEDOUT);
  SetLastError(ERROR_OPERATION_ABORTED);
  CHECK(usb_win_error_to_errno() == ETIMEDOUT);
  SetLastError(ERROR_NOT_ENOUGH_MEMORY);
  CHECK(usb_win_error_to_errno() == ENOMEM);
  SetLastError(ERROR_GEN_FAILURE);
  CHECK(usb_win_error_to_errno() == EIO);
  usb_error("x %d", 7);
  CHECK(strcmp(usb_strerror(), "x 7") == 0);
}

int main(void)
{
  cfg[2] = (unsigned char)(sizeof(cfg) & 0xFF);
  cfg[3] = (unsigned char)(sizeof(cfg) >> 8);
  test_parse_descriptor();
  test_parse_configuration();
  test_malformed();
  test_fetch();
  test_abi();
  test_errors();
  printf("%s: %d checks, %d failed\n", failures ? "FAIL" : "PASS", checks, failures);
  return failures ? 1 : 0;
}
