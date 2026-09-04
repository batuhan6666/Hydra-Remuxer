#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* xxhash.h (v0.8.3) tek basina yeterli, ayrica obje derlenmiyor.
   XXH_INLINE_ALL'i acmayi unutma yoksa link patlar.
   Not: -fanalyzer buradaki hizali malloc desenini anlamayip sahte
   leak uyarisi veriyor, o yuzden asagidaki pragma var. create/free
   eslesmesi SideThread'de tek noktada, sorun yok. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
#define XXH_INLINE_ALL
#include "xxhash.h"
#pragma GCC diagnostic pop

/* Hydra Remuxer: Win32 arayuz + ffmpeg ile mkv'ya tasima.
   video/ses'e dokunmaz (-c copy), bitince XXH3 ile karsilastirir.
   derleme: build.bat (mingw). bayraklarla oynama, hiz oradan geliyor. */

#define IDC_LIST      101
#define IDC_BTN_ADD   102
#define IDC_BTN_START 103
#define IDC_BTN_STOP  104
#define IDC_BTN_CLEAR 105
#define IDC_SPIN_EDIT 106
#define IDC_SPIN      107
#define IDC_PROG      108
#define IDC_STATUS    109
#define IDC_CHK_VERIFY 110
#define IDC_HINT      111

#define WM_APP_JOB_UPDATE (WM_APP + 1)
#define WM_APP_ALL_DONE   (WM_APP + 2)

#define MAXJOBS 4096

typedef enum { JS_QUEUED = 0, JS_RUNNING, JS_DONE, JS_ERROR, JS_CANCELLED } JobStatus;

typedef struct {
    wchar_t  *in;          /* tam giris yolu */
    wchar_t  *out;         /* tam cikis yolu (worker calisirken uretilir) */
    wchar_t  *name;        /* listede gorunen dosya adi */
    ULONGLONG size;
    JobStatus st;
    int       pct;         /* 0..100, -1 = bilinmiyor */
    wchar_t   info[128];   /* hiz / hata detayi */
} Job;

static HINSTANCE g_hInst;
static HWND g_hMain, g_hList, g_hBtnAdd, g_hBtnStart, g_hBtnStop, g_hBtnClear;
static HWND g_hSpinEdit, g_hSpin, g_hProg, g_hStatus;
static HWND g_hChkVerify;
static BOOL g_doVerify = TRUE;
static HWND g_hHint;
static HFONT g_hFontBig;
static HIMAGELIST g_hImg;
static ITaskbarList3 *g_pTask;

/* asagida tanimlananlar, yukaridan cagrildigi icin ondeklarasyon */
static int StatusIcon(JobStatus s);
static void TaskSet(int state, ULONGLONG done, ULONGLONG total);
static HFONT g_hFont;

static Job *g_jobs[MAXJOBS];
static int  g_count = 0;
static CRITICAL_SECTION g_cs;

static volatile BOOL g_busy = FALSE;
static volatile BOOL g_stop = FALSE;
static volatile LONG g_active = 0;
static int    g_maxPar = 4;
static HANDLE g_procs[MAXJOBS];   /* calisan ffmpeg process handle'lari */
static ULONGLONG g_startTick = 0;

// yardimcilar

static const wchar_t *StatusText(JobStatus s) {
    switch (s) {
    case JS_QUEUED:    return L"Bekliyor";
    case JS_RUNNING:   return L"İşleniyor";
    case JS_DONE:      return L"Bitti";
    case JS_ERROR:     return L"Hata";
    case JS_CANCELLED: return L"İptal";
    default:           return L"?";
    }
}

static void FmtSize(ULONGLONG b, wchar_t *o, int cch) {
    if (b >= (ULONGLONG)1024*1024*1024)
        swprintf(o, cch, L"%.2f GB", (double)b / 1073741824.0);
    else if (b >= (ULONGLONG)1024*1024)
        swprintf(o, cch, L"%.1f MB", (double)b / 1048576.0);
    else if (b >= 1024)
        swprintf(o, cch, L"%.0f KB", (double)b / 1024.0);
    else
        swprintf(o, cch, L"%llu B", b);
}

static int CpuCount(void) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    if (si.dwNumberOfProcessors < 1) return 2;
    if (si.dwNumberOfProcessors > 16) return 16;
    return (int)si.dwNumberOfProcessors;
}

static BOOL FileExistsW(const wchar_t *p) {
    DWORD a = GetFileAttributesW(p);
    return (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY));
}

/* Klasor drop'unda filtre icin medya uzantilari.
   (Tek dosya birakmada filtre YOK: ffmpeg ne acarsa kabul.) */
static BOOL IsMediaExt(const wchar_t *ext) {
    static const wchar_t *list[] = {
        L".mp4",L".m4v",L".mov",L".avi",L".mkv",L".ts",L".m2ts",L".mts",
        L".m2t",L".flv",L".wmv",L".asf",L".webm",L".mpg",L".mpeg",L".vob",
        L".3gp",L".3g2",L".ogv",L".rm",L".rmvb",L".divx",L".m4a",L".mp3",
        L".flac",L".aac",L".ac3",L".dts",L".wav",L".opus",L".mka",L".mpv",NULL
    };
    int i;
    if (!ext || !*ext) return FALSE;
    for (i = 0; list[i]; i++)
        if (_wcsicmp(ext, list[i]) == 0) return TRUE;
    return FALSE;
}

/* Cikis yolu: giris klasoru + ayni isim + .mkv (giris mkv ise .remux.mkv).
   Varsa " (n)" ekler, hicbir seyin uzerine yazmaz. */
static void BuildOutputPath(const wchar_t *in, wchar_t *out, int cch) {
    wchar_t dir[32768], stem[32768], ext[64], cand[32768];
    const wchar_t *slash, *dot, *fname;
    size_t dirlen;

    slash = wcsrchr(in, L'\\');
    if (!slash) slash = wcsrchr(in, L'/');
    if (slash) {
        dirlen = (size_t)(slash - in) + 1;
        if (dirlen >= 32767) dirlen = 32767;
        wcsncpy(dir, in, dirlen); dir[dirlen] = 0;
        fname = slash + 1;
    } else {
        wcscpy(dir, L".\\"); fname = in;
    }
    dot = wcsrchr(fname, L'.');
    if (dot && dot != fname) {
        size_t sl = (size_t)(dot - fname);
        if (sl >= 32767) sl = 32767;
        wcsncpy(stem, fname, sl); stem[sl] = 0;
        wcsncpy(ext, dot, 63); ext[63] = 0;
    } else {
        wcsncpy(stem, fname, 32767); stem[32767] = 0; ext[0] = 0;
    }

    if (_wcsicmp(ext, L".mkv") == 0)
        _snwprintf(cand, 32768, L"%s%s.remux.mkv", dir, stem);
    else
        _snwprintf(cand, 32768, L"%s%s.mkv", dir, stem);
    cand[32767] = 0;

    if (!FileExistsW(cand)) { wcsncpy(out, cand, cch - 1); out[cch-1] = 0; return; }
    {
        int n;
        const wchar_t *suffix = (_wcsicmp(ext, L".mkv") == 0) ? L".remux.mkv" : L".mkv";
        for (n = 1; n < 10000; n++) {
            _snwprintf(cand, 32768, L"%s%s (%d)%s", dir, stem, n, suffix);
            cand[32767] = 0;
            if (!FileExistsW(cand)) break;
        }
        wcsncpy(out, cand, cch - 1); out[cch-1] = 0;
    }
}

/* ffmpeg.exe bul: 1) exe yani 2) PATH (WinGet Links dahil) */
static BOOL FindFfmpeg(wchar_t *out, int cch) {
    static wchar_t exeDir[32768];
    static BOOL haveDir = FALSE;
    wchar_t cand[32768], found[32768];
    wchar_t *tail;

    if (!haveDir) {
        DWORD n = GetModuleFileNameW(NULL, exeDir, 32767);
        if (n > 0 && n < 32767) {
            tail = wcsrchr(exeDir, L'\\');
            if (tail) *(tail + 1) = 0;
            haveDir = TRUE;
        }
    }
    if (haveDir) {
        _snwprintf(cand, 32768, L"%sffmpeg.exe", exeDir); cand[32767] = 0;
        if (FileExistsW(cand)) { wcsncpy(out, cand, cch - 1); out[cch-1] = 0; return TRUE; }
    }
    if (SearchPathW(NULL, L"ffmpeg.exe", NULL, cch, out, NULL) > 0)
        return TRUE;
    if (SearchPathW(NULL, L"ffmpeg", L".exe", 32768, found, NULL) > 0) {
        wcsncpy(out, found, cch - 1); out[cch-1] = 0; return TRUE;
    }
    return FALSE;
}

// kuyruk / liste

static void UpdateRow(int idx) {
    Job *j;
    wchar_t status[96], sz[32];
    LVITEMW li;
    if (idx < 0 || idx >= g_count) return;
    j = g_jobs[idx];
    if (j->st == JS_RUNNING && j->pct >= 0)
        swprintf(status, 96, L"İşleniyor %%%d", j->pct);
    else
        wcsncpy(status, StatusText(j->st), 95);
    status[95] = 0;
    FmtSize(j->size, sz, 32);

    memset(&li, 0, sizeof(li));
    li.mask = LVIF_TEXT | LVIF_IMAGE; li.iItem = idx;
    li.iSubItem = 0; li.pszText = j->name; li.iImage = StatusIcon(j->st);
    ListView_SetItem(g_hList, &li);
    li.mask = LVIF_TEXT;
    li.iSubItem = 1; li.pszText = sz;
    ListView_SetItem(g_hList, &li);
    li.iSubItem = 2; li.pszText = status;
    ListView_SetItem(g_hList, &li);
    li.iSubItem = 3; li.pszText = j->info;
    ListView_SetItem(g_hList, &li);
}

