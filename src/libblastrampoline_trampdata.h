// Load in our exported function names, they'll be stored in a macro called
// EXPORTED_FUNCS(XX), and we'll do item-by-item processing by defining XX.
#include "exported_funcs.inc"

// Define holder locations for function addresses as `const void * $(name)_addr`
// We define a second set the same as the first, but with `_64` on the end, to
// provide our ILP64 interface, which is a perfect clone of the
#define XX(name)    JL_HIDDEN const void * name##_addr;
#define XX_64(name) JL_HIDDEN const void * name##64__addr;
EXPORTED_FUNCS(XX)
EXPORTED_FUNCS(XX_64)
#undef XX
#undef XX_64

// Generate list of function names
#define XX(name)    #name,
static const char *const exported_func_names[] = {
    EXPORTED_FUNCS(XX)
    NULL
};
#undef XX

// Generate list of function addresses to tie names -> variables
#define XX(name)    &name##_addr,
#define XX_64(name) &name##64__addr,
static const void ** exported_func32_addrs[] = {
    EXPORTED_FUNCS(XX)
    NULL
};
static const void ** exported_func64_addrs[] = {
    EXPORTED_FUNCS(XX_64)
    NULL
};
#undef XX
#undef XX_64