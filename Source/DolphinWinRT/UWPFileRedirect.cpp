/* Copyright (c) 2026 Skitzelles - part of Game Vault, https://github.com/skitzellez/xbox-game-vault
 * GPL-2.0-or-later, like Dolphin. Same file as XBSX2 for Game Vault's UWPFileRedirect.cpp.
 *
 * Dolphin opens disc images, saves and settings with the C library (fopen and friends), which ends in CreateFileW.
 * In a UWP app that direct call only works where the app container itself has rights: on an NTFS drive
 * that was given "ALL APPLICATION PACKAGES" access. On a FAT32 or exFAT stick, or an NTFS one that was
 * not prepared, it fails with "access denied" although the user allowed file system access
 * (broadFileSystemAccess): that permission is honoured only by the *FromApp functions, which go through
 * the file broker when the direct call is refused and are otherwise the same call.
 *
 * So the import tables of the C runtime and of Dolphin itself are pointed at the FromApp
 * versions. They take the same arguments, and where the direct call is allowed they behave exactly as
 * before. Then any USB drive works, as it already did for Game Vault, xemu and XBSX2.
 *
 * One S (2026-10-25, measured with a probe app on the console): a brokered call takes about 850 ms there, and
 * the first brokered call of the process can fail with ERROR_ACCESS_DENIED while every later one succeeds.
 * Dolphin's first call on the game is the existence check (File::Exists, std::filesystem, GetFileAttributesExW),
 * which answered "does not exist" and was never logged. So every call here is made again, up to kTries times,
 * when it is refused, and every call on a path outside the app's own folders (a game on a USB drive) is written
 * to LocalState\uwp_redirect.log with its result, error and time, failed or not.
 */
#include <windows.h>
#include <fileapifromapp.h>
#include <memoryapi.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <mutex>
#include <string>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>

extern "C" unsigned uwp_redirect_count = 0;   /* how many imports were pointed at FromApp, for the start log */
static void log_redirect(const char *, unsigned n) { uwp_redirect_count = n; }

namespace {

constexpr int kTries = 3;                 /* attempts for a call refused with ERROR_ACCESS_DENIED */
constexpr DWORD kRetryWaitMs = 200;
constexpr LONG kOutsideLogMax = 2000;     /* lines for calls outside the app's folders, per process */
constexpr LONG kInsideFailureLogMax = 50; /* lines for failed calls inside them (missing optional files) */

/* "\\?\F:\..." (the long-path form some cores use) is refused by the broker; the plain path works */
static LPCWSTR Plain(LPCWSTR name)
{
   if (name && name[0] == L'\\' && name[1] == L'\\' && name[2] == L'?' && name[3] == L'\\' && name[4] &&
         name[5] == L':')
      return name + 4;
   return name;
}

wchar_t *Widen(LPCSTR s)
{
   if (!s)
      return NULL;
   int n = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
   wchar_t *w = (wchar_t*)malloc(n * sizeof(wchar_t));
   if (w)
      MultiByteToWideChar(CP_ACP, 0, s, -1, w, n);
   return w;
}

/* The app's own folders: LocalState, the temp folder, the install folder and the publisher cache. A path
 * under none of them is "outside" (a player's drive). Resolved once the WinRT apartment is up; until then
 * every path counts as outside, which only costs log lines. */
struct OwnFolders
{
   std::wstring paths[4];
   bool resolved = false;
};

static OwnFolders &Own()
{
   static OwnFolders own;
   static std::once_flag once;   /* a throw leaves the flag unset, so the next call tries again */
   try
   {
      std::call_once(once, [] {
         using namespace winrt::Windows::Storage;
         own.paths[0] = ApplicationData::Current().LocalFolder().Path();
         own.paths[1] = ApplicationData::Current().TemporaryFolder().Path();
         own.paths[2] = winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path();
         own.paths[3] = ApplicationData::Current().GetPublisherCacheFolder(L"games").Path();
         own.resolved = true;
      });
   }
   catch (...)
   {
   }
   return own;
}

static bool Outside(LPCWSTR name)
{
   if (!name)
      return false;
   OwnFolders &own = Own();
   if (!own.resolved)
      return true;
   for (const std::wstring &p : own.paths)
   {
      if (!p.empty() && _wcsnicmp(name, p.c_str(), p.size()) == 0)
         return false;
   }
   return true;
}

/* The log file: LocalState\uwp_redirect.log, one line per call, the process token first. */
static void LogLine(const char *line, size_t len)
{
   static wchar_t logPath[MAX_PATH + 64];
   if (!logPath[0])
   {
      OwnFolders &own = Own();
      if (!own.resolved)
         return;
      wcsncpy_s(logPath, (own.paths[0] + L"\\uwp_redirect.log").c_str(), _TRUNCATE);
   }
   HANDLE f = CreateFile2FromAppW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ, OPEN_ALWAYS, NULL);
   if (f == INVALID_HANDLE_VALUE)
      return;
   DWORD wrote;
   WriteFile(f, line, (DWORD)len, &wrote, NULL);
   CloseHandle(f);
}

