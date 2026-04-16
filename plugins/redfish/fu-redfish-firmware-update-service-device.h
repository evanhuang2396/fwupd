/*
 * Copyright 2024 NVIDIA Corporation
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <fwupdplugin.h>

#include "fu-redfish-backend.h"
#include "fu-redfish-device.h"

#define FU_TYPE_REDFISH_FIRMWARE_UPDATE_SERVICE_DEVICE                                             \
	(fu_redfish_firmware_update_service_device_get_type())
G_DECLARE_FINAL_TYPE(FuRedfishFirmwareUpdateServiceDevice,
		     fu_redfish_firmware_update_service_device,
		     FU,
		     REDFISH_FIRMWARE_UPDATE_SERVICE_DEVICE,
		     FuRedfishDevice)

FuRedfishFirmwareUpdateServiceDevice *
fu_redfish_firmware_update_service_device_new(FuContext *ctx, FuRedfishBackend *backend);