static void RefreshTotals(void) {
    int i, doneN = 0;
    wchar_t t[256];
    if (g_count == 0) {
        SendMessageW(g_hProg, PBM_SETPOS, 0, 0);
        SendMessageW(g_hStatus, SB_SETTEXTW, 0, (LPARAM)L"Hazır — dosya sürükleyip bırakın");
        SendMessageW(g_hStatus, SB_SETTEXTW, 1, (LPARAM)L"");
        if (g_hHint) ShowWindow(g_hHint, SW_SHOW);
        TaskSet(TBPF_NOPROGRESS, 0, 0);
        return;
    }
    for (i = 0; i < g_count; i++)
        if (g_jobs[i]->st == JS_DONE || g_jobs[i]->st == JS_ERROR || g_jobs[i]->st == JS_CANCELLED)
            doneN++;
    SendMessageW(g_hProg, PBM_SETPOS, (WPARAM)(doneN * 100 / g_count), 0);
    if (g_busy)
        swprintf(t, 256, L"%d/%d tamamlandı • %d aktif iş", doneN, g_count, (int)g_active);
    else
        swprintf(t, 256, L"%d dosya kuyrukta", g_count);
    SendMessageW(g_hStatus, SB_SETTEXTW, 0, (LPARAM)t);
    if (g_hHint) ShowWindow(g_hHint, SW_HIDE);
    if (g_busy) TaskSet(TBPF_NORMAL, (ULONGLONG)doneN, (ULONGLONG)g_count);
}

// UI thread'inden cagrilir. donus: index, olmazsa -1
static int AddJob(const wchar_t *path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    const wchar_t *slash;
    Job *j;
    LVITEMW li;
    int i, idx;

    if (g_count >= MAXJOBS) return -1;
    for (i = 0; i < g_count; i++)
        if (_wcsicmp(g_jobs[i]->in, path) == 0) return -1; /* duplicate */

    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return -1;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return -1;

    j = (Job *)calloc(1, sizeof(Job));
    if (!j) return -1;
    j->in = _wcsdup(path);
    j->out = _wcsdup(L"");
    slash = wcsrchr(path, L'\\');
    if (!slash) slash = wcsrchr(path, L'/');
    j->name = _wcsdup(slash ? slash + 1 : path);
    if (!j->in || !j->out || !j->name) { /* OOM: yarim job birakma */
        free(j->in); free(j->out); free(j->name); free(j);
        return -1;
    }
    j->size = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    j->st = JS_QUEUED; j->pct = -1;
    wcscpy(j->info, L"");

    EnterCriticalSection(&g_cs);
    idx = g_count;
    g_jobs[g_count++] = j;
    memset(&li, 0, sizeof(li));
    li.mask = LVIF_TEXT | LVIF_PARAM;
    li.iItem = idx; li.iSubItem = 0;
    li.pszText = j->name; li.lParam = (LPARAM)idx;
    idx = ListView_InsertItem(g_hList, &li);
    LeaveCriticalSection(&g_cs);

    UpdateRow(idx);
    RefreshTotals();
    return idx;
}

