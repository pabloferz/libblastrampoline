#include "libblastrampoline_internal.h"

/*
 * Autodetect the name mangling suffix used by the given BLAS/LAPACK library.
 * We are primarily concerned with the FORTRAN names, so we automatically append
 * an underscore to the end, as is standard.
 */
const char * autodetect_symbol_suffix(void * handle) {
    // We auto-detect the symbol suffix of the given library by searching for common
    // BLAS and LAPACK symbols, combined with various suffixes that we know of.
    const char * symbol_names[] = {
        // fortran BLAS symbol
        "isamax_",
        // fortran LAPACK symbol
        "dpotrf_",
    };
    const char * suffixes[] = {
        // First, LP64-mangling suffixes: No underscore, single underscore, double underscore
        "", "_", "__",
        // Next, ILP64-mangling suffixes: the same, but with the suffix prepended as well:
        "64_", "_64__", "__64___",
    };
    char symbol_name[MAX_SYMBOL_LEN];
    for (int symbol_idx=0; symbol_idx<sizeof(symbol_names)/sizeof(const char *); symbol_idx++) {
        for (int suffix_idx=0; suffix_idx<sizeof(suffixes)/sizeof(const char *); suffix_idx++) {
            sprintf(symbol_name, "%s%s", symbol_names[symbol_idx], suffixes[suffix_idx]);
            if (lookup_symbol(handle, symbol_name) != NULL) {
                return suffixes[suffix_idx];
            }
        }
    }
    return NULL;
}

/*
 * If this is a BLAS library, we'll check interface type by invoking `isamax` with a purposefully
 * incorrect `N` to cause it to change its return value based on how it is interpreting arugments.
 */
int32_t autodetect_blas_interface(void * isamax_addr) {
    // Typecast to function pointer for easier usage below
    int64_t (*isamax)(int64_t *, float *, int64_t *) = isamax_addr;

    // Purposefully incorrect length that is `3` on 32-bit, but massively negative on 64-bit
    int64_t n = 0xffffffff00000003;
    float X[3] = {1.0f, 2.0f, 1.0f};
    int64_t incx = 1;

    // Override `lsame_` to point to our `fake_lsame` if we're unable to `RTLD_DEEPBIND`
#ifdef LBT_DEEPBINDLESS
    push_fake_lsame();
#endif
    int64_t max_idx = isamax(&n, X, &incx);
#ifdef LBT_DEEPBINDLESS
    pop_fake_lsame();
#endif

    // This means the `isamax()` implementation saw `N < 0`, ergo it's a 64-bit library
    if (max_idx == 0) {
        return LBT_INTERFACE_ILP64;
    }
    // This means the `isamax()` implementation saw `N == 3`, ergo it's a 32-bit library
    if (max_idx == 2) {
        return LBT_INTERFACE_LP64;
    }
    // We have no idea what happened; `max_idx` isn't any of the options we thought it would be.
    return LBT_INTERFACE_UNKNOWN;
}

/*
 * If this is an LAPACK library, we'll check interface type by invoking `dpotrf` with a
 * purposefully incorrect `lda` to cause it to store an error code that we can inspect
 * and determine if the internal pointer dereferences were 32-bit or 64-bit.
 */
int32_t autodetect_lapack_interface(void * dpotrf_addr) {
    // Typecast to function pointer for easier usage below
    void (*dpotrf)(char *, int64_t *, double *, int64_t *, int64_t *) = dpotrf_addr;

    // This `dpotrf` invocation should result in an error code stored into `info`
    double testmat[4];
    char uplo = 'U';
    int64_t m = 2;
    int64_t lda = 0;
    int64_t info = 0;
    dpotrf(&uplo, &m, testmat, &lda, &info);

    if (info == 0xfffffffffffffffc) {
        // If `info` is actually `-4`, it means that `dpotrf` actually stored a 64-bit
        // value into `info`, which means that `lapack_int` internally is 64-bits
        return LBT_INTERFACE_ILP64;
    }
    if (info == 0x00000000fffffffc) {
        // This is what it looks like when a library stores a 32-bit value in a 64-bit slot.
        return LBT_INTERFACE_LP64;
    }
    // We have no idea what happened; `info` isn't any of the options we thought it would be.
    return LBT_INTERFACE_UNKNOWN;
}

/*
 * Autodetect the interface type of the given library with the given symbol mangling suffix.
 * Returns the values "32", "64" or "0", denoting the bitwidth of the internal index representation.
 */
int32_t autodetect_interface(void * handle, const char * suffix) {
    char symbol_name[MAX_SYMBOL_LEN];

    // Attempt BLAS `isamax()` test
    sprintf(symbol_name, "isamax_%s", suffix);
    void * isamax = lookup_symbol(handle, symbol_name);
    if (isamax != NULL) {
        return autodetect_blas_interface(isamax);
    }

    // Attempt LAPACK `dpotrf()` test
    sprintf(symbol_name, "dpotrf_%s", suffix);
    void * dpotrf = lookup_symbol(handle, symbol_name);
    if (dpotrf != NULL) {
        return autodetect_lapack_interface(dpotrf);
    }

    // Otherwise, this is probably not an LAPACK or BLAS library?!
    return LBT_INTERFACE_UNKNOWN;
}

#ifdef F2C_AUTODETECTION
int autodetect_f2c(void * handle, const char * suffix) {
    char symbol_name[MAX_SYMBOL_LEN];

    // Attempt BLAS `sdot_()` test
    sprintf(symbol_name, "sdot_%s", suffix);
    void * sdot_addr = lookup_symbol(handle, symbol_name);
    if (sdot_addr == NULL) {
        return 0;
    }

    // Typecast to function pointer for easier usage below
    float  (*sdot_plain)(int64_t *, float *, int64_t *, float *, int64_t *) = sdot_addr;
    double (*sdot_f2c)(int64_t *, float *, int64_t *, float *, int64_t *)   = sdot_addr;

    // This `sdot_` invocation will return properly in either the `float` or `double` calling conventions
    float A[1] = {.5};
    float B[1] = {.5};
    int64_t n = 1;
    int64_t inca = 1;
    int64_t incb = 1;
    float plain = sdot_plain(&n, &A[0], &inca, &B[0], &incb);
    float f2c = sdot_f2c(&n, &A[0], &inca, &B[0], &incb);

    if (plain == 0.25) {
        // Normal calling convention
        return LBT_F2C_PLAIN;
    }
    if (f2c == 0.25) {
        // It's an f2c style calling convention
        return LBT_F2C_REQUIRED;
    }
    // We have no idea what happened; nothing works and everything is broken
    return LBT_F2C_UNKNOWN;
}
#endif
