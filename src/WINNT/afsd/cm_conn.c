/*
 * Copyright 2000, International Business Machines Corporation and others.
 * All Rights Reserved.
 * 
 * This software has been released under the terms of the IBM Public
 * License.  For details, see the LICENSE file in the top-level source
 * directory or online at http://www.openafs.org/dl/license10.html
 */

#include <afs/param.h>
#include <afs/stds.h>

#ifndef DJGPP
#include <windows.h>
#endif /* !DJGPP */
#include <string.h>
#include <malloc.h>
#include <osi.h>
#include <rx/rx.h>
#ifndef DJGPP
#include <rx/rxkad.h>
#else
#include <rx/rxkad.h>
#endif

#include "afsd.h"

osi_rwlock_t cm_connLock;

long RDRtimeout = CM_CONN_DEFAULTRDRTIMEOUT;
long ConnDeadtimeout = CM_CONN_CONNDEADTIME;
long HardDeadtimeout = CM_CONN_HARDDEADTIME;

#define LANMAN_WKS_PARAM_KEY "SYSTEM\\CurrentControlSet\\Services\\lanmanworkstation\\parameters"
#define LANMAN_WKS_SESSION_TIMEOUT "SessTimeout"

afs_int32 cryptall = 0;

void cm_PutConn(cm_conn_t *connp)
{
	lock_ObtainWrite(&cm_connLock);
	osi_assert(connp->refCount-- > 0);
	lock_ReleaseWrite(&cm_connLock);
}

void cm_InitConn(void)
{
	static osi_once_t once;
	long code;
	DWORD sessTimeout;
	HKEY parmKey;
        
    if (osi_Once(&once)) {
		lock_InitializeRWLock(&cm_connLock, "connection global lock");

        /* keisa - read timeout value for lanmanworkstation  service.
         * jaltman - as per 
         *   http://support.microsoft.com:80/support/kb/articles/Q102/0/67.asp&NoWebContent=1
         * the SessTimeout is a minimum timeout not a maximum timeout.  Therefore, 
         * I believe that the default should not be short.  Instead, we should wait until
         * RX times out before reporting a timeout to the SMB client.
         */
		code = RegOpenKeyEx(HKEY_LOCAL_MACHINE, LANMAN_WKS_PARAM_KEY,
                            0, KEY_QUERY_VALUE, &parmKey);
		if (code == ERROR_SUCCESS)
        {
		    DWORD dummyLen = sizeof(sessTimeout);
		    code = RegQueryValueEx(parmKey, LANMAN_WKS_SESSION_TIMEOUT, NULL, NULL, 
                                   (BYTE *) &sessTimeout, &dummyLen);
		    if (code == ERROR_SUCCESS)
            {
                afsi_log("lanmanworkstation : SessTimeout %d", sessTimeout);
                RDRtimeout = sessTimeout;
                if ( ConnDeadtimeout < RDRtimeout + 15 ) {
                    ConnDeadtimeout = RDRtimeout + 15;
                    afsi_log("ConnDeadTimeout increased to %d", ConnDeadtimeout);
                }
                if ( HardDeadtimeout < 2 * ConnDeadtimeout ) {
                    HardDeadtimeout = 2 * ConnDeadtimeout;
                    afsi_log("HardDeadTimeout increased to %d", HardDeadtimeout);
                }
            }
        }

        osi_EndOnce(&once);
    }
}

void cm_InitReq(cm_req_t *reqp)
{
	memset((char *)reqp, 0, sizeof(cm_req_t));
#ifndef DJGPP
	reqp->startTime = GetCurrentTime();
#else
        gettimeofday(&reqp->startTime, NULL);
#endif
}

static long cm_GetServerList(struct cm_fid *fidp, struct cm_user *userp,
	struct cm_req *reqp, cm_serverRef_t ***serversppp)
{
	long code;
    cm_volume_t *volp = NULL;
    cm_cell_t *cellp = NULL;

    if (!fidp) {
		*serversppp = NULL;
		return 0;
	}

	cellp = cm_FindCellByID(fidp->cell);
    if (!cellp) return CM_ERROR_NOSUCHCELL;

    code = cm_GetVolumeByID(cellp, fidp->volume, userp, reqp, &volp);
    if (code) return code;
    
    *serversppp = cm_GetVolServers(volp, fidp->volume);

    cm_PutVolume(volp);
	return 0;
}

