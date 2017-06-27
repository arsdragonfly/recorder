// ****************************************************************************
//  recorder.c                                                Recorder project
// ****************************************************************************
//
//   File Description:
//
//     Implementation of a non-blocking flight recorder
//
//
//
//
//
//
//
//
// ****************************************************************************
//  (C) 2017 Christophe de Dinechin <christophe@dinechin.org>
//   This software is licensed under the GNU General Public License v3
//   See file LICENSE for details.
// ****************************************************************************

#include "recorder.h"
#include "config.h"
#include <fcntl.h>
#include <pthread.h>
#include <regex.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/stat.h>



// ============================================================================
//
//    Recorder dump utility
//
// ============================================================================

/// Global counter indicating the order of entries across recorders.
uintptr_t       recorder_order   = 0;

/// List of the currently active flight recorders (ring buffers)
recorder_info * recorders        = NULL;

/// List of the currently active tweaks
recorder_tweak *tweaks           = NULL;


static void recorder_dump_entry(const char         *label,
                                recorder_entry     *entry,
                                recorder_format_fn  format,
                                recorder_show_fn    show,
                                void               *output)
// ----------------------------------------------------------------------------
//  Dump a recorder entry in a buffer between dst and dst_end, return last pos
// ----------------------------------------------------------------------------
{
    char            buffer[256];
    char            format_buffer[32];
    char           *dst         = buffer;
    char           *dst_end     = buffer + sizeof buffer - 2;
    const char     *fmt         = entry->format;
    unsigned        argIndex    = 0;
    const unsigned  maxArgIndex = sizeof(entry->args) / sizeof(entry->args[0]);

    // Apply formatting. This complicated loop is because
    // we need to detect floating-point values, which are passed
    // differently on many architectures such as x86 or ARM
    // (passed in different registers). So we detect them from the format,
    // convert intptr_t to float or double depending on its size,
    // and call the variadic snprintf passing a double value that will
    // naturally go in the right register. A bit ugly.
    bool finishedInNewline = false;
    while (dst < dst_end && argIndex < maxArgIndex)
    {
        char c = *fmt++;
        if (c != '%')
        {
            *dst = c;
            if (!c)
                break;
            dst++;
        }
        else
        {
            char *fmtCopy = format_buffer;
            int floatingPoint = 0;
            int done = 0;
            int unsupported = 0;
            *fmtCopy++ = c;
            char *fmt_end = format_buffer + sizeof format_buffer - 1;
            while (!done && fmt < fmt_end)
            {
                c = *fmt++;
                *fmtCopy++ = c;
                switch(c)
                {
                case 'f': case 'F':  // Floating point formatting
                case 'g': case 'G':
                case 'e': case 'E':
                case 'a': case 'A':
                    floatingPoint = 1;
                    // Fall through here on purpose
                case 'b':           // Integer formatting
                case 'c': case 'C':
                case 's': case 'S':
                case 'd': case 'D':
                case 'i':
                case 'o': case 'O':
                case 'u': case 'U':
                case 'x':
                case 'X':
                case 'p':
                case '%':
                case 0:             // End of string
                    done = 1;
                    break;

                    // GCC: case '0' ... '9', not supported on IAR
                case '0': case '1': case '2': case '3': case '4':
                case '5': case '6': case '7': case '8': case '9':
                case '.':
                case '+':
                case '-':
                case 'l': case 'L':
                case 'h':
                case 'j':
                case 't':
                case 'z':
                case 'q':
                case 'v':
                    break;
                case 'n':           // Expect two args
                case '*':
                default:
                    unsupported = 1;
                    break;
                }
            }
            if (!c || unsupported)
                break;
            int isString = (c == 's' || c == 'S');
            *fmtCopy++ = 0;
            if (floatingPoint)
            {
                double floatArg;
                if (sizeof(intptr_t) == sizeof(float))
                {
                    union { float f; intptr_t i; } u;
                    u.i = entry->args[argIndex++];
                    floatArg = (double) u.f;
                }
                else
                {
                    union { double d; intptr_t i; } u;
                    u.i = entry->args[argIndex++];
                    floatArg = u.d;
                }
                dst += snprintf(dst, dst_end - dst,
                                format_buffer,
                                floatArg);
            }
            else
            {
                intptr_t arg = entry->args[argIndex++];
                if (isString && arg == 0)
                    arg = (intptr_t) "<NULL>";
                dst += snprintf(dst, dst_end - dst,
                                format_buffer, arg);
            }
        }
        finishedInNewline = c == '\n';
    }
    if (!finishedInNewline)
        *dst++ = '\n';
    *dst++ = 0;

    format(show, output, label,
           entry->where, entry->order, entry->timestamp, buffer);
}



// ============================================================================
//
//    Default output prints things to stderr
//
// ============================================================================

static void * recorder_output = NULL;
void *recorder_configure_output(void *output)
// ----------------------------------------------------------------------------
//   Configure the output stream
// ----------------------------------------------------------------------------
{
    void *previous = recorder_output;
    recorder_output = output;
    return previous;
}


