/*
 * profanity.c
 *
 * Copyright (C) 2012 - 2014 James Booth <boothj5@gmail.com>
 *
 * This file is part of Profanity.
 *
 * Profanity is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Profanity is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Profanity.  If not, see <http://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link the code of portions of this program with the OpenSSL library under
 * certain conditions as described in each individual source file, and
 * distribute linked combinations including the two.
 *
 * You must obey the GNU General Public License in all respects for all of the
 * code used other than OpenSSL. If you modify file(s) with this exception, you
 * may extend this exception to your version of the file(s), but you are not
 * obligated to do so. If you do not wish to do so, delete this exception
 * statement from your version. If you delete this exception statement from all
 * source files in the program, then also delete it here.
 *
 */
#include "prof_config.h"

#ifdef PROF_HAVE_GIT_VERSION
#include "gitversion.h"
#endif

#include <locale.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "profanity.h"
#include "chat_session.h"
#include "chat_state.h"
#include "config/accounts.h"
#include "config/preferences.h"
#include "config/theme.h"
#include "command/command.h"
#include "common.h"
#include "contact.h"
#include "roster_list.h"
#include "log.h"
#include "muc.h"
#include "plugins/plugins.h"
#ifdef PROF_HAVE_LIBOTR
#include "otr/otr.h"
#endif
#include "resource.h"
#include "xmpp/xmpp.h"
#include "ui/ui.h"
#include "ui/windows.h"

static void _handle_idle_time(void);
static void _init(const int disable_tls, char *log_level);
static void _shutdown(void);
static void _create_directories(void);

static gboolean idle = FALSE;

void
prof_run(const int disable_tls, char *log_level, char *account_name)
{
    _init(disable_tls, log_level);

    char inp[INP_WIN_MAX];

    plugins_on_start();

    char *pref_connect_account = prefs_get_string(PREF_CONNECT_ACCOUNT);
    if (account_name != NULL) {
        snprintf(inp, sizeof(inp), "%s %s", "/connect", account_name);
        prof_process_input(inp);
    } else if (pref_connect_account != NULL) {
        snprintf(inp, sizeof(inp), "%s %s", "/connect", pref_connect_account);
        prof_process_input(inp);
    }
    prefs_free_string(pref_connect_account);

    ui_update();

    log_info("Starting main event loop");

    jabber_conn_status_t conn_status = jabber_get_connection_status();
    int size = 0;
    gboolean read_input = TRUE;
    gboolean cmd_result = TRUE;

    while(cmd_result == TRUE) {
        size = 0;
        read_input = TRUE;
        while(read_input) {
            conn_status = jabber_get_connection_status();
            if (conn_status == JABBER_CONNECTED) {
                _handle_idle_time();
            }

            read_input = ui_get_char(inp, &size);

            plugins_run_timed();

#ifdef PROF_HAVE_LIBOTR
            otr_poll();
#endif
            notify_remind();
            jabber_process_events();
            ui_update();
        }

        cmd_result = prof_process_input(inp);
    }
}

void
prof_handle_idle(void)
{
    jabber_conn_status_t status = jabber_get_connection_status();
    if (status == JABBER_CONNECTED) {
        GSList *recipients = ui_get_chat_recipients();
        GSList *curr = recipients;

        while (curr != NULL) {
            char *barejid = curr->data;
            ProfChatWin *chatwin = wins_get_chat(barejid);
            chat_state_handle_idle(chatwin->barejid, chatwin->state);
            curr = g_slist_next(curr);
        }

        if (recipients != NULL) {
            g_slist_free(recipients);
        }
    }
}

void
prof_handle_activity(void)
{
    win_type_t win_type = ui_current_win_type();
    jabber_conn_status_t status = jabber_get_connection_status();

    if ((status == JABBER_CONNECTED) && (win_type == WIN_CHAT)) {
        ProfChatWin *chatwin = wins_get_current_chat();
        chat_state_handle_typing(chatwin->barejid, chatwin->state);
    }
}

/*
 * Take a line of input and process it, return TRUE if profanity is to
 * continue, FALSE otherwise
 */
gboolean
prof_process_input(char *inp)
{
    log_debug("Input received: %s", inp);
    gboolean result = FALSE;
    g_strstrip(inp);

    // add line to history if something typed
    if (strlen(inp) > 0) {
        cmd_history_append(inp);
    }

    // just carry on if no input
    if (strlen(inp) == 0) {
        result = TRUE;

    // handle command if input starts with a '/'
    } else if (inp[0] == '/') {
        char *inp_cpy = strdup(inp);
        char *command = strtok(inp_cpy, " ");
        result = cmd_execute(command, inp);
        free(inp_cpy);

    // call a default handler if input didn't start with '/'
    } else {
        result = cmd_execute_default(inp);
    }

    ui_input_clear();
    roster_reset_search_attempts();

    return result;
}