static void LogCall(LPCWSTR what, LPCWSTR name, bool ok, DWORD err, DWORD ms, int tries, bool outside)
{
   static LONG outsides, insideFailures;
   if (!name)
      return;
   const LONG n = InterlockedIncrement(outside ? &outsides : &insideFailures);
   if (n > (outside ? kOutsideLogMax : kInsideFailureLogMax))
      return;
   char line[1300];
   int len = ok ? snprintf(line, sizeof(line), "%ls ok %lu ms try %d: %ls\r\n", what, ms, tries, name)
               : snprintf(line, sizeof(line), "%ls failed (error %lu) %lu ms try %d: %ls\r\n", what, err, ms,
                     tries, name);
   if (len > 0)
      LogLine(line, (size_t)len);
}

/* Makes one FromApp call, again after a short wait when it is refused, and logs it when it failed or when the
 * path is outside the app's folders. `bad` is the call's failure value; GetLastError() is kept for the caller. */
template <typename R, typename F>
static R Brokered(LPCWSTR what, LPCWSTR name, R bad, F call)
{
   const bool outside = Outside(name);
   const ULONGLONG started = GetTickCount64();
   R r = bad;
   DWORD err = 0;
   int tries = 0;
   for (;;)
   {
      tries++;
      r = call();
      err = r == bad ? GetLastError() : 0;
      if (r != bad || err != ERROR_ACCESS_DENIED || tries >= kTries)
         break;
      Sleep(kRetryWaitMs);
   }
   const DWORD ms = (DWORD)(GetTickCount64() - started);
   if (r == bad || outside)
      LogCall(what, name, r != bad, err, ms, tries, outside);
   if (r == bad)
      SetLastError(err);
   return r;
}

BOOL WINAPI GetFileAttributesExW_FromApp(LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info)
{
   name = Plain(name);
   return Brokered<BOOL>(L"GetFileAttributesExW", name, FALSE,
         [&] { return GetFileAttributesExFromAppW(name, level, info); });
}

DWORD WINAPI GetFileAttributesW_FromApp(LPCWSTR name)
{
   WIN32_FILE_ATTRIBUTE_DATA data;
   if (!GetFileAttributesExW_FromApp(name, GetFileExInfoStandard, &data))
      return INVALID_FILE_ATTRIBUTES;
   return data.dwFileAttributes;
}

/* CreateFileFromAppW, and when the file broker refuses the call's extras (FAT32 / exFAT sticks are opened
 * through the broker, which answers ERROR_INVALID_PARAMETER for an inheritable handle and for some flags; the
 * C runtime passes both, and fopen then fails with errno 22) the same open again without them. */
static HANDLE CreateFileW_FromApp_Try(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
      DWORD disposition, DWORD flags, HANDLE templ);

