/** @file
  UEFI Shell应用程序，用于读写块设备数据。

  Copyright (c) 2025, Your Name. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/ShellLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/BaseCryptLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>

/**
  The main entry point for the application.

  @param[in] ImageHandle    The firmware allocated handle for the EFI image.
  @param[in] SystemTable    A pointer to the EFI System Table.

  @retval EFI_SUCCESS       The application executed successfully.
  @retval Other             An error occurred during execution.
**/
EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS               Status;
  LIST_ENTRY               *Package;
  CHAR16                   *ProblemParam;
  UINTN                    Argc;
  EFI_BLOCK_IO_PROTOCOL    *BlockIo;
  EFI_HANDLE               BlockIoHandle;
  EFI_DEVICE_PATH_PROTOCOL *DevPath;
  CHAR16                   *DeviceName;
  CHAR16                   Operation;
  CHAR16                   *ChecksumType;
  UINT64                   Lba;
  UINTN                    Length;
  UINT8                    *DataBuffer;
  UINT8                    *ChecksumBuffer;
  UINTN                    DataSize;
  UINTN                    ChecksumSize;
  UINTN                    TotalBufferSize;
  UINT32                   BlockSize;
  UINTN                    i;

  // Initialize Shell parameters
  Status = ShellInitialize();
  if (EFI_ERROR(Status)) {
    Print(L"Error: Failed to initialize Shell library - %r\n", Status);
    return Status;
  }

  // Parse command line arguments
  Status = ShellCommandLineParse(EmptyParamList, &Package, &ProblemParam, TRUE);
  if (EFI_ERROR(Status)) {
    Print(L"Error: Command line parsing failed - %r\n", Status);
    return Status;
  }

  Argc = ShellCommandLineGetCount(Package);

  // Check parameter count - support both 5 and 6 parameters
  if (Argc != 5 && Argc != 6) {
    Print(L"Usage: BlkRW <device> <operation[r/w]> <checksum[md5/crc32/none]> <start LBA> <length>\n");
    Print(L"       BlkRW <device> <operation[r/w]> <start LBA> <length>    # Without checksum (read only)\n");
    Print(L"Example: BlkRW blk0 r md5 0 10    # Read 10 blocks with MD5 checksum verification\n");
    Print(L"         BlkRW blk1 w crc32 100 5 # Write 5 blocks with CRC32 checksum\n");
    Print(L"         BlkRW blk2 r none 50 8   # Read 8 blocks without checksum\n");
    Print(L"         BlkRW blk3 r 0 10        # Read 10 blocks without checksum (simplified)\n");
    ShellCommandLineFreeVarList(Package);
    return EFI_INVALID_PARAMETER;
  }

  // Get parameters based on argument count
  DeviceName = (CHAR16*)ShellCommandLineGetRawValue(Package, 1);
  if (DeviceName == NULL) {
    Print(L"Error: Failed to get device name parameter\n");
    ShellCommandLineFreeVarList(Package);
    return EFI_INVALID_PARAMETER;
  }
  
  Operation = ((CHAR16*)ShellCommandLineGetRawValue(Package, 2))[0];  // Get first character

  // Determine if we have checksum parameter
  BOOLEAN UseChecksum = FALSE;
  if (Argc == 6) {
    // 6 parameters: device, operation, checksum, LBA, length
    ChecksumType = (CHAR16*)ShellCommandLineGetRawValue(Package, 3);
    if (ChecksumType == NULL) {
      Print(L"Error: Failed to get checksum type parameter\n");
      ShellCommandLineFreeVarList(Package);
      return EFI_INVALID_PARAMETER;
    }
    UseChecksum = TRUE;
    
    Status = ShellConvertStringToUint64(ShellCommandLineGetRawValue(Package, 4), &Lba, TRUE, FALSE);
    if (EFI_ERROR(Status)) {
      Print(L"Error: Invalid LBA parameter - %s\n", ShellCommandLineGetRawValue(Package, 4));
      ShellCommandLineFreeVarList(Package);
      return Status;
    }
    
    Status = ShellConvertStringToUint64(ShellCommandLineGetRawValue(Package, 5), (UINT64*)&Length, TRUE, FALSE);
    if (EFI_ERROR(Status)) {
      Print(L"Error: Invalid length parameter - %s\n", ShellCommandLineGetRawValue(Package, 5));
      ShellCommandLineFreeVarList(Package);
      return Status;
    }
  } else {
    // 5 parameters: device, operation, LBA, length (no checksum)
    ChecksumType = L"none";
    UseChecksum = FALSE;
    
    Status = ShellConvertStringToUint64(ShellCommandLineGetRawValue(Package, 3), &Lba, TRUE, FALSE);
    if (EFI_ERROR(Status)) {
      Print(L"Error: Invalid LBA parameter - %s\n", ShellCommandLineGetRawValue(Package, 3));
      ShellCommandLineFreeVarList(Package);
      return Status;
    }
    
    Status = ShellConvertStringToUint64(ShellCommandLineGetRawValue(Package, 4), (UINT64*)&Length, TRUE, FALSE);
    if (EFI_ERROR(Status)) {
      Print(L"Error: Invalid length parameter - %s\n", ShellCommandLineGetRawValue(Package, 4));
      ShellCommandLineFreeVarList(Package);
      return Status;
    }
  }

  // Validate operation type
  if (Operation != L'r' && Operation != L'w') {
    Print(L"Error: Invalid operation type '%c', should be 'r' (read) or 'w' (write)\n", Operation);
    ShellCommandLineFreeVarList(Package);
    return EFI_INVALID_PARAMETER;
  }

  // Get device path from device name using Shell protocol
  if (gEfiShellProtocol == NULL) {
    Print(L"Error: Shell protocol not available\n");
    ShellCommandLineFreeVarList(Package);
    return EFI_UNSUPPORTED;
  }

  DevPath = (EFI_DEVICE_PATH_PROTOCOL*)gEfiShellProtocol->GetDevicePathFromMap(DeviceName);
  if (DevPath == NULL) {
    Print(L"Error: Device '%s' not found in device map\n", DeviceName);
    ShellCommandLineFreeVarList(Package);
    return EFI_NOT_FOUND;
  }

  // Locate BlockIo protocol using device path
  Status = gBS->LocateDevicePath(&gEfiBlockIoProtocolGuid, &DevPath, &BlockIoHandle);
  if (EFI_ERROR(Status)) {
    Print(L"Error: Failed to locate BlockIo protocol for device '%s' - %r\n", DeviceName, Status);
    ShellCommandLineFreeVarList(Package);
    return Status;
  }

  Status = gBS->OpenProtocol(
                  BlockIoHandle,
                  &gEfiBlockIoProtocolGuid,
                  (VOID**)&BlockIo,
                  gImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR(Status)) {
    Print(L"Error: Failed to open BlockIo protocol - %r\n", Status);
    ShellCommandLineFreeVarList(Package);
    return Status;
  }

  // Check if device media is present
  if (!BlockIo->Media->MediaPresent) {
    Print(L"Error: Device '%s' media not present\n", DeviceName);
    ShellCommandLineFreeVarList(Package);
    return EFI_NO_MEDIA;
  }

  // Check if LBA range is valid
  if (Lba + Length > BlockIo->Media->LastBlock + 1) {
    Print(L"Error: LBA range exceeds device limit (max LBA: %lu)\n", BlockIo->Media->LastBlock);
    ShellCommandLineFreeVarList(Package);
    return EFI_INVALID_PARAMETER;
  }

  BlockSize = BlockIo->Media->BlockSize;
  TotalBufferSize = Length * BlockSize;

  // Set checksum size based on type
  if (UseChecksum) {
    if (StrCmp(ChecksumType, L"md5") == 0) {
      ChecksumSize = 16;  // MD5 produces 16-byte hash
    } else if (StrCmp(ChecksumType, L"crc32") == 0) {
      ChecksumSize = 4;   // CRC32 produces 4-byte hash
    } else if (StrCmp(ChecksumType, L"none") == 0) {
      // "none" means no checksum even in 6-parameter mode
      ChecksumSize = 0;
      UseChecksum = FALSE;  // Override to disable checksum
    } else {
      Print(L"Error: Invalid checksum type '%s', should be 'md5', 'crc32', or 'none'\n", ChecksumType);
      ShellCommandLineFreeVarList(Package);
      return EFI_INVALID_PARAMETER;
    }

    if (UseChecksum) {
      // Calculate data size (total buffer minus checksum) only when checksum is actually used
      if (TotalBufferSize <= ChecksumSize) {
        Print(L"Error: Buffer size %lu is too small for checksum (needs at least %lu bytes)\n", 
              TotalBufferSize, ChecksumSize + 1);
        ShellCommandLineFreeVarList(Package);
        return EFI_INVALID_PARAMETER;
      }
      DataSize = TotalBufferSize - ChecksumSize;
    } else {
      // No checksum mode - use entire buffer for data
      DataSize = TotalBufferSize;
    }
  } else {
    // No checksum mode - use entire buffer for data
    ChecksumSize = 0;
    DataSize = TotalBufferSize;
  }

  // Allocate buffers
  DataBuffer = AllocatePool(DataSize);
  ChecksumBuffer = AllocatePool(ChecksumSize);
  if (DataBuffer == NULL || ChecksumBuffer == NULL) {
    Print(L"Error: Cannot allocate buffers (Data: %lu bytes, Checksum: %lu bytes)\n", DataSize, ChecksumSize);
    if (DataBuffer != NULL) FreePool(DataBuffer);
    if (ChecksumBuffer != NULL) FreePool(ChecksumBuffer);
    ShellCommandLineFreeVarList(Package);
    return EFI_OUT_OF_RESOURCES;
  }

  // Execute read/write operation
  if (Operation == L'r') {
    // Read operation
    Print(L"Reading data from device '%s' (LBA: %lu, Length: %lu, Block Size: %lu, Checksum: %s)\n", 
          DeviceName, Lba, Length, BlockSize, ChecksumType);
    
    UINT8 *ReadBuffer = AllocatePool(TotalBufferSize);
    if (ReadBuffer == NULL) {
      Print(L"Error: Cannot allocate %lu bytes for read buffer\n", TotalBufferSize);
      FreePool(DataBuffer);
      FreePool(ChecksumBuffer);
      ShellCommandLineFreeVarList(Package);
      return EFI_OUT_OF_RESOURCES;
    }
    
    Status = BlockIo->ReadBlocks(
                        BlockIo,
                        BlockIo->Media->MediaId,
                        Lba,
                        TotalBufferSize,
                        ReadBuffer
                        );
    if (EFI_ERROR(Status)) {
      Print(L"Error: Read operation failed - %r\n", Status);
      FreePool(ReadBuffer);
    } else {
      if (UseChecksum) {
        // With checksum: split read buffer into data and checksum
        CopyMem(DataBuffer, ReadBuffer, DataSize);
        CopyMem(ChecksumBuffer, ReadBuffer + DataSize, ChecksumSize);
        
        // Calculate checksum of read data
        UINT8 CalculatedChecksum[16]; // Maximum size for MD5
        BOOLEAN ChecksumMatch = FALSE;
        
        if (StrCmp(ChecksumType, L"md5") == 0) {
          // Calculate MD5 checksum using HashAll function
          Status = Md5HashAll(DataBuffer, DataSize, CalculatedChecksum);
          if (!EFI_ERROR(Status)) {
            ChecksumMatch = CompareMem(CalculatedChecksum, ChecksumBuffer, 16) == 0;
          } else {
            Print(L"Error: MD5 calculation failed - %r\n", Status);
          }
        } else if (StrCmp(ChecksumType, L"crc32") == 0) {
          // Calculate CRC32 checksum
          UINT32 CalculatedCrc = gBS->CalculateCrc32(DataBuffer, DataSize, 0);
          UINT32 StoredCrc = *(UINT32*)ChecksumBuffer;
          ChecksumMatch = (CalculatedCrc == StoredCrc);
        }
        
        if (ChecksumMatch) {
          Print(L"Read successful! Data integrity verified with %s checksum\n", ChecksumType);
          Print(L"Data content (%lu bytes):\n", DataSize);
          
          // Display data in hex and ASCII format
          for (i = 0; i < DataSize; i += 16) {
            UINTN j;
            Print(L"%04X: ", i);
            
            // Display hex
            for (j = 0; j < 16 && (i + j) < DataSize; j++) {
              Print(L"%02X ", DataBuffer[i + j]);
            }
            
            // Padding for alignment
            for (; j < 16; j++) {
              Print(L"   ");
            }
            
            Print(L" ");
            
            // Display ASCII characters
            for (j = 0; j < 16 && (i + j) < DataSize; j++) {
              UINT8 ch = DataBuffer[i + j];
              if (ch >= 0x20 && ch <= 0x7E) {
                Print(L"%c", ch);
              } else {
                Print(L".");
              }
            }
            Print(L"\n");
          }
        } else {
          Print(L"Error: Data integrity check failed! %s checksum mismatch\n", ChecksumType);
          Status = EFI_CRC_ERROR;
        }
      } else {
        // No checksum: directly use the entire read buffer as data
        CopyMem(DataBuffer, ReadBuffer, DataSize);
        Print(L"Read successful! Data content (%lu bytes):\n", DataSize);
        
        // Display data in hex and ASCII format
        for (i = 0; i < DataSize; i += 16) {
          UINTN j;
          Print(L"%04X: ", i);
          
          // Display hex
          for (j = 0; j < 16 && (i + j) < DataSize; j++) {
            Print(L"%02X ", DataBuffer[i + j]);
          }
          
          // Padding for alignment
          for (; j < 16; j++) {
            Print(L"   ");
          }
          
          Print(L" ");
          
          // Display ASCII characters
          for (j = 0; j < 16 && (i + j) < DataSize; j++) {
            UINT8 ch = DataBuffer[i + j];
            if (ch >= 0x20 && ch <= 0x7E) {
              Print(L"%c", ch);
            } else {
              Print(L".");
            }
          }
          Print(L"\n");
        }
      }
      
      FreePool(ReadBuffer);
    }
  } else {
    // Write operation - generate random data
    Print(L"Writing random data to device '%s' (LBA: %lu, Length: %lu, Block Size: %lu, Checksum: %s)\n", 
          DeviceName, Lba, Length, BlockSize, ChecksumType);
    
    // Fill data buffer with simple pseudo-random data
    for (i = 0; i < DataSize; i++) {
      DataBuffer[i] = (UINT8)((i * 1103515245 + 12345) & 0xFF);
    }
    
    if (UseChecksum) {
      // Calculate checksum
      if (StrCmp(ChecksumType, L"md5") == 0) {
        // Calculate MD5 checksum using HashAll function
        Status = Md5HashAll(DataBuffer, DataSize, ChecksumBuffer);
        if (EFI_ERROR(Status)) {
          Print(L"Error: MD5 calculation failed - %r\n", Status);
          FreePool(DataBuffer);
          FreePool(ChecksumBuffer);
          ShellCommandLineFreeVarList(Package);
          return Status;
        }
      } else if (StrCmp(ChecksumType, L"crc32") == 0) {
        // Calculate CRC32 checksum
        UINT32 CrcValue = gBS->CalculateCrc32(DataBuffer, DataSize, 0);
        CopyMem(ChecksumBuffer, &CrcValue, 4);
      }
      
      // Combine data and checksum into write buffer
      UINT8 *WriteBuffer = AllocatePool(TotalBufferSize);
      if (WriteBuffer == NULL) {
        Print(L"Error: Cannot allocate %lu bytes for write buffer\n", TotalBufferSize);
        FreePool(DataBuffer);
        FreePool(ChecksumBuffer);
        ShellCommandLineFreeVarList(Package);
        return EFI_OUT_OF_RESOURCES;
      }
      
      CopyMem(WriteBuffer, DataBuffer, DataSize);
      CopyMem(WriteBuffer + DataSize, ChecksumBuffer, ChecksumSize);
      
      Status = BlockIo->WriteBlocks(
                          BlockIo,
                          BlockIo->Media->MediaId,
                          Lba,
                          TotalBufferSize,
                          WriteBuffer
                          );
      if (EFI_ERROR(Status)) {
        Print(L"Error: Write operation failed - %r\n", Status);
      } else {
        Print(L"Write successful! %lu bytes of data + %lu bytes %s checksum written\n", 
              DataSize, ChecksumSize, ChecksumType);
      }
      
      FreePool(WriteBuffer);
    } else {
      // No checksum: directly write data buffer
      Status = BlockIo->WriteBlocks(
                          BlockIo,
                          BlockIo->Media->MediaId,
                          Lba,
                          DataSize,
                          DataBuffer
                          );
      if (EFI_ERROR(Status)) {
        Print(L"Error: Write operation failed - %r\n", Status);
      } else {
        Print(L"Write successful! %lu bytes of data written without checksum\n", DataSize);
      }
    }
  }

  // Clean up resources
  FreePool(DataBuffer);
  FreePool(ChecksumBuffer);
  gBS->CloseProtocol(BlockIoHandle, &gEfiBlockIoProtocolGuid, gImageHandle, NULL);
  ShellCommandLineFreeVarList(Package);

  return Status;
}
