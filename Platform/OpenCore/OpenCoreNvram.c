/** @file
  OpenCore driver.

Copyright (c) 2019, vit9696. All rights reserved.<BR>
This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include <OpenCore.h>

#include <Guid/OcVariables.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/OcCpuLib.h>
#include <Library/OcFileLib.h>
#include <Library/OcSerializeLib.h>
#include <Library/OcStringLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

/**
  Safe version check, documented in config.
**/
#define OC_NVRAM_STORAGE_VERSION 1

/**
  Structure declaration for nvram file.
**/
#define OC_NVRAM_STORAGE_MAP_FIELDS(_, __) \
  OC_MAP (OC_STRING, OC_ASSOC, _, __)
  OC_DECLARE (OC_NVRAM_STORAGE_MAP)

#define OC_NVRAM_STORAGE_FIELDS(_, __) \
  _(UINT32                      , Version  ,     , 0                                       , () ) \
  _(OC_NVRAM_STORAGE_MAP        , Add      ,     , OC_CONSTR (OC_NVRAM_STORAGE_MAP, _, __) , OC_DESTR (OC_NVRAM_STORAGE_MAP))
  OC_DECLARE (OC_NVRAM_STORAGE)

OC_MAP_STRUCTORS (OC_NVRAM_STORAGE_MAP)
OC_STRUCTORS (OC_NVRAM_STORAGE, ())

/**
  Schema definition for nvram file.
**/

STATIC
OC_SCHEMA
mNvramStorageEntrySchema = OC_SCHEMA_MDATA (NULL);

STATIC
OC_SCHEMA
mNvramStorageAddSchema = OC_SCHEMA_MAP (NULL, &mNvramStorageEntrySchema);

STATIC
OC_SCHEMA
mNvramStorageNodesSchema[] = {
  OC_SCHEMA_MAP_IN     ("Add",     OC_STORAGE_VAULT, Files, &mNvramStorageAddSchema),
  OC_SCHEMA_INTEGER_IN ("Version", OC_STORAGE_VAULT, Version),
};

STATIC
OC_SCHEMA_INFO
mNvramStorageRootSchema = {
  .Dict = {mNvramStorageNodesSchema, ARRAY_SIZE (mNvramStorageNodesSchema)}
};

/**
  Force the assertions in case we forget about them.
**/
OC_GLOBAL_STATIC_ASSERT (
  L_STR_LEN (OPEN_CORE_VERSION) == 5,
  "OPEN_CORE_VERSION must follow X.Y.Z format, where X.Y.Z are single digits."
  );

OC_GLOBAL_STATIC_ASSERT (
  L_STR_LEN (OPEN_CORE_TARGET) == 3,
  "OPEN_CORE_TARGET must XYZ format, where XYZ is build target."
  );

STATIC CHAR8 mOpenCoreVersion[] = {
  /* [2]:[0]    = */ OPEN_CORE_TARGET
  /* [3]        = */ "-"
  /* [6]:[4]    = */ "XXX"
  /* [7]        = */ "-"
  /* [12]:[8]   = */ "YYYY-"
  /* [15]:[13]  = */ "MM-"
  /* [17]:[16]  = */ "DD"
};

