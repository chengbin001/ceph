// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#ifndef CEPH_OBJECTER_H
#define CEPH_OBJECTER_H

#include "include/types.h"
#include "include/buffer.h"
#include "include/xlist.h"

#include "osd/OSDMap.h"
#include "messages/MOSDOp.h"

#include "common/Timer.h"

#include <list>
#include <map>
#include <memory>
#include <ext/hash_map>
using namespace std;
using namespace __gnu_cxx;

class Context;
class Messenger;
class OSDMap;
class MonClient;
class Message;

class MPoolOpReply;

class MGetPoolStatsReply;
class MStatfsReply;

// -----------------------------------------

struct ObjectOperation {
  vector<OSDOp> ops;
  int flags;
  int priority;
  bool linger;

  ObjectOperation() : flags(0), priority(0), linger(false) {}

  void add_op(int op) {
    int s = ops.size();
    ops.resize(s+1);
    ops[s].op.op = op;
  }
  void add_data(int op, uint64_t off, uint64_t len, bufferlist& bl) {
    int s = ops.size();
    ops.resize(s+1);
    ops[s].op.op = op;
    ops[s].op.extent.offset = off;
    ops[s].op.extent.length = len;
    ops[s].data.claim_append(bl);
  }
  void add_xattr(int op, const char *name, const bufferlist& data) {
    int s = ops.size();
    ops.resize(s+1);
    ops[s].op.op = op;
    ops[s].op.xattr.name_len = (name ? strlen(name) : 0);
    ops[s].op.xattr.value_len = data.length();
    if (name)
      ops[s].data.append(name);
    ops[s].data.append(data);
  }
  void add_call(int op, const char *cname, const char *method, bufferlist &indata) {
    int s = ops.size();
    ops.resize(s+1);
    ops[s].op.op = op;
    ops[s].op.cls.class_len = strlen(cname);
    ops[s].op.cls.method_len = strlen(method);
    ops[s].op.cls.indata_len = indata.length();
    ops[s].data.append(cname, ops[s].op.cls.class_len);
    ops[s].data.append(method, ops[s].op.cls.method_len);
    ops[s].data.append(indata);
  }
  void add_watch(int op, uint64_t cookie, uint64_t ver, uint8_t flag) {
    int s = ops.size();
    ops.resize(s+1);
    ops[s].op.op = op;
    ops[s].op.watch.cookie = cookie;
    ops[s].op.watch.ver = ver;
    ops[s].op.watch.flag = flag;
  }
  void add_pgls(int op, uint64_t count, uint64_t cookie) {
    int s = ops.size();
    ops.resize(s+1);
    ops[s].op.op = op;
    ops[s].op.pgls.count = count;
    ops[s].op.pgls.cookie = cookie;
  }
  void set_linger(bool l) {
    linger = l;
  }

  // ------

  // pg
  void pg_ls(uint64_t count, uint64_t cookie) {
    add_pgls(CEPH_OSD_OP_PGLS, count, cookie);
    flags |= CEPH_OSD_FLAG_PGOP;
  }

  // object data
  void read(uint64_t off, uint64_t len) {
    bufferlist bl;
    add_data(CEPH_OSD_OP_READ, off, len, bl);
  }
  void write(uint64_t off, uint64_t len, bufferlist& bl) {
    add_data(CEPH_OSD_OP_WRITE, off, len, bl);
  }
  void write_full(bufferlist& bl) {
    add_data(CEPH_OSD_OP_WRITEFULL, 0, bl.length(), bl);
  }
  void zero(uint64_t off, uint64_t len) {
    bufferlist bl;
    add_data(CEPH_OSD_OP_ZERO, off, len, bl);
  }
  void remove() {
    bufferlist bl;
    add_data(CEPH_OSD_OP_DELETE, 0, 0, bl);
  }
  void mapext(uint64_t off, uint64_t len) {
    bufferlist bl;
    add_data(CEPH_OSD_OP_MAPEXT, off, len, bl);
  }
  void sparse_read(uint64_t off, uint64_t len) {
    bufferlist bl;
    add_data(CEPH_OSD_OP_SPARSE_READ, off, len, bl);
  }

  // object attrs
  void getxattr(const char *name) {
    bufferlist bl;
    add_xattr(CEPH_OSD_OP_GETXATTR, name, bl);
  }
  void getxattrs() {
    bufferlist bl;
    add_xattr(CEPH_OSD_OP_GETXATTRS, 0, bl);
  }
  void setxattr(const char *name, const bufferlist& bl) {
    add_xattr(CEPH_OSD_OP_SETXATTR, name, bl);
  }
  void setxattr(const char *name, const string& s) {
    bufferlist bl;
    bl.append(s);
    add_xattr(CEPH_OSD_OP_SETXATTR, name, bl);
  }
  void rmxattr(const char *name) {
    bufferlist bl;
    add_xattr(CEPH_OSD_OP_RMXATTR, name, bl);
  }
  void setxattrs(map<string, bufferlist>& attrs) {
    bufferlist bl;
    ::encode(attrs, bl);
    add_xattr(CEPH_OSD_OP_RESETXATTRS, 0, bl.length());
  }
  void resetxattrs(const char *prefix, map<string, bufferlist>& attrs) {
    bufferlist bl;
    ::encode(attrs, bl);
    add_xattr(CEPH_OSD_OP_RESETXATTRS, prefix, bl);
  }
  
