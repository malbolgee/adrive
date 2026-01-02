#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <curl/curl.h>
#include <openssl/evp.h>
#ifdef USE_LOCAL_CJSON
#include "cJSON.h"
#else
#include <cjson/cJSON.h>
#endif

#define ROOT_BASE "https://artifacts.mot.com:443/artifactory"
#define REPO "scratch_US"
#define CHUNK_SIZE (8 * 1024 * 1024)

static char *g_user = NULL;
static char *g_key = NULL;
static CURL *g_curl = NULL;

/**
 * Prints an error message to stderr and exits the program with status 1.
 *
 * @param fmt Format string (printf-style).
 * @param ... Arguments for the format string.
 */
void die(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    exit(1);
}

/**
 * Loads Artifactory credentials from environment variables.
 * Sets global variables g_user and g_key.
 */
void load_basic_auth()
{
    g_user = getenv("ARTIFACTORY_USER");
    g_key = getenv("ARTIFACTORY_API_KEY");
}

/**
 * Determines the repository path to use.
 * If path_hint is provided, it is cleaned and returned.
 * Otherwise, defaults to the ARTIFACTORY_USER.
 * Exits if ARTIFACTORY_USER is not set when path_hint is missing.
 *
 * @param path_hint Optional path provided by the user.
 * @return A newly allocated string containing the path.
 */
char *default_user_path_or_die(const char *path_hint)
{
    if (path_hint && *path_hint)
    {
        // Strip leading slashes
        while (*path_hint == '/')
            path_hint++;
        // Strip trailing slashes (create copy)
        char *p = strdup(path_hint);
        size_t len = strlen(p);
        while (len > 0 && p[len - 1] == '/')
        {
            p[len - 1] = '\0';
            len--;
        }
        return p;
    }
    load_basic_auth();
    if (!g_user)
    {
        die("ARTIFACTORY_USER is required when --path is omitted.");
    }
    // Strip slashes from user
    char *p = strdup(g_user);
    while (*p == '/')
        p++;
    size_t len = strlen(p);
    while (len > 0 && p[len - 1] == '/')
    {
        p[len - 1] = '\0';
        len--;
    }
    return p;
}

/**
 * Formats a byte size into a human-readable string (e.g., "1.50 MB").
 *
 * @param size Size in bytes.
 * @return A newly allocated string containing the formatted size.
 */
char *human_size(double size)
{
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (size >= 1024 && i < 4)
    {
        size /= 1024.0;
        i++;
    }
    char *buf;
    asprintf(&buf, "%.2f %s", size, units[i]);
    return buf;
}

/**
 * Formats a duration in seconds into a human-readable string (e.g., "1h 30m 10s").
 *
 * @param seconds Duration in seconds.
 * @return A newly allocated string containing the formatted duration.
 */
char *human_duration(double seconds)
{
    long s_total = (long)seconds;
    long d = s_total / 86400;
    long r = s_total % 86400;
    long h = r / 3600;
    r %= 3600;
    long m = r / 60;
    long s = r % 60;

    char *buf;
    if (d > 0)
        asprintf(&buf, "%ldd %ldh %ldm %lds", d, h, m, s);
    else if (h > 0)
        asprintf(&buf, "%ldh %ldm %lds", h, m, s);
    else if (m > 0)
        asprintf(&buf, "%ldm %lds", m, s);
    else
        asprintf(&buf, "%lds", s);
    return buf;
}

typedef struct
{
    const char *prefix;
    curl_off_t total;
    curl_off_t current;
    curl_off_t base_done;
    double start_time;
    double last_update;
    char *human_total;
} ProgressState;

/**
 * Callback function for libcurl to report transfer progress.
 * Updates the progress bar on stdout.
 *
 * @param clientp Pointer to ProgressState struct.
 * @param dltotal Total bytes to download.
 * @param dlnow Bytes downloaded so far.
 * @param ultotal Total bytes to upload.
 * @param ulnow Bytes uploaded so far.
 * @return 0 to continue, non-zero to abort transfer.
 */
int progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    ProgressState *st = (ProgressState *)clientp;

    // Use monotonic clock for better precision
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now_mono = ts.tv_sec + ts.tv_nsec / 1e9;

    // Determine if we are uploading or downloading
    curl_off_t current = (ultotal > 0) ? ulnow : dlnow;
    curl_off_t total = (ultotal > 0) ? ultotal : dltotal;

    // Override total if we know it from elsewhere (e.g. file size on upload, or Content-Length on download)
    if (st->total > 0)
        total = st->total;

    // For download resume, dlnow is bytes received *this session*.
    // We want to show total progress.
    curl_off_t effective_done = current + st->base_done;
    curl_off_t effective_total = total + st->base_done;
    // Note: If total is 0 (unknown), we can't calculate pct.

    if (effective_total == 0)
        return 0;

    // Throttle updates: 0.2s
    if (effective_done < effective_total && (now_mono - st->last_update < 0.2))
    {
        return 0;
    }
    st->last_update = now_mono;

    double elapsed = now_mono - st->start_time;
    if (elapsed < 1e-6)
        elapsed = 1e-6;

    double speed = (double)current / elapsed; // Speed is based on this session's bytes
    double eta = (speed > 0) ? (double)(effective_total - effective_done) / speed : 0;

    double pct = (double)effective_done / effective_total;
    int bar_len = 30;
    int filled = (int)(pct * bar_len);
    if (filled > bar_len)
        filled = bar_len;

    char bar[32];
    memset(bar, 0, sizeof(bar));
    for (int i = 0; i < filled; i++)
        bar[i] = '#';
    for (int i = filled; i < bar_len; i++)
        bar[i] = '-';

    char *s_done = human_size((double)effective_done);
    char *s_total = st->human_total ? strdup(st->human_total) : human_size((double)effective_total);
    char *s_speed = human_size(speed);
    char *s_elapsed = human_duration(elapsed);
    char *s_eta = human_duration(eta);

    printf("\r%s [%s] %6.2f%% %s/%s speed=%s/s elapsed=%s ETA=%s\033[K",
           st->prefix, bar, pct * 100.0, s_done, s_total, s_speed, s_elapsed, s_eta);
    fflush(stdout);

    free(s_done);
    free(s_total);
    free(s_speed);
    free(s_elapsed);
    free(s_eta);

    return 0;
}

/**
 * Calculates the SHA1 hash of a file and returns the first 8 characters.
 *
 * @param path Path to the file.
 * @return A newly allocated string containing the short SHA1 hash, or NULL on error.
 */
char *short_sha1(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx)
    {
        fclose(f);
        return NULL;
    }
    EVP_DigestInit_ex(ctx, EVP_sha1(), NULL);

    unsigned char buf[1024 * 1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        EVP_DigestUpdate(ctx, buf, n);
    }
    fclose(f);

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int len;
    EVP_DigestFinal_ex(ctx, hash, &len);
    EVP_MD_CTX_free(ctx);

    char *out = malloc(9); // 8 chars + null
    for (int i = 0; i < 4; i++)
    {
        sprintf(out + (i * 2), "%02x", hash[i]);
    }
    out[8] = '\0';
    return out;
}

/**
 * Appends a suffix to a filename, preserving the extension.
 * e.g., "file.txt", "hash" -> "file__hash.txt"
 *
 * @param filename Original filename.
 * @param suffix Suffix to append.
 * @return A newly allocated string containing the new filename.
 */
char *with_hash_suffix(const char *filename, const char *suffix)
{
    char *dot = strrchr(filename, '.');
    char *out;
    if (dot && dot != filename)
    {
        // has extension
        int base_len = dot - filename;
        asprintf(&out, "%.*s__%s%s", base_len, filename, suffix, dot);
    }
    else
    {
        asprintf(&out, "%s__%s", filename, suffix);
    }
    return out;
}

struct HeaderStruct
{
    char *content_type;
    char *content_encoding;
};

