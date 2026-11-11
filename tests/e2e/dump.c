#include "bgapi_tables.h"
#include <stdio.h>
int main(void){ printf("{\"msgs\":["); for(unsigned i=0;i<bgapi_msg_def_count;i++){const bgapi_msg_def_t*d=&bgapi_msg_defs[i];
 printf("%s[%u,%d,\"%s\",%u,%u]",i?",":"",d->id,d->kind,d->name,d->min_len,d->array_len_size);} printf("]}\n"); return 0;}
