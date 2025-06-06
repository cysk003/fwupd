/*
 * Copyright 2018 Evan Lojewski
 * Copyright 2020 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef HAVE_MMAN_H
#include <sys/mman.h>
#endif

#ifdef HAVE_VALGRIND
#include <valgrind.h>
#endif /* HAVE_VALGRIND */

#include "fu-bcm57xx-common.h"
#include "fu-bcm57xx-firmware.h"
#include "fu-bcm57xx-recovery-device.h"

/* offsets into BAR[0] */
#define REG_DEVICE_PCI_VENDOR_DEVICE_ID 0x6434
#define REG_NVM_SOFTWARE_ARBITRATION	0x7020
#define REG_NVM_ACCESS			0x7024
#define REG_NVM_COMMAND			0x7000
#define REG_NVM_ADDR			0x700c
#define REG_NVM_READ			0x7010
#define REG_NVM_WRITE			0x7008

/* offsets into BAR[2] */
#define REG_APE_MODE 0x0

typedef struct {
	guint8 *buf;
	gsize bufsz;
} FuBcm57xxMmap;

#define FU_BCM57XX_BAR_DEVICE 0
#define FU_BCM57XX_BAR_APE    1
#define FU_BCM57XX_BAR_MAX    3

struct _FuBcm57xxRecoveryDevice {
	FuUdevDevice parent_instance;
	FuBcm57xxMmap bar[FU_BCM57XX_BAR_MAX];
};

typedef union {
	guint32 r32;
	struct {
		guint32 reserved_0_0 : 1;
		guint32 Reset : 1;
		guint32 reserved_2_2 : 1;
		guint32 Done : 1;
		guint32 Doit : 1;
		guint32 Wr : 1;
		guint32 Erase : 1;
		guint32 First : 1;
		guint32 Last : 1;
		guint32 reserved_15_9 : 7;
		guint32 WriteEnableCommand : 1;
		guint32 WriteDisableCommand : 1;
		guint32 reserved_31_18 : 14;
	} __attribute__((packed)) bits; /* nocheck:blocked */
} BcmRegNVMCommand;

typedef union {
	guint32 r32;
	struct {
		guint32 ReqSet0 : 1;
		guint32 ReqSet1 : 1;
		guint32 ReqSet2 : 1;
		guint32 ReqSet3 : 1;
		guint32 ReqClr0 : 1;
		guint32 ReqClr1 : 1;
		guint32 ReqClr2 : 1;
		guint32 ReqClr3 : 1;
		guint32 ArbWon0 : 1;
		guint32 ArbWon1 : 1;
		guint32 ArbWon2 : 1;
		guint32 ArbWon3 : 1;
		guint32 Req0 : 1;
		guint32 Req1 : 1;
		guint32 Req2 : 1;
		guint32 Req3 : 1;
		guint32 reserved_31_16 : 16;
	} __attribute__((packed)) bits; /* nocheck:blocked */
} BcmRegNVMSoftwareArbitration;

typedef union {
	guint32 r32;
	struct {
		guint32 Enable : 1;
		guint32 WriteEnable : 1;
		guint32 reserved_31_2 : 30;
	} __attribute__((packed)) bits; /* nocheck:blocked */
} BcmRegNVMAccess;

typedef union {
	guint32 r32;
	struct {
		guint32 Reset : 1;
		guint32 Halt : 1;
		guint32 FastBoot : 1;
		guint32 HostDiag : 1;
		guint32 reserved_4_4 : 1;
		guint32 Event1 : 1;
		guint32 Event2 : 1;
		guint32 GRCint : 1;
		guint32 reserved_8_8 : 1;
		guint32 SwapATBdword : 1;
		guint32 reserved_10_10 : 1;
		guint32 SwapARBdword : 1;
		guint32 reserved_13_12 : 2;
		guint32 Channel0Enable : 1;
		guint32 Channel2Enable : 1;
		guint32 reserved_17_16 : 2;
		guint32 MemoryECC : 1;
		guint32 ICodePIPRdDisable : 1;
		guint32 reserved_29_20 : 10;
		guint32 Channel1Enable : 1;
		guint32 Channel3Enable : 1;
	} __attribute__((packed)) bits; /* nocheck:blocked */
} BcmRegAPEMode;