static size_t HeaderCallback(char *buffer, size_t size, size_t nitems, void *userdata)
{
    size_t numbytes = size * nitems;
    struct HeaderStruct *hs = (struct HeaderStruct *)userdata;

    char *line = strndup(buffer, numbytes);
    // Trim newline
    char *end = line + strlen(line) - 1;
    while (end > line && isspace((unsigned char)*end))
        *end-- = '\0';

    if (strncasecmp(line, "Content-Type:", 13) == 0)
    {
        char *val = line + 13;
        while (isspace((unsigned char)*val))
            val++;
        if (hs->content_type)
            free(hs->content_type);
        hs->content_type = strdup(val);
    }
    else if (strncasecmp(line, "Content-Encoding:", 17) == 0)
    {
        char *val = line + 17;
        while (isspace((unsigned char)*val))
            val++;
        if (hs->content_encoding)
            free(hs->content_encoding);
        hs->content_encoding = strdup(val);
    }
    free(line);
    return numbytes;
}

char *get_mime_type_from_file(const char *filename)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "file --mime-type -b \"%s\"", filename);
    FILE *fp = popen(cmd, "r");
    if (!fp)
        return NULL;
    char buf[256];
    if (fgets(buf, sizeof(buf), fp))
    {
        char *p = strchr(buf, '\n');
        if (p)
            *p = 0;
        pclose(fp);
        return strdup(buf);
    }
    pclose(fp);
    return NULL;
}

void get_config_path(char *buf, size_t size)
{
    const char *home = getenv("HOME");
    if (!home)
        home = ".";
    snprintf(buf, size, "%s/.adrive/data_config.json", home);
}

void ensure_config_dir()
{
    const char *home = getenv("HOME");
    if (!home)
        return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/.adrive", home);
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
    {
        fprintf(stderr, "Warning: Could not create config directory %s\n", path);
    }
}

void perform_extraction(const char *filename, const char *ctype, const char *cenc)
{
    // 1. Determine effective type
    char *eff_type = ctype ? strdup(ctype) : NULL;
    if (eff_type)
    {
        // Strip parameters (e.g. ; charset=utf-8)
        char *p = strchr(eff_type, ';');
        if (p)
            *p = 0;
        // Trim trailing spaces
        p = eff_type + strlen(eff_type) - 1;
        while (p > eff_type && isspace((unsigned char)*p))
            *p-- = 0;
    }

    if (!eff_type || strcmp(eff_type, "application/octet-stream") == 0)
    {
        if (eff_type)
            free(eff_type);
        eff_type = get_mime_type_from_file(filename);
    }

    if (!eff_type)
    {
        printf("Could not determine file type for extraction.\n");
        return;
    }

    printf("Extraction: Type='%s', Encoding='%s'\n", eff_type, cenc ? cenc : "(null)");

    // 2. Load config
    char config_path[1024];
    get_config_path(config_path, sizeof(config_path));

    cJSON *root = NULL;
    FILE *f = fopen(config_path, "rb");
    if (f)
    {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *data = malloc(len + 1);
        if (data)
        {
            fread(data, 1, len, f);
            data[len] = 0;
            root = cJSON_Parse(data);
            free(data);
        }
        fclose(f);
    }

    if (!root)
    {
        root = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "configs", cJSON_CreateArray());
    }

    cJSON *configs = cJSON_GetObjectItem(root, "configs");
    if (!configs)
    {
        configs = cJSON_CreateArray();
        cJSON_AddItemToObject(root, "configs", configs);
    }

    // 3. Find match
    cJSON *match = NULL;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, configs)
    {
        cJSON *j_ext = cJSON_GetObjectItem(item, "ext");
        cJSON *j_enc = cJSON_GetObjectItem(item, "encoding");

        int ext_match = (cJSON_IsString(j_ext) && strcmp(j_ext->valuestring, eff_type) == 0);
        int enc_match = 0;
        if (cenc == NULL)
            enc_match = cJSON_IsNull(j_enc);
        else
            enc_match = (cJSON_IsString(j_enc) && strcmp(j_enc->valuestring, cenc) == 0);

        if (ext_match && enc_match)
        {
            match = item;
            break;
        }
    }

    char *prog = NULL;
    char *attrs = NULL;

    if (match)
    {
        prog = strdup(cJSON_GetObjectItem(match, "prog")->valuestring);
        cJSON *j_attrs = cJSON_GetObjectItem(match, "attrs");
        if (cJSON_IsString(j_attrs))
            attrs = strdup(j_attrs->valuestring);
    }
    else
    {
        // 4. Ask user
        printf("No extraction config found for this file type.\n");
        printf("Enter program to use (e.g. tar): ");
        char buf[256];
        if (fgets(buf, sizeof(buf), stdin))
        {
            char *p = strchr(buf, '\n');
            if (p)
                *p = 0;
            prog = strdup(buf);
        }
        printf("Enter flags (e.g. xzvf) or leave empty: ");
        if (fgets(buf, sizeof(buf), stdin))
        {
            char *p = strchr(buf, '\n');
            if (p)
                *p = 0;
            if (strlen(buf) > 0)
                attrs = strdup(buf);
        }

        // Save
        cJSON *new_entry = cJSON_CreateObject();
        cJSON_AddStringToObject(new_entry, "ext", eff_type);
        if (cenc)
            cJSON_AddStringToObject(new_entry, "encoding", cenc);
        else
            cJSON_AddNullToObject(new_entry, "encoding");
        cJSON_AddStringToObject(new_entry, "prog", prog);
        if (attrs)
            cJSON_AddStringToObject(new_entry, "attrs", attrs);
        else
            cJSON_AddNullToObject(new_entry, "attrs");

        cJSON_AddItemToArray(configs, new_entry);

        ensure_config_dir();
        f = fopen(config_path, "wb");
        if (f)
        {
            char *out = cJSON_Print(root);
            fprintf(f, "%s", out);
            free(out);
            fclose(f);
        }
    }

    // 5. Execute
    if (prog)
    {
        char cmd[2048];
        snprintf(cmd, sizeof(cmd), "%s %s %s", prog, attrs ? attrs : "", filename);
        printf("Executing: %s\n", cmd);
        int ret = system(cmd);
        if (ret != 0)
            printf("Extraction failed with code %d\n", ret);
        else
            printf("Extraction successful.\n");
    }

    if (prog)
        free(prog);
    if (attrs)
        free(attrs);
    if (eff_type)
        free(eff_type);
    cJSON_Delete(root);
}

