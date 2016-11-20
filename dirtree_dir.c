/*
Copyright (C) 2016  Charles Cagle

This file is part of archive2sqfs.

archive2sqfs is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, version 3.

archive2sqfs is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with archive2sqfs.  If not, see <http://www.gnu.org/licenses/>.
*/

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <search.h>

#include "dirtree.h"
#include "sqsh_defs.h"
#include "sqsh_writer.h"

static int dirtree_dirop_prep(struct dirtree * const dt)
{
  if (dt->addi.dir.nentries == dt->addi.dir.space)
    {
      size_t const space = dt->addi.dir.space + 0x10;
      struct dirtree_entry * const entries = realloc(dt->addi.dir.entries, sizeof(*entries) * space);
      if (entries == NULL)
        return ENOMEM;

      dt->addi.dir.entries = entries;
      dt->addi.dir.space = space;
    }
  return 0;
}

void dirtree_dir_init(struct dirtree * const dt, struct sqsh_writer * const wr)
{
  dirtree_init(dt, wr);

  dt->inode_type = SQFS_INODE_TYPE_DIR;
  dt->mode = 0755;

  dt->addi.dir.nentries = 0;
  dt->addi.dir.space = 0;
  dt->addi.dir.entries = NULL;
}

struct dirtree * dirtree_dir_new(struct sqsh_writer * const wr)
{
  return dirtree_new(wr, dirtree_dir_init);
}

static struct dirtree_entry * dirtree_get_child_entry(struct dirtree * const dt, char const * name)
{
  struct dirtree_entry * entry = lfind(name, dt->addi.dir.entries, &dt->addi.dir.nentries, sizeof(*entry), dirtree_entry_by_name);
  if (entry == NULL && !dirtree_dirop_prep(dt))
    {
      entry = dt->addi.dir.entries + dt->addi.dir.nentries++;
      entry->name = name;
      entry->inode = NULL;
    }

  return entry;
}

static struct dirtree * dirtree_get_child(struct sqsh_writer * const wr, struct dirtree * const dt, char const * name, struct dirtree * (*con)(struct sqsh_writer *))
{
  struct dirtree_entry * const entry = dirtree_get_child_entry(dt, name);
  if (entry == NULL)
    return NULL;

  if (entry->inode == NULL)
    {
      entry->name = strdup(name);
      entry->inode = entry->name == NULL ? NULL : con(wr);
      if (entry->inode == NULL)
        {
          free((char *) entry->name);
          dt->addi.dir.nentries--;
          return NULL;
        }
    }

  return entry->inode;
}

struct dirtree * dirtree_get_subdir(struct sqsh_writer * const wr, struct dirtree * const dt, char const * name)
{
  return dirtree_get_child(wr, dt, name, dirtree_dir_new);
}

struct dirtree * dirtree_put_reg(struct sqsh_writer * const wr, struct dirtree * const dt, char const * const name)
{
  return dirtree_get_child(wr, dt, name, dirtree_reg_new);
}

struct dirtree * dirtree_get_subdir_for_path(struct sqsh_writer * const wr, struct dirtree * const dt, char const * const path)
{
  char pathtokens[strlen(path) + 1];
  strcpy(pathtokens, path);

  char * ststate;
  struct dirtree * subdir = dt;
  for (char const * component = strtok_r(pathtokens, "/", &ststate); component != NULL; component = strtok_r(NULL, "/", &ststate))
    if (component[0] != 0 && subdir != NULL)
      subdir = dirtree_get_subdir(wr, subdir, component);

  return subdir;
}

static struct dirtree * dirtree_put_nondir_for_path(struct sqsh_writer * const wr, struct dirtree * const root, char const * const path, struct dirtree * (*con)(struct sqsh_writer *))
{
  char tmppath[strlen(path) + 1];
  strcpy(tmppath, path);

  char * const sep = strrchr(tmppath, '/');
  char * const name = sep == NULL ? tmppath : sep + 1;
  char * const parent = sep == NULL ? "/" : tmppath;

  if (sep != NULL)
    *sep = 0;

  struct dirtree * parent_dt = dirtree_get_subdir_for_path(wr, root, parent);
  return parent_dt == NULL ? NULL : dirtree_get_child(wr, parent_dt, name, con);
}

struct dirtree * dirtree_put_reg_for_path(struct sqsh_writer * const wr, struct dirtree * const root, char const * const path)
{
  return dirtree_put_nondir_for_path(wr, root, path, dirtree_reg_new);
}

struct dirtree * dirtree_put_sym_for_path(struct sqsh_writer * const wr, struct dirtree * const root, char const * const path, char const * const target)
{
  struct dirtree * const sym = dirtree_put_nondir_for_path(wr, root, path, dirtree_sym_new);
  if (sym == NULL)
    return NULL;

  sym->addi.sym.target = strdup(target);
  if (sym->addi.sym.target == NULL)
    return dirtree_free(sym), NULL;

  return sym;
}

struct dirtree * dirtree_put_dev_for_path(struct sqsh_writer * const wr, struct dirtree * const root, char const * const path, uint16_t type, uint32_t rdev)
{
  struct dirtree * const dev = dirtree_put_nondir_for_path(wr, root, path, dirtree_dev_new);
  if (dev != NULL)
    {
      dev->inode_type = type;
      dev->addi.dev.rdev = rdev;
    }

  return dev;
}

struct dirtree * dirtree_put_ipc_for_path(struct sqsh_writer * const wr, struct dirtree * const root, char const * const path, uint16_t type)
{
  struct dirtree * const ipc = dirtree_put_nondir_for_path(wr, root, path, dirtree_ipc_new);
  if (ipc != NULL)
    ipc->inode_type = type;

  return ipc;
}