/*
 * Analyze the error return from an RPC.  Determine whether or not to retry,
 * and if we're going to retry, determine whether failover is appropriate,
 * and whether timed backoff is appropriate.
 *
 * If the error code is from cm_Conn() or friends, it will be a CM_ERROR code.
 * Otherwise it will be an RPC code.  This may be a UNIX code (e.g. EDQUOT), or
 * it may be an RX code, or it may be a special code (e.g. VNOVOL), or it may
 * be a security code (e.g. RXKADEXPIRED).
 *
 * If the error code is from cm_Conn() or friends, connp will be NULL.
 *
 * For VLDB calls, fidp will be NULL.
 *
 * volSyncp and/or cbrp may also be NULL.
 */
int
cm_Analyze(cm_conn_t *connp, cm_user_t *userp, cm_req_t *reqp,
	struct cm_fid *fidp, 
	AFSVolSync *volSyncp, 
	cm_serverRef_t * serversp,
	cm_callbackRequest_t *cbrp, long errorCode)
{
    cm_server_t *serverp = 0;
    cm_serverRef_t **serverspp = 0;
	cm_serverRef_t *tsrp;
	cm_ucell_t *ucellp;
    int retry = 0;
    int free_svr_list = 0;
	int dead_session;
    long timeUsed, timeLeft;
        
	osi_Log2(afsd_logp, "cm_Analyze connp 0x%x, code %d",
		 (long) connp, errorCode);

	/* no locking required, since connp->serverp never changes after
	 * creation */
	dead_session = (userp->cellInfop == NULL);
	if (connp)
		serverp = connp->serverp;

	/* Update callback pointer */
    if (cbrp && serverp && errorCode == 0) {
        if (cbrp->serverp) {
            if ( cbrp->serverp != serverp ) {
                lock_ObtainWrite(&cm_serverLock);
                cm_PutServerNoLock(cbrp->serverp);
                cm_GetServerNoLock(serverp);
                lock_ReleaseWrite(&cm_serverLock);
            }
        } else {
            cm_GetServer(serverp);
        }
        lock_ObtainWrite(&cm_callbackLock);
        cbrp->serverp = serverp;
        lock_ReleaseWrite(&cm_callbackLock);
    }

	/* If not allowed to retry, don't */
	if (reqp->flags & CM_REQ_NORETRY)
		goto out;

	/* if timeout - check that it did not exceed the SMB timeout
     * and retry */
    
	    /* timeleft - get if from reqp the same way as cmXonnByMServers does */
#ifndef DJGPP
	    timeUsed = (GetCurrentTime() - reqp->startTime) / 1000;
#else
	    gettimeofday(&now, NULL);
	    timeUsed = sub_time(now, reqp->startTime) / 1000;
#endif
	    
	    /* leave 5 seconds margin for sleep */
	    timeLeft = RDRtimeout - timeUsed;

    if (errorCode == CM_ERROR_TIMEDOUT && timeLeft > 5 ) {
            thrd_Sleep(3000);
            cm_CheckServers(CM_FLAG_CHECKDOWNSERVERS, NULL);
            retry = 1;
        } 

    /* if all servers are offline, mark them non-busy and start over */
    if (errorCode == CM_ERROR_ALLOFFLINE && timeLeft > 7) {
	    osi_Log0(afsd_logp, "cm_Analyze passed CM_ERROR_ALLOFFLINE.");
	    thrd_Sleep(5000);
	    /* cm_ForceUpdateVolume marks all servers as non_busy */
		/* No it doesn't and it won't do anything if all of the 
		 * the servers are marked as DOWN.  So clear the DOWN
		 * flag and reset the busy state as well.
		 */
        if (!serversp) {
            cm_GetServerList(fidp, userp, reqp, &serverspp);
            serversp = *serverspp;
            free_svr_list = 1;
        }
        if (serversp) {
            lock_ObtainWrite(&cm_serverLock);
            for (tsrp = serversp; tsrp; tsrp=tsrp->next) {
                tsrp->server->flags &= ~CM_SERVERFLAG_DOWN;
                if (tsrp->status == busy)
                    tsrp->status = not_busy;
            }
            lock_ReleaseWrite(&cm_serverLock);
            if (free_svr_list) {
                cm_FreeServerList(&serversp);
                *serverspp = serversp;
            }
            retry = 1;
        }

        if (fidp != NULL)   /* Not a VLDB call */
            cm_ForceUpdateVolume(fidp, userp, reqp);
	}

	/* if all servers are busy, mark them non-busy and start over */
    if (errorCode == CM_ERROR_ALLBUSY && timeLeft > 7) {
        thrd_Sleep(5000);
        if (!serversp) {
            cm_GetServerList(fidp, userp, reqp, &serverspp);
            serversp = *serverspp;
            free_svr_list = 1;
        }
		lock_ObtainWrite(&cm_serverLock);
		for (tsrp = serversp; tsrp; tsrp=tsrp->next) {
			if (tsrp->status == busy)
				tsrp->status = not_busy;
		}
        lock_ReleaseWrite(&cm_serverLock);
        if (free_svr_list) {
            cm_FreeServerList(&serversp);
            *serverspp = serversp;
        }
		retry = 1;
	}

	/* special codes:  VBUSY and VRESTARTING */
	if (errorCode == VBUSY || errorCode == VRESTARTING) {
        if (!serversp) {
            cm_GetServerList(fidp, userp, reqp, &serverspp);
            serversp = *serverspp;
            free_svr_list = 1;
        }
		lock_ObtainWrite(&cm_serverLock);
		for (tsrp = serversp; tsrp; tsrp=tsrp->next) {
			if (tsrp->server == serverp
			    && tsrp->status == not_busy) {
				tsrp->status = busy;
				break;
			}
		}
        lock_ReleaseWrite(&cm_serverLock);
        if (free_svr_list) {
            cm_FreeServerList(&serversp);
            *serverspp = serversp;
        }
		retry = 1;
	}

	/* special codes:  missing volumes */
	if (errorCode == VNOVOL || errorCode == VMOVED || errorCode == VOFFLINE
	    || errorCode == VSALVAGE || errorCode == VNOSERVICE) {
		/* Log server being offline for this volume */
		osi_Log4(afsd_logp, "cm_Analyze found server %d.%d.%d.%d marked offline for a volume",
			 ((serverp->addr.sin_addr.s_addr & 0xff)),
			 ((serverp->addr.sin_addr.s_addr & 0xff00)>> 8),
			 ((serverp->addr.sin_addr.s_addr & 0xff0000)>> 16),
			 ((serverp->addr.sin_addr.s_addr & 0xff000000)>> 24));
		/* Create Event Log message */ 
		{
		    HANDLE h;
		    char *ptbuf[1];
		    char s[100];
		    h = RegisterEventSource(NULL, AFS_DAEMON_EVENT_NAME);
		    sprintf(s, "cm_Analyze: Server %d.%d.%d.%d reported volume %d as missing.",
			    ((serverp->addr.sin_addr.s_addr & 0xff)),
			    ((serverp->addr.sin_addr.s_addr & 0xff00)>> 8),
			    ((serverp->addr.sin_addr.s_addr & 0xff0000)>> 16),
			    ((serverp->addr.sin_addr.s_addr & 0xff000000)>> 24),
			    fidp->volume);
		    ptbuf[0] = s;
		    ReportEvent(h, EVENTLOG_WARNING_TYPE, 0, 1009, NULL,
				1, 0, ptbuf, NULL);
		    DeregisterEventSource(h);
		}

		/* Mark server offline for this volume */
        if (!serversp) {
            cm_GetServerList(fidp, userp, reqp, &serverspp);
            serversp = *serverspp;
            free_svr_list = 1;
        }
		for (tsrp = serversp; tsrp; tsrp=tsrp->next) {
			if (tsrp->server == serverp)
				tsrp->status = offline;
		}
        if (free_svr_list) {
            cm_FreeServerList(&serversp);
            *serverspp = serversp;
        }
        if ( timeLeft > 2 )
		retry = 1;
	}

	/* RX codes */
	if (errorCode == RX_CALL_TIMEOUT) {
		/* server took longer than hardDeadTime 
		 * don't mark server as down but don't retry
		 * this is to prevent the SMB session from timing out
		 * In addition, we log an event to the event log 
		 */
#ifndef DJGPP
		HANDLE h;
                char *ptbuf[1];
                char s[100];
                h = RegisterEventSource(NULL, AFS_DAEMON_EVENT_NAME);
                sprintf(s, "cm_Analyze: HardDeadTime exceeded.");
                ptbuf[0] = s;
                ReportEvent(h, EVENTLOG_WARNING_TYPE, 0, 1009, NULL,
                        1, 0, ptbuf, NULL);
                DeregisterEventSource(h);
#endif /* !DJGPP */
	  
		retry = 0;
	  	osi_Log0(afsd_logp, "cm_Analyze: hardDeadTime exceeded");
	}
	else if (errorCode >= -64 && errorCode < 0) {
		/* mark server as down */
		lock_ObtainMutex(&serverp->mx);
        serverp->flags |= CM_SERVERFLAG_DOWN;
		lock_ReleaseMutex(&serverp->mx);
            if ( timeLeft > 2 )
        retry = 1;
    }

	if (errorCode == RXKADEXPIRED && !dead_session) {
		lock_ObtainMutex(&userp->mx);
		ucellp = cm_GetUCell(userp, serverp->cellp);
		if (ucellp->ticketp) {
			free(ucellp->ticketp);
			ucellp->ticketp = NULL;
		}
		ucellp->flags &= ~CM_UCELLFLAG_RXKAD;
		ucellp->gen++;
		lock_ReleaseMutex(&userp->mx);
            if ( timeLeft > 2 )
		retry = 1;
	}

	if (retry && dead_session)
		retry = 0;
 
out:
	/* drop this on the way out */
	if (connp)
		cm_PutConn(connp);

	/* retry until we fail to find a connection */
        return retry;
}