  // trivialmap
  void tmap_update(bufferlist& bl) {
    add_data(CEPH_OSD_OP_TMAPUP, 0, 0, bl);
  }
  void tmap_put(bufferlist& bl) {
    add_data(CEPH_OSD_OP_TMAPPUT, 0, bl.length(), bl);
  }
  void tmap_get() {
    add_op(CEPH_OSD_OP_TMAPGET);
  }

  // object classes
  void call(const char *cname, const char *method, bufferlist &indata) {
    add_call(CEPH_OSD_OP_CALL, cname, method, indata);
  }

  // watch/notify
  void watch(uint64_t cookie, uint64_t ver, bool set) {
    add_watch(CEPH_OSD_OP_WATCH, cookie, ver, (set ? 1 : 0));
  }

  void notify(uint64_t cookie, uint64_t ver) {
    add_watch(CEPH_OSD_OP_NOTIFY, cookie, ver, 1); 
  }

  void notify_ack(uint64_t notify_id, uint64_t ver) {
    add_watch(CEPH_OSD_OP_NOTIFY_ACK, notify_id, ver, 0);
  }

  void assert_version(uint64_t ver) {
    add_watch(CEPH_OSD_OP_ASSERT_VER, 0, ver, 0);
  }
};


// ----------------


class Objecter {
 public:  
  Messenger *messenger;
  MonClient *monc;
  OSDMap    *osdmap;

 
 private:
  tid_t last_tid;
  int client_inc;
  int num_unacked;
  int num_uncommitted;
  bool keep_balanced_budget;
  bool honor_osdmap_full;

  void maybe_request_map();

  version_t last_seen_osdmap_version;
  version_t last_seen_pgmap_version;

  Mutex &client_lock;
  SafeTimer &timer;
  
  class C_Tick : public Context {
    Objecter *ob;
  public:
    C_Tick(Objecter *o) : ob(o) {}
    void finish(int r) { ob->tick(); }
  };
  void tick();

public:
  /*** track pending operations ***/
  // read
 public:

  struct Op {
    xlist<Op*>::item session_item;

    object_t oid;
    object_locator_t oloc;
    pg_t pgid;

    Connection *con;

    vector<OSDOp> ops;

    snapid_t snapid;
    SnapContext snapc;
    utime_t mtime;

    bufferlist *outbl;

    int flags, priority;
    Context *onack, *oncommit;

    tid_t tid;
    eversion_t version;        // for op replay
    int attempts;

    bool paused;

    eversion_t *objver;

    bool linger;

    Op(const object_t& o, const object_locator_t& ol, vector<OSDOp>& op,
       int f, Context *ac, Context *co, eversion_t *ov, bool ln) :
      session_item(this),
      oid(o), oloc(ol),
      con(NULL),
      snapid(CEPH_NOSNAP), outbl(0), flags(f), priority(0), onack(ac), oncommit(co), 
      tid(0), attempts(0),
      paused(false), objver(ov), linger(ln) {
      ops.swap(op);
    }
  };

  struct C_Stat : public Context {
    bufferlist bl;
    uint64_t *psize;
    utime_t *pmtime;
    Context *fin;
    C_Stat(uint64_t *ps, utime_t *pm, Context *c) :
      psize(ps), pmtime(pm), fin(c) {}
    void finish(int r) {
      if (r >= 0) {
	bufferlist::iterator p = bl.begin();
	uint64_t s;
	utime_t m;
	::decode(s, p);
	::decode(m, p);
	if (psize)
	  *psize = s;
	if (pmtime)
	  *pmtime = m;
      }
      fin->finish(r);
      delete fin;
    }
  };

  struct C_GetAttrs : public Context {
    bufferlist bl;
    map<string,bufferlist>& attrset;
    Context *fin;
    C_GetAttrs(map<string, bufferlist>& set, Context *c) : attrset(set), fin(c) {}
    void finish(int r) {
      if (r >= 0) {
	bufferlist::iterator p = bl.begin();
	::decode(attrset, p);
      }
      fin->finish(r);
      delete fin;
    }
  };


  // Pools and statistics 
  struct ListContext {
    int current_pg;
    uint64_t cookie;
    int starting_pg_num;
    bool at_end;

