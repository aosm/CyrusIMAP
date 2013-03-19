/* version.h: the version number
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
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * $Id: version.h,v 1.139 2005/02/14 16:43:51 shadow Exp $
 */

#define _CYRUS_VERSION "v2.2.12"

/* EXTRA_IDENT is a hack to add some version information for which compile
 * was used to build this version (at CMU, but we don't care what you do with
 * it).
 */

#ifdef EXTRA_IDENT
#define CYRUS_VERSION _CYRUS_VERSION "-" EXTRA_IDENT
#else
#define CYRUS_VERSION _CYRUS_VERSION
#endif

/* CAPABILITIES are now defined here, not including sasl ones */
#define CAPABILITY_STRING "IMAP4 IMAP4rev1 ACL QUOTA LITERAL+ " \
	"MAILBOX-REFERRALS NAMESPACE UIDPLUS ID " \
	"NO_ATOMIC_RENAME UNSELECT " \
	"CHILDREN MULTIAPPEND BINARY " \
	"SORT THREAD=ORDEREDSUBJECT THREAD=REFERENCES " \
	"ANNOTATEMORE"


/* Values for ID processing */
enum {
    MAXIDFAILED	= 3,
    MAXIDLOG = 5,
    MAXIDFIELDLEN = 30,
    MAXIDVALUELEN = 1024,
    MAXIDPAIRS = 30,
    MAXIDLOGLEN = (MAXIDPAIRS * (MAXIDFIELDLEN + MAXIDVALUELEN + 6))
};
