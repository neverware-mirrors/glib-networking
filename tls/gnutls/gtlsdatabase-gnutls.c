/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * GIO - GLib Input, Output and Streaming Library
 *
 * Copyright 2010 Collabora, Ltd
 * Copyright 2018 Igalia S.L.
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
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, see
 * <http://www.gnu.org/licenses/>.
 *
 * In addition, when the library is used with OpenSSL, a special
 * exception applies. Refer to the LICENSE_EXCEPTION file for details.
 *
 * Author: Stef Walter <stefw@collabora.co.uk>
 */

#include "config.h"

#include "gtlsdatabase-gnutls.h"

#include <gio/gio.h>
#include <glib/gi18n-lib.h>
#include <gnutls/x509.h>

#include "gtlscertificate-gnutls.h"

typedef struct
{
  /*
   * This class is protected by mutex because the default GTlsDatabase
   * is a global singleton, accessible via the default GTlsBackend.
   */
  GMutex mutex;

  /* read-only after construct */
  gnutls_x509_trust_list_t trust_list;

  /*
   * These are hash tables of GBytes -> GPtrArray<GBytes>. The values of
   * the ptr array are full DER encoded certificate values. The keys are byte
   * arrays containing either subject DNs, issuer DNs, or full DER encoded certs
   */
  GHashTable *subjects;
  GHashTable *issuers;

  /*
   * This is a table of GBytes -> GBytes. The values and keys are
   * DER encoded certificate values.
   */
  GHashTable *complete;

  /*
   * This is a table of gchar * -> GPtrArray<GBytes>. The values of
   * the ptr array are full DER encoded certificate values. The keys are the
   * string handles. This array is populated on demand.
   */
  GHashTable *handles;

  /* Unowned. This is only set temporarily, during certificate verification. */
  GCancellable *verify_chain_cancellable;
} GTlsDatabaseGnutlsPrivate;

static void g_tls_database_gnutls_initable_interface_init (GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (GTlsDatabaseGnutls, g_tls_database_gnutls, G_TYPE_TLS_DATABASE,
                         G_ADD_PRIVATE (GTlsDatabaseGnutls);
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                g_tls_database_gnutls_initable_interface_init);
                         );

static GHashTable *
bytes_multi_table_new (void)
{
  return g_hash_table_new_full (g_bytes_hash, g_bytes_equal,
                                (GDestroyNotify)g_bytes_unref,
                                (GDestroyNotify)g_ptr_array_unref);
}

static void
bytes_multi_table_insert (GHashTable *table,
                          GBytes     *key,
                          GBytes     *value)
{
  GPtrArray *multi;

  multi = g_hash_table_lookup (table, key);
  if (!multi)
    {
      multi = g_ptr_array_new_with_free_func ((GDestroyNotify)g_bytes_unref);
      g_hash_table_insert (table, g_bytes_ref (key), multi);
    }
  g_ptr_array_add (multi, g_bytes_ref (value));
}

static GBytes *
bytes_multi_table_lookup_ref_one (GHashTable *table,
                                  GBytes     *key)
{
  GPtrArray *multi;

  multi = g_hash_table_lookup (table, key);
  if (!multi)
    return NULL;

  g_assert (multi->len > 0);
  return g_bytes_ref (multi->pdata[0]);
}

static GList *
bytes_multi_table_lookup_ref_all (GHashTable *table,
                                  GBytes     *key)
{
  GPtrArray *multi;
  GList *list = NULL;
  guint i;

  multi = g_hash_table_lookup (table, key);
  if (!multi)
    return NULL;

  for (i = 0; i < multi->len; i++)
    list = g_list_prepend (list, g_bytes_ref (multi->pdata[i]));

  return g_list_reverse (list);
}