HANDLE WINAPI CreateFileW_FromApp(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
      DWORD disposition, DWORD flags, HANDLE templ)
{
   name = Plain(name);
   wchar_t what[160];
   swprintf_s(what, L"CreateFileW access %08lx share %lx disposition %lu flags %08lx sa %d", access, share,
         disposition, flags, sa ? 1 : 0);
   return Brokered<HANDLE>(what, name, INVALID_HANDLE_VALUE,
         [&] { return CreateFileW_FromApp_Try(name, access, share, sa, disposition, flags, templ); });
}

static HANDLE CreateFileW_FromApp_Try(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
      DWORD disposition, DWORD flags, HANDLE templ)
{
   HANDLE h = CreateFileFromAppW(name, access, share, sa, disposition, flags, templ);
   if (h != INVALID_HANDLE_VALUE)
      return h;
   /* the broker refuses security attributes (an inheritable handle) with error 4252 or 87: try without */
   if (sa)
   {
      h = CreateFileFromAppW(name, access, share, NULL, disposition, flags, templ);
      if (h != INVALID_HANDLE_VALUE)
         return h;
   }
   if (GetLastError() != ERROR_INVALID_PARAMETER)
      return h;
   h = CreateFileFromAppW(name, access, share, NULL, disposition, flags, templ);
   if (h != INVALID_HANDLE_VALUE || GetLastError() != ERROR_INVALID_PARAMETER)
      return h;
   /* attributes and the access-pattern hints only */
   DWORD plain = flags & (0xFFFF | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_BACKUP_SEMANTICS);
   return CreateFileFromAppW(name, access, share, NULL, disposition, plain, NULL);
}

static HANDLE CreateFile2_FromApp_Try(LPCWSTR name, DWORD access, DWORD share, DWORD disposition,
      LPCREATEFILE2_EXTENDED_PARAMETERS ext)
{
   HANDLE h = CreateFile2FromAppW(name, access, share, disposition, ext);
   if (h == INVALID_HANDLE_VALUE && ext && ext->lpSecurityAttributes)
   {
      /* the broker refuses security attributes: try without them */
      CREATEFILE2_EXTENDED_PARAMETERS nosa = *ext;
      nosa.lpSecurityAttributes = NULL;
      h = CreateFile2FromAppW(name, access, share, disposition, &nosa);
   }
   if (h != INVALID_HANDLE_VALUE || GetLastError() != ERROR_INVALID_PARAMETER || !ext)
      return h;
   CREATEFILE2_EXTENDED_PARAMETERS plain = *ext;
   plain.lpSecurityAttributes = NULL;
   plain.hTemplateFile        = NULL;
   plain.dwSecurityQosFlags   = 0;
   plain.dwFileFlags         &= FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_BACKUP_SEMANTICS;
   return CreateFile2FromAppW(name, access, share, disposition, &plain);
}

HANDLE WINAPI CreateFile2_FromApp(LPCWSTR name, DWORD access, DWORD share, DWORD disposition,
      LPCREATEFILE2_EXTENDED_PARAMETERS ext)
{
   name = Plain(name);
   wchar_t what[120];
   swprintf_s(what, L"CreateFile2 access %08lx share %lx disposition %lu", access, share, disposition);
   return Brokered<HANDLE>(what, name, INVALID_HANDLE_VALUE,
         [&] { return CreateFile2_FromApp_Try(name, access, share, disposition, ext); });
}

HANDLE WINAPI CreateFileA_FromApp(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
      DWORD disposition, DWORD flags, HANDLE templ)
{
   wchar_t *w = Widen(name);
   HANDLE h   = CreateFileW_FromApp(w, access, share, sa, disposition, flags, templ);
   DWORD err  = GetLastError();
   free(w);
   SetLastError(err);
   return h;
}

DWORD WINAPI GetFileAttributesA_FromApp(LPCSTR name)
{
   wchar_t *w = Widen(name);
   DWORD a    = GetFileAttributesW_FromApp(w);
   DWORD err  = GetLastError();
   free(w);
   SetLastError(err);
   return a;
}