    int pool_id;
    int pool_snap_seq;
    int max_entries;
    std::list<object_t> list;

    ListContext() : current_pg(0), cookie(0), starting_pg_num(0),
		    at_end(false), pool_id(0),
		    pool_snap_seq(0), max_entries(0) {}
  };

  struct C_List : public Context {
    ListContext *list_context;
    Context *final_finish;
    bufferlist *bl;
    Objecter *objecter;
    C_List(ListContext *lc, Context * finish, bufferlist *b, Objecter *ob) :
      list_context(lc), final_finish(finish), bl(b), objecter(ob) {}
    void finish(int r) {
      if (r >= 0) {
        objecter->_list_reply(list_context, bl, final_finish);
      } else {
        final_finish->finish(r);
        delete final_finish;
      }
    }
  };
  
  struct PoolStatOp {
    tid_t tid;
    list<string> pools;

    map<string,pool_stat_t> *pool_stats;
    Context *onfinish;

    utime_t last_submit;
  };

  struct StatfsOp {
    tid_t tid;
    struct ceph_statfs *stats;
    Context *onfinish;

    utime_t last_submit;
  };

  struct PoolOp {
    tid_t tid;
    int pool;
    string name;
    Context *onfinish;
    int pool_op;
    uint64_t auid;
    __u8 crush_rule;
    snapid_t snapid;
    bufferlist *blp;

    utime_t last_submit;
    PoolOp() : tid(0), pool(0), onfinish(0), pool_op(0),
	       auid(0), crush_rule(0), snapid(0), blp(NULL) {}
  };



  // -- osd sessions --
  struct Session {
    xlist<Op*> ops;
    int osd;
    int incarnation;
  };
  map<int,Session*> sessions;


 private:
  // pending ops
  hash_map<tid_t,Op*>       op_osd;
  hash_map<tid_t,Op*>       op_osd_linger;
  map<tid_t,PoolStatOp*>    op_poolstat;
  map<tid_t,StatfsOp*>      op_statfs;
  map<tid_t,PoolOp*>        op_pool;

  map<epoch_t,list< pair<Context*, int> > > waiting_for_map;

  /**
   * track pending ops by pg
   *  ...so we can cope with failures, map changes
   */
  class PG {
  public:
    vector<int> acting;
    set<tid_t>  active_tids; // active ops
    set<tid_t>  linger_tids; // active ops
    utime_t last;
    epoch_t epoch; // generation epoch

    PG() {}
    
    // primary - where i write
    int primary() {
      if (acting.empty()) return -1;
      return acting[0];
    }
    // acker - where i read, and receive acks from
    int acker() {
      if (acting.empty()) return -1;
      return acting[0];
    }
  };

  hash_map<pg_t,PG> pg_map;
  
  
  PG &get_pg(pg_t pgid);
  void close_pg(pg_t pgid) {
    assert(pg_map.count(pgid));
    assert(pg_map[pgid].active_tids.empty());
    pg_map.erase(pgid);
  }
  void scan_pgs(set<pg_t>& changed_pgs);
  void scan_pgs_for(set<pg_t>& changed_pgs, int osd);
  void kick_requests(set<pg_t>& changed_pgs);

  void _list_reply(ListContext *list_context, bufferlist *bl, Context *final_finish);

  void resend_mon_ops();

  /**
   * handle a budget for in-flight ops
   * budget is taken whenever an op goes into the op_osd map
   * and returned whenever an op is removed from the map
   * If throttle_op needs to throttle it will unlock client_lock.
   */
  int calc_op_budget(Op *op);
  void throttle_op(Op *op, int op_size=0);
  void take_op_budget(Op *op) {
    int op_budget = calc_op_budget(op);
    if (keep_balanced_budget)
      throttle_op(op, op_budget);
    else
      op_throttler.take(op_budget);
  }
  void put_op_budget(Op *op) {
    int op_budget = calc_op_budget(op);
    op_throttler.put(op_budget);
  }
  Throttle op_throttler;

 public:
  Objecter(Messenger *m, MonClient *mc, OSDMap *om, Mutex& l, SafeTimer& t) : 
    messenger(m), monc(mc), osdmap(om),
    last_tid(0), client_inc(-1),
    num_unacked(0), num_uncommitted(0),
    keep_balanced_budget(false), honor_osdmap_full(true),
    last_seen_osdmap_version(0),
    last_seen_pgmap_version(0),
    client_lock(l), timer(t),
    op_throttler(g_conf.objecter_inflight_op_bytes)
  { }
  ~Objecter() { }

  void init();
  void shutdown();

  /**
   * Tell the objecter to throttle outgoing ops according to its
   * budget (in g_conf). If you do this, ops can block, in
   * which case it will unlock client_lock and sleep until
   * incoming messages reduce the used budget low enough for
   * the ops to continue going; then it will lock client_lock again.
   */
  void set_balanced_budget() { keep_balanced_budget = true; }
  void unset_balanced_budget() { keep_balanced_budget = false; }

