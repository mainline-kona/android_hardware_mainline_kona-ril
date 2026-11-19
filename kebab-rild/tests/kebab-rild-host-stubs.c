/* Host-side stubs for a kebab-rild smoke test (not shipped). */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int property_get(const char *key, char *value, const char *default_value)
{
    const char *v = default_value ? default_value : "";

    if (!strcmp(key, "persist.vendor.kebab.service_domain"))
        v = "2";
    snprintf(value, 92, "%s", v);
    return (int)strlen(value);
}

int property_set(const char *key, const char *value)
{
    (void)key;
    (void)value;
    return 0;
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    int n;

    (void)prio;
    fprintf(stderr, "[%s] ", tag);
    va_start(ap, fmt);
    n = vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    return n;
}

int __android_log_write(int prio, const char *tag, const char *text)
{
    return __android_log_print(prio, tag, "%s", text);
}
