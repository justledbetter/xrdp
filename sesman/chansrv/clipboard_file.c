/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) Jay Sorg 2012
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* MS-RDPECLIP
 * http://msdn.microsoft.com/en-us/library/cc241066%28prot.20%29.aspx
 *
 * CLIPRDR_FILEDESCRIPTOR
 * http://msdn.microsoft.com/en-us/library/ff362447%28prot.20%29.aspx */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include <ctype.h>
#include <sys/time.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xfixes.h>
#include "arch.h"
#include "parse.h"
#include "os_calls.h"
#include "string_calls.h"
#include "list.h"
#include "chansrv.h"
#include "clipboard.h"
#include "clipboard_file.h"
#include "clipboard_common.h"
#include "xcommon.h"
#include "chansrv_fuse.h"
#include "ms-rdpeclip.h"

extern int g_cliprdr_chan_id; /* in chansrv.c */

extern struct clip_s2c g_clip_s2c; /* in clipboard.c */
extern struct clip_c2s g_clip_c2s; /* in clipboard.c */

extern char g_fuse_clipboard_path[];

struct cb_file_info
{
    char *pathname;
    char *filename;
    int flags;
    int size;
    tui64 time;
};

static struct list *g_files_list = 0;

/* used when server is asking for file info from the client */
static int g_file_request_sent_type = 0;

/* number of seconds from 1 Jan. 1601 00:00 to 1 Jan 1970 00:00 UTC */
#define CB_EPOCH_DIFF 11644473600LL

/*****************************************************************************/
#if 0
static tui64
timeval2wintime(struct timeval *tv)
{
    tui64 result;

    result = CB_EPOCH_DIFF;
    result += tv->tv_sec;
    result *= 10000000LL;
    result += tv->tv_usec * 10;
    return result;
}
#endif

/**
 * Gets a useable filename from a file specification passed to us
 *
 * The passed-in specification may contain instances of RFC3986 encoded
 * octets '%xx' where 'x' is a hex digit (e.g. %20 == ASCII SPACE). For
 * UTF-8, there may be many of these (e.g. %E6%97%A5 maps to the U+65E5
 * Unicode character)
 *
 * The result must be free'd by the caller.
 */
static char *
decode_rfc3986(const char *rfc3986, int len)
{
    char *result = (char *)malloc(len + 1);
    if (result != NULL)
    {
        int i = 0;
        int j = 0;
        /* Copy the passed-in filename so we can modify it */
        while (i < len)
        {
            /* Check for %xx for a character (e.g. %20 == ASCII 32 == SPACE) */
            if (rfc3986[i] == '%' && (len - i) > 2 &&
                    isxdigit(rfc3986[i + 1]) && isxdigit(rfc3986[i + 2]))
            {
                char jchr[] = { rfc3986[i + 1], rfc3986[i + 2], '\0' };
                result[j++] = g_htoi(jchr);
                i += 3;
            }
            else
            {
                result[j++] = rfc3986[i++];
            }
        }
        result[j] = '\0';
    }

    return result;
}

/**
 * Allocates a alloc_cb_file_info struct
 *
 * The memory for the struct is allocated in such a way that a single
 * free() call can be used to de-allocate it
 *
 * Filename elements are copied into the struct
 */
static struct cb_file_info *
alloc_cb_file_info(const char *full_name)
{
    struct cb_file_info *result = NULL;

    /* Find the last path separator in the string */
    const char *psep = strrchr(full_name, '/');

    /* Separate the name into a path and an unqualified name */
    const char *path_ptr = "/";
    unsigned int path_len = 1;
    const char *name_ptr;

    if (psep == NULL)
    {
        name_ptr = full_name;
    }
    else if (psep == full_name)
    {
        name_ptr = full_name + 1;
    }
    else
    {
        path_ptr = full_name;
        path_len = psep - full_name;
        name_ptr = psep + 1;
    }

    /* Allocate a block big enough for the struct, and
     * for both the strings */
    unsigned int name_len = strlen(name_ptr);
    unsigned int alloc_size = sizeof(struct cb_file_info) +
                              (path_len + 1) + (name_len + 1);

    result = (struct cb_file_info *)malloc(alloc_size);
    if (result != NULL)
    {
        /* Get a pointer to the first byte past the struct */
        result->pathname = (char *)(result + 1);
        result->filename = result->pathname + path_len + 1;
        memcpy(result->pathname, path_ptr, path_len);
        result->pathname[path_len] = '\0';
        memcpy(result->filename, name_ptr, name_len);
        result->filename[name_len] = '\0';
    }

    return result;
}

