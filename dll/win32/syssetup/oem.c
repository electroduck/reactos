/*
 * COPYRIGHT:         See COPYING in the top level directory
 * PROJECT:           ReactOS system libraries
 * PURPOSE:           System setup: execution of OEM commands
 * FILE:              dll/win32/syssetup/oem.c
 * PROGRAMER:         electroduck (root@electroduck.com)
 */

#include "precomp.h"

#ifndef SETUP_OEM_CDROM_MOUNT
#define SETUP_OEM_CDROM_MOUNT 0
#endif

static void
GetErrorMessage(DWORD nErrorCode, OUT LPWSTR pwzBuffer, SIZE_T nBufferChars);

static const WCHAR CommandsSection[] = L"COMMANDS";
static WCHAR OEMFolderPath[MAX_PATH];

BOOL
FindOEMFolder(void)
{
    WCHAR wszCurrentAttempt[MAX_PATH];
    DWORD dwAttributes;
    unsigned nCDROM;
    WCHAR wcDriveLetter;
#if SETUP_OEM_CDROM_MOUNT
    WCHAR wszError[256];
#endif

    // First, try CD-ROMs
    for (nCDROM = 0; nCDROM < 32; nCDROM++)
    {
        _snwprintf(wszCurrentAttempt, ARRAYSIZE(wszCurrentAttempt), L"\\\\?\\GLOBALROOT\\Device\\CdRom%u\\$OEM$\\", nCDROM);

        dwAttributes = GetFileAttributesW(wszCurrentAttempt);
        if ((dwAttributes != INVALID_FILE_ATTRIBUTES) && (dwAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            // Found it on a CD-ROM
#if SETUP_OEM_CDROM_MOUNT
            /* Some Win32 programs and APIs do not like NT-style Win32 paths.
               The simplest and most robust way to convert the NT-style Win32 path to a DOS-style Win32 path is just to mount it. */

            if (!DefineDosDeviceW(0, L"O:", wszCurrentAttempt))
            {
                _snwprintf(wszError, ARRAYSIZE(wszError), "Error 0x%08X mounting OEM folder \"%ws.\"\r\n"
                    "OEM setup tasks will not be performed.", GetLastError(), wszCurrentAttempt);
                MessageBoxW(NULL, wszError, L"Setup Error", MB_ICONERROR);
                return FALSE;
            }

            wcsncpy(OEMFolderPath, L"O:\\", ARRAYSIZE(OEMFolderPath));
            return TRUE;
#else
            wcsncpy(OEMFolderPath, wszCurrentAttempt, ARRAYSIZE(OEMFolderPath));
            return TRUE;
#endif
        }
    }

    // Next, try all lettered drives
    for (wcDriveLetter = 'A'; wcDriveLetter <= 'Z'; wcDriveLetter++)
    {
        _snwprintf(wszCurrentAttempt, ARRAYSIZE(wszCurrentAttempt), L"%wc:\\$OEM$\\", wcDriveLetter);

        dwAttributes = GetFileAttributesW(wszCurrentAttempt);
        if ((dwAttributes != INVALID_FILE_ATTRIBUTES) && (dwAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            // Found it somewhere
            wcsncpy(OEMFolderPath, wszCurrentAttempt, ARRAYSIZE(OEMFolderPath));
            return TRUE;
        }
    }

    // Not found
    return FALSE;
}

void
ExecuteOEMCommands(void)
{
    WCHAR wszCommandsFile[MAX_PATH];
    HINF hCommandsInf;
    UINT nErrorLine;
    WCHAR wszSysError[256];
    WCHAR wszErrorMessage[512];
    DWORD nErrorCode, nExitCode;
    LONG nCommands, nCurCommand;
    INFCONTEXT ctxCurCommand;
    WCHAR wszCommandLine[4096];
    STARTUPINFOW infoStartup;
    PROCESS_INFORMATION infoProcess;

    ZeroMemory(&infoProcess, sizeof(infoProcess));

    wcsncpy(wszCommandsFile, OEMFolderPath, ARRAYSIZE(wszCommandsFile));
    wcsncat(wszCommandsFile, L"CMDLINES.TXT", ARRAYSIZE(wszCommandsFile) - wcslen(wszCommandsFile));

    // Open CMDLINES.TXT - style must be set to OLDNT, not WIN4
    hCommandsInf = SetupOpenInfFileW(wszCommandsFile, NULL, INF_STYLE_OLDNT, &nErrorLine);
    if ((hCommandsInf == INVALID_HANDLE_VALUE) || (hCommandsInf == NULL))
    {
        nErrorCode = GetLastError();
        if (nErrorCode == ERROR_FILE_NOT_FOUND)
            return; // No CMDLINES.TXT file - that's OK

        GetErrorMessage(nErrorCode, wszSysError, ARRAYSIZE(wszSysError));

        if (nErrorCode == ERROR_OUTOFMEMORY)
            wcsncpy(wszErrorMessage, L"Ran out ot memory while processing CMDLINES.TXT", ARRAYSIZE(wszErrorMessage));
        else
            _snwprintf(wszErrorMessage, ARRAYSIZE(wszErrorMessage), L"Error 0x%08X on line %u of CMDLINES.TXT: %ws", nErrorCode, nErrorLine, wszSysError);

        MessageBoxW(NULL, wszErrorMessage, L"Error reading CMDLINES.TXT", MB_ICONERROR);
        return;
    }

    // Get command count
    nCommands = SetupGetLineCountW(hCommandsInf, CommandsSection);
    if (nCommands <= 0)
    {
        //DbgPrint("CMDLINES.TXT contains no commands (SetupGetLineCountW returned %d).\r\n", nCommands);
        SetupCloseInfFile(hCommandsInf);
        return;
    }

    // Process commands
    for (nCurCommand = 0; nCurCommand < nCommands; nCurCommand++)
    {
    L_retry:
        ZeroMemory(&infoStartup, sizeof(infoStartup));
        infoStartup.cb = sizeof(infoStartup);

        // Get line context
        if (!SetupGetLineByIndexW(hCommandsInf, CommandsSection, nCurCommand, &ctxCurCommand))
        {
            nErrorCode = GetLastError();
            GetErrorMessage(nErrorCode, wszSysError, ARRAYSIZE(wszSysError));
            _snwprintf(
                wszErrorMessage, ARRAYSIZE(wszErrorMessage), L"Error 0x%08X finding command line %d: %ls", nErrorCode,
                nCurCommand, wszSysError);
            goto L_error;
        }

        // Get line data
        if (!SetupGetLineTextW(&ctxCurCommand, NULL, NULL, NULL, wszCommandLine, ARRAYSIZE(wszCommandLine), NULL))
        {
            nErrorCode = GetLastError();
            GetErrorMessage(nErrorCode, wszSysError, ARRAYSIZE(wszSysError));
            _snwprintf(
                wszErrorMessage, ARRAYSIZE(wszErrorMessage), L"Error 0x%08X reading text of command line %d: %ls", nErrorCode,
                nCurCommand, wszSysError);
            goto L_error;
        }

        // Start command
        if (!CreateProcessW(NULL, wszCommandLine, NULL, NULL, FALSE, 0, NULL, OEMFolderPath, &infoStartup, &infoProcess))
        {
            nErrorCode = GetLastError();
            GetErrorMessage(nErrorCode, wszSysError, ARRAYSIZE(wszSysError));
            _snwprintf(
                wszErrorMessage, ARRAYSIZE(wszErrorMessage), L"Error 0x%08X executing command %d: %ls",
                nErrorCode, nCurCommand, wszSysError);
            goto L_error;
        }

        // Wait for exit
        if (WaitForSingleObject(infoProcess.hProcess, INFINITE) != WAIT_OBJECT_0)
        {
            nErrorCode = GetLastError();
            GetErrorMessage(nErrorCode, wszSysError, ARRAYSIZE(wszSysError));
            _snwprintf(
                wszErrorMessage, ARRAYSIZE(wszErrorMessage), L"Error 0x%08X waiting for command %d to exit: %ls", nErrorCode,
                nCurCommand, wszSysError);
            goto L_error;
        }

        // Get exit code
        if (!GetExitCodeProcess(infoProcess.hProcess, &nExitCode))
        {
            nErrorCode = GetLastError();
            GetErrorMessage(nErrorCode, wszSysError, ARRAYSIZE(wszSysError));
            _snwprintf(
                wszErrorMessage, ARRAYSIZE(wszErrorMessage), L"Error 0x%08X getting exit code for command %d: %ls",
                nErrorCode, nCurCommand, wszSysError);
            goto L_error;
        }

        // Check exit code
        if (nExitCode != 0)
        {
            _snwprintf(
                wszErrorMessage, ARRAYSIZE(wszErrorMessage), L"Command %d exited with code %u.", nCurCommand,
                nExitCode);
            goto L_error;
        }

        goto L_close;

    L_error:
        switch (MessageBoxW(NULL, wszErrorMessage, L"Error processing OEM commands", MB_ICONERROR | MB_ABORTRETRYIGNORE))
        {
            case IDABORT:
                SetupCloseInfFile(hCommandsInf);
                return;

            case IDRETRY:
                goto L_retry;

            case IDIGNORE:
            default:
                break;
        }

    L_close:
        if (infoProcess.hThread)
        {
            CloseHandle(infoProcess.hThread);
            infoProcess.hThread = NULL;
        }

        if (infoProcess.hProcess)
        {
            CloseHandle(infoProcess.hProcess);
            infoProcess.hProcess = NULL;
        }
    }

    // Done
    SetupCloseInfFile(hCommandsInf);
}

static void
GetErrorMessage(DWORD nErrorCode, OUT LPWSTR pwzBuffer, SIZE_T nBufferChars)
{
    DWORD nMessageChars;

    nMessageChars = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, nErrorCode, GetUserDefaultUILanguage(),
        pwzBuffer, nBufferChars, NULL);

    if (nMessageChars == 0)
        lstrcpynW(pwzBuffer, L"Unknown error", nBufferChars);
}
