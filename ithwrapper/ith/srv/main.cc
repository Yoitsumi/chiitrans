// main.cc
// 8/24/2013 jichi
// Branch IHF/main.cpp, rev 111
// 8/24/2013 TODO: Clean up this file

//#ifdef _MSC_VER
//# pragma warning(disable:4800) // C4800: forcing value to bool (performance warning)
//#endif // _MSC_VER

#include "config.h"
//#include "customfilter.h"
#include "srv.h"
#include "srv_p.h"
#include "settings.h"
#include "ith/common/const.h"
#include "ith/common/defs.h"
#include "ith/common/growl.h"
#include "ith/common/types.h"
#include "ith/sys/sys.h"
#include "cc/ccmacro.h"
#include <commctrl.h>

//#define ITH_WINE
#define ITH_USE_XP_DLLS  (IthIsWindowsXp() && !IthIsWine())
//#define ITH_USE_XP_DLLS  IthIsWindowsXp()

namespace { // unnamed

enum { HOOK_TIMEOUT = -50000000 }; // in nanoseconds = 5 seconds

CRITICAL_SECTION cs;
//WCHAR exist[] = ITH_PIPEEXISTS_EVENT;
//WCHAR mutex[] = L"ITH_RUNNING";
//WCHAR EngineName[] = ITH_ENGINE_DLL;
//WCHAR EngineNameXp[] = ITH_ENGINE_XP_DLL;
//WCHAR DllName[] = ITH_CLIENT_DLL;
//WCHAR DllNameXp[] = ITH_CLIENT_XP_DLL;
HANDLE hServerMutex; // jichi 9/28/2013: used to guard pipe
HANDLE hHookMutex;  // jichi 9/28/2013: used to guard hook modification
DWORD admin;
} // unnamed namespace

//extern LPWSTR current_dir;
extern CRITICAL_SECTION detach_cs;

Settings *settings;
HWND hMainWnd;
HANDLE hPipeExist;
BOOL running;

#define ITH_SYNC_HOOK   IthMutexLocker locker(::hHookMutex);

