// Original code taken from the example webkit-gtk+ application. see notice below.
// Modified code is licensed under the GPL 3.  See LICENSE file.


/*
 * Copyright (C) 2006, 2007 Apple Inc.
 * Copyright (C) 2007 Alp Toker <alp@atoker.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#define LENGTH(x) (sizeof x / sizeof x[0])
#define MAX_BINDINGS 256

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gdk/gdkkeysyms.h>
#include <webkit/webkit.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>

/* housekeeping / internal variables */
static GtkWidget* main_window;
static GtkWidget* mainbar;
static GtkWidget* mainbar_label;
static WebKitWebView* web_view;
static gchar* main_title;
static gchar selected_url[500] = "\0";
static gint load_progress;
static Window xwin = 0;
static char fifo_path[64];
static char socket_path[108];

/* state variables (initial values coming from command line arguments but may be changed later) */
static gchar*   uri         = NULL;
static gchar*   config_file = NULL;
static gchar    config_file_path[500];
static gboolean verbose     = FALSE;

/* settings from config: group behaviour */
static gchar*   history_handler    = NULL;
static gchar*   fifo_dir           = NULL;
static gchar*   socket_dir         = NULL;
static gchar*   download_handler   = NULL;
static gboolean always_insert_mode = FALSE;
static gboolean show_status        = FALSE;
static gboolean insert_mode        = FALSE;
static gboolean status_top         = FALSE;
static gchar*   modkey             = NULL;
static guint    modmask            = 0;

/* settings from config: group bindings, key -> action */
static GHashTable *bindings;

/* command list: name -> Command  */
static GHashTable *commands;

typedef struct {
    char *name;
    char *param;
} Action;

/* commandline arguments (set initial values for the state variables) */
static GOptionEntry entries[] =
{
    { "uri",     'u', 0, G_OPTION_ARG_STRING, &uri,         "Uri to load", NULL },
    { "verbose", 'v', 0, G_OPTION_ARG_NONE,   &verbose,     "Be verbose",  NULL },
    { "config",  'c', 0, G_OPTION_ARG_STRING, &config_file, "Config file", NULL },
    { NULL,      0, 0, 0, NULL, NULL, NULL }
};

typedef void (*Command)(WebKitWebView*, const char *);

/* XDG stuff */
static char *XDG_CONFIG_HOME_default[256];
static char *XDG_CONFIG_DIRS_default = "/etc/xdg";

static void
update_title (GtkWindow* window);

static void
load_uri ( WebKitWebView * web_view, const gchar * uri);

static void
new_window_load_uri (const gchar * uri);

static void
close_uzbl (WebKitWebView *page, const char *param);

static gboolean
run_command(const char *command, const char *args);

static void
spawn(WebKitWebView *web_view, const char *param);

static void
free_action(gpointer action);

static Action*
new_action(const gchar *name, const gchar *param);



/* --- CALLBACKS --- */

static gboolean
new_window_cb (WebKitWebView *web_view, WebKitWebFrame *frame, WebKitNetworkRequest *request, WebKitWebNavigationAction *navigation_action, WebKitWebPolicyDecision *policy_decision, gpointer user_data) {
    (void) web_view;
    (void) frame;
    (void) navigation_action;
    (void) policy_decision;
    (void) user_data;
    const gchar* uri = webkit_network_request_get_uri (request);
    printf("New window requested -> %s \n", uri);
    new_window_load_uri(uri);
    return (FALSE);
}

WebKitWebView*
create_web_view_cb (WebKitWebView  *web_view, WebKitWebFrame *frame, gpointer user_data) {
    (void) web_view;
    (void) frame;
    (void) user_data;
    if (selected_url[0]!=0) {
        printf("\nNew web view -> %s\n",selected_url);
        new_window_load_uri(selected_url);
    } else {
        printf("New web view -> %s\n","Nothing to open, exiting");
    }
    return (NULL);
}