HANDLE WINAPI FindFirstFileExW_FromApp(LPCWSTR name, FINDEX_INFO_LEVELS level, LPVOID data,
      FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags)
{
   name = Plain(name);
   return Brokered<HANDLE>(L"FindFirstFileExW", name, INVALID_HANDLE_VALUE,
         [&] { return FindFirstFileExFromAppW(name, level, data, op, filter, flags); });
}

HANDLE WINAPI FindFirstFileW_FromApp(LPCWSTR name, LPWIN32_FIND_DATAW data)
{
   return FindFirstFileExW_FromApp(name, FindExInfoStandard, data, FindExSearchNameMatch, NULL, 0);
}

BOOL WINAPI SetFileAttributesW_FromApp(LPCWSTR name, DWORD attributes)
{
   return SetFileAttributesFromAppW(Plain(name), attributes);
}

/* Saving through a temporary file and renaming it over the old one (save files, states, memory cards, the
 * settings). MoveFileFromAppW never replaces, so an existing target is replaced with ReplaceFileFromAppW, or
 * deleted first when that is refused; another drive means copy and delete, as MOVEFILE_COPY_ALLOWED asks. */
BOOL WINAPI MoveFileExW_FromApp(LPCWSTR from, LPCWSTR to, DWORD flags)
{
   from = Plain(from);
   to   = Plain(to);
   if (!to)
      return DeleteFileFromAppW(from);
   if (MoveFileFromAppW(from, to))
      return TRUE;
   DWORD err = GetLastError();
   if ((flags & MOVEFILE_REPLACE_EXISTING) && (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS))
   {
      if (ReplaceFileFromAppW(to, from, NULL, REPLACEFILE_IGNORE_MERGE_ERRORS, NULL, NULL))
         return TRUE;
      if (DeleteFileFromAppW(to) && MoveFileFromAppW(from, to))
         return TRUE;
      err = GetLastError();
   }
   else if ((flags & MOVEFILE_COPY_ALLOWED) && err == ERROR_NOT_SAME_DEVICE)
   {
      if (CopyFileFromAppW(from, to, !(flags & MOVEFILE_REPLACE_EXISTING)) && DeleteFileFromAppW(from))
         return TRUE;
      err = GetLastError();
   }
   SetLastError(err);
   return FALSE;
}

BOOL WINAPI MoveFileW_FromApp(LPCWSTR from, LPCWSTR to)
{
   return MoveFileExW_FromApp(from, to, MOVEFILE_COPY_ALLOWED);
}

BOOL WINAPI ReplaceFileW_FromApp(LPCWSTR replaced, LPCWSTR replacement, LPCWSTR backup, DWORD flags,
      LPVOID exclude, LPVOID reserved)
{
   return ReplaceFileFromAppW(Plain(replaced), Plain(replacement), Plain(backup), flags, exclude, reserved);
}

struct Redirect
{
   const char *name;
   void *to;
};

const Redirect kRedirects[] = {
   { "CreateFileW",          (void*)CreateFileW_FromApp },
   { "CreateFile2",          (void*)CreateFile2_FromApp },
   { "CreateFileA",          (void*)CreateFileA_FromApp },
   { "GetFileAttributesExW", (void*)GetFileAttributesExW_FromApp },
   { "GetFileAttributesW",   (void*)GetFileAttributesW_FromApp },
   { "GetFileAttributesA",   (void*)GetFileAttributesA_FromApp },
   { "FindFirstFileExW",     (void*)FindFirstFileExW_FromApp },
   { "FindFirstFileW",       (void*)FindFirstFileW_FromApp },
   { "CreateDirectoryW",     (void*)CreateDirectoryFromAppW },
   { "DeleteFileW",          (void*)DeleteFileFromAppW },
   { "RemoveDirectoryW",     (void*)RemoveDirectoryFromAppW },
   { "SetFileAttributesW",   (void*)SetFileAttributesW_FromApp },
   { "MoveFileExW",          (void*)MoveFileExW_FromApp },
   { "MoveFileW",            (void*)MoveFileW_FromApp },
   { "ReplaceFileW",         (void*)ReplaceFileW_FromApp },
};

} // namespace

