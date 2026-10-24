/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Host-test ABI sizes, never compiled into a driver.
 */
#include "uvcpay.h"
#include "uvcdesc.h"
unsigned int UvcTestPaySize(void) { return sizeof(UVC_PAY); }
unsigned int UvcTestDescSize(void) { return sizeof(UVC_DESC); }
