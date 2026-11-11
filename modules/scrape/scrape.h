#ifndef SCRAPE_H
#define SCRAPE_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    SCRAPE_SOURCE_ITCH_IO,
    SCRAPE_SOURCE_LEWDZONE_FORUM
} scrape_source_t;

typedef struct {
    char name[128];
    char current_version[64];
    char url[512];
    scrape_source_t source;
} scrape_target_t;

bool scrape_check_all(const scrape_target_t *targets, size_t n);

#endif
