#include "recdir.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <fcntl.h>
#include <regex.h>
#include <sys/types.h>
#include <unistd.h>

#include "util.h"
#include "args.h"

#define FRAMES_REALLOC_SIZE 16
#define RECDIR_LOG(...) if (recdir->fmt) printf(recdir->fmt, __VA_ARGS__) \

typedef struct {
    char *path;
    DIR *dir;
} RECDIR_FRAME;

struct RECDIR_ {
    RECDIR_FRAME *frames;
    regex_t *exclude_reg;
    const char *fmt;
    size_t frames_sz;
    size_t maxdepth;
    size_t mindepth;
    size_t depth;
    char *fpath;
};

static RECDIR_FRAME *recdirtop(RECDIR *recdir);
static int recdirpush(RECDIR *recdir, const char *path);
static int recdirpop(RECDIR *recdir);
static char *makepath(const char *p1, const char *p2, char *outpath);

char *
makepath(const char *p1, const char *p2, char *outpath)
{
    int p1sz = strlen(p1);
    const char *fmt;

    if (outpath == NULL) outpath = emalloc(p1sz + strlen(p2) + 2);
    fmt = p1[p1sz - 1] != '/' ? "%s/%s" : "%s%s";
    sprintf(outpath, fmt, p1, p2);
    return outpath;
}

int
recdirpush(RECDIR *recdir, const char *path)
{
    RECDIR_FRAME *top;
    DIR *dir;

    if ((dir = opendir(path)) == NULL)
        return 1;

    if (++recdir->depth >= recdir->frames_sz) {
        recdir->frames_sz += FRAMES_REALLOC_SIZE;
        recdir->frames = erealloc(
                recdir->frames, sizeof(RECDIR_FRAME) * recdir->frames_sz
        );
    }

    top = recdirtop(recdir);
    top->dir = dir;
    top->path = strdup(path);
    return 0;
}

int
recdirpop(RECDIR *recdir)
{
    RECDIR_FRAME *top;
    int excode;

    assert(recdir->depth > 0);
    top = recdirtop(recdir);
    free(top->path);
    excode = closedir(top->dir);
    if (--recdir->depth < recdir->frames_sz - FRAMES_REALLOC_SIZE) {
        recdir->frames_sz -= FRAMES_REALLOC_SIZE;
        recdir->frames = erealloc(
                recdir->frames, sizeof(RECDIR_FRAME) * recdir->frames_sz
        );
    }
    return excode;
}

RECDIR_FRAME *
recdirtop(RECDIR *recdir)
{
    return &recdir->frames[recdir->depth - 1];
}

RECDIR *
recdiropen(const char *path, regex_t *exclude_reg, size_t maxdepth,
           size_t mindepth, int verbose)
{
    RECDIR *recdir;

    recdir = ecalloc(1, sizeof(struct RECDIR_));

    if (recdirpush(recdir, path) != 0) {
        free(recdir);
        return NULL;
    }

    if (verbose & VERBOSE_STACK)
        recdir->fmt = (verbose & VERBOSE_HASH) ? "%-64s  %s\n" : "%-10s  %s\n";

    recdir->maxdepth = maxdepth;
    recdir->mindepth = mindepth;
    recdir->exclude_reg = exclude_reg;
    RECDIR_LOG("OPEN", path);

    return recdir;
}

void
recdirclose(RECDIR *recdir)
{
    free(recdir->frames);
    free(recdir);
}

char *
recdirread(RECDIR *recdir)
{
    struct dirent *ent;
    RECDIR_FRAME *top;
    char *path;

    if (recdir->fpath != NULL) {
        free(recdir->fpath);
        recdir->fpath = NULL;
    }

    while (1) {
        top = recdirtop(recdir);

        while ((ent = readdir(top->dir)) != NULL &&
               (strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0));

        if (errno != 0) {
            perror(path);
            errno = 0;
            if (recdirpop(recdir) < 0 || recdir->depth == 0)
                return NULL;
            continue;
        }

        if (ent == NULL) {
            RECDIR_LOG("CLOSE", top->path);
            if (recdirpop(recdir) < 0 || recdir->depth == 0)
                return NULL;
            continue;
        }

        path = alloca(strlen(top->path) + strlen(ent->d_name) + 2);
        makepath(top->path, ent->d_name, path);

        if (access(path, R_OK) != 0) {
            perror(path);
            errno = 0;
            continue;
        }

        switch (ent->d_type) {
        case DT_DIR:
            if (recdir->maxdepth <= recdir->depth)
                continue;
            if (recdir->exclude_reg != NULL &&
                regexec(recdir->exclude_reg, path, 0, NULL, 0) == 0) {
                RECDIR_LOG("EXCLUDE", path);
                continue;
            }
            if (recdirpush(recdir, path) != 0) {
                perror(path);
                errno = 0;
                assert(0 && "UNREACHABLE?");
                continue;
            }
            RECDIR_LOG("OPEN", recdirtop(recdir)->path);
            break;
        case DT_REG:
            if (recdir->mindepth > recdir->depth)
                continue;
            recdir->fpath = makepath(top->path, ent->d_name, NULL);
            return recdir->fpath;
        default:
            /* TODO: handle for symlinks */
            RECDIR_LOG("SKIP [T]", path);
        }
    }
}