/***
 * See MS-RDPECLIP 3.1.5.4.7
 *
 * Sends a failure response to a CLIPRDR_FILECONTENTS_REQUEST
 * @param streamId Stream ID from CLIPRDR_FILECONTENTS_REQUEST
 * @return 0 for success
 */

static int
clipboard_send_filecontents_response_fail(int streamId)
{
    LOG_DEVEL(LOG_LEVEL_TRACE, "clipboardn_send_filecontents_response_fail:");

    struct stream *s;
    int size;
    int rv;

    make_stream(s);
    init_stream(s, 64);

    out_uint16_le(s, CB_FILECONTENTS_RESPONSE);
    out_uint16_le(s, CB_RESPONSE_FAIL);
    out_uint32_le(s, 4);
    out_uint32_le(s, streamId);
    s_mark_end(s);
    size = (int)(s->end - s->data);
    rv = send_channel_data(g_cliprdr_chan_id, s->data, size);
    free_stream(s);
    return rv;
}

/*****************************************************************************/
static int
clipboard_get_file(const char *file, int bytes)
{
    char *full_fn;
    struct cb_file_info *cfi;
    int result = 1;

    /* x-special/gnome-copied-files */
    if ((g_strncmp(file, "copy", 4) == 0) && (bytes == 4))
    {
        return 0;
    }
    if ((g_strncmp(file, "cut", 3) == 0) && (bytes == 3))
    {
        return 0;
    }

    /* text/uri-list */
    /* x-special/gnome-copied-files */
    if (bytes > 7 && g_strncmp(file, "file://", 7) == 0)
    {
        full_fn = decode_rfc3986(file + 7, bytes - 7);
    }
    else
    {
        full_fn = decode_rfc3986(file, bytes);
    }

    if (full_fn == NULL)
    {
        LOG(LOG_LEVEL_ERROR, "clipboard_get_file: Out of memory");
        return 1;
    }

    /*
     * Before we look at the file, see if it's in the FUSE filesystem. If it is,
     * we can't call normal file checking functions, as these will result in
     * a deadlock */
    if (xfuse_path_in_xfuse_fs(full_fn))
    {
        LOG(LOG_LEVEL_ERROR, "clipboard_get_file: Can't add client-side file "
            "%s to clipboard", full_fn);
    }
    else if (g_directory_exist(full_fn))
    {
        LOG(LOG_LEVEL_ERROR, "clipboard_get_file: file [%s] is a directory, "
            "not supported", full_fn);
    }
    else if (!g_file_exist(full_fn))
    {
        LOG(LOG_LEVEL_ERROR, "clipboard_get_file: file [%s] does not exist",
            full_fn);
    }
    else if ((cfi = alloc_cb_file_info(full_fn)) == NULL)
    {
        LOG(LOG_LEVEL_ERROR, "clipboard_get_file: Out of memory");
    }
    else
    {
        list_add_item(g_files_list, (tintptr)cfi);
        cfi->size = g_file_get_size(full_fn);
        cfi->flags = CB_FILE_ATTRIBUTE_ARCHIVE;
        cfi->time = (time(NULL) + CB_EPOCH_DIFF) * 10000000LL;
        LOG_DEVEL(LOG_LEVEL_DEBUG, "ok filename [%s] pathname [%s] size [%d]",
                  cfi->filename, cfi->pathname, cfi->size);
        result = 0;
    }

    free(full_fn);
    return result;
}

/*****************************************************************************/
/*
 * Calls clipboard_get_file() for each filename in a list.
 *
 * List items are separated by line terminators. Blank items are ignored */
