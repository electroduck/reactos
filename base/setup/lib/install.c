/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS Setup Library
 * FILE:            base/setup/lib/install.c
 * PURPOSE:         Installation functions
 * PROGRAMMERS:     Hervé Poussineau (hpoussin@reactos.org)
 *                  Hermes Belusca-Maito (hermes.belusca@sfr.fr)
 */

/* INCLUDES *****************************************************************/

#include "precomp.h"
#include "filesup.h"
#include "infsupp.h"

#include "setuplib.h" // HAXX for USETUP_DATA!!

#include "install.h"

#define NDEBUG
#include <debug.h>

static NTSTATUS
IndexOEMFolders(
    IN OUT PUSETUP_DATA pSetupData);

static NTSTATUS
IndexOEMSubfolder(
    IN OUT PUSETUP_DATA pSetupData,
    IN PCWSTR pcwzSrcFolderName,
    IN PCWSTR pcwzDestRelPath);

static NTSTATUS
OpenOEMSourceFolder(
    OUT PHANDLE pHandle,
    IN PCWSTR pcwzPath);

static NTSTATUS
CreateOEMDestFolder(
    IN PCWSTR pcwzPath);

/* FUNCTIONS ****************************************************************/

static BOOL
LookupDirectoryById(
    IN HINF InfHandle,
    IN OUT PINFCONTEXT InfContext,
    IN PCWSTR DirId,
    OUT PCWSTR* pDirectory)
{
    BOOL Success;

    // ReactOS-specific
    Success = SpInfFindFirstLine(InfHandle, L"Directories", DirId, InfContext);
    if (!Success)
    {
        // Windows-compatible
        Success = SpInfFindFirstLine(InfHandle, L"WinntDirectories", DirId, InfContext);
        if (!Success)
            DPRINT1("SpInfFindFirstLine() failed\n");
    }
    if (Success)
    {
        Success = INF_GetData(InfContext, NULL, pDirectory);
        if (!Success)
            DPRINT1("INF_GetData() failed\n");
    }

    if (!Success)
        DPRINT1("LookupDirectoryById(%S) - directory not found!\n", DirId);

    return Success;
}

/*
 * Note: Modeled after SetupGetSourceFileLocation(), SetupGetSourceInfo()
 * and SetupGetTargetPath() APIs.
 * Technically the target path is the same for a given file section,
 * but here we try to remove this constraint.
 *
 * TXTSETUP.SIF entries syntax explained at:
 * http://www.msfn.org/board/topic/125480-txtsetupsif-syntax/
 */
