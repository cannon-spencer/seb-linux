/*
 * WebKitGTK web process extension for seb-linux.
 *
 * WebKitGTK runs page loads in a separate web process, so the UI process
 * (Qt side) cannot intercept subresource requests like QtWebEngine's
 * QWebEngineUrlRequestInterceptor does. This extension runs inside the web
 * process and does the work of seb::browser::RequestInterceptor there:
 *
 *   - URL filter: block rules, then allow rules, default block, matched
 *     case-insensitively against the percent-decoded URL (same as
 *     RequestFilter::evaluate). Main and content requests are filtered
 *     separately (URLFilterEnable / URLFilterEnableContentFilter).
 *   - seb:// and sebs:// are rewritten to http:// and https://; mailto: is
 *     blocked.
 *   - X-SafeExamBrowser-ConfigKeyHash and X-SafeExamBrowser-RequestHash
 *     (sha256 of the URL without fragment + key, hex) on main requests and
 *     on requests to the page's own host.
 *   - The SEB JavaScript API (SafeExamBrowser.security.configKey and
 *     .browserExamKey, hashed with each frame's URL).
 *
 * The UI process passes everything in the initialization user data, a
 * GVariant of type a{sv}:
 *
 *   configKey          s       Config Key (hex), may be empty
 *   browserExamKey     s       Browser Exam Key (hex)
 *   sendConfigKey      b
 *   sendBrowserExamKey b
 *   filterMain         b
 *   filterContent      b
 *   rules              a(sb)   (regex pattern, allow?) in config order
 *   apiScript          s       Api.js with %%_CK_%% / %%_BEK_%% placeholders
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <string.h>

#include <libsoup/soup.h>
#include <webkit2/webkit-web-extension.h>

typedef struct {
    GRegex *regex;
    gboolean allow;
} Rule;

typedef struct {
    gchar *config_key;
    gchar *browser_exam_key;
    gboolean send_config_key;
    gboolean send_browser_exam_key;
    gboolean filter_main;
    gboolean filter_content;
    GPtrArray *block_rules; /* Rule* */
    GPtrArray *allow_rules; /* Rule* */
    gchar *api_script;
} SebConfig;

static SebConfig config;
static gboolean debug; /* SEB_WEBEXT_DEBUG=1 logs every request */

static void rule_free(gpointer p)
{
    Rule *r = p;
    if (r->regex)
        g_regex_unref(r->regex);
    g_free(r);
}

/* URL without fragment, as SEB hashes it. Caller frees. */
static gchar *strip_fragment(const gchar *uri)
{
    const gchar *hash = strchr(uri, '#');
    return hash ? g_strndup(uri, (gsize)(hash - uri)) : g_strdup(uri);
}

/* sha256_hex(url_without_fragment + key). Caller frees. */
static gchar *keyed_hash(const gchar *uri, const gchar *key)
{
    gchar *url = strip_fragment(uri);
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(sum, (const guchar *)url, -1);
    g_checksum_update(sum, (const guchar *)key, -1);
    gchar *hex = g_strdup(g_checksum_get_string(sum));
    g_checksum_free(sum);
    g_free(url);
    return hex;
}

/* Lowercase host of a URI, or NULL. Caller frees. */
static gchar *uri_host(const gchar *uri)
{
    if (!uri)
        return NULL;
    GUri *parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
    if (!parsed)
        return NULL;
    gchar *host = g_uri_get_host(parsed) ? g_ascii_strdown(g_uri_get_host(parsed), -1) : NULL;
    g_uri_unref(parsed);
    return host;
}

static gboolean same_host(const gchar *a, const gchar *b)
{
    gchar *ha = uri_host(a);
    gchar *hb = uri_host(b);
    gboolean same = ha && hb && g_strcmp0(ha, hb) == 0;
    g_free(ha);
    g_free(hb);
    return same;
}