G_DEFINE_TYPE(FuBcm57xxRecoveryDevice, fu_bcm57xx_recovery_device, FU_TYPE_UDEV_DEVICE)

#ifdef __ppc64__
#define BARRIER() __asm__ volatile("sync 0\neieio\n" : : : "memory")
#else
#define BARRIER() __asm__ volatile("" : : : "memory");
#endif

static gboolean
fu_bcm57xx_recovery_device_bar_read(FuBcm57xxRecoveryDevice *self,
				    guint bar,
				    gsize offset,
				    guint32 *val,
				    GError **error)
{
	/* this should never happen */
	if (self->bar[bar].buf == NULL) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INTERNAL,
			    "BAR[%u] is not mapped!",
			    bar);
		return FALSE;
	}

	BARRIER();
	return fu_memcpy_safe((guint8 *)val,
			      sizeof(*val),
			      0x0, /* dst */
			      self->bar[bar].buf,
			      self->bar[bar].bufsz,
			      offset,
			      sizeof(*val),
			      error);
}

static gboolean
fu_bcm57xx_recovery_device_bar_write(FuBcm57xxRecoveryDevice *self,
				     guint bar,
				     gsize offset,
				     guint32 val,
				     GError **error)
{
	/* this should never happen */
	if (self->bar[bar].buf == NULL) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INTERNAL,
			    "BAR[%u] is not mapped!",
			    bar);
		return FALSE;
	}

	BARRIER();
	if (!fu_memcpy_safe(self->bar[bar].buf,
			    self->bar[bar].bufsz,
			    offset, /* dst */
			    (const guint8 *)&val,
			    sizeof(val),
			    0x0, /* src */
			    sizeof(val),
			    error))
		return FALSE;
	BARRIER();
	return TRUE;
}

static gboolean
fu_bcm57xx_recovery_device_nvram_disable(FuBcm57xxRecoveryDevice *self, GError **error)
{
	BcmRegNVMAccess tmp;
	if (!fu_bcm57xx_recovery_device_bar_read(self,
						 FU_BCM57XX_BAR_DEVICE,
						 REG_NVM_ACCESS,
						 &tmp.r32,
						 error))
		return FALSE;
	tmp.bits.Enable = FALSE;
	tmp.bits.WriteEnable = FALSE;
	return fu_bcm57xx_recovery_device_bar_write(self,
						    FU_BCM57XX_BAR_DEVICE,
						    REG_NVM_ACCESS,
						    tmp.r32,
						    error);
}

static gboolean
fu_bcm57xx_recovery_device_nvram_enable(FuBcm57xxRecoveryDevice *self, GError **error)
{
	BcmRegNVMAccess tmp;
	if (!fu_bcm57xx_recovery_device_bar_read(self,
						 FU_BCM57XX_BAR_DEVICE,
						 REG_NVM_ACCESS,
						 &tmp.r32,
						 error))
		return FALSE;
	tmp.bits.Enable = TRUE;
	tmp.bits.WriteEnable = FALSE;
	return fu_bcm57xx_recovery_device_bar_write(self,
						    FU_BCM57XX_BAR_DEVICE,
						    REG_NVM_ACCESS,
						    tmp.r32,
						    error);
}

static gboolean
fu_bcm57xx_recovery_device_nvram_enable_write(FuBcm57xxRecoveryDevice *self, GError **error)
{
	BcmRegNVMAccess tmp;
	if (!fu_bcm57xx_recovery_device_bar_read(self,
						 FU_BCM57XX_BAR_DEVICE,
						 REG_NVM_ACCESS,
						 &tmp.r32,
						 error))
		return FALSE;
	tmp.bits.Enable = TRUE;
	tmp.bits.WriteEnable = TRUE;
	return fu_bcm57xx_recovery_device_bar_write(self,
						    FU_BCM57XX_BAR_DEVICE,
						    REG_NVM_ACCESS,
						    tmp.r32,
						    error);
}

