#include "native.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef struct Block {
    size_t size;
    struct Block *next;
    uint32_t in_use;
    uint32_t padding;
} Block;

static Block *arena;
#ifdef PT_PROFILE
static uint32_t live_bytes, peak_bytes;
#endif

static void start_heap(void *p, uint32_t size)
{
    arena = p;
    arena->size = size - sizeof(*arena);
    arena->next = NULL;
    arena->in_use = 0;
#ifdef PT_PROFILE
    live_bytes = peak_bytes = 0;
#endif
}

void *malloc(size_t size)
{
    if (!size || size > 0x7ffffff0) return NULL;
    size = (size + 7) & ~(size_t)7;
    for (Block *block = arena; block; block = block->next) {
        if (block->in_use || block->size < size) continue;
        if (block->size >= size + sizeof(Block) + 8) {
            Block *split = (void *)((uint8_t *)(block + 1) + size);
            split->size = block->size - size - sizeof(Block);
            split->next = block->next;
            split->in_use = 0;
            block->next = split;
            block->size = size;
        }
        block->in_use = 1;
#ifdef PT_PROFILE
        live_bytes += block->size + sizeof(Block);
        if (live_bytes > peak_bytes) peak_bytes = live_bytes;
#endif
        return block + 1;
    }
    return NULL;
}

void free(void *ptr)
{
    if (!ptr) return;
    Block *released = (Block *)ptr - 1;
    pt_wipe(ptr, released->size);
#ifdef PT_PROFILE
    live_bytes -= released->size + sizeof(Block);
#endif
    released->in_use = 0;
    for (Block *block = arena; block && block->next;) {
        if (!block->in_use && !block->next->in_use) {
            block->size += block->next->size + sizeof(Block);
            block->next = block->next->next;
        } else {
            block = block->next;
        }
    }
}

void *realloc(void *ptr, size_t size)
{
    if (!size) {
        free(ptr);
        return NULL;
    }
    if (!ptr) return malloc(size);
    size_t old = ((Block *)ptr - 1)->size;
    if (old >= size) return ptr;
    void *next = malloc(size);
    if (next) {
        memcpy(next, ptr, old);
        free(ptr);
    }
    return next;
}

void *calloc(size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size) return NULL;
    size *= count;
    void *p = malloc(size);
    if (p) memset(p, 0, size);
    return p;
}

void *memcpy(void *dest, const void *source, size_t length)
{
    uint8_t *out = dest;
    const uint8_t *in = source;
    while (length--) *out++ = *in++;
    return dest;
}

void *memmove(void *dest, const void *source, size_t length)
{
    uint8_t *out = dest;
    const uint8_t *in = source;
    if ((uintptr_t)out <= (uintptr_t)in) return memcpy(dest, source, length);
    while (length) {
        --length;
        out[length] = in[length];
    }
    return dest;
}

void *memset(void *dest, int value, size_t length)
{
    uint8_t *out = dest;
    while (length--) *out++ = (uint8_t)value;
    return dest;
}

int memcmp(const void *first, const void *second, size_t length)
{
    const uint8_t *a = first, *b = second;
    while (length--) {
        if (*a != *b) return *a - *b;
        a++;
        b++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    const char *end = s;
    while (*end) end++;
    return (size_t)(end - s);
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n--) {
        if (*a != *b) return (uint8_t)*a - (uint8_t)*b;
        if (!*a) break;
        a++;
        b++;
    }
    return 0;
}

int strcmp(const char *a, const char *b)
{
    return strncmp(a, b, SIZE_MAX);
}

char *strchr(const char *s, int value)
{
    do {
        if (*s == (char)value) return (char *)s;
    } while (*s++);
    return NULL;
}

char *strrchr(const char *s, int value)
{
    char *found = NULL;
    do {
        if (*s == (char)value) found = (char *)s;
    } while (*s++);
    return found;
}

char *strstr(const char *s, const char *needle)
{
    size_t n = strlen(needle);
    do {
        if (!strncmp(s, needle, n)) return (char *)s;
    } while (*s++);
    return NULL;
}

char *strncpy(char *dest, const char *source, size_t n)
{
    char *p = dest;
    while (n && *source) {
        *p++ = *source++;
        n--;
    }
    memset(p, 0, n);
    return dest;
}

char *strncat(char *dest, const char *source, size_t n)
{
    char *end = dest + strlen(dest);
    while (n-- && *source) *end++ = *source++;
    *end = 0;
    return dest;
}

char *strdup(const char *source)
{
    size_t length = strlen(source) + 1;
    char *copy = malloc(length);
    if (copy) memcpy(copy, source, length);
    return copy;
}

int abs(int n) { return n < 0 ? -n : n; }

