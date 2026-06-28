/*
 * htmltext_viewer.c - Claws Mail plugin: HTML to plain text viewer
 *
 * Renders text/html MIME parts by piping the raw HTML through an
 * external binary (htmltext-render) and displaying the resulting
 * plain text in a GtkTextView.  No network access, no JS, no images.
 *
 * Copyright (C) 2026
 *  - claude@anthropic.com (Sonnet 4.6)
 *  - olivier.delhomme@free.fr (Human 1.0)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#include <errno.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>

/* Claws Mail public headers (installed by claws-mail-dev) */
#include "common/version.h"
#include "file-utils.h"
#include "plugin.h"
#include "mimeview.h"
#include "procmime.h"
#include "utils.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define PLUGIN_NAME  (_("HTMLText Viewer"))

/*
 * Name (or full path) of the companion Rust binary.
 * Override at compile time with:
 *   make CFLAGS_EXTRA='-DHTMLTEXT_RENDER_BIN=\"/usr/lib/claws-mail/htmltext-render\"'
 */
#ifndef HTMLTEXT_RENDER_BIN
    #define HTMLTEXT_RENDER_BIN "htmltext-render"
#endif

/* Maximum bytes read from the render process stdout (4 MiB). */
#define MAX_OUTPUT_BYTES (4 * 1024 * 1024)

/* ------------------------------------------------------------------ */
/* Viewer struct                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    MimeViewer  mimeviewer;   /* MUST be first – Claws casts the pointer */
    GtkWidget  *scrolled;     /* GtkScrolledWindow (the widget Claws embeds) */
    GtkWidget  *textview;     /* GtkTextView inside the scrolled window      */
    gchar      *tmp_filename; /* temp file holding the current MIME part     */
} HtmlTextViewer;

/* Forward declaration */
static MimeViewerFactory htmltext_viewer_factory;

/* Write all @len bytes of @buf to @fd, retrying on EINTR.
 * Returns TRUE on success, FALSE on unrecoverable error.
 */
static gboolean pipe_write_all(gint fd, const gchar *buf, gsize len)
{
    gboolean ok = TRUE;
    gssize   n  = 0;

    while (len > 0) {
        n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            g_warning("%s: write: %s", PLUGIN_NAME, g_strerror(errno));
            ok = FALSE;
            break;
        }
        buf += n;
        len -= (gsize) n;
    }

    /* Always close fd; check for late errors (e.g. broken pipe). */
    if (close(fd) < 0) {
        g_warning("%s: close(stdin): %s", PLUGIN_NAME, g_strerror(errno));
        ok = FALSE;
    }

    return ok;
}

/* Read all output from @fd into a newly allocated string (NUL-terminated),
 * capped at MAX_OUTPUT_BYTES.  Sets *out_text and *out_len on success.
 * Returns TRUE on success, FALSE on unrecoverable error.
 */
static gboolean pipe_read_all(gint fd, gchar **out_text, gsize *out_len)
{
    GString *buf   = g_string_new_len(NULL, 8192);
    gssize   n     = 0;
    gchar    chunk[4096];

    while (TRUE) {
        n = read(fd, chunk, sizeof(chunk));

        if (n < 0) {
            if (errno == EINTR) continue;
            g_warning("%s: read: %s", PLUGIN_NAME, g_strerror(errno));
            g_string_free(buf, TRUE);
            return FALSE;
        }

        if (n == 0) break; /* EOF */

        if (buf->len + (gsize) n > MAX_OUTPUT_BYTES) {
            g_warning("%s: output truncated at %d bytes", PLUGIN_NAME,
                      MAX_OUTPUT_BYTES);
            g_string_append_len(buf, chunk, (gssize)(MAX_OUTPUT_BYTES - buf->len));
            break;
        }

        g_string_append_len(buf, chunk, n);
    }

    if (close(fd) < 0)
        g_warning("%s: close(stdout): %s", PLUGIN_NAME, g_strerror(errno));

    *out_len  = buf->len;
    *out_text = g_string_free(buf, FALSE); /* transfer ownership */

    return TRUE;
}


/* ------------------------------------------------------------------ */
/* Helper: spawn htmltext-render, pipe HTML in, collect text out       */
/* ------------------------------------------------------------------ */