static gboolean
fu_bcm57xx_recovery_device_nvram_acquire_lock(FuBcm57xxRecoveryDevice *self, GError **error)
{
	BcmRegNVMSoftwareArbitration tmp = {0};
	g_autoptr(GTimer) timer = g_timer_new();
	tmp.bits.ReqSet1 = 1;
	if (!fu_bcm57xx_recovery_device_bar_write(self,
						  FU_BCM57XX_BAR_DEVICE,
						  REG_NVM_SOFTWARE_ARBITRATION,
						  tmp.r32,
						  error))
		return FALSE;
	do {
		if (!fu_bcm57xx_recovery_device_bar_read(self,
							 FU_BCM57XX_BAR_DEVICE,
							 REG_NVM_SOFTWARE_ARBITRATION,
							 &tmp.r32,
							 error))
			return FALSE;
		if (tmp.bits.ArbWon1)
			return TRUE;
		if (g_timer_elapsed(timer, NULL) > 0.2)
			break;
	} while (TRUE);

	/* timed out */
	g_set_error_literal(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_TIMED_OUT,
			    "timed out trying to acquire lock #1");
	return FALSE;
}

static gboolean
fu_bcm57xx_recovery_device_nvram_release_lock(FuBcm57xxRecoveryDevice *self, GError **error)
{
	BcmRegNVMSoftwareArbitration tmp = {0};
	tmp.r32 = 0;
	tmp.bits.ReqClr1 = 1;
	return fu_bcm57xx_recovery_device_bar_write(self,
						    FU_BCM57XX_BAR_DEVICE,
						    REG_NVM_SOFTWARE_ARBITRATION,
						    tmp.r32,
						    error);
}

static gboolean
fu_bcm57xx_recovery_device_nvram_wait_done(FuBcm57xxRecoveryDevice *self, GError **error)
{
	BcmRegNVMCommand tmp = {0};
	g_autoptr(GTimer) timer = g_timer_new();
	do {
		if (!fu_bcm57xx_recovery_device_bar_read(self,
							 FU_BCM57XX_BAR_DEVICE,
							 REG_NVM_COMMAND,
							 &tmp.r32,
							 error))
			return FALSE;
		if (tmp.bits.Done)
			return TRUE;
		if (g_timer_elapsed(timer, NULL) > 0.2)
			break;
	} while (TRUE);

	/* timed out */
	g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_TIMED_OUT, "timed out");
	return FALSE;
}

static gboolean
fu_bcm57xx_recovery_device_nvram_clear_done(FuBcm57xxRecoveryDevice *self, GError **error)
{
	BcmRegNVMCommand tmp = {0};
	tmp.bits.Done = 1;
	return fu_bcm57xx_recovery_device_bar_write(self,
						    FU_BCM57XX_BAR_DEVICE,
						    REG_NVM_COMMAND,
						    tmp.r32,
						    error);
}

static gboolean
fu_bcm57xx_recovery_device_nvram_read(FuBcm57xxRecoveryDevice *self,
				      guint32 address,
				      guint32 *buf,
				      gsize bufsz,
				      FuProgress *progress,
				      GError **error)
{
	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_set_steps(progress, bufsz);
	for (guint i = 0; i < bufsz; i++) {
		BcmRegNVMCommand tmp = {0};
		guint32 val32 = 0;
		if (!fu_bcm57xx_recovery_device_nvram_clear_done(self, error))
			return FALSE;
		if (!fu_bcm57xx_recovery_device_bar_write(self,
							  FU_BCM57XX_BAR_DEVICE,
							  REG_NVM_ADDR,
							  address,
							  error))
			return FALSE;
		tmp.bits.Doit = 1;
		tmp.bits.First = i == 0;
		tmp.bits.Last = i == bufsz - 1;

		if (!fu_bcm57xx_recovery_device_bar_write(self,
							  FU_BCM57XX_BAR_DEVICE,
							  REG_NVM_COMMAND,
							  tmp.r32,
							  error))
			return FALSE;
		if (!fu_bcm57xx_recovery_device_nvram_wait_done(self, error)) {
			g_prefix_error(error, "failed to read @0x%x: ", address);
			return FALSE;
		}
		if (!fu_bcm57xx_recovery_device_bar_read(self,
							 FU_BCM57XX_BAR_DEVICE,
							 REG_NVM_READ,
							 &val32,
							 error))
			return FALSE;
		buf[i] = GUINT32_FROM_BE(val32); /* nocheck:blocked */
		address += sizeof(guint32);
		fu_progress_step_done(progress);
	}

	/* success */
	return TRUE;
}

