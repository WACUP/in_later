/*
 * in_asap.c - ASAP plugin for Winamp
 *
 * Copyright (C) 2005-2023  Piotr Fusik
 *
 * This file is part of ASAP (Another Slight Atari Player),
 * see http://asap.sourceforge.net
 *
 * ASAP is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * ASAP is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ASAP; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <windows.h>
#include <stdlib.h>
#include <strsafe.h>
#include <commctrl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <sdk/winamp/in2.h>
#include <sdk/winamp/ipc_pe.h>
#include <sdk/winamp/wa_ipc.h>
#include <sdk/nu/AutoChar.h>
#include <sdk/nu/AutoCharFn.h>
#include "api.h"
#include <loader/loader/paths.h>
#include <loader/loader/utils.h>

#include "aatr-stdio.h"
#include "asap.h"
#include "..\info_dlg.h"
#include "..\settings_dlg.h"
#include "win32\resource.h"
#include "..\..\astil.h"

 // wasabi based services for localisation support
SETUP_API_LNG_VARS;

// {AFE5A171-710D-471d-A927-FF68AA4BA84B}
static const GUID InASAPLangGUID =
{ 0xafe5a171, 0x710d, 0x471d, { 0xa9, 0x27, 0xff, 0x68, 0xaa, 0x4b, 0xa8, 0x4b } };

// Winamp's equalizer works only with 16-bit samples
#define SUPPORT_EQUALIZER  1

#define BITS_PER_SAMPLE    16
// 576 is a magic number for Winamp, better do not modify it
#define BUFFERED_BLOCKS    576

extern In_Module plugin;

// configuration
//#define INI_SECTION  TEXT("in_asap")
//static const wchar_t *ini_file;

// TODO need to remove as many of the globals when it comes to
//		playback handling as it's possible to crash this where
//		multiple playback threads can be triggered on overlaps
// current file
CRITICAL_SECTION g_info_cs = { 0 };
ASAP *asap = NULL;
static char *playing_filename_with_song = NULL,
			*last_info_filename = NULL;
static BYTE *playing_module;
static int playing_module_len;
static int duration;

static int playlistLength;

static HANDLE thread_handle = NULL;
static volatile bool thread_run = false;
static bool paused = false;
static int seek_needed;

//static int title_song;

LPCTSTR atrFilenameHash(LPCTSTR filename);

#if 0
static void writeIniInt(const wchar_t *name, int value, const wchar_t *ini_file)
{
	wchar_t str[16] = { 0 };
	WritePrivateProfileString(INI_SECTION, name, I2WStr(value, str, ARRAYSIZE(str)), ini_file);
}

void onUpdatePlayingInfo(void)
{
	writeIniInt(TEXT("playing_info"), playing_info, GetPaths()->winamp_ini_file);
}
#endif

static void read_config(void)
{
	static bool loaded;
	if (!loaded)
	{
		loaded = true;

		LPCWSTR ini_file = GetPaths()->winamp_ini_file;
		sample_rate = GetPrivateProfileInt(INI_SECTION, TEXT("sample_rate"), sample_rate, ini_file);
		song_length = GetPrivateProfileInt(INI_SECTION, TEXT("song_length"), song_length, ini_file);
		silence_seconds = GetPrivateProfileInt(INI_SECTION, TEXT("silence_seconds"), silence_seconds, ini_file);
		play_loops = GetPrivateProfileInt(INI_SECTION, TEXT("play_loops"), play_loops, ini_file);
		mute_mask = GetPrivateProfileInt(INI_SECTION, TEXT("mute_mask"), mute_mask, ini_file);
		playing_info = GetPrivateProfileInt(INI_SECTION, TEXT("playing_info"), playing_info, ini_file);
	}
}

static void config(HWND hwndParent)
{
	read_config();

	LangCreateDialogBox(IDD_SETTINGS, hwndParent, settingsDialogProc, 0);

#if 0
	if (settingsDialog(plugin.hDllInstance, hwndParent))
	{
		LPCWSTR ini_file = GetPaths()->winamp_ini_file;
		writeIniInt(TEXT("sample_rate"), sample_rate, ini_file);
		writeIniInt(TEXT("song_length"), song_length, ini_file);
		writeIniInt(TEXT("silence_seconds"), silence_seconds, ini_file);
		writeIniInt(TEXT("play_loops"), play_loops, ini_file);
		writeIniInt(TEXT("mute_mask"), mute_mask, ini_file);
	}
#endif
}

static void about(HWND hwndParent)
{
	wchar_t message[1024]/* = { 0 }*/;
	// TODO localise
	PrintfCch(message, ARRAYSIZE(message), TEXT("%s\n\n%hs\nWACUP modifications by "
			  "%s (2023-%s)\n\nBuild date: %s\n\n%hs"), (wchar_t*)plugin.description,
			  ASAPInfo_CREDITS, WACUP_Author(), WACUP_Copyright(), TEXT(__DATE__),
			  ASAPInfo_COPYRIGHT);
	AboutMessageBox(hwndParent, message, L"ASAP Decoder");
}

