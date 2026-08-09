#include "adrive.h"

static size_t ThreadWriteCallback(void *buffer, size_t size, size_t nmemb, void *userp)
{
    FILE *f = (FILE *)userp;
    return fwrite(buffer, size, nmemb, f);
}

static int thread_progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal; (void)ultotal; (void)ulnow;
    curl_off_t *progress = (curl_off_t *)clientp;
    *progress = dlnow;
    return 0;
}

typedef struct {
    const char *url;
    const char *out_path;
    int thread_id;
    curl_off_t start_pos;
    curl_off_t end_pos;
    curl_off_t *progress_ptr;
    int success;
} ThreadData;

static void *download_thread_worker(void *arg)
{
    ThreadData *data = (ThreadData *)arg;
    CURL *curl = curl_easy_init();
    if (!curl)
    {
        data->success = 0;
        return NULL;
    }

    FILE *f = fopen(data->out_path, "r+b");
    if (!f)
    {
        curl_easy_cleanup(curl);
        data->success = 0;
        return NULL;
    }

    if (fseeko(f, data->start_pos, SEEK_SET) != 0)
    {
        fclose(f);
        curl_easy_cleanup(curl);
        data->success = 0;
        return NULL;
    }

    char range_hdr[128];
    snprintf(range_hdr, sizeof(range_hdr), "%" CURL_FORMAT_CURL_OFF_T "-%" CURL_FORMAT_CURL_OFF_T, data->start_pos, data->end_pos);

    curl_easy_setopt(curl, CURLOPT_URL, data->url);
    curl_easy_setopt(curl, CURLOPT_USERNAME, g_user);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, g_key);
    curl_easy_setopt(curl, CURLOPT_RANGE, range_hdr);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ThreadWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, thread_progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, data->progress_ptr);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

    CURLcode res = curl_easy_perform(curl);
    fclose(f);
    curl_easy_cleanup(curl);

    data->success = (res == CURLE_OK);
    return NULL;
}

typedef struct {
    ProgressState *pstate;
    curl_off_t *thread_progress;
    int *threads_running;
} ProgressPrinterData;

static void *progress_printer_worker(void *arg)
{
    ProgressPrinterData *data = (ProgressPrinterData *)arg;
    while (*data->threads_running)
    {
        curl_off_t total_dl = 0;
        for (int i = 0; i < THREAD_COUNT; i++)
        {
            total_dl += data->thread_progress[i];
        }
        progress_callback(data->pstate, data->pstate->total, total_dl, 0, 0);
        usleep(200000); // 200ms
    }
    curl_off_t total_dl = 0;
    for (int i = 0; i < THREAD_COUNT; i++)
    {
        total_dl += data->thread_progress[i];
    }
    progress_callback(data->pstate, data->pstate->total, total_dl, 0, 0);
    return NULL;
}

static curl_off_t api_get_file_size(const char *url, struct HeaderStruct *hs)
{
    CURL *curl = curl_easy_init();
    if (!curl)
        return -1;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERNAME, g_user);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, g_key);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    if (hs)
    {
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, hs);
    }

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK)
    {
        curl_easy_cleanup(curl);
        return -1;
    }

    curl_off_t size = -1;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &size);
    curl_easy_cleanup(curl);
    return size;
}

void cmd_download(const char *path, const char *name_arg, int last, const char *id, const char *out_arg, int extract)
{
    load_basic_auth();
    if (!g_user || !g_key)
        die("Set ARTIFACTORY_USER and ARTIFACTORY_API_KEY.");

    char *final_path = NULL;
    char *final_name = NULL;

    if (id && !name_arg)
    {
        char *resolved = find_by_sha1(id);
        if (!resolved)
            die("No artifact with sha1=%s found in repo %s", id, REPO);

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

            cJSON *json = api_get_json(list_url);
            free(list_url);
            if (!json)
                die("Failed to list files for --last");

            cJSON *files = cJSON_GetObjectItem(json, "files");
            if (!files)
                die("No files found.");

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

    curl_off_t resume_pos = 0;
    struct stat st;
    if (stat(out_file, &st) == 0)
    {
        resume_pos = st.st_size;
    }

    struct HeaderStruct hs = {0};
    curl_off_t total_size = api_get_file_size(url, &hs);

    // parallel download threshold: 10 MB
    if (total_size >= 10 * 1024 * 1024 && resume_pos == 0)
    {
        printf("Starting parallel download with %d threads...\n", THREAD_COUNT);

        FILE *f_alloc = fopen(out_file, "wb");
        if (!f_alloc)
            die("Could not create output file: %s", out_file);
        if (fseeko(f_alloc, total_size - 1, SEEK_SET) != 0)
        {
            fclose(f_alloc);
            die("Failed to seek for pre-allocation.");
        }
        char zero = 0;
        if (fwrite(&zero, 1, 1, f_alloc) != 1)
        {
            fclose(f_alloc);
            die("Failed to write zero byte for pre-allocation.");
        }
        fclose(f_alloc);

        curl_off_t chunk_size = total_size / THREAD_COUNT;
        ThreadData threads_data[THREAD_COUNT];
        pthread_t threads[THREAD_COUNT];
        curl_off_t thread_progress[THREAD_COUNT] = {0};

        for (int i = 0; i < THREAD_COUNT; i++)
        {
            threads_data[i].url = url;
            threads_data[i].out_path = out_file;
            threads_data[i].thread_id = i;
            threads_data[i].start_pos = i * chunk_size;
            threads_data[i].end_pos = (i == THREAD_COUNT - 1) ? (total_size - 1) : ((i + 1) * chunk_size - 1);
            threads_data[i].progress_ptr = &thread_progress[i];
            threads_data[i].success = 0;
        }

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);

        ProgressState pstate = {0};
        pstate.prefix = "Downloading (Parallel)";
        pstate.total = total_size;
        pstate.start_time = ts.tv_sec + ts.tv_nsec / 1e9;
        pstate.human_total = human_size((double)total_size);

        int threads_running = 1;
        ProgressPrinterData printer_data = {
            &pstate,
            thread_progress,
            &threads_running
        };

        pthread_t printer_thread;
        pthread_create(&printer_thread, NULL, progress_printer_worker, &printer_data);

        for (int i = 0; i < THREAD_COUNT; i++)
        {
            pthread_create(&threads[i], NULL, download_thread_worker, &threads_data[i]);
        }

        int all_success = 1;
        for (int i = 0; i < THREAD_COUNT; i++)
        {
            pthread_join(threads[i], NULL);
            if (!threads_data[i].success)
                all_success = 0;
        }

        threads_running = 0;
        pthread_join(printer_thread, NULL);
        free(pstate.human_total);

        if (!all_success)
        {
            die("One or more download threads failed.");
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
        return;
    }

    // Fallback: Single-threaded download (resumes or small files)
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
    curl_easy_setopt(g_curl, CURLOPT_FAILONERROR, 1L);

    if (!hs.content_type)
    {
        curl_easy_setopt(g_curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
        curl_easy_setopt(g_curl, CURLOPT_HEADERDATA, &hs);
    }

    if (resume_pos > 0)
    {
        curl_easy_setopt(g_curl, CURLOPT_RESUME_FROM_LARGE, resume_pos);
    }

    printf("\n");
    CURLcode res = curl_easy_perform(g_curl);

    long code;
    curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &code);

    if (res != CURLE_OK && code == 416)
    {
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