static gboolean
fu_bcm57xx_recovery_device_nvram_write(FuBcm57xxRecoveryDevice *self,
				       guint32 address,
				       const guint32 *buf,
				       gsize bufsz_dwrds,
				       FuProgress *progress,
				       GError **error)
{
	const guint32 page_size_dwrds = 64;

	/* can only write in pages of 64 dwords */
	if (bufsz_dwrds % page_size_dwrds != 0 ||
	    (address * sizeof(guint32)) % page_size_dwrds != 0) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "can only write aligned with page size 0x%x",
			    page_size_dwrds);
		return FALSE;
	}

	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_set_steps(progress, bufsz_dwrds);
	for (guint i = 0; i < bufsz_dwrds; i++) {
		BcmRegNVMCommand tmp = {0};
		if (!fu_bcm57xx_recovery_device_nvram_clear_done(self, error))
			return FALSE;
		if (!fu_bcm57xx_recovery_device_bar_write(
			self,
			FU_BCM57XX_BAR_DEVICE,
			REG_NVM_WRITE,
			GUINT32_TO_BE(buf[i]), /* nocheck:blocked */
			error))
			return FALSE;
		if (!fu_bcm57xx_recovery_device_bar_write(self,
							  FU_BCM57XX_BAR_DEVICE,
							  REG_NVM_ADDR,
							  address,
							  error))
			return FALSE;
		tmp.bits.Wr = TRUE;
		tmp.bits.Doit = TRUE;
		tmp.bits.First = i % page_size_dwrds == 0;
		tmp.bits.Last = (i + 1) % page_size_dwrds == 0;
		if (!fu_bcm57xx_recovery_device_bar_write(self,
							  FU_BCM57XX_BAR_DEVICE,
							  REG_NVM_COMMAND,
							  tmp.r32,
							  error))
			return FALSE;
		if (!fu_bcm57xx_recovery_device_nvram_wait_done(self, error)) {
			g_prefix_error(error, "failed to write @0x%x: ", address);
			return FALSE;
		}
		address += sizeof(guint32);
		fu_progress_step_done(progress);
	}

	/* success */
	return TRUE;
}

static gboolean
fu_bcm57xx_recovery_device_detach(FuDevice *device, FuProgress *progress, GError **error)
{
	/* unbind tg3 */
	return fu_device_unbind_driver(device, error);
}

static gboolean
fu_bcm57xx_recovery_device_attach(FuDevice *device, FuProgress *progress, GError **error)
{
	g_autoptr(GError) error_local = NULL;

	/* bind tg3, which might fail if the module is not compiled */
	if (!fu_device_bind_driver(device, "pci", "tg3", &error_local)) {
		if (g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED)) {
			g_warning("failed to bind tg3: %s", error_local->message);
		} else {
			g_propagate_prefixed_error(error,
						   g_steal_pointer(&error_local),
						   "failed to bind tg3: ");
			return FALSE;
		}
	}

	/* success */
	return TRUE;
}

