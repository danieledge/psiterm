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
	PW_CMD_QUIT
	};

#endif