static NTSTATUS
GetSourceFileAndTargetLocation(
    IN HINF InfHandle,
    IN PINFCONTEXT InfContext OPTIONAL,
    IN PCWSTR SourceFileName OPTIONAL,
    OUT PCWSTR* pSourceRootPath,
    OUT PCWSTR* pSourcePath,
    OUT PCWSTR* pTargetDirectory,
    OUT PCWSTR* pTargetFileName)
{
    BOOL Success;
    INFCONTEXT FileContext;
    INFCONTEXT DirContext;
    PCWSTR SourceRootDirId;
    PCWSTR SourceRootDir;
    PCWSTR SourceRelativePath;
    PCWSTR TargetDirId;
    PCWSTR TargetDir;
    PCWSTR TargetFileName;

    /* Either InfContext or SourceFileName must be specified */
    if (!InfContext && !SourceFileName)
        return STATUS_INVALID_PARAMETER;

    /* InfContext to a file was not given, retrieve one corresponding to SourceFileName */
    if (!InfContext)
    {
        /* Search for the SourceDisksFiles section */

        /* Search in the optional platform-specific first (currently hardcoded; make it runtime-dependent?) */
        Success = SpInfFindFirstLine(InfHandle, L"SourceDisksFiles." INF_ARCH, SourceFileName, &FileContext);
        if (!Success)
        {
            /* Search in the global section */
            Success = SpInfFindFirstLine(InfHandle, L"SourceDisksFiles", SourceFileName, &FileContext);
        }
        if (!Success)
        {
            // pSetupData->LastErrorNumber = ERROR_TXTSETUP_SECTION;
            // if (pSetupData->ErrorRoutine)
                // pSetupData->ErrorRoutine(pSetupData, SectionName);
            return STATUS_NOT_FOUND;
        }
        InfContext = &FileContext;
    }
    // else, InfContext != NULL and ignore SourceFileName (that may or may not be == NULL).

    /*
     * Getting Source File Location -- SetupGetSourceFileLocation()
     */

    /* Get source root directory id */
    if (!INF_GetDataField(InfContext, 1, &SourceRootDirId))
    {
        /* FIXME: Handle error! */
        DPRINT1("INF_GetData() failed\n");
        return STATUS_NOT_FOUND;
    }

    /* Lookup source root directory -- SetupGetSourceInfo() */
    /* Search in the optional platform-specific first (currently hardcoded; make it runtime-dependent?) */
    Success = SpInfFindFirstLine(InfHandle, L"SourceDisksNames." INF_ARCH, SourceRootDirId, &DirContext);
    if (!Success)
    {
        /* Search in the global section */
        Success = SpInfFindFirstLine(InfHandle, L"SourceDisksNames", SourceRootDirId, &DirContext);
        if (!Success)
            DPRINT1("SpInfFindFirstLine(\"SourceDisksNames\", \"%S\") failed\n", SourceRootDirId);
    }
    INF_FreeData(SourceRootDirId);
    if (!Success)
    {
        /* FIXME: Handle error! */
        // pSetupData->LastErrorNumber = ERROR_TXTSETUP_SECTION;
        // if (pSetupData->ErrorRoutine)
            // pSetupData->ErrorRoutine(pSetupData, SectionName);
        return STATUS_NOT_FOUND;
    }
    if (!INF_GetDataField(&DirContext, 4, &SourceRootDir))
    {
        /* FIXME: Handle error! */
        DPRINT1("INF_GetData() failed\n");
        return STATUS_NOT_FOUND;
    }

    /* Get optional source relative directory */
    if (!INF_GetDataField(InfContext, 2, &SourceRelativePath))
    {
        SourceRelativePath = NULL;
    }
    else if (!*SourceRelativePath)
    {
        INF_FreeData(SourceRelativePath);
        SourceRelativePath = NULL;
    }
    if (!SourceRelativePath)
    {
        /* Use WinPE directory instead */
        if (INF_GetDataField(InfContext, 13, &TargetDirId))
        {
            /* Lookup directory */
            Success = LookupDirectoryById(InfHandle, &DirContext, TargetDirId, &SourceRelativePath);
            INF_FreeData(TargetDirId);
            if (!Success)
            {
                SourceRelativePath = NULL;
            }
            else if (!*SourceRelativePath)
            {
                INF_FreeData(SourceRelativePath);
                SourceRelativePath = NULL;
            }
        }
    }

    /*
     * Getting Target File Location -- SetupGetTargetPath()
     */

    /* Get target directory id */
    if (!INF_GetDataField(InfContext, 8, &TargetDirId))
    {
        /* FIXME: Handle error! */
        DPRINT1("INF_GetData() failed\n");
        INF_FreeData(SourceRelativePath);
        INF_FreeData(SourceRootDir);
        return STATUS_NOT_FOUND;
    }

    /* Lookup target directory */
    Success = LookupDirectoryById(InfHandle, &DirContext, TargetDirId, &TargetDir);
    INF_FreeData(TargetDirId);
    if (!Success)
    {
        /* FIXME: Handle error! */
        INF_FreeData(SourceRelativePath);
        INF_FreeData(SourceRootDir);
        return STATUS_NOT_FOUND;
    }

    /* Get optional target file name */
    if (!INF_GetDataField(InfContext, 11, &TargetFileName))
        TargetFileName = NULL;
    else if (!*TargetFileName)
        TargetFileName = NULL;

    DPRINT("GetSourceFileAndTargetLocation(%S) = "
           "SrcRootDir: '%S', SrcRelPath: '%S' --> TargetDir: '%S', TargetFileName: '%S'\n",
           SourceFileName, SourceRootDir, SourceRelativePath, TargetDir, TargetFileName);

#if 0
    INF_FreeData(TargetDir);
    INF_FreeData(TargetFileName);
    INF_FreeData(SourceRelativePath);
    INF_FreeData(SourceRootDir);
#endif

    *pSourceRootPath  = SourceRootDir;
    *pSourcePath      = SourceRelativePath;
    *pTargetDirectory = TargetDir;
    *pTargetFileName  = TargetFileName;

    return STATUS_SUCCESS;
}

static NTSTATUS
BuildFullDirectoryPath(
    IN PCWSTR RootPath,
    IN PCWSTR BasePath,
    IN PCWSTR RelativePath,
    OUT PWSTR FullPath,
    IN SIZE_T cchFullPathSize)
{
    NTSTATUS Status;

    if ((RelativePath[0] == UNICODE_NULL) || (RelativePath[0] == L'\\' && RelativePath[1] == UNICODE_NULL))
    {
        /* Installation path */
        DPRINT("InstallationPath: '%S'\n", RelativePath);

        Status = CombinePaths(FullPath, cchFullPathSize, 2,
                              RootPath, BasePath);

        DPRINT("InstallationPath(2): '%S'\n", FullPath);
    }
    else if (RelativePath[0] == L'\\')
    {
        /* Absolute path */
        DPRINT("AbsolutePath: '%S'\n", RelativePath);

        Status = CombinePaths(FullPath, cchFullPathSize, 2,
                              RootPath, RelativePath);

        DPRINT("AbsolutePath(2): '%S'\n", FullPath);
    }
    else // if (RelativePath[0] != L'\\')
    {
        /* Path relative to the installation path */
        DPRINT("RelativePath: '%S'\n", RelativePath);

        Status = CombinePaths(FullPath, cchFullPathSize, 3,
                              RootPath, BasePath, RelativePath);

        DPRINT("RelativePath(2): '%S'\n", FullPath);
    }

    return Status;
}


/*
 * This code enumerates the list of files in reactos.dff / reactos.inf
 * that need to be extracted from reactos.cab and be installed in their
 * respective directories.
 */
/*
 * IMPORTANT NOTE: The INF file specification used for the .CAB in ReactOS
 * is not compliant with respect to TXTSETUP.SIF syntax or the standard syntax.
 */
