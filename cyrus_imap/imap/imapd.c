/* 
 * Copyright (c) 1998-2003 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any other legal
 *    details, please contact  
 *      Office of Technology Transfer
 *      Carnegie Mellon University
 *      5000 Forbes Avenue
 *      Pittsburgh, PA  15213-3890
 *      (412) 268-4387, fax: (412) 268-7395
 *      tech-transfer@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    "This product includes software developed by Computing Services
 *    acknowledgment:
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/* $Id: imapd.c,v 1.12 2005/03/05 00:36:55 dasenbro Exp $ */

#include <config.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <syslog.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <sasl/sasl.h>

#include "acl.h"
#include "annotate.h"
#include "append.h"
#include "auth.h"
#include "backend.h"
#include "charset.h"
#include "exitcodes.h"
#include "idle.h"
#include "global.h"
#include "imap_err.h"
#include "imapd.h"
#include "imapurl.h"
#include "imparse.h"
#include "iptostring.h"
#include "mailbox.h"
#include "message.h"
#include "mboxname.h"
#include "mboxlist.h"
#include "mbdump.h"
#include "mkgmtime.h"
#include "mupdate-client.h"
#include "quota.h"
#include "telemetry.h"
#include "tls.h"
#include "user.h"
#include "util.h"
#include "version.h"
#include "xmalloc.h"

#include "pushstats.h"		/* SNMP interface */

#include "AppleOD.h"

extern void seen_done(void);

extern int optind;
extern char *optarg;

/* global state */
const int config_need_data = CONFIG_NEED_PARTITION_DATA;

static char shutdownfilename[1024];
static int imaps = 0;
static sasl_ssf_t extprops_ssf = 0;

/* per-user/session state */
struct protstream *imapd_out = NULL;
struct protstream *imapd_in = NULL;
static char imapd_clienthost[NI_MAXHOST*2+1] = "[local]";
static int imapd_logfd = -1;
char *imapd_userid;
static char *imapd_magicplus = NULL;
struct auth_state *imapd_authstate = 0;
static int imapd_userisadmin = 0;
static int imapd_userisproxyadmin = 0;
static sasl_conn_t *imapd_saslconn; /* the sasl connection context */
static int imapd_starttls_done = 0; /* have we done a successful starttls? */
#ifdef HAVE_SSL
/* our tls connection, if any */
static SSL *tls_conn = NULL;
#endif /* HAVE_SSL */

/* stage(s) for APPEND */
struct appendstage {
    struct stagemsg *stage;
    char **flag;
    int nflags, flagalloc;
    time_t internaldate;
} **stage = NULL;
unsigned long numstage = 0;

/* the sasl proxy policy context */
static struct proxy_context imapd_proxyctx = {
    1, 1, &imapd_authstate, &imapd_userisadmin, &imapd_userisproxyadmin
};

/* current sub-user state */
static struct mailbox mboxstruct;
static struct mailbox *imapd_mailbox;
int imapd_exists = -1;

/* current namespace */
static struct namespace imapd_namespace;

static const char *monthname[] = {
    "jan", "feb", "mar", "apr", "may", "jun",
    "jul", "aug", "sep", "oct", "nov", "dec"
};

static const int max_monthdays[] = {
    31, 29, 31, 30, 31, 30,
    31, 31, 30, 31, 30, 31
};

void motd_file(int fd);
void shut_down(int code);
void fatal(const char *s, int code);

void cmdloop(void);
void cmd_login(char *tag, char *user);
void cmd_authenticate(char *tag, char *authtype, char *resp);
void cmd_noop(char *tag, char *cmd);
void cmd_capability(char *tag);
void cmd_append(char *tag, char *name);
void cmd_select(char *tag, char *cmd, char *name);
void cmd_close(char *tag);
void cmd_fetch(char *tag, char *sequence, int usinguid);
void cmd_partial(const char *tag, const char *msgno, char *data,
		 const char *start, const char *count);
void cmd_store(char *tag, char *sequence, char *operation, int usinguid);
void cmd_search(char *tag, int usinguid);
void cmd_sort(char *tag, int usinguid);
void cmd_thread(char *tag, int usinguid);
void cmd_copy(char *tag, char *sequence, char *name, int usinguid);
void cmd_expunge(char *tag, char *sequence);
void cmd_create(char *tag, char *name, char *partition, int localonly);
void cmd_delete(char *tag, char *name, int localonly);
void cmd_dump(char *tag, char *name, int uid_start);
void cmd_undump(char *tag, char *name);
void cmd_xfer(char *tag, char *name, char *toserver, char *topart);
void cmd_rename(const char *tag, char *oldname, 
		char *newname, char *partition);
void cmd_reconstruct(const char *tag, const char *name, int recursive);
void cmd_find(char *tag, char *namespace, char *pattern);
void cmd_list(char *tag, int subscribed, char *reference, char *pattern);
void cmd_changesub(char *tag, char *namespace, char *name, int add);
void cmd_getacl(const char *tag, const char *name);
void cmd_listrights(char *tag, char *name, char *identifier);
void cmd_myrights(const char *tag, const char *name);
void cmd_setacl(const char *tag, const char *name,
		const char *identifier, const char *rights);
void cmd_getquota(const char *tag, const char *name);
void cmd_getquotaroot(const char *tag, const char *name);
void cmd_setquota(const char *tag, const char *quotaroot);
void cmd_status(char *tag, char *name);
void cmd_getuids(char *tag, char *startuid);
void cmd_unselect(char* tag);
void cmd_namespace(char* tag);
void cmd_mupdatepush(char *tag, char *name);
void cmd_id(char* tag);
extern void id_getcmdline(int argc, char **argv);
extern void id_response(struct protstream *pout);

void cmd_idle(char* tag);
void idle_update(idle_flags_t flags);

void cmd_starttls(char *tag, int imaps);

#ifdef ENABLE_X_NETSCAPE_HACK
void cmd_netscrape(char* tag);
#endif

void cmd_getannotation(char* tag, char *mboxpat);
void cmd_setannotation(char* tag, char *mboxpat);

int getannotatefetchdata(char *tag,
			 struct strlist **entries, struct strlist **attribs);
int getannotatestoredata(char *tag, struct entryattlist **entryatts);

void annotate_response(struct entryattlist *l);

#ifdef ENABLE_LISTEXT
int getlistopts(char *tag, int *listopts);
#endif

int getsearchprogram(char *tag, struct searchargs *searchargs,
			int *charset, int parsecharset);
int getsearchcriteria(char *tag, struct searchargs *searchargs,
			 int *charset, int parsecharset);
int getsearchdate(time_t *start, time_t *end);
int getsortcriteria(char *tag, struct sortcrit **sortcrit);
int getdatetime(time_t *date);

void printstring(const char *s);
void printastring(const char *s);

void appendfieldlist(struct fieldlist **l, char *section,
		     struct strlist *fields, char *trail,
		     void *d, size_t size);
void freefieldlist(struct fieldlist *l);
void freestrlist(struct strlist *l);
void appendsearchargs(struct searchargs *s, struct searchargs *s1,
			 struct searchargs *s2);
void freesearchargs(struct searchargs *s);
static void freesortcrit(struct sortcrit *s);

static int mailboxdata(char *name, int matchlen, int maycreate, void *rock);
static int listdata(char *name, int matchlen, int maycreate, void *rock);
static void mstringdata(char *cmd, char *name, int matchlen, int maycreate,
			int listopts);

extern void setproctitle_init(int argc, char **argv, char **envp);
extern int proc_register(const char *progname, const char *clienthost, 
			 const char *userid, const char *mailbox);
extern void proc_cleanup(void);

extern int saslserver(sasl_conn_t *conn, const char *mech,
		      const char *init_resp, const char *resp_prefix,
		      const char *continuation, const char *empty_resp,
		      struct protstream *pin, struct protstream *pout,
		      int *sasl_result, char **success_data);

/* Enable the resetting of a sasl_conn_t */
static int reset_saslconn(sasl_conn_t **conn);

static struct 
{
    char *ipremoteport;
    char *iplocalport;
    sasl_ssf_t ssf;
    char *authid;
} saslprops = {NULL,NULL,0,NULL};

static int imapd_canon_user(sasl_conn_t *conn, void *context,
			    const char *user, unsigned ulen,
			    unsigned flags, const char *user_realm,
			    char *out, unsigned out_max, unsigned *out_ulen)
{
    char userbuf[MAX_MAILBOX_NAME+1], *p;
    size_t n;
    int r;

    if (!ulen) ulen = strlen(user);

    if (config_getswitch(IMAPOPT_IMAPMAGICPLUS)) {
	/* make a working copy of the auth[z]id */
	if (ulen > MAX_MAILBOX_NAME) {
	    sasl_seterror(conn, 0, "buffer overflow while canonicalizing");
	    return SASL_BUFOVER;
	}
	memcpy(userbuf, user, ulen);
	userbuf[ulen] = '\0';
	user = userbuf;

	/* See if we're using the magic plus
	   (currently we don't support anything after '+') */
	if ((p = strchr(userbuf, '+')) && 
	    (n = config_virtdomains ? strcspn(p, "@") : strlen(p)) == 1) {

	    if (flags & SASL_CU_AUTHZID) {
		/* make a copy of the magic plus */
		if (imapd_magicplus) free(imapd_magicplus);
		imapd_magicplus = xstrndup(p, n);
	    }

	    /* strip the magic plus from the auth[z]id */
	    memmove(p, p+n, strlen(p+n)+1);
	    ulen -= n;
	}
    }

    r = mysasl_canon_user(conn, context, user, ulen, flags, user_realm,
			  out, out_max, out_ulen);

    if (!r && imapd_magicplus && flags == SASL_CU_AUTHZID) {
	/* If we're only doing the authzid, put back the magic plus
	   in case its used in the challenge/response calculation */
	n = strlen(imapd_magicplus);
	if (*out_ulen + n > out_max) {
	    sasl_seterror(conn, 0, "buffer overflow while canonicalizing");
	    r = SASL_BUFOVER;
	}
	else {
	    p = (config_virtdomains && (p = strchr(out, '@'))) ?
		p : out + *out_ulen;
	    memmove(p+n, p, strlen(p)+1);
	    memcpy(p, imapd_magicplus, n);
	    *out_ulen += n;
	}
    }

    return r;
}

static int imapd_proxy_policy(sasl_conn_t *conn,
			      void *context,
			      const char *requested_user, unsigned rlen,
			      const char *auth_identity, unsigned alen,
			      const char *def_realm,
			      unsigned urlen,
			      struct propctx *propctx)
{
    if (config_getswitch(IMAPOPT_IMAPMAGICPLUS)) {
	char userbuf[MAX_MAILBOX_NAME+1], *p;
	size_t n;

	/* make a working copy of the authzid */
	if (!rlen) rlen = strlen(requested_user);
	if (rlen > MAX_MAILBOX_NAME) {
	    sasl_seterror(conn, 0, "buffer overflow while proxying");
	    return SASL_BUFOVER;
	}
	memcpy(userbuf, requested_user, rlen);
	userbuf[rlen] = '\0';
	requested_user = userbuf;

	/* See if we're using the magic plus */
	if ((p = strchr(userbuf, '+'))) {
	    n = config_virtdomains ? strcspn(p, "@") : strlen(p);

	    /* strip the magic plus from the authzid */
	    memmove(p, p+n, strlen(p+n)+1);
	    rlen -= n;
	}
    }

    return mysasl_proxy_policy(conn, context, requested_user, rlen,
			       auth_identity, alen, def_realm, urlen, propctx);
}

static const struct sasl_callback mysasl_cb[] = {
    { SASL_CB_GETOPT, &mysasl_config, NULL },
    { SASL_CB_PROXY_POLICY, &imapd_proxy_policy, (void*) &imapd_proxyctx },
    { SASL_CB_CANON_USER, &imapd_canon_user, NULL },
    { SASL_CB_LIST_END, NULL, NULL }
};

/* imapd_refer() issues a referral to the client. */
static void imapd_refer(const char *tag,
			const char *server,
			const char *mailbox)
{
    char url[MAX_MAILBOX_PATH+1];

    if(!strcmp(imapd_userid, "anonymous")) {
	imapurl_toURL(url, server, mailbox, "ANONYMOUS");
    } else {
	imapurl_toURL(url, server, mailbox, "*");
    }
    
    prot_printf(imapd_out, "%s NO [REFERRAL %s] Remote mailbox.\r\n", 
		tag, url);
}

/* wrapper for mboxlist_lookup that will force a referral if we are remote
 * returns IMAP_SERVER_UNAVAILABLE if we don't have a place to send the client
 * (that'd be a bug).
 * returns IMAP_MAILBOX_MOVED if we referred the client */
/* ext_name is the external name of the mailbox */
/* you can avoid referring the client by setting tag or ext_name to NULL. */
static int mlookup(const char *tag, const char *ext_name,
		   const char *name, int *flags, char **pathp, char **partp,
		   char **aclp, struct txn **tid) 
{
    int r, mbtype;
    char *remote, *acl;

    r = mboxlist_detail(name, &mbtype, pathp, &remote, &acl, tid);

    if(partp) *partp = remote;
    if(aclp) *aclp = acl;
    if(flags) *flags = mbtype;
    if(r) return r;

    if(mbtype & MBTYPE_RESERVE) return IMAP_MAILBOX_RESERVED;
    
    if(mbtype & MBTYPE_MOVING) {
	/* do we have rights on the mailbox? */
	if(!imapd_userisadmin &&
	   (!acl || !(cyrus_acl_myrights(imapd_authstate,acl) & ACL_LOOKUP))) {
	    r = IMAP_MAILBOX_NONEXISTENT;
	} else if(tag && ext_name && remote && *remote) {
	    char *c = NULL;
	    
	    c = strchr(remote, '!');
	    if(c) *c = '\0';
	    imapd_refer(tag, remote, ext_name);
	    r = IMAP_MAILBOX_MOVED;
	} else if(config_mupdate_server) {
	    r = IMAP_SERVER_UNAVAILABLE;
	} else {
	    r = IMAP_MAILBOX_NOTSUPPORTED;
	}
    }
    
    return r;
}

static void imapd_reset(void)
{
    proc_cleanup();

    if (imapd_mailbox) {
	index_closemailbox(imapd_mailbox);
	mailbox_close(imapd_mailbox);
	imapd_mailbox = 0;
    }

    if (imapd_in) {
	/* Flush the incoming buffer */
	prot_NONBLOCK(imapd_in);
	prot_fill(imapd_in);

	prot_free(imapd_in);
    }

    if (imapd_out) {
	/* Flush the outgoing buffer */
	prot_flush(imapd_out);

	prot_free(imapd_out);
    }
    
    imapd_in = imapd_out = NULL;

#ifdef HAVE_SSL
    if (tls_conn) {
	if (tls_reset_servertls(&tls_conn) == -1) {
	    fatal("tls_reset() failed", EC_TEMPFAIL);
	}
	tls_conn = NULL;
    }
#endif

    cyrus_reset_stdio(); 

    strcpy(imapd_clienthost, "[local]");
    if (imapd_logfd != -1) {
	close(imapd_logfd);
	imapd_logfd = -1;
    }
    if (imapd_userid != NULL) {
	free(imapd_userid);
	imapd_userid = NULL;
    }
    if (imapd_magicplus != NULL) {
	free(imapd_magicplus);
	imapd_magicplus = NULL;
    }
    if (imapd_authstate) {
	auth_freestate(imapd_authstate);
	imapd_authstate = NULL;
    }
    imapd_userisadmin = 0;
    imapd_userisproxyadmin = 0;
    if (imapd_saslconn) {
	sasl_dispose(&imapd_saslconn);
	imapd_saslconn = NULL;
    }
    imapd_starttls_done = 0;

    if(saslprops.iplocalport) {
	free(saslprops.iplocalport);
	saslprops.iplocalport = NULL;
    }
    if(saslprops.ipremoteport) {
	free(saslprops.ipremoteport);
	saslprops.ipremoteport = NULL;
    }
    if(saslprops.authid) {
	free(saslprops.authid);
	saslprops.authid = NULL;
    }
    saslprops.ssf = 0;

    imapd_exists = -1;
}

/*
 * run once when process is forked;
 * MUST NOT exit directly; must return with non-zero error code
 */
int service_init(int argc, char **argv, char **envp)
{
    int ret;
    int opt;
    
    if (geteuid() == 0) fatal("must run as the Cyrus user", EC_USAGE);
    setproctitle_init(argc, argv, envp);

    /* set signal handlers */
    signals_set_shutdown(&shut_down);
    signal(SIGPIPE, SIG_IGN);

    /* load the SASL plugins */
    global_sasl_init(1, 1, mysasl_cb);

    ret = snprintf(shutdownfilename, sizeof(shutdownfilename),
		   "%s/msg/shutdown", config_dir);
    
    if(ret < 0 || ret >= sizeof(shutdownfilename)) {
       fatal("shutdownfilename buffer too small (configdirectory too long)",
	     EC_CONFIG);
    }

    /* open the mboxlist, we'll need it for real work */
    mboxlist_init(0);
    mboxlist_open(NULL);
    mailbox_initialize();

    /* open the quota db, we'll need it for real work */
    quotadb_init(0);
    quotadb_open(NULL);

    /* setup for sending IMAP IDLE notifications */
    idle_enabled();

    /* create connection to the SNMP listener, if available. */
    snmp_connect(); /* ignore return code */
    snmp_set_str(SERVER_NAME_VERSION,CYRUS_VERSION);

    while ((opt = getopt(argc, argv, "sp:")) != EOF) {
	switch (opt) {
	case 's': /* imaps (do starttls right away) */
	    imaps = 1;
	    if (!tls_enabled()) {
		syslog(LOG_ERR, "imaps: required OpenSSL options not present");
		fatal("imaps: required OpenSSL options not present",
		      EC_CONFIG);
	    }
	    break;
	case 'p': /* external protection */
	    extprops_ssf = atoi(optarg);
	    break;
	default:
	    break;
	}
    }

    /* Initialize the annotatemore extention */
    annotatemore_init(0, NULL, NULL);
    annotatemore_open(NULL);

    return 0;
}

/*
 * run for each accepted connection
 */
#ifdef ID_SAVE_CMDLINE
int service_main(int argc, char **argv, char **envp __attribute__((unused)))
#else
int service_main(int argc __attribute__((unused)),
		 char **argv __attribute__((unused)),
		 char **envp __attribute__((unused)))
#endif
{
    socklen_t salen;
    int timeout;
    sasl_security_properties_t *secprops = NULL;
    struct sockaddr_storage imapd_localaddr, imapd_remoteaddr;
    char localip[60], remoteip[60];
    char hbuf[NI_MAXHOST];
    int niflags;
    int imapd_haveaddr = 0;

    signals_poll();

#ifdef ID_SAVE_CMDLINE
    /* get command line args for use in ID before getopt mangles them */
    id_getcmdline(argc, argv);
#endif

    imapd_in = prot_new(0, 0);
    imapd_out = prot_new(1, 1);

    /* Find out name of client host */
    salen = sizeof(imapd_remoteaddr);
    if (getpeername(0, (struct sockaddr *)&imapd_remoteaddr, &salen) == 0 &&
	(imapd_remoteaddr.ss_family == AF_INET ||
	 imapd_remoteaddr.ss_family == AF_INET6)) {
	if (getnameinfo((struct sockaddr *)&imapd_remoteaddr, salen,
			hbuf, sizeof(hbuf), NULL, 0, NI_NAMEREQD) == 0) {
	    strncpy(imapd_clienthost, hbuf, sizeof(hbuf));
	    strlcat(imapd_clienthost, " ", sizeof(imapd_clienthost));
	    imapd_clienthost[sizeof(imapd_clienthost)-30] = '\0';
	} else {
	    imapd_clienthost[0] = '\0';
	}
	niflags = NI_NUMERICHOST;
#ifdef NI_WITHSCOPEID
	if (((struct sockaddr *)&imapd_remoteaddr)->sa_family == AF_INET6)
	    niflags |= NI_WITHSCOPEID;
#endif
	if (getnameinfo((struct sockaddr *)&imapd_remoteaddr, salen, hbuf,
			sizeof(hbuf), NULL, 0, niflags) != 0)
	    strlcpy(hbuf, "unknown", sizeof(hbuf));
	strlcat(imapd_clienthost, "[", sizeof(imapd_clienthost));
	strlcat(imapd_clienthost, hbuf, sizeof(imapd_clienthost));
	strlcat(imapd_clienthost, "]", sizeof(imapd_clienthost));
	salen = sizeof(imapd_localaddr);
	if (getsockname(0, (struct sockaddr *)&imapd_localaddr, &salen) == 0) {
	    if(iptostring((struct sockaddr *)&imapd_remoteaddr, salen,
			  remoteip, sizeof(remoteip)) == 0
	       && iptostring((struct sockaddr *)&imapd_localaddr, salen,
			     localip, sizeof(localip)) == 0) {
		imapd_haveaddr = 1;
	    }
	}
    }

    /* create the SASL connection */
    if (sasl_server_new("imap", config_servername, 
			NULL, NULL, NULL, NULL, 0, 
			&imapd_saslconn) != SASL_OK) {
	fatal("SASL failed initializing: sasl_server_new()", EC_TEMPFAIL);
    }

    /* never allow plaintext, since IMAP has the LOGIN command */
    secprops = mysasl_secprops(SASL_SEC_NOPLAINTEXT);
    sasl_setprop(imapd_saslconn, SASL_SEC_PROPS, secprops);
    sasl_setprop(imapd_saslconn, SASL_SSF_EXTERNAL, &extprops_ssf);

    if (imapd_haveaddr) {
	sasl_setprop(imapd_saslconn, SASL_IPREMOTEPORT, remoteip);
	saslprops.ipremoteport = xstrdup(remoteip);
	sasl_setprop(imapd_saslconn, SASL_IPLOCALPORT, localip);
	saslprops.iplocalport = xstrdup(localip);
    }

    proc_register("imapd", imapd_clienthost, NULL, NULL);

    /* Set inactivity timer */
    timeout = config_getint(IMAPOPT_TIMEOUT);
    if (timeout < 30) timeout = 30;
    prot_settimeout(imapd_in, timeout*60);
    prot_setflushonread(imapd_in, imapd_out);

    /* we were connected on imaps port so we should do 
       TLS negotiation immediately */
    if (imaps == 1) cmd_starttls(NULL, 1);

    snmp_increment(TOTAL_CONNECTIONS, 1);
    snmp_increment(ACTIVE_CONNECTIONS, 1);

 	char *skipCommand = getenv( "IMAP_LIMIT_REACHED" );
	pid_t pid = getpid();
	
	if (skipCommand == NULL)
		cmdloop();
	else
	{
		prot_printf( imapd_out, "* BYE %s Cyrus IMAP4 server connection limit reached\r\n", config_servername );
		syslog(LOG_INFO, "Process %d is over the server connection limit. Signalling process to exit.",pid);	
	}

    /* LOGOUT executed */
    prot_flush(imapd_out);
    snmp_increment(ACTIVE_CONNECTIONS, -1);

    /* cleanup */
    imapd_reset();

	if (skipCommand != NULL)	
	{
		kill( pid, SA_NOCLDSTOP);
		syslog(LOG_INFO, "Process %d is Done with kill.",pid);	
	}

    return 0;
}

/* Called by service API to shut down the service */
void service_abort(int error)
{
    shut_down(error);
}

/*
 * found a motd file; spit out message and return
 */
void motd_file(fd)
int fd;
{
    struct protstream *motd_in;
    char buf[1024];
    char *p;

    motd_in = prot_new(fd, 0);

    prot_fgets(buf, sizeof(buf), motd_in);
    if ((p = strchr(buf, '\r'))!=NULL) *p = 0;
    if ((p = strchr(buf, '\n'))!=NULL) *p = 0;

    for(p = buf; *p == '['; p++); /* can't have [ be first char, sigh */
    prot_printf(imapd_out, "* OK [ALERT] %s\r\n", p);
}

/*
 * Cleanly shut down and exit
 */
void shut_down(int code) __attribute__((noreturn));
void shut_down(int code)
{
    proc_cleanup();
    if (imapd_mailbox) {
	index_closemailbox(imapd_mailbox);
	mailbox_close(imapd_mailbox);
    }
    seen_done();
    mboxlist_close();
    mboxlist_done();

    quotadb_close();
    quotadb_done();

    annotatemore_close();
    annotatemore_done();

    if (imapd_in) {
	/* Flush the incoming buffer */
	prot_NONBLOCK(imapd_in);
	prot_fill(imapd_in);
	
	prot_free(imapd_in);
    }
    
    if (imapd_out) {
	/* Flush the outgoing buffer */
	prot_flush(imapd_out);
	prot_free(imapd_out);
	
	/* one less active connection */
	snmp_increment(ACTIVE_CONNECTIONS, -1);
    }

#ifdef HAVE_SSL
    tls_shutdown_serverengine();
#endif

    cyrus_done();

    exit(code);
}

void fatal(const char *s, int code)
{
    static int recurse_code = 0;

    if (recurse_code) {
	/* We were called recursively. Just give up */
	proc_cleanup();
	snmp_increment(ACTIVE_CONNECTIONS, -1);
	exit(recurse_code);
    }
    recurse_code = code;
    if (imapd_out) {
	prot_printf(imapd_out, "* BYE Fatal error: %s\r\n", s);
	prot_flush(imapd_out);
    }
    if (stage) {
	/* Cleanup the stage(s) */
	while (numstage) {
	    struct appendstage *curstage = stage[--numstage];

	    append_removestage(curstage->stage);
	    while (curstage->nflags--) {
		free(curstage->flag[curstage->nflags]);
	    }
	    if (curstage->flag) free((char *) curstage->flag);
	    free(curstage);
	}
	free(stage);
    }

    syslog(LOG_ERR, "Fatal error: %s", s);
    shut_down(code);
}

/*
 * Top-level command loop parsing
 */