static unsigned recorder_print(const char *ptr, size_t len, void *file_arg)
// ----------------------------------------------------------------------------
//   The default printing function - prints to stderr
// ----------------------------------------------------------------------------
{
    FILE *file = file_arg ? file_arg : stderr;
    return (unsigned) fwrite(ptr, 1, len, file);
}


static recorder_show_fn recorder_show = recorder_print;
recorder_show_fn  recorder_configure_show(recorder_show_fn show)
// ----------------------------------------------------------------------------
//   Configure the function used to output data to the stream
// ----------------------------------------------------------------------------
{
    recorder_show_fn previous = recorder_show;
    recorder_show = show;
    return previous;
}



// ============================================================================
//
//    Default format for recorder entries
//
// ============================================================================

// Truly shocking that Visual Studio before 2015 does not have a working
// snprintf or vsnprintf. Note that the proposed replacements are not accurate
// since they return -1 on overflow instead of the length that would have
// been written as the standard mandates.
// http://stackoverflow.com/questions/2915672/snprintf-and-visual-studio-2010.
#if defined(_MSC_VER) && _MSC_VER < 1900
#  define snprintf  _snprintf
#endif

static void recorder_format_entry(recorder_show_fn show,
                                  void *output,
                                  const char *label,
                                  const char *location,
                                  uintptr_t order,
                                  uintptr_t timestamp,
                                  const char *message)
// ----------------------------------------------------------------------------
//   Default formatting for the entries
// ----------------------------------------------------------------------------
{
    char buffer[256];
    char *dst = buffer;
    char *dst_end = buffer + sizeof buffer;

    if (UINTPTR_MAX >= 0x7fffffff) // Static if to detect how to display time
    {
        // Time stamp in us, show in seconds
        dst += snprintf(dst, dst_end - dst,
                        "%s: [%lu %.6f] %s: %s",
                        location,
                        (unsigned long) order,
                        (double) timestamp / RECORDER_HZ,
                        label, message);
    }
    else
    {
        // Time stamp  in ms, show in seconds
        dst += snprintf(dst, dst_end - dst,
                        "%s: [%lu %.3f] %s: %s",
                        location,
                        (unsigned long) order,
                        (float) timestamp / RECORDER_HZ,
                        label, message);
    }

    show(buffer, dst - buffer, output);
}


static recorder_format_fn recorder_format = recorder_format_entry;
recorder_format_fn recorder_configure_format(recorder_format_fn format)
// ----------------------------------------------------------------------------
//   Configure the function used to format entries
// ----------------------------------------------------------------------------
{
    recorder_format_fn previous = recorder_format;
    recorder_format = format;
    return previous;
}


unsigned recorder_sort(const char *what,
                       recorder_format_fn format,
                       recorder_show_fn show, void *output)
// ----------------------------------------------------------------------------
//   Dump all entries, sorted by their global 'order' field
// ----------------------------------------------------------------------------
{
    recorder_entry entry;
    regex_t        re;
    unsigned       dumped = 0;

    int status = regcomp(&re, what, REG_EXTENDED|REG_NOSUB|REG_ICASE);

    while (status == 0)
    {
        uintptr_t      lowestOrder = ~0UL;
        recorder_info *lowest      = NULL;
        recorder_info *rec;

        for (rec = recorders; rec; rec = rec->next)
        {
            // Skip recorders that don't match the pattern
            if (regexec(&re, rec->name, 0, NULL, 0) != 0)
                continue;

            // Loop while this recorder is readable and we can find next order
            if (rec->readable())
            {
                rec->peek(&entry);
                if (entry.order < lowestOrder)
                {
                    lowest = rec;
                    lowestOrder = entry.order;
                }
            }
        }

        if (!lowest)
            break;

        // The first read may fail due to 'catch up', if so continue
        if (!lowest->read(&entry, 1))
            continue;

        recorder_dump_entry(lowest->name, &entry, format, show, output);
        dumped++;
    }

    regfree(&re);

    return dumped;
}


unsigned recorder_dump(void)
// ----------------------------------------------------------------------------
//   Dump all entries, sorted by their global 'order' field
// ----------------------------------------------------------------------------
{
    return recorder_sort(".*", recorder_format,recorder_show,recorder_output);
}


unsigned recorder_dump_for(const char *what)
// ----------------------------------------------------------------------------
//   Dump all entries for recorder with names matching 'what'
// ----------------------------------------------------------------------------
{
    return recorder_sort(what, recorder_format,recorder_show,recorder_output);
}



// ============================================================================
//
//    Implementation of recorder shared memory structures
//
// ============================================================================

typedef struct recorder_shmem_sh
// ----------------------------------------------------------------------------
//   Shared-memory information about recorder_shmem
// ----------------------------------------------------------------------------
{
    uint32_t    magic;          // Magic number to check structure type
    uint32_t    version;        // Version number for shared memory format
    off_t       head;           // First recorder_chan in linked list
    off_t       free_list;      // Free list
    off_t       offset;         // Current offset for new recorder_shmem
} recorder_shmem_sh, *recorder_shmem_shp;


