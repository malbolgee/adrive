#include "adrive.h"

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

    struct HeaderStruct hs = {0};
    curl_easy_setopt(g_curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(g_curl, CURLOPT_HEADERDATA, &hs);

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
