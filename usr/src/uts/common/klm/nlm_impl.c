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
 * NFS LockManager, start/stop, support functions, etc.
 * Most of the interesting code is here.
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
#include <sys/class.h>
#include <sys/unistd.h>
#include <sys/vnode.h>
#include <sys/vfs.h>
#include <sys/queue.h>
#include <sys/bitmap.h>
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
#include <nfs/lm.h>

#include "nlm_impl.h"

/*
 * Number of attempts NLM tries to obtain RPC binding
 * of local statd.
 */
#define	NLM_NSM_RPCBIND_RETRIES 10

/*
 * Timeout (in seconds) NLM waits before making another
 * attempt to obtain RPC binding of local statd.
 */
#define	NLM_NSM_RPCBIND_TIMEOUT 5

/*
 * Total number of sysids in NLM sysid bitmap
 */
#define NLM_BMAP_NITEMS	(LM_SYSID_MAX + 1)

/*
 * Number of ulong_t words in bitmap that is used
 * for allocation of sysid numbers.
 */
#define	NLM_BMAP_WORDS  (NLM_BMAP_NITEMS / BT_NBIPUL)

/*
 * Given an interger x, the macro is returned
 * -1 if x is negative,
 *  0 if x is zero
 *  1 if x is positive
 */
#define	SIGN(x) (((x) < 0) - ((x) > 0))

/*
 * List of all Zone globals nlm_globals instences
 * linked together.
 */
static struct nlm_globals_list nlm_zones_list;

/*
 * NLM kmem caches
 */
static struct kmem_cache *nlm_hosts_cache = NULL;
static struct kmem_cache *nlm_vhold_cache = NULL;

/*
 * A bitmap for allocation of new sysids.
 * Sysid is a unique number between LM_SYSID
 * and LM_SYSID_MAX. Sysid represents unique remote
 * host that does file locks on the given host.
 */
static ulong_t	nlm_sysid_bmap[NLM_BMAP_WORDS];
static int	nlm_sysid_nidx;

/*
 * RPC service registrations for LOOPBACK,
 * allowed to call the real nlm_prog_2.
 * None of the others are used locally.
 */
static SVC_CALLOUT nlm_svcs_lo[] = {
	{ NLM_PROG, 2, 2, nlm_prog_2 } /* NLM_SM */
};
static SVC_CALLOUT_TABLE nlm_sct_lo = {
	sizeof (nlm_svcs_lo) / sizeof (nlm_svcs_lo[0]),
	FALSE,
	nlm_svcs_lo
};

static SVC_CALLOUT nlm_svcs_in[] = {
	{ NLM_PROG, 4, 4, nlm_prog_4 },	/* NLM4_VERS */
	{ NLM_PROG, 1, 3, nlm_prog_3 }	/* NLM_VERS - NLM_VERSX */
};

static SVC_CALLOUT_TABLE nlm_sct_in = {
	sizeof (nlm_svcs_in) / sizeof (nlm_svcs_in[0]),
	FALSE,
	nlm_svcs_in
};

krwlock_t lm_lck;

/*
 * NLM misc. function
 */
static void nlm_copy_netbuf(struct netbuf *, struct netbuf *);
static int nlm_netbuf_addrs_cmp(struct netbuf *, struct netbuf *);
static void nlm_kmem_reclaim(void *);

/*
 * NLM thread functions
 */
static void nlm_hosts_gc(struct nlm_globals *);
static void nlm_reclaimer(struct nlm_host *);

/*
 * NLM NSM functions
 */
static int nlm_nsm_init_knc(struct knetconfig *);
static int nlm_nsm_init(struct nlm_nsm *);
static void nlm_nsm_fini(struct nlm_nsm *);
static enum clnt_stat nlm_nsm_simu_crash(struct nlm_nsm *);
static enum clnt_stat nlm_nsm_stat(struct nlm_nsm *, int32_t *);
static enum clnt_stat nlm_nsm_mon(struct nlm_nsm *, char *, uint16_t);
static enum clnt_stat nlm_nsm_unmon(struct nlm_nsm *, char *);
static enum clnt_stat nlm_nsm_unmon_all(struct nlm_nsm *);

/*
 * NLM host functions
 */
static int nlm_host_ctor(void *, void *, int);
static void nlm_host_dtor(void *, void *);
static void nlm_host_destroy(struct nlm_host *);
static struct nlm_host *nlm_create_host(struct nlm_globals *,
    char *, const char *, struct knetconfig *, struct netbuf *);
static struct nlm_host *nlm_host_find_locked(struct nlm_globals *,
    const char *, struct netbuf *, avl_index_t *);
static void nlm_host_unregister(struct nlm_globals *, struct nlm_host *);
static void nlm_host_gc_vholds(struct nlm_host *);
static bool_t nlm_host_has_locks(struct nlm_host *);

/*
 * NLM vhold functions
 */
static int nlm_vhold_ctor(void *, void *, int);
static void nlm_vhold_dtor(void *, void *);
static void nlm_vhold_destroy(struct nlm_host *,
    struct nlm_vhold *);
static bool_t nlm_vhold_busy(struct nlm_host *, struct nlm_vhold *);

/*
 * NLM client/server sleeping locks functions
 */
struct nlm_slreq *nlm_slreq_find_locked(struct nlm_host *,
    struct nlm_vhold *, struct flock64 *);

/*
 * NLM initialization functions.
 */
void
nlm_init(void)
{
	nlm_hosts_cache = kmem_cache_create("nlm_host_cache",
	    sizeof (struct nlm_host), 0, nlm_host_ctor, nlm_host_dtor,
	    nlm_kmem_reclaim, NULL, NULL, 0);

	nlm_vhold_cache = kmem_cache_create("nlm_vhold_cache",
	    sizeof (struct nlm_vhold), 0, nlm_vhold_ctor, nlm_vhold_dtor,
	    NULL, NULL, NULL, 0);

	nlm_rpc_init();
	TAILQ_INIT(&nlm_zones_list);

	/* initialize sysids bitmap */
	bzero(nlm_sysid_bmap, sizeof (nlm_sysid_bmap));
	nlm_sysid_nidx = 1;

	/*
	 * Reserv the sysid #0, because it's associated
	 * with local locks only. Don't let to allocate
	 * it for remote locks.
	 */
	BT_SET(nlm_sysid_bmap, 0);
}

void
nlm_globals_register(struct nlm_globals *g)
{
	rw_enter(&lm_lck, RW_WRITER);
	TAILQ_INSERT_TAIL(&nlm_zones_list, g, nlm_link);
	rw_exit(&lm_lck);
}

void
nlm_globals_unregister(struct nlm_globals *g)
{
	rw_enter(&lm_lck, RW_WRITER);
	TAILQ_REMOVE(&nlm_zones_list, g, nlm_link);
	rw_exit(&lm_lck);
}

static void
nlm_kmem_reclaim(void *cdrarg)
{
	struct nlm_globals *g;

	rw_enter(&lm_lck, RW_READER);
	TAILQ_FOREACH(g, &nlm_zones_list, nlm_link)
		cv_broadcast(&g->nlm_gc_sched_cv);

	rw_exit(&lm_lck);
}

/*
 * Returns TRUE if the given vnode has
 * any active or sleeping locks.
 */
int
nlm_vp_active(const vnode_t *vp)
{
	struct nlm_globals *g;
	struct nlm_host *hostp;
	struct nlm_vhold *nvp;
	int active = 0;

	g = zone_getspecific(nlm_zone_key, curzone);

	/*
	 * Server side NLM has locks on the given vnode
	 * if there exist a vhold object that holds
	 * the given vnode "vp" in one of NLM hosts.
	 */
	mutex_enter(&g->lock);
	hostp = avl_first(&g->nlm_hosts_tree);
	while (hostp != NULL) {
		nvp = nlm_vhold_find_locked(hostp, vp);
		if (nvp != NULL) {
			active = 1;
			break;
		}

		hostp = AVL_NEXT(&g->nlm_hosts_tree, hostp);
	}

	mutex_exit(&g->lock);
	return (active);
}