static gboolean
download_cb (WebKitWebView *web_view, GObject *download, gpointer user_data) {
    (void) web_view;
    (void) user_data;
    if (download_handler) {
        const gchar* uri = webkit_download_get_uri ((WebKitDownload*)download);
        printf("Download -> %s\n",uri);
        run_command(download_handler, uri);
    }
    return (FALSE);
}

static void
toggle_status_cb (WebKitWebView* page, const char *param) {
    (void)page;
    (void)param;
    if (show_status) {
        gtk_widget_hide(mainbar);
    } else {
        gtk_widget_show(mainbar);
    }
    show_status = !show_status;
    update_title (GTK_WINDOW (main_window));
}

static void
link_hover_cb (WebKitWebView* page, const gchar* title, const gchar* link, gpointer data) {
    (void) page;
    (void) title;
    (void) data;    
    //ADD HOVER URL TO WINDOW TITLE
    selected_url[0] = '\0';
    if (link) {
        strcpy (selected_url, link);
    }
    update_title (GTK_WINDOW (main_window));
}

static void
title_change_cb (WebKitWebView* web_view, WebKitWebFrame* web_frame, const gchar* title, gpointer data) {
    (void) web_view;
    (void) web_frame;
    (void) data;
    if (main_title)
        g_free (main_title);
    main_title = g_strdup (title);
    update_title (GTK_WINDOW (main_window));
}

static void
progress_change_cb (WebKitWebView* page, gint progress, gpointer data) {
    (void) page;
    (void) data;
    load_progress = progress;
    update_title (GTK_WINDOW (main_window));
}

static void
load_commit_cb (WebKitWebView* page, WebKitWebFrame* frame, gpointer data) {
    (void) page;
    (void) data;
    free (uri);
    GString* newuri = g_string_new (webkit_web_frame_get_uri (frame));
    uri = g_string_free (newuri, FALSE);
}

static void
destroy_cb (GtkWidget* widget, gpointer data) {
    (void) widget;
    (void) data;
    gtk_main_quit ();
}

static void
log_history_cb () {
   if (history_handler) {
       time_t rawtime;
       struct tm * timeinfo;
       char date [80];
       time ( &rawtime );
       timeinfo = localtime ( &rawtime );
       strftime (date, 80, "%Y-%m-%d %H:%M:%S", timeinfo);
       GString* args = g_string_new ("");
       g_string_printf (args, "'%s' '%s' '%s'", uri, "TODO:page title here", date);
       run_command(history_handler, args->str);
       g_string_free (args, TRUE);
   }
}

/* VIEW funcs (little webkit wrappers) */

#define VIEWFUNC(name) static void view_##name(WebKitWebView *page, const char *param){(void)param; webkit_web_view_##name(page);}
VIEWFUNC(reload)
VIEWFUNC(stop_loading)
VIEWFUNC(zoom_in)
VIEWFUNC(zoom_out)
VIEWFUNC(go_back)
VIEWFUNC(go_forward)
#undef VIEWFUNC

/* -- command to callback/function map for things we cannot attach to any signals */
// TODO: reload

static struct {char *name; Command command;} cmdlist[] =
{
    { "back",          view_go_back       },
    { "forward",       view_go_forward    },
    { "reload",        view_reload,       }, //Buggy
    { "refresh",       view_reload,       }, /* for convenience, will change */
    { "stop",          view_stop_loading, },
    { "zoom_in",       view_zoom_in,      }, //Can crash (when max zoom reached?).
    { "zoom_out",      view_zoom_out,     },
    { "uri",           load_uri           },
    { "toggle_status", toggle_status_cb   },
    { "spawn",         spawn              },
    { "exit",          close_uzbl         },
};

static void
commands_hash(void)
{
    unsigned int i;
    commands = g_hash_table_new(g_str_hash, g_str_equal);

    for (i = 0; i < LENGTH(cmdlist); i++)
        g_hash_table_insert(commands, cmdlist[i].name, cmdlist[i].command);
}

/* -- CORE FUNCTIONS -- */

void
free_action(gpointer act) {
    Action *action = (Action*)act;
    g_free(action->name);
    if (action->param)
        g_free(action->param);
    g_free(action);
}