static GHashTable *
create_handles_array_unlocked (GTlsDatabaseGnutls *self,
                               GHashTable         *complete)
{
  GHashTable *handles;
  GHashTableIter iter;
  GBytes *der;
  gchar *handle;

  handles = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                   (GDestroyNotify)g_bytes_unref);

  g_hash_table_iter_init (&iter, complete);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *)&der))
    {
      g_assert (G_TLS_DATABASE_GNUTLS_GET_CLASS (self)->create_handle_for_certificate);
      handle = G_TLS_DATABASE_GNUTLS_GET_CLASS (self)->create_handle_for_certificate (self, der);
      if (handle)
        g_hash_table_insert (handles, handle, g_bytes_ref (der));
    }

  return handles;
}

static void
initialize_tables (gnutls_x509_trust_list_t  trust_list,
                   GHashTable               *subjects,
                   GHashTable               *issuers,
                   GHashTable               *complete)
{
  gnutls_x509_trust_list_iter_t iter = NULL;
  gnutls_x509_crt_t cert = NULL;
  gnutls_datum_t dn;
  GBytes *der = NULL;
  GBytes *subject = NULL;
  GBytes *issuer = NULL;
  gint gerr;

  while (gnutls_x509_trust_list_iter_get_ca (trust_list, &iter, &cert) == 0)
    {
      gerr = gnutls_x509_crt_get_raw_dn (cert, &dn);
      if (gerr < 0)
        {
          g_warning ("failed to get subject of anchor certificate: %s",
                     gnutls_strerror (gerr));
          goto next;
        }
      subject = g_bytes_new_with_free_func (dn.data, dn.size, gnutls_free, dn.data);

      gerr = gnutls_x509_crt_get_raw_issuer_dn (cert, &dn);
      if (gerr < 0)
        {
          g_warning ("failed to get issuer of anchor certificate: %s",
                     gnutls_strerror (gerr));
          goto next;
        }
      issuer = g_bytes_new_with_free_func (dn.data, dn.size, gnutls_free, dn.data);

      gerr = gnutls_x509_crt_export2 (cert, GNUTLS_X509_FMT_DER, &dn);
      if (gerr < 0)
        {
          g_warning ("failed to get certificate DER: %s",
                     gnutls_strerror (gerr));
          goto next;
        }
      der = g_bytes_new_with_free_func (dn.data, dn.size, gnutls_free, dn.data);

      /* Three different ways of looking up same certificate */
      bytes_multi_table_insert (subjects, subject, der);
      bytes_multi_table_insert (issuers, issuer, der);

      g_hash_table_insert (complete, g_bytes_ref (der),
                           g_bytes_ref (der));

next:
      g_clear_pointer (&der, g_bytes_unref);
      g_clear_pointer (&subject, g_bytes_unref);
      g_clear_pointer (&issuer, g_bytes_unref);
      g_clear_pointer (&cert, gnutls_x509_crt_deinit);
    }
}

static void
g_tls_database_gnutls_finalize (GObject *object)
{
  GTlsDatabaseGnutls *self = G_TLS_DATABASE_GNUTLS (object);
  GTlsDatabaseGnutlsPrivate *priv = g_tls_database_gnutls_get_instance_private (self);

  g_assert (!priv->verify_chain_cancellable);

  g_clear_pointer (&priv->subjects, g_hash_table_destroy);
  g_clear_pointer (&priv->issuers, g_hash_table_destroy);
  g_clear_pointer (&priv->complete, g_hash_table_destroy);
  g_clear_pointer (&priv->handles, g_hash_table_destroy);

  gnutls_x509_trust_list_deinit (priv->trust_list, 1);

  g_mutex_clear (&priv->mutex);

  G_OBJECT_CLASS (g_tls_database_gnutls_parent_class)->finalize (object);
}

static void
g_tls_database_gnutls_init (GTlsDatabaseGnutls *self)
{
  GTlsDatabaseGnutlsPrivate *priv = g_tls_database_gnutls_get_instance_private (self);

  g_mutex_init (&priv->mutex);
}