void cmdloop()
{
    int fd;
    char motdfilename[1024];
    int c;
    int ret;
    int usinguid, havepartition, havenamespace, recursive;
    static struct buf tag, cmd, arg1, arg2, arg3, arg4;
    char *p, shut[1024];
    const char *err;

    prot_printf(imapd_out,
		"* OK %s Cyrus IMAP4 %s server ready\r\n", config_servername,
		CYRUS_VERSION);

    ret = snprintf(motdfilename, sizeof(motdfilename), "%s/msg/motd",
		   config_dir);
    
    if(ret < 0 || ret >= sizeof(motdfilename)) {
       fatal("motdfilename buffer too small (configdirectory too long)",
	     EC_CONFIG);
    }
    
    if ((fd = open(motdfilename, O_RDONLY, 0)) != -1) {
	motd_file(fd);
	close(fd);
    }

    for (;;) {
	if ( !imapd_userisadmin && imapd_userid
	     && shutdown_file(shut, sizeof(shut))) {
	    for (p = shut; *p == '['; p++); /* can't have [ be first char */
	    prot_printf(imapd_out, "* BYE [ALERT] %s\r\n", p);
	    shut_down(0);
	}

	signals_poll();

	/* Parse tag */
	c = getword(imapd_in, &tag);
	if (c == EOF) {
	    if ((err = prot_error(imapd_in))!=NULL
		&& strcmp(err, PROT_EOF_STRING)) {
		syslog(LOG_WARNING, "%s, closing connection", err);
		prot_printf(imapd_out, "* BYE %s\r\n", err);
	    }
	    return;
	}
	if (c != ' ' || !imparse_isatom(tag.s) || (tag.s[0] == '*' && !tag.s[1])) {
	    prot_printf(imapd_out, "* BAD Invalid tag\r\n");
	    eatline(imapd_in, c);
	    continue;
	}

	/* Parse command name */
	c = getword(imapd_in, &cmd);
	if (!cmd.s[0]) {
	    prot_printf(imapd_out, "%s BAD Null command\r\n", tag.s);
	    eatline(imapd_in, c);
	    continue;
	}
	if (islower((unsigned char) cmd.s[0])) 
	    cmd.s[0] = toupper((unsigned char) cmd.s[0]);
	for (p = &cmd.s[1]; *p; p++) {
	    if (isupper((unsigned char) *p)) *p = tolower((unsigned char) *p);
	}

	/* Only Authenticate/Login/Logout/Noop/Capability/Id/Starttls
	   allowed when not logged in */
	if (!imapd_userid && !strchr("ALNCIS", cmd.s[0])) goto nologin;
    
	/* note that about half the commands (the common ones that don't
	   hit the mailboxes file) now close the mailboxes file just in
	   case it was open. */
	switch (cmd.s[0]) {
	case 'A':
	    if (!strcmp(cmd.s, "Authenticate")) {
		int haveinitresp = 0;

		if (c != ' ') goto missingargs;
		c = getword(imapd_in, &arg1);
		if (!imparse_isatom(arg1.s)) {
		    prot_printf(imapd_out, "%s BAD Invalid authenticate mechanism\r\n", tag.s);
		    eatline(imapd_in, c);
		    continue;
		}
		if (c == ' ') {
		    haveinitresp = 1;
		    c = getword(imapd_in, &arg2);
		    if (c == EOF) goto missingargs;
		}
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;
		
		if (imapd_userid) {
		    prot_printf(imapd_out, "%s BAD Already authenticated\r\n", tag.s);
		    continue;
		}
		cmd_authenticate(tag.s, arg1.s, haveinitresp ? arg2.s : NULL);

		snmp_increment(AUTHENTICATE_COUNT, 1);
	    }
	    else if (!imapd_userid) goto nologin;
	    else if (!strcmp(cmd.s, "Append")) {
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg1);
		if (c != ' ') goto missingargs;

		cmd_append(tag.s, arg1.s);

		snmp_increment(APPEND_COUNT, 1);
	    }
	    else goto badcmd;
	    break;

	case 'B':
	    if (!strcmp(cmd.s, "Bboard")) {
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg1);
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;

		cmd_select(tag.s, cmd.s, arg1.s);

		snmp_increment(BBOARD_COUNT, 1);
	    }
	    else goto badcmd;
	    break;

	case 'C':
	    if (!strcmp(cmd.s, "Capability")) {
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;
		cmd_capability(tag.s);

		snmp_increment(CAPABILITY_COUNT, 1);
	    }
	    else if (!imapd_userid) goto nologin;
	    else if (!strcmp(cmd.s, "Check")) {
		if (!imapd_mailbox) goto nomailbox;
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;

		cmd_noop(tag.s, cmd.s);

		snmp_increment(CHECK_COUNT, 1);
	    }
	    else if (!strcmp(cmd.s, "Copy")) {
		if (!imapd_mailbox) goto nomailbox;
		usinguid = 0;
		if (c != ' ') goto missingargs;
	    copy:
		c = getword(imapd_in, &arg1);
		if (c == '\r') goto missingargs;
		if (c != ' ' || !imparse_issequence(arg1.s)) goto badsequence;
		c = getastring(imapd_in, imapd_out, &arg2);
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;

		cmd_copy(tag.s, arg1.s, arg2.s, usinguid);

		snmp_increment(COPY_COUNT, 1);
	    }
	    else if (!strcmp(cmd.s, "Create")) {
		havepartition = 0;
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg1);
		if (c == EOF) goto missingargs;
		if (c == ' ') {
		    havepartition = 1;
		    c = getword(imapd_in, &arg2);
		    if (!imparse_isatom(arg2.s)) goto badpartition;
		}
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;
		cmd_create(tag.s, arg1.s, havepartition ? arg2.s : 0, 0);

		snmp_increment(CREATE_COUNT, 1);
	    }
	    else if (!strcmp(cmd.s, "Close")) {
		if (!imapd_mailbox) goto nomailbox;
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;

		cmd_close(tag.s);

		snmp_increment(CLOSE_COUNT, 1);
	    }
	    else goto badcmd;
	    break;

	case 'D':
	    if (!strcmp(cmd.s, "Delete")) {
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg1);
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;
		cmd_delete(tag.s, arg1.s, 0);

		snmp_increment(DELETE_COUNT, 1);
	    }
	    else if (!strcmp(cmd.s, "Deleteacl")) {
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg1);
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg2);
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;
		cmd_setacl(tag.s, arg1.s, arg2.s, NULL);

		snmp_increment(DELETEACL_COUNT, 1);
	    }
	    else if (!strcmp(cmd.s, "Dump")) {
		int uid_start = 0;
		
		if(c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg1);
		if(c == ' ') {
		    c = getastring(imapd_in, imapd_out, &arg2);
		    if(!imparse_isnumber(arg2.s)) goto extraargs;
		    uid_start = atoi(arg2.s);
		}
		
		if(c == '\r') c = prot_getc(imapd_in);
		if(c != '\n') goto extraargs;
		
		cmd_dump(tag.s, arg1.s, uid_start);
	    /*	snmp_increment(DUMP_COUNT, 1);*/
	    }
	    else goto badcmd;
	    break;

	case 'E':
	    if (!strcmp(cmd.s, "Expunge")) {
		if (!imapd_mailbox) goto nomailbox;
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;

		cmd_expunge(tag.s, 0);

		snmp_increment(EXPUNGE_COUNT, 1);
	    }
	    else if (!strcmp(cmd.s, "Examine")) {
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg1);
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;

		cmd_select(tag.s, cmd.s, arg1.s);

		snmp_increment(EXAMINE_COUNT, 1);
	    }
	    else goto badcmd;
	    break;

	case 'F':
	    if (!strcmp(cmd.s, "Fetch")) {
		if (!imapd_mailbox) goto nomailbox;
		usinguid = 0;
		if (c != ' ') goto missingargs;
	    fetch:
		c = getword(imapd_in, &arg1);
		if (c == '\r') goto missingargs;
		if (c != ' ' || !imparse_issequence(arg1.s)) goto badsequence;

		cmd_fetch(tag.s, arg1.s, usinguid);

		snmp_increment(FETCH_COUNT, 1);
	    }
	    else if (!strcmp(cmd.s, "Find")) {
		c = getword(imapd_in, &arg1);
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg2);
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;
		cmd_find(tag.s, arg1.s, arg2.s);

		snmp_increment(FIND_COUNT, 1);
	    }
	    else goto badcmd;
	    break;

	case 'G':
	    if (!strcmp(cmd.s, "Getacl")) {
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg1);
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;
		cmd_getacl(tag.s, arg1.s);

		snmp_increment(GETACL_COUNT, 1);
	    }
	    else if (!strcmp(cmd.s, "Getannotation")) {
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg1);
		if (c != ' ') goto missingargs;

		cmd_getannotation(tag.s, arg1.s);

		snmp_increment(GETANNOTATION_COUNT, 1);
	    }
	    else if (!strcmp(cmd.s, "Getquota")) {
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg1);
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;
		cmd_getquota(tag.s, arg1.s);

		snmp_increment(GETQUOTA_COUNT, 1);
	    }
	    else if (!strcmp(cmd.s, "Getquotaroot")) {
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg1);
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;
		cmd_getquotaroot(tag.s, arg1.s);

		snmp_increment(GETQUOTAROOT_COUNT, 1);
	    }
	    else goto badcmd;
	    break;

	case 'I':
	    if (!strcmp(cmd.s, "Id")) {
		if (c != ' ') goto missingargs;
		cmd_id(tag.s);

		snmp_increment(ID_COUNT, 1);
	    }
	    else if (!imapd_userid) goto nologin;
	    else if (!strcmp(cmd.s, "Idle")) {
		if (!idle_enabled()) {
		    /* we don't support idle */
		    goto badcmd;
		}

		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;
		cmd_idle(tag.s);

		snmp_increment(IDLE_COUNT, 1);
	    }
	    else goto badcmd;
	    break;

	case 'L':
	    if (!strcmp(cmd.s, "Login")) {
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg1);
		if(c != ' ') goto missingargs;

		cmd_login(tag.s, arg1.s);

		snmp_increment(LOGIN_COUNT, 1);
	    }
	    else if (!strcmp(cmd.s, "Logout")) {
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;

		snmp_increment(LOGOUT_COUNT, 1);		

		prot_printf(imapd_out, "* BYE %s\r\n", 
			    error_message(IMAP_BYE_LOGOUT));
		prot_printf(imapd_out, "%s OK %s\r\n", tag.s, 
			    error_message(IMAP_OK_COMPLETED));
		return;
	    }
	    else if (!imapd_userid) goto nologin;
	    else if (!strcmp(cmd.s, "List")) {
		int listopts = LIST_CHILDREN;
#ifdef ENABLE_LISTEXT
		/* Check for and parse LISTEXT options */
		c = prot_getc(imapd_in);
		if (c == '(') {
		    c = getlistopts(tag.s, &listopts);
		    if (c == EOF) {
			eatline(imapd_in, c);
			continue;
		    }
		}
		else
		    prot_ungetc(c, imapd_in);
#endif /* ENABLE_LISTEXT */
		if (imapd_magicplus) listopts |= LIST_SUBSCRIBED;
		c = getastring(imapd_in, imapd_out, &arg1);
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg2);
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;
		cmd_list(tag.s, listopts, arg1.s, arg2.s);

		snmp_increment(LIST_COUNT, 1);
	    }
	    else if (!strcmp(cmd.s, "Lsub")) {
		c = getastring(imapd_in, imapd_out, &arg1);
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg2);
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;
		cmd_list(tag.s, LIST_LSUB | LIST_CHILDREN, arg1.s, arg2.s);

		snmp_increment(LSUB_COUNT, 1);
	    }
	    else if (!strcmp(cmd.s, "Listrights")) {
		c = getastring(imapd_in, imapd_out, &arg1);
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg2);
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;
		cmd_listrights(tag.s, arg1.s, arg2.s);

		snmp_increment(LISTRIGHTS_COUNT, 1);
	    }
	    else if (!strcmp(cmd.s, "Localcreate")) {
		/* create a local-only mailbox */
		havepartition = 0;
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg1);
		if (c == EOF) goto missingargs;
		if (c == ' ') {
		    havepartition = 1;
		    c = getword(imapd_in, &arg2);
		    if (!imparse_isatom(arg2.s)) goto badpartition;
		}
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;
		cmd_create(tag.s, arg1.s, havepartition ? arg2.s : NULL, 1);

		/* xxxx snmp_increment(CREATE_COUNT, 1); */
	    }
	    else if (!strcmp(cmd.s, "Localdelete")) {
		/* delete a mailbox locally only */
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg1);
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;
		cmd_delete(tag.s, arg1.s, 1);

		/* xxxx snmp_increment(DELETE_COUNT, 1); */
	    }
	    else goto badcmd;
	    break;

	case 'M':
	    if (!strcmp(cmd.s, "Myrights")) {
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg1);
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;
		cmd_myrights(tag.s, arg1.s);

		/* xxxx snmp_increment(MYRIGHTS_COUNT, 1); */
	    }
	    else if (!strcmp(cmd.s, "Mupdatepush")) {
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg1);
		if(c == EOF) goto missingargs;
		if(c == '\r') c = prot_getc(imapd_in);
		if(c != '\n') goto extraargs;
		cmd_mupdatepush(tag.s, arg1.s);
		
		/* xxxx snmp_increment(MUPDATEPUSH_COUNT, 1); */
	    } else goto badcmd;
	    break;

	case 'N':
	    if (!strcmp(cmd.s, "Noop")) {
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;

		cmd_noop(tag.s, cmd.s);

		/* xxxx snmp_increment(NOOP_COUNT, 1); */
	    }
#ifdef ENABLE_X_NETSCAPE_HACK
	    else if (!strcmp(cmd.s, "Netscape")) {
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;
		cmd_netscrape(tag.s);
	    }
#endif
	    else if (!imapd_userid) goto nologin;
	    else if (!strcmp(cmd.s, "Namespace")) {
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;
		cmd_namespace(tag.s);

		/* xxxx snmp_increment(NAMESPACE_COUNT, 1); */
	    }
	    else goto badcmd;
	    break;

	case 'P':
	    if (!strcmp(cmd.s, "Partial")) {
		if (!imapd_mailbox) goto nomailbox;
		if (c != ' ') goto missingargs;
		c = getword(imapd_in, &arg1);
		if (c != ' ') goto missingargs;
		c = getword(imapd_in, &arg2);
		if (c != ' ') goto missingargs;
		c = getword(imapd_in, &arg3);
		if (c != ' ') goto missingargs;
		c = getword(imapd_in, &arg4);
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;

		cmd_partial(tag.s, arg1.s, arg2.s, arg3.s, arg4.s);

		/* xxxx snmp_increment(PARTIAL_COUNT, 1); */
	    }
	    else goto badcmd;
	    break;

	case 'R':
	    if (!strcmp(cmd.s, "Rename")) {
		havepartition = 0;
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg1);
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg2);
		if (c == EOF) goto missingargs;
		if (c == ' ') {
		    havepartition = 1;
		    c = getword(imapd_in, &arg3);
		    if (!imparse_isatom(arg3.s)) goto badpartition;
		}
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;
		cmd_rename(tag.s, arg1.s, arg2.s, havepartition ? arg3.s : 0);

		/* xxxx snmp_increment(RENAME_COUNT, 1); */
	    } else if(!strcmp(cmd.s, "Reconstruct")) {
		recursive = 0;
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg1);
		if(c == ' ') {
		    /* Optional RECURSEIVE argument */
		    c = getword(imapd_in, &arg2);
		    if(!imparse_isatom(arg2.s))
			goto extraargs;
		    else if(!strcasecmp(arg2.s, "RECURSIVE"))
			recursive = 1;
		    else
			goto extraargs;
		}
		if(c == '\r') c = prot_getc(imapd_in);
		if(c != '\n') goto extraargs;
		cmd_reconstruct(tag.s, arg1.s, recursive);

		/* snmp_increment(RECONSTRUCT_COUNT, 1); */
	    } 
	    else if (!strcmp(cmd.s, "Rlist")) {
		c = getastring(imapd_in, imapd_out, &arg1);
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg2);
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;
		cmd_list(tag.s, LIST_CHILDREN | LIST_REMOTE, arg1.s, arg2.s);

/* 		snmp_increment(LIST_COUNT, 1); */
	    }
	    else if (!strcmp(cmd.s, "Rlsub")) {
		c = getastring(imapd_in, imapd_out, &arg1);
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg2);
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;
		cmd_list(tag.s, LIST_LSUB | LIST_CHILDREN | LIST_REMOTE,
			 arg1.s, arg2.s);
/* 		snmp_increment(LSUB_COUNT, 1); */
	    } else goto badcmd;
	    break;
	    
	case 'S':
	    if (!strcmp(cmd.s, "Starttls")) {
		if (!tls_enabled()) {
		    /* we don't support starttls */
		    goto badcmd;
		}

		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;

		/* if we've already done SASL fail */
		if (imapd_userid != NULL) {
		    prot_printf(imapd_out, 
	       "%s BAD Can't Starttls after authentication\r\n", tag.s);
		    continue;
		}
		
		/* check if already did a successful tls */
		if (imapd_starttls_done == 1) {
		    prot_printf(imapd_out, 
				"%s BAD Already did a successful Starttls\r\n",
				tag.s);
		    continue;
		}
		cmd_starttls(tag.s, 0);	

		snmp_increment(STARTTLS_COUNT, 1);      
		continue;
	    }
	    if (!imapd_userid) {
		goto nologin;
	    } else if (!strcmp(cmd.s, "Store")) {
		if (!imapd_mailbox) goto nomailbox;
		usinguid = 0;
		if (c != ' ') goto missingargs;
	    store:
		c = getword(imapd_in, &arg1);
		if (c != ' ' || !imparse_issequence(arg1.s)) goto badsequence;
		c = getword(imapd_in, &arg2);
		if (c != ' ') goto missingargs;

		cmd_store(tag.s, arg1.s, arg2.s, usinguid);

		snmp_increment(STORE_COUNT, 1);
	    }
	    else if (!strcmp(cmd.s, "Select")) {
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg1);
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;

		cmd_select(tag.s, cmd.s, arg1.s);

		snmp_increment(SELECT_COUNT, 1);
	    }
	    else if (!strcmp(cmd.s, "Search")) {
		if (!imapd_mailbox) goto nomailbox;
		usinguid = 0;
		if (c != ' ') goto missingargs;
	    search:

		cmd_search(tag.s, usinguid);

		snmp_increment(SEARCH_COUNT, 1);
	    }
	    else if (!strcmp(cmd.s, "Subscribe")) {
		if (c != ' ') goto missingargs;
		havenamespace = 0;
		c = getastring(imapd_in, imapd_out, &arg1);
		if (c == ' ') {
		    havenamespace = 1;
		    c = getastring(imapd_in, imapd_out, &arg2);
		}
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;
		if (havenamespace) {
		    cmd_changesub(tag.s, arg1.s, arg2.s, 1);
		}
		else {
		    cmd_changesub(tag.s, (char *)0, arg1.s, 1);
		}
		snmp_increment(SUBSCRIBE_COUNT, 1);
	    }		
	    else if (!strcmp(cmd.s, "Setacl")) {
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg1);
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg2);
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg3);
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;
		cmd_setacl(tag.s, arg1.s, arg2.s, arg3.s);

		snmp_increment(SETACL_COUNT, 1);
	    }
	    else if (!strcmp(cmd.s, "Setannotation")) {
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg1);
		if (c != ' ') goto missingargs;

		cmd_setannotation(tag.s, arg1.s);

		snmp_increment(SETANNOTATION_COUNT, 1);
	    }
	    else if (!strcmp(cmd.s, "Setquota")) {
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg1);
		if (c != ' ') goto missingargs;
		cmd_setquota(tag.s, arg1.s);

		snmp_increment(SETQUOTA_COUNT, 1);
	    }
	    else if (!strcmp(cmd.s, "Sort")) {
		if (!imapd_mailbox) goto nomailbox;
		usinguid = 0;
		if (c != ' ') goto missingargs;
	    sort:
		cmd_sort(tag.s, usinguid);

		snmp_increment(SORT_COUNT, 1);
	    }
	    else if (!strcmp(cmd.s, "Status")) {
		if (c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg1);
		if (c != ' ') goto missingargs;
		cmd_status(tag.s, arg1.s);

		snmp_increment(STATUS_COUNT, 1);
	    }
	    else goto badcmd;
	    break;

	case 'T':
	    if (!strcmp(cmd.s, "Thread")) {
		if (!imapd_mailbox) goto nomailbox;
		usinguid = 0;
		if (c != ' ') goto missingargs;
	    thread:
		cmd_thread(tag.s, usinguid);

		snmp_increment(THREAD_COUNT, 1);
	    }
	    else goto badcmd;
	    break;

	case 'U':
	    if (!strcmp(cmd.s, "Uid")) {
		if (!imapd_mailbox) goto nomailbox;
		usinguid = 1;
		if (c != ' ') goto missingargs;
		c = getword(imapd_in, &arg1);
		if (c != ' ') goto missingargs;
		lcase(arg1.s);
		if (!strcmp(arg1.s, "fetch")) {
		    goto fetch;
		}
		else if (!strcmp(arg1.s, "store")) {
		    goto store;
		}
		else if (!strcmp(arg1.s, "search")) {
		    goto search;
		}
		else if (!strcmp(arg1.s, "sort")) {
		    goto sort;
		}
		else if (!strcmp(arg1.s, "thread")) {
		    goto thread;
		}
		else if (!strcmp(arg1.s, "copy")) {
		    goto copy;
		}
		else if (!strcmp(arg1.s, "expunge")) {
		    c = getword(imapd_in, &arg1);
		    if (!imparse_issequence(arg1.s)) goto badsequence;
		    if (c == '\r') c = prot_getc(imapd_in);
		    if (c != '\n') goto extraargs;
		    cmd_expunge(tag.s, arg1.s);

		    snmp_increment(EXPUNGE_COUNT, 1);
		}
		else {
		    prot_printf(imapd_out, "%s BAD Unrecognized UID subcommand\r\n", tag.s);
		    eatline(imapd_in, c);
		}
	    }
	    else if (!strcmp(cmd.s, "Unsubscribe")) {
		if (c != ' ') goto missingargs;
		havenamespace = 0;
		c = getastring(imapd_in, imapd_out, &arg1);
		if (c == ' ') {
		    havenamespace = 1;
		    c = getastring(imapd_in, imapd_out, &arg2);
		}
		if (c == EOF) goto missingargs;
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;
		if (havenamespace) {
		    cmd_changesub(tag.s, arg1.s, arg2.s, 0);
		}
		else {
		    cmd_changesub(tag.s, (char *)0, arg1.s, 0);
		}

		snmp_increment(UNSUBSCRIBE_COUNT, 1);
	    }		
	    else if (!strcmp(cmd.s, "Unselect")) {
		if (!imapd_mailbox) goto nomailbox;
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;
		cmd_unselect(tag.s);

		snmp_increment(UNSELECT_COUNT, 1);
	    }
	    else if (!strcmp(cmd.s, "Undump")) {
		if(c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg1);

		/* we want to get a list at this point */
		if(c != ' ') goto missingargs;
		
		cmd_undump(tag.s, arg1.s);
	    /*	snmp_increment(UNDUMP_COUNT, 1);*/
	    }
	    else goto badcmd;
	    break;

	case 'X':
	    if (!strcmp(cmd.s, "Xfer")) {
		int havepartition = 0;
		
		/* Mailbox */
		if(c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg1);

		/* Dest Server */
		if(c != ' ') goto missingargs;
		c = getastring(imapd_in, imapd_out, &arg2);

		if(c == ' ') {
		    /* Dest Partition */
		    c = getastring(imapd_in, imapd_out, &arg3);
		    if (!imparse_isatom(arg3.s)) goto badpartition;
		    havepartition = 1;
		}
		
		if (c == '\r') c = prot_getc(imapd_in);
		if (c != '\n') goto extraargs;

		cmd_xfer(tag.s, arg1.s, arg2.s,
			 (havepartition ? arg3.s : NULL));
	    /*	snmp_increment(XFER_COUNT, 1);*/
	    }
	    else goto badcmd;
	    break;

	default:
	badcmd:
	    prot_printf(imapd_out, "%s BAD Unrecognized command\r\n", tag.s);
	    eatline(imapd_in, c);
	}

	continue;

    nologin:
	prot_printf(imapd_out, "%s BAD Please login first\r\n", tag.s);
	eatline(imapd_in, c);
	continue;

    nomailbox:
	prot_printf(imapd_out, "%s BAD Please select a mailbox first\r\n", tag.s);
	eatline(imapd_in, c);
	continue;

    missingargs:
	prot_printf(imapd_out, "%s BAD Missing required argument to %s\r\n", tag.s, cmd.s);
	eatline(imapd_in, c);
	continue;

    extraargs:
	prot_printf(imapd_out, "%s BAD Unexpected extra arguments to %s\r\n", tag.s, cmd.s);
	eatline(imapd_in, c);
	continue;

    badsequence:
	prot_printf(imapd_out, "%s BAD Invalid sequence in %s\r\n", tag.s, cmd.s);
	eatline(imapd_in, c);
	continue;

    badpartition:
	prot_printf(imapd_out, "%s BAD Invalid partition name in %s\r\n",
	       tag.s, cmd.s);
	eatline(imapd_in, c);
	continue;
    }
}

/*
 * Perform a LOGIN command
 */
void cmd_login(char *tag, char *user)
{
    char userbuf[MAX_MAILBOX_NAME+1];
    unsigned userlen;
    const char *canon_user = userbuf;
    char c;
    struct buf passwdbuf;
    char *passwd;
    const char *reply = NULL;
    int plaintextloginpause;
    int r;
    char mailboxname[ MAX_MAILBOX_NAME + 1 ] = "\0";
    struct od_user_opts	useropts;
    
    if (imapd_userid) {
	eatline(imapd_in, ' ');
	prot_printf(imapd_out, "%s BAD Already logged in\r\n", tag);
	return;
    }

	/* imap_auth_clear */
	if ( config_getswitch( IMAPOPT_IMAP_AUTH_CLEAR ) == 0 )
	{
		eatline(imapd_in, ' ');
		prot_printf( imapd_out, "%s NO Auth method not enabled\r\n", tag );
		return;
	}

	odGetUserOpts( user, &useropts );
	if ( useropts.fRecName[ 0 ] == '\0' )
	{
		strlcpy( useropts.fRecName, user, sizeof(useropts.fRecName) );
	}
	
    r = imapd_canon_user(imapd_saslconn, NULL, useropts.fRecName, 0,
			 SASL_CU_AUTHID | SASL_CU_AUTHZID, NULL,
			 userbuf, sizeof(userbuf), &userlen);

    if (r) {
	syslog(LOG_NOTICE, "badlogin: %s plaintext %s invalid user",
	       imapd_clienthost, beautify_string(user));
	prot_printf(imapd_out, "%s NO %s\r\n", tag, 
		    error_message(IMAP_INVALID_USER));
	return;
    }

    /* possibly disallow login */
    if ((imapd_starttls_done == 0) &&
	(config_getswitch(IMAPOPT_ALLOWPLAINTEXT) == 0) &&
	!is_userid_anonymous(canon_user)) {
	eatline(imapd_in, ' ');
	prot_printf(imapd_out, "%s NO Login only available under a layer\r\n",
		    tag);
	return;
    }

    memset(&passwdbuf,0,sizeof(struct buf));
    c = getastring(imapd_in, imapd_out, &passwdbuf);

    if(c == '\r') c = prot_getc(imapd_in);
    if (c != '\n') {
	freebuf(&passwdbuf);
	prot_printf(imapd_out,
		    "%s BAD Unexpected extra arguments to LOGIN\r\n",
		    tag);
	eatline(imapd_in, c);
	return;
    }

    passwd = passwdbuf.s;

    if ( !is_userid_anonymous( user ) )
	{
		/* do we know this user */
		if ( useropts.fRecName[ 0 ] == '\0' )
		{
			syslog( LOG_NOTICE, "badlogin from: %s plaintext user: %s. unknown user",
					imapd_clienthost, beautify_string( user ) );

			prot_printf( imapd_out, "%s NO unknown user or bad password\r\n", tag);

			freebuf( &passwdbuf );

			return;
		}

		/* is their account enabled */
		if ( useropts.fAcctState != eAcctEnabled )
		{
			if ( useropts.fAcctState == eAcctNotMember )
			{
				syslog( LOG_NOTICE, "badlogin from: %s. plaintext user: %s. service ACL is not enabled for this user",
						imapd_clienthost, beautify_string( user ) );

				prot_printf( imapd_out, "%s NO Mail service ACL is not enabled for this user\r\n", tag );
			}
			else if ( useropts.fAcctState == eAcctForwarded )
			{
				syslog( LOG_NOTICE, "badlogin from: %s. plaintext user: %s. auto-forwarding is enabled for this user",
						imapd_clienthost, beautify_string( user ) );

				prot_printf( imapd_out, "%s NO Auto-forwarding is enabled for this user\r\n", tag);
			}
			else
			{
				syslog( LOG_NOTICE, "badlogin from: %s. plaintext user: %s. mail is not enabled for this user",
						imapd_clienthost, beautify_string( user ) );

				prot_printf( imapd_out, "%s NO Mail is not enabled for this user\r\n", tag);
			}

			freebuf( &passwdbuf );

			return;
		}

		/* is their imap login enabled */
		if ( useropts.fIMAPLogin != eAcctProtocolEnabled )
		{
			syslog( LOG_NOTICE, "badlogin from: %s. plaintext user: %s. IMAP access is not enabled for this user",
					imapd_clienthost, beautify_string( user ) );

			prot_printf( imapd_out, "%s NO IMAP access is not enabled for this user\r\n", tag );

			freebuf( &passwdbuf );

			return;
		}
	}

    if (is_userid_anonymous(canon_user)) {
	if (config_getswitch(IMAPOPT_ALLOWANONYMOUSLOGIN)) {
	    passwd = beautify_string(passwd);
	    if (strlen(passwd) > 500) passwd[500] = '\0';
	    syslog(LOG_NOTICE, "login: %s anonymous %s",
		   imapd_clienthost, passwd);
	    reply = "Anonymous access granted";
	    imapd_userid = xstrdup("anonymous");
	}
	else {
	    syslog(LOG_NOTICE, "badlogin: %s anonymous login refused",
		   imapd_clienthost);
	    prot_printf(imapd_out, "%s NO %s\r\n", tag,
		   error_message(IMAP_ANONYMOUS_NOT_PERMITTED));
	    freebuf(&passwdbuf);
	    return;
	}
    }
    else if ( (config_getswitch(IMAPOPT_APPLE_AUTH) == 0) &&
			  ((r = sasl_checkpass(imapd_saslconn,
				 canon_user,
				 strlen(canon_user),
				 passwd,
				 strlen(passwd))) != SASL_OK) ) {
	syslog(LOG_NOTICE, "badlogin: %s plaintext %s %s",
	       imapd_clienthost, canon_user, sasl_errdetail(imapd_saslconn));

	sleep(3);

	if ((reply = sasl_errstring(r, NULL, NULL)) != NULL) {
	    prot_printf(imapd_out, "%s NO Login failed: %s\r\n", tag, reply);
	} else {
	    prot_printf(imapd_out, "%s NO Login failed: %d\r\n", tag, r);
	}

	snmp_increment_args(AUTHENTICATION_NO, 1,
			    VARIABLE_AUTH, 0 /* hash_simple("LOGIN") */,
			    VARIABLE_LISTEND);
    	freebuf(&passwdbuf);
	return;
    }
	else if ( config_getswitch( IMAPOPT_APPLE_AUTH ) )
	{
		const char *canon_user = auth_canonifyid( useropts.fRecName, 0 );

		if ( r = odCheckPass( canon_user, passwd ) )
		{
			syslog( LOG_NOTICE, "badlogin from: %s. login user: %s (error = %d)",
					imapd_clienthost, canon_user, r );
		
			sleep( 3 );

			prot_printf( imapd_out, "%s NO Login failed.\r\n", tag );

			snmp_increment_args( AUTHENTICATION_NO, 1,
								 VARIABLE_AUTH, 0,
								 VARIABLE_LISTEND );

			freebuf(&passwdbuf);

			return;
		}
		else
		{
			reply = "User logged in";
			imapd_userid = xstrdup(canon_user);
			snmp_increment_args(AUTHENTICATION_YES, 1,
						VARIABLE_AUTH, 0 /*hash_simple("LOGIN") */, 
						VARIABLE_LISTEND);
			syslog(LOG_NOTICE, "login: %s %s plaintext%s %s", imapd_clienthost,
				   canon_user, imapd_starttls_done ? "+TLS" : "", 
				   reply ? reply : "");

			plaintextloginpause = config_getint(IMAPOPT_PLAINTEXTLOGINPAUSE);
			if (plaintextloginpause != 0 && !imapd_starttls_done)
			{
				/* Apply penalty only if not under layer */
				sleep(plaintextloginpause);
			}
		}
	}
    else {
	r = sasl_getprop(imapd_saslconn, SASL_USERNAME,
			 (const void **) &canon_user);

	if(r != SASL_OK) {
	    if ((reply = sasl_errstring(r, NULL, NULL)) != NULL) {
		prot_printf(imapd_out, "%s NO Login failed: %s\r\n",
			    tag, reply);
	    } else {
		prot_printf(imapd_out, "%s NO Login failed: %d\r\n", tag, r);
	    }

	    snmp_increment_args(AUTHENTICATION_NO, 1,
				VARIABLE_AUTH, 0 /* hash_simple("LOGIN") */,
				VARIABLE_LISTEND);
	    freebuf(&passwdbuf);
	    return;
	}

	reply = "User logged in";
	imapd_userid = xstrdup(canon_user);
	snmp_increment_args(AUTHENTICATION_YES, 1,
			    VARIABLE_AUTH, 0 /*hash_simple("LOGIN") */, 
			    VARIABLE_LISTEND);
	syslog(LOG_NOTICE, "login: %s %s%s plaintext%s %s", imapd_clienthost,
	       imapd_userid, imapd_magicplus ? imapd_magicplus : "",
	       imapd_starttls_done ? "+TLS" : "", 
	       reply ? reply : "");

	plaintextloginpause = config_getint(IMAPOPT_PLAINTEXTLOGINPAUSE);
	if (plaintextloginpause != 0 && !imapd_starttls_done) {
	    /* Apply penalty only if not under layer */
	    sleep(plaintextloginpause);
	}
    }
    
    imapd_authstate = auth_newstate(imapd_userid);

    imapd_userisadmin = global_authisa(imapd_authstate, IMAPOPT_ADMINS);

    prot_printf(imapd_out, "%s OK %s\r\n", tag, reply);

    /* Create telemetry log */
    imapd_logfd = telemetry_log(imapd_userid, imapd_in, imapd_out, 0);

    /* Set namespace */
    if ((r = mboxname_init_namespace(&imapd_namespace,
				     imapd_userisadmin || imapd_userisproxyadmin)) != 0) {
	syslog(LOG_ERR, error_message(r));
	fatal(error_message(r), EC_CONFIG);
    }

    /* Translate any separators in userid */
    mboxname_hiersep_tointernal(&imapd_namespace, imapd_userid,
				config_virtdomains ?
				strcspn(imapd_userid, "@") : 0);

	/* Create INBOX on login */
	if ( !imapd_userisadmin )
	{
		r = (*imapd_namespace.mboxname_tointernal)(&imapd_namespace, "INBOX", imapd_userid, mailboxname);
		if ( !r )
		{
			char *partition	= NULL;
			if ( useropts.fAltDataLoc[ 0 ] != '\0' )
			{
				partition = useropts.fAltDataLoc;
			}
			r = mboxlist_createmailbox( mailboxname, MAILBOX_FORMAT_NORMAL,  partition, 1, imapd_userid, imapd_authstate, 0, 0, 0 );
			if ( r && (r !=IMAP_MAILBOX_EXISTS) )
			{
				syslog( LOG_DEBUG, "mboxlist_createmailbox error(%d) for mailbox(%s)", r, mailboxname);
			}
		}
		if ( r == IMAP_MAILBOX_EXISTS )
		{
			if ( useropts.fDiskQuota == 0 )
			{
				/* make sure that quotas are set so that the /quota tool works */
				mboxlist_setquota( mailboxname, INT32_MAX, 0 );
				syslog( LOG_DEBUG, "quota set to \"unlimited\" for mailbox %s ", mailboxname);
			}
			else
			{
				mboxlist_setquota( mailboxname, useropts.fDiskQuota * 1024, 0 );
				syslog( LOG_DEBUG, "quota set to %lu for mailbox %s ", useropts.fDiskQuota * 1024, mailboxname);
			}
		}
	}

    proc_register("imapd", imapd_clienthost, imapd_userid, (char *)0);

    freebuf(&passwdbuf);
    return;
}

/*
 * Perform an AUTHENTICATE command
 */