long cm_ConnByMServers(cm_serverRef_t *serversp, cm_user_t *usersp,
	cm_req_t *reqp, cm_conn_t **connpp)
{
    long code;
    cm_serverRef_t *tsrp;
    cm_server_t *tsp;
    long firstError = 0;
    int someBusy = 0, someOffline = 0, allBusy = 1, allDown = 1;
    long timeUsed, timeLeft, hardTimeLeft;
#ifdef DJGPP
    struct timeval now;
#endif /* DJGPP */        

    *connpp = NULL;

#ifndef DJGPP
    timeUsed = (GetCurrentTime() - reqp->startTime) / 1000;
#else
    gettimeofday(&now, NULL);
    timeUsed = sub_time(now, reqp->startTime) / 1000;
#endif
        
    /* leave 5 seconds margin of safety */
    timeLeft =  ConnDeadtimeout - timeUsed - 5;
    hardTimeLeft = HardDeadtimeout - timeUsed - 5;

    lock_ObtainWrite(&cm_serverLock);
    for (tsrp = serversp; tsrp; tsrp=tsrp->next) {
        tsp = tsrp->server;
        cm_GetServerNoLock(tsp);
        lock_ReleaseWrite(&cm_serverLock);
        if (!(tsp->flags & CM_SERVERFLAG_DOWN)) {
            allDown = 0;
            if (tsrp->status == busy)
                someBusy = 1;
            else if (tsrp->status == offline)
                someOffline = 1;
            else {
                allBusy = 0;
                code = cm_ConnByServer(tsp, usersp, connpp);
                if (code == 0) {
                    cm_PutServer(tsp);
                    /* Set RPC timeout */
                    if (timeLeft > ConnDeadtimeout)
                        timeLeft = ConnDeadtimeout;

                    if (hardTimeLeft > HardDeadtimeout) 
                        hardTimeLeft = HardDeadtimeout;

                    lock_ObtainMutex(&(*connpp)->mx);
                    rx_SetConnDeadTime((*connpp)->callp, timeLeft);
                    rx_SetConnHardDeadTime((*connpp)->callp, (u_short) hardTimeLeft);
                    lock_ReleaseMutex(&(*connpp)->mx);
                    return 0;
                }
                if (firstError == 0) 
                    firstError = code;
            }
        } 
        lock_ObtainWrite(&cm_serverLock);
        cm_PutServerNoLock(tsp);
    }   

    lock_ReleaseWrite(&cm_serverLock);
    if (firstError == 0) {
        if (serversp == NULL)
            firstError = CM_ERROR_NOSUCHVOLUME;
        else if (allDown) 
            firstError = CM_ERROR_ALLOFFLINE;
        else if (allBusy) 
            firstError = CM_ERROR_ALLBUSY;
        else
            firstError = CM_ERROR_TIMEDOUT;
    }

    osi_Log1(afsd_logp, "cm_ConnByMServers returning %x", firstError);
    return firstError;
}

