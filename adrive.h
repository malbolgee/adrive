#ifndef ADRIVE_H
#define ADRIVE_H

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

extern char *g_user;
extern char *g_key;
extern CURL *g_curl;

/**
 * Tracks the state of the progress bar.
 *
 * @param prefix Label displayed before the bar (e.g., "Downloading").
 * @param total Total bytes to transfer.
 * @param current Bytes transferred in the current session.
 * @param base_done Bytes already transferred before this session (for resume).
 * @param start_time Monotonic time when the transfer started.
 * @param last_update Monotonic time of the last UI update.
 * @param human_total Cached human-readable string of the total size.
 */
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
 * Stores HTTP headers relevant for file extraction.
 *
 * @param content_type The value of the Content-Type header.
 * @param content_encoding The value of the Content-Encoding header.
 */
struct HeaderStruct
{
    char *content_type;
    char *content_encoding;
};

/**
 * A dynamic buffer for storing response data in memory.
 * Used by libcurl callbacks.
 *
 * @param memory Pointer to the allocated memory.
 * @param size Current size of the data in bytes.
 */
struct MemoryStruct
{
    char *memory;
    size_t size;
};

/**
 * Represents a file entry from an Artifactory listing.
 *
 * @param uri The URI of the file.
 * @param lastModified The last modified timestamp string.
 */
typedef struct
{
    char *uri;
    char *lastModified;
} FileEntry;

/**
 * Context for writing downloaded data to a file.
 * Handles file opening logic (overwrite vs append) for resuming downloads.
 *
 * @param f File pointer.
 * @param out_path Path to the output file.
 * @param resume_attempted Flag indicating if a resume was requested.
 * @param resume_pos The byte offset to resume from.
 */
typedef struct
{
    FILE *f;
    const char *out_path;
    int resume_attempted;
    curl_off_t resume_pos;
} WriteContext;

/**
 * Prints an error message to stderr and exits the program with status 1.
 *
 * @param fmt Format string (printf-style).
 * @param ... Arguments for the format string.
 */
void die(const char *fmt, ...);

/**
 * Loads Artifactory credentials from environment variables.
 * Sets global variables g_user and g_key.
 */
void load_basic_auth();

/**
 * Determines the repository path to use.
 * If path_hint is provided, it is cleaned and returned.
 * Otherwise, defaults to the ARTIFACTORY_USER.
 * Exits if ARTIFACTORY_USER is not set when path_hint is missing.
 *
 * @param path_hint Optional path provided by the user.
 * @return A newly allocated string containing the path.
 */
char *default_user_path_or_die(const char *path_hint);

/**
 * Formats a byte size into a human-readable string (e.g., "1.50 MB").
 *
 * @param size Size in bytes.
 * @return A newly allocated string containing the formatted size.
 */
char *human_size(double size);

/**
 * Formats a duration in seconds into a human-readable string (e.g., "1h 30m 10s").
 *
 * @param seconds Duration in seconds.
 * @return A newly allocated string containing the formatted duration.
 */
char *human_duration(double seconds);

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
int progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);

/**
 * Calculates the SHA1 hash of a file and returns the first 8 characters.
 *
 * @param path Path to the file.
 * @return A newly allocated string containing the short SHA1 hash, or NULL on error.
 */
char *short_sha1(const char *path);

/**
 * Appends a suffix to a filename, preserving the extension.
 * e.g., "file.txt", "hash" -> "file__hash.txt"
 *
 * @param filename Original filename.
 * @param suffix Suffix to append.
 * @return A newly allocated string containing the new filename.
 */
char *with_hash_suffix(const char *filename, const char *suffix);

/* Helper callbacks and utils */

/**
 * Callback function for libcurl to capture HTTP headers.
 * Used to extract Content-Type and Content-Encoding.
 *
 * @param buffer Pointer to header data.
 * @param size Size of one element.
 * @param nitems Number of elements.
 * @param userdata Pointer to HeaderStruct.
 * @return Number of bytes handled.
 */
size_t HeaderCallback(char *buffer, size_t size, size_t nitems, void *userdata);

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
size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp);

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
size_t FileWriteCallback(void *buffer, size_t size, size_t nmemb, void *userp);

/**
 * Comparator function for qsort to sort FileEntry structs by lastModified date (descending).
 *
 * @param a Pointer to first FileEntry.
 * @param b Pointer to second FileEntry.
 * @return Integer less than, equal to, or greater than zero.
 */
int compare_files(const void *a, const void *b);

/* Extraction utils */

/**
 * Extracts a file based on its MIME type and encoding using external tools.
 * Uses ~/.adrive/data_config.json for configuration.
 *
 * @param filename Path to the file to extract.
 * @param ctype Content-Type header value.
 * @param cenc Content-Encoding header value.
 */
void perform_extraction(const char *filename, const char *ctype, const char *cenc);

/* API helpers */

/**
 * Performs a curl GET request to Artifactory and parses the response as JSON.
 *
 * @param url The target URL.
 * @return A cJSON object pointer, or NULL on error.
 */
cJSON *api_get_json(const char *url);

/**
 * Searches for an artifact by its SHA1 hash.
 *
 * @param sha1 SHA1 hash to search for.
 * @return A newly allocated string containing the path/filename relative to repo root, or NULL if not found.
 */
char *find_by_sha1(const char *sha1);

/* Commands */

/**
 * Uploads a file to Artifactory.
 *
 * @param dest_path Destination path in the repository (optional).
 * @param local_file Path to the local file to upload.
 * @param filename_override Optional filename to use in Artifactory.
 */
void cmd_upload(const char *dest_path, const char *local_file, const char *filename_override);

/**
 * Lists files in a specific path in Artifactory.
 *
 * @param path Path to list files from.
 */
void cmd_list(const char *path);

/**
 * Downloads a file from Artifactory.
 *
 * @param path Path in the repository.
 * @param name_arg Filename (optional if --id or --last is used).
 * @param last Boolean flag to download the latest file.
 * @param id SHA1 hash to search for (optional).
 * @param out_arg Output filename (optional).
 * @param extract Boolean flag to extract the file after download.
 */
void cmd_download(const char *path, const char *name_arg, int last, const char *id, const char *out_arg, int extract);

#endif