void
cmd_authenticate(char *tag, char *authtype, char *resp)
{
    int sasl_result;
    
    const int *ssfp;
    char *ssfmsg=NULL;
    struct od_user_opts	useropts;
    const char *auth_user;

    const char *canon_user;

    int r;

    char mailboxname[ MAX_MAILBOX_NAME + 1 ] = "\0";

	if ( config_getswitch( IMAPOPT_APPLE_AUTH ) == 0 )
	{
		r = saslserver(imapd_saslconn, authtype, resp, "", "+ ", "",
			   imapd_in, imapd_out, &sasl_result, NULL);

		if (r) {
		const char *errorstring = NULL;

		switch (r) {
		case IMAP_SASL_CANCEL:
			prot_printf(imapd_out,
				"%s BAD Client canceled authentication\r\n", tag);
			break;
		case IMAP_SASL_PROTERR:
			errorstring = prot_error(imapd_in);

			prot_printf(imapd_out,
				"%s NO Error reading client response: %s\r\n",
				tag, errorstring ? errorstring : "");
			break;
		default: 
			/* failed authentication */
			errorstring = sasl_errstring(sasl_result, NULL, NULL);

			syslog(LOG_NOTICE, "badlogin: %s %s [%s]",
			   imapd_clienthost, authtype, sasl_errdetail(imapd_saslconn));

			snmp_increment_args(AUTHENTICATION_NO, 1,
					VARIABLE_AUTH, 0, /* hash_simple(authtype) */ 
					VARIABLE_LISTEND);
			sleep(3);

			if (errorstring) {
			prot_printf(imapd_out, "%s NO %s\r\n", tag, errorstring);
			} else {
			prot_printf(imapd_out, "%s NO Error authenticating\r\n", tag);
			}
		}

		reset_saslconn(&imapd_saslconn);
		return;
		}

		/* successful authentication */

		/* get the userid from SASL --- already canonicalized from
		 * mysasl_proxy_policy()
		 */
		sasl_result = sasl_getprop(imapd_saslconn, SASL_USERNAME,
					   (const void **) &canon_user);
		if (sasl_result != SASL_OK) {
		prot_printf(imapd_out, "%s NO weird SASL error %d SASL_USERNAME\r\n", 
				tag, sasl_result);
		syslog(LOG_ERR, "weird SASL error %d getting SASL_USERNAME", 
			   sasl_result);
		reset_saslconn(&imapd_saslconn);
		return;
		}
	}
	else
	{
		if ( (strcasecmp( authtype, "gssapi" ) == 0) && 
			 (config_getswitch( IMAPOPT_IMAP_AUTH_GSSAPI ) == 0 ) )
		{
			r = 1;
		}
		else if ( (strcasecmp( authtype, "cram-md5" ) == 0) &&
				  (config_getswitch( IMAPOPT_IMAP_AUTH_CRAM_MD5 ) == 0) )
		{
			r = 1;
		}
		else if ( (strcasecmp( authtype, "plain" ) == 0) &&
				  (config_getswitch( IMAPOPT_IMAP_AUTH_PLAIN ) == 0) )
		{
			r = 1;
		}
		else if ( (strcasecmp( authtype, "login" ) == 0) &&
				  (config_getswitch( IMAPOPT_IMAP_AUTH_LOGIN ) == 0) )
		{
			r = 1;
		}

		if ( r == 1 )
		{
			prot_printf( imapd_out, "%s NO Auth method (%s) not enabled\r\n", tag, authtype );
			return;
		}

		r = odDoAuthenticate( authtype, NULL, "+ ", kXMLIMAP_Principal, imapd_in, imapd_out, (char **)&auth_user );
		if ( r )
		{
			switch ( r )
			{
				case ODA_AUTH_CANCEL:
					prot_printf(imapd_out, "%s NO Client canceled authentication\r\n", tag );
					break;

				case ODA_PROTOCOL_ERROR:
					prot_printf( imapd_out, "%s NO Error reading client response\r\n", tag );
					break;

				default:
					syslog( LOG_NOTICE, "badlogin: %s %s", imapd_clienthost, authtype );

					snmp_increment_args( AUTHENTICATION_NO, 1, VARIABLE_AUTH, 0, /* hash_simple(authtype) */  VARIABLE_LISTEND );

					prot_printf( imapd_out, "%s NO Error authenticating\r\n", tag );

					sleep( 3 );
			}

			return;
		}

		odGetUserOpts( auth_user, &useropts );

		/* do we know this user */
		if ( useropts.fRecName[ 0 ] == '\0' )
		{
			syslog( LOG_NOTICE, "badlogin from: %s plaintext user: %s. unknown user",
					imapd_clienthost, beautify_string( auth_user ) );

			prot_printf( imapd_out, "%s NO unknown user or bad password\r\n", tag);

			return;
		}

		/* is their account enabled */
		if ( useropts.fAcctState != eAcctEnabled )
		{
			if ( useropts.fAcctState == eAcctNotMember )
			{
				syslog( LOG_NOTICE, "badlogin from: %s. plaintext user: %s. service ACL is not enabled for this user",
						imapd_clienthost, beautify_string( auth_user ) );

				prot_printf( imapd_out, "%s NO Mail service ACL is not enabled for this user\r\n", tag );
			}
			else if ( useropts.fAcctState == eAcctForwarded )
			{
				syslog( LOG_NOTICE, "badlogin from: %s. plaintext user: %s. auto-forwarding is enabled for this user",
						imapd_clienthost, beautify_string( auth_user ) );

				prot_printf( imapd_out, "%s NO Auto-forwarding is enabled for this user\r\n", tag);
			}
			else
			{
				syslog( LOG_NOTICE, "badlogin from: %s. plaintext user: %s. mail is not enabled for this user",
						imapd_clienthost, beautify_string( auth_user ) );

				prot_printf( imapd_out, "%s NO Mail is not enabled for this user\r\n", tag);
			}

			return;
		}

		/* is their imap login enabled */
		if ( useropts.fIMAPLogin != eAcctProtocolEnabled )
		{
			syslog( LOG_NOTICE, "badlogin from: %s. plaintext user: %s. IMAP access is not enabled for this user",
					imapd_clienthost, beautify_string( auth_user ) );

			prot_printf( imapd_out, "%s NO IMAP access is not enabled for this user\r\n", tag );

			return;
		}

		/* successful authentication */
		canon_user = auth_canonifyid( useropts.fRecName, 0 );
	}

    /* If we're proxying, the authzid may contain a magic plus,
       so re-canonify it */
    if (config_getswitch(IMAPOPT_IMAPMAGICPLUS) && strchr(canon_user, '+')) {
	char userbuf[MAX_MAILBOX_NAME+1];
	unsigned userlen;

	sasl_result = imapd_canon_user(imapd_saslconn, NULL, canon_user, 0,
				       SASL_CU_AUTHID | SASL_CU_AUTHZID,
				       NULL, userbuf, sizeof(userbuf), &userlen);
	if (sasl_result != SASL_OK) {
	    prot_printf(imapd_out, 
			"%s NO SASL canonification error %d\r\n", 
			tag, sasl_result);
	    reset_saslconn(&imapd_saslconn);
	    return;
	}

	imapd_userid = xstrdup(userbuf);
    } else {
	imapd_userid = xstrdup(canon_user);
    }

    proc_register("imapd", imapd_clienthost, imapd_userid, (char *)0);

    syslog(LOG_NOTICE, "login: %s %s%s %s%s %s", imapd_clienthost,
	   imapd_userid, imapd_magicplus ? imapd_magicplus : "",
	   authtype, imapd_starttls_done ? "+TLS" : "", "User logged in");

    sasl_getprop(imapd_saslconn, SASL_SSF, (const void **) &ssfp);

    /* really, we should be doing a sasl_getprop on SASL_SSF_EXTERNAL,
       but the current libsasl doesn't allow that. */
    if (imapd_starttls_done) {
	switch(*ssfp) {
	case 0: ssfmsg = "tls protection"; break;
	case 1: ssfmsg = "tls plus integrity protection"; break;
	default: ssfmsg = "tls plus privacy protection"; break;
	}
    } else {
	switch(*ssfp) {
	case 0: ssfmsg = "no protection"; break;
	case 1: ssfmsg = "integrity protection"; break;
	default: ssfmsg = "privacy protection"; break;
	}
    }

    snmp_increment_args(AUTHENTICATION_YES, 1,
			VARIABLE_AUTH, 0, /* hash_simple(authtype) */
			VARIABLE_LISTEND);

    prot_printf(imapd_out, "%s OK Success (%s)\r\n", tag, ssfmsg);

    prot_setsasl(imapd_in,  imapd_saslconn);
    prot_setsasl(imapd_out, imapd_saslconn);

    /* Create telemetry log */
    imapd_logfd = telemetry_log(imapd_userid, imapd_in, imapd_out, 0);

    /* Set namespace */
    if ((r = mboxname_init_namespace(&imapd_namespace,
				     imapd_userisadmin || imapd_userisproxyadmin)) != 0) {
	syslog(LOG_ERR, error_message(r));
	fatal(error_message(r), EC_CONFIG);
    }

    /* Translate any separators in userid */
    mboxname_hiersep_tointernal(&imapd_namespace, imapd_userid,
				config_virtdomains ?
				strcspn(imapd_userid, "@") : 0);

    imapd_authstate = auth_newstate(imapd_userid);

	imapd_userisadmin = global_authisa( imapd_authstate, IMAPOPT_ADMINS );

	/* Create INBOX on login */
	if ( !imapd_userisadmin )
	{
		r = (*imapd_namespace.mboxname_tointernal)(&imapd_namespace, "INBOX", imapd_userid, mailboxname);
		if ( !r )
		{
			char *partition	= NULL;
			struct od_user_opts	useropts;
			odGetUserOpts( imapd_userid, &useropts );
			if ( useropts.fAltDataLoc[ 0 ] != '\0' )
			{
				partition = useropts.fAltDataLoc;
			}
			r = mboxlist_createmailbox( mailboxname, MAILBOX_FORMAT_NORMAL, partition, 1, imapd_userid, imapd_authstate, 0, 0, 0 );
			if ( r && (r !=IMAP_MAILBOX_EXISTS) )
			{
				syslog( LOG_DEBUG, "mboxlist_createmailbox error(%d) for mailbox(%s)", r, mailboxname);
			}
		}
		if ( r == IMAP_MAILBOX_EXISTS )
		{
			if ( useropts.fDiskQuota == 0 )
			{
				/* make sure that quotas are set so that the /quota tool works */
				mboxlist_setquota( mailboxname, INT32_MAX, 0 );
				syslog( LOG_DEBUG, "quota set to \"unlimited\" for mailbox %s ", mailboxname);
			}
			else
			{
				mboxlist_setquota( mailboxname, useropts.fDiskQuota * 1024, 0 );
				syslog( LOG_DEBUG, "quota set to %lu for mailbox %s ", useropts.fDiskQuota * 1024, mailboxname);
			}
		}
	}

    return;
}

/*
 * Perform a NOOP command
 */
void
cmd_noop(char *tag, char *cmd __attribute__((unused)))
{
    if (imapd_mailbox) {
	index_check(imapd_mailbox, 0, 1);
    }
    prot_printf(imapd_out, "%s OK %s\r\n", tag,
		error_message(IMAP_OK_COMPLETED));
}

/*
 * Parse and perform an ID command.
 *
 * the command has been parsed up to the parameter list.
 *
 * we only allow one ID in non-authenticated state from a given client.
 * we only allow MAXIDFAILED consecutive failed IDs from a given client.
 * we only record MAXIDLOG ID responses from a given client.
 */
void cmd_id(char *tag)
{
    static int did_id = 0;
    static int failed_id = 0;
    static int logged_id = 0;
    int error = 0;
    int c = EOF, npair = 0;
    static struct buf arg, field;
    struct attvaluelist *params = 0;

    /* check if we've already had an ID in non-authenticated state */
    if (!imapd_userid && did_id) {
	prot_printf(imapd_out,
		    "%s NO Only one Id allowed in non-authenticated state\r\n",
		    tag);
	eatline(imapd_in, c);
	return;
    }

    /* check if we've had too many failed IDs in a row */
    if (failed_id >= MAXIDFAILED) {
	prot_printf(imapd_out, "%s NO Too many (%u) invalid Id commands\r\n",
		    tag, failed_id);
	eatline(imapd_in, c);
	return;
    }

    /* ok, accept parameter list */
    c = getword(imapd_in, &arg);
    /* check for "NIL" or start of parameter list */
    if (strcasecmp(arg.s, "NIL") && c != '(') {
	prot_printf(imapd_out, "%s BAD Invalid parameter list in Id\r\n", tag);
	eatline(imapd_in, c);
	failed_id++;
	return;
    }

    /* parse parameter list */
    if (c == '(') {
	for (;;) {
	    if (c == ')') {
		/* end of string/value pairs */
		break;
	    }

	    /* get field name */
	    c = getstring(imapd_in, imapd_out, &field);
	    if (c != ' ') {
		prot_printf(imapd_out,
			    "%s BAD Invalid/missing field name in Id\r\n",
			    tag);
		error = 1;
		break;
	    }

	    /* get field value */
	    c = getnstring(imapd_in, imapd_out, &arg);
	    if (c != ' ' && c != ')') {
		prot_printf(imapd_out,
			    "%s BAD Invalid/missing value in Id\r\n",
			    tag);
		error = 1;
		break;
	    }

	    /* ok, we're anal, but we'll still process the ID command */
	    if (strlen(field.s) > MAXIDFIELDLEN) {
		prot_printf(imapd_out, 
			    "%s BAD field longer than %u octets in Id\r\n",
			    tag, MAXIDFIELDLEN);
		error = 1;
		break;
	    }
	    if (strlen(arg.s) > MAXIDVALUELEN) {
		prot_printf(imapd_out,
			    "%s BAD value longer than %u octets in Id\r\n",
			    tag, MAXIDVALUELEN);
		error = 1;
		break;
	    }
	    if (++npair > MAXIDPAIRS) {
		prot_printf(imapd_out,
			    "%s BAD too many (%u) field-value pairs in ID\r\n",
			    tag, MAXIDPAIRS);
		error = 1;
		break;
	    }
	    
	    /* ok, we're happy enough */
	    appendattvalue(&params, field.s, arg.s);
	}

	if (error || c != ')') {
	    /* erp! */
	    eatline(imapd_in, c);
	    freeattvalues(params);
	    failed_id++;
	    return;
	}
	c = prot_getc(imapd_in);
    }

    /* check for CRLF */
    if (c == '\r') c = prot_getc(imapd_in);
    if (c != '\n') {
	prot_printf(imapd_out,
		    "%s BAD Unexpected extra arguments to Id\r\n", tag);
	eatline(imapd_in, c);
	freeattvalues(params);
	failed_id++;
	return;
    }

    /* log the client's ID string.
       eventually this should be a callback or something. */
    if (npair && logged_id < MAXIDLOG) {
	char logbuf[MAXIDLOGLEN + 1] = "";
	struct attvaluelist *pptr;

	for (pptr = params; pptr; pptr = pptr->next) {
	    /* should we check for and format literals here ??? */
	    snprintf(logbuf + strlen(logbuf), MAXIDLOGLEN - strlen(logbuf),
		     " \"%s\" ", pptr->attrib);
	    if (!strcmp(pptr->value, "NIL"))
		snprintf(logbuf + strlen(logbuf), MAXIDLOGLEN - strlen(logbuf),
			 "NIL");
	    else
		snprintf(logbuf + strlen(logbuf), MAXIDLOGLEN - strlen(logbuf),
			"\"%s\"", pptr->value);
	}

	syslog(LOG_INFO, "client id:%s", logbuf);

	logged_id++;
    }

    freeattvalues(params);

    /* spit out our ID string.
       eventually this might be configurable. */
    if (config_getswitch(IMAPOPT_IMAPIDRESPONSE)) {
	id_response(imapd_out);
	prot_printf(imapd_out, ")\r\n");
    }
    else
	prot_printf(imapd_out, "* ID NIL\r\n");

    prot_printf(imapd_out, "%s OK %s\r\n", tag,
		error_message(IMAP_OK_COMPLETED));

    failed_id = 0;
    did_id = 1;
}

/*
 * Perform an IDLE command
 */
void cmd_idle(char *tag)
{
    int c;
    static struct buf arg;

    /* Setup for doing mailbox updates */
    if (!idle_init(imapd_mailbox, idle_update)) {
	prot_printf(imapd_out, 
		    "%s NO cannot start idling\r\n", tag);
	return;
    }

    /* Tell client we are idling and waiting for end of command */
    prot_printf(imapd_out, "+ idling\r\n");
    prot_flush(imapd_out);

    /* Get continuation data */
    c = getword(imapd_in, &arg);
    if (c != EOF) {
	if (!strcasecmp(arg.s, "Done") &&
	    (c = (c == '\r') ? prot_getc(imapd_in) : c) == '\n') {
	    prot_printf(imapd_out, "%s OK %s\r\n", tag,
			error_message(IMAP_OK_COMPLETED));
	}

	else {
	    prot_printf(imapd_out, 
			"%s BAD Invalid Idle continuation\r\n", tag);
	    eatline(imapd_in, c);
	}
    }

    /* Do any necessary cleanup */
    idle_done(imapd_mailbox);

    return;
}

/* Send unsolicited untagged responses to the client */
void idle_update(idle_flags_t flags)
{
    if ((flags & IDLE_MAILBOX) && imapd_mailbox)
	index_check(imapd_mailbox, 0, 1);

    if (flags & IDLE_ALERT) {
	char shut[1024];
	if (! imapd_userisadmin && shutdown_file(shut, sizeof(shut))) {
	    char *p;
	    for (p = shut; *p == '['; p++); /* can't have [ be first char */
	    prot_printf(imapd_out, "* BYE [ALERT] %s\r\n", p);
	    shut_down(0);
	}
    }

    prot_flush(imapd_out);
}

/*
 * Perform a CAPABILITY command
 */
void cmd_capability(char *tag)
{
    const char *sasllist; /* the list of SASL mechanisms */
    unsigned mechcount;

    if (imapd_mailbox) {
	index_check(imapd_mailbox, 0, 0);
    }
    prot_printf(imapd_out, "* CAPABILITY " CAPABILITY_STRING);

    if (idle_enabled()) {
	prot_printf(imapd_out, " IDLE");
    }

    if (tls_enabled() && !imapd_starttls_done && !imapd_authstate) {
	prot_printf(imapd_out, " STARTTLS");
    }
    if (imapd_authstate ||
	(!imapd_starttls_done && !config_getswitch(IMAPOPT_ALLOWPLAINTEXT))) {
	prot_printf(imapd_out, " LOGINDISABLED");
    }

    if(config_mupdate_server) {
	prot_printf(imapd_out, " MUPDATE=mupdate://%s/", config_mupdate_server);
    }

    /* add the SASL mechs */
	if ( config_getswitch( IMAPOPT_APPLE_AUTH ) == 0 )
	{
		if (!imapd_authstate &&
		sasl_listmech(imapd_saslconn, NULL, 
				  "AUTH=", " AUTH=", " SASL-IR",
				  &sasllist,
				  NULL, &mechcount) == SASL_OK && mechcount > 0) {
		prot_printf(imapd_out, " %s", sasllist);      
		} else {
		/* else don't show anything */
		}
	}
	else
	{
		if ( config_getswitch( IMAPOPT_IMAP_AUTH_PLAIN ) )
		{
			prot_printf(imapd_out, " AUTH=PLAIN");
		}

		if ( config_getswitch( IMAPOPT_IMAP_AUTH_LOGIN ) )
		{
			prot_printf(imapd_out, " AUTH=LOGIN");
		}

		if ( config_getswitch( IMAPOPT_IMAP_AUTH_CRAM_MD5 ) )
		{
			prot_printf(imapd_out, " AUTH=CRAM-MD5");
		}

		if ( config_getswitch( IMAPOPT_IMAP_AUTH_GSSAPI ) )
		{
			prot_printf(imapd_out, " AUTH=GSSAPI");
		}
	}
#ifdef ENABLE_LISTEXT
    prot_printf(imapd_out, " LISTEXT LIST-SUBSCRIBED");
#endif /* ENABLE_LISTEXT */

#ifdef ENABLE_X_NETSCAPE_HACK
    prot_printf(imapd_out, " X-NETSCAPE");
#endif
    prot_printf(imapd_out, "\r\n%s OK %s\r\n", tag,
		error_message(IMAP_OK_COMPLETED));
}

/*
 * Parse and perform an APPEND command.
 * The command has been parsed up to and including
 * the mailbox name.
 */
static int isokflag(char *s)
{
    if (s[0] == '\\') {
	lcase(s);
	if (!strcmp(s, "\\seen")) return 1;
	if (!strcmp(s, "\\answered")) return 1;
	if (!strcmp(s, "\\flagged")) return 1;
	if (!strcmp(s, "\\draft")) return 1;
	if (!strcmp(s, "\\deleted")) return 1;
	
	/* uh oh, system flag i don't recognize */
	return 0;
    } else {
	/* valid user flag? */
	return imparse_isatom(s);
    }
}

#define FLAGGROW 10
void cmd_append(char *tag, char *name)

{
    int c;
    static struct buf arg;
    char *p;
    time_t now = time(NULL);
    unsigned size, totalsize = 0;
    int sawdigit = 0;
    int isnowait = 0;
    int r, i;
    char mailboxname[MAX_MAILBOX_NAME+1];
    struct appendstate mailbox;
    unsigned long uidvalidity;
    unsigned long firstuid, num;
    const char *parseerr = NULL;
    FILE *f;
    int numalloc = 5;
    struct appendstage *curstage;

    /* See if we can append */
    r = (*imapd_namespace.mboxname_tointernal)(&imapd_namespace, name,
					       imapd_userid, mailboxname);
    if (!r) {
	r = append_check(mailboxname, MAILBOX_FORMAT_NORMAL,
			 imapd_authstate, ACL_INSERT, totalsize);
    }
    if (r) {
	eatline(imapd_in, ' ');
	prot_printf(imapd_out, "%s NO %s%s\r\n",
		    tag,
		    (r == IMAP_MAILBOX_NONEXISTENT &&
		     mboxlist_createmailboxcheck(mailboxname, 0, 0,
						 imapd_userisadmin,
						 imapd_userid, imapd_authstate,
						 (char **)0, (char **)0) == 0)
		    ? "[TRYCREATE] " : "", error_message(r));
	return;
    }

    stage = xmalloc(numalloc * sizeof(struct appendstage *));

    c = ' '; /* just parsed a space */
    /* we loop, to support MULTIAPPEND */
    while (!r && c == ' ') {
	/* Grow the stage array, if necessary */
	if (numstage == numalloc) {
	    /* Avoid integer wrap as arg to xrealloc */
	    if (numalloc > INT_MAX/(2*sizeof(struct appendstage *)))
		goto done;
	    numalloc *= 2;
	    stage = xrealloc(stage, numalloc * sizeof(struct appendstage *));
	}
	curstage = stage[numstage] = xzmalloc(sizeof(struct appendstage));
	numstage++;
	/* Parse flags */
	c = getword(imapd_in, &arg);
	if  (c == '(' && !arg.s[0]) {
	    curstage->nflags = 0;
	    do {
		c = getword(imapd_in, &arg);
		if (!curstage->nflags && !arg.s[0] && c == ')') break; /* empty list */
		if (!isokflag(arg.s)) {
		    parseerr = "Invalid flag in Append command";
		    r = IMAP_PROTOCOL_ERROR;
		    goto done;
		}
		if (curstage->nflags == curstage->flagalloc) {
		    curstage->flagalloc += FLAGGROW;
		    curstage->flag =
			(char **) xrealloc((char *) curstage->flag, 
					   curstage->flagalloc * sizeof(char *));
		}
		curstage->flag[curstage->nflags] = xstrdup(arg.s);
		curstage->nflags++;
	    } while (c == ' ');
	    if (c != ')') {
		parseerr = 
		    "Missing space or ) after flag name in Append command";
		r = IMAP_PROTOCOL_ERROR;
		goto done;
	    }
	    c = prot_getc(imapd_in);
	    if (c != ' ') {
		parseerr = "Missing space after flag list in Append command";
		r = IMAP_PROTOCOL_ERROR;
		goto done;
	    }
	    c = getword(imapd_in, &arg);
	}

	/* Parse internaldate */
	if (c == '\"' && !arg.s[0]) {
	    prot_ungetc(c, imapd_in);
	    c = getdatetime(&(curstage->internaldate));
	    if (c != ' ') {
		parseerr = "Invalid date-time in Append command";
		r = IMAP_PROTOCOL_ERROR;
		goto done;
	    }
	    c = getword(imapd_in, &arg);
	} else {
	    curstage->internaldate = now;
	}

	p = arg.s;
	/* Check for literal8 */
	if (*p == '~') {
	    p++;
	    /* We don't support binary append yet */
	    r = IMAP_NO_UNKNOWN_CTE;
	    goto done;
	}
	if (*p != '{') {
	    parseerr = "Missing required argument to Append command";
	    r = IMAP_PROTOCOL_ERROR;
	    goto done;
	}
	
	/* Read size from literal */
	isnowait = 0;
	size = 0;
	for (++p; *p && isdigit((int) *p); p++) {
	    sawdigit++;
	    size = size*10 + *p - '0';
#if 0
            if (size < 0) {
                lose();
            }
#endif
	}
	if (*p == '+') {
	    isnowait++;
	    p++;
	}
	
	if (c == '\r') {
	    c = prot_getc(imapd_in);
	}
	else {
	    prot_ungetc(c, imapd_in);
	    c = ' ';		/* Force a syntax error */
	}
	
	if (*p != '}' || p[1] || c != '\n' || !sawdigit) {
	    parseerr = "Invalid literal in Append command";
	    r = IMAP_PROTOCOL_ERROR;
	    goto done;
	}

	if (!isnowait) {
	    /* Tell client to send the message */
	    prot_printf(imapd_out, "+ go ahead\r\n");
	    prot_flush(imapd_out);
	}
	
	/* Stage the message */
	f = append_newstage(mailboxname, now, numstage, &(curstage->stage));
	if (f) {
	    totalsize += size;
	    r = message_copy_strict(imapd_in, f, size);
	    fclose(f);
	} else {
	    r = IMAP_IOERROR;
	}
	
	/* if we see a SP, we're trying to append more than one message */

	/* Parse newline terminating command */
	c = prot_getc(imapd_in);
    }

 done:
    if (r) {
	eatline(imapd_in, c);
    } else {
	/* we should be looking at the end of the line */
	if (c == '\r') c = prot_getc(imapd_in);
	if (c != '\n') {
	    parseerr = "junk after literal";
	    r = IMAP_PROTOCOL_ERROR;
	    eatline(imapd_in, c);
	}
    }

    /* Append from the stage(s) */
    if (!r) {
	r = append_setup(&mailbox, mailboxname, MAILBOX_FORMAT_NORMAL,
			 imapd_userid, imapd_authstate, ACL_INSERT, totalsize);
    }
    if (!r) {
	for (i = 0; !r && i < numstage; i++) {
	    r = append_fromstage(&mailbox, stage[i]->stage, stage[i]->internaldate, 
				 (const char **) stage[i]->flag, stage[i]->nflags, 0);
	}

	if (!r) {
	    r = append_commit(&mailbox, totalsize, &uidvalidity, &firstuid, &num);
	} else {
	    append_abort(&mailbox);
	}
    }

    /* Cleanup the stage(s) */
    while (numstage) {
	curstage = stage[--numstage];

	append_removestage(curstage->stage);
	while (curstage->nflags--) {
	    free(curstage->flag[curstage->nflags]);
	}
	if (curstage->flag) free((char *) curstage->flag);
	free(curstage);
    }
    if (stage) free(stage);
    stage = NULL;

    if (imapd_mailbox) {
	index_check(imapd_mailbox, 0, 0);
    }

    if (r == IMAP_PROTOCOL_ERROR && parseerr) {
	prot_printf(imapd_out, "%s BAD %s\r\n", tag, parseerr);
    } else if (r) {
	prot_printf(imapd_out, "%s NO %s%s\r\n",
		    tag,
		    (r == IMAP_MAILBOX_NONEXISTENT &&
		     mboxlist_createmailboxcheck(mailboxname, 0, 0,
						 imapd_userisadmin,
						 imapd_userid, imapd_authstate,
						 (char **)0, (char **)0) == 0)
		    ? "[TRYCREATE] " : "", error_message(r));
    } else {
	/* is this a space seperated list or sequence list? */
	prot_printf(imapd_out, "%s OK [APPENDUID %lu", tag, uidvalidity);
	if (num == 1) {
	    prot_printf(imapd_out, " %lu", firstuid);
	} else {
	    prot_printf(imapd_out, " %lu:%lu", firstuid, firstuid + num - 1);
	}
	prot_printf(imapd_out, "] %s\r\n", error_message(IMAP_OK_COMPLETED));
    }
}

/*
 * Perform a SELECT/EXAMINE/BBOARD command
 */
void cmd_select(char *tag, char *cmd, char *name)
{
    struct mailbox mailbox;
    char mailboxname[MAX_MAILBOX_NAME+1];
    int r = 0;
    double usage;
    int doclose = 0;

    if (imapd_mailbox) {
	index_closemailbox(imapd_mailbox);
	mailbox_close(imapd_mailbox);
	imapd_mailbox = 0;
    }

    if (cmd[0] == 'B') {
	/* BBoard namespace is empty */
	r = IMAP_MAILBOX_NONEXISTENT;
    }
    else {
	r = (*imapd_namespace.mboxname_tointernal)(&imapd_namespace, name,
						   imapd_userid, mailboxname);
    }

    if (!r) {
	r = mlookup(tag, name, mailboxname, NULL, NULL, NULL, NULL, NULL);
    }
    if (r == IMAP_MAILBOX_MOVED) return;

    if (!r) {
	r = mailbox_open_header(mailboxname, imapd_authstate, &mailbox);
    }

    if (!r) {
	doclose = 1;
	r = mailbox_open_index(&mailbox);
    }
    if (!r && !(mailbox.myrights & ACL_READ)) {
	r = (imapd_userisadmin || (mailbox.myrights & ACL_LOOKUP)) ?
	  IMAP_PERMISSION_DENIED : IMAP_MAILBOX_NONEXISTENT;
    }
    if (!r && chdir(mailbox.path)) {
	syslog(LOG_ERR, "IOERROR: changing directory to %s: %m", mailbox.path);
	r = IMAP_IOERROR;
    }

    if (r) {
	prot_printf(imapd_out, "%s NO %s\r\n", tag, error_message(r));
	if (doclose) mailbox_close(&mailbox);
	return;
    }

    mboxstruct = mailbox;
    imapd_mailbox = &mboxstruct;

    index_newmailbox(imapd_mailbox, cmd[0] == 'E');

    /* Examine command puts mailbox in read-only mode */
    if (cmd[0] == 'E') {
	imapd_mailbox->myrights &= ~(ACL_SEEN|ACL_WRITE|ACL_REMOVE);
    }

    if (imapd_mailbox->myrights & ACL_REMOVE) {
	/* Warn if mailbox is close to or over quota */
	r = quota_read(&imapd_mailbox->quota, NULL, 0);
	if (!r && imapd_mailbox->quota.limit > 0) {
 	    /* Warn if the following possibilities occur:
 	     * - quotawarnkb not set + quotawarn hit
	     * - quotawarnkb set larger than mailbox + quotawarn hit
 	     * - quotawarnkb set + hit + quotawarn hit
 	     */
 	    int warnsize = config_getint(IMAPOPT_QUOTAWARNKB);
 	    if (warnsize <= 0 || warnsize >= imapd_mailbox->quota.limit ||
 	        (int)((imapd_mailbox->quota.limit * QUOTA_UNITS) -
		      imapd_mailbox->quota.used) < (warnsize * QUOTA_UNITS)) {
		usage = ((double) imapd_mailbox->quota.used * 100.0) / (double)
		    (imapd_mailbox->quota.limit * QUOTA_UNITS);
		if (usage >= 100.0) {
		    prot_printf(imapd_out, "* NO [ALERT] %s\r\n",
				error_message(IMAP_NO_OVERQUOTA));
		}
		else if (usage > config_getint(IMAPOPT_QUOTAWARN)) {
		    int usageint = (int) usage;
		    prot_printf(imapd_out, "* NO [ALERT] ");
		    prot_printf(imapd_out, error_message(IMAP_NO_CLOSEQUOTA),
				usageint);
		    prot_printf(imapd_out, "\r\n");
		}
	    }
	}
    }

    prot_printf(imapd_out, "%s OK [READ-%s] %s\r\n", tag,
	   (imapd_mailbox->myrights & (ACL_WRITE|ACL_REMOVE)) ?
		"WRITE" : "ONLY", error_message(IMAP_OK_COMPLETED));

    proc_register("imapd", imapd_clienthost, imapd_userid, mailboxname);
    syslog(LOG_DEBUG, "open: user %s opened %s", imapd_userid, name);
}
	  