struct MemoryStruct
{
    char *memory;
    size_t size;
};

/**
 * Callback function for libcurl to write received data into memory.
 * Reallocates the memory buffer as needed.
 *
 * @param contents Pointer to data received.
 * @param size Size of one element.
 * @param nmemb Number of elements.
 * @param userp Pointer to MemoryStruct.
 * @return Number of bytes handled.
 */
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr)
        return 0; // OOM
    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;
    return realsize;
}

/**
 * Uploads a file to Artifactory.
 *
 * @param dest_path Destination path in the repository (optional).
 * @param local_file Path to the local file to upload.
 * @param filename_override Optional filename to use in Artifactory.
 */
void cmd_upload(const char *dest_path, const char *local_file, const char *filename_override)
{
    load_basic_auth();
    if (!g_user || !g_key)
        die("export ARTIFACTORY_USER and ARTIFACTORY_API_KEY.");

    if (access(local_file, F_OK) != 0)
        die("File not found: %s", local_file);

    char *targetpath = default_user_path_or_die(dest_path);

    const char *original_name = filename_override;
    if (!original_name)
    {
        char *slash = strrchr(local_file, '/');
        original_name = slash ? slash + 1 : local_file;
    }

    char *hash = short_sha1(local_file);
    char *filename = with_hash_suffix(original_name, hash);
    free(hash);

    char *url;
    asprintf(&url, "%s/%s/%s/%s", ROOT_BASE, REPO, targetpath, filename);

    struct stat st;
    stat(local_file, &st);
    curl_off_t fsize = st.st_size;

    FILE *fd = fopen(local_file, "rb");
    if (!fd)
        die("Could not open file %s", local_file);

    g_curl = curl_easy_init();
    if (g_curl)
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);

        ProgressState pstate = {0};
        pstate.prefix = "Uploading";
        pstate.total = fsize;
        pstate.start_time = ts.tv_sec + ts.tv_nsec / 1e9;
        pstate.human_total = human_size((double)fsize);

        curl_easy_setopt(g_curl, CURLOPT_URL, url);
        curl_easy_setopt(g_curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(g_curl, CURLOPT_READDATA, fd);
        curl_easy_setopt(g_curl, CURLOPT_INFILESIZE_LARGE, fsize);
        curl_easy_setopt(g_curl, CURLOPT_USERNAME, g_user);
        curl_easy_setopt(g_curl, CURLOPT_PASSWORD, g_key);
        curl_easy_setopt(g_curl, CURLOPT_TIMEOUT, 300L);
        curl_easy_setopt(g_curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
        curl_easy_setopt(g_curl, CURLOPT_XFERINFODATA, &pstate);
        curl_easy_setopt(g_curl, CURLOPT_NOPROGRESS, 0L);

        // Capture response for error handling
        struct MemoryStruct chunk;
        chunk.memory = malloc(1);
        chunk.size = 0;
        curl_easy_setopt(g_curl, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);

        printf("\n");
        CURLcode res = curl_easy_perform(g_curl);
        printf("\n");

        long response_code;
        curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &response_code);

        if (res != CURLE_OK || response_code >= 400)
        {
            fprintf(stderr, "HTTP error: %ld\n%s\n", response_code, chunk.memory);
            exit(1);
        }
        else
        {
            // Pretty print response
            cJSON *json = cJSON_Parse(chunk.memory);
            if (json)
            {
                char *s = cJSON_Print(json);
                printf("%s\n", s);
                free(s);
                cJSON_Delete(json);
            }
            else
            {
                printf("%s\n", chunk.memory);
            }
        }
        free(chunk.memory);
        free(pstate.human_total);
        curl_easy_cleanup(g_curl);
    }
    fclose(fd);
    free(targetpath);
    free(filename);
    free(url);
}

