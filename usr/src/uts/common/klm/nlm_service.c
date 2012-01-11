/*
 * Copyright 2011 Nexenta Systems, Inc.  All rights reserved.
 * Copyright (c) 2008 Isilon Inc http://www.isilon.com/
 * Authors: Doug Rabson <dfr@rabson.org>
 * Developed with Red Inc: Alfred Perlstein <alfred@freebsd.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * NFS Lock Manager service functions (nlm_do_...)
 * Called from nlm_rpc_svc.c wrappers.
 *
 * Source code derived from FreeBSD nlm_prot_impl.c
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/fcntl.h>
#include <sys/flock.h>
#include <sys/mount.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/share.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/syslog.h>
#include <sys/systm.h>
#include <sys/taskq.h>
#include <sys/unistd.h>
#include <sys/vnode.h>
#include <sys/vfs.h>
#include <sys/queue.h>
#include <sys/sdt.h>
#include <netinet/in.h>

#include <rpc/rpc.h>
#include <rpc/xdr.h>
#include <rpc/pmap_prot.h>
#include <rpc/pmap_clnt.h>
#include <rpc/rpcb_prot.h>

#include <rpcsvc/nlm_prot.h>
#include <rpcsvc/sm_inter.h>

#include <nfs/nfs.h>
#include <nfs/nfs_clnt.h>
#include <nfs/export.h>
#include <nfs/rnode.h>

#include "nlm_impl.h"

#define	NLM_IN_GRACE(g) (ddi_get_lbolt() < (g)->grace_threshold)

static void nlm_block(
	nlm4_lockargs *lockargs,
	struct nlm_host *host,
	struct nlm_vhold *nvp,
	struct flock64 *fl,
	nlm_testargs_cb grant_cb,
	rpcvers_t vers);

static void nlm_init_flock(struct flock64 *, struct nlm4_lock *, int);
static vnode_t *nlm_do_fh_to_vp(struct netobj *);
static vnode_t *nlm_fh_to_vp(struct netobj *);
static struct nlm_vhold *nlm_fh_to_vhold(struct nlm_host *, struct netobj *);
static void nlm_init_shrlock(struct shrlock *, nlm4_share *, struct nlm_host *);

static void
nlm_init_flock(struct flock64 *fl, struct nlm4_lock *nl, int sysid)
{
	bzero(fl, sizeof (*fl));
	/* fl->l_type set by caller */
	fl->l_whence = SEEK_SET;
	fl->l_start = nl->l_offset;
	fl->l_len = nl->l_len;
	fl->l_sysid = sysid;
	fl->l_pid = nl->svid;
}

/*
 * Gets vnode from client's filehandle
 * NOTE: Holds vnode, it _must_ be explicitly
 * released by VN_RELE().
 */
static vnode_t *
nlm_do_fh_to_vp(struct netobj *fh)
{
	fhandle_t *fhp;

	/*
	 * Get a vnode pointer for the given NFS file handle.
	 * Note that it could be an NFSv2 for NFSv3 handle,
	 * which means the size might vary.  (don't copy)
	 */
	if (fh->n_len < sizeof (*fhp))
		return (NULL);

	/* We know this is aligned (kmem_alloc) */
	fhp = (fhandle_t *)fh->n_bytes;
	return (lm_fhtovp(fhp));
}

/*
 * Like nlm_do_fh_to_vp(), but checks some access rights
 * on vnode before returning it.
 * NOTE: vnode _must_ be explicitly released by VN_RELE().
 */
static vnode_t *
nlm_fh_to_vp(struct netobj *fh)
{
	vnode_t *vp;

	vp = nlm_do_fh_to_vp(fh);
	if (vp == NULL)
		return (vp);

	/*
	 * Do not allow to add locks/shares to read only
	 * file system.
	 */
	if (vp->v_vfsp->vfs_flag & VFS_RDONLY)
		goto error;

	/*
	 * TODO[DK]: check whether given thread can add locks
	 * to given vnode.
	 */

	return (vp);

error:
	VN_RELE(vp);
	return (NULL);
}

/*
 * Get vhold from client's filehandle, but in contrast to
 * The function tries to check some access rights as well.
 *
 * NOTE: vhold object _must_ be explicitly released by
 * nlm_vhold_release().
 */