/*
 * Perform a CLOSE command
 */
void
cmd_close(tag)
char *tag;
{
    int r;

    if (!(imapd_mailbox->myrights & ACL_REMOVE)) r = 0;
    else {
	r = mailbox_expunge(imapd_mailbox, 1, (int (*)())0, (char *)0);
    }

    index_closemailbox(imapd_mailbox);
    mailbox_close(imapd_mailbox);
    imapd_mailbox = 0;

    if (r) {
	prot_printf(imapd_out, "%s NO %s\r\n", tag, error_message(r));
    }
    else {
	prot_printf(imapd_out, "%s OK %s\r\n", tag,
		    error_message(IMAP_OK_COMPLETED));
    }
}    

/*
 * Perform an UNSELECT command -- for some support of IMAP proxy.
 * Just like close except no expunge.
 */
void
cmd_unselect(tag)
char* tag;
{
    index_closemailbox(imapd_mailbox);
    mailbox_close(imapd_mailbox);
    imapd_mailbox = 0;

    prot_printf(imapd_out, "%s OK %s\r\n", tag,
		error_message(IMAP_OK_COMPLETED));
}

/*
 * Parse the syntax for a partial fetch:
 *   "<" number "." nz-number ">"
 */
#define PARSE_PARTIAL(start_octet, octet_count)			        \
    (start_octet) = (octet_count) = 0;                                  \
    if (*p == '<' && isdigit((int) p[1])) {				\
	(start_octet) = p[1] - '0';				\
	p += 2;								\
	while (isdigit((int) *p)) {					\
	    (start_octet) =					\
		(start_octet) * 10 + *p++ - '0';		\
	}								\
									\
	if (*p == '.' && p[1] >= '1' && p[1] <= '9') {			\
	    (octet_count) = p[1] - '0';				\
	    p[0] = '>'; p[1] = '\0'; /* clip off the octet count 	\
					(its not used in the reply) */	\
	    p += 2;							\
	    while (isdigit((int) *p)) {					\
		(octet_count) =					\
		    (octet_count) * 10 + *p++ - '0';		\
	    }								\
	}								\
	else p--;							\
									\
	if (*p != '>') {						\
	    prot_printf(imapd_out,					\
			"%s BAD Invalid body partial\r\n", tag);	\
	    eatline(imapd_in, c);					\
	    goto freeargs;						\
	}								\
	p++;								\
    }

/*
 * Parse and perform a FETCH/UID FETCH command
 * The command has been parsed up to and including
 * the sequence
 */
void cmd_fetch(char *tag, char *sequence, int usinguid)
{
    char *cmd = usinguid ? "UID Fetch" : "Fetch";
    static struct buf fetchatt, fieldname;
    int c;
    int inlist = 0;
    int fetchitems = 0;
    struct fetchargs fetchargs;
    struct octetinfo oi;
    struct strlist *newfields = 0;
    char *p, *section;
    int fetchedsomething, r;
    clock_t start = clock();
    char mytime[100];

    memset(&fetchargs, 0, sizeof(struct fetchargs));

    c = getword(imapd_in, &fetchatt);
    if (c == '(' && !fetchatt.s[0]) {
	inlist = 1;
	c = getword(imapd_in, &fetchatt);
    }
    for (;;) {
	ucase(fetchatt.s);
	switch (fetchatt.s[0]) {
	case 'A':
	    if (!inlist && !strcmp(fetchatt.s, "ALL")) {
		fetchitems |= FETCH_ALL;
	    }
	    else goto badatt;
	    break;

	case 'B':
	    if (!strncmp(fetchatt.s, "BINARY[", 7) ||
		!strncmp(fetchatt.s, "BINARY.PEEK[", 12) ||
		!strncmp(fetchatt.s, "BINARY.SIZE[", 12)) {
		int binsize = 0;

		p = section = fetchatt.s + 7;
		if (!strncmp(p, "PEEK[", 5)) {
		    p = section += 5;
		}
		else if (!strncmp(p, "SIZE[", 5)) {
		    p = section += 5;
		    binsize = 1;
		}
		else {
		    fetchitems |= FETCH_SETSEEN;
		}
		while ((*p >= '1' && *p <= '9') || *p == '.') {
		    if (*p == '.' && !isdigit((int) p[-1])) break;
		    p++;
		}

		if (*p != ']') {
		    prot_printf(imapd_out, "%s BAD Invalid body section\r\n", tag);
		    eatline(imapd_in, c);
		    goto freeargs;
		}
		p++;

		if (!binsize) PARSE_PARTIAL(oi.start_octet, oi.octet_count);

		if (*p) {
		    prot_printf(imapd_out, "%s BAD Junk after body section\r\n", tag);
		    eatline(imapd_in, c);
		    goto freeargs;
		}
		if (binsize)
		    appendstrlist_withdata(&fetchargs.sizesections, section, &oi, sizeof(oi));
		else
		    appendstrlist_withdata(&fetchargs.binsections, section, &oi, sizeof(oi));
	    }
	    else if (!strcmp(fetchatt.s, "BODY")) {
		fetchitems |= FETCH_BODY;
	    }
	    else if (!strcmp(fetchatt.s, "BODYSTRUCTURE")) {
		fetchitems |= FETCH_BODYSTRUCTURE;
	    }
	    else if (!strncmp(fetchatt.s, "BODY[", 5) ||
		     !strncmp(fetchatt.s, "BODY.PEEK[", 10)) {
		p = section = fetchatt.s + 5;
		if (!strncmp(p, "PEEK[", 5)) {
		    p = section += 5;
		}
		else {
		    fetchitems |= FETCH_SETSEEN;
		}
		while (isdigit((int) *p) || *p == '.') {
		    if (*p == '.' && !isdigit((int) p[-1])) break;
		    /* Obsolete section 0 can only occur before close brace */
		    if (*p == '0' && !isdigit((int) p[-1]) && p[1] != ']') break;
		    p++;
		}

		if (*p == 'H' && !strncmp(p, "HEADER.FIELDS", 13) &&
		    (p == section || p[-1] == '.') &&
		    (p[13] == '\0' || !strcmp(p+13, ".NOT"))) {

		    /*
		     * If not top-level or a HEADER.FIELDS.NOT, can't pull
		     * the headers out of the cache.
		     */
		    if (p != section || p[13] != '\0') {
			fetchargs.cache_atleast = BIT32_MAX;
		    }

		    if (c != ' ') {
			prot_printf(imapd_out,
				    "%s BAD Missing required argument to %s %s\r\n",
				    tag, cmd, fetchatt.s);
			eatline(imapd_in, c);
			goto freeargs;
		    }
		    c = prot_getc(imapd_in);
		    if (c != '(') {
			prot_printf(imapd_out, "%s BAD Missing required open parenthesis in %s %s\r\n",
				    tag, cmd, fetchatt.s);
			eatline(imapd_in, c);
			goto freeargs;
		    }
		    do {
			c = getastring(imapd_in, imapd_out, &fieldname);
			for (p = fieldname.s; *p; p++) {
			    if (*p <= ' ' || *p & 0x80 || *p == ':') break;
			}
			if (*p || !*fieldname.s) {
			    prot_printf(imapd_out, "%s BAD Invalid field-name in %s %s\r\n",
					tag, cmd, fetchatt.s);
			    eatline(imapd_in, c);
			    goto freeargs;
			}
			appendstrlist(&newfields, fieldname.s);
			if (fetchargs.cache_atleast < BIT32_MAX) {
			    bit32 this_ver =
				mailbox_cached_header(fieldname.s);
			    if(this_ver > fetchargs.cache_atleast)
				fetchargs.cache_atleast = this_ver;
			}
		    } while (c == ' ');
		    if (c != ')') {
			prot_printf(imapd_out, "%s BAD Missing required close parenthesis in %s %s\r\n",
				    tag, cmd, fetchatt.s);
			eatline(imapd_in, c);
			goto freeargs;
		    }

		    /* Grab/parse the ]<x.y> part */
		    c = getword(imapd_in, &fieldname);
		    p = fieldname.s;
		    if (*p++ != ']') {
			prot_printf(imapd_out, "%s BAD Missing required close bracket after %s %s\r\n",
				    tag, cmd, fetchatt.s);
			eatline(imapd_in, c);
			goto freeargs;
		    }

		    PARSE_PARTIAL(oi.start_octet, oi.octet_count);

		    if (*p) {
			prot_printf(imapd_out, "%s BAD Junk after body section\r\n", tag);
			eatline(imapd_in, c);
			goto freeargs;
		    }
		    appendfieldlist(&fetchargs.fsections,
				    section, newfields, fieldname.s,
				    &oi, sizeof(oi));
		    newfields = 0;
		    break;
		}

		switch (*p) {
		case 'H':
		    if (p != section && p[-1] != '.') break;
		    if (!strncmp(p, "HEADER]", 7)) p += 6;
		    break;

		case 'M':
		    if (!strncmp(p-1, ".MIME]", 6)) p += 4;
		    break;

		case 'T':
		    if (p != section && p[-1] != '.') break;
		    if (!strncmp(p, "TEXT]", 5)) p += 4;
		    break;
		}

		if (*p != ']') {
		    prot_printf(imapd_out, "%s BAD Invalid body section\r\n", tag);
		    eatline(imapd_in, c);
		    goto freeargs;
		}
		p++;

		PARSE_PARTIAL(oi.start_octet, oi.octet_count);

		if (*p) {
		    prot_printf(imapd_out, "%s BAD Junk after body section\r\n", tag);
		    eatline(imapd_in, c);
		    goto freeargs;
		}
		appendstrlist_withdata(&fetchargs.bodysections, section,
				       &oi, sizeof(oi));
	    }
	    else goto badatt;
	    break;

	case 'E':
	    if (!strcmp(fetchatt.s, "ENVELOPE")) {
		fetchitems |= FETCH_ENVELOPE;
	    }
	    else goto badatt;
	    break;

	case 'F':
	    if (!inlist && !strcmp(fetchatt.s, "FAST")) {
		fetchitems |= FETCH_FAST;
	    }
	    else if (!inlist && !strcmp(fetchatt.s, "FULL")) {
		fetchitems |= FETCH_FULL;
	    }
	    else if (!strcmp(fetchatt.s, "FLAGS")) {
		fetchitems |= FETCH_FLAGS;
	    }
	    else goto badatt;
	    break;

	case 'I':
	    if (!strcmp(fetchatt.s, "INTERNALDATE")) {
		fetchitems |= FETCH_INTERNALDATE;
	    }
	    else goto badatt;
	    break;

	case 'R':
	    if (!strcmp(fetchatt.s, "RFC822")) {
		fetchitems |= FETCH_RFC822|FETCH_SETSEEN;
	    }
	    else if (!strcmp(fetchatt.s, "RFC822.HEADER")) {
		fetchitems |= FETCH_HEADER;
	    }
	    else if (!strcmp(fetchatt.s, "RFC822.PEEK")) {
		fetchitems |= FETCH_RFC822;
	    }
	    else if (!strcmp(fetchatt.s, "RFC822.SIZE")) {
		fetchitems |= FETCH_SIZE;
	    }
	    else if (!strcmp(fetchatt.s, "RFC822.TEXT")) {
		fetchitems |= FETCH_TEXT|FETCH_SETSEEN;
	    }
	    else if (!strcmp(fetchatt.s, "RFC822.TEXT.PEEK")) {
		fetchitems |= FETCH_TEXT;
	    }
	    else if (!strcmp(fetchatt.s, "RFC822.HEADER.LINES") ||
		     !strcmp(fetchatt.s, "RFC822.HEADER.LINES.NOT")) {
		if (c != ' ') {
		    prot_printf(imapd_out, "%s BAD Missing required argument to %s %s\r\n",
			   tag, cmd, fetchatt.s);
		    eatline(imapd_in, c);
		    goto freeargs;
		}
		c = prot_getc(imapd_in);
		if (c != '(') {
		    prot_printf(imapd_out, "%s BAD Missing required open parenthesis in %s %s\r\n",
			   tag, cmd, fetchatt.s);
		    eatline(imapd_in, c);
		    goto freeargs;
		}
		do {
		    c = getastring(imapd_in, imapd_out, &fieldname);
		    for (p = fieldname.s; *p; p++) {
			if (*p <= ' ' || *p & 0x80 || *p == ':') break;
		    }
		    if (*p || !*fieldname.s) {
			prot_printf(imapd_out, "%s BAD Invalid field-name in %s %s\r\n",
			       tag, cmd, fetchatt.s);
			eatline(imapd_in, c);
			goto freeargs;
		    }
		    lcase(fieldname.s);;
		    /* 19 is magic number -- length of 
		     * "RFC822.HEADERS.NOT" */
		    appendstrlist(strlen(fetchatt.s) == 19 ?
				  &fetchargs.headers : &fetchargs.headers_not,
				  fieldname.s);
		    if (strlen(fetchatt.s) != 19) {
			fetchargs.cache_atleast = BIT32_MAX;
		    }
		    if (fetchargs.cache_atleast < BIT32_MAX) {
			bit32 this_ver =
			    mailbox_cached_header(fieldname.s);
			if(this_ver > fetchargs.cache_atleast)
			    fetchargs.cache_atleast = this_ver;
		   }
		} while (c == ' ');
		if (c != ')') {
		    prot_printf(imapd_out, "%s BAD Missing required close parenthesis in %s %s\r\n",
			   tag, cmd, fetchatt.s);
		    eatline(imapd_in, c);
		    goto freeargs;
		}
		c = prot_getc(imapd_in);
	    }
	    else goto badatt;
	    break;

	case 'U':
	    if (!strcmp(fetchatt.s, "UID")) {
		fetchitems |= FETCH_UID;
	    }
	    else goto badatt;
	    break;

	default:
	badatt:
	    prot_printf(imapd_out, "%s BAD Invalid %s attribute %s\r\n", tag, cmd, fetchatt.s);
	    eatline(imapd_in, c);
	    goto freeargs;
	}

	if (inlist && c == ' ') c = getword(imapd_in, &fetchatt);
	else break;
    }
    
    if (inlist && c == ')') {
	inlist = 0;
	c = prot_getc(imapd_in);
    }
    if (inlist) {
	prot_printf(imapd_out, "%s BAD Missing close parenthesis in %s\r\n", tag, cmd);
	eatline(imapd_in, c);
	goto freeargs;
    }
    if (c == '\r') c = prot_getc(imapd_in);
    if (c != '\n') {
	prot_printf(imapd_out, "%s BAD Unexpected extra arguments to %s\r\n", tag, cmd);
	eatline(imapd_in, c);
	goto freeargs;
    }

    if (!fetchitems && !fetchargs.bodysections && !fetchargs.fsections &&
	!fetchargs.binsections && !fetchargs.sizesections &&
	!fetchargs.headers && !fetchargs.headers_not) {
	prot_printf(imapd_out, "%s BAD Missing required argument to %s\r\n", tag, cmd);
	goto freeargs;
    }

    if (usinguid) {
	fetchitems |= FETCH_UID;
	index_check(imapd_mailbox, 1, 0);
    }

    fetchargs.fetchitems = fetchitems;
    r = index_fetch(imapd_mailbox, sequence, usinguid, &fetchargs,
		&fetchedsomething);

    snprintf(mytime, sizeof(mytime), "%2.3f", 
	     (clock() - start) / (double) CLOCKS_PER_SEC);

    if (r) {
	prot_printf(imapd_out, "%s NO %s (%s sec)\r\n", tag,
		    error_message(r), mytime);
    } else if (fetchedsomething || usinguid) {
	prot_printf(imapd_out, "%s OK %s (%s sec)\r\n", tag,
		    error_message(IMAP_OK_COMPLETED), mytime);
    } else {
	/* normal FETCH, nothing came back */
	prot_printf(imapd_out, "%s NO %s (%s sec)\r\n", tag,
		    error_message(IMAP_NO_NOSUCHMSG), mytime);
    }

 freeargs:
    freestrlist(newfields);
    freestrlist(fetchargs.bodysections);
    freefieldlist(fetchargs.fsections);
    freestrlist(fetchargs.headers);
    freestrlist(fetchargs.headers_not);
}

#undef PARSE_PARTIAL /* cleanup */

/*
 * Perform a PARTIAL command
 */
void cmd_partial(const char *tag, const char *msgno, char *data,
		 const char *start, const char *count)
{
    const char *pc;
    char *p;
    struct fetchargs fetchargs;
    char *section;
    int prev;
    int fetchedsomething;

    memset(&fetchargs, 0, sizeof(struct fetchargs));

    for (pc = msgno; *pc; pc++) {
	if (!isdigit((int) *pc)) break;
    }
    if (*pc || !*msgno) {
	prot_printf(imapd_out, "%s BAD Invalid message number\r\n", tag);
	return;
    }

    lcase(data);
    if (!strcmp(data, "rfc822")) {
	fetchargs.fetchitems = FETCH_RFC822|FETCH_SETSEEN;
    }
    else if (!strcmp(data, "rfc822.header")) {
	fetchargs.fetchitems = FETCH_HEADER;
    }
    else if (!strcmp(data, "rfc822.peek")) {
	fetchargs.fetchitems = FETCH_RFC822;
    }
    else if (!strcmp(data, "rfc822.text")) {
	fetchargs.fetchitems = FETCH_TEXT|FETCH_SETSEEN;
    }
    else if (!strcmp(data, "rfc822.text.peek")) {
	fetchargs.fetchitems = FETCH_TEXT;
    }
    else if (!strncmp(data, "body[", 5) ||
	     !strncmp(data, "body.peek[", 10)) {
	p = section = data + 5;
	if (!strncmp(p, "peek[", 5)) {
	    p = section += 5;
	}
	else {
	    fetchargs.fetchitems = FETCH_SETSEEN;
	}
	while (isdigit((int) *p) || *p == '.') {
	    if (*p == '.' && (p == section || !isdigit((int) p[1]))) break;
	    p++;
	}
	if (p == section || *p != ']' || p[1]) {
	    prot_printf(imapd_out, "%s BAD Invalid body section\r\n", tag);
	    freestrlist(fetchargs.bodysections);
	    return;
	}
	*(p+1) = '\0'; /* Keep the closing bracket in place */
	appendstrlist(&fetchargs.bodysections, section);
    }
    else {
	prot_printf(imapd_out, "%s BAD Invalid Partial item\r\n", tag);
	freestrlist(fetchargs.bodysections);
	return;
    }

    for (pc = start; *pc; pc++) {
	if (!isdigit((int) *pc)) break;
	prev = fetchargs.start_octet;
	fetchargs.start_octet = fetchargs.start_octet*10 + *pc - '0';
	if(fetchargs.start_octet < prev) {
	    fetchargs.start_octet = 0;
	    break;
	}
    }
    if (*pc || !fetchargs.start_octet) {
	prot_printf(imapd_out, "%s BAD Invalid starting octet\r\n", tag);
	freestrlist(fetchargs.bodysections);
	return;
    }
    fetchargs.start_octet--;	/* Normalize to be 0-based */
    
    prev = fetchargs.octet_count;
    for (pc = count; *pc; pc++) {
	if (!isdigit((int) *pc)) break;
	prev = fetchargs.octet_count;
	fetchargs.octet_count = fetchargs.octet_count*10 + *pc - '0';
	if(fetchargs.octet_count < prev) {
	    prev = -1;
	    break;
	}
    }
    if (*pc || !*count || prev == -1) {
	prot_printf(imapd_out, "%s BAD Invalid octet count\r\n", tag);
	freestrlist(fetchargs.bodysections);
	return;
    }

    fetchargs.fetchitems |= FETCH_IS_PARTIAL;

    index_fetch(imapd_mailbox, msgno, 0, &fetchargs, &fetchedsomething);

    index_check(imapd_mailbox, 0, 0);

    if (fetchedsomething) {
	prot_printf(imapd_out, "%s OK %s\r\n", tag,
		    error_message(IMAP_OK_COMPLETED));
    } else {
	prot_printf(imapd_out,
		    "%s BAD Invalid sequence in PARTIAL command\r\n",
		    tag);
    }

    freestrlist(fetchargs.bodysections);
}

/*
 * Parse and perform a STORE/UID STORE command
 * The command has been parsed up to and including
 * the FLAGS/+FLAGS/-FLAGS
 */
void cmd_store(char *tag, char *sequence, char *operation, int usinguid)
{
    char *cmd = usinguid ? "UID Store" : "Store";
    struct storeargs storeargs;
    static struct buf flagname;
    int len, c;
    char **flag = 0;
    int nflags = 0, flagalloc = 0;
    int flagsparsed = 0, inlist = 0;
    int r;

    memset(&storeargs, 0, sizeof storeargs);

    lcase(operation);

    len = strlen(operation);
    if (len > 7 && !strcmp(operation+len-7, ".silent")) {
	storeargs.silent = 1;
	operation[len-7] = '\0';
    }
    
    if (!strcmp(operation, "+flags")) {
	storeargs.operation = STORE_ADD;
    }
    else if (!strcmp(operation, "-flags")) {
	storeargs.operation = STORE_REMOVE;
    }
    else if (!strcmp(operation, "flags")) {
	storeargs.operation = STORE_REPLACE;
    }
    else {
	prot_printf(imapd_out, "%s BAD Invalid %s attribute\r\n", tag, cmd);
	eatline(imapd_in, ' ');
	return;
    }

    for (;;) {
	c = getword(imapd_in, &flagname);
	if (c == '(' && !flagname.s[0] && !flagsparsed && !inlist) {
	    inlist = 1;
	    continue;
	}

	if (!flagname.s[0]) break;

	if (flagname.s[0] == '\\') {
	    lcase(flagname.s);
	    if (!strcmp(flagname.s, "\\seen")) {
		storeargs.seen = 1;
	    }
	    else if (!strcmp(flagname.s, "\\answered")) {
		storeargs.system_flags |= FLAG_ANSWERED;
	    }
	    else if (!strcmp(flagname.s, "\\flagged")) {
		storeargs.system_flags |= FLAG_FLAGGED;
	    }
	    else if (!strcmp(flagname.s, "\\deleted")) {
		storeargs.system_flags |= FLAG_DELETED;
	    }
	    else if (!strcmp(flagname.s, "\\draft")) {
		storeargs.system_flags |= FLAG_DRAFT;
	    }
	    else {
		prot_printf(imapd_out, "%s BAD Invalid system flag in %s command\r\n",
		       tag, cmd);
		eatline(imapd_in, c);
		goto freeflags;
	    }
	}
	else if (!imparse_isatom(flagname.s)) {
	    prot_printf(imapd_out, "%s BAD Invalid flag name %s in %s command\r\n",
		   tag, flagname.s, cmd);
	    eatline(imapd_in, c);
	    goto freeflags;
	}
	else {
	    if (nflags == flagalloc) {
		flagalloc += FLAGGROW;
		flag = (char **)xrealloc((char *)flag,
					 flagalloc*sizeof(char *));
	    }
	    flag[nflags] = xstrdup(flagname.s);
	    nflags++;
	}

	flagsparsed++;
	if (c != ' ') break;
    }

    if (!inlist && !flagsparsed) {
	prot_printf(imapd_out, "%s BAD Missing required argument to %s\r\n", tag, cmd);
	eatline(imapd_in, c);
	return;
    }
    if (inlist && c == ')') {
	inlist = 0;
	c = prot_getc(imapd_in);
    }
    if (inlist) {
	prot_printf(imapd_out, "%s BAD Missing close parenthesis in %s\r\n", tag, cmd);
	eatline(imapd_in, c);
	goto freeflags;
    }
    if (c == '\r') c = prot_getc(imapd_in);
    if (c != '\n') {
	prot_printf(imapd_out, "%s BAD Unexpected extra arguments to %s\r\n", tag, cmd);
	eatline(imapd_in, c);
	goto freeflags;
    }

    r = index_store(imapd_mailbox, sequence, usinguid, &storeargs,
		    flag, nflags);

    if (usinguid) {
	index_check(imapd_mailbox, 1, 0);
    }

    if (r) {
	prot_printf(imapd_out, "%s NO %s\r\n", tag, error_message(r));
    }
    else {
	prot_printf(imapd_out, "%s OK %s\r\n", tag,
		    error_message(IMAP_OK_COMPLETED));
    }

 freeflags:
    while (nflags--) {
	free(flag[nflags]);
    }
    if (flag) free((char *)flag);
}

void
cmd_search(tag, usinguid)
char *tag;
int usinguid;
{
    int c;
    int charset = 0;
    struct searchargs *searchargs;
    clock_t start = clock();
    char mytime[100];
    int n;

    searchargs = (struct searchargs *)xzmalloc(sizeof(struct searchargs));

    c = getsearchprogram(tag, searchargs, &charset, 1);
    if (c == EOF) {
	eatline(imapd_in, ' ');
	freesearchargs(searchargs);
	return;
    }

    if (c == '\r') c = prot_getc(imapd_in);
    if (c != '\n') {
	prot_printf(imapd_out, "%s BAD Unexpected extra arguments to Search\r\n", tag);
	eatline(imapd_in, c);
	freesearchargs(searchargs);
	return;
    }

    if (charset == -1) {
	prot_printf(imapd_out, "%s NO %s\r\n", tag,
	       error_message(IMAP_UNRECOGNIZED_CHARSET));
    }
    else {
	n = index_search(imapd_mailbox, searchargs, usinguid);
	snprintf(mytime, sizeof(mytime), "%2.3f", 
		 (clock() - start) / (double) CLOCKS_PER_SEC);
	prot_printf(imapd_out, "%s OK %s (%d msgs in %s secs)\r\n", tag,
		    error_message(IMAP_OK_COMPLETED), n, mytime);
    }

    freesearchargs(searchargs);
}

/*
 * Perform a SORT/UID SORT command
 */    
void
cmd_sort(tag, usinguid)
char *tag;
int usinguid;
{
    int c;
    struct sortcrit *sortcrit = NULL;
    static struct buf arg;
    int charset = 0;
    struct searchargs *searchargs;
    clock_t start = clock();
    char mytime[100];
    int n;

    c = getsortcriteria(tag, &sortcrit);
    if (c == EOF) {
	eatline(imapd_in, ' ');
	freesortcrit(sortcrit);
	return;
    }

    /* get charset */
    if (c != ' ') {
	prot_printf(imapd_out, "%s BAD Missing charset in Sort\r\n",
		    tag);
	eatline(imapd_in, c);
	freesortcrit(sortcrit);
	return;
    }

    c = getword(imapd_in, &arg);
    if (c != ' ') {
	prot_printf(imapd_out, "%s BAD Missing search criteria in Sort\r\n",
		    tag);
	eatline(imapd_in, c);
	freesortcrit(sortcrit);
	return;
    }
    lcase(arg.s);
    charset = charset_lookupname(arg.s);

    if (charset == -1) {
	prot_printf(imapd_out, "%s NO %s\r\n", tag,
	       error_message(IMAP_UNRECOGNIZED_CHARSET));
	eatline(imapd_in, c);
	freesortcrit(sortcrit);
	return;
    }

    searchargs = (struct searchargs *)xzmalloc(sizeof(struct searchargs));

    c = getsearchprogram(tag, searchargs, &charset, 0);
    if (c == EOF) {
	eatline(imapd_in, ' ');
	freesearchargs(searchargs);
	freesortcrit(sortcrit);
	return;
    }

    if (c == '\r') c = prot_getc(imapd_in);
    if (c != '\n') {
	prot_printf(imapd_out, 
		    "%s BAD Unexpected extra arguments to Sort\r\n", tag);
	eatline(imapd_in, c);
	freesearchargs(searchargs);
	freesortcrit(sortcrit);
	return;
    }

    n = index_sort(imapd_mailbox, sortcrit, searchargs, usinguid);
    snprintf(mytime, sizeof(mytime), "%2.3f",
	     (clock() - start) / (double) CLOCKS_PER_SEC);
    prot_printf(imapd_out, "%s OK %s (%d msgs in %s secs)\r\n", tag,
		error_message(IMAP_OK_COMPLETED), n, mytime);

    freesortcrit(sortcrit);
    freesearchargs(searchargs);
    return;
}

/*
 * Perform a THREAD/UID THREAD command
 */    
void
cmd_thread(tag, usinguid)
char *tag;
int usinguid;
{
    static struct buf arg;
    int c;
    int charset = 0;
    int alg;
    struct searchargs *searchargs;
    clock_t start = clock();
    char mytime[100];
    int n;

    /* get algorithm */
    c = getword(imapd_in, &arg);
    if (c != ' ') {
	prot_printf(imapd_out, "%s BAD Missing algorithm in Thread\r\n", tag);
	eatline(imapd_in, c);
	return;
    }

    if ((alg = find_thread_algorithm(arg.s)) == -1) {
	prot_printf(imapd_out, "%s BAD Invalid Thread algorithm %s\r\n",
		    tag, arg.s);
	eatline(imapd_in, c);
	return;
    }

    /* get charset */
    c = getword(imapd_in, &arg);
    if (c != ' ') {
	prot_printf(imapd_out, "%s BAD Missing charset in Thread\r\n",
		    tag);
	eatline(imapd_in, c);
	return;
    }
    lcase(arg.s);
    charset = charset_lookupname(arg.s);

    if (charset == -1) {
	prot_printf(imapd_out, "%s NO %s\r\n", tag,
	       error_message(IMAP_UNRECOGNIZED_CHARSET));
	eatline(imapd_in, c);
	return;
    }

    searchargs = (struct searchargs *)xzmalloc(sizeof(struct searchargs));

    c = getsearchprogram(tag, searchargs, &charset, 0);
    if (c == EOF) {
	eatline(imapd_in, ' ');
	freesearchargs(searchargs);
	return;
    }

    if (c == '\r') c = prot_getc(imapd_in);
    if (c != '\n') {
	prot_printf(imapd_out, 
		    "%s BAD Unexpected extra arguments to Thread\r\n", tag);
	eatline(imapd_in, c);
	freesearchargs(searchargs);
	return;
    }

    n = index_thread(imapd_mailbox, alg, searchargs, usinguid);
    snprintf(mytime, sizeof(mytime), "%2.3f", 
	     (clock() - start) / (double) CLOCKS_PER_SEC);
    prot_printf(imapd_out, "%s OK %s (%d msgs in %s secs)\r\n", tag,
		error_message(IMAP_OK_COMPLETED), n, mytime);

    freesearchargs(searchargs);
    return;
}

/*
 * Perform a COPY/UID COPY command
 */    
void
cmd_copy(tag, sequence, name, usinguid)
char *tag;
char *sequence;
char *name;
int usinguid;
{
    int r;
    char mailboxname[MAX_MAILBOX_NAME+1];
    char *copyuid;

    r = (*imapd_namespace.mboxname_tointernal)(&imapd_namespace, name,
					       imapd_userid, mailboxname);
    if (!r) {
	r = index_copy(imapd_mailbox, sequence, usinguid, mailboxname,
		       &copyuid);
    }

    index_check(imapd_mailbox, usinguid, 0);

    if (r) {
	prot_printf(imapd_out, "%s NO %s%s\r\n", tag,
		    (r == IMAP_MAILBOX_NONEXISTENT &&
		     mboxlist_createmailboxcheck(mailboxname, 0, 0,
						 imapd_userisadmin,
						 imapd_userid, imapd_authstate,
						 (char **)0, (char **)0) == 0)
		    ? "[TRYCREATE] " : "", error_message(r));
    }
    else {
	if (copyuid) {
	    prot_printf(imapd_out, "%s OK [COPYUID %s] %s\r\n", tag,
			copyuid, error_message(IMAP_OK_COMPLETED));
	    free(copyuid);
	}
	else if (usinguid) {
	    prot_printf(imapd_out, "%s OK %s\r\n", tag,
			error_message(IMAP_OK_COMPLETED));
	}
	else {
	    /* normal COPY, message doesn't exist */
	    prot_printf(imapd_out, "%s NO %s\r\n", tag,
			error_message(IMAP_NO_NOSUCHMSG));
	}
    }
}    