static gchar *
g_tls_database_gnutls_create_certificate_handle (GTlsDatabase    *database,
                                                 GTlsCertificate *certificate)
{
  GTlsDatabaseGnutls *self = G_TLS_DATABASE_GNUTLS (database);
  GTlsDatabaseGnutlsPrivate *priv = g_tls_database_gnutls_get_instance_private (self);
  GBytes *der;
  gboolean contains;
  gchar *handle = NULL;

  der = g_tls_certificate_gnutls_get_bytes (G_TLS_CERTIFICATE_GNUTLS (certificate));
  g_return_val_if_fail (der, FALSE);

  g_mutex_lock (&priv->mutex);

  /* At the same time look up whether this certificate is in list */
  contains = g_hash_table_lookup (priv->complete, der) ? TRUE : FALSE;

  g_mutex_unlock (&priv->mutex);

  /* Certificate is in the database */
  if (contains)
    {
      g_assert (G_TLS_DATABASE_GNUTLS_GET_CLASS (self)->create_handle_for_certificate);
      handle = G_TLS_DATABASE_GNUTLS_GET_CLASS (self)->create_handle_for_certificate (self, der);
    }

  g_bytes_unref (der);
  return handle;
}

static GTlsCertificate *
g_tls_database_gnutls_lookup_certificate_for_handle (GTlsDatabase             *database,
                                                     const gchar              *handle,
                                                     GTlsInteraction          *interaction,
                                                     GTlsDatabaseLookupFlags   flags,
                                                     GCancellable             *cancellable,
                                                     GError                  **error)
{
  GTlsDatabaseGnutls *self = G_TLS_DATABASE_GNUTLS (database);
  GTlsDatabaseGnutlsPrivate *priv = g_tls_database_gnutls_get_instance_private (self);
  GTlsCertificate *cert;
  GBytes *der;
  gnutls_datum_t datum;
  gsize length;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return NULL;

  if (!handle)
    return NULL;

  g_mutex_lock (&priv->mutex);

  /* Create the handles table if not already done */
  if (!priv->handles)
    priv->handles = create_handles_array_unlocked (self, priv->complete);

  der = g_hash_table_lookup (priv->handles, handle);
  if (der)
    g_bytes_ref (der);

  g_mutex_unlock (&priv->mutex);

  if (!der)
    return NULL;

  datum.data = (unsigned char *)g_bytes_get_data (der, &length);
  datum.size = length;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    cert = NULL;
  else
    cert = g_tls_certificate_gnutls_new (&datum, NULL);

  g_bytes_unref (der);
  return cert;
}

static GTlsCertificate *
g_tls_database_gnutls_lookup_certificate_issuer (GTlsDatabase             *database,
                                                 GTlsCertificate          *certificate,
                                                 GTlsInteraction          *interaction,
                                                 GTlsDatabaseLookupFlags   flags,
                                                 GCancellable             *cancellable,
                                                 GError                  **error)
{
  GTlsDatabaseGnutls *self = G_TLS_DATABASE_GNUTLS (database);
  GTlsDatabaseGnutlsPrivate *priv = g_tls_database_gnutls_get_instance_private (self);
  gnutls_datum_t dn = { NULL, 0 };
  GBytes *subject, *der;
  gnutls_datum_t datum;
  GTlsCertificate *issuer = NULL;
  gnutls_x509_crt_t cert;
  gsize length;
  int gerr;

  g_return_val_if_fail (G_IS_TLS_CERTIFICATE_GNUTLS (certificate), NULL);

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return NULL;

  if (flags & G_TLS_DATABASE_LOOKUP_KEYPAIR)
    return NULL;

  /* Dig out the issuer of this certificate */
  cert = g_tls_certificate_gnutls_get_cert (G_TLS_CERTIFICATE_GNUTLS (certificate));
  gerr = gnutls_x509_crt_get_raw_issuer_dn (cert, &dn);
  if (gerr < 0)
    {
      g_warning ("failed to get issuer of certificate: %s", gnutls_strerror (gerr));
      return NULL;
    }

  subject = g_bytes_new_with_free_func (dn.data, dn.size, gnutls_free, dn.data);

  /* Find the full DER value of the certificate */
  g_mutex_lock (&priv->mutex);
  der = bytes_multi_table_lookup_ref_one (priv->subjects, subject);
  g_mutex_unlock (&priv->mutex);

  g_bytes_unref (subject);

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    {
      issuer = NULL;
    }
  else if (der)
    {
      datum.data = (unsigned char *)g_bytes_get_data (der, &length);
      datum.size = length;
      issuer = g_tls_certificate_gnutls_new (&datum, NULL);
    }

  if (der)
    g_bytes_unref (der);
  return issuer;
}