static int
clipboard_get_files(const char *files, int bytes)
{
    const char *start = files;
    const char *end = files + bytes;
    const char *p;

    for (p = start ; p < end ; ++p)
    {
        if (*p == '\n' || *p == '\r')
        {
            /* Skip zero-length files (which might be caused by
             * multiple line terminators */
            if (p > start)
            {
                /* Get file. Errors are logged */
                (void)clipboard_get_file(start, p - start);
            }

            /* Move the start of filename pointer to either 'end', or
             * the next character which will either be a filename or
             * another terminator */
            start = p + 1;
        }
    }
    if (end > start)
    {
        (void)clipboard_get_file(start, end - start);
    }
    if (g_files_list->count < 1)
    {
        return 1;
    }
    return 0;
}

/*****************************************************************************/
/* server to client */
/* response to client asking for clipboard contents that is file list */
int
clipboard_send_data_response_for_file(const char *data, int data_size)
{
    struct stream *s;
    int size;
    int rv;
    int bytes_after_header;
    int cItems;
    int flags;
    int index;
    tui32 ui32;
    unsigned int utf8_count;
    unsigned int utf16_count;
    struct cb_file_info *cfi;

    LOG_DEVEL(LOG_LEVEL_DEBUG, "clipboard_send_data_response_for_file: data_size %d",
              data_size);
    LOG_DEVEL_HEXDUMP(LOG_LEVEL_TRACE, "", data, data_size);
    if (g_files_list == 0)
    {
        g_files_list = list_create();
        g_files_list->auto_free = 1;
    }
    list_clear(g_files_list);
    clipboard_get_files(data, data_size);
    cItems = g_files_list->count;
    bytes_after_header = cItems * 592 + 4;
    make_stream(s);
    init_stream(s, 64 + bytes_after_header);
    out_uint16_le(s, CB_FORMAT_DATA_RESPONSE); /* 5 CLIPRDR_DATA_RESPONSE */
    out_uint16_le(s, CB_RESPONSE_OK); /* 1 status */
    out_uint32_le(s, bytes_after_header);
    out_uint32_le(s, cItems);
    for (index = 0; index < cItems; index++)
    {
        cfi = (struct cb_file_info *)list_get_item(g_files_list, index);
        flags = CB_FD_ATTRIBUTES | CB_FD_FILESIZE | CB_FD_WRITESTIME | CB_FD_PROGRESSUI;
        out_uint32_le(s, flags);
        out_uint8s(s, 32); /* reserved1 */
        flags = cfi->flags;
        out_uint32_le(s, flags);
        out_uint8s(s, 16); /* reserved2 */
        /* file time */
        /* 100-nanoseconds intervals since 1 January 1601 */
        //out_uint32_le(s, 0x2c305d08); /* 25 October 2009, 21:17 */
        //out_uint32_le(s, 0x01ca55f3);
        ui32 = cfi->time & 0xffffffff;
        out_uint32_le(s, ui32);
        ui32 = cfi->time >> 32;
        out_uint32_le(s, ui32);
        /* file size */
        out_uint32_le(s, 0);
        out_uint32_le(s, cfi->size);
        /* Name is fixed-size 260 UTF-16 words */
        utf8_count = strlen(cfi->filename) + 1;  // Include terminator
        utf16_count = utf8_as_utf16_word_count(cfi->filename, utf8_count);
        if (utf16_count > 260)
        {
            LOG(LOG_LEVEL_ERROR,
                "clipboard_send_data_response_for_file:"
                " filename overflow (%u words)", utf16_count);
            utf8_count = 0;
            utf16_count = 0;
        }
        out_utf8_as_utf16_le(s, cfi->filename, utf8_count);
        out_uint8s(s, (260 - utf16_count) * 2);
    }
    out_uint32_le(s, 0);
    s_mark_end(s);
    size = (int)(s->end - s->data);
    rv = send_channel_data(g_cliprdr_chan_id, s->data, size);
    free_stream(s);
    return rv;
}

