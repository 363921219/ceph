// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2014 Red Hat
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef SCRUBSTACK_H_
#define SCRUBSTACK_H_

#include "CDir.h"
#include "CDentry.h"
#include "CInode.h"
#include "MDSContext.h"

#include "include/elist.h"

class MDCache;
class Finisher;

/**
 * Externally input parameters for a scrub, associated with the root
 * of where we are doing a recursive scrub
 */
class ScrubHeader {
public:
  std::string tag;
  CDentry *origin;
};

class ScrubStack {
  protected:
  /// A finisher needed so that we don't re-enter kick_off_scrubs
  Finisher *finisher;

  /// The stack of dentries we want to scrub
  elist<CDentry*> dentry_stack;
  /// current number of dentries we're actually scrubbing
  int scrubs_in_progress;
  ScrubStack *scrubstack; // hack for dout
  int stack_size;
public:
  MDCache *mdcache;
  ScrubStack(MDCache *mdc, Finisher *finisher_) :
    finisher(finisher_),
    dentry_stack(member_offset(CDentry, item_scrub)),
    scrubs_in_progress(0),
    scrubstack(this),
    stack_size(0),
    mdcache(mdc) {}
  ~ScrubStack() {
    assert(dentry_stack.empty());
    assert(!scrubs_in_progress);
  }
  /**
   * Put a dentry on the top of the scrub stack, so it is the highest priority.
   * If there are other scrubs in progress, they will not continue scrubbing new
   * entries until this one is completed.
   * @param dn The dentry to scrub
   * @param recursive True if we want to recursively scrub the
   * entire hierarchy under dn.
   * @param children True if we want to scrub the direct children of
   * dn but aren't doing a recursive scrub. (Otherwise, all checks are
   * local to dn's disk state.)
   * @param tag If non-empty, tag applied to each verified rados object
   */
  void enqueue_dentry_top(CDentry *dn, bool recursive, bool children,
                          const std::string &tag,
                          MDSInternalContextBase *on_finish) {
    enqueue_dentry(dn, recursive, children, tag, on_finish, true);
  }
  /** Like enqueue_dentry_top, but we wait for all pending scrubs before
   * starting this one.
   */
  void enqueue_dentry_bottom(CDentry *dn, bool recursive, bool children,
                             const std::string &tag,
                             MDSInternalContextBase *on_finish) {
    enqueue_dentry(dn, recursive, children, tag, on_finish, false);
  }

private:
  /**
   * Put the dentry at either the top or bottom of the stack, with
   * the given scrub params, and then try and kick off more scrubbing.
   */
  void enqueue_dentry(CDentry *dn, bool recursive, bool children,
                      const std::string &tag,
                      MDSInternalContextBase *on_finish, bool top);
  void _enqueue_dentry(CDentry *dn, CDir *parent, bool recursive, bool children,
                      const std::string &tag,
                       MDSInternalContextBase *on_finish, bool top);
  /**
   * Kick off as many scrubs as are appropriate, based on the current
   * state of the stack.
   */
  void kick_off_scrubs();
  /**
   * Push a dentry on top of the stack.
   */
  inline void push_dentry(CDentry *dentry);
  /**
   * Push a dentry to the bottom of the stack.
   */
  inline void push_dentry_bottom(CDentry *dentry);
  /**
   * Pop the given dentry off the stack.
   */
  inline void pop_dentry(CDentry *dn);

  /**
   * Scrub a file-representing dentry.
   * @param dn The dentry to scrub
   * @param progress Out pointer to a bool, which will be set to true.
   * @pre dn->get_projected_inode()->is_file()==true;
   */
  void scrub_file_dentry(CDentry *dn);
  /**
   * Make progress on scrubbing a directory-representing dirfrag and
   * its children..
   *
   * 1) Select the next dirfrag which hasn't been scrubbed, and make progress
   * on it if possible.
   *
   * 2) If not, move on to the next dirfrag and start it up, if any.
   *
   * 3) If waiting for results from dirfrag scrubs, do nothing.
   *
   * 4) If all dirfrags have been scrubbed, scrub my inode.
   *
   * @param dn The CDentry to scrub as a directory
   * @param added_dentries set to true if we pushed some of our children
   * onto the ScrubStack
   * @param is_terminal set to true if there are no descendant dentries
   * remaining to start scrubbing.
   * @param done set to true if we and all our children have finished scrubbing
   */
  void scrub_dir_dentry(CDentry *dn, bool *added_children, bool *is_terminal,
                        bool *done);
  /**
   * Make progress on scrubbing a dirfrag. It may return after each of the
   * following steps, but will report making progress on each one.
   *
   * 1) enqueues the next unscrubbed child directory dentry at the
   * top of the stack.
   *
   * 2) Initiates a scrub on the next unscrubbed file dentry
   *
   * If there are scrubs currently in progress on child dentries, no more child
   * dentries to scrub, and this function is invoked, it will report no
   * progress. Try again later.
   *
   */
  void scrub_dirfrag(CDir *dir, bool *added_children, bool *is_terminal,
		     bool *done);
  /**
   * Scrub a directory-representing dentry.
   *
   * @param dn The CDentry of the directory we're doing final scrub on.
   * @param done Set to true if the dentry scrub is finished. (That
   * won't every actually happen, right? It needs disk accesses? I forget.)
   */
  void scrub_dir_dentry_final(CDentry *dn, bool *done);

  /**
   * Get a CDir into memory, and return it if it's already complete.
   * Otherwise, fetch it and kick off scrubbing when done.
   *
   * @param in The Inode to get the next directory from
   * @param new_dir The CDir we're returning to you. NULL if
   * not ready yet or there aren't any.
   * @returns false if you have to wait, true if there's no work
   * left to do (we returned it, or there are none left in this inode).
   */
  bool get_next_cdir(CInode *in, CDir **new_dir);

  class C_KickOffScrubs : public MDSInternalContext {
    ScrubStack *stack;
  public:
    C_KickOffScrubs(MDS *mds, ScrubStack *s)
      : MDSInternalContext(mds), stack(s)
    {
    }
    void finish(int r){
      stack->kick_off_scrubs();
    }
  };
};

#endif /* SCRUBSTACK_H_ */