/* called with a held server to GC all bad connections hanging off of the server */
void cm_GCConnections(cm_server_t *serverp)
{
    cm_conn_t *tcp;
    cm_conn_t **lcpp;
    cm_user_t *userp;

    lock_ObtainWrite(&cm_connLock);
    lcpp = &serverp->connsp;
    for (tcp = *lcpp; tcp; tcp = *lcpp) {
        userp = tcp->userp;
        if (userp && tcp->refCount == 0 && (userp->vcRefs == 0)) {
            /* do the deletion of this guy */
            cm_PutServer(tcp->serverp);
            cm_ReleaseUser(userp);
            *lcpp = tcp->nextp;
            rx_DestroyConnection(tcp->callp);
            lock_FinalizeMutex(&tcp->mx);
            free(tcp);
        }
        else {
            /* just advance to the next */
            lcpp = &tcp->nextp;
        }
    }
    lock_ReleaseWrite(&cm_connLock);
}

static void cm_NewRXConnection(cm_conn_t *tcp, cm_ucell_t *ucellp,
	cm_server_t *serverp)
{
    unsigned short port;
    int serviceID;
    int secIndex;
    struct rx_securityClass *secObjp;
    afs_int32 level;

    if (serverp->type == CM_SERVER_VLDB) {
        port = htons(7003);
        serviceID = 52;
    }
    else {
        osi_assert(serverp->type == CM_SERVER_FILE);
        port = htons(7000);
        serviceID = 1;
    }
    if (ucellp->flags & CM_UCELLFLAG_RXKAD) {
        secIndex = 2;
        if (cryptall) {
            level = tcp->cryptlevel = rxkad_crypt;
        } else {
            level = tcp->cryptlevel = rxkad_clear;
        }
        secObjp = rxkad_NewClientSecurityObject(level,
                                                &ucellp->sessionKey, ucellp->kvno,
                                                ucellp->ticketLen, ucellp->ticketp);    
    }
    else {
        /* normal auth */
        secIndex = 0;
        secObjp = rxnull_NewClientSecurityObject();
    }
    osi_assert(secObjp != NULL);
    tcp->callp = rx_NewConnection(serverp->addr.sin_addr.s_addr,
                                  port,
                                  serviceID,
                                  secObjp,
                                  secIndex);
    rx_SetConnDeadTime(tcp->callp, ConnDeadtimeout);
    rx_SetConnHardDeadTime(tcp->callp, HardDeadtimeout);
    tcp->ucgen = ucellp->gen;
    if (secObjp)
        rxs_Release(secObjp);   /* Decrement the initial refCount */
}

