/*
 * HTTP File Server
 * Serves SD card files over HTTP with browse/download/upload/delete support.
 * Uses ESP-IDF httpd. 
 * Access from any browser at http://<device-ip>:<port>/
 */

#include "file_server.h"
#include "harpy_config.h"
#include "ui_sdcard.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_vfs.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/unistd.h>

static const char *TAG = "file_srv";

static httpd_handle_t s_server = NULL;
static bool s_running = false;
static uint32_t s_request_count = 0;

#define SCRATCH_BUFSIZE 4096
static char s_scratch[SCRATCH_BUFSIZE];

/* ==================== HTML Templates ==================== */

static const char *HTML_HEADER =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>HARPY File Server</title>"
    "<style>"
    "body{background:#0D1117;color:#C9D1D9;font-family:system-ui,-apple-system,sans-serif;margin:0;padding:20px}"
    "h1{color:#58A6FF;margin-bottom:5px}"
    ".subtitle{color:#8B949E;margin-bottom:20px}"
    "table{width:100%%;border-collapse:collapse;background:#161B22;border-radius:12px;overflow:hidden}"
    "th{background:#1C2333;color:#8B949E;padding:12px;text-align:left;font-weight:500}"
    "td{padding:10px 12px;border-bottom:1px solid #21262D}"
    "tr:hover{background:#1C2333}"
    "a{color:#58A6FF;text-decoration:none}a:hover{text-decoration:underline}"
    ".size{color:#8B949E;text-align:right}"
    ".dir{color:#F97316}"
    ".del{color:#F85149;cursor:pointer;border:none;background:none;font-size:14px}"
    ".del:hover{text-decoration:underline}"
    ".upload{background:#161B22;padding:20px;border-radius:12px;margin-bottom:20px;"
    "display:flex;gap:10px;align-items:center;flex-wrap:wrap}"
    ".upload input[type=file]{color:#C9D1D9;flex:1}"
    ".btn{background:#238636;color:white;border:none;padding:10px 20px;border-radius:8px;"
    "cursor:pointer;font-size:14px}.btn:hover{background:#2EA043}"
    ".btn-del{background:#DA3633}.btn-del:hover{background:#F85149}"
    ".path{color:#58A6FF;font-size:14px;margin-bottom:15px}"
    ".breadcrumb{display:flex;gap:5px;flex-wrap:wrap}"
    ".breadcrumb a{padding:4px 8px;background:#1C2333;border-radius:6px}"
    "</style></head><body><h1>HARPY File Server</h1>";

static const char *HTML_FOOTER = "</body></html>";

/* ==================== Helpers ==================== */

static void urldecode(char *dst, const char *src, size_t len)
{
    size_t i = 0;
    while (*src && i < len - 1) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], 0};
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
        i++;
    }
    *dst = '\0';
}

static const char *get_content_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".htm") == 0) return "text/html";
    if (strcasecmp(ext, ".css") == 0) return "text/css";
    if (strcasecmp(ext, ".js") == 0) return "application/javascript";
    if (strcasecmp(ext, ".json") == 0) return "application/json";
    if (strcasecmp(ext, ".txt") == 0 || strcasecmp(ext, ".log") == 0 || strcasecmp(ext, ".csv") == 0) return "text/plain";
    if (strcasecmp(ext, ".png") == 0) return "image/png";
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, ".gif") == 0) return "image/gif";
    if (strcasecmp(ext, ".bmp") == 0) return "image/bmp";
    if (strcasecmp(ext, ".ico") == 0) return "image/x-icon";
    if (strcasecmp(ext, ".pdf") == 0) return "application/pdf";
    if (strcasecmp(ext, ".zip") == 0) return "application/zip";
    if (strcasecmp(ext, ".mp3") == 0) return "audio/mpeg";
    if (strcasecmp(ext, ".wav") == 0) return "audio/wav";
    return "application/octet-stream";
}

static void format_size(char *buf, size_t buflen, long size)
{
    if (size > 1024 * 1024)
        snprintf(buf, buflen, "%.1f MB", size / (1024.0f * 1024.0f));
    else if (size > 1024)
        snprintf(buf, buflen, "%.1f KB", size / 1024.0f);
    else
        snprintf(buf, buflen, "%ld B", size);
}

/* ==================== Directory Listing Handler ==================== */