/* TRUE when the request may load. */
static gboolean filter_allows(const gchar *uri, gboolean is_main)
{
    if ((is_main && !config.filter_main) || (!is_main && !config.filter_content))
        return TRUE;

    gchar *decoded = g_uri_unescape_string(uri, NULL);
    const gchar *subject = decoded ? decoded : uri;
    gboolean allowed = FALSE;
    gboolean decided = FALSE;

    for (guint i = 0; i < config.block_rules->len && !decided; i++) {
        Rule *r = g_ptr_array_index(config.block_rules, i);
        if (g_regex_match(r->regex, subject, 0, NULL)) {
            allowed = FALSE;
            decided = TRUE;
        }
    }
    for (guint i = 0; i < config.allow_rules->len && !decided; i++) {
        Rule *r = g_ptr_array_index(config.allow_rules, i);
        if (g_regex_match(r->regex, subject, 0, NULL)) {
            allowed = TRUE;
            decided = TRUE;
        }
    }
    g_free(decoded);
    return decided ? allowed : FALSE;
}

/* Returns TRUE to cancel the request (WebKit "send-request" semantics). */
static gboolean on_send_request(WebKitWebPage *page,
                                WebKitURIRequest *request,
                                WebKitURIResponse *redirected_response,
                                gpointer user_data)
{
    (void)user_data;
    const gchar *uri = webkit_uri_request_get_uri(request);
    if (!uri)
        return FALSE;

    if (g_str_has_prefix(uri, "mailto:"))
        return TRUE;

    /* Local documents and in-page data never touch the network; the
     * QtWebEngine interceptor does not see them either. */
    if (g_str_has_prefix(uri, "about:") || g_str_has_prefix(uri, "data:") || g_str_has_prefix(uri, "blob:"))
        return FALSE;

    if (debug)
        g_message("seb-web-extension: request %s", uri);

    if (g_str_has_prefix(uri, "seb://") || g_str_has_prefix(uri, "sebs://")) {
        gboolean secure = g_str_has_prefix(uri, "sebs://");
        gchar *rewritten = g_strconcat(secure ? "https://" : "http://", uri + (secure ? 7 : 6), NULL);
        webkit_uri_request_set_uri(request, rewritten);
        g_free(rewritten);
        uri = webkit_uri_request_get_uri(request);
    }

    /* The page URI already points at the main resource while it is being
     * requested; a redirect of the main resource arrives with the previous
     * hop as the redirected response. */
    const gchar *page_uri = webkit_web_page_get_uri(page);
    gboolean is_main = g_strcmp0(uri, page_uri) == 0;
    if (!is_main && redirected_response) {
        const gchar *from = webkit_uri_response_get_uri(redirected_response);
        is_main = from && g_strcmp0(from, page_uri) == 0;
    }

    if (!filter_allows(uri, is_main)) {
        g_message("seb-web-extension: blocked %s request %s", is_main ? "main" : "content", uri);
        return TRUE;
    }

    if (!is_main && !same_host(page_uri, uri))
        return FALSE;

    SoupMessageHeaders *headers = webkit_uri_request_get_http_headers(request);
    if (!headers)
        return FALSE;

    if (config.send_config_key && config.config_key && *config.config_key) {
        gchar *h = keyed_hash(uri, config.config_key);
        soup_message_headers_replace(headers, "X-SafeExamBrowser-ConfigKeyHash", h);
        g_free(h);
    }
    if (config.send_browser_exam_key && config.browser_exam_key && *config.browser_exam_key) {
        gchar *h = keyed_hash(uri, config.browser_exam_key);
        soup_message_headers_replace(headers, "X-SafeExamBrowser-RequestHash", h);
        g_free(h);
    }
    return FALSE;
}

static gchar *replace_all(const gchar *text, const gchar *needle, const gchar *value)
{
    gchar **parts = g_strsplit(text, needle, -1);
    gchar *joined = g_strjoinv(value, parts);
    g_strfreev(parts);
    return joined;
}