/**
 * Lists files in a specific path in Artifactory.
 *
 * @param path Path to list files from.
 */
void cmd_list(const char *path)
{
    load_basic_auth();
    if (!g_user || !g_key)
        die("Set ARTIFACTORY_USER and ARTIFACTORY_API_KEY.");

    char *p = default_user_path_or_die(path);
    char *url;
    asprintf(&url, "%s/api/storage/%s/%s?list&deep=1&listFolders=0", ROOT_BASE, REPO, p);

    g_curl = curl_easy_init();
    struct MemoryStruct chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;

    curl_easy_setopt(g_curl, CURLOPT_URL, url);
    curl_easy_setopt(g_curl, CURLOPT_USERNAME, g_user);
    curl_easy_setopt(g_curl, CURLOPT_PASSWORD, g_key);
    curl_easy_setopt(g_curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(g_curl, CURLOPT_WRITEDATA, (void *)&chunk);

    CURLcode res = curl_easy_perform(g_curl);
    long response_code;
    curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &response_code);

    if (res != CURLE_OK || response_code >= 400)
    {
        die("HTTP error: %ld\n%s", response_code, chunk.memory);
    }

    cJSON *json = cJSON_Parse(chunk.memory);
    if (!json)
        die("Failed to parse JSON");

    cJSON *files = cJSON_GetObjectItem(json, "files");
    if (!files || cJSON_GetArraySize(files) == 0)
    {
        printf("No files found.\n");
    }
    else
    {
        printf("Files under %s/%s:\n\n", REPO, p);
        cJSON *item;
        cJSON_ArrayForEach(item, files)
        {
            cJSON *folder = cJSON_GetObjectItem(item, "folder");
            if (folder && (cJSON_IsTrue(folder) || (cJSON_IsString(folder) && strcasecmp(folder->valuestring, "true") == 0)))
            {
                continue;
            }
            char *uri = cJSON_GetObjectItem(item, "uri")->valuestring;
            // uri starts with /, strip it
            if (uri && *uri == '/')
                uri++;

            double size = 0;
            cJSON *j_size = cJSON_GetObjectItem(item, "size");
            if (cJSON_IsNumber(j_size))
                size = j_size->valuedouble;
            else if (cJSON_IsString(j_size))
                size = atof(j_size->valuestring);

            char *sha1 = cJSON_GetObjectItem(item, "sha1")->valuestring;
            char *lm = cJSON_GetObjectItem(item, "lastModified")->valuestring;

            char *hsize = human_size(size);
            printf("  %s  size=%s  sha1=%s  lastModified=%s\n", uri, hsize, sha1, lm);
            free(hsize);
        }
    }

    cJSON_Delete(json);
    free(chunk.memory);
    free(url);
    free(p);
    curl_easy_cleanup(g_curl);
}