static gboolean
fu_bcm57xx_recovery_device_activate(FuDevice *device, FuProgress *progress, GError **error)
{
	BcmRegAPEMode mode = {0};
	FuBcm57xxRecoveryDevice *self = FU_BCM57XX_RECOVERY_DEVICE(device);

	/* halt */
	mode.bits.Halt = 1;
	mode.bits.FastBoot = 0;
	if (!fu_bcm57xx_recovery_device_bar_write(self,
						  FU_BCM57XX_BAR_APE,
						  REG_APE_MODE,
						  mode.r32,
						  error))
		return FALSE;

	/* boot */
	mode.bits.Halt = 0;
	mode.bits.FastBoot = 0;
	mode.bits.Reset = 1;
	return fu_bcm57xx_recovery_device_bar_write(self,
						    FU_BCM57XX_BAR_APE,
						    REG_APE_MODE,
						    mode.r32,
						    error);
}

static GBytes *
fu_bcm57xx_recovery_device_dump_firmware(FuDevice *device, FuProgress *progress, GError **error)
{
	FuBcm57xxRecoveryDevice *self = FU_BCM57XX_RECOVERY_DEVICE(device);
	gsize bufsz_dwrds = fu_device_get_firmware_size_max(FU_DEVICE(self)) / sizeof(guint32);
	g_autofree guint32 *buf_dwrds = g_new0(guint32, bufsz_dwrds);
	g_autoptr(FuDeviceLocker) locker = NULL;
	g_autoptr(FuDeviceLocker) locker2 = NULL;

	/* read from hardware */
	fu_progress_set_status(progress, FWUPD_STATUS_DEVICE_READ);
	locker = fu_device_locker_new_full(
	    self,
	    (FuDeviceLockerFunc)fu_bcm57xx_recovery_device_nvram_acquire_lock,
	    (FuDeviceLockerFunc)fu_bcm57xx_recovery_device_nvram_release_lock,
	    error);
	if (locker == NULL)
		return NULL;
	locker2 =
	    fu_device_locker_new_full(self,
				      (FuDeviceLockerFunc)fu_bcm57xx_recovery_device_nvram_enable,
				      (FuDeviceLockerFunc)fu_bcm57xx_recovery_device_nvram_disable,
				      error);
	if (locker2 == NULL)
		return NULL;
	if (!fu_bcm57xx_recovery_device_nvram_read(self,
						   0x0,
						   buf_dwrds,
						   bufsz_dwrds,
						   progress,
						   error))
		return NULL;
	if (!fu_device_locker_close(locker2, error))
		return NULL;
	return g_bytes_new(buf_dwrds, bufsz_dwrds * sizeof(guint32));
}

static FuFirmware *
fu_bcm57xx_recovery_device_prepare_firmware(FuDevice *device,
					    GInputStream *stream,
					    FuProgress *progress,
					    FuFirmwareParseFlags flags,
					    GError **error)
{
	g_autoptr(FuFirmware) firmware_bin = fu_firmware_new();
	g_autoptr(FuFirmware) firmware_tmp = fu_bcm57xx_firmware_new();

	/* check is a NVRAM backup */
	if (!fu_firmware_parse_stream(firmware_tmp, stream, 0x0, flags, error)) {
		g_prefix_error(error, "failed to parse new firmware: ");
		return NULL;
	}
	if (!fu_bcm57xx_firmware_is_backup(FU_BCM57XX_FIRMWARE(firmware_tmp))) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "can only recover with backup firmware");
		return NULL;
	}
	if (!fu_firmware_parse_stream(firmware_bin, stream, 0x0, flags, error))
		return NULL;
	return g_steal_pointer(&firmware_bin);
}