static struct nlm_vhold *
nlm_fh_to_vhold(struct nlm_host *hostp, struct netobj *fh)
{
	vnode_t *vp;
	struct nlm_vhold *nvp;

	vp = nlm_fh_to_vp(fh);
	if (vp == NULL)
		return (NULL);


	nvp = nlm_vhold_get(hostp, vp);

	/*
	 * Both nlm_fh_to_vp() and nlm_vhold_get()
	 * do VN_HOLD(), so we need to drop one
	 * reference on vnode.
	 */
	VN_RELE(vp);
	return (nvp);
}

/* ******************************************************************* */

/*
 * NLM implementation details, called from the RPC svc code.
 */

/*
 * Call-back from NFS statd, used to notify that one of our
 * hosts had a status change. The host can be either an
 * NFS client, NFS server or both.
 * According to NSM protocol description, the state is a
 * number that is increases monotonically each time the
 * state of host changes. An even number indicates that
 * the host is doen, while an odd number indicates that
 * the host is up.
 *
 * Here we ignore this even/odd difference of status number
 * reported by the NSM, we launch notification handlers
 * every time the state is changed. The reason we why do so
 * is that client and server can talk to each other using
 * connectionless transport and it's easy to lose packet
 * containing NSM notification with status number update.
 *
 * In nlm_host_monitor(), we put the sysid in the private data
 * that statd carries in this callback, so we can easliy find
 * the host this call applies to.
 */
/* ARGSUSED */
void
nlm_do_notify1(nlm_sm_status *argp, void *res, struct svc_req *sr)
{
	struct nlm_globals *g;
	struct nlm_host *host;
	uint16_t sysid;

	g = zone_getspecific(nlm_zone_key, curzone);
	bcopy(&argp->priv, &sysid, sizeof (sysid));

	DTRACE_PROBE2(nsm__notify, uint16_t, sysid,
	    int, argp->state);

	host = nlm_host_find_by_sysid(g, (sysid_t)sysid);
	if (host == NULL)
		return;

	nlm_host_notify_server(host, argp->state);
	nlm_host_notify_client(host, argp->state);
	nlm_host_release(g, host);
}

/*
 * Another available call-back for NFS statd.
 * Not currently used.
 */
/* ARGSUSED */
void
nlm_do_notify2(nlm_sm_status *argp, void *res, struct svc_req *sr)
{
}


/*
 * NLM_TEST, NLM_TEST_MSG,
 * NLM4_TEST, NLM4_TEST_MSG,
 * Client inquiry about locks, non-blocking.
 */
void
nlm_do_test(nlm4_testargs *argp, nlm4_testres *resp,
    struct svc_req *sr, nlm_testres_cb cb)
{
	struct nlm_globals *g;
	struct nlm_host *host;
	vnode_t *vp = NULL;
	struct netbuf *addr;
	char *netid;
	char *name;
	int error;
	struct flock64 fl;

	nlm_copy_netobj(&resp->cookie, &argp->cookie);

	name = argp->alock.caller_name;
	netid = svc_getnetid(sr->rq_xprt);
	addr = svc_getrpccaller(sr->rq_xprt);

	g = zone_getspecific(nlm_zone_key, curzone);
	host = nlm_host_findcreate(g, name, netid, addr);
	if (host == NULL) {
		resp->stat.stat = nlm4_denied_nolocks;
		return;
	}

	/*
	 * Do not check access rights to vnode when
	 * deal with NLM_TEST. It's simply a read
	 * access to filesystem.
	 */
	vp = nlm_do_fh_to_vp(&argp->alock.fh);
	if (vp == NULL) {
		resp->stat.stat = nlm4_stale_fh;
		goto out;
	}

	if (NLM_IN_GRACE(g)) {
		resp->stat.stat = nlm4_denied_grace_period;
		goto out;
	}

	nlm_init_flock(&fl, &argp->alock, nlm_host_get_sysid(host));
	fl.l_type = (argp->exclusive) ? F_WRLCK : F_RDLCK;

	/* BSD: VOP_ADVLOCK(nv->nv_vp, NULL, F_GETLK, &fl, F_REMOTE); */
	error = VOP_FRLOCK(vp, F_GETLK, &fl,
	    F_REMOTELOCK | FREAD | FWRITE,
	    (u_offset_t)0, NULL, CRED(), NULL);
	if (error) {
		resp->stat.stat = nlm4_failed;
		goto out;
	}

