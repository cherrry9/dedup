#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <signal.h>
#include <pthread.h>

#include "args.h"
#include "recdir.h"
#include "sha256.h"
#include "util.h"
#include "sql.h"

#define THREADS_CAP 1024

typedef struct {
    SQL *sql;
    Args args;
    int excode;
    char *fpath;
} ExecutionData;

static int terminated;

static void terminate();
static void *process_file(void *datap);

void
terminate()
{
    fputs("\nterminating...\n", stderr);
    terminated = 1;
}

void *
process_file(void *datap)
{
    ExecutionData *data = datap;
    unsigned char hash[SHA256_LENGTH];
    char hash_cstr[SHA256_CSTR_LENGTH];
    FILE *fp;

    if ((fp = fopen(data->fpath, "r")) == NULL) {
        if (errno != 0) {
            perror(data->fpath);
            errno = 0;
        }
        return NULL;
    }

    sha256(hash, fp, data->args.nbytes);
    fclose(fp);

    if (data->args.verbose & VERBOSE_HASH) {
        hash2cstr(hash, hash_cstr);
        printf("%-64s  %s\n", hash_cstr, data->fpath);
    }

    if (data->sql != NULL && sql_insert(data->sql, data->fpath, hash) != 0) {
        fprintf(stderr, "sqlite3: %s\n", sql_errmsg(data->sql));
        fprintf(stderr, "terminating...\n");
        errno = 0;
        data->excode = 1;
        terminated = 1;
        return NULL;
    }

    free(data->fpath);
    free(data);
    return NULL;
}

int
main(int argc, char *argv[])
{
    ExecutionData data = {0};
    ExecutionData *data_copy;
    RECDIR *recdir = NULL;
    char *fpath;
    pthread_t threads[THREADS_CAP];
    size_t i, threads_sz = 0;

    signal(SIGINT, terminate);

    argsparse(argc, argv, &data.args);

    if (data.args.db && sql_open(&data.sql, data.args.db) != 0) {
        fprintf(stderr, "sqlite3: %s\n", sql_errmsg(data.sql));
        errno = 0;
        data.excode = 1;
        goto cleanup;
    }

    recdir = recdiropen(
        data.args.path, data.args.exclude_reg,
        data.args.maxdepth, data.args.mindepth, data.args.verbose
    );

    if (recdir == NULL) {
        perror(data.args.path);
        errno = 0;
        data.excode = 1;
        goto cleanup;
    }

    while ((fpath = recdirread(recdir)) != NULL && !terminated) {
        data_copy = emalloc(sizeof(ExecutionData));
        *data_copy = data;
        data_copy->fpath = fpath;
        pthread_create(threads + threads_sz, NULL, process_file, data_copy);
        threads_sz++;
        if (threads_sz >= THREADS_CAP) {
            for (i = 0; i < threads_sz; i++)
                pthread_join(threads[i], NULL);
            threads_sz = 0;
        }
    }

    for (i = 0; i < threads_sz; i++)
        pthread_join(threads[i], NULL);


cleanup:
    if (data.sql) sql_close(data.sql);
    if (recdir) recdirclose(recdir);
    argsfree(&data.args);

    if (errno != 0)
        die("Could not read directory:");

    return data.excode;
}
