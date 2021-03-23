/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     GPL - See COPYING in the top level directory
 * FILE:        dll\win32\iernonce\iernonce.c
 * PURPOSE:     ReactOS Extended RunOnce processing with UI
 * PROGRAMMERS: Copyright 2013-2016 Robert Naumann
 */


#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winuser.h>
#include <stdio.h>
#include <stdlib.h>

#define NDEBUG
#include <debug.h>

#include "resource.h"

#define IERUNONCE_KEY_NAME_MAX 255

typedef struct ROXDLGDATA
{
    PCWSTR pcwszTitle;
    PCWSTR *ppcwszKeyNames;
} ROXDLGDATA, *PROXDLGDATA;

typedef struct ROXSTEP
{
    WCHAR wszName[261];
    PWSTR pwszCommand;
} ROXSTEP, *PROXSTEP;

typedef const ROXSTEP* PCROXSTEP;

typedef struct ROXTARGET
{
    WCHAR wszKeyName[256];
    PWSTR pwszTitle;
    UINT nSteps;
    PROXSTEP pSteps;
} ROXTARGET, *PROXTARGET;

typedef const ROXTARGET* PCROXTARGET;

static int
CompareWideStringPointerPointers(const void* ppcwszA, const void* ppcwszB);

static int
CompareSteps(const void* pStepA, const void* pStepB);

static int
CompareTargets(const void* pTargetA, const void* pTargetB);

static INT_PTR
RunOnceExDialogProc(HWND hDialogWindow, UINT nMessage, WPARAM wParam, LPARAM lParam);

static const WCHAR DefaultErrorTitle[] = L"IERunOnce error";
static const WCHAR DefaultUITitle[] = L"Setup";

HINSTANCE hInstance;
HANDLE ProcessHeap;

BOOL
WINAPI
DllMain(HINSTANCE hinstDLL,
        DWORD dwReason,
        LPVOID reserved)
{
    switch (dwReason)
    {
        case DLL_PROCESS_ATTACH:
            hInstance = hinstDLL;
            ProcessHeap = GetProcessHeap();
            break;

        case DLL_PROCESS_DETACH:
            hInstance = hinstDLL;
            ProcessHeap = NULL;
            break;
    }

   return TRUE;
}

