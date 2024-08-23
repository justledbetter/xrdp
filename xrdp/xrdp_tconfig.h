/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) Koichiro Iwao
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

/**
 *
 * @file xrdp_tconfig.c
 * @brief TOML config loader and structures
 * @author Koichiro Iwao
 *
 */
#ifndef _XRDP_TCONFIG_H_
#define _XRDP_TCONFIG_H_

/* The number of connection types in MS-RDPBCGR 2.2.1.3.2 */
#define NUM_CONNECTION_TYPES 7
#define GFX_CONF XRDP_CFG_PATH "/gfx.toml"

/* nc stands for new config */
struct xrdp_tconfig_gfx_x264_param
{
    char preset[16];
    char tune[16];
    char profile[16];
    int vbv_max_bitrate;
    int vbv_buffer_size;
    int fps_num;
    int fps_den;
};

struct xrdp_tconfig_gfx_codec_order
{
    int h264_idx;
    int rfx_idx;
};

struct xrdp_tconfig_gfx
{
    struct xrdp_tconfig_gfx_codec_order codec;
    /* store x264 parameters for each connection type */
    struct xrdp_tconfig_gfx_x264_param x264_param[NUM_CONNECTION_TYPES];
};

static const char *const rdpbcgr_connection_type_names[] =
{
    "default", /* for xrdp internal use */
    "modem",
    "broadband_low",
    "satellite",
    "broadband_high",
    "wan",
    "lan",
    "autodetect",
    0
};

int tconfig_load_gfx(const char *filename, struct xrdp_tconfig_gfx *config);

#endif