static gboolean
fu_bcm57xx_recovery_device_write_firmware(FuDevice *device,
					  FuFirmware *firmware,
					  FuProgress *progress,
					  FwupdInstallFlags flags,
					  GError **error)
{
	FuBcm57xxRecoveryDevice *self = FU_BCM57XX_RECOVERY_DEVICE(device);
	const guint8 *buf;
	gsize bufsz = 0;
	gsize bufsz_dwrds;
	g_autofree guint32 *buf_dwrds = NULL;
	g_autoptr(FuDeviceLocker) locker = NULL;
	g_autoptr(FuDeviceLocker) locker2 = NULL;
	g_autoptr(GBytes) blob = NULL;

	/* progress */
	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_add_flag(progress, FU_PROGRESS_FLAG_GUESSED);
	fu_progress_add_step(progress, FWUPD_STATUS_DECOMPRESSING, 1, NULL);
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 95, NULL);
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_RESTART, 5, NULL);

	/* build the images into one linear blob of the correct size */
	blob = fu_firmware_write(firmware, error);
	if (blob == NULL)
		return FALSE;
	fu_progress_step_done(progress);

	/* align into uint32_t buffer */
	buf = g_bytes_get_data(blob, &bufsz);
	bufsz_dwrds = bufsz / sizeof(guint32);
	buf_dwrds = g_new0(guint32, bufsz_dwrds);
	if (!fu_memcpy_safe((guint8 *)buf_dwrds,
			    bufsz_dwrds * sizeof(guint32),
			    0x0, /* dst */
			    buf,
			    bufsz,
			    0x0, /* src */
			    bufsz,
			    error))
		return FALSE;

	/* hit hardware */
	locker = fu_device_locker_new_full(
	    self,
	    (FuDeviceLockerFunc)fu_bcm57xx_recovery_device_nvram_acquire_lock,
	    (FuDeviceLockerFunc)fu_bcm57xx_recovery_device_nvram_release_lock,
	    error);
	if (locker == NULL)
		return FALSE;
	locker2 = fu_device_locker_new_full(
	    self,
	    (FuDeviceLockerFunc)fu_bcm57xx_recovery_device_nvram_enable_write,
	    (FuDeviceLockerFunc)fu_bcm57xx_recovery_device_nvram_disable,
	    error);
	if (locker2 == NULL)
		return FALSE;
	if (!fu_bcm57xx_recovery_device_nvram_write(self,
						    0x0,
						    buf_dwrds,
						    bufsz_dwrds,
						    fu_progress_get_child(progress),
						    error))
		return FALSE;
	if (!fu_device_locker_close(locker2, error))
		return FALSE;
	if (!fu_device_locker_close(locker, error))
		return FALSE;
	fu_progress_step_done(progress);

	/* reset APE */
	if (!fu_device_activate(device, fu_progress_get_child(progress), error))
		return FALSE;
	fu_progress_step_done(progress);

	/* success */
	return TRUE;
}

