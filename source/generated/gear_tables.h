#ifndef GEAR_TABLES_H
#define GEAR_TABLES_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t id;
    const char* name;
    const char* brand;
    const char* icon; // romfs path to 128x128 PNG
} GearDataEntry;

extern const GearDataEntry g_head_data[];
extern const size_t g_head_count;
extern const GearDataEntry g_clothes_data[];
extern const size_t g_clothes_count;
extern const GearDataEntry g_shoes_data[];
extern const size_t g_shoes_count;

const GearDataEntry* gear_data_find(int type, uint16_t id);

#endif