/*
 * Allocate new unique sysid.
 * In case of failure (no available sysids)
 * return LM_NOSYSID.
 */
sysid_t
nlm_sysid_alloc(void)
{
	sysid_t ret_sysid = LM_NOSYSID;

	rw_enter(&lm_lck, RW_WRITER);
	if (nlm_sysid_nidx > LM_SYSID_MAX)
		nlm_sysid_nidx = LM_SYSID;

	if (!BT_TEST(nlm_sysid_bmap, nlm_sysid_nidx)) {
		BT_SET(nlm_sysid_bmap, nlm_sysid_nidx);
		ret_sysid = nlm_sysid_nidx++;
	} else {
		index_t id;

		id = bt_availbit(nlm_sysid_bmap, NLM_BMAP_NITEMS);
		if (id > 0) {
			nlm_sysid_nidx = id + 1;
			ret_sysid = id;
			BT_SET(nlm_sysid_bmap, id);
		}
	}

	rw_exit(&lm_lck);
	return (ret_sysid);
}

void
nlm_sysid_free(sysid_t sysid)
{
	ASSERT(sysid >= LM_SYSID && sysid <= LM_SYSID_MAX);

	rw_enter(&lm_lck, RW_WRITER);
	ASSERT(BT_TEST(nlm_sysid_bmap, sysid));
	BT_CLEAR(nlm_sysid_bmap, sysid);
	rw_exit(&lm_lck);
}

/*
 * NLM garbage collector thread (GC).
 *
 * NLM GC periodically checks whether there're any host objects
 * that can be cleaned up. It also releases stale vnodes that
 * live on the server side (under protection of vhold objects).
 *
 * NLM host objects are cleaned up from GC thread because
 * operations helping us to determine whether given host has
 * any locks can be quite expensive and it's not good to call
 * them every time the very last reference to the host is dropped.
 * Thus we use "lazy" approach for hosts cleanup.
 *
 * The work of GC is to release stale vnodes on the server side
 * and destroy hosts that haven't any locks and any activity for
 * some time (i.e. idle hosts).
 */
static void
nlm_gc(struct nlm_globals *g)
{
	struct nlm_host *hostp;
	clock_t now, idle_period;

	idle_period = SEC_TO_TICK(g->cn_idle_tmo);
	mutex_enter(&g->lock);
	for (;;) {
		/*
		 * GC thread can be explicitly scheduled from
		 * memory reclamation function.
		 */
		cv_timedwait(&g->nlm_gc_sched_cv, &g->lock,
		    ddi_get_lbolt() + idle_period);

		/*
		 * NLM is shutting down, time to die.
		 */
		if (g->run_status == NLM_ST_STOPPING)
			break;

		now = ddi_get_lbolt();
		DTRACE_PROBE2(gc__start, struct nlm_globals *, g,
		    clock_t, now);

		/*
		 * Handle all hosts that are unused at the moment
		 * until we meet one with idle timeout in future.
		 */
		while ((hostp = TAILQ_FIRST(&g->nlm_idle_hosts)) != NULL) {
			bool_t has_locks = FALSE;

			if (hostp->nh_idle_timeout > now)
				break;

			/*
			 * NOTE: it's important to drop nlm_globals lock
			 * before acquiring host lock, because order does
			 * matter. nlm_globals lock _always must be
			 * acquired before host lock and released after it.
			 */
			mutex_exit(&g->lock);
			mutex_enter(&hostp->nh_lock);

			/*
			 * nlm_globals lock was dropped earlier because
			 * garbage collecting of vholds and checking whether
			 * host has any locks/shares are expensive operations.
			 */
			nlm_host_gc_vholds(hostp);
			has_locks = nlm_host_has_locks(hostp);

			mutex_exit(&hostp->nh_lock);
			mutex_enter(&g->lock);

			/*
			 * While we were doing expensive operations outside of
			 * nlm_globals critical section, somebody could
			 * take the host, add lock/share to one of its vnodes
			 * and release the host back. If so, host's idle timeout
			 * is renewed and our information about locks on the
			 * given host is outdated.
			 */
			if (hostp->nh_idle_timeout > now)
				continue;

			/*
			 * Either host has locks or somebody has began to
			 * use it while we were outside the nlm_globals critical
			 * section. In both cases we have to renew host's
			 * timeout and put it to the end of LRU list.
			 */
			if (has_locks || hostp->nh_refs > 0) {
				TAILQ_REMOVE(&g->nlm_idle_hosts,
				    hostp, nh_link);
				hostp->nh_idle_timeout = now + idle_period;
				TAILQ_INSERT_TAIL(&g->nlm_idle_hosts,
				    hostp, nh_link);
				continue;
			}

			/*
			 * We're here if all the following conditions hold:
			 * 1) Host hasn't any locks or share reservations
			 * 2) Host is unused
			 * 3) Host wasn't touched by anyone at least for
			 *    g->cn_idle_tmo seconds.
			 *
			 * So, now we can destroy it.
			 */
			nlm_host_unregister(g, hostp);
			mutex_exit(&g->lock);

			nlm_host_unmonitor(g, hostp);
			nlm_host_destroy(hostp);
			mutex_enter(&g->lock);
			if (g->run_status == NLM_ST_STOPPING)
				break;

		}

		DTRACE_PROBE(gc__end);
	}

	DTRACE_PROBE1(gc__exit, struct nlm_globals *, g);

	/* Let others know that GC has died */
	g->nlm_gc_thread = NULL;
	mutex_exit(&g->lock);

	cv_broadcast(&g->nlm_gc_finish_cv);
	zthread_exit();
}

/*
 * Thread reclaim locks/shares acquired by the client side
 * on the given server represented by hostp.
 */
static void
nlm_reclaimer(struct nlm_host *hostp)
{
	struct nlm_globals *g;

	g = zone_getspecific(nlm_zone_key, curzone);
	nlm_reclaim_client(g, hostp);

	mutex_enter(&hostp->nh_lock);
	hostp->nh_flags &= ~NLM_NH_RECLAIM;
	cv_broadcast(&hostp->nh_recl_cv);
	mutex_exit(&hostp->nh_lock);

	/*
	 * Host was explicitly referenced before
	 * nlm_reclaim() was called, release it
	 * here.
	 */
	nlm_host_release(g, hostp);
	zthread_exit();
}

/*
 * Copy a struct netobj.  (see xdr.h)
 */
void
nlm_copy_netobj(struct netobj *dst, struct netobj *src)
{
	dst->n_len = src->n_len;
	dst->n_bytes = kmem_alloc(src->n_len, KM_SLEEP);
	bcopy(src->n_bytes, dst->n_bytes, src->n_len);
}

/*
 * NLM functions responsible for operations on NSM handle.
 */

/*
 * Initialize knetconfig that is used for communication
 * with local statd via loopback transport.
 */
static int
nlm_nsm_init_knc(struct knetconfig *knc)
{
	int error;
	vnode_t *vp;

	bzero(knc, sizeof (*knc));

	error = lookupname("/dev/ticotsord", UIO_SYSSPACE,
	    FOLLOW, NULLVPP, &vp);
	if (error != 0)
		return (error);

	knc->knc_semantics = NC_TPI_COTS_ORD;
	knc->knc_protofmly = NC_LOOPBACK;
	knc->knc_proto = NC_NOPROTO;
	knc->knc_rdev = vp->v_rdev;
	VN_RELE(vp);

	return (0);
}

/*
 * Initialize NSM handle that will be used to talk
 * to local statd.
 */
