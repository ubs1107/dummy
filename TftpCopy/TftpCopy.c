/** @file
  TftpCopy - UEFI TFTP File Transfer Utility

  Copies a file from a TFTP server directly to a physical memory address.
  The server IP address is read from the 'serverip' UEFI Shell environment
  variable, mirroring the convention used by U-Boot and network boot loaders.

  Usage:
    TftpCopy <RemoteFile> <DestAddress> [MaxSize]

  Arguments:
    RemoteFile    Path to the file on the TFTP server.
    DestAddress   Destination physical memory address, hexadecimal
                  (e.g. 0x80000000).
    MaxSize       Maximum bytes to receive, hex or decimal
                  (default: 256 MiB).  The destination region must be at
                  least this large and writable.

  Environment Variables:
    serverip      IPv4 address of the TFTP server (required before calling
                  this utility).

  Return Values:
    0   – success; file loaded to the specified address.
    1   – bad arguments or missing/invalid 'serverip' variable.
    2   – network initialisation or transfer failure.

  Examples:
    Shell> set serverip 192.168.1.10
    Shell> TftpCopy Image 0x80000000
    Shell> TftpCopy rootfs.cpio.gz 0xA0000000 0x8000000

  Copyright (c) 2024, UEFI Developer. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "TftpCopy.h"

//
// Set to TRUE the first time a progress line is printed so that the
// caller knows to emit a trailing newline after the transfer finishes.
//
STATIC BOOLEAN  mProgressShown = FALSE;

/**
  Print usage information to the console.
**/
VOID
PrintUsage (
  VOID
  )
{
  Print (L"\n%s v%s - UEFI TFTP File Transfer Utility\n\n",
         TFTPCOPY_APP_NAME, TFTPCOPY_VERSION_STRING);
  Print (L"Usage:\n");
  Print (L"  %s <RemoteFile> <DestAddress> [MaxSize]\n\n",
         TFTPCOPY_APP_NAME);
  Print (L"Arguments:\n");
  Print (L"  RemoteFile    Path to file on TFTP server\n");
  Print (L"  DestAddress   Destination memory address (hex, e.g. 0x80000000)\n");
  Print (L"  MaxSize       Maximum transfer size in bytes, hex or decimal\n");
  Print (L"                Default: 256 MiB\n\n");
  Print (L"Environment:\n");
  Print (L"  serverip      IPv4 address of TFTP server (required)\n\n");
  Print (L"Examples:\n");
  Print (L"  Shell> set serverip 192.168.1.10\n");
  Print (L"  Shell> %s Image 0x80000000\n", TFTPCOPY_APP_NAME);
  Print (L"  Shell> %s kernel.bin 0x80000000 0x2000000\n\n",
         TFTPCOPY_APP_NAME);
}

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
  )
{
  CHAR16  *Buf;
  CHAR16  *Cursor;
  CHAR16  *DotPos;
  UINTN   Idx;
  UINTN   Val;

  if ((IpStr == NULL) || (IpAddr == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Work on a mutable copy so we can NUL-terminate octet substrings.
  //
  Buf = AllocateCopyPool ((StrLen (IpStr) + 1) * sizeof (CHAR16), IpStr);
  if (Buf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Cursor = Buf;
  for (Idx = 0; Idx < 4; Idx++) {
    if (Idx < 3) {
      DotPos = StrStr (Cursor, L".");
      if (DotPos == NULL) {
        FreePool (Buf);
        return EFI_INVALID_PARAMETER;
      }

      *DotPos = L'\0';
    }

    Val = StrDecimalToUintn (Cursor);
    if (Val > 255) {
      FreePool (Buf);
      return EFI_INVALID_PARAMETER;
    }

    IpAddr->Addr[Idx] = (UINT8)Val;

    if (Idx < 3) {
      Cursor = DotPos + 1;
    }
  }

  FreePool (Buf);
  return EFI_SUCCESS;
}

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
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       HandleCount;
  UINTN       Idx;

  ASSERT (ControllerHandle != NULL);
  ASSERT (ServiceBinding   != NULL);

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiMtftp4ServiceBindingProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  //
  // Return the first available network interface.
  //
  Status = EFI_NOT_FOUND;
  for (Idx = 0; Idx < HandleCount; Idx++) {
    Status = gBS->OpenProtocol (
                    Handles[Idx],
                    &gEfiMtftp4ServiceBindingProtocolGuid,
                    (VOID **)ServiceBinding,
                    gImageHandle,
                    NULL,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );
    if (!EFI_ERROR (Status)) {
      *ControllerHandle = Handles[Idx];
      break;
    }
  }

  FreePool (Handles);
  return Status;
}

/**
  Print a live progress indicator, overwriting the current console line.

  Shows a percentage progress bar when the total size is known (via the
  TFTP tsize option), or a spinning cursor with a byte counter otherwise.

  @param[in] Received   Bytes received so far.
  @param[in] Total      Total expected bytes (used only when HasTotal is TRUE).
  @param[in] HasTotal   TRUE if the total size is known.
**/
VOID
PrintProgress (
  IN UINT64   Received,
  IN UINT64   Total,
  IN BOOLEAN  HasTotal
  )
{
  UINTN                Percent;
  UINTN                Filled;
  UINTN                Idx;
  STATIC UINTN         SpinIdx = 0;
  STATIC CONST CHAR16  *SpinChars[] = { L"|", L"/", L"-", L"\\" };

  if (HasTotal && (Total > 0)) {
    //
    // Percentage progress bar.
    //
    Percent = (Received >= Total) ? 100 : (UINTN)((Received * 100) / Total);
    Filled  = (Percent * PROGRESS_BAR_WIDTH) / 100;

    Print (L"\r  [");
    for (Idx = 0; Idx < PROGRESS_BAR_WIDTH; Idx++) {
      Print ((Idx < Filled) ? L"#" : L".");
    }

    Print (L"] %3u%%  %Lu / %Lu bytes", Percent, Received, Total);
  } else {
    //
    // Spinner with byte counter (file size unknown).
    //
    Print (
      L"\r  %s  %Lu bytes received...",
      SpinChars[SpinIdx % ARRAY_SIZE (SpinChars)],
      Received
      );
    SpinIdx++;
  }

  mProgressShown = TRUE;
}

/**
  MTFTP4 per-packet callback.

  @param[in] This       MTFTP4 protocol instance.
  @param[in] Token      Active transfer token; Token->Context points to a
                        TFTP_COPY_CONTEXT.
  @param[in] PacketLen  Total length of the received packet.
  @param[in] Packet     Pointer to the raw TFTP packet (network byte order).

  @retval EFI_SUCCESS  Continue the transfer.
  @retval EFI_ABORTED  File exceeds MaxBufferSize; transfer is cancelled.
**/
EFI_STATUS
EFIAPI
TftpCheckPacket (
  IN EFI_MTFTP4_PROTOCOL  *This,
  IN EFI_MTFTP4_TOKEN     *Token,
  IN UINT16               PacketLen,
  IN EFI_MTFTP4_PACKET    *Packet
  )
{
  TFTP_COPY_CONTEXT  *Context;
  UINT16             OpCode;
  UINT16             DataLen;
  CONST CHAR8        *OptionPtr;
  CONST CHAR8        *Name;
  CONST CHAR8        *Value;
  UINTN              NameLen;
  UINTN              ValueLen;
  UINTN              Remaining;

  ASSERT (Token   != NULL);
  ASSERT (Packet  != NULL);

  Context = (TFTP_COPY_CONTEXT *)Token->Context;
  ASSERT (Context != NULL);

  //
  // OpCode is in network (big-endian) byte order; convert to host order.
  //
  OpCode = SwapBytes16 (Packet->OpCode);

  switch (OpCode) {
    case EFI_MTFTP4_OPCODE_OACK:
      //
      // Options Acknowledgement (RFC 2347).  Parse name=value pairs to
      // extract the tsize so we can show percentage progress.
      //
      if (PacketLen <= 2) {
        break;
      }

      OptionPtr = (CONST CHAR8 *)Packet->Oack.Data;
      Remaining = (UINTN)(PacketLen - 2);

      while (Remaining > 1) {
        Name    = OptionPtr;
        NameLen = AsciiStrLen (Name) + 1;  // include NUL
        if (NameLen > Remaining) {
          break;
        }

        Remaining -= NameLen;
        OptionPtr += NameLen;

        if (Remaining == 0) {
          break;
        }

        Value    = OptionPtr;
        ValueLen = AsciiStrLen (Value) + 1;  // include NUL
        if (ValueLen > Remaining) {
          break;
        }

        Remaining -= ValueLen;
        OptionPtr += ValueLen;

        if (AsciiStriCmp (Name, TFTP_OPT_TSIZE) == 0) {
          Context->TotalFileSize = AsciiStrDecimalToUint64 (Value);
          Context->HasTotalSize  = TRUE;
        }
      }

      break;

    case EFI_MTFTP4_OPCODE_DATA:
      //
      // DATA packet: 2-byte opcode + 2-byte block number = 4-byte header.
      //
      DataLen = (PacketLen > 4) ? (UINT16)(PacketLen - 4) : 0;
      Context->ReceivedBytes += DataLen;

      //
      // Safety: abort if the file is larger than the caller's buffer.
      //
      if (Context->ReceivedBytes > Context->MaxBufferSize) {
        if (mProgressShown) {
          Print (L"\n");
          mProgressShown = FALSE;
        }

        Print (
          L"  ERROR: File exceeds maximum buffer size of %Lu bytes.\n"
          L"         Increase MaxSize or use a larger destination region.\n",
          Context->MaxBufferSize
          );
        return EFI_ABORTED;
      }

      PrintProgress (
        Context->ReceivedBytes,
        Context->TotalFileSize,
        Context->HasTotalSize
        );
      break;

    case EFI_MTFTP4_OPCODE_ERROR:
      //
      // Server-reported error: print the error code and message.
      //
      if (mProgressShown) {
        Print (L"\n");
        mProgressShown = FALSE;
      }

      Print (L"  TFTP server error %u", SwapBytes16 (Packet->Err.ErrorCode));
      if (PacketLen > 4) {
        Print (L": %a", (CONST CHAR8 *)Packet->Err.ErrorMessage);
      }

      Print (L"\n");
      break;

    default:
      break;
  }

  return EFI_SUCCESS;
}

/**
  Download a file via TFTP directly into a caller-supplied memory buffer.

  @param[in]  ServerIp        IPv4 address of the TFTP server.
  @param[in]  RemoteFilename  Path of the file on the TFTP server.
  @param[in]  DestBuffer      Destination memory address; data is written here.
  @param[in]  MaxSize         Maximum number of bytes to receive.
  @param[out] FileSize        Set to the number of bytes actually received.

  @retval EFI_SUCCESS            File downloaded successfully.
  @retval EFI_NOT_FOUND          No MTFTP4-capable network interface found.
  @retval EFI_BUFFER_TOO_SMALL   File is larger than MaxSize.
  @retval EFI_INVALID_PARAMETER  A required pointer is NULL.
  @retval other                  Protocol configuration or transfer error.
**/
EFI_STATUS
DownloadFile (
  IN  EFI_IPv4_ADDRESS  *ServerIp,
  IN  CONST CHAR16      *RemoteFilename,
  IN  VOID              *DestBuffer,
  IN  UINT64            MaxSize,
  OUT UINT64            *FileSize
  )
{
  EFI_STATUS                    Status;
  EFI_HANDLE                    ControllerHandle;
  EFI_HANDLE                    ChildHandle;
  EFI_SERVICE_BINDING_PROTOCOL  *ServiceBinding;
  EFI_MTFTP4_PROTOCOL           *Mtftp4;
  EFI_MTFTP4_CONFIG_DATA        ConfigData;
  EFI_MTFTP4_TOKEN              Token;
  EFI_MTFTP4_OPTION             Options[2];
  TFTP_COPY_CONTEXT             Context;
  CHAR8                         *AsciiFilename;
  UINTN                         FilenameLen;
  CHAR8                         BlkSizeStr[] = TFTP_BLOCK_SIZE_STR;
  CHAR8                         TSizeStr[]   = TFTP_TSIZE_REQUEST;

  if ((ServerIp == NULL) || (RemoteFilename == NULL) ||
      (DestBuffer == NULL) || (FileSize == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  AsciiFilename = NULL;
  ChildHandle   = NULL;
  Mtftp4        = NULL;
  ServiceBinding = NULL;

  //
  // Convert the Unicode filename to ASCII for the MTFTP4 API.
  //
  FilenameLen   = StrLen (RemoteFilename) + 1;
  AsciiFilename = AllocateZeroPool (FilenameLen);
  if (AsciiFilename == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  UnicodeStrToAsciiStrS (RemoteFilename, AsciiFilename, FilenameLen);

  //
  // Find a network interface that supports MTFTP4.
  //
  Status = LocateMtftp4ServiceBinding (&ControllerHandle, &ServiceBinding);
  if (EFI_ERROR (Status)) {
    Print (L"  ERROR: No network interface with MTFTP4 support found.\n");
    Print (L"         Ensure the network stack is initialised and a NIC is present.\n");
    goto CleanupFilename;
  }

  //
  // Allocate an MTFTP4 child handle via the service binding.
  //
  Status = ServiceBinding->CreateChild (ServiceBinding, &ChildHandle);
  if (EFI_ERROR (Status)) {
    Print (L"  ERROR: Failed to create MTFTP4 instance: %r\n", Status);
    goto CleanupFilename;
  }

  //
  // Retrieve the MTFTP4 protocol interface from the child handle.
  //
  Status = gBS->HandleProtocol (
                  ChildHandle,
                  &gEfiMtftp4ProtocolGuid,
                  (VOID **)&Mtftp4
                  );
  if (EFI_ERROR (Status)) {
    Print (L"  ERROR: Failed to open MTFTP4 protocol: %r\n", Status);
    goto CleanupChild;
  }

  //
  // Configure the MTFTP4 instance.
  // UseDefaultSetting = TRUE: inherit IP/mask/gateway from the current
  // network stack configuration (DHCP or static, already negotiated).
  //
  ZeroMem (&ConfigData, sizeof (ConfigData));
  ConfigData.UseDefaultSetting = TRUE;
  ConfigData.InitialServerPort = TFTP_SERVER_PORT;
  ConfigData.TryCount          = TFTP_RETRY_COUNT;
  ConfigData.TimeoutValue      = TFTP_TIMEOUT_SECONDS;
  CopyMem (&ConfigData.ServerIp, ServerIp, sizeof (EFI_IPv4_ADDRESS));

  Status = Mtftp4->Configure (Mtftp4, &ConfigData);
  if (EFI_ERROR (Status)) {
    //
    // Fall back: try with a zeroed station IP (let the driver assign one).
    //
    ConfigData.UseDefaultSetting = FALSE;
    ZeroMem (&ConfigData.StationIp,  sizeof (EFI_IPv4_ADDRESS));
    ZeroMem (&ConfigData.SubnetMask, sizeof (EFI_IPv4_ADDRESS));
    ZeroMem (&ConfigData.GatewayIp,  sizeof (EFI_IPv4_ADDRESS));

    Status = Mtftp4->Configure (Mtftp4, &ConfigData);
    if (EFI_ERROR (Status)) {
      Print (L"  ERROR: Failed to configure MTFTP4: %r\n", Status);
      goto CleanupChild;
    }
  }

  //
  // Initialise the per-transfer context shared with the callback.
  //
  ZeroMem (&Context, sizeof (Context));
  Context.MaxBufferSize = MaxSize;

  //
  // Request blksize (RFC 2348) and tsize (RFC 2349) options.
  // blksize 1468 fits a standard 1500-byte Ethernet frame with IP+UDP headers.
  // tsize 0 asks the server to report the file size in its OACK reply.
  //
  Options[0].OptionStr = (UINT8 *)TFTP_OPT_BLKSIZE;
  Options[0].ValueStr  = (UINT8 *)BlkSizeStr;
  Options[1].OptionStr = (UINT8 *)TFTP_OPT_TSIZE;
  Options[1].ValueStr  = (UINT8 *)TSizeStr;

  //
  // Build the synchronous (Event = NULL) transfer token.
  // Setting Buffer to DestBuffer causes the MTFTP4 driver to write
  // received file data directly to the destination memory address.
  //
  ZeroMem (&Token, sizeof (Token));
  Token.Event       = NULL;
  Token.Filename    = (UINT8 *)AsciiFilename;
  Token.OptionCount = 2;
  Token.OptionList  = Options;
  Token.BufferSize  = MaxSize;
  Token.Buffer      = DestBuffer;
  Token.Context     = &Context;
  Token.CheckPacket = TftpCheckPacket;

  Print (
    L"  Downloading '%a' from %d.%d.%d.%d ...\n",
    AsciiFilename,
    ServerIp->Addr[0], ServerIp->Addr[1],
    ServerIp->Addr[2], ServerIp->Addr[3]
    );

  //
  // Issue the blocking TFTP GET.  Returns when the transfer completes,
  // fails, or is aborted by the callback.
  //
  Status = Mtftp4->ReadFile (Mtftp4, &Token);

  //
  // Terminate the in-place progress line.
  //
  if (mProgressShown) {
    Print (L"\n");
    mProgressShown = FALSE;
  }

  if (Status == EFI_ABORTED) {
    //
    // Aborted by the callback due to buffer overflow; already reported.
    //
    Status = EFI_BUFFER_TOO_SMALL;
  } else if (EFI_ERROR (Status)) {
    Print (L"  ERROR: TFTP transfer failed: %r\n", Status);
  }

  *FileSize = Token.BufferSize;

CleanupChild:
  ServiceBinding->DestroyChild (ServiceBinding, ChildHandle);

CleanupFilename:
  FreePool (AsciiFilename);
  return Status;
}

/**
  Shell application entry point.

  @param[in] Argc  Argument count (argv[0] is the application name).
  @param[in] Argv  Null-terminated Unicode argument strings.

  @retval 0  File transferred successfully.
  @retval 1  Argument or environment error.
  @retval 2  Network or transfer error.
**/
INTN
EFIAPI
ShellAppMain (
  IN UINTN    Argc,
  IN CHAR16   **Argv
  )
{
  EFI_STATUS        Status;
  CONST CHAR16      *ServerIpStr;
  EFI_IPv4_ADDRESS  ServerIp;
  VOID              *DestBuffer;
  UINT64            DestAddress;
  UINT64            MaxSize;
  UINT64            TransferredSize;
  CONST CHAR16      *RemoteFilename;

  //
  // Validate argument count: program + 2 required + 1 optional.
  //
  if ((Argc < 3) || (Argc > 4)) {
    PrintUsage ();
    return 1;
  }

  RemoteFilename = Argv[1];

  //
  // Parse the destination address; must be a non-zero hex value.
  //
  DestAddress = StrHexToUint64 (Argv[2]);
  if (DestAddress == 0) {
    Print (
      L"\n  ERROR: Invalid destination address '%s'.\n"
      L"         Provide a non-zero hexadecimal address, e.g. 0x80000000.\n\n",
      Argv[2]
      );
    return 1;
  }

  //
  // Parse the optional maximum transfer size.
  //
  if (Argc == 4) {
    if ((StrnCmp (Argv[3], L"0x", 2) == 0) ||
        (StrnCmp (Argv[3], L"0X", 2) == 0))
    {
      MaxSize = StrHexToUint64 (Argv[3]);
    } else {
      MaxSize = StrDecimalToUint64 (Argv[3]);
    }

    if (MaxSize == 0) {
      Print (L"\n  ERROR: Invalid maximum size '%s'.\n\n", Argv[3]);
      return 1;
    }
  } else {
    MaxSize = TFTP_DEFAULT_MAX_SIZE;
  }

  //
  // Retrieve the TFTP server address from the shell environment.
  //
  ServerIpStr = ShellGetEnvironmentVariable (L"serverip");
  if (ServerIpStr == NULL) {
    Print (
      L"\n  ERROR: Environment variable 'serverip' is not set.\n"
      L"         Set it with:  set serverip <ip_address>\n\n"
      );
    return 1;
  }

  Status = ParseIpv4Address (ServerIpStr, &ServerIp);
  if (EFI_ERROR (Status)) {
    Print (
      L"\n  ERROR: 'serverip' value '%s' is not a valid IPv4 address: %r\n\n",
      ServerIpStr, Status
      );
    return 1;
  }

  //
  // Cast the parsed address to a raw pointer.
  // The caller is responsible for ensuring the destination region is
  // mapped, writable, and large enough to hold MaxSize bytes.
  //
  DestBuffer = (VOID *)(UINTN)DestAddress;

  //
  // Print a summary banner.
  //
  Print (L"\n%s v%s\n\n", TFTPCOPY_APP_NAME, TFTPCOPY_VERSION_STRING);
  Print (
    L"  Server IP   : %s (%d.%d.%d.%d)\n",
    ServerIpStr,
    ServerIp.Addr[0], ServerIp.Addr[1],
    ServerIp.Addr[2], ServerIp.Addr[3]
    );
  Print (L"  Remote file : %s\n",          RemoteFilename);
  Print (L"  Destination : 0x%016Lx\n",    DestAddress);
  Print (L"  Max size    : 0x%Lx (%Lu B)\n\n", MaxSize, MaxSize);

  //
  // Execute the transfer.
  //
  TransferredSize = 0;
  Status = DownloadFile (
             &ServerIp,
             RemoteFilename,
             DestBuffer,
             MaxSize,
             &TransferredSize
             );

  if (!EFI_ERROR (Status)) {
    Print (
      L"\n  SUCCESS: %Lu bytes loaded to 0x%016Lx - 0x%016Lx\n\n",
      TransferredSize,
      DestAddress,
      DestAddress + TransferredSize - 1
      );
    return 0;
  }

  Print (L"\n  FAILED: %r\n\n", Status);
  return 2;
}