  void set_honor_osdmap_full() { honor_osdmap_full = true; }
  void unset_honor_osdmap_full() { honor_osdmap_full = false; }

  // messages
 public:
  void dispatch(Message *m);
  void handle_osd_op_reply(class MOSDOpReply *m);
  void handle_osd_map(class MOSDMap *m);
  void wait_for_osd_map();

private:
  // low-level
  tid_t op_submit(Op *op, bool register_linger = true);

  // public interface
 public:
  bool is_active() {
    return !(op_osd.empty() && op_poolstat.empty() && op_statfs.empty());
  }
  void dump_active();

  int get_client_incarnation() const { return client_inc; }
  void set_client_incarnation(int inc) { client_inc = inc; }

  void wait_for_new_map(Context *c, epoch_t epoch, int replyCode=0) {
    maybe_request_map();
    waiting_for_map[epoch].push_back(pair<Context *, int>(c, replyCode));
  }

  // mid-level helpers
  tid_t mutate(const object_t& oid, const object_locator_t& oloc, 
	       ObjectOperation& op,
	       const SnapContext& snapc, utime_t mtime, int flags,
	       Context *onack, Context *oncommit, eversion_t *objver = NULL) {
    Op *o = new Op(oid, oloc, op.ops, flags | CEPH_OSD_FLAG_WRITE, onack, oncommit, objver, op.linger);
    o->priority = op.priority;
    o->mtime = mtime;
    o->snapc = snapc;
    return op_submit(o);
  }
  tid_t read(const object_t& oid, const object_locator_t& oloc, 
	     ObjectOperation& op,
	     snapid_t snapid, bufferlist *pbl, int flags,
	     Context *onack, eversion_t *objver = NULL) {
    Op *o = new Op(oid, oloc, op.ops, flags | CEPH_OSD_FLAG_READ, onack, NULL, objver, op.linger);
    o->priority = op.priority;
    o->snapid = snapid;
    o->outbl = pbl;
    return op_submit(o);
  }

  int init_ops(vector<OSDOp>& ops, int ops_count, ObjectOperation *extra_ops, bool *plinger) {
    int i;
    bool linger = false;

    if (extra_ops)
      ops_count += extra_ops->ops.size();

    ops.resize(ops_count);

    for (i=0; i<ops_count - 1; i++) {
      ops[i] = extra_ops->ops[i];
      linger |= extra_ops->linger;
    }

    if (plinger)
      *plinger = linger;

    return i;
  }


  // high-level helpers
  tid_t stat(const object_t& oid, const object_locator_t& oloc, snapid_t snap,
	     uint64_t *psize, utime_t *pmtime, int flags, 
	     Context *onfinish,
	     eversion_t *objver = NULL, ObjectOperation *extra_ops = NULL) {
    vector<OSDOp> ops;
    bool linger;
    int i = init_ops(ops, 1, extra_ops, &linger);
    ops[i].op.op = CEPH_OSD_OP_STAT;
    C_Stat *fin = new C_Stat(psize, pmtime, onfinish);
    Op *o = new Op(oid, oloc, ops, flags | CEPH_OSD_FLAG_READ, fin, 0, objver, linger);
    o->snapid = snap;
    o->outbl = &fin->bl;
    return op_submit(o);
  }