	if (fl.l_type == F_UNLCK) {
		resp->stat.stat = nlm4_granted;
	} else {
		struct nlm4_holder *lh;
		resp->stat.stat = nlm4_denied;
		lh = &resp->stat.nlm4_testrply_u.holder;
		lh->exclusive = (fl.l_type == F_WRLCK);
		lh->svid = fl.l_pid;
		/* Leave OH zero. XXX: sysid? */
		lh->l_offset = fl.l_start;
		lh->l_len = fl.l_len;
	}

out:
	/*
	 * If we have a callback funtion, use that to
	 * deliver the response via another RPC call.
	 */
	if (cb != NULL) {
		nlm_rpc_t *rpcp;
		int stat;

		error = nlm_host_get_rpc(host, sr->rq_vers, &rpcp);
		if (error == 0) {
			/* i.e. nlm_test_res_4_cb */
			stat = (*cb)(resp, NULL, rpcp->nr_handle);
			if (stat != RPC_SUCCESS) {
				struct rpc_err err;

				CLNT_GETERR(rpcp->nr_handle, &err);
				NLM_ERR("NLM: do_test CB, stat=%d err=%d\n",
				    stat, err.re_errno);
			}

			nlm_host_rele_rpc(host, rpcp);
		}
	}

	if (vp != NULL)
		VN_RELE(vp);

	nlm_host_release(g, host);
}

/*
 * NLM_LOCK, NLM_LOCK_MSG, NLM_NM_LOCK
 * NLM4_LOCK, NLM4_LOCK_MSG, NLM4_NM_LOCK
 *
 * Client request to set a lock, possibly blocking.
 *
 * If the lock needs to block, we return status blocked to
 * this RPC call, and then later call back the client with
 * a "granted" callback.  Tricky aspects of this include:
 * sending a reply before this function returns, and then
 * borrowing this thread from the RPC service pool for the
 * wait on the lock and doing the later granted callback.
 *
 * We also have to keep a list of locks (pending + granted)
 * both to handle retransmitted requests, and to keep the
 * vnodes for those locks active.
 */