typedef struct recorder_shmem
// ----------------------------------------------------------------------------
//   Information about mapping of shared recorder_shmem
// ----------------------------------------------------------------------------
{
    int             fd;         // File descriptor for mmap
    void *          map_addr;   // Address in memory for mmap
    size_t          map_size;   // Size allocated for mmap
    recorder_chan_p head;       // First recorder_chan in list
} recorder_shmem_t;


typedef struct recorder_chan_sh
// ----------------------------------------------------------------------------
//   A named data recorder_chan in shared memory
// ----------------------------------------------------------------------------
{
    recorder_type type;         // Data type stored in recorder_chan
    off_t         next;         // Offset to next recorder_chan in linked list
    off_t         name;         // Offset of name in recorder_chan_sh
    off_t         description;  // Offset of description
    off_t         unit;         // Offset of measurement unit
    recorder_data min;          // Minimum value
    recorder_data max;          // Maximum value
    ring_t        ring;         // Ring data
} recorder_chan_sh, *recorder_chan_shp;


typedef struct recorder_chan
// ----------------------------------------------------------------------------
//   Accessing a shared memory recorder_chan
// ----------------------------------------------------------------------------
{
    recorder_shmem_p recorder_shmem;
    off_t            offset;
    recorder_chan_p  next;
} recorder_chan_t, *recorder_chan_p;


// Map memory in 4K chunks (one page)
#define MAP_SIZE        4096


static inline recorder_chan_shp recorder_chan_shared(recorder_chan_p chan)
// ----------------------------------------------------------------------------
//  Return the recorder_chan shared address
// ----------------------------------------------------------------------------
{
    recorder_shmem_p   chans    = chan->recorder_shmem;
    char              *map_addr = chans->map_addr;
    recorder_chan_shp  chanp    = (recorder_chan_shp) (map_addr + chan->offset);
    return chanp;
}



// ============================================================================
//
//    Interface for the local process
//
// ============================================================================

static bool recorder_chan_file_extend(int fd, off_t new_size)
// ----------------------------------------------------------------------------
//    Extend a file to the given size
// ----------------------------------------------------------------------------
{
    return
        lseek(fd, new_size-1, SEEK_SET) != -1 &&
        write(fd, "", 1) == 1;
}


recorder_shmem_p recorder_shmem_new(const char *file)
// ----------------------------------------------------------------------------
//   Create a new mmap'd file
// ----------------------------------------------------------------------------
{
    // Open the file
    int fd = open(file, O_RDWR|O_CREAT|O_TRUNC, (mode_t) 0600);
    if (fd == -1)
        return NULL;

    // Make sure we have enough space for the data
    size_t map_size = MAP_SIZE;
    if (!recorder_chan_file_extend(fd, map_size))
    {
        close(fd);
        return NULL;
    }

    // Map space for the recorder_shmem
    off_t  offset = 0;
    void *map_addr = mmap(NULL, map_size,
                          PROT_READ | PROT_WRITE,
                          MAP_FILE | MAP_SHARED,
                          fd, offset);
    if (map_addr == MAP_FAILED)
    {
        close(fd);
        return NULL;
    }

    // Successful: Initialize in-memory recorder_shmem list
    recorder_shmem_p chans = malloc(sizeof(recorder_shmem_t));
    chans->fd = fd;
    chans->map_addr = map_addr;
    chans->map_size = map_size;
    chans->head = NULL;

    // Initialize shared-memory data
    recorder_shmem_shp chansp = map_addr;
    chansp->magic = RECORDER_CHAN_MAGIC;
    chansp->version = RECORDER_CHAN_VERSION;
    chansp->head = 0;
    chansp->free_list = 0;
    chansp->offset = sizeof(recorder_shmem_sh);

    return chans;
}


void recorder_shmem_delete(recorder_shmem_p shmem)
// ----------------------------------------------------------------------------
//   Delete the local recorder_shmem
// ----------------------------------------------------------------------------
{
    int i;
    recorder_info *rec;
    for (rec = recorders; rec; rec = rec->next)
    {
        if (rec->trace == INTPTR_MAX)
            rec->trace = 0;
        for (i = 0; i < 4; i++)
            rec->exported[i] = NULL;
    }

    recorder_chan_p next = NULL;
    recorder_chan_p chan;
    for (chan = shmem->head; chan; chan = next)
    {
        next = chan->next;
        recorder_chan_delete(chan);
    }

    munmap(shmem->map_addr, shmem->map_size);
    close(shmem->fd);
    free(shmem);
}


recorder_chan_p recorder_chan_new(recorder_shmem_p shmem,
                                  recorder_type    type,
                                  size_t           size,
                                  const char *     name,
                                  const char *     description,
                                  const char *     unit,
                                  recorder_data    min,
                                  recorder_data    max)
