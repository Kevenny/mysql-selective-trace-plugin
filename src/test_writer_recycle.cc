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
  Standalone unit tests for src/writer_recycle_policy.h — the TABLE
  writer's connection-recycling policy, which bounds the unbounded RSS
  growth described in that header.

  No test framework and no server headers on purpose — build and run:

      g++ -std=c++17 -Wall -Wextra -Werror \
          -I src src/test_writer_recycle.cc -o test_writer_recycle \
          && ./test_writer_recycle

  What these tests CAN cover: the period/boundary/reset semantics of the
  policy. What they CANNOT cover: that recycling actually bounds server
  RSS — that needs a live mysqld under sustained load, and is validated
  separately (see docs/RESEARCH_NOTES_MYSQL.md, Etapa 5).
*/

#include <cstdio>
#include "writer_recycle_policy.h"

using selective_trace::WriterRecyclePolicy;
using selective_trace::WRITER_RECONNECT_EVERY_N_INSERTS;

static int failures= 0;
static int checks= 0;

#define CHECK(cond)                                                     \
  do                                                                    \
  {                                                                     \
    checks++;                                                           \
    if (!(cond))                                                        \
    {                                                                   \
      failures++;                                                       \
      std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                                   \
  } while (0)

/* Run n inserts through p, returning how many recycles it asked for. */
static unsigned long run_inserts(WriterRecyclePolicy *p, unsigned long n)
{
  unsigned long recycles= 0;
  for (unsigned long i= 0; i < n; i++)
    if (p->note_insert())
      recycles++;
  return recycles;
}

static void test_fresh_policy()
{
  WriterRecyclePolicy p(10);
  CHECK(p.since_recycle() == 0);
  CHECK(p.recycles() == 0);
  CHECK(p.threshold() == 10);
}

static void test_does_not_recycle_early()
{
  /* threshold-1 inserts must not trigger a recycle: closing the
     connection more often than configured would turn a bounded cost into
     a per-insert reconnect storm */
  WriterRecyclePolicy p(10);
  CHECK(run_inserts(&p, 9) == 0);
  CHECK(p.recycles() == 0);
  CHECK(p.since_recycle() == 9);
}

static void test_recycles_exactly_on_threshold()
{
  WriterRecyclePolicy p(10);
  CHECK(run_inserts(&p, 9) == 0);
  CHECK(p.note_insert());              /* the 10th trips it */
  CHECK(p.recycles() == 1);
  CHECK(p.since_recycle() == 0);       /* counter reset for the next cycle */
}

static void test_period_does_not_drift()
{
  /* Each recycle resets the counter, so recycles land on a fixed period
     (10, 20, 30...), never drifting later and later. */
  WriterRecyclePolicy p(10);
  CHECK(run_inserts(&p, 10) == 1);
  CHECK(run_inserts(&p, 10) == 1);
  CHECK(run_inserts(&p, 10) == 1);
  CHECK(p.recycles() == 3);
  CHECK(p.since_recycle() == 0);
}

static void test_recycle_count_matches_floor_division()
{
  /* The core invariant: N inserts produce exactly floor(N/threshold)
     recycles, and the leftovers stay pending on the counter. */
  WriterRecyclePolicy p(10);
  CHECK(run_inserts(&p, 95) == 9);
  CHECK(p.recycles() == 9);
  CHECK(p.since_recycle() == 5);       /* 95 - 9*10 */

  WriterRecyclePolicy p2(7);
  CHECK(run_inserts(&p2, 100) == 14);  /* floor(100/7) */
  CHECK(p2.since_recycle() == 2);      /* 100 - 14*7 */
}

static void test_threshold_one_recycles_every_insert()
{
  WriterRecyclePolicy p(1);
  CHECK(p.note_insert());
  CHECK(p.note_insert());
  CHECK(p.note_insert());
  CHECK(p.recycles() == 3);
  CHECK(p.since_recycle() == 0);
}

static void test_threshold_zero_disables_recycling()
{
  /* Escape hatch for benchmarking the old (leaking) behavior — must
     never recycle, and must not divide-by-zero or wrap. */
  WriterRecyclePolicy p(0);
  CHECK(run_inserts(&p, 1000000) == 0);
  CHECK(p.recycles() == 0);
}

static void test_connection_reset_restarts_the_count()
{
  /* An out-of-band drop (server closed the connection, error-path retry)
     gives us a fresh THD with a fresh arena, so the count restarts —
     but it is not a recycle this policy performed, so recycles() must
     not move. */
  WriterRecyclePolicy p(10);
  CHECK(run_inserts(&p, 7) == 0);
  CHECK(p.since_recycle() == 7);

  p.note_connection_reset();
  CHECK(p.since_recycle() == 0);
  CHECK(p.recycles() == 0);            /* not counted as a recycle */

  /* and the next recycle is a FULL period away, not the 3 that were
     left over before the reset */
  CHECK(run_inserts(&p, 9) == 0);
  CHECK(p.note_insert());
  CHECK(p.recycles() == 1);
}

static void test_bounded_inserts_per_connection()
{
  /* The property that actually bounds RSS: no single connection ever
     accumulates more than `threshold` inserts. Walk a large run and
     assert the invariant holds at every step. */
  WriterRecyclePolicy p(20);
  bool ever_exceeded= false;
  for (unsigned long i= 0; i < 10000; i++)
  {
    p.note_insert();
    if (p.since_recycle() >= p.threshold())
      ever_exceeded= true;
  }
  CHECK(!ever_exceeded);
}

static void test_default_constructor_uses_the_shipped_threshold()
{
  WriterRecyclePolicy p;
  CHECK(p.threshold() == WRITER_RECONNECT_EVERY_N_INSERTS);
}

#if SELECTIVE_TRACE_RECONNECT_EVERY == 20000
/* These two only make sense for a normal build. A benchmark build
   (-DSELECTIVE_TRACE_RECONNECT_EVERY=0, used to reproduce the old
   unbounded-RSS behavior) deliberately changes the threshold, so
   skip them rather than fail. */

static void test_high_volume_matches_load_repro()
{
  /* Mirrors the sustained-load reproduction used on the MariaDB sibling
     (1.5M events) at the shipped default threshold: 1500000/20000 = 75
     recycles exactly, nothing pending. */
  WriterRecyclePolicy p;
  CHECK(run_inserts(&p, 1500000) == 75);
  CHECK(p.recycles() == 75);
  CHECK(p.since_recycle() == 0);
}

static void test_default_threshold_value()
{
  /* Pinned deliberately: this is the number that keeps the sibling
     MariaDB plugin's RSS flat at ~500 MB in the load repro. Changing it
     is a behavior change, not a tuning detail — this check makes that
     explicit rather than silent. */
  CHECK(WRITER_RECONNECT_EVERY_N_INSERTS == 20000);
}
#endif

int main()
{
  test_fresh_policy();
  test_does_not_recycle_early();
  test_recycles_exactly_on_threshold();
  test_period_does_not_drift();
  test_recycle_count_matches_floor_division();
  test_threshold_one_recycles_every_insert();
  test_threshold_zero_disables_recycling();
  test_connection_reset_restarts_the_count();
  test_bounded_inserts_per_connection();
  test_default_constructor_uses_the_shipped_threshold();
#if SELECTIVE_TRACE_RECONNECT_EVERY == 20000
  test_high_volume_matches_load_repro();
  test_default_threshold_value();
#endif

  if (failures)
  {
    std::fprintf(stderr, "%d/%d checks FAILED\n", failures, checks);
    return 1;
  }
  std::printf("OK — %d checks passed\n", checks);
  return 0;
}