/*****************************************************************************/
/* send the file size from server to the client */
static int
clipboard_send_file_size(int streamId, int lindex)
{
    struct stream *s;
    int size;
    int rv;
    int file_size;
    struct cb_file_info *cfi;

    if (g_files_list == 0)
    {
        LOG_DEVEL(LOG_LEVEL_ERROR, "clipboard_send_file_size: error g_files_list is nil");
        clipboard_send_filecontents_response_fail(streamId);
        return 1;
    }
    cfi = (struct cb_file_info *)list_get_item(g_files_list, lindex);
    if (cfi == 0)
    {
        LOG_DEVEL(LOG_LEVEL_ERROR, "clipboard_send_file_size: error cfi is nil");
        clipboard_send_filecontents_response_fail(streamId);
        return 1;
    }
    if (cfi->size < 0)
    {
        LOG(LOG_LEVEL_ERROR, "clipboard_send_file_size: error cfi->size is negative"
            "value [%d]", cfi->size);
        clipboard_send_filecontents_response_fail(streamId);
        return 1;
    }
    file_size = cfi->size;
    LOG_DEVEL(LOG_LEVEL_DEBUG, "clipboard_send_file_size: streamId %d file_size %d",
              streamId, file_size);
    make_stream(s);
    init_stream(s, 8192);
    out_uint16_le(s, CB_FILECONTENTS_RESPONSE); /* 9 */
    out_uint16_le(s, CB_RESPONSE_OK); /* 1 status */
    out_uint32_le(s, 12);
    out_uint32_le(s, streamId);
    out_uint32_le(s, file_size);
    out_uint32_le(s, 0);
    out_uint32_le(s, 0);
    s_mark_end(s);
    size = (int)(s->end - s->data);
    rv = send_channel_data(g_cliprdr_chan_id, s->data, size);
    free_stream(s);
    return rv;
}

/*****************************************************************************/
/* ask the client to send the file size */
int
clipboard_request_file_size(int stream_id, int lindex)
{
    struct stream *s;
    int size;
    int rv;

    LOG_DEVEL(LOG_LEVEL_DEBUG, "clipboard_request_file_size:");
    if (g_file_request_sent_type != 0)
    {
        LOG_DEVEL(LOG_LEVEL_ERROR, "clipboard_request_file_size: warning, still waiting "
                  "for CB_FILECONTENTS_RESPONSE");
    }
    make_stream(s);
    init_stream(s, 8192);
    out_uint16_le(s, CB_FILECONTENTS_REQUEST); /* 8 */
    out_uint16_le(s, 0);
    out_uint32_le(s, 28);
    out_uint32_le(s, stream_id);
    out_uint32_le(s, lindex);
    out_uint32_le(s, CB_FILECONTENTS_SIZE);
    out_uint32_le(s, 0); /* nPositionLow */
    out_uint32_le(s, 0); /* nPositionHigh */
    out_uint32_le(s, 0); /* cbRequested */
    out_uint32_le(s, 0); /* clipDataId */
    out_uint32_le(s, 0);
    s_mark_end(s);
    size = (int)(s->end - s->data);
    rv = send_channel_data(g_cliprdr_chan_id, s->data, size);
    free_stream(s);
    g_file_request_sent_type = CB_FILECONTENTS_SIZE;
    return rv;
}