void
nlm_do_lock(nlm4_lockargs *argp, nlm4_res *resp, struct svc_req *sr,
    nlm_reply_cb reply_cb, nlm_res_cb res_cb, nlm_testargs_cb grant_cb)
{
	struct nlm_globals *g;
	struct flock64 fl;
	struct nlm_host *host;
	struct netbuf *addr;
	struct nlm_vhold *nvp = NULL;
	char *netid;
	char *name;
	int error, flags;
	bool_t do_blocking = FALSE;
	bool_t do_mon_req = FALSE;
	enum nlm4_stats status;

	nlm_copy_netobj(&resp->cookie, &argp->cookie);

	name = argp->alock.caller_name;
	netid = svc_getnetid(sr->rq_xprt);
	addr = svc_getrpccaller(sr->rq_xprt);

	g = zone_getspecific(nlm_zone_key, curzone);
	host = nlm_host_findcreate(g, name, netid, addr);
	if (host == NULL) {
		DTRACE_PROBE4(no__host, struct nlm_globals *, g,
		    char *, name, char *, netid, struct netbuf *, addr);
		status = nlm4_denied_nolocks;
		goto doreply;
	}

	DTRACE_PROBE3(start, struct nlm_globals *, g,
	    struct nlm_host *, host, nlm4_lockargs *, argp);

	/*
	 * During the "grace period", only allow reclaim.
	 */
	if (argp->reclaim == 0 && NLM_IN_GRACE(g)) {
		status = nlm4_denied_grace_period;
		goto doreply;
	}

	/*
	 * Check whether we missed host shutdown event
	 */
	if (nlm_host_get_state(host) != argp->state)
		nlm_host_notify_server(host, argp->state);

	/*
	 * Get holded vnode when on lock operation.
	 * Only lock() and share() need vhold objects.
	 */
	nvp = nlm_fh_to_vhold(host, &argp->alock.fh);
	if (nvp == NULL) {
		resp->stat.stat = nlm4_stale_fh;
		goto doreply;
	}

	/*
	 * Try to lock non-blocking first.  If we succeed
	 * getting the lock, we can reply with the granted
	 * status directly and avoid the complications of
	 * making the "granted" RPC callback later.
	 *
	 * This also let's us find out now about some
	 * possible errors like EROFS, etc.
	 */
	nlm_init_flock(&fl, &argp->alock, nlm_host_get_sysid(host));
	fl.l_type = (argp->exclusive) ? F_WRLCK : F_RDLCK;

	flags = F_REMOTELOCK | FREAD | FWRITE;
	error = VOP_FRLOCK(nvp->nv_vp, F_SETLK, &fl, flags,
	    (u_offset_t)0, NULL, CRED(), NULL);

	DTRACE_PROBE3(setlk__res, struct flock64 *, &fl,
	    int, flags, int, error);

	switch (error) {
	case 0:
		/* Got it without waiting! */
		status = nlm4_granted;
		do_mon_req = TRUE;
		break;

	/* EINPROGRESS too? */
	case EAGAIN:
		/* We did not get the lock. Should we block? */
		if (argp->block == FALSE || grant_cb == NULL) {
			status = nlm4_denied;
			break;
		}
		/*
		 * Should block.  Try to reserve this thread
		 * so we can use it to wait for the lock and
		 * later send the granted message.  If this
		 * reservation fails, say "no resources".
		 */
		if (!svc_reserve_thread(sr->rq_xprt)) {
			status = nlm4_denied_nolocks;
			break;
		}
		/*
		 * OK, can detach this thread, so this call
		 * will block below (after we reply).
		 */
		status = nlm4_blocked;
		do_blocking = TRUE;
		do_mon_req = TRUE;
		break;

	case ENOLCK:
		/* Failed for lack of resources. */
		status = nlm4_denied_nolocks;
		break;

	default:
		status = nlm4_denied;
		break;
	}

doreply:
	resp->stat.stat = status;

	/*
	 * We get one of two function pointers; one for a
	 * normal RPC reply, and another for doing an RPC
	 * "callback" _res reply for a _msg function.
	 * Use either of those to send the reply now.
	 *
	 * If sending this reply fails, just leave the
	 * lock in the list for retransmitted requests.
	 * Cleanup is via unlock or host rele (statmon).
	 */
	if (reply_cb != NULL) {
		/* i.e. nlm_lock_1_reply */
		if (0 == (*reply_cb)(sr->rq_xprt, resp)) {
			svcerr_systemerr(sr->rq_xprt);
		}
	}
	if (res_cb != NULL) {
		nlm_rpc_t *rpcp;

		error = nlm_host_get_rpc(host, sr->rq_vers, &rpcp);
		if (error == 0) {
			enum clnt_stat stat;

			/* i.e. nlm_lock_res_1_cb */
			stat = (*res_cb)(resp, NULL, rpcp->nr_handle);
			if (stat != RPC_SUCCESS) {
				struct rpc_err err;

				CLNT_GETERR(rpcp->nr_handle, &err);
				NLM_ERR("NLM: do_lock CB, stat=%d err=%d\n",
				    stat, err.re_errno);
			}

			nlm_host_rele_rpc(host, rpcp);
		}
	}

	/*
	 * The reply has been sent to the client.
	 * Start monitoring this client (maybe).
	 *
	 * Note that the non-monitored (NM) calls pass grant_cb=NULL
	 * indicating that the client doesn't support RPC callbacks.
	 * No monitoring for these (lame) clients.
	 */
	if (do_mon_req && grant_cb != NULL)
		nlm_host_monitor(g, host, argp->state);

	if (do_blocking) {
		/*
		 * We need to block on this lock, and when that
		 * completes, do the granted RPC call. Note that
		 * we "reserved" this thread above, so we can now
		 * "detach" it from the RPC SVC pool, allowing it
		 * to block indefinitely if needed.
		 */
		(void) svc_detach_thread(sr->rq_xprt);
		nlm_block(argp, host, nvp, &fl, grant_cb, sr->rq_vers);
	}

	DTRACE_PROBE3(end, struct nlm_globals *, g,
	    struct nlm_host *, host, nlm4_res *, resp);

	nlm_vhold_release(host, nvp);
	nlm_host_release(g, host);
}

/*
 * Helper for nlm_do_lock(), partly for observability,
 * (we'll see a call blocked in this function) and
 * because nlm_do_lock() was getting quite long.
 */
