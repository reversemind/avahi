/* $Id$ */

/***
  This file is part of avahi.
 
  avahi is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.
 
  avahi is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
  or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General
  Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with avahi; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib.h>
#include <string.h>

#define DBUS_API_SUBJECT_TO_CHANGE
#include <dbus/dbus.h>
#include <dbus/dbus-glib-lowlevel.h>

#include <avahi-core/llist.h>
#include <avahi-core/log.h>
#include <avahi-core/core.h>


#include "dbus-protocol.h"
#include "main.h"

#define AVAHI_DBUS_NAME "org.freedesktop.Avahi"
#define AVAHI_DBUS_INTERFACE_SERVER AVAHI_DBUS_NAME".Server"
#define AVAHI_DBUS_PATH_SERVER "/org/freedesktop/Avahi/Server"
#define AVAHI_DBUS_INTERFACE_ENTRY_GROUP AVAHI_DBUS_NAME".EntryGroup"


typedef struct Server Server;
typedef struct Client Client;
typedef struct EntryGroupInfo EntryGroupInfo;

struct EntryGroupInfo {
    guint id;
    Client *client;
    AvahiEntryGroup *entry_group;
    gchar *path;
    
    AVAHI_LLIST_FIELDS(EntryGroupInfo, entry_groups);
};

struct Client {
    guint id;
    gchar *name;
    guint current_id;
    
    AVAHI_LLIST_FIELDS(Client, clients);
    AVAHI_LLIST_HEAD(EntryGroupInfo, entry_groups);
};

struct Server {
    DBusConnection *bus;

    AVAHI_LLIST_HEAD(Client, clients);
    guint current_id;
};

static Server *server = NULL;

static void entry_group_free(EntryGroupInfo *i) {
    g_assert(i);
    
    avahi_entry_group_free(i->entry_group);
    dbus_connection_unregister_object_path(server->bus, i->path);
    g_free(i->path);
    AVAHI_LLIST_REMOVE(EntryGroupInfo, entry_groups, i->client->entry_groups, i);
    g_free(i);
 }

static void client_free(Client *c) {
    
    g_assert(server);
    g_assert(c);

    while (c->entry_groups)
        entry_group_free(c->entry_groups);
    
    g_free(c->name);
    AVAHI_LLIST_REMOVE(Client, clients, server->clients, c);
    g_free(c);
}

static Client *client_get(const gchar *name, gboolean create) {
    Client *client;

    g_assert(server);
    g_assert(name);

    for (client = server->clients; client; client = client->clients_next)
        if (!strcmp(name, client->name))
            return client;

    if (!create)
        return NULL;

    /* If not existant yet, create a new entry */
    client = g_new(Client, 1);
    client->id = server->current_id++;
    client->name = g_strdup(name);
    client->current_id = 0;
    AVAHI_LLIST_HEAD_INIT(Client, client->entry_groups);

    AVAHI_LLIST_PREPEND(Client, clients, server->clients, client);
    return client;
}

