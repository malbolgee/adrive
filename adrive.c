#include "adrive.h"

char *g_user = NULL;
char *g_key = NULL;
CURL *g_curl = NULL;

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