/*
 * Perform an EXPUNGE command
 */
void
cmd_expunge(tag, sequence)
char *tag;
char *sequence;
{
    int r;

    if (!(imapd_mailbox->myrights & ACL_REMOVE)) r = IMAP_PERMISSION_DENIED;
    else if (sequence) {
	r = mailbox_expunge(imapd_mailbox, 1, index_expungeuidlist, sequence);
    }
    else {
	r = mailbox_expunge(imapd_mailbox, 1, (mailbox_decideproc_t *)0,
			    (void *)0);
    }

    index_check(imapd_mailbox, 0, 0);

    if (r) {
	prot_printf(imapd_out, "%s NO %s\r\n", tag, error_message(r));
    }
    else {
	prot_printf(imapd_out, "%s OK %s\r\n", tag,
		    error_message(IMAP_OK_COMPLETED));
    }
}    

/*
 * Perform a CREATE command
 */
void
cmd_create(char *tag, char *name, char *partition, int localonly)
{
    int r = 0;
    char mailboxname[MAX_MAILBOX_NAME+1];
    int autocreatequota;
	char *mbox_partition	= NULL;
	struct od_user_opts	useropts;

    if (partition && !imapd_userisadmin) {
	r = IMAP_PERMISSION_DENIED;
    }

	if ( partition && imapd_userisadmin )
	{
		mbox_partition = partition;
	}
	
	if ( !mbox_partition )
	{
		odGetUserOpts( imapd_userid, &useropts );
		if ( useropts.fAltDataLoc[ 0 ] != '\0' )
		{
			mbox_partition = useropts.fAltDataLoc;
		}
	}

    if (name[0] && name[strlen(name)-1] == imapd_namespace.hier_sep) {
	/* We don't care about trailing hierarchy delimiters. */
	name[strlen(name)-1] = '\0';
    }

    if (!r) {
	r = (*imapd_namespace.mboxname_tointernal)(&imapd_namespace, name,
						   imapd_userid, mailboxname);
    }

    if (!r) {
	/* xxx we do forced user creates on LOCALCREATE to facilitate
	 * mailbox moves */
	r = mboxlist_createmailbox(mailboxname, 0, mbox_partition,
				   imapd_userisadmin, 
				   imapd_userid, imapd_authstate,
				   localonly, localonly, 0);

	if (r == IMAP_PERMISSION_DENIED && !strcasecmp(name, "INBOX") &&
	    (autocreatequota = config_getint(IMAPOPT_AUTOCREATEQUOTA))) {

	    /* Auto create */
	    r = mboxlist_createmailbox(mailboxname, 0,
				       mbox_partition, 1, imapd_userid,
				       imapd_authstate, 0, 0, 0);
	    
	    if (!r && autocreatequota > 0) {
		(void) mboxlist_setquota(mailboxname, autocreatequota, 0);
	    }
	}
    }

    if (imapd_mailbox) {
	index_check(imapd_mailbox, 0, 0);
    }

    if (r) {
	prot_printf(imapd_out, "%s NO %s\r\n", tag, error_message(r));
    }
    else {
	prot_printf(imapd_out, "%s OK %s\r\n", tag,
		    error_message(IMAP_OK_COMPLETED));
    }
}	

/* Callback for use by cmd_delete */
static int delmbox(char *name,
		   int matchlen __attribute__((unused)),
		   int maycreate __attribute__((unused)),
		   void *rock __attribute__((unused)))
{
    int r;

    r = mboxlist_deletemailbox(name, imapd_userisadmin,
			       imapd_userid, imapd_authstate,
			       0, 0, 0);
    
    if(r) {
	prot_printf(imapd_out, "* NO delete %s: %s\r\n",
		    name, error_message(r));
    }
    
    return 0;
}

/*
 * Perform a DELETE command
 */
void cmd_delete(char *tag, char *name, int localonly)
{
    int r;
    char mailboxname[MAX_MAILBOX_NAME+1];
    char *p;
    int domainlen = 0;

    r = (*imapd_namespace.mboxname_tointernal)(&imapd_namespace, name,
					       imapd_userid, mailboxname);

    if (!r) {
	if (config_virtdomains && (p = strchr(mailboxname, '!')))
	    domainlen = p - mailboxname + 1;

	r = mboxlist_deletemailbox(mailboxname, imapd_userisadmin,
				   imapd_userid, imapd_authstate, 1,
				   localonly, 0);
    }

    /* was it a top-level user mailbox? */
    /* localonly deletes are only per-mailbox */
    if (!r && !localonly &&
	!strncmp(mailboxname+domainlen, "user.", 5) &&
	!strchr(mailboxname+domainlen+5, '.')) {
 	int mailboxname_len = strlen(mailboxname);

 	/* If we aren't too close to MAX_MAILBOX_NAME, append .* */
 	p = mailboxname + mailboxname_len; /* end of mailboxname */
 	if (mailboxname_len < sizeof(mailboxname) - 3) {
 	    strcpy(p, ".*");
 	}
	
	/* build a list of mailboxes - we're using internal names here */
	mboxlist_findall(NULL, mailboxname, imapd_userisadmin, imapd_userid,
			 imapd_authstate, delmbox, NULL);

	/* take care of deleting ACLs, subscriptions, seen state and quotas */
	*p = '\0'; /* clip off pattern */
	if ((!domainlen) || 
	    (domainlen+1 < (sizeof(mailboxname) - mailboxname_len))) {
	    if (domainlen) {
		/* fully qualify the userid */
		snprintf(p, (sizeof(mailboxname) - mailboxname_len), "@%.*s", 
			 domainlen-1, mailboxname);
	    }
	    user_deletedata(mailboxname+domainlen+5, imapd_userid,
			    imapd_authstate, 1);
	}
    }

    if (imapd_mailbox) {
	index_check(imapd_mailbox, 0, 0);
    }

    if (r) {
	prot_printf(imapd_out, "%s NO %s\r\n", tag, error_message(r));
    }
    else {
	prot_printf(imapd_out, "%s OK %s\r\n", tag,
		    error_message(IMAP_OK_COMPLETED));
    }
}	

struct renrock 
{
    int ol;
    int nl;
    int rename_user;
    char *olduser, *newuser;
    char *acl_olduser, *acl_newuser;
    char *newmailboxname;
    char *partition;
};

/* Callback for use by cmd_rename */
static int renmbox(char *name,
		   int matchlen __attribute__((unused)),
		   int maycreate __attribute__((unused)),
		   void *rock)
{
    char oldextname[MAX_MAILBOX_NAME+1];
    char newextname[MAX_MAILBOX_NAME+1];
    struct renrock *text = (struct renrock *)rock;
    int r;

    if((text->nl + strlen(name + text->ol)) > MAX_MAILBOX_NAME)
	return 0;

    strcpy(text->newmailboxname + text->nl, name + text->ol);

    r = mboxlist_renamemailbox(name, text->newmailboxname,
			       text->partition,
			       1, imapd_userid, imapd_authstate);
    
    (*imapd_namespace.mboxname_toexternal)(&imapd_namespace,
					   name,
					   imapd_userid, oldextname);
    (*imapd_namespace.mboxname_toexternal)(&imapd_namespace,
					   text->newmailboxname,
					   imapd_userid, newextname);

    if(r) {
	prot_printf(imapd_out, "* NO rename %s %s: %s\r\n",
		    oldextname, newextname, error_message(r));
	if (RENAME_STOP_ON_ERROR) return r;
    } else {
	/* If we're renaming a user, change quotaroot and ACL */
	if (text->rename_user) {
	    user_copyquotaroot(name, text->newmailboxname);
	    user_renameacl(text->newmailboxname,
			   text->acl_olduser, text->acl_newuser);
	}

	/* Rename mailbox annotations */
	annotatemore_rename(name, text->newmailboxname,
			    text->rename_user ? text->olduser : NULL,
			    text->newuser);
	
	prot_printf(imapd_out, "* OK rename %s %s\r\n",
		    oldextname, newextname);
    }

    prot_flush(imapd_out);

    return 0;
}

/*
 * Perform a RENAME command
 */
void cmd_rename(const char *tag, 
		char *oldname, char *newname, char *partition)
{
    int r = 0;
    char oldmailboxname[MAX_MAILBOX_NAME+3];
    char newmailboxname[MAX_MAILBOX_NAME+2];
    char oldextname[MAX_MAILBOX_NAME+1];
    char newextname[MAX_MAILBOX_NAME+1];
    int omlen, nmlen;
    char *p;
    int recursive_rename = 1;
    int rename_user = 0;
    char olduser[128], newuser[128];
    char acl_olduser[128], acl_newuser[128];

    /* canonicalize names */
    if (partition && !imapd_userisadmin) {
	r = IMAP_PERMISSION_DENIED;
    }

    if (!r)
	r = (*imapd_namespace.mboxname_tointernal)(&imapd_namespace, oldname,
						   imapd_userid, oldmailboxname);
    if (!r)
	r = (*imapd_namespace.mboxname_tointernal)(&imapd_namespace, newname,
						   imapd_userid, newmailboxname);

    /* if this is my inbox, don't do recursive renames */
    if (!strcasecmp(oldname, "inbox")) {
	recursive_rename = 0;
    }
    /* check if we're an admin renaming a user */
    else if (config_getswitch(IMAPOPT_ALLOWUSERMOVES) &&
	     mboxname_isusermailbox(oldmailboxname, 1) &&
	     mboxname_isusermailbox(newmailboxname, 1) &&
	     strcmp(oldmailboxname, newmailboxname) && /* different user */
	     imapd_userisadmin) {
	rename_user = 1;
    }

    /* if we're renaming something inside of something else, 
       don't recursively rename stuff */
    omlen = strlen(oldmailboxname);
    nmlen = strlen(newmailboxname);
    if (omlen < nmlen) {
	if (!strncmp(oldmailboxname, newmailboxname, omlen) &&
	    newmailboxname[omlen] == '.') {
	    recursive_rename = 0;
	}
    } else {
	if (!strncmp(oldmailboxname, newmailboxname, nmlen) &&
	    oldmailboxname[nmlen] == '.') {
	    recursive_rename = 0;
	}
    }

	/* case change not supported on mac os x */
    if (!strcasecmp(oldname, newname)) {
	r = IMAP_MAILBOX_CASE_CHANGE_NOT_SUPPORTED;
    }

    /* verify that the mailbox doesn't have a wildcard in it */
    for (p = oldmailboxname; !r && *p; p++) {
	if (*p == '*' || *p == '%') r = IMAP_MAILBOX_BADNAME;
    }

    /* attempt to rename the base mailbox */
    if (!r) {
	r = mboxlist_renamemailbox(oldmailboxname, newmailboxname, partition,
				   imapd_userisadmin, 
				   imapd_userid, imapd_authstate);
    }

    /* If we're renaming a user, take care of changing quotaroot, ACL,
       seen state, subscriptions and sieve scripts */
    if (!r && rename_user) {
	char *domain;

	/* create canonified userids */

	domain = strchr(oldmailboxname, '!');
	strcpy(olduser, domain ? domain+6 : oldmailboxname+5);
	if (domain)
	    sprintf(olduser+strlen(olduser), "@%.*s",
		    domain - oldmailboxname, oldmailboxname);
	strcpy(acl_olduser, olduser);

	/* Translate any separators in source old userid (for ACLs) */
	mboxname_hiersep_toexternal(&imapd_namespace, acl_olduser,
				    config_virtdomains ?
				    strcspn(acl_olduser, "@") : 0);

	domain = strchr(newmailboxname, '!');
	strcpy(newuser, domain ? domain+6 : newmailboxname+5);
	if (domain)
	    sprintf(newuser+strlen(newuser), "@%.*s",
		    domain - newmailboxname, newmailboxname);
	strcpy(acl_newuser, newuser);

	/* Translate any separators in destination new userid (for ACLs) */
	mboxname_hiersep_toexternal(&imapd_namespace, acl_newuser,
				    config_virtdomains ?
				    strcspn(acl_newuser, "@") : 0);

	user_copyquotaroot(oldmailboxname, newmailboxname);
	user_renameacl(newmailboxname, acl_olduser, acl_newuser);
	user_renamedata(olduser, newuser, imapd_userid, imapd_authstate);

	/* XXX report status/progress of meta-data */
    }

    if (!r) {
	/* Rename mailbox annotations */
	annotatemore_rename(oldmailboxname, newmailboxname,
			    rename_user ? olduser : NULL,
			    newuser);
    }

    /* rename all mailboxes matching this */
    if (!r && recursive_rename) {
	struct renrock rock;
	int ol = omlen + 1;
	int nl = nmlen + 1;

	(*imapd_namespace.mboxname_toexternal)(&imapd_namespace,
					       oldmailboxname,
					       imapd_userid, oldextname);
	(*imapd_namespace.mboxname_toexternal)(&imapd_namespace,
					       newmailboxname,
					       imapd_userid, newextname);

	prot_printf(imapd_out, "* OK rename %s %s\r\n",
		    oldextname, newextname);
	prot_flush(imapd_out);

	strcat(oldmailboxname, ".*");
	strcat(newmailboxname, ".");

	/* setup the rock */
	rock.newmailboxname = newmailboxname;
	rock.ol = ol;
	rock.nl = nl;
	rock.olduser = olduser;
	rock.newuser = newuser;
	rock.acl_olduser = acl_olduser;
	rock.acl_newuser = acl_newuser;
	rock.partition = partition;
	rock.rename_user = rename_user;
	
	/* add submailboxes; we pretend we're an admin since we successfully
	   renamed the parent - we're using internal names here */
	r = mboxlist_findall(NULL, oldmailboxname, 1, imapd_userid,
			     imapd_authstate, renmbox, &rock);
    }

    /* take care of deleting old ACLs, subscriptions, seen state and quotas */
    if (!r && rename_user)
	user_deletedata(olduser, imapd_userid, imapd_authstate, 1);

    if (imapd_mailbox) {
	index_check(imapd_mailbox, 0, 0);
    }

    if (r) {
	prot_printf(imapd_out, "%s NO %s\r\n", tag, error_message(r));
    } else {
	prot_printf(imapd_out, "%s OK %s\r\n", tag,
		    error_message(IMAP_OK_COMPLETED));
    }
}	

/*
 * Perform a RECONSTRUCT command
 */
void
cmd_reconstruct(const char *tag, const char *name, int recursive)
{
    int r = 0;
    char mailboxname[MAX_MAILBOX_NAME+1];
    char quotaroot[MAX_MAILBOX_NAME+1];
    struct mailbox mailbox;

    /* administrators only please */
    if (!imapd_userisadmin) {
	r = IMAP_PERMISSION_DENIED;
    }

    if (!r) {
	r = (*imapd_namespace.mboxname_tointernal)(&imapd_namespace, name,
						   imapd_userid, mailboxname);
    }
    
    if (!r) {
	r = mlookup(tag, name, mailboxname, NULL, NULL, NULL, NULL, NULL);
    }
    if (r == IMAP_MAILBOX_MOVED) return;

    if (!r) {
	int pid;
	    
	/* Reconstruct it */

	pid = fork();
	if(pid == -1) {
	    r = IMAP_SYS_ERROR;
	} else if(pid == 0) {
	    char buf[4096];
	    int ret;
	    
	    /* Child - exec reconstruct*/	    
	    syslog(LOG_NOTICE, "Reconstructing '%s' (%s) for user '%s'",
		   mailboxname, recursive ? "recursive" : "not recursive",
		   imapd_userid);

	    fclose(stdin);
	    fclose(stdout);
	    fclose(stderr);

	    ret = snprintf(buf, sizeof(buf), "%s/reconstruct", SERVICE_PATH);
	    if(ret < 0 || ret >= sizeof(buf)) {
		/* in child, so fatailing won't disconnect our user */ 
	        fatal("reconstruct buffer not sufficiently big", EC_CONFIG);
	    }

	    if(recursive) {
		execl(buf, buf, "-C", config_filename, "-r", "-f",
		      mailboxname, NULL);
	    } else {
		execl(buf, buf, "-C", config_filename, mailboxname, NULL);
	    }
	    
	    /* if we are here, we have a problem */
	    exit(-1);
	} else {
	    int status;

	    /* Parent, wait on child */
	    if(waitpid(pid, &status, 0) < 0) r = IMAP_SYS_ERROR;

	    /* Did we fail? */
	    if(WEXITSTATUS(status) != 0) r = IMAP_SYS_ERROR;
	}
    }

    /* Still in parent, need to re-quota the mailbox*/

    /* Find its quota root */
    if (!r) {
	r = mailbox_open_header(mailboxname, imapd_authstate, &mailbox);
    }

    if(!r) {
	if(mailbox.quota.root) {
	    strcpy(quotaroot, mailbox.quota.root);
	} else {
	    strcpy(quotaroot, mailboxname);
	}
	mailbox_close(&mailbox);
    }
    
    /* Run quota -f */
    if (!r) {
	int pid;

	pid = fork();
	if(pid == -1) {
	    r = IMAP_SYS_ERROR;
	} else if(pid == 0) {
	    char buf[4096];
	    int ret;
	    
	    /* Child - exec reconstruct*/	    
	    syslog(LOG_NOTICE,
		   "Regenerating quota roots starting with '%s' for user '%s'",
		   mailboxname, imapd_userid);

	    fclose(stdin);
	    fclose(stdout);
	    fclose(stderr);

	    ret = snprintf(buf, sizeof(buf), "%s/quota", SERVICE_PATH);
	    if(ret < 0 || ret >= sizeof(buf)) {
		/* in child, so fatailing won't disconnect our user */ 
	        fatal("quota buffer not sufficiently big", EC_CONFIG);
	    }

	    execl(buf, buf, "-C", config_filename, "-f", quotaroot, NULL);
	    
	    /* if we are here, we have a problem */
	    exit(-1);
	} else {
	    int status;

	    /* Parent, wait on child */
	    if(waitpid(pid, &status, 0) < 0) r = IMAP_SYS_ERROR;

	    /* Did we fail? */
	    if(WEXITSTATUS(status) != 0) r = IMAP_SYS_ERROR;
	}
    }

    if (r) {
	prot_printf(imapd_out, "%s NO %s\r\n", tag, error_message(r));
    } else {
	prot_printf(imapd_out, "%s OK %s\r\n", tag,
		    error_message(IMAP_OK_COMPLETED));
    }
}	

/*
 * Perform a FIND command
 */
void
cmd_find(tag, namespace, pattern)
char *tag;
char *namespace;
char *pattern;
{
    char *p;
    lcase(namespace);

    for (p = pattern; *p; p++) {
	if (*p == '%') *p = '?';
    }

    /* Translate any separators in pattern */
    mboxname_hiersep_tointernal(&imapd_namespace, pattern,
				config_virtdomains ?
				strcspn(pattern, "@") : 0);

    if (!strcasecmp(namespace, "mailboxes")) {
	int force = config_getswitch(IMAPOPT_ALLOWALLSUBSCRIBE);

	(*imapd_namespace.mboxlist_findsub)(&imapd_namespace, pattern,
					    imapd_userisadmin, imapd_userid,
					    imapd_authstate, mailboxdata,
					    NULL, force);
    }
    else if (!strcasecmp(namespace, "all.mailboxes")) {
	(*imapd_namespace.mboxlist_findall)(&imapd_namespace, pattern,
					    imapd_userisadmin, imapd_userid,
					    imapd_authstate, mailboxdata, NULL);
    }
    else if (!strcasecmp(namespace, "bboards")
	     || !strcasecmp(namespace, "all.bboards")) {
	;
    }
    else {
	prot_printf(imapd_out, "%s BAD Invalid FIND subcommand\r\n", tag);
	return;
    }
    prot_printf(imapd_out, "%s OK %s\r\n", tag,
		error_message(IMAP_OK_COMPLETED));
}

static int mstringdatacalls;

/*
 * Perform a LIST or LSUB command
 */
void cmd_list(char *tag, int listopts, char *reference, char *pattern)
{
    char *buf = NULL;
    int patlen = 0;
    int reflen = 0;
    static int ignorereference = 0;
    clock_t start = clock();
    char mytime[100];
    int (*findall)(struct namespace *namespace,
		   const char *pattern, int isadmin, char *userid, 
		   struct auth_state *auth_state, int (*proc)(),
		   void *rock);
    int (*findsub)(struct namespace *namespace,
		   const char *pattern, int isadmin, char *userid, 
		   struct auth_state *auth_state, int (*proc)(),
		   void *rock, int force);

    /* Ignore the reference argument?
       (the behavior in 1.5.10 & older) */
    if (ignorereference == 0) {
	ignorereference = config_getswitch(IMAPOPT_IGNOREREFERENCE);
    }

    /* Reset state in mstringdata */
    mstringdata(NULL, NULL, 0, 0, 0);

    if (!pattern[0] && !(listopts & LIST_LSUB)) {
	/* Special case: query top-level hierarchy separator */
	prot_printf(imapd_out, "* LIST (\\Noselect) \"%c\" \"\"\r\n",
		    imapd_namespace.hier_sep);
    } else {
	/* Do we need to concatenate fields? */
	if (!ignorereference || pattern[0] == imapd_namespace.hier_sep) {
	    /* Either
	     * - name begins with dot
	     * - we're configured to honor the reference argument */

	    /* Allocate a buffer, figure out how to stick the arguments
	       together, do it, then do that instead of using pattern. */
	    patlen = strlen(pattern);
	    reflen = strlen(reference);
	    
	    buf = xmalloc(patlen + reflen + 1);
	    buf[0] = '\0';

	    if (*reference) {
		/* check for LIST A. .B, change to LIST "" A.B */
		if (reference[reflen-1] == imapd_namespace.hier_sep &&
		    pattern[0] == imapd_namespace.hier_sep) {
		    reference[--reflen] = '\0';
		}
		strcpy(buf, reference);
	    }
	    strcat(buf, pattern);
	    pattern = buf;
	}

	/* Translate any separators in pattern */
	mboxname_hiersep_tointernal(&imapd_namespace, pattern,
				    config_virtdomains ?
				    strcspn(pattern, "@") : 0);

	/* Check to see if we should only list the personal namespace */
	if (!strcmp(pattern, "*")
	    && config_getswitch(IMAPOPT_FOOLSTUPIDCLIENTS)) {
	    if (buf) free(buf);
	    buf = xstrdup("INBOX*");
	    pattern = buf;
	    findsub = mboxlist_findsub;
	    findall = mboxlist_findall;
	}
	else {
	    findsub = imapd_namespace.mboxlist_findsub;
	    findall = imapd_namespace.mboxlist_findall;
	}

	if (listopts & (LIST_LSUB | LIST_SUBSCRIBED)) {
	    int force = config_getswitch(IMAPOPT_ALLOWALLSUBSCRIBE);

	    (*findsub)(&imapd_namespace, pattern,
		       imapd_userisadmin, imapd_userid, imapd_authstate,
		       listdata, &listopts, force);
	}
	else {
	    (*findall)(&imapd_namespace, pattern,
		       imapd_userisadmin, imapd_userid, imapd_authstate,
		       listdata, &listopts);
	}

	listdata((char *)0, 0, 0, &listopts);

	if (buf) free(buf);
    }
    snprintf(mytime, sizeof(mytime), "%2.3f",
	     (clock() - start) / (double) CLOCKS_PER_SEC);
    prot_printf(imapd_out, "%s OK %s (%s secs %d calls)\r\n", tag,
		error_message(IMAP_OK_COMPLETED), mytime, mstringdatacalls);
}
  
/*
 * Perform a SUBSCRIBE (add is nonzero) or
 * UNSUBSCRIBE (add is zero) command
 */
void cmd_changesub(char *tag, char *namespace, 
		   char *name, int add)
{
    int r;
    char mailboxname[MAX_MAILBOX_NAME+1];
    int force = config_getswitch(IMAPOPT_ALLOWALLSUBSCRIBE);

    if (namespace) lcase(namespace);
    if (!namespace || !strcmp(namespace, "mailbox")) {
	int len = strlen(name);
	if (force && imapd_namespace.isalt &&
	    (((len == strlen(imapd_namespace.prefix[NAMESPACE_USER]) - 1) &&
	      !strncmp(name, imapd_namespace.prefix[NAMESPACE_USER], len)) ||
	     ((len == strlen(imapd_namespace.prefix[NAMESPACE_SHARED]) - 1) &&
	      !strncmp(name, imapd_namespace.prefix[NAMESPACE_SHARED], len)))) {
	    r = 0;
	}
	else {
	    r = (*imapd_namespace.mboxname_tointernal)(&imapd_namespace, name,
						       imapd_userid, mailboxname);
	    if (!r) {
		r = mboxlist_changesub(mailboxname, imapd_userid, 
				       imapd_authstate, add, force);
	    }
	}
    }
    else if (!strcmp(namespace, "bboard")) {
	r = add ? IMAP_MAILBOX_NONEXISTENT : 0;
    }
    else {
	prot_printf(imapd_out, "%s BAD Invalid %s subcommand\r\n", tag,
	       add ? "Subscribe" : "Unsubscribe");
	return;
    }

    if (r) {
	prot_printf(imapd_out, "%s NO %s: %s\r\n", tag,
	       add ? "Subscribe" : "Unsubscribe", error_message(r));
    }
    else {
	prot_printf(imapd_out, "%s OK %s\r\n", tag,
		    error_message(IMAP_OK_COMPLETED));
    }
}

/*
 * Perform a GETACL command
 */
void cmd_getacl(const char *tag, const char *name)
{
    char mailboxname[MAX_MAILBOX_NAME+1];
    int r, access;
    char *acl;
    char *rights, *nextid;

    r = (*imapd_namespace.mboxname_tointernal)(&imapd_namespace, name,
					       imapd_userid, mailboxname);

    if (!r) {
	r = mlookup(tag, name, mailboxname, NULL, NULL, NULL, &acl, NULL);
    }
    if (r == IMAP_MAILBOX_MOVED) return;

    if (!r) {
	access = cyrus_acl_myrights(imapd_authstate, acl);

	if (!(access & (ACL_READ|ACL_ADMIN)) &&
	    !imapd_userisadmin &&
	    !mboxname_userownsmailbox(imapd_userid, mailboxname)) {
	    r = (access&ACL_LOOKUP) ?
	      IMAP_PERMISSION_DENIED : IMAP_MAILBOX_NONEXISTENT;
	}
    }
    if (r) {
	prot_printf(imapd_out, "%s NO %s\r\n", tag, error_message(r));
	return;
    }
    
    prot_printf(imapd_out, "* ACL ");
    printastring(name);
    
    while (acl) {
	rights = strchr(acl, '\t');
	if (!rights) break;
	*rights++ = '\0';
	
	nextid = strchr(rights, '\t');
	if (!nextid) break;
	*nextid++ = '\0';
	
	prot_printf(imapd_out, " ");
	printastring(acl);
	prot_printf(imapd_out, " ");
	printastring(rights);
	acl = nextid;
    }
    prot_printf(imapd_out, "\r\n");
    prot_printf(imapd_out, "%s OK %s\r\n", tag,
		error_message(IMAP_OK_COMPLETED));
}

/*
 * Perform a LISTRIGHTS command
 */
void
cmd_listrights(tag, name, identifier)
char *tag;
char *name;
char *identifier;
{
    char mailboxname[MAX_MAILBOX_NAME+1];
    int r, rights;
    char *acl;

    r = (*imapd_namespace.mboxname_tointernal)(&imapd_namespace, name,
					       imapd_userid, mailboxname);

    if (!r) {
	r = mlookup(tag, name, mailboxname, NULL, NULL, NULL, &acl, NULL);
    }
    if (r == IMAP_MAILBOX_MOVED) return;

    if (!r) {
	rights = cyrus_acl_myrights(imapd_authstate, acl);

	if (!rights && !imapd_userisadmin &&
	    !mboxname_userownsmailbox(imapd_userid, mailboxname)) {
	    r = IMAP_MAILBOX_NONEXISTENT;
	}
    }

    if (!r) {
	struct auth_state *authstate = auth_newstate(identifier);
	char *canon_identifier;
	int canonidlen = 0;
	int implicit;
	char rightsdesc[100], optional[33];

	if (global_authisa(authstate, IMAPOPT_ADMINS))
	    canon_identifier = identifier; /* don't canonify global admins */
	else
	    canon_identifier = canonify_userid(identifier, imapd_userid, NULL);
	auth_freestate(authstate);

	if (canon_identifier) canonidlen = strlen(canon_identifier);

	if (!canon_identifier) {
	    implicit = 0;
	}
	else if (mboxname_userownsmailbox(canon_identifier, mailboxname)) {
	    /* identifier's personal mailbox */
	    implicit = config_implicitrights;
	}
	else if (mboxname_isusermailbox(mailboxname, 1)) {
	    /* anyone can post to an INBOX */
	    implicit = ACL_POST;
	}
	else {
	    implicit = 0;
	}

	/* calculate optional rights */
	cyrus_acl_masktostr(implicit ^ (canon_identifier ? ACL_FULL : 0),
			    optional);

	/* build the rights string */
	if (implicit) {
	    cyrus_acl_masktostr(implicit, rightsdesc);
	}
	else {
	    strcpy(rightsdesc, "\"\"");
	}

	if (*optional) {
	    int i, n = strlen(optional);
	    char *p = rightsdesc + strlen(rightsdesc);

	    for (i = 0; i < n; i++) {
		*p++ = ' ';
		*p++ = optional[i];
	    }
	    *p = '\0';
	}

	prot_printf(imapd_out, "* LISTRIGHTS ");
	printastring(name);
	prot_putc(' ', imapd_out);
	printastring(identifier);
	prot_printf(imapd_out, " %s", rightsdesc);

	prot_printf(imapd_out, "\r\n%s OK %s\r\n", tag,
		    error_message(IMAP_OK_COMPLETED));
	return;
    }

    prot_printf(imapd_out, "%s NO %s\r\n", tag, error_message(r));
}

/*
 * Perform a MYRIGHTS command
 */
void cmd_myrights(const char *tag, const char *name)
{
    char mailboxname[MAX_MAILBOX_NAME+1];
    int r, rights = 0;
    char *acl;
    char str[ACL_MAXSTR];

    r = (*imapd_namespace.mboxname_tointernal)(&imapd_namespace, name,
					       imapd_userid, mailboxname);

    if (!r) {
	r = mlookup(tag, name, mailboxname, NULL, NULL, NULL, &acl, NULL);
    }
    if (r == IMAP_MAILBOX_MOVED) return;

    if (!r) {
	rights = cyrus_acl_myrights(imapd_authstate, acl);

	/* Add in implicit rights */
	if (imapd_userisadmin) {
	    rights |= ACL_LOOKUP|ACL_ADMIN;
	}
	else if (mboxname_userownsmailbox(imapd_userid, mailboxname)) {
	    rights |= config_implicitrights;
	}

	if (!rights) {
	    r = IMAP_MAILBOX_NONEXISTENT;
	}
    }
    if (r) {
	prot_printf(imapd_out, "%s NO %s\r\n", tag, error_message(r));
	return;
    }
    
    prot_printf(imapd_out, "* MYRIGHTS ");
    printastring(name);
    prot_printf(imapd_out, " ");
    printastring(cyrus_acl_masktostr(rights, str));
    prot_printf(imapd_out, "\r\n%s OK %s\r\n", tag,
		error_message(IMAP_OK_COMPLETED));
}