// ----------------------------------------------------------------------------
//    Allocate and create a new recorder_chan
// ----------------------------------------------------------------------------
{
    recorder_shmem_shp shmemp      = shmem->map_addr;
    size_t             offset      = shmemp->offset;
    size_t             item_size   = 2 * sizeof(recorder_data);

    size_t             name_len    = strlen(name);
    size_t             descr_len   = strlen(description);
    size_t             unit_len    = strlen(unit);

    size_t             name_offs   = sizeof(recorder_chan_sh) + size*item_size;
    size_t             descr_offs  = name_offs + name_len + 1;
    size_t             unit_offs   = descr_offs + descr_len + 1;

    size_t             alloc       = unit_offs + unit_len + 1;

    // TODO: Find one from the free list if there is one

    size_t align = sizeof(long double);
    size_t new_offset = (offset + alloc + align-1) & ~(align-1);
    if (new_offset >= shmem->map_size)
    {
        size_t map_size = (new_offset / MAP_SIZE + 1) * MAP_SIZE;
        if (!recorder_chan_file_extend(shmem->fd, map_size))
            return NULL;
        void *map_addr = mmap(shmem->map_addr, map_size,
                              PROT_READ | PROT_WRITE,
                              MAP_FILE | MAP_SHARED | MAP_FIXED,
                              shmem->fd, 0);
        if (map_addr == MAP_FAILED)
            return NULL;

        // Note that if the new mapping address is different,
        // all recorder_chan_p become invalid
        shmem->map_size = map_size;
        shmem->map_addr = map_addr;
    }
    shmemp->offset = new_offset;

    // Initialize recorder_chan fields
    recorder_chan_shp chanp = (recorder_chan_shp)
        ((char *) shmem->map_addr + offset);
    chanp->type = type;
    chanp->next = shmemp->head;
    chanp->name = name_offs;
    chanp->description = descr_offs;
    chanp->unit = unit_offs;
    chanp->min = min;
    chanp->max = max;
    memcpy((char *) chanp + name_offs, name, name_len + 1);
    memcpy((char *) chanp + descr_offs, description, descr_len + 1);
    memcpy((char *) chanp + unit_offs, unit, unit_len + 1);

    // Initialize ring fields
    ring_p ring = &chanp->ring;
    ring->size = size;
    ring->item_size = item_size;
    ring->reader = 0;
    ring->writer = 0;
    ring->commit = 0;
    ring->overflow = 0;

    // Link recorder_chan in recorder_shmem list
    shmemp->head = offset;

    // Create recorder_chan access
    recorder_chan_p chan = malloc(sizeof(recorder_chan_t));
    chan->recorder_shmem = shmem;
    chan->offset = offset;
    chan->next = shmem->head;
    shmem->head = chan;

    return chan;
}


void recorder_chan_delete(recorder_chan_p chan)
// ----------------------------------------------------------------------------
//   Delete a recorder_chan
// ----------------------------------------------------------------------------
{
    recorder_shmem_p    shmem       = chan->recorder_shmem;
    intptr_t            chan_offset = chan->offset;
    char               *map_addr    = shmem->map_addr;
    recorder_shmem_shp  shmemp      = (recorder_shmem_shp) map_addr;
    off_t              *last        = &shmemp->head;
    off_t               offset;

    for (offset = *last; offset; offset = *last)
    {
        recorder_chan_shp chanp = (recorder_chan_shp) (map_addr + offset);
        if (*last == chan_offset)
        {
            *last = chanp->next;
            chanp->next = shmemp->free_list;
            shmemp->free_list = chan->offset;
            break;
        }
        last = &chanp->next;
    }

    free(chan);
}


size_t recorder_chan_write(recorder_chan_p chan, const void *ptr, size_t count)
// ----------------------------------------------------------------------------
//   Write some data in the recorder_chan
// ----------------------------------------------------------------------------
{
    recorder_chan_shp chanp = recorder_chan_shared(chan);
    return ring_write(&chanp->ring, ptr, count, NULL, NULL, NULL);
}


size_t recorder_chan_writable(recorder_chan_p chan)
// ----------------------------------------------------------------------------
//    Return number of items that can be written in ring
// ----------------------------------------------------------------------------
{
    recorder_chan_shp chanp = recorder_chan_shared(chan);
    return ring_writable(&chanp->ring);
}


ringidx_t recorder_chan_writer(recorder_chan_p chan)
// ----------------------------------------------------------------------------
//    Return current writer index
// ----------------------------------------------------------------------------
{
    recorder_chan_shp chanp = recorder_chan_shared(chan);
    return chanp->ring.writer;
}



// ============================================================================
//
//    Subscribing to recorder_shmem in a remote process
//
// ============================================================================

recorder_shmem_p recorder_shmem_open(const char *file)
// ----------------------------------------------------------------------------
//    Map the file in memory, and scan its structure
// ----------------------------------------------------------------------------
{
    int fd = open(file, O_RDWR);
    if (fd == -1)
        return NULL;
    struct stat stat;
    if (fstat(fd, &stat) != 0)
        return NULL;

    // Map space for the recorder_shmem
    size_t  map_size = stat.st_size;
    off_t   offset   = 0;
    void   *map_addr = mmap(NULL, map_size,
                            PROT_READ|PROT_WRITE,
                            MAP_FILE | MAP_SHARED,
                            fd, offset);
    recorder_shmem_shp shmemp = map_addr;
    if (map_addr == MAP_FAILED                  ||
        shmemp->magic != RECORDER_CHAN_MAGIC          ||
        shmemp->version != RECORDER_CHAN_VERSION)
    {
        close(fd);
        return NULL;
    }

    // Successful: Initialize with recorder_chan descriptor
    recorder_shmem_p shmem = malloc(sizeof(recorder_shmem_t));
    shmem->fd = fd;
    shmem->map_addr = map_addr;
    shmem->map_size = map_size;
    shmem->head = NULL;

    // Create recorder_shmem for all recorder_shmem in shared memory
    recorder_chan_shp chanp;
    off_t             off;
    for (off = shmemp->head; off; off = chanp->next)
    {
        chanp = (recorder_chan_shp) ((char *) map_addr + off);
        recorder_chan_p chan = malloc(sizeof(recorder_chan_t));
        chan->recorder_shmem = shmem;
        chan->offset = off;
        chan->next = shmem->head;
        shmem->head = chan;
    }

    return shmem;
}