static BOOLEAN
AddSectionToCopyQueueCab(
    IN PUSETUP_DATA pSetupData,
    IN HINF InfFile,
    IN PCWSTR SectionName,
    IN PCWSTR SourceCabinet,
    IN PCUNICODE_STRING DestinationPath)
{
    BOOLEAN Success;
    NTSTATUS Status;
    INFCONTEXT FilesContext;
    INFCONTEXT DirContext;
    PCWSTR SourceFileName;
    PCWSTR TargetDirId;
    PCWSTR TargetDir;
    PCWSTR TargetFileName;
    WCHAR FileDstPath[MAX_PATH];

    /* Search for the SectionName section */
    if (!SpInfFindFirstLine(InfFile, SectionName, NULL, &FilesContext))
    {
        DPRINT1("AddSectionToCopyQueueCab(): Unable to find section '%S' in cabinet file\n", SectionName);
        return FALSE;
    }

    /*
     * Enumerate the files in the section and add them to the file queue.
     */
    do
    {
        /* Get source file name and target directory id */
        if (!INF_GetData(&FilesContext, &SourceFileName, &TargetDirId))
        {
            /* FIXME: Handle error! */
            DPRINT1("INF_GetData() failed\n");
            break;
        }

        /* Get optional target file name */
        if (!INF_GetDataField(&FilesContext, 2, &TargetFileName))
        {
            TargetFileName = NULL;
        }
        else if (!*TargetFileName)
        {
            INF_FreeData(TargetFileName);
            TargetFileName = NULL;
        }

        /* Lookup target directory */
        Success = LookupDirectoryById(InfFile, &DirContext, TargetDirId, &TargetDir);
        INF_FreeData(TargetDirId);
        if (!Success)
        {
            /* FIXME: Handle error! */
            INF_FreeData(TargetFileName);
            INF_FreeData(SourceFileName);
            break;
        }

        DPRINT("GetSourceTargetFromCab(%S) = "
               "SrcRootDir: '%S', SrcRelPath: '%S' --> TargetDir: '%S', TargetFileName: '%S'\n",
               SourceFileName,
               pSetupData->SourcePath.Buffer,
               pSetupData->SourceRootDir.Buffer,
               TargetDir, TargetFileName);

        Status = CombinePaths(FileDstPath, ARRAYSIZE(FileDstPath), 2,
                              pSetupData->DestinationPath.Buffer,
                              TargetDir);
        UNREFERENCED_PARAMETER(Status);
        DPRINT("  --> FileDstPath = '%S'\n", FileDstPath);

        INF_FreeData(TargetDir);

        if (!SpFileQueueCopy((HSPFILEQ)pSetupData->SetupFileQueue,
                             pSetupData->SourcePath.Buffer, // SourcePath == SourceRootPath ++ SourceRootDir
                             NULL,
                             SourceFileName,
                             NULL,
                             SourceCabinet,
                             NULL,
                             FileDstPath,
                             TargetFileName,
                             0 /* FIXME */))
        {
            /* FIXME: Handle error! */
            DPRINT1("SpFileQueueCopy() failed\n");
        }

        INF_FreeData(TargetFileName);
        INF_FreeData(SourceFileName);

    } while (SpInfFindNextLine(&FilesContext, &FilesContext));

    return TRUE;
}