/*
 * Perform a SETACL command
 */
void cmd_setacl(const char *tag, const char *name,
		const char *identifier, const char *rights)
{
    int r;
    char mailboxname[MAX_MAILBOX_NAME+1];

    r = (*imapd_namespace.mboxname_tointernal)(&imapd_namespace, name,
					       imapd_userid, mailboxname);

    /* is it remote? */
    if (!r) {
	r = mlookup(tag, name, mailboxname, NULL, NULL, NULL, NULL, NULL);
    }
    if (r == IMAP_MAILBOX_MOVED) return;
    
    if (!r) {
	r = mboxlist_setacl(mailboxname, identifier, rights,
			    imapd_userisadmin, imapd_userid, imapd_authstate);
    }

    if (r) {
	prot_printf(imapd_out, "%s NO %s\r\n", tag, error_message(r));
	return;
    }
    
    prot_printf(imapd_out, "%s OK %s\r\n", tag,
		error_message(IMAP_OK_COMPLETED));
}

/*
 * Perform a GETQUOTA command
 */
void
cmd_getquota(const char *tag, const char *name)
{
    int r;
    struct quota quota;
    char mailboxname[MAX_MAILBOX_NAME+1];

    if (!imapd_userisadmin) r = IMAP_PERMISSION_DENIED;
    else {
	r = (*imapd_namespace.mboxname_tointernal)(&imapd_namespace, name,
						   imapd_userid, mailboxname);
	if (!r) {
	    quota.root = mailboxname;
	    r = quota_read(&quota, NULL, 0);
	}
    }
    
    if (!r) {
	prot_printf(imapd_out, "* QUOTA ");
	printastring(name);
	prot_printf(imapd_out, " (");
	if (quota.limit >= 0) {
	    prot_printf(imapd_out, "STORAGE %lu %d",
			quota.used/QUOTA_UNITS, quota.limit);
	}
	prot_printf(imapd_out, ")\r\n");
    }

    if (r) {
	prot_printf(imapd_out, "%s NO %s\r\n", tag, error_message(r));
	return;
    }
    
    prot_printf(imapd_out, "%s OK %s\r\n", tag,
		error_message(IMAP_OK_COMPLETED));
}


/*
 * Perform a GETQUOTAROOT command
 */
void
cmd_getquotaroot(const char *tag, const char *name)
{
    char mailboxname[MAX_MAILBOX_NAME+1];
    struct mailbox mailbox;
    int r;
    int doclose = 0;

    r = (*imapd_namespace.mboxname_tointernal)(&imapd_namespace, name,
					       imapd_userid, mailboxname);

    if (!r) {
	r = mlookup(tag, name, mailboxname, NULL, NULL, NULL, NULL, NULL);
    }
    if (r == IMAP_MAILBOX_MOVED) return;

    if (!r) {
	r = mailbox_open_header(mailboxname, imapd_authstate, &mailbox);
    }

    if (!r) {
	doclose = 1;
	if (!imapd_userisadmin && !(mailbox.myrights & ACL_READ)) {
	    r = (mailbox.myrights & ACL_LOOKUP) ?
	      IMAP_PERMISSION_DENIED : IMAP_MAILBOX_NONEXISTENT;
	}
    }

    if (!r) {
	prot_printf(imapd_out, "* QUOTAROOT ");
	printastring(name);
	if (mailbox.quota.root) {
	    (*imapd_namespace.mboxname_toexternal)(&imapd_namespace,
						   mailbox.quota.root,
						   imapd_userid, mailboxname);
	    prot_printf(imapd_out, " ");
	    printastring(mailboxname);
	    r = quota_read(&mailbox.quota, NULL, 0);
	    if (!r) {
		prot_printf(imapd_out, "\r\n* QUOTA ");
		printastring(mailboxname);
		prot_printf(imapd_out, " (");
		if (mailbox.quota.limit >= 0) {
		    prot_printf(imapd_out, "STORAGE %lu %d",
				mailbox.quota.used/QUOTA_UNITS,
				mailbox.quota.limit);
		}
		prot_putc(')', imapd_out);
	    }
	}
	prot_printf(imapd_out, "\r\n");
    }

    if (doclose) mailbox_close(&mailbox);

    if (r) {
	prot_printf(imapd_out, "%s NO %s\r\n", tag, error_message(r));
	return;
    }
    
    prot_printf(imapd_out, "%s OK %s\r\n", tag,
		error_message(IMAP_OK_COMPLETED));
}

/*
 * Parse and perform a SETQUOTA command
 * The command has been parsed up to the resource list
 */
void
cmd_setquota(const char *tag, const char *quotaroot)
{
    int newquota = -1;
    int badresource = 0;
    int c;
    int force = 0;
    static struct buf arg;
    char *p;
    int r;
    char mailboxname[MAX_MAILBOX_NAME+1];

    c = prot_getc(imapd_in);
    if (c != '(') goto badlist;

    c = getword(imapd_in, &arg);
    if (c != ')' || arg.s[0] != '\0') {
	for (;;) {
	    if (c != ' ') goto badlist;
	    if (strcasecmp(arg.s, "storage") != 0) badresource = 1;
	    c = getword(imapd_in, &arg);
	    if (c != ' ' && c != ')') goto badlist;
	    if (arg.s[0] == '\0') goto badlist;
	    newquota = 0;
	    for (p = arg.s; *p; p++) {
		if (!isdigit((int) *p)) goto badlist;
		newquota = newquota * 10 + *p - '0';
                if (newquota < 0) goto badlist; /* overflow */
	    }
	    if (c == ')') break;
	}
    }
    c = prot_getc(imapd_in);
    if (c == '\r') c = prot_getc(imapd_in);
    if (c != '\n') {
	prot_printf(imapd_out, "%s BAD Unexpected extra arguments to SETQUOTA\r\n", tag);
	eatline(imapd_in, c);
	return;
    }

    if (badresource) r = IMAP_UNSUPPORTED_QUOTA;
    else if (!imapd_userisadmin && !imapd_userisproxyadmin) {
	/* need to allow proxies so that mailbox moves can set initial quota
	 * roots */
	r = IMAP_PERMISSION_DENIED;
    } else {
	/* are we forcing the creation of a quotaroot by having a leading +? */
	if(quotaroot[0] == '+') {
	    force = 1;
	    quotaroot++;
	}
	
	r = (*imapd_namespace.mboxname_tointernal)(&imapd_namespace, quotaroot,
						   imapd_userid, mailboxname);

	if (!r) {
	    r = mboxlist_setquota(mailboxname, newquota, force);
	}
    }

    if (r) {
	prot_printf(imapd_out, "%s NO %s\r\n", tag, error_message(r));
	return;
    }
    
    prot_printf(imapd_out, "%s OK %s\r\n", tag,
		error_message(IMAP_OK_COMPLETED));
    return;

 badlist:
    prot_printf(imapd_out, "%s BAD Invalid quota list in Setquota\r\n", tag);
    eatline(imapd_in, c);
}

#ifdef HAVE_SSL
/*
 * this implements the STARTTLS command, as described in RFC 2595.
 * one caveat: it assumes that no external layer is currently present.
 * if a client executes this command, information about the external
 * layer that was passed on the command line is disgarded. this should
 * be fixed.
 */
/* imaps - whether this is an imaps transaction or not */
void cmd_starttls(char *tag, int imaps)
{
    int result;
    int *layerp;

    char *auth_id;
    sasl_ssf_t ssf;

    /* SASL and openssl have different ideas about whether ssf is signed */
    layerp = (int *) &ssf;

    if (imapd_starttls_done == 1)
    {
	prot_printf(imapd_out, "%s NO TLS already active\r\n", tag);
	return;
    }

    result=tls_init_serverengine("imap",
				 5,        /* depth to verify */
				 !imaps,   /* can client auth? */
				 !imaps);  /* TLS only? */

    if (result == -1) {

	syslog(LOG_ERR, "error initializing TLS");

	if (imaps == 0) {
	    prot_printf(imapd_out, "%s NO Error initializing TLS\r\n", tag);
 	} else {
	    fatal("tls_init() failed", EC_CONFIG);
	}

	return;
    }

    if (imaps == 0)
    {
	prot_printf(imapd_out, "%s OK Begin TLS negotiation now\r\n", tag);
	/* must flush our buffers before starting tls */
	prot_flush(imapd_out);
    }
  
    result=tls_start_servertls(0, /* read */
			       1, /* write */
			       layerp,
			       &auth_id,
			       &tls_conn);

    /* if error */
    if (result==-1) {
	if (imaps == 0)	{
	    prot_printf(imapd_out, "%s NO Starttls negotiation failed\r\n", 
			tag);
	    syslog(LOG_NOTICE, "STARTTLS negotiation failed: %s", 
		   imapd_clienthost);
	    return;
	} else {
	    syslog(LOG_NOTICE, "imaps TLS negotiation failed: %s", 
		   imapd_clienthost);
	    fatal("tls_start_servertls() failed", EC_TEMPFAIL);
	    return;
	}
    }

    /* tell SASL about the negotiated layer */
    result = sasl_setprop(imapd_saslconn, SASL_SSF_EXTERNAL, &ssf);
    if (result != SASL_OK) {
	fatal("sasl_setprop() failed: cmd_starttls()", EC_TEMPFAIL);
    }
    saslprops.ssf = ssf;

    result = sasl_setprop(imapd_saslconn, SASL_AUTH_EXTERNAL, auth_id);
    if (result != SASL_OK) {
	fatal("sasl_setprop() failed: cmd_starttls()", EC_TEMPFAIL);
    }
    if(saslprops.authid) {
	free(saslprops.authid);
	saslprops.authid = NULL;
    }
    if(auth_id)
        saslprops.authid = xstrdup(auth_id);

    /* tell the prot layer about our new layers */
    prot_settls(imapd_in, tls_conn);
    prot_settls(imapd_out, tls_conn);

    imapd_starttls_done = 1;
}
#else
void cmd_starttls(char *tag, int imaps)
{
    fatal("cmd_starttls() executed, but starttls isn't implemented!",
	  EC_SOFTWARE);
}
#endif /* HAVE_SSL */

/*
 * Parse and perform a STATUS command
 * The command has been parsed up to the attribute list
 */
void
cmd_status(tag, name)
char *tag;
char *name;
{
    int c;
    int statusitems = 0;
    static struct buf arg;
    struct mailbox mailbox;
    char mailboxname[MAX_MAILBOX_NAME+1];
    int r = 0;
    int doclose = 0;

    c = prot_getc(imapd_in);
    if (c != '(') goto badlist;

    c = getword(imapd_in, &arg);
    if (arg.s[0] == '\0') goto badlist;
    for (;;) {
	lcase(arg.s);
	if (!strcmp(arg.s, "messages")) {
	    statusitems |= STATUS_MESSAGES;
	}
	else if (!strcmp(arg.s, "recent")) {
	    statusitems |= STATUS_RECENT;
	}
	else if (!strcmp(arg.s, "uidnext")) {
	    statusitems |= STATUS_UIDNEXT;
	}
	else if (!strcmp(arg.s, "uidvalidity")) {
	    statusitems |= STATUS_UIDVALIDITY;
	}
	else if (!strcmp(arg.s, "unseen")) {
	    statusitems |= STATUS_UNSEEN;
	}
	else {
	    prot_printf(imapd_out, "%s BAD Invalid Status attribute %s\r\n",
			tag, arg.s);
	    eatline(imapd_in, c);
	    return;
	}
	    
	if (c == ' ') c = getword(imapd_in, &arg);
	else break;
    }

    if (c != ')') {
	prot_printf(imapd_out,
		    "%s BAD Missing close parenthesis in Status\r\n", tag);
	eatline(imapd_in, c);
	return;
    }

    c = prot_getc(imapd_in);
    if (c == '\r') c = prot_getc(imapd_in);
    if (c != '\n') {
	prot_printf(imapd_out,
		    "%s BAD Unexpected extra arguments to Status\r\n", tag);
	eatline(imapd_in, c);
	return;
    }

    /*
     * Perform a full checkpoint of any open mailbox, in case we're
     * doing a STATUS check of the current mailbox.
     */
    if (imapd_mailbox) {
	index_check(imapd_mailbox, 0, 1);
    }

    r = (*imapd_namespace.mboxname_tointernal)(&imapd_namespace, name,
					       imapd_userid, mailboxname);

    if (!r) {
	r = mlookup(tag, name, mailboxname, NULL, NULL, NULL, NULL, NULL);
    }
    if (r == IMAP_MAILBOX_MOVED) return;

    if (!r) {
	r = mailbox_open_header(mailboxname, imapd_authstate, &mailbox);
    }

    if (!r) {
	doclose = 1;
	r = mailbox_open_index(&mailbox);
    }
    if (!r && !(mailbox.myrights & ACL_READ)) {
	r = (imapd_userisadmin || (mailbox.myrights & ACL_LOOKUP)) ?
	  IMAP_PERMISSION_DENIED : IMAP_MAILBOX_NONEXISTENT;
    }

    if (!r) {
	r = index_status(&mailbox, name, statusitems);
    }

    if (doclose) mailbox_close(&mailbox);
    
    if (r) {
	prot_printf(imapd_out, "%s NO %s\r\n", tag, error_message(r));
	return;
    }
    
    prot_printf(imapd_out, "%s OK %s\r\n", tag,
		error_message(IMAP_OK_COMPLETED));
    return;

 badlist:
    prot_printf(imapd_out, "%s BAD Invalid status list in Status\r\n", tag);
    eatline(imapd_in, c);
}

#ifdef ENABLE_X_NETSCAPE_HACK
/*
 * Reply to Netscape's crock with a crock of my own
 */
void cmd_netscrape(char *tag)
{
    const char *url;

    url = config_getstring(IMAPOPT_NETSCAPEURL);

    /* I only know of three things to reply with: */
    prot_printf(imapd_out,
		"* OK [NETSCAPE] Carnegie Mellon Cyrus IMAP\r\n"
		"* VERSION %s\r\n",
		CYRUS_VERSION);
    if (url) prot_printf(imapd_out, "* ACCOUNT-URL %s\r\n", url);
    prot_printf(imapd_out, "%s OK %s\r\n",
		tag, error_message(IMAP_OK_COMPLETED));
}
#endif /* ENABLE_X_NETSCAPE_HACK */

/* Callback for cmd_namespace to be passed to mboxlist_findall.
 * For each top-level mailbox found, print a bit of the response
 * if it is a shared namespace.  The rock is used as an integer in
 * order to ensure the namespace response is correct on a server with
 * no shared namespace.
 */
static int namespacedata(char *name,
			 int matchlen __attribute__((unused)),
			 int maycreate __attribute__((unused)),
			 void *rock)
{
    int* sawone = (int*) rock;

    if (!name) {
	return 0;
    }
    
    if (!(strncmp(name, "INBOX.", 6))) {
	/* The user has a "personal" namespace. */
	sawone[NAMESPACE_INBOX] = 1;
    } else if (mboxname_isusermailbox(name, 0)) {
	/* The user can see the "other users" namespace. */
	sawone[NAMESPACE_USER] = 1;
    } else {
	/* The user can see the "shared" namespace. */
	sawone[NAMESPACE_SHARED] = 1;
    }

    return 0;
}

/*
 * Print out a response to the NAMESPACE command defined by
 * RFC 2342.
 */
void cmd_namespace(tag)
    char* tag;
{
    int sawone[3] = {0, 0, 0};
    char* pattern;

    if (SLEEZY_NAMESPACE) {
	char inboxname[MAX_MAILBOX_NAME+1];

	if (strlen(imapd_userid) + 5 > MAX_MAILBOX_NAME)
	    sawone[NAMESPACE_INBOX] = 0;
	else {
	    (*imapd_namespace.mboxname_tointernal)(&imapd_namespace, "INBOX",
						   imapd_userid, inboxname);
	    sawone[NAMESPACE_INBOX] = 
		!mboxlist_lookup(inboxname, NULL, NULL, NULL);
	}
	sawone[NAMESPACE_USER] = 1;
	sawone[NAMESPACE_SHARED] = 1;
    } else {
	pattern = xstrdup("%");
	/* now find all the exciting toplevel namespaces -
	 * we're using internal names here
	 */
	mboxlist_findall(NULL, pattern, imapd_userisadmin, imapd_userid,
			 imapd_authstate, namespacedata, (void*) sawone);
	free(pattern);
    }

    prot_printf(imapd_out, "* NAMESPACE");
    if (sawone[NAMESPACE_INBOX]) {
	prot_printf(imapd_out, " ((\"%s\" \"%c\"))",
		    imapd_namespace.prefix[NAMESPACE_INBOX],
		    imapd_namespace.hier_sep);
    } else {
	prot_printf(imapd_out, " NIL");
    }
    if (sawone[NAMESPACE_USER]) {
	prot_printf(imapd_out, " ((\"%s\" \"%c\"))",
		    imapd_namespace.prefix[NAMESPACE_USER],
		    imapd_namespace.hier_sep);
    } else {
	prot_printf(imapd_out, " NIL");
    }
    if (sawone[NAMESPACE_SHARED]) {
	prot_printf(imapd_out, " ((\"%s\" \"%c\"))",
		    imapd_namespace.prefix[NAMESPACE_SHARED],
		    imapd_namespace.hier_sep);
    } else {
	prot_printf(imapd_out, " NIL");
    }
    prot_printf(imapd_out, "\r\n");

    prot_printf(imapd_out, "%s OK %s\r\n", tag,
		error_message(IMAP_OK_COMPLETED));
}

/*
 * Parse annotate fetch data.
 *
 * This is a generic routine which parses just the annotation data.
 * Any surrounding command text must be parsed elsewhere, ie,
 * GETANNOTATION, FETCH.
 */

int getannotatefetchdata(char *tag,
			 struct strlist **entries, struct strlist **attribs)
{
    int c;
    static struct buf arg;

    *entries = *attribs = NULL;

    c = prot_getc(imapd_in);
    if (c == EOF) {
	prot_printf(imapd_out,
		    "%s BAD Missing annotation entry\r\n", tag);
	goto baddata;
    }
    else if (c == '(') {
	/* entry list */
	do {
	    c = getqstring(imapd_in, imapd_out, &arg);
	    if (c == EOF) {
		prot_printf(imapd_out,
			    "%s BAD Missing annotation entry\r\n", tag);
		goto baddata;
	    }

	    /* add the entry to the list */
	    appendstrlist(entries, arg.s);

	} while (c == ' ');

	if (c != ')') {
	    prot_printf(imapd_out,
			"%s BAD Missing close paren in annotation entry list \r\n",
			tag);
	    goto baddata;
	}

	c = prot_getc(imapd_in);
    }
    else {
	/* single entry -- add it to the list */
	prot_ungetc(c, imapd_in);
	c = getqstring(imapd_in, imapd_out, &arg);
	if (c == EOF) {
	    prot_printf(imapd_out,
			"%s BAD Missing annotation entry\r\n", tag);
	    goto baddata;
	}

	appendstrlist(entries, arg.s);
    }

    if (c != ' ' || (c = prot_getc(imapd_in)) == EOF) {
	prot_printf(imapd_out,
		    "%s BAD Missing annotation attribute(s)\r\n", tag);
	goto baddata;
    }

    if (c == '(') {
	/* attrib list */
	do {
	    c = getnstring(imapd_in, imapd_out, &arg);
	    if (c == EOF) {
		prot_printf(imapd_out,
			    "%s BAD Missing annotation attribute(s)\r\n", tag);
		goto baddata;
	    }

	    /* add the attrib to the list */
	    appendstrlist(attribs, arg.s);

	} while (c == ' ');

	if (c != ')') {
	    prot_printf(imapd_out,
			"%s BAD Missing close paren in "
			"annotation attribute list\r\n", tag);
	    goto baddata;
	}

	c = prot_getc(imapd_in);
    }
    else {
	/* single attrib */
	prot_ungetc(c, imapd_in);
	c = getqstring(imapd_in, imapd_out, &arg);
	    if (c == EOF) {
		prot_printf(imapd_out,
			    "%s BAD Missing annotation attribute\r\n", tag);
		goto baddata;
	    }

	appendstrlist(attribs, arg.s);
   }

    return c;

  baddata:
    if (c != EOF) prot_ungetc(c, imapd_in);
    return EOF;
}

/*
 * Parse annotate store data.
 *
 * This is a generic routine which parses just the annotation data.
 * Any surrounding command text must be parsed elsewhere, ie,
 * SETANNOTATION, STORE, APPEND.
 */

int getannotatestoredata(char *tag, struct entryattlist **entryatts)
{
    int c, islist = 0;
    static struct buf entry, attrib, value;
    struct attvaluelist *attvalues = NULL;

    *entryatts = NULL;

    c = prot_getc(imapd_in);
    if (c == EOF) {
	prot_printf(imapd_out,
		    "%s BAD Missing annotation entry\r\n", tag);
	goto baddata;
    }
    else if (c == '(') {
	/* entry list */
	islist = 1;
    }
    else {
	/* single entry -- put the char back */
	prot_ungetc(c, imapd_in);
    }

    do {
	/* get entry */
	c = getqstring(imapd_in, imapd_out, &entry);
	if (c == EOF) {
	    prot_printf(imapd_out,
			"%s BAD Missing annotation entry\r\n", tag);
	    goto baddata;
	}

	/* parse att-value list */
	if (c != ' ' || (c = prot_getc(imapd_in)) != '(') {
	    prot_printf(imapd_out,
			"%s BAD Missing annotation attribute-values list\r\n",
			tag);
	    goto baddata;
	}

	do {
	    /* get attrib */
	    c = getqstring(imapd_in, imapd_out, &attrib);
	    if (c == EOF) {
		prot_printf(imapd_out,
			    "%s BAD Missing annotation attribute\r\n", tag);
		goto baddata;
	    }

	    /* get value */
	    if (c != ' ' ||
		(c = getnstring(imapd_in, imapd_out, &value)) == EOF) {
		prot_printf(imapd_out,
			    "%s BAD Missing annotation value\r\n", tag);
		goto baddata;
	    }

	    /* add the attrib-value pair to the list */
	    appendattvalue(&attvalues, attrib.s, value.s);

	} while (c == ' ');

	if (c != ')') {
	    prot_printf(imapd_out,
			"%s BAD Missing close paren in annotation "
			"attribute-values list\r\n", tag);
	    goto baddata;
	}

	/* add the entry to the list */
	appendentryatt(entryatts, entry.s, attvalues);
	attvalues = NULL;

	c = prot_getc(imapd_in);

    } while (c == ' ');

    if (islist) {
	if (c != ')') {
	    prot_printf(imapd_out,
			"%s BAD Missing close paren in annotation entry list \r\n",
			tag);
	    goto baddata;
	}

	c = prot_getc(imapd_in);
    }

    return c;

  baddata:
    if (attvalues) freeattvalues(attvalues);
    if (c != EOF) prot_ungetc(c, imapd_in);
    return EOF;
}

/*
 * Output an entry/attribute-value list response.
 *
 * This is a generic routine which outputs just the annotation data.
 * Any surrounding response text must be output elsewhere, ie,
 * GETANNOTATION, FETCH. 
 */
void annotate_response(struct entryattlist *l)
{
    int islist; /* do we have more than one entry? */

    if (!l) return;

    islist = (l->next != NULL);

    if (islist) prot_printf(imapd_out, "(");

    while (l) {
	prot_printf(imapd_out, "\"%s\"", l->entry);

	/* do we have attributes?  solicited vs. unsolicited */
	if (l->attvalues) {
	    struct attvaluelist *av = l->attvalues;

	    prot_printf(imapd_out, " (");
	    while (av) {
		prot_printf(imapd_out, "\"%s\" ", av->attrib);
		if (!strcasecmp(av->value, "NIL"))
		    prot_printf(imapd_out, "NIL");
		else
		    prot_printf(imapd_out, "\"%s\"", av->value);

		if ((av = av->next) == NULL)
		    prot_printf(imapd_out, ")");
		else
		    prot_printf(imapd_out, " ");
	    }
	}

	if ((l = l->next) != NULL)
	    prot_printf(imapd_out, " ");
    }

    if (islist) prot_printf(imapd_out, ")");
}

/*
 * Perform a GETANNOTATION command
 *
 * The command has been parsed up to the entries
 */    
void cmd_getannotation(char *tag, char *mboxpat)
{
    int c, r = 0;
    struct strlist *entries = NULL, *attribs = NULL;

    c = getannotatefetchdata(tag, &entries, &attribs);
    if (c == EOF) {
	eatline(imapd_in, c);
	return;
    }

    /* check for CRLF */
    if (c == '\r') c = prot_getc(imapd_in);
    if (c != '\n') {
	prot_printf(imapd_out,
		    "%s BAD Unexpected extra arguments to Getannotation\r\n",
		    tag);
	eatline(imapd_in, c);
	goto freeargs;
    }

    r = annotatemore_fetch(mboxpat, entries, attribs, &imapd_namespace,
			   imapd_userisadmin || imapd_userisproxyadmin,
			   imapd_userid, imapd_authstate, imapd_out);

    if (r) {
	prot_printf(imapd_out, "%s NO %s\r\n", tag, error_message(r));
    } else {
	prot_printf(imapd_out, "%s OK %s\r\n",
		    tag, error_message(IMAP_OK_COMPLETED));
    }

  freeargs:
    if (entries) freestrlist(entries);
    if (attribs) freestrlist(attribs);

    return;
}

/*
 * Perform a SETANNOTATION command
 *
 * The command has been parsed up to the entry-att list
 */    
void cmd_setannotation(char *tag, char *mboxpat)
{
    int c, r = 0;
    struct entryattlist *entryatts = NULL;

    c = getannotatestoredata(tag, &entryatts);
    if (c == EOF) {
	eatline(imapd_in, c);
	return;
    }

    /* check for CRLF */
    if (c == '\r') c = prot_getc(imapd_in);
    if (c != '\n') {
	prot_printf(imapd_out,
		    "%s BAD Unexpected extra arguments to Setannotation\r\n",
		    tag);
	eatline(imapd_in, c);
	goto freeargs;
    }

    r = annotatemore_store(mboxpat,
			   entryatts, &imapd_namespace, imapd_userisadmin,
			   imapd_userid, imapd_authstate);

    if (r) {
	prot_printf(imapd_out, "%s NO %s\r\n", tag, error_message(r));
    } else {
	prot_printf(imapd_out, "%s OK %s\r\n", tag,
		    error_message(IMAP_OK_COMPLETED));
    }

  freeargs:
    if (entryatts) freeentryatts(entryatts);
    return;
}

/*
 * Parse a search program
 */
int getsearchprogram(tag, searchargs, charset, parsecharset)
char *tag;
struct searchargs *searchargs;
int *charset;
int parsecharset;
{
    int c;

    do {
	c = getsearchcriteria(tag, searchargs, charset, parsecharset);
	parsecharset = 0;
    } while (c == ' ');
    return c;
}

/*
 * Parse a search criteria
 */
