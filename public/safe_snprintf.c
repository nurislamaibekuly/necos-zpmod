#include "safe_snprintf.h"
#include <stdarg.h>
#include <stdio.h>

#if defined(_MSC_VER) || defined(XASH_WIN32)
int safe_snprintf(char *buffer, int buffersize, const char *format, ...)
{
    va_list args;
    int result;

    if( buffersize <= 0 )
        return -1;

    va_start(args, format);
    result = _vsnprintf(buffer, buffersize, format, args);
    va_end(args);

    if(result >= buffersize)
        buffer[buffersize-1] = '\0';

    return result;
}
#endif