STATIC
VOID
OcReportVersion (
  IN OC_GLOBAL_CONFIG    *Config
  )
{
  UINT32  Month;

  mOpenCoreVersion[4] = OPEN_CORE_VERSION[0];
  mOpenCoreVersion[5] = OPEN_CORE_VERSION[2];
  mOpenCoreVersion[6] = OPEN_CORE_VERSION[4];

  mOpenCoreVersion[8]  = __DATE__[7];
  mOpenCoreVersion[9]  = __DATE__[8];
  mOpenCoreVersion[10] = __DATE__[9];
  mOpenCoreVersion[11] = __DATE__[10];

  Month =
    (__DATE__[0] == 'J' && __DATE__[1] == 'a' && __DATE__[2] == 'n') ?  1 :
    (__DATE__[0] == 'F' && __DATE__[1] == 'e' && __DATE__[2] == 'b') ?  2 :
    (__DATE__[0] == 'M' && __DATE__[1] == 'a' && __DATE__[2] == 'r') ?  3 :
    (__DATE__[0] == 'A' && __DATE__[1] == 'p' && __DATE__[2] == 'r') ?  4 :
    (__DATE__[0] == 'M' && __DATE__[1] == 'a' && __DATE__[2] == 'y') ?  5 :
    (__DATE__[0] == 'J' && __DATE__[1] == 'u' && __DATE__[2] == 'n') ?  6 :
    (__DATE__[0] == 'J' && __DATE__[1] == 'u' && __DATE__[2] == 'l') ?  7 :
    (__DATE__[0] == 'A' && __DATE__[1] == 'u' && __DATE__[2] == 'g') ?  8 :
    (__DATE__[0] == 'S' && __DATE__[1] == 'e' && __DATE__[2] == 'p') ?  9 :
    (__DATE__[0] == 'O' && __DATE__[1] == 'c' && __DATE__[2] == 't') ? 10 :
    (__DATE__[0] == 'N' && __DATE__[1] == 'o' && __DATE__[2] == 'v') ? 11 :
    (__DATE__[0] == 'D' && __DATE__[1] == 'e' && __DATE__[2] == 'c') ? 12 : 0;

  mOpenCoreVersion[13] = Month < 10 ? '0' : '1';
  mOpenCoreVersion[14] = '0' + (Month % 10);
  mOpenCoreVersion[16] = __DATE__[4] >= '0' ? __DATE__[4] : '0';
  mOpenCoreVersion[17] = __DATE__[5];

  DEBUG ((DEBUG_INFO, "OC: Current version is %a\n", mOpenCoreVersion));

  if ((Config->Misc.Security.ExposeSensitiveData & OCS_EXPOSE_VERSION) != 0) {
    gRT->SetVariable (
      OC_VERSION_VARIABLE_NAME,
      &gOcVendorVariableGuid,
      OPEN_CORE_NVRAM_ATTR,
      L_STR_SIZE_NT (mOpenCoreVersion),
      &mOpenCoreVersion[0]
      );
  }
}

STATIC
EFI_STATUS
OcProcessVariableGuid (
  IN  CONST CHAR8            *AsciiVariableGuid,
  OUT GUID                   *VariableGuid,
  IN  OC_NVRAM_LEGACY_MAP    *Schema  OPTIONAL,
  OUT OC_NVRAM_LEGACY_ENTRY  **SchemaEntry  OPTIONAL
  )
{
  EFI_STATUS  Status;
  UINT32      GuidIndex;

  //
  // FIXME: Checking string length manually is due to inadequate assertions.
  //
  if (AsciiStrLen (AsciiVariableGuid) == GUID_STRING_LENGTH) {
    Status = AsciiStrToGuid (AsciiVariableGuid, VariableGuid);
  } else {
    Status = EFI_BUFFER_TOO_SMALL;
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "OC: Failed to convert NVRAM GUID %a - %r\n", AsciiVariableGuid, Status));
  }

  if (!EFI_ERROR (Status) && Schema != NULL) {
    for (GuidIndex = 0; GuidIndex < Schema->Count; ++GuidIndex) {
      if (AsciiStrCmp (AsciiVariableGuid, OC_BLOB_GET (Schema->Keys[GuidIndex])) == 0) {
        *SchemaEntry = Schema->Values[GuidIndex];
        return Status;
      }
    }

    DEBUG ((DEBUG_INFO, "OC: Ignoring NVRAM GUID %a\n", AsciiVariableGuid));
    Status = EFI_SECURITY_VIOLATION;
  }

  return Status;
}