  tid_t read(const object_t& oid, const object_locator_t& oloc, 
	     uint64_t off, uint64_t len, snapid_t snap, bufferlist *pbl, int flags,
	     Context *onfinish,
	     eversion_t *objver = NULL, ObjectOperation *extra_ops = NULL) {
    vector<OSDOp> ops;
    bool linger;
    int i = init_ops(ops, 1, extra_ops, &linger);
    ops[i].op.op = CEPH_OSD_OP_READ;
    ops[i].op.extent.offset = off;
    ops[i].op.extent.length = len;
    ops[i].op.extent.truncate_size = 0;
    ops[i].op.extent.truncate_seq = 0;
    Op *o = new Op(oid, oloc, ops, flags | CEPH_OSD_FLAG_READ, onfinish, 0, objver, linger);
    o->snapid = snap;
    o->outbl = pbl;
    return op_submit(o);
  }
  tid_t read_trunc(const object_t& oid, const object_locator_t& oloc, 
	     uint64_t off, uint64_t len, snapid_t snap, bufferlist *pbl, int flags,
	     uint64_t trunc_size, __u32 trunc_seq,
	     Context *onfinish,
	     eversion_t *objver = NULL, ObjectOperation *extra_ops = NULL) {
    vector<OSDOp> ops;
    bool linger;
    int i = init_ops(ops, 1, extra_ops, &linger);
    ops[i].op.op = CEPH_OSD_OP_READ;
    ops[i].op.extent.offset = off;
    ops[i].op.extent.length = len;
    ops[i].op.extent.truncate_size = trunc_size;
    ops[i].op.extent.truncate_seq = trunc_seq;
    Op *o = new Op(oid, oloc, ops, flags | CEPH_OSD_FLAG_READ, onfinish, 0, objver, linger);
    o->snapid = snap;
    o->outbl = pbl;
    return op_submit(o);
  }
  tid_t mapext(const object_t& oid, const object_locator_t& oloc,
	     uint64_t off, uint64_t len, snapid_t snap, bufferlist *pbl, int flags,
	     Context *onfinish,
	     eversion_t *objver = NULL, ObjectOperation *extra_ops = NULL) {
    vector<OSDOp> ops;
    bool linger;
    int i = init_ops(ops, 1, extra_ops, &linger);
    ops[i].op.op = CEPH_OSD_OP_MAPEXT;
    ops[i].op.extent.offset = off;
    ops[i].op.extent.length = len;
    ops[i].op.extent.truncate_size = 0;
    ops[i].op.extent.truncate_seq = 0;
    Op *o = new Op(oid, oloc, ops, flags | CEPH_OSD_FLAG_READ, onfinish, 0, objver, linger);
    o->snapid = snap;
    o->outbl = pbl;
    return op_submit(o);
  }
  tid_t sparse_read(const object_t& oid, const object_locator_t& oloc,
	     uint64_t off, uint64_t len, snapid_t snap, bufferlist *pbl, int flags,
	     Context *onfinish,
	     eversion_t *objver = NULL, ObjectOperation *extra_ops = NULL) {
    vector<OSDOp> ops;
    bool linger;
    int i = init_ops(ops, 1, extra_ops, &linger);
    ops[i].op.op = CEPH_OSD_OP_SPARSE_READ;
    ops[i].op.extent.offset = off;
    ops[i].op.extent.length = len;
    ops[i].op.extent.truncate_size = 0;
    ops[i].op.extent.truncate_seq = 0;
    Op *o = new Op(oid, oloc, ops, flags | CEPH_OSD_FLAG_READ, onfinish, 0, objver, linger);
    o->snapid = snap;
    o->outbl = pbl;
    return op_submit(o);
  }

  tid_t getxattr(const object_t& oid, const object_locator_t& oloc,
	     const char *name, snapid_t snap, bufferlist *pbl, int flags,
	     Context *onfinish,
	     eversion_t *objver = NULL, ObjectOperation *extra_ops = NULL) {
    vector<OSDOp> ops;
    bool linger;
    int i = init_ops(ops, 1, extra_ops, &linger);
    ops[i].op.op = CEPH_OSD_OP_GETXATTR;
    ops[i].op.xattr.name_len = (name ? strlen(name) : 0);
    ops[i].op.xattr.value_len = 0;
    if (name)
      ops[i].data.append(name);
    Op *o = new Op(oid, oloc, ops, flags | CEPH_OSD_FLAG_READ, onfinish, 0, objver, linger);
    o->snapid = snap;
    o->outbl = pbl;
    return op_submit(o);
  }

  tid_t getxattrs(const object_t& oid, const object_locator_t& oloc, snapid_t snap,
             map<string,bufferlist>& attrset,
	     int flags, Context *onfinish,
	     eversion_t *objver = NULL, ObjectOperation *extra_ops = NULL) {
    vector<OSDOp> ops;
    bool linger;
    int i = init_ops(ops, 1, extra_ops, &linger);
    ops[i].op.op = CEPH_OSD_OP_GETXATTRS;
    C_GetAttrs *fin = new C_GetAttrs(attrset, onfinish);
    Op *o = new Op(oid, oloc, ops, flags | CEPH_OSD_FLAG_READ, fin, 0, objver, linger);
    o->snapid = snap;
    o->outbl = &fin->bl;
    return op_submit(o);
  }

  tid_t read_full(const object_t& oid, const object_locator_t& oloc,
		  snapid_t snap, bufferlist *pbl, int flags,
		  Context *onfinish,
	          eversion_t *objver = NULL, ObjectOperation *extra_ops = NULL) {
    return read(oid, oloc, 0, 0, snap, pbl, flags | CEPH_OSD_FLAG_READ, onfinish, objver);
  }
     