static int
nlm_nsm_init(struct nlm_nsm *nsm)
{
	CLIENT *clnt = NULL;
	char *addr, *nodename;
	enum clnt_stat stat;
	int error, retries;

	error = nlm_nsm_init_knc(&nsm->ns_knc);
	if (error != 0)
		return (error);

	/*
	 * Initialize an address of local statd we'll talk to.
	 * We use local transport for communication with local
	 * NSM, so the address will be simply our nodename follwed
	 * by a dot.
	 */
	nodename = uts_nodename();
	nsm->ns_addr.len = nsm->ns_addr.maxlen = strlen(nodename) + 1;
	nsm->ns_addr.buf = kmem_zalloc(nsm->ns_addr.len, KM_SLEEP);
	(void) strncpy(nsm->ns_addr.buf, nodename, nsm->ns_addr.len - 1);
	nsm->ns_addr.buf[nsm->ns_addr.len - 1] = '.';

	/*
	 * Try several times to get the port of local statd service,
	 * because it's possible that we start before statd registers
	 * on the rpcbind.
	 *
	 * If rpcbind_getaddr returns either RPC_INTR or
	 * RPC_PROGNOTREGISTERED, retry an attempt, but wait
	 * for NLM_NSM_RPCBIND_TIMEOUT seconds berofore.
	 */
	for (retries = 0; retries < NLM_NSM_RPCBIND_RETRIES; retries++) {
		stat = rpcbind_getaddr(&nsm->ns_knc, SM_PROG,
		    SM_VERS, &nsm->ns_addr);
		if (stat != RPC_SUCCESS) {
			if (stat == RPC_PROGNOTREGISTERED) {
				delay(SEC_TO_TICK(NLM_NSM_RPCBIND_TIMEOUT));
				continue;
			}
		}

		break;
	}

	if (stat != RPC_SUCCESS) {
		DTRACE_PROBE2(rpcbind__error, enum clnt_stat, stat,
		    int, retries);
		error = ENOENT;
		goto error;
	}

	/*
	 * Create a RPC handle that'll be used for
	 * communication with local statd
	 */
	error = clnt_tli_kcreate(&nsm->ns_knc, &nsm->ns_addr, SM_PROG, SM_VERS,
	    0, NLM_RPC_RETRIES, kcred, &clnt);
	if (error != 0)
		goto error;

	nsm->ns_handle = clnt;
	sema_init(&nsm->ns_sem, 1, NULL, SEMA_DEFAULT, NULL);
	return (0);

error:
	kmem_free(nsm->ns_addr.buf, nsm->ns_addr.maxlen);
	if (clnt)
		CLNT_DESTROY(clnt);

	return (error);
}

static void
nlm_nsm_fini(struct nlm_nsm *nsm)
{
	kmem_free(nsm->ns_addr.buf, nsm->ns_addr.maxlen);
	CLNT_DESTROY(nsm->ns_handle);
	nsm->ns_handle = NULL;
	sema_destroy(&nsm->ns_sem);
}

static enum clnt_stat
nlm_nsm_simu_crash(struct nlm_nsm *nsm)
{
	enum clnt_stat stat;

	sema_v(&nsm->ns_sem);
	stat = sm_simu_crash_1(NULL, NULL, nsm->ns_handle);
	sema_p(&nsm->ns_sem);

	return (stat);
}

static enum clnt_stat
nlm_nsm_stat(struct nlm_nsm *nsm, int32_t *out_stat)
{
	struct sm_name args;
	struct sm_stat_res res;
	enum clnt_stat stat;

	args.mon_name = uts_nodename();
	bzero(&res, sizeof (res));

	sema_v(&nsm->ns_sem);
	stat = sm_stat_1(&args, &res, nsm->ns_handle);
	if (stat != RPC_SUCCESS) {
		sema_p(&nsm->ns_sem);
		return (stat);
	}

	sema_p(&nsm->ns_sem);
	*out_stat = res.state;

	return (stat);
}

static enum clnt_stat
nlm_nsm_mon(struct nlm_nsm *nsm, char *hostname, uint16_t priv)
{
	struct mon args;
	struct sm_stat_res res;
	enum clnt_stat stat;

	bzero(&args, sizeof (args));
	bzero(&res, sizeof (res));

	args.mon_id.mon_name = hostname;
	args.mon_id.my_id.my_name = uts_nodename();
	args.mon_id.my_id.my_prog = NLM_PROG;
	args.mon_id.my_id.my_vers = NLM_SM;
	args.mon_id.my_id.my_proc = NLM_SM_NOTIFY1;
	bcopy(&priv, args.priv, sizeof (priv));

	sema_v(&nsm->ns_sem);
	stat = sm_mon_1(&args, &res, nsm->ns_handle);
	sema_p(&nsm->ns_sem);

	return (stat);
}

static enum clnt_stat
nlm_nsm_unmon(struct nlm_nsm *nsm, char *hostname)
{
	struct mon_id args;
	struct sm_stat res;
	enum clnt_stat stat;

	bzero(&args, sizeof (args));
	bzero(&res, sizeof (res));

	args.mon_name = hostname;
	args.my_id.my_name = uts_nodename();
	args.my_id.my_prog = NLM_PROG;
	args.my_id.my_vers = NLM_SM;
	args.my_id.my_proc = NLM_SM_NOTIFY1;

	sema_v(&nsm->ns_sem);
	stat = sm_unmon_1(&args, &res, nsm->ns_handle);
	sema_p(&nsm->ns_sem);

	return (stat);
}

static enum clnt_stat
nlm_nsm_unmon_all(struct nlm_nsm *nsm)
{
	struct my_id args;
	struct sm_stat res;
	enum clnt_stat stat;

	bzero(&args, sizeof (args));
	bzero(&res, sizeof (res));

	args.my_name = uts_nodename();
	args.my_prog = NLM_PROG;
	args.my_vers = NLM_SM;
	args.my_proc = NLM_SM_NOTIFY1;

	sema_v(&nsm->ns_sem);
	stat = sm_unmon_all_1(&args, &res, nsm->ns_handle);
	sema_p(&nsm->ns_sem);

	return (stat);
}

/*
 * Get NLM vhold object corresponding to vnode "vp".
 * If no such object was found, create a new one.
 *
 * The purpose of this function is to associate vhold
 * object with given vnode, so that:
 * 1) vnode is hold (VN_HOLD) while vhold object is alive.
 * 2) host has a track of all vnodes it touched by lock
 *    or share operations. These vnodes are accessible
 *    via collection of vhold objects.
 */
struct nlm_vhold *
nlm_vhold_get(struct nlm_host *hostp, vnode_t *vp)
{
	struct nlm_vhold *nvp, *new_nvp = NULL;

	mutex_enter(&hostp->nh_lock);
	nvp = nlm_vhold_find_locked(hostp, vp);
	if (nvp != NULL)
		goto out;

	/* nlm_vhold wasn't found, then create a new one */
	mutex_exit(&hostp->nh_lock);
	new_nvp = kmem_cache_alloc(nlm_vhold_cache, KM_SLEEP);

	/*
	 * Check if another thread has already
	 * created the same nlm_vhold.
	 */
	mutex_enter(&hostp->nh_lock);
	nvp = nlm_vhold_find_locked(hostp, vp);
	if (nvp == NULL) {
		nvp = new_nvp;
		new_nvp = NULL;

		TAILQ_INIT(&nvp->nv_slreqs);
		nvp->nv_vp = vp;
		nvp->nv_refcnt = 1;
		VN_HOLD(nvp->nv_vp);

		VERIFY(mod_hash_insert(hostp->nh_vholds_by_vp,
		    (mod_hash_key_t)vp, (mod_hash_val_t)nvp) == 0);
		TAILQ_INSERT_TAIL(&hostp->nh_vholds_list, nvp, nv_link);
	}

out:
	mutex_exit(&hostp->nh_lock);
	if (new_nvp != NULL)
		kmem_cache_free(nlm_vhold_cache, new_nvp);

	return (nvp);
}

/*
 * Drop a reference to vhold object nvp.
 */
