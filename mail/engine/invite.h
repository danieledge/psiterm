/* invite.h - invitations and contact cards in messages (invite.c, invmsg.c)
 *
 * When a message's text is downloaded, its first calendar part (text/calendar,
 * application/ics, or a file ending .ics) and its first contact card
 * (text/vcard, text/x-vcard, or .vcf) - if not too big - are fetched too, and
 * what PsiMail.app needs is written beside the message:
 *
 *   <uid>.ics, <uid>.vcf  the parts as they came (decoded)
 *   <uid>.inv             the invitation: "#PSIINV1", then key TAB value lines
 *                         (method uid seq summary location start end allday
 *                         utcstart utcend alarm repeats tzok recur orgname
 *                         orgaddr orgline me mystatus replyname replyaddr
 *                         replystatus events). start/end are the Psion's wall
 *                         clock (YYYYMMDDHHMM; an all-day end is its last day);
 *                         method is request, cancel, reply, publish...
 *   <uid>.vcd             the cards: "#PSIVCD1 TAB count", then per card
 *                         "begin TAB n", key TAB value lines (fn given family
 *                         middle prefix suffix org title url note, and
 *                         email<kind> tel<kind> adr<kind> with VCF_* bits;
 *                         an address is its seven parts - PO box, extended, street, town,
 *                         region, postcode, country - with \x01 between them), "end"
 * All text is Windows-1252.
 *
 * A reply to an invitation is an ordinary outbox message with Itip headers
 * (see compose.c); itip_build makes its text/calendar METHOD:REPLY part.
 */
#ifndef PM_INVITE_H
#define PM_INVITE_H
#include <stdio.h>

#define INV_MAX_TZ 4

typedef struct
	{
	int ok;                  /* TZOFFSETTO was understood */
	int offset;              /* minutes east of UTC */
	int rule;                /* has BYMONTH/BYDAY */
	int month, week, wday;   /* week 1..4, -1 the last */
	int secs;                /* the local time of day it changes */
	long since;              /* its DTSTART (the newest wins) */
	} TzRule;

typedef struct
	{
	char tzid[64];
	TzRule std, dst;
	int has_std, has_dst;
	} TzDef;

typedef struct
	{
	char method[16];         /* lower case: request, cancel, reply, publish... */
	char uid[256];
	int seq;
	char summary[200];       /* cp1252 */
	char location[120];
	long ustart, uend;       /* UTC; or local for all-day / floating */
	long lstart, lend;       /* the Psion's wall clock */
	int have_start;
	int allday, floating;
	int tz_ok;               /* 0: a TZID we couldn't place (taken as the Psion's) */
	int alarm;               /* minutes before, -1 none */
	int repeats;
	int cancelled;           /* STATUS:CANCELLED */
	char recur_line[200];    /* RECURRENCE-ID as it came, for the reply */
	char org_name[80], org_addr[120];
	char org_line[300];      /* ORGANIZER as it came, for the reply */
	char me[120];            /* the attendee that is this account */
	char my_partstat[24];
	char reply_name[80], reply_addr[120], reply_partstat[24];   /* METHOD:REPLY: who answered */
	int nattendees;
	int nevents;
	int ntz;
	TzDef tz[INV_MAX_TZ];
	} PmInvite;

/* the invitation in an iCalendar text (UTF-8); me = the account's address.
   0 ok, -1 no event in it */
int  inv_parse(const char *text, int len, int zone, const char *me, PmInvite *iv);
void inv_write(FILE *f, const PmInvite *iv);

/* a reply: the outbox file's Itip headers */
typedef struct
	{
	char partstat[16];       /* "Itip:" ACCEPTED / TENTATIVE / DECLINED */
	char uid[256];
	char seq[12];
	char recur[200];         /* the RECURRENCE-ID line, if any */
	char organizer[300];     /* the ORGANIZER line */
	char attendee[120];      /* our address */
	char cn[80];             /* our name (cp1252) */
	char summary[200];       /* cp1252 */
	char dtstart[24], dtend[24];   /* UTC (or dates), may be "" */
	} ItipReply;
/* an outbox header line: 1 if it was one of ours */
int  itip_header(ItipReply *r, const char *name, const char *value);
/* the VCALENDAR (UTF-8, CRLF). Returns its length, -1 if the headers are
   not a proper reply or it doesn't fit */
int  itip_build(const ItipReply *r, long now, char *out, int max);

/* contact cards */
#define VCF_MAX_EMAIL 4
#define VCF_MAX_TEL 6
#define VCF_MAX_ADR 2
#define VCF_HOME 1
#define VCF_WORK 2
#define VCF_CELL 4
#define VCF_FAX 8
#define VCF_PAGER 16
#define VCF_ADR_SEP ','
typedef struct
	{
	char fn[80];
	char given[60], family[60], middle[40], prefix[24], suffix[24];
	char org[80], title[80], url[120], note[200];
	char email[VCF_MAX_EMAIL][96];
	int email_kind[VCF_MAX_EMAIL], nemail;
	char tel[VCF_MAX_TEL][40];
	int tel_kind[VCF_MAX_TEL], ntel;
	char adr[VCF_MAX_ADR][200];
	int adr_kind[VCF_MAX_ADR], nadr;
	} PmCard;
/* the cards in a vCard text: how many (at most max) */
int  vcf_parse(const char *text, int len, PmCard *cards, int max);
void vcf_write(FILE *f, const PmCard *cards, int n);

/* invmsg.c: the engine's side (pm.h first) */
void inv_fetch(int acct, const char *folder, unsigned int uid, const PmStructure *st);
void inv_remove(int acct, const char *folder, unsigned int uid);

#endif