VOID WINAPI RunOnceExProcess(HWND hwnd, HINSTANCE hInst, LPCSTR path, int nShow)
{
    HKEY hRunOnceKey, hTargetKey;
    LSTATUS status;
    WCHAR wszErrorMessage[256];
    WCHAR wszUITitle[256];
    DWORD nLength, nNameLength, nValue;
    PROXTARGET pTargets;
    PROXTARGET pTargetsExpanded;
    PROXSTEP pStepsExpanded, pCurStep;
    UINT nTargetCount, nTarget, nStep;
    BOOL bMoreItems;
    //INT_PTR nDialogResult;

    ZeroMemory(wszErrorMessage, sizeof(wszErrorMessage));
    ZeroMemory(wszUITitle, sizeof(wszUITitle));
    pTargets = NULL;
    pTargetsExpanded = NULL;
    hRunOnceKey = NULL;
    hTargetKey = NULL;

    // Open RunOnceEx key
    status = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnceEx", 0, KEY_READ | KEY_WOW64_64KEY,
        &hRunOnceKey);
    switch (status)
    {
        case ERROR_SUCCESS:
            break;

        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return; // Nothing to do

        default:
            _snwprintf(wszErrorMessage, ARRAYSIZE(wszErrorMessage) - 1, L"Error 0x%08X opening RunOnceEx key", status);
            MessageBoxW(hwnd, wszErrorMessage, DefaultErrorTitle, MB_ICONERROR);
            return;
    }

    // Read title
    nLength = sizeof(wszUITitle) - sizeof(WCHAR);
    status = RegQueryValueExW(hRunOnceKey, L"TITLE", NULL, NULL, (LPBYTE)wszUITitle, &nLength);
    switch (status)
    {
        case ERROR_SUCCESS:
            wszUITitle[nLength / sizeof(WCHAR)] = L'\0';
            break;

        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            // Title not specified, use default
            if (!LoadStringW(hInstance, IDS_SETUP, wszUITitle, ARRAYSIZE(wszUITitle)))
                lstrcpynW(wszUITitle, DefaultUITitle, ARRAYSIZE(wszUITitle));
            break;

        default:
            _snwprintf(
                wszErrorMessage, ARRAYSIZE(wszErrorMessage) - 1, L"Error 0x%08X reading RunOnceEx title", status);
            MessageBoxW(hwnd, wszErrorMessage, DefaultErrorTitle, MB_ICONERROR);
            goto L_closeROXkey;
    }

    // Get targets
    nTargetCount = 0;
    for (bMoreItems = TRUE; bMoreItems;)
    {
        // Add space to target array
        pTargetsExpanded = pTargets ? HeapReAlloc(ProcessHeap, HEAP_ZERO_MEMORY, pTargets, (nTargetCount + 1) * sizeof(ROXTARGET))
            : HeapAlloc(ProcessHeap, HEAP_ZERO_MEMORY, (nTargetCount + 1) * sizeof(ROXTARGET));
        if (!pTargetsExpanded)
        {
            MessageBoxW(hwnd, L"Error expanding subkeys array", wszUITitle, MB_ICONERROR);
            goto L_freetargets;
        }
        pTargets = pTargetsExpanded;

        // Read name
        nLength = ARRAYSIZE(pTargets[nTargetCount].wszKeyName);
        status = RegEnumKeyExW(hRunOnceKey, nTargetCount, pTargets[nTargetCount].wszKeyName, &nLength, NULL, NULL, NULL, NULL);
        switch (status)
        {
            case ERROR_SUCCESS:
                nTargetCount++;
                break;

            case ERROR_NO_MORE_ITEMS:
                bMoreItems = FALSE;
                break;

            default:
                _snwprintf(
                    wszErrorMessage, ARRAYSIZE(wszErrorMessage) - 1, L"Error 0x%08X querying name of subkey",
                    status);
                MessageBoxW(hwnd, wszErrorMessage, wszUITitle, MB_ICONERROR);
                goto L_freetargets;
        }
    }

    // Sort targets by key name
    qsort(pTargets, nTargetCount, sizeof(ROXTARGET), CompareTargets);

    // Read steps for each target
    for (nTarget = 0; nTarget < nTargetCount; nTarget++)
    {
        // Open target key
        status = RegOpenKeyExW(hRunOnceKey, pTargets[nTarget].wszKeyName, 0, KEY_READ, &hTargetKey);
        if (status != ERROR_SUCCESS)
        {
            _snwprintf(
                wszErrorMessage, ARRAYSIZE(wszErrorMessage) - 1, L"Error 0x%08X opening target subkey", status);
            MessageBoxW(hwnd, wszErrorMessage, wszUITitle, MB_ICONERROR);
            goto L_freetargets;
        }

        // Get target title length
        nLength = 0;
        status = RegQueryValueExW(hTargetKey, NULL, NULL, NULL, NULL, &nLength);
        switch (status)
        {
            case ERROR_SUCCESS:
                break;

            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
                // No title specified, use key name
                pTargets[nTarget].pwszTitle = pTargets[nTarget].wszKeyName;
                goto L_skiptitle;

            default:
                _snwprintf(
                    wszErrorMessage, ARRAYSIZE(wszErrorMessage) - 1, L"Error 0x%08X querying target title length",
                    status);
                MessageBoxW(hwnd, wszErrorMessage, wszUITitle, MB_ICONERROR);
                goto L_closetargetkey;
        }

        // Allocate space for target title
        pTargets[nTarget].pwszTitle = HeapAlloc(ProcessHeap, HEAP_ZERO_MEMORY, nLength + sizeof(WCHAR));
        if (!pTargets[nTarget].pwszTitle)
        {
            MessageBoxW(hwnd, L"Error allocating memory for target title", wszUITitle, MB_ICONERROR);
            goto L_closetargetkey;
        }

        // Read target title
        status = RegQueryValueExW(hTargetKey, NULL, NULL, NULL, (LPBYTE)pTargets[nTarget].pwszTitle, &nLength);
        if (status != ERROR_SUCCESS)
        {
            _snwprintf(
                wszErrorMessage, ARRAYSIZE(wszErrorMessage) - 1, L"Error 0x%08X reading target title", status);
            MessageBoxW(hwnd, wszErrorMessage, wszUITitle, MB_ICONERROR);
            goto L_closetargetkey;
        }
        pTargets[nTarget].pwszTitle[nLength / sizeof(WCHAR)] = L'\0';

    L_skiptitle:

        // Enumerate steps
        for (bMoreItems = TRUE, nValue = 0; bMoreItems; nValue++)
        {
            // Allocate space for step
            pStepsExpanded = pTargets[nTarget].pSteps
                ? HeapReAlloc(ProcessHeap, HEAP_ZERO_MEMORY, pTargets[nTarget].pSteps, (pTargets[nTarget].nSteps + 1) * sizeof(ROXSTEP))
                : HeapAlloc(ProcessHeap, HEAP_ZERO_MEMORY, (pTargets[nTarget].nSteps + 1) * sizeof(ROXSTEP));
            if (!pStepsExpanded)
            {
                MessageBoxW(hwnd, L"Error expanding steps array", wszUITitle, MB_ICONERROR);
                goto L_closetargetkey;
            }
            pTargets[nTarget].pSteps = pStepsExpanded;
            pCurStep = &pTargets[nTarget].pSteps[pTargets[nTarget].nSteps];

            // Get value name and command length (in bytes)
            nLength = 0, nNameLength = ARRAYSIZE(pCurStep->wszName);
            status = RegEnumValueW(hTargetKey, nValue, pCurStep->wszName, &nNameLength, NULL, NULL, NULL, &nLength);
            switch (status)
            {
                case ERROR_SUCCESS:
                    if (nNameLength == 0)
                        continue; // Skip default value (step name)
                    break;

                case ERROR_NO_MORE_ITEMS:
                    bMoreItems = FALSE;
                    continue;

                default:
                    _snwprintf(
                        wszErrorMessage, ARRAYSIZE(wszErrorMessage) - 1, L"Error 0x%08X querying step name and command size",
                        status);
                    MessageBoxW(hwnd, wszErrorMessage, wszUITitle, MB_ICONERROR);
                    goto L_closetargetkey;
            }

            // Allocate space for command
            pCurStep->pwszCommand = HeapAlloc(ProcessHeap, HEAP_ZERO_MEMORY, nLength + sizeof(WCHAR));
            if (!pCurStep->pwszCommand)
            {
                MessageBoxW(hwnd, L"Error allocating memory for step command", wszUITitle, MB_ICONERROR);
                goto L_closetargetkey;
            }

            // Read command
            status = RegQueryValueExW(hTargetKey, pCurStep->wszName, NULL, NULL, (LPBYTE)pCurStep->pwszCommand, &nLength);
            if (status != ERROR_SUCCESS)
            {
                _snwprintf(
                    wszErrorMessage, ARRAYSIZE(wszErrorMessage) - 1, L"Error 0x%08X reading step command", status);
                MessageBoxW(hwnd, wszErrorMessage, wszUITitle, MB_ICONERROR);
                goto L_closetargetkey;
            }

            pTargets[nTarget].nSteps++;
        } // END step enumeration

        // Sort steps by value name
        qsort(pTargets[nTarget].pSteps, pTargets[nTarget].nSteps, sizeof(ROXSTEP), CompareSteps);

        // Close target key
        RegCloseKey(hTargetKey);
        hTargetKey = NULL;
    }

    // Print targets and steps
    #if 1
    for (nTarget = 0; nTarget < nTargetCount; nTarget++)
    {
        OutputDebugStringW(L"Begin target ");
        OutputDebugStringW(pTargets[nTarget].wszKeyName);
        OutputDebugStringW(L": ");
        OutputDebugStringW(pTargets[nTarget].pwszTitle);
        OutputDebugStringW(L"\r\n");

        for (nStep = 0; nStep < pTargets[nTarget].nSteps; nStep++)
        {
            OutputDebugStringW(L"Step ");
            OutputDebugStringW(pTargets[nTarget].pSteps[nStep].wszName);
            OutputDebugStringW(L": ");
            OutputDebugStringW(pTargets[nTarget].pSteps[nStep].pwszCommand);
            OutputDebugStringW(L"\r\n");
        }

        OutputDebugStringW(L"End target ");
        OutputDebugStringW(pTargets[nTarget].wszKeyName);
        OutputDebugStringW(L": ");
        OutputDebugStringW(pTargets[nTarget].pwszTitle);
        OutputDebugStringW(L"\r\n\r\n");
    }
    #endif