/* Defines window.SafeExamBrowser in every frame, keyed to that frame's URL. */
static void on_window_object_cleared(WebKitScriptWorld *world,
                                     WebKitWebPage *page,
                                     WebKitFrame *frame,
                                     gpointer user_data)
{
    (void)page;
    (void)user_data;
    if (!config.api_script || !*config.api_script)
        return;
    const gchar *uri = webkit_frame_get_uri(frame);
    if (!uri || !(g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://")))
        return;

    gchar *ck = config.config_key && *config.config_key ? keyed_hash(uri, config.config_key) : g_strdup("");
    gchar *bek = config.browser_exam_key && *config.browser_exam_key ? keyed_hash(uri, config.browser_exam_key) : g_strdup("");
    gchar *with_ck = replace_all(config.api_script, "%%_CK_%%", ck);
    gchar *script = replace_all(with_ck, "%%_BEK_%%", bek);

    JSCContext *js = webkit_frame_get_js_context_for_script_world(frame, world);
    JSCValue *result = jsc_context_evaluate(js, script, -1);
    JSCException *ex = jsc_context_get_exception(js);
    if (ex) {
        g_warning("seb-web-extension: JS API injection failed: %s", jsc_exception_get_message(ex));
        jsc_context_clear_exception(js);
    }
    if (result)
        g_object_unref(result);
    g_object_unref(js);

    g_free(script);
    g_free(with_ck);
    g_free(bek);
    g_free(ck);
}

static void on_page_created(WebKitWebExtension *extension, WebKitWebPage *page, gpointer user_data)
{
    (void)extension;
    (void)user_data;
    g_signal_connect(page, "send-request", G_CALLBACK(on_send_request), NULL);
}

static void load_config(GVariant *data)
{
    config.block_rules = g_ptr_array_new_with_free_func(rule_free);
    config.allow_rules = g_ptr_array_new_with_free_func(rule_free);
    config.filter_main = TRUE;
    config.filter_content = TRUE;

    if (!data || !g_variant_is_of_type(data, G_VARIANT_TYPE_VARDICT)) {
        /* No config means the UI process did not set us up: fail closed. */
        g_warning("seb-web-extension: no configuration, blocking everything");
        return;
    }

    GVariantDict dict;
    g_variant_dict_init(&dict, data);
    g_variant_dict_lookup(&dict, "configKey", "s", &config.config_key);
    g_variant_dict_lookup(&dict, "browserExamKey", "s", &config.browser_exam_key);
    g_variant_dict_lookup(&dict, "sendConfigKey", "b", &config.send_config_key);
    g_variant_dict_lookup(&dict, "sendBrowserExamKey", "b", &config.send_browser_exam_key);
    g_variant_dict_lookup(&dict, "filterMain", "b", &config.filter_main);
    g_variant_dict_lookup(&dict, "filterContent", "b", &config.filter_content);
    g_variant_dict_lookup(&dict, "apiScript", "s", &config.api_script);

    GVariantIter *rules = NULL;
    if (g_variant_dict_lookup(&dict, "rules", "a(sb)", &rules)) {
        const gchar *pattern = NULL;
        gboolean allow = FALSE;
        while (g_variant_iter_next(rules, "(&sb)", &pattern, &allow)) {
            GError *error = NULL;
            GRegex *re = g_regex_new(pattern, G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, &error);
            if (!re) {
                /* RequestFilter also skips rules that do not compile. */
                g_warning("seb-web-extension: skipping invalid rule %s: %s", pattern, error->message);
                g_clear_error(&error);
                continue;
            }
            Rule *r = g_new0(Rule, 1);
            r->regex = re;
            r->allow = allow;
            g_ptr_array_add(allow ? config.allow_rules : config.block_rules, r);
        }
        g_variant_iter_free(rules);
    }
    g_variant_dict_clear(&dict);

    g_message("seb-web-extension: %u allow / %u block rules, config key %s, browser exam key %s",
              config.allow_rules->len, config.block_rules->len,
              config.send_config_key && config.config_key && *config.config_key ? "on" : "off",
              config.send_browser_exam_key ? "on" : "off");
}

G_MODULE_EXPORT void webkit_web_extension_initialize_with_user_data(WebKitWebExtension *extension,
                                                                    const GVariant *user_data)
{
    debug = g_strcmp0(g_getenv("SEB_WEBEXT_DEBUG"), "1") == 0;
    load_config((GVariant *)user_data);
    g_signal_connect(extension, "page-created", G_CALLBACK(on_page_created), NULL);
    g_signal_connect(webkit_script_world_get_default(), "window-object-cleared",
                     G_CALLBACK(on_window_object_cleared), NULL);
}