namespace { // unnamed

void GetDebugPriv()
{
  HANDLE  hToken;
  DWORD  dwRet;
  NTSTATUS status;

  TOKEN_PRIVILEGES Privileges = {1,{0x14,0,SE_PRIVILEGE_ENABLED}};

  NtOpenProcessToken(NtCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken);

  status = NtAdjustPrivilegesToken(hToken, 0, &Privileges, sizeof(Privileges), 0, &dwRet);
  admin = 1; // jichi 8/24/2013: ITH do not need admin
  //if (STATUS_SUCCESS == status)
  //{
  //  admin = 1;
  //}
  //else
  //{
  //  WCHAR buffer[0x10];
  //  swprintf(buffer, L"%.8X",status);
  //  MessageBox(0, NotAdmin, buffer, 0);
  //}
  NtClose(hToken);
}

// jichi 9/22/2013: Change current directory to the same as main module path
// Otherwise NtFile* would not work for files with relative paths.
//BOOL ChangeCurrentDirectory()
//{
//  if (const wchar_t *path = GetMainModulePath()) // path to VNR's python exe
//    if (const wchar_t *base = wcsrchr(path, L'\\')) {
//      size_t len = base - path;
//      if (len < MAX_PATH) {
//        wchar_t buf[MAX_PATH];
//        wcsncpy(buf, path, len);
//        buf[len] = 0;
//        return SetCurrentDirectoryW(buf);
//      }
//    }
//  return FALSE;
//}

DWORD Inject(HANDLE hProc, LPCWSTR engine)
{
  enum : DWORD { error = (DWORD)-1 };
  LPVOID lpvAllocAddr = 0;
  DWORD dwWrite = 0x1000; //, len = 0;
  HANDLE hTH;
  //LPWSTR dllname = (IthIsWindowsXp() && !IthIsWine()) ? DllNameXp : DllName;
  LPCWSTR dllname = ITH_USE_XP_DLLS ? ITH_CLIENT_XP_DLL : ITH_CLIENT_DLL;
  //if (!IthCheckFile(dllname))
  //  return error;
  wchar_t path[MAX_PATH];
  size_t len = IthGetCurrentModulePath(path, MAX_PATH);
  if (!len)
    return error;

  wchar_t *p;
  for (p = path + len; *p != L'\\'; p--);
  p++; // ending with L"\\"

  //LPCWSTR mp = GetMainModulePath();
  //len = wcslen(mp);
  //memcpy(path, mp, len << 1);
  //memset(path + len, 0, (MAX_PATH - len) << 1);
  //LPWSTR p;
  //for (p = path + len; *p != L'\\'; p--); // Always a \ after drive letter.
  //p++;
  wcscpy(p, dllname);
  //if (IthIsWine())
  //  lpvAllocAddr = VirtualAllocEx(hProc, nullptr, dwWrite, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
  //else
  NtAllocateVirtualMemory(hProc, &lpvAllocAddr, 0, &dwWrite, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
  if (!lpvAllocAddr)
    return error;

  CheckThreadStart();

  //Copy module path into address space of target process.
  //if (IthIsWine())
  //  WriteProcessMemory(hProc, lpvAllocAddr, path, MAX_PATH << 1, &dwWrite);
  //else
  NtWriteVirtualMemory(hProc, lpvAllocAddr, path, MAX_PATH << 1, &dwWrite);
  hTH = IthCreateThread(LoadLibraryW, (DWORD)lpvAllocAddr, hProc);
  if (hTH == 0 || hTH == INVALID_HANDLE_VALUE)
    //ConsoleOutput(ErrorRemoteThread);
    return -1;
  // jichi 9/28/2013: no wait as it will not blocked
  NtWaitForSingleObject(hTH, 0, 0);
  THREAD_BASIC_INFORMATION info;
  NtQueryInformationThread(hTH, ThreadBasicInformation, &info, sizeof(info), &dwWrite);
  NtClose(hTH);
  if (info.ExitStatus) {
    //IthCoolDown();
    wcscpy(p, engine);
    //if (IthIsWine())
    //  WriteProcessMemory(hProc, lpvAllocAddr, path, MAX_PATH << 1, &dwWrite);
    //else
    NtWriteVirtualMemory(hProc, lpvAllocAddr, path, MAX_PATH << 1, &dwWrite);
    hTH = IthCreateThread(LoadLibraryW, (DWORD)lpvAllocAddr, hProc);
    if (hTH == 0 || hTH == INVALID_HANDLE_VALUE)
      //ConsoleOutput(ErrorRemoteThread);
      return error;

    // jichi 9/28/2013: no wait as it will not blocked
    NtWaitForSingleObject(hTH, 0, 0);
    NtClose(hTH);
  }

  dwWrite = 0;
  //if (IthIsWine())
  //  VirtualFreeEx(hProc, lpvAllocAddr, dwWrite, MEM_RELEASE);
  //else
  NtFreeVirtualMemory(hProc, &lpvAllocAddr, &dwWrite, MEM_RELEASE);
  return info.ExitStatus;
}


} // unnamed namespace

void CreateNewPipe();

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
  CC_UNUSED(lpvReserved);
  switch (fdwReason)
  {
  case DLL_PROCESS_ATTACH:
    LdrDisableThreadCalloutsForDll(hinstDLL);
    InitializeCriticalSection(&cs);
    IthInitSystemService();
    GetDebugPriv();
    //Used by timers.
    InitCommonControls();
    // jichi 8/24/2013: Create hidden window so that ITH can access timer and events
    // TODO: Find the game window HWND for timer instead of creating one in ITH
    hMainWnd = CreateWindow(L"Button", L"InternalWindow", 0, 0, 0, 0, 0, 0, 0, hinstDLL, 0);

    //ChangeCurrentDirectory();
    break;
  case DLL_PROCESS_DETACH:
    if (running) IHF_Cleanup();
    DeleteCriticalSection(&cs);
    IthCloseSystemService();
    DestroyWindow(hMainWnd);
    break;
  default:
    break;
  }
  return true;
}

HANDLE IthOpenPipe(LPWSTR name, ACCESS_MASK direction)
{
  UNICODE_STRING us;
  RtlInitUnicodeString(&us,name);
  SECURITY_DESCRIPTOR sd={1};
  OBJECT_ATTRIBUTES oa={sizeof(oa),0,&us,OBJ_CASE_INSENSITIVE,&sd,0};
  HANDLE hFile;
  IO_STATUS_BLOCK isb;
  if (NT_SUCCESS(NtCreateFile(&hFile,direction,&oa,&isb,0,0,FILE_SHARE_READ,FILE_OPEN,0,0,0)))
    return hFile;
  else return INVALID_HANDLE_VALUE;
}

