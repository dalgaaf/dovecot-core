/* Copyright (c) 2010 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "mail-host.h"
#include "user-directory.h"
#include "director.h"
#include "director-request.h"

#define DIRECTOR_REQUEST_TIMEOUT_SECS 30

struct director_request {
	struct director *dir;

	time_t create_time;
	unsigned int username_hash;

	director_request_callback *callback;
	void *context;
};

static void director_request_timeout(struct director *dir)
{
	struct director_request **requestp, *request;

	while (array_count(&dir->pending_requests) > 0) {
		requestp = array_idx_modifiable(&dir->pending_requests, 0);
		request = *requestp;

		if (request->create_time +
		    DIRECTOR_REQUEST_TIMEOUT_SECS > ioloop_time)
			break;

		array_delete(&dir->pending_requests, 0, 1);
		request->callback(NULL, request->context);
		i_free(request);
	}

	if (array_count(&dir->pending_requests) == 0 && dir->to_request != NULL)
		timeout_remove(&dir->to_request);
}

void director_request(struct director *dir, const char *username,
		      director_request_callback *callback, void *context)
{
	struct director_request *request;
	unsigned int username_hash = user_directory_get_username_hash(username);

	request = i_new(struct director_request, 1);
	request->dir = dir;
	request->create_time = ioloop_time;
	request->username_hash = username_hash;
	request->callback = callback;
	request->context = context;

	if (director_request_continue(request))
		return;

	/* need to queue it */
	if (dir->to_request == NULL) {
		dir->to_request =
			timeout_add(DIRECTOR_REQUEST_TIMEOUT_SECS * 1000,
				    director_request_timeout, dir);
	}
	array_append(&dir->pending_requests, &request, 1);
}

bool director_request_continue(struct director_request *request)
{
	struct director *dir = request->dir;
	struct mail_host *host;
	struct user *user;

	if (!dir->ring_handshaked) {
		/* delay requests until ring handshaking is complete */
		if (!dir->ring_handshake_warning_sent) {
			i_warning("Delaying requests until all "
				  "directors have connected");
			dir->ring_handshake_warning_sent = TRUE;
		}
		return FALSE;
	}

	user = user_directory_lookup(dir->users, request->username_hash);
	if (user != NULL)
		user_directory_refresh(dir->users, user);
	else {
		if (!dir->ring_synced) {
			/* delay adding new users until ring is again synced */
			if (dir->debug)
				i_debug("Delaying request until ring is synced");
			return FALSE;
		}
		host = mail_host_get_by_hash(dir->mail_hosts,
					     request->username_hash);
		if (host == NULL) {
			/* all hosts have been removed */
			return FALSE;
		}
		user = user_directory_add(dir->users, request->username_hash,
					  host, ioloop_time);
	}

	director_update_user(dir, dir->self_host, user);
	request->callback(&user->host->ip, request->context);
	i_free(request);
	return TRUE;
}
