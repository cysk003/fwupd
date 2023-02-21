/*
 * Copyright (C) 2021 Jeffrey Lin <jlin@kinet-ic.com>
 * Copyright (C) 2022 Hai Su <hsu@kinet-ic.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <fwupdplugin.h>

#include "fu-kinetic-dp-aux-dpcd.h"

gboolean
fu_kinetic_dp_aux_dpcd_read_oui(FuKineticDpConnection *connection,
				guint8 *buf,
				gsize bufsz,
				GError **error)
{
	if (bufsz < DPCD_SIZE_IEEE_OUI) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INTERNAL,
			    "aux dpcd read buffer size [%u] is too small to read IEEE OUI",
			    (guint)bufsz);
		return FALSE;
	}
	if (!fu_kinetic_dp_connection_read(connection,
					   DPCD_ADDR_IEEE_OUI,
					   buf,
					   DPCD_SIZE_IEEE_OUI,
					   error)) {
		g_prefix_error(error, "aux dpcd read OUI failed: ");
		return FALSE;
	}
	return TRUE;
}

gboolean
fu_kinetic_dp_aux_dpcd_write_oui(FuKineticDpConnection *connection,
				 const guint8 *buf,
				 GError **error)
{
	if (!fu_kinetic_dp_connection_write(connection,
					    DPCD_ADDR_IEEE_OUI,
					    buf,
					    DPCD_SIZE_IEEE_OUI,
					    error)) {
		g_prefix_error(error, "aux dpcd write OUI failed: ");
		return FALSE;
	}
	return TRUE;
}

gboolean
fu_kinetic_dp_aux_dpcd_read_branch_id_str(FuKineticDpConnection *connection,
					  guint8 *buf,
					  gsize bufsz,
					  GError **error)
{
	if (bufsz < DPCD_SIZE_BRANCH_DEV_ID_STR) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INTERNAL,
			    "aux dpcd read buffer size [%u] is too small to read branch ID string",
			    (guint)bufsz);
		return FALSE;
	}

	/* Clear the buffer to all 0s as DP spec mentioned */
	memset(buf, 0, DPCD_SIZE_BRANCH_DEV_ID_STR);

	if (!fu_kinetic_dp_connection_read(connection,
					   DPCD_ADDR_BRANCH_DEV_ID_STR,
					   buf,
					   DPCD_SIZE_BRANCH_DEV_ID_STR,
					   error)) {
		g_prefix_error(error, "aux dpcd read branch device ID string failed: ");
		return FALSE;
	}
	return TRUE;
}
