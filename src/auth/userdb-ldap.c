/* Copyright (C) 2003 Timo Sirainen */

#include "config.h"
#undef HAVE_CONFIG_H

#ifdef USERDB_LDAP

#include "common.h"
#include "str.h"
#include "var-expand.h"
#include "db-ldap.h"
#include "userdb.h"

#include <ldap.h>
#include <stdlib.h>

/* using posixAccount */
#define DEFAULT_ATTRIBUTES "uid,homeDirectory,,,uidNumber,gidNumber"

enum ldap_user_attr {
	ATTR_VIRTUAL_USER = 0,
	ATTR_HOME,
	ATTR_MAIL,
	ATTR_SYSTEM_USER,
	ATTR_UID_NUMBER,
	ATTR_GID_NUMBER,

	ATTR_COUNT
};

struct userdb_ldap_connection {
	struct ldap_connection *conn;

        unsigned int *attrs;
	char **attr_names;
};

struct userdb_ldap_request {
	struct ldap_request request;
        struct auth_request *auth_request;
        userdb_callback_t *userdb_callback;
};

static struct userdb_ldap_connection *userdb_ldap_conn;

static void parse_attr(struct auth_request *auth_request,
		       struct userdb_ldap_connection *conn,
		       struct user_data *user,
		       const char *attr, const char *value)
{
	enum ldap_user_attr i;

	for (i = 0; i < ATTR_COUNT; i++) {
		if (strcasecmp(conn->attr_names[i], attr) == 0)
			break;
	}

	if (i == ATTR_COUNT) {
		i_error("ldap(%s): Unknown attribute '%s'",
			get_log_prefix(auth_request), attr);
		return;
	}

	switch (conn->attrs[i]) {
	case ATTR_VIRTUAL_USER:
		user->virtual_user = t_strdup(value);
		break;
	case ATTR_HOME:
		user->home = t_strdup(value);
		break;
	case ATTR_MAIL:
		user->mail = t_strdup(value);
		break;
	case ATTR_SYSTEM_USER:
		user->system_user = t_strdup(value);
		break;
	case ATTR_UID_NUMBER:
		user->uid = atoi(value);
		break;
	case ATTR_GID_NUMBER:
		user->gid = atoi(value);
		break;

	case ATTR_COUNT:
		break;
	}
}

static void handle_request(struct ldap_connection *conn,
			   struct ldap_request *request, LDAPMessage *res)
{
	struct userdb_ldap_request *urequest =
		(struct userdb_ldap_request *) request;
	struct auth_request *auth_request = urequest->auth_request;
	struct user_data user;
	LDAPMessage *entry;
	BerElement *ber;
	char *attr, **vals;
	int ret;

	ret = ldap_result2error(conn->ld, res, 0);
	if (ret != LDAP_SUCCESS) {
		i_error("ldap(%s): ldap_search() failed: %s",
			get_log_prefix(auth_request), ldap_err2string(ret));
		urequest->userdb_callback(NULL, request->context);
		return;
	}

	entry = res == NULL ? NULL : ldap_first_entry(conn->ld, res);
	if (entry == NULL) {
		if (res != NULL) {
			i_error("ldap(%s): Authenticated user not found",
				get_log_prefix(auth_request));
		}
		urequest->userdb_callback(NULL, request->context);
		return;
	}

	t_push();
	memset(&user, 0, sizeof(user));

	user.uid = conn->set.user_global_uid;
	user.gid = conn->set.user_global_gid;

	attr = ldap_first_attribute(conn->ld, entry, &ber);
	while (attr != NULL) {
		vals = ldap_get_values(conn->ld, entry, attr);
		if (vals != NULL && vals[0] != NULL && vals[1] == NULL) {
			parse_attr(auth_request, userdb_ldap_conn,
				   &user, attr, vals[0]);
		}
		ldap_value_free(vals);
		ldap_memfree(attr);

		attr = ldap_next_attribute(conn->ld, entry, ber);
	}

	if (user.virtual_user == NULL)
		i_error("ldap(%s): No username in reply",
			get_log_prefix(auth_request));
	else if (user.uid == (uid_t)-1) {
		i_error("ldap(%s): uidNumber not set and no default given in "
			"user_global_uid", get_log_prefix(auth_request));
	} else if (user.gid == (gid_t)-1) {
		i_error("ldap(%s): gidNumber not set and no default given in "
			"user_global_gid", get_log_prefix(auth_request));
	} else if (ldap_next_entry(conn->ld, entry) != NULL) {
		i_error("ldap(%s): Multiple replies found for user",
			get_log_prefix(auth_request));
	} else {
		urequest->userdb_callback(&user, request->context);
		t_pop();
		return;
	}

	/* error */
	urequest->userdb_callback(NULL, request->context);
	t_pop();
}

static void userdb_ldap_lookup(struct auth_request *auth_request,
			       userdb_callback_t *callback, void *context)
{
	struct ldap_connection *conn = userdb_ldap_conn->conn;
	struct userdb_ldap_request *request;
	const char *filter;
	string_t *str;

	if (conn->set.user_filter == NULL) {
		filter = t_strdup_printf("(&(objectClass=posixAccount)(%s=%s))",
			userdb_ldap_conn->attr_names[ATTR_VIRTUAL_USER],
			ldap_escape(auth_request->user));
	} else {
		str = t_str_new(512);
		var_expand(str, conn->set.user_filter,
			   auth_request_get_var_expand_table(auth_request,
							     ldap_escape));
		filter = str_c(str);
	}

	request = i_new(struct userdb_ldap_request, 1);
	request->request.callback = handle_request;
	request->request.context = context;
	request->auth_request = auth_request;
	request->userdb_callback = callback;

	if (verbose_debug) {
		i_info("ldap(%s): base=%s scope=%s filter=%s fields=%s",
		       get_log_prefix(auth_request), conn->set.base,
		       conn->set.scope, filter,
		       strarray_join((const char **)
				     userdb_ldap_conn->attr_names, ","));
	}

	db_ldap_search(conn, conn->set.base, conn->set.ldap_scope,
		       filter, userdb_ldap_conn->attr_names,
		       &request->request);
}

static void userdb_ldap_preinit(const char *args)
{
	struct ldap_connection *conn;

	userdb_ldap_conn = i_new(struct userdb_ldap_connection, 1);
	userdb_ldap_conn->conn = conn = db_ldap_init(args);

	db_ldap_set_attrs(conn, conn->set.user_attrs ?
			  conn->set.user_attrs : DEFAULT_ATTRIBUTES,
			  &userdb_ldap_conn->attrs,
			  &userdb_ldap_conn->attr_names);
}

static void userdb_ldap_init(const char *args __attr_unused__)
{
	(void)db_ldap_connect(userdb_ldap_conn->conn);
}

static void userdb_ldap_deinit(void)
{
	db_ldap_unref(userdb_ldap_conn->conn);
	i_free(userdb_ldap_conn);
}

struct userdb_module userdb_ldap = {
	"ldap",

	userdb_ldap_preinit,
	userdb_ldap_init,
	userdb_ldap_deinit,

	userdb_ldap_lookup
};

#endif
