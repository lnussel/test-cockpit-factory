/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2014 Red Hat, Inc.
 *
 * Cockpit is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * Cockpit is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Cockpit; If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "cockpitcgroupsamples.h"

#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const char *cockpit_cgroup_memory_root = "/sys/fs/cgroup/memory";
const char *cockpit_cgroup_cpuacct_root = "/sys/fs/cgroup/cpuacct";

static const char *
read_file (int dirfd,
           char *buf,
           size_t bufsize,
           const char *cgroup,
           const char *fname)
{
  int fd = -1;
  ssize_t len;
  const char *ret = NULL;

  fd = openat (dirfd, fname, O_RDONLY);
  if (fd < 0)
    {
      if (errno == ENOENT || errno == ENODEV)
        g_debug ("samples file not found: %s/%s", cgroup, fname);
      else
        g_message ("error opening file: %s/%s: %m", cgroup, fname);
      return NULL;
    }

  /* don't do fancy retry/error handling here -- we know what cgroupfs attributes look like,
   * it's a virtual file system (does not block/no multiple reads), and it's ok to miss
   * one sample due to EINTR or some race condition */
  len = read (fd, buf, bufsize);
  if (len < 0)
    {
      g_message ("error loading file: %s/%s: %m", cgroup, fname);
      goto out;
    }
  /* we really expect a much smaller read; if we get a full buffer, there's likely
   * more data, and we are misinterpreting stuff */
  if (len >= bufsize)
    {
      g_warning ("cgroupfs value %s/%s is too large", cgroup, fname);
      goto out;
    }
  buf[len] = '\0';
  ret = buf;

out:
  if (fd >= 0)
    close (fd);
  return ret;
}


static gint64
read_int64 (int dirfd,
            const char *cgroup,
            const char *fname)
{
  char buf[30];
  const char *contents = read_file (dirfd, buf, sizeof buf, cgroup, fname);

  if (contents == NULL)
      return -1;
  /* no error checking; these often have values like "max" which we want to treat as "invalid/absent" */
  return atoll(contents);
}

static void
collect_memory (CockpitSamples *samples,
                int dirfd,
                const char *cgroup)
{
  gint64 val;

  val = read_int64 (dirfd, cgroup, "memory.usage_in_bytes");
  if (val > 0 && val < G_MAXINT64)
    cockpit_samples_sample (samples, "cgroup.memory.usage", cgroup, val);

  val = read_int64 (dirfd, cgroup, "memory.limit_in_bytes");
  /* If at max for arch, then unlimited => zero */
  if (val > 0 && val < G_MAXINT64)
    cockpit_samples_sample (samples, "cgroup.memory.limit", cgroup, val);

  val = read_int64 (dirfd, cgroup, "memory.memsw.usage_in_bytes");
  if (val >= 0 && val < G_MAXINT64)
      cockpit_samples_sample (samples, "cgroup.memory.sw-usage", cgroup, val);

  val = read_int64 (dirfd, cgroup, "memory.memsw.limit_in_bytes");
  /* If at max for arch, then unlimited => zero */
  if (val > 0 && val < G_MAXINT64)
      cockpit_samples_sample (samples, "cgroup.memory.sw-limit", cgroup, val);
}

static void
collect_cpu (CockpitSamples *samples,
             int dirfd,
             const char *cgroup)
{
  gint64 val;

  val = read_int64 (dirfd, cgroup, "cpuacct.usage");
  if (val >= 0 && val < G_MAXINT64)
    cockpit_samples_sample (samples, "cgroup.cpu.usage", cgroup, val/1000000);

  val = read_int64 (dirfd, cgroup, "cpu.shares");
  if (val > 0 && val < G_MAXINT64)
    cockpit_samples_sample (samples, "cgroup.cpu.shares", cgroup, val);
}

static void
notice_cgroups_in_hierarchy (CockpitSamples *samples,
                             const char *root_dir,
                             void (* collect) (CockpitSamples *, int, const char *))
{
  const char *paths[] = { root_dir, NULL };
  gsize root_dir_len = strlen (root_dir);
  FTSENT *ent;
  FTS *fs;

  fs = fts_open ((char **)paths, FTS_NOCHDIR | FTS_COMFOLLOW, NULL);
  if (fs)
    {
      while((ent = fts_read (fs)) != NULL)
        {
          if (ent->fts_info == FTS_D)
            {
              const char *f = ent->fts_path + root_dir_len;
              int dfd;

              if (*f == '/')
                f++;
              dfd = open (ent->fts_path, O_PATH | O_DIRECTORY);
              if (dfd >= 0)
                {
                  collect (samples, dfd, f);
                  close (dfd);
                }
              else
                {
                  g_message ("error opening cgroup directory: %s: %m", ent->fts_path);
                }
            }
        }
      fts_close (fs);
    }
}


void
cockpit_cgroup_samples (CockpitSamples *samples)
{
  /* We are looking for files like

     /sys/fs/cgroup/memory/.../memory.usage_in_bytes
     /sys/fs/cgroup/memory/.../memory.limit_in_bytes
     /sys/fs/cgroup/cpuacct/.../cpuacct.usage
  */

  notice_cgroups_in_hierarchy (samples, cockpit_cgroup_memory_root, collect_memory);
  notice_cgroups_in_hierarchy (samples, cockpit_cgroup_cpuacct_root, collect_cpu);
}
