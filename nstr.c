#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

char *n_str_strip_ws(char *str)
{
    char *p;

    if (str == NULL)
        return str;

    p = str;
    while (isspace((unsigned char)*p))
        p++;

    str = p;
    p += strlen(p);

    while (p > str && isspace((unsigned char)p[-1]))
        *--p = '\0';

    return str;
}

static
int n_str_vin(const char *s, va_list ap)
{
    va_list local_ap;
    char *p;

    va_copy(local_ap, ap);
    while ((p = va_arg(local_ap, char *)) != NULL) {
        if (strcmp(p, s) == 0) {
            va_end(local_ap);
            return 1;
        }
    }
    va_end(local_ap);

    return 0;
}


int n_str_in(const char *s,...)
{
    va_list ap;

    va_start(ap, s);
    int in = n_str_vin(s, ap);
    va_end(ap);

    return in;
}
