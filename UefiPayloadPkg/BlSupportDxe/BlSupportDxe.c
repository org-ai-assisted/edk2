/** @file
  This driver will setup PCDs for DXE phase from HOBs
  and initialise architecture-specific settings and resources.

  Copyright (c) 2014 - 2021, Intel Corporation. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/
#include "BlSupportDxe.h"
#include <Guid/TpmInstance.h>
#include <Coreboot.h>
#include <Library/BlParseLib.h>
#include <Library/PcdLib.h>
#include <Guid/EventGroup.h>

/**
  Main entry for the bootloader support DXE module.

  @param[in] ImageHandle    The firmware allocated handle for the EFI image.
  @param[in] SystemTable    A pointer to the EFI System Table.

  @retval EFI_SUCCESS       The entry point is executed successfully.
  @retval other             Some error occurs when executing this entry point.

**/

//
// VirtualBox/coreboot desktop fix (opt-in, PcdStripCorebootFramebufferTag): the guest
// kernel builds a simple-framebuffer from the coreboot table's LB_TAG_FRAMEBUFFER and
// reserves the fb region INSIDE the vboxvga PCI BAR, so vboxvideo/simpledrm cannot claim
// the BAR (-16) and no DRM node appears -> black GUI. The payload has already consumed the
// fb tag (the GraphicsInfo HOB is built in PEI) by OS-handoff, so retag it to UNUSED at
// ReadyToBoot and fix the table checksums; the guest then falls back to the evictable EFI
// GOP framebuffer, which vboxvideo can evict and bind.
//
STATIC
UINT16
VBoxCbCheckSum16 (
  IN UINT16  *Buffer,
  IN UINTN   Length
  )
{
  UINT32  Sum;
  UINT32  TmpValue;
  UINTN   Idx;
  UINT8   *TmpPtr;

  Sum    = 0;
  TmpPtr = (UINT8 *)Buffer;
  for (Idx = 0; Idx < Length; Idx++) {
    TmpValue = TmpPtr[Idx];
    if (Idx % 2 == 1) {
      TmpValue <<= 8;
    }

    Sum += TmpValue;

    if (Sum >= 0x10000) {
      Sum = (Sum + (Sum >> 16)) & 0xFFFF;
    }
  }

  return (UINT16)((~Sum) & 0xFFFF);
}

STATIC
VOID
EFIAPI
VBoxStripFbCbTag (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  struct cb_header  *Header;
  struct cb_record  *Record;
  UINT8             *Entries;
  UINT8             *Ptr;
  UINT32            Idx;
  BOOLEAN           Found;

  Header = (struct cb_header *)GetParameterBase ();
  if ((Header == NULL) || (Header->table_bytes == 0)) {
    return;
  }

  Entries = (UINT8 *)Header + Header->header_bytes;
  Ptr     = Entries;
  Found   = FALSE;
  for (Idx = 0; Idx < Header->table_entries; Idx++) {
    Record = (struct cb_record *)Ptr;
    if (Record->size == 0) {
      break;
    }

    if (Record->tag == CB_TAG_FRAMEBUFFER) {
      Record->tag = CB_TAG_UNUSED;
      Found       = TRUE;
    }

    Ptr += Record->size;
  }

  if (Found) {
    Header->table_checksum  = VBoxCbCheckSum16 ((UINT16 *)Entries, Header->table_bytes);
    Header->header_checksum = 0;
    Header->header_checksum = VBoxCbCheckSum16 ((UINT16 *)Header, sizeof (*Header));
  }
}