// Note: Modeled after the SetupQueueCopySection() API
/*
BOOL SetupQueueCopySection(
  _In_ HSPFILEQ QueueHandle,
  _In_ PCTSTR   SourceRootPath,
  _In_ HINF     InfHandle,
  _In_ HINF     ListInfHandle,
  _In_ PCTSTR   Section,
  _In_ DWORD    CopyStyle
);
*/
static BOOLEAN
AddSectionToCopyQueue(
    IN PUSETUP_DATA pSetupData,
    IN HINF InfFile,
    IN PCWSTR SectionName,
    IN PCUNICODE_STRING DestinationPath)
{
    NTSTATUS Status;
    INFCONTEXT FilesContext;
    PCWSTR SourceFileName;
    PCWSTR SourceRootPath;
    PCWSTR SourcePath;
    PCWSTR TargetDirectory;
    PCWSTR TargetFileName;
    WCHAR FileSrcRootPath[MAX_PATH];
    WCHAR FileDstPath[MAX_PATH];

    /*
     * This code enumerates the list of files in txtsetup.sif
     * that need to be installed in their respective directories.
     */

    /* Search for the SectionName section */
    if (!SpInfFindFirstLine(InfFile, SectionName, NULL, &FilesContext))
    {
        DPRINT1("AddSectionToCopyQueue(): Unable to find section '%S' in TXTSETUP.SIF\n", SectionName);
        return FALSE;
    }

    /*
     * Enumerate the files in the section and add them to the file queue.
     */
    do
    {
        /* Get source file name */
        if (!INF_GetDataField(&FilesContext, 0, &SourceFileName))
        {
            /* FIXME: Handle error! */
            DPRINT1("INF_GetData() failed\n");
            break;
        }

        Status = GetSourceFileAndTargetLocation(InfFile,
                                                &FilesContext,
                                                SourceFileName,
                                                &SourceRootPath, // SourceRootDir
                                                &SourcePath,
                                                &TargetDirectory,
                                                &TargetFileName);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Could not find source and target location for file '%S'\n", SourceFileName);
            INF_FreeData(SourceFileName);

            // FIXME: Another error?
            pSetupData->LastErrorNumber = ERROR_TXTSETUP_SECTION;
            if (pSetupData->ErrorRoutine)
                pSetupData->ErrorRoutine(pSetupData, SectionName);
            return FALSE;
            // break;
        }
        /*
         * SourcePath: '\Device\CdRom0\I386'
         * SourceRootPath: '\Device\CdRom0'
         * SourceRootDir: '\I386'
         */

        Status = CombinePaths(FileSrcRootPath, ARRAYSIZE(FileSrcRootPath), 2,
                              pSetupData->SourceRootPath.Buffer,
                              SourceRootPath);
        UNREFERENCED_PARAMETER(Status);
        // DPRINT1("Could not build the full path for '%S', skipping...\n", SourceRootPath);
        DPRINT("  --> FileSrcRootPath = '%S'\n", FileSrcRootPath);

        INF_FreeData(SourceRootPath);

        Status = CombinePaths(FileDstPath, ARRAYSIZE(FileDstPath), 2,
                              pSetupData->DestinationPath.Buffer,
                              TargetDirectory);
        UNREFERENCED_PARAMETER(Status);
        // DPRINT1("Could not build the full path for '%S', skipping...\n", TargetDirectory);
        DPRINT("  --> FileDstPath = '%S'\n", FileDstPath);

        INF_FreeData(TargetDirectory);

        if (!SpFileQueueCopy((HSPFILEQ)pSetupData->SetupFileQueue,
                             FileSrcRootPath,
                             SourcePath,
                             SourceFileName,
                             NULL,
                             NULL, // No SourceCabinet
                             NULL,
                             FileDstPath,
                             TargetFileName,
                             0 /* FIXME */))
        {
            /* FIXME: Handle error! */
            DPRINT1("SpFileQueueCopy() failed\n");
        }

        INF_FreeData(TargetFileName);
        INF_FreeData(SourcePath);
        INF_FreeData(SourceFileName);

    } while (SpInfFindNextLine(&FilesContext, &FilesContext));

    return TRUE;
}