static void
nlm_block(nlm4_lockargs *lockargs,
    struct nlm_host *host,
    struct nlm_vhold *nvp,
    struct flock64 *flp,
    nlm_testargs_cb grant_cb,
    rpcvers_t vers)
{
	nlm4_testargs args;
	int error;
	enum clnt_stat stat;
	nlm_rpc_t *rpcp;

	/*
	 * Keep a list of blocked locks on nh_pending, and use it
	 * to cancel these threads in nlm_destroy_client_pending.
	 *
	 * Check to see if this lock is already in the list
	 * and if not, add an entry for it.  Allocate first,
	 * then if we don't insert, free the new one.
	 * Caller already has vp held.
	 */

	if (nlm_slreq_register(host, nvp, flp) < 0) {
		/*
		 * Sleeping lock request with given fl is already
		 * registered by someone else. This means that
		 * some other thread is handling the request, let
		 * him do its work.
		 */
		return;
	}

	/* BSD: VOP_ADVLOCK(vp, NULL, F_SETLK, fl, F_REMOTE); */
	error = VOP_FRLOCK(nvp->nv_vp, F_SETLKW, flp,
	    F_REMOTELOCK | FREAD | FWRITE,
	    (u_offset_t)0, NULL, CRED(), NULL);

	/*
	 * Done waiting, it's time to unregister sleeping request
	 */
	nlm_slreq_unregister(host, nvp, flp);
	if (error != 0) {
		/*
		 * We failed getting the lock, but have no way to
		 * tell the client about that.  Let 'em time out.
		 */
		return;
	}

	error = nlm_host_get_rpc(host, vers, &rpcp);
	if (error != 0)
		return;

	/*
	 * Do the "granted" call-back to the client.
	 */
	args.cookie	= lockargs->cookie;
	args.exclusive	= lockargs->exclusive;
	args.alock	= lockargs->alock;
	stat = (*grant_cb)(&args, NULL, rpcp->nr_handle);
	if (stat != RPC_SUCCESS) {
		struct rpc_err err;

		CLNT_GETERR(rpcp->nr_handle, &err);
		NLM_ERR("NLM: grant CB, stat=%d err=%d\n",
		    stat, err.re_errno);
	}

	nlm_host_rele_rpc(host, rpcp);
}

/*
 * NLM_CANCEL, NLM_CANCEL_MSG,
 * NLM4_CANCEL, NLM4_CANCEL_MSG,
 * Client gives up waiting for a blocking lock.
 */
void
nlm_do_cancel(nlm4_cancargs *argp, nlm4_res *resp,
    struct svc_req *sr, nlm_res_cb cb)
{
	struct nlm_globals *g;
	struct nlm_host *host;
	struct netbuf *addr;
	struct nlm_vhold *nvp = NULL;
	char *netid;
	int error;
	bool_t slreq_unreg = FALSE;
	struct flock64 fl;

	nlm_copy_netobj(&resp->cookie, &argp->cookie);
	netid = svc_getnetid(sr->rq_xprt);
	addr = svc_getrpccaller(sr->rq_xprt);

	g = zone_getspecific(nlm_zone_key, curzone);
	host = nlm_host_find(g, netid, addr);
	if (host == NULL) {
		resp->stat.stat = nlm4_denied_nolocks;
		return;
	}

	DTRACE_PROBE3(start, struct nlm_globals *, g,
	    struct nlm_host *, host, nlm4_cancargs *, argp);

	if (NLM_IN_GRACE(g)) {
		resp->stat.stat = nlm4_denied_grace_period;
		goto out;
	}

	nvp = nlm_fh_to_vhold(host, &argp->alock.fh);
	if (nvp == NULL) {
		resp->stat.stat = nlm4_stale_fh;
		goto out;
	}

	nlm_init_flock(&fl, &argp->alock, nlm_host_get_sysid(host));
	fl.l_type = (argp->exclusive) ? F_WRLCK : F_RDLCK;
	if (nlm_slreq_unregister(host, nvp, &fl) == 0)
		slreq_unreg = TRUE;

	fl.l_type = F_UNLCK;

