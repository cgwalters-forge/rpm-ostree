/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

/**
 * Decode RPM payloads with librpm, adapting them to OSTree's archive importer.
 */

#include "config.h"

#include "rpmostree-core.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-unpacker-core.h"
#include <gio/gunixinputstream.h>
#include <grp.h>
#include <pwd.h>
#include <rpm/rpmarchive.h>
#include <rpm/rpmfi.h>
#include <rpm/rpmfiles.h>
#include <rpm/rpmlib.h>
#include <rpm/rpmlog.h>
#include <rpm/rpmts.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/**
 * throw_libarchive_error:
 * @ar: An archive
 * @error: GError
 * @prefix: Optional error prefix
 *
 * Transform a libarchive error into a Glib one, then clean up the input archive.
 *
 * Return: Always FALSE
 */
static gboolean
throw_libarchive_error (struct archive *ar, GError **error, char const *prefix)
{
  g_assert (ar != NULL);

  const char *err_string = archive_error_string (ar);
  g_assert (err_string != NULL);
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, err_string);

  if (prefix != NULL)
    glnx_prefix_error (error, "%s", prefix);

  return FALSE;
}

typedef struct archive archive_writer;

G_DEFINE_AUTOPTR_CLEANUP_FUNC (archive_writer, archive_write_free);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (archive_entry, archive_entry_free);

/* librpm understands both classic and stripped cpio. Stream an uncompressed
 * tar so the existing OSTree path translation,
 * filters and xattr/SELinux callbacks remain the only import path. No payload
 * paths are ever extracted to the filesystem. */
static gboolean
rpmostree_unpack_rpm_payload (int fd, Header hdr, off_t payload_offset, int output_fd,
                              GCancellable *cancellable, GError **error)
{
  const char *format = headerGetString (hdr, RPMTAG_PAYLOADFORMAT);
  if (!format || !g_str_equal (format, "cpio"))
    return glnx_throw (error, "Unsupported payload format '%s'", format ?: "unknown");

  g_autofree char *path = g_strdup_printf ("/proc/self/fd/%d", fd);
  g_auto (FD_t) payload = Fopen (path, "r.fdio");
  if (!payload || Ferror (payload))
    return glnx_throw (error, "Opening RPM payload");
  if (Fseek (payload, payload_offset, SEEK_SET) < 0)
    return glnx_throw (error, "Seeking RPM payload: %s", Fstrerror (payload));
  const char *compressor = headerGetString (hdr, RPMTAG_PAYLOADCOMPRESSOR);
  g_autofree char *mode = g_strdup_printf ("r.%s", compressor ?: "gzip");
  if (!Fdopen (payload, mode) || Ferror (payload))
    return glnx_throw (error, "Opening payload compressor '%s'", compressor ?: "gzip");

  g_auto (rpmfiles) files = rpmfilesNew (NULL, hdr, 0, RPMFI_KEEPHEADER);
  g_auto (rpmfi) fi = rpmfiNewArchiveReader (payload, files, RPMFI_ITER_READ_ARCHIVE_CONTENT_FIRST);
  if (!fi)
    return glnx_throw (error, "Creating RPM payload reader");

  g_autoptr (archive_writer) writer = archive_write_new ();
  auto write_cb = [] (struct archive *ar, void *data, const void *buf, size_t len) -> la_ssize_t
    {
      int sock = *static_cast<int *> (data);
      ssize_t n;
      do
        n = send (sock, buf, len, MSG_NOSIGNAL);
      while (n < 0 && errno == EINTR);
      if (n < 0)
        archive_set_error (ar, errno, "Writing payload stream: %s", g_strerror (errno));
      return n;
    };
  if (archive_write_set_format_pax_restricted (writer) != ARCHIVE_OK
      || archive_write_set_format_option (writer, "pax", "hdrcharset", "BINARY") != ARCHIVE_OK
      || archive_write_open (writer, &output_fd, NULL, write_cb, NULL) != ARCHIVE_OK)
    return throw_libarchive_error (writer, error, "Opening payload adapter");

  g_autoptr (archive_entry) entry = archive_entry_new ();
  g_auto (GLnxTmpfile) hardlink_content = { 0 };
  guint links_remaining = 0;
  char buf[128 * 1024];
  int rc;
  while ((rc = rpmfiNext (fi)) >= 0)
    {
      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        return FALSE;
      archive_entry_clear (entry);
      g_autofree char *name = g_strconcat (".", rpmfiFN (fi), NULL);
      archive_entry_set_pathname (entry, name);
      /* Avoid rpmfiStat: host account lookup is not thread-safe in older RPM
       * versions. Everything is imported as root, and ownership is applied
       * from the header overrides at checkout, which also restores the mode
       * of regular files. Until then, never leave a setid bit meant for
       * another account on a root-owned executable. Directory modes are kept
       * as is: they aren't reapplied, and setgid there only means inheritance. */
      struct stat st = { 0 };
      st.st_mode = rpmfiFMode (fi);
      if (S_ISREG (st.st_mode) && !g_str_equal (rpmfiFUser (fi) ?: "root", "root"))
        st.st_mode &= ~S_ISUID;
      if (S_ISREG (st.st_mode) && !g_str_equal (rpmfiFGroup (fi) ?: "root", "root"))
        st.st_mode &= ~S_ISGID;
      st.st_nlink = rpmfiFNlink (fi);
      st.st_ino = rpmfiFInode (fi);
      st.st_rdev = rpmfiFRdev (fi);
      st.st_mtime = rpmfiFMtime (fi);
      if (S_ISREG (st.st_mode) || S_ISLNK (st.st_mode))
        st.st_size = rpmfiFSize (fi);
      archive_entry_copy_stat (entry, &st);
      if (S_ISLNK (st.st_mode))
        archive_entry_set_symlink (entry, rpmfiFLink (fi));
      if (S_ISREG (st.st_mode) && st.st_nlink > 1)
        {
          if (rpmfiArchiveHasContent (fi))
            {
              links_remaining = st.st_nlink;
              if (rpmfiFSize (fi) > 0
                  && !glnx_open_anonymous_tmpfile (O_RDWR | O_CLOEXEC, &hardlink_content, error))
                return FALSE;
            }
          /* Emit independent regular entries: filters may discard any member.
           * OSTree deduplicates their content. CONTENT_FIRST visits each group
           * consecutively, so temporary storage is bounded by the largest
           * hardlinked file, rather than the entire uncompressed package. */
          archive_entry_set_size (entry, rpmfiFSize (fi));
        }
      if (archive_write_header (writer, entry) != ARCHIVE_OK)
        return throw_libarchive_error (writer, error, "Writing payload header");

      if (S_ISREG (st.st_mode))
        {
          rpm_loff_t remaining = rpmfiFSize (fi);
          off_t offset = 0;
          bool has_content = rpmfiArchiveHasContent (fi);
          while (remaining > 0)
            {
              if (g_cancellable_set_error_if_cancelled (cancellable, error))
                return FALSE;
              size_t len = MIN (remaining, sizeof buf);
              ssize_t n = has_content
                              ? rpmfiArchiveRead (fi, buf, len)
                              : TEMP_FAILURE_RETRY (pread (hardlink_content.fd, buf, len, offset));
              if (n != static_cast<ssize_t> (len))
                return glnx_throw (error, "Reading RPM payload file %s", name);
              if (has_content && hardlink_content.initialized
                  && glnx_loop_write (hardlink_content.fd, buf, len) < 0)
                return glnx_throw_errno_prefix (error, "Buffering hardlink %s", name);
              if (archive_write_data (writer, buf, len) != n)
                return throw_libarchive_error (writer, error, "Writing payload data");
              remaining -= len;
              offset += len;
            }
          if (links_remaining > 0 && --links_remaining == 0)
            glnx_tmpfile_clear (&hardlink_content);
        }
    }
  if (rc != RPMERR_ITER_END)
    {
      g_autofree char *message = rpmfileStrerror (rc);
      return glnx_throw (error, "Reading RPM payload: %s", message);
    }
  if (rpmfiArchiveClose (fi) != 0)
    return glnx_throw (error, "Closing RPM payload");
  if (archive_write_close (writer) != ARCHIVE_OK)
    return throw_libarchive_error (writer, error, "Closing payload adapter");

  return TRUE;
}

