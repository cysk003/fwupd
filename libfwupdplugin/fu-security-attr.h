/*
 * Copyright 2022 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <libfwupd/fwupd-security-attr.h>

#include "fu-context.h"

#define FU_TYPE_SECURITY_ATTR (fu_security_attr_get_type())
G_DECLARE_DERIVABLE_TYPE(FuSecurityAttr, fu_security_attr, FU, SECURITY_ATTR, FwupdSecurityAttr)

struct _FuSecurityAttrClass {
	FwupdSecurityAttrClass parent_class;
};

FwupdSecurityAttr *
fu_security_attr_new(FuContext *ctx, const gchar *appstream_id) G_GNUC_NON_NULL(1);
void
fu_security_attr_add_bios_target_value(FwupdSecurityAttr *attr,
				       const gchar *id,
				       const gchar *needle) G_GNUC_NON_NULL(1, 2);
gboolean
fu_security_attr_check_fwupd_version(FwupdSecurityAttr *attr, const gchar *fwupd_version)
    G_GNUC_NON_NULL(1);