int getsearchcriteria(tag, searchargs, charset, parsecharset)
char *tag;
struct searchargs *searchargs;
int *charset;
int parsecharset;
{
    static struct buf criteria, arg;
    struct searchargs *sub1, *sub2;
    char *p, *str;
    int c, flag;
    unsigned size;
    time_t start, end;

    c = getword(imapd_in, &criteria);
    lcase(criteria.s);
    switch (criteria.s[0]) {
    case '\0':
	if (c != '(') goto badcri;
	c = getsearchprogram(tag, searchargs, charset, 0);
	if (c == EOF) return EOF;
	if (c != ')') {
	    prot_printf(imapd_out, "%s BAD Missing required close paren in Search command\r\n",
		   tag);
	    if (c != EOF) prot_ungetc(c, imapd_in);
	    return EOF;
	}
	c = prot_getc(imapd_in);
	break;

    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
    case '*':
	if (imparse_issequence(criteria.s)) {
	    appendstrlist(&searchargs->sequence, criteria.s);
	}
	else goto badcri;
	break;

    case 'a':
	if (!strcmp(criteria.s, "answered")) {
	    searchargs->system_flags_set |= FLAG_ANSWERED;
	}
	else if (!strcmp(criteria.s, "all")) {
	    break;
	}
	else goto badcri;
	break;

    case 'b':
	if (!strcmp(criteria.s, "before")) {
	    if (c != ' ') goto missingarg;		
	    c = getsearchdate(&start, &end);
	    if (c == EOF) goto baddate;
	    if (!searchargs->before || searchargs->before > start) {
		searchargs->before = start;
	    }
	}
	else if (!strcmp(criteria.s, "bcc")) {
	    if (c != ' ') goto missingarg;		
	    c = getastring(imapd_in, imapd_out, &arg);
	    if (c == EOF) goto missingarg;
	    str = charset_convert(arg.s, *charset, NULL, 0);
	    if (strchr(str, EMPTY)) {
		/* Force failure */
		searchargs->flags = (SEARCH_RECENT_SET|SEARCH_RECENT_UNSET);
	    }
	    else {
		appendstrlistpat(&searchargs->bcc, str);
	    }
	}
	else if (!strcmp(criteria.s, "body")) {
	    if (c != ' ') goto missingarg;		
	    c = getastring(imapd_in, imapd_out, &arg);
	    if (c == EOF) goto missingarg;
	    str = charset_convert(arg.s, *charset, NULL, 0);
	    if (strchr(str, EMPTY)) {
		/* Force failure */
		searchargs->flags = (SEARCH_RECENT_SET|SEARCH_RECENT_UNSET);
	    }
	    else {
		appendstrlistpat(&searchargs->body, str);
	    }
	}
	else goto badcri;
	break;

    case 'c':
	if (!strcmp(criteria.s, "cc")) {
	    if (c != ' ') goto missingarg;		
	    c = getastring(imapd_in, imapd_out, &arg);
	    if (c == EOF) goto missingarg;
	    str = charset_convert(arg.s, *charset, NULL, 0);
	    if (strchr(str, EMPTY)) {
		/* Force failure */
		searchargs->flags = (SEARCH_RECENT_SET|SEARCH_RECENT_UNSET);
	    }
	    else {
		appendstrlistpat(&searchargs->cc, str);
	    }
	}
	else if (parsecharset && !strcmp(criteria.s, "charset")) {
	    if (c != ' ') goto missingarg;		
	    c = getastring(imapd_in, imapd_out, &arg);
	    if (c != ' ') goto missingarg;
	    lcase(arg.s);
	    *charset = charset_lookupname(arg.s);
	}
	else goto badcri;
	break;

    case 'd':
	if (!strcmp(criteria.s, "deleted")) {
	    searchargs->system_flags_set |= FLAG_DELETED;
	}
	else if (!strcmp(criteria.s, "draft")) {
	    searchargs->system_flags_set |= FLAG_DRAFT;
	}
	else goto badcri;
	break;

    case 'f':
	if (!strcmp(criteria.s, "flagged")) {
	    searchargs->system_flags_set |= FLAG_FLAGGED;
	}
	else if (!strcmp(criteria.s, "from")) {
	    if (c != ' ') goto missingarg;		
	    c = getastring(imapd_in, imapd_out, &arg);
	    if (c == EOF) goto missingarg;
	    str = charset_convert(arg.s, *charset, NULL, 0);
	    if (strchr(str, EMPTY)) {
		/* Force failure */
		searchargs->flags = (SEARCH_RECENT_SET|SEARCH_RECENT_UNSET);
	    }
	    else {
		appendstrlistpat(&searchargs->from, str);
	    }
	}
	else goto badcri;
	break;

    case 'h':
	if (!strcmp(criteria.s, "header")) {
	    struct strlist **patlist;

	    if (c != ' ') goto missingarg;		
	    c = getastring(imapd_in, imapd_out, &arg);
	    if (c != ' ') goto missingarg;
	    lcase(arg.s);

	    /* some headers can be reduced to search terms */
            if (!strcmp(arg.s, "bcc")) {
                patlist = &searchargs->bcc;
            }
            else if (!strcmp(arg.s, "cc")) {
		patlist = &searchargs->cc;
            }
	    else if (!strcmp(arg.s, "to")) {
		patlist = &searchargs->to;
            }
	    else if (!strcmp(arg.s, "from")) {
		patlist = &searchargs->from;
            }
	    else if (!strcmp(arg.s, "subject")) {
		patlist = &searchargs->subject;
            }

	    /* we look message-id up in the envelope */
	    else if (!strcmp(arg.s, "message-id")) {
		patlist = &searchargs->messageid;
	    }

	    /* all other headers we handle normally */
	    else {
		if (searchargs->cache_atleast < BIT32_MAX) {
		    bit32 this_ver =
			mailbox_cached_header(arg.s);
		    if(this_ver > searchargs->cache_atleast)
			searchargs->cache_atleast = this_ver;
		}
		appendstrlist(&searchargs->header_name, arg.s);
		patlist = &searchargs->header;
	    }

	    c = getastring(imapd_in, imapd_out, &arg);
	    if (c == EOF) goto missingarg;
	    str = charset_convert(arg.s, *charset, NULL, 0);
	    if (strchr(str, EMPTY)) {
		/* Force failure */
		searchargs->flags = (SEARCH_RECENT_SET|SEARCH_RECENT_UNSET);
	    }
	    else {
		appendstrlistpat(patlist, str);
	    }
	}
	else goto badcri;
	break;

    case 'k':
	if (!strcmp(criteria.s, "keyword")) {
	    if (c != ' ') goto missingarg;		
	    c = getword(imapd_in, &arg);
	    if (!imparse_isatom(arg.s)) goto badflag;
	    lcase(arg.s);
	    for (flag=0; flag < MAX_USER_FLAGS; flag++) {
		if (imapd_mailbox->flagname[flag] &&
		    !strcasecmp(imapd_mailbox->flagname[flag], arg.s)) break;
	    }
	    if (flag == MAX_USER_FLAGS) {
		/* Force failure */
		searchargs->flags = (SEARCH_RECENT_SET|SEARCH_RECENT_UNSET);
		break;
	    }
	    searchargs->user_flags_set[flag/32] |= 1<<(flag&31);
	}
	else goto badcri;
	break;

    case 'l':
	if (!strcmp(criteria.s, "larger")) {
	    if (c != ' ') goto missingarg;		
	    c = getword(imapd_in, &arg);
	    size = 0;
	    for (p = arg.s; *p && isdigit((int) *p); p++) {
		size = size * 10 + *p - '0';
                /* if (size < 0) goto badnumber; */
	    }
	    if (!arg.s || *p) goto badnumber;
	    if (size > searchargs->larger) searchargs->larger = size;
	}
	else goto badcri;
	break;

    case 'n':
	if (!strcmp(criteria.s, "not")) {
	    if (c != ' ') goto missingarg;		
	    sub1 = (struct searchargs *)xzmalloc(sizeof(struct searchargs));
	    c = getsearchcriteria(tag, sub1, charset, 0);
	    if (c == EOF) {
		freesearchargs(sub1);
		return EOF;
	    }

	    appendsearchargs(searchargs, sub1, (struct searchargs *)0);
	}
	else if (!strcmp(criteria.s, "new")) {
	    searchargs->flags |= (SEARCH_SEEN_UNSET|SEARCH_RECENT_SET);
	}
	else goto badcri;
	break;

    case 'o':
	if (!strcmp(criteria.s, "or")) {
	    if (c != ' ') goto missingarg;		
	    sub1 = (struct searchargs *)xzmalloc(sizeof(struct searchargs));
	    c = getsearchcriteria(tag, sub1, charset, 0);
	    if (c == EOF) {
		freesearchargs(sub1);
		return EOF;
	    }
	    if (c != ' ') goto missingarg;		
	    sub2 = (struct searchargs *)xzmalloc(sizeof(struct searchargs));
	    c = getsearchcriteria(tag, sub2, charset, 0);
	    if (c == EOF) {
		freesearchargs(sub1);
		freesearchargs(sub2);
		return EOF;
	    }
	    appendsearchargs(searchargs, sub1, sub2);
	}
	else if (!strcmp(criteria.s, "old")) {
	    searchargs->flags |= SEARCH_RECENT_UNSET;
	}
	else if (!strcmp(criteria.s, "on")) {
	    if (c != ' ') goto missingarg;		
	    c = getsearchdate(&start, &end);
	    if (c == EOF) goto baddate;
	    if (!searchargs->before || searchargs->before > end) {
		searchargs->before = end;
	    }
	    if (!searchargs->after || searchargs->after < start) {
		searchargs->after = start;
	    }
	}
	else goto badcri;
	break;

    case 'r':
	if (!strcmp(criteria.s, "recent")) {
	    searchargs->flags |= SEARCH_RECENT_SET;
	}
	else goto badcri;
	break;

    case 's':
	if (!strcmp(criteria.s, "seen")) {
	    searchargs->flags |= SEARCH_SEEN_SET;
	}
	else if (!strcmp(criteria.s, "sentbefore")) {
	    if (c != ' ') goto missingarg;		
	    c = getsearchdate(&start, &end);
	    if (c == EOF) goto baddate;
	    if (!searchargs->sentbefore || searchargs->sentbefore > start) {
		searchargs->sentbefore = start;
	    }
	}
	else if (!strcmp(criteria.s, "senton")) {
	    if (c != ' ') goto missingarg;		
	    c = getsearchdate(&start, &end);
	    if (c == EOF) goto baddate;
	    if (!searchargs->sentbefore || searchargs->sentbefore > end) {
		searchargs->sentbefore = end;
	    }
	    if (!searchargs->sentafter || searchargs->sentafter < start) {
		searchargs->sentafter = start;
	    }
	}
	else if (!strcmp(criteria.s, "sentsince")) {
	    if (c != ' ') goto missingarg;		
	    c = getsearchdate(&start, &end);
	    if (c == EOF) goto baddate;
	    if (!searchargs->sentafter || searchargs->sentafter < start) {
		searchargs->sentafter = start;
	    }
	}
	else if (!strcmp(criteria.s, "since")) {
	    if (c != ' ') goto missingarg;		
	    c = getsearchdate(&start, &end);
	    if (c == EOF) goto baddate;
	    if (!searchargs->after || searchargs->after < start) {
		searchargs->after = start;
	    }
	}
	else if (!strcmp(criteria.s, "smaller")) {
	    if (c != ' ') goto missingarg;		
	    c = getword(imapd_in, &arg);
	    size = 0;
	    for (p = arg.s; *p && isdigit((int) *p); p++) {
		size = size * 10 + *p - '0';
                /* if (size < 0) goto badnumber; */
	    }
	    if (!arg.s || *p) goto badnumber;
	    if (size == 0) size = 1;
	    if (!searchargs->smaller || size < searchargs->smaller)
	      searchargs->smaller = size;
	}
	else if (!strcmp(criteria.s, "subject")) {
	    if (c != ' ') goto missingarg;		
	    c = getastring(imapd_in, imapd_out, &arg);
	    if (c == EOF) goto missingarg;
	    str = charset_convert(arg.s, *charset, NULL, 0);
	    if (strchr(str, EMPTY)) {
		/* Force failure */
		searchargs->flags = (SEARCH_RECENT_SET|SEARCH_RECENT_UNSET);
	    }
	    else {
		appendstrlistpat(&searchargs->subject, str);
	    }
	}
	else goto badcri;
	break;

    case 't':
	if (!strcmp(criteria.s, "to")) {
	    if (c != ' ') goto missingarg;		
	    c = getastring(imapd_in, imapd_out, &arg);
	    if (c == EOF) goto missingarg;
	    str = charset_convert(arg.s, *charset, NULL, 0);
	    if (strchr(str, EMPTY)) {
		/* Force failure */
		searchargs->flags = (SEARCH_RECENT_SET|SEARCH_RECENT_UNSET);
	    }
	    else {
		appendstrlistpat(&searchargs->to, str);
	    }
	}
	else if (!strcmp(criteria.s, "text")) {
	    if (c != ' ') goto missingarg;		
	    c = getastring(imapd_in, imapd_out, &arg);
	    if (c == EOF) goto missingarg;
	    str = charset_convert(arg.s, *charset, NULL, 0);
	    if (strchr(str, EMPTY)) {
		/* Force failure */
		searchargs->flags = (SEARCH_RECENT_SET|SEARCH_RECENT_UNSET);
	    }
	    else {
		appendstrlistpat(&searchargs->text, str);
	    }
	}
	else goto badcri;
	break;

    case 'u':
	if (!strcmp(criteria.s, "uid")) {
	    if (c != ' ') goto missingarg;
	    c = getword(imapd_in, &arg);
	    if (!imparse_issequence(arg.s)) goto badcri;
	    appendstrlist(&searchargs->uidsequence, arg.s);
	}
	else if (!strcmp(criteria.s, "unseen")) {
	    searchargs->flags |= SEARCH_SEEN_UNSET;
	}
	else if (!strcmp(criteria.s, "unanswered")) {
	    searchargs->system_flags_unset |= FLAG_ANSWERED;
	}
	else if (!strcmp(criteria.s, "undeleted")) {
	    searchargs->system_flags_unset |= FLAG_DELETED;
	}
	else if (!strcmp(criteria.s, "undraft")) {
	    searchargs->system_flags_unset |= FLAG_DRAFT;
	}
	else if (!strcmp(criteria.s, "unflagged")) {
	    searchargs->system_flags_unset |= FLAG_FLAGGED;
	}
	else if (!strcmp(criteria.s, "unkeyword")) {
	    if (c != ' ') goto missingarg;		
	    c = getword(imapd_in, &arg);
	    if (!imparse_isatom(arg.s)) goto badflag;
	    lcase(arg.s);
	    for (flag=0; flag < MAX_USER_FLAGS; flag++) {
		if (imapd_mailbox->flagname[flag] &&
		    !strcasecmp(imapd_mailbox->flagname[flag], arg.s)) break;
	    }
	    if (flag != MAX_USER_FLAGS) {
		searchargs->user_flags_unset[flag/32] |= 1<<(flag&31);
	    }
	}
	else goto badcri;
	break;

    default:
    badcri:
	prot_printf(imapd_out, "%s BAD Invalid Search criteria\r\n", tag);
	if (c != EOF) prot_ungetc(c, imapd_in);
	return EOF;
    }

    return c;

 missingarg:
    prot_printf(imapd_out, "%s BAD Missing required argument to Search %s\r\n",
	   tag, criteria.s);
    if (c != EOF) prot_ungetc(c, imapd_in);
    return EOF;

 badflag:
    prot_printf(imapd_out, "%s BAD Invalid flag name %s in Search command\r\n",
	   tag, arg.s);
    if (c != EOF) prot_ungetc(c, imapd_in);
    return EOF;

 baddate:
    prot_printf(imapd_out, "%s BAD Invalid date in Search command\r\n", tag);
    if (c != EOF) prot_ungetc(c, imapd_in);
    return EOF;

 badnumber:
    prot_printf(imapd_out, "%s BAD Invalid number in Search command\r\n", tag);
    if (c != EOF) prot_ungetc(c, imapd_in);
    return EOF;
}

void cmd_dump(char *tag, char *name, int uid_start) 
{
    int r = 0;
    char mailboxname[MAX_MAILBOX_NAME+1];
    char *path, *acl;

    /* administrators only please */
    if (!imapd_userisadmin) {
	r = IMAP_PERMISSION_DENIED;
    }

    if (!r) {
	r = (*imapd_namespace.mboxname_tointernal)(&imapd_namespace, name,
						   imapd_userid, mailboxname);
    }
    
    if (!r) {
	r = mlookup(tag, name, mailboxname, NULL, &path, NULL, &acl, NULL);
    }
    if (r == IMAP_MAILBOX_MOVED) return;

    if(!r) {
	r = dump_mailbox(tag, mailboxname, path, acl, uid_start, imapd_in,
			 imapd_out, imapd_authstate);
    }

    if (r) {
	prot_printf(imapd_out, "%s NO %s\r\n", tag, error_message(r));
    } else {
	prot_printf(imapd_out, "%s OK %s\r\n", tag,
		    error_message(IMAP_OK_COMPLETED));
    }
}

void cmd_undump(char *tag, char *name) 
{
    int r = 0;
    char mailboxname[MAX_MAILBOX_NAME+1];
    char *path, *acl;

    /* administrators only please */
    if (!imapd_userisadmin) {
	r = IMAP_PERMISSION_DENIED;
    }

    if (!r) {
	r = (*imapd_namespace.mboxname_tointernal)(&imapd_namespace, name,
						   imapd_userid, mailboxname);
    }
    
    if (!r) {
	r = mlookup(tag, name, mailboxname, NULL, &path, NULL, &acl, NULL);
    }
    if (r == IMAP_MAILBOX_MOVED) return;

    if(!r) {
	/* save this stuff from additional mlookups */
	char *safe_path = xstrdup(path);
	char *safe_acl = xstrdup(acl);
	r = undump_mailbox(mailboxname, safe_path, safe_acl,
			   imapd_in, imapd_out,
			   imapd_authstate);
	free(safe_path);
	free(safe_acl);
    }

    if (r) {
	prot_printf(imapd_out, "%s NO %s%s\r\n",
		    tag,
		    (r == IMAP_MAILBOX_NONEXISTENT &&
		     mboxlist_createmailboxcheck(mailboxname, 0, 0,
						 imapd_userisadmin,
						 imapd_userid, imapd_authstate,
						 NULL, NULL) == 0)
		    ? "[TRYCREATE] " : "", error_message(r));
    } else {
	prot_printf(imapd_out, "%s OK %s\r\n", tag,
		    error_message(IMAP_OK_COMPLETED));
    }
}

static int getresult(struct protstream *p, char *tag) 
{
    char buf[4096];
    char *str = (char *) buf;
    
    while(1) {
	if (!prot_fgets(str, sizeof(buf), p)) {
	    return IMAP_SERVER_UNAVAILABLE;
	}
	if (!strncmp(str, tag, strlen(tag))) {
	    str += strlen(tag);
	    if(!*str) {
		/* We got a tag, but no response */
		return IMAP_SERVER_UNAVAILABLE;
	    }
	    str++;
	    if (!strncasecmp(str, "OK ", 3)) { return 0; }
	    if (!strncasecmp(str, "NO ", 3)) { return IMAP_REMOTE_DENIED; }
	    return IMAP_SERVER_UNAVAILABLE; /* huh? */
	}
	/* skip this line, we don't really care */
    }
}

/* given 2 protstreams and a mailbox, gets the acl and then wipes it */
static int trashacl(struct protstream *pin, struct protstream *pout,
		    char *mailbox) 
{
    int i=0, j=0;
    char tagbuf[128];
    int c;		/* getword() returns an int */
    struct buf tag, cmd, tmp, user;
    int r = 0;

    memset(&tag, 0, sizeof(struct buf));
    memset(&cmd, 0, sizeof(struct buf));
    memset(&tmp, 0, sizeof(struct buf));
    memset(&user, 0, sizeof(struct buf));

    prot_printf(pout, "ACL0 GETACL {%d+}\r\n%s\r\n",
		strlen(mailbox), mailbox);

    while(1) {
	c = getword(pin, &tag);
	if (c == EOF) {
	    r = IMAP_SERVER_UNAVAILABLE;
	    break;
	}

	c = getword(pin, &cmd);
	if (c == EOF) {
	    r = IMAP_SERVER_UNAVAILABLE;
	    break;
	}
	
	if(c == '\r') {
	    c = prot_getc(pin);
	    if(c != '\n') {
		r = IMAP_SERVER_UNAVAILABLE;
		goto cleanup;
	    }
	}
	if(c == '\n') goto cleanup;	

	if (tag.s[0] == '*' && !strncmp(cmd.s, "ACL", 3)) {
	    while(c != '\n') {
		/* An ACL response, we should send a DELETEACL command */
		c = getastring(pin, pout, &tmp);
		if (c == EOF) {
		    r = IMAP_SERVER_UNAVAILABLE;
		    goto cleanup;
		}

		if(c == '\r') {
		    c = prot_getc(pin);
		    if(c != '\n') {
			r = IMAP_SERVER_UNAVAILABLE;
			goto cleanup;
		    }
		}
		if(c == '\n') goto cleanup;
		
		c = getastring(pin, pout, &user);
		if (c == EOF) {
		    r = IMAP_SERVER_UNAVAILABLE;
		    goto cleanup;
		}

		snprintf(tagbuf, sizeof(tagbuf), "ACL%d", ++i);
		
		prot_printf(pout, "%s DELETEACL {%d+}\r\n%s {%d+}\r\n%s\r\n",
			    tagbuf, strlen(mailbox), mailbox,
			    strlen(user.s), user.s);
		if(c == '\r') {
		    c = prot_getc(pin);
		    if(c != '\n') {
			r = IMAP_SERVER_UNAVAILABLE;
			goto cleanup;
		    }
		}
		/* if the next character is \n, we'll exit the loop */
	    }
	    continue;
	} else if (!strncmp(tag.s, "ACL0", 4)) {
	    /* end of this command */
	    if (!strcasecmp(cmd.s, "OK")) { break; }
	    if (!strcasecmp(cmd.s, "NO")) { r = IMAP_REMOTE_DENIED; break; }
	    r = IMAP_SERVER_UNAVAILABLE;
	    break;
	}
    }

    cleanup:

    /* Now cleanup after all the DELETEACL commands */
    if(!r) {
	while(j < i) {
	    c = getword(pin, &tag);
	    if (c == EOF) {
		r = IMAP_SERVER_UNAVAILABLE;
		break;
	    }
	    
	    eatline(pin, c);
	    
	    if(!strncmp("ACL", tag.s, 3)) {
		j++;
	    }
	}
    }

    if(r) eatline(pin, c);

    freebuf(&user);
    freebuf(&tmp);
    freebuf(&cmd);
    freebuf(&tag);

    return r;
}

static int dumpacl(struct protstream *pin, struct protstream *pout,
		   char *mailbox, char *acl_in) 
{
    int r = 0;
    int c;		/* getword() returns an int */
    char tag[128];
    int tagnum = 1;
    char *rights, *nextid;
    int mailboxlen = strlen(mailbox);
    char *acl_safe = acl_in ? xstrdup(acl_in) : NULL;
    char *acl = acl_safe;
    struct buf inbuf;
    
    memset(&inbuf, 0, sizeof(struct buf));

    while (acl) {
	rights = strchr(acl, '\t');
	if (!rights) break;
	*rights++ = '\0';
	
	nextid = strchr(rights, '\t');
	if (!nextid) break;
	*nextid++ = '\0';

	snprintf(tag, sizeof(tag), "SACL%d", tagnum++);
	
	prot_printf(pout, "%s SETACL {%d+}\r\n%s {%d+}\r\n%s {%d+}\r\n%s\r\n",
		    tag,
		    mailboxlen, mailbox,
		    strlen(acl), acl,
		    strlen(rights), rights);

	while(1) {
	    c = getword(pin, &inbuf);
	    if (c == EOF) {
		r = IMAP_SERVER_UNAVAILABLE;
		break;
	    }
	    if(strncmp(tag, inbuf.s, strlen(tag))) {
		eatline(pin, c);
		continue;
	    } else {
		/* this is our line */
		break;
	    }
	}

	/* Are we OK? */

	c = getword(pin, &inbuf);
	if (c == EOF) {
	    r = IMAP_SERVER_UNAVAILABLE;
	    break;
	}

	if(strncmp("OK", inbuf.s, 2)) {
	    r = IMAP_REMOTE_DENIED;
	    break;
	}

	/* Eat the line and get the next one */
	eatline(pin, c);
	acl = nextid;
    }

    freebuf(&inbuf);
    if(acl_safe) free(acl_safe);

    return r;
}

static int do_xfer_single(char *toserver, char *topart,
			  char *name, char *mailboxname,
			  int mbflags, 
			  char *path, char *part, char *acl,
			  int prereserved,
			  mupdate_handle *h_in,
			  struct backend *be_in) 
{
    int r = 0, rerr = 0;
    char buf[MAX_PARTITION_LEN+HOSTNAME_SIZE+2];
    struct backend *be = NULL;
    mupdate_handle *mupdate_h = NULL;
    int backout_mupdate = 0;
    int backout_remotebox = 0;
    int backout_remoteflag = 0;

    /* Make sure we're given a sane value */
    if(topart && !imparse_isatom(topart)) {
	return IMAP_PARTITION_UNKNOWN;
    }

    if(!strcmp(toserver, config_servername)) {
	return IMAP_BAD_SERVER;
    }
    
    /* Okay, we have the mailbox, now the order of steps is:
     *
     * 1) Connect to remote server.
     * 2) LOCALCREATE on remote server
     * 2.5) Set mailbox as REMOTE on local server
     * 3) mupdate.DEACTIVATE(mailbox, remoteserver) xxx what partition?
     * 4) undump mailbox from local to remote
     * 5) Sync remote acl
     * 6) mupdate.ACTIVATE(mailbox, remoteserver)
     * ** MAILBOX NOW LIVING ON REMOTE SERVER
     * 6.5) force remote server to push the final mupdate entry to ensure
     *      that the state of the world is correct (required if we do not
     *      know the remote partition, but worst case it will be caught
     *      when they next sync)
     * 7) local delete of mailbox
     * 8) remove local remote mailbox entry??????
     */

    /* Step 1: Connect to remote server */
    if(!r && !be_in) {
	/* Just authorize as the IMAP server, so pass "" as our authzid */
	be = backend_connect(NULL, toserver, &protocol[PROTOCOL_IMAP], "", NULL);
	if(!be) r = IMAP_SERVER_UNAVAILABLE;
	if(r) syslog(LOG_ERR,
		     "Could not move mailbox: %s, Backend connect failed",
		     mailboxname);
    } else if(!r) {
	be = be_in;
    }

    /* Step 1a: Connect to mupdate (as needed) */
    if(h_in) {
	mupdate_h = h_in;
    } else if (config_mupdate_server) {
	r = mupdate_connect(config_mupdate_server, NULL, &mupdate_h, NULL);
	if(r) {
	    syslog(LOG_ERR,
		   "Could not move mailbox: %s, MUPDATE connect failed",
		   mailboxname);
	    goto done;
	}

    }

    /* Step 2: LOCALCREATE on remote server */
    if(!r) {
	if(topart) {
	    /* need to send partition as an atom */
	    prot_printf(be->out, "LC1 LOCALCREATE {%d+}\r\n%s %s\r\n",
			strlen(name), name, topart);
	} else {
	    prot_printf(be->out, "LC1 LOCALCREATE {%d+}\r\n%s\r\n",
			strlen(name), name);
	}
	r = getresult(be->in, "LC1");
	if(r) syslog(LOG_ERR, "Could not move mailbox: %s, LOCALCREATE failed",
		     mailboxname);
	else backout_remotebox = 1;
    }

    /* Step 2.5: Set mailbox as REMOTE on local server */
    if(!r) {
	snprintf(buf, sizeof(buf), "%s!%s", toserver, part);
	r = mboxlist_update(mailboxname, mbflags|MBTYPE_MOVING, buf, acl, 1);
	if(r) syslog(LOG_ERR, "Could not move mailbox: %s, " \
		     "mboxlist_update failed", mailboxname);
    }

    /* Step 3: mupdate.DEACTIVATE(mailbox, newserver) */
    /* (only if mailbox has not been already deactivated by our caller) */
    if(!r && mupdate_h && !prereserved) {
	backout_remoteflag = 1;

	/* Note we are making the reservation on OUR host so that recovery
	 * make sense */
	snprintf(buf, sizeof(buf), "%s!%s", config_servername, part);
	r = mupdate_deactivate(mupdate_h, mailboxname, buf);
	if(r) syslog(LOG_ERR,
		     "Could not move mailbox: %s, MUPDATE DEACTIVATE failed",
		     mailboxname);
    }

    /* Step 4: Dump local -> remote */
    if(!r) {
	backout_mupdate = 1;

	prot_printf(be->out, "D01 UNDUMP {%d+}\r\n%s ", strlen(name), name);

	r = dump_mailbox(NULL, mailboxname, path, acl, 0, be->in, be->out,
			 imapd_authstate);

	if(r)
	    syslog(LOG_ERR,
		   "Could not move mailbox: %s, dump_mailbox() failed",
		   mailboxname);
    }

    if(!r) {
	r = getresult(be->in, "D01");
	if(r) syslog(LOG_ERR, "Could not move mailbox: %s, UNDUMP failed",
		     mailboxname);
    }
    
    /* Step 5: Set ACL on remote */
    if(!r) {
	r = trashacl(be->in, be->out, name);
	if(r) syslog(LOG_ERR, "Could not clear remote acl on %s",
		     mailboxname);
    }
    if(!r) {
	r = dumpacl(be->in, be->out, name, acl);
	if(r) syslog(LOG_ERR, "Could not set remote acl on %s",
		     mailboxname);
    }

    /* Step 6: mupdate.activate(mailbox, remote) */
    /* We do this from the local server first so that recovery is easier */
    if(!r && mupdate_h) {
	/* Note the flag that we don't have a valid partiton at the moment */
	snprintf(buf, sizeof(buf), "%s!MOVED", toserver);
	r = mupdate_activate(mupdate_h, mailboxname, buf, acl);
    }
    
    /* MAILBOX NOW LIVES ON REMOTE */
    if(!r) {
	backout_remotebox = 0;
	backout_mupdate = 0;
	backout_remoteflag = 0;

	/* 6.5) Kick remote server to correct mupdate entry */
	/* Note that we don't really care if this succeeds or not */
	if (mupdate_h) {
	    prot_printf(be->out, "MP1 MUPDATEPUSH {%d+}\r\n%s\r\n",
			strlen(name), name);
	    rerr = getresult(be->in, "MP1");
	    if(rerr) {
		syslog(LOG_ERR,
		       "Could not trigger remote push to mupdate server" \
		       "during move of %s",
		       mailboxname);
	    }
	}
    }

    /* 7) local delete of mailbox
     * & remove local "remote" mailboxlist entry */
    if(!r) {
	/* Note that we do not check the ACL, and we don't update MUPDATE */
	/* note also that we need to remember to let proxyadmins do this */
	r = mboxlist_deletemailbox(mailboxname,
				   imapd_userisadmin || imapd_userisproxyadmin,
				   imapd_userid, imapd_authstate, 0, 1, 0);
	if(r) syslog(LOG_ERR,
		     "Could not delete local mailbox during move of %s",
		     mailboxname);
     }

done:
    if(r && mupdate_h && backout_mupdate) {
	rerr = 0;
	/* xxx if the mupdate server is what failed, then this won't
	   help any! */
	snprintf(buf, sizeof(buf), "%s!%s", config_servername, part);
	rerr = mupdate_activate(mupdate_h, mailboxname, buf, acl);
	if(rerr) {
	    syslog(LOG_ERR,
		   "Could not back out mupdate during move of %s (%s)",
		   mailboxname, error_message(rerr));
	}
    }
    if(r && backout_remotebox) {
	rerr = 0;
	prot_printf(be->out, "LD1 LOCALDELETE {%d+}\r\n%s\r\n",
		    strlen(name), name);
	rerr = getresult(be->in, "LD1");
 	if(rerr) {
	    syslog(LOG_ERR,
		   "Could not back out remote mailbox during move of %s (%s)",
		   name, error_message(rerr));
	}   
    }
    if(r && backout_remoteflag) {
	rerr = 0;

	rerr = mboxlist_update(mailboxname, mbflags, part, acl, 1);
	if(rerr) syslog(LOG_ERR, "Could not unset remote flag on mailbox: %s",
			mailboxname);
    }

    /* release the handles we got locally if necessary */
    if(mupdate_h && !h_in)
	mupdate_disconnect(&mupdate_h);
    if(be && !be_in)
	backend_disconnect(be, &protocol[PROTOCOL_IMAP]);

    return r;
}

struct xfer_user_rock 
{
    char *toserver;
    char *topart;
    mupdate_handle *h;
    struct backend *be;
};

static int xfer_user_cb(char *name,
			int matchlen __attribute__((unused)),
			int maycreate __attribute__((unused)),
			void *rock) 
{
    mupdate_handle *mupdate_h = ((struct xfer_user_rock *)rock)->h;
    char *toserver = ((struct xfer_user_rock *)rock)->toserver;
    char *topart = ((struct xfer_user_rock *)rock)->topart;
    struct backend *be = ((struct xfer_user_rock *)rock)->be;
    char externalname[MAX_MAILBOX_NAME+1];
    int mbflags;
    int r = 0;
    char *inpath, *inpart, *inacl;
    char *path = NULL, *part = NULL, *acl = NULL;

    if (!r) {
	/* NOTE: NOT mlookup() because we don't want to issue a referral */
	/* xxx but what happens if they are remote
	 * mailboxes? */
	r = mboxlist_detail(name, &mbflags,
			    &inpath, &inpart, &inacl, NULL);
    }
    
    if (!r) {
	path = xstrdup(inpath);
	part = xstrdup(inpart);
	acl = xstrdup(inacl);
    }

    if (!r) {
	r = (*imapd_namespace.mboxname_toexternal)(&imapd_namespace,
						   name,
						   imapd_userid,
						   externalname);
    }

    if(!r) {
	r = do_xfer_single(toserver, topart, externalname, name, mbflags,
			   path, part, acl, 0, mupdate_h, be);
    }

    if(path) free(path);
    if(part) free(part);
    if(acl) free(acl);

    return r;
}