static int extractSongNumber(const wchar_t *s, wchar_t *filename)
{
	// for compatibility this will handle "<file>#index" but the
	// preferred handling under a WACUP instance is "<file>,index"
	int i = (int)_tcslen(s);
	int song = -1;
	if (i > 6 && s[i - 1] >= L'0' && s[i - 1] <= L'9') {
		if (s[i - 2] == L',' || s[i - 2] == L'#') {
			song = s[i - 1] - L'1';
			i -= 2;
		}
		else if (s[i - 2] >= L'0' && s[i - 2] <= L'9' &&
				 (s[i - 3] == L',' || s[i - 3] == L'#')) {
			song = (s[i - 2] - L'0') * 10 + s[i - 1] - L'1';
			i -= 3;
		}
	}
	memcpy(filename, s, i * sizeof(wchar_t));
	filename[i] = '\0';
	return song;
}

static BOOL isATR(const wchar_t *filename)
{
	LPCWSTR ext = FindPathExtension(filename);
	return (SameStr(ext, L"atr"));/*/
	size_t len = strlen(filename);
	return len >= 4 && stricmp(filename + len - 4, TEXT(".atr")) == 0;/**/
}

static void addFileSongs(HWND playlistWnd, fileinfo *fi, const ASAPInfo *info, int *index)
{
#if 0
	char *p = fi->file + strlen(fi->file);
	for (int song = 0; song < ASAPInfo_GetSongs(info); song++) {
		sprintf(p, ",%d", song + 1);
		fi->index = (*index)++;
		COPYDATASTRUCT cds;
		cds.dwData = IPC_PE_INSERTFILENAME;
		cds.lpData = fi;
		cds.cbData = sizeof(fileinfo);
		SendMessage(playlistWnd, WM_COPYDATA, 0, (LPARAM) &cds);
	}
#endif
}

static void expandFileSongs(HWND playlistWnd, int index, ASAPInfo *info)
{
#if 0	// TODO
	const wchar_t *fn = (const wchar_t *) SendMessage(plugin.hMainWindow, WM_WA_IPC, index, IPC_GETPLAYLISTFILEW);
	fileinfoW fi;
	int song = extractSongNumber(fn, fi.file);
	if (song >= 0)
		return;
	if (ASAPInfo_IsOurFile(fi.file)) {
		if (loadModule(fi.file, module, &module_len)
		 && ASAPInfo_Load(info, fi.file, module, module_len)) {
			SendMessage(playlistWnd, WM_WA_IPC, IPC_PE_DELETEINDEX, index);
			addFileSongs(playlistWnd, &fi, info, &index);
		}
	}
	else if (isATR(fi.file)) {
		AATR *disk = AATRStdio_New(fi.file);
		if (disk != NULL) {
			bool found = false;
			AATRRecursiveLister *lister = AATRRecursiveLister_New();
			if (lister != NULL) {
				AATRFileStream *stream = AATRFileStream_New();
				if (stream != NULL) {
					size_t atr_fn_len = strlen(fi.file);
					fi.file[atr_fn_len++] = '#';
					AATRRecursiveLister_Open(lister, disk);
					for (;;) {
						const char *inside_fn = AATRRecursiveLister_NextFile(lister);
						if (inside_fn == NULL)
							break;
						if (ASAPInfo_IsOurFile(inside_fn)) {
							AATRFileStream_Open(stream, AATRRecursiveLister_GetDirectory(lister));
							module_len = AATRFileStream_Read(stream, module, 0, sizeof(module));
							if (ASAPInfo_Load(info, inside_fn, module, module_len)) {
								size_t inside_fn_len = strlen(inside_fn);
								if (atr_fn_len + inside_fn_len + 4 <= sizeof(fi.file)) {
									memcpy(fi.file + atr_fn_len, inside_fn, inside_fn_len + 1);
									if (!found) {
										found = true;
										SendMessage(playlistWnd, WM_WA_IPC, IPC_PE_DELETEINDEX, index);
									}
									addFileSongs(playlistWnd, &fi, info, &index);
								}
							}
						}
					}
					AATRFileStream_Delete(stream);
				}
				AATRRecursiveLister_Delete(lister);
			}
			AATRStdio_Delete(disk);
			/* Prevent Winamp crash:
			   1. Play anything.
			   2. Open an ATR with no songs.
			   3. If the ATR deletes itself, Winamp crashes (tested with 5.581).
			   Solution: leave the ATR on playlist. */
			if (!found && SendMessage(plugin.hMainWindow, WM_WA_IPC, 0, IPC_GETLISTLENGTH) > 1)
				SendMessage(playlistWnd, WM_WA_IPC, IPC_PE_DELETEINDEX, index);
		}
	}
#endif
}

#if 0
static INT_PTR CALLBACK progressDialogProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (uMsg == WM_INITDIALOG)
		SendDlgItemMessage(hDlg, IDC_PROGRESS, PBM_SETRANGE, 0, MAKELPARAM(0, playlistLength));
	return FALSE;
}
#endif

static void expandPlaylistSongs(void)
{
#if 0
	static bool processing = false;
	if (processing)
		return;
	HWND playlistWnd = (HWND) SendMessage(plugin.hMainWindow, WM_WA_IPC, IPC_GETWND_PE, IPC_GETWND);
	if (playlistWnd == NULL)
		return;
	processing = true;
	ASAPInfo *info = ASAPInfo_New();
	playlistLength = SendMessage(plugin.hMainWindow, WM_WA_IPC, 0, IPC_GETLISTLENGTH);
	HWND progressWnd = CreateDialogParam(plugin.hDllInstance, MAKEINTRESOURCE(IDD_PROGRESS), plugin.hMainWindow, progressDialogProc, NULL);
	int index = playlistLength;
	while (--index >= 0) {
		if ((index & 15) == 0)
			SendDlgItemMessage(progressWnd, IDC_PROGRESS, PBM_SETPOS, playlistLength - index, 0);
		expandFileSongs(playlistWnd, index, info);
	}
	DestroyWindow(progressWnd);
	ASAPInfo_Delete(info);
	processing = false;
#endif
}