/*
 * render_html_to_text:
 *   @html_bytes:  raw HTML content
 *   @html_len:    byte length of @html_bytes
 *   @out_text:    (out) newly allocated NUL-terminated UTF-8 text, or NULL
 *   @out_len:     (out) byte length of *out_text
 *
 * Returns TRUE on success.
 *
 * Security properties:
 *   - The child binary performs no network I/O; it only reads stdin.
 *   - We pass no environment variables beyond the minimal inherited set.
 *   - stdout is collected in memory; we enforce MAX_OUTPUT_BYTES.
 */
static gboolean render_html_to_text(const gchar *html_bytes,
                                    gsize        html_len,
                                    gchar      **out_text,
                                    gsize       *out_len)
{
    gchar   *argv[2]   = { HTMLTEXT_RENDER_BIN, NULL };
    gint     stdin_fd  = -1;
    gint     stdout_fd = -1;
    GPid     child_pid = 0;
    GError  *err       = NULL;
    gint     status    = 0;
    gboolean ok        = FALSE;

    *out_text = NULL;
    *out_len  = 0;

    if (!g_spawn_async_with_pipes(
            NULL,       /* working directory: inherit */
            argv,
            NULL,       /* environment:       inherit */
            G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
            NULL, NULL,
            &child_pid,
            &stdin_fd,
            &stdout_fd,
            NULL,       /* stderr: inherit */
            &err)) {
        g_warning("%s: spawn failed: %s", PLUGIN_NAME,
                  err ? err->message : "unknown");
        if (err) g_error_free(err);

        return FALSE;
    }

    /* pipe_write_all() closes stdin_fd on success or error. */
    if (pipe_write_all(stdin_fd, html_bytes, html_len)) {

        /* pipe_read_all() closes stdout_fd on success or error. */
        ok = pipe_read_all(stdout_fd, out_text, out_len);
        stdout_fd = -1; /* already closed */
    } else {
        if (stdout_fd >= 0) {
            if (close(stdout_fd) < 0)
                g_warning("%s: close(stdout): %s", PLUGIN_NAME, g_strerror(errno));
        }
    }

    if (waitpid(child_pid, &status, 0) >= 0) {
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            g_warning("%s: render binary exited with status %d", PLUGIN_NAME, status);
    } else {
            g_warning("%s: error while waiting for render binary to exit", PLUGIN_NAME);
    }

    return ok;
}


/* ------------------------------------------------------------------ */
/* MimeViewer callbacks                                                */
/* ------------------------------------------------------------------ */

static GtkWidget *htmltext_get_widget(MimeViewer *_viewer)
{
    return GTK_WIDGET(((HtmlTextViewer *) _viewer)->scrolled);
}

static void htmltext_show_mimepart(MimeViewer  *_viewer,
                                   const gchar *infile,
                                   MimeInfo    *partinfo)
{
    HtmlTextViewer *viewer = (HtmlTextViewer *) _viewer;
    GtkTextBuffer  *buf    =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(viewer->textview));
    gchar  *html  = NULL;
    gsize   hlen  = 0;
    gchar  *plain = NULL;
    gsize   plen  = 0;
    GError *err   = NULL;

    (void) infile; /* not used directly */

    /* Clean up previous temp file. */
    if (viewer->tmp_filename) {
        claws_unlink(viewer->tmp_filename);
        g_free(viewer->tmp_filename);
        viewer->tmp_filename = NULL;
    }

    viewer->tmp_filename = procmime_get_tmp_file_name(partinfo);
    if (!viewer->tmp_filename) {
        gtk_text_buffer_set_text(buf,
            _("[Error: could not create temp file]"), -1);
        return;
    }

    if (procmime_get_part(viewer->tmp_filename, partinfo) < 0) {
        gtk_text_buffer_set_text(buf,
            _("[Error: could not extract MIME part]"), -1);
        return;
    }

    if (!g_file_get_contents(viewer->tmp_filename, &html, &hlen, &err)) {
        gchar *msg = g_strdup_printf(_("[Error reading temp file: %s]"),
                                     err ? err->message : "?");
        gtk_text_buffer_set_text(buf, msg, -1);
        g_free(msg);
        if (err) g_error_free(err);
        return;
    }

    if (!render_html_to_text(html, hlen, &plain, &plen)) {
        gtk_text_buffer_set_text(buf,
            _("[Error: htmltext-render failed "
              "— is it installed and in PATH variable ?]"), -1);
        g_free(html);
        return;
    }
    g_free(html);

    /* Ensure valid UTF-8 before handing to GTK. */
    if (!g_utf8_validate(plain, (gssize) plen, NULL)) {
        gchar *safe = g_utf8_make_valid(plain, (gssize) plen);
        g_free(plain);
        plain = safe;
        plen  = strlen(safe);
    }

    gtk_text_buffer_set_text(buf, plain, (gint) plen);
    g_free(plain);

    {
        GtkTextIter start;
        gtk_text_buffer_get_start_iter(buf, &start);
        gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(viewer->textview),
                                     &start, 0.0, FALSE, 0.0, 0.0);
    }
}