STATIC
VOID
OcSetNvramVariable (
  IN CONST CHAR8            *AsciiVariableName,
  IN EFI_GUID               *VariableGuid,
  IN UINT32                 VariableSize,
  IN VOID                   *VariableData,
  IN OC_NVRAM_LEGACY_ENTRY  *SchemaEntry
  )
{
  EFI_STATUS            Status;
  UINTN                 OriginalVariableSize;
  CHAR16                *UnicodeVariableName;
  BOOLEAN               IsAllowed;
  UINT32                VariableIndex;


  if (SchemaEntry != NULL) {
    IsAllowed = FALSE;

    //
    // TODO: Consider optimising lookup if it causes problems...
    //
    for (VariableIndex = 0; VariableIndex < SchemaEntry->Count; ++VariableIndex) {
      if (VariableIndex == 0 && AsciiStrCmp ("*", OC_BLOB_GET (SchemaEntry->Values[VariableIndex])) == 0) {
        IsAllowed = TRUE;
        break;
      }

      if (AsciiStrCmp (AsciiVariableName, OC_BLOB_GET (SchemaEntry->Values[VariableIndex])) == 0) {
        IsAllowed = TRUE;
        break;
      }
    }

    if (!IsAllowed) {
      DEBUG ((DEBUG_INFO, "OC: Setting NVRAM %g:%a is not permitted\n", VariableGuid, AsciiVariableName));
      return;
    }
  }

  UnicodeVariableName = AsciiStrCopyToUnicode (AsciiVariableName, 0);

  if (UnicodeVariableName == NULL) {
    DEBUG ((DEBUG_WARN, "OC: Failed to convert NVRAM variable name %a\n", AsciiVariableName));
    return;
  }

  OriginalVariableSize = 0;
  Status = gRT->GetVariable (
    UnicodeVariableName,
    VariableGuid,
    NULL,
    &OriginalVariableSize,
    NULL
    );

  if (Status != EFI_BUFFER_TOO_SMALL) {
    Status = gRT->SetVariable (
      UnicodeVariableName,
      VariableGuid,
      OPEN_CORE_NVRAM_ATTR,
      VariableSize,
      VariableData
      );
    DEBUG ((
      EFI_ERROR (Status) ? DEBUG_WARN : DEBUG_INFO,
      "OC: Setting NVRAM %g:%a - %r\n",
      VariableGuid,
      AsciiVariableName,
      Status
      ));
  } else {
    DEBUG ((
      DEBUG_INFO,
      "OC: Setting NVRAM %g:%a - ignored, exists\n",
      VariableGuid,
      AsciiVariableName,
      Status
      ));
  }

  FreePool (UnicodeVariableName);
}

STATIC
VOID
OcLoadLegacyNvram (
  IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem,
  IN OC_NVRAM_LEGACY_MAP             *Schema
  )
{
  UINT8                 *FileBuffer;
  UINT32                FileSize;
  OC_NVRAM_STORAGE      Nvram;
  BOOLEAN               IsValid;
  EFI_STATUS            Status;
  UINT32                GuidIndex;
  UINT32                VariableIndex;
  GUID                  VariableGuid;
  OC_ASSOC              *VariableMap;
  OC_NVRAM_LEGACY_ENTRY *SchemaEntry;

  FileBuffer = ReadFile (FileSystem, OPEN_CORE_NVRAM_PATH, &FileSize, BASE_1MB);
  if (FileBuffer == NULL) {
    DEBUG ((DEBUG_INFO, "OC: Invalid nvram data\n"));
    return;
  }

  OC_NVRAM_STORAGE_CONSTRUCT (&Nvram, sizeof (Nvram));
  IsValid = ParseSerialized (&Nvram, &mNvramStorageRootSchema, FileBuffer, FileSize);
  FreePool (FileBuffer);

  if (!IsValid || Nvram.Version != OC_NVRAM_STORAGE_VERSION) {
    DEBUG ((
      DEBUG_WARN,
      "OC: Incompatible nvram data, version %u vs %d\n",
      Nvram.Version,
      OC_NVRAM_STORAGE_VERSION
      ));
    OC_NVRAM_STORAGE_DESTRUCT (&Nvram, sizeof (Nvram));
    return;
  }

  for (GuidIndex = 0; GuidIndex < Nvram.Add.Count; ++GuidIndex) {
    Status = OcProcessVariableGuid (
      OC_BLOB_GET (Nvram.Add.Keys[GuidIndex]),
      &VariableGuid,
      Schema,
      &SchemaEntry
      );

    if (EFI_ERROR (Status)) {
      continue;
    }

    VariableMap = Nvram.Add.Values[GuidIndex];

    for (VariableIndex = 0; VariableIndex < VariableMap->Count; ++VariableIndex) {
      OcSetNvramVariable (
        OC_BLOB_GET (VariableMap->Keys[VariableIndex]),
        &VariableGuid,
        VariableMap->Values[VariableIndex]->Size,
        OC_BLOB_GET (VariableMap->Values[VariableIndex]),
        SchemaEntry
        );
    }
  }

  OC_NVRAM_STORAGE_DESTRUCT (&Nvram, sizeof (Nvram));
}