static int init(void)
{
	InitializeCriticalSectionEx(&g_info_cs, 400, CRITICAL_SECTION_NO_DEBUG_INFO);

	StartPluginLangWithDesc(plugin.hDllInstance, InASAPLangGUID, IDS_PLUGIN_NAME,
									TEXT(ASAPInfo_VERSION), &plugin.description);

	return IN_INIT_SUCCESS;
}

static void quit(void)
{
	if (asap != NULL)
	{
		ASAP_Delete(asap);
		asap = NULL;
	}

	DeleteCriticalSection(&g_info_cs);
}

#if 0
static void getTitle(char *title)
{
	if (ASAPInfo_GetSongs(title_info) > 1)
		sprintf(title, "%s (song %d)", ASAPInfo_GetTitleOrFilename(title_info), title_song + 1);
	else
		strcpy(title, ASAPInfo_GetTitleOrFilename(title_info));
}

static char *tagFunc(char *tag, void *p)
{
	if (_stricmp(tag, "artist") == 0) {
		const char *author = ASAPInfo_GetAuthor(title_info);
		return author[0] != '\0' ? _strdup(author) : NULL;
	}
	if (_stricmp(tag, "title") == 0) {
		char *title = (char*)malloc(strlen(ASAPInfo_GetTitleOrFilename(title_info)) + 11);
		if (title != NULL)
			getTitle(title);
		return title;
	}
	return NULL;
}

static void tagFreeFunc(char *tag, void *p)
{
	free(tag);
}
#endif

static void getFileInfo(const in_char *file, in_char *title, int *length_in_ms)
{
#if 0	// TODO
	if (file == NULL || file[0] == '\0')
		file = playing_filename_with_song;
	char filename[MAX_PATH];
	title_song = extractSongNumber(file, filename);
	if (title_song < 0)
		expandPlaylistSongs();
	if (!loadModule(filename, module, &module_len))
		return;
	const char *hash = atrFilenameHash(filename);
	if (!ASAPInfo_Load(title_info, hash != NULL ? hash + 1 : filename, module, module_len))
		return;
	if (title_song < 0)
		title_song = ASAPInfo_GetDefaultSong(title_info);
	if (title != NULL) {
		waFormatTitle fmt_title = {
			NULL, NULL, title, 512, tagFunc, tagFreeFunc
		};
		getTitle(title); // in case IPC_FORMAT_TITLE doesn't work...
		SendMessage(plugin.hMainWindow, WM_WA_IPC, (WPARAM) &fmt_title, IPC_FORMAT_TITLE);
	}
	if (length_in_ms != NULL)
		*length_in_ms = getSongDuration(title_info, title_song);
#else
	if (file == NULL || file[0] == '\0')
	{
		if (length_in_ms != NULL)
		{
			*length_in_ms = duration;
		}
	}

	if (title)
	{
		*title = 0;
	}
#endif
}

/*static int infoBox(const in_char *file, HWND hwndParent)
{
#if 0	// TODO
	char filename[MAX_PATH];
	int song = extractSongNumber(file, filename);
	showInfoDialog(plugin.hDllInstance, hwndParent, filename, song);
	return 0;
#else
	return INFOBOX_UNCHANGED;
#endif
}*/

static int isOurFile(IN_ISOURFILE_PARAM)
{
	if (!IsPathURL(fn))
	{
		wchar_t filename[FILENAME_SIZE]/* = { 0 }*/;
		extractSongNumber(fn, filename);
#ifndef _WIN64
		LPCWSTR ext = FindPathExtension(filename);
#endif
		if (ext)
		{
			const AutoChar ext8(ext);
			return ASAPInfo_IsOurExt(ext8);
		}
	}
	return 0;
}

static DWORD WINAPI playThread(LPVOID dummy)
{
	const int channels = ASAPInfo_GetChannels(ASAP_GetInfo(asap));
	while (thread_run) {
		static BYTE buffer[BUFFERED_BLOCKS * 2 * (BITS_PER_SAMPLE / 8)
#if SUPPORT_EQUALIZER
			* 2
#endif
			];
		int buffered_bytes = BUFFERED_BLOCKS * channels * (BITS_PER_SAMPLE / 8);
		if (seek_needed >= 0) {
			plugin.outMod->Flush(seek_needed);
			ASAP_Seek(asap, seek_needed);
			seek_needed = -1;
		}
		if (plugin.outMod->CanWrite() >= buffered_bytes
#if SUPPORT_EQUALIZER
			<< plugin.dsp_isactive()
#endif
		) {
			buffered_bytes = ASAP_Generate(asap, buffer, buffered_bytes, /*BITS_PER_SAMPLE == 8 ?
												ASAPSampleFormat_U8 :*/ ASAPSampleFormat_S16_L_E);
			if (buffered_bytes <= 0) {
				plugin.outMod->CanWrite();
				if (!plugin.outMod->IsPlaying()) {
					PostEOF();/*/
					PostMessage(plugin.hMainWindow, WM_WA_MPEG_EOF, 0, 0);/**/
					break;
				}
				SleepEx(10, TRUE);
				continue;
			}
			int t = plugin.outMod->GetWrittenTime();
			plugin.SAAddPCMData(buffer, channels, BITS_PER_SAMPLE, t);
			/*plugin.VSAAddPCMData(buffer, channels, BITS_PER_SAMPLE, t);*/
#if SUPPORT_EQUALIZER
			t = buffered_bytes / (channels * (BITS_PER_SAMPLE / 8));
			t = plugin.dsp_dosamples((short *) buffer, t, BITS_PER_SAMPLE, channels, sample_rate);
			t *= channels * (BITS_PER_SAMPLE / 8);
			plugin.outMod->Write((char *) buffer, t);
#else
			plugin.outMod->Write((char *) buffer, buffered_bytes);
#endif
		}
		else
			SleepEx(20, TRUE);
	}

	if (thread_handle != NULL)
	{
		CloseHandle(thread_handle);
		thread_handle = NULL;
	}
	return 0;
}