void recorder_shmem_close(recorder_shmem_p shmem)
// ----------------------------------------------------------------------------
//   Close shared memory recorder_shmem
// ----------------------------------------------------------------------------
{
    recorder_shmem_delete(shmem);
}


recorder_chan_p recorder_chan_find(recorder_shmem_p  shmem,
                                   const char       *pattern,
                                   recorder_chan_p   after)
// ----------------------------------------------------------------------------
//   Find a recorder_chan with the given name in the recorder_chan list
// ----------------------------------------------------------------------------
{
    regex_t re;
    int     status = regcomp(&re, pattern, REG_EXTENDED|REG_NOSUB|REG_ICASE);
    recorder_chan_p first = after ? after->next : shmem->head;
    recorder_chan_p chan = NULL;;

    if (status == 0)
        for (chan = first; chan; chan = chan->next)
            if (regexec(&re, pattern, 0, NULL, 0) == 0)
                break;

    regfree(&re);
    return chan;
}


const char *recorder_chan_name(recorder_chan_p chan)
// ----------------------------------------------------------------------------
//   Return the name for a given recorder_chan
// ----------------------------------------------------------------------------
{
    recorder_chan_shp chanp = recorder_chan_shared(chan);
    return (const char *) chanp + chanp->name;
}


const char *recorder_chan_description(recorder_chan_p chan)
// ----------------------------------------------------------------------------
//   Return the description for a given recorder_chan
// ----------------------------------------------------------------------------
{
    recorder_chan_shp chanp = recorder_chan_shared(chan);
    return (const char *) chanp + chanp->description;
}


const char *recorder_chan_unit(recorder_chan_p chan)
// ----------------------------------------------------------------------------
//   Return the measurement unit for a given recorder_chan
// ----------------------------------------------------------------------------
{
    recorder_chan_shp chanp = recorder_chan_shared(chan);
    return (const char *) chanp + chanp->unit;
}


recorder_data recorder_chan_min(recorder_chan_p chan)
// ----------------------------------------------------------------------------
//   Return the min value specified for the given channel
// ----------------------------------------------------------------------------
{
    recorder_chan_shp chanp = recorder_chan_shared(chan);
    return chanp->min;
}


recorder_data recorder_chan_max(recorder_chan_p chan)
// ----------------------------------------------------------------------------
//   Return the max value specified for the given channel
// ----------------------------------------------------------------------------
{
    recorder_chan_shp chanp = recorder_chan_shared(chan);
    return chanp->max;
}


recorder_type recorder_chan_type(recorder_chan_p chan)
// ----------------------------------------------------------------------------
//   Return the element type for a given recorder_chan
// ----------------------------------------------------------------------------
{
    recorder_chan_shp chanp = recorder_chan_shared(chan);
    return chanp->type;
}


size_t recorder_chan_size(recorder_chan_p chan)
// ----------------------------------------------------------------------------
//   Return the ring size for a given recorder_chan
// ----------------------------------------------------------------------------
{
    recorder_chan_shp chanp = recorder_chan_shared(chan);
    return chanp->ring.size;
}


size_t recorder_chan_item_size(recorder_chan_p chan)
// ----------------------------------------------------------------------------
//   Return the ring item size for a given recorder_chan
// ----------------------------------------------------------------------------
{
    recorder_chan_shp chanp = recorder_chan_shared(chan);
    return chanp->ring.item_size;
}


size_t recorder_chan_readable(recorder_chan_p chan)
// ----------------------------------------------------------------------------
//   Return number of readable elements in ring
// ----------------------------------------------------------------------------
{
    recorder_chan_shp chanp = recorder_chan_shared(chan);
    return ring_readable(&chanp->ring);
}


size_t recorder_chan_read(recorder_chan_p chan,
                          recorder_data *ptr, size_t count)
// ----------------------------------------------------------------------------
//   Read data from the ring
// ----------------------------------------------------------------------------
{
    recorder_chan_shp chanp = recorder_chan_shared(chan);
    return ring_read(&chanp->ring, ptr, count, NULL, NULL, NULL);
}


ringidx_t recorder_chan_reader(recorder_chan_p chan)
// ----------------------------------------------------------------------------
//   Return current reader index for recorder_chan
// ----------------------------------------------------------------------------
{
    recorder_chan_shp chanp = recorder_chan_shared(chan);
    return chanp->ring.reader;
}