static DBusHandlerResult msg_signal_filter_impl(DBusConnection *c, DBusMessage *m, void *userdata) {
    GMainLoop *loop = userdata;
    DBusError error;

    dbus_error_init(&error);

/*     avahi_log_debug("dbus: interface=%s, path=%s, member=%s", */
/*                     dbus_message_get_interface(m), */
/*                     dbus_message_get_path(m), */
/*                     dbus_message_get_member(m)); */

    if (dbus_message_is_signal(m, DBUS_INTERFACE_LOCAL, "Disconnected")) {
        /* No, we shouldn't quit, but until we get somewhere
         * usefull such that we can restore our state, we will */
        avahi_log_warn("Disconnnected from d-bus, terminating...");
        g_main_loop_quit (loop);
        return DBUS_HANDLER_RESULT_HANDLED;
        
    } else if (dbus_message_is_signal(m, DBUS_INTERFACE_DBUS, "NameAcquired")) {
        gchar *name;

        if (!dbus_message_get_args(m, &error, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID)) {
            avahi_log_warn("Error parsing NameAcquired message");
            goto fail;
        }

        avahi_log_info("dbus: name acquired (%s)", name);
        return DBUS_HANDLER_RESULT_HANDLED;
    } else if (dbus_message_is_signal(m, DBUS_INTERFACE_DBUS, "NameOwnerChanged")) {
        gchar *name, *old, *new;

        if (!dbus_message_get_args(m, &error, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old, DBUS_TYPE_STRING, &new, DBUS_TYPE_INVALID)) {
            avahi_log_warn("Error parsing NameOwnerChanged message");
            goto fail;
        }

        if (!*new) {
            Client *client;

            if ((client = client_get(name, FALSE))) {
                avahi_log_info("dbus: client %s vanished", name);
                client_free(client);
            }
        }
    }

fail:
    if (dbus_error_is_set(&error))
        dbus_error_free(&error);
    
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusHandlerResult respond_error(DBusConnection *c, DBusMessage *m, const gchar *error, const gchar *text) {
    DBusMessage *reply;

    reply = dbus_message_new_error(m, error, text);
    dbus_connection_send(c, reply, NULL);
    dbus_message_unref(reply);
    
    return DBUS_HANDLER_RESULT_HANDLED;
}

static void entry_group_callback(AvahiServer *s, AvahiEntryGroup *g, AvahiEntryGroupState state, gpointer userdata) {
    EntryGroupInfo *i = userdata;
    DBusMessage *m;
    gint32 t;
    
    g_assert(s);
    g_assert(g);
    g_assert(i);

    m = dbus_message_new_signal(i->path, AVAHI_DBUS_INTERFACE_ENTRY_GROUP, "StateChanged");
    t = (gint32) state;
    dbus_message_append_args(m, DBUS_TYPE_INT32, &t, DBUS_TYPE_INVALID);
    dbus_message_set_destination(m, i->client->name);  
    dbus_connection_send(server->bus, m, NULL);
    dbus_message_unref(m);
}

static DBusHandlerResult respond_ok(DBusConnection *c, DBusMessage *m) {
    DBusMessage *reply;

    reply = dbus_message_new_method_return(m);
    dbus_connection_send(c, reply, NULL);
    dbus_message_unref(reply);
    
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult msg_entry_group_impl(DBusConnection *c, DBusMessage *m, void *userdata) {
    DBusError error;
    EntryGroupInfo *i = userdata;

    g_assert(c);
    g_assert(m);
    g_assert(i);
    
    dbus_error_init(&error);

    avahi_log_debug("dbus: interface=%s, path=%s, member=%s",
                    dbus_message_get_interface(m),
                    dbus_message_get_path(m),
                    dbus_message_get_member(m));

    /* Access control */
    if (strcmp(dbus_message_get_sender(m), i->client->name)) 
        return respond_error(c, m, DBUS_ERROR_ACCESS_DENIED, NULL);
    
    if (dbus_message_is_method_call(m, AVAHI_DBUS_INTERFACE_ENTRY_GROUP, "Free")) {

        if (!dbus_message_get_args(m, &error, DBUS_TYPE_INVALID)) {
            avahi_log_warn("Error parsing EntryGroup::Free message");
            goto fail;
        }

        entry_group_free(i);
        return respond_ok(c, m);
    } else if (dbus_message_is_method_call(m, AVAHI_DBUS_INTERFACE_ENTRY_GROUP, "Commit")) {

        if (!dbus_message_get_args(m, &error, DBUS_TYPE_INVALID)) {
            avahi_log_warn("Error parsing EntryGroup::Commit message");
            goto fail;
        }

        avahi_entry_group_commit(i->entry_group);
        return respond_ok(c, m);
    } else if (dbus_message_is_method_call(m, AVAHI_DBUS_INTERFACE_ENTRY_GROUP, "GetState")) {
        DBusMessage *reply;
        gint32 t;

        if (!dbus_message_get_args(m, &error, DBUS_TYPE_INVALID)) {
            avahi_log_warn("Error parsing EntryGroup::GetState message");
            goto fail;
        }

        t = (gint32) avahi_entry_group_get_state(i->entry_group);
        reply = dbus_message_new_method_return(m);
        dbus_message_append_args(reply, DBUS_TYPE_INT32, &t, DBUS_TYPE_INVALID);
        dbus_connection_send(c, reply, NULL);
        dbus_message_unref(reply);
        
        return DBUS_HANDLER_RESULT_HANDLED;
    } else if (dbus_message_is_method_call(m, AVAHI_DBUS_INTERFACE_ENTRY_GROUP, "AddService")) {
        gint32 interface, protocol;
        gchar *type, *name, *domain, *host;
        guint16 port;
        gchar **txt = NULL;
        gint txt_len;
        AvahiStringList *strlst;
        
        if (!dbus_message_get_args(
                m, &error,
                DBUS_TYPE_INT32, &interface,
                DBUS_TYPE_INT32, &protocol,
                DBUS_TYPE_STRING, &type,
                DBUS_TYPE_STRING, &name,
                DBUS_TYPE_STRING, &domain,
                DBUS_TYPE_STRING, &host,
                DBUS_TYPE_UINT16, &port, 
                DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &txt, &txt_len,
                DBUS_TYPE_INVALID) || !type || !*type || !name || !*name || !port) {
            avahi_log_warn("Error parsing EntryGroup::AddService message");
            goto fail;
        }

        strlst = avahi_string_list_new_from_array((const gchar**) txt, txt_len);
        dbus_free_string_array(txt);

        if (domain && !*domain)
            domain = NULL;

        if (host && !*host)
            host = NULL;

        if (avahi_server_add_service_strlst(avahi_server, i->entry_group, (AvahiIfIndex) interface, (AvahiProtocol) protocol, type, name, domain, host, port, strlst) < 0) {
            avahi_log_warn("Failed to add service: %s", name);
            return respond_error(c, m, "org.freedesktop.Avahi.InvalidServiceError", NULL);
        } else
            avahi_log_info("Successfully added service: %s", name);
        
        return respond_ok(c, m);
    } else if (dbus_message_is_method_call(m, AVAHI_DBUS_INTERFACE_ENTRY_GROUP, "AddAddress")) {
        gint32 interface, protocol;
        gchar *name, *address;
        AvahiAddress a;
        
        if (!dbus_message_get_args(
                m, &error,
                DBUS_TYPE_INT32, &interface,
                DBUS_TYPE_INT32, &protocol,
                DBUS_TYPE_STRING, &name,
                DBUS_TYPE_STRING, &address,
                DBUS_TYPE_INVALID) || !name || !*name || !address || !*address) {
            avahi_log_warn("Error parsing EntryGroup::AddAddress message");
            goto fail;
        }

        if (!(avahi_address_parse(address, AVAHI_PROTO_UNSPEC, &a))) {
            avahi_log_warn("Error parsing address data");
            return respond_error(c, m, "org.freedesktop.Avahi.InvalidAddressError", NULL);
        }

        if (avahi_server_add_address(avahi_server, i->entry_group, (AvahiIfIndex) interface, (AvahiProtocol) protocol, 0, name, &a) < 0) {
            avahi_log_warn("Failed to add service: %s", name);
            return respond_error(c, m, "org.freedesktop.Avahi.InvalidAddressError", NULL);
        } else
            avahi_log_info("Successfully added address: %s -> %s", name, address);
        
        return respond_ok(c, m);
    }

    avahi_log_warn("Missed message %s::%s()", dbus_message_get_interface(m), dbus_message_get_member(m));

fail:
    if (dbus_error_is_set(&error))
        dbus_error_free(&error);
    
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusHandlerResult respond_string(DBusConnection *c, DBusMessage *m, const gchar *text) {
    DBusMessage *reply;

    reply = dbus_message_new_method_return(m);
    dbus_message_append_args(reply, DBUS_TYPE_STRING, &text, DBUS_TYPE_INVALID);
    dbus_connection_send(c, reply, NULL);
    dbus_message_unref(reply);
    
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult msg_server_impl(DBusConnection *c, DBusMessage *m, void *userdata) {
    DBusError error;

    dbus_error_init(&error);

    avahi_log_debug("dbus: interface=%s, path=%s, member=%s",
                    dbus_message_get_interface(m),
                    dbus_message_get_path(m),
                    dbus_message_get_member(m));

    if (dbus_message_is_method_call(m, AVAHI_DBUS_INTERFACE_SERVER, "GetHostName")) {

        if (!dbus_message_get_args(m, &error, DBUS_TYPE_INVALID)) {
            avahi_log_warn("Error parsing Server::GetHostName message");
            goto fail;
        }

        return respond_string(c, m, avahi_server_get_host_name(avahi_server));
        
    } if (dbus_message_is_method_call(m, AVAHI_DBUS_INTERFACE_SERVER, "GetDomainName")) {

        if (!dbus_message_get_args(m, &error, DBUS_TYPE_INVALID)) {
            avahi_log_warn("Error parsing Server::GetDomainName message");
            goto fail;
        }

        return respond_string(c, m, avahi_server_get_domain_name(avahi_server));

    } if (dbus_message_is_method_call(m, AVAHI_DBUS_INTERFACE_SERVER, "GetHostNameFqdn")) {

        if (!(dbus_message_get_args(m, &error, DBUS_TYPE_INVALID))) {
            avahi_log_warn("Error parsing Server::GetHostNameFqdn message");
            goto fail;
        }
    
        return respond_string(c, m, avahi_server_get_host_name_fqdn(avahi_server));
        
    } else if (dbus_message_is_method_call(m, AVAHI_DBUS_INTERFACE_SERVER, "EntryGroupNew")) {
        Client *client;
        EntryGroupInfo *i;
        static const DBusObjectPathVTable vtable = {
            NULL,
            msg_entry_group_impl,
            NULL,
            NULL,
            NULL,
            NULL
        };
        DBusMessage *reply;

        if (!dbus_message_get_args(m, &error, DBUS_TYPE_INVALID)) {
            avahi_log_warn("Error parsing Server::EntryGroupNew message");
            goto fail;
        }

        client = client_get(dbus_message_get_sender(m), TRUE);

        i = g_new(EntryGroupInfo, 1);
        i->id = ++client->current_id;
        i->client = client;
        i->entry_group = avahi_entry_group_new(avahi_server, entry_group_callback, i);
        i->path = g_strdup_printf("/org/freedesktop/Avahi/Client%u/EntryGroup%u", client->id, i->id);

        AVAHI_LLIST_PREPEND(EntryGroupInfo, entry_groups, client->entry_groups, i);

        dbus_connection_register_object_path(c, i->path, &vtable, i);
        reply = dbus_message_new_method_return(m);
        dbus_message_append_args(reply, DBUS_TYPE_OBJECT_PATH, &i->path, DBUS_TYPE_INVALID);
        dbus_connection_send(c, reply, NULL);
        dbus_message_unref(reply);
        
        return DBUS_HANDLER_RESULT_HANDLED;
    } 
    
    avahi_log_warn("Missed message %s::%s()", dbus_message_get_interface(m), dbus_message_get_member(m));


fail:
    if (dbus_error_is_set(&error))
        dbus_error_free(&error);
    
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void dbus_protocol_server_state_changed(AvahiServerState state) {
    DBusMessage *m;
    gint32 t;
    
    if (!server)
        return;

    m = dbus_message_new_signal(AVAHI_DBUS_PATH_SERVER, AVAHI_DBUS_INTERFACE_SERVER, "StateChanged");
    t = (gint32) state;
    dbus_message_append_args(m, DBUS_TYPE_INT32, &t, DBUS_TYPE_INVALID);
    dbus_connection_send(server->bus, m, NULL);
    dbus_message_unref(m);
}

int dbus_protocol_setup(GMainLoop *loop) {
    DBusError error;

    static const DBusObjectPathVTable server_vtable = {
        NULL,
        msg_server_impl,
        NULL,
        NULL,
        NULL,
        NULL
    };

    dbus_error_init(&error);

    server = g_malloc(sizeof(Server));
    server->clients = NULL;
    server->current_id = 0;

    server->bus = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    if (dbus_error_is_set(&error)) {
        avahi_log_warn("dbus_bus_get(): %s", error.message);
        goto fail;
    }

    dbus_connection_setup_with_g_main(server->bus, NULL);
    dbus_connection_set_exit_on_disconnect(server->bus, FALSE);

    dbus_bus_request_name(server->bus, AVAHI_DBUS_NAME, 0, &error);
    if (dbus_error_is_set(&error)) {
        avahi_log_warn("dbus_bus_request_name(): %s", error.message);
        goto fail;
    }

    dbus_bus_add_match(server->bus, "type='signal',""interface='" DBUS_INTERFACE_DBUS  "'", &error);

    dbus_connection_add_filter(server->bus, msg_signal_filter_impl, loop, NULL);
    dbus_connection_register_object_path(server->bus, AVAHI_DBUS_PATH_SERVER, &server_vtable, NULL);

    return 0;

fail:
    if (server->bus) {
        dbus_connection_disconnect(server->bus);
        dbus_connection_unref(server->bus);
    }
    
    dbus_error_free (&error);
    g_free(server);
    server = NULL;
    return -1;
}

void dbus_protocol_shutdown(void) {

    if (server) {
    
        while (server->clients)
            client_free(server->clients);

        if (server->bus) {
            dbus_connection_disconnect(server->bus);
            dbus_connection_unref(server->bus);
        }

        g_free(server);
        server = NULL;
    }
}