static int play(const in_char *fn)
{
	read_config();

	if (asap == NULL)
	{
		asap = ASAP_New();
	}
	if (asap != NULL)
	{
		wchar_t filename[MAX_PATH]/* = { 0 }*/;
		int song = extractSongNumber(fn, filename);

		if (playing_module == NULL)
		{
			playing_module = (BYTE*)SafeMalloc(ASAPInfo_MAX_MODULE_LENGTH);
		}

		if (!loadModule(filename, playing_module, &playing_module_len, NULL))
			return -1;

		EnterCriticalSection(&g_info_cs);
		if (playing_filename_with_song)
		{
			SafeFree(playing_filename_with_song);
		}
		playing_filename_with_song = AutoCharFnDup(filename);
		LeaveCriticalSection(&g_info_cs);

		if (!ASAP_Load(asap, playing_filename_with_song, playing_module, playing_module_len))
			return 1;
		const ASAPInfo *info = ASAP_GetInfo(asap);
		if (song < 0)
			song = ASAPInfo_GetDefaultSong(info);
		duration = playSong(song);
		if (duration < 0)
			return 1;

		const int channels = ASAPInfo_GetChannels(info);
		const int maxlatency = (plugin.outMod && plugin.outMod->Open && sample_rate &&
								channels ? plugin.outMod->Open(sample_rate, channels,
														BITS_PER_SAMPLE, -1, -1) : -1);
		if (maxlatency < 0)
			return 1;

		const int bitrate = (channels * sample_rate * BITS_PER_SAMPLE);
		plugin.SetInfo(bitrate / 1000, sample_rate / 1000, channels, 1);
#ifndef _WIN64
		plugin.SAVSAInit(maxlatency, sample_rate);
		plugin.VSASetInfo(sample_rate, channels);
#else
		plugin.VisInitInfo(maxlatency, sample_rate, channels);
#endif
		plugin.outMod->SetVolume(-666);
		seek_needed = -1;

		thread_handle = StartPlaybackThread(playThread, 0, 0, NULL);
		thread_run = (thread_handle != NULL);
		//setPlayingSong(filename, song);
		return !thread_run;
	}
	return -1;
}

static void pause(void)
{
	paused = true;
	if (plugin.outMod)
	{
		plugin.outMod->Pause(1);
	}
}

static void unPause(void)
{
	paused = false;
	if (plugin.outMod)
	{
		plugin.outMod->Pause(0);
	}
}

static int isPaused(void)
{
	return paused;
}

static void stop(void)
{
	thread_run = false;
	// wait max 10 seconds
	WaitForThreadToClose(&thread_handle, 10 * 1000/*/INFINITE/**/);

	if (plugin.outMod && plugin.outMod->Close)
	{
		plugin.outMod->Close();
	}
	if (plugin.outMod)
	{
		plugin.SAVSADeInit();
	}
}

static int getLength(void)
{
	return duration;
}

static int getOutputTime(void)
{
	return (plugin.outMod ? plugin.outMod->GetOutputTime() : 0);
}

static void setOutputTime(int time_in_ms)
{
	seek_needed = time_in_ms;
}

static void setVolume(int volume)
{
	if (plugin.outMod && plugin.outMod->SetVolume)
	{
		plugin.outMod->SetVolume(volume);
	}
}

static void setPan(int pan)
{
	if (plugin.outMod && plugin.outMod->SetPan)
	{
		plugin.outMod->SetPan(pan);
	}
}

void GetFileExtensions(void)
{
	if (!plugin.FileExtensions)
	{
		const InputFileListArray extensions[]
		{
			{ L"SAP", 3 },
			{ L"CMC;CM3;CMR;CMS;DMC", 19 },
			{ L"DLT", 3 },
			{ L"MPT;MPD", 7 },
			{ L"RMT", 3 },
			{ L"TMC;TM8", 7 },
			{ L"TM2", 3 },
			{ L"FC", 2 },
			{ L"ATR", 3 }
		},
			// TODO localise
			descriptions[]
		{
			{ L"Slight Atari Player (*.SAP)", 27 },
			{ L"Chaos Music Composer (*.CMC;*.CM3;*.CMR;*.CMS;*.DMC)", 52 },
			{ L"Delta Music Composer (*.DLT)", 28 },
			{ L"Music ProTracker (*.MPT;*.MPD)", 30 },
			{ L"Raster Music Tracker (*.RMT)", 28 },
			{ L"Theta Music Composer 1.x (*.TMC;*.TM8)", 38 },
			{ L"Theta Music Composer 2.x (*.TM2)", 32 },
			{ L"Future Composer (*.FC)", 22 },
			{ L"Atari 8-bit Disk Image (*.ATR)", 30 }
		};

		plugin.FileExtensions = BuildInputFileListArrayString(extensions, descriptions,
													ARRAYSIZE(extensions), NULL, NULL);
	}
}

