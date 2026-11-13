#ifndef HW_VFIO_APPLE_PM_CONTROLLER_H
#define HW_VFIO_APPLE_PM_CONTROLLER_H

#include "hw/vfio/apple.h"

struct AppleVFIOPM;
typedef bool AppleVFIODMACheck(VFIOApplePCIDevice *dev, Error **errp);

/* The caller owns the device and stops the controller before unrealizing it. */
struct AppleVFIOPM *apple_vfio_pm_start(VFIOApplePCIDevice *dev,
                                      const char *icd_socket,
                                      const char *guest_chardev,
                                      AppleVFIODMACheck *verify_dma,
                                      Error **errp);
void apple_vfio_pm_stop(struct AppleVFIOPM *pm);

#endif