/*****************************************************************************/
/* send a chunk of the file from server to client */
static int
clipboard_send_file_data(int streamId, int lindex,
                         int nPositionLow, int cbRequested)
{
    struct stream *s;
    int size;
    int rv;
    int fd;
    char full_fn[256];
    struct cb_file_info *cfi;

    if (g_files_list == 0)
    {
        LOG_DEVEL(LOG_LEVEL_ERROR, "clipboard_send_file_data: error g_files_list is nil");
        clipboard_send_filecontents_response_fail(streamId);
        return 1;
    }
    cfi = (struct cb_file_info *)list_get_item(g_files_list, lindex);
    if (cfi == 0)
    {
        LOG_DEVEL(LOG_LEVEL_ERROR, "clipboard_send_file_data: error cfi is nil");
        clipboard_send_filecontents_response_fail(streamId);
        return 1;
    }
    LOG_DEVEL(LOG_LEVEL_DEBUG, "clipboard_send_file_data: streamId %d lindex %d "
              "nPositionLow %d cbRequested %d", streamId, lindex,
              nPositionLow, cbRequested);
    g_snprintf(full_fn, 255, "%s/%s", cfi->pathname, cfi->filename);
    fd = g_file_open_ro(full_fn);
    if (fd == -1)
    {
        LOG(LOG_LEVEL_ERROR, "clipboard_send_file_data: file open [%s] failed: %s",
            full_fn, g_get_strerror());
        clipboard_send_filecontents_response_fail(streamId);
        return 1;
    }
    if (g_file_seek(fd, nPositionLow) < 0)
    {
        LOG(LOG_LEVEL_ERROR, "clipboard_send_file_data: seek error in file [%s]: %s",
            full_fn, g_get_strerror());
        g_file_close(fd);
        clipboard_send_filecontents_response_fail(streamId);
        return 1;
    }
    make_stream(s);
    init_stream(s, cbRequested + 64);
    size = g_file_read(fd, s->data + 12, cbRequested);
    // If we're at end-of-file, 0 is a valid response
    if (size < 0)
    {
        LOG_DEVEL(LOG_LEVEL_ERROR,
                  "clipboard_send_file_data: read error, want %d got [%s]",
                  cbRequested, g_get_strerror());
        free_stream(s);
        g_file_close(fd);
        clipboard_send_filecontents_response_fail(streamId);
        return 1;
    }
    out_uint16_le(s, CB_FILECONTENTS_RESPONSE); /* 9 */
    out_uint16_le(s, CB_RESPONSE_OK); /* 1 status */
    out_uint32_le(s, size + 4);
    out_uint32_le(s, streamId);
    s->p += size;
    out_uint32_le(s, 0);
    s_mark_end(s);
    size = (int)(s->end - s->data);
    rv = send_channel_data(g_cliprdr_chan_id, s->data, size);
    free_stream(s);
    g_file_close(fd);

    /* Log who transferred which file via clipboard for the purpose of audit */
    LOG(LOG_LEVEL_INFO, "S2C: Transferred a file: filename=%s, uid=%d", full_fn, g_getuid());

    return rv;
}

/*****************************************************************************/
/* ask the client to send the file size */
int
clipboard_request_file_data(int stream_id, int lindex, int offset,
                            int request_bytes)
{
    struct stream *s;
    int size;
    int rv;

    LOG_DEVEL(LOG_LEVEL_DEBUG, "clipboard_request_file_data: stream_id=%d lindex=%d off=%d request_bytes=%d",
              stream_id, lindex, offset, request_bytes);

    if (g_file_request_sent_type != 0)
    {
        LOG_DEVEL(LOG_LEVEL_ERROR, "clipboard_request_file_data: warning, still waiting "
                  "for CB_FILECONTENTS_RESPONSE");
    }
    make_stream(s);
    init_stream(s, 8192);
    out_uint16_le(s, CB_FILECONTENTS_REQUEST); /* 8 */
    out_uint16_le(s, 0);
    out_uint32_le(s, 28);
    out_uint32_le(s, stream_id);
    out_uint32_le(s, lindex);
    out_uint32_le(s, CB_FILECONTENTS_RANGE);
    out_uint32_le(s, offset); /* nPositionLow */
    out_uint32_le(s, 0); /* nPositionHigh */
    out_uint32_le(s, request_bytes); /* cbRequested */
    out_uint32_le(s, 0); /* clipDataId */
    out_uint32_le(s, 0);
    s_mark_end(s);
    size = (int)(s->end - s->data);
    rv = send_channel_data(g_cliprdr_chan_id, s->data, size);
    free_stream(s);
    g_file_request_sent_type = CB_FILECONTENTS_RANGE;
    return rv;
}