const wchar_t wacup_plugin_id[] = { L'w', L'a', L'c', L'u', L'p', L'(', L'i', L'n', L'_', L'l',
									L'a', L't', L'e', L'r', L'.', L'd', L'l', L'l', L')', 0 };

#define OUR_INPUT_PLUG_IN_FEATURES INPUT_HAS_READ_META | INPUT_USES_UNIFIED_ALT3 | \
								   INPUT_HAS_FORMAT_CONVERSION_UNICODE | \
								   INPUT_HAS_FORMAT_CONVERSION_SET_TIME_MODE

In_Module plugin = {
	IN_VER_WACUP,
	IN_INIT_PRE_FEATURES
	/*"ASAP " ASAPInfo_VERSION/*/
	(char*)wacup_plugin_id/**/,
	0, 0, // filled by Winamp
	0,	  // filled in by GetFileExtensions
	1,    // is_seekable
	1,    // UsesOutputPlug
	config,
	about,
	init,
	quit,
	getFileInfo,
	0/*infoBox*/,
	isOurFile,
	play,
	pause,
	unPause,
	isPaused,
	stop,
	getLength,
	getOutputTime,
	setOutputTime,
	setVolume,
	setPan,
	IN_INIT_VIS_RELATED_CALLS,
	0, 0,	// dsp related
	IN_INIT_WACUP_EQSET_EMPTY
	NULL,	// SetInfo
	NULL,	// filled by Winamp
	NULL,	// api_service
	IN_INIT_POST_FEATURES
	GetFileExtensions,	// loading optimisation
	IN_INIT_WACUP_END_STRUCT
};

extern "C" __declspec(dllexport) In_Module *winampGetInModule2(void)
{
	return &plugin;
}

extern "C" __declspec(dllexport) int winampUninstallPlugin(HINSTANCE hDllInst, HWND hwndDlg, int param)
{
	// TODO
	// prompt to remove our settings with default as no (just incase)
	/*if (UninstallSettingsPrompt(reinterpret_cast<const wchar_t*>(plugin.description)))
	{
		REMOVE_INI_SECTION_W(app_nameW, 0);
	}*/

	// as we're doing too much in subclasses, etc we cannot allow for on-the-fly removal so need to do a normal reboot
	return IN_PLUGIN_UNINSTALL_REBOOT;
}

// TODO localise
extern "C" const wchar_t* pExtList[15] = { L"SAP", L"CMC", L"CM3", L"CMR", L"CMS",
										   L"DMC", L"DLT", L"MPT", L"MPD", L"RMT",
										   L"TMC", L"TM8", L"TM2", L"FC", L"ATR" };
extern "C" const wchar_t* pExtDescList[15] =
{
	L"Slight Atari Player File",
	L"Chaos Music Composer File",
	L"Chaos Music Composer File",
	L"Chaos Music Composer File",
	L"Chaos Music Composer File",
	L"Chaos Music Composer File",
	L"Delta Music Composer File",
	L"Music ProTracker File",
	L"Music ProTracker File",
	L"Raster Music Tracker File",
	L"Theta Music Composer 1.x File",
	L"Theta Music Composer 1.x File",
	L"Theta Music Composer 2.x File",
	L"Future Composer File",
	L"Atari 8-bit Disk Image",
};

BOOL GetExtensionName(LPCWSTR pszExt, LPWSTR pszDest, INT cchDest)
{
	int index = sizeof(pExtList) / sizeof(wchar_t*) - 1;
	for (; (index >= 0) && (CSTR_EQUAL != CompareStringEx(LOCALE_NAME_INVARIANT, NORM_IGNORECASE, pszExt,
													  -1, pExtList[index], -1, NULL, NULL, 0)); index--);
	return ((index >= 0) && CopyCchStr(pszDest, cchDest, pExtDescList[index]));
}

// return 1 if you want winamp to show it's own file info dialogue, 0 if you want to show your own (via In_Module.InfoBox)
// if returning 1, remember to implement winampGetExtendedFileInfo("formatinformation")!
extern "C" __declspec(dllexport) int winampUseUnifiedFileInfoDlg(const wchar_t* fn)
{
	return 1;
}

// should return a child window of 513x271 pixels (341x164 in msvc dlg units), or return NULL for no tab.
// Fill in name (a buffer of namelen characters), this is the title of the tab (defaults to "Advanced").
// filename will be valid for the life of your window. n is the tab number. This function will first be 
// called with n == 0, then n == 1 and so on until you return NULL (so you can add as many tabs as you like).
// The window you return will recieve WM_COMMAND, IDOK/IDCANCEL messages when the user clicks OK or Cancel.
// when the user edits a field which is duplicated in another pane, do a SendMessage(GetParent(hwnd),WM_USER,(WPARAM)L"fieldname",(LPARAM)L"newvalue");
// this will be broadcast to all panes (including yours) as a WM_USER.
extern "C" __declspec(dllexport) HWND winampAddUnifiedFileInfoPane(int n, const wchar_t* filename, HWND parent, wchar_t* name, size_t namelen)
{
	return NULL;
}

