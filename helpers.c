#include "adrive.h"

void die(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    exit(1);
}

void load_basic_auth()
{
    g_user = getenv("ARTIFACTORY_USER");
    g_key = getenv("ARTIFACTORY_API_KEY");
}

char *default_user_path_or_die(const char *path_hint)
{
    if (path_hint && *path_hint)
    {
        while (*path_hint == '/')
            ++path_hint;
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
    char *p = strdup(g_user);
    while (*p == '/')
        ++p;
    size_t len = strlen(p);
    while (len > 0 && p[len - 1] == '/')
    {
        p[len - 1] = '\0';
        --len;
    }
    return p;
}

char *human_size(double size)
{
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (size >= 1024 && i < 4)
    {
        size /= 1024.0;
        ++i;
    }
    char *buf;
    asprintf(&buf, "%.2f %s", size, units[i]);
    return buf;
}

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

int progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    ProgressState *st = (ProgressState *)clientp;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now_mono = ts.tv_sec + ts.tv_nsec / 1e9;

    curl_off_t current = (ultotal > 0) ? ulnow : dlnow;
    curl_off_t total = (ultotal > 0) ? ultotal : dltotal;

    if (st->total > 0)
        total = st->total;

    curl_off_t effective_done = current + st->base_done;
    curl_off_t effective_total = total + st->base_done;

    if (effective_total == 0)
        return 0;

    if (effective_done < effective_total && (now_mono - st->last_update < 0.2))
    {
        return 0;
    }
    st->last_update = now_mono;

    double elapsed = now_mono - st->start_time;
    if (elapsed < 1e-6)
        elapsed = 1e-6;

    double speed = (double)current / elapsed;
    double eta = (speed > 0) ? (double)(effective_total - effective_done) / speed : 0;

    double pct = (double)effective_done / effective_total;
    int bar_len = 30;
    int filled = (int)(pct * bar_len);
    if (filled > bar_len)
        filled = bar_len;

    char bar[32];
    memset(bar, 0, sizeof(bar));
    for (int i = 0; i < filled; ++i)
        bar[i] = '#';
    for (int i = filled; i < bar_len; ++i)
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

    char *out = malloc(9);
    for (int i = 0; i < 4; ++i)
    {
        sprintf(out + (i * 2), "%02x", hash[i]);
    }
    out[8] = '\0';
    return out;
}

char *with_hash_suffix(const char *filename, const char *suffix)
{
    char *dot = strrchr(filename, '.');
    char *out;
    if (dot && dot != filename)
    {
        int base_len = dot - filename;
        asprintf(&out, "%.*s__%s%s", base_len, filename, suffix, dot);
    }
    else
    {
        asprintf(&out, "%s__%s", filename, suffix);
    }
    return out;
}

size_t HeaderCallback(char *buffer, size_t size, size_t nitems, void *userdata)
{
    size_t numbytes = size * nitems;
    struct HeaderStruct *hs = (struct HeaderStruct *)userdata;

    char *line = strndup(buffer, numbytes);
    char *end = line + strlen(line) - 1;
    while (end > line && isspace((unsigned char)*end))
        *end-- = '\0';

    if (strncasecmp(line, "Content-Type:", 13) == 0)
    {
        char *val = line + 13;
        while (isspace((unsigned char)*val))
            ++val;
        if (hs->content_type)
            free(hs->content_type);
        hs->content_type = strdup(val);
    }
    else if (strncasecmp(line, "Content-Encoding:", 17) == 0)
    {
        char *val = line + 17;
        while (isspace((unsigned char)*val))
            ++val;
        if (hs->content_encoding)
            free(hs->content_encoding);
        hs->content_encoding = strdup(val);
    }
    free(line);
    return numbytes;
}

static char *get_mime_type_from_file(const char *filename)
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

static void get_config_path(char *buf, size_t size)
{
    const char *home = getenv("HOME");
    if (!home)
        home = ".";
    snprintf(buf, size, "%s/.adrive/data_config.json", home);
}

static void ensure_config_dir()
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
    char *eff_type = ctype ? strdup(ctype) : NULL;
    if (eff_type)
    {
        char *p = strchr(eff_type, ';');
        if (p)
            *p = 0;
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

size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr)
        return 0;
    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;
    return realsize;
}

int compare_files(const void *a, const void *b)
{
    FileEntry *fa = (FileEntry *)a;
    FileEntry *fb = (FileEntry *)b;
    return strcmp(fb->lastModified, fa->lastModified);
}

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
            result = strdup(pos + strlen(needle));
            break;
        }
    }
    cJSON_Delete(json);
    return result;
}

size_t FileWriteCallback(void *buffer, size_t size, size_t nmemb, void *userp)
{
    WriteContext *ctx = (WriteContext *)userp;

    if (!ctx->f)
    {
        long code = 0;
        curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &code);

        if (code == 206)
        {
            ctx->f = fopen(ctx->out_path, "ab");
        }
        else
        {
            ctx->f = fopen(ctx->out_path, "wb");
            ctx->resume_pos = 0;
        }
        if (!ctx->f)
            return 0;
    }

    return fwrite(buffer, size, nmemb, ctx->f);
}