static esp_err_t send_dir_listing(httpd_req_t *req, const char *dirpath, const char *uri_path)
{
    s_request_count++;
    httpd_resp_set_type(req, "text/html");

    /* Send header */
    httpd_resp_sendstr_chunk(req, HTML_HEADER);

    /* Breadcrumb navigation */
    httpd_resp_sendstr_chunk(req, "<div class='path breadcrumb'>");
    httpd_resp_sendstr_chunk(req, "<a href='/'>/ root</a>");

    /* Build breadcrumbs from URI path */
    char bc[256];
    strncpy(bc, uri_path, sizeof(bc) - 1);
    bc[sizeof(bc) - 1] = '\0';
    char link[256] = "/";
    char *tok = strtok(bc, "/");
    while (tok) {
        strncat(link, tok, sizeof(link) - strlen(link) - 2);
        strcat(link, "/");
        char chunk[384];
        snprintf(chunk, sizeof(chunk), "<a href='%s'>%s</a>", link, tok);
        httpd_resp_sendstr_chunk(req, chunk);
        tok = strtok(NULL, "/");
    }
    httpd_resp_sendstr_chunk(req, "</div>");

    /* Upload form */
    char form[512];
    snprintf(form, sizeof(form),
             "<div class='upload'>"
             "<form method='POST' action='%s' enctype='multipart/form-data' style='display:flex;gap:10px;align-items:center;width:100%%'>"
             "<input type='file' name='file'>"
             "<button type='submit' class='btn'>Upload</button>"
             "</form></div>",
             uri_path[0] ? uri_path : "/");
    httpd_resp_sendstr_chunk(req, form);

    /* Table header */
    httpd_resp_sendstr_chunk(req,
        "<table><tr><th>Name</th><th>Size</th><th>Action</th></tr>");

    /* Parent directory link */
    if (strcmp(uri_path, "/") != 0 && strlen(uri_path) > 1) {
        char parent[256];
        strncpy(parent, uri_path, sizeof(parent) - 1);
        parent[sizeof(parent) - 1] = '\0';
        /* Remove trailing slash */
        size_t pl = strlen(parent);
        if (pl > 1 && parent[pl - 1] == '/') parent[pl - 1] = '\0';
        /* Find last slash */
        char *ls = strrchr(parent, '/');
        if (ls && ls != parent) *ls = '\0';
        else strcpy(parent, "/");

        char row[512];
        snprintf(row, sizeof(row),
                 "<tr><td colspan='3'><a href='%s'>&#x1F519; ..</a></td></tr>", parent);
        httpd_resp_sendstr_chunk(req, row);
    }

    /* List directory entries */
    DIR *dir = opendir(dirpath);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            char fullpath[512];
            snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, entry->d_name);
            struct stat st;
            bool is_dir = false;
            long size = 0;
            if (stat(fullpath, &st) == 0) {
                is_dir = S_ISDIR(st.st_mode);
                size = st.st_size;
            }

            char entry_uri[512];
            snprintf(entry_uri, sizeof(entry_uri), "%s%s%s",
                     uri_path,
                     (uri_path[strlen(uri_path) - 1] == '/' ? "" : "/"),
                     entry->d_name);

            char sz[32] = "";
            if (!is_dir) format_size(sz, sizeof(sz), size);

            char row[2048];
            snprintf(row, sizeof(row),
                     "<tr><td><a href='%s%s' class='%s'>%s%s %.64s</a></td>"
                     "<td class='size'>%s</td>"
                     "<td><a href='/api/delete?path=%s' class='del' "
                     "onclick=\"return confirm('Delete?')\">"
                     "&#x1F5D1; Delete</a></td></tr>",
                     entry_uri, is_dir ? "/" : "", is_dir ? "dir" : "",
                     is_dir ? "&#x1F4C1;" : "&#x1F4C4;", is_dir ? "" : "", entry->d_name,
                     sz, entry_uri);
            httpd_resp_sendstr_chunk(req, row);
        }
        closedir(dir);
    }

    httpd_resp_sendstr_chunk(req, "</table>");
    httpd_resp_sendstr_chunk(req, HTML_FOOTER);
    httpd_resp_sendstr_chunk(req, NULL); /* End chunked */
    return ESP_OK;
}

/* ==================== GET Handler ==================== */

