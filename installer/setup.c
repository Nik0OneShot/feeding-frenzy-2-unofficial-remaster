// FF2 Remaster Setup: installs the Unofficial Remaster into a copy of Feeding Frenzy 2 Deluxe
// the player already has, and takes it out again.
//
// Nothing of the game comes with it: the changed game data (FF2.saf, splash/splashScreen.xml)
// is made here from the player's own files (mods.c), after the originals are backed up as
// *.original. The Remaster's own files (the patch, online play, cnc-ddraw) are inside this
// program as resources (setup.rc).
//
// Without arguments it shows a window. For scripts and tests:
//   setup.exe /install "game folder" [/force]    /force: also on an untested copy
//   setup.exe /uninstall "game folder"
// (or --install, --uninstall, --force)
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <ctype.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "mods.h"
#include "resource.h"
#include "saf.h"

#define TITLE L"Feeding Frenzy 2 - Unofficial Remaster Setup"
#define STEAM_APP_ID L"3390"
#define GAME_DIR_NAME L"Feeding Frenzy 2 Deluxe"

// The Steam version the Remaster is made for (plain MD5s).
static const char *const KNOWN_EXE = "051246797fd8f05f76786c85c928d206";   // FeedingFrenzyTwo.exe
static const char *const KNOWN_SAF = "6c8c799c0012d7d73843012c4d465c4a";   // FF2.saf
static const char *const KNOWN_SPLASH = "3be24cf8a3f004a0b58b31cf064d4771"; // splash/splashScreen.xml

// The Remaster's files: resource, where it goes in the game folder.
static const struct { int id; const wchar_t *path; } payload[] = {
    { RES_VERSION_DLL, L"version.dll" },
    { RES_ONLINE_DLL, L"ff2online.dll" },
    { RES_GUEST_EXE, L"ff2guest.exe" },
    { RES_DDRAW_DLL, L"ddraw.dll" },
    { RES_DDRAW_INI, L"ddraw.ini" },
    { RES_SHADER, L"Shaders\\interpolation\\catmull-rom-bilinear.glsl" },
    { RES_README, L"FF2 Remaster - read me.txt" },
};
// Files of cnc-ddraw's a player may have put there already: kept as *.original.
static const wchar_t *const replaced[] = { L"ddraw.dll", L"ddraw.ini" };
// Files the game makes while the Remaster runs, taken out with it.
static const wchar_t *const made[] = { L"ff2wide.ini", L"ff2wide.log", L"ff2guest.log" };

static HWND window, folder_edit, status_edit;
static int cli;

