/*
 * Copyright(c) 2012 Tim Ruehsen
 *
 * This file is part of MGet.
 *
 * Mget is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mget is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Mget.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * gnutls SSL/TLS routines
 *
 * Changelog
 * 03.08.2012  Tim Ruehsen  created
 *
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>

#include "log.h"
#include "options.h"
#include "gnutls.h"

static gnutls_certificate_credentials_t
	credentials;

static char
	check_certificate;

void ssl_set_check_certificate(char value)
{
	check_certificate = value;
}

/* This function will verify the peer's certificate, and check
 * if the hostname matches, as well as the activation, expiration dates.
 */
static int _verify_certificate_callback(gnutls_session_t session)
{
	unsigned int status;
	const gnutls_datum_t *cert_list;
	unsigned int cert_list_size;
	int ret = 0, err;
	gnutls_x509_crt_t cert;
	const char *hostname;
	const char *tag = check_certificate ? _("ERROR") : _("WARNING");

	// read hostname
	hostname = gnutls_session_get_ptr(session);

	/* This verification function uses the trusted CAs in the credentials
	 * structure. So you must have installed one or more CA certificates.
	 */
	if (gnutls_certificate_verify_peers2(session, &status) < 0) {
		err_printf(_("%s: Certificate verification error\n"), tag);
		ret = -1;
		goto out;
	}

	if (status) {
		if (status & GNUTLS_CERT_INVALID)
			err_printf(_("%s: The certificate is not trusted.\n"), tag);
		if (status & GNUTLS_CERT_REVOKED)
			err_printf(_("%s: The certificate has been revoked.\n"), tag);
		if (status & GNUTLS_CERT_SIGNER_NOT_FOUND)
			err_printf(_("%s: The certificate hasn't got a known issuer.\n"), tag);
		if (status & GNUTLS_CERT_SIGNER_NOT_CA)
			err_printf(_("%s: The certificate signer was not a CA.\n"), tag);
		if (status & GNUTLS_CERT_INSECURE_ALGORITHM)
			err_printf(_("%s: The certificate was signed using an insecure algorithm\n"), tag);
		if (status & GNUTLS_CERT_NOT_ACTIVATED)
			err_printf(_("%s: The certificate is not yet activated\n"), tag);
		if (status & GNUTLS_CERT_EXPIRED)
			err_printf(_("%s: The certificate has expired\n"), tag);

		ret = -1;
		goto out;
	}

	/* Up to here the process is the same for X.509 certificates and
	 * OpenPGP keys. From now on X.509 certificates are assumed. This can
	 * be easily extended to work with openpgp keys as well.
	 */
	if (gnutls_certificate_type_get(session) != GNUTLS_CRT_X509) {
		err_printf(_("%s: Certificate must be X.509\n"), tag);
		ret = -1;
		goto out;
	}

	if (gnutls_x509_crt_init(&cert) < 0) {
		err_printf(_("%s: Error initializing X.509 certificate\n"), tag);
		ret = -1;
		goto out;
	}

	if ((cert_list = gnutls_certificate_get_peers(session, &cert_list_size)) == NULL) {
		err_printf(_("%s: No certificate was found!\n"), tag);
		ret = -1;
	} else if ((err = gnutls_x509_crt_import(cert, &cert_list[0], GNUTLS_X509_FMT_DER)) < 0) {
		err_printf(_("%s: Failed to parse certificate: %s\n"), tag, gnutls_strerror (err));
		ret = -1;
	} else {
		time_t now = time(NULL);

		if (now < gnutls_x509_crt_get_activation_time (cert)) {
			err_printf (_("%s: The certificate is not yet activated\n"), tag);
			ret = -1;
		}
		if (now >= gnutls_x509_crt_get_expiration_time (cert)) {
			err_printf (_("%s: The certificate has expired\n"), tag);
			ret = -1;
		}
	}

	if (!gnutls_x509_crt_check_hostname(cert, hostname)) {
		err_printf(_("%s: The certificate's owner does not match hostname '%s'\n"), tag, hostname);
		ret = -1;
	}

	gnutls_x509_crt_deinit(cert);

	// 0: continue handshake
	// else: stop handshake
out:
	return check_certificate ? ret : 0;
}

