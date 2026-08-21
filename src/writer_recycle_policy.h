/* Copyright (C) 2026 selective_trace plugin authors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/*
  writer_recycle_policy — when should the TABLE writer recycle its
  internal SQL connection?

  *** WHY THIS EXISTS (read before changing the threshold) ***
  The TABLE writer keeps one internal connection (and, server-side, one
  THD) alive and reuses it for every queued INSERT. Under sustained
  high-volume tracing that long-lived THD's memory is not returned to the
  OS at a matching rate. It is NOT a leak in the pointer sense — the
  sibling MariaDB plugin confirmed with Valgrind (start -> 6000 inserts ->
  drain -> shutdown) that a full cycle reports 0 bytes lost and 0
  reachable. The memory is freed correctly, but stays interleaved with
  live blocks in the same long-lived arena, so the allocator never hands
  it back and RSS climbs without bound (~11-12 KB per event observed on
  MariaDB) until the OOM killer takes the server. malloc_trim() does not
  help, for the same fragmentation reason.

  The fix is to bound how much any single THD can accumulate: close the
  connection every N inserts and let the next insert reopen it, which
  destroys the old THD and its arena. This header holds only the "is it
  time?" counter so it can be unit-tested with no server headers at all
  (see src/test_writer_recycle.cc) — the actual close/reopen lives in
  log_writer_table_mysql.cc.

  Measured effect on the MariaDB sibling, same reproduction (1.5M events,
  concurrency 16): RSS went from 17.8 GB and still climbing, to ~500 MB
  and flat.
*/

#ifndef SELECTIVE_TRACE_WRITER_RECYCLE_POLICY_H
#define SELECTIVE_TRACE_WRITER_RECYCLE_POLICY_H

namespace selective_trace {

/*
  Default recycle interval, in inserts. Matches the sibling MariaDB
  plugin (v1.2.2) so both behave the same under load. Large enough that
  the reconnect cost is negligible (one reconnect per 20k inserts), small
  enough to bound a single THD's arena growth well under any sane memory
  budget.

  Overridable at build time purely so the old (unbounded) behavior can be
  reproduced for A/B memory measurements:
  -DSELECTIVE_TRACE_RECONNECT_EVERY=0 disables recycling entirely. Not a
  supported production setting — 0 reinstates the RSS growth described
  above.
*/
#ifndef SELECTIVE_TRACE_RECONNECT_EVERY
#define SELECTIVE_TRACE_RECONNECT_EVERY 20000
#endif

const unsigned long WRITER_RECONNECT_EVERY_N_INSERTS=
    SELECTIVE_TRACE_RECONNECT_EVERY;

/*
  Pure counter: the writer calls note_insert() once per completed insert
  (successful or not — a failed insert still ran on, and dirtied, the
  THD) and recycles the connection when it returns true.

  A threshold of 0 disables recycling entirely (note_insert() never
  returns true). That is an escape hatch for debugging/benchmarking the
  old behavior, not a supported production setting.
*/
class WriterRecyclePolicy
{
public:
  explicit WriterRecyclePolicy(
      unsigned long threshold= WRITER_RECONNECT_EVERY_N_INSERTS)
    : since_recycle_(0), recycles_(0), threshold_(threshold)
  {}

  /*
    Account for one completed insert. Returns true exactly when the
    caller should close the connection now (and lets the next insert
    reopen it lazily). The internal counter resets on every true, so
    recycles land on a fixed period, not drifting.
  */
  bool note_insert()
  {
    if (threshold_ == 0)                 /* recycling disabled */
      return false;
    if (++since_recycle_ < threshold_)
      return false;
    since_recycle_= 0;
    recycles_++;
    return true;
  }

  /* Inserts accumulated on the current connection. */
  unsigned long since_recycle() const { return since_recycle_; }

  /* Total recycles since the writer thread started — surfaced as the
     selective_trace_writer_reconnects status variable. */
  unsigned long recycles() const { return recycles_; }

  unsigned long threshold() const { return threshold_; }

  /*
    Called when the connection was dropped for a reason other than the
    periodic recycle (server closed it, error-path retry): the fresh
    connection starts with a fresh arena, so the counter restarts too.
    Does NOT count as a recycle — recycles() tracks only the periodic
    ones this policy triggered.
  */
  void note_connection_reset() { since_recycle_= 0; }

private:
  unsigned long since_recycle_;
  unsigned long recycles_;
  unsigned long threshold_;
};

} /* namespace selective_trace */

#endif /* SELECTIVE_TRACE_WRITER_RECYCLE_POLICY_H */