static void
_handle_idle_time()
{
    gint prefs_time = prefs_get_autoaway_time() * 60000;
    unsigned long idle_ms = ui_get_idle_time();
    char *pref_autoaway_mode = prefs_get_string(PREF_AUTOAWAY_MODE);

    if (!idle) {
        resource_presence_t current_presence = accounts_get_last_presence(jabber_get_account_name());
        if ((current_presence == RESOURCE_ONLINE) || (current_presence == RESOURCE_CHAT)) {
            if (idle_ms >= prefs_time) {
                idle = TRUE;
                char *pref_autoaway_message = prefs_get_string(PREF_AUTOAWAY_MESSAGE);

                // handle away mode
                if (strcmp(pref_autoaway_mode, "away") == 0) {
                    presence_update(RESOURCE_AWAY, pref_autoaway_message, 0);
                    ui_auto_away();

                // handle idle mode
                } else if (strcmp(pref_autoaway_mode, "idle") == 0) {
                    presence_update(RESOURCE_ONLINE, pref_autoaway_message, idle_ms / 1000);
                }

                prefs_free_string(pref_autoaway_message);
            }
        }

    } else {
        if (idle_ms < prefs_time) {
            idle = FALSE;

            // handle check
            if (prefs_get_boolean(PREF_AUTOAWAY_CHECK)) {
                if (strcmp(pref_autoaway_mode, "away") == 0) {
                    presence_update(RESOURCE_ONLINE, NULL, 0);
                    ui_end_auto_away();
                } else if (strcmp(pref_autoaway_mode, "idle") == 0) {
                    presence_update(RESOURCE_ONLINE, NULL, 0);
                    ui_titlebar_presence(CONTACT_ONLINE);
                }
            }
        }
    }

    prefs_free_string(pref_autoaway_mode);
}

static void
_init(const int disable_tls, char *log_level)
{
    setlocale(LC_ALL, "");
    // ignore SIGPIPE
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    _create_directories();
    log_level_t prof_log_level = log_level_from_string(log_level);
    prefs_load();
    log_init(prof_log_level);
    if (strcmp(PROF_PACKAGE_STATUS, "development") == 0) {
#ifdef PROF_HAVE_GIT_VERSION
            log_info("Starting Profanity (%sdev.%s.%s)...", PROF_PACKAGE_VERSION, PROF_GIT_BRANCH, PROF_GIT_REVISION);
#else
            log_info("Starting Profanity (%sdev)...", PROF_PACKAGE_VERSION);
#endif
    } else {
        log_info("Starting Profanity (%s)...", PROF_PACKAGE_VERSION);
    }
    chat_log_init();
    groupchat_log_init();
    accounts_load();
    char *theme = prefs_get_string(PREF_THEME);
    theme_init(theme);
    prefs_free_string(theme);
    ui_init();
    jabber_init(disable_tls);
    cmd_init();
    log_info("Initialising contact list");
    roster_init();
    muc_init();
#ifdef PROF_HAVE_LIBOTR
    otr_init();
#endif
    atexit(_shutdown);
    plugins_init();
    ui_input_nonblocking(TRUE);
}

static void
_shutdown(void)
{
    if (prefs_get_boolean(PREF_TITLEBAR_SHOW)) {
        if (prefs_get_boolean(PREF_TITLEBAR_GOODBYE)) {
            ui_goodbye_title();
        } else {
            ui_clear_win_title();
        }
    }
    ui_close_all_wins();
    jabber_disconnect();
    jabber_shutdown();
    plugins_on_shutdown();
    roster_free();
    muc_close();
    caps_close();
    ui_close();
#ifdef HAVE_LIBOTR
    otr_shutdown();
#endif
    chat_log_close();
    prefs_close();
    theme_close();
    accounts_close();
    cmd_uninit();
    log_close();
    plugins_shutdown();
}

static void
_create_directories(void)
{
    gchar *xdg_config = xdg_get_config_home();
    gchar *xdg_data = xdg_get_data_home();

    GString *themes_dir = g_string_new(xdg_config);
    g_string_append(themes_dir, "/profanity/themes");
    GString *chatlogs_dir = g_string_new(xdg_data);
    g_string_append(chatlogs_dir, "/profanity/chatlogs");
    GString *logs_dir = g_string_new(xdg_data);
    g_string_append(logs_dir, "/profanity/logs");
    GString *plugins_dir = g_string_new(xdg_data);
    g_string_append(plugins_dir, "/profanity/plugins");

    if (!mkdir_recursive(themes_dir->str)) {
        log_error("Error while creating directory %s", themes_dir->str);
    }
    if (!mkdir_recursive(chatlogs_dir->str)) {
        log_error("Error while creating directory %s", chatlogs_dir->str);
    }
    if (!mkdir_recursive(logs_dir->str)) {
        log_error("Error while creating directory %s", logs_dir->str);
    }
    if (!mkdir_recursive(plugins_dir->str)) {
        log_error("Error while creating directory %s", plugins_dir->str);
    }

    g_string_free(themes_dir, TRUE);
    g_string_free(chatlogs_dir, TRUE);
    g_string_free(logs_dir, TRUE);
    g_string_free(plugins_dir, TRUE);

    g_free(xdg_config);
    g_free(xdg_data);
}