Action*
new_action(const gchar *name, const gchar *param) {
    Action *action = g_new(Action, 1);

    action->name = g_strdup(name);
    if (param)
        action->param = g_strdup(param);
    else
        action->param = NULL;

    return action;
}

static bool
file_exists (const char * filename) {
    FILE *file = fopen (filename, "r");
    if (file) {
        fclose (file);
        return true;
    }
    return false;
}

static void
load_uri (WebKitWebView * web_view, const gchar *param) {
    if (param) {
        GString* newuri = g_string_new (param);
        if (g_strrstr (param, "://") == NULL)
            g_string_prepend (newuri, "http://"); 
        webkit_web_view_load_uri (web_view, newuri->str);
        g_string_free (newuri, TRUE);
    }
}

static void
new_window_load_uri (const gchar * uri) {
    GString* to_execute = g_string_new ("");
    if (!config_file) {
        g_string_printf (to_execute, "uzbl --uri '%s'", uri);
    } else {
        g_string_printf (to_execute, "uzbl --uri '%s' --config '%s'", uri, config_file);
    }
    printf("Spawning %s\n",to_execute->str);
    if (!g_spawn_command_line_async (to_execute->str, NULL)) {
        if (!config_file) {
            g_string_printf (to_execute, "./uzbl --uri '%s'", uri);
        } else {
            g_string_printf (to_execute, "./uzbl --uri '%s' --config '%s'", uri, config_file);
        }
        printf("Spawning %s\n",to_execute->str);
    g_spawn_command_line_async (to_execute->str, NULL);
    }
    g_string_free (to_execute, TRUE);
}

static void
close_uzbl (WebKitWebView *page, const char *param) {
    (void)page;
    (void)param;
    gtk_main_quit ();
}

// make sure to put '' around args, so that if there is whitespace we can still keep arguments together.
static gboolean
run_command(const char *command, const char *args) {
   //command <uzbl conf> <uzbl pid> <uzbl win id> <uzbl fifo file> <uzbl socket file> [args]
    GString* to_execute = g_string_new ("");
    gboolean result;
    g_string_printf (to_execute, "%s '%s' '%i' '%i' '%s' '%s'", command, config_file, (int) getpid() , (int) xwin, fifo_path, socket_path);
    if(args) {
        g_string_append_printf (to_execute, " %s", args);
    }
    result = g_spawn_command_line_async (to_execute->str, NULL);
    printf("Called %s.  Result: %s\n", to_execute->str, (result ? "TRUE" : "FALSE" ));
    g_string_free (to_execute, TRUE);
    return result;
}

static void
spawn(WebKitWebView *web_view, const char *param) {
    (void)web_view;
    run_command(param, NULL);
}

static void
parse_command(const char *cmd, const char *param) {
    Command c;

    if ((c = g_hash_table_lookup(commands, cmd)))
        c(web_view, param);
    else
        fprintf (stderr, "command \"%s\" not understood. ignoring.\n", cmd);
}

static void
parse_line(char *line) {
    gchar **parts;

    g_strstrip(line);

    parts = g_strsplit(line, " ", 2);

    if (!parts)
        return;

    parse_command(parts[0], parts[1]);

    g_strfreev(parts);
}

static void
control_fifo(GIOChannel *fd) {
    gchar *ctl_line;
    gsize term_pos;

    if(!fd)
       return;

    g_io_channel_read_line(fd, &ctl_line, NULL, &term_pos, NULL);
    ctl_line[term_pos] ='\0';

    parse_line(ctl_line);

    g_free(ctl_line);

    return;
}

static void
create_fifo() {
    GIOChannel *chan = NULL;

    if (fifo_dir) {
        sprintf (fifo_path, "%s/uzbl_fifo_%d", fifo_dir, (int) xwin);
    } else {
        sprintf (fifo_path, "/tmp/uzbl_fifo_%d", (int) xwin);
    }
    printf ("Control fifo opened in %s\n", fifo_path);
    if (mkfifo (fifo_path, 0666) == -1) {
        printf ("Possible error creating fifo\n");
    }

    if( (chan = g_io_channel_new_file((gchar *) fifo_path, "r+", NULL)) )
        g_io_add_watch(chan, G_IO_IN|G_IO_HUP, (GIOFunc) control_fifo, chan);
    return;
}