L_closetargetkey:
    if (hTargetKey)
        RegCloseKey(hTargetKey);
    hTargetKey = NULL;

L_freetargets:
    for (nTarget = 0; nTarget < nTargetCount; nTarget++)
    {
        for (nStep = 0; nStep < pTargets[nTarget].nSteps; nStep++)
        {
            if (pTargets[nTarget].pSteps[nStep].pwszCommand)
                HeapFree(ProcessHeap, 0, pTargets[nTarget].pSteps[nStep].pwszCommand);
            pTargets[nTarget].pSteps[nStep].pwszCommand = NULL;
        }

        if (pTargets[nTarget].pSteps)
            HeapFree(ProcessHeap, 0, pTargets[nTarget].pSteps);
        pTargets[nTarget].pSteps = NULL;

        if (pTargets[nTarget].pwszTitle)
            HeapFree(ProcessHeap, 0, pTargets[nTarget].pwszTitle);
        pTargets[nTarget].pwszTitle = NULL;
    }

    if (pTargets)
        HeapFree(ProcessHeap, 0, pTargets);
    pTargets = NULL;

L_closeROXkey:
    if (hRunOnceKey)
        RegCloseKey(hRunOnceKey);
    hRunOnceKey = NULL;
}

static int
CompareWideStringPointerPointers(const void *ppcwszA, const void *ppcwszB)
{
    PCWSTR pcwzStringA = *(PCWSTR*)ppcwszA;
    PCWSTR pcwzStringB = *(PCWSTR*)ppcwszB;

    return lstrcmpiW(pcwzStringA, pcwzStringB);
}

static INT_PTR
RunOnceExDialogProc(HWND hDialogWindow, UINT nMessage, WPARAM wParam, LPARAM lParam)
{
    switch (nMessage)
    {
        case WM_INITDIALOG:
            SetWindowLongPtrW(hDialogWindow, GWLP_USERDATA, (LONG)lParam);
            return (INT_PTR)TRUE;

    }

    return (INT_PTR)FALSE;
}

static int
CompareSteps(const void* pStepA, const void* pStepB)
{
    PCROXSTEP pStepACasted = pStepA;
    PCROXSTEP pStepBCasted = pStepB;

    return lstrcmpiW(pStepACasted->wszName, pStepBCasted->wszName);
}

static int
CompareTargets(const void* pTargetA, const void* pTargetB)
{
    PCROXTARGET pTargetACasted = pTargetA;
    PCROXTARGET pTargetBCasted = pTargetB;

    return lstrcmpiW(pTargetACasted->wszKeyName, pTargetBCasted->wszKeyName);
}