/*****************************************************************************/
/* client is asking from info about a file */
int
clipboard_process_file_request(struct stream *s, int clip_msg_status,
                               int clip_msg_len)
{
    int streamId;
    int lindex;
    int dwFlags;
    int nPositionLow;
    int cbRequested;
    //int clipDataId;

    LOG_DEVEL(LOG_LEVEL_DEBUG, "clipboard_process_file_request:");
    LOG_DEVEL_HEXDUMP(LOG_LEVEL_TRACE, "", s->p, clip_msg_len);
    in_uint32_le(s, streamId);
    in_uint32_le(s, lindex);
    in_uint32_le(s, dwFlags);
    in_uint32_le(s, nPositionLow);
    in_uint8s(s, 4); /* nPositionHigh */
    in_uint32_le(s, cbRequested);
    //in_uint32_le(s, clipDataId); /* options, used when locking */
    if (dwFlags & CB_FILECONTENTS_SIZE)
    {
        clipboard_send_file_size(streamId, lindex);
    }
    if (dwFlags & CB_FILECONTENTS_RANGE)
    {
        clipboard_send_file_data(streamId, lindex, nPositionLow, cbRequested);
    }
    return 0;
}

/*****************************************************************************/
/* server requested info about the file and this is the response
   it's either the file size or file data */
int
clipboard_process_file_response(struct stream *s, int clip_msg_status,
                                int clip_msg_len)
{
    int streamId;
    int file_size;

    LOG_DEVEL(LOG_LEVEL_DEBUG, "clipboard_process_file_response:");
    if (g_file_request_sent_type == CB_FILECONTENTS_SIZE)
    {
        g_file_request_sent_type = 0;
        in_uint32_le(s, streamId);
        in_uint32_le(s, file_size);
        LOG_DEVEL(LOG_LEVEL_DEBUG, "clipboard_process_file_response: streamId %d "
                  "file_size %d", streamId, file_size);
        xfuse_file_contents_size(streamId, file_size);
    }
    else if (g_file_request_sent_type == CB_FILECONTENTS_RANGE)
    {
        g_file_request_sent_type = 0;
        in_uint32_le(s, streamId);
        xfuse_file_contents_range(streamId, s->p, clip_msg_len - 4);
    }
    else
    {
        LOG_DEVEL(LOG_LEVEL_ERROR, "clipboard_process_file_response: error");
        g_file_request_sent_type = 0;
    }
    return 0;
}

/*****************************************************************************/
/* read in CLIPRDR_FILEDESCRIPTOR [MS-RDPECLIP] 2.2.5.2.3.1 */
static int
clipboard_c2s_in_file_info(struct stream *s, struct clip_file_desc *cfd)
{
    int filename_bytes;
    int ex_bytes;

    if (!s_check_rem_and_log(s, 4 + 32 + 4 + 16 + 8 + 8 + 520,
                             "Parsing [MS-RDPECLIP] CLIPRDR_FILEDESCRIPTOR"))
    {
        return 1;
    }
    in_uint32_le(s, cfd->flags);
    in_uint8s(s, 32); /* reserved1 */
    in_uint32_le(s, cfd->fileAttributes);
    in_uint8s(s, 16); /* reserved2 */
    in_uint32_le(s, cfd->lastWriteTimeLow);
    in_uint32_le(s, cfd->lastWriteTimeHigh);
    in_uint32_le(s, cfd->fileSizeHigh);
    in_uint32_le(s, cfd->fileSizeLow);
    filename_bytes =
        clipboard_in_utf16_le_as_utf8(s, cfd->cFileName,
                                      sizeof(cfd->cFileName));
    if (filename_bytes > 520)
    {
        LOG(LOG_LEVEL_ERROR,
            "Filename in CLIPRDR_FILEDESCRIPTOR is too long (%d bytes)",
            filename_bytes);
        return 1;
    }
    ex_bytes = 520 - filename_bytes;
    in_uint8s(s, ex_bytes);
    LOG_DEVEL(LOG_LEVEL_DEBUG, "clipboard_c2s_in_file_info:");
    LOG_DEVEL(LOG_LEVEL_DEBUG, "  flags 0x%8.8x", cfd->flags);
    LOG_DEVEL(LOG_LEVEL_DEBUG, "  fileAttributes 0x%8.8x", cfd->fileAttributes);
    LOG_DEVEL(LOG_LEVEL_DEBUG, "  lastWriteTime 0x%8.8x%8.8x", cfd->lastWriteTimeHigh,
              cfd->lastWriteTimeLow);
    LOG_DEVEL(LOG_LEVEL_DEBUG, "  fileSize 0x%8.8x%8.8x", cfd->fileSizeHigh,
              cfd->fileSizeLow);
    LOG_DEVEL(LOG_LEVEL_DEBUG, "  cFileName [%s]", cfd->cFileName);
    return 0;
}