static GList *
g_tls_database_gnutls_lookup_certificates_issued_by (GTlsDatabase             *database,
                                                     GByteArray               *issuer_raw_dn,
                                                     GTlsInteraction          *interaction,
                                                     GTlsDatabaseLookupFlags   flags,
                                                     GCancellable             *cancellable,
                                                     GError                  **error)
{
  GTlsDatabaseGnutls *self = G_TLS_DATABASE_GNUTLS (database);
  GTlsDatabaseGnutlsPrivate *priv = g_tls_database_gnutls_get_instance_private (self);
  GBytes *issuer;
  gnutls_datum_t datum;
  GList *issued = NULL;
  GList *ders;
  gsize length;
  GList *l;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return NULL;

  /* We don't have any private keys here */
  if (flags & G_TLS_DATABASE_LOOKUP_KEYPAIR)
    return NULL;

  issuer = g_bytes_new_static (issuer_raw_dn->data, issuer_raw_dn->len);

  /* Find the full DER value of the certificate */
  g_mutex_lock (&priv->mutex);
  ders = bytes_multi_table_lookup_ref_all (priv->issuers, issuer);
  g_mutex_unlock (&priv->mutex);

  g_bytes_unref (issuer);

  for (l = ders; l; l = g_list_next (l))
    {
      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        {
          g_list_free_full (issued, g_object_unref);
          issued = NULL;
          break;
        }

      datum.data = (unsigned char *)g_bytes_get_data (l->data, &length);
      datum.size = length;
      issued = g_list_prepend (issued, g_tls_certificate_gnutls_new (&datum, NULL));
    }

  g_list_free_full (ders, (GDestroyNotify)g_bytes_unref);
  return issued;
}

typedef struct {
  gnutls_x509_crt_t *chain;
  guint              length;
} CertificateChain;

static CertificateChain *
certificate_chain_new (void)
{
  return g_new0 (CertificateChain, 1);
}

static void
certificate_chain_free (CertificateChain *chain)
{
  g_free (chain->chain);
  g_free (chain);
}

static CertificateChain *
convert_certificate_chain_to_gnutls (GTlsCertificateGnutls *chain)
{
  GTlsCertificate *cert;
  CertificateChain *gnutls_chain;
  guint i = 0;

  gnutls_chain = certificate_chain_new ();

  for (cert = G_TLS_CERTIFICATE (chain); cert; cert = g_tls_certificate_get_issuer (cert))
    gnutls_chain->length++;

  gnutls_chain->chain = g_new (gnutls_x509_crt_t, gnutls_chain->length);

  for (cert = G_TLS_CERTIFICATE (chain); cert; cert = g_tls_certificate_get_issuer (cert), i++)
    gnutls_chain->chain[i] = g_tls_certificate_gnutls_get_cert (G_TLS_CERTIFICATE_GNUTLS (cert));

  g_assert (i == gnutls_chain->length);

  return gnutls_chain;
}

