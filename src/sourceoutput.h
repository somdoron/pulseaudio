#ifndef foosourceoutputhfoo
#define foosourceoutputhfoo

#include <inttypes.h>

#include "source.h"
#include "sample.h"
#include "memblockq.h"

struct source_output {
    uint32_t index;

    char *name;
    struct source *source;
    struct pa_sample_spec sample_spec;
    
    void (*push)(struct source_output *o, struct memchunk *chunk);
    void (*kill)(struct source_output* o);

    void *userdata;
};

struct source_output* source_output_new(struct source *s, struct pa_sample_spec *spec, const char *name);
void source_output_free(struct source_output* o);

void source_output_kill(struct source_output*o);

char *source_output_list_to_string(struct core *c);

#endif