BOOLEAN // ERROR_NUMBER
PrepareCopyInfFile(
    IN OUT PUSETUP_DATA pSetupData,
    IN HINF InfFile,
    IN PCWSTR SourceCabinet OPTIONAL)
{
    BOOLEAN Success;
    NTSTATUS Status;
    INFCONTEXT DirContext;
    PWCHAR AdditionalSectionName = NULL;
    PCWSTR DirKeyValue;
    WCHAR PathBuffer[MAX_PATH];

    if (SourceCabinet == NULL)
    {
        /* Add common files -- Search for the SourceDisksFiles section */
        /* Search in the optional platform-specific first (currently hardcoded; make it runtime-dependent?) */
        Success = AddSectionToCopyQueue(pSetupData, InfFile,
                                        L"SourceDisksFiles." INF_ARCH,
                                        &pSetupData->DestinationPath);
        if (!Success)
        {
            DPRINT1("AddSectionToCopyQueue(%S) failed!\n", L"SourceDisksFiles." INF_ARCH);
        }
        /* Search in the global section */
        Success = AddSectionToCopyQueue(pSetupData, InfFile,
                                        L"SourceDisksFiles",
                                        &pSetupData->DestinationPath);
        if (!Success)
        {
            DPRINT1("AddSectionToCopyQueue(%S) failed!\n", L"SourceDisksFiles");
            pSetupData->LastErrorNumber = ERROR_TXTSETUP_SECTION;
            if (pSetupData->ErrorRoutine)
                pSetupData->ErrorRoutine(pSetupData, L"SourceDisksFiles");
            return FALSE;
        }

        /* Add specific files depending of computer type */
        if (!ProcessComputerFiles(InfFile, pSetupData->ComputerList, &AdditionalSectionName))
            return FALSE;

        if (AdditionalSectionName &&
            !AddSectionToCopyQueue(pSetupData, InfFile,
                                   AdditionalSectionName,
                                   &pSetupData->DestinationPath))
        {
            pSetupData->LastErrorNumber = ERROR_TXTSETUP_SECTION;
            if (pSetupData->ErrorRoutine)
                pSetupData->ErrorRoutine(pSetupData, AdditionalSectionName);
            return FALSE;
        }
    }
    else
    {
        /* Process a cabinet INF */
        Success = AddSectionToCopyQueueCab(pSetupData, InfFile,
                                           L"SourceFiles",
                                           SourceCabinet,
                                           &pSetupData->DestinationPath);
        if (!Success)
        {
            DPRINT1("AddSectionToCopyQueueCab(%S) failed!\n", SourceCabinet);
            pSetupData->LastErrorNumber = ERROR_CABINET_SECTION;
            if (pSetupData->ErrorRoutine)
                pSetupData->ErrorRoutine(pSetupData, L"SourceFiles");
            return FALSE;
        }
    }

    /* Create directories */

    /*
     * NOTE: This is technically optional since SpFileQueueCommit()
     * does that. This is however needed if one wants to create
     * empty directories.
     */

    /*
     * FIXME:
     * Copying files to pSetupData->DestinationRootPath should be done from within
     * the SystemPartitionFiles section.
     * At the moment we check whether we specify paths like '\foo' or '\\' for that.
     * For installing to pSetupData->DestinationPath specify just '\' .
     */

    /* Get destination path */
    RtlStringCchCopyW(PathBuffer, ARRAYSIZE(PathBuffer),
                      pSetupData->DestinationPath.Buffer);

    DPRINT("FullPath(1): '%S'\n", PathBuffer);

    /* Create the install directory */
    Status = SetupCreateDirectory(PathBuffer);
    if (!NT_SUCCESS(Status) && Status != STATUS_OBJECT_NAME_COLLISION)
    {
        DPRINT1("Creating directory '%S' failed: Status = 0x%08lx\n", PathBuffer, Status);
        pSetupData->LastErrorNumber = ERROR_CREATE_INSTALL_DIR;
        if (pSetupData->ErrorRoutine)
            pSetupData->ErrorRoutine(pSetupData, PathBuffer);
        return FALSE;
    }

    /* Search for the 'Directories' section */
    // ReactOS-specific
    if (!SpInfFindFirstLine(InfFile, L"Directories", NULL, &DirContext))
    {
        // Windows-compatible
        if (!SpInfFindFirstLine(InfFile, L"WinntDirectories", NULL, &DirContext))
        {
            if (SourceCabinet)
                pSetupData->LastErrorNumber = ERROR_CABINET_SECTION;
            else
                pSetupData->LastErrorNumber = ERROR_TXTSETUP_SECTION;

            if (pSetupData->ErrorRoutine)
                pSetupData->ErrorRoutine(pSetupData, L"Directories");
            return FALSE;
        }
    }

    /* Enumerate the directory values and create the subdirectories */
    do
    {
        if (!INF_GetData(&DirContext, NULL, &DirKeyValue))
        {
            DPRINT1("break\n");
            break;
        }

        Status = BuildFullDirectoryPath(pSetupData->DestinationRootPath.Buffer,
                                        pSetupData->InstallPath.Buffer,
                                        DirKeyValue,
                                        PathBuffer,
                                        ARRAYSIZE(PathBuffer));
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Could not build the full path for '%S', skipping...\n", DirKeyValue);
            INF_FreeData(DirKeyValue);
            continue;
        }

        if ((DirKeyValue[0] == UNICODE_NULL) || (DirKeyValue[0] == L'\\' && DirKeyValue[1] == UNICODE_NULL))
        {
            /*
             * Installation path -- No need to create it
             * because it has been already created above.
             */
        }
        else
        {
            /* Arbitrary path -- Create it */
            Status = SetupCreateDirectory(PathBuffer);
            if (!NT_SUCCESS(Status) && Status != STATUS_OBJECT_NAME_COLLISION)
            {
                INF_FreeData(DirKeyValue);
                DPRINT("Creating directory '%S' failed: Status = 0x%08lx", PathBuffer, Status);
                pSetupData->LastErrorNumber = ERROR_CREATE_DIR;
                if (pSetupData->ErrorRoutine)
                    pSetupData->ErrorRoutine(pSetupData, PathBuffer);
                return FALSE;
            }
        }

        INF_FreeData(DirKeyValue);
    } while (SpInfFindNextLine(&DirContext, &DirContext));

    return TRUE;
}


// #define USE_CABINET_INF