static GTlsCertificateFlags
g_tls_database_gnutls_verify_chain (GTlsDatabase             *database,
                                    GTlsCertificate          *chain,
                                    const gchar              *purpose,
                                    GSocketConnectable       *identity,
                                    GTlsInteraction          *interaction,
                                    GTlsDatabaseVerifyFlags   flags,
                                    GCancellable             *cancellable,
                                    GError                  **error)
{
  GTlsDatabaseGnutls *self = G_TLS_DATABASE_GNUTLS (database);
  GTlsDatabaseGnutlsPrivate *priv = g_tls_database_gnutls_get_instance_private (self);
  GTlsCertificateFlags result;
  guint gnutls_result;
  CertificateChain *gnutls_chain;
  const char *hostname = NULL;
  char *free_hostname = NULL;
  int gerr;

  g_return_val_if_fail (G_IS_TLS_CERTIFICATE_GNUTLS (chain),
                        G_TLS_CERTIFICATE_GENERIC_ERROR);
  g_assert (purpose);

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return G_TLS_CERTIFICATE_GENERIC_ERROR;

  g_assert (!priv->verify_chain_cancellable);
  priv->verify_chain_cancellable = cancellable;
  gnutls_chain = convert_certificate_chain_to_gnutls (G_TLS_CERTIFICATE_GNUTLS (chain));
  gerr = gnutls_x509_trust_list_verify_crt (priv->trust_list,
                                            gnutls_chain->chain, gnutls_chain->length,
                                            0, &gnutls_result, NULL);
  priv->verify_chain_cancellable = NULL;

  if (gerr != 0 || g_cancellable_set_error_if_cancelled (cancellable, error))
    {
      certificate_chain_free (gnutls_chain);
      return G_TLS_CERTIFICATE_GENERIC_ERROR;
    }

  result = g_tls_certificate_gnutls_convert_flags (gnutls_result);

  if (G_IS_NETWORK_ADDRESS (identity))
    hostname = g_network_address_get_hostname (G_NETWORK_ADDRESS (identity));
  else if (G_IS_NETWORK_SERVICE (identity))
    hostname = g_network_service_get_domain (G_NETWORK_SERVICE (identity));
  else if (G_IS_INET_SOCKET_ADDRESS (identity))
    {
      GInetAddress *addr;

      addr = g_inet_socket_address_get_address (G_INET_SOCKET_ADDRESS (identity));
      hostname = free_hostname = g_inet_address_to_string (addr);
    }
  if (hostname)
    {
      if (!gnutls_x509_crt_check_hostname (gnutls_chain->chain[0], hostname))
        result |= G_TLS_CERTIFICATE_BAD_IDENTITY;
      g_free (free_hostname);
    }

  certificate_chain_free (gnutls_chain);
  return result;
}

static gchar *
g_tls_database_gnutls_create_handle_for_certificate (GTlsDatabaseGnutls *self,
                                                     GBytes             *der)
{
  gchar *bookmark;
  gchar *uri;

  /*
   * Here we create a URI that looks like
   * system-trust:#11b2641821252596420e468c275771f5e51022c121a17bd7a89a2f37b6336c8f.
   *
   * system-trust is a meaningless URI scheme, and the handle does not
   * even need to be a URI; this is just a nice stable way to uniquely
   * identify a certificate.
   */

  bookmark = g_compute_checksum_for_bytes (G_CHECKSUM_SHA256, der);
  uri = g_strconcat ("system-trust:#", bookmark, NULL);

  g_free (bookmark);

  return uri;
}

static gboolean
g_tls_database_gnutls_populate_trust_list (GTlsDatabaseGnutls        *self,
                                           gnutls_x509_trust_list_t   trust_list,
                                           GError                   **error)
{
  int gerr = gnutls_x509_trust_list_add_system_trust (trust_list, 0, 0);
  if (gerr == GNUTLS_E_UNIMPLEMENTED_FEATURE)
    {
      g_set_error_literal (error, G_TLS_ERROR, G_TLS_ERROR_MISC,
                           _("Failed to load system trust store: GnuTLS was not configured with a system trust"));
    }
  else if (gerr < 0)
    {
      g_set_error (error, G_TLS_ERROR, G_TLS_ERROR_MISC,
                   _("Failed to load system trust store: %s"),
                   gnutls_strerror (gerr));
    }
  return gerr >= 0;
}