static wchar_t* appendAddress(wchar_t* p, const wchar_t* format, int value)
{
	if (value >= 0)
	{
		p += swprintf(p, format, value);
	}
	return p;
}

static char* appendStilString(char* p, const char* s)
{
	for (;;)
	{
		char c = *s++;
		switch (c)
		{
			case '\0':
				return p;
			case '\n':
				c = ' ';
				break;
			default:
				break;
		}
		*p++ = c;
	}
}

static char* appendStil(char* p, const char* prefix, const char* value)
{
	if (value[0] != '\0')
	{
		p = appendStilString(p, prefix);
		p = appendStilString(p, value);
		*p++ = '\r';
		*p++ = '\n';
	}
	return p;
}

static int get_metadata(const char* filename, const ASAPInfo* info, int song,
						BYTE *the_module, const int the_module_len,
						const char* data, wchar_t* dest, const int destlen)
{
	if (song < 0)
	{
		song = ASAPInfo_GetDefaultSong(info);
	}

	if (SameStrA(data, "length"))
	{
		I2WStr(getSongDuration(info, song), dest, destlen);
		return 1;
	}
	else if (SameStrA(data, "artist"))
	{
		const char* author = ASAPInfo_GetAuthor(info);
		if (author && *author)
		{
			ConvertANSI(author, -1, CP_ACP, dest, destlen, NULL);
			return 1;
		}
	}
	else if (SameStrA(data, "title"))
	{
		const char* title = ASAPInfo_GetTitle(info);
		if (title && *title)
		{
			ConvertANSI(title, -1, CP_ACP, dest, destlen, NULL);
			return 1;
		}
	}
	else if (SameStrA(data, "year"))
	{
		const char* date = ASAPInfo_GetDate(info);
		if (date && *date)
		{
			// clean-up the date to try to just be
			// the year that wacup is expecting...
			const char* slash = strrchr(date, '/');
			char* space = (char*)strrchr(date, ' ');
			if (space)
			{
				*space = 0;
			}
			ConvertANSI((slash ? (slash + 1) : date), -1, CP_ACP, dest, destlen, NULL);
			return 1;
		}
	}
	else if (SameStrA(data, "formatinformation"))
	{
		wchar_t* p = dest;
		const int length = getSongDuration(info, song);
		if (length > 0)
		{
			p += swprintf(p, L"Length: %.2f seconds\r\n", length / 1000.f);
		}

		const char* ext = ASAPInfo_GetOriginalModuleExt(info, the_module, the_module_len);
		if (ext != NULL)
		{
			p += swprintf(p, L"Composed in %hs\r\n", ASAPInfo_GetExtDescription(ext));
		}

		int i = ASAPInfo_GetSongs(info);
		if (i > 1)
		{
			p += swprintf(p, L"Songs: %d\r\n", i);
			i = ASAPInfo_GetDefaultSong(info);
			if (i > 0)
			{
				p += swprintf(p, L"Default Song: %d\r\n", i + 1);
			}
		}

		p += swprintf(p, L"Channels: ");
		p += swprintf(p, ASAPInfo_GetChannels(info) > 1 ? L"Stereo\t" : L"Mono\t");
		p += swprintf(p, L"Mode: ");
		p += swprintf(p, ASAPInfo_IsNtsc(info) ? L"NTSC\r\n" : L"PAL\r\n");

		const int type = ASAPInfo_GetTypeLetter(info);
		if (type != 0)
		{
			p += swprintf(p, L"Type: %c\r\n", type);
		}
		p += swprintf(p, L"Fastplay: %d (%d Hz)\r\n", ASAPInfo_GetPlayerRateScanlines(info),
															ASAPInfo_GetPlayerRateHz(info));
		if (type == 'C')
		{
			p += swprintf(p, L"Music: %04X\r\n", ASAPInfo_GetMusicAddress(info));
		}

		if (type != 0)
		{
			p = appendAddress(p, L"Init: %04X\r\n", ASAPInfo_GetInitAddress(info));
			p = appendAddress(p, L"Player: %04X\r\n", ASAPInfo_GetPlayerAddress(info));
			p = appendAddress(p, L"Covox: %04X\r\n", ASAPInfo_GetCovoxAddress(info));
		}

		i = ASAPInfo_GetSapHeaderLength(info);
		if (i >= 0)
		{
			while (p < dest + destlen - 17 && i + 4 < the_module_len)
			{
				int start = the_module[i] + (the_module[i + 1] << 8);
				if (start == 0xffff)
				{
					i += 2;
					start = the_module[i] + (the_module[i + 1] << 8);
				}

				const int end = the_module[i + 2] + (the_module[i + 3] << 8);
				p += swprintf(p, L"Load: %04X-%04X\r\n", start, end);
				i += 5 + end - start;
			}
		}
		return 1;
	}
	else if (SameStrA(data, "genre"))
	{
		CopyCchStr(dest, destlen, L"Video Game Music");
		return 1;
	}
	else if (SameStrA(data, "comment"))
	{
		static ASTIL *astil;
		if (!astil)
		{
			astil = ASTIL_New();
			if (astil != NULL)
			{
				if (!ASTIL_Load(astil, filename, song))
				{
					ASTIL_Delete(astil);
					astil = NULL;
				}
			}
		}

		if (astil)
		{
			char* buf = (char*)SafeMalloc(16000);
			if (buf)
			{
				char* p = buf;
				p = appendStil(p, "", ASTIL_GetTitle(astil));
				p = appendStil(p, "by ", ASTIL_GetAuthor(astil));
				p = appendStil(p, "Directory comment: ", ASTIL_GetDirectoryComment(astil));
				p = appendStil(p, "File comment: ", ASTIL_GetFileComment(astil));
				p = appendStil(p, "Song comment: ", ASTIL_GetSongComment(astil));

				for (int i = 0; ; i++)
				{
					const ASTILCover* cover = ASTIL_GetCover(astil, i);
					if (cover == NULL)
					{
						break;
					}

					const int startSeconds = ASTILCover_GetStartSeconds(cover);
					if (startSeconds >= 0)
					{
						const int endSeconds = ASTILCover_GetEndSeconds(cover);
						if (endSeconds >= 0)
						{
							p += sprintf(p, "At %d:%02d-%d:%02d c", startSeconds / 60,
								 startSeconds % 60, endSeconds / 60, endSeconds % 60);
						}
						else
						{
							p += sprintf(p, "At %d:%02d c", startSeconds / 60, startSeconds % 60);
						}
					}
					else
					{
						*p++ = 'C';
					}

					const char* s = ASTILCover_GetTitleAndSource(cover);
					p = appendStil(p, "overs: ", s[0] != '\0' ? s : "<?>");
					p = appendStil(p, "by ", ASTILCover_GetArtist(cover));
					p = appendStil(p, "Comment: ", ASTILCover_GetComment(cover));
				}

				ConvertANSI(buf, -1, (ASTIL_IsUTF8(astil) ? CP_UTF8 : CP_ACP), dest, destlen, NULL);

				SafeFree(buf);
			}
			return 1;
		}
	}
	else if (SameStrA(data, "bitrate"))
	{
		const int br = (ASAPInfo_GetChannels(info) * sample_rate * BITS_PER_SAMPLE);
		if (br > 0)
		{
			I2WStr((br / 1000), dest, destlen);
			return 1;
		}
	}
	else if (SameStrA(data, "samplerate"))
	{
		// TODO possible need to consider
		//		looking at this for info.
		//ASAP_GetSampleRate(asap)
		I2WStr(sample_rate, dest, destlen);
		return 1;
	}
	else if (SameStrA(data, "bitdepth"))
	{
		// TODO is this correct though as it's been
		//      hard-coded to be 16-bit it should
		dest[0] = L'1';
		dest[1] = L'6';
		dest[2] = 0;
		return 1;
	}
	return 0;
}