BOOLEAN // ERROR_NUMBER
PrepareFileCopy(
    IN OUT PUSETUP_DATA pSetupData,
    IN PFILE_COPY_STATUS_ROUTINE StatusRoutine OPTIONAL)
{
    HINF InfHandle;
    INFCONTEXT CabinetsContext;
    PCWSTR CabinetName;
    UINT ErrorLine;
#if defined(__REACTOS__) && defined(USE_CABINET_INF)
    ULONG InfFileSize;
    PVOID InfFileData;
    CABINET_CONTEXT CabinetContext;
#endif
    WCHAR PathBuffer[MAX_PATH];
    NTSTATUS status;

    /* Create the file queue */
    pSetupData->SetupFileQueue = (PVOID)SpFileQueueOpen();
    if (pSetupData->SetupFileQueue == NULL)
    {
        pSetupData->LastErrorNumber = ERROR_COPY_QUEUE;
        if (pSetupData->ErrorRoutine)
            pSetupData->ErrorRoutine(pSetupData);
        return FALSE;
    }

    /* Prepare the copy of the common files that are not in installation cabinets */
    if (!PrepareCopyInfFile(pSetupData, pSetupData->SetupInf, NULL))
    {
        /* FIXME: show an error dialog */
        return FALSE;
    }

    /* Search for the 'Cabinets' section */
    if (!SpInfFindFirstLine(pSetupData->SetupInf, L"Cabinets", NULL, &CabinetsContext))
    {
        /* Skip this step and return success if no cabinet file is listed */
        return TRUE;
    }

    /*
     * Enumerate the installation cabinets listed in the
     * 'Cabinets' section and parse their inf files.
     */
    do
    {
        if (!INF_GetData(&CabinetsContext, NULL, &CabinetName))
            break;

        CombinePaths(PathBuffer, ARRAYSIZE(PathBuffer), 2,
                     pSetupData->SourcePath.Buffer, CabinetName);

#if defined(__REACTOS__) && defined(USE_CABINET_INF)
        INF_FreeData(CabinetName);

        CabinetInitialize(&CabinetContext);
        CabinetSetEventHandlers(&CabinetContext, NULL, NULL, NULL);
        CabinetSetCabinetName(&CabinetContext, PathBuffer);

        if (CabinetOpen(&CabinetContext) == CAB_STATUS_SUCCESS)
        {
            DPRINT("Cabinet %S\n", PathBuffer);

            InfFileData = CabinetGetCabinetReservedArea(&CabinetContext, &InfFileSize);
            if (InfFileData == NULL)
            {
                CabinetCleanup(&CabinetContext);

                pSetupData->LastErrorNumber = ERROR_CABINET_SCRIPT;
                if (pSetupData->ErrorRoutine)
                    pSetupData->ErrorRoutine(pSetupData, PathBuffer);
                return FALSE;
            }
        }
        else
        {
            DPRINT("Cannot open cabinet: %S.\n", PathBuffer);
            CabinetCleanup(&CabinetContext);

            pSetupData->LastErrorNumber = ERROR_CABINET_MISSING;
            if (pSetupData->ErrorRoutine)
                pSetupData->ErrorRoutine(pSetupData, PathBuffer);
            return FALSE;
        }

        InfHandle = INF_OpenBufferedFileA((PSTR)InfFileData,
                                          InfFileSize,
                                          NULL,
                                          INF_STYLE_WIN4,
                                          pSetupData->LanguageId,
                                          &ErrorLine);

        CabinetCleanup(&CabinetContext);
#else
        {
        PWCHAR ptr;

        /* First find the filename */
        ptr = wcsrchr(PathBuffer, L'\\');
        if (!ptr) ptr = PathBuffer;

        /* Then find its extension */
        ptr = wcsrchr(ptr, L'.');
        if (!ptr)
            ptr = PathBuffer + wcslen(PathBuffer);

        /* Replace it by '.inf' */
        wcscpy(ptr, L".inf");

        InfHandle = SpInfOpenInfFile(PathBuffer,
                                     NULL,
                                     INF_STYLE_WIN4,
                                     pSetupData->LanguageId,
                                     &ErrorLine);
        }
#endif

        if (InfHandle == INVALID_HANDLE_VALUE)
        {
            pSetupData->LastErrorNumber = ERROR_INVALID_CABINET_INF;
            if (pSetupData->ErrorRoutine)
                pSetupData->ErrorRoutine(pSetupData, PathBuffer);
            return FALSE;
        }

        if (!PrepareCopyInfFile(pSetupData, InfHandle, CabinetName))
        {
#if !(defined(__REACTOS__) && defined(USE_CABINET_INF))
            SpInfCloseInfFile(InfHandle);
#endif
            /* FIXME: show an error dialog */
            return FALSE;
        }

#if !(defined(__REACTOS__) && defined(USE_CABINET_INF))
        SpInfCloseInfFile(InfHandle);
#endif
    } while (SpInfFindNextLine(&CabinetsContext, &CabinetsContext));

    // Add files in $OEM$ subfolders to the list
    status = IndexOEMFolders(pSetupData);
    if (status != STATUS_SUCCESS)
    {
        // Not sure which error number should be used if this fails...
        DPRINT1("Error 0x%08X indexing OEM subfolders", status);
        pSetupData->LastErrorNumber = ERROR_COPY_QUEUE;
        if (pSetupData->ErrorRoutine)
            pSetupData->ErrorRoutine(pSetupData);
        return FALSE;
    }

    return TRUE;
}

BOOLEAN
DoFileCopy(
    IN OUT PUSETUP_DATA pSetupData,
    IN PSP_FILE_CALLBACK_W MsgHandler,
    IN PVOID Context OPTIONAL)
{
    BOOLEAN Success;

    Success = SpFileQueueCommit(NULL,
                                (HSPFILEQ)pSetupData->SetupFileQueue,
                                MsgHandler,
                                Context);

    SpFileQueueClose((HSPFILEQ)pSetupData->SetupFileQueue);
    pSetupData->SetupFileQueue = NULL;

    return Success;
}

/*
(base/setup/lib/install.c:874) SourceRootPath: 0x00540D40 \Device\CdRom0
(base/setup/lib/install.c:875) SourceRootDir: 0x00540D20 \reactos
(base/setup/lib/install.c:876) SourcePath: 0x00540CE8 \Device\CdRom0\reactos
(base/setup/lib/install.c:877) SystemRootPath: 0x00000000
(base/setup/lib/install.c:878) DestinationArcPath: 0x00547920 multi(0)disk(0)rdisk(0)partition(1)\ReactOS
(base/setup/lib/install.c:879) DestinationPath: 0x00547980 \Device\Harddisk0\Partition1\ReactOS
(base/setup/lib/install.c:880) DestinationRootPath: 0x005478D8 \Device\Harddisk0\Partition1\
(base/setup/lib/install.c:881) InstallPath: 0x005479D8 \ReactOS
*/