int tolower(int n)
{
    return n >= 'A' && n <= 'Z' ? n + ('a' - 'A') : n;
}
int strcasecmp(const char *a, const char *b)
{
    while (*a && tolower((uint8_t)*a) == tolower((uint8_t)*b)) {
        a++;
        b++;
    }
    return tolower((uint8_t)*a) - tolower((uint8_t)*b);
}
int isspace(int n)
{
    return n == ' ' || (n >= '\t' && n <= '\r');
}
int isdigit(int n) { return n >= '0' && n <= '9'; }
int isprint(int n) { return n >= 32 && n <= 126; }

long strtol(const char *p, char **end, int base)
{
    const char *start = p;
    while (isspace(*p)) p++;
    int negative = *p == '-';
    if (*p == '+' || *p == '-') p++;
    if (!base) base = 10;
    unsigned long value = 0;
    const char *digits = p;
    for (; *p; p++) {
        int ch = tolower((uint8_t)*p);
        int digit = isdigit(ch) ? ch - '0' : ch - 'a' + 10;
        if ((!isdigit(ch) && (ch < 'a' || ch > 'z')) || digit >= base) break;
        if (value > (0x7fffffffUL + (unsigned)negative - (unsigned)digit) / (unsigned)base)
            value = 0x7fffffffUL + (unsigned)negative;
        else
            value = value * (unsigned)base + (unsigned)digit;
    }
    if (end) *end = (char *)(digits == p ? start : p);
    return (long)(negative ? 0UL - value : value);
}

typedef struct {
    char *text;
    size_t capacity;
    size_t count;
} Format;

static void output(Format *f, char c)
{
    if (f->count + 1 < f->capacity) f->text[f->count] = c;
    f->count++;
}

int vsnprintf(char *text, size_t capacity, const char *format, va_list arguments)
{
    Format f = {text, capacity, 0};
    while (*format) {
        char ch = *format++;
        if (ch != '%') {
            output(&f, ch);
            continue;
        }
        int limit = -1;
        if (format[0] == '.' && format[1] == '*') {
            limit = va_arg(arguments, int);
            format += 2;
        }
        int wide = *format == 'l';
        if (wide) format++;
        ch = *format++;
        if (ch == 's') {
            const char *s = va_arg(arguments, const char *);
            for (int i = 0; s[i] && (limit < 0 || i < limit); i++) output(&f, s[i]);
        } else if (ch == 'c') {
            output(&f, (char)va_arg(arguments, int));
        } else if (ch == '%') {
            output(&f, '%');
        } else if (ch == 'd' || ch == 'u') {
            unsigned long magnitude;
            if (ch == 'd') {
                long value = wide ? va_arg(arguments, long) : va_arg(arguments, int);
                if (value < 0) output(&f, '-');
                magnitude = value < 0 ? 0UL - (unsigned long)value : (unsigned long)value;
            } else {
                magnitude = wide ? va_arg(arguments, unsigned long) : va_arg(arguments, unsigned);
            }
            char digits[16];
            size_t count = 0;
            do {
                digits[count++] = (char)('0' + magnitude % 10);
                magnitude /= 10;
            } while (magnitude);
            while (count) output(&f, digits[--count]);
        } else {
            break;
        }
    }
    size_t written = f.count < capacity ? f.count : capacity ? capacity - 1 : 0;
    if (capacity) text[written] = 0;
    /* libvterm's callers expect the bytes actually present. */
    return (int)written;
}

int snprintf(char *text, size_t capacity, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int length = vsnprintf(text, capacity, format, args);
    va_end(args);
    return length;
}

void __aeabi_memcpy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy4(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy8(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memclr(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr4(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr8(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memmove(void *d, const void *s, size_t n) { memmove(d, s, n); }
void __aeabi_memset(void *d, size_t n, int c) { memset(d, c, n); }
void __aeabi_memset4(void *d, size_t n, int c) { memset(d, c, n); }

static uint32_t recovery[10];
static Block *heap_base;
static uint32_t heap_size;

extern int pt_set_return(uint32_t *saved) __attribute__((returns_twice));
extern void pt_fault_return(uint32_t *saved) __attribute__((noreturn));
extern void pt_reset_after_fault(void);

void abort(void) { pt_fault_return(recovery); }
void exit(int status) { (void)status; abort(); }

void *stderr;

int fprintf(void *unused, const char *format, ...)
{
    (void)unused;
    (void)format;
    return 0;
}

int32_t pt_arm_dispatch(const uint8_t *packet)
{
    if (pt_set_return(recovery)) {
        pt_reset_after_fault();
        if (heap_base) {
            pt_wipe(heap_base, heap_size);
            start_heap(heap_base, heap_size);
        }
        return PT_NO_MEMORY;
    }
    uint32_t command = pt_le32(packet + 4);
    void *buffer = (void *)(uintptr_t)pt_le32(packet + 8);
    uint32_t size = pt_le32(packet + 12);
    if (command == PT_BOOT) {
        heap_base = buffer;
        heap_size = size;
        start_heap(buffer, size);
    }
#ifdef PT_PROFILE
    if (command == PT_STATS) return (int32_t)peak_bytes;
#endif
    return pt_dispatch(command, buffer, size, pt_le32(packet + 16));
}