typedef struct
{
    char *uri;
    char *lastModified;
} FileEntry;

/**
 * Comparator function for qsort to sort FileEntry structs by lastModified date (descending).
 *
 * @param a Pointer to first FileEntry.
 * @param b Pointer to second FileEntry.
 * @return Integer less than, equal to, or greater than zero.
 */
int compare_files(const void *a, const void *b)
{
    FileEntry *fa = (FileEntry *)a;
    FileEntry *fb = (FileEntry *)b;
    // Reverse sort
    return strcmp(fb->lastModified, fa->lastModified);
}

/**
 * Searches for an artifact by its SHA1 hash.
 *
 * @param sha1 SHA1 hash to search for.
 * @return A newly allocated string containing the path/filename relative to repo root, or NULL if not found.
 */
char *find_by_sha1(const char *sha1)
{
    char *url;
    asprintf(&url, "%s/api/search/checksum?sha1=%s&repos=%s", ROOT_BASE, sha1, REPO);

    struct MemoryStruct chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;

    CURL *curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERNAME, g_user);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, g_key);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

    CURLcode res = curl_easy_perform(curl);
    long code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    free(url);

    if (res != CURLE_OK || code != 200)
    {
        free(chunk.memory);
        return NULL;
    }

    cJSON *json = cJSON_Parse(chunk.memory);
    free(chunk.memory);
    if (!json)
        return NULL;

    char *result = NULL;
    cJSON *results = cJSON_GetObjectItem(json, "results");
    cJSON *item;
    char needle[256];
    snprintf(needle, sizeof(needle), "/api/storage/%s/", REPO);

    cJSON_ArrayForEach(item, results)
    {
        char *uri = cJSON_GetObjectItem(item, "uri")->valuestring;
        char *pos = strstr(uri, needle);
        if (pos)
        {
            // Extract path/filename
            result = strdup(pos + strlen(needle));
            break;
        }
    }
    cJSON_Delete(json);
    return result;
}

// Struct to manage file writing during download
typedef struct
{
    FILE *f;
    const char *out_path;
    int resume_attempted;
    curl_off_t resume_pos;
} WriteContext;

/**
 * Callback function for libcurl to write received data to a file.
 * Handles file opening (overwrite vs append) based on HTTP response code.
 *
 * @param buffer Pointer to data received.
 * @param size Size of one element.
 * @param nmemb Number of elements.
 * @param userp Pointer to WriteContext.
 * @return Number of bytes written.
 */
static size_t FileWriteCallback(void *buffer, size_t size, size_t nmemb, void *userp)
{
    WriteContext *ctx = (WriteContext *)userp;

    if (!ctx->f)
    {
        // File not open yet. Check response code to decide mode.
        long code = 0;
        curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &code);

        if (code == 206)
        {
            ctx->f = fopen(ctx->out_path, "ab");
        }
        else
        {
            // If we tried to resume but got 200, we must overwrite
            ctx->f = fopen(ctx->out_path, "wb");
            ctx->resume_pos = 0; // Reset effective resume pos for progress bar
        }
        if (!ctx->f)
            return 0; // Error
    }

    return fwrite(buffer, size, nmemb, ctx->f);
}

/**
 * Downloads a file from Artifactory.
 *
 * @param path Path in the repository.
 * @param name_arg Filename (optional if --id or --last is used).
 * @param last Boolean flag to download the latest file.
 * @param id SHA1 hash to search for (optional).
 * @param out_arg Output filename (optional).
 */