  // writes
  tid_t _modify(const object_t& oid, const object_locator_t& oloc, 
		vector<OSDOp>& ops, utime_t mtime,
		const SnapContext& snapc, int flags,
	        Context *onack, Context *oncommit,
	        eversion_t *objver = NULL) {
    Op *o = new Op(oid, oloc, ops, flags | CEPH_OSD_FLAG_WRITE, onack, oncommit, objver, false);
    o->mtime = mtime;
    o->snapc = snapc;
    return op_submit(o);
  }
  tid_t write(const object_t& oid, const object_locator_t& oloc,
	      uint64_t off, uint64_t len, const SnapContext& snapc, const bufferlist &bl,
	      utime_t mtime, int flags,
	      Context *onack, Context *oncommit,
	      eversion_t *objver = NULL, ObjectOperation *extra_ops = NULL) {
    vector<OSDOp> ops;
    bool linger;
    int i = init_ops(ops, 1, extra_ops, &linger);
    ops[i].op.op = CEPH_OSD_OP_WRITE;
    ops[i].op.extent.offset = off;
    ops[i].op.extent.length = len;
    ops[i].op.extent.truncate_size = 0;
    ops[i].op.extent.truncate_seq = 0;
    ops[i].data = bl;
    Op *o = new Op(oid, oloc, ops, flags | CEPH_OSD_FLAG_WRITE, onack, oncommit, objver, linger);
    o->mtime = mtime;
    o->snapc = snapc;
    return op_submit(o);
  }
  tid_t write_trunc(const object_t& oid, const object_locator_t& oloc,
	      uint64_t off, uint64_t len, const SnapContext& snapc, const bufferlist &bl,
	      utime_t mtime, int flags,
	      uint64_t trunc_size, __u32 trunc_seq,
	      Context *onack, Context *oncommit,
	      eversion_t *objver = NULL, ObjectOperation *extra_ops = NULL) {
    vector<OSDOp> ops;
    bool linger;
    int i = init_ops(ops, 1, extra_ops, &linger);
    ops[i].op.op = CEPH_OSD_OP_WRITE;
    ops[i].op.extent.offset = off;
    ops[i].op.extent.length = len;
    ops[i].op.extent.truncate_size = trunc_size;
    ops[i].op.extent.truncate_seq = trunc_seq;
    ops[i].data = bl;
    Op *o = new Op(oid, oloc, ops, flags | CEPH_OSD_FLAG_WRITE, onack, oncommit, objver, linger);
    o->mtime = mtime;
    o->snapc = snapc;
    return op_submit(o);
  }
  tid_t write_full(const object_t& oid, const object_locator_t& oloc,
		   const SnapContext& snapc, const bufferlist &bl, utime_t mtime, int flags,
		   Context *onack, Context *oncommit,
		   eversion_t *objver = NULL, ObjectOperation *extra_ops = NULL) {
    vector<OSDOp> ops;
    bool linger;
    int i = init_ops(ops, 1, extra_ops, &linger);
    ops[i].op.op = CEPH_OSD_OP_WRITEFULL;
    ops[i].op.extent.offset = 0;
    ops[i].op.extent.length = bl.length();
    ops[i].data = bl;
    Op *o = new Op(oid, oloc, ops, flags | CEPH_OSD_FLAG_WRITE, onack, oncommit, objver, linger);
    o->mtime = mtime;
    o->snapc = snapc;
    return op_submit(o);
  }
  tid_t trunc(const object_t& oid, const object_locator_t& oloc,
	      const SnapContext& snapc,
	      utime_t mtime, int flags,
	      uint64_t trunc_size, __u32 trunc_seq,
              Context *onack, Context *oncommit,
	      eversion_t *objver = NULL, ObjectOperation *extra_ops = NULL) {
    vector<OSDOp> ops;
    bool linger;
    int i = init_ops(ops, 1, extra_ops, &linger);
    ops[i].op.op = CEPH_OSD_OP_TRUNCATE;
    ops[i].op.extent.offset = trunc_size;
    ops[i].op.extent.truncate_size = trunc_size;
    ops[i].op.extent.truncate_seq = trunc_seq;
    Op *o = new Op(oid, oloc, ops, flags | CEPH_OSD_FLAG_WRITE, onack, oncommit, objver, linger);
    o->mtime = mtime;
    o->snapc = snapc;
    return op_submit(o);
  }
  tid_t zero(const object_t& oid, const object_locator_t& oloc, 
	     uint64_t off, uint64_t len, const SnapContext& snapc, utime_t mtime, int flags,
             Context *onack, Context *oncommit,
	     eversion_t *objver = NULL, ObjectOperation *extra_ops = NULL) {
    vector<OSDOp> ops;
    bool linger;
    int i = init_ops(ops, 1, extra_ops, &linger);
    ops[i].op.op = CEPH_OSD_OP_ZERO;
    ops[i].op.extent.offset = off;
    ops[i].op.extent.length = len;
    Op *o = new Op(oid, oloc, ops, flags | CEPH_OSD_FLAG_WRITE, onack, oncommit, objver, linger);
    o->mtime = mtime;
    o->snapc = snapc;
    return op_submit(o);
  }
  tid_t rollback_object(const object_t& oid, const object_locator_t& oloc,
		 const SnapContext& snapc, snapid_t snapid,
		 utime_t mtime, Context *onack, Context *oncommit,
		 eversion_t *objver = NULL, ObjectOperation *extra_ops = NULL) {
    vector<OSDOp> ops;
    bool linger;
    int i = init_ops(ops, 1, extra_ops, &linger);
    ops[i].op.op = CEPH_OSD_OP_ROLLBACK;
    ops[i].op.snap.snapid = snapid;
    Op *o = new Op(oid, oloc, ops, CEPH_OSD_FLAG_WRITE, onack, oncommit, objver, linger);
    o->mtime = mtime;
    o->snapc = snapc;
    return op_submit(o);
  }
  tid_t create(const object_t& oid, const object_locator_t& oloc, 
	     const SnapContext& snapc, utime_t mtime,
             int global_flags, int create_flags,
             Context *onack, Context *oncommit,
             eversion_t *objver = NULL, ObjectOperation *extra_ops = NULL) {
    vector<OSDOp> ops;
    bool linger;
    int i = init_ops(ops, 1, extra_ops, &linger);
    ops[i].op.op = CEPH_OSD_OP_CREATE;
    ops[i].op.flags = create_flags;
    Op *o = new Op(oid, oloc, ops, global_flags | CEPH_OSD_FLAG_WRITE, onack, oncommit, objver, linger);
    o->mtime = mtime;
    o->snapc = snapc;
    return op_submit(o);
  }
  tid_t remove(const object_t& oid, const object_locator_t& oloc, 
	       const SnapContext& snapc, utime_t mtime, int flags,
	       Context *onack, Context *oncommit,
	       eversion_t *objver = NULL, ObjectOperation *extra_ops = NULL) {
    vector<OSDOp> ops;
    bool linger;
    int i = init_ops(ops, 1, extra_ops, &linger);
    ops[i].op.op = CEPH_OSD_OP_DELETE;
    Op *o = new Op(oid, oloc, ops, flags | CEPH_OSD_FLAG_WRITE, onack, oncommit, objver, linger);
    o->mtime = mtime;
    o->snapc = snapc;
    return op_submit(o);
  }

