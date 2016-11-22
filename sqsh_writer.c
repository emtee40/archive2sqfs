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
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <search.h>

#include "dw.h"
#include "le.h"
#include "mdw.h"
#include "sqsh_writer.h"
#include "util.h"

static int fround_to(FILE * const f, long int const block)
{
  long int const tell = ftell(f);
  if (tell == -1)
    return 1;

  size_t const fill = block - (tell % block);
  unsigned char buff[fill];
  memset(buff, 0, fill);
  return fwrite(buff, 1, fill, f) != fill;
}

static void sqfs_super_init(struct sqfs_super * const super, int const block_log)
{
  super->fragments = 0;
  super->compression = 1;
  super->block_log = block_log;
  super->flags = 0;
  super->root_inode = 0;
  super->bytes_used = 0;
  super->id_table_start = 0;
  super->xattr_table_start = 0xffffffffffffffffu;
  super->inode_table_start = 0;
  super->directory_table_start = 0;
  super->fragment_table_start = 0;
  super->lookup_table_start = 0xffffffffffffffffu;
}

static void sqsh_writer_free(struct sqsh_writer * const wr)
{
  free(wr->current_block);
  free(wr->ids);
}

static int sqsh_writer_alloc(struct sqsh_writer * const wr, int const block_log)
{
  wr->current_block = malloc((size_t) 2 << block_log);
  wr->current_fragment = wr->current_block == NULL ? NULL : wr->current_block + ((size_t) 1 << block_log);
  wr->ids = wr->current_fragment == NULL ? NULL : malloc(sizeof(*wr->ids) * 0x10000);

  if (wr->current_fragment == NULL)
    return sqsh_writer_free(wr), ENOMEM;
  return 0;
}

int sqsh_writer_init(struct sqsh_writer * const wr, char const * const path, int const block_log)
{
  if (sqsh_writer_alloc(wr, block_log))
    return ENOMEM;

  wr->next_inode = 1;
  mdw_init(&wr->dentry_writer);
  mdw_init(&wr->inode_writer);

  sqfs_super_init(&wr->super, block_log);
  wr->current_pos = 0;
  wr->fragment_pos = 0;
  wr->fragments = NULL;
  wr->fragment_space = 0;
  wr->nids = 0;

  wr->outfile = fopen(path, "wb");
  return wr->outfile == NULL || fseek(wr->outfile, 96L, SEEK_SET);
}

int sqsh_writer_destroy(struct sqsh_writer * const wr)
{
  sqsh_writer_free(wr);
  mdw_destroy(&wr->inode_writer);
  mdw_destroy(&wr->dentry_writer);
  return wr->outfile == NULL || fclose(wr->outfile);
}

static int sqsh_writer_append_fragment(struct sqsh_writer * const wr, uint32_t const size, uint64_t const start_block)
{
  if (wr->super.fragments == wr->fragment_space)
    {
      size_t const space = wr->fragment_space + 0x100;
      struct fragment_entry * const fragments = realloc(wr->fragments, sizeof(*fragments) * space);
      if (fragments == NULL)
        return ENOMEM;

      wr->fragment_space = space;
      wr->fragments = fragments;
    }

  wr->fragments[wr->super.fragments].start_block = start_block;
  wr->fragments[wr->super.fragments++].size = size;
  return 0;
}

int sqsh_writer_flush_fragment(struct sqsh_writer * const wr)
{
  if (wr->fragment_pos == 0)
    return 0;

  long int const tell = ftell(wr->outfile);
  if (tell == -1)
    return 1;

  uint32_t const bsize = dw_write_data(wr->current_fragment, wr->fragment_pos, wr->outfile);
  if (bsize == 0xffffffff)
    return 1;

  wr->fragment_pos = 0;
  return sqsh_writer_append_fragment(wr, bsize, tell);
}

size_t sqsh_writer_put_fragment(struct sqsh_writer * const wr, unsigned char const * const buff, size_t const len)
{
  size_t const block_size = (size_t) 1 << wr->super.block_log;

  if (wr->fragment_pos + len > block_size)
    if (sqsh_writer_flush_fragment(wr))
      return block_size;

  memcpy(wr->current_fragment + wr->fragment_pos, buff, len);
  size_t const offset = wr->fragment_pos;
  wr->fragment_pos += len;
  return offset;
}

int u32cmp(void const * va, void const * vb)
{
  uint32_t const a = *(uint32_t const *) va;
  uint32_t const b = *(uint32_t const *) vb;
  return a < b ? -1 : a > b;
}