void recorder_export(recorder_shmem_p  shmem,
                     recorder_info    *info,
                     size_t            size,
                     unsigned          index,
                     recorder_type     type,
                     const char       *name,
                     const char       *descr,
                     const char       *unit,
                     recorder_data     min,
                     recorder_data     max)
// ----------------------------------------------------------------------------
//    Export the given recorder recorder_chan to a shared-memory ring
// ----------------------------------------------------------------------------
{
    if (index < 4)
    {
        recorder_chan_p chan = info->exported[index];
        if (chan)
            recorder_chan_delete(chan);
        chan = recorder_chan_new(shmem, type, size,
                                 name, descr, unit, min, max);
        info->exported[index] = chan;
        if (info->trace == 0)
            info->trace = INTPTR_MAX;
    }
}


void recorder_export_u(recorder_shmem_p  shmem,
                       recorder_info    *info,
                       size_t            size,
                       unsigned          index,
                       const char       *name,
                       const char       *descr,
                       const char       *unit,
                       uintptr_t         min,
                       uintptr_t         max)
// ----------------------------------------------------------------------------
//   Export unsigned channel
// ----------------------------------------------------------------------------
{
    recorder_data mindata, maxdata;
    mindata.unsigned_value = min;
    maxdata.unsigned_value = max;
    recorder_export(shmem, info, index, RECORDER_UNSIGNED, size,
                    name, descr, unit, mindata, maxdata);
}


void recorder_export_s(recorder_shmem_p  shmem,
                       recorder_info    *info,
                       size_t            size,
                       unsigned          index,
                       const char       *name,
                       const char       *descr,
                       const char       *unit,
                       intptr_t          min,
                       intptr_t          max)
// ----------------------------------------------------------------------------
//   Export signed channel
// ----------------------------------------------------------------------------
{
    recorder_data mindata, maxdata;
    mindata.signed_value = min;
    maxdata.signed_value = max;
    recorder_export(shmem, info, index, RECORDER_SIGNED, size,
                    name, descr, unit, mindata, maxdata);
}


void recorder_export_r(recorder_shmem_p  shmem,
                       recorder_info    *info,
                       size_t            size,
                       unsigned          index,
                       const char       *name,
                       const char       *descr,
                       const char       *unit,
                       double            min,
                       double            max)
// ----------------------------------------------------------------------------
//   Export real (floating-point) channel
// ----------------------------------------------------------------------------
{
    recorder_data mindata, maxdata;
    mindata.real_value = min;
    maxdata.real_value = max;
    recorder_export(shmem, info, index, RECORDER_REAL, size,
                    name, descr, unit, mindata, maxdata);
}


void recorder_trace_entry(recorder_info *info, recorder_entry *entry)
// ----------------------------------------------------------------------------
//   Show a recorder entry when a trace is enabled
// ----------------------------------------------------------------------------
{
    unsigned i;
    if (info->trace != INTPTR_MAX)
        recorder_dump_entry(info->name, entry,
                            recorder_format, recorder_show, recorder_output);
    for (i = 0; i < 4; i++)
    {
        recorder_chan_p exported = info->exported[i];
        if (exported)
        {
            recorder_chan_shp  chanp  = recorder_chan_shared(exported);
            ring_p             ring   = &chanp->ring;
            ringidx_t          writer = ring_fetch_add(ring->writer, 1);
            recorder_data     *data   = (recorder_data *) (ring + 1);
            size_t             size   = ring->size;
            data += 2 * (writer % size);
            data[0].unsigned_value = entry->timestamp;
            data[1].unsigned_value = entry->args[i];
            ring_fetch_add(ring->commit, 1);
        }
    }
}



// ============================================================================
//
//   Background dump
//
// ============================================================================

RECORDER_TWEAK_DEFINE(recorder_dump_sleep, 100,
                      "Sleep time between background dumps (ms)");

static bool background_dump_running = false;


static void *background_dump(void *pattern)
// ----------------------------------------------------------------------------
//    Dump the recorder (background thread)
// ----------------------------------------------------------------------------
{
    const char *what = pattern;
    while (background_dump_running)
    {
        unsigned dumped = recorder_sort(what, recorder_format,
                                        recorder_show, recorder_output);
        if (dumped == 0)
        {
            struct timespec tm;
            tm.tv_sec  = 0;
            tm.tv_nsec = 1000000 * RECORDER_TWEAK(recorder_dump_sleep);
            nanosleep(&tm, NULL);
        }
    }
    return pattern;
}


void recorder_background_dump(const char *what)
// ----------------------------------------------------------------------------
//   Dump the selected recorders, sleeping sleep_ms if nothing to dump
// ----------------------------------------------------------------------------
{
    pthread_t tid;
    background_dump_running = true;
    if (strcmp(what, "all") == 0)
        what = ".*";
    pthread_create(&tid, NULL, background_dump, (void *) what);
}


void recorder_background_dump_stop(void)
// ----------------------------------------------------------------------------
//   Stop the background dump task
// ----------------------------------------------------------------------------
{
    background_dump_running = false;
}



// ============================================================================
//
//    Signal handling
//
// ============================================================================

RECORDER(signals, 32, "Information about signals");