static void
*control_socket() {
    if (socket_dir) {
        sprintf (socket_path, "%s/uzbl_socket_%d", socket_dir, (int) xwin);
    } else {
        sprintf (socket_path, "/tmp/uzbl_socket_%d", (int) xwin);
    }
 
    int sock, clientsock, len;
    unsigned int t;
    struct sockaddr_un local, remote;

    sock = socket (AF_UNIX, SOCK_STREAM, 0);

    local.sun_family = AF_UNIX;
    strcpy (local.sun_path, socket_path);
    unlink (local.sun_path);

    len = strlen (local.sun_path) + sizeof (local.sun_family);
    bind (sock, (struct sockaddr *) &local, len);

    if (errno == -1) {
        printf ("A problem occurred when opening a socket in %s\n", socket_path);
    } else {
        printf ("Control socket opened in %s\n", socket_path);
    }

    listen (sock, 5);
 
    char buffer[512];
    char temp[128];
    int done, n;
    for(;;) {
        memset (buffer, 0, sizeof (buffer));

        t          = sizeof (remote);
        clientsock = accept (sock, (struct sockaddr *) &remote, &t);
        printf ("Connected to client\n");

        done = 0;
        do {
            memset (temp, 0, sizeof (temp));
            n = recv (clientsock, temp, 128, 0);
            if (n == 0) {
                buffer[strlen (buffer)] = '\0';
                done = 1;
            }

            if (!done)
                strcat (buffer, temp);
        } while (!done);

        if (strcmp (buffer, "\n") < 0) {
            buffer[strlen (buffer) - 1] = '\0';
        } else {
          buffer[strlen (buffer)] = '\0';
        }

        parse_line (buffer);
        close (clientsock);
    }

    return NULL;
} 
 
static void
setup_threading () {
    pthread_t control_thread;
    pthread_create(&control_thread, NULL, control_socket, NULL);
}

static void
update_title (GtkWindow* window) {
    GString* string_long = g_string_new ("");
    GString* string_short = g_string_new ("");
    if (!always_insert_mode)
        g_string_append (string_long, (insert_mode ? "[I] " : "[C] "));
    if (main_title) {
        g_string_append (string_long, main_title);
        g_string_append (string_short, main_title);
    }
    g_string_append (string_long, " - Uzbl browser");
    g_string_append (string_short, " - Uzbl browser");
    if (load_progress < 100)
        g_string_append_printf (string_long, " (%d%%)", load_progress);

    if (selected_url[0]!=0) {
        g_string_append_printf (string_long, " -> (%s)", selected_url);
    }

    gchar* title_long = g_string_free (string_long, FALSE);
    gchar* title_short = g_string_free (string_short, FALSE);

    if (show_status) {
        gtk_window_set_title (window, title_short);
    gtk_label_set_text(GTK_LABEL(mainbar_label), title_long);
    } else {
        gtk_window_set_title (window, title_long);
    }

    g_free (title_long);
    g_free (title_short);
}
 
static gboolean
key_press_cb (WebKitWebView* page, GdkEventKey* event)
{
    //TRUE to stop other handlers from being invoked for the event. FALSE to propagate the event further.

    (void) page;
    Action *action;

    if (event->type != GDK_KEY_PRESS || event->keyval == GDK_Page_Up || event->keyval == GDK_Page_Down
        || event->keyval == GDK_Up || event->keyval == GDK_Down || event->keyval == GDK_Left || event->keyval == GDK_Right)
        return FALSE;

    //TURN OFF/ON INSERT MODE
    if ((insert_mode && (event->keyval == GDK_Escape)) || (!insert_mode && (event->string[0] == 'i'))) {
        insert_mode = !insert_mode || always_insert_mode;
        update_title (GTK_WINDOW (main_window));
        return TRUE;
    }

    if ((!insert_mode || (event->state == modmask)) && (action = g_hash_table_lookup(bindings, event->string))) {
        parse_command(action->name, action->param);
        return TRUE;
    }

    return !insert_mode;
}

