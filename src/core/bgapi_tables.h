/* bgapi_tables.h -- message, class and status tables generated from the
 * Silicon Labs SDK headers (see tools/gen_bgapi_tables.py). */
#ifndef BGAPI_TABLES_H
#define BGAPI_TABLES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BGAPI_KIND_CMD = 0,
    BGAPI_KIND_RSP = 1,
    BGAPI_KIND_EVT = 2
} bgapi_kind_t;

typedef struct {
    uint32_t     id;         /* header & 0xffff00f8 (SL_BGAPI_MSG_ID) */
    bgapi_kind_t kind;
    const char  *name;       /* e.g. "cmd_system_hello" */
    uint16_t     min_len;    /* sizeof() of the SDK's packed payload struct */
    uint8_t      array_len_size; /* 0 fixed; 1 trailing uint8array; 2 trailing byte_array */
} bgapi_msg_def_t;

typedef struct {
    uint8_t     class_id;
    const char *name;
} bgapi_class_def_t;

typedef struct {
    uint16_t    value;
    const char *name;
} bgapi_status_def_t;

extern const bgapi_msg_def_t    bgapi_msg_defs[];
extern const unsigned           bgapi_msg_def_count;
extern const bgapi_class_def_t  bgapi_class_defs[];
extern const unsigned           bgapi_class_def_count;
extern const bgapi_status_def_t bgapi_status_defs[];
extern const unsigned           bgapi_status_def_count;

#ifdef __cplusplus
}
#endif

#endif