void cmd_download(const char *path, const char *name_arg, int last, const char *id, const char *out_arg, int extract)
{
    load_basic_auth();
    if (!g_user || !g_key)
        die("Set ARTIFACTORY_USER and ARTIFACTORY_API_KEY.");

    char *final_path = NULL; // Path inside repo
    char *final_name = NULL; // Filename

    if (id && !name_arg)
    {
        char *resolved = find_by_sha1(id);
        if (!resolved)
            die("No artifact with sha1=%s found in repo %s", id, REPO);

        // resolved is "path/filename" or just "filename"
        char *slash = strrchr(resolved, '/');
        if (slash)
        {
            *slash = '\0';
            final_path = strdup(resolved);
            final_name = strdup(slash + 1);
        }
        else
        {
            final_path = strdup("");
            final_name = strdup(resolved);
        }
        free(resolved);
        printf("Resolved SHA1 at repo root.\n");
    }
    else
    {
        char *p = default_user_path_or_die(path);
        final_path = p;

        if (last && !name_arg)
        {
            char *list_url;
            asprintf(&list_url, "%s/api/storage/%s/%s?list&deep=1&listFolders=0", ROOT_BASE, REPO, p);

            struct MemoryStruct chunk = {malloc(1), 0};
            CURL *curl = curl_easy_init();
            curl_easy_setopt(curl, CURLOPT_URL, list_url);
            curl_easy_setopt(curl, CURLOPT_USERNAME, g_user);
            curl_easy_setopt(curl, CURLOPT_PASSWORD, g_key);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);
            curl_easy_perform(curl);
            curl_easy_cleanup(curl);
            free(list_url);

            cJSON *json = cJSON_Parse(chunk.memory);
            free(chunk.memory);
            if (!json)
                die("Failed to list files for --last");

            cJSON *files = cJSON_GetObjectItem(json, "files");
            if (!files)
                die("No files found.");

            // Collect files
            int count = cJSON_GetArraySize(files);
            if (count == 0)
                die("No files found.");

            FileEntry *entries = malloc(sizeof(FileEntry) * count);
            int valid_count = 0;
            cJSON *item;
            cJSON_ArrayForEach(item, files)
            {
                cJSON *folder = cJSON_GetObjectItem(item, "folder");
                if (folder && (cJSON_IsTrue(folder) || (cJSON_IsString(folder) && strcasecmp(folder->valuestring, "true") == 0)))
                    continue;

                entries[valid_count].uri = strdup(cJSON_GetObjectItem(item, "uri")->valuestring);
                entries[valid_count].lastModified = strdup(cJSON_GetObjectItem(item, "lastModified")->valuestring);
                valid_count++;
            }

            if (valid_count == 0)
                die("No files found.");
            qsort(entries, valid_count, sizeof(FileEntry), compare_files);

            char *uri = entries[0].uri;
            if (*uri == '/')
                uri++;

            final_name = strdup(uri);
            printf("Selected latest file: %s\n", final_name);

            // Cleanup
            for (int i = 0; i < valid_count; i++)
            {
                free(entries[i].uri);
                free(entries[i].lastModified);
            }
            free(entries);
            cJSON_Delete(json);
        }
        else
        {
            if (name_arg)
                final_name = strdup(name_arg);
        }
    }

    if (!final_name)
        die("Must provide --name, --id, or --last");

    char *url;
    if (final_path && *final_path)
    {
        asprintf(&url, "%s/%s/%s/%s", ROOT_BASE, REPO, final_path, final_name);
    }
    else
    {
        asprintf(&url, "%s/%s/%s", ROOT_BASE, REPO, final_name);
    }

    const char *out_file = out_arg ? out_arg : final_name;

    // Resume logic
    curl_off_t resume_pos = 0;
    struct stat st;
    if (stat(out_file, &st) == 0)
    {
        resume_pos = st.st_size;
    }

    g_curl = curl_easy_init();

    WriteContext wctx = {0};
    wctx.out_path = out_file;
    wctx.resume_attempted = (resume_pos > 0);
    wctx.resume_pos = resume_pos;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    ProgressState pstate = {0};
    pstate.prefix = "Downloading";
    pstate.start_time = ts.tv_sec + ts.tv_nsec / 1e9;
    pstate.base_done = resume_pos;

    curl_easy_setopt(g_curl, CURLOPT_URL, url);
    curl_easy_setopt(g_curl, CURLOPT_USERNAME, g_user);
    curl_easy_setopt(g_curl, CURLOPT_PASSWORD, g_key);
    curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, FileWriteCallback);
    curl_easy_setopt(g_curl, CURLOPT_WRITEDATA, &wctx);
    curl_easy_setopt(g_curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(g_curl, CURLOPT_XFERINFODATA, &pstate);
    curl_easy_setopt(g_curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(g_curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(g_curl, CURLOPT_FAILONERROR, 1L); // To catch 416

    // Capture headers for extraction
    struct HeaderStruct hs = {0};
    curl_easy_setopt(g_curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(g_curl, CURLOPT_HEADERDATA, &hs);

    if (resume_pos > 0)
    {
        curl_easy_setopt(g_curl, CURLOPT_RESUME_FROM_LARGE, resume_pos);
    }

    printf("\n");
    CURLcode res = curl_easy_perform(g_curl);

    // Handle 416 Range Not Satisfiable (file might be smaller than resume_pos or fully done)
    long code;
    curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &code);

    if (res != CURLE_OK && code == 416)
    {
        // Retry from 0
        if (wctx.f)
            fclose(wctx.f);
        wctx.f = NULL;
        wctx.resume_pos = 0;
        pstate.base_done = 0;

        curl_easy_setopt(g_curl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)0);
        res = curl_easy_perform(g_curl);
    }

    if (wctx.f)
        fclose(wctx.f);

    if (res != CURLE_OK)
    {
        die("Download failed: %s", curl_easy_strerror(res));
    }

    printf("\nSaved to %s\n", out_file);

    if (extract)
    {
        perform_extraction(out_file, hs.content_type, hs.content_encoding);
    }

    free(hs.content_type);
    free(hs.content_encoding);

    free(url);
    free(final_path);
    free(final_name);
    curl_easy_cleanup(g_curl);
}

/**
 * Main entry point. Parses command line arguments and dispatches commands.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return Exit status code.
 */
int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <upload|list|download> [args...]\n", argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "upload") == 0)
    {
        char *dest_path = NULL;
        char *filename = NULL;
        char *file = NULL;

        for (int i = 2; i < argc; i++)
        {
            if (strcmp(argv[i], "--dest-path") == 0 && i + 1 < argc)
                dest_path = argv[++i];
            else if (strcmp(argv[i], "--filename") == 0 && i + 1 < argc)
                filename = argv[++i];
            else
                file = argv[i];
        }
        if (!file)
            die("Missing file argument");
        cmd_upload(dest_path, file, filename);
    }
    else if (strcmp(cmd, "list") == 0)
    {
        char *path = NULL;
        for (int i = 2; i < argc; i++)
        {
            if (strcmp(argv[i], "--path") == 0 && i + 1 < argc)
                path = argv[++i];
        }
        cmd_list(path);
    }
    else if (strcmp(cmd, "download") == 0)
    {
        char *path = NULL;
        char *name = NULL;
        char *id = NULL;
        char *out = NULL;
        int last = 0;
        int extract = 0;

        for (int i = 2; i < argc; i++)
        {
            if (strcmp(argv[i], "--path") == 0 && i + 1 < argc)
                path = argv[++i];
            else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc)
                name = argv[++i];
            else if (strcmp(argv[i], "--id") == 0 && i + 1 < argc)
                id = argv[++i];
            else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)
                out = argv[++i];
            else if (strcmp(argv[i], "--last") == 0)
                last = 1;
            else if (strcmp(argv[i], "--extract") == 0)
                extract = 1;
        }
        cmd_download(path, name, last, id, out, extract);
    }
    else
    {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        return 1;
    }

    return 0;
}
