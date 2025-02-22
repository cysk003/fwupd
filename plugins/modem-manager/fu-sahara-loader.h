/*
 * Copyright 2021 Quectel Wireless Solutions Co., Ltd.
 *                    Ivan Mikhanchuk <ivan.mikhanchuk@quectel.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <fwupdplugin.h>

#define FU_TYPE_SAHARA_LOADER (fu_sahara_loader_get_type())
G_DECLARE_FINAL_TYPE(FuSaharaLoader, fu_sahara_loader, FU, SAHARA_LOADER, GObject)

FuSaharaLoader *
fu_sahara_loader_new(void);

gboolean
fu_sahara_loader_qdl_is_open(FuSaharaLoader *self);
GByteArray *
fu_sahara_loader_qdl_read(FuSaharaLoader *self, GError **error);
gboolean
fu_sahara_loader_qdl_write_bytes(FuSaharaLoader *self, GBytes *bytes, GError **error);
void
fu_sahara_loader_set_supports_zlp(FuSaharaLoader *self, gboolean supports_zlp);

gboolean
fu_sahara_loader_open(FuSaharaLoader *self, FuUsbDevice *usb_device, GError **error);
gboolean
fu_sahara_loader_run(FuSaharaLoader *self, GBytes *prog, GError **error);
gboolean
fu_sahara_loader_close(FuSaharaLoader *self, GError **error);