  tid_t lock(const object_t& oid, const object_locator_t& oloc, int op, int flags,
	     Context *onack, Context *oncommit, eversion_t *objver = NULL, ObjectOperation *extra_ops = NULL) {
    SnapContext snapc;  // no snapc for lock ops
    vector<OSDOp> ops;
    bool linger;
    int i = init_ops(ops, 1, extra_ops, &linger);
    ops[i].op.op = op;
    Op *o = new Op(oid, oloc, ops, flags | CEPH_OSD_FLAG_WRITE, onack, oncommit, objver, linger);
    o->snapc = snapc;
    return op_submit(o);
  }
  tid_t setxattr(const object_t& oid, const object_locator_t& oloc,
	      const char *name, const SnapContext& snapc, const bufferlist &bl,
	      utime_t mtime, int flags,
	      Context *onack, Context *oncommit,
	      eversion_t *objver = NULL, ObjectOperation *extra_ops = NULL) {
    vector<OSDOp> ops;
    bool linger;
    int i = init_ops(ops, 1, extra_ops, &linger);
    ops[i].op.op = CEPH_OSD_OP_SETXATTR;
    ops[i].op.xattr.name_len = (name ? strlen(name) : 0);
    ops[i].op.xattr.value_len = bl.length();
    if (name)
      ops[i].data.append(name);
    ops[i].data.append(bl);
    Op *o = new Op(oid, oloc, ops, flags | CEPH_OSD_FLAG_WRITE, onack, oncommit, objver, linger);
    o->mtime = mtime;
    o->snapc = snapc;
    return op_submit(o);
  }
  tid_t removexattr(const object_t& oid, const object_locator_t& oloc,
	      const char *name, const SnapContext& snapc,
	      utime_t mtime, int flags,
	      Context *onack, Context *oncommit,
	      eversion_t *objver = NULL, ObjectOperation *extra_ops = NULL) {
    vector<OSDOp> ops;
    bool linger;
    int i = init_ops(ops, 1, extra_ops, &linger);
    ops[i].op.op = CEPH_OSD_OP_RMXATTR;
    ops[i].op.xattr.name_len = (name ? strlen(name) : 0);
    ops[i].op.xattr.value_len = 0;
    if (name)
      ops[i].data.append(name);
    Op *o = new Op(oid, oloc, ops, flags | CEPH_OSD_FLAG_WRITE, onack, oncommit, objver, linger);
    o->mtime = mtime;
    o->snapc = snapc;
    return op_submit(o);
  }

  void list_objects(ListContext *p, Context *onfinish);

