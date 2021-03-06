/* $Id$ */
/** @file
 * IOMMU - Input/Output Memory Management Unit - AMD implementation.
 */

/*
 * Copyright (C) 2020 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_GROUP LOG_GROUP_DEV_IOMMU
#include <VBox/msi.h>
#include <VBox/vmm/pdmdev.h>
#include <VBox/AssertGuest.h>

#include "VBoxDD.h"
#include <iprt/x86.h>
#include <iprt/string.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
/**
 * @name PCI configuration register offsets.
 * In accordance with the AMD spec.
 * @{
 */
#define IOMMU_PCI_OFF_CAP_HDR                       0x40
#define IOMMU_PCI_OFF_BASE_ADDR_REG_LO              0x44
#define IOMMU_PCI_OFF_BASE_ADDR_REG_HI              0x48
#define IOMMU_PCI_OFF_RANGE_REG                     0x4c
#define IOMMU_PCI_OFF_MISCINFO_REG_0                0x50
#define IOMMU_PCI_OFF_MISCINFO_REG_1                0x54
#define IOMMU_PCI_OFF_MSI_CAP_HDR                   0x64
#define IOMMU_PCI_OFF_MSI_ADDR_LO                   0x68
#define IOMMU_PCI_OFF_MSI_ADDR_HI                   0x6c
#define IOMMU_PCI_OFF_MSI_DATA                      0x70
#define IOMMU_PCI_OFF_MSI_MAP_CAP_HDR               0x74
/** @} */

/**
 * @name MMIO register offsets.
 * In accordance with the AMD spec.
 * @{
 */
#define IOMMU_MMIO_OFF_DEV_TAB_BAR                  0x00
#define IOMMU_MMIO_OFF_CMD_BUF_BAR                  0x08
#define IOMMU_MMIO_OFF_EVT_LOG_BAR                  0x10
#define IOMMU_MMIO_OFF_CTRL                         0x18
#define IOMMU_MMIO_OFF_EXCL_BAR                     0x20
#define IOMMU_MMIO_OFF_EXCL_RANGE_LIMIT             0x28
#define IOMMU_MMIO_OFF_EXT_FEAT                     0x30

#define IOMMU_MMIO_OFF_PPR_LOG_BAR                  0x38
#define IOMMU_MMIO_OFF_HW_EVT_HI                    0x40
#define IOMMU_MMIO_OFF_HW_EVT_LO                    0x48
#define IOMMU_MMIO_OFF_HW_EVT_STATUS                0x50

#define IOMMU_MMIO_OFF_SMI_FLT_FIRST                0x60
#define IOMMU_MMIO_OFF_SMI_FLT_LAST                 0xd8

#define IOMMU_MMIO_OFF_GALOG_BAR                    0xe0
#define IOMMU_MMIO_OFF_GALOG_TAIL_ADDR              0xe8

#define IOMMU_MMIO_OFF_PPR_LOG_B_BAR                0xf0
#define IOMMU_MMIO_OFF_PPR_EVT_B_BAR                0xf8

#define IOMMU_MMIO_OFF_DEV_TAB_SEG_FIRST            0x100
#define IOMMU_MMIO_OFF_DEV_TAB_SEG_1                0x100
#define IOMMU_MMIO_OFF_DEV_TAB_SEG_2                0x108
#define IOMMU_MMIO_OFF_DEV_TAB_SEG_3                0x110
#define IOMMU_MMIO_OFF_DEV_TAB_SEG_4                0x118
#define IOMMU_MMIO_OFF_DEV_TAB_SEG_5                0x120
#define IOMMU_MMIO_OFF_DEV_TAB_SEG_6                0x128
#define IOMMU_MMIO_OFF_DEV_TAB_SEG_7                0x130
#define IOMMU_MMIO_OFF_DEV_TAB_SEG_LAST             0x130

#define IOMMU_MMIO_OFF_DEV_SPECIFIC_FEAT            0x138
#define IOMMU_MMIO_OFF_DEV_SPECIFIC_CTRL            0x140
#define IOMMU_MMIO_OFF_DEV_SPECIFIC_STATUS          0x148

#define IOMMU_MMIO_OFF_MSI_VECTOR_0                 0x150
#define IOMMU_MMIO_OFF_MSI_VECTOR_1                 0x154
#define IOMMU_MMIO_OFF_MSI_CAP_HDR                  0x158
#define IOMMU_MMIO_OFF_MSI_ADDR_LO                  0x15c
#define IOMMU_MMIO_OFF_MSI_ADDR_HI                  0x160
#define IOMMU_MMIO_OFF_MSI_DATA                     0x164
#define IOMMU_MMIO_OFF_MSI_MAPPING_CAP_HDR          0x168

#define IOMMU_MMIO_OFF_PERF_OPT_CTRL                0x16c

#define IOMMU_MMIO_OFF_XT_GEN_INTR_CTRL             0x170
#define IOMMU_MMIO_OFF_XT_PPR_INTR_CTRL             0x178
#define IOMMU_MMIO_OFF_XT_GALOG_INT_CTRL            0x180

#define IOMMU_MMIO_OFF_MARC_APER_BAR_0              0x200
#define IOMMU_MMIO_OFF_MARC_APER_RELOC_0            0x208
#define IOMMU_MMIO_OFF_MARC_APER_LEN_0              0x210
#define IOMMU_MMIO_OFF_MARC_APER_BAR_1              0x218
#define IOMMU_MMIO_OFF_MARC_APER_RELOC_1            0x220
#define IOMMU_MMIO_OFF_MARC_APER_LEN_1              0x228
#define IOMMU_MMIO_OFF_MARC_APER_BAR_2              0x230
#define IOMMU_MMIO_OFF_MARC_APER_RELOC_2            0x238
#define IOMMU_MMIO_OFF_MARC_APER_LEN_2              0x240
#define IOMMU_MMIO_OFF_MARC_APER_BAR_3              0x248
#define IOMMU_MMIO_OFF_MARC_APER_RELOC_3            0x250
#define IOMMU_MMIO_OFF_MARC_APER_LEN_3              0x258

#define IOMMU_MMIO_OFF_RSVD_REG                     0x1ff8

#define IOMMU_MMIO_CMD_BUF_HEAD_PTR                 0x2000
#define IOMMU_MMIO_CMD_BUF_TAIL_PTR                 0x2008
#define IOMMU_MMIO_EVT_LOG_HEAD_PTR                 0x2010
#define IOMMU_MMIO_EVT_LOG_TAIL_PTR                 0x2018

#define IOMMU_MMIO_OFF_STATUS                       0x2020

#define IOMMU_MMIO_OFF_PPR_LOG_HEAD_PTR             0x2030
#define IOMMU_MMIO_OFF_PPR_LOG_TAIL_PTR             0x2038

#define IOMMU_MMIO_OFF_GALOG_HEAD_PTR               0x2040
#define IOMMU_MMIO_OFF_GALOG_TAIL_PTR               0x2048

#define IOMMU_MMIO_OFF_PPR_LOG_B_HEAD_PTR           0x2050
#define IOMMU_MMIO_OFF_PPR_LOG_B_TAIL_PTR           0x2058

#define IOMMU_MMIO_OFF_EVT_LOG_B_HEAD_PTR           0x2070
#define IOMMU_MMIO_OFF_EVT_LOG_B_TAIL_PTR           0x2078

#define IOMMU_MMIO_OFF_PPR_LOG_AUTO_RESP            0x2080
#define IOMMU_MMIO_OFF_PPR_LOG_OVERFLOW_EARLY       0x2088
#define IOMMU_MMIO_OFF_PPR_LOG_B_OVERFLOW_EARLY     0x2090
/** @} */

/**
 * @name MMIO register-access table offsets.
 * Each table [first..last] (both inclusive) represents the range of registers
 * covered by a distinct register-access table. This is done due to arbitrary large
 * gaps in the MMIO register offsets themselves.
 * @{
 */
#define IOMMU_MMIO_OFF_TABLE_0_FIRST               0x00
#define IOMMU_MMIO_OFF_TABLE_0_LAST                0x258

#define IOMMU_MMIO_OFF_TABLE_1_FIRST               0x1ff8
#define IOMMU_MMIO_OFF_TABLE_1_LAST                0x2090
/** @} */

/**
 * @name Commands.
 * In accordance with the AMD spec.
 * @{
 */
#define IOMMU_CMD_COMPLETION_WAIT                   0x01
#define IOMMU_CMD_INV_DEV_TAB_ENTRY                 0x02
#define IOMMU_CMD_INV_IOMMU_PAGES                   0x03
#define IOMMU_CMD_INV_IOTLB_PAGES                   0x04
#define IOMMU_CMD_INV_INTR_TABLE                    0x05
#define IOMMU_CMD_PREFETCH_IOMMU_PAGES              0x06
#define IOMMU_CMD_COMPLETE_PPR_REQ                  0x07
#define IOMMU_CMD_INV_IOMMU_ALL                     0x08
/** @} */

/**
 * @name Event codes.
 * In accordance with the AMD spec.
 * @{
 */
#define IOMMU_EVT_ILLEGAL_DEV_TAB_ENTRY             0x01
#define IOMMU_EVT_IO_PAGE_FAULT                     0x02
#define IOMMU_EVT_DEV_TAB_HW_ERROR                  0x03
#define IOMMU_EVT_PAGE_TAB_HW_ERROR                 0x04
#define IOMMU_EVT_ILLEGAL_CMD_ERROR                 0x05
#define IOMMU_EVT_COMMAND_HW_ERROR                  0x06
#define IOMMU_EVT_IOTLB_INV_TIMEOUT                 0x07
#define IOMMU_EVT_INVALID_DEV_REQ                   0x08
#define IOMMU_EVT_INVALID_PPR_REQ                   0x09
#define IOMMU_EVT_EVENT_COUNTER_ZERO                0x10
#define IOMMU_EVT_GUEST_EVENT_FAULT                 0x11
/** @} */

/**
 * @name IOMMU Capability Header.
 * In accordance with the AMD spec.
 * @{
 */
/** CapId: Capability ID. */
#define IOMMU_BF_CAPHDR_CAP_ID_SHIFT                0
#define IOMMU_BF_CAPHDR_CAP_ID_MASK                 UINT32_C(0x000000ff)
/** CapPtr: Capability Pointer. */
#define IOMMU_BF_CAPHDR_CAP_PTR_SHIFT               8
#define IOMMU_BF_CAPHDR_CAP_PTR_MASK                UINT32_C(0x0000ff00)
/** CapType: Capability Type. */
#define IOMMU_BF_CAPHDR_CAP_TYPE_SHIFT              16
#define IOMMU_BF_CAPHDR_CAP_TYPE_MASK               UINT32_C(0x00070000)
/** CapRev: Capability Revision. */
#define IOMMU_BF_CAPHDR_CAP_REV_SHIFT               19
#define IOMMU_BF_CAPHDR_CAP_REV_MASK                UINT32_C(0x00f80000)
/** IoTlbSup: IO TLB Support. */
#define IOMMU_BF_CAPHDR_IOTLB_SUP_SHIFT             24
#define IOMMU_BF_CAPHDR_IOTLB_SUP_MASK              UINT32_C(0x01000000)
/** HtTunnel: HyperTransport Tunnel translation support. */
#define IOMMU_BF_CAPHDR_HT_TUNNEL_SHIFT             25
#define IOMMU_BF_CAPHDR_HT_TUNNEL_MASK              UINT32_C(0x02000000)
/** NpCache: Not Present table entries Cached. */
#define IOMMU_BF_CAPHDR_NP_CACHE_SHIFT              26
#define IOMMU_BF_CAPHDR_NP_CACHE_MASK               UINT32_C(0x04000000)
/** EFRSup: Extended Feature Register (EFR) Supported. */
#define IOMMU_BF_CAPHDR_EFR_SUP_SHIFT               27
#define IOMMU_BF_CAPHDR_EFR_SUP_MASK                UINT32_C(0x08000000)
/** CapExt: Miscellaneous Information Register Supported . */
#define IOMMU_BF_CAPHDR_CAP_EXT_SHIFT               28
#define IOMMU_BF_CAPHDR_CAP_EXT_MASK                UINT32_C(0x10000000)
/** Bits 31:29 reserved. */
#define IOMMU_BF_CAPHDR_RSVD_29_31_SHIFT            29
#define IOMMU_BF_CAPHDR_RSVD_29_31_MASK             UINT32_C(0xe0000000)
RT_BF_ASSERT_COMPILE_CHECKS(IOMMU_BF_CAPHDR_, UINT32_C(0), UINT32_MAX,
                            (CAP_ID, CAP_PTR, CAP_TYPE, CAP_REV, IOTLB_SUP, HT_TUNNEL, NP_CACHE, EFR_SUP, CAP_EXT, RSVD_29_31));
/** @} */

/**
 * @name IOMMU Base Address Low Register.
 * In accordance with the AMD spec.
 * @{
 */
/** Enable: Enables access to the address specified in the Base Address Register. */
#define IOMMU_BF_BASEADDR_LO_ENABLE_SHIFT           0
#define IOMMU_BF_BASEADDR_LO_ENABLE_MASK            UINT32_C(0x00000001)
/** Bits 13:1 reserved. */
#define IOMMU_BF_BASEADDR_LO_RSVD_1_13_SHIFT        1
#define IOMMU_BF_BASEADDR_LO_RSVD_1_13_MASK         UINT32_C(0x00003ffe)
/** Base Address[31:14]: Low Base address of IOMMU MMIO control registers. */
#define IOMMU_BF_BASEADDR_LO_ADDR_SHIFT             14
#define IOMMU_BF_BASEADDR_LO_ADDR_MASK              UINT32_C(0xffffc000)
RT_BF_ASSERT_COMPILE_CHECKS(IOMMU_BF_BASEADDR_LO_, UINT32_C(0), UINT32_MAX,
                            (ENABLE, RSVD_1_13, ADDR));
/** @} */

/**
 * @name IOMMU Range Register.
 * In accordance with the AMD spec.
 * @{
 */
/** UnitID: HyperTransport Unit ID. */
#define IOMMU_BF_RANGE_UNIT_ID_SHIFT                0
#define IOMMU_BF_RANGE_UNIT_ID_MASK                 UINT32_C(0x0000001f)
/** Bits 6:5 reserved. */
#define IOMMU_BF_RANGE_RSVD_5_6_SHIFT               5
#define IOMMU_BF_RANGE_RSVD_5_6_MASK                UINT32_C(0x00000060)
/** RngValid: Range valid. */
#define IOMMU_BF_RANGE_VALID_SHIFT                  7
#define IOMMU_BF_RANGE_VALID_MASK                   UINT32_C(0x00000080)
/** BusNumber: Device range bus number. */
#define IOMMU_BF_RANGE_BUS_NUMBER_SHIFT             8
#define IOMMU_BF_RANGE_BUS_NUMBER_MASK              UINT32_C(0x0000ff00)
/** First Device. */
#define IOMMU_BF_RANGE_FIRST_DEVICE_SHIFT           16
#define IOMMU_BF_RANGE_FIRST_DEVICE_MASK            UINT32_C(0x00ff0000)
/** Last Device. */
#define IOMMU_BF_RANGE_LAST_DEVICE_SHIFT            24
#define IOMMU_BF_RANGE_LAST_DEVICE_MASK             UINT32_C(0xff000000)
RT_BF_ASSERT_COMPILE_CHECKS(IOMMU_BF_RANGE_, UINT32_C(0), UINT32_MAX,
                            (UNIT_ID, RSVD_5_6, VALID, BUS_NUMBER, FIRST_DEVICE, LAST_DEVICE));
/** @} */

/**
 * @name IOMMU Miscellaneous Information Register 0.
 * In accordance with the AMD spec.
 * @{
 */
/** MsiNum: MSI message number. */
#define IOMMU_BF_MISCINFO_0_MSI_NUM_SHIFT           0
#define IOMMU_BF_MISCINFO_0_MSI_NUM_MASK            UINT32_C(0x0000001f)
/** GvaSize: Guest Virtual Address Size. */
#define IOMMU_BF_MISCINFO_0_GVA_SIZE_SHIFT          5
#define IOMMU_BF_MISCINFO_0_GVA_SIZE_MASK           UINT32_C(0x000000e0)
/** PaSize: Physical Address Size. */
#define IOMMU_BF_MISCINFO_0_PA_SIZE_SHIFT           8
#define IOMMU_BF_MISCINFO_0_PA_SIZE_MASK            UINT32_C(0x00007f00)
/** VaSize: Virtual Address Size. */
#define IOMMU_BF_MISCINFO_0_VA_SIZE_SHIFT           15
#define IOMMU_BF_MISCINFO_0_VA_SIZE_MASK            UINT32_C(0x003f8000)
/** HtAtsResv: HyperTransport ATS Response Address range Reserved. */
#define IOMMU_BF_MISCINFO_0_HT_ATS_RESV_SHIFT       22
#define IOMMU_BF_MISCINFO_0_HT_ATS_RESV_MASK        UINT32_C(0x00400000)
/** Bits 26:23 reserved. */
#define IOMMU_BF_MISCINFO_0_RSVD_23_26_SHIFT        23
#define IOMMU_BF_MISCINFO_0_RSVD_23_26_MASK         UINT32_C(0x07800000)
/** MsiNumPPR: Peripheral Page Request MSI message number. */
#define IOMMU_BF_MISCINFO_0_MSI_NUM_PPR_SHIFT       27
#define IOMMU_BF_MISCINFO_0_MSI_NUM_PPR_MASK        UINT32_C(0xf8000000)
RT_BF_ASSERT_COMPILE_CHECKS(IOMMU_BF_MISCINFO_0_, UINT32_C(0), UINT32_MAX,
                            (MSI_NUM, GVA_SIZE, PA_SIZE, VA_SIZE, HT_ATS_RESV, RSVD_23_26, MSI_NUM_PPR));
/** @} */

/**
 * @name IOMMU Miscellaneous Information Register 1.
 * In accordance with the AMD spec.
 * @{
 */
/** MsiNumGA: MSI message number for guest virtual-APIC log. */
#define IOMMU_BF_MISCINFO_1_MSI_NUM_GA_SHIFT        0
#define IOMMU_BF_MISCINFO_1_MSI_NUM_GA_MASK         UINT32_C(0x0000001f)
/** Bits 31:5 reserved. */
#define IOMMU_BF_MISCINFO_1_RSVD_5_31_SHIFT         5
#define IOMMU_BF_MISCINFO_1_RSVD_5_31_MASK          UINT32_C(0xffffffe0)
RT_BF_ASSERT_COMPILE_CHECKS(IOMMU_BF_MISCINFO_1_, UINT32_C(0), UINT32_MAX,
                            (MSI_NUM_GA, RSVD_5_31));
/** @} */

/**
 * @name MSI Capability Header Register.
 * In accordance with the AMD spec.
 * @{
 */
/** MsiCapId: Capability ID. */
#define IOMMU_BF_MSI_CAP_HDR_CAP_ID_SHIFT           0
#define IOMMU_BF_MSI_CAP_HDR_CAP_ID_MASK            UINT32_C(0x000000ff)
/** MsiCapPtr: Pointer (PCI config offset) to the next capability. */
#define IOMMU_BF_MSI_CAP_HDR_CAP_PTR_SHIFT          8
#define IOMMU_BF_MSI_CAP_HDR_CAP_PTR_MASK           UINT32_C(0x0000ff00)
/** MsiEn: Message Signal Interrupt enable. */
#define IOMMU_BF_MSI_CAP_HDR_EN_SHIFT               16
#define IOMMU_BF_MSI_CAP_HDR_EN_MASK                UINT32_C(0x00010000)
/** MsiMultMessCap: MSI Multi-Message Capability. */
#define IOMMU_BF_MSI_CAP_HDR_MULTMESS_CAP_SHIFT     17
#define IOMMU_BF_MSI_CAP_HDR_MULTMESS_CAP_MASK      UINT32_C(0x000e0000)
/** MsiMultMessEn: MSI Mult-Message Enable. */
#define IOMMU_BF_MSI_CAP_HDR_MULTMESS_EN_SHIFT      20
#define IOMMU_BF_MSI_CAP_HDR_MULTMESS_EN_MASK       UINT32_C(0x00700000)
/** Msi64BitEn: MSI 64-bit Enabled. */
#define IOMMU_BF_MSI_CAP_HDR_64BIT_EN_SHIFT         23
#define IOMMU_BF_MSI_CAP_HDR_64BIT_EN_MASK          UINT32_C(0x00800000)
/** Bits 31:24 reserved. */
#define IOMMU_BF_MSI_CAP_HDR_RSVD_24_31_SHIFT       24
#define IOMMU_BF_MSI_CAP_HDR_RSVD_24_31_MASK        UINT32_C(0xff000000)
RT_BF_ASSERT_COMPILE_CHECKS(IOMMU_BF_MSI_CAP_HDR_, UINT32_C(0), UINT32_MAX,
                            (CAP_ID, CAP_PTR, EN, MULTMESS_CAP, MULTMESS_EN, 64BIT_EN, RSVD_24_31));
/** @} */

/**
 * @name MSI Mapping Capability Header Register.
 * In accordance with the AMD spec.
 * @{
 */
/** MsiMapCapId: Capability ID. */
#define IOMMU_BF_MSI_MAP_CAPHDR_CAP_ID_SHIFT        0
#define IOMMU_BF_MSI_MAP_CAPHDR_CAP_ID_MASK         UINT32_C(0x000000ff)
/** MsiMapCapPtr: Pointer (PCI config offset) to the next capability. */
#define IOMMU_BF_MSI_MAP_CAPHDR_CAP_PTR_SHIFT       8
#define IOMMU_BF_MSI_MAP_CAPHDR_CAP_PTR_MASK        UINT32_C(0x0000ff00)
/** MsiMapEn: MSI mapping capability enable. */
#define IOMMU_BF_MSI_MAP_CAPHDR_EN_SHIFT            16
#define IOMMU_BF_MSI_MAP_CAPHDR_EN_MASK             UINT32_C(0x00010000)
/** MsiMapFixd: MSI interrupt mapping range is not programmable. */
#define IOMMU_BF_MSI_MAP_CAPHDR_FIXED_SHIFT         17
#define IOMMU_BF_MSI_MAP_CAPHDR_FIXED_MASK          UINT32_C(0x00020000)
/** Bits 18:28 reserved. */
#define IOMMU_BF_MSI_MAP_CAPHDR_RSVD_18_28_SHIFT    18
#define IOMMU_BF_MSI_MAP_CAPHDR_RSVD_18_28_MASK     UINT32_C(0x07fc0000)
/** MsiMapCapType: MSI mapping capability. */
#define IOMMU_BF_MSI_MAP_CAPHDR_CAP_TYPE_SHIFT      27
#define IOMMU_BF_MSI_MAP_CAPHDR_CAP_TYPE_MASK       UINT32_C(0xf8000000)
RT_BF_ASSERT_COMPILE_CHECKS(IOMMU_BF_MSI_MAP_CAPHDR_, UINT32_C(0), UINT32_MAX,
                            (CAP_ID, CAP_PTR, EN, FIXED, RSVD_18_28, CAP_TYPE));
/** @} */

/**
 * @name IOMMU Status Register Bits.
 * In accordance with the AMD spec.
 * @{
 */
/** EventOverflow: Event log overflow. */
#define IOMMU_STATUS_EVT_LOG_OVERFLOW               RT_BIT_64(0)
/** EventLogInt: Event log interrupt. */
#define IOMMU_STATUS_EVT_LOG_INTR                   RT_BIT_64(1)
/** ComWaitInt: Completion wait interrupt. */
#define IOMMU_STATUS_COMPLETION_WAIT_INTR           RT_BIT_64(2)
/** EventLogRun: Event log is running. */
#define IOMMU_STATUS_EVT_LOG_RUNNING                RT_BIT_64(3)
/** CmdBufRun: Command buffer is running. */
#define IOMMU_STATUS_CMD_BUF_RUNNING                RT_BIT_64(4)
/** PprOverflow: Peripheral page request log overflow. */
#define IOMMU_STATUS_PPR_LOG_OVERFLOW               RT_BIT_64(5)
/** PprInt: Peripheral page request log interrupt. */
#define IOMMU_STATUS_PPR_LOG_INTR                   RT_BIT_64(6)
/** PprLogRun: Peripheral page request log is running. */
#define IOMMU_STATUS_PPR_LOG_RUN                    RT_BIT_64(7)
/** GALogRun: Guest virtual-APIC log is running. */
#define IOMMU_STATUS_GA_LOG_RUN                     RT_BIT_64(8)
/** GALOverflow: Guest virtual-APIC log overflow. */
#define IOMMU_STATUS_GA_LOG_OVERFLOW                RT_BIT_64(9)
/** GAInt: Guest virtual-APIC log interrupt. */
#define IOMMU_STATUS_GA_LOG_INTR                    RT_BIT_64(10)
/** PprOvrflwB: PPR Log B overflow. */
#define IOMMU_STATUS_PPR_LOG_B_OVERFLOW             RT_BIT_64(11)
/** PprLogActive: PPR Log B is active. */
#define IOMMU_STATUS_PPR_LOG_B_ACTIVE               RT_BIT_64(12)
/** EvtOvrflwB: Event log B overflow. */
#define IOMMU_STATUS_EVT_LOG_B_OVERFLOW             RT_BIT_64(15)
/** EventLogActive: Event log B active. */
#define IOMMU_STATUS_EVT_LOG_B_ACTIVE               RT_BIT_64(16)
/** PprOvrflwEarlyB: PPR log B overflow early warning. */
#define IOMMU_STATUS_PPR_LOG_B_OVERFLOW_EARLY       RT_BIT_64(17)
/** PprOverflowEarly: PPR log overflow early warning. */
#define IOMMU_STATUS_PPR_LOG_OVERFLOW_EARLY         RT_BIT_64(18)
/** @} */

/** @name IOMMU_IO_PERM_XXX: IOMMU I/O access permissions bits.
 * In accordance with the AMD spec.
 *
 * These values match the shifted values of the IR and IW field of the DTE and the
 * PTE, PDE of the I/O page tables.
 *
 * @{ */
#define IOMMU_IO_PERM_NONE                          (0)
#define IOMMU_IO_PERM_READ                          RT_BIT_64(0)
#define IOMMU_IO_PERM_WRITE                         RT_BIT_64(1)
#define IOMMU_IO_PERM_READ_WRITE                    (IOMMU_IO_PERM_READ | IOMMU_IO_PERM_WRITE)
#define IOMMU_IO_PERM_SHIFT                         61
#define IOMMU_IO_PERM_MASK                          0x3
/** @} */

/** @name SYSMGT_TYPE_XXX: System Management Message Enable Types.
 * In accordance with the AMD spec.
 * @{ */
#define SYSMGTTYPE_DMA_DENY                         (0)
#define SYSMGTTYPE_MSG_ALL_ALLOW                    (1)
#define SYSMGTTYPE_MSG_INT_ALLOW                    (2)
#define SYSMGTTYPE_DMA_ALLOW                        (3)
/** @} */

/** @name IOMMU_INTR_CTRL_XX: DTE::IntCtl field values.
 * These are control bits for handling fixed and arbitrated interrupts.
 * In accordance with the AMD spec.
 * @{ */
#define IOMMU_INTR_CTRL_TARGET_ABORT                (0)
#define IOMMU_INTR_CTRL_FWD_UNMAPPED                (1)
#define IOMMU_INTR_CTRL_REMAP                       (2)
#define IOMMU_INTR_CTRL_RSVD                        (3)
/** @} */

/** @name Miscellaneous IOMMU defines.
 * @{ */
/** Log prefix string. */
#define IOMMU_LOG_PFX                               "AMD_IOMMU"
/** The current saved state version. */
#define IOMMU_SAVED_STATE_VERSION                   1
/** AMD's vendor ID. */
#define IOMMU_PCI_VENDOR_ID                         0x1022
/** VirtualBox IOMMU device ID. */
#define IOMMU_PCI_DEVICE_ID                         0xc0de
/** VirtualBox IOMMU device revision ID. */
#define IOMMU_PCI_REVISION_ID                       0x01
/** Size of the MMIO region in bytes. */
#define IOMMU_MMIO_REGION_SIZE                      _16K
/** Number of device table segments supported (power of 2). */
#define IOMMU_MAX_DEV_TAB_SEGMENTS                  3
/** Maximum host address translation level supported (inclusive). */
#define IOMMU_MAX_HOST_PT_LEVEL                     6
/** The IOTLB entry magic. */
#define IOMMU_IOTLBE_MAGIC                          0x10acce55
/** The device-specific feature major revision. */
#define IOMMU_DEVSPEC_FEAT_MAJOR_VERSION            0x1
/** The device-specific feature minor revision. */
#define IOMMU_DEVSPEC_FEAT_MINOR_VERSION            0x0
/** The device-specific control major revision. */
#define IOMMU_DEVSPEC_CTRL_MAJOR_VERSION            0x1
/** The device-specific control minor revision. */
#define IOMMU_DEVSPEC_CTRL_MINOR_VERSION            0x0
/** The device-specific status major revision. */
#define IOMMU_DEVSPEC_STATUS_MAJOR_VERSION          0x1
/** The device-specific status minor revision. */
#define IOMMU_DEVSPEC_STATUS_MINOR_VERSION          0x0
/** @} */

/**
 * Acquires the IOMMU PDM lock.
 * This will make a long jump to ring-3 to acquire the lock if necessary.
 */
#define IOMMU_LOCK(a_pDevIns)  \
    do { \
        int rcLock = PDMDevHlpCritSectEnter((a_pDevIns), (a_pDevIns)->CTX_SUFF(pCritSectRo), VINF_SUCCESS); \
        if (RT_LIKELY(rcLock == VINF_SUCCESS)) \
        { /* likely */ } \
        else \
            return rcLock; \
    } while (0)

/**
 * Acquires the IOMMU PDM lock (asserts on failure rather than returning an error).
 * This will make a long jump to ring-3 to acquire the lock if necessary.
 */
#define IOMMU_LOCK_NORET(a_pDevIns)  \
    do { \
        int rcLock = PDMDevHlpCritSectEnter((a_pDevIns), (a_pDevIns)->CTX_SUFF(pCritSectRo), VINF_SUCCESS); \
        AssertRC(rcLock); \
    } while (0)

/**
 * Releases the IOMMU PDM lock.
 */
#define IOMMU_UNLOCK(a_pDevIns) \
    do { \
        PDMDevHlpCritSectLeave((a_pDevIns), (a_pDevIns)->CTX_SUFF(pCritSectRo)); \
    } while (0)

/**
 * Asserts that the critsect is owned by this thread.
 */
#define IOMMU_ASSERT_LOCKED(a_pDevIns) \
    do { \
        Assert(PDMDevHlpCritSectIsOwner(pDevIns, pDevIns->CTX_SUFF(pCritSectRo))); \
    }  while (0)

/**
 * Asserts that the critsect is not owned by this thread.
 */
#define IOMMU_ASSERT_NOT_LOCKED(a_pDevIns) \
    do { \
        Assert(!PDMDevHlpCritSectIsOwner(pDevIns, pDevIns->CTX_SUFF(pCritSectRo))); \
    }  while (0)

/**
 * Gets the device table size given the size field.
 */
#define IOMMU_GET_DEV_TAB_SIZE(a_uSize)     (((a_uSize) + 1) << X86_PAGE_4K_SHIFT)


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/
/**
 * The Device ID.
 * In accordance with VirtualBox's PCI configuration.
 */
typedef union
{
    struct
    {
        uint16_t    u3Function : 3;  /**< Bits 2:0   - Function. */
        uint16_t    u9Device : 9;    /**< Bits 11:3  - Device. */
        uint16_t    u4Bus : 4;       /**< Bits 15:12 - Bus. */
    } n;
    /** The unsigned integer view. */
    uint16_t        u;
} DEVICE_ID_T;
AssertCompileSize(DEVICE_ID_T, 2);

/**
 * Device Table Entry (DTE).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        RT_GCC_EXTENSION uint64_t  u1Valid : 1;                   /**< Bit  0       - V: Valid. */
        RT_GCC_EXTENSION uint64_t  u1TranslationValid : 1;        /**< Bit  1       - TV: Translation information Valid. */
        RT_GCC_EXTENSION uint64_t  u5Rsvd0 : 5;                   /**< Bits 6:2     - Reserved. */
        RT_GCC_EXTENSION uint64_t  u2Had : 2;                     /**< Bits 8:7     - HAD: Host Access Dirty. */
        RT_GCC_EXTENSION uint64_t  u3Mode : 3;                    /**< Bits 11:9    - Mode: Paging mode. */
        RT_GCC_EXTENSION uint64_t  u40PageTableRootPtrLo : 40;    /**< Bits 51:12   - Page Table Root Pointer. */
        RT_GCC_EXTENSION uint64_t  u1Ppr : 1;                     /**< Bit  52      - PPR: Peripheral Page Request. */
        RT_GCC_EXTENSION uint64_t  u1GstPprRespPasid : 1;         /**< Bit  53      - GRPR: Guest PPR Response with PASID. */
        RT_GCC_EXTENSION uint64_t  u1GstIoValid : 1;              /**< Bit  54      - GIoV: Guest I/O Protection Valid. */
        RT_GCC_EXTENSION uint64_t  u1GstTranslateValid : 1;       /**< Bit  55      - GV: Guest translation Valid. */
        RT_GCC_EXTENSION uint64_t  u2GstMode : 2;                 /**< Bits 57:56   - GLX: Guest Paging mode levels. */
        RT_GCC_EXTENSION uint64_t  u3GstCr3TableRootPtrLo : 2;    /**< Bits 60:58   - GCR3 TRP: Guest CR3 Table Root Ptr (Lo). */
        RT_GCC_EXTENSION uint64_t  u1IoRead : 1;                  /**< Bit  61      - IR: I/O Read permission. */
        RT_GCC_EXTENSION uint64_t  u1IoWrite : 1;                 /**< Bit  62      - IW: I/O Write permission. */
        RT_GCC_EXTENSION uint64_t  u1Rsvd0 : 1;                   /**< Bit  63      - Reserved. */
        RT_GCC_EXTENSION uint64_t  u16DomainId : 1;               /**< Bits 79:64   - Domain ID. */
        RT_GCC_EXTENSION uint64_t  u16GstCr3TableRootPtrMed : 16; /**< Bits 95:80   - GCR3 TRP: Guest CR3 Table Root Ptr (Mid). */
        RT_GCC_EXTENSION uint64_t  u1IoTlbEnable : 1;             /**< Bit  96      - I: IOTLB Enable. */
        RT_GCC_EXTENSION uint64_t  u1SuppressPfEvents : 1;        /**< Bit  97      - SE: Supress Page-fault events. */
        RT_GCC_EXTENSION uint64_t  u1SuppressAllPfEvents : 1;     /**< Bit  98      - SA: Supress All Page-fault events. */
        RT_GCC_EXTENSION uint64_t  u2IoCtl : 1;                   /**< Bits 100:99  - IoCtl: Port I/O Control. */
        RT_GCC_EXTENSION uint64_t  u1Cache : 1;                   /**< Bit  101     - Cache: IOTLB Cache Hint. */
        RT_GCC_EXTENSION uint64_t  u1SnoopDisable : 1;            /**< Bit  102     - SD: Snoop Disable. */
        RT_GCC_EXTENSION uint64_t  u1AllowExclusion : 1;          /**< Bit  103     - EX: Allow Exclusion. */
        RT_GCC_EXTENSION uint64_t  u2SysMgt : 2;                  /**< Bits 105:104 - SysMgt: System Management message enable. */
        RT_GCC_EXTENSION uint64_t  u1Rsvd1 : 1;                   /**< Bit  106     - Reserved. */
        RT_GCC_EXTENSION uint64_t  u21GstCr3TableRootPtrHi : 21;  /**< Bits 127:107 - GCR3 TRP: Guest CR3 Table Root Ptr (Hi). */
        RT_GCC_EXTENSION uint64_t  u1IntrMapValid : 1;            /**< Bit  128     - IV: Interrupt map Valid. */
        RT_GCC_EXTENSION uint64_t  u4IntrTableLength : 4;         /**< Bits 132:129 - IntTabLen: Interrupt Table Length. */
        RT_GCC_EXTENSION uint64_t  u1IgnoreUnmappedIntrs : 1;     /**< Bits 133     - IG: Ignore unmapped interrupts. */
        RT_GCC_EXTENSION uint64_t  u26IntrTableRootPtr : 26;      /**< Bits 159:134 - Interrupt Root Table Pointer (Lo). */
        RT_GCC_EXTENSION uint64_t  u20IntrTableRootPtr : 20;      /**< Bits 179:160 - Interrupt Root Table Pointer (Hi). */
        RT_GCC_EXTENSION uint64_t  u4Rsvd0 : 4;                   /**< Bits 183:180 - Reserved. */
        RT_GCC_EXTENSION uint64_t  u1InitPassthru : 1;            /**< Bits 184     - INIT Pass-through. */
        RT_GCC_EXTENSION uint64_t  u1ExtIntPassthru : 1;          /**< Bits 185     - External Interrupt Pass-through. */
        RT_GCC_EXTENSION uint64_t  u1NmiPassthru : 1;             /**< Bits 186     - NMI Pass-through. */
        RT_GCC_EXTENSION uint64_t  u1Rsvd2 : 1;                   /**< Bits 187     - Reserved. */
        RT_GCC_EXTENSION uint64_t  u2IntrCtrl : 2;                /**< Bits 189:188 - IntCtl: Interrupt Control. */
        RT_GCC_EXTENSION uint64_t  u1Lint0Passthru : 1;           /**< Bit  190     - Lint0Pass: LINT0 Pass-through. */
        RT_GCC_EXTENSION uint64_t  u1Lint1Passthru : 1;           /**< Bit  191     - Lint1Pass: LINT1 Pass-through. */
        RT_GCC_EXTENSION uint64_t  u32Rsvd0 : 32;                 /**< Bits 223:192 - Reserved. */
        RT_GCC_EXTENSION uint64_t  u22Rsvd0 : 22;                 /**< Bits 245:224 - Reserved. */
        RT_GCC_EXTENSION uint64_t  u1AttrOverride : 1;            /**< Bit  246     - AttrV: Attribute Override. */
        RT_GCC_EXTENSION uint64_t  u1Mode0FC: 1;                  /**< Bit  247     - Mode0FC. */
        RT_GCC_EXTENSION uint64_t  u8SnoopAttr: 1;                /**< Bits 255:248 - Snoop Attribute. */
    } n;
    /** The 32-bit unsigned integer view. */
    uint32_t        au32[8];
    /** The 64-bit unsigned integer view. */
    uint64_t        au64[4];
} DTE_T;
AssertCompileSize(DTE_T, 32);
/** Pointer to a device table entry. */
typedef DTE_T *PDTE_T;
/** Pointer to a const device table entry. */
typedef DTE_T const *PCDTE_T;

/** Mask of valid  bits for EPHSUP (Enhanced Peripheral Page Request Handling
 *  Support) feature (bits 52:53). */
#define IOMMU_DTE_QWORD_0_FEAT_EPHSUP_MASK      UINT64_C(0x0030000000000000)

/** Mask of valid bits for GTSup (Guest Translation Support) feature (bits 55:60,
 *  bits 80:95). */
#define IOMMU_DTE_QWORD_0_FEAT_GTSUP_MASK       UINT64_C(0x1f80000000000000)
#define IOMMU_DTE_QWORD_1_FEAT_GTSUP_MASK       UINT64_C(0x00000000ffff0000)

/* Mask of valid bits for GIoSup (Guest I/O Protection Support) feature (bit 54). */
#define IOMMU_DTE_QWORD_0_FEAT_GIOSUP_MASK      UINT64_C(0x0040000000000000)

/* Mask of valid DTE feature bits. */
#define IOMMU_DTE_QWORD_0_FEAT_MASK             (  IOMMU_DTE_QWORD_0_FEAT_EPHSUP_MASK \
                                                 | IOMMU_DTE_QWORD_0_FEAT_GTSUP_MASK  \
                                                 | IOMMU_DTE_QWORD_0_FEAT_GIOSUP_MASK)
#define IOMMU_DTE_QWORD_1_FEAT_MASK             (IOMMU_DTE_QWORD_0_FEAT_GIOSUP_MASK)

/* Mask of all valid DTE bits (including all feature bits). */
#define IOMMU_DTE_QWORD_0_VALID_MASK            UINT64_C(0x7fffffffffffff83)
#define IOMMU_DTE_QWORD_1_VALID_MASK            UINT64_C(0xfffffbffffffffff)
#define IOMMU_DTE_QWORD_2_VALID_MASK            UINT64_C(0xf70fffffffffffff)
#define IOMMU_DTE_QWORD_3_VALID_MASK            UINT64_C(0xffc0000000000000)

/* Mask of the interrupt table root pointer. */
#define IOMMU_DTE_IRTE_ROOT_PTR_MASK            UINT64_C(0x000fffffffffff80)

/**
 * I/O Page Translation Entry.
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        RT_GCC_EXTENSION uint64_t    u1Present : 1;             /**< Bit  0      - PR: Present. */
        RT_GCC_EXTENSION uint64_t    u4Ign0 : 4;                /**< Bits 4:1    - Ignored. */
        RT_GCC_EXTENSION uint64_t    u1Accessed : 1;            /**< Bit  5      - A: Accessed. */
        RT_GCC_EXTENSION uint64_t    u1Dirty : 1;               /**< Bit  6      - D: Dirty. */
        RT_GCC_EXTENSION uint64_t    u2Ign0 : 2;                /**< Bits 8:7    - Ignored. */
        RT_GCC_EXTENSION uint64_t    u3NextLevel : 3;           /**< Bits 11:9   - Next Level: Next page translation level. */
        RT_GCC_EXTENSION uint64_t    u40PageAddr : 40;          /**< Bits 51:12  - Page address. */
        RT_GCC_EXTENSION uint64_t    u7Rsvd0 : 7;               /**< Bits 58:52  - Reserved. */
        RT_GCC_EXTENSION uint64_t    u1UntranslatedAccess : 1;  /**< Bit 59      - U: Untranslated Access Only. */
        RT_GCC_EXTENSION uint64_t    u1ForceCoherent : 1;       /**< Bit 60      - FC: Force Coherent. */
        RT_GCC_EXTENSION uint64_t    u1IoRead : 1;              /**< Bit 61      - IR: I/O Read permission. */
        RT_GCC_EXTENSION uint64_t    u1IoWrite : 1;             /**< Bit 62      - IW: I/O Wead permission. */
        RT_GCC_EXTENSION uint64_t    u1Ign0 : 1;                /**< Bit 63      - Ignored. */
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t        u64;
} IOPTE_T;
AssertCompileSize(IOPTE_T, 8);

/**
 * I/O Page Directory Entry.
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        RT_GCC_EXTENSION uint64_t    u1Present : 1;         /**< Bit  0      - PR: Present. */
        RT_GCC_EXTENSION uint64_t    u4Ign0 : 4;            /**< Bits 4:1    - Ignored. */
        RT_GCC_EXTENSION uint64_t    u1Accessed : 1;        /**< Bit  5      - A: Accessed. */
        RT_GCC_EXTENSION uint64_t    u3Ign0 : 3;            /**< Bits 8:6    - Ignored. */
        RT_GCC_EXTENSION uint64_t    u3NextLevel : 3;       /**< Bits 11:9   - Next Level: Next page translation level. */
        RT_GCC_EXTENSION uint64_t    u40PageAddr : 40;      /**< Bits 51:12  - Page address (Next Table Address). */
        RT_GCC_EXTENSION uint64_t    u9Rsvd0 : 9;           /**< Bits 60:52  - Reserved. */
        RT_GCC_EXTENSION uint64_t    u1IoRead : 1;          /**< Bit 61      - IR: I/O Read permission. */
        RT_GCC_EXTENSION uint64_t    u1IoWrite : 1;         /**< Bit 62      - IW: I/O Wead permission. */
        RT_GCC_EXTENSION uint64_t    u1Ign0 : 1;            /**< Bit 63      - Ignored. */
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t        u64;
} IOPDE_T;
AssertCompileSize(IOPDE_T, 8);

/**
 * I/O Page Table Entity.
 * In accordance with the AMD spec.
 *
 * This a common subset of an DTE.au64[0], PTE and PDE.
 * Named as an "entity" to avoid confusing it with PTE.
 */
typedef union
{
    struct
    {
        RT_GCC_EXTENSION uint64_t    u1Present : 1;         /**< Bit  0      - PR: Present. */
        RT_GCC_EXTENSION uint64_t    u8Ign0 : 8;            /**< Bits 8:1    - Ignored. */
        RT_GCC_EXTENSION uint64_t    u3NextLevel : 3;       /**< Bits 11:9   - Mode / Next Level: Next page translation level. */
        RT_GCC_EXTENSION uint64_t    u40Addr : 40;          /**< Bits 51:12  - Page address. */
        RT_GCC_EXTENSION uint64_t    u9Ign0 : 9;            /**< Bits 60:52  - Ignored. */
        RT_GCC_EXTENSION uint64_t    u1IoRead : 1;          /**< Bit 61      - IR: I/O Read permission. */
        RT_GCC_EXTENSION uint64_t    u1IoWrite : 1;         /**< Bit 62      - IW: I/O Wead permission. */
        RT_GCC_EXTENSION uint64_t    u1Ign0 : 1;            /**< Bit 63      - Ignored. */
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t        u64;
} IOPTENTITY_T;
AssertCompileSize(IOPTENTITY_T, 8);
AssertCompile(sizeof(IOPTENTITY_T) == sizeof(IOPTE_T));
AssertCompile(sizeof(IOPTENTITY_T) == sizeof(IOPDE_T));
/** Pointer to an IOPT_ENTITY_T struct. */
typedef IOPTENTITY_T *PIOPTENTITY_T;
/** Pointer to a const IOPT_ENTITY_T struct. */
typedef IOPTENTITY_T const *PCIOPTENTITY_T;
/** Mask of the address field. */
#define IOMMU_PTENTITY_ADDR_MASK     UINT64_C(0x000ffffffffff000)

/**
 * Interrupt Remapping Table Entry (IRTE).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    u1RemapEnable : 1;      /**< Bit  0     - RemapEn: Remap Enable. */
        uint32_t    u1SuppressPf : 1;       /**< Bit  1     - SupIOPF: Supress I/O Page Fault. */
        uint32_t    u3IntrType : 1;         /**< Bits 4:2   - IntType: Interrupt Type. */
        uint32_t    u1ReqEoi : 1;           /**< Bit  5     - RqEoi: Request EOI. */
        uint32_t    u1DestMode : 1;         /**< Bit  6     - DM: Destination Mode. */
        uint32_t    u1GuestMode : 1;        /**< Bit  7     - GuestMode. */
        uint32_t    u8Dest : 8;             /**< Bits 15:8  - Destination. */
        uint32_t    u8Vector : 8;           /**< Bits 23:16 - Vector. */
        uint32_t    u8Rsvd0 : 8;            /**< Bits 31:24 - Reserved. */
    } n;
    /** The 32-bit unsigned integer view. */
    uint32_t        u32;
} IRTE_T;
AssertCompileSize(IRTE_T, 4);
/** The number of bits to shift the IRTE offset to get the IRTE. */
#define IOMMU_IRTE_SIZE_SHIFT   (2)
/** Pointer to an IRTE_T struct. */
typedef IRTE_T *PIRTE_T;
/** Pointer to a const IRTE_T struct. */
typedef IRTE_T const *PCIRTE_T;

/** The IRTE offset corresponds directly to bits 10:0 of the originating MSI
 *  interrupt message. See AMD IOMMU spec. 2.2.5 "Interrupt Remapping Tables". */
#define IOMMU_MSI_DATA_IRTE_OFFSET_MASK     UINT32_C(0x000007ff)

/**
 * Command: Generic Command Buffer Entry.
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    u32Operand1Lo;          /**< Bits 31:0   - Operand 1 (Lo). */
        uint32_t    u32Operand1Hi : 28;     /**< Bits 59:32  - Operand 1 (Hi). */
        uint32_t    u4Opcode : 4;           /**< Bits 63:60  - Op Code. */
        uint64_t    u64Operand2;            /**< Bits 127:64 - Operand 2. */
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t    au64[2];
} CMD_GENERIC_T;
AssertCompileSize(CMD_GENERIC_T, 16);
/** Pointer to a generic command buffer entry. */
typedef CMD_GENERIC_T *PCMD_GENERIC_T;
/** Pointer to a const generic command buffer entry. */
typedef CMD_GENERIC_T const *PCCMD_GENERIC_T;

/** Number of bits to shift the byte offset of a command in the command buffer to
 *  get its index. */
#define IOMMU_CMD_GENERIC_SHIFT   4

/**
 * Command: COMPLETION_WAIT.
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    u1Store : 1;           /**< Bit  0      - S: Completion Store. */
        uint32_t    u1Interrupt : 1;       /**< Bit  1      - I: Completion Interrupt. */
        uint32_t    u1Flush : 1;           /**< Bit  2      - F: Flush Queue. */
        uint32_t    u29StoreAddrLo : 29;   /**< Bits 31:3   - Store Address (Lo). */
        uint32_t    u20StoreAddrHi : 20;   /**< Bits 51:32  - Store Address (Hi). */
        uint32_t    u8Rsvd0 : 8;           /**< Bits 59:52  - Reserved. */
        uint32_t    u4OpCode : 4;          /**< Bits 63:60  - OpCode (Command). */
        uint64_t    u64StoreData;          /**< Bits 127:64 - Store Data. */
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t    au64[2];
} CMD_COMWAIT_T;
AssertCompileSize(CMD_COMWAIT_T, 16);
/** Pointer to a completion wait command. */
typedef CMD_COMWAIT_T *PCMD_COMWAIT_T;
/** Pointer to a const completion wait command. */
typedef CMD_COMWAIT_T const *PCCMD_COMWAIT_T;
#define IOMMU_CMD_COM_WAIT_QWORD_0_VALID_MASK        UINT64_C(0xf00fffffffffffff)

/**
 * Command: INVALIDATE_DEVTAB_ENTRY.
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint16_t                     u16DevId;          /**< Bits 15:0   - Device ID. */
        uint16_t                     u16Rsvd0;          /**< Bits 31:16  - Reserved. */
        uint32_t                     u28Rsvd0 : 28;     /**< Bits 59:32  - Reserved. */
        uint32_t                     u4OpCode : 4;      /**< Bits 63:60  - Op Code (Command). */
        uint64_t                     u64Rsvd0;          /**< Bits 127:64 - Reserved. */
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t    au64[2];
} CMD_INV_DTE_T;
AssertCompileSize(CMD_INV_DTE_T, 16);

/**
 * Command: INVALIDATE_IOMMU_PAGES.
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    u20Pasid : 20;          /**< Bits 19:0   - PASID: Process Address-Space ID. */
        uint32_t    u12Rsvd0 : 12;          /**< Bits 31:20  - Reserved. */
        uint32_t    u16DomainId : 16;       /**< Bits 47:32  - Domain ID. */
        uint32_t    u12Rsvd1 : 12;          /**< Bits 59:48  - Reserved. */
        uint32_t    u4OpCode : 4;           /**< Bits 63:60  - Op Code (Command). */
        uint32_t    u1Size : 1;             /**< Bit  64     - S: Size. */
        uint32_t    u1PageDirEntries : 1;   /**< Bit  65     - PDE: Page Directory Entries. */
        uint32_t    u1GuestOrNested : 1;    /**< Bit  66     - GN: Guest (GPA) or Nested (GVA). */
        uint32_t    u9Rsvd0 : 9;            /**< Bits 75:67  - Reserved. */
        uint32_t    u20AddrLo : 20;         /**< Bits 95:76  - Address (Lo). */
        uint32_t    u32AddrHi;              /**< Bits 127:96 - Address (Hi). */
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t    au64[2];
} CMD_INV_IOMMU_PAGES_T;
AssertCompileSize(CMD_INV_IOMMU_PAGES_T, 16);

/**
 * Command: INVALIDATE_IOTLB_PAGES.
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint16_t    u16DevId;               /**< Bits 15:0   - Device ID. */
        uint8_t     u8PasidLo;              /**< Bits 23:16  - PASID: Process Address-Space ID (Lo). */
        uint8_t     u8MaxPend;              /**< Bits 31:24  - Maxpend: Maximum simultaneous in-flight transactions. */
        uint32_t    u16QueueId : 16;        /**< Bits 47:32  - Queue ID. */
        uint32_t    u12PasidHi : 12;        /**< Bits 59:48  - PASID: Process Address-Space ID (Hi). */
        uint32_t    u4OpCode : 4;           /**< Bits 63:60  - Op Code (Command). */
        uint32_t    u1Size : 1;             /**< Bit  64     - S: Size. */
        uint32_t    u1Rsvd0: 1;             /**< Bit  65     - Reserved. */
        uint32_t    u1GuestOrNested : 1;    /**< Bit  66     - GN: Guest (GPA) or Nested (GVA). */
        uint32_t    u1Rsvd1 : 1;            /**< Bit  67     - Reserved. */
        uint32_t    u2Type : 2;             /**< Bit  69:68  - Type. */
        uint32_t    u6Rsvd0 : 6;            /**< Bits 75:70  - Reserved. */
        uint32_t    u20AddrLo : 20;         /**< Bits 95:76  - Address (Lo). */
        uint32_t    u32AddrHi;              /**< Bits 127:96 - Address (Hi). */
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t    au64[2];
} CMD_INV_IOTLB_PAGES_T;
AssertCompileSize(CMD_INV_IOTLB_PAGES_T, 16);

/**
 * Command: INVALIDATE_INTR_TABLE.
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint16_t    u16DevId;           /**< Bits 15:0   - Device ID. */
        uint16_t    u16Rsvd0;           /**< Bits 31:16  - Reserved. */
        uint32_t    u32Rsvd0 : 28;      /**< Bits 59:32  - Reserved. */
        uint32_t    u4OpCode : 4;       /**< Bits 63:60  - Op Code (Command). */
        uint64_t    u64Rsvd0;           /**< Bits 127:64 - Reserved. */
    } u;
    /** The 64-bit unsigned integer view. */
    uint64_t    au64[2];
} CMD_INV_INTR_TABLE_T;
AssertCompileSize(CMD_INV_INTR_TABLE_T, 16);

/**
 * Command: COMPLETE_PPR_REQ.
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint16_t    u16DevId;               /**< Bits 15:0    - Device ID. */
        uint16_t    u16Rsvd0;               /**< Bits 31:16   - Reserved. */
        uint32_t    u20Pasid : 20;          /**< Bits 51:32   - PASID: Process Address-Space ID. */
        uint32_t    u8Rsvd0 : 8;            /**< Bits 59:52   - Reserved. */
        uint32_t    u4OpCode : 4;           /**< Bits 63:60   - Op Code (Command). */
        uint32_t    u2Rsvd0 : 2;            /**< Bits 65:64   - Reserved. */
        uint32_t    u1GuestOrNested : 1;    /**< Bit  66      - GN: Guest (GPA) or Nested (GVA). */
        uint32_t    u29Rsvd0 : 29;          /**< Bits 95:67   - Reserved. */
        uint32_t    u16CompletionTag : 16;  /**< Bits 111:96  - Completion Tag. */
        uint32_t    u16Rsvd1 : 16;          /**< Bits 127:112 - Reserved. */
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t    au64[2];
} CMD_COMPLETE_PPR_REQ_T;
AssertCompileSize(CMD_COMPLETE_PPR_REQ_T, 16);

/**
 * Command: INV_IOMMU_ALL.
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    u32Rsvd0;           /**< Bits 31:0   - Reserved. */
        uint32_t    u28Rsvd0 : 28;      /**< Bits 59:32  - Reserved. */
        uint32_t    u4OpCode : 4;       /**< Bits 63:60  - Op Code (Command). */
        uint64_t    u64Rsvd0;           /**< Bits 127:64 - Reserved. */
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t    au64[2];
} CMD_IOMMU_ALL_T;
AssertCompileSize(CMD_IOMMU_ALL_T, 16);

/**
 * Event Log Entry: Generic.
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    u32Operand1Lo;          /**< Bits 31:0   - Operand 1 (Lo). */
        uint32_t    u32Operand1Hi : 28;     /**< Bits 59:32  - Operand 1 (Hi). */
        uint32_t    u4EvtCode : 4;          /**< Bits 63:60  - Event code. */
        uint32_t    u32Operand2Lo;          /**< Bits 95:64  - Operand 2 (Lo). */
        uint32_t    u32Operand2Hi;          /**< Bits 127:96 - Operand 2 (Hi). */
    } n;
    /** The 32-bit unsigned integer view.  */
    uint32_t    au32[4];
} EVT_GENERIC_T;
AssertCompileSize(EVT_GENERIC_T, 16);
/** Number of bits to shift the byte offset of an event entry in the event log
 *  buffer to get its index. */
#define IOMMU_EVT_GENERIC_SHIFT   4
/** Pointer to a generic event log entry. */
typedef EVT_GENERIC_T *PEVT_GENERIC_T;
/** Pointer to a const generic event log entry. */
typedef const EVT_GENERIC_T *PCEVT_GENERIC_T;

/**
 * Hardware event types.
 * In accordance with the AMD spec.
 */
typedef enum HWEVTTYPE
{
    HWEVTTYPE_RSVD = 0,
    HWEVTTYPE_MASTER_ABORT,
    HWEVTTYPE_TARGET_ABORT,
    HWEVTTYPE_DATA_ERROR
} HWEVTTYPE;
AssertCompileSize(HWEVTTYPE, 4);

/**
 * Event Log Entry: ILLEGAL_DEV_TABLE_ENTRY.
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint16_t    u16DevId;               /**< Bits 15:0   - Device ID. */
        uint16_t    u4PasidHi : 4;          /**< Bits 19:16  - PASID: Process Address-Space ID (Hi). */
        uint16_t    u12Rsvd0 : 12;          /**< Bits 31:20  - Reserved. */
        uint16_t    u16PasidLo;             /**< Bits 47:32  - PASID: Process Address-Space ID (Lo). */
        uint16_t    u1GuestOrNested : 1;    /**< Bit  48     - GN: Guest (GPA) or Nested (GVA). */
        uint16_t    u2Rsvd0 : 2;            /**< Bits 50:49  - Reserved. */
        uint16_t    u1Interrupt : 1;        /**< Bit  51     - I: Interrupt. */
        uint16_t    u1Rsvd0 : 1;            /**< Bit  52     - Reserved. */
        uint16_t    u1ReadWrite : 1;        /**< Bit  53     - RW: Read/Write. */
        uint16_t    u1Rsvd1 : 1;            /**< Bit  54     - Reserved. */
        uint16_t    u1RsvdNotZero : 1;      /**< Bit  55     - RZ: Reserved bit not Zero (0=invalid level encoding). */
        uint16_t    u1Translation : 1;      /**< Bit  56     - TN: Translation. */
        uint16_t    u3Rsvd0 : 3;            /**< Bits 59:57  - Reserved. */
        uint16_t    u4EvtCode : 4;          /**< Bits 63:60  - Event code. */
        uint64_t    u64Addr;                /**< Bits 127:64 - Address: I/O Virtual Address (IOVA). */
    } n;
    /** The 32-bit unsigned integer view.  */
    uint32_t    au32[4];
    /** The 64-bit unsigned integer view. */
    uint64_t    au64[2];
} EVT_ILLEGAL_DTE_T;
AssertCompileSize(EVT_ILLEGAL_DTE_T, 16);
/** Pointer to an illegal device table entry event. */
typedef EVT_ILLEGAL_DTE_T *PEVT_ILLEGAL_DTE_T;
/** Pointer to a const illegal device table entry event. */
typedef EVT_ILLEGAL_DTE_T const *PCEVT_ILLEGAL_DTE_T;

/**
 * Event Log Entry: IO_PAGE_FAULT_EVENT.
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint16_t    u16DevId;               /**< Bits 15:0   - Device ID. */
        uint16_t    u4PasidHi : 4;          /**< Bits 19:16  - PASID: Process Address-Space ID (Hi). */
        uint16_t    u16DomainOrPasidLo;     /**< Bits 47:32  - D/P: Domain ID or Process Address-Space ID (Lo). */
        uint16_t    u1GuestOrNested : 1;    /**< Bit  48     - GN: Guest (GPA) or Nested (GVA). */
        uint16_t    u1NoExecute : 1;        /**< Bit  49     - NX: No Execute. */
        uint16_t    u1User : 1;             /**< Bit  50     - US: User/Supervisor. */
        uint16_t    u1Interrupt : 1;        /**< Bit  51     - I: Interrupt. */
        uint16_t    u1Present : 1;          /**< Bit  52     - PR: Present. */
        uint16_t    u1ReadWrite : 1;        /**< Bit  53     - RW: Read/Write. */
        uint16_t    u1PermDenied : 1;       /**< Bit  54     - PE: Permission Indicator. */
        uint16_t    u1RsvdNotZero : 1;      /**< Bit  55     - RZ: Reserved bit not Zero (0=invalid level encoding). */
        uint16_t    u1Translation : 1;      /**< Bit  56     - TN: Translation. */
        uint16_t    u3Rsvd0 : 3;            /**< Bit  59:57  - Reserved. */
        uint16_t    u4EvtCode : 4;          /**< Bits 63:60  - Event code. */
        uint64_t    u64Addr;                /**< Bits 127:64 - Address: I/O Virtual Address (IOVA). */
    } n;
    /** The 32-bit unsigned integer view.  */
    uint32_t    au32[4];
    /** The 64-bit unsigned integer view. */
    uint64_t    au64[2];
} EVT_IO_PAGE_FAULT_T;
AssertCompileSize(EVT_IO_PAGE_FAULT_T, 16);
/** Pointer to an I/O page fault event. */
typedef EVT_IO_PAGE_FAULT_T *PEVT_IO_PAGE_FAULT_T;
/** Pointer to a const I/O page fault event. */
typedef EVT_IO_PAGE_FAULT_T const *PCEVT_IO_PAGE_FAULT_T;


/**
 * Event Log Entry: DEV_TAB_HARDWARE_ERROR.
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint16_t    u16DevId;               /**< Bits 15:0   - Device ID. */
        uint16_t    u16Rsvd0;               /**< Bits 31:16  - Reserved. */
        uint32_t    u19Rsvd0 : 19;          /**< Bits 50:32  - Reserved. */
        uint32_t    u1Intr : 1;             /**< Bit  51     - I: Interrupt (1=interrupt request, 0=memory request). */
        uint32_t    u1Rsvd0 : 1;            /**< Bit  52     - Reserved. */
        uint32_t    u1ReadWrite : 1;        /**< Bit  53     - RW: Read/Write transaction (only meaninful when I=0 and TR=0). */
        uint32_t    u2Rsvd0 : 2;            /**< Bits 55:54  - Reserved. */
        uint32_t    u1Translation : 1;      /**< Bit  56     - TR: Translation (1=translation, 0=transaction). */
        uint32_t    u2Type : 2;             /**< Bits 58:57  - Type: The type of hardware error. */
        uint32_t    u1Rsvd1 : 1;            /**< Bit  59     - Reserved. */
        uint32_t    u4EvtCode : 4;          /**< Bits 63:60  - Event code. */
        uint64_t    u64Addr;                /**< Bits 127:64 - Address. */
    } n;
    /** The 32-bit unsigned integer view.  */
    uint32_t    au32[4];
    /** The 64-bit unsigned integer view. */
    uint64_t    au64[2];
} EVT_DEV_TAB_HW_ERROR_T;
AssertCompileSize(EVT_DEV_TAB_HW_ERROR_T, 16);
/** Pointer to a device table hardware error event. */
typedef EVT_DEV_TAB_HW_ERROR_T *PEVT_DEV_TAB_HW_ERROR_T;
/** Pointer to a const device table hardware error event. */
typedef EVT_DEV_TAB_HW_ERROR_T const *PCEVT_DEV_TAB_HW_ERROR_T;

/**
 * Event Log Entry: EVT_PAGE_TAB_HARDWARE_ERROR.
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint16_t    u16DevId;                   /**< Bits 15:0   - Device ID. */
        uint16_t    u16Rsvd0;                   /**< Bits 31:16  - Reserved. */
        uint32_t    u16DomainOrPasidLo : 16;    /**< Bits 47:32  - D/P: Domain ID or Process Address-Space ID (Lo). */
        uint32_t    u1GuestOrNested : 1;        /**< Bit  48     - GN: Guest (GPA) or Nested (GVA). */
        uint32_t    u2Rsvd0 : 2;                /**< Bits 50:49  - Reserved. */
        uint32_t    u1Interrupt : 1;            /**< Bit  51     - I: Interrupt. */
        uint32_t    u1Rsvd0 : 1;                /**< Bit  52     - Reserved. */
        uint32_t    u1ReadWrite : 1;            /**< Bit  53     - RW: Read/Write. */
        uint32_t    u2Rsvd1 : 2;                /**< Bit  55:54  - Reserved. */
        uint32_t    u1Translation : 1;          /**< Bit  56     - TR: Translation. */
        uint32_t    u2Type : 2;                 /**< Bits 58:57  - Type: The type of hardware error. */
        uint32_t    u1Rsvd1 : 1;                /**< Bit  59     - Reserved. */
        uint32_t    u4EvtCode : 4;              /**< Bit  63:60  - Event code. */
        /** @todo r=ramshankar: Figure 55: PAGE_TAB_HARDWARE_ERROR says Addr[31:3] but
         *        table 58 mentions Addr[31:4], we just use the full 64-bits. Looks like a
         *        typo in the figure.See AMD AMD IOMMU spec (3.05-PUB, Jan 2020). */
        uint64_t    u64Addr;                    /** Bits 127:64  - Address: SPA of the page table entry. */
    } n;
    /** The 32-bit unsigned integer view. */
    uint32_t    au32[4];
    /** The 64-bit unsigned integer view. */
    uint64_t    au64[2];
} EVT_PAGE_TAB_HW_ERR_T;
AssertCompileSize(EVT_PAGE_TAB_HW_ERR_T, 16);
/** Pointer to a page table hardware error event. */
typedef EVT_PAGE_TAB_HW_ERR_T *PEVT_PAGE_TAB_HW_ERR_T;
/** Pointer to a const page table hardware error event. */
typedef EVT_PAGE_TAB_HW_ERR_T const *PCEVT_PAGE_TAB_HW_ERR_T;

/**
 * Event Log Entry: ILLEGAL_COMMAND_ERROR.
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    u32Rsvd0;           /**< Bits 31:0   - Reserved. */
        uint32_t    u28Rsvd0 : 28;      /**< Bits 47:32  - Reserved. */
        uint32_t    u4EvtCode : 4;      /**< Bits 63:60  - Event code. */
        uint64_t    u64Addr;            /**< Bits 127:64 - Address: SPA of the invalid command. */
    } n;
    /** The 32-bit unsigned integer view. */
    uint32_t    au32[4];
    /** The 64-bit unsigned integer view. */
    uint64_t    au64[2];
} EVT_ILLEGAL_CMD_ERR_T;
AssertCompileSize(EVT_ILLEGAL_CMD_ERR_T, 16);
/** Pointer to an illegal command error event. */
typedef EVT_ILLEGAL_CMD_ERR_T *PEVT_ILLEGAL_CMD_ERR_T;
/** Pointer to a const illegal command error event. */
typedef EVT_ILLEGAL_CMD_ERR_T const *PCEVT_ILLEGAL_CMD_ERR_T;

/**
 * Event Log Entry: COMMAND_HARDWARE_ERROR.
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    u32Rsvd0;           /**< Bits 31:0   - Reserved. */
        uint32_t    u25Rsvd1 : 25;      /**< Bits 56:32  - Reserved. */
        uint32_t    u2Type : 2;         /**< Bits 58:57  - Type: The type of hardware error. */
        uint32_t    u1Rsvd1 : 1;        /**< Bit  59     - Reserved. */
        uint32_t    u4EvtCode : 4;      /**< Bits 63:60  - Event code. */
        uint64_t    u64Addr;            /**< Bits 128:64 - Address: SPA of the attempted access. */
    } n;
    /** The 32-bit unsigned integer view. */
    uint32_t    au32[4];
    /** The 64-bit unsigned integer view. */
    uint64_t    au64[2];
} EVT_CMD_HW_ERR_T;
AssertCompileSize(EVT_CMD_HW_ERR_T, 16);
/** Pointer to a command hardware error event. */
typedef EVT_CMD_HW_ERR_T *PEVT_CMD_HW_ERR_T;
/** Pointer to a const command hardware error event. */
typedef EVT_CMD_HW_ERR_T const *PCEVT_CMD_HW_ERR_T;

/**
 * Event Log Entry: IOTLB_INV_TIMEOUT.
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint16_t    u16DevId;           /**< Bits 15:0   - Device ID. */
        uint16_t    u16Rsvd0;           /**< Bits 31:16  - Reserved.*/
        uint32_t    u28Rsvd0 : 28;      /**< Bits 59:32  - Reserved. */
        uint32_t    u4EvtCode : 4;      /**< Bits 63:60  - Event code. */
        uint32_t    u4Rsvd0 : 4;        /**< Bits 67:64  - Reserved. */
        uint32_t    u28AddrLo : 28;     /**< Bits 95:68  - Address: SPA of the invalidation command that timedout (Lo). */
        uint32_t    u32AddrHi;          /**< Bits 127:96 - Address: SPA of the invalidation command that timedout (Hi). */
    } n;
    /** The 32-bit unsigned integer view. */
    uint32_t    au32[4];
} EVT_IOTLB_INV_TIMEOUT_T;
AssertCompileSize(EVT_IOTLB_INV_TIMEOUT_T, 16);

/**
 * Event Log Entry: INVALID_DEVICE_REQUEST.
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    u16DevId : 16;          /***< Bits 15:0   - Device ID. */
        uint32_t    u4PasidHi : 4;          /***< Bits 19:16  - PASID: Process Address-Space ID (Hi). */
        uint32_t    u12Rsvd0 : 12;          /***< Bits 31:20  - Reserved. */
        uint32_t    u16PasidLo : 16;        /***< Bits 47:32  - PASID: Process Address-Space ID (Lo). */
        uint32_t    u1GuestOrNested : 1;    /***< Bit  48     - GN: Guest (GPA) or Nested (GVA). */
        uint32_t    u1User : 1;             /***< Bit  49     - US: User/Supervisor. */
        uint32_t    u6Rsvd0 : 6;            /***< Bits 55:50  - Reserved. */
        uint32_t    u1Translation: 1;       /***< Bit  56     - TR: Translation. */
        uint32_t    u3Type: 3;              /***< Bits 59:57  - Type: The type of hardware error. */
        uint32_t    u4EvtCode : 4;          /***< Bits 63:60  - Event code. */
        uint64_t    u64Addr;                /***< Bits 127:64 - Address: Translation or access address. */
    } n;
    /** The 32-bit unsigned integer view. */
    uint32_t    au32[4];
} EVT_INVALID_DEV_REQ_T;
AssertCompileSize(EVT_INVALID_DEV_REQ_T, 16);

/**
 * Event Log Entry: EVENT_COUNTER_ZERO.
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    u32Rsvd0;               /**< Bits 31:0   - Reserved. */
        uint32_t    u28Rsvd0 : 28;          /**< Bits 59:32  - Reserved. */
        uint32_t    u4EvtCode : 4;          /**< Bits 63:60  - Event code. */
        uint32_t    u20CounterNoteHi : 20;  /**< Bits 83:64  - CounterNote: Counter value for the event counter register (Hi). */
        uint32_t    u12Rsvd0 : 12;          /**< Bits 95:84  - Reserved. */
        uint32_t    u32CounterNoteLo;       /**< Bits 127:96 - CounterNote: Counter value for the event cuonter register (Lo). */
    } n;
    /** The 32-bit unsigned integer view. */
    uint32_t    au32[4];
} EVT_EVENT_COUNTER_ZERO_T;
AssertCompileSize(EVT_EVENT_COUNTER_ZERO_T, 16);

/**
 * IOMMU Capability Header (PCI).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    u8CapId : 8;        /**< Bits 7:0   - CapId: Capability ID. */
        uint32_t    u8CapPtr : 8;       /**< Bits 15:8  - CapPtr: Pointer (PCI config offset) to the next capability. */
        uint32_t    u3CapType : 3;      /**< Bits 18:16 - CapType: Capability Type. */
        uint32_t    u5CapRev : 5;       /**< Bits 23:19 - CapRev: Capability revision. */
        uint32_t    u1IoTlbSup : 1;     /**< Bit  24    - IotlbSup: IOTLB Support. */
        uint32_t    u1HtTunnel : 1;     /**< Bit  25    - HtTunnel: HyperTransport Tunnel translation support. */
        uint32_t    u1NpCache : 1;      /**< Bit  26    - NpCache: Not Present table entries are cached. */
        uint32_t    u1EfrSup : 1;       /**< Bit  27    - EFRSup: Extended Feature Register Support. */
        uint32_t    u1CapExt : 1;       /**< Bit  28    - CapExt: Misc. Information Register 1 Support. */
        uint32_t    u3Rsvd0 : 3;        /**< Bits 31:29 - Reserved. */
    } n;
    /** The 32-bit unsigned integer view. */
    uint32_t    u32;
} IOMMU_CAP_HDR_T;
AssertCompileSize(IOMMU_CAP_HDR_T, 4);

/**
 * IOMMU Base Address (Lo and Hi) Register (PCI).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t   u1Enable : 1;       /**< Bit  1     - Enable: RW1S - Enable IOMMU MMIO region. */
        uint32_t   u12Rsvd0 : 12;      /**< Bits 13:1  - Reserved. */
        uint32_t   u18BaseAddrLo : 18; /**< Bits 31:14 - Base address (Lo) of the MMIO region. */
        uint32_t   u32BaseAddrHi;      /**< Bits 63:32 - Base address (Hi) of the MMIO region. */
    } n;
    /** The 32-bit unsigned integer view. */
    uint32_t    au32[2];
    /** The 64-bit unsigned integer view. */
    uint64_t    u64;
} IOMMU_BAR_T;
AssertCompileSize(IOMMU_BAR_T, 8);
#define IOMMU_BAR_VALID_MASK        UINT64_C(0xffffffffffffc001)

/**
 * IOMMU Range Register (PCI).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    u5HtUnitId : 5;     /**< Bits 4:0   - UnitID: IOMMU HyperTransport Unit ID (not used). */
        uint32_t    u2Rsvd0 : 2;        /**< Bits 6:5   - Reserved. */
        uint32_t    u1RangeValid : 1;   /**< Bit  7     - RngValid: Range Valid. */
        uint32_t    u8Bus : 8;          /**< Bits 15:8  - BusNumber: Bus number of the first and last device. */
        uint32_t    u8FirstDevice : 8;  /**< Bits 23:16 - FirstDevice: Device and function number of the first device. */
        uint32_t    u8LastDevice: 8;    /**< Bits 31:24 - LastDevice: Device and function number of the last device. */
    } n;
    /** The 32-bit unsigned integer view. */
    uint32_t    u32;
} IOMMU_RANGE_T;
AssertCompileSize(IOMMU_RANGE_T, 4);

/**
 * Device Table Base Address Register (MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        RT_GCC_EXTENSION uint64_t   u9Size : 9;     /**< Bits 8:0   - Size: Size of the device table. */
        RT_GCC_EXTENSION uint64_t   u3Rsvd0 : 3;    /**< Bits 11:9  - Reserved. */
        RT_GCC_EXTENSION uint64_t   u40Base : 40;   /**< Bits 51:12 - DevTabBase: Device table base address. */
        RT_GCC_EXTENSION uint64_t   u12Rsvd0 : 12;  /**< Bits 63:52 - Reserved. */
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t    u64;
} DEV_TAB_BAR_T;
AssertCompileSize(DEV_TAB_BAR_T, 8);
#define IOMMU_DEV_TAB_BAR_VALID_MASK          UINT64_C(0x000ffffffffff1ff)
#define IOMMU_DEV_TAB_SEG_BAR_VALID_MASK      UINT64_C(0x000ffffffffff0ff)

/**
 * Command Buffer Base Address Register (MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        RT_GCC_EXTENSION uint64_t   u12Rsvd0 : 12;      /**< Bits 11:0  - Reserved. */
        RT_GCC_EXTENSION uint64_t   u40Base : 40;       /**< Bits 51:12 - ComBase: Command buffer base address. */
        RT_GCC_EXTENSION uint64_t   u4Rsvd0 : 4;        /**< Bits 55:52 - Reserved. */
        RT_GCC_EXTENSION uint64_t   u4Len : 4;          /**< Bits 59:56 - ComLen: Command buffer length. */
        RT_GCC_EXTENSION uint64_t   u4Rsvd1 : 4;        /**< Bits 63:60 - Reserved. */
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t    u64;
} CMD_BUF_BAR_T;
AssertCompileSize(CMD_BUF_BAR_T, 8);
#define IOMMU_CMD_BUF_BAR_VALID_MASK      UINT64_C(0x0f0ffffffffff000)

/**
 * Event Log Base Address Register (MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        RT_GCC_EXTENSION uint64_t   u12Rsvd0 : 12;      /**< Bits 11:0  - Reserved. */
        RT_GCC_EXTENSION uint64_t   u40Base : 40;       /**< Bits 51:12 - EventBase: Event log base address. */
        RT_GCC_EXTENSION uint64_t   u4Rsvd0 : 4;        /**< Bits 55:52 - Reserved. */
        RT_GCC_EXTENSION uint64_t   u4Len : 4;          /**< Bits 59:56 - EventLen: Event log length. */
        RT_GCC_EXTENSION uint64_t   u4Rsvd1 : 4;        /**< Bits 63:60 - Reserved. */
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t    u64;
} EVT_LOG_BAR_T;
AssertCompileSize(EVT_LOG_BAR_T, 8);
#define IOMMU_EVT_LOG_BAR_VALID_MASK      UINT64_C(0x0f0ffffffffff000)

/**
 * IOMMU Control Register (MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    u1IommuEn : 1;               /**< Bit  0     - IommuEn: IOMMU Enable. */
        uint32_t    u1HtTunEn : 1;               /**< Bit  1     - HtTunEn: HyperTransport Tunnel Enable. */
        uint32_t    u1EvtLogEn : 1;              /**< Bit  2     - EventLogEn: Event Log Enable. */
        uint32_t    u1EvtIntrEn : 1;             /**< Bit  3     - EventIntEn: Event Log Interrupt Enable. */
        uint32_t    u1CompWaitIntrEn : 1;        /**< Bit  4     - ComWaitIntEn: Completion Wait Interrupt Enable. */
        uint32_t    u3InvTimeOut : 3;            /**< Bits 7:5   - InvTimeOut: Invalidation Timeout. */
        uint32_t    u1PassPW : 1;                /**< Bit  8     - PassPW: Pass Posted Write. */
        uint32_t    u1ResPassPW : 1;             /**< Bit  9     - ResPassPW: Response Pass Posted Write. */
        uint32_t    u1Coherent : 1;              /**< Bit  10    - Coherent: HT read request packet Coherent bit. */
        uint32_t    u1Isoc : 1;                  /**< Bit  11    - Isoc: HT read request packet Isochronous bit. */
        uint32_t    u1CmdBufEn : 1;              /**< Bit  12    - CmdBufEn: Command Buffer Enable. */
        uint32_t    u1PprLogEn : 1;              /**< Bit  13    - PprLogEn: Peripheral Page Request (PPR) Log Enable. */
        uint32_t    u1PprIntrEn : 1;             /**< Bit  14    - PprIntrEn: Peripheral Page Request Interrupt Enable. */
        uint32_t    u1PprEn : 1;                 /**< Bit  15    - PprEn: Peripheral Page Request processing Enable. */
        uint32_t    u1GstTranslateEn : 1;        /**< Bit  16    - GTEn: Guest Translate Enable. */
        uint32_t    u1GstVirtApicEn : 1;         /**< Bit  17    - GAEn: Guest Virtual-APIC Enable. */
        uint32_t    u4Crw : 1;                   /**< Bits 21:18 - CRW: Intended for future use (not documented). */
        uint32_t    u1SmiFilterEn : 1;           /**< Bit  22    - SmiFEn: SMI Filter Enable. */
        uint32_t    u1SelfWriteBackDis : 1;      /**< Bit  23    - SlfWBDis: Self Write-Back Disable. */
        uint32_t    u1SmiFilterLogEn : 1;        /**< Bit  24    - SmiFLogEn: SMI Filter Log Enable. */
        uint32_t    u3GstVirtApicModeEn : 3;     /**< Bits 27:25 - GAMEn: Guest Virtual-APIC Mode Enable. */
        uint32_t    u1GstLogEn : 1;              /**< Bit  28    - GALogEn: Guest Virtual-APIC GA Log Enable. */
        uint32_t    u1GstIntrEn : 1;             /**< Bit  29    - GAIntEn: Guest Virtual-APIC Interrupt Enable. */
        uint32_t    u2DualPprLogEn : 2;          /**< Bits 31:30 - DualPprLogEn: Dual Peripheral Page Request Log Enable. */
        uint32_t    u2DualEvtLogEn : 2;          /**< Bits 33:32 - DualEventLogEn: Dual Event Log Enable. */
        uint32_t    u3DevTabSegEn : 3;           /**< Bits 36:34 - DevTblSegEn: Device Table Segment Enable. */
        uint32_t    u2PrivAbortEn : 2;           /**< Bits 38:37 - PrivAbrtEn: Privilege Abort Enable. */
        uint32_t    u1PprAutoRespEn : 1;         /**< Bit  39    - PprAutoRspEn: Peripheral Page Request Auto Response Enable. */
        uint32_t    u1MarcEn : 1;                /**< Bit  40    - MarcEn: Memory Address Routing and Control Enable. */
        uint32_t    u1BlockStopMarkEn : 1;       /**< Bit  41    - BlkStopMarkEn: Block StopMark messages Enable. */
        uint32_t    u1PprAutoRespAlwaysOnEn : 1; /**< Bit  42    - PprAutoRspAon:: PPR Auto Response - Always On Enable. */
        uint32_t    u1DomainIDPNE : 1;           /**< Bit  43    - DomainIDPE: Reserved (not documented). */
        uint32_t    u1Rsvd0 : 1;                 /**< Bit  44    - Reserved. */
        uint32_t    u1EnhancedPpr : 1;           /**< Bit  45    - EPHEn: Enhanced Peripheral Page Request Handling Enable. */
        uint32_t    u2HstAccDirtyBitUpdate : 2;  /**< Bits 47:46 - HADUpdate: Access and Dirty Bit updated in host page table. */
        uint32_t    u1GstDirtyUpdateDis : 1;     /**< Bit  48    - GDUpdateDis: Disable hardare update of Dirty bit in GPT. */
        uint32_t    u1Rsvd1 : 1;                 /**< Bit  49    - Reserved. */
        uint32_t    u1X2ApicEn : 1;              /**< Bit  50    - XTEn: Enable X2APIC. */
        uint32_t    u1X2ApicIntrGenEn : 1;       /**< Bit  51    - IntCapXTEn: Enable IOMMU X2APIC Interrupt generation. */
        uint32_t    u2Rsvd0 : 2;                 /**< Bits 53:52 - Reserved. */
        uint32_t    u1GstAccessUpdateDis : 1;    /**< Bit  54    - GAUpdateDis: Disable hardare update of Access bit in GPT. */
        uint32_t    u8Rsvd0 : 8;                 /**< Bits 63:55 - Reserved. */
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t    u64;
} IOMMU_CTRL_T;
AssertCompileSize(IOMMU_CTRL_T, 8);
#define IOMMU_CTRL_VALID_MASK           UINT64_C(0x004defffffffffff)
#define IOMMU_CTRL_CMD_BUF_EN_MASK      UINT64_C(0x0000000000001001)

/**
 * IOMMU Exclusion Base Register (MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        RT_GCC_EXTENSION uint64_t   u1ExclEnable : 1;       /**< Bit 0      - ExEn: Exclusion Range Enable. */
        RT_GCC_EXTENSION uint64_t   u1AllowAll : 1;         /**< Bit 1      - Allow: Allow All Devices. */
        RT_GCC_EXTENSION uint64_t   u10Rsvd0 : 10;          /**< Bits 11:2  - Reserved. */
        RT_GCC_EXTENSION uint64_t   u40ExclRangeBase : 40;  /**< Bits 51:12 - Exclusion Range Base Address. */
        RT_GCC_EXTENSION uint64_t   u12Rsvd0 : 12;          /**< Bits 63:52 - Reserved. */
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t    u64;
} IOMMU_EXCL_RANGE_BAR_T;
AssertCompileSize(IOMMU_EXCL_RANGE_BAR_T, 8);
#define IOMMU_EXCL_RANGE_BAR_VALID_MASK     UINT64_C(0x000ffffffffff003)

/**
 * IOMMU Exclusion Range Limit Register (MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        RT_GCC_EXTENSION uint64_t   u52ExclLimit : 52;  /**< Bits 51:0 - Exclusion Range Limit (last 12 bits are treated as 1s). */
        RT_GCC_EXTENSION uint64_t   u12Rsvd1 : 12;      /**< Bits 63:52 - Reserved. */
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t    u64;
} IOMMU_EXCL_RANGE_LIMIT_T;
AssertCompileSize(IOMMU_EXCL_RANGE_LIMIT_T, 8);
#define IOMMU_EXCL_RANGE_LIMIT_VALID_MASK   UINT64_C(0x000fffffffffffff)

/**
 * IOMMU Extended Feature Register (MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    u1PrefetchSup : 1;            /**< Bit  0     - PreFSup: Prefetch Support. */
        uint32_t    u1PprSup : 1;                 /**< Bit  1     - PPRSup: Peripheral Page Request Support. */
        uint32_t    u1X2ApicSup : 1;              /**< Bit  2     - XTSup: x2Apic Support. */
        uint32_t    u1NoExecuteSup : 1;           /**< Bit  3     - NXSup: No-Execute and Privilege Level Support. */
        uint32_t    u1GstTranslateSup : 1;        /**< Bit  4     - GTSup: Guest Translations (for GVAs) Support. */
        uint32_t    u1Rsvd0 : 1;                  /**< Bit  5     - Reserved. */
        uint32_t    u1InvAllSup : 1;              /**< Bit  6     - IASup: Invalidate-All Support. */
        uint32_t    u1GstVirtApicSup : 1;         /**< Bit  7     - GASup: Guest Virtual-APIC Support. */
        uint32_t    u1HwErrorSup : 1;             /**< Bit  8     - HESup: Hardware Error registers Support. */
        uint32_t    u1PerfCounterSup : 1;         /**< Bit  8     - PCSup: Performance Counter Support. */
        uint32_t    u2HostAddrTranslateSize : 2;  /**< Bits 11:10 - HATS: Host Address Translation Size. */
        uint32_t    u2GstAddrTranslateSize : 2;   /**< Bits 13:12 - GATS: Guest Address Translation Size. */
        uint32_t    u2GstCr3RootTblLevel : 2;     /**< Bits 15:14 - GLXSup: Guest CR3 Root Table Level (Max) Size Support. */
        uint32_t    u2SmiFilterSup : 2;           /**< Bits 17:16 - SmiFSup: SMI Filter Register Support. */
        uint32_t    u3SmiFilterCount : 3;         /**< Bits 20:18 - SmiFRC: SMI Filter Register Count. */
        uint32_t    u3GstVirtApicModeSup : 3;     /**< Bits 23:21 - GAMSup: Guest Virtual-APIC Modes Supported. */
        uint32_t    u2DualPprLogSup : 2;          /**< Bits 25:24 - DualPprLogSup: Dual Peripheral Page Request Log Support. */
        uint32_t    u2Rsvd0 : 2;                  /**< Bits 27:26 - Reserved. */
        uint32_t    u2DualEvtLogSup : 2;          /**< Bits 29:28 - DualEventLogSup: Dual Event Log Support. */
        uint32_t    u2Rsvd1 : 2;                  /**< Bits 31:30 - Reserved. */
        uint32_t    u5MaxPasidSup : 5;            /**< Bits 36:32 - PASMax: Maximum PASID Supported. */
        uint32_t    u1UserSupervisorSup : 1;      /**< Bit  37    - USSup: User/Supervisor Page Protection Support. */
        uint32_t    u2DevTabSegSup : 2;           /**< Bits 39:38 - DevTlbSegSup: Segmented Device Table Support. */
        uint32_t    u1PprLogOverflowWarn : 1;     /**< Bit  40    - PprOvrflwEarlySup: PPR Log Overflow Early Warning Support. */
        uint32_t    u1PprAutoRespSup : 1;         /**< Bit  41    - PprAutoRspSup: PPR Automatic Response Support. */
        uint32_t    u2MarcSup : 2;                /**< Bit  43:42 - MarcSup: Memory Access Routing and Control Support. */
        uint32_t    u1BlockStopMarkSup : 1;       /**< Bit  44    - BlkStopMarkSup: Block StopMark messages Support. */
        uint32_t    u1PerfOptSup : 1;             /**< Bit  45    - PerfOptSup: IOMMU Performance Optimization Support. */
        uint32_t    u1MsiCapMmioSup : 1;          /**< Bit  46    - MsiCapMmioSup: MSI Capability Register MMIO Access Support. */
        uint32_t    u1Rsvd1 : 1;                  /**< Bit  47    - Reserved. */
        uint32_t    u1GstIoSup : 1;               /**< Bit  48    - GIoSup: Guest I/O Protection Support. */
        uint32_t    u1HostAccessSup : 1;          /**< Bit  49    - HASup: Host Access Support. */
        uint32_t    u1EnhancedPprSup : 1;         /**< Bit  50    - EPHSup: Enhanced Peripheral Page Request Handling Support. */
        uint32_t    u1AttrForwardSup : 1;         /**< Bit  51    - AttrFWSup: Attribute Forward Support. */
        uint32_t    u1HostDirtySup : 1;           /**< Bit  52    - HDSup: Host Dirty Support. */
        uint32_t    u1Rsvd2 : 1;                  /**< Bit  53    - Reserved. */
        uint32_t    u1InvIoTlbTypeSup : 1;        /**< Bit  54    - InvIotlbTypeSup: Invalidate IOTLB Type Support. */
        uint32_t    u6Rsvd0 : 6;                  /**< Bit  60:55 - Reserved. */
        uint32_t    u1GstUpdateDisSup : 1;        /**< Bit  61    - GAUpdateDisSup: Disable hardware update on GPT Support. */
        uint32_t    u1ForcePhysDstSup : 1;        /**< Bit  62    - ForcePhyDestSup: Force Phys. Dst. Mode for Remapped Intr. */
        uint32_t    u1Rsvd3 : 1;                  /**< Bit  63    - Reserved. */
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t    u64;
} IOMMU_EXT_FEAT_T;
AssertCompileSize(IOMMU_EXT_FEAT_T, 8);

/**
 * Peripheral Page Request Log Base Address Register (MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        RT_GCC_EXTENSION uint64_t   u12Rsvd0 : 12;      /**< Bit 11:0   - Reserved. */
        RT_GCC_EXTENSION uint64_t   u40Base : 40;       /**< Bits 51:12 - PPRLogBase: Peripheral Page Request Log Base Address. */
        RT_GCC_EXTENSION uint64_t   u4Rsvd0 : 4;        /**< Bits 55:52 - Reserved. */
        RT_GCC_EXTENSION uint64_t   u4Len : 4;          /**< Bits 59:56 - PPRLogLen: Peripheral Page Request Log Length. */
        RT_GCC_EXTENSION uint64_t   u4Rsvd1 : 4;        /**< Bits 63:60 - Reserved. */
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t    u64;
} PPR_LOG_BAR_T;
AssertCompileSize(PPR_LOG_BAR_T, 8);
#define IOMMU_PPR_LOG_BAR_VALID_MASK    UINT64_C(0x0f0ffffffffff000)

/**
 * IOMMU Hardware Event Upper Register (MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        RT_GCC_EXTENSION uint64_t   u60FirstOperand : 60;   /**< Bits 59:0  - First event code dependent operand. */
        RT_GCC_EXTENSION uint64_t   u4EvtCode : 4;          /**< Bits 63:60 - Event Code. */
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t    u64;
} IOMMU_HW_EVT_HI_T;
AssertCompileSize(IOMMU_HW_EVT_HI_T, 8);

/**
 * IOMMU Hardware Event Lower Register (MMIO).
 * In accordance with the AMD spec.
 */
typedef uint64_t IOMMU_HW_EVT_LO_T;

/**
 * IOMMU Hardware Event Status (MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t   u1Valid : 1;     /**< Bit 0      - HEV: Hardware Event Valid. */
        uint32_t   u1Overflow : 1;  /**< Bit 1      - HEO: Hardware Event Overflow. */
        uint32_t   u30Rsvd0 : 30;   /**< Bits 31:2  - Reserved. */
        uint32_t   u32Rsvd0;        /**< Bits 63:32 - Reserved. */
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t    u64;
} IOMMU_HW_EVT_STATUS_T;
AssertCompileSize(IOMMU_HW_EVT_STATUS_T, 8);
#define IOMMU_HW_EVT_STATUS_VALID_MASK      UINT64_C(0x0000000000000003)

/**
 * Guest Virtual-APIC Log Base Address Register (MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        RT_GCC_EXTENSION uint64_t   u12Rsvd0 : 12;  /**< Bit 11:0   - Reserved. */
        RT_GCC_EXTENSION uint64_t   u40Base : 40;   /**< Bits 51:12 - GALogBase: Guest Virtual-APIC Log Base Address. */
        RT_GCC_EXTENSION uint64_t   u4Rsvd0 : 4;    /**< Bits 55:52 - Reserved. */
        RT_GCC_EXTENSION uint64_t   u4Len : 4;      /**< Bits 59:56 - GALogLen: Guest Virtual-APIC Log Length. */
        RT_GCC_EXTENSION uint64_t   u4Rsvd1 : 4;    /**< Bits 63:60 - Reserved. */
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t    u64;
} GALOG_BAR_T;
AssertCompileSize(GALOG_BAR_T, 8);

/**
 * Guest Virtual-APIC Log Tail Address Register (MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        RT_GCC_EXTENSION uint64_t   u3Rsvd0 : 3;            /**< Bits 2:0   - Reserved. */
        RT_GCC_EXTENSION uint64_t   u40GALogTailAddr : 48;  /**< Bits 51:3  - GATAddr: Guest Virtual-APIC Tail Log Address. */
        RT_GCC_EXTENSION uint64_t   u11Rsvd1 : 11;          /**< Bits 63:52 - Reserved. */
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t    u64;
} GALOG_TAIL_ADDR_T;
AssertCompileSize(GALOG_TAIL_ADDR_T, 8);

/**
 * PPR Log B Base Address Register (MMIO).
 * In accordance with the AMD spec.
 * Currently identical to PPR_LOG_BAR_T.
 */
typedef PPR_LOG_BAR_T       PPR_LOG_B_BAR_T;

/**
 * Event Log B Base Address Register (MMIO).
 * In accordance with the AMD spec.
 * Currently identical to EVT_LOG_BAR_T.
 */
typedef EVT_LOG_BAR_T       EVT_LOG_B_BAR_T;

/**
 * Device-specific Feature Extension (DSFX) Register (MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    u24DevSpecFeat : 24;     /**< Bits 23:0  - DevSpecificFeatSupp: Implementation specific features. */
        uint32_t    u4RevMinor : 4;          /**< Bits 27:24 - RevMinor: Minor revision identifier. */
        uint32_t    u4RevMajor : 4;          /**< Bits 31:28 - RevMajor: Major revision identifier. */
        uint32_t    u32Rsvd0;                /**< Bits 63:32 - Reserved.*/
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t    u64;
} DEV_SPECIFIC_FEAT_T;
AssertCompileSize(DEV_SPECIFIC_FEAT_T, 8);

/**
 * Device-specific Control Extension (DSCX) Register (MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    u24DevSpecCtrl : 24;     /**< Bits 23:0  - DevSpecificFeatCntrl: Implementation specific control. */
        uint32_t    u4RevMinor : 4;          /**< Bits 27:24 - RevMinor: Minor revision identifier. */
        uint32_t    u4RevMajor : 4;          /**< Bits 31:28 - RevMajor: Major revision identifier. */
        uint32_t    u32Rsvd0;                /**< Bits 63:32 - Reserved.*/
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t    u64;
} DEV_SPECIFIC_CTRL_T;
AssertCompileSize(DEV_SPECIFIC_CTRL_T, 8);

/**
 * Device-specific Status Extension (DSSX) Register (MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    u24DevSpecStatus : 24;      /**< Bits 23:0  - DevSpecificFeatStatus: Implementation specific status. */
        uint32_t    u4RevMinor : 4;             /**< Bits 27:24 - RevMinor: Minor revision identifier. */
        uint32_t    u4RevMajor : 4;             /**< Bits 31:28 - RevMajor: Major revision identifier. */
        uint32_t    u32Rsvd0;                   /**< Bits 63:32 - Reserved.*/
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t    u64;
} DEV_SPECIFIC_STATUS_T;
AssertCompileSize(DEV_SPECIFIC_STATUS_T, 8);

/**
 * MSI Information Register 0 and 1 (PCI) / MSI Vector Register 0 and 1 (MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    u5MsiNumEvtLog : 5;     /**< Bits 4:0   - MsiNum: Event Log MSI message number. */
        uint32_t    u3GstVirtAddrSize: 3;   /**< Bits 7:5   - GVAsize: Guest Virtual Address Size. */
        uint32_t    u7PhysAddrSize : 7;     /**< Bits 14:8  - PAsize: Physical Address Size. */
        uint32_t    u7VirtAddrSize : 7;     /**< Bits 21:15 - VAsize: Virtual Address Size. */
        uint32_t    u1HtAtsResv: 1;         /**< Bit  22    - HtAtsResv: HyperTransport ATS Response Address range Reserved. */
        uint32_t    u4Rsvd0 : 4;            /**< Bits 26:23 - Reserved. */
        uint32_t    u5MsiNumPpr : 5;        /**< Bits 31:27 - MsiNumPPR: Peripheral Page Request MSI message number. */
        uint32_t    u5MsiNumGa : 5;         /**< Bits 36:32 - MsiNumGa: MSI message number for guest virtual-APIC log. */
        uint32_t    u27Rsvd0: 27;           /**< Bits 63:37 - Reserved. */
    } n;
    /** The 32-bit unsigned integer view. */
    uint32_t    au32[2];
    /** The 64-bit unsigned integer view. */
    uint64_t    u64;
} MSI_MISC_INFO_T;
AssertCompileSize(MSI_MISC_INFO_T, 8);
/** MSI Vector Register 0 and 1 (MMIO). */
typedef MSI_MISC_INFO_T       MSI_VECTOR_T;

/**
 * MSI Capability Header Register (PCI + MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    u8MsiCapId : 8;         /**< Bits 7:0   - MsiCapId: Capability ID. */
        uint32_t    u8MsiCapPtr : 8;        /**< Bits 15:8  - MsiCapPtr: Pointer (PCI config offset) to the next capability. */
        uint32_t    u1MsiEnable : 1;        /**< Bit  16    - MsiEn: Message Signal Interrupt Enable. */
        uint32_t    u3MsiMultiMessCap : 3;  /**< Bits 19:17 - MsiMultMessCap: MSI Multi-Message Capability. */
        uint32_t    u3MsiMultiMessEn : 3;   /**< Bits 22:20 - MsiMultMessEn: MSI Multi-Message Enable. */
        uint32_t    u1Msi64BitEn : 1;       /**< Bit  23    - Msi64BitEn: MSI 64-bit Enable. */
        uint32_t    u8Rsvd0 : 8;            /**< Bits 31:24 - Reserved. */
    } n;
    /** The 32-bit unsigned integer view. */
    uint32_t    u32;
} MSI_CAP_HDR_T;
AssertCompileSize(MSI_CAP_HDR_T, 4);
#define IOMMU_MSI_CAP_HDR_MSI_EN_MASK       RT_BIT(16)

/**
 * MSI Mapping Capability Header Register (PCI + MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    u8MsiMapCapId : 8;  /**< Bits 7:0   - MsiMapCapId: MSI Map capability ID. */
        uint32_t    u8Rsvd0 : 8;        /**< Bits 15:8  - Reserved. */
        uint32_t    u1MsiMapEn : 1;     /**< Bit  16    - MsiMapEn: MSI Map enable. */
        uint32_t    u1MsiMapFixed : 1;  /**< Bit  17    - MsiMapFixd: MSI Map fixed. */
        uint32_t    u9Rsvd0 : 9;        /**< Bits 26:18 - Reserved. */
        uint32_t    u5MapCapType : 5;   /**< Bits 31:27 - MsiMapCapType: MSI Mapping capability type. */
    } n;
    /** The 32-bit unsigned integer view. */
    uint32_t    u32;
} MSI_MAP_CAP_HDR_T;
AssertCompileSize(MSI_MAP_CAP_HDR_T, 4);

/**
 * Performance Optimization Control Register (MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    u13Rsvd0 : 13;      /**< Bits 12:0  - Reserved. */
        uint32_t    u1PerfOptEn : 1;    /**< Bit  13    - PerfOptEn: Performance Optimization Enable. */
        uint32_t    u17Rsvd0 : 18;      /**< Bits 31:14 - Reserved. */
    } n;
    /** The 32-bit unsigned integer view. */
    uint32_t    u32;
} IOMMU_PERF_OPT_CTRL_T;
AssertCompileSize(IOMMU_PERF_OPT_CTRL_T, 4);

/**
 * XT (x2APIC) IOMMU General Interrupt Control Register (MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    u2Rsvd0 : 2;                    /**< Bits 1:0   - Reserved.*/
        uint32_t    u1X2ApicIntrDstMode : 1;        /**< Bit  2     - Destination Mode for general interrupt.*/
        uint32_t    u4Rsvd0 : 4;                    /**< Bits 7:3   - Reserved.*/
        uint32_t    u24X2ApicIntrDstLo : 24;        /**< Bits 31:8  - Destination for general interrupt (Lo).*/
        uint32_t    u8X2ApicIntrVector : 8;         /**< Bits 39:32 - Vector for general interrupt.*/
        uint32_t    u1X2ApicIntrDeliveryMode : 1;   /**< Bit  40    - Delivery Mode for general interrupt.*/
        uint32_t    u15Rsvd0 : 15;                  /**< Bits 55:41 - Reserved.*/
        uint32_t    u7X2ApicIntrDstHi : 7;          /**< Bits 63:56 - Destination for general interrupt (Hi) .*/
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t    u64;
} IOMMU_XT_GEN_INTR_CTRL_T;
AssertCompileSize(IOMMU_XT_GEN_INTR_CTRL_T, 8);

/**
 * XT (x2APIC) IOMMU General Interrupt Control Register (MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    u2Rsvd0 : 2;                    /**< Bits 1:0   - Reserved.*/
        uint32_t    u1X2ApicIntrDstMode : 1;        /**< Bit  2     - Destination Mode for the interrupt.*/
        uint32_t    u4Rsvd0 : 4;                    /**< Bits 7:3   - Reserved.*/
        uint32_t    u24X2ApicIntrDstLo : 24;        /**< Bits 31:8  - Destination for the interrupt (Lo).*/
        uint32_t    u8X2ApicIntrVector : 8;         /**< Bits 39:32 - Vector for the interrupt.*/
        uint32_t    u1X2ApicIntrDeliveryMode : 1;   /**< Bit  40    - Delivery Mode for the interrupt.*/
        uint32_t    u15Rsvd0 : 15;                  /**< Bits 55:41 - Reserved.*/
        uint32_t    u7X2ApicIntrDstHi : 7;          /**< Bits 63:56 - Destination for the interrupt (Hi) .*/
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t    u64;
} IOMMU_XT_INTR_CTRL_T;
AssertCompileSize(IOMMU_XT_INTR_CTRL_T, 8);

/**
 * XT (x2APIC) IOMMU PPR Interrupt Control Register (MMIO).
 * In accordance with the AMD spec.
 * Currently identical to IOMMU_XT_INTR_CTRL_T.
 */
typedef IOMMU_XT_INTR_CTRL_T    IOMMU_XT_PPR_INTR_CTRL_T;

/**
 * XT (x2APIC) IOMMU GA (Guest Address) Log Control Register (MMIO).
 * In accordance with the AMD spec.
 * Currently identical to IOMMU_XT_INTR_CTRL_T.
 */
typedef IOMMU_XT_INTR_CTRL_T    IOMMU_XT_GALOG_INTR_CTRL_T;

/**
 * Memory Access and Routing Control (MARC) Aperture Base Register (MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        RT_GCC_EXTENSION uint64_t   u12Rsvd0 : 12;          /**< Bits 11:0  - Reserved. */
        RT_GCC_EXTENSION uint64_t   u40MarcBaseAddr : 40;   /**< Bits 51:12 - MarcBaseAddr: MARC Aperture Base Address. */
        RT_GCC_EXTENSION uint64_t   u12Rsvd1 : 12;          /**< Bits 63:52 - Reserved. */
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t    u64;
} MARC_APER_BAR_T;
AssertCompileSize(MARC_APER_BAR_T, 8);

/**
 * Memory Access and Routing Control (MARC) Relocation Register (MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        RT_GCC_EXTENSION uint64_t   u1RelocEn : 1;          /**< Bit  0     - RelocEn: Relocation Enabled. */
        RT_GCC_EXTENSION uint64_t   u1ReadOnly : 1;         /**< Bit  1     - ReadOnly: Whether only read-only acceses allowed. */
        RT_GCC_EXTENSION uint64_t   u10Rsvd0 : 10;          /**< Bits 11:2  - Reserved. */
        RT_GCC_EXTENSION uint64_t   u40MarcRelocAddr : 40;  /**< Bits 51:12 - MarcRelocAddr: MARC Aperture Relocation Address. */
        RT_GCC_EXTENSION uint64_t   u12Rsvd1 : 12;          /**< Bits 63:52 - Reserved. */
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t    u64;
} MARC_APER_RELOC_T;
AssertCompileSize(MARC_APER_RELOC_T, 8);

/**
 * Memory Access and Routing Control (MARC) Length Register (MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        RT_GCC_EXTENSION uint64_t   u12Rsvd0 : 12;          /**< Bits 11:0  - Reserved. */
        RT_GCC_EXTENSION uint64_t   u40MarcLength : 40;     /**< Bits 51:12 - MarcLength: MARC Aperture Length. */
        RT_GCC_EXTENSION uint64_t   u12Rsvd1 : 12;          /**< Bits 63:52 - Reserved. */
    } n;
    /** The 64-bit unsigned integer view. */
    uint64_t    u64;
} MARC_APER_LEN_T;

/**
 * Memory Access and Routing Control (MARC) Aperture Register.
 * This combines other registers to match the MMIO layout for convenient access.
 */
typedef struct
{
    MARC_APER_BAR_T     Base;
    MARC_APER_RELOC_T   Reloc;
    MARC_APER_LEN_T     Length;
} MARC_APER_T;
AssertCompileSize(MARC_APER_T, 24);

/**
 * IOMMU Reserved Register (MMIO).
 * In accordance with the AMD spec.
 * This register is reserved for hardware use (although RW?).
 */
typedef uint64_t    IOMMU_RSVD_REG_T;

/**
 * Command Buffer Head Pointer Register (MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    off;            /**< Bits 31:0  - Buffer pointer (offset; 16 byte aligned, 512 KB max). */
        uint32_t    u32Rsvd0;       /**< Bits 63:32 - Reserved. */
    } n;
    /** The 32-bit unsigned integer view. */
    uint32_t    au32[2];
    /** The 64-bit unsigned integer view. */
    uint64_t    u64;
} CMD_BUF_HEAD_PTR_T;
AssertCompileSize(CMD_BUF_HEAD_PTR_T, 8);
#define IOMMU_CMD_BUF_HEAD_PTR_VALID_MASK       UINT64_C(0x000000000007fff0)

/**
 * Command Buffer Tail Pointer Register (MMIO).
 * In accordance with the AMD spec.
 * Currently identical to CMD_BUF_HEAD_PTR_T.
 */
typedef CMD_BUF_HEAD_PTR_T    CMD_BUF_TAIL_PTR_T;
#define IOMMU_CMD_BUF_TAIL_PTR_VALID_MASK       IOMMU_CMD_BUF_HEAD_PTR_VALID_MASK

/**
 * Event Log Head Pointer Register (MMIO).
 * In accordance with the AMD spec.
 * Currently identical to CMD_BUF_HEAD_PTR_T.
 */
typedef CMD_BUF_HEAD_PTR_T    EVT_LOG_HEAD_PTR_T;
#define IOMMU_EVT_LOG_HEAD_PTR_VALID_MASK       IOMMU_CMD_BUF_HEAD_PTR_VALID_MASK

/**
 * Event Log Tail Pointer Register (MMIO).
 * In accordance with the AMD spec.
 * Currently identical to CMD_BUF_HEAD_PTR_T.
 */
typedef CMD_BUF_HEAD_PTR_T    EVT_LOG_TAIL_PTR_T;
#define IOMMU_EVT_LOG_TAIL_PTR_VALID_MASK       IOMMU_CMD_BUF_HEAD_PTR_VALID_MASK


/**
 * IOMMU Status Register (MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    u1EvtOverflow : 1;          /**< Bit  0     - EventOverflow: Event log overflow. */
        uint32_t    u1EvtLogIntr : 1;           /**< Bit  1     - EventLogInt: Event log interrupt. */
        uint32_t    u1CompWaitIntr : 1;         /**< Bit  2     - ComWaitInt: Completion wait interrupt . */
        uint32_t    u1EvtLogRunning : 1;        /**< Bit  3     - EventLogRun: Event logging is running. */
        uint32_t    u1CmdBufRunning : 1;        /**< Bit  4     - CmdBufRun: Command buffer is running. */
        uint32_t    u1PprOverflow : 1;          /**< Bit  5     - PprOverflow: Peripheral Page Request Log (PPR) overflow. */
        uint32_t    u1PprIntr : 1;              /**< Bit  6     - PprInt: PPR interrupt. */
        uint32_t    u1PprLogRunning : 1;        /**< Bit  7     - PprLogRun: PPR logging is running. */
        uint32_t    u1GstLogRunning : 1;        /**< Bit  8     - GALogRun: Guest virtual-APIC logging is running. */
        uint32_t    u1GstLogOverflow : 1;       /**< Bit  9     - GALOverflow: Guest virtual-APIC log overflow. */
        uint32_t    u1GstLogIntr : 1;           /**< Bit  10    - GAInt: Guest virtual-APIC log interrupt. */
        uint32_t    u1PprOverflowB : 1;         /**< Bit  11    - PprOverflowB: PPR log B overflow. */
        uint32_t    u1PprLogActive : 1;         /**< Bit  12    - PprLogActive: PPR log A is active. */
        uint32_t    u2Rsvd0 : 2;                /**< Bits 14:13 - Reserved. */
        uint32_t    u1EvtOverflowB : 1;         /**< Bit  15    - EvtOverflowB: Event log B overflow. */
        uint32_t    u1EvtLogActive : 1;         /**< Bit  16    - EvtLogActive: Event log A active. */
        uint32_t    u1PprOverflowEarlyB : 1;    /**< Bit  17    - PprOverflowEarlyB: PPR log B overflow early warning. */
        uint32_t    u1PprOverflowEarly : 1;     /**< Bit  18    - PprOverflowEarly: PPR log overflow early warning. */
        uint32_t    u13Rsvd0 : 13;              /**< Bits 31:19 - Reserved. */
        uint32_t    u32Rsvd0;                   /**< Bits 63:32 - Reserved . */
    } n;
    /** The 32-bit unsigned integer view. */
    uint32_t    au32[2];
    /** The 64-bit unsigned integer view. */
    uint64_t    u64;
} IOMMU_STATUS_T;
AssertCompileSize(IOMMU_STATUS_T, 8);
#define IOMMU_STATUS_VALID_MASK     UINT64_C(0x0000000000079fff)
#define IOMMU_STATUS_RW1C_MASK      UINT64_C(0x0000000000068e67)

/**
 * PPR Log Head Pointer Register (MMIO).
 * In accordance with the AMD spec.
 * Currently identical to CMD_BUF_HEAD_PTR_T.
 */
typedef CMD_BUF_HEAD_PTR_T      PPR_LOG_HEAD_PTR_T;

/**
 * PPR Log Tail Pointer Register (MMIO).
 * In accordance with the AMD spec.
 * Currently identical to CMD_BUF_HEAD_PTR_T.
 */
typedef CMD_BUF_HEAD_PTR_T      PPR_LOG_TAIL_PTR_T;

/**
 * Guest Virtual-APIC Log Head Pointer Register (MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    u2Rsvd0 : 2;            /**< Bits 2:0   - Reserved. */
        uint32_t    u12GALogPtr : 12;       /**< Bits 15:3  - Guest Virtual-APIC Log Head or Tail Pointer. */
        uint32_t    u16Rsvd0 : 16;          /**< Bits 31:16 - Reserved. */
        uint32_t    u32Rsvd0;               /**< Bits 63:32 - Reserved. */
    } n;
    /** The 32-bit unsigned integer view. */
    uint32_t    au32[2];
    /** The 64-bit unsigned integer view. */
    uint64_t    u64;
} GALOG_HEAD_PTR_T;
AssertCompileSize(GALOG_HEAD_PTR_T, 8);

/**
 * Guest Virtual-APIC Log Tail Pointer Register (MMIO).
 * In accordance with the AMD spec.
 * Currently identical to GALOG_HEAD_PTR_T.
 */
typedef GALOG_HEAD_PTR_T        GALOG_TAIL_PTR_T;

/**
 * PPR Log B Head Pointer Register (MMIO).
 * In accordance with the AMD spec.
 * Currently identical to CMD_BUF_HEAD_PTR_T.
 */
typedef CMD_BUF_HEAD_PTR_T      PPR_LOG_B_HEAD_PTR_T;

/**
 * PPR Log B Tail Pointer Register (MMIO).
 * In accordance with the AMD spec.
 * Currently identical to CMD_BUF_HEAD_PTR_T.
 */
typedef CMD_BUF_HEAD_PTR_T      PPR_LOG_B_TAIL_PTR_T;

/**
 * Event Log B Head Pointer Register (MMIO).
 * In accordance with the AMD spec.
 * Currently identical to CMD_BUF_HEAD_PTR_T.
 */
typedef CMD_BUF_HEAD_PTR_T      EVT_LOG_B_HEAD_PTR_T;

/**
 * Event Log B Tail Pointer Register (MMIO).
 * In accordance with the AMD spec.
 * Currently identical to CMD_BUF_HEAD_PTR_T.
 */
typedef CMD_BUF_HEAD_PTR_T      EVT_LOG_B_TAIL_PTR_T;

/**
 * PPR Log Auto Response Register (MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    u4AutoRespCode : 4;     /**< Bits 3:0   - PprAutoRespCode: PPR log Auto Response Code. */
        uint32_t    u1AutoRespMaskGen : 1;  /**< Bit  4     - PprAutoRespMaskGn: PPR log Auto Response Mask Gen. */
        uint32_t    u27Rsvd0 : 27;          /**< Bits 31:5  - Reserved. */
        uint32_t    u32Rsvd0;               /**< Bits 63:32 - Reserved.*/
    } n;
    /** The 32-bit unsigned integer view. */
    uint32_t    au32[2];
    /** The 64-bit unsigned integer view. */
    uint64_t    u64;
} PPR_LOG_AUTO_RESP_T;
AssertCompileSize(PPR_LOG_AUTO_RESP_T, 8);

/**
 * PPR Log Overflow Early Indicator Register (MMIO).
 * In accordance with the AMD spec.
 */
typedef union
{
    struct
    {
        uint32_t    u15Threshold : 15;  /**< Bits 14:0  - PprOvrflwEarlyThreshold: Overflow early indicator threshold. */
        uint32_t    u15Rsvd0 : 15;      /**< Bits 29:15 - Reserved. */
        uint32_t    u1IntrEn : 1;       /**< Bit  30    - PprOvrflwEarlyIntEn: Overflow early indicator interrupt enable. */
        uint32_t    u1Enable : 1;       /**< Bit  31    - PprOvrflwEarlyEn: Overflow early indicator enable. */
        uint32_t    u32Rsvd0;           /**< Bits 63:32 - Reserved. */
    } n;
    /** The 32-bit unsigned integer view. */
    uint32_t    au32[2];
    /** The 64-bit unsigned integer view. */
    uint64_t    u64;
} PPR_LOG_OVERFLOW_EARLY_T;
AssertCompileSize(PPR_LOG_OVERFLOW_EARLY_T, 8);

/**
 * PPR Log B Overflow Early Indicator Register (MMIO).
 * In accordance with the AMD spec.
 * Currently identical to PPR_LOG_OVERFLOW_EARLY_T.
 */
typedef PPR_LOG_OVERFLOW_EARLY_T        PPR_LOG_B_OVERFLOW_EARLY_T;

/**
 * ILLEGAL_DEV_TABLE_ENTRY Event Types.
 * In accordance with the AMD spec.
 */
typedef enum EVT_ILLEGAL_DTE_TYPE_T
{
    kIllegalDteType_RsvdNotZero = 0,
    kIllegalDteType_RsvdIntTabLen,
    kIllegalDteType_RsvdIoCtl,
    kIllegalDteType_RsvdIntCtl
} EVT_ILLEGAL_DTE_TYPE_T;

/**
 * ILLEGAL_DEV_TABLE_ENTRY Event Types.
 * In accordance with the AMD spec.
 */
typedef enum EVT_IO_PAGE_FAULT_TYPE_T
{
    /* Memory transaction. */
    kIoPageFaultType_DteRsvdPagingMode = 0,
    kIoPageFaultType_PteInvalidPageSize,
    kIoPageFaultType_PteInvalidLvlEncoding,
    kIoPageFaultType_SkippedLevelIovaNotZero,
    kIoPageFaultType_PteRsvdNotZero,
    kIoPageFaultType_PteValidNotSet,
    kIoPageFaultType_DteTranslationDisabled,
    kIoPageFaultType_PasidInvalidRange,
    kIoPageFaultType_PermDenied,
    kIoPageFaultType_UserSupervisor,
    /* Interrupt remapping */
    kIoPageFaultType_IrteAddrInvalid,
    kIoPageFaultType_IrteRsvdNotZero,
    kIoPageFaultType_IrteRemapEn,
    kIoPageFaultType_IrteRsvdIntType,
    kIoPageFaultType_IntrReqAborted,
    kIoPageFaultType_IntrWithPasid,
    kIoPageFaultType_SmiFilterMismatch,
    /* Memory transaction or interrupt remapping. */
    kIoPageFaultType_DevId_Invalid
} EVT_IO_PAGE_FAULT_TYPE_T;

/**
 * IOTLB_INV_TIMEOUT Event Types.
 * In accordance with the AMD spec.
 */
typedef enum EVT_IOTLB_INV_TIMEOUT_TYPE_T
{
    InvTimeoutType_NoResponse = 0
} EVT_IOTLB_INV_TIMEOUT_TYPE_T;

/**
 * INVALID_DEVICE_REQUEST Event Types.
 * In accordance with the AMD spec.
 */
typedef enum EVT_INVALID_DEV_REQ_TYPE_T
{
    /* Access. */
    kInvalidDevReqType_ReadOrNonPostedWrite = 0,
    kInvalidDevReqType_PretranslatedTransaction,
    kInvalidDevReqType_PortIo,
    kInvalidDevReqType_SysMgt,
    kInvalidDevReqType_IntrRange,
    kInvalidDevReqType_RsvdIntrRange,
    kInvalidDevReqType_SysMgtAddr,
    /* Translation Request. */
    kInvalidDevReqType_TrAccessInvalid,
    kInvalidDevReqType_TrDisabled,
    kInvalidDevReqType_DevIdInvalid,
} EVT_INVALID_DEV_REQ_TYPE_T;

/**
 * INVALID_PPR_REQUEST Event Types.
 * In accordance with the AMD spec.
 */
typedef enum EVT_INVALID_PPR_REQ_TYPE_T
{
    kInvalidPprReqType_PriNotSupported,
    kInvalidPprReqType_GstTranslateDisabled
} EVT_INVALID_PPR_REQ_TYPE_T;

/**
 * IOMMU operations (transaction) types.
 */
typedef enum IOMMUOP
{
    /** Address translation request. */
    IOMMUOP_TRANSLATE_REQ = 0,
    /** Memory read request. */
    IOMMUOP_MEM_READ,
    /** Memory write request. */
    IOMMUOP_MEM_WRITE,
    /** Interrupt request. */
    IOMMUOP_INTR_REQ,
    /** Command. */
    IOMMUOP_CMD
} IOMMUOP;
AssertCompileSize(IOMMUOP, 4);

/**
 * I/O page walk result.
 */
typedef struct
{
    /** The translated system physical address. */
    RTGCPHYS        GCPhysSpa;
    /** The number of offset bits in the system physical address. */
    uint8_t         cShift;
    /** The I/O permissions allowed by the translation (IOMMU_IO_PERM_XXX). */
    uint8_t         fIoPerm;
    /** Padding. */
    uint8_t         abPadding[2];
} IOWALKRESULT;
/** Pointer to an I/O walk result struct. */
typedef IOWALKRESULT *PIOWALKRESULT;
/** Pointer to a const I/O walk result struct. */
typedef IOWALKRESULT *PCIOWALKRESULT;

/**
 * IOMMU I/O TLB Entry.
 * Keep this as small and aligned as possible.
 */
typedef struct
{
    /** The translated system physical address (SPA) of the page. */
    RTGCPHYS        GCPhysSpa;
    /** The index of the 4K page within a large page. */
    uint32_t        idxSubPage;
    /** The I/O access permissions (IOMMU_IO_PERM_XXX). */
    uint8_t         fIoPerm;
    /** The number of offset bits in the translation indicating page size. */
    uint8_t         cShift;
    /** Alignment padding. */
    uint8_t         afPadding[2];
} IOTLBE_T;
AssertCompileSize(IOTLBE_T, 16);
/** Pointer to an IOMMU I/O TLB entry struct. */
typedef IOTLBE_T *PIOTLBE_T;
/** Pointer to a const IOMMU I/O TLB entry struct. */
typedef IOTLBE_T const *PCIOTLBE_T;

/**
 * The shared IOMMU device state.
 */
typedef struct IOMMU
{
    /** IOMMU device index (0 is at the top of the PCI tree hierarchy). */
    uint32_t                    idxIommu;
    /** Alignment padding. */
    uint32_t                    uPadding0;

    /** Whether the command thread is sleeping. */
    bool volatile               fCmdThreadSleeping;
    /** Alignment padding. */
    uint8_t                     afPadding0[3];
    /** Whether the command thread has been signaled for wake up. */
    bool volatile               fCmdThreadSignaled;
    /** Alignment padding. */
    uint8_t                     afPadding1[3];

    /** The event semaphore the command thread waits on. */
    SUPSEMEVENT                 hEvtCmdThread;
    /** The MMIO handle. */
    IOMMMIOHANDLE               hMmio;

    /** @name PCI: Base capability block registers.
     * @{ */
    IOMMU_BAR_T                 IommuBar;            /**< IOMMU base address register. */
    /** @} */

    /** @name MMIO: Control and status registers.
     * @{ */
    DEV_TAB_BAR_T               aDevTabBaseAddrs[8]; /**< Device table base address registers. */
    CMD_BUF_BAR_T               CmdBufBaseAddr;      /**< Command buffer base address register. */
    EVT_LOG_BAR_T               EvtLogBaseAddr;      /**< Event log base address register. */
    IOMMU_CTRL_T                Ctrl;                /**< IOMMU control register. */
    IOMMU_EXCL_RANGE_BAR_T      ExclRangeBaseAddr;   /**< IOMMU exclusion range base register. */
    IOMMU_EXCL_RANGE_LIMIT_T    ExclRangeLimit;      /**< IOMMU exclusion range limit. */
    IOMMU_EXT_FEAT_T            ExtFeat;             /**< IOMMU extended feature register. */
    /** @} */

    /** @name MMIO: PPR Log registers.
     * @{ */
    PPR_LOG_BAR_T               PprLogBaseAddr;      /**< PPR Log base address register. */
    IOMMU_HW_EVT_HI_T           HwEvtHi;             /**< IOMMU hardware event register (Hi). */
    IOMMU_HW_EVT_LO_T           HwEvtLo;             /**< IOMMU hardware event register (Lo). */
    IOMMU_HW_EVT_STATUS_T       HwEvtStatus;         /**< IOMMU hardware event status. */
    /** @} */

    /** @todo IOMMU: SMI filter. */

    /** @name MMIO: Guest Virtual-APIC Log registers.
     * @{ */
    GALOG_BAR_T                 GALogBaseAddr;       /**< Guest Virtual-APIC Log base address register. */
    GALOG_TAIL_ADDR_T           GALogTailAddr;       /**< Guest Virtual-APIC Log Tail address register. */
    /** @} */

    /** @name MMIO: Alternate PPR and Event Log registers.
     *  @{ */
    PPR_LOG_B_BAR_T             PprLogBBaseAddr;     /**< PPR Log B base address register. */
    EVT_LOG_B_BAR_T             EvtLogBBaseAddr;     /**< Event Log B base address register. */
    /** @} */

    /** @name MMIO: Device-specific feature registers.
     * @{ */
    DEV_SPECIFIC_FEAT_T         DevSpecificFeat;     /**< Device-specific feature extension register (DSFX). */
    DEV_SPECIFIC_CTRL_T         DevSpecificCtrl;     /**< Device-specific control extension register (DSCX). */
    DEV_SPECIFIC_STATUS_T       DevSpecificStatus;   /**< Device-specific status extension register (DSSX). */
    /** @} */

    /** @name MMIO: MSI Capability Block registers.
     * @{ */
    MSI_MISC_INFO_T             MsiMiscInfo;         /**< MSI Misc. info registers / MSI Vector registers. */
    /** @} */

    /** @name MMIO: Performance Optimization Control registers.
     *  @{ */
    IOMMU_PERF_OPT_CTRL_T       PerfOptCtrl;         /**< IOMMU Performance optimization control register. */
    /** @} */

    /** @name MMIO: x2APIC Control registers.
     * @{ */
    IOMMU_XT_GEN_INTR_CTRL_T    XtGenIntrCtrl;       /**< IOMMU X2APIC General interrupt control register. */
    IOMMU_XT_PPR_INTR_CTRL_T    XtPprIntrCtrl;       /**< IOMMU X2APIC PPR interrupt control register. */
    IOMMU_XT_GALOG_INTR_CTRL_T  XtGALogIntrCtrl;     /**< IOMMU X2APIC Guest Log interrupt control register. */
    /** @} */

    /** @name MMIO: MARC registers.
     * @{ */
    MARC_APER_T                 aMarcApers[4];       /**< MARC Aperture Registers. */
    /** @} */

    /** @name MMIO: Reserved register.
     *  @{ */
    IOMMU_RSVD_REG_T            RsvdReg;             /**< IOMMU Reserved Register. */
    /** @} */

    /** @name MMIO: Command and Event Log pointer registers.
     * @{ */
    CMD_BUF_HEAD_PTR_T          CmdBufHeadPtr;       /**< Command buffer head pointer register. */
    CMD_BUF_TAIL_PTR_T          CmdBufTailPtr;       /**< Command buffer tail pointer register. */
    EVT_LOG_HEAD_PTR_T          EvtLogHeadPtr;       /**< Event log head pointer register. */
    EVT_LOG_TAIL_PTR_T          EvtLogTailPtr;       /**< Event log tail pointer register. */
    /** @} */

    /** @name MMIO: Command and Event Status register.
     * @{ */
    IOMMU_STATUS_T              Status;              /**< IOMMU status register. */
    /** @} */

    /** @name MMIO: PPR Log Head and Tail pointer registers.
     * @{ */
    PPR_LOG_HEAD_PTR_T          PprLogHeadPtr;       /**< IOMMU PPR log head pointer register. */
    PPR_LOG_TAIL_PTR_T          PprLogTailPtr;       /**< IOMMU PPR log tail pointer register. */
    /** @} */

    /** @name MMIO: Guest Virtual-APIC Log Head and Tail pointer registers.
     * @{ */
    GALOG_HEAD_PTR_T            GALogHeadPtr;        /**< Guest Virtual-APIC log head pointer register. */
    GALOG_TAIL_PTR_T            GALogTailPtr;        /**< Guest Virtual-APIC log tail pointer register. */
    /** @} */

    /** @name MMIO: PPR Log B Head and Tail pointer registers.
     *  @{ */
    PPR_LOG_B_HEAD_PTR_T        PprLogBHeadPtr;      /**< PPR log B head pointer register. */
    PPR_LOG_B_TAIL_PTR_T        PprLogBTailPtr;      /**< PPR log B tail pointer register. */
    /** @} */

    /** @name MMIO: Event Log B Head and Tail pointer registers.
     * @{ */
    EVT_LOG_B_HEAD_PTR_T        EvtLogBHeadPtr;      /**< Event log B head pointer register. */
    EVT_LOG_B_TAIL_PTR_T        EvtLogBTailPtr;      /**< Event log B tail pointer register. */
    /** @} */

    /** @name MMIO: PPR Log Overflow protection registers.
     * @{ */
    PPR_LOG_AUTO_RESP_T         PprLogAutoResp;         /**< PPR Log Auto Response register. */
    PPR_LOG_OVERFLOW_EARLY_T    PprLogOverflowEarly;    /**< PPR Log Overflow Early Indicator register. */
    PPR_LOG_B_OVERFLOW_EARLY_T  PprLogBOverflowEarly;   /**< PPR Log B Overflow Early Indicator register. */
    /** @} */

    /** @todo IOMMU: IOMMU Event counter registers. */

    /** @todo IOMMU: Stat counters. */
} IOMMU;
/** Pointer to the IOMMU device state. */
typedef struct IOMMU *PIOMMU;
/** Pointer to the const IOMMU device state. */
typedef const struct IOMMU *PCIOMMU;
AssertCompileMemberAlignment(IOMMU, fCmdThreadSleeping, 4);
AssertCompileMemberAlignment(IOMMU, fCmdThreadSignaled, 4);
AssertCompileMemberAlignment(IOMMU, hEvtCmdThread, 8);
AssertCompileMemberAlignment(IOMMU, hMmio, 8);
AssertCompileMemberAlignment(IOMMU, IommuBar, 8);

/**
 * The ring-3 IOMMU device state.
 */
typedef struct IOMMUR3
{
    /** Device instance. */
    PPDMDEVINSR3                pDevInsR3;
    /** The IOMMU helpers. */
    PCPDMIOMMUHLPR3             pIommuHlpR3;
    /** The command thread handle. */
    R3PTRTYPE(PPDMTHREAD)       pCmdThread;
} IOMMUR3;
/** Pointer to the ring-3 IOMMU device state. */
typedef IOMMUR3 *PIOMMUR3;

/**
 * The ring-0 IOMMU device state.
 */
typedef struct IOMMUR0
{
    /** Device instance. */
    PPDMDEVINSR0                pDevInsR0;
    /** The IOMMU helpers. */
    PCPDMIOMMUHLPR0             pIommuHlpR0;
} IOMMUR0;
/** Pointer to the ring-0 IOMMU device state. */
typedef IOMMUR0 *PIOMMUR0;

/**
 * The raw-mode IOMMU device state.
 */
typedef struct IOMMURC
{
    /** Device instance. */
    PPDMDEVINSR0                pDevInsRC;
    /** The IOMMU helpers. */
    PCPDMIOMMUHLPRC             pIommuHlpRC;
} IOMMURC;
/** Pointer to the raw-mode IOMMU device state. */
typedef IOMMURC *PIOMMURC;

/** The IOMMU device state for the current context. */
typedef CTX_SUFF(IOMMU)  IOMMUCC;
/** Pointer to the IOMMU device state for the current context. */
typedef CTX_SUFF(PIOMMU) PIOMMUCC;

/**
 * IOMMU register access routines.
 */
typedef struct
{
    const char   *pszName;
    VBOXSTRICTRC (*pfnRead )(PPDMDEVINS pDevIns, PIOMMU pThis, uint32_t iReg, uint64_t *pu64Value);
    VBOXSTRICTRC (*pfnWrite)(PPDMDEVINS pDevIns, PIOMMU pThis, uint32_t iReg, uint64_t  u64Value);
    bool         f64BitReg;
} IOMMUREGACC;


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/
/**
 * An array of the number of device table segments supported.
 * Indexed by u2DevTabSegSup.
 */
static uint8_t const g_acDevTabSegs[] = { 0, 2, 4, 8 };

/**
 * An array of the masks to select the device table segment index from a device ID.
 */
static uint16_t const g_auDevTabSegMasks[] = { 0x0, 0x8000, 0xc000, 0xe000 };

/**
 * The maximum size (inclusive) of each device table segment (0 to 7).
 * Indexed by the device table segment index.
 */
static uint16_t const g_auDevTabSegMaxSizes[] = { 0x1ff, 0xff, 0x7f, 0x7f, 0x3f, 0x3f, 0x3f, 0x3f };


#ifndef VBOX_DEVICE_STRUCT_TESTCASE
/**
 * Gets the maximum number of buffer entries for the given buffer length.
 *
 * @returns Number of buffer entries.
 * @param   uEncodedLen     The length (power-of-2 encoded).
 */
DECLINLINE(uint32_t) iommuAmdGetBufMaxEntries(uint8_t uEncodedLen)
{
    Assert(uEncodedLen > 7);
    return 2 << (uEncodedLen - 1);
}


/**
 * Gets the total length of the buffer given a base register's encoded length.
 *
 * @returns The length of the buffer in bytes.
 * @param   uEncodedLen     The length (power-of-2 encoded).
 */
DECLINLINE(uint32_t) iommuAmdGetTotalBufLength(uint8_t uEncodedLen)
{
    Assert(uEncodedLen > 7);
    return (2 << (uEncodedLen - 1)) << 4;
}


/**
 * Gets the number of (unconsumed) entries in the event log.
 *
 * @returns The number of entries in the event log.
 * @param   pThis     The IOMMU device state.
 */
static uint32_t iommuAmdGetEvtLogEntryCount(PIOMMU pThis)
{
    uint32_t const idxTail = pThis->EvtLogTailPtr.n.off >> IOMMU_EVT_GENERIC_SHIFT;
    uint32_t const idxHead = pThis->EvtLogHeadPtr.n.off >> IOMMU_EVT_GENERIC_SHIFT;
    if (idxTail >= idxHead)
        return idxTail - idxHead;

    uint32_t const cMaxEvts = iommuAmdGetBufMaxEntries(pThis->EvtLogBaseAddr.n.u4Len);
    return cMaxEvts - idxHead + idxTail;
}


/**
 * Gets the number of (unconsumed) commands in the command buffer.
 *
 * @returns The number of commands in the command buffer.
 * @param   pThis     The IOMMU device state.
 */
static uint32_t iommuAmdGetCmdBufEntryCount(PIOMMU pThis)
{
    uint32_t const idxTail = pThis->CmdBufTailPtr.n.off >> IOMMU_CMD_GENERIC_SHIFT;
    uint32_t const idxHead = pThis->CmdBufHeadPtr.n.off >> IOMMU_CMD_GENERIC_SHIFT;
    if (idxTail >= idxHead)
        return idxTail - idxHead;

    uint32_t const cMaxCmds = iommuAmdGetBufMaxEntries(pThis->CmdBufBaseAddr.n.u4Len);
    return cMaxCmds - idxHead + idxTail;
}


DECL_FORCE_INLINE(IOMMU_STATUS_T) iommuAmdGetStatus(PCIOMMU pThis)
{
    IOMMU_STATUS_T Status;
    Status.u64 = ASMAtomicReadU64((volatile uint64_t *)&pThis->Status.u64);
    return Status;
}


DECL_FORCE_INLINE(IOMMU_CTRL_T) iommuAmdGetCtrl(PCIOMMU pThis)
{
    IOMMU_CTRL_T Ctrl;
    Ctrl.u64 = ASMAtomicReadU64((volatile uint64_t *)&pThis->Ctrl.u64);
    return Ctrl;
}


/**
 * Returns whether MSI is enabled for the IOMMU.
 *
 * @returns Whether MSI is enabled.
 * @param   pDevIns     The IOMMU device instance.
 *
 * @note There should be a PCIDevXxx function for this.
 */
static bool iommuAmdIsMsiEnabled(PPDMDEVINS pDevIns)
{
    MSI_CAP_HDR_T MsiCapHdr;
    MsiCapHdr.u32 = PDMPciDevGetDWord(pDevIns->apPciDevs[0], IOMMU_PCI_OFF_MSI_CAP_HDR);
    return MsiCapHdr.n.u1MsiEnable;
}


/**
 * Signals a PCI target abort.
 *
 * @param   pDevIns     The IOMMU device instance.
 */
static void iommuAmdSetPciTargetAbort(PPDMDEVINS pDevIns)
{
    PPDMPCIDEV pPciDev = pDevIns->apPciDevs[0];
    uint16_t const u16Status = PDMPciDevGetStatus(pPciDev) | VBOX_PCI_STATUS_SIG_TARGET_ABORT;
    PDMPciDevSetStatus(pPciDev, u16Status);
}


/**
 * Wakes up the command thread if there are commands to be processed or if
 * processing is requested to be stopped by software.
 *
 * @param   pDevIns     The IOMMU device instance.
 */
static void iommuAmdCmdThreadWakeUpIfNeeded(PPDMDEVINS pDevIns)
{
    IOMMU_ASSERT_LOCKED(pDevIns);

    PIOMMU pThis = PDMDEVINS_2_DATA(pDevIns, PIOMMU);
    if (   !ASMAtomicXchgBool(&pThis->fCmdThreadSignaled, true)
        &&  ASMAtomicReadBool(&pThis->fCmdThreadSleeping))
        PDMDevHlpSUPSemEventSignal(pDevIns, pThis->hEvtCmdThread);
}


/**
 * Writes to a read-only register.
 */
static VBOXSTRICTRC iommuAmdIgnore_w(PPDMDEVINS pDevIns, PIOMMU pThis, uint32_t iReg, uint64_t u64Value)
{
    RT_NOREF(pDevIns, pThis, iReg, u64Value);
    Log((IOMMU_LOG_PFX ": Write to read-only register (%#x) with value %#RX64 ignored\n", iReg, u64Value));
    return VINF_SUCCESS;
}


/**
 * Writes the Device Table Base Address Register.
 */
static VBOXSTRICTRC iommuAmdDevTabBar_w(PPDMDEVINS pDevIns, PIOMMU pThis, uint32_t iReg, uint64_t u64Value)
{
    RT_NOREF(pDevIns, iReg);

    /* Mask out all unrecognized bits. */
    u64Value &= IOMMU_DEV_TAB_BAR_VALID_MASK;

    /* Update the register. */
    pThis->aDevTabBaseAddrs[0].u64 = u64Value;
    return VINF_SUCCESS;
}


/**
 * Writes the Command Buffer Base Address Register.
 */
static VBOXSTRICTRC iommuAmdCmdBufBar_w(PPDMDEVINS pDevIns, PIOMMU pThis, uint32_t iReg, uint64_t u64Value)
{
    RT_NOREF(pDevIns, iReg);

    /*
     * While this is not explicitly specified like the event log base address register,
     * the AMD spec. does specify "CmdBufRun must be 0b to modify the command buffer registers properly".
     * Inconsistent specs :/
     */
    IOMMU_STATUS_T const Status = iommuAmdGetStatus(pThis);
    if (Status.n.u1CmdBufRunning)
    {
        Log((IOMMU_LOG_PFX ": Setting CmdBufBar (%#RX64) when command buffer is running -> Ignored\n", u64Value));
        return VINF_SUCCESS;
    }

    /* Mask out all unrecognized bits. */
    CMD_BUF_BAR_T CmdBufBaseAddr;
    CmdBufBaseAddr.u64 = u64Value & IOMMU_CMD_BUF_BAR_VALID_MASK;

    /* Validate the length. */
    if (CmdBufBaseAddr.n.u4Len >= 8)
    {
        /* Update the register. */
        pThis->CmdBufBaseAddr.u64 = CmdBufBaseAddr.u64;

        /*
         * Writing the command buffer base address, clears the command buffer head and tail pointers.
         * See AMD spec. 2.4 "Commands".
         */
        pThis->CmdBufHeadPtr.u64 = 0;
        pThis->CmdBufTailPtr.u64 = 0;
    }
    else
        Log((IOMMU_LOG_PFX ": Command buffer length (%#x) invalid -> Ignored\n", CmdBufBaseAddr.n.u4Len));

    return VINF_SUCCESS;
}


/**
 * Writes the Event Log Base Address Register.
 */
static VBOXSTRICTRC iommuAmdEvtLogBar_w(PPDMDEVINS pDevIns, PIOMMU pThis, uint32_t iReg, uint64_t u64Value)
{
    RT_NOREF(pDevIns, iReg);

    /*
     * IOMMU behavior is undefined when software writes this register when event logging is running.
     * In our emulation, we ignore the write entirely.
     * See AMD IOMMU spec. "Event Log Base Address Register".
     */
    IOMMU_STATUS_T const Status = iommuAmdGetStatus(pThis);
    if (Status.n.u1EvtLogRunning)
    {
        Log((IOMMU_LOG_PFX ": Setting EvtLogBar (%#RX64) when event logging is running -> Ignored\n", u64Value));
        return VINF_SUCCESS;
    }

    /* Mask out all unrecognized bits. */
    u64Value &= IOMMU_EVT_LOG_BAR_VALID_MASK;
    EVT_LOG_BAR_T EvtLogBaseAddr;
    EvtLogBaseAddr.u64 = u64Value;

    /* Validate the length. */
    if (EvtLogBaseAddr.n.u4Len >= 8)
    {
        /* Update the register. */
        pThis->EvtLogBaseAddr.u64 = EvtLogBaseAddr.u64;

        /*
         * Writing the event log base address, clears the event log head and tail pointers.
         * See AMD spec. 2.5 "Event Logging".
         */
        pThis->EvtLogHeadPtr.u64 = 0;
        pThis->EvtLogTailPtr.u64 = 0;
    }
    else
        Log((IOMMU_LOG_PFX ": Event log length (%#x) invalid -> Ignored\n", EvtLogBaseAddr.n.u4Len));

    return VINF_SUCCESS;
}


/**
 * Writes the Control Register.
 */
static VBOXSTRICTRC iommuAmdCtrl_w(PPDMDEVINS pDevIns, PIOMMU pThis, uint32_t iReg, uint64_t u64Value)
{
    RT_NOREF(pDevIns, iReg);

    /* Mask out all unrecognized bits. */
    u64Value &= IOMMU_CTRL_VALID_MASK;

    IOMMU_CTRL_T const OldCtrl = iommuAmdGetCtrl(pThis);
    IOMMU_CTRL_T NewCtrl;
    NewCtrl.u64 = u64Value;

    /* Update the register. */
    ASMAtomicWriteU64(&pThis->Ctrl.u64, NewCtrl.u64);

    /* Enable or disable event logging when the bit transitions. */
    bool const fNewIommuEn  = NewCtrl.n.u1IommuEn;
    bool const fOldEvtLogEn = OldCtrl.n.u1EvtLogEn;
    bool const fNewEvtLogEn = NewCtrl.n.u1EvtLogEn;
    if (fOldEvtLogEn != fNewEvtLogEn)
    {
        if (   fNewIommuEn
            && fNewEvtLogEn)
        {
            ASMAtomicAndU64(&pThis->Status.u64, ~IOMMU_STATUS_EVT_LOG_OVERFLOW);
            ASMAtomicOrU64(&pThis->Status.u64, IOMMU_STATUS_EVT_LOG_RUNNING);
        }
        else
            ASMAtomicAndU64(&pThis->Status.u64, ~IOMMU_STATUS_EVT_LOG_RUNNING);
    }

    /* Enable or disable command buffer processing when the bit transitions. */
    bool const fOldCmdBufEn = OldCtrl.n.u1CmdBufEn;
    bool const fNewCmdBufEn = NewCtrl.n.u1CmdBufEn;
    if (fOldCmdBufEn != fNewCmdBufEn)
    {
        if (   fNewIommuEn
            && fNewCmdBufEn)
            ASMAtomicOrU64(&pThis->Status.u64, IOMMU_STATUS_CMD_BUF_RUNNING);
        else
            ASMAtomicAndU64(&pThis->Status.u64, ~IOMMU_STATUS_CMD_BUF_RUNNING);

        /* Wake up the command thread to start or stop processing commands. */
        iommuAmdCmdThreadWakeUpIfNeeded(pDevIns);
    }
}


/**
 * Writes to the Excluse Range Base Address Register.
 */
static VBOXSTRICTRC iommuAmdExclRangeBar_w(PPDMDEVINS pDevIns, PIOMMU pThis, uint32_t iReg, uint64_t u64Value)
{
    RT_NOREF(pDevIns, iReg);
    pThis->ExclRangeBaseAddr.u64 = u64Value & IOMMU_EXCL_RANGE_BAR_VALID_MASK;
    return VINF_SUCCESS;
}


/**
 * Writes to the Excluse Range Limit Register.
 */
static VBOXSTRICTRC iommuAmdExclRangeLimit_w(PPDMDEVINS pDevIns, PIOMMU pThis, uint32_t iReg, uint64_t u64Value)
{
    RT_NOREF(pDevIns, iReg);
    u64Value &= IOMMU_EXCL_RANGE_LIMIT_VALID_MASK;
    u64Value |= UINT64_C(0xfff);
    pThis->ExclRangeLimit.u64 = u64Value;
    return VINF_SUCCESS;
}


/**
 * Writes the Hardware Event Register (Hi).
 */
static VBOXSTRICTRC iommuAmdHwEvtHi_w(PPDMDEVINS pDevIns, PIOMMU pThis, uint32_t iReg, uint64_t u64Value)
{
    /** @todo IOMMU: Why the heck is this marked read/write by the AMD IOMMU spec? */
    RT_NOREF(pDevIns, iReg);
    Log((IOMMU_LOG_PFX ": Writing %#RX64 to hardware event (Hi) register!\n", u64Value));
    pThis->HwEvtHi.u64 = u64Value;
    return VINF_SUCCESS;
}


/**
 * Writes the Hardware Event Register (Lo).
 */
static VBOXSTRICTRC iommuAmdHwEvtLo_w(PPDMDEVINS pDevIns, PIOMMU pThis, uint32_t iReg, uint64_t u64Value)
{
    /** @todo IOMMU: Why the heck is this marked read/write by the AMD IOMMU spec? */
    RT_NOREF(pDevIns, iReg);
    Log((IOMMU_LOG_PFX ": Writing %#RX64 to hardware event (Lo) register!\n", u64Value));
    pThis->HwEvtLo = u64Value;
    return VINF_SUCCESS;
}


/**
 * Writes the Hardware Event Status Register.
 */
static VBOXSTRICTRC iommuAmdHwEvtStatus_w(PPDMDEVINS pDevIns, PIOMMU pThis, uint32_t iReg, uint64_t u64Value)
{
    RT_NOREF(pDevIns, iReg);

    /* Mask out all unrecognized bits. */
    u64Value &= IOMMU_HW_EVT_STATUS_VALID_MASK;

    /*
     * The two bits (HEO and HEV) are RW1C (Read/Write 1-to-Clear; writing 0 has no effect).
     * If the current status bits or the bits being written are both 0, we've nothing to do.
     * The Overflow bit (bit 1) is only valid when the Valid bit (bit 0) is 1.
     */
    uint64_t HwStatus = pThis->HwEvtStatus.u64;
    if (!(HwStatus & RT_BIT(0)))
        return VINF_SUCCESS;
    if (u64Value & HwStatus & RT_BIT_64(0))
        HwStatus &= ~RT_BIT_64(0);
    if (u64Value & HwStatus & RT_BIT_64(1))
        HwStatus &= ~RT_BIT_64(1);

    /* Update the register. */
    pThis->HwEvtStatus.u64 = HwStatus;
    return VINF_SUCCESS;
}


/**
 * Writes the Device Table Segment Base Address Register.
 */
static VBOXSTRICTRC iommuAmdDevTabSegBar_w(PPDMDEVINS pDevIns, PIOMMU pThis, uint32_t iReg, uint64_t u64Value)
{
    RT_NOREF(pDevIns);

    /* Figure out which segment is being written. */
    uint8_t const offSegment = (iReg - IOMMU_MMIO_OFF_DEV_TAB_SEG_FIRST) >> 3;
    uint8_t const idxSegment = offSegment + 1;
    Assert(idxSegment < RT_ELEMENTS(pThis->aDevTabBaseAddrs));

    /* Mask out all unrecognized bits. */
    u64Value &= IOMMU_DEV_TAB_SEG_BAR_VALID_MASK;
    DEV_TAB_BAR_T DevTabSegBar;
    DevTabSegBar.u64 = u64Value;

    /* Validate the size. */
    uint16_t const uSegSize    = DevTabSegBar.n.u9Size;
    uint16_t const uMaxSegSize = g_auDevTabSegMaxSizes[idxSegment];
    if (uSegSize <= uMaxSegSize)
    {
        /* Update the register. */
        pThis->aDevTabBaseAddrs[idxSegment].u64 = u64Value;
    }
    else
        Log((IOMMU_LOG_PFX ": Device table segment (%u) size invalid (%#RX32) -> Ignored\n", idxSegment, uSegSize));

    return VINF_SUCCESS;
}


/**
 * Writes the MSI Capability Header Register.
 */
static VBOXSTRICTRC iommuAmdMsiCapHdr_w(PPDMDEVINS pDevIns, PIOMMU pThis, uint32_t iReg, uint64_t u64Value)
{
    RT_NOREF(pThis, iReg);
    PPDMPCIDEV pPciDev = pDevIns->apPciDevs[0];
    PDMPCIDEV_ASSERT_VALID(pDevIns, pPciDev);
    MSI_CAP_HDR_T MsiCapHdr;
    MsiCapHdr.u32 = PDMPciDevGetDWord(pPciDev, IOMMU_PCI_OFF_MSI_CAP_HDR);
    MsiCapHdr.n.u1MsiEnable = RT_BOOL(u64Value & IOMMU_MSI_CAP_HDR_MSI_EN_MASK);
    PDMPciDevSetDWord(pPciDev, IOMMU_PCI_OFF_MSI_CAP_HDR, MsiCapHdr.u32);
    return VINF_SUCCESS;
}


/**
 * Writes the MSI Address (Lo) Register (32-bit).
 */
static VBOXSTRICTRC iommuAmdMsiAddrLo_w(PPDMDEVINS pDevIns, PIOMMU pThis, uint32_t iReg, uint64_t u64Value)
{
    RT_NOREF(pThis, iReg);
    Assert(!RT_HI_U32(u64Value));
    PPDMPCIDEV pPciDev = pDevIns->apPciDevs[0];
    PDMPCIDEV_ASSERT_VALID(pDevIns, pPciDev);
    PDMPciDevSetDWord(pPciDev, IOMMU_PCI_OFF_MSI_ADDR_LO, u64Value & VBOX_MSI_ADDR_VALID_MASK);
    return VINF_SUCCESS;
}


/**
 * Writes the MSI Address (Hi) Register (32-bit).
 */
static VBOXSTRICTRC iommuAmdMsiAddrHi_w(PPDMDEVINS pDevIns, PIOMMU pThis, uint32_t iReg, uint64_t u64Value)
{
    RT_NOREF(pThis, iReg);
    Assert(!RT_HI_U32(u64Value));
    PPDMPCIDEV pPciDev = pDevIns->apPciDevs[0];
    PDMPCIDEV_ASSERT_VALID(pDevIns, pPciDev);
    PDMPciDevSetDWord(pPciDev, IOMMU_PCI_OFF_MSI_ADDR_HI, u64Value);
    return VINF_SUCCESS;
}


/**
 * Writes the MSI Data Register (32-bit).
 */
static VBOXSTRICTRC iommuAmdMsiData_w(PPDMDEVINS pDevIns, PIOMMU pThis, uint32_t iReg, uint64_t u64Value)
{
    RT_NOREF(pThis, iReg);
    PPDMPCIDEV pPciDev = pDevIns->apPciDevs[0];
    PDMPCIDEV_ASSERT_VALID(pDevIns, pPciDev);
    PDMPciDevSetDWord(pPciDev, IOMMU_PCI_OFF_MSI_DATA, u64Value & VBOX_MSI_DATA_VALID_MASK);
    return VINF_SUCCESS;
}


/**
 * Writes the Command Buffer Head Pointer Register (32-bit).
 */
static VBOXSTRICTRC iommuAmdCmdBufHeadPtr_w(PPDMDEVINS pDevIns, PIOMMU pThis, uint32_t iReg, uint64_t u64Value)
{
    RT_NOREF(pDevIns, iReg);

    /*
     * IOMMU behavior is undefined when software writes this register when the command buffer is running.
     * In our emulation, we ignore the write entirely.
     * See AMD IOMMU spec. 3.3.13 "Command and Event Log Pointer Registers".
     */
    IOMMU_STATUS_T const Status = iommuAmdGetStatus(pThis);
    if (Status.n.u1CmdBufRunning)
    {
        Log((IOMMU_LOG_PFX ": Setting CmdBufHeadPtr (%#RX64) when command buffer is running -> Ignored\n", u64Value));
        return VINF_SUCCESS;
    }

    /*
     * IOMMU behavior is undefined when software writes a value outside the buffer length.
     * In our emulation, we ignore the write entirely.
     */
    uint32_t const offBuf = u64Value & IOMMU_CMD_BUF_HEAD_PTR_VALID_MASK;
    uint32_t const cbBuf  = iommuAmdGetTotalBufLength(pThis->CmdBufBaseAddr.n.u4Len);
    Assert(cbBuf <= _512K);
    if (offBuf >= cbBuf)
    {
        Log((IOMMU_LOG_PFX ": Setting CmdBufHeadPtr (%#RX32) to a value that exceeds buffer length (%#RX23) -> Ignored\n",
             offBuf, cbBuf));
        return VINF_SUCCESS;
    }

    /* Update the register. */
    pThis->CmdBufHeadPtr.au32[0] = offBuf;

    iommuAmdCmdThreadWakeUpIfNeeded(pDevIns);

    LogFlow((IOMMU_LOG_PFX ": Set CmdBufHeadPtr to %#RX32\n", offBuf));
    return VINF_SUCCESS;
}


/**
 * Writes the Command Buffer Tail Pointer Register (32-bit).
 */
static VBOXSTRICTRC iommuAmdCmdBufTailPtr_w(PPDMDEVINS pDevIns, PIOMMU pThis, uint32_t iReg, uint64_t u64Value)
{
    RT_NOREF(pDevIns, iReg);

    /*
     * IOMMU behavior is undefined when software writes a value outside the buffer length.
     * In our emulation, we ignore the write entirely.
     * See AMD IOMMU spec. 3.3.13 "Command and Event Log Pointer Registers".
     */
    uint32_t const offBuf = u64Value & IOMMU_CMD_BUF_TAIL_PTR_VALID_MASK;
    uint32_t const cbBuf  = iommuAmdGetTotalBufLength(pThis->CmdBufBaseAddr.n.u4Len);
    Assert(cbBuf <= _512K);
    if (offBuf >= cbBuf)
    {
        Log((IOMMU_LOG_PFX ": Setting CmdBufTailPtr (%#RX32) to a value that exceeds buffer length (%#RX32) -> Ignored\n",
             offBuf, cbBuf));
        return VINF_SUCCESS;
    }

    /*
     * IOMMU behavior is undefined if software advances the tail pointer equal to or beyond the
     * head pointer after adding one or more commands to the buffer.
     *
     * However, we cannot enforce this strictly because it's legal for software to shrink the
     * command queue (by reducing the offset) as well as wrap around the pointer (when head isn't
     * at 0). Software might even make the queue empty by making head and tail equal which is
     * allowed. I don't think we can or should try too hard to prevent software shooting itself
     * in the foot here. As long as we make sure the offset value is within the circular buffer
     * bounds (which we do by masking bits above) it should be sufficient.
     */
    pThis->CmdBufTailPtr.au32[0] = offBuf;

    iommuAmdCmdThreadWakeUpIfNeeded(pDevIns);

    LogFlow((IOMMU_LOG_PFX ": Set CmdBufTailPtr to %#RX32\n", offBuf));
    return VINF_SUCCESS;
}


/**
 * Writes the Event Log Head Pointer Register (32-bit).
 */
static VBOXSTRICTRC iommuAmdEvtLogHeadPtr_w(PPDMDEVINS pDevIns, PIOMMU pThis, uint32_t iReg, uint64_t u64Value)
{
    RT_NOREF(pDevIns, iReg);

    /*
     * IOMMU behavior is undefined when software writes a value outside the buffer length.
     * In our emulation, we ignore the write entirely.
     * See AMD IOMMU spec. 3.3.13 "Command and Event Log Pointer Registers".
     */
    uint32_t const offBuf = u64Value & IOMMU_EVT_LOG_HEAD_PTR_VALID_MASK;
    uint32_t const cbBuf  = iommuAmdGetTotalBufLength(pThis->EvtLogBaseAddr.n.u4Len);
    Assert(cbBuf <= _512K);
    if (offBuf >= cbBuf)
    {
        Log((IOMMU_LOG_PFX ": Setting EvtLogHeadPtr (%#RX32) to a value that exceeds buffer length (%#RX32) -> Ignored\n",
             offBuf, cbBuf));
        return VINF_SUCCESS;
    }

    /* Update the register. */
    pThis->EvtLogHeadPtr.au32[0] = offBuf;

    LogFlow((IOMMU_LOG_PFX ": Set EvtLogHeadPtr to %#RX32\n", offBuf));
    return VINF_SUCCESS;
}


/**
 * Writes the Event Log Tail Pointer Register (32-bit).
 */
static VBOXSTRICTRC iommuAmdEvtLogTailPtr_w(PPDMDEVINS pDevIns, PIOMMU pThis, uint32_t iReg, uint64_t u64Value)
{
    RT_NOREF(pDevIns, iReg);
    NOREF(pThis);

    /*
     * IOMMU behavior is undefined when software writes this register when the event log is running.
     * In our emulation, we ignore the write entirely.
     * See AMD IOMMU spec. 3.3.13 "Command and Event Log Pointer Registers".
     */
    IOMMU_STATUS_T const Status = iommuAmdGetStatus(pThis);
    if (Status.n.u1EvtLogRunning)
    {
        Log((IOMMU_LOG_PFX ": Setting EvtLogTailPtr (%#RX64) when event log is running -> Ignored\n", u64Value));
        return VINF_SUCCESS;
    }

    /*
     * IOMMU behavior is undefined when software writes a value outside the buffer length.
     * In our emulation, we ignore the write entirely.
     */
    uint32_t const offBuf = u64Value & IOMMU_EVT_LOG_TAIL_PTR_VALID_MASK;
    uint32_t const cbBuf  = iommuAmdGetTotalBufLength(pThis->EvtLogBaseAddr.n.u4Len);
    Assert(cbBuf <= _512K);
    if (offBuf >= cbBuf)
    {
        Log((IOMMU_LOG_PFX ": Setting EvtLogTailPtr (%#RX32) to a value that exceeds buffer length (%#RX32) -> Ignored\n",
             offBuf, cbBuf));
        return VINF_SUCCESS;
    }

    /* Update the register. */
    pThis->EvtLogTailPtr.au32[0] = offBuf;

    LogFlow((IOMMU_LOG_PFX ": Set EvtLogTailPtr to %#RX32\n", offBuf));
    return VINF_SUCCESS;
}


/**
 * Writes the Status Register (64-bit).
 */
static VBOXSTRICTRC iommuAmdStatus_w(PPDMDEVINS pDevIns, PIOMMU pThis, uint32_t iReg, uint64_t u64Value)
{
    RT_NOREF(pDevIns, iReg);

    /* Mask out all unrecognized bits. */
    u64Value &= IOMMU_STATUS_VALID_MASK;

    /*
     * Compute RW1C (read-only, write-1-to-clear) bits and preserve the rest (which are read-only).
     * Writing 0 to an RW1C bit has no effect. Writing 1 to an RW1C bit, clears the bit if it's already 1.
     */
    IOMMU_STATUS_T const OldStatus = iommuAmdGetStatus(pThis);
    uint64_t const fOldRw1cBits = (OldStatus.u64 &  IOMMU_STATUS_RW1C_MASK);
    uint64_t const fOldRoBits   = (OldStatus.u64 & ~IOMMU_STATUS_RW1C_MASK);
    uint64_t const fNewRw1cBits = (u64Value      &  IOMMU_STATUS_RW1C_MASK);

    uint64_t const uNewStatus = (fOldRw1cBits & ~fNewRw1cBits) | fOldRoBits;

    /* Update the register. */
    ASMAtomicWriteU64(&pThis->Status.u64, uNewStatus);
    return VINF_SUCCESS;
}


#if 0
/**
 * Table 0: Registers-access table.
 */
static const IOMMUREGACC g_aTable0Regs[] =
{

};

/**
 * Table 1: Registers-access table.
 */
static const IOMMUREGACC g_aTable1Regs[] =
{
};
#endif


/**
 * Writes an IOMMU register (32-bit and 64-bit).
 *
 * @returns Strict VBox status code.
 * @param   pDevIns     The IOMMU device instance.
 * @param   off         MMIO byte offset to the register.
 * @param   cb          The size of the write access.
 * @param   uValue      The value being written.
 *
 * @thread  EMT.
 */
static VBOXSTRICTRC iommuAmdWriteRegister(PPDMDEVINS pDevIns, uint32_t off, uint8_t cb, uint64_t uValue)
{
    Assert(off < IOMMU_MMIO_REGION_SIZE);
    Assert(cb == 4 || cb == 8);
    Assert(!(off & (cb - 1)));

    PIOMMU pThis = PDMDEVINS_2_DATA(pDevIns, PIOMMU);
    switch (off)
    {
        case IOMMU_MMIO_OFF_DEV_TAB_BAR:         return iommuAmdDevTabBar_w(pDevIns, pThis, off, uValue);
        case IOMMU_MMIO_OFF_CMD_BUF_BAR:         return iommuAmdCmdBufBar_w(pDevIns, pThis, off, uValue);
        case IOMMU_MMIO_OFF_EVT_LOG_BAR:         return iommuAmdEvtLogBar_w(pDevIns, pThis, off, uValue);
        case IOMMU_MMIO_OFF_CTRL:                return iommuAmdCtrl_w(pDevIns, pThis, off, uValue);
        case IOMMU_MMIO_OFF_EXCL_BAR:            return iommuAmdExclRangeBar_w(pDevIns, pThis, off, uValue);
        case IOMMU_MMIO_OFF_EXCL_RANGE_LIMIT:    return iommuAmdExclRangeLimit_w(pDevIns, pThis, off, uValue);
        case IOMMU_MMIO_OFF_EXT_FEAT:            return iommuAmdIgnore_w(pDevIns, pThis, off, uValue);

        case IOMMU_MMIO_OFF_PPR_LOG_BAR:         return iommuAmdIgnore_w(pDevIns, pThis, off, uValue);
        case IOMMU_MMIO_OFF_HW_EVT_HI:           return iommuAmdHwEvtHi_w(pDevIns, pThis, off, uValue);
        case IOMMU_MMIO_OFF_HW_EVT_LO:           return iommuAmdHwEvtLo_w(pDevIns, pThis, off, uValue);
        case IOMMU_MMIO_OFF_HW_EVT_STATUS:       return iommuAmdHwEvtStatus_w(pDevIns, pThis, off, uValue);

        case IOMMU_MMIO_OFF_GALOG_BAR:
        case IOMMU_MMIO_OFF_GALOG_TAIL_ADDR:     return iommuAmdIgnore_w(pDevIns, pThis, off, uValue);

        case IOMMU_MMIO_OFF_PPR_LOG_B_BAR:
        case IOMMU_MMIO_OFF_PPR_EVT_B_BAR:       return iommuAmdIgnore_w(pDevIns, pThis, off, uValue);

        case IOMMU_MMIO_OFF_DEV_TAB_SEG_1:
        case IOMMU_MMIO_OFF_DEV_TAB_SEG_2:
        case IOMMU_MMIO_OFF_DEV_TAB_SEG_3:
        case IOMMU_MMIO_OFF_DEV_TAB_SEG_4:
        case IOMMU_MMIO_OFF_DEV_TAB_SEG_5:
        case IOMMU_MMIO_OFF_DEV_TAB_SEG_6:
        case IOMMU_MMIO_OFF_DEV_TAB_SEG_7:       return iommuAmdDevTabSegBar_w(pDevIns, pThis, off, uValue);

        case IOMMU_MMIO_OFF_DEV_SPECIFIC_FEAT:
        case IOMMU_MMIO_OFF_DEV_SPECIFIC_CTRL:
        case IOMMU_MMIO_OFF_DEV_SPECIFIC_STATUS: return iommuAmdIgnore_w(pDevIns, pThis, off, uValue);

        case IOMMU_MMIO_OFF_MSI_VECTOR_0:
        case IOMMU_MMIO_OFF_MSI_VECTOR_1:        return iommuAmdIgnore_w(pDevIns, pThis, off, uValue);
        case IOMMU_MMIO_OFF_MSI_CAP_HDR:
        {
            VBOXSTRICTRC rcStrict = iommuAmdMsiCapHdr_w(pDevIns, pThis, off, (uint32_t)uValue);
            if (cb == 4 || RT_FAILURE(rcStrict))
                return rcStrict;
            uValue >>= 32;
            RT_FALL_THRU();
        }
        case IOMMU_MMIO_OFF_MSI_ADDR_LO:         return iommuAmdMsiAddrLo_w(pDevIns, pThis, off, uValue);
        case IOMMU_MMIO_OFF_MSI_ADDR_HI:
        {
            VBOXSTRICTRC rcStrict = iommuAmdMsiAddrHi_w(pDevIns, pThis, off, (uint32_t)uValue);
            if (cb == 4 || RT_FAILURE(rcStrict))
                return rcStrict;
            uValue >>= 32;
            RT_FALL_THRU();
        }
        case IOMMU_MMIO_OFF_MSI_DATA:            return iommuAmdMsiData_w(pDevIns, pThis, off, uValue);
        case IOMMU_MMIO_OFF_MSI_MAPPING_CAP_HDR: return iommuAmdIgnore_w(pDevIns, pThis, off, uValue);

        case IOMMU_MMIO_OFF_PERF_OPT_CTRL:       return iommuAmdIgnore_w(pDevIns, pThis, off, uValue);

        case IOMMU_MMIO_OFF_XT_GEN_INTR_CTRL:
        case IOMMU_MMIO_OFF_XT_PPR_INTR_CTRL:
        case IOMMU_MMIO_OFF_XT_GALOG_INT_CTRL:   return iommuAmdIgnore_w(pDevIns, pThis, off, uValue);

        case IOMMU_MMIO_OFF_MARC_APER_BAR_0:
        case IOMMU_MMIO_OFF_MARC_APER_RELOC_0:
        case IOMMU_MMIO_OFF_MARC_APER_LEN_0:
        case IOMMU_MMIO_OFF_MARC_APER_BAR_1:
        case IOMMU_MMIO_OFF_MARC_APER_RELOC_1:
        case IOMMU_MMIO_OFF_MARC_APER_LEN_1:
        case IOMMU_MMIO_OFF_MARC_APER_BAR_2:
        case IOMMU_MMIO_OFF_MARC_APER_RELOC_2:
        case IOMMU_MMIO_OFF_MARC_APER_LEN_2:
        case IOMMU_MMIO_OFF_MARC_APER_BAR_3:
        case IOMMU_MMIO_OFF_MARC_APER_RELOC_3:
        case IOMMU_MMIO_OFF_MARC_APER_LEN_3:     return iommuAmdIgnore_w(pDevIns, pThis, off, uValue);

        case IOMMU_MMIO_OFF_RSVD_REG:            return iommuAmdIgnore_w(pDevIns, pThis, off, uValue);

        case IOMMU_MMIO_CMD_BUF_HEAD_PTR:        return iommuAmdCmdBufHeadPtr_w(pDevIns, pThis, off, uValue);
        case IOMMU_MMIO_CMD_BUF_TAIL_PTR:        return iommuAmdCmdBufTailPtr_w(pDevIns, pThis, off, uValue);
        case IOMMU_MMIO_EVT_LOG_HEAD_PTR:        return iommuAmdEvtLogHeadPtr_w(pDevIns, pThis, off, uValue);
        case IOMMU_MMIO_EVT_LOG_TAIL_PTR:        return iommuAmdEvtLogTailPtr_w(pDevIns, pThis, off, uValue);

        case IOMMU_MMIO_OFF_STATUS:              return iommuAmdStatus_w(pDevIns, pThis, off, uValue);

        case IOMMU_MMIO_OFF_PPR_LOG_HEAD_PTR:
        case IOMMU_MMIO_OFF_PPR_LOG_TAIL_PTR:

        case IOMMU_MMIO_OFF_GALOG_HEAD_PTR:
        case IOMMU_MMIO_OFF_GALOG_TAIL_PTR:

        case IOMMU_MMIO_OFF_PPR_LOG_B_HEAD_PTR:
        case IOMMU_MMIO_OFF_PPR_LOG_B_TAIL_PTR:

        case IOMMU_MMIO_OFF_EVT_LOG_B_HEAD_PTR:
        case IOMMU_MMIO_OFF_EVT_LOG_B_TAIL_PTR:  return iommuAmdIgnore_w(pDevIns, pThis, off, uValue);

        case IOMMU_MMIO_OFF_PPR_LOG_AUTO_RESP:
        case IOMMU_MMIO_OFF_PPR_LOG_OVERFLOW_EARLY:
        case IOMMU_MMIO_OFF_PPR_LOG_B_OVERFLOW_EARLY:

        /* Not implemented. */
        case IOMMU_MMIO_OFF_SMI_FLT_FIRST:
        case IOMMU_MMIO_OFF_SMI_FLT_LAST:
        {
            Log((IOMMU_LOG_PFX ": Writing unsupported register: SMI filter %u -> Ignored\n",
                 (off - IOMMU_MMIO_OFF_SMI_FLT_FIRST) >> 3));
            return VINF_SUCCESS;
        }

        /* Unknown. */
        default:
        {
            Log((IOMMU_LOG_PFX ": Writing unknown register %u (%#x) with %#RX64 -> Ignored\n", off, off, uValue));
            return VINF_SUCCESS;
        }
    }
}


/**
 * Reads an IOMMU register (64-bit) given its MMIO offset.
 *
 * All reads are 64-bit but reads to 32-bit registers that are aligned on an 8-byte
 * boundary include the lower half of the subsequent register.
 *
 * This is because most registers are 64-bit and aligned on 8-byte boundaries but
 * some are really 32-bit registers aligned on an 8-byte boundary. We cannot assume
 * software will only perform 32-bit reads on those 32-bit registers that are
 * aligned on 8-byte boundaries.
 *
 * @returns Strict VBox status code.
 * @param   pDevIns     The IOMMU device instance.
 * @param   off         The MMIO offset of the register in bytes.
 * @param   puResult    Where to store the value being read.
 *
 * @thread  EMT.
 */
static VBOXSTRICTRC iommuAmdReadRegister(PPDMDEVINS pDevIns, uint32_t off, uint64_t *puResult)
{
    Assert(off < IOMMU_MMIO_REGION_SIZE);
    Assert(!(off & 7) || !(off & 3));

    PIOMMU      pThis = PDMDEVINS_2_DATA(pDevIns, PIOMMU);
    PCPDMPCIDEV pPciDev = pDevIns->apPciDevs[0];
    PDMPCIDEV_ASSERT_VALID(pDevIns, pPciDev);

    /** @todo IOMMU: fine-grained locking? */
    uint64_t uReg;
    switch (off)
    {
        case IOMMU_MMIO_OFF_DEV_TAB_BAR:              uReg = pThis->aDevTabBaseAddrs[0].u64;    break;
        case IOMMU_MMIO_OFF_CMD_BUF_BAR:              uReg = pThis->CmdBufBaseAddr.u64;         break;
        case IOMMU_MMIO_OFF_EVT_LOG_BAR:              uReg = pThis->EvtLogBaseAddr.u64;         break;
        case IOMMU_MMIO_OFF_CTRL:                     uReg = pThis->Ctrl.u64;                   break;
        case IOMMU_MMIO_OFF_EXCL_BAR:                 uReg = pThis->ExclRangeBaseAddr.u64;      break;
        case IOMMU_MMIO_OFF_EXCL_RANGE_LIMIT:         uReg = pThis->ExclRangeLimit.u64;         break;
        case IOMMU_MMIO_OFF_EXT_FEAT:                 uReg = pThis->ExtFeat.u64;                break;

        case IOMMU_MMIO_OFF_PPR_LOG_BAR:              uReg = pThis->PprLogBaseAddr.u64;         break;
        case IOMMU_MMIO_OFF_HW_EVT_HI:                uReg = pThis->HwEvtHi.u64;                break;
        case IOMMU_MMIO_OFF_HW_EVT_LO:                uReg = pThis->HwEvtLo;                    break;
        case IOMMU_MMIO_OFF_HW_EVT_STATUS:            uReg = pThis->HwEvtStatus.u64;            break;

        case IOMMU_MMIO_OFF_GALOG_BAR:                uReg = pThis->GALogBaseAddr.u64;          break;
        case IOMMU_MMIO_OFF_GALOG_TAIL_ADDR:          uReg = pThis->GALogTailAddr.u64;          break;

        case IOMMU_MMIO_OFF_PPR_LOG_B_BAR:            uReg = pThis->PprLogBBaseAddr.u64;        break;
        case IOMMU_MMIO_OFF_PPR_EVT_B_BAR:            uReg = pThis->EvtLogBBaseAddr.u64;        break;

        case IOMMU_MMIO_OFF_DEV_TAB_SEG_1:
        case IOMMU_MMIO_OFF_DEV_TAB_SEG_2:
        case IOMMU_MMIO_OFF_DEV_TAB_SEG_3:
        case IOMMU_MMIO_OFF_DEV_TAB_SEG_4:
        case IOMMU_MMIO_OFF_DEV_TAB_SEG_5:
        case IOMMU_MMIO_OFF_DEV_TAB_SEG_6:
        case IOMMU_MMIO_OFF_DEV_TAB_SEG_7:
        {
            uint8_t const offDevTabSeg = (off - IOMMU_MMIO_OFF_DEV_TAB_SEG_FIRST) >> 3;
            uint8_t const idxDevTabSeg = offDevTabSeg + 1;
            Assert(idxDevTabSeg < RT_ELEMENTS(pThis->aDevTabBaseAddrs));
            uReg = pThis->aDevTabBaseAddrs[idxDevTabSeg].u64;
            break;
        }

        case IOMMU_MMIO_OFF_DEV_SPECIFIC_FEAT:        uReg = pThis->DevSpecificFeat.u64;        break;
        case IOMMU_MMIO_OFF_DEV_SPECIFIC_CTRL:        uReg = pThis->DevSpecificCtrl.u64;        break;
        case IOMMU_MMIO_OFF_DEV_SPECIFIC_STATUS:      uReg = pThis->DevSpecificStatus.u64;      break;

        case IOMMU_MMIO_OFF_MSI_VECTOR_0:             uReg = pThis->MsiMiscInfo.u64;            break;
        case IOMMU_MMIO_OFF_MSI_VECTOR_1:             uReg = pThis->MsiMiscInfo.au32[1];        break;
        case IOMMU_MMIO_OFF_MSI_CAP_HDR:
        {
            uint32_t const uMsiCapHdr = PDMPciDevGetDWord(pPciDev, IOMMU_PCI_OFF_MSI_CAP_HDR);
            uint32_t const uMsiAddrLo = PDMPciDevGetDWord(pPciDev, IOMMU_PCI_OFF_MSI_ADDR_LO);
            uReg = RT_MAKE_U64(uMsiCapHdr, uMsiAddrLo);
            break;
        }
        case IOMMU_MMIO_OFF_MSI_ADDR_LO:
        {
            uReg = PDMPciDevGetDWord(pPciDev, IOMMU_PCI_OFF_MSI_ADDR_LO);
            break;
        }
        case IOMMU_MMIO_OFF_MSI_ADDR_HI:
        {
            uint32_t const uMsiAddrHi = PDMPciDevGetDWord(pPciDev, IOMMU_PCI_OFF_MSI_ADDR_HI);
            uint32_t const uMsiData   = PDMPciDevGetDWord(pPciDev, IOMMU_PCI_OFF_MSI_DATA);
            uReg = RT_MAKE_U64(uMsiAddrHi, uMsiData);
            break;
        }
        case IOMMU_MMIO_OFF_MSI_DATA:
        {
            uReg = PDMPciDevGetDWord(pPciDev, IOMMU_PCI_OFF_MSI_DATA);
            break;
        }
        case IOMMU_MMIO_OFF_MSI_MAPPING_CAP_HDR:
        {
            /*
             * The PCI spec. lists MSI Mapping Capability 08H as related to HyperTransport capability.
             * The AMD IOMMU spec. fails to mention it explicitly and lists values for this register as
             * though HyperTransport is supported. We don't support HyperTransport, we thus just return
             * 0 for this register.
             */
            uReg = RT_MAKE_U64(0, pThis->PerfOptCtrl.u32);
            break;
        }

        case IOMMU_MMIO_OFF_PERF_OPT_CTRL:            uReg = pThis->PerfOptCtrl.u32;            break;

        case IOMMU_MMIO_OFF_XT_GEN_INTR_CTRL:         uReg = pThis->XtGenIntrCtrl.u64;          break;
        case IOMMU_MMIO_OFF_XT_PPR_INTR_CTRL:         uReg = pThis->XtPprIntrCtrl.u64;          break;
        case IOMMU_MMIO_OFF_XT_GALOG_INT_CTRL:        uReg = pThis->XtGALogIntrCtrl.u64;        break;

        case IOMMU_MMIO_OFF_MARC_APER_BAR_0:          uReg = pThis->aMarcApers[0].Base.u64;     break;
        case IOMMU_MMIO_OFF_MARC_APER_RELOC_0:        uReg = pThis->aMarcApers[0].Reloc.u64;    break;
        case IOMMU_MMIO_OFF_MARC_APER_LEN_0:          uReg = pThis->aMarcApers[0].Length.u64;   break;
        case IOMMU_MMIO_OFF_MARC_APER_BAR_1:          uReg = pThis->aMarcApers[1].Base.u64;     break;
        case IOMMU_MMIO_OFF_MARC_APER_RELOC_1:        uReg = pThis->aMarcApers[1].Reloc.u64;    break;
        case IOMMU_MMIO_OFF_MARC_APER_LEN_1:          uReg = pThis->aMarcApers[1].Length.u64;   break;
        case IOMMU_MMIO_OFF_MARC_APER_BAR_2:          uReg = pThis->aMarcApers[2].Base.u64;     break;
        case IOMMU_MMIO_OFF_MARC_APER_RELOC_2:        uReg = pThis->aMarcApers[2].Reloc.u64;    break;
        case IOMMU_MMIO_OFF_MARC_APER_LEN_2:          uReg = pThis->aMarcApers[2].Length.u64;   break;
        case IOMMU_MMIO_OFF_MARC_APER_BAR_3:          uReg = pThis->aMarcApers[3].Base.u64;     break;
        case IOMMU_MMIO_OFF_MARC_APER_RELOC_3:        uReg = pThis->aMarcApers[3].Reloc.u64;    break;
        case IOMMU_MMIO_OFF_MARC_APER_LEN_3:          uReg = pThis->aMarcApers[3].Length.u64;   break;

        case IOMMU_MMIO_OFF_RSVD_REG:                 uReg = pThis->RsvdReg;                    break;

        case IOMMU_MMIO_CMD_BUF_HEAD_PTR:             uReg = pThis->CmdBufHeadPtr.u64;          break;
        case IOMMU_MMIO_CMD_BUF_TAIL_PTR:             uReg = pThis->CmdBufTailPtr.u64;          break;
        case IOMMU_MMIO_EVT_LOG_HEAD_PTR:             uReg = pThis->EvtLogHeadPtr.u64;          break;
        case IOMMU_MMIO_EVT_LOG_TAIL_PTR:             uReg = pThis->EvtLogTailPtr.u64;          break;

        case IOMMU_MMIO_OFF_STATUS:                   uReg = pThis->Status.u64;                 break;

        case IOMMU_MMIO_OFF_PPR_LOG_HEAD_PTR:         uReg = pThis->PprLogHeadPtr.u64;          break;
        case IOMMU_MMIO_OFF_PPR_LOG_TAIL_PTR:         uReg = pThis->PprLogTailPtr.u64;          break;

        case IOMMU_MMIO_OFF_GALOG_HEAD_PTR:           uReg = pThis->GALogHeadPtr.u64;           break;
        case IOMMU_MMIO_OFF_GALOG_TAIL_PTR:           uReg = pThis->GALogTailPtr.u64;           break;

        case IOMMU_MMIO_OFF_PPR_LOG_B_HEAD_PTR:       uReg = pThis->PprLogBHeadPtr.u64;         break;
        case IOMMU_MMIO_OFF_PPR_LOG_B_TAIL_PTR:       uReg = pThis->PprLogBTailPtr.u64;         break;

        case IOMMU_MMIO_OFF_EVT_LOG_B_HEAD_PTR:       uReg = pThis->EvtLogBHeadPtr.u64;         break;
        case IOMMU_MMIO_OFF_EVT_LOG_B_TAIL_PTR:       uReg = pThis->EvtLogBTailPtr.u64;         break;

        case IOMMU_MMIO_OFF_PPR_LOG_AUTO_RESP:        uReg = pThis->PprLogAutoResp.u64;         break;
        case IOMMU_MMIO_OFF_PPR_LOG_OVERFLOW_EARLY:   uReg = pThis->PprLogOverflowEarly.u64;    break;
        case IOMMU_MMIO_OFF_PPR_LOG_B_OVERFLOW_EARLY: uReg = pThis->PprLogBOverflowEarly.u64;   break;

        /* Not implemented. */
        case IOMMU_MMIO_OFF_SMI_FLT_FIRST:
        case IOMMU_MMIO_OFF_SMI_FLT_LAST:
        {
            Log((IOMMU_LOG_PFX ": Reading unsupported register: SMI filter %u\n", (off - IOMMU_MMIO_OFF_SMI_FLT_FIRST) >> 3));
            uReg = 0;
            break;
        }

        /* Unknown. */
        default:
        {
            Log((IOMMU_LOG_PFX ": Reading unknown register %u (%#x) -> 0\n", off, off));
            uReg = 0;
            return VINF_IOM_MMIO_UNUSED_00;
        }
    }

    *puResult = uReg;
    return VINF_SUCCESS;
}


/**
 * Raises the MSI interrupt for the IOMMU device.
 *
 * @param   pDevIns     The IOMMU device instance.
 *
 * @thread  Any.
 */
static void iommuAmdRaiseMsiInterrupt(PPDMDEVINS pDevIns)
{
    if (iommuAmdIsMsiEnabled(pDevIns))
        PDMDevHlpPCISetIrq(pDevIns, 0, PDM_IRQ_LEVEL_HIGH);
}


/**
 * Clears the MSI interrupt for the IOMMU device.
 *
 * @param   pDevIns     The IOMMU device instance.
 *
 * @thread  Any.
 */
static void iommuAmdClearMsiInterrupt(PPDMDEVINS pDevIns)
{
    if (iommuAmdIsMsiEnabled(pDevIns))
        PDMDevHlpPCISetIrq(pDevIns, 0, PDM_IRQ_LEVEL_LOW);
}


/**
 * Writes an entry to the event log in memory.
 *
 * @returns VBox status code.
 * @param   pDevIns     The IOMMU device instance.
 * @param   pEvent      The event to log.
 *
 * @thread  Any.
 */
static int iommuAmdWriteEvtLogEntry(PPDMDEVINS pDevIns, PCEVT_GENERIC_T pEvent)
{
    PIOMMU pThis = PDMDEVINS_2_DATA(pDevIns, PIOMMU);

    IOMMU_ASSERT_LOCKED(pDevIns);

    /* Check if event logging is active and the log has not overflowed. */
    IOMMU_STATUS_T const Status = iommuAmdGetStatus(pThis);
    if (   Status.n.u1EvtLogRunning
        && !Status.n.u1EvtOverflow)
    {
        uint32_t const cbEvt = sizeof(*pEvent);

        /* Get the offset we need to write the event to in memory (circular buffer offset). */
        uint32_t const offEvt = pThis->EvtLogTailPtr.n.off;
        Assert(!(offEvt & ~IOMMU_EVT_LOG_TAIL_PTR_VALID_MASK));

        /* Ensure we have space in the event log. */
        uint32_t const cMaxEvts = iommuAmdGetBufMaxEntries(pThis->EvtLogBaseAddr.n.u4Len);
        uint32_t const cEvts    = iommuAmdGetEvtLogEntryCount(pThis);
        if (cEvts + 1 < cMaxEvts)
        {
            /* Write the event log entry to memory. */
            RTGCPHYS const GCPhysEvtLog      = pThis->EvtLogBaseAddr.n.u40Base << X86_PAGE_4K_SHIFT;
            RTGCPHYS const GCPhysEvtLogEntry = GCPhysEvtLog + offEvt;
            int rc = PDMDevHlpPCIPhysWrite(pDevIns, GCPhysEvtLogEntry, pEvent, cbEvt);
            if (RT_FAILURE(rc))
                Log((IOMMU_LOG_PFX ": Failed to write event log entry at %#RGp. rc=%Rrc\n", GCPhysEvtLogEntry, rc));

            /* Increment the event log tail pointer. */
            uint32_t const cbEvtLog = iommuAmdGetTotalBufLength(pThis->EvtLogBaseAddr.n.u4Len);
            pThis->EvtLogTailPtr.n.off = (offEvt + cbEvt) % cbEvtLog;

            /* Indicate that an event log entry was written. */
            ASMAtomicOrU64(&pThis->Status.u64, IOMMU_STATUS_EVT_LOG_INTR);

            /* Check and signal an interrupt if software wants to receive one when an event log entry is written. */
            IOMMU_CTRL_T const Ctrl = iommuAmdGetCtrl(pThis);
            if (Ctrl.n.u1EvtIntrEn)
                iommuAmdRaiseMsiInterrupt(pDevIns);
        }
        else
        {
            /* Indicate that the event log has overflowed. */
            ASMAtomicOrU64(&pThis->Status.u64, IOMMU_STATUS_EVT_LOG_OVERFLOW);

            /* Check and signal an interrupt if software wants to receive one when the event log has overflowed. */
            IOMMU_CTRL_T const Ctrl = iommuAmdGetCtrl(pThis);
            if (Ctrl.n.u1EvtIntrEn)
                iommuAmdRaiseMsiInterrupt(pDevIns);
        }
    }
}


/**
 * Sets an event in the hardware error registers.
 *
 * @param   pDevIns     The IOMMU device instance.
 * @param   pEvent      The event.
 *
 * @thread  Any.
 */
static void iommuAmdSetHwError(PPDMDEVINS pDevIns, PCEVT_GENERIC_T pEvent)
{
    IOMMU_ASSERT_LOCKED(pDevIns);

    PIOMMU pThis = PDMDEVINS_2_DATA(pDevIns, PIOMMU);
    if (pThis->ExtFeat.n.u1HwErrorSup)
    {
        if (pThis->HwEvtStatus.n.u1Valid)
            pThis->HwEvtStatus.n.u1Overflow = 1;
        pThis->HwEvtStatus.n.u1Valid = 1;
        pThis->HwEvtHi.u64 = RT_MAKE_U64(pEvent->au32[0], pEvent->au32[1]);
        pThis->HwEvtLo     = RT_MAKE_U64(pEvent->au32[2], pEvent->au32[3]);
        Assert(pThis->HwEvtHi.n.u4EvtCode == IOMMU_EVT_DEV_TAB_HW_ERROR);
    }
}


/**
 * Initializes a PAGE_TAB_HARDWARE_ERROR event.
 *
 * @param   uDevId              The device ID.
 * @param   uDomainId           The domain ID.
 * @param   GCPhysPtEntity      The system physical address of the page table
 *                              entity.
 * @param   enmOp               The IOMMU operation being performed.
 * @param   pEvtPageTabHwErr    Where to store the initialized event.
 */
static void iommuAmdInitPageTabHwErrorEvent(uint16_t uDevId, uint16_t uDomainId, RTGCPHYS GCPhysPtEntity, IOMMUOP enmOp,
                                            PEVT_PAGE_TAB_HW_ERR_T pEvtPageTabHwErr)
{
    memset(pEvtPageTabHwErr, 0, sizeof(*pEvtPageTabHwErr));
    pEvtPageTabHwErr->n.u16DevId           = uDevId;
    pEvtPageTabHwErr->n.u16DomainOrPasidLo = uDomainId;
    pEvtPageTabHwErr->n.u1GuestOrNested    = 0;
    pEvtPageTabHwErr->n.u1Interrupt        = RT_BOOL(enmOp == IOMMUOP_INTR_REQ);
    pEvtPageTabHwErr->n.u1ReadWrite        = RT_BOOL(enmOp == IOMMUOP_MEM_WRITE);
    pEvtPageTabHwErr->n.u1Translation      = RT_BOOL(enmOp == IOMMUOP_TRANSLATE_REQ);
    pEvtPageTabHwErr->n.u2Type             = enmOp == IOMMUOP_CMD ? HWEVTTYPE_DATA_ERROR : HWEVTTYPE_TARGET_ABORT;
    pEvtPageTabHwErr->n.u4EvtCode          = IOMMU_EVT_PAGE_TAB_HW_ERROR;
    pEvtPageTabHwErr->n.u64Addr            = GCPhysPtEntity;
}


/**
 * Raises a PAGE_TAB_HARDWARE_ERROR event.
 *
 * @param   pDevIns             The IOMMU device instance.
 * @param   enmOp               The IOMMU operation being performed.
 * @param   pEvtPageTabHwErr    The page table hardware error event.
 *
 * @thread  Any.
 */
static void iommuAmdRaisePageTabHwErrorEvent(PPDMDEVINS pDevIns, IOMMUOP enmOp, PEVT_PAGE_TAB_HW_ERR_T pEvtPageTabHwErr)
{
    AssertCompile(sizeof(EVT_GENERIC_T) == sizeof(EVT_PAGE_TAB_HW_ERR_T));
    PCEVT_GENERIC_T pEvent = (PCEVT_GENERIC_T)pEvtPageTabHwErr;

    IOMMU_LOCK_NORET(pDevIns);

    iommuAmdSetHwError(pDevIns, (PCEVT_GENERIC_T)pEvent);
    iommuAmdWriteEvtLogEntry(pDevIns, (PCEVT_GENERIC_T)pEvent);
    if (enmOp != IOMMUOP_CMD)
        iommuAmdSetPciTargetAbort(pDevIns);

    IOMMU_UNLOCK(pDevIns);

    Log((IOMMU_LOG_PFX ": Raised PAGE_TAB_HARDWARE_ERROR. uDevId=%#x uDomainId=%#x GCPhysPtEntity=%#RGp enmOp=%u u2Type=%u\n",
         pEvtPageTabHwErr->n.u16DevId, pEvtPageTabHwErr->n.u16DomainOrPasidLo, pEvtPageTabHwErr->n.u64Addr, enmOp,
         pEvtPageTabHwErr->n.u2Type));
}


/**
 * Initializes a COMMAND_HARDWARE_ERROR event.
 *
 * @param   GCPhysAddr      The system physical address the IOMMU attempted to access.
 * @param   pEvtCmdHwErr    Where to store the initialized event.
 */
static void iommuAmdInitCmdHwErrorEvent(RTGCPHYS GCPhysAddr, PEVT_CMD_HW_ERR_T pEvtCmdHwErr)
{
    memset(pEvtCmdHwErr, 0, sizeof(*pEvtCmdHwErr));
    pEvtCmdHwErr->n.u2Type    = HWEVTTYPE_DATA_ERROR;
    pEvtCmdHwErr->n.u4EvtCode = IOMMU_EVT_COMMAND_HW_ERROR;
    pEvtCmdHwErr->n.u64Addr   = GCPhysAddr;
}


/**
 * Raises a COMMAND_HARDWARE_ERROR event.
 *
 * @param   pDevIns         The IOMMU device instance.
 * @param   pEvtCmdHwErr    The command hardware error event.
 *
 * @thread  Any.
 */
static void iommuAmdRaiseCmdHwErrorEvent(PPDMDEVINS pDevIns, PCEVT_CMD_HW_ERR_T pEvtCmdHwErr)
{
    AssertCompile(sizeof(EVT_GENERIC_T) == sizeof(EVT_CMD_HW_ERR_T));
    PCEVT_GENERIC_T pEvent = (PCEVT_GENERIC_T)pEvtCmdHwErr;
    PIOMMU pThis = PDMDEVINS_2_DATA(pDevIns, PIOMMU);

    IOMMU_LOCK_NORET(pDevIns);

    iommuAmdSetHwError(pDevIns, (PCEVT_GENERIC_T)pEvent);
    iommuAmdWriteEvtLogEntry(pDevIns, (PCEVT_GENERIC_T)pEvent);
    ASMAtomicAndU64(&pThis->Status.u64, ~IOMMU_STATUS_CMD_BUF_RUNNING);

    IOMMU_UNLOCK(pDevIns);

    Log((IOMMU_LOG_PFX ": Raised COMMAND_HARDWARE_ERROR. GCPhysCmd=%#RGp u2Type=%u\n", pEvtCmdHwErr->n.u64Addr,
         pEvtCmdHwErr->n.u2Type));
}


/**
 * Initializes a DEV_TAB_HARDWARE_ERROR event.
 *
 * @param   uDevId              The device ID.
 * @param   GCPhysDte           The system physical address of the failed device table
 *                              access.
 * @param   enmOp               The IOMMU operation being performed.
 * @param   pEvtDevTabHwErr     Where to store the initialized event.
 */
static void iommuAmdInitDevTabHwErrorEvent(uint16_t uDevId, RTGCPHYS GCPhysDte, IOMMUOP enmOp,
                                           PEVT_DEV_TAB_HW_ERROR_T pEvtDevTabHwErr)
{
    memset(pEvtDevTabHwErr, 0, sizeof(*pEvtDevTabHwErr));
    pEvtDevTabHwErr->n.u16DevId      = uDevId;
    pEvtDevTabHwErr->n.u1Intr        = RT_BOOL(enmOp == IOMMUOP_INTR_REQ);
    /** @todo IOMMU: Any other transaction type that can set read/write bit? */
    pEvtDevTabHwErr->n.u1ReadWrite   = RT_BOOL(enmOp == IOMMUOP_MEM_WRITE);
    pEvtDevTabHwErr->n.u1Translation = RT_BOOL(enmOp == IOMMUOP_TRANSLATE_REQ);
    pEvtDevTabHwErr->n.u2Type        = enmOp == IOMMUOP_CMD ? HWEVTTYPE_DATA_ERROR : HWEVTTYPE_TARGET_ABORT;
    pEvtDevTabHwErr->n.u4EvtCode     = IOMMU_EVT_DEV_TAB_HW_ERROR;
    pEvtDevTabHwErr->n.u64Addr       = GCPhysDte;
}


/**
 * Raises a DEV_TAB_HARDWARE_ERROR event.
 *
 * @param   pDevIns             The IOMMU device instance.
 * @param   enmOp               The IOMMU operation being performed.
 * @param   pEvtDevTabHwErr     The device table hardware error event.
 *
 * @thread  Any.
 */
static void iommuAmdRaiseDevTabHwErrorEvent(PPDMDEVINS pDevIns, IOMMUOP enmOp, PEVT_DEV_TAB_HW_ERROR_T pEvtDevTabHwErr)
{
    AssertCompile(sizeof(EVT_GENERIC_T) == sizeof(EVT_DEV_TAB_HW_ERROR_T));
    PCEVT_GENERIC_T pEvent = (PCEVT_GENERIC_T)pEvtDevTabHwErr;

    IOMMU_LOCK_NORET(pDevIns);

    iommuAmdSetHwError(pDevIns, (PCEVT_GENERIC_T)pEvent);
    iommuAmdWriteEvtLogEntry(pDevIns, (PCEVT_GENERIC_T)pEvent);
    if (enmOp != IOMMUOP_CMD)
        iommuAmdSetPciTargetAbort(pDevIns);

    IOMMU_UNLOCK(pDevIns);

    Log((IOMMU_LOG_PFX ": Raised DEV_TAB_HARDWARE_ERROR. uDevId=%#x GCPhysDte=%#RGp enmOp=%u u2Type=%u\n",
         pEvtDevTabHwErr->n.u16DevId, pEvtDevTabHwErr->n.u64Addr, enmOp, pEvtDevTabHwErr->n.u2Type));
}


/**
 * Initializes an ILLEGAL_COMMAND_ERROR event.
 *
 * @param   GCPhysCmd       The system physical address of the failed command
 *                          access.
 * @param   pEvtIllegalCmd  Where to store the initialized event.
 */
static void iommuAmdInitIllegalCmdEvent(RTGCPHYS GCPhysCmd, PEVT_ILLEGAL_CMD_ERR_T pEvtIllegalCmd)
{
    Assert(!(GCPhysCmd & UINT64_C(0xf)));
    memset(pEvtIllegalCmd, 0, sizeof(*pEvtIllegalCmd));
    pEvtIllegalCmd->n.u4EvtCode = IOMMU_EVT_ILLEGAL_CMD_ERROR;
    pEvtIllegalCmd->n.u64Addr   = GCPhysCmd;
}


/**
 * Raises an ILLEGAL_COMMAND_ERROR event.
 *
 * @param   pDevIns         The IOMMU device instance.
 * @param   pEvtIllegalCmd  The illegal command error event.
 */
static void iommuAmdRaiseIllegalCmdEvent(PPDMDEVINS pDevIns, PCEVT_ILLEGAL_CMD_ERR_T pEvtIllegalCmd)
{
    AssertCompile(sizeof(EVT_GENERIC_T) == sizeof(EVT_ILLEGAL_DTE_T));
    PCEVT_GENERIC_T pEvent = (PCEVT_GENERIC_T)pEvtIllegalCmd;
    PIOMMU pThis = PDMDEVINS_2_DATA(pDevIns, PIOMMU);

    IOMMU_LOCK_NORET(pDevIns);

    iommuAmdWriteEvtLogEntry(pDevIns, pEvent);
    ASMAtomicAndU64(&pThis->Status.u64, ~IOMMU_STATUS_CMD_BUF_RUNNING);

    IOMMU_UNLOCK(pDevIns);

    Log((IOMMU_LOG_PFX ": Raised ILLEGAL_COMMAND_ERROR. Addr=%#RGp\n", pEvtIllegalCmd->n.u64Addr));
}


/**
 * Initializes an ILLEGAL_DEV_TABLE_ENTRY event.
 *
 * @param   uDevId          The device ID.
 * @param   uIova           The I/O virtual address.
 * @param   fRsvdNotZero    Whether reserved bits are not zero. Pass @c false if the
 *                          event was caused by an invalid level encoding in the
 *                          DTE.
 * @param   enmOp           The IOMMU operation being performed.
 * @param   pEvtIllegalDte  Where to store the initialized event.
 */
static void iommuAmdInitIllegalDteEvent(uint16_t uDevId, uint64_t uIova, bool fRsvdNotZero, IOMMUOP enmOp,
                                        PEVT_ILLEGAL_DTE_T pEvtIllegalDte)
{
    memset(pEvtIllegalDte, 0, sizeof(*pEvtIllegalDte));
    pEvtIllegalDte->n.u16DevId      = uDevId;
    pEvtIllegalDte->n.u1Interrupt   = RT_BOOL(enmOp == IOMMUOP_INTR_REQ);
    pEvtIllegalDte->n.u1ReadWrite   = RT_BOOL(enmOp == IOMMUOP_MEM_WRITE);
    pEvtIllegalDte->n.u1RsvdNotZero = fRsvdNotZero;
    pEvtIllegalDte->n.u1Translation = RT_BOOL(enmOp == IOMMUOP_TRANSLATE_REQ);
    pEvtIllegalDte->n.u4EvtCode     = IOMMU_EVT_ILLEGAL_DEV_TAB_ENTRY;
    pEvtIllegalDte->n.u64Addr       = uIova & ~UINT64_C(0x3);
    /** @todo r=ramshankar: Not sure why the last 2 bits are marked as reserved by the
     *        IOMMU spec here but not for this field for I/O page fault event. */
    Assert(!(uIova & UINT64_C(0x3)));
}


/**
 * Raises an ILLEGAL_DEV_TABLE_ENTRY event.
 *
 * @param   pDevIns         The IOMMU instance data.
 * @param   enmOp           The IOMMU operation being performed.
 * @param   pEvtIllegalDte  The illegal device table entry event.
 * @param   enmEvtType      The illegal device table entry event type.
 *
 * @thread  Any.
 */
static void iommuAmdRaiseIllegalDteEvent(PPDMDEVINS pDevIns, IOMMUOP enmOp, PCEVT_ILLEGAL_DTE_T pEvtIllegalDte,
                                         EVT_ILLEGAL_DTE_TYPE_T enmEvtType)
{
    AssertCompile(sizeof(EVT_GENERIC_T) == sizeof(EVT_ILLEGAL_DTE_T));
    PCEVT_GENERIC_T pEvent = (PCEVT_GENERIC_T)pEvtIllegalDte;

    IOMMU_LOCK_NORET(pDevIns);

    iommuAmdWriteEvtLogEntry(pDevIns, pEvent);
    if (enmOp != IOMMUOP_CMD)
        iommuAmdSetPciTargetAbort(pDevIns);

    IOMMU_UNLOCK(pDevIns);

    Log((IOMMU_LOG_PFX ": Raised ILLEGAL_DTE_EVENT. uDevId=%#x uIova=%#RX64 enmOp=%u enmEvtType=%u\n", pEvtIllegalDte->n.u16DevId,
         pEvtIllegalDte->n.u64Addr, enmOp, enmEvtType));
    NOREF(enmEvtType);
}


/**
 * Initializes an IO_PAGE_FAULT event.
 *
 * @param   uDevId              The device ID.
 * @param   uDomainId           The domain ID.
 * @param   uIova               The I/O virtual address being accessed.
 * @param   fPresent            Transaction to a page marked as present (including
 *                              DTE.V=1) or interrupt marked as remapped
 *                              (IRTE.RemapEn=1).
 * @param   fRsvdNotZero        Whether reserved bits are not zero. Pass @c false if
 *                              the I/O page fault was caused by invalid level
 *                              encoding.
 * @param   fPermDenied         Permission denied for the address being accessed.
 * @param   enmOp               The IOMMU operation being performed.
 * @param   pEvtIoPageFault     Where to store the initialized event.
 */
static void iommuAmdInitIoPageFaultEvent(uint16_t uDevId, uint16_t uDomainId, uint64_t uIova, bool fPresent, bool fRsvdNotZero,
                                         bool fPermDenied, IOMMUOP enmOp, PEVT_IO_PAGE_FAULT_T pEvtIoPageFault)
{
    Assert(!fPermDenied || fPresent);
    memset(pEvtIoPageFault, 0, sizeof(*pEvtIoPageFault));
    pEvtIoPageFault->n.u16DevId            = uDevId;
    //pEvtIoPageFault->n.u4PasidHi         = 0;
    pEvtIoPageFault->n.u16DomainOrPasidLo  = uDomainId;
    //pEvtIoPageFault->n.u1GuestOrNested   = 0;
    //pEvtIoPageFault->n.u1NoExecute       = 0;
    //pEvtIoPageFault->n.u1User            = 0;
    pEvtIoPageFault->n.u1Interrupt         = RT_BOOL(enmOp == IOMMUOP_INTR_REQ);
    pEvtIoPageFault->n.u1Present           = fPresent;
    pEvtIoPageFault->n.u1ReadWrite         = RT_BOOL(enmOp == IOMMUOP_MEM_WRITE);
    pEvtIoPageFault->n.u1PermDenied        = fPermDenied;
    pEvtIoPageFault->n.u1RsvdNotZero       = fRsvdNotZero;
    pEvtIoPageFault->n.u1Translation       = RT_BOOL(enmOp == IOMMUOP_TRANSLATE_REQ);
    pEvtIoPageFault->n.u4EvtCode           = IOMMU_EVT_IO_PAGE_FAULT;
    pEvtIoPageFault->n.u64Addr             = uIova;
}


/**
 * Raises an IO_PAGE_FAULT event.
 *
 * @param   pDevIns             The IOMMU instance data.
 * @param   pDte                The device table entry. Optional, can be NULL
 *                              depending on @a enmOp.
 * @param   pIrte               The interrupt remapping table entry. Optional, can
 *                              be NULL depending on @a enmOp.
 * @param   enmOp               The IOMMU operation being performed.
 * @param   pEvtIoPageFault     The I/O page fault event.
 * @param   enmEvtType          The I/O page fault event type.
 *
 * @thread  Any.
 */
static void iommuAmdRaiseIoPageFaultEvent(PPDMDEVINS pDevIns, PCDTE_T pDte, PCIRTE_T pIrte, IOMMUOP enmOp,
                                          PCEVT_IO_PAGE_FAULT_T pEvtIoPageFault, EVT_IO_PAGE_FAULT_TYPE_T enmEvtType)
{
    AssertCompile(sizeof(EVT_GENERIC_T) == sizeof(EVT_IO_PAGE_FAULT_T));
    PCEVT_GENERIC_T pEvent = (PCEVT_GENERIC_T)pEvtIoPageFault;

    IOMMU_LOCK_NORET(pDevIns);

    bool fSuppressEvtLogging = false;
    if (   enmOp == IOMMUOP_MEM_READ
        || enmOp == IOMMUOP_MEM_WRITE)
    {
        if (   pDte
            && pDte->n.u1Valid)
        {
            fSuppressEvtLogging = pDte->n.u1SuppressAllPfEvents;
            /** @todo IOMMU: Implement DTE.SE bit, i.e. device ID specific I/O page fault
             *        suppression. Perhaps will be possible when we complete IOTLB/cache
             *        handling. */
        }
    }
    else if (enmOp == IOMMUOP_INTR_REQ)
    {
        if (   pDte
            && pDte->n.u1IntrMapValid)
            fSuppressEvtLogging = !pDte->n.u1IgnoreUnmappedIntrs;

        if (   !fSuppressEvtLogging
            && pIrte)
            fSuppressEvtLogging = pIrte->n.u1SuppressPf;
    }
    /* else: Events are never suppressed for commands. */

    switch (enmEvtType)
    {
        case kIoPageFaultType_PermDenied:
        {
            /* Cannot be triggered by a command. */
            Assert(enmOp != IOMMUOP_CMD);
            RT_FALL_THRU();
        }
        case kIoPageFaultType_DteRsvdPagingMode:
        case kIoPageFaultType_PteInvalidPageSize:
        case kIoPageFaultType_PteInvalidLvlEncoding:
        case kIoPageFaultType_SkippedLevelIovaNotZero:
        case kIoPageFaultType_PteRsvdNotZero:
        case kIoPageFaultType_PteValidNotSet:
        case kIoPageFaultType_DteTranslationDisabled:
        case kIoPageFaultType_PasidInvalidRange:
        {
            /*
             * For a translation request, the IOMMU doesn't signal an I/O page fault nor does it
             * create an event log entry. See AMD spec. 2.1.3.2 "I/O Page Faults".
             */
            if (enmOp != IOMMUOP_TRANSLATE_REQ)
            {
                if (!fSuppressEvtLogging)
                    iommuAmdWriteEvtLogEntry(pDevIns, pEvent);
                if (enmOp != IOMMUOP_CMD)
                    iommuAmdSetPciTargetAbort(pDevIns);
            }
            break;
        }

        case kIoPageFaultType_UserSupervisor:
        {
            /* Access is blocked and only creates an event log entry. */
            if (!fSuppressEvtLogging)
                iommuAmdWriteEvtLogEntry(pDevIns, pEvent);
            break;
        }

        case kIoPageFaultType_IrteAddrInvalid:
        case kIoPageFaultType_IrteRsvdNotZero:
        case kIoPageFaultType_IrteRemapEn:
        case kIoPageFaultType_IrteRsvdIntType:
        case kIoPageFaultType_IntrReqAborted:
        case kIoPageFaultType_IntrWithPasid:
        {
            /* Only trigerred by interrupt requests. */
            Assert(enmOp == IOMMUOP_INTR_REQ);
            if (!fSuppressEvtLogging)
                iommuAmdWriteEvtLogEntry(pDevIns, pEvent);
            iommuAmdSetPciTargetAbort(pDevIns);
            break;
        }

        case kIoPageFaultType_SmiFilterMismatch:
        {
            /* Not supported and probably will never be, assert. */
            AssertMsgFailed(("kIoPageFaultType_SmiFilterMismatch - Upstream SMI requests not supported/implemented."));
            break;
        }

        case kIoPageFaultType_DevId_Invalid:
        {
            /* Cannot be triggered by a command. */
            Assert(enmOp != IOMMUOP_CMD);
            Assert(enmOp != IOMMUOP_TRANSLATE_REQ); /** @todo IOMMU: We don't support translation requests yet. */
            if (!fSuppressEvtLogging)
                iommuAmdWriteEvtLogEntry(pDevIns, pEvent);
            if (   enmOp == IOMMUOP_MEM_READ
                || enmOp == IOMMUOP_MEM_WRITE)
                iommuAmdSetPciTargetAbort(pDevIns);
            break;
        }
    }

    IOMMU_UNLOCK(pDevIns);
}


/**
 * Returns whether the I/O virtual address is to be excluded from translation and
 * permission checks.
 *
 * @returns @c true if the DVA is excluded, @c false otherwise.
 * @param   pThis   The IOMMU device state.
 * @param   pDte    The device table entry.
 * @param   uIova   The I/O virtual address.
 *
 * @remarks Ensure the exclusion range is enabled prior to calling this function.
 *
 * @thread  Any.
 */
static bool iommuAmdIsDvaInExclRange(PCIOMMU pThis, PCDTE_T pDte, uint64_t uIova)
{
    /* Ensure the exclusion range is enabled. */
    Assert(pThis->ExclRangeBaseAddr.n.u1ExclEnable);

    /* Check if the IOVA falls within the exclusion range. */
    uint64_t const uIovaExclFirst = pThis->ExclRangeBaseAddr.n.u40ExclRangeBase << X86_PAGE_4K_SHIFT;
    uint64_t const uIovaExclLast  = pThis->ExclRangeLimit.n.u52ExclLimit;
    if (uIovaExclLast - uIova >= uIovaExclFirst)
    {
        /* Check if device access to addresses in the exclusion range can be forwarded untranslated. */
        if (    pThis->ExclRangeBaseAddr.n.u1AllowAll
            ||  pDte->n.u1AllowExclusion)
            return true;
    }
    return false;
}


/**
 * Reads a device table entry from guest memory given the device ID.
 *
 * @returns VBox status code.
 * @param   pDevIns     The IOMMU device instance.
 * @param   uDevId      The device ID.
 * @param   enmOp       The IOMMU operation being performed.
 * @param   pDte        Where to store the device table entry.
 *
 * @thread  Any.
 */
static int iommuAmdReadDte(PPDMDEVINS pDevIns, uint16_t uDevId, IOMMUOP enmOp, PDTE_T pDte)
{
    PCIOMMU pThis = PDMDEVINS_2_DATA(pDevIns, PIOMMU);
    IOMMU_CTRL_T const Ctrl = iommuAmdGetCtrl(pThis);

    uint8_t const idxSegsEn = Ctrl.n.u3DevTabSegEn;
    Assert(idxSegsEn < RT_ELEMENTS(g_auDevTabSegMasks));

    uint8_t const idxSeg = uDevId & g_auDevTabSegMasks[idxSegsEn] >> 13;
    Assert(idxSeg < RT_ELEMENTS(pThis->aDevTabBaseAddrs));

    RTGCPHYS const GCPhysDevTab = pThis->aDevTabBaseAddrs[idxSeg].n.u40Base << X86_PAGE_4K_SHIFT;
    uint16_t const offDte       = uDevId & ~g_auDevTabSegMasks[idxSegsEn];
    RTGCPHYS const GCPhysDte    = GCPhysDevTab + offDte;

    Assert(!(GCPhysDevTab & X86_PAGE_4K_OFFSET_MASK));
    int rc = PDMDevHlpPCIPhysRead(pDevIns, GCPhysDte, pDte, sizeof(*pDte));
    if (RT_FAILURE(rc))
    {
        Log((IOMMU_LOG_PFX ": Failed to read device table entry at %#RGp. rc=%Rrc -> DevTabHwError\n", GCPhysDte, rc));

        EVT_DEV_TAB_HW_ERROR_T EvtDevTabHwErr;
        iommuAmdInitDevTabHwErrorEvent(uDevId, GCPhysDte, enmOp, &EvtDevTabHwErr);
        iommuAmdRaiseDevTabHwErrorEvent(pDevIns, enmOp, &EvtDevTabHwErr);
        return VERR_IOMMU_IPE_1;
    }


    return rc;
}


/**
 * Walks the I/O page table to translate the I/O virtual address to a system
 * physical address.
 *
 * @returns VBox status code.
 * @param   pDevIns         The IOMMU device instance.
 * @param   uIova           The I/O virtual address to translate. Must be 4K aligned.
 * @param   uDevId          The device ID.
 * @param   fAccess         The access permissions (IOMMU_IO_PERM_XXX). This is the
 *                          permissions for the access being made.
 * @param   pDte            The device table entry.
 * @param   enmOp           The IOMMU operation being performed.
 * @param   pWalkResult     Where to store the results of the I/O page walk. This is
 *                          only updated when VINF_SUCCESS is returned.
 *
 * @thread  Any.
 */
static int iommuAmdWalkIoPageTable(PPDMDEVINS pDevIns, uint16_t uDevId, uint64_t uIova, uint8_t fAccess, PCDTE_T pDte,
                                   IOMMUOP enmOp, PIOWALKRESULT pWalkResult)
{
    Assert(pDte->n.u1Valid);
    Assert(!(uIova & X86_PAGE_4K_OFFSET_MASK));

    /* If the translation is not valid, raise an I/O page fault. */
    if (pDte->n.u1TranslationValid)
    { /* likely */ }
    else
    {
        /** @todo r=ramshankar: The AMD IOMMU spec. says page walk is terminated but
         *        doesn't explicitly say whether an I/O page fault is raised. From other
         *        places in the spec. it seems early page walk terminations (starting with
         *        the DTE) return the state computed so far and raises an I/O page fault. So
         *        returning an invalid translation rather than skipping translation. */
        Log((IOMMU_LOG_PFX ": Translation valid bit not set -> IOPF"));
        EVT_IO_PAGE_FAULT_T EvtIoPageFault;
        iommuAmdInitIoPageFaultEvent(uDevId, pDte->n.u16DomainId, uIova, false /* fPresent */, false /* fRsvdNotZero */,
                                     false /* fPermDenied */, enmOp, &EvtIoPageFault);
        iommuAmdRaiseIoPageFaultEvent(pDevIns, pDte, NULL /* pIrte */, enmOp, &EvtIoPageFault,
                                      kIoPageFaultType_DteTranslationDisabled);
        return VERR_IOMMU_ADDR_TRANSLATION_FAILED;
    }

    /* If the root page table level is 0, translation is skipped and access is controlled by the permission bits. */
    uint8_t const uMaxLevel = pDte->n.u3Mode;
    if (uMaxLevel != 0)
    { /* likely */ }
    else
    {
        uint8_t const fDtePerm = (pDte->au64[0] >> IOMMU_IO_PERM_SHIFT) & IOMMU_IO_PERM_MASK;
        if ((fAccess & fDtePerm) != fAccess)
        {
            Log((IOMMU_LOG_PFX ": Access denied for IOVA (%#RX64). fAccess=%#x fDtePerm=%#x\n", uIova, fAccess, fDtePerm));
            return VERR_IOMMU_ADDR_ACCESS_DENIED;
        }
        pWalkResult->GCPhysSpa = uIova;
        pWalkResult->cShift    = 0;
        pWalkResult->fIoPerm   = fDtePerm;
        return VINF_SUCCESS;
    }

    /* If the root page table level exceeds the allowed host-address translation level, page walk is terminated. */
    if (uMaxLevel <= IOMMU_MAX_HOST_PT_LEVEL)
    { /* likely */ }
    else
    {
        /** @todo r=ramshankar: I cannot make out from the AMD IOMMU spec. if I should be
         *        raising an ILLEGAL_DEV_TABLE_ENTRY event or an IO_PAGE_FAULT event here.
         *        I'm just going with I/O page fault. */
        Log((IOMMU_LOG_PFX ": Invalid root page table level %#x -> IOPF", uMaxLevel));
        EVT_IO_PAGE_FAULT_T EvtIoPageFault;
        iommuAmdInitIoPageFaultEvent(uDevId, pDte->n.u16DomainId, uIova, true /* fPresent */, false /* fRsvdNotZero */,
                                     false /* fPermDenied */, enmOp, &EvtIoPageFault);
        iommuAmdRaiseIoPageFaultEvent(pDevIns, pDte, NULL /* pIrte */, enmOp, &EvtIoPageFault,
                                      kIoPageFaultType_PteInvalidLvlEncoding);
        return VERR_IOMMU_ADDR_TRANSLATION_FAILED;
    }

    /* Check permissions bits of the root page table. */
    uint8_t const fPtePerm  = (pDte->au64[0] >> IOMMU_IO_PERM_SHIFT) & IOMMU_IO_PERM_MASK;
    if ((fAccess & fPtePerm) == fAccess)
    { /* likely */ }
    else
    {
        Log((IOMMU_LOG_PFX ": Permission denied (fAccess=%#x fPtePerm=%#x) -> IOPF", fAccess, fPtePerm));
        EVT_IO_PAGE_FAULT_T EvtIoPageFault;
        iommuAmdInitIoPageFaultEvent(uDevId, pDte->n.u16DomainId, uIova, true /* fPresent */, false /* fRsvdNotZero */,
                                     true /* fPermDenied */, enmOp, &EvtIoPageFault);
        iommuAmdRaiseIoPageFaultEvent(pDevIns, pDte, NULL /* pIrte */, enmOp, &EvtIoPageFault, kIoPageFaultType_PermDenied);
        return VERR_IOMMU_ADDR_TRANSLATION_FAILED;
    }

    /** @todo r=ramshankar: IOMMU: Consider splitting the rest of this into a separate
     *        function called iommuAmdWalkIoPageDirectory() and call it for multi-page
     *        accesses from the 2nd page. We can avoid re-checking the DTE root-page
     *        table entry every time. Not sure if it's worth optimizing that case now
     *        or if at all. */

    /* The virtual address bits indexing table. */
    static uint8_t const  s_acIovaLevelShifts[] = { 0, 12, 21, 30, 39, 48, 57, 0 };
    static uint64_t const s_auIovaLevelMasks[]  = { UINT64_C(0x0000000000000000),
                                                    UINT64_C(0x00000000001ff000),
                                                    UINT64_C(0x000000003fe00000),
                                                    UINT64_C(0x0000007fc0000000),
                                                    UINT64_C(0x0000ff8000000000),
                                                    UINT64_C(0x01ff000000000000),
                                                    UINT64_C(0xfe00000000000000),
                                                    UINT64_C(0x0000000000000000) };
    AssertCompile(RT_ELEMENTS(s_acIovaLevelShifts) == RT_ELEMENTS(s_auIovaLevelMasks));
    AssertCompile(RT_ELEMENTS(s_acIovaLevelShifts) > IOMMU_MAX_HOST_PT_LEVEL);

    /* Traverse the I/O page table starting with the page directory in the DTE. */
    IOPTENTITY_T PtEntity;
    PtEntity.u64 = pDte->au64[0];
    for (;;)
    {
        /* Figure out the system physical address of the page table at the current level. */
        uint8_t const uLevel = PtEntity.n.u3NextLevel;

        /* Read the page table entity at the current level. */
        {
            Assert(uLevel > 0 && uLevel < RT_ELEMENTS(s_acIovaLevelShifts));
            Assert(uLevel <= IOMMU_MAX_HOST_PT_LEVEL);
            uint16_t const idxPte         = (uIova >> s_acIovaLevelShifts[uLevel]) & UINT64_C(0x1ff);
            uint64_t const offPte         = idxPte << 3;
            RTGCPHYS const GCPhysPtEntity = (PtEntity.u64 & IOMMU_PTENTITY_ADDR_MASK) + offPte;
            int rc = PDMDevHlpPCIPhysRead(pDevIns, GCPhysPtEntity, &PtEntity.u64, sizeof(PtEntity));
            if (RT_FAILURE(rc))
            {
                Log((IOMMU_LOG_PFX ": Failed to read page table entry at %#RGp. rc=%Rrc -> PageTabHwError\n", GCPhysPtEntity, rc));
                EVT_PAGE_TAB_HW_ERR_T EvtPageTabHwErr;
                iommuAmdInitPageTabHwErrorEvent(uDevId, pDte->n.u16DomainId, GCPhysPtEntity, enmOp, &EvtPageTabHwErr);
                iommuAmdRaisePageTabHwErrorEvent(pDevIns, enmOp, &EvtPageTabHwErr);
                return VERR_IOMMU_IPE_2;
            }
        }

        /* Check present bit. */
        if (PtEntity.n.u1Present)
        { /* likely */ }
        else
        {
            Log((IOMMU_LOG_PFX ": Page table entry not present -> IOPF", fAccess, fPtePerm));
            EVT_IO_PAGE_FAULT_T EvtIoPageFault;
            iommuAmdInitIoPageFaultEvent(uDevId, pDte->n.u16DomainId, uIova, false /* fPresent */, false /* fRsvdNotZero */,
                                         false /* fPermDenied */, enmOp, &EvtIoPageFault);
            iommuAmdRaiseIoPageFaultEvent(pDevIns, pDte, NULL /* pIrte */, enmOp, &EvtIoPageFault, kIoPageFaultType_PermDenied);
            return VERR_IOMMU_ADDR_TRANSLATION_FAILED;
        }

        /* Check permission bits. */
        uint8_t const fPtePerm  = (PtEntity.u64 >> IOMMU_IO_PERM_SHIFT) & IOMMU_IO_PERM_MASK;
        if ((fAccess & fPtePerm) == fAccess)
        { /* likely */ }
        else
        {
            Log((IOMMU_LOG_PFX ": Page table entry permission denied (fAccess=%#x fPtePerm=%#x) -> IOPF", fAccess, fPtePerm));
            EVT_IO_PAGE_FAULT_T EvtIoPageFault;
            iommuAmdInitIoPageFaultEvent(uDevId, pDte->n.u16DomainId, uIova, true /* fPresent */, false /* fRsvdNotZero */,
                                         true /* fPermDenied */, enmOp, &EvtIoPageFault);
            iommuAmdRaiseIoPageFaultEvent(pDevIns, pDte, NULL /* pIrte */, enmOp, &EvtIoPageFault, kIoPageFaultType_PermDenied);
            return VERR_IOMMU_ADDR_TRANSLATION_FAILED;
        }

        /* If this is a PTE, we're at the final level and we're done. */
        uint8_t const uNextLevel = PtEntity.n.u3NextLevel;
        if (uNextLevel == 0)
        {
            /* The page size of the translation is the default (4K). */
            pWalkResult->GCPhysSpa = PtEntity.u64 & IOMMU_PTENTITY_ADDR_MASK;
            pWalkResult->cShift    = X86_PAGE_4K_SHIFT;
            pWalkResult->fIoPerm   = fPtePerm;
            return VINF_SUCCESS;
        }
        if (uNextLevel == 7)
        {
            /* The default page size of the translation is overriden. */
            RTGCPHYS const GCPhysPte = PtEntity.u64 & IOMMU_PTENTITY_ADDR_MASK;
            uint8_t        cShift    = X86_PAGE_4K_SHIFT;
            while (GCPhysPte & RT_BIT_64(cShift++))
                ;

            /* The page size must be larger than the default size and lower than the default size of the higher level. */
            Assert(uLevel < IOMMU_MAX_HOST_PT_LEVEL);   /* PTE at level 6 handled outside the loop, uLevel should be <= 5. */
            if (   cShift > s_acIovaLevelShifts[uLevel]
                && cShift < s_acIovaLevelShifts[uLevel + 1])
            {
                pWalkResult->GCPhysSpa = GCPhysPte;
                pWalkResult->cShift    = cShift;
                pWalkResult->fIoPerm   = fPtePerm;
                return VINF_SUCCESS;
            }

            Log((IOMMU_LOG_PFX ": Page size invalid cShift=%#x -> IOPF", cShift));
            EVT_IO_PAGE_FAULT_T EvtIoPageFault;
            iommuAmdInitIoPageFaultEvent(uDevId, pDte->n.u16DomainId, uIova, true /* fPresent */, false /* fRsvdNotZero */,
                                         false /* fPermDenied */, enmOp, &EvtIoPageFault);
            iommuAmdRaiseIoPageFaultEvent(pDevIns, pDte, NULL /* pIrte */, enmOp, &EvtIoPageFault,
                                          kIoPageFaultType_PteInvalidPageSize);
            return VERR_IOMMU_ADDR_TRANSLATION_FAILED;
        }

        /* Validate the next level encoding of the PDE. */
#if IOMMU_MAX_HOST_PT_LEVEL < 6
        if (uNextLevel <= IOMMU_MAX_HOST_PT_LEVEL)
        { /* likely */ }
        else
        {
            Log((IOMMU_LOG_PFX ": Next level of PDE invalid uNextLevel=%#x -> IOPF", uNextLevel));
            EVT_IO_PAGE_FAULT_T EvtIoPageFault;
            iommuAmdInitIoPageFaultEvent(uDevId, pDte->n.u16DomainId, uIova, true /* fPresent */, false /* fRsvdNotZero */,
                                         false /* fPermDenied */, enmOp, &EvtIoPageFault);
            iommuAmdRaiseIoPageFaultEvent(pDevIns, pDte, NULL /* pIrte */, enmOp, &EvtIoPageFault,
                                          kIoPageFaultType_PteInvalidLvlEncoding);
            return VERR_IOMMU_ADDR_TRANSLATION_FAILED;
        }
#else
        Assert(uNextLevel <= IOMMU_MAX_HOST_PT_LEVEL);
#endif

        /* Validate level transition. */
        if (uNextLevel < uLevel)
        { /* likely */ }
        else
        {
            Log((IOMMU_LOG_PFX ": Next level (%#x) must be less than the current level (%#x) -> IOPF", uNextLevel, uLevel));
            EVT_IO_PAGE_FAULT_T EvtIoPageFault;
            iommuAmdInitIoPageFaultEvent(uDevId, pDte->n.u16DomainId, uIova, true /* fPresent */, false /* fRsvdNotZero */,
                                         false /* fPermDenied */, enmOp, &EvtIoPageFault);
            iommuAmdRaiseIoPageFaultEvent(pDevIns, pDte, NULL /* pIrte */, enmOp, &EvtIoPageFault,
                                          kIoPageFaultType_PteInvalidLvlEncoding);
            return VERR_IOMMU_ADDR_TRANSLATION_FAILED;
        }

        /* Ensure IOVA bits of skipped levels are zero. */
        Assert(uLevel > 0);
        uint64_t uIovaSkipMask = 0;
        for (unsigned idxLevel = uLevel - 1; idxLevel > uNextLevel; idxLevel--)
            uIovaSkipMask |= s_auIovaLevelMasks[idxLevel];
        if (!(uIova & uIovaSkipMask))
        { /* likely */ }
        else
        {
            Log((IOMMU_LOG_PFX ": IOVA of skipped levels are not zero %#RX64 (SkipMask=%#RX64) -> IOPF", uIova, uIovaSkipMask));
            EVT_IO_PAGE_FAULT_T EvtIoPageFault;
            iommuAmdInitIoPageFaultEvent(uDevId, pDte->n.u16DomainId, uIova, true /* fPresent */, false /* fRsvdNotZero */,
                                         false /* fPermDenied */, enmOp, &EvtIoPageFault);
            iommuAmdRaiseIoPageFaultEvent(pDevIns, pDte, NULL /* pIrte */, enmOp, &EvtIoPageFault,
                                          kIoPageFaultType_SkippedLevelIovaNotZero);
            return VERR_IOMMU_ADDR_TRANSLATION_FAILED;
        }

        /* Continue with traversing the page directory at this level. */
    }

    /* Shouldn't really get here. */
    return VERR_IOMMU_IPE_3;
}


/**
 * Looks up an I/O virtual address from the device table.
 *
 * @returns VBox status code.
 * @param   pDevIns     The IOMMU instance data.
 * @param   uDevId      The device ID.
 * @param   uIova       The I/O virtual address to lookup.
 * @param   cbAccess    The size of the access.
 * @param   fAccess     The access permissions (IOMMU_IO_PERM_XXX). This is the
 *                      permissions for the access being made.
 * @param   enmOp       The IOMMU operation being performed.
 * @param   pGCPhysSpa  Where to store the translated system physical address. Only
 *                      valid when translation succeeds and VINF_SUCCESS is
 *                      returned!
 *
 * @thread  Any.
 */
static int iommuAmdLookupDeviceTable(PPDMDEVINS pDevIns, uint16_t uDevId, uint64_t uIova, size_t cbAccess, uint8_t fAccess,
                                     IOMMUOP enmOp, PRTGCPHYS pGCPhysSpa)
{
    PIOMMU pThis = PDMDEVINS_2_DATA(pDevIns, PIOMMU);

    /* Read the device table entry from memory. */
    DTE_T Dte;
    int rc = iommuAmdReadDte(pDevIns, uDevId, enmOp, &Dte);
    if (RT_SUCCESS(rc))
    {
        /* If the DTE is not valid, addresses are forwarded without translation */
        if (Dte.n.u1Valid)
        { /* likely */ }
        else
        {
            /** @todo IOMMU: Add to IOLTB cache. */
            *pGCPhysSpa = uIova;
            return VINF_SUCCESS;
        }

        /* Validate bits 127:0 of the device table entry when DTE.V is 1. */
        uint64_t const fRsvd0 = Dte.au64[0] & ~(IOMMU_DTE_QWORD_0_VALID_MASK & ~IOMMU_DTE_QWORD_0_FEAT_MASK);
        uint64_t const fRsvd1 = Dte.au64[1] & ~(IOMMU_DTE_QWORD_1_VALID_MASK & ~IOMMU_DTE_QWORD_1_FEAT_MASK);
        if (RT_LIKELY(   !fRsvd0
                      && !fRsvd1))
        { /* likely */ }
        else
        {
            Log((IOMMU_LOG_PFX ": Invalid reserved bits in DTE (u64[0]=%#RX64 u64[1]=%#RX64) -> Illegal DTE\n", fRsvd0, fRsvd1));
            EVT_ILLEGAL_DTE_T Event;
            iommuAmdInitIllegalDteEvent(uDevId, uIova, true /* fRsvdNotZero */, enmOp, &Event);
            iommuAmdRaiseIllegalDteEvent(pDevIns, enmOp, &Event, kIllegalDteType_RsvdNotZero);
            return VERR_IOMMU_ADDR_TRANSLATION_FAILED;
        }

        /* If the IOVA is subject to address exclusion, addresses are forwarded without translation. */
        if (   !pThis->ExclRangeBaseAddr.n.u1ExclEnable
            || !iommuAmdIsDvaInExclRange(pThis, &Dte, uIova))
        { /* likely */ }
        else
        {
            /** @todo IOMMU: Add to IOLTB cache. */
            *pGCPhysSpa = uIova;
            return VINF_SUCCESS;
        }

        /** @todo IOMMU: Perhaps do the <= 4K access case first, if the generic loop
         *        below gets too expensive and when we have iommuAmdWalkIoPageDirectory. */

        uint64_t uBaseIova   = uIova & X86_PAGE_4K_BASE_MASK;
        uint64_t offIova     = uIova & X86_PAGE_4K_OFFSET_MASK;
        uint64_t cbRemaining = cbAccess;
        for (;;)
        {
            /* Walk the I/O page tables to translate the IOVA and check permission for the access. */
            IOWALKRESULT WalkResult;
            rc = iommuAmdWalkIoPageTable(pDevIns, uDevId, uBaseIova, fAccess, &Dte, enmOp, &WalkResult);
            if (RT_SUCCESS(rc))
            {
                /** @todo IOMMU: Split large pages into 4K IOTLB entries and add to IOTLB cache. */

                /* Store the translated base address before continuing to check permissions for any more pages. */
                if (cbRemaining == cbAccess)
                {
                    RTGCPHYS const offSpa = ~(UINT64_C(0xffffffffffffffff) << WalkResult.cShift);
                    *pGCPhysSpa = WalkResult.GCPhysSpa | offSpa;
                }

                uint64_t const cbPhysPage = UINT64_C(1) << WalkResult.cShift;
                if (cbRemaining > cbPhysPage - offIova)
                {
                    cbRemaining -= (cbPhysPage - offIova);
                    uBaseIova   += cbPhysPage;
                    offIova      = 0;
                }
                else
                    break;
            }
            else
            {
                Log((IOMMU_LOG_PFX ": I/O page table walk failed. uIova=%#RX64 uBaseIova=%#RX64 fAccess=%u rc=%Rrc\n", uIova,
                     uBaseIova, fAccess, rc));
                *pGCPhysSpa = NIL_RTGCPHYS;
                return rc;
            }
        }

        return rc;
    }

    Log((IOMMU_LOG_PFX ": Failed to read device table entry. uDevId=%#x rc=%Rrc\n", uDevId, rc));
    return VERR_IOMMU_ADDR_TRANSLATION_FAILED;
}


/**
 * Memory read request from a device.
 *
 * @returns VBox status code.
 * @param   pDevIns     The IOMMU device instance.
 * @param   uDevId      The device ID (bus, device, function).
 * @param   uIova       The I/O virtual address being read.
 * @param   cbRead      The number of bytes being read.
 * @param   pGCPhysSpa  Where to store the translated system physical address.
 *
 * @thread  Any.
 */
static int iommuAmdDeviceMemRead(PPDMDEVINS pDevIns, uint16_t uDevId, uint64_t uIova, size_t cbRead, PRTGCPHYS pGCPhysSpa)
{
    /* Validate. */
    Assert(pDevIns);
    Assert(pGCPhysSpa);
    Assert(cbRead > 0);

    PIOMMU pThis = PDMDEVINS_2_DATA(pDevIns, PIOMMU);

    /* Addresses are forwarded without translation when the IOMMU is disabled. */
    IOMMU_CTRL_T const Ctrl = iommuAmdGetCtrl(pThis);
    if (Ctrl.n.u1IommuEn)
    {
        /** @todo IOMMU: IOTLB cache lookup. */

        /* Lookup the IOVA from the device table. */
        return iommuAmdLookupDeviceTable(pDevIns, uDevId, uIova, cbRead, IOMMU_IO_PERM_READ, IOMMUOP_MEM_READ, pGCPhysSpa);
    }

    *pGCPhysSpa = uIova;
    return VINF_SUCCESS;
}


/**
 * Memory write request from a device.
 *
 * @returns VBox status code.
 * @param   pDevIns     The IOMMU device instance.
 * @param   uDevId      The device ID (bus, device, function).
 * @param   uIova       The I/O virtual address being written.
 * @param   cbWrite     The number of bytes being written.
 * @param   pGCPhysSpa  Where to store the translated physical address.
 *
 * @thread  Any.
 */
static int iommuAmdDeviceMemWrite(PPDMDEVINS pDevIns, uint16_t uDevId, uint64_t uIova, size_t cbWrite, PRTGCPHYS pGCPhysSpa)
{
    /* Validate. */
    Assert(pDevIns);
    Assert(pGCPhysSpa);
    Assert(cbWrite > 0);

    PIOMMU pThis = PDMDEVINS_2_DATA(pDevIns, PIOMMU);

    /* Addresses are forwarded without translation when the IOMMU is disabled. */
    IOMMU_CTRL_T const Ctrl = iommuAmdGetCtrl(pThis);
    if (Ctrl.n.u1IommuEn)
    {
        /** @todo IOMMU: IOTLB cache lookup. */

        /* Lookup the IOVA from the device table. */
        return iommuAmdLookupDeviceTable(pDevIns, uDevId, uIova, cbWrite, IOMMU_IO_PERM_WRITE, IOMMUOP_MEM_WRITE, pGCPhysSpa);
    }

    *pGCPhysSpa = uIova;
    return VINF_SUCCESS;
}


/**
 * Reads an interrupt remapping table entry from guest memory given its DTE.
 *
 * @returns VBox status code.
 * @param   pDevIns     The IOMMU device instance.
 * @param   uDevId      The device ID.
 * @param   pDte        The device table entry.
 * @param   GCPhysIn    The source MSI address.
 * @param   uDataIn     The source MSI data.
 * @param   enmOp       The IOMMU operation being performed.
 * @param   pIrte       Where to store the interrupt remapping table entry.
 *
 * @thread  Any.
 */
static int iommuAmdReadIrte(PPDMDEVINS pDevIns, uint16_t uDevId, PCDTE_T pDte, RTGCPHYS GCPhysIn, uint32_t uDataIn,
                            IOMMUOP enmOp, PIRTE_T pIrte)
{
    RTGCPHYS const GCPhysIntrTable = pDte->au64[2] & IOMMU_DTE_IRTE_ROOT_PTR_MASK;
    uint16_t const offIrte         = (uDataIn & IOMMU_MSI_DATA_IRTE_OFFSET_MASK) << IOMMU_IRTE_SIZE_SHIFT;
    RTGCPHYS const GCPhysIrte      = GCPhysIntrTable + offIrte;

    /* Ensure the IRTE offset is within the specified table size. */
    Assert(pDte->n.u4IntrTableLength < 12);
    if (offIrte + sizeof(IRTE_T) <= (1 << pDte->n.u4IntrTableLength) << IOMMU_IRTE_SIZE_SHIFT)
    { /* likely */ }
    else
    {
        EVT_IO_PAGE_FAULT_T EvtIoPageFault;
        iommuAmdInitIoPageFaultEvent(uDevId, pDte->n.u16DomainId, GCPhysIn, false /* fPresent */, false /* fRsvdNotZero */,
                                     false /* fPermDenied */, enmOp, &EvtIoPageFault);
        iommuAmdRaiseIoPageFaultEvent(pDevIns, pDte, NULL /* pIrte */, enmOp, &EvtIoPageFault,
                                      kIoPageFaultType_IrteAddrInvalid);
        return VERR_IOMMU_ADDR_TRANSLATION_FAILED;
    }

    /* Read the IRTE from memory. */
    Assert(!(GCPhysIrte & 3));
    int rc = PDMDevHlpPCIPhysRead(pDevIns, GCPhysIrte, pIrte, sizeof(*pIrte));
    if (RT_SUCCESS(rc))
        return VINF_SUCCESS;

    /** @todo The IOMMU spec. does not tell what kind of error is reported in this
     *        situation. Is it an I/O page fault or a device table hardware error?
     *        There's no interrupt table hardware error event, but it's unclear what
     *        we should do here. */
    Log((IOMMU_LOG_PFX ": Failed to read interrupt table entry at %#RGp. rc=%Rrc -> ???\n", GCPhysIrte, rc));
    return VERR_IOMMU_IPE_4;
}


/**
 * Remap the interrupt using the interrupt remapping table.
 *
 * @returns VBox status code.
 * @param   pDevIns     The IOMMU instance data.
 * @param   uDevId      The device ID.
 * @param   pDte        The device table entry.
 * @param   enmOp       The IOMMU operation being performed.
 * @param   pMsiIn      The source MSI.
 * @param   pMsiOut     Where to store the remapped MSI.
 *
 * @thread  Any.
 */
static int iommuAmdRemapIntr(PPDMDEVINS pDevIns, uint16_t uDevId, PCDTE_T pDte, IOMMUOP enmOp, PCMSIMSG pMsiIn,
                             PMSIMSG pMsiOut)
{
    Assert(pDte->n.u2IntrCtrl == IOMMU_INTR_CTRL_REMAP);

    IRTE_T Irte;
    int rc = iommuAmdReadIrte(pDevIns, uDevId, pDte, pMsiIn->Addr.u64, pMsiIn->Data.u32, enmOp, &Irte);
    if (RT_SUCCESS(rc))
    {
        if (Irte.n.u1RemapEnable)
        {
            if (!Irte.n.u1GuestMode)
            {
                if (Irte.n.u3IntrType < VBOX_MSI_DELIVERY_MODE_LOWEST_PRIO)
                {
                    /* Preserve all bits from the source MSI address that don't map 1:1 from the IRTE. */
                    pMsiOut->Addr.u64 = pMsiIn->Addr.u64;
                    pMsiOut->Addr.n.u1DestMode = Irte.n.u1DestMode;
                    pMsiOut->Addr.n.u8DestId   = Irte.n.u8Dest;

                    /* Preserve all bits from the source MSI data that don't map 1:1 from the IRTE. */
                    pMsiOut->Data.u32 = pMsiIn->Data.u32;
                    pMsiOut->Data.n.u8Vector       = Irte.n.u8Vector;
                    pMsiOut->Data.n.u3DeliveryMode = Irte.n.u3IntrType;

                    return VINF_SUCCESS;
                }

                Log((IOMMU_LOG_PFX ": Interrupt type (%#x) invalid -> IOPF", Irte.n.u3IntrType));
                EVT_IO_PAGE_FAULT_T EvtIoPageFault;
                iommuAmdInitIoPageFaultEvent(uDevId, pDte->n.u16DomainId, pMsiIn->Addr.u64, Irte.n.u1RemapEnable,
                                             true /* fRsvdNotZero */, false /* fPermDenied */, enmOp, &EvtIoPageFault);
                iommuAmdRaiseIoPageFaultEvent(pDevIns, pDte, &Irte, enmOp, &EvtIoPageFault, kIoPageFaultType_IrteRsvdIntType);
                return VERR_IOMMU_ADDR_TRANSLATION_FAILED;
            }

            Log((IOMMU_LOG_PFX ": Guest mode not supported -> IOPF"));
            EVT_IO_PAGE_FAULT_T EvtIoPageFault;
            iommuAmdInitIoPageFaultEvent(uDevId, pDte->n.u16DomainId, pMsiIn->Addr.u64, Irte.n.u1RemapEnable,
                                         true /* fRsvdNotZero */, false /* fPermDenied */, enmOp, &EvtIoPageFault);
            iommuAmdRaiseIoPageFaultEvent(pDevIns, pDte, &Irte, enmOp, &EvtIoPageFault, kIoPageFaultType_IrteRsvdNotZero);
            return VERR_IOMMU_ADDR_TRANSLATION_FAILED;
        }

        Log((IOMMU_LOG_PFX ": Remapping disabled -> IOPF"));
        EVT_IO_PAGE_FAULT_T EvtIoPageFault;
        iommuAmdInitIoPageFaultEvent(uDevId, pDte->n.u16DomainId, pMsiIn->Addr.u64, Irte.n.u1RemapEnable,
                                     false /* fRsvdNotZero */, false /* fPermDenied */, enmOp, &EvtIoPageFault);
        iommuAmdRaiseIoPageFaultEvent(pDevIns, pDte, &Irte, enmOp, &EvtIoPageFault, kIoPageFaultType_IrteRemapEn);
        return VERR_IOMMU_ADDR_TRANSLATION_FAILED;
    }

    return rc;
}


/**
 * Looks up an MSI interrupt from the interrupt remapping table.
 *
 * @returns VBox status code.
 * @param   pDevIns     The IOMMU instance data.
 * @param   uDevId      The device ID.
 * @param   enmOp       The IOMMU operation being performed.
 * @param   pMsiIn      The source MSI.
 * @param   pMsiOut     Where to store the remapped MSI.
 *
 * @thread  Any.
 */
static int iommuAmdLookupIntrTable(PPDMDEVINS pDevIns, uint16_t uDevId, IOMMUOP enmOp, PCMSIMSG pMsiIn, PMSIMSG pMsiOut)
{
    /* Read the device table entry from memory. */
    DTE_T Dte;
    int rc = iommuAmdReadDte(pDevIns, uDevId, enmOp, &Dte);
    if (RT_SUCCESS(rc))
    {
        /* If the DTE is not valid, all interrupts are forwarded without remapping. */
        if (Dte.n.u1IntrMapValid)
        {
            /* Validate bits 255:128 of the device table entry when DTE.IV is 1. */
            uint64_t const fRsvd0 = Dte.au64[2] & ~IOMMU_DTE_QWORD_2_VALID_MASK;
            uint64_t const fRsvd1 = Dte.au64[3] & ~IOMMU_DTE_QWORD_3_VALID_MASK;
            if (RT_LIKELY(   !fRsvd0
                          && !fRsvd1))
            { /* likely */ }
            else
            {
                Log((IOMMU_LOG_PFX ": Invalid reserved bits in DTE (u64[2]=%#RX64 u64[3]=%#RX64) -> Illegal DTE\n", fRsvd0,
                     fRsvd1));
                EVT_ILLEGAL_DTE_T Event;
                iommuAmdInitIllegalDteEvent(uDevId, pMsiIn->Addr.u64, true /* fRsvdNotZero */, enmOp, &Event);
                iommuAmdRaiseIllegalDteEvent(pDevIns, enmOp, &Event, kIllegalDteType_RsvdNotZero);
                return VERR_IOMMU_INTR_REMAP_FAILED;
            }

            /*
             * LINT0/LINT1 pins cannot be driven by PCI(e) devices. Perhaps for a Southbridge
             * that's connected through HyperTransport it might be possible; but for us, it
             * doesn't seem we need to specially handle these pins.
             */

            /*
             * Validate the MSI source address.
             *
             * 64-bit MSIs are supported by the PCI and AMD IOMMU spec. However as far as the
             * CPU is concerned, the MSI region is fixed and we must ensure no other device
             * claims the region as I/O space.
             *
             * See PCI spec. 6.1.4. "Message Signaled Interrupt (MSI) Support".
             * See AMD IOMMU spec. 2.8 "IOMMU Interrupt Support".
             * See Intel spec. 10.11.1 "Message Address Register Format".
             */
            if ((pMsiIn->Addr.u64 & VBOX_MSI_ADDR_ADDR_MASK) == VBOX_MSI_ADDR_BASE)
            {
                /*
                 * The IOMMU remaps fixed and arbitrated interrupts using the IRTE.
                 * See AMD IOMMU spec. "2.2.5.1 Interrupt Remapping Tables, Guest Virtual APIC Not Enabled".
                 */
                uint8_t const u8DeliveryMode = pMsiIn->Data.n.u3DeliveryMode;
                bool fPassThru = false;
                switch (u8DeliveryMode)
                {
                    case VBOX_MSI_DELIVERY_MODE_FIXED:
                    case VBOX_MSI_DELIVERY_MODE_LOWEST_PRIO:
                    {
                        uint8_t const uIntrCtrl = Dte.n.u2IntrCtrl;
                        if (uIntrCtrl == IOMMU_INTR_CTRL_TARGET_ABORT)
                        {
                            Log((IOMMU_LOG_PFX ": IntCtl=0: Target aborting fixed/arbitrated interrupt -> Target abort\n"));
                            iommuAmdSetPciTargetAbort(pDevIns);
                            return VINF_SUCCESS;
                        }

                        if (uIntrCtrl == IOMMU_INTR_CTRL_FWD_UNMAPPED)
                        {
                            fPassThru = true;
                            break;
                        }

                        if (uIntrCtrl == IOMMU_INTR_CTRL_REMAP)
                        {
                            /* Validate the encoded interrupt table length when IntCtl specifies remapping. */
                            uint32_t const uIntTabLen = Dte.n.u4IntrTableLength;
                            if (Dte.n.u4IntrTableLength < 12)
                            {
                                /*
                                 * We don't support guest interrupt remapping yet. When we do, we'll need to
                                 * check Ctrl.u1GstVirtApicEn and use the guest Virtual APIC Table Root Pointer
                                 * in the DTE rather than the Interrupt Root Table Pointer. Since the caller
                                 * already reads the control register, add that as a parameter when we eventually
                                 * support guest interrupt remapping. For now, just assert.
                                 */
                                PIOMMU pThis = PDMDEVINS_2_DATA(pDevIns, PIOMMU);
                                Assert(!pThis->ExtFeat.n.u1GstVirtApicSup);
                                NOREF(pThis);

                                return iommuAmdRemapIntr(pDevIns, uDevId, &Dte, enmOp, pMsiIn, pMsiOut);
                            }

                            Log((IOMMU_LOG_PFX ": Invalid interrupt table length %#x -> Illegal DTE\n", uIntTabLen));
                            EVT_ILLEGAL_DTE_T Event;
                            iommuAmdInitIllegalDteEvent(uDevId, pMsiIn->Addr.u64, false /* fRsvdNotZero */, enmOp, &Event);
                            iommuAmdRaiseIllegalDteEvent(pDevIns, enmOp, &Event, kIllegalDteType_RsvdIntTabLen);
                            return VERR_IOMMU_INTR_REMAP_FAILED;
                        }

                        /* Paranoia. */
                        Assert(uIntrCtrl == IOMMU_INTR_CTRL_RSVD);

                        Log((IOMMU_LOG_PFX ":IntCtl mode invalid %#x -> Illegal DTE", uIntrCtrl));
                        EVT_ILLEGAL_DTE_T Event;
                        iommuAmdInitIllegalDteEvent(uDevId, pMsiIn->Addr.u64, true /* fRsvdNotZero */, enmOp, &Event);
                        iommuAmdRaiseIllegalDteEvent(pDevIns, enmOp, &Event, kIllegalDteType_RsvdIntCtl);
                        return VERR_IOMMU_INTR_REMAP_FAILED;
                    }

                    /* SMIs are passed through unmapped. We don't implement SMI filters. */
                    case VBOX_MSI_DELIVERY_MODE_SMI:        fPassThru = true;                   break;
                    case VBOX_MSI_DELIVERY_MODE_NMI:        fPassThru = Dte.n.u1NmiPassthru;    break;
                    case VBOX_MSI_DELIVERY_MODE_INIT:       fPassThru = Dte.n.u1InitPassthru;   break;
                    case VBOX_MSI_DELIVERY_MODE_EXT_INT:    fPassThru = Dte.n.u1ExtIntPassthru; break;
                    default:
                    {
                        Log((IOMMU_LOG_PFX ":MSI data delivery mode invalid %#x -> Target abort", u8DeliveryMode));
                        iommuAmdSetPciTargetAbort(pDevIns);
                        return VERR_IOMMU_INTR_REMAP_FAILED;
                    }
                }

                if (fPassThru)
                {
                    *pMsiOut = *pMsiIn;
                    return VINF_SUCCESS;
                }

                iommuAmdSetPciTargetAbort(pDevIns);
                return VERR_IOMMU_INTR_REMAP_FAILED;
            }
            else
            {
                Log((IOMMU_LOG_PFX ":MSI address region invalid %#RX64.", pMsiIn->Addr.u64));
                return VERR_IOMMU_INTR_REMAP_FAILED;
            }
        }
        else
        {
            /** @todo IOMMU: Add to interrupt remapping cache. */
            *pMsiOut = *pMsiIn;
            return VINF_SUCCESS;
        }
    }

    Log((IOMMU_LOG_PFX ": Failed to read device table entry. uDevId=%#x rc=%Rrc\n", uDevId, rc));
    return VERR_IOMMU_INTR_REMAP_FAILED;
}


/**
 * Interrupt remap request from a device.
 *
 * @returns VBox status code.
 * @param   pDevIns     The IOMMU device instance.
 * @param   uDevId      The device ID (bus, device, function).
 * @param   pMsiIn      The source MSI.
 * @param   pMsiOut     Where to store the remapped MSI.
 */
static int iommuAmdDeviceMsiRemap(PPDMDEVINS pDevIns, uint16_t uDevId, PCMSIMSG pMsiIn, PMSIMSG pMsiOut)
{
    /* Validate. */
    Assert(pDevIns);
    Assert(pMsiIn);
    Assert(pMsiOut);

    PIOMMU pThis = PDMDEVINS_2_DATA(pDevIns, PIOMMU);

    /* Interrupts are forwarded with remapping when the IOMMU is disabled. */
    IOMMU_CTRL_T const Ctrl = iommuAmdGetCtrl(pThis);
    if (Ctrl.n.u1IommuEn)
    {
        /** @todo Cache? */

        return iommuAmdLookupIntrTable(pDevIns, uDevId, IOMMUOP_INTR_REQ, pMsiIn, pMsiOut);
    }

    *pMsiOut = *pMsiIn;
    return VINF_SUCCESS;
}


/**
 * @callback_method_impl{FNIOMMMIONEWWRITE}
 */
static DECLCALLBACK(VBOXSTRICTRC) iommuAmdMmioWrite(PPDMDEVINS pDevIns, void *pvUser, RTGCPHYS off, void const *pv, unsigned cb)
{
    NOREF(pvUser);
    Assert(cb == 4 || cb == 8);
    Assert(!(off & (cb - 1)));

    uint64_t const uValue = cb == 8 ? *(uint64_t const *)pv : *(uint32_t const *)pv;
    return iommuAmdWriteRegister(pDevIns, off, cb, uValue);
}


/**
 * @callback_method_impl{FNIOMMMIONEWREAD}
 */
static DECLCALLBACK(VBOXSTRICTRC) iommuAmdMmioRead(PPDMDEVINS pDevIns, void *pvUser, RTGCPHYS off, void *pv, unsigned cb)
{
    NOREF(pvUser);
    Assert(cb == 4 || cb == 8);
    Assert(!(off & (cb - 1)));

    uint64_t uResult;
    VBOXSTRICTRC rcStrict = iommuAmdReadRegister(pDevIns, off, &uResult);
    if (cb == 8)
        *(uint64_t *)pv = uResult;
    else
        *(uint32_t *)pv = (uint32_t)uResult;

    return rcStrict;
}

# ifdef IN_RING3

/**
 * Processes an IOMMU command.
 *
 * @returns VBox status code.
 * @param   pDevIns         The IOMMU device instance.
 * @param   pCmd            The command to process.
 * @param   GCPhysCmd       The system physical address of the command.
 * @param   pEvtError       Where to store the error event in case of failures.
 *
 * @thread  Command thread.
 */
static int iommuAmdR3ProcessCmd(PPDMDEVINS pDevIns, PCCMD_GENERIC_T pCmd, RTGCPHYS GCPhysCmd, PEVT_GENERIC_T pEvtError)
{
    IOMMU_ASSERT_NOT_LOCKED(pDevIns);

    PIOMMU pThis = PDMDEVINS_2_DATA(pDevIns, PIOMMU);
    uint8_t const bCmd = pCmd->n.u4Opcode;
    switch (bCmd)
    {
        case IOMMU_CMD_COMPLETION_WAIT:
        {
            PCCMD_COMWAIT_T pCmdComWait = (PCCMD_COMWAIT_T)pCmd;
            AssertCompile(sizeof(*pCmdComWait) == sizeof(*pCmd));

            /* Validate reserved bits in the command. */
            if (!(pCmdComWait->au64[0] & ~IOMMU_CMD_COM_WAIT_QWORD_0_VALID_MASK))
            {
                /* If Completion Store is requested, write the StoreData to the specified address.*/
                if (pCmdComWait->n.u1Store)
                {
                    RTGCPHYS const GCPhysStore = RT_MAKE_U64(pCmdComWait->n.u29StoreAddrLo << 3, pCmdComWait->n.u20StoreAddrHi);
                    uint64_t const u64Data     = pCmdComWait->n.u64StoreData;
                    int rc = PDMDevHlpPCIPhysWrite(pDevIns, GCPhysStore, &u64Data, sizeof(u64Data));
                    if (RT_FAILURE(rc))
                    {
                        Log((IOMMU_LOG_PFX ": Cmd(%#x): Failed to write StoreData (%#RX64) to %#RGp, rc=%Rrc\n", bCmd, u64Data,
                             GCPhysStore, rc));
                        iommuAmdInitCmdHwErrorEvent(GCPhysStore, (PEVT_CMD_HW_ERR_T)pEvtError);
                        return VERR_IOMMU_CMD_HW_ERROR;
                    }
                }

                /* If command completion interrupt is requested, raise an interrupt. */
                if (pCmdComWait->n.u1Interrupt)
                {
                    IOMMU_LOCK(pDevIns);
                    IOMMU_CTRL_T const Ctrl = iommuAmdGetCtrl(pThis);
                    if (Ctrl.n.u1CompWaitIntrEn)
                    {
                        /* Indicate that this command completed. */
                        ASMAtomicOrU64(&pThis->Status.u64, IOMMU_STATUS_COMPLETION_WAIT_INTR);

                        /* Check and signal an interrupt if software wants to receive one when this command completes. */
                        IOMMU_CTRL_T const Ctrl = iommuAmdGetCtrl(pThis);
                        if (Ctrl.n.u1CompWaitIntrEn)
                            iommuAmdRaiseMsiInterrupt(pDevIns);
                    }
                    IOMMU_UNLOCK(pDevIns);
                }
                return VINF_SUCCESS;
            }
            iommuAmdInitIllegalCmdEvent(GCPhysCmd, (PEVT_ILLEGAL_CMD_ERR_T)pEvtError);
            return VERR_IOMMU_CMD_INVALID_FORMAT;
        }

        case IOMMU_CMD_INV_DEV_TAB_ENTRY:
        {
            /** @todo IOMMU: Implement this once we implement IOTLB. Pretend success until
             *        then. */
            return VINF_SUCCESS;
        }

        case IOMMU_CMD_INV_IOMMU_PAGES:
        {
            /** @todo IOMMU: Implement this once we implement IOTLB. Pretend success until
             *        then. */
            return VINF_SUCCESS;
        }

        case IOMMU_CMD_INV_IOTLB_PAGES:
        {
            uint32_t const uCapHdr = PDMPciDevGetDWord(pDevIns->apPciDevs[0], IOMMU_PCI_OFF_CAP_HDR);
            if (RT_BF_GET(uCapHdr, IOMMU_BF_CAPHDR_IOTLB_SUP))
            {
                /** @todo IOMMU: Implement remote IOTLB invalidation. */
                return VERR_NOT_IMPLEMENTED;
            }
            iommuAmdInitIllegalCmdEvent(GCPhysCmd, (PEVT_ILLEGAL_CMD_ERR_T)pEvtError);
            return VERR_IOMMU_CMD_NOT_SUPPORTED;
        }

        case IOMMU_CMD_INV_INTR_TABLE:
        {
            /** @todo IOMMU: Implement this once we implement IOTLB. Pretend success until
             *        then. */
            return VINF_SUCCESS;
        }

        case IOMMU_CMD_PREFETCH_IOMMU_PAGES:
        {
            if (pThis->ExtFeat.n.u1PrefetchSup)
            {
                /** @todo IOMMU: Implement prefetch. Pretend success until then. */
                return VINF_SUCCESS;
            }
            iommuAmdInitIllegalCmdEvent(GCPhysCmd, (PEVT_ILLEGAL_CMD_ERR_T)pEvtError);
            return VERR_IOMMU_CMD_NOT_SUPPORTED;
        }

        case IOMMU_CMD_COMPLETE_PPR_REQ:
        {
            /* We don't support PPR requests yet. */
            Assert(!pThis->ExtFeat.n.u1PprSup);
            iommuAmdInitIllegalCmdEvent(GCPhysCmd, (PEVT_ILLEGAL_CMD_ERR_T)pEvtError);
            return VERR_IOMMU_CMD_NOT_SUPPORTED;
        }

        case IOMMU_CMD_INV_IOMMU_ALL:
        {
            if (pThis->ExtFeat.n.u1InvAllSup)
            {
                /** @todo IOMMU: Invalidate all. Pretend success until then. */
                return VINF_SUCCESS;
            }
            iommuAmdInitIllegalCmdEvent(GCPhysCmd, (PEVT_ILLEGAL_CMD_ERR_T)pEvtError);
            return VERR_IOMMU_CMD_NOT_SUPPORTED;
        }
    }

    Log((IOMMU_LOG_PFX ": Cmd(%#x): Unrecognized\n", bCmd));
    iommuAmdInitIllegalCmdEvent(GCPhysCmd, (PEVT_ILLEGAL_CMD_ERR_T)pEvtError);
    return VERR_IOMMU_CMD_NOT_SUPPORTED;
}


/**
 * The IOMMU command thread.
 *
 * @returns VBox status code.
 * @param   pDevIns     The IOMMU device instance.
 * @param   pThread     The command thread.
 */
static DECLCALLBACK(int) iommuAmdR3CmdThread(PPDMDEVINS pDevIns, PPDMTHREAD pThread)
{
    PIOMMU pThis = PDMDEVINS_2_DATA(pDevIns, PIOMMU);

    if (pThread->enmState == PDMTHREADSTATE_INITIALIZING)
        return VINF_SUCCESS;

    while (pThread->enmState == PDMTHREADSTATE_RUNNING)
    {
        /*
         * Sleep perpetually until we are woken up to process commands.
         */
        {
            ASMAtomicWriteBool(&pThis->fCmdThreadSleeping, true);
            bool fSignaled = ASMAtomicXchgBool(&pThis->fCmdThreadSignaled, false);
            if (!fSignaled)
            {
                Assert(ASMAtomicReadBool(&pThis->fCmdThreadSleeping));
                int rc = PDMDevHlpSUPSemEventWaitNoResume(pDevIns, pThis->hEvtCmdThread, RT_INDEFINITE_WAIT);
                AssertLogRelMsgReturn(RT_SUCCESS(rc) || rc == VERR_INTERRUPTED, ("%Rrc\n", rc), rc);
                if (RT_UNLIKELY(pThread->enmState != PDMTHREADSTATE_RUNNING))
                    break;
                LogFlowFunc(("Woken up with rc=%Rrc\n", rc));
                ASMAtomicWriteBool(&pThis->fCmdThreadSignaled, false);
            }
            ASMAtomicWriteBool(&pThis->fCmdThreadSleeping, false);
        }

        /*
         * Fetch and process IOMMU commands.
         */
        /** @todo r=ramshankar: This employs a simplistic method of fetching commands (one
         *        at a time) and is expensive due to calls to PGM for fetching guest memory.
         *        We could optimize by fetching a bunch of commands at a time reducing
         *        number of calls to PGM. In the longer run we could lock the memory and
         *        mappings and accessing them directly. */
        IOMMU_LOCK(pDevIns);

        IOMMU_STATUS_T const Status = iommuAmdGetStatus(pThis);
        if (Status.n.u1CmdBufRunning)
        {
            /* Get the offset we need to read the command from memory (circular buffer offset). */
            uint32_t const cbCmdBuf = iommuAmdGetTotalBufLength(pThis->CmdBufBaseAddr.n.u4Len);
            uint32_t offHead = pThis->CmdBufHeadPtr.n.off;
            Assert(!(offHead & ~IOMMU_CMD_BUF_HEAD_PTR_VALID_MASK));
            Assert(offHead < cbCmdBuf);
            while (offHead != pThis->CmdBufTailPtr.n.off)
            {
                /* Read the command from memory. */
                CMD_GENERIC_T Cmd;
                RTGCPHYS const GCPhysCmd = (pThis->CmdBufBaseAddr.n.u40Base << X86_PAGE_4K_SHIFT) + offHead;
                int rc = PDMDevHlpPCIPhysRead(pDevIns, GCPhysCmd, &Cmd, sizeof(Cmd));
                if (RT_SUCCESS(rc))
                {
                    /* Increment the command buffer head pointer. */
                    offHead = (offHead + sizeof(CMD_GENERIC_T)) % cbCmdBuf;
                    pThis->CmdBufHeadPtr.n.off = offHead;

                    /* Process the fetched command. */
                    EVT_GENERIC_T EvtError;
                    IOMMU_UNLOCK(pDevIns);
                    rc = iommuAmdR3ProcessCmd(pDevIns, &Cmd, GCPhysCmd, &EvtError);
                    IOMMU_LOCK(pDevIns);
                    if (RT_FAILURE(rc))
                    {
                        if (   rc == VERR_IOMMU_CMD_NOT_SUPPORTED
                            || rc == VERR_IOMMU_CMD_INVALID_FORMAT)
                        {
                            Assert(EvtError.n.u4EvtCode == IOMMU_EVT_ILLEGAL_CMD_ERROR);
                            iommuAmdRaiseIllegalCmdEvent(pDevIns, (PCEVT_ILLEGAL_CMD_ERR_T)&EvtError);
                        }
                        else if (rc == VERR_IOMMU_CMD_HW_ERROR)
                        {
                            Assert(EvtError.n.u4EvtCode == IOMMU_EVT_COMMAND_HW_ERROR);
                            iommuAmdRaiseCmdHwErrorEvent(pDevIns, (PCEVT_CMD_HW_ERR_T)&EvtError);
                        }
                        break;
                    }
                }
                else
                {
                    EVT_CMD_HW_ERR_T EvtCmdHwErr;
                    iommuAmdInitCmdHwErrorEvent(GCPhysCmd, &EvtCmdHwErr);
                    iommuAmdRaiseCmdHwErrorEvent(pDevIns, &EvtCmdHwErr);
                    break;
                }
            }
        }

        IOMMU_UNLOCK(pDevIns);
    }

    LogFlow((IOMMU_LOG_PFX ": Command thread terminating\n"));
    return VINF_SUCCESS;
}


/**
 * Wakes up the command thread so it can respond to a state change.
 *
 * @returns VBox status code.
 * @param   pDevIns     The IOMMU device instance.
 * @param   pThread     The command thread.
 */
static DECLCALLBACK(int) iommuAmdR3CmdThreadWakeUp(PPDMDEVINS pDevIns, PPDMTHREAD pThread)
{
    RT_NOREF(pThread);

    PIOMMU pThis = PDMDEVINS_2_DATA(pDevIns, PIOMMU);
    return PDMDevHlpSUPSemEventSignal(pDevIns, pThis->hEvtCmdThread);
}


/**
 * @callback_method_impl{FNPCICONFIGREAD}
 */
static DECLCALLBACK(VBOXSTRICTRC) iommuAmdR3PciConfigRead(PPDMDEVINS pDevIns, PPDMPCIDEV pPciDev, uint32_t uAddress,
                                                          unsigned cb, uint32_t *pu32Value)
{
    /** @todo IOMMU: PCI config read stat counter. */
    VBOXSTRICTRC rcStrict = PDMDevHlpPCIConfigRead(pDevIns, pPciDev, uAddress, cb, pu32Value);
    Log3((IOMMU_LOG_PFX ": Reading PCI config register %#x (cb=%u) -> %#x %Rrc\n", uAddress, cb, *pu32Value,
          VBOXSTRICTRC_VAL(rcStrict)));
    return rcStrict;
}


/**
 * @callback_method_impl{FNPCICONFIGWRITE}
 */
static DECLCALLBACK(VBOXSTRICTRC) iommuAmdR3PciConfigWrite(PPDMDEVINS pDevIns, PPDMPCIDEV pPciDev, uint32_t uAddress,
                                                           unsigned cb, uint32_t u32Value)
{
    PIOMMU pThis = PDMDEVINS_2_DATA(pDevIns, PIOMMU);

    /*
     * Discard writes to read-only registers that are specific to the IOMMU.
     * Other common PCI registers are handled by the generic code, see devpciR3IsConfigByteWritable().
     * See PCI spec. 6.1. "Configuration Space Organization".
     */
    switch (uAddress)
    {
        case IOMMU_PCI_OFF_CAP_HDR:         /* All bits are read-only. */
        case IOMMU_PCI_OFF_RANGE_REG:       /* We don't have any devices integrated with the IOMMU. */
        case IOMMU_PCI_OFF_MISCINFO_REG_0:  /* We don't support MSI-X. */
        case IOMMU_PCI_OFF_MISCINFO_REG_1:  /* We don't support guest-address translation. */
        {
            Log((IOMMU_LOG_PFX ": PCI config write (%#RX32) to read-only register %#x -> Ignored\n", u32Value, uAddress));
            return VINF_SUCCESS;
        }
    }

    IOMMU_LOCK(pDevIns);

    VBOXSTRICTRC rcStrict;
    switch (uAddress)
    {
        case IOMMU_PCI_OFF_BASE_ADDR_REG_LO:
        {
            if (pThis->IommuBar.n.u1Enable)
            {
                rcStrict = VINF_SUCCESS;
                Log((IOMMU_LOG_PFX ": Writing Base Address (Lo) when it's already enabled -> Ignored\n"));
                break;
            }

            pThis->IommuBar.au32[0] = u32Value & IOMMU_BAR_VALID_MASK;
            if (pThis->IommuBar.n.u1Enable)
            {
                Assert(pThis->hMmio == NIL_IOMMMIOHANDLE);
                Assert(!pThis->ExtFeat.n.u1PerfCounterSup); /* Base is 16K aligned when performance counters aren't supported. */
                RTGCPHYS const GCPhysMmioBase = RT_MAKE_U64(pThis->IommuBar.au32[0] & 0xffffc000, pThis->IommuBar.au32[1]);
                rcStrict = PDMDevHlpMmioMap(pDevIns, pThis->hMmio, GCPhysMmioBase);
                if (RT_FAILURE(rcStrict))
                    Log((IOMMU_LOG_PFX ": Failed to map IOMMU MMIO region at %#RGp. rc=%Rrc\n", GCPhysMmioBase, rcStrict));
            }
            break;
        }

        case IOMMU_PCI_OFF_BASE_ADDR_REG_HI:
        {
            if (!pThis->IommuBar.n.u1Enable)
                pThis->IommuBar.au32[1] = u32Value;
            else
            {
                rcStrict = VINF_SUCCESS;
                Log((IOMMU_LOG_PFX ": Writing Base Address (Hi) when it's already enabled -> Ignored\n"));
            }
            break;
        }

        case IOMMU_PCI_OFF_MSI_CAP_HDR:
        {
            u32Value |= RT_BIT(23);     /* 64-bit MSI addressess must always be enabled for IOMMU. */
            RT_FALL_THRU();
        }
        default:
        {
            rcStrict = PDMDevHlpPCIConfigWrite(pDevIns, pPciDev, uAddress, cb, u32Value);
            break;
        }
    }

    IOMMU_UNLOCK(pDevIns);

    Log3((IOMMU_LOG_PFX ": PCI config write: %#x -> To %#x (%u) %Rrc\n", u32Value, uAddress, cb, VBOXSTRICTRC_VAL(rcStrict)));
    return rcStrict;
}


/**
 * @callback_method_impl{FNDBGFHANDLERDEV}
 */
static DECLCALLBACK(void) iommuAmdR3DbgInfo(PPDMDEVINS pDevIns, PCDBGFINFOHLP pHlp, const char *pszArgs)
{
    PCIOMMU    pThis   = PDMDEVINS_2_DATA(pDevIns, PIOMMU);
    PCPDMPCIDEV pPciDev = pDevIns->apPciDevs[0];
    PDMPCIDEV_ASSERT_VALID(pDevIns, pPciDev);

    LogFlow((IOMMU_LOG_PFX ": iommuAmdR3DbgInfo: pThis=%p pszArgs=%s\n", pThis, pszArgs));
    bool const fVerbose = !strncmp(pszArgs, RT_STR_TUPLE("verbose")) ? true : false;

    pHlp->pfnPrintf(pHlp, "AMD-IOMMU:\n");
    /* Device Table Base Addresses (all segments). */
    for (unsigned i = 0; i < RT_ELEMENTS(pThis->aDevTabBaseAddrs); i++)
    {
        DEV_TAB_BAR_T const DevTabBar = pThis->aDevTabBaseAddrs[i];
        pHlp->pfnPrintf(pHlp, "  Device Table BAR [%u]                   = %#RX64\n", i, DevTabBar.u64);
        if (fVerbose)
        {
            pHlp->pfnPrintf(pHlp, "    Size                                    = %#x (%u bytes)\n", DevTabBar.n.u9Size,
                            IOMMU_GET_DEV_TAB_SIZE(DevTabBar.n.u9Size));
            pHlp->pfnPrintf(pHlp, "    Base address                            = %#RX64\n", DevTabBar.n.u40Base << X86_PAGE_4K_SHIFT);
        }
    }
    /* Command Buffer Base Address Register. */
    {
        CMD_BUF_BAR_T const CmdBufBar = pThis->CmdBufBaseAddr;
        uint8_t const  uEncodedLen = CmdBufBar.n.u4Len;
        uint32_t const cEntries    = iommuAmdGetBufMaxEntries(uEncodedLen);
        uint32_t const cbBuffer    = iommuAmdGetTotalBufLength(uEncodedLen);
        pHlp->pfnPrintf(pHlp, "  Command buffer BAR                      = %#RX64\n", CmdBufBar.u64);
        if (fVerbose)
        {
            pHlp->pfnPrintf(pHlp, "    Base address                            = %#RX64\n", CmdBufBar.n.u40Base << X86_PAGE_4K_SHIFT);
            pHlp->pfnPrintf(pHlp, "    Length                                  = %u (%u entries, %u bytes)\n", uEncodedLen,
                            cEntries, cbBuffer);
        }
    }
    /* Event Log Base Address Register. */
    {
        EVT_LOG_BAR_T const EvtLogBar = pThis->EvtLogBaseAddr;
        uint8_t const  uEncodedLen = EvtLogBar.n.u4Len;
        uint32_t const cEntries    = iommuAmdGetBufMaxEntries(uEncodedLen);
        uint32_t const cbBuffer    = iommuAmdGetTotalBufLength(uEncodedLen);
        pHlp->pfnPrintf(pHlp, "  Event log BAR                           = %#RX64\n", EvtLogBar.u64);
        if (fVerbose)
        {
            pHlp->pfnPrintf(pHlp, "    Base address                            = %#RX64\n", EvtLogBar.n.u40Base << X86_PAGE_4K_SHIFT);
            pHlp->pfnPrintf(pHlp, "    Length                                  = %u (%u entries, %u bytes)\n", uEncodedLen,
                            cEntries, cbBuffer);
        }
    }
    /* IOMMU Control Register. */
    {
        IOMMU_CTRL_T const Ctrl = pThis->Ctrl;
        pHlp->pfnPrintf(pHlp, "  Control                                 = %#RX64\n", Ctrl.u64);
        if (fVerbose)
        {
            pHlp->pfnPrintf(pHlp, "    IOMMU enable                            = %RTbool\n", Ctrl.n.u1IommuEn);
            pHlp->pfnPrintf(pHlp, "    HT Tunnel translation enable            = %RTbool\n", Ctrl.n.u1HtTunEn);
            pHlp->pfnPrintf(pHlp, "    Event log enable                        = %RTbool\n", Ctrl.n.u1EvtLogEn);
            pHlp->pfnPrintf(pHlp, "    Event log interrupt enable              = %RTbool\n", Ctrl.n.u1EvtIntrEn);
            pHlp->pfnPrintf(pHlp, "    Completion wait interrupt enable        = %RTbool\n", Ctrl.n.u1EvtIntrEn);
            pHlp->pfnPrintf(pHlp, "    Invalidation timeout                    = %u\n",      Ctrl.n.u3InvTimeOut);
            pHlp->pfnPrintf(pHlp, "    Pass posted write                       = %RTbool\n", Ctrl.n.u1PassPW);
            pHlp->pfnPrintf(pHlp, "    Respose Pass posted write               = %RTbool\n", Ctrl.n.u1ResPassPW);
            pHlp->pfnPrintf(pHlp, "    Coherent                                = %RTbool\n", Ctrl.n.u1Coherent);
            pHlp->pfnPrintf(pHlp, "    Isochronous                             = %RTbool\n", Ctrl.n.u1Isoc);
            pHlp->pfnPrintf(pHlp, "    Command buffer enable                   = %RTbool\n", Ctrl.n.u1CmdBufEn);
            pHlp->pfnPrintf(pHlp, "    PPR log enable                          = %RTbool\n", Ctrl.n.u1PprLogEn);
            pHlp->pfnPrintf(pHlp, "    PPR interrupt enable                    = %RTbool\n", Ctrl.n.u1PprIntrEn);
            pHlp->pfnPrintf(pHlp, "    PPR enable                              = %RTbool\n", Ctrl.n.u1PprEn);
            pHlp->pfnPrintf(pHlp, "    Guest translation eanble                = %RTbool\n", Ctrl.n.u1GstTranslateEn);
            pHlp->pfnPrintf(pHlp, "    Guest virtual-APIC enable               = %RTbool\n", Ctrl.n.u1GstVirtApicEn);
            pHlp->pfnPrintf(pHlp, "    CRW                                     = %#x\n",     Ctrl.n.u4Crw);
            pHlp->pfnPrintf(pHlp, "    SMI filter enable                       = %RTbool\n", Ctrl.n.u1SmiFilterEn);
            pHlp->pfnPrintf(pHlp, "    Self-writeback disable                  = %RTbool\n", Ctrl.n.u1SelfWriteBackDis);
            pHlp->pfnPrintf(pHlp, "    SMI filter log enable                   = %RTbool\n", Ctrl.n.u1SmiFilterLogEn);
            pHlp->pfnPrintf(pHlp, "    Guest virtual-APIC mode enable          = %#x\n",     Ctrl.n.u3GstVirtApicModeEn);
            pHlp->pfnPrintf(pHlp, "    Guest virtual-APIC GA log enable        = %RTbool\n", Ctrl.n.u1GstLogEn);
            pHlp->pfnPrintf(pHlp, "    Guest virtual-APIC interrupt enable     = %RTbool\n", Ctrl.n.u1GstIntrEn);
            pHlp->pfnPrintf(pHlp, "    Dual PPR log enable                     = %#x\n",     Ctrl.n.u2DualPprLogEn);
            pHlp->pfnPrintf(pHlp, "    Dual event log enable                   = %#x\n",     Ctrl.n.u2DualEvtLogEn);
            pHlp->pfnPrintf(pHlp, "    Device table segmentation enable        = %#x\n",     Ctrl.n.u3DevTabSegEn);
            pHlp->pfnPrintf(pHlp, "    Privilege abort enable                  = %#x\n",     Ctrl.n.u2PrivAbortEn);
            pHlp->pfnPrintf(pHlp, "    PPR auto response enable                = %RTbool\n", Ctrl.n.u1PprAutoRespEn);
            pHlp->pfnPrintf(pHlp, "    MARC enable                             = %RTbool\n", Ctrl.n.u1MarcEn);
            pHlp->pfnPrintf(pHlp, "    Block StopMark enable                   = %RTbool\n", Ctrl.n.u1BlockStopMarkEn);
            pHlp->pfnPrintf(pHlp, "    PPR auto response always-on enable      = %RTbool\n", Ctrl.n.u1PprAutoRespAlwaysOnEn);
            pHlp->pfnPrintf(pHlp, "    Domain IDPNE                            = %RTbool\n", Ctrl.n.u1DomainIDPNE);
            pHlp->pfnPrintf(pHlp, "    Enhanced PPR handling                   = %RTbool\n", Ctrl.n.u1EnhancedPpr);
            pHlp->pfnPrintf(pHlp, "    Host page table access/dirty bit update = %#x\n",     Ctrl.n.u2HstAccDirtyBitUpdate);
            pHlp->pfnPrintf(pHlp, "    Guest page table dirty bit disable      = %RTbool\n", Ctrl.n.u1GstDirtyUpdateDis);
            pHlp->pfnPrintf(pHlp, "    x2APIC enable                           = %RTbool\n", Ctrl.n.u1X2ApicEn);
            pHlp->pfnPrintf(pHlp, "    x2APIC interrupt enable                 = %RTbool\n", Ctrl.n.u1X2ApicIntrGenEn);
            pHlp->pfnPrintf(pHlp, "    Guest page table access bit update      = %RTbool\n", Ctrl.n.u1GstAccessUpdateDis);
        }
    }
    /* Exclusion Base Address Register. */
    {
        IOMMU_EXCL_RANGE_BAR_T const ExclRangeBar = pThis->ExclRangeBaseAddr;
        pHlp->pfnPrintf(pHlp, "  Exclusion BAR                           = %#RX64\n", ExclRangeBar.u64);
        if (fVerbose)
        {
            pHlp->pfnPrintf(pHlp, "    Exclusion enable                        = %RTbool\n", ExclRangeBar.n.u1ExclEnable);
            pHlp->pfnPrintf(pHlp, "    Allow all devices                       = %RTbool\n", ExclRangeBar.n.u1AllowAll);
            pHlp->pfnPrintf(pHlp, "    Base address                            = %#RX64\n",
                            ExclRangeBar.n.u40ExclRangeBase << X86_PAGE_4K_SHIFT);
        }
    }
    /* Exclusion Range Limit Register. */
    {
        IOMMU_EXCL_RANGE_LIMIT_T const ExclRangeLimit = pThis->ExclRangeLimit;
        pHlp->pfnPrintf(pHlp, "  Exclusion Range Limit                   = %#RX64\n", ExclRangeLimit.u64);
        if (fVerbose)
            pHlp->pfnPrintf(pHlp, "    Range limit                             = %#RX64\n", ExclRangeLimit.n.u52ExclLimit);
    }
    /* Extended Feature Register. */
    {
        IOMMU_EXT_FEAT_T ExtFeat = pThis->ExtFeat;
        pHlp->pfnPrintf(pHlp, "  Extended Feature Register               = %#RX64\n", ExtFeat.u64);
        pHlp->pfnPrintf(pHlp, "    Prefetch support                        = %RTbool\n", ExtFeat.n.u1PrefetchSup);
        if (fVerbose)
        {
            pHlp->pfnPrintf(pHlp, "    PPR support                             = %RTbool\n",  ExtFeat.n.u1PprSup);
            pHlp->pfnPrintf(pHlp, "    x2APIC support                          = %RTbool\n",  ExtFeat.n.u1X2ApicSup);
            pHlp->pfnPrintf(pHlp, "    NX and privilege level support          = %RTbool\n",  ExtFeat.n.u1NoExecuteSup);
            pHlp->pfnPrintf(pHlp, "    Guest translation support               = %RTbool\n",  ExtFeat.n.u1GstTranslateSup);
            pHlp->pfnPrintf(pHlp, "    Invalidate-All command support          = %RTbool\n",  ExtFeat.n.u1InvAllSup);
            pHlp->pfnPrintf(pHlp, "    Guest virtual-APIC support              = %RTbool\n",  ExtFeat.n.u1GstVirtApicSup);
            pHlp->pfnPrintf(pHlp, "    Hardware error register support         = %RTbool\n",  ExtFeat.n.u1HwErrorSup);
            pHlp->pfnPrintf(pHlp, "    Performance counters support            = %RTbool\n",  ExtFeat.n.u1PerfCounterSup);
            pHlp->pfnPrintf(pHlp, "    Host address translation size           = %#x\n",      ExtFeat.n.u2HostAddrTranslateSize);
            pHlp->pfnPrintf(pHlp, "    Guest address translation size          = %#x\n",      ExtFeat.n.u2GstAddrTranslateSize);
            pHlp->pfnPrintf(pHlp, "    Guest CR3 root table level support      = %#x\n",      ExtFeat.n.u2GstCr3RootTblLevel);
            pHlp->pfnPrintf(pHlp, "    SMI filter register support             = %#x\n",      ExtFeat.n.u2SmiFilterSup);
            pHlp->pfnPrintf(pHlp, "    SMI filter register count               = %#x\n",      ExtFeat.n.u3SmiFilterCount);
            pHlp->pfnPrintf(pHlp, "    Guest virtual-APIC modes support        = %#x\n",      ExtFeat.n.u3GstVirtApicModeSup);
            pHlp->pfnPrintf(pHlp, "    Dual PPR log support                    = %#x\n",      ExtFeat.n.u2DualPprLogSup);
            pHlp->pfnPrintf(pHlp, "    Dual event log support                  = %#x\n",      ExtFeat.n.u2DualEvtLogSup);
            pHlp->pfnPrintf(pHlp, "    Maximum PASID                           = %#x\n",      ExtFeat.n.u5MaxPasidSup);
            pHlp->pfnPrintf(pHlp, "    User/supervisor page protection support = %RTbool\n",  ExtFeat.n.u1UserSupervisorSup);
            pHlp->pfnPrintf(pHlp, "    Device table segments supported         = %#x (%u)\n", ExtFeat.n.u2DevTabSegSup,
                            g_acDevTabSegs[ExtFeat.n.u2DevTabSegSup]);
            pHlp->pfnPrintf(pHlp, "    PPR log overflow early warning support  = %RTbool\n",  ExtFeat.n.u1PprLogOverflowWarn);
            pHlp->pfnPrintf(pHlp, "    PPR auto response support               = %RTbool\n",  ExtFeat.n.u1PprAutoRespSup);
            pHlp->pfnPrintf(pHlp, "    MARC support                            = %#x\n",      ExtFeat.n.u2MarcSup);
            pHlp->pfnPrintf(pHlp, "    Block StopMark message support          = %RTbool\n",  ExtFeat.n.u1BlockStopMarkSup);
            pHlp->pfnPrintf(pHlp, "    Performance optimization support        = %RTbool\n",  ExtFeat.n.u1PerfOptSup);
            pHlp->pfnPrintf(pHlp, "    MSI capability MMIO access support      = %RTbool\n",  ExtFeat.n.u1MsiCapMmioSup);
            pHlp->pfnPrintf(pHlp, "    Guest I/O protection support            = %RTbool\n",  ExtFeat.n.u1GstIoSup);
            pHlp->pfnPrintf(pHlp, "    Host access support                     = %RTbool\n",  ExtFeat.n.u1HostAccessSup);
            pHlp->pfnPrintf(pHlp, "    Enhanced PPR handling support           = %RTbool\n",  ExtFeat.n.u1EnhancedPprSup);
            pHlp->pfnPrintf(pHlp, "    Attribute forward supported             = %RTbool\n",  ExtFeat.n.u1AttrForwardSup);
            pHlp->pfnPrintf(pHlp, "    Host dirty support                      = %RTbool\n",  ExtFeat.n.u1HostDirtySup);
            pHlp->pfnPrintf(pHlp, "    Invalidate IOTLB type support           = %RTbool\n",  ExtFeat.n.u1InvIoTlbTypeSup);
            pHlp->pfnPrintf(pHlp, "    Guest page table access bit hw disable  = %RTbool\n",  ExtFeat.n.u1GstUpdateDisSup);
            pHlp->pfnPrintf(pHlp, "    Force physical dest for remapped intr.  = %RTbool\n",  ExtFeat.n.u1ForcePhysDstSup);
        }
    }
    /* PPR Log Base Address Register. */
    {
        PPR_LOG_BAR_T PprLogBar = pThis->PprLogBaseAddr;
        uint8_t const  uEncodedLen = PprLogBar.n.u4Len;
        uint32_t const cEntries    = iommuAmdGetBufMaxEntries(uEncodedLen);
        uint32_t const cbBuffer    = iommuAmdGetTotalBufLength(uEncodedLen);
        pHlp->pfnPrintf(pHlp, "  PPR Log BAR                             = %#RX64\n",   PprLogBar.u64);
        if (fVerbose)
        {
            pHlp->pfnPrintf(pHlp, "    Base address                            = %#RX64\n", PprLogBar.n.u40Base << X86_PAGE_4K_SHIFT);
            pHlp->pfnPrintf(pHlp, "    Length                                  = %u (%u entries, %u bytes)\n", uEncodedLen,
                            cEntries, cbBuffer);
        }
    }
    /* Hardware Event (Hi) Register. */
    {
        IOMMU_HW_EVT_HI_T HwEvtHi = pThis->HwEvtHi;
        pHlp->pfnPrintf(pHlp, "  Hardware Event (Hi)                     = %#RX64\n",   HwEvtHi.u64);
        if (fVerbose)
        {
            pHlp->pfnPrintf(pHlp, "    First operand                           = %#RX64\n", HwEvtHi.n.u60FirstOperand);
            pHlp->pfnPrintf(pHlp, "    Event code                              = %#RX8\n",  HwEvtHi.n.u4EvtCode);
        }
    }
    /* Hardware Event (Lo) Register. */
    pHlp->pfnPrintf(pHlp, "  Hardware Event (Lo)                         = %#RX64\n", pThis->HwEvtLo);
    /* Hardware Event Status. */
    {
        IOMMU_HW_EVT_STATUS_T HwEvtStatus = pThis->HwEvtStatus;
        pHlp->pfnPrintf(pHlp, "  Hardware Event Status                   = %#RX64\n",    HwEvtStatus.u64);
        if (fVerbose)
        {
            pHlp->pfnPrintf(pHlp, "    Valid                                   = %RTbool\n", HwEvtStatus.n.u1Valid);
            pHlp->pfnPrintf(pHlp, "    Overflow                                = %RTbool\n", HwEvtStatus.n.u1Overflow);
        }
    }
    /* Guest Virtual-APIC Log Base Address Register. */
    {
        GALOG_BAR_T const GALogBar = pThis->GALogBaseAddr;
        uint8_t const  uEncodedLen = GALogBar.n.u4Len;
        uint32_t const cEntries    = iommuAmdGetBufMaxEntries(uEncodedLen);
        uint32_t const cbBuffer    = iommuAmdGetTotalBufLength(uEncodedLen);
        pHlp->pfnPrintf(pHlp, "  Guest Log BAR                           = %#RX64\n",    GALogBar.u64);
        if (fVerbose)
        {
            pHlp->pfnPrintf(pHlp, "    Base address                            = %RTbool\n", GALogBar.n.u40Base << X86_PAGE_4K_SHIFT);
            pHlp->pfnPrintf(pHlp, "    Length                                  = %u (%u entries, %u bytes)\n", uEncodedLen,
                            cEntries, cbBuffer);
        }
    }
    /* Guest Virtual-APIC Log Tail Address Register. */
    {
        GALOG_TAIL_ADDR_T GALogTail = pThis->GALogTailAddr;
        pHlp->pfnPrintf(pHlp, "  Guest Log Tail Address                  = %#RX64\n",   GALogTail.u64);
        if (fVerbose)
            pHlp->pfnPrintf(pHlp, "    Tail address                            = %#RX64\n", GALogTail.n.u40GALogTailAddr);
    }
    /* PPR Log B Base Address Register. */
    {
        PPR_LOG_B_BAR_T PprLogBBar = pThis->PprLogBBaseAddr;
        uint8_t const uEncodedLen  = PprLogBBar.n.u4Len;
        uint32_t const cEntries    = iommuAmdGetBufMaxEntries(uEncodedLen);
        uint32_t const cbBuffer    = iommuAmdGetTotalBufLength(uEncodedLen);
        pHlp->pfnPrintf(pHlp, "  PPR Log B BAR                           = %#RX64\n",   PprLogBBar.u64);
        if (fVerbose)
        {
            pHlp->pfnPrintf(pHlp, "    Base address                            = %#RX64\n", PprLogBBar.n.u40Base << X86_PAGE_4K_SHIFT);
            pHlp->pfnPrintf(pHlp, "    Length                                  = %u (%u entries, %u bytes)\n", uEncodedLen,
                            cEntries, cbBuffer);
        }
    }
    /* Event Log B Base Address Register. */
    {
        EVT_LOG_B_BAR_T EvtLogBBar = pThis->EvtLogBBaseAddr;
        uint8_t const  uEncodedLen = EvtLogBBar.n.u4Len;
        uint32_t const cEntries    = iommuAmdGetBufMaxEntries(uEncodedLen);
        uint32_t const cbBuffer    = iommuAmdGetTotalBufLength(uEncodedLen);
        pHlp->pfnPrintf(pHlp, "  Event Log B BAR                         = %#RX64\n",   EvtLogBBar.u64);
        if (fVerbose)
        {
            pHlp->pfnPrintf(pHlp, "    Base address                            = %#RX64\n", EvtLogBBar.n.u40Base << X86_PAGE_4K_SHIFT);
            pHlp->pfnPrintf(pHlp, "    Length                                  = %u (%u entries, %u bytes)\n", uEncodedLen,
                            cEntries, cbBuffer);
        }
    }
    /* Device-Specific Feature Extension Register. */
    {
        DEV_SPECIFIC_FEAT_T const DevSpecificFeat = pThis->DevSpecificFeat;
        pHlp->pfnPrintf(pHlp, "  Device-specific Feature                 = %#RX64\n",   DevSpecificFeat.u64);
        if (fVerbose)
        {
            pHlp->pfnPrintf(pHlp, "    Feature                                 = %#RX32\n", DevSpecificFeat.n.u24DevSpecFeat);
            pHlp->pfnPrintf(pHlp, "    Minor revision ID                       = %#x\n",    DevSpecificFeat.n.u4RevMinor);
            pHlp->pfnPrintf(pHlp, "    Major revision ID                       = %#x\n",    DevSpecificFeat.n.u4RevMajor);
        }
    }
    /* Device-Specific Control Extension Register. */
    {
        DEV_SPECIFIC_CTRL_T const DevSpecificCtrl = pThis->DevSpecificCtrl;
        pHlp->pfnPrintf(pHlp, "  Device-specific Control                 = %#RX64\n",   DevSpecificCtrl.u64);
        if (fVerbose)
        {
            pHlp->pfnPrintf(pHlp, "    Control                                 = %#RX32\n", DevSpecificCtrl.n.u24DevSpecCtrl);
            pHlp->pfnPrintf(pHlp, "    Minor revision ID                       = %#x\n",    DevSpecificCtrl.n.u4RevMinor);
            pHlp->pfnPrintf(pHlp, "    Major revision ID                       = %#x\n",    DevSpecificCtrl.n.u4RevMajor);
        }
    }
    /* Device-Specific Status Extension Register. */
    {
        DEV_SPECIFIC_STATUS_T const DevSpecificStatus = pThis->DevSpecificStatus;
        pHlp->pfnPrintf(pHlp, "  Device-specific Control                 = %#RX64\n",   DevSpecificStatus.u64);
        if (fVerbose)
        {
            pHlp->pfnPrintf(pHlp, "    Status                                  = %#RX32\n", DevSpecificStatus.n.u24DevSpecStatus);
            pHlp->pfnPrintf(pHlp, "    Minor revision ID                       = %#x\n",    DevSpecificStatus.n.u4RevMinor);
            pHlp->pfnPrintf(pHlp, "    Major revision ID                       = %#x\n",    DevSpecificStatus.n.u4RevMajor);
        }
    }
    /* MSI Miscellaneous Information Register (Lo and Hi). */
    {
        MSI_MISC_INFO_T const MsiMiscInfo = pThis->MsiMiscInfo;
        pHlp->pfnPrintf(pHlp, "  MSI Misc. Info. Register                = %#RX64\n",    MsiMiscInfo.u64);
        if (fVerbose)
        {
            pHlp->pfnPrintf(pHlp, "    Event Log MSI number                    = %#x\n",     MsiMiscInfo.n.u5MsiNumEvtLog);
            pHlp->pfnPrintf(pHlp, "    Guest Virtual-Address Size              = %#x\n",     MsiMiscInfo.n.u3GstVirtAddrSize);
            pHlp->pfnPrintf(pHlp, "    Physical Address Size                   = %#x\n",     MsiMiscInfo.n.u7PhysAddrSize);
            pHlp->pfnPrintf(pHlp, "    Virtual-Address Size                    = %#x\n",     MsiMiscInfo.n.u7VirtAddrSize);
            pHlp->pfnPrintf(pHlp, "    HT Transport ATS Range Reserved         = %RTbool\n", MsiMiscInfo.n.u1HtAtsResv);
            pHlp->pfnPrintf(pHlp, "    PPR MSI number                          = %#x\n",     MsiMiscInfo.n.u5MsiNumPpr);
            pHlp->pfnPrintf(pHlp, "    GA Log MSI number                       = %#x\n",     MsiMiscInfo.n.u5MsiNumGa);
        }
    }
    /* MSI Capability Header. */
    {
        MSI_CAP_HDR_T MsiCapHdr;
        MsiCapHdr.u32 = PDMPciDevGetDWord(pPciDev, IOMMU_PCI_OFF_MSI_CAP_HDR);
        pHlp->pfnPrintf(pHlp, "  MSI Capability Header                   = %#RX32\n",    MsiCapHdr.u32);
        if (fVerbose)
        {
            pHlp->pfnPrintf(pHlp, "    Capability ID                           = %#x\n",     MsiCapHdr.n.u8MsiCapId);
            pHlp->pfnPrintf(pHlp, "    Capability Ptr (PCI config offset)      = %#x\n",     MsiCapHdr.n.u8MsiCapPtr);
            pHlp->pfnPrintf(pHlp, "    Enable                                  = %RTbool\n", MsiCapHdr.n.u1MsiEnable);
            pHlp->pfnPrintf(pHlp, "    Multi-message capability                = %#x\n",     MsiCapHdr.n.u3MsiMultiMessCap);
            pHlp->pfnPrintf(pHlp, "    Multi-message enable                    = %#x\n",     MsiCapHdr.n.u3MsiMultiMessEn);
        }
    }
    /* MSI Address Register (Lo and Hi). */
    {
        uint32_t const uMsiAddrLo = PDMPciDevGetDWord(pPciDev, IOMMU_PCI_OFF_MSI_ADDR_LO);
        uint32_t const uMsiAddrHi = PDMPciDevGetDWord(pPciDev, IOMMU_PCI_OFF_MSI_ADDR_HI);
        MSIADDR MsiAddr;
        MsiAddr.u64 = RT_MAKE_U64(uMsiAddrLo, uMsiAddrHi);
        pHlp->pfnPrintf(pHlp, "  MSI Address                             = %#RX64\n",   MsiAddr.u64);
        if (fVerbose)
        {
            pHlp->pfnPrintf(pHlp, "    Destination mode                        = %#x\n",    MsiAddr.n.u1DestMode);
            pHlp->pfnPrintf(pHlp, "    Redirection hint                        = %#x\n",    MsiAddr.n.u1RedirHint);
            pHlp->pfnPrintf(pHlp, "    Destination Id                          = %#x\n",    MsiAddr.n.u8DestId);
            pHlp->pfnPrintf(pHlp, "    Address                                 = %#Rx32\n", MsiAddr.n.u12Addr);
            pHlp->pfnPrintf(pHlp, "    Address (Hi) / Rsvd?                    = %#Rx32\n", MsiAddr.n.u32Rsvd0);
        }
    }
    /* MSI Data. */
    {
        MSIDATA MsiData;
        MsiData.u32 = PDMPciDevGetDWord(pPciDev, IOMMU_PCI_OFF_MSI_DATA);
        pHlp->pfnPrintf(pHlp, "  MSI Data                                = %#RX32\n", MsiData.u32);
        if (fVerbose)
        {
            pHlp->pfnPrintf(pHlp, "    Vector                                  = %#x (%u)\n", MsiData.n.u8Vector,
                            MsiData.n.u8Vector);
            pHlp->pfnPrintf(pHlp, "    Delivery mode                           = %#x\n", MsiData.n.u3DeliveryMode);
            pHlp->pfnPrintf(pHlp, "    Level                                   = %#x\n", MsiData.n.u1Level);
            pHlp->pfnPrintf(pHlp, "    Trigger mode                            = %s\n",  MsiData.n.u1TriggerMode ?
                                                                                         "level" : "edge");
        }
    }
    /* MSI Mapping Capability Header (HyperTransport, reporting all 0s currently). */
    {
        MSI_MAP_CAP_HDR_T MsiMapCapHdr;
        MsiMapCapHdr.u32 = 0;
        pHlp->pfnPrintf(pHlp, "  MSI Mapping Capability Header           = %#RX32\n",    MsiMapCapHdr.u32);
        if (fVerbose)
        {
            pHlp->pfnPrintf(pHlp, "    Capability ID                           = %#x\n",     MsiMapCapHdr.n.u8MsiMapCapId);
            pHlp->pfnPrintf(pHlp, "    Map enable                              = %RTbool\n", MsiMapCapHdr.n.u1MsiMapEn);
            pHlp->pfnPrintf(pHlp, "    Map fixed                               = %RTbool\n", MsiMapCapHdr.n.u1MsiMapFixed);
            pHlp->pfnPrintf(pHlp, "    Map capability type                     = %#x\n",     MsiMapCapHdr.n.u5MapCapType);
        }
    }
    /* Performance Optimization Control Register. */
    {
        IOMMU_PERF_OPT_CTRL_T const PerfOptCtrl = pThis->PerfOptCtrl;
        pHlp->pfnPrintf(pHlp, "  Performance Optimization Control        = %#RX32\n",    PerfOptCtrl.u32);
        if (fVerbose)
            pHlp->pfnPrintf(pHlp, "    Enable                                  = %RTbool\n", PerfOptCtrl.n.u1PerfOptEn);
    }
    /* XT (x2APIC) General Interrupt Control Register. */
    {
        IOMMU_XT_GEN_INTR_CTRL_T const XtGenIntrCtrl = pThis->XtGenIntrCtrl;
        pHlp->pfnPrintf(pHlp, "  XT General Interrupt Control            = %#RX64\n", XtGenIntrCtrl.u64);
        if (fVerbose)
        {
            pHlp->pfnPrintf(pHlp, "    Interrupt destination mode              = %s\n",
                            !XtGenIntrCtrl.n.u1X2ApicIntrDstMode ? "physical" : "logical");
            pHlp->pfnPrintf(pHlp, "    Interrupt destination                   = %#RX64\n",
                            RT_MAKE_U64(XtGenIntrCtrl.n.u24X2ApicIntrDstLo, XtGenIntrCtrl.n.u7X2ApicIntrDstHi));
            pHlp->pfnPrintf(pHlp, "    Interrupt vector                        = %#x\n", XtGenIntrCtrl.n.u8X2ApicIntrVector);
            pHlp->pfnPrintf(pHlp, "    Interrupt delivery mode                 = %#x\n",
                            !XtGenIntrCtrl.n.u8X2ApicIntrVector ? "fixed" : "arbitrated");
        }
    }
    /* XT (x2APIC) PPR Interrupt Control Register. */
    {
        IOMMU_XT_PPR_INTR_CTRL_T const XtPprIntrCtrl = pThis->XtPprIntrCtrl;
        pHlp->pfnPrintf(pHlp, "  XT PPR Interrupt Control                = %#RX64\n", XtPprIntrCtrl.u64);
        if (fVerbose)
        {
            pHlp->pfnPrintf(pHlp, "   Interrupt destination mode               = %s\n",
                            !XtPprIntrCtrl.n.u1X2ApicIntrDstMode ? "physical" : "logical");
            pHlp->pfnPrintf(pHlp, "   Interrupt destination                    = %#RX64\n",
                            RT_MAKE_U64(XtPprIntrCtrl.n.u24X2ApicIntrDstLo, XtPprIntrCtrl.n.u7X2ApicIntrDstHi));
            pHlp->pfnPrintf(pHlp, "   Interrupt vector                         = %#x\n", XtPprIntrCtrl.n.u8X2ApicIntrVector);
            pHlp->pfnPrintf(pHlp, "   Interrupt delivery mode                  = %#x\n",
                            !XtPprIntrCtrl.n.u8X2ApicIntrVector ? "fixed" : "arbitrated");
        }
    }
    /* XT (X2APIC) GA Log Interrupt Control Register. */
    {
        IOMMU_XT_GALOG_INTR_CTRL_T const XtGALogIntrCtrl = pThis->XtGALogIntrCtrl;
        pHlp->pfnPrintf(pHlp, "  XT PPR Interrupt Control                = %#RX64\n", XtGALogIntrCtrl.u64);
        if (fVerbose)
        {
            pHlp->pfnPrintf(pHlp, "    Interrupt destination mode              = %s\n",
                            !XtGALogIntrCtrl.n.u1X2ApicIntrDstMode ? "physical" : "logical");
            pHlp->pfnPrintf(pHlp, "    Interrupt destination                   = %#RX64\n",
                            RT_MAKE_U64(XtGALogIntrCtrl.n.u24X2ApicIntrDstLo, XtGALogIntrCtrl.n.u7X2ApicIntrDstHi));
            pHlp->pfnPrintf(pHlp, "    Interrupt vector                        = %#x\n", XtGALogIntrCtrl.n.u8X2ApicIntrVector);
            pHlp->pfnPrintf(pHlp, "    Interrupt delivery mode                 = %#x\n",
                            !XtGALogIntrCtrl.n.u8X2ApicIntrVector ? "fixed" : "arbitrated");
        }
    }
    /* MARC Registers. */
    {
        for (unsigned i = 0; i < RT_ELEMENTS(pThis->aMarcApers); i++)
        {
            pHlp->pfnPrintf(pHlp, " MARC Aperature %u:\n", i);
            MARC_APER_BAR_T const MarcAperBar = pThis->aMarcApers[i].Base;
            pHlp->pfnPrintf(pHlp, "   Base    = %#RX64\n", MarcAperBar.n.u40MarcBaseAddr << X86_PAGE_4K_SHIFT);

            MARC_APER_RELOC_T const MarcAperReloc = pThis->aMarcApers[i].Reloc;
            pHlp->pfnPrintf(pHlp, "   Reloc   = %#RX64 (addr: %#RX64, read-only: %RTbool, enable: %RTbool)\n",
                            MarcAperReloc.u64, MarcAperReloc.n.u40MarcRelocAddr << X86_PAGE_4K_SHIFT,
                            MarcAperReloc.n.u1ReadOnly, MarcAperReloc.n.u1RelocEn);

            MARC_APER_LEN_T const MarcAperLen = pThis->aMarcApers[i].Length;
            pHlp->pfnPrintf(pHlp, "   Length  = %u pages\n", MarcAperLen.n.u40MarcLength);
        }
    }
    /* Reserved Register. */
    pHlp->pfnPrintf(pHlp, "  Reserved Register                           = %#RX64\n", pThis->RsvdReg);
    /* Command Buffer Head Pointer Register. */
    {
        CMD_BUF_HEAD_PTR_T const CmdBufHeadPtr = pThis->CmdBufHeadPtr;
        pHlp->pfnPrintf(pHlp, "  Command Buffer Head Pointer             = %#RX64\n", CmdBufHeadPtr.u64);
        pHlp->pfnPrintf(pHlp, "    Pointer                                 = %#x\n",  CmdBufHeadPtr.n.off);
    }
    /* Command Buffer Tail Pointer Register. */
    {
        CMD_BUF_HEAD_PTR_T const CmdBufTailPtr = pThis->CmdBufTailPtr;
        pHlp->pfnPrintf(pHlp, "  Command Buffer Tail Pointer             = %#RX64\n", CmdBufTailPtr.u64);
        pHlp->pfnPrintf(pHlp, "    Pointer                                 = %#x\n",  CmdBufTailPtr.n.off);
    }
    /* Event Log Head Pointer Register. */
    {
        EVT_LOG_HEAD_PTR_T const EvtLogHeadPtr = pThis->EvtLogHeadPtr;
        pHlp->pfnPrintf(pHlp, "  Event Log Head Pointer                  = %#RX64\n", EvtLogHeadPtr.u64);
        pHlp->pfnPrintf(pHlp, "    Pointer                                 = %#x\n",  EvtLogHeadPtr.n.off);
    }
    /* Event Log Tail Pointer Register. */
    {
        EVT_LOG_TAIL_PTR_T const EvtLogTailPtr = pThis->EvtLogTailPtr;
        pHlp->pfnPrintf(pHlp, "  Event Log Head Pointer                  = %#RX64\n", EvtLogTailPtr.u64);
        pHlp->pfnPrintf(pHlp, "    Pointer                                 = %#x\n",  EvtLogTailPtr.n.off);
    }
    /* Status Register. */
    {
        IOMMU_STATUS_T const Status = pThis->Status;
        pHlp->pfnPrintf(pHlp, "  Status Register                         = %#RX64\n", Status.u64);
        if (fVerbose)
        {
            pHlp->pfnPrintf(pHlp, "    Event log overflow                      = %RTbool\n", Status.n.u1EvtOverflow);
            pHlp->pfnPrintf(pHlp, "    Event log interrupt                     = %RTbool\n", Status.n.u1EvtLogIntr);
            pHlp->pfnPrintf(pHlp, "    Completion wait interrupt               = %RTbool\n", Status.n.u1CompWaitIntr);
            pHlp->pfnPrintf(pHlp, "    Event log running                       = %RTbool\n", Status.n.u1EvtLogRunning);
            pHlp->pfnPrintf(pHlp, "    Command buffer running                  = %RTbool\n", Status.n.u1CmdBufRunning);
            pHlp->pfnPrintf(pHlp, "    PPR overflow                            = %RTbool\n", Status.n.u1PprOverflow);
            pHlp->pfnPrintf(pHlp, "    PPR interrupt                           = %RTbool\n", Status.n.u1PprIntr);
            pHlp->pfnPrintf(pHlp, "    PPR log running                         = %RTbool\n", Status.n.u1PprLogRunning);
            pHlp->pfnPrintf(pHlp, "    Guest log running                       = %RTbool\n", Status.n.u1GstLogRunning);
            pHlp->pfnPrintf(pHlp, "    Guest log interrupt                     = %RTbool\n", Status.n.u1GstLogIntr);
            pHlp->pfnPrintf(pHlp, "    PPR log B overflow                      = %RTbool\n", Status.n.u1PprOverflowB);
            pHlp->pfnPrintf(pHlp, "    PPR log active                          = %RTbool\n", Status.n.u1PprLogActive);
            pHlp->pfnPrintf(pHlp, "    Event log B overflow                    = %RTbool\n", Status.n.u1EvtOverflowB);
            pHlp->pfnPrintf(pHlp, "    Event log active                        = %RTbool\n", Status.n.u1EvtLogActive);
            pHlp->pfnPrintf(pHlp, "    PPR log B overflow early warning        = %RTbool\n", Status.n.u1PprOverflowEarlyB);
            pHlp->pfnPrintf(pHlp, "    PPR log overflow early warning          = %RTbool\n", Status.n.u1PprOverflowEarly);
        }
    }
    /* PPR Log Head Pointer. */
    {
        PPR_LOG_HEAD_PTR_T const PprLogHeadPtr = pThis->PprLogHeadPtr;
        pHlp->pfnPrintf(pHlp, "  PPR Log Head Pointer                    = %#RX64\n", PprLogHeadPtr.u64);
        pHlp->pfnPrintf(pHlp, "    Pointer                                 = %#x\n",  PprLogHeadPtr.n.off);
    }
    /* PPR Log Tail Pointer. */
    {
        PPR_LOG_TAIL_PTR_T const PprLogTailPtr = pThis->PprLogTailPtr;
        pHlp->pfnPrintf(pHlp, "  PPR Log Tail Pointer                    = %#RX64\n", PprLogTailPtr.u64);
        pHlp->pfnPrintf(pHlp, "    Pointer                                 = %#x\n",  PprLogTailPtr.n.off);
    }
    /* Guest Virtual-APIC Log Head Pointer. */
    {
        GALOG_HEAD_PTR_T const GALogHeadPtr = pThis->GALogHeadPtr;
        pHlp->pfnPrintf(pHlp, "  Guest Virtual-APIC Log Head Pointer     = %#RX64\n", GALogHeadPtr.u64);
        pHlp->pfnPrintf(pHlp, "    Pointer                                 = %#x\n",  GALogHeadPtr.n.u12GALogPtr);
    }
    /* Guest Virtual-APIC Log Tail Pointer. */
    {
        GALOG_HEAD_PTR_T const GALogTailPtr = pThis->GALogTailPtr;
        pHlp->pfnPrintf(pHlp, "  Guest Virtual-APIC Log Tail Pointer     = %#RX64\n", GALogTailPtr.u64);
        pHlp->pfnPrintf(pHlp, "    Pointer                                 = %#x\n",  GALogTailPtr.n.u12GALogPtr);
    }
    /* PPR Log B Head Pointer. */
    {
        PPR_LOG_B_HEAD_PTR_T const PprLogBHeadPtr = pThis->PprLogBHeadPtr;
        pHlp->pfnPrintf(pHlp, "  PPR Log B Head Pointer                  = %#RX64\n", PprLogBHeadPtr.u64);
        pHlp->pfnPrintf(pHlp, "    Pointer                                 = %#x\n",  PprLogBHeadPtr.n.off);
    }
    /* PPR Log B Tail Pointer. */
    {
        PPR_LOG_B_TAIL_PTR_T const PprLogBTailPtr = pThis->PprLogBTailPtr;
        pHlp->pfnPrintf(pHlp, "  PPR Log B Tail Pointer                  = %#RX64\n", PprLogBTailPtr.u64);
        pHlp->pfnPrintf(pHlp, "    Pointer                                 = %#x\n",  PprLogBTailPtr.n.off);
    }
    /* Event Log B Head Pointer. */
    {
        EVT_LOG_B_HEAD_PTR_T const EvtLogBHeadPtr = pThis->EvtLogBHeadPtr;
        pHlp->pfnPrintf(pHlp, "  Event Log B Head Pointer                = %#RX64\n", EvtLogBHeadPtr.u64);
        pHlp->pfnPrintf(pHlp, "    Pointer                                 = %#x\n",  EvtLogBHeadPtr.n.off);
    }
    /* Event Log B Tail Pointer. */
    {
        EVT_LOG_B_TAIL_PTR_T const EvtLogBTailPtr = pThis->EvtLogBTailPtr;
        pHlp->pfnPrintf(pHlp, "  Event Log B Tail Pointer                = %#RX64\n", EvtLogBTailPtr.u64);
        pHlp->pfnPrintf(pHlp, "    Pointer                                 = %#x\n",  EvtLogBTailPtr.n.off);
    }
    /* PPR Log Auto Response Register. */
    {
        PPR_LOG_AUTO_RESP_T const PprLogAutoResp = pThis->PprLogAutoResp;
        pHlp->pfnPrintf(pHlp, "  PPR Log Auto Response Register          = %#RX64\n",     PprLogAutoResp.u64);
        if (fVerbose)
        {
            pHlp->pfnPrintf(pHlp, "    Code                                    = %#x\n",      PprLogAutoResp.n.u4AutoRespCode);
            pHlp->pfnPrintf(pHlp, "    Mask Gen.                               = %RTbool\n",  PprLogAutoResp.n.u1AutoRespMaskGen);
        }
    }
    /* PPR Log Overflow Early Warning Indicator Register. */
    {
        PPR_LOG_OVERFLOW_EARLY_T const PprLogOverflowEarly = pThis->PprLogOverflowEarly;
        pHlp->pfnPrintf(pHlp, "  PPR Log overflow early warning          = %#RX64\n",    PprLogOverflowEarly.u64);
        if (fVerbose)
        {
            pHlp->pfnPrintf(pHlp, "    Threshold                               = %#x\n",     PprLogOverflowEarly.n.u15Threshold);
            pHlp->pfnPrintf(pHlp, "    Interrupt enable                        = %RTbool\n", PprLogOverflowEarly.n.u1IntrEn);
            pHlp->pfnPrintf(pHlp, "    Enable                                  = %RTbool\n", PprLogOverflowEarly.n.u1Enable);
        }
    }
    /* PPR Log Overflow Early Warning Indicator Register. */
    {
        PPR_LOG_OVERFLOW_EARLY_T const PprLogBOverflowEarly = pThis->PprLogBOverflowEarly;
        pHlp->pfnPrintf(pHlp, "  PPR Log B overflow early warning        = %#RX64\n",    PprLogBOverflowEarly.u64);
        if (fVerbose)
        {
            pHlp->pfnPrintf(pHlp, "    Threshold                               = %#x\n",     PprLogBOverflowEarly.n.u15Threshold);
            pHlp->pfnPrintf(pHlp, "    Interrupt enable                        = %RTbool\n", PprLogBOverflowEarly.n.u1IntrEn);
            pHlp->pfnPrintf(pHlp, "    Enable                                  = %RTbool\n", PprLogBOverflowEarly.n.u1Enable);
        }
    }
}


/**
 * @callback_method_impl{FNSSMDEVSAVEEXEC}
 */
static DECLCALLBACK(int) iommuAmdR3SaveExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM)
{
    /** @todo IOMMU: Save state. */
    RT_NOREF2(pDevIns, pSSM);
    return VERR_NOT_IMPLEMENTED;
}


/**
 * @callback_method_impl{FNSSMDEVLOADEXEC}
 */
static DECLCALLBACK(int) iommuAmdR3LoadExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM, uint32_t uVersion, uint32_t uPass)
{
    /** @todo IOMMU: Load state. */
    RT_NOREF4(pDevIns, pSSM, uVersion, uPass);
    return VERR_NOT_IMPLEMENTED;
}


/**
 * @interface_method_impl{PDMDEVREG,pfnReset}
 */
static DECLCALLBACK(void) iommuAmdR3Reset(PPDMDEVINS pDevIns)
{
    /*
     * Resets read-write portion of the IOMMU state.
     *
     * State data not initialized here is expected to be initialized during
     * device construction and remain read-only through the lifetime of the VM.
     */
    PIOMMU     pThis   = PDMDEVINS_2_DATA(pDevIns, PIOMMU);
    PPDMPCIDEV pPciDev = pDevIns->apPciDevs[0];
    PDMPCIDEV_ASSERT_VALID(pDevIns, pPciDev);

    memset(&pThis->aDevTabBaseAddrs[0], 0, sizeof(pThis->aDevTabBaseAddrs));

    pThis->CmdBufBaseAddr.u64        = 0;
    pThis->CmdBufBaseAddr.n.u4Len    = 8;

    pThis->EvtLogBaseAddr.u64        = 0;
    pThis->EvtLogBaseAddr.n.u4Len    = 8;

    pThis->Ctrl.u64                  = 0;

    pThis->ExclRangeBaseAddr.u64     = 0;
    pThis->ExclRangeLimit.u64        = 0;

    pThis->PprLogBaseAddr.u64        = 0;
    pThis->PprLogBaseAddr.n.u4Len    = 8;

    pThis->HwEvtHi.u64               = 0;
    pThis->HwEvtLo                   = 0;
    pThis->HwEvtStatus.u64           = 0;

    pThis->GALogBaseAddr.u64         = 0;
    pThis->GALogBaseAddr.n.u4Len     = 8;
    pThis->GALogTailAddr.u64         = 0;

    pThis->PprLogBBaseAddr.u64       = 0;
    pThis->PprLogBBaseAddr.n.u4Len   = 8;

    pThis->EvtLogBBaseAddr.u64       = 0;
    pThis->EvtLogBBaseAddr.n.u4Len   = 8;

    pThis->MsiMiscInfo.u64           = 0;
    pThis->PerfOptCtrl.u32           = 0;

    pThis->XtGenIntrCtrl.u64         = 0;
    pThis->XtPprIntrCtrl.u64         = 0;
    pThis->XtGALogIntrCtrl.u64       = 0;

    memset(&pThis->aMarcApers[0], 0, sizeof(pThis->aMarcApers));

    pThis->CmdBufHeadPtr.u64         = 0;
    pThis->CmdBufTailPtr.u64         = 0;
    pThis->EvtLogHeadPtr.u64         = 0;
    pThis->EvtLogTailPtr.u64         = 0;

    pThis->Status.u64                = 0;

    pThis->PprLogHeadPtr.u64         = 0;
    pThis->PprLogTailPtr.u64         = 0;

    pThis->GALogHeadPtr.u64          = 0;
    pThis->GALogTailPtr.u64          = 0;

    pThis->PprLogBHeadPtr.u64        = 0;
    pThis->PprLogBTailPtr.u64        = 0;

    pThis->EvtLogBHeadPtr.u64        = 0;
    pThis->EvtLogBTailPtr.u64        = 0;

    pThis->PprLogAutoResp.u64        = 0;
    pThis->PprLogOverflowEarly.u64   = 0;
    pThis->PprLogBOverflowEarly.u64  = 0;

    PDMPciDevSetDWord(pPciDev, IOMMU_PCI_OFF_BASE_ADDR_REG_LO, 0);
    PDMPciDevSetDWord(pPciDev, IOMMU_PCI_OFF_BASE_ADDR_REG_HI, 0);
}


/**
 * @interface_method_impl{PDMDEVREG,pfnDestruct}
 */
static DECLCALLBACK(int) iommuAmdR3Destruct(PPDMDEVINS pDevIns)
{
    PDMDEV_CHECK_VERSIONS_RETURN_QUIET(pDevIns);
    PIOMMU   pThis   = PDMDEVINS_2_DATA(pDevIns, PIOMMU);
    LogFlowFunc(("\n"));

    /* Close the command thread semaphore. */
    if (pThis->hEvtCmdThread != NIL_SUPSEMEVENT)
    {
        PDMDevHlpSUPSemEventClose(pDevIns, pThis->hEvtCmdThread);
        pThis->hEvtCmdThread = NIL_SUPSEMEVENT;
    }
    return VINF_SUCCESS;
}


/**
 * @interface_method_impl{PDMDEVREG,pfnConstruct}
 */
static DECLCALLBACK(int) iommuAmdR3Construct(PPDMDEVINS pDevIns, int iInstance, PCFGMNODE pCfg)
{
    NOREF(iInstance);

    PDMDEV_CHECK_VERSIONS_RETURN(pDevIns);
    PIOMMU          pThis   = PDMDEVINS_2_DATA(pDevIns, PIOMMU);
    PIOMMUCC        pThisCC = PDMDEVINS_2_DATA_CC(pDevIns, PIOMMUCC);
    PCPDMDEVHLPR3   pHlp    = pDevIns->pHlpR3;
    int             rc;
    LogFlowFunc(("\n"));

    pThisCC->pDevInsR3 = pDevIns;

    /*
     * Validate and read the configuration.
     */
    PDMDEV_VALIDATE_CONFIG_RETURN(pDevIns, "Device|Function", "");

    uint8_t uPciDevice;
    rc = pHlp->pfnCFGMQueryU8Def(pCfg, "Device", &uPciDevice, 0);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("IOMMU: Failed to query \"Device\""));

    uint8_t uPciFunction;
    rc = pHlp->pfnCFGMQueryU8Def(pCfg, "Function", &uPciFunction, 2);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("IOMMU: Failed to query \"Function\""));

    /*
     * Register the IOMMU with PDM.
     */
    PDMIOMMUREGR3 IommuReg;
    RT_ZERO(IommuReg);
    IommuReg.u32Version  = PDM_IOMMUREGCC_VERSION;
    IommuReg.pfnMemRead  = iommuAmdDeviceMemRead;
    IommuReg.pfnMemWrite = iommuAmdDeviceMemWrite;
    IommuReg.pfnMsiRemap = iommuAmdDeviceMsiRemap;
    IommuReg.u32TheEnd   = PDM_IOMMUREGCC_VERSION;
    rc = PDMDevHlpIommuRegister(pDevIns, &IommuReg, &pThisCC->CTX_SUFF(pIommuHlp), &pThis->idxIommu);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("Failed to register ourselves as an IOMMU device"));
    if (pThisCC->CTX_SUFF(pIommuHlp)->u32Version != PDM_IOMMUHLPR3_VERSION)
        return PDMDevHlpVMSetError(pDevIns, VERR_VERSION_MISMATCH, RT_SRC_POS,
                                   N_("IOMMU helper version mismatch; got %#x expected %#x"),
                                   pThisCC->CTX_SUFF(pIommuHlp)->u32Version, PDM_IOMMUHLPR3_VERSION);
    if (pThisCC->CTX_SUFF(pIommuHlp)->u32TheEnd != PDM_IOMMUHLPR3_VERSION)
        return PDMDevHlpVMSetError(pDevIns, VERR_VERSION_MISMATCH, RT_SRC_POS,
                                   N_("IOMMU helper end-version mismatch; got %#x expected %#x"),
                                   pThisCC->CTX_SUFF(pIommuHlp)->u32TheEnd, PDM_IOMMUHLPR3_VERSION);

    /*
     * Initialize read-only PCI configuration space.
     */
    PPDMPCIDEV pPciDev = pDevIns->apPciDevs[0];
    PDMPCIDEV_ASSERT_VALID(pDevIns, pPciDev);

    /* Header. */
    PDMPciDevSetVendorId(pPciDev,           IOMMU_PCI_VENDOR_ID);      /* AMD */
    PDMPciDevSetDeviceId(pPciDev,           IOMMU_PCI_DEVICE_ID);      /* VirtualBox IOMMU device */
    PDMPciDevSetCommand(pPciDev,            0);                        /* Command */
    PDMPciDevSetStatus(pPciDev,             VBOX_PCI_STATUS_CAP_LIST); /* Status - CapList supported */
    PDMPciDevSetRevisionId(pPciDev,         IOMMU_PCI_REVISION_ID);    /* VirtualBox specific device implementation revision */
    PDMPciDevSetClassBase(pPciDev,          0x08);                     /* System Base Peripheral */
    PDMPciDevSetClassSub(pPciDev,           0x06);                     /* IOMMU */
    PDMPciDevSetClassProg(pPciDev,          0x00);                     /* IOMMU Programming interface */
    PDMPciDevSetHeaderType(pPciDev,         0x00);                     /* Single function, type 0. */
    PDMPciDevSetSubSystemId(pPciDev,        IOMMU_PCI_DEVICE_ID);      /* AMD */
    PDMPciDevSetSubSystemVendorId(pPciDev,  IOMMU_PCI_VENDOR_ID);      /* VirtualBox IOMMU device */
    PDMPciDevSetCapabilityList(pPciDev,     IOMMU_PCI_OFF_CAP_HDR);    /* Offset into capability registers. */
    PDMPciDevSetInterruptPin(pPciDev,       0x01);                     /* INTA#. */
    PDMPciDevSetInterruptLine(pPciDev,      0x00);                     /* For software compatibility; no effect on hardware. */

    /* Capability Header. */
    PDMPciDevSetDWord(pPciDev, IOMMU_PCI_OFF_CAP_HDR,
                        RT_BF_MAKE(IOMMU_BF_CAPHDR_CAP_ID,    0xf)     /* RO - Secure Device capability block */
                      | RT_BF_MAKE(IOMMU_BF_CAPHDR_CAP_PTR,   IOMMU_PCI_OFF_MSI_CAP_HDR)  /* RO - Offset to next capability */
                      | RT_BF_MAKE(IOMMU_BF_CAPHDR_CAP_TYPE,  0x3)     /* RO - IOMMU capability block */
                      | RT_BF_MAKE(IOMMU_BF_CAPHDR_CAP_REV,   0x1)     /* RO - IOMMU interface revision */
                      | RT_BF_MAKE(IOMMU_BF_CAPHDR_IOTLB_SUP, 0x0)     /* RO - Remote IOTLB support */
                      | RT_BF_MAKE(IOMMU_BF_CAPHDR_HT_TUNNEL, 0x0)     /* RO - HyperTransport Tunnel support */
                      | RT_BF_MAKE(IOMMU_BF_CAPHDR_NP_CACHE,  0x0)     /* RO - Cache NP page table entries */
                      | RT_BF_MAKE(IOMMU_BF_CAPHDR_EFR_SUP,   0x1)     /* RO - Extended Feature Register support */
                      | RT_BF_MAKE(IOMMU_BF_CAPHDR_CAP_EXT,   0x1));   /* RO - Misc. Information Register support */

    /* Base Address Low Register. */
    PDMPciDevSetDWord(pPciDev, IOMMU_PCI_OFF_BASE_ADDR_REG_LO, 0x0);   /* RW - Base address (Lo) and enable bit. */

    /* Base Address High Register. */
    PDMPciDevSetDWord(pPciDev, IOMMU_PCI_OFF_BASE_ADDR_REG_HI, 0x0);   /* RW - Base address (Hi) */

    /* IOMMU Range Register. */
    PDMPciDevSetDWord(pPciDev, IOMMU_PCI_OFF_RANGE_REG, 0x0);          /* RW - Range register (implemented as RO by us). */

    /* Misc. Information Register 0. */
    PDMPciDevSetDWord(pPciDev, IOMMU_PCI_OFF_MISCINFO_REG_0,
                        RT_BF_MAKE(IOMMU_BF_MISCINFO_0_MSI_NUM,     0x0)    /* RO - MSI number */
                      | RT_BF_MAKE(IOMMU_BF_MISCINFO_0_GVA_SIZE,    0x2)    /* RO - Guest Virt. Addr size (2=48 bits) */
                      | RT_BF_MAKE(IOMMU_BF_MISCINFO_0_PA_SIZE,     0x30)   /* RO - Physical Addr size (48 bits) */
                      | RT_BF_MAKE(IOMMU_BF_MISCINFO_0_VA_SIZE,     0x40)   /* RO - Virt. Addr size (64 bits) */
                      | RT_BF_MAKE(IOMMU_BF_MISCINFO_0_HT_ATS_RESV, 0x0)    /* RW - HT ATS reserved */
                      | RT_BF_MAKE(IOMMU_BF_MISCINFO_0_MSI_NUM_PPR, 0x0));  /* RW - PPR interrupt number */

    /* Misc. Information Register 1. */
    PDMPciDevSetDWord(pPciDev, IOMMU_PCI_OFF_MISCINFO_REG_0, 0);

    /* MSI Capability Header register. */
    PDMMSIREG MsiReg;
    RT_ZERO(MsiReg);
    MsiReg.cMsiVectors    = 1;
    MsiReg.iMsiCapOffset  = IOMMU_PCI_OFF_MSI_CAP_HDR;
    MsiReg.iMsiNextOffset = 0; /* IOMMU_PCI_OFF_MSI_MAP_CAP_HDR */
    MsiReg.fMsi64bit      = 1; /* 64-bit addressing support is mandatory; See AMD spec. 2.8 "IOMMU Interrupt Support". */
    rc = PDMDevHlpPCIRegisterMsi(pDevIns, &MsiReg);
    AssertRCReturn(rc, rc);

    /* MSI Address (Lo, Hi) and MSI data are read-write PCI config registers handled by our generic PCI config space code. */
#if 0
    /* MSI Address Lo. */
    PDMPciDevSetDWord(pPciDev, IOMMU_PCI_OFF_MSI_ADDR_LO, 0);         /* RW - MSI message address (Lo). */
    /* MSI Address Hi. */
    PDMPciDevSetDWord(pPciDev, IOMMU_PCI_OFF_MSI_ADDR_HI, 0);         /* RW - MSI message address (Hi). */
    /* MSI Data. */
    PDMPciDevSetDWord(pPciDev, IOMMU_PCI_OFF_MSI_DATA, 0);            /* RW - MSI data. */
#endif

#if 0
    /** @todo IOMMU: I don't know if we need to support this, enable later if
     *        required. */
    /* MSI Mapping Capability Header register. */
    PDMPciDevSetDWord(pPciDev, IOMMU_PCI_OFF_MSI_MAP_CAP_HDR,
                        RT_BF_MAKE(IOMMU_BF_MSI_MAP_CAPHDR_CAP_ID,   0x8)       /* RO - Capability ID */
                      | RT_BF_MAKE(IOMMU_BF_MSI_MAP_CAPHDR_CAP_PTR,  0x0)       /* RO - Offset to next capability (NULL) */
                      | RT_BF_MAKE(IOMMU_BF_MSI_MAP_CAPHDR_EN,       0x1)       /* RO - MSI mapping capability enable */
                      | RT_BF_MAKE(IOMMU_BF_MSI_MAP_CAPHDR_FIXED,    0x1)       /* RO - MSI mapping range is fixed */
                      | RT_BF_MAKE(IOMMU_BF_MSI_MAP_CAPHDR_CAP_TYPE, 0x15));    /* RO - MSI mapping capability */
    /* When implementing don't forget to copy this to its MMIO shadow register (MsiMapCapHdr) in iommuAmdR3Init. */
#endif

    /*
     * Register the PCI function with PDM.
     */
    rc = PDMDevHlpPCIRegisterEx(pDevIns, pPciDev, 0 /* fFlags */, uPciDevice, uPciFunction, "amd-iommu");
    AssertLogRelRCReturn(rc, rc);

    /*
     * Intercept PCI config. space accesses.
     */
    rc = PDMDevHlpPCIInterceptConfigAccesses(pDevIns, pPciDev, iommuAmdR3PciConfigRead, iommuAmdR3PciConfigWrite);
    AssertLogRelRCReturn(rc, rc);

    /*
     * Create the MMIO region.
     * Mapping of the region is done when software configures it via PCI config space.
     */
    rc = PDMDevHlpMmioCreate(pDevIns, IOMMU_MMIO_REGION_SIZE, pPciDev, 0 /* iPciRegion */, iommuAmdMmioWrite, iommuAmdMmioRead,
                             NULL /* pvUser */, IOMMMIO_FLAGS_READ_DWORD_QWORD | IOMMMIO_FLAGS_WRITE_DWORD_QWORD_ZEROED,
                             "AMD-IOMMU", &pThis->hMmio);
    AssertLogRelRCReturn(rc, rc);

    /*
     * Register saved state.
     */
    rc = PDMDevHlpSSMRegisterEx(pDevIns, IOMMU_SAVED_STATE_VERSION, sizeof(IOMMU), NULL,
                                NULL, NULL, NULL,
                                NULL, iommuAmdR3SaveExec, NULL,
                                NULL, iommuAmdR3LoadExec, NULL);
    AssertLogRelRCReturn(rc, rc);

    /*
     * Register debugger info item.
     */
    rc = PDMDevHlpDBGFInfoRegister(pDevIns, "iommu", "Display IOMMU state.", iommuAmdR3DbgInfo);
    AssertLogRelRCReturn(rc, rc);

    /*
     * Create the command thread and its event semaphore.
     */
    rc = PDMDevHlpThreadCreate(pDevIns, &pThisCC->pCmdThread, pThis, iommuAmdR3CmdThread, iommuAmdR3CmdThreadWakeUp,
                               0 /* cbStack */, RTTHREADTYPE_IO, "AMD-IOMMU");
    AssertLogRelRCReturn(rc, rc);

    rc = PDMDevHlpSUPSemEventCreate(pDevIns, &pThis->hEvtCmdThread);
    AssertLogRelRCReturn(rc, rc);

    /*
     * Initialize read-only registers.
     */
    /** @todo Don't remove the =0 assignment for now. It's just there so it's easier
     *        for me to see existing features that we might want to implement. Do it
     *        later. */
    pThis->ExtFeat.u64 = 0;
    pThis->ExtFeat.n.u1PrefetchSup           = 0;
    pThis->ExtFeat.n.u1PprSup                = 0;
    pThis->ExtFeat.n.u1X2ApicSup             = 0;
    pThis->ExtFeat.n.u1NoExecuteSup          = 0;
    pThis->ExtFeat.n.u1GstTranslateSup       = 0;
    pThis->ExtFeat.n.u1InvAllSup             = 0;
    pThis->ExtFeat.n.u1GstVirtApicSup        = 0;
    pThis->ExtFeat.n.u1HwErrorSup            = 1;
    pThis->ExtFeat.n.u1PerfCounterSup        = 0;
    pThis->ExtFeat.n.u2HostAddrTranslateSize = IOMMU_MAX_HOST_PT_LEVEL;
    pThis->ExtFeat.n.u2GstAddrTranslateSize  = 0;   /* Requires GstTranslateSup. */
    pThis->ExtFeat.n.u2GstCr3RootTblLevel    = 0;   /* Requires GstTranslateSup. */
    pThis->ExtFeat.n.u2SmiFilterSup          = 0;
    pThis->ExtFeat.n.u3SmiFilterCount        = 0;
    pThis->ExtFeat.n.u3GstVirtApicModeSup    = 0;   /* Requires GstVirtApicSup */
    pThis->ExtFeat.n.u2DualPprLogSup         = 0;
    pThis->ExtFeat.n.u2DualEvtLogSup         = 0;
    pThis->ExtFeat.n.u5MaxPasidSup           = 0;   /* Requires GstTranslateSup. */
    pThis->ExtFeat.n.u1UserSupervisorSup     = 0;
    AssertCompile(IOMMU_MAX_DEV_TAB_SEGMENTS <= 3);
    pThis->ExtFeat.n.u2DevTabSegSup          = IOMMU_MAX_DEV_TAB_SEGMENTS;
    pThis->ExtFeat.n.u1PprLogOverflowWarn    = 0;
    pThis->ExtFeat.n.u1PprAutoRespSup        = 0;
    pThis->ExtFeat.n.u2MarcSup               = 0;
    pThis->ExtFeat.n.u1BlockStopMarkSup      = 0;
    pThis->ExtFeat.n.u1PerfOptSup            = 0;
    pThis->ExtFeat.n.u1MsiCapMmioSup         = 1;
    pThis->ExtFeat.n.u1GstIoSup              = 0;
    pThis->ExtFeat.n.u1HostAccessSup         = 0;
    pThis->ExtFeat.n.u1EnhancedPprSup        = 0;
    pThis->ExtFeat.n.u1AttrForwardSup        = 0;
    pThis->ExtFeat.n.u1HostDirtySup          = 0;
    pThis->ExtFeat.n.u1InvIoTlbTypeSup       = 0;
    pThis->ExtFeat.n.u1GstUpdateDisSup       = 0;
    pThis->ExtFeat.n.u1ForcePhysDstSup       = 0;

    pThis->RsvdReg = 0;

    pThis->DevSpecificFeat.u64   = 0;
    pThis->DevSpecificFeat.n.u4RevMajor = IOMMU_DEVSPEC_FEAT_MAJOR_VERSION;
    pThis->DevSpecificFeat.n.u4RevMinor = IOMMU_DEVSPEC_FEAT_MINOR_VERSION;

    pThis->DevSpecificCtrl.u64 = 0;
    pThis->DevSpecificCtrl.n.u4RevMajor = IOMMU_DEVSPEC_CTRL_MAJOR_VERSION;
    pThis->DevSpecificCtrl.n.u4RevMinor = IOMMU_DEVSPEC_CTRL_MINOR_VERSION;

    pThis->DevSpecificStatus.u64 = 0;
    pThis->DevSpecificStatus.n.u4RevMajor = IOMMU_DEVSPEC_STATUS_MAJOR_VERSION;
    pThis->DevSpecificStatus.n.u4RevMinor = IOMMU_DEVSPEC_STATUS_MINOR_VERSION;

    /*
     * Initialize parts of the IOMMU state as it would during reset.
     * Must be called -after- initializing PCI config. space registers.
     */
    iommuAmdR3Reset(pDevIns);

    return VINF_SUCCESS;
}

# else  /* !IN_RING3 */

/**
 * @callback_method_impl{PDMDEVREGR0,pfnConstruct}
 */
static DECLCALLBACK(int) iommuAmdRZConstruct(PPDMDEVINS pDevIns)
{
    PDMDEV_CHECK_VERSIONS_RETURN(pDevIns);
    PIOMMU   pThis   = PDMDEVINS_2_DATA(pDevIns, PIOMMU);
    PIOMMUCC pThisCC = PDMDEVINS_2_DATA_CC(pDevIns, PIOMMUCC);

    pThisCC->CTX_SUFF(pDevIns) = pDevIns;

    /* Set up the MMIO RZ handlers. */
    int rc = PDMDevHlpMmioSetUpContext(pDevIns, pThis->hMmio, iommuAmdMmioWrite, iommuAmdMmioRead, NULL /* pvUser */);
    AssertRCReturn(rc, rc);

    /* Set up the IOMMU RZ callbacks. */
    PDMIOMMUREGCC IommuReg;
    RT_ZERO(IommuReg);
    IommuReg.u32Version  = PDM_IOMMUREGCC_VERSION;
    IommuReg.idxIommu    = pThis->idxIommu;
    IommuReg.pfnMemRead  = iommuAmdDeviceMemRead;
    IommuReg.pfnMemWrite = iommuAmdDeviceMemWrite;
    IommuReg.pfnMsiRemap = iommuAmdDeviceMsiRemap;
    IommuReg.u32TheEnd   = PDM_IOMMUREGCC_VERSION;
    rc = PDMDevHlpIommuSetUpContext(pDevIns, &IommuReg, &pThisCC->CTX_SUFF(pIommuHlp));
    AssertRCReturn(rc, rc);

    return VINF_SUCCESS;
}

# endif /* !IN_RING3 */

/**
 * The device registration structure.
 */
const PDMDEVREG g_DeviceIommuAmd =
{
    /* .u32Version = */             PDM_DEVREG_VERSION,
    /* .uReserved0 = */             0,
    /* .szName = */                 "iommu-amd",
    /* .fFlags = */                 PDM_DEVREG_FLAGS_DEFAULT_BITS | PDM_DEVREG_FLAGS_RZ | PDM_DEVREG_FLAGS_NEW_STYLE,
    /* .fClass = */                 PDM_DEVREG_CLASS_BUS_ISA,   /* Instantiate after PDM_DEVREG_CLASS_BUS_PCI */
    /* .cMaxInstances = */          ~0U,
    /* .uSharedVersion = */         42,
    /* .cbInstanceShared = */       sizeof(IOMMU),
    /* .cbInstanceCC = */           sizeof(IOMMUCC),
    /* .cbInstanceRC = */           sizeof(IOMMURC),
    /* .cMaxPciDevices = */         1,
    /* .cMaxMsixVectors = */        0,
    /* .pszDescription = */         "IOMMU (AMD)",
#if defined(IN_RING3)
    /* .pszRCMod = */               "VBoxDDRC.rc",
    /* .pszR0Mod = */               "VBoxDDR0.r0",
    /* .pfnConstruct = */           iommuAmdR3Construct,
    /* .pfnDestruct = */            iommuAmdR3Destruct,
    /* .pfnRelocate = */            NULL,
    /* .pfnMemSetup = */            NULL,
    /* .pfnPowerOn = */             NULL,
    /* .pfnReset = */               iommuAmdR3Reset,
    /* .pfnSuspend = */             NULL,
    /* .pfnResume = */              NULL,
    /* .pfnAttach = */              NULL,
    /* .pfnDetach = */              NULL,
    /* .pfnQueryInterface = */      NULL,
    /* .pfnInitComplete = */        NULL,
    /* .pfnPowerOff = */            NULL,
    /* .pfnSoftReset = */           NULL,
    /* .pfnReserved0 = */           NULL,
    /* .pfnReserved1 = */           NULL,
    /* .pfnReserved2 = */           NULL,
    /* .pfnReserved3 = */           NULL,
    /* .pfnReserved4 = */           NULL,
    /* .pfnReserved5 = */           NULL,
    /* .pfnReserved6 = */           NULL,
    /* .pfnReserved7 = */           NULL,
#elif defined(IN_RING0)
    /* .pfnEarlyConstruct = */      NULL,
    /* .pfnConstruct = */           iommuAmdRZConstruct,
    /* .pfnDestruct = */            NULL,
    /* .pfnFinalDestruct = */       NULL,
    /* .pfnRequest = */             NULL,
    /* .pfnReserved0 = */           NULL,
    /* .pfnReserved1 = */           NULL,
    /* .pfnReserved2 = */           NULL,
    /* .pfnReserved3 = */           NULL,
    /* .pfnReserved4 = */           NULL,
    /* .pfnReserved5 = */           NULL,
    /* .pfnReserved6 = */           NULL,
    /* .pfnReserved7 = */           NULL,
#elif defined(IN_RC)
    /* .pfnConstruct = */           iommuAmdRZConstruct,
    /* .pfnReserved0 = */           NULL,
    /* .pfnReserved1 = */           NULL,
    /* .pfnReserved2 = */           NULL,
    /* .pfnReserved3 = */           NULL,
    /* .pfnReserved4 = */           NULL,
    /* .pfnReserved5 = */           NULL,
    /* .pfnReserved6 = */           NULL,
    /* .pfnReserved7 = */           NULL,
#else
# error "Not in IN_RING3, IN_RING0 or IN_RC!"
#endif
    /* .u32VersionEnd = */          PDM_DEVREG_VERSION
};

#endif /* !VBOX_DEVICE_STRUCT_TESTCASE */

