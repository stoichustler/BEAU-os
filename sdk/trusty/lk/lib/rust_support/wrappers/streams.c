#include <streams.h>

FILE* lk_stdin(void) {
    return stdin;
}

FILE* lk_stdout(void) {
    return stderr;
}

FILE* lk_stderr(void) {
    return stderr;
}