  // -------------------------
  // pool ops
private:
  void pool_op_submit(PoolOp *op);
public:
  int create_pool_snap(int pool, string& snapName, Context *onfinish);
  int allocate_selfmanaged_snap(int pool, snapid_t *psnapid, Context *onfinish);
  int delete_pool_snap(int pool, string& snapName, Context *onfinish);
  int delete_selfmanaged_snap(int pool, snapid_t snap, Context *onfinish);

  int create_pool(string& name, Context *onfinish, uint64_t auid=0,
		  int crush_rule=-1);
  int delete_pool(int pool, Context *onfinish);
  int change_pool_auid(int pool, Context *onfinish, uint64_t auid);

  void handle_pool_op_reply(MPoolOpReply *m);

  // --------------------------
  // pool stats
private:
  void poolstat_submit(PoolStatOp *op);
public:
  void handle_get_pool_stats_reply(MGetPoolStatsReply *m);
  void get_pool_stats(list<string>& pools, map<string,pool_stat_t> *result,
		      Context *onfinish);

  // ---------------------------
  // df stats
private:
  void fs_stats_submit(StatfsOp *op);
public:
  void handle_fs_stats_reply(MStatfsReply *m);
  void get_fs_stats(struct ceph_statfs& result, Context *onfinish);

  // ---------------------------
  // some scatter/gather hackery

  void _sg_read_finish(vector<ObjectExtent>& extents, vector<bufferlist>& resultbl, 
		       bufferlist *bl, Context *onfinish);

  struct C_SGRead : public Context {
    Objecter *objecter;
    vector<ObjectExtent> extents;
    vector<bufferlist> resultbl;
    bufferlist *bl;
    Context *onfinish;
    C_SGRead(Objecter *ob, 
	     vector<ObjectExtent>& e, vector<bufferlist>& r, bufferlist *b, Context *c) :
      objecter(ob), bl(b), onfinish(c) {
      extents.swap(e);
      resultbl.swap(r);
    }
    void finish(int r) {
      objecter->_sg_read_finish(extents, resultbl, bl, onfinish);
    }      
  };

  void sg_read_trunc(vector<ObjectExtent>& extents, snapid_t snap, bufferlist *bl, int flags,
		uint64_t trunc_size, __u32 trunc_seq, Context *onfinish) {
    if (extents.size() == 1) {
      read_trunc(extents[0].oid, extents[0].oloc, extents[0].offset, extents[0].length,
	   snap, bl, flags, trunc_size, trunc_seq, onfinish);
    } else {
      C_Gather *g = new C_Gather;
      vector<bufferlist> resultbl(extents.size());
      int i=0;
      for (vector<ObjectExtent>::iterator p = extents.begin(); p != extents.end(); p++) {
	read_trunc(p->oid, p->oloc, p->offset, p->length,
	     snap, &resultbl[i++], flags, trunc_size, trunc_seq, g->new_sub());
      }
      g->set_finisher(new C_SGRead(this, extents, resultbl, bl, onfinish));
    }
  }

  void sg_read(vector<ObjectExtent>& extents, snapid_t snap, bufferlist *bl, int flags, Context *onfinish) {
    sg_read_trunc(extents, snap, bl, flags, 0, 0, onfinish);
  }

  void sg_write_trunc(vector<ObjectExtent>& extents, const SnapContext& snapc, const bufferlist& bl, utime_t mtime,
		int flags, uint64_t trunc_size, __u32 trunc_seq,
		Context *onack, Context *oncommit) {
    if (extents.size() == 1) {
      write_trunc(extents[0].oid, extents[0].oloc, extents[0].offset, extents[0].length,
	    snapc, bl, mtime, flags, trunc_size, trunc_seq, onack, oncommit);
    } else {
      C_Gather *gack = 0, *gcom = 0;
      if (onack)
	gack = new C_Gather(onack);
      if (oncommit)
	gcom = new C_Gather(oncommit);
      for (vector<ObjectExtent>::iterator p = extents.begin(); p != extents.end(); p++) {
	bufferlist cur;
	for (map<__u32,__u32>::iterator bit = p->buffer_extents.begin();
	     bit != p->buffer_extents.end();
	     bit++)
	  bl.copy(bit->first, bit->second, cur);
	assert(cur.length() == p->length);
	write_trunc(p->oid, p->oloc, p->offset, p->length, 
	      snapc, cur, mtime, flags, trunc_size, trunc_seq,
	      gack ? gack->new_sub():0,
	      gcom ? gcom->new_sub():0);
      }
    }
  }

  void sg_write(vector<ObjectExtent>& extents, const SnapContext& snapc, const bufferlist& bl, utime_t mtime,
		int flags, Context *onack, Context *oncommit) {
    sg_write_trunc(extents, snapc, bl, mtime, flags, 0, 0, onack, oncommit);
  }

  void ms_handle_connect(Connection *con);
  void ms_handle_reset(Connection *con);
  void ms_handle_remote_reset(Connection *con);
};

#endif