void
nlm_vhold_release(struct nlm_host *hostp, struct nlm_vhold *nvp)
{
	if (nvp == NULL)
		return;

	mutex_enter(&hostp->nh_lock);
	ASSERT(nvp->nv_refcnt > 0);
	nvp->nv_refcnt--;
	mutex_exit(&hostp->nh_lock);
}

static void
nlm_vhold_destroy(struct nlm_host *hostp, struct nlm_vhold *nvp)
{
	ASSERT(MUTEX_HELD(&hostp->nh_lock));

	VERIFY(mod_hash_remove(hostp->nh_vholds_by_vp,
	    (mod_hash_key_t)nvp->nv_vp,
	    (mod_hash_val_t)&nvp) == 0);

	TAILQ_REMOVE(&hostp->nh_vholds_list, nvp, nv_link);
	VN_RELE(nvp->nv_vp);
	nvp->nv_vp = NULL;

	kmem_cache_free(nlm_vhold_cache, nvp);
}

/*
 * Return TRUE if the given vhold is busy.
 * Vhold object is considered to be "busy" when
 * all the following conditions hold:
 * 1) No one uses it at the moment;
 * 2) It hasn't any locks;
 * 3) It hasn't any share reservations;
 */
static bool_t
nlm_vhold_busy(struct nlm_host *hostp, struct nlm_vhold *nvp)
{
	vnode_t *vp;
	int sysid;

	ASSERT(MUTEX_HELD(&hostp->nh_lock));

	if (nvp->nv_refcnt > 0)
		return (TRUE);

	vp = nvp->nv_vp;
	sysid = nlm_host_get_sysid(hostp);
	if (flk_has_remote_locks_for_sysid(vp, sysid) ||
	    shr_has_remote_shares(vp, sysid))
		return (TRUE);

	return (FALSE);
}

static int
nlm_vhold_ctor(void *datap, void *cdrarg, int kmflags)
{
	struct nlm_vhold *nvp = (struct nlm_vhold *)datap;

	bzero(nvp, sizeof (*nvp));
	return (0);
}

static void
nlm_vhold_dtor(void *datap, void *cdrarg)
{
	struct nlm_vhold *nvp = (struct nlm_vhold *)datap;

	ASSERT(nvp->nv_refcnt == 0);
	ASSERT(TAILQ_EMPTY(&nvp->nv_slreqs));
	ASSERT(nvp->nv_vp == NULL);
}

struct nlm_vhold *
nlm_vhold_find_locked(struct nlm_host *hostp, const vnode_t *vp)
{
	struct nlm_vhold *nvp = NULL;

	ASSERT(MUTEX_HELD(&hostp->nh_lock));
	(void) mod_hash_find(hostp->nh_vholds_by_vp,
	    (mod_hash_key_t)vp,
	    (mod_hash_val_t)&nvp);

	if (nvp != NULL)
		nvp->nv_refcnt++;

	return (nvp);
}

/*
 * NLM host functions
 */
static void
nlm_copy_netbuf(struct netbuf *dst, struct netbuf *src)
{
	ASSERT(src->len <= src->maxlen);

	dst->maxlen = src->maxlen;
	dst->len = src->len;
	dst->buf = kmem_zalloc(src->maxlen, KM_SLEEP);
	bcopy(src->buf, dst->buf, src->len);
}

static int
nlm_host_ctor(void *datap, void *cdrarg, int kmflags)
{
	struct nlm_host *hostp = (struct nlm_host *)datap;

	bzero(hostp, sizeof (*hostp));
	return (0);
}

static void
nlm_host_dtor(void *datap, void *cdrarg)
{
	struct nlm_host *hostp = (struct nlm_host *)datap;
	ASSERT(hostp->nh_refs == 0);
}

static void
nlm_host_unregister(struct nlm_globals *g, struct nlm_host *hostp)
{
	ASSERT(hostp->nh_refs == 0);

	avl_remove(&g->nlm_hosts_tree, hostp);
	VERIFY(mod_hash_remove(g->nlm_hosts_hash,
	    (mod_hash_key_t)(uintptr_t)hostp->nh_sysid,
	    (mod_hash_val_t)&hostp) == 0);
	TAILQ_REMOVE(&g->nlm_idle_hosts, hostp, nh_link);
}

/*
 * Free resources used by a host. This is called after the reference
 * count has reached zero so it doesn't need to worry about locks.
 */
static void
nlm_host_destroy(struct nlm_host *hostp)
{
	ASSERT(hostp->nh_name != NULL);
	ASSERT(hostp->nh_netid != NULL);
	ASSERT(TAILQ_EMPTY(&hostp->nh_vholds_list));

	strfree(hostp->nh_name);
	strfree(hostp->nh_netid);
	kmem_free(hostp->nh_addr.buf, sizeof (struct netbuf));

	if (hostp->nh_sysid != LM_NOSYSID)
		nlm_sysid_free(hostp->nh_sysid);

	nlm_rpc_cache_destroy(hostp);

	ASSERT(TAILQ_EMPTY(&hostp->nh_vholds_list));
	mod_hash_destroy_ptrhash(hostp->nh_vholds_by_vp);

	mutex_destroy(&hostp->nh_lock);
	cv_destroy(&hostp->nh_rpcb_cv);
	cv_destroy(&hostp->nh_recl_cv);

	kmem_cache_free(nlm_hosts_cache, hostp);
}

/*
 * Cleanup SERVER-side state after a client restarts,
 * or becomes unresponsive, or whatever.
 *
 * We unlock any active locks owned by the host.
 * When rpc.lockd is shutting down,
 * this function is called with newstate set to zero
 * which allows us to cancel any pending async locks
 * and clear the locking state.
 *
 * When "state" is 0, we don't update host's state,
 * but cleanup all remote locks on the host.
 * It's useful to call this function for resources
 * cleanup.
 */
void
nlm_host_notify_server(struct nlm_host *hostp, int32_t state)
{
	struct nlm_vhold *nvp;
	struct nlm_slreq *slr;
	struct nlm_slreq_list slreqs2free;
	int sysid;

	TAILQ_INIT(&slreqs2free);
	mutex_enter(&hostp->nh_lock);
	if (state != 0)
		hostp->nh_state = state;

	sysid = nlm_host_get_sysid(hostp);
	TAILQ_FOREACH(nvp, &hostp->nh_vholds_list, nv_link) {

		/* cleanup sleeping requests at first */
		while ((slr = TAILQ_FIRST(&nvp->nv_slreqs)) != NULL) {
			TAILQ_REMOVE(&nvp->nv_slreqs, slr, nsr_link);

			/*
			 * Instead of freeing cancelled sleeping request
			 * here, we add it to the linked list created
			 * on the stack in order to do all frees outside
			 * the critical section.
			 */
			TAILQ_INSERT_TAIL(&slreqs2free, slr, nsr_link);
		}

		mutex_exit(&hostp->nh_lock);

		/* cleanup all active locks and shares */
		cleanlocks(nvp->nv_vp, IGN_PID, sysid);
		cleanshares_by_sysid(nvp->nv_vp, sysid);
		mutex_enter(&hostp->nh_lock);
	}

	mutex_exit(&hostp->nh_lock);
	while ((slr = TAILQ_FIRST(&slreqs2free)) != NULL) {
		TAILQ_REMOVE(&slreqs2free, slr, nsr_link);
		kmem_free(slr, sizeof (*slr));
	}
}

/*
 * Cleanup CLIENT-side state after a server restarts,
 * or becomes unresponsive, or whatever.
 *
 * This is called by the local NFS statd when we receive a
 * host state change notification.  (also nlm_svc_stopping)
 *
 * Deal with a server restart.  If we are stopping the
 * NLM service, we'll have newstate == 0, and will just
 * cancel all our client-side lock requests.  Otherwise,
 * star the "recovery" process to reclaim any locks
 * we hold on this server.
 */