// Saved old actions
#if HAVE_STRUCT_SIGACTION
typedef void (*sig_fn)(int, siginfo_t *, void *);
static struct sigaction old_action[NSIG] = { };

static void signal_handler(int sig, siginfo_t *info, void *ucontext)
// ----------------------------------------------------------------------------
//    Dump the recorder when receiving a signal
// ----------------------------------------------------------------------------
{
    RECORD(signals, "Received signal %s (%d) si_addr=%p, dumping recorder",
           strsignal(sig), sig, info->si_addr);
    fprintf(stderr, "Received signal %s (%d), dumping recorder\n",
            strsignal(sig), sig);

    // Restore previous handler in case we crash during the dump
    struct sigaction save, next;
    sigaction(sig, &old_action[sig], &save);
    recorder_dump();
    sigaction(sig, &save, &next);

    // If there is another handler, call it now
    if (next.sa_sigaction != (sig_fn) SIG_DFL &&
        next.sa_sigaction != (sig_fn) SIG_IGN)
        next.sa_sigaction(sig, info, ucontext);
}


void recorder_dump_on_signal(int sig)
// ----------------------------------------------------------------------------
//    C interface for Recorder::DumpOnSignal
// ----------------------------------------------------------------------------
{
    if (sig < 0 || sig >= NSIG)
        return;

    struct sigaction action;
    action.sa_sigaction = signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    sigaction(sig, &action, &old_action[sig]);
}

#else // !HAVE_STRUCT_SIGACTION

/* For MinGW, there is no struct sigaction */
typedef void (*sig_fn)(int);
static sig_fn old_handler[NSIG] = { };

static void signal_handler(int sig)
// ----------------------------------------------------------------------------
//   Dump the recorder when receiving the given signal
// ----------------------------------------------------------------------------
{
    RECORD(signals, "Received signal %d, dumping recorder", sig);
    fprintf(stderr, "Received signal %d, dumping recorder\n", sig);

    // Restore previous handler
    sig_fn save = signal(sig, old_handler[sig]);
    recorder_dump();
    sig_fn next = signal(sig, save);

    // If there is a 'next' handler, call it
    if (next != SIG_DFL && next != SIG_IGN)
        next(sig);
}


void recorder_dump_on_signal(int sig)
// ----------------------------------------------------------------------------
//    C interface for Recorder::DumpOnSignal
// ----------------------------------------------------------------------------
{
    if (sig < 0 || sig >= NSIG)
        return;
    old_handler[sig] = signal(sig, signal_handler);
}

#endif // HAVE_STRUCT_SIGACTION


enum
{
    RECORDER_SIGNALS_MASK = 0
#ifdef SIGQUIT
    | (1U << SIGQUIT)
#endif // SIGQUIT
#ifdef SIGILL
    | (1U << SIGILL)
#endif // SIGILL
#ifdef SIGABRT
    | (1U << SIGABRT)
#endif // SIGABRT
#ifdef SIGBUS
    | (1U << SIGBUS)
#endif // SIGBUS
#ifdef SIGSEGV
    | (1U << SIGSEGV)
#endif // SIGSEGV
#ifdef SIGSYS
    | (1U << SIGSYS)
#endif // SIGSYS
#ifdef SIGXCPU
    | (1U << SIGXCPU)
#endif // SIGXCPU
#ifdef SIGXFSZ
    | (1U << SIGXFSZ)
#endif // SIGXFSZ
#ifdef SIGINFO
    | (1U << SIGINFO)
#endif // SIGINFO
#ifdef SIGUSR1
    | (1U << SIGUSR1)
#endif // SIGUSR1
#ifdef SIGUSR2
    | (1U << SIGUSR2)
#endif // SIGUSR2
#ifdef SIGSTKFLT
    | (1U << SIGSTKFLT)
#endif // SIGSTKFLT
#ifdef SIGPWR
    | (1U << SIGPWR)
#endif // SIGPWR
};


RECORDER_TWEAK_DEFINE(recorder_signals,
                      RECORDER_SIGNALS_MASK,
                      "Default mask for signals");


void recorder_dump_on_common_signals(unsigned add, unsigned remove)
// ----------------------------------------------------------------------------
//    Easy interface to dump on the most common signals
// ----------------------------------------------------------------------------
{
    // Normally, this is called after constructors have run, so this is
    // a good time to check environment settings
    recorder_trace_set(getenv("RECORDER_TRACES"));
    recorder_trace_set(getenv("RECORDER_TWEAKS"));

    const char *dump_pattern = getenv("RECORDER_DUMP");
    if (dump_pattern)
        recorder_background_dump(dump_pattern);

    unsigned sig;
    unsigned signals = (add | RECORDER_TWEAK(recorder_signals)) & ~remove;

    RECORD(signals, "Activating dump for signal mask 0x%X", signals);
    for (sig = 0; signals; sig++)
    {
        unsigned mask = 1U << sig;
        if (signals & mask)
            recorder_dump_on_signal(sig);
        signals &= ~mask;
    }
}



// ============================================================================
//
//    Support functions
//
// ============================================================================

