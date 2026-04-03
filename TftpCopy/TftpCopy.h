/** @file
  TftpCopy - UEFI TFTP File Transfer Utility

  Header file defining constants, data structures, and function prototypes
  for the TftpCopy UEFI shell application.

  Copyright (c) 2024, UEFI Developer. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef TFTP_COPY_H_
#define TFTP_COPY_H_

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/ShellLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/Mtftp4.h>
#include <Protocol/ServiceBinding.h>

//
// Application identity
//
#define TFTPCOPY_APP_NAME        L"TftpCopy"
#define TFTPCOPY_VERSION_STRING  L"1.0"

//
// TFTP protocol defaults
//
#define TFTP_SERVER_PORT       69
#define TFTP_TIMEOUT_SECONDS   3     ///< Per-packet timeout (seconds)
#define TFTP_RETRY_COUNT       6     ///< Retransmission attempts before failure
#define TFTP_BLOCK_SIZE_STR    "1468" ///< 1500 (MTU) - 20 (IP) - 8 (UDP) - 4 (TFTP) = 1468 bytes
#define TFTP_TSIZE_REQUEST     "0"   ///< Request server to report file size

//
// Default maximum transfer size: 256 MiB
//
#define TFTP_DEFAULT_MAX_SIZE  (256ULL * 1024ULL * 1024ULL)

//
// TFTP option names (RFC 2347 / RFC 2348 / RFC 2349)
//
#define TFTP_OPT_BLKSIZE  "blksize"
#define TFTP_OPT_TSIZE    "tsize"

//
// Width (characters) of the ASCII progress bar
//
#define PROGRESS_BAR_WIDTH  50

/**
  Per-transfer context passed to the MTFTP4 CheckPacket callback.
**/
typedef struct {
  /// Total file size reported by the server via the tsize OACK option.
  /// Valid only when HasTotalSize is TRUE.
  UINT64    TotalFileSize;

  /// Running byte count accumulated from DATA packet payloads.
  UINT64    ReceivedBytes;

  /// Upper bound on bytes that may be written to DestBuffer.
  /// The callback aborts the transfer if ReceivedBytes exceeds this value.
  UINT64    MaxBufferSize;

  /// TRUE once TotalFileSize has been obtained from the server's OACK.
  BOOLEAN   HasTotalSize;
} TFTP_COPY_CONTEXT;

//
// Function prototypes
//

/**
  Print usage information to the console.
**/
VOID
PrintUsage (
  VOID
  );

/**
  Parse a dotted-decimal IPv4 string into an EFI_IPv4_ADDRESS.

  @param[in]  IpStr   Null-terminated Unicode string, e.g. L"192.168.1.1".
  @param[out] IpAddr  Receives the parsed address.

  @retval EFI_SUCCESS            Parsed successfully.
  @retval EFI_INVALID_PARAMETER  NULL pointer, wrong number of octets,
                                 or an octet value exceeds 255.
  @retval EFI_OUT_OF_RESOURCES   Memory allocation failure.
**/
EFI_STATUS
ParseIpv4Address (
  IN  CONST CHAR16      *IpStr,
  OUT EFI_IPv4_ADDRESS  *IpAddr
  );

/**
  Locate the first NIC that exposes EFI_MTFTP4_SERVICE_BINDING_PROTOCOL.

  @param[out] ControllerHandle  Handle on which the binding was found.
  @param[out] ServiceBinding    Pointer to the service binding interface.

  @retval EFI_SUCCESS    A suitable handle was found.
  @retval EFI_NOT_FOUND  No handle exposes the required protocol.
  @retval other          Error from LocateHandleBuffer or OpenProtocol.
**/
EFI_STATUS
LocateMtftp4ServiceBinding (
  OUT EFI_HANDLE                    *ControllerHandle,
  OUT EFI_SERVICE_BINDING_PROTOCOL  **ServiceBinding
  );

/**
  Print a live progress indicator.

  Overwrites the current console line using a carriage-return so the
  display is updated in place without scrolling.

  @param[in] Received   Bytes received so far.
  @param[in] Total      Total expected bytes (meaningful only when HasTotal
                        is TRUE).
  @param[in] HasTotal   TRUE if the total size is known (tsize option).
**/
VOID
PrintProgress (
  IN UINT64   Received,
  IN UINT64   Total,
  IN BOOLEAN  HasTotal
  );

/**
  MTFTP4 per-packet callback.

  Called by the MTFTP4 driver for every packet it receives during a
  ReadFile transfer.  Responsibilities:
    - Extract the tsize value from an OACK packet.
    - Accumulate received bytes from DATA packets.
    - Enforce the buffer-size limit (returns EFI_ABORTED on overflow).
    - Refresh the progress display.
    - Log any server-reported ERROR packets.

  @param[in] This       MTFTP4 protocol instance.
  @param[in] Token      The active transfer token; Token->Context points to
                        a TFTP_COPY_CONTEXT.
  @param[in] PacketLen  Total length of the received packet in bytes.
  @param[in] Packet     Pointer to the raw TFTP packet.

  @retval EFI_SUCCESS   Continue the transfer.
  @retval EFI_ABORTED   Abort because the file exceeds MaxBufferSize.
**/
EFI_STATUS
EFIAPI
TftpCheckPacket (
  IN EFI_MTFTP4_PROTOCOL  *This,
  IN EFI_MTFTP4_TOKEN     *Token,
  IN UINT16               PacketLen,
  IN EFI_MTFTP4_PACKET    *Packet
  );

/**
  Download a file from a TFTP server to a caller-supplied memory buffer.

  Allocates an MTFTP4 child, configures it with the server address, issues
  a ReadFile with blksize and tsize options, and waits for completion.
  Resources are released before returning.

  @param[in]  ServerIp        IPv4 address of the TFTP server.
  @param[in]  RemoteFilename  Path of the file on the TFTP server.
  @param[in]  DestBuffer      Destination memory address; the file is written
                              here directly.
  @param[in]  MaxSize         Maximum number of bytes to receive.
  @param[out] FileSize        Set to the number of bytes actually received.

  @retval EFI_SUCCESS            Transfer completed without error.
  @retval EFI_NOT_FOUND          No MTFTP4-capable network interface found.
  @retval EFI_BUFFER_TOO_SMALL   File is larger than MaxSize.
  @retval EFI_INVALID_PARAMETER  A required pointer argument is NULL.
  @retval other                  Protocol configuration or transfer error.
**/
EFI_STATUS
DownloadFile (
  IN  EFI_IPv4_ADDRESS  *ServerIp,
  IN  CONST CHAR16      *RemoteFilename,
  IN  VOID              *DestBuffer,
  IN  UINT64            MaxSize,
  OUT UINT64            *FileSize
  );

/**
  Shell application entry point.

  Usage:
    TftpCopy <RemoteFile> <DestAddress> [MaxSize]

  Environment:
    serverip  – IPv4 address of the TFTP server (required).

  @param[in] Argc  Argument count (includes the program name).
  @param[in] Argv  Null-terminated Unicode argument strings.

  @retval 0  Success.
  @retval 1  Argument or environment error.
  @retval 2  Network or transfer error.
**/
INTN
EFIAPI
ShellAppMain (
  IN UINTN    Argc,
  IN CHAR16   **Argv
  );

#endif // TFTP_COPY_H_