extern "C" __declspec(dllexport) int winampGetExtendedFileInfoW(const wchar_t *fn, const char *data, wchar_t *dest, int destlen)
{
	if (SameStrA(data, "type") ||
		SameStrA(data, "lossless") ||
		SameStrA(data, "streammetadata"))
	{
		dest[0] = L'0';
		dest[1] = L'\0';
		return 1;
	}
    else if (SameStrNA(data, "stream", 6) &&
             (SameStrA((data + 6), "type") ||
              SameStrA((data + 6), "genre") ||
              SameStrA((data + 6), "url") ||
              SameStrA((data + 6), "name") ||
			  SameStrA((data + 6), "title")))
	{
		return 0;
	}

	if (!fn || !fn[0])
	{
		return 0;
	}

	if (SameStrA(data, "family"))
	{
		LPCWSTR ext = FindPathExtension(fn);
		return (ext ? GetExtensionName(ext, dest, destlen) : 0);
	}

	read_config();

	wchar_t filename[MAX_PATH]/* = { 0 }*/;
	const int title_song = extractSongNumber(fn, filename);

	// if we're playing then try to get the metadata
	// from that copy to save loading a new instance
	EnterCriticalSection(&g_info_cs);

	const AutoCharFn playing_file(fn);
	const bool playing = SameStrA(playing_file, playing_filename_with_song);
	if (playing && (playing_module_len > 0))
	{
		const int ret = get_metadata(playing_file, ASAP_GetInfo(asap),
									 title_song, playing_module, playing_module_len,
									 data, dest, destlen);
		LeaveCriticalSection(&g_info_cs);
		return ret;
	}

	static BYTE* title_module;
	static int title_module_len;
	static ASAPInfo* title_info;
	const AutoCharFn file(filename);
	bool reset = SameStrA(data, "reset"), clean_up = false;
	if (reset || !last_info_filename || !SameStrA(file, last_info_filename))
	{
		if (last_info_filename != NULL)
		{
			SafeFree(last_info_filename);
			last_info_filename = NULL;
		}

		if (!reset)
		{
			last_info_filename = SafeAnsiDup(file);

			if (title_module == NULL)
			{
				title_module = (BYTE*)SafeMalloc(ASAPInfo_MAX_MODULE_LENGTH);
			}

			LPCTSTR hashW = NULL;
			if (loadModule(filename, title_module, &title_module_len, &hashW))
			{
				//LPCTSTR hashW = atrFilenameHash(filename);
				const AutoChar hash(hashW);

				if (title_info == NULL)
				{
					title_info = ASAPInfo_New();
				}

				if (!title_info || !ASAPInfo_Load(title_info, (hash != NULL) ? (hash + 1) :
												  file, title_module, title_module_len))
				{
					clean_up = true;
				}
			}
		}
		else
		{
			clean_up = true;
		}
	}

	if (clean_up)
	{
		title_module_len = 0;

		if (title_module != NULL)
		{
			SafeFree(title_module);
			title_module = NULL;
		}

		if (title_info != NULL)
		{
			ASAPInfo_Delete(title_info);
			title_info = NULL;
		}
	}

	const int ret = (((title_module_len > 0) && title_info) ? get_metadata(file, title_info,
					 title_song, title_module, title_module_len, data, dest, destlen) : 0);
	LeaveCriticalSection(&g_info_cs);
	return ret;
}


