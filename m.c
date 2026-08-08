/*
 * Burst HTTP requester using libcurl + pthreads
 * Usage: http_get_loop -u <url> -r <requests_per_interval> -t <threads> -m <ms_interval> -s <seconds>
 * -u: target URL
 * -r: number of requests to send each interval (integer)
 * -t: number of worker threads (integer)
 * -m: interval in milliseconds between bursts
 * -s: total duration in seconds (integer)
 *
 * Semantics (as requested): with -r 2 -m 200 the program will send 2 requests every 200ms.
 * The requests are divided among -t threads as evenly as possible.
 *
 * Requires libcurl and pthreads. Compile example (Linux/Mingw with pthreads):
 * gcc -o http_get_loop http_get_loop.c -lcurl -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <pthread.h>
#endif
#include <curl/curl.h>

static size_t discard_write(void *ptr, size_t size, size_t nmemb, void *userdata) {
    (void)ptr;
    (void)userdata;
    return size * nmemb;
}

static void ms_sleep(unsigned int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

typedef struct {
    int id;
    const char *url;
    int assigned_per_burst;
    const char **proxies;
    int proxy_count;
    volatile long *burst_id;
    pthread_mutex_t *burst_mutex;
    pthread_cond_t *burst_cond;
    volatile int *shutdown;
} worker_arg_t;

static char *clone_string(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = (char *)malloc(len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

static void free_proxy_list(char **proxies, int count) {
    if (!proxies) return;
    for (int i = 0; i < count; ++i) {
        free(proxies[i]);
    }
    free(proxies);
}

static char *normalize_proxy(const char *proxy) {
    if (!proxy || *proxy == '\0') return NULL;
    const char *scheme = strstr(proxy, "://");
    if (scheme) {
        return clone_string(proxy);
    }

    size_t len = strlen(proxy) + sizeof("http://");
    char *normalized = (char *)malloc(len);
    if (!normalized) return NULL;
    strcpy(normalized, "http://");
    strcat(normalized, proxy);
    return normalized;
}

static int read_proxy_file(const char *filename, char ***out_proxies, int *out_count) {
    FILE *file = fopen(filename, "r");
    if (!file) return -1;

    char **list = NULL;
    int count = 0;
    char line[1024];

    while (fgets(line, sizeof(line), file)) {
        char *start = line;
        while (*start && (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')) {
            start++;
        }
        if (*start == '\0' || *start == '#') {
            continue;
        }
        char *end = start + strlen(start) - 1;
        while (end >= start && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
            *end = '\0';
            end--;
        }
        if (*start == '\0') {
            continue;
        }

        char *normalized = normalize_proxy(start);
        if (!normalized) {
            free_proxy_list(list, count);
            fclose(file);
            return -1;
        }

        char **new_list = (char **)realloc(list, (count + 1) * sizeof(char *));
        if (!new_list) {
            free(normalized);
            free_proxy_list(list, count);
            fclose(file);
            return -1;
        }

        list = new_list;
        list[count] = normalized;
        count++;
    }

    fclose(file);
    *out_proxies = list;
    *out_count = count;
    return 0;
}

static void *worker_thread(void *arg) {
    worker_arg_t *w = (worker_arg_t *)arg;
    CURL *curl = NULL;
    long last_handled = -1;

    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "thread %d: curl_easy_init failed\n", w->id);
        return NULL;
    }

    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "C-GET-Burst/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_write);

    while (!*(w->shutdown)) {
        if (w->proxy_count > 0) {
            int proxy_index = (w->id + (int)(time(NULL) % w->proxy_count)) % w->proxy_count;
            curl_easy_setopt(curl, CURLOPT_PROXY, w->proxies[proxy_index]);
        }
        pthread_mutex_lock(w->burst_mutex);
        while (*(w->burst_id) == last_handled && !*(w->shutdown)) {
            pthread_cond_wait(w->burst_cond, w->burst_mutex);
        }
        if (*(w->shutdown)) {
            pthread_mutex_unlock(w->burst_mutex);
            break;
        }
        last_handled = *(w->burst_id);
        pthread_mutex_unlock(w->burst_mutex);

        for (int i = 0; i < w->assigned_per_burst; ++i) {
            curl_easy_setopt(curl, CURLOPT_URL, w->url);
            CURLcode res = curl_easy_perform(curl);
            if (res != CURLE_OK) {
                fprintf(stderr, "thread %d: request failed: %s\n", w->id, curl_easy_strerror(res));
            } else {
                long http_code = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
                printf("thread %d: HTTP %ld\n", w->id, http_code);
            }
        }
    }

    curl_easy_cleanup(curl);
    return NULL;
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s -u <url> -r <requests_per_interval> -t <threads> -m <ms_interval> -s <seconds>\n", prog);
}

int main(int argc, char **argv) {
    const char *url = NULL;
    const char *proxy_file = NULL;
    int r = -1;
    int t = 1;  
    int m = 1000; 
    int s = 10; 

    if (argc == 1) {
        print_usage(argv[0]);
        return 1;
    }

    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "-u") == 0 || strncmp(argv[i], "-u=", 3) == 0)) {
            if (argv[i][2] == '=') url = argv[i]+3; else url = argv[++i];
        } else if ((strcmp(argv[i], "-x") == 0 || strncmp(argv[i], "-x=", 3) == 0)) {
            if (argv[i][2] == '=') proxy_file = argv[i]+3; else proxy_file = argv[++i];
        } else if ((strcmp(argv[i], "-r") == 0 || strncmp(argv[i], "-r=", 3) == 0)) {
            if (argv[i][2] == '=') r = atoi(argv[i]+3); else r = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "-t") == 0 || strncmp(argv[i], "-t=", 3) == 0)) {
            if (argv[i][2] == '=') t = atoi(argv[i]+3); else t = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "-m") == 0 || strncmp(argv[i], "-m=", 3) == 0)) {
            if (argv[i][2] == '=') m = atoi(argv[i]+3); else m = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "-s") == 0 || strncmp(argv[i], "-s=", 3) == 0)) {
            if (argv[i][2] == '=') s = atoi(argv[i]+3); else s = atoi(argv[++i]);
        } else if (strncmp(argv[i], "-h", 2) == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    if (!url || r <= 0 || t <= 0 || m <= 0 || s <= 0) {
        print_usage(argv[0]);
        return 1;
    }

    char **proxy_list = NULL;
    int proxy_count = 0;
    if (proxy_file) {
        if (read_proxy_file(proxy_file, &proxy_list, &proxy_count) != 0) {
            fprintf(stderr, "failed to read proxy file: %s\n", proxy_file);
            return 6;
        }
        if (proxy_count == 0) {
            fprintf(stderr, "proxy file %s contains no valid proxies\n", proxy_file);
            return 6;
        }
    }

    fprintf(stderr, "Warning: ensure you have permission to send repeated requests to %s\n", url);

    CURLcode g = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (g != CURLE_OK) {
        fprintf(stderr, "curl_global_init() failed: %s\n", curl_easy_strerror(g));
        return 2;
    }

    int *assigned = (int *)calloc(t, sizeof(int));
    if (!assigned) {
        fprintf(stderr, "allocation failed\n");
        curl_global_cleanup();
        return 3;
    }
    int base = r / t;
    int rem = r % t;
    for (int i = 0; i < t; ++i) assigned[i] = base + (i < rem ? 1 : 0);

    pthread_t *threads = (pthread_t *)calloc(t, sizeof(pthread_t));
    worker_arg_t *wargs = (worker_arg_t *)calloc(t, sizeof(worker_arg_t));
    if (!threads || !wargs) {
        fprintf(stderr, "allocation failed\n");
        free(assigned);
        curl_global_cleanup();
        return 4;
    }

    pthread_mutex_t burst_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t burst_cond = PTHREAD_COND_INITIALIZER;
    volatile long burst_id = 0;
    volatile int shutdown = 0;

    /* create worker threads */
    for (int i = 0; i < t; ++i) {
        wargs[i].id = i;
        wargs[i].url = url;
        wargs[i].assigned_per_burst = assigned[i];
        wargs[i].proxies = (const char **)proxy_list;
        wargs[i].proxy_count = proxy_count;
        wargs[i].burst_id = &burst_id;
        wargs[i].burst_mutex = &burst_mutex;
        wargs[i].burst_cond = &burst_cond;
        wargs[i].shutdown = &shutdown;
        if (pthread_create(&threads[i], NULL, worker_thread, &wargs[i]) != 0) {
            fprintf(stderr, "failed to create thread %d\n", i);
            shutdown = 1;
            for (int j = 0; j < i; ++j) pthread_join(threads[j], NULL);
            free(threads); free(wargs); free(assigned);
            free_proxy_list(proxy_list, proxy_count);
            curl_global_cleanup();
            return 5;
        }
    }

    /* run bursts for s seconds */
    time_t start = time(NULL);
    while (difftime(time(NULL), start) < s) {
        /* signal a new burst */
        pthread_mutex_lock(&burst_mutex);
        ++burst_id;
        pthread_cond_broadcast(&burst_cond);
        pthread_mutex_unlock(&burst_mutex);

        ms_sleep(m);
    }

    /* shutdown workers */
    shutdown = 1;
    pthread_mutex_lock(&burst_mutex);
    pthread_cond_broadcast(&burst_cond);
    pthread_mutex_unlock(&burst_mutex);

    for (int i = 0; i < t; ++i) pthread_join(threads[i], NULL);

    free(threads);
    free(wargs);
    free(assigned);
    free_proxy_list(proxy_list, proxy_count);

    curl_global_cleanup();
    return 0;
}
