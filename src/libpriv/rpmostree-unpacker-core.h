/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#pragma once

#include <ostree.h>

#include "libglnx.h"
#include <archive.h>
#include <archive_entry.h>
#include <libdnf/libdnf.h>
#include <rpm/rpmlib.h>

G_BEGIN_DECLS

G_DEFINE_AUTOPTR_CLEANUP_FUNC (archive, archive_read_free);

G_END_DECLS

/* Stream the tar adapter through a bounded socket buffer. Only hardlinked
 * contents need temporary storage, at most one file at a time. The producer
 * is always joined before its inputs go away. */
class RpmOstreePayload
{
  int sockets[2] = { -1, -1 };
  GThread *thread = nullptr;
  GError *producer_error = nullptr;
  int fd;
  Header hdr;
  off_t offset;
  GCancellable *cancellable;

public:
  RpmOstreePayload (int fd, Header hdr, off_t offset, GCancellable *cancellable)
      : fd (fd), hdr (hdr), offset (offset), cancellable (cancellable)
  {
  }
  RpmOstreePayload (const RpmOstreePayload &) = delete;
  RpmOstreePayload &operator= (const RpmOstreePayload &) = delete;
  ~RpmOstreePayload ();
  struct archive *open (GError **error);
  gboolean finish (GError **error);
};