static NTSTATUS
IndexOEMFolders(IN OUT PUSETUP_DATA pSetupData)
{
    HANDLE hOEMDir;
    NTSTATUS status;
    WCHAR wszOEMDirPath[MAX_PATH];

    DPRINT1("SourceRootPath: 0x%08X %ws\r\n", pSetupData->SourceRootPath.Buffer, pSetupData->SourceRootPath.Buffer);
    DPRINT1("SourceRootDir: 0x%08X %ws\r\n", pSetupData->SourceRootDir.Buffer, pSetupData->SourceRootDir.Buffer);
    DPRINT1("SourcePath: 0x%08X %ws\r\n", pSetupData->SourcePath.Buffer, pSetupData->SourcePath.Buffer);
    DPRINT1("SystemRootPath: 0x%08X\r\n", pSetupData->SystemRootPath.Buffer);
    DPRINT1("DestinationArcPath: 0x%08X %ws\r\n", pSetupData->DestinationArcPath.Buffer, pSetupData->DestinationArcPath.Buffer);
    DPRINT1("DestinationPath: 0x%08X %ws\r\n", pSetupData->DestinationPath.Buffer, pSetupData->DestinationPath.Buffer);
    DPRINT1("DestinationRootPath: 0x%08X %ws\r\n", pSetupData->DestinationRootPath.Buffer, pSetupData->DestinationRootPath.Buffer);
    DPRINT1("InstallPath: 0x%08X %ws\r\n", pSetupData->InstallPath.Buffer, pSetupData->InstallPath.Buffer);

    status = CombinePaths(wszOEMDirPath, ARRAYSIZE(wszOEMDirPath), 2, pSetupData->SourceRootPath.Buffer, L"\\$OEM$");
    if (FAILED(status))
    {
        DPRINT1("Error combining paths 0x%08X and 0x%08X\r\n", pSetupData->SourceRootPath.Buffer, L"\\$OEM$");
        return status;
    }

    // Check if $OEM$ exists - exit function if it doesn't
    status = OpenOEMSourceFolder(&hOEMDir, wszOEMDirPath);
    if (status == STATUS_NO_SUCH_FILE)
        return STATUS_SUCCESS; // No $OEM$ folder
    else if (FAILED(status))
    {
        DPRINT1("Error 0x%08X opening OEM source folder 0x%08X\r\n", status, wszOEMDirPath);
        return status;
    }

    NtClose(hOEMDir);
    hOEMDir = NULL;

    // Index $OEM$\$$, which contains files that should be copied to the OS install folder (e.g. C:\ReactOS)
    status = IndexOEMSubfolder(pSetupData, L"\\$$", pSetupData->DestinationPath.Buffer);
    if (FAILED(status) && (status != STATUS_NO_SUCH_FILE))
    {
        DPRINT1(
            "Error 0x%08X indexing OEM subfolder \\$$ with destination 0x%08X\r\n", status,
            pSetupData->DestinationPath.Buffer);
        return status;
    }

    // Index $OEM$\$1, which contains files that should be copied to the system drive (e.g. C:\)
    status = IndexOEMSubfolder(pSetupData, L"\\$1", pSetupData->DestinationRootPath.Buffer);
    if ((status != STATUS_SUCCESS) && (status != STATUS_NO_SUCH_FILE))
    {
        DPRINT1(
            "Error 0x%08X indexing OEM subfolder \\$1 with destination 0x%08X\r\n", status,
            pSetupData->DestinationPath.Buffer);
        return status;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
IndexOEMSubfolder(IN OUT PUSETUP_DATA pSetupData, IN PCWSTR pcwzSrcFolderName, IN PCWSTR pcwzDestPath)
{
    HANDLE hDir;
    NTSTATUS status;
    WCHAR wszSrcPath[MAX_PATH];
    IO_STATUS_BLOCK iosb;
    BYTE arrEntryInfoData[sizeof(FILE_DIRECTORY_INFORMATION) + MAX_PATH * sizeof(WCHAR)];
    PFILE_DIRECTORY_INFORMATION pinfEntry;
    WCHAR wszSubdirSrcName[MAX_PATH];
    WCHAR wszSubdirDestPath[MAX_PATH];
    ULONG nNameLengthChars;

    pinfEntry = (PFILE_DIRECTORY_INFORMATION)&arrEntryInfoData[0];

    status = CombinePaths(
        wszSrcPath, ARRAYSIZE(wszSrcPath), 3, pSetupData->SourceRootPath.Buffer, L"\\$OEM$\\", pcwzSrcFolderName);
    if (status != STATUS_SUCCESS)
    {
        DPRINT1(
            "Error combining paths 0x%08X, 0x%08X, and 0x%08X\r\n", pSetupData->SourceRootPath.Buffer, L"\\$OEM$\\",
            pcwzSrcFolderName);
        return status;
    }

    status = CreateOEMDestFolder(pcwzDestPath);
    if (status != STATUS_SUCCESS)
    {
        DPRINT1("Error 0x%08X creating OEM destination folder 0x%08X\r\n", status, pcwzDestPath);
        return status;
    }

    status = OpenOEMSourceFolder(&hDir, wszSrcPath);
    if (status == STATUS_SUCCESS)
    {
        // Loop through all items (files and directories) in the folder
        while ((status = NtQueryDirectoryFile(
                hDir, NULL, NULL, NULL, &iosb, pinfEntry, sizeof(arrEntryInfoData), FileDirectoryInformation, TRUE,
                NULL, FALSE)) == STATUS_SUCCESS)
        {
            // Other statuses indicate there are no more files
            if (iosb.Status != STATUS_SUCCESS)
                break;

            // Check name length before null terminating
            nNameLengthChars = pinfEntry->FileNameLength / sizeof(pinfEntry->FileName[0]);
            if ((UINT_PTR)(&pinfEntry->FileName[nNameLengthChars]) >
                (UINT_PTR)(&arrEntryInfoData[sizeof(arrEntryInfoData)]))
            {
                DPRINT1("Filename 0x%08X too long to null terminate\r\n", pinfEntry->FileName);
                status = STATUS_NAME_TOO_LONG;
                break;
            }

            // Null terminate
            pinfEntry->FileName[nNameLengthChars] = 0;

            // Ignore 'files' . and ..
            if ((pinfEntry->FileName[0] == L'.') &&
                ((pinfEntry->FileName[1] == '.') || (pinfEntry->FileName[1] == '\0')))
                continue;

            // Check attributes to determine if directory or file
            if (pinfEntry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                // Subdirectory

                status = CombinePaths(
                    wszSubdirSrcName, ARRAYSIZE(wszSubdirSrcName), 2, pcwzSrcFolderName, pinfEntry->FileName);
                if (status != STATUS_SUCCESS)
                {
                    DPRINT1("Error combining paths 0x%08X and 0x%08X\r\n", pcwzSrcFolderName, pinfEntry->FileName);
                    break;
                }

                status = CombinePaths(
                    wszSubdirDestPath, ARRAYSIZE(wszSubdirDestPath), 2, pcwzDestPath, pinfEntry->FileName);
                if (status != STATUS_SUCCESS)
                {
                    DPRINT1("Error combining paths 0x%08X and 0x%08X\r\n", pcwzDestPath, pinfEntry->FileName);
                    break;
                }

                status = IndexOEMSubfolder(pSetupData, wszSubdirSrcName, wszSubdirDestPath);
                if (status != STATUS_SUCCESS)
                {
                    DPRINT1(
                        "Error 0x%08X indexing OEM subfolder 0x%08X with destination 0x%08X\r\n", status, wszSubdirSrcName,
                        wszSubdirDestPath);
                    break;
                }
            }
            else
            {
                // File
                if (!SpFileQueueCopy(
                        pSetupData->SetupFileQueue, wszSrcPath, NULL, pinfEntry->FileName, NULL, NULL, NULL,
                        pcwzDestPath, pinfEntry->FileName, 0))
                {
                    DPRINT1(
                        "Error queueing OEM file 0x%08X for copy from 0x%08X to 0x%08X\r\n", pinfEntry->FileName, wszSrcPath,
                        pcwzDestPath);
                    status = STATUS_PRINT_QUEUE_FULL; // Suggestions welcome
                    break;
                }
            }
        }

        // If we looped through all the files, that was a success
        if (status == STATUS_NO_MORE_FILES)
            status = STATUS_SUCCESS;
        else
            DPRINT1("Error 0x%08X querying OEM source folder 0x%08X\r\n", status, wszSrcPath);
    }
    else
    {
        DPRINT1("Error 0x%08X opening OEM source folder 0x%08X\r\n", status, wszSrcPath);
    }

    return status;
}

static NTSTATUS
OpenOEMSourceFolder(OUT PHANDLE pHandle, IN PCWSTR pcwzPath)
{
    OBJECT_ATTRIBUTES attrOEMDir;
    UNICODE_STRING usOEMDirPath;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status;

    RtlInitUnicodeString(&usOEMDirPath, pcwzPath);
    InitializeObjectAttributes(&attrOEMDir, &usOEMDirPath, OBJ_OPENIF | OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = NtOpenFile(
        pHandle, FILE_DIRECTORY_FILE | FILE_LIST_DIRECTORY | SYNCHRONIZE, &attrOEMDir, &iosb, FILE_SHARE_VALID_FLAGS,
        FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT);

    return status;
}

static NTSTATUS
CreateOEMDestFolder(IN PCWSTR pcwzPath)
{
    OBJECT_ATTRIBUTES attrDir;
    UNICODE_STRING usDirPath;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status;
    HANDLE hDir;

    RtlInitUnicodeString(&usDirPath, pcwzPath);
    InitializeObjectAttributes(&attrDir, &usDirPath, OBJ_OPENIF | OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = NtCreateFile(
        &hDir, FILE_LIST_DIRECTORY | SYNCHRONIZE, &attrDir, &iosb, NULL, FILE_ATTRIBUTE_DIRECTORY,
        FILE_SHARE_VALID_FLAGS, FILE_OPEN_IF, FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);

    if (SUCCEEDED(status))
        NtClose(hDir);

    return status;
}

/* EOF */