void
nlm_host_notify_client(struct nlm_host *hostp, int32_t state)
{
	mutex_enter(&hostp->nh_lock);
	hostp->nh_state = state;
	if (hostp->nh_flags & NLM_NH_RECLAIM) {
		/*
		 * Either host's state is up to date or
		 * host is already in recovery.
		 */
		mutex_exit(&hostp->nh_lock);
		return;
	}

	hostp->nh_flags |= NLM_NH_RECLAIM;

	/*
	 * Host will be released by the recovery thread,
	 * thus we need to increment refcount.
	 */
	hostp->nh_refs++;
	mutex_exit(&hostp->nh_lock);

	(void) zthread_create(NULL, 0, nlm_reclaimer,
	    hostp, 0, minclsyspri);
}

/*
 * The function is called when NLM client detects that
 * server has entered in grace period and client needs
 * to wait until reclamation process (if any) does
 * its job.
 */
int
nlm_host_wait_grace(struct nlm_host *hostp)
{
	struct nlm_globals *g;
	int error = 0;

	g = zone_getspecific(nlm_zone_key, curzone);
	mutex_enter(&hostp->nh_lock);

	do {
		int rc;

		rc = cv_timedwait_sig(&hostp->nh_recl_cv,
		    &hostp->nh_lock, ddi_get_lbolt() +
		    SEC_TO_TICK(g->retrans_tmo));

		if (rc == 0) {
			error = EINTR;
			break;
		}
	} while (hostp->nh_flags & NLM_NH_RECLAIM);

	mutex_exit(&hostp->nh_lock);
	return (error);
}

/*
 * Create a new NLM host.
 *
 * NOTE: The in-kernel RPC (kRPC) subsystem uses TLI/XTI,
 * which needs both a knetconfig and an address when creating
 * endpoints. Thus host object stores both knetconfig and
 * netid.
 */
static struct nlm_host *
nlm_create_host(struct nlm_globals *g, char *name,
    const char *netid, struct knetconfig *knc, struct netbuf *naddr)
{
	struct nlm_host *host;

	host = kmem_cache_alloc(nlm_hosts_cache, KM_SLEEP);

	mutex_init(&host->nh_lock, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&host->nh_rpcb_cv, NULL, CV_DEFAULT, NULL);
	cv_init(&host->nh_recl_cv, NULL, CV_DEFAULT, NULL);

	host->nh_sysid = LM_NOSYSID;
	host->nh_refs = 1;
	host->nh_name = strdup(name);
	host->nh_netid = strdup(netid);
	host->nh_knc = *knc;
	nlm_copy_netbuf(&host->nh_addr, naddr);

	host->nh_state = 0;
	host->nh_rpcb_state = NRPCB_NEED_UPDATE;

	host->nh_vholds_by_vp = mod_hash_create_ptrhash("nlm vholds hash",
	    32, mod_hash_null_valdtor, sizeof (vnode_t));

	TAILQ_INIT(&host->nh_vholds_list);
	TAILQ_INIT(&host->nh_rpchc);

	return (host);
}

/*
 * Cancel all client side sleeping locks owned by given host.
 */
void
nlm_host_cancel_slocks(struct nlm_globals *g, struct nlm_host *hostp)
{
	struct nlm_slock *nslp;

	mutex_enter(&g->lock);
	TAILQ_FOREACH(nslp, &g->nlm_slocks, nsl_link) {
		if (nslp->nsl_host == hostp) {
			nslp->nsl_state = NLM_SL_CANCELLED;
			cv_broadcast(&nslp->nsl_cond);
		}
	}

	mutex_exit(&g->lock);
}

/*
 * Garbage collect stale vhold objects.
 *
 * In other words check whether vnodes that are
 * held by vnold objects still have any locks
 * or shares or still in use. If they aren't,
 * just destroy them.
 */
static void
nlm_host_gc_vholds(struct nlm_host *hostp)
{
	struct nlm_vhold *nvp, *nvp_tmp;

	ASSERT(MUTEX_HELD(&hostp->nh_lock));
	TAILQ_FOREACH_SAFE(nvp, nvp_tmp, &hostp->nh_vholds_list, nv_link) {
		if (nlm_vhold_busy(hostp, nvp))
			continue;

		nlm_vhold_destroy(hostp, nvp);
	}
}

/*
 * Determine whether the given host owns any
 * locks or share reservations.
 */
static bool_t
nlm_host_has_locks(struct nlm_host *hostp)
{
	ASSERT(MUTEX_HELD(&hostp->nh_lock));

	/*
	 * Check the server side at first.
	 * It's cheap and simple: if server has
	 * any locks/shares there must be vhold
	 * object storing the affected vnode.
	 *
	 * NOTE: We don't need to check sleeping
	 * locks on the server side, because if
	 * server side sleeping lock is alive,
	 * there must be a vhold object corresponding
	 * to target vnode.
	 */
	if (!TAILQ_EMPTY(&hostp->nh_vholds_list))
		return (TRUE);

	/*
	 * Then check whether cliet side made any locks.
	 *
	 * XXX: It's not the way I'd like to do the check,
	 * because flk_sysid_has_locks() can be very
	 * expensive by design. Unfortunatelly it iterates
	 * throght all locks on the system, doesn't matter
	 * were they made on remote system via NLM or
	 * on local system via reclock. To understand the
	 * problem, consider that there're dozens of thousands
	 * of locks that are made on some ZFS dataset. And there's
	 * another dataset shared by NFS where NLM client had locks
	 * some time ago, but doesn't have them now.
	 * In this case flk_sysid_has_locks() will iterate
	 * thrught dozens of thousands locks until it returns us
	 * FALSE.
	 * Oh, I hope that in shiny future somebody will make
	 * local lock manager (os/flock.c) better, so that
	 * it'd be more friedly to remote locks and
	 * flk_sysid_has_locks() wouldn't be so expensive.
	 */
	if (flk_sysid_has_locks(nlm_host_get_sysid(hostp) |
	    LM_SYSID_CLIENT, FLK_QUERY_ACTIVE))
		return (TRUE);

	/*
	 * FIXME: Share reservations on the client side are
	 * temporary disabled. The reason is that there's no
	 * function similar to flk_sysid_has_locks() for share
	 * reservations, so we can not determine whether host
	 * has any shares. Actually os/share.c can answer a question
	 * whether particular _vnode_ has any shares. But on the
	 * client side we don't have a track of vnodes. The problem
	 * having collection of touched vnodes on the client side
	 * is that client (in contrast to server) must release vnode
	 * (by VN_RELE) ASAP, because NFS code won't be able to
	 * unmount the filesystem with some vnodes in use, it also
	 * has a tricy part of file removing code that would affect
	 * us if we would track vnodes on the client. NFS file
	 * removing functions (see nfs3_remove() as an example)
	 * won't remove a file if its vnode is held by someone.
	 * Instead it just renames a file and gives it randomly
	 * generated name (for example: .nfsB701). I don't know
	 * why it does this...
	 * So for this moment I don't know how to check whether
	 * host has share reservations without keeping track
	 * of vnodes touched by the host on the client side.
	 * Moreover, I don't know how to track vnodes on the
	 * client side without some modification of NFSv2/NFSv3
	 * code.
	 */

	return (FALSE);
}

/*
 * This function compares only addresses of two netbufs
 * that belong to NC_TCP[6] or NC_UDP[6] protofamily.
 * Port part of netbuf is ignored.
 *
 * Return values:
 *  -1: nb1's address is "smaller" than nb2's
 *   0: addresses are equal
 *   1: nb1's address is "greater" than nb2's
 */