//void ConsoleOutput(LPCWSTR text)
//{
//  // jichi 8/24/2013: ConsoleOutput is ignored
//  CC_UNUSED(text);
//  //man->AddConsoleOutput(text);
//}

#define IHS_SIZE 0x80
#define IHS_BUFF_SIZE (IHS_SIZE - sizeof(HookParam))

struct InsertHookStruct
{
  SendParam sp;
  BYTE name_buffer[IHS_SIZE];
};

IHFSERVICE DWORD IHFAPI IHF_Init()
{
  BOOL result = false;
  DWORD present;
  EnterCriticalSection(&cs);
  hServerMutex = IthCreateMutex(ITH_SERVER_MUTEX, 1, &present);
  if (present)
    //MessageBox(0,L"Already running.",0,0);
    // jichi 8/24/2013
    ITH_WARN(L"I am sorry that this game is attached by some other VNR ><\nPlease restart the game and try again!");
  else if (!running) {
    running = true;
    settings = new Settings;
    man = new HookManager;
    //cmdq = new CommandQueue;
    InitializeCriticalSection(&detach_cs);

    ::hHookMutex = IthCreateMutex(ITH_SERVER_HOOK_MUTEX, FALSE);
    result = true;

  }
  LeaveCriticalSection(&cs);
  return result;
}

IHFSERVICE DWORD IHFAPI IHF_Start()
{
  //IthBreak();
  CreateNewPipe();
  hPipeExist = IthCreateEvent(ITH_PIPEEXISTS_EVENT);
  NtSetEvent(hPipeExist, nullptr);
  return 0;
}

IHFSERVICE DWORD IHFAPI IHF_Cleanup()
{
  BOOL result = FALSE;
  EnterCriticalSection(&cs);
  if (running) {
    running = FALSE;
    HANDLE hRecvPipe = IthOpenPipe(recv_pipe, GENERIC_WRITE);
    NtClose(hRecvPipe);
    NtClearEvent(hPipeExist);
    //delete cmdq;
    delete man;
    delete settings;
    NtClose(::hHookMutex);
    NtClose(hServerMutex);
    NtClose(hPipeExist);
    DeleteCriticalSection(&detach_cs);
    result = TRUE;
  }
  LeaveCriticalSection(&cs);
  return result;
}

