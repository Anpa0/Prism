/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PRISM_SYSINFO_H
#define PRISM_SYSINFO_H

#include "PrismCapture.h"

/* Fills `out` from /sys/bus/pci, /dev/dri and the environment. Prism.exe cannot
 * see any of this itself: under Wine it only knows about DXGI adapters, which
 * carry no PCI address, no kernel driver and no PCIe link state. */
void prism_sysinfo_query(PrismSystemInfo* out);

#endif /* PRISM_SYSINFO_H */