static GtkWidget*
create_browser () {
    GtkWidget* scrolled_window = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled_window), GTK_POLICY_NEVER, GTK_POLICY_NEVER); //todo: some sort of display of position/total length. like what emacs does

    web_view = WEBKIT_WEB_VIEW (webkit_web_view_new ());
    gtk_container_add (GTK_CONTAINER (scrolled_window), GTK_WIDGET (web_view));

    g_signal_connect (G_OBJECT (web_view), "title-changed", G_CALLBACK (title_change_cb), web_view);
    g_signal_connect (G_OBJECT (web_view), "load-progress-changed", G_CALLBACK (progress_change_cb), web_view);
    g_signal_connect (G_OBJECT (web_view), "load-committed", G_CALLBACK (load_commit_cb), web_view);
    g_signal_connect (G_OBJECT (web_view), "load-committed", G_CALLBACK (log_history_cb), web_view);
    g_signal_connect (G_OBJECT (web_view), "hovering-over-link", G_CALLBACK (link_hover_cb), web_view);
    g_signal_connect (G_OBJECT (web_view), "key-press-event", G_CALLBACK (key_press_cb), web_view);
    g_signal_connect (G_OBJECT (web_view), "new-window-policy-decision-requested", G_CALLBACK (new_window_cb), web_view); 
    g_signal_connect (G_OBJECT (web_view), "download-requested", G_CALLBACK (download_cb), web_view); 
    g_signal_connect (G_OBJECT (web_view), "create-web-view", G_CALLBACK (create_web_view_cb), web_view);  

    return scrolled_window;
}

static GtkWidget*
create_mainbar () {
    mainbar = gtk_hbox_new (FALSE, 0);
    mainbar_label = gtk_label_new ("");  
    gtk_misc_set_alignment (GTK_MISC(mainbar_label), 0, 0);
    gtk_misc_set_padding (GTK_MISC(mainbar_label), 2, 2);
    gtk_box_pack_start (GTK_BOX (mainbar), mainbar_label, TRUE, TRUE, 0);
    return mainbar;
}

static
GtkWidget* create_window () {
    GtkWidget* window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size (GTK_WINDOW (window), 800, 600);
    gtk_widget_set_name (window, "Uzbl browser");
    g_signal_connect (G_OBJECT (window), "destroy", G_CALLBACK (destroy_cb), NULL);

    return window;
}

static void
add_binding (const gchar *key, const gchar *act) {
    char **parts = g_strsplit(act, " ", 2);
    Action *action;

    if (!parts)
        return;

    action = new_action(parts[0], parts[1]);
    g_hash_table_insert(bindings, g_strdup(key), action);

    g_strfreev(parts);
}