#if GNUTLS_VERSION_MAJOR > 3 || GNUTLS_VERSION_MAJOR == 3 && GNUTLS_VERSION_MINOR >= 7
static int
issuer_missing_cb (gnutls_x509_trust_list_t   tlist,
                   const gnutls_x509_crt_t    crt,
                   gnutls_x509_crt_t        **issuers,
                   guint                     *issuers_size)
{
  GTlsDatabaseGnutls *self = gnutls_x509_trust_list_get_ptr (tlist);
  GTlsDatabaseGnutlsPrivate *priv = g_tls_database_gnutls_get_instance_private (self);
  gnutls_datum_t datum;
  GFile *file = NULL;
  GFileInputStream *istream = NULL;
  char *aia = NULL;
  char *scheme = NULL;
  int gerr;
  int ret = -1;
  guchar buffer[2048];
  gssize n_read;
  GByteArray *der = NULL;
  GError *error = NULL;

  /* The server sent an incomplete certificate chain, but we may be able to
   * download the missing certificate to allow verification to proceed. See
   * Authority Information Access, RFC 5280 §4.2.2.1. Also see:
   * https://blogs.gnome.org/mcatanzaro/2015/01/30/mozilla-is-responsible-for-the-redhat-corpmerchandise-com-fiasco/
   */

  for (int i = 0; ; i++)
    {
      gerr = gnutls_x509_crt_get_authority_info_access (crt, i, GNUTLS_IA_CAISSUERS_URI, &datum, NULL);
      if (gerr == GNUTLS_E_UNKNOWN_ALGORITHM)
        continue;

      if (gerr == GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE)
        return -1;

      if (gerr < 0)
        {
          g_warning ("Failed to read Authority Information Access from certificate: %s", gnutls_strerror (gerr));
          return -1;
        }

      /* Success */
      break;
    }
  g_assert (gerr == GNUTLS_E_SUCCESS);

  aia = g_malloc0 (datum.size + 1);
  memcpy (aia, datum.data, datum.size);

  if (!g_uri_is_valid (aia, G_URI_FLAGS_NONE, &error))
    {
      g_warning ("Authority Information Access URI %s is not a valid URI: %s", aia, error->message);
      goto out;
    }

  /* We support only HTTP. Notably, HTTPS is not supported because (a) it is
   * not specified by RFC 5280, and (b) since we have no way to break a
   * recursive loop if the connection to retrieve the certificate itself also
   * requires a missing certificate. We could easily support FTP, but we don't,
   * because that's silly. Also note that we don't support "certs-only" CMS
   * messages, we only support directly retrieving a DER certificate. Finally,
   * we don't support the case where accessLocation is a directoryName, so no
   * private DAP or LDAP.
   */
  scheme = g_uri_parse_scheme (aia);
  if (!scheme || strcmp (scheme, "http") != 0)
    {
      g_warning ("Authority Information Access URI %s uses unsupported URI scheme '%s'", scheme, aia);
      goto out;
    }

  file = g_file_new_for_uri (aia);
  istream = g_file_read (file, priv->verify_chain_cancellable, &error);
  if (!istream)
    {
      g_warning ("Failed to download missing issuer certificate from Authority Information Access URI %s: failed g_file_read (do you need to install gvfs?): %s",
                 aia, error->message);
      goto out;
    }

  der = g_byte_array_sized_new (sizeof (buffer));
  do
    {
      n_read = g_input_stream_read (G_INPUT_STREAM (istream), buffer, sizeof (buffer),
                                    priv->verify_chain_cancellable, &error);
      if (n_read == -1)
        {
          g_warning ("Failed to download missing issuer certificate from Authority Information Access URI %s: failed g_input_stream_read: %s",
                     aia, error->message);
          goto out;
        }
      g_byte_array_append (der, buffer, n_read);
    } while (n_read > 0);

  gnutls_free (datum.data);
  datum.size = der->len;
  datum.data = (unsigned char *)g_byte_array_free (der, FALSE);
  der = NULL;

  gerr = gnutls_x509_crt_list_import2 (issuers, issuers_size, &datum, GNUTLS_X509_FMT_DER, 0);
  if (gerr < 0)
    {
      g_warning ("Failed to download missing issuer certificate from Authority Information Access URI %s: failed gnutls_x509_crt_import: %s",
                 aia, gnutls_strerror (gerr));
      goto out;
    }

  ret = 0;

out:
  if (error)
    g_error_free (error);
  if (file)
    g_object_unref (file);
  if (istream)
    g_object_unref (istream);
  if (der)
    g_byte_array_unref (der);
  gnutls_free (datum.data);
  g_free (aia);
  return ret;
}
#endif