long cm_ConnByServer(cm_server_t *serverp, cm_user_t *userp, cm_conn_t **connpp)
{
    cm_conn_t *tcp;
    cm_ucell_t *ucellp;

    lock_ObtainMutex(&userp->mx);
    lock_ObtainWrite(&cm_connLock);
    for (tcp = serverp->connsp; tcp; tcp=tcp->nextp) {
        if (tcp->userp == userp) 
            break;
    }
    
    /* find ucell structure */
    ucellp = cm_GetUCell(userp, serverp->cellp);
    if (!tcp) {
        cm_GetServer(serverp);
        tcp = malloc(sizeof(*tcp));
        memset(tcp, 0, sizeof(*tcp));
        tcp->nextp = serverp->connsp;
        serverp->connsp = tcp;
        cm_HoldUser(userp);
        tcp->userp = userp;
        lock_InitializeMutex(&tcp->mx, "cm_conn_t mutex");
        lock_ObtainMutex(&tcp->mx);
        tcp->serverp = serverp;
        tcp->cryptlevel = rxkad_clear;
        cm_NewRXConnection(tcp, ucellp, serverp);
        tcp->refCount = 1;
        lock_ReleaseMutex(&tcp->mx);
    } else {
        if ((tcp->ucgen < ucellp->gen) ||
            (tcp->cryptlevel != (cryptall ? rxkad_crypt : rxkad_clear)))
        {
            lock_ObtainMutex(&tcp->mx);
            rx_DestroyConnection(tcp->callp);
            cm_NewRXConnection(tcp, ucellp, serverp);
            lock_ReleaseMutex(&tcp->mx);
        }
        tcp->refCount++;
    }
    lock_ReleaseWrite(&cm_connLock);
    lock_ReleaseMutex(&userp->mx);

    /* return this pointer to our caller */
    osi_Log1(afsd_logp, "cm_ConnByServer returning conn 0x%x", (long) tcp);
    *connpp = tcp;

    return 0;
}

long cm_Conn(struct cm_fid *fidp, struct cm_user *userp, cm_req_t *reqp,
	cm_conn_t **connpp)
{
	long code;

	cm_serverRef_t **serverspp;

	code = cm_GetServerList(fidp, userp, reqp, &serverspp);
	if (code) {
		*connpp = NULL;
		return code;
	}

	code = cm_ConnByMServers(*serverspp, userp, reqp, connpp);
    cm_FreeServerList(serverspp);
    return code;
}

extern struct rx_connection * 
cm_GetRxConn(cm_conn_t *connp)
{
    struct rx_connection * rxconn;
    lock_ObtainMutex(&connp->mx);
    rxconn = connp->callp;
    rx_GetConnection(rxconn);
    lock_ReleaseMutex(&connp->mx);
    return rxconn;
}