EFI_STATUS
EFIAPI
BlDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                 Status;
  EFI_HOB_GUID_TYPE          *GuidHob;
  EFI_PEI_GRAPHICS_INFO_HOB  *GfxInfo;
  ACPI_BOARD_INFO            *AcpiBoardInfo;
  FIRMWARE_INFO              *FirmwareInfo;
  EFI_SYSTEM_RESOURCE_TABLE  *Esrt;
  EFI_SYSTEM_RESOURCE_ENTRY  *Esre;
  UINTN                      Size;

  //
  // Find the frame buffer information and update PCDs
  //
  GuidHob = GetFirstGuidHob (&gEfiGraphicsInfoHobGuid);
  if (GuidHob != NULL) {
    GfxInfo = (EFI_PEI_GRAPHICS_INFO_HOB *)GET_GUID_HOB_DATA (GuidHob);
    Status  = PcdSet32S (PcdVideoHorizontalResolution, GfxInfo->GraphicsMode.HorizontalResolution);
    ASSERT_EFI_ERROR (Status);
    Status = PcdSet32S (PcdVideoVerticalResolution, GfxInfo->GraphicsMode.VerticalResolution);
    ASSERT_EFI_ERROR (Status);
    Status = PcdSet32S (PcdSetupVideoHorizontalResolution, GfxInfo->GraphicsMode.HorizontalResolution);
    ASSERT_EFI_ERROR (Status);
    Status = PcdSet32S (PcdSetupVideoVerticalResolution, GfxInfo->GraphicsMode.VerticalResolution);
    ASSERT_EFI_ERROR (Status);
  }

  //
  // Set PcdPciExpressBaseAddress and PcdPciExpressBaseSize by HOB info
  //
  GuidHob = GetFirstGuidHob (&gUefiAcpiBoardInfoGuid);
  if (GuidHob != NULL) {
    AcpiBoardInfo = (ACPI_BOARD_INFO *)GET_GUID_HOB_DATA (GuidHob);
    Status        = PcdSet64S (PcdPciExpressBaseAddress, AcpiBoardInfo->PcieBaseAddress);
    ASSERT_EFI_ERROR (Status);
    Status = PcdSet64S (PcdPciExpressBaseSize, AcpiBoardInfo->PcieBaseSize);
    ASSERT_EFI_ERROR (Status);

    if (AcpiBoardInfo->TPM12Present)
    {
      Size = sizeof (gEfiTpmDeviceInstanceTpm12Guid);
      Status = PcdSetPtrS (
               PcdTpmInstanceGuid,
               &Size,
               &gEfiTpmDeviceInstanceTpm12Guid
               );
      ASSERT_EFI_ERROR (Status);
    }
    else if (AcpiBoardInfo->TPM20Present)
    {
      Size = sizeof (gEfiTpmDeviceInstanceTpm20DtpmGuid);
      Status = PcdSetPtrS (
                 PcdTpmInstanceGuid,
                 &Size,
                 &gEfiTpmDeviceInstanceTpm20DtpmGuid
                 );
      ASSERT_EFI_ERROR (Status);
    }
  }

  Status = BlArchAdditionalOps (ImageHandle, SystemTable);
  ASSERT_EFI_ERROR (Status);

  //
  // Build and publish a single-entry ESRT if a bootloader provided us with
  // details about currently running firmware
  //
  GuidHob = GetFirstGuidHob (&gEfiFirmwareInfoHobGuid);
  if (GuidHob != NULL) {
    FirmwareInfo = (FIRMWARE_INFO *)GET_GUID_HOB_DATA (GuidHob);

    Size = sizeof (EFI_SYSTEM_RESOURCE_TABLE) + sizeof (EFI_SYSTEM_RESOURCE_ENTRY);
    Status = gBS->AllocatePool (EfiACPIMemoryNVS, Size, (VOID **)&Esrt);
    ASSERT_EFI_ERROR (Status);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    ZeroMem (Esrt, Size);

    Esrt->FwResourceVersion  = EFI_SYSTEM_RESOURCE_TABLE_FIRMWARE_RESOURCE_VERSION;
    Esrt->FwResourceCount    = 1;
    Esrt->FwResourceCountMax = 1;

    Esre = (EFI_SYSTEM_RESOURCE_ENTRY *)&Esrt[1];
    CopyMem (&Esre->FwClass, &FirmwareInfo->Type, sizeof (EFI_GUID));
    Esre->FwType                   = ESRT_FW_TYPE_SYSTEMFIRMWARE;
    Esre->FwVersion                = FirmwareInfo->Version;
    Esre->LowestSupportedFwVersion = FirmwareInfo->LowestSupportedVersion;

    Status = gBS->InstallConfigurationTable (&gEfiSystemResourceTableGuid, Esrt);
    ASSERT_EFI_ERROR (Status);
  }

  //
  // Retag the coreboot framebuffer entry right before OS handoff (VBoxStripFbCbTag),
  // only when the platform opts in via PcdStripCorebootFramebufferTag.
  //
  if (FeaturePcdGet (PcdStripCorebootFramebufferTag)) {
    EFI_EVENT  ReadyToBootEvent;

    Status = gBS->CreateEventEx (
                    EVT_NOTIFY_SIGNAL,
                    TPL_CALLBACK,
                    VBoxStripFbCbTag,
                    NULL,
                    &gEfiEventReadyToBootGuid,
                    &ReadyToBootEvent
                    );
    ASSERT_EFI_ERROR (Status);
  }

  return EFI_SUCCESS;
}
