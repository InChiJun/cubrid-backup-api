#ifndef _BACKUP_COMMON_H_
#define _BACKUP_COMMON_H_

#include <unistd.h>

#define SUCCESS_FRAGMENTED (1) // means there is still data left to read on cubrid_backup_read ()
#define SUCCESS            (0)
#define FAILURE            (-1)

#define IS_SUCCESS(a) ((a) == SUCCESS)
#define IS_FAILURE(a) ((a) != SUCCESS)

#define IS_NULL(a) ((a) == NULL)

#define IS_ZERO(a) ((a) == 0)
#define IS_NOT_ZERO(a) ((a) != 0)

#define PATH_MAX 4096

typedef enum
{
    false,
    true
} bool;

#endif