int sqsh_writer_write_header(struct sqsh_writer * const writer)
{
  uint8_t header[96];

  le32(header, SQFS_MAGIC);
  le32(header + 4, writer->next_inode - 1);
  le32(header + 8, 0);
  le32(header + 12, 1u << writer->super.block_log);
  le32(header + 16, writer->super.fragments);

  le16(header + 20, writer->super.compression);
  le16(header + 22, writer->super.block_log);
  le16(header + 24, writer->super.flags);
  le16(header + 26, writer->nids);
  le16(header + 28, SQFS_MAJOR);
  le16(header + 30, SQFS_MINOR);

  le64(header + 32, writer->super.root_inode);
  le64(header + 40, writer->super.bytes_used);
  le64(header + 48, writer->super.id_table_start);
  le64(header + 56, writer->super.xattr_table_start);
  le64(header + 64, writer->super.inode_table_start);
  le64(header + 72, writer->super.directory_table_start);
  le64(header + 80, writer->super.fragment_table_start);
  le64(header + 88, writer->super.lookup_table_start);

  return fround_to(writer->outfile, SQFS_PAD_SIZE) || fseek(writer->outfile, 0L, SEEK_SET) || fwrite(header, 1, sizeof(header), writer->outfile) != sizeof(header);
}

#define ITD_SHIFT(ENTRY_LB) (SQFS_META_BLOCK_SIZE_LB - (ENTRY_LB))
#define ITD_MASK(ENTRY_LB) MASK_LOW(ITD_SHIFT(ENTRY_LB))
#define ITD_ENTRY_SIZE(ENTRY_LB) (1u << (ENTRY_LB))

#define SQSH_WRITER_WRITE_INDEXED_TABLE_DEFN(TABLE, ENTRY_LB, COUNT_FIELD)                                        \
  static int sqsh_writer_write_##TABLE##_table(struct sqsh_writer * const wr)                                     \
  {                                                                                                               \
    size_t const index_count = (wr->COUNT_FIELD >> ITD_SHIFT(ENTRY_LB)) + ((wr->nids & ITD_MASK(ENTRY_LB)) != 0); \
    unsigned char indices[index_count * 8];                                                                       \
    struct mdw mdw;                                                                                               \
    mdw_init(&mdw);                                                                                               \
    size_t index = 0;                                                                                             \
                                                                                                                  \
    for (size_t i = 0; i < wr->COUNT_FIELD; i++)                                                                  \
      {                                                                                                           \
        unsigned char buff[ITD_ENTRY_SIZE(ENTRY_LB)];                                                             \
        sqsh_writer_##TABLE##_table_entry(buff, wr, i);                                                           \
        uint64_t const maddr = mdw_put(&mdw, buff, ITD_ENTRY_SIZE(ENTRY_LB));                                     \
        RETIF_C(meta_address_error(maddr), mdw_destroy(&mdw));                                                    \
                                                                                                                  \
        if ((i & ITD_MASK(ENTRY_LB)) == 0)                                                                        \
          le64(indices + index++ * 8, wr->super.TABLE##_table_start + meta_address_block(maddr));                 \
      }                                                                                                           \
                                                                                                                  \
    int error = 0;                                                                                                \
    if (wr->COUNT_FIELD & ITD_MASK(ENTRY_LB))                                                                     \
      error = mdw_write_block_no_pad(&mdw);                                                                       \
                                                                                                                  \
    error = error || mdw_out(&mdw, wr->outfile);                                                                  \
    mdw_destroy(&mdw);                                                                                            \
                                                                                                                  \
    long int const tell = error ? -1 : ftell(wr->outfile);                                                        \
    error = error || tell == -1;                                                                                  \
    wr->super.TABLE##_table_start = tell;                                                                         \
                                                                                                                  \
    return error || fwrite(indices, 1, index_count * 8, wr->outfile) != index_count * 8;                          \
  }

static inline void sqsh_writer_id_table_entry(unsigned char buff[static 4], struct sqsh_writer * const wr, size_t const i)
{
  le32(buff, wr->ids[i]);
}

static inline void sqsh_writer_fragment_table_entry(unsigned char buff[static 16], struct sqsh_writer * const wr, size_t const i)
{
  struct fragment_entry const * const frag = wr->fragments + i;
  le64(buff, frag->start_block);
  le32(buff + 8, frag->size);
  le32(buff + 12, 0);
}

SQSH_WRITER_WRITE_INDEXED_TABLE_DEFN(id, 2, nids)
SQSH_WRITER_WRITE_INDEXED_TABLE_DEFN(fragment, 4, super.fragments)

static int sqsh_writer_write_inode_table(struct sqsh_writer * const wr)
{
  return mdw_out(&wr->inode_writer, wr->outfile);
}

static int sqsh_writer_write_directory_table(struct sqsh_writer * const wr)
{
  return mdw_out(&wr->dentry_writer, wr->outfile);
}

static inline void tell_wr(struct sqsh_writer * const wr, int * const error, uint64_t * const start, int (*cb)(struct sqsh_writer *))
{
  long int const tell = ftell(wr->outfile);
  *error = *error || tell == -1;
  *start = tell;
  *error = *error || cb(wr);
}

int sqsh_writer_write_tables(struct sqsh_writer * const wr)
{
  int error = 0;

#define TELL_WR(T) tell_wr(wr, &error, &wr->super.T##_table_start, sqsh_writer_write_##T##_table)
  TELL_WR(inode);
  TELL_WR(directory);
  TELL_WR(fragment);
  TELL_WR(id);
#undef TELL_WR

  long int const tell = ftell(wr->outfile);
  error = error || tell == -1;
  wr->super.bytes_used = tell;

  return error;
}