static int
nlm_netbuf_addrs_cmp(struct netbuf *nb1, struct netbuf *nb2)
{
	union nlm_addr {
		struct sockaddr sa;
		struct sockaddr_in sin;
		struct sockaddr_in6 sin6;
	} *na1, *na2;
	int res;

	na1 = (union nlm_addr *)nb1->buf;
	na2 = (union nlm_addr *)nb2->buf;

	if (na1->sa.sa_family < na2->sa.sa_family)
		return (-1);
	if (na1->sa.sa_family > na2->sa.sa_family)
		return (1);

	switch (na1->sa.sa_family) {
	case AF_INET:
		res = memcmp(&na1->sin.sin_addr, &na2->sin.sin_addr,
		    sizeof (na1->sin.sin_addr));
		break;
	case AF_INET6:
		res = memcmp(&na1->sin6.sin6_addr, &na2->sin6.sin6_addr,
		    sizeof (na1->sin6.sin6_addr));
		break;
	default:
		VERIFY(0);
		break;
	}

	return (SIGN(res));
}

/*
 * Compare two nlm hosts.
 * Return values:
 * -1: host1 is "smaller" than host2
 *  0: host1 is equal to host2
 *  1: host1 is "greater" than host2
 */
int
nlm_host_cmp(const void *p1, const void *p2)
{
	struct nlm_host *h1 = (struct nlm_host *)p1;
	struct nlm_host *h2 = (struct nlm_host *)p2;
	int res;

	res = nlm_netbuf_addrs_cmp(&h1->nh_addr, &h2->nh_addr);
	if (res != 0)
		return (res);

	res = strcmp(h1->nh_netid, h2->nh_netid);
	return (SIGN(res));
}

/*
 * Find the host specified by...  (see below)
 * If found, increment the ref count.
 */
static struct nlm_host *
nlm_host_find_locked(struct nlm_globals *g, const char *netid,
    struct netbuf *naddr, avl_index_t *wherep)
{
	struct nlm_host *hostp, key;
	avl_index_t pos;

	ASSERT(MUTEX_HELD(&g->lock));

	key.nh_netid = (char *)netid;
	key.nh_addr.buf = naddr->buf;
	key.nh_addr.len = naddr->len;
	key.nh_addr.maxlen = naddr->maxlen;

	hostp = avl_find(&g->nlm_hosts_tree, &key, &pos);

	if (hostp != NULL) {
		/*
		 * Host is inuse now. Remove it from idle
		 * hosts list if needed.
		 */
		if (hostp->nh_refs == 0)
			TAILQ_REMOVE(&g->nlm_idle_hosts, hostp, nh_link);

		hostp->nh_refs++;
	}
	if (wherep != NULL)
		*wherep = pos;

	return (hostp);
}

/*
 * Find NLM host for the given name and address.
 */
struct nlm_host *
nlm_host_find(struct nlm_globals *g, const char *netid,
    struct netbuf *addr)
{
	struct nlm_host *hostp = NULL;

	mutex_enter(&g->lock);
	if (g->run_status != NLM_ST_UP)
		goto out;

	hostp = nlm_host_find_locked(g, netid, addr, NULL);

out:
	mutex_exit(&g->lock);
	return (hostp);
}


/*
 * Find or create an NLM host for the given name and address.
 *
 * The remote host is determined by all of: name, netidd, address.
 * Note that the netid is whatever nlm_svc_add_ep() gave to
 * svc_tli_kcreate() for the service binding.  If any of these
 * are different, allocate a new host (new sysid).
 */
struct nlm_host *
nlm_host_findcreate(struct nlm_globals *g, char *name,
    const char *netid, struct netbuf *addr)
{
	int err;
	struct nlm_host *host, *newhost = NULL;
	struct knetconfig knc;
	avl_index_t where;

	mutex_enter(&g->lock);
	if (g->run_status != NLM_ST_UP) {
		mutex_exit(&g->lock);
		return (NULL);
	}

	host = nlm_host_find_locked(g, netid, addr, NULL);
	mutex_exit(&g->lock);
	if (host != NULL)
		return (host);

	err = nlm_knetconfig_from_netid(netid, &knc);
	if (err)
		return (NULL);
	/*
	 * Do allocations (etc.) outside of mutex,
	 * and then check again before inserting.
	 */
	newhost = nlm_create_host(g, name, netid, &knc, addr);
	newhost->nh_sysid = nlm_sysid_alloc();
	if (newhost->nh_sysid == LM_NOSYSID)
		goto out;

	mutex_enter(&g->lock);
	host = nlm_host_find_locked(g, netid, addr, &where);
	if (host == NULL) {
		host = newhost;
		newhost = NULL;

		/*
		 * Insert host to the hosts AVL tree that is
		 * used to lookup by <netid, address> pair.
		 */
		avl_insert(&g->nlm_hosts_tree, host, where);

		/*
		 * Insert host ot the hosts hash table that is
		 * used to lookup host by sysid.
		 */
		VERIFY(mod_hash_insert(g->nlm_hosts_hash,
		    (mod_hash_key_t)(uintptr_t)host->nh_sysid,
		    (mod_hash_val_t)host) == 0);
	}

	mutex_exit(&g->lock);

out:
	if (newhost != NULL)
		nlm_host_destroy(newhost);

	return (host);
}

/*
 * Find the NLM host that matches the value of 'sysid'.
 * If found, return it with a new ref,
 * else return NULL.
 */
struct nlm_host *
nlm_host_find_by_sysid(struct nlm_globals *g, sysid_t sysid)
{
	mod_hash_val_t hval;
	struct nlm_host *hostp = NULL;

	mutex_enter(&g->lock);
	if (g->run_status != NLM_ST_UP)
		goto out;

	mod_hash_find(g->nlm_hosts_hash,
	    (mod_hash_key_t)(uintptr_t)sysid,
	    (mod_hash_val_t)&hostp);

	if (hostp == NULL)
		goto out;

	/*
	 * Host is inuse now. Remove it
	 * from idle hosts list if needed.
	 */
	if (hostp->nh_refs == 0)
		TAILQ_REMOVE(&g->nlm_idle_hosts, hostp, nh_link);

	hostp->nh_refs++;

out:
	mutex_exit(&g->lock);
	return (hostp);
}

/*
 * Release the given host.
 * I.e. drop a reference that was taken earlier by one of
 * the following functions: nlm_host_findcreate(), nlm_host_find(),
 * nlm_host_find_by_sysid().
 *
 * When the very last reference is dropped, host is moved to
 * so-called "idle state". All hosts that are in idle state
 * have an idle timeout. If timeout is expired, GC thread
 * checks whether hosts have any locks and if they heven't
 * any, it removes them.
 * NOTE: only unused hosts can be in idle state.
 */
void
nlm_host_release(struct nlm_globals *g, struct nlm_host *hostp)
{
	if (hostp == NULL)
		return;

	mutex_enter(&g->lock);
	ASSERT(hostp->nh_refs > 0);

	hostp->nh_refs--;
	if (hostp->nh_refs != 0) {
		mutex_exit(&g->lock);
		return;
	}

	/*
	 * The very last reference to the host was dropped,
	 * thus host is unused now. Set its idle timeout
	 * and move it to the idle hosts LRU list.
	 */
	hostp->nh_idle_timeout = ddi_get_lbolt() +
	    SEC_TO_TICK(g->cn_idle_tmo);

	TAILQ_INSERT_TAIL(&g->nlm_idle_hosts, hostp, nh_link);
	mutex_exit(&g->lock);
}

/*
 * Unregister this NLM host (NFS client) with the local statd
 * due to idleness (no locks held for a while).
 */
void
nlm_host_unmonitor(struct nlm_globals *g, struct nlm_host *host)
{
	enum clnt_stat stat;

	VERIFY(host->nh_refs == 0);
	if (!(host->nh_flags & NLM_NH_MONITORED))
		return;

	host->nh_flags &= ~NLM_NH_MONITORED;
	stat = nlm_nsm_unmon(&g->nlm_nsm, host->nh_name);
	if (stat != RPC_SUCCESS) {
		NLM_WARN("NLM: Failed to contact statd, stat=%d\n", stat);
		return;
	}
}