static void
g_tls_database_gnutls_class_init (GTlsDatabaseGnutlsClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GTlsDatabaseClass *database_class = G_TLS_DATABASE_CLASS (klass);

  gobject_class->finalize     = g_tls_database_gnutls_finalize;

  database_class->create_certificate_handle = g_tls_database_gnutls_create_certificate_handle;
  database_class->lookup_certificate_for_handle = g_tls_database_gnutls_lookup_certificate_for_handle;
  database_class->lookup_certificate_issuer = g_tls_database_gnutls_lookup_certificate_issuer;
  database_class->lookup_certificates_issued_by = g_tls_database_gnutls_lookup_certificates_issued_by;
  database_class->verify_chain = g_tls_database_gnutls_verify_chain;

  klass->create_handle_for_certificate = g_tls_database_gnutls_create_handle_for_certificate;
  klass->populate_trust_list = g_tls_database_gnutls_populate_trust_list;
}

static gboolean
g_tls_database_gnutls_initable_init (GInitable     *initable,
                                     GCancellable  *cancellable,
                                     GError       **error)
{
  GTlsDatabaseGnutls *self = G_TLS_DATABASE_GNUTLS (initable);
  GTlsDatabaseGnutlsPrivate *priv = g_tls_database_gnutls_get_instance_private (self);
  gnutls_x509_trust_list_t trust_list = NULL;
  GHashTable *subjects = NULL;
  GHashTable *issuers = NULL;
  GHashTable *complete = NULL;
  gboolean result = TRUE;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  gnutls_x509_trust_list_init (&trust_list, 0);
#if GNUTLS_VERSION_MAJOR > 3 || GNUTLS_VERSION_MAJOR == 3 && GNUTLS_VERSION_MINOR >= 7
  gnutls_x509_trust_list_set_getissuer_function (trust_list, issuer_missing_cb);
  gnutls_x509_trust_list_set_ptr (trust_list, self);
#endif

  g_assert (G_TLS_DATABASE_GNUTLS_GET_CLASS (self)->populate_trust_list);
  if (!G_TLS_DATABASE_GNUTLS_GET_CLASS (self)->populate_trust_list (self, trust_list, error))
    {
      result = FALSE;
      goto out;
    }

  subjects = bytes_multi_table_new ();
  issuers = bytes_multi_table_new ();

  complete = g_hash_table_new_full (g_bytes_hash, g_bytes_equal,
                                    (GDestroyNotify)g_bytes_unref,
                                    (GDestroyNotify)g_bytes_unref);

  initialize_tables (trust_list, subjects, issuers, complete);

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    result = FALSE;

  if (result)
    {
      g_mutex_lock (&priv->mutex);
      if (!priv->trust_list)
        {
          priv->trust_list = trust_list;
          trust_list = NULL;
        }
      if (!priv->subjects)
        {
          priv->subjects = subjects;
          subjects = NULL;
        }
      if (!priv->issuers)
        {
          priv->issuers = issuers;
          issuers = NULL;
        }
      if (!priv->complete)
        {
          priv->complete = complete;
          complete = NULL;
        }
      g_mutex_unlock (&priv->mutex);
    }

out:
  if (trust_list)
    gnutls_x509_trust_list_deinit (trust_list, 1);
  if (subjects)
    g_hash_table_unref (subjects);
  if (issuers)
    g_hash_table_unref (issuers);
  if (complete)
    g_hash_table_unref (complete);
  return result;
}

static void
g_tls_database_gnutls_initable_interface_init (GInitableIface *iface)
{
  iface->init = g_tls_database_gnutls_initable_init;
}

GTlsDatabaseGnutls *
g_tls_database_gnutls_new (GError **error)
{
  g_return_val_if_fail (!error || !*error, NULL);

  return g_initable_new (G_TYPE_TLS_DATABASE_GNUTLS, NULL, error, NULL);
}