	/*
	 * Sleeping lock we're trying to cancel could
	 * already be applied. In this case we have to try
	 * to ask our local os/flock manager to unlock it.
	 * We interested in frlock retcode only if
	 * server-side sleeping request wasn't found.
	 */
	error = VOP_FRLOCK(nvp->nv_vp, F_SETLK, &fl,
	    F_REMOTELOCK | FREAD | FWRITE,
	    (u_offset_t)0, NULL, CRED(), NULL);

	resp->stat.stat = nlm4_granted;
	if (!slreq_unreg && error != 0)
		resp->stat.stat = nlm4_denied;

out:
	/*
	 * If we have a callback funtion, use that to
	 * deliver the response via another RPC call.
	 */
	if (cb != NULL) {
		nlm_rpc_t *rpcp;
		int stat;

		error = nlm_host_get_rpc(host, sr->rq_vers, &rpcp);
		if (error == 0) {
			/* i.e. nlm_cancel_res_4_cb */
			stat = (*cb)(resp, NULL, rpcp->nr_handle);
			if (stat != RPC_SUCCESS) {
				struct rpc_err err;

				CLNT_GETERR(rpcp->nr_handle, &err);
				NLM_ERR("NLM: do_cancel CB, stat=%d err=%d\n",
				    stat, err.re_errno);
			}

			nlm_host_rele_rpc(host, rpcp);
		}
	}

	DTRACE_PROBE3(end, struct nlm_globals *, g,
	    struct nlm_host *, host, nlm4_res *, resp);

	nlm_vhold_release(host, nvp);
	nlm_host_release(g, host);
}

/*
 * NLM_UNLOCK, NLM_UNLOCK_MSG,
 * NLM4_UNLOCK, NLM4_UNLOCK_MSG,
 * Client removes one of their locks.
 */
void
nlm_do_unlock(nlm4_unlockargs *argp, nlm4_res *resp,
    struct svc_req *sr, nlm_res_cb cb)
{
	struct nlm_globals *g;
	struct nlm_host *host;
	struct netbuf *addr;
	vnode_t *vp = NULL;
	char *netid;
	int error;
	bool_t nvp_check_locks = FALSE;
	struct flock64 fl;

	nlm_copy_netobj(&resp->cookie, &argp->cookie);

	netid = svc_getnetid(sr->rq_xprt);
	addr = svc_getrpccaller(sr->rq_xprt);

	g = zone_getspecific(nlm_zone_key, curzone);
	host = nlm_host_find(g, netid, addr);
	if (host == NULL) {
		resp->stat.stat = nlm4_denied_nolocks;
		return;
	}

	DTRACE_PROBE3(start, struct nlm_globals *, g,
	    struct nlm_host *, host, nlm4_unlockargs *, argp);

	if (NLM_IN_GRACE(g)) {
		resp->stat.stat = nlm4_denied_grace_period;
		goto out;
	}

	vp = nlm_fh_to_vp(&argp->alock.fh);
	if (vp == NULL) {
		resp->stat.stat = nlm4_stale_fh;
		goto out;
	}

	nlm_init_flock(&fl, &argp->alock, nlm_host_get_sysid(host));
	fl.l_type = F_UNLCK;

	/* BSD: VOP_ADVLOCK(nv->nv_vp, NULL, F_UNLCK, &fl, F_REMOTE); */
	error = VOP_FRLOCK(vp, F_SETLK, &fl,
	    F_REMOTELOCK | FREAD | FWRITE,
	    (u_offset_t)0, NULL, CRED(), NULL);

	/*
	 * Ignore the error - there is no result code for failure,
	 * only for grace period.
	 */
	DTRACE_PROBE1(unlock__res, int, error);
	resp->stat.stat = nlm4_granted;

out:
	/*
	 * If we have a callback funtion, use that to
	 * deliver the response via another RPC call.
	 */
	if (cb != NULL) {
		nlm_rpc_t *rpcp;
		int stat;

		error = nlm_host_get_rpc(host, sr->rq_vers, &rpcp);
		if (error == 0) {
			/* i.e. nlm_unlock_res_4_cb */
			stat = (*cb)(resp, NULL, rpcp->nr_handle);
			if (stat != RPC_SUCCESS) {
				struct rpc_err err;

				CLNT_GETERR(rpcp->nr_handle, &err);
				NLM_ERR("NLM: do_unlock CB, stat=%d err=%d\n",
				    stat, err.re_errno);
			}

			nlm_host_rele_rpc(host, rpcp);
		}
	}