struct ExtendedRead
{
	ExtendedRead() : asap(ASAP_New()), info(NULL), module((BYTE*)calloc(sizeof(BYTE),
										  ASAPInfo_MAX_MODULE_LENGTH)), module_len(0)
	{
	}

	~ExtendedRead()
	{
		if (asap != NULL)
		{
			ASAP_Delete(asap);
			asap = NULL;
		}
	}

	ASAP* asap;
	const ASAPInfo* info;
	BYTE* module;
	int module_len = 0;
};

extern "C" __declspec(dllexport) intptr_t winampGetExtendedRead_openW(const wchar_t* fn, int* size, int* bps, int* nch, int* srate)
{
	ExtendedRead* e = new ExtendedRead();
	if (e)
	{
		wchar_t filename[MAX_PATH]/* = { 0 }*/;
		int song = extractSongNumber(fn, filename);
		if (loadModule(filename, e->module, &e->module_len, NULL))
		{
			const AutoCharFn file(filename);
			if (ASAP_Load(e->asap, file, e->module, e->module_len))
			{
				e->info = ASAP_GetInfo(e->asap);
				if (e->info)
				{
					if (song < 0)
					{
						song = ASAPInfo_GetDefaultSong(e->info);
					}

					if (ASAP_PlaySong(e->asap, song, getSongDurationInternal(e->info, song, e->asap)))
					{
						ASAP_MutePokeyChannels(e->asap, 0/*/mute_mask/**/);

						*bps = BITS_PER_SAMPLE;
						*nch = ASAPInfo_GetChannels(e->info);
						*srate = sample_rate;
						*size = -1; // TODO need to get number of samples, etc
						return reinterpret_cast<intptr_t>(e);
					}
				}
			}
		}

		delete e;
	}
	return 0;
}

extern "C" __declspec(dllexport) intptr_t winampGetExtendedRead_getData(intptr_t handle, char* dest, size_t len, int* /*killswitch*/)
{
	ExtendedRead* e = reinterpret_cast<ExtendedRead*>(handle);
	return (e ? ASAP_Generate(e->asap, (uint8_t*)dest, (int)len,
								 ASAPSampleFormat_S16_L_E) : 0);
}

extern "C" __declspec(dllexport) int winampGetExtendedRead_setTime(intptr_t handle, int time_in_ms)
{
	ExtendedRead* e = reinterpret_cast<ExtendedRead*>(handle);
	return (e ? ASAP_Seek(e->asap, time_in_ms) : 0);
}

extern "C" __declspec(dllexport) void winampGetExtendedRead_close(intptr_t handle)
{
	ExtendedRead* e = reinterpret_cast<ExtendedRead*>(handle);
	if (e)
	{
		delete e;
	}
}

const int get_songs_count(const char* fn, const wchar_t* inside_fn, AATRRecursiveLister* lister, AATRFileStream* stream, int *songs)
{
	if (ASAPInfo_IsOurFile(fn))
	{
		ASAPInfo* playlist_info = ASAPInfo_New();
		if (playlist_info != NULL)
		{
			if (lister != NULL)
			{
				AATRFileStream_Open(stream, AATRRecursiveLister_GetDirectory(lister));
			}

			BYTE* playlist_module = (BYTE*)SafeMalloc(ASAPInfo_MAX_MODULE_LENGTH);
			if (playlist_module != NULL)
			{
				int playlist_module_len = (lister ? AATRFileStream_Read(stream, playlist_module, 0,
																  ASAPInfo_MAX_MODULE_LENGTH) : 0);

				if ((!lister ? loadModule(inside_fn, playlist_module, &playlist_module_len, NULL) : true) &&
									 ASAPInfo_Load(playlist_info, fn, playlist_module, playlist_module_len))
				{
					if (songs)
					{
						*songs += ASAPInfo_GetSongs(playlist_info);
					}
				}
				SafeFree(playlist_module);
			}
			ASAPInfo_Delete(playlist_info);
			return true;
		}
	}
	return false;
}

extern "C" __declspec(dllexport) int GetSubSongInfo(const wchar_t* filename)
{
	wchar_t inside_fn[MAX_PATH]/* = { 0 }*/;
	extractSongNumber(filename, inside_fn);
	int songs = 0;
	const AutoCharFn fn(inside_fn);
	if (!get_songs_count(fn, inside_fn, NULL, NULL, &songs) && isATR(filename))
	{
		AATR *disk = AATRStdio_New(fn);
		if (disk != NULL) {
			bool found = false;
			AATRRecursiveLister *lister = AATRRecursiveLister_New();
			if (lister != NULL) {
				AATRFileStream *stream = AATRFileStream_New();
				if (stream != NULL)
				{
					AATRRecursiveLister_Open(lister, disk);
					for (;;)
					{
						const char *inside_fn = AATRRecursiveLister_NextFile(lister);
						if (inside_fn == NULL)
						{
							break;
						}
						songs += get_songs_count(fn, NULL, lister, stream, &songs);
					}
					AATRFileStream_Delete(stream);
				}
				AATRRecursiveLister_Delete(lister);
			}
			AATRStdio_Delete(disk);
		}
	}
	return songs;
}