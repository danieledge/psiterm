#ifndef PSI_SYSLOG_H
#define PSI_SYSLOG_H
#define LOG_EMERG 0
#define LOG_ALERT 1
#define LOG_CRIT 2
#define LOG_ERR 3
#define LOG_WARNING 4
#define LOG_NOTICE 5
#define LOG_INFO 6
#define LOG_DEBUG 7
#define LOG_AUTH (4<<3)
#define LOG_AUTHPRIV (10<<3)
#define LOG_DAEMON (3<<3)
#define LOG_PID 0x01
#define LOG_NDELAY 0x08
#define openlog(a,b,c) ((void)0)
#define syslog(...) ((void)0)
#define closelog() ((void)0)
#endif