	DTRACE_PROBE3(end, struct nlm_globals *, g,
	    struct nlm_host *, host, nlm4_res *, resp);

	if (vp != NULL)
		VN_RELE(vp);

	nlm_host_release(g, host);
}

/*
 * NLM_GRANTED, NLM_GRANTED_MSG,
 * NLM4_GRANTED, NLM4_GRANTED_MSG,
 *
 * This service routine is special.  It's the only one that's
 * really part of our NLM _client_ support, used by _servers_
 * to "call back" when a blocking lock from this NLM client
 * is granted by the server.  In this case, we _know_ there is
 * already an nlm_host allocated and held by the client code.
 * We want to find that nlm_host here.
 *
 * Over in nlm_call_lock(), the client encoded the sysid for this
 * server in the "owner handle" netbuf sent with our lock request.
 * We can now use that to find the nlm_host object we used there.
 * (NB: The owner handle is opaque to the server.)
 */
void
nlm_do_granted(nlm4_testargs *argp, nlm4_res *resp,
    struct svc_req *sr, nlm_res_cb cb)
{
	struct nlm_globals *g;
	struct nlm_owner_handle *oh;
	struct nlm_host *host;
	int error;

	nlm_copy_netobj(&resp->cookie, &argp->cookie);

	g = zone_getspecific(nlm_zone_key, curzone);
	oh = (void *) argp->alock.oh.n_bytes;
	host = nlm_host_find_by_sysid(g, oh->oh_sysid);
	if (host == NULL) {
		/* could not match alock */
		resp->stat.stat = nlm4_denied;
		return;
	}

	error = nlm_slock_grant(g, host, &argp->alock);
	resp->stat.stat = (error == 0) ?
		nlm4_granted : nlm4_denied;

	/*
	 * If we have a callback funtion, use that to
	 * deliver the response via another RPC call.
	 */
	if (cb != NULL) {
		nlm_rpc_t *rpcp;
		int stat;
		stat = nlm_host_get_rpc(host, sr->rq_vers, &rpcp);
		if (stat == 0) {
			/* i.e. nlm_granted_res_4_cb */
			stat = (*cb)(resp, NULL, rpcp->nr_handle);
			if (stat != RPC_SUCCESS) {
				struct rpc_err err;

				CLNT_GETERR(rpcp->nr_handle, &err);
				NLM_ERR("NLM: do_grantd CB, stat=%d err=%d\n",
				    stat, err.re_errno);
			}

			nlm_host_rele_rpc(host, rpcp);
		}
	}

	nlm_host_release(g, host);
}

/*
 * NLM_FREE_ALL, NLM4_FREE_ALL
 *
 * Destroy all lock state for the calling client.
 */
void
nlm_do_free_all(nlm4_notify *argp, void *res, struct svc_req *sr)
{
	struct nlm_globals *g;
	struct nlm_host *host;
	struct netbuf *addr;
	char *netid;
	char *name;

	name = argp->name;
	netid = svc_getnetid(sr->rq_xprt);
	addr = svc_getrpccaller(sr->rq_xprt);

	g = zone_getspecific(nlm_zone_key, curzone);
	host = nlm_host_find(g, netid, addr);
	if (host == NULL) {
		/* nothing to do */
		return;
	}

	/*
	 * Note that this does not do client-side cleanup.
	 * We want to do that ONLY if statd tells us the
	 * server has restarted.
	 */
	nlm_host_notify_server(host, argp->state);
	nlm_host_release(g, host);
	(void) res;
}

static void
nlm_init_shrlock(struct shrlock *shr,
    nlm4_share *nshare, struct nlm_host *host)
{

	switch (nshare->access) {
	default:
	case fsa_NONE:
		shr->s_access = 0;
		break;
	case fsa_R:
		shr->s_access = F_RDACC;
		break;
	case fsa_W:
		shr->s_access = F_WRACC;
		break;
	case fsa_RW:
		shr->s_access = F_RWACC;
		break;
	}

	switch (nshare->mode) {
	default:
	case fsm_DN:
		shr->s_deny = F_NODNY;
		break;
	case fsm_DR:
		shr->s_deny = F_RDDNY;
		break;
	case fsm_DW:
		shr->s_deny = F_WRDNY;
		break;
	case fsm_DRW:
		shr->s_deny = F_RWDNY;
		break;
	}

	shr->s_sysid = nlm_host_get_sysid(host);
	shr->s_pid = 0;
	shr->s_own_len = nshare->oh.n_len;
	shr->s_owner   = nshare->oh.n_bytes;
}