static void
settings_init () {
    GKeyFile* config;
    gboolean res  = FALSE;
    char *saveptr;
    gchar** keys = NULL;

    if (!config_file) {
        const char* XDG_CONFIG_HOME = getenv ("XDG_CONFIG_HOME");
        if (! XDG_CONFIG_HOME || ! strcmp (XDG_CONFIG_HOME, "")) {
          XDG_CONFIG_HOME = (char*)XDG_CONFIG_HOME_default;
        }
        printf("XDG_CONFIG_HOME: %s\n", XDG_CONFIG_HOME);
    
        strcpy (config_file_path, XDG_CONFIG_HOME);
        strcat (config_file_path, "/uzbl/config");
        if (file_exists (config_file_path)) {
          printf ("Config file %s found.\n", config_file_path);
          config_file = &config_file_path[0];
        } else {
            // Now we check $XDG_CONFIG_DIRS
            char *XDG_CONFIG_DIRS = getenv ("XDG_CONFIG_DIRS");
            if (! XDG_CONFIG_DIRS || ! strcmp (XDG_CONFIG_DIRS, ""))
                XDG_CONFIG_DIRS = XDG_CONFIG_DIRS_default;

            printf("XDG_CONFIG_DIRS: %s\n", XDG_CONFIG_DIRS);

            char buffer[512];
            strcpy (buffer, XDG_CONFIG_DIRS);
            const gchar* dir = (char *) strtok_r (buffer, ":", &saveptr);
            while (dir && ! file_exists (config_file_path)) {
                strcpy (config_file_path, dir);
                strcat (config_file_path, "/uzbl/config_file_pathig");
                if (file_exists (config_file_path)) {
                    printf ("Config file %s found.\n", config_file_path);
                    config_file = &config_file_path[0];
                }
                dir = (char * ) strtok_r (NULL, ":", &saveptr);
            }
        }
    }

    if (config_file) {
        config = g_key_file_new ();
        res = g_key_file_load_from_file (config, config_file, G_KEY_FILE_NONE, NULL);
          if(res) {
            printf ("Config %s loaded\n", config_file);
          } else {
            fprintf (stderr, "Config %s loading failed\n", config_file);
        }
    } else {
        printf ("No configuration.\n");
    }

    if (res) {
        history_handler    = g_key_file_get_value   (config, "behavior", "history_handler",    NULL);
        download_handler   = g_key_file_get_value   (config, "behavior", "download_handler",   NULL);
        always_insert_mode = g_key_file_get_boolean (config, "behavior", "always_insert_mode", NULL);
        show_status        = g_key_file_get_boolean (config, "behavior", "show_status",        NULL);
        modkey             = g_key_file_get_value   (config, "behavior", "modkey",             NULL);
        status_top         = g_key_file_get_boolean (config, "behavior", "status_top",         NULL);
        if (! fifo_dir)
            fifo_dir        = g_key_file_get_value   (config, "behavior", "fifodir",            NULL);
        if (! socket_dir)
            socket_dir     = g_key_file_get_value   (config, "behavior", "socket_dir",         NULL);
        keys               = g_key_file_get_keys    (config, "bindings", NULL,                 NULL);
    }
    
    printf ("History handler: %s\n",    (history_handler    ? history_handler  : "disabled"));
    printf ("Download manager: %s\n",   (download_handler   ? download_handler : "disabled"));
    printf ("Fifo directory: %s\n",     (fifo_dir           ? fifo_dir         : "/tmp"));
    printf ("Socket directory: %s\n",   (socket_dir         ? socket_dir       : "/tmp"));
    printf ("Always insert mode: %s\n", (always_insert_mode ? "TRUE"           : "FALSE"));
    printf ("Show status: %s\n",        (show_status        ? "TRUE"           : "FALSE"));
    printf ("Status top: %s\n",         (status_top         ? "TRUE"           : "FALSE"));
    printf ("Modkey: %s\n",             (modkey             ? modkey           : "disabled"));

    if (! modkey)
        modkey = "";

    //POSSIBLE MODKEY VALUES (COMBINATIONS CAN BE USED)
    gchar* modkeyup = g_utf8_strup (modkey, -1);
    if (g_strrstr (modkeyup,"SHIFT") != NULL)    modmask |= GDK_SHIFT_MASK;    //the Shift key.
    if (g_strrstr (modkeyup,"LOCK") != NULL)     modmask |= GDK_LOCK_MASK;     //a Lock key (depending on the modifier mapping of the X server this may either be CapsLock or ShiftLock).
    if (g_strrstr (modkeyup,"CONTROL") != NULL)  modmask |= GDK_CONTROL_MASK;  //the Control key.
    if (g_strrstr (modkeyup,"MOD1") != NULL)     modmask |= GDK_MOD1_MASK;     //the fourth modifier key (it depends on the modifier mapping of the X server which key is interpreted as this modifier, but normally it is the Alt key).
    if (g_strrstr (modkeyup,"MOD2") != NULL)     modmask |= GDK_MOD2_MASK;     //the fifth modifier key (it depends on the modifier mapping of the X server which key is interpreted as this modifier).
    if (g_strrstr (modkeyup,"MOD3") != NULL)     modmask |= GDK_MOD3_MASK;     //the sixth modifier key (it depends on the modifier mapping of the X server which key is interpreted as this modifier).
    if (g_strrstr (modkeyup,"MOD4") != NULL)     modmask |= GDK_MOD4_MASK;     //the seventh modifier key (it depends on the modifier mapping of the X server which key is interpreted as this modifier).
    if (g_strrstr (modkeyup,"MOD5") != NULL)     modmask |= GDK_MOD5_MASK;     //the eighth modifier key (it depends on the modifier mapping of the X server which key is interpreted as this modifier).
    if (g_strrstr (modkeyup,"BUTTON1") != NULL)  modmask |= GDK_BUTTON1_MASK;  //the first mouse button.
    if (g_strrstr (modkeyup,"BUTTON2") != NULL)  modmask |= GDK_BUTTON2_MASK;  //the second mouse button.
    if (g_strrstr (modkeyup,"BUTTON3") != NULL)  modmask |= GDK_BUTTON3_MASK;  //the third mouse button.
    if (g_strrstr (modkeyup,"BUTTON4") != NULL)  modmask |= GDK_BUTTON4_MASK;  //the fourth mouse button.
    if (g_strrstr (modkeyup,"BUTTON5") != NULL)  modmask |= GDK_BUTTON5_MASK;  //the fifth mouse button.
    if (g_strrstr (modkeyup,"SUPER") != NULL)    modmask |= GDK_SUPER_MASK;    //the Super modifier. Since 2.10
    if (g_strrstr (modkeyup,"HYPER") != NULL)    modmask |= GDK_HYPER_MASK;    //the Hyper modifier. Since 2.10
    if (g_strrstr (modkeyup,"META") != NULL)     modmask |= GDK_META_MASK;     //the Meta modifier. Since 2.10  */
    free (modkeyup);

    if (keys) {
        int i;
        for (i = 0; keys[i]; i++) {
            gchar *value = g_key_file_get_string (config, "bindings", keys[i], NULL);
            
            add_binding(g_strstrip(keys[i]), value);
            g_free(value);
        }

        g_strfreev(keys);
    }
}