static void AddFolderDropped(const wchar_t *dir) {
    wchar_t pat[32768];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    _snwprintf(pat, 32768, L"%s\\*", dir); pat[32767] = 0;
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        {
            const wchar_t *dot = wcsrchr(fd.cFileName, L'.');
            if (IsMediaExt(dot)) {
                wchar_t full[32768];
                _snwprintf(full, 32768, L"%s\\%s", dir, fd.cFileName); full[32767] = 0;
                AddJob(full);
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* ---- ffmpeg ciktisindan ilerleme okuma ---- */

typedef struct {
    LONGLONG dur_us;      /* toplam sure, 0 = bilinmiyor */
    LONGLONG out_us;      /* yazilan konum */
    char speed[32];
    char lastErr[512];
    BOOL ended;
} ProgState;

static void ProcessLine(Job *j, ProgState *ps, const char *line, int idx, ULONGLONG *lastPost) {
    const char *p;
    (void)j;

    if ((p = strstr(line, "Duration:")) != NULL && ps->dur_us == 0) {
        int h = 0, m = 0; double s = 0;
        if (sscanf(p + 9, " %d:%d:%lf", &h, &m, &s) == 3 && (h > 0 || m > 0 || s > 0))
            ps->dur_us = (LONGLONG)(((h * 3600.0 + m * 60.0 + s) * 1000000.0));
    }
    if ((p = strstr(line, "out_time_us=")) != NULL)
        ps->out_us = _atoi64(p + 12);
    else if ((p = strstr(line, "out_time_ms=")) != NULL)
        ps->out_us = _atoi64(p + 12); /* adi ms, degeri mikrosaniye (ffmpeg huyu) */
    if ((p = strstr(line, "speed=")) != NULL) {
        size_t n = 0;
        p += 6;
        while (*p == ' ') p++;
        while (p[n] && p[n] != ' ' && n < sizeof(ps->speed) - 1) { ps->speed[n] = p[n]; n++; }
        ps->speed[n] = 0;
    }
    if (strstr(line, "progress=end") != NULL) ps->ended = TRUE;
    if (strstr(line, "Error") != NULL || strstr(line, "error") != NULL ||
        strstr(line, "Invalid") != NULL || strstr(line, "No such") != NULL) {
        strncpy(ps->lastErr, line, sizeof(ps->lastErr) - 1);
        ps->lastErr[sizeof(ps->lastErr) - 1] = 0;
    } else if (strlen(line) > 0 && strlen(line) < 200 && strchr(line, '=') == NULL &&
               strstr(line, "frame=") == NULL && strstr(line, "size=") == NULL) {
        /* son anlamli log satirini sakla (hata durumunda gosterilir) */
        if (ps->lastErr[0] == 0 || strstr(line, ".mp4") || strstr(line, ".mkv") || strstr(line, ": ")) {
            strncpy(ps->lastErr, line, sizeof(ps->lastErr) - 1);
            ps->lastErr[sizeof(ps->lastErr) - 1] = 0;
        }
    }

    /* her satirda PostMessage yapma, UI'yi bogar. % degisince ya da 300ms'de bir */
    {
        int pct = -1;
        ULONGLONG now = GetTickCount64();
        if (ps->dur_us > 0 && ps->out_us > 0) {
            pct = (int)(ps->out_us * 100 / ps->dur_us);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
        }
        EnterCriticalSection(&g_cs);
        if (pct >= 0 && pct != g_jobs[idx]->pct) {
            g_jobs[idx]->pct = pct;
            if (ps->speed[0]) {
                wchar_t ws[32]; MultiByteToWideChar(CP_UTF8, 0, ps->speed, -1, ws, 32);
                _snwprintf(g_jobs[idx]->info, 128, L"%s hız", ws);
                g_jobs[idx]->info[127] = 0;
            }
            *lastPost = now;
            LeaveCriticalSection(&g_cs);
            PostMessageW(g_hMain, WM_APP_JOB_UPDATE, (WPARAM)idx, 0);
        } else if (now - *lastPost > 300) {
            if (ps->speed[0]) {
                wchar_t ws[32]; MultiByteToWideChar(CP_UTF8, 0, ps->speed, -1, ws, 32);
                _snwprintf(g_jobs[idx]->info, 128, L"%s hız", ws);
                g_jobs[idx]->info[127] = 0;
            }
            *lastPost = now;
            LeaveCriticalSection(&g_cs);
            PostMessageW(g_hMain, WM_APP_JOB_UPDATE, (WPARAM)idx, 0);
        } else {
            LeaveCriticalSection(&g_cs);
        }
    }
}

/* --- remux isi --- */

/* bazi mp4'ler mkv'ya direkt gecmiyor (mov_text altyazi, timecode izleri...).
   once tam kopya dene, olmazsa sirayla: veri izlerini at, altyaziyi srt yap,
   en son care video+ses. video/ses hep kopya, encode yok. */
static void BuildCmd(const wchar_t *ff, const wchar_t *in, const wchar_t *out,
                     int attempt, wchar_t *cmd, size_t cch) {
    const wchar_t *mapopts;
    switch (attempt) {
    case 1:  mapopts = L"-map 0 -map -0:d -c copy"; break;
    case 2:  mapopts = L"-map 0 -map -0:d -c copy -c:s srt"; break;
    case 3:  mapopts = L"-map 0:v -map 0:a -map 0:t -c copy"; break;
    default: mapopts = L"-map 0 -c copy"; break;
    }
    /* hiz buradan: re-encode yok, probe kisa, thread auto, kuyruk buyuk.
       -nostdin sart (yoksa arka planda takiliyor), -y guvenli cunku
       cikis ismi onceden tekillestiriliyor. -progress ile canli % okunuyor */
    _snwprintf(cmd, cch,
        L"\"%s\" -hide_banner -loglevel warning -nostdin -y "
        L"-fflags +genpts+discardcorrupt -probesize 5M -analyzeduration 5M -threads auto "
        L"-i \"%s\" %s -copytb 1 -max_muxing_queue_size 9999 "
        L"-progress pipe:1 -nostats \"%s\"",
        ff, in, mapopts, out);
    cmd[cch - 1] = 0;
}

static const wchar_t *AttemptNote(int attempt) {
    switch (attempt) {
    case 1:  return L"veri izleri atıldı";
    case 2:  return L"altyazı srt'ye çevrildi";
    case 3:  return L"sadece video+ses taşındı";
    default: return L"";
    }
}

// tek pas: 0 ok, -1 iptal, -2 acilamadi, >0 ffmpeg cikis kodu
static int RunPass(int idx, const wchar_t *ff, const wchar_t *out, int attempt,
                   ProgState *ps, Job *j) {
    wchar_t *cmd;
    SECURITY_ATTRIBUTES sa;
    HANDLE hR = NULL, hW = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    char buf[8192], line[8192];
    int llen = 0;
    DWORD n;
    ULONGLONG lastPost = 0;
    BOOL killed = FALSE;
    DWORD code = 1;

    cmd = (wchar_t *)malloc(140000 * sizeof(wchar_t));
    if (!cmd) return -2;
    BuildCmd(ff, j->in, out, attempt, cmd, 140000);

    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    if (!CreatePipe(&hR, &hW, &sa, 65536)) { free(cmd); return -2; }
    SetHandleInformation(hR, HANDLE_FLAG_INHERIT, 0);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hW; si.hStdError = hW; si.hStdInput = NULL;
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        DWORD e = GetLastError();
        EnterCriticalSection(&g_cs);
        swprintf(j->info, 128, L"ffmpeg başlatılamadı (hata %lu)", e);
        LeaveCriticalSection(&g_cs);
        CloseHandle(hW); CloseHandle(hR); free(cmd);
        return -2;
    }
    free(cmd);
    CloseHandle(hW); hW = NULL;

    /* ffmpeg'i one cikar, GUI'yi normalde tut: remux I/O + mux agirlikli */
    SetPriorityClass(pi.hProcess, ABOVE_NORMAL_PRIORITY_CLASS);

    EnterCriticalSection(&g_cs);
    g_procs[idx] = pi.hProcess;
    LeaveCriticalSection(&g_cs);

    memset(ps, 0, sizeof(*ps));

    for (;;) {
        BOOL r = ReadFile(hR, buf, sizeof(buf) - 1, &n, NULL);
        if (g_stop && !killed) {
            killed = TRUE;
            TerminateProcess(pi.hProcess, 130);
        }
        if (!r || n == 0) break;
        {
            DWORD k;
            buf[n] = 0;
            for (k = 0; k < n; k++) {
                char c = buf[k];
                if (c == '\r' || c == '\n') {
                    if (llen > 0) { line[llen] = 0; ProcessLine(j, ps, line, idx, &lastPost); llen = 0; }
                } else if (llen < (int)sizeof(line) - 1) {
                    line[llen++] = c;
                }
            }
        }
    }
    if (llen > 0) { line[llen] = 0; ProcessLine(j, ps, line, idx, &lastPost); }

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(hR);
    EnterCriticalSection(&g_cs);
    g_procs[idx] = NULL;
    LeaveCriticalSection(&g_cs);

    if (g_stop || killed || code == 130) {
        DeleteFileW(out); /* yarim dosyayi temizle */
        return -1;
    }
    if (code != 0)
        DeleteFileW(out); /* basarisiz pasin bozuk ciktisini birakma */
    return (int)code;
}

static int RunFfmpeg(int idx) {
    Job *j = g_jobs[idx];
    FILETIME ftC, ftA, ftW;
    BOOL haveTime = FALSE;
    HANDLE hIn;
    wchar_t ff[32768], out[32768];
    ProgState ps;
    char keepErr[512];
    int attempt, rc = 1;

    keepErr[0] = 0;

    hIn = CreateFileW(j->in, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hIn != INVALID_HANDLE_VALUE) {
        if (GetFileTime(hIn, &ftC, &ftA, &ftW)) haveTime = TRUE;
        CloseHandle(hIn);
    }

    if (!FindFfmpeg(ff, 32768)) {
        EnterCriticalSection(&g_cs);
        wcscpy(j->info, L"ffmpeg.exe bulunamadı");
        LeaveCriticalSection(&g_cs);
        return 3;
    }

    EnterCriticalSection(&g_cs);
    BuildOutputPath(j->in, out, 32768);
    free(j->out); j->out = _wcsdup(out);
    if (!j->out) {
        wcscpy(j->info, L"bellek yok");
        LeaveCriticalSection(&g_cs);
        return 3;
    }
    LeaveCriticalSection(&g_cs);

    for (attempt = 0; attempt < 4 && !g_stop; attempt++) {
        if (attempt > 0) {
            EnterCriticalSection(&g_cs);
            j->pct = 0;
            swprintf(j->info, 128, L"uyumluluk denemesi %d/3...", attempt);
            LeaveCriticalSection(&g_cs);
            PostMessageW(g_hMain, WM_APP_JOB_UPDATE, (WPARAM)idx, 0);
        }
        rc = RunPass(idx, ff, out, attempt, &ps, j);
        if (rc == -1 || g_stop) { /* iptal */
            DeleteFileW(out);
            EnterCriticalSection(&g_cs);
            wcscpy(j->info, L"");
            LeaveCriticalSection(&g_cs);
            return -1;
        }
        if (rc == -2) return 3; /* baslatma hatasi RunPass'te yazildi, tekrar deneme */
        if (rc == 0) {
            if (haveTime) {
                HANDLE hOut = CreateFileW(out, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
                if (hOut != INVALID_HANDLE_VALUE) {
                    SetFileTime(hOut, &ftC, &ftA, &ftW); /* olusturma + degistirme tarihi korunur */
                    CloseHandle(hOut);
                }
            }
            EnterCriticalSection(&g_cs);
            j->pct = 100;
            wcsncpy(j->info, AttemptNote(attempt), 127); j->info[127] = 0;
            LeaveCriticalSection(&g_cs);
            return 0;
        }
        if (ps.lastErr[0]) {
            strncpy(keepErr, ps.lastErr, sizeof(keepErr) - 1);
            keepErr[sizeof(keepErr) - 1] = 0;
        }
    }
    if (g_stop) return -1;
    EnterCriticalSection(&g_cs);
    if (keepErr[0]) {
        wchar_t w[120];
        MultiByteToWideChar(CP_UTF8, 0, keepErr, -1, w, 120);
        w[119] = 0;
        wcsncpy(j->info, w, 127); j->info[127] = 0;
    } else {
        swprintf(j->info, 128, L"ffmpeg çıkış kodu %d", rc);
    }
    LeaveCriticalSection(&g_cs);
    return rc ? rc : 1;
}

/* XXH3 dogrulama: kaynakla ciktiyi ham kareye acip karsilastir.
   iki derste ogrenildi, unutma:
   1) -vsync 0 sart. yoksa ffmpeg container'a gore kare kopyaliyor,
      sayilar tutmuyor (mkv tarafi +1 kare verdi, 1 saat gitti).
   2) sesi decode edip karsilastirma! mp3 gapless budamasi mp4 ile
      mkv'de farkli cikiyor. ses = paket yuku (-f data) + akan hash.
   boru dogrudan kare tamponuna okunuyor, ara kopya yok. */

typedef struct {
    int code;               /* 0=ok, -1=iptal, 1=hata */
    ULONGLONG frames;       /* toplam video karesi */
    int nastreams;          /* ses akis sayisi */
    wchar_t detail[192];
} VerifyResult;

typedef struct {
    int idx;                /* 0:v:N / 0:a:N icin N */
    int w, h;
    double fps;             /* 0 = bilinmiyor */
    char pix[32];
} VStream;

// ilerleme bildirimi. wnd yoksa sessiz (/check modu)
static void VProgress(HWND wnd, int idx, int pct, const wchar_t *txt) {
    if (!wnd || idx < 0 || !txt) return;
    EnterCriticalSection(&g_cs);
    if (idx < g_count && g_jobs[idx]) {
        g_jobs[idx]->pct = pct;
        wcsncpy(g_jobs[idx]->info, txt, 127);
        g_jobs[idx]->info[127] = 0;
    }
    LeaveCriticalSection(&g_cs);
    PostMessageW(wnd, WM_APP_JOB_UPDATE, (WPARAM)idx, 0);
}

static void VerifyLog(const wchar_t *src, const wchar_t *dst, const VerifyResult *vr, int rc) {
    const wchar_t *env = _wgetenv(L"HYDRA_VERIFYLOG");
    FILE *f;
    if (!env || !*env || !vr) return;
    f = _wfopen(env, L"a, ccs=UTF-8");
    if (!f) return;
    fwprintf(f, L"%s => %s : [%d] %s\n", src, dst, rc, vr->detail);
    fclose(f);
}

/* pix_fmt -> kare bayti. Donus 0 = bilinmiyor (ortak format filtresi gerekir).
   Ham boru hizalamasiz (align=1) yazildigi icin chroma ceil formul. */
static size_t FrameBytes(int w, int h, const char *pix) {
    long long y, c42, c22;
    if (!pix || w <= 0 || h <= 0 || w > 16384 || h > 16384) return 0;
    y = (long long)w * h;
    c42 = (long long)((w + 1) / 2) * ((h + 1) / 2);
    c22 = (long long)((w + 1) / 2) * h;
#define PX(s) (strcmp(pix, s) == 0)
    if (PX("yuv420p") || PX("yuvj420p") || PX("nv12") || PX("nv21")) return (size_t)(y + 2 * c42);
    if (PX("yuv422p") || PX("yuvj422p")) return (size_t)(y + 2 * c22);
    if (PX("yuv444p") || PX("yuvj444p") || PX("rgb24") || PX("bgr24")) return (size_t)(y * 3);
    if (PX("rgba") || PX("bgra") || PX("argb") || PX("abgr")) return (size_t)(y * 4);
    if (PX("yuv420p10le") || PX("yuv420p12le") || PX("yuv420p16le")) return (size_t)(2 * (y + 2 * c42));
    if (PX("yuv422p10le") || PX("yuv422p12le") || PX("yuv422p16le")) return (size_t)(2 * (y + 2 * c22));
    if (PX("yuv444p10le") || PX("yuv444p12le") || PX("yuv444p16le") ||
        PX("rgb48le") || PX("bgr48le")) return (size_t)(y * 6);
    if (PX("gray") || PX("monow") || PX("monob")) return (size_t)y;
    if (PX("gray10le") || PX("gray12le") || PX("gray16le")) return (size_t)(y * 2);
    return 0;
#undef PX
}

/* Tek Stream satirini coz: "Stream #0:1(eng): Video: h264 ..., yuv420p(...), 1920x1080 ...,
   30 fps, ...". Donus: 1=video, 2=ses, 0=ilgilenmiyoruz. */
static int ParseStreamLine(const char *line, VStream *v, int *isAudio) {
    const char *p;
    *isAudio = 0;
    if (!strstr(line, "Stream #")) return 0;
    if ((p = strstr(line, ": Video:")) != NULL) {
        const char *d = line, *dimsAt = NULL;
        /* ", GENISxYUKSEK" ara (0x31637661 gibi hex'i w=0 ile ele) */
        while ((d = strchr(d, ',')) != NULL) {
            int w = 0, h = 0;
            if (sscanf(d + 1, " %d%*1[xX]%d", &w, &h) == 2 &&
                w > 0 && h > 0 && w <= 16384 && h <= 16384) { dimsAt = d; break; }
            d++;
        }
        if (!dimsAt) return 0;
        sscanf(dimsAt + 1, " %dx%d", &v->w, &v->h);
        /* pix: boyutlardan onceki son virgullu jeton */
        {
            const char *tok = dimsAt;
            while (tok > line && *(tok - 1) != ',' && *(tok - 1) != ':') tok--;
            while (*tok == ' ') tok++;
            {
                size_t n = 0;
                while (tok[n] && tok[n] != '(' && tok[n] != ' ' && tok[n] != ',' && n < sizeof(v->pix) - 1) {
                    v->pix[n] = tok[n]; n++;
                }
                v->pix[n] = 0;
            }
        }
        /* fps: ", 29.97 fps" */
        v->fps = 0;
        {
            const char *f = strstr(dimsAt, " fps");
            if (f) {
                const char *tok = f;
                while (tok > line && *(tok - 1) != ',') tok--;
                sscanf(tok, " %lf", &v->fps);
                if (v->fps < 0 || v->fps > 1000) v->fps = 0;
            }
        }
        return 1;
    }
    if (strstr(line, ": Audio:") != NULL) { *isAudio = 1; return 2; }
    return 0;
}

/* Akis kesfi: ciktisiz "ffmpeg -i" stderr parse. Paket okuma yok, aninda biter.
   Ilk denemede akis bulunamazsa tam demux'lu (-f null) yedek deneme yapilir. */
static int ProbeStreams(const wchar_t *ff, const wchar_t *path,
                        VStream *vs, int vmax, int *nv, int *na, double *durSec) {
    int pass;
    *nv = 0; *na = 0; *durSec = 0;
    for (pass = 0; pass < 2; pass++) {
        wchar_t *cmd = (wchar_t *)malloc(70000 * sizeof(wchar_t));
        SECURITY_ATTRIBUTES sa;
        HANDLE hR = NULL, hW = NULL;
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        char buf[4096], line[2048];
        int llen = 0;
        DWORD n;
        if (!cmd) return -1;
        if (pass == 0)
            _snwprintf(cmd, 70000, L"\"%s\" -hide_banner -nostdin -i \"%s\"", ff, path);
        else
            _snwprintf(cmd, 70000, L"\"%s\" -hide_banner -loglevel info -nostdin -i \"%s\" -map 0 -c copy -f null -", ff, path);
        cmd[70000 - 1] = 0;
        memset(&sa, 0, sizeof(sa));
        sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
        if (!CreatePipe(&hR, &hW, &sa, 65536)) { free(cmd); return -1; }
        SetHandleInformation(hR, HANDLE_FLAG_INHERIT, 0);
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = hW; si.hStdError = hW; si.hStdInput = NULL;
        memset(&pi, 0, sizeof(pi));
        if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            CloseHandle(hW); CloseHandle(hR); free(cmd);
            return -1;
        }
        free(cmd);
        CloseHandle(hW);
        for (;;) {
            BOOL r = ReadFile(hR, buf, sizeof(buf) - 1, &n, NULL);
            if (!r || n == 0) break;
            {
                DWORD k;
                buf[n] = 0;
                for (k = 0; k < n; k++) {
                    char c = buf[k];
                    if (c == '\r' || c == '\n') {
                        if (llen > 0) {
                            int isA = 0;
                            VStream v;
                            line[llen] = 0;
                            if (strstr(line, "Duration:")) {
                                int hh = 0, mm = 0; double ss = 0;
                                const char *dp = strstr(line, "Duration:");
                                if (sscanf(dp + 9, " %d:%d:%lf", &hh, &mm, &ss) == 3)
                                    *durSec = hh * 3600.0 + mm * 60.0 + ss;
                            } else {
                                memset(&v, 0, sizeof(v));
                                if (ParseStreamLine(line, &v, &isA) == 1) {
                                    if (*nv < vmax) { v.idx = *nv; vs[*nv] = v; (*nv)++; }
                                } else if (isA) {
                                    (*na)++;
                                }
                            }
                            llen = 0;
                        }
                    } else if (llen < (int)sizeof(line) - 1) {
                        line[llen++] = c;
                    }
                }
            }
        }
        CloseHandle(hR);
        WaitForSingleObject(pi.hProcess, 8000);
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (*nv > 0 || *na > 0 || pass == 1) break;
    }
    return 0;
}

static void BuildDecCmd(const wchar_t *ff, const wchar_t *path, BOOL video, int sidx,
                        BOOL force420, wchar_t *cmd, size_t cch) {
    if (video) {
        /* -vsync 0 SART: varsayilan CFR esitleme zaman damgasina gore kare
           kopyalayip duserir; container degisince (mp4->mkv) sayi farkli cikar.
           Passthrough ile decode edilen her kare aynen bir kez gecer. */
        if (force420)
            _snwprintf(cmd, cch, L"\"%s\" -hide_banner -loglevel error -nostdin -threads auto -i \"%s\" -map 0:v:%d -vf format=yuv420p -vsync 0 -f rawvideo pipe:1",
                       ff, path, sidx);
        else
            _snwprintf(cmd, cch, L"\"%s\" -hide_banner -loglevel error -nostdin -threads auto -i \"%s\" -map 0:v:%d -vsync 0 -f rawvideo pipe:1",
                       ff, path, sidx);
    } else {
        /* Ses: decode ETME, paket yukunu kopyala (-f data). Gerekce: kayipli
           codec'lerde (mp3/aac) container gapless budamasi (skip samples) mp4
           ile mkv'de farkli uygulanir; decode karsilastirma sahte hata verir.
           -c copy remux'ta paket yukleri birebir korunur, akan XXH3 + uzunluk
           hem hizli hem kesindir. */
        _snwprintf(cmd, cch, L"\"%s\" -hide_banner -loglevel error -nostdin -threads auto -i \"%s\" -map 0:a:%d -c:a copy -f data pipe:1",
                   ff, path, sidx);
    }
    cmd[cch - 1] = 0;
}

static HANDLE OpenNulWrite(void) {
    return CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
}

typedef struct {
    HANDLE hPipe;               /* decoder stdout (okuma ucu) */
    size_t unit;                /* video: kare bayti, ses: 1 MiB */
    BOOL video;
    XXH64_hash_t *hashes;       /* video: kare hash dizisi */
    size_t count, cap;
    XXH64_hash_t digest;        /* ses: akan tek hash */
    ULONGLONG units;            /* ilerleme sayaci */
    ULONGLONG bytes;            /* ses: toplam bayt */
    int error;                  /* 0=ok, 1=okuma/bozuk, -1=iptal */
} SideJob;

static int HashAppend(SideJob *s, XXH64_hash_t h) {
    if (s->count >= s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 4096;
        XXH64_hash_t *p = (XXH64_hash_t *)realloc(s->hashes, ncap * sizeof(*p));
        if (!p) return -1;
        s->hashes = p; s->cap = ncap;
    }
    s->hashes[s->count++] = h;
    return 0;
}

// boruyu direkt kare tamponuna oku, hash'i oradan al. kopya yok.
static DWORD WINAPI SideThread(LPVOID p) {
    SideJob *s = (SideJob *)p;
    unsigned char *fbuf = (unsigned char *)malloc(s->unit);
    if (!fbuf) { s->error = 1; return 0; }
    if (s->video) {
        for (;;) {
            size_t off = 0;
            if (g_stop) { s->error = -1; break; }
            while (off < s->unit) {
                DWORD want = (DWORD)((s->unit - off) > 65536 ? 65536 : (s->unit - off));
                DWORD got = 0;
                if (!ReadFile(s->hPipe, fbuf + off, want, &got, NULL) || got == 0) break;
                off += got;
            }
            if (off == 0) break;              /* temiz EOF */
            if (off < s->unit) { s->error = 1; break; }  /* yarim kare */
            if (HashAppend(s, XXH3_64bits(fbuf, s->unit)) != 0) { s->error = 1; break; }
            s->units++;
        }
    } else {
        XXH3_state_t *st = XXH3_createState();
        if (!st) { s->error = 1; }
        else {
            XXH3_64bits_reset(st);
            for (;;) {
                DWORD got = 0;
                if (g_stop) { s->error = -1; break; }
                if (!ReadFile(s->hPipe, fbuf, (DWORD)s->unit, &got, NULL) || got == 0) break;
                XXH3_64bits_update(st, fbuf, got);
                s->bytes += got;
                s->units++;
            }
            if (s->error == 0) s->digest = XXH3_64bits_digest(st);
            XXH3_freeState(st);
        }
    }
    free(fbuf);
    return 0;
}

// tek akis, iki taraf: 0 ok, -1 iptal, 1 fark
static int VerifyStream(const wchar_t *ff, const wchar_t *src, const wchar_t *dst,
                        BOOL video, int sidx, size_t unit, BOOL force420,
                        ULONGLONG est, HWND vwnd, int vidx,
                        const wchar_t *label, ULONGLONG *outUnits, wchar_t *failDetail) {
    wchar_t *cmd;
    SideJob sj[2];
    HANDLE ht[2] = { NULL, NULL }, hp[2] = { NULL, NULL };
    const wchar_t *paths[2];
    int i;
    ULONGLONG tick0 = GetTickCount64();
    ULONGLONG lastPost = 0;

    cmd = (wchar_t *)malloc(70000 * sizeof(wchar_t));
    if (!cmd) { swprintf(failDetail, 128, L"%s: bellek yok", label); return 1; }
    memset(sj, 0, sizeof(sj));
    paths[0] = src; paths[1] = dst;

    for (i = 0; i < 2; i++) {
        SECURITY_ATTRIBUTES sa;
        HANDLE hR = NULL, hW = NULL, hErr = NULL, hDiscard = NULL;
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        BuildDecCmd(ff, paths[i], video, sidx, force420, cmd, 70000);
        memset(&sa, 0, sizeof(sa));
        sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
        if (!CreatePipe(&hR, &hW, &sa, 1 << 20)) {
            swprintf(failDetail, 128, L"%s: boru açılamadı", label);
            free(cmd); return 1;
        }
        SetHandleInformation(hR, HANDLE_FLAG_INHERIT, 0);
        hErr = OpenNulWrite();
        if (hErr == INVALID_HANDLE_VALUE) {
            /* yedek: yazilip kapatilacak boru (loglevel error ciktisi minik) */
            if (CreatePipe(&hDiscard, &hErr, &sa, 65536)) {
                SetHandleInformation(hDiscard, HANDLE_FLAG_INHERIT, 0);
            } else {
                hErr = NULL;
            }
        }
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = hW; si.hStdError = hErr; si.hStdInput = NULL;
        memset(&pi, 0, sizeof(pi));
        if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            int k;
            if (hErr && hErr != INVALID_HANDLE_VALUE) CloseHandle(hErr);
            if (hDiscard) CloseHandle(hDiscard);
            CloseHandle(hW); CloseHandle(hR);
            /* onceki turun decoder'ini yetim birakma: oldur + kapat */
            for (k = 0; k < i; k++) {
                if (hp[k]) { TerminateProcess(hp[k], 130); CloseHandle(hp[k]); hp[k] = NULL; }
                if (sj[k].hPipe) { CloseHandle(sj[k].hPipe); sj[k].hPipe = NULL; }
            }
            swprintf(failDetail, 128, L"%s: decoder başlatılamadı", label);
            free(cmd); return 1;
        }
        CloseHandle(hW);
        if (hErr && hErr != INVALID_HANDLE_VALUE) CloseHandle(hErr);
        if (hDiscard) CloseHandle(hDiscard); /* cocuk yazarsa sessizce duser */
        CloseHandle(pi.hThread);
        hp[i] = pi.hProcess;
        sj[i].hPipe = hR;
        sj[i].unit = unit;
        sj[i].video = video;
    }
    free(cmd);

    for (i = 0; i < 2; i++) {
        ht[i] = CreateThread(NULL, 0, SideThread, &sj[i], 0, NULL);
        if (!ht[i]) {
            int k;
            swprintf(failDetail, 128, L"%s: okuyucu açılamadı", label);
            TerminateProcess(hp[0], 130); TerminateProcess(hp[1], 130);
            for (k = 0; k < 2; k++) {
                if (ht[k]) { WaitForSingleObject(ht[k], 5000); CloseHandle(ht[k]); }
                if (hp[k]) CloseHandle(hp[k]);
                if (sj[k].hPipe) CloseHandle(sj[k].hPipe);
                free(sj[k].hashes);
            }
            return 1;
        }
    }

    /* bitmesini bekle, arada % pompla */
    for (;;) {
        BOOL d0 = (WaitForSingleObject(ht[0], 0) == WAIT_OBJECT_0);
        BOOL d1 = (WaitForSingleObject(ht[1], 0) == WAIT_OBJECT_0);
        ULONGLONG now = GetTickCount64();
        if (d0 && d1) break;
        if (g_stop) { TerminateProcess(hp[0], 130); TerminateProcess(hp[1], 130); }
        if (vwnd && now - lastPost > 150) {
            ULONGLONG u = (sj[0].units + sj[1].units) / 2;
            ULONGLONG ms = now - tick0;
            int fps = (ms > 0) ? (int)(u * 1000 / ms) : 0;
            wchar_t t[128];
            int pct = -1;
            if (video && est > 0) {
                pct = (int)(u * 100 / est);
                if (pct > 99) pct = 99;
                swprintf(t, 128, L"Doğrulanıyor %s %%%d • %llu kare • %d fps", label, pct, u, fps);
            } else if (video) {
                swprintf(t, 128, L"Doğrulanıyor %s • %llu kare • %d fps", label, u, fps);
            } else {
                swprintf(t, 128, L"Ses doğrulanıyor %s • %llu KB", label, (sj[0].bytes + sj[1].bytes) / 2048);
            }
            VProgress(vwnd, vidx, pct, t);
            lastPost = now;
        }
        Sleep(60);
    }
    WaitForSingleObject(ht[0], INFINITE);
    WaitForSingleObject(ht[1], INFINITE);
    CloseHandle(ht[0]); CloseHandle(ht[1]);
    CloseHandle(hp[0]); CloseHandle(hp[1]);
    CloseHandle(sj[0].hPipe); CloseHandle(sj[1].hPipe);

    if (g_stop || sj[0].error == -1 || sj[1].error == -1) {
        free(sj[0].hashes); free(sj[1].hashes);
        return -1;
    }
    if (sj[0].error != 0) {
        swprintf(failDetail, 128, L"%s: kaynak okunamadı", label);
        free(sj[0].hashes); free(sj[1].hashes);
        return 1;
    }
    if (sj[1].error != 0) {
        swprintf(failDetail, 128, L"%s: çıktı okunamadı", label);
        free(sj[0].hashes); free(sj[1].hashes);
        return 1;
    }
    if (video) {
        int r = 0;
        if (sj[0].count != sj[1].count) {
            swprintf(failDetail, 128, L"%s: kare sayısı farklı (%llu/%llu)", label, sj[0].count, sj[1].count);
            r = 1;
        } else {
            size_t k;
            for (k = 0; k < sj[0].count; k++) {
                if (sj[0].hashes[k] != sj[1].hashes[k]) {
                    swprintf(failDetail, 128, L"DOĞRULAMA HATASI %s @kare %llu", label, (ULONGLONG)k);
                    r = 1;
                    break;
                }
            }
        }
        if (r == 0 && outUnits) *outUnits = sj[0].count;
        free(sj[0].hashes); free(sj[1].hashes);
        return r;
    }
    {
        int r = 0;
        if (sj[0].bytes != sj[1].bytes) {
            swprintf(failDetail, 128, L"%s: ses uzunluğu farklı", label);
            r = 1;
        } else if (sj[0].digest != sj[1].digest) {
            swprintf(failDetail, 128, L"DOĞRULAMA HATASI %s içerik farklı", label);
            r = 1;
        }
        free(sj[0].hashes); free(sj[1].hashes);
        return r;
    }
}

// kaynak<->cikti karsilastirma: 0 ok, -1 iptal, 1 hata
static int VerifyJobPaths(const wchar_t *ff, const wchar_t *src, const wchar_t *dst,
                          HWND vwnd, int vidx, VerifyResult *vr) {
    VStream vs[16], vd[16];
    int nv = 0, na = 0, mnv = 0, mna = 0, i;
    double durS = 0, mdurS = 0;
    ULONGLONG vframes = 0;
    wchar_t fail[128];

    memset(vr, 0, sizeof(*vr));
    vr->code = 1;
    if (ProbeStreams(ff, src, vs, 16, &nv, &na, &durS) != 0 || (nv == 0 && na == 0)) {
        wcscpy(vr->detail, L"akış bilgisi okunamadı");
        return 1;
    }
    if (ProbeStreams(ff, dst, vd, 16, &mnv, &mna, &mdurS) != 0) {
        wcscpy(vr->detail, L"çıktı akış bilgisi okunamadı");
        return 1;
    }
    if (nv != mnv) {
        swprintf(vr->detail, 192, L"video akış sayısı farklı (kaynak %d, çıktı %d)", nv, mnv);
        return 1;
    }
    if (na != mna) {
        swprintf(vr->detail, 192, L"ses akış sayısı farklı (kaynak %d, çıktı %d)", na, mna);
        return 1;
    }

    for (i = 0; i < nv && !g_stop; i++) {
        size_t us = FrameBytes(vs[i].w, vs[i].h, vs[i].pix);
        size_t ud = FrameBytes(vd[i].w, vd[i].h, vd[i].pix);
        BOOL force420 = FALSE;
        ULONGLONG est = 0;
        wchar_t label[32];
        int r;
        if (vs[i].w != vd[i].w || vs[i].h != vd[i].h) {
            swprintf(vr->detail, 192, L"video#%d boyut farklı (%dx%d / %dx%d)",
                     i, vs[i].w, vs[i].h, vd[i].w, vd[i].h);
            return 1;
        }
        if (us == 0 || ud == 0 || us != ud) {
            /* bilinmeyen pix_fmt: iki tarafa da ayni yuv420p filtresi */
            us = FrameBytes(vs[i].w, vs[i].h, "yuv420p");
            force420 = TRUE;
        }
        if (durS > 0 && vs[i].fps > 0) est = (ULONGLONG)(durS * vs[i].fps);
        swprintf(label, 32, L"video#%d", i);
        {
            ULONGLONG u = 0;
            r = VerifyStream(ff, src, dst, TRUE, i, us, force420, est, vwnd, vidx, label, &u, fail);
            vframes += u;
        }
        if (r == -1 || g_stop) { vr->code = -1; wcscpy(vr->detail, L""); return -1; }
        if (r != 0) { wcsncpy(vr->detail, fail, 191); vr->detail[191] = 0; return 1; }
    }
    for (i = 0; i < na && !g_stop; i++) {
        wchar_t label[32];
        int r;
        swprintf(label, 32, L"ses#%d", i);
        r = VerifyStream(ff, src, dst, FALSE, i, 1 << 20, FALSE, 0, vwnd, vidx, label, NULL, fail);
        if (r == -1 || g_stop) { vr->code = -1; wcscpy(vr->detail, L""); return -1; }
        if (r != 0) { wcsncpy(vr->detail, fail, 191); vr->detail[191] = 0; return 1; }
    }

    vr->code = 0;
    vr->frames = vframes;
    vr->nastreams = na;
    if (nv > 0 && na > 0)
        swprintf(vr->detail, 192, L"doğrulandı: %llu kare + ses(%d) XXH3 OK", vframes, na);
    else if (nv > 0)
        swprintf(vr->detail, 192, L"doğrulandı: %llu kare XXH3 OK", vframes);
    else
        swprintf(vr->detail, 192, L"doğrulandı: ses(%d) XXH3 OK", na);
    return 0;
}

static DWORD WINAPI WorkerThread(LPVOID p) {
    int idx = (int)(INT_PTR)p;
    int rc = RunFfmpeg(idx);
    if (rc == 0 && g_doVerify && !g_stop) {
        wchar_t ff[32768], prefix[128], finalt[192];
        VerifyResult vr;
        int vrc;
        EnterCriticalSection(&g_cs);
        wcsncpy(prefix, g_jobs[idx]->info, 127); prefix[127] = 0;
        g_jobs[idx]->pct = 0;
        wcscpy(g_jobs[idx]->info, L"doğrulanıyor...");
        LeaveCriticalSection(&g_cs);
        PostMessageW(g_hMain, WM_APP_JOB_UPDATE, (WPARAM)idx, 0);
        if (!FindFfmpeg(ff, 32768)) {
            EnterCriticalSection(&g_cs);
            g_jobs[idx]->st = JS_ERROR;
            wcscpy(g_jobs[idx]->info, L"ffmpeg.exe bulunamadı");
            LeaveCriticalSection(&g_cs);
            PostMessageW(g_hMain, WM_APP_JOB_UPDATE, (WPARAM)idx, 0);
            InterlockedDecrement(&g_active);
            return 0;
        }
        vrc = VerifyJobPaths(ff, g_jobs[idx]->in, g_jobs[idx]->out, g_hMain, idx, &vr);
        VerifyLog(g_jobs[idx]->in, g_jobs[idx]->out, &vr, vrc);
        EnterCriticalSection(&g_cs);
        if (vrc == 0) {
            g_jobs[idx]->st = JS_DONE;
            g_jobs[idx]->pct = 100;
            if (prefix[0])
                _snwprintf(finalt, 192, L"%s • %s", prefix, vr.detail);
            else
                wcsncpy(finalt, vr.detail, 191);
            finalt[191] = 0;
            wcsncpy(g_jobs[idx]->info, finalt, 127);
            g_jobs[idx]->info[127] = 0;
        } else if (vrc == -1) {
            g_jobs[idx]->st = JS_CANCELLED;
            wcscpy(g_jobs[idx]->info, L"");
        } else {
            g_jobs[idx]->st = JS_ERROR;
            wcsncpy(g_jobs[idx]->info, vr.detail, 127);
            g_jobs[idx]->info[127] = 0;
        }
        LeaveCriticalSection(&g_cs);
        PostMessageW(g_hMain, WM_APP_JOB_UPDATE, (WPARAM)idx, 0);
        InterlockedDecrement(&g_active);
        return 0;
    }
    EnterCriticalSection(&g_cs);
    if (rc == 0) g_jobs[idx]->st = JS_DONE;
    else if (rc == -1) g_jobs[idx]->st = JS_CANCELLED;
    else g_jobs[idx]->st = JS_ERROR;
    LeaveCriticalSection(&g_cs);
    PostMessageW(g_hMain, WM_APP_JOB_UPDATE, (WPARAM)idx, 0);
    InterlockedDecrement(&g_active);
    return 0;
}

static DWORD WINAPI ManagerThread(LPVOID unused) {
    HANDLE h;
    (void)unused;
    g_startTick = GetTickCount64();
    for (;;) {
        int idx = -1, i;
        if (g_stop) break;
        EnterCriticalSection(&g_cs);
        for (i = 0; i < g_count; i++)
            if (g_jobs[i]->st == JS_QUEUED) { idx = i; break; }
        LeaveCriticalSection(&g_cs);
        if (idx < 0) {
            if (InterlockedCompareExchange(&g_active, 0, 0) == 0) break; /* kuyruk bitti */
            Sleep(100);
            continue;
        }
        while (!g_stop && InterlockedCompareExchange(&g_active, 0, 0) >= (LONG)g_maxPar)
            Sleep(50);
        if (g_stop) break;
        EnterCriticalSection(&g_cs);
        g_jobs[idx]->st = JS_RUNNING;
        g_jobs[idx]->pct = 0;
        wcscpy(g_jobs[idx]->info, L"başlıyor...");
        LeaveCriticalSection(&g_cs);
        PostMessageW(g_hMain, WM_APP_JOB_UPDATE, (WPARAM)idx, 0);
        InterlockedIncrement(&g_active);
        h = CreateThread(NULL, 0, WorkerThread, (LPVOID)(INT_PTR)idx, 0, NULL);
        if (h) CloseHandle(h);
        else {
            EnterCriticalSection(&g_cs);
            g_jobs[idx]->st = JS_ERROR;
            wcscpy(g_jobs[idx]->info, L"iş parçacığı açılamadı");
            LeaveCriticalSection(&g_cs);
            InterlockedDecrement(&g_active);
            PostMessageW(g_hMain, WM_APP_JOB_UPDATE, (WPARAM)idx, 0);
        }
    }
    while (InterlockedCompareExchange(&g_active, 0, 0) > 0) Sleep(50);
    PostMessageW(g_hMain, WM_APP_ALL_DONE, 0, 0);
    return 0;
}

/* is akisi: baslat/durdur/temizle + dosya ekleme diyaloğu */

static void StartJobs(void) {
    HANDLE h;
    int i, q = 0;
    wchar_t ff[32768];
    if (g_busy || g_count == 0) return;
    if (!FindFfmpeg(ff, 32768)) {
        MessageBoxW(g_hMain, L"ffmpeg.exe bulunamadı.\n\nffmpeg.exe'yi programın yanına koyun veya PATH'e ekleyin.",
                    L"Hydra Remuxer", MB_ICONERROR | MB_OK);
        return;
    }
    for (i = 0; i < g_count; i++)
        if (g_jobs[i]->st == JS_QUEUED) q++;
    if (q == 0) { /* onceki calismanin hata/iptallerini tekrar kuyruga al */
        for (i = 0; i < g_count; i++)
            if (g_jobs[i]->st == JS_ERROR || g_jobs[i]->st == JS_CANCELLED) {
                g_jobs[i]->st = JS_QUEUED; g_jobs[i]->pct = -1;
                wcscpy(g_jobs[i]->info, L"");
                UpdateRow(i); q++;
            }
    }
    if (q == 0) {
        MessageBoxW(g_hMain, L"Tüm işler zaten tamamlanmış.", L"Hydra Remuxer", MB_ICONINFORMATION | MB_OK);
        return;
    }
    {
        BOOL ok = FALSE;
        int v = GetDlgItemInt(g_hMain, IDC_SPIN_EDIT, &ok, FALSE);
        if (ok && v >= 1 && v <= 16) g_maxPar = v;
    }
    g_doVerify = (SendMessageW(g_hChkVerify, BM_GETCHECK, 0, 0) == BST_CHECKED);
    g_stop = FALSE; g_busy = TRUE;
    EnableWindow(g_hBtnStart, FALSE);
    EnableWindow(g_hBtnClear, FALSE);
    EnableWindow(g_hBtnStop, TRUE);
    EnableWindow(g_hChkVerify, FALSE);
    TaskSet(TBPF_NORMAL, 0, 1);
    h = CreateThread(NULL, 0, ManagerThread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

static void StopJobs(void) {
    int i;
    if (!g_busy) return;
    g_stop = TRUE;
    TaskSet(TBPF_PAUSED, 0, 0);
    EnterCriticalSection(&g_cs);
    for (i = 0; i < g_count; i++)
        if (g_procs[i]) TerminateProcess(g_procs[i], 130);
    LeaveCriticalSection(&g_cs);
    EnableWindow(g_hBtnStop, FALSE);
}

static void ClearJobs(void) {
    int i;
    if (g_busy) return;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < g_count; i++) {
        free(g_jobs[i]->in); free(g_jobs[i]->out); free(g_jobs[i]->name);
        free(g_jobs[i]); g_jobs[i] = NULL; g_procs[i] = NULL;
    }
    g_count = 0;
    LeaveCriticalSection(&g_cs);
    ListView_DeleteAllItems(g_hList);
    RefreshTotals();
}

static void AddFilesDialog(void) {
    wchar_t *buf = (wchar_t *)malloc(32768 * sizeof(wchar_t));
    OPENFILENAMEW ofn;
    if (!buf) return;
    memset(buf, 0, 32768 * sizeof(wchar_t));
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMain;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 32768;
    ofn.lpstrFilter = L"Tüm medya dosyaları\0*.mp4;*.m4v;*.mov;*.avi;*.mkv;*.ts;*.m2ts;*.mts;*.flv;*.wmv;*.asf;*.webm;*.mpg;*.mpeg;*.vob;*.3gp;*.ogv;*.mp3;*.flac;*.mka\0Tüm dosyalar\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrTitle = L"Dönüştürülecek dosyaları seç";
    if (GetOpenFileNameW(&ofn)) {
        if (buf[wcslen(buf) + 1] == 0) {
            AddJob(buf); /* tek secim: tam yol */
        } else {
            const wchar_t *dir = buf, *f = buf + wcslen(buf) + 1;
            while (*f) {
                wchar_t full[32768];
                _snwprintf(full, 32768, L"%s\\%s", dir, f); full[32767] = 0;
                AddJob(full);
                f += wcslen(f) + 1;
            }
        }
    }
    free(buf);
}

// pencere yerlesimi, elle hizalanmis kontroller

// durum -> satir ikonu (0 bekliyor, 1 calisiyor, 2 bitti, 3 hata, 4 iptal)
static int StatusIcon(JobStatus s) {
    switch (s) {
    case JS_RUNNING:   return 1;
    case JS_DONE:      return 2;
    case JS_ERROR:     return 3;
    case JS_CANCELLED: return 4;
    default:           return 0;
    }
}

// 16x16 ikon: renkli daire + beyaz sembol. seffaflik icin alpha elle islenir
// (GDI ciziyor, alpha'ya dokunmuyor; sonradan bos olmayan piksele 255 yazilir).
static HBITMAP MakeStatusBitmap(COLORREF fill, int glyph) {
    BITMAPINFO bi;
    void *bits = NULL;
    HDC hdc, m;
    HBITMAP hb, old;
    HPEN pen, oldPen;
    HBRUSH br, oldBr;
    DWORD *px;
    int n;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 16;
    bi.bmiHeader.biHeight = -16;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    hdc = GetDC(NULL);
    if (!hdc) return NULL;
    hb = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!hb || !bits) {
        if (hb) DeleteObject(hb);
        ReleaseDC(NULL, hdc);
        return NULL;
    }
    m = CreateCompatibleDC(hdc);
    old = (HBITMAP)SelectObject(m, hb);
    br = CreateSolidBrush(fill);
    pen = CreatePen(PS_SOLID, 1, fill);
    oldBr = (HBRUSH)SelectObject(m, br);
    oldPen = (HPEN)SelectObject(m, pen);
    Ellipse(m, 1, 1, 15, 15);
    SelectObject(m, oldBr); SelectObject(m, oldPen);
    DeleteObject(br); DeleteObject(pen);
    if (glyph == 1) { /* oynat ucgneti */
        POINT pt[3] = { { 6, 4 }, { 6, 12 }, { 12, 8 } };
        br = CreateSolidBrush(RGB(255, 255, 255));
        pen = (HPEN)GetStockObject(NULL_PEN);
        oldBr = (HBRUSH)SelectObject(m, br);
        oldPen = (HPEN)SelectObject(m, pen);
        Polygon(m, pt, 3);
        SelectObject(m, oldBr); SelectObject(m, oldPen);
        DeleteObject(br);
    } else if (glyph == 2) { /* tik */
        pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        oldPen = (HPEN)SelectObject(m, pen);
        MoveToEx(m, 4, 8, NULL); LineTo(m, 7, 11); LineTo(m, 12, 5);
        SelectObject(m, oldPen); DeleteObject(pen);
    } else if (glyph == 3) { /* carpi */
        pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        oldPen = (HPEN)SelectObject(m, pen);
        MoveToEx(m, 5, 5, NULL); LineTo(m, 11, 11);
        MoveToEx(m, 11, 5, NULL); LineTo(m, 5, 11);
        SelectObject(m, oldPen); DeleteObject(pen);
    } else if (glyph == 4) { /* cizgi */
        pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        oldPen = (HPEN)SelectObject(m, pen);
        MoveToEx(m, 5, 8, NULL); LineTo(m, 11, 8);
        SelectObject(m, oldPen); DeleteObject(pen);
    } else { /* soru isareti */
        HFONT hf = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HFONT of = (HFONT)SelectObject(m, hf);
        RECT r = { 0, 0, 16, 16 };
        SetTextColor(m, RGB(255, 255, 255));
        SetBkMode(m, TRANSPARENT);
        DrawTextW(m, L"?", 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(m, of);
    }
    SelectObject(m, old);
    DeleteDC(m);
    ReleaseDC(NULL, hdc);
    px = (DWORD *)bits;
    for (n = 0; n < 256; n++)
        if (px[n] & 0x00FFFFFF) px[n] |= 0xFF000000;
    return hb;
}

static HIMAGELIST CreateStatusIcons(void) {
    static const COLORREF fills[5] = {
        RGB(158, 158, 158), RGB(33, 150, 243), RGB(76, 175, 80),
        RGB(244, 67, 54), RGB(255, 152, 0)
    };
    HIMAGELIST him;
    int k;
    him = ImageList_Create(16, 16, ILC_COLOR32, 5, 0);
    if (!him) return NULL;
    for (k = 0; k < 5; k++) {
        HBITMAP hb = MakeStatusBitmap(fills[k], k);
        if (hb) { ImageList_Add(him, hb, NULL); DeleteObject(hb); }
    }
    return him;
}

// gorev cubugu ilerlemesi. state < 0 ise sadece deger guncellenir.
static void TaskSet(int state, ULONGLONG done, ULONGLONG total) {
    if (!g_pTask || !g_hMain) return;
    if (state >= 0)
        g_pTask->lpVtbl->SetProgressState(g_pTask, g_hMain, (TBPFLAG)state);
    if (total > 0)
        g_pTask->lpVtbl->SetProgressValue(g_pTask, g_hMain, done, total);
}

static void Layout(void) {
    RECT rc;
    int W, btnY = 8, listY = 44, progH = 24;
    if (!g_hMain) return;
    GetClientRect(g_hMain, &rc);
    W = rc.right - rc.left;
    {
        int x = 8;
        SetWindowPos(g_hBtnAdd, NULL, x, btnY, 110, 28, SWP_NOZORDER); x += 116;
        SetWindowPos(g_hBtnStart, NULL, x, btnY, 110, 28, SWP_NOZORDER); x += 116;
        SetWindowPos(g_hBtnStop, NULL, x, btnY, 110, 28, SWP_NOZORDER); x += 116;
        SetWindowPos(g_hBtnClear, NULL, x, btnY, 110, 28, SWP_NOZORDER); x += 126;
        SetWindowPos(g_hSpinEdit, NULL, x, btnY + 3, 44, 22, SWP_NOZORDER);
        SetWindowPos(g_hSpin, NULL, x + 44, btnY + 3, 18, 22, SWP_NOZORDER);
        SetWindowPos(g_hChkVerify, NULL, x + 70, btnY + 5, 170, 20, SWP_NOZORDER);
    }
    SetWindowPos(g_hList, NULL, 8, listY, W - 16, rc.bottom - listY - progH - 40, SWP_NOZORDER);
    {
        int lw = W - 16, lh = rc.bottom - listY - progH - 40, hw = 440, hh = 64;
        if (g_hHint) {
            if (hw > lw - 40) hw = lw - 40;
            if (hw < 100) hw = 100;
            SetWindowPos(g_hHint, NULL, 8 + (lw - hw) / 2, listY + (lh - hh) / 2,
                         hw, hh, SWP_NOZORDER);
        }
    }
    SetWindowPos(g_hProg, NULL, 8, rc.bottom - progH - 30, W - 16, 20, SWP_NOZORDER);
    {
        RECT src;
        GetClientRect(g_hMain, &src);
        SendMessageW(g_hStatus, WM_SIZE, 0, 0);
        (void)src;
    }
}

/* ----- WndProc: butun pencere olaylari burada ----- */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        INITCOMMONCONTROLSEX icc;
        LVCOLUMNW col;
        int parts[2];
        RECT rc;
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_UPDOWN_CLASS;
        InitCommonControlsEx(&icc);

        g_hFont = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              TURKISH_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        if (!g_hFont)
            g_hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        g_hBtnAdd = CreateWindowW(L"BUTTON", L"Dosya Ekle", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BTN_ADD, g_hInst, NULL);
        g_hBtnStart = CreateWindowW(L"BUTTON", L"Başlat", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BTN_START, g_hInst, NULL);
        g_hBtnStop = CreateWindowW(L"BUTTON", L"Durdur", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BTN_STOP, g_hInst, NULL);
        g_hBtnClear = CreateWindowW(L"BUTTON", L"Temizle", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BTN_CLEAR, g_hInst, NULL);
        EnableWindow(g_hBtnStop, FALSE);

        g_hSpinEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"4",
                                      WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_CENTER,
                                      0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SPIN_EDIT, g_hInst, NULL);
        g_hSpin = CreateWindowExW(0, UPDOWN_CLASSW, NULL,
                                  WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT |
                                  UDS_ARROWKEYS | UDS_NOTHOUSANDS,
                                  0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SPIN, g_hInst, NULL);
        SendMessageW(g_hSpin, UDM_SETBUDDY, (WPARAM)g_hSpinEdit, 0);
        SendMessageW(g_hSpin, UDM_SETRANGE, 0, MAKELPARAM(16, 1));
        SendMessageW(g_hSpin, UDM_SETPOS, 0, MAKELPARAM(CpuCount(), 0));

        g_hChkVerify = CreateWindowExW(0, L"BUTTON", L"XXH3 ile doğrula",
                                       WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                       0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_CHK_VERIFY, g_hInst, NULL);
        SendMessageW(g_hChkVerify, BM_SETCHECK, BST_CHECKED, 0);

        g_hImg = CreateStatusIcons();
        if (g_hImg && g_hList) ListView_SetImageList(g_hList, g_hImg, LVSIL_SMALL);

        g_hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                  WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
                                  0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_LIST, g_hInst, NULL);
        ListView_SetExtendedListViewStyle(g_hList,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);

        memset(&col, 0, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = L"Dosya"; col.cx = 340; col.iSubItem = 0;
        ListView_InsertColumn(g_hList, 0, &col);
        col.pszText = L"Boyut"; col.cx = 90; col.iSubItem = 1;
        ListView_InsertColumn(g_hList, 1, &col);
        col.pszText = L"Durum"; col.cx = 150; col.iSubItem = 2;
        ListView_InsertColumn(g_hList, 2, &col);
        col.pszText = L"Bilgi"; col.cx = 280; col.iSubItem = 3;
        ListView_InsertColumn(g_hList, 3, &col);

        g_hProg = CreateWindowExW(0, PROGRESS_CLASSW, NULL,
                                  WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                  0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_PROG, g_hInst, NULL);
        SendMessageW(g_hProg, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

        GetClientRect(hwnd, &rc);
        g_hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, NULL,
                                    WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_STATUS, g_hInst, NULL);
        parts[0] = 620; parts[1] = -1;
        SendMessageW(g_hStatus, SB_SETPARTS, 2, (LPARAM)parts);
        SendMessageW(g_hStatus, SB_SETTEXTW, 0, (LPARAM)L"Hazır — dosya sürükleyip bırakın");
        SendMessageW(g_hStatus, SB_SETTEXTW, 1, (LPARAM)L"hedef: MKV • -c copy");

        {
            HWND ch = GetWindow(hwnd, GW_CHILD);
            while (ch) { SendMessageW(ch, WM_SETFONT, (WPARAM)g_hFont, TRUE); ch = GetWindow(ch, GW_HWNDNEXT); }
        }
        g_hFontBig = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 TURKISH_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        if (!g_hFontBig) g_hFontBig = g_hFont;
        g_hHint = CreateWindowExW(0, L"STATIC",
                                  L"Videoları buraya sürükleyip bırakın\r\nveya \"Dosya Ekle\" düğmesine basın",
                                  WS_CHILD | SS_CENTER,
                                  0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_HINT, g_hInst, NULL);
        if (g_hHint) {
            SendMessageW(g_hHint, WM_SETFONT, (WPARAM)g_hFontBig, TRUE);
            ShowWindow(g_hHint, SW_SHOW);
        }
        DragAcceptFiles(hwnd, TRUE);
        Layout();
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        if ((HWND)lParam == g_hHint) {
            SetTextColor(hdc, RGB(110, 110, 110));
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)GetStockObject(HOLLOW_BRUSH);
        }
        break;
    }
    case WM_SIZE:
        Layout();
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *m = (MINMAXINFO *)lParam;
        m->ptMinTrackSize.x = 640; m->ptMinTrackSize.y = 400;
        return 0;
    }
    case WM_DROPFILES: {
        HDROP h = (HDROP)wParam;
        UINT n = DragQueryFileW(h, 0xFFFFFFFF, NULL, 0), i;
        for (i = 0; i < n; i++) {
            UINT len = DragQueryFileW(h, i, NULL, 0);
            wchar_t *p = (wchar_t *)malloc((len + 2) * sizeof(wchar_t));
            if (!p) continue;
            DragQueryFileW(h, i, p, len + 1);
            {
                DWORD a = GetFileAttributesW(p);
                if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY))
                    AddFolderDropped(p);
                else
                    AddJob(p);
            }
            free(p);
        }
        DragFinish(h);
        SetForegroundWindow(hwnd);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDC_BTN_ADD) AddFilesDialog();
        else if (id == IDC_BTN_START) StartJobs();
        else if (id == IDC_BTN_STOP) StopJobs();
        else if (id == IDC_BTN_CLEAR) ClearJobs();
        return 0;
    }
    case WM_APP_JOB_UPDATE:
        UpdateRow((int)wParam);
        RefreshTotals();
        return 0;
    case WM_APP_ALL_DONE: {
        int i, ok = 0, err = 0, canc = 0;
        double secs;
        wchar_t t[256];
        for (i = 0; i < g_count; i++) {
            if (g_jobs[i]->st == JS_DONE) ok++;
            else if (g_jobs[i]->st == JS_ERROR) err++;
            else if (g_jobs[i]->st == JS_CANCELLED) canc++;
        }
        g_busy = FALSE;
        EnableWindow(g_hBtnStart, TRUE);
        EnableWindow(g_hBtnClear, TRUE);
        EnableWindow(g_hBtnStop, FALSE);
        EnableWindow(g_hChkVerify, TRUE);
        TaskSet(err > 0 ? TBPF_ERROR : TBPF_NOPROGRESS, 0, 0);
        RefreshTotals();
        secs = (GetTickCount64() - g_startTick) / 1000.0;
        swprintf(t, 256, L"Bitti: %d başarılı, %d hata, %d iptal (%.1f sn)", ok, err, canc, secs);
        SendMessageW(g_hStatus, SB_SETTEXTW, 0, (LPARAM)t);
        return 0;
    }
    case WM_CLOSE:
        if (g_busy) {
            if (MessageBoxW(hwnd, L"İşlem devam ediyor. Çıkıp yarım dosyaları iptal edilsin mi?",
                            L"Hydra Remuxer", MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
            StopJobs();
            Sleep(400);
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_hImg) ImageList_Destroy(g_hImg);
        if (g_hFontBig && g_hFontBig != g_hFont) DeleteObject(g_hFontBig);
        if (g_pTask) { g_pTask->lpVtbl->Release(g_pTask); g_pTask = NULL; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// giris noktasi

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmd, int show) {
    WNDCLASSEXW wc;
    HWND hwnd;
    MSG m;
    int autoStart = 0;
    (void)hPrev; (void)cmd;

    g_hInst = hInst;
    InitializeCriticalSection(&g_cs);
    memset(g_procs, 0, sizeof(g_procs));
    /* gorev cubugu ilerlemesi (Win7+). olmazsa sessizce gecilir. */
    if (SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        if (FAILED(CoCreateInstance(&CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER,
                                    &IID_ITaskbarList3, (void **)&g_pTask)))
            g_pTask = NULL;
        else if (FAILED(g_pTask->lpVtbl->HrInit(g_pTask))) {
            g_pTask->lpVtbl->Release(g_pTask);
            g_pTask = NULL;
        }
    }

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"HydraRemuxerClass";
    wc.hIconSm = LoadIconW(NULL, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"Pencere sınıfı kaydedilemedi.", L"Hydra Remuxer", MB_ICONERROR | MB_OK);
        return 1;
    }

    hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, L"HydraRemuxerClass",
                           L"Hydra Remuxer — Win32 FFmpeg Remux (hedef: MKV)",
                           WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                           CW_USEDEFAULT, CW_USEDEFAULT, 920, 560,
                           NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;
    g_hMain = hwnd;

    // penceresiz mod: HydraRemuxer.exe /check kaynak cikti [/silent]
    // donus: 0 ok, 1 hata, 3 eksik arguman ya da ffmpeg yok
    {
        int argc = 0;
        LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            int i, ci = -1, silent = 0;
            for (i = 1; i < argc; i++) {
                if (_wcsicmp(argv[i], L"/check") == 0) ci = i;
                else if (_wcsicmp(argv[i], L"/silent") == 0) silent = 1;
            }
            if (ci >= 0) {
                int rc = 3;
                if (ci + 2 < argc) {
                    wchar_t ff[32768];
                    VerifyResult vr;
                    memset(&vr, 0, sizeof(vr));
                    if (FindFfmpeg(ff, 32768)) {
                        int vrc = VerifyJobPaths(ff, argv[ci + 1], argv[ci + 2], NULL, -1, &vr);
                        VerifyLog(argv[ci + 1], argv[ci + 2], &vr, vrc);
                        rc = (vrc == 0) ? 0 : 1;
                    } else {
                        wcscpy(vr.detail, L"ffmpeg.exe bulunamadı");
                    }
                    if (!silent) {
                        MessageBoxW(NULL, vr.detail,
                                    rc == 0 ? L"Hydra Remuxer Doğrulama OK" : L"Hydra Remuxer Doğrulama HATASI",
                                    (rc == 0 ? MB_ICONINFORMATION : MB_ICONERROR) | MB_OK);
                    }
                } else if (!silent) {
                    MessageBoxW(NULL, L"Kullanım: HydraRemuxer.exe /check kaynak çıktı [/silent]",
                                L"Hydra Remuxer", MB_ICONINFORMATION | MB_OK);
                }
                LocalFree(argv);
                DeleteCriticalSection(&g_cs);
                CoUninitialize();
                return rc;
            }
            LocalFree(argv);
        }
    }

    // exe'ye dosya suruklenip birakilirsa (ya da arguman verilirse) kuyruga al, baslat
    {
        int argc = 0;
        LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            int i;
            for (i = 1; i < argc; i++) {
                DWORD a;
                if (argv[i][0] == L'-' || argv[i][0] == L'/') continue;
                a = GetFileAttributesW(argv[i]);
                if (a == INVALID_FILE_ATTRIBUTES) continue;
                if (a & FILE_ATTRIBUTE_DIRECTORY) {
                    int before = g_count;
                    AddFolderDropped(argv[i]);
                    if (g_count > before) autoStart = 1;
                } else if (AddJob(argv[i]) >= 0) autoStart = 1;
            }
            LocalFree(argv);
        }
    }

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    if (autoStart) PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_BTN_START, BN_CLICKED), 0);

    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (g_pTask) { g_pTask->lpVtbl->Release(g_pTask); g_pTask = NULL; }
    CoUninitialize();
    DeleteCriticalSection(&g_cs);
    return (int)m.wParam;
}