static gboolean
fu_bcm57xx_recovery_device_setup(FuDevice *device, GError **error)
{
	FuBcm57xxRecoveryDevice *self = FU_BCM57XX_RECOVERY_DEVICE(device);
	guint32 fwversion = 0;
	g_autoptr(FuDeviceLocker) locker = NULL;
	g_autoptr(FuDeviceLocker) locker2 = NULL;
	g_autoptr(FuProgress) progress = fu_progress_new(G_STRLOC);

	/* progress */
	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_add_flag(progress, FU_PROGRESS_FLAG_GUESSED);
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 10, "enable");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 80, "nvram");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 10, "veraddr");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 10, "version");

	locker = fu_device_locker_new_full(
	    self,
	    (FuDeviceLockerFunc)fu_bcm57xx_recovery_device_nvram_acquire_lock,
	    (FuDeviceLockerFunc)fu_bcm57xx_recovery_device_nvram_release_lock,
	    error);
	if (locker == NULL)
		return FALSE;
	locker2 =
	    fu_device_locker_new_full(self,
				      (FuDeviceLockerFunc)fu_bcm57xx_recovery_device_nvram_enable,
				      (FuDeviceLockerFunc)fu_bcm57xx_recovery_device_nvram_disable,
				      error);
	if (locker2 == NULL)
		return FALSE;
	fu_progress_step_done(progress);

	/* get NVRAM version */
	if (!fu_bcm57xx_recovery_device_nvram_read(self,
						   BCM_NVRAM_STAGE1_BASE + BCM_NVRAM_STAGE1_VERSION,
						   &fwversion,
						   1,
						   fu_progress_get_child(progress),
						   error))
		return FALSE;
	fu_progress_step_done(progress);
	if (fwversion != 0x0) {
		/* this is only set on the OSS firmware */
		fu_device_set_version_format(device, FWUPD_VERSION_FORMAT_TRIPLET);
		fu_device_set_version_raw(device, GUINT32_FROM_BE(fwversion)); /* nocheck:blocked */
		fu_device_set_branch(device, BCM_FW_BRANCH_OSS_FIRMWARE);
		fu_progress_step_done(progress);
		fu_progress_step_done(progress);
	} else {
		guint32 bufver[4] = {0x0};
		guint32 veraddr = 0;
		g_autoptr(Bcm57xxVeritem) veritem = NULL;

		/* fall back to the string, e.g. '5719-v1.43' */
		if (!fu_bcm57xx_recovery_device_nvram_read(self,
							   BCM_NVRAM_STAGE1_BASE +
							       BCM_NVRAM_STAGE1_VERADDR,
							   &veraddr,
							   1,
							   fu_progress_get_child(progress),
							   error))
			return FALSE;
		fu_progress_step_done(progress);
		veraddr = GUINT32_FROM_BE(veraddr); /* nocheck:blocked */
		if (veraddr > BCM_PHYS_ADDR_DEFAULT)
			veraddr -= BCM_PHYS_ADDR_DEFAULT;
		if (!fu_bcm57xx_recovery_device_nvram_read(self,
							   BCM_NVRAM_STAGE1_BASE + veraddr,
							   bufver,
							   4,
							   fu_progress_get_child(progress),
							   error))
			return FALSE;
		fu_progress_step_done(progress);
		veritem = fu_bcm57xx_veritem_new((guint8 *)bufver, sizeof(bufver));
		if (veritem != NULL) {
			fu_device_set_version(device, veritem->version); /* nocheck:set-version */
			fu_device_set_branch(device, veritem->branch);
			fu_device_set_version_format(device, veritem->verfmt);
		}
	}

	return TRUE;
}

static gboolean
fu_bcm57xx_recovery_device_open(FuDevice *device, GError **error)
{
#ifdef HAVE_MMAN_H
	FuBcm57xxRecoveryDevice *self = FU_BCM57XX_RECOVERY_DEVICE(device);
	FuUdevDevice *udev_device = FU_UDEV_DEVICE(device);
	const gchar *sysfs_path = fu_udev_device_get_sysfs_path(udev_device);
#endif

#ifdef RUNNING_ON_VALGRIND
	/* this can't work */
	if (RUNNING_ON_VALGRIND) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "cannot mmap'ing BARs when using valgrind");
		return FALSE;
	}
#endif

#ifdef HAVE_MMAN_H
	/* map BARs */
	for (guint i = 0; i < FU_BCM57XX_BAR_MAX; i++) {
		int memfd;
		struct stat st;
		g_autofree gchar *fn = NULL;
		g_autofree gchar *resfn = NULL;

		/* open 64 bit resource */
		resfn = g_strdup_printf("resource%u", i * 2);
		fn = g_build_filename(sysfs_path, resfn, NULL);
		memfd = open(fn, O_RDWR | O_SYNC);
		if (memfd < 0) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_FOUND,
				    "error opening %s",
				    fn);
			return FALSE;
		}
		if (fstat(memfd, &st) < 0) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "could not stat %s",
				    fn);
			close(memfd);
			return FALSE;
		}

		/* mmap */
		g_debug("mapping BAR[%u] %s for 0x%x bytes", i, fn, (guint)st.st_size);
		self->bar[i].buf =
		    (guint8 *)mmap(0, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
		self->bar[i].bufsz = st.st_size;
		close(memfd);
		if (self->bar[i].buf == MAP_FAILED) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "could not mmap %s: %s",
				    fn,
				    fwupd_strerror(errno));
			return FALSE;
		}
	}

	/* success */
	return TRUE;