/*
 * NLM_SHARE, NLM4_SHARE
 *
 * Request a DOS-style share reservation
 */
void
nlm_do_share(nlm4_shareargs *argp, nlm4_shareres *resp, struct svc_req *sr)
{
	struct nlm_globals *g;
	struct nlm_host *host;
	struct netbuf *addr;
	struct nlm_vhold *nvp = NULL;
	char *netid;
	char *name;
	int error;
	struct shrlock shr;

	nlm_copy_netobj(&resp->cookie, &argp->cookie);

	name = argp->share.caller_name;
	netid = svc_getnetid(sr->rq_xprt);
	addr = svc_getrpccaller(sr->rq_xprt);

	g = zone_getspecific(nlm_zone_key, curzone);
	host = nlm_host_findcreate(g, name, netid, addr);
	if (host == NULL) {
		resp->stat = nlm4_denied_nolocks;
		return;
	}

	DTRACE_PROBE3(share__start, struct nlm_globals *, g,
	    struct nlm_host *, host, nlm4_shareargs *, argp);

	if (argp->reclaim == 0 && NLM_IN_GRACE(g)) {
		resp->stat = nlm4_denied_grace_period;
		goto out;
	}

	/*
	 * Get holded vnode when on lock operation.
	 * Only lock() and share() need vhold objects.
	 */
	nvp = nlm_fh_to_vhold(host, &argp->share.fh);
	if (nvp == NULL) {
		resp->stat = nlm4_stale_fh;
		goto out;
	}

	/* Convert to local form. */
	nlm_init_shrlock(&shr, &argp->share, host);
	error = VOP_SHRLOCK(nvp->nv_vp, F_SHARE, &shr,
	    FREAD | FWRITE, CRED(), NULL);

	if (error == 0) {
		resp->stat = nlm4_granted;
		nlm_host_monitor(g, host, 0);
	} else {
		resp->stat = nlm4_denied;
	}

out:
	DTRACE_PROBE3(share__end, struct nlm_globals *, g,
	    struct nlm_host *, host, nlm4_shareres *, resp);

	nlm_vhold_release(host, nvp);
	nlm_host_release(g, host);
}

/*
 * NLM_UNSHARE, NLM4_UNSHARE
 *
 * Release a DOS-style share reservation
 */
void
nlm_do_unshare(nlm4_shareargs *argp, nlm4_shareres *resp, struct svc_req *sr)
{
	struct nlm_globals *g;
	struct nlm_host *host;
	struct netbuf *addr;
	vnode_t *vp = NULL;
	char *netid;
	char *name;
	int error;
	struct shrlock shr;

	nlm_copy_netobj(&resp->cookie, &argp->cookie);

	name = argp->share.caller_name;
	netid = svc_getnetid(sr->rq_xprt);
	addr = svc_getrpccaller(sr->rq_xprt);

	g = zone_getspecific(nlm_zone_key, curzone);
	host = nlm_host_find(g, netid, addr);
	if (host == NULL) {
		resp->stat = nlm4_denied_nolocks;
		return;
	}

	DTRACE_PROBE3(unshare__start, struct nlm_globals *, g,
	    struct nlm_host *, host, nlm4_shareargs *, argp);

	if (NLM_IN_GRACE(g)) {
		resp->stat = nlm4_denied_grace_period;
		goto out;
	}

	vp = nlm_fh_to_vp(&argp->share.fh);
	if (vp == NULL) {
		resp->stat = nlm4_stale_fh;
		goto out;
	}

	/* Convert to local form. */
	nlm_init_shrlock(&shr, &argp->share, host);
	error = VOP_SHRLOCK(vp, F_UNSHARE, &shr,
	    FREAD | FWRITE, CRED(), NULL);

	(void) error;
	resp->stat = nlm4_granted;

out:
	DTRACE_PROBE3(unshare__end, struct nlm_globals *, g,
	    struct nlm_host *, host, nlm4_shareres *, resp);

	if (vp != NULL)
		VN_RELE(vp);

	nlm_host_release(g, host);
}