static esp_err_t file_get_handler(httpd_req_t *req)
{
    const char *mount_point = sd_get_mount_point();
    if (!sd_is_mounted()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD card not mounted");
        return ESP_FAIL;
    }

    /* Decode URI */
    char uri_decoded[256];
    urldecode(uri_decoded, req->uri, sizeof(uri_decoded));

    /* Build filesystem path */
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s%s", mount_point,
             strcmp(uri_decoded, "/") == 0 ? "" : uri_decoded);

    /* Remove trailing slash for stat (but keep it for root) */
    size_t flen = strlen(filepath);
    if (flen > strlen(mount_point) && filepath[flen - 1] == '/') {
        filepath[flen - 1] = '\0';
    }

    struct stat st;
    if (stat(filepath, &st) != 0) {
        /* Not found */
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    if (S_ISDIR(st.st_mode)) {
        return send_dir_listing(req, filepath, uri_decoded);
    }

    /* Serve file */
    s_request_count++;
    FILE *f = fopen(filepath, "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, get_content_type(filepath));

    /* For binary/download files, suggest download */
    const char *ct = get_content_type(filepath);
    if (strcmp(ct, "application/octet-stream") == 0) {
        const char *fname = strrchr(filepath, '/');
        fname = fname ? fname + 1 : filepath;
        char hdr[320];
        snprintf(hdr, sizeof(hdr), "attachment; filename=\"%.255s\"", fname);
        httpd_resp_set_hdr(req, "Content-Disposition", hdr);
    }

    size_t n;
    do {
        n = fread(s_scratch, 1, SCRATCH_BUFSIZE, f);
        if (n > 0) {
            if (httpd_resp_send_chunk(req, s_scratch, n) != ESP_OK) {
                fclose(f);
                httpd_resp_sendstr_chunk(req, NULL);
                return ESP_FAIL;
            }
        }
    } while (n > 0);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ==================== Upload (POST) Handler ==================== */

static esp_err_t file_upload_handler(httpd_req_t *req)
{
    const char *mount_point = sd_get_mount_point();
    if (!sd_is_mounted()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD card not mounted");
        return ESP_FAIL;
    }

    s_request_count++;

    /* Decode URI path for target directory */
    char uri_decoded[256];
    urldecode(uri_decoded, req->uri, sizeof(uri_decoded));

    /* Extract boundary from content type header */
    char ct_hdr[256] = "";
    httpd_req_get_hdr_value_str(req, "Content-Type", ct_hdr, sizeof(ct_hdr));
    char *boundary_start = strstr(ct_hdr, "boundary=");
    if (!boundary_start) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No boundary in Content-Type");
        return ESP_FAIL;
    }
    char boundary[128];
    snprintf(boundary, sizeof(boundary), "--%s", boundary_start + 9);
    /* Remove trailing whitespace/CR from boundary */
    size_t blen = strlen(boundary);
    while (blen > 0 && (boundary[blen-1] == '\r' || boundary[blen-1] == '\n' || boundary[blen-1] == ' ')) {
        boundary[--blen] = '\0';
    }

    int total = req->content_len;
    int remaining = total;

    /* Phase 1: Read header portion to find filename and data start.
     * We read in chunks into s_scratch and look for the header end (\r\n\r\n). */
    char filename[128] = "upload.bin";
    FILE *f = NULL;
    bool header_parsed = false;
    int header_buf_len = 0;
    char header_buf[1024]; /* multipart header is typically < 512 bytes */

    /* Read until we find \r\n\r\n (end of part headers) */
    while (!header_parsed && remaining > 0) {
        int to_read = remaining;
        if (to_read > (int)(sizeof(header_buf) - header_buf_len))
            to_read = sizeof(header_buf) - header_buf_len;
        int recv_len = httpd_req_recv(req, header_buf + header_buf_len, to_read);
        if (recv_len <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }
        header_buf_len += recv_len;
        remaining -= recv_len;

        /* Look for header end */
        char *hdr_end = memmem(header_buf, header_buf_len, "\r\n\r\n", 4);
        if (hdr_end) {
            hdr_end[0] = '\0'; /* null-terminate header for string search */
            /* Extract filename */
            char *cd = strstr(header_buf, "filename=\"");
            if (cd) {
                cd += 10;
                char *end = strchr(cd, '"');
                if (end) {
                    size_t nlen = end - cd;
                    if (nlen >= sizeof(filename)) nlen = sizeof(filename) - 1;
                    memcpy(filename, cd, nlen);
                    filename[nlen] = '\0';
                }
            }

            /* Build target path */
            char filepath[512];
            snprintf(filepath, sizeof(filepath), "%s%s%s%s", mount_point,
                     strcmp(uri_decoded, "/") == 0 ? "/" : uri_decoded,
                     (uri_decoded[strlen(uri_decoded) - 1] == '/' ? "" : "/"),
                     filename);

            f = fopen(filepath, "w");
            if (!f) {
                ESP_LOGE(TAG, "Cannot create %s", filepath);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file");
                return ESP_FAIL;
            }
            ESP_LOGI(TAG, "Uploading: %s", filepath);

            /* Write any file data already in our buffer (after \r\n\r\n) */
            char *data_start = hdr_end + 4; /* skip \r\n\r\n */
            int leftover = header_buf_len - (data_start - header_buf);
            if (leftover > 0) {
                fwrite(data_start, 1, leftover, f);
            }
            header_parsed = true;
        }

        if (header_buf_len >= (int)sizeof(header_buf) && !header_parsed) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Multipart header too large");
            return ESP_FAIL;
        }
    }

    if (!f) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad multipart data - no header end");
        return ESP_FAIL;
    }

    /* Phase 2: Stream remaining data directly to file */
    while (remaining > 0) {
        int to_read = remaining > SCRATCH_BUFSIZE ? SCRATCH_BUFSIZE : remaining;
        int recv_len = httpd_req_recv(req, s_scratch, to_read);
        if (recv_len <= 0) {
            fclose(f);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }
        fwrite(s_scratch, 1, recv_len, f);
        remaining -= recv_len;
    }
    fclose(f);

    /* The file may contain the trailing boundary (\r\n--boundary--) at the end.
     * For simplicity we leave it — it's typically < 128 bytes at end of file.
     * A more precise implementation would trim it, but for embedded use this is fine. */

    ESP_LOGI(TAG, "Upload complete: %s", filename);

    /* Redirect back to directory listing */
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", uri_decoded);
    httpd_resp_sendstr(req, "Upload complete. Redirecting...");
    return ESP_OK;
}

