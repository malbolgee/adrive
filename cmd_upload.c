#include "adrive.h"

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
            cJSON *json = cJSON_Parse(chunk.memory);
            if (json)
            {
                cJSON *j_path = cJSON_GetObjectItem(json, "path");
                char *filename_display = (j_path && j_path->valuestring) ? j_path->valuestring : "unknown";
                if (filename_display)
                {
                    char *slash = strrchr(filename_display, '/');
                    if (slash)
                        filename_display = slash + 1;
                }

                double size_val = 0;
                cJSON *j_size = cJSON_GetObjectItem(json, "size");
                if (cJSON_IsNumber(j_size))
                    size_val = j_size->valuedouble;
                else if (cJSON_IsString(j_size))
                    size_val = atof(j_size->valuestring);
                char *h_size = human_size(size_val);

                cJSON *j_mime = cJSON_GetObjectItem(json, "mimeType");
                cJSON *j_checksums = cJSON_GetObjectItem(json, "checksums");
                cJSON *j_sha1 = j_checksums ? cJSON_GetObjectItem(j_checksums, "sha1") : NULL;
                cJSON *j_link = cJSON_GetObjectItem(json, "downloadUri");

                printf("\nfile: %s\n", filename_display);
                printf("size: %s\n", h_size);
                printf("type: %s\n", (j_mime && j_mime->valuestring) ? j_mime->valuestring : "unknown");
                printf("id: %s\n", (j_sha1 && j_sha1->valuestring) ? j_sha1->valuestring : "unknown");
                printf("link: %s\n", (j_link && j_link->valuestring) ? j_link->valuestring : "unknown");

                free(h_size);
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