static void say(const wchar_t *fmt, ...)
{
    wchar_t text[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(text, sizeof text / sizeof *text, fmt, ap);
    va_end(ap);
    if (cli) {
        char out[2048];
        int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, out, sizeof out - 2, NULL, NULL);
        if (n > 0) {
            out[n - 1] = '\n';
            DWORD written;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), out, n, &written, NULL);
        }
        return;
    }
    int len = GetWindowTextLengthW(status_edit);
    SendMessageW(status_edit, EM_SETSEL, len, len);
    SendMessageW(status_edit, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageW(status_edit, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
}

// Asks before going on with something unusual; on the command line, only with /force.
static int force;
static int ask(const wchar_t *question)
{
    if (cli) {
        say(L"%ls%ls", question, force ? L" (going on: /force)" : L" (stopping; /force to go on)");
        return force;
    }
    return MessageBoxW(window, question, TITLE, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
}

// --- Files -----------------------------------------------------------------------------

static void join(wchar_t *out, const wchar_t *dir, const wchar_t *name)
{
    swprintf(out, MAX_PATH, L"%ls\\%ls", dir, name);
}

static int exists(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static BYTE *read_file(const wchar_t *path, DWORD *len)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(f, NULL), got = 0;
    BYTE *data = malloc(size + 1);
    if (!ReadFile(f, data, size, &got, NULL) || got != size) {
        free(data);
        data = NULL;
    }
    CloseHandle(f);
    if (data) data[size] = 0;
    *len = size;
    return data;
}

static void make_dirs(const wchar_t *path)
{
    wchar_t dir[MAX_PATH];
    wcscpy(dir, path);
    for (wchar_t *p = dir + 3; *p; p++)
        if (*p == L'\\') {
            *p = 0;
            CreateDirectoryW(dir, NULL);
            *p = L'\\';
        }
}

// Through a temporary file, so a failed write never leaves half a file.
static int write_file(const wchar_t *path, const void *data, DWORD len)
{
    wchar_t temp[MAX_PATH];
    swprintf(temp, MAX_PATH, L"%ls.new", path);
    make_dirs(path);
    HANDLE f = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD written = 0;
    BOOL ok = WriteFile(f, data, len, &written, NULL) && written == len;
    CloseHandle(f);
    if (ok) ok = MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING);
    if (!ok) DeleteFileW(temp);
    return ok;
}

static void hex_md5(const BYTE *data, DWORD len, char out[33])
{
    BYTE h[16];
    md5_plain(data, len, h);
    for (int i = 0; i < 16; i++) sprintf(out + i * 2, "%02x", h[i]);
}

static const BYTE *resource(int id, DWORD *len)
{
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL g = r ? LoadResource(NULL, r) : NULL;
    *len = r ? SizeofResource(NULL, r) : 0;
    return g ? LockResource(g) : NULL;
}

// --- Settings: the template's lines, with the values the player already set ----------

static int ini_key(const char *line, char *key, int max)
{
    while (*line == ' ' || *line == '\t') line++;
    int n = 0;
    while ((isalnum((BYTE)line[n]) || line[n] == '_') && n < max - 1) key[n] = line[n], n++;
    key[n] = 0;
    const char *p = line + n;
    while (*p == ' ' || *p == '\t') p++;
    return n && *p == '=';
}

static int install_settings(const wchar_t *dir)
{
    DWORD tlen, olen = 0;
    const BYTE *template = resource(RES_SETTINGS, &tlen);
    wchar_t path[MAX_PATH];
    join(path, dir, L"ff2wide.ini");
    BYTE *old = read_file(path, &olen);
    char *out = malloc(tlen * 2 + olen + 16), *o = out;
    int kept = 0;
    for (const char *line = (const char *)template; line < (const char *)template + tlen;) {
        const char *end = memchr(line, '\n', (const char *)template + tlen - line);
        size_t n = end ? (size_t)(end - line) : (size_t)((const char *)template + tlen - line);
        if (n && line[n - 1] == '\r') n--;
        char key[64], line_buf[512];
        snprintf(line_buf, sizeof line_buf, "%.*s", (int)n, line);
        int written = 0;
        if (old && ini_key(line_buf, key, sizeof key)) {
            // the player's value for this key, if they have one
            for (char *l = (char *)old; l && *l;) {
                char *e = strchr(l, '\n'), k[64];
                if (e) *e = 0;
                if (ini_key(l, k, sizeof k) && !_stricmp(k, key)) {
                    char *v = strchr(l, '=') + 1;
                    size_t vl = strlen(v);
                    while (vl && (v[vl - 1] == '\r' || v[vl - 1] == ' ')) vl--;
                    while (*v == ' ') v++, vl--;
                    o += sprintf(o, "%s=%.*s\r\n", key, (int)vl, v);
                    written = 1;
                    kept++;
                }
                if (e) *e = '\n';
                if (written) break;
                l = e ? e + 1 : NULL;
            }
        }
        if (!written) o += sprintf(o, "%s\r\n", line_buf);
        line = end ? end + 1 : (const char *)template + tlen;
    }
    int ok = write_file(path, out, (DWORD)(o - out));
    if (ok && old) say(L"Settings: ff2wide.ini updated, keeping the %d settings you had.", kept);
    free(out);
    free(old);
    return ok;
}

// --- Install and uninstall --------------------------------------------------------------

// The original of a file the Remaster changes: its *.original backup, else the file itself.
static BYTE *original(const wchar_t *dir, const wchar_t *name, DWORD *len, const char *known, const wchar_t *what)
{
    wchar_t path[MAX_PATH], backup[MAX_PATH];
    join(path, dir, name);
    swprintf(backup, MAX_PATH, L"%ls.original", path);
    BYTE *data = read_file(exists(backup) ? backup : path, len);
    if (!data) {
        say(L"Can't read %ls.", name);
        return NULL;
    }
    char md5[33];
    hex_md5(data, *len, md5);
    if (strcmp(md5, known)) {
        wchar_t q[512];
        swprintf(q, 512, L"%ls isn't the one from the Steam version the Remaster was made for (it may be a different "
                         L"version, or changed by another mod).\n\nTry anyway?", what);
        if (!ask(q)) {
            free(data);
            return NULL;
        }
    }
    return data;
}

// 0: failed, 1: nothing to do (backed up before, or no such file), 2: backed up now.
static int backup_once(const wchar_t *dir, const wchar_t *name)
{
    wchar_t path[MAX_PATH], backup[MAX_PATH];
    join(path, dir, name);
    swprintf(backup, MAX_PATH, L"%ls.original", path);
    if (exists(backup) || !exists(path)) return 1;
    return CopyFileW(path, backup, TRUE) ? 2 : 0;
}

static int install(const wchar_t *dir)
{
    wchar_t path[MAX_PATH];
    say(L"Installing into %ls", dir);
    join(path, dir, L"FeedingFrenzyTwo.exe");
    DWORD len;
    BYTE *exe = read_file(path, &len);
    if (!exe) {
        say(L"There's no FeedingFrenzyTwo.exe there: that's not the game's folder.");
        return 0;
    }
    char md5[33];
    hex_md5(exe, len, md5);
    free(exe);
    if (strcmp(md5, KNOWN_EXE) &&
        !ask(L"This FeedingFrenzyTwo.exe isn't the Steam version's. The Remaster patches the game's code in "
             L"memory and was only made and tested for that one; on another version parts of it may do "
             L"nothing, or the game may not start.\n\nInstall anyway?"))
        return 0;

    DWORD saf_len, splash_len;
    BYTE *saf_file = original(dir, L"FF2.saf", &saf_len, KNOWN_SAF, L"FF2.saf");
    if (!saf_file) return 0;
    BYTE *splash = original(dir, L"splash\\splashScreen.xml", &splash_len, KNOWN_SPLASH, L"splash\\splashScreen.xml");
    if (!splash) return 0;
    say(L"Making the Remaster's game data from your copy...");
    Saf saf;
    if (!saf_read(&saf, saf_file, saf_len)) {
        say(L"FF2.saf can't be read: is it damaged? Steam's \"Verify integrity of game files\" fixes it.");
        return 0;
    }
    char err[512];
    BYTE *new_splash;
    DWORD new_splash_len;
    if (!mods_apply(&saf, splash, splash_len, &new_splash, &new_splash_len, err, sizeof err)) {
        say(L"Can't make the game data: %hs", err);
        return 0;
    }
    DWORD new_len;
    BYTE *new_saf = saf_build(&saf, &new_len);

    int saf_backup = backup_once(dir, L"FF2.saf"), splash_backup = backup_once(dir, L"splash\\splashScreen.xml");
    if (!saf_backup || !splash_backup) {
        say(L"Can't back up the original game files (is the folder read-only?).");
        return 0;
    }
    if (saf_backup == 2 || splash_backup == 2)
        say(L"Backed up the originals as FF2.saf.original and splash\\splashScreen.xml.original.");
    for (int i = 0; i < (int)(sizeof replaced / sizeof *replaced); i++) {
        // cnc-ddraw the player had already: kept, unless it's this very file
        DWORD have_len, ours_len;
        int id = !wcscmp(replaced[i], L"ddraw.dll") ? RES_DDRAW_DLL : RES_DDRAW_INI;
        const BYTE *ours = resource(id, &ours_len);
        join(path, dir, replaced[i]);
        BYTE *have = read_file(path, &have_len);
        if (have && (have_len != ours_len || memcmp(have, ours, have_len)) && backup_once(dir, replaced[i]) == 2)
            say(L"Kept your own %ls as %ls.original.", replaced[i], replaced[i]);
        free(have);
    }
    join(path, dir, L"FF2.saf");
    if (!write_file(path, new_saf, new_len)) {
        say(L"Can't write FF2.saf. Is the game running? Close it and try again.");
        return 0;
    }
    join(path, dir, L"splash\\splashScreen.xml");
    if (!write_file(path, new_splash, new_splash_len)) {
        say(L"Can't write splash\\splashScreen.xml.");
        return 0;
    }
    for (int i = 0; i < (int)(sizeof payload / sizeof *payload); i++) {
        DWORD plen;
        const BYTE *data = resource(payload[i].id, &plen);
        join(path, dir, payload[i].path);
        if (!data || !write_file(path, data, plen)) {
            say(L"Can't write %ls. Is the game running? Close it and try again.", payload[i].path);
            return 0;
        }
    }
    if (!install_settings(dir)) {
        say(L"Can't write ff2wide.ini.");
        return 0;
    }
    say(L"Done! Start the game from Steam as usual. Settings are in ff2wide.ini in the game folder.");
    return 1;
}

static void restore(const wchar_t *dir, const wchar_t *name, int keep_if_no_backup)
{
    wchar_t path[MAX_PATH], backup[MAX_PATH];
    join(path, dir, name);
    swprintf(backup, MAX_PATH, L"%ls.original", path);
    if (exists(backup)) MoveFileExW(backup, path, MOVEFILE_REPLACE_EXISTING);
    else if (!keep_if_no_backup) DeleteFileW(path);
}

static int uninstall(const wchar_t *dir)
{
    wchar_t path[MAX_PATH], backup[MAX_PATH];
    say(L"Uninstalling from %ls", dir);
    join(path, dir, L"FF2.saf");
    swprintf(backup, MAX_PATH, L"%ls.original", path);
    if (!exists(backup)) {
        say(L"There's no FF2.saf.original: either the Remaster isn't installed here, or Steam already put "
            L"the original back. Taking out the Remaster's other files.");
    } else if (!MoveFileExW(backup, path, MOVEFILE_REPLACE_EXISTING)) {
        say(L"Can't put FF2.saf back. Is the game running? Close it and try again.");
        return 0;
    }
    restore(dir, L"splash\\splashScreen.xml", 1);
    for (int i = 0; i < (int)(sizeof payload / sizeof *payload); i++) {
        join(path, dir, payload[i].path);
        if (exists(path) && !DeleteFileW(path) && !wcschr(payload[i].path, L'\\'))
            say(L"Can't remove %ls. Is the game running?", payload[i].path);
    }
    // the player's own cnc-ddraw back, once the Remaster's is gone
    for (int i = 0; i < (int)(sizeof replaced / sizeof *replaced); i++) restore(dir, replaced[i], 1);
    join(path, dir, L"Shaders\\interpolation");
    RemoveDirectoryW(path); // only if empty
    join(path, dir, L"Shaders");
    RemoveDirectoryW(path);
    for (int i = 0; i < (int)(sizeof made / sizeof *made); i++) {
        join(path, dir, made[i]);
        DeleteFileW(path);
    }
    say(L"Done: the game is back as it was.");
    return 1;
}

// --- Finding the game -----------------------------------------------------------------

static int is_game_dir(const wchar_t *dir)
{
    wchar_t path[MAX_PATH];
    join(path, dir, L"FeedingFrenzyTwo.exe");
    return exists(path);
}

// Steam's install of the game: its uninstall entry (Setup is 32-bit, so this reads the 32-bit
// registry, where Steam keeps them), else each Steam library folder.
static int find_game(wchar_t *out)
{
    DWORD size = MAX_PATH * sizeof(wchar_t);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App " STEAM_APP_ID,
                     L"InstallLocation", RRF_RT_REG_SZ, NULL, out, &size) == ERROR_SUCCESS &&
        is_game_dir(out))
        return 1;
    wchar_t steam[MAX_PATH], vdf[MAX_PATH];
    size = sizeof steam;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, NULL, steam, &size))
        return 0;
    for (wchar_t *p = steam; *p; p++)
        if (*p == L'/') *p = L'\\';
    swprintf(out, MAX_PATH, L"%ls\\steamapps\\common\\" GAME_DIR_NAME, steam);
    if (is_game_dir(out)) return 1;
    swprintf(vdf, MAX_PATH, L"%ls\\steamapps\\libraryfolders.vdf", steam);
    DWORD len;
    char *text = (char *)read_file(vdf, &len);
    for (char *p = text; p && (p = strstr(p, "\"path\""));) {
        p = strchr(p + 6, '"');
        char *end = p ? strchr(p + 1, '"') : NULL;
        if (!end) break;
        char lib[MAX_PATH];
        int n = 0;
        for (char *c = p + 1; c < end && n < MAX_PATH - 1; c++) {
            if (*c == '\\' && c[1] == '\\') c++;
            lib[n++] = *c;
        }
        lib[n] = 0;
        wchar_t wlib[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, lib, -1, wlib, MAX_PATH);
        swprintf(out, MAX_PATH, L"%ls\\steamapps\\common\\" GAME_DIR_NAME, wlib);
        if (is_game_dir(out)) {
            free(text);
            return 1;
        }
        p = end + 1;
    }
    free(text);
    return 0;
}

