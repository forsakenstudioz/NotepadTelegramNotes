// Minimal subset of the Notepad++ plugin interface (PluginInterface.h,
// Notepad_plus_msgs.h, menuCmdID.h and Scintilla.h) used by this plugin.
#pragma once
#include <windows.h>
#include <stdint.h>

// ---- Plugin interface -------------------------------------------------------
typedef void(__cdecl *PFUNCPLUGINCMD)();

struct NppData {
	HWND _nppHandle;
	HWND _scintillaMainHandle;
	HWND _scintillaSecondHandle;
};

struct ShortcutKey {
	bool _isCtrl;
	bool _isAlt;
	bool _isShift;
	UCHAR _key;
};

const int menuItemSize = 64;

struct FuncItem {
	wchar_t _itemName[menuItemSize];
	PFUNCPLUGINCMD _pFunc;
	int _cmdID;
	bool _init2Check;
	ShortcutKey *_pShKey;
};

struct Sci_NotifyHeader {
	void *hwndFrom;
	uintptr_t idFrom;
	unsigned int code;
};

// Only the header is read; the rest of SCNotification is never touched.
struct SCNotification {
	Sci_NotifyHeader nmhdr;
};

// ---- Notepad++ messages -----------------------------------------------------
#define NPPMSG (WM_USER + 1000)
#define NPPM_GETCURRENTSCINTILLA (NPPMSG + 4)
#define NPPM_GETCURRENTLANGTYPE (NPPMSG + 5)
#define NPPM_GETNBOPENFILES (NPPMSG + 7)
#define ALL_OPEN_FILES 0
#define PRIMARY_VIEW 1
#define SECOND_VIEW 2
#define NPPM_SETSTATUSBAR (NPPMSG + 24)
#define STATUSBAR_DOC_TYPE 0
#define NPPM_GETPLUGINSCONFIGDIR (NPPMSG + 46)
#define NPPM_MENUCOMMAND (NPPMSG + 48)
#define NPPM_GETFULLPATHFROMBUFFERID (NPPMSG + 58)
#define NPPM_GETBUFFERIDFROMPOS (NPPMSG + 59)
#define MAIN_VIEW 0
#define SUB_VIEW 1
#define NPPM_DOOPEN (NPPMSG + 77)

#define RUNCOMMAND_USER (WM_USER + 3000)
#define NPPM_GETFULLCURRENTPATH (RUNCOMMAND_USER + 1)
#define NPPM_GETFILENAME (RUNCOMMAND_USER + 3)

#define NPPN_FIRST 1000
#define NPPN_READY (NPPN_FIRST + 1)
#define NPPN_FILESAVED (NPPN_FIRST + 8)
#define NPPN_SHUTDOWN (NPPN_FIRST + 9)

#define L_TEXT 0
#define L_USER 15

#define IDM_FILE_NEW 41001

// ---- Scintilla messages -----------------------------------------------------
#define SCI_GETLENGTH 2006
#define SCI_CONVERTEOLS 2029
#define SCI_GETEOLMODE 2030
#define SCI_GETCODEPAGE 2137
#define SCI_GETSELTEXT 2161
#define SCI_SETTEXT 2181
#define SCI_GETTEXT 2182
#define SCI_DOCUMENTSTART 2316
#define SCI_GETSELECTIONEMPTY 2650