RpmOstreePayload::~RpmOstreePayload ()
{
  /* An import filter, cancellation or corrupt input can stop the consumer early.
   * Wake a blocked producer without delivering SIGPIPE to the process. */
  if (sockets[0] >= 0)
    shutdown (sockets[0], SHUT_RDWR);
  if (thread)
    g_thread_join (thread);
  glnx_close_fd (&sockets[0]);
  glnx_close_fd (&sockets[1]);
  g_clear_error (&producer_error);
}

struct archive *
RpmOstreePayload::open (GError **error)
{
  if (socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) < 0)
    {
      glnx_throw_errno (error);
      return NULL;
    }
  thread = g_thread_new (
      "rpm-payload",
      [] (gpointer data) -> gpointer
        {
          auto self = static_cast<RpmOstreePayload *> (data);
          rpmostree_unpack_rpm_payload (self->fd, self->hdr, self->offset, self->sockets[1],
                                        self->cancellable, &self->producer_error);
          shutdown (self->sockets[1], SHUT_WR);
          return NULL;
        },
      this);
  g_autoptr (archive) ar = archive_read_new ();
  /* The adapter marks its headers hdrcharset=BINARY, so names pass through as
   * raw bytes regardless of the process locale, as they did from cpio. */
  if (archive_read_support_format_tar (ar) != ARCHIVE_OK
      || archive_read_open_fd (ar, sockets[0], 10240) != ARCHIVE_OK)
    {
      shutdown (sockets[0], SHUT_RDWR);
      if (!finish (error))
        return NULL;
      throw_libarchive_error (ar, error, "Opening payload adapter");
      return NULL;
    }
  return util::move_nullify (ar);
}

gboolean
RpmOstreePayload::finish (GError **error)
{
  g_thread_join (thread);
  thread = nullptr;
  if (producer_error)
    {
      g_propagate_error (error, util::move_nullify (producer_error));
      return FALSE;
    }
  return TRUE;
}
