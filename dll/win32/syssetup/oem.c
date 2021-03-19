/*
 * COPYRIGHT:         See COPYING in the top level directory
 * PROJECT:           ReactOS system libraries
 * PURPOSE:           System setup: execution of OEM commands
 * FILE:              dll/win32/syssetup/oem.c
 * PROGRAMER:         electroduck (root@electroduck.com)
 */

#include "precomp.h"

typedef BOOL (WINAPI* SHELLEXECPROC)(LPSHELLEXECUTEINFOW pExecInfo);
typedef void (WINAPI* SHCHANGENOTIFYPROC)(LONG nEventID, UINT flags, LPCVOID pItem1, LPCVOID pItem2);
typedef LPWSTR* (WINAPI* CMDLINE2ARGVPROC)(IN LPCWSTR pcwzCommandLine, OUT int* pnArgc);

static void
GetErrorMessage(DWORD nErrorCode, OUT LPWSTR pwzBuffer, SIZE_T nBufferChars);

static HANDLE
RunCommand(IN LPCWSTR pcwzCommand, IN LPCWSTR pcwzWorkingDirectory);

static const WCHAR CommandsSection[] = L"COMMANDS";
static WCHAR OEMFolderPath[MAX_PATH];

BOOL
FindOEMFolder(void)
{
    WCHAR wszCurrentAttempt[MAX_PATH];
    WCHAR wszBestResult[MAX_PATH];
    DWORD dwAttributes;
    WCHAR wcDriveLetter;
    UINT nDriveType;
    WCHAR wszDriveRoot[4];

    ZeroMemory(wszCurrentAttempt, sizeof(wszCurrentAttempt));
    ZeroMemory(wszBestResult, sizeof(wszBestResult));
    ZeroMemory(wszDriveRoot, sizeof(wszDriveRoot));

    // Try all lettered drives
    for (wcDriveLetter = 'A'; wcDriveLetter <= 'Z'; wcDriveLetter++)
    {
        _snwprintf(wszCurrentAttempt, ARRAYSIZE(wszCurrentAttempt) - 1, L"%wc:\\$OEM$\\", wcDriveLetter);

        // See if the OEM folder exists on this drive
        dwAttributes = GetFileAttributesW(wszCurrentAttempt);
        if ((dwAttributes != INVALID_FILE_ATTRIBUTES) && (dwAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            // It does
            wszDriveRoot[0] = wcDriveLetter;
            wszDriveRoot[1] = L':';
            wszDriveRoot[2] = L'\\';
            wszDriveRoot[3] = L'\0';

            // Check drive type
            nDriveType = GetDriveTypeW(wszDriveRoot);
            switch (nDriveType) {
                case DRIVE_REMOVABLE:
                case DRIVE_CDROM:
                    // USB or CD-ROM drive - this must be it, we can stop now
                    wcsncpy(OEMFolderPath, wszCurrentAttempt, ARRAYSIZE(OEMFolderPath));
                    return TRUE;

                default:
                    // Other drive type - may be it, but keep looking to see if we can find a removable one instead
                    wcsncpy(wszBestResult, wszCurrentAttempt, ARRAYSIZE(wszBestResult));
                    break;
            }
        }
    }

    // It wasn't found on a USB or CD-ROM, did we find it somewhere else?
    if (wszBestResult[0] != '\0') {
        // Yes we did
        wcsncpy(OEMFolderPath, wszBestResult, ARRAYSIZE(OEMFolderPath));
        return TRUE;
    }

    // No we didn't - not found
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
    HANDLE hCommandProcess;

    hCommandProcess = NULL;
    ZeroMemory(wszCommandsFile, sizeof(wszCommandsFile));
    ZeroMemory(wszSysError, sizeof(wszSysError));
    ZeroMemory(wszErrorMessage, sizeof(wszErrorMessage));
    ZeroMemory(wszCommandLine, sizeof(wszCommandLine));

    wcsncpy(wszCommandsFile, OEMFolderPath, ARRAYSIZE(wszCommandsFile) - 1);
    wcsncat(wszCommandsFile, L"CMDLINES.TXT", ARRAYSIZE(wszCommandsFile) - wcslen(wszCommandsFile) - 1);

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
            _snwprintf(wszErrorMessage, ARRAYSIZE(wszErrorMessage) - 1, L"Error 0x%08X on line %u of CMDLINES.TXT: %ws", nErrorCode, nErrorLine, wszSysError);

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
        // Get line context
        if (!SetupGetLineByIndexW(hCommandsInf, CommandsSection, nCurCommand, &ctxCurCommand))
        {
            nErrorCode = GetLastError();
            GetErrorMessage(nErrorCode, wszSysError, ARRAYSIZE(wszSysError));
            _snwprintf(
                wszErrorMessage, ARRAYSIZE(wszErrorMessage) - 1, L"Error 0x%08X finding command line %d: %ls", nErrorCode,
                nCurCommand, wszSysError);
            goto L_error;
        }

        // Get line data
        if (!SetupGetLineTextW(&ctxCurCommand, NULL, NULL, NULL, wszCommandLine, ARRAYSIZE(wszCommandLine), NULL))
        {
            nErrorCode = GetLastError();
            GetErrorMessage(nErrorCode, wszSysError, ARRAYSIZE(wszSysError));
            _snwprintf(
                wszErrorMessage, ARRAYSIZE(wszErrorMessage) - 1, L"Error 0x%08X reading text of command line %d: %ls", nErrorCode,
                nCurCommand, wszSysError);
            goto L_error;
        }

        // Start command
        hCommandProcess = RunCommand(wszCommandLine, OEMFolderPath);
        switch ((UINT_PTR)hCommandProcess)
        {
            case (UINT_PTR)INVALID_HANDLE_VALUE: // Error
                nErrorCode = GetLastError();
                GetErrorMessage(nErrorCode, wszSysError, ARRAYSIZE(wszSysError));
                _snwprintf(
                    wszErrorMessage, ARRAYSIZE(wszErrorMessage) - 1, L"Error 0x%08X executing command %d: %ls",
                    nErrorCode, nCurCommand, wszSysError);
                goto L_error;

            case (UINT_PTR)NULL: // No process needed to be started
                continue; // Proceed to next command

            default: // A process was started
                break;
        }

        // Wait for exit
        if (WaitForSingleObject(hCommandProcess, INFINITE) != WAIT_OBJECT_0)
        {
            nErrorCode = GetLastError();
            GetErrorMessage(nErrorCode, wszSysError, ARRAYSIZE(wszSysError));
            _snwprintf(
                wszErrorMessage, ARRAYSIZE(wszErrorMessage) - 1, L"Error 0x%08X waiting for command %d to exit: %ls", nErrorCode,
                nCurCommand, wszSysError);
            goto L_error;
        }

        // Get exit code
        if (!GetExitCodeProcess(hCommandProcess, &nExitCode))
        {
            nErrorCode = GetLastError();
            GetErrorMessage(nErrorCode, wszSysError, ARRAYSIZE(wszSysError));
            _snwprintf(
                wszErrorMessage, ARRAYSIZE(wszErrorMessage) - 1, L"Error 0x%08X getting exit code for command %d: %ls",
                nErrorCode, nCurCommand, wszSysError);
            goto L_error;
        }

        // Check exit code
        if (nExitCode != 0)
        {
            _snwprintf(
                wszErrorMessage, ARRAYSIZE(wszErrorMessage) - 1, L"Command %d exited with code %u.", nCurCommand,
                nExitCode);
            goto L_error;
        }

        goto L_loopclose;

    L_error:
        switch (MessageBoxW(NULL, wszErrorMessage, L"Error processing OEM commands", MB_ICONERROR | MB_ABORTRETRYIGNORE))
        {
            case IDABORT:
                SetupCloseInfFile(hCommandsInf);
                goto L_finalclose;

            case IDRETRY:
                goto L_retry;

            case IDIGNORE:
            default:
                break;
        }

    L_loopclose:
        if ((hCommandProcess) && (hCommandProcess != INVALID_HANDLE_VALUE))
        {
            CloseHandle(hCommandProcess);
            hCommandProcess = NULL;
        }
    }

    // Done
L_finalclose:
    if ((hCommandProcess) && (hCommandProcess != INVALID_HANDLE_VALUE))
        CloseHandle(hCommandProcess);
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

/*
 * Returns one of:
 *  - A valid handle to a process, if one was started
 *  - NULL if no process needed to be started
 *  - INVALID_HANDLE_VALUE if an error occurred
 */
static HANDLE
RunCommand(IN LPCWSTR pcwzCommand, IN LPCWSTR pcwzWorkingDirectory)
{
    HMODULE hShell32;
    HANDLE hProcess;
    DWORD nErrCode;
    SHELLEXECPROC procShellExecuteExW;
    CMDLINE2ARGVPROC procCommandLineToArgvW;
    SHELLEXECUTEINFOW infoExecute;
    PWSTR* ppwzCommandArgv;
    int nCommandArgc;
    size_t nFilenameLength;

    nErrCode = ERROR_SUCCESS;
    hProcess = INVALID_HANDLE_VALUE;

    // Load shell32
    hShell32 = LoadLibraryA("shell32.dll");
    if (hShell32 == NULL)
    {
        nErrCode = GetLastError();
        goto L_exit;
    }

    // Find ShellExecuteExW
    procShellExecuteExW = (SHELLEXECPROC)GetProcAddress(hShell32, "ShellExecuteExW");
    if (procShellExecuteExW == NULL)
    {
        nErrCode = GetLastError();
        goto L_unloadshell32;
    }

    // Find CommandLineToArgvW
    procCommandLineToArgvW = (CMDLINE2ARGVPROC)GetProcAddress(hShell32, "CommandLineToArgvW");
    if (procCommandLineToArgvW == NULL)
    {
        nErrCode = GetLastError();
        goto L_unloadshell32;
    }

    // Split command string - we need to know the file name
    ppwzCommandArgv = procCommandLineToArgvW(pcwzCommand, &nCommandArgc);
    if (ppwzCommandArgv == NULL)
    {
        nErrCode = GetLastError();
        goto L_unloadshell32;
    }

    // Set up the ShellExecuteInfo struct
    ZeroMemory(&infoExecute, sizeof(infoExecute));
    infoExecute.cbSize = sizeof(infoExecute);
    infoExecute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_DOENVSUBST;
    infoExecute.lpFile = ppwzCommandArgv[0];
    infoExecute.lpParameters = pcwzCommand + wcslen(ppwzCommandArgv[0]) + 1;
    infoExecute.lpDirectory = pcwzWorkingDirectory;
    infoExecute.nShow = SW_SHOW;

    // If present, arguments will be located in the command string after the file name
    nFilenameLength = wcslen(ppwzCommandArgv[0]);
    if (pcwzCommand[nFilenameLength] != L'\0')
        infoExecute.lpParameters = pcwzCommand + nFilenameLength + 1;
    else
        infoExecute.lpParameters = L"";

    // Start the process
    if (!procShellExecuteExW(&infoExecute))
    {
        nErrCode = GetLastError();
        goto L_freeargv;
    }

    hProcess = infoExecute.hProcess;

L_freeargv:
    LocalFree((HLOCAL)ppwzCommandArgv);
L_unloadshell32:
    FreeLibrary(hShell32);
L_exit:
    SetLastError(nErrCode);
    return hProcess;
}