/*
 * Ask the local NFS statd to begin monitoring this host.
 * It will call us back when that host restarts, using the
 * prog,vers,proc specified below, i.e. NLM_SM_NOTIFY1,
 * which is handled in nlm_do_notify1().
 */
void
nlm_host_monitor(struct nlm_globals *g, struct nlm_host *host, int state)
{
	enum clnt_stat stat;

	if (state != 0 && host->nh_state == 0) {
		/*
		 * This is the first time we have seen an NSM state
		 * Value for this host. We record it here to help
		 * detect host reboots.
		 */
		host->nh_state = state;
	}

	mutex_enter(&host->nh_lock);
	if (host->nh_flags & NLM_NH_MONITORED) {
		mutex_exit(&host->nh_lock);
		return;
	}

	host->nh_flags |= NLM_NH_MONITORED;
	mutex_exit(&host->nh_lock);

	/*
	 * Tell statd how to call us with status updates for
	 * this host. Updates arrive via nlm_do_notify1().
	 *
	 * We put our assigned system ID value in the priv field to
	 * make it simpler to find the host if we are notified of a
	 * host restart.
	 */
	stat = nlm_nsm_mon(&g->nlm_nsm, host->nh_name,
	    (uint16_t)host->nh_sysid);

	if (stat != RPC_SUCCESS) {
		NLM_WARN("Failed to contact local NSM, stat=%d\n", stat);
		mutex_enter(&g->lock);
		host->nh_flags &= ~NLM_NH_MONITORED;
		mutex_exit(&g->lock);

		return;
	}
}

int
nlm_host_get_sysid(struct nlm_host *hostp)
{
	return (hostp->nh_sysid);
}

int
nlm_host_get_state(struct nlm_host *hostp)
{

	return (hostp->nh_state);
}

/*
 * NLM client/server sleeping locks
 */

/*
 * Register client side sleeping lock.
 *
 * Our client code calls this to keep information
 * about sleeping lock somewhere. When it receives
 * grant callback from server or when it just
 * needs to remove all sleeping locks from vnode,
 * it uses this information for remove/apply lock
 * properly.
 */
struct nlm_slock *
nlm_slock_register(
	struct nlm_globals *g,
	struct nlm_host *host,
	struct nlm4_lock *lock,
	struct vnode *vp)
{
	struct nlm_owner_handle *oh;
	struct nlm_slock *nslp;

	ASSERT(lock->oh.n_len == sizeof (*oh));

	oh = (void *) lock->oh.n_bytes;
	nslp = kmem_zalloc(sizeof (*nslp), KM_SLEEP);
	cv_init(&nslp->nsl_cond, NULL, CV_DEFAULT, NULL);
	nslp->nsl_lock = *lock;
	nlm_copy_netobj(&nslp->nsl_fh, &nslp->nsl_lock.fh);
	nslp->nsl_state = NLM_SL_BLOCKED;
	nslp->nsl_host = host;
	nslp->nsl_vp = vp;

	mutex_enter(&g->lock);
	TAILQ_INSERT_TAIL(&g->nlm_slocks, nslp, nsl_link);
	mutex_exit(&g->lock);

	return (nslp);
}

/*
 * Remove this lock from the wait list and destroy it.
 */
void
nlm_slock_unregister(struct nlm_globals *g, struct nlm_slock *nslp)
{
	mutex_enter(&g->lock);
	TAILQ_REMOVE(&g->nlm_slocks, nslp, nsl_link);
	mutex_exit(&g->lock);

	kmem_free(nslp->nsl_fh.n_bytes, nslp->nsl_fh.n_len);
	cv_destroy(&nslp->nsl_cond);
	kmem_free(nslp, sizeof (*nslp));
}

/*
 * Wait for a granted callback or cancellation event
 * for a sleeping lock.
 *
 * If a signal interrupted the wait or if the lock
 * was cancelled, return EINTR - the caller must arrange to send
 * a cancellation to the server.
 *
 * If timeout occurred, return ETIMEDOUT - the caller must
 * resend the lock request to the server.
 *
 * On success return 0.
 */
int
nlm_slock_wait(struct nlm_globals *g,
    struct nlm_slock *nslp, uint_t timeo_secs)
{
	struct nlm_host *host = nslp->nsl_host;
	clock_t timeo_ticks;
	int cv_res, error;

	/*
	 * If the granted message arrived before we got here,
	 * nw->nw_state will be GRANTED - in that case, don't sleep.
	 */
	cv_res = 1;
	timeo_ticks = ddi_get_lbolt() + SEC_TO_TICK(timeo_secs);

	mutex_enter(&g->lock);
	if (nslp->nsl_state == NLM_SL_BLOCKED) {
		cv_res = cv_timedwait_sig(&nslp->nsl_cond,
		    &g->lock, timeo_ticks);
	}

	/*
	 * No matter why we wake up, if the lock was
	 * cancelled, let the function caller to know
	 * about it by returning EINTR.
	 */
	if (nslp->nsl_state == NLM_SL_CANCELLED) {
		error = EINTR;
		goto out;
	}

	if (cv_res <= 0) {
		/* We was woken up either by timeout or interrupt */
		error = (cv_res < 0) ? ETIMEDOUT : EINTR;

		/*
		 * The granted message may arrive after the
		 * interrupt/timeout but before we manage to lock the
		 * mutex. Detect this by examining nslp.
		 */
		if (nslp->nsl_state == NLM_SL_GRANTED)
			error = 0;
	} else { /* awaken via cv_signal or didn't block */
		error = 0;
		VERIFY(nslp->nsl_state == NLM_SL_GRANTED);
	}

out:
	mutex_exit(&g->lock);
	return (error);
}

/*
 * Mark client side sleeping lock as granted
 * and wake up a process blocked on the lock.
 * Called from server side NLM_GRANT handler.
 *
 * If sleeping lock is found return 0, otherwise
 * return ENOENT.
 */
int
nlm_slock_grant(struct nlm_globals *g,
    struct nlm_host *hostp, struct nlm4_lock *alock)
{
	struct nlm_slock *nslp;
	int error = ENOENT;

	mutex_enter(&g->lock);
	TAILQ_FOREACH(nslp, &g->nlm_slocks, nsl_link) {
		if ((nslp->nsl_state != NLM_SL_BLOCKED) ||
		    (nslp->nsl_host != hostp))
			continue;

		if (alock->svid		== nslp->nsl_lock.svid &&
		    alock->l_offset	== nslp->nsl_lock.l_offset &&
		    alock->l_len	== nslp->nsl_lock.l_len &&
		    alock->fh.n_len	== nslp->nsl_lock.fh.n_len &&
		    memcmp(alock->fh.n_bytes, nslp->nsl_lock.fh.n_bytes,
		        nslp->nsl_lock.fh.n_len) == 0) {
			nslp->nsl_state = NLM_SL_GRANTED;
			cv_broadcast(&nslp->nsl_cond);
			error = 0;
			break;
		}
	}

	mutex_exit(&g->lock);
	return (error);
}

/*
 * Register sleeping lock request corresponding to
 * flp on the given vhold object.
 * On success function returns 0, otherwise (if
 * lock request with the same flp is already
 * registered) function returns -1.
 */
int
nlm_slreq_register(struct nlm_host *hostp, struct nlm_vhold *nvp,
	struct flock64 *flp)
{
	struct nlm_slreq *slr, *new_slr = NULL;
	int ret = -1;

	mutex_enter(&hostp->nh_lock);
	slr = nlm_slreq_find_locked(hostp, nvp, flp);
	if (slr != NULL)
		goto out;

	mutex_exit(&hostp->nh_lock);
	new_slr = kmem_zalloc(sizeof (*slr), KM_SLEEP);
	bcopy(flp, &new_slr->nsr_fl, sizeof (*flp));