/*****************************************************************************/
int
clipboard_c2s_in_files(struct stream *s, char *file_list, int file_list_size,
                       const char *fprefix)
{
    int citems;
    int lindex;
    int str_len;
    struct clip_file_desc cfd;
    char *ptr;
    char *last; /* Last writeable char in buffer */
    int dropped_files = 0; /* # files we can't add to buffer */

    if (file_list_size < 1)
    {
        LOG(LOG_LEVEL_ERROR, "clipboard_c2s_in_files: No space in string");
        return 1;
    }
    if (!s_check_rem(s, 4))
    {
        LOG(LOG_LEVEL_ERROR, "clipboard_c2s_in_files: parse error");
        return 1;
    }
    in_uint32_le(s, citems);
    if (citems < 0 || citems > 64 * 1024) /* sanity check */
    {
        LOG(LOG_LEVEL_ERROR, "clipboard_c2s_in_files: "
            "Bad number of files in list (%d)", citems);
        return 1;
    }
    xfuse_clear_clip_dir();
    LOG_DEVEL(LOG_LEVEL_DEBUG, "clipboard_c2s_in_files: cItems %d", citems);
    ptr = file_list;
    last = file_list + file_list_size - 1;

    for (lindex = 0; lindex < citems; lindex++)
    {
        g_memset(&cfd, 0, sizeof(struct clip_file_desc));
        if (clipboard_c2s_in_file_info(s, &cfd) != 0)
        {
            return 1;
        }
        if ((g_pos(cfd.cFileName, "\\") >= 0) ||
                (cfd.fileAttributes & CB_FILE_ATTRIBUTE_DIRECTORY))
        {
            LOG(LOG_LEVEL_WARNING, "clipboard_c2s_in_files: skipping "
                "directory not supported [%s]", cfd.cFileName);
            continue;
        }

        /* Have we already run out of room in the list? */
        if (dropped_files > 0)
        {
            dropped_files += 1;
            continue;
        }

        /* Room for this file? */
        str_len = (ptr == file_list) ? 0 : 1; /* Delimiter */
        str_len += g_strlen(fprefix); /* e.g. "file://" */
        str_len += g_strlen(g_fuse_clipboard_path);
        str_len += 1; /* '/' */
        str_len += g_strlen(cfd.cFileName);
        if (str_len > (last - ptr))
        {
            dropped_files += 1;
            continue;
        }

        if (xfuse_add_clip_dir_item(cfd.cFileName, 0, cfd.fileSizeLow, lindex) == -1)
        {
            LOG(LOG_LEVEL_WARNING, "clipboard_c2s_in_files: "
                "failed to add clip dir item %s", cfd.cFileName);
            continue;
        }

        if (ptr > file_list)
        {
            *ptr++ = '\n';
        }

        str_len = g_strlen(fprefix);
        g_strcpy(ptr, fprefix);
        ptr += str_len;

        str_len = g_strlen(g_fuse_clipboard_path);
        g_strcpy(ptr, g_fuse_clipboard_path);
        ptr += str_len;
        *ptr++ = '/';

        str_len = g_strlen(cfd.cFileName);
        g_strcpy(ptr, cfd.cFileName);
        ptr += str_len;
    }
    *ptr = '\0';

    if (dropped_files > 0)
    {
        LOG(LOG_LEVEL_WARNING, "clipboard_c2s_in_files: "
            "Dropped %d files from the clip buffer due to insufficient space",
            dropped_files);
    }
    return 0;
}