void cmd_xfer(char *tag, char *name, char *toserver, char *topart)
{
    int r = 0;
    char buf[MAX_PARTITION_LEN+HOSTNAME_SIZE+2];
    char mailboxname[MAX_MAILBOX_NAME+1];
    int mbflags;
    int moving_user = 0;
    int backout_mupdate = 0;
    mupdate_handle *mupdate_h = NULL;
    char *inpath, *inpart, *inacl;
    char *path = NULL, *part = NULL, *acl = NULL;
    char *p, *mbox = mailboxname;
    
    /* administrators only please */
    /* however, proxys can do this, if their authzid is an admin */
    if (!imapd_userisadmin && !imapd_userisproxyadmin) {
	r = IMAP_PERMISSION_DENIED;
    }

    if (!r) {
	r = (*imapd_namespace.mboxname_tointernal)(&imapd_namespace,
						   name,
						   imapd_userid,
						   mailboxname);
    }

    /* NOTE: Since XFER can only be used by an admin, and we always connect
     * to the destination backend as an admin, we take advantage of the fact
     * that admins *always* use a consistent mailbox naming scheme.
     * So, 'name' should be used in any command we send to a backend, and
     * 'mailboxname' is the internal name to be used for mupdate and findall.
     */

    if (config_virtdomains && (p = strchr(mailboxname, '!'))) {
	/* pointer to mailbox w/o domain prefix */
	mbox = p + 1;
    }

    if(!strncmp(mbox, "user.", 5) && !strchr(mbox+5, '.')) {
	if ((strlen(mbox+5) == (strlen(imapd_userid) - (mbox - mailboxname))) &&
	    !strncmp(mbox+5, imapd_userid, strlen(mbox+5))) {
	    /* don't move your own inbox, that could be troublesome */
	    r = IMAP_MAILBOX_NOTSUPPORTED;
	} else if (!config_getswitch(IMAPOPT_ALLOWUSERMOVES)) {
	    /* not configured to allow user moves */
	    r = IMAP_MAILBOX_NOTSUPPORTED;
	} else {
	    moving_user = 1;
	}
    }
    
    if (!r) {
	r = mlookup(tag, name, mailboxname, &mbflags,
		    &inpath, &inpart, &inacl, NULL);
    }
    if (r == IMAP_MAILBOX_MOVED) return;
    
    if (!r) {
	path = xstrdup(inpath);
	part = xstrdup(inpart);
	acl = xstrdup(inacl);
    }

    /* if we are not moving a user, just move the one mailbox */
    if(!r && !moving_user) {
	r = do_xfer_single(toserver, topart, name, mailboxname, mbflags,
			   path, part, acl, 0, NULL, NULL);
    } else if (!r) {
	struct backend *be = NULL;
	
	/* we need to reserve the users inbox - connect to mupdate */
	if(!r && config_mupdate_server) {
	    r = mupdate_connect(config_mupdate_server, NULL, &mupdate_h, NULL);
	    if(r) {
		syslog(LOG_ERR,
		       "Could not move mailbox: %s, MUPDATE connect failed",
		       mailboxname);
		goto done;
	    }
	}

	/* Get a single connection to the remote backend */
	be = backend_connect(NULL, toserver, &protocol[PROTOCOL_IMAP], "", NULL);
	if(!be) {
	    r = IMAP_SERVER_UNAVAILABLE;
	    syslog(LOG_ERR,
		   "Could not move mailbox: %s, " \
		   "Initial backend connect failed",
		   mailboxname);
	}

	/* deactivate their inbox */
	if(!r && mupdate_h) {
	    /* Note we are making the reservation on OUR host so that recovery
	     * make sense */
	    snprintf(buf, sizeof(buf), "%s!%s", config_servername, part);
	    r = mupdate_deactivate(mupdate_h, mailboxname, buf);
	    if(r) syslog(LOG_ERR,
			 "Could deactivate mailbox: %s, during move",
			 mailboxname);
	    else backout_mupdate = 1;
	}

	/* If needed, set an uppermost quota root */
	if(!r) {
	    struct quota quota;
	    
	    quota.root = mailboxname;
	    r = quota_read(&quota, NULL, 0);
	    
	    if(!r) {
		/* note use of + to force the setting of a nonexistant
		 * quotaroot */
		prot_printf(be->out, "Q01 SETQUOTA {%d+}\r\n" \
			    "+%s (STORAGE %d)\r\n",
			    strlen(name)+1, name, quota.limit);
		r = getresult(be->in, "Q01");
		if(r) syslog(LOG_ERR,
			     "Could not move mailbox: %s, " \
			     "failed setting initial quota root\r\n",
			     mailboxname);
	    }
	    else if (r == IMAP_QUOTAROOT_NONEXISTENT) r = 0;
	}


	/* recursively move all sub-mailboxes, using internal names */
	if(!r) {
	    struct xfer_user_rock rock;

	    rock.toserver = toserver;
	    rock.topart = topart;
	    rock.h = mupdate_h;
	    rock.be = be;

	    snprintf(buf, sizeof(buf), "%s.*", mailboxname);
	    r = mboxlist_findall(NULL, buf, 1, imapd_userid,
				 imapd_authstate, xfer_user_cb,
				 &rock);
	}

	/* xxx how do you back out if one of the above moves fails? */
	    
	/* move this mailbox */
	/* ...and seen file, and subs file, and sieve scripts... */
	if(!r) {
	    r = do_xfer_single(toserver, topart, name, mailboxname, mbflags,
			       path, part, acl, 1, mupdate_h, be);
	}

	if(be) {
	    backend_disconnect(be, &protocol[PROTOCOL_IMAP]);
	    free(be);
	}

	if(r && mupdate_h && backout_mupdate) {
	    int rerr = 0;
	    /* xxx if the mupdate server is what failed, then this won't
	       help any! */
	    snprintf(buf, sizeof(buf), "%s!%s", config_servername, part);
	    rerr = mupdate_activate(mupdate_h, mailboxname, buf, acl);
	    if(rerr) {
		syslog(LOG_ERR,
		       "Could not back out mupdate during move of %s (%s)",
		       mailboxname, error_message(rerr));
	    }
	} else if(!r) {
	    /* this was a successful user delete, and we need to delete
	       certain user meta-data (but not seen state!) */
	    user_deletedata(mailboxname+5, imapd_userid, imapd_authstate, 0);
	}
	
	if(!r && mupdate_h) {
	    mupdate_disconnect(&mupdate_h);
	}
    }

 done:
    if(part) free(part);
    if(path) free(path);
    if(acl) free(acl);

    if (r) {
	prot_printf(imapd_out, "%s NO %s\r\n",
		    tag,
		    error_message(r));
    } else {
	prot_printf(imapd_out, "%s OK %s\r\n", tag,
		    error_message(IMAP_OK_COMPLETED));
    }

    return;
}

/*
 * Parse a "date", for SEARCH criteria
 * The time_t's pointed to by 'start' and 'end' are set to the
 * times of the start and end of the parsed date.
 */
int getsearchdate(start, end)
time_t *start, *end;
{
    int c;
    struct tm tm;
    int quoted = 0;
    char month[4];

    memset(&tm, 0, sizeof tm);

    c = prot_getc(imapd_in);
    if (c == '\"') {
	quoted++;
	c = prot_getc(imapd_in);
    }

    /* Day of month */
    if (!isdigit(c)) goto baddate;
    tm.tm_mday = c - '0';
    c = prot_getc(imapd_in);
    if (isdigit(c)) {
	tm.tm_mday = tm.tm_mday * 10 + c - '0';
	c = prot_getc(imapd_in);
    }
    
    if (c != '-') goto baddate;
    c = prot_getc(imapd_in);

    /* Month name */
    if (!isalpha(c)) goto baddate;
    month[0] = c;
    c = prot_getc(imapd_in);
    if (!isalpha(c)) goto baddate;
    month[1] = c;
    c = prot_getc(imapd_in);
    if (!isalpha(c)) goto baddate;
    month[2] = c;
    c = prot_getc(imapd_in);
    month[3] = '\0';
    lcase(month);

    for (tm.tm_mon = 0; tm.tm_mon < 12; tm.tm_mon++) {
	if (!strcmp(month, monthname[tm.tm_mon])) break;
    }
    if (tm.tm_mon == 12) goto baddate;

    if (c != '-') goto baddate;
    c = prot_getc(imapd_in);

    /* Year */
    if (!isdigit(c)) goto baddate;
    tm.tm_year = c - '0';
    c = prot_getc(imapd_in);
    if (!isdigit(c)) goto baddate;
    tm.tm_year = tm.tm_year * 10 + c - '0';
    c = prot_getc(imapd_in);
    if (isdigit(c)) {
	if (tm.tm_year < 19) goto baddate;
	tm.tm_year -= 19;
	tm.tm_year = tm.tm_year * 10 + c - '0';
	c = prot_getc(imapd_in);
	if (!isdigit(c)) goto baddate;
	tm.tm_year = tm.tm_year * 10 + c - '0';
	c = prot_getc(imapd_in);
    }

    if (quoted) {
	if (c != '\"') goto baddate;
	c = prot_getc(imapd_in);
    }

    tm.tm_isdst = -1;
    *start = mktime(&tm);

    tm.tm_sec = tm.tm_min = 59;
    tm.tm_hour = 23;
    tm.tm_isdst = -1;
    *end = mktime(&tm);

    return c;

 baddate:
    prot_ungetc(c, imapd_in);
    return EOF;
}

#define SORTGROWSIZE	10

/*
 * Parse sort criteria
 */
int getsortcriteria(char *tag, struct sortcrit **sortcrit)
{
    int c;
    static struct buf criteria;
    int nsort, n;

    *sortcrit = NULL;

    c = prot_getc(imapd_in);
    if (c != '(') goto missingcrit;

    c = getword(imapd_in, &criteria);
    if (criteria.s[0] == '\0') goto missingcrit;

    nsort = 0;
    n = 0;
    for (;;) {
	if (n >= nsort - 1) {	/* leave room for implicit criterion */
	    /* (Re)allocate an array for sort criteria */
	    nsort += SORTGROWSIZE;
	    *sortcrit =
		(struct sortcrit *) xrealloc(*sortcrit,
					     nsort * sizeof(struct sortcrit));
	    /* Zero out the newly added sortcrit */
	    memset((*sortcrit)+n, 0, SORTGROWSIZE * sizeof(struct sortcrit));
	}

	lcase(criteria.s);
	if (!strcmp(criteria.s, "reverse")) {
	    (*sortcrit)[n].flags |= SORT_REVERSE;
	    goto nextcrit;
	}
	else if (!strcmp(criteria.s, "arrival"))
	    (*sortcrit)[n].key = SORT_ARRIVAL;
	else if (!strcmp(criteria.s, "cc"))
	    (*sortcrit)[n].key = SORT_CC;
	else if (!strcmp(criteria.s, "date"))
	    (*sortcrit)[n].key = SORT_DATE;
	else if (!strcmp(criteria.s, "from"))
	    (*sortcrit)[n].key = SORT_FROM;
	else if (!strcmp(criteria.s, "size"))
	    (*sortcrit)[n].key = SORT_SIZE;
	else if (!strcmp(criteria.s, "subject"))
	    (*sortcrit)[n].key = SORT_SUBJECT;
	else if (!strcmp(criteria.s, "to"))
	    (*sortcrit)[n].key = SORT_TO;
#if 0
	else if (!strcmp(criteria.s, "annotation")) {
	    (*sortcrit)[n].key = SORT_ANNOTATION;
	    if (c != ' ') goto missingarg;
	    c = getstring(imapd_in, &arg);
	    if (c != ' ') goto missingarg;
	    (*sortcrit)[n].args.annot.entry = xstrdup(arg.s);
	    c = getstring(imapd_in, &arg);
	    if (c == EOF) goto missingarg;
	    (*sortcrit)[n].args.annot.attrib = xstrdup(arg.s);
	}
#endif
	else {
	    prot_printf(imapd_out, "%s BAD Invalid Sort criterion %s\r\n",
			tag, criteria.s);
	    if (c != EOF) prot_ungetc(c, imapd_in);
	    return EOF;
	}

	n++;

 nextcrit:
	if (c == ' ') c = getword(imapd_in, &criteria);
	else break;
    }

    if ((*sortcrit)[n].flags & SORT_REVERSE  && !(*sortcrit)[n].key) {
	prot_printf(imapd_out,
		    "%s BAD Missing Sort criterion to reverse\r\n", tag);
	if (c != EOF) prot_ungetc(c, imapd_in);
	return EOF;
    }

    if (c != ')') {
	prot_printf(imapd_out,
		    "%s BAD Missing close parenthesis in Sort\r\n", tag);
	if (c != EOF) prot_ungetc(c, imapd_in);
	return EOF;
    }

    /* Terminate the list with the implicit sort criterion */
    (*sortcrit)[n++].key = SORT_SEQUENCE;

    c = prot_getc(imapd_in);

    return c;

 missingcrit:
    prot_printf(imapd_out, "%s BAD Missing Sort criteria\r\n", tag);
    if (c != EOF) prot_ungetc(c, imapd_in);
    return EOF;
#if 0 /* For annotations stuff above */
 missingarg:
    prot_printf(imapd_out, "%s BAD Missing argument to Sort criterion %s\r\n",
		tag, criteria.s);
    if (c != EOF) prot_ungetc(c, imapd_in);
    return EOF;
#endif
}

#ifdef ENABLE_LISTEXT
/*
 * Parse LIST options.
 * The command has been parsed up to and including the opening '('.
 */
int getlistopts(char *tag, int *listopts)
{
    int c;
    static struct buf arg;

    *listopts = LIST_EXT;

    for (;;) {
	c = getword(imapd_in, &arg);
	if (!arg.s[0]) break;

	lcase(arg.s);
	if (!strcmp(arg.s, "subscribed")) {
	    *listopts |= LIST_SUBSCRIBED;
	}
	else if (!strcmp(arg.s, "children")) {
	    *listopts |= LIST_CHILDREN;
	}
	else if (!strcmp(arg.s, "remote")) {
	    *listopts |= LIST_REMOTE;
	}
	else {
	    prot_printf(imapd_out, "%s BAD Invalid List option %s\r\n",
			tag, arg.s);
	    return EOF;
	}

	if (c != ' ') break;
    }

    if (c != ')') {
	prot_printf(imapd_out,
		    "%s BAD Missing close parenthesis in List\r\n", tag);
	return EOF;
    }

    c = prot_getc(imapd_in);

    return c;
}
#endif /* ENABLE_LISTEXT */

/*
 * Parse a date_time, for the APPEND command
 */
int getdatetime(date)
time_t *date;
{
    int c;
    struct tm tm;
    int old_format = 0;
    char month[4], zone[4], *p;
    time_t tmp_gmtime;
    int zone_off;

    memset(&tm, 0, sizeof tm);

    c = prot_getc(imapd_in);
    if (c != '\"') goto baddate;
    
    /* Day of month */
    c = prot_getc(imapd_in);
    if (c == ' ') c = '0';
    if (!isdigit(c)) goto baddate;
    tm.tm_mday = c - '0';
    c = prot_getc(imapd_in);
    if (isdigit(c)) {
	tm.tm_mday = tm.tm_mday * 10 + c - '0';
	c = prot_getc(imapd_in);
	if(tm.tm_mday <= 0 || tm.tm_mday > 31)
	    goto baddate;
    }
    
    if (c != '-') goto baddate;
    c = prot_getc(imapd_in);

    /* Month name */
    if (!isalpha(c)) goto baddate;
    month[0] = c;
    c = prot_getc(imapd_in);
    if (!isalpha(c)) goto baddate;
    month[1] = c;
    c = prot_getc(imapd_in);
    if (!isalpha(c)) goto baddate;
    month[2] = c;
    c = prot_getc(imapd_in);
    month[3] = '\0';
    lcase(month);

    for (tm.tm_mon = 0; tm.tm_mon < 12; tm.tm_mon++) {
	if (!strcmp(month, monthname[tm.tm_mon])) break;
    }
    if (tm.tm_mon == 12) goto baddate;
    /* xxx this doesn't quite work in leap years */
    if (tm.tm_mday > max_monthdays[tm.tm_mon]) goto baddate;

    if (c != '-') goto baddate;
    c = prot_getc(imapd_in);

    /* Year */
    if (!isdigit(c)) goto baddate;
    tm.tm_year = c - '0';
    c = prot_getc(imapd_in);
    if (!isdigit(c)) goto baddate;
    tm.tm_year = tm.tm_year * 10 + c - '0';
    c = prot_getc(imapd_in);
    if (isdigit(c)) {
	if (tm.tm_year < 19) goto baddate;
	tm.tm_year -= 19;
	tm.tm_year = tm.tm_year * 10 + c - '0';
	c = prot_getc(imapd_in);
	if (!isdigit(c)) goto baddate;
	tm.tm_year = tm.tm_year * 10 + c - '0';
	c = prot_getc(imapd_in);
    }
    else old_format++;

    /* Hour */
    if (c != ' ') goto baddate;
    c = prot_getc(imapd_in);
    if (!isdigit(c)) goto baddate;
    tm.tm_hour = c - '0';
    c = prot_getc(imapd_in);
    if (!isdigit(c)) goto baddate;
    tm.tm_hour = tm.tm_hour * 10 + c - '0';
    c = prot_getc(imapd_in);
    if (tm.tm_hour > 23) goto baddate;

    /* Minute */
    if (c != ':') goto baddate;
    c = prot_getc(imapd_in);
    if (!isdigit(c)) goto baddate;
    tm.tm_min = c - '0';
    c = prot_getc(imapd_in);
    if (!isdigit(c)) goto baddate;
    tm.tm_min = tm.tm_min * 10 + c - '0';
    c = prot_getc(imapd_in);
    if (tm.tm_min > 59) goto baddate;

    /* Second */
    if (c != ':') goto baddate;
    c = prot_getc(imapd_in);
    if (!isdigit(c)) goto baddate;
    tm.tm_sec = c - '0';
    c = prot_getc(imapd_in);
    if (!isdigit(c)) goto baddate;
    tm.tm_sec = tm.tm_sec * 10 + c - '0';
    c = prot_getc(imapd_in);
    if (tm.tm_min > 60) goto baddate;

    /* Time zone */
    if (old_format) {
	if (c != '-') goto baddate;
	c = prot_getc(imapd_in);

	if (!isalpha(c)) goto baddate;
	zone[0] = c;
	c = prot_getc(imapd_in);

	if (c == '\"') {
	    /* Military (single-char) zones */
	    zone[1] = '\0';
	    lcase(zone);
	    if (zone[0] <= 'm') {
		zone_off = (zone[0] - 'a' + 1)*60;
	    }
	    else if (zone[0] < 'z') {
		zone_off = ('m' - zone[0])*60;
	    }
	    else zone_off = 0;
	}
	else {
	    /* UT (universal time) */
	    zone[1] = c;
	    c = prot_getc(imapd_in);
	    if (c == '\"') {
		zone[2] = '\0';
		lcase(zone);
		if (!strcmp(zone, "ut")) goto baddate;
		zone_off = 0;
	    }
	    else {
		/* 3-char time zone */
		zone[2] = c;
		c = prot_getc(imapd_in);
		if (c != '\"') goto baddate;
		zone[3] = '\0';
		lcase(zone);
		p = strchr("aecmpyhb", zone[0]);
		if (c != '\"' || zone[2] != 't' || !p) goto baddate;
		zone_off = (strlen(p) - 12)*60;
		if (zone[1] == 'd') zone_off -= 60;
		else if (zone[1] != 's') goto baddate;
	    }
	}
    }
    else {
	if (c != ' ') goto baddate;
	c = prot_getc(imapd_in);

	if (c != '+' && c != '-') goto baddate;
	zone[0] = c;

	c = prot_getc(imapd_in);
	if (!isdigit(c)) goto baddate;
	zone_off = c - '0';
	c = prot_getc(imapd_in);
	if (!isdigit(c)) goto baddate;
	zone_off = zone_off * 10 + c - '0';
	c = prot_getc(imapd_in);
	if (!isdigit(c)) goto baddate;
	zone_off = zone_off * 6 + c - '0';
	c = prot_getc(imapd_in);
	if (!isdigit(c)) goto baddate;
	zone_off = zone_off * 10 + c - '0';

	if (zone[0] == '-') zone_off = -zone_off;

	c = prot_getc(imapd_in);
	if (c != '\"') goto baddate;

    }

    c = prot_getc(imapd_in);

    tm.tm_isdst = -1;

    tmp_gmtime = mkgmtime(&tm);
    if(tmp_gmtime == -1) goto baddate;

    *date = tmp_gmtime - zone_off*60;

    return c;

 baddate:
    prot_ungetc(c, imapd_in);
    return EOF;
}

/*
 * Print 's' as a quoted-string or literal (but not an atom)
 */
void
printstring(s)
const char *s;
{
    const char *p;
    int len = 0;

    /* Look for any non-QCHAR characters */
    for (p = s; *p && len < 1024; p++) {
	len++;
	if (*p & 0x80 || *p == '\r' || *p == '\n'
	    || *p == '\"' || *p == '%' || *p == '\\') break;
    }

    /* if it's too long, literal it */
    if (*p || len >= 1024) {
	prot_printf(imapd_out, "{%u}\r\n%s", strlen(s), s);
    } else {
	prot_printf(imapd_out, "\"%s\"", s);
    }
}

/*
 * Print 's' as an atom, quoted-string, or literal
 */
void
printastring(s)
const char *s;
{
    const char *p;
    int len = 0;

    if (imparse_isatom(s)) {
	prot_printf(imapd_out, "%s", s);
	return;
    }

    /* Look for any non-QCHAR characters */
    for (p = s; *p && len < 1024; p++) {
	len++;
	if (*p & 0x80 || *p == '\r' || *p == '\n'
	    || *p == '\"' || *p == '%' || *p == '\\') break;
    }

    /* if it's too long, literal it */
    if (*p || len >= 1024) {
	prot_printf(imapd_out, "{%u}\r\n%s", strlen(s), s);
    } else {
	prot_printf(imapd_out, "\"%s\"", s);
    }
}

/*
 * Append 'section', 'fields', 'trail' to the fieldlist 'l'.
 */
void
appendfieldlist(struct fieldlist **l, char *section,
		struct strlist *fields, char *trail,
		void *d, size_t size)
{
    struct fieldlist **tail = l;

    while (*tail) tail = &(*tail)->next;

    *tail = (struct fieldlist *)xmalloc(sizeof(struct fieldlist));
    (*tail)->section = xstrdup(section);
    (*tail)->fields = fields;
    (*tail)->trail = xstrdup(trail);
    if(d && size) {
	(*tail)->rock = xmalloc(size);
	memcpy((*tail)->rock, d, size);
    } else {
	(*tail)->rock = NULL;
    }
    (*tail)->next = 0;
}


/*
 * Free the fieldlist 'l'
 */
void freefieldlist(struct fieldlist *l)
{
    struct fieldlist *n;

    while (l) {
	n = l->next;
	free(l->section);
	freestrlist(l->fields);
	free(l->trail);
	if (l->rock) free(l->rock);
	free((char *)l);
	l = n;
    }
}

/*
 * Append the searchargs 's1' and 's2' to the sublist of 's'
 */
void
appendsearchargs(s, s1, s2)
struct searchargs *s, *s1, *s2;
{
    struct searchsub **tail = &s->sublist;

    while (*tail) tail = &(*tail)->next;

    *tail = (struct searchsub *)xmalloc(sizeof(struct searchsub));
    (*tail)->sub1 = s1;
    (*tail)->sub2 = s2;
    (*tail)->next = 0;
}


/*
 * Free the searchargs 's'
 */
void
freesearchargs(s)
struct searchargs *s;
{
    struct searchsub *sub, *n;

    if (!s) return;

    freestrlist(s->sequence);
    freestrlist(s->uidsequence);
    freestrlist(s->from);
    freestrlist(s->to);
    freestrlist(s->cc);
    freestrlist(s->bcc);
    freestrlist(s->subject);
    freestrlist(s->body);
    freestrlist(s->text);
    freestrlist(s->header_name);
    freestrlist(s->header);

    for (sub = s->sublist; sub; sub = n) {
	n = sub->next;
	freesearchargs(sub->sub1);
	freesearchargs(sub->sub2);
	free(sub);
    }
    free(s);
}

/*
 * Free an array of sortcrit
 */
static void freesortcrit(struct sortcrit *s)
{
    int i = 0;

    if (!s) return;
    do {
	switch (s[i].key) {
	case SORT_ANNOTATION:
	    free(s[i].args.annot.entry);
	    free(s[i].args.annot.attrib);
	    break;
	}
	i++;
    } while (s[i].key != SORT_SEQUENCE);
    free(s);
}

/*
 * Issue a MAILBOX untagged response
 */
static int mailboxdata(char *name,
		       int matchlen __attribute__((unused)),
		       int maycreate __attribute__((unused)),
		       void *rock __attribute__((unused)))
{
    char mboxname[MAX_MAILBOX_PATH+1];

    (*imapd_namespace.mboxname_toexternal)(&imapd_namespace, name,
					   imapd_userid, mboxname);
    prot_printf(imapd_out, "* MAILBOX %s\r\n", mboxname);
    return 0;
}

/*
 * Issue a LIST or LSUB untagged response
 */
static void mstringdata(char *cmd, char *name, int matchlen, int maycreate,
			int listopts)
{
    static char lastname[MAX_MAILBOX_PATH+1];
    static int lastnamedelayed = 0;
    static int lastnamenoinferiors = 0;
    static int nonexistent = 0;
    static int sawuser = 0;
    int lastnamehassub = 0;
    int c, mbtype;
    char mboxname[MAX_MAILBOX_PATH+1];

    /* We have to reset the sawuser flag before each list command.
     * Handle it as a dirty hack.
     */
    if (cmd == NULL) {
	sawuser = 0;
	mstringdatacalls = 0;
	return;
    }
    mstringdatacalls++;

    if (lastnamedelayed) {
	/* Check if lastname has children */
	if (name && strncmp(lastname, name, strlen(lastname)) == 0 &&
	    name[strlen(lastname)] == '.') {
	    lastnamehassub = 1;
	}
	prot_printf(imapd_out, "* %s (", cmd);
	if (nonexistent == IMAP_MAILBOX_RESERVED) {
	    /* LISTEXT wants \\PlaceHolder instead of \\Noselect */
	    if (listopts & LIST_EXT)
		prot_printf(imapd_out, "\\PlaceHolder");
	    else
		prot_printf(imapd_out, "\\Noselect");
	} else if (nonexistent) {
	    prot_printf(imapd_out, "\\NonExistent");
	}
	if (lastnamenoinferiors) {
	    prot_printf(imapd_out, "%s\\Noinferiors", nonexistent ? " " : "");
	}
	else if ((listopts & LIST_CHILDREN) &&
		 /* we can't determine \HasNoChildren for subscriptions */
		 (lastnamehassub ||
		  !(listopts & (LIST_LSUB | LIST_SUBSCRIBED)))) {
	    prot_printf(imapd_out, "%s%s", nonexistent ? " " : "",
			lastnamehassub ? "\\HasChildren" : "\\HasNoChildren");
	}
	prot_printf(imapd_out, ") \"%c\" ", imapd_namespace.hier_sep);
		    
	(*imapd_namespace.mboxname_toexternal)(&imapd_namespace, lastname,
					       imapd_userid, mboxname);
	printstring(mboxname);
	prot_printf(imapd_out, "\r\n");
	lastnamedelayed = lastnamenoinferiors = nonexistent = 0;
    }

    /* Special-case to flush any final state */
    if (!name) {
	lastname[0] = '\0';
	return;
    }

    /* Suppress any output of a partial match */
    if ((name[matchlen]
	 && strncmp(lastname, name, matchlen) == 0
	 && (lastname[matchlen] == '\0' || lastname[matchlen] == '.'))) {
	return;
    }
	
    /*
     * We can get a partial match for "user" multiple times with
     * other matches inbetween.  Handle it as a special case
     */
    if (matchlen == 4 && strncasecmp(name, "user", 4) == 0) {
	if (sawuser) return;
	sawuser = 1;
    }

    strlcpy(lastname, name, sizeof(lastname));
    lastname[matchlen] = '\0';
    nonexistent = 0;

    /* Now we need to see if this mailbox exists */
    /* first convert "INBOX" to "user.<userid>" */
    if (!strncasecmp(lastname, "inbox", 5)) {
	(*imapd_namespace.mboxname_tointernal)(&imapd_namespace, "INBOX",
					       imapd_userid, mboxname);
	strlcat(mboxname, lastname+5, sizeof(mboxname));
    }
    else
	strlcpy(mboxname, lastname, sizeof(mboxname));

    /* Look it up */
    nonexistent = mboxlist_detail(mboxname, &mbtype,
				  NULL, NULL, NULL, NULL);
    if(!nonexistent && (mbtype & MBTYPE_RESERVE))
	nonexistent = IMAP_MAILBOX_RESERVED;

    if (!name[matchlen]) {
	lastnamedelayed = 1;
	if (!maycreate) lastnamenoinferiors = 1;
	return;
    }

    c = name[matchlen];
    if (c) name[matchlen] = '\0';
    prot_printf(imapd_out, "* %s (", cmd);
    if (c) {
	/* Handle namespace prefix as a special case */ 
	if (!strcmp(name, "user") ||
	    !strcmp(name, imapd_namespace.prefix[NAMESPACE_SHARED])) {
	    prot_printf(imapd_out, "\\Noselect");
	    if (listopts & LIST_EXT)
		prot_printf(imapd_out, " \\PlaceHolder");
	}
	else {
	    if (nonexistent)
		prot_printf(imapd_out, "\\NonExistent");
	    /* LISTEXT uses \PlaceHolder instead of \Noselect */
	    if (listopts & LIST_EXT)
		prot_printf(imapd_out, "%s\\PlaceHolder", nonexistent ? " " : "");
	    else
		prot_printf(imapd_out, "%s\\Noselect", nonexistent ? " " : "");
	}
	if (listopts & LIST_CHILDREN)
	    prot_printf(imapd_out, " \\HasChildren");
    }
    prot_printf(imapd_out, ") \"%c\" ", imapd_namespace.hier_sep);

    (*imapd_namespace.mboxname_toexternal)(&imapd_namespace, name,
					   imapd_userid, mboxname);
    printstring(mboxname);
    prot_printf(imapd_out, "\r\n");
    if (c) name[matchlen] = c;
    return;
}

/*
 * Issue a LIST untagged response
 */
static int listdata(char *name, int matchlen, int maycreate, void *rock)
{
    int listopts = *((int *)rock);
    
    mstringdata(((listopts & LIST_LSUB) ? "LSUB" : "LIST"),
	name, matchlen, maycreate, listopts);

    return 0;
}

/* Reset the given sasl_conn_t to a sane state */
static int reset_saslconn(sasl_conn_t **conn) 
{
    int ret;
    sasl_security_properties_t *secprops = NULL;

    sasl_dispose(conn);
    /* do initialization typical of service_main */
    ret = sasl_server_new("imap", config_servername,
		          NULL, NULL, NULL,
			  NULL, 0, conn);
    if(ret != SASL_OK) return ret;

    if(saslprops.ipremoteport)
	ret = sasl_setprop(*conn, SASL_IPREMOTEPORT,
			   saslprops.ipremoteport);
    if(ret != SASL_OK) return ret;
    
    if(saslprops.iplocalport)
	ret = sasl_setprop(*conn, SASL_IPLOCALPORT,
			   saslprops.iplocalport);
    if(ret != SASL_OK) return ret;
    
    secprops = mysasl_secprops(SASL_SEC_NOPLAINTEXT);
    ret = sasl_setprop(*conn, SASL_SEC_PROPS, secprops);
    if(ret != SASL_OK) return ret;
    /* end of service_main initialization excepting SSF */

    /* If we have TLS/SSL info, set it */
    if(saslprops.ssf) {
	ret = sasl_setprop(*conn, SASL_SSF_EXTERNAL, &saslprops.ssf);
    } else {
	ret = sasl_setprop(*conn, SASL_SSF_EXTERNAL, &extprops_ssf);
    }
    if(ret != SASL_OK) return ret;

    if(saslprops.authid) {
	ret = sasl_setprop(*conn, SASL_AUTH_EXTERNAL, saslprops.authid);
	if(ret != SASL_OK) return ret;
    }
    /* End TLS/SSL Info */

    return SASL_OK;
}

void cmd_mupdatepush(char *tag, char *name)
{
    int r = 0;
    char mailboxname[MAX_MAILBOX_NAME+1];
    char *part, *acl;
    mupdate_handle *mupdate_h = NULL;
    char buf[MAX_PARTITION_LEN + HOSTNAME_SIZE + 2];

    if (!imapd_userisadmin) {
	r = IMAP_PERMISSION_DENIED;
    }
    if (!config_mupdate_server) {
	r = IMAP_SERVER_UNAVAILABLE;
    }

    if (!r) {
	r = (*imapd_namespace.mboxname_tointernal)(&imapd_namespace, name,
						   imapd_userid, mailboxname);
    }

    if (!r) {
	r = mlookup(tag, name, mailboxname, NULL, NULL, &part, &acl, NULL);
    }
    if (r == IMAP_MAILBOX_MOVED) return;

    /* Push mailbox to mupdate server */
    if (!r) {
	r = mupdate_connect(config_mupdate_server, NULL, &mupdate_h, NULL);
    }

    if (!r) {
	snprintf(buf, sizeof(buf), "%s!%s", config_servername, part);

	r = mupdate_activate(mupdate_h, mailboxname, buf, acl);
    }

    if(mupdate_h) {
	mupdate_disconnect(&mupdate_h);
    }

    if (r) {
	prot_printf(imapd_out, "%s NO %s\r\n", tag, error_message(r));
    }
    else {
	prot_printf(imapd_out, "%s OK %s\r\n", tag,
		    error_message(IMAP_OK_COMPLETED));
    }
}