/* Points the module's imports of the functions above at the FromApp versions. Returns how many. */
extern "C" unsigned uwp_redirect_file_apis(void *module)
{
   unsigned count = 0;
   BYTE *base     = (BYTE*)module;
   if (!base)
      return 0;
   IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER*)base;
   if (dos->e_magic != IMAGE_DOS_SIGNATURE)
      return 0;
   IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
   IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
   if (!dir.VirtualAddress)
      return 0;
   for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
         imp->Name; imp++)
   {
      IMAGE_THUNK_DATA *thunk = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
      IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA*)(base +
            (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
      for (; names->u1.AddressOfData; names++, thunk++)
      {
         if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
            continue;
         const char *fn = (const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
         for (const Redirect &r : kRedirects)
         {
            if (strcmp(fn, r.name) || thunk->u1.Function == (ULONG_PTR)r.to)
               continue;
            ULONG old;
            if (VirtualProtectFromApp(&thunk->u1.Function, sizeof(thunk->u1.Function),
                     PAGE_READWRITE, &old))
            {
               thunk->u1.Function = (ULONG_PTR)r.to;
               VirtualProtectFromApp(&thunk->u1.Function, sizeof(thunk->u1.Function), old, &old);
               count++;
            }
            break;
         }
      }
   }
   return count;
}

/* The C and C++ runtimes (std::filesystem lives in msvcp140) and Dolphin itself, once at start-up. */
extern "C" void uwp_redirect_file_apis_startup(void)
{
   static const wchar_t *const modules[] = {
      L"ucrtbase.dll", L"ucrtbased.dll", L"msvcrt.dll", L"msvcp140.dll", L"msvcp140d.dll",
      L"msvcp140_app.dll", L"msvcp140d_app.dll", NULL
   };
   unsigned n = uwp_redirect_file_apis(GetModuleHandleW(NULL));
   for (int i = 0; modules[i]; i++)
   {
      HMODULE m = GetModuleHandleW(modules[i]);
      if (m)
         n += uwp_redirect_file_apis(m);
   }
   log_redirect("C runtime and Dolphin", n);
}

/* The process token, for the logs: whether the process runs in an AppContainer and at which integrity level
 * (the manifest asks for mediumIL; a C# probe with the same attribute still ran in an AppContainer at low on
 * the One S). Written as the first line of uwp_redirect.log and returned for Dolphin's own log. */
extern "C" void uwp_process_token(char *out, size_t size)
{
   HANDLE token;
   if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
   {
      snprintf(out, size, "token: OpenProcessToken failed (error %lu)", GetLastError());
      return;
   }
   DWORD container = 0, len = 0;
   const BOOL knownContainer = GetTokenInformation(token, TokenIsAppContainer, &container, sizeof(container), &len);
   DWORD level = 0;
   BOOL knownLevel = FALSE;
   BYTE label[SECURITY_MAX_SID_SIZE + sizeof(TOKEN_MANDATORY_LABEL)];
   if (GetTokenInformation(token, TokenIntegrityLevel, label, sizeof(label), &len))
   {
      PSID sid = ((TOKEN_MANDATORY_LABEL*)label)->Label.Sid;
      level = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
      knownLevel = TRUE;
   }
   CloseHandle(token);
   const char *name = level >= SECURITY_MANDATORY_HIGH_RID ? "high" :
         level >= SECURITY_MANDATORY_MEDIUM_RID ? "medium" :
         level >= SECURITY_MANDATORY_LOW_RID ? "low" : "untrusted";
   snprintf(out, size, "token: appcontainer=%s integrity=0x%lx (%s) imports redirected=%u",
         knownContainer ? (container ? "1" : "0") : "?", level, knownLevel ? name : "?", uwp_redirect_count);
   char line[400];
   const int n = snprintf(line, sizeof(line), "%s\r\n", out);
   if (n > 0)
      LogLine(line, (size_t)n);
}