#else
	g_set_error_literal(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "mmap() not supported as sys/mman.h not available");
	return FALSE;
#endif
}

static gboolean
fu_bcm57xx_recovery_device_close(FuDevice *device, GError **error)
{
#ifdef HAVE_MMAN_H
	FuBcm57xxRecoveryDevice *self = FU_BCM57XX_RECOVERY_DEVICE(device);

	/* unmap BARs */
	for (guint i = 0; i < FU_BCM57XX_BAR_MAX; i++) {
		if (self->bar[i].buf == NULL)
			continue;
		g_debug("unmapping BAR[%u]", i);
		munmap(self->bar[i].buf, self->bar[i].bufsz);
		self->bar[i].buf = NULL;
		self->bar[i].bufsz = 0;
	}

	/* success */
	return TRUE;
#else
	g_set_error_literal(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "munmap() not supported as sys/mman.h not available");
	return FALSE;
#endif
}

static void
fu_bcm57xx_recovery_device_set_progress(FuDevice *self, FuProgress *progress)
{
	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_add_flag(progress, FU_PROGRESS_FLAG_GUESSED);
	fu_progress_add_step(progress, FWUPD_STATUS_DECOMPRESSING, 0, "prepare-fw");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_RESTART, 2, "detach");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 94, "write");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_RESTART, 2, "attach");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 2, "reload");
}

static gchar *
fu_bcm57xx_recovery_device_convert_version(FuDevice *device, guint64 version_raw)
{
	return fu_version_from_uint32(version_raw, fu_device_get_version_format(device));
}

static void
fu_bcm57xx_recovery_device_init(FuBcm57xxRecoveryDevice *self)
{
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_CAN_VERIFY_IMAGE);
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_NEEDS_REBOOT);
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_BACKUP_BEFORE_INSTALL);
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_UNSIGNED_PAYLOAD);
	fu_device_add_protocol(FU_DEVICE(self), "com.broadcom.bcm57xx");
	fu_device_add_icon(FU_DEVICE(self), FU_DEVICE_ICON_NETWORK_WIRED);
	fu_device_set_logical_id(FU_DEVICE(self), "recovery");

	/* other values are set from a quirk */
	fu_device_set_firmware_size(FU_DEVICE(self), BCM_FIRMWARE_SIZE);

	/* no BARs mapped */
	for (guint i = 0; i < FU_BCM57XX_BAR_MAX; i++) {
		self->bar[i].buf = NULL;
		self->bar[i].bufsz = 0;
	}
}

static gboolean
fu_bcm57xx_recovery_device_probe(FuDevice *device, GError **error)
{
	return fu_udev_device_set_physical_id(FU_UDEV_DEVICE(device), "pci", error);
}

static void
fu_bcm57xx_recovery_device_class_init(FuBcm57xxRecoveryDeviceClass *klass)
{
	FuDeviceClass *device_class = FU_DEVICE_CLASS(klass);
	device_class->activate = fu_bcm57xx_recovery_device_activate;
	device_class->prepare_firmware = fu_bcm57xx_recovery_device_prepare_firmware;
	device_class->setup = fu_bcm57xx_recovery_device_setup;
	device_class->reload = fu_bcm57xx_recovery_device_setup;
	device_class->open = fu_bcm57xx_recovery_device_open;
	device_class->close = fu_bcm57xx_recovery_device_close;
	device_class->write_firmware = fu_bcm57xx_recovery_device_write_firmware;
	device_class->dump_firmware = fu_bcm57xx_recovery_device_dump_firmware;
	device_class->attach = fu_bcm57xx_recovery_device_attach;
	device_class->detach = fu_bcm57xx_recovery_device_detach;
	device_class->probe = fu_bcm57xx_recovery_device_probe;
	device_class->set_progress = fu_bcm57xx_recovery_device_set_progress;
	device_class->convert_version = fu_bcm57xx_recovery_device_convert_version;
}
