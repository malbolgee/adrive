#include "adrive.h"

void cmd_list(const char *path)
{
    load_basic_auth();
    if (!g_user || !g_key)
        die("Set ARTIFACTORY_USER and ARTIFACTORY_API_KEY.");

    char *p = default_user_path_or_die(path);
    char *url;
    asprintf(&url, "%s/api/storage/%s/%s?list&deep=1&listFolders=0", ROOT_BASE, REPO, p);

    cJSON *json = api_get_json(url);
    if (!json)
        die("Failed to list files or fetch JSON response from Artifactory.");

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
    free(url);
    free(p);
}