static void htmltext_clear_viewer(MimeViewer *_viewer)
{
    HtmlTextViewer *viewer = (HtmlTextViewer *) _viewer;
    GtkTextBuffer  *buf    =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(viewer->textview));
    gtk_text_buffer_set_text(buf, "", 0);
    if (viewer->tmp_filename) {
        claws_unlink(viewer->tmp_filename);
        g_free(viewer->tmp_filename);
        viewer->tmp_filename = NULL;
    }
}

static void htmltext_destroy_viewer(MimeViewer *_viewer)
{
    HtmlTextViewer *viewer = (HtmlTextViewer *) _viewer;
    if (viewer->tmp_filename) {
        claws_unlink(viewer->tmp_filename);
        g_free(viewer->tmp_filename);
    }
    g_object_unref(viewer->scrolled);
    g_free(viewer);
}

/* ------------------------------------------------------------------ */
/* Factory                                                             */
/* ------------------------------------------------------------------ */

static MimeViewer *htmltext_viewer_create(void)
{
    HtmlTextViewer *viewer = g_new0(HtmlTextViewer, 1);
    GtkWidget *scrolled, *textview;

    viewer->mimeviewer.factory        = &htmltext_viewer_factory;
    viewer->mimeviewer.get_widget     = htmltext_get_widget;
    viewer->mimeviewer.show_mimepart  = htmltext_show_mimepart;
    viewer->mimeviewer.clear_viewer   = htmltext_clear_viewer;
    viewer->mimeviewer.destroy_viewer = htmltext_destroy_viewer;

    scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);

    textview = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(textview), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(textview), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(textview), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(textview), 8);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(textview), 8);
    gtk_text_view_set_pixels_above_lines(GTK_TEXT_VIEW(textview), 3);
    gtk_text_view_set_pixels_inside_wrap(GTK_TEXT_VIEW(textview), 3);

    gtk_container_add(GTK_CONTAINER(scrolled), textview);
    gtk_widget_show_all(scrolled);
    g_object_ref(scrolled);

    viewer->scrolled      = scrolled;
    viewer->textview      = textview;
    viewer->tmp_filename  = NULL;

    return (MimeViewer *) viewer;
}

static gchar *content_types[] = { "text/html", NULL };

static MimeViewerFactory htmltext_viewer_factory = {
    content_types,
    0,
    htmltext_viewer_create,
};

/* ------------------------------------------------------------------ */
/* Plugin entry points                                                 */
/* ------------------------------------------------------------------ */

gint plugin_init(gchar **error)
{
    if (!check_plugin_version(MAKE_NUMERIC_VERSION(0, 0, 1, 0),
                              VERSION_NUMERIC,
                              PLUGIN_NAME, error))
        return -1;

    mimeview_register_viewer_factory(&htmltext_viewer_factory);
    return 0;
}

gboolean plugin_done(void)
{
    mimeview_unregister_viewer_factory(&htmltext_viewer_factory);
    return TRUE;
}

const gchar *plugin_name(void)    { return PLUGIN_NAME; }
const gchar *plugin_type(void)    { return "GTK3"; }
const gchar *plugin_licence(void) { return "GPL3+"; }
const gchar *plugin_version(void) { return VERSION; }

const gchar *plugin_desc(void)
{
    return _(
        "Renders HTML e-mail parts as plain text by piping the raw HTML "
        "through the 'htmltext-render' helper binary.\n"
        "\n"
        "No JavaScript, no images, no remote resources are fetched.\n"
        "Install 'htmltext-render' somewhere in your PATH.");
}

struct PluginFeature *plugin_provides(void)
{
    static struct PluginFeature features[] = {
        { PLUGIN_MIMEVIEWER, "text/html" },
        { PLUGIN_NOTHING,    NULL        }
    };
    return features;
}
