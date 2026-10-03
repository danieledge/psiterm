/* psiweb_cmds.h - menu commands PsiWeb.app sends to psiweb.exe */
#ifndef PSIWEB_CMDS_H
#define PSIWEB_CMDS_H

/* commands (menu items): app sets cmd_arg then cmd, psiweb clears cmd */
enum
	{
	PW_CMD_NONE = 0,
	PW_CMD_OPEN,                    /* cmd_arg = URL (or words to search for) */
	PW_CMD_BACK,
	PW_CMD_FORWARD,
	PW_CMD_RELOAD,
	PW_CMD_STOP,
	PW_CMD_HOME,
	PW_CMD_ZOOM,                    /* cmd_arg = scale in percent, e.g. "90" */
	PW_CMD_PAGEUP,
	PW_CMD_PAGEDOWN,
	PW_CMD_TOP,
	PW_CMD_BOTTOM,
	PW_CMD_IMAGES,                  /* cmd_arg = "1" load images, "0" don't */
	PW_CMD_QUIT,
	PW_CMD_UPDATE,                  /* download a newer PsiWeb.sis (pwupdate.c) */
	PW_CMD_HANGUP,                  /* hang up and give the serial port back now */
	PW_CMD_SCROLL                   /* cmd_arg = the top of the view, pixels down the
	                                   page (the app's scroll bar: page_y) */
	};

/* auth_state, save_state (psiweb.h) */
enum
	{
	PW_ASK_NONE = 0,
	PW_ASK_ASKING,                  /* psiweb: show the dialog */
	PW_ASK_OK,                      /* app: answered */
	PW_ASK_CANCEL                   /* app: cancelled */
	};

/* update_state */
enum
	{
	PW_UPD_IDLE = 0,
	PW_UPD_RUNNING,
	PW_UPD_CURRENT,                 /* already the newest version */
	PW_UPD_READY,                   /* downloaded to net.save_as, signature OK */
	PW_UPD_FAILED
	};

#endif