// --- The window -------------------------------------------------------------------------

enum { ID_FOLDER = 100, ID_BROWSE, ID_INSTALL, ID_UNINSTALL, ID_PLAY };

// Steam folders under Program Files may need administrator rights to change. If this one
// can't be written, offers to start again as administrator (which closes this window).
static int writable_or_elevate(const wchar_t *dir)
{
    wchar_t probe[MAX_PATH];
    join(probe, dir, L"ff2remaster-write-test.tmp");
    HANDLE f = CreateFileW(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        CloseHandle(f);
        return 1;
    }
    if (GetLastError() != ERROR_ACCESS_DENIED) return 1; // let the install itself say what's wrong
    if (MessageBoxW(window, L"Windows doesn't let this program change the game's folder without administrator "
                            L"rights.\n\nStart Setup again as administrator?", TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return 0;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    if ((INT_PTR)ShellExecuteW(window, L"runas", exe, NULL, NULL, SW_SHOWNORMAL) > 32) DestroyWindow(window);
    return 0;
}

static int chosen_dir(wchar_t *dir)
{
    GetWindowTextW(folder_edit, dir, MAX_PATH);
    size_t n = wcslen(dir);
    while (n && (dir[n - 1] == L'\\' || dir[n - 1] == L' ')) dir[--n] = 0;
    if (is_game_dir(dir)) return 1;
    MessageBoxW(window, L"That folder doesn't have the game in it (FeedingFrenzyTwo.exe). Choose the "
                        L"\"" GAME_DIR_NAME L"\" folder.", TITLE, MB_ICONINFORMATION);
    return 0;
}

static void browse(void)
{
    BROWSEINFOW bi = { .hwndOwner = window, .lpszTitle = L"Choose the \"" GAME_DIR_NAME L"\" folder",
                       .ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE };
    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&bi);
    wchar_t dir[MAX_PATH];
    if (item && SHGetPathFromIDListW(item, dir)) SetWindowTextW(folder_edit, dir);
    if (item) CoTaskMemFree(item);
}

static HWND control(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id, HFONT font)
{
    HWND c = CreateWindowExW(!wcscmp(cls, L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w,
                             h, window, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

static LRESULT CALLBACK window_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND: {
        wchar_t dir[MAX_PATH];
        switch (LOWORD(wp)) {
        case ID_BROWSE: browse(); break;
        case ID_INSTALL:
        case ID_UNINSTALL:
            if (!chosen_dir(dir) || !writable_or_elevate(dir)) break;
            SetCursor(LoadCursor(NULL, IDC_WAIT));
            if (LOWORD(wp) == ID_INSTALL) install(dir);
            else uninstall(dir);
            SetCursor(LoadCursor(NULL, IDC_ARROW));
            break;
        case ID_PLAY: ShellExecuteW(w, L"open", L"steam://rungameid/" STEAM_APP_ID, NULL, NULL, SW_SHOWNORMAL); break;
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC: return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

static int run_window(void)
{
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    WNDCLASSW wc = { .lpfnWndProc = window_proc, .hInstance = GetModuleHandleW(NULL), .lpszClassName = L"ff2remaster_setup",
                     .hCursor = LoadCursor(NULL, IDC_ARROW), .hbrBackground = GetSysColorBrush(COLOR_WINDOW) };
    RegisterClassW(&wc);
    HDC screen = GetDC(NULL);
    int dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(NULL, screen);
#define S(v) MulDiv(v, dpi, 96)
    RECT r = { 0, 0, S(560), S(380) };
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    window = CreateWindowW(wc.lpszClassName, TITLE, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    NONCLIENTMETRICSW ncm = { .cbSize = sizeof ncm };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_BOLD;
    ncm.lfMessageFont.lfHeight = ncm.lfMessageFont.lfHeight * 3 / 2;
    HFONT big = CreateFontIndirectW(&ncm.lfMessageFont);
    control(L"STATIC", L"Feeding Frenzy 2 - Unofficial Remaster", 0, S(16), S(14), S(528), S(28), 0, big);
    control(L"STATIC",
            L"Widescreen, high frame rates, and co-op for up to 4 players on one PC or 10 online. "
            L"Installs into your own copy of Feeding Frenzy 2 Deluxe (Steam); Uninstall puts it back as it was.",
            0, S(16), S(46), S(528), S(36), 0, font);
    control(L"STATIC", L"Game folder:", 0, S(16), S(94), S(528), S(18), 0, font);
    folder_edit = control(L"EDIT", L"", ES_AUTOHSCROLL, S(16), S(114), S(432), S(24), ID_FOLDER, font);
    control(L"BUTTON", L"Browse...", BS_PUSHBUTTON, S(456), S(113), S(88), S(26), ID_BROWSE, font);
    control(L"BUTTON", L"Install", BS_DEFPUSHBUTTON, S(16), S(150), S(120), S(30), ID_INSTALL, font);
    control(L"BUTTON", L"Uninstall", BS_PUSHBUTTON, S(144), S(150), S(120), S(30), ID_UNINSTALL, font);
    control(L"BUTTON", L"Play", BS_PUSHBUTTON, S(424), S(150), S(120), S(30), ID_PLAY, font);
    status_edit = control(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, S(16), S(192), S(528),
                          S(172), 0, font);
    wchar_t dir[MAX_PATH];
    if (find_game(dir)) {
        SetWindowTextW(folder_edit, dir);
        say(L"Found the game at %ls", dir);
    } else {
        say(L"Couldn't find the game's Steam folder by myself: choose it with Browse.");
    }
    ShowWindow(window, SW_SHOW);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(window, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)inst, (void)prev, (void)cmd, (void)show;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    int argc;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    // options start with / or -- (shells like Git Bash turn "/install" into a path)
    for (int i = 1; i < argc; i++) {
        wchar_t *a = argv[i];
        if (a[0] == L'-' && a[1] == L'-') argv[i] = a = a + 1, a[0] = L'/';
        if (!_wcsicmp(a, L"/force")) force = 1;
    }
    if (argc >= 3 && (!_wcsicmp(argv[1], L"/install") || !_wcsicmp(argv[1], L"/uninstall"))) {
        cli = 1;
        return !_wcsicmp(argv[1], L"/install") ? !install(argv[2]) : !uninstall(argv[2]);
    }
    return run_window();
}