void ssl_init(void)
{
	static int init;
	static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

	pthread_mutex_lock(&mutex);

	if (!init) {
		init = 1;

		gnutls_global_init();
		gnutls_certificate_allocate_credentials(&credentials);
		gnutls_certificate_set_x509_trust_file(credentials, "/etc/ssl/certs/ca-certificates.crt", GNUTLS_X509_FMT_PEM);
		gnutls_certificate_set_verify_function(credentials, _verify_certificate_callback);
	}

	pthread_mutex_unlock(&mutex);
}

void ssl_deinit(void)
{
	gnutls_certificate_free_credentials(credentials);
	gnutls_global_deinit();
}

static int ready_2_transfer(gnutls_session_t session, int timeout, int mode)
{
	// 0: no timeout / immediate
	// -1: INFINITE timeout
	if (timeout) {
		int sockfd = (int)(ptrdiff_t)gnutls_transport_get_ptr(session);
		int rc;

		// wait for socket to be ready to read
		struct pollfd pollfd[1] = {
			{ sockfd, mode, 0}};

		if ((rc = poll(pollfd, 1, timeout)) <= 0)
			return rc < 0 ? -1 : 0;

		if (!(pollfd[0].revents & mode))
			return -1;
	}

	return 1;
}

static int ready_2_read(gnutls_session_t session, int timeout)
{
	return ready_2_transfer(session, timeout, POLLIN);
}

static int ready_2_write(gnutls_session_t session, int timeout)
{
	return ready_2_transfer(session, timeout, POLLOUT);
}

void *ssl_open(int sockfd, const char *hostname)
{
	gnutls_session_t session;
	const char *err;
	int ret;

	ssl_init();

	gnutls_init(&session, GNUTLS_CLIENT | GNUTLS_NONBLOCK);
	gnutls_session_set_ptr(session, (void *)hostname);
	gnutls_server_name_set(session, GNUTLS_NAME_DNS, hostname, strlen(hostname));
	gnutls_priority_set_direct(session, "NORMAL", &err);
	gnutls_credentials_set(session, GNUTLS_CRD_CERTIFICATE, credentials);
	gnutls_transport_set_ptr(session, (gnutls_transport_ptr_t)(ptrdiff_t)sockfd);

	// Perform the TLS handshake
	for (;;) {
		ret = gnutls_handshake(session);
		if (ret == 0 || gnutls_error_is_fatal(ret)) {
			if (ret == 0)
				ret = 1;
			break;
		}

		if (gnutls_record_get_direction(session)) {
			// wait for writeability
			ret = ready_2_write(session, config.connect_timeout);
		} else {
			// wait for readability
			ret = ready_2_read(session, config.connect_timeout);
		}

		if (ret <= 0)
			break;
	}

	if (ret <= 0) {
		if (ret)
			log_printf("Handshake failed (%d)\n", ret);
		else
			log_printf("Handshake timed out\n");
		gnutls_perror(ret);
		ssl_close((void **)&session);
		session = NULL;
	} else {
		log_printf("Handshake completed\n");
	}

	return session;
}

void ssl_close(void **session)
{
	gnutls_session_t s = *session;

	if (s) {
		gnutls_bye(s, GNUTLS_SHUT_RDWR);
		gnutls_deinit(s);
		*session = NULL;
	}
}

ssize_t ssl_read_timeout(void *session, char *buf, size_t count, int timeout)
{
	ssize_t nbytes, x=55, y=66;

	for (;;) {
		if ((x=gnutls_record_check_pending(session)) <= 0 && (y=ready_2_read(session, timeout)) <= 0) {
			log_printf("return 0 %zd %zd\n",x,y);
			return 0;
		}

		nbytes=gnutls_record_recv(session, buf, count);

		if (nbytes >= 0 || nbytes != GNUTLS_E_AGAIN)
			break;
	}

	log_printf("return %zd\n",nbytes < -1 ? -1 : nbytes);
	return nbytes < -1 ? -1 : nbytes;
}

ssize_t ssl_write(void *session, const char *buf, size_t count)
{
	return gnutls_record_send(session, buf, count);
}