int
main (int argc, char* argv[]) {
    gtk_init (&argc, &argv);
    if (!g_thread_supported ())
        g_thread_init (NULL);

    printf("Uzbl start location: %s\n", argv[0]);

    strcat ((char *) XDG_CONFIG_HOME_default, getenv ("HOME"));
    strcat ((char *) XDG_CONFIG_HOME_default, "/.config");

    GError *error = NULL;
    GOptionContext* context = g_option_context_new ("- some stuff here maybe someday");
    g_option_context_add_main_entries (context, entries, NULL);
    g_option_context_add_group (context, gtk_get_option_group (TRUE));
    g_option_context_parse (context, &argc, &argv, &error);
    /* initialize hash table */
    bindings = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_action);

    settings_init ();
    commands_hash ();

    if (always_insert_mode)
        insert_mode = TRUE;

    GtkWidget* vbox = gtk_vbox_new (FALSE, 0);
    if (status_top)
        gtk_box_pack_start (GTK_BOX (vbox), create_mainbar (), FALSE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (vbox), create_browser (), TRUE, TRUE, 0);
    if (!status_top)
        gtk_box_pack_start (GTK_BOX (vbox), create_mainbar (), FALSE, TRUE, 0);

    main_window = create_window ();
    gtk_container_add (GTK_CONTAINER (main_window), vbox);

    load_uri (web_view, uri);

    gtk_widget_grab_focus (GTK_WIDGET (web_view));
    gtk_widget_show_all (main_window);
    xwin = GDK_WINDOW_XID (GTK_WIDGET (main_window)->window);
    printf("window_id %i\n",(int) xwin);
    printf("pid %i\n", getpid ());

    if (!show_status)
        gtk_widget_hide(mainbar);

    setup_threading ();
    create_fifo ();

    gtk_main ();

    unlink (socket_path);
    unlink (fifo_path);

    g_hash_table_destroy(bindings);
    g_hash_table_destroy(commands);
    return 0;
}

/* vi: set et ts=4: */
