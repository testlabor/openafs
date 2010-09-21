/*
 * Copyright 2000, International Business Machines Corporation and others.
 * All Rights Reserved.
 *
 * This software has been released under the terms of the IBM Public
 * License.  For details, see the LICENSE file in the top-level source
 * directory or online at http://www.openafs.org/dl/license10.html
 */

/*
	System:		VICE-TWO
	Module:		errors.h

 */

/*
 * Vice2 error codes
 * 3/20/85
 * Note:  all of the errors listed here are currently generated by the volume
 * package.  Other vice error codes, should they be needed, could be included
 * here also.
 */
#include <afs/param.h>

#define VREADONLY	EROFS	/* Attempt to write a read-only volume */

/* Special error codes, which may require special handling (other than just
   passing them through directly to the end user) are listed below */

#define VICE_SPECIAL_ERRORS	101	/* Lowest special error code */

#define VSALVAGE	101	/* Volume needs salvage */
#define VNOVNODE	102	/* Bad vnode number quoted */
#define VNOVOL		103	/* Volume not attached, doesn't exist,
				 * not created or not online */
#define VVOLEXISTS	104	/* Volume already exists */
#define VNOSERVICE	105	/* Volume is not in service (i.e. it's
				 * is out of funds, is obsolete, or somesuch) */
#define VOFFLINE	106	/* Volume is off line, for the reason
				 * given in the offline message */
#define VONLINE		107	/* Volume is already on line */
#define VDISKFULL	108	/* ENOSPC - Partition is "full", i.e. rougly within
				 * n% of full */
#define VOVERQUOTA	109	/* EDQUOT- Volume max quota exceeded */
#define VBUSY		110	/* Volume temporarily unavailable; try again.
				 * The volume should be available again shortly; if
				 * it isn't something is wrong.  Not normally to be
				 * propagated to the application level */
#define VMOVED		111	/* Volume has moved to another server; do a VGetVolumeInfo
				 * to THIS server to find out where */
#define VIO		112	/* Vnode temporarily unaccessible, but not known
				 * to be permanently bad. */
#define VSALVAGING      113     /* Volume is being salvaged (demand attach fs) */
#define VRESTRICTED     120	/* Volume is restricted from using one or more
				 * of the given residencies; do a
				 * vos examine to find out the current
				 * restrictions. */
#define VRESTARTING	-100	/* server is restarting, otherwise similar to
				 * VBUSY above.  This is negative so that old
				 * cache managers treat it as "server is down" */