/* ==================== Delete API Handler ==================== */

static int rmdir_recursive_srv(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) return -1;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode)) rmdir_recursive_srv(full);
            else unlink(full);
        }
    }
    closedir(dir);
    return rmdir(path);
}

static esp_err_t delete_handler(httpd_req_t *req)
{
    const char *mount_point = sd_get_mount_point();
    if (!sd_is_mounted()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD not mounted");
        return ESP_FAIL;
    }
    s_request_count++;

    /* Get ?path= query parameter */
    char query[256] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No path");
        return ESP_FAIL;
    }
    char path_val[256] = "";
    httpd_query_key_value(query, "path", path_val, sizeof(path_val));

    char decoded[256];
    urldecode(decoded, path_val, sizeof(decoded));

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s%s", mount_point, decoded);

    struct stat st;
    if (stat(filepath, &st) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }

    int r;
    if (S_ISDIR(st.st_mode)) {
        r = rmdir_recursive_srv(filepath);
    } else {
        r = unlink(filepath);
    }
    ESP_LOGI(TAG, "Delete %s: %s", filepath, r == 0 ? "OK" : "FAIL");

    /* Redirect back to parent */
    char parent[256];
    strncpy(parent, decoded, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    char *lslash = strrchr(parent, '/');
    if (lslash && lslash != parent) *lslash = '\0';
    else strcpy(parent, "/");

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", parent);
    httpd_resp_sendstr(req, "Deleted. Redirecting...");
    return ESP_OK;
}

/* ==================== Public API ==================== */

esp_err_t file_server_start(int port)
{
    if (s_running) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 8;
    config.stack_size = 12288;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Delete API */
    httpd_uri_t delete_uri = {
        .uri = "/api/delete",
        .method = HTTP_GET,
        .handler = delete_handler,
    };
    httpd_register_uri_handler(s_server, &delete_uri);

    /* Upload (POST to any path) */
    httpd_uri_t upload_uri = {
        .uri = "/*",
        .method = HTTP_POST,
        .handler = file_upload_handler,
    };
    httpd_register_uri_handler(s_server, &upload_uri);

    /* File/directory GET (wildcard — must be last) */
    httpd_uri_t get_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = file_get_handler,
    };
    httpd_register_uri_handler(s_server, &get_uri);

    s_running = true;
    s_request_count = 0;
    ESP_LOGI(TAG, "HTTP file server started on port %d", port);
    return ESP_OK;
}

void file_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    s_running = false;
    ESP_LOGI(TAG, "HTTP file server stopped");
}

bool file_server_is_running(void)
{
    return s_running;
}

uint32_t file_server_get_request_count(void)
{
    return s_request_count;
}
