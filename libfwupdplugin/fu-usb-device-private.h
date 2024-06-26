/*
 * Copyright 2018 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "fu-usb-device.h"

#define FU_USB_DEVICE_EMULATION_TAG "org.freedesktop.fwupd.emulation.v1"

FuUsbDevice *
fu_usb_device_new(FuContext *ctx, GUsbDevice *usb_device) G_GNUC_NON_NULL(1);
const gchar *
fu_usb_device_get_platform_id(FuUsbDevice *self) G_GNUC_NON_NULL(1);