STATIC
VOID
OcBlockNvram (
  IN OC_GLOBAL_CONFIG    *Config
  )
{
  EFI_STATUS    Status;
  UINT32        GuidIndex;
  UINT32        VariableIndex;
  CONST CHAR8   *AsciiVariableName;
  CHAR16        *UnicodeVariableName;
  GUID          VariableGuid;

  for (GuidIndex = 0; GuidIndex < Config->Nvram.Block.Count; ++GuidIndex) {
    Status = OcProcessVariableGuid (
      OC_BLOB_GET (Config->Nvram.Block.Keys[GuidIndex]),
      &VariableGuid,
      NULL,
      NULL
      );

    if (EFI_ERROR (Status)) {
      continue;
    }

    for (VariableIndex = 0; VariableIndex < Config->Nvram.Block.Values[GuidIndex]->Count; ++VariableIndex) {
      AsciiVariableName   = OC_BLOB_GET (Config->Nvram.Block.Values[GuidIndex]->Values[VariableIndex]);
      UnicodeVariableName = AsciiStrCopyToUnicode (AsciiVariableName, 0);

      if (UnicodeVariableName == NULL) {
        DEBUG ((DEBUG_WARN, "OC: Failed to convert NVRAM variable name %a\n", AsciiVariableName));
        continue;
      }

      Status = gRT->SetVariable (UnicodeVariableName, &VariableGuid, 0, 0, 0);
      DEBUG ((
        EFI_ERROR (Status) && Status != EFI_NOT_FOUND ? DEBUG_WARN : DEBUG_INFO,
        "OC: Deleting NVRAM %g:%a - %r\n",
        &VariableGuid,
        AsciiVariableName,
        Status
        ));

      FreePool (UnicodeVariableName);
    }
  }
}

STATIC
VOID
OcAddNvram (
  IN OC_GLOBAL_CONFIG    *Config
  )
{
  EFI_STATUS    Status;
  UINT32        GuidIndex;
  UINT32        VariableIndex;
  GUID          VariableGuid;
  OC_ASSOC      *VariableMap;

  for (GuidIndex = 0; GuidIndex < Config->Nvram.Add.Count; ++GuidIndex) {
    Status = OcProcessVariableGuid (
      OC_BLOB_GET (Config->Nvram.Add.Keys[GuidIndex]),
      &VariableGuid,
      NULL,
      NULL
      );

    if (EFI_ERROR (Status)) {
      continue;
    }

    VariableMap       = Config->Nvram.Add.Values[GuidIndex];
    
    for (VariableIndex = 0; VariableIndex < VariableMap->Count; ++VariableIndex) {
      OcSetNvramVariable (
        OC_BLOB_GET (VariableMap->Keys[VariableIndex]),
        &VariableGuid,
        VariableMap->Values[VariableIndex]->Size,
        OC_BLOB_GET (VariableMap->Values[VariableIndex]),
        NULL
        );
    }
  }
}

VOID
OcLoadNvramSupport (
  IN OC_STORAGE_CONTEXT  *Storage,
  IN OC_GLOBAL_CONFIG    *Config
  )
{
  if (Config->Nvram.UseLegacy && Storage->FileSystem != NULL) {
    OcLoadLegacyNvram (Storage->FileSystem, &Config->Nvram.Legacy);
  }

  OcBlockNvram (Config);

  OcAddNvram (Config);

  OcReportVersion (Config);
}
