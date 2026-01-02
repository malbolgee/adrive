#include "adrive.h"

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
            if (uri && *uri == '/')
                ++uri;

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