IHFSERVICE DWORD IHFAPI IHF_GetPIDByName(LPCWSTR pwcTarget)
{
  DWORD dwSize = 0x20000,
        dwExpectSize = 0;
  LPVOID pBuffer = 0;
  SYSTEM_PROCESS_INFORMATION *spiProcessInfo;
  DWORD dwPid = 0;
  DWORD dwStatus;

  NtAllocateVirtualMemory(NtCurrentProcess(), &pBuffer, 0, &dwSize, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
  dwStatus = NtQuerySystemInformation(SystemProcessInformation, pBuffer, dwSize, &dwExpectSize);
  if (!NT_SUCCESS(dwStatus)) {
    NtFreeVirtualMemory(NtCurrentProcess(),&pBuffer,&dwSize,MEM_RELEASE);
    if (dwStatus != STATUS_INFO_LENGTH_MISMATCH || dwExpectSize < dwSize)
      return 0;
    dwSize = (dwExpectSize | 0xFFF) + 0x4001; //
    pBuffer = 0;
    NtAllocateVirtualMemory(NtCurrentProcess(), &pBuffer, 0, &dwSize, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    dwStatus = NtQuerySystemInformation(SystemProcessInformation, pBuffer, dwSize, &dwExpectSize);
    if (!NT_SUCCESS(dwStatus)) goto _end;
  }

  for (spiProcessInfo = (SYSTEM_PROCESS_INFORMATION *)pBuffer; spiProcessInfo->dNext;) {
    spiProcessInfo = (SYSTEM_PROCESS_INFORMATION *)
      ((DWORD)spiProcessInfo + spiProcessInfo -> dNext);
    if (_wcsicmp(pwcTarget, spiProcessInfo -> usName.Buffer) == 0) {
      dwPid = spiProcessInfo->dUniqueProcessId;
      break;
    }
  }
  //if (dwPid == 0) ConsoleOutput(ErrorNoProcess);
_end:
  NtFreeVirtualMemory(NtCurrentProcess(),&pBuffer,&dwSize,MEM_RELEASE);
  return dwPid;
}

IHFSERVICE DWORD IHFAPI IHF_InjectByPID(DWORD pid, LPCWSTR engine)
{
  WCHAR str[0x80];
  if (!running)
    return 0;
  if (pid == current_process_id)
    //ConsoleOutput(SelfAttach);
    return -1;
  if (man->GetProcessRecord(pid))
    //ConsoleOutput(AlreadyAttach);
    return -1;
  swprintf(str, ITH_HOOKMAN_MUTEX_ L"%d", pid);
  DWORD s;
  NtClose(IthCreateMutex(str, 0, &s));
  if (s)
    return -1;
  CLIENT_ID id;
  OBJECT_ATTRIBUTES oa = {};
  HANDLE hProc;
  id.UniqueProcess = pid;
  id.UniqueThread = 0;
  oa.uLength = sizeof(oa);
  if (!NT_SUCCESS(NtOpenProcess(&hProc,
      PROCESS_QUERY_INFORMATION|
      PROCESS_CREATE_THREAD|
      PROCESS_VM_OPERATION|
      PROCESS_VM_READ|
      PROCESS_VM_WRITE,
      &oa, &id)))
    //ConsoleOutput(ErrorOpenProcess);
    return -1;

  if (!engine)
    engine = ITH_USE_XP_DLLS ? ITH_ENGINE_XP_DLL : ITH_ENGINE_DLL;
  DWORD module = Inject(hProc, engine);
  NtClose(hProc);
  if (module == -1)
    return -1;
  //swprintf(str, FormatInject, pid, module);
  //ConsoleOutput(str);
  return module;
}

IHFSERVICE DWORD IHFAPI IHF_ActiveDetachProcess(DWORD pid)
{
  ITH_SYNC_HOOK

  DWORD module, engine, dwWrite;
  HANDLE hProc, hThread, hCmd;
  IO_STATUS_BLOCK ios;
  //man->LockHookman();
  ProcessRecord* pr = man->GetProcessRecord(pid);
  hCmd = man->GetCmdHandleByPID(pid);
  if (pr == 0 || hCmd == 0) return FALSE;
  //hProc = pr->process_handle; //This handle may be closed(thus invalid) during the detach process.
  NtDuplicateObject(NtCurrentProcess(), pr->process_handle,
      NtCurrentProcess(), &hProc, 0, 0, DUPLICATE_SAME_ACCESS); //Make a copy of the process handle.
  module = pr->module_register;
  if (module == 0)
    return FALSE;
  engine = pr->engine_register;
  engine &= ~0xFF;
  SendParam sp = {};
  sp.type = 4;

  NtWriteFile(hCmd, 0,0,0, &ios, &sp, sizeof(SendParam),0,0);

  //cmdq->AddRequest(sp, pid);
  dwWrite = 0x1000;
//#ifdef ITH_WINE // Nt series crash on wine
//  hThread = IthCreateThread(FreeLibrary, engine, hProc);
//#else
  hThread = IthCreateThread(LdrUnloadDll, engine, hProc);
//#endif // ITH_WINE
  if (hThread == 0 ||
    hThread == INVALID_HANDLE_VALUE) return FALSE;
  const LONGLONG timeout = HOOK_TIMEOUT;
  NtWaitForSingleObject(hThread, 0, (PLARGE_INTEGER)&timeout);
  NtClose(hThread);
  //IthCoolDown();
//#ifdef ITH_WINE // Nt series crash on wine
//  hThread = IthCreateThread(FreeLibrary, engine, hProc);
//#else
  hThread = IthCreateThread(LdrUnloadDll, module, hProc);
//#endif // ITH_WINE
  if (hThread == 0 ||
    hThread == INVALID_HANDLE_VALUE)
    return FALSE;
  NtWaitForSingleObject(hThread, 0, (PLARGE_INTEGER)&timeout);
  //man->UnlockHookman();
  THREAD_BASIC_INFORMATION info;
  NtQueryInformationThread(hThread, ThreadBasicInformation, &info, sizeof(info), 0);
  NtClose(hThread);
  NtSetEvent(hPipeExist, 0);
  FreeThreadStart(hProc);
  NtClose(hProc);
  dwWrite = 0x1000;
  return info.ExitStatus;
}

IHFSERVICE DWORD IHFAPI IHF_GetHookManager(HookManager** hookman)
{
  if (running) {
    *hookman = man;
    return 0;
  }
  else return 1;
}

IHFSERVICE DWORD IHFAPI IHF_GetSettings(Settings **p)
{
  if (running) {
    *p = settings;
    return 0;
  }
  else return 1;
}

IHFSERVICE DWORD IHFAPI IHF_InsertHook(DWORD pid, HookParam* hp, LPWSTR name)
{
  ITH_SYNC_HOOK

  HANDLE hCmd = man->GetCmdHandleByPID(pid);
  if (hCmd == 0)
    return -1;

  InsertHookStruct s;
  s.sp.type = IHF_COMMAND_NEW_HOOK;
  s.sp.hp = *hp;
  DWORD len;
  if (name) len = wcslen(name) << 1;
  else len = 0;
  if (len >= IHS_BUFF_SIZE - 2) len = IHS_BUFF_SIZE - 2;
  memcpy(s.name_buffer, name, len);
  s.name_buffer[len] = 0;
  s.name_buffer[len + 1] = 0;
  IO_STATUS_BLOCK ios;
  NtWriteFile(hCmd, 0,0,0, &ios, &s, IHS_SIZE, 0, 0);

  //memcpy(&sp.hp,hp,sizeof(HookParam));
  //cmdq->AddRequest(sp, pid);
  return 0;
}

IHFSERVICE DWORD IHFAPI IHF_ModifyHook(DWORD pid, HookParam* hp)
{
  ITH_SYNC_HOOK

  SendParam sp;
  HANDLE hModify,hCmd;
  hCmd = GetCmdHandleByPID(pid);
  if (hCmd == 0) return -1;
  hModify = IthCreateEvent(ITH_MODIFYHOOK_EVENT);
  sp.type = IHF_COMMAND_MODIFY_HOOK;
  sp.hp = *hp;
  IO_STATUS_BLOCK ios;
  if (NT_SUCCESS(NtWriteFile(hCmd, 0,0,0, &ios, &sp, sizeof(SendParam), 0, 0)))
    // jichi 9/28/2013: no wait timeout
    //const LONGLONG timeout = HOOK_TIMEOUT;
    NtWaitForSingleObject(hModify, 0, 0);
  NtClose(hModify);
  man -> RemoveSingleHook(pid, sp.hp.addr);
  return 0;
}

IHFSERVICE DWORD IHFAPI IHF_RemoveHook(DWORD pid, DWORD addr)
{
  ITH_SYNC_HOOK

  HANDLE hRemoved,hCmd;
  hCmd = GetCmdHandleByPID(pid);
  if (hCmd == 0) return -1;
  hRemoved = IthCreateEvent(ITH_REMOVEHOOK_EVENT);
  SendParam sp = {};
  IO_STATUS_BLOCK ios;
  sp.type = IHF_COMMAND_REMOVE_HOOK;
  sp.hp.addr = addr;
  //cmdq -> AddRequest(sp, pid);
  NtWriteFile(hCmd, 0,0,0, &ios, &sp, sizeof(SendParam),0,0);
  const LONGLONG timeout = HOOK_TIMEOUT;
  NtWaitForSingleObject(hRemoved, 0, (PLARGE_INTEGER)&timeout);
  NtClose(hRemoved);
  man -> RemoveSingleHook(pid, sp.hp.addr);
  return 0;
}

IHFSERVICE DWORD IHFAPI IHF_IsAdmin()
{
  return admin;
}

IHFSERVICE DWORD IHFAPI IHF_AddLink(DWORD from, DWORD to)
{
  man->AddLink(from & 0xFFFF, to & 0xFFFF);
  return 0;
}

IHFSERVICE DWORD IHFAPI IHF_UnLink(DWORD from)
{
  man->UnLink(from & 0xFFFF);
  return 0;
}

IHFSERVICE DWORD IHFAPI IHF_UnLinkAll(DWORD from)
{
  man->UnLinkAll(from & 0xFFFF);
  return 0;
}

// EOF
