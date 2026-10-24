/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Taken from libusb-win32 0.1.12.2 (git 00eb396), this path.
 * The header below names no LGPL version; recorded as LGPL-2.1-or-later,
 * the version of the COPYING_LGPL.txt shipped with it (docs/design/libusb.md 2.4).
 */
#ifndef __ERROR_H__
#define __ERROR_H__

/* Connection timed out */
#define ETIMEDOUT 116	

typedef enum {
  USB_ERROR_TYPE_NONE = 0,
  USB_ERROR_TYPE_STRING,
  USB_ERROR_TYPE_ERRNO,
} usb_error_type_t;


void usb_error(char *format, ...);
void usb_message(char *format, ...);
const char *usb_win_error_to_string(void);
int usb_win_error_to_errno(void);



#endif /* _ERROR_H_ */