#ifndef recorder_tick
uintptr_t recorder_tick()
// ----------------------------------------------------------------------------
//   Return the "ticks" as stored in the recorder
// ----------------------------------------------------------------------------
{
    static uintptr_t initialTick = 0;
    struct timeval t;
    gettimeofday(&t, NULL);
#if INTPTR_MAX < 0x8000000
    uintptr_t tick = t.tv_sec * 1000ULL + t.tv_usec / 1000;
#else
    uintptr_t tick = t.tv_sec * 1000000ULL + t.tv_usec;
#endif
    if (!initialTick)
        initialTick = tick;
    return tick - initialTick;
}
#endif // recorder_tick


void recorder_activate (recorder_info *recorder)
// ----------------------------------------------------------------------------
//   Activate the given recorder by putting it in linked list
// ----------------------------------------------------------------------------
{
    recorder_info  *head = recorders;
    do { recorder->next = head; }
    while (!ring_compare_exchange(recorders, head, recorder));
}


void recorder_tweak_activate (recorder_tweak *tweak)
// ----------------------------------------------------------------------------
//   Activate the given recorder by putting it in linked list
// ----------------------------------------------------------------------------
{
    recorder_tweak  *head = tweaks;
    do { tweak->next = head; }
    while (!ring_compare_exchange(tweaks, head, tweak));
}


RECORDER(recorder_trace_set, 64, "Setting recorder traces");

int recorder_trace_set(const char *param_spec)
// ----------------------------------------------------------------------------
//   Activate given traces
// ----------------------------------------------------------------------------
{
    const char     *next = param_spec;
    char            buffer[128];
    int             rc   = RECORDER_TRACE_OK;
    recorder_info  *rec;
    recorder_tweak *tweak;
    regex_t         re;
    static char     error[128];

    // Facilitate usage such as: recorder_trace_set(getenv("RECORDER_TRACES"))
    if (!param_spec)
        return 0;

    if (strcmp(param_spec, "help") == 0 || strcmp(param_spec, "list") == 0)
    {
        printf("List of available recorders:\n");
        for (rec = recorders; rec; rec = rec->next)
            printf("%20s : %s\n", rec->name, rec->description);

        printf("List of available tweaks:\n");
        for (tweak = tweaks; tweak; tweak = tweak->next)
            printf("%20s : %s = %ld (0x%lX) \n",
                   tweak->name, tweak->description,
                   (long) tweak->tweak, (long) tweak->tweak);

        return 0;
    }
    if (strcmp(param_spec, "all") == 0)
        next = param_spec = ".*";

    RECORD(recorder_trace_set, "Setting traces to %s", param_spec);

    // Loop splitting input at ':' and ' ' boundaries
    do
    {
        // Default value is 1 if not specified
        int value = 1;
        char *param = (char *) next;
        const char *original = param;
        const char *value_ptr = NULL;
        char *alloc = NULL;
        char *end = NULL;

        // Split foo:bar:baz so that we consider only foo in this loop
        next = strpbrk(param, ": ");
        if (next)
        {
            if (next - param < sizeof(buffer)-1)
            {
                memcpy(buffer, param, next - param);
                param = buffer;
            }
            else
            {
                alloc = malloc(next - param + 1);
                memcpy(alloc, param, next - param);
            }
            param[next - param] = 0;
            next++;
        }

        // Check if we have an explicit value (foo=1), otherwise use default
        value_ptr = strchr(param, '=');
        if (value_ptr)
        {
            if (param == buffer)
            {
                if (value_ptr - buffer < sizeof(buffer)-1)
                {
                    memcpy(buffer, param, value_ptr - param);
                    param = buffer;
                }
                else
                {
                    alloc = malloc(value_ptr - param + 1);
                    memcpy(alloc, param, value_ptr - param);
                }
            }
            param[value_ptr - param] = 0;
            value = strtol(value_ptr + 1, &end, 0);
            if (*end != 0)
            {
                rc = RECORDER_TRACE_INVALID_VALUE;
                RECORD(recorder_trace_set,
                       "Invalid numerical value %s (ends with %c)",
                       original + (value_ptr + 1 - param),
                       *end);
            }
        }

        int status = regcomp(&re, param, REG_EXTENDED|REG_NOSUB|REG_ICASE);
        if (status == 0)
        {
            for (rec = recorders; rec; rec = rec->next)
            {
                int re_result = regexec(&re, rec->name, 0, NULL, 0);
                if (re_result == 0)
                {
                    RECORD(recorder_trace_set, "Set %s from %ld to %ld",
                           rec->name, rec->trace, value);
                    rec->trace = value;
                }
            }
            for (tweak = tweaks; tweak; tweak = tweak->next)
            {
                int re_result = regexec(&re, tweak->name, 0, NULL, 0);
                if (re_result == 0)
                {
                    RECORD(recorder_trace_set, "Set tweak %s from %ld to %ld",
                           tweak->name, tweak->tweak, value);
                    tweak->tweak = value;
                }
            }
        }
        else
        {
            rc = RECORDER_TRACE_INVALID_NAME;
            regerror(status, &re, error, sizeof(error));
            RECORD(recorder_trace_set, "regcomp returned %d: %s",
                   status, error);
        }

        if (alloc)
            free(alloc);
    } while (next);

    return rc;
}