	mutex_enter(&hostp->nh_lock);
	slr = nlm_slreq_find_locked(hostp, nvp, flp);
	if (slr == NULL) {
		slr = new_slr;
		new_slr = NULL;
		ret = 0;

		TAILQ_INSERT_TAIL(&nvp->nv_slreqs, slr, nsr_link);
	}

out:
	mutex_exit(&hostp->nh_lock);
	if (new_slr != NULL)
		kmem_free(new_slr, sizeof (*new_slr));

	return (ret);
}

/*
 * Unregister sleeping lock request corresponding
 * to flp from the given vhold object.
 * On success function returns 0, otherwise (if
 * lock request corresponding to flp isn't found
 * on the given vhold) function returns -1.
 */
int
nlm_slreq_unregister(struct nlm_host *hostp, struct nlm_vhold *nvp,
	struct flock64 *flp)
{
	struct nlm_slreq *slr;

	mutex_enter(&hostp->nh_lock);
	slr = nlm_slreq_find_locked(hostp, nvp, flp);
	if (slr == NULL) {
		mutex_exit(&hostp->nh_lock);
		return (-1);
	}

	TAILQ_REMOVE(&nvp->nv_slreqs, slr, nsr_link);
	mutex_exit(&hostp->nh_lock);

	kmem_free(slr, sizeof (*slr));
	return (0);
}

/*
 * Find sleeping lock request on the given vhold object by flp.
 */
struct nlm_slreq *
nlm_slreq_find_locked(struct nlm_host *hostp, struct nlm_vhold *nvp,
    struct flock64 *flp)
{
	struct nlm_slreq *slr = NULL;

	ASSERT(MUTEX_HELD(&hostp->nh_lock));
	TAILQ_FOREACH(slr, &nvp->nv_slreqs, nsr_link) {
		if (slr->nsr_fl.l_start		== flp->l_start	&&
		    slr->nsr_fl.l_len		== flp->l_len	&&
		    slr->nsr_fl.l_pid		== flp->l_pid	&&
		    slr->nsr_fl.l_type		== flp->l_type)
			break;
	}

	return (slr);
}

/*
 * Called by klmmod.c when lockd adds a network endpoint
 * on which we should begin RPC services.
 */
int
nlm_svc_add_ep(struct nlm_globals *g, struct file *fp,
    const char *netid, struct knetconfig *knc)
{
	SVC_CALLOUT_TABLE *sct;
	SVCMASTERXPRT *xprt = NULL;

	if (0 == strcmp(knc->knc_protofmly, NC_LOOPBACK))
		sct = &nlm_sct_lo;
	else
		sct = &nlm_sct_in;

	return (svc_tli_kcreate(fp, 0, (char *)netid, NULL, &xprt,
	    sct, NULL, NLM_SVCPOOL_ID, FALSE));
}

/*
 * Start NLM service.
 */
int
nlm_svc_starting(struct nlm_globals *g, struct file *fp,
    const char *netid, struct knetconfig *knc)
{
	clock_t time_uptime;
	int error;
	enum clnt_stat stat;

	VERIFY(g->run_status == NLM_ST_STARTING);
	VERIFY(g->nlm_gc_thread == NULL);

	bzero(&g->nlm_nsm, sizeof (g->nlm_nsm));
	error = nlm_nsm_init(&g->nlm_nsm);
	if (error != 0) {
		NLM_ERR("Failed to initialize NSM handler "
		    "(error=%d)\n", error);
		g->run_status = NLM_ST_DOWN;
		return (error);
	}

	error = EIO;

	/*
	 * Create an NLM garbage collector thread that will
	 * clean up stale vholds and hosts objects.
	 */
	g->nlm_gc_thread = zthread_create(NULL, 0, nlm_gc,
	    g, 0, minclsyspri);

	/*
	 * Send SIMU_CRASH to local statd to report that
	 * NLM started, so that statd can report other hosts
	 * about NLM state change.
	 */

	stat = nlm_nsm_simu_crash(&g->nlm_nsm);
	if (stat != RPC_SUCCESS) {
		NLM_ERR("Failed to connect to local statd "
		    "(rpcerr=%d)\n", stat);
		goto shutdown_lm;
	}

	stat = nlm_nsm_stat(&g->nlm_nsm, &g->nsm_state);
	if (stat != RPC_SUCCESS) {
		NLM_ERR("Failed to get the status of local statd "
		    "(rpcerr=%d)\n", stat);
		goto shutdown_lm;
	}

	g->grace_threshold = ddi_get_lbolt() +
	    SEC_TO_TICK(g->grace_period);
	g->run_status = NLM_ST_UP;

	/* Register endpoint used for communications with local NLM */
	error = nlm_svc_add_ep(g, fp, netid, knc);
	if (error != 0)
		goto shutdown_lm;

	return (0);

shutdown_lm:
	mutex_enter(&g->lock);
	g->run_status = NLM_ST_STOPPING;
	mutex_exit(&g->lock);

	nlm_svc_stopping(g);
	return (error);
}

/*
 * Stop NLM service, cleanup all resources
 * NLM owns at the moment.
 *
 * NOTE: NFS code can call NLM while it's
 * stopping or even if it's shut down. Any attemp
 * to lock file either on client or on the server
 * will fail if NLM isn't in NLM_ST_UP state.
 */
void
nlm_svc_stopping(struct nlm_globals *g)
{
	ASSERT(g->run_status == NLM_ST_STOPPING);
	mutex_enter(&g->lock);

	/*
	 * Ask NLM GC thread to exit and wait until it do dies.
	 */
	cv_signal(&g->nlm_gc_sched_cv);
	while (g->nlm_gc_thread != NULL)
		cv_wait(&g->nlm_gc_finish_cv, &g->lock);

	mutex_exit(&g->lock);

	/*
	 * Cleanup locks owned by NLM hosts.
	 * NOTE: New hosts won't be created while
	 * NLM is topping.
	 */
	while (!avl_is_empty(&g->nlm_hosts_tree)) {
		struct nlm_host *hostp;
		int busy_hosts = 0;

		/*
		 * Iterate through all NLM hosts in the system
		 * and drop the locks they own by force.
		 */
		hostp = avl_first(&g->nlm_hosts_tree);
		while (hostp != NULL) {
			/* Cleanup all client and server side locks */
			nlm_client_cancel_all(g, hostp);
			nlm_host_notify_server(hostp, 0);

			mutex_enter(&hostp->nh_lock);
			nlm_host_gc_vholds(hostp);
			if (hostp->nh_refs > 0 || nlm_host_has_locks(hostp)) {
				/*
				 * Oh, it seems the host is still busy, let
				 * it some time to release and go to the
				 * next one.
				 */

				mutex_exit(&hostp->nh_lock);
				hostp = AVL_NEXT(&g->nlm_hosts_tree, hostp);
				busy_hosts++;
				continue;
			}

			mutex_exit(&hostp->nh_lock);
			hostp = AVL_NEXT(&g->nlm_hosts_tree, hostp);
		}

		/*
		 * All hosts go to nlm_idle_hosts list after
		 * all locks they own are cleaned up and last refereces
		 * were dropped. Just destroy all hosts in nlm_idle_hosts
		 * list, they can not be removed from there while we're
		 * in stopping state.
		 */
		while ((hostp = TAILQ_FIRST(&g->nlm_idle_hosts)) != NULL) {
			nlm_host_unregister(g, hostp);
			nlm_host_destroy(hostp);
		}

		if (busy_hosts > 0) {
			/*
			 * There're some hosts that weren't cleaned
			 * up. Probably they're in resource cleanup
			 * process. Give them some time to do drop
			 * references.
			 */
			delay(MSEC_TO_TICK(500));
		}
	}

	ASSERT(TAILQ_EMPTY(&g->nlm_slocks));

	(void) nlm_nsm_unmon_all(&g->nlm_nsm);
	nlm_nsm_fini(&g->nlm_nsm);
	g->lockd_pid = 0;
	g->run_status = NLM_ST_DOWN;
}
