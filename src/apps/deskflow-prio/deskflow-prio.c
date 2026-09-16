// SPDX-FileCopyrightText: (C) 2026 Deskflow Contributors
// SPDX-License-Identifier: MIT
//
// deskflow-prio: promote deskflow-core to the foreground-application task
// role and raise the scheduling importance of all of its threads.
//
// deskflow-core is spawned by the Deskflow GUI as a plain child process, so it
// lands in the QoS-default band (PRI 31) and its input-relay threads compete
// with background daemons. renice has no effect (threads are QoS-clamped) and
// launchd ProcessType=Interactive also tops out at 31, so the task role and
// per-thread precedence are set directly via Mach. task_for_pid needs root
// (the tool runs from a LaunchDaemon, see
// tools/launchd/io.github.hughesyadaddy.deskflow-prio.plist) and re-runs
// periodically because the core creates fresh threads on every coordination
// epoch / role switch.
//
// Usage:
//   deskflow-prio <pid> [importance]
//   deskflow-prio --name <process-name> [importance]   (every matching pid)
//
// Historically built out-of-tree from /usr/local/src-deskflow-prio.c and
// installed ad-hoc/unsigned to /usr/local/bin; it now ships inside
// Deskflow.app/Contents/MacOS with the org.deskflow.deskflow-prio identifier
// so it is covered by the same signing gates as every other first-party binary.

#include <libproc.h>
#include <mach/mach.h>
#include <mach/task_policy.h>
#include <mach/thread_policy.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

static const int kDefaultImportance = 16;

static int promote(pid_t pid, int importance)
{
  mach_port_t task;
  kern_return_t kr = task_for_pid(mach_task_self(), pid, &task);
  if (kr != KERN_SUCCESS) {
    fprintf(stderr, "task_for_pid(%d): %s\n", (int)pid, mach_error_string(kr));
    return 1;
  }
  struct task_category_policy tcat = {.role = TASK_FOREGROUND_APPLICATION};
  kr = task_policy_set(task, TASK_CATEGORY_POLICY, (task_policy_t)&tcat, TASK_CATEGORY_POLICY_COUNT);
  if (kr != KERN_SUCCESS)
    fprintf(stderr, "task role: %s\n", mach_error_string(kr));

  thread_act_array_t threads;
  mach_msg_type_number_t n = 0;
  kr = task_threads(task, &threads, &n);
  if (kr != KERN_SUCCESS) {
    fprintf(stderr, "task_threads: %s\n", mach_error_string(kr));
    mach_port_deallocate(mach_task_self(), task);
    return 1;
  }
  int ok = 0;
  for (unsigned i = 0; i < n; i++) {
    thread_extended_policy_data_t ext = {.timeshare = TRUE};
    thread_policy_set(threads[i], THREAD_EXTENDED_POLICY, (thread_policy_t)&ext, THREAD_EXTENDED_POLICY_COUNT);
    thread_precedence_policy_data_t prec = {.importance = importance};
    if (thread_policy_set(threads[i], THREAD_PRECEDENCE_POLICY, (thread_policy_t)&prec, THREAD_PRECEDENCE_POLICY_COUNT) ==
        KERN_SUCCESS)
      ok++;
    mach_port_deallocate(mach_task_self(), threads[i]);
  }
  vm_deallocate(mach_task_self(), (vm_address_t)threads, n * sizeof(thread_act_t));
  mach_port_deallocate(mach_task_self(), task);
  printf("pid %d: role=foreground, %d/%u threads importance=%d\n", (int)pid, ok, n, importance);
  return 0;
}

// Promote every process whose short name equals `name`. Returns 0 when at
// least one promotion succeeded or no process matched (nothing to do is not
// an error for a periodic daemon), 1 when every matching pid failed.
static int promoteByName(const char *name, int importance)
{
  int count = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
  if (count <= 0) {
    fprintf(stderr, "proc_listpids: no processes\n");
    return 1;
  }
  size_t bytes = (size_t)count * sizeof(pid_t) * 2;
  pid_t *pids = calloc(1, bytes);
  if (!pids) {
    fprintf(stderr, "out of memory\n");
    return 1;
  }
  count = proc_listpids(PROC_ALL_PIDS, 0, pids, (int)bytes) / (int)sizeof(pid_t);
  int matched = 0, promoted = 0;
  for (int i = 0; i < count; i++) {
    if (pids[i] <= 0)
      continue;
    char pname[2 * MAXCOMLEN + 1] = {0};
    if (proc_name(pids[i], pname, sizeof(pname)) <= 0)
      continue;
    if (strcmp(pname, name) != 0)
      continue;
    matched++;
    if (promote(pids[i], importance) == 0)
      promoted++;
  }
  free(pids);
  if (matched == 0) {
    printf("no process named %s\n", name);
    return 0;
  }
  return promoted > 0 ? 0 : 1;
}

static int usage(void)
{
  fprintf(stderr, "usage: deskflow-prio <pid> [importance]\n"
                  "       deskflow-prio --name <process-name> [importance]\n");
  return 2;
}

int main(int argc, char **argv)
{
  if (argc < 2)
    return usage();
  if (strcmp(argv[1], "--name") == 0) {
    if (argc < 3)
      return usage();
    int imp = (argc > 3) ? atoi(argv[3]) : kDefaultImportance;
    return promoteByName(argv[2], imp);
  }
  pid_t pid = (pid_t)atoi(argv[1]);
  if (pid <= 0)
    return usage();
  int imp = (argc > 2) ? atoi(argv[2]) : kDefaultImportance;
  return promote(pid, imp);
